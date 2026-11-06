// Orion backend — WorldGeometry 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// === 核心逻辑：批量更新 Bindless 纹理描述符集 ===
// === 新增：全局几何体动态重建核心 ===
bool rebuildWorldGeometry(
    size_t maxSectionUpdates
)
{
    if(
        g_device == VK_NULL_HANDLE ||
        g_rtCommandPool == VK_NULL_HANDLE ||
        g_queue == VK_NULL_HANDLE
    )
    {
        return true;
    }

    ensureGeometryCaptureWorker();

    g_geometryRebuildCallTotal += 1u;

    RTXMarkPhase("geometry:entry");

    {
        const bool previousBuildPending =
            sectionRebuildFenceGateEnabled() &&
            g_sectionRetirementFencesReady &&
            g_sectionRetirementRing[g_currentRetirementSlot].fencePending;

        if(previousBuildPending)
        {
            const VkFence previousBuildFence =
                g_sectionRetirementFences[g_currentRetirementSlot];

            VkResult fenceStatus =
                vkGetFenceStatus(g_device, previousBuildFence);

            if(
                fenceStatus == VK_NOT_READY &&
                SECTION_REBUILD_FENCE_WAIT_MS > 0u
            )
            {
                fenceStatus = vkWaitForFences(
                    g_device,
                    1,
                    &previousBuildFence,
                    VK_TRUE,
                    static_cast<uint64_t>(SECTION_REBUILD_FENCE_WAIT_MS) * 1000000ull
                );
            }

            if(
                fenceStatus == VK_NOT_READY ||
                fenceStatus == VK_TIMEOUT
            )
            {
                g_geometryFenceDeferredCount += 1u;
                return false;
            }

            check(
                fenceStatus,
                "poll previous Section build fence failed"
            );

            reapSignaledRetirements();
        }
    }

    const auto perfL10RebuildStart =
        std::chrono::steady_clock::now();

    double perfL10QueueIdleMilliseconds =
        0.0;

    bool forceReset = false;

    std::vector<
        std::pair<
            uint64_t,
            SectionGeometrySplit
        >
    > sectionUpdates;

    // 2026-11-06r：函数返回时把 sectionUpdates 里所有 split 连同容量还给池。
    // RAII 形式，覆盖下面所有 early return；split 在 build 循环里只被读，
    // 这里归还时它已经用完。
    struct SectionSplitPoolReturn
    {
        std::vector<std::pair<uint64_t, SectionGeometrySplit>>* updates = nullptr;

        ~SectionSplitPoolReturn()
        {
            if(updates == nullptr)
            {
                return;
            }

            for(auto& entry : *updates)
            {
                returnSectionSplitToPool(std::move(entry.second));
            }
        }
    };
    SectionSplitPoolReturn sectionSplitPoolReturn{&sectionUpdates};



    const auto preLoopLockWaitStart = std::chrono::steady_clock::now();

    const auto geometryLockWaitBudget =
        std::chrono::milliseconds(geometryPreLoopLockWaitMs());
    bool preLoopLockAcquired = geometryLockWaitBudget.count() <= 0;

    if(!preLoopLockAcquired)
    {
        if(g_chunkMutex.try_lock_for(geometryLockWaitBudget))
        {
            preLoopLockAcquired = true;
            g_geometryPreLoopLockSkips = 0u;
        }
    }

    if(!preLoopLockAcquired)
    {
        ++g_geometryPreLoopLockSkips;

        if(g_geometryPreLoopLockSkips >= 5u)
        {
            preLoopLockAcquired = g_chunkMutex.try_lock_for(
                std::chrono::milliseconds(SECTION_PRELOOP_LOCK_FALLBACK_MS));

            if(preLoopLockAcquired)
            {
                g_geometryPreLoopLockSkips = 0u;
            }
            else
            {
                static bool s_lockFallbackDeferLogged = false;

                if(!s_lockFallbackDeferLogged)
                {
                    s_lockFallbackDeferLogged = true;
                    RTLog(
                        "[GEOMETRY LOCK] deferred after the bounded fallback "
                        "wait (geometry will catch up on a later frame)"
                    );
                }
            }
        }
    }

    if(!preLoopLockAcquired)
    {
        return false;
    }

    {
        const double lockWaitMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - preLoopLockWaitStart).count();

        if(lockWaitMs > g_geometryMaxPreLoopLockWaitMs)
        {
            g_geometryMaxPreLoopLockWaitMs = lockWaitMs;
        }
    }

    {
        std::lock_guard<std::timed_mutex>
            lock(
                g_chunkMutex,
                std::adopt_lock
            );


        forceReset =
            g_forceSectionASReset;


        if(vramGovernorMode() >= 2 && !g_nonResidentSections.empty())
        {
            const CameraUBO* rehydrateUbo =
                g_cameraUboMapped != nullptr
                    ? static_cast<const CameraUBO*>(g_cameraUboMapped)
                    : nullptr;

            // 2026-11-03：补回半径必须**严格小于**驱逐半径（Hysteresis 的本意）。
            // 超预算时驱逐用的是 keepRadius = 96×(1-0.25) = 72，驱逐半径
            // = 72×1.25 = 90（见下面 VRAM_EVICT_SHRINK_RADIUS）；而这里原来用
            // 未 shrink 的 96 ⇒ 补回 96 > 驱逐 90，落在 90~96 格之间的 Section
            // 每一帧"补回 → 下一帧又被驱逐"，永久乒乓：那圈几何反复进出 TLAS，
            // 表现为太阳阴影/远景色块逐帧抖。这里按同一 shrink 计算，
            // 补回 72 < 驱逐 90，留 18 格带宽（> 一个 Section 的 16 格）。
            const float rehydrateRadius = vramEvictKeepRadius() *
                (g_vramGovernor.over ? (1.0f - 0.25f) : 1.0f);
            const float rehydrateRadiusSq = rehydrateRadius * rehydrateRadius;

            const size_t rehydrateQuota = vramRehydratePerPass();
            size_t rehydrated = 0u;

            for(auto it = g_nonResidentSections.begin();
                it != g_nonResidentSections.end() &&
                rehydrated < rehydrateQuota;)
            {
                const uint64_t spatialKey =
                    *it & SPATIAL_SECTION_KEY_MASK;

                float centerX = 0.0f;
                float centerY = 0.0f;
                float centerZ = 0.0f;
                vramSectionCenterFromSpatialKey(
                    spatialKey,
                    centerX,
                    centerY,
                    centerZ
                );

                const float dx = centerX - (rehydrateUbo ? rehydrateUbo->playerX : 0.0f);
                const float dy = centerY - (rehydrateUbo ? rehydrateUbo->playerY : 0.0f);
                const float dz = centerZ - (rehydrateUbo ? rehydrateUbo->playerZ : 0.0f);

                if(dx * dx + dy * dy + dz * dz <= rehydrateRadiusSq)
                {
                    g_dirtySectionKeys.insert(spatialKey);
                    it = g_nonResidentSections.erase(it);
                    ++rehydrated;
                    ++g_vramRehydratedSections;
                }
                else
                {
                    ++it;
                }
            }
        }


        if(forceReset)
        {
            {
                std::lock_guard<std::mutex> capturedLock(g_capturedSectionsMutex);
                resetCapturedSectionsLocked();
            }

            std::unordered_map<
                uint64_t,
                SectionGeometrySplit
            > mergedSections;


            for(const auto& layerEntry : g_chunkMeshes)
            {
                const uint64_t spatialSectionKey =
                    layerEntry.first &
                    SPATIAL_SECTION_KEY_MASK;

                if(mergedSections.find(spatialSectionKey) != mergedSections.end())
                {
                    continue;
                }

                geometryCaptureSectionLocked(
                    spatialSectionKey,
                    mergedSections[spatialSectionKey]
                );
            }


            g_geometryForceResetCount.fetch_add(1u, std::memory_order_relaxed);
            RTLog(
                "[RT GEOMETRY] force reset: synchronous full rebuild of " +
                std::to_string(mergedSections.size()) +
                " section(s), frame=" + std::to_string(g_perfL10FrameSerial)
            );

            sectionUpdates.reserve(
                mergedSections.size()
            );


            for(auto& sectionEntry : mergedSections)
            {
                sectionUpdates.emplace_back(
                    sectionEntry.first,
                    std::move(
                        sectionEntry.second
                    )
                );
            }
        }
        else
        {
            const size_t batchCap =
                std::min(maxSectionUpdates, effectiveSectionRebuildBatchCap());

            const bool cursorMode = captureQueueCursorEnabled();

            std::vector<CapturedSectionUpdate> capturedBatch;
            {
                std::lock_guard<std::mutex> capturedLock(g_capturedSectionsMutex);

                if(cursorMode)
                {
                    const size_t available =
                        g_capturedSections.size() - g_capturedSectionsHead;

                    const size_t take =
                        std::min(available, batchCap);

                    capturedBatch.assign(
                        std::make_move_iterator(
                            g_capturedSections.begin() + g_capturedSectionsHead),
                        std::make_move_iterator(
                            g_capturedSections.begin() + g_capturedSectionsHead + take));

                    g_capturedSectionsHead += take;

                    if(g_capturedSectionsHead >= 256u &&
                       g_capturedSectionsHead * 2u >= g_capturedSections.size())
                    {
                        g_capturedSections.erase(
                            g_capturedSections.begin(),
                            g_capturedSections.begin() + g_capturedSectionsHead);
                        g_capturedSectionsHead = 0u;
                    }
                }
                else
                {
                    capturedBatch.swap(g_capturedSections);
                }
            }

            const bool entityDirty =
                g_dirtySectionKeys.find(ENTITY_RT_SPATIAL_KEY) !=
                g_dirtySectionKeys.end();

            const size_t adoptedCount =
                std::min(capturedBatch.size(), batchCap);
            if(!cursorMode && adoptedCount < capturedBatch.size())
            {
                std::lock_guard<std::mutex> capturedLock(g_capturedSectionsMutex);
                g_capturedSections.insert(
                    g_capturedSections.end(),
                    std::make_move_iterator(capturedBatch.begin() + adoptedCount),
                    std::make_move_iterator(capturedBatch.end())
                );
            }

            const size_t selectedCount = std::min(
                g_dirtySectionKeys.size(),
                batchCap - adoptedCount
            );
            sectionUpdates.reserve(adoptedCount + selectedCount);

            std::unordered_map<uint64_t, size_t> updateIndices;
            updateIndices.reserve(selectedCount + 1u);

            if(entityDirty && (adoptedCount + selectedCount) > 0u)
            {
                sectionUpdates.emplace_back(
                    ENTITY_RT_SPATIAL_KEY,
                    takeSectionSplitFromPool()
                );
                updateIndices.emplace(
                    ENTITY_RT_SPATIAL_KEY,
                    0u
                );
            }

            std::unordered_set<uint64_t> adoptedKeys;
            adoptedKeys.reserve(adoptedCount);
            for(size_t adopted = adoptedCount; adopted-- > 0u;)
            {
                const uint64_t adoptedKey = capturedBatch[adopted].spatialKey;
                if(g_dirtySectionKeys.find(adoptedKey) !=
                   g_dirtySectionKeys.end())
                {
                    continue;
                }
                if(!adoptedKeys.insert(adoptedKey).second)
                {
                    continue;
                }
                sectionUpdates.emplace_back(
                    adoptedKey,
                    std::move(capturedBatch[adopted].split)
                );
                g_geometryAdoptedSectionTotal += 1u;
            }

            if(!sectionUpdates.empty() && adoptedCount > 0u)
            {
                static bool loggedFirstAdoption = false;
                if(!loggedFirstAdoption)
                {
                    loggedFirstAdoption = true;
                    RTLog(
                        "[RT GEOMETRY] async capture active: adopted " +
                        std::to_string(adoptedCount) +
                        " pre-split section(s) from the capture worker"
                    );
                }
            }

            if(selectedCount > 0u)
            {
                constexpr size_t DIRTY_CANDIDATE_LIMIT = 512u;

                std::vector<std::pair<float, uint64_t>> nearest;
                nearest.reserve(
                    std::min(selectedCount * 4u + 16u, DIRTY_CANDIDATE_LIMIT));

                for(uint64_t sectionKey : g_dirtySectionKeys)
                {
                    if(sectionKey == ENTITY_RT_SPATIAL_KEY)
                    {
                        continue;
                    }

                    float centerX = 0.0f;
                    float centerY = 0.0f;
                    float centerZ = 0.0f;
                    vramSectionCenterFromSpatialKey(
                        sectionKey,
                        centerX,
                        centerY,
                        centerZ);

                    const float dx = centerX - g_dynamicCaptureCameraX;
                    const float dy = centerY - g_dynamicCaptureCameraY;
                    const float dz = centerZ - g_dynamicCaptureCameraZ;

                    nearest.emplace_back(
                        dx * dx + dy * dy + dz * dz,
                        sectionKey);

                    if(nearest.size() >= DIRTY_CANDIDATE_LIMIT)
                    {
                        break;
                    }
                }

                if(nearest.size() > selectedCount)
                {
                    std::nth_element(
                        nearest.begin(),
                        nearest.begin() + static_cast<std::ptrdiff_t>(selectedCount),
                        nearest.end());
                    nearest.resize(selectedCount);
                }

                std::sort(
                    nearest.begin(),
                    nearest.end(),
                    [](const std::pair<float, uint64_t>& a,
                       const std::pair<float, uint64_t>& b)
                    {
                        return a.first < b.first;
                    });

                for(const auto& nearestEntry : nearest)
                {
                    const size_t updateIndex = sectionUpdates.size();
                    sectionUpdates.emplace_back(
                        nearestEntry.second,
                        takeSectionSplitFromPool());
                    updateIndices.emplace(nearestEntry.second, updateIndex);
                }
            }


            const bool onlyDynamicSection =
                sectionUpdates.size() == 1u &&
                sectionUpdates.front().first == ENTITY_RT_SPATIAL_KEY;
            if(onlyDynamicSection)
            {
                geometryCaptureSectionLocked(
                    ENTITY_RT_SPATIAL_KEY,
                    sectionUpdates.front().second
                );
            }
            else
            {
                std::vector<size_t> captureOrder;
                captureOrder.reserve(updateIndices.size());
                for(const auto& updateEntry : updateIndices)
                {
                    captureOrder.push_back(updateEntry.second);
                }
                std::sort(captureOrder.begin(), captureOrder.end());

                size_t capturedCount = 0u;
                for(size_t orderIndex = 0u; orderIndex < captureOrder.size(); ++orderIndex)
                {
                    if(orderIndex != 0u)
                    {
                        const double captureElapsedMs =
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - perfL10RebuildStart
                            ).count();
                        if(captureElapsedMs >= sectionRebuildTimeBudgetMs())
                        {
                            break;
                        }
                    }

                    const size_t captureIndex = captureOrder[orderIndex];
                    geometryCaptureSectionLocked(
                        sectionUpdates[captureIndex].first,
                        sectionUpdates[captureIndex].second
                    );
                    ++capturedCount;
                }

                if(capturedCount < captureOrder.size())
                {
                    sectionUpdates.resize(captureOrder[capturedCount]);

                    g_sectionRebuildInterrupted = true;
                    g_sectionRebuildContinueFrame =
                        g_perfL10FrameSerial + SECTION_REBUILD_CONTINUE_FRAMES;

                    g_worldGeometryDirty.store(
                        !g_dirtySectionKeys.empty(),
                        std::memory_order_release
                    );
                }
            }
            }


        if(forceReset)
        {
            g_dirtySectionKeys.clear();
        }
        else
        {
            for(const auto& update : sectionUpdates)
            {
                g_dirtySectionKeys.erase(update.first);
            }
        }

        refreshDhLodSpatialSnapshotLocked();

        g_forceSectionASReset = false;
        bool capturedPending = false;
        {
            std::lock_guard<std::mutex> capturedLock(g_capturedSectionsMutex);
            capturedPending =
                g_capturedSections.size() > g_capturedSectionsHead;
        }
        g_worldGeometryDirty.store(
            !g_dirtySectionKeys.empty() || capturedPending,
            std::memory_order_release
        );
    }

    updateVramGovernor();

    const std::chrono::steady_clock::time_point rtxPreLoopMarkLog =
        std::chrono::steady_clock::now();

    if(g_perfL10FrameSerial % 600u == 0u)
    {
        const uint64_t capturedNow = g_geometryCapturedSectionTotal.load();
        const uint64_t callsNow = g_geometryRebuildCallTotal;
        const uint64_t capturedDelta = capturedNow - g_geometryCountersLastCaptured;
        const uint64_t callsDelta = callsNow - g_geometryCountersLastCalls;
        g_geometryCountersLastCaptured = capturedNow;
        g_geometryCountersLastCalls = callsNow;

        std::ostringstream countersLog;
        countersLog
            << std::fixed << std::setprecision(1)
            << "[RT GEOMETRY] counters: captured=" << capturedNow
            << " adopted=" << g_geometryAdoptedSectionTotal
            << " forceResets=" << g_geometryForceResetCount.load()
            << " rebuildCalls=" << callsNow
            << " fenceDeferred=" << g_geometryFenceDeferredCount
            << " perCall="
            << (callsDelta > 0u
                    ? static_cast<double>(capturedDelta) /
                          static_cast<double>(callsDelta)
                    : 0.0)
            << " cap=" << effectiveSectionRebuildBatchCap()
            << " capPerWake=" << geometryCapturePerWake()
            << " capWakeUs=" << geometryCaptureWakeMicros()
            << " budgetMs=" << sectionRebuildTimeBudgetMs()
            << " maxSectionMs=" << g_geometryMaxSectionMs
            << " preLoopMs=" << g_geometryMaxPreLoopMs
            << " buildLoopMs=" << g_geometryMaxBuildLoopMs
            << " tailMs=" << g_geometryMaxTailMs
            << " poolHit=" << g_sectionMemoryPoolHits
            << " poolMiss=" << g_sectionMemoryPoolMisses
            << " allocCalls=" << g_sectionMemoryAllocateCalls
            << std::fixed << std::setprecision(1)
            << " allocMs=" << g_sectionMemoryAllocateMs
            << " poolKeptMB="
            << (static_cast<double>(g_sectionMemoryPoolKeptBytes) /
                1048576.0)
            << " buildPoolReuse=" << g_sectionBuildPoolReuseCount
            << " buildPoolAlloc=" << g_sectionBuildPoolAllocCount
            << " asCreateN=" << g_sectionASCreateCalls
            << " asRetireN=" << g_sectionASRetireCalls
            << std::fixed << std::setprecision(2)
            << " asCreateMs=" << g_sectionASCreateMs
            << " asRetireMs=" << g_sectionASRetireMs
            << " asSameTris=" << g_sectionASRebuildSameTris
            << " asRefitCand=" << g_sectionASRefitCandidateHits
            << " asRefitHits=" << g_sectionASRefitHits
            << std::fixed << std::setprecision(1)
            << std::fixed << std::setprecision(1)
            << " upWaitMs="
            << (g_chunkLockMaxWaitNanos.load(std::memory_order_relaxed) / 1.0e6)
            << " upHoldMs="
            << (g_chunkLockMaxHoldNanos.load(std::memory_order_relaxed) / 1.0e6)
            << " capHoldMs="
            << (g_captureLockMaxHoldNanos.load(std::memory_order_relaxed) / 1.0e6)
            << " capWaitMs="
            << (g_captureLockMaxWaitNanos.load(std::memory_order_relaxed) / 1.0e6);

        {
            std::lock_guard<std::mutex> capturedLock(g_capturedSectionsMutex);
            countersLog << " queued="
            << (g_capturedSections.size() - g_capturedSectionsHead)
            << " lockSkips=" << g_geometryPreLoopLockSkips
            << " lockWaitMs=" << g_geometryMaxPreLoopLockWaitMs
            << " logMs=" << g_geometryMaxCountersLogMs;
        }

        RTLog(countersLog.str());

        logNativeMemoryReport();

        logVramGovernor();

        g_geometryMaxSectionMs = 0.0;
        g_geometryMaxPreLoopMs = 0.0;
    g_geometryMaxPreLoopLockWaitMs = 0.0;
    g_geometryMaxCountersLogMs = 0.0;
        g_geometryMaxBuildLoopMs = 0.0;
        g_geometryMaxTailMs = 0.0;
        g_sectionMemoryPoolHits = 0u;
        g_sectionMemoryPoolMisses = 0u;
        g_sectionMemoryAllocateCalls = 0u;
        g_sectionMemoryAllocateMs = 0.0;
        g_sectionASCreateCalls = 0u;
        g_sectionASRetireCalls = 0u;
        g_sectionASCreateMs = 0.0;
        g_sectionASRetireMs = 0.0;
        g_sectionASRebuildSameTris = 0u;
        g_sectionASRefitCandidateHits = 0u;
        g_sectionASRefitHits = 0u;
        g_chunkLockMaxWaitNanos.store(0u, std::memory_order_relaxed);
        g_chunkLockMaxHoldNanos.store(0u, std::memory_order_relaxed);
        g_captureLockMaxHoldNanos.store(0u, std::memory_order_relaxed);
        g_captureLockMaxWaitNanos.store(0u, std::memory_order_relaxed);
    }


    const std::chrono::steady_clock::time_point rtxPreLoopMarkMid =
        std::chrono::steady_clock::now();

    {
        const double countersLogMs =
            std::chrono::duration<double, std::milli>(
                rtxPreLoopMarkMid - rtxPreLoopMarkLog).count();

        if(countersLogMs > g_geometryMaxCountersLogMs)
        {
            g_geometryMaxCountersLogMs = countersLogMs;
        }
    }

    if(
        !forceReset &&
        sectionUpdates.empty()
    )
    {
        const double rtxEmptyTotalMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() -
                perfL10RebuildStart
            ).count();

        if(rtxEmptyTotalMs >= 15.0)
        {
            RTLog(
                "[RT GEOMETRY SLOW] frame=" +
                std::to_string(g_perfL10FrameSerial) +
                " total=" + std::to_string(rtxEmptyTotalMs) +
                " sections=0 empty=1");
        }

        return true;
    }

    if(!getDestroyAccelerationStructure())
    {
        throw std::runtime_error(
            "vkDestroyAccelerationStructureKHR unavailable"
        );
    }

    const uint32_t previousRetirementSlot =
        g_currentRetirementSlot;

    g_currentRetirementSlot =
        acquireRetirementSlot();

    g_sectionRetirement =
        &g_sectionRetirementRing[g_currentRetirementSlot];

    g_sectionRetirement->fencePending =
        false;

    g_sectionRetirement->deferredBuild =
        std::move(g_deferredSectionBuild);

    g_deferredSectionBuild = {};

    g_sectionRetirement->occupied =
        true;


    if(forceReset)
    {
        for(auto& entry : g_sectionBLAS)
        {
            retireSectionAS(
                entry.second
            );
        }


        g_sectionBLAS.clear();
    }


    uint32_t removedSectionCount = 0;
    uint32_t rebuiltSectionCount = 0;
    uint64_t rebuiltOpaqueTriangleCount = 0;
    uint64_t rebuiltOmmTriangleCount = 0;
    uint64_t rebuiltAlphaTriangleCount = 0;
    uint64_t rebuiltWaterTriangleCount = 0;
    uint64_t rebuiltParticleTriangleCount = 0;

    std::vector<RTSectionAS*>
        sectionsNeedingBuild;

    sectionsNeedingBuild.reserve(
        sectionUpdates.size() * 7u
    );

    g_sectionBLAS.reserve(
        g_sectionBLAS.size() + sectionUpdates.size() * 7u
    );


    struct PendingSectionUpload
    {
        RTSectionAS* section = nullptr;
        const std::vector<Vertex>* vertices = nullptr;
        VkDeviceSize stagingOffset = 0;
        const std::vector<float>* motion = nullptr;
        const std::vector<SectionVertex>* packed = nullptr;
    };


    std::vector<PendingSectionUpload>
        pendingUploads;

    pendingUploads.reserve(
        sectionUpdates.size() * 7u
    );


    VkDeviceSize totalStagingBytes = 0;


    VkDeviceSize maxBLASScratchSize = 0;


    const double sectionRebuildBudgetMs = sectionRebuildTimeBudgetMs();


    std::chrono::steady_clock::time_point previousSectionStart{};
    bool previousSectionTimingArmed = false;

    const std::chrono::steady_clock::time_point perfL10BuildLoopStart =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point perfL10AfterBuildLoop =
        perfL10BuildLoopStart;

    const auto rebuildElapsedMsNow =
        [&]() -> double
        {
            return std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - perfL10RebuildStart
            ).count();
        };

    const auto requeueSectionsFrom =
        [&](size_t fromIndex)
        {
            {
                std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
                for(size_t pending = fromIndex;
                    pending < sectionUpdates.size();
                    ++pending)
                {
                    g_dirtySectionKeys.insert(sectionUpdates[pending].first);
                }
                g_worldGeometryDirty.store(
                    !g_dirtySectionKeys.empty(),
                    std::memory_order_release
                );
            }
            g_sectionRebuildInterrupted = true;
            g_sectionRebuildContinueFrame =
                g_perfL10FrameSerial + SECTION_REBUILD_CONTINUE_FRAMES;
            g_sectionRebuildBatchCap = std::max<size_t>(
                1u,
                g_sectionRebuildBatchCap / 2u
            );
        };

    for(size_t sectionUpdateIndex = 0u;
        sectionUpdateIndex < sectionUpdates.size();
        ++sectionUpdateIndex)
    {
        {
            const std::chrono::steady_clock::time_point sectionNow =
                std::chrono::steady_clock::now();

            if(sectionUpdateIndex == 0u)
            {
                const double preLoopMs =
                    std::chrono::duration<double, std::milli>(
                        sectionNow - perfL10RebuildStart
                    ).count();

                if(preLoopMs > g_geometryMaxPreLoopMs)
                {
                    g_geometryMaxPreLoopMs = preLoopMs;
                }
            }

            if(previousSectionTimingArmed)
            {
                const double previousSectionMs =
                    std::chrono::duration<double, std::milli>(
                        sectionNow - previousSectionStart
                    ).count();

                if(previousSectionMs > g_geometryMaxSectionMs)
                {
                    g_geometryMaxSectionMs = previousSectionMs;
                }
            }

            previousSectionStart = sectionNow;
            previousSectionTimingArmed = true;
        }

        auto& update = sectionUpdates[sectionUpdateIndex];

        if(sectionUpdateIndex != 0u && g_asBuilt && !forceReset)
        {
            const double rebuildElapsedMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - perfL10RebuildStart
                ).count();
            if(rebuildElapsedMs >= sectionRebuildBudgetMs)
            {
                {
                    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
                    for(size_t pending = sectionUpdateIndex;
                        pending < sectionUpdates.size();
                        ++pending)
                    {
                        g_dirtySectionKeys.insert(sectionUpdates[pending].first);
                    }
                    g_worldGeometryDirty.store(
                        !g_dirtySectionKeys.empty(),
                        std::memory_order_release
                    );
                }
                g_sectionRebuildInterrupted = true;
                g_sectionRebuildContinueFrame =
                    g_perfL10FrameSerial + SECTION_REBUILD_CONTINUE_FRAMES;
                g_sectionRebuildBatchCap = std::max<size_t>(
                    1u,
                    g_sectionRebuildBatchCap / 2u
                );
                break;
            }
        }

        const uint64_t spatialSectionKey =
            update.first;

        auto rebuildGeometryClass =
            [&]
            (
                uint64_t classKey,
                const std::vector<Vertex>& vertices,
                bool opaqueGeometry,
                const OmmCutoutInput* ommInput,
                const std::vector<float>* vertexMotion,
                const std::vector<SectionVertex>* packedVertices
            )
            {
                if(vramGovernorMode() >= 2 &&
                   g_vramGovernor.over &&
                   g_nonResidentSections.count(
                       classKey & SPATIAL_SECTION_KEY_MASK) != 0u)
                {
                    return;
                }

                uint32_t previousTriangleCount = 0u;
                uint32_t previousMotionFloats = 0u;
                VkDeviceSize previousVertexBytes = 0u;
                VkDeviceSize previousUpdateScratchSize = 0u;
                bool previousHadOpacityMicromap = false;
                bool previousRefitCapable = false;
                bool hadPreviousSection = false;

                if(auto old = g_sectionBLAS.find(classKey);
                   old != g_sectionBLAS.end())
                {
                    previousTriangleCount = old->second.triangleCount;
                    previousMotionFloats = old->second.vertexMotionFloats;
                    previousVertexBytes = old->second.vertexBytes;
                    previousUpdateScratchSize = old->second.updateScratchSize;
                    previousHadOpacityMicromap = old->second.hasOpacityMicromap;
                    previousRefitCapable = old->second.refitCapable;
                    hadPreviousSection = true;
                }

                const auto retirePreviousSection =
                    [&]()
                    {
                        if(!hadPreviousSection)
                        {
                            return;
                        }

                        auto old = g_sectionBLAS.find(classKey);

                        if(old == g_sectionBLAS.end())
                        {
                            return;
                        }

                        const std::chrono::steady_clock::time_point retireStart =
                            std::chrono::steady_clock::now();

                        retireSectionAS(old->second);

                        g_sectionASRetireMs +=
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - retireStart
                            ).count();
                        ++g_sectionASRetireCalls;

                        g_sectionBLAS.erase(old);
                        hadPreviousSection = false;
                    };

                g_nonResidentSections.erase(
                    classKey & SPATIAL_SECTION_KEY_MASK
                );

                if(vertices.empty())
                {
                    retirePreviousSection();
                    return;
                }

                if(hadPreviousSection)
                {
                    const bool newIndexedGeometry =
                        sectionUsesIndexedGeometry(classKey, vertices.size());

                    const uint32_t newTriangleCount =
                        newIndexedGeometry
                            ? static_cast<uint32_t>((vertices.size() / 4u) * 2u)
                            : static_cast<uint32_t>(vertices.size() / 3u);

                    if(newTriangleCount == previousTriangleCount)
                    {
                        ++g_sectionASRebuildSameTris;

                        if(!previousHadOpacityMicromap)
                        {
                            ++g_sectionASRefitCandidateHits;
                        }
                    }
                }

                if(hadPreviousSection &&
                   previousRefitCapable &&
                   previousUpdateScratchSize != 0u &&
                   sectionBLASRefitEnabled())
                {
                    const bool newIndexedGeometry =
                        sectionUsesIndexedGeometry(classKey, vertices.size());

                    const uint32_t newTriangleCount =
                        newIndexedGeometry
                            ? static_cast<uint32_t>((vertices.size() / 4u) * 2u)
                            : static_cast<uint32_t>(vertices.size() / 3u);

                    const uint32_t newMotionFloats =
                        (vertexMotion != nullptr && !vertexMotion->empty())
                            ? static_cast<uint32_t>(vertices.size() * 3u)
                            : 0u;

                    const VkDeviceSize newVertexStride =
                        static_cast<VkDeviceSize>(
                            sectionUsesPackedVertices(
                                classKey,
                                newIndexedGeometry,
                                vertices.size())
                                ? sizeof(SectionVertex)
                                : sizeof(Vertex));

                    VkDeviceSize newVertexBytes =
                        static_cast<VkDeviceSize>(vertices.size()) *
                        newVertexStride;

                    if(newMotionFloats != 0u)
                    {
                        newVertexBytes +=
                            static_cast<VkDeviceSize>(newMotionFloats) *
                            sizeof(float);
                    }

                    if(newTriangleCount == previousTriangleCount &&
                       newVertexBytes == previousVertexBytes &&
                       newMotionFloats == previousMotionFloats &&
                       !previousHadOpacityMicromap)
                    {
                        auto refitEntry = g_sectionBLAS.find(classKey);

                        if(refitEntry != g_sectionBLAS.end())
                        {
                            RTSectionAS& refitSection = refitEntry->second;

                            refitSection.pendingRefit = true;
                            sectionsNeedingBuild.push_back(&refitSection);

                            constexpr VkDeviceSize STAGING_ALIGNMENT = 16;

                            if(totalStagingBytes >
                               std::numeric_limits<VkDeviceSize>::max() -
                                   (STAGING_ALIGNMENT - 1))
                            {
                                throw std::runtime_error(
                                    "Section refit staging offset overflow");
                            }

                            const VkDeviceSize stagingOffset =
                                (totalStagingBytes + STAGING_ALIGNMENT - 1) &
                                ~(STAGING_ALIGNMENT - 1);

                            if(stagingOffset >
                               std::numeric_limits<VkDeviceSize>::max() -
                                   refitSection.vertexBytes)
                            {
                                throw std::runtime_error(
                                    "Section refit staging size overflow");
                            }

                            pendingUploads.push_back(
                                { &refitSection, &vertices, stagingOffset,
                                  vertexMotion, packedVertices }
                            );
                            totalStagingBytes =
                                stagingOffset + refitSection.vertexBytes;
                            maxBLASScratchSize = std::max(
                                maxBLASScratchSize,
                                refitSection.updateScratchSize
                            );

                            if(opaqueGeometry)
                            {
                                rebuiltOpaqueTriangleCount +=
                                    refitSection.triangleCount;
                            }
                            else if(
                                (classKey & ~SPATIAL_SECTION_KEY_MASK) ==
                                SECTION_AS_CLASS_WATER
                            )
                            {
                                rebuiltWaterTriangleCount +=
                                    refitSection.triangleCount;
                            }

                            ++g_sectionASRefitHits;
                            return;
                        }
                    }
                }

                retirePreviousSection();

                RTSectionAS newSection{};
                try
                {
                    const std::chrono::steady_clock::time_point createStart =
                        std::chrono::steady_clock::now();

                    const VkDeviceSize scratchSize =
                        createSectionBLAS(
                            newSection,
                            classKey,
                            vertices,
                            opaqueGeometry,
                            ommInput,
                            vertexMotion,
                            sectionBLASRefitEnabled()
                        );

                    g_sectionASCreateMs +=
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - createStart
                        ).count();
                    ++g_sectionASCreateCalls;

                    auto insertResult =
                        g_sectionBLAS.emplace(
                            classKey,
                            std::move(newSection)
                        );
                    if(!insertResult.second)
                    {
                        throw std::runtime_error("duplicate Section BLAS key");
                    }

                    RTSectionAS& insertedSection =
                        insertResult.first->second;
                    newSection = {};
                    sectionsNeedingBuild.push_back(&insertedSection);

                    constexpr VkDeviceSize STAGING_ALIGNMENT = 16;
                    if(totalStagingBytes >
                       std::numeric_limits<VkDeviceSize>::max() -
                           (STAGING_ALIGNMENT - 1))
                    {
                        throw std::runtime_error("Section staging offset overflow");
                    }

                    const VkDeviceSize stagingOffset =
                        (totalStagingBytes + STAGING_ALIGNMENT - 1) &
                        ~(STAGING_ALIGNMENT - 1);
                    if(stagingOffset >
                       std::numeric_limits<VkDeviceSize>::max() -
                           insertedSection.vertexBytes)
                    {
                        throw std::runtime_error("Section staging size overflow");
                    }

                    pendingUploads.push_back(
                        { &insertedSection, &vertices, stagingOffset,
                          vertexMotion, packedVertices }
                    );
                    totalStagingBytes =
                        stagingOffset + insertedSection.vertexBytes;
                    maxBLASScratchSize = std::max(
                        maxBLASScratchSize,
                        scratchSize
                    );
                    if(opaqueGeometry)
                    {
                        rebuiltOpaqueTriangleCount +=
                            insertedSection.triangleCount;
                    }
                    else if(insertedSection.hasOpacityMicromap)
                    {
                        rebuiltOmmTriangleCount +=
                            insertedSection.triangleCount;
                    }
                    else if(
                        (classKey & ~SPATIAL_SECTION_KEY_MASK) ==
                        SECTION_AS_CLASS_WATER
                    )
                    {
                        rebuiltWaterTriangleCount +=
                            insertedSection.triangleCount;
                    }
                    else if(
                        (classKey & ~SPATIAL_SECTION_KEY_MASK) ==
                        SECTION_AS_CLASS_PARTICLE
                    )
                    {
                        rebuiltParticleTriangleCount +=
                            insertedSection.triangleCount;
                    }
                    else
                    {
                        rebuiltAlphaTriangleCount +=
                            insertedSection.triangleCount;
                    }
                    ++rebuiltSectionCount;
                }
                catch(const SectionMemoryUnavailableError&)
                {
                    destroySectionAS(newSection);
                    g_sectionMemoryOomSkipCount += 1u;
                    g_nonResidentSections.insert(
                        classKey & SPATIAL_SECTION_KEY_MASK
                    );
                    RTLogFlushed(
                        "[SECTION OOM] build skipped (out of device memory), key=" +
                        std::to_string(classKey) +
                        " totalSkips=" +
                        std::to_string(g_sectionMemoryOomSkipCount)
                    );
                    return;
                }
                catch(...)
                {
                    destroySectionAS(newSection);
                    throw;
                }
            };

        if(update.second.opaque.empty() &&
           update.second.ommCutout.empty() &&
           update.second.alpha.empty() &&
           update.second.water.empty() &&
           update.second.entity.empty() &&
           update.second.localPlayer.empty() &&
           update.second.particle.empty())
        {
            ++removedSectionCount;
        }

        struct RtGeometryClassPlan
        {
            uint64_t classKey;
            const std::vector<Vertex>* vertices;
            bool opaqueGeometry;
            const OmmCutoutInput* ommInput;
            const std::vector<float>* vertexMotion;
            const std::vector<SectionVertex>* packed;
        };

        constexpr size_t RT_GEOMETRY_CLASS_COUNT = 7u;

        const RtGeometryClassPlan geometryClassPlan[RT_GEOMETRY_CLASS_COUNT] =
        {
            { spatialSectionKey | SECTION_AS_CLASS_OPAQUE,
              &update.second.opaque, true, nullptr, nullptr,
              &update.second.opaquePacked },
            { spatialSectionKey | SECTION_AS_CLASS_OMM,
              &update.second.ommCutout, false,
              update.second.hasOmmInput ? &update.second.ommInput : nullptr,
              nullptr, &update.second.ommCutoutPacked },
            { spatialSectionKey | SECTION_AS_CLASS_ALPHA,
              &update.second.alpha, false, nullptr, nullptr,
              &update.second.alphaPacked },
            { spatialSectionKey | SECTION_AS_CLASS_WATER,
              &update.second.water, false, nullptr, nullptr,
              &update.second.waterPacked },
            { spatialSectionKey | SECTION_AS_CLASS_ENTITY,
              &update.second.entity, false, nullptr,
              &update.second.entityMotion, nullptr },
            { spatialSectionKey | SECTION_AS_CLASS_LOCAL_PLAYER,
              &update.second.localPlayer, false, nullptr,
              &update.second.localPlayerMotion, nullptr },
            { spatialSectionKey | SECTION_AS_CLASS_PARTICLE,
              &update.second.particle, false, nullptr,
              &update.second.particleMotion, nullptr }
        };

        bool sectionAbortedMidway = false;

        for(size_t classIndex = 0u;
            classIndex < RT_GEOMETRY_CLASS_COUNT;
            ++classIndex)
        {
            if(classIndex != 0u &&
               sectionUpdateIndex != 0u &&
               g_asBuilt &&
               !forceReset &&
               rebuildElapsedMsNow() >= sectionRebuildBudgetMs)
            {
                requeueSectionsFrom(sectionUpdateIndex);
                sectionAbortedMidway = true;
                break;
            }

            const RtGeometryClassPlan& plan =
                geometryClassPlan[classIndex];

            rebuildGeometryClass(
                plan.classKey,
                *plan.vertices,
                plan.opaqueGeometry,
                plan.ommInput,
                plan.vertexMotion,
                plan.packed
            );
        }

        if(sectionAbortedMidway)
        {
            break;
        }
    }

    perfL10AfterBuildLoop =
        std::chrono::steady_clock::now();

    std::chrono::steady_clock::time_point rtxTailAfterStaging =
        perfL10AfterBuildLoop;
    std::chrono::steady_clock::time_point rtxPreCmdAfterOrdered =
        perfL10AfterBuildLoop;
    std::chrono::steady_clock::time_point rtxPreCmdAfterEvict =
        perfL10AfterBuildLoop;
    std::chrono::steady_clock::time_point rtxPreCmdAfterInstances =
        perfL10AfterBuildLoop;
    std::chrono::steady_clock::time_point rtxPreCmdAfterTlas =
        perfL10AfterBuildLoop;
    std::chrono::steady_clock::time_point rtxTailBeforeCommandRecord =
        perfL10AfterBuildLoop;
    std::chrono::steady_clock::time_point rtxTailAfterCommandRecord =
        perfL10AfterBuildLoop;
    std::chrono::steady_clock::time_point rtxTailAfterFenceWait =
        perfL10AfterBuildLoop;
    std::chrono::steady_clock::time_point rtxTailAfterSubmit =
        perfL10AfterBuildLoop;

    {
        const double buildLoopMs =
            std::chrono::duration<double, std::milli>(
                perfL10AfterBuildLoop - perfL10BuildLoopStart
            ).count();

        if(buildLoopMs > g_geometryMaxBuildLoopMs)
        {
            g_geometryMaxBuildLoopMs = buildLoopMs;
        }
    }

    if(!sectionUpdates.empty())
    {
        g_sectionRebuildBatchCap = std::min<size_t>(
            SECTION_REBUILD_MAX_SECTIONS_PER_BATCH,
            g_sectionRebuildBatchCap + 2u
        );
    }
    g_sectionRebuildInterrupted = false;
    g_sectionRebuildContinueFrame = 0u;


    VkBuffer vertexStagingBuffer =
        VK_NULL_HANDLE;

    VkDeviceMemory vertexStagingMemory =
        VK_NULL_HANDLE;


    if(!pendingUploads.empty())
    {
        RtxSectionBuildBufferSlot vertexStagingSlot;

        if(g_vertexStagingPool.tryAcquire(totalStagingBytes, vertexStagingSlot))
        {
            vertexStagingBuffer = vertexStagingSlot.buffer;
            vertexStagingMemory = vertexStagingSlot.memory;

            g_sectionBuildPoolReuseCount += 1u;
        }
        else
        {

            const VkDeviceSize vertexStagingSlotBytes =
                sectionBuildPoolSlotBytes(totalStagingBytes);

            createBuffer(
                g_device,
                g_physicalDevice,
                vertexStagingSlotBytes,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                vertexStagingBuffer,
                vertexStagingMemory,
                true
            );

            g_vertexStagingPool.adopt(
                vertexStagingBuffer,
                vertexStagingMemory,
                vertexStagingSlotBytes
            );

            g_sectionBuildPoolAllocCount += 1u;
        }


        void* mappedStaging = nullptr;

        check(
            vkMapMemory(
                g_device,
                vertexStagingMemory,
                0,
                totalStagingBytes,
                0,
                &mappedStaging
            ),
            "map batched Section staging buffer failed"
        );


        uint8_t* stagingBytes =
            static_cast<uint8_t*>(
                mappedStaging
            );


        for(const PendingSectionUpload& upload : pendingUploads)
        {
            const VkDeviceSize vertexOnlyBytes =
                static_cast<VkDeviceSize>(
                    upload.vertices->size()
                ) *
                static_cast<VkDeviceSize>(
                    upload.section->vertexStride);

            if(upload.section->packedSectionVertices &&
               upload.packed != nullptr &&
               upload.packed->size() == upload.vertices->size())
            {
                std::memcpy(
                    stagingBytes +
                        static_cast<size_t>(upload.stagingOffset),
                    upload.packed->data(),
                    static_cast<size_t>(upload.vertices->size()) *
                        sizeof(SectionVertex)
                );
            }
            else if(upload.section->packedSectionVertices)
            {
                SectionVertex* const packed =
                    reinterpret_cast<SectionVertex*>(
                        stagingBytes +
                        static_cast<size_t>(upload.stagingOffset));

                const std::vector<Vertex>& source = *upload.vertices;

                for(size_t vertexIndex = 0u;
                    vertexIndex < source.size();
                    ++vertexIndex)
                {
                    packed[vertexIndex] = makeSectionVertex(source[vertexIndex]);
                }
            }
            else
            {
                std::memcpy(
                    stagingBytes +
                        static_cast<size_t>(
                            upload.stagingOffset
                        ),
                    upload.vertices->data(),
                    static_cast<size_t>(
                        vertexOnlyBytes
                    )
                );
            }

            const VkDeviceSize motionTailBytes =
                upload.section->vertexBytes -
                vertexOnlyBytes;

            if(motionTailBytes > 0u)
            {
                uint8_t* const tail =
                    stagingBytes +
                    static_cast<size_t>(upload.stagingOffset) +
                    static_cast<size_t>(vertexOnlyBytes);

                std::memset(
                    tail,
                    0,
                    static_cast<size_t>(motionTailBytes)
                );

                if(
                    upload.motion != nullptr &&
                    !upload.motion->empty()
                )
                {
                    std::memcpy(
                        tail,
                        upload.motion->data(),
                        std::min(
                            static_cast<size_t>(motionTailBytes),
                            upload.motion->size() * sizeof(float)
                        )
                    );
                }
            }
        }


        vkUnmapMemory(
            g_device,
            vertexStagingMemory
        );
    }


    rtxTailAfterStaging = std::chrono::steady_clock::now();

    std::vector<RTSectionAS*>
        orderedSections;

    orderedSections.reserve(
        g_sectionBLAS.size()
    );


    for(auto& entry : g_sectionBLAS)
    {
        if(
            entry.second.blas == VK_NULL_HANDLE ||
            entry.second.blasAddress == 0 ||
            entry.second.vertexAddress == 0
        )
        {
            throw std::runtime_error(
                "Section AS map contains an incomplete entry"
            );
        }


        orderedSections.push_back(
            &entry.second
        );
    }


    std::sort(
        orderedSections.begin(),
        orderedSections.end(),
        [](
            const RTSectionAS* a,
            const RTSectionAS* b
        )
        {
            return
                a->sectionKey <
                b->sectionKey;
        }
    );


    for(auto it = orderedSections.begin(); it != orderedSections.end(); )
    {
        RTSectionAS* section = *it;

        if(!ensureSectionVertexAddressSlot(*section))
        {
            it = orderedSections.erase(it);
            continue;
        }

        writeSectionRoleSlot(
            *section
        );

        ++it;
    }


    rebuildLodCoverage(
        orderedSections
    );


    constexpr size_t MAX_TLAS_CUSTOM_INDEX_COUNT =
        static_cast<size_t>(1u << 24u);


    if(
        orderedSections.size() >
        MAX_TLAS_CUSTOM_INDEX_COUNT
    )
    {
        throw std::runtime_error(
            "Section count exceeds instanceCustomIndex 24-bit capacity"
        );
    }


    static float s_tlasFrustumPlanes[5][4] = {};
    static bool s_tlasFrustumPlanesValid = false;
    s_tlasFrustumPlanesValid = false;

    static float s_tlasBasePlanes[6][4] = {};
    static bool s_tlasBasePlanesValid = false;
    s_tlasBasePlanesValid = false;
    {
        const CameraUBO* frustumUbo =
            g_cameraUboMapped != nullptr
                ? static_cast<const CameraUBO*>(g_cameraUboMapped)
                : nullptr;

        if(frustumUbo != nullptr)
        {
            float view[16];
            float proj[16];
            float viewProj[16];

            if(mat4Invert(frustumUbo->viewInverse, view) &&
               mat4Invert(frustumUbo->projInverse, proj))
            {
                mat4Mul(proj, view, viewProj);

                const float (*m)[4] =
                    reinterpret_cast<const float (*)[4]>(viewProj);
                float planes[5][4] = {
                    { m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0] },
                    { m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0] },
                    { m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1] },
                    { m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1] },
                    { m[0][3] + m[0][2], m[1][3] + m[1][2], m[2][3] + m[2][2], m[3][3] + m[3][2] },
                };

                bool allFinite = true;
                for(int p = 0; p < 5; ++p)
                {
                    const float len = std::sqrt(
                        planes[p][0] * planes[p][0] +
                        planes[p][1] * planes[p][1] +
                        planes[p][2] * planes[p][2]);

                    if(len > 1.0e-8f)
                    {
                        for(int k = 0; k < 4; ++k)
                        {
                            s_tlasFrustumPlanes[p][k] = planes[p][k] / len;
                            if(!std::isfinite(s_tlasFrustumPlanes[p][k]))
                            {
                                allFinite = false;
                            }
                        }
                    }
                    else
                    {
                        allFinite = false;
                    }
                }

                s_tlasFrustumPlanesValid = allFinite;

                float basePlanes[6][4];

                for(int k = 0; k < 4; ++k)
                {
                    basePlanes[0][k] = planes[0][k];
                    basePlanes[1][k] = planes[1][k];
                    basePlanes[2][k] = planes[2][k];
                    basePlanes[3][k] = planes[3][k];
                    basePlanes[4][k] = m[k][3] - m[k][2];
                    basePlanes[5][k] = planes[4][k];
                }

                bool baseFinite = allFinite;

                for(int p = 0; p < 6; ++p)
                {
                    const float len = std::sqrt(
                        basePlanes[p][0] * basePlanes[p][0] +
                        basePlanes[p][1] * basePlanes[p][1] +
                        basePlanes[p][2] * basePlanes[p][2]);

                    if(len > 1.0e-8f)
                    {
                        for(int k = 0; k < 4; ++k)
                        {
                            s_tlasBasePlanes[p][k] = basePlanes[p][k] / len;

                            if(!std::isfinite(s_tlasBasePlanes[p][k]))
                            {
                                baseFinite = false;
                            }
                        }
                    }
                    else
                    {
                        baseFinite = false;
                    }
                }

                s_tlasBasePlanesValid = baseFinite;
            }
        }
    }

    static float s_sunShadowKeepBasis[3][3] = {};
    static float s_sunShadowKeepCenter[3] = {};
    static bool s_sunShadowKeepValid = false;
    s_sunShadowKeepValid = false;
    {
        const CameraUBO* keepUbo =
            g_cameraUboMapped != nullptr
                ? static_cast<const CameraUBO*>(g_cameraUboMapped)
                : nullptr;

        if(keepUbo != nullptr)
        {
            const float phase = keepUbo->historyParamsPadding1;
            const float angle =
                (phase - std::floor(phase)) * 6.28318530718f;
            float sunX = std::cos(angle);
            float sunY = 0.81915204429f * std::sin(angle);
            float sunZ = 0.57357643635f * std::sin(angle);
            const float sunLen =
                std::sqrt(sunX * sunX + sunY * sunY + sunZ * sunZ);

            if(sunLen > 1.0e-8f)
            {
                sunX /= sunLen;
                sunY /= sunLen;
                sunZ /= sunLen;
                if(sunY < 0.0f)
                {
                    sunX = -sunX;
                    sunY = -sunY;
                    sunZ = -sunZ;
                }

                float rightX;
                float rightY;
                float rightZ;
                if(std::fabs(sunY) > 0.99f)
                {
                    rightX = 0.0f;
                    rightY = -sunZ;
                    rightZ = sunY;
                }
                else
                {
                    rightX = sunZ;
                    rightY = 0.0f;
                    rightZ = -sunX;
                }

                const float rightLen = std::sqrt(
                    rightX * rightX + rightY * rightY + rightZ * rightZ);

                if(rightLen > 1.0e-8f)
                {
                    rightX /= rightLen;
                    rightY /= rightLen;
                    rightZ /= rightLen;

                    float upX = sunY * rightZ - sunZ * rightY;
                    float upY = sunZ * rightX - sunX * rightZ;
                    float upZ = sunX * rightY - sunY * rightX;
                    const float upLen =
                        std::sqrt(upX * upX + upY * upY + upZ * upZ);

                    if(upLen > 1.0e-8f)
                    {
                        upX /= upLen;
                        upY /= upLen;
                        upZ /= upLen;

                        for(int axis = 0; axis < 3; ++axis)
                        {
                            s_sunShadowKeepCenter[axis] =
                                keepUbo->viewInverse[12 + axis];
                        }

                        s_sunShadowKeepBasis[0][0] = rightX;
                        s_sunShadowKeepBasis[0][1] = rightY;
                        s_sunShadowKeepBasis[0][2] = rightZ;
                        s_sunShadowKeepBasis[1][0] = upX;
                        s_sunShadowKeepBasis[1][1] = upY;
                        s_sunShadowKeepBasis[1][2] = upZ;
                        s_sunShadowKeepBasis[2][0] = sunX;
                        s_sunShadowKeepBasis[2][1] = sunY;
                        s_sunShadowKeepBasis[2][2] = sunZ;
                        s_sunShadowKeepValid = true;
                    }
                }
            }
        }
    }

    static float s_casterPlanes[13][4] = {};
    static int s_casterPlaneCount = 0;
    s_casterPlaneCount = 0;

    if(s_tlasBasePlanesValid && s_sunShadowKeepValid)
    {
        const float* light = s_sunShadowKeepBasis[2];

        bool isBack[6] = { false, false, false, false, false, false };

        for(int p = 0; p < 6; ++p)
        {
            const float dot =
                s_tlasBasePlanes[p][0] * light[0] +
                s_tlasBasePlanes[p][1] * light[1] +
                s_tlasBasePlanes[p][2] * light[2];

            isBack[p] = dot > 0.0f;

            if(dot >= 0.0f && s_casterPlaneCount < 13)
            {
                for(int k = 0; k < 4; ++k)
                {
                    s_casterPlanes[s_casterPlaneCount][k] =
                        s_tlasBasePlanes[p][k];
                }

                s_casterPlaneCount += 1;
            }
        }

        static const int kNeighborPlanes[3][4] =
        {
            { 2, 3, 4, 5 },
            { 0, 1, 4, 5 },
            { 0, 1, 2, 3 },
        };

        for(int p = 0; p < 6; ++p)
        {
            if(!isBack[p])
            {
                continue;
            }

            for(int n = 0; n < 4; ++n)
            {
                const int front = kNeighborPlanes[p >> 1][n];

                if(isBack[front] || s_casterPlaneCount >= 13)
                {
                    continue;
                }

                const float* backPlane = s_tlasBasePlanes[p];
                const float* frontPlane = s_tlasBasePlanes[front];

                const float ix =
                    backPlane[1] * frontPlane[2] - backPlane[2] * frontPlane[1];
                const float iy =
                    backPlane[2] * frontPlane[0] - backPlane[0] * frontPlane[2];
                const float iz =
                    backPlane[0] * frontPlane[1] - backPlane[1] * frontPlane[0];

                const float intersectionLenSq = ix * ix + iy * iy + iz * iz;

                if(intersectionLenSq < 1.0e-12f)
                {
                    continue;
                }

                float nx = iy * light[2] - iz * light[1];
                float ny = iz * light[0] - ix * light[2];
                float nz = ix * light[1] - iy * light[0];

                const float normalLen = std::sqrt(nx * nx + ny * ny + nz * nz);

                if(normalLen < 1.0e-8f)
                {
                    continue;
                }

                nx /= normalLen;
                ny /= normalLen;
                nz /= normalLen;

                const float ixbX = iy * backPlane[2] - iz * backPlane[1];
                const float ixbY = iz * backPlane[0] - ix * backPlane[2];
                const float ixbZ = ix * backPlane[1] - iy * backPlane[0];

                const float fxiX = frontPlane[1] * iz - frontPlane[2] * iy;
                const float fxiY = frontPlane[2] * ix - frontPlane[0] * iz;
                const float fxiZ = frontPlane[0] * iy - frontPlane[1] * ix;

                const float invLenSq = 1.0f / intersectionLenSq;

                const float pointX =
                    (ixbX * (-frontPlane[3]) + fxiX * (-backPlane[3])) * invLenSq;
                const float pointY =
                    (ixbY * (-frontPlane[3]) + fxiY * (-backPlane[3])) * invLenSq;
                const float pointZ =
                    (ixbZ * (-frontPlane[3]) + fxiZ * (-backPlane[3])) * invLenSq;

                const float w = -(nx * pointX + ny * pointY + nz * pointZ);

                if(!std::isfinite(w))
                {
                    continue;
                }

                s_casterPlanes[s_casterPlaneCount][0] = nx;
                s_casterPlanes[s_casterPlaneCount][1] = ny;
                s_casterPlanes[s_casterPlaneCount][2] = nz;
                s_casterPlanes[s_casterPlaneCount][3] = w;
                s_casterPlaneCount += 1;
            }
        }
    }

    // ------------------------------------------------------------------
    // 2026-11-06：驱逐扫描按帧节流。
    //
    // 这段是 O(全部段) 的全表扫描：遍历 g_sectionBLAS（本局约 2000~2900 段）、
    // 每段解 spatialKey 求中心、过太阳阴影保留盒（9 次乘加）、再过视锥 5 平面，
    // 然后 sort 候选、remove_if 有序表、遍历 BLAS 逐条 retire。
    // 实测（16 区块跑图，[RT GEOMETRY SLOW] 92 个慢帧）单次 evict = 5.03ms，
    // p90 9.81ms，max 15.43ms —— 是 tail 里最大的一块。
    //
    // 而它只在 VRAM 超预算时才有意义，且 governor 自身是按
    // VRAM_GOVERNOR_SAMPLE_TICKS = 30 采样的。每次 rebuild 都扫一遍纯属浪费，
    // 这里按帧节流（默认 4 帧一次）：超预算时的响应仍比采样周期快一个数量级。
    // ORION_VRAM_EVICT_INTERVAL=1 回到逐帧（A/B 用）。
    // ------------------------------------------------------------------
    static const uint32_t sEvictInterval = []()
    {
        const char* raw = std::getenv("ORION_VRAM_EVICT_INTERVAL");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 4u;
        }

        const int parsed = std::atoi(raw);

        if(parsed < 1)
        {
            return 1u;
        }

        return static_cast<uint32_t>(parsed > 64 ? 64 : parsed);
    }();
    static uint32_t sEvictTick = 0u;

    const bool evictDue =
        sEvictInterval <= 1u ||
        ((sEvictTick++ % sEvictInterval) == 0u);

    // ------------------------------------------------------------------
    // 2026-11-06：段数/距离驱动的驱逐（不再只等 VRAM over）。
    //
    // 实测（16 区块跑图）：显存只到 65%（vramUsedMB=4646/7180）⇒ over 永不成立
    // ⇒ 跑过的 Section 永久留在 BLAS/TLAS 里 ⇒ meshSec 1280→5437、
    // geometry 段 0.78→5.59ms、fps 165→29。
    //
    // 这里补一条与显存无关的判据：段数超过上限时，用固定保留半径驱逐最远的段。
    // 非 over 路径**不缩**太阳阴影保留盒（shadowKeepScale=1）：那条收缩是为
    // 了压显存，正常运行时缩了会漏光（见下方 2026-11-03 的注释）。
    //
    // ORION_SECTION_MAX_COUNT   段数上限（默认 4000；=0 关闭）
    // ORION_SECTION_KEEP_RADIUS 保留半径（默认 384 格）
    // ------------------------------------------------------------------
    static const size_t sSectionMaxCount = []()
    {
        const char* raw = std::getenv("ORION_SECTION_MAX_COUNT");

        if(raw == nullptr || raw[0] == '\0')
        {
            // 2026-11-06b：默认关闭。
            // 实测 16 区块视野内的真实段数就是 ~6000（33x33 chunk x 每 chunk 数个
            // 垂直 Section），4000 上限让系统永远超限 ⇒ 每 4 帧全表扫 6000 段 +
            // 持续驱逐/重建（asRetireN 1130→2519、upWaitMs 12.8→19.2、fps 69.6→47.6）。
            // 只在"段数异常增长"的场景（例如传送/维度切换残留）用 env 手动开启。
            return static_cast<size_t>(0u);
        }

        const long parsed = std::atol(raw);

        return parsed > 0
            ? static_cast<size_t>(parsed)
            : static_cast<size_t>(0u);
    }();
    static const float sSectionKeepRadius = []()
    {
        const char* raw = std::getenv("ORION_SECTION_KEEP_RADIUS");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 384.0f;
        }

        const double parsed = std::atof(raw);

        return parsed > 16.0 ? static_cast<float>(parsed) : 384.0f;
    }();

    const bool sectionCountPressure =
        sSectionMaxCount > 0u &&
        orderedSections.size() > sSectionMaxCount;

    // 2026-11-06c：驱逐饱和保护。
    // 被驱逐进 nonResident 的段已经远多于还活着的段时，继续驱逐只会清空视野
    // （实测 nonResident 4637 / blas 2038 ⇒ 大片区块在 RT 里不可见），而释放的
    // 显存可以忽略（BLAS 合计 144MB）。此时停止驱逐，等 rehydrate 或手工降画质。
    // ORION_NONRESIDENT_GUARD=0 关闭该保护。
    static const size_t sNonResidentGuard = []()
    {
        const char* raw = std::getenv("ORION_NONRESIDENT_GUARD");

        if(raw == nullptr || raw[0] == '\0')
        {
            return static_cast<size_t>(3000u);
        }

        const long parsed = std::atol(raw);

        return parsed >= 0
            ? static_cast<size_t>(parsed)
            : static_cast<size_t>(3000u);
    }();

    const bool nonResidentSaturated =
        sNonResidentGuard > 0u &&
        g_nonResidentSections.size() > sNonResidentGuard;

    if(vramGovernorMode() >= 2 &&
       (g_vramGovernor.over || sectionCountPressure) &&
       evictDue &&
       !nonResidentSaturated &&
       !orderedSections.empty())
    {
        constexpr float VRAM_EVICT_SHRINK_RADIUS = 0.25f;
        // 2026-11-03：0.40 → 0.25。这个系数只缩**太阳阴影保留盒**，而 rgen 的
        // 太阳可见性射线（shadowRayCap = 160 格，rt_test.rgen:23947）长度不变。
        // 0.40 ⇒ 盒只剩 ±96 格：96~160 格内本该挡住太阳的墙/山被驱逐出 TLAS，
        // 射线一律 miss ⇒ 低太阳角时长影整段消失、墙面被判受光（"阳光穿墙"）。
        // 0.25 ⇒ ±120 格，漏光带从 64 格缩到 40 格，且只多保留一圈 Section。
        constexpr float VRAM_EVICT_SHRINK_SHADOW = 0.25f;

        const double overRatio =
            g_vramGovernor.budgetMb > 0.0
                ? g_vramGovernor.usedMb / g_vramGovernor.budgetMb
                : 0.0;

        const float overageT = 1.0f;

        (void)overRatio;

        // 非 over（段数驱动）时用固定的保留半径，且不缩阴影盒。
        const float keepRadius = g_vramGovernor.over
            ? vramEvictKeepRadius() *
                  (1.0f - VRAM_EVICT_SHRINK_RADIUS * overageT)
            : sSectionKeepRadius;
        const float evictRadius = keepRadius * VRAM_EVICT_HYSTERESIS;
        const float evictRadiusSq = evictRadius * evictRadius;

        const float shadowKeepScale = g_vramGovernor.over
            ? (1.0f - VRAM_EVICT_SHRINK_SHADOW * overageT)
            : 1.0f;

        const CameraUBO* evictUbo =
            g_cameraUboMapped != nullptr
                ? static_cast<const CameraUBO*>(g_cameraUboMapped)
                : nullptr;

        const float playerX = evictUbo ? evictUbo->playerX : 0.0f;
        const float playerY = evictUbo ? evictUbo->playerY : 0.0f;
        const float playerZ = evictUbo ? evictUbo->playerZ : 0.0f;

        struct VramEvictCandidate
        {
            uint64_t spatialKey = 0u;
            float distanceSq = 0.0f;
        };

        rtxPreCmdAfterOrdered = std::chrono::steady_clock::now();

        // 2026-11-06：候选容器改成静态复用。
        // 段数驱动驱逐让这段每 4 帧就跑一次，每次重建 ~4000 个哈希节点
        // 纯属浪费；clear() 保留 bucket，后续插入不再分配。
        // rebuildWorldGeometry 只在渲染线程调用，无需加锁。
        static std::vector<VramEvictCandidate> candidates;
        static std::unordered_set<uint64_t> seen;

        candidates.clear();
        candidates.reserve(g_sectionBLAS.size());
        seen.clear();

        {
            for(const auto& entry : g_sectionBLAS)
            {
                const uint64_t spatialKey =
                    entry.second.sectionKey & SPATIAL_SECTION_KEY_MASK;

                if(!seen.insert(spatialKey).second)
                {
                    continue;
                }

                if(!vramWorldHeightSection(spatialKey))
                {
                    continue;
                }

                float centerX = 0.0f;
                float centerY = 0.0f;
                float centerZ = 0.0f;
                vramSectionCenterFromSpatialKey(
                    spatialKey,
                    centerX,
                    centerY,
                    centerZ
                );

                const float dx = centerX - playerX;
                const float dy = centerY - playerY;
                const float dz = centerZ - playerZ;
                const float distanceSq = dx * dx + dy * dy + dz * dz;

                if(distanceSq <= evictRadiusSq)
                {
                    continue;
                }

                if(s_sunShadowKeepValid)
                {
                    constexpr float SECTION_HALF_DIAGONAL = 13.8564f;

                    const float keepX = centerX - s_sunShadowKeepCenter[0];
                    const float keepY = centerY - s_sunShadowKeepCenter[1];
                    const float keepZ = centerZ - s_sunShadowKeepCenter[2];

                    const bool insideKeepBox =
                        std::fabs(keepX * s_sunShadowKeepBasis[0][0] +
                            keepY * s_sunShadowKeepBasis[0][1] +
                            keepZ * s_sunShadowKeepBasis[0][2]) <=
                                SUN_SHADOW_EXTENT_XY * shadowKeepScale +
                                    SECTION_HALF_DIAGONAL &&
                        std::fabs(keepX * s_sunShadowKeepBasis[1][0] +
                            keepY * s_sunShadowKeepBasis[1][1] +
                            keepZ * s_sunShadowKeepBasis[1][2]) <=
                                SUN_SHADOW_EXTENT_XY * shadowKeepScale +
                                    SECTION_HALF_DIAGONAL &&
                        std::fabs(keepX * s_sunShadowKeepBasis[2][0] +
                            keepY * s_sunShadowKeepBasis[2][1] +
                            keepZ * s_sunShadowKeepBasis[2][2]) <=
                                SUN_SHADOW_EXTENT_Z * shadowKeepScale +
                                    SECTION_HALF_DIAGONAL;

                    if(insideKeepBox)
                    {
                        continue;
                    }
                }

                if(!s_tlasFrustumPlanesValid)
                {
                    continue;
                }

                {
                    constexpr float SECTION_HALF_DIAGONAL = 13.8564f;
                    bool frustumOutside = false;

                    for(int plane = 0; plane < 5; ++plane)
                    {
                        const float planeDist =
                            s_tlasFrustumPlanes[plane][0] * centerX +
                            s_tlasFrustumPlanes[plane][1] * centerY +
                            s_tlasFrustumPlanes[plane][2] * centerZ +
                            s_tlasFrustumPlanes[plane][3];

                        if(planeDist < -SECTION_HALF_DIAGONAL)
                        {
                            frustumOutside = true;
                            break;
                        }
                    }

                    if(!frustumOutside)
                    {
                        continue;
                    }
                }

                VramEvictCandidate candidate;
                candidate.spatialKey = spatialKey;
                candidate.distanceSq = distanceSq;
                candidates.push_back(candidate);
            }
        }

        if(!candidates.empty())
        {
            std::sort(
                candidates.begin(),
                candidates.end(),
                [](const VramEvictCandidate& a, const VramEvictCandidate& b)
                {
                    return a.distanceSq > b.distanceSq;
                }
            );

            const size_t evictQuota =
                std::min(candidates.size(), vramEvictPerPass());

            std::unordered_set<uint64_t> victims;
            victims.reserve(evictQuota);

            for(size_t index = 0u; index < evictQuota; ++index)
            {
                victims.insert(candidates[index].spatialKey);
            }

            for(const PendingSectionUpload& upload : pendingUploads)
            {
                if(upload.section != nullptr)
                {
                    victims.erase(
                        upload.section->sectionKey & SPATIAL_SECTION_KEY_MASK
                    );
                }
            }

            for(const RTSectionAS* queuedSection : sectionsNeedingBuild)
            {
                if(queuedSection != nullptr)
                {
                    victims.erase(
                        queuedSection->sectionKey & SPATIAL_SECTION_KEY_MASK
                    );
                }
            }

            orderedSections.erase(
                std::remove_if(
                    orderedSections.begin(),
                    orderedSections.end(),
                    [&victims](const RTSectionAS* section)
                    {
                        return victims.count(
                            section->sectionKey & SPATIAL_SECTION_KEY_MASK) != 0u;
                    }
                ),
                orderedSections.end()
            );

            for(auto it = g_sectionBLAS.begin(); it != g_sectionBLAS.end();)
            {
                if(victims.count(
                       it->second.sectionKey & SPATIAL_SECTION_KEY_MASK) != 0u)
                {
                    retireSectionAS(it->second);
                    it = g_sectionBLAS.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            for(const uint64_t spatialKey : victims)
            {
                g_nonResidentSections.insert(spatialKey);
            }

            g_vramEvictedSections += victims.size();
        }
    }


    rtxPreCmdAfterEvict = std::chrono::steady_clock::now();

    std::vector<VkAccelerationStructureInstanceKHR>
        instances;


    if(orderedSections.empty())
    {
        VkAccelerationStructureInstanceKHR inactive{};

        inactive.transform.matrix[0][0] = 1.0f;
        inactive.transform.matrix[1][1] = 1.0f;
        inactive.transform.matrix[2][2] = 1.0f;

        inactive.instanceCustomIndex = 0u;
        inactive.mask = 0u;

        inactive.instanceShaderBindingTableRecordOffset =
            0u;

        inactive.flags =
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
            VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;

        inactive.accelerationStructureReference =
            0;


        instances.push_back(
            inactive
        );
    }
    else
    {
        instances.reserve(
            orderedSections.size()
        );

        RTXMarkPhase("geometry:instances");

        const int tlasCullModeValue = tlasCullMode();
        const int cullDistanceLane = g_tlasCullDistance.load();
        const int cullMarginLane = g_tlasFrustumCullMargin.load();
        const bool cullKeepBoxEnabled = tlasKeepBoxEnabled();
        const float cullNearGuardBlocks = tlasNearGuardBlocks();

        const bool cullDistanceActive =
            cullDistanceLane > 0 &&
            (tlasCullModeValue < 0 ||
             tlasCullModeValue == 1 ||
             tlasCullModeValue == 3 ||
             tlasCullModeValue == 4);

        const bool cullViewFrustumActive =
            s_tlasFrustumPlanesValid &&
            (tlasCullModeValue < 0
                ? cullMarginLane > 0
                : (tlasCullModeValue == 2 || tlasCullModeValue == 3));

        const bool cullCasterFrustumActive =
            s_casterPlaneCount > 0 &&
            tlasCullModeValue == 4;

        const bool cullPrimaryMaskActive = tlasCullModeValue == 5;
        static int s_cullPrimaryMaskedCount = 0;

        const float cullSphereRadius =
            13.8564f + static_cast<float>(std::max(cullMarginLane, 0));

        static bool s_tlasCullModeLogged = false;

        if(tlasCullModeValue >= 0 && !s_tlasCullModeLogged)
        {
            s_tlasCullModeLogged = true;

            RTLog(
                std::string("[TLAS CULL] mode=") +
                std::to_string(tlasCullModeValue) +
                " distanceLane=" + std::to_string(cullDistanceLane) +
                " marginLane=" + std::to_string(cullMarginLane) +
                " casterPlanes=" + std::to_string(s_casterPlaneCount) +
                " radius=" + std::to_string(cullSphereRadius) +
                " keepBox=" + std::to_string(cullKeepBoxEnabled ? 1 : 0) +
                " nearGuard=" + std::to_string(
                    static_cast<int>(cullNearGuardBlocks))
            );
        }

        const int visCullMode = visibleSectionCullMode();

        static std::vector<uint64_t> s_visibleSectionKeysLocal;

        static int s_visCullCandidateCount = 0;

        s_visibleSectionKeysLocal.clear();

        if(visCullMode > 0)
        {
            std::lock_guard<std::mutex> visLock(g_visibleSectionKeysMutex);

            s_visibleSectionKeysLocal.assign(
                g_visibleSectionKeys.begin(),
                g_visibleSectionKeys.end()
            );
        }


        for(
            uint32_t instanceIndex = 0;
            instanceIndex <
                static_cast<uint32_t>(
                    orderedSections.size()
                );
            ++instanceIndex
        )
        {
            const RTSectionAS& section =
                *orderedSections[instanceIndex];

            float diagDistance = 0.0f;

            bool primaryMaskCull = false;

            {
                auto signExtend = [](uint64_t value, int bits) -> int {
                    const uint64_t mask = 1ull << (bits - 1);
                    return static_cast<int>((value ^ mask) - mask);
                };

                const uint64_t spatialKey =
                    section.sectionKey & SPATIAL_SECTION_KEY_MASK;

                const float sectionWorldX = static_cast<float>(
                    signExtend(spatialKey & 0x3FFFFFull, 22)) * 16.0f + 8.0f;
                const float sectionWorldZ = static_cast<float>(
                    signExtend((spatialKey >> 22) & 0x3FFFFFull, 22)) * 16.0f + 8.0f;
                const float sectionWorldY = static_cast<float>(
                    signExtend((spatialKey >> 44) & 0xFFFFull, 16)) * 16.0f + 8.0f;

                const CameraUBO* diagUbo =
                    g_cameraUboMapped != nullptr
                        ? static_cast<const CameraUBO*>(g_cameraUboMapped)
                        : nullptr;
                const float diagCenterX = diagUbo ? diagUbo->playerX : 0.0f;
                const float diagCenterZ = diagUbo ? diagUbo->playerZ : 0.0f;
                const float diagCenterY = diagUbo ? diagUbo->playerY : 0.0f;

                const float diagDx = sectionWorldX - diagCenterX;
                const float diagDy = sectionWorldY - diagCenterY;
                const float diagDz = sectionWorldZ - diagCenterZ;
                diagDistance = std::sqrt(
                    diagDx * diagDx + diagDy * diagDy + diagDz * diagDz);

                static int diagBandCount[7] = {};
                static uint64_t diagBandTriangles[7] = {};
                static uint64_t diagTotalTriangles = 0;
                static int diagCulledCount = 0;
                static int diagFrustumCulled = 0;
                static int diagCasterCulled = 0;
                static int diagCullLimit = 0;
                static std::chrono::steady_clock::time_point diagLastPrint{};
                static bool diagLastPrintValid = false;
                diagCullLimit = g_tlasCullDistance.load();

                int diagBand =
                    diagDistance < 32.0f ? 0 :
                    diagDistance < 64.0f ? 1 :
                    diagDistance < 96.0f ? 2 :
                    diagDistance < 128.0f ? 3 :
                    diagDistance < 192.0f ? 4 :
                    diagDistance < 256.0f ? 5 : 6;

                diagBandCount[diagBand] += 1;
                diagBandTriangles[diagBand] += section.triangleCount;
                diagTotalTriangles += section.triangleCount;

                const auto diagNow = std::chrono::steady_clock::now();
                if(!diagLastPrintValid ||
                   std::chrono::duration<float>(diagNow - diagLastPrint).count() > 5.0f)
                {
                    if(diagLastPrintValid)
                    {
                        RTLogFlushed(std::string("[CULL DIAG]") +
                            " total=" + std::to_string(
                                diagBandCount[0] + diagBandCount[1] + diagBandCount[2] +
                                diagBandCount[3] + diagBandCount[4] + diagBandCount[5] +
                                diagBandCount[6]) +
                            " d<32=" + std::to_string(diagBandCount[0]) +
                            " 32-64=" + std::to_string(diagBandCount[1]) +
                            " 64-96=" + std::to_string(diagBandCount[2]) +
                            " 96-128=" + std::to_string(diagBandCount[3]) +
                            " 128-192=" + std::to_string(diagBandCount[4]) +
                            " 192-256=" + std::to_string(diagBandCount[5]) +
                            " >=256=" + std::to_string(diagBandCount[6]) +
                            " tris=" + std::to_string(diagTotalTriangles) +
                            " culled=" + std::to_string(diagCulledCount) +
                            " frustumCulled=" + std::to_string(diagFrustumCulled) +
                            " casterCulled=" + std::to_string(diagCasterCulled) +
                            " primaryMasked=" + std::to_string(s_cullPrimaryMaskedCount) +
                            " visKeys=" + std::to_string(
                                g_visibleSectionKeyCount.load()) +
                            " visCand=" + std::to_string(s_visCullCandidateCount) +
                            " visMode=" + std::to_string(visibleSectionCullMode()) +
                            " cullDist=" + std::to_string(diagCullLimit) +
                            " mode=" + std::to_string(tlasCullModeValue) +
                            " casterPlanes=" + std::to_string(s_casterPlaneCount) +
                            " keepBox=" + std::to_string(cullKeepBoxEnabled ? 1 : 0) +
                            " nearGuard=" + std::to_string(
                                static_cast<int>(cullNearGuardBlocks)) +
                            " planes=" + std::to_string(s_tlasFrustumPlanesValid ? 1 : 0) +
                            " base=" + std::to_string(s_tlasBasePlanesValid ? 1 : 0) +
                            " sun=" + std::to_string(s_sunShadowKeepValid ? 1 : 0));
                    }
                    diagLastPrint = diagNow;
                    diagLastPrintValid = true;
                    std::fill(std::begin(diagBandCount), std::end(diagBandCount), 0);
                    std::fill(std::begin(diagBandTriangles), std::end(diagBandTriangles), 0);
                    diagTotalTriangles = 0;
                    diagCulledCount = 0;
                    diagFrustumCulled = 0;
                    diagCasterCulled = 0;
                    s_cullPrimaryMaskedCount = 0;
                    s_visCullCandidateCount = 0;
                }

                const int32_t diagSectionY =
                    static_cast<int32_t>(
                        (static_cast<uint32_t>(spatialKey >> 44) & 0xFFFFu) << 16) >> 16;
                const bool diagWorldHeightSection =
                    diagSectionY >= -64 && diagSectionY <= 512;

                bool sunShadowKeepsSection = false;
                if(cullKeepBoxEnabled && s_sunShadowKeepValid)
                {
                    const float keepX =
                        sectionWorldX - s_sunShadowKeepCenter[0];
                    const float keepY =
                        sectionWorldY - s_sunShadowKeepCenter[1];
                    const float keepZ =
                        sectionWorldZ - s_sunShadowKeepCenter[2];
                    constexpr float SECTION_HALF_DIAGONAL = 13.8564f;

                    sunShadowKeepsSection =
                        std::fabs(keepX * s_sunShadowKeepBasis[0][0] +
                            keepY * s_sunShadowKeepBasis[0][1] +
                            keepZ * s_sunShadowKeepBasis[0][2]) <=
                                SUN_SHADOW_EXTENT_XY + SECTION_HALF_DIAGONAL &&
                        std::fabs(keepX * s_sunShadowKeepBasis[1][0] +
                            keepY * s_sunShadowKeepBasis[1][1] +
                            keepZ * s_sunShadowKeepBasis[1][2]) <=
                                SUN_SHADOW_EXTENT_XY + SECTION_HALF_DIAGONAL &&
                        std::fabs(keepX * s_sunShadowKeepBasis[2][0] +
                            keepY * s_sunShadowKeepBasis[2][1] +
                            keepZ * s_sunShadowKeepBasis[2][2]) <=
                                SUN_SHADOW_EXTENT_Z + SECTION_HALF_DIAGONAL;
                }

                const bool cullGuardKeepsSection =
                    cullNearGuardBlocks > 0.0f &&
                    diagDistance < cullNearGuardBlocks;

                if(cullPrimaryMaskActive &&
                   diagWorldHeightSection &&
                   !cullGuardKeepsSection &&
                   s_tlasFrustumPlanesValid)
                {
                    for(int plane = 0; plane < 5; ++plane)
                    {
                        const float planeDist =
                            s_tlasFrustumPlanes[plane][0] * sectionWorldX +
                            s_tlasFrustumPlanes[plane][1] * sectionWorldY +
                            s_tlasFrustumPlanes[plane][2] * sectionWorldZ +
                            s_tlasFrustumPlanes[plane][3];

                        if(planeDist < -cullSphereRadius)
                        {
                            primaryMaskCull = true;
                            break;
                        }
                    }
                }

                if(visCullMode > 0 &&
                   diagWorldHeightSection &&
                   !cullGuardKeepsSection)
                {
                    const bool visibleInGraph =
                        std::binary_search(
                            s_visibleSectionKeysLocal.begin(),
                            s_visibleSectionKeysLocal.end(),
                            spatialKey
                        );

                    if(!visibleInGraph)
                    {
                        s_visCullCandidateCount += 1;

                        if(visCullMode >= 2)
                        {
                            primaryMaskCull = true;
                        }
                    }
                }

                if(diagWorldHeightSection && !sunShadowKeepsSection &&
                   !cullGuardKeepsSection &&
                   cullDistanceActive &&
                   diagDistance > static_cast<float>(diagCullLimit))
                {
                    diagCulledCount += 1;
                    continue;
                }

                if(diagWorldHeightSection && !sunShadowKeepsSection &&
                   !cullGuardKeepsSection &&
                   (cullViewFrustumActive || cullCasterFrustumActive))
                {
                    const float (*cullPlanes)[4] =
                        cullCasterFrustumActive
                            ? s_casterPlanes
                            : s_tlasFrustumPlanes;

                    const int cullPlaneCount =
                        cullCasterFrustumActive ? s_casterPlaneCount : 5;

                    bool frustumOutside = false;

                    for(int plane = 0; plane < cullPlaneCount; ++plane)
                    {
                        const float planeDist =
                            cullPlanes[plane][0] * sectionWorldX +
                            cullPlanes[plane][1] * sectionWorldY +
                            cullPlanes[plane][2] * sectionWorldZ +
                            cullPlanes[plane][3];

                        if(planeDist < -cullSphereRadius)
                        {
                            frustumOutside = true;
                            break;
                        }
                    }

                    if(frustumOutside)
                    {
                        if(cullCasterFrustumActive)
                        {
                            diagCasterCulled += 1;
                        }
                        else
                        {
                            diagFrustumCulled += 1;
                        }

                        continue;
                    }
                }
            }


            VkAccelerationStructureInstanceKHR instance{};

            instance.transform.matrix[0][0] = 1.0f;
            instance.transform.matrix[1][1] = 1.0f;
            instance.transform.matrix[2][2] = 1.0f;

            instance.instanceCustomIndex =
                section.vertexAddressSlot;

            instance.mask =
                0xff;

            const uint64_t instanceSpatialKey =
                section.sectionKey &
                SPATIAL_SECTION_KEY_MASK;

            if(g_dhFarFieldMode > 0.5f &&
               isDhLodSpatialKey(instanceSpatialKey))
            {
                instance.mask = 0xfe;
            }

            const uint64_t geometryClass =
                section.sectionKey & ~SPATIAL_SECTION_KEY_MASK;

            const bool lodSpatialKey =
                (section.sectionKey & VOXY_LOD_RESERVED_Y) == VOXY_LOD_RESERVED_Y;
            if(!lodSpatialKey && geometryClass == SECTION_AS_CLASS_ALPHA)
            {
                instance.mask = 0xfb;
            }

            if(geometryClass == SECTION_AS_CLASS_LOCAL_PLAYER)
            {
                instance.mask = 0x02;
            }

            // 2026-11-03：第一人称手部（见 SECTION_AS_CLASS_HAND 的说明）。
            // 0x04 = 只被主相机射线（0x05）命中，所有次级射线（0xfb）都排除它
            // ⇒ 手能看见、但不在世界里投影。
            if(geometryClass == SECTION_AS_CLASS_HAND)
            {
                instance.mask = 0x04;
            }

            if(geometryClass == SECTION_AS_CLASS_PARTICLE)
            {
                instance.mask = 0xf7;
            }

            if(primaryMaskCull)
            {
                instance.mask &= 0xfeu;
                s_cullPrimaryMaskedCount += 1;
            }

            if(geometryClass == SECTION_AS_CLASS_OPAQUE)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_OPAQUE;
            }
            else if(geometryClass == SECTION_AS_CLASS_OMM)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_OMM;
            }
            else if(geometryClass == SECTION_AS_CLASS_WATER)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_WATER;
            }
            else if(geometryClass == SECTION_AS_CLASS_ENTITY)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_ENTITY;
            }
            else if(geometryClass == SECTION_AS_CLASS_LOCAL_PLAYER)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_ENTITY;
            }
            else if(geometryClass == SECTION_AS_CLASS_HAND)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_ENTITY;
            }
            else if(geometryClass == SECTION_AS_CLASS_PARTICLE)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_ENTITY;
            }
            else
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_ALPHA;
            }

            const bool opaqueGeometry =
                geometryClass ==
                    SECTION_AS_CLASS_OPAQUE;

            const bool ommGeometry = section.hasOpacityMicromap;

            instance.flags =
                (geometryClass == SECTION_AS_CLASS_OMM
                    ? 0u
                    : VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR) |
                (
                    (opaqueGeometry || ommGeometry)
                        ? 0u
                        : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR
                );

            instance.accelerationStructureReference =
                section.blasAddress;


            instances.push_back(
                instance
            );
        }
    }

    if(instances.empty())
    {
        VkAccelerationStructureInstanceKHR inactive{};
        inactive.transform.matrix[0][0] = 1.0f;
        inactive.transform.matrix[1][1] = 1.0f;
        inactive.transform.matrix[2][2] = 1.0f;
        inactive.instanceCustomIndex = 0u;
        inactive.mask = 0u;
        inactive.instanceShaderBindingTableRecordOffset = 0u;
        inactive.flags =
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
            VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
        inactive.accelerationStructureReference = 0;
        instances.push_back(inactive);
        RTLogFlushed("[TLAS CULL] all instances culled; inactive placeholder in use");
    }

    rtxPreCmdAfterInstances = std::chrono::steady_clock::now();

    const VkAccelerationStructureKHR tlasHandleAtEntry = g_tlas;
    static std::vector<VkAccelerationStructureInstanceKHR> s_tlasInstancesShadow;

    // 2026-10-17：把实例列表排成**规范顺序**，让下面的 memcmp 变成"集合比较"。
    //
    // 依据（`ORION_TLAS_DELTA=1` 实测 1881 条差量）：63% 的"变化"是**尺寸不变、内容变了**，
    // posDiff 中位 3 但 p90=1025、最大 1518 —— 即同一批实例被重排。memcmp 对顺序敏感，
    // 于是每帧都判"变了" ⇒ TLAS 每帧重建 ⇒ 阴影/间接光反复微调，表现为"光源时不时闪一下"。
    // 排序后：集合不变 ⇒ tlasInstancesUnchanged=true ⇒ 跳过重建（既治闪，也省下每帧 AS 重建）。
    // 排序键按"每个实例自身携带的字段"，所以 SBT offset/mask 仍跟着各自实例，映射关系不变。
    std::sort(
        instances.begin(),
        instances.end(),
        [](const VkAccelerationStructureInstanceKHR& a,
           const VkAccelerationStructureInstanceKHR& b)
        {
            if(a.accelerationStructureReference != b.accelerationStructureReference)
            {
                return a.accelerationStructureReference < b.accelerationStructureReference;
            }

            if(a.instanceCustomIndex != b.instanceCustomIndex)
            {
                return a.instanceCustomIndex < b.instanceCustomIndex;
            }

            if(a.mask != b.mask)
            {
                return a.mask < b.mask;
            }

            return a.instanceShaderBindingTableRecordOffset <
                   b.instanceShaderBindingTableRecordOffset;
        }
    );

    bool tlasInstancesUnchanged =
        tlasHandleAtEntry != VK_NULL_HANDLE &&
        s_tlasInstancesShadow.size() == instances.size() &&
        (instances.empty() ||
         std::memcmp(
             s_tlasInstancesShadow.data(),
             instances.data(),
             instances.size() * sizeof(VkAccelerationStructureInstanceKHR)
         ) == 0);
    bool tlasInstancesBufferRecreated = false;
    bool tlasStorageRecreated = false;


    auto destroyAS =
        getDestroyAccelerationStructure();

    auto createAS =
        getCreateAccelerationStructure();

    auto getBuildSizes =
        getGetAccelerationStructureBuildSizes();

    auto getASAddress =
        getGetAccelerationStructureDeviceAddress();

    auto cmdBuild =
        getCmdBuildAccelerationStructures();


    if(
        !destroyAS ||
        !createAS ||
        !getBuildSizes ||
        !getASAddress ||
        !cmdBuild
    )
    {
        throw std::runtime_error(
            "multi-Section TLAS Vulkan functions unavailable"
        );
    }

    constexpr bool updateTlas = false;



    constexpr VkDeviceSize TLAS_INSTANCE_STRIDE =
        sizeof(VkAccelerationStructureInstanceKHR);

    const size_t requiredInstanceCount =
        instances.size();

    const size_t previousCapacityInstances =
        static_cast<size_t>(
            g_tlasInstancesCapacity / TLAS_INSTANCE_STRIDE);

    size_t capacityInstances =
        previousCapacityInstances;

    if(requiredInstanceCount > previousCapacityInstances)
    {
        constexpr size_t TLAS_CAPACITY_SLACK_MIN = 4096u;

        size_t targetCapacity =
            requiredInstanceCount +
            requiredInstanceCount / 2u +
            TLAS_CAPACITY_SLACK_MIN;

        const size_t doublePreviousCapacity =
            previousCapacityInstances >
                std::numeric_limits<size_t>::max() / 2u
                ? std::numeric_limits<size_t>::max()
                : previousCapacityInstances * 2u;

        if(doublePreviousCapacity > targetCapacity)
        {
            targetCapacity = doublePreviousCapacity;
        }

        capacityInstances =
            (targetCapacity +
             63u) &
            ~static_cast<size_t>(63u);
    }

    const VkDeviceSize instanceBytes =
        static_cast<VkDeviceSize>(
            capacityInstances
        ) *
        TLAS_INSTANCE_STRIDE;


    if(
        g_tlasInstancesBuffer == VK_NULL_HANDLE ||
        g_tlasInstancesCapacity < instanceBytes
    )
    {
        if(g_tlasInstancesBuffer != VK_NULL_HANDLE)
        {
            parkTlasInstances();
        }

        createBuffer(
            g_device,
            g_physicalDevice,
            instanceBytes,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            g_tlasInstancesBuffer,
            g_tlasInstancesMemory
        );

        g_tlasInstancesCapacity =
            instanceBytes;

        g_tlasInstancesAddress =
            getBufferDeviceAddress(
                g_device,
                g_tlasInstancesBuffer
            );
    }




    VkAccelerationStructureGeometryInstancesDataKHR
        instanceData{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR
        };

    instanceData.arrayOfPointers =
        VK_FALSE;

    instanceData.data.deviceAddress =
        g_tlasInstancesAddress;


    VkAccelerationStructureGeometryKHR
        tlasGeometry{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR
        };

    tlasGeometry.geometryType =
        VK_GEOMETRY_TYPE_INSTANCES_KHR;

    tlasGeometry.geometry.instances =
        instanceData;


    VkAccelerationStructureBuildGeometryInfoKHR
        tlasBuildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
        };

    tlasBuildInfo.type =
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

    tlasBuildInfo.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

    tlasBuildInfo.mode =
        VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

    tlasBuildInfo.geometryCount = 1;
    tlasBuildInfo.pGeometries =
        &tlasGeometry;


    const uint32_t tlasPrimitiveCount =
        static_cast<uint32_t>(
            instances.size()
        );


    const uint32_t tlasCapacityPrimitiveCount =
        static_cast<uint32_t>(
            capacityInstances
        );


    VkAccelerationStructureBuildSizesInfoKHR
        tlasSizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
        };


    getBuildSizes(
        g_device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &tlasBuildInfo,
        &tlasCapacityPrimitiveCount,
        &tlasSizes
    );


    if(
        g_tlas == VK_NULL_HANDLE ||
        g_tlasBuffer == VK_NULL_HANDLE ||
        g_tlasAsSize < tlasSizes.accelerationStructureSize
    )
    {
        if(
            g_tlas != VK_NULL_HANDLE ||
            g_tlasBuffer != VK_NULL_HANDLE
        )
        {
            parkTlasStorage();
        }

        createBuffer(
            g_device,
            g_physicalDevice,
            tlasSizes.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            g_tlasBuffer,
            g_tlasMemory
        );


    VkAccelerationStructureCreateInfoKHR
        tlasCreateInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR
        };

    tlasCreateInfo.buffer =
        g_tlasBuffer;

    tlasCreateInfo.size =
        tlasSizes.accelerationStructureSize;

    tlasCreateInfo.type =
        VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;


        check(
            createAS(
                g_device,
                &tlasCreateInfo,
                nullptr,
                &g_tlas
            ),
            "create multi-Section TLAS failed"
        );

        g_tlasAsSize =
            tlasSizes.accelerationStructureSize;

        tlasStorageRecreated = true;
    }


    if(
        tlasSizes.buildScratchSize > 0 &&
        (
            g_tlasScratchBuffer == VK_NULL_HANDLE ||
            g_tlasScratchCapacity < tlasSizes.buildScratchSize
        )
    )
    {
        if(g_tlasScratchBuffer != VK_NULL_HANDLE)
        {
            parkTlasScratch();
        }

        g_tlasScratchCapacity =
            tlasSizes.buildScratchSize;

        createBuffer(
            g_device,
            g_physicalDevice,
            g_tlasScratchCapacity,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            g_tlasScratchBuffer,
            g_tlasScratchMemory
        );


        g_tlasScratchAddress =
            getBufferDeviceAddress(
                g_device,
                g_tlasScratchBuffer
            );
    }


    VkBuffer blasScratchBuffer =
        VK_NULL_HANDLE;

    VkDeviceMemory blasScratchMemory =
        VK_NULL_HANDLE;

    VkDeviceAddress blasScratchAddress =
        0;


    if(
        !sectionsNeedingBuild.empty() &&
        maxBLASScratchSize > 0
    )
    {
        RtxSectionBuildBufferSlot blasScratchSlot;

        if(g_blasScratchPool.tryAcquire(maxBLASScratchSize, blasScratchSlot))
        {
            blasScratchBuffer = blasScratchSlot.buffer;
            blasScratchMemory = blasScratchSlot.memory;

            g_sectionBuildPoolReuseCount += 1u;
        }
        else
        {

            const VkDeviceSize blasScratchSlotBytes =
                sectionBuildPoolSlotBytes(maxBLASScratchSize);

            createBuffer(
                g_device,
                g_physicalDevice,
                blasScratchSlotBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                blasScratchBuffer,
                blasScratchMemory
            );

            g_blasScratchPool.adopt(
                blasScratchBuffer,
                blasScratchMemory,
                blasScratchSlotBytes
            );

            g_sectionBuildPoolAllocCount += 1u;
        }


        blasScratchAddress =
            getBufferDeviceAddress(
                g_device,
                blasScratchBuffer
            );
    }


    rtxPreCmdAfterTlas = std::chrono::steady_clock::now();

    VkCommandBufferAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };

    allocateInfo.commandPool =
        g_rtCommandPool;

    allocateInfo.level =
        VK_COMMAND_BUFFER_LEVEL_PRIMARY;

    allocateInfo.commandBufferCount =
        1;


    VkCommandBuffer cmd =
        VK_NULL_HANDLE;

    rtxTailBeforeCommandRecord = std::chrono::steady_clock::now();

    check(
        vkAllocateCommandBuffers(
            g_device,
            &allocateInfo,
            &cmd
        ),
        "allocate Section AS command buffer failed"
    );

    if(tlasStorageRecreated || tlasInstancesBufferRecreated)
    {
        tlasInstancesUnchanged = false;
    }


    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };

    beginInfo.flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;


    check(
        vkBeginCommandBuffer(
            cmd,
            &beginInfo
        ),
        "begin Section AS command buffer failed"
    );


    if(!pendingUploads.empty())
    {
        for(const PendingSectionUpload& upload : pendingUploads)
        {
            const auto liveEntry =
                (upload.section != nullptr)
                    ? g_sectionBLAS.find(upload.section->sectionKey)
                    : g_sectionBLAS.end();

            const bool staleEntry =
                upload.section == nullptr ||
                liveEntry == g_sectionBLAS.end() ||
                &liveEntry->second != upload.section;

            if(
                vertexStagingBuffer == VK_NULL_HANDLE ||
                upload.section == nullptr ||
                upload.section->vertexBuffer == VK_NULL_HANDLE ||
                upload.section->vertexBytes == 0u ||
                staleEntry
            )
            {
                static uint64_t guardSkips = 0u;

                ++guardSkips;

                if(
                    guardSkips <= 5u ||
                    (guardSkips % 100u) == 0u
                )
                {
                    std::stringstream guardLog;
                    guardLog
                        << "[SECTION UPLOAD GUARD] skip#" << guardSkips
                        << " stagingNull="
                        << (vertexStagingBuffer == VK_NULL_HANDLE ? 1 : 0)
                        << " sectionNull=" << (upload.section == nullptr ? 1 : 0)
                        << " vbNull="
                        << ((upload.section == nullptr ||
                             upload.section->vertexBuffer == VK_NULL_HANDLE) ? 1 : 0)
                        << " key="
                        << (upload.section != nullptr ? upload.section->sectionKey : 0ull)
                        << " vertexBytes="
                        << (upload.section != nullptr
                                ? static_cast<unsigned long long>(
                                      upload.section->vertexBytes)
                                : 0ull)
                        << " stale=" << (staleEntry ? 1 : 0)
                        << " stagingOffset="
                        << static_cast<unsigned long long>(upload.stagingOffset);

                    RTLog(guardLog.str());
                }

                continue;
            }

            VkBufferCopy copy{};

            copy.srcOffset =
                upload.stagingOffset;

            copy.dstOffset = 0;

            copy.size =
                upload.section->vertexBytes;


            vkCmdCopyBuffer(
                cmd,
                vertexStagingBuffer,
                upload.section->vertexBuffer,
                1,
                &copy
            );
        }


        VkMemoryBarrier uploadBarrier{
            VK_STRUCTURE_TYPE_MEMORY_BARRIER
        };

        uploadBarrier.srcAccessMask =
            VK_ACCESS_TRANSFER_WRITE_BIT;

        uploadBarrier.dstAccessMask =
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_SHADER_READ_BIT;


        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0,
            1,
            &uploadBarrier,
            0,
            nullptr,
            0,
            nullptr
        );
    }


    VkMemoryBarrier asBuildBarrier{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER
    };

    asBuildBarrier.srcAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

    asBuildBarrier.dstAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;


    for(RTSectionAS* section : sectionsNeedingBuild)
    {
        if(section == nullptr)
        {
            continue;
        }

        {
            const auto liveEntry = g_sectionBLAS.find(section->sectionKey);

            if(liveEntry == g_sectionBLAS.end() ||
               &liveEntry->second != section)
            {
                static uint64_t blasStaleSkips = 0u;

                ++blasStaleSkips;

                if(blasStaleSkips <= 5u || (blasStaleSkips % 100u) == 0u)
                {
                    std::stringstream staleLog;
                    staleLog
                        << "[SECTION BLAS GUARD] skip#" << blasStaleSkips
                        << " key=" << section->sectionKey
                        << " (entry erased while queued)";
                    RTLog(staleLog.str());
                }

                continue;
            }
        }

        recordSectionBLASBuild(
            cmd,
            *section,
            blasScratchAddress
        );


        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            0,
            1,
            &asBuildBarrier,
            0,
            nullptr,
            0,
            nullptr
        );
    }


    tlasBuildInfo.dstAccelerationStructure =
        g_tlas;

    tlasBuildInfo.mode =
        updateTlas
            ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
            : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

    tlasBuildInfo.srcAccelerationStructure =
        updateTlas
            ? g_tlas
            : VK_NULL_HANDLE;

    tlasBuildInfo.scratchData.deviceAddress =
        g_tlasScratchAddress;


    VkAccelerationStructureBuildRangeInfoKHR
        tlasRange{};

    tlasRange.primitiveCount =
        tlasPrimitiveCount;


    const VkAccelerationStructureBuildRangeInfoKHR*
        tlasRanges[] =
        {
            &tlasRange
        };


    RTXMarkPhase("tlas:build");

    g_rtxDetailInstances.store(
        static_cast<uint64_t>(instances.size()),
        std::memory_order_relaxed
    );

    if(!tlasInstancesUnchanged)
    {
        cmdBuild(
            cmd,
            1,
            &tlasBuildInfo,
            tlasRanges
        );
    }

    RTXMarkPhase("tlas:built");


    check(
        vkEndCommandBuffer(cmd),
        "end Section AS command buffer failed"
    );

    rtxTailAfterCommandRecord = std::chrono::steady_clock::now();


    VkSubmitInfo submitInfo{
        VK_STRUCTURE_TYPE_SUBMIT_INFO
    };

    submitInfo.commandBufferCount =
        1;

    submitInfo.pCommandBuffers =
        &cmd;


    const bool rebuildPublishesTlasDescriptor =
        g_tlasDescriptorHandle != g_tlas;

    const auto perfL10QueueIdleStart =
        std::chrono::steady_clock::now();

    if(rebuildPublishesTlasDescriptor)
    {
        PERF_L10_TIME_STALL("worldGeometryQueueIdle", check(
            vkQueueWaitIdle(g_queue),
            "wait RT queue before Section AS rebuild failed"
        ));

    }

    perfL10QueueIdleMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() -
            perfL10QueueIdleStart
        ).count();


    if(
        g_sectionRetirementFencesReady &&
        g_sectionRetirementRing[previousRetirementSlot].fencePending
    )
    {
        VkFence previousBuildFence =
            g_sectionRetirementFences[previousRetirementSlot];

        VkResult previousBuildStatus =
            VK_NOT_READY;

        PERF_L10_TIME_STALL(
            "sectionBuildFenceWait",
            previousBuildStatus = vkGetFenceStatus(
                g_device,
                previousBuildFence
            )
        );

        if(previousBuildStatus == VK_NOT_READY)
        {
            static bool s_tailFenceWaitFallbackLogged = false;

            if(!s_tailFenceWaitFallbackLogged)
            {
                s_tailFenceWaitFallbackLogged = true;

                RTLog(
                    "[SECTION BUILD FENCE] tail poll found the previous batch "
                    "still in flight (entry gate bypassed?)"
                );
            }

            previousBuildStatus = vkWaitForFences(
                g_device,
                1,
                &previousBuildFence,
                VK_TRUE,
                static_cast<uint64_t>(
                    SECTION_REBUILD_FENCE_WAIT_MS > 0u
                        ? SECTION_REBUILD_FENCE_WAIT_MS
                        : 2u
                ) * 1000000ull
            );

            if(previousBuildStatus == VK_TIMEOUT)
            {
                previousBuildStatus = vkWaitForFences(
                    g_device,
                    1,
                    &previousBuildFence,
                    VK_TRUE,
                    std::numeric_limits<uint64_t>::max()
                );
            }
        }

        check(
            previousBuildStatus,
            "wait previous Section build fence failed"
        );

        reapSignaledRetirements();
    }

    rtxTailAfterFenceWait = std::chrono::steady_clock::now();

    if(!tlasInstancesUnchanged)
    {
        void* mappedInstances = nullptr;

        check(
            vkMapMemory(
                g_device,
                g_tlasInstancesMemory,
                0,
                instanceBytes,
                0,
                &mappedInstances
            ),
            "map multi-Section TLAS instance buffer failed"
        );


        std::memcpy(
            mappedInstances,
            instances.data(),
            static_cast<size_t>(tlasPrimitiveCount) * TLAS_INSTANCE_STRIDE
        );


        vkUnmapMemory(
            g_device,
            g_tlasInstancesMemory
        );
    }


    VkFence buildFence =
        VK_NULL_HANDLE;

    if(g_sectionRetirementFencesReady)
    {
        buildFence =
            g_sectionRetirementFences[g_currentRetirementSlot];

        check(
            vkResetFences(
                g_device,
                1,
                &buildFence
            ),
            "reset Section retirement fence failed"
        );
    }

    check(
        vkQueueSubmit(
            g_queue,
            1,
            &submitInfo,
            buildFence
        ),
        "submit Section BLAS/TLAS build failed"
    );

    rtxTailAfterSubmit = std::chrono::steady_clock::now();

    if(!tlasInstancesUnchanged)
    {
        if(std::getenv("ORION_TLAS_DELTA") != nullptr)
        {
            const size_t prevSize = s_tlasInstancesShadow.size();
            const size_t nowSize = instances.size();
            const size_t shorter = prevSize < nowSize ? prevSize : nowSize;
            size_t posDiff = 0u;

            for(size_t i = 0u; i < shorter; ++i)
            {
                if(s_tlasInstancesShadow[i].accelerationStructureReference !=
                   instances[i].accelerationStructureReference)
                {
                    ++posDiff;
                }
            }

            uint64_t hashPrev = 1469598103934665603ull;
            uint64_t hashNow = 1469598103934665603ull;

            for(size_t i = 0u; i < prevSize; ++i)
            {
                hashPrev =
                    (hashPrev ^ s_tlasInstancesShadow[i].accelerationStructureReference) *
                    1099511628211ull;
            }

            for(size_t i = 0u; i < nowSize; ++i)
            {
                hashNow =
                    (hashNow ^ instances[i].accelerationStructureReference) *
                    1099511628211ull;
            }

            std::fprintf(
                stderr,
                "[TLAS DELTA] prev=%zu now=%zu posDiff=%zu hashPrev=%016llx hashNow=%016llx\n",
                prevSize,
                nowSize,
                posDiff,
                static_cast<unsigned long long>(hashPrev),
                static_cast<unsigned long long>(hashNow)
            );

            std::fflush(stderr);
        }

        s_tlasInstancesShadow = instances;
    }

    {
        static std::chrono::steady_clock::time_point s_tlasSkipLastPrint{};
        static bool s_tlasSkipLastValid = false;

        const auto skipNow = std::chrono::steady_clock::now();
        if(!s_tlasSkipLastValid ||
           std::chrono::duration<float>(skipNow - s_tlasSkipLastPrint).count() > 5.0f)
        {
            RTLogFlushed(std::string("[TLAS SKIP]") +
                (tlasInstancesUnchanged ? " skipped=true" : " skipped=false") +
                " instances=" + std::to_string(instances.size()));
            s_tlasSkipLastPrint = skipNow;
            s_tlasSkipLastValid = true;
        }
    }

    if(g_sectionRetirementFencesReady)
    {
        g_sectionRetirement->fencePending =
            true;
    }


    g_tlasInstanceCount =
        instances.size();

    for(RTSectionAS* section : sectionsNeedingBuild)
    {
        deferSectionOpacityMicromapBuildInputs(*section);
    }


    g_deferredSectionBuild.commandBuffer =
        cmd;

    g_deferredSectionBuild.blasScratchBuffer =
        blasScratchBuffer;

    g_deferredSectionBuild.blasScratchMemory =
        blasScratchMemory;

    g_deferredSectionBuild.blasScratchSize =
        maxBLASScratchSize;

    g_deferredSectionBuild.vertexStagingBuffer =
        vertexStagingBuffer;

    g_deferredSectionBuild.vertexStagingMemory =
        vertexStagingMemory;

    g_deferredSectionBuild.vertexStagingSize =
        totalStagingBytes;

    cmd = VK_NULL_HANDLE;
    blasScratchBuffer = VK_NULL_HANDLE;
    blasScratchMemory = VK_NULL_HANDLE;
    vertexStagingBuffer = VK_NULL_HANDLE;
    vertexStagingMemory = VK_NULL_HANDLE;


    VkAccelerationStructureDeviceAddressInfoKHR
        tlasAddressInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
        };

    tlasAddressInfo.accelerationStructure =
        g_tlas;


    g_tlasDeviceAddress =
        getASAddress(
            g_device,
            &tlasAddressInfo
        );




    VkWriteDescriptorSetAccelerationStructureKHR
        tlasDescriptorInfo{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };

    tlasDescriptorInfo.accelerationStructureCount =
        1;

    tlasDescriptorInfo.pAccelerationStructures =
        &g_tlas;


    VkWriteDescriptorSet tlasWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
    };

    tlasWrite.pNext =
        &tlasDescriptorInfo;

    tlasWrite.dstSet =
        g_rayTracingDescriptorSet;

    tlasWrite.dstBinding = 0;
    tlasWrite.descriptorCount = 1;

    tlasWrite.descriptorType =
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;


    if(g_tlasDescriptorHandle != g_tlas)
    {
        vkUpdateDescriptorSets(
            g_device,
            1,
            &tlasWrite,
            0,
            nullptr
        );

        if(g_sunShadowSet != VK_NULL_HANDLE)
        {
            VkWriteDescriptorSet sunShadowTlasWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
            };

            sunShadowTlasWrite.pNext =
                &tlasDescriptorInfo;

            sunShadowTlasWrite.dstSet =
                g_sunShadowSet;

            sunShadowTlasWrite.dstBinding = 1u;
            sunShadowTlasWrite.descriptorCount = 1u;

            sunShadowTlasWrite.descriptorType =
                VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

            vkUpdateDescriptorSets(
                g_device,
                1,
                &sunShadowTlasWrite,
                0,
                nullptr
            );

            g_sunShadowTlasHandle =
                g_tlas;
        }

        if(g_terrainHeightSet != VK_NULL_HANDLE)
        {
            VkWriteDescriptorSet terrainHeightTlasWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
            };

            terrainHeightTlasWrite.pNext =
                &tlasDescriptorInfo;

            terrainHeightTlasWrite.dstSet =
                g_terrainHeightSet;

            terrainHeightTlasWrite.dstBinding = 1u;
            terrainHeightTlasWrite.descriptorCount = 1u;

            terrainHeightTlasWrite.descriptorType =
                VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

            vkUpdateDescriptorSets(
                g_device,
                1,
                &terrainHeightTlasWrite,
                0,
                nullptr
            );

            g_terrainHeightTlasHandle =
                g_tlas;
        }

        if(g_sunShadowFarSet != VK_NULL_HANDLE)
        {
            VkWriteDescriptorSet sunShadowFarTlasWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
            };

            sunShadowFarTlasWrite.pNext =
                &tlasDescriptorInfo;

            sunShadowFarTlasWrite.dstSet =
                g_sunShadowFarSet;

            sunShadowFarTlasWrite.dstBinding = 1u;
            sunShadowFarTlasWrite.descriptorCount = 1u;

            sunShadowFarTlasWrite.descriptorType =
                VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

            vkUpdateDescriptorSets(
                g_device,
                1,
                &sunShadowFarTlasWrite,
                0,
                nullptr
            );

            g_sunShadowFarTlasHandle =
                g_tlas;
        }

        g_tlasDescriptorHandle =
            g_tlas;
    }


    g_asBuilt =
        true;


    uint64_t totalTriangleCount = 0;


    for(const RTSectionAS* section : orderedSections)
    {
        totalTriangleCount +=
            section->triangleCount;
    }


    if constexpr(PERF_L10_ENABLE_SECTION_REBUILD_LOGS)
    {
        std::stringstream log;

        log
            << "[RT SECTION AS]"
            << " rebuilt=" << rebuiltSectionCount
            << " removed=" << removedSectionCount
            << " instances=" << orderedSections.size()
            << " triangles=" << totalTriangleCount
            << " rebuiltOpaqueTriangles=" << rebuiltOpaqueTriangleCount
            << " rebuiltOmmTriangles=" << rebuiltOmmTriangleCount
            << " rebuiltAlphaTriangles=" << rebuiltAlphaTriangleCount
            << " rebuiltWaterTriangles=" << rebuiltWaterTriangleCount
            << " rebuiltParticleTriangles=" << rebuiltParticleTriangleCount
            << " queued=1"
            << " tlasAddr=0x"
            << std::hex
            << static_cast<unsigned long long>(
                g_tlasDeviceAddress
            );

        RTLog(
            log.str()
        );

        const double perfL10TotalMilliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() -
                perfL10RebuildStart
            ).count();

        std::stringstream perfL10Log;
        perfL10Log
            << "[PERF L10 SECTION REBUILD]"
            << " frame=" << g_perfL10FrameSerial
            << " totalCpuMs=" << std::fixed << std::setprecision(3)
            << perfL10TotalMilliseconds
            << " queueIdleMs=" << perfL10QueueIdleMilliseconds
            << " rebuilt=" << rebuiltSectionCount
            << " removed=" << removedSectionCount
            << " instances=" << orderedSections.size()
            << " triangles=" << totalTriangleCount
            << " rebuiltOpaqueTriangles=" << rebuiltOpaqueTriangleCount
            << " rebuiltOmmTriangles=" << rebuiltOmmTriangleCount
            << " rebuiltAlphaTriangles=" << rebuiltAlphaTriangleCount
            << " rebuiltWaterTriangles=" << rebuiltWaterTriangleCount
            << " rebuiltParticleTriangles=" << rebuiltParticleTriangleCount;

        RTLog(
            perfL10Log.str()
        );
    }

    {
        const double tailMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() -
                perfL10AfterBuildLoop
            ).count();

        if(tailMs > g_geometryMaxTailMs)
        {
            g_geometryMaxTailMs = tailMs;
        }

        const double rtxRebuildTotalMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() -
                perfL10RebuildStart
            ).count();

        if(rtxRebuildTotalMs >= 15.0)
        {
            const auto rtxSegMs =
                [](const std::chrono::steady_clock::time_point& from,
                   const std::chrono::steady_clock::time_point& to)
                {
                    return std::chrono::duration<double, std::milli>(
                        to - from
                    ).count();
                };

            RTLog(
                "[RT GEOMETRY SLOW] frame=" +
                std::to_string(g_perfL10FrameSerial) +
                " total=" + std::to_string(rtxRebuildTotalMs) +
                " sections=" + std::to_string(sectionUpdates.size()) +
                " pre=" + std::to_string(
                    rtxSegMs(perfL10RebuildStart, perfL10BuildLoopStart)) +
                " pCap=" + std::to_string(
                    rtxSegMs(perfL10RebuildStart, rtxPreLoopMarkLog)) +
                " pLog=" + std::to_string(
                    rtxSegMs(rtxPreLoopMarkLog, rtxPreLoopMarkMid)) +
                " pRsv=" + std::to_string(
                    rtxSegMs(rtxPreLoopMarkMid, perfL10BuildLoopStart)) +
                " bld=" + std::to_string(
                    rtxSegMs(perfL10BuildLoopStart, perfL10AfterBuildLoop)) +
                " tail=" + std::to_string(tailMs) +
                " staging=" + std::to_string(
                    rtxSegMs(perfL10AfterBuildLoop, rtxTailAfterStaging)) +
                " stagingMB=" + std::to_string(
                    static_cast<double>(totalStagingBytes) / 1048576.0) +
                " preCmd=" + std::to_string(
                    rtxSegMs(rtxTailAfterStaging, rtxTailBeforeCommandRecord)) +
                " ord=" + std::to_string(
                    rtxSegMs(rtxTailAfterStaging, rtxPreCmdAfterOrdered)) +
                " evict=" + std::to_string(
                    rtxSegMs(rtxPreCmdAfterOrdered, rtxPreCmdAfterEvict)) +
                " inst=" + std::to_string(
                    rtxSegMs(rtxPreCmdAfterEvict, rtxPreCmdAfterInstances)) +
                " tlas=" + std::to_string(
                    rtxSegMs(rtxPreCmdAfterInstances, rtxPreCmdAfterTlas)) +
                " preMisc=" + std::to_string(
                    rtxSegMs(rtxPreCmdAfterTlas, rtxTailBeforeCommandRecord)) +
                " record=" + std::to_string(
                    rtxSegMs(rtxTailBeforeCommandRecord, rtxTailAfterCommandRecord)) +
                " waits=" + std::to_string(
                    rtxSegMs(rtxTailAfterCommandRecord, rtxTailAfterFenceWait)) +
                " submit=" + std::to_string(
                    rtxSegMs(rtxTailAfterFenceWait, rtxTailAfterSubmit)) +
                " park=" + std::to_string(
                    rtxSegMs(rtxTailAfterSubmit, std::chrono::steady_clock::now())) +
                " qIdle=" + std::to_string(perfL10QueueIdleMilliseconds));
        }
    }

    return true;
}
