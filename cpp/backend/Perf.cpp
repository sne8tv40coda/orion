// Orion backend — Perf 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// 解析一次并缓存（同 perfTimestampsEnabled：改完必须重启客户端）。
uint32_t perfL10AblationRequestedStage()
{
    static const uint32_t request = []() -> uint32_t
    {
        const char* raw = std::getenv("ORION_PERF_ABLATION");
        if(raw == nullptr || std::string(raw) == "0")
        {
            return PERF_L10_ABLATION_OFF;
        }
        const std::string text(raw);
        const std::size_t alt = text.find("alt:");
        if(alt != std::string::npos)
        {
            std::string list = text.substr(alt + 4);
            std::size_t begin = 0u;
            while(begin <= list.size() &&
                  g_perfL10AblationAltStages.size() < PERF_L10_ABLATION_ALT_MAX)
            {
                const std::size_t comma = list.find(',', begin);
                const std::string item = list.substr(
                    begin,
                    comma == std::string::npos ? std::string::npos : comma - begin);
                const int value = std::atoi(item.c_str());
                if(value >= 0)
                {
                    g_perfL10AblationAltStages.push_back(
                        static_cast<uint32_t>(value % PERF_L10_ABLATION_STAGE_COUNT));
                }
                if(comma == std::string::npos)
                {
                    break;
                }
                begin = comma + 1u;
            }
            return PERF_L10_ABLATION_ALT_FLAG;
        }
        const std::size_t colon = text.find("pin:");
        if(colon == std::string::npos)
        {
            return PERF_L10_ABLATION_SWEEP;
        }
        const int value = std::atoi(text.c_str() + colon + 4);
        if(value < 0)
        {
            return PERF_L10_ABLATION_SWEEP;
        }
        return static_cast<uint32_t>(value % PERF_L10_ABLATION_STAGE_COUNT);
    }();
    return request;
}
bool perfL10AblationEnabled()
{
    return perfL10AblationRequestedStage() != PERF_L10_ABLATION_OFF;
}
bool perfL10AblationAltMode(uint32_t request)
{
    return (request & PERF_L10_ABLATION_ALT_FLAG) != 0u;
}


uint32_t perfL10AblationStage()
{
    return perfL10AblationEnabled()
        ? g_perfL10AblationStage
        : 0u;
}


// =====================================================
// Perf-L10.0 - non-blocking per-pass GPU timestamps
// =====================================================
// A frame is cut into contiguous passes by boundary timestamps.  A pass time is
// the delta between two neighbouring boundaries, so every pass time adds up to
// the whole-frame GPU time and no part of the frame stays unattributed.
// Timestamp results are read only when a ring slot has completed.  No
// query read uses VK_QUERY_RESULT_WAIT_BIT, so diagnostics cannot add a
// CPU/GPU synchronization point to the measured frame.
// A1（2026-10-01）：这两个诊断仪器默认**关闭**，运行时用环境变量按需打开。
// 原先两个常量都写死成 true，与上面那句 "Disabled for normal play" 自相矛盾：
// 每帧会往命令缓冲写 PERF_L10_GPU_BOUNDARY_COUNT(14) 个 timestamp、每帧轮询一次
// query pool（vkGetQueryPoolResults），另加窗口摘要日志与强制 flush。
//   ORION_PERF_TIMESTAMPS=1 ⇒ 打开（只读一次，改完必须重启客户端）
//   未设 / =0                  ⇒ 关闭（默认）
// 两个开关共用一条：CPU 那条的作用就是给 GPU 那条做"CPU 受限 vs GPU 受限"对账。
bool perfTimestampsEnabled()
{
    static const bool enabled = []() {
        const char* raw = std::getenv("ORION_PERF_TIMESTAMPS");
        if(raw == nullptr)
        {
            return false;
        }
        return std::string(raw) != "0";
    }();
    return enabled;
}

// 2026-10-24 远场太阳阴影的同局 A/B（ORION_SUNSHADOW_FAR_AB=1）。
//
// 为什么需要：远场阴影的收益只能靠 rt= 的变化看出来，而 rt 的成本跟场景强相关
// （实例数 / 三角形数每局差两三倍），跨局相减等于把场景差当成开关键。所以按
// **同一个窗口常量**（PERF_L10_GPU_WINDOW_FRAMES）交替开/关，并在 [PERF L10 GPU]
// 行上打 abFar=，让两臂落在不同窗口、同一个世界里。
bool sunShadowFarAbEnabled()
{
    static const bool s_enabled = []() -> bool
    {
        const char* raw = std::getenv("ORION_SUNSHADOW_FAR_AB");

        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    return s_enabled;
}

// 0 = 关臂，1 = 开臂。窗口索引与 [PERF L10 GPU] 完全同源。
uint32_t sunShadowFarAbArm()
{
    return static_cast<uint32_t>(
        (g_perfL10FrameSerial / PERF_L10_GPU_WINDOW_FRAMES) & 1u);
}


// 2026-10-23 心跳：每 5 秒一行，回答"死前还活着几帧 / 是冻死还是秒退"。
// 只写一行且 flush，成本可忽略；若日志里心跳停在某帧之后什么都没有，
// 说明进程不是死在我们自己的代码路径上（被外部终止）。
void RTXHeartbeatTick()
{
    static auto s_lastHeartbeat = std::chrono::steady_clock::now();
    static uint64_t s_lastFrame = std::numeric_limits<uint64_t>::max();

    g_rtxLastFrameSerial.store(
        static_cast<uint64_t>(g_perfL10FrameSerial),
        std::memory_order_relaxed
    );

    const auto now = std::chrono::steady_clock::now();

    const bool timeElapsed =
        s_lastFrame == std::numeric_limits<uint64_t>::max() ||
        std::chrono::duration<float>(now - s_lastHeartbeat).count() >= 5.0f;
    const bool frameElapsed =
        s_lastFrame == std::numeric_limits<uint64_t>::max() ||
        (static_cast<uint64_t>(g_perfL10FrameSerial) - s_lastFrame) >= 60u;

    if(!timeElapsed && !frameElapsed)
    {
        return;
    }

    s_lastHeartbeat = now;
    s_lastFrame = static_cast<uint64_t>(g_perfL10FrameSerial);

    const char* phase = g_rtxPhaseCurrent.load(std::memory_order_relaxed);

    RTLogFlushed(
        std::string("[HEARTBEAT] frame=") +
        std::to_string(g_perfL10FrameSerial) +
        " phase=" + (phase != nullptr ? phase : "?")
    );
}

// =====================================================
// 2026-10-15：批次上限的 env 覆盖（HANDOFF-2026-10-15 §C：g_sectionRebuildBatchCap
// 的全部出现点 1687/58118/58418/58707/58788/59109 都没有 getenv，而 budget 有
// ORION_GEOMETRY_BUDGET_MS —— 消费端两条腿只有一条可 A/B）。
//
// 写法照抄 sectionRebuildTimeBudgetMs() 的 env 解析（static lambda 一次解析，
// 未设/<=0 = 不生效）。语义：**只在取用时把有效上限压到 env 值**，自适应逻辑
// （中断减半 :58707/:58788、批内完成 +2 :59109）一行不动 —— 这样 =X 的对照局
// 与基线局只差"上限被压"，其余状态机逐位一致；不设 env 时路径逐位回到原行为。
// 默认值 24 不改（HANDOFF §C：加载变慢是用户可感的退化，先量曲线再谈默认）。
// =====================================================
size_t sectionRebuildBatchCapOverride()
{
    static const size_t overrideCap = []() {
        const char* raw = std::getenv("ORION_GEOMETRY_BATCH_CAP");
        if(raw == nullptr || raw[0] == '\0')
        {
            return static_cast<size_t>(0u);
        }
        const long parsed = std::atol(raw);
        return parsed > 0
            ? static_cast<size_t>(parsed)
            : static_cast<size_t>(0u);
    }();
    return overrideCap;
}
size_t effectiveSectionRebuildBatchCap()
{
    const size_t overrideCap = sectionRebuildBatchCapOverride();

    if(overrideCap > 0u)
    {
        return overrideCap;
    }

    return g_sectionRebuildBatchCap;
}
double sectionRebuildTimeBudgetMs()
{
    static const double overrideMs = []() {
        const char* raw = std::getenv("ORION_GEOMETRY_BUDGET_MS");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 0.0;
        }
        const double parsed = std::atof(raw);
        return parsed > 0.0 ? parsed : 0.0;
    }();

    if(overrideMs > 0.0)
    {
        g_sectionRebuildBudgetMirrorMs.store(
            overrideMs,
            std::memory_order_relaxed);

        return overrideMs;
    }

    static double cachedGpuFrameMs = 0.0;
    const double gpuSamples =
        static_cast<double>(g_perfL10GpuPassWindow.samples);
    if(gpuSamples > 0.0)
    {
        cachedGpuFrameMs = g_perfL10GpuPassWindow.sumFrameMs / gpuSamples;
    }

    if(cachedGpuFrameMs <= 0.0)
    {
        g_sectionRebuildBudgetMirrorMs.store(
            SECTION_REBUILD_TIME_BUDGET_MS,
            std::memory_order_relaxed);

        return SECTION_REBUILD_TIME_BUDGET_MS;
    }

    const double adaptiveBudgetMs = std::max(
        SECTION_REBUILD_TIME_BUDGET_MS,
        std::min(
            SECTION_REBUILD_TIME_BUDGET_ADAPTIVE_MAX_MS,
            cachedGpuFrameMs * SECTION_REBUILD_TIME_BUDGET_GPU_FRACTION
        )
    );

    g_sectionRebuildBudgetMirrorMs.store(
        adaptiveBudgetMs,
        std::memory_order_relaxed);

    return adaptiveBudgetMs;
}

// ============================================================================
// 2026-10-08（B）：**水相关 LOD 跟着实测 GPU 帧时间自适应**。
// ============================================================================
// 背景（用户报"一接近水就疯狂掉帧"）：水像素是整帧最吃射线的像素 —— 反射/折射
// 两条 walker 各 ≤8 步、水底 GI 弹射、太阳透射 march ≤6 步、局部光 RIS 与阴影
// 射线，合计十几到二十条着色射线/像素（rt_test.rgen:14565-14567 的作者量化），
// 而开销与水面的**屏幕占比**成正比 ⇒ 一靠近，占比爆炸，帧时间没有上限。
//
// 已经有一批"按距离"的闸门（焦散 96 格、walker 步数 48/96 档、水底 GI
// 32/64/128/160 档），但它们是**固定**的：既不知道当前帧贵不贵，也不知道收紧后
// 够不够。这里补上反馈环 —— 复用 GPU 时间戳窗口（与 sectionRebuildTimeBudgetMs
// 同一份平滑值），把实测帧时间发给 rgen，由它按需收紧水面那几道距离闸。
//
//   · 未设 ORION_WATER_ADAPTIVE / =1 ⇒ 发送实测 ms（着色器 12ms 以下不收紧，
//     12→22ms 线性收到 0.35 倍距离，22ms 以上保持 0.35）；
//   · ORION_WATER_ADAPTIVE=0 ⇒ 发送 0 ⇒ 着色器恒用全质量距离 ⇒ **与改动前逐位相同**；
//   · 拿不到 GPU 时间戳（窗口样本为 0 且从未有过）⇒ 同样发 0 ⇒ 退化为旧行为。
double smoothedGpuFrameMs()
{
    static double cached = 0.0;
    const double samples = static_cast<double>(g_perfL10GpuPassWindow.samples);
    if(samples > 0.0)
    {
        cached = g_perfL10GpuPassWindow.sumFrameMs / samples;
    }
    return cached;
}
bool waterAdaptiveEnabled()
{
    static const bool enabled = []() {
        const char* raw = std::getenv("ORION_WATER_ADAPTIVE");
        return raw == nullptr || raw[0] != '0';
    }();
    return enabled;
}

// ---------------------------------------------------------------------------
// 2026-10-12：SER 的 A/B 从**编译期常量**改成**运行期 ladder**，并把 "SER off"
// 也做成其中一档 —— 于是**一局**就能同时拿到官方式的关键对照。
//
// 动机（NVIDIA 官方 "Path Tracing Optimization in Indiana Jones: SER and Live
// State Reductions"，2025-05-15）：SER 的收益取决于 **RT live state spilled
// bytes**。同一 pass、同一份代码，spilled 从 222 B 降到 84 B，SER 的加速从
// −11% 变成 −24%。而本工程 2026-10-07 那次 "key 形状无所谓" 的结论（gpuRt
// 15.66/16.10/16.17/15.66，全在 ±1.5% 噪声带内）**没有控住 spill 这个前置
// 条件** —— 若 RayGen 本身 spill 很多，L2 已经饱和，key 好坏的差异会被淹没。
// 所以要在**同一局、同一场景**里同时采到 SER off 与各 key 形状，才能把
// "SER 有没有用" 与 "key 形状有没有用" 两件事分开。
//
// ladder（5 档 × PERF_L10_SER_AB_WINDOW_FRAMES=60 帧 = 300 帧一个周期）：
//   phase 0  serToggle=0            ⇒ **SER 完全关闭**（官方 SER OFF 基线）
//   phase 1  serToggle=1, mode=1    ⇒ 现网 pin 的形状（自定义 key）
//   phase 2  serToggle=1, mode=4    ⇒ 后置 instance-class key
//   phase 3  serToggle=1, mode=5    ⇒ 全融合 hitObjectTraceReorderExecuteEXT
//   phase 4  serToggle=1, mode=6    ⇒ 探针臂（DISTANCE_ONLY 走 rgen 侧查询）
//
// 读数：`[PERF L10 GPU]` 每窗口打 ser=<0/1> serMode=<0/1/4/5/6>，
// 窗口累加器按同一相位分桶（slot.serToggle / slot.serMode），
// 所以一局的数据可以直接按 (ser, serMode) 分组求均值。
//
// ⚠ 与 ORION_PERF_ABLATION 的 alt: 交错**不要同时开**：两者都用
//   g_perfL10FrameSerial 做窗口轮换，会互相切碎，窗口无法归因到单一档位。
// ⚠ 只读一次，改完必须重启客户端。
// ---------------------------------------------------------------------------
bool perfL10SerLadderEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_SER_LADDER");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    return cached;
}

// 当前窗口的 ladder 档位值（0/1/4/5/6）。**抽成函数**而不是全局变量：
// 两个 update 函数的调用顺序是 updatePerfL10SerToggle() -> 
// updatePerfL10SerPathToggles()，若用一个全局变量传值，前者会读到**上一帧**
// 的值，换档边界上 ser 与 serMode 会错配一帧。两边各自按同一个 frameSerial
// 现算，就天然同帧一致。
uint32_t perfL10SerLadderValue()
{
    static const uint32_t kSerLadder[5] = { 0u, 1u, 4u, 5u, 6u };
    const uint32_t serLadderPhase = static_cast<uint32_t>(
        (g_perfL10FrameSerial / PERF_L10_SER_AB_WINDOW_FRAMES) % 5u);

    return kSerLadder[serLadderPhase];
}


void updatePerfL10SerPathToggles()
{
    if(perfL10SerLadderEnabled())
    {
        const uint32_t serLadderValue = perfL10SerLadderValue();

        g_perfL10RtaoSkipToggle = 1u;
        g_perfL10SerMode = serLadderValue;
        g_perfL10SerPathsPhase = serLadderValue;
        g_perfL10SerRadToggle = serLadderValue;
        g_perfL10SerVisToggle = 0u;
        g_perfL10SerPrimToggle = 1u;
        return;
    }
    if constexpr(PERF_L10_ENABLE_RTAO_SKIP_AB)
    {
        g_perfL10RtaoSkipToggle = static_cast<uint32_t>(
            (g_perfL10FrameSerial / PERF_L10_SER_AB_WINDOW_FRAMES) & 1u);
    }
    else
    {
        g_perfL10RtaoSkipToggle = 1u;
    }

    if constexpr(PERF_L10_ENABLE_SER_MODE_AB)
    {
        static const uint32_t kSerModePhases[4] = { 1u, 4u, 5u, 6u };
        const uint32_t serModePhase =
            kSerModePhases[(g_perfL10FrameSerial / PERF_L10_SER_AB_WINDOW_FRAMES) & 3u];
        g_perfL10SerMode = serModePhase;
        g_perfL10SerPathsPhase = serModePhase;
        g_perfL10SerRadToggle = serModePhase;
    }
    else
    {
        g_perfL10SerMode = 6u;
        g_perfL10SerRadToggle = 6u;

        g_perfL10SerPathsPhase = g_perfL10RtaoSkipToggle;
    }

    g_perfL10SerVisToggle = 0u;
    g_perfL10SerPrimToggle = g_perfL10RtaoSkipToggle;
}


void updatePerfL10SerToggle()
{
    if(perfL10SerLadderEnabled())
    {
        g_perfL10SerToggle = perfL10SerLadderValue();
        return;
    }

    g_perfL10SerToggle = PERF_L10_ENABLE_SER_AB
        ? static_cast<uint32_t>(
              (g_perfL10FrameSerial / PERF_L10_SER_AB_WINDOW_FRAMES) & 1u)
        : 1u;
}


// Called at the top of the frame-recording function: captures the frame
// interval and arms the CPU-work clock for this frame.
void samplePerfL10CpuFrame()
{
    if(!perfTimestampsEnabled())
    {
        return;
    }

    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();

    g_perfL10CpuWorkStart = now;

    if(!g_perfL10CpuFrameHavePrev)
    {
        g_perfL10CpuFrameHavePrev = true;
        g_perfL10CpuFramePrev = now;
        return;
    }

    const double frameMs =
        std::chrono::duration<double, std::milli>(
            now - g_perfL10CpuFramePrev
        ).count();

    g_perfL10CpuFramePrev = now;

    g_perfL10CpuFrameWindow.samples += 1u;
    g_perfL10CpuFrameWindow.sumMs += frameMs;

    if(
        g_perfL10CpuFrameWindow.samples == 1u ||
        frameMs < g_perfL10CpuFrameWindow.minMs
    )
    {
        g_perfL10CpuFrameWindow.minMs = frameMs;
    }
    if(frameMs > g_perfL10CpuFrameWindow.maxMs)
    {
        g_perfL10CpuFrameWindow.maxMs = frameMs;
    }
}


// Called at the end of the frame-recording function: closes the CPU-work clock
// and emits the window summary once it is full.
void finishPerfL10CpuFrame()
{
    if(!perfTimestampsEnabled())
    {
        return;
    }

    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();

    const double workMs =
        std::chrono::duration<double, std::milli>(
            now - g_perfL10CpuWorkStart
        ).count();

    g_perfL10CpuFrameWindow.sumWorkMs += workMs;
    if(workMs > g_perfL10CpuFrameWindow.maxWorkMs)
    {
        g_perfL10CpuFrameWindow.maxWorkMs = workMs;
    }

    if(g_perfL10CpuFrameWindow.samples < PERF_L10_CPU_FRAME_WINDOW_FRAMES)
    {
        return;
    }

    const double avgMs =
        g_perfL10CpuFrameWindow.sumMs /
        static_cast<double>(g_perfL10CpuFrameWindow.samples);
    const double avgWorkMs =
        g_perfL10CpuFrameWindow.sumWorkMs /
        static_cast<double>(g_perfL10CpuFrameWindow.samples);

    std::stringstream log;
    log
        << "[PERF L10 CPU]"
        << " frame=" << g_perfL10FrameSerial
        << " windowFrames=" << g_perfL10CpuFrameWindow.samples
        << " avgMs=" << std::fixed << std::setprecision(3) << avgMs
        << " minMs=" << g_perfL10CpuFrameWindow.minMs
        << " maxMs=" << g_perfL10CpuFrameWindow.maxMs
        << " workAvgMs=" << std::setprecision(3) << avgWorkMs
        << " workMaxMs=" << g_perfL10CpuFrameWindow.maxWorkMs
        << " cpuFps=" << std::setprecision(1) << (1000.0 / avgMs);

    RTLog(log.str());

    g_perfL10CpuFrameWindow = PerfL10CpuFrameWindow{};
}


void destroyPerfL10GpuTimestampPool()
{
    if(g_perfL10GpuTimestampPool != VK_NULL_HANDLE && g_device != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(
            g_device,
            g_perfL10GpuTimestampPool,
            nullptr
        );
    }

    g_perfL10GpuTimestampPool = VK_NULL_HANDLE;
    g_perfL10TimestampPeriodNs = 0.0;
    g_perfL10GpuActiveSlot = -1;
    g_perfL10GpuPassWindow = PerfL10GpuPassWindow{};

    for(PerfL10GpuTimestampSlot& slot : g_perfL10GpuTimestampSlots)
    {
        slot = {};
    }
}


bool ensurePerfL10GpuTimestampPool()
{
    if(g_perfL10GpuTimestampPool != VK_NULL_HANDLE)
    {
        return true;
    }

    if(g_device == VK_NULL_HANDLE || g_physicalDevice == VK_NULL_HANDLE)
    {
        return false;
    }

    VkQueryPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    createInfo.queryCount =
        PERF_L10_GPU_TIMESTAMP_RING_SIZE *
        PERF_L10_GPU_BOUNDARY_COUNT;

    const VkResult result = vkCreateQueryPool(
        g_device,
        &createInfo,
        nullptr,
        &g_perfL10GpuTimestampPool
    );

    if(result != VK_SUCCESS)
    {
        g_perfL10GpuTimestampPool = VK_NULL_HANDLE;

        RTLog(
            "[PERF L10 GPU] timestampQueryPoolCreate=" +
            std::to_string(static_cast<int>(result))
        );

        return false;
    }

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(
        g_physicalDevice,
        &properties
    );

    g_perfL10TimestampPeriodNs =
        static_cast<double>(properties.limits.timestampPeriod);

    RTLog(
        "[PERF L10 GPU] timestampQueryPool=ready periodNs=" +
        std::to_string(g_perfL10TimestampPeriodNs) +
        " passes=" +
        std::to_string(PERF_L10_GPU_SCOPE_COUNT)
    );

    return true;
}
float giRayMaxDistanceConfigured()
{
    static const float s_value = []() -> float
    {
        const char* raw = std::getenv("ORION_GI_MAX_DISTANCE");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 96.0f;
        }

        const float parsed = static_cast<float>(std::atof(raw));

        return parsed > 0.0f ? parsed : 0.0f;
    }();

    return s_value;
}
bool giRayMaxDistanceAbEnabled()
{
    static const bool s_enabled = []() -> bool
    {
        const char* raw = std::getenv("ORION_GI_MAX_DISTANCE_AB");

        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    return s_enabled;
}
float giRayMaxDistanceEffective()
{
    const float configured = giRayMaxDistanceConfigured();

    if(configured <= 0.0f)
    {
        return 0.0f;
    }

    if(!giRayMaxDistanceAbEnabled())
    {
        return configured;
    }

    return ((g_perfL10GpuWindowsLogged & 1u) == 0u) ? 0.0f : configured;
}
void logPerfL10GpuPassWindow(
    uint64_t frameSerial,
    uint32_t renderWidth,
    uint32_t renderHeight
)
{
    const double sampleCount =
        static_cast<double>(g_perfL10GpuPassWindow.samples);

    if(sampleCount <= 0.0)
    {
        return;
    }

    std::stringstream averages;
    std::stringstream maxima;

    const uint32_t stage =
        g_perfL10GpuPassWindow.stage < PERF_L10_ABLATION_STAGE_COUNT
            ? g_perfL10GpuPassWindow.stage
            : 0u;

    averages
        << "[PERF L10 GPU]"
        << " frame=" << frameSerial
        << " win=" << g_perfL10GpuPassWindow.samples
        << " ablate=" << PERF_L10_ABLATION_STAGE_NAMES[stage]
        << " ser=" << (g_perfL10GpuPassWindow.serToggle != 0u ? 1 : 0)
        << " serMode=" << g_perfL10GpuPassWindow.serMode
        << " aoSkip=" << g_perfL10GpuPassWindow.serPaths
        << " abFar=" << g_perfL10GpuPassWindow.abFarArm
        << " spp=" << g_perfL10SppForLog.load()
        << " giMax=" << g_perfL10GpuPassWindow.giMaxDistance
        << " pom=" << g_perfL10GpuPassWindow.pomDepth
        << " dispatch=" << renderWidth << "x" << renderHeight
        << " avgMs frame=" << std::fixed << std::setprecision(2)
        << (g_perfL10GpuPassWindow.sumFrameMs / sampleCount);

    maxima
        << "[PERF L10 GPU]"
        << " frame=" << frameSerial
        << " win=" << g_perfL10GpuPassWindow.samples
        << " ablate=" << PERF_L10_ABLATION_STAGE_NAMES[stage]
        << " ser=" << (g_perfL10GpuPassWindow.serToggle != 0u ? 1 : 0)
        << " serMode=" << g_perfL10GpuPassWindow.serMode
        << " aoSkip=" << g_perfL10GpuPassWindow.serPaths
        << " abFar=" << g_perfL10GpuPassWindow.abFarArm
        << " spp=" << g_perfL10SppForLog.load()
        << " giMax=" << g_perfL10GpuPassWindow.giMaxDistance
        << " pom=" << g_perfL10GpuPassWindow.pomDepth
        << " maxMs frame=" << std::fixed << std::setprecision(2)
        << g_perfL10GpuPassWindow.maxFrameMs;

    for(uint32_t scope = 0u; scope < PERF_L10_GPU_SCOPE_COUNT; ++scope)
    {
        averages
            << " " << PERF_L10_GPU_SCOPE_NAMES[scope] << "="
            << std::setprecision(2)
            << (g_perfL10GpuPassWindow.sumScopeMs[scope] / sampleCount);

        maxima
            << " " << PERF_L10_GPU_SCOPE_NAMES[scope] << "="
            << std::setprecision(2)
            << g_perfL10GpuPassWindow.maxScopeMs[scope];
    }

    RTLog(averages.str());
    RTLog(maxima.str());

    g_perfL10GpuPassWindow = PerfL10GpuPassWindow{};

    g_perfL10GpuWindowsLogged += 1u;
}


// Reads the previous occupant of a ring slot.  A frame that bailed out early
// leaves some boundaries unwritten, so anything short of the full set is
// discarded rather than reported as a partial pass.
void pollPerfL10GpuTimestampSlot(
    uint32_t slotIndex
)
{
    PerfL10GpuTimestampSlot& slot =
        g_perfL10GpuTimestampSlots[slotIndex];

    if(g_perfL10GpuTimestampPool == VK_NULL_HANDLE)
    {
        return;
    }

    uint64_t results[PERF_L10_GPU_BOUNDARY_COUNT * 2u]{};

    const VkResult result = vkGetQueryPoolResults(
        g_device,
        g_perfL10GpuTimestampPool,
        slotIndex * PERF_L10_GPU_BOUNDARY_COUNT,
        PERF_L10_GPU_BOUNDARY_COUNT,
        sizeof(results),
        results,
        sizeof(uint64_t) * 2u,
        VK_QUERY_RESULT_64_BIT |
        VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
    );

    if(result != VK_SUCCESS)
    {
        return;
    }

    for(uint32_t boundary = 0u;
        boundary < PERF_L10_GPU_BOUNDARY_COUNT;
        ++boundary)
    {
        if(results[boundary * 2u + 1u] == 0u)
        {
            return;
        }
    }

    if(
        g_perfL10GpuPassWindow.samples > 0u &&
        (g_perfL10GpuPassWindow.stage != slot.stage ||
         g_perfL10GpuPassWindow.serToggle != slot.serToggle ||
         g_perfL10GpuPassWindow.serPaths != slot.serPaths ||
         g_perfL10GpuPassWindow.serMode != slot.serMode)
    )
    {
        g_perfL10GpuPassWindow = PerfL10GpuPassWindow{};
    }

    g_perfL10GpuPassWindow.stage = slot.stage;
    g_perfL10GpuPassWindow.serToggle = slot.serToggle;
    g_perfL10GpuPassWindow.serPaths = slot.serPaths;
    g_perfL10GpuPassWindow.serMode = slot.serMode;

    if(g_perfL10GpuPassWindow.samples == 0u)
    {
        g_perfL10GpuPassWindow.abFarArm =
            sunShadowFarAbEnabled() ? sunShadowFarAbArm() : 1u;

        g_perfL10GpuPassWindow.giMaxDistance =
            g_perfL10GiMaxForLog.load(std::memory_order_relaxed);

        g_perfL10GpuPassWindow.pomDepth =
            g_perfL10PomForLog.load(std::memory_order_relaxed);
    }

    double frameMs = 0.0;

    for(uint32_t scope = 0u; scope < PERF_L10_GPU_SCOPE_COUNT; ++scope)
    {
        const uint64_t beginTick = results[scope * 2u];
        const uint64_t endTick = results[(scope + 1u) * 2u];

        const double scopeMs =
            endTick >= beginTick
                ? static_cast<double>(endTick - beginTick) *
                      g_perfL10TimestampPeriodNs /
                      1000000.0
                : 0.0;

        g_perfL10GpuPassWindow.sumScopeMs[scope] += scopeMs;

        if(scopeMs > g_perfL10GpuPassWindow.maxScopeMs[scope])
        {
            g_perfL10GpuPassWindow.maxScopeMs[scope] = scopeMs;
        }

        frameMs += scopeMs;
    }

    g_perfL10GpuPassWindow.samples += 1u;
    g_perfL10GpuPassWindow.sumFrameMs += frameMs;

    if(frameMs > g_perfL10GpuPassWindow.maxFrameMs)
    {
        g_perfL10GpuPassWindow.maxFrameMs = frameMs;
    }

    if(g_perfL10GpuPassWindow.samples >= PERF_L10_GPU_WINDOW_FRAMES)
    {
        logPerfL10GpuPassWindow(
            slot.frameSerial,
            slot.renderWidth,
            slot.renderHeight
        );
    }

    slot = {};
}
void writePerfL10GpuBoundary(
    VkCommandBuffer commandBuffer,
    PerfL10GpuBoundary boundary
)
{
    if(!perfTimestampsEnabled())
    {
        (void)commandBuffer;
        (void)boundary;
        return;
    }

    if(
        g_perfL10GpuActiveSlot < 0 ||
        g_perfL10GpuTimestampPool == VK_NULL_HANDLE
    )
    {
        return;
    }

    vkCmdWriteTimestamp(
        commandBuffer,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        g_perfL10GpuTimestampPool,
        static_cast<uint32_t>(g_perfL10GpuActiveSlot) *
                PERF_L10_GPU_BOUNDARY_COUNT +
            static_cast<uint32_t>(boundary)
    );

    g_perfL10BoundaryMask |= (1u << static_cast<uint32_t>(boundary));
}


// Records the render resolution for this frame's summary line.  Only known once
// the DLSS sizing block has run, so it cannot be passed to the frame opener.
void setPerfL10GpuRenderSize(
    uint32_t renderWidth,
    uint32_t renderHeight
)
{
    if(g_perfL10GpuActiveSlot < 0)
    {
        return;
    }

    PerfL10GpuTimestampSlot& slot =
        g_perfL10GpuTimestampSlots[g_perfL10GpuActiveSlot];

    slot.renderWidth = renderWidth;
    slot.renderHeight = renderHeight;
}


// Opens a frame.  The slot this frame will use is recycled here: the previous
// occupant is read first, then the whole range is reset for the new frame.
// When the slot cannot be recycled the frame simply records no timestamps.
void beginPerfL10GpuFrame(
    VkCommandBuffer commandBuffer
)
{
    g_perfL10GpuActiveSlot = -1;
    g_perfL10BoundaryMask = 0u;

    if(!perfTimestampsEnabled())
    {
        (void)commandBuffer;
        return;
    }

    if(!ensurePerfL10GpuTimestampPool())
    {
        return;
    }

    const uint32_t slotIndex =
        static_cast<uint32_t>(
            g_perfL10FrameSerial %
            PERF_L10_GPU_TIMESTAMP_RING_SIZE
        );

    PerfL10GpuTimestampSlot& slot =
        g_perfL10GpuTimestampSlots[slotIndex];

    if(slot.used)
    {
        pollPerfL10GpuTimestampSlot(slotIndex);
    }

    vkCmdResetQueryPool(
        commandBuffer,
        g_perfL10GpuTimestampPool,
        slotIndex * PERF_L10_GPU_BOUNDARY_COUNT,
        PERF_L10_GPU_BOUNDARY_COUNT
    );

    slot.used = true;
    slot.frameSerial = g_perfL10FrameSerial;
    slot.stage = perfL10AblationStage();
    slot.serToggle = g_perfL10SerToggle;
    slot.serPaths = g_perfL10SerPathsPhase;
    slot.serMode = g_perfL10SerMode;

    g_perfL10GpuActiveSlot = static_cast<int32_t>(slotIndex);

    writePerfL10GpuBoundary(commandBuffer, PERF_L10_GPU_B_FRAME);
}


// Closes a frame.  Called from the single exit that finishes the whole frame;
// the other exits are early bail-outs whose slots are discarded on reuse.
void endPerfL10GpuFrame(
    VkCommandBuffer commandBuffer
)
{
    if(g_perfL10GpuActiveSlot < 0)
    {
        return;
    }

    {
        int32_t highestWritten = -1;

        for(uint32_t index = 0u; index < PERF_L10_GPU_BOUNDARY_COUNT; ++index)
        {
            if((g_perfL10BoundaryMask & (1u << index)) != 0u)
            {
                highestWritten = static_cast<int32_t>(index);
            }
        }

        for(uint32_t index = 0u; index < PERF_L10_GPU_BOUNDARY_COUNT; ++index)
        {
            if(index == static_cast<uint32_t>(PERF_L10_GPU_B_TAIL))
            {
                continue;
            }

            if((g_perfL10BoundaryMask & (1u << index)) == 0u &&
               static_cast<int32_t>(index) > highestWritten)
            {
                writePerfL10GpuBoundary(
                    commandBuffer,
                    static_cast<PerfL10GpuBoundary>(index)
                );
            }
        }
    }

    writePerfL10GpuBoundary(commandBuffer, PERF_L10_GPU_B_TAIL);

    {
        const uint32_t wanted =
            (1u << static_cast<uint32_t>(PERF_L10_GPU_BOUNDARY_COUNT)) - 1u;

        if((g_perfL10BoundaryMask & wanted) != wanted)
        {
            static uint64_t s_lastReportFrame = 0u;

            if(g_perfL10FrameSerial > s_lastReportFrame + 300u)
            {
                s_lastReportFrame = g_perfL10FrameSerial;

                std::string missing;

                for(uint32_t index = 0u; index < PERF_L10_GPU_BOUNDARY_COUNT; ++index)
                {
                    if((g_perfL10BoundaryMask & (1u << index)) == 0u)
                    {
                        missing += (missing.empty() ? "" : ",");
                        missing += std::to_string(index);
                    }
                }

                RTLogFlushed(
                    "[PERF L10 GPU] boundaries incomplete, missing index=" + missing +
                    " (a skipped boundary kills every timestamp slot)"
                );
            }
        }
    }

    g_perfL10GpuActiveSlot = -1;
}


// Advances the feature-isolation stage once every stage's frame budget.  Called
// before the frame is recorded so the UBO write and the pass window both see
// the same stage for this frame.
void updatePerfL10AblationStage()
{
    const uint32_t request = perfL10AblationRequestedStage();

    if(request == PERF_L10_ABLATION_OFF)
    {
        g_perfL10AblationStage = 0u;
        return;
    }

    static bool announced = false;
    if(!announced)
    {
        announced = true;
        if(request == PERF_L10_ABLATION_SWEEP)
        {
            RTLogFlushed(
                "[PERF L10 ABLATE] enabled sweep framesPerStage=120 stages=14 (ORION_PERF_ABLATION)"
            );
        }
        else if(perfL10AblationAltMode(request))
        {
            std::string stages;
            for(std::size_t i = 0u; i < g_perfL10AblationAltStages.size(); ++i)
            {
                stages += (i == 0u ? "" : ",");
                stages += PERF_L10_ABLATION_STAGE_NAMES[g_perfL10AblationAltStages[i]];
            }
            RTLogFlushed(
                "[PERF L10 ABLATE] enabled interleaved A/B windowFrames=" +
                std::to_string(PERF_L10_GPU_WINDOW_FRAMES) +
                " stages=" + (stages.empty() ? std::string("<empty>") : stages)
            );
        }
        else
        {
            RTLogFlushed(
                "[PERF L10 ABLATE] enabled pinned stage=" + std::to_string(request)
            );
        }
    }

    if(perfL10AblationAltMode(request))
    {
        if(g_perfL10AblationAltStages.empty())
        {
            g_perfL10AblationStage = 0u;
            return;
        }
        const uint32_t windowIndex = g_perfL10GpuWindowsLogged;

        {
            static uint32_t s_seenWindows = 0xffffffffu;
            static uint64_t s_seenFrame = 0u;

            if(s_seenWindows == 0xffffffffu)
            {
                s_seenWindows = g_perfL10GpuWindowsLogged;
                s_seenFrame = g_perfL10FrameSerial;
            }
            else if(s_seenWindows != g_perfL10GpuWindowsLogged)
            {
                s_seenWindows = g_perfL10GpuWindowsLogged;
                s_seenFrame = g_perfL10FrameSerial;
            }
            else if(g_perfL10FrameSerial > s_seenFrame + 600u)
            {
                s_seenFrame = g_perfL10FrameSerial;

                RTLogFlushed(
                    "[PERF L10 ABLATE] stalled: no GPU window in 600 frames "
                    "(check ORION_PERF_TIMESTAMPS=1; the A/B arm cannot advance)"
                );
            }
        }
        if(windowIndex % 2u == 0u)
        {
            g_perfL10AblationStage = 0u;
            return;
        }
        g_perfL10AblationStage = g_perfL10AblationAltStages[
            (windowIndex / 2u) % g_perfL10AblationAltStages.size()];
        return;
    }

    if(request != PERF_L10_ABLATION_SWEEP)
    {
        g_perfL10AblationStage = request;
        return;
    }

    g_perfL10AblationStage = static_cast<uint32_t>(
        (g_perfL10FrameSerial / PERF_L10_ABLATION_FRAMES_PER_STAGE) %
        PERF_L10_ABLATION_STAGE_COUNT
    );
}
void perfL10ReportStall(const char* tag, double milliseconds)
{
    if constexpr(!PERF_L10_ENABLE_STALL_LOG)
    {
        (void)tag;
        (void)milliseconds;
        return;
    }

    if(milliseconds < PERF_L10_STALL_LOG_THRESHOLD_MS)
    {
        return;
    }

    std::stringstream log;
    log
        << "[PERF L10 STALL]"
        << " frame=" << g_perfL10FrameSerial
        << " tag=" << (tag ? tag : "<null>")
        << " ms=" << std::fixed << std::setprecision(2)
        << milliseconds;

    RTLogFlushed(log.str());
}
void logPerfL10CpuPhaseWindow()
{
    const double sampleCount =
        static_cast<double>(g_perfL10CpuPhaseWindow.samples);

    if(sampleCount <= 0.0)
    {
        return;
    }

    std::stringstream averages;
    std::stringstream maxima;

    averages
        << "[PERF L10 CPUPHASE]"
        << " frame=" << g_perfL10FrameSerial
        << " win=" << g_perfL10CpuPhaseWindow.samples
        << " avgMs total=" << std::fixed << std::setprecision(2)
        << (g_perfL10CpuPhaseWindow.sumFrameMs / sampleCount);

    maxima
        << "[PERF L10 CPUPHASE]"
        << " frame=" << g_perfL10FrameSerial
        << " win=" << g_perfL10CpuPhaseWindow.samples
        << " maxMs total=" << std::fixed << std::setprecision(2)
        << g_perfL10CpuPhaseWindow.maxFrameMs;

    for(uint32_t phase = 0u; phase < PERF_L10_CPU_PHASE_COUNT; ++phase)
    {
        averages
            << " " << PERF_L10_CPU_PHASE_NAMES[phase] << "="
            << std::setprecision(2)
            << (g_perfL10CpuPhaseWindow.sumMs[phase] / sampleCount);

        maxima
            << " " << PERF_L10_CPU_PHASE_NAMES[phase] << "="
            << std::setprecision(2)
            << g_perfL10CpuPhaseWindow.maxMs[phase];
    }

    RTLog(averages.str());
    RTLog(maxima.str());

    g_perfL10CpuPhaseWindow = PerfL10CpuPhaseWindow{};
}


// Opens phase 0 and discards anything a previous frame left unfinished.
void perfL10CpuPhaseBeginFrame()
{
    if constexpr(!PERF_L10_ENABLE_CPU_PHASES)
    {
        return;
    }

    g_perfL10CpuPhaseWindow.samples += 1u;

    g_perfL10CpuPhaseCursor = PERF_L10_CPU_P_PUBLISH;
    g_perfL10CpuPhaseStart = std::chrono::steady_clock::now();
}


// Closes the phase the cursor is on and moves to `nextPhase`.
void perfL10CpuPhaseAdvance(uint32_t nextPhase)
{
    if constexpr(!PERF_L10_ENABLE_CPU_PHASES)
    {
        (void)nextPhase;
        return;
    }

    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();

    if(g_perfL10CpuPhaseCursor < PERF_L10_CPU_PHASE_COUNT)
    {
        const double phaseMs =
            std::chrono::duration<double, std::milli>(
                now - g_perfL10CpuPhaseStart
            ).count();

        g_perfL10CpuPhaseWindow.sumMs[g_perfL10CpuPhaseCursor] += phaseMs;

        if(phaseMs > g_perfL10CpuPhaseWindow.maxMs[g_perfL10CpuPhaseCursor])
        {
            g_perfL10CpuPhaseWindow.maxMs[g_perfL10CpuPhaseCursor] = phaseMs;
        }

        g_perfL10CpuPhaseWindow.sumFrameMs += phaseMs;

        if(phaseMs > g_perfL10CpuPhaseWindow.maxFrameMs)
        {
            g_perfL10CpuPhaseWindow.maxFrameMs = phaseMs;
        }
    }

    g_perfL10CpuPhaseCursor = nextPhase;
    g_perfL10CpuPhaseStart = now;
}


// Closes the final phase and emits the window summary when it is full.
void perfL10CpuPhaseEndFrame()
{
    if constexpr(!PERF_L10_ENABLE_CPU_PHASES)
    {
        return;
    }

    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();

    if(g_perfL10CpuPhaseCursor < PERF_L10_CPU_PHASE_COUNT)
    {
        const double phaseMs =
            std::chrono::duration<double, std::milli>(
                now - g_perfL10CpuPhaseStart
            ).count();

        g_perfL10CpuPhaseWindow.sumMs[g_perfL10CpuPhaseCursor] += phaseMs;

        if(phaseMs > g_perfL10CpuPhaseWindow.maxMs[g_perfL10CpuPhaseCursor])
        {
            g_perfL10CpuPhaseWindow.maxMs[g_perfL10CpuPhaseCursor] = phaseMs;
        }

        g_perfL10CpuPhaseWindow.sumFrameMs += phaseMs;

        if(phaseMs > g_perfL10CpuPhaseWindow.maxFrameMs)
        {
            g_perfL10CpuPhaseWindow.maxFrameMs = phaseMs;
        }
    }

    g_perfL10CpuPhaseCursor = PERF_L10_CPU_PHASE_COUNT;
    g_perfL10CpuPhaseStart = now;

    if(
        g_perfL10CpuPhaseWindow.samples >= PERF_L10_GPU_WINDOW_FRAMES
    )
    {
        logPerfL10CpuPhaseWindow();
    }
}


// =====================================================
// Perf-L10.3 - GI ray-count probe
// =====================================================
// The RayGen folds per-pixel counts of the GI/AO/LOD work it actually did into
// binding 46, so the host can report how many rays the path traced instead of
// inferring the count from pass time.  Readback reuses the timestamp ring rule:
// a slot is copied on the GPU at the end of the frame and only folded into the
// window RING_SIZE frames later, once the CPU has recorded that far ahead, so
// no query read or wait is ever issued for it.  Diagnostic-only.
// Dormant: the matching counter writes live in shaders/rt_test.rgen, which was
// replaced by a concurrent workstream that never had them, so the counters can
// no longer be produced.  Kept switched off rather than deleted so the host-side
// plumbing (buffer, staging ring, descriptor, window log) stays reviewable if
// the ray counts are ever needed again.
// 2026-10-24：从编译期常量改成运行期开关。
// 着色器侧只在 stage 15（原空档 spare15）写计数，所以采集必须：
//   ORION_RAY_STATS=1  ORION_PERF_ABLATION=pin:15
// 不设则本函数返回 false，begin/end 直接返回，与改动前逐位相同。
bool perfL10RayStatsEnabled()
{
    static const bool s_enabled = []() -> bool
    {
        const char* raw = std::getenv("ORION_RAY_STATS");

        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    return s_enabled;
}


VkDeviceSize perfL10RayStatsBytes()
{
    return static_cast<VkDeviceSize>(PERF_L10_RAY_STAT_COUNT) *
           sizeof(uint32_t);
}


void destroyPerfL10RayStats()
{
    if(
        g_perfL10RayStatsStagingMapped != nullptr &&
        g_perfL10RayStatsStagingMemory != VK_NULL_HANDLE &&
        g_device != VK_NULL_HANDLE
    )
    {
        vkUnmapMemory(
            g_device,
            g_perfL10RayStatsStagingMemory
        );
    }

    g_perfL10RayStatsStagingMapped = nullptr;

    if(g_device != VK_NULL_HANDLE)
    {
        if(g_perfL10RayStatsStagingBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(
                g_device,
                g_perfL10RayStatsStagingBuffer,
                nullptr
            );
        }
        if(g_perfL10RayStatsBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(
                g_device,
                g_perfL10RayStatsBuffer,
                nullptr
            );
        }
    }

    g_perfL10RayStatsStagingBuffer = VK_NULL_HANDLE;
    g_perfL10RayStatsBuffer = VK_NULL_HANDLE;

    if(
        g_perfL10RayStatsStagingMemory != VK_NULL_HANDLE &&
        g_device != VK_NULL_HANDLE
    )
    {
        vkFreeMemory(
            g_device,
            g_perfL10RayStatsStagingMemory,
            nullptr
        );
    }
    if(
        g_perfL10RayStatsMemory != VK_NULL_HANDLE &&
        g_device != VK_NULL_HANDLE
    )
    {
        vkFreeMemory(
            g_device,
            g_perfL10RayStatsMemory,
            nullptr
        );
    }

    g_perfL10RayStatsStagingMemory = VK_NULL_HANDLE;
    g_perfL10RayStatsMemory = VK_NULL_HANDLE;

    for(uint32_t i = 0u; i < PERF_L10_RAY_STAT_COUNT; ++i)
    {
        g_perfL10RayStatsSums[i] = 0u;
    }

    g_perfL10RayStatsWindowFrames = 0u;
    g_perfL10RayStatsActiveSlot = 0u;

    for(PerfL10RayStatsSlot& slot : g_perfL10RayStatsSlots)
    {
        slot = {};
    }
}


bool ensurePerfL10RayStats()
{
    if(g_perfL10RayStatsBuffer != VK_NULL_HANDLE)
    {
        return true;
    }

    if(
        g_device == VK_NULL_HANDLE ||
        g_physicalDevice == VK_NULL_HANDLE ||
        g_rayTracingDescriptorSet == VK_NULL_HANDLE
    )
    {
        return false;
    }

    const VkDeviceSize statsBytes = perfL10RayStatsBytes();
    const VkDeviceSize stagingBytes =
        statsBytes * PERF_L10_RAY_STAT_RING_SIZE;

    createBuffer(
        g_device,
        g_physicalDevice,
        statsBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_perfL10RayStatsBuffer,
        g_perfL10RayStatsMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        stagingBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_perfL10RayStatsStagingBuffer,
        g_perfL10RayStatsStagingMemory
    );

    const VkResult mapResult = vkMapMemory(
        g_device,
        g_perfL10RayStatsStagingMemory,
        0,
        stagingBytes,
        0,
        reinterpret_cast<void**>(&g_perfL10RayStatsStagingMapped)
    );

    if(mapResult != VK_SUCCESS)
    {
        g_perfL10RayStatsStagingMapped = nullptr;

        RTLog(
            "[PERF L10 RAY] stagingMap=" +
            std::to_string(static_cast<int>(mapResult))
        );

        destroyPerfL10RayStats();

        return false;
    }

    VkDescriptorBufferInfo statsInfo{};
    statsInfo.buffer = g_perfL10RayStatsBuffer;
    statsInfo.offset = 0;
    statsInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet statsWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
    };
    statsWrite.dstSet = g_rayTracingDescriptorSet;
    statsWrite.dstBinding = 46;
    statsWrite.descriptorCount = 1;
    statsWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    statsWrite.pBufferInfo = &statsInfo;

    vkUpdateDescriptorSets(
        g_device,
        1,
        &statsWrite,
        0,
        nullptr
    );

    RTLog(
        "[PERF L10 RAY] statsBuffer=ready bytes=" +
        std::to_string(static_cast<unsigned long long>(statsBytes)) +
        " ring=" +
        std::to_string(PERF_L10_RAY_STAT_RING_SIZE)
    );

    return true;
}


// Window summary, one line per window.  Everything except the percentages is
// expressed per instrumented pixel, so the numbers stay valid when the launch
// resolution changes between windows.
void logPerfL10RayStatsWindow()
{
    const double instrumentedPixels =
        static_cast<double>(
            g_perfL10RayStatsSums[PERF_L10_RAY_STAT_INSTRUMENTED_PIXELS]
        );

    if(instrumentedPixels <= 0.0)
    {
        return;
    }

    const double giRays =
        static_cast<double>(g_perfL10RayStatsSums[PERF_L10_RAY_STAT_GI_RAYS]);

    const double zeroThroughputRays =
        static_cast<double>(
            g_perfL10RayStatsSums[
                PERF_L10_RAY_STAT_GI_ZERO_THROUGHPUT_RAYS
            ]
        );

    std::stringstream log;
    log
        << "[PERF L10 RAY]"
        << " frame=" << g_perfL10FrameSerial
        << " win=" << g_perfL10RayStatsWindowFrames
        << " instPix="
        << g_perfL10RayStatsSums[PERF_L10_RAY_STAT_INSTRUMENTED_PIXELS]
        << " giPix=" << g_perfL10RayStatsSums[PERF_L10_RAY_STAT_GI_PIXELS]
        << std::fixed << std::setprecision(2)
        << " giRayPerPix="
        << (giRays / instrumentedPixels)
        << " b0PerPix="
        << (
            static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_GI_BOUNCE0_RAYS]
            ) / instrumentedPixels
        )
        << " b1PerPix="
        << (
            static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_GI_BOUNCE1_RAYS]
            ) / instrumentedPixels
        )
        << " missPerPix="
        << (
            static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_GI_MISS_RAYS]
            ) / instrumentedPixels
        )
        << " zeroTpPerPix="
        << (zeroThroughputRays / instrumentedPixels)
        << " waterPerPix="
        << (
            static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_GI_WATER_RAYS]
            ) / instrumentedPixels
        )
        << std::setprecision(1)
        << " aoPixPct="
        << (
            100.0 * static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_AO_PIXELS]
            ) / instrumentedPixels
        )
        << std::setprecision(2)
        << " aoRayPerPix="
        << (
            static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_AO_RAYS]
            ) / instrumentedPixels
        )
        << std::setprecision(1)
        << " farPixPct="
        << (
            100.0 * static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_FAR_LOD_PIXELS]
            ) / instrumentedPixels
        )
        << " vfarPixPct="
        << (
            100.0 * static_cast<double>(
                g_perfL10RayStatsSums[
                    PERF_L10_RAY_STAT_VERY_FAR_LOD_PIXELS
                ]
            ) / instrumentedPixels
        )
        << " localPerPix="
        << (
            static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_LOCAL_LIGHT_RAYS]
            ) / instrumentedPixels
        )
        << " sunRayPerPix="
        << (
            static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_SUN_SHADOW_RAYS]
            ) / instrumentedPixels
        )
        << " glossyPerPix="
        << (
            static_cast<double>(
                g_perfL10RayStatsSums[PERF_L10_RAY_STAT_GLOSSY_RAYS]
            ) / instrumentedPixels
        )
        << " zeroTpPctOfGiRays="
        << (
            100.0 * zeroThroughputRays /
            (giRays > 1.0 ? giRays : 1.0)
        );

    RTLog(log.str());
}


// Opens the probe for this frame.  The previous occupant of the ring slot is
// folded into the window first, then the device buffer is zeroed so the shader
// starts this frame from a known state.
void beginPerfL10RayStats(
    VkCommandBuffer commandBuffer
)
{
    if(!perfL10RayStatsEnabled())
    {
        (void)commandBuffer;
        return;
    }

    if(!ensurePerfL10RayStats())
    {
        return;
    }

    const uint32_t slotIndex =
        static_cast<uint32_t>(
            g_perfL10FrameSerial %
            PERF_L10_RAY_STAT_RING_SIZE
        );

    g_perfL10RayStatsActiveSlot = slotIndex;

    PerfL10RayStatsSlot& slot = g_perfL10RayStatsSlots[slotIndex];

    if(slot.used && g_perfL10RayStatsStagingMapped != nullptr)
    {
        const uint32_t* sample =
            g_perfL10RayStatsStagingMapped +
            static_cast<size_t>(slotIndex) * PERF_L10_RAY_STAT_COUNT;

        if(sample[PERF_L10_RAY_STAT_INSTRUMENTED_PIXELS] > 0u)
        {
            for(uint32_t i = 0u; i < PERF_L10_RAY_STAT_COUNT; ++i)
            {
                g_perfL10RayStatsSums[i] += sample[i];
            }

            g_perfL10RayStatsWindowFrames += 1u;

            if(
                g_perfL10RayStatsWindowFrames >=
                PERF_L10_RAY_STAT_WINDOW_FRAMES
            )
            {
                logPerfL10RayStatsWindow();

                for(uint32_t i = 0u; i < PERF_L10_RAY_STAT_COUNT; ++i)
                {
                    g_perfL10RayStatsSums[i] = 0u;
                }

                g_perfL10RayStatsWindowFrames = 0u;
            }
        }

        slot = {};
    }

    const VkDeviceSize statsBytes = perfL10RayStatsBytes();

    vkCmdFillBuffer(
        commandBuffer,
        g_perfL10RayStatsBuffer,
        0,
        statsBytes,
        0u
    );

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = g_perfL10RayStatsBuffer;
    barrier.offset = 0;
    barrier.size = statsBytes;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0,
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr
    );
}


// Closes the probe for this frame: publishes the counters the RayGen wrote and
// copies them into this frame's ring slot with no host wait.
void endPerfL10RayStats(
    VkCommandBuffer commandBuffer
)
{
    if(!perfL10RayStatsEnabled())
    {
        (void)commandBuffer;
        return;
    }

    if(
        g_perfL10RayStatsBuffer == VK_NULL_HANDLE ||
        g_perfL10RayStatsStagingBuffer == VK_NULL_HANDLE
    )
    {
        return;
    }

    const VkDeviceSize statsBytes = perfL10RayStatsBytes();

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = g_perfL10RayStatsBuffer;
    barrier.offset = 0;
    barrier.size = statsBytes;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        1,
        &barrier,
        0,
        nullptr
    );

    VkBufferCopy copy{};
    copy.srcOffset = 0;
    copy.dstOffset =
        statsBytes * static_cast<VkDeviceSize>(g_perfL10RayStatsActiveSlot);
    copy.size = statsBytes;

    vkCmdCopyBuffer(
        commandBuffer,
        g_perfL10RayStatsBuffer,
        g_perfL10RayStatsStagingBuffer,
        1,
        &copy
    );

    PerfL10RayStatsSlot& slot =
        g_perfL10RayStatsSlots[g_perfL10RayStatsActiveSlot];

    slot.used = true;
    slot.frameSerial = g_perfL10FrameSerial;
}

// 2026-10-10 入口闸门的总闸。ORION_REBUILD_FENCE_GATE=0 退回旧行为（无限等上一批的
// fence），用于 A/B 对照与快速回退 —— 这样这条改动不需要重新编译就能关掉。
bool sectionRebuildFenceGateEnabled()
{
    static const bool enabled = []()
    {
        const char* raw = std::getenv("ORION_REBUILD_FENCE_GATE");
        return !(raw != nullptr && raw[0] == '0');
    }();

    return enabled;
}
