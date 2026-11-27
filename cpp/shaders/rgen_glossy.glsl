#ifndef ORION_K2I_COMPOSE_COMPUTE
#ifndef ORION_SECTION_VERTEX_ADDR_BUFFER_DEFINED
#define ORION_SECTION_VERTEX_ADDR_BUFFER_DEFINED
layout(std430, binding = 21, set = 0) readonly buffer SectionVertexAddressBuffer {
    uvec4 vertexAddresses[];
} sectionVertexAddressBuffer;
#endif

layout(buffer_reference, scalar) readonly buffer GlossyVertexPos {
    float x, y, z;
};

vec3 glossyLoadVertexPos(uint64_t baseAddr, uint stride, uint vertexIndex)
{
    GlossyVertexPos p = GlossyVertexPos(baseAddr + uint64_t(vertexIndex) * uint64_t(stride));
    return vec3(p.x, p.y, p.z);
}
#endif

// ============================================================================
// 2026-11-10：glossy 反射链的俄罗斯轮盘 + 层数 LOD
//
// 实测依据（[PERF L10 GPU] + ORION_PERF_ABLATION=alt:0,3,2,1,10）：
//   base trace=16.12ms / noGlossy 9.64ms（-6.48，即反射链占 40%）
//   noGi -1.11ms / noRtao +0.09ms / shadow1of2 -0.01ms
// 而这条路径此前是**确定性跑满**：全文件没有 roulette（GI 路径有），
// 层数只受用户设置 mirrorBounceLimit(1..8) 约束，与表面粗糙度/距离无关。
//
// 两个开关（都在 CameraUBO 里，运行时改，不需重编）：
//   cam.giRayParams.w        > 0.5 ⇒ RR 生效      （ORION_GLOSSY_RR，默认开，=0 关）
//   cam.pomSecondaryParams.y > 0.5 ⇒ 层数 LOD 生效（ORION_GLOSSY_STEP_LOD，默认关，=1 开）
//
// 2026-11-16：上面两行的默认值此前与 Settings.cpp 不一致（代码要求 =1 才开，
// 注释写"默认开"），导致 RR 这份"半成品"实际从未生效。现在以代码为准对齐：
// RR 默认开（Settings.cpp 改成 =0 才关），层数 LOD 保持默认关（仍是 A/B 开关）。
// ============================================================================

// 2026-11-16：glossy RR 的两个调节旋钮（编译期，便于 A/B 出数）。
// 语义与回退见下方 RR 块。
#ifndef GLOSSY_RR_DEPTH_RAMP
#define GLOSSY_RR_DEPTH_RAMP 0.5
#endif

#ifndef GLOSSY_RR_SURVIVAL_FLOOR
#define GLOSSY_RR_SURVIVAL_FLOOR 0.35
#endif

uint orionGlossyRrSeed()
{
    const uvec2 p = rtLogicalPixelCoord();
    return (p.x * 1973u +
            p.y * 9277u +
            uint(cam.frameCount) * 26699u) | 1u;
}

// 私有 RNG：不动主采样流 state，避免改变已有噪声模式。
float orionGlossyRandNext(inout uint s)
{
    s = s * 747796405u + 2891336453u;
    uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    w = (w >> 22u) ^ w;
    return float(w) * 2.3283064365386963e-10;
}

bool orionGlossyRrEnabled()
{
    return cam.giRayParams.w > 0.5f;
}

#ifndef GLOSSY_STEP_LOD_DEFAULT_ON
#define GLOSSY_STEP_LOD_DEFAULT_ON 1
#endif

bool orionGlossyStepLodEnabled()
{
    return (GLOSSY_STEP_LOD_DEFAULT_ON != 0) || (cam.pomSecondaryParams.y > 0.5f);
}

#ifndef GLOSSY_SHARC_CONTINUATION
#define GLOSSY_SHARC_CONTINUATION 1
#endif
#ifndef WATER_REFLECT_SUN_OPT_V1
#define WATER_REFLECT_SUN_OPT_V1 1
#endif

#ifndef ORION_K2_INDIRECT_TRACE_ONLY
vec3 tracePrimaryGlossyReflectionRadiance(
    vec3 primarySurfaceOrigin,
    vec3 startOrigin,
    vec3 startDirection,
    vec3 sunDirection,
    float primarySurfaceRoughness,
    inout uint state,
    out float firstSpecularHitDistance
)
{
    firstSpecularHitDistance =
        65504.0;

    // 2026-10-29 消融档 25/26：反射射线的距离上限。
    // 动机见档 24（noLabPbrMath）的同局实测：只把 roughness 换成常量 0.5，
    // trace 就从 16.30 掉到 9.91 ⇒ 钱在"反射射线打到哪里"，不在射线数。
    // 这里只截 tMax，材质 / 方向 / 采样数一概不动；超距按 miss 处理（rmiss 给天空色），
    // 所以观感是"远处反射变空"，属探针专用。
    // 2026-10-30 默认：反射射线距离预算 96 格（原来是 10000 = 不限）。
    //
    // 依据同局 A/B（alt:0,25/26）：tMax=96 -> 1.85 ms，tMax=48 -> 1.95 ms，
    // 两者几乎相同 ⇒ 省下的钱来自「打空的射线不必走完整个 BVH」，
    // 而不是「打到远处几何」。96 格以内的反射完全正确。
    //
    // 回退：消融档 32（glossyBudgetOff）回到 10000；
    //       或把下面的 kGlossyRayBudget 改回 10000.0f。
    // 2026-10-30 回退：96 -> 10000。tMax 截断会让"本该打到远处几何"的反射
    // 回落到天空色，天空明显更亮 ⇒ 室内被整体抬亮（用户实机反馈）。
    // 这条副作用当初评估不足；收益 1.85 ms 先放弃，探针档 25/26/32 保留以便复测。
    const float kGlossyRayBudget = 10000.0;

    float glossyRayMaxDistance =
        (cam.perfAblationStage.x == 32u)
            ? 10000.0
            : kGlossyRayBudget;

    if(cam.perfAblationStage.x == 25u)
    {
        glossyRayMaxDistance = 48.0;
    }
    else if(cam.perfAblationStage.x == 26u)
    {
        glossyRayMaxDistance = 96.0;
    }
    // 2026-11-01 档 35：tMax=192（12 区块）。与 25/26 一起给出远场替换
    // （SSR / SSPT / 天空）能省多少钱的三段上限：
    //   26 vs base = 96 格以外全部的钱；26 vs 35 = 6–12 区块段（96–192 格）；
    //   35 vs base = 12 区块以外。口径同 25/26：只截 tMax，超距按 miss 回
    //   天空色，观感不正确是定义，探针专用。
    else if(cam.perfAblationStage.x == 35u)
    {
        glossyRayMaxDistance = 192.0;
    }

    const float WATER_IOR =
        1.33;

    const float WATER_F0 =
        0.0200593;

    const vec3 WATER_ABSORPTION =
        waterAbsorptionColor();
        const vec3 WATER_SCATTER_COLOR = waterScatterColor();

const float WATER_EXTINCTION_DISTANCE =
    16.0;

    const float RAY_BIAS =
        0.001;

    const int MAX_WATER_INTERFACE_STEPS =
    6;


// A3-4：Primary glossy 命中第一个 opaque 后，
// 最多允许 1 次额外 PBR indirect continuation。
const int MAX_OPAQUE_INDIRECT_CONTINUATIONS =
    1;

// 2026-10-03 镜厅（用户要求）：**纯镜面**命中（LabPBR 金属 + 粗糙度 < 0.05）走
// **确定性镜像续弹**（reflect 方向，无随机采样），层数由 UBO 控制（1..8，出厂
// 1 = 现状）。非镜面命中走原有随机续弹（上限不变）。最坏路径：
// 6 次水界面 + 8 层镜面续弹 + 1 次随机续弹 + 它的终点 ⇒ 16。
const int MIRROR_BOUNCE_HARD_CAP = 8;
const int MAX_PATH_TRACE_STEPS =
    MAX_WATER_INTERFACE_STEPS +
    MIRROR_BOUNCE_HARD_CAP +
    MAX_OPAQUE_INDIRECT_CONTINUATIONS +
    2;


vec3 throughput =
    vec3(1.0);


vec3 accumulatedRadiance =
    vec3(0.0);


vec3 currentOrigin =
    startOrigin;


vec3 currentDirection =
    normalize(
        startDirection
    );


bool insideWater =
    false;


int waterInterfaceStepCount =
    0;


// 2026-11-03：本像素在这条路径上已经走过的**水内光程**（格）。
// 只服务吸收项的浅水软化，与 transmissionInfo.waterPathLength（导给
// SSS/焦散的那份）互不影响。
float waterPathTravelled =
    0.0;

// 浅水区厚度（格）。与 18617 处那条"已确认 0~3 格保持高通透"同值；
// 那份声明在另一个作用域里、主走查取不到，所以这里自带一份。
const float kShallowWaterDistance =
    3.0;


int opaqueContinuationCount =
    0;


// 2026-10-03 镜厅：已完成的确定性镜像弹射层数与上限（UBO 水尾块保留槽 2）
int mirrorBounceCount =
    0;

int mirrorBounceLimit =
    clamp(int(cam.waterFftParams2.y + 0.5), 1, MIRROR_BOUNCE_HARD_CAP);

// 首次命中后按粗糙度/距离收紧的**有效**层数（见下方 pathStep==0 段）。
uint orionGlossyRrState = orionGlossyRrSeed();
int orionGlossyBounceLimit = mirrorBounceLimit;

#if GLOSSY_SHARC_CONTINUATION
float glossyRoughnessPrev = clamp(primarySurfaceRoughness, 0.0, 0.99);
const bool glossySharcEnabled = sharcQueryActive() && (cam.perfPrimary.z > 0u);
#endif


for(
    int pathStep = 0;
    pathStep < MAX_PATH_TRACE_STEPS;
    ++pathStep
)
    {
        hit.hitDistance =
            1.0;


        hit.state = makeTraceState(RT_PURPOSE_FULL);

        // 2026-11-09：反射路径的视差是"每条 glossy 路径、每一步"都要付的固定税
        // （material_resolve 的 POM 判据里没有射线用途，最多 18 步 x 296 次
        //  texelFetch）。这里沿用 GI 那条同源的 state 位，命中侧走既有早退分支。
        // 关掉：ORION_POM_SECONDARY=0 ⇒ 逐位回到改动前。
        if(secondaryRayPomSkipEnabled())
        {
            hit.state |= RT_STATE_GI_NO_POM_BIT;
        }

        // ================================================================
        // 2026-11-16：俄罗斯轮盘从"固定阈值筛选"改成 GI 同形的**存活概率模型**。
        //
        // 旧写法（q < 0.30 才掷骰，存活概率 = q）在镜面链上形同虚设：金属每跳
        // 只乘一个 Fresnel（0.9+），q 永远 > 0.3 ⇒ RR 一次也不触发，深度完全由
        // 整数 mirrorBounceLimit 决定 ⇒ "镜子里映镜子少一层"肉眼可见，且空房间
        // 和堆满镜面的房间付一样的射线钱。
        //
        //   term      = (1 - q) + GLOSSY_RR_DEPTH_RAMP * depthRamp
        //   depthRamp = max(0, pathStep / cap - 0.4)   // 前 40% cap 不参与
        //   survival  = clamp(1 - term, 0.35, 1)
        //   存活      ⇒ throughput /= survival          // 无偏，权重 ≤ 2.86×
        //
        // 语义：q 越高（镜面越亮、Fresnel 越接近 1）越难终止；越接近 cap 越容易
        // 终止 ⇒ 同一条链里亮的自动多弹、暗的自动早停，**期望**深度低于 cap，
        // 但 cap 仍是硬上限（纯镜面的 :433 判断照样先返回）。第一跳不抽签，保证
        // 主反射始终存在；抽签用私有 RNG state，不动主采样流。
        //
        // 0.35 的地板只抬方差、不引入偏差（推导见 docs/decisions/russian-roulette.md）。
        // 回退：ORION_GLOSSY_RR=0 整块不执行；或 -DGLOSSY_RR_DEPTH_RAMP=0 只留亮度项；
        //       -DGLOSSY_RR_SURVIVAL_FLOOR=1.0 回到"只抽签不减预算"的极端保守档。
        // ================================================================
        if(orionGlossyRrEnabled() && pathStep > 0)
        {
            const float orionQ =
                clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.0, 1.0);

            // 萤火虫闸：throughput 已 ≈ 0 的路径继续抽签，只会把 1/0.02 的权重
            // 抬得更离谱。改动前同样是硬 return，行为不变。
            if(orionQ < 0.02)
            {
                return accumulatedRadiance;
            }

            const float orionDepthRamp =
                max(
                    0.0,
                    float(pathStep) / float(max(orionGlossyBounceLimit, 1)) - 0.4
                );

            const float orionSurvival =
                clamp(
                    1.0 - ((1.0 - orionQ) + GLOSSY_RR_DEPTH_RAMP * orionDepthRamp),
                    GLOSSY_RR_SURVIVAL_FLOOR,
                    1.0
                );

            if(orionSurvival < 1.0)
            {
                if(orionGlossyRandNext(orionGlossyRrState) > orionSurvival)
                {
                    return accumulatedRadiance;
                }

                throughput /= orionSurvival;
            }
        }

#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
        serTraceRay(
            currentOrigin,
            0.001,
            currentDirection,
            glossyRayMaxDistance,
            secondaryTraverseFlags(),
            g_secondaryCullMask,
            pathStep
        );
#else
#ifdef RT_ENABLE_SER
        // Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
        if(cam.perfSerAb.z != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.001,
                currentDirection,
                glossyRayMaxDistance,
                secondaryTraverseFlags(),
                g_secondaryCullMask,
                pathStep
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
                currentOrigin,
                0.001,
                currentDirection,
                glossyRayMaxDistance,
                0
            );
        }
#endif


        if(pathStep == 0)
        {
            if(payload.hitDistance >= 0.0)
            {
                vec3 firstSpecularHitPos =
                    currentOrigin +
                    currentDirection *
                    payload.hitDistance;

                firstSpecularHitDistance =
                    length(
                        firstSpecularHitPos -
                        primarySurfaceOrigin
                    );
            }
            else
            {
                firstSpecularHitDistance =
                    65504.0;
            }
        }

        // 层数 LOD：首次命中后按表面粗糙度与命中距离收紧镜面弹射层数。
        if(pathStep == 0 && orionGlossyStepLodEnabled())
        {
            const float orionGlossyRough =
                clamp(payload.roughness, 0.0, 1.0);

            if(orionGlossyRough > 0.30 || firstSpecularHitDistance > 32.0)
            {
                orionGlossyBounceLimit = min(orionGlossyBounceLimit, 2);
            }
            else if(orionGlossyRough > 0.15 || firstSpecularHitDistance > 16.0)
            {
                orionGlossyBounceLimit = min(orionGlossyBounceLimit, 4);
            }
        }


        // =============================================
        // Sky
        // =============================================

        if(payload.hitDistance < 0.0)
{
    // The in-water segment is already Beer-Lambert attenuated in
    // throughput.  Preserve the environment term instead of producing a
    // black reflection fragment at a fluid-section boundary.
    return
        accumulatedRadiance +
        throughput *
        unpackAlbedoUnorm(payload.albedoPacked);
}


        // 后续 visibility trace 会覆盖 payload，
        // 所以这里必须立即保存。
        float hitDistance =
            payload.hitDistance;


        vec3 hitNormal =
            normalize(
                payload.normal
            );


        vec3 hitAlbedo =
            unpackAlbedoUnorm(payload.albedoPacked);


        float hitMaterialId =
            getLegacyMaterialId(payload.state);


        float hitRoughness =
            payload.roughness;


        float hitF0Metal =
            payload.f0Metal;


        vec3 hitEmission =
            payload.emission;


        float hitPOMSelfShadow =
            clamp(
                payload.pomSelfShadow,
                0.0,
                1.0
            );


        vec3 hitPos =
            currentOrigin +
            currentDirection *
            hitDistance;


        // 当前 ray segment 真正在水中传播。
        if(insideWater)
        {
#ifndef VPT_WATER_SHALLOW_ABSORPTION
// 1 = 前 WATER_SHALLOW_DISTANCE 格几乎不吸收（**当前默认**，与本文件
//     18617 那条注释的"0~3 格保持高通透 / 约 16 格后水底基本不可见"一致）。
// 0 = 逐段满吸收（2026-11-03 之前的行为：1 格就 0.84、5 格 0.41 ⇒ 一片蓝）。
#define VPT_WATER_SHALLOW_ABSORPTION 1
#endif
#if VPT_WATER_SHALLOW_ABSORPTION
            // 吸收只作用在"累计光程超出浅水区"的那部分，按段增量算，
            // 所以海洋那种连续深水仍然会按真实深度衰减（10 格 ≈ 0.28 透过）。
            const float previousWaterPath = waterPathTravelled;
            waterPathTravelled += hitDistance;
            const float absorbedDepth =
                max(waterPathTravelled - kShallowWaterDistance, 0.0) -
                max(previousWaterPath - kShallowWaterDistance, 0.0);
            throughput *=
                exp(-WATER_ABSORPTION * absorbedDepth);
#else
            waterPathTravelled += hitDistance;
            throughput *=
                exp(
                    -WATER_ABSORPTION *
                    hitDistance
                );
#endif
        }


        // =============================================
        // Opaque hit
        // =============================================

if(hitMaterialId <= 0.5)
{
    const bool hitIsPureMirror =
        decodeMetallicFromCode(decodeF0MetalCode(hitF0Metal)) > 0.5 &&
        hitRoughness < 0.05;

#if GLOSSY_SHARC_CONTINUATION
    if(!hitIsPureMirror && glossySharcEnabled)
    {
        SharcParameters glossySharcParams = sharcBuildParameters();
        const uint glossySharcLevel = HashGridGetLevel(
            hitPos, glossySharcParams.hashGridParameters);
        const float glossySharcVoxelSize = HashGridGetVoxelSize(
            glossySharcLevel, glossySharcParams.hashGridParameters);
        const float glossySharcR = min(glossyRoughnessPrev, 0.99);
        const float glossySharcAlpha = glossySharcR * glossySharcR;
        const float glossySharcFootprint = hitDistance *
            sqrt(0.5 * glossySharcAlpha * glossySharcAlpha /
                 max(1.0 - glossySharcAlpha * glossySharcAlpha, 1.0e-5));
        if(hitDistance > glossySharcVoxelSize * sqrt(3.0) &&
           glossySharcFootprint > glossySharcVoxelSize)
        {
            SharcHitData glossySharcHit;
            glossySharcHit.positionWorld = hitPos;
            glossySharcHit.normalWorld =
                dot(hitNormal, currentDirection) > 0.0 ? -hitNormal : hitNormal;
            glossySharcHit.materialDemodulation = max(hitAlbedo, vec3(0.05));
            glossySharcHit.emissive = hitEmission;
            vec3 glossySharcRad = vec3(0.0);
            if(SharcGetCachedRadiance(
                glossySharcParams, glossySharcHit, glossySharcRad, false))
            {
                return accumulatedRadiance + throughput * glossySharcRad;
            }
        }
    }
#endif

    // =================================================
    // A3-4
    //
    // 当前 opaque 自己的 local radiance：
    //
    // emission
    // +
    // 完整 PBR direct sun
    // =================================================

    // 2026-10-03 镜厅：**纯镜面**（LabPBR 金属 + 粗糙度 < 0.05）命中：
    //   · 不累加镜面自己的表面辐射（眼睛从镜面只该看到反射像）
    //   · 走确定性镜像续弹（reflect 方向，吞吐 × Fresnel，无随机采样）
    //   · 层数到顶 → 退化为普通墙面观感（拿它自己的 direct/自发光兜底）并返回
    // 非镜面命中走原有随机续弹（上限与行为都不变）。
    if(hitIsPureMirror)
    {
        if(mirrorBounceCount >= orionGlossyBounceLimit)
        {
            const vec3 surfaceRadiance =
                evaluateOpaqueSurfaceRadiance(
                    hitPos,
                    hitNormal,
                    -currentDirection,
                    hitAlbedo,
                    hitRoughness,
                    hitF0Metal,
                    hitEmission,
                    hitPOMSelfShadow,
                    sunDirection
                );

            accumulatedRadiance +=
                throughput *
                surfaceRadiance;

            return
                accumulatedRadiance;
        }

        const float mirrorNdotV =
            max(
                dot(
                    hitNormal,
                    -currentDirection
                ),
                0.0
            );

        if(mirrorNdotV <= 1.0e-5)
        {
            return
                accumulatedRadiance;
        }

        const int mirrorF0Code =
            decodeF0MetalCode(
                hitF0Metal
            );

        const vec3 mirrorF0 =
            decodeBaseF0(
                mirrorF0Code,
                hitAlbedo
            );

        throughput *=
            fresnelSchlick(
                mirrorNdotV,
                mirrorF0
            );

        currentOrigin =
            hitPos +
            hitNormal *
            RAY_BIAS;

        currentDirection =
            reflect(
                currentDirection,
                hitNormal
            );

        mirrorBounceCount +=
            1;

        continue;
    }

    vec3 surfaceRadiance =
        evaluateOpaqueSurfaceRadiance(
            hitPos,
            hitNormal,
            -currentDirection,
            hitAlbedo,
            hitRoughness,
            hitF0Metal,
            hitEmission,
            hitPOMSelfShadow,
            sunDirection
        );

    accumulatedRadiance +=
        throughput *
        surfaceRadiance;

#ifndef GLOSSY_ROUGH_SKIP_CONTINUATION
#define GLOSSY_ROUGH_SKIP_CONTINUATION 1
#endif
#if GLOSSY_ROUGH_SKIP_CONTINUATION
    if(primarySurfaceRoughness >= 0.35)
    {
        return accumulatedRadiance;
    }
#endif


    // =================================================
    // 已经完成一次 opaque indirect continuation。
    //
    // 第二个 opaque 就是终点：
    // 取它自己的 emission + direct，然后返回。
    // =================================================

    if(
        opaqueContinuationCount >=
        MAX_OPAQUE_INDIRECT_CONTINUATIONS
    )
    {
        return
            accumulatedRadiance;
    }


    // =================================================
    // 当前 opaque 的 view/outgoing direction
    // =================================================

    vec3 bounceV =
        normalize(
            -currentDirection
        );


    float bounceNdotV =
        max(
            dot(
                hitNormal,
                bounceV
            ),
            0.0
        );


    if(bounceNdotV <= 1.0e-5)
    {
        return
            accumulatedRadiance;
    }


    // =================================================
    // LabPBR material decode
    // =================================================

    int hitF0MetalCode =
        decodeF0MetalCode(
            hitF0Metal
        );


    float hitMetallic =
        decodeMetallicFromCode(
            hitF0MetalCode
        );


    vec3 bounceF0 =
        decodeBaseF0(
            hitF0MetalCode,
            hitAlbedo
        );


    float bounceRoughness =
        max(
            hitRoughness,
            0.05
        );


    // =================================================
    // Fresnel-based lobe selection
    //
    // 和已经确认的 A3-1 使用相同规则。
    // =================================================

    vec3 bounceFForChoice =
        fresnelSchlick(
            bounceNdotV,
            bounceF0
        );


    float bounceFMax =
        max(
            max(
                bounceFForChoice.r,
                bounceFForChoice.g
            ),
            bounceFForChoice.b
        );


    float bounceSpecularProbability =
        hitMetallic > 0.5
            ? 1.0
            : clamp(
                bounceFMax,
                0.05,
                0.95
            );


    bool chooseBounceSpecular =
        rand(state) <
        bounceSpecularProbability;


    vec3 bounceDirection =
        vec3(0.0);


    vec3 bounceWeight =
        vec3(0.0);


    // =================================================
    // GGX Specular continuation
    // =================================================

    if(chooseBounceSpecular)
    {
        vec3 bounceH =
            sampleGGXVNDF(
                hitNormal,
                bounceV,
                bounceRoughness,
                vec2(rand(state), rand(state))
            );


        float bounceVdotH =
            max(
                dot(
                    bounceV,
                    bounceH
                ),
                0.0
            );


        float bounceNdotH =
            max(
                dot(
                    hitNormal,
                    bounceH
                ),
                0.0
            );


        bounceDirection =
            normalize(
                reflect(
                    -bounceV,
                    bounceH
                )
            );


        float bounceNdotL =
            max(
                dot(
                    hitNormal,
                    bounceDirection
                ),
                0.0
            );


        if(
            bounceVdotH <= 1.0e-5 ||
            bounceNdotH <= 1.0e-5 ||
            bounceNdotL <= 1.0e-5
        )
        {
            return
                accumulatedRadiance;
        }


        vec3 bounceF =
            fresnelSchlick(
                bounceVdotH,
                bounceF0
            );


        float bounceG =
            geometrySmith(
                hitNormal,
                bounceV,
                bounceDirection,
                bounceRoughness
            );


        float bounceG1V =
            ggxVNDFMaskingG1(
                bounceNdotV,
                bounceRoughness
            );


        bounceWeight =
            (
                bounceF *
                multiscatterEnergyCompensation(
                    bounceF0,
                    bounceRoughness
                ) *
                (bounceG / max(bounceG1V, 1.0e-5))
            ) /
            max(
                bounceSpecularProbability,
                1.0e-5
            );
    }


    // =================================================
    // Diffuse continuation
    // =================================================

    else
    {
        float bounceDiffuseProbability =
            max(
                1.0 -
                bounceSpecularProbability,
                1.0e-5
            );


        vec3 bounceKD =
            (
                vec3(1.0) -
                bounceFForChoice
            ) *
            (
                1.0 -
                hitMetallic
            );


        bounceDirection =
            sampleHemisphereCosine(
                hitNormal,
                state
            );


        // Cosine hemisphere：
        //
        // BRDF * NdotL / PDF
        // =>
        // kD * albedo
        bounceWeight =
            (
                bounceKD *
                hitAlbedo
            ) /
            bounceDiffuseProbability;
    }


    // =================================================
    // 与已经确认的 A3-1 保持同一 GI transport 尺度。
    // =================================================

    throughput *=
        bounceWeight *
        BSDF_GI_THROUGHPUT_SCALE;


    currentOrigin =
        hitPos +
        hitNormal *
        RAY_BIAS;


    currentDirection =
        bounceDirection;

#if GLOSSY_SHARC_CONTINUATION
    glossyRoughnessPrev += chooseBounceSpecular ? bounceRoughness : 1.0;
#endif

    opaqueContinuationCount +=
        1;


    continue;
}


        // =============================================
        // Water interface
        //
        // rchit 已确认：
        // 1.0 = Air -> Water
        // 2.0 = Water -> Air
        // =============================================
if(
    waterInterfaceStepCount >=
    MAX_WATER_INTERFACE_STEPS
)
{
    return
        accumulatedRadiance;
}


waterInterfaceStepCount +=
    1;

        hitNormal =
            waterInterfaceNormalForRay(
                hitNormal,
                currentDirection
            );

        bool exitingWater =
            hitMaterialId > 1.5;


        float cosTheta =
            clamp(
                -dot(
                    currentDirection,
                    hitNormal
                ),
                0.0,
                1.0
            );


        float fresnel =
            WATER_F0 +
            (
                1.0 -
                WATER_F0
            ) *
            pow(
                1.0 -
                cosTheta,
                5.0
            );


        float waterChoice =
            rand(
                state
            );


        // =============================================
        // Reflection
        // =============================================

        if(
            waterChoice <
            fresnel
        )
        {
            // 没跨介质：
            //
            // Air -> Water 的界面反射：
            // 仍在 Air。
            //
            // Water -> Air 的界面反射：
            // 仍在 Water。
            insideWater =
                exitingWater;


            currentOrigin =
                hitPos +
                hitNormal *
                RAY_BIAS;


            currentDirection =
                normalize(
                    reflect(
                        currentDirection,
                        hitNormal
                    )
                );


            continue;
        }


        // =============================================
        // Transmission
        // =============================================

        float eta =
            exitingWater
                ? WATER_IOR
                : (1.0 / WATER_IOR);


        vec3 refractedDirection =
            refract(
                currentDirection,
                hitNormal,
                eta
            );


        // Total internal reflection
        if(
            dot(
                refractedDirection,
                refractedDirection
            ) < 1.0e-6
        )
        {
            insideWater =
                exitingWater;


            currentOrigin =
                hitPos +
                hitNormal *
                RAY_BIAS;


            currentDirection =
                normalize(
                    reflect(
                        currentDirection,
                        hitNormal
                    )
                );


            continue;
        }


        // 真正跨介质
        insideWater =
            !exitingWater;


        currentOrigin =
            hitPos -
            hitNormal *
            RAY_BIAS;


        currentDirection =
            normalize(
                refractedDirection
            );
    }


return
    accumulatedRadiance;
}
#endif
// =====================================================
// Explicit reflected sunlight from flat water
//
// Path:
//
// Sun
//   -> horizontal water surface
//   -> specular reflection
//   -> opaque receiver surface
//
// This explicitly samples the specular water path instead
// of waiting for a random GI ray to hit the exact sun path.
// =====================================================
float traceWaterReflectedSun(
    vec3 receiverPos,
    vec3 receiverNormal,
    vec3 sunDirection
)
{
#if WATER_REFLECT_SUN_OPT_V1
    if(sunDirection.y <= 0.02)
    {
        return 0.0;
    }
#endif

    const vec3 WATER_PLANE_NORMAL =
        vec3(0.0, 1.0, 0.0);

    const float WATER_F0 =
        0.0200593;

    // -------------------------------------------------
    // For a flat horizontal mirror:
    //
    // receiver -> virtual sun
    //
    // This ray must hit the water surface if the receiver
    // can receive the reflected sunlight.
    // -------------------------------------------------
    vec3 toWaterDir =
        normalize(
            reflect(
                sunDirection,
                WATER_PLANE_NORMAL
            )
        );

    float receiverNdotL =
        max(
            dot(
                receiverNormal,
                toWaterDir
            ),
            0.0
        );

    // Receiver faces away from reflected light.
    if(receiverNdotL <= 0.0)
    {
        return 0.0;
    }


    // =================================================
    // Receiver -> Water (2026-11-26 Batch V2: inline rayQueryEXT fast-path)
    // =================================================
#if WATER_REFLECT_SUN_OPT_V1
    const float waterReflectMaxDist = 64.0;
#else
    const float waterReflectMaxDist = 10000.0;
#endif

    vec3 waterHitPos = receiverPos;
#ifndef ORION_K2I_COMPOSE_COMPUTE
    {
        rayQueryEXT waterHitQuery;
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        rayQueryInitializeEXT(
            waterHitQuery,
            topLevelAS,
            gl_RayFlagsNoneEXT,
            0xcfu,
            receiverPos + toWaterDir * 0.001,
            0.02,
            toWaterDir,
            waterReflectMaxDist
        );
        int waterGuard = 0;
        while(rayQueryProceedEXT(waterHitQuery) && ++waterGuard < 32)
        {
            if(rayQueryGetIntersectionTypeEXT(waterHitQuery, false) ==
               gl_RayQueryCandidateIntersectionTriangleEXT)
            {
                const uint candSbtOff =
                    rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(
                        waterHitQuery, false);
                if(candSbtOff == 3u || candSbtOff == 0u || candSbtOff == 1u)
                {
                    rayQueryConfirmIntersectionEXT(waterHitQuery);
                }
            }
        }
        if(rayQueryGetIntersectionTypeEXT(waterHitQuery, true) ==
           gl_RayQueryCommittedIntersectionNoneEXT)
        {
            return 0.0;
        }
        if(rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(
               waterHitQuery, true) != 3u)
        {
            return 0.0;
        }

        const uint instanceIdx =
            uint(rayQueryGetIntersectionInstanceCustomIndexEXT(waterHitQuery, true)) +
            uint(rayQueryGetIntersectionGeometryIndexEXT(waterHitQuery, true));
        const uvec4 sectionAddressEntry =
            sectionVertexAddressBuffer.vertexAddresses[instanceIdx];
        const uint64_t rawAddr = packUint2x32(sectionAddressEntry.xy);
        if(rawAddr == 0ul)
        {
            return 0.0;
        }

        const bool indexedQuads = (sectionAddressEntry.w & 1u) != 0u;
        const uint sectionStride = sectionAddressEntry.w & ~1u;
        const uint primitiveId =
            uint(rayQueryGetIntersectionPrimitiveIndexEXT(waterHitQuery, true));
        uint i0;
        uint i1;
        uint i2;
        if(indexedQuads)
        {
            const uint quadBase = (primitiveId >> 1u) * 4u;
            i0 = quadBase;
            if((primitiveId & 1u) == 0u)
            {
                i1 = quadBase + 1u;
                i2 = quadBase + 2u;
            }
            else
            {
                i1 = quadBase + 2u;
                i2 = quadBase + 3u;
            }
        }
        else
        {
            i0 = primitiveId * 3u;
            i1 = i0 + 1u;
            i2 = i0 + 2u;
        }

        const vec3 p0 = glossyLoadVertexPos(rawAddr, sectionStride, i0);
        const vec3 p1 = glossyLoadVertexPos(rawAddr, sectionStride, i1);
        const vec3 p2 = glossyLoadVertexPos(rawAddr, sectionStride, i2);
        vec3 geoNormal = cross(p1 - p0, p2 - p0);
        const float nLen2 = dot(geoNormal, geoNormal);
        if(nLen2 <= 1.0e-12)
        {
            return 0.0;
        }
        geoNormal *= inversesqrt(nLen2);

        // Require Air -> Water horizontal top-facing surface.
        if(geoNormal.y <= 0.5 || dot(geoNormal, toWaterDir) >= 0.0)
        {
            return 0.0;
        }

        const float waterHitT = rayQueryGetIntersectionTEXT(waterHitQuery, true);
        waterHitPos = receiverPos + toWaterDir * (0.001 + waterHitT);

        // =================================================
        // Water -> Sun visibility (inline rayQueryEXT)
        // =================================================
        rayQueryEXT glossSunVisQuery;
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        rayQueryInitializeEXT(
            glossSunVisQuery,
            topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT,
            0xcfu,
            waterHitPos + geoNormal * 0.02 + sunDirection * 0.001,
            0.02,
            sunDirection,
            10000.0
        );
        int glossSunVisGuard = 0;
        while(rayQueryProceedEXT(glossSunVisQuery) && ++glossSunVisGuard < 32)
        {
            if(rayQueryGetIntersectionTypeEXT(glossSunVisQuery, false) ==
               gl_RayQueryCandidateIntersectionTriangleEXT)
            {
                const uint candSbtOff =
                    rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(
                        glossSunVisQuery, false);
                if(candSbtOff != 2u && candSbtOff != 3u)
                {
                    rayQueryConfirmIntersectionEXT(glossSunVisQuery);
                }
            }
        }
        if(rayQueryGetIntersectionTypeEXT(glossSunVisQuery, true) !=
           gl_RayQueryCommittedIntersectionNoneEXT)
        {
            return 0.0;
        }
    }
#else
    return 0.0;
#endif


    // =================================================
    // 2026-10-19 reflected caustics (ceiling "wave light")
    //
    // The flat-mirror path above decides **whether** this
    // receiver sees the sun at all; the host's forward-
    // rasterised map (same FFT slopes, reflect(-sun, n))
    // supplies **how much** the wavy surface concentrates
    // it.  The map is keyed by the water source point, so
    // the pattern is world-anchored and camera-independent
    // — the same convention as the refracted map (binding 69).
    //
    // Flat water = 1 (no change).  Uncovered texels stay 0
    // and must read as "unperturbed", hence the > 0 test.
    // =================================================
    // 2026-11-20 Remix 核心对齐：Remix 的反射瓣只吃场景辐亮度，**不采焦散图**
    //（translucent_surface_material_interaction.slangh 的
    // CalcSpecularReflectionSample 没有任何 caustic 输入）。这里原来的
    // 1 + (d-1)*4（clamp 到 6）在掠射 F≈1 时会把波纹打成过曝白斑，
    // 且新泼溅图的 floor=0.20 会让平坦水面落进"负增益"档（黑洞）。
#ifndef WATER_REFLECT_CAUSTIC_ENABLED
#define WATER_REFLECT_CAUSTIC_ENABLED 0
#endif
    float reflectedCausticGain = 1.0;
#if WATER_REFLECT_CAUSTIC_ENABLED
    if(cam.waterFftParams.y > 0.0 && cam.waterFftParams2.w > 0.5)
    {
        float projScale = float(rtLaunchSize().y) /
            (2.0 * max(tan(cam.cameraParams.z * 0.5), 0.05));
        float causticLod = waterCausticMapLodFromProj(
            receiverPos,
            length(receiverPos - cam.viewInverse[3].xyz),
            projScale);
        const float reflectHeight = max(
            waterHitPos.y - cam.waterSimSurfaceAndOrigin.x, 0.0);
        const float mapReflectHeight = 16.0;
        const vec2 flatReflectOffset = vec2(-sunDirection.x, -sunDirection.z) *
            ((mapReflectHeight - reflectHeight) /
                max(abs(sunDirection.y), 0.02));
        float causticDensity = textureLod(
            waterReflectCausticMap,
            (waterHitPos.xz + flatReflectOffset) /
                max(cam.waterFftParams.x, 1.0e-3),
            causticLod).r;
        if(causticDensity > 1.0e-4)
        {
            float causticStrength =
                clamp(cam.waterWaveParams.w, 0.0, 8.0);
            reflectedCausticGain = clamp(
                1.0 + (causticDensity - 1.0) * causticStrength,
                0.0,
                6.0);
        }
    }
#endif


    // =================================================
    // Fresnel reflection
    // =================================================
    float waterCosTheta =
        clamp(
            dot(
            WATER_PLANE_NORMAL,
            sunDirection
            ),
            0.0,
            1.0
        );

    float waterFresnel =
        WATER_F0 +
        (
            1.0 -
            WATER_F0
        ) *
        pow(
            1.0 -
            waterCosTheta,
            5.0
        );


    return
        receiverNdotL *
        waterFresnel *
        reflectedCausticGain;
}