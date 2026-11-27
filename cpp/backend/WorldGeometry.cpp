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

    // 2026-11-07i：这里持有的是 arena 视图，split 已在捕获点还池。
    // 因此不再需要"函数退出时归还 split"的 RAII。
    std::vector<
        std::pair<
            uint64_t,
            CaptureArenaSegment
        >
    > sectionUpdates;



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
                CaptureArenaSegment
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

                // 2026-11-07i：临时 split 填完 → 打包 → commit 进 arena → 还池。
                SectionGeometrySplit split = takeSectionSplitFromPool();

                geometryCaptureSectionLocked(spatialSectionKey, split);
                packSectionSplitBucketsLocked(spatialSectionKey, split);

                mergedSections[spatialSectionKey] = captureArenaCommit(split);
                returnSectionSplitToPool(std::move(split));
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

            if(entityDirty)
            {
                // 2026-11-07g：这里不 take，推迟到真正捕获前（见下方捕获循环）。
                // 否则"选 key"阶段就把池掏空，捕获线程唤醒即 miss。
                sectionUpdates.emplace_back(
                    ENTITY_RT_SPATIAL_KEY,
                    CaptureArenaSegment{}
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
                    capturedBatch[adopted].segment
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
                    // 2026-11-07g：同上，take 推迟到捕获前。
                    sectionUpdates.emplace_back(
                        nearestEntry.second,
                        CaptureArenaSegment{});
                    updateIndices.emplace(nearestEntry.second, updateIndex);
                }
            }


            const bool onlyDynamicSection =
                sectionUpdates.size() == 1u &&
                sectionUpdates.front().first == ENTITY_RT_SPATIAL_KEY;
            if(onlyDynamicSection)
            {
                // 2026-11-07i：临时 split → 捕获 → commit 进 arena → 立刻还池。
                SectionGeometrySplit split = takeSectionSplitFromPool();

                geometryCaptureSectionLocked(ENTITY_RT_SPATIAL_KEY, split);

                sectionUpdates.front().second = captureArenaCommit(split);
                returnSectionSplitToPool(std::move(split));
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
                        if(captureElapsedMs >= std::min(sectionRebuildTimeBudgetMs(), SECTION_CAPTURE_LOCK_BUDGET_MS))
                        {
                            break;
                        }
                    }

                    const size_t captureIndex = captureOrder[orderIndex];

                    // 2026-11-07i：split 只在本次捕获内活着。填完立刻搬进 arena
                    // 并还池 —— 池的稳态大小从此不再等于"在途段数"。
                    SectionGeometrySplit split = takeSectionSplitFromPool();

                    geometryCaptureSectionLocked(
                        sectionUpdates[captureIndex].first,
                        split
                    );

                    sectionUpdates[captureIndex].second = captureArenaCommit(split);
                    returnSectionSplitToPool(std::move(split));
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
            << (g_captureLockMaxWaitNanos.load(std::memory_order_relaxed) / 1.0e6)
            // 2026-11-07g：split 池命中与捕获扩容分桶（窗口增量口径）。
            << " spHit=" << g_sectionSplitPoolTakeHits
            << " spMiss=" << g_sectionSplitPoolTakeMisses
            << " spRet=" << g_sectionSplitPoolReturns
            << " spRej=" << g_sectionSplitPoolReturnRejects
            << " spDrop=" << g_sectionSplitPoolReturnDrops
            << " spStarve=" << g_sectionSplitPoolStarveSkips
            << " spSize=" << g_sectionSplitPoolSize
            << " growBk="
            << g_captureGrowBucketCount[0] << "/"
            << g_captureGrowBucketCount[1] << "/"
            << g_captureGrowBucketCount[2] << "/"
            << g_captureGrowBucketCount[3] << "/"
            << g_captureGrowBucketCount[4] << "/"
            << g_captureGrowBucketCount[5] << "/"
            << g_captureGrowBucketCount[6] << "/"
            << g_captureGrowBucketCount[7]
            << " growEmpty=" << g_captureGrowFromEmpty
            << " growSmall=" << g_captureGrowFromSmall
            << " arenaN=" << g_captureArenaSegments
            << " arenaMB="
            << (static_cast<double>(g_captureArenaBytes) / 1048576.0)
            << " arenaRst=" << g_captureArenaResets
            << " arenaSkip=" << g_captureArenaSkipResets
            << " arenaChunks=" << g_captureArenaChunks
            << " arenaStale=" << g_captureArenaStaleSegments;

        {
            std::lock_guard<std::mutex> capturedLock(g_capturedSectionsMutex);
            countersLog << " queued="
            << (g_capturedSections.size() - g_capturedSectionsHead)
            << " lockSkips=" << g_geometryPreLoopLockSkips
            << " lockWaitMs=" << g_geometryMaxPreLoopLockWaitMs
            << " logMs=" << g_geometryMaxCountersLogMs;
        }

        RTLog(countersLog.str());

        // 2026-11-15：**自愈扫描**（默认开，ORION_SECTION_SELFHEAL=0 关）。
        // 动机：用户报告"每局一个固定一区块大小、永不恢复的洞，位置每局不同"。
        // 根因类别是竞态（槽位/退休环/arena），确定性的对症修法需要多轮定位；
        // 但"重建一次该段"对所有这些坏状态都有效（重建会重发槽位 + 重写 BLAS）。
        // 做法：每 60 帧（≈1 s）把游标推进 4 段，把它们重新标脏 ⇒ 以 g_sectionBLAS
        // 的规模（~5000 段）计约 20 分钟内全表轮一遍；对单个 ~5 ms 的重建成本而言
        // 4 段/秒 ≈ 2% 单核占用，可忽略。
        static const uint32_t sSelfHealPerSecond = []
        {
            const char* raw = std::getenv("ORION_SECTION_SELFHEAL_RATE");
            if(raw == nullptr || raw[0] == '\0')
            {
                return 4u;
            }
            const int v = std::atoi(raw);
            return v < 0 ? 0u : static_cast<uint32_t>(v);
        }();

        static bool sSelfHealEnabled = []
        {
            const char* raw = std::getenv("ORION_SECTION_SELFHEAL");
            return raw == nullptr || raw[0] != '0';
        }();

        if(sSelfHealEnabled && sSelfHealPerSecond > 0u && !g_sectionBLAS.empty())
        {
            static size_t sSelfHealPhase = 0u;

            const size_t total = g_sectionBLAS.size();
            const size_t stride = std::max<size_t>(
                1u,
                total / std::max<size_t>(1u, sSelfHealPerSecond));

            std::vector<uint64_t> healKeys;
            healKeys.reserve(sSelfHealPerSecond + 2u);
            size_t index = 0u;

            for(auto it = g_sectionBLAS.begin();
                it != g_sectionBLAS.end();
                ++it, ++index)
            {
                if((index % stride) != sSelfHealPhase)
                {
                    continue;
                }

                // 只重建地形类（opaque/OMM）：实体/粒子每帧都在变，不归这里管。
                const uint64_t classBits =
                    it->second.sectionKey & ~SPATIAL_SECTION_KEY_MASK;

                if(classBits != SECTION_AS_CLASS_OPAQUE &&
                   classBits != SECTION_AS_CLASS_OMM &&
                   classBits != SECTION_AS_CLASS_OMM_LEAVES)
                {
                    continue;
                }

                healKeys.push_back(
                    it->second.sectionKey & SPATIAL_SECTION_KEY_MASK
                );
            }

            sSelfHealPhase = (sSelfHealPhase + 1u) % stride;

            if(!healKeys.empty())
            {
                std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
                size_t healed = 0u;
                for(const uint64_t spatialKey : healKeys)
                {
                    if(g_dirtySectionKeys.insert(spatialKey).second)
                    {
                        ++healed;
                    }
                }
                if(healed > 0u)
                {
                    markWorldGeometryDirty();
                }
            }
        }

        // 2026-11-15：段级转储（ORION_SECTION_DUMP=1）。
        // 目的：定位"固定一区块大小的永久洞"。
        //   若洞里那段出现在本行的 zeroTri 列表（triangleCount==0）⇒ 几何到过
        //     C++，死在下游（捕获/BLAS 构建）；
        //   若整段不在 g_sectionBLAS 里 ⇒ 几何根本没到（Java 侧没发/被去重拦住）。
        // 同时打印 asN（BLAS 表规模）与 dirtyN（待重建脏段数）。
        static const bool sSectionDumpEnabled = []
        {
            const char* raw = std::getenv("ORION_SECTION_DUMP");
            return raw != nullptr && raw[0] == '1';
        }();

        if(sSectionDumpEnabled)
        {
            size_t zeroTriCount = 0u;
            size_t listed = 0u;
            std::string zeroTriList;

            for(const auto& entry : g_sectionBLAS)
            {
                const RTSectionAS& asEntry = entry.second;

                // 只关心地形主类（opaque/OMM），实体/粒子类没有三角数是常态。
                const uint64_t classBits =
                    asEntry.sectionKey & ~SPATIAL_SECTION_KEY_MASK;

                if(classBits != SECTION_AS_CLASS_OPAQUE &&
                   classBits != SECTION_AS_CLASS_OMM &&
                   classBits != SECTION_AS_CLASS_OMM_LEAVES)
                {
                    continue;
                }

                if(asEntry.triangleCount != 0u)
                {
                    continue;
                }

                ++zeroTriCount;

                if(listed < 24u)
                {
                    ++listed;

                    const uint64_t spatialKey =
                        asEntry.sectionKey & SPATIAL_SECTION_KEY_MASK;

                    const int32_t sx = sectionKeyColumnX(spatialKey);
                    const int32_t sz = sectionKeyColumnZ(spatialKey);
                    const int32_t sy =
                        static_cast<int16_t>(
                            (spatialKey >> 44) & 0xFFFFull);

                    zeroTriList += " (" + std::to_string(sx) + "," +
                        std::to_string(sy) + "," +
                        std::to_string(sz) + ")";
                }
            }

            size_t dirtyCount = 0u;

            {
                std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
                dirtyCount = g_dirtySectionKeys.size();
            }

            RTLogFlushed(
                "[SECTION DUMP] asN=" + std::to_string(g_sectionBLAS.size()) +
                " zeroTri=" + std::to_string(zeroTriCount) +
                " dirtyN=" + std::to_string(dirtyCount) +
                " zeroTriList(first " + std::to_string(listed) + "):" +
                (zeroTriList.empty() ? std::string(" <none>") : zeroTriList));
        }

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
        g_sectionSplitPoolTakeHits = 0u;
        g_sectionSplitPoolTakeMisses = 0u;
        g_sectionSplitPoolReturns = 0u;
        g_sectionSplitPoolReturnRejects = 0u;
        g_sectionSplitPoolReturnDrops = 0u;
        g_sectionSplitPoolStarveSkips = 0u;
        for(size_t growBucket = 0u; growBucket < 8u; ++growBucket)
        {
            g_captureGrowBucketCount[growBucket] = 0u;
            g_captureGrowBucketBytes[growBucket] = 0u;
        }
        g_captureGrowFromEmpty = 0u;
        g_captureGrowFromSmall = 0u;
        g_captureArenaSegments = 0u;
        g_captureArenaBytes = 0u;
        g_captureArenaResets = 0u;
        g_captureArenaSkipResets = 0u;
        g_captureArenaStaleSegments = 0u;
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

    static std::vector<RTSectionAS*> sectionsNeedingBuild;
    sectionsNeedingBuild.clear();

    sectionsNeedingBuild.reserve(
        sectionUpdates.size() * 7u
    );

    g_sectionBLAS.reserve(
        g_sectionBLAS.size() + sectionUpdates.size() * 7u
    );


    struct PendingSectionUpload
    {
        RTSectionAS* section = nullptr;
        // 2026-11-07i：数据来自 capture arena（稳定地址），不再指向 split。
        std::span<const Vertex> vertices;
        VkDeviceSize stagingOffset = 0;
        std::span<const float> motion;
        std::span<const SectionVertex> packed;
    };


    // 2026-11-07k：跨帧复用。rebuildWorldGeometry 只在渲染线程调用，
    // 每帧 clear() 保留容量 —— 段数到 5500 时这几个容器原本每帧都要
    // malloc/free 一次（几十~几百 KB），还要吃一次首次触碰的缺页。
    static std::vector<PendingSectionUpload> pendingUploads;
    pendingUploads.clear();

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

    // 2026-11-26 (Batch AB): Per-frame & distance OMM GPU budget gate.
    // Beyond 48 blocks from the camera or when a section/frame exceeds budget, cutout
    // geometry builds immediately via the clean anyHit fallback (no mid-section requeue).
    constexpr float kOmmMaxDistanceBlocks = 48.0f;
    constexpr float kOmmMaxDistanceSq = kOmmMaxDistanceBlocks * kOmmMaxDistanceBlocks;
    constexpr uint32_t kMaxOmmBuildsPerFrame = 2u;
    constexpr size_t kMaxOmmBytesPerFrame = 256u * 1024u;
    constexpr size_t kMaxSingleSectionOmmBytes = 512u * 1024u;

    const CameraUBO* ommCamUbo =
        g_cameraUboMapped != nullptr
            ? static_cast<const CameraUBO*>(g_cameraUboMapped)
            : nullptr;

    uint32_t ommSectionsCreatedThisFrame = 0u;
    size_t ommBytesCreatedThisFrame = 0u;
    double ommMsThisFrame = 0.0;

    static const double sOmmMsPerFrameCap = []
    {
        const char* raw = std::getenv("ORION_OMM_MS_PER_FRAME");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 3.0;
        }

        const double parsed = std::atof(raw);

        return parsed < 0.0 ? 0.0 : parsed;
    }();

    uint64_t ommFallbackAnyHitThisFrame = 0u;

    // 2026-11-12：build loop 的"class 调用总和"，用来把 bld 拆成
    // 骨架（update 循环本身）与 class（rebuildGeometryClass × 7）两部分。
    double bldClassSumMs = 0.0;
    uint64_t bldClassCalls = 0u;
    uint64_t bldUpdateCount = 0u;

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

        // 2026-11-07j：arena 代际校验。段所在的内存块若已被复位后的新段覆盖，
        // 顶点数据就不是本段的 —— 继续 build 会得到一片错位/空白的几何
        // （外部表现就是"某个方向整片没有几何"）。这里直接跳过并重新标脏。
        if(update.second.arenaEpoch != 0u &&
           update.second.arenaEpoch != g_captureArenaEpoch)
        {
            ++g_captureArenaStaleSegments;

            {
                std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
                g_dirtySectionKeys.insert(update.first);
            }

            continue;
        }

        if(sectionUpdateIndex != 0u && g_asBuilt && !forceReset)
        {
            constexpr VkDeviceSize kMaxStagingBytesPerFrame = 6ull * 1024ull * 1024ull;
            const double rebuildElapsedMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - perfL10RebuildStart
                ).count();
            if(rebuildElapsedMs >= sectionRebuildBudgetMs ||
               totalStagingBytes >= kMaxStagingBytesPerFrame)
            {
                requeueSectionsFrom(sectionUpdateIndex);
                break;
            }
        }

        ++bldUpdateCount;

        const uint64_t spatialSectionKey =
            update.first;

        auto rebuildGeometryClass =
            [&]
            (
                uint64_t classKey,
                std::span<const Vertex> vertices,
                bool opaqueGeometry,
                const OmmCutoutInput* ommInput,
                std::span<const float> vertexMotion,
                std::span<const SectionVertex> packedVertices
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
                        (!vertexMotion.empty())
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
                                { &refitSection, vertices, stagingOffset,
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

                    const uint64_t classBits =
                        classKey & ~SPATIAL_SECTION_KEY_MASK;
                    const bool isHandClass = classBits == SECTION_AS_CLASS_HAND;
                    const bool dynamicRefitClass =
                        !isHandClass && (
                            classBits == SECTION_AS_CLASS_ENTITY ||
                            classBits == SECTION_AS_CLASS_LOCAL_PLAYER ||
                            classBits == SECTION_AS_CLASS_PARTICLE ||
                            !vertexMotion.empty());
                    const bool allowBlasRefit =
                        sectionBLASRefitEnabled() && dynamicRefitClass;

                    const VkDeviceSize scratchSize =
                        createSectionBLAS(
                            newSection,
                            classKey,
                            vertices,
                            opaqueGeometry,
                            ommInput,
                            vertexMotion,
                            allowBlasRefit
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
                        { &insertedSection, vertices, stagingOffset,
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
           update.second.ommLeaves.empty() &&
           update.second.alpha.empty() &&
           update.second.water.empty() &&
           update.second.entity.empty() &&
           update.second.localPlayer.empty() &&
           update.second.particle.empty() &&
           update.second.hand.empty())
        {
            ++removedSectionCount;
        }

        struct RtGeometryClassPlan
        {
            uint64_t classKey;
            std::span<const Vertex> vertices;
            bool opaqueGeometry;
            const OmmCutoutInput* ommInput;
            std::span<const float> vertexMotion;
            std::span<const SectionVertex> packed;
        };

        constexpr size_t RT_GEOMETRY_CLASS_COUNT = 9u;

        const RtGeometryClassPlan geometryClassPlan[RT_GEOMETRY_CLASS_COUNT] =
        {
            { spatialSectionKey | SECTION_AS_CLASS_OPAQUE,
              update.second.opaque, true, nullptr, {},
              update.second.opaquePacked },
            { spatialSectionKey | SECTION_AS_CLASS_OMM,
              update.second.ommCutout, false,
              update.second.hasOmmInput ? &update.second.ommInput : nullptr,
              {}, update.second.ommCutoutPacked },
            { spatialSectionKey | SECTION_AS_CLASS_OMM_LEAVES,
              update.second.ommLeaves, false,
              update.second.hasOmmLeavesInput ? &update.second.ommLeavesInput : nullptr,
              {}, update.second.ommLeavesPacked },
            { spatialSectionKey | SECTION_AS_CLASS_ALPHA,
              update.second.alpha, false, nullptr, {},
              update.second.alphaPacked },
            { spatialSectionKey | SECTION_AS_CLASS_WATER,
              update.second.water, false, nullptr, {},
              update.second.waterPacked },
            { spatialSectionKey | SECTION_AS_CLASS_ENTITY,
              update.second.entity, false, nullptr,
              update.second.entityMotion, {} },
            { spatialSectionKey | SECTION_AS_CLASS_LOCAL_PLAYER,
              update.second.localPlayer, false, nullptr,
              update.second.localPlayerMotion, {} },
            { spatialSectionKey | SECTION_AS_CLASS_PARTICLE,
              update.second.particle, false, nullptr,
              update.second.particleMotion, {} },
            { spatialSectionKey | SECTION_AS_CLASS_HAND,
              update.second.hand, false, nullptr,
              update.second.handMotion, {} }
        };

        bool sectionWithinOmmRange = true;
        if(ommCamUbo != nullptr)
        {
            float secCx = 0.0f;
            float secCy = 0.0f;
            float secCz = 0.0f;
            vramSectionCenterFromSpatialKey(
                spatialSectionKey,
                secCx,
                secCy,
                secCz
            );
            const float dx = secCx - ommCamUbo->playerX;
            const float dy = secCy - ommCamUbo->playerY;
            const float dz = secCz - ommCamUbo->playerZ;
            sectionWithinOmmRange =
                (dx * dx + dy * dy + dz * dz) <= kOmmMaxDistanceSq;
        }

        for(size_t classIndex = 0u;
            classIndex < RT_GEOMETRY_CLASS_COUNT;
            ++classIndex)
        {
            const RtGeometryClassPlan& plan =
                geometryClassPlan[classIndex];

            const uint64_t planClassBits =
                plan.classKey & ~SPATIAL_SECTION_KEY_MASK;
            const OmmCutoutInput* effectiveOmmInput = plan.ommInput;
            if(effectiveOmmInput != nullptr &&
               (planClassBits == SECTION_AS_CLASS_OMM ||
                planClassBits == SECTION_AS_CLASS_OMM_LEAVES))
            {
                const size_t ommBytes = effectiveOmmInput->data.size();
                const bool allowOmm =
                    sectionWithinOmmRange &&
                    ommBytes <= kMaxSingleSectionOmmBytes &&
                    ommSectionsCreatedThisFrame < kMaxOmmBuildsPerFrame &&
                    (ommSectionsCreatedThisFrame == 0u ||
                     ommBytesCreatedThisFrame + ommBytes <= kMaxOmmBytesPerFrame) &&
                    (sOmmMsPerFrameCap <= 0.0 ||
                     ommSectionsCreatedThisFrame == 0u ||
                     ommMsThisFrame < sOmmMsPerFrameCap);
                if(allowOmm)
                {
                    ++ommSectionsCreatedThisFrame;
                    ommBytesCreatedThisFrame += ommBytes;
                }
                else
                {
                    effectiveOmmInput = nullptr;
                    ++ommFallbackAnyHitThisFrame;
                }
            }

            const double sectionClassCreateMsBefore =
                g_sectionASCreateMs;

            const double sectionClassRetireMsBefore =
                g_sectionASRetireMs;

            const std::chrono::steady_clock::time_point sectionClassStart =
                std::chrono::steady_clock::now();

            rebuildGeometryClass(
                plan.classKey,
                plan.vertices,
                plan.opaqueGeometry,
                effectiveOmmInput,
                plan.vertexMotion,
                plan.packed
            );

            // 2026-11-12：单段宽打点。此前 buildLoopMs 只有"每 600 帧一条 counters"
            // 的全局累计（峰值 13.4ms 无法归因）。这里用全局计数器差值把一次调用
            // 拆成 createSectionBLAS / retire / 其它（refit 判等 + 三角分类），
            // 只加计时，不改任何逻辑。阈值：ORION_SECTION_SLOW_MS，默认 0.5。
            const double sectionClassMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - sectionClassStart
                ).count();

            static const double s_sectionSlowLogMs = []
            {
                const char* raw = std::getenv("ORION_SECTION_SLOW_MS");

                if(raw == nullptr || raw[0] == '\0')
                {
                    return 0.5;
                }

                const double parsed = std::atof(raw);

                return parsed <= 0.0 ? 0.5 : parsed;
            }();

            bldClassSumMs += sectionClassMs;
            ++bldClassCalls;
            // g_sectionBuildOmmMs 是「最后一次 createSectionBLAS」的值，非 OMM
            // 段会读到陈旧值 ⇒ 只累加本段确实走 OMM 创建的那次。
            if(effectiveOmmInput != nullptr)
            {
                ommMsThisFrame += g_sectionBuildOmmMs;
            }

            if(sectionClassMs >= s_sectionSlowLogMs)
            {
                const double sectionClassCreateMs =
                    g_sectionASCreateMs - sectionClassCreateMsBefore;

                const double sectionClassRetireMs =
                    g_sectionASRetireMs - sectionClassRetireMsBefore;

                (CPUPERF_QUIET_HOT_LOGS ? RTLog : RTLogFlushed)(
                    "[RT SECTION SLOW] frame=" +
                    std::to_string(g_perfL10FrameSerial) +
                    " key=" + std::to_string(plan.classKey) +
                    " verts=" + std::to_string(plan.vertices.size()) +
                    " totalMs=" + std::to_string(sectionClassMs) +
                    " createMs=" + std::to_string(sectionClassCreateMs) +
                    " retireMs=" + std::to_string(sectionClassRetireMs) +
                    " opaque=" + std::string(plan.opaqueGeometry ? "1" : "0") +
                    " prepMs=" + std::to_string(g_sectionBuildPrepMs) +
                    " ommMs=" + std::to_string(g_sectionBuildOmmMs) +
                    " bufMs=" + std::to_string(g_sectionBuildBufMs) +
                    " vkBufMs=" + std::to_string(g_sectionBuildVkBufMs) +
                    " reqMs=" + std::to_string(g_sectionBuildReqMs) +
                    " sizesMs=" + std::to_string(g_sectionBuildSizesMs) +
                    " memMs=" + std::to_string(g_sectionBuildMemMs) +
                    " asMs=" + std::to_string(g_sectionBuildAsMs) +
                    " addrMs=" + std::to_string(g_sectionBuildAddrMs) +
                    " otherMs=" + std::to_string(
                        sectionClassMs -
                        sectionClassCreateMs -
                        sectionClassRetireMs)
                );
            }
        }
    }

    perfL10AfterBuildLoop =
        std::chrono::steady_clock::now();

    // 2026-11-12：bld 归因。total 里 class 之和之外的部分就是 update 循环骨架
    // （arena 代际校验、g_sectionBLAS 查找、pendingUploads 扩容、requeue 等）。
    {
        const double bldTotalMs =
            std::chrono::duration<double, std::milli>(
                perfL10AfterBuildLoop - perfL10BuildLoopStart
            ).count();

        if(ommFallbackAnyHitThisFrame > 0u &&
           (g_perfL10FrameSerial % 60u) == 0u)
        {
            (CPUPERF_QUIET_HOT_LOGS ? RTLog : RTLogFlushed)(
                "[RT OMM GATE] frame=" +
                std::to_string(g_perfL10FrameSerial) +
                " fallbackAnyHit=" + std::to_string(ommFallbackAnyHitThisFrame) +
                " created=" + std::to_string(ommSectionsCreatedThisFrame) +
                " ommKB=" + std::to_string(ommBytesCreatedThisFrame / 1024u) +
                " ommMsThisFrame=" + std::to_string(ommMsThisFrame) +
                " capMs=" + std::to_string(sOmmMsPerFrameCap)
            );
        }

        if(bldTotalMs >= 5.0)
        {
            (CPUPERF_QUIET_HOT_LOGS ? RTLog : RTLogFlushed)(
                "[RT BLD BREAKDOWN] frame=" +
                std::to_string(g_perfL10FrameSerial) +
                " totalMs=" + std::to_string(bldTotalMs) +
                " classSumMs=" + std::to_string(bldClassSumMs) +
                " skelMs=" + std::to_string(bldTotalMs - bldClassSumMs) +
                " updateN=" + std::to_string(bldUpdateCount) +
                " classCalls=" + std::to_string(bldClassCalls)
            );
        }
    }

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
                    upload.vertices.size()
                ) *
                static_cast<VkDeviceSize>(
                    upload.section->vertexStride);

            if(upload.section->packedSectionVertices &&
               upload.packed.size() == upload.vertices.size())
            {
                std::memcpy(
                    stagingBytes +
                        static_cast<size_t>(upload.stagingOffset),
                    upload.packed.data(),
                    static_cast<size_t>(upload.vertices.size()) *
                        sizeof(SectionVertex)
                );
            }
            else if(upload.section->packedSectionVertices)
            {
                SectionVertex* const packed =
                    reinterpret_cast<SectionVertex*>(
                        stagingBytes +
                        static_cast<size_t>(upload.stagingOffset));

                const std::span<const Vertex> source = upload.vertices;

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
                    upload.vertices.data(),
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

                if(!upload.motion.empty())
                {
                    std::memcpy(
                        tail,
                        upload.motion.data(),
                        std::min(
                            static_cast<size_t>(motionTailBytes),
                            upload.motion.size() * sizeof(float)
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

    // 2026-11-07i：本帧的段全部消费完（staging 已把顶点拷走），队列为空时
    // 复位 capture arena。还有在途段（捕获线程已 commit 但渲染线程没取）时
    // 跳过，留到下一帧 —— 否则会把它们的数据覆盖掉。
    {
        size_t queuedSegments = 0u;

        {
            std::lock_guard<std::mutex> capturedLock(g_capturedSectionsMutex);
            queuedSegments = g_capturedSections.size() - g_capturedSectionsHead;
        }

        captureArenaTryReset(queuedSegments);
    }

    // 2026-11-07k：跨帧复用（见 pendingUploads 的说明）。
    static std::vector<RTSectionAS*> orderedSections;

    // 2026-11-10：截面集合缓存。
    //
    // 触发 rebuild 的源有两类：世界/区块几何变化（markWorldGeometryDirty(true)，
    // 会 bump terrain revision）与实体/粒子变化（markWorldGeometryDirty(false)，
    // 只 bump world revision）。后者**完全不改变 section 集合**，却会重跑下面
    // 三步：重建+排序 orderedSections、逐 section 写地址槽与角色表、
    // rebuildLodCoverage 整表重建。这三步只依赖集合本身 ⇒ 用 terrain revision
    // 做缓存键。实测 rebuild 占 geometry 段 100%（other≈0.01ms），
    // 中位 2.83ms / p90 12.8ms，其中排序与整表重建是大头。
    //
    // 回退：ORION_SECTION_ROLE_CACHE=0（每次都重建，逐位回到改动前）。
    // 2026-11-10 事故后改为**默认关闭**：
    // 09:02 一局在进世界约 1 分钟后崩溃（VK semaphore 5s 超时 ⇒ GPU 挂死）。
    // 最可能的原因是缓存里的 RTSectionAS* 变成了悬垂指针 —— 某个 section 被销毁
    // 但没 bump terrain revision 时，复用旧列表会拿已释放的 blasAddress 去建
    // TLAS。缓存本身没有代际校验，无法区分"同一指针换了内容"。
    // 现在只在该 env 显式置 1 时启用，用于单独复现/验证。
    static const bool s_sectionRoleCacheEnabled = []()
    {
        const char* raw = std::getenv("ORION_SECTION_ROLE_CACHE");
        // 2026-11-12：默认开启。判等已补上"集合内容指纹 + slot 剔除保护"（见下方
        // sectionSetFingerprintNow），能区分"同一 revision 但段被原地替换/指针复用"，
        // 这是 2026-11-10 悬垂指针事故的关键缺口。回退：ORION_SECTION_ROLE_CACHE=0。
        return raw == nullptr || raw[0] != '0';
    }();
    static uint64_t s_orderedSectionsRevision = ~0ull;
    static size_t s_orderedSectionsCount = 0u;
    static uint64_t s_orderedSectionsFingerprint = 0xcbf29ce484222325ull;
    static size_t s_orderedTerrainCount = 0u;
    static uint64_t s_orderedTerrainFingerprint = 0xcbf29ce484222325ull;
    static size_t s_orderedDynamicCount = 0u;
    static uint64_t s_orderedDynamicFingerprint = 0xcbf29ce484222325ull;
    static std::vector<RTSectionAS*> s_dynamicSectionsScratch;

    const uint64_t sectionRevisionNow =
        g_terrainGeometryRevision.load(std::memory_order_acquire);

    uint64_t terrainFingerprintNow = 0xcbf29ce484222325ull;
    size_t terrainCountNow = 0u;
    uint64_t dynamicFingerprintNow = 0xcbf29ce484222325ull;
    size_t dynamicCountNow = 0u;
    bool hasAnyLodInBlas = false;
    s_dynamicSectionsScratch.clear();

    for(auto& fingerprintEntry : g_sectionBLAS)
    {
        const uint64_t entrySpatialKey =
            fingerprintEntry.first & SPATIAL_SECTION_KEY_MASK;
        if(!hasAnyLodInBlas &&
           (((fingerprintEntry.first & VOXY_LOD_RESERVED_Y) == VOXY_LOD_RESERVED_Y) ||
            isDhLodSpatialKey(entrySpatialKey)))
        {
            hasAnyLodInBlas = true;
        }
        uint64_t entryHash = 0xcbf29ce484222325ull;
        entryHash = (entryHash ^ fingerprintEntry.first) * 0x100000001b3ull;
        entryHash =
            (entryHash ^
             static_cast<uint64_t>(
                 reinterpret_cast<uintptr_t>(&fingerprintEntry.second))) *
            0x100000001b3ull;
        entryHash =
            (entryHash ^
             static_cast<uint64_t>(fingerprintEntry.second.blasAddress)) *
            0x100000001b3ull;
        entryHash =
            (entryHash ^
             static_cast<uint64_t>(fingerprintEntry.second.vertexAddress)) *
            0x100000001b3ull;
        entryHash =
            (entryHash ^
             static_cast<uint64_t>(fingerprintEntry.second.triangleCount)) *
            0x100000001b3ull;

        const bool isDynamicEntry =
            (fingerprintEntry.first & SPATIAL_SECTION_KEY_MASK) ==
            ENTITY_RT_SPATIAL_KEY;
        if(isDynamicEntry)
        {
            dynamicFingerprintNow =
                (dynamicFingerprintNow ^ entryHash) * 0x100000001b3ull;
            ++dynamicCountNow;
            s_dynamicSectionsScratch.push_back(&fingerprintEntry.second);
        }
        else
        {
            terrainFingerprintNow =
                (terrainFingerprintNow ^ entryHash) * 0x100000001b3ull;
            ++terrainCountNow;
        }
    }

    const uint64_t sectionSetFingerprintNow =
        (terrainFingerprintNow ^ dynamicFingerprintNow) * 0x100000001b3ull;

    // 上一次重建曾因槽位耗尽剔除过段（列表比 map 短）⇒ 不缓存，下一帧继续重试，
    // 与"每次重建都会重新尝试 ensureSectionVertexAddressSlot"的原语义一致。
    const bool cachedSectionListTruncated =
        orderedSections.size() != s_orderedSectionsCount;

    const bool terrainUnchanged =
        s_sectionRoleCacheEnabled &&
        !cachedSectionListTruncated &&
        sectionRevisionNow == s_orderedSectionsRevision &&
        terrainCountNow == s_orderedTerrainCount &&
        terrainFingerprintNow == s_orderedTerrainFingerprint;

    const bool dynamicUnchanged =
        dynamicCountNow == s_orderedDynamicCount &&
        dynamicFingerprintNow == s_orderedDynamicFingerprint;

    if(terrainUnchanged && dynamicUnchanged)
    {
        // Everything unchanged: reuse orderedSections, role slots, and LOD coverage.
    }
    else if(terrainUnchanged && !dynamicUnchanged)
    {
        // Batch X (P1): Only dynamic sections (hand/entity/localPlayer/particle) changed.
        // Splice out old ENTITY_RT_SPATIAL_KEY pointers, update slots for dynamic entries
        // only, and insert in sorted sectionKey order without re-sorting terrain or calling
        // rebuildLodCoverage(orderedSections).
        orderedSections.erase(
            std::remove_if(
                orderedSections.begin(),
                orderedSections.end(),
                [](const RTSectionAS* section)
                {
                    return (section->sectionKey & SPATIAL_SECTION_KEY_MASK) ==
                           ENTITY_RT_SPATIAL_KEY;
                }
            ),
            orderedSections.end()
        );

        for(const RTSectionAS* dyn : s_dynamicSectionsScratch)
        {
            if(dyn->blas == VK_NULL_HANDLE ||
               dyn->blasAddress == 0 ||
               dyn->vertexAddress == 0)
            {
                throw std::runtime_error(
                    "Section AS map contains an incomplete entry"
                );
            }
        }

        std::sort(
            s_dynamicSectionsScratch.begin(),
            s_dynamicSectionsScratch.end(),
            [](const RTSectionAS* a, const RTSectionAS* b)
            {
                return a->sectionKey < b->sectionKey;
            }
        );

        size_t activeDynamicCount = 0u;
        for(RTSectionAS* section : s_dynamicSectionsScratch)
        {
            if(!ensureSectionVertexAddressSlot(*section))
            {
                continue;
            }

            writeSectionRoleSlot(*section);

            auto insertPos = std::lower_bound(
                orderedSections.begin(),
                orderedSections.end(),
                section,
                [](const RTSectionAS* a, const RTSectionAS* b)
                {
                    return a->sectionKey < b->sectionKey;
                }
            );
            orderedSections.insert(insertPos, section);
            ++activeDynamicCount;
        }

        s_orderedDynamicCount = dynamicCountNow;
        s_orderedDynamicFingerprint = dynamicFingerprintNow;
        s_orderedSectionsCount =
            (activeDynamicCount == dynamicCountNow)
                ? g_sectionBLAS.size()
                : (g_sectionBLAS.size() - (dynamicCountNow - activeDynamicCount));
        s_orderedSectionsFingerprint = sectionSetFingerprintNow;
    }
    else
    {
    s_orderedSectionsRevision = sectionRevisionNow;
    s_orderedSectionsCount = g_sectionBLAS.size();
    s_orderedSectionsFingerprint = sectionSetFingerprintNow;
    s_orderedTerrainCount = terrainCountNow;
    s_orderedTerrainFingerprint = terrainFingerprintNow;
    s_orderedDynamicCount = dynamicCountNow;
    s_orderedDynamicFingerprint = dynamicFingerprintNow;

    orderedSections.clear();

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


        static bool s_lodCoverageClearedWhenEmpty = false;
        if(hasAnyLodInBlas)
        {
            s_lodCoverageClearedWhenEmpty = false;
            rebuildLodCoverage(
                orderedSections
            );
        }
        else if(!s_lodCoverageClearedWhenEmpty)
        {
            s_lodCoverageClearedWhenEmpty = true;
            rebuildLodCoverage({});
        }
    } // end of sectionSetChanged

    rtxPreCmdAfterOrdered = std::chrono::steady_clock::now();


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
            // 2026-11-12：4 -> 8。实测单次驱逐 ~10 ms（11 段的小场景也一样，因为它是
            // O(全部段) 的扫描），4 帧一次折合 ~2.5 ms/帧，占轻量图单帧的 15%。
            // 而 VRAM governor 自己是 30 tick 采样一次，8 帧的响应仍远快于它。
            // 需要更激进回收时：ORION_VRAM_EVICT_INTERVAL=1/2/4 现场可调。
            return 8u;
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

        // 2026-11-12：evict 内部分段打点（只计时）。
        const std::chrono::steady_clock::time_point evictBlockStart =
            std::chrono::steady_clock::now();

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

        const std::chrono::steady_clock::time_point evictScanEnd =
            std::chrono::steady_clock::now();

        const size_t evictCandidateCount = candidates.size();

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

            const std::chrono::steady_clock::time_point evictSortEnd =
                std::chrono::steady_clock::now();

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

            const std::chrono::steady_clock::time_point evictVictimsEnd =
                std::chrono::steady_clock::now();

            const size_t evictVictimCount = victims.size();

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

            const std::chrono::steady_clock::time_point evictRemoveEnd =
                std::chrono::steady_clock::now();

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

            {
                const std::chrono::steady_clock::time_point evictEnd =
                    std::chrono::steady_clock::now();

                const auto evictSegMs =
                    [](const std::chrono::steady_clock::time_point& from,
                       const std::chrono::steady_clock::time_point& to)
                    {
                        return std::chrono::duration<double, std::milli>(
                            to - from
                        ).count();
                    };

                const double evictTotalMs =
                    evictSegMs(evictBlockStart, evictEnd);

                if(evictTotalMs >= 3.0)
                {
                    RTLogFlushed(
                        "[RT EVICT BREAKDOWN] frame=" +
                        std::to_string(g_perfL10FrameSerial) +
                        " totalMs=" + std::to_string(evictTotalMs) +
                        " scanMs=" + std::to_string(
                            evictSegMs(evictBlockStart, evictScanEnd)) +
                        " sortMs=" + std::to_string(
                            evictSegMs(evictScanEnd, evictSortEnd)) +
                        " victimsMs=" + std::to_string(
                            evictSegMs(evictSortEnd, evictVictimsEnd)) +
                        " removeMs=" + std::to_string(
                            evictSegMs(evictVictimsEnd, evictRemoveEnd)) +
                        " retireMs=" + std::to_string(
                            evictSegMs(evictRemoveEnd, evictEnd)) +
                        " candidates=" + std::to_string(evictCandidateCount) +
                        " victims=" + std::to_string(evictVictimCount) +
                        " sections=" + std::to_string(g_sectionBLAS.size())
                    );
                }
            }
        }
    }


    rtxPreCmdAfterEvict = std::chrono::steady_clock::now();

    // 2026-11-07k：跨帧复用。段数 5500 时这个 vector 是 5500×64B ≈ 352KB，
    // 原本每帧 malloc/free 一次。
    static std::vector<VkAccelerationStructureInstanceKHR> instances;
    instances.clear();
    uint32_t activeWaterSectionCount = 0u;

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

        const float effectiveCullMargin =
            (cullMarginLane > 0)
                ? static_cast<float>(cullMarginLane)
                : (cullCasterFrustumActive ? 24.0f : 0.0f);
        const float cullSphereRadius =
            13.8564f + effectiveCullMargin;

        if(tlasCullModeValue >= 2)
        {
            const CameraUBO* cullCamUbo =
                g_cameraUboMapped != nullptr
                    ? static_cast<const CameraUBO*>(g_cameraUboMapped)
                    : nullptr;
            if(cullCamUbo != nullptr)
            {
                const float fx = -cullCamUbo->viewInverse[8];
                const float fy = -cullCamUbo->viewInverse[9];
                const float fz = -cullCamUbo->viewInverse[10];
                const float fLen = std::sqrt(fx * fx + fy * fy + fz * fz);
                if(fLen > 1.0e-6f)
                {
                    g_lastTlasCullForward[0] = fx / fLen;
                    g_lastTlasCullForward[1] = fy / fLen;
                    g_lastTlasCullForward[2] = fz / fLen;
                    g_lastTlasCullPos[0] = cullCamUbo->viewInverse[12];
                    g_lastTlasCullPos[1] = cullCamUbo->viewInverse[13];
                    g_lastTlasCullPos[2] = cullCamUbo->viewInverse[14];
                    g_lastTlasCullValid = true;
                }
            }
        }

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


        static int diagBandCount[7] = {};
        static uint64_t diagBandTriangles[7] = {};
        static uint64_t diagTotalTriangles = 0;
        static int diagCulledCount = 0;
        static int diagFrustumCulled = 0;
        static int diagCasterCulled = 0;
        static std::chrono::steady_clock::time_point diagLastPrint{};
        static bool diagLastPrintValid = false;
        const int diagCullLimit = g_tlasCullDistance.load();
        const CameraUBO* diagUbo =
            g_cameraUboMapped != nullptr
                ? static_cast<const CameraUBO*>(g_cameraUboMapped)
                : nullptr;
        const float diagCenterX = diagUbo ? diagUbo->playerX : 0.0f;
        const float diagCenterZ = diagUbo ? diagUbo->playerZ : 0.0f;
        const float diagCenterY = diagUbo ? diagUbo->playerY : 0.0f;
        const bool entityInstancesEmpty = g_entityMeshInstances.empty();

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

                const float diagDx = sectionWorldX - diagCenterX;
                const float diagDy = sectionWorldY - diagCenterY;
                const float diagDz = sectionWorldZ - diagCenterZ;
                diagDistance = std::sqrt(
                    diagDx * diagDx + diagDy * diagDy + diagDz * diagDz);

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

            // ===================================================================
            // 2026-11-19 rasterLod（挂载点③，原规格行号 WorldGeometry.cpp:3210）：
            // DH 键不进 TLAS —— 远场由 dh_lod_raster.comp 光栅投影接管，主射线
            // miss 的像素由 K1 mode=14 从光栅 color/depth 图合成。与上面视锥
            // 剔除的 continue 同款；旗标关闭时不进本分支，与改前逐位一致。
            // ===================================================================
            if(g_dhRasterLodEnabled &&
               isDhLodSpatialKey(instanceSpatialKey))
            {
                ++g_dhLodRasterTlasSkips;
                continue;
            }

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

            // 2026-11-26：第一人称手部（见 SECTION_AS_CLASS_HAND 的说明）。
            // 0x20 = 只被主相机射线（0x21）命中，所有次级射线（0xcf）与动态光（0xcd）都排除它
            // ⇒ 手能看见、但不在世界里投影或遮光。
            if(geometryClass == SECTION_AS_CLASS_HAND)
            {
                instance.mask = 0x20u;
            }

            if(geometryClass == SECTION_AS_CLASS_PARTICLE)
            {
                instance.mask = 0xf7;
            }

            if(primaryMaskCull && geometryClass != SECTION_AS_CLASS_HAND)
            {
                instance.mask &= 0xfeu;
                s_cullPrimaryMaskedCount += 1;
            }

            // 2026-11-26 (Batch V, Feature 4): Reserve bit 4 (0x10) exclusively for the paired
            // solid-cube leaves instance used by volumetric light / fog queries.
            // Reserve bit 5 (0x20) exclusively for SECTION_AS_CLASS_HAND.
            if(geometryClass != SECTION_AS_CLASS_HAND)
            {
                instance.mask &= static_cast<uint8_t>(~0x30u);
            }

            if(geometryClass == SECTION_AS_CLASS_OPAQUE)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_OPAQUE;
            }
            else if(geometryClass == SECTION_AS_CLASS_OMM)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    section.hasOpacityMicromap ? SBT_RECORD_OMM : SBT_RECORD_ALPHA;
            }
            else if(geometryClass == SECTION_AS_CLASS_OMM_LEAVES)
            {
                instance.mask = primaryMaskCull ? 0xc6u : 0xc7u;
                instance.instanceShaderBindingTableRecordOffset =
                    section.hasOpacityMicromap ? SBT_RECORD_OMM : SBT_RECORD_ALPHA;
            }
            else if(geometryClass == SECTION_AS_CLASS_WATER)
            {
                instance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_WATER;
                ++activeWaterSectionCount;
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

            // 2026-11-22 (Batch 2): Voxy/DH LOD sections use SECTION_AS_CLASS_OPAQUE
            // but MUST enter rahit for lodHitIsRedundant(); force non-opaque on LOD only.
            static constexpr bool kTLAS_LOD_FORCE_NO_OPAQUE = true;
            const bool forceNoOpaque =
                (!opaqueGeometry && !ommGeometry) ||
                (kTLAS_LOD_FORCE_NO_OPAQUE && lodSpatialKey);

            const bool ommSurfaceCutout =
                (geometryClass == SECTION_AS_CLASS_OMM) && ommGeometry;
            const bool ommLeafCutout =
                (geometryClass == SECTION_AS_CLASS_OMM_LEAVES) && ommGeometry;

            instance.flags =
                ((ommSurfaceCutout || ommLeafCutout)
                    ? 0u
                    : VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR) |
                (
                    forceNoOpaque
                        ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR
                        : 0u
                );

            instance.accelerationStructureReference =
                section.blasAddress;

            if(geometryClass == SECTION_AS_CLASS_OMM_LEAVES)
            {
                instances.push_back(instance);
                VkAccelerationStructureInstanceKHR solidLeavesInstance = instance;
                solidLeavesInstance.mask = 0x10u;
                solidLeavesInstance.instanceShaderBindingTableRecordOffset =
                    SBT_RECORD_OPAQUE;
                solidLeavesInstance.flags =
                    VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR |
                    VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                    (section.hasOpacityMicromap
                        ? VK_GEOMETRY_INSTANCE_DISABLE_OPACITY_MICROMAPS_EXT
                        : 0u);
                instances.push_back(solidLeavesInstance);
                continue;
            }


            // 2026-11-12 实例化第 2 步：该 mesh 若登记了 N 组变换，就发 N 个实例
            // （共享同一个 BLAS / vertexAddressSlot，只是实例矩阵不同）。
            // 命中着色器已用 gl_ObjectToWorldEXT 把局部顶点变回世界（第 1 步）。
            const auto instanceIt = entityInstancesEmpty
                ? g_entityMeshInstances.end()
                : g_entityMeshInstances.find(section.sectionKey);

            if(instanceIt != g_entityMeshInstances.end() &&
               instanceIt->second.size() >= 12u)
            {
                const size_t registeredCount =
                    instanceIt->second.size() / 12u;

                static bool sLoggedInstanceExpansion = false;
                if(!sLoggedInstanceExpansion)
                {
                    sLoggedInstanceExpansion = true;
                    RTLog(
                        "[INSTANCE] mesh key expanded to " +
                        std::to_string(registeredCount) + " instances"
                    );
                }

                for(size_t placedIndex = 0u;
                    placedIndex < registeredCount;
                    ++placedIndex)
                {
                    VkAccelerationStructureInstanceKHR placed = instance;
                    const float* const m =
                        instanceIt->second.data() + placedIndex * 12u;

                    placed.transform.matrix[0][0] = m[0];
                    placed.transform.matrix[0][1] = m[1];
                    placed.transform.matrix[0][2] = m[2];
                    placed.transform.matrix[0][3] = m[3];
                    placed.transform.matrix[1][0] = m[4];
                    placed.transform.matrix[1][1] = m[5];
                    placed.transform.matrix[1][2] = m[6];
                    placed.transform.matrix[1][3] = m[7];
                    placed.transform.matrix[2][0] = m[8];
                    placed.transform.matrix[2][1] = m[9];
                    placed.transform.matrix[2][2] = m[10];
                    placed.transform.matrix[2][3] = m[11];

                    instances.push_back(placed);
                }
            }
            else
            {
                instances.push_back(
                    instance
                );
            }
        }

        const auto diagNow = std::chrono::steady_clock::now();
        if(!diagLastPrintValid ||
           std::chrono::duration<float>(diagNow - diagLastPrint).count() > 5.0f)
        {
            if(diagLastPrintValid)
            {
                RTLog(std::string("[CULL DIAG]") +
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
        else
        {
            std::fill(std::begin(diagBandCount), std::end(diagBandCount), 0);
            std::fill(std::begin(diagBandTriangles), std::end(diagBandTriangles), 0);
            diagTotalTriangles = 0;
            diagCulledCount = 0;
            diagFrustumCulled = 0;
            diagCasterCulled = 0;
            s_cullPrimaryMaskedCount = 0;
            s_visCullCandidateCount = 0;
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

    g_tlasWaterSectionCount.store(activeWaterSectionCount, std::memory_order_relaxed);
    g_tlasWaterSectionCountValid.store(true, std::memory_order_relaxed);

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
        sectionsNeedingBuild.empty() &&
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

    // 2026-11-15：TLAS MODE_UPDATE 快路闸门。
    //   ORION_TLAS_UPDATE 默认 0 = 改动前行为：TLAS 恒 MODE_BUILD。
    //   1 = 下方保守护栏全部通过时走 MODE_UPDATE、沿用同一 g_tlas；任一护栏
    //       不满足一律回落 BUILD（首帧 / 重建后第一帧 / 容量扩容 / 实例数
    //       变化 / BLAS 引用逐项不一致）。
    // 回退：=0（或不设）即全部 BUILD，逐位旧行为；改完重启客户端生效。
    static const bool s_tlasUpdateEnabled = []()
    {
        const char* raw = std::getenv("ORION_TLAS_UPDATE");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    // 已带 ALLOW_UPDATE_BIT 构建的 TLAS 句柄；句柄不同（首建 / park / 重建 /
    // 销毁）一律不得 UPDATE —— 规范要求可更新形态必须在首次 BUILD 时带 flag。
    static VkAccelerationStructureKHR s_tlasAllowUpdateHandle =
        VK_NULL_HANDLE;



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

        tlasInstancesBufferRecreated = true;

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

    // 仅闸门开时注入 ALLOW_UPDATE_BIT：这是一次性结构改动，影响该 TLAS 后续
    // 能否走 UPDATE；闸门关时 flags 与改动前逐位一致。带 flag 的首次 BUILD
    // 时间可能略增（驱动需预留可更新形态空间），属预期代价。
    tlasBuildInfo.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        (s_tlasUpdateEnabled
            ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
            : 0);

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

        // 新对象即将按上面的 flags 构建：仅闸门开时它才是可更新形态；
        // 闸门关时句柄置空，后续帧一律回落 BUILD。
        s_tlasAllowUpdateHandle =
            s_tlasUpdateEnabled ? g_tlas : VK_NULL_HANDLE;
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


    // UPDATE 保守护栏：以下全部通过才允许 MODE_UPDATE，任一不满足回落 BUILD。
    // 比较仅在闸门开时执行；关闸门零额外遍历。
    bool tlasCanUpdate = false;
    std::string tlasUpdateRejectReason = "gate off";
    if(s_tlasUpdateEnabled)
    {
        if(tlasHandleAtEntry == VK_NULL_HANDLE)
        {
            tlasUpdateRejectReason = "first create";
        }
        else if(tlasStorageRecreated || tlasInstancesBufferRecreated)
        {
            tlasUpdateRejectReason = "storage/instance buffer recreated";
        }
        else if(s_tlasAllowUpdateHandle != g_tlas)
        {
            tlasUpdateRejectReason = "existing TLAS lacks ALLOW_UPDATE";
        }
        else if(s_tlasInstancesShadow.size() != instances.size())
        {
            // 规范：UPDATE 实例数必须与 src TLAS 构建时一致，不允许增删实例。
            tlasUpdateRejectReason = "instance count changed";
        }
        else
        {
            // 保守项：规范对 UPDATE 下更换 BLAS 引用存在不确定性，逐项比对
            // accelerationStructureReference，任一变化一律回落 BUILD，不赌。
            bool referencesStable = true;

            for(size_t i = 0u; i < instances.size(); ++i)
            {
                if(s_tlasInstancesShadow[i].accelerationStructureReference !=
                   instances[i].accelerationStructureReference)
                {
                    referencesStable = false;
                    break;
                }
            }

            if(!referencesStable)
            {
                tlasUpdateRejectReason = "blas reference changed";
            }
            else
            {
                tlasCanUpdate = true;
            }
        }
    }


    struct BlasBuildWave
    {
        size_t begin = 0u;
        size_t end = 0u;
        VkDeviceSize scratchBytes = 0u;
    };

    static std::vector<RTSectionAS*> liveSectionsToBuild;
    liveSectionsToBuild.clear();
    static std::vector<VkDeviceSize> waveScratchOffsets;
    waveScratchOffsets.clear();
    static std::vector<BlasBuildWave> blasBuildWaves;
    blasBuildWaves.clear();

    VkDeviceSize requiredBlasScratchSize = maxBLASScratchSize;

    if(!sectionsNeedingBuild.empty())
    {
        liveSectionsToBuild.reserve(sectionsNeedingBuild.size());
        for(RTSectionAS* section : sectionsNeedingBuild)
        {
            if(section == nullptr)
            {
                continue;
            }

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

            liveSectionsToBuild.push_back(section);
        }

        if(!liveSectionsToBuild.empty())
        {
            static const VkDeviceSize s_blasScratchAlign = []() -> VkDeviceSize
            {
                if(g_physicalDevice == VK_NULL_HANDLE)
                {
                    return 256u;
                }
                VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR
                };
                VkPhysicalDeviceProperties2 props2{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
                };
                props2.pNext = &asProps;
                vkGetPhysicalDeviceProperties2(g_physicalDevice, &props2);
                const VkDeviceSize minAlign = static_cast<VkDeviceSize>(
                    asProps.minAccelerationStructureScratchOffsetAlignment
                );
                return minAlign > 256u ? minAlign : 256u;
            }();

            constexpr VkDeviceSize kMaxBlasWaveScratchBytes = 16ull * 1024ull * 1024ull;
            waveScratchOffsets.resize(liveSectionsToBuild.size());

            size_t waveBegin = 0u;
            VkDeviceSize waveOffset = 0u;
            for(size_t i = 0u; i < liveSectionsToBuild.size(); ++i)
            {
                const RTSectionAS* sec = liveSectionsToBuild[i];
                VkDeviceSize req = sec->pendingRefit ? sec->updateScratchSize : sec->buildScratchSize;
                if(req == 0u)
                {
                    req = std::max(sec->buildScratchSize, sec->updateScratchSize);
                }
                const VkDeviceSize alignedReq =
                    (req + s_blasScratchAlign - 1u) & ~(s_blasScratchAlign - 1u);

                if(i > waveBegin && waveOffset + alignedReq > kMaxBlasWaveScratchBytes)
                {
                    blasBuildWaves.push_back(BlasBuildWave{waveBegin, i, waveOffset});
                    requiredBlasScratchSize = std::max(requiredBlasScratchSize, waveOffset);
                    waveBegin = i;
                    waveOffset = 0u;
                }

                waveScratchOffsets[i] = waveOffset;
                waveOffset += alignedReq;
            }

            if(waveBegin < liveSectionsToBuild.size())
            {
                blasBuildWaves.push_back(BlasBuildWave{
                    waveBegin,
                    liveSectionsToBuild.size(),
                    waveOffset
                });
                requiredBlasScratchSize = std::max(requiredBlasScratchSize, waveOffset);
            }
        }
    }


    VkBuffer blasScratchBuffer =
        VK_NULL_HANDLE;

    VkDeviceMemory blasScratchMemory =
        VK_NULL_HANDLE;

    VkDeviceAddress blasScratchAddress =
        0;


    if(
        !liveSectionsToBuild.empty() &&
        requiredBlasScratchSize > 0
    )
    {
        RtxSectionBuildBufferSlot blasScratchSlot;

        if(g_blasScratchPool.tryAcquire(requiredBlasScratchSize, blasScratchSlot))
        {
            blasScratchBuffer = blasScratchSlot.buffer;
            blasScratchMemory = blasScratchSlot.memory;

            g_sectionBuildPoolReuseCount += 1u;
        }
        else
        {

            const VkDeviceSize blasScratchSlotBytes =
                sectionBuildPoolSlotBytes(requiredBlasScratchSize);

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

    const uint32_t geomTsSlot = beginGeomGpuTimestamp(cmd);


    bool anyPendingRefit = false;
    for(const RTSectionAS* section : sectionsNeedingBuild)
    {
        if(section != nullptr && section->pendingRefit)
        {
            anyPendingRefit = true;
            break;
        }
    }
    if(anyPendingRefit)
    {
        VkMemoryBarrier refitWarBarrier{
            VK_STRUCTURE_TYPE_MEMORY_BARRIER
        };
        refitWarBarrier.srcAccessMask =
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_SHADER_READ_BIT;
        refitWarBarrier.dstAccessMask =
            VK_ACCESS_TRANSFER_WRITE_BIT |
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_TRANSFER_BIT |
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            0,
            1,
            &refitWarBarrier,
            0,
            nullptr,
            0,
            nullptr
        );
    }

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


    if(!liveSectionsToBuild.empty())
    {
        recordSectionOmmBuildBatch(cmd, liveSectionsToBuild);
    }

    markGeomGpuTimestamp(cmd, geomTsSlot, 1u);


    VkMemoryBarrier asBuildBarrier{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER
    };

    asBuildBarrier.srcAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;

    asBuildBarrier.dstAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
        VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;


    for(const BlasBuildWave& wave : blasBuildWaves)
    {
        const size_t waveCount = wave.end - wave.begin;
        std::span<RTSectionAS* const> waveSections(
            liveSectionsToBuild.data() + wave.begin,
            waveCount
        );
        std::span<const VkDeviceSize> waveOffsets(
            waveScratchOffsets.data() + wave.begin,
            waveCount
        );

        recordSectionBLASBuildWave(
            cmd,
            waveSections,
            waveOffsets,
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

        for(RTSectionAS* section : waveSections)
        {
            enqueueBlasCompactionQuery(
                cmd,
                *section
            );
        }
    }

    std::vector<BlasCompactedAddressRemap> blasCompactedRemaps;
    if(recordPendingBlasCompactions(
           cmd,
           blasCompactedRemaps,
           rebuiltSectionCount > 1u) &&
       !blasCompactedRemaps.empty())
    {
        for(VkAccelerationStructureInstanceKHR& inst : instances)
        {
            for(const BlasCompactedAddressRemap& remap : blasCompactedRemaps)
            {
                if(inst.accelerationStructureReference == remap.oldAddress)
                {
                    inst.accelerationStructureReference = remap.newAddress;
                    break;
                }
            }
        }
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
        tlasInstancesUnchanged = false;
        tlasCanUpdate = false;
        tlasUpdateRejectReason = "blas compacted";
    }


    markGeomGpuTimestamp(cmd, geomTsSlot, 2u);

    tlasBuildInfo.dstAccelerationStructure =
        g_tlas;

    const bool updateTlas = tlasCanUpdate;

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
        // 闸门开时每 5 秒打一条 UPDATE/BUILD 选路与回落原因；关闸门不进这段。
        if(s_tlasUpdateEnabled)
        {
            static std::chrono::steady_clock::time_point s_tlasUpdateLastPrint{};
            static bool s_tlasUpdateLastValid = false;
            const auto tlasUpdateNow = std::chrono::steady_clock::now();

            if(!s_tlasUpdateLastValid ||
               std::chrono::duration<float>(
                   tlasUpdateNow - s_tlasUpdateLastPrint).count() > 5.0f)
            {
                s_tlasUpdateLastValid = true;
                s_tlasUpdateLastPrint = tlasUpdateNow;

                RTLog(std::string("[TLAS UPDATE] mode=") +
                    (updateTlas ? "UPDATE" : "BUILD") +
                    " reason=" + tlasUpdateRejectReason +
                    " instances=" + std::to_string(instances.size()));
            }
        }

        cmdBuild(
            cmd,
            1,
            &tlasBuildInfo,
            tlasRanges
        );
    }

    RTXMarkPhase("tlas:built");

    markGeomGpuTimestamp(cmd, geomTsSlot, 3u);


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

    commitGeomGpuTimestamp(geomTsSlot, g_perfL10FrameSerial);

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
            RTLog(std::string("[TLAS SKIP]") +
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
        requiredBlasScratchSize;

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

        // 2026-11-23 CPU批⑥：>=15ms 恒打（旧口径）；另加 >=6ms 每 60 帧抽样一条。
        static uint64_t s_geometrySlowSampleFrame = 0u;
        const bool geometrySlowSampled =
            CPUPERF_GEOMETRY_SLOW_SAMPLE &&
            rtxRebuildTotalMs >= 6.0 &&
            g_perfL10FrameSerial >= s_geometrySlowSampleFrame + 60u;

        if(geometrySlowSampled)
        {
            s_geometrySlowSampleFrame = g_perfL10FrameSerial;
        }

        if(rtxRebuildTotalMs >= 15.0 || geometrySlowSampled)
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
