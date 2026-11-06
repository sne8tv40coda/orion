// Orion backend — Frame 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateCamera(
    JNIEnv* env,
    jclass clazz,
    jfloatArray viewInvArray,
    jfloatArray projInvArray,
    jint frameCount,
    jfloat cameraNear,
    jfloat cameraFar,
    jfloat cameraFov,
    jfloat overworldTimePhase,
    jfloat rainLevel,
    jfloat thunderLevel,
    jfloat dimensionCode,
    jfloat playerX,
    jfloat playerY,
    jfloat playerZ,
    jfloat playerVelocityX,
    jfloat playerVelocityY,
    jfloat playerVelocityZ,
    jfloat playerInWater,
    jfloat waterSurfaceY,
    jfloat cameraInWater)
{
    if (!g_cameraUboMapped) return;
    sharcProbeTick();

    sharcStatsTick();

#ifndef RTX_CAMERA_UBO_SERIALIZE
#define RTX_CAMERA_UBO_SERIALIZE 0
#endif
#if RTX_CAMERA_UBO_SERIALIZE
    if(g_device != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(g_device);
    }
#endif

    CameraUBO* ubo = static_cast<CameraUBO*>(g_cameraUboMapped);

    float nextViewInverse[16]{};
    float nextProjInverse[16]{};


    env->GetFloatArrayRegion(
        viewInvArray,
        0,
        16,
        nextViewInverse
    );

    env->GetFloatArrayRegion(
        projInvArray,
        0,
        16,
        nextProjInverse
    );


    const int32_t nextFrame =
        static_cast<int32_t>(frameCount);


    const bool cameraHistoryValid =
        g_cameraHistoryValid &&
        g_hasPrevCameraInverse;

    if(cameraHistoryValid)
    {
        std::memcpy(
            ubo->prevViewInverse,
            g_prevCameraViewInverse,
            sizeof(ubo->prevViewInverse)
        );

        std::memcpy(
            ubo->prevProjInverse,
            g_prevCameraProjInverse,
            sizeof(ubo->prevProjInverse)
        );


        GetDLSSFrameJitter(
            static_cast<uint32_t>(
                std::max(g_lastCameraFrame, 0)
            ),
            ubo->historyPrevJitterX,
            ubo->historyPrevJitterY
        );
    }
    else
    {
        std::memcpy(
            ubo->prevViewInverse,
            nextViewInverse,
            sizeof(ubo->prevViewInverse)
        );

        std::memcpy(
            ubo->prevProjInverse,
            nextProjInverse,
            sizeof(ubo->prevProjInverse)
        );


        GetDLSSFrameJitter(
            static_cast<uint32_t>(
                std::max(nextFrame, 0)
            ),
            ubo->historyPrevJitterX,
            ubo->historyPrevJitterY
        );
    }


    std::memcpy(
        ubo->viewInverse,
        nextViewInverse,
        sizeof(ubo->viewInverse)
    );

    std::memcpy(
        ubo->projInverse,
        nextProjInverse,
        sizeof(ubo->projInverse)
    );

    // ★★★ 2026-09-29：把"本帧收到的矩阵"存为快照，供下一次调用当 prev 使用。
    //     注意顺序：必须在 prevViewInverse / prevProjInverse 已经按快照写完**之后**，
    //     再由本帧值覆盖快照，否则 prev 会变成当帧值（正是要修的那个 bug）。
    std::memcpy(
        g_prevCameraViewInverse,
        nextViewInverse,
        sizeof(g_prevCameraViewInverse)
    );

    std::memcpy(
        g_prevCameraProjInverse,
        nextProjInverse,
        sizeof(g_prevCameraProjInverse)
    );

    g_hasPrevCameraInverse = true;

#ifndef RTX_CAMERA_PREV_MATRIX_PROBE
#define RTX_CAMERA_PREV_MATRIX_PROBE 1
#endif
#if RTX_CAMERA_PREV_MATRIX_PROBE
    {
        static int32_t probeLastFrameCount = -1;
        static int32_t probeLastLogFrame   = -1000000;
        static uint32_t probeCalls         = 0u;
        static uint32_t probeRepeatCalls   = 0u;
        static uint32_t probeRotFrames     = 0u;
        static uint32_t probeRotPrevEqCur  = 0u;

        probeCalls++;
        if(probeLastFrameCount == nextFrame) probeRepeatCalls++;

        const bool prevEqCurView =
            std::memcmp(
                ubo->prevViewInverse,
                ubo->viewInverse,
                sizeof(ubo->viewInverse)
            ) == 0;

        const bool prevEqCurProj =
            std::memcmp(
                ubo->prevProjInverse,
                ubo->projInverse,
                sizeof(ubo->projInverse)
            ) == 0;

        const float posDx = ubo->viewInverse[12] - ubo->prevViewInverse[12];
        const float posDy = ubo->viewInverse[13] - ubo->prevViewInverse[13];
        const float posDz = ubo->viewInverse[14] - ubo->prevViewInverse[14];
        const float posDelta = std::sqrt(posDx * posDx + posDy * posDy + posDz * posDz);

        const float curFx  = -ubo->viewInverse[8];
        const float curFy  = -ubo->viewInverse[9];
        const float curFz  = -ubo->viewInverse[10];
        const float prevFx = -ubo->prevViewInverse[8];
        const float prevFy = -ubo->prevViewInverse[9];
        const float prevFz = -ubo->prevViewInverse[10];

        float rotDot = curFx * prevFx + curFy * prevFy + curFz * prevFz;
        rotDot = std::max(-1.0f, std::min(1.0f, rotDot));

        const float rotDeg = std::acos(rotDot) * 57.29577951f;
        const bool rotating = rotDeg > 0.02f || posDelta > 1.0e-4f;

        if(rotating)
        {
            probeRotFrames++;
            if(prevEqCurView || prevEqCurProj) probeRotPrevEqCur++;
        }

        if(nextFrame - probeLastLogFrame >= 30 ||
           (rotating && probeRotFrames == 1u))
        {
            probeLastLogFrame = nextFrame;

            char probeBuf[512];
            std::snprintf(
                probeBuf,
                sizeof(probeBuf),
                "[RT CAMPROBE] frame=%d calls=%u repeats=%u historyValid=%d | "
                "prevEqCurView=%d prevEqCurProj=%d | rotDeg=%.4f posDelta=%.5f | "
                "mvZeroWhileRotating=%d | rotFrames=%u rotWithPrevEqCur=%u",
                nextFrame,
                probeCalls,
                probeRepeatCalls,
                cameraHistoryValid ? 1 : 0,
                prevEqCurView ? 1 : 0,
                prevEqCurProj ? 1 : 0,
                rotDeg,
                posDelta,
                (rotating && (prevEqCurView || prevEqCurProj)) ? 1 : 0,
                probeRotFrames,
                probeRotPrevEqCur
            );

            RTLog(std::string(probeBuf));
        }

        probeLastFrameCount = nextFrame;
    }
#endif

    ubo->vertexBufferAddrLo = static_cast<uint32_t>(g_testVertexDeviceAddress & 0xFFFFFFFF);
    ubo->vertexBufferAddrHi = static_cast<uint32_t>(g_testVertexDeviceAddress >> 32);

    ubo->frameCount =
        static_cast<uint32_t>(frameCount);

    const float waterWaveTimeSeconds =
        std::chrono::duration<float>(
            std::chrono::steady_clock::now() - g_waterWaveEpoch
        ).count();
    std::memcpy(
        &ubo->padding,
        &waterWaveTimeSeconds,
        sizeof(waterWaveTimeSeconds)
    );


    ubo->historyReadIndex =
        (static_cast<uint32_t>(frameCount) + 1u) & 1u;

    ubo->historyValid =
        cameraHistoryValid
            ? 1u
            : 0u;

    {
        const float giMaxDistance = giRayMaxDistanceEffective();
        uint32_t giMaxBits = 0u;
        std::memcpy(&giMaxBits, &giMaxDistance, sizeof(giMaxBits));
        ubo->historyPadding0 = giMaxBits;
        g_perfL10GiMaxForLog.store(giMaxDistance, std::memory_order_relaxed);
    }

    g_perfL10PomForLog.store(g_pomDepth.load(), std::memory_order_relaxed);
    const float localLightIntensityLane = g_localLightIntensity.load() + 1.0f;
    std::memcpy(
        &ubo->historyPadding1,
        &localLightIntensityLane,
        sizeof(localLightIntensityLane)
    );
    ubo->perfAoNearSamples = g_perfAoNearSamples.load();
    ubo->perfAoMidSamples = g_perfAoMidSamples.load();
    ubo->perfSerAb[0] = g_perfL10SerToggle;
    ubo->perfSerAb[1] = g_perfL10SerVisToggle;
    ubo->perfSerAb[2] = g_perfL10SerRadToggle;
    ubo->perfSerAb[3] = g_perfL10SerPrimToggle;
    ubo->dhFarFieldParams[0] = dhTexturesAvailable() ? 1.0f : 0.0f;
    ubo->dhFarFieldParams[1] = dhFarFieldMatrixAvailable() ? 1.0f : 0.0f;
    ubo->dhFarFieldParams[2] = g_dhFarFieldNear;
    ubo->dhFarFieldParams[3] = g_dhFarFieldFar;

    for(int dhMatrixIndex = 0; dhMatrixIndex < 16; ++dhMatrixIndex)
    {
        ubo->dhFarFieldInvMvp[dhMatrixIndex] = g_dhFarFieldInvMvp[dhMatrixIndex];
        ubo->dhFarFieldMvp[dhMatrixIndex] = g_dhFarFieldMvp[dhMatrixIndex];
    }

    ubo->dhFarFieldLight[0] = g_dhFarFieldMode;
    ubo->dhFarFieldLight[1] = g_dhFarFieldStrength;
    ubo->dhFarFieldLight[2] = g_dhFarFieldGain;
    ubo->dhFarFieldLight[3] = g_dhFarFieldVariant;

    const float farShadowExtent = sunShadowFarExtent();
    ubo->sunShadowFarParams[0] = farShadowExtent;
    ubo->sunShadowFarParams[1] = farShadowExtent;
    ubo->sunShadowFarParams[2] = 1.0f / static_cast<float>(SUN_SHADOW_FAR_RESOLUTION);
    ubo->sunShadowFarParams[3] =
        (g_sunShadowFarAvailable.load() && sunShadowFarEnabled()) ? 1.0f : 0.0f;

    ubo->perfGiBounces = g_perfGiBounces.load();
    ubo->perfSunShadowSamples = g_perfSunShadowSamples.load();
    ubo->perfLocalLightRisCandidates = g_perfLocalLightRisCandidates.load();
    ubo->perfLocalLightAreaSamples = g_perfLocalLightAreaSamples.load();
    ubo->perfWaterReflectionInterval = g_perfWaterReflectionInterval.load();
    ubo->perfGlossyFullDistance = g_perfGlossyFullDistance.load();

    ubo->cameraNear =
        static_cast<float>(cameraNear);

    ubo->cameraFar =
        static_cast<float>(cameraFar);

    ubo->cameraFov =
        static_cast<float>(cameraFov);

    {
        static const float ambientFloor = []() -> float
        {
            const char* raw = std::getenv("ORION_AMBIENT_FLOOR");

            if(raw != nullptr && raw[0] != '\0')
            {
                const float requested = static_cast<float>(std::atof(raw));
                return requested > 0.0f ? requested : 0.0f;
            }

            return 0.0f;
        }();

        const float requestedAmbient = g_noDirectLightAmbient.load();

        ubo->cameraParamsPadding =
            requestedAmbient > ambientFloor ? requestedAmbient : ambientFloor;
    }

    ubo->historyParamsPadding0 =
        g_pomDepth.load();

    ubo->pomParams[0] =
        g_pomSharp.load();

    ubo->sssParams[0] =
        g_sssMode.load();

    ubo->giHalfParams[0] =
        (giHalfEnabled() && g_giHalfView != VK_NULL_HANDLE) ? 1.0f : 0.0f;
    ubo->giHalfParams[1] =
        (giHalfEnabled() && giHalfRotateEnabled()) ? 1.0f : 0.0f;
    ubo->giHalfParams[2] = giNoPomEnabled() ? 1.0f : 0.0f;
    ubo->giHalfParams[3] = glossyRoughLodEnabled() ? 1.0f : 0.0f;

    // 2026-11-02 屏幕空间降级链。x = SSPT 射程（0..12 区块，0 = 关），
    // y = SSR 射程上限（0 = 关；硬性下限 12 由 ss_hybrid.comp 逐像素判）。
    ubo->ssHybridParams[0] = std::clamp(g_ssptChunks.load(), 0.0f, 12.0f);
    ubo->ssHybridParams[1] = std::clamp(g_ssrChunks.load(), 0.0f, 64.0f);
    // z = RT 光泽射线开关（1 = 开）。rgen 读到 < 0.5 就整段跳过那条 GGX
    // 反射射线，镜面交给 SSR/SSPT —— 见 rt_test.rgen 的对应门控。
    ubo->ssHybridParams[2] = g_ssRtGlossy.load() > 0.5f ? 1.0f : 0.0f;
    ubo->ssHybridParams[3] = 0.0f;

    ubo->giRayParams[0] = static_cast<float>(giNearRateLevel());
    // 2026-11-04 A1：y = GI 时域重投影开关（rgen_gi_temporal.glsl 读 >0.5）。
    // y/z/w 此前恒 0，借 y 不动 UBO 布局；ORION_GI_TEMPORAL=0 回旧路径。
    ubo->giRayParams[1] = giTemporalEnabled() ? 1.0f : 0.0f;
    ubo->giRayParams[2] = 0.0f;
    ubo->giRayParams[3] = 0.0f;

    populateHpParams(ubo);

    const int directLightModeCurrent = directLightMode();

    static int previousDirectLightMode = directLightModeCurrent;

    if(directLightModeCurrent != previousDirectLightMode)
    {
        previousDirectLightMode = directLightModeCurrent;
        g_sparseRateHistoryReset.store(true, std::memory_order_relaxed);
    }

    ubo->sparseParams[0] = static_cast<float>(directLightModeCurrent);
    const float sparseRateForMask = sparseSamplingRateEffective();

    g_sparseEffectiveRate.store(sparseRateForMask, std::memory_order_relaxed);
    g_sparseEffectiveFrameIndex.store(
        static_cast<uint32_t>(nextFrame),
        std::memory_order_relaxed);

    ubo->sparseParams[1] = sparseRateForMask * 100.0f;
    ubo->sparseParams[2] = static_cast<float>(rtxdiReuseInactiveEnabled());

    // 2026-11-02：诊断。稀疏 A/B 需要先确认三件事有没有真的传进来 ——
    // 请求率、折算后的有效率、以及非活跃像素是否进 reservoir tail（sparseParams.z）。
    // 不能用 static 打一次：第一帧时 RTLog 到 Java logger 的通道还没接好，那条会丢；
    // 改成每 300 帧（约 5 秒）刷一条，用 Flushed 保证落盘。只读 env / 已缓存开关。
    if((static_cast<uint32_t>(nextFrame) % 300u) == 0u)
    {
        RTLogFlushed(std::string("[SPARSE-DIAG] frame=") +
            std::to_string(nextFrame) +
            " request=" + std::to_string(sparseRatePercent()) +
            "% effective=" + std::to_string(sparseRateForMask) +
            " inactiveReuse=" + std::to_string(rtxdiReuseInactiveEnabled()) +
            " perfTs=" + std::to_string(perfTimestampsEnabled() ? 1 : 0) +
            " dlssRR=" + std::to_string(g_dlssRRReady ? 1 : 0) +
            " directLightMode=" + std::to_string(directLightModeCurrent));
    }
    ubo->sparseParams[3] =
        g_sparseHistoryValid.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

    double sunPhaseForThisDimension = static_cast<double>(overworldTimePhase);
    {
        static double endDayAccumulator = 0.0;
        static double previousEndPhase = -1.0;
        if(std::fabs(static_cast<double>(dimensionCode) - 3.0) < 0.25)
        {
            if(previousEndPhase >= 0.0)
            {
                double delta = static_cast<double>(overworldTimePhase) - previousEndPhase;
                if(delta < -0.5)      delta += 1.0;
                else if(delta > 0.5)  delta -= 1.0;
                endDayAccumulator += delta;
            }
            previousEndPhase = static_cast<double>(overworldTimePhase);
            double slow = endDayAccumulator / 29.5;
            slow -= std::floor(slow);
            sunPhaseForThisDimension = slow;
        }
        else
        {
            previousEndPhase = -1.0;
        }
    }
    ubo->historyParamsPadding1 =
        std::clamp(
            static_cast<float>(sunPhaseForThisDimension),
            0.0f,
            0.99999994f
        );
    ubo->playerX = static_cast<float>(playerX);
    ubo->playerY = static_cast<float>(playerY);
    ubo->playerZ = static_cast<float>(playerZ);
    ubo->playerInWater = playerInWater > 0.5f ? 1.0f : 0.0f;
    ubo->playerVelocityX = static_cast<float>(playerVelocityX);
    ubo->playerVelocityY = static_cast<float>(playerVelocityY);
    ubo->playerVelocityZ = static_cast<float>(playerVelocityZ);
    ubo->playerHorizontalSpeed = std::sqrt(
        ubo->playerVelocityX * ubo->playerVelocityX +
        ubo->playerVelocityZ * ubo->playerVelocityZ
    );

    const bool nowInWater = ubo->playerInWater > 0.5f;
    if(nowInWater || !g_waterSimPlayerStateValid)
    {
        g_waterSimLastSurfaceY = static_cast<float>(waterSurfaceY);
    }

    if(g_waterSimPlayerStateValid &&
       nowInWater != g_waterSimPlayerWasInWater)
    {
        const float eventSpeed = std::sqrt(
            ubo->playerVelocityX * ubo->playerVelocityX +
            ubo->playerVelocityY * ubo->playerVelocityY +
            ubo->playerVelocityZ * ubo->playerVelocityZ
        );
        g_waterSimPendingImpulse = true;
        g_waterSimPendingImpulseX = ubo->playerX;
        g_waterSimPendingImpulseZ = ubo->playerZ;
        g_waterSimPendingImpulseStrength =
            (nowInWater ? 1.0f : -1.0f) *
            (0.42f + 0.05f * std::min(eventSpeed, 8.0f));
        g_waterSimKeepAliveSeconds = 8.0f;
    }

    g_waterSimPlayerStateValid = true;
    g_waterSimPlayerWasInWater = nowInWater;

    ubo->waterSurfaceY = g_waterSimLastSurfaceY;
    ubo->waterSimOriginX = g_waterSimOriginX;
    ubo->waterSimOriginZ = g_waterSimOriginZ;
    ubo->waterSimTexelWorldSize = WATER_SIM_TEXEL_WORLD_SIZE;
    ubo->waterSimReadLayer = g_waterSimReadLayer;
    ubo->waterSimEnabled =
        (g_waterSimOriginValid &&
         (nowInWater || g_waterSimPendingImpulse ||
          g_waterSimKeepAliveSeconds > 0.0f ||
          waterWindStrength() > 0.0f)) ? 1u : 0u;
    ubo->waterSimImpulsePending =
        g_waterSimPendingImpulse ? 1u : 0u;
    ubo->cameraInWater =
        cameraInWater > 0.5f ? 1u : 0u;
    g_cameraInWater.store(ubo->cameraInWater, std::memory_order_relaxed);
    ubo->waterSimImpulseX = g_waterSimPendingImpulseX;
    ubo->waterSimImpulseZ = g_waterSimPendingImpulseZ;
    ubo->waterSimImpulseStrength = g_waterSimPendingImpulseStrength;
    ubo->waterSimImpulseSurfaceY = g_waterSimLastSurfaceY;

    ubo->weatherParams[0] = std::clamp(static_cast<float>(rainLevel), 0.0f, 1.0f);
    g_waterWeatherRain.store(ubo->weatherParams[0]);
    ubo->weatherParams[1] = std::clamp(static_cast<float>(thunderLevel), 0.0f, 1.0f);
    ubo->weatherParams[2] = static_cast<float>(dimensionCode);
    ubo->weatherParams[3] =
        std::clamp(g_cloudSunForwardScattering.load(), 0.0f, 1.0f);

    ubo->auroraParams[0] = std::clamp(g_auroraStrength.load(), 0.0f, 1.0f);
    ubo->auroraParams[1] = std::clamp(g_auroraBrightness.load(), 0.0f, 3.0f);
    ubo->auroraParams[2] = std::clamp(g_auroraFlowSpeed.load(), 0.0f, 4.0f);
    ubo->auroraParams[3] = 0.0f;

    ubo->rtSampleParams[0] = std::clamp(g_perfRtSpp.load(), 0.5f, 4.0f);

    g_perfL10SppForLog.store(ubo->rtSampleParams[0], std::memory_order_relaxed);
    ubo->rtSampleParams[1] = g_giRoulette.load() != 0u ? 1.0f : 0.0f;
    ubo->rtSampleParams[2] = static_cast<float>(g_sunMarchFarDistance.load());
    ubo->rtSampleParams[3] = waterAdaptiveEnabled()
        ? static_cast<float>(smoothedGpuFrameMs())
        : 0.0f;

    static std::atomic<int> auroraGateLogs{0};
    if(ubo->auroraParams[0] > 0.01f && auroraGateLogs.load() < 3)
    {
        auroraGateLogs.fetch_add(1);
        fprintf(
            stderr,
            "[RTX-AURORA] gate: strength=%.3f dim=%.2f rain=%.2f phase=%.4f\n",
            ubo->auroraParams[0],
            ubo->weatherParams[2],
            ubo->weatherParams[0],
            static_cast<float>(overworldTimePhase));
        fflush(stderr);
    }

    const float requestedDimension = static_cast<float>(dimensionCode);
    constexpr float anchorDistance = 8192.0f;
    const float anchorDeltaX = static_cast<float>(playerX) - g_cloudWorldAnchorX;
    const float anchorDeltaZ = static_cast<float>(playerZ) - g_cloudWorldAnchorZ;
    if(!g_cloudWorldAnchorValid || requestedDimension != g_cloudWorldAnchorDimension ||
       anchorDeltaX * anchorDeltaX + anchorDeltaZ * anchorDeltaZ > anchorDistance * anchorDistance)
    {
        g_cloudWorldAnchorX = std::floor(static_cast<float>(playerX) / 2048.0f) * 2048.0f;
        g_cloudWorldAnchorZ = std::floor(static_cast<float>(playerZ) / 2048.0f) * 2048.0f;
        g_cloudWorldAnchorDimension = requestedDimension;
        g_cloudWorldAnchorValid = true;
        g_volumetricCloudHistoryNeedsClear.store(true);
    }
    populateCloudParams(ubo->cloudParams);
    populateSkyParams(ubo->skyParams);
    populateSkyModelParams(ubo->skyModelParams);
    populateGalaxyParams(ubo->galaxyParams);
    populateGodRayParams(ubo->godRayParams);
    populateGroundMistParams(ubo->godRayParams + 4);
    populateRainWallParams(ubo->rainWallParams);
    populateWaterStyleParams(ubo->waterStyleParams);
    populateHighCloudParams(ubo->highCloudParams);
    populateCloudMsParams(ubo->cloudMsParams);
    populateCloudMarchParams(ubo->cloudMarchParams);
    populateWaterParams(ubo->waterParams);
    populateCloudPhiParams(ubo->cloudPhiParams);
    populateCloudLightParams(ubo->cloudLightParams);
    populateCloudShapeParams(ubo->cloudShapeParams);
    populateCloudMoonParams(ubo->cloudMoonParams);
    populateCloudShape2Params(ubo->cloudShape2Params);
    populateCloudAmbientParams(ubo->cloudAmbientParams);
    populateCloudMsParams2(ubo->cloudMsParams2);
    populateCloudLightParams2(ubo->cloudLightParams2);
    populateCloudProfileParams(ubo->cloudProfileParams);
    populateCloudDetailParams(ubo->cloudDetailParams);
    populateFogExtrasParams(ubo->fogExtrasParams);
    populateCloudTemporalParams(ubo->cloudTemporalParams);
    populateCloudAerialParams(ubo->cloudAerialParams);

#if 0
    {
        const bool foamPublished =
            g_waterFoamAvailable.load() &&
            g_waterFoamLayoutReady &&
            g_waterFoamOriginValid;
        ubo->pomParams[1] = foamPublished ? g_waterFoamOriginX : 0.0f;
        ubo->pomParams[2] = foamPublished ? g_waterFoamOriginZ : 0.0f;
        ubo->pomParams[3] =
            foamPublished ? WATER_FOAM_TEXEL_WORLD_SIZE : 0.0f;
        ubo->cloudAerialParams[3] = foamPublished
            ? static_cast<float>(g_waterFoamReadLayer)
            : 0.0f;
    }
#endif

    populateCloudSkyHueParams(ubo->cloudSkyHueParams);
    populateCloudPhysicsParams(ubo->cloudPhysicsParams);
    populateUeCloudParams(ubo->ueCloudA, ubo->ueCloudB);

    {
        const uint64_t sharcAddresses[4] = {
            g_sharcHashEntriesAddress,
            g_sharcAccumulationAddress,
            g_sharcResolvedAddress,
            g_sharcLockAddress
        };

        for(uint32_t sharcIndex = 0u; sharcIndex < 4u; ++sharcIndex)
        {
            ubo->sharcBuffersLo[sharcIndex] =
                static_cast<uint32_t>(sharcAddresses[sharcIndex] & 0xFFFFFFFFull);
            ubo->sharcBuffersHi[sharcIndex] =
                static_cast<uint32_t>(sharcAddresses[sharcIndex] >> 32u);
        }

        const float sharcSceneScaleValue = sharcSceneScale();

        const float sharcRadianceScaleValue = 1000.0f;
        uint32_t sharcSceneScaleBits = 0u;
        uint32_t sharcRadianceScaleBits = 0u;
        std::memcpy(&sharcSceneScaleBits, &sharcSceneScaleValue, sizeof(uint32_t));
        std::memcpy(&sharcRadianceScaleBits, &sharcRadianceScaleValue, sizeof(uint32_t));

        ubo->sharcCacheParams[0] =
            g_sharcBuffersReady ? sharcSceneScaleBits : 0u;
        ubo->sharcCacheParams[1] =
            g_sharcBuffersReady ? g_sharcEntryCountResolved : 0u;
        ubo->sharcCacheParams[2] =
            g_sharcBuffersReady ? sharcRadianceScaleBits : 0u;
        ubo->sharcCacheParams[3] = sharcModeLane();
    }
    populateWaterWaveParams(ubo->waterWaveParams);
    populateWaterWaveShapeParams(ubo->waterWaveShapeParams);
    populateWaterSssParams(ubo->waterSssParams);
    populateWaterFftParams(ubo->waterFftParams);


    g_cameraHistoryValid = true;
    g_lastCameraFrame = nextFrame;

    g_sparseHistoryValid.store(true, std::memory_order_relaxed);
}

// ============================================================
// 2026-11-04 阶段 0：确定性抓帧哈希（重构回归安全网）
//
// 为什么需要：现有 benchmark 只记帧时间，回答不了"画面有没有变"。
// 拆 rgen / 折叠 traceRay 这类重构必须能判定行为是否逐位一致，否则
// 一旦画面出问题，无法区分"重构引入的 bug"和噪声。
//
// 读哪张图：g_rtOutputImage —— 它是 DLSS-RR 的**输入**、渲染分辨率的
// HDR 结果（rgba16f）。因此不受超分、补帧、swapchain/present 路径影响，
// 正是重构最直接的作用对象。
//
// 用法：ORION_CAPTURE_HASH=<帧号> 启动客户端。到达该帧时读回图像，
// 对原始字节算 FNV-1a 64，写一行到 rt_debug.log：
//   [CAPTURE] frame=N hash=0x... w=.. h=.. nonzero=..
// 改前/改后各跑一次，比对同一帧号的 hash 即可判定逐位一致。
//
// 为什么在帧首读：主 command buffer 此刻还在录制、未提交，直接往里插
// copy 会读到半成品。帧首单独提交一个 cb 并 waitIdle，拿到的是上一帧
// 已完成的最终结果 —— 固定帧号下同样可复现。
// ============================================================
static void orionCaptureDeterministicFrameHash()
{
    static bool parseDone = false;
    static long targetFrame = -1;

    if(!parseDone)
    {
        parseDone = true;

        const char* env =
            std::getenv(
                "ORION_CAPTURE_HASH"
            );

        if(env != nullptr && env[0] != '\0')
        {
            targetFrame =
                std::atol(env);
        }
    }

    if(targetFrame < 0) return;

    if(static_cast<long>(g_perfL10FrameSerial) != targetFrame) return;

    if(g_rtOutputImage == VK_NULL_HANDLE) return;

    if(g_rtOutputWidth == 0u || g_rtOutputHeight == 0u) return;

    if(g_rtOutputLayout == VK_IMAGE_LAYOUT_UNDEFINED) return;

    // rgba16f = 8 字节/像素
    const VkDeviceSize imageBytes =
        static_cast<VkDeviceSize>(g_rtOutputWidth) *
        static_cast<VkDeviceSize>(g_rtOutputHeight) *
        8ull;

    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if(vkCreateBuffer(g_device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
    {
        RTLogFlushed("[CAPTURE] buffer create failed");

        return;
    }

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, buffer, &req);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = req.size;
    allocInfo.memoryTypeIndex =
        findMemoryType(
            g_physicalDevice,
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );

    if(allocInfo.memoryTypeIndex == 0xFFFFFFFFu ||
       vkAllocateMemory(g_device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
    {
        vkDestroyBuffer(g_device, buffer, nullptr);

        RTLogFlushed("[CAPTURE] memory allocate failed");

        return;
    }

    vkBindBufferMemory(g_device, buffer, memory, 0u);

    VkCommandBufferAllocateInfo cmdAlloc{};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = g_rtCommandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1u;

    if(vkAllocateCommandBuffers(g_device, &cmdAlloc, &cmd) != VK_SUCCESS)
    {
        vkFreeMemory(g_device, memory, nullptr);
        vkDestroyBuffer(g_device, buffer, nullptr);

        RTLogFlushed("[CAPTURE] command buffer allocate failed");

        return;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = g_rtOutputLayout;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = g_rtOutputImage;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1u;
    toTransfer.subresourceRange.layerCount = 1u;
    toTransfer.srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer
    );

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1u;
    region.imageExtent.width = g_rtOutputWidth;
    region.imageExtent.height = g_rtOutputHeight;
    region.imageExtent.depth = 1u;

    vkCmdCopyImageToBuffer(
        cmd,
        g_rtOutputImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        buffer,
        1u,
        &region
    );

    VkImageMemoryBarrier toRead = toTransfer;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toRead.newLayout = g_rtOutputLayout;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toRead.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &toRead
    );

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &cmd;

    if(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS)
    {
        vkQueueWaitIdle(g_queue);

        void* mapped = nullptr;

        if(vkMapMemory(
               g_device,
               memory,
               0u,
               VK_WHOLE_SIZE,
               0u,
               &mapped) == VK_SUCCESS)
        {
            const uint8_t* bytes =
                static_cast<const uint8_t*>(mapped);

            uint64_t hash = 14695981039346656037ull;
            size_t nonZero = 0u;

            for(VkDeviceSize i = 0u; i < imageBytes; ++i)
            {
                const uint8_t byte = bytes[i];

                if(byte != 0u) ++nonZero;

                hash ^= byte;
                hash *= 1099511628211ull;
            }

            char line[256];

            std::snprintf(
                line,
                sizeof(line),
                "[CAPTURE] frame=%ld hash=0x%016llx w=%u h=%u "
                "nonzero=%zu bytes=%llu",
                targetFrame,
                static_cast<unsigned long long>(hash),
                g_rtOutputWidth,
                g_rtOutputHeight,
                nonZero,
                static_cast<unsigned long long>(imageBytes)
            );

            RTLogFlushed(
                std::string(line)
            );

            vkUnmapMemory(g_device, memory);
        }
    }

    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &cmd);
    vkFreeMemory(g_device, memory, nullptr);
    vkDestroyBuffer(g_device, buffer, nullptr);
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_recordFrameCommand(
    JNIEnv* env,
    jclass clazz,
    jlong commandBufferHandle,
    jlong targetImageHandle,
    jint width,
    jint height,
    jboolean targetIsMainRenderTarget
)
{
    const LeakProbeScope leakProbeScope;

    RTXMarkPhase("frame:begin");
    RTXHeartbeatTick();

    applyRenderThreadPriorityOnce();
    RTXMarkPhase("frame:prio");

    orionCaptureDeterministicFrameHash();
    RTXMarkPhase("frame:hash");

    perfL10CpuPhaseBeginFrame();
    RTXMarkPhase("frame:phaseCpu");

    maybePersistSharedPipelineCache();
    RTXMarkPhase("frame:persist");

    // 2026-11-02 P1：SPV 热重载（默认关，ORION_RT_PIPE_HOT_RELOAD=1 开启）。
    // 放在帧首、任何 RT pass 录制之前：重建会 vkDeviceWaitIdle 并重建 SBT，
    // 必须发生在这一帧的 command buffer 开始录制之前，否则 SBT 会在帧中途被换掉。
    pollRtPipelineHotReload();
    RTXMarkPhase("frame:hotreload");

    ensurePbrAtlasSampler();
    RTXMarkPhase("frame:atlas");

    const bool publishPbrAtlases =
        g_pbrAtlasesDirty &&
        g_normalAtlasView != VK_NULL_HANDLE &&
        g_specularAtlasView != VK_NULL_HANDLE &&
        g_atlasSampler != VK_NULL_HANDLE;

    const VkSampler pbrAtlasSampler =
        g_pbrAtlasSampler != VK_NULL_HANDLE
            ? g_pbrAtlasSampler
            : g_atlasSampler;
    const bool publishAlbedoAtlas =
        g_atlasDirty &&
        g_atlasImageView != VK_NULL_HANDLE &&
        g_atlasSampler != VK_NULL_HANDLE;

    const bool dynamicTextureBindingsBlocked =
        g_dynamicTextureBindingsNeedAtlasRefresh &&
        !g_dynamicTextureBindingsCanPublish;
    const bool publishEntityTextures =
        (g_entityTexturesDirty ||
         (g_dynamicTextureBindingsNeedAtlasRefresh &&
          g_dynamicTextureBindingsCanPublish)) &&
        !dynamicTextureBindingsBlocked &&
        g_atlasImageView != VK_NULL_HANDLE &&
        g_atlasSampler != VK_NULL_HANDLE;
    const bool publishEntityPbrTextures =
        (g_entityPbrTexturesDirty ||
         (g_dynamicTextureBindingsNeedAtlasRefresh &&
          g_dynamicTextureBindingsCanPublish)) &&
        !dynamicTextureBindingsBlocked &&
        g_entityPbrFallbackView != VK_NULL_HANDLE &&
        g_entityPbrFallbackSampler != VK_NULL_HANDLE;
    const bool publishParticleTextures =
        (g_particleTexturesDirty ||
         (g_dynamicTextureBindingsNeedAtlasRefresh &&
          g_dynamicTextureBindingsCanPublish)) &&
        !dynamicTextureBindingsBlocked &&
        g_atlasImageView != VK_NULL_HANDLE &&
        g_atlasSampler != VK_NULL_HANDLE;
    const bool publishRainStreakTexture =
        (g_rainStreakTextureDirty ||
         (g_dynamicTextureBindingsNeedAtlasRefresh &&
          g_dynamicTextureBindingsCanPublish)) &&
        !dynamicTextureBindingsBlocked &&
        g_atlasImageView != VK_NULL_HANDLE &&
        g_atlasSampler != VK_NULL_HANDLE;
    const bool publishStarMapTexture =
        g_starMapTextureView != VK_NULL_HANDLE &&
        g_starMapTextureSampler != VK_NULL_HANDLE &&
        (g_starMapTextureDirty ||
         (g_dynamicTextureBindingsNeedAtlasRefresh &&
          g_dynamicTextureBindingsCanPublish)) &&
        !dynamicTextureBindingsBlocked;
    const bool publishEndSkyAtlasTexture =
        g_endSkyAtlasView != VK_NULL_HANDLE &&
        g_endSkyAtlasSampler != VK_NULL_HANDLE &&
        (g_endSkyAtlasDirty ||
         (g_dynamicTextureBindingsNeedAtlasRefresh &&
          g_dynamicTextureBindingsCanPublish)) &&
        !dynamicTextureBindingsBlocked;

    const bool reloadTexturePublish =
        publishPbrAtlases ||
        publishAlbedoAtlas ||
        (g_dynamicTextureBindingsNeedAtlasRefresh &&
         g_dynamicTextureBindingsCanPublish);

    const bool churnTexturePublish =
        publishEntityTextures ||
        publishEntityPbrTextures ||
        publishParticleTextures ||
        publishRainStreakTexture ||
        publishStarMapTexture ||
        publishEndSkyAtlasTexture;

    const bool texturePublishDue =
        reloadTexturePublish ||
        g_perfL10FrameSerial >= g_perfL10NextTexturePublishFrame;

    if(
        (reloadTexturePublish || churnTexturePublish) &&
        texturePublishDue &&
        g_rayTracingDescriptorSet == VK_NULL_HANDLE
    )
    {
        RTLogFlushed(
            "[RT DESCRIPTOR] atlas publish deferred: dstSet is VK_NULL_HANDLE "
            "(dirty flags kept for the next frame)"
        );
    }

    if(
        (reloadTexturePublish || churnTexturePublish) &&
        texturePublishDue &&
        g_rayTracingDescriptorSet != VK_NULL_HANDLE
    )
    {
        if(churnTexturePublish)
        {
            g_perfL10NextTexturePublishFrame =
                g_perfL10FrameSerial +
                PERF_L10_TEXTURE_PUBLISH_MIN_FRAME_INTERVAL;
        }

        static const bool logTexturePublish = []() {
            const char* raw = std::getenv("RTX_LOG_TEXPUB");
            return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
        }();

        if(logTexturePublish)
        {
            std::stringstream publishLog;
            publishLog
                << "[PERF L10 TEXPUB]"
                << " frame=" << g_perfL10FrameSerial
                << " kind=" << (reloadTexturePublish ? "reload" : "churn")
                << " ring=" << (g_rtDescriptorRingLastPublishUsedRing ? 1 : 0)
                << " pbr=" << (publishPbrAtlases ? 1 : 0)
                << " albedo=" << (publishAlbedoAtlas ? 1 : 0)
                << " entity=" << (publishEntityTextures ? 1 : 0)
                << " entitypbr=" << (publishEntityPbrTextures ? 1 : 0)
                << " particle=" << (publishParticleTextures ? 1 : 0)
                << " rain=" << (publishRainStreakTexture ? 1 : 0)
                << " star=" << (publishStarMapTexture ? 1 : 0)
                << " endsky=" << (publishEndSkyAtlasTexture ? 1 : 0);

            RTLog(publishLog.str());
        }

        if(reloadTexturePublish)
        {
            PERF_L10_TIME_STALL("atlasPublishDeviceIdle", FGDeviceWaitIdle(g_device));
        }

        VkDescriptorImageInfo normalInfo{};
        VkDescriptorImageInfo specularInfo{};
        VkDescriptorImageInfo atlasInfo{};
        VkDescriptorImageInfo entityInfos[ENTITY_RT_TEXTURE_SLOT_COUNT]{};
        VkDescriptorImageInfo entityNormalInfos[ENTITY_RT_TEXTURE_SLOT_COUNT]{};
        VkDescriptorImageInfo entitySpecularInfos[ENTITY_RT_TEXTURE_SLOT_COUNT]{};
        VkDescriptorImageInfo particleInfos[PARTICLE_RT_TEXTURE_SLOT_COUNT]{};
        VkDescriptorImageInfo rainStreakInfo{};
        VkDescriptorImageInfo starMapInfo{};
        VkDescriptorImageInfo endSkyAtlasInfo{};
        VkWriteDescriptorSet writes[10]{};
        uint32_t writeCount = 0u;

        if(publishAlbedoAtlas)
        {
            atlasInfo.imageView = g_atlasImageView;
            atlasInfo.sampler = g_atlasSampler;
            atlasInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet& write = writes[writeCount++];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = g_rayTracingDescriptorSet;
            write.dstBinding = 3;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &atlasInfo;
        }

        if(publishPbrAtlases)
        {
            normalInfo.imageView = g_normalAtlasView;
            normalInfo.sampler = pbrAtlasSampler;
            normalInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet& normalWrite = writes[writeCount++];
            normalWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            normalWrite.dstSet = g_rayTracingDescriptorSet;
            normalWrite.dstBinding = 4;
            normalWrite.descriptorCount = 1;
            normalWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            normalWrite.pImageInfo = &normalInfo;

            specularInfo.imageView = g_specularAtlasView;
            specularInfo.sampler = pbrAtlasSampler;
            specularInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet& specularWrite = writes[writeCount++];
            specularWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            specularWrite.dstSet = g_rayTracingDescriptorSet;
            specularWrite.dstBinding = 5;
            specularWrite.descriptorCount = 1;
            specularWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            specularWrite.pImageInfo = &specularInfo;
        }

        if(publishEntityTextures)
        {
            for(uint32_t slot = 0u;
                slot < ENTITY_RT_TEXTURE_SLOT_COUNT;
                ++slot)
            {
                entityInfos[slot].imageView =
                    g_entityTextureViews[slot] != VK_NULL_HANDLE
                        ? g_entityTextureViews[slot]
                        : g_atlasImageView;
                entityInfos[slot].sampler =
                    g_entityTextureSamplers[slot] != VK_NULL_HANDLE
                        ? g_entityTextureSamplers[slot]
                        : g_atlasSampler;
                entityInfos[slot].imageLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            VkWriteDescriptorSet& entityWrite = writes[writeCount++];
            entityWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            entityWrite.dstSet = g_rayTracingDescriptorSet;
            entityWrite.dstBinding = 36;
            entityWrite.descriptorCount = ENTITY_RT_TEXTURE_SLOT_COUNT;
            entityWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            entityWrite.pImageInfo = entityInfos;
        }

        if(publishEntityPbrTextures)
        {
            for(uint32_t slot = 0u;
                slot < ENTITY_RT_TEXTURE_SLOT_COUNT;
                ++slot)
            {
                const bool hasNormal =
                    g_entityNormalViews[slot] != VK_NULL_HANDLE &&
                    g_entityNormalSamplers[slot] != VK_NULL_HANDLE;
                entityNormalInfos[slot].imageView = hasNormal
                    ? g_entityNormalViews[slot]
                    : g_entityPbrFallbackView;
                entityNormalInfos[slot].sampler = hasNormal
                    ? g_entityNormalSamplers[slot]
                    : g_entityPbrFallbackSampler;
                entityNormalInfos[slot].imageLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                const bool hasSpecular =
                    g_entitySpecularViews[slot] != VK_NULL_HANDLE &&
                    g_entitySpecularSamplers[slot] != VK_NULL_HANDLE;
                entitySpecularInfos[slot].imageView = hasSpecular
                    ? g_entitySpecularViews[slot]
                    : g_entityPbrFallbackView;
                entitySpecularInfos[slot].sampler = hasSpecular
                    ? g_entitySpecularSamplers[slot]
                    : g_entityPbrFallbackSampler;
                entitySpecularInfos[slot].imageLayout =
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }

            VkWriteDescriptorSet& entityNormalWrite = writes[writeCount++];
            entityNormalWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            entityNormalWrite.dstSet = g_rayTracingDescriptorSet;
            entityNormalWrite.dstBinding = 63;
            entityNormalWrite.descriptorCount = ENTITY_RT_TEXTURE_SLOT_COUNT;
            entityNormalWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            entityNormalWrite.pImageInfo = entityNormalInfos;

            VkWriteDescriptorSet& entitySpecularWrite = writes[writeCount++];
            entitySpecularWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            entitySpecularWrite.dstSet = g_rayTracingDescriptorSet;
            entitySpecularWrite.dstBinding = 64;
            entitySpecularWrite.descriptorCount = ENTITY_RT_TEXTURE_SLOT_COUNT;
            entitySpecularWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            entitySpecularWrite.pImageInfo = entitySpecularInfos;
        }

        if(publishParticleTextures)
        {
            for(uint32_t slot = 0u; slot < PARTICLE_RT_TEXTURE_SLOT_COUNT; ++slot)
            {
                particleInfos[slot].imageView = g_particleTextureViews[slot] != VK_NULL_HANDLE
                    ? g_particleTextureViews[slot] : g_atlasImageView;
                particleInfos[slot].sampler = g_particleTextureSamplers[slot] != VK_NULL_HANDLE
                    ? g_particleTextureSamplers[slot] : g_atlasSampler;
                particleInfos[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            VkWriteDescriptorSet& particleWrite = writes[writeCount++];
            particleWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            particleWrite.dstSet = g_rayTracingDescriptorSet;
            particleWrite.dstBinding = 37;
            particleWrite.descriptorCount = PARTICLE_RT_TEXTURE_SLOT_COUNT;
            particleWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            particleWrite.pImageInfo = particleInfos;
        }

        if(publishRainStreakTexture)
        {
            rainStreakInfo.imageView =
                g_rainStreakTextureView != VK_NULL_HANDLE
                    ? g_rainStreakTextureView
                    : g_atlasImageView;
            rainStreakInfo.sampler =
                g_rainStreakTextureSampler != VK_NULL_HANDLE
                    ? g_rainStreakTextureSampler
                    : g_atlasSampler;
            rainStreakInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet& rainStreakWrite = writes[writeCount++];
            rainStreakWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rainStreakWrite.dstSet = g_rayTracingDescriptorSet;
            rainStreakWrite.dstBinding = 40;
            rainStreakWrite.descriptorCount = 1;
            rainStreakWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            rainStreakWrite.pImageInfo = &rainStreakInfo;
        }

        if(publishStarMapTexture)
        {
            starMapInfo.imageView = g_starMapTextureView;
            starMapInfo.sampler = g_starMapTextureSampler;
            starMapInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet& starMapWrite = writes[writeCount++];
            starMapWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            starMapWrite.dstSet = g_rayTracingDescriptorSet;
            starMapWrite.dstBinding = 44;
            starMapWrite.descriptorCount = 1;
            starMapWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            starMapWrite.pImageInfo = &starMapInfo;
        }

        if(publishEndSkyAtlasTexture)
        {
            endSkyAtlasInfo.imageView = g_endSkyAtlasView;
            endSkyAtlasInfo.sampler = g_endSkyAtlasSampler;
            endSkyAtlasInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet& endSkyWrite = writes[writeCount++];
            endSkyWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            endSkyWrite.dstSet = g_rayTracingDescriptorSet;
            endSkyWrite.dstBinding = 60;
            endSkyWrite.descriptorCount = 1;
            endSkyWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            endSkyWrite.pImageInfo = &endSkyAtlasInfo;
        }

        if(logTexturePublish)
        {
            std::stringstream paramLog;
            paramLog
                << "[RT DESCRIPTOR] publish dstSet=0x"
                << std::hex << (unsigned long long)g_rayTracingDescriptorSet
                << std::dec << " writes=" << writeCount;

            for(uint32_t i = 0u; i < writeCount; ++i)
            {
                paramLog
                    << " | b" << writes[i].dstBinding
                    << " n=" << writes[i].descriptorCount;

                if(writes[i].pImageInfo != nullptr)
                {
                    paramLog
                        << " iv=0x"
                        << std::hex
                        << (unsigned long long)writes[i].pImageInfo[0].imageView
                        << " s=0x"
                        << (unsigned long long)writes[i].pImageInfo[0].sampler
                        << std::dec;
                }
            }

            RTLogFlushed(paramLog.str());
        }

        vkUpdateDescriptorSets(g_device, writeCount, writes, 0, nullptr);
        if(publishAlbedoAtlas)
        {
            g_atlasDirty = false;
        }
        if(publishPbrAtlases)
        {
            g_pbrAtlasesDirty = false;
        }
        if(publishEntityTextures)
        {
            g_entityTexturesDirty = false;
        }
        if(publishEntityPbrTextures)
        {
            g_entityPbrTexturesDirty = false;
        }
        if(publishParticleTextures)
        {
            g_particleTexturesDirty = false;
        }
        if(publishRainStreakTexture)
        {
            g_rainStreakTextureDirty = false;
        }
        if(publishStarMapTexture)
        {
            g_starMapTextureDirty = false;
            updateSkyAtmosphereStarMapDescriptor();
        }
        if(publishEndSkyAtlasTexture)
        {
            g_endSkyAtlasDirty = false;
            updateSkyAtmosphereEndSkyAtlasDescriptor();
        }

        if(publishEntityTextures && publishParticleTextures && publishRainStreakTexture)
        {
            g_dynamicTextureBindingsNeedAtlasRefresh = false;
            g_dynamicTextureBindingsCanPublish = false;
        }

        if(logTexturePublish)
        {
            RTLog(
                std::string("[RT DESCRIPTOR] Published pending atlas descriptor bindings.") +
                (g_rtDescriptorRingLastPublishUsedRing ? " ring=1" : " ring=0")
            );
        }
    }

    ensureDhTextureDescriptors();
    ensureSunShadowFarDescriptor();
    ensureSunShadowNearDescriptor();

    perfL10CpuPhaseAdvance(PERF_L10_CPU_P_GEOMETRY);

    const auto rtxGeometryPhaseBegin = std::chrono::steady_clock::now();
    double rtxGeometryRebuildMs = 0.0;

    logFrameEntryHeartbeat();

    if (!g_isInWorld)
    {
        logFrameSkip("notInWorld", "");
        return;
    }


    ++g_perfL10FrameSerial;

    tickVramProbe();

    updatePerfL10AblationStage();

    {
        CameraUBO* ablateUbo = static_cast<CameraUBO*>(g_cameraUboMapped);
        ablateUbo->perfAblationStage[0] = perfL10AblationStage();

        {
            static const float secondaryMipBias = []() -> float
            {
                const char* raw = std::getenv("ORION_SECONDARY_MIP_BIAS");

                if(raw == nullptr || raw[0] == '\0')
                {
                    return 0.0f;
                }

                const float parsed = static_cast<float>(std::atof(raw));

                return parsed < 0.0f ? 0.0f : (parsed > 3.0f ? 3.0f : parsed);
            }();

            uint32_t biasBits = 0u;
            std::memcpy(&biasBits, &secondaryMipBias, sizeof(biasBits));
            ablateUbo->historyPadding0 = biasBits;
        }
        ablateUbo->perfAblationStage[1] =
            g_motionDebugView.load();
        ablateUbo->perfAblationStage[2] =
            g_motionVectorsEnabled.load();
        {
            static const auto cloudClockOrigin = std::chrono::steady_clock::now();
            const float clockSeconds = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - cloudClockOrigin).count();
            uint32_t clockBits = 0u;
            std::memcpy(&clockBits, &clockSeconds, sizeof(clockBits));
            ablateUbo->padding = clockBits;
        }
        static const uint32_t surfaceDebugLane = [] {
            {
                std::ifstream marker(ResolveRTXRuntimePath("surface_terms.flag"));

                if(marker.good())
                {
                    RTLog("[SURFACE DBG] lane 1 = 光照项分解 (surface_terms.flag)");
                    return 1u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("surface_shadowvis.flag"));

                if(marker.good())
                {
                    RTLog("[SURFACE DBG] lane 2 = 太阳影透过率 (surface_shadowvis.flag)");
                    return 2u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("surface_pom.flag"));

                if(marker.good())
                {
                    RTLog("[SURFACE DBG] lane 3 = 浮雕自阴影链 (surface_pom.flag)");
                    return 3u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("surface_relief.flag"));

                if(marker.good())
                {
                    RTLog("[SURFACE DBG] lane 4 = 浮雕法线细节 (surface_relief.flag)");
                    return 4u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("glass_layer_first.flag"));

                if(marker.good())
                {
                    RTLog("[GLASS DBG] lane 5 = 选层钉第一层 (glass_layer_first.flag)");
                    return 5u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("glass_layer_last.flag"));

                if(marker.good())
                {
                    RTLog("[GLASS DBG] lane 6 = 选层钉最后一层 (glass_layer_last.flag)");
                    return 6u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("glass_write_probe.flag"));

                if(marker.good())
                {
                    RTLog("[GLASS DBG] lane 7 = 无条件写 74 号图 (glass_write_probe.flag)");
                    return 7u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("glass_kill_local.flag"));

                if(marker.good())
                {
                    RTLog("[GLASS DBG] lane 8 = 只杀 localEntityIrradiance (glass_kill_local.flag)");
                    return 8u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("glass_kill_direct.flag"));

                if(marker.good())
                {
                    RTLog("[GLASS DBG] lane 9 = 只杀 directAndEmission (glass_kill_direct.flag)");
                    return 9u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("glass_kill_ambient.flag"));

                if(marker.good())
                {
                    RTLog("[GLASS DBG] lane 10 = 只杀 ambientTerm (glass_kill_ambient.flag)");
                    return 10u;
                }
            }

            {
                std::ifstream marker(ResolveRTXRuntimePath("glass_kill_all.flag"));

                if(marker.good())
                {
                    RTLog("[GLASS DBG] lane 11 = behind 三项全杀 (glass_kill_all.flag)");
                    return 11u;
                }
            }

            return 0u;
        }();
        ablateUbo->perfAblationStage[3] = surfaceDebugLane;
        ablateUbo->perfAoNearSamples =
            g_perfAoNearSamples.load();
        ablateUbo->perfAoMidSamples =
            g_perfAoMidSamples.load();
        ablateUbo->perfGiBounces =
            g_perfGiBounces.load();
        ablateUbo->perfSunShadowSamples =
            g_perfSunShadowSamples.load();
        ablateUbo->perfLocalLightRisCandidates =
            g_perfLocalLightRisCandidates.load();
        ablateUbo->perfLocalLightAreaSamples =
            g_perfLocalLightAreaSamples.load();
        {
            static uint32_t loggedPerfLaneHash = 0u;
            const uint32_t laneHash =
                ablateUbo->perfGiBounces * 73856093u ^
                ablateUbo->perfAoNearSamples * 19349663u ^
                ablateUbo->perfAoMidSamples * 83492791u ^
                ablateUbo->perfSunShadowSamples * 2654435761u ^
                ablateUbo->perfLocalLightRisCandidates;

            if(laneHash != loggedPerfLaneHash)
            {
                loggedPerfLaneHash = laneHash;

                RTLog(
                    "[PERF LANE] giBounces=" + std::to_string(ablateUbo->perfGiBounces) +
                    " aoNear=" + std::to_string(ablateUbo->perfAoNearSamples) +
                    " aoMid=" + std::to_string(ablateUbo->perfAoMidSamples) +
                    " sunShadow=" + std::to_string(ablateUbo->perfSunShadowSamples) +
                    " risCand=" + std::to_string(ablateUbo->perfLocalLightRisCandidates) +
                    " areaSamples=" + std::to_string(ablateUbo->perfLocalLightAreaSamples));
            }
        }


        ablateUbo->perfWaterReflectionInterval =
            g_perfWaterReflectionInterval.load();
        ablateUbo->perfGlossyFullDistance =
            g_perfGlossyFullDistance.load();


        ablateUbo->rtSampleParams[3] = waterAdaptiveEnabled()
            ? static_cast<float>(smoothedGpuFrameMs())
            : 0.0f;
    }

    updatePerfL10SerToggle();

    updatePerfL10SerPathToggles();

    samplePerfL10CpuFrame();

    if(localLightRebuildEnabled() &&
       g_localLightsDirty.load(std::memory_order_acquire))
    {
        rebuildLocalLightGpuBuffer();
    }

    if(g_worldGeometryDirty.load(std::memory_order_acquire))
    {
        if(g_entityRebuildRequested.exchange(false, std::memory_order_acq_rel))
        {
            const auto rtxRebuildCallBegin = std::chrono::steady_clock::now();

            const bool rtxRebuildDone =
                rebuildWorldGeometry(SECTION_REBUILD_MAX_SECTIONS_PER_BATCH);

            rtxGeometryRebuildMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - rtxRebuildCallBegin).count();

            if(!rtxRebuildDone)
            {
                g_entityRebuildRequested.store(true, std::memory_order_release);
            }
        }
        else
        {
        const uint64_t revision =
            g_worldGeometryRevision.load(std::memory_order_acquire);
        if(revision != g_lastObservedSectionRevision)
        {
            g_lastObservedSectionRevision = revision;
            if(g_firstPendingSectionFrame == 0u)
            {
                g_firstPendingSectionFrame = g_perfL10FrameSerial;
            }

            const uint64_t quietDeadline =
                g_perfL10FrameSerial + SECTION_REBUILD_QUIET_FRAMES;
            const uint64_t hardDeadline =
                g_firstPendingSectionFrame +
                SECTION_REBUILD_MAX_DEFERRAL_FRAMES;
            g_nextSectionRebuildFrame = std::min(
                quietDeadline,
                hardDeadline
            );
        }

        const bool firstWorldBatch = !g_asBuilt;
        const bool quietWindowElapsed =
            g_perfL10FrameSerial >= g_nextSectionRebuildFrame;
        const bool hardDeadlineElapsed =
            g_firstPendingSectionFrame != 0u &&
            g_perfL10FrameSerial >=
                g_firstPendingSectionFrame +
                SECTION_REBUILD_MAX_DEFERRAL_FRAMES;
        if(firstWorldBatch || quietWindowElapsed || hardDeadlineElapsed ||
           (g_sectionRebuildInterrupted &&
            g_perfL10FrameSerial >= g_sectionRebuildContinueFrame))
        {
            const auto rtxRebuildCallBegin = std::chrono::steady_clock::now();

            const bool rtxRebuildDone =
                rebuildWorldGeometry(SECTION_REBUILD_MAX_SECTIONS_PER_BATCH);

            rtxGeometryRebuildMs += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - rtxRebuildCallBegin).count();

            if(rtxRebuildDone)
            {
                if(g_worldGeometryDirty.load(std::memory_order_acquire))
                {
                    g_nextSectionRebuildFrame =
                        g_perfL10FrameSerial +
                        SECTION_REBUILD_MIN_FRAME_INTERVAL;
                }
                else
                {
                    g_firstPendingSectionFrame = 0u;
                    g_nextSectionRebuildFrame = 0u;
                }
            }
        }
        }
    }
    else
    {
        g_firstPendingSectionFrame = 0u;
        g_nextSectionRebuildFrame = 0u;
    }

    VkCommandBuffer cmd =
        reinterpret_cast<VkCommandBuffer>(
            commandBufferHandle
        );

    if(g_cameraUboBuffer != VK_NULL_HANDLE)
    {
        vkCmdUpdateBuffer(
            cmd,
            g_cameraUboBuffer,
            0u,
            sizeof(CameraUBO),
            &g_cameraUboHost
        );

        VkBufferMemoryBarrier cameraBarrier{};
        cameraBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        cameraBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        cameraBarrier.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
        cameraBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cameraBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        cameraBarrier.buffer = g_cameraUboBuffer;
        cameraBarrier.offset = 0u;
        cameraBarrier.size = sizeof(CameraUBO);
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 1u,
            &cameraBarrier, 0u, nullptr);
    }

    recordDynamicLightUpload(cmd);


    VkImage target =
        reinterpret_cast<VkImage>(
            targetImageHandle
        );


    const bool targetIsMain =
        targetIsMainRenderTarget == JNI_TRUE;

    if (cmd == VK_NULL_HANDLE ||
        target == VK_NULL_HANDLE)
    {
        logFrameSkip(
            "nullTarget",
            std::string("cmdNull=") +
                ((cmd == VK_NULL_HANDLE) ? "1" : "0") +
            " targetNull=" +
                ((target == VK_NULL_HANDLE) ? "1" : "0")
        );

        return;
    }

    beginPerfL10GpuFrame(cmd);

    beginPerfL10RayStats(cmd);

    {
        const double rtxGeometryPhaseMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() -
                rtxGeometryPhaseBegin
            ).count();

        if(rtxGeometryPhaseMs >= 15.0)
        {
            RTLog(
                "[RT PHASE SLOW] frame=" +
                std::to_string(g_perfL10FrameSerial) +
                " geometry=" + std::to_string(rtxGeometryPhaseMs) +
                " rebuild=" + std::to_string(rtxGeometryRebuildMs) +
                " other=" +
                std::to_string(rtxGeometryPhaseMs - rtxGeometryRebuildMs));
        }
    }

    perfL10CpuPhaseAdvance(PERF_L10_CPU_P_VLI);

    {
        const auto rtxVliBegin = std::chrono::steady_clock::now();

        recordVLIUpdates(cmd);

        const double rtxVliMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() -
                rtxVliBegin
            ).count();

        if(rtxVliMs >= 8.0)
        {
            RTLog(
                "[RT VLI SLOW] frame=" +
                std::to_string(g_perfL10FrameSerial) +
                " vli=" + std::to_string(rtxVliMs) +
                " pending=" + std::to_string(g_vliPendingBacklogForLog) +
                " idx=" + std::to_string(g_vliMsIndex) +
                " alias=" + std::to_string(g_vliMsAlias) +
                " apply=" + std::to_string(g_vliMsApply) +
                " wait=" + std::to_string(g_vliMsLockWait) +
                " gather=" + std::to_string(g_vliMsGather) +
                " pages=" + std::to_string(g_vliGatherPages) +
                " upload=" + std::to_string(g_vliMsUpload) +
                " solve=" + std::to_string(g_vliMsSolve) +
                " rest=" + std::to_string(
                    rtxVliMs -
                    g_vliMsIndex -
                    g_vliMsAlias -
                    g_vliMsApply -
                    g_vliMsLockWait -
                    g_vliMsGather -
                    g_vliMsUpload -
                    g_vliMsSolve));
        }
    }

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_VLI);

    perfL10CpuPhaseAdvance(PERF_L10_CPU_P_FAR);


if(width <= 0 || height <= 0)
{
    std::cerr
        << "[DLSS SIZE] invalid swapchain size "
        << width
        << "x"
        << height
        << std::endl;

    { logFrameSkip("ret@50630", "auto"); return; }
}


const uint32_t outputWidth =
    static_cast<uint32_t>(width);

    const uint32_t outputHeight =
        static_cast<uint32_t>(height);


    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_FAR);

    perfL10CpuPhaseAdvance(PERF_L10_CPU_P_SETUP);



    static bool rr31BuildProbePrinted =
        false;

    if(!rr31BuildProbePrinted)
    {
        fprintf(
            stderr,
            "[RR-3.1 BUILD PROBE] NEW DLL ACTIVE output=%ux%u\n",
            outputWidth,
            outputHeight
        );

        fflush(stderr);

        RTLog(
            "[RR-3.1 BUILD PROBE] NEW DLL ACTIVE"
        );

        rr31BuildProbePrinted =
            true;
    }


    const bool requestedSR = g_upscalerMode.load() == 1;
    const bool useSR = requestedSR && g_dlssReady &&
        g_slDLSSGetOptimalSettings && g_slDLSSSetOptions;
    if((useSR && (!g_dlssReady || !g_slDLSSGetOptimalSettings || !g_slDLSSSetOptions)) ||
       (!useSR && (!g_dlssRRReady || !g_slDLSSDGetOptimalSettings || !g_slDLSSDSetOptions)))
    {
        std::cerr
            << "[RR-5.2] RR functions unavailable"
            << std::endl;

        static bool rrUnavailableLogged = false;
        if(!rrUnavailableLogged)
        {
            rrUnavailableLogged = true;
            RTLog(
                std::string("[UPSCALER] RR/SR functions unavailable, "
                    "upscaler skipped: requestedSR=") +
                (requestedSR ? "1" : "0") +
                " dlssReady=" + (g_dlssReady ? "1" : "0") +
                " rrReady=" + (g_dlssRRReady ? "1" : "0") +
                " srGetOptimal=" + (g_slDLSSGetOptimalSettings ? "1" : "0") +
                " srSetOptions=" + (g_slDLSSSetOptions ? "1" : "0") +
                " rrGetOptimal=" + (g_slDLSSDGetOptimalSettings ? "1" : "0") +
                " rrSetOptions=" + (g_slDLSSDSetOptions ? "1" : "0")
            );
        }

        { logFrameSkip("ret@50704", "auto"); return; }
    }


sl::DLSSDOptions rrOptions{};
sl::DLSSOptions srOptions{};


    const int rrMode = g_rrMode.load();
    switch(rrMode)
    {
        case 0:
        {
            rrOptions.mode =
                sl::DLSSMode::eMaxPerformance;

            break;
        }

        case 1:
        {
            rrOptions.mode =
                sl::DLSSMode::eBalanced;

            break;
        }

        case 2:
        {
            rrOptions.mode =
                sl::DLSSMode::eMaxQuality;

            break;
        }

        case 3:
        {
            rrOptions.mode =
                sl::DLSSMode::eDLAA;

            break;
        }

        case 4:
        {
            rrOptions.mode =
                sl::DLSSMode::eUltraPerformance;

            break;
        }

        default:
        {
            rrOptions.mode =
                sl::DLSSMode::eMaxQuality;

            break;
        }
    }


rrOptions.outputWidth =
    outputWidth;

rrOptions.outputHeight =
    outputHeight;


rrOptions.colorBuffersHDR =
    sl::Boolean::eTrue;


rrOptions.normalRoughnessMode =
    sl::DLSSDNormalRoughnessMode::ePacked;


rrOptions.alphaUpscalingEnabled =
    sl::Boolean::eFalse;


rrOptions.preExposure =
    1.0f;

rrOptions.exposureScale =
    1.0f;


rrOptions.dlaaPreset = sl::DLSSDPreset::ePresetF;
rrOptions.qualityPreset = sl::DLSSDPreset::ePresetF;
rrOptions.balancedPreset = sl::DLSSDPreset::ePresetF;
rrOptions.performancePreset = sl::DLSSDPreset::ePresetF;
rrOptions.ultraPerformancePreset = sl::DLSSDPreset::ePresetF;
rrOptions.ultraQualityPreset = sl::DLSSDPreset::ePresetF;


rrOptions.sharpness =
    0.0f;

const int srMode = g_srMode.load();
srOptions.mode = srMode == 0 ? sl::DLSSMode::eMaxPerformance :
                 srMode == 1 ? sl::DLSSMode::eBalanced :
                 srMode == 3 ? sl::DLSSMode::eDLAA :
                 srMode == 4 ? sl::DLSSMode::eUltraPerformance :
                 sl::DLSSMode::eMaxQuality;
srOptions.outputWidth = outputWidth;
srOptions.outputHeight = outputHeight;
srOptions.colorBuffersHDR = sl::Boolean::eTrue;
srOptions.alphaUpscalingEnabled = sl::Boolean::eFalse;
srOptions.preExposure = 1.0f;
srOptions.exposureScale = 1.0f;
srOptions.sharpness = 0.0f;




static bool hasCachedRROptimalSettings = false;
static uint32_t cachedRROutputWidth = 0u;
static uint32_t cachedRROutputHeight = 0u;
static int cachedRRMode = -1;
static VkDevice cachedRRDevice = VK_NULL_HANDLE;
static PFN_slDLSSDGetOptimalSettings cachedRROptimalFunction = nullptr;
static sl::DLSSDOptimalSettings cachedRROptimal{};
static sl::Result cachedRROptimalResult =
    sl::Result::eErrorFeatureNotSupported;

const bool canQueryRROptimal =
    !useSR && g_dlssRRReady && g_slDLSSDGetOptimalSettings != nullptr;
const bool rrOptimalCacheMiss =
    !hasCachedRROptimalSettings ||
    cachedRROutputWidth != outputWidth ||
    cachedRROutputHeight != outputHeight ||
    cachedRRMode != rrMode ||
    cachedRRDevice != g_device ||
    cachedRROptimalFunction != g_slDLSSDGetOptimalSettings;

if(canQueryRROptimal && rrOptimalCacheMiss)
{
    cachedRROptimal = {};
    cachedRROptimalResult =
        g_slDLSSDGetOptimalSettings(rrOptions, cachedRROptimal);
    cachedRROutputWidth = outputWidth;
    cachedRROutputHeight = outputHeight;
    cachedRRMode = rrMode;
    cachedRRDevice = g_device;
    cachedRROptimalFunction = g_slDLSSDGetOptimalSettings;
    hasCachedRROptimalSettings = true;
}
else if(!canQueryRROptimal)
{
    hasCachedRROptimalSettings = false;
    cachedRROptimal = {};
    cachedRROptimalResult = sl::Result::eErrorFeatureNotSupported;
}

static bool hasCachedRRCloudPinSettings = false;
static uint32_t cachedRRCloudPinOutputWidth = 0u;
static uint32_t cachedRRCloudPinOutputHeight = 0u;
static VkDevice cachedRRCloudPinDevice = VK_NULL_HANDLE;
static PFN_slDLSSDGetOptimalSettings cachedRRCloudPinFunction = nullptr;
static sl::DLSSDOptimalSettings cachedRRCloudPin{};
static sl::Result cachedRRCloudPinResult = sl::Result::eErrorFeatureNotSupported;

const bool rrCloudPinCacheMiss =
    !hasCachedRRCloudPinSettings ||
    cachedRRCloudPinOutputWidth != outputWidth ||
    cachedRRCloudPinOutputHeight != outputHeight ||
    cachedRRCloudPinDevice != g_device ||
    cachedRRCloudPinFunction != g_slDLSSDGetOptimalSettings;

if(canQueryRROptimal && rrCloudPinCacheMiss)
{
    sl::DLSSDOptions qualityOptions = rrOptions;
    qualityOptions.mode = sl::DLSSMode::eMaxQuality;
    cachedRRCloudPin = {};
    cachedRRCloudPinResult =
        g_slDLSSDGetOptimalSettings(qualityOptions, cachedRRCloudPin);
    cachedRRCloudPinOutputWidth = outputWidth;
    cachedRRCloudPinOutputHeight = outputHeight;
    cachedRRCloudPinDevice = g_device;
    cachedRRCloudPinFunction = g_slDLSSDGetOptimalSettings;
    hasCachedRRCloudPinSettings = true;
}
else if(!canQueryRROptimal)
{
    hasCachedRRCloudPinSettings = false;
    cachedRRCloudPin = {};
    cachedRRCloudPinResult = sl::Result::eErrorFeatureNotSupported;
}

static bool hasCachedSROptimalSettings = false;
static uint32_t cachedSROutputWidth = 0u;
static uint32_t cachedSROutputHeight = 0u;
static int cachedSRMode = -1;
static VkDevice cachedSRDevice = VK_NULL_HANDLE;
static PFN_slDLSSGetOptimalSettings cachedSROptimalFunction = nullptr;
static sl::DLSSOptimalSettings cachedSROptimal{};
static sl::Result cachedSROptimalResult = sl::Result::eErrorFeatureNotSupported;
const bool canQuerySROptimal = useSR && g_dlssReady && g_slDLSSGetOptimalSettings;
const bool srOptimalCacheMiss = !hasCachedSROptimalSettings || cachedSROutputWidth != outputWidth ||
    cachedSROutputHeight != outputHeight || cachedSRMode != srMode || cachedSRDevice != g_device ||
    cachedSROptimalFunction != g_slDLSSGetOptimalSettings;
if(canQuerySROptimal && srOptimalCacheMiss)
{
    cachedSROptimal = {};
    cachedSROptimalResult = g_slDLSSGetOptimalSettings(srOptions, cachedSROptimal);
    cachedSROutputWidth = outputWidth;
    cachedSROutputHeight = outputHeight;
    cachedSRMode = srMode;
    cachedSRDevice = g_device;
    cachedSROptimalFunction = g_slDLSSGetOptimalSettings;
    hasCachedSROptimalSettings = true;
}
else if(!canQuerySROptimal)
{
    hasCachedSROptimalSettings = false;
    cachedSROptimal = {};
    cachedSROptimalResult = sl::Result::eErrorFeatureNotSupported;
}

const sl::DLSSDOptimalSettings& rrOptimal = cachedRROptimal;
const sl::Result rrOptimalResult = cachedRROptimalResult;

{
    static int optimalMirrorKey = -1;
    const int optimalMirrorKeyNow =
        (useSR ? 1 : 0) * 10000000 +
        (canQueryRROptimal ? 1 : 0) * 1000000 +
        (canQuerySROptimal ? 1 : 0) * 100000 +
        g_rrMode.load() * 1000 +
        g_srMode.load() * 10 +
        static_cast<int>(outputWidth / 100u);
    if(optimalMirrorKeyNow != optimalMirrorKey)
    {
        optimalMirrorKey = optimalMirrorKeyNow;
        std::stringstream optimalMirror;
        optimalMirror << "[OPTIMAL]"
                      << " path=" << (useSR ? "SR" : "RR")
                      << " requestedSR=" << (requestedSR ? 1 : 0)
                      << " upscalerMode=" << g_upscalerMode.load()
                      << " dlssReady=" << (g_dlssReady ? 1 : 0)
                      << " rrReady=" << (g_dlssRRReady ? 1 : 0)
                      << " rrMode=" << g_rrMode.load()
                      << " srMode=" << g_srMode.load()
                      << " queryRR=" << (canQueryRROptimal ? 1 : 0)
                      << " querySR=" << (canQuerySROptimal ? 1 : 0)
                      << " rrResult=" << static_cast<int>(rrOptimalResult)
                      << " rrOptimal=" << rrOptimal.optimalRenderWidth
                      << "x" << rrOptimal.optimalRenderHeight
                      << " srResult=" << static_cast<int>(cachedSROptimalResult)
                      << " srOptimal=" << cachedSROptimal.optimalRenderWidth
                      << "x" << cachedSROptimal.optimalRenderHeight
                      << " out=" << outputWidth << "x" << outputHeight;
        RTLog(optimalMirror.str());
    }
}


static uint32_t lastRRSizeLogWidth =
    0;

static uint32_t lastRRSizeLogHeight =
    0;


if(lastRRSizeLogWidth != outputWidth ||
   lastRRSizeLogHeight != outputHeight)
{
    fprintf(
        stderr,
        "[RR SIZE]"
        " result=%d"
        " output=%ux%u"
        " rrOptimal=%ux%u"
        " rrMin=%ux%u"
        " rrMax=%ux%u"
        " HDR=1"
        " packedNR=1\n",

        static_cast<int>(
            rrOptimalResult
        ),

        outputWidth,
        outputHeight,

        rrOptimal.optimalRenderWidth,
        rrOptimal.optimalRenderHeight,

        rrOptimal.renderWidthMin,
        rrOptimal.renderHeightMin,

        rrOptimal.renderWidthMax,
        rrOptimal.renderHeightMax
    );


    fflush(
        stderr
    );


    lastRRSizeLogWidth =
        outputWidth;

    lastRRSizeLogHeight =
        outputHeight;
}

if(!g_dlssGFeatureInitialized)
{
    g_dlssGFeatureInitialized = true;
    g_dlssGLoadResult =
        g_dlssGSupported && g_slSetFeatureLoaded
            ? g_slSetFeatureLoaded(sl::kFeatureDLSS_G, true)
            : sl::Result::eErrorFeatureNotSupported;

    if(g_dlssGLoadResult == sl::Result::eOk && g_slGetFeatureFunction)
    {
        void* setOptionsFn = nullptr;
        void* getStateFn = nullptr;
        const sl::Result setOptionsResult =
            g_slGetFeatureFunction(
                sl::kFeatureDLSS_G,
                "slDLSSGSetOptions",
                setOptionsFn
            );
        const sl::Result getStateResult =
            g_slGetFeatureFunction(
                sl::kFeatureDLSS_G,
                "slDLSSGGetState",
                getStateFn
            );

        g_slDLSSGSetOptions =
            reinterpret_cast<PFN_slDLSSGSetOptions>(setOptionsFn);
        g_slDLSSGGetState =
            reinterpret_cast<PFN_slDLSSGGetState>(getStateFn);
        g_dlssGReady =
            setOptionsResult == sl::Result::eOk &&
            getStateResult == sl::Result::eOk &&
            g_slDLSSGSetOptions != nullptr &&
            g_slDLSSGGetState != nullptr;

        fprintf(
            stderr,
            "[FG-1 INIT] load=%d setOptions=%d getState=%d ready=%d\n",
            static_cast<int>(g_dlssGLoadResult),
            static_cast<int>(setOptionsResult),
            static_cast<int>(getStateResult),
            g_dlssGReady ? 1 : 0
        );

        {
            std::ostringstream fgInitLog;

            fgInitLog
                << "[FG-1 INIT]"
                << " load=" << static_cast<int>(g_dlssGLoadResult)
                << " setOptions=" << static_cast<int>(setOptionsResult)
                << " getState=" << static_cast<int>(getStateResult)
                << " ready=" << (g_dlssGReady ? 1 : 0);

            RTLog(fgInitLog.str());
        }
    }
    else
    {
        g_dlssGReady = false;
        fprintf(
            stderr,
            "[FG-1 INIT] load=%d ready=0\n",
            static_cast<int>(g_dlssGLoadResult)
        );

        {
            std::ostringstream fgInitLog;

            fgInitLog
                << "[FG-1 INIT]"
                << " load=" << static_cast<int>(g_dlssGLoadResult)
                << " ready=0";

            RTLog(fgInitLog.str());
        }
    }
}
    if(
        g_dlssGReady &&
        g_slDLSSGGetState &&
        (g_perfL10FrameSerial % 120u) == 0u
    )
    {
        sl::DLSSGState state{};


        sl::Result stateResult =
            g_slDLSSGGetState(
                g_dlssViewport,
                state,
                nullptr
            );


        if(stateResult == sl::Result::eOk && state.numFramesToGenerateMax >= 1u)
        {
            g_fgGeneratedMaxSeen.store(
                state.numFramesToGenerateMax
            );
        }


        fprintf(
            stderr,
            "[FG-1 STATE]"
            " result=%d"
            " status=%u"
            " minWidthOrHeight=%u"
            " numFramesToGenerateMax=%u"
            " presented=%u\n",

            static_cast<int>(
                stateResult
            ),

            static_cast<unsigned int>(
                state.status
            ),

            state.minWidthOrHeight,

            state.numFramesToGenerateMax,

            state.numFramesActuallyPresented
        );

        fflush(stderr);

        {
            std::ostringstream fgStateLog;

            fgStateLog
                << "[FG-1 STATE]"
                << " result=" << static_cast<int>(stateResult)
                << " status=" << static_cast<unsigned int>(state.status)
                << " minWidthOrHeight=" << state.minWidthOrHeight
                << " numFramesToGenerateMax=" << state.numFramesToGenerateMax
                << " presented=" << state.numFramesActuallyPresented;

            RTLog(fgStateLog.str());
        }
    }



    if((!useSR && (rrOptimalResult != sl::Result::eOk || rrOptimal.optimalRenderWidth == 0 || rrOptimal.optimalRenderHeight == 0)) ||
       (useSR && (cachedSROptimalResult != sl::Result::eOk || cachedSROptimal.optimalRenderWidth == 0 || cachedSROptimal.optimalRenderHeight == 0)))
    {
        fprintf(
            stderr,
            "[RR-5.1] invalid RR optimal sizing"
            " result=%d"
            " render=%ux%u\n",

            static_cast<int>(useSR ? cachedSROptimalResult : rrOptimalResult),

            useSR ? cachedSROptimal.optimalRenderWidth : rrOptimal.optimalRenderWidth,
            useSR ? cachedSROptimal.optimalRenderHeight : rrOptimal.optimalRenderHeight
        );

        fflush(stderr);

        { logFrameSkip("ret@51238", "auto"); return; }
    }


    const uint32_t renderWidth = useSR
        ? cachedSROptimal.optimalRenderWidth
        : rrOptimal.optimalRenderWidth;

    const uint32_t renderHeight = useSR
        ? cachedSROptimal.optimalRenderHeight
        : rrOptimal.optimalRenderHeight;
    static bool rr53Printed =
    false;

    if(!rr53Printed)
    {
        fprintf(
            stderr,
            "[DLSS-5.3]"
            " runtimeSizing=selected"
            " SRQuery=enabled\n"
        );

        fflush(stderr);

        rr53Printed =
            true;
    }


    {
        static int sizingMirrorKey = -1;
        const int sizingMirrorKeyNow =
            (useSR ? 1 : 0) * 1000000 +
            g_rrMode.load() * 10000 +
            g_srMode.load() * 100 +
            static_cast<int>(renderWidth / 32u);
        if(sizingMirrorKeyNow != sizingMirrorKey)
        {
            sizingMirrorKey = sizingMirrorKeyNow;
            std::stringstream sizingMirror;
            sizingMirror << "[SIZING]"
                         << " path=" << (useSR ? "SR" : "RR")
                         << " rrMode=" << g_rrMode.load()
                         << " srMode=" << g_srMode.load()
                         << " render=" << renderWidth << "x" << renderHeight
                         << " output=" << outputWidth << "x" << outputHeight
                         << " rrOptimal=" << rrOptimal.optimalRenderWidth
                         << "x" << rrOptimal.optimalRenderHeight
                         << " rrResult=" << static_cast<int>(rrOptimalResult)
                         << " srResult=" << static_cast<int>(cachedSROptimalResult);
            RTLog(sizingMirror.str());
        }
    }

    static uint32_t rr51LastWidth = 0;
    static uint32_t rr51LastHeight = 0;


    if(
        rr51LastWidth != renderWidth ||
        rr51LastHeight != renderHeight
    )
    {
        fprintf(
            stderr,
            "[DLSS-5.1]"
            " activeSizing=%s"
            " render=%ux%u"
            " output=%ux%u\n",

            useSR ? "SR" : "RR",
            renderWidth,
            renderHeight,

            outputWidth,
            outputHeight
        );

        fflush(stderr);

        rr51LastWidth =
            renderWidth;

        rr51LastHeight =
            renderHeight;
    }



    if(g_rtOutputImage == VK_NULL_HANDLE ||
       g_rtOutputWidth != renderWidth ||
       g_rtOutputHeight != renderHeight)
    {
        createRTOutputImage(
            renderWidth,
            renderHeight
        );
    }



    if(
        g_rrDiffuseAlbedoImage ==
            VK_NULL_HANDLE ||

        g_rrSpecularAlbedoImage ==
            VK_NULL_HANDLE ||

        g_rrNormalRoughnessImage ==
            VK_NULL_HANDLE ||

        g_rrSpecularHitDistanceImage ==
            VK_NULL_HANDLE ||

        g_rrStableRadianceImage ==
            VK_NULL_HANDLE ||

        g_rrGBufferWidth !=
            renderWidth ||

        g_rrGBufferHeight !=
            renderHeight
        )
    {
        createRRGBufferImages(
            renderWidth,
            renderHeight
        );
    }


    // 2026-11-02 屏幕空间降级链（SSR + SSPT）：两条滑条都为 0 时此调用
    // 立刻返回，不建任何图 / 管线（默认关闭 ⇒ 零代价）。
    createSSHybridResources(
        renderWidth,
        renderHeight
    );


    createGiHalfImage(
        renderWidth,
        renderHeight
    );


    // 2026-11-04 A1：GI 时域重投影历史（binding 86/87）。始终创建：
    // rgen 描述符布局含 86/87，且关断走 giRayParams.y，资源很小可忽略。
    createGiTemporalImages(
        renderWidth,
        renderHeight
    );


    createGlassAlphaBlendImages(
        renderWidth,
        renderHeight
    );


    createVPTDirectHistoryBuffers(
        renderWidth,
        renderHeight
    );



if(g_dlssOutputImage == VK_NULL_HANDLE ||
   g_dlssOutputWidth != outputWidth ||
   g_dlssOutputHeight != outputHeight)
{
    createDLSSOutputImage(
        outputWidth,
        outputHeight
    );
}

    if(g_motionImage == VK_NULL_HANDLE ||
       g_motionWidth != renderWidth ||
       g_motionHeight != renderHeight)
    {
        createMotionImage(
            renderWidth,
            renderHeight
        );
    }


    if(g_dlssDepthImage == VK_NULL_HANDLE ||
       g_dlssDepthWidth != renderWidth ||
       g_dlssDepthHeight != renderHeight)
    {
        createDLSSDepthImage(
            renderWidth,
            renderHeight
        );
    }

    ensureNRDResources(
        renderWidth,
        renderHeight
    );


    if(g_dlssDepthLayout != VK_IMAGE_LAYOUT_GENERAL)
    {
        VkImageMemoryBarrier depthToGeneral{};

        depthToGeneral.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

        depthToGeneral.oldLayout =
            g_dlssDepthLayout;

        depthToGeneral.newLayout =
            VK_IMAGE_LAYOUT_GENERAL;

        depthToGeneral.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;

        depthToGeneral.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;

        depthToGeneral.srcAccessMask =
            0;

        depthToGeneral.dstAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT;

        depthToGeneral.image =
            g_dlssDepthImage;

        depthToGeneral.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;

        depthToGeneral.subresourceRange.baseMipLevel = 0;
        depthToGeneral.subresourceRange.levelCount = 1;

        depthToGeneral.subresourceRange.baseArrayLayer = 0;
        depthToGeneral.subresourceRange.layerCount = 1;


        vkCmdPipelineBarrier(
            cmd,

            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,

            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,

            0,

            0,
            nullptr,

            0,
            nullptr,

            1,
            &depthToGeneral
        );


        g_dlssDepthLayout =
            VK_IMAGE_LAYOUT_GENERAL;


        std::cerr
            << "[DLSS DEPTH LAYOUT]"
            << " GENERAL before trace"
            << std::endl;
    }



VkImageMemoryBarrier rrGuideBarriers[5]{};

uint32_t rrGuideBarrierCount =
    0;


auto prepareRRGuideForRayGen =
[&](
    VkImage image,
    VkImageLayout& layout)
{
    if(layout ==
       VK_IMAGE_LAYOUT_GENERAL)
    {
        { logFrameSkip("ret@51535", "auto"); return; }
    }


    VkImageMemoryBarrier& b =
        rrGuideBarriers[
            rrGuideBarrierCount++
        ];


    b.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    b.oldLayout =
        layout;

    b.newLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    b.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    b.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    b.srcAccessMask =
        0;

    b.dstAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT;

    b.image =
        image;

    b.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;

    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
};


if(!useSR)
{
prepareRRGuideForRayGen(g_rrDiffuseAlbedoImage, g_rrDiffuseAlbedoLayout);

prepareRRGuideForRayGen(
    g_rrSpecularAlbedoImage,
    g_rrSpecularAlbedoLayout
);

prepareRRGuideForRayGen(
    g_rrNormalRoughnessImage,
    g_rrNormalRoughnessLayout
);

prepareRRGuideForRayGen(g_rrSpecularHitDistanceImage, g_rrSpecularHitDistanceLayout);
}


prepareRRGuideForRayGen(
    g_rrStableRadianceImage,
    g_rrStableRadianceLayout
);


if(rrGuideBarrierCount > 0)
{
    vkCmdPipelineBarrier(
        cmd,

        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,

        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,

        0,

        0,
        nullptr,

        0,
        nullptr,

        rrGuideBarrierCount,
        rrGuideBarriers
    );


    g_rrDiffuseAlbedoLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    g_rrSpecularAlbedoLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    g_rrNormalRoughnessLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    g_rrSpecularHitDistanceLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    g_rrStableRadianceLayout =
        VK_IMAGE_LAYOUT_GENERAL;


    std::cerr
        << "[RR GBUFFER LAYOUT]"
        << " GENERAL before trace"
        << std::endl;
}



    VkImageMemoryBarrier barrier{};

    barrier.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;


    barrier.oldLayout =
    g_rtOutputLayout;

    barrier.newLayout =
        VK_IMAGE_LAYOUT_GENERAL;


    barrier.srcAccessMask = 0;


    barrier.dstAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT;


    barrier.image =
        g_rtOutputImage;


    barrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;


    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;


    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;



    if(g_rtOutputLayout != VK_IMAGE_LAYOUT_GENERAL)
    {

        VkImageMemoryBarrier barrier{};

        barrier.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;


        barrier.oldLayout =
            g_rtOutputLayout;


        barrier.newLayout =
            VK_IMAGE_LAYOUT_GENERAL;


        barrier.srcAccessMask = 0;


        barrier.dstAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT;


        barrier.image =
            g_rtOutputImage;


        barrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;


        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;

        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;


        vkCmdPipelineBarrier(

            cmd,

            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,

            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,

            0,

            0,
            nullptr,

            0,
            nullptr,

            1,
            &barrier
        );

        std::cerr
    << "[RT OUTPUT LAYOUT] "
    << (int)g_rtOutputLayout
    << " -> GENERAL before trace"
    << std::endl;


        g_rtOutputLayout =
            VK_IMAGE_LAYOUT_GENERAL;

    }


    static bool printedHandle = false;

    if(!printedHandle)
    {

        printedHandle = true;
    }

    recordDhTextureBarrier(cmd);

    prepareVPTDirectHistoryBuffers(cmd);

    // 2026-11-04 A1：GI 时域历史 clear + 帧内屏障（needsClear 只在重建帧为真）。
    prepareGiTemporalHistoryImages(cmd);

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SETUP);

    perfL10CpuPhaseAdvance(PERF_L10_CPU_P_WATER);

    recordWaterSimulation(cmd);

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_WSIM);

    recordWaterFftUpdate(cmd);

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_WFFT);

    recordWaterCausticMap(cmd);

#if 0
    recordWaterFoam(cmd);
#endif

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_WCAUS);

    if(kWATER_CAUSTIC_PASS_ENABLED)
    {
        recordWaterCaustics(cmd);
    }

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_CAUS);

    perfL10CpuPhaseAdvance(PERF_L10_CPU_P_RT);

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_CLOUDPIPE);
    const auto rtPhaseT0 = std::chrono::steady_clock::now();
    auto rtPhaseMark = rtPhaseT0;
    double rtSpikeCloudPipe = 0.0, rtSpikeSkyPipe = 0.0, rtSpikeShadowFar = 0.0;
    double rtSpikeSharc = 0.0, rtSpikeBind = 0.0, rtSpikeSky = 0.0;
    double rtSpikeTrace = 0.0, rtSpikeSharcResolve = 0.0;
    auto rtMark = [&rtPhaseMark]()
    {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - rtPhaseMark).count();
        rtPhaseMark = now;
        return ms;
    };
    {
        uint32_t cloudPassWidth = renderWidth;
        uint32_t cloudPassHeight = renderHeight;
        const uint32_t cloudQuality = g_cloudQuality.load();
        const uint32_t cloudDownscale = cloudQuality == 0u ? 4u : (cloudQuality == 2u ? 1u : 2u);
        const bool postRrCloud =
            g_cloudPostRrComposite.load() &&
            g_dlssOutputWidth > 0u && g_dlssOutputHeight > 0u;
        if(postRrCloud)
        {
            cloudPassWidth = std::max(1u, g_dlssOutputWidth / cloudDownscale);
            cloudPassHeight = std::max(1u, g_dlssOutputHeight / cloudDownscale);
        }
        else
        {
            cloudPassWidth = std::max(1u, cloudPassWidth / cloudDownscale);
            cloudPassHeight = std::max(1u, cloudPassHeight / cloudDownscale);
        }

        const uint32_t cloudFloorPercent = g_cloudResFloorPercent.load();
        if(cloudFloorPercent > 0u &&
           g_dlssOutputWidth > 0u && g_dlssOutputHeight > 0u)
        {
            const uint32_t floorWidth =
                (g_dlssOutputWidth * cloudFloorPercent + 99u) / 100u;
            const uint32_t floorHeight =
                (g_dlssOutputHeight * cloudFloorPercent + 99u) / 100u;
            cloudPassWidth = std::min(
                std::max(cloudPassWidth, floorWidth), g_dlssOutputWidth);
            cloudPassHeight = std::min(
                std::max(cloudPassHeight, floorHeight), g_dlssOutputHeight);
        }
        createVolumetricCloudPipeline(cloudPassWidth, cloudPassHeight);
    }
    rtSpikeCloudPipe = rtMark();
    createSkyAtmospherePipeline();
    rtSpikeSkyPipe = rtMark();

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_RTPIPE);

    recordSunShadowFarMap(cmd);
    rtSpikeShadowFar = rtMark();

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SFAR);

    recordSharcZeroFillIfNeeded(cmd);
recordSharcUpdate(cmd, renderWidth, renderHeight);
    rtSpikeSharc = rtMark();

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SHARCUP);

    vkCmdBindPipeline(
        cmd,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        g_rayTracingPipeline
    );
    vkCmdBindDescriptorSets(

        cmd,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        g_rayTracingPipelineLayout,
        0,
        1,
        &g_rayTracingDescriptorSet,
        0,
        nullptr
    );
    rtSpikeBind = rtMark();

    auto trace =
       getCmdTraceRays();

    if(!trace)
    {
        logFrameSkip("noTraceRays", "getCmdTraceRays returned null");

        return;
    }


    setPerfL10GpuRenderSize(renderWidth, renderHeight);
    recordSkyAtmosphere(cmd);

    recordVolumeRestir(cmd, renderWidth, renderHeight);
    rtSpikeSky = rtMark();

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SKY);

    recordGiHalfPass(cmd, renderWidth, renderHeight);
    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_GI);

    vkCmdBindPipeline(
        cmd,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        g_rayTracingPipeline
    );
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        g_rayTracingPipelineLayout,
        0,
        1,
        &g_rayTracingDescriptorSet,
        0,
        nullptr
    );

    recordSparseActiveMask(cmd, renderWidth, renderHeight);

    recordGlassAlphaBlendPrepare(cmd);

    // 2026-11-05 B：K1(主视线)→landing→K2(传输) 两发（2026-11-06 起默认开，ORION_K1_K2=0 回退）；
    // 未启用/管线失败时回退单发主管线，路径与旧版逐位一致。
    if(!traceK1K2(
           cmd,
           renderWidth,
           renderHeight
       ))
    {
        trace(
            cmd,
            &g_raygenRegion,
            &g_missRegion,
            &g_hitRegion,
            &g_callableRegion,
            renderWidth,
            renderHeight,
            1
        );
    }
    rtSpikeTrace = rtMark();
    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_TRACE);

    recordGlassAlphaBlend(cmd, renderWidth, renderHeight);

    recordSharcResolve(cmd);
    rtSpikeSharcResolve = rtMark();

    // 2026-11-02 屏幕空间降级链（SSR + SSPT）。挂点在这里的理由：
    //   · 本帧 rgen 已写完颜色 / 深度 / guide / 分项 G-buffer；
    //   · 玻璃 alpha-blend 已在本帧颜色上合成（2716）⇒ 拷贝的基准色是最终色；
    //   · 还没进 NRD / DLSS ⇒ 屏幕空间的偏差仍能被降噪与 RR 吃掉。
    // 默认关闭时本调用立刻返回，不录任何命令。
    recordSSHybridPass(
        cmd,
        renderWidth,
        renderHeight
    );

    {
        const auto rtPhaseEnd = std::chrono::steady_clock::now();
        const double rtPhaseTotal =
            std::chrono::duration<double, std::milli>(rtPhaseEnd - rtPhaseT0).count();
        if(rtPhaseTotal > 25.0)
        {
            char rtSpikeBuffer[320];
            std::snprintf(rtSpikeBuffer, sizeof(rtSpikeBuffer),
                "[RT SPIKE] total=%.1fms cloudPipe=%.1f skyPipe=%.1f shadowFar=%.1f "
                "sharc=%.1f bind=%.1f sky=%.1f trace=%.1f sharcResolve=%.1f "
                "render=%ux%u",
                rtPhaseTotal, rtSpikeCloudPipe, rtSpikeSkyPipe, rtSpikeShadowFar,
                rtSpikeSharc, rtSpikeBind, rtSpikeSky, rtSpikeTrace, rtSpikeSharcResolve,
                renderWidth, renderHeight);
            RTLog(std::string(rtSpikeBuffer));
        }
    }

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_RT);

    perfL10CpuPhaseAdvance(PERF_L10_CPU_P_CLOUD);

    endPerfL10RayStats(cmd);

#if NRD_LOCAL_DI_CONFIDENCE
    if(kNrdLocalDiEnabled)
    {
        g_nrdConfidenceViewThisFrame =
            recordDiConfidence(cmd, renderWidth, renderHeight);
    }
    else
    {
        g_nrdConfidenceViewThisFrame = VK_NULL_HANDLE;
    }
#endif

    if(g_directAtrousEnabled)
    {
        recordDirectAtrous(
            cmd,
            renderWidth,
            renderHeight
        );
    }

    if(kNrdLocalDiEnabled && !g_directAtrousReady && g_nrdReady)
    {
        recordNRDLocalDenoiser(
            cmd,
            renderWidth,
            renderHeight
        );
    }

#if NRD_LOCAL_DI_CONFIDENCE
    if(kNrdLocalDiEnabled && g_nrdConfidenceViewThisFrame != VK_NULL_HANDLE)
    {
        g_diConfidenceParity ^= 1u;
    }
#endif

    if(kNrdLocalDiEnabled && (g_nrdReady || g_directAtrousReady))
    {
        if(g_nrdCompositePipeline != VK_NULL_HANDLE)
        {
            VkImageMemoryBarrier colourForNRD{};
            colourForNRD.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            colourForNRD.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            colourForNRD.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            colourForNRD.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            colourForNRD.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            colourForNRD.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            colourForNRD.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            colourForNRD.image = g_rtOutputImage;
            colourForNRD.subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            colourForNRD.subresourceRange.baseMipLevel = 0u;
            colourForNRD.subresourceRange.levelCount = 1u;
            colourForNRD.subresourceRange.baseArrayLayer = 0u;
            colourForNRD.subresourceRange.layerCount = 1u;

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &colourForNRD
            );

            if(g_nrdMvProbeImage != VK_NULL_HANDLE)
            {
                VkImageMemoryBarrier mvProbeForComposite{};
                mvProbeForComposite.sType =
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                mvProbeForComposite.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                mvProbeForComposite.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                mvProbeForComposite.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                mvProbeForComposite.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                mvProbeForComposite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                mvProbeForComposite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                mvProbeForComposite.image = g_nrdMvProbeImage;
                mvProbeForComposite.subresourceRange.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                mvProbeForComposite.subresourceRange.baseMipLevel = 0u;
                mvProbeForComposite.subresourceRange.levelCount = 1u;
                mvProbeForComposite.subresourceRange.baseArrayLayer = 0u;
                mvProbeForComposite.subresourceRange.layerCount = 1u;

                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0,
                    0, nullptr,
                    0, nullptr,
                    1, &mvProbeForComposite
                );
            }

            vkCmdBindPipeline(
                cmd,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                g_nrdCompositePipeline
            );
            vkCmdBindDescriptorSets(
                cmd,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                g_nrdCompositePipelineLayout,
                0u,
                1u,
                &g_rayTracingDescriptorSet,
                0u,
                nullptr
            );

            NRDCompositePushConstants compositePush{};
            compositePush.width = renderWidth;
            compositePush.height = renderHeight;
            vkCmdPushConstants(
                cmd,
                g_nrdCompositePipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT,
                0u,
                sizeof(compositePush),
                &compositePush
            );
            vkCmdDispatch(
                cmd,
                (renderWidth + 7u) / 8u,
                (renderHeight + 7u) / 8u,
                1u
            );

            VkImageMemoryBarrier colourAfterNRD{};
            colourAfterNRD.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            colourAfterNRD.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            colourAfterNRD.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            colourAfterNRD.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            colourAfterNRD.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            colourAfterNRD.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            colourAfterNRD.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            colourAfterNRD.image = g_rtOutputImage;
            colourAfterNRD.subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            colourAfterNRD.subresourceRange.baseMipLevel = 0u;
            colourAfterNRD.subresourceRange.levelCount = 1u;
            colourAfterNRD.subresourceRange.baseArrayLayer = 0u;
            colourAfterNRD.subresourceRange.layerCount = 1u;

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &colourAfterNRD
            );
        }
    }

    // 2026-11-02：屏幕空间降级链的"上一帧最终色"快照。放在 NRD composite
    // 之后 ⇒ 下一帧 SSR/SSPT 拿到的辐射源是降噪过的色，不是 1spp 噪声。
    // 体积云在 2915 之后才合成，所以历史色里不含云（已知、可接受：
    // 云的贡献在反射里本来就被 RR 当低置信度处理）。
    recordSSHybridHistoryCopy(cmd);

    // 2026-11-01：ReSTIR 屏幕积分必须在本帧 recordVolumetricClouds 里的
    // god_ray_composite **之前**写 g_godRayTargetImage。原来它排在 RR evaluate
    // 之前（帧尾），而消费者在 recordVolumetricClouds 内部 ⇒ 局部光整体滞后一帧：
    // 贴墙、转头时重投影/遮挡都是上一帧的，看起来就像光从墙后拖出来。
    // 这里同时满足两个前提：vol_restir 链已在 recordVolumeRestir() 跑完，
    // 且本帧深度已由 RT pass 写好（composite 要用它算 march 距离）。
    recordVolumeRestirScreenComposite(cmd);

    recordVolumetricClouds(cmd);

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_CLOUD);

    perfL10CpuPhaseAdvance(PERF_L10_CPU_P_DLSS);




    VkImageMemoryBarrier slInputBarriers[7]{};


    auto setupSLReadBarrier =
    [](
        VkImageMemoryBarrier& b,
        VkImage image)
    {
        b.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

        b.oldLayout =
            VK_IMAGE_LAYOUT_GENERAL;

        b.newLayout =
            VK_IMAGE_LAYOUT_GENERAL;

        b.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;

        b.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;

        b.srcAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT;

        b.dstAccessMask =
            VK_ACCESS_MEMORY_READ_BIT;

        b.image =
            image;

        b.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;

        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = 1;

        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = 1;
    };


    setupSLReadBarrier(
        slInputBarriers[0],
        g_rtOutputImage
    );

    setupSLReadBarrier(
        slInputBarriers[1],
        g_dlssDepthImage
    );

    setupSLReadBarrier(
        slInputBarriers[2],
        g_rrDiffuseAlbedoImage
    );

    setupSLReadBarrier(
        slInputBarriers[3],
        g_rrSpecularAlbedoImage
    );

    setupSLReadBarrier(
        slInputBarriers[4],
        g_rrNormalRoughnessImage
    );

    setupSLReadBarrier(
        slInputBarriers[5],
        g_rrSpecularHitDistanceImage
    );

    setupSLReadBarrier(
        slInputBarriers[6],
        g_motionImage
    );


    vkCmdPipelineBarrier(
        cmd,

        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,

        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,

        0,

        0,
        nullptr,

        0,
        nullptr,

        7,
        slInputBarriers
    );



VkImageMemoryBarrier dlssLayoutBarriers[3]{};
uint32_t dlssLayoutBarrierCount = 0;

    if(g_dlssOutputLayout != VK_IMAGE_LAYOUT_GENERAL)
    {
        VkImageMemoryBarrier& b =
            dlssLayoutBarriers[dlssLayoutBarrierCount++];


        b.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;


        b.oldLayout =
            g_dlssOutputLayout;


        b.newLayout =
            VK_IMAGE_LAYOUT_GENERAL;


        b.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;

        b.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;


        b.srcAccessMask =
            0;


        b.dstAccessMask =
            VK_ACCESS_MEMORY_READ_BIT |
            VK_ACCESS_MEMORY_WRITE_BIT;


        b.image =
            g_dlssOutputImage;


        b.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;

        b.subresourceRange.baseMipLevel =
            0;

        b.subresourceRange.levelCount =
            1;

        b.subresourceRange.baseArrayLayer =
            0;

        b.subresourceRange.layerCount =
            1;


        std::cerr
            << "[DLSS OUTPUT LAYOUT]"
            << " "
            << (int)g_dlssOutputLayout
            << " -> GENERAL before Evaluate"
            << std::endl;
    }
if(g_dlssDepthLayout != VK_IMAGE_LAYOUT_GENERAL)
{
    VkImageMemoryBarrier& b =
        dlssLayoutBarriers[dlssLayoutBarrierCount++];

    b.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    b.oldLayout =
        g_dlssDepthLayout;

    b.newLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    b.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    b.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    b.srcAccessMask = 0;

    b.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT |
        VK_ACCESS_MEMORY_WRITE_BIT;

    b.image =
        g_dlssDepthImage;

    b.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
}


if(g_motionLayout != VK_IMAGE_LAYOUT_GENERAL)
{
    VkImageMemoryBarrier& b =
        dlssLayoutBarriers[dlssLayoutBarrierCount++];

    b.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    b.oldLayout =
        g_motionLayout;

    b.newLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    b.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    b.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    b.srcAccessMask = 0;

    b.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT |
        VK_ACCESS_MEMORY_WRITE_BIT;

    b.image =
        g_motionImage;

    b.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
}


if(dlssLayoutBarrierCount > 0)
{
    vkCmdPipelineBarrier(
        cmd,

        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,

        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,

        0,

        0,
        nullptr,

        0,
        nullptr,

        dlssLayoutBarrierCount,
        dlssLayoutBarriers
    );


    g_dlssOutputLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    g_dlssDepthLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    g_motionLayout =
        VK_IMAGE_LAYOUT_GENERAL;
}

    static VkImage motionImageClearedFor = VK_NULL_HANDLE;

    const bool needsMotionClear =
        motionImageClearedFor != g_motionImage;

    VkClearColorValue motionZero{};
    motionZero.float32[0] = 0.0f;
    motionZero.float32[1] = 0.0f;
    motionZero.float32[2] = 0.0f;
    motionZero.float32[3] = 0.0f;


    VkImageSubresourceRange motionRange{};

    motionRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    motionRange.baseMipLevel = 0;
    motionRange.levelCount = 1;

    motionRange.baseArrayLayer = 0;
    motionRange.layerCount = 1;


    if(needsMotionClear)
    {
        vkCmdClearColorImage(
            cmd,
            g_motionImage,
            VK_IMAGE_LAYOUT_GENERAL,
            &motionZero,
            1,
            &motionRange
        );
    }

    VkImageMemoryBarrier motionClearToDLSS{};

    motionClearToDLSS.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    motionClearToDLSS.oldLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    motionClearToDLSS.newLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    motionClearToDLSS.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    motionClearToDLSS.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    motionClearToDLSS.srcAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT;

    motionClearToDLSS.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT;

    motionClearToDLSS.image =
        g_motionImage;

    motionClearToDLSS.subresourceRange =
        motionRange;


    if(needsMotionClear)
    {
        vkCmdPipelineBarrier(
            cmd,

            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,

            0,

            0,
            nullptr,

            0,
            nullptr,

            1,
            &motionClearToDLSS
        );

        motionImageClearedFor =
            g_motionImage;
    }
    if(g_dlssRRReady || g_dlssReady)
    {
        if(!g_currentSLFrameValid ||
           g_currentSLFrameToken == nullptr)
        {
            logFrameSkip(
                "slFrameInvalid",
                std::string("valid=") +
                    (g_currentSLFrameValid ? "1" : "0") +
                " tokenNull=" +
                    ((g_currentSLFrameToken == nullptr) ? "1" : "0") +
                " rrReady=" +
                    (g_dlssRRReady ? "1" : "0") +
                " dlssReady=" +
                    (g_dlssReady ? "1" : "0") +
                " idx=" +
                    std::to_string(g_currentSLFrameIndex)
            );

            return;
        }


        sl::FrameToken* frameToken =
            g_currentSLFrameToken;


        static uint32_t lastDLSSFrameIndex =
            UINT32_MAX;


        if(lastDLSSFrameIndex ==
           g_currentSLFrameIndex)
        {
            logFrameSkip(
                "slFrameIndexFrozen",
                std::string("idx=") +
                    std::to_string(g_currentSLFrameIndex) +
                " last=" +
                    std::to_string(lastDLSSFrameIndex)
            );

            return;
        }


        lastDLSSFrameIndex =
            g_currentSLFrameIndex;


        uint32_t dlssJitterFrameIndex =
            0u;


        sl::float4x4 currentCameraViewToWorld =
        {
            sl::float4(1, 0, 0, 0),
            sl::float4(0, 1, 0, 0),
            sl::float4(0, 0, 1, 0),
            sl::float4(0, 0, 0, 1)
        };

        sl::float4x4 currentClipToCameraView =
            currentCameraViewToWorld;

        sl::float4x4 currentCameraViewToClip =
            currentCameraViewToWorld;


        if(g_cameraUboMapped)
        {
            const CameraUBO* cameraUbo =
                static_cast<const CameraUBO*>(
                    g_cameraUboMapped
                );


            currentCameraViewToWorld =
                JOMLColumnMajorToSLRowMajor(
                    cameraUbo->viewInverse
                );


            currentClipToCameraView =
                JOMLColumnMajorToSLRowMajor(
                    cameraUbo->projInverse
                );


            sl::matrixFullInvert(
                currentCameraViewToClip,
                currentClipToCameraView
            );


            dlssJitterFrameIndex =
                cameraUbo->frameCount;
        }


        sl::float4x4 currentWorldToCameraView{};


        sl::matrixFullInvert(
            currentWorldToCameraView,
            currentCameraViewToWorld
        );


        rrOptions.cameraViewToWorld =
            currentCameraViewToWorld;


        rrOptions.worldToCameraView =
            currentWorldToCameraView;



        float rrMatrixMaxError =
            0.0f;

#if 0
        sl::float4x4 rrMatrixIdentityCheck{};


        sl::matrixMul(
            rrMatrixIdentityCheck,
            rrOptions.worldToCameraView,
            rrOptions.cameraViewToWorld
        );


        const float* rrIdentityValues =
            &rrMatrixIdentityCheck[0].x;


        for(uint32_t i = 0; i < 16; ++i)
        {
            const uint32_t row =
                i / 4;

            const uint32_t column =
                i % 4;


            const float expected =
                row == column
                    ? 1.0f
                    : 0.0f;


            rrMatrixMaxError =
                std::max(
                    rrMatrixMaxError,
                    std::abs(
                        rrIdentityValues[i] -
                        expected
                    )
                );
        }
#endif



        sl::Result rrSetOptionsResult =
            sl::Result::eErrorFeatureNotSupported;


        if(useSR && g_dlssReady && g_slDLSSSetOptions)
        {
            rrSetOptionsResult = g_slDLSSSetOptions(g_dlssViewport, srOptions);
        }
        else if(!useSR && g_dlssRRReady && g_slDLSSDSetOptions)
        {
            rrSetOptionsResult =
                g_slDLSSDSetOptions(
                    g_dlssViewport,
                    rrOptions
                );
        }


        static bool rr32SuccessPrinted =
            false;

        static bool rr32RealCameraPrinted =
            false;

        if(
            rrSetOptionsResult ==
                sl::Result::eOk
        )
        {
            static int upscalerLoggedKey = -1;
            const int upscalerKey =
                (useSR ? 1 : 0) * 100 + g_rrMode.load();
            if(upscalerKey != upscalerLoggedKey)
            {
                upscalerLoggedKey = upscalerKey;
                std::stringstream ss;
                ss << "[UPSCALER] active="
                   << (useSR ? "SR" : "RR")
                   << " upscalerMode=" << g_upscalerMode.load()
                   << " rrMode=" << g_rrMode.load()
                   << " srMode=" << g_srMode.load()
                   << " setOptions=eOk"
                   << " historyValid=" << g_cameraHistoryValid;
                RTLog(ss.str());
            }

            static bool rr52Printed =
    false;

            if(!rr52Printed)
            {
                fprintf(
                    stderr,
                    "[RR-5.2]"
                    " activeSetOptions=RR"
                    " SRSetOptions=disabled\n"
                );

                fflush(stderr);

                rr52Printed =
                    true;
            }
            const float rrCameraMagnitude =
    std::abs(currentCameraViewToWorld[3].x) +
    std::abs(currentCameraViewToWorld[3].y) +
    std::abs(currentCameraViewToWorld[3].z);


            if(
                !rr32RealCameraPrinted &&
                rrCameraMagnitude > 0.001f
            )
            {
                fprintf(
                    stderr,
                    "[RR-3.2 REAL CAMERA]"
                    " inverseError=%.9g"
                    " cameraPos=(%.6f,%.6f,%.6f)\n",

                    static_cast<double>(
                        rrMatrixMaxError
                    ),

                    static_cast<double>(
                        currentCameraViewToWorld[3].x
                    ),

                    static_cast<double>(
                        currentCameraViewToWorld[3].y
                    ),

                    static_cast<double>(
                        currentCameraViewToWorld[3].z
                    )
                );

                fflush(stderr);

                rr32RealCameraPrinted =
                    true;
            }
            if(!rr32SuccessPrinted)
            {
                fprintf(
                    stderr,
                    "[RR-3.2]"
                    " SetOptions=0"
                    " inverseError=%.9g"
                    " cameraPos=(%.6f,%.6f,%.6f)"
                    "\n",

                    static_cast<double>(
                        rrMatrixMaxError
                    ),

                    static_cast<double>(
                        currentCameraViewToWorld[3].x
                    ),

                    static_cast<double>(
                        currentCameraViewToWorld[3].y
                    ),

                    static_cast<double>(
                        currentCameraViewToWorld[3].z
                    )
                );


                fflush(
                    stderr
                );


                rr32SuccessPrinted =
                    true;
            }
        }
        else
        {
            fprintf(
                stderr,
                "[RR-3.2]"
                " SetOptions FAILED"
                " result=%d"
                " inverseError=%.9g"
                "\n",

                static_cast<int>(
                    rrSetOptionsResult
                ),

                static_cast<double>(
                    rrMatrixMaxError
                )
            );


            fflush(
                stderr
            );
        }


        if(rrMatrixMaxError > 1.0e-3f)
        {
            fprintf(
                stderr,
                "[RR-3.2]"
                " MATRIX VALIDATION FAILED"
                " inverseError=%.9g"
                "\n",

                static_cast<double>(
                    rrMatrixMaxError
                )
            );


            fflush(
                stderr
            );
        }


        float dlssJitterX =
            0.0f;

        float dlssJitterY =
            0.0f;


        GetDLSSFrameJitter(
            dlssJitterFrameIndex,
            dlssJitterX,
            dlssJitterY
        );
        sl::Constants constants{};


        constants.cameraViewToClip =
    currentCameraViewToClip;

        constants.clipToCameraView =
            currentClipToCameraView;


        if(g_hasPrevCameraMatrices)
        {
            sl::float4x4 cameraViewToPrevCameraView{};

            sl::calcCameraToPrevCamera(
                cameraViewToPrevCameraView,
                currentCameraViewToWorld,
                g_prevCameraViewToWorld
            );


            sl::float4x4 clipToPrevCameraView{};

            sl::matrixMul(
                clipToPrevCameraView,
                currentClipToCameraView,
                cameraViewToPrevCameraView
            );


            sl::matrixMul(
                constants.clipToPrevClip,
                clipToPrevCameraView,
                g_prevCameraViewToClip
            );


            sl::matrixFullInvert(
                constants.prevClipToClip,
                constants.clipToPrevClip
            );
        }
        else
        {
            for(int i = 0; i < 4; i++)
            {
                constants.clipToPrevClip[i] =
                    sl::float4(
                        i == 0 ? 1.0f : 0.0f,
                        i == 1 ? 1.0f : 0.0f,
                        i == 2 ? 1.0f : 0.0f,
                        i == 3 ? 1.0f : 0.0f
                    );

                constants.prevClipToClip[i] =
                    constants.clipToPrevClip[i];
            }
        }


        constants.jitterOffset =
            sl::float2(
                -dlssJitterX,
                -dlssJitterY
            );
        constants.cameraPinholeOffset =
    sl::float2(
        0.0f,
        0.0f
    );


        constants.cameraPos =
            sl::float3(
                currentCameraViewToWorld[3].x,
                currentCameraViewToWorld[3].y,
                currentCameraViewToWorld[3].z
            );


        constants.cameraRight =
            sl::float3(
                currentCameraViewToWorld[0].x,
                currentCameraViewToWorld[0].y,
                currentCameraViewToWorld[0].z
            );


        constants.cameraUp =
            sl::float3(
                currentCameraViewToWorld[1].x,
                currentCameraViewToWorld[1].y,
                currentCameraViewToWorld[1].z
            );


        constants.cameraFwd =
            sl::float3(
                -currentCameraViewToWorld[2].x,
                -currentCameraViewToWorld[2].y,
                -currentCameraViewToWorld[2].z
            );

        if(g_cameraUboMapped)
        {
            const CameraUBO* cameraUbo =
                static_cast<const CameraUBO*>(
                    g_cameraUboMapped
                );

            constants.cameraNear =
                cameraUbo->cameraNear;

            constants.cameraFar =
                cameraUbo->cameraFar;

            constants.cameraFOV =
                cameraUbo->cameraFov;
        }


        constants.cameraAspectRatio =
            g_rtOutputHeight != 0u
                ? static_cast<float>(g_rtOutputWidth) /
                      static_cast<float>(g_rtOutputHeight)
                : static_cast<float>(outputWidth) /
                      static_cast<float>(outputHeight);


        {
            const uint32_t mvMode = g_mvScaleMode.load();
            const bool haveSize = g_motionWidth > 0u && g_motionHeight > 0u;
            const float invW = haveSize
                ? 1.0f / static_cast<float>(g_motionWidth) : 1.0f;
            const float invH = haveSize
                ? 1.0f / static_cast<float>(g_motionHeight) : 1.0f;

            constants.mvecScale =
                (mvMode == 2u)
                    ? sl::float2(invW, -invH)
                    : ((mvMode == 3u)
                           ? sl::float2(invW, invH)
                           : sl::float2(1.0f, 1.0f));
        }


        constants.depthInverted =
            sl::Boolean::eTrue;

        const bool mvOutputEnabled =
            g_motionVectorsEnabled.load() != 0u;

        constants.cameraMotionIncluded =
            mvOutputEnabled
                ? sl::Boolean::eTrue
                : sl::Boolean::eFalse;

        constants.motionVectors3D =
            sl::Boolean::eFalse;

        constants.motionVectorsJittered =
            sl::Boolean::eFalse;

        constants.motionVectorsDilated =
            sl::Boolean::eFalse;

        static uint32_t lastResetOutputWidth = 0u;
        static uint32_t lastResetOutputHeight = 0u;
        static int lastResetUpscalerMode = -1;
        static int lastResetRRMode = -1;
        static int lastResetMvOutput = -1;

        const int resetUpscalerMode = g_upscalerMode.load();
        const int resetRRMode = g_rrMode.load();
        const int resetMvOutput = mvOutputEnabled ? 1 : 0;

        const bool resetByShapeChange =
            lastResetOutputWidth != outputWidth ||
            lastResetOutputHeight != outputHeight ||
            lastResetUpscalerMode != resetUpscalerMode ||
            lastResetRRMode != resetRRMode ||
            lastResetMvOutput != resetMvOutput;

        lastResetOutputWidth = outputWidth;
        lastResetOutputHeight = outputHeight;
        lastResetUpscalerMode = resetUpscalerMode;
        lastResetRRMode = resetRRMode;
        lastResetMvOutput = resetMvOutput;

        if(resetByShapeChange)
        {
            g_fgForceOffFrames.store(2u);
        }

        const bool sparseRateChanged =
            g_sparseRateHistoryReset.exchange(false, std::memory_order_relaxed);

        constants.reset =
            (!g_hasPrevCameraMatrices || resetByShapeChange || sparseRateChanged)
                ? sl::Boolean::eTrue
                : sl::Boolean::eFalse;


        sl::Result constantsResult =
            g_slSetConstants(
                constants,
                *frameToken,
                g_dlssViewport
            );



        if(constantsResult != sl::Result::eOk)
        {
            fprintf(
                stderr,
                "[RR-3.4] SetConstants FAILED result=%d\n",
                static_cast<int>(
                    constantsResult
                )
            );

            fflush(stderr);

            { logFrameSkip("ret@53145", "auto"); return; }
        }


        const bool rr34SizeMatch =
            g_dlssDepthWidth ==
                g_rtOutputWidth &&
            g_dlssDepthHeight ==
                g_rtOutputHeight &&
            g_motionWidth ==
                g_rtOutputWidth &&
            g_motionHeight ==
                g_rtOutputHeight;


        static bool rr34Printed =
            false;


        if(!rr34Printed)
        {
            fprintf(
                stderr,
                "[RR-3.4]"
                " Constants=0"
                " depth=%ux%u"
                " motion=%ux%u"
                " input=%ux%u"
                " sizeMatch=%d"
                " reversedZ=1"
                " cameraMotionIncluded=1"
                " mvec3D=0"
                " jitteredMV=0"
                " dilatedMV=0"
                " jitter=(%.6f,%.6f)"
                " near=%.6f"
                " far=%.6f"
                " fov=%.6f\n",

                g_dlssDepthWidth,
                g_dlssDepthHeight,

                g_motionWidth,
                g_motionHeight,

                g_rtOutputWidth,
                g_rtOutputHeight,

                rr34SizeMatch
                    ? 1
                    : 0,

                static_cast<double>(
                    constants.jitterOffset.x
                ),

                static_cast<double>(
                    constants.jitterOffset.y
                ),

                static_cast<double>(
                    constants.cameraNear
                ),

                static_cast<double>(
                    constants.cameraFar
                ),

                static_cast<double>(
                    constants.cameraFOV
                )
            );

            fflush(stderr);

            rr34Printed =
                true;
        }
sl::Resource dlssInput(
    sl::ResourceType::eTex2d,
    (void*)g_rtOutputImage,
    (void*)g_rtOutputMemory,
    (void*)g_rtOutputView,
    VK_IMAGE_LAYOUT_GENERAL
);

dlssInput.width =
    g_rtOutputWidth;

dlssInput.height =
    g_rtOutputHeight;

dlssInput.nativeFormat =
    static_cast<uint32_t>(
        VK_FORMAT_R16G16B16A16_SFLOAT
    );

dlssInput.mipLevels = 1;
dlssInput.arrayLayers = 1;
dlssInput.flags = 0;

dlssInput.usage =
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;



sl::Resource dlssOutput(
    sl::ResourceType::eTex2d,
    (void*)g_dlssOutputImage,
    (void*)g_dlssOutputMemory,
    (void*)g_dlssOutputView,
    VK_IMAGE_LAYOUT_GENERAL
);

dlssOutput.width =
    g_dlssOutputWidth;

dlssOutput.height =
    g_dlssOutputHeight;

dlssOutput.nativeFormat =
    static_cast<uint32_t>(
        VK_FORMAT_R16G16B16A16_SFLOAT
    );

dlssOutput.mipLevels = 1;
dlssOutput.arrayLayers = 1;
dlssOutput.flags = 0;

dlssOutput.usage =
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;



sl::Resource dlssDepth(
    sl::ResourceType::eTex2d,
    (void*)g_dlssDepthImage,
    (void*)g_dlssDepthMemory,
    (void*)g_dlssDepthView,
    VK_IMAGE_LAYOUT_GENERAL
);

dlssDepth.width =
    g_dlssDepthWidth;

dlssDepth.height =
    g_dlssDepthHeight;

dlssDepth.nativeFormat =
    static_cast<uint32_t>(
        VK_FORMAT_R32_SFLOAT
    );

dlssDepth.mipLevels = 1;
dlssDepth.arrayLayers = 1;
dlssDepth.flags = 0;

dlssDepth.usage =
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;



sl::Resource dlssMotion(
    sl::ResourceType::eTex2d,
    (void*)g_motionImage,
    (void*)g_motionMemory,
    (void*)g_motionView,
    VK_IMAGE_LAYOUT_GENERAL
);

dlssMotion.width =
    g_motionWidth;

dlssMotion.height =
    g_motionHeight;

dlssMotion.nativeFormat =
    static_cast<uint32_t>(
        VK_FORMAT_R16G16_SFLOAT
    );

dlssMotion.mipLevels = 1;
dlssMotion.arrayLayers = 1;
dlssMotion.flags = 0;

dlssMotion.usage =
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;



sl::Resource rrDiffuseAlbedo(
    sl::ResourceType::eTex2d,
    (void*)g_rrDiffuseAlbedoImage,
    (void*)g_rrDiffuseAlbedoMemory,
    (void*)g_rrDiffuseAlbedoView,
    VK_IMAGE_LAYOUT_GENERAL
);

rrDiffuseAlbedo.width =
    g_rrGBufferWidth;

rrDiffuseAlbedo.height =
    g_rrGBufferHeight;

rrDiffuseAlbedo.nativeFormat =
    static_cast<uint32_t>(
        VK_FORMAT_R16G16B16A16_SFLOAT
    );

rrDiffuseAlbedo.mipLevels = 1;
rrDiffuseAlbedo.arrayLayers = 1;
rrDiffuseAlbedo.flags = 0;

rrDiffuseAlbedo.usage =
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;



sl::Resource rrSpecularAlbedo(
    sl::ResourceType::eTex2d,
    (void*)g_rrSpecularAlbedoImage,
    (void*)g_rrSpecularAlbedoMemory,
    (void*)g_rrSpecularAlbedoView,
    VK_IMAGE_LAYOUT_GENERAL
);

rrSpecularAlbedo.width =
    g_rrGBufferWidth;

rrSpecularAlbedo.height =
    g_rrGBufferHeight;

rrSpecularAlbedo.nativeFormat =
    static_cast<uint32_t>(
        VK_FORMAT_R16G16B16A16_SFLOAT
    );

rrSpecularAlbedo.mipLevels = 1;
rrSpecularAlbedo.arrayLayers = 1;
rrSpecularAlbedo.flags = 0;

rrSpecularAlbedo.usage =
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;



sl::Resource rrNormalRoughness(
    sl::ResourceType::eTex2d,
    (void*)g_rrNormalRoughnessImage,
    (void*)g_rrNormalRoughnessMemory,
    (void*)g_rrNormalRoughnessView,
    VK_IMAGE_LAYOUT_GENERAL
);

rrNormalRoughness.width =
    g_rrGBufferWidth;

rrNormalRoughness.height =
    g_rrGBufferHeight;

rrNormalRoughness.nativeFormat =
    static_cast<uint32_t>(
        VK_FORMAT_R16G16B16A16_SFLOAT
    );

rrNormalRoughness.mipLevels = 1;
rrNormalRoughness.arrayLayers = 1;
rrNormalRoughness.flags = 0;

rrNormalRoughness.usage =
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;



sl::Resource rrSpecularHitDistance(
    sl::ResourceType::eTex2d,
    (void*)g_rrSpecularHitDistanceImage,
    (void*)g_rrSpecularHitDistanceMemory,
    (void*)g_rrSpecularHitDistanceView,
    VK_IMAGE_LAYOUT_GENERAL
);

rrSpecularHitDistance.width =
    g_rrGBufferWidth;

rrSpecularHitDistance.height =
    g_rrGBufferHeight;

rrSpecularHitDistance.nativeFormat =
    static_cast<uint32_t>(
        VK_FORMAT_R32_SFLOAT
    );

rrSpecularHitDistance.mipLevels = 1;
rrSpecularHitDistance.arrayLayers = 1;
rrSpecularHitDistance.flags = 0;

rrSpecularHitDistance.usage =
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;


sl::Resource rrTransparencyHint(
    sl::ResourceType::eTex2d,
    (void*)g_rrTransparencyHintImage,
    (void*)g_rrTransparencyHintMemory,
    (void*)g_rrTransparencyHintView,
    VK_IMAGE_LAYOUT_GENERAL
);

rrTransparencyHint.width =
    g_rrGBufferWidth;

rrTransparencyHint.height =
    g_rrGBufferHeight;

rrTransparencyHint.nativeFormat =
    static_cast<uint32_t>(
        VK_FORMAT_R8_UNORM
    );

rrTransparencyHint.mipLevels = 1;
rrTransparencyHint.arrayLayers = 1;
rrTransparencyHint.flags = 0;

rrTransparencyHint.usage =
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;




        {
            static VkImageView rrHintBoundView = VK_NULL_HANDLE;
            if(rrHintBoundView != g_rrTransparencyHintView)
            {
                updateRRTransparencyHintRayDescriptor();
                rrHintBoundView = g_rrTransparencyHintView;
            }
        }

        sl::Extent inputExtent{};

        inputExtent.left = 0;
        inputExtent.top = 0;

        inputExtent.width =
            g_rtOutputWidth;

        inputExtent.height =
            g_rtOutputHeight;


        sl::ResourceTag motionTag(
            &dlssMotion,
            sl::kBufferTypeMotionVectors,
            sl::eValidUntilEvaluate,
            &inputExtent
        );


        sl::ResourceTag depthTag(
            &dlssDepth,
            sl::kBufferTypeDepth,
            sl::eValidUntilEvaluate,
            &inputExtent
        );



sl::ResourceTag rrDiffuseAlbedoTag(
    &rrDiffuseAlbedo,
    sl::kBufferTypeAlbedo,
    sl::eValidUntilEvaluate,
    &inputExtent
);

sl::ResourceTag rrSpecularAlbedoTag(
    &rrSpecularAlbedo,
    sl::kBufferTypeSpecularAlbedo,
    sl::eValidUntilEvaluate,
    &inputExtent
);

sl::ResourceTag rrNormalRoughnessTag(
    &rrNormalRoughness,
    sl::kBufferTypeNormalRoughness,
    sl::eValidUntilEvaluate,
    &inputExtent
);

sl::ResourceTag rrSpecularHitDistanceTag(
    &rrSpecularHitDistance,
    sl::kBufferTypeSpecularHitDistance,
    sl::eValidUntilEvaluate,
    &inputExtent
);

sl::ResourceTag rrTransparencyHintTag(
    &rrTransparencyHint,
    sl::kBufferTypeTransparencyHint,
    sl::eValidUntilEvaluate,
    &inputExtent
);



static bool rr33Printed =
    false;

if(!rr33Printed)
{
    const bool rrGuideSizesValid =
        g_rrGBufferWidth ==
            g_rtOutputWidth &&
        g_rrGBufferHeight ==
            g_rtOutputHeight;


    fprintf(
        stderr,
        "[RR-3.3]"
        " guides=%ux%u"
        " input=%ux%u"
        " sizeMatch=%d"
        " tags=Albedo,SpecAlbedo,NormalRoughness,SpecHitDistance\n",

        g_rrGBufferWidth,
        g_rrGBufferHeight,

        g_rtOutputWidth,
        g_rtOutputHeight,

        rrGuideSizesValid
            ? 1
            : 0
    );

    fflush(stderr);

    rr33Printed =
        true;
}



sl::Extent outputExtent{};

outputExtent.left = 0;
outputExtent.top = 0;

outputExtent.width =
    g_dlssOutputWidth;

outputExtent.height =
    g_dlssOutputHeight;



sl::ResourceTag inputTag(
    &dlssInput,
    sl::kBufferTypeScalingInputColor,
    sl::eValidUntilEvaluate,
    &inputExtent
);

sl::ResourceTag outputTag(
    &dlssOutput,
    sl::kBufferTypeScalingOutputColor,
    sl::eValidUntilEvaluate,
    &outputExtent
);




        const sl::BaseStructure* inputs[] =
        {
            &inputTag, &outputTag, &depthTag, &motionTag,
            &rrDiffuseAlbedoTag, &rrSpecularAlbedoTag,
            &rrNormalRoughnessTag, &rrSpecularHitDistanceTag,
            &g_dlssViewport,
            &rrTransparencyHintTag
        };
        const uint32_t inputCount = useSR
            ? 5u
            : (rrTransparencyHintEnabled() ? 10u : 9u);


        sl::CommandBuffer* slCmd =
            reinterpret_cast<sl::CommandBuffer*>(cmd);


        if(
            ((!useSR && !g_dlssRRReady) || (useSR && !g_dlssReady)) ||
            !g_slEvaluateFeature ||
            rrSetOptionsResult != sl::Result::eOk
        )
        {
            static bool hasLoggedRRSkip = false;
            static int lastRRSkipReady = -1;
            static int lastRRSkipEvaluate = -1;
            static int lastRRSkipSetOptions = std::numeric_limits<int>::min();
            const int ready = g_dlssRRReady ? 1 : 0;
            const int evaluate = g_slEvaluateFeature ? 1 : 0;
            const int setOptions = static_cast<int>(rrSetOptionsResult);
            if(!hasLoggedRRSkip ||
               ready != lastRRSkipReady ||
               evaluate != lastRRSkipEvaluate ||
               setOptions != lastRRSkipSetOptions)
            {
                fprintf(
                    stderr,
                    "[DLSS-4.1] SKIP ready=%d evaluate=%d setOptions=%d\n",
                    ready,
                    evaluate,
                    setOptions
                );
                hasLoggedRRSkip = true;
                lastRRSkipReady = ready;
                lastRRSkipEvaluate = evaluate;
                lastRRSkipSetOptions = setOptions;
            }

            { logFrameSkip("ret@53694", "auto"); return; }
        }


        if(!useSR)
        {
            recordRRGuideRelax(cmd);
        }


        sl::Result dlssResult =
            g_slEvaluateFeature(
                useSR ? sl::kFeatureDLSS : sl::kFeatureDLSS_RR,
                *frameToken,
                inputs,
                inputCount,
                slCmd
            );


        static bool hasLoggedRREvaluateResult = false;
        static sl::Result lastRREvaluateResult =
            sl::Result::eErrorFeatureNotSupported;
        if(!hasLoggedRREvaluateResult ||
           dlssResult != lastRREvaluateResult ||
           (g_perfL10FrameSerial % 120u) == 0u)
        {
            fprintf(
                stderr,
                "[DLSS-4.1] Evaluate result=%d render=%ux%u output=%ux%u\n",
                static_cast<int>(dlssResult),
                g_rtOutputWidth,
                g_rtOutputHeight,
                g_dlssOutputWidth,
                g_dlssOutputHeight
            );
            hasLoggedRREvaluateResult = true;
            lastRREvaluateResult = dlssResult;
        }



        vkCmdBindPipeline(
            cmd,
            VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
            g_rayTracingPipeline
        );


        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
            g_rayTracingPipelineLayout,
            0,
            1,
            &g_rayTracingDescriptorSet,
            0,
            nullptr
        );


        static bool rr42RestorePrinted =
            false;


        if(!rr42RestorePrinted)
        {
            fprintf(
                stderr,
                "[RR-4.2]"
                " Vulkan state restored"
                " pipeline=RT"
                " descriptors=1\n"
            );

            fflush(stderr);

            rr42RestorePrinted =
                true;
        }

    recordCloudComposite(cmd);


        if(dlssResult != sl::Result::eOk)
        {
            static bool oomWarnPrinted = false;

            const bool outOfVram =
                dlssResult == sl::Result::eWarnOutOfVRAM;

            if(outOfVram)
            {
                if(!oomWarnPrinted)
                {
                    oomWarnPrinted = true;

                    fprintf(
                        stderr,
                        "[DLSS] out of VRAM (eWarnOutOfVRAM): keeping the last good "
                        "output instead of dropping the frame\n"
                    );

                    fflush(stderr);
                }
            }
            else
            {
                fprintf(
                    stderr,
                    "[DLSS] Evaluate FAILED result=%d (0x%X)\n",
                    static_cast<int>(dlssResult),
                    static_cast<unsigned int>(dlssResult)
                );

                logFrameSkip(
                    "dlssEvaluateFailed",
                    std::to_string(static_cast<int>(dlssResult))
                );

                return;
            }
        }


        g_prevCameraViewToWorld =
            currentCameraViewToWorld;

        g_prevCameraViewToClip =
            currentCameraViewToClip;

        g_hasPrevCameraMatrices =
            true;


writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_DLSS);

perfL10CpuPhaseAdvance(PERF_L10_CPU_P_POST);

if(g_dlssOutputLayout != VK_IMAGE_LAYOUT_GENERAL)
{
    logFrameSkip(
        "dlssOutputLayout",
        std::string("layout=") +
            std::to_string(static_cast<int>(g_dlssOutputLayout))
    );

    return;
}


const bool hdrPresentation =
    hdrOutputActive() &&
    g_hdrPostPipeline != VK_NULL_HANDLE;

VkPipeline displayPipeline =
    hdrPresentation
        ? g_hdrPostPipeline
        : g_sdrPostPipeline;

if(
    displayPipeline ==
        VK_NULL_HANDLE ||

    g_sdrPostPipelineLayout ==
        VK_NULL_HANDLE ||

    g_sdrPostDescriptorSet ==
        VK_NULL_HANDLE
)
{
    fprintf(
        stderr,
        "[RR-6.5.2] display post unavailable\n"
    );

    logFrameSkip(
        "displayPostUnavailable",
        std::string("hdr=") +
            (hdrPresentation ? "1" : "0") +
        " pipeDefined=" +
            ((displayPipeline != VK_NULL_HANDLE) ? "1" : "0") +
        " layoutDefined=" +
            ((g_sdrPostPipelineLayout != VK_NULL_HANDLE) ? "1" : "0") +
        " descDefined=" +
            ((g_sdrPostDescriptorSet != VK_NULL_HANDLE) ? "1" : "0")
    );

    return;
}


if(hdrPresentation != g_hdrPresentationWasActive)
{
    fprintf(
        stderr,
        "[RR-6.6] presentation=%s"
        " requested=%d surfaceHDR10=%d\n",

        hdrPresentation
            ? "HDR10/PQ"
            : "SDR/gamma2.2",

        g_hdrRequested.load() ? 1 : 0,
        g_hdrSurfaceSupported.load() ? 1 : 0
    );

    fflush(stderr);

    g_hdrPresentationWasActive =
        hdrPresentation;
}


VkImageMemoryBarrier rrToSDRPost{};

rrToSDRPost.sType =
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

rrToSDRPost.oldLayout =
    VK_IMAGE_LAYOUT_GENERAL;

rrToSDRPost.newLayout =
    VK_IMAGE_LAYOUT_GENERAL;

rrToSDRPost.srcQueueFamilyIndex =
    VK_QUEUE_FAMILY_IGNORED;

rrToSDRPost.dstQueueFamilyIndex =
    VK_QUEUE_FAMILY_IGNORED;

rrToSDRPost.srcAccessMask =
    VK_ACCESS_MEMORY_WRITE_BIT;

rrToSDRPost.dstAccessMask =
    VK_ACCESS_SHADER_READ_BIT |
    VK_ACCESS_SHADER_WRITE_BIT;

rrToSDRPost.image =
    g_dlssOutputImage;

rrToSDRPost.subresourceRange.aspectMask =
    VK_IMAGE_ASPECT_COLOR_BIT;

rrToSDRPost.subresourceRange.baseMipLevel = 0;
rrToSDRPost.subresourceRange.levelCount = 1;

rrToSDRPost.subresourceRange.baseArrayLayer = 0;
rrToSDRPost.subresourceRange.layerCount = 1;


VkImageMemoryBarrier rrStableToPost{};

rrStableToPost.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
rrStableToPost.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
rrStableToPost.newLayout = VK_IMAGE_LAYOUT_GENERAL;
rrStableToPost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
rrStableToPost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
rrStableToPost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
rrStableToPost.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
rrStableToPost.image = g_rrStableRadianceImage;
rrStableToPost.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
rrStableToPost.subresourceRange.baseMipLevel = 0;
rrStableToPost.subresourceRange.levelCount = 1;
rrStableToPost.subresourceRange.baseArrayLayer = 0;
rrStableToPost.subresourceRange.layerCount = 1;


VkImageMemoryBarrier preDisplayBarriers[2] =
{
    rrToSDRPost,
    rrStableToPost
};

const uint32_t preDisplayBarrierCount =
    (g_rrStableRadianceImage != VK_NULL_HANDLE)
        ? 2u
        : 1u;


vkCmdPipelineBarrier(
    cmd,

    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,

    0,

    0,
    nullptr,

    0,
    nullptr,

    preDisplayBarrierCount,
    preDisplayBarriers
);



{
    const bool aeActive =
        g_aeHistogramPipeline != VK_NULL_HANDLE &&
        g_aeAdaptPipeline != VK_NULL_HANDLE &&
        g_aeHistogramSet != VK_NULL_HANDLE &&
        g_aeAdaptSet != VK_NULL_HANDLE &&
        g_aeHistogramBuffer != VK_NULL_HANDLE &&
        g_aeStateBuffer != VK_NULL_HANDLE;

    if(aeActive)
    {
        const std::chrono::steady_clock::time_point aeNow =
            std::chrono::steady_clock::now();

        float aeDtSec =
            0.0f;

        if(g_aeHasLastTick)
        {
            aeDtSec =
                std::chrono::duration_cast<std::chrono::duration<float>>(
                    aeNow - g_aeLastTick
                ).count();
        }

        g_aeLastTick =
            aeNow;

        g_aeHasLastTick =
            true;

        aeDtSec =
            aeDtSec < 0.0f
                ? 0.0f
                : (
                    aeDtSec > 0.1f
                        ? 0.1f
                        : aeDtSec
                );



        VkBufferMemoryBarrier aePrevious[2]{};

        for(int i = 0; i < 2; ++i)
        {
            aePrevious[i].sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;

            aePrevious[i].srcAccessMask =
                VK_ACCESS_SHADER_WRITE_BIT;

            aePrevious[i].dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_SHADER_WRITE_BIT;

            aePrevious[i].srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;

            aePrevious[i].dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;

            aePrevious[i].offset =
                0;

            aePrevious[i].size =
                VK_WHOLE_SIZE;
        }

        aePrevious[0].buffer =
            g_aeHistogramBuffer;

        aePrevious[1].buffer =
            g_aeStateBuffer;


        vkCmdPipelineBarrier(
            cmd,

            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,

            0,

            0,
            nullptr,

            2,
            aePrevious,

            0,
            nullptr
        );


        AePushConstants aePush{};

        aePush.dtSec =
            aeDtSec;

        aePush.keyEV =
            AE_DEFAULT_KEY_EV +
            g_aeCompensationEV.load();

        aePush.minEV =
            g_aeMinEV;

        aePush.maxEV =
            g_aeMaxEV;

        aePush.adaptUpRate =
            g_aeAdaptUpRate;

        aePush.adaptDownRate =
            g_aeAdaptDownRate;

        aePush.trimLow =
            g_aeTrimLow;

        aePush.trimHigh =
            g_aeTrimHigh;

        aePush.enabled =
            g_aeEnabled.load()
                ? 1
                : 0;

        aePush.displayWhiteScale =
            hdrOutputActive()
                ? AE_HDR_CLOUD_WHITE_SCALE
                : 1.0f;


        vkCmdBindPipeline(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_aeAdaptPipeline
        );


        vkCmdBindDescriptorSets(
            cmd,

            VK_PIPELINE_BIND_POINT_COMPUTE,

            g_aeAdaptLayout,

            0,
            1,
            &g_aeAdaptSet,

            0,
            nullptr
        );


        vkCmdPushConstants(
            cmd,

            g_aeAdaptLayout,

            VK_SHADER_STAGE_COMPUTE_BIT,

            0,

            sizeof(AePushConstants),

            &aePush
        );


        vkCmdDispatch(
            cmd,
            1,
            1,
            1
        );



        VkBufferMemoryBarrier aePostAdapt[2]{};

        for(int i = 0; i < 2; ++i)
        {
            aePostAdapt[i].sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;

            aePostAdapt[i].srcAccessMask =
                VK_ACCESS_SHADER_WRITE_BIT;

            aePostAdapt[i].dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT |
                VK_ACCESS_SHADER_WRITE_BIT;

            aePostAdapt[i].srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;

            aePostAdapt[i].dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;

            aePostAdapt[i].offset =
                0;

            aePostAdapt[i].size =
                VK_WHOLE_SIZE;
        }

        aePostAdapt[0].buffer =
            g_aeHistogramBuffer;

        aePostAdapt[1].buffer =
            g_aeStateBuffer;


        vkCmdPipelineBarrier(
            cmd,

            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,

            0,

            0,
            nullptr,

            2,
            aePostAdapt,

            0,
            nullptr
        );


        if(g_aeEnabled.load())
        {
            vkCmdBindPipeline(
                cmd,
                VK_PIPELINE_BIND_POINT_COMPUTE,
                g_aeHistogramPipeline
            );


            vkCmdBindDescriptorSets(
                cmd,

                VK_PIPELINE_BIND_POINT_COMPUTE,

                g_aeHistogramLayout,

                0,
                1,
                &g_aeHistogramSet,

                0,
                nullptr
            );


            vkCmdDispatch(
                cmd,

                (
                    g_dlssOutputWidth +
                    63
                ) / 64,

                (
                    g_dlssOutputHeight +
                    63
                ) / 64,

                1
            );
        }


        if(g_aeStateCpu != nullptr &&
           (g_perfL10FrameSerial % 120u) == 0u)
        {
            std::ostringstream aeStateLog;

            aeStateLog
                << "[RR-7 STATE]"
                << " enabled=" << (g_aeEnabled.load() ? 1 : 0)
                << " currentEV=" << g_aeStateCpu[0]
                << " targetEV=" << g_aeStateCpu[1]
                << " avgLogLuma=" << g_aeStateCpu[2]
                << " samples="
                << reinterpret_cast<const uint32_t*>(g_aeStateCpu)[3]
                << " maxEV=" << g_aeMaxEV;

            RTLog(aeStateLog.str());
        }


        if(!g_aeReported)
        {
            fprintf(
                stderr,
                "[RR-7] auto exposure ACTIVE"
                " enabled=%d keyEV=%.3f range=[%.2f,%.2f]\n",

                g_aeEnabled.load() ? 1 : 0,
                double(
                    AE_DEFAULT_KEY_EV +
                    g_aeCompensationEV.load()
                ),
                double(g_aeMinEV),
                double(g_aeMaxEV)
            );

            fflush(stderr);

            g_aeReported =
                true;
        }
    }
}


recordBloom(cmd);

vkCmdBindPipeline(
    cmd,
    VK_PIPELINE_BIND_POINT_COMPUTE,
    displayPipeline
);


vkCmdBindDescriptorSets(
    cmd,

    VK_PIPELINE_BIND_POINT_COMPUTE,

    g_sdrPostPipelineLayout,

    0,
    1,
    &g_sdrPostDescriptorSet,

    0,
    nullptr
);


{
    PostProcessingPush postPush{};
    postPush.tonemapMode = g_postTonemapMode.load();
    postPush.exposureStrength = g_postExposureStrength.load();
    postPush.contrast = g_postContrast.load();
    postPush.saturation = g_postSaturation.load();
    postPush.gamma = g_postGamma.load();
    postPush.gainR = g_postGainR.load();
    postPush.gainG = g_postGainG.load();
    postPush.gainB = g_postGainB.load();
    postPush.manualEV = g_cgExposureEV.load();
    postPush.blackPoint = g_cgBlackPoint.load();
    postPush.whitePoint = g_cgWhitePoint.load();
    postPush.gradeContrast = g_cgContrast.load();
    postPush.gradePivot = g_cgPivot.load();
    postPush.liftR = g_cgLiftR.load();
    postPush.liftG = g_cgLiftG.load();
    postPush.liftB = g_cgLiftB.load();
    postPush.gradeGammaR = g_cgGammaR.load();
    postPush.gradeGammaG = g_cgGammaG.load();
    postPush.gradeGammaB = g_cgGammaB.load();
    postPush.gradeGainR = g_cgGainR.load();
    postPush.gradeGainG = g_cgGainG.load();
    postPush.gradeGainB = g_cgGainB.load();
    postPush.shadowHue = g_cgShadowHue.load();
    postPush.shadowStrength = g_cgShadowStrength.load();
    postPush.highlightHue = g_cgHighlightHue.load();
    postPush.highlightStrength = g_cgHighlightStrength.load();
    postPush.vibrance = g_cgVibrance.load();
    postPush.fade = g_cgFade.load();
    postPush.vignette = g_cgVignette.load();
    postPush.vignetteRadius = g_cgVignetteRadius.load();
    postPush.hueRotate = g_cgHueRotate.load();
    postPush.tintGreen = g_cgTintGreen.load();
    vkCmdPushConstants(
        cmd,
        g_sdrPostPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0u,
        static_cast<uint32_t>(sizeof(postPush)),
        &postPush
    );
}

vkCmdDispatch(
    cmd,

    (
        g_dlssOutputWidth +
        7
    ) / 8,

    (
        g_dlssOutputHeight +
        7
    ) / 8,

    1
);


writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SDR);

static bool rr65PostPrinted =
    false;


if(!rr65PostPrinted)
{
    fprintf(
        stderr,
        "[RR-6.5.2]"
        " postTransfer=%s"
        " size=%ux%u\n",

        hdrPresentation
            ? "ST2084_PQ"
            : "pow(1/2.2)",

        g_dlssOutputWidth,
        g_dlssOutputHeight
    );

    fflush(stderr);

    rr65PostPrinted =
        true;
}



VkImageMemoryBarrier dlssToTransfer{};

dlssToTransfer.sType =
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

dlssToTransfer.oldLayout =
    VK_IMAGE_LAYOUT_GENERAL;

dlssToTransfer.newLayout =
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

dlssToTransfer.srcQueueFamilyIndex =
    VK_QUEUE_FAMILY_IGNORED;

dlssToTransfer.dstQueueFamilyIndex =
    VK_QUEUE_FAMILY_IGNORED;

dlssToTransfer.srcAccessMask =
    VK_ACCESS_MEMORY_WRITE_BIT;

dlssToTransfer.dstAccessMask =
    VK_ACCESS_TRANSFER_READ_BIT;

dlssToTransfer.image =
    g_dlssOutputImage;

dlssToTransfer.subresourceRange.aspectMask =
    VK_IMAGE_ASPECT_COLOR_BIT;

dlssToTransfer.subresourceRange.baseMipLevel = 0;
dlssToTransfer.subresourceRange.levelCount = 1;

dlssToTransfer.subresourceRange.baseArrayLayer = 0;
dlssToTransfer.subresourceRange.layerCount = 1;



        VkImageMemoryBarrier targetOverwriteBarrier{};

        targetOverwriteBarrier.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;


        const VkImageLayout targetLayout =
            targetIsMain
                ? VK_IMAGE_LAYOUT_GENERAL
                : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;


        targetOverwriteBarrier.oldLayout =
            targetLayout;

        targetOverwriteBarrier.newLayout =
            targetLayout;


        targetOverwriteBarrier.srcQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;

        targetOverwriteBarrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;


        targetOverwriteBarrier.srcAccessMask =
            targetIsMain
                ? (
                    VK_ACCESS_MEMORY_WRITE_BIT |
                    VK_ACCESS_TRANSFER_READ_BIT
                  )
                : VK_ACCESS_TRANSFER_WRITE_BIT;


        targetOverwriteBarrier.dstAccessMask =
            VK_ACCESS_TRANSFER_WRITE_BIT;


        targetOverwriteBarrier.image =
            target;


        targetOverwriteBarrier.subresourceRange.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;

        targetOverwriteBarrier.subresourceRange.baseMipLevel =
            0;

        targetOverwriteBarrier.subresourceRange.levelCount =
            1;

        targetOverwriteBarrier.subresourceRange.baseArrayLayer =
            0;

        targetOverwriteBarrier.subresourceRange.layerCount =
            1;


        VkImageMemoryBarrier dlssPresentBarriers[2] =
        {
            dlssToTransfer,
            targetOverwriteBarrier
        };


vkCmdPipelineBarrier(
    cmd,

    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,

    VK_PIPELINE_STAGE_TRANSFER_BIT,

    0,

    0,
    nullptr,

    0,
    nullptr,

    2,
    dlssPresentBarriers
);


g_dlssOutputLayout =
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;


VkImageBlit blit{};


blit.srcSubresource.aspectMask =
    VK_IMAGE_ASPECT_COLOR_BIT;

blit.srcSubresource.mipLevel = 0;
blit.srcSubresource.baseArrayLayer = 0;
blit.srcSubresource.layerCount = 1;

blit.srcOffsets[0] =
{
    0,
    0,
    0
};

blit.srcOffsets[1] =
{
    static_cast<int32_t>(g_dlssOutputWidth),
    static_cast<int32_t>(g_dlssOutputHeight),
    1
};


blit.dstSubresource.aspectMask =
    VK_IMAGE_ASPECT_COLOR_BIT;

blit.dstSubresource.mipLevel = 0;
blit.dstSubresource.baseArrayLayer = 0;
blit.dstSubresource.layerCount = 1;

        if(targetIsMain)
        {

            blit.dstOffsets[0] =
            {
                0,
                static_cast<int32_t>(outputHeight),
                0
            };

            blit.dstOffsets[1] =
            {
                static_cast<int32_t>(outputWidth),
                0,
                1
            };
        }
        else
        {

            blit.dstOffsets[0] =
            {
                0,
                0,
                0
            };

            blit.dstOffsets[1] =
            {
                static_cast<int32_t>(outputWidth),
                static_cast<int32_t>(outputHeight),
                1
            };
        }
        vkCmdBlitImage(
            cmd,

            g_dlssOutputImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,

            target,
            targetLayout,

            1,
            &blit,

            VK_FILTER_NEAREST
        );

        writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_BLIT);


if(
    targetIsMain &&

    g_currentSLFrameValid &&

    (g_fgMultiplierRequested.load() >= 2 || !g_fgSnapshotsReleased) &&

    ensureFGUIResources(
        outputWidth,
        outputHeight
    )
)
{
    VkImageMemoryBarrier
        hudlessBarriers[2]{};


    hudlessBarriers[0].sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    hudlessBarriers[0].oldLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    hudlessBarriers[0].newLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    hudlessBarriers[0].srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    hudlessBarriers[0].dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    hudlessBarriers[0].srcAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT;

    hudlessBarriers[0].dstAccessMask =
        VK_ACCESS_TRANSFER_READ_BIT;

    hudlessBarriers[0].image =
        target;

    hudlessBarriers[0].subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    hudlessBarriers[0].subresourceRange.baseMipLevel =
        0;

    hudlessBarriers[0].subresourceRange.levelCount =
        1;

    hudlessBarriers[0].subresourceRange.baseArrayLayer =
        0;

    hudlessBarriers[0].subresourceRange.layerCount =
        1;


    hudlessBarriers[1].sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    hudlessBarriers[1].oldLayout =
        g_fgHudlessLayout;

    hudlessBarriers[1].newLayout =
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    hudlessBarriers[1].srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    hudlessBarriers[1].dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    hudlessBarriers[1].srcAccessMask =
        g_fgHudlessLayout ==
            VK_IMAGE_LAYOUT_UNDEFINED
                ? 0
                : VK_ACCESS_SHADER_READ_BIT;

    hudlessBarriers[1].dstAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT;

    hudlessBarriers[1].image =
        g_fgHudlessImage;

    hudlessBarriers[1].subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    hudlessBarriers[1].subresourceRange.baseMipLevel =
        0;

    hudlessBarriers[1].subresourceRange.levelCount =
        1;

    hudlessBarriers[1].subresourceRange.baseArrayLayer =
        0;

    hudlessBarriers[1].subresourceRange.layerCount =
        1;


    vkCmdPipelineBarrier(
        cmd,

        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,

        0,

        0,
        nullptr,

        0,
        nullptr,

        2,
        hudlessBarriers
    );


    g_fgHudlessLayout =
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;


    VkImageBlit
        hudlessBlit{};


    hudlessBlit.srcSubresource.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    hudlessBlit.srcSubresource.mipLevel =
        0;

    hudlessBlit.srcSubresource.baseArrayLayer =
        0;

    hudlessBlit.srcSubresource.layerCount =
        1;


    hudlessBlit.srcOffsets[0] =
    {
        0,
        0,
        0
    };

    hudlessBlit.srcOffsets[1] =
    {
        static_cast<int32_t>(
            outputWidth
        ),

        static_cast<int32_t>(
            outputHeight
        ),

        1
    };


    hudlessBlit.dstSubresource.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    hudlessBlit.dstSubresource.mipLevel =
        0;

    hudlessBlit.dstSubresource.baseArrayLayer =
        0;

    hudlessBlit.dstSubresource.layerCount =
        1;


    hudlessBlit.dstOffsets[0] =
    {
        0,

        static_cast<int32_t>(
            outputHeight
        ),

        0
    };

    hudlessBlit.dstOffsets[1] =
    {
        static_cast<int32_t>(
            outputWidth
        ),

        0,

        1
    };


    vkCmdBlitImage(
        cmd,

        target,
        VK_IMAGE_LAYOUT_GENERAL,

        g_fgHudlessImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,

        1,
        &hudlessBlit,

        VK_FILTER_NEAREST
    );

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_FG);


    VkImageMemoryBarrier
        hudlessToSample{};

    hudlessToSample.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    hudlessToSample.oldLayout =
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    hudlessToSample.newLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    hudlessToSample.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    hudlessToSample.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    hudlessToSample.srcAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT;

    hudlessToSample.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT;

    hudlessToSample.image =
        g_fgHudlessImage;

    hudlessToSample.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    hudlessToSample.subresourceRange.baseMipLevel =
        0;

    hudlessToSample.subresourceRange.levelCount =
        1;

    hudlessToSample.subresourceRange.baseArrayLayer =
        0;

    hudlessToSample.subresourceRange.layerCount =
        1;


    vkCmdPipelineBarrier(
        cmd,

        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,

        0,

        0,
        nullptr,

        0,
        nullptr,

        1,
        &hudlessToSample
    );


    g_fgHudlessLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    g_fgHudlessFrameIndex =
        g_currentSLFrameIndex;


    static uint32_t
        lastHudlessWidth =
            0;

    static uint32_t
        lastHudlessHeight =
            0;


    if(
        lastHudlessWidth !=
            outputWidth ||

        lastHudlessHeight !=
            outputHeight
    )
    {
        fprintf(
            stderr,
            "[FG-4.1 HUDLESS]"
            " frame=%u"
            " size=%ux%u\n",

            g_currentSLFrameIndex,

            outputWidth,
            outputHeight
        );

        fflush(stderr);


        lastHudlessWidth =
            outputWidth;

        lastHudlessHeight =
            outputHeight;
    }
}


VkImageMemoryBarrier dlssBack{};

dlssBack.sType =
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

dlssBack.oldLayout =
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

dlssBack.newLayout =
    VK_IMAGE_LAYOUT_GENERAL;

dlssBack.srcQueueFamilyIndex =
    VK_QUEUE_FAMILY_IGNORED;

dlssBack.dstQueueFamilyIndex =
    VK_QUEUE_FAMILY_IGNORED;

dlssBack.srcAccessMask =
    VK_ACCESS_TRANSFER_READ_BIT;

dlssBack.dstAccessMask =
    VK_ACCESS_MEMORY_READ_BIT |
    VK_ACCESS_MEMORY_WRITE_BIT;

dlssBack.image =
    g_dlssOutputImage;

dlssBack.subresourceRange.aspectMask =
    VK_IMAGE_ASPECT_COLOR_BIT;

dlssBack.subresourceRange.baseMipLevel = 0;
dlssBack.subresourceRange.levelCount = 1;

dlssBack.subresourceRange.baseArrayLayer = 0;
dlssBack.subresourceRange.layerCount = 1;


vkCmdPipelineBarrier(
    cmd,

    VK_PIPELINE_STAGE_TRANSFER_BIT,

    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,

    0,

    0,
    nullptr,

    0,
    nullptr,

    1,
    &dlssBack
);

        if(targetIsMain)
        {
            VkImageMemoryBarrier targetToGui{};

            targetToGui.sType =
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

            targetToGui.oldLayout =
                VK_IMAGE_LAYOUT_GENERAL;

            targetToGui.newLayout =
                VK_IMAGE_LAYOUT_GENERAL;

            targetToGui.srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;

            targetToGui.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;

            targetToGui.srcAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT |
                VK_ACCESS_TRANSFER_WRITE_BIT;

            targetToGui.dstAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            targetToGui.image =
                target;

            targetToGui.subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;

            targetToGui.subresourceRange.baseMipLevel =
                0;

            targetToGui.subresourceRange.levelCount =
                1;

            targetToGui.subresourceRange.baseArrayLayer =
                0;

            targetToGui.subresourceRange.layerCount =
                1;


            vkCmdPipelineBarrier(
                cmd,

                VK_PIPELINE_STAGE_TRANSFER_BIT,

                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,

                0,

                0,
                nullptr,

                0,
                nullptr,

                1,
                &targetToGui
            );
        }


g_dlssOutputLayout =
    VK_IMAGE_LAYOUT_GENERAL;

    logFrameDoneHeartbeat();

    endPerfL10GpuFrame(cmd);

    perfL10CpuPhaseEndFrame();

    finishPerfL10CpuFrame();

    { logFrameSkip("ret@55192", "auto"); return; }
}
}


extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_testFrame(JNIEnv*,jclass){
}
