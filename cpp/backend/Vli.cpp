// Orion backend — Vli 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// Total bytes of the capacity-sized VLI/VPT pools, i.e. every buffer that
// grows with the live page capacity.  Used by the allocation and growth logs so
// the resident cost of a session can be read straight from rt_debug.log.
VkDeviceSize vliPoolBytesForCapacity(uint32_t capacity)
{
    const VkDeviceSize pages = static_cast<VkDeviceSize>(capacity);

    return pages * VLI_PAGE_WORDS * sizeof(uint32_t)
        + pages * VPT_DIRECTIONAL_CACHE_WORDS * sizeof(uint32_t)
        + pages * VPT_SURFACE_FACES_PER_SECTION * sizeof(uint32_t)
        + (1u + pages) * sizeof(uint32_t)
        + pages * VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t)
        + pages * VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t)
        + pages * VPT_RELATED_EMITTERS_PER_SECTION * sizeof(VPTRelatedEmitterGPU)
        + pages * VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE *
            sizeof(VPTSourceDirectoryEntryGPU);
}

// =====================================================
// 2026-10-24：g_vliMutex 的**持锁时长**探针（每个加锁点一份）
//
// 动机（实测，不是推测）：
//   渲染线程 [RT VLI SLOW]  vli=16.30  wait=15.21  gather=0.63 upload=0.23
//   工作线程 [VLI WORKER]   hold=13.43 lockWait=11.98 cmp=0.11 apply=0.43
// 两边各等 12~15ms，而锁内实际工作只有 ~1ms ⇒ 存在一段长持锁，但既有探针只量了
// "等锁"，没量"持锁"。
//
// 用法：必须在 lock_guard **之前**声明 —— 析构顺序相反（先解锁再打日志），
// 否则 RTLog 落在临界区里，等于自己把持锁时间又拉长（同 VliWorkerHoldProbe）。
// 只在持锁 >= ORION_VLI_LOCK_LOG_MS（默认 2ms）时落盘。
// =====================================================
double vliLockHoldLogThresholdMs()
{
    static const double cached = []()
    {
        const char* raw = std::getenv("ORION_VLI_LOCK_LOG_MS");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 2.0;
        }

        const double parsed = std::atof(raw);

        return parsed <= 0.0 ? 2.0 : parsed;
    }();

    return cached;
}
uint64_t vliChunkColumnKey(int32_t chunkX, int32_t chunkZ)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(chunkX)) << 32) |
        static_cast<uint64_t>(static_cast<uint32_t>(chunkZ));
}
void vliChunkIndexAdd(int32_t sectionX, int32_t sectionZ, uint64_t sectionKey)
{
    std::vector<uint64_t>& column =
        g_vliChunkSectionIndex[vliChunkColumnKey(sectionX, sectionZ)];

    if(std::find(column.begin(), column.end(), sectionKey) == column.end())
    {
        column.push_back(sectionKey);
    }
}
void vliChunkIndexRemove(int32_t sectionX, int32_t sectionZ, uint64_t sectionKey)
{
    const auto it =
        g_vliChunkSectionIndex.find(vliChunkColumnKey(sectionX, sectionZ));

    if(it == g_vliChunkSectionIndex.end())
    {
        return;
    }

    std::vector<uint64_t>& column = it->second;
    column.erase(
        std::remove(column.begin(), column.end(), sectionKey),
        column.end()
    );

    if(column.empty())
    {
        g_vliChunkSectionIndex.erase(it);
    }
}
void vliHotRecordAndReport(uint64_t sectionKey, uint64_t ordinal)
{
    std::lock_guard<std::mutex> lock(g_vliHotMutex);

    ++g_vliHotCounts[sectionKey];

    if((ordinal % 500u) != 0u || g_vliHotCounts.empty())
    {
        return;
    }

    std::string report = "[VLI HOT] n=500 distinct=" +
        std::to_string(g_vliHotCounts.size());

    for(int rank = 0; rank < 5; ++rank)
    {
        uint64_t bestKey = 0u;
        uint32_t bestCount = 0u;

        for(const auto& entry : g_vliHotCounts)
        {
            if(entry.second > bestCount)
            {
                bestCount = entry.second;
                bestKey = entry.first;
            }
        }

        if(bestCount == 0u)
        {
            break;
        }

        const int32_t hotX = static_cast<int32_t>(bestKey & 0x3FFFFFull);
        const int32_t hotZ = static_cast<int32_t>((bestKey >> 22) & 0x3FFFFFull);
        const int32_t hotY = static_cast<int32_t>((bestKey >> 44) & 0xFFFFull);

        report += " t" + std::to_string(rank + 1) +
            "=(" + std::to_string(hotX) + "," + std::to_string(hotY) + "," +
            std::to_string(hotZ) + "):" + std::to_string(bestCount);

        g_vliHotCounts[bestKey] = 0u;
    }

    RTLog(report);
    g_vliHotCounts.clear();
}
bool vliHashGateEnabled()
{
    static const bool enabled = []()
    {
        const char* raw = std::getenv("ORION_VLI_HASH_GATE");

        return !(raw != nullptr && raw[0] == '0');
    }();

    return enabled;
}
uint64_t vliHashWords(const uint32_t* words, size_t count)
{
    uint64_t hash = 1469598103934665603ull;

    for(size_t i = 0u; i < count; ++i)
    {
        hash ^= static_cast<uint64_t>(words[i]);
        hash *= 1099511628211ull;
    }

    return hash;
}
bool vliUploadHashUnchanged(uint64_t sectionKey, uint64_t hash)
{
    std::lock_guard<std::mutex> lock(g_vliUploadHashMutex);

    const auto it = g_vliUploadHashByKey.find(sectionKey);

    return it != g_vliUploadHashByKey.end() && it->second == hash;
}
void vliUploadHashStore(uint64_t sectionKey, uint64_t hash)
{
    std::lock_guard<std::mutex> lock(g_vliUploadHashMutex);
    g_vliUploadHashByKey[sectionKey] = hash;
}
void vliUploadHashForget(uint64_t sectionKey)
{
    std::lock_guard<std::mutex> lock(g_vliUploadHashMutex);
    g_vliUploadHashByKey.erase(sectionKey);
}

// Caller must hold g_vliMutex.
VLISectionCpuPage* findVLISectionPageByIndexLocked(uint32_t pageIndex)
{
    if(pageIndex >= g_vliPageSectionKeys.size())
    {
        return nullptr;
    }

    const uint64_t sectionKey = g_vliPageSectionKeys[pageIndex];
    if(sectionKey == VLI_INVALID_SECTION_KEY)
    {
        return nullptr;
    }

    const auto it = g_vliSections.find(sectionKey);
    return it == g_vliSections.end() ? nullptr : &it->second;
}


void destroyVPTDirectHistoryBuffers()
{
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        if(g_vptDirectHistoryBuffers[i] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(g_device, g_vptDirectHistoryBuffers[i], nullptr);
            g_vptDirectHistoryBuffers[i] = VK_NULL_HANDLE;
        }
        if(g_vptDirectHistoryMemories[i] != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, g_vptDirectHistoryMemories[i], nullptr);
            g_vptDirectHistoryMemories[i] = VK_NULL_HANDLE;
        }
    }
    g_vptDirectHistoryWidth = 0u;
    g_vptDirectHistoryHeight = 0u;
    g_vptDirectHistoryBytes = 0;
    g_vptDirectHistoryNeedsClear = false;

    if(g_vptBestLightBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_vptBestLightBuffer, nullptr);
        g_vptBestLightBuffer = VK_NULL_HANDLE;
    }
    if(g_vptBestLightMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_vptBestLightMemory, nullptr);
        g_vptBestLightMemory = VK_NULL_HANDLE;
    }
    g_vptBestLightBytes = 0;
    g_vptBestLightTileColumns = 0u;
    g_vptBestLightTileRows = 0u;
}


void updateVPTDirectHistoryDescriptors()
{
    if(
        g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
        g_vptDirectHistoryBuffers[0] == VK_NULL_HANDLE ||
        g_vptDirectHistoryBuffers[1] == VK_NULL_HANDLE
    )
    {
        return;
    }

    VkDescriptorBufferInfo infos[3]{};
    VkWriteDescriptorSet writes[3]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        infos[i].buffer = g_vptDirectHistoryBuffers[i];
        infos[i].offset = 0;
        infos[i].range = VK_WHOLE_SIZE;

        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding = 26u + i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    uint32_t writeCount = 2u;
    if(g_vptBestLightBuffer != VK_NULL_HANDLE)
    {
        infos[2].buffer = g_vptBestLightBuffer;
        infos[2].offset = 0;
        infos[2].range = VK_WHOLE_SIZE;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = g_rayTracingDescriptorSet;
        writes[2].dstBinding = 80u;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &infos[2];
        writeCount = 3u;
    }
    vkUpdateDescriptorSets(g_device, writeCount, writes, 0u, nullptr);
}


void createVPTDirectHistoryBuffers(uint32_t width, uint32_t height)
{
    if(width == 0u || height == 0u)
    {
        return;
    }
    if(
        g_vptDirectHistoryBuffers[0] != VK_NULL_HANDLE &&
        g_vptDirectHistoryBuffers[1] != VK_NULL_HANDLE &&
        g_vptDirectHistoryWidth == width &&
        g_vptDirectHistoryHeight == height
    )
    {
        return;
    }
    if(
        g_vptDirectHistoryBuffers[0] != VK_NULL_HANDLE ||
        g_vptDirectHistoryBuffers[1] != VK_NULL_HANDLE
    )
    {
        check(
            FGDeviceWaitIdle(g_device),
            "wait device before VPT direct-history recreate failed"
        );
    }

    destroyVPTDirectHistoryBuffers();

    const uint64_t pixelCount =
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if(
        pixelCount > static_cast<uint64_t>(
            std::numeric_limits<VkDeviceSize>::max() / sizeof(VPTDirectHistoryGPU)
        )
    )
    {
        throw std::runtime_error("VPT direct-history size overflow");
    }

    const VkDeviceSize historyBytes =
        static_cast<VkDeviceSize>(pixelCount) *
        sizeof(VPTDirectHistoryGPU);
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        createBuffer(
            g_device,
            g_physicalDevice,
            historyBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            g_vptDirectHistoryBuffers[i],
            g_vptDirectHistoryMemories[i]
        );
    }

    g_vptDirectHistoryWidth = width;
    g_vptDirectHistoryHeight = height;
    g_vptDirectHistoryBytes = historyBytes;
    g_vptDirectHistoryNeedsClear = true;

    // 2026-11-02：per-tile best-light。一块 buffer 分两半（按
    // cam.historyReadIndex ping-pong），每 tile 一个 r32ui 打包键。
    // 必须在 updateVPTDirectHistoryDescriptors() 之前建好，否则 binding 80 不会被写。
    {
        const uint32_t bestLightTileSize = 8u;
        g_vptBestLightTileColumns =
            (width + bestLightTileSize - 1u) / bestLightTileSize;
        g_vptBestLightTileRows =
            (height + bestLightTileSize - 1u) / bestLightTileSize;
        const uint64_t bestLightTileCount =
            static_cast<uint64_t>(g_vptBestLightTileColumns) *
            static_cast<uint64_t>(g_vptBestLightTileRows);
        g_vptBestLightBytes =
            static_cast<VkDeviceSize>(bestLightTileCount * 2ull * 4ull);
        if(g_vptBestLightBytes == 0)
        {
            g_vptBestLightBytes = 8u;
        }
        createBuffer(
            g_device,
            g_physicalDevice,
            g_vptBestLightBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            g_vptBestLightBuffer,
            g_vptBestLightMemory
        );
    }

    updateVPTDirectHistoryDescriptors();

    std::stringstream ss;
    ss << "[VPT-2.2] primary DDA history created size="
       << width << "x" << height
       << " bytesPerBuffer="
       << static_cast<unsigned long long>(historyBytes)
       << " (48 B/px 单半区；本地光 DI EMA 后半已随 rt_restir_final 删除而回收)"
       << " bindings=26,27,80(best-light tiles="
       << g_vptBestLightTileColumns << "x" << g_vptBestLightTileRows
       << ", " << static_cast<unsigned long long>(g_vptBestLightBytes) << " B)";
    RTLog(ss.str());
}


void prepareVPTDirectHistoryBuffers(VkCommandBuffer cmd)
{
    if(
        cmd == VK_NULL_HANDLE ||
        g_vptDirectHistoryBuffers[0] == VK_NULL_HANDLE ||
        g_vptDirectHistoryBuffers[1] == VK_NULL_HANDLE ||
        g_vptDirectHistoryBytes == 0
    )
    {
        return;
    }

    VkBufferMemoryBarrier barriers[3]{};
    uint32_t barrierCount = 2u;
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        if(g_vptDirectHistoryNeedsClear)
        {
            vkCmdFillBuffer(
                cmd,
                g_vptDirectHistoryBuffers[i],
                0,
                g_vptDirectHistoryBytes,
                0u
            );
        }

        barriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].buffer = g_vptDirectHistoryBuffers[i];
        barriers[i].offset = 0;
        barriers[i].size = g_vptDirectHistoryBytes;
        barriers[i].srcAccessMask = g_vptDirectHistoryNeedsClear
            ? VK_ACCESS_TRANSFER_WRITE_BIT
            : VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }

    // best-light 只在重建时清零：之后的失效靠 rgen 里写入的帧奇偶位（代际校验），
    // 因此不需要每帧 fill —— 清了反而会把上一帧刚写的那半擦掉。
    if(g_vptBestLightBuffer != VK_NULL_HANDLE && g_vptBestLightBytes > 0)
    {
        if(g_vptDirectHistoryNeedsClear)
        {
            vkCmdFillBuffer(
                cmd,
                g_vptBestLightBuffer,
                0,
                g_vptBestLightBytes,
                0u
            );
        }

        barriers[2].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[2].buffer = g_vptBestLightBuffer;
        barriers[2].offset = 0;
        barriers[2].size = g_vptBestLightBytes;
        barriers[2].srcAccessMask = g_vptDirectHistoryNeedsClear
            ? VK_ACCESS_TRANSFER_WRITE_BIT
            : VK_ACCESS_SHADER_WRITE_BIT;
        barriers[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrierCount = 3u;
    }

    vkCmdPipelineBarrier(
        cmd,
        g_vptDirectHistoryNeedsClear
            ? VK_PIPELINE_STAGE_TRANSFER_BIT
            : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        barrierCount,
        barriers,
        0,
        nullptr
    );
    g_vptDirectHistoryNeedsClear = false;
}
bool rtDescriptorRingEnabledByEnv()
#if 0
{
    static int cached = -1;
    if(cached < 0)
    {
        const char* v = std::getenv("RTX_ATLAS_RING");
        cached = (v != nullptr && v[0] == '1') ? 1 : 0;
    }
    return cached != 0;
}
#endif
// 2026-11-06 #13 退役：环的 publish 是 bisect 残留（恒 return false、零调用
// 点），创建体在 RTX_ATLAS_RING=1 时仍会白占 512 CIS 配额 × 槽数的池。按死码
// 规范外层 #if 0 屏蔽保留（含 env 门，-D 也打不开）；下方三行空壳维持 ABI。
{
    return false;
}
void createRtDescriptorRing()
#if 0
{
    g_rtDescriptorRingReady = false;

    if(!rtDescriptorRingEnabledByEnv())
    {
        RTLog("[RT DESCRIPTOR] ring disabled by RTX_ATLAS_RING=0 (legacy device-idle path stays)");
        return;
    }

    if(
        g_device == VK_NULL_HANDLE ||
        g_rayTracingDescriptorSetLayout == VK_NULL_HANDLE ||
        g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
        g_queueFamily == UINT32_MAX
    )
    {
        return;
    }

    const VkDescriptorPoolSize ps[5] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 * RT_DESCRIPTOR_RING_SIZE },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16 * RT_DESCRIPTOR_RING_SIZE },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 * RT_DESCRIPTOR_RING_SIZE },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512 * RT_DESCRIPTOR_RING_SIZE },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 20 * RT_DESCRIPTOR_RING_SIZE }
    };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = RT_DESCRIPTOR_RING_SIZE;
    pi.poolSizeCount = 5;
    pi.pPoolSizes = ps;

    if(vkCreateDescriptorPool(g_device, &pi, nullptr, &g_rtDescriptorRingPool) != VK_SUCCESS)
    {
        RTLog("[RT DESCRIPTOR] ring pool create failed, keeping legacy idle path");
        return;
    }

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_rtDescriptorRingPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_rayTracingDescriptorSetLayout;

    VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    bool ok = true;
    for(uint32_t i = 0u; i < RT_DESCRIPTOR_RING_SIZE && ok; ++i)
    {
        if(
            vkAllocateDescriptorSets(g_device, &ai, &g_rtDescriptorRingSets[i]) != VK_SUCCESS ||
            vkCreateFence(g_device, &fi, nullptr, &g_rtDescriptorRingFences[i]) != VK_SUCCESS
        )
        {
            ok = false;
        }
    }

    if(ok)
    {
        VkCommandPoolCreateInfo cpi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = g_queueFamily;

        if(vkCreateCommandPool(g_device, &cpi, nullptr, &g_rtDescriptorRingCmdPool) != VK_SUCCESS)
        {
            ok = false;
        }
    }

    if(ok)
    {
        VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = g_rtDescriptorRingCmdPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1u;

        if(vkAllocateCommandBuffers(g_device, &cai, &g_rtDescriptorRingCmd) != VK_SUCCESS)
        {
            ok = false;
        }
    }

    if(ok)
    {
        g_rtDescriptorRingReady = rtDescriptorRingEnabledByEnv();
    }

    RTLog(
        std::string("[RT DESCRIPTOR] ring ready=") +
        (g_rtDescriptorRingReady ? "1" : "0") +
        " slots=" + std::to_string(RT_DESCRIPTOR_RING_SIZE) +
        (g_rtDescriptorRingReady ? "" : " (legacy device-idle path stays)")
    );
}
#endif
{}

// ---------------------------------------------------------------------------
// churn 发布的零等待路径。成功时 g_rayTracingDescriptorSet 已指向"整份拷贝自
// 旧 live"的新槽，调用方随后把脏 binding 写到它上面即可，无需任何等待；
// 返回 false = 未就绪/被 RTX_ATLAS_RING=0 关闭/环耗尽，调用方走 legacy
// device-idle 路径（行为与旧版逐位一致）。
// ---------------------------------------------------------------------------
bool rtDescriptorRingPublish()
#if 0
{
    g_rtDescriptorRingLastPublishUsedRing = false;

    if(!g_rtDescriptorRingReady)
    {
        return false;
    }

    for(uint32_t i = 0u; i < RT_DESCRIPTOR_RING_SIZE; ++i)
    {
        if(
            g_rtDescriptorRingFencePending[i] &&
            vkGetFenceStatus(g_device, g_rtDescriptorRingFences[i]) == VK_SUCCESS
        )
        {
            g_rtDescriptorRingFencePending[i] = false;
        }
    }

    uint32_t slot = UINT32_MAX;
    for(uint32_t n = 0u; n < RT_DESCRIPTOR_RING_SIZE; ++n)
    {
        const uint32_t i = (g_rtDescriptorRingNext + n) % RT_DESCRIPTOR_RING_SIZE;
        if(i == g_rtDescriptorRingLiveIndex || (g_rtDescriptorRingUsedMask & (1u << i)) != 0u)
        {
            continue;
        }
        slot = i;
        break;
    }

    if(slot == UINT32_MAX)
    {
        static uint32_t s_ringExhaustedLogged = 0u;
        if(s_ringExhaustedLogged < 8u)
        {
            ++s_ringExhaustedLogged;
            RTLog("[RT DESCRIPTOR] ring exhausted (bisect: no-submit mode), legacy idle");
        }
        return false;
    }

    VkDescriptorSet newSet = g_rtDescriptorRingSets[slot];

    VkCopyDescriptorSet copy{ VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET };
    copy.srcSet = g_rayTracingDescriptorSet;
    copy.dstSet = newSet;
    vkUpdateDescriptorSets(g_device, 0u, nullptr, 1u, &copy);

    g_rtDescriptorRingLastPublishUsedRing = false;
    return false;
}
#endif
{
    return false;
}
void createRayTracingDescriptorPoolAndSet()
{
    VkDescriptorPoolSize ps[5] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
        // 2026-11-04 A1：+2 = GI 时域历史 binding 86/87（20 → 22，原配额恰好用满）。
        // 2026-11-05 ③：+1 = K2-D/K2-I 直射级交接图 binding 89（22 → 23）。
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 23 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 512 },
        // 2026-11-05 B：+1 = K1/K2 landing SSBO（binding 88，21 → 22）。
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 22 }
    };
    VkDescriptorPoolCreateInfo pi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.maxSets = 1;
    pi.poolSizeCount = 5;
    pi.pPoolSizes = ps;

    check(vkCreateDescriptorPool(g_device, &pi, nullptr, &g_rayTracingDescriptorPool), "Failed to create RT descriptor pool");

    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = g_rayTracingDescriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_rayTracingDescriptorSetLayout;

    check(vkAllocateDescriptorSets(g_device, &ai, &g_rayTracingDescriptorSet), "Failed to allocate RT descriptor set");

    // createRtDescriptorRing() 已随 #13 退役（publish 零调用点 + bisect 残留），
    // 不再调用；空壳与 #if 0 原体留在上方。

    VkWriteDescriptorSetAccelerationStructureKHR asi{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    asi.accelerationStructureCount = 1;
    asi.pAccelerationStructures = &g_tlas;

    VkWriteDescriptorSet tlasWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    tlasWrite.pNext = &asi;
    tlasWrite.dstSet = g_rayTracingDescriptorSet;
    tlasWrite.dstBinding = 0;
    tlasWrite.descriptorCount = 1;
    tlasWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = g_rtOutputView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet imageWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    imageWrite.dstSet = g_rayTracingDescriptorSet;
    imageWrite.dstBinding = 1;
    imageWrite.descriptorCount = 1;
    imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    imageWrite.pImageInfo = &imageInfo;

    VkDescriptorBufferInfo uboInfo{};
    uboInfo.buffer = g_cameraUboBuffer;
    uboInfo.offset = 0;
    uboInfo.range = sizeof(CameraUBO);

    VkWriteDescriptorSet uboWrite{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    uboWrite.dstSet = g_rayTracingDescriptorSet;
    uboWrite.dstBinding = 2;
    uboWrite.descriptorCount = 1;
    uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboWrite.pBufferInfo = &uboInfo;


    VkWriteDescriptorSet writes[3] = { tlasWrite, imageWrite, uboWrite };
    vkUpdateDescriptorSets(g_device, 3, writes, 0, nullptr);

    g_tlasDescriptorHandle = g_tlas;

    createSectionVertexAddressTable();

    rebuildLocalLightGpuBuffer();
    createVLILightResources();

    ensureDynamicLightBuffer();
    updateDynamicLightDescriptor();

    ensureSharcBuffers();
    updateSharcDescriptors();
}


// 2026-10-07: one formula for the eight capacity-sized pool buffers, shared by
// createVLIPoolBuffers() and the copy-based growth migration.  Index order must
// match both call sites: page, scratch, surface cache, work list, emitter
// index, emitter alias, candidates, source directory.
VkDeviceSize poolByteSizeForCapacity(
    uint32_t capacity,
    uint32_t poolIndex
)
{
    const VkDeviceSize pages = static_cast<VkDeviceSize>(capacity);
    switch(poolIndex)
    {
        case 0u: return pages * VLI_PAGE_WORDS * sizeof(uint32_t);
        case 1u: return pages * VPT_DIRECTIONAL_CACHE_WORDS * sizeof(uint32_t);
        case 2u: return pages * VPT_SURFACE_FACES_PER_SECTION * sizeof(uint32_t);
        case 3u: return (pages + 1u) * sizeof(uint32_t);
        case 4u: return pages * VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t);
        case 5u: return pages * VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t);
        case 6u: return pages * VPT_RELATED_EMITTERS_PER_SECTION *
            sizeof(VPTRelatedEmitterGPU);
        case 7u: return pages * VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE *
            sizeof(VPTSourceDirectoryEntryGPU);
        default: return 0u;
    }
}


void updateVLIDescriptors()
{
    if(
        g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
        g_vliPageTableBuffer == VK_NULL_HANDLE ||
        g_vliPageBuffer == VK_NULL_HANDLE ||
        g_vliScratchBuffer == VK_NULL_HANDLE ||
        g_vliWorkListBuffer == VK_NULL_HANDLE ||
        g_vptEmitterIndexBuffer == VK_NULL_HANDLE ||
        g_vptEmitterAliasBuffer == VK_NULL_HANDLE ||
        g_vptCandidateBuffer == VK_NULL_HANDLE ||
        g_vptSourceDirectoryBuffer == VK_NULL_HANDLE ||
        g_vptSurfaceCacheBuffer == VK_NULL_HANDLE
    )
    {
        return;
    }

    VkDescriptorBufferInfo infos[9]{};
    infos[0] = { g_vliPageTableBuffer, 0, VK_WHOLE_SIZE };
    infos[1] = { g_vliPageBuffer, 0, VK_WHOLE_SIZE };
    infos[2] = { g_vliScratchBuffer, 0, VK_WHOLE_SIZE };
    infos[3] = { g_vliWorkListBuffer, 0, VK_WHOLE_SIZE };
    infos[4] = { g_vptEmitterIndexBuffer, 0, VK_WHOLE_SIZE };
    infos[5] = { g_vptCandidateBuffer, 0, VK_WHOLE_SIZE };
    infos[6] = { g_vptSurfaceCacheBuffer, 0, VK_WHOLE_SIZE };
    infos[7] = { g_vptSourceDirectoryBuffer, 0, VK_WHOLE_SIZE };
    infos[8] = { g_vptEmitterAliasBuffer, 0, VK_WHOLE_SIZE };

    VkWriteDescriptorSet writes[9]{};
    for(uint32_t i = 0; i < 9; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding =
            i < 4u ? 22u + i :
            (i == 4u ? 28u : (i == 5u ? 29u :
                (i == 6u ? 30u : (i == 7u ? 31u : 32u))));
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }

    vkUpdateDescriptorSets(g_device, 9, writes, 0, nullptr);
}


// Creates the pool buffers whose size is derived from g_vliPageCapacity.  The
// Section page table is fixed at VLI_PAGE_TABLE_CAPACITY entries and is created
// separately, because it addresses pages rather than bytes.
void createVLIPoolBuffers()
{
    const VkBufferUsageFlags storageUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    const VkDeviceSize pageCapacity =
        static_cast<VkDeviceSize>(g_vliPageCapacity);

    createBuffer(
        g_device,
        g_physicalDevice,
        pageCapacity * VLI_PAGE_WORDS * sizeof(uint32_t),
        storageUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_vliPageBuffer,
        g_vliPageMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        pageCapacity * VPT_DIRECTIONAL_CACHE_WORDS * sizeof(uint32_t),
        storageUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_vliScratchBuffer,
        g_vliScratchMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        pageCapacity * VPT_SURFACE_FACES_PER_SECTION * sizeof(uint32_t),
        storageUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_vptSurfaceCacheBuffer,
        g_vptSurfaceCacheMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        (1u + pageCapacity) * sizeof(uint32_t),
        storageUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_vliWorkListBuffer,
        g_vliWorkListMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        pageCapacity * VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t),
        storageUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_vptEmitterIndexBuffer,
        g_vptEmitterIndexMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        pageCapacity * VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t),
        storageUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_vptEmitterAliasBuffer,
        g_vptEmitterAliasMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        pageCapacity * VPT_RELATED_EMITTERS_PER_SECTION *
            sizeof(VPTRelatedEmitterGPU),
        storageUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_vptCandidateBuffer,
        g_vptCandidateMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        pageCapacity * VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE *
            sizeof(VPTSourceDirectoryEntryGPU),
        storageUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_vptSourceDirectoryBuffer,
        g_vptSourceDirectoryMemory
    );
}


// Destroys the capacity-sized pool buffers.  Only the device allocations are
// released; the CPU-side Section pages keep their payloads, which is what lets
// a growth re-upload them without any device-to-device copy.
void destroyVLIPoolBuffers()
{
    const auto destroyBuffer = [](VkBuffer& buffer, VkDeviceMemory& memory)
    {
        if(buffer != VK_NULL_HANDLE) vkDestroyBuffer(g_device, buffer, nullptr);
        if(memory != VK_NULL_HANDLE) vkFreeMemory(g_device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    };

    destroyBuffer(g_vliWorkListBuffer, g_vliWorkListMemory);
    destroyBuffer(g_vptSourceDirectoryBuffer, g_vptSourceDirectoryMemory);
    destroyBuffer(g_vptCandidateBuffer, g_vptCandidateMemory);
    destroyBuffer(g_vptEmitterAliasBuffer, g_vptEmitterAliasMemory);
    destroyBuffer(g_vptEmitterIndexBuffer, g_vptEmitterIndexMemory);
    destroyBuffer(g_vptSurfaceCacheBuffer, g_vptSurfaceCacheMemory);
    destroyBuffer(g_vliScratchBuffer, g_vliScratchMemory);
    destroyBuffer(g_vliPageBuffer, g_vliPageMemory);
}


void createVLILightResources()
{
    if(
        g_vliPageBuffer != VK_NULL_HANDLE ||
        g_device == VK_NULL_HANDLE ||
        g_physicalDevice == VK_NULL_HANDLE
    )
    {
        return;
    }

    const VkBufferUsageFlags storageUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    createBuffer(
        g_device,
        g_physicalDevice,
        static_cast<VkDeviceSize>(VLI_PAGE_TABLE_CAPACITY) * sizeof(VLITableSlot),
        storageUsage,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_vliPageTableBuffer,
        g_vliPageTableMemory
    );

    createVLIPoolBuffers();

    g_vliNeedsInitialClear = true;
    updateVLIDescriptors();

    std::stringstream ss;
    ss << "[VPT-3.1] exact Section emitter index allocated bytes="
       << static_cast<unsigned long long>(
            static_cast<VkDeviceSize>(g_vliPageCapacity) *
            VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t)
          )
       << " binding=28";
    RTLog(ss.str());
    std::stringstream candidateLog;
    candidateLog << "[VPT-3.2] bounded Section emitter candidates allocated bytes="
       << static_cast<unsigned long long>(
            static_cast<VkDeviceSize>(g_vliPageCapacity) *
            VPT_RELATED_EMITTERS_PER_SECTION *
            sizeof(VPTRelatedEmitterGPU)
          )
       << " binding=29";
    RTLog(candidateLog.str());
    std::stringstream sourceDirectoryLog;
    sourceDirectoryLog << "[VPT-3.3D.7] source-Section directory allocated bytes="
        << static_cast<unsigned long long>(
            static_cast<VkDeviceSize>(g_vliPageCapacity) *
            VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE *
            sizeof(VPTSourceDirectoryEntryGPU)
        )
        << " maxSections=" << VPT_SOURCE_DIRECTORY_MAX_SECTIONS
        << " binding=31";
    RTLog(sourceDirectoryLog.str());
    std::stringstream directionalCacheLog;
    directionalCacheLog << "[VPT-3.2B] directional cache allocated bytes="
        << static_cast<unsigned long long>(
            static_cast<VkDeviceSize>(g_vliPageCapacity) *
            VPT_DIRECTIONAL_CACHE_WORDS * sizeof(uint32_t)
        )
        << " directions=" << VPT_DIRECTION_COUNT
        << " binding=23";
    RTLog(directionalCacheLog.str());
    std::stringstream secondaryCacheLog;
    secondaryCacheLog << "[VPT-3.3] secondary directional cache allocated bytes="
        << static_cast<unsigned long long>(
            static_cast<VkDeviceSize>(g_vliPageCapacity) *
            VPT_DIRECTIONAL_CACHE_WORDS * sizeof(uint32_t)
        )
        << " directions=" << VPT_DIRECTION_COUNT
        << " binding=24";
    RTLog(secondaryCacheLog.str());
    std::stringstream surfaceCacheLog;
    surfaceCacheLog << "[VPT-3.3B.1] face cache allocated bytes="
        << static_cast<unsigned long long>(
            static_cast<VkDeviceSize>(g_vliPageCapacity) *
            VPT_SURFACE_FACES_PER_SECTION * sizeof(uint32_t)
        )
        << " facesPerPage=" << VPT_SURFACE_FACES_PER_SECTION
        << " binding=30";
    RTLog(surfaceCacheLog.str());
    std::stringstream poolLog;
    poolLog << "[VLI-1.4] persistent Section voxel buffers allocated capacity="
        << g_vliPageCapacity
        << " limit=" << vliPageCapacityLimit()
        << " bytes="
        << static_cast<unsigned long long>(
            vliPoolBytesForCapacity(g_vliPageCapacity)
          );
    RTLog(poolLog.str());
}


// Grows the persistent VLI/VPT page pool.  This runs on the render thread only:
// it is called from recordVLIUpdates() before that command buffer records any
// VLI/VPT work, so the buffers destroyed here are no longer referenced by an
// in-flight submission once the queue has drained.  Caller must hold g_vliMutex.
void growVLIPageCapacityLocked(uint32_t requiredPages)
{
    const uint32_t capacityLimit = vliPageCapacityLimit();

    if(g_vliPageCapacity >= capacityLimit)
    {
        return;
    }

    uint32_t newCapacity = g_vliPageCapacity;

    // 2026-11-06h：2x -> 4x。
    // 实测一局会触发 2 次增长（1024 -> 2048 -> 3253），每次在 g_vliMutex 内同步
    // 迁移整池并 vkQueueWaitIdle，持锁 17~22ms（[VLI LOCK] site=vliGrow）。
    // 4x 让它一步到 limit（1024 -> 4096，被 capacityLimit 截断），一局最多 1 次；
    // 最终占用不变（本来也会涨到 3253），只是少付一次迁移。
    while(newCapacity < requiredPages && newCapacity < capacityLimit)
    {
        newCapacity *= 4u;
    }
    if(newCapacity > capacityLimit)
    {
        newCapacity = capacityLimit;
    }
    if(newCapacity <= g_vliPageCapacity)
    {
        return;
    }

    PerfL10StallTimer perfL10StallTimer("vliPageGrow");

    const uint32_t previousCapacity = g_vliPageCapacity;
    const size_t livePages = g_vliSections.size();


    VkBuffer oldBuffers[8];
    VkDeviceMemory oldMemories[8];
    VkBuffer* const newBuffers[8] = {
        &g_vliPageBuffer,
        &g_vliScratchBuffer,
        &g_vptSurfaceCacheBuffer,
        &g_vliWorkListBuffer,
        &g_vptEmitterIndexBuffer,
        &g_vptEmitterAliasBuffer,
        &g_vptCandidateBuffer,
        &g_vptSourceDirectoryBuffer
    };
    VkDeviceMemory* const newMemories[8] = {
        &g_vliPageMemory,
        &g_vliScratchMemory,
        &g_vptSurfaceCacheMemory,
        &g_vliWorkListMemory,
        &g_vptEmitterIndexMemory,
        &g_vptEmitterAliasMemory,
        &g_vptCandidateMemory,
        &g_vptSourceDirectoryMemory
    };
    const VkDeviceSize oldPoolBytes[8] = {
        static_cast<VkDeviceSize>(previousCapacity) * VLI_PAGE_WORDS * sizeof(uint32_t),
        static_cast<VkDeviceSize>(previousCapacity) * VPT_DIRECTIONAL_CACHE_WORDS * sizeof(uint32_t),
        static_cast<VkDeviceSize>(previousCapacity) * VPT_SURFACE_FACES_PER_SECTION * sizeof(uint32_t),
        (static_cast<VkDeviceSize>(previousCapacity) + 1u) * sizeof(uint32_t),
        static_cast<VkDeviceSize>(previousCapacity) * VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t),
        static_cast<VkDeviceSize>(previousCapacity) * VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t),
        static_cast<VkDeviceSize>(previousCapacity) * VPT_RELATED_EMITTERS_PER_SECTION *
            sizeof(VPTRelatedEmitterGPU),
        static_cast<VkDeviceSize>(previousCapacity) * VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE *
            sizeof(VPTSourceDirectoryEntryGPU)
    };
    for(size_t pool = 0; pool < 8u; ++pool)
    {
        oldBuffers[pool] = *newBuffers[pool];
        oldMemories[pool] = *newMemories[pool];
        *newBuffers[pool] = VK_NULL_HANDLE;
        *newMemories[pool] = VK_NULL_HANDLE;
    }

    g_vliPageCapacity = newCapacity;
    g_vliPageSectionKeys.resize(newCapacity, VLI_INVALID_SECTION_KEY);
    createVLIPoolBuffers();

    VkCommandBufferAllocateInfo migrateAllocateInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    migrateAllocateInfo.commandPool = g_rtCommandPool;
    migrateAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    migrateAllocateInfo.commandBufferCount = 1u;

    VkCommandBuffer migrateCmd = VK_NULL_HANDLE;
    check(
        vkAllocateCommandBuffers(g_device, &migrateAllocateInfo, &migrateCmd),
        "allocate VLI pool migration command buffer failed"
    );

    VkCommandBufferBeginInfo migrateBeginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    migrateBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    check(
        vkBeginCommandBuffer(migrateCmd, &migrateBeginInfo),
        "begin VLI pool migration command buffer failed"
    );

    for(size_t pool = 0; pool < 8u; ++pool)
    {
        VkBufferCopy region{};
        region.srcOffset = 0u;
        region.dstOffset = 0u;
        region.size = oldPoolBytes[pool];
        vkCmdCopyBuffer(
            migrateCmd,
            oldBuffers[pool],
            *newBuffers[pool],
            1u,
            &region
        );

        const VkDeviceSize newBytes = poolByteSizeForCapacity(
            newCapacity,
            static_cast<uint32_t>(pool)
        );
        if(newBytes > oldPoolBytes[pool])
        {
            vkCmdFillBuffer(
                migrateCmd,
                *newBuffers[pool],
                oldPoolBytes[pool],
                newBytes - oldPoolBytes[pool],
                0u
            );
        }
    }

    VkMemoryBarrier migrateWriteBarrier{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER
    };
    migrateWriteBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    migrateWriteBarrier.dstAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(
        migrateCmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u,
        1u,
        &migrateWriteBarrier,
        0u,
        nullptr,
        0u,
        nullptr
    );

    vkCmdFillBuffer(migrateCmd, g_vliWorkListBuffer, 0u, sizeof(uint32_t), 0u);

    check(
        vkEndCommandBuffer(migrateCmd),
        "end VLI pool migration command buffer failed"
    );

    VkSubmitInfo migrateSubmitInfo{
        VK_STRUCTURE_TYPE_SUBMIT_INFO
    };
    migrateSubmitInfo.commandBufferCount = 1u;
    migrateSubmitInfo.pCommandBuffers = &migrateCmd;

    check(
        vkQueueSubmit(g_queue, 1u, &migrateSubmitInfo, VK_NULL_HANDLE),
        "submit VLI pool migration failed"
    );

    check(
        vkQueueWaitIdle(g_queue),
        "wait RT queue after VLI pool migration failed"
    );

    vkFreeCommandBuffers(
        g_device,
        g_rtCommandPool,
        1u,
        &migrateCmd
    );

    for(size_t pool = 0; pool < 8u; ++pool)
    {
        if(oldBuffers[pool] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(g_device, oldBuffers[pool], nullptr);
        }
        if(oldMemories[pool] != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, oldMemories[pool], nullptr);
        }
    }

    updateVLIDescriptors();

    std::stringstream ss;
    ss << "[VLI-1.4 GROW] page capacity "
       << previousCapacity << " -> " << g_vliPageCapacity
       << " livePages=" << livePages
       << " limit=" << vliPageCapacityLimit()
       << " migratedBy=deviceCopy"
       << " poolBytes="
       << static_cast<unsigned long long>(
            vliPoolBytesForCapacity(g_vliPageCapacity)
          );
    RTLog(ss.str());
}


uint32_t vliHashSectionCoord(
    int32_t x,
    int32_t y,
    int32_t z
)
{
    return localLightGridHash32(x, y, z);
}


uint32_t vliFindTableSlot(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    bool& found
)
{
    const uint32_t mask = VLI_PAGE_TABLE_CAPACITY - 1u;
    uint32_t slot = vliHashSectionCoord(sectionX, sectionY, sectionZ) & mask;

    for(uint32_t probe = 0; probe < VLI_PAGE_TABLE_CAPACITY; ++probe)
    {
        const VLITableSlot& entry = g_vliTableCpu[slot];
        if(entry.pagePlusOne == 0u)
        {
            found = false;
            return slot;
        }

        if(
            entry.sectionX == sectionX &&
            entry.sectionY == sectionY &&
            entry.sectionZ == sectionZ
        )
        {
            found = true;
            return slot;
        }

        slot = (slot + 1u) & mask;
    }

    throw std::runtime_error("VLI Section page table is full");
}

template <typename Visit>
static void forEachVPTNeighborhoodSection(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    Visit&& visit
)
{
    constexpr int32_t radius = static_cast<int32_t>(VPT_SOURCE_DIRECTORY_RADIUS);

    for(int32_t offsetX = -radius; offsetX <= radius; ++offsetX)
    {
        for(int32_t offsetY = -radius; offsetY <= radius; ++offsetY)
        {
            for(int32_t offsetZ = -radius; offsetZ <= radius; ++offsetZ)
            {
                const auto it = g_vliSections.find(
                    packSectionKey(
                        sectionX + offsetX,
                        sectionY + offsetY,
                        sectionZ + offsetZ,
                        0
                    )
                );

                if(it != g_vliSections.end())
                {
                    visit(it->second);
                }
            }
        }
    }
}


void markVLINeighborhoodDirty(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ
)
{
    forEachVPTNeighborhoodSection(
        sectionX,
        sectionY,
        sectionZ,
        [](VLISectionCpuPage& page)
        {
            g_vliDirtySolveTableSlots.insert(page.tableSlot);
        }
    );
}


std::vector<uint32_t> buildVPTEmitterIndexWords(
    const std::vector<uint32_t>& sourceWords,
    uint32_t revision
)
{
    std::vector<uint32_t> emitterWords(VPT_EMITTER_PAGE_WORDS, 0u);
    emitterWords[1] = revision;

    uint32_t emitterCount = 0u;
    const uint32_t sourceCount = static_cast<uint32_t>(
        std::min<size_t>(sourceWords.size(), VLI_SOURCE_WORDS)
    );

    for(uint32_t sourceIndex = 0u; sourceIndex < sourceCount; ++sourceIndex)
    {
        const uint32_t source = sourceWords[sourceIndex];
        const uint32_t emission = (source >> 2u) & 15u;
        if(emission == 0u)
        {
            continue;
        }

        const uint32_t localX = sourceIndex & 15u;
        const uint32_t localZ = (sourceIndex >> 4u) & 15u;
        const uint32_t localY = (sourceIndex >> 8u) & 15u;
        // 2026-10-31：光源档案 id 扩到 5 bit（bits 6..10），见
        // local_light_profile.glsl 与 SectionCompilerMixin 的体素字布局。
        const uint32_t typeCode = (source >> 6u) & 31u;

        const uint32_t packedEmitter =
            localX |
            (localY << 4u) |
            (localZ << 8u) |
            (emission << 12u) |
            (typeCode << 16u);

        emitterWords[VPT_EMITTER_HEADER_WORDS + emitterCount] = packedEmitter;
        ++emitterCount;
    }

    emitterWords[0] = emitterCount;
    uint32_t totalEmissionPower = 0u;
    for(uint32_t emitterIndex = 0u; emitterIndex < emitterCount; ++emitterIndex)
    {
        totalEmissionPower += (emitterWords[VPT_EMITTER_HEADER_WORDS + emitterIndex] >> 12u) & 15u;
    }
    emitterWords[2] = totalEmissionPower;
    return emitterWords;
}

// VPT-3.3D.8: Binding 17's Section light records are the authoritative
// source of local emitters.  VLI sourceWords encode voxel occupancy/albedo
// for DDA and face exposure, but their emission bits are not populated by the
// current Java VLI upload.  Build the VPT exact emitter page from the same
// accepted records that already light ordinary opaque geometry.
std::vector<uint32_t> buildVPTEmitterIndexWordsFromLocalLights(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    const std::vector<LocalLightRecord>& lights,
    uint32_t revision
)
{
    std::vector<uint32_t> emitterWords(VPT_EMITTER_PAGE_WORDS, 0u);
    emitterWords[1] = revision;

    uint32_t emitterCount = 0u;
    uint32_t totalEmissionPower = 0u;
    const int32_t baseX = sectionX * 16;
    const int32_t baseY = sectionY * 16;
    const int32_t baseZ = sectionZ * 16;

    for(const LocalLightRecord& light : lights)
    {
        if(
            light.emissionLevel <= 0 ||
            light.emissionLevel > 15 ||
            emitterCount >= VPT_MAX_EMITTERS_PER_SECTION
        )
        {
            continue;
        }

        const int32_t localX = light.worldX - baseX;
        const int32_t localY = light.worldY - baseY;
        const int32_t localZ = light.worldZ - baseZ;
        if(
            localX < 0 || localX >= 16 ||
            localY < 0 || localY >= 16 ||
            localZ < 0 || localZ >= 16
        )
        {
            continue;
        }

        const uint32_t emission = static_cast<uint32_t>(light.emissionLevel);
        const uint32_t typeCode = static_cast<uint32_t>(
            std::clamp(light.typeCode, 0, 31)
        );
        emitterWords[VPT_EMITTER_HEADER_WORDS + emitterCount] =
            static_cast<uint32_t>(localX) |
            (static_cast<uint32_t>(localY) << 4u) |
            (static_cast<uint32_t>(localZ) << 8u) |
            (emission << 12u) |
            (typeCode << 16u);
        ++emitterCount;
        totalEmissionPower += emission;
    }

    emitterWords[0] = emitterCount;
    emitterWords[2] = totalEmissionPower;
    return emitterWords;
}
std::vector<uint32_t> buildVPTEmitterIndexWordsForSection(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    const std::vector<uint32_t>& sourceWords,
    const std::vector<LocalLightRecord>& localLights,
    uint32_t revision
)
{
    return localLights.empty()
        ? buildVPTEmitterIndexWords(sourceWords, revision)
        : buildVPTEmitterIndexWordsFromLocalLights(
            sectionX,
            sectionY,
            sectionZ,
            localLights,
            revision
        );
}

// One uint per compact emitter: low 16 bits are the Alias threshold and high
// 16 bits are the fallback emitter index.  The proposal probability itself is
// exactly emission / emitterWords[2], so it is never quantized.
std::vector<uint32_t> buildVPTEmitterAliasWords(
    const std::vector<uint32_t>& emitterWords
)
{
    std::vector<uint32_t> aliasWords(VPT_EMITTER_PAGE_WORDS, 0u);
    if(emitterWords.size() != VPT_EMITTER_PAGE_WORDS) return aliasWords;

    const uint32_t count = std::min(emitterWords[0], VPT_MAX_EMITTERS_PER_SECTION);
    const uint32_t totalPower = emitterWords[2];
    aliasWords[0] = count;
    aliasWords[1] = totalPower;
    if(count == 0u || totalPower == 0u) return aliasWords;

    std::vector<double> scaled(count, 0.0);
    std::deque<uint32_t> small;
    std::deque<uint32_t> large;
    for(uint32_t i = 0u; i < count; ++i)
    {
        const uint32_t power = (emitterWords[VPT_EMITTER_HEADER_WORDS + i] >> 12u) & 15u;
        scaled[i] = static_cast<double>(power) * static_cast<double>(count) /
                    static_cast<double>(totalPower);
        (scaled[i] < 1.0 ? small : large).push_back(i);
    }

    while(!small.empty() && !large.empty())
    {
        const uint32_t s = small.front(); small.pop_front();
        const uint32_t l = large.front(); large.pop_front();
        const uint32_t threshold = static_cast<uint32_t>(std::clamp(
            std::lround(scaled[s] * 65535.0), 0l, 65535l));
        aliasWords[VPT_EMITTER_HEADER_WORDS + s] = threshold | (l << 16u);
        scaled[l] = (scaled[l] + scaled[s]) - 1.0;
        (scaled[l] < 1.0 ? small : large).push_back(l);
    }
    while(!small.empty())
    {
        const uint32_t i = small.front(); small.pop_front();
        aliasWords[VPT_EMITTER_HEADER_WORDS + i] = 65535u | (i << 16u);
    }
    while(!large.empty())
    {
        const uint32_t i = large.front(); large.pop_front();
        aliasWords[VPT_EMITTER_HEADER_WORDS + i] = 65535u | (i << 16u);
    }
    return aliasWords;
}


float vptDistanceSquaredToSectionBounds(
    const VPTRelatedEmitterGPU& emitter,
    const VLISectionCpuPage& receiver
)
{
    const float minX = static_cast<float>(receiver.sectionX * 16);
    const float minY = static_cast<float>(receiver.sectionY * 16);
    const float minZ = static_cast<float>(receiver.sectionZ * 16);
    const float maxX = minX + 16.0f;
    const float maxY = minY + 16.0f;
    const float maxZ = minZ + 16.0f;

    const float x = static_cast<float>(emitter.worldX) + 0.5f;
    const float y = static_cast<float>(emitter.worldY) + 0.5f;
    const float z = static_cast<float>(emitter.worldZ) + 0.5f;

    const float dx = x < minX ? minX - x : (x > maxX ? x - maxX : 0.0f);
    const float dy = y < minY ? minY - y : (y > maxY ? y - maxY : 0.0f);
    const float dz = z < minZ ? minZ - z : (z > maxZ ? z - maxZ : 0.0f);
    return dx * dx + dy * dy + dz * dz;
}


uint32_t packVPTSourceDirectoryMetadata(
    uint32_t emitterCount,
    int32_t offsetX,
    int32_t offsetY,
    int32_t offsetZ
)
{
    return (std::min(emitterCount, VPT_MAX_EMITTERS_PER_SECTION) & 0x1fffu) |
        ((static_cast<uint32_t>(offsetX + 2) & 7u) << 13u) |
        ((static_cast<uint32_t>(offsetY + 2) & 7u) << 16u) |
        ((static_cast<uint32_t>(offsetZ + 2) & 7u) << 19u);
}


void rebuildVPTSourceDirectoryPageLocked(VLISectionCpuPage& receiver)
{
    std::array<VPTSourceDirectoryEntryGPU,
        VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE> directory{};
    uint32_t directoryCount = 0u;
    std::array<double, VPT_SOURCE_DIRECTORY_MAX_SECTIONS> weights{};

    forEachVPTNeighborhoodSection(
        receiver.sectionX,
        receiver.sectionY,
        receiver.sectionZ,
        [&](VLISectionCpuPage& sourcePage)
        {
            if(sourcePage.emitterIndexWords.size() < VPT_EMITTER_HEADER_WORDS)
            {
                return;
            }

            const uint32_t emitterCount = std::min(
                sourcePage.emitterIndexWords[0],
                VPT_MAX_EMITTERS_PER_SECTION
            );
            if(emitterCount == 0u ||
               directoryCount >= VPT_SOURCE_DIRECTORY_MAX_SECTIONS)
            {
                return;
            }

            const int32_t offsetX = sourcePage.sectionX - receiver.sectionX;
            const int32_t offsetY = sourcePage.sectionY - receiver.sectionY;
            const int32_t offsetZ = sourcePage.sectionZ - receiver.sectionZ;

            VPTSourceDirectoryEntryGPU& destination =
                directory[1u + directoryCount++];
            destination.pageIndex = sourcePage.pageIndex;
            destination.emitterCountAndOffset = packVPTSourceDirectoryMetadata(
                emitterCount, offsetX, offsetY, offsetZ
            );
            const uint32_t sectionPower = sourcePage.emitterIndexWords.size() > 2u
                ? sourcePage.emitterIndexWords[2] : 0u;
            const float dx = std::max(0, std::abs(offsetX) - 1) * 16.0f;
            const float dy = std::max(0, std::abs(offsetY) - 1) * 16.0f;
            const float dz = std::max(0, std::abs(offsetZ) - 1) * 16.0f;
            weights[directoryCount - 1u] = static_cast<double>(sectionPower) /
                static_cast<double>(1.0f + dx * dx + dy * dy + dz * dz);
        }
    );

    directory[0].pageIndex = directoryCount;

    const uint32_t directoryRevision = g_vptSourceDirectoryRevision++;
    if(g_vptSourceDirectoryRevision == 0u ||
       g_vptSourceDirectoryRevision > 0x00ffffffu)
    {
        g_vptSourceDirectoryRevision = 1u;
    }
    directory[0].aliasThresholdQ16 = directoryRevision;

    if(directoryCount > 0u)
    {
        double totalWeight = 0.0;
        for(uint32_t i = 0u; i < directoryCount; ++i) totalWeight += weights[i];
        if(totalWeight > 0.0)
        {
            std::array<double, VPT_SOURCE_DIRECTORY_MAX_SECTIONS> scaled{};
            std::deque<uint32_t> small;
            std::deque<uint32_t> large;
            for(uint32_t i = 0u; i < directoryCount; ++i)
            {
                directory[1u + i].selectionProbability =
                    static_cast<float>(weights[i] / totalWeight);
                scaled[i] = weights[i] * static_cast<double>(directoryCount) / totalWeight;
                (scaled[i] < 1.0 ? small : large).push_back(i);
            }
            while(!small.empty() && !large.empty())
            {
                const uint32_t s = small.front(); small.pop_front();
                const uint32_t l = large.front(); large.pop_front();
                directory[1u + s].aliasThresholdQ16 = static_cast<uint32_t>(std::clamp(
                    std::lround(scaled[s] * 65535.0), 0l, 65535l));
                directory[1u + s].aliasIndex = l;
                scaled[l] = (scaled[l] + scaled[s]) - 1.0;
                (scaled[l] < 1.0 ? small : large).push_back(l);
            }
            while(!small.empty())
            {
                const uint32_t i = small.front(); small.pop_front();
                directory[1u + i].aliasThresholdQ16 = 65535u;
                directory[1u + i].aliasIndex = i;
            }
            while(!large.empty())
            {
                const uint32_t i = large.front(); large.pop_front();
                directory[1u + i].aliasThresholdQ16 = 65535u;
                directory[1u + i].aliasIndex = i;
            }
        }
    }
    receiver.sourceDirectory = directory;
    g_vptDirtySourceDirectoryPages.insert(receiver.pageIndex);
}


void rebuildVPTCandidatePageLocked(VLISectionCpuPage& receiver)
{
    std::array<VPTRelatedEmitterGPU, VPT_RELATED_EMITTERS_PER_SECTION>
        candidates{};
    std::array<float, VPT_RELATED_EMITTERS_PER_SECTION> scores{};
    scores.fill(-1.0f);

    forEachVPTNeighborhoodSection(
        receiver.sectionX,
        receiver.sectionY,
        receiver.sectionZ,
        [&](VLISectionCpuPage& sourcePage)
        {
            if(sourcePage.emitterIndexWords.size() < VPT_EMITTER_HEADER_WORDS)
            {
                return;
            }

            const uint32_t emitterCount = std::min(
                sourcePage.emitterIndexWords[0],
                VPT_MAX_EMITTERS_PER_SECTION
            );
            for(uint32_t emitterIndex = 0u;
                emitterIndex < emitterCount;
                ++emitterIndex)
            {
                const uint32_t packed = sourcePage.emitterIndexWords[
                    VPT_EMITTER_HEADER_WORDS + emitterIndex
                ];
                const uint32_t emission = (packed >> 12u) & 15u;
                if(emission == 0u)
                {
                    continue;
                }

                VPTRelatedEmitterGPU candidate{};
                candidate.worldX = sourcePage.sectionX * 16 +
                    static_cast<int32_t>(packed & 15u);
                candidate.worldY = sourcePage.sectionY * 16 +
                    static_cast<int32_t>((packed >> 4u) & 15u);
                candidate.worldZ = sourcePage.sectionZ * 16 +
                    static_cast<int32_t>((packed >> 8u) & 15u);
                candidate.emissionAndType = emission |
                    (((packed >> 16u) & 31u) << 4u);

                const float score = static_cast<float>(emission) /
                    (1.0f + vptDistanceSquaredToSectionBounds(candidate, receiver));
                if(score <= scores[VPT_RELATED_EMITTERS_PER_SECTION - 1u])
                {
                    continue;
                }

                uint32_t insertAt = VPT_RELATED_EMITTERS_PER_SECTION - 1u;
                while(insertAt > 0u && score > scores[insertAt - 1u])
                {
                    scores[insertAt] = scores[insertAt - 1u];
                    candidates[insertAt] = candidates[insertAt - 1u];
                    --insertAt;
                }
                scores[insertAt] = score;
                candidates[insertAt] = candidate;
            }
        }
    );

    receiver.relatedEmitters = candidates;
    g_vptDirtyCandidatePages.insert(receiver.pageIndex);
    g_vliDirtySolveTableSlots.insert(receiver.tableSlot);
}


// 2026-11-06e：邻域重建的去重版本（配合 applyVLISectionVoxelsLocked 的
// deferredNeighborhoodPages 参数）。
//
// 为什么需要：rebuildVPTCandidateNeighborhoodLocked 是 125 个邻域 Section，
// 每个再走 rebuildVPTSourceDirectoryPageLocked（内部又扫 125）⇒ 单次约 15625 次
// 哈希查找。一帧批量 apply N 个相邻 Section 时，邻域重叠高达 64/125，等于重复
// 做 N 遍。这里先按 pageIndex 去重收集，再统一重建一次。
static void collectVPTNeighborhoodPageIndices(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    std::unordered_set<uint32_t>& outPageIndices
)
{
    forEachVPTNeighborhoodSection(
        sectionX,
        sectionY,
        sectionZ,
        [&outPageIndices](VLISectionCpuPage& page)
        {
            outPageIndices.insert(page.pageIndex);
        }
    );
}
static void rebuildVPTNeighborhoodPagesLocked(
    const std::unordered_set<uint32_t>& pageIndices
)
{
    for(const uint32_t pageIndex : pageIndices)
    {
        if(pageIndex >= g_vliPageSectionKeys.size())
        {
            continue;
        }

        const uint64_t sectionKey = g_vliPageSectionKeys[pageIndex];

        if(sectionKey == VLI_INVALID_SECTION_KEY)
        {
            continue;
        }

        const auto pageIt = g_vliSections.find(sectionKey);

        if(pageIt == g_vliSections.end())
        {
            continue;
        }

        rebuildVPTCandidatePageLocked(pageIt->second);
        rebuildVPTSourceDirectoryPageLocked(pageIt->second);
    }
}

// 2026-11-06h：延后到帧内统一重建的邻域页集合。
// 生产者是 refreshVPTEmitterSectionFromLocalLights（Java 回调线程，持 g_vliMutex），
// 消费者是 recordVLIUpdates（渲染线程，持 g_vliMutex）。用 g_vliMutex 保护，
// 另用一个原子标志避免空闲帧白拿一次锁。
static std::unordered_set<uint32_t> s_deferredVptNeighborhoodPages;
static std::atomic<bool> s_hasDeferredVptNeighborhoodPages{ false };

// 2026-11-06i：定义在本文件靠后（约 3000 行），而调用点在 applyVLISectionVoxelsLocked，
// 需要提前声明。
static bool evictFarthestVLIPageForIncomingLocked(
    uint32_t& outPageIndex,
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ
);

// 2026-11-06m???????????recordVLIUpdates ??????
static void drainVLIEvictGarbage();

void rebuildVPTCandidateNeighborhoodLocked(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ
)
{
    forEachVPTNeighborhoodSection(
        sectionX,
        sectionY,
        sectionZ,
        [](VLISectionCpuPage& receiver)
        {
            rebuildVPTCandidatePageLocked(receiver);
            rebuildVPTSourceDirectoryPageLocked(receiver);
        }
    );
}

// Registers or refreshes one VLI Section page and marks the affected
// neighborhood dirty.  Caller must hold g_vliMutex and must already have built
// the emitter payloads.  Returns false when a new page is required while the
// pool is at its current capacity: the payloads are then untouched and the
// caller has to park the Section until the render thread has grown the pool.
bool applyVLISectionVoxelsLocked(
    uint64_t sectionKey,
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    std::vector<uint32_t>& incoming,
    std::vector<uint32_t>& emitterIndexWords,
    std::vector<uint32_t>& emitterAliasWords,
    std::unordered_set<uint32_t>* deferredNeighborhoodPages
)
{
    uint32_t pageIndex = 0u;
    const auto pageIt = g_vliSections.find(sectionKey);

    if(pageIt != g_vliSections.end())
    {
        pageIt->second.sourceWords = std::move(incoming);
        pageIt->second.emitterIndexWords = std::move(emitterIndexWords);
        pageIt->second.emitterAliasWords = std::move(emitterAliasWords);
        pageIndex = pageIt->second.pageIndex;

        pageIt->second.lastUseTick = ++g_vliPageUseTick;
    }
    else
    {
        if(!g_vliFreePages.empty())
        {
            pageIndex = g_vliFreePages.back();
            g_vliFreePages.pop_back();
        }
        else
        {
            if(g_vliNextPage >= g_vliPageCapacity)
            {
                uint32_t evictedPageIndex = 0u;

                // 2026-11-06i：判据从「显存是否超预算」改为「最远页是否比新段更远」。
                // 淘汰不释放显存（页号被新 Section 复用），所以 over 门本来就不解决
                // 显存问题，只导致 over=0 时新 Section 被永久丢弃（远处光照缺失）。
                if(!evictFarthestVLIPageForIncomingLocked(
                       evictedPageIndex,
                       sectionX,
                       sectionY,
                       sectionZ))
                {
                    ++g_vliGovernorIgnoredSections;
                    return false;
                }

                ++g_vliGovernorEvictedPages;
                pageIndex = evictedPageIndex;
            }
            else
            {
                pageIndex = g_vliNextPage++;
            }
        }

        bool found = false;
        const uint32_t tableSlot = vliFindTableSlot(sectionX, sectionY, sectionZ, found);
        if(found)
        {
            RTLog("[VLI-1.4 SOURCE] table/map mismatch; Section ignored");
            return true;
        }

        VLITableSlot& slot = g_vliTableCpu[tableSlot];
        slot.sectionX = sectionX;
        slot.sectionY = sectionY;
        slot.sectionZ = sectionZ;
        slot.pagePlusOne = pageIndex + 1u;
        g_vliDirtyTableSlots.insert(tableSlot);

        VLISectionCpuPage page{};
        page.sectionX = sectionX;
        page.sectionY = sectionY;
        page.sectionZ = sectionZ;
        page.pageIndex = pageIndex;
        page.tableSlot = tableSlot;
        page.lastUseTick = ++g_vliPageUseTick;
        page.sourceWords = std::move(incoming);
        page.emitterIndexWords = std::move(emitterIndexWords);
        page.emitterAliasWords = std::move(emitterAliasWords);
        g_vliSections.emplace(sectionKey, std::move(page));
        g_vliPageSectionKeys[pageIndex] = sectionKey;
        vliChunkIndexAdd(sectionX, sectionZ, sectionKey);
    }

    g_vliDirtySourcePages.insert(pageIndex);
    g_vptDirtyEmitterPages.insert(pageIndex);
    g_vptDirtyEmitterAliasPages.insert(pageIndex);
    markVLINeighborhoodDirty(sectionX, sectionY, sectionZ);

    if(deferredNeighborhoodPages != nullptr)
    {
        // 批量路径：只收集受影响的邻域页，由调用方在循环结束后统一重建一次。
        collectVPTNeighborhoodPageIndices(
            sectionX,
            sectionY,
            sectionZ,
            *deferredNeighborhoodPages);
    }
    else
    {
        rebuildVPTCandidateNeighborhoodLocked(sectionX, sectionY, sectionZ);
    }

    return true;
}

// Local-light and voxel uploads are independent Java callbacks.  Refresh the
// VPT emitter representation when a light Section changes, but only after
// the light-cache mutex has been released; this keeps the global lock order
// one-way and lets either callback arrive first.
void refreshVPTEmitterSectionFromLocalLights(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    const std::vector<LocalLightRecord>& localLights
)
{
    const uint64_t sectionKey = packSectionKey(sectionX, sectionY, sectionZ, 0);
    uint32_t pageIndex = 0u;
    uint32_t emitterCount = 0u;
    bool hasVoxelPage = false;

    {
        VliLockHoldProbe vliLockHoldProbe("workerApply");
        std::lock_guard<std::mutex> lock(g_vliMutex);
        vliLockHoldProbe.mark();
        auto pageIt = g_vliSections.find(sectionKey);
        if(pageIt == g_vliSections.end())
        {
            return;
        }

        uint32_t emitterRevision = g_vptEmitterRevision++;
        if(g_vptEmitterRevision == 0u)
        {
            g_vptEmitterRevision = 1u;
        }

        std::vector<uint32_t> emitterIndexWords =
            buildVPTEmitterIndexWordsForSection(
                sectionX,
                sectionY,
                sectionZ,
                pageIt->second.sourceWords,
                localLights,
                emitterRevision
            );

        emitterCount = emitterIndexWords[0];
        pageIt->second.emitterIndexWords = std::move(emitterIndexWords);
        pageIt->second.emitterAliasWords =
            buildVPTEmitterAliasWords(pageIt->second.emitterIndexWords);
        pageIndex = pageIt->second.pageIndex;
        hasVoxelPage = true;

        g_vptDirtyEmitterPages.insert(pageIndex);
        g_vptDirtyEmitterAliasPages.insert(pageIndex);
        markVLINeighborhoodDirty(sectionX, sectionY, sectionZ);

        // 2026-11-06h：不在这里做 125x125 邻域重建。这条路径由 Java 逐 Section
        // 调用且持有 g_vliMutex，是这个锁 wait 的常态来源（site=workerApply）。
        // 只收集受影响的页，帧内 recordVLIUpdates 统一去重重建一次。
        collectVPTNeighborhoodPageIndices(
            sectionX,
            sectionY,
            sectionZ,
            s_deferredVptNeighborhoodPages);
        s_hasDeferredVptNeighborhoodPages.store(true, std::memory_order_release);
    }

    if(hasVoxelPage)
    {
        if constexpr(PERF_L10_ENABLE_LIGHT_REFRESH_LOGS)
        {
            std::stringstream ss;
            ss << "[VPT-3.3D.8 EMITTER BRIDGE] section=("
               << sectionX << "," << sectionY << "," << sectionZ << ")"
               << " local=" << localLights.size()
               << " vpt=" << emitterCount
               << " page=" << pageIndex;
            RTLog(ss.str());
        }
    }
}


// 2026-10-22：VLI（体素光注入）分帧预算。
//
// 现场（2026-10-22 21:51~21:54 那局，3.5 分钟）：[RT VLI SLOW]（阈值 8ms）
// 触发 **105 次**，平均 16.93ms、中位 12.54ms、最大 57.84ms，其中 >=20ms 有 25 次。
// 同一局 GPU 只有 7.87ms/帧、几何单段构建只有 0.1ms ⇒ 卡顿几乎全部来自这一条。
//
// 根因：recordVLIUpdates 一次把 g_vliPendingSections 里**全部**待处理 Section
// 做完（每段还要 buildVPTEmitterIndexWordsForSection + buildVPTEmitterAliasWords
// + applyVLISectionVoxelsLocked）。新区块流入时它们挤在同一帧，直接顶穿帧时间。
//
// 做法：加"至少做 1 段，之后按时间/段数收手"的闸门 —— 与 SectionMemoryPool、
// 几何预算同一套思路（**延后，不丢弃**）。剩下的留在 pending 队列里下一帧继续；
// 它本来就是队列，跨帧天然安全（容量在 growVLIPageCapacityLocked 里已按全部
// pending 预留）。
//
//   ORION_VLI_BUDGET_MS=<ms>          默认 4；0 = 不分帧（逐位回到旧行为）
//   ORION_VLI_SECTIONS_PER_FRAME=<n>  默认 2；0 = 不限段数（只按时间闸）
double vliPendingSectionBudgetMs()
{
    static const double cached = []()
    {
        const char* raw = std::getenv("ORION_VLI_BUDGET_MS");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 4.0;
        }

        const double parsed = std::atof(raw);

        if(parsed <= 0.0)
        {
            return 0.0;
        }

        return parsed > 50.0 ? 50.0 : parsed;
    }();

    return cached;
}
uint32_t vliPendingSectionLimit()
{
    static const uint32_t cached = []() -> uint32_t
    {
        const char* raw = std::getenv("ORION_VLI_SECTIONS_PER_FRAME");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 2u;
        }

        const long parsed = std::atol(raw);

        if(parsed <= 0)
        {
            return 0u;
        }

        return static_cast<uint32_t>(parsed > 64 ? 64 : parsed);
    }();

    return cached;
}
void beginVliUploadArena()
{
    g_vliUploadArena.clear();
    g_vliSourceUploads.clear();
    g_vliEmitterUploads.clear();
    g_vliEmitterAliasUploads.clear();
}
void appendVliUpload(
    std::vector<VliUploadRef>& refs,
    uint32_t pageIndex,
    const std::vector<uint32_t>& words)
{
    VliUploadRef ref;
    ref.pageIndex = pageIndex;
    ref.wordOffset = static_cast<uint32_t>(g_vliUploadArena.size());
    ref.wordCount = static_cast<uint32_t>(words.size());

    g_vliUploadArena.insert(
        g_vliUploadArena.end(),
        words.begin(),
        words.end()
    );

    refs.push_back(ref);
}
void recordVLIUpdates(VkCommandBuffer cmd)
{
    if(
        cmd == VK_NULL_HANDLE ||
        g_vliPageTableBuffer == VK_NULL_HANDLE ||
        g_vliPageBuffer == VK_NULL_HANDLE ||
        g_vliScratchBuffer == VK_NULL_HANDLE ||
        g_vptEmitterIndexBuffer == VK_NULL_HANDLE ||
        g_vptEmitterAliasBuffer == VK_NULL_HANDLE ||
        g_vptCandidateBuffer == VK_NULL_HANDLE ||
        g_vptSourceDirectoryBuffer == VK_NULL_HANDLE ||
        g_vptSurfaceCacheBuffer == VK_NULL_HANDLE ||
        g_vliComputePipeline == VK_NULL_HANDLE ||
        g_vliComputePipelineLayout == VK_NULL_HANDLE
    )
    {
        return;
    }

    g_vliMsIndex = 0.0;
    g_vliMsAlias = 0.0;
    g_vliMsApply = 0.0;
    g_vliMsGather = 0.0;
    g_vliMsUpload = 0.0;

    // 2026-11-06m：这里在取 g_vliMutex 之前，回收上一帧淘汰摘下的页（锁外析构）。
    drainVLIEvictGarbage();

    if(g_vliGrowthRequested)
    {
        // 2026-10-31（C）：增长放在"安静帧"做。
        //
        // 增长全程在渲染线程 + g_vliMutex 内：分配 8 个 device-local buffer、
        // vkCmdCopyBuffer 迁移整个旧池、vkQueueWaitIdle、销毁旧 buffer。实测持锁
        // 8.55ms / 22.46ms（[VLI LOCK] site=vliGrow），期间 VLI worker 排队 12ms、
        // 渲染线程 [RT VLI SLOW] wait 15.8ms。而它触发的时机恰好是几何流入最猛的
        // 那几帧（进世界后 21s / 38s）。
        //
        // 判据（都不需要长时间持锁）：本帧没有待处理 Section、几何没有被预算打断、
        // 也没有新增脏段。三个条件任一不满足就先推迟 —— pending 段本来就停在 park
        // 队列里，跨帧安全，最多晚一点注入局部光照，不会丢。
        //
        // 兜底：连续推迟 ORION_VLI_GROW_DEFER_FRAMES 帧（默认 30，0 = 行为回到旧版）
        // 之后强制做一次，避免世界持续流式时永远等不到安静帧。
        static uint32_t s_growDeferFrames = 0u;

        static const uint32_t s_growDeferLimit = []() -> uint32_t
        {
            const char* raw = std::getenv("ORION_VLI_GROW_DEFER_FRAMES");

            if(raw == nullptr || raw[0] == '\0')
            {
                return 30u;
            }

            const long parsed = std::strtol(raw, nullptr, 10);

            return parsed < 0
                ? 30u
                : static_cast<uint32_t>(std::min<long>(parsed, 600));
        }();

        bool vliNoPendingSections = false;
        {
            // 只为了读一个 empty()，持锁时间可忽略；不持锁读 vector 是数据竞争
            // （上传线程在 g_vliMutex 下 push_back，会 realloc）。
            std::lock_guard<std::mutex> peekLock(g_vliMutex);
            vliNoPendingSections = g_vliPendingSections.empty();
        }

        const bool vliGrowWindowQuiet =
            vliNoPendingSections &&
            !g_sectionRebuildInterrupted &&
            !g_worldGeometryDirty.load(std::memory_order_acquire);

        if(!vliGrowWindowQuiet && s_growDeferFrames < s_growDeferLimit)
        {
            ++s_growDeferFrames;
        }
        else
        {
            s_growDeferFrames = 0u;

            VliLockHoldProbe vliLockHoldProbe("vliGrow");
            std::lock_guard<std::mutex> lock(g_vliMutex);
            vliLockHoldProbe.mark();

            if(g_vliGrowthRequested)
            {
                g_vliGrowthRequested = false;
                growVLIPageCapacityLocked(
                    g_vliNextPage +
                    static_cast<uint32_t>(g_vliPendingSections.size())
                );
            }
        }
    }

    if(!g_vliPendingSections.empty())
    {
        VliLockHoldProbe vliLockHoldProbe("pending");
        std::lock_guard<std::mutex> lock(g_vliMutex);
        vliLockHoldProbe.mark();

        const double vliPendingBudgetMs = vliPendingSectionBudgetMs();
        const uint32_t vliPendingLimit = vliPendingSectionLimit();
        const auto vliPendingBegin = std::chrono::steady_clock::now();

        size_t vliPendingProcessed = 0u;

        // 2026-11-06e：本帧受影响的邻域页（按 pageIndex 去重），循环结束后统一重建。
        std::unordered_set<uint32_t> deferredNeighborhoodPages;

        for(;
            vliPendingProcessed < g_vliPendingSections.size();
            ++vliPendingProcessed)
        {
            if(vliPendingProcessed > 0u)
            {
                if(vliPendingLimit != 0u &&
                   vliPendingProcessed >= vliPendingLimit)
                {
                    break;
                }

                if(vliPendingBudgetMs > 0.0)
                {
                    const double vliPendingElapsedMs =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            vliPendingBegin
                        ).count();

                    if(vliPendingElapsedMs >= vliPendingBudgetMs)
                    {
                        break;
                    }
                }
            }

            VLIPendingSection& pending =
                g_vliPendingSections[vliPendingProcessed];

            std::vector<uint32_t> incoming = std::move(pending.sourceWords);
            std::vector<LocalLightRecord> pendingLights = std::move(pending.lights);

            const uint32_t emitterRevision = g_vptEmitterRevision++;
            if(g_vptEmitterRevision == 0u)
            {
                g_vptEmitterRevision = 1u;
            }
            const auto vliIdxBegin = std::chrono::steady_clock::now();

            std::vector<uint32_t> emitterIndexWords =
                buildVPTEmitterIndexWordsForSection(
                    pending.sectionX,
                    pending.sectionY,
                    pending.sectionZ,
                    incoming,
                    pendingLights,
                    emitterRevision
                );

            const auto vliAliasBegin = std::chrono::steady_clock::now();

            g_vliMsIndex +=
                std::chrono::duration<double, std::milli>(
                    vliAliasBegin - vliIdxBegin
                ).count();

            std::vector<uint32_t> emitterAliasWords =
                buildVPTEmitterAliasWords(emitterIndexWords);

            const auto vliApplyBegin = std::chrono::steady_clock::now();

            g_vliMsAlias +=
                std::chrono::duration<double, std::milli>(
                    vliApplyBegin - vliAliasBegin
                ).count();

            if(!applyVLISectionVoxelsLocked(
                   pending.sectionKey,
                   pending.sectionX,
                   pending.sectionY,
                   pending.sectionZ,
                   incoming,
                   emitterIndexWords,
                   emitterAliasWords,
                   &deferredNeighborhoodPages
               ))
            {
                // 2026-11-06i：改成距离判据后，这条表示「池满 且 新段比最远页更远」，
                // 属于正常丢弃（实测 2.5 分钟 1211 次），限频到每 600 次一条。
                static uint32_t s_exhaustedLogCount = 0u;

                if((++s_exhaustedLogCount % 600u) == 1u)
                {
                    RTLog(
                        "[VLI-1.4 SOURCE] page pool full; farther Section ignored"
                        " total=" + std::to_string(s_exhaustedLogCount));
                }
            }

            g_vliMsApply +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - vliApplyBegin
                ).count();
        }

        // 2026-11-06e：批量 apply 结束后，按去重后的页集合统一重建一次邻域。
        // 原来每段都重建一次 125x125，相邻段邻域重叠 64/125 ⇒ 重复 N 遍。
        //
        // 2026-11-06f：计入 g_vliMsApply。之前这段在 apply 循环之外、也没被任何
        // 计时区间包住，全部落进 [RT VLI SLOW] 的 rest 残差里（实测 rest 22~28ms
        // 就是这么来的），从日志看像是"时间凭空消失"。
        if(!deferredNeighborhoodPages.empty())
        {
            const auto vliDedupBegin = std::chrono::steady_clock::now();

            rebuildVPTNeighborhoodPagesLocked(deferredNeighborhoodPages);

            g_vliMsApply +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - vliDedupBegin
                ).count();

            deferredNeighborhoodPages.clear();
        }

        if(vliPendingProcessed > 0u)
        {
            g_vliPendingSections.erase(
                g_vliPendingSections.begin(),
                g_vliPendingSections.begin() + vliPendingProcessed
            );
        }

        g_vliPendingBacklogForLog = g_vliPendingSections.size();
    }

    // 2026-11-06h：把 Java 回调线程延后的邻域重建在这里做完（按页去重一次）。
    // 必须早于下面的 upload 采集 —— 候选/目录就是在采集后被上传到 GPU 的。
    if(s_hasDeferredVptNeighborhoodPages.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> deferredLock(g_vliMutex);

        if(!s_deferredVptNeighborhoodPages.empty())
        {
            // 2026-11-06i：计入 apply，别让它再落进 rest 残差。
            const auto vliDeferredBegin = std::chrono::steady_clock::now();

            rebuildVPTNeighborhoodPagesLocked(s_deferredVptNeighborhoodPages);
            s_deferredVptNeighborhoodPages.clear();

            g_vliMsApply +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - vliDeferredBegin
                ).count();
        }

        s_hasDeferredVptNeighborhoodPages.store(false, std::memory_order_release);
    }

    const auto vliBreakdownGatherBegin =
        std::chrono::steady_clock::now();

    beginVliUploadArena();

    std::vector<VliUploadRef>& sourceUploads = g_vliSourceUploads;
    std::vector<VliUploadRef>& emitterUploads = g_vliEmitterUploads;
    std::vector<VliUploadRef>& emitterAliasUploads = g_vliEmitterAliasUploads;
    std::vector<std::pair<uint32_t,
        std::array<VPTRelatedEmitterGPU, VPT_RELATED_EMITTERS_PER_SECTION>>>
        candidateUploads;
    std::vector<std::pair<uint32_t, std::array<VPTSourceDirectoryEntryGPU,
        VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE>>> sourceDirectoryUploads;
    std::vector<uint32_t> tableUpdates;
    std::vector<std::pair<uint32_t, VLITableSlot>> tablePayloads;
    std::vector<std::pair<uint32_t, uint32_t>> solvePages;

    {
        VliLockHoldProbe vliLockHoldProbe("gather");
        std::lock_guard<std::mutex> lock(g_vliMutex);
        vliLockHoldProbe.mark();

        g_vliMsLockWait =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() -
                vliBreakdownGatherBegin
            ).count();

        if(
            !g_vliNeedsInitialClear &&
            g_vliDirtySourcePages.empty() &&
            g_vliDirtyTableSlots.empty() &&
            g_vliDirtySolveTableSlots.empty() &&
            g_vptDirtyEmitterPages.empty() &&
            g_vptDirtyEmitterAliasPages.empty() &&
            g_vptDirtyCandidatePages.empty() &&
            g_vptDirtySourceDirectoryPages.empty()
        )
        {
            return;
        }

        // 2026-11-07d：每帧最多搬 N 个源页（每页 16KB）。没搬完的留脏，下一帧继续。
        static const size_t sGatherSourcePageBudget = []() -> size_t
        {
            const char* raw = std::getenv("ORION_VLI_GATHER_PAGES");

            if(raw == nullptr || raw[0] == '\0')
            {
                return 384u;
            }

            const long parsed = std::atol(raw);

            if(parsed <= 0)
            {
                return 384u;
            }

            return static_cast<size_t>(parsed > 4096 ? 4096 : parsed);
        }();

        sourceUploads.reserve(
            std::min(g_vliDirtySourcePages.size(), sGatherSourcePageBudget));

        size_t gatheredSourcePages = 0u;

        for(auto it = g_vliDirtySourcePages.begin();
            it != g_vliDirtySourcePages.end() &&
            gatheredSourcePages < sGatherSourcePageBudget;)
        {
            const uint32_t pageIndex = *it;
            const VLISectionCpuPage* const page =
                findVLISectionPageByIndexLocked(pageIndex);

            if(page != nullptr)
            {
                appendVliUpload(
                    sourceUploads,
                    pageIndex,
                    page->sourceWords
                );
            }

            it = g_vliDirtySourcePages.erase(it);
            ++gatheredSourcePages;
        }


        emitterUploads.reserve(g_vptDirtyEmitterPages.size());
        for(uint32_t pageIndex : g_vptDirtyEmitterPages)
        {
            const VLISectionCpuPage* const page =
                findVLISectionPageByIndexLocked(pageIndex);
            if(page != nullptr)
            {
                appendVliUpload(
                    emitterUploads,
                    pageIndex,
                    page->emitterIndexWords
                );
            }
        }

        emitterAliasUploads.reserve(g_vptDirtyEmitterAliasPages.size());
        for(uint32_t pageIndex : g_vptDirtyEmitterAliasPages)
        {
            const VLISectionCpuPage* const page =
                findVLISectionPageByIndexLocked(pageIndex);
            if(page != nullptr)
            {
                appendVliUpload(
                    emitterAliasUploads,
                    pageIndex,
                    page->emitterAliasWords
                );
            }
        }

        candidateUploads.reserve(g_vptDirtyCandidatePages.size());
        for(uint32_t pageIndex : g_vptDirtyCandidatePages)
        {
            const VLISectionCpuPage* const page =
                findVLISectionPageByIndexLocked(pageIndex);
            if(page != nullptr)
            {
                candidateUploads.emplace_back(
                    pageIndex,
                    page->relatedEmitters
                );
            }
        }

        sourceDirectoryUploads.reserve(g_vptDirtySourceDirectoryPages.size());
        for(uint32_t pageIndex : g_vptDirtySourceDirectoryPages)
        {
            const VLISectionCpuPage* const page =
                findVLISectionPageByIndexLocked(pageIndex);
            if(page != nullptr)
            {
                sourceDirectoryUploads.emplace_back(
                    pageIndex,
                    page->sourceDirectory
                );
            }
        }

        tableUpdates.assign(
            g_vliDirtyTableSlots.begin(),
            g_vliDirtyTableSlots.end()
        );

        tablePayloads.reserve(tableUpdates.size());
        for(uint32_t tableSlot : tableUpdates)
        {
            tablePayloads.emplace_back(tableSlot, g_vliTableCpu[tableSlot]);
        }

        solvePages.reserve(g_vliDirtySolveTableSlots.size());
        for(uint32_t tableSlot : g_vliDirtySolveTableSlots)
        {
            const uint32_t pagePlusOne = g_vliTableCpu[tableSlot].pagePlusOne;
            if(pagePlusOne != 0u)
            {
                solvePages.emplace_back(tableSlot, pagePlusOne - 1u);
            }
        }

        // 2026-11-07e：无条件清空（源页已在上面按预算逐页 erase）。
        // 这些集合每帧都会重新收集；若像 07d 那样"排空才清"，稳态下它们永不
        // 清空 ⇒ 每帧全量重做，gather 从 n=2 恶化到 n=179。
        g_vliDirtyTableSlots.clear();
        g_vliDirtySolveTableSlots.clear();
        g_vptDirtyEmitterPages.clear();
        g_vptDirtyEmitterAliasPages.clear();
        g_vptDirtyCandidatePages.clear();
        g_vptDirtySourceDirectoryPages.clear();
    }

    g_vliGatherPages =
        tableUpdates.size() +
        sourceUploads.size() +
        emitterUploads.size() +
        emitterAliasUploads.size() +
        candidateUploads.size() +
        sourceDirectoryUploads.size();

    g_vliMsGather =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() -
            vliBreakdownGatherBegin
        ).count() -
        g_vliMsLockWait;

    const auto vliBreakdownUploadBegin =
        std::chrono::steady_clock::now();

    const bool didInitialClear = g_vliNeedsInitialClear;
    if(didInitialClear)
    {
        vkCmdFillBuffer(cmd, g_vliPageTableBuffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, g_vliPageBuffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, g_vliScratchBuffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, g_vptSurfaceCacheBuffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, g_vliWorkListBuffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, g_vptEmitterIndexBuffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, g_vptEmitterAliasBuffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, g_vptCandidateBuffer, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, g_vptSourceDirectoryBuffer, 0, VK_WHOLE_SIZE, 0u);
        g_vliNeedsInitialClear = false;
    }

    for(const auto& update : tablePayloads)
    {
        vkCmdUpdateBuffer(
            cmd,
            g_vliPageTableBuffer,
            static_cast<VkDeviceSize>(update.first) * sizeof(VLITableSlot),
            sizeof(VLITableSlot),
            &update.second
        );
    }

    for(const VliUploadRef& upload : sourceUploads)
    {
        vkCmdUpdateBuffer(
            cmd,
            g_vliPageBuffer,
            static_cast<VkDeviceSize>(upload.pageIndex) *
                VLI_PAGE_WORDS * sizeof(uint32_t),
            static_cast<VkDeviceSize>(upload.wordCount) * sizeof(uint32_t),
            g_vliUploadArena.data() + upload.wordOffset
        );
    }

    uint64_t uploadedEmitterCount = 0u;
    for(const VliUploadRef& upload : emitterUploads)
    {
        if(upload.wordCount != VPT_EMITTER_PAGE_WORDS)
        {
            continue;
        }

        uploadedEmitterCount += g_vliUploadArena[upload.wordOffset];
        vkCmdUpdateBuffer(
            cmd,
            g_vptEmitterIndexBuffer,
            static_cast<VkDeviceSize>(upload.pageIndex) *
                VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t),
            static_cast<VkDeviceSize>(VPT_EMITTER_PAGE_WORDS) * sizeof(uint32_t),
            g_vliUploadArena.data() + upload.wordOffset
        );
    }

    static uint32_t vptPageLogThrottle = 0u;
    const bool vptLogDue = (vptPageLogThrottle++ % 120u) == 0u;

    if(!emitterUploads.empty() && vptLogDue)
    {
        std::stringstream ss;
        ss << "[VPT-3.1 INDEX] pages=" << emitterUploads.size()
           << " emitters=" << uploadedEmitterCount
           << " bytes="
           << static_cast<unsigned long long>(
                emitterUploads.size() *
                static_cast<size_t>(VPT_EMITTER_PAGE_WORDS) *
                sizeof(uint32_t)
              )
           << " binding=28";
        RTLog(ss.str());
    }

    for(const VliUploadRef& upload : emitterAliasUploads)
    {
        if(upload.wordCount != VPT_EMITTER_PAGE_WORDS) continue;
        vkCmdUpdateBuffer(
            cmd,
            g_vptEmitterAliasBuffer,
            static_cast<VkDeviceSize>(upload.pageIndex) *
                VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t),
            static_cast<VkDeviceSize>(VPT_EMITTER_PAGE_WORDS) * sizeof(uint32_t),
            g_vliUploadArena.data() + upload.wordOffset
        );
    }

    uint64_t uploadedCandidateCount = 0u;
    for(const auto& upload : candidateUploads)
    {
        for(const VPTRelatedEmitterGPU& candidate : upload.second)
        {
            if((candidate.emissionAndType & 15u) != 0u)
            {
                ++uploadedCandidateCount;
            }
        }

        vkCmdUpdateBuffer(
            cmd,
            g_vptCandidateBuffer,
            static_cast<VkDeviceSize>(upload.first) *
                VPT_RELATED_EMITTERS_PER_SECTION *
                sizeof(VPTRelatedEmitterGPU),
            sizeof(upload.second),
            upload.second.data()
        );
    }

    if(!candidateUploads.empty() && vptLogDue)
    {
        std::stringstream ss;
        ss << "[VPT-3.2 CANDIDATES] pages=" << candidateUploads.size()
           << " entries=" << uploadedCandidateCount
           << " maxPerPage=" << VPT_RELATED_EMITTERS_PER_SECTION
           << " binding=29";
        RTLog(ss.str());
    }

    uint64_t uploadedDirectorySources = 0u;
    for(const auto& upload : sourceDirectoryUploads)
    {
        uploadedDirectorySources += upload.second[0].pageIndex;
        vkCmdUpdateBuffer(
            cmd,
            g_vptSourceDirectoryBuffer,
            static_cast<VkDeviceSize>(upload.first) *
                VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE *
                sizeof(VPTSourceDirectoryEntryGPU),
            sizeof(upload.second),
            upload.second.data()
        );
    }

    if(!sourceDirectoryUploads.empty() && vptLogDue)
    {
        std::stringstream ss;
        ss << "[VPT-3.3D.7 DIRECTORY] pages="
           << sourceDirectoryUploads.size()
           << " sourceSections=" << uploadedDirectorySources
           << " maxPerPage=" << VPT_SOURCE_DIRECTORY_MAX_SECTIONS
           << " binding=31";
        RTLog(ss.str());
    }

    if(!tableUpdates.empty() || !sourceUploads.empty() ||
       !emitterUploads.empty() || !emitterAliasUploads.empty() || !candidateUploads.empty() ||
       !sourceDirectoryUploads.empty())
    {
        g_vptDirectHistoryNeedsClear = true;
    }

    g_vliMsUpload =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() -
            vliBreakdownUploadBegin
        ).count();

    const auto vliBreakdownSolveBegin =
        std::chrono::steady_clock::now();

    std::sort(
        solvePages.begin(),
        solvePages.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; }
    );
    solvePages.erase(
        std::unique(solvePages.begin(), solvePages.end()),
        solvePages.end()
    );

    if(solvePages.empty())
    {
        if(
            !tableUpdates.empty() ||
            !sourceUploads.empty() ||
            !emitterUploads.empty() ||
            !candidateUploads.empty() ||
            !sourceDirectoryUploads.empty() ||
            didInitialClear
        )
        {
            VkMemoryBarrier rayReadBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            rayReadBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            rayReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                0,
                1,
                &rayReadBarrier,
                0,
                nullptr,
                0,
                nullptr
            );
        }
        return;
    }

    std::vector<uint32_t> workList(1u + solvePages.size());
    workList[0] = static_cast<uint32_t>(solvePages.size());
    for(size_t i = 0; i < solvePages.size(); ++i)
    {
        workList[1u + i] = solvePages[i].first;
    }

    vkCmdUpdateBuffer(
        cmd,
        g_vliWorkListBuffer,
        0,
        static_cast<VkDeviceSize>(workList.size()) * sizeof(uint32_t),
        workList.data()
    );

    VkMemoryBarrier workBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    workBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    workBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        1,
        &workBarrier,
        0,
        nullptr,
        0,
        nullptr
    );

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_vliComputePipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_vliComputePipelineLayout,
        0,
        1,
        &g_rayTracingDescriptorSet,
        0,
        nullptr
    );

    const uint64_t totalSurfaceFaces =
        static_cast<uint64_t>(solvePages.size()) * VPT_SURFACE_FACES_PER_SECTION;
    const uint32_t surfaceDispatchX =
        static_cast<uint32_t>((totalSurfaceFaces + 63u) / 64u);

    VPTPushConstants pushConstants{};
    pushConstants.eventSerial = g_vptTemporalFrame++;
    pushConstants.passIndex = 1u;
    vkCmdPushConstants(cmd, g_vliComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0, sizeof(VPTPushConstants), &pushConstants);
    vkCmdDispatch(cmd, surfaceDispatchX, 1, 1);

    VkMemoryBarrier computeToRayBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    computeToRayBarrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    computeToRayBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0,
        1,
        &computeToRayBarrier,
        0,
        nullptr,
        0,
        nullptr
    );

    g_vliMsSolve =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() -
            vliBreakdownSolveBegin
        ).count();
}
void createVLIComputePipeline()
{
    if(g_vliComputePipeline != VK_NULL_HANDLE)
    {
        return;
    }

    const std::vector<uint8_t> code = loadSpv("shaders/rt_vpt_irradiance.comp.spv");
    VkShaderModule shaderModule = createShaderModule(g_device, code);

    try
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(VPTPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &g_rayTracingDescriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        check(
            vkCreatePipelineLayout(g_device, &layoutInfo, nullptr, &g_vliComputePipelineLayout),
            "VLI-1.4 create compute pipeline layout failed"
        );
        VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shaderModule;
        stage.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipelineInfo.stage = stage;
        pipelineInfo.layout = g_vliComputePipelineLayout;
        check(
            vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1, &pipelineInfo, nullptr, &g_vliComputePipeline),
            "VLI-1.4 create compute pipeline failed"
        );
    }
    catch(...)
    {
        if(g_vliComputePipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(g_device, g_vliComputePipelineLayout, nullptr);
            g_vliComputePipelineLayout = VK_NULL_HANDLE;
        }
        vkDestroyShaderModule(g_device, shaderModule, nullptr);
        throw;
    }

    vkDestroyShaderModule(g_device, shaderModule, nullptr);
    RTLog("[VLI-1.4] compute pipeline created");
}


void destroyVLIResources()
{
    if(g_vliComputePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_vliComputePipeline, nullptr);
        g_vliComputePipeline = VK_NULL_HANDLE;
    }
    if(g_vliComputePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_vliComputePipelineLayout, nullptr);
        g_vliComputePipelineLayout = VK_NULL_HANDLE;
    }

    destroyVLIPoolBuffers();

    if(g_vliPageTableBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_vliPageTableBuffer, nullptr);
        g_vliPageTableBuffer = VK_NULL_HANDLE;
    }
    if(g_vliPageTableMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_vliPageTableMemory, nullptr);
        g_vliPageTableMemory = VK_NULL_HANDLE;
    }

    g_vliPendingSections.clear();
    g_vliGrowthRequested = false;
    g_vliNeedsInitialClear = true;
}
void updateRTOutputDescriptor()
{
    if(!g_rayTracingDescriptorSet ||
       !g_rtOutputView)
    {
        return;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = g_rtOutputView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(
        g_device,
        1,
        &write,
        0,
        nullptr
    );
}


void clearVLISections()
{
    VliLockHoldProbe vliLockHoldProbe("worldClear");
    std::lock_guard<std::mutex> lock(g_vliMutex);
    vliLockHoldProbe.mark();
    g_vliSections.clear();
    g_vliChunkSectionIndex.clear();

    {
        std::lock_guard<std::mutex> uploadHashLock(g_vliUploadHashMutex);
        g_vliUploadHashByKey.clear();
    }
    std::fill(g_vliTableCpu.begin(), g_vliTableCpu.end(), VLITableSlot{});
    std::fill(
        g_vliPageSectionKeys.begin(),
        g_vliPageSectionKeys.end(),
        VLI_INVALID_SECTION_KEY
    );
    {
        std::stringstream pageDemandLog;

        pageDemandLog
            << "[VLI PAGE DEMAND]"
            << " peakPages=" << g_vliNextPage
            << " capacity=" << g_vliPageCapacity
            << " limit=" << vliPageCapacityLimit()
            << " governorEvicted=" << g_vliGovernorEvictedPages
            << " governorIgnored=" << g_vliGovernorIgnoredSections
            << " livePages=" << g_vliSections.size()
            << " freePages=" << g_vliFreePages.size()
            << " poolBytes="
            << static_cast<unsigned long long>(
                vliPoolBytesForCapacity(g_vliPageCapacity)
               )
            << " renderRes=" << g_rtOutputWidth
            << "x" << g_rtOutputHeight;

        RTLog(pageDemandLog.str());
    }

    g_vliPendingSections.clear();
    g_vliGrowthRequested = false;
    g_vliFreePages.clear();
    g_vliNextPage = 0u;
    g_vliDirtySourcePages.clear();
    g_vliDirtyTableSlots.clear();
    g_vliDirtySolveTableSlots.clear();
    g_vptDirtyEmitterPages.clear();
    g_vptDirtyEmitterAliasPages.clear();
    g_vptDirtyCandidatePages.clear();
    g_vptDirtySourceDirectoryPages.clear();
    g_vptSourceDirectoryRevision = 1u;
    g_vptEmitterRevision = 1u;
    g_vptTemporalFrame = 0u;
    g_vliNeedsInitialClear = true;
}
void eraseVLITableSlot(uint32_t eraseSlot)
{
    const uint32_t mask = VLI_PAGE_TABLE_CAPACITY - 1u;
    g_vliTableCpu[eraseSlot] = {};
    g_vliDirtyTableSlots.insert(eraseSlot);

    uint32_t cursor = (eraseSlot + 1u) & mask;
    while(g_vliTableCpu[cursor].pagePlusOne != 0u)
    {
        const VLITableSlot displaced = g_vliTableCpu[cursor];
        g_vliTableCpu[cursor] = {};
        g_vliDirtyTableSlots.insert(cursor);

        bool found = false;
        const uint32_t newSlot = vliFindTableSlot(
            displaced.sectionX,
            displaced.sectionY,
            displaced.sectionZ,
            found
        );
        if(found)
        {
            throw std::runtime_error("VLI table reinsertion unexpectedly found an entry");
        }

        g_vliTableCpu[newSlot] = displaced;
        g_vliDirtyTableSlots.insert(newSlot);

        const uint64_t sectionKey = packSectionKey(
            displaced.sectionX,
            displaced.sectionY,
            displaced.sectionZ,
            0
        );
        const auto pageIt = g_vliSections.find(sectionKey);
        if(pageIt != g_vliSections.end())
        {
            pageIt->second.tableSlot = newSlot;
            g_vliDirtySolveTableSlots.insert(newSlot);
        }

        ++g_vliTableReinsertTotal;
        cursor = (cursor + 1u) & mask;
    }
}


// =====================================================
// 2026-10-13：淘汰"离玩家最远"的一页 VLI 页（甲·二级动作）
//
// 触发点只有一个：applyVLISectionVoxelsLocked 发现池满、**且**闸门判定超预算。
// 旧行为是把**新来的** Section 丢掉（"page capacity exhausted; Section ignored"），
// 于是池满之后永久保留最先注册的那批页 —— 它们通常离玩家最远，而**近处的光进不来**。
//
// 淘汰序列与按列卸载（clearChunkLayers 里那一段）完全一致，只是把"这一列全删"换成
// "删最远的一页"，不引入新的生命周期约定：
//   ① 从 g_vliSections 摘掉 → ② 清页索引表 → ③ eraseVLITableSlot（含后移重插）
//   → ④ 邻域置脏 + 重建 VPT 候选邻域
// ⚠ 顺序：必须**先**从 map 摘掉再 eraseVLITableSlot —— 后者会对被后移的条目回查
//   g_vliSections 并改写它们的 tableSlot，被淘汰的那条不能还留在表里。
//
// 页号不推回 g_vliFreePages：调用点会立刻把它让给新 Section（省一次往返）。
// 池内的旧字节不动 —— 表项已清，着色器查不到这一页，不会读到它们。
//
// 距离判据用玩家位置（g_cameraUboMapped 是常驻映射的 CPU 副本，见 14440），
// Section 中心 = sectionCoord * 16 + 8。距离相同时用 lastUseTick 打破平局（更旧者先出）。
//
// Caller must hold g_vliMutex.
// =====================================================
// 2026-11-06m：淘汰摘下的页先缓存，由调用方在锁外清空（析构）。
// thread_local：淘汰可能发生在渲染线程（apply 循环）或 JNI 线程（上传回调），
// 各自独立，不需要额外加锁。
static thread_local std::vector<VLISectionCpuPage> s_vliEvictGarbage;

// 必须在 g_vliMutex 之外调用：这里才真正析构那些大对象。
static void drainVLIEvictGarbage()
{
    s_vliEvictGarbage.clear();
}

// 2026-11-06i：带「距离闸」的内部实现。hasIncomingSection 为真时，只有当池里
// 最远的页严格比新来的 Section 更远，才允许淘汰 —— 否则换掉它只是抖动。
// 淘汰本身不释放显存（页号立刻被新 Section 复用），所以不该用 over 当判据。
static bool evictFarthestVLIPageImpl(
    uint32_t& outPageIndex,
    bool hasIncomingSection,
    int32_t incomingSectionX,
    int32_t incomingSectionY,
    int32_t incomingSectionZ
)
{
    outPageIndex = 0u;

    if(g_vliSections.empty())
    {
        return false;
    }

    const CameraUBO* ubo =
        g_cameraUboMapped != nullptr
            ? static_cast<const CameraUBO*>(g_cameraUboMapped)
            : nullptr;

    const float centerX = ubo != nullptr ? ubo->playerX : 0.0f;
    const float centerY = ubo != nullptr ? ubo->playerY : 0.0f;
    const float centerZ = ubo != nullptr ? ubo->playerZ : 0.0f;

    uint64_t victimKey = VLI_INVALID_SECTION_KEY;
    uint32_t victimPageIndex = 0u;
    uint32_t victimTableSlot = 0u;
    int32_t victimX = 0;
    int32_t victimY = 0;
    int32_t victimZ = 0;
    float victimDistanceSq = 0.0f;
    uint64_t victimTick = 0u;

    // 2026-11-06j：等距抽样，避免每帧在锁内全表扫描 3000+ 页。
    // 页数 3253 时 stride=13、候选约 250 个；判据只要求「比新段更远」，
    // 近似取样不影响正确性（最坏是本次不淘汰，退回旧行为：丢弃新 Section）。
    constexpr size_t VLI_EVICT_SCAN_LIMIT = 256u;

    size_t scanStride = 1u;

    if(g_vliSections.size() > VLI_EVICT_SCAN_LIMIT)
    {
        scanStride =
            (g_vliSections.size() + VLI_EVICT_SCAN_LIMIT - 1u) / VLI_EVICT_SCAN_LIMIT;
    }

    size_t scanIndex = 0u;

    for(const auto& entry : g_vliSections)
    {
        if(scanStride > 1u && (scanIndex++ % scanStride) != 0u)
        {
            continue;
        }

        const VLISectionCpuPage& page = entry.second;

        const float posX = static_cast<float>(page.sectionX) * 16.0f + 8.0f;
        const float posY = static_cast<float>(page.sectionY) * 16.0f + 8.0f;
        const float posZ = static_cast<float>(page.sectionZ) * 16.0f + 8.0f;

        const float dx = posX - centerX;
        const float dy = posY - centerY;
        const float dz = posZ - centerZ;
        const float distanceSq = dx * dx + dy * dy + dz * dz;

        bool better = false;

        if(victimKey == VLI_INVALID_SECTION_KEY)
        {
            better = true;
        }
        else if(distanceSq > victimDistanceSq)
        {
            better = true;
        }
        else if(distanceSq == victimDistanceSq && page.lastUseTick < victimTick)
        {
            better = true;
        }

        if(better)
        {
            victimKey = entry.first;
            victimPageIndex = page.pageIndex;
            victimTableSlot = page.tableSlot;
            victimX = page.sectionX;
            victimY = page.sectionY;
            victimZ = page.sectionZ;
            victimDistanceSq = distanceSq;
            victimTick = page.lastUseTick;
        }
    }

    // 2026-11-06i：新 Section 不比最远页更近 ⇒ 不值得换（换了下一帧又换回来）。
    if(hasIncomingSection && victimKey != VLI_INVALID_SECTION_KEY)
    {
        const float incomingPosX =
            static_cast<float>(incomingSectionX) * 16.0f + 8.0f;
        const float incomingPosY =
            static_cast<float>(incomingSectionY) * 16.0f + 8.0f;
        const float incomingPosZ =
            static_cast<float>(incomingSectionZ) * 16.0f + 8.0f;

        const float inDx = incomingPosX - centerX;
        const float inDy = incomingPosY - centerY;
        const float inDz = incomingPosZ - centerZ;
        const float incomingDistanceSq = inDx * inDx + inDy * inDy + inDz * inDz;

        if(victimDistanceSq <= incomingDistanceSq)
        {
            return false;
        }
    }

    if(victimKey == VLI_INVALID_SECTION_KEY ||
       victimPageIndex >= g_vliPageSectionKeys.size())
    {
        return false;
    }

    // 2026-11-06m：与列卸载同因 —— 节点内联着整个 VLISectionCpuPage，
    // erase 会在锁内析构 180µs 级的大块。先 move 出来（只搬指针），锁外销毁。
    {
        const auto victimIt = g_vliSections.find(victimKey);

        if(victimIt != g_vliSections.end())
        {
            s_vliEvictGarbage.push_back(std::move(victimIt->second));

            // JNI 线程可能在两个 drain 之间攒很多，兜底：攒到 16 个就地清一次。
            if(s_vliEvictGarbage.size() >= 16u)
            {
                s_vliEvictGarbage.clear();
            }
        }
    }

    g_vliSections.erase(victimKey);
    vliChunkIndexRemove(victimX, victimZ, victimKey);
    vliUploadHashForget(victimKey);

    g_vliPageSectionKeys[victimPageIndex] = VLI_INVALID_SECTION_KEY;

    eraseVLITableSlot(victimTableSlot);

    markVLINeighborhoodDirty(victimX, victimY, victimZ);

    // 2026-11-06o?????????????
    //
    // ???????????????????????"???"??"????????"?
    // ??????? g_vliMutex ???? 125x125?VPT_SOURCE_DIRECTORY_RADIUS=2?
    // ?? receiver ???? 125??? ???? ~8.9ms?? [RT VLI SLOW] ? rest?
    // ???????? wait ??max 47ms??
    // ? apply ???????????????? recordVLIUpdates ???????
    collectVPTNeighborhoodPageIndices(
        victimX,
        victimY,
        victimZ,
        s_deferredVptNeighborhoodPages);
    s_hasDeferredVptNeighborhoodPages.store(true, std::memory_order_release);

    outPageIndex = victimPageIndex;
    return true;
}

// 保留原原型（OrionInternal.h），行为 = 不带距离闸（无条件淘汰最远页）。
bool evictFarthestVLIPageLocked(uint32_t& outPageIndex)
{
    return evictFarthestVLIPageImpl(outPageIndex, false, 0, 0, 0);
}

// 池满时给新 Section 让位：仅当最远页比新段更远才淘汰。
static bool evictFarthestVLIPageForIncomingLocked(
    uint32_t& outPageIndex,
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ
)
{
    return evictFarthestVLIPageImpl(
        outPageIndex,
        true,
        sectionX,
        sectionY,
        sectionZ);
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadVoxelLightSection(
    JNIEnv*,
    jclass,
    jint sectionX,
    jint sectionY,
    jint sectionZ,
    jlong voxelPointer,
    jint voxelBytes
)
{
    constexpr size_t expectedBytes =
        static_cast<size_t>(VLI_SOURCE_WORDS) * sizeof(uint32_t);

    if(
        voxelPointer == 0 ||
        voxelBytes != static_cast<jint>(expectedBytes)
    )
    {
        std::stringstream ss;
        ss << "[VLI-1.4 SOURCE] INVALID section=("
           << sectionX << "," << sectionY << "," << sectionZ << ") bytes="
           << voxelBytes;
        RTLog(ss.str());
        return;
    }

    const uint32_t* const incomingWords =
        reinterpret_cast<const uint32_t*>(
            static_cast<uintptr_t>(voxelPointer)
        );


    const uint64_t sectionKey = packSectionKey(sectionX, sectionY, sectionZ, 0);

    const uint64_t vliUploadHash = vliHashWords(incomingWords, VLI_SOURCE_WORDS);
    const bool vliHashGate = vliHashGateEnabled();

    const uint64_t vliUploadOrdinal = ++g_vliUploadTotal;

    vliHotRecordAndReport(sectionKey, vliUploadOrdinal);

    if(vliHashGate && vliUploadHashUnchanged(sectionKey, vliUploadHash))
    {
        ++g_vliUploadGated;

        if((vliUploadOrdinal % 500u) == 0u)
        {
            RTLog(
                "[VLI GATE] total=" + std::to_string(g_vliUploadTotal.load()) +
                " gated=" + std::to_string(g_vliUploadGated.load()) +
                " applied=" + std::to_string(g_vliUploadApplied.load())
            );
        }

        return;
    }

    std::vector<LocalLightRecord> localLightsForVPT;
    {
        std::lock_guard<std::mutex> lightLock(g_lightMutex);
        const auto lightIt = g_lightSections.find(sectionKey);
        if(lightIt != g_lightSections.end())
        {
            localLightsForVPT = lightIt->second;
        }
    }

    {
        VliWorkerHoldProbe vliWorkerProbe;

        std::lock_guard<std::mutex> lock(g_vliMutex);

        vliWorkerProbe.lockWaitMs =
            VliWorkerHoldProbe::sinceMs(vliWorkerProbe.begin);

        auto pageIt = g_vliSections.find(sectionKey);

        if(
            pageIt != g_vliSections.end() &&
            pageIt->second.sourceWords.size() == VLI_SOURCE_WORDS &&
            std::memcmp(
                pageIt->second.sourceWords.data(),
                incomingWords,
                expectedBytes
            ) == 0
        )
        {
            if(vliHashGate)
            {
                vliUploadHashStore(sectionKey, vliUploadHash);
            }

            return;
        }

        std::vector<uint32_t> incoming(VLI_SOURCE_WORDS);
        std::memcpy(
            incoming.data(),
            incomingWords,
            expectedBytes
        );

        vliWorkerProbe.cmpMs =
            VliWorkerHoldProbe::sinceMs(vliWorkerProbe.begin) -
            vliWorkerProbe.lockWaitMs;

        if(vliHashGate)
        {
            vliUploadHashStore(sectionKey, vliUploadHash);
            ++g_vliUploadApplied;

            if((vliUploadOrdinal % 500u) == 0u)
            {
                RTLog(
                    "[VLI GATE] total=" + std::to_string(g_vliUploadTotal.load()) +
                    " gated=" + std::to_string(g_vliUploadGated.load()) +
                    " applied=" + std::to_string(g_vliUploadApplied.load())
                );
            }
        }

        {
            std::vector<LocalLightRecord> derivedLights;

            for(uint32_t wordIndex = 0u;
                wordIndex < VLI_SOURCE_WORDS;
                ++wordIndex)
            {
                const uint32_t word = incoming[wordIndex];
                const uint32_t emission = (word >> 2u) & 15u;

                if(emission == 0u)
                {
                    continue;
                }

                LocalLightRecord record{};
                record.worldX =
                    sectionX * 16 + static_cast<int>(wordIndex & 15u);
                record.worldY =
                    sectionY * 16 + static_cast<int>((wordIndex >> 8u) & 15u);
                record.worldZ =
                    sectionZ * 16 + static_cast<int>((wordIndex >> 4u) & 15u);
                record.emissionLevel = static_cast<int>(emission);
                record.typeCode = static_cast<int>((word >> 6u) & 31u);

                derivedLights.push_back(record);
            }

            std::lock_guard<std::mutex> lightLock(g_lightMutex);

            const auto previousIt = g_lightSections.find(sectionKey);
            const size_t previousCount =
                previousIt == g_lightSections.end()
                    ? 0u
                    : previousIt->second.size();

            size_t derivedCount = 0u;

            if(derivedLights.empty())
            {
                g_lightSections.erase(sectionKey);
            }
            else
            {
                derivedCount = derivedLights.size();
                g_lightSections[sectionKey] = std::move(derivedLights);
            }

            g_totalLocalLights =
                g_totalLocalLights - previousCount + derivedCount;

            g_localLightsDirty = true;
        }

        vliWorkerProbe.lightsMs =
            VliWorkerHoldProbe::sinceMs(vliWorkerProbe.begin) -
            vliWorkerProbe.lockWaitMs -
            vliWorkerProbe.cmpMs;

        const uint32_t emitterRevision = g_vptEmitterRevision++;
        if(g_vptEmitterRevision == 0u)
        {
            g_vptEmitterRevision = 1u;
        }
        std::vector<LocalLightRecord> legacyBakeLights;

        std::vector<uint32_t> emitterIndexWords =
            buildVPTEmitterIndexWordsForSection(
                sectionX,
                sectionY,
                sectionZ,
                incoming,
                legacyBakeLights,
                emitterRevision
            );
        std::vector<uint32_t> emitterAliasWords =
            buildVPTEmitterAliasWords(emitterIndexWords);

        vliWorkerProbe.indexMs =
            VliWorkerHoldProbe::sinceMs(vliWorkerProbe.begin) -
            vliWorkerProbe.lockWaitMs -
            vliWorkerProbe.cmpMs -
            vliWorkerProbe.lightsMs;

        if(applyVLISectionVoxelsLocked(
               sectionKey,
               sectionX,
               sectionY,
               sectionZ,
               incoming,
               emitterIndexWords,
               emitterAliasWords
           ))
        {
            return;
        }

        VLIPendingSection pending{};
        pending.sectionKey = sectionKey;
        pending.sectionX = sectionX;
        pending.sectionY = sectionY;
        pending.sectionZ = sectionZ;
        pending.sourceWords = std::move(incoming);
        pending.lights = std::move(localLightsForVPT);

        bool replaced = false;
        for(VLIPendingSection& parked : g_vliPendingSections)
        {
            if(parked.sectionKey == sectionKey)
            {
                parked = std::move(pending);
                replaced = true;
                break;
            }
        }
        if(!replaced)
        {
            g_vliPendingSections.push_back(std::move(pending));
        }

        if(!g_vliGrowthRequested)
        {
            g_vliGrowthRequested = true;

            // 2026-11-06i：限频。池顶到 capacityLimit 之后增长会立即返回，
            // 而 g_vliGrowthRequested 每帧被清，于是这条每帧重打（实测刷屏）。
            static uint32_t s_parkLogCount = 0u;

            if((++s_parkLogCount % 600u) == 1u)
            {
                std::stringstream ss;
                ss << "[VLI-1.4 SOURCE] page pool at capacity "
                   << g_vliPageCapacity
                   << "; Section parked for the render-thread growth"
                   << " growthPossible="
                   << (g_vliPageCapacity < vliPageCapacityLimit() ? 1 : 0)
                   << " parkedTotal=" << s_parkLogCount;
                RTLog(ss.str());
            }
        }
    }
}



extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadLightSection(
    JNIEnv*,
    jclass,
    jint sectionX,
    jint sectionY,
    jint sectionZ,
    jlong lightPointer,
    jint lightCount,
    jint lightStride
)
{
    constexpr int
        EXPECTED_LIGHT_STRIDE = 20;

    constexpr int
        MAX_LIGHTS_PER_SECTION = 16 * 16 * 16;


    const int
        count =
            static_cast<int>(
                lightCount
            );

    const int
        stride =
            static_cast<int>(
                lightStride
            );



    if(
        count < 0 ||
        count > MAX_LIGHTS_PER_SECTION ||
        stride != EXPECTED_LIGHT_STRIDE ||
        (
            count > 0 &&
            lightPointer == 0
        )
    )
    {
        std::stringstream ss;

        ss
            << "[RT LOCAL LIGHTS L2] INVALID"
            << " section=("
            << sectionX
            << ","
            << sectionY
            << ","
            << sectionZ
            << ")"
            << " ptr=0x"
            << std::hex
            << static_cast<unsigned long long>(
                lightPointer
            )
            << std::dec
            << " count="
            << count
            << " stride="
            << stride;

        RTLog(
            ss.str()
        );

        return;
    }


    const uint8_t* rawBase = nullptr;

    if(count > 0)
    {
        rawBase =
            reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(
                    lightPointer
                )
            );

        std::lock_guard<std::mutex> lock(g_lightMutex);

        const uint64_t sectionKey =
            packSectionKey(
                static_cast<int>(sectionX),
                static_cast<int>(sectionY),
                static_cast<int>(sectionZ),
                0
            );

        const auto oldIt = g_lightSections.find(sectionKey);

        if(
            oldIt != g_lightSections.end() &&
            oldIt->second.size() == static_cast<size_t>(count)
        )
        {
            bool allRecordsAreAccepted = true;

            for(int i = 0; i < count; ++i)
            {
                int32_t emissionLevel = 0;

                std::memcpy(
                    &emissionLevel,
                    rawBase +
                        static_cast<size_t>(i) *
                            static_cast<size_t>(stride) +
                        12,
                    sizeof(emissionLevel)
                );

                if(emissionLevel <= 0 || emissionLevel > 15)
                {
                    allRecordsAreAccepted = false;
                    break;
                }
            }

            if(
                allRecordsAreAccepted &&
                std::memcmp(
                    oldIt->second.data(),
                    rawBase,
                    static_cast<size_t>(count) *
                        sizeof(LocalLightRecord)
                ) == 0
            )
            {
                return;
            }
        }
    }


    std::vector<LocalLightRecord>
        incomingLights;


    incomingLights.reserve(
        static_cast<size_t>(
            count
        )
    );


    if(count > 0)
    {
        for(
            int i = 0;
            i < count;
            ++i
        )
        {
            const uint8_t* const
                recordBase =
                    rawBase +
                    static_cast<size_t>(
                        i
                    ) *
                    static_cast<size_t>(
                        stride
                    );


            LocalLightRecord
                light{};


            std::memcpy(
                &light.worldX,
                recordBase + 0,
                sizeof(int32_t)
            );

            std::memcpy(
                &light.worldY,
                recordBase + 4,
                sizeof(int32_t)
            );

            std::memcpy(
                &light.worldZ,
                recordBase + 8,
                sizeof(int32_t)
            );

            std::memcpy(
                &light.emissionLevel,
                recordBase + 12,
                sizeof(int32_t)
            );

            std::memcpy(
                &light.typeCode,
                recordBase + 16,
                sizeof(int32_t)
            );


            if(
                light.emissionLevel <= 0 ||
                light.emissionLevel > 15
            )
            {
                continue;
            }


            incomingLights.push_back(
                light
            );
        }
    }


    const uint64_t
        sectionKey =
            packSectionKey(
                static_cast<int>(
                    sectionX
                ),
                static_cast<int>(
                    sectionY
                ),
                static_cast<int>(
                    sectionZ
                ),
                0
            );


    size_t
        sectionLightCount = 0;

    size_t
        totalLightCount = 0;

    bool
        sectionChanged = false;


    {
        std::lock_guard<std::mutex>
            lock(
                g_lightMutex
            );


        auto oldIt =
            g_lightSections.find(
                sectionKey
            );

        if(
            oldIt !=
                g_lightSections.end() &&
            sameLocalLightPayload(
                oldIt->second,
                incomingLights
            )
        )
        {
            sectionLightCount =
                oldIt->second.size();

            totalLightCount =
                g_totalLocalLights;
        }
        else if(
            oldIt ==
                g_lightSections.end() &&
            incomingLights.empty()
        )
        {
            sectionLightCount =
                0;

            totalLightCount =
                g_totalLocalLights;
        }
        else
        {
            if(
                oldIt !=
                    g_lightSections.end()
            )
            {
                const size_t
                    oldCount =
                        oldIt->second.size();


                if(
                    g_totalLocalLights >=
                    oldCount
                )
                {
                    g_totalLocalLights -=
                        oldCount;
                }
                else
                {
                    g_totalLocalLights =
                        0;
                }
            }

            if(
                incomingLights.empty()
            )
            {
                g_lightSections.erase(
                    sectionKey
                );

                sectionLightCount =
                    0;
            }
            else
            {
                sectionLightCount =
                    incomingLights.size();

                g_lightSections[
                    sectionKey
                ] =
                    std::move(
                        incomingLights
                    );


                g_totalLocalLights +=
                    sectionLightCount;
            }

            totalLightCount =
                g_totalLocalLights;

            sectionChanged =
                true;
        }
    }


    if(!sectionChanged)
    {
        return;
    }

    std::vector<LocalLightRecord> localLightsForVPT;
    {
        std::lock_guard<std::mutex> lightLock(g_lightMutex);
        const auto lightIt = g_lightSections.find(sectionKey);
        if(lightIt != g_lightSections.end())
        {
            localLightsForVPT = lightIt->second;
        }
    }
    refreshVPTEmitterSectionFromLocalLights(
        static_cast<int32_t>(sectionX),
        static_cast<int32_t>(sectionY),
        static_cast<int32_t>(sectionZ),
        localLightsForVPT
    );

    g_localLightsDirty =
        true;


    std::stringstream ss;

    ss
        << "[RT LOCAL LIGHTS L2]"
        << " section=("
        << sectionX
        << ","
        << sectionY
        << ","
        << sectionZ
        << ")"
        << " lights="
        << sectionLightCount
        << " total="
        << totalLightCount;


    RTLog(
        ss.str()
    );
}



extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_removeLightChunk(
    JNIEnv*,
    jclass,
    jint chunkX,
    jint chunkZ
)
{
    constexpr uint64_t
        SECTION_XZ_MASK =
            (1ull << 44) - 1ull;


    const uint64_t
        targetXZ =
            packSectionKey(
                static_cast<int>(
                    chunkX
                ),
                0,
                static_cast<int>(
                    chunkZ
                ),
                0
            ) &
            SECTION_XZ_MASK;


    size_t removedSections =
        0;

    size_t removedLights =
        0;

    size_t remainingLights =
        0;


    {
        std::lock_guard<std::mutex>
            lock(
                g_lightMutex
            );


        for(
            auto it =
                g_lightSections.begin();

            it !=
                g_lightSections.end();
        )
        {
            const bool sameChunk =
                (
                    it->first &
                    SECTION_XZ_MASK
                ) ==
                targetXZ;


            if(!sameChunk)
            {
                ++it;

                continue;
            }


            const size_t
                sectionLightCount =
                    it->second.size();


            removedLights +=
                sectionLightCount;


            if(
                g_totalLocalLights >=
                sectionLightCount
            )
            {
                g_totalLocalLights -=
                    sectionLightCount;
            }
            else
            {
                g_totalLocalLights =
                    0;
            }


            it =
                g_lightSections.erase(
                    it
                );


            ++removedSections;
        }


        remainingLights =
            g_totalLocalLights;
    }


    if(removedSections > 0)
    {
        g_localLightsDirty =
            true;
    }


    size_t vliRemovedSections = 0;

    std::vector<std::array<int32_t, 3>> vliRemovedCoordinates;

    // 2026-11-06l：被摘下的 VLI 页先 move 到这里，等 g_vliMutex 释放后再析构。
    // 声明位置必须在锁作用域之外 —— 否则析构仍发生在锁内。
    std::vector<VLISectionCpuPage> vliUnloadGarbage;
    vliUnloadGarbage.reserve(64u);

    {
        VliLockHoldProbe vliLockHoldProbe("chunkUnloadScan");
        std::lock_guard<std::mutex> lock(g_vliMutex);
        vliLockHoldProbe.mark();

        const uint64_t vliUnloadColumn = vliChunkColumnKey(chunkX, chunkZ);
        const auto vliColumnIt = g_vliChunkSectionIndex.find(vliUnloadColumn);

        if(vliColumnIt != g_vliChunkSectionIndex.end())
        {
            std::vector<uint64_t> vliColumnKeys = std::move(vliColumnIt->second);
            g_vliChunkSectionIndex.erase(vliColumnIt);

            for(const uint64_t vliColumnKey : vliColumnKeys)
            {
                const auto vliPageIt = g_vliSections.find(vliColumnKey);

                if(vliPageIt == g_vliSections.end())
                {
                    continue;
                }

                const VLISectionCpuPage& page = vliPageIt->second;

                // 先把要用的标量取出来：下面 move 之后 page 就是空壳了。
                const int32_t pageSectionX = page.sectionX;
                const int32_t pageSectionY = page.sectionY;
                const int32_t pageSectionZ = page.sectionZ;
                const uint32_t pageIndexValue = page.pageIndex;
                const uint32_t pageTableSlot = page.tableSlot;

                vliRemovedCoordinates.push_back({
                    pageSectionX,
                    pageSectionY,
                    pageSectionZ
                });
                g_vliFreePages.push_back(pageIndexValue);
                g_vliPageSectionKeys[pageIndexValue] = VLI_INVALID_SECTION_KEY;

                eraseVLITableSlot(pageTableSlot);

                // 2026-11-06l：把大对象 move 出去（只搬内部指针），让 erase 立刻返回；
                // 真正的析构发生在函数返回、g_vliMutex 已释放之后。
                // 实测这一段曾占 chunkUnloadScan 的 98%（7 段 1.26ms，180µs/次）。
                vliUnloadGarbage.push_back(std::move(vliPageIt->second));
                g_vliSections.erase(vliPageIt);

                vliUploadHashForget(vliColumnKey);

                ++vliRemovedSections;
            }
        }

        {
            static const bool vliIndexCheckEnabled = []()
            {
                const char* raw = std::getenv("ORION_VLI_INDEX_CHECK");

                return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
            }();

            if(vliIndexCheckEnabled)
            {
                size_t vliStalePages = 0u;

                for(auto it = g_vliSections.begin(); it != g_vliSections.end();)
                {
                    const VLISectionCpuPage& page = it->second;

                    if(page.sectionX != chunkX || page.sectionZ != chunkZ)
                    {
                        ++it;
                        continue;
                    }

                    g_vliFreePages.push_back(page.pageIndex);
                    g_vliPageSectionKeys[page.pageIndex] = VLI_INVALID_SECTION_KEY;
                    eraseVLITableSlot(page.tableSlot);
                    it = g_vliSections.erase(it);
                    ++vliStalePages;
                }

                if(vliStalePages > 0u)
                {
                    RTLog(
                        "[VLI INDEX] stale pages removed=" +
                        std::to_string(vliStalePages)
                    );
                }
            }
        }

        {
            static uint64_t vliReinsertAtUnloadBegin = 0u;
            static uint32_t vliUnloadProbeCount = 0u;

            if(vliRemovedSections > 0u)
            {
                const uint64_t reinsertsThisUnload =
                    g_vliTableReinsertTotal - vliReinsertAtUnloadBegin;

                if(reinsertsThisUnload >= 200u || vliUnloadProbeCount < 3u)
                {
                    RTLog(
                        "[VLI UNLOAD] removed=" + std::to_string(vliRemovedSections) +
                        " reinserts=" + std::to_string(reinsertsThisUnload) +
                        " pending=" + std::to_string(g_vliPendingSections.size())
                    );
                    ++vliUnloadProbeCount;
                }
            }

            vliReinsertAtUnloadBegin = g_vliTableReinsertTotal;
        }

        if(!g_vliPendingSections.empty())
        {
            g_vliPendingSections.erase(
                std::remove_if(
                    g_vliPendingSections.begin(),
                    g_vliPendingSections.end(),
                    [chunkX, chunkZ](const VLIPendingSection& parked)
                    {
                        return parked.sectionX == chunkX &&
                               parked.sectionZ == chunkZ;
                    }
                ),
                g_vliPendingSections.end()
            );
        }

    }

    for(const auto& coordinate : vliRemovedCoordinates)
    {
        VliLockHoldProbe vliLockHoldProbe("chunkUnloadNbr");
        std::lock_guard<std::mutex> lock(g_vliMutex);
        vliLockHoldProbe.mark();

        markVLINeighborhoodDirty(
            coordinate[0],
            coordinate[1],
            coordinate[2]
        );
        rebuildVPTCandidateNeighborhoodLocked(
            coordinate[0],
            coordinate[1],
            coordinate[2]
        );
    }


    size_t removedGeometryLayers =
        0;


    {
        std::lock_guard<std::timed_mutex>
            lock(
                g_chunkMutex
            );


        for(
            auto it =
                g_chunkMeshes.begin();

            it !=
                g_chunkMeshes.end();
        )
        {
            if(
                (
                    it->first &
                    SECTION_XZ_MASK
                ) !=
                targetXZ
            )
            {
                ++it;

                continue;
            }


            g_dirtySectionKeys.insert(
                it->first &
                SPATIAL_SECTION_KEY_MASK
            );


            it =
                g_chunkMeshes.erase(
                    it
                );


            ++removedGeometryLayers;
        }


        for(
            auto it =
                g_chunkRawPayloads.begin();

            it !=
                g_chunkRawPayloads.end();
        )
        {
            if(
                (
                    it->first &
                    SECTION_XZ_MASK
                ) ==
                targetXZ
            )
            {
                it =
                    g_chunkRawPayloads.erase(
                        it
                    );
            }
            else
            {
                ++it;
            }
        }


        for(
            auto it =
                g_chunkUploadDigests.begin();

            it !=
                g_chunkUploadDigests.end();
        )
        {
            if(
                (
                    it->first &
                    SECTION_XZ_MASK
                ) ==
                targetXZ
            )
            {
                it =
                    g_chunkUploadDigests.erase(
                        it
                    );
            }
            else
            {
                ++it;
            }
        }


        for(
            auto it =
                g_cutoutOmmInputs.begin();

            it !=
                g_cutoutOmmInputs.end();
        )
        {
            if(
                (
                    it->first &
                    SECTION_XZ_MASK
                ) ==
                targetXZ
            )
            {
                it =
                    g_cutoutOmmInputs.erase(
                        it
                    );
            }
            else
            {
                ++it;
            }
        }


        if(removedGeometryLayers > 0)
        {
            markWorldGeometryDirty();
        }
    }


    std::stringstream ss;

    ss
        << "[RT LOCAL LIGHTS L6]"
        << " chunk=("
        << chunkX
        << ","
        << chunkZ
        << ")"
        << " removedSections="
        << removedSections
        << " removedLights="
        << removedLights
        << " total="
        << remainingLights
        << " vliRemovedSections="
        << vliRemovedSections
        << " vliPeakPages="
        << g_vliNextPage
        << " vliLivePages="
        << g_vliSections.size()
        << " vliFreePages="
        << g_vliFreePages.size()
        << " geometryLayers="
        << removedGeometryLayers;


    RTLog(
        ss.str()
    );
}
