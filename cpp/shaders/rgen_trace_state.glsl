
#ifdef ORION_K2I_COMPOSE_COMPUTE
HitPayload payload;
#else
layout(location = 0) rayPayloadEXT HitPayload payload;
#endif
// Old mode keeps one variable for both roles; the seed assignments and the
// Any-Hit-only sites were retargeted to `hit` when the split was introduced,
// so alias it back instead of keeping two code paths.
#define hit payload

// 2026-11-16：可见性探针早退宏的 rgen 侧镜像。**必须与 rt_test.rahit 头部的
// 默认值保持一致**：三个 purpose==3 点位改 inline ray query 的等价性，成立前提就是
// rahit 对 purpose 3 在 main() 最开头早退（只写 gl_HitTEXT 并 return，远场去重 /
// 粒子不遮挡 / 玻璃早退 / alpha test / CLASSIFY 等后续 any-hit 分支全部不可达）。
// 若 rahit 侧用 -DRTAO_VISIBILITY_EARLYOUT=0 编译，等价不成立，rgen 侧这份也要
// 跟着改成 0，否则 query 臂会在不等价的前提下被启用。
#ifndef RTAO_VISIBILITY_EARLYOUT
#define RTAO_VISIBILITY_EARLYOUT 1
#endif

#ifdef RT_ENABLE_SER
#ifndef ORION_SER_FUSED_ONLY
#define ORION_SER_FUSED_ONLY 1
#endif
// Perf-L10.8 - SER now covers every ray path, not only the diffuse-GI bounce.
//
// serTraceRay is the single funnel. Under ORION_SER_FUSED_ONLY=1 (default),
// all runtime serMode branching and SerLodRoles buffer lookups are compiled out,
// emitting a single hitObjectTraceReorderExecuteEXT instruction.
//
// Callers pass the exact ray flags, cull mask and ray interval they would have
// passed to traceRayEXT.  SER only changes how the hit shader is scheduled; it
// never changes which ray is traced, so the two paths must stay pixel-equal.

#if !ORION_SER_FUSED_ONLY
layout(std430, binding = 19, set = 0) readonly buffer SerLodRoles {
    uint roles[];
} serLodRoles;

#define SER_ROLE_TERRAIN 0u
#define SER_ROLE_SLOT_LIMIT (1u << 16)
#endif

void serTraceRayOffset(
    vec3 origin,
    float tMin,
    vec3 direction,
    float tMax,
    uint rayFlags,
    uint cullMask,
    uint hint,
    uint sbtOffset
)
{
    // RT 计数器：SER 漏斗入口计一次射线（与下方 fallback 的 traceRayEXT 臂一一对应）。
    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntRayIssued();
    #endif
#if ORION_SER_FUSED_ONLY
    hitObjectEXT fusedObject;
    hitObjectTraceReorderExecuteEXT(
        fusedObject,
        topLevelAS,
        rayFlags,
        cullMask,
        sbtOffset,
        0,
        0,
        origin,
        tMin,
        direction,
        tMax,
        0
    );
    return;
#else
    uint serMode = max(cam.perfSerAb.y, cam.perfSerAb.z);
    if(serMode == 6u)
    {
        serMode = 1u;
    }

    if(serMode == 5u)
    {
        hitObjectEXT fusedObject;
        hitObjectTraceReorderExecuteEXT(
            fusedObject,
            topLevelAS,
            rayFlags,
            cullMask,
            sbtOffset,
            0,
            0,
            origin,
            tMin,
            direction,
            tMax,
            0
        );
        return;
    }

    hitObjectEXT hitObject;
    hitObjectTraceRayEXT(
        hitObject,
        topLevelAS,
        rayFlags,
        cullMask,
        sbtOffset,
        0,
        0,
        origin,
        tMin,
        direction,
        tMax,
        0
    );

    if(serMode == 4u)
    {
        uint classKey = 0u;
        if(!hitObjectIsMissEXT(hitObject))
        {
            const uint slot = hitObjectGetInstanceCustomIndexEXT(hitObject);
            uint role = SER_ROLE_TERRAIN;
            if(slot < SER_ROLE_SLOT_LIMIT)
            {
                role = serLodRoles.roles[slot];
            }
            classKey = 1u + (((role & 0x3u) << 2u)
                | (hitObjectGetShaderBindingTableRecordIndexEXT(hitObject) & 0x3u));
        }
        hitObjectReorderExecuteEXT(hitObject, classKey, 5u, 0);
    }
    else if(serMode == 3u)
    {
        hitObjectExecuteShaderEXT(hitObject, 0);
    }
    else if(serMode == 2u)
    {
        hitObjectReorderExecuteEXT(hitObject, 0);
    }
    else
    {
        uint sbtIndex = hitObjectGetShaderBindingTableRecordIndexEXT(hitObject);

#ifndef GI_SER_HINT_SBT_BITS
#define GI_SER_HINT_SBT_BITS 1
#endif
        if((GI_SER_HINT_SBT_BITS != 0) && !perfL10SerKeyMinimal())
        {
            uint coherenceHint = ((hint & 0x3u) << 2u) | (sbtIndex & 0x3u);
            hitObjectReorderExecuteEXT(hitObject, coherenceHint, 4u, 0);
        }
        else
        {
            uint coherenceHint = (hint & 0x3u);
            hitObjectReorderExecuteEXT(hitObject, coherenceHint, 2u, 0);
        }
    }
#endif
}

void serTraceRay(
    vec3 origin,
    float tMin,
    vec3 direction,
    float tMax,
    uint rayFlags,
    uint cullMask,
    uint hint
)
{
    serTraceRayOffset(origin, tMin, direction, tMax, rayFlags, cullMask, hint, 0u);
}

// g_secondaryCullMask 在 rt_test.rgen 顶部全局声明，供本地光 NEE 与次级光线共用。
// Entry point of the diffuse-GI bounce.  Kept as a named wrapper so the call
// site and its measured behaviour stay recognisable; the body is now the shared
// funnel, so GI cannot drift away from the other paths.
void traceDiffuseSer(vec3 origin, vec3 direction, uint bouncePhase)
{
    serTraceRay(
        origin,
        0.001,
        direction,
        perfGiTraceMaxDistance(10000.0),
        secondaryTraverseFlags(),
        g_secondaryCullMask,
        bouncePhase
    );
}
#endif

// state is the compact transport contract shared by every RT shader.
// It replaces the overloaded floating materialId without increasing payload
// size.  Low bits are supplied by rgen; hit shaders preserve them and append
// their result classification.
const uint RT_STATE_PURPOSE_MASK       = 0x7u;
const uint RT_STATE_HIT_CLASS_SHIFT    = 3u;
const uint RT_STATE_WATER_EVENT_SHIFT  = 5u;
const uint RT_STATE_GLASS_BIT          = 1u << 7u;
const uint RT_STATE_GLASS_BOTTOM_BIT   = 1u << 8u;
const uint RT_STATE_PROFILE_SHIFT      = 9u;
// 2026-10-07（用户要求）：普通（未染色）玻璃标记，bit 12（bit 0..11 已用满）。
// 置位者：rchit 的 t12IsGlass 分支（rt_test_translucent.rchit 是主相机玻璃的真实入口，
// rt_test.rchit 兜底），判据 = glassVertexFactorIsFaceShade(顶点色) ⇒ 没注入染料。
// 消费者：isClearGlassHit() 与 tracePrimaryGenericTransparentRadiance()。
const uint RT_STATE_CLEAR_GLASS_BIT    = 1u << 12u;

// =====================================================
// 2026-10-22 MIS-A：GI 射线"不回传太阳日盘"标记（bit 14）
//
// 病灶（重复计入）：太阳直射已由每个着色点的 NEE 估计（主命中 A1、GI 顶点 A3-2
// 的 evaluateDirectSunLighting，都是沿 sunDir 打 1~2 条软阴影射线，delta 近似）；
// 而 rmiss 里另画了一个**有限角**日盘（rt_test.rmiss:942-1003，辐亮度 20，尾部被
// clamp 到 2.5，GI 未命中时按 albedo*0.6 全额计入，本文件 22008-22011）。
// 于是同一条 "着色点 → 太阳" 的连接被两个估计量各算一次：
//   · 期望值：日盘立体角 ≈ 6.8e-5 sr ⇒ 净增量 ~1e-4，肉眼不可见；
//   · 方差：cosine 采样命中日盘的概率 ~2e-4，一旦命中就贡献 ~1.5（典型漫反射
//     辐亮度 O(0.1)）⇒ 这就是"孤立、夹不掉的亮点"里属于太阳的那一份。
//
// delta 光源的 MIS 正解就是 BSDF 命中权重 = 0（vs NEE 的 p_light 是 delta 测度）。
// 只压**日盘**，不压 pow64 日冕瓣（±8°，非 delta，NEE 覆盖不到，压了会丢能量）。
//
// 作用域：只标记 GI 回路里发出的射线（21986-21990）。主射线（purpose 7）与
// glossy 反射（purpose 0，但不在回路里）不受影响 —— 镜面里的太阳必须保留。
// A/B 回退：把 GI_SKIP_SUN_DISK 改成 0（或编译加 -DGI_SKIP_SUN_DISK=0）。
// 两处必须同值：本文件与 rt_test.rmiss（rmiss 是独立 TU，宏不跨文件传递）。
// =====================================================
#ifndef GI_SKIP_SUN_DISK
#define GI_SKIP_SUN_DISK 1
#endif
const uint RT_STATE_NO_SUN_DISK_BIT    = 1u << 14u;

// =====================================================
// 2026-10-22 水面镜面反射的太阳遮挡（bit 14 复用 + bit 15）
//
// 病灶：水面点自身被墙/屋檐挡住时，GGX 高光与 SSS 都已沿 sunDir 门控，
// 但水面镜面反射走 tracePrimaryTransparentRadiance 的独立 walker，它只在
// 反射线撞到几何时才有遮挡。日冕瓣 sunGlow = pow(sunDot, 64)（±8°）可以
// 从遮挡物边缘上方漏进 miss ⇒ 阴影里的水面出现太阳色白斑。
//
// 做法：在水面点先做一次太阳透射（与 GGX 高光共用同一次 trace），透射为 0
// 时给反射射线同时置"无日盘"（bit 14）与"无日冕"（bit 15）。反射线仍正常
// 走墙/天空/玻璃，只去掉太阳点源项；日盘/日冕的合成、以及天空 LUT 里烘进去的
// 米氏日晕（占比在 sky_atmosphere.comp 写在 LUT 的 alpha）都在 rt_test.rmiss。
//
// 只作用于相机可见水面的**反射**射线（tracePrimaryTransparentRadiance 的
// 水面反射调用点）。折射/玻璃/主射线不置位 ⇒ 直射阳光下的镜面太阳逐位保留。
// A/B 回退：WATER_SKY_SUN_OCCLUSION=0（两处必须同值：本文件与 rt_test.rmiss）。
// =====================================================
#ifndef WATER_SKY_SUN_OCCLUSION
#define WATER_SKY_SUN_OCCLUSION 1
#endif
const uint RT_STATE_NO_SUN_GLOW_BIT    = 1u << 15u;
// 2026-10-24：粒子命中标记，bit 16 —— 置位者 material_resolve.glsl
//（isParticleMaterial，makeHitState 之后），消费者 rrParticlePixel（主射线读取，
// 喂 RR TransparencyHint 治粒子残影）。与 rt_test.rchit 同名同值。
const uint RT_STATE_PARTICLE_BIT       = 1u << 16u;
// 2026-11-26：第一人称手部命中标记，bit 20 —— 用于在发射阴影/GI/局部光/光泽次级光线时
// 屏蔽 0x02（LOCAL_PLAYER 代理头壳），避免近距缩小手部陷入头壳内部导致全黑。
const uint RT_STATE_HAND_BIT           = 1u << 20u;

// =====================================================
// 2026-10-25 A/B：GI 射线跳过 POM（bit 17）
//
// 病灶：rt_test.rchit 的 POM march 固定 48 步 + 自阴影最多 24 步，且**不区分射线
// 用途**；GI 射线方向四周散射、图集地址完全不相干 ⇒ 每条 GI 命中可达 25ns，
// 严重场景下 GI 段 11ms。
// 置位者：下面 GI 回路（与 traceTransparentLowCostGI）播种 state 时。消费端只有
// rt_test.rchit 的 getPOMDepthScale()（返回 0 ⇒ 走既有的"POM off"早退分支）。
// 为什么几乎无损：POM 位移是厘米量级，只对主射线近处掠射有意义；GI 是二阶弹射，
// 结果还要落回 1/4 甚至 1/9 分辨率再被 RR 重建，厘米位移不可见。
// 回退：宿主 ORION_GI_NO_POM=0（CameraUBO.giHalfParams.z 写 0）。
// =====================================================
const uint RT_STATE_GI_NO_POM_BIT      = 1u << 17u;
const uint RT_STATE_FOLIAGE_BIT        = 1u << 18u;

// 2026-11-09：次级射线（反射 / 水体 / 玻璃后）跳过 POM 的宿主开关。
// 复用同一个 state 位与同一条早退路径（rchit 播种 → getPOMDepthScale() 返 0），
// 所以只是"多一类射线置这个位"，命中侧一行都不用改。
// 宿主：ORION_POM_SECONDARY（出厂开）→ CameraUBO.pomSecondaryParams.x。
bool secondaryRayPomSkipEnabled()
{
    return cam.pomSecondaryParams.x > 0.5f;
}

const uint RT_PURPOSE_FULL             = 0u;
const uint RT_PURPOSE_CLASSIFY         = 1u;
const uint RT_PURPOSE_DISTANCE_ONLY    = 2u;
const uint RT_PURPOSE_VISIBILITY_ONLY  = 3u;
const uint RT_PURPOSE_PASSTHROUGH      = 4u;
const uint RT_PURPOSE_GLASS_VISIBILITY = 5u;
const uint RT_PURPOSE_WATER_MEDIUM     = 6u;
// Primary sky misses use the 16-step volume; secondary misses use the
// reference low-quality five-step branch without changing their ray type.
const uint RT_PURPOSE_PRIMARY_SKY      = 7u;
// 2026-10-25 阶段 D：主射线的透射段（单条射线 + any-hit 逐面累乘，Remix PSR 同构）。
const uint RT_PURPOSE_PRIMARY_TRANSMISSION = 8u;

// 2026-10-28 修复：主透射链改用**独立 state 位**（bit 18）。
//
// 病灶（代码事实，不是推测）：RT_PURPOSE_ 只有 3 位（RT_STATE_PURPOSE_MASK =
// 0x7u，0..7 全满），而 RT_PURPOSE_PRIMARY_TRANSMISSION = 8 ⇒
// makeTraceState(8) == (8 & 7) == 0 == RT_PURPOSE_FULL。于是两个 any-hit 里的
//     getRayPurpose(payload.state) == RT_PURPOSE_PRIMARY_TRANSMISSION
// 永远为假：2026-10-25 加入的整条玻璃透射链（逐面累乘 / alpha blend 写入）
// 从未执行过。玻璃面因此在链里被当成不透明遮挡接受，链终止在玻璃的出射面，
// rgen 拿到的 behindRadiance 退化成 albedo × 环境项 —— 表现为"透过玻璃看到
// 灰色实心面"：视线恰好多穿过一层玻璃就实，没穿过就透。
//
// 位预算：15 NO_SUN_GLOW / 16 PARTICLE / 17 GI_NO_POM 已用，18 空闲。
// 回退：-DORION_GLASS_CHAIN_BIT=0（逐位回到旧行为）。
#ifndef ORION_GLASS_CHAIN_BIT
#define ORION_GLASS_CHAIN_BIT 1
#endif
const uint RT_STATE_PRIMARY_TRANSMISSION_BIT = 1u << 18u;
// 2026-11-24：漫反射 GI 射线跳过 rmiss 的 24 步 Tyndall 3D 云行军（bit 19）。
// 水面/玻璃/glossy 镜面反射保留完整云层，仅半余弦积分后的漫反射 GI 略过，
// 避免与 bit 14（阴影水面反射也置位，仍需云）混淆。
const uint RT_STATE_GI_SKIP_CLOUDS_BIT       = 1u << 19u;
// 2026-11-26: K1-Primary landing-identity ray marker (bit 21).
// Tells rt_test_translucent.rchit to restore (slot, prim, baryX, baryY)
// whenever an ALPHA-bucket hit resolves to non-translucent (e.g. clear glass
// VPT_GLASS_CUTOUT_BORDER -> RT_HIT_OPAQUE, Voxy opaque, DH water) so
// K1-Primary's ptLandingStore writes a valid identity for K2.
const uint RT_STATE_K1_LANDING_BIT           = 1u << 21u;

const uint RT_HIT_OPAQUE               = 0u;
const uint RT_HIT_WATER                = 1u;
const uint RT_HIT_TRANSLUCENT          = 2u;
const uint RT_HIT_MISS                 = 3u;
const uint RT_WATER_NONE               = 0u;
const uint RT_WATER_AIR_TO_WATER       = 1u;
const uint RT_WATER_WATER_TO_AIR       = 2u;

uint makeTraceState(uint purpose)
{
    return purpose & RT_STATE_PURPOSE_MASK;
}

// Hardware visibility for the local-light NEE / local direct estimator.
// Mirrors rt_restir_final.rgen's traceLocalLightVisibility() 1:1 and reuses the
// contract the sun / AO / rain queries already run -- no private hit group and,
// deliberately, no SER arm (reordering the visibility group was measured
// harmful: +1.99ms on rt, see the SER notes).  The Any-Hit sheet model is what
// makes crossed stained panes FILTER the ray (unpackAlbedoUnorm(payload.albedoPacked)) instead of
// stopping it, so the local-light pool behind one pane uses the same model as
// the sunlit pool behind it.
vec3 traceVptLocalLightVisibility(
    vec3 origin,
    vec3 target,
    float targetVoxelEntryDistance,
    uint cullMask
)
{
    // 2026-10-08：stage 6（"noEmitterScan"）以前是个**空臂**。perfL10DisableLocalEmitterHitScan()
    // 在这份文件里没有调用点，而 stage 5/7 的 perfL10DisableG2bLocalGuide() 又被
    // LL_P3_ENABLE_G2B_LOCAL_GUIDE = false 挡在编译期就折叠掉的分支里，所以 5/6/7 三级实测
    // ≈0 并不是"这些功能免费"，而是**什么都没关**。这里把 stage 6 接到本地光可见性射线上，
    // 让 Tier-1「硬件可见性射线换 inline RayQuery / 直接砍掉」第一次有价格可读。
    // 返回 vec3(1.0) = 当作完全无遮挡：只去掉射线本身，NEE 其余部分照常跑，
    // 所以 dRt 就是这条射线的净成本（观感会穿墙，仅用于诊断）。
    if(perfL10DisableLocalEmitterHitScan())
    {
        return vec3(1.0);
    }

    vec3 segment = target - origin;
    float distanceToTarget = length(segment);
    if(distanceToTarget <= 0.03)
    {
        return vec3(1.0);
    }

    // 0.03 short of the emitter's own voxel entry, so the emitter mesh cannot
    // shadow its own sample.  The callers only hand in that entry distance when
    // the sampled point really is inside the emitter's voxel; otherwise they
    // pass the full distance, because stopping early there would skip the very
    // blocker that sits between the receiver and the sample.
    float rayMaxDistance = min(
        distanceToTarget - 0.03,
        targetVoxelEntryDistance - 0.03
    );
    if(rayMaxDistance <= 0.02)
    {
        return vec3(1.0);
    }

    // [12] 本地光可见性射线（traceVptLocalLightVisibility 的真实发射点）。
    perfRayStatAdd(12u, 1u);

    #ifndef ORION_K2I_COMPOSE_COMPUTE
#ifndef ORION_OPAQUE_VIS_FASTPATH
#define ORION_OPAQUE_VIS_FASTPATH 1
#endif
#if ORION_OPAQUE_VIS_FASTPATH
    {
        rayQueryEXT localVisQuery;
        rayQueryInitializeEXT(
            localVisQuery,
            topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT,
            cullMask,
            origin,
            0.02,
            segment / distanceToTarget,
            rayMaxDistance
        );
        if(!rayQueryProceedEXT(localVisQuery))
        {
            #ifdef ORION_RT_STAGE_COUNTERS
            rtCntRayIssued();
            #endif
            return (rayQueryGetIntersectionTypeEXT(localVisQuery, true) !=
                    gl_RayQueryCommittedIntersectionNoneEXT)
                ? vec3(0.0)
                : vec3(1.0);
        }
        rayQueryTerminateEXT(localVisQuery);
    }
#endif

    hit.hitDistance = 1.0;
    hit.albedoPacked = packAlbedoUnorm(vec3(1.0));
    hit.parallaxRayTravel = 0.0;
    hit.glassAlpha = (0.0);
    hit.state = makeTraceState(RT_PURPOSE_GLASS_VISIBILITY);

    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntRayIssued();
    #endif
    traceRayEXT(
        topLevelAS,
        gl_RayFlagsTerminateOnFirstHitEXT |
            gl_RayFlagsSkipClosestHitShaderEXT,
        cullMask,
        0,
        0,
        0,
        origin,
        0.02,
        segment / distanceToTarget,
        rayMaxDistance,
        0
    );

    if(hit.hitDistance >= 0.0)
    {
        return vec3(0.0);
    }

    return clamp(unpackAlbedoUnorm(hit.albedoPacked), vec3(0.0), vec3(1.0));
    #else
    return vec3(1.0);
    #endif
}

uint getRayPurpose(uint state)
{
    return state & RT_STATE_PURPOSE_MASK;
}

uint getHitClass(uint state)
{
    return (state >> RT_STATE_HIT_CLASS_SHIFT) & 0x3u;
}

uint getWaterEvent(uint state)
{
    return (state >> RT_STATE_WATER_EVENT_SHIFT) & 0x3u;
}

uint getTransparentProfile(uint state)
{
    return (state >> RT_STATE_PROFILE_SHIFT) & 0x7u;
}

bool isGlassHit(uint state)
{
    return (state & RT_STATE_GLASS_BIT) != 0u;
}

bool isGlassBottomHit(uint state)
{
    return (state & RT_STATE_GLASS_BOTTOM_BIT) != 0u;
}

// =====================================================
// 2026-10-07（用户要求）：普通玻璃板/玻璃块「当空气」
// =====================================================
// 退出 PBR 反射与折射：不做薄片染色、不打那条 F0=0.04 的确定性镜面射线、不加太阳
// GGX 高光，光直接穿过去。走查本身用 PASSTHROUGH（忽略一切玻璃），所以"背后辐射"
// 本来就是穿过去的景象 —— 这里只是把它原样返回，不叠加任何玻璃自己的贡献。
//
// 为什么需要新状态位：native 把 Java marker 2（普通玻璃）与 3..18（染色玻璃）都写成
// materialId=2.0，染料只体现在**顶点 RGB** 上 ⇒ shader 侧本来区分不出两者。判据由
// rchit 在命中时算好并置位（那两个 rchit 的 t12IsGlass 分支，用
// glassVertexFactorIsFaceShade）。
//
// 2026-11-13（用户要求）：默认置 0 —— 清玻璃**不再**"当空气"，与染色玻璃等
// 其它半透明体一样走完整电介质（菲涅耳反射 + 折射走查）。消费者见
// tracePrimaryGenericTransparentRadiance / GI_CLEAR_GLASS_GI_AS_AIR /
// glassWalkExitPosition / GI_GLASS_WALK / nested dielectrics 闸门，全部经
// isClearGlassHit() 汇总到这一个开关。
// 回退：把默认改回 1（或 -DGI_CLEAR_GLASS_AS_AIR=1）即逐位恢复"当空气"档。
#ifndef GI_CLEAR_GLASS_AS_AIR
#define GI_CLEAR_GLASS_AS_AIR 0
#endif
bool isClearGlassHit(uint state)
{
#if GI_CLEAR_GLASS_AS_AIR
    return (state & RT_STATE_CLEAR_GLASS_BIT) != 0u;
#else
    return false;
#endif
}

// =====================================================
// 2026-10-13（用户要求）：嵌套介质优先级 nested dielectrics
//
// 参考（逐字，NVIDIA Falcor，BSD-3-Clause，Source/Falcor/Scene/Material/
// MaterialData.slang）：
//     static constexpr uint kNestedPriorityBits = 4;
//     /** Set the nested priority used for nested dielectrics. */
//     SETTER_DECL void setNestedPriority(uint32_t priority)
//     /** Get the nested priority used for nested dielectrics. \return Nested
//         priority, with 0 reserved for the highest possible priority. */
//     uint getNestedPriority() CONST_FUNCTION
//
// 本工程**照抄它的字段语义**（位宽 4、0 = 最高优先级）。**转移算法是本工程自创** ——
// Falcor 的进出判定原文没有取到，所以这里不声称与它同构，只声称优先级序与它一致。
//
// 要解决的问题（Minecraft 特有）：两个半透明体相邻时，共享的那一面由两份几何各渲染
// 一次（冰块的底面与水块的顶面落在同一位置）。GI 走查会把**两个界面**各处理一遍
// ⇒ 一条真实的介质边界吃两遍菲涅耳 / 两遍染色。优先级规则让"更强的那一体"拥有这块
// 空间，较弱的那个界面直接跳过（不染色、不打镜面、不折向，原方向穿过去）。
//
// 优先级表（0 = 最高；数值越大越弱，越容易让位）：
//     1  染色玻璃块 / 染色玻璃板  (transparent profile 0)
//     2  遮光玻璃                (1)
//     3  冰族 ice/packed/blue/frosted (2)
//     4  粘液块                  (3)
//     5  蜂蜜块                  (4)
//    14  水                      （最弱的一体：永远给固体让位）
//    15  空气 / 无介质           （哨兵）
//
// 2026-11-13（GI_CLEAR_GLASS_AS_AIR 默认置 0）起清玻璃块 / 板**参与**本规则：
// 它的 transparent profile 也是 0 ⇒ 优先级 1，与染色玻璃同档。
//
// ⚠ 已知局限（刻意接受）：Minecraft 的方块互不重叠，所以介质态退化为**单个**变量，
//   不做栈。真正的嵌套（玻璃盒里的水、水里再放冰）在共面处仍由"更强的一体胜出"
//   近似，不保证逐层还原。
//
// 开关：-DGI_NESTED_DIELECTRIC=0 逐位回到旧档。
// =====================================================
#ifndef GI_NESTED_DIELECTRIC
#define GI_NESTED_DIELECTRIC 1
#endif

const uint RT_NESTED_PRIORITY_AIR   = 15u;
const uint RT_NESTED_PRIORITY_WATER = 14u;

// 命中界面的优先级。2026-11-13 前调用点需先排除清玻璃（isClearGlassHit）；
// 清玻璃改走电介质后（GI_CLEAR_GLASS_AS_AIR=0）会正常走到这里（profile 0）。
uint nestedPriorityOfHit(uint state)
{
    if(getHitClass(state) == RT_HIT_WATER)
    {
        return RT_NESTED_PRIORITY_WATER;
    }

    // profile 0 = 染色玻璃块/板与清玻璃；1..4 = 遮光玻璃 / 冰 / 粘液 / 蜂蜜。
    return 1u + getTransparentProfile(state);
}

// 这个界面要不要处理。false = 它被更强的一体拥有，走查应当原方向穿过去。
// 进出判定：
//   同优先级        -> 离开当前介质（回到空气）
//   当前在空气      -> 进入这个介质
//   当前介质更强    -> 跳过（我们不拥有这块空间）
//   当前介质更弱    -> 从较弱的一体撞进更强的一体 => 进入
bool nestedInterfaceAccepted(uint interfacePriority, inout uint currentPriority)
{
    if(interfacePriority == currentPriority)
    {
        currentPriority = RT_NESTED_PRIORITY_AIR;
        return true;
    }

    if(currentPriority == RT_NESTED_PRIORITY_AIR)
    {
        currentPriority = interfacePriority;
        return true;
    }

    if(interfacePriority > currentPriority)
    {
        return false;
    }

    currentPriority = interfacePriority;
    return true;
}

// 当前是不是在一个**固体**半透明体内部（用于水界面的共面跳过）。
// 水自己（14）不算，空气（15）不算。
bool nestedInsideSolidMedium(uint currentPriority)
{
    return
        currentPriority != RT_NESTED_PRIORITY_AIR &&
        currentPriority != RT_NESTED_PRIORITY_WATER;
}