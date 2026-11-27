
// Forward declaration only: the definition (11906) sits below
// tracePrimaryGenericTransparentRadiance, which now calls it for glass
// receivers as well.  `startInsideWater` is a parameter rather than a
// hard-coded true so a receiver reached through a pane in AIR does not run its
// bounce through Beer-Lambert water absorption.  Both the parameter and the
// call it enables are guarded by GI_GLASS_RECEIVER_INDIRECT, so switching the
// feature off reproduces the previous shader exactly.
// 2026-11-27: 玻璃链已改为 100% 确定性零噪点着色，不再依赖随机 GI walker。

// One straight continuation for exact clear/stained glass.  The dedicated
// Any-Hit path skips later glass faces, so this trace reaches either the
// real opaque receiver, water transport, or the sky.  Glass never owns a
// refractive or POM branch.
//
// Dead code: no call sites remain after the Orion thin-sheet port moved
// the primary camera pane to tracePrimaryGenericTransparentRadiance.  Kept
// under #if 0 for future reference (it used the pre-port pure-multiplicative
// blockTransmission * receiverRadiance model).
#if 0
vec3 tracePrimarySimpleGlassRadiance(
    vec3 glassSurfacePosition,
    vec3 rayDirection,
    vec3 glassTint,
    float glassAlpha,
    vec3 sunDirection,
    out WaterTransmissionInfo transmissionInfo
)
{
    transmissionInfo.waterPathLength = 0.0;
    transmissionInfo.receiverWorldPos = vec3(0.0);
    transmissionInfo.receiverNormal = vec3(0.0, 1.0, 0.0);
    transmissionInfo.receiverViewDirection = vec3(0.0, 0.0, 1.0);
    transmissionInfo.receiverAlbedo = vec3(0.0);
    transmissionInfo.receiverRoughness = 1.0;
    transmissionInfo.receiverF0Metal = 0.0;
    transmissionInfo.reachedUnderwaterReceiver = 0.0;
    transmissionInfo.waterSunVisibility = 1.0;
    transmissionInfo.receiverSelfEmissionRadiance = vec3(0.0);
    transmissionInfo.cameraWaterColumnTransmittance = vec3(-1.0);
    transmissionInfo.receiverSunTransmittance = vec3(0.0);
    transmissionInfo.receiverSunTransmittanceValid = 0.0;

    vec3 straightDirection = normalize(rayDirection);
    vec3 blockTransmission =
        computeGlassBlockTransmission(glassTint, glassAlpha, 0);

    hit.hitDistance = 1.0;
    hit.state = makeTraceState(RT_PURPOSE_PASSTHROUGH);

#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
    serTraceRay(
        glassSurfacePosition + straightDirection * 0.01,
        0.01,
        straightDirection,
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
            glassSurfacePosition + straightDirection * 0.01,
            0.01,
            straightDirection,
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
            glassSurfacePosition + straightDirection * 0.01,
            0.01,
            straightDirection,
            10000.0,
            0
        );
    }
#endif

    if(payload.hitDistance < 0.0)
    {
        return blockTransmission * unpackAlbedoUnorm(payload.albedoPacked);
    }

    vec3 hitPos =
        glassSurfacePosition +
        straightDirection * (payload.hitDistance + 0.01);
    float hitMaterialId = getLegacyMaterialId(payload.state);

    // A glass ray can enter real water.  Transfer to the authoritative water
    // walker instead of returning the water atlas colour.
    if(hitMaterialId > 0.5 && hitMaterialId < 2.5)
    {
        bool exitingWater = hitMaterialId > 1.5;
        vec3 refractedDirection = refract(
            straightDirection,
            payload.normal,
            exitingWater ? 1.33 : (1.0 / 1.33)
        );
        bool totalInternalReflection =
            dot(refractedDirection, refractedDirection) < 1.0e-6;
        if(totalInternalReflection)
        {
            refractedDirection = reflect(
                straightDirection,
                payload.normal
            );
        }

        WaterTransmissionInfo waterInfo;
        vec3 waterRadiance = tracePrimaryTransparentRadiance(
            hitPos + refractedDirection * 0.001,
            refractedDirection,
            totalInternalReflection ? exitingWater : !exitingWater,
            sunDirection,
            false,
            0.0,
            waterInfo
        );
        transmissionInfo = waterInfo;
        return blockTransmission * waterRadiance;
    }

    // Any-Hit normally removes a second glass face.  If it is still reported,
    // continue past it through the existing straight transparent walker;
    // returning the face albedo here discarded a nearby emitter/receiver.
    if(hitMaterialId >= 2.5)
    {
        WaterTransmissionInfo continuationInfo;
        vec3 continuedRadiance = tracePrimaryTransparentRadiance(
            hitPos + straightDirection * 0.001,
            straightDirection,
            false,
            sunDirection,
            false,
            0.0,
            continuationInfo
        );
        transmissionInfo = continuationInfo;
        return blockTransmission * continuedRadiance;
    }

    vec3 hitNormal = payload.normal;
    vec3 hitAlbedo = unpackAlbedoUnorm(payload.albedoPacked);
    transmissionInfo.receiverWorldPos = hitPos;
    transmissionInfo.receiverNormal = hitNormal;
    transmissionInfo.receiverViewDirection = -straightDirection;
    transmissionInfo.receiverAlbedo = hitAlbedo;
    transmissionInfo.receiverRoughness = payload.roughness;
    transmissionInfo.receiverF0Metal = payload.f0Metal;
    transmissionInfo.reachedUnderwaterReceiver = 1.0;
    transmissionInfo.waterSunVisibility = 1.0;
    // 玻璃后的接收器不喂水体多散射项（该字段只在水体走查的接收器分支导出）。
    transmissionInfo.receiverSelfEmissionRadiance = vec3(0.0);

    vec3 receiverRadiance = evaluateOpaqueSurfaceRadiance(
        hitPos,
        hitNormal,
        -straightDirection,
        hitAlbedo,
        payload.roughness,
        payload.f0Metal,
        payload.emission +
            evaluateVLIEmitterRadianceAtHit(hitPos, hitNormal, hitAlbedo),
        payload.pomSelfShadow,
        sunDirection
    );

    return blockTransmission * receiverRadiance;
}
#endif

// =====================================================
// 2026-10-07 修"玻璃反射里的天空色半圆"
//
// 旧判据（见下方 13405 起的注释）是"反射方向朝上 ⇒ 大概率天空"，据此在低 Fresnel
// 区直接采样天空 LUT、**完全不做遮挡检测**。但**朝上 != 天空**：天花板、屋檐、
// 树冠、山脊都在地平线以上。于是
//     { 低 Fresnel 区（NdotV ≳ 0.63，平面上是一个圆） }
//   ∩ { mirrorDir.y > 0.05（一个半平面） }
// 的交集 = 玻璃面上一个**天光色半圆** —— 用户报告的形状正是这个交集的几何。
//
// 现在把"朝上"换成一条**真正的可见性探针**：只问"有没有命中"（RT_PURPOSE_CLASSIFY
// 不取 albedo、不评估材质、不进 PBR），这段距离内确实没有任何几何体，才敢用解析
// 天空；否则退回真实镜面 trace（那才是正确内容）。
//   · 探针只证明 "maxDistance 格内无遮挡"，因此比旧判据严格得多；
//   · 代价 = 候选像素多一条分类射线。比对"所有候选都打完整镜面 trace"便宜，因为
//     后者命中后还要跑 evaluateOpaqueSurfaceRadiance 一整套着色。
//   · 回旧档：-DGI_GLASS_ANALYTIC_REFLECTION_PROBE=0（会有半圆）。
//
// 起点偏置 / ray flags / SBT 偏移逐字照抄 traceSunTransmittanceThroughWater 的
// 分类段，miss 约定也一致：rmiss 把
// payload.hitDistance 置负。
// =====================================================
#if !VPT_GLASS_DIELECTRIC_REWRITE
bool glassMirrorPathIsClear(
    vec3 origin,
    vec3 dir,
    float maxDistance
)
{
    hit.hitDistance = 1.0;
    hit.state = makeTraceState(RT_PURPOSE_CLASSIFY);
    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntRayIssued();
    #endif
    traceRayEXT(
        topLevelAS,
        gl_RayFlagsNoneEXT,
        0xcfu,
        0,
        0,
        0,
        origin,
        0.02,
        dir,
        maxDistance,
        0
    );
    // 无命中 ⇒ 这段距离内确实没有东西挡在反射方向上。
    return payload.hitDistance < 0.0;
}
#endif

// ============================================================================
// 2026-10-26：半透明 alpha blend 拆 pass 的总闸（docs §7.4 步骤 3）
//
// 1 = 玻璃分支只**写旁路图**并把**未合成**的背景辐射返回给 rtOutput，
//     由 glass_alpha_blend.comp + glass_alpha_finish.comp 在屏幕空间合成。
// 0 = 逐位回到本轮之前的"rgen 就地合成"（下面 transmittedRadiance 那条路）。
//
// A/B 口径（两条必须同时满足才生效）：
//   · VPT_GLASS_ALPHA_BLEND=1（alpha blend 口径本身）
//   · VPT_GLASS_ALPHA_BLEND_SURFACE_RADIANCE=1（加性合成，第三轮的口径）
// 只把本宏置 0 时，宿主仍会清 74 号图 ⇒ 新 pass 找不到半透明像素、整条链惰性，
// 画面 = 旧档逐位不变。这就是 M1/M2 的回退开关。
//
// ⚠ PANE_SPECULAR=1 时强制关掉本宏：那一档要叠裸 Fresnel 反射 + 太阳 GGX 高光，
//   而那两条都在本函数**后半段**（镜面 trace + 解析天空）求和；提前返回会静默
//   丢掉它们。alpha blend 口径（Remix）本来就没有反射项，所以默认档不受影响。
// ============================================================================
// 2026-10-26（第四轮）：定义已上提到 glass_chain_common.glsl —— 两个 rahit
// 也要看到同一个值（选层语义改在 any-hit 里）。见那里的长注释。

#if VPT_GLASS_ALPHA_BLEND && VPT_GLASS_ALPHA_BLEND_DEFERRED_COMPOSITE
// 三张旁路图的唯一写点。参数语义与消费方（glass_alpha_blend.comp）逐项对应：
//   surfaceWeight      = 73.rgb（已乘本层 a 与该层之前的累积透射）
//   layerAlpha         = 73.a  = a_0
//   chainTransmittance = 74.rgb = PI(1-a_i)（第 2..N 层，any-hit 累乘）
//   paneHitT           = 74.a  （邻居搜索的共面/深度判据）
//   surfaceLight       = 75.rgb = E_bg
void storeGlassAlphaBlendDeferred(
    vec3 surfaceWeight,
    float layerAlpha,
    vec3 chainTransmittance,
    float paneHitT,
    vec3 surfaceLight)
{
    const ivec2 alphaBlendPixel =
        ivec2(gl_LaunchIDEXT.xy);

    imageStore(
        glassAlphaBlendGbufferImage,
        alphaBlendPixel,
        vec4(
            max(surfaceWeight, vec3(0.0)),
            clamp(layerAlpha, 0.0, 1.0)));

    imageStore(
        glassAlphaBlendMetaImage,
        alphaBlendPixel,
        vec4(
            clamp(chainTransmittance, vec3(0.0), vec3(1.0)),
            max(paneHitT, 1.0e-3)));

    imageStore(
        glassAlphaBlendLightImage,
        alphaBlendPixel,
        vec4(max(surfaceLight, vec3(0.0)), 1.0));
}
#endif

// ============================================================================
// 半透明重写（2026-11-02）：电介质界面 + 介质内传播
//
// 参考实现（用户提供）：Caustica-rewrite
//   · docs/GLASS_COVERAGE_20260914.md
//       "Glass blocks and panes use opaque coverage, OpenPBR transmission weight 1,
//        and an interior dielectric volume. ... Base texture alpha neither removes
//        surface samples nor scales transmission."
//   · packages/renderer-raytracing/src/main/resources/caustica/shaders/world/
//     trace_transport.slang:68-107（traceFresnel / traceLobes）
//       精确电介质 Fresnel；折射 refract(dir, N, etaI/etaT)；
//       透射权重 (1-F) * transmission_color * (etaI/etaT)^2；
//       跨界面时把路径的介质状态换成命中体的 IOR。
//   · packages/renderer-raytracing/src/main/resources/caustica/shaders/world/
//     surface_bsdf.slang:113-175
//       boundaryWeight = dielectricWeight * transmission_weight；厚壁介质的
//       Fresnel 与折射归体积输运所有，表面闭包只保留其余部分。
//
// 与旧实现（alpha blend 旁路合成 + 玻璃链 any-hit 累进 + 剖面不透明度）的差别：
//   1. 贴图 alpha 只当覆盖率，不再参与任何"透明度/混合"；a = 0.4 一类常量消失。
//   2. 反射与透射按精确 Fresnel 分配，两瓣都确定性求值（不抽签、无方差）。
//   3. 透射是真的穿过介质：进入面折射 -> 介质内走到对面 -> 出射面折射，
//      逐界面乘 (1-F)、染色与 eta^2。玻璃体积本身不带吸收（参考实现的体积只有 IOR）。
//   4. 73/74/75 三张旁路图不再被写（74.y 恒 0 => glass_alpha_finish.comp 空转），
//      玻璃链 any-hit 分支也不再被这条射线触发。资源保留，仅用于 A/B 回退。
//
// 回退：-DVPT_GLASS_DIELECTRIC_REWRITE=0 => 逐位回到本轮之前的 alpha blend 链路。
// ============================================================================
#ifndef VPT_GLASS_DIELECTRIC_REWRITE
#define VPT_GLASS_DIELECTRIC_REWRITE 1
#endif
#ifndef ORION_GLASS_REMIX_PSTR_GUIDES
#define ORION_GLASS_REMIX_PSTR_GUIDES 1
#endif
#ifndef ORION_GLASS_TWO_INTERFACE_REFRACT
#define ORION_GLASS_TWO_INTERFACE_REFRACT 1
#endif
#ifndef ORION_GLASS_RADIANCE_DOMINANCE_FIX
#define ORION_GLASS_RADIANCE_DOMINANCE_FIX 1
#endif
#ifndef ORION_GLASS_FAR_NOISE_FIX
#define ORION_GLASS_FAR_NOISE_FIX 1
#endif
#ifndef ORION_GLASS_LOCAL_RIS_SAMPLES
#define ORION_GLASS_LOCAL_RIS_SAMPLES 1
#endif
#ifndef VPT_GLASS_MAX_INTERFACES
// 一条视线最多跨过几个玻璃界面。1 块玻璃 = 2 个界面（进入 + 离开），
// 8 允许"眼睛 -> 玻璃板×n -> 接收面"的叠放（真实遍历下 1 块板必付 2 个界面，
// 旧位移档因为起点越过出射面只算 1 个）。
#define VPT_GLASS_MAX_INTERFACES 8
#endif
#ifndef VPT_GLASS_BOUNDARY_TINT_EACH_FACE
// 染色乘几次。一条视线穿过一块玻璃会跨 **2 个界面**（进入 + 离开）。
//   0 = 只在进入界面乘一次 ⇒ 等效"一片玻璃一层色"（**当前默认**）。
//   1 = 逐界面各乘一次 ⇒ 等效 tint^2，与参考实现（Caustica）逐字一致，
//       但实测偏暗：染色玻璃红 (0.94,0.24,0.24) 平方后次通道只剩 1/4。
// 想回到参考实现原样：-DVPT_GLASS_BOUNDARY_TINT_EACH_FACE=1。
#define VPT_GLASS_BOUNDARY_TINT_EACH_FACE 0
#endif
#ifndef GI_ROUGH_TRANSMISSION
// 磨砂（致密体透射方向的锥形抖动）总闸，与旧档同名同值；旧档的 #ifndef 不会重定义。
#define GI_ROUGH_TRANSMISSION 1
#endif
#ifndef GI_ROUGH_TRANSMISSION_MAX_ANGLE
// 满粗糙度的锥半角（弧度，约 20 度）。
#define GI_ROUGH_TRANSMISSION_MAX_ANGLE 0.35
#endif

// 精确的非偏振电介质 Fresnel（逐字口径见参考 trace_transport.slang:68-75）。
// 与旧档的 Schlick(F0 = 0.04) 的关键差别：掠射角 100% 反射由公式自然给出，
// 不再依赖一个手调的 F0；全内反射由 sinT2 >= 1 判定。
float glassDielectricFresnel(float cosThetaI, float etaI, float etaT)
{
    const float eta = etaI / max(etaT, 1.0e-6);
    const float sinT2 = eta * eta * max(0.0, 1.0 - cosThetaI * cosThetaI);
    if(sinT2 >= 1.0)
    {
        return 1.0;
    }
    const float cosT = sqrt(max(0.0, 1.0 - sinT2));
    const float rs =
        (etaI * cosThetaI - etaT * cosT) /
        max(etaI * cosThetaI + etaT * cosT, 1.0e-6);
    const float rp =
        (etaT * cosThetaI - etaI * cosT) /
        max(etaT * cosThetaI + etaI * cosT, 1.0e-6);
    return clamp(0.5 * (rs * rs + rp * rp), 0.0, 1.0);
}

// ----------------------------------------------------------------------------
// 主入口：玻璃面命中后的完整辐射
//
//   C = F * L_reflect + (1 - F) * Tint * L_transmitted        （每个板面只付一次）
//
// L_reflect   : 一条确定性镜像射线（复用 traceGlassSceneReflectionOneRay，
//               它同时导出 RR 的反射端点 guide 与反射命中距离）。
// L_transmitted: **平行板折射**——入射面折射一次求位移，穿过板厚后出射方向
//               恢复成入射方向，再继续走查。没有临界角、没有 TIR、没有第二份
//               Fresnel / eta^2，因此不会因为"没认出出射面"而整块变黑。
// ----------------------------------------------------------------------------
// 所有玻璃族（普通玻璃 / 染色玻璃 / 遮光玻璃 / 冰 / 粘液 / 蜂蜜 / 地狱门）
// 共用这一条路：贴图 alpha 只当覆盖率，Fresnel 与位移由 IOR 决定。
#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_RADIANCE_DOMINANCE_FIX
bool g_glassReflectionDominatesPixel = false;
#endif
vec3 tracePrimaryDielectricGlassRadiance(
    vec3 glassSurfacePosition,
    vec3 rayDirection,
    vec3 glassNormal,
    vec3 glassTint,
    float glassRoughness,
    int transparentProfile,
    vec3 sunDirection,
    vec3 historyKeyPosition,
    out WaterTransmissionInfo transmissionInfo
#if RR_STABLE_GLASS_RADIANCE
    , out vec3 stableGlassRadiance
#endif
)
{
#if RR_STABLE_GLASS_RADIANCE
    stableGlassRadiance = vec3(0.0);
#endif
#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_RADIANCE_DOMINANCE_FIX
    g_glassReflectionDominatesPixel = false;
#endif
    transmissionInfo.waterPathLength = 0.0;
    transmissionInfo.receiverWorldPos = vec3(0.0);
    transmissionInfo.receiverNormal = vec3(0.0, 1.0, 0.0);
    transmissionInfo.receiverViewDirection = vec3(0.0, 0.0, 1.0);
    transmissionInfo.receiverAlbedo = vec3(0.0);
    transmissionInfo.receiverRoughness = 1.0;
    transmissionInfo.receiverF0Metal = 0.0;
    transmissionInfo.reachedUnderwaterReceiver = 0.0;
    transmissionInfo.waterSunVisibility = 1.0;
    transmissionInfo.receiverSelfEmissionRadiance = vec3(0.0);
    transmissionInfo.cameraWaterColumnTransmittance = vec3(-1.0);
    transmissionInfo.receiverSunTransmittance = vec3(0.0);
    transmissionInfo.receiverSunTransmittanceValid = 0.0;

    const vec3 viewDirection = normalize(rayDirection);

    // 界面法线：**着色法线**（material_resolve 已把它翻到朝向来向）。
    // 旧模型用 vptGeometricFaceNormalFromRay() 把命中点吸附到最近的体素格子面，
    // 1/16 厚的玻璃板在不同像素上会吸附到不同轴 ⇒ 法线逐像素跳变 ⇒ 三角形花纹。
    vec3 entryNormal = normalize(glassNormal);
    if(dot(entryNormal, entryNormal) < 0.5)
    {
        entryNormal = -viewDirection;
    }
    if(dot(entryNormal, viewDirection) > 0.0)
    {
        entryNormal = -entryNormal;
    }

    // 进入介质：空气 -> 该玻璃体的 IOR（玻璃 1.5 / 冰 1.309）。
    const float entryIor = max(glassIorForProfile(transparentProfile), 1.0);
    const float entryCosine = clamp(dot(-viewDirection, entryNormal), 1.0e-4, 1.0);
#ifndef VPT_GLASS_MAX_FRESNEL
// 入射 Fresnel 上限。法线接近切向 / 命中点落在格边界时 F 会被算到 ≈1，
// 透射于是拿到 (1-F)≈0 ⇒ 整片纯黑（2026-11-03 用户实测）。
//
// 2026-11-13（对齐参考实现 Caustica-rewrite trace_transport.slang:84-93）：
// 参考的界面瓣是**精确 Fresnel、无钳制**（reflection = boundaryWeight × F，
// F 由 traceFresnel 精确公式给出）。0.35 的旧档把 78° 以上的真实反射全部
// 压平——而玻璃反射恰恰是在掠射角才肉眼可见（F(60°)=0.10, F(75°)=0.26,
// F(85°)=0.61）。11-03 黑片伪影的根因是退化法线（11-12 已改用 shading
// normal），不在 Fresnel 本身；0.9 只拦病态退化（F≥0.9 ⇒ 透射 <10% 的
// 整片黑），88° 以内的全部真实角度物理不变。
// A/B：-DVPT_GLASS_MAX_FRESNEL=1.0（= 参考档，纯物理）；
//      -DVPT_GLASS_MAX_FRESNEL=0.35（= 2026-11-03 旧档）。
#define VPT_GLASS_MAX_FRESNEL 0.9
#endif
    const float entryFresnelRaw =
        glassDielectricFresnel(entryCosine, 1.0, entryIor);
    const float entryFresnel =
        clamp(entryFresnelRaw, 0.0, VPT_GLASS_MAX_FRESNEL);
    const vec3 surfaceTint = clamp(glassTint, vec3(0.0), vec3(1.0));

    vec3 slabReflection;
#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_TWO_INTERFACE_REFRACT && ORION_GLASS_REMIX_PSTR_GUIDES
    // Match dxvk-remix (!isThinWalled): reflection uses single-interface Fresnel
    // since two-interface transmission already pays (1 - F_in) * (1 - F_out).
    slabReflection = vec3(entryFresnel);
#else
    vec3 slabTransmissionUnused;
    glassThinSheetFactorsScalarF(
        entryFresnel,
        surfaceTint,
        slabReflection,
        slabTransmissionUnused);
    slabReflection = clamp(slabReflection, vec3(0.0), vec3(VPT_GLASS_MAX_FRESNEL));
#endif

    rrGlassPaneFresnel = clamp(
        dot(slabReflection, vec3(0.2126, 0.7152, 0.0722)),
        0.0,
        VPT_GLASS_MAX_FRESNEL);
    rrGlassPaneTransmissionFilter =
        clamp((1.0 - entryFresnel) * surfaceTint, vec3(0.0), vec3(1.0));

#if GI_GLASS_DOMINANT_GUIDE
    // RR 的端点归属判据：两瓣的能量权重由同一份 Fresnel 分配给出。
    glassLobeReflectionWeight = slabReflection;
    glassLobeTransmissionWeight = vec3(1.0 - entryFresnel) * surfaceTint;
    glassLobeWeightsValid = true;
#endif

    const float glassCamDist = length(glassSurfacePosition - cam.viewInverse[3].xyz);

    // ---------------- 反射瓣（确定性） ----------------
    // 掠射端就算 F 很小也保留这条射线：它是 RR 反射端点 guide 与
    // rrGlassReflectionHitDistance 的唯一来源。
#ifndef VPT_GLASS_REFLECTION
#define VPT_GLASS_REFLECTION 1
#endif
    vec3 reflectionRadiance = vec3(0.0);
    vec3 sunSpecularGlint = vec3(0.0);
#if VPT_GLASS_REFLECTION
    if(entryFresnel > 1.0e-4)
    {
        // 2026-11-27: 取消假天空短路，全距离/全角度统一走确定性极速反射射线，
        // 保证玻璃反射永不消失。
        reflectionRadiance =
            traceGlassSceneReflectionOneRay(
                glassSurfacePosition,
                entryNormal,
                -viewDirection,
                sunDirection);

        // Narrow shadow-tested direct-sun GGX glint (gated by dot(R, L) > 0.92
        // so >98% of translucent pixels pay zero extra shadow rays).
        const vec3 reflV = -viewDirection;
        const vec3 reflL = normalize(sunDirection);
        const float reflNdotL = max(dot(entryNormal, reflL), 0.0);
        const vec3 reflDir = reflect(viewDirection, entryNormal);
        if(reflL.y > 0.0 && reflNdotL > 0.0 && dot(reflDir, reflL) > 0.92)
        {
            const float GLASS_MIRROR_SUN_ROUGHNESS = 0.04;
            const vec3 reflH = normalize(reflV + reflL);
            const float reflVdotH = max(dot(reflV, reflH), 0.0);
            const float reflSunF = glassDielectricFresnel(reflVdotH, 1.0, entryIor);
            const float reflD = distributionGGX(entryNormal, reflH, GLASS_MIRROR_SUN_ROUGHNESS);
            const float reflG = geometrySmith(entryNormal, reflV, reflL, GLASS_MIRROR_SUN_ROUGHNESS);
            const float reflSpecBrdf =
                (reflD * reflG * reflSunF) / max(4.0 * entryCosine * reflNdotL, 1.0e-4);
            if(reflSpecBrdf * reflNdotL > 1.0e-4)
            {
                const vec3 sunTrans = traceSunTransmittanceAir(
                    glassSurfacePosition + entryNormal * 0.02,
                    reflL);
                if(max(sunTrans.r, max(sunTrans.g, sunTrans.b)) > 0.0)
                {
                    const vec3 sunRad =
                        getTimeDrivenSkyLightColor() *
                        3.2 *
                        getTimeDrivenSkyLightRadianceScale() *
                        sunTrans *
                        c3CloudSunTransmittance(glassSurfacePosition + entryNormal * 0.02, reflL);
                    sunSpecularGlint = min(sunRad * reflSpecBrdf * reflNdotL, vec3(6.0));
                }
            }
        }
    }
#endif

    // ---------------- 透射瓣（平行板） ----------------
    // 每个板面只付一次 (1-F)·tint；出射方向不改变，所以什么都不用再乘。
    vec3 throughput = (1.0 - entryFresnel) * surfaceTint;
    vec3 transmittedRadiance = vec3(0.0);

#ifndef VPT_GLASS_TRANSMIT_BENT
// 透射方向弯折开关。0 = 旧档（钉死原视线方向：只挪起点，正入射时横向位移
// 恒为 0 ⇒ 观感"没有折射"，2026-11-13 用户复测确认）。
// 1 = 沿折射方向走（参考实现 trace_transport.slang:88 的 transmittedDirection
// = refract(...)；也是旧 v3 弯折模型 glassWalkExitDirection = refract(...) 的
// 语义）——"看得见的折射"就是这条弯折视线。
#define VPT_GLASS_TRANSMIT_BENT 1
#endif
#ifndef VPT_GLASS_BENT_STRENGTH
// 弯折强度（0~1）：1 = 完整折射方向，0.5 = 与视线方向各半。
// 觉得背景位移过强时调小：-DVPT_GLASS_BENT_STRENGTH=0.5。
#define VPT_GLASS_BENT_STRENGTH 1.0
#endif

    // 从"穿过第一块板"之后的点开始沿**透射方向**继续。
    vec3 travelDirection = viewDirection;
#if VPT_GLASS_TRANSMIT_BENT
    {
        const vec3 bentDirection = refract(
            viewDirection,
            entryNormal,
            1.0 / max(entryIor, 1.0e-6));
        if(dot(bentDirection, bentDirection) > 1.0e-8)
        {
            travelDirection = normalize(mix(
                viewDirection,
                normalize(bentDirection),
                clamp(VPT_GLASS_BENT_STRENGTH, 0.0, 1.0)));
        }
    }
#endif
    vec3 travelOrigin = glassSurfacePosition;
    bool reachedEndpoint = false;
    float accumulatedGlassOpticalDist = 0.0;
    bool nextStepSkipGlass = false;
#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_TWO_INTERFACE_REFRACT
    bool insideGlass = true;
    float currentIor = entryIor;
#endif

    bool hitOpaqueReceiver = false;
    vec3 opqHitPos = vec3(0.0);
    vec3 opqHitNormal = vec3(0.0, 1.0, 0.0);
    vec3 opqHitAlbedo = vec3(0.0);
    float opqHitRoughness = 1.0;
    float opqHitF0Metal = 0.0;
    vec3 opqHitEmission = vec3(0.0);
    float opqHitPomSelfShadow = 1.0;

    const int baseGlassInterfaces =
        (glassCamDist < 24.0)
            ? 4
            : ((glassCamDist < 64.0) ? 3 : 2);
    const int effectiveGlassMaxInterfaces =
        (cam.frameCount != 0xFFFFFFFFu)
            ? min(VPT_GLASS_MAX_INTERFACES, baseGlassInterfaces)
            : 0;

    for(int slabStep = 0; slabStep < effectiveGlassMaxInterfaces; ++slabStep)
    {
        if(max(throughput.r, max(throughput.g, throughput.b)) < 0.03)
        {
            break;
        }
        // 当前板面：第一轮用入射面（已在函数入口算过 F），后续轮次用循环里遇到的
        // 新玻璃面重新算。这里先把"离开当前板"的位移做掉。
#ifndef VPT_GLASS_REFRACTION_MODEL
// 2 = 薄壁 + 常量厚度位移（**默认**，Remix 的 thinWallThickness 口径）
// 1 = 平行板（板厚由几何反推）
// 0 = 直穿（无位移）
#define VPT_GLASS_REFRACTION_MODEL 2
#endif
#ifndef VPT_GLASS_THIN_WALL_THICKNESS
// 薄壁名义厚度（格）。
// 2026-11-13 晚（用户复测"仍没有折射"）：0.05 = 1/20 格的位移在任何距离下都
// 不可见——"没有折射"是**数值**问题而非链路问题。提到 0.3（上限 0.15，见下）：
// 0.15 > 玻璃板厚 0.125 ⇒ 续射线起点越过出射面，玻璃板变成单界面——
// 顺带修掉染色玻璃"入射面 + 出射面各付一次 dye/Fresnel"的双重染色（旧 0.03
// 位移时起点仍在板内 ⇒ 出射面被再次命中 ⇒ 染色按 dye×dye 重复乘）。
// A/B：-DVPT_GLASS_THIN_WALL_THICKNESS=0.05 回旧档。
#define VPT_GLASS_THIN_WALL_THICKNESS 0.3
#endif
#ifndef VPT_GLASS_CLEAR_REFRACTION
// 2026-11-13（用户要求）：清玻璃（profile 0）也吃薄壁位移折射——与遮光玻璃/
// 冰/粘液/蜂蜜同一套 thinWallThickness 口径（位移 min(0.05/cos, 0.03) 格，
// 出射方向恢复入射方向）。清玻璃板厚 1/16 格 > 0.05 名义厚度，位移不会跨出
// 自己的格子（正入射时）。
// ⚠ 2026-11-12 曾因此旁路修掉"块尺度三角亮斑"；若那族伪影回归，回退：
//   -DVPT_GLASS_CLEAR_REFRACTION=0 ⇒ 恢复清玻璃零位移档。
#define VPT_GLASS_CLEAR_REFRACTION 1
#endif
#ifndef VPT_GLASS_CLEAR_NO_CLAMP
// 2026-11-13（用户报"折射里出现淡蓝色三角形/块状图案"）：清玻璃（profile 0）
// 的折射位移 ≤0.03 格，小于玻璃板半厚 0.0625 ⇒ 位移本身出不了玻璃板；真正把
// 续射线起点逐像素推来推去的是**格夹取**——floor(hitPos − N·0.02) 在"面正好
// 落在格边界"的清玻璃上会把起点推回玻璃格内 ⇒ 起点逐像素跳变 ⇒ 块尺度三角
// 亮带（11-12 记载的同一病灶；当时用"清玻璃零位移"整条绕过，今天用户要求
// 恢复清玻璃折射后回归）。这里保留位移、只对 profile 0 去掉夹取。
// A/B：-DVPT_GLASS_CLEAR_NO_CLAMP=0（回到带夹取档）。
#define VPT_GLASS_CLEAR_NO_CLAMP 1
#endif
#ifndef VPT_GLASS_REAL_TRAVERSAL
// 2026-11-13（用户："换个折射模型，看看 caustica 为什么不会漏"）：
// 对齐参考实现 trace_transport.slang:39-55 traceAdvance —— **不做任何位移**，
// 续射起点 = 真实命中点 + ε 偏移；折射只改方向。质量差异的根源：
//   位移模型把起点推过玻璃板后 0.15 格（掠射时斜向可达 0.2 格）⇒ 起点可能
//   落进相邻实体 / 越过薄结构 ⇒ 漏天空、亮缝；
//   真实遍历里进/出两个界面都是真实几何命中 ⇒ 射线物理上不可能跳过中间
//   结构 ⇒ 漏光在**结构上**不存在（参考实现不会漏就是因为这一点）。
// 代价：一块玻璃板 = 2 个界面（进+出），界面预算与染色只付一次的规则见下。
// 1 = 真实遍历（默认）/ 0 = 旧薄壁位移档（A/B）。
#define VPT_GLASS_REAL_TRAVERSAL 1
#endif
#if VPT_GLASS_REFRACTION_MODEL != 0
        // 2026-11-12：普通清玻璃（profile 0）曾**不做任何位移**——板厚只有 1/16 格，
        // 当时观测到位移与板厚同量级 ⇒ 起点跨格 ⇒ 三角形亮斑。
        // 2026-11-13（用户要求 VPT_GLASS_CLEAR_REFRACTION=1）：清玻璃回到与致密
        // 介质同一套薄壁微位移（名义厚度 0.05 < 板厚 0.0625，正入射不跨格；
        // 掠射时位移被 min(...,0.03) 钳住）。回退：-DVPT_GLASS_CLEAR_REFRACTION=0。
#if !VPT_GLASS_REAL_TRAVERSAL
        if(slabStep == 0 &&
            (transparentProfile > 0 || VPT_GLASS_CLEAR_REFRACTION != 0))
        {
            vec3 insideDirection = refract(
                viewDirection,
                entryNormal,
                1.0 / max(entryIor, 1.0e-6));
            if(dot(insideDirection, insideDirection) < 1.0e-8)
            {
                insideDirection = viewDirection;
            }
            insideDirection = normalize(insideDirection);
            const float insideCosine = clamp(
                dot(insideDirection, -entryNormal), 0.05, 1.0);
#if VPT_GLASS_REFRACTION_MODEL == 2
            // 薄壁：厚度是**常量**，不从几何反推 ⇒ 位移有上界、与形状无关。
            const float slabThickness =
                max(VPT_GLASS_THIN_WALL_THICKNESS, 0.0);
#else
            const float slabThickness = clamp(
                glassSlabThicknessFromFace(glassSurfacePosition, entryNormal),
                0.01,
                2.0);
#endif
#ifndef VPT_GLASS_CLAMP_SLAB_EXIT
// 1 = 解析出射点夹在入射格内（**默认**）：薄板 + 大角度时出射点会落到相邻格，
//     续射线从实体内部起飞 ⇒ 那一整条三角形被当成"看到天空/背面" ⇒ 亮斑。
// 0 = 不夹（上一版行为，A/B 用）。
// ⚠ 2026-11-03 实测否决：夹取区间取自 floor(hitPos)，而玻璃面正好落在格边界上
// （顶面 y=5.0 属于 y∈[4,5] 的方块，floor 却给出 5）⇒ 起点被推到方块外，
// 逐行结果不同 ⇒ 整块玻璃出现横向黑带。保留代码供将来用**正确的格**重做，
// 默认关闭。
#define VPT_GLASS_CLAMP_SLAB_EXIT 0
#endif
#if VPT_GLASS_REFRACTION_MODEL == 2
            // 1) 位移长度上界：1/cos 在掠射时会把 0.05 放大成整整一格。
            const float insideTravel =
                min(slabThickness / insideCosine, 0.15);
            // 2) 起点夹在**入射格**内。格必须由"沿法线往内挪半格"求得 ——
            //    方块表面正好落在格边界上，直接 floor(pos) 会取到上方的格子。
#if VPT_GLASS_CLEAR_NO_CLAMP
            if(transparentProfile == 0)
            {
                // profile 0：不夹取（见 VPT_GLASS_CLEAR_NO_CLAMP 处的记载）。
                travelOrigin =
                    glassSurfacePosition + insideDirection * insideTravel;
            }
            else
#endif
            {
                const ivec3 entryCell =
                    ivec3(floor(glassSurfacePosition - entryNormal * 0.02));
                travelOrigin = clamp(
                    glassSurfacePosition + insideDirection * insideTravel,
                    vec3(entryCell) + vec3(0.002),
                    vec3(entryCell) + vec3(0.998));
            }
#else
            travelOrigin =
                glassSurfacePosition +
                insideDirection * (slabThickness / insideCosine);
#endif
#if VPT_GLASS_CLAMP_SLAB_EXIT
            {
                const vec3 entryCell = floor(glassSurfacePosition);
                travelOrigin = clamp(
                    travelOrigin,
                    entryCell + vec3(0.001),
                    entryCell + vec3(0.999));
            }
#endif
        }
#endif // !VPT_GLASS_REAL_TRAVERSAL
#endif // VPT_GLASS_REFRACTION_MODEL

        // 出射点之后再往前推一点，避免立刻自命中同一张面。
#if VPT_GLASS_REAL_TRAVERSAL
        // 真实遍历：起点就是真实命中点，只推 ε（tmin 兜底）。
        // 参考实现同样只做 ε 偏移，位移是漏光的唯一来源。
#if ORION_GLASS_REMIX_PSTR_GUIDES
        if(slabStep == 0)
        {
            travelOrigin += travelDirection * 0.002;
            accumulatedGlassOpticalDist += 0.002;
        }
#else
        travelOrigin += travelDirection * 0.002;
#endif
#else
        // 直穿模型下 travelOrigin 仍是入射点，这一推就是全部位移（约 3 cm）。
        travelOrigin += travelDirection * 0.03;
#endif

        hit.hitDistance = 1.0;
        hit.albedoPacked = packAlbedoUnorm(vec3(1.0));
        hit.state = makeTraceState(RT_PURPOSE_FULL) | RT_STATE_GI_NO_POM_BIT;
        if(glassCamDist >= 32.0)
        {
            hit.state |= RT_STATE_GI_SKIP_CLOUDS_BIT;
        }

#if ORION_GLASS_REMIX_PSTR_GUIDES && !defined(ORION_K2_TRANSPORT) && ORION_GLASS_TWO_INTERFACE_REFRACT
        const uint stepCullMask = nextStepSkipGlass ? 0x04u : 0xcfu;
        const float stepTmin = nextStepSkipGlass ? 0.0005 : (insideGlass ? 0.002 : 0.004);
        nextStepSkipGlass = false;
#else
        const uint stepCullMask = 0xcfu;
        const float stepTmin = 0.01;
#endif

        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        traceRayEXT(
            topLevelAS,
            gl_RayFlagsNoneEXT,
            stepCullMask,
            0,
            0,
            0,
            travelOrigin,
            stepTmin,
            travelDirection,
            10000.0,
            0
        );

        if(hit.hitDistance < 0.0)
        {
            // 天空 / 无命中：miss shader 已经把天空辐射写进 albedoPacked。
            transmittedRadiance += throughput * unpackAlbedoUnorm(hit.albedoPacked);
#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_TWO_INTERFACE_REFRACT
            rrGlassPaneTransmissionFilter =
                clamp(throughput, vec3(0.0), vec3(1.0));
#if GI_GLASS_DOMINANT_GUIDE
            glassLobeTransmissionWeight = rrGlassPaneTransmissionFilter;
#endif
#endif
            reachedEndpoint = true;
            break;
        }

        accumulatedGlassOpticalDist += hit.hitDistance;
        const vec3 hitPosition = travelOrigin + travelDirection * hit.hitDistance;
        const uint hitState = hit.state;
        const uint hitClass = getHitClass(hitState);

        if(hitClass == RT_HIT_TRANSLUCENT)
        {
            // 共面重复面（相邻玻璃块的内部面没被剔除时会出现）：距离几乎为 0，
            // 再收一次 Fresnel/染色就会"越叠越黑"，这里直接穿过去。
#if ORION_GLASS_REMIX_PSTR_GUIDES
            if(hit.hitDistance < 0.004)
#else
            if(hit.hitDistance < 0.02)
#endif
            {
#if VPT_GLASS_REAL_TRAVERSAL
                // 共面跳过也只推 ε：0.03 的推进足以跳过一块 0.125 厚的玻璃板。
                travelOrigin = hitPosition + travelDirection * 0.002;
                accumulatedGlassOpticalDist += 0.002;
#else
                travelOrigin = hitPosition + travelDirection * 0.03;
#endif
                continue;
            }

            vec3 faceNormal = normalize(hit.normal);
            if(dot(faceNormal, faceNormal) < 0.5)
            {
                faceNormal = -travelDirection;
            }
#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_TWO_INTERFACE_REFRACT && VPT_GLASS_REAL_TRAVERSAL
            if(dot(faceNormal, travelDirection) > 0.0)
            {
                faceNormal = -faceNormal;
            }
            const float interfaceCosine =
                clamp(dot(-travelDirection, faceNormal), 1.0e-4, 1.0);

            if(insideGlass)
            {
                // Exit face (glass -> air): restore air-side ray direction via Snell
                // and apply exit Fresnel without multiplying tint a second time.
                vec3 exitRefracted =
                    refract(travelDirection, faceNormal, max(currentIor, 1.0));
                if(dot(exitRefracted, exitRefracted) > 1.0e-8)
                {
                    const float exitFresnel = clamp(
                        glassDielectricFresnel(interfaceCosine, currentIor, 1.0),
                        0.0,
                        VPT_GLASS_MAX_FRESNEL);
                    throughput *= (1.0 - exitFresnel);
                    travelDirection = normalize(mix(
                        viewDirection,
                        normalize(exitRefracted),
                        clamp(VPT_GLASS_BENT_STRENGTH, 0.0, 1.0)));
                }
                else
                {
                    // Guard against TIR blackouts on perpendicular block corners.
                    const float exitFresnel = clamp(
                        glassDielectricFresnel(interfaceCosine, 1.0, currentIor),
                        0.0,
                        VPT_GLASS_MAX_FRESNEL);
                    throughput *= (1.0 - exitFresnel);
                    travelDirection = viewDirection;
                }
                insideGlass = false;
                currentIor = 1.0;
#if ORION_GLASS_REMIX_PSTR_GUIDES
                {
                    uint extVoxel = 0u;
                    if(vptReadSourceVoxel(ivec3(floor(hitPosition - faceNormal * 0.05)), extVoxel) &&
                       ((extVoxel & 3u) == 1u))
                    {
                        // Exit face is coplanar with an opaque ground/wall block: step slightly
                        // inward and trace opaque-only (mask 0x04) to hit the solid face instead
                        // of tunneling through it into blue sky.
                        travelOrigin = hitPosition + faceNormal * 0.004;
                        nextStepSkipGlass = true;
                    }
                    else
                    {
                        travelOrigin = hitPosition - faceNormal * 0.002;
                        accumulatedGlassOpticalDist += 0.002;
                    }
                }
                continue;
#endif
            }
            else
            {
                // Entry face of a subsequent glass block (air -> glass).
                const float nextIor =
                    max(glassIorForProfile(int(getTransparentProfile(hitState))), 1.0);
                const float nextEntryFresnel = clamp(
                    glassDielectricFresnel(interfaceCosine, 1.0, nextIor),
                    0.0,
                    VPT_GLASS_MAX_FRESNEL);
                const vec3 faceTint =
                    clamp(unpackAlbedoUnorm(hit.albedoPacked), vec3(0.0), vec3(1.0));
                throughput *= (1.0 - nextEntryFresnel) * faceTint;
#if VPT_GLASS_TRANSMIT_BENT
                {
                    const vec3 nextBent = refract(
                        travelDirection,
                        faceNormal,
                        1.0 / max(nextIor, 1.0e-6));
                    if(dot(nextBent, nextBent) > 1.0e-8)
                    {
                        travelDirection = normalize(mix(
                            travelDirection,
                            normalize(nextBent),
                            clamp(VPT_GLASS_BENT_STRENGTH, 0.0, 1.0)));
                    }
                }
#endif
                insideGlass = true;
                currentIor = nextIor;
            }
            travelOrigin = hitPosition - faceNormal * 0.002;
            accumulatedGlassOpticalDist += 0.002;
            continue;
#else
            // 进入面 / 出射面：镜像后 faceNormal 恒与行进方向相反，翻面之前先记
            // 原始朝向。进入面（外法线与行进方向相反）= dot < 0。
            // 真实遍历下一块玻璃板会被命中两次（进 + 出），染色只在**进入**付一次
            // （参考实现 thinTransmissionColor 也是单次 sheet 染色）。
            const bool enteringGlassFace = dot(faceNormal, travelDirection) < 0.0;
            if(dot(faceNormal, travelDirection) > 0.0)
            {
                faceNormal = -faceNormal;
            }

            const float nextIor =
                max(glassIorForProfile(int(getTransparentProfile(hitState))), 1.0);
            const float interfaceCosine =
                clamp(dot(-travelDirection, faceNormal), 1.0e-4, 1.0);
            const float interfaceFresnel =
                glassDielectricFresnel(interfaceCosine, 1.0, nextIor);
            const vec3 faceTint =
                clamp(unpackAlbedoUnorm(hit.albedoPacked), vec3(0.0), vec3(1.0));

            // 下一块板：同样只付一次 (1-F)·tint，然后按同一套平行板位移穿过它。
#if VPT_GLASS_TRANSMIT_BENT
            // 弯折档：视线已在入射界面弯过一次，后续界面**不再**二次弯折——
            // 每面各弯一次会在穿过几层玻璃后把视线折进面内。只保留
            // Fresnel/染色与薄壁位移。
            vec3 insideDirection = travelDirection;
#else
            vec3 insideDirection = refract(
                travelDirection,
                faceNormal,
                1.0 / max(nextIor, 1.0e-6));
            if(dot(insideDirection, insideDirection) < 1.0e-8)
            {
                insideDirection = travelDirection;
            }
            insideDirection = normalize(insideDirection);
#endif
            const float insideCosine =
                clamp(dot(insideDirection, -faceNormal), 0.05, 1.0);
            const float slabThickness = clamp(
                glassSlabThicknessFromFace(hitPosition, faceNormal),
                0.01,
                2.0);

            // 染色只在进入面付一次；出射面只付 Fresnel。
            throughput *= (1.0 - interfaceFresnel) *
                (enteringGlassFace ? faceTint : vec3(1.0));
#if VPT_GLASS_REAL_TRAVERSAL
            // 真实遍历：不位移。起点 = 真实命中点，沿"离开该面"的方向推 ε
            // （faceNormal 已翻成逆着行进方向 ⇒ 出射侧是 -faceNormal）。
            travelOrigin = hitPosition - faceNormal * 0.002;
            continue;
#endif
            // 2026-11-12：清玻璃（profile 0）零厚度 —— 后续界面与入射界面同口径，
            // 不做折射位移、不做格夹取。出射面正落在格边界上，floor(hitPos - N*0.02)
            // 的夹取区间会把续射线起点推回玻璃格内（掠射可达整格）⇒ 起点逐像素跳变
            // ⇒ 块尺度亮带、云团上的直线切口、共面跳过循环耗尽在板数预算上。
            // 2026-11-13（用户要求 VPT_GLASS_CLEAR_REFRACTION=1）：清玻璃后续界面
            // 也走与致密介质相同的限位移路径（laterTravel 同样被 min(...,0.03) 钳住）。
            // 伪影回归时的回退：-DVPT_GLASS_CLEAR_REFRACTION=0。
#if !VPT_GLASS_CLEAR_REFRACTION
            if(int(getTransparentProfile(hitState)) == 0)
            {
                travelOrigin = hitPosition + travelDirection * 0.03;
                continue;
            }
#endif
            // 2026-11-09：第二个及以后的界面也必须限位移！
            // 这里原本用几何反推的板厚 + cos 下限 0.05，
            // 掠射时位移 = 1.0 / 0.05 = **20 格** ⇒ 起点直接跳到远处，
            // 中间的墙全被跳过 ⇒ “透视到远处矿洞”。
            const float laterTravel = min(slabThickness / insideCosine, 0.15);
#if VPT_GLASS_CLEAR_NO_CLAMP
            if(int(getTransparentProfile(hitState)) == 0)
            {
                // profile 0：不夹取（见 VPT_GLASS_CLEAR_NO_CLAMP 处的记载）。
                travelOrigin = hitPosition + insideDirection * laterTravel;
            }
            else
#endif
            {
                const ivec3 laterCell =
                    ivec3(floor(hitPosition - faceNormal * 0.02));
                travelOrigin = clamp(
                    hitPosition + insideDirection * laterTravel,
                    vec3(laterCell) + vec3(0.002),
                    vec3(laterCell) + vec3(0.998));
            }
#if VPT_GLASS_CLAMP_SLAB_EXIT
            {
                const vec3 hitCell = floor(hitPosition);
                travelOrigin = clamp(
                    travelOrigin,
                    hitCell + vec3(0.001),
                    hitCell + vec3(0.999));
            }
#endif
            continue;
#endif
        }

        if(hitClass == RT_HIT_WATER)
        {
            // 玻璃后面还有水：交还给水体走查（含 Beer-Lambert / 散射）。
            const bool exitingWater =
                getWaterEvent(hitState) == RT_WATER_WATER_TO_AIR;
            vec3 waterDirection = refract(
                travelDirection,
                normalize(hit.normal),
                exitingWater ? 1.33 : (1.0 / 1.33));
            const bool totalInternalReflection =
                dot(waterDirection, waterDirection) < 1.0e-6;
            if(totalInternalReflection)
            {
                waterDirection = reflect(travelDirection, normalize(hit.normal));
            }
#ifdef ORION_K1_GLASS_ONLY
            vec3 gwOrigin = hitPosition + waterDirection * 0.002;
            vec3 gwDir = normalize(waterDirection);
            bool gwInside = totalInternalReflection ? exitingWater : !exitingWater;
            float gwPathLen = 0.0;
            vec3 gwThroughput = vec3(1.0);
            vec3 gwInscatter = vec3(0.0);
            vec3 waterRadiance = vec3(0.0);
            bool gwDone = false;
            const int glassWaterMaxSteps =
                (cam.frameCount != 0xFFFFFFFFu) ? 2 : 0;

            for(int gwStep = 0; gwStep < glassWaterMaxSteps; ++gwStep)
            {
                hit.hitDistance = 1.0;
                hit.albedoPacked = packAlbedoUnorm(vec3(1.0));
                hit.state = makeTraceState(gwInside ? RT_PURPOSE_WATER_MEDIUM : RT_PURPOSE_FULL) |
                    RT_STATE_GI_SKIP_CLOUDS_BIT |
                    RT_STATE_GI_NO_POM_BIT;
#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
                serTraceRay(
                    gwOrigin, 0.01, gwDir, 10000.0,
                    secondaryTraverseFlags(), 0xcfu, uint(gwStep));
#else
#ifdef RT_ENABLE_SER
                if(cam.perfSerAb.z != 0u)
                {
                    serTraceRay(
                        gwOrigin, 0.01, gwDir, 10000.0,
                        secondaryTraverseFlags(), 0xcfu, uint(gwStep));
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
                        0, 0, 0,
                        gwOrigin, 0.01, gwDir, 10000.0, 0);
                }
#endif

                if(hit.hitDistance < 0.0)
                {
                    waterRadiance = gwInscatter + gwThroughput * unpackAlbedoUnorm(hit.albedoPacked);
                    transmissionInfo.waterPathLength = gwInside ? max(gwPathLen, 60.0) : max(gwPathLen, 0.0);
                    transmissionInfo.reachedUnderwaterReceiver = 0.0;
                    gwDone = true;
                    break;
                }

                const float segDist = hit.hitDistance;
                const vec3 segPos = gwOrigin + gwDir * segDist;
                if(gwInside)
                {
                    gwPathLen += segDist;
                    const vec3 wAbs = exp(-waterAbsorptionColor() * max(segDist - 1.5, 0.0));
                    gwInscatter += gwThroughput * (vec3(1.0) - wAbs) * waterScatterColor() *
                        (getSurfaceAmbientRadiance() + getTimeDrivenSkyLightColor() * 0.35);
                    gwThroughput *= wAbs;
                }

                const float segMat = getLegacyMaterialId(hit.state);
                if(segMat <= 0.5)
                {
                    const vec3 uwNormal = normalize(hit.normal);
                    const vec3 uwAlbedo = unpackAlbedoUnorm(hit.albedoPacked);
                    const float uwCamDist = length(segPos - cam.viewInverse[3].xyz);
                    const vec3 uwSurface =
                        evaluateOpaqueSurfaceRadianceInMedium(
                            segPos, uwNormal, -gwDir, uwAlbedo,
                            max(hit.roughness, 0.05), hit.f0Metal,
                            hit.emission + evaluateVLIEmitterRadianceAtHit(segPos, uwNormal, uwAlbedo),
                            hit.pomSelfShadow, sunDirection, gwInside) +
                        evaluateVPTExactFaceDirectLighting(segPos, uwNormal, uwAlbedo) +
                        evaluateDynamicLightDeterministicDirect(segPos, uwNormal, uwAlbedo, uwCamDist) +
                        uwAlbedo * evaluateDeterministicIndirectAmbient(uwNormal, 1.0);
                    waterRadiance = gwInscatter + gwThroughput * uwSurface;
                    transmissionInfo.waterPathLength = max(gwPathLen, 0.0);
                    transmissionInfo.receiverWorldPos = segPos;
                    transmissionInfo.receiverNormal = uwNormal;
                    transmissionInfo.receiverViewDirection = -gwDir;
                    transmissionInfo.receiverAlbedo = uwAlbedo;
                    transmissionInfo.receiverRoughness = hit.roughness;
                    transmissionInfo.receiverF0Metal = hit.f0Metal;
                    transmissionInfo.reachedUnderwaterReceiver = 1.0;
                    transmissionInfo.waterSunVisibility = 1.0;
                    transmissionInfo.receiverSelfEmissionRadiance = vec3(0.0);
                    gwDone = true;
                    break;
                }
                else if(segMat < 2.5)
                {
                    if(gwInside)
                    {
                        vec3 wExitN = normalize(hit.normal);
                        if(dot(wExitN, gwDir) > 0.0)
                        {
                            wExitN = -wExitN;
                        }
                        vec3 wExitDir = refract(gwDir, wExitN, 1.33);
                        if(dot(wExitDir, wExitDir) < 1.0e-6)
                        {
                            wExitDir = gwDir;
                        }
                        gwDir = normalize(wExitDir);
                        gwInside = false;
                        gwOrigin = segPos - wExitN * 0.004;
                    }
                    else
                    {
                        gwInside = true;
                        gwOrigin = segPos + gwDir * 0.004;
                    }
                    continue;
                }
                else
                {
                    gwThroughput *= clamp(unpackAlbedoUnorm(hit.albedoPacked), vec3(0.0), vec3(1.0));
                    gwOrigin = segPos + gwDir * 0.004;
                    continue;
                }
            }
            if(!gwDone)
            {
                waterRadiance = gwInscatter + gwThroughput * evaluateDeterministicIndirectAmbient(-gwDir, 1.0);
            }
            rrGlassPaneTransmissionFilter = clamp(throughput, vec3(0.0), vec3(1.0));
#if GI_GLASS_DOMINANT_GUIDE
            glassLobeTransmissionWeight = rrGlassPaneTransmissionFilter;
#endif
            transmittedRadiance += throughput * waterRadiance;
            reachedEndpoint = true;
            break;
#else
            WaterTransmissionInfo waterInfo;
            const vec3 waterRadiance = tracePrimaryTransparentRadiance(
                hitPosition + waterDirection * 0.001,
                waterDirection,
                totalInternalReflection ? exitingWater : !exitingWater,
                sunDirection,
                false,
                0.0,
                historyKeyPosition,
                false,
                // 2026-11-16：透射瓣（行为与旧档逐位一致）。
                false,
                waterInfo);
            transmittedRadiance += throughput * waterRadiance;
            transmissionInfo = waterInfo;
            reachedEndpoint = true;
            break;
#endif
        }

        // 不透明接收面：离开 slab 循环后再着色，压低循环体跨 traceRayEXT 寄存器生命周期。
        opqHitPos = hitPosition;
        opqHitNormal = normalize(hit.normal);
        opqHitAlbedo = unpackAlbedoUnorm(hit.albedoPacked);
        opqHitRoughness = hit.roughness;
        opqHitF0Metal = hit.f0Metal;
        opqHitEmission = hit.emission;
        opqHitPomSelfShadow = hit.pomSelfShadow;
        hitOpaqueReceiver = true;
        reachedEndpoint = true;
        break;
    }

    if(hitOpaqueReceiver)
    {
        transmissionInfo.waterPathLength = max(accumulatedGlassOpticalDist, 0.0);
        transmissionInfo.receiverWorldPos = opqHitPos;
        transmissionInfo.receiverNormal = opqHitNormal;
        transmissionInfo.receiverViewDirection = -travelDirection;
        transmissionInfo.receiverAlbedo = opqHitAlbedo;
        transmissionInfo.receiverRoughness = opqHitRoughness;
        transmissionInfo.receiverF0Metal = opqHitF0Metal;
        transmissionInfo.reachedUnderwaterReceiver = 1.0;
        transmissionInfo.waterSunVisibility = 1.0;
        transmissionInfo.receiverSelfEmissionRadiance = vec3(0.0);

        const float receiverCameraDist = length(
            opqHitPos - cam.viewInverse[3].xyz);

        // 2026-11-27: 100% 确定性零噪点着色（空气侧单发太阳可见性 + 确定性 VPT 局部光 +
        // 确定性动态光源 + 确定性半球环境光；彻底消除 1-spp RIS 与随机 GI 噪点）。
        vec3 receiverRadiance =
            evaluateOpaqueSurfaceRadianceInMedium(
                opqHitPos,
                opqHitNormal,
                -travelDirection,
                opqHitAlbedo,
                max(opqHitRoughness, 0.05),
                opqHitF0Metal,
                opqHitEmission +
                    evaluateVLIEmitterRadianceAtHit(
                        opqHitPos, opqHitNormal, opqHitAlbedo),
                opqHitPomSelfShadow,
                sunDirection,
                false) +
            evaluateVPTExactFaceDirectLighting(
                opqHitPos,
                opqHitNormal,
                opqHitAlbedo) +
            evaluateDynamicLightDeterministicDirect(
                opqHitPos,
                opqHitNormal,
                opqHitAlbedo,
                receiverCameraDist) +
            opqHitAlbedo * evaluateDeterministicIndirectAmbient(opqHitNormal, 1.0);

        rrGlassPaneTransmissionFilter =
            clamp(throughput, vec3(0.0), vec3(1.0));
#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_TWO_INTERFACE_REFRACT && GI_GLASS_DOMINANT_GUIDE
        glassLobeTransmissionWeight = rrGlassPaneTransmissionFilter;
#endif
        transmittedRadiance += throughput * receiverRadiance;
    }

    if(!reachedEndpoint)
    {
        // 板数预算用尽：用环境底光收尾，避免整块玻璃变黑。
#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_TWO_INTERFACE_REFRACT
        rrGlassPaneTransmissionFilter =
            clamp(throughput, vec3(0.0), vec3(1.0));
#if GI_GLASS_DOMINANT_GUIDE
        glassLobeTransmissionWeight = rrGlassPaneTransmissionFilter;
#endif
#endif
        transmittedRadiance += throughput * getSurfaceAmbientRadiance();
    }

    const vec3 surfaceReflection =
        reflectionRadiance * slabReflection + sunSpecularGlint;

#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_RADIANCE_DOMINANCE_FIX
    {
        const float reflRadLuma = dot(
            max(surfaceReflection, vec3(0.0)),
            vec3(0.2126, 0.7152, 0.0722));
        const float transRadLuma = dot(
            max(transmittedRadiance, vec3(0.0)),
            vec3(0.2126, 0.7152, 0.0722));
        g_glassReflectionDominatesPixel =
            (reflRadLuma >= max(transRadLuma, 1.0e-4));
    }
#endif

#if RR_STABLE_GLASS_RADIANCE
    // 反射瓣是零方差的确定性量，交给 RR 之后的合成路径单独加回。
    stableGlassRadiance = surfaceReflection;
#endif

    return surfaceReflection + transmittedRadiance;
}

