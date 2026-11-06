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

        int hz = 15;

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
        g_hdrSurfaceSupported.load();
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
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
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
            return 0;
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
bool k1k2SplitEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_K1_K2");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
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

    // 2026-11-02：A/B 专用直通开关。Remix 的 calculateSamplingRate 会按
    // (参考像素数 / 实际 RT 输出像素数)^0.69 折算，小窗口/低内部分辨率下这个
    // 系数能到十几倍 —— ORION_SPARSE_RATE=50 会被折成 >0.75 然后被下面的
    // "rate > kMaxSamplingRate ⇒ 归 0" 直接关掉（实测 285x160 时 effective=0）。
    // 做 A/B 时需要两臂拿到同一个确定的有效率，所以给一条跳过折算的路。
    static const bool rawRate = []() {
        const char* raw = std::getenv("ORION_SPARSE_RATE_RAW");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
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
    constexpr bool kEnableFarBandSplit = false;
    return kEnableFarBandSplit &&
        !g_cloudPostRrComposite.load() &&
        g_rrFarCloudDistance.load() > 0.0f &&
        g_cloudResFloorPercent.load() > 0u;
}
float cloudFarBandSplitDistance()
{
    return std::max(g_rrFarCloudDistance.load(), 0.0f);
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
    params[11] = g_cloudPostRrComposite.load()
        ? 0.90f
        : (quality == 0u ? 0.72f : (quality == 2u ? 0.80f : 0.74f));
    params[12] = g_cloudWorldAnchorX;
    params[13] = g_cloudWorldAnchorZ;
    params[14] = g_cloudSize.load();
    params[15] = g_cloudDistribution.load();
    params[16] = g_cloudBaseFeather.load();
    // 2026-10-31：雾浪车道（cloudParams4.y/.z/.w）已无着色器消费者，
    // 但仍占 std140 位置，显式写 0 以保持"未使用 = 0"的契约。
    params[17] = 0.0f;
    params[18] = 0.0f;
    params[19] = 0.0f;
    params[20] = g_cloudType.load();
    params[21] = g_cloudErosionScale.load();
    params[22] = g_cloudSunlightStrength.load();
    params[23] = g_cloudPowderStrength.load();
    params[24] = g_cloudFarStepDensity.load();
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
//   [11]  雨天换介质总闸（g_fogRainMediumMix；旧 Rayleigh 通道已退役，当前不读）
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
    params[11] = g_fogRainMediumMix.load();
}

// 旧档贴地薄雾。写的是 godRayParams 的 [4..7]（调用方传 +4 的偏移）。
// 该雾当前已退役，着色器里没有消费者；保留写入只为布局/旧配置兼容。
void populateGroundMistParams(float* params)
{
    params[0] = g_fogGroundMistEnabled.load() ? 1.0f : 0.0f;
    params[1] = g_fogGroundMistHeight.load();
    params[2] = g_fogGroundMistExtinction.load();
    params[3] = g_fogGroundMistDistance.load();
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
void populateCloudLightParams(float* params)
{
    params[0] = g_cloudSourceOdScale.load();
    params[1] = g_cloudSourceCurvePow.load();
    params[2] = g_cloudAoUpwardScale.load();
    params[3] = g_cloudSunsetAmbientTint.load();
}

// 参考重构（2026-10-02）云底软化 + 两级步进阈值。4 个 lane 写到
// CameraUBO.cloudShapeParams（偏移 1104 尾块，四个消费者都声明）。
void populateCloudShapeParams(float* params)
{
    params[0] = g_cloudBottomSmoothHeight.load();
    params[1] = g_cloudBottomSmoothPow.load();
    params[2] = g_cloudTwoTierThreshold.load();
    params[3] = 0.0f;
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
void populateCloudLightParams2(float* params)
{
    params[0] = g_cloudSunPathReddening.load();
    params[1] = g_cloudAtmExtinction.load();
    params[2] = g_cloudPowderMode.load();
    params[3] = g_cloudPowderStrength.load();
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
    // 并且与 ReSTIR 的同一份照明双计。滑杆保留（读档/UI 不动），但不再生效。
    params[0] = (volRestirEnabled() != 0u)
        ? 0.0f
        : (g_cloudFogLightDescriptorsReady.load()
            ? g_fogDynamicLightStrength.load() : 0.0f);
    params[1] = g_fogGroundAnchorStrength.load();
    params[2] = g_fogSkyAmbientStrength.load();
    params[3] = g_fogTemporalJitterStrength.load();
}

// 2026-10-10：云时域 / 天光质量旋钮
//（CameraUBO.cloudTemporalParams，偏移 1392）。
// 2026-10-16：.w 从"保留写 0"升格为**底部遮蔽强度**（见上面 atomic 的注释）。
void populateCloudTemporalParams(float* params)
{
    params[0] = g_cloudSkyVisStrength.load();
    params[1] = g_cloudSkyVisSteps.load();
    params[2] = g_cloudTemporalDamp.load();
    params[3] = g_cloudBottomOcclusion.load();
}

// 2026-10-10 借鉴（CameraUBO.cloudAerialParams，偏移 1408）。
// #1 云后空气透视强度 / #2 地面大气透视强度 + 遮挡衰减。.w 保留写 0。
void populateCloudAerialParams(float* params)
{
    params[0] = g_cloudAerialStrength.load();
    params[1] = g_groundAerialStrength.load();
    params[2] = g_groundAerialOcclusion.load();
    params[3] = 0.0f;
}

// 2026-10-14 日落天光修正（CameraUBO.cloudSkyHueParams，偏移 1472）。
// x 天光冷化强度 / y 天光能量衰减强度。
// 2026-10-17：z 方向性天光强度 / w 顶底性质统一强度（原为保留，写 0）。
void populateCloudSkyHueParams(float* params)
{
    params[0] = g_cloudSkyHueCool.load();
    params[1] = g_cloudSkyHueEnergy.load();
    params[2] = g_cloudSkyDirStrength.load();
    params[3] = g_cloudAmbientUnify.load();
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
    params[4] = g_waterStyleWaveSpeed.load();
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
