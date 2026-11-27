

// =====================================================
// T9 LOW-COST - one-sample diffuse GI behind transparent glass
//
// Exactly ONE additional diffuse GI trace is used for a glass pixel.
// To keep the T9 budget conservative, a secondary opaque hit uses
// emission + configured ambient only; no secondary local-light scan
// and no secondary sun visibility ray are spawned here.
//
// A sky miss uses the real miss radiance, giving the receiver a
// physically meaningful environment bounce.
// =====================================================

vec3 traceTransparentLowCostGI(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 hitAlbedo
)
{
    vec3 N =
        normalize(
            hitNormal
        );


    vec3 giDirection =
        sampleHemisphereCosine2D(
            N,
            vec2(
                glassBlueNoiseSample(rtLogicalPixelCoord(), cam.frameCount, 67u),
                glassBlueNoiseSample(rtLogicalPixelCoord(), cam.frameCount, 68u)
            )
        );


    hit.hitDistance =
        1.0;

    hit.state = makeTraceState(RT_PURPOSE_FULL);
    hit.state |= RT_STATE_GI_SKIP_CLOUDS_BIT;
#if GI_SKIP_SUN_DISK
    hit.state |= RT_STATE_NO_SUN_DISK_BIT;
#endif

    // 2026-10-25 A/B：低代价 GI 射线同样跳过 POM（与主 GI 回路同口径）。
    if(cam.giHalfParams.z > 0.5f)
    {
        hit.state |= RT_STATE_GI_NO_POM_BIT;
    }


#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
    serTraceRay(
        hitPos + giDirection * 0.001,
        0.02,
        giDirection,
        10000.0,
        secondaryTraverseFlags(),
        g_secondaryCullMask,
        0u
    );
#else
#ifdef RT_ENABLE_SER
    // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
    if(cam.perfSerAb.z != 0u)
    {
        serTraceRay(
            hitPos + giDirection * 0.001,
            0.02,
            giDirection,
            10000.0,
            secondaryTraverseFlags(),
            g_secondaryCullMask,
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
            g_secondaryCullMask,
            0,
            0,
            0,
            hitPos + giDirection * 0.001,
            0.02,
            giDirection,
            10000.0,
            0
        );
    }
#endif


    vec3 incomingRadiance =
        vec3(0.0);


    if(payload.hitDistance < 0.0)
    {
        incomingRadiance =
            unpackAlbedoUnorm(payload.albedoPacked);
    }
    else
    {
        float secondaryMaterialId =
            getLegacyMaterialId(payload.state);


        // 2026-10-09：加上末地银河的环境底光（非末地 vec3(0) ⇒ 逐位不变）。
        // 这一处底光是**内联**的（不像近场/DH 远场走 getSurfaceAmbientBase()，
        // 它不含 noonShadowContrast 那一档），所以只加增量、不动原来的 clamp。
        vec3 ambientRadiance =
            vec3(clamp(
                cam.cameraParams.w,
                0.0,
                1.0
            )) + endGalaxyAmbientLift();


        if(secondaryMaterialId <= 0.5)
        {
            incomingRadiance =
                payload.emission +
                unpackAlbedoUnorm(payload.albedoPacked) *
                ambientRadiance;
        }
        else
        {
            // Water / glass secondary interfaces are intentionally
            // conservative here; recursive transparent GI is deferred
            // to the later multi-layer stage.
            incomingRadiance =
                unpackAlbedoUnorm(payload.albedoPacked) *
                ambientRadiance;
        }
    }


    // Match the project's established GI transport scale:
    // 0.7 throughput and 1.8 final indirect multiplier.
    return
        hitAlbedo *
        incomingRadiance *
        BSDF_GI_RESULT_SCALE;
}


vec3 evaluateOpaqueSurfaceRadianceInMedium(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 viewDir,
    vec3 hitAlbedo,
    float hitRoughness,
    float hitF0Metal,
    vec3 hitEmission,
    float pomSelfShadow,
    vec3 sunDirection,
    bool startInWater
)
{
    return
        hitEmission +
        evaluateDirectSunLightingInMedium(
            hitPos,
            hitNormal,
            viewDir,
            hitAlbedo,
            hitRoughness,
            hitF0Metal,
            pomSelfShadow,
            sunDirection,
            startInWater
        );
}

vec3 evaluateOpaqueSurfaceRadiance(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 viewDir,
    vec3 hitAlbedo,
    float hitRoughness,
    float hitF0Metal,
    vec3 hitEmission,
    float pomSelfShadow,
    vec3 sunDirection
)
{
    return evaluateOpaqueSurfaceRadianceInMedium(
        hitPos,
        hitNormal,
        viewDir,
        hitAlbedo,
        hitRoughness,
        hitF0Metal,
        hitEmission,
        pomSelfShadow,
        sunDirection,
        false
    );
}
// =====================================================
// T1-2 Generic translucent straight transmission
//
// Cost target:
// - exactly ONE additional trace for pixels whose primary hit
//   is generic translucent terrain;
// - any-hit skips generic translucent geometry on that trace;
// - no refraction, no reflection split, no recursive branching.
//
// T1 scope intentionally shades the first opaque surface behind
// the glass with the existing emission + direct-sun evaluator.
// Local Light / GI participation is reserved for T9.
// =====================================================
// Shared primary continuation for water and generic translucent terrain.
//
// The start medium is explicit: this prevents a water path from treating a
// glass face as a Water -> Air interface, and lets a glass path hand a water
// hit back to the real water transport instead of returning its raw albedo.
struct WaterTransmissionInfo {
    float waterPathLength;
    vec3 receiverWorldPos;
    vec3 receiverNormal;
    vec3 receiverViewDirection;
    vec3 receiverAlbedo;
    float receiverRoughness;
    float receiverF0Metal;
    float reachedUnderwaterReceiver;
    // 2026-10-03 修「水体散射/SSS 无太阳环境发光」：首次进入水体时对水面
    // 入口点做一次太阳可见性 trace（miss = 1 可见，命中 = 0 被遮挡）。
    // S_volume / 波峰透光 / 薄层 SSS 都吃 getTimeDrivenSkyLight* —— 那套只
    // 门控时间与天气，洞穴/室内的水在正午照样满功率发光。默认 1.0（未
    // trace 的路径保持旧行为：相机已在水下等场景由水下管线自己负责）。
    float waterSunVisibility;
    // 2026-10-05 次表面散射只响应水面上方的光源：水下接收器若是**发光体
    // 本身**（payload 自发光 + VLI 邻接辉光 × 水体路径吞吐），这部分辐亮度
    // 没有穿过水面进入水体，不该被 P1c 多散射项当成"从上面进来的光"在
    // 水面再叠一层 SSS 光斑。水体走查在接收器分支导出，多散射项扣除；
    // 非发光接收器恒为 0 ⇒ 逐位旧档。
    vec3 receiverSelfEmissionRadiance;
    // 2026-11-16：相机侧水柱透射（walker 逐段累乘的实值，含 GLASS_TINT 混色
    // 与深水 0.15 压透，不含界面 Fresnel）。焦散消费端与它逐位同式 —— walker
    // 算一次存这里，焦散不再重算。哨兵：负值 = 本 walker 未提供（消费端回退
    // 到旧的单发公式，逐位旧档）。
    vec3 cameraWaterColumnTransmittance;
    // 2026-11-27 (Batch AE): Reuse direct-sun water transmittance captured by
    // the water receiver path so underwater caustics avoid a duplicate sun trace.
    vec3 receiverSunTransmittance;
    float receiverSunTransmittanceValid;
};

// RR has one guide record per pixel.  When transmission contributes most of
// the pixel, that record must describe the real opaque receiver rather than
// the transparent front face which the camera ray first touched.
void applyTransparentReceiverRRGuides(
    WaterTransmissionInfo receiver,
    inout vec3 diffuseAlbedo,
    inout vec3 specularAlbedo,
    inout vec4 normalRoughness,
    inout float specularHitDistance
)
{
    int f0MetalCode =
        decodeF0MetalCode(
            receiver.receiverF0Metal
        );

    float metallic =
        decodeMetallicFromCode(
            f0MetalCode
        );

    vec3 N =
        normalize(
            receiver.receiverNormal
        );

    vec3 V =
        normalize(
            receiver.receiverViewDirection
        );

    float NdotV =
        abs(
            dot(N, V)
        );

    float roughness =
        clamp(
            receiver.receiverRoughness,
            0.0,
            1.0
        );

    diffuseAlbedo =
        receiver.receiverAlbedo *
        (1.0 - metallic);

    specularAlbedo =
        rrEnvBRDFApprox2(
            decodeBaseF0(
                f0MetalCode,
                receiver.receiverAlbedo
            ),
            roughness * roughness,
            NdotV
        );

    normalRoughness =
        vec4(N, roughness);

    // The opaque receiver's separate glossy-hit distance is not available
    // on this continuation.  Far keeps RR from treating the transparent
    // front-surface reflection distance as the receiver's specular event.
    specularHitDistance =
        65504.0;
}

// =====================================================
// guide roughness for a pixel whose radiance arrives through glass
//
// DLSS Ray Reconstruction reads the guide roughness to size its spatial
// filter, and the pane used to write 0.0 -- a perfect mirror.  A perfect
// mirror is the one surface RR deliberately never blurs, so the pixel kept
// whatever history it had and the *transmitted* content never averaged
// spatially.  Everything behind the glass is lit by the local-light
// estimator, which is 1-spp white noise by construction (see the note at
// its dispatch site), so "no spatial filter" meant "the grain never goes
// away": the pane itself looked clean while every pixel seen through it
// stayed noisy.
//
// The reference implementation does not have this failure mode because the
// guides for a transmitted pixel come from the receiver, with roughness on
// a hard 0.2 floor plus a relaxation term that grows when the path crossed
// a delta surface.  We already borrow the receiver's depth and diffuse
// albedo here; this finishes the job for the surface record.
//
// GI_GLASS_RECEIVER_GUIDE_MODE, for A/B on a live build:
//   0 = old behaviour: pane normal, roughness 0.0   （= 噪点留着，见下）
//   1 = pane normal kept, receiver roughness (floor + delta relaxation)
//   2 = receiver normal + receiver roughness        （当前默认）
//   3 = pane normal kept, PANE_SOLID_ROUGHNESS
//
// 2026-09-30: 默认由 0 改为 2。0 档把 0.0 粗糙度写进 guide，RR 把它判成完美
// 镜面并**对它关闭空间滤波**，于是透过玻璃看到的每一条内容（由本地光估计器
// 产生，本身就是 1spp 白噪声）永远不参与空间平均 —— 症状就是"玻璃里边有噪点"。
// transmitted 像素的取法一致），RR 的空间核就能真正作用在这片内容上。
// 3 档是本项目自己的折中（保留玻璃法线、0.02 粗糙度 ⇒ 只剩很窄的核），
// 若 2 档让玻璃观感变软，退回 3 档即可；0 档只作为 A/B 复现旧行为。
//
// Modes 1/2 copy the behind-glass receiver's surface record onto the guide.
// That is what made a pane read as a soft film: the single normal-roughness
// record has to serve the pane's own mirror lobe (alphaRoughness 0) AND a
// rough wall 30 blocks away at once, so the pane's reflection image rides a
// wide NRD spatial kernel and smears into fog.
//
// A pane is a solid delta surface.  Its normal and its near-zero roughness
// are the pane's own, always; the content behind it is reprojected through
// the separate receiver depth guide (rrTransparentReceiverWorldPos), which
// does not depend on this switch.  Mode 3 therefore keeps the pane's normal
// (sharp reflection) but pins a small NON-ZERO roughness instead of the hard
// 0.0 of mode 0, so NRD keeps a narrow kernel: solid pane, no mirror
// exclusion, and the receiver's own noise still gets a little filtering.
// =====================================================
#ifndef GI_GLASS_RECEIVER_GUIDE_MODE
#define GI_GLASS_RECEIVER_GUIDE_MODE 3
#endif

// Glass is a delta surface: near-zero roughness.  RR's roughness->radius
// mapping widens quickly, so this has to stay small or the pane fogs again.
// Raise it only to experiment with a slightly softer pane.
#ifndef PANE_SOLID_ROUGHNESS
#define PANE_SOLID_ROUGHNESS 0.02
#endif

// =====================================================
// 方案 B（2026-09-30）：玻璃像素写进 RR 深度图的是哪一张深度
//
//   0 = 旧行为：玻璃**后面接收面**的深度（主命中深度先写一次，随后被
//       覆盖）。接收面隔着玻璃面，在玻璃内部的视差 / 折射下逐帧跳动，
//       RR 的深度一致性判据会把这份跳动读成 disocclusion 并反复清历史，
//       表现就是"静止能压住、相机一动玻璃里就重新冒 1spp 噪点"。
//   1 = 新行为：玻璃面**自身**的深度（= rt_test.rgen:14286 那次主命中深度，
//       含 POM 虚拟面位移）。玻璃面是静止几何，重投影精确到亚像素；透过
//       玻璃看到的内容改为「稳定历史 + 空间滤波」而不是逐帧重投影。
//
// 玻璃面的一个点决定它后面的接收面（透射射线在世界上是固定的），所以按
// 玻璃面重投影后落回的是**同一个**玻璃面点 ⇒ 后接的接收面记录（法线/粗糙度
// /反照率，见 GI_GLASS_RECEIVER_GUIDE_MODE）依然自洽，不冲突。
//
// 水面不走这条路：水底接收面本身稳定，保持旧行为（GI 水路径不设下面的
// rrTransparentReceiverIsPane）。
//
// A/B：-DGI_GLASS_PANE_DEPTH_GUIDE=1 切到玻璃面深度。
//
// 2026-09-30 实测（用户侧）：切到玻璃面深度后"几乎没啥改善，还是噪" ⇒ 这份
// 噪点不是 RR 的重投影落点造成的，接收面深度按 RR 的契约保留（深度描述的
// 就是 currentColor 里那份辐射的面）。开关留着做 A/B。
// =====================================================
#ifndef GI_GLASS_PANE_DEPTH_GUIDE
#define GI_GLASS_PANE_DEPTH_GUIDE 1
#endif

// Hard floor.  Below this RR classifies the surface as a mirror and stops
// filtering it spatially, which is exactly the state that produced the
// noise.
#ifndef RR_GUIDE_ROUGHNESS_FLOOR
#define RR_GUIDE_ROUGHNESS_FLOOR 0.2
#endif

// Optional extra radius on top of the floor, default OFF.
//
// The reference does widen the radius across a delta crossing, but it derives
// the amount from the *screen-space spread of the neighbour normals*
// (nrd_compose/main.comp.slang:37-44) -- and in its form that term is zero on
// a flat receiver, because the transmitted endpoint's normal is the
// refraction-folded receiver normal, which its neighbours share.  One ray per
// pixel cannot see its neighbours, and a constant added to every glass pixel
// would blur the whole window instead of just its silhouette, so the honest
// transcription is 0.  Raise it only as an experiment, e.g.
// -DRR_GUIDE_DELTA_RELAXATION=0.25.
#ifndef RR_GUIDE_DELTA_RELAXATION
#define RR_GUIDE_DELTA_RELAXATION 0.0
#endif


// =====================================================
// indirect light behind glass - one cosine bounce from the receiver
//
// The T1 transmission stopgap shades the behind-glass receiver with emission
// and direct sun and nothing else: the evaluator itself says so
// ("Local Light / GI participation is reserved for T9",
// evaluateOpaqueSurfaceRadiance).  Every GI bounce in this renderer lives in
// the opaque branch of the primary loop, and a straight-through transmission
// stopgap never re-enters it.  So a wall read through a pane keeps only its
// direct sun and an albedo * noDirectLightAmbient floor, while the very same
// wall read directly is lit by its neighbours.  A corridor is the worst case:
// its vertical walls get no direct sun at all, so their entire visible value
// is the bounce off the sunlit floor - and that is exactly the term that was
// missing, which is why everything behind a pane reads black.
//
// Water never had this gap: its receiver walker adds
// tracePrimaryWaterReceiverIndirectReworked (12738).  This closes the
// asymmetry with the same walker, the same distance gating and one extra
// traversal per pane pixel (skipped past 160 blocks, like water's).
//
// 1 = add one cosine-weighted receiver bounce
// 0 = emission+sun-only receiver (the 09-23 behaviour, 视效回退)
// =====================================================
#ifndef GI_GLASS_RECEIVER_INDIRECT
// =====================================================
// 2026-10-10：由 0 改为 1（用户报"半透明整体偏暗一档以上"，长期存在）。
//
// 原值 0 是 2026-09-23 的 A/B 回退档：玻璃后的接收面只算 emission + 直射太阳 +
// 环境项，**把场景弹射（GI）整项拿掉**。作者自己在下面 13573-13578 就写下了后果：
//   "a corridor wall behind a pane is black while the same wall seen directly
//    is lit by the bounce off the sunlit floor"
// —— 隔着玻璃看，等于把间接光（不透明路径里权重 1.8 的最大一项）全部删掉，
// 所以越是室内/越依赖弹射的地方越黑，且表现为"整体压暗一档以上"而不是缺一块色。
//
// 代价：玻璃后的接收面多一条余弦弹射射线（仅当主射线穿过半透明时才发生）。
// 若要回到旧观感，把这里改回 0 即可（单点开关）。
// =====================================================
#define GI_GLASS_RECEIVER_INDIRECT 1
#endif



// =====================================================
// analytic pane radiance kept out of the denoiser
//
// A pane's contribution has two halves with opposite statistics:
//
//   transmitted  = behindRadiance * glassTransmission   -- the lit world seen
//                  through the sheet.  Its local-light term is 1-spp white
//                  noise, and it wants every denoiser we have.
//   reflection   = traceGlassSceneReflectionOneRay() * glassReflection
//   solar gloss  = analytic GGX against the sun
//                  -- both are closed-form.  One deterministic ray and one
//                  BRDF evaluation, no estimator, therefore no variance.
//
// Handing the second half to DLSS Ray Reconstruction can only hurt it: RR
// reprojects and filters every sample it is given, and the pane's guide now
// describes the *receiver* behind the glass (see GI_GLASS_RECEIVER_GUIDE_MODE
// above), so the mirror image of the window frame has nothing to anchor to.
// That is the coupling the reference implementation breaks by keeping its
// delta-surface radiance in `nrdStableRadiance` and adding it back only after
// the last denoised plane (nrd_compose/main.comp.slang:141-153).
//
// Same shape here: subtract the analytic half before the RT colour leaves the
// raygen, add it back in the display transform after RR.
//
// ⚠ Only exactly invertible when the RR input and output are the same
// resolution, i.e. rrMode 3 (DLAA).  At a scaling preset the add-back reads a
// render-resolution buffer at output resolution, so the pane's mirror lobe
// comes back at render resolution instead of RR's reconstructed resolution.
//
// The default lives at the top of the file, next to binding 54.

float rrTransmittedGuideRoughness(float receiverRoughness)
{
    // Floor raised for the behind-glass content only, per
    // GI_GLASS_RR_ROUGHNESS_FLOOR: a wider RR kernel on exactly the content that
    // has no G-buffer record of its own.
    return clamp(
        max(
            max(RR_GUIDE_ROUGHNESS_FLOOR, GI_GLASS_RR_ROUGHNESS_FLOOR),
            clamp(receiverRoughness, 0.0, 1.0)
        ) +
        RR_GUIDE_DELTA_RELAXATION,
        0.0,
        1.0
    );
}