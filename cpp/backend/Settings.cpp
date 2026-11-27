// Orion backend — Settings 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

float waterSimUpdateSeconds()
{
    static const float cached = []() -> float
    {
        const char* raw = std::getenv("ORION_WATER_SIM_HZ");

        // 2026-11-21 顾问轮1（修复①）：默认 15 → 30 Hz。旧 15 Hz 下交互法线
        // 以 66.7ms 步进，用户实测"一卡一卡、拖拽感"；且双子步改版后每次
        // 调度的状态跳变量还放大了 ~30%。30 Hz 单子步：物理子步率不变
        //（等效 Courant 与稳定域一致），步进间隔减半到 33ms，每步跳变量减半。
        // 512² RG16F compute 实测 <0.2ms/次，翻倍无感知成本。
        int hz = 30;

        if(raw != nullptr)
        {
            const int parsed = std::atoi(raw);

            if(parsed >= 15 && parsed <= 60)
            {
                hz = parsed;
            }
        }

        return 1.0f / static_cast<float>(hz);
    }();

    return cached;
}
bool volumetricCloudV3Requested()
{
    static const bool requested = [] {
        std::ifstream marker(ResolveRTXRuntimePath("cloud_v3.flag"));
        const bool enabled = marker.good();
        if(enabled)
        {
            RTLog("[CLOUD V3] cloud_v3.flag present -> v3 cloud model");
        }
        return enabled;
    }();
    return requested;
}

#if 0
static void createWaterFoamResources();
static void recordWaterFoam(VkCommandBuffer cmd);
static void destroyWaterFoamResources();
static void updateWaterFoamRayDescriptor();
#endif
bool hdrOutputActive()
{
    return
        g_hdrRequested.load() &&
        g_hdrSurfaceSupported.load() &&
        // 2026-11-23：再加一道系统侧门控 —— Windows「使用 HDR」没开时驱动仍会
        // 报告 ST2084 支持，若照样输出 HDR10/PQ，系统只能按 SDR 桌面合成 ⇒
        // 画面发灰。查询失败时 windowsHdrAllowsHdrOutput() 返回 true（保持旧行为）。
        windowsHdrAllowsHdrOutput();
}
bool glassAlphaBlendEnvFlag(const char* name)
{
    const char* raw = std::getenv(name);
    return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
}

// 邻居搜索 / 体积辐照度兜底的开关。
//
// 2026-10-26（第四轮，严格搬）：出厂值改成与原文一致 ——
//   rtx_composite.h  stochasticAlphaBlendUseNeighborSearch = true
//                    stochasticAlphaBlendUseRadianceVolume = true
// 这两个环境变量现在只做 **A/B 关闭**（默认开），名字也改成 OFF 语义。
//   ORION_GLASS_ALPHA_BLEND_NEIGHBOR=0          关邻居搜索（退回本像素兜底）
//   ORION_GLASS_ALPHA_BLEND_RADIANCE_VOLUME=0   关体积辐照度兜底
bool glassAlphaBlendEnvDisabled(const char* name)
{
    const char* raw = std::getenv(name);
    return raw != nullptr && raw[0] == '0' && raw[1] == '\0';
}
bool glassAlphaBlendNeighborSearchEnabled()
{
    static const bool cached =
        !glassAlphaBlendEnvDisabled("ORION_GLASS_ALPHA_BLEND_NEIGHBOR");
    return cached;
}
bool glassAlphaBlendRadianceVolumeEnabled()
{
    static const bool cached =
        !glassAlphaBlendEnvDisabled("ORION_GLASS_ALPHA_BLEND_RADIANCE_VOLUME");
    return cached;
}

// =====================================================
// ORION_GI_HALF：半分辨率 GI 的**唯一**环境开关（默认关 = 逐位旧画面）。
//
// 为什么是环境变量而不是设置项：这是评估阶段的一条对照臂，判据是
// "[PERF L10 GPU] 的 rt 段下降 ≥ 1.5ms"（HANDOFF §8），不是玩家功能。
// 关掉时：宿主不建图、不建管线、不 dispatch，且往 UBO 写 0 ⇒ 主 raygen 走
// 原来的全分辨率 GI 回路（那段代码仍在，只是被运行时分支跳过）。
// =====================================================
// ORION_GI_HALF_ROTATE：半图相位是否按帧轮转（默认关）。
// 开 = 2×2 块内四个子像素四帧各算一次（HANDOFF §3 的原方案），但每个 texel 代表的
// 世界位置每帧平移一个像素 ⇒ 时间抖动、RR 累积失效（2026-10-25 实测满屏闪点）。
// 关 = texel (i,j) 固定代表像素 (2i,2j)，稳定但永久 2×2 模糊。
bool giHalfRotateEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_GI_HALF_ROTATE");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    return cached;
}

// ORION_GI_NEAR_RATE：近场 GI 的降速级（0 = 全速 / 1 = 半速 / 2 = 四分之一）。
// 距离 LOD（rtLodDiffuseGiEnabled）只管 64 格以外；室内近场原本全速，这一项才是
// 给近场用的。机制与光泽粗糙度分级同构：独立 bit 片 + 权重补偿 = 无偏的时间
// 估计量，跳过的是 GI 信号而不是像素 ⇒ 不出锯齿。
int giNearRateLevel()
{
    const int override = g_giNearRateOverride.load(std::memory_order_relaxed);
    if(override >= 0)
    {
        return override;
    }

    static const int cached = []()
    {
        const char* raw = std::getenv("ORION_GI_NEAR_RATE");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 2;
        }

        const int parsed = std::atoi(raw);
        if(parsed >= 2)
        {
            return 2;
        }
        return parsed >= 1 ? 1 : 0;
    }();

    return cached;
}

// ORION_GI_TEMPORAL：GI 第一反弹时域重投影（A1）。2026-11-05 默认改**关**：
// K1/K2 拆分实测下它是噪点源（开=静止不收敛+运动扭曲，关=干净，A/B 定案），
// 且用户当前不需要任何时域复用。1 = 开（rgen 走 EMA 历史路径）。
// 关 = rgen 走逐位旧路径：交错补偿 + 跳过帧 indirectLight=0。
// 2026-11-10：GI 可见性门控 —— **默认关闭**。
//
// 原始动机：太阳全遮蔽且附近无局部光的区域，一次反弹的期望为 0，射线是纯浪费。
// 但实现出来的判据是错的："太阳照不到"不等于"没有间接光"——背光面、屋檐下、
// 树荫里仍然有**天空光**的间接照明，而那正是 GI 要算的东西；局部光检查也覆盖
// 不到天空光。用户实测表现为块状、边界生硬的变暗。
//
// 且收益上限很小：noGi 消融测得 GI 总共只占 trace 段的 1.1ms。
// 正确判据应当是"半球天空可见度"，需要额外射线或专用通道，另立项。
//
// 保留实现仅供实验：ORION_GI_VISIBILITY_GATE=1 才启用。
// 2026-11-10 A/B：次级射线最小材质（跳过 _n，保留 albedo/_s ⇒ 金属色与粗糙度不变）
bool matSecondaryMinimalEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_MAT_SECONDARY_MINIMAL");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}

// 2026-11-10 A/B：远场材质 LOD 起始距离（0 = 关闭；shader 内 32→+16 格淡出）
float matFarLodDistance()
{
    static const float cached = []()
    {
        const char* raw = std::getenv("ORION_MAT_FAR_LOD");
        if(raw == nullptr || raw[0] == '\0')
        {
            // 2026-11-12：默认改为**关闭**（0）。原默认 32 格会带来三个确定可见的
            // 副作用：①反射变钝（_s 被跳过后粗糙度/金属码退化为常量）
            // ②32→48 格出现可辨认的分层 ③_s.a 是 LabPBR 自发光掩码，跳过后
            // 远处光源方块不再发光。而它的收益从未单独实测（估算 0.5~1 ms），
            // 交换不划算。需要时：ORION_MAT_FAR_LOD=96（单位是**格**，96=6 区块）。
            return 0.0f;
        }
        const float v = static_cast<float>(std::atof(raw));
        return v < 0.0f ? 0.0f : v;
    }();

    return cached;
}

// 2026-11-15：GI 弹射分档边界的**环境变量覆盖**（单位：区块；未设 / 负数 = 用
// g_perfBounceBandChunks 的 UI 值，出厂 4 区块）。
// 用途：同场景 A/B「>4 区块只 1 弹」这条改动。设 32 = 覆盖到常见视距之外 ⇒
// 等效关闭分档，且**不重编 SPV、不触发冷编**（只改 UBO 的 perfBandParams.y）。
float giBounceBandChunksOverride()
{
    static const float cached = []()
    {
        const char* raw = std::getenv("ORION_GI_BOUNCE_BAND");
        if(raw == nullptr || raw[0] == '\0')
        {
            return -1.0f;
        }
        return static_cast<float>(std::atof(raw));
    }();

    return cached;
}

// 2026-11-15 远场贴图降档起点（单位**格**；0 = 关）。
// shader 侧 material_resolve.glsl：起点起每 32 格抬 1 级 mip，封顶 +3，
// albedo/normal/specular 通道全部保留采样（不复现旧 ORION_MAT_FAR_LOD 硬跳档
// 的反射变钝/分层/远处灯不亮副作用）。默认 128 格 = 8 区块（用户指定）。
float matTexFarLodDistance()
{
    static const float cached = []()
    {
        const char* raw = std::getenv("ORION_MAT_TEX_FAR_LOD");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 128.0f;
        }
        const float v = static_cast<float>(std::atof(raw));
        return v < 0.0f ? 0.0f : v;
    }();

    return cached;
}

bool giVisibilityGateEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_GI_VISIBILITY_GATE");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    return cached;
}

// 2026-11-10：glossy 反射链的俄罗斯轮盘（无偏，只换方差）。
// 依据：trace 段 16.12ms 中反射链占 6.5ms（noGlossy 消融），而该路径此前
// 确定性跑满、没有 RR。
// 2026-11-16：默认值对齐。shader 头注释一直写"默认开"，这里却要求 =1，
// 于是这份半成品从未生效（镜面链仍然确定性跑满 cap）。改为默认开、
// =0 关闭，与 ORION_WATER_ADAPTIVE 等同一口径（env 缺省 = 开）。
// 回退：ORION_GLOSSY_RR=0。
bool glossyRrEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_GLOSSY_RR");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}

// 2026-11-10：glossy 镜面弹射层数按粗糙度/首次命中距离收敛。
// 回退：ORION_GLOSSY_STEP_LOD=0。
bool glossyStepLodEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_GLOSSY_STEP_LOD");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}

bool giTemporalEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_GI_TEMPORAL");
        if(raw == nullptr || raw[0] == '\0')
        {
            return false;
        }
        return raw[0] != '0';
    }();

    return cached;
}

// ORION_GLOSSY_ROUGH_LOD：光泽反射按粗糙度降速（半速/四分之一），见
// rt_test.rgen 的 glossySampleLevel 处注释。默认关 = 逐位旧行为。
bool glossyRoughLodEnabled()
{
    const int override = g_glossyRoughLodOverride.load(std::memory_order_relaxed);
    if(override >= 0)
    {
        return override != 0;
    }

    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_GLOSSY_ROUGH_LOD");
        if(raw == nullptr || raw[0] == '\0')
        {
            return true;
        }
        return raw[0] != '0';
    }();

    return cached;
}
bool giNoPomEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_GI_NO_POM");
        if(raw == nullptr || raw[0] == '\0')
        {
            return true;
        }
        return raw[0] != '0';
    }();

    return cached;
}

// 2026-11-09：次级射线（反射 / 水体 / 玻璃后）跳过 POM。
// 出厂**开**（env 未设即 true）：POM 是厘米级位移，只对主射线的近处掠射有意义；
// 反射里再跑一遍意味着每条 glossy 路径最多 18 步 x 288 次 texelFetch。
// 回退：ORION_POM_SECONDARY=0 ⇒ 次级命中恢复视差（逐位回到改动前）。
bool pomSecondarySkipEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_POM_SECONDARY");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}

// 直接光采样模式：0 = 传统单帧 RIS（旧路径）；1 = RTXDI 式时域 reservoir
// （重投影 + 灯身份验证 + WRS 合并 + 1 条可见性射线；见 rt_test.rgen 的
// LOCAL_RIS_PRIMARY_TEMPORAL 运行时分支）。
int directLightMode()
{
    const int override =
        g_directLightModeOverride.load(std::memory_order_relaxed);
    if(override >= 0)
    {
        return override > 0 ? 1 : 0;
    }

    static const int cached = []()
    {
        const char* raw = std::getenv("ORION_DIRECT_LIGHT_MODE");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 0;
        }
        return std::atoi(raw) >= 1 ? 1 : 0;
    }();

    return cached;
}

// 稀疏渲染活跃采样率（%）：0 = 关（默认）；1..100 = 界面/env 的请求值。
//
// 2026-10-27 更正：这条注释此前写「官方 rtx-remix-minecraft 的量产档为
// quality 0.3 ≈ 30~35%」，把另一套模组当成参考标注，属于错误归因。
// RTX Remix 的实际参数在 rtx_sparse_rendering.h 的 Options 里：
//   samplingRate = 0.357、enableAdaptiveSamplingRate = true、
//   samplingRateResolutionExponent = 0.69、quality = 0.5、maxSamplingRate = 0.75。
// 本函数只取「请求的百分比」；真正的有效采样率由 sparseSamplingRateEffective()
// 按 Remix 的 calculateSamplingRate 计算（见该函数注释）。
int sparseRatePercent()
{
    const int override =
        g_sparseRatePercentOverride.load(std::memory_order_relaxed);
    if(override >= 0)
    {
        return std::min(100, std::max(0, override));
    }

    static const int cached = []()
    {
        const char* raw = std::getenv("ORION_SPARSE_RATE");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 0;
        }
        return std::min(100, std::max(0, std::atoi(raw)));
    }();

    return cached;
}

// RTXDI×稀疏联动：非活跃像素是否仍做廉价 reservoir 验证/老化（默认 1）。
//
// ⚠ 2026-10-27 自创标注：此前这里写「对齐官方
// rtx.sparseRendering.enableRtxdiReuseForInactivePixels=true」，该选项名在
// _ref/dxvk-remix 全仓库 0 命中。Remix 的 RTXDI initial / temporal / spatial
// reuse 三个 pass 全部走 SparseRendering::dispatchCompactedConsumer
// (rtx_rtxdi_rayquery.cpp:518 / 524 / 593)，非活跃像素根本没有线程，不存在
// 「仍合并/老化 reservoir」这一行为。本开关是本工程的取舍（防再激活闪烁），
// 不是移植；默认值 1 也不代表官方默认。
int rtxdiReuseInactiveEnabled()
{
    const int override =
        g_rtxdiReuseInactiveOverride.load(std::memory_order_relaxed);
    if(override >= 0)
    {
        return override > 0 ? 1 : 0;
    }

    static const int cached = []()
    {
        const char* raw = std::getenv("ORION_RTDI_REUSE_INACTIVE");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 1;
        }
        return raw[0] != '0' ? 1 : 0;
    }();

    return cached;
}
bool giHalfEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_GI_HALF");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    return cached;
}

// 2026-11-05 B：K1(主视线)/K2(传输) 两发拆分。
// 2026-11-06 落盘为默认开（实测 trace 7.74→6.12ms，视觉验证通过）；
// ORION_K1_K2=0 仅作紧急回退口，正常启动无需任何 env。
// 2026-11-16 主管线退役定案：K1/K2 拆分管线是唯一 RT 路径。6-stage 主单体的
// 全部创建点已 #if 0 屏蔽（boot worker / recreateMainRtPipeline / Device.cpp
// 同步版）——29GB commit 冷编墙随之一并退役。这里强制 true：ORION_K1_K2=0
// 不再通往主管线，只会得到"整局无 RT"。旧逃生口逻辑屏蔽保留
//（回滚 = 还原本函数 + 去掉三处创建点的 #if 0/#endif）。
bool k1k2SplitEnabled()
{
#if 0
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_K1_K2");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
#endif
    return true;
}

// 2026-11-07 SER 门控口径（对齐规范与官方示例）：
//   `VK_EXT_ray_tracing_invocation_reorder` 的
//   rayTracingInvocationReorderReorderingHint **只是提示**——报 MODE_NONE 时
//   reorderThread* 退化为 no-op，但 hit object 本身照常可用
//   （Khronos Vulkan-Samples `ray_tracing_invocation_reorder` 的 README
//   "向后兼容行为" 一节：无硬件支持时 reorderThreadEXT 为 no-op，hit object
//   仍正常工作）。原先的加载条件额外要求 MODE_REORDER，等于在没报重排提示的
//   设备上把 hitObject 查询类用法（AO/阴影跳过 shader dispatch）一起关掉。
//   默认改成只看特性；需要旧口径时 ORION_SER_REQUIRE_REORDER=1。
bool serRequireReorderHint()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_SER_REQUIRE_REORDER");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    return cached;
}

// 是否加载 *_ser.rgen.spv。本机 hardwareReorder=1 ⇒ 改口径前后结果相同，
// 差异只出现在报 MODE_NONE 的设备上（那里现在会用 hitObject 但重排是 no-op）。
bool serShaderVariantWanted()
{
    static const int serModeEnvPinned = []()
    {
        const char* raw = std::getenv("ORION_SER_MODE");
        return (raw != nullptr && raw[0] != '\0') ? std::atoi(raw) : -1;
    }();
    if(serModeEnvPinned == 0)
    {
        return false;
    }

    if(!g_serEnabled.load() || !g_serFeatureSupported)
    {
        return false;
    }

    return !serRequireReorderHint() || g_serHardwareReordering;
}

// ③ K2 传输段再拆分：K1→K2D(直射级)→K2I(间接级) 三发。
// 2026-11-06 落盘为默认开（叠加 SER 恢复后 trace 稳态 5.62ms，GPU 帧 8.41ms）；
// ORION_K2_SPLIT=0 仅作紧急回退口，正常启动无需任何 env。
bool k2DirectIndirectSplitEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_K2_SPLIT");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}

// ④ K2-D 再拆分：K2-D → K2-SUN（水+太阳直射/GGX/SSS/RTAO）+ K2-LOC（局部光/
// 水面反射太阳/GGX 镜面）+ K2-I 四发。动机：k2d 是最坏编译单元（2026-11-12
// 实测 26.3min 冷编，k2i 仅 91.8s），驱动并行粒度=per-stage，劈不开单 stage
// ⇒ 只能切单元。默认开；ORION_K2_SPLIT2=0 回退 ③ 的 K2-D/K2-I 两腿。
bool k2ThreeSplitEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_K2_SPLIT2");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}

// ⑤ K2-SUN 再拆分（2026-11-22）：K2-SUN → K2-SUNA（水+RTAO）+ K2-SUNB
//（阴影+GGX+植被SSS，load-add 复用 binding 89）。动机：k2sun 单腿优化冷编
// 20GB 级 commit、9.5~50 分钟，2026-11-21/22 四连 OOM（result=-3）；rgen 臂
// 占编译 ~85%（k2i 91.8s = 共享 6 stage 下限），劈半把单腿峰值拉到墙下。
// GPU 总工作量不变（同样光线同样着色，多一道腿间 barrier ≈ 0.1ms 级）。
// 默认开；ORION_K2_SPLIT3=0 回退 ④ 三腿（sun/loc/i）。
bool k2FourSplitEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_K2_SPLIT3");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}

// P2 跨腿材质复用（2026-11-17）：运行期开关，不是编译期宏。同一份二进制内
// 复用 / 非复用两条路径共存。
// 2026-11-17 晚（用户决定）：**默认开**。探针局 J3 已把差异面钉死在一个字段上：
// 165/165 窗口的位图都是 0x00010080 ⇒ 只有 pomSelfShadow（位 7）不同，且只出现在
// I 腿（位 16）；hitDistance/roughness/state/albedo/normal.xyz/emission 逐位相同，
// 量级约 3.5 像素/帧（1.4e-5）。该字段只乘太阳直射项，是一片刻度 0..1 的平滑遮蔽，
// 画面不可见。收益：trace −0.74 ms / −7.9%（对照 A 局），ev_primary 3→1。
// 显式 ORION_MAT_REUSE_LEGS=0/f/F 可回到逐位旧行为做反向对照。
bool matReuseLegsEnabled()
{
    if(k2ThreeSplitEnabled() || k2FourSplitEnabled())
    {
        return true;
    }

    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_MAT_REUSE_LEGS");

        if(raw == nullptr || raw[0] == '\0')
        {
            return true;
        }

        return raw[0] != '0' && raw[0] != 'f' && raw[0] != 'F';
    }();

    return cached;
}

// 一次性校验臂：消费者腿既读复用记录又原地重跑一次解析，逐位比对，不一致计数进
// 计数器缓冲 reservedLane6。默认关；需配合 ORION_RT_STAGE_COUNTERS=1 的采集档
// 部署才有落盘缓冲可读。
bool matReuseAssertEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_MAT_REUSE_ASSERT");
        return raw != nullptr && raw[0] != '\0' &&
               raw[0] != '0' && raw[0] != 'f' && raw[0] != 'F';
    }();

    return cached;
}

// 本地光 NEE 硬件可见性射线的跨腿结果缓存。默认关。
// 设计上的安全边界：只有 (receiverOrigin, selectedSample, targetVoxelEntryDistance,
// cullMask) 的 64 位键与记录逐位相同才复用，否则自己打射线 —— 所以"键不同"只会
// 白跑，不会算错；键相同则同一段线段、同一静态 TLAS，遍历结果必然逐位相同。
bool localNeeVisCacheEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_LOCAL_NEE_CACHE");
        return raw != nullptr && raw[0] != '\0' &&
               raw[0] != '0' && raw[0] != 'f' && raw[0] != 'F';
    }();

    return cached;
}

// 计数器 lane 6（reservedLane6，材质复用不一致计数）的额外打印开关。默认关：
// 现有 L10 [RT STAGE CNT] 日志只打 lane 0-5，格式不动；ORION_RT_STAGE_CNT_LANE6=1
// 才在每个窗口闭合时多打一行 matReuseMismatch=<lane6>。需配合计数器采集档部署。
bool rtStageCntLane6PrintEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_RT_STAGE_CNT_LANE6");
        return raw != nullptr && raw[0] != '\0' &&
               raw[0] != '0' && raw[0] != 'f' && raw[0] != 'F';
    }();

    return cached;
}


bool rrTransparencyHintEnabled()
{
    static const bool enabled = []() {
        const char* raw = std::getenv("ORION_RR_TRANSPARENCY_HINT");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();
    return enabled;
}
float sparseSamplingRateEffective()
{
    if(!g_dlssRRReady)
    {
        return 0.0f;
    }

    const int percent = sparseRatePercent();
    if(percent <= 0)
    {
        return 0.0f;
    }

    // 2026-11-02：直通开关（跳过分辨率折算）。Remix 的 calculateSamplingRate 会按
    // (参考像素数 / 实际 RT 输出像素数)^0.69 折算，小窗口/低内部分辨率下这个
    // 系数能到十几倍 —— ORION_SPARSE_RATE=50 会被折成 >0.75 然后被下面的
    // "rate > kMaxSamplingRate ⇒ 归 0" 直接关掉（实测 285x160 时 effective=0）。
    //
    // 2026-11-08：**默认改为直通**（原先需 ORION_SPARSE_RATE_RAW=1 才生效）。
    // 理由：本工程恒以远低于 2227x1238 的内部分辨率渲染再上采样（实测 707x376，
    // 折算系数 5.02），任何 >14% 的请求都会被放大过 0.75 然后归 0 ——
    // 结果是设置页「稀疏渲染采样率」这一项在任何档位下都等于关闭
    // （日志 4163 条 [SPARSE-DIAG] 全是 effective=0.000000）。
    // 直通后 UI 的百分比就是真实采样率；恢复 Remix 折算口径设
    // ORION_SPARSE_RATE_RAW=0。
    static const bool rawRate = []() {
        const char* raw = std::getenv("ORION_SPARSE_RATE_RAW");
        if(raw != nullptr && raw[0] != '\0' && raw[0] == '0')
        {
            return false;
        }
        return true;
    }();
    if(rawRate)
    {
        return std::min(
            std::max(static_cast<float>(percent) * 0.01f, 1.0f / 16.0f),
            0.75f);
    }

    constexpr float kReferencePixelCount = 2227.0f * 1238.0f;
    constexpr float kSamplingRateResolutionExponent = 0.69f;
    constexpr float kMinSamplingRate = 1.0f / 16.0f;
    constexpr float kMaxSamplingRate = 0.75f;

    const float pixelCount = static_cast<float>(
        std::max(1u, g_rtOutputWidth) *
        std::max(1u, g_rtOutputHeight));

    float rate = static_cast<float>(percent) * 0.01f;
    rate *= std::pow(
        kReferencePixelCount / pixelCount,
        kSamplingRateResolutionExponent);
    rate = std::clamp(rate, kMinSamplingRate, 1.0f);

    if(rate > kMaxSamplingRate)
    {
        rate = 0.0f;
    }

    static float previousRate = 0.0f;
    if(rate != previousRate)
    {
        previousRate = rate;
        g_sparseRateHistoryReset.store(true, std::memory_order_relaxed);

        g_sparseHistoryValid.store(false, std::memory_order_relaxed);
    }

    return rate;
}
bool cloudFarBandSplitActive()
{
    // 2026-11-16：远云脱离 RR。默认开；ORION_CLOUD_FARBAND=0 回到旧档
    // （云全部混进 RR 输入，逐位等价于改动前）。
    //
    // 只在"确实在做上采样"时才有意义：
    //   · 没有 RR/SR 输出图（g_dlssOutputImage 为空）⇒ 没有后合成目标；
    //   · 输出分辨率不大于渲染分辨率 ⇒ 分层不会带来任何细节收益。
    static const bool s_enabled = []() -> bool
    {
        const char* raw = std::getenv("ORION_CLOUD_FARBAND");

        if(raw != nullptr &&
           (raw[0] == '0' || raw[0] == 'n' || raw[0] == 'N'))
        {
            return false;
        }

        return true;
    }();

    if(!s_enabled)
    {
        return false;
    }

    if(g_dlssOutputImage == VK_NULL_HANDLE)
    {
        return false;
    }

    if(g_dlssOutputWidth <= g_rtOutputWidth ||
       g_dlssOutputHeight <= g_rtOutputHeight)
    {
        return false;
    }

    return cloudFarBandSplitDistance() > 0.0f;
}

float cloudFarBandSplitDistance()
{
    // 分带距离（世界单位，沿射线）。ORION_CLOUD_FARBAND_SPLIT 可调，<=0 关闭分层。
    static const float s_distance = []() -> float
    {
        const char* raw = std::getenv("ORION_CLOUD_FARBAND_SPLIT");

        if(raw == nullptr)
        {
            return 320.0f;
        }

        const float value = static_cast<float>(std::atof(raw));

        return value > 0.0f ? value : 0.0f;
    }();

    return s_distance;
}
float effectiveCloudFarFadeDistance()
{
    const float base = g_cloudFarFadeDistance.load();
    if(!g_cloudFadeFollowsRrMode.load() || base <= 0.0f)
    {
        return base;
    }

    const uint32_t outputHeight = g_dlssOutputHeight;
    const uint32_t renderHeight = g_rrGBufferHeight;
    if(outputHeight == 0u || renderHeight == 0u || outputHeight <= renderHeight)
    {
        return base;
    }

    const float upscale =
        static_cast<float>(outputHeight) / static_cast<float>(renderHeight);
    if(upscale < 2.0f)
    {
        return base;
    }

    const float effective = base / upscale;

    static int lastLoggedKey = -1;
    const int key = static_cast<int>(upscale * 100.0f + 0.5f) * 1000000 +
        static_cast<int>(effective / 100.0f + 0.5f);
    if(key != lastLoggedKey)
    {
        lastLoggedKey = key;
        std::stringstream fadeLog;
        fadeLog << "[CLOUDFADE]"
                << " base=" << static_cast<int>(base)
                << " upscale=" << upscale
                << " out=" << outputHeight << " render=" << renderHeight
                << " effective=" << static_cast<int>(effective);
        RTLog(fadeLog.str());
    }

    return effective;
}
void populateCloudParams(float* params)
{
    params[0] = g_cloudBottom.load();
    params[1] = g_cloudThickness.load();
    params[2] = g_cloudCoverage.load();
    params[3] = g_cloudDensity.load();
    params[4] = g_cloudDetailStrength.load();
    params[5] = g_cloudWeatherScale.load();
    params[6] = g_cloudViewDistance.load();
    params[7] = g_cloudEnabled.load() ? 1.0f : 0.0f;
    const uint32_t quality = g_cloudQuality.load();
    params[8] = quality == 0u ? 48.0f : (quality == 2u ? 72.0f : 56.0f);
    params[9] = quality == 0u ? 10.0f : (quality == 2u ? 16.0f : 12.0f);
    params[10] = g_cloudSilver.load();
    params[11] = quality == 0u ? 0.72f : (quality == 2u ? 0.80f : 0.74f);
    params[12] = g_cloudWorldAnchorX;
    params[13] = g_cloudWorldAnchorZ;
    params[14] = g_cloudSize.load();
    params[15] = g_cloudDistribution.load();
    params[16] = g_cloudBaseFeather.load();
    // 2026-11-11：原"雨天地面雾浪"占位车道，改作云隙光三件套
    //（消费方 volumetric_fog.glsl: cloudGapTransmittance）。
    //   y = 开关, z = 云层光学厚度倍率（强度）, w = 缝宽偏置
    params[17] = g_cloudGapEnabled.load() ? 1.0f : 0.0f;
    params[18] = std::max(g_cloudGapStrength.load(), 0.0f);
    params[19] = std::min(std::max(g_cloudGapWidth.load(), 0.0f), 1.0f);
    params[20] = g_cloudType.load();
    params[21] = g_cloudErosionScale.load();
    params[22] = g_cloudSunlightStrength.load();
    params[23] = g_cloudPowderStrength.load();
    params[24] = g_cloudFarStepDensity.load();
    // 2026-11-07 复核：cloudParams6.y/.z 的**实际消费者是步数覆盖**，
    // 不是"太阳红化 / 云顶拉伸"（这两个 atomic 的名字是历史误名）：
    //   volumetric_clouds_v5.comp:159-162  stepsOverride = cloudParams6.y
    //   volumetric_clouds_v5.comp:172-175  shadowSteps  = cloudParams6.z
    // 界面上的「光线步进 / 阴影步进」滑条就是写这两个 lane，是对的。
    // （tyndall_cloud_field.glsl:138 把 .z 当"顶部拉伸"用，是旧路径的残留解读。）
    params[25] = g_cloudSunReddening.load();
    params[26] = g_cloudTopStretch.load();
    params[27] = g_cloudDetileStrength.load();

    params[28] = g_fogEnabled.load() ? 1.0f : 0.0f;
    params[29] = g_fogBaseDensity.load();
    params[30] = g_fogHeightBlock.load();
    params[31] = g_fogScaleHeight.load();
    params[32] = g_fogAnisotropyG.load();
    params[33] = g_fogSunStrength.load();
    params[34] = g_fogAmbientStrength.load();
    params[35] = g_fogMaxDistance.load();
    params[36] = g_fogStartDistance.load();
    params[37] = static_cast<float>(g_fogSunSteps.load());
    params[38] = g_fogRainBoost.load();
    params[39] = 0.0f; // 2026-10-31：雾浪已删（原 cloudParams9.w）
    {
        static bool fogParamsDumped = false;
        if(!fogParamsDumped)
        {
            fogParamsDumped = true;
            RTLog(
                "[FOG] enabled=" + std::to_string(params[28]) +
                " baseDensity=" + std::to_string(params[29]) +
                " heightMax=" + std::to_string(params[30]) +
                " heightMin=" + std::to_string(params[31]) +
                " g=" + std::to_string(params[32]) +
                " sun=" + std::to_string(params[33]) +
                " ambient=" + std::to_string(params[34]) +
                " maxDist=" + std::to_string(params[35]) +
                " startDist=" + std::to_string(params[36]) +
                " sunSteps=" + std::to_string(params[37]) +
                " rainMul=" + std::to_string(params[38])
            );
        }
    }

    {
        static bool dumped = false;
        if(!dumped)
        {
            dumped = true;
            if(FILE* file = std::fopen("cloud_v4_debug.log", "a"))
            {
                std::fprintf(file,
                    "ubo cloud: bottom=%.1f thick=%.1f coverage=%.3f density=%.3f "
                    "steps=%.0f shadow=%.0f size=%.3f type=%.2f erosion=%.2f detail=%.2f "
                    "sunLight=%.2f powder=%.2f farStep=%.2f\n",
                    params[0], params[1], params[2], params[3],
                    params[8], params[9], params[11], params[20], params[21], params[27],
                    params[22], params[23], params[24]);
                std::fclose(file);
            }
        }
    }
}
void populateSkyParams(float* params)
{
    params[0] = g_sunDiskLuminance.load();
    params[1] = g_sunHaloStrength.load();
    params[2] = g_sunAngularDiameterDegrees.load();
    params[3] = g_starIntensity.load();
    params[4] = g_sunTintStrength.load();
    params[5] = g_skyScatterEnabled.load() ? 1.0f : 0.0f;
    params[6] = g_skyScatterIntensity.load();
    params[7] = g_ozoneStrength.load();
}

// =====================================================
// 2026-11-01 天空散射模型切换（A 方案：Hillaire 结构）
//
// 车道语义见 struct CameraUBO 的 skyModelParams 注释。取值优先级：
//   1) 环境变量 ORION_SKY_MODEL（"0" = Bruneton，"1" = Hillaire）
//   2) 标记文件 run/sky_model_hillaire.flag（存在 ⇒ 1，删掉即回 Bruneton）
//   3) 默认 0 = Bruneton（出厂口径：与改动前逐位一致）
// 辐亮度标定：ORION_SKY_MODEL_SCALE（浮点，缺省 1.0）。两个模型的量纲不完全
// 一致（Bruneton 走 LUT 的 spectral 定义、Hillaire 走逐像素 march），所以
// 对齐靠这个旋钮，不必重编。
//
// 生效时机：环境变量 / 标记文件在进程内只解析一次（它们要 ifstream），
// 但**模型与标定倍率每帧从原子量读** ⇒ 设置界面改完下一帧就生效
//（天空 LUT 每帧重算，既不用重启也不用重建管线）。
// =====================================================
void populateSkyModelParams(float* params)
{
    static const float overrideModel = [] {
        const char* raw = std::getenv("ORION_SKY_MODEL");

        if(raw != nullptr && raw[0] != '\0')
        {
            const float parsed = static_cast<float>(std::atof(raw));
            RTLog("[SKY MODEL] env ORION_SKY_MODEL=" + std::to_string(parsed) +
                  (parsed > 0.5f ? " => Hillaire 结构" : " => Bruneton LUT"));
            return parsed > 0.5f ? 1.0f : 0.0f;
        }

        std::ifstream marker(ResolveRTXRuntimePath("sky_model_hillaire.flag"));

        if(marker.good())
        {
            RTLog("[SKY MODEL] 标记文件 sky_model_hillaire.flag => Hillaire 结构");
            return 1.0f;
        }

        return -1.0f;   // 无覆盖 ⇒ 用设置值
    }();

    static const float overrideScale = [] {
        const char* raw = std::getenv("ORION_SKY_MODEL_SCALE");

        if(raw != nullptr && raw[0] != '\0')
        {
            const float parsed = static_cast<float>(std::atof(raw));

            if(parsed > 0.0f)
            {
                RTLog("[SKY MODEL] 辐亮度标定倍率=" + std::to_string(parsed));
                return parsed;
            }
        }

        return -1.0f;   // 无覆盖 ⇒ 用设置值
    }();

    // 每帧读原子量 ⇒ 设置界面改完下一帧生效。
    params[0] = overrideModel >= 0.0f
        ? overrideModel
        : static_cast<float>(g_skyModel.load());
    params[1] = overrideScale > 0.0f
        ? overrideScale
        : g_skyModelScale.load();
    // z / w：2026-11-01 起不再保留（夜间天空底光 / 雨天压暗倍率）
    params[2] = g_skyNightAmbient.load();
    params[3] = g_skyRainDim.load();
}

// 2026-11-16 月光两旋钮（车道见 CameraUBO.moonParams）。
// 每帧读原子量 ⇒ UI 改完下一帧生效（天空 LUT 与 miss 共享同一份 UBO）。
void populateMoonParams(float* params)
{
    params[0] = g_moonLightFloor.load();
    params[1] = g_moonDaylightDim.load();
    // z = 有效性标记：shader 靠它区分"宿主已写入"与"旧 DLL 未写本块"。
    params[2] = 1.0f;
    params[3] = 0.0f;
}

// 2026-11-16 雾外观（车道见 CameraUBO.fogMoodParams）。
// 每帧读原子量 ⇒ UI 改完下一帧生效。
void populateFogMoodParams(float* params)
{
    params[0] = g_fogShapeStrength.load();
    params[1] = g_fogDawnBoost.load();
    // 2026-11-26 (Batch V): [2] = underwaterSkyFactor (default 1.0 open sky),
    // [3] = tlasHasWater (default 1.0 active water). Overwritten per-frame in Frame.cpp.
    params[2] = 1.0f;
    params[3] = 1.0f;
}

// 2026-11-21 统一风场（车道见 CameraUBO.windParams，消费方 wind_common.glsl）。
// 每帧读原子量 ⇒ UI 改完下一帧生效。
//   [0] = 风向角（度）  [1] = 风强倍率  [2] = 阵风强度  [3] = 有效标记
void populateWindParams(float* params)
{
    params[0] = std::min(std::max(g_windAngle.load(), 0.0f), 360.0f);
    params[1] = std::min(std::max(g_windStrength.load(), 0.0f), 2.0f);
    params[2] = std::min(std::max(g_windGust.load(), 0.0f), 1.0f);
    params[3] = 1.0f;
}

// 末地银河四个旋钮（2026-10-08，最后一个当天由"替换式"改成叠加时启用）。
// 只有 rt_test.rmiss 的 galaxyApplyParams 读；lane 语义见 CameraUBO.galaxyParams 尾块注释。
void populateGalaxyParams(float* params)
{
    params[0] = g_galaxyScale.load();
    params[1] = g_galaxyTiltDegrees.load();
    params[2] = g_galaxyAzimuthDegrees.load();
    params[3] = g_galaxyMix.load();
}

// Volumetric light (god rays).  See the CameraUBO comment for the lane map.
//
// 当前实际写入的 lane：
//   [0]   enabled（[0].x；[0].yzw 保留）
//   [1]   临时太阳遮挡调试视图（2026-10-14，靠 godray_debug_vis.flag 开启）
//   [2]   保留，写 0（2026-10-06 前的雾内自阴影步数，随设置删除）
//   [3]   强度 g_godRayIntensity（设置界面「体积光强度」，默认 1.0 = 标定档）
//   [4..7] 由 populateGroundMistParams() 写（贴地薄雾，2026-11-04 shader 侧复活，
//          消费方 volumetric_fog.glsl；出厂值 enabled/1.0/0.15/48.0）
//   [8]   视线 march 步数 g_godRaySteps
//   [9]   云影 march 采样数 g_godRayOcclusionSamples
//   [10]  保留，写 0（2026-10-06 前的雾内自阴影标定，随设置删除）
//   [11]  保留，写 0（2026-11-07 死设置清理：原「雨天换介质」g_fogRainMediumMix，
//          旧 Rayleigh 通道退役后 volumetric_light.comp 只读 godRayParams2 的
//          .x/.y，.w 零读取；UI 滑条也已不存在 ⇒ atomic 已删）
// godRayParams1 的其余位保留。**数组仍是 12 个 float、起点仍是偏移 640**：
// rainWallParams 紧跟在 688，缩短这个块会把后面所有参数整体平移，静默污染
// 水墙与水色读取。
void populateGodRayParams(float* params)
{
    params[0] = g_godRayEnabled.load() ? 1.0f : 0.0f;
    static const bool godRayDebugVisibility = [] {
        std::ifstream marker(ResolveRTXRuntimePath("godray_debug_vis.flag"));
        const bool enabled = marker.good();
        if(enabled)
        {
            RTLog("[GODRAY DEBUG] sun-visibility view ACTIVE (godray_debug_vis.flag)");
        }
        return enabled;
    }();
    params[1] = godRayDebugVisibility ? 1.0f : 0.0f;
    params[2] = 0.0f;
    params[3] = g_godRayIntensity.load();
    params[4] = 0.0f;
    params[5] = 0.0f;
    params[6] = 0.0f;
    params[7] = 0.0f;
    params[8] = static_cast<float>(g_godRaySteps.load());
    params[9] = static_cast<float>(g_godRayOcclusionSamples.load());
    params[10] = 0.0f;
    params[11] = 0.0f;
}

// 旧档贴地薄雾。写的是 godRayParams 的 [4..7]（调用方传 +4 的偏移）。
// 该雾当前已退役，着色器里没有消费者；保留写入只为布局/旧配置兼容。
void populateGroundMistParams(float* params)
{
    params[0] = g_fogGroundMistEnabled.load() ? 1.0f : 0.0f;
    params[1] = g_fogGroundMistHeight.load();
    params[2] = g_fogGroundMistExtinction.load();
    params[3] = g_fogGroundMistDistance.load();

    // 2026-11-10：平流雾滚动速度 → godRayParams2.z（= godRayParams[10]）。
    // 本函数的入参是 &godRayParams[4]，所以 params[6] 正好落在 [10]。
    // 消费方：volumetric_fog.glsl 的 rollingFogDensity（<=0 时回落到默认 1.35）。
    params[6] = std::max(g_fogGroundMistSpeed.load(), 0.0f);

    // 2026-11-11：雨波纹旋钮打包进 godRayParams2.w（= godRayParams[11]，params[7]）。
    //   w = size + strength * 8   （size 0.25~4，strength 0~3）
    // 消费方：rchit_helpers_water.glsl / material_common.glsl 的
    //         rainImpactRippleGradient（水面与地面共用同一个解析场）。
    // 这是该 vec4 里唯一空闲的车道（.x/.y 归旧体积光，.z 归平流雾速度）。
    {
        const float rippleSize =
            std::min(std::max(g_rainRippleSize.load(), 0.25f), 4.0f);
        const float rippleStrength =
            std::min(std::max(g_rainRippleStrength.load(), 0.0f), 3.0f);
        params[7] = rippleSize + rippleStrength * 8.0f;
    }
}

// 高层平面云（2026-10-02 清理）：该层已整层退役，参数侧全部移除 ⇒ 这里恒定写 0
//（lane 作为 std140 布局占位保留，见上面原子量段的注释）。
// 副作用：highCloudParams[0] = 0 ⇒ volumetric_clouds.comp 的 highCloudEnabled 恒为
// false ⇒ 云/雾全关时那一帧可以正常早退（旧默认是 true，会白跑一趟）。
// 平面云。20 个 lane 写到 CameraUBO.highCloudParams
// （偏移 944），与 camera_ubo.glsl / high_cloud_field.glsl 的 lane 注释逐字对应。
// 只是预初始化：setPlaneCloudParams 会用 Java 默认值覆盖一遍。hc3/hc4 恒 0。
void populateHighCloudParams(float* params)
{
    for(int i = 0; i < 20; ++i)
    {
        params[i] = 0.0f;
    }
    params[0] = g_planeCloudEnabled.load();
    params[1] = g_planeCloudHeightOffset.load();
    params[2] = g_planeCloudScale.load();
    params[3] = g_planeCloudSpeed.load();
    params[4] = g_planeCloudDensity.load();
    params[5] = g_planeCloudCoverage.load();
    params[6] = g_planeCloudOctaves.load();
    params[7] = g_planeCloudFadeDistance.load();
    params[8] = g_planeCloudBrightness.load();
}

// phi_fwd 多重散射漫射场 + MS 残量尾巴（2026-10-02）。4 个 lane 写到
// CameraUBO.cloudMsParams，与 volumetric_clouds.comp 的 cloudMsParams 注释
// 逐字对应。同样只是预初始化：setCloudMsParams 会用 Java 默认值覆盖。
void populateCloudMsParams(float* params)
{
    params[0] = g_cloudPhiIntensity.load();
    params[1] = g_cloudPhiDepthPow.load();
    params[2] = g_cloudPhiDepthBias.load();
    params[3] = g_cloudMsTailStrength.load();
}

// 批次2（2026-10-02）步进旋钮。4 个 lane 写到 CameraUBO.cloudMarchParams
//（偏移 1040 尾块），与 volumetric_clouds.comp 的 cloudMarchParams 注释逐字
// 对应。[2] 保留写 0。
void populateCloudMarchParams(float* params)
{
    params[0] = g_cloudAdaptiveLightRange.load();
    params[1] = g_cloudTwoTierStep.load();
    params[2] = effectiveCloudFarFadeDistance();
    params[3] = g_cloudMeanDepthReproject.load();
}

// 参考重构（2026-10-02）phi_fwd 四件套。4 个 lane 写到 CameraUBO.cloudPhiParams
//（偏移 1072 尾块），与 volumetric_clouds.comp 的 cloudPhiParams 注释逐字对应。
// 允许负值：<0 表示"回落到着色器编译期宏"，供 A/B 用（BoundaryConfidence 除外，
// 它被钳到 [0,1]，0 本身就是合法的"关闭"）。
void populateCloudPhiParams(float* params)
{
    params[0] = g_cloudPhiBoundaryConfidence.load();
    params[1] = g_cloudPhiMsBuildScale.load();
    params[2] = g_cloudPhiCompress.load();
    params[3] = g_cloudPhiConeRatio.load();
}

// 参考重构（2026-10-02）受光逻辑的三个尺度量。4 个 lane 写到
// CameraUBO.cloudLightParams（偏移 1088 尾块）。0/负值语义见该结构体注释。
//
// 2026-11-07 死设置清理：v3→v4→v5 云模型迁移后，`cloudLightParams` 在
// cpp/shaders 里**只剩声明、零读取**（唯一消费者早已是 v5，
// Settings.cpp 旧注释里写的 volumetric_clouds_v4.comp 也已退役）。
// 对应 UI 滑条在更早一次改版里就没了控件，只剩存档/JNI/atomic 三层残留。
// 四个 atomic 已删；这里显式写 0 保持 std140 偏移与
// "未使用 = 0" 的契约（同 cloudParams4 尾块的处理）。
void populateCloudLightParams(float* params)
{
    params[0] = 0.0f;
    params[1] = 0.0f;
    params[2] = 0.0f;
    params[3] = 0.0f;
}

// 参考重构（2026-10-02）云底软化 + 两级步进阈值。4 个 lane 写到
// CameraUBO.cloudShapeParams（偏移 1104 尾块，四个消费者都声明）。
void populateCloudShapeParams(float* params)
{
    params[0] = g_cloudBottomSmoothHeight.load();
    params[1] = g_cloudBottomSmoothPow.load();
    params[2] = g_cloudTwoTierThreshold.load();
    // 2026-11-13 审计 ⑤：原来恒写 0 ⇒ volumetric_clouds_v5.comp:325 读到的永远是 0。
    params[3] = g_cloudLightStepErosion.load();
}

// 参考重构（2026-10-02）物理月光项。4 个 lane 写到 CameraUBO.cloudMoonParams
//（偏移 1120 尾块，只有本 pass 声明）。
void populateCloudMoonParams(float* params)
{
    params[0] = g_cloudMoonStrength.load();
    params[1] = g_cloudMoonPhase.load();
    params[2] = 0.0f;
    params[3] = 0.0f;
}

// 2026-10-22 UE 风格体积云八项。写 CameraUBO.ueCloudA / ueCloudB（偏移
// 1536/1552；唯一下发入口，唯一消费者 volumetric_clouds_v4.comp）。
// 出厂顺序与 NativeVulkan.setUeCloudParam 的 index 严格一致。
void populateUeCloudParams(float* cloudA, float* cloudB)
{
    for(int lane = 0; lane < 4; ++lane) cloudA[lane] = g_ueCloudParams[lane].load();
    for(int lane = 0; lane < 4; ++lane) cloudB[lane] = g_ueCloudParams[4 + lane].load();
}

// 第四阶段（2026-10-02）细节/形状旋钮。4 个 lane 写到 CameraUBO.cloudShape2Params
//（偏移 1136 尾块；tyndall_cloud_field.glsl 直接读，四个消费者都声明）。
void populateCloudShape2Params(float* params)
{
    params[0] = g_cloudDetailScale.load();
    params[1] = g_cloudDetailContrast.load();
    params[2] = g_cloudProfileCurve.load();
    params[3] = 0.0f;
}

// 参考重构（2026-10-02）#2~#7 五块。4 个 lane 各自写到对应的 CameraUBO 尾块
//（偏移 1152/1168/1184/1200/1216），与 volumetric_clouds.comp 的 cam 块注释逐字
// 对应；Profile/Detail 两块同时被密度场的另外三个消费者读取（云影/反射/光柱）。
void populateCloudAmbientParams(float* params)
{
    params[0] = g_cloudAmbientTopMul.load();
    params[1] = g_cloudAmbientBottomMul.load();
    params[2] = g_cloudAmbientSunSide.load();
    params[3] = g_cloudAmbientHueFollow.load();
}
void populateCloudMsParams2(float* params)
{
    params[0] = g_cloudScatterTint.load();
    params[1] = g_cloudMsOctaves.load();
    params[2] = g_cloudMsAttenuation.load();
    params[3] = g_cloudMsEccentricity.load();
}
// 2026-11-07 死设置清理：`cloudLightParams2` 四个 lane 在 cpp/shaders 里零读取
// （只剩 camera_ubo.glsl 声明）。其中"光粉强度"不是死项 —— 同一个
// g_cloudPowderStrength 还写进 cloudParams[23]（上面 populateCloudParams），
// 那条路是 v5 的真实消费者，所以该 atomic 保留。这里四 lane 全写 0。
void populateCloudLightParams2(float* params)
{
    params[0] = g_cloudPowderEffect.load();
    params[1] = g_cloudSilverSpread.load();
    params[2] = g_cloudSharpness.load();
    params[3] = g_cloudBaseErosion.load();
}
void populateCloudProfileParams(float* params)
{
    params[0] = g_cloudProfileType.load();
    params[1] = g_cloudProfileTypeSpread.load();
    params[2] = g_godRayHistoryWeight.load();
    params[3] = 0.0f;
}
void populateCloudDetailParams(float* params)
{
    params[0] = g_cloudWispyStrength.load();
    params[1] = g_cloudWispyEdgeWidth.load();
    params[2] = g_cloudWispyReach.load();
    params[3] = g_cloudWispyTopHeight.load();
}

// 2026-10-10：雾附加项（CameraUBO.fogExtrasParams，
// 偏移 1376）。4 个 lane 与 camera_ubo.glsl / volumetric_fog.glsl 的注释逐字对应。
// 只是预初始化：setFogExtrasParams 会用 Java 默认值覆盖。
void populateFogExtrasParams(float* params)
{
    // 2026-11-01：ReSTIR 体积光接管局部光照明之后，解析雾里那条动态光项必须关掉。
    // 它没有任何可见性射线：volumetric_fog.glsl 只在"视线 24 格处"收集一次灯表，
    // 然后对每个 march 步按距离/相位相加 ⇒ 墙那边的火把会照亮墙这边的雾（漏光），
    // 并且与 ReSTIR 的同一份照明双计。
    //
    // 2026-11-07 死设置清理：界面上早已没有这根滑条，atomic 已删。但**不能常量写 0**：
    // 原逻辑在「ReSTIR 体积光关闭」时这项是活的（值 = g_fogDynamicLightStrength，
    // 出厂 1.0）⇒ 这里保留原来的门控，用常量 1.0 代表出厂值，行为不变。
    params[0] = (volRestirEnabled() != 0u)
        ? 0.0f
        : (g_cloudFogLightDescriptorsReady.load() ? 1.0f : 0.0f);
    params[1] = g_fogGroundAnchorStrength.load();
    params[2] = g_fogSkyAmbientStrength.load();
    params[3] = g_fogTemporalJitterStrength.load();
}

// 2026-10-10：云时域 / 天光质量旋钮
//（CameraUBO.cloudTemporalParams，偏移 1392）。
// 2026-10-16：.w 从"保留写 0"升格为**底部遮蔽强度**（见上面 atomic 的注释）。
//
// 2026-11-07 死设置清理：只有 .z（时域阻尼）有消费者
// （volumetric_cloud_resolve.comp:182）。x/y/w 三个（天光可见性强度、
// 天光可见性步数、云底遮蔽）在全部 cpp/shaders 里零读取，UI 控件也早已
// 移除 ⇒ 三个 atomic 已删，这里写 0 保持 std140 偏移。
void populateCloudTemporalParams(float* params)
{
    params[0] = static_cast<float>(g_cloudTemporalMode.load());
    params[1] = 0.0f;
    params[2] = g_cloudTemporalDamp.load();
    params[3] = 0.0f;
}

// 2026-10-10 借鉴（CameraUBO.cloudAerialParams，偏移 1408）。
// #1 云后空气透视强度 / #2 地面大气透视强度 + 遮挡衰减。.w 保留写 0。
//
// 2026-11-07 死设置清理：.x（云后大气透视强度）唯一的读取点是退役模型
// volumetric_clouds_v3.comp:294，而 v3 只在运行时存在 cloud_v3.flag 标记文件
// 时才启用（volumetricCloudV3Requested，本文件顶部）⇒ 默认档下一个永远
// 不生效的滑条。UI 滑条已摘除、atomic 已删，.x 写 0。
// .y/.z 是活的（rt_test.rgen 的地面大气透视 + 遮挡衰减）。
void populateCloudAerialParams(float* params)
{
    params[0] = g_cloudAerialStrength.load();
    params[1] = g_groundAerialStrength.load();
    params[2] = g_groundAerialOcclusion.load();
    params[3] = 0.0f;
}

void populateCloudSkyHueParams(float* params)
{
    params[0] = g_cloudStormDarkness.load();
    params[1] = g_cloudLightningStrength.load();
    params[2] = g_cloudSunsetWarmth.load();
    params[3] = g_cloudWeatherCoupling.load();
}

// 2026-10-15 云地面反射与视觉回退参数（CameraUBO.cloudPhysicsParams，偏移 1488）。
// x 反照率 / y 地面反射强度 / z 自阴影残光回退 / w 相位地板回退。
void populateCloudPhysicsParams(float* params)
{
    params[0] = g_cloudGroundAlbedo.load();
    params[1] = g_cloudGroundBounce.load();
    params[2] = g_cloudSunlightResidual.load();
    params[3] = g_cloudPhaseFloor.load();
}

// 批次3（2026-10-02）水体观感四件套。4 个 lane 写到 CameraUBO.waterParams
//（偏移 1056 尾块），与 rt_test.rgen 的 waterParams 注释逐字对应。
void populateWaterParams(float* params)
{
    params[0] = g_waterCrestBacklit.load();
    params[1] = g_waterCausticDispersion.load();
    params[2] = g_waterCausticDepthDiffusion.load();
    params[3] = g_waterPhysicalPhase.load();
}
void populateRainWallParams(float* params)
{
    params[0] = g_rainWallEnabled.load() ? 1.0f : 0.0f;
    params[1] = g_rainWallWetness.load();
    params[2] = g_rainWallDripDensity.load();
    params[3] = g_rainWallDripSpeed.load();
    params[4] = g_rainWallDripSize.load();
    params[5] = g_rainWallDistance.load();
    params[6] = g_rainWallDripBump.load();
    params[7] = g_localLightMaxDistance.load();
}
// 2026-10-03 按 HPWater 重构自然波面：波方程场的"风源"强度（写进 push 的保留槽，
// 见 water_sim.comp 的风源块）。0 = 只有交互/落水才起浪；1 = 常驻的自然流动波纹。
// **它还决定"水模拟是否需要常驻运行"**：风源开着时 recordWaterSimulation 不允许
// 进入 idle —— 否则站在岸上看水面时模拟根本不跑、波场恒为零，画面上看不出任何
// 变化（这正是风源第一次上线却没效果的原因）。
//
// 2026-10-14 注：曾把此值置 0 试图治「重复感」（理由：风源是 5 个绕玩家盒中心
// 公转的高斯包，盒子跟玩家走 ⇒ 波前贴在相机上、周期循环）。但用户明确"重复感"
// 说的是**纯 afl 波面**、不是交互场 ⇒ 该改动超出本次范围，**已还原为 1.0f**
// （真正的重复感根因是 afl 谱带太窄，见 water_wave_field.glsl 的 2026-10-14 注释）。
// 若日后要临时关掉常驻风源做 A/B：把这行改 0.0f、重编 DLL 即可（纯源码，不动 spv）。
// 2026-10-25：风源默认 **关闭**（0）。
// 水面视觉的波纹感由 FFT 斜率图（binding 59）承担；water_sim 只保留交互脉冲。
// 这样无交互时 recordWaterSimulation 直接 idle，省掉实测 12.47 ms/帧。
// 要恢复"常驻风源水面"：ORION_WATER_WIND=1
float waterWindStrength()
{
    static const float cached = []() -> float
    {
        const char* raw = std::getenv("ORION_WATER_WIND");

        if(raw != nullptr && std::atoi(raw) != 0)
        {
            return 1.0f;
        }

        return 0.0f;
    }();

    return cached;
}
void populateWaterSssParams(float* params)
{
    params[0] = g_waterSssNonlinear.load();
    params[1] = g_waterSssBoost.load();
    params[2] = g_waterSssPhaseG.load();
    params[3] = g_waterSssStrength.load();
}
void populateWaterWaveParams(float* params)
{
    params[0] = g_waterWaveCalibration.load();
    params[1] = g_waterWaveStrength.load();
    params[2] = g_waterWaveBlend.load();
    params[3] = g_waterReflectCausticStrength.load();
}

// 2026-10-14 ROUTE A：写复用后的 waterWaveShapeParams 尾块（详见上方注释）。
void populateWaterWaveShapeParams(float* params)
{
    params[0] = g_waterWaveFbmWavelength.load();
    params[1] = g_waterWaveFbmWarp.load();
    params[2] = g_waterWaveFbmOctaves.load();
    params[3] = g_waterWaveFbmWaves.load();
}
void populateWaterStyleParams(float* params)
{
    params[0] = g_waterStyleWaveScale.load();
    params[1] = g_waterStyleTintMix.load();
    params[2] = g_waterStyleDensityScale.load();
    params[3] = g_waterStyleCausticStrength.load();
    // 2026-11-20：waterStyleParams1.x 改吃「风速」（g_waterFftWindSpeed/7），与 FFT
    // 主表同源。这条车道唯一的活消费者是 rt_test_translucent.rchit 的解析波相位
    // （group3 半透明命中组），原来由已下线的「波速」滑杆写死 1.0，跟 FFT 主表
    // 的快慢不一致。现在两套波面共用同一个时间标量。
    params[4] = std::min(std::max(g_waterFftWindSpeed.load() / 7.0f, 0.05f), 2.0f);
    params[5] = g_waterStyleSwellScale.load();
    params[6] = g_waterStyleMidScale.load();
    params[7] = g_waterStyleFineScale.load();
}



// 保护半径（格）：比它近的一律不驱逐。96 格保证"玩家周围一圈"完整，
// 也给补传留出缓冲带（补传阈值 = 本值，驱逐阈值 = 本值 × 1.25，中间那条带子防抖）。
float vramEvictKeepRadius()
{
    static const float cached = []()
    {
        const char* raw = std::getenv("ORION_VRAM_EVICT_KEEP_RADIUS");

        if(raw == nullptr || raw[0] == '\0')
        {
            // 2026-11-06p?96 -> 288?
            //
            // ???16 ???? = 256 ???over ? keep = 96*0.75 = 72?
            // ???? = 72*1.25 = 90????? = 72???? 90~256 ?????
            // Section ???? BLAS???????? 72 -> ??????
            // ???nonResident=2665 / secEvicted=3004 / secRehydrated=230?
            // meshSec=6682 ? blas=4447?? 2235?= "??????????"?
            //
            // 288 ? over ? keep=216???=270???=216??? 54 ?????
            // ????????????????????????
            return 288.0f;
        }

        const double parsed = std::atof(raw);
        return (parsed >= 16.0 && parsed <= 512.0)
            ? static_cast<float>(parsed)
            : 288.0f;
    }();

    return cached;
}

// 每批（每次重建）驱逐 / 补传的 Section 上限。补传是**重建**，一次塞太多会把这一帧
// 拖长（maxSectionMs 到过 92ms）；驱逐只是 retire + erase，可以多放一些。
size_t vramEvictPerPass()
{
    static const size_t cached = []() -> size_t
    {
        const char* raw = std::getenv("ORION_VRAM_EVICT_PER_PASS");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 16u;
        }

        const long parsed = std::atol(raw);
        return (parsed > 0 && parsed <= 4096)
            ? static_cast<size_t>(parsed)
            : 16u;
    }();

    return cached;
}
size_t vramRehydratePerPass()
{
    static const size_t cached = []() -> size_t
    {
        const char* raw = std::getenv("ORION_VRAM_REHYDRATE_PER_PASS");

        if(raw == nullptr || raw[0] == '\0')
        {
            // 2026-11-06p?16 -> 64???????????16/? ????????
            // ????"?????"?64/? ??????????? BLAS??
            return 64u;
        }

        const long parsed = std::atol(raw);
        return (parsed > 0 && parsed <= 4096)
            ? static_cast<size_t>(parsed)
            : 64u;
    }();

    return cached;
}

// spatial Section 键 → 世界坐标中心（16 格/段 + 半格）。
// 键位布局见 packSectionKey：bits0..21=X、22..43=Z、44..59=Y（各自补码）。
void vramSectionCenterFromSpatialKey(
    uint64_t spatialKey,
    float& outX,
    float& outY,
    float& outZ
)
{
    auto signExtend = [](uint64_t value, int bits) -> int {
        const uint64_t mask = 1ull << (bits - 1);
        return static_cast<int>((value ^ mask) - mask);
    };

    outX = static_cast<float>(signExtend(spatialKey & 0x3FFFFFull, 22)) * 16.0f + 8.0f;
    outZ = static_cast<float>(signExtend((spatialKey >> 22) & 0x3FFFFFull, 22)) * 16.0f + 8.0f;
    outY = static_cast<float>(signExtend((spatialKey >> 44) & 0xFFFFull, 16)) * 16.0f + 8.0f;
}

// 只处理"真实世界高度"的 Section：voxy LOD / DH LOD 与动态实体聚合共用保留 Y 段，
// 它们的 X/Z **不是世界坐标**（LOD 是哈希分配槽位）⇒ 对它算距离恒为天文数字。
// 与 TLAS 距离 / 视锥剔除用的是同一条判据（见 rebuildWorldGeometry 里的 ⚠⚠ 注释）。
bool vramWorldHeightSection(uint64_t spatialKey)
{
    const int32_t sectionY =
        static_cast<int32_t>(
            (static_cast<uint32_t>(spatialKey >> 44) & 0xFFFFu) << 16) >> 16;

    return sectionY >= -64 && sectionY <= 512;
}


// 2026-10-21：重建入口取 g_chunkMutex 的等待预算（毫秒，0 = 退回旧的阻塞行为）。
// 见 rebuildWorldGeometry 里"有界取锁"一节的实测依据。
int64_t geometryPreLoopLockWaitMs()
{
    static const int64_t cached = []() -> int64_t
    {
        const char* raw = std::getenv("ORION_GEOMETRY_LOCK_WAIT_MS");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 3;
        }

        const long parsed = std::strtol(raw, nullptr, 10);

        if(parsed <= 0)
        {
            return 0;
        }

        return std::min<long>(parsed, 50);
    }();

    return cached;
}
