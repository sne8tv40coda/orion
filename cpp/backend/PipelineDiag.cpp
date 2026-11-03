// Orion backend — PipelineDiag 模块。
//
// 2026-11-02 P0（冷编分解）：把管线创建耗时按 SPV 名逐条记录，用于把
// "改一个 SPV 就要等十几分钟"拆成 cache 加载 / compute 管线 / RT 管线三部分。
//
// 只做观测：不改变任何创建参数（pipelineCache、deferredOperation、flags 全部
// 原样透传），也不改变返回码。日志统一前缀 [PERF PIPE]，与既有 [PERF PC.*]
// 走同一个 stderr + RTLog 出口，便于把时间轴对齐。
//
// 为什么 compute 侧不逐个改调用点：TrackingHooks.h 已经把
// vkCreateComputePipelines 宏到 rtxTrackedCreateComputePipelines，改 Tracking.cpp
// 里那一处就覆盖了全部 40+ 个 compute 创建点。

#include "OrionInternal.h"

namespace
{

std::mutex g_orionPipelineDiagMutex;

// SPV 内容哈希 -> 请求路径（loadSpv 登记）。
std::unordered_map<uint64_t, std::string> g_orionSpvNameByHash;

// VkShaderModule -> SPV 内容哈希（rtxTrackedCreateShaderModule 登记）。
// module 被销毁后句柄可能复用，但这里只用于日志标注，标错也不影响功能。
std::unordered_map<VkShaderModule, uint64_t> g_orionModuleSpvHash;

std::atomic<uint64_t> g_orionPipelineCreateCount{0u};
std::atomic<uint64_t> g_orionPipelineCreateMicros{0u};

// FNV-1a 64：与 PipelineCache.cpp 的 ComputeRTPipelineCacheKey 同口径，
// 避免同一份 SPV 在两处算出不同身份。
uint64_t orionPipelineDiagFnv1a(const void* data, size_t size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);

    uint64_t hash = 1469598103934665603ull;

    for(size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }

    return hash;
}

void orionPipelineDiagEmit(const char* text)
{
    fprintf(stderr, "%s\n", text);
    fflush(stderr);

    // 2026-11-02：必须用 flush 版本。RTLog 走的是不 flush 的 ofstream
    // （Platform.cpp:233-263），冷编期间 rt_debug.log 会一直停在上一帧，
    // 只能盯 stderr。冷编正是"需要实时看进度"的唯一场景，这点落盘开销
    // （初始化期几十行）远小于失去可观测性。
    RTLogFlushed(text);
}

} // namespace

uint64_t OrionPipelineSpvHash(const void* data, size_t size)
{
    if(data == nullptr || size == 0u)
    {
        return 0ull;
    }

    return orionPipelineDiagFnv1a(data, size);
}

void OrionPipelineRegisterSpvName(uint64_t hash, const char* name)
{
    if(hash == 0ull)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_orionPipelineDiagMutex);

    g_orionSpvNameByHash[hash] = (name != nullptr) ? name : "<null>";
}

void OrionPipelineRegisterShaderModule(VkShaderModule module, uint64_t spvHash)
{
    if(module == VK_NULL_HANDLE || spvHash == 0ull)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_orionPipelineDiagMutex);

    g_orionModuleSpvHash[module] = spvHash;
}

const char* OrionPipelineSpvNameOfModule(VkShaderModule module)
{
    // 调用方（rtxTrackedCreateComputePipelines）是同步使用返回值，thread_local
    // 避免持锁返回 map 内 string 的指针（rehash 会让引用失效）。
    static thread_local std::string name;

    name = "<unknown-spv>";

    if(module == VK_NULL_HANDLE)
    {
        return name.c_str();
    }

    std::lock_guard<std::mutex> lock(g_orionPipelineDiagMutex);

    const auto moduleIt = g_orionModuleSpvHash.find(module);

    if(moduleIt == g_orionModuleSpvHash.end())
    {
        return name.c_str();
    }

    const auto nameIt = g_orionSpvNameByHash.find(moduleIt->second);

    if(nameIt != g_orionSpvNameByHash.end())
    {
        name = nameIt->second;
    }

    return name.c_str();
}

void OrionPipelineLogCreate(
    const char* kind,
    const char* spvName,
    bool cacheUsed,
    double milliseconds,
    VkResult result)
{
    const uint64_t creates =
        g_orionPipelineCreateCount.fetch_add(1u, std::memory_order_relaxed) + 1u;

    const uint64_t micros =
        g_orionPipelineCreateMicros.fetch_add(
            static_cast<uint64_t>(milliseconds * 1000.0),
            std::memory_order_relaxed
        ) + static_cast<uint64_t>(milliseconds * 1000.0);

    char text[768] = {};

    std::snprintf(
        text,
        sizeof(text),
        "[PERF PIPE] kind=%s spv=%s cache=%d ms=%.1f result=%d creates=%llu totalMs=%.1f",
        (kind != nullptr) ? kind : "?",
        (spvName != nullptr) ? spvName : "?",
        cacheUsed ? 1 : 0,
        milliseconds,
        static_cast<int>(result),
        static_cast<unsigned long long>(creates),
        static_cast<double>(micros) / 1000.0
    );

    orionPipelineDiagEmit(text);
}

void OrionPipelineLogCachePhase(
    const char* phase,
    double milliseconds,
    size_t bytes,
    const char* detail)
{
    char text[768] = {};

    std::snprintf(
        text,
        sizeof(text),
        "[PERF PIPE] kind=cache-%s ms=%.1f bytes=%zu detail=%s",
        (phase != nullptr) ? phase : "?",
        milliseconds,
        bytes,
        (detail != nullptr) ? detail : ""
    );

    orionPipelineDiagEmit(text);
}

VkResult OrionCreateRayTracingPipelineTimed(
    const char* label,
    uint32_t createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR* pCreateInfos,
    VkPipelineCache pipelineCache,
    VkPipeline* pPipelines)
{
    auto createPipe =
        reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
            vkGetDeviceProcAddr(g_device, "vkCreateRayTracingPipelinesKHR"));

    if(createPipe == nullptr)
    {
        throw std::runtime_error("vkCreateRayTracingPipelinesKHR unavailable");
    }

    const auto start = std::chrono::steady_clock::now();

    // deferredOperation 仍为 VK_NULL_HANDLE，其余参数与各调用点原样一致。
    const VkResult result = createPipe(
        g_device,
        VK_NULL_HANDLE,
        pipelineCache,
        createInfoCount,
        pCreateInfos,
        nullptr,
        pPipelines
    );

    const double milliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start
        ).count();

    // 标签里要的是**参与编译的 shader stage 数**（RTPSO 的编译量由它决定），
    // 不是 createInfoCount。主 RT 管线是 1 条 createInfo / 6 个 stage，
    // 之前打 "1stage" 会误导"这条管线很小"。
    uint32_t stageCount = 0u;

    if(pCreateInfos != nullptr && createInfoCount > 0u)
    {
        stageCount = pCreateInfos[0].stageCount;
    }

    char name[192] = {};

    std::snprintf(
        name,
        sizeof(name),
        "rt:%s(%ustage)",
        (label != nullptr) ? label : "?",
        stageCount
    );

    OrionPipelineLogCreate(
        "rt",
        name,
        pipelineCache != VK_NULL_HANDLE,
        milliseconds,
        result
    );

    return result;
}
