
// Temporary decode used only while rgen's existing transport calls are moved
// one by one in subsequent blocks.  No hit shader writes a legacy ID.
float getLegacyMaterialId(uint state)
{
    uint hitClass = getHitClass(state);
    if(hitClass == RT_HIT_WATER)
    {
        return getWaterEvent(state) == RT_WATER_WATER_TO_AIR ? 2.0 : 1.0;
    }
    if(hitClass == RT_HIT_TRANSLUCENT)
    {
        return isGlassBottomHit(state) ? 4.0 : 3.0;
    }
    return 0.0;
}

// Ray purpose is carried by hit.state.  parallaxRayTravel is written only
// by closest-hit as an actual POM travel distance.


// rchit now supplies one analytical normal for all top-water triangles at a
// world position.  Keep that optical slope; flattening it here would erase
// the wave response from reflection and transmission.  Vertical waterfalls
// still arrive with their original geometric normal.
vec3 stabilizeWaterInterfaceNormal(
    vec3 normal
)
{
    return normalize(normal);
}

// 2026-11-27 (Batch AE): Shared frame-load and adaptive distance scale helpers
// placed early so glass reflection/primary and water receiver can all throttle
// smoothly under close-up load spikes.
bool g_waterWalkerSkipClouds = false;

float perfL10WaterLoad()
{
    const float frameMs = cam.rtSampleParams.w;
    if(frameMs <= 0.0)
    {
        return 0.0;
    }
    return clamp((frameMs - 16.0) / 14.0, 0.0, 1.0);
}

float perfL10WaterAdaptiveScale()
{
    // 2026-11-27: 重构后水体/半透明链已确定性轻量，不再随帧时间抖动收紧距离闸。
    return 1.0;
}

// 2026-11-27: 零射线、零噪点确定性间接环境光近似（用于替代水底/玻璃后 1-spp 随机 GI）。
vec3 evaluateDeterministicIndirectAmbient(vec3 hitNormal, float skyOpenFactor)
{
    float hemi = 0.65 + 0.35 * clamp(normalize(hitNormal).y * 0.5 + 0.5, 0.0, 1.0);
    return getSurfaceAmbientRadiance() * mix(0.45, 1.0, clamp(skyOpenFactor, 0.0, 1.0)) * hemi;
}

// ===========================================================================
// 2026-11-20：**水面 Fresnel 的稳定余弦**（针对"水底闪烁黑三角"）
//
// 症状（用户）：早晨太阳低、从**水面之上**看水底时，水底出现成片的
// **黑色三角形**，会随波浪闪；一潜进水里就完全看不到。
//
// 成因（本函数要修的就是它）：
//   · 水面法线是逐三角形的波浪法线，相邻三角形之间有几度到十几度的差异；
//   · Fresnel 用的是 cosTheta = |dir·N|，而水底可见度 ∝ (1 - F)；
//   · 视角一掠射（|dir·N| → 0），F → 1 ⇒ 透射 → 0 ⇒ **该三角形下面的水底
//     直接变黑**。三角形之间法线不同 ⇒ 有的黑、有的正常 ⇒ 成片的黑三角。
//   · 潜水后相机在水里，视线不再穿过水面 ⇒ 与界面 Fresnel 无关 ⇒ 现象消失。
//
// 做法（与"水面低频频谱"这一物理事实一致，不是为了好看硬压）：
//   Fresnel 是一阶界面项，属于**低频**物理量；波浪法线只应该负责"把光线
//   往哪儿折"，不应该负责"透射多少能量"。所以：
//     · 近水平水面（|N.y| 大）：Fresnel 余弦取**平面几何法线**的余弦，
//       即 |dir.y| —— 对平水面与波浪法线逐位相同（N=(0,1,0) 时两者恒等），
//       只是不再随三角形跳动；
//     · 竖直面（瀑布/水柱/水体侧壁，|N.y| → 0）：仍用逐面余弦，否则侧壁
//       会被判成恒掠射（恒 F≈1）、太阳高光整片消失；
//     · 0.30~0.60 之间平滑过渡，不产生新的硬边。
//   折射/反射的**方向**仍用波浪法线（`interfaceNormal`），一点不改。
//
// A/B：-DWATER_STABLE_FRESNEL_COSINE=0 回到"逐面余弦"（逐位旧档）。
// ===========================================================================
#ifndef WATER_STABLE_FRESNEL_COSINE
#define WATER_STABLE_FRESNEL_COSINE 0
#endif

float waterFresnelCosine(vec3 rayDirection, vec3 interfaceNormal)
{
    const float cosFace =
        clamp(-dot(rayDirection, interfaceNormal), 0.0, 1.0);
#if WATER_STABLE_FRESNEL_COSINE
    // planarWeight = 1 ⇒ 这个面是水平水面，用平面余弦
    const float planarWeight =
        smoothstep(0.30, 0.60, abs(interfaceNormal.y));
    const float cosPlanar =
        clamp(abs(rayDirection.y), 0.0, 1.0);
    return mix(cosFace, cosPlanar, planarWeight);
#else
    return cosFace;
#endif
}

// ===========================================================================
// 2026-11-20（第二版）：**掠射视角下把水面界面法线收敛到平面**
//
// 用户实测：不管怎么改 Fresnel，**只要视线到掠射角就出现黑色闪烁三角形**，
// 潜进水里就消失。⇒ 问题不在"透射多少能量"，而在**掠射时逐三角形的界面处理
// 本身**（折射方向 / 反射方向 / 出射再命中），任何一个按三角形跳量都会在
// 掠射时被放大成可见的碎块。
//
// 做法：掠射权重 `grazing = 1 - smoothstep(0.10, 0.45, |dir.y|)`，把这个权重
// 与"该面确实是水平水面"的权重相乘后，把界面法线从波浪法线插值到平面法线。
//   · 掠射视线（|dir.y| ≤ 0.10）：法线 = (0,±1,0) ⇒ 折射/反射/判定全部与
//     三角形无关，碎块不可能存在；
//   · 陡视角（|dir.y| ≥ 0.45）：逐位保留波浪法线，波纹折射/反射一点不变；
//   · 中间平滑过渡，不产生新的硬边。
//   · 竖直面（瀑布/水柱侧壁，|N.y| → 0）planarWeight → 0，仍走几何法线。
//
// 物理依据：掠射时水面在屏幕上的投影被压得极扁，单个像素覆盖许多三角形，
// 任何逐三角形量都已经是亚像素噪声；此时用平面（低频）近似才是正确的
// 采样频率，而不是"偷懒压系数"。
//
// A/B：-DWATER_GRAZING_PLANAR_NORMAL=0 回到逐面波浪法线（上一版行为）。
// ===========================================================================
#ifndef WATER_GRAZING_PLANAR_NORMAL
#define WATER_GRAZING_PLANAR_NORMAL 0
#endif

vec3 waterInterfaceNormalForRay(vec3 normal, vec3 rayDirection)
{
    const vec3 n = normalize(normal);
#if WATER_GRAZING_PLANAR_NORMAL
    // 视线越掠射，权重越靠近"平面"
    const float grazingWeight =
        1.0 - smoothstep(0.10, 0.45, abs(rayDirection.y));
    // 这个面本身是不是水平水面
    const float horizontalWeight =
        smoothstep(0.30, 0.60, abs(n.y));
    const float planarWeight = grazingWeight * horizontalWeight;
    if(planarWeight <= 1.0e-4)
    {
        return n;
    }
    const vec3 planar = vec3(0.0, n.y >= 0.0 ? 1.0 : -1.0, 0.0);
    return normalize(mix(n, planar, planarWeight));
#else
    return n;
#endif
}


// Voxel DDA transmission for the final RIS-selected local-light sample.
//
// The march itself lives in glass_tint_common.glsl
// (glassTraceTransmissionToEmitter): it multiplies the stained-pane dye of every
// voxel it crosses and performs no occlusion test.  It replaced a hardware
// GLASS_VISIBILITY ray that could never return a tint -- that ray seeded
// hit.hitDistance = 1.0 and tested `hit.hitDistance >= 0.0` as its
// "was I blocked?" sentinel, expecting an Any-Hit shader to write a negative
// value.  No loaded rahit ever writes that field (rt_test.rahit and
// rt_test_translucent.rahit only declare it), so the test was unconditionally
// true and the caller saw vec3(0.0) on every pixel.
//
// Kept here: the judgement-experiment probe below.
//
// 判决实验 I：DDA 覆盖率探针（DBG_VPT_DDA_PROBE=true 时生效）
//
// 目的：从「玻璃后的接收面」射向光源的 DDA，到底有没有真的穿过玻璃体素。
// 阶段 1（可达性验证）：输出纯品红 (1, 0, 1)。
// 自然画面里不可能出现这个颜色，所以只要屏幕上看到品红，
// 就证明「玻璃后的接收面 → evaluateVPTRISDirectLighting →
// evaluateVPTRISDirectLightingSingle → vptTraceGlassTransmissionToEmitter」
// 这条链确实在执行。看不到品红则说明这条链是死的。
vec3 vptTraceGlassTransmissionToEmitter(
    vec3 origin,
    vec3 target,
    float targetVoxelEntryDistance
)
{
    if(DBG_VPT_DDA_PROBE)
    {
        return vec3(1.0, 0.0, 1.0);
    }

    return glassTraceTransmissionToEmitter(
        origin, target, targetVoxelEntryDistance
    );
}

// R16: glass receivers cannot use Binding 30's cached face-centre irradiance:
// it is intentionally one value per Minecraft block face, which creates the
// visible un-smoothed block shadow pattern.  Select one of Binding 29's eight
// already-ranked nearby emitters at the true hardware hit instead.  The score,
// light point and visibility ray are all deterministic: no RIS proposal, no
// frame seed and exactly one hardware visibility trace for this glass pixel.
//
// The hardware query is the existing GLASS_VISIBILITY path, so Any-Hit keeps
// accumulating RGB glass transmission for any intervening stained panes.
// The primary camera pane is filtered separately by blockTransmission below.
//
// Disabled (no call sites): kept as a future interface option.
#if 0
vec3 evaluateGlassReceiverDeterministicLocalLighting(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 hitAlbedo
)
{
    ivec3 receiverSection = ivec3(floor(hitPos / 16.0));
    uint receiverPage;
    if(!vliLookupSectionPage(receiverSection, receiverPage))
    {
        return vec3(0.0);
    }

    vec3 N = normalize(hitNormal);
    float bestScore = 0.0;
    vec3 bestEmitterCenter = vec3(0.0);
    ivec3 bestEmitterBlock = ivec3(0);
    float bestDistance = 0.0;
    float bestEmission = 0.0;
    uint bestTypeCode = 0u;

    uint candidateBase =
        receiverPage * VPT_RELATED_EMITTERS_PER_SECTION;

    for(uint candidateIndex = 0u;
        candidateIndex < VPT_RELATED_EMITTERS_PER_SECTION;
        ++candidateIndex)
    {
        uvec4 candidate = vptCandidatePagesBuffer.entries[
            candidateBase + candidateIndex
        ];
        uint emission = candidate.w & 15u;
        if(emission == 0u)
        {
            continue;
        }

        ivec3 emitterBlock = ivec3(candidate.xyz);
        vec3 emitterCenter = vec3(emitterBlock) + vec3(0.5);
        vec3 toLight = emitterCenter - hitPos;
        float distanceSquared = dot(toLight, toLight);
        if(distanceSquared <= 1.0e-6)
        {
            continue;
        }

        float distanceToLight = sqrt(distanceSquared);
        float emissionLevel = float(emission);
        // 2026-10-31：档案 id 扩到 5 bit（bits 4..8），距离/亮度按档案走。
        uint candidateTypeCode = (candidate.w >> 4u) & 31u;
        if(distanceToLight > vptLocalLightRange(candidateTypeCode, emissionLevel))
        {
            continue;
        }

        float NdotL = max(dot(N, toLight / distanceToLight), 0.0);
        if(NdotL <= 0.0)
        {
            continue;
        }

        float score =
            localLightProfileBrightness(candidateTypeCode, emissionLevel) * 15.0 *
            vptLocalLightDistanceAttenuation(distanceToLight) *
            vptLocalLightRangeFade(distanceToLight, candidateTypeCode, emissionLevel) *
            NdotL;

        // Stable tie break: Binding 29 itself has a stable CPU ordering.
        if(score > bestScore)
        {
            bestScore = score;
            bestEmitterCenter = emitterCenter;
            bestEmitterBlock = emitterBlock;
            bestDistance = distanceToLight;
            bestEmission = emissionLevel;
            bestTypeCode = candidateTypeCode;
        }
    }

    if(bestScore <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 receiverOrigin = hitPos + N * 0.035;
    float targetVoxelEntryDistance;
    if(!vptLoadedPathToCandidateEmitter(
        receiverOrigin,
        bestEmitterCenter,
        bestEmitterBlock,
        targetVoxelEntryDistance
    ))
    {
        return vec3(0.0);
    }

    vec3 glassVisibility = vptTraceGlassTransmissionToEmitter(
        receiverOrigin,
        bestEmitterCenter,
        targetVoxelEntryDistance
    );
    if(max(glassVisibility.r, max(glassVisibility.g, glassVisibility.b)) <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 lightDirection = normalize(bestEmitterCenter - hitPos);
    float NdotL = max(dot(N, lightDirection), 0.0);
    float radianceScale =
        3.2 *
        LOCAL_LIGHT_ENERGY_SCALE * localLightIntensity() *
        localLightProfileBrightness(bestTypeCode, bestEmission) *
        vptLocalLightDistanceAttenuation(bestDistance) *
        vptLocalLightRangeFade(bestDistance, bestTypeCode, bestEmission);

    return
        vliDecodeEmitterColor(bestTypeCode) *
        radianceScale *
        NdotL *
        glassVisibility *
        max(hitAlbedo, vec3(0.0)) *
        0.92;
}
#endif

uint seed;