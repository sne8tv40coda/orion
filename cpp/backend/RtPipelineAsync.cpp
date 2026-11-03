// Orion backend — RtPipelineAsync 模块。
//
// 2026-11-02 P1.5：主 RT 管线的**分片异步重建**（VK_KHR_deferred_host_operations）。
//
// 要解决的问题（本机实测，[PERF PIPE] kind=rt）：
//   同一份 SPV 命中缓存 182.8 ms；任一变体变化后主 RT 管线（6 stage）全量编译
//   519 772 ~ 1 064 060 ms（8m40s ~ 17m44s）。这段时间渲染线程阻塞在
//   vkCreateRayTracingPipelinesKHR 里，窗口假死，改一次 shader 就是十几分钟不可用。
//
// 做法：
//   1) vkCreateDeferredOperationKHR 建一个延迟操作；
//   2) vkCreateRayTracingPipelinesKHR(deferredOperation=…) 立刻返回
//      VK_OPERATION_DEFERRED_KHR，编译工作在驱动内部待办；
//   3) 每帧在渲染线程调一次 vkDeferredOperationJoinKHR：驱动只做**一片**工作就返回
//      （VK_THREAD_DONE_KHR / VK_THREAD_IDLE_KHR），因此不会一次卡满整帧；
//   4) 返回 VK_SUCCESS 时取结果，成功则重建 SBT 并原子切换，失败则回滚到旧管线。
//
// 边界与前提：
//   · 必须先确认设备启用了 VK_KHR_deferred_host_operations。VkDevice 由 renderpearl
//     创建，本模块改不了它的扩展链，所以走 vkGetDeviceProcAddr 探测：拿不到函数
//     指针就返回 false，调用方回退到同步的 recreateMainRtPipeline()（现行为）。
//   · 编译总时长**不会**缩短，只是从"一次阻塞 15 分钟"变成"每帧几十毫秒、持续 15 分钟"。
//   · 各结构体（stages / groups / pci / shader modules）在操作完成前必须保持有效，
//     因此全部由本文件的静态状态持有，不能放栈上。
//   · 操作一旦提交就不能中途放弃（规范不允许销毁未完成的 deferred operation），
//     所以这里只做进度日志，不做超时取消。

#include "OrionInternal.h"
#include "TrackingHooks.h"

namespace
{

constexpr uint32_t RT_ASYNC_STAGE_COUNT = 6u;
constexpr uint32_t RT_ASYNC_GROUP_COUNT = 5u;

// 每 600 帧（约 10 s @60fps）打一条进度，避免日志刷屏。
constexpr uint32_t RT_ASYNC_PROGRESS_FRAME_INTERVAL = 600u;

struct RtAsyncRebuildState
{
    bool active = false;
    bool submitted = false;

    VkDeferredOperationKHR operation = VK_NULL_HANDLE;

    PFN_vkCreateDeferredOperationKHR createDeferredOperation = nullptr;
    PFN_vkDestroyDeferredOperationKHR destroyDeferredOperation = nullptr;
    PFN_vkDeferredOperationJoinKHR joinDeferredOperation = nullptr;
    PFN_vkGetDeferredOperationResultKHR getDeferredOperationResult = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR createRayTracingPipelines = nullptr;

    VkShaderModule modules[RT_ASYNC_STAGE_COUNT] = {};
    VkPipelineShaderStageCreateInfo stages[RT_ASYNC_STAGE_COUNT] = {};
    VkRayTracingShaderGroupCreateInfoKHR groups[RT_ASYNC_GROUP_COUNT] = {};

    // pci 里的指针指向上面的数组，必须与它们同寿命。
    VkRayTracingPipelineCreateInfoKHR pipelineCreateInfo{};

    VkPipeline candidatePipeline = VK_NULL_HANDLE;
    VkResult createResult = VK_ERROR_UNKNOWN;

    std::chrono::steady_clock::time_point startTime{};
    uint32_t frameCounter = 0u;
    char reason[64] = {};
};

RtAsyncRebuildState g_rtAsyncRebuild;

void destroyAsyncShaderModules()
{
    for(VkShaderModule& module : g_rtAsyncRebuild.modules)
    {
        if(module != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(g_device, module, nullptr);
            module = VK_NULL_HANDLE;
        }
    }
}

void destroyAsyncDeferredOperation()
{
    if(g_rtAsyncRebuild.operation != VK_NULL_HANDLE)
    {
        g_rtAsyncRebuild.destroyDeferredOperation(
            g_device,
            g_rtAsyncRebuild.operation,
            nullptr
        );

        g_rtAsyncRebuild.operation = VK_NULL_HANDLE;
    }
}

// 把新管线接进全局状态：重建 SBT → 成功则交换，失败则恢复旧 SBT 并销毁新管线。
// 逻辑与 RtPipeline.cpp 的 recreateMainRtPipeline() 尾部逐条对应（那边是同步版本）；
// 刻意复制而不是抽出公共函数，避免动到已经验证过的热重建路径。
bool adoptAsyncRtPipeline(VkPipeline fresh, const char* tag)
{
    if(fresh == VK_NULL_HANDLE)
    {
        return false;
    }

    vkDeviceWaitIdle(g_device);

    const VkPipeline oldPipeline = g_rayTracingPipeline;
    const VkBuffer oldSbtBuffer = g_sbtBuffer;
    const VkDeviceMemory oldSbtMemory = g_sbtMemory;

    g_rayTracingPipeline = fresh;
    g_sbtBuffer = VK_NULL_HANDLE;
    g_sbtMemory = VK_NULL_HANDLE;

    bool sbtRebuilt = false;

    try
    {
        createRayTracingSBT();
        sbtRebuilt = g_sbtBuffer != VK_NULL_HANDLE;
    }
    catch(...)
    {
        sbtRebuilt = false;
    }

    if(!sbtRebuilt)
    {
        g_sbtBuffer = oldSbtBuffer;
        g_sbtMemory = oldSbtMemory;
        g_rayTracingPipeline = oldPipeline;

        vkDeviceWaitIdle(g_device);
        vkDestroyPipeline(g_device, fresh, nullptr);

        RTLogFlushed(
            std::string("[RT PIPE ASYNC] SBT rebuild FAILED -> rolled back (") +
            (tag != nullptr ? tag : "?") +
            ")"
        );

        return false;
    }

    g_rtPipelinePendingDestroy = oldPipeline;
    g_rtSbtPendingDestroyBuffer = oldSbtBuffer;
    g_rtSbtPendingDestroyMemory = oldSbtMemory;

    return true;
}

void finishAsyncRtPipelineRebuild(VkResult result)
{
    RtAsyncRebuildState& state = g_rtAsyncRebuild;

    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - state.startTime
        ).count();

    VkPipeline fresh = state.candidatePipeline;

    // 先收掉操作对象与 shader module：此时驱动已不再引用它们。
    if(state.getDeferredOperationResult != nullptr &&
       state.operation != VK_NULL_HANDLE)
    {
        const VkResult operationResult =
            state.getDeferredOperationResult(g_device, state.operation);

        if(result == VK_SUCCESS && operationResult != VK_SUCCESS)
        {
            result = operationResult;
        }
    }

    destroyAsyncDeferredOperation();
    destroyAsyncShaderModules();

    state.candidatePipeline = VK_NULL_HANDLE;
    state.active = false;
    state.submitted = false;
    state.createResult = result;

    const std::string tag =
        (state.reason[0] != '\0') ? std::string(state.reason) : std::string("?");

    if(result != VK_SUCCESS || fresh == VK_NULL_HANDLE)
    {
        if(fresh != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(g_device);
            vkDestroyPipeline(g_device, fresh, nullptr);
        }

        RTLogFlushed(
            "[RT PIPE ASYNC] rebuild FAILED result=" +
            std::to_string(static_cast<int>(result)) +
            " ms=" +
            std::to_string(static_cast<long long>(elapsedMs)) +
            " tag=" + tag +
            " (previous pipeline kept)"
        );

        fprintf(
            stderr,
            "[RT PIPE ASYNC] rebuild FAILED result=%d ms=%.1f tag=%s\n",
            static_cast<int>(result),
            elapsedMs,
            tag.c_str()
        );
        fflush(stderr);

        return;
    }

    const bool adopted = adoptAsyncRtPipeline(fresh, tag.c_str());

    RTLogFlushed(
        std::string("[RT PIPE ASYNC] ") +
        (adopted ? "swapped" : "discarded") +
        " ms=" + std::to_string(static_cast<long long>(elapsedMs)) +
        " tag=" + tag
    );

    fprintf(
        stderr,
        "[RT PIPE ASYNC] %s ms=%.1f tag=%s\n",
        adopted ? "swapped" : "discarded",
        elapsedMs,
        tag.c_str()
    );
    fflush(stderr);
}

} // namespace

bool asyncRtPipelineRebuildSupported()
{
    RtAsyncRebuildState& state = g_rtAsyncRebuild;

    if(g_device == VK_NULL_HANDLE || g_rayTracingPipelineLayout == VK_NULL_HANDLE)
    {
        return false;
    }

    if(state.createDeferredOperation != nullptr)
    {
        return true;
    }

    // VkDevice 由 renderpearl 创建，本模块无法修改它的扩展链，只能探测：
    // 扩展未启用时 vkGetDeviceProcAddr 对这四个入口返回 NULL。
    state.createDeferredOperation =
        reinterpret_cast<PFN_vkCreateDeferredOperationKHR>(
            vkGetDeviceProcAddr(g_device, "vkCreateDeferredOperationKHR"));
    state.destroyDeferredOperation =
        reinterpret_cast<PFN_vkDestroyDeferredOperationKHR>(
            vkGetDeviceProcAddr(g_device, "vkDestroyDeferredOperationKHR"));
    state.joinDeferredOperation =
        reinterpret_cast<PFN_vkDeferredOperationJoinKHR>(
            vkGetDeviceProcAddr(g_device, "vkDeferredOperationJoinKHR"));
    state.getDeferredOperationResult =
        reinterpret_cast<PFN_vkGetDeferredOperationResultKHR>(
            vkGetDeviceProcAddr(g_device, "vkGetDeferredOperationResultKHR"));
    state.createRayTracingPipelines =
        reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
            vkGetDeviceProcAddr(g_device, "vkCreateRayTracingPipelinesKHR"));

    const bool supported =
        state.createDeferredOperation != nullptr &&
        state.destroyDeferredOperation != nullptr &&
        state.joinDeferredOperation != nullptr &&
        state.getDeferredOperationResult != nullptr &&
        state.createRayTracingPipelines != nullptr;

    RTLogFlushed(
        std::string("[RT PIPE ASYNC] deferred host operations ") +
        (supported ? "available" : "unavailable -> sync fallback")
    );

    return supported;
}

bool beginAsyncRtPipelineRebuild(const char* reason)
{
    RtAsyncRebuildState& state = g_rtAsyncRebuild;

    if(state.active || state.submitted)
    {
        return false;
    }

    if(!asyncRtPipelineRebuildSupported())
    {
        return false;
    }

    state.reason[0] = '\0';

    if(reason != nullptr)
    {
        std::snprintf(state.reason, sizeof(state.reason), "%s", reason);
    }

    // 管线形态与 Device.cpp 里的主管线逐项一致（6 stage / 5 group / 递归深度 1）。
    const VkShaderStageFlagBits kinds[RT_ASYNC_STAGE_COUNT] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_MISS_BIT_KHR,
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR};

    const char* spvNames[RT_ASYNC_STAGE_COUNT] = {
        "shaders/rt_test.rgen.spv",
        "shaders/rt_test.rchit.spv",
        "shaders/rt_test.rmiss.spv",
        "shaders/rt_test.rahit.spv",
        "shaders/rt_test_translucent.rchit.spv",
        "shaders/rt_test_translucent.rahit.spv"};

    for(uint32_t i = 0u; i < RT_ASYNC_STAGE_COUNT; ++i)
    {
        try
        {
            const std::vector<uint8_t> spv = loadSpv(spvNames[i]);
            state.modules[i] = createShaderModule(g_device, spv);
        }
        catch(const std::exception& error)
        {
            RTLogFlushed(
                std::string("[RT PIPE ASYNC] load shader failed: ") +
                spvNames[i] + " (" + error.what() + ")"
            );

            destroyAsyncShaderModules();
            return false;
        }

        state.stages[i] = VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr,
            0u,
            kinds[i],
            state.modules[i],
            "main",
            nullptr};
    }

    // 组表与 Device.cpp:226-234 相同：
    //   group0 = raygen，group1 = rchit+rahit（不透明），group2 = miss，
    //   group3 = translucent rchit+rahit，group4 = rchit+rahit（水面）。
    state.groups[0] = {
        VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
        VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0u,
        VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
    state.groups[1] = {
        VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
        VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
        VK_SHADER_UNUSED_KHR, 1u, 3u, VK_SHADER_UNUSED_KHR};
    state.groups[2] = {
        VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
        VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 2u,
        VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
    state.groups[3] = {
        VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
        VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
        VK_SHADER_UNUSED_KHR, 4u, 5u, VK_SHADER_UNUSED_KHR};
    state.groups[4] = {
        VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
        VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
        VK_SHADER_UNUSED_KHR, 1u, 3u, VK_SHADER_UNUSED_KHR};

    state.pipelineCreateInfo = VkRayTracingPipelineCreateInfoKHR{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};

    state.pipelineCreateInfo.stageCount = RT_ASYNC_STAGE_COUNT;
    state.pipelineCreateInfo.pStages = state.stages;
    state.pipelineCreateInfo.groupCount = RT_ASYNC_GROUP_COUNT;
    state.pipelineCreateInfo.pGroups = state.groups;
    state.pipelineCreateInfo.maxPipelineRayRecursionDepth = 1u;
    state.pipelineCreateInfo.layout = g_rayTracingPipelineLayout;

    if(g_ommNativeEnabled)
    {
        state.pipelineCreateInfo.flags |=
            VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
    }

    if(state.createDeferredOperation(
           g_device,
           nullptr,
           &state.operation) != VK_SUCCESS ||
       state.operation == VK_NULL_HANDLE)
    {
        RTLogFlushed("[RT PIPE ASYNC] vkCreateDeferredOperationKHR failed");

        destroyAsyncShaderModules();
        return false;
    }

    state.candidatePipeline = VK_NULL_HANDLE;
    state.startTime = std::chrono::steady_clock::now();
    state.frameCounter = 0u;
    state.active = true;
    state.submitted = true;

    // cache 传 VK_NULL_HANDLE：与 recreateMainRtPipeline 的既有做法一致
    // （RtPipeline.cpp 注释记录了"传 cache 曾让 RT 编译从 3-8 s 劣化到 130-243 s"）。
    const VkResult createResult = state.createRayTracingPipelines(
        g_device,
        state.operation,
        VK_NULL_HANDLE,
        1u,
        &state.pipelineCreateInfo,
        nullptr,
        &state.candidatePipeline
    );

    state.createResult = createResult;

    RTLogFlushed(
        std::string("[RT PIPE ASYNC] submitted tag=") +
        (state.reason[0] != '\0' ? state.reason : "?") +
        " result=" + std::to_string(static_cast<int>(createResult))
    );

    if(createResult == VK_SUCCESS)
    {
        // 驱动选择同步完成（小管线或已命中驱动内部缓存）。
        finishAsyncRtPipelineRebuild(VK_SUCCESS);
        return true;
    }

    if(createResult != VK_OPERATION_DEFERRED_KHR)
    {
        finishAsyncRtPipelineRebuild(createResult);
        return false;
    }

    return true;
}

bool asyncRtPipelineRebuildActive()
{
    return g_rtAsyncRebuild.active;
}

void pollAsyncRtPipelineRebuild()
{
    RtAsyncRebuildState& state = g_rtAsyncRebuild;

    if(!state.active)
    {
        return;
    }

    if(g_device == VK_NULL_HANDLE)
    {
        return;
    }

    ++state.frameCounter;

    // 每帧只让驱动做一片工作；这个调用在驱动内部有明确的分片边界，
    // 不会像同步版本那样一次卡满整个编译时长。
    const VkResult joinResult =
        state.joinDeferredOperation(g_device, state.operation);

    if(joinResult == VK_SUCCESS)
    {
        finishAsyncRtPipelineRebuild(VK_SUCCESS);
        return;
    }

    if(joinResult != VK_THREAD_DONE_KHR &&
       joinResult != VK_THREAD_IDLE_KHR)
    {
        finishAsyncRtPipelineRebuild(joinResult);
        return;
    }

    if((state.frameCounter % RT_ASYNC_PROGRESS_FRAME_INTERVAL) == 0u)
    {
        const double elapsedMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - state.startTime
            ).count();

        const std::string text =
            "[RT PIPE ASYNC] compiling... " +
            std::to_string(static_cast<long long>(elapsedMs / 1000.0)) +
            "s (frames still rendering with previous pipeline)";

        fprintf(stderr, "%s\n", text.c_str());
        fflush(stderr);
        RTLogFlushed(text);
    }
}
