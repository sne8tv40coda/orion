// Orion backend — Tracking 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"


// 某 memoryTypeIndex 是否落在 DEVICE_LOCAL 堆。物理设备未就绪时返回 false（无法判定）。
bool rtxMemTypeIsDeviceLocal(uint32_t memoryTypeIndex)
{
    if(g_physicalDevice == VK_NULL_HANDLE)
    {
        return false;
    }

    VkPhysicalDeviceMemoryProperties properties{};

    vkGetPhysicalDeviceMemoryProperties(g_physicalDevice,&properties);

    if(memoryTypeIndex >= properties.memoryTypeCount)
    {
        return false;
    }

    const uint32_t heapIndex =
        properties.memoryTypes[memoryTypeIndex].heapIndex;

    return (
        properties.memoryHeaps[heapIndex].flags &
        VK_MEMORY_HEAP_DEVICE_LOCAL_BIT
    ) != 0u;
}


// ============================================================
// 2026-10-29 OPT-EXT②：VK_EXT_memory_priority 分层
//
// 【策略是自创的】官方规范只定义语义（priority ∈ [0,1]，驱动在显存压力下
// 先驱逐低优先级分配），没有给"什么该低"的推荐值。这里的假设是：
//   · host-visible / staging  → 0.80（马上要被读回或上传，尽量别动）
//   · device-local 且 ≥ 8 MiB → 0.35（section 顶点/BLAS/历史缓冲，可重建，
//                                    也是本工程最大的一块：asTrueMB 574MiB）
//   · 其余 device-local       → 0.50（中性）
// 只影响驱动驱逐顺序，不影响任何数据正确性。
//
// 关掉：ORION_MEM_PRIORITY=0（或 false）。扩展未启用时自动失效（用函数指针
// 存在性判定，不依赖 feature 查询）。
// ============================================================
bool rtxMemPriorityEnabled()
{
    static const bool enabled = []()
    {
        const char* raw = std::getenv("ORION_MEM_PRIORITY");

        if(
            raw != nullptr &&
            (raw[0] == '0' || raw[0] == 'f' || raw[0] == 'F')
        )
        {
            fprintf(
                stderr,
                "[MEM PRIORITY] disabled by ORION_MEM_PRIORITY\n"
            );

            fflush(stderr);
            return false;
        }

        if(g_device == VK_NULL_HANDLE)
        {
            return false;
        }

        const bool available =
            vkGetDeviceProcAddr(
                g_device,
                "vkSetDeviceMemoryPriorityEXT") != nullptr;

        fprintf(
            stderr,
            "[MEM PRIORITY] enabled=%d"
            " hostVisible=0.80 largeDeviceLocal=0.35 default=0.50\n",
            available ? 1 : 0
        );

        fflush(stderr);

        return available;
    }();

    return enabled;
}
float rtxMemPriorityForAllocation(const VkMemoryAllocateInfo& allocateInfo)
{
    if(!rtxMemTypeIsDeviceLocal(allocateInfo.memoryTypeIndex))
    {
        return 0.80f;
    }

    if(allocateInfo.allocationSize >= (8ull << 20ull))
    {
        return 0.35f;
    }

    return 0.50f;
}

// ⚠ 上面两行 #undef 就是为这里服务的：本函数体必须调用真的 vkAllocateMemory。
VkResult rtxTrackedAllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* allocateInfo,
    const VkAllocationCallbacks* allocator,
    VkDeviceMemory* memory)
{
    VkMemoryAllocateInfo prioritizedInfo{};
    const VkMemoryAllocateInfo* effectiveInfo = allocateInfo;
    VkMemoryPriorityAllocateInfoEXT priorityInfo{};

    if(allocateInfo != nullptr && rtxMemPriorityEnabled())
    {
        prioritizedInfo = *allocateInfo;
        priorityInfo.sType =
            VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT;
        priorityInfo.pNext = const_cast<void*>(allocateInfo->pNext);
        priorityInfo.priority = rtxMemPriorityForAllocation(*allocateInfo);
        prioritizedInfo.pNext = &priorityInfo;
        effectiveInfo = &prioritizedInfo;
    }

    const VkResult result =
        vkAllocateMemory(device,effectiveInfo,allocator,memory);

    if(
        result == VK_SUCCESS &&
        allocateInfo != nullptr &&
        memory != nullptr &&
        *memory != VK_NULL_HANDLE
    )
    {
        const uint64_t bytes =
            static_cast<uint64_t>(allocateInfo->allocationSize);

        const bool deviceLocal =
            rtxMemTypeIsDeviceLocal(allocateInfo->memoryTypeIndex);

        RtxMemLiveRecord record;
        record.size = bytes;
        record.deviceLocal = deviceLocal;

        std::lock_guard<std::mutex> lock(g_rtxMemAccountMutex);

        g_rtxMemLive[*memory] = record;
        g_rtxMemAllocCount += 1u;
        g_rtxMemAllocBytesTotal += bytes;
        g_rtxMemLiveBytes += bytes;

        if(deviceLocal)
        {
            g_rtxMemLiveDeviceLocalBytes += bytes;
        }
        else
        {
            g_rtxMemLiveHostBytes += bytes;
        }

        if(g_rtxMemLiveBytes > g_rtxMemLivePeakBytes)
        {
            g_rtxMemLivePeakBytes = g_rtxMemLiveBytes;
        }
    }

    return result;
}
void rtxTrackedFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* allocator)
{
    if(memory != VK_NULL_HANDLE)
    {
        std::lock_guard<std::mutex> lock(g_rtxMemAccountMutex);

        const auto it = g_rtxMemLive.find(memory);

        if(it != g_rtxMemLive.end())
        {
            const RtxMemLiveRecord record = it->second;

            g_rtxMemLive.erase(it);
            g_rtxMemFreeCount += 1u;
            g_rtxMemFreeBytesTotal += record.size;

            g_rtxMemLiveBytes =
                g_rtxMemLiveBytes >= record.size
                    ? g_rtxMemLiveBytes - record.size
                    : 0u;

            if(record.deviceLocal)
            {
                g_rtxMemLiveDeviceLocalBytes =
                    g_rtxMemLiveDeviceLocalBytes >= record.size
                        ? g_rtxMemLiveDeviceLocalBytes - record.size
                        : 0u;
            }
            else
            {
                g_rtxMemLiveHostBytes =
                    g_rtxMemLiveHostBytes >= record.size
                        ? g_rtxMemLiveHostBytes - record.size
                        : 0u;
            }

            if(record.hasUsage)
            {
                const auto churnIt = g_rtxBufferChurn.find(record.usage);

                if(churnIt != g_rtxBufferChurn.end())
                {
                    RtxBufferChurnStats& stats = churnIt->second;

                    stats.liveCount =
                        stats.liveCount > 0u ? stats.liveCount - 1u : 0u;
                    stats.liveBytes =
                        stats.liveBytes >= record.size
                            ? stats.liveBytes - record.size
                            : 0u;
                }
            }
        }
        else
        {
            g_rtxMemFreeUnknownCount += 1u;
        }
    }

    vkFreeMemory(device,memory,allocator);
}

// createBuffer 成功分配到内存后调用：把这块内存登记到 usage 名下，供抖动归因。
//
// 只是给已有记录补一个字段，并累加该 usage 的累计抖动与存活量；重复登记（同一内存
// 被调两次）不会再计一次累计分配，但会把存活量修正到唯一值。
void rtxNoteBufferMemory(
    uint64_t usage,
    uint64_t bytes,
    VkDeviceMemory memory)
{
    if(memory == VK_NULL_HANDLE)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_rtxMemAccountMutex);

    const auto it = g_rtxMemLive.find(memory);

    if(it == g_rtxMemLive.end())
    {
        return;
    }

    RtxMemLiveRecord& record = it->second;

    if(record.hasUsage)
    {
        if(record.usage == usage)
        {
            return;
        }

        const auto previousIt = g_rtxBufferChurn.find(record.usage);

        if(previousIt != g_rtxBufferChurn.end())
        {
            RtxBufferChurnStats& previous = previousIt->second;

            previous.liveCount =
                previous.liveCount > 0u ? previous.liveCount - 1u : 0u;
            previous.liveBytes =
                previous.liveBytes >= record.size
                    ? previous.liveBytes - record.size
                    : 0u;
        }
    }

    RtxBufferChurnStats& stats = g_rtxBufferChurn[usage];

    stats.allocCount += 1u;
    stats.allocBytes += bytes;
    stats.liveCount += 1u;
    stats.liveBytes += bytes;

    record.usage = usage;
    record.hasUsage = true;
}

// 抖动归因报告：按累计次数排出最热的 usage（最多 topN 条）。
//
// 读法：`churnTop` 里的 usage 十六进制值就是 VkBufferUsageFlags —— 拿它去源码里搜
// createBuffer 的 usage 实参即可对上子系统；`liveN/liveMB` 是同一 usage 名下当前
// 还活着多少，用来区分"高频但平衡"和"高频且只进不出"。
std::string rtxBufferChurnTopReport(size_t topN)
{
    std::vector<
        std::pair<uint64_t, RtxBufferChurnStats>
    > sorted;

    {
        std::lock_guard<std::mutex> lock(g_rtxMemAccountMutex);

        sorted.reserve(g_rtxBufferChurn.size());

        for(const auto& entry : g_rtxBufferChurn)
        {
            sorted.push_back(entry);
        }
    }

    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.second.allocCount > rhs.second.allocCount;
        }
    );

    std::ostringstream out;
    out << std::fixed << std::setprecision(1);

    const size_t count = sorted.size() < topN ? sorted.size() : topN;

    for(size_t index = 0u; index < count; ++index)
    {
        const auto& entry = sorted[index];

        out << " use=0x" << std::hex << entry.first << std::dec
            << "(n=" << entry.second.allocCount
            << " MB=" << (static_cast<double>(entry.second.allocBytes) / 1048576.0)
            << " liveN=" << entry.second.liveCount
            << " liveMB=" << (static_cast<double>(entry.second.liveBytes) / 1048576.0)
            << ")";
    }

    return out.str();
}
RtxMemAccountSnapshot rtxMemAccountSnapshot()
{
    std::lock_guard<std::mutex> lock(g_rtxMemAccountMutex);

    RtxMemAccountSnapshot snapshot;

    snapshot.allocCount = g_rtxMemAllocCount;
    snapshot.freeCount = g_rtxMemFreeCount;
    snapshot.freeUnknownCount = g_rtxMemFreeUnknownCount;
    snapshot.allocBytes = g_rtxMemAllocBytesTotal;
    snapshot.freeBytes = g_rtxMemFreeBytesTotal;
    snapshot.liveBytes = g_rtxMemLiveBytes;
    snapshot.liveDeviceLocalBytes = g_rtxMemLiveDeviceLocalBytes;
    snapshot.liveHostBytes = g_rtxMemLiveHostBytes;
    snapshot.livePeakBytes = g_rtxMemLivePeakBytes;
    snapshot.liveCount = g_rtxMemLive.size();

    return snapshot;
}

// 与 perfTimestampsEnabled 同一套写法：只读一次环境变量，改完必须重启客户端。
bool rtxMemObjTrackEnabled()
{
    static const bool enabled = []() {
        const char* raw = std::getenv("ORION_MEM_OBJ_TRACK");
        if(raw == nullptr || raw[0] == '\0')
        {
            return true;
        }
        return std::string(raw) != "0";
    }();
    return enabled;
}
VkResult rtxTrackedCreateDescriptorPool(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDescriptorPool* pool)
{
    const VkResult result =
        vkCreateDescriptorPool(device, createInfo, allocator, pool);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.descPool.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
VkResult rtxTrackedAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* allocateInfo,
    VkDescriptorSet* descriptorSets)
{
    const VkResult result =
        vkAllocateDescriptorSets(device, allocateInfo, descriptorSets);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS && allocateInfo != nullptr)
    {
        g_rtxMemObj.descSet.fetch_add(
            static_cast<uint64_t>(allocateInfo->descriptorSetCount),
            std::memory_order_relaxed
        );
    }

    return result;
}
VkResult rtxTrackedCreateCommandPool(
    VkDevice device,
    const VkCommandPoolCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkCommandPool* commandPool)
{
    const VkResult result =
        vkCreateCommandPool(device, createInfo, allocator, commandPool);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.cmdPool.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
VkResult rtxTrackedAllocateCommandBuffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* allocateInfo,
    VkCommandBuffer* commandBuffers)
{
    const VkResult result =
        vkAllocateCommandBuffers(device, allocateInfo, commandBuffers);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS && allocateInfo != nullptr)
    {
        g_rtxMemObj.cmdBuf.fetch_add(
            static_cast<uint64_t>(allocateInfo->commandBufferCount),
            std::memory_order_relaxed
        );
    }

    return result;
}
VkResult rtxTrackedCreatePipelineLayout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkPipelineLayout* pipelineLayout)
{
    const VkResult result =
        vkCreatePipelineLayout(device, createInfo, allocator, pipelineLayout);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.layout.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
VkResult rtxTrackedCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkShaderModule* shaderModule)
{
    const VkResult result =
        vkCreateShaderModule(device, createInfo, allocator, shaderModule);

    // 2026-11-02 P0（冷编分解）：登记 module -> SPV 哈希。compute 管线创建日志
    // 只拿得到 VkShaderModule，靠这张表反查 loadSpv 登记的文件路径。只做观测。
    if(result == VK_SUCCESS &&
       shaderModule != nullptr &&
       createInfo != nullptr &&
       createInfo->pCode != nullptr &&
       createInfo->codeSize >= sizeof(uint32_t))
    {
        OrionPipelineRegisterShaderModule(
            *shaderModule,
            OrionPipelineSpvHash(createInfo->pCode, createInfo->codeSize)
        );
    }

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.shader.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
VkResult rtxTrackedCreateComputePipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* createInfos,
    const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines)
{
    // 2026-11-02 P0（冷编分解）：全部 compute 管线都经过这里（TrackingHooks.h 把
    // vkCreateComputePipelines 整体宏到本函数），所以只此一处计时即可把
    // "启动里 compute 编译占多少"打成逐条 [PERF PIPE]。
    // 注意：只观测，pipelineCache 与 allocator 原样透传，不影响命中率。
    const auto orionPipelineCreateStart =
        std::chrono::steady_clock::now();

    const VkResult result = vkCreateComputePipelines(
        device,
        pipelineCache,
        createInfoCount,
        createInfos,
        allocator,
        pipelines
    );

    const double orionPipelineCreateMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - orionPipelineCreateStart
        ).count();

    const char* orionPipelineSpvName =
        (createInfoCount > 0u && createInfos != nullptr)
            ? OrionPipelineSpvNameOfModule(createInfos[0].stage.module)
            : "<none>";

    OrionPipelineLogCreate(
        "compute",
        orionPipelineSpvName,
        pipelineCache != VK_NULL_HANDLE,
        orionPipelineCreateMilliseconds,
        result
    );

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.pipe.fetch_add(
            static_cast<uint64_t>(createInfoCount),
            std::memory_order_relaxed
        );
    }

    return result;
}
VkResult rtxTrackedCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkImageView* view)
{
    const VkResult result =
        vkCreateImageView(device, createInfo, allocator, view);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.view.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
VkResult rtxTrackedCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkSampler* sampler)
{
    const VkResult result =
        vkCreateSampler(device, createInfo, allocator, sampler);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.sampler.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}


// 唯一一个记在销毁侧的计数：只有 create - free = live 才能把"真漏"与"churn"分开。
// 计**对象数**（commandBufferCount），与 cmdBuf 同口径。
void rtxTrackedFreeCommandBuffers(
    VkDevice device,
    VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const VkCommandBuffer* commandBuffers)
{
    vkFreeCommandBuffers(device, commandPool, commandBufferCount, commandBuffers);

    if(rtxMemObjTrackEnabled())
    {
        g_rtxMemObj.cmdBufFree.fetch_add(
            static_cast<uint64_t>(commandBufferCount),
            std::memory_order_relaxed
        );
    }
}
VkResult rtxTrackedCreateFence(
    VkDevice device,
    const VkFenceCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkFence* fence)
{
    const VkResult result = vkCreateFence(device, createInfo, allocator, fence);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.fence.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
VkResult rtxTrackedCreateSemaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkSemaphore* semaphore)
{
    const VkResult result =
        vkCreateSemaphore(device, createInfo, allocator, semaphore);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.semaphore.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
VkResult rtxTrackedCreateQueryPool(
    VkDevice device,
    const VkQueryPoolCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkQueryPool* queryPool)
{
    const VkResult result =
        vkCreateQueryPool(device, createInfo, allocator, queryPool);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.queryPool.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
