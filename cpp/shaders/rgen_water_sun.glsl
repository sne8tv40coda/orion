
// =====================================================
// 2026-11-13：玻璃光学焦散（GLASS_CAUSTIC_LEVEL）
//
// ⚠ 自创设计——仓库内没有玻璃焦散的参考实现（Caustica 未 vendored，docs 只有
//   水焦散 docs/decisions/water-wave-field.md 一族）。数学约定复用仓内既有
//   口径：透射 = 乘性 transmittance、"无扰动 = 不加光"（水焦散的 density 1
//   契约，rgen_water_caustics.glsl:480-489）；薄片 Fresnel 系列复用
//   glassSheetTransmission（rgen_glass_profile.glsl，含 Schlick 0.04 双程）。
//
// 物理依据（为什么这样做）：
//   1. 覆盖率裁剪 = 平板玻璃唯一真实的光学焦散来源。MC 玻璃是平面，折射波前
//      Jacobian ≡ 1 ⇒ 浓度恒 ≤ 1（能量守恒，不存在透镜式会聚）；可观测的
//      "光透过玻璃形成亮纹图案"来自贴图 alpha 对日盘的逐纹素裁剪：心部
//      alpha=0（原版玻璃透明心）⇒ 日盘直通，边框/条纹 alpha≈1 ⇒ 走薄片滤光
//      （含 Fresnel）。合成式 mix(1, 薄片, alpha) 与 2026-11-02 相机重写档
//      "贴图 alpha 只当覆盖率"同一语义（Caustica GLASS_COVERAGE 条款）。
//      只对清玻璃生效（RT_STATE_CLEAR_GLASS_BIT）：染色玻璃/致密体的 alpha
//      是整片透射率而非二值覆盖率，混入会把染色心部抬成白光。
//   2. 折射位移：march 续段起点沿介质内折射方向平移，出射方向恢复入射
//      方向（平行板）。2026-11-13② 起对**全部玻璃族**生效并用真实板厚
//      （glassSlabThicknessFromFace 同款解析公式，块 1 格 / 板 1/8 格；位移
//      上界 1.0）——染色玻璃的焦散主体就是这条：色斑在影子内部随光学路径
//      偏移。此前清玻璃用的 3cm 薄壁钳制是相机路径的伪影口径，对块玻璃会
//      把位移钳到不可见。附带修正：位移后起点落在出射面之外 ⇒ 竖直面玻璃
//      的"进出两面各 tint 一次"双计随之消失。
//      与 2026-10-11 被删除的 R14（POM 浮雕调制的彩色光斑，rahit 里 #if 0
//      封存）完全不同：那条是浮雕遮蔽，这条是覆盖率透光 + 折射位移。
//
// 历史：2026-10-11 曾有一版"玻璃焦散"（R14 出射面 POM 遮蔽）被用户要求删除，
//   当时用户的口径是"焦散 = POM 光斑"。本次是用户 2026-11-13 明确要求的
//   光学（折射/透光）焦散，二者不冲突。
//
// 0 = 旧档（整片 tint、无位移）。回退：-DGLASS_CAUSTIC_LEVEL=0。
// =====================================================
// 2026-11-13（用户拍板）：玻璃光学焦散**整体回退**，LEVEL 默认 0 ⇒ 太阳 march
// 逐位回到焦散前档（整片 tint、无位移）。下方 LEVEL>=1 的覆盖率/位移代码
// 全部编译掉，保留作 A/B。
#ifndef GLASS_CAUSTIC_LEVEL
#define GLASS_CAUSTIC_LEVEL 0
#endif
#ifndef GLASS_CAUSTIC_LIFT
// 2026-11-13⑧：覆盖率合成的"阴影底"参数。纯覆盖率（lift=1）下原版清玻璃
// 心部 alpha=0 ⇒ 透射=全阳光 ⇒ 玻璃下的染色阴影整体消失——折射位移没有
// 可位移的影子边缘，用户实测"根本没焦散、折射也没了"。lift=0 = 旧档均匀
// 滤光（有阴影无图案）。默认 0.5：心部 = tint + (1-tint)×0.5，亮于边框/
// 条纹（图案可见）且整体仍暗于露天（阴影可见）。
#define GLASS_CAUSTIC_LIFT 0.5
#endif

// 2026-11-15：透射统一随水深 —— 签名去掉 stableTransmission。
// 旧 stable 档把水下段吸收钳到 min(hitDistance, 2.0)，水底直射太阳与真实
// 水深脱钩（浅水深水一样亮）；且旧的步数耗尽语义（一律 return 0）让远档
// （着色点超出"太阳透射距离"设置）的水底 march 在穿出水面后直接判 0，
// 远处水底太阳/焦散整片消失。现在：吸收一律按真实水程 Beer-Lambert，
// "最后一步完成出射即返回累积透射"（见函数尾），透射观感只由水深决定，
// 与性能档位无关。
// 2026-11-27: 空气侧半透明/树叶太阳可见性单发路径。
// 完全不受“太阳透射距离”或 1 步循环限制；any-hit (RT_PURPOSE_GLASS_VISIBILITY)
// 会自动累积沿途多层染色玻璃过滤并在不透明/镂空实体命中时终止。
vec3 traceAirSunTransmittanceGlassVis(
    vec3 startOrigin,
    vec3 sunDir
) {
#ifndef ORION_K2I_COMPOSE_COMPUTE
    const vec3 dir = normalize(sunDir);
    hit.hitDistance = 1.0;
    hit.albedoPacked = packAlbedoUnorm(vec3(1.0));
    hit.parallaxRayTravel = 0.0;
    hit.glassAlpha = 0.0;
    hit.state = makeTraceState(RT_PURPOSE_GLASS_VISIBILITY);

#if ORION_OPAQUE_VIS_FASTPATH
    {
        rayQueryEXT airSunVisQuery;
        rayQueryInitializeEXT(
            airSunVisQuery,
            topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT,
            g_secondaryCullMask,
            startOrigin,
            0.02,
            dir,
            10000.0
        );
        if(!rayQueryProceedEXT(airSunVisQuery))
        {
            #ifdef ORION_RT_STAGE_COUNTERS
            rtCntRayIssued();
            #endif
            return (rayQueryGetIntersectionTypeEXT(airSunVisQuery, true) !=
                    gl_RayQueryCommittedIntersectionNoneEXT)
                ? vec3(0.0)
                : vec3(1.0);
        }
        rayQueryTerminateEXT(airSunVisQuery);
    }
#endif
    // 2026-11-27 (Batch AI): visibility-only glass shadow ray skips SER overhead.
    {
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        traceRayEXT(
            topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT |
            gl_RayFlagsSkipClosestHitShaderEXT,
            g_secondaryCullMask,
            0,
            0,
            0,
            startOrigin,
            0.02,
            dir,
            10000.0,
            0
        );
    }
    if(payload.hitDistance >= 0.0)
    {
        return vec3(0.0);
    }
    return clamp(unpackAlbedoUnorm(payload.albedoPacked), vec3(0.0), vec3(1.0));
#else
    return vec3(1.0);
#endif
}

// 空气侧太阳透射入口：直接走单次 RT_PURPOSE_GLASS_VISIBILITY，零步数/距离钳制。
vec3 traceSunTransmittanceAir(
    vec3 startOrigin,
    vec3 sunDir
) {
    return traceAirSunTransmittanceGlassVis(startOrigin, sunDir);
}

vec3 traceSunTransmittanceThroughWaterEx(
    vec3 startOrigin,
    vec3 rayDir,
    vec3 airSunDir
) {

    const float WATER_IOR =
        1.33;

    // Waterlight 风格青绿松石水色：红强吸收、蓝轻压、绿最透。
    const vec3 WATER_ABSORPTION =
        waterAbsorptionColor();
        const vec3 WATER_SCATTER_COLOR = waterScatterColor();

const float WATER_EXTINCTION_DISTANCE =
    16.0;

    const float WATER_F0 =
        0.0200593;

    const float RAY_BIAS =
        0.001;

    vec3 transmittance =
        vec3(1.0);

    vec3 currentOrigin =
        startOrigin;

    vec3 currentDir =
        normalize(rayDir);

    const vec3 targetAirDir =
        normalize(airSunDir);

    // 2026-11-15：标记"最后一步刚好完成 Water->Air 出射（人已在空中）"。
    bool exitedToAir = false;
    bool crossedGlass = false;

#if ORION_OPAQUE_VIS_FASTPATH && !defined(ORION_K2I_COMPOSE_COMPUTE)
    {
        rayQueryEXT waterSunVisQuery;
        rayQueryInitializeEXT(
            waterSunVisQuery,
            topLevelAS,
            secondaryTraverseFlags() | gl_RayFlagsTerminateOnFirstHitEXT,
            0xcfu,
            currentOrigin,
            0.02,
            currentDir,
            10000.0
        );
        if(!rayQueryProceedEXT(waterSunVisQuery))
        {
            #ifdef ORION_RT_STAGE_COUNTERS
            rtCntRayIssued();
            #endif
            return (rayQueryGetIntersectionTypeEXT(waterSunVisQuery, true) !=
                    gl_RayQueryCommittedIntersectionNoneEXT)
                ? vec3(0.0)
                : vec3(1.0);
        }
        rayQueryTerminateEXT(waterSunVisQuery);
    }
#endif

    // UBO 掩蔽防编译展开膨胀；专责水下找水面出射，不再受旧 1 步距离设置钳制。
    const int maxUnderwaterSunSteps =
        (cam.frameCount != 0xFFFFFFFFu) ? 3 : 0;

    for(
        int step = 0;
        step < maxUnderwaterSunSteps;
        ++step
    )
    {
        exitedToAir = false;

        hit.hitDistance =
            1.0;

        hit.state = makeTraceState(RT_PURPOSE_CLASSIFY);

#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
        serTraceRay(
            currentOrigin,
            0.02,
            currentDir,
            10000.0,
            secondaryTraverseFlags(),
            0xcfu,
            step
        );
#else
#ifdef RT_ENABLE_SER
        if(cam.perfSerAb.y != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.02,
                currentDir,
                10000.0,
                secondaryTraverseFlags(),
                0xcfu,
                step
            );
        }
        else
#endif
        {
            #ifndef ORION_K2I_COMPOSE_COMPUTE
            #ifdef ORION_RT_STAGE_COUNTERS
            rtCntRayIssued();
            #endif
            traceRayEXT(
                topLevelAS,
                secondaryTraverseFlags(),
                0xcfu,
                0,
                0,
                0,
                currentOrigin,
                0.02,
                currentDir,
                10000.0,
                0
            );
            #else
            payload.hitDistance = -1.0;
            #endif
        }
#endif

        // 没有任何几何体阻挡。
        if(
            payload.hitDistance < 0.0
        )
        {
            return transmittance;
        }

        float hitMaterialId =
            getLegacyMaterialId(payload.state);

if(hitMaterialId >= 2.5)
{
    if(hitMaterialId < 3.5)
    {
        transmittance *=
            glassSheetTransmission(
                unpackAlbedoUnorm(payload.albedoPacked),
                payload.normal,
                currentDir
            );

        if(max(transmittance.r, max(transmittance.g, transmittance.b)) < 0.01)
        {
            return vec3(0.0);
        }
    }

    vec3 glassHitPos =
        currentOrigin +
        currentDir *
        payload.hitDistance;

    crossedGlass = true;
    currentOrigin =
        glassHitPos +
        currentDir *
        0.002;

    continue;
}

        if(
            hitMaterialId <= 0.5
        )
        {
            return vec3(0.0);
        }

        bool exitingWater =
            hitMaterialId > 1.5;

        if(exitingWater)
        {
            float waterDistance =
                payload.hitDistance;

            vec3 extinction =
                exp(
                    -WATER_ABSORPTION *
                    waterDistance
                );

            transmittance *= extinction;

            float scatterDepth =
                1.0 -
                exp(
                    -waterDistance * 0.05
                );

            vec3 inScatter =
                WATER_SCATTER_COLOR *
                scatterDepth;

            transmittance +=
                inScatter;
        }

        vec3 hitPos =
            currentOrigin +
            currentDir *
            payload.hitDistance;

        vec3 hitNormal =
            stabilizeWaterInterfaceNormal(
                normalize(
                    payload.normal
                )
            );

        vec3 interfaceNormal =
            (abs(hitNormal.y) > 0.15)
                ? vec3(
                    0.0,
                    hitNormal.y >= 0.0 ? 1.0 : -1.0,
                    0.0
                )
                : hitNormal;

        float cosTheta =
            clamp(
                -dot(
                    currentDir,
                    interfaceNormal
                ),
                0.0,
                1.0
            );

        float fresnelX = 1.0 - cosTheta;
        float fresnelX2 = fresnelX * fresnelX;
        float fresnel =
            WATER_F0 +
            (1.0 - WATER_F0) *
            (fresnelX2 * fresnelX2) *
            fresnelX;

        transmittance *=
            1.0 -
            fresnel;

        if(max(transmittance.r, max(transmittance.g, transmittance.b)) < 0.01)
        {
            return vec3(0.0);
        }

        if(exitingWater)
        {
            exitedToAir = true;
            currentOrigin =
                hitPos -
                interfaceNormal *
                0.005;
            currentDir = targetAirDir;
            break;
        }
        else
        {
            crossedGlass = false;
            exitedToAir = false;
            float eta =
                1.0 / WATER_IOR;
            vec3 transmittedDir =
                refract(
                    currentDir,
                    interfaceNormal,
                    eta
                );
            currentOrigin =
                hitPos -
                interfaceNormal *
                RAY_BIAS;
            currentDir =
                (dot(transmittedDir, transmittedDir) >= 1.0e-6)
                    ? normalize(transmittedDir)
                    : currentDir;
        }
    }

    if(!exitedToAir)
    {
        if(crossedGlass)
        {
            return transmittance *
                traceAirSunTransmittanceGlassVis(
                    currentOrigin + targetAirDir * 0.02,
                    targetAirDir
                );
        }
        return vec3(0.0);
    }

    return transmittance *
        traceAirSunTransmittanceGlassVis(
            currentOrigin + currentDir * 0.02,
            currentDir
        );
}

vec3 traceSunTransmittanceThroughWater(
    vec3 startOrigin,
    vec3 rayDir
) {
    return traceSunTransmittanceThroughWaterEx(
        startOrigin,
        rayDir,
        rayDir
    );
}
const float PI =
    3.14159265359;