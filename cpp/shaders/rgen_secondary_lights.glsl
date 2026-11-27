


// =====================================================
// L7.3 B - Secondary diffuse Local Light GI
//
// Budget per opaque secondary hit:
//   - scan the existing Local Light SSBO,
//   - keep the strongest 2 approximate contributors,
//   - 1 real 2.0x2.0 area sample per selected light,
//   - at most 2 visibility rays total,
//   - diffuse PBR response only (no secondary GGX specular).
//
// The result is outgoing radiance at this secondary surface.
// The caller multiplies it by currentThroughput before adding it
// to indirectLight.
// =====================================================

vec3 evaluateSecondaryDiffuseLocalLights(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 viewDir,
    vec3 hitAlbedo,
    float hitF0Metal
)
{
    uint lightCount =
        localLightBuffer.header.x;


    if(lightCount == 0u)
    {
        return
            vec3(0.0);
    }


    vec3 N =
        normalize(
            hitNormal
        );


    vec3 V =
        normalize(
            viewDir
        );


    float NdotV =
        max(
            dot(
                N,
                V
            ),
            0.0
        );


    if(NdotV <= 0.0)
    {
        return
            vec3(0.0);
    }


    // -------------------------------------------------
    // Select only the strongest two local contributors.
    // No visibility ray is fired during this scan.
    // -------------------------------------------------

    uint selectedIndex[
        MAX_SECONDARY_LOCAL_LIGHTS
    ];


    float selectedScore[
        MAX_SECONDARY_LOCAL_LIGHTS
    ];


    for(
        int slot = 0;
        slot < MAX_SECONDARY_LOCAL_LIGHTS;
        ++slot
    )
    {
        selectedIndex[slot] =
            0u;

        selectedScore[slot] =
            -1.0;
    }


    // Probe slightly inside the currently hit block.
    // If this secondary surface itself belongs to an emitter,
    // nextEmission already carries its self-radiance, so do not
    // add a local-light self-contribution here.
    vec3 emitterProbePos =
        hitPos -
        N * 0.02;


    for(
        uint lightIndex = 0u;
        lightIndex < lightCount;
        ++lightIndex
    )
    {
        LocalLightGPU candidateLight =
            localLightBuffer
                .lights[lightIndex];


        vec4 candidateWorldPosEmission =
            candidateLight
                .worldPosEmission;


        vec3 emitterBlockMin =
            candidateWorldPosEmission.xyz -
            vec3(0.001);


        vec3 emitterBlockMax =
            candidateWorldPosEmission.xyz +
            vec3(1.001);


        bool hitThisEmitter =
            all(
                greaterThanEqual(
                    emitterProbePos,
                    emitterBlockMin
                )
            ) &&
            all(
                lessThanEqual(
                    emitterProbePos,
                    emitterBlockMax
                )
            );


        if(hitThisEmitter)
        {
            continue;
        }


        float score =
            localLightApproxScore(
                hitPos,
                N,
                candidateWorldPosEmission,
                candidateLight.meta.x
            );


        if(score <= 0.0)
        {
            continue;
        }


        for(
            int slot = 0;
            slot < MAX_SECONDARY_LOCAL_LIGHTS;
            ++slot
        )
        {
            if(score <= selectedScore[slot])
            {
                continue;
            }


            for(
                int shift =
                    MAX_SECONDARY_LOCAL_LIGHTS - 1;
                shift > slot;
                --shift
            )
            {
                selectedScore[shift] =
                    selectedScore[
                        shift - 1
                    ];

                selectedIndex[shift] =
                    selectedIndex[
                        shift - 1
                    ];
            }


            selectedScore[slot] =
                score;

            selectedIndex[slot] =
                lightIndex;

            break;
        }
    }


    int f0MetalCode =
        decodeF0MetalCode(
            hitF0Metal
        );


    float metallic =
        decodeMetallicFromCode(
            f0MetalCode
        );


    // Secondary diffuse Local Light deliberately carries no
    // metallic diffuse energy.
    if(metallic >= 0.999)
    {
        return
            vec3(0.0);
    }


    vec3 F0 =
        decodeBaseF0(
            f0MetalCode,
            hitAlbedo
        );


    vec3 accumulatedSecondaryLocal =
        vec3(0.0);


    for(
        int slot = 0;
        slot < MAX_SECONDARY_LOCAL_LIGHTS;
        ++slot
    )
    {
        if(selectedScore[slot] <= 0.0)
        {
            continue;
        }


        LocalLightGPU selectedLight =
            localLightBuffer
                .lights[
                    selectedIndex[slot]
                ];


        vec4 worldPosEmission =
            selectedLight
                .worldPosEmission;


        uint typeCode =
            selectedLight.meta.x;


        float emissionLevel =
            clamp(
                worldPosEmission.w,
                1.0,
                15.0
            );


        vec3 emitterSurfaceNormal =
            vec3(0.0);


        float faceSelectionProbability =
            1.0;


        bool pointLikeEmitter =
            isPointLikeLocalLightType(
                typeCode
            );


        // B mode = exactly one source sample per light.
        vec3 sampleLightPos =
            vec3(0.0);


        if(pointLikeEmitter)
        {
            sampleLightPos =
                sampleLocalLightPointSphereSurface(
                    worldPosEmission.xyz,
                    hitPos,
                    slot,
                    0
                );
        }
        else
        {
            sampleLightPos =
                sampleLocalLightCubeSurface(
                    worldPosEmission.xyz,
                    hitPos,
                    slot,
                    0,
                    emitterSurfaceNormal,
                    faceSelectionProbability
                );
        }


        vec3 sampleToLight =
            sampleLightPos -
            hitPos;


        float sampleDistanceSquared =
            dot(
                sampleToLight,
                sampleToLight
            );


        if(sampleDistanceSquared <= 1.0e-8)
        {
            continue;
        }


        float sampleDistance =
            sqrt(
                sampleDistanceSquared
            );


        float sampleRangeFade =
            localLightRangeFade(
                sampleDistance,
                typeCode,
                emissionLevel
            );


        if(sampleRangeFade <= 0.0)
        {
            continue;
        }


        vec3 L =
            sampleToLight /
            sampleDistance;


        float geometricNdotL =
            max(
                dot(
                    N,
                    L
                ),
                0.0
            );


        if(geometricNdotL <= 0.0)
        {
            continue;
        }


        float sourceAngularWeight =
            LOCAL_LIGHT_POINT_SPHERE_PROJECTED_AREA;


        if(!pointLikeEmitter)
        {
            float emitterFacing =
                max(
                    dot(
                        emitterSurfaceNormal,
                        -L
                    ),
                    0.0
                );


            if(emitterFacing <= 0.0)
            {
                continue;
            }


            sourceAngularWeight =
                emitterFacing /
                max(
                    faceSelectionProbability,
                    1.0e-4
                );
        }


        // -------------------------------------------------
        // One colored-glass-aware visibility ray.
        // -------------------------------------------------

        float shadowMaxDistance =
            sampleDistance -
            LOCAL_LIGHT_CUBE_RAY_END_EPSILON;


        bool visible =
            true;


        vec3 localShadowTransmittance =
            vec3(1.0);


        if(shadowMaxDistance > 0.03)
        {
            hit.hitDistance =
                1.0;

            hit.albedoPacked = packAlbedoUnorm(
                vec3(1.0));

            hit.parallaxRayTravel =
                0.0;

            hit.state = makeTraceState(RT_PURPOSE_GLASS_VISIBILITY);
            // 2026-10-23：可见性射线必须从 albedo = vec3(1.0) 起算 ——
            // 见 HANDOFF-2026-10-21-VERTEX-32B-TERRAIN.md §8。否则累乘起点是
            // 上一条射线的残留 albedo，逐帧不同 ⇒ 静止时被 DLSS-RR 当信号累积。
            hit.albedoPacked = packAlbedoUnorm(vec3(1.0));


            // 2026-11-27 (Batch AI): visibility-only local-light ray skips SER overhead.
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

                    hitPos + L * 0.001,

                    0.02,

                    L,

                    shadowMaxDistance,

                    0
                );
            }


            visible =
                hit.hitDistance <
                0.0;


            if(visible)
            {
                localShadowTransmittance =
                    clamp(
                        unpackAlbedoUnorm(hit.albedoPacked),
                        vec3(0.0),
                        vec3(1.0)
                    );
            }
        }


        if(!visible)
        {
            continue;
        }


        // -------------------------------------------------
        // Diffuse-only PBR response.
        //
        // Reuse the same Fresnel/energy split as the primary
        // local-light diffuse lobe, but deliberately skip GGX
        // specular in L7.3 B.
        // -------------------------------------------------

        vec3 H =
            normalize(
                V + L
            );


        float VdotH =
            max(
                dot(
                    V,
                    H
                ),
                0.0
            );


        vec3 F =
            fresnelSchlick(
                VdotH,
                F0
            );


        vec3 kD =
            (
                vec3(1.0) -
                F
            ) *
            (
                1.0 -
                metallic
            );


        float softDiffuseNdotL =
            geometricNdotL;


        vec3 diffuseTerm =
            kD *
            hitAlbedo *
            softDiffuseNdotL *
            BSDF_LOCAL_DIFFUSE_SCALE;


        // 2026-10-31：亮度按光源档案（每档自己的满级亮度）。
        float emission01 =
            localLightProfileBrightness(
                typeCode,
                emissionLevel
            );


float distanceAttenuation =
    localLightDistanceAttenuation(
        sampleDistance
    );

float radianceScale =
    3.2 *
    LOCAL_LIGHT_L7_RADIANCE_SCALE * localLightIntensity() *
    emission01 *
    distanceAttenuation *
    sampleRangeFade *
    sourceAngularWeight;


        vec3 localRadiance =
            decodeLocalLightColor(
                typeCode
            ) *
            radianceScale;


        accumulatedSecondaryLocal +=
            localRadiance *
            localShadowTransmittance *
            diffuseTerm;
    }


    return
        accumulatedSecondaryLocal;
}