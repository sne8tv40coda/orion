


// =====================================================
// T11 - Deterministic scene reflection for glass
//
// Exactly one first-surface reflection ray.  It follows the geometric mirror
// direction and returns either the real opaque-scene radiance at its first
// hit, or the existing miss-shader sky radiance.  Water and later transparent
// hits deliberately contribute nothing here.
// =====================================================


vec3 traceGlassSceneReflectionOneRay(
    vec3 glassSurfacePosition,
    vec3 glassNormal,
    vec3 viewDir,
    vec3 sunDirection
)
{
    vec3 N = normalize(glassNormal);
    vec3 V = normalize(viewDir);
    float NdotV = max(dot(N, V), 0.0);
    if(NdotV <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 reflectionDirection =
        normalize(reflect(-V, N));

    const float glassReflCamDist =
        length(glassSurfacePosition - cam.viewInverse[3].xyz);
    // 2026-11-27: 确定性极速反射（UBO 掩蔽防展开）：最多 1 次有效反射命中，
    // 若首跳擦到 coplanar 玻璃（hitDist < 0.02）或穿过 1 层玻璃，最多续追 1 步。
    const int maxGlassReflectSteps =
        (cam.frameCount != 0xFFFFFFFFu) ? 2 : 0;

    vec3 currentOrigin =
        glassSurfacePosition + N * 0.002 + reflectionDirection * 0.001;
    vec3 currentDirection = reflectionDirection;
    vec3 throughput = vec3(1.0);
    vec3 reflectedSceneRadiance = vec3(0.0);
    float accumulatedDistance = 0.0;
    bool recordedFirstGuide = false;
    bool appliedTranslucentTint = false;
    bool reachedTerminus = false;

    for(int step = 0; step < maxGlassReflectSteps; ++step)
    {
        hit.hitDistance = 1.0;
        hit.albedoPacked = packAlbedoUnorm(vec3(1.0));
        hit.state = makeTraceState(RT_PURPOSE_FULL) |
            RT_STATE_NO_SUN_DISK_BIT |
            RT_STATE_NO_SUN_GLOW_BIT |
            RT_STATE_GI_NO_POM_BIT |
            RT_STATE_GI_SKIP_CLOUDS_BIT;

#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
        serTraceRay(
            currentOrigin,
            0.01,
            currentDirection,
            10000.0,
            secondaryTraverseFlags(),
            0xcfu,
            0u
        );
#else
#ifdef RT_ENABLE_SER
        // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
        if(cam.perfSerAb.z != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.01,
                currentDirection,
                10000.0,
                secondaryTraverseFlags(),
                0xcfu,
                0u
            );
        }
        else
#endif
        {
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
                0.01,
                currentDirection,
                10000.0,
                0
            );
        }
#endif

        if(payload.hitDistance < 0.0)
        {
            rrGlassReflectionHitDistance = 65504.0;
            reflectedSceneRadiance +=
                throughput * unpackAlbedoUnorm(payload.albedoPacked);
            reachedTerminus = true;
            break;
        }

        float hitDist = payload.hitDistance;
        vec3 hitPos = currentOrigin + currentDirection * hitDist;
        vec3 hitNormal = normalize(payload.normal);
        vec3 hitAlbedo = unpackAlbedoUnorm(payload.albedoPacked);
        float hitMatId = getLegacyMaterialId(payload.state);
        float hitRoughness = payload.roughness;
        float hitF0Metal = payload.f0Metal;
        vec3 hitEmission = payload.emission;
        float hitPomSelfShadow = payload.pomSelfShadow;

        accumulatedDistance += hitDist;

        if(hitMatId <= 0.5)
        {
            rrGlassReflectionHitDistance = accumulatedDistance;

#if GI_GLASS_DOMINANT_GUIDE
            if(!recordedFirstGuide)
            {
                glassReflectionReceiverValid = true;
                glassReflectionReceiverNormal = hitNormal;
                glassReflectionReceiverAlbedo =
                    clamp(hitAlbedo, vec3(0.0), vec3(1.0));
                glassReflectionReceiverRoughness =
                    clamp(hitRoughness, 0.0, 1.0);
                recordedFirstGuide = true;
            }
#endif

            vec3 reflectionHitEmission =
                hitEmission +
                evaluateVLIEmitterRadianceAtHit(
                    hitPos,
                    hitNormal,
                    hitAlbedo
                );

            // 2026-11-27: 玻璃反射禁用 SHaRC 短窗有噪缓存查询，统一走 100% 确定性着色，
            // 彻底消除倒影沸腾闪动。
            reflectedSceneRadiance +=
                throughput * (
                    reflectionHitEmission +
                    evaluateDirectSunLightingDiffuseInMedium(
                        hitPos,
                        hitNormal,
                        hitAlbedo,
                        hitF0Metal,
                        hitPomSelfShadow,
                        sunDirection,
                        false
                    ) +
                    evaluateVPTExactFaceDirectLighting(
                        hitPos,
                        hitNormal,
                        hitAlbedo
                    ) +
                    evaluateDynamicLightDeterministicDirect(
                        hitPos,
                        hitNormal,
                        hitAlbedo,
                        glassReflCamDist
                    ) +
                    hitAlbedo * evaluateDeterministicIndirectAmbient(hitNormal, 1.0)
                );

            reachedTerminus = true;
            break;
        }
        else if(hitMatId < 2.5)
        {
            vec3 waterNormal =
                dot(hitNormal, currentDirection) > 0.0
                    ? -hitNormal
                    : hitNormal;
            float waterCos =
                clamp(-dot(currentDirection, waterNormal), 0.0, 1.0);
            float waterF = fresnelSchlickScalar(waterCos, 0.02);
            vec3 waterMirrorDir =
                normalize(reflect(currentDirection, waterNormal));
            vec3 waterReflSky = mix(
                waterScatterColorAt(hitPos.xz) * getSurfaceAmbientRadiance() * 0.6,
                samplePhysicalClearSkyForAerialPerspective(waterMirrorDir),
                smoothstep(-0.05, 0.25, waterMirrorDir.y)
            );
            vec3 waterTransBase =
                hitAlbedo * evaluateDeterministicIndirectAmbient(waterNormal, 0.85);

            reflectedSceneRadiance +=
                throughput * mix(waterTransBase, waterReflSky, waterF);

            rrGlassReflectionHitDistance = accumulatedDistance;
            reachedTerminus = true;
            break;
        }
        else
        {
            reflectedSceneRadiance += throughput * hitEmission;

            if(hitDist < 0.02)
            {
                currentOrigin = hitPos + currentDirection * 0.002;
                continue;
            }

            vec3 faceNormal =
                dot(hitNormal, currentDirection) > 0.0
                    ? -hitNormal
                    : hitNormal;
            float faceCos =
                clamp(-dot(currentDirection, faceNormal), 0.0, 1.0);
            float faceF = fresnelSchlickScalar(faceCos, 0.04);
            vec3 faceTint =
                appliedTranslucentTint
                    ? vec3(1.0)
                    : clamp(hitAlbedo, vec3(0.0), vec3(1.0));
            appliedTranslucentTint = true;

            throughput *= (1.0 - faceF) * faceTint;
            if(max(throughput.r, max(throughput.g, throughput.b)) < 0.02)
            {
                rrGlassReflectionHitDistance = accumulatedDistance;
                reachedTerminus = true;
                break;
            }

            currentOrigin = hitPos + currentDirection * 0.002;
            continue;
        }
    }

    if(!reachedTerminus)
    {
        rrGlassReflectionHitDistance =
            accumulatedDistance > 0.0 ? accumulatedDistance : 65504.0;
        reflectedSceneRadiance +=
            throughput * evaluateDeterministicIndirectAmbient(-currentDirection, 1.0);
    }

    return reflectedSceneRadiance;
}