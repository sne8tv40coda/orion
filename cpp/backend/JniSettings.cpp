// Orion backend — JniSettings 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setColorGradeParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    const jsize laneCount = env->GetArrayLength(params);
    if(laneCount != 24) return JNI_FALSE;

    jfloat values[24] = {};
    env->GetFloatArrayRegion(params, 0, 24, values);
    for(int i = 0; i < 24; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }

    g_cgExposureEV.store(std::min(std::max(static_cast<float>(values[0]), -4.00f), 4.00f));
    g_cgBlackPoint.store(std::min(std::max(static_cast<float>(values[1]), 0.00f), 0.30f));
    g_cgWhitePoint.store(std::min(std::max(static_cast<float>(values[2]), 0.70f), 1.50f));
    g_cgContrast.store(std::min(std::max(static_cast<float>(values[3]), 0.50f), 2.00f));
    g_cgPivot.store(std::min(std::max(static_cast<float>(values[4]), 0.05f), 0.95f));
    g_cgLiftR.store(std::min(std::max(static_cast<float>(values[5]), -0.15f), 0.15f));
    g_cgLiftG.store(std::min(std::max(static_cast<float>(values[6]), -0.15f), 0.15f));
    g_cgLiftB.store(std::min(std::max(static_cast<float>(values[7]), -0.15f), 0.15f));
    g_cgGammaR.store(std::min(std::max(static_cast<float>(values[8]), 0.50f), 2.00f));
    g_cgGammaG.store(std::min(std::max(static_cast<float>(values[9]), 0.50f), 2.00f));
    g_cgGammaB.store(std::min(std::max(static_cast<float>(values[10]), 0.50f), 2.00f));
    g_cgGainR.store(std::min(std::max(static_cast<float>(values[11]), 0.50f), 2.00f));
    g_cgGainG.store(std::min(std::max(static_cast<float>(values[12]), 0.50f), 2.00f));
    g_cgGainB.store(std::min(std::max(static_cast<float>(values[13]), 0.50f), 2.00f));
    g_cgShadowHue.store(std::min(std::max(static_cast<float>(values[14]), 0.00f), 360.00f));
    g_cgShadowStrength.store(std::min(std::max(static_cast<float>(values[15]), 0.00f), 1.00f));
    g_cgHighlightHue.store(std::min(std::max(static_cast<float>(values[16]), 0.00f), 360.00f));
    g_cgHighlightStrength.store(std::min(std::max(static_cast<float>(values[17]), 0.00f), 1.00f));
    g_cgVibrance.store(std::min(std::max(static_cast<float>(values[18]), -1.00f), 1.00f));
    g_cgFade.store(std::min(std::max(static_cast<float>(values[19]), 0.00f), 1.00f));
    g_cgVignette.store(std::min(std::max(static_cast<float>(values[20]), 0.00f), 1.00f));
    g_cgVignetteRadius.store(std::min(std::max(static_cast<float>(values[21]), 0.10f), 1.20f));
    g_cgHueRotate.store(std::min(std::max(static_cast<float>(values[22]), -180.00f), 180.00f));
    g_cgTintGreen.store(std::min(std::max(static_cast<float>(values[23]), -1.00f), 1.00f));
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setRrGuideSettings(
    JNIEnv*,
    jclass,
    jboolean relaxEnabled,
    jfloat farCloudDistance)
{
    const float requested = static_cast<float>(farCloudDistance);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 80000.0f)
    {
        return JNI_FALSE;
    }
    g_rrGuideRelaxEnabled.store(relaxEnabled == JNI_TRUE);
    g_rrFarCloudDistance.store(requested);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getRrFarCloudDistance(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(g_rrFarCloudDistance.load());
}
extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setNoDirectLightAmbient(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested =
        static_cast<float>(
            value
        );


    if(
        !std::isfinite(requested) ||
        requested < 0.0f ||
        requested > 1.0f
    )
    {
        fprintf(
            stderr,
            "[RTX AMBIENT] Invalid value=%.4f\n",
            static_cast<double>(
                requested
            )
        );

        return JNI_FALSE;
    }


    g_noDirectLightAmbient.store(
        requested
    );


    fprintf(
        stderr,
        "[RTX AMBIENT] value=%.2f\n",
        static_cast<double>(
            requested
        )
    );

    fflush(stderr);


    return JNI_TRUE;
}


extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getNoDirectLightAmbient(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(
        g_noDirectLightAmbient.load()
    );
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setPOMDepth(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested =
        static_cast<float>(
            value
        );


    if(
        !std::isfinite(requested) ||
        requested < 0.0f ||
        requested > 0.20f
    )
    {
        fprintf(
            stderr,
            "[RTX POM] Invalid depth=%.4f\n",
            static_cast<double>(
                requested
            )
        );

        return JNI_FALSE;
    }


    const float snapped =
        std::round(
            requested * 100.0f
        ) / 100.0f;


    g_pomDepth.store(
        snapped
    );


    fprintf(
        stderr,
        "[RTX POM] depth=%.2f\n",
        static_cast<double>(
            snapped
        )
    );

    fflush(stderr);


    return JNI_TRUE;
}


extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getPOMDepth(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(
        g_pomDepth.load()
    );
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setPOMSharp(
    JNIEnv*,
    jclass,
    jboolean sharp)
{
    g_pomSharp.store(
        sharp != JNI_FALSE
            ? 1.0f
            : 0.0f
    );


    fprintf(
        stderr,
        "[RTX POM] sampling=%s\n",
        sharp != JNI_FALSE
            ? "sharp"
            : "smooth"
    );

    fflush(stderr);


    return JNI_TRUE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_getPOMSharp(
    JNIEnv*,
    jclass)
{
    return g_pomSharp.load() > 0.5f
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setGiNearRate(
    JNIEnv*,
    jclass,
    jint level)
{
    if(level < 0 || level > 2)
    {
        return JNI_FALSE;
    }

    g_giNearRateOverride.store(
        static_cast<int>(level),
        std::memory_order_relaxed
    );

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getGiNearRate(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(giNearRateLevel());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setGlossyRoughLod(
    JNIEnv*,
    jclass,
    jboolean enabled)
{
    g_glossyRoughLodOverride.store(
        enabled != JNI_FALSE ? 1 : 0,
        std::memory_order_relaxed
    );

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_getGlossyRoughLod(
    JNIEnv*,
    jclass)
{
    return glossyRoughLodEnabled()
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setDirectLightMode(
    JNIEnv*,
    jclass,
    jint mode)
{
    if(mode < 0 || mode > 1)
    {
        return JNI_FALSE;
    }

    g_directLightModeOverride.store(
        static_cast<int>(mode),
        std::memory_order_relaxed
    );

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getDirectLightMode(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(directLightMode());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setIndirectMode(
    JNIEnv*,
    jclass,
    jint mode)
{
    if(mode < 0 || mode > 1)
    {
        return JNI_FALSE;
    }

    g_indirectModeOverride.store(
        static_cast<int>(mode),
        std::memory_order_relaxed
    );

    RTLog(
        std::string("[INDIRECT MODE] requested=") +
        (mode == 1 ? "SHaRC" : "GI") +
        " sharcEnabledNow=" + (sharcEnabled() ? "1" : "0") +
        " (SHaRC 的缓冲/管线在 init 创建 ⇒ 生效需重启客户端)"
    );

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getIndirectMode(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(
        g_indirectModeOverride.load(std::memory_order_relaxed));
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setSparseRatePercent(
    JNIEnv*,
    jclass,
    jint percent)
{
    if(percent < 0 || percent > 100)
    {
        return JNI_FALSE;
    }

    g_sparseRatePercentOverride.store(
        static_cast<int>(percent),
        std::memory_order_relaxed
    );

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getSparseRatePercent(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(sparseRatePercent());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setRtdiReuseInactive(
    JNIEnv*,
    jclass,
    jboolean enabled)
{
    g_rtxdiReuseInactiveOverride.store(
        enabled != JNI_FALSE ? 1 : 0,
        std::memory_order_relaxed
    );

    return JNI_TRUE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setUeCloudParam(
    JNIEnv*,
    jclass,
    jint index,
    jfloat value)
{
    if(index < 0 || index >= UE_CLOUD_PARAM_COUNT) return JNI_FALSE;
    if(!std::isfinite(static_cast<double>(value))) return JNI_FALSE;

    g_ueCloudParams[index].store(value);

    fprintf(
        stderr,
        "[RTX CLOUD UE] param[%d] = %.4f\n",
        static_cast<int>(index),
        static_cast<double>(value));
    fflush(stderr);

    return JNI_TRUE;
}


extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getUeCloudParam(
    JNIEnv*,
    jclass,
    jint index)
{
    if(index < 0 || index >= UE_CLOUD_PARAM_COUNT) return -1.0f;

    return static_cast<jfloat>(g_ueCloudParams[index].load());
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setSSSMode(
    JNIEnv*,
    jclass,
    jboolean trueSubsurface)
{
    g_sssMode.store(
        trueSubsurface != JNI_FALSE
            ? 1.0f
            : 0.0f
    );


    fprintf(
        stderr,
        "[RTX SSS] mode=%s\n",
        trueSubsurface != JNI_FALSE
            ? "true"
            : "sun-only"
    );

    fflush(stderr);


    return JNI_TRUE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_getSSSMode(
    JNIEnv*,
    jclass)
{
    return g_sssMode.load() > 0.5f
        ? JNI_TRUE
        : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setLocalLightMaxDistance(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested =
        static_cast<float>(
            value
        );


    if(
        !std::isfinite(requested) ||
        requested < 0.25f ||
        requested > 2.0f
    )
    {
        fprintf(
            stderr,
            "[RTX LOCAL LIGHT] Invalid range scale=%.3f\n",
            static_cast<double>(
                requested
            )
        );

        return JNI_FALSE;
    }


    const float snapped =
        std::round(
            requested * 100.0f
        ) / 100.0f;

    g_localLightMaxDistance.store(
        snapped
    );

    fprintf(
        stderr,
        "[RTX LOCAL LIGHT] range scale=%.2f\n",
        static_cast<double>(
            snapped
        )
    );

    fflush(stderr);

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getLocalLightMaxDistance(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(
        g_localLightMaxDistance.load()
    );
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setLocalLightIntensity(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested =
        static_cast<float>(
            value
        );


    if(
        !std::isfinite(requested) ||
        requested < 0.0f ||
        requested > 4.0f
    )
    {
        fprintf(
            stderr,
            "[RTX LOCAL LIGHT] Invalid intensity=%.3f\n",
            static_cast<double>(
                requested
            )
        );

        return JNI_FALSE;
    }


    const float snapped =
        std::round(
            requested * 100.0f
        ) / 100.0f;

    g_localLightIntensity.store(
        snapped
    );

    fprintf(
        stderr,
        "[RTX LOCAL LIGHT] intensity=%.2f\n",
        static_cast<double>(
            snapped
        )
    );

    fflush(stderr);

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getLocalLightIntensity(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(
        g_localLightIntensity.load()
    );
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setSunVisualSettings(
    JNIEnv*,
    jclass,
    jfloat diskLuminance,
    jfloat haloStrength,
    jfloat angularDiameterDegrees)
{
    const float requestedDiskLuminance = static_cast<float>(diskLuminance);
    const float requestedHaloStrength = static_cast<float>(haloStrength);
    const float requestedAngularDiameterDegrees = static_cast<float>(angularDiameterDegrees);
    if(!std::isfinite(requestedDiskLuminance) || !std::isfinite(requestedHaloStrength) ||
       !std::isfinite(requestedAngularDiameterDegrees) ||
       requestedDiskLuminance < 0.0f || requestedDiskLuminance > 8.0f ||
       requestedHaloStrength < 0.0f || requestedHaloStrength > 1.0f ||
       requestedAngularDiameterDegrees < 0.10f || requestedAngularDiameterDegrees > 2.00f)
    {
        return JNI_FALSE;
    }
    g_sunDiskLuminance.store(requestedDiskLuminance);
    g_sunHaloStrength.store(requestedHaloStrength);
    g_sunAngularDiameterDegrees.store(requestedAngularDiameterDegrees);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setAtmosphereSettings(
    JNIEnv*,
    jclass,
    jfloat starIntensity,
    jfloat sunTintStrength,
    jboolean physicalSkyEnabled,
    jfloat scatterIntensity,
    jfloat ozoneStrength)
{
    const float requestedStarIntensity = static_cast<float>(starIntensity);
    const float requestedSunTintStrength = static_cast<float>(sunTintStrength);
    const float requestedScatterIntensity = static_cast<float>(scatterIntensity);
    const float requestedOzoneStrength = static_cast<float>(ozoneStrength);
    if(!std::isfinite(requestedStarIntensity) || !std::isfinite(requestedSunTintStrength) ||
       !std::isfinite(requestedScatterIntensity) || !std::isfinite(requestedOzoneStrength) ||
       requestedStarIntensity < 0.0f || requestedStarIntensity > 2.0f ||
       requestedSunTintStrength < 0.0f || requestedSunTintStrength > 1.0f ||
       requestedScatterIntensity < 0.0f || requestedScatterIntensity > 2.0f ||
       requestedOzoneStrength < 0.0f || requestedOzoneStrength > 2.0f)
    {
        return JNI_FALSE;
    }
    g_starIntensity.store(requestedStarIntensity);
    g_sunTintStrength.store(requestedSunTintStrength);
    g_skyScatterEnabled.store(physicalSkyEnabled == JNI_TRUE);
    g_skyScatterIntensity.store(requestedScatterIntensity);
    g_ozoneStrength.store(requestedOzoneStrength);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setPowderStrength(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 3.0f)
    {
        return JNI_FALSE;
    }
    g_cloudPowderStrength.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

// 2026-11-01 天空散射模型切换（设置界面「大气散射模型」+「Hillaire 亮度标定」）。
// model: 0 = Bruneton 预计算 LUT，1 = Hillaire 结构（逐像素 march）
// scale: Hillaire 分支的辐亮度标定倍率 0.05..8（1 = 出厂）
// 生效时机：下一帧起（UBO 每帧重写）；天空 LUT 每帧重算 ⇒ 不需要重建管线。
extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setSkyModelSettings(
    JNIEnv*,
    jclass,
    jint model,
    jfloat scale,
    jfloat nightAmbient,
    jfloat rainDim)
{
    const int requestedModel = static_cast<int>(model);
    const float requestedScale = static_cast<float>(scale);
    const float requestedNightAmbient = static_cast<float>(nightAmbient);
    const float requestedRainDim = static_cast<float>(rainDim);

    if(requestedModel < 0 || requestedModel > 1)
    {
        return JNI_FALSE;
    }

    if(!std::isfinite(requestedScale) || requestedScale < 0.05f || requestedScale > 8.0f)
    {
        return JNI_FALSE;
    }

    if(!std::isfinite(requestedNightAmbient) ||
       requestedNightAmbient < 0.0f || requestedNightAmbient > 2.0f ||
       !std::isfinite(requestedRainDim) ||
       requestedRainDim < 0.0f || requestedRainDim > 2.0f)
    {
        return JNI_FALSE;
    }

    g_skyModel.store(requestedModel);
    g_skyModelScale.store(requestedScale);
    g_skyNightAmbient.store(requestedNightAmbient);
    g_skyRainDim.store(requestedRainDim);

    fprintf(
        stderr,
        "[SKY MODEL] java model=%d scale=%.3f night=%.2f rainDim=%.2f\n",
        requestedModel,
        requestedScale,
        requestedNightAmbient,
        requestedRainDim);
    fflush(stderr);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setAuroraStrength(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 1.0f)
    {
        return JNI_FALSE;
    }
    static std::atomic<bool> auroraWasOn{false};
    const bool nowOn = requested > 0.01f;
    if(nowOn != auroraWasOn.exchange(nowOn))
    {
        fprintf(
            stderr,
            "[RTX-AURORA] native strength=%.3f (Java -> JNI ok)\n",
            requested);
        fflush(stderr);
    }
    g_auroraStrength.store(requested);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setAuroraBrightness(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 3.0f)
    {
        return JNI_FALSE;
    }
    g_auroraBrightness.store(requested);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setAuroraFlowSpeed(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 4.0f)
    {
        return JNI_FALSE;
    }
    g_auroraFlowSpeed.store(requested);
    return JNI_TRUE;
}

// ============================================================================
// 2026-11-02 屏幕空间降级链（SSR + SSPT）
//
// 两个 setter 都只写 atomic（生效时机 = 下一帧的 UBO 重写），不重建管线、
// 不作废历史 —— 唯一的例外是下面 setSSRRange 把 1..11 钳到 12，这是用户
// 2026-11-02 的硬性约束：SSR 只在 > 12 区块的镜面命中上出现。
// ============================================================================
extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setSSPTRange(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 12.0f)
    {
        return JNI_FALSE;
    }

    // 取整到区块（滑条步长 1）
    g_ssptChunks.store(std::round(requested));
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setSSRRange(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 64.0f)
    {
        return JNI_FALSE;
    }

    float chunks = std::round(requested);

    // 硬性约束：SSR 只在 > 12 区块的镜面命中上工作 ⇒ 1..11 一律抬到 12
    if(chunks > 0.5f && chunks < 12.0f)
    {
        chunks = 12.0f;
    }

    g_ssrChunks.store(chunks);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudSunForwardScattering(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 1.0f)
    {
        return JNI_FALSE;
    }
    g_cloudSunForwardScattering.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudSunReddening(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 160.0f)
    {
        return JNI_FALSE;
    }
    g_cloudSunReddening.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudTopStretch(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 32.0f)
    {
        return JNI_FALSE;
    }
    g_cloudTopStretch.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudResFloorPercent(
    JNIEnv*,
    jclass,
    jint percent)
{
    const int requested = static_cast<int>(percent);
    if(requested < 0 || requested > 100)
    {
        return JNI_FALSE;
    }
    g_cloudResFloorPercent.store(static_cast<uint32_t>(requested));
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getCloudResFloorPercent(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(g_cloudResFloorPercent.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setGiRoulette(
    JNIEnv*,
    jclass,
    jint enabled)
{
    g_giRoulette.store(enabled == 0 ? 0u : 1u);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getGiRoulette(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(g_giRoulette.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setMotionDebugView(
    JNIEnv*,
    jclass,
    jint mode)
{
    g_motionDebugView.store(
        (mode >= 0 && mode <= 2)
            ? static_cast<uint32_t>(mode)
            : 0u
    );

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getMotionDebugView(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(g_motionDebugView.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setMotionVectorsEnabled(
    JNIEnv*,
    jclass,
    jint enabled)
{
    g_motionVectorsEnabled.store(enabled == 0 ? 0u : 1u);

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getMotionVectorsEnabled(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(g_motionVectorsEnabled.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setSunMarchFarDistance(
    JNIEnv*,
    jclass,
    jint blocks)
{
    const int requested = static_cast<int>(blocks);
    if(requested < 0 || requested > 256)
    {
        return JNI_FALSE;
    }
    g_sunMarchFarDistance.store(static_cast<uint32_t>(requested));
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getSunMarchFarDistance(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(g_sunMarchFarDistance.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudFarStepDensity(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 1.0f)
    {
        return JNI_FALSE;
    }
    g_cloudFarStepDensity.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getCloudFarStepDensity(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(g_cloudFarStepDensity.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudType(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 1.0f)
    {
        return JNI_FALSE;
    }
    g_cloudType.store(std::abs(requested - 0.35f) < 0.005f ? 0.8f : requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getCloudType(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(g_cloudType.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudErosionScale(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 2.0f)
    {
        return JNI_FALSE;
    }
    g_cloudErosionScale.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getCloudErosionScale(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(g_cloudErosionScale.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudWeatherScale(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 0.99f)
    {
        return JNI_FALSE;
    }
    g_cloudWeatherScale.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getCloudWeatherScale(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(g_cloudWeatherScale.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudSilver(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.0f || requested > 1.0f)
    {
        return JNI_FALSE;
    }
    g_cloudSilver.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getCloudSilver(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(g_cloudSilver.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudViewDistance(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 2000.0f || requested > 200000.0f)
    {
        return JNI_FALSE;
    }
    g_cloudViewDistance.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getCloudViewDistance(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(g_cloudViewDistance.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudDetileStrength(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested = static_cast<float>(value);
    if(!std::isfinite(requested) || requested < 0.1f || requested > 12.0f)
    {
        return JNI_FALSE;
    }
    g_cloudDetileStrength.store(requested);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudMarchParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudAdaptiveLightRange.store(std::max(0.0f, values[0]));
    g_cloudTwoTierStep.store(values[1] >= 0.5f ? 1.0f : 0.0f);
    g_cloudFarFadeDistance.store(std::max(0.0f, values[2]));
    g_cloudMeanDepthReproject.store(values[3] >= 0.5f ? 1.0f : 0.0f);

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudFadeFollowsRrMode(
    JNIEnv*,
    jclass,
    jboolean enabled)
{
    g_cloudFadeFollowsRrMode.store(enabled == JNI_TRUE);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_getCloudFadeFollowsRrMode(
    JNIEnv*,
    jclass)
{
    return g_cloudFadeFollowsRrMode.load() ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudPostRrComposite(
    JNIEnv*,
    jclass,
    jboolean enabled)
{
    g_cloudPostRrComposite.store(enabled == JNI_TRUE);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_getCloudPostRrComposite(
    JNIEnv*,
    jclass)
{
    return g_cloudPostRrComposite.load() ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setWaterParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_waterCrestBacklit.store(std::max(0.0f, values[0]));
    g_waterCausticDispersion.store(std::max(0.0f, values[1]));
    g_waterCausticDepthDiffusion.store(std::max(0.0f, values[2]));
    g_waterPhysicalPhase.store(values[3] >= 0.5f ? 1.0f : 0.0f);
    return JNI_TRUE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setPlaneCloudParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 9) return JNI_FALSE;
    jfloat values[9];
    env->GetFloatArrayRegion(params, 0, 9, values);
    for(int i = 0; i < 9; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_planeCloudEnabled.store(values[0] >= 0.5f ? 1.0f : 0.0f);
    g_planeCloudHeightOffset.store(values[1]);
    g_planeCloudScale.store(std::max(values[2], 0.01f));
    g_planeCloudSpeed.store(std::max(values[3], 0.0f));
    g_planeCloudDensity.store(std::max(values[4], 0.0f));
    g_planeCloudCoverage.store(std::min(std::max(values[5], 0.0f), 1.0f));
    g_planeCloudOctaves.store(std::min(std::max(values[6], 1.0f), 8.0f));
    g_planeCloudFadeDistance.store(std::max(values[7], 1.0f));
    g_planeCloudBrightness.store(std::max(values[8], 0.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudMsParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudPhiIntensity.store(std::max(0.0f, values[0]));
    g_cloudPhiDepthPow.store(std::max(0.0f, values[1]));
    g_cloudPhiDepthBias.store(values[2]);
    g_cloudMsTailStrength.store(std::max(0.0f, values[3]));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudPhiParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudPhiBoundaryConfidence.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 1.0f));
    g_cloudPhiMsBuildScale.store(values[1]);
    g_cloudPhiCompress.store(values[2]);
    g_cloudPhiConeRatio.store(values[3]);

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudLightParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudSourceOdScale.store(values[0]);
    g_cloudSourceCurvePow.store(values[1]);
    g_cloudAoUpwardScale.store(values[2]);
    g_cloudSunsetAmbientTint.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudHpParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 28) return JNI_FALSE;
    jfloat values[28];
    env->GetFloatArrayRegion(params, 0, 28, values);
    for(int i = 0; i < 28; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    auto sat = [](float v) { return std::min(std::max(v, 0.0f), 1.0f); };
    g_hpDetailStrengthCu.store(std::max(values[0], 0.0f));
    g_hpDetailStrengthTcu.store(std::max(values[1], 0.0f));
    g_hpDetailStrengthCb.store(std::max(values[2], 0.0f));
    g_hpBottomSmoothHeight.store(std::max(values[3], 0.0f));
    g_hpWispyEdgeWidth.store(std::max(values[4], 0.0f));
    g_hpWispyReach.store(std::max(values[5], 0.0f));
    g_hpWispyTopHeight.store(sat(values[6]));
    g_hpWispyTopHardness.store(std::max(values[7], 0.0f));
    g_hpWispyWeight.store(std::max(values[8], 0.0f));
    g_hpBillowyWeight.store(std::max(values[9], 0.0f));
    g_hpLoCoverTopMax.store(std::max(values[10], 1.0f));
    g_hpLoCoverTopStr.store(sat(values[11]));
    g_hpCovMapScale.store(std::max(values[12], 0.01f));
    g_hpScWorleyStrength.store(sat(values[13]));
    g_hpScHeightScale.store(std::max(values[14], 0.01f));
    g_hpScCellThickStr.store(sat(values[15]));
    g_hpForwardEcc.store(sat(values[16]));
    g_hpBackwardEcc.store(sat(values[17]));
    g_hpPhiFwdIntensity.store(std::max(values[18], 0.0f));
    g_hpPhiFwdDepthPow.store(std::max(values[19], 0.0f));
    g_hpPhiFwdDepthBias.store(values[20]);
    g_hpPhiFwdCompress.store(std::max(values[21], 0.0f));
    g_hpScatterOdScale.store(std::max(values[22], 0.0f));
    g_hpScatterCurvePow.store(std::max(values[23], 0.0f));
    g_hpMsEccentricity.store(sat(values[24]));
    g_hpAoUpwardScale.store(std::max(values[25], 0.0f));
    g_hpFadeStart.store(std::min(std::max(values[26], 0.0f), 0.95f));
    g_hpPowderIntensity.store(sat(values[27]));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_getCloudHpParams(
    JNIEnv* env, jclass, jfloatArray out)
{
    if(out == nullptr || env->GetArrayLength(out) < 28) return JNI_FALSE;
    const float values[28] = {
        g_hpDetailStrengthCu.load(), g_hpDetailStrengthTcu.load(),
        g_hpDetailStrengthCb.load(), g_hpBottomSmoothHeight.load(),
        g_hpWispyEdgeWidth.load(), g_hpWispyReach.load(),
        g_hpWispyTopHeight.load(), g_hpWispyTopHardness.load(),
        g_hpWispyWeight.load(), g_hpBillowyWeight.load(),
        g_hpLoCoverTopMax.load(), g_hpLoCoverTopStr.load(),
        g_hpCovMapScale.load(), g_hpScWorleyStrength.load(),
        g_hpScHeightScale.load(), g_hpScCellThickStr.load(),
        g_hpForwardEcc.load(), g_hpBackwardEcc.load(),
        g_hpPhiFwdIntensity.load(), g_hpPhiFwdDepthPow.load(),
        g_hpPhiFwdDepthBias.load(), g_hpPhiFwdCompress.load(),
        g_hpScatterOdScale.load(), g_hpScatterCurvePow.load(),
        g_hpMsEccentricity.load(), g_hpAoUpwardScale.load(),
        g_hpFadeStart.load(), g_hpPowderIntensity.load()};
    env->SetFloatArrayRegion(out, 0, 28, values);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudShapeParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudBottomSmoothHeight.store(values[0]);
    g_cloudBottomSmoothPow.store(values[1]);
    g_cloudTwoTierThreshold.store(values[2]);

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudMoonParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudMoonStrength.store(values[0]);
    g_cloudMoonPhase.store(values[1]);

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudShape2Params(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudDetailScale.store(values[0]);
    g_cloudDetailContrast.store(values[1]);
    g_cloudProfileCurve.store(values[2]);

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudAmbientParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudAmbientTopMul.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 3.0f));
    g_cloudAmbientBottomMul.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 3.0f));
    g_cloudAmbientSunSide.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 2.0f));
    g_cloudAmbientHueFollow.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudSkyHueParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudSkyHueCool.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 1.0f));
    g_cloudSkyHueEnergy.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.0f));
    g_cloudSkyDirStrength.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 2.0f));
    g_cloudAmbientUnify.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudPhysicsParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudGroundAlbedo.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 0.8f));
    g_cloudGroundBounce.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.0f));
    g_cloudSunlightResidual.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 0.25f));
    g_cloudPhaseFloor.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudMs2Params(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudScatterTint.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 1.0f));
    g_cloudMsOctaves.store(
        std::min(std::max(static_cast<float>(values[1]), 1.0f), 3.0f));
    g_cloudMsAttenuation.store(std::max(0.0f, static_cast<float>(values[2])));
    g_cloudMsEccentricity.store(std::max(0.0f, static_cast<float>(values[3])));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudLight2Params(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudSunPathReddening.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 2.0f));
    g_cloudAtmExtinction.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.0f));
    g_cloudPowderMode.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudProfileParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudProfileType.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 1.0f));
    g_cloudProfileTypeSpread.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudDetailParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudWispyStrength.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 2.0f));
    g_cloudWispyEdgeWidth.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.0f));
    g_cloudWispyReach.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 1.0f));
    g_cloudWispyTopHeight.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setFogExtrasParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_fogDynamicLightStrength.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 4.0f));
    g_fogGroundAnchorStrength.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.0f));
    g_fogSkyAmbientStrength.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 1.0f));
    g_fogTemporalJitterStrength.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudTemporalParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudSkyVisStrength.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 1.0f));
    g_cloudSkyVisSteps.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 32.0f));
    g_cloudTemporalDamp.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 1.0f));
    g_cloudBottomOcclusion.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudAerialParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_cloudAerialStrength.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 2.0f));
    g_groundAerialStrength.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 2.0f));
    g_groundAerialOcclusion.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 1.0f));

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setCloudSettings(
    JNIEnv*,
    jclass,
    jboolean enabled,
    jfloat bottom,
    jfloat thickness,
    jfloat coverage,
    jfloat density,
    jfloat detailStrength,
    jfloat windSpeed,
    jfloat evolution,
    jint quality,
    jfloat size,
    jfloat distribution,
    jfloat baseFeather,
    jfloat sunlightStrength)
{
    const float requestedBottom = static_cast<float>(bottom);
    const float requestedThickness = static_cast<float>(thickness);
    const float requestedCoverage = static_cast<float>(coverage);
    const float requestedDensity = static_cast<float>(density);
    const float requestedDetailStrength = static_cast<float>(detailStrength);
    const float requestedWindSpeed = static_cast<float>(windSpeed);
    const float requestedEvolution = static_cast<float>(evolution);
    const int requestedQuality = static_cast<int>(quality);
    const float requestedSize = static_cast<float>(size);
    const float requestedDistribution = static_cast<float>(distribution);
    const float requestedBaseFeather = static_cast<float>(baseFeather);
    const float requestedSunlightStrength = static_cast<float>(sunlightStrength);
    if(!std::isfinite(requestedBottom) || !std::isfinite(requestedThickness) ||
       !std::isfinite(requestedCoverage) || !std::isfinite(requestedDensity) ||
       !std::isfinite(requestedDetailStrength) || !std::isfinite(requestedWindSpeed) ||
       !std::isfinite(requestedEvolution) || !std::isfinite(requestedSize) ||
       !std::isfinite(requestedDistribution) || !std::isfinite(requestedBaseFeather) ||
       !std::isfinite(requestedSunlightStrength) ||
       requestedBottom < 96.0f || requestedBottom > 8000.0f ||
       requestedThickness < 256.0f || requestedThickness > 16384.0f ||
       requestedCoverage < 0.10f || requestedCoverage > 0.90f ||
       requestedDensity < 0.25f || requestedDensity > 5.0f ||
       requestedDetailStrength < 0.0f || requestedDetailStrength > 0.50f ||
       requestedWindSpeed < 0.0f || requestedWindSpeed > 32.0f ||
       requestedEvolution < 0.0f || requestedEvolution > 2.0f ||
       requestedQuality < 0 || requestedQuality > 2 ||
       requestedSize < 0.50f || requestedSize > 3.0f ||
       requestedDistribution < 0.0f || requestedDistribution > 1.0f ||
       requestedBaseFeather < 0.03f || requestedBaseFeather > 0.30f ||
       requestedSunlightStrength < 0.0f || requestedSunlightStrength > 3.0f)
    {
        return JNI_FALSE;
    }
    g_cloudEnabled.store(enabled == JNI_TRUE);
    g_cloudBottom.store(requestedBottom);
    g_cloudThickness.store(requestedThickness);
    g_cloudCoverage.store(requestedCoverage);
    g_cloudDensity.store(requestedDensity);
    g_cloudDetailStrength.store(requestedDetailStrength);
    g_cloudWindSpeed.store(requestedWindSpeed);
    g_cloudEvolution.store(requestedEvolution);
    g_cloudQuality.store(static_cast<uint32_t>(requestedQuality));
    g_cloudSize.store(requestedSize);
    g_cloudDistribution.store(requestedDistribution);
    g_cloudBaseFeather.store(requestedBaseFeather);
    g_cloudSunlightStrength.store(requestedSunlightStrength);
    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setVolumetricFogSettings(
    JNIEnv*,
    jclass,
    jboolean enabled,
    jfloat density,
    jfloat height,
    jfloat scaleHeight,
    jfloat anisotropy,
    jfloat sunStrength,
    jfloat ambientStrength,
    jfloat maxDistance,
    jfloat startDistance,
    jint sunSteps,
    jfloat rainBoost)
{
    const float requestedDensity = static_cast<float>(density);
    const float requestedHeight = static_cast<float>(height);
    const float requestedScaleHeight = static_cast<float>(scaleHeight);
    const float requestedAnisotropy = static_cast<float>(anisotropy);
    const float requestedSunStrength = static_cast<float>(sunStrength);
    const float requestedAmbientStrength = static_cast<float>(ambientStrength);
    const float requestedMaxDistance = static_cast<float>(maxDistance);
    const float requestedStartDistance = static_cast<float>(startDistance);
    const int requestedSunSteps = static_cast<int>(sunSteps);
    const float requestedRainBoost = static_cast<float>(rainBoost);
    if(!std::isfinite(requestedDensity) || !std::isfinite(requestedHeight) ||
       !std::isfinite(requestedScaleHeight) ||
       !std::isfinite(requestedAnisotropy) ||
       !std::isfinite(requestedSunStrength) ||
       !std::isfinite(requestedAmbientStrength) ||
       !std::isfinite(requestedMaxDistance) ||
       !std::isfinite(requestedStartDistance) ||
       !std::isfinite(requestedRainBoost) ||
       requestedDensity < 0.0f || requestedDensity > 100.0f ||
       requestedHeight < -1024.0f || requestedHeight > 4096.0f ||
       requestedScaleHeight < -1024.0f || requestedScaleHeight > 4096.0f ||
       requestedAnisotropy < -0.95f || requestedAnisotropy > 0.95f ||
       requestedSunStrength < 0.0f || requestedSunStrength > 16.0f ||
       requestedAmbientStrength < 0.0f || requestedAmbientStrength > 16.0f ||
       requestedMaxDistance < 1.0f || requestedMaxDistance > 16384.0f ||
       requestedStartDistance < 0.0f || requestedStartDistance > 4096.0f ||
       requestedSunSteps < 1 || requestedSunSteps > 8 ||
       requestedRainBoost < 0.0f || requestedRainBoost > 64.0f)
    {
        return JNI_FALSE;
    }
    g_fogEnabled.store(enabled == JNI_TRUE);
    g_fogBaseDensity.store(requestedDensity);
    g_fogHeightBlock.store(requestedHeight);
    g_fogScaleHeight.store(requestedScaleHeight);
    g_fogAnisotropyG.store(requestedAnisotropy);
    g_fogSunStrength.store(requestedSunStrength);
    g_fogAmbientStrength.store(requestedAmbientStrength);
    g_fogMaxDistance.store(requestedMaxDistance);
    g_fogStartDistance.store(requestedStartDistance);
    g_fogSunSteps.store(static_cast<uint32_t>(requestedSunSteps));
    g_fogRainBoost.store(requestedRainBoost);

    g_volumetricCloudHistoryNeedsClear.store(true);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setRainWallSettings(
    JNIEnv*,
    jclass,
    jboolean enabled,
    jfloat wetness,
    jfloat dripDensity,
    jfloat dripSpeed,
    jfloat dripSize,
    jfloat dripBump,
    jfloat distance)
{
    const float requestedWetness = static_cast<float>(wetness);
    const float requestedDripDensity = static_cast<float>(dripDensity);
    const float requestedDripSpeed = static_cast<float>(dripSpeed);
    const float requestedDripSize = static_cast<float>(dripSize);
    const float requestedDripBump = static_cast<float>(dripBump);
    const float requestedDistance = static_cast<float>(distance);
    if(!std::isfinite(requestedWetness) || !std::isfinite(requestedDripDensity) ||
       !std::isfinite(requestedDripSpeed) || !std::isfinite(requestedDripSize) ||
       !std::isfinite(requestedDripBump) || !std::isfinite(requestedDistance) ||
       requestedWetness < 0.0f || requestedWetness > 2.0f ||
       requestedDripDensity < 0.0f || requestedDripDensity > 1.0f ||
       requestedDripSpeed < 0.1f || requestedDripSpeed > 4.0f ||
       requestedDripSize < 0.25f || requestedDripSize > 4.0f ||
       requestedDripBump < 0.0f || requestedDripBump > 2.0f ||
       requestedDistance < 16.0f || requestedDistance > 160.0f)
    {
        return JNI_FALSE;
    }
    g_rainWallEnabled.store(enabled == JNI_TRUE);
    g_rainWallWetness.store(requestedWetness);
    g_rainWallDripDensity.store(requestedDripDensity);
    g_rainWallDripSpeed.store(requestedDripSpeed);
    g_rainWallDripSize.store(requestedDripSize);
    g_rainWallDripBump.store(requestedDripBump);
    g_rainWallDistance.store(requestedDistance);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setGalaxySettings(
    JNIEnv*,
    jclass,
    jfloat scale,
    jfloat tiltDegrees,
    jfloat azimuthDegrees,
    jfloat mix)
{
    const float requestedScale = static_cast<float>(scale);
    const float requestedTilt = static_cast<float>(tiltDegrees);
    const float requestedAzimuth = static_cast<float>(azimuthDegrees);
    const float requestedMix = static_cast<float>(mix);
    if(!std::isfinite(requestedScale) || !std::isfinite(requestedTilt) ||
       !std::isfinite(requestedAzimuth) || !std::isfinite(requestedMix) ||
       requestedScale < 0.2f || requestedScale > 3.0f ||
       requestedTilt < 0.0f || requestedTilt > 90.0f ||
       requestedAzimuth < -180.0f || requestedAzimuth > 180.0f ||
       requestedMix < 0.0f || requestedMix > 1.0f)
    {
        return JNI_FALSE;
    }
    g_galaxyScale.store(requestedScale);
    g_galaxyTiltDegrees.store(requestedTilt);
    g_galaxyAzimuthDegrees.store(requestedAzimuth);
    g_galaxyMix.store(requestedMix);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setWaterStyleSettings(
    JNIEnv*,
    jclass,
    jfloat waveScale,
    jfloat tintMix,
    jfloat densityScale,
    jfloat causticStrength,
    jfloat waveSpeed,
    jfloat swellScale,
    jfloat midScale,
    jfloat fineScale)
{
    const float requestedWaveScale = static_cast<float>(waveScale);
    const float requestedTintMix = static_cast<float>(tintMix);
    const float requestedDensityScale = static_cast<float>(densityScale);
    const float requestedCausticStrength = static_cast<float>(causticStrength);
    const float requestedWaveSpeed = static_cast<float>(waveSpeed);
    const float requestedSwellScale = static_cast<float>(swellScale);
    const float requestedMidScale = static_cast<float>(midScale);
    const float requestedFineScale = static_cast<float>(fineScale);
    if(!std::isfinite(requestedWaveScale) || !std::isfinite(requestedTintMix) ||
       !std::isfinite(requestedDensityScale) || !std::isfinite(requestedCausticStrength) ||
       !std::isfinite(requestedWaveSpeed) ||
       !std::isfinite(requestedSwellScale) || !std::isfinite(requestedMidScale) ||
       !std::isfinite(requestedFineScale) ||
       requestedWaveScale < 0.0f || requestedWaveScale > 2.0f ||
       requestedTintMix < 0.0f || requestedTintMix > 1.0f ||
       requestedDensityScale < 0.25f || requestedDensityScale > 4.0f ||
       requestedCausticStrength < 0.0f || requestedCausticStrength > 4.0f ||
       requestedWaveSpeed < 0.0f || requestedWaveSpeed > 3.0f ||
       requestedSwellScale < 0.0f || requestedSwellScale > 2.0f ||
       requestedMidScale < 0.0f || requestedMidScale > 2.0f ||
       requestedFineScale < 0.0f || requestedFineScale > 2.0f)
    {
        return JNI_FALSE;
    }
    g_waterStyleWaveScale.store(requestedWaveScale);
    g_waterStyleTintMix.store(requestedTintMix);
    g_waterStyleDensityScale.store(requestedDensityScale);
    g_waterStyleCausticStrength.store(requestedCausticStrength);
    g_waterStyleWaveSpeed.store(requestedWaveSpeed);
    g_waterStyleSwellScale.store(requestedSwellScale);
    g_waterStyleMidScale.store(requestedMidScale);
    g_waterStyleFineScale.store(requestedFineScale);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setWaterWaveParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    const jsize length = env->GetArrayLength(params);
    const jsize readCount = std::min<jsize>(length, 5);
    jfloat values[5] = {0.0f, 0.0f, 0.0f, 0.0f, 4.0f};
    env->GetFloatArrayRegion(params, 0, readCount, values);
    for(jsize i = 0; i < readCount; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_waterWaveCalibration.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 1.0f));
    g_waterWaveStrength.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 2.0f));
    g_waterWaveBlend.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 1.0f));
    if(length >= 5)
    {
        g_waterReflectCausticStrength.store(
            std::min(std::max(static_cast<float>(values[4]), 0.0f), 8.0f));
    }
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setWaterSimWaveParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_waterSimSpongeWidth.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 0.45f));
    g_waterSimGaussianSource.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.0f));
    g_waterSimDampingScale.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 3.0f));
    g_waterSimPropagationSpeed.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 2.0f));
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setWaterSssParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_waterSssNonlinear.store(
        std::min(std::max(static_cast<float>(values[0]), 0.0f), 1.0f));
    g_waterSssBoost.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 4.0f));
    g_waterSssPhaseG.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 0.95f));
    g_waterSssStrength.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 2.0f));
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setWaterWaveShapeParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 4) return JNI_FALSE;
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, 4, values);
    for(int i = 0; i < 4; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_waterWaveLengthScale.store(
        std::min(std::max(static_cast<float>(values[0]), 0.35f), 25.0f));
    g_waterWaveDirection.store(static_cast<float>(values[1]));
    g_waterWaveCrestSharpness.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 2.0f));
    g_waterWaveWarpStrength.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 2.0f));
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setWaterWaveAflParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 3) return JNI_FALSE;
    const jsize count = std::min<jsize>(env->GetArrayLength(params), 4);
    jfloat values[4];
    env->GetFloatArrayRegion(params, 0, count, values);
    for(int i = 0; i < count; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_waterWaveFbmWavelength.store(
        std::min(std::max(static_cast<float>(values[0]), 1.0f), 8.0f));
    g_waterWaveFbmWarp.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.5f));
    g_waterWaveFbmOctaves.store(
        std::min(std::max(static_cast<float>(values[2]), 1.0f), 12.0f));
    if(count >= 4)
    {
        g_waterWaveFbmWaves.store(
            std::min(std::max(static_cast<float>(values[3]), 1.0f), 12.0f));
    }
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setVolumetricLightSettings(
    JNIEnv*,
    jclass,
    jboolean enabled,
    jint steps,
    jint occlusionSamples,
    jfloat intensity)
{
    const int requestedSteps = static_cast<int>(steps);
    const int requestedOcclusionSamples = static_cast<int>(occlusionSamples);
    const float requestedIntensity = static_cast<float>(intensity);
    if(requestedSteps < 4 || requestedSteps > 48 ||
       requestedOcclusionSamples < 1 || requestedOcclusionSamples > 6 ||
       !(requestedIntensity >= 0.0f && requestedIntensity <= 4.0f))
    {
        return JNI_FALSE;
    }
    g_godRayEnabled.store(enabled == JNI_TRUE);
    g_godRaySteps.store(requestedSteps);
    g_godRayOcclusionSamples.store(requestedOcclusionSamples);
    g_godRayIntensity.store(requestedIntensity);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setGodRayReprojection(
    JNIEnv*,
    jclass,
    jfloat weight)
{
    const float requested = static_cast<float>(weight);
    if(!(requested >= 0.0f && requested <= 0.95f))
    {
        return JNI_FALSE;
    }
    g_godRayHistoryWeight.store(requested);
    return JNI_TRUE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setRRMode(
    JNIEnv*,
    jclass,
    jint mode)
{
    const int requestedMode =
        static_cast<int>(mode);


    if(requestedMode < 0 ||
       requestedMode > 4)
    {
        fprintf(
            stderr,
            "[RR SETTINGS] Invalid mode=%d\n",
            requestedMode
        );

        return JNI_FALSE;
    }


    const int oldMode =
        g_rrMode.exchange(
            requestedMode
        );


    if(oldMode != requestedMode)
    {
        fprintf(
            stderr,
            "[RR SETTINGS] mode %d -> %d\n",
            oldMode,
            requestedMode
        );

        fflush(stderr);
    }


    return JNI_TRUE;
}


extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getRRMode(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(
        g_rrMode.load()
    );
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setUpscalerMode(
    JNIEnv*, jclass, jint mode)
{
    if(mode < 0 || mode > 1) return JNI_FALSE;
    g_upscalerMode.store(static_cast<int>(mode));
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setSRMode(
    JNIEnv*, jclass, jint mode)
{
    if(mode < 0 || mode > 4) return JNI_FALSE;
    g_srMode.store(static_cast<int>(mode));
    return JNI_TRUE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setPerformanceSettingsNative(
    JNIEnv*, jclass,
    jint aoNearSamples, jint aoMidSamples, jint giBounces, jint sunShadowSamples,
    jint localLightRisCandidates, jint localLightAreaSamples,
    jint waterReflectionInterval, jint glossyFullDistance, jint dynamicRtasInterval,
    jfloat rtSpp, jint volumetricRestir)
{
    if(aoNearSamples < 0 || aoNearSamples > 4 ||
       aoMidSamples < 0 || aoMidSamples > 2 ||
       giBounces < 0 || giBounces > 4 ||
       sunShadowSamples < 1 || sunShadowSamples > 2 ||
       localLightRisCandidates < 1 || localLightRisCandidates > 32 ||
       localLightAreaSamples < 1 || localLightAreaSamples > 2 ||
       waterReflectionInterval < 0 || waterReflectionInterval > 4 ||
       glossyFullDistance < 16 || glossyFullDistance > 96 ||
       dynamicRtasInterval < 1 || dynamicRtasInterval > 8 ||
       !std::isfinite(static_cast<float>(rtSpp)) ||
       rtSpp < 0.5f || rtSpp > 4.0f ||
       volumetricRestir < 0 || volumetricRestir > 1)
    {
        return JNI_FALSE;
    }
    g_perfAoNearSamples.store(static_cast<uint32_t>(aoNearSamples));
    g_perfAoMidSamples.store(static_cast<uint32_t>(aoMidSamples));
    g_perfGiBounces.store(static_cast<uint32_t>(giBounces));
    g_perfSunShadowSamples.store(static_cast<uint32_t>(sunShadowSamples));
    g_perfLocalLightRisCandidates.store(static_cast<uint32_t>(localLightRisCandidates));
    g_perfLocalLightAreaSamples.store(static_cast<uint32_t>(localLightAreaSamples));
    g_perfWaterReflectionInterval.store(static_cast<uint32_t>(waterReflectionInterval));
    g_perfGlossyFullDistance.store(static_cast<uint32_t>(glossyFullDistance));
    g_perfDynamicRtasInterval.store(static_cast<uint64_t>(dynamicRtasInterval));
    g_volRestirGuiOverride.store(
        volumetricRestir != 0 ? 1u : 0u, std::memory_order_relaxed);
    g_perfRtSpp.store(static_cast<float>(rtSpp));
    return JNI_TRUE;
}
extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_setSerEnabledNative(
    JNIEnv*,
    jclass,
    jboolean enabled
)
{
    g_serEnabled.store(enabled == JNI_TRUE);
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_setHDREnabledNative(
    JNIEnv*,
    jclass,
    jboolean enabled
)
{
    g_hdrRequested.store(enabled == JNI_TRUE);
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_setHDRSurfaceSupportedNative(
    JNIEnv*,
    jclass,
    jboolean supported
)
{
    g_hdrSurfaceSupported.store(supported == JNI_TRUE);
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_setAutoExposureEnabledNative(
    JNIEnv*,
    jclass,
    jboolean enabled
)
{
    g_aeEnabled.store(enabled == JNI_TRUE);


    fprintf(
        stderr,
        "[RR-7] auto exposure enabled=%d\n",
        enabled == JNI_TRUE ? 1 : 0
    );

    fflush(stderr);


    RTLog(
        std::string("[RR-7 SWITCH] auto exposure enabled=") +
        (enabled == JNI_TRUE ? "1" : "0")
    );
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setAutoExposureCompensationNative(
    JNIEnv*,
    jclass,
    jfloat value)
{
    const float requested =
        static_cast<float>(
            value
        );


    if(
        !std::isfinite(requested) ||
        requested < -2.0f ||
        requested > 2.0f
    )
    {
        fprintf(
            stderr,
            "[RR-7] Invalid exposure compensation=%.3f\n",
            static_cast<double>(
                requested
            )
        );

        {
            std::ostringstream rejectLog;

            rejectLog
                << "[RR-7 SWITCH] exposure compensation REJECTED="
                << requested;

            RTLog(rejectLog.str());
        }

        return JNI_FALSE;
    }


    const float snapped =
        std::round(
            requested * 10.0f
        ) / 10.0f;


    g_aeCompensationEV.store(
        snapped
    );


    fprintf(
        stderr,
        "[RR-7] exposure compensation=%+.1f EV keyEV=%.4f\n",
        static_cast<double>(
            snapped
        ),
        static_cast<double>(
            AE_DEFAULT_KEY_EV +
            snapped
        )
    );

    fflush(stderr);


    {
        std::ostringstream compensationLog;

        compensationLog
            << "[RR-7 SWITCH] exposure compensation="
            << snapped
            << " keyEV="
            << (AE_DEFAULT_KEY_EV + snapped);

        RTLog(compensationLog.str());
    }


    return JNI_TRUE;
}

extern "C"
JNIEXPORT jfloat JNICALL Java_com_example_vulkan_NativeVulkan_getAutoExposureCompensation(
    JNIEnv*,
    jclass)
{
    return static_cast<jfloat>(
        g_aeCompensationEV.load()
    );
}
