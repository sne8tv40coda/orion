
#if !GI_HALFRES
// ============================================================================
// 3×3 深度感知上采样（HANDOFF-2026-10-24-GI-HALFRES.md §3）
//
// giHalfImage 的每个 texel 覆盖 2×2 个全分辨率像素，四帧轮转才把四个子像素各算一次
// ⇒ 全分辨率像素拿到的往往是**同块内相邻子像素、最多 3 帧前**的辐照度。所以：
//   · 权重 w = 1/(1e-3 + |z0 - z1|)：深度一致才接近双线性展开，跨物体边缘退化成
//     最近邻（HANDOFF §6.4：宁可退化也不要跨玻璃/水面边缘漏光）；
//   · 返回的是**辐照度本身**（不解调 —— 原因见半图写入处的 2026-10-25 注释）。
// 不做材质一致性判据：这张图只有 4 个通道，matId 无处可放；玻璃/水面与实体在
// **深度**上就是两道不同的面，深度权重已经把它们挡住。
// ============================================================================
vec3 upsampleGiHalf(ivec2 fullCoord, float centerViewDepth)
{
    const ivec2 halfSize = imageSize(giHalfImage);
    // 半图 texel 与全分辨率像素的映射：GI 变体写的是 texel = launchID，
    // 对应全分辨率像素 launchID * 2 + phase ⇒ 反查就是 fullCoord / 2（整数截断）。
    const ivec2 baseCoord = ivec2(fullCoord.x / 2, fullCoord.y / 2);
    const ivec2 maxCoord = max(halfSize - ivec2(1), ivec2(0));

    vec3 irradianceSum = vec3(0.0);
    float weightSum = 0.0;

    for(int tapY = -1; tapY <= 1; ++tapY)
    {
        for(int tapX = -1; tapX <= 1; ++tapX)
        {
            const ivec2 tapCoord = clamp(
                baseCoord + ivec2(tapX, tapY),
                ivec2(0),
                maxCoord);

            const vec4 tapGi = imageLoad(giHalfImage, tapCoord);

            // 相对深度差：远端掠射面上相邻 texel 的**绝对**深度差可以有几十米，
            // 用绝对差会让权重量纲随距离漂移，所以一律相对化。
            const float relativeDepthDelta =
                abs(centerViewDepth - tapGi.a) / max(centerViewDepth, 1.0);

            // ⚠ 2026-10-25 实机第二轮：原来是 1/(1e-3 + 绝对差)。
            //   "相位刚好轮到自己"的那 1/4 像素，Δz 精确为 0 ⇒ 权重 1000，而 8 个
            //   邻居只有 0.2~5 ⇒ 求和实际上只剩那一格，9 个 tap **根本没平均**，
            //   单个 1spp 的 GI 萤火虫原样上屏；又因为相位 4 帧一轮，同一像素在
            //   "自己的噪点样本"与"邻居的平滑值"之间反复跳变，RR 的时间累积把这
            //   当成真实变化 ⇒ 满屏闪烁小方点（对照实验：同位置 ORION_GI_HALF=0
            //   时地板干净，=1 时满是小点）。
            //   现在用相对差 + 0.05 的底：平面邻格落在 4~20，与中心格的 20 同量级
            //   ⇒ 真正做 9 tap 平均（方差 ÷3）；跨面时相对差 ~0.5 以上，权重降到
            //   1.8 以下，仍有 ~10:1 以上的拒绝比，漏光不会回来。
            const float weight =
                1.0 / (0.05 + relativeDepthDelta);

            irradianceSum += tapGi.rgb * weight;
            weightSum += weight;
        }
    }

    if(weightSum <= 0.0)
    {
        return vec3(0.0);
    }

    return irradianceSum / weightSum;
}
#endif

void main() {
    ivec2 size = imageSize(image);

#if (defined(ORION_K2_INDIRECT_STAGE) || defined(ORION_K2_DIRECT_LOCAL_STAGE) || defined(ORION_K2_DIRECT_SUN_STAGE_B) || defined(ORION_K1_GLASS_ONLY) || SHARC_UPDATE) && !defined(ORION_K2_INDIRECT_COMPOSE_ONLY)
    const bool k2iCompactTraceActive = (k2iSparseCompact.headers[K2_COMPACT_SLOT].mode != 0u);
    ivec2 pixelCoord;
    if(k2iCompactTraceActive)
    {
        const uint compactIdx = gl_LaunchIDEXT.y * gl_LaunchSizeEXT.x + gl_LaunchIDEXT.x;
        if(compactIdx >= k2iSparseCompact.headers[K2_COMPACT_SLOT].activeCount)
        {
            return;
        }
        const uint packedCoord = k2iSparseCompact.activeCoords[
            k2iSparseCompact.headers[K2_COMPACT_SLOT].baseElemOffset + compactIdx
        ];
        pixelCoord = ivec2(int(packedCoord & 0xFFFFu), int(packedCoord >> 16u));
        if(pixelCoord.x >= size.x || pixelCoord.y >= size.y)
        {
            return;
        }
    }
    else
    {
#if SHARC_UPDATE
        const ivec2 sharcBlock = ivec2(gl_LaunchIDEXT.xy);
        const uint sharcBlockSize = sharcUpdateBlockSize();
        pixelCoord =
            sharcBlock * int(sharcBlockSize) +
            sharcUpdateBlockOffset(sharcBlock, sharcBlockSize);
        if(pixelCoord.x >= size.x || pixelCoord.y >= size.y)
        {
            return;
        }
#else
        if(gl_LaunchIDEXT.x >= size.x || gl_LaunchIDEXT.y >= size.y)
        {
            return;
        }
        pixelCoord = ivec2(gl_LaunchIDEXT.xy);
#ifdef ORION_K1_GLASS_ONLY
        {
            const float glassMarker = ptLandingRecords[
                ptLandingRecordIndex(
                    uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x), 0
                )
            ].a.x;
            if(glassMarker <= 0.1 || glassMarker >= 0.5)
            {
                return;
            }
        }
#endif
#endif
    }
#else
    const bool k2iCompactTraceActive = false;
    if (gl_LaunchIDEXT.x >= size.x ||
        gl_LaunchIDEXT.y >= size.y) {
        return;
    }
#endif

    // 2026-10-11 5a：SHaRC 寻址探针（mode == 1 时由宿主打开，见 sharcProbeWriteBack）。
    // 放在所有渲染之前：探针只做缓存读写，与像素是否被遮挡无关，越早越省事。
    if (!k2iCompactTraceActive && sharcMode() == 1u && gl_LaunchIDEXT.x == 0u && gl_LaunchIDEXT.y == 0u) {
        sharcProbeWriteBack();
    }

    // ===== DBG_REACH_MAIN: 整屏纯品红可达性探针（不可能认错） =====
    if(DBG_REACH_MAIN) {
        imageStore(image, ivec2(gl_LaunchIDEXT.xy), vec4(1.0, 0.0, 1.0, 1.0));
        return;
    }

    // =====================================================
    // 2026-10-28 **写入通路探针**（标记文件 glass_write_probe.flag ⇒ lane 7）
    //
    // 起因：玻璃的所有诊断档（dbg 1/9/10/11/12/13）在实机上**零效果**，
    // 而 dbg=1 显示整屏洋红 = blend 读到的 74 号图 y 通道全 0 = 没有任何像素被写。
    // 这一档绕开全部玻璃逻辑，无条件把 74 号图写成 (bgT=1, hitT=1)。
    //
    // 判读（配 ORION_GLASS_ALPHA_BLEND_DBG / glass_dbg_1.flag）：
    //   整屏变成纯红 (1,0,0) ⇒ 写入通路正常
    //        ⇒ 洋红的原因就是"玻璃分支一个像素都没进"，与描述符无关；
    //   仍整屏洋红        ⇒ rgen 写的不是 blend 读的那张图（描述符/镜像问题）。
    //
    // 只在主变体上有效（GI_HALFRES 变体的 LaunchID 是半分辨率坐标）。
    // =====================================================
#if !SHARC_UPDATE
    if(!k2iCompactTraceActive && cam.perfAblationStage.w == 7u)
    {
        imageStore(
            glassAlphaBlendMetaImage,
            ivec2(gl_LaunchIDEXT.xy),
            vec4(1.0, 1.0, 0.0, 0.0));
    }
#endif


#if !((defined(ORION_K2_INDIRECT_STAGE) || defined(ORION_K2_DIRECT_LOCAL_STAGE) || defined(ORION_K2_DIRECT_SUN_STAGE_B) || defined(ORION_K1_GLASS_ONLY) || SHARC_UPDATE) && !defined(ORION_K2_INDIRECT_COMPOSE_ONLY))
#if GI_HALFRES
    // 2026-10-24 半分辨率 GI 变体：dispatch 是 (w+1)/2 × (h+1)/2，每个 invocation
    // 负责 2×2 块里的**一个**子像素；相位 (frameCount&1, frameCount>>1&1) 逐帧轮转，
    // 四帧覆盖整个 2×2。
    // ⚠ 这是本文件 pixelCoord 的**唯一来源**（HANDOFF §4 已核实全文件只有这一处派生），
    //   所以只改这里就能把整条主射线 + 材质求值搬到一个子像素上。
    // ⚠ 2026-10-25 实机第二轮：**默认不再轮转相位**。
    //   dispatch 覆盖全部半图 texel ⇒ 每个 texel 每帧都被写，于是"四帧轮转四个
    //   子像素"并不成立；真实效果是每个 texel 代表的**世界位置每帧平移一个像素**，
    //   信号在 -1/0/+1 像素之间跳，RR 把它当真实变化 ⇒ 时间累积失效 ⇒ 闪烁。
    //   固定 (0,0) 之后 texel (i,j) 永远代表像素 (2i,2j)，半图稳定，主 pass 用
    //   3×3 双边权重上采样（标准 half-res + bilateral upsample）。
    //   代价：3/4 的像素自己从不贡献 GI（读到的是 2×2 块的左上邻居）= 固定 2×2 模糊。
    //   回到轮转：ORION_GI_HALF_ROTATE=1（宿主把 giHalfParams.y 写 1）。
    const ivec2 giHalfPhase =
        (cam.giHalfParams.y > 0.5f)
            ? ivec2(
                int(cam.frameCount & 1u),
                int((cam.frameCount >> 1u) & 1u))
            : ivec2(0);

    ivec2 pixelCoord =
        ivec2(gl_LaunchIDEXT.xy) * 2 + giHalfPhase;
#else
    ivec2 pixelCoord =
        ivec2(gl_LaunchIDEXT.xy);
#endif
#endif


    // 一帧唯一一个 Primary jitter。
    // 所有像素、4 个 sample 共享。
    vec2 frameJitter =
        getDLSSFrameJitter(
            cam.frameCount
        );


    // Camera UBO 当前保存的是 inverse matrices。
    // Depth 要恢复真实 view/projection 后计算 HW depth。
    // 2026-11-09：宿主每帧算好的正变换（CameraUBO.viewMatrix /
    // projectionMatrix）。原先这里是每像素两次 inverse(mat4)——
    // 矩阵是帧常量，逐像素求逆纯属浪费，还占 32 个寄存器。
    mat4 viewMatrix =
        cam.viewMatrix;

    mat4 projectionMatrix =
        cam.projectionMatrix;


    // =====================================================
    // 2026-10-06 用户「每像素采样数」(spp) 滑块 —— cam.rtSampleParams.x
    // （离散档 0.5 / 1 / 2 / 3 / 4，出厂 1）
    //
    //   >= 1：每像素每帧打 N 条主射线（像素内超采样），最后取平均。四条射线共用
    //         同一个帧级 Halton jitter（见下面的 frameJitter 注释），所以主命中点
    //         相同、被平均的是**随机次级项**（GI / 反射 / 局部光）—— 等效于把
    //         这些估计量的方差按 1/√N 压下来。
    //   == 0.5：棋盘交错 —— 每帧只渲染棋盘格的一半像素，另一半**一个字节都不写**。
    //         g_rtOutputImage 与 RR 的 guide 图（法线/粗糙度/反照率/深度/镜面距离）
    //         都不清空，于是未渲染的像素原样保留上一帧的值 ⇒ 每个像素隔帧采样一次。
    //         代价：未渲染像素的 guide 陈旧一帧（用户已知悉并接受）。
    //         注：g_nrdMotionImage 目前无生产者（NRD local DI 链关停），不受影响。
    //         棋盘按帧翻转（异或 frameCount 奇偶），所以相邻像素交错、时间上也交错。
    //
    // 回退：把 rtSampleParams.x 固定成 1.0 即逐位回到旧档（单采样、无闸门）。
    // =====================================================
    const float rtUserSpp =
        clamp(cam.rtSampleParams.x, 0.5, 4.0);

    const bool rtSampleThisPixel =
        rtUserSpp >= 1.0 ||
        (((uint(pixelCoord.x) + uint(pixelCoord.y)) & 1u) == (cam.frameCount & 1u));

    if(!rtSampleThisPixel)
    {
        // 本帧不负责这个像素：不写任何输出，上一帧的值原样留在各目标图里。
        return;
    }

#if !GI_HALFRES && !SHARC_UPDATE && !defined(ORION_K1_GLASS_ONLY) && !defined(ORION_K2_TRANSPORT)
    // 2026-11-21 P0-1：per-pixel 旁路槽清槽。样本循环**外**每像素无条件清一次
    //（vec4(0)，a=0 = 无有效偏移）；miss / 未被本帧重写的像素读回按 0 处理。
    // 不能每条射线清：多样本共享同一槽位，后写覆盖先写（蓝图 §3.6）。
    // 半分辨率 GI 变体不参与本信道：其 LaunchID 是半分辨率坐标，写全分辨率图会
    // 错位（蓝图 §3.6 / 风险 1）；读回侧同样被 #if !GI_HALFRES 守卫。
    if(!k2iCompactTraceActive)
    {
        imageStore(
            hitOffsetImage,
            pixelCoord,
            vec4(0.0, 0.0, 0.0, 0.0));
    }
#endif

    // [0] 被统计的像素数（所有比率的分母）
    perfRayStatAdd(0u, 1u);

    const int SAMPLES_PER_FRAME =
        max(1, int(rtUserSpp + 0.5));

#ifdef ORION_K2I_COMPOSE_COMPUTE
    if(SAMPLES_PER_FRAME == 1 &&
       ptLandingRecords[uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x)].a.x <= 0.5)
    {
        return;
    }
#endif

    vec3 accumulatedSampleColor =
        vec3(0.0);
#ifdef ORION_K2_INDIRECT_TRACE_ONLY
    vec3 k2iGiTraceAccum = vec3(0.0);
    bool k2iGiTraceAny = false;
#endif

    // "这个像素对我们是远场"的标记，供 main 末尾的 DH 合成块使用。
    // primaryHitDistance 的作用域只到采样循环体内，所以在循环外先开一个槽，
    // 由循环里按主射线是否 miss 置位。0 = 我们命中了（近场，别动），
    // 1 = 我们没命中（远场，可以让 DH 接管）。判据的完整理由见置位处。
    float dhFarFieldOurs = 0.0;

    // 2026-10-27 阶段 3：稀疏判定的两个量在采样循环内（vptPixelActive / 
    // primaryHitDistance 都是循环体局部量），而最终 imageStore 在循环外。
    // 照上面 dhFarFieldOurs 的同一做法开槽，循环内 s==0 记录。
    bool vptPixelActiveOutside = true;
    bool primaryRayMissed = false;

    // 主射线的最终命中距离（miss 保持负数）。合成块用它区分"我们真的命中了"
    // 与"我们没命中"——补空气那条分支必须只在前者之外生效。
    // 水下视角（applyUnderwaterWaterColumn）也取这个值当水柱长度，不再另开一个。
    float dhFarFieldOursDistance = -1.0;

    // 主射线方向。worldDir 只在采样循环体内可见，而水柱是在循环之后、最后一次
    // 写出之前才施加的（理由见那里的注释），所以在循环外留一个槽，循环里赋值。
    vec3 primaryViewDirectionForWater = vec3(0.0, 0.0, -1.0);

// =====================================================
// RR-6.2 - Dense guide values for this primary pixel
// =====================================================

vec3 rrDiffuseAlbedo =
    vec3(0.0);

vec3 rrSpecularAlbedo =
    vec3(0.0);

vec4 rrNormalRoughness =
    vec4(0.0);

float rrSpecularHitDistance =
    65504.0;

// Transparent paths replace the front-surface RR record only when their
// transmitted receiver is the dominant visible signal.
bool rrTransparentReceiverGuideValid =
    false;

vec3 rrTransparentReceiverWorldPos =
    vec3(0.0);

// True when the transparent receiver above belongs to a **glass pane**
// (as opposed to water).  Only the pane wants its own depth kept in the RR
// depth image -- see GI_GLASS_PANE_DEPTH_GUIDE.
bool rrTransparentReceiverIsPane =
    false;
bool rrGlassPstrActive =
    false;

// =====================================================
// 2026-10-01：(a) "主导路径是否穿过玻璃"标记 → RR guide 的空闲通道
//
// DLSS-RR 只使用 diffuse-albedo 纹理的 RGB（alpha 未用），所以把这条信息写进
// 做法，只在"本像素或邻居跨过 delta"时定向放宽 RR 的核半径）。
//
// 1 = 本像素内容来自"穿过玻璃"的路径（玻璃分支置位）。
// A/B：-DGI_RR_DELTA_FLAG=0 恢复 alpha=1（不携带标记）。
// =====================================================
#ifndef GI_RR_DELTA_FLAG
#define GI_RR_DELTA_FLAG 1
#endif

#ifndef ORION_GLASS_REMIX_PSTR_GUIDES
#define ORION_GLASS_REMIX_PSTR_GUIDES 1
#endif
#ifndef RR_GUIDE_PURIFY_V1
#define RR_GUIDE_PURIFY_V1 1
#endif

float rrDeltaCrossedFlag =
    0.0;

// =====================================================
// 2026-10-24：粒子像素标记 → RR TransparencyHint（治粒子残影）。
// 主射线命中粒子（payload.state 的 RT_STATE_PARTICLE_BIT，material_resolve 置位）
// 时置 1；hint 写入块把它并进 TransparencyHint ⇒ RR 对粒子像素降低历史信任。
// 粒子会移动/生灭，其像素的历史按主表面语义积累必然拖尾 —— 官方现行方案是
// TransparencyLayer + ColorBeforeTransparency（大工程，跟进项见交接文档），
// 本标记是旧机制 kBufferTypeTransparencyHint 的最小接入。
// =====================================================
bool rrParticlePixel =
    false;
bool primaryHandPixel =
    false;

#if RR_STABLE_GLASS_RADIANCE
// Analytic pane radiance for this pixel (mirror lobe + solar gloss), handed to
// the display transform after DLSS RR instead of to the denoiser.  Zero for
// every pixel that is not seen through a pane.
vec3 rrStableGlassRadiance =
    vec3(0.0);
#endif

    vec3 sunDir =
        getTimeDrivenSkyLightDirection();


    // 2026-10-09 运动矢量：本像素写进 MV 图的值，**提到循环外**声明，好让循环结束后的
    // 最终 imageStore 也能读到（调试图要用，见 DBG_SHOW_MV）。非调试时它只多存 2 个
    // float，没有其它代价。
    vec2 rtDebugPrimaryMotion =
        vec2(0.0);

#ifdef ORION_K1_PRIMARY
    // K1 在不透明切点写过 landing 的像素，输出尾部（水柱/稳定玻璃/最终写出）
    // 整体交给 K2 —— K1 自己的 currentColor 还没有该像素的辐射，绝不能写。
    bool anyLandingStored = false;
#endif
#ifdef ORION_K2_TRANSPORT
    // K2：本像素至少一个样本成功载入 landing（= opaque）。非 opaque 像素的
    // 全部输出（含 DH/水柱/guide/最终色）都由 K1 完成，K2 尾部整体跳过。
    bool ptLandingActive = false;
#endif


    for (int s = 0;
         s < SAMPLES_PER_FRAME;
         ++s) {
// 2026-10-24：同上，用 pixelCoord 而不是 launchID —— 半分辨率变体里同一个
// launchID 每四帧才轮到一次，用 launchID 播种会让四个子像素共用同一串随机数。
seed =
    uint(pixelCoord.y * int(size.x) + pixelCoord.x) +
    cam.frameCount * 97253u +
    uint(s * 19937u);

pcg_hash(seed);


// Primary camera sampling 不再消耗 PCG。
// 4 个 sample 使用相同的帧级 Halton jitter。
// 2026-10-24：像素中心改用 pixelCoord，**不再直读 gl_LaunchIDEXT**。
// 主变体里 pixelCoord == ivec2(gl_LaunchIDEXT.xy)，所以这一句是逐位等价的重写；
// 半分辨率 GI 变体里 pixelCoord = launchID*2+phase ⇒ 这条射线才是本 invocation
// 真正负责的那个全分辨率像素的射线（否则 GI 会把左上角四分之一的几何写进半图）。
// 分母同理换成 imageSize(image)（全分辨率）：GI 变体的 gl_LaunchSizeEXT 是半分辨率，
// 用它算 UV 会把整个视锥缩小一半。
vec2 pixelCenter =
    vec2(pixelCoord) +
    vec2(0.5) +
    frameJitter;


vec2 inUV =
    pixelCenter /
    vec2(size);

        vec2 d = inUV * 2.0 - 1.0;
        d.y = -d.y;

        vec4 origin = cam.viewInverse * vec4(0.0, 0.0, 0.0, 1.0);
        vec4 target = cam.projInverse * vec4(d.x, d.y, 1.0, 1.0);

        // Infinite-far perspective matrices map the far-plane clip point to
        // w == 0. In that case target.xyz is already the homogeneous ray
        // direction; dividing by w would inject NaNs into traceRayEXT.
        vec3 viewTarget = abs(target.w) > 1.0e-6
            ? target.xyz / target.w
            : target.xyz;
        float viewTargetLengthSquared = dot(viewTarget, viewTarget);
        vec3 rayDir = viewTargetLengthSquared > 1.0e-12
            ? viewTarget * inversesqrt(viewTargetLengthSquared)
            : vec3(0.0, 0.0, -1.0);
        vec4 worldDir = cam.viewInverse * vec4(rayDir, 0.0);

        // ==========================================
        // 步骤 1: Primary Ray (主光线发射)
        // ==========================================
        //
        // 2026-10-05：主相机射线带 CullBackFacingTriangles。OMM 桶（cutout：
        // 草丛/花/树叶）的 TLAS 实例已改为可剔除（见 VulkanCore.cpp 的
        // instance.flags），原版 cutout 层本来就是单面 authored —— cross 草丛
        // 正反两份共面 quad 靠背面剔除各显示一面。带剔除后：
        //   • 命中永远是朝向相机的正面副本（其 UV 就是原版该侧应显示的朝向）；
        //   • 透过正面副本镂空看到的是后面的世界，不再是背面副本的镜像纹素。
        // 其余射线不带此标志 ⇒ OMM 实例对它们仍双面，行为不变。
        // 不透明/水体/玻璃/实体/粒子实例保留 FACING_CULL_DISABLE，不受影响。

        // 2026-10-21「静止时玻璃上慢慢积累彩色噪点」修复：
        // Any-Hit 用 `unpackAlbedoUnorm(payload.albedoPacked) *= 透射色` 累积玻璃染色（见 rt_test.rahit 的
        // "T1/T9/T3 colored visibility through glass" 段），这个累乘的初值**必须是
        // 1.0**。主射线此前只播种 state ⇒ 累乘起点是**上一条射线**留在 payload 里的
        // albedo（反射 / AO / GI 的命中色），而每个像素每帧跑的射线条数还随 RR 的
        // 采样变化 ⇒ 玻璃透射色逐帧不同；静止时 DLSS-RR 把它当信号累积，于是
        // 无色玻璃出白色噪点、染色玻璃出彩色噪点，相机一移动历史被拒就干净。
        // 玻璃可见性射线早就在 6410 这样播种，主射线是漏掉的那一处。
// =====================================================
// B：K1/K2 拆分 —— 前半（主射线 + 捕获 + depth/MV）。
//
// K2 变体不重跑前半：主射线与它的全部派生量从 landing 载入重建（见
// rgen_k1_landing.glsl）。payload 快照 = 切点前的完整主命中状态。
// 非 opaque 像素（miss/水/玻璃）K1 已独立完成全部输出，K2 直接跳样本。
// =====================================================
#if defined(ORION_K1_PRIMARY) && !defined(ORION_K1_GLASS_ONLY)
    // 每样本先盖哨兵：本样本若不走 opaque 切点，这条记录保持 valid=0，
    // K2 就不会读到上一帧的陈旧记录。
    ptLandingInvalidate(
        ptLandingRecordIndex(
            uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
            s
        )
    );
#endif

#ifndef ORION_K2_TRANSPORT
        hit.albedoPacked = packAlbedoUnorm(vec3(1.0));
#if defined(ORION_K1_PRIMARY) && !defined(ORION_K1_GLASS_ONLY)
hit.state = makeTraceState(RT_PURPOSE_PRIMARY_SKY) | RT_STATE_K1_LANDING_BIT;
const uint k1PrimarySbtOffset = 5u;
#else
hit.state = makeTraceState(RT_PURPOSE_PRIMARY_SKY);
const uint k1PrimarySbtOffset = 0u;
#endif

#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
serTraceRayOffset(
    origin.xyz,
    0.001,
    worldDir.xyz,
    10000.0,
    gl_RayFlagsCullBackFacingTrianglesEXT,
    0x21u,
    s,
    k1PrimarySbtOffset
);
#else
#ifdef RT_ENABLE_SER
// Perf-L10.8: same ray, same flags; SER only reschedules the hit shader.
if(cam.perfSerAb.y != 0u)
{
    serTraceRayOffset(
        origin.xyz,
        0.001,
        worldDir.xyz,
        10000.0,
        gl_RayFlagsCullBackFacingTrianglesEXT,
        0x21u,
        s,
        k1PrimarySbtOffset
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
    gl_RayFlagsCullBackFacingTrianglesEXT,
        0x21u,
        k1PrimarySbtOffset, 0, 0,
        origin.xyz,
        0.001,
        worldDir.xyz,
        10000.0,
        0
    );
}
#endif


// =====================================================
// 保存 Primary Ray 命中结果。
// 从这里开始，后面的 reflection/AO/shadow/GI
// 都允许继续覆盖 payload。
// =====================================================

float primaryHitDistance =
    payload.hitDistance;

// 2026-10-09 运动矢量：命中点的**上一帧世界位移**（prev − cur）必须与 hitDistance
// **同一时刻**取走 —— 就在上面那段说明的下方，后面的 reflection/AO/shadow/GI 每次
// traceRay 都会重写 payload（包括这三个字段），late 读就是"别的物体的位移"。
// 天空（miss）时 rmiss 把 payload 诸字段写成 0/哨兵，位移在这里自然是 0。
//
// 世界位移（不是相机相对）：closest-hit 已经用 UBO 的 viewInverse 平移列把 Δcam
// 补好了，见 rt_test.rchit 里那段说明 —— rgen **不要**再补一次。
vec3 primaryHitWorldOffset =
    vec3(
        0.0,
        0.0,
        0.0
    );

#if !GI_HALFRES
// 2026-11-21 P0-1：从旁路槽（binding 98）读回 rchit 算好的主命中世界位移。
// xyz = 世界位移，a = 有效标记；a < 0.5（miss / 未被本帧写过）按 vec3(0) 处理。
// 守卫与现有 MV 写出同节奏（!GI_HALFRES 且 s==0，见下方 dlssMotionImage 写出块）：
// 只取首样本值，避免多样本互相覆盖后的随机性。K2 传输腿走 landing 重建，槽内为
// 清后 0，与改动前硬编码 vec3(0) 逐位一致。
if(s == 0)
{
    const vec4 hitOffsetSlot =
        imageLoad(hitOffsetImage, pixelCoord);

    if(hitOffsetSlot.a >= 0.5)
    {
        primaryHitWorldOffset = hitOffsetSlot.xyz;
    }
}
#endif

// 2026-10-24：主射线是否命中了粒子。必须与 hitDistance/prevOffset **同一时刻**
// 取走 —— 之后的 reflection/AO/shadow/GI 每次 traceRay 都会重写 payload.state
//（与上面 prevOffset 的注释同一约束）。消费者：循环外的 TransparencyHint 写入。
rrParticlePixel =
    (payload.state & RT_STATE_PARTICLE_BIT) != 0u;
primaryHandPixel =
    (payload.state & RT_STATE_HAND_BIT) != 0u;
g_secondaryCullMask = primaryHandPixel ? 0xcdu : 0xcfu;

// =====================================================
// DH 的"我们侧"判据（09-23 第三次定案）：**只有 miss 算远场**
// =====================================================
// miss ⇒ 我们的 TLAS 在这个方向上没有任何几何（视距之外），远场是唯一内容。
//
// 为什么**删掉**"距离 > 64 格"这条：
//   ① 64 格 ~ 我们视距边缘这一整段，我们的 TLAS 里是**真几何**（原版区块 quad），
//      它永远比 DH 的 LOD 精确 ⇒ 让 LOD 顶掉它只会白丢细节；
//   ② 这条判据在屏幕上画的正是一个**以相机为中心的等距边界**（平视=直线、
//      俯视=圆弧，实测截图两者都拍到过），而旧合成逻辑在那个边界上把 DH 的
//      颜色按深度差**混进来一半**（远处则整片是 DH）⇒ 一条既硬又亮的圆弧，
//      就是用户报的"颜色明显不同的线 / 圆形缺块 / 某个方向非常突兀"；
//   ③ 判据变成"我们命中就是我们的"之后，交接边界落在**我们自己数据的边缘**
//      ——语义上唯一合理的边界——再配上两侧同一套光照（见重打光那段），
//      台阶就消失了，不需要任何"深度竞争 / 渐变权重"补丁。
if(primaryHitDistance < 0.0)
{
    dhFarFieldOurs = 1.0;
}

// 带出循环给合成块用：负数 = 我们没命中（远场分支），正值 = 我们真的命中了。
// "补空气"那条分支必须只在前者之外生效（否则会拿平色盖掉真地形）。
dhFarFieldOursDistance = primaryHitDistance;

// 同一个理由把主射线方向也带出去（水下视角要用）。SAMPLES_PER_FRAME 目前是 1，
// 每次只有一个样本，所以这里存的就是这个像素唯一的方向；将来若放开多样本平均，
// 水柱应该改用每样本各自的方向，而不是这里留下的最后一个。
primaryViewDirectionForWater = worldDir.xyz;
#else
    // ORION_K2_TRANSPORT：载入 landing，重建主命中状态。
    // 这里的 worldDir 与主变体的主射线方向逐位一致（同一相机 UBO、同一
    // pixelCoord 推导，无随机成分），所以 primaryViewDirectionForWater 可直接
    // 重算。seed 在循环头已按同一公式重播种，opaque 路径在切点前不消费
    // 主随机流 ⇒ K2 的传输段随机序列与主变体逐位一致。
    if(!ptLandingLoad(
        ptLandingRecordIndex(
            uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
            s
        ),
        rtDebugPrimaryMotion
    ))
    {
#ifdef ORION_K2I_COMPOSE_COMPUTE
        if(SAMPLES_PER_FRAME == 1)
        {
            return;
        }
#endif
        continue;
    }
    ptLandingActive = true;

    // B①：rgen 侧材质解析。身份（hitDistance/slot/primitive/bary）已由
    // landing 载入 payload，与主变体 rchit 走同一份材质层 ⇒ 材质字段逐位同值。
    // ③/④：稳定路径走内联 resolveHitMaterial（ORION_K2_MATERIAL_INLINE）；
    // D1 callable 路径硬崩 parked，借 landing 槽位回传（material_callable.glsl）。
#ifdef ORION_K2_MATERIAL_INLINE
    // P2 跨腿材质复用（运行期闸门 cam.matReuseAb.x = ORION_MAT_REUSE_LEGS，默认 0）。
    // 关闭：三条腿各自原地 resolveHitMaterial，与旧行为逐位一致。
    // 打开：SUN 解析一次后落中转 buffer（binding 94），LOC/I 读回灌进 payload、
    // 跳过解析。下标与 landing 记录同式（同像素同样本）。
    const uint matReuseIdx =
        matReuseRecordIndex(
            uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x), s);
    const bool matReuseOn = cam.matReuseAb.x != 0u;
    const bool matReuseAssert = cam.matReuseAb.y != 0u;
#if defined(ORION_K2_DIRECT_SUN_STAGE) || defined(ORION_K2_DIRECT_SUN_STAGE_A)
    // 生产者腿（K2-SUN / ⑤ K2-SUNA）：照常解析一次；复用打开则把材质快照落
    // 中转 buffer。⑤ 拆腿后 SUNA 独揽生产行（SUNB 的水像素由 SUNA 覆盖、
    // 地形像素也在 SUNA 预 fork 落记录），SUNB 与 LOC/I 一样做消费者。
    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntMateval();
    #endif
    resolveHitMaterial(
        origin.xyz,
        worldDir.xyz);
    if(g_hitVertexMaterialId > 0.2 && g_hitVertexMaterialId < 0.45)
    {
        payload.state |= RT_STATE_FOLIAGE_BIT;
    }
    if(matReuseOn)
    {
        matReuseStore(matReuseIdx);
    }
#elif defined(ORION_K2_DIRECT_SUN_STAGE_B) || defined(ORION_K2_DIRECT_LOCAL_STAGE) || defined(ORION_K2_INDIRECT_STAGE)
    // 消费者腿（K2-SUNB / K2-LOC / K2-I）
#ifdef ORION_K2_CONSUMER_MAT_REUSE_ONLY
    MatReuseRecord reuseRecord;
    if(!matReuseLoad(matReuseIdx, reuseRecord))
    {
        continue;
    }
    matReuseApply(reuseRecord);
#else
    if(matReuseOn)
    {
        MatReuseRecord reuseRecord;
        if(matReuseLoad(matReuseIdx, reuseRecord))
        {
            if(matReuseAssert)
            {
                // 校验臂：先缓存复用值，再原地解析一次，逐位比对；payload 留解析结果。
                #ifdef ORION_RT_STAGE_COUNTERS
                rtCntMateval();
                #endif
                resolveHitMaterial(
                    origin.xyz,
                    worldDir.xyz);
                matReuseAssertCompare(reuseRecord);
            }
            else
            {
                matReuseApply(reuseRecord);
            }
        }
        else
        {
            // 无有效生产者记录（防御路径）：退回原地解析。
            #ifdef ORION_RT_STAGE_COUNTERS
            rtCntMateval();
            #endif
            resolveHitMaterial(
                origin.xyz,
                worldDir.xyz);
        }
    }
    else
    {
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntMateval();
        #endif
        resolveHitMaterial(
            origin.xyz,
            worldDir.xyz);
    }
#endif
#else
    // 其余 K2 变体（单发 K2 / ③ K2-D / 内联版 / 各 SER 等）：不参与复用，原路径。
    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntMateval();
    #endif
    resolveHitMaterial(
        origin.xyz,
        worldDir.xyz);
#endif
#else
    matCallableResolveToLanding(
        origin.xyz,
        worldDir.xyz,
        ptLandingRecordIndex(
            uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
            s
        ));
#endif

    // 主变体在主射线后立刻取的 state 派生量（rrParticlePixel）；K2 在解析后
    // 重算 —— 粒子位由 material_resolve 的 makeHitState 置位。
    rrParticlePixel =
        (payload.state & RT_STATE_PARTICLE_BIT) != 0u;
    primaryHandPixel =
        (payload.state & RT_STATE_HAND_BIT) != 0u;
    g_secondaryCullMask = primaryHandPixel ? 0xcdu : 0xcfu;

    // 主变体在 #ifndef 段声明 primaryHitDistance；K2 在这里自己声明。
    float primaryHitDistance = payload.hitDistance;
    dhFarFieldOursDistance = primaryHitDistance;
    primaryViewDirectionForWater = worldDir.xyz;
#endif


// =====================================================
// P1-2A - RT distance sampling LOD
//
// Hard distance bands:
//   0-16  = near
//   16-32 = medium
//   32-48 = far
//   48+   = very far
//
// No Raster color compositing in this version.
// =====================================================

float rtLodDistance =
    max(
        primaryHitDistance,
        0.0
    );


uint rtLodState =
    uint(pixelCoord.x) * 73856093u ^
    uint(pixelCoord.y) * 19349663u ^
    cam.frameCount * 83492791u;


uint rtLodRandom =
    pcg_hash(
        rtLodState
    );


bool rtLodHalfSample =
    (rtLodRandom & 1u) == 0u;


bool rtLodQuarterSample =
    (rtLodRandom & 3u) == 0u;


bool rtLodGlossyHalfSample =
    ((rtLodRandom >> 3u) & 1u) == 0u;


bool rtLodGlossyQuarterSample =
    ((rtLodRandom >> 3u) & 3u) == 0u;

// ⭐ 远场间接光的抽稀位（bit 0~4 已被 half/quarter/glossy 占用，这里从 bit 5 起）。
//   三个独立 1/2 嵌套 ⇒ 1/8 / 1/16 / 1/32。时间累积负责把它们抹平。
bool rtLodEighthSample =
    ((rtLodRandom >> 5u) & 1u) == 0u;

bool rtLodSixteenthSample =
    ((rtLodRandom >> 6u) & 1u) == 0u;

bool rtLodThirtySecondSample =
    ((rtLodRandom >> 7u) & 1u) == 0u;


// 2026-10-25 近场 GI 降速专用位（bit 0~7 已被 half/quarter/glossy/远场 1/8~1/32 占满）。
// ⚠ 必须是**独立位**：与距离阶梯的位重叠会让"两级速率相乘、权重乘两次"变成
//   权重过曝（光泽那边就是因此只能合成一个级别）。
bool rtLodGiHalfSample =
    ((rtLodRandom >> 8u) & 1u) == 0u;

bool rtLodGiQuarterSample =
    ((rtLodRandom >> 8u) & 3u) == 0u;


// =====================================================
// 间接光（漫反射 GI）距离梯形 —— 【唯一出口】
//
//   0-64    : 1.0     spp
//   64-128  : 0.5     spp（时间交错）
//   128-160 : 0.25    spp（时间交错）
//   160-256 : 0.125   spp（时间交错）  ⭐ 2026-09-24 新增
//   256-384 : 0.0625  spp（时间交错）  ⭐
//   384+    : 0.03125 spp（时间交错）  ⭐
//
// ⚠ 为什么必须补最后三档（用户报"离远了背光面直接变成全黑"）：
//   主着色的间接光项是 `indirectLight * 1.8 * rtaoFactor`（见 litColor），
//   而环境底 `getSurfaceAmbientBase()` 在有日照时会被压到 `base * 0.55`
//   （用户设置里的本局 base ≈ 0.01）⇒ 原来 `rtLodDistance >= 160` 直接
//   `runDiffuseGI = false`，于是 160 格外**法线背对太阳的面只剩
//   albedo × 0.01 ≈ 纯黑**。同距离段 RTAO 也一起归零，连"暗一点"都兜不住。
//   这不是"远场需要另一种光"，而是**这个梯形被人为砍断了**：我们的远场几何
//   本来就在 TLAS 里，真 GI 一直可用，只是要按距离继续折半采样率。
//   ⇒ 不用引入 SSR / SSPT（它们需要屏幕内已着色的反弹源 + G-buffer 法线，
//     而远场是单面 quad、视角又几乎平行地面，恰好是屏幕空间最差的工况）。
//
// ⚠ 这里是**唯一**出口。此前 `prepareDiffuseGI`（G2B 方向准备）与
//   `runDiffuseGI`（GI while 的入口）各写了一份一模一样的距离判断，
//   改一处漏一处就会变成"GI 打开了、但 bounceDir 还是零向量"
//   ⇒ traceRay 方向为零，**静默变黑**（比不开还难看，且完全查不出来）。
// =====================================================

bool rtLodDiffuseGiEnabled = false;

float rtLodDiffuseGiWeight = 1.0;

if(rtLodDistance < 64.0 * perfL10TierScale())
{
    rtLodDiffuseGiEnabled = true;
}
else if(rtLodDistance < 128.0 * perfL10TierScale())
{
    rtLodDiffuseGiEnabled = rtLodHalfSample;
    rtLodDiffuseGiWeight = 2.0;
}
else if(rtLodDistance < 160.0 * perfL10TierScale())
{
    rtLodDiffuseGiEnabled = rtLodQuarterSample;
    rtLodDiffuseGiWeight = 4.0;
}
else if(rtLodDistance < 256.0 * perfL10TierScale())
{
    rtLodDiffuseGiEnabled = rtLodEighthSample;
    rtLodDiffuseGiWeight = 8.0;
}
else if(rtLodDistance < 384.0 * perfL10TierScale())
{
    rtLodDiffuseGiEnabled = rtLodSixteenthSample;
    rtLodDiffuseGiWeight = 16.0;
}
else
{
    rtLodDiffuseGiEnabled = rtLodThirtySecondSample;
    rtLodDiffuseGiWeight = 32.0;
}


// stage 3/4 = "整条漫反射 GI 关掉"的性能测量档，折进同一个出口。
if(perfL10DisableDiffuseGi())
{
    rtLodDiffuseGiEnabled = false;
}


// =====================================================
// 2026-10-25：近场 GI 降速（sub-1spp 的 GI 部分）
//
// 上面那张距离阶梯只管 64 格以外；室内/近场原本一律全速，而 noGi 消融在室内
// 是 -5.86ms（rt=18.04，光泽分级后 base 15.30），是当前最大的单项。
//
// 用**独立 bit 片**（rtLodGiHalfSample / rtLodGiQuarterSample，bit8/9）：
// 与距离阶梯的 bit0~7 不重叠 ⇒ 两级速率相乘、rtLodDiffuseGiWeight 同步相乘，
// 估计量仍无偏。跳过的是 GI **信号**而不是像素 —— 颜色/深度/MV/guide 每帧照写，
// 所以不会像 spp=0.5 那样出现锯齿（2026-10-25 实测过：整像素棋盘会退化边缘）。
//
// 代价：该帧不跑 GI 的像素 indirectLight = 0，跑的那帧乘 2（或 4）。时间平均
// 正确，但方差按倍数放大（半速约 1.4×、四分一约 2× 幅度），由 DLSS-RR 的时间
// 累积抹平。若实测噪点不可接受，下一步才是"读自己上一帧的 GI 历史"那套。
// 回退：宿主 ORION_GI_NEAR_RATE=0。
// =====================================================
if(cam.giRayParams.x > 0.5f)
{
    if(cam.giRayParams.x > 1.5f)
    {
        rtLodDiffuseGiEnabled =
            rtLodDiffuseGiEnabled &&
            rtLodGiQuarterSample;

        rtLodDiffuseGiWeight *=
            4.0;
    }
    else
    {
        rtLodDiffuseGiEnabled =
            rtLodDiffuseGiEnabled &&
            rtLodGiHalfSample;

        rtLodDiffuseGiWeight *=
            2.0;
    }
}


#ifndef ORION_K2_TRANSPORT
float primaryPOMSelfShadow =
    clamp(
        payload.pomSelfShadow,
        0.0,
        1.0
    );

// 2026-10-05 高度场微遮蔽（#12 环境光/GI）与微结构反射遮蔽（#44）：
// POM 命中点的静态深度遮蔽，rchit 导出（非 POM 面 = 1.0 ⇒ 逐位旧档）。
float primaryPOMMicroOcclusion =
    clamp(
        payload.pomMicroOcclusion,
        0.0,
        1.0
    );


float primaryGlassAlpha =
    clamp(
        payload.glassAlpha,
        0.0,
        1.0
    );

// alpha blend 的源 alpha（texture x vertex），与覆盖率字段分离。
float primaryGlassBlendAlpha =
    clamp(
        0.0,
        0.0,
        1.0
    );


float primaryRoughness =
    clamp(
        payload.roughness,
        0.0,
        1.0
    );


float primaryF0Metal =
    payload.f0Metal;


float primaryParallaxRayTravel =
    max(
        payload.parallaxRayTravel,
        0.0
    );

// =====================================================
// 玻璃的"内部折射"：解析式电介质平板（GI_GLASS_DIELECTRIC）
//
// 旧做法（=0）把 rchit 的 POM 浮雕位移当成内部折射。那份位移是**逐纹理 texel
// 量化**的：视向一动、采到的 texel 一翻，玻璃后的接收面就整体跳一格 ⇒ 玻璃上
// 出现与玻璃纹理同形的颗粒，而且只跟本地直射光走（太阳那份被 shadowTransmittance
// 压掉），采样数/候选数都动不了它。
//
// 新做法（=1，默认）按 Snell 解析求平板出射点：入射点 P、面法线 N、入射方向 I、
// 板厚 t、折射率 n ⇒ 内部方向 T = refract(I, N, 1/n)、出射点 E = P + T*(t/|dot(T,N)|)。
//   * 位移是世界空间**连续**的（不含任何纹理量化）⇒ 同形颗粒的根因消失；
//   * 不额外发任何射线（仍是原来那一条透射频）⇒ 性能与旧路径完全相同；
//   * 平板的出射方向与入射方向相同（只整体横移），所以走查仍沿原方向继续；
//   * 板厚由命中面到格心的距离推出（玻璃块 1.0 / 玻璃板 0.125），无需第二次 trace。
//
// A/B：-DGI_GLASS_DIELECTRIC=0 回到 POM 假折射。
// 默认值在文件前部（5200 附近）集中定义。
// =====================================================

// 入射面命中点（**不含** POM 位移），既是解析折射的输入，也是历史键。
vec3 primaryGlassEntryPoint =
    origin.xyz +
    worldDir.xyz *
    primaryHitDistance;

float primaryGlassSlabThickness =
    0.0;

bool primaryGlassUsesDielectric =
    false;

// =====================================================
// 2026-10-28：解析出射点必须用**面法线**（轴对齐），不能用带法线贴图扰动的
// payload.normal。
//
// 现象（用户）：玻璃正对面干净，与它不平行的其他面全是固定形状的块状噪点；
//   ORION_GLASS_ALPHA_BLEND_DBG=9 旁路 alpha blend 后仍在 ⇒ 在 rgen 输出里。
//
// 机理：出射点 = entry + insideDirection * (thickness / cosInside)
//   · payload.normal 已被法线贴图逐 texel 扰动（LabPBR 浮雕）；
//   · thickness 由 glassSlabThicknessFromFace(entry, normal) 按"命中面到格心
//     的距离"给出 —— 法线被扰动到靠近另一个主轴时，它会从 1.0（整块）跳到
//     0.125（板）；
//   · cosInside = -dot(refract(dir, n, 1/ior), n)，掠射时 -> 0，1/cosInside 放大。
//   ⇒ 链起点逐 texel 乱跳 ⇒ 穿过玻璃看到的背景被切成块。
//   正对面 |N·V|≈1，1/cosInside≈1，扰动不放大 ⇒ 干净 —— 这就是"平行才干净"。
//
// MC 的方块面全部轴对齐 ⇒ 把扰动法线吸附到最近主轴即可恢复连续出射点。
// 光照仍用带法线贴图的 payload.normal（本变量只服务解析折射与厚度）。
//
// A/B：-DORION_GLASS_EXIT_AXIS_NORMAL=0 回到旧档（逐位）。
// =====================================================
#ifndef ORION_GLASS_EXIT_AXIS_NORMAL
#define ORION_GLASS_EXIT_AXIS_NORMAL 1
#endif
vec3 primaryGlassExitNormal =
    payload.normal;
#if ORION_GLASS_EXIT_AXIS_NORMAL
{
    const vec3 exitAxisAbs =
        abs(payload.normal);

    if(exitAxisAbs.x > max(exitAxisAbs.y, exitAxisAbs.z))
    {
        primaryGlassExitNormal =
            vec3(payload.normal.x >= 0.0 ? 1.0 : -1.0, 0.0, 0.0);
    }
    else if(exitAxisAbs.y > exitAxisAbs.z)
    {
        primaryGlassExitNormal =
            vec3(0.0, payload.normal.y >= 0.0 ? 1.0 : -1.0, 0.0);
    }
    else
    {
        primaryGlassExitNormal =
            vec3(0.0, 0.0, payload.normal.z >= 0.0 ? 1.0 : -1.0);
    }
}
#endif

#if GI_GLASS_DIELECTRIC
primaryGlassUsesDielectric =
    getLegacyMaterialId(payload.state) >= 2.5;

if(primaryGlassUsesDielectric)
{
    primaryGlassSlabThickness =
        glassSlabThicknessFromFace(
            primaryGlassEntryPoint,
            primaryGlassExitNormal
        );

    // RR 深度仍要一个沿视线的标量：给出射面的距离。
    primaryParallaxRayTravel =
        glassSlabTravelAlongView(
            primaryGlassExitNormal,
            worldDir.xyz,
            primaryGlassSlabThickness
        );
}
#else
#if !GI_GLASS_PANE_POM
// See GI_GLASS_PANE_POM: a glass pane's relief displacement is quantised per
// texture texel, so it makes the transmitted receiver jump one texel at a time
// and paints the glass with a texture-shaped grain.  Opaque blocks keep theirs.
if(getLegacyMaterialId(payload.state) >= 2.5)
{
    primaryParallaxRayTravel =
        0.0;
}
#endif
#endif

#else
    // ORION_K2_TRANSPORT：重算切点前的 payload 标量捕获（与主变体逐字同源）。
    // B①：主命中标量是 payload 的纯函数，而 payload 在 K2 里由
    // resolveHitMaterial() 从身份重建（rgen_k2_material.glsl 的调用点），
    // 所以这里重算的结果与主变体 rchit 路径逐位一致。
    float primaryPOMSelfShadow =
        clamp(payload.pomSelfShadow, 0.0, 1.0);
    float primaryPOMMicroOcclusion =
        clamp(payload.pomMicroOcclusion, 0.0, 1.0);
    float primaryGlassAlpha =
        clamp(payload.glassAlpha, 0.0, 1.0);
    float primaryGlassBlendAlpha =
        clamp(0.0, 0.0, 1.0);
    float primaryRoughness =
        clamp(payload.roughness, 0.0, 1.0);
    float primaryF0Metal =
        payload.f0Metal;
    float primaryParallaxRayTravel =
        max(payload.parallaxRayTravel, 0.0);
    // 玻璃支路专用标量：K2 运行时永远走不到（landing 只装延期像素，玻璃由
    // K1 行内处理），但玻璃支路的代码仍要编译通过 —— 按主变体同源的声明补齐。
    vec3 primaryGlassEntryPoint =
        origin.xyz + worldDir.xyz * primaryHitDistance;
    float primaryGlassSlabThickness = 0.0;
    bool primaryGlassUsesDielectric = false;
    vec3 primaryGlassExitNormal = payload.normal;
#endif // ORION_K2_TRANSPORT（标量捕获段；下面的 RR-6.2 guide 计算 K1/K2 共用）


// =====================================================
// RR-6.2 - Primary surface material guides
// Capture before any secondary ray overwrites payload.
// =====================================================

#if !defined(ORION_K2_DIRECT_SUN_STAGE_B) && !defined(ORION_K2_DIRECT_LOCAL_STAGE) \
    && !defined(ORION_K2_INDIRECT_TRACE_ONLY) && !SHARC_UPDATE
if(
    s == 0 &&
    primaryHitDistance >= 0.0 &&
    getLegacyMaterialId(payload.state) <= 0.5
)
{
    vec3 guideAlbedo =
        unpackAlbedoUnorm(payload.albedoPacked);

    vec3 guideNormal =
        normalize(
            payload.normal
        );

    float guideRoughness =
        clamp(
            payload.roughness,
            0.0,
            1.0
        );

    int guideF0MetalCode =
        decodeF0MetalCode(
            payload.f0Metal
        );

    float guideMetallic =
        decodeMetallicFromCode(
            guideF0MetalCode
        );

    vec3 guideF0 =
        decodeBaseF0(
            guideF0MetalCode,
            guideAlbedo
        );

    vec3 guideV =
        normalize(
            -worldDir.xyz
        );

    float guideNdotV =
        abs(
            dot(
                guideNormal,
                guideV
            )
        );

    rrDiffuseAlbedo =
        guideAlbedo *
        (
            1.0 -
            guideMetallic
        );

    rrSpecularAlbedo =
        rrEnvBRDFApprox2(
            guideF0,
            guideRoughness *
                guideRoughness,
            guideNdotV
        );

    rrNormalRoughness =
        vec4(
            guideNormal,
            guideRoughness
        );
}
#endif


#if !defined(ORION_K2_TRANSPORT) && !defined(ORION_K1_GLASS_ONLY)
// K1 独占：G-buffer（depth / dlssMotion）写入。K1 对**所有**像素（含延期到
// K2 的不透明/水）各写一遍，K2 跳过 —— 同址两写没有屏障保证顺序。
// =====================================================
// Probe - RR POM depth consistency
//
// ClosestHit already reports the POM virtual surface travel
// through primaryParallaxRayTravel.
//
// RR albedo / normal / roughness already describe that POM
// surface, so feed RR depth from the same virtual position.
//
// Main shading and all ray origins remain untouched.
// =====================================================

#if !GI_HALFRES
// 2026-10-24 半分辨率 GI 变体**不写** G-buffer（depthImage / dlssMotionImage）：
// 它与主变体写的是同一批像素，而两条 dispatch 之间没有屏障 ⇒ 同址两写是未定义
// 顺序，DLSS 可能吃到半分辨率变体那份（天空像素的 MV 还是按 launchID 算的）。
// 主变体本来就对每个像素无条件写一遍，所以这里跳过不影响任何输出。
if (s == 0)
{
    // Minecraft 26.2 reversed-Z:
    // miss / far = 0.0
    float primaryDepth =
        0.0;

    // 2026-10-09 运动矢量：默认 0。在 cameraMotionIncluded=false 的语义下，
    // "0" 对天空与**静态几何**都是**正确值**（它们没有物体自身运动）。
    vec2 primaryMotionPixels =
        vec2(0.0);


    if(primaryHitDistance >= 0.0)
    {
        float primarySurfaceTravel =
            getHitClass(payload.state) == RT_HIT_TRANSLUCENT
                ? primaryParallaxRayTravel
                : 0.0;

        vec3 primaryHitPos =
    origin.xyz +
    worldDir.xyz *
    (primaryHitDistance + primarySurfaceTravel);


        // =================================================
        // 2026-10-09 运动矢量（DLSS-RR）
        //
        // mv = project_prevCam(P + d) − project_prevCam(P)
        //   · P = 本帧世界位置（primaryHitPos，含 POM 位移行程，与深度同源）
        //   · d = 命中点位移（closest-hit 从顶点缓冲尾段插值出的**相机相对**位移，
        //         在 closest-hit 里补 Δcam 之后经 payload 传来 —— 不是尾段原值）
        //
        // 为什么只有这一项就够：宿主告诉 DLSS 的是 cameraMotionIncluded=false，
        // 即"MV 里不要含相机运动"。两次投影都过**上一帧**的 view-projection，
        // 于是相机位移在两项里完全相同、自动抵消；剩下的恰好就是物体自身的运动。
        //
        // 为什么静态面必然是 0：地形四个 class 的 motionOffset 恒为 0 ⇒ d=0 ⇒
        // 两项投影同一个世界点 ⇒ 差恒为 0。所以这一改动**逐位不动静态画面**，
        // 不会出现"改完整屏糊"的风险。
        //
        // 矩阵来源：cam.prevViewInverse / cam.prevProjInverse 是**逆矩阵**，而
        // 我们需要正变换 prevProj * prevView = inverse(prevViewInverse * prevProjInverse)
        // ✓（(AB)⁻¹ = B⁻¹A⁻¹）。这一对矩阵不含 jitter ⇒ 结果天然满足
        // motionVectorsJittered=false。
        // =================================================
        {
            // 2026-10-09：这里的位移**已经是世界位移** —— 相机相对 → 世界的换算
            // （补 Δcam）在 closest-hit 里做，因为只有那里知道"这一面有没有位移段"
            // （见 rt_test.rchit main() 里「★★ 修「移动时四周闪动」：相机相对 → 世界
            // ★★」那一段，`hitWorldOffset += prevViewInverse[3] - viewInverse[3]`）。
            // 因此**不要**在这里再补相机项：静态面（无位移段 ⇒ offset 恒 0）必须
            // 保持恰好 0，那正是它该有的值；在这里无条件加会让静态世界拿到
            // MV = Δcam，一移动就全屏闪。
            const vec3 prevWorldPos =
                primaryHitPos + primaryHitWorldOffset;

            // 2026-11-09：= inverse(prevViewInverse * prevProjInverse)，宿主算好。
            const mat4 prevViewProj =
                cam.prevViewProj;

            // =================================================
            // 2026-10-22（官方口径修正）
            //
            // DLSS-RR 要求 MV 是 **dense** 场：
            //   "Dense motion vector field (i.e. includes camera motion,
            //    and motion of dynamic objects)"
            //   —— docs/ProgrammingGuideDLSS_RR.md §4.1.6
            //
            // 所以两次投影**不能用同一个矩阵**：
            //   上一帧世界位置 → 上一帧 viewProj
            //   当前世界位置   → 当前帧 viewProj
            // 两者之差自然包含相机运动；静态面（offset=0）也会得到正确的
            // 屏幕位移，而不再恒 0。
            //
            // 旧写法两个投影都过 prevViewProj，等于把相机运动从 MV 里剔除，
            // 再靠 cameraMotionIncluded=false 让 SL 自己补 —— 而 SL 的补法与
            // 我们的 clipToPrevClip 一旦不完全一致，整屏就错位。
            // =================================================
            const mat4 curViewProj =
                cam.viewProj;

            const vec4 prevClipPrev =
                prevViewProj * vec4(prevWorldPos, 1.0);

            const vec4 curClipCur =
                curViewProj * vec4(primaryHitPos, 1.0);

            if(abs(curClipCur.w) > 1.0e-6 &&
               abs(prevClipPrev.w) > 1.0e-6)
            {
                const vec2 prevNdc =
                    prevClipPrev.xy / prevClipPrev.w;

                const vec2 curNdc =
                    curClipCur.xy / curClipCur.w;

                // NDC → UV 位移：x 半宽；UV 的 y 向下而 NDC 的 y 向上 ⇒ y 取负。
                // 输出即为 UV 单位，宿主配 mvecScale=(1,1)（官方 §6.0）。
                primaryMotionPixels =
                    (prevNdc - curNdc) * vec2(0.5, -0.5);
            }
        }


        vec4 viewPos =
            viewMatrix *
            vec4(
                primaryHitPos,
                1.0
            );


        vec4 clipPos =
            projectionMatrix *
            viewPos;


        if(abs(clipPos.w) > 1.0e-6)
        {
            primaryDepth =
                clamp(
                    clipPos.z /
                    clipPos.w,
                    0.0,
                    1.0
                );
        }
    }
    else
    {
        // =================================================
        // 2026-10-23：天空（miss）的运动矢量。
        //
        // 症状：抬头看天再转动视角，整片天空拖影。
        // 原因：命中分支算的是 dense MV（含相机运动），miss 分支却让
        //   primaryMotionPixels 保持 0；而宿主告诉 DLSS 的是
        //   cameraMotionIncluded = eTrue。于是 SL 认为"天空这一帧没动"，
        //   仍按 0 位移重投影历史 ⇒ 相机一转，天空历史全错位。
        //
        // 天空点按**无穷远**处理：用齐次方向向量（w = 0）投影，相机平移天然
        // 不参与，只剩旋转：
        //   prevClip = prevViewProj * vec4(dir, 0)
        //   curClip  = curViewProj  * vec4(dir, 0)
        // 与命中分支同一套 NDC→UV 换算（宿主 mvecScale = (1,1)）。
        //
        // 方向必须是**去抖**的：frameJitter 只作用于当前帧，若把它带进上一帧
        // 投影，MV 里会多出约 1px 的假位移（宿主 motionVectorsJittered=false）。
        // =================================================
        const vec2 skyPixelCenter =
            vec2(gl_LaunchIDEXT.xy) + vec2(0.5);

        const vec2 skyUv =
            skyPixelCenter / vec2(rtLaunchSize());

        vec2 skyNdc = skyUv * 2.0 - 1.0;
        skyNdc.y = -skyNdc.y;

        const vec4 skyTarget =
            cam.projInverse * vec4(skyNdc.x, skyNdc.y, 1.0, 1.0);

        const vec3 skyViewTarget =
            abs(skyTarget.w) > 1.0e-6
                ? skyTarget.xyz / skyTarget.w
                : skyTarget.xyz;

        const float skyViewTargetLengthSquared =
            dot(skyViewTarget, skyViewTarget);

        const vec3 skyRayDir =
            skyViewTargetLengthSquared > 1.0e-12
                ? skyViewTarget * inversesqrt(skyViewTargetLengthSquared)
                : vec3(0.0, 0.0, -1.0);

        const vec3 skyWorldDir =
            (cam.viewInverse * vec4(skyRayDir, 0.0)).xyz;

        const mat4 skyPrevViewProj =
            cam.prevViewProj;

        const mat4 skyCurViewProj =
            cam.viewProj;

        const vec4 skyPrevClip =
            skyPrevViewProj * vec4(skyWorldDir, 0.0);

        const vec4 skyCurClip =
            skyCurViewProj * vec4(skyWorldDir, 0.0);

        if(abs(skyCurClip.w) > 1.0e-6 &&
           abs(skyPrevClip.w) > 1.0e-6)
        {
            const vec2 skyPrevNdc =
                skyPrevClip.xy / skyPrevClip.w;

            const vec2 skyCurNdc =
                skyCurClip.xy / skyCurClip.w;

            primaryMotionPixels =
                (skyPrevNdc - skyCurNdc) * vec2(0.5, -0.5);
        }
    }


    imageStore(
        depthImage,
        pixelCoord,
        vec4(
            primaryDepth,
            0.0,
            0.0,
            0.0
        )
    );

    // =====================================================
    // 2026-10-09 运动矢量：**每个像素、每帧、无条件**写一次。
    //
    // 为什么必须无条件：这张图只在"图像被重建"时 clear 过一次
    // （旧的 needsMotionClear 是拿图像句柄做判据的），一旦某个像素某帧不写，
    // 它会保留**上上帧**的旧 MV，DLSS 就会按那个旧位移去重投影 ⇒ 新的拖影。
    // 所以 miss/静态面也在这里写 0，而不是省下这一次写。
    // =====================================================
    // 2026-10-09 总开关（设置项「运动矢量」）：关掉时写 **0** 而不是不写。
    //   不写 ⇒ 这一格保留上一帧的值，而 DLSS 仍会拿它去重投影（错得没边）；
    //   写 0 ⇒ 确定的"这一帧没有物体运动"，与接 MV 之前**逐位一致**。
    // 顺带把"每像素每帧都写"钉死：无论是总开关关着、还是 spp=0.5 棋盘跳过像素，
    // 这一格都不会留下旧值（历史上只有 spp=0.5 那条早退会跳过，那条已单独讨论过）。
    const vec2 motionOutput =
        cam.perfAblationStage.z != 0u
            ? primaryMotionPixels
            : vec2(0.0);

    imageStore(
        dlssMotionImage,
        pixelCoord,
        vec4(
            motionOutput,
            0.0,
            1.0
        )
    );

    // 调试图用（DBG_SHOW_MV）：把写出去的那个值留到最终 imageStore 处画出来。
    rtDebugPrimaryMotion = primaryMotionPixels;
}
#endif

#elif defined(ORION_K1_GLASS_ONLY)
if(s == 0)
{
    rtDebugPrimaryMotion = unpackHalf2x16(ptLandingRecords[
        ptLandingRecordIndex(
            uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x), 0
        )
    ].b.z);
}
#endif // ORION_K2_TRANSPORT（重开的 K1 独占段：G-buffer / depth / MV 写入）

// =====================================================
// 2026-10-26 稀疏渲染抽签。
//
// 位置刻意放在 depth/MV 写入**之后**：非活跃像素的 depth 与 motion vector
// 已经是本帧新鲜值，RR/DLSS 的重投影与重建不依赖本像素的 radiance。
// 后续只把 RTAO / 太阳软影 / 本地光 / GI / glossy 的**贡献与射线**置零，
// 不提前 return —— guides 与主命中的自发光/天空仍在下面照常写。
// =====================================================
bool vptPixelActive = true;
#if !SHARC_UPDATE && !GI_HALFRES
vptPixelActive = vptSparsePixelIsActive(uvec2(pixelCoord));
#endif

// 2026-10-27 阶段 3：带出循环（只有做稀疏的变体需要；SHARC_UPDATE / GI_HALFRES
// 不判稀疏，槽保持默认 true/false ⇒ 末尾全写，与它们原有的行为一致）。
#if !SHARC_UPDATE && !GI_HALFRES
if(s == 0)
{
    vptPixelActiveOutside = vptPixelActive;
    primaryRayMissed = primaryHitDistance < 0.0;
}
#endif


vec3 sampleColor =
    vec3(0.0);

#if defined(ORION_K2_INDIRECT_STAGE) && !SHARC_UPDATE && !defined(ORION_K2_INDIRECT_TRACE_ONLY)
// ③ K2-I：直射级交接图在分叉前整体载入一次（水支在源序上先于 RTAO 桩出现，
// GLSL 要求先声明后使用）。rgb = 直射三项和（或水的完整着色），a = rtaoFactor。
const vec4 k2DirectFromImage = imageLoad(k2DirectImage, pixelCoord);
#elif defined(ORION_K2_INDIRECT_STAGE)
const vec4 k2DirectFromImage = vec4(0.0, 0.0, 0.0, 1.0);
#endif

        // ==========================================
        // 步骤 2: ClosestHit 结果判定 (若未击中则输出天空)
        // ==========================================
if (primaryHitDistance < 0.0) {
#ifdef ORION_K1_GLASS_ONLY
    anyLandingStored = true;
    continue;
#endif
#if !defined(ORION_K2_TRANSPORT) && !defined(ORION_K1_GLASS_ONLY)
#if GI_HALFRES
    // 2026-10-24 半分辨率 GI 变体：天空像素没有间接光。写"零辐照度 + 极远深度"
    // 的哨兵后**立即 return** —— 不能继续落到 24156 起的 RR guide 写入，那些由
    // 主变体独占（两个 pass 同时写同一批 guide 是数据竞争，见 HANDOFF §6.6）。
    // 60000 是 16F 能表示的大值；深度权重 1/(1e-3+60000) 让相邻命中像素几乎不吃它。
    imageStore(
        giHalfImage,
        ivec2(gl_LaunchIDEXT.xy),
        vec4(0.0, 0.0, 0.0, 60000.0)
    );
    return;
#endif

#if RR_GUIDE_PURIFY_V1
    if(s == 0)
    {
        rrDiffuseAlbedo = vec3(0.0);
        rrSpecularAlbedo = vec3(0.0);
        rrNormalRoughness = vec4(normalize(-worldDir.xyz), 1.0);
        rrSpecularHitDistance = 65504.0;
    }
#endif

    sampleColor =
        unpackAlbedoUnorm(payload.albedoPacked);
#endif
// ④ K2 死分支摘除：miss 像素没有 landing（K1 只对主命中落盘），K2 在分叉处
// 已 continue，此分支体在 K2 变体不可达 ⇒ 编译期整段摘除（省寄存器/编译时间）。

} else {
#ifdef ORION_K1_PRIMARY
#ifdef ORION_K1_GLASS_ONLY
    if(getLegacyMaterialId(payload.state) < 2.5)
    {
        anyLandingStored = true;
        continue;
    }
#else
    // B①：K1 切点 —— 主命中组（地形/水/实体/粒子）只落身份，材质解析与
    // 后续传输整体延期给 K2（landing → continue）。玻璃（matId >= 2.5，由
    // 全功能 rt_test_translucent.rchit 写进 state）在 ORION_K1_DEFER_GLASS 下
    // 标为 a.x=0.25 交给压紧后的 k1-glass 腿，大幅瘦身 k1-primary。
    if(getLegacyMaterialId(payload.state) < 2.5)
    {
        ptLandingStore(
            ptLandingRecordIndex(
                uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
                s
            ),
            rtDebugPrimaryMotion
        );
        anyLandingStored = true;
        continue;
    }
#ifdef ORION_K1_DEFER_GLASS
    else
    {
        ptLandingMarkGlass(
            ptLandingRecordIndex(
                uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
                s
            ),
            rtDebugPrimaryMotion
        );
        anyLandingStored = true;
        continue;
    }
#endif
#endif
#endif
    vec3 hitPos =
        origin.xyz +
        worldDir.xyz *
        primaryHitDistance;
            vec3 albedo = unpackAlbedoUnorm(payload.albedoPacked);
            vec3 normal = payload.normal;
            float matId = getLegacyMaterialId(payload.state);
            // 2026-11-22 批3：叶道回填（0.25 = Java LEAVES/FLOWERS/草/作物打标）。
            // getLegacyMaterialId 对不透明恒回 0 ⇒ 顶点 materialId 只能经
            // material_resolve 的 g_hitVertexMaterialId 导出（resolveHitMaterial
            // 已在本 fork 中执行过，全局已就绪）。这一行点亮既有但一直死着的
            // 叶子背光/SSS 分支（:4703 一带的 matId 0.2~0.45 判据）。
            // 门控：全局只在 K2 内联材质路径（ORION_K2_MATERIAL_INLINE →
            // material_resolve）存在；K1/单发 K2/callable 路径不 include
            // material_resolve ⇒ 不引用。四腿/三腿/两腿档全是内联路径。
#if defined(ORION_K2_CONSUMER_MAT_REUSE_ONLY)
            if((payload.state & RT_STATE_FOLIAGE_BIT) != 0u)
            {
                matId = 0.25;
            }
#elif defined(ORION_K2_MATERIAL_INLINE)
            if((payload.state & RT_STATE_FOLIAGE_BIT) != 0u ||
               (g_hitVertexMaterialId > 0.2 && g_hitVertexMaterialId < 0.45))
            {
                matId = 0.25;
            }
#endif

// The geometric face normal, captured before POM, normal maps or rain
// displacement touch `normal`.  Rain surface classification must key off the
// axis-aligned face orientation rather than a shaded normal: a normal map can
// tilt normal.y below the threshold even on a perfectly horizontal ground
// face.
vec3 primaryGeometricNormal =
    payload.normal;

// SectionCompiler marks every solid axis-aligned exterior face with marker 23
// -- horizontal top faces and vertical wall faces alike.  VulkanCore carries
// that marker through the opaque closest-hit payload as glassAlpha = 0.25,
// while ordinary opaque faces retain 1.0.  The payload geometric normal is
// what separates the two orientations, so one marker covers both rain models
// and neither VulkanCore nor rt_test.rchit needs to know which is which.
bool rainSolidSurfaceCandidate =
    matId <= 0.5 &&
    primaryGlassAlpha > 0.0 &&
    primaryGlassAlpha < 0.5;

// Upward rain model: the existing puddle film and splash ripples.
bool rainTopSurfaceCandidate =
    rainSolidSurfaceCandidate &&
    primaryGeometricNormal.y > 0.5;

// Wall rain model: run-off film and slow droplets.  Only the two horizontal
// axes qualify, so undersides (normal.y < -0.5) enter neither model.
bool rainWallSurfaceCandidate =
    rainSolidSurfaceCandidate &&
    abs(primaryGeometricNormal.y) < 0.5;


// 水体折射与吸收管线保持独立。
// T1-2 payload materialId=3 is generic translucent terrain
// and must never enter the water path.
if (matId > 0.5 && matId < 2.5) {
#if (defined(ORION_K2_INDIRECT_STAGE) || defined(ORION_K2_DIRECT_LOCAL_STAGE)) && !SHARC_UPDATE && !defined(ORION_K2_INDIRECT_COMPOSE_ONLY)
    if(k2iCompactTraceActive)
    {
        return;
    }
#endif
#if SHARC_UPDATE || defined(ORION_K2_INDIRECT_TRACE_ONLY) \
    || defined(ORION_K2_DIRECT_LOCAL_STAGE) || defined(ORION_K2_DIRECT_SUN_STAGE_B)
    continue;
#endif
#if !defined(ORION_K1_PRIMARY) && !defined(ORION_K2_INDIRECT_TRACE_ONLY) \
    && !SHARC_UPDATE && !defined(ORION_K2_DIRECT_LOCAL_STAGE) && !defined(ORION_K2_DIRECT_SUN_STAGE_B)
// ④ K1 死分支摘除：K1 的主命中组（地形/水/实体/粒子）全部在切点落 landing 后
// continue，水像素永远走不到水分支体（水面着色整体延期给 K2）⇒ K1 编译期摘除。
#ifndef WATER_K2_GUIDE_PROBE_DEDUP
#define WATER_K2_GUIDE_PROBE_DEDUP 1
#endif
#if WATER_K2_GUIDE_PROBE_DEDUP && \
    (defined(ORION_K2_DIRECT_LOCAL_STAGE) || defined(ORION_K2_DIRECT_SUN_STAGE_B))
                // 2026-11-22 非时域性能优化 #2：K2-LOC / K2-SUNB 对水像素无直射
                // 附加输出、也不写 RR guide，原路径仍跑完 BRDF 与整条
                // RT_PURPOSE_DISTANCE_ONLY 射线后在尾部 continue 丢弃 ⇒ 直接早退。
                continue;
#endif
                vec3 viewDir = -worldDir.xyz;
                float cosTheta = clamp(dot(normal, viewDir), 0.0, 1.0);
                float R0 = 0.02;
                float fresnelX = 1.0 - cosTheta;
                float fresnelX2 = fresnelX * fresnelX;
                float fresnel = R0 + (1.0 - R0) * (fresnelX2 * fresnelX2) * fresnelX;

                // Geometric water normal (flat horizontal top/bottom or non-wave side wall)
                // used to keep ray-origin offsets and DLSS-RR guide switches wave-invariant.
                vec3 waterGeoN = (abs(normal.y) > 0.25)
                    ? vec3(0.0, (dot(worldDir.xyz, vec3(0.0, 1.0, 0.0)) < 0.0) ? 1.0 : -1.0, 0.0)
                    : normalize((dot(primaryGeometricNormal, worldDir.xyz) > 0.0)
                        ? -primaryGeometricNormal
                        : primaryGeometricNormal);
                float geoCosTheta = clamp(dot(waterGeoN, viewDir), 0.0, 1.0);
                float geoFresnelX = 1.0 - geoCosTheta;
                float geoFresnelX2 = geoFresnelX * geoFresnelX;
                float geoFresnel = R0 + (1.0 - R0) * (geoFresnelX2 * geoFresnelX2) * geoFresnelX;

                // =====================================================
                // RR-6.2 - Primary water guides
                // =====================================================
#if !WATER_K2_GUIDE_PROBE_DEDUP || \
    (!defined(ORION_K2_DIRECT_STAGE) && !defined(ORION_K2_DIRECT_SUN_STAGE) && \
     !defined(ORION_K2_DIRECT_SUN_STAGE_A) && !defined(ORION_K2_DIRECT_SUN_STAGE_B) && \
     !defined(ORION_K2_DIRECT_LOCAL_STAGE))
                if(s == 0)
                {
                    const float waterGuideRoughness =
                        WATER_SURFACE_SUN_ROUGHNESS;

                    rrDiffuseAlbedo =
                        vec3(0.0);

                    rrSpecularAlbedo =
                        rrEnvBRDFApprox2(
                            vec3(R0),
                            waterGuideRoughness *
                                waterGuideRoughness,
                            cosTheta
                        );

                    rrNormalRoughness =
                        vec4(
                            normalize(normal),
                            waterGuideRoughness
                        );

#if !defined(ORION_K2_INDIRECT_STAGE) && !defined(ORION_K2_DIRECT_LOCAL_STAGE) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_B)
                    // 2026-11-27: 删除冗余的 RR-6.2 DISTANCE_ONLY 探针射线，
                    // 改由下方反射瓣直接导出 rrSpecularHitDistance。
                    rrSpecularHitDistance = 65504.0;
#elif defined(ORION_K2_INDIRECT_STAGE)
                    rrSpecularHitDistance = ptLandingRecords[
                        ptLandingRecordIndex(
                            uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
                            s
                        )
                    ].d.w;
#endif
                }
#endif

#if defined(ORION_K2_INDIRECT_STAGE)
                // ③ K2-I：水像素的完整着色（反射/折射/太阳瓣/吸收）已由 K2-D
                // 在其水出口落进 k2DirectImage（rgb = 完整 sampleColor）。这里
                // 只保留上面的 s==0 水面 RR guide 段（无成本重算，guide 语义与
                // 主变体逐位一致），着色本体替换为图像载入。
                sampleColor = k2DirectFromImage.rgb;
#elif defined(ORION_K2_DIRECT_LOCAL_STAGE) || defined(ORION_K2_DIRECT_SUN_STAGE_B)
                // ④ K2-LOC / ⑤ K2-SUNB：水像素着色归 K2-SUN(A)，本变体在水
                // 出口只 continue（不 store —— k2DirectImage 里已是 K2-SUN(A)
                // 落的完整水色）。
                sampleColor = vec3(0.0);
#else
#ifndef WATER_UNDERWATER_SNELL_WINDOW
#define WATER_UNDERWATER_SNELL_WINDOW 1
#endif
#ifndef WATER_CAUSTIC_FRESNEL_EXIT
#define WATER_CAUSTIC_FRESNEL_EXIT 1
#endif
bool underwaterExit = false;
bool underwaterTir = false;
bool underwaterColumnWall = false;
float exitFresnel = 0.0;
vec3 waterSurfaceSunTransmittance = vec3(0.0);
float waterSurfaceCloudShadow = 0.0;
bool hasAboveTransmitted = false;
int waterLobeCount = 0;

vec3 lobe0Origin = vec3(0.0);
vec3 lobe0Dir = vec3(0.0, 1.0, 0.0);
bool lobe0InsideWater = false;
bool lobe0AllowGi = false;
bool lobe0SuppressSun = false;
bool lobe0IsRefl = false;

vec3 lobe1Origin = vec3(0.0);
vec3 lobe1Dir = vec3(0.0, 1.0, 0.0);
bool lobe1InsideWater = false;
bool lobe1AllowGi = false;
bool lobe1SuppressSun = false;
bool lobe1IsRefl = false;

#if WATER_UNDERWATER_SNELL_WINDOW
if(matId > 1.5 && cam.waterSimState.w != 0u)
{
    underwaterExit = true;
    const bool isVerticalWaterColumnWall = abs(waterGeoN.y) <= 0.5;
    if(isVerticalWaterColumnWall)
    {
        // 2026-11-27: 站在水柱内部向四周看竖直水柱侧壁：温和透射穿出空气，
        // 彻底消除四面 48.75° 全内反射黑/白牢笼。
        underwaterTir = false;
        underwaterColumnWall = true;
        vec3 colExitRefr = refract(worldDir.xyz, waterGeoN, 1.18);
        vec3 colExitDir = (dot(colExitRefr, colExitRefr) >= 1.0e-6)
            ? normalize(colExitRefr)
            : normalize(worldDir.xyz);
        exitFresnel = min(geoFresnel, 0.25);

        waterLobeCount = 1;
        lobe0Origin = hitPos - waterGeoN * 0.015;
        lobe0Dir = colExitDir;
        lobe0InsideWater = false;
        lobe0AllowGi = false;
        lobe0SuppressSun = false;
        lobe0IsRefl = false;
    }
    else
    {
        // Water -> Air horizontal surface viewed from underwater: Snell's Window + TIR.
        vec3 exitRefrDir = refract(worldDir.xyz, normal, 1.33);
        vec3 exitReflDir = normalize(reflect(worldDir.xyz, normal));

        if(dot(exitRefrDir, exitRefrDir) < 1.0e-6)
        {
            underwaterTir = true;
            waterLobeCount = 1;
            lobe0Origin = hitPos + waterGeoN * 0.015;
            lobe0Dir = exitReflDir;
            lobe0InsideWater = true;
            lobe0AllowGi = false;
            lobe0SuppressSun = true;
            lobe0IsRefl = true;
        }
        else
        {
            vec3 exitRefrDirN = normalize(exitRefrDir);
            float cosExitAir = clamp(dot(exitRefrDirN, -normal), 0.0, 1.0);
            float efX = 1.0 - cosExitAir;
            float efX2 = efX * efX;
            exitFresnel = R0 + (1.0 - R0) * (efX2 * efX2) * efX;

            waterLobeCount = 1;
            lobe0Origin = hitPos - waterGeoN * 0.015;
            lobe0Dir = exitRefrDirN;
            lobe0InsideWater = false;
            lobe0AllowGi = false;
            lobe0SuppressSun = false;
            lobe0IsRefl = false;

            if(exitFresnel > 0.06)
            {
                waterLobeCount = 2;
                lobe1Origin = hitPos + waterGeoN * 0.015;
                lobe1Dir = exitReflDir;
                lobe1InsideWater = true;
                lobe1AllowGi = false;
                lobe1SuppressSun = true;
                lobe1IsRefl = true;
            }
        }
    }
}
#endif

if(!underwaterExit)
{
    bool waterSurfaceSunOccluded = false;
    if(sunDir.y > 0.0)
    {
        waterSurfaceSunTransmittance =
            traceSunTransmittanceAir(
                hitPos + waterGeoN * 0.02 + sunDir * 0.001,
                sunDir
            );
#if WATER_SKY_SUN_OCCLUSION
        waterSurfaceSunOccluded =
            max(
                waterSurfaceSunTransmittance.r,
                max(
                    waterSurfaceSunTransmittance.g,
                    waterSurfaceSunTransmittance.b
                )
            ) <= 0.0;
#endif
    }
    waterSurfaceCloudShadow =
        (sunDir.y > 0.02 && !waterSurfaceSunOccluded)
            ? c3CloudSunTransmittance(hitPos + waterGeoN * 0.02 + sunDir * 0.001, sunDir)
            : 0.0;

    waterLobeCount = 1;
    lobe0Origin = hitPos + waterGeoN * 0.015;
    lobe0Dir = normalize(reflect(worldDir.xyz, normal));
    lobe0InsideWater = false;
    lobe0AllowGi = false;
    lobe0SuppressSun = true;
    lobe0IsRefl = true;

    vec3 refrDir =
        refract(
            worldDir.xyz,
            normal,
            1.0 / 1.33
        );
    if(dot(refrDir, refrDir) >= 1.0e-6)
    {
        hasAboveTransmitted = true;
        waterLobeCount = 2;
        lobe1Origin = hitPos - waterGeoN * 0.015;
        lobe1Dir = refrDir;
        lobe1InsideWater = true;
        lobe1AllowGi = false;
        lobe1SuppressSun = false;
        lobe1IsRefl = false;
    }
}

// Unified single-site water lobe loop: reflection never disappears and both lobes
// stay 100% deterministic and zero-noise.
vec3 lobe0Radiance = vec3(0.0);
vec3 lobe1Radiance = vec3(0.0);
WaterTransmissionInfo transmittedTransmissionInfo;
float reflHitDistFromLobe0 = 65504.0;
for(int waterLobe = 0; waterLobe < waterLobeCount; ++waterLobe)
{
    vec3 lobeOrigin = (waterLobe == 0) ? lobe0Origin : lobe1Origin;
    vec3 lobeDir = (waterLobe == 0) ? lobe0Dir : lobe1Dir;
    bool lobeInsideWater = (waterLobe == 0) ? lobe0InsideWater : lobe1InsideWater;
    bool lobeAllowGi = (waterLobe == 0) ? lobe0AllowGi : lobe1AllowGi;
    bool lobeSuppressSun = (waterLobe == 0) ? lobe0SuppressSun : lobe1SuppressSun;
    bool lobeIsRefl = (waterLobe == 0) ? lobe0IsRefl : lobe1IsRefl;
    bool setEntrySun = (!underwaterExit && waterLobe == 1);
    g_waterWalkerSkipClouds = lobeIsRefl;
    if(setEntrySun)
    {
        g_waterEntrySunValid = true;
        g_waterEntrySunTransmittance = waterSurfaceSunTransmittance;
        g_waterEntryCloudShadow = waterSurfaceCloudShadow;
    }
    WaterTransmissionInfo lobeInfo;
    vec3 lobeRad = tracePrimaryTransparentRadiance(
        lobeOrigin,
        lobeDir,
        lobeInsideWater,
        sunDir,
        lobeAllowGi,
        primaryHitDistance,
        hitPos,
        lobeSuppressSun,
        lobeIsRefl,
        lobeInfo
    );
    g_waterWalkerSkipClouds = false;
    if(setEntrySun)
    {
        g_waterEntrySunValid = false;
    }
    if(waterLobe == 0)
    {
        lobe0Radiance = lobeRad;
        if(lobeIsRefl)
        {
            reflHitDistFromLobe0 = lobeInfo.waterPathLength;
        }
    }
    else
    {
        lobe1Radiance = lobeRad;
        transmittedTransmissionInfo = lobeInfo;
    }
}

if(!underwaterExit && s == 0)
{
    rrSpecularHitDistance = reflHitDistFromLobe0;
}

if(underwaterExit)
{
    if(underwaterTir)
    {
        sampleColor = lobe0Radiance;
    }
    else if(underwaterColumnWall || waterLobeCount <= 1)
    {
        sampleColor = mix(
            lobe0Radiance,
            waterScatterColorAt(hitPos.xz) * getSurfaceAmbientRadiance() * 0.65,
            exitFresnel
        );
    }
    else
    {
        sampleColor = mix(lobe0Radiance, lobe1Radiance, exitFresnel);
    }
}
else
{
    vec3 reflectedRadiance = lobe0Radiance;
    if(!hasAboveTransmitted)
    {
        sampleColor = reflectedRadiance;
    }
    else
    {
        vec3 transmittedRadiance = lobe1Radiance;

                    // Below the Fresnel crossover this pixel is primarily
                    // the lit receiver seen through water, so RR must not use
                    // the water-surface depth/normal as its reconstruction
                    // guide.
                    if(
                        s == 0 &&
                        transmittedTransmissionInfo.reachedUnderwaterReceiver > 0.5 &&
                        geoFresnel < 0.5
                    )
                    {
                        applyTransparentReceiverRRGuides(
                            transmittedTransmissionInfo,
                            rrDiffuseAlbedo,
                            rrSpecularAlbedo,
                            rrNormalRoughness,
                            rrSpecularHitDistance
                        );

                        rrTransparentReceiverWorldPos =
                            transmittedTransmissionInfo.receiverWorldPos;

                        rrTransparentReceiverGuideValid =
                            true;
                    }

                    sampleColor =
                        mix(
                            transmittedRadiance,
                            reflectedRadiance,
                            fresnel
                        );

                    // 2026-10-06 近水降本：焦散的距离闸（常量见 caustics 常量块）。
                    // 2026-10-08（B）：再乘上帧时间反馈环 —— 帧时间越紧，焦散越早
                    // 停算（未启用时 scale = 1.0 ⇒ 与改动前逐位相同）。
#if WATER_CAUSTIC_FRESNEL_EXIT
                    if(fresnel < 0.95 &&
                       primaryHitDistance <
                       WATER_CAUSTIC_MAX_DISTANCE * perfL10WaterAdaptiveScale())
                    {
                        g_waterEntrySunValid = true;
                        g_waterEntryCloudShadow = waterSurfaceCloudShadow;
                        sampleColor +=
                            evaluatePrimaryWaterReceiverCaustics(
                                hitPos,
                                transmittedTransmissionInfo,
                                sunDir
                            ) * (1.0 - fresnel);
                        g_waterEntrySunValid = false;
                    }
#else
                    if(primaryHitDistance <
                       WATER_CAUSTIC_MAX_DISTANCE * perfL10WaterAdaptiveScale())
                    {
                        g_waterEntrySunValid = true;
                        g_waterEntryCloudShadow = waterSurfaceCloudShadow;
                        sampleColor +=
                            evaluatePrimaryWaterReceiverCaustics(
                                hitPos,
                                transmittedTransmissionInfo,
                                sunDir
                            );
                        g_waterEntrySunValid = false;
                    }
#endif

                    // 2026-10-22 A+B：SSS / 波峰透光的太阳门控改口径。
                    float waterSssCloudShadow =
                        (cam.waterParams.x > 0.001 && sunDir.y > 0.02)
                            ? waterSurfaceCloudShadow
                            : 0.0;

                    // 批次3-P1：波峰透光（透射光的窄前向散射瓣，按 (1-F) 计权
                    // —— HP 的 diffT 走透射侧、出射菲涅尔单独应用，同构）。
                    sampleColor +=
                        evaluateWaterCrestBacklit(
                            hitPos,
                            normal,
                            worldDir.xyz,
                            sunDir,
                            transmittedTransmissionInfo
                        ) * (1.0 - fresnel) *
                        waterSurfaceSunTransmittance *
                        waterSssCloudShadow;

                    // 批次3-P1b（2026-10-03 参考重构）：薄层 SSS —— 补上参考 diffT
                    // 的另一半（宽相位 g≈0.8 + 线性/非线性双光程 + ×2 能量补偿 +
                    // G_sss = 1−NdotL）。与 P1 同为透射侧，同样乘出射菲涅尔。
                    sampleColor +=
                        evaluateWaterThinLayerSSS(
                            hitPos,
                            normal,
                            worldDir.xyz,
                            sunDir,
                            transmittedTransmissionInfo
                        ) * (1.0 - fresnel) *
                        // 2026-10-22 太阳可见性门控（用户：「水的次表面散射在
                        // 无太阳光环境下应该看不见」）：薄层 SSS 吃的是
                        // getTimeDrivenSkyLight 的满功率太阳辐射，洞穴水体
                        // 与云影下的水面之前照样发光。现改为「水面点实测的彩色
                        // 太阳透射 × 云影」两项相乘。
                        waterSurfaceSunTransmittance *
                        waterSssCloudShadow;

                    // 批次3-P1c（2026-10-03 参考重构第 2 步余项）：**多散射 / 前向散射**。
                    if(transmittedTransmissionInfo.reachedUnderwaterReceiver > 0.5)
                    {
#if WATER_VOLUME_INSCATTER_V2
                        const float kWaterMultiScatterScale = 0.9;
#else
                        const float kWaterMultiScatterScale = 2.0;
#endif
                        vec3 waterHitScatter = waterScatterColorAt(hitPos.xz);
                        vec3 waterMultiExtinction =
                            waterAbsorptionColorAt(hitPos.xz) + waterHitScatter;
                        float waterMultiPath = clamp(
                            transmittedTransmissionInfo.waterPathLength, 0.05, 60.0);
                        vec3 waterMultiShare = vec3(1.0) -
                            exp(-waterMultiExtinction * waterMultiPath);
                        vec3 waterMultiSource = max(
                            transmittedRadiance -
                                transmittedTransmissionInfo.receiverSelfEmissionRadiance,
                            vec3(0.0));
                        sampleColor += waterMultiSource *
                            waterHitScatter * waterScatteringAmount() *
                            waterMultiShare *
                            (kWaterMultiScatterScale * (1.0 - fresnel));
                    }
                }

                // The reflected/transmitted walkers do not contain an
                // explicit directional solar lobe.  Add the shadow-correct
                // water-surface GGX glint once after their Fresnel combination.
                // Visibility was traced once above (waterSurfaceSunTransmittance).
                sampleColor +=
                    evaluatePrimaryWaterSunLighting(
                        hitPos,
                        normal,
                        viewDir,
                        sunDir,
                        waterSurfaceSunTransmittance
                    );
}

                // =====================================================
                // 2026-10-26 Persistent foam, sampled once (Crest route B).
                //
                // One texture fetch + two ALU-noise octaves per water pixel; no
                // extra BVH ray.  The map itself is advanced by water_foam.comp
                // at 30 Hz (fade + whitecap / interaction / shoreline injection).
                //
                // Foam is a diffuse layer: sun visibility and cloud shadow use
                // the same values as the water glint above, so foam in a cave
                // or under a cloud does not glow.  The 0.45 ambient term keeps
                // shaded foam readable instead of black.
                //
                // 2026-10-26 21:1x 用户不要泡沫 ⇒ 回退：整块采样/着色屏蔽保留，
                // 水面回到"只有反射+折射+烧结"的旧观感。
                // 2026-11-16 清理：随泡沫函数族一并删除（见 rgen_water_caustics
                // 的清理注释）；git 历史可找回。
                // =====================================================

#endif // ORION_K2_INDIRECT_STAGE（③：水着色半段归 K2-D，K2-I 用图像载入替换）

#if defined(ORION_K2_DIRECT_STAGE) || defined(ORION_K2_DIRECT_SUN_STAGE) \
    || defined(ORION_K2_DIRECT_SUN_STAGE_A)
                // ③ K2-D 水出口：水像素的着色在此完整结束（镜面反射/天空/
                // 折射全在 sampleColor 里），落 k2DirectImage 后交棒 K2-I。
                // a = 0：水像素不消费 rtaoFactor（K2-I 水支直接取 rgb）。
                // ④ K2-SUN 同一出口（水着色半段归 SUN）。
                // ⑤ K2-SUNA 同一出口（四腿拆分后水着色半段归 SUNA）。
                imageStore(
                    k2DirectImage,
                    pixelCoord,
                    vec4(sampleColor, 0.0)
                );
                if(s == 0)
                {
                    ptLandingRecords[
                        ptLandingRecordIndex(
                            uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
                            s
                        )
                    ].d.w = rrSpecularHitDistance;
                }
                continue;
#elif defined(ORION_K2_DIRECT_LOCAL_STAGE) || defined(ORION_K2_DIRECT_SUN_STAGE_B)
                // ④ K2-LOC / ⑤ K2-SUNB 水出口：不 store —— k2DirectImage 已由
                // K2-SUN(A) 落完整水色，本腿对水像素无直射附加项。
                continue;
#endif

#endif
            }
            // =====================================================
            // T1-2 Generic translucent terrain
            //
            // Vertex materialId=2 (confirmed T1-1) is translated by
            // rchit to payload materialId=3 for normal faces and 4 for
            // the physical bottom face, so neither can collide with the
            // existing water back-face payload value 2.
            //
            // Straight transmission only: no refraction.
            // =====================================================
else if(
    matId >= 2.5 &&
    matId < 4.5
)
{
#if !defined(ORION_K2_TRANSPORT) && !defined(ORION_K1_DEFER_GLASS)
                GlassSurfaceInfo glassSurface;
                #if GI_RR_DELTA_FLAG
                // (a) 这个像素的颜色来自"穿过玻璃"的路径：写进 RR input 的 diffuse-albedo alpha
                // 这条"跨 delta"标记，只有跨过 delta 才用邻居法线差放宽核半径）。
                rrDeltaCrossedFlag = 1.0;
                #endif

                // 2026-10-05：走查曾发 CLASSIFY 短射线并覆写 payload；
                // v3 弯折射线模型不再发任何 trace，但 profile 提取保留
                //（trace 调用参数使用局部量，语义不变）。
                // 这里是**命中后**的材质状态（profile / clear-glass 位由材质体写），
                // 延迟模式下存在局部 payload.state 里，不是身份载荷。
                const int glassPayloadProfile =
                    int(getTransparentProfile(payload.state));

                // 2026-10-07（用户要求）：普通玻璃板/玻璃块「当空气」。这一位由 rchit 在
                // 命中时按"顶点色里有没有注入染料"置好（见 isClearGlassHit 的完整说明）。
                // 染色玻璃恒 false ⇒ 继续走镜面反射 + 薄片染色那套，行为逐位不变。
                const bool clearGlassAsAir =
                    isClearGlassHit(payload.state);

                // =====================================================
                // path/transmission/shade.glsl 的折射模型）
                //
                // v2 教训：物理正确的"两次折射 + 侧面 TIR 弹射 + CLASSIFY
                // 邻格判定 + 预算回退"在 MC 方块世界里反而更糟 —— 平行板
                //
                //   入射面折射**一次**得到弯折方向，透射频线从入射点沿
                //   **弯折方向** trace，并用 gl_RayFlagsCullBackFacingTrianglesEXT
                //   剔除玻璃自身所有背面 ⇒ 玻璃块/墙在一条 trace 内被跳过，
                //   直接落在接收器/天空/下一块玻璃正面。没有第二次折射、
                //   没有 TIR 弹射、没有出射点计算 —— 透射内容按弯折射线
                //   采样（经典屏幕空间折射观感，位移减半且连续）。
                //
                // 本引擎的等价物：PASSTHROUGH any-hit 本来就忽略一切玻璃
                //，所以透射频线只需从入射点沿弯折
                // 染料：首格 dye^(厚度/cosInside)，既有 glassSlabInsidePathLength
                // 修正不变。多格墙的第二格
                // 染色不累计（透射频线忽略玻璃）—— 与 v2 之前的旧档一致，
                // 从空气入射不存在全内反射 ⇒ refract 必然成功；护栏留给
                // 数值极端（失败回退直射，逐位旧档）。
                // =====================================================
                // 2026-10-07：普通玻璃当空气 ⇒ **不做平板出射点位移**（出射点就是命中点），
                // 透射频线从原处沿原方向起飞，等价于这一片玻璃不存在。染色玻璃维持原档
                //（解析平板出射点 + 可选 -DGI_GLASS_WALK 弯折）。
                // 2026-10-17：逐材质折射率（参考 GLASS_COVERAGE_20260914.md：
                // 玻璃 1.5、冰 1.309；粘液/蜂蜜参考未给 ⇒ 沿用 1.5）。
                const float glassIor =
                    glassIorForProfile(glassPayloadProfile);
                // =====================================================
                // 2026-10-28：起点直接用**入射点**，撤掉解析出射点位移。
                //
                // 现象：出射面与入射面**平行**时干净（正对玻璃块 / 玻璃板），
                //   不平行时（斜看玻璃块的侧面）整面固定形状块状噪点。
                //
                // 机理：glassSlabExitPoint 的最后一步把出射点 clamp 回入射格
                //   （glass_dielectric_common.glsl:176-180，"修透过玻璃穿墙"）。
                //   它假设内部光线一定走到对面；斜穿/掠射时
                //       exitPoint = entry + T * (thickness / cosInside)
                //   立刻冲出格子 ⇒ 被夹到格边界。而"夹/不夹"是**二值**的，
                //   逐像素切换 ⇒ 链起点在"真实出射点"与"格边界"之间跳
                //   ⇒ 透过玻璃看到的背景被切成块。平行进出面时横移是常数、
                //   不会冲出格子 ⇒ 不夹 ⇒ 干净。
                //
                // 为什么现在不需要位移：2026-10-25 阶段 D 之后，主透射链的
                //   any-hit 会 ignore 掉每一片玻璃面，射线本来就沿真实几何穿过
                //   玻璃体。出射点位移属于"单射线跳过整块玻璃"的旧模型
                //   （v2 弯折 / PASSTHROUGH），现在只会把起点推离真实光路。
                //
                // A/B：-DORION_GLASS_STRAIGHT_START=0 回到解析出射点位移。
                // =====================================================
#ifndef ORION_GLASS_STRAIGHT_START
#define ORION_GLASS_STRAIGHT_START 1
#endif
#if ORION_GLASS_STRAIGHT_START
                vec3 glassWalkExitPosition = hitPos;
#else
                vec3 glassWalkExitPosition = clearGlassAsAir
                    ? hitPos
                    : (primaryGlassUsesDielectric
                        ? glassSlabExitPoint(
                            primaryGlassEntryPoint,
                            primaryGlassExitNormal,
                            worldDir.xyz,
                            primaryGlassSlabThickness,
                            glassIor
                        )
                        : hitPos + worldDir.xyz * primaryParallaxRayTravel);
#endif
                vec3 glassWalkExitDirection = worldDir.xyz;
// =====================================================
// 2026-10-17：v3 弯折射线此前**没有默认值** ⇒ 从未生效
//
// 10-05 的交付说明写"GI_GLASS_WALK 默认 1"，但源码里只有 `#if GI_GLASS_WALK`，
// 全仓库没有任何 `#ifndef GI_GLASS_WALK / #define`，而 _deploy_stale_spv.py
// 编译时不传这个宏 ⇒ `#if` 恒为 0，弯折那段从来没进过编译（预处理后无
// walkInsideDir）。现网一直跑的是 10-01 的平板档：解析出射点被**夹回入射格**
// ⇒ 位移在格边整体跳变、大角度被截断在 ~0.45 格（用户报的"折射模型很怪"）。
//
// 这里补上默认 1，让源码与交付口径一致（默认值定义在文件前部 5200 附近）。
// A/B：-DGI_GLASS_WALK=0 ⇒ 逐位回到平板档（不做弯折方向，只留出射点位移）。
// =====================================================
#if GI_GLASS_WALK && !VPT_GLASS_DIELECTRIC_REWRITE
                // 2026-11-13：弯折块只服务**回退档**（-DVPT_GLASS_DIELECTRIC_REWRITE=0）
                // 的续走查语义（出射点/出射方向 = 介质内折射后的续射线）。
                // 重写档必须禁用：tracePrimaryDielectricGlassRadiance 把这个方向当
                // **入射视线**用（viewDirection = normalize(rayDirection)，入射
                // Fresnel、镜像反射方向、整条透射走查全由它出发，内部自己做折射）。
                // 喂折射方向 ⇒ 透射射线被折向面法线（竖直面 ⇒ 贴地平飞），放在
                // 平地上的半透明体底边一排像素打不到地面、直接命中天空
                // ⇒ 用户实测的"底边天空色亮线"（2026-11-13 截图）。
                // 普通玻璃（块/板）自 2026-10-07 起"当空气"：不做出射点位移，也不
                // 做弯折。这段写于 10-05（早于 clearGlassAsAir 引入），只补宏会把它
                // 一起弯折 ⇒ 必须带上这个闸门，否则会推翻 10-07 的既定口径。
                if(!clearGlassAsAir && primaryGlassUsesDielectric)
                {
                    vec3 walkInsideDir =
                        refract(worldDir.xyz, normalize(normal), 1.0 / glassIor);
                    if(dot(walkInsideDir, walkInsideDir) > 1.0e-6)
                    {
                        glassWalkExitPosition =
                            primaryGlassEntryPoint + walkInsideDir * 0.01;
                        glassWalkExitDirection = normalize(walkInsideDir);
                    }
                }
#endif

                // 2026-10-05：glassSurface.position 回归**入射面命中点**语义
                //（反射线与太阳 GGX 高光的起点）。旧电介质模型把出射点塞在
                // 这里，反射线从介质内部出发自命中玻璃 ⇒ 表面反射恒黑。
                glassSurface.position = hitPos;
                glassSurface.normal = normal;
                glassSurface.tint = albedo;
                // 2026-10-26：传 texture x vertex 的 alpha（Remix-MC
                // ATLAS_TRANSLUCENT_BLEND = textureTimesVertexColor），
                // 而不是只按顶点 materialId 判定的覆盖率字段。
                glassSurface.alpha = primaryGlassBlendAlpha;
                const vec3 glassF0 = vec3(0.04);

                float glassNdotV =
                    clamp(
                        abs(
                            dot(
                                glassSurface.normal,
                                normalize(-worldDir.xyz)
                            )
                        ),
                        0.0,
                        1.0
                    );

                // =====================================================
                // RR guide policy - hybrid guide for generic glass
                //
                // One RR call, one guide record, and a pane whose content comes
                // from two depths.  The split below is deliberate:
                //
                //   pane identity    - specular F0 (glassF0) and the real
                //                      scene-reflection hit distance.  Both are
                //                      the pane's own, always.
                //   transmitted side - diffuse albedo, and under
                //                      GI_GLASS_RECEIVER_GUIDE_MODE >= 1 also
                //                      normal + roughness, come from the
                //                      behind-glass receiver once the ray
                //                      actually reached one (merge below).
                //
                // Both halves are needed.  With rrDiffuseAlbedo=0 and the depth
                // at the pane, DLSS RR classified the transmitted content as
                // distant specular (reprojected at the pane's motion / 65504)
                // and smeared it.  But the opposite extreme is not free either:
                // the single normal-roughness record has to serve the pane's
                // mirror lobe AND the rough receiver at once, so at mode 2 the
                // pane's own reflection rides a rough guide and softens.  That
                // is the coupling RR_STABLE_GLASS_RADIANCE exists to break, by
                // taking the pane's closed-form specular out of RR entirely and
                // adding it back in the display transform (see its banner).
                //
                // Water keeps its own full receiver-guide switch; this hybrid
                // targets generic glass only.
                // =====================================================


                if(s == 0)
                {
                    rrDiffuseAlbedo =
                        vec3(0.0);


#if VPT_GLASS_ALPHA_BLEND_PANE_SPECULAR
                    rrSpecularAlbedo =
                        rrEnvBRDFApprox2(
                            glassF0,
                            0.0,
                            glassNdotV
                        );
#else
                    // 2026-10-27：镜面反射已在渲染侧关掉（PANE_SPECULAR=0），
                    // RR 的 specular guide 必须同样为 0。
                    //
                    // 症状对照：原来这一行让 rrSpecularAlbedo 随 NdotV 变化 ——
                    //   正对面 NdotV≈1 ⇒ 0.04（几乎无镜面）⇒ 背景能透出来
                    //   侧面   NdotV≈0 ⇒ 1.0（纯镜面）  ⇒ RR 走镜面通道 ⇒ 发实
                    // 这正是"只有正对面透明、邻面一直不透明"的机制。
                    rrSpecularAlbedo =
                        vec3(0.0);
#endif


                    rrNormalRoughness =
                        vec4(
                            glassSurface.normal,
                            0.0
                        );


                    rrSpecularHitDistance =
                        65504.0;
                }


                WaterTransmissionInfo glassTransmissionInfo;

#if RR_STABLE_GLASS_RADIANCE
                // `sampleColor` still carries the whole pane so every
                // downstream consumer (DH far field, underwater column) sees
                // the same radiance it always did.  This holds the analytic
                // half separately; the subtraction happens once, on the final
                // colour, just before the RR input is written.
                vec3 paneStableRadiance =
                    vec3(0.0);
#endif

                sampleColor =
                    tracePrimaryGenericTransparentRadiance(
                        glassSurface.position,
                        glassWalkExitPosition,
                        glassWalkExitDirection,
                        glassSurface.normal,
                        glassSurface.tint,
                        glassSurface.alpha,
                        primaryRoughness,
                        glassPayloadProfile,
                        clearGlassAsAir,
                        sunDir,
                        // Raw pane hit (no POM travel) = the history key that
                        // reprojects exactly; glassSurface.position adds the
                        // per-texel relief travel and lands in the relief
                        // pattern's own failures.
                        hitPos,
#if GI_GLASS_RECEIVER_INDIRECT
                        primaryHitDistance,
#endif
                        glassTransmissionInfo
#if RR_STABLE_GLASS_RADIANCE
                        , paneStableRadiance
#endif
                    );

#if DBG_GLASS_HISTORY_PROBE
                // The receiver path filled dbgGlassHistoryProbe for this pixel;
                // show it instead of the pane's colour.
                dbgGlassHistoryProbeWanted = true;
#endif


                // =====================================================
                // RR guide merge - pane surface identity (kept) +
                // transmitted content guide (from the behind-glass
                // receiver).  Run only on the first sample; the guide
                // images are stored once per pixel after the loop.
                // =====================================================
                if(s == 0)
                {
                    // Real glass scene-reflection distance.  The pane's
                    // reflection ray was traced inside
                    // tracePrimaryGenericTransparentRadiance above and
                    // exported its hit distance, so RR can reproject the
                    // reflected content at the correct depth (opaque
                    // surfaces already do this via
                    // glossySpecularHitDistance).  Replaces the previous
                    // hard-coded 65504 that made the pane's specular read
                    // as a sky/infinity reflection and blur.
                    rrSpecularHitDistance =
                        rrGlassReflectionHitDistance;

                    // 按两瓣能量解调（参考实现 nrdDemodulate 的等价物）。
                    // 旧档这一格写 0 是因为那时的玻璃**没有**反射能量；
                    // 重写后反射是真的，必须如实告诉 RR。
#if VPT_GLASS_DIELECTRIC_REWRITE && !RR_STABLE_GLASS_RADIANCE
                    // 重写档：反射是真的能量，按 Fresnel 解调给 RR。
                    rrSpecularAlbedo =
                        vec3(clamp(rrGlassPaneFresnel, 0.0, 1.0));
#else
                    // 旧档玻璃没有反射能量（PANE_SPECULAR=0）⇒ 镜面 albedo 必须为 0。
                    rrSpecularAlbedo = vec3(0.0);
#endif

#if RR_STABLE_GLASS_RADIANCE
                    // Hold the analytic half for the display transform.
                    rrStableGlassRadiance =
                        paneStableRadiance;
#endif

                    // Borrow the transmitted-content guide from the
                    // behind-glass receiver: diffuse albedo + depth so RR
                    // can reproject the transmitted light (sharp).  The
                    // specular-F0 and the reflection hit distance above stay
                    // the pane's own, so the pane keeps reading as a solid
                    // surface instead of fogging into a gas cloud.  Normal
                    // and roughness, on the other hand, are the receiver's
                    // under GI_GLASS_RECEIVER_GUIDE_MODE >= 1 -- see the mode
                    // switch below -- because that is what lets RR filter
                    // what is behind the pane in the first place.
                    if(
                        glassTransmissionInfo.reachedUnderwaterReceiver
                            > 0.5
                    )
                    {
                        int receiverF0MetalCode =
                            decodeF0MetalCode(
                                glassTransmissionInfo.receiverF0Metal
                            );

                        float receiverMetallic =
                            decodeMetallicFromCode(
                                receiverF0MetalCode
                            );

                        // 只乘接收面 albedo 是错的：这个像素的颜色是
                        // throughput x 接收面着色，解调用的 albedo 必须同乘
                        // throughput（含 (1-F)、染色、eta^2），否则 RR 会把
                        // 玻璃的透过率当成光照变化。
                        rrDiffuseAlbedo =
                            glassTransmissionInfo.receiverAlbedo *
                            (1.0 - receiverMetallic) *
                            clamp(rrGlassPaneTransmissionFilter,
                                vec3(0.0), vec3(1.0));

                        // Surface record for the transmitted content.
                        //   mode 0 - pane normal + 0.0 roughness, left as
                        //            written above (the grain stays).
                        //   mode 1 - pane normal + receiver roughness.
                        //   mode 2 - receiver normal + receiver roughness.
                        //   mode 3 - pane normal + PANE_SOLID_ROUGHNESS, the
                        //            solid-pane default: the pane keeps its
                        //            own (sharp) reflection guide and gives NRD
                        //            a narrow kernel instead of either a mirror
                        //            exclusion or a 30-block-wide wall kernel.
#if RR_STABLE_GLASS_RADIANCE
                        // 反射已从 RR 输入里摘出 ⇒ 这份记录只描述透射内容，
                        // 直接用接收面自己的法线/粗糙度（参考实现的 stable plane
                        // 就是"每个端点各自一份记录"）。
                        rrNormalRoughness =
                            vec4(
                                normalize(glassTransmissionInfo.receiverNormal),
                                rrTransmittedGuideRoughness(
                                    glassTransmissionInfo.receiverRoughness)
                            );
                        rrTransparentReceiverWorldPos =
                            glassTransmissionInfo.receiverWorldPos;
#elif ORION_GLASS_REMIX_PSTR_GUIDES
                        const bool glassPstrTransmissionDominant =
#if !defined(ORION_K2_TRANSPORT) && ORION_GLASS_RADIANCE_DOMINANCE_FIX
                            !g_glassReflectionDominatesPixel;
#else
                            true;
#endif
                        if(glassPstrTransmissionDominant)
                        {
                            rrNormalRoughness = vec4(
                                normalize(glassTransmissionInfo.receiverNormal),
                                rrTransmittedGuideRoughness(glassTransmissionInfo.receiverRoughness)
                            );
                            rrTransparentReceiverWorldPos =
                                origin.xyz +
                                worldDir.xyz * (primaryHitDistance + max(glassTransmissionInfo.waterPathLength, 0.0));
                            rrGlassPstrActive = true;
                        }
                        else
                        {
                            rrNormalRoughness = vec4(glassSurface.normal, PANE_SOLID_ROUGHNESS);
                            rrTransparentReceiverWorldPos = glassTransmissionInfo.receiverWorldPos;
                            rrGlassPstrActive = false;
                        }
#elif GI_GLASS_RECEIVER_GUIDE_MODE == 3
                        rrNormalRoughness =
                            vec4(
                                glassSurface.normal,
                                PANE_SOLID_ROUGHNESS
                            );
                        rrTransparentReceiverWorldPos =
                            glassTransmissionInfo.receiverWorldPos;
#elif GI_GLASS_RECEIVER_GUIDE_MODE >= 1
                        vec3 transmittedGuideNormal =
#if GI_GLASS_RECEIVER_GUIDE_MODE >= 2
                            normalize(glassTransmissionInfo.receiverNormal);
#else
                            glassSurface.normal;
#endif

                        rrNormalRoughness =
                            vec4(
                                transmittedGuideNormal,
                                rrTransmittedGuideRoughness(
                                    glassTransmissionInfo.receiverRoughness
                                )
                            );
                        rrTransparentReceiverWorldPos =
                            glassTransmissionInfo.receiverWorldPos;
#else
                        rrTransparentReceiverWorldPos =
                            glassTransmissionInfo.receiverWorldPos;
#endif

                        rrTransparentReceiverGuideValid =
                            true;

                        // This receiver sits behind a pane, not water.
                        // GI_GLASS_PANE_DEPTH_GUIDE keeps the pane's own
                        // depth in the RR depth image for it.
                        rrTransparentReceiverIsPane =
                            true;
                    }


// 2026-11-03：玻璃后面是天空/远场（没有接收面）时，上面那两条 albedo
                    // 会同时为 0 —— RR 按 albedo 反解调再回乘，0 会被它的下限夹住，
                    // 归一化辐亮度爆掉后回乘 ≈ 0 ⇒ 整块玻璃发黑（用户实测现象）。
                    // 这里用透射滤镜本身当 albedo：内容是 throughput × 天空，
                    // 按 throughput 反解调得到的正是天空辐射，物理上自洽。
#if VPT_GLASS_DIELECTRIC_REWRITE
                    if(!rrTransparentReceiverGuideValid)
                    {
                        rrDiffuseAlbedo =
                            clamp(
                                rrGlassPaneTransmissionFilter,
                                vec3(0.05),
                                vec3(1.0));
#if !RR_STABLE_GLASS_RADIANCE && (GI_GLASS_RECEIVER_GUIDE_MODE == 3)
                        rrNormalRoughness =
                            vec4(
                                glassSurface.normal,
                                PANE_SOLID_ROUGHNESS
                            );
#endif
                    }
#endif

#if GI_GLASS_DOMINANT_GUIDE && !RR_STABLE_GLASS_RADIANCE && (GI_GLASS_RECEIVER_GUIDE_MODE != 3)
                        // Route A: the merge above assumed transmission owns
                        // this pane pixel.  When the mirror lobe actually
                        // carries more energy, that assumption is what makes a
                        // pane read as a soft film -- the reflected endpoint,
                        // which is at a completely different depth and is a
                        // completely different material, inherits the receiver's
                        // rough guide.  Re-assert the reflected endpoint's own
                        // record instead, the same way the reference renderer's
                        // dominant plane owns the screen guides.
                        //
                        // Runs last, so whichever lobe wins here is the one NRD
                        // reprojects against.  Uses the lobe weights the trace
                        // exported -- main cannot see the trace's locals.
                        if(
                            glassLobeWeightsValid &&
                            glassReflectionReceiverValid &&
                            dot(
                                glassLobeReflectionWeight,
                                vec3(0.2126, 0.7152, 0.0722)
                            )
                                >=
                            dot(
                                glassLobeTransmissionWeight,
                                vec3(0.2126, 0.7152, 0.0722)
                            )
                        )
                        {
                            rrNormalRoughness =
                                vec4(
                                    normalize(glassReflectionReceiverNormal),
                                    glassReflectionReceiverRoughness
                                );

                            // The transmitted receiver's albedo describes the
                            // wrong surface for a mirror-dominated pixel; use
                            // the reflected surface's own albedo instead.
                            rrDiffuseAlbedo =
                                glassReflectionReceiverAlbedo;

                            // Drop the behind-glass receiver guide: for a
                            // mirror-dominated pixel nothing downstream should
                            // reproject the pane's content against the surface
                            // behind it.
                            rrTransparentReceiverGuideValid =
                                false;
                        }
#endif
#ifndef ORION_GLASS_RECEIVER_MV
// 2026-10-25 阶段 A（修正版）：接收面运动矢量。置 0 = 沿用玻璃面的 MV（旧档）。
#define ORION_GLASS_RECEIVER_MV 0
#endif
#if !GI_HALFRES && ORION_GLASS_RECEIVER_MV
                        // =====================================================
                        // 运动矢量必须跟随**这张像素最终的深度 guide**：
                        //   · 透射支配的像素 => 深度在 24529 起的
                        //     rrUseReceiverDepthGuide 里被换成接收面 ⇒ MV 也要用接收面；
                        //   · 镜面支配的像素（上面的 dominant guide）=> valid 已被置
                        //     false、深度保持玻璃面 ⇒ MV 必须保持玻璃面。
                        // 之前把覆盖写在 dominant guide 之前、且不看 valid，镜面支配的
                        // 像素就成了"玻璃面的深度 + 接收面的位移" ⇒ RR 重投影错位
                        // ⇒ 玻璃发虚（2026-10-25 用户实测）。
                        // 判据与宿主 24529-24538 逐字同源。
                        // 半分辨率 GI 变体不写 G-buffer，故 !GI_HALFRES。
                        // =====================================================
                        bool receiverOwnsDepthGuide =
                            rrTransparentReceiverGuideValid;
#if GI_GLASS_PANE_DEPTH_GUIDE
                        if(rrTransparentReceiverIsPane)
                        {
                            receiverOwnsDepthGuide = false;
                        }
#endif
                        if(receiverOwnsDepthGuide &&
                           cam.perfAblationStage.z != 0u &&
                           dot(glassTransmissionInfo.receiverWorldPos,
                               glassTransmissionInfo.receiverWorldPos) > 1.0e-8)
                        {
                            const mat4 receiverPrevViewProj =
                                cam.prevViewProj;
                            const mat4 receiverCurViewProj =
                                cam.viewProj;

                            const vec4 receiverPrevClip =
                                receiverPrevViewProj *
                                vec4(glassTransmissionInfo.receiverWorldPos, 1.0);
                            const vec4 receiverCurClip =
                                receiverCurViewProj *
                                vec4(glassTransmissionInfo.receiverWorldPos, 1.0);

                            if(abs(receiverPrevClip.w) > 1.0e-6 &&
                               abs(receiverCurClip.w) > 1.0e-6)
                            {
                                const vec2 receiverPrevNdc =
                                    receiverPrevClip.xy / receiverPrevClip.w;
                                const vec2 receiverCurNdc =
                                    receiverCurClip.xy / receiverCurClip.w;

                                imageStore(
                                    dlssMotionImage,
                                    pixelCoord,
                                    vec4(
                                        (receiverPrevNdc - receiverCurNdc) *
                                            vec2(0.5, -0.5),
                                        0.0,
                                        1.0
                                    )
                                );
                            }
                        }
#endif
                }
#endif
// ④ K2 死分支摘除：玻璃（matId>=2.5）由全功能 translucent rchit 写进 state，
// K1 切点不落 landing ⇒ 玻璃像素 landing valid=0，K2 在分叉处 continue，
// 玻璃主命中链（GI_GLASS_WALK 全套）在 K2 变体不可达 ⇒ 编译期整段摘除。
            }
            // 不透明方块的 PBR 核心管线
            else {
#ifndef ORION_K1_PRIMARY
// ④ K1 死分支摘除：不透明/水像素在 K1 切点已 continue，玻璃像素走 1838 的
// 行内链 ⇒ 整条不透明管线（RTAO/软影/GI 回路/litColor，~4700 行）在 K1
// 变体不可达 ⇒ 编译期整段摘除。块内声明零外引用（脚本核实）。
vec3 emission =
    payload.emission;


float roughness =
    max(
        payload.roughness,
        0.05
    );


// =====================================================
// LabPBR 1.3 F0 / Metal decoding
//
// 0..229   = dielectric F0
// 230..255 = metal
//
// Hardcoded metal N/K tables are optional in LabPBR,
// so this stage treats 230..255 as generic metals
// using albedo as metallic F0.
// =====================================================

int f0MetalCode =
    int(
        round(
            clamp(
                payload.f0Metal,
                0.0,
                1.0
            ) * 255.0
        )
    );


bool isMetal =
    f0MetalCode >= 230;


float metallic =
    isMetal
        ? 1.0
        : 0.0;


// =====================================================
// PBR-2 - rough-specular tracing gate
//
// Use the raw LabPBR roughness payload here, BEFORE the local
// max(..., 0.05) shading clamp, so the original smoothness
// threshold is preserved exactly.
// =====================================================
float pbr2ReflectionStrength =
    evaluatePBR2ReflectionStrength(
        payload.roughness,
        f0MetalCode
    );

vec3 materialF0 =
    isMetal
        ? albedo
        : vec3(
            float(f0MetalCode) /
            255.0
        );

// =====================================================
// 2026-11-22 Round 2 (Item A): zero-ray shoreline wetness band (hoisted 2026-11-23).
// Hoisted before the K2 split so K2-SUNB (direct), K2-LOC (glossy reflections),
// and K2-I (indirect GI) all observe consistent wet albedo / roughness / normal.
// =====================================================
float shoreWetnessApplied = 0.0;

#ifndef WATER_SHORELINE_WETNESS
#define WATER_SHORELINE_WETNESS 1
#endif
#if WATER_SHORELINE_WETNESS && !defined(ORION_K2_DIRECT_SUN_STAGE_A)
if(rainSolidSurfaceCandidate &&
   cam.glassCausticParams.w > 0.5 &&
   cam.waterSimState.w == 0u &&
   rtLodDistance < 48.0)
{
    const float waterY = cam.waterSimSurfaceAndOrigin.x;
    const float hAboveWater = hitPos.y - waterY;
    if(hAboveWater > -0.35 && hAboveWater < 1.40)
    {
        const vec2 maskUv =
            (hitPos.xz - cam.glassCausticParams.xy) * cam.glassCausticParams.z;
        if(all(greaterThan(maskUv, vec2(0.02))) && all(lessThan(maskUv, vec2(0.98))))
        {
            float waterProx = 0.0;
            if(primaryGeometricNormal.y > 0.5)
            {
                const float stepUv = cam.glassCausticParams.z * 0.90;
                float mC = textureLod(waterBiomeMaskMap, maskUv, 0.0).a;
                float mE = textureLod(waterBiomeMaskMap, maskUv + vec2(stepUv, 0.0), 0.0).a;
                float mW = textureLod(waterBiomeMaskMap, maskUv - vec2(stepUv, 0.0), 0.0).a;
                float mN = textureLod(waterBiomeMaskMap, maskUv + vec2(0.0, stepUv), 0.0).a;
                float mS = textureLod(waterBiomeMaskMap, maskUv - vec2(0.0, stepUv), 0.0).a;
                waterProx = max(mC, max(max(mE, mW), max(mN, mS)));
            }
            else if(abs(primaryGeometricNormal.y) < 0.5)
            {
                const vec2 outUv =
                    maskUv + primaryGeometricNormal.xz * (cam.glassCausticParams.z * 0.55);
                waterProx = textureLod(waterBiomeMaskMap, outUv, 0.0).a;
            }

            if(waterProx > 0.12)
            {
                float shoreNoise = evaluateContinuousRainWetnessNoise(hitPos.xz * 1.35);
                float maxWetHeight =
                    (primaryGeometricNormal.y > 0.5 ? 0.65 : 0.95) + 0.35 * shoreNoise;
                float heightMask = (1.0 - smoothstep(0.05, maxWetHeight, hAboveWater)) *
                    smoothstep(-0.35, -0.05, hAboveWater);
                float distFade = 1.0 - smoothstep(32.0, 48.0, rtLodDistance);
                float shoreWetness = clamp(
                    smoothstep(0.12, 0.72, waterProx) * heightMask * distFade,
                    0.0,
                    1.0);
                if(shoreWetness > 0.005)
                {
                    shoreWetnessApplied = shoreWetness;
                    albedo *= 1.0 - shoreWetness * 0.42;
                    if(isMetal)
                    {
                        materialF0 = albedo;
                    }
                    // 2026-11-24: match damp roughness to glossy full-res distance cutoff so
                    // 16..48 block shorelines don't enter half-rate stochastic glossy flicker.
                    const float shoreGlossFullDist =
                        float(clamp(cam.perfSecondary.w, 16u, 96u));
                    const float shoreNearGloss = 1.0 - smoothstep(
                        shoreGlossFullDist * 0.65, shoreGlossFullDist * 0.98, rtLodDistance);
                    const float shoreTargetRoughness = mix(0.45, 0.18, shoreNearGloss);
                    roughness = min(
                        roughness, mix(roughness, shoreTargetRoughness, shoreWetness * 0.85));
                    if(primaryGeometricNormal.y > 0.5)
                    {
                        normal = normalize(
                            mix(normal, primaryGeometricNormal, shoreWetness * 0.35));
                    }
                    pbr2ReflectionStrength = evaluatePBR2ReflectionStrength(
                        roughness,
                        f0MetalCode);
#if RR_GUIDE_PURIFY_V1 && !defined(ORION_K2_DIRECT_SUN_STAGE_B) && !defined(ORION_K2_DIRECT_LOCAL_STAGE) \
    && !defined(ORION_K2_INDIRECT_TRACE_ONLY) && !SHARC_UPDATE
                    if(s == 0 && matId <= 0.5)
                    {
                        vec3 wetGuideV = normalize(-worldDir.xyz);
                        float wetGuideNdotV = abs(dot(normal, wetGuideV));
                        rrDiffuseAlbedo = albedo * (1.0 - metallic);
                        rrSpecularAlbedo = rrEnvBRDFApprox2(
                            materialF0,
                            roughness * roughness,
                            wetGuideNdotV
                        );
                        rrNormalRoughness = vec4(normal, roughness);
                    }
#endif
                }
            }
        }
    }
}
#endif

// B①：原 B 方案的切点（材质求值后）已上移到 fork 的 else 顶部 —— 新架构下
// K1 精简 rchit 不做材质求值，切点在解析之前落身份（见 else 顶部的
// ORION_K1_PRIMARY 段）。

// ④ ④-K2-LOC / ⑤ K2-SUNB 不编译 RTAO/阴影/GGX/SSS（归 K2-SUN / K2-SUNA）。
// ⑤ 拆腿：SUNA 只编译 RTAO（:2695 本体），阴影/GGX/SSS 归 SUNB（:3111 门）；
// RTAO 的 rtaoFactor 不进阴影簇（只进出口 alpha）⇒ 这是依赖干净的切线。
#if !defined(ORION_K2_INDIRECT_STAGE) && !defined(ORION_K2_DIRECT_LOCAL_STAGE) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_B)
                // ==========================================
                // 步骤 3: RTAO (8 条短距离遮蔽射线)
                // ==========================================
// === 强化版 RTAO：增大探测范围、加深遮蔽对比 ===
                float occlusion =
    0.0;


// 10-chunk RTAO distance budget:
//   0-16   : 4 rays  (existing near quality)
//   16-32  : 2 rays  (existing near quality)
//   32-64  : 1 ray
//   64-96  : 1 ray
//   96-128 : 0.5 ray, 2-frame interleave
//   128-160: 0.25 ray, 4-frame interleave
//   160+   : disabled
int aoSamples = 0;
float aoTemporalWeight = 1.0;


if(rtLodDistance < 16.0 * perfL10TierScale())
{
    aoSamples =
        clamp(int(cam.perfPrimary.x), 0, 4); // user "RTAO 近距采样"
}
else if(rtLodDistance < 32.0 * perfL10TierScale())
{
    aoSamples =
        clamp(int(cam.perfPrimary.y), 0, 2); // user "RTAO 中距采样"
}
else if(rtLodDistance < 64.0 * perfL10TierScale())
{
    aoSamples = 1;
}
else if(rtLodDistance < 96.0 * perfL10TierScale())
{
    aoSamples = 1;
}
else if(rtLodDistance < 128.0 * perfL10TierScale())
{
    aoSamples = rtLodHalfSample ? 1 : 0;
    aoTemporalWeight = 2.0;
}
else if(rtLodDistance < 160.0 * perfL10TierScale())
{
    aoSamples = rtLodQuarterSample ? 1 : 0;
    aoTemporalWeight = 4.0;
}


if(perfL10DisableRtao())
{
    aoSamples =
        0;
}

// 2026-10-27 用户「RTAO 近距采样」的关闭档（=5）：整条 RTAO 链关闭。
// 0 仍是旧语义（0~16 格不发 AO 射线，32~96 格照旧 1 条）；5 覆盖所有距离段，
// rtaoFactor 保持 1.0 中性。放在 perfL10AoSampleCap 之前，cap 不会把 0 抬回去。
if(int(cam.perfPrimary.x) >= 5)
{
    aoSamples = 0;
}

// Perf-L10.1 stage 9: cap the near-field ray count so the cost of four RTAO
// rays against two is a measurable partial delta.
aoSamples =
    min(aoSamples, perfL10AoSampleCap());

// =====================================================
// 2026-11-02：**RTAO 不再参与稀疏**。
//
// 旧行为：非活跃像素 aoSamples = 0（rtaoFactor 保持 1.0 的"无遮蔽"中性值）。
// 实测症状：草/作物（alpha-test cutout）上出现成片发光白块。
//
// 机理：AO 与太阳软影是**可见性项**，跳过它们等于让叶片失去自遮挡 ⇒ 本应暗的
// 侧壁被点亮；再乘 1/rate（40% ⇒ 2.5 倍）就把"漏光"放大成发光。而 RR 的
// anti-firefly 会拒掉明显低于历史的值 —— 于是 60% 帧的"正常暗"被少采、40% 帧的
// "漏光×2.5"被保留，时域平均后整体偏亮，叶越密越明显。
//
// 稀疏只保留**能量项**（GI / glossy / NEE / 二级 DDA）：跳过它们只增加噪声、
// 不破坏能量守恒，RR 能吸收。
// A/B：-DVPT_SPARSE_SKIP_VISIBILITY=1 回到旧的"非活跃像素跳过 AO/阴影"。
// =====================================================
// 2026-11-02：恢复 1（非活跃像素跳过 AO/阴影）。
// 之前改成 0 的依据是"草叶发亮"，后查明那批亮来自材质包把植被标成了发光材质，
// 与可见性项无关 ⇒ 那条取舍建立在误判上。30% 实测 trace 只省 32%（主射线+满采样项
// 占 54%），把 AO/软影重新纳入稀疏是本轮最大的一块可省成本。
#ifndef VPT_SPARSE_SKIP_VISIBILITY
#define VPT_SPARSE_SKIP_VISIBILITY 1
#endif
#if VPT_SPARSE_SKIP_VISIBILITY
if(!vptPixelActive)
{
    aoSamples = 0;
}
#endif

// [8] 本像素是否会**真的发出** RTAO 射线。
// 2026-10-28：原口径只看 aoSamples > 0，但生产档（SER 变体）把"死 AO 射线"
// 整体跳过（perfSerAb.w 恒 1，见 VulkanCore.cpp 的 PERF_L10_ENABLE_RTAO_SKIP_AB
// = false ⇒ 永不做 A/B）。于是日志里 aoPixPct 显示 82% 而 aoRayPerPix 恒 0，
// 两个字段口径相反，读起来像"AO 在工作"。这里让两者同源。
if(aoSamples > 0 && cam.perfSerAb.w == 0u)
{
    perfRayStatAdd(8u, 1u);
}

float maxDist =
    5.5;


for(
    int i = 0;
    i < aoSamples;
    ++i
)
{
    vec2 aoBn = vec2(
        glassBlueNoiseSample(uvec2(pixelCoord), cam.frameCount, 61u + uint(i) * 2u),
        glassBlueNoiseSample(uvec2(pixelCoord), cam.frameCount, 62u + uint(i) * 2u)
    );
    vec3 aoDir =
        sampleHemisphereCosine2D(
            normal,
            aoBn
        );


    hit.hitDistance =
        -1.0;

    hit.state = makeTraceState(RT_PURPOSE_VISIBILITY_ONLY);


#if RTAO_VISIBILITY_EARLYOUT
    // 2026-11-16：AO 可见性探针的 inline ray query 可选臂（ORION_VIS_QUERY bit0，
    // cam.visQueryAb.x）。默认 0 = 走下方既有 traceRayEXT，逐位旧行为。
    // 同一 TLAS / cullMask 0xff / tmin 0.01 / 同方向；query 没有 any-hit，旗标只用
    // TerminateOnFirstHit（SkipClosestHitShader 是 traceRayEXT 旗标，query 上加它可能
    // 导致命中不 commit，教训见 volumetric_fog.glsl 头部注释）。未命中写 -1.0，与
    // rt_test.rmiss 对 purpose 3 的写入值一致（调用方按 hitDistance > 0.0 判遮挡距离）。
    if(cam.visQueryAb.x != 0u)
    {
        // [9] 真实发出的 AO 射线（query 臂同样计一次）
        perfRayStatAdd(9u, 1u);

        rayQueryEXT aoVisQuery;
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        rayQueryInitializeEXT(
            aoVisQuery,
            topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT,
            g_secondaryCullMask,
            hitPos + aoDir * 0.001,
            0.01,
            aoDir,
            maxDist
        );

        int aoVisProceedGuard = 0;
        // 防死循环上限（参照 volumetric_fog.glsl 的 proceedGuard）：TLAS 未绑定/失效
        // 时 rayQueryProceedEXT 可能一直返回 true ⇒ GPU 侧无限循环 ⇒ TDR。
        // 2026-11-16 修正：committed 交点只有在查询终止后才可读，不能在 Proceed 循环
        // 体里判——TerminateOnFirstHit 下一旦命中就终止，循环体可能一次都不进，那样
        // 永远读不到 committed，AO 会恒判"无遮挡"。必须在循环结束后统一读。
        while(rayQueryProceedEXT(aoVisQuery) && ++aoVisProceedGuard < 32)
        {
            if(rayQueryGetIntersectionTypeEXT(aoVisQuery, false) ==
               gl_RayQueryCandidateIntersectionTriangleEXT)
            {
                rayQueryConfirmIntersectionEXT(aoVisQuery);
            }
        }
        if(rayQueryGetIntersectionTypeEXT(aoVisQuery, true) !=
            gl_RayQueryCommittedIntersectionNoneEXT)
        {
            hit.hitDistance = rayQueryGetIntersectionTEXT(aoVisQuery, true);
        }
        else
        {
            hit.hitDistance = -1.0;
        }
    }
    else
#endif
    // 2026-11-27 (Batch AI): decoupled from RT_ENABLE_SER so plain legs also skip dead RTAO rays.
    if(cam.perfSerAb.w == 0u)
    {
        // [9] 真实发出的 AO 射线（perfSerAb.w=1 时整条 trace 被跳过，不计）
        perfRayStatAdd(9u, 1u);

        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        traceRayEXT(
            topLevelAS,

            gl_RayFlagsTerminateOnFirstHitEXT,

            g_secondaryCullMask,
            0,
            0,
            0,

            hitPos + aoDir * 0.001,

            0.01,
            aoDir,
            maxDist,
            0
        );
    }


    if(hit.hitDistance > 0.0)
    {
        float distWeight =
            1.0 -
            clamp(
                hit.hitDistance /
                maxDist,
                0.0,
                1.0
            );


        occlusion +=
            distWeight *
            distWeight;
    }
}


float rtaoFactor =
    1.0;


if(aoSamples > 0)
{
    rtaoFactor =
        clamp(
            pow(
                clamp(
                    1.0 -
                    (
                        (occlusion * aoTemporalWeight) /
                        float(aoSamples)
                    ),
                    0.0,
                    1.0
                ),
                1.3
            ),
            0.45,
            1.0
        );
}
#elif defined(ORION_K2_DIRECT_LOCAL_STAGE)
// ④ K2-LOC：直射级（RTAO/阴影/GGX/SSS）归 K2-SUN，本体不编译对应段。这里的
// 桩只补共享区表达式仍引用的最小量；局部光/镜面的真值由 LOC 自己的块（下方
// B/C）求值，出口做 load-add 叠加，**不**走 K2-I 的整图载入桩。
const float rtaoFactor = 1.0;
const float NdotL = max(0.0, dot(normal, sunDir));
#elif defined(ORION_K2_DIRECT_SUN_STAGE_B)
// ⑤ K2-SUNB：只桩 rtaoFactor（共享区若有引用则取 1.0；SUNB 出口的 a 来自
// imageLoad 原样携带，实际不读它）。⚠ NdotL **不在**这里桩——阴影簇门内
// （:3121 门）自己定义 float NdotL，SUNB 编译该门 ⇒ 桩里再定义会撞车
// （LOC 桩需要它是因为 LOC 排除了该门，SUNB 不排除）。
const float rtaoFactor = 1.0;
#else
// ============================================================================
// ③ K2-I 桩：直射级（RTAO/阴影/GGX/SSS/局部光/水面太阳/镜面）已由 K2-D 求值。
// 桩变量让稀疏区 / litColor / SS 分项 / 诊断区的表达式**逐字保持原语义**：
//   · image.rgb = directLight + localDirect + localSecondary + waterSun
//     （K2-D 未缩放落盘；经 directLight 桩随稀疏区既有表达式参与 1/rate
//      补偿与 firefly 钳制 —— 拆分前提 VPT_SPARSE_SKIP_VISIBILITY=1，
//      稀疏关闭时 sparseTermScale 恒 1 ⇒ 逐位回原值）
//   · glossyReflectionLight = landing 里的**原始** glossy（×POM 由既有表达式
//     照常施加 ⇒ 与主变体逐位同语义）；rrSpecularHitDistance 用 K2-D 的
//     稳定快照。水像素不走这条桩（水支在上方已替换）。
// ============================================================================
#ifndef VPT_SPARSE_SKIP_VISIBILITY
#define VPT_SPARSE_SKIP_VISIBILITY 1
#endif
#if !VPT_SPARSE_SKIP_VISIBILITY
#error "K2 直射/间接拆分要求 VPT_SPARSE_SKIP_VISIBILITY=1（直射已并进 image.rgb 参与 1/rate 补偿）"
#endif
#if defined(ORION_K2_INDIRECT_TRACE_ONLY) || SHARC_UPDATE
const float rtaoFactor = 1.0;
const vec3 shadowTransmittance = vec3(1.0);
const float NdotL = max(0.0, dot(normal, sunDir));
#else
const float rtaoFactor = k2DirectFromImage.a;
vec3 k2GlossyExtra = vec3(0.0);
float k2SpecHitDistFromLanding = 0.0;
ptLandingLoadGlossyExtra(
    ptLandingRecordIndex(
        uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
        s
    ),
    k2GlossyExtra,
    k2SpecHitDistFromLanding
);
rrSpecularHitDistance = k2SpecHitDistFromLanding;
const vec3 directLight = k2DirectFromImage.rgb;
const vec3 localDirectLight = vec3(0.0);
const vec3 localSecondaryLight = vec3(0.0);
const vec3 waterReflectedSunLight = vec3(0.0);
vec3 glossyReflectionLight = k2GlossyExtra;
const vec3 localExactFaceDirect = vec3(0.0);
const vec3 shadowTransmittance = vec3(1.0);
// 阴影区（K2-I 已摘除）声明、GI/远场段仍在用的标量 —— 按原公式就地重算，
// 语义逐位同主变体。
const float NdotL = max(0.0, dot(normal, sunDir));
#endif
#endif

                vec3 V = -worldDir.xyz;
                float NdotV = max(dot(normal, V), 0.0001);
#if !defined(ORION_K2_DIRECT_LOCAL_STAGE) && !defined(ORION_K2_DIRECT_SUN_STAGE_A) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_B)
vec3 F0 =
    materialF0;


vec3 F =
    F0 +
    (1.0 - F0) *
    pow(
        1.0 - NdotV,
        5.0
    );

                // === 严格控制：非金属且粗糙的表面绝对禁止镜面反射，消除镜面世界 ===
                // =====================================================
// G2B-2 Q1 + E1
// Diffuse GI remains independent from glossy reflection, but its
// single direction now uses the confirmed 50:50 BSDF/Local-Light
// proposal mixture.  No second GI ray is introduced.
// =====================================================

vec3 bounceDir =
    vec3(0.0);


float g2bPrimaryMixtureWeight =
    1.0;


// Match the diffuse-GI LOD gate below before doing the expensive local-light
// guide scan.  On interleaved/distant pixels the GI loop is skipped anyway,
// so generating a guide direction there only burns ALU and buffer reads.
//
// ⚠⚠ 必须与下面那个梯形是**同一个出口**：这一处一旦比它窄，GI 打开了
//    也拿不到方向（bounceDir 保持零向量）⇒ traceRay 方向为零 ⇒ 静默变黑。
bool prepareDiffuseGI =
    rtLodDiffuseGiEnabled;

if(prepareDiffuseGI)
{
    vec2 giBn = vec2(
        glassBlueNoiseSample(uvec2(pixelCoord), cam.frameCount, 51u),
        glassBlueNoiseSample(uvec2(pixelCoord), cam.frameCount, 52u)
    );
    bounceDir =
        sampleHemisphereCosine2D(
            normalize(normal),
            giBn
        );
    g2bPrimaryMixtureWeight = 1.0;
}
else
{
    // The direction/weight are dead when the GI loop is disabled.  Keep a
    // valid zero direction for debuggers and avoid touching the RNG state.
    bounceDir = normal;
    g2bPrimaryMixtureWeight = 0.0;
}


vec3 brdfWeight =
    (
        vec3(1.0) -
        F
    ) *
    (
        1.0 -
        metallic
    ) *
    albedo *
    g2bPrimaryMixtureWeight;
#endif

// ============================================================================
// 2026-10-11 SHaRC 缓存值的归一化基准 —— **结论：就用 brdfWeight 原样除**
//
// 判断依据不是"哪个看起来更物理"，而是**看循环里真实的数据流**：
// 本文件每一处 GI 贡献的形态都是 `currentThroughput × 辐射度`，例如未命中那一支
//     indirectLight += currentThroughput * unpackAlbedoUnorm(hit.albedoPacked) * 0.6;
// 也就是说 brdfWeight 承载的是**全部估计量权重**（材质 BSDF × cos/pdf × 提议混合
// 权重），而真正"物理"的是它右边那个被乘的量。
// ⇒ indirectLight / brdfWeight 就是一个**物理辐射度 R**（该顶点沿首个采样方向来的
//    辐射度，含更深的弹射）。辐射度是低方差的（不会跨数量级），这正是缓存需要的性质。
//
// 我曾一度"只除材质因子"（把 g2bPrimaryMixtureWeight 从分母里拿掉，改成
// `bsdfFactor = brdfWeight / W_mix`），理由是"让量跨材质可比"。**那是做反了**：
// 拿掉 W_mix 等于把那个**逐样本随机的采样权重塞回了缓存值**，而查询侧乘的又是自己的
// 因子 ⇒ 整个式子变成 `B_query × W_mix(存) × R`，其中 W_mix(存) 没有任何东西抵消
// ⇒ **逐体素随机缩放**。实机症状正是"不糊、但每块随机色、硬边界"，而且随 A/B 换臂
// 时有时无（换臂才用缓存）。
//
// 正确的一对（低方差、且与渲染器自身的估计量形式逐字一致）：
//     存：indirectLight / brdfWeight        ← 物理辐射度 R
//     查：brdfWeight(自己) × R              ← 就是渲染器自己会算出的那个贡献
// 体素平均的是"许多方向上的 R"，收敛到该体素的**半球平均入射辐射度**（这正是 SHARC
// 想要的东西）。跨材质/跨方向的平均只带来**空间模糊**，不会带来随机缩放。
//
// 所以这里不再引入任何新变量 —— 下面 update / query 两处都直接用 brdfWeight。
// ============================================================================

// ③④⑤ 同上：阴影段 K2-D/K2-SUN/K2-SUNB 编译，K2-LOC/K2-I/SUNA 不编译
//（⑤ 拆腿：SUNA 只产 RTAO，阴影+GGX+SSS 归 SUNB）。
#if !defined(ORION_K2_INDIRECT_STAGE) && !defined(ORION_K2_DIRECT_LOCAL_STAGE) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_A)
                // ==========================================
                // 步骤 4: Direct Shadow (4 条软阴影射线)
                // ==========================================
float NdotL = max(0.0, dot(normal, sunDir));
vec3 shadowTransmittance =
    vec3(0.0);

// Direct-sun visibility is deliberately kept independent from rain exposure.
// Rain falls vertically, while sampleSunDirection() follows the sun and can
// be sideways or below the horizon.  Using the latter as a roof test was the
// reason an open floor never received the ground-rain material response.
float rainSkyExposure =
    0.0;

int shadowSamples =
    rtLodDistance < 16.0 * perfL10TierScale()
        ? 2
        : 1;

// Perf-L10.1 stage 10: cap the soft-shadow ray count so one extra visibility
// ray per near pixel becomes a measurable partial delta.
shadowSamples =
    min(shadowSamples, perfL10ShadowSampleCap());

// =====================================================
// 2026-11-04：采样数固定化（A/B）。
//
// 上面两行让 `shadowSamples` 随性能档变化，而 1 与 2 对应**两套不同的半影**：
//   samples = 2 ⇒ 太阳盘上两个点的平均（边缘更软）
//   samples = 1 ⇒ 盘上一个固定点（≈ 硬边）
// tier/cap 一旦在负载下滑动，16 格附近的像素就会在两种阴影值之间翻 ⇒ 阴影终止线
// 静止时反复出颗粒，动一下负载变了反而"干净"。画质量不该由性能档决定。
//
//   0 = 现状（自适应，逐位回退）
//   2 = **默认**：恒软，边界最稳（+1 条近场遮蔽射线）
//   1 = 恒硬：最省，边界发脆
// A/B：-DSUN_SHADOW_FORCE_SAMPLES=0 / =1
// =====================================================
#ifndef SUN_SHADOW_FORCE_SAMPLES
#define SUN_SHADOW_FORCE_SAMPLES 0
#endif

#if SUN_SHADOW_FORCE_SAMPLES > 0
shadowSamples = SUN_SHADOW_FORCE_SAMPLES;
#endif

// 2026-11-05：硬阴影总闸。
// 1 = 单条射线 + 太阳中心方向；0 = 软阴影（太阳盘方向 tap，见下面 2026-11-10 段）。
#ifndef SUN_SHADOW_HARD
// 0 = 软阴影（默认）；1 = 硬阴影（单射线）
#define SUN_SHADOW_HARD 0
#endif

// =====================================================
// 2026-11-10 太阳软影重做：**tap 落在太阳盘方向上**，不再平移射线起点
// =====================================================
// 旧档（2026-11-06 起）两个病灶，逐行可查：
//   ① 循环里 `shadowDir` 三元表达式两边都写 normalize(sunDir)
//      ⇒ N 个 tap 方向完全相同，sampleSunDirection() 的结果被丢弃；
//   ② tap 只沿垂直光方向平移射线**起点**，半径 = 相机距离 × 0.0015
//      ⇒ 数学上等价于把影子整体平移，半影宽被钉死在 ~1.2 屏幕像素，
//        与遮挡物距离、太阳高度都无关（所以"直射阴影一直这么硬"）。
//
// 现在：tap 直接取太阳盘上的方向，半影宽 = 孔径 × 遮挡物到着色点距离，
// 物理上正确，且随遮挡距离自然变宽。
//
// 确定性：golden-angle 螺旋 + 面积均匀半径，**无随机数、无 frameCount、
// 无逐像素相位** ⇒ 同一场景/同一太阳方向恒定给出同一结果：既不逐帧闪，
// 也不随摄像机移动游动（图案是世界锚定的）。只剩 N+1 级量化台阶。
//
// 两个量必须一起调，缺一个都会更难看：
//   带宽度 = 2 × 孔径 × 遮挡物到着色点距离      ← 孔径
//   带内级数 = tap 数 + 1                      ← tap 数
// 只加宽孔径 ⇒ 同样的 tap 摊在更宽的带上 ⇒ 台阶变粗（更难看）；
// 只加 tap 数 ⇒ 带宽不变 ⇒ 仍然生硬。
//
// 2026-11-10 第二刀（0.014 -> 0.030，tap 8/4 -> 16/8）：
//   0.014 时，贴墙/墙角这类"遮挡物离着色点只有 1 格"的自遮挡，
//   带宽度 = 2×0.014×1 = 0.028 格，5 格视距下只有 ~5 px ⇒ 看着仍是硬边。
//   0.030（≈1.7°，等效太阳视直径 3.4°）同样条件下 ~12 px，才是"看得出的过渡"。
//   级距 = 12/17 ≈ 0.7 px，与上一版（5/9 ≈ 0.55 px）同量级 ⇒ 没有变粗。
//
// 代价：近场射线 8 -> 16 条/像素。低档（设置「太阳阴影采样 = 低」）就是
// 上一版的高档（8/4），运行中随时可以切回去对比，不需要重编。
//   -DSUN_SHADOW_SOFT_APERTURE=0.0047  → 物理（最硬，等于没做这一步）
//   -DSUN_SHADOW_SOFT_APERTURE=0.045   → 更软
//   -DSUN_SHADOW_SOFT_APERTURE=0.060   → 很软（开始明显漏光/接触影变淡）
#ifndef SUN_SHADOW_SOFT_APERTURE
#define SUN_SHADOW_SOFT_APERTURE 0.030
#endif

// 高档 tap 数（用户「太阳阴影采样」= 高，即 CameraUBO.perfPrimary.w = 2）。
// 2026-11-22 非时域性能优化 #3：半影已走逐帧随机方向 + DLSS-RR 时域收敛，
// 不再需要确定性 16/8-tap 去量化台阶 ⇒ 与时空蓝噪声配对降到近场 2/1、远场 1/1。
#ifndef SUN_SHADOW_SOFT_TAPS
#define SUN_SHADOW_SOFT_TAPS 2
#endif
#ifndef SUN_SHADOW_SOFT_TAPS_LOW
#define SUN_SHADOW_SOFT_TAPS_LOW 1
#endif

// 已停用（方向 tap 取代起点平移）。保留宏名只为外部 -D 不报未定义。
#ifndef SUN_SHADOW_SOFT_TAP_SCALE
#define SUN_SHADOW_SOFT_TAP_SCALE 0.0015
#endif
#ifndef SUN_SHADOW_SOFT_TAP_MIN
#define SUN_SHADOW_SOFT_TAP_MIN 0.002
#endif

// 2026-11-05 探针 A：阴影射线起点偏移与 TMin。
// 默认与历史逐位一致（0.001 / 0.02）。
#ifndef SUN_SHADOW_RAY_ORIGIN_OFFSET
#define SUN_SHADOW_RAY_ORIGIN_OFFSET 0.001
#endif
#ifndef SUN_SHADOW_RAY_TMIN
#define SUN_SHADOW_RAY_TMIN 0.02
#endif

#ifndef SUN_SHADOW_SOFT_TAPS_FAR
// 2026-11-22：远场（>= 32m）统一 1 tap，交由时空蓝噪声 + DLSS-RR 收敛。
#define SUN_SHADOW_SOFT_TAPS_FAR 1
#endif
#ifndef SUN_SHADOW_SOFT_TAPS_FAR_LOW
#define SUN_SHADOW_SOFT_TAPS_FAR_LOW 1
#endif
#ifndef SUN_SHADOW_SOFT_TAPS_FAR_DISTANCE
#define SUN_SHADOW_SOFT_TAPS_FAR_DISTANCE 32.0
#endif

#if SUN_SHADOW_HARD
// 硬阴影：单射线。
shadowSamples = 1;
#else
// 软阴影 tap 数：跟随用户「太阳阴影采样」（1 = 低 / 2 = 高）。
// 这是它**第一次真正生效** —— 旧档下面这行会把它覆写成常量，滑块是死参数。
// 档 10（PERF-L10）仍强制 1 条：它的用途就是量"每多一条射线的价格"。
{
    const bool sunShadowNearField =
        rtLodDistance < SUN_SHADOW_SOFT_TAPS_FAR_DISTANCE * perfL10TierScale();
    const bool sunShadowHighQuality =
        perfL10ShadowSampleCap() > 1;

    shadowSamples = sunShadowNearField
        ? (sunShadowHighQuality ? SUN_SHADOW_SOFT_TAPS : SUN_SHADOW_SOFT_TAPS_LOW)
        : (sunShadowHighQuality ? SUN_SHADOW_SOFT_TAPS_FAR : SUN_SHADOW_SOFT_TAPS_FAR_LOW);

    if(perfL10Stage() == 10u)
    {
        shadowSamples = 1;
    }
}
#endif

// =====================================================
// 2026-11-12 采用 Caustica 的 "只用 DLSS-RR" 路径
//
// 参考实现（本地 caustica\shaders\world，逐行核对过）：
//   · trace_transport.slang 的 traceDirect：每像素**每帧**沿太阳盘内一个随机
//     方向发可见性射线，结果直接进最终辐射 —— **没有空域滤波、没有解析半影**；
//   · retained_lights.slang / world_common.slang：太阳是面积光
//     （light.angularRadius），selected.direction 是盘内随机点 ⇒ 逐帧变；
//   · nrd_signals.slang：hitDistance 只喂 NRD。本工程没有直射降噪器，
//     所以那次"要遮挡距离"的改造也不需要了。
//   ⇒ 半影完全由 **DLSS-RR 的时域累积**收敛出来。
//
// 与确定性 16-tap 的取舍：那版"零噪 + 17 级量化台阶"，这版"逐帧微噪 + RR
// 收敛成连续半影"。后者才是参考的做法，也是用户选定要走的路径。
//
// 形态1（屏幕空间滤波）实测噪点明显，已停用：下面这个常量恒 false，
// 相关代码保留但被编译器整段折叠（ORION_SUNVIS=1 也不会复活它）。
// =====================================================
const bool sunVisibilityFilterActive = false;

// 逐帧变化的 seed（参考实现同款：随机方向 + RR 收敛）。
// ⚠ 不含 frameCount 会退化成"钉死的噪点"——RR 没有可平均的信号。
uint shadowSeed =
    (gl_LaunchIDEXT.x * 3129371u) ^
    (gl_LaunchIDEXT.y * 9277u) ^
    0x68bc21ebu ^
    (cam.frameCount * 26699u);
pcg_hash(shadowSeed);

// =====================================================
// 2026-10-06 降本：背光面等"射线结果无处可用"的像素直接不发太阳阴影射线。
//
// 阴影射线沿太阳方向发出、**与法线无关**，所以它只在"这个像素确实可能收到直射"
// 时才有用。以下任一条成立就保留射线：
//   · NdotL > 0：正面受光，直射项要用；
//   · cam.weatherParams.x > 0.01：有雨。雨天地面湿膜会把法线翻向天顶并在 18117
//     重算 NdotL，背光面可能因此变正面 ⇒ 必须保留；
//   · rtLodDistance >= 160：那里 shadowTransmittance 还喂 farSlopeOcclusion；
//   · matId ∈ (0.2,0.45)：树叶/花瓣的背光透射项（18518）要用它。
// 2026-11-10：方向 tap 是**纯确定性**的（无随机流），跳过整个循环同样不扰动任何
// 下游采样；shadowSamples = 0 时归一化给出 shadowTransmittance = 0，
// 与"背光面无直射"逐位一致。
// 回退：把下面这个 if 整段删掉即回到"永远发满 shadowSamples 条"。
// =====================================================
if(!(NdotL > 0.0 ||
     cam.weatherParams.x > 0.01 ||
     rtLodDistance >= 160.0 ||
     (matId > 0.2 && matId < 0.45)))
{
    shadowSamples = 0;
}

// 2026-11-02：太阳软阴影同样不再参与稀疏（理由见上面 RTAO 那段的长注释 ——
// 跳过可见性项会造成漏光，1/rate 再把漏光放大成发光）。
#if VPT_SPARSE_SKIP_VISIBILITY
if(!vptPixelActive)
{
    shadowSamples = 0;
}
#endif

// =====================================================
// 2026-11-01 档 2：低太阳角 + 近场图覆盖范围内，改用阴影图，不再发射线。
//
// 病灶：低太阳角时太阳软影射线几乎水平，TerminateOnFirstHit 靠"撞到东西"
// 早停；开阔地/高处撞不到 ⇒ 每条射线要横穿 tMax 格。正午射线垂直，几十格
// 内就被地面挡住。前两轮已经把 tMax 收到 160 格，这一轮把"射线"本身换成
// "查图"——近场太阳阴影图（1024²、160 格半径、8 Hz 重建）本来就是同一条
// 数学，只是用 1M 条射线预计算过。
//
// 只对 140 格内生效：留 20 格余量避开图边界的 outside 淡出，否则图外像素会
// 被平滑地判成背光（SUN_SHADOW_OUTSIDE_VISIBILITY = 0.08）而整体变暗。
// 140 格外仍走射线，那里的 tMax 已经是 160 格。
// =====================================================
// 2026-11-02：**档 2 停用**（用光栅阴影图接管低角度直射）。
//
// 用户实测：夕阳时漏光 + 阴影仍抖。复盘确认这是方案本身的固有缺陷，不是参数：
//   1) 阴影图存"沿太阳方向的首个命中距离"，掠射角时一个 0.31 格纹素对应几十格
//      深度落差，单一 bias（0.06）不可能同时压住近处 acne 与远处 peter-panning
//      ⇒ 必然漏光；
//   2) 修"阶梯凹角亮线"的 vptVoxelPathBlocked（16 格体素 DDA 保守门）只加在
//      射线路径上，图路径完全没有这道门 ⇒ 亮线直接回来；
//   3) 图只有 8 Hz 重建、纹素 0.31 格，而掠射角把 penumbra 拉长几十倍 ⇒ 步进
//      被放大成可见跳变。
// 光追直射本来就是为了避开 bias/纹素这些而存在的，用给雾用的低分辨率图接管
// 它等于把 shadow map 的经典缺陷全盘接回来。
//
// 代码保留（宏置 1 可重开做对照），以后若上真正的 CSM（多级联 + 法线偏移 +
// PCSS）再启用。
#ifndef SUN_SHADOW_MAP_TAKEOVER
#define SUN_SHADOW_MAP_TAKEOVER 0
#endif

const float sunHeightForNearMap =
    clamp(sunDir.y / 0.81915204429, 0.0, 1.0);
const bool useNearShadowMap =
    (SUN_SHADOW_MAP_TAKEOVER != 0) &&
    (sunHeightForNearMap < 0.30) &&
    (rtLodDistance <= 140.0 * perfL10TierScale());

float nearShadowMapVisibility = 1.0;

if(useNearShadowMap)
{
    shadowSamples = 0;

    // 4 tap 固定偏移平均：单点采样在 0.31 格/纹素的图上会给出阶梯边缘，
    // 而固定的 4 个方向求平均既平滑、又完全不随时间变化。
    float nearVisibilitySum = 0.0;
    for(int tap = 0; tap < 4; ++tap)
    {
        float nearOutsideFade = 0.0;
        nearVisibilitySum += sunShadowVisibilityIn(
            sunShadowMapNear,
            hitPos,
            sunDir,
            tap,
            SUN_SHADOW_EXTENT_XY,
            SUN_SHADOW_EXTENT_Z,
            SUN_SHADOW_TEXEL,
            SUN_SHADOW_CENTER_QUANTUM,
            SUN_SHADOW_OUTSIDE_VISIBILITY,
            SUN_SHADOW_BIAS_NEAR,
            nearOutsideFade);
    }
    nearShadowMapVisibility = clamp(nearVisibilitySum * 0.25, 0.0, 1.0);
}

// =====================================================
// 远场档标记（2026-10-01）
//
// ⚠ 原路线 A（"远场直射阴影改查 1024² 远场太阳阴影图"）**已撤回**，取证记录：
//
// 1) 远场本来就有正确的直射遮蔽。上面这个 shadowSamples 对 rtLodDistance ≥ 16
//    一律给 1，射线是 mask 0xff、tMax 10000 的**∞ 距离逐像素**射线。山体投下的
//    大尺度影一直成立 —— 把一条精确射线换成一个"同一条射线的低分辨率副本"，
//    物理上<b>不可能</b>新增遮蔽尺度，故前提不成立。
//
// 2) 那张图的深度比较对<b>表面</b>采样点根本不成立。生产者存的是"从盒的
//    上风侧边界沿太阳方向首个命中体的距离"，而"上风侧"在<b>背太阳</b>方向
//    （sunShadowRayOriginFar 里的 -sunDirection*extentZ）。于是对任意表面点，
//    该射线恰好穿过它自己，首个命中体永远落在采样点的背光侧；再加上所有实例都
//    带 TRIANGLE_FACING_CULL_DISABLE（背面不剔除，见 VulkanCore.cpp 的
//    instance.flags），地形会正好在 t == depth 处被命中 ⇒
//    `depth < stored - SUN_SHADOW_BIAS` 恒不成立 ⇒ 远场地形恒判为全遮挡。
//    实测探针（一次性，已删）给出的可见性就是 0，表现为"远场 LOD 全黑"。
//    （体积光/雾不受影响：它们的采样点在空气里，不是表面。）
//
//    ⚠ 2026-10-10 追加修正：上面第 2) 条的因果判断**已过时**。生产者起点
//    2026-10-10 起翻到**朝太阳**一侧（sunShadowRayOrigin* 由 "-" 改为
//    "+sunDirection"），"上风侧"不再是背太阳方向；同一份数学（sun_shadow.glsl）
//    也服务近场图，而近场图的这一处反向正是用户报"体积光多数情况下穿墙"的根因 ——
//    背太阳端起点让 `depth < stored` 的含义反成"比最低遮挡体还低"，于是空中采样点
//    被系统性判可见、背光侧墙体拦不住光柱。修正后本图的表面点可见性也恢复正常
//    （表面受光），第 2) 条不再成立。本段保留作取证记录。
//    远场直射仍走下面的逐像素射线（第 1) 条理由不受影响），故 farShadowActive 的
//    用途不变、本文件依旧不采样 binding 58。
//
// farShadowActive 现在只服务环境底/GI 的坡度遮蔽（见下方 farSlopeOcclusion），
// 不再触碰 shadowSamples / shadowTransmittance。
//
// 一键回退：ORION_SUNSHADOW_FAR=0 ⇒ host 把 sunShadowFarParams.w 置 0，
//  本布尔量恒假，远场逐字节回到改动前。
// =====================================================
bool farShadowActive =
    cam.sunShadowFarParams.w > 0.5 &&
    rtLodDistance >= 160.0 * perfL10TierScale();


// =====================================================
// T1/T9/T3 COMBINED - colored transparent sun shadows
//
// Visibility rays start with white transmittance.
// Any-Hit multiplies glass color into unpackAlbedoUnorm(hit.albedoPacked) and ignores
// that glass intersection. Opaque geometry still terminates the ray.
// =====================================================

// =====================================================
// 2026-10-05 太阳盘采样改为逐像素稳定（不含 frameCount）
//
// 直射光照没有时域累积（kNrdLocalDiEnabled=false、g_directAtrousEnabled=false，
// 见 VulkanCore.cpp），旧的"seed 含 frameCount ⇒ 每帧每像素重抽太阳盘方向"
// 让细小遮挡体（草丛/树叶/栅栏）的影子逐帧重新掷骰 —— 用户报告"这个阴影有缝
// 而且还一跳一跳的"。改成按像素坐标哈希后，每个像素永远采样太阳盘上固定的
// 点：影子的软边变成**静止**的抖动图案（近处 2 样本、屏幕空间呈软边），不再
// 逐帧跳变。只影响太阳直射可见性；局部光/GI/AO 的采样链（seed）保持逐帧变化。
// =====================================================
// =====================================================
// 2026-10-10 修正：逐像素冻结只对"多样本"成立，1 样本时它是一份**永不动**的图案
//
// 上面那条结论（"直射光照没有时域累积"）漏掉了 **DLSS-RR** —— RR 才是最终那道
// 时域滤波。于是同一次改动有两种截然相反的后果：
//   · 近场（<16 格）采样数 = 2：冻结图案是"静止的软边"，是当时想要的取舍；
//   · 远场（≥16 格）采样数 = 1（**硬编码**，用户的"太阳阴影采样"只当上限，
//     在 16 格外不起作用）：每个像素永远查太阳盘上同一个点 ⇒ 结果**逐帧完全
//     相同** ⇒ RR 的时域累积没有东西可平均 ⇒ 屏幕上是一份钉死的噪点。表现：
//     "太阳照射下的阴影呈噪点状、静止时最明显、移动时正常、任何设置都改不动"。
//     0.43 的种子里含 frameCount（噪点逐帧变）⇒ 被 RR 平均掉 ⇒ 干净。
//
// 所以这里按**采样数**分流，而不是按距离 —— 判据与 shadowSamples 的构造同源：
//   样本 ≥ 2 ⇒ 保持冻结（10-05 修的"叶隙影子一跳一跳"不会回来）
//   样本 = 1 ⇒ 恢复帧项，把 1 样本的白噪声交给 RR 去收敛成柔边
// 恢复它是安全的：10-09 起 MV 才真正被上报，RR 的重投影到那时才第一次能用。
// =====================================================
// 2026-11-10：这里原本是 shadowSeed（逐像素哈希 + 可选的 frameCount 项）以及
// 每 tap 一次的 sampleSunDirection()。方向 tap 改成确定性螺旋后不再需要随机流；
// shadowSeed 在本 TU 内**没有其它读者**（2026-11-10 全文件核对：只有 3238 的
// sampleSunDirection 与 3171 的 pcg_hash 两处），删掉不改变任何下游采样链。
// 上面 2026-10-05 / 10-10 / 11-02 三段关于"冻结 vs 逐帧"的结论已随实现失效，
// 保留作取证记录；SUN_SHADOW_STATIC_SEED 宏同时停用。


// =====================================================
// 2026-11-01 低太阳角的太阳软影射线长度封顶（性能）
//
// 实测（PERF L10 GPU，277 个窗口）：主 RT pass 在日出/日落从 1.77ms 涨到
// 26ms，而近场太阳阴影图只有 0.05ms —— 账全在这条**逐像素**的太阳可见性
// 射线上。两个效应同时放大：
//   1) 门控是 `NdotL > 0`（见上方 2026-10-06 注释）：正午太阳在天顶，竖直面
//      NdotL <= 0 ⇒ 直接跳过射线；日出日落太阳贴地，所有朝太阳的竖直面都
//      > 0 ⇒ 屏幕上一大半像素都要发。
//   2) 射线是水平的，TerminateOnFirstHit 靠"撞到东西"早停；开阔地/高处撞
//      不到，于是要横穿 tMax = 10000 格。正午垂直下打，几十格内就被地面
//      挡住，两项都不成立。
//
// 修法只动 tMax，且**只对远场像素**（rtLodDistance >= 64 格）：
//   太阳高度 < 0.03（约 1.7°）时收到 384 格（24 区块），升到约 20° 时完全
//   回到 10000 —— 白天逐位不变；近景（< 64 格，玩家实际盯着的范围）恒
//   10000，近处长阴影逐位不变。被截掉的只是"低太阳角下 24 区块之外的阴影"，
//   那部分本来就被大气透视压得很淡。
// 回退：把 shadowRayMaxDistance 换回字面量 10000.0 即逐字回到改动前。
// =====================================================
float sunHeight01 =
    clamp(sunDir.y / 0.81915204429, 0.0, 1.0);

// 2026-11-01（第二轮）：门槛 64 -> 16 格、封顶 384 -> 160 格。
//
// 依据（NVIDIA《最佳实践：使用 NVIDIA RTX 光线跟踪》）：背面/正面剔除在光追里
// **不是优化**，反而更慢（与光栅化相反），所以不走剔除路线；有效的只有
// TerminateOnFirstHit（已用）、缩短 tMax、降低每像素射线数（UE 的
// r.RayTracing.Shadows.SamplesPerPixel 默认 1，与本工程一致，无余量）。
//
// 160 = 近场太阳阴影图的半径（SUN_SHADOW_EXTENT_XY）。封顶超过它就只是
// "用一条昂贵的逐像素射线去复算一张已经有的图"，没有新增信息。
// 16 格内的像素仍保留 10000（第一人称近景的长阴影逐位不变）。
// 2026-11-01（第三轮，用户定案）：封顶**固定 10 区块 = 160 格**。
//
// 作废的两个方向（记在这里，避免以后重复讨论）：
//   1) "跟随远平面放宽到 320 格" —— 射线变长一倍，只是把漏阴影的分界推到更远，
//      远处阴影该有的问题并没有解决；
//   2) "按显存预算分档（extent/分辨率）" —— extent 同时喂 BLAS 实例保留盒
//      （VulkanCore.cpp:69188 / 69569），放大 2.4 倍会让**所有** RT pass 的
//      TLAS 变大，不是免费的质量提升。
//
// 定案：光追阴影只负责 10 区块内；8 区块以外将来改走**光栅阴影**（计划中），
// 所以这条射线的覆盖范围以后只会缩、不会涨。
const float shadowRayCap = 160.0;

float shadowRayMaxDistance =
    (rtLodDistance >= 16.0 * perfL10TierScale())
        ? mix(10000.0, shadowRayCap, 1.0 - smoothstep(0.03, 0.35, sunHeight01))
        : 10000.0;


for (int si = 0;
     si < shadowSamples;
     ++si)
{
    // =====================================================
    // 2026-11-12 随机太阳盘方向（Caustica 的 RR-only 路径）
    //   · 每帧、每 tap 在太阳盘内取随机点，面积均匀（sqrt(r2)）
    //   · 孔径走 cam.sssParams.y（宿主 ORION_SUNVIS_APERTURE，默认 0.030）
    //     ⇒ 软度可以**只重启、不重编**地调
    //   · 与 RR 的时域累积配合：N tap 平均把逐帧噪声压到 1/sqrt(N)，
    //     残余抖动交给 DLSS-RR 收敛成连续半影
    // 回退：把 shadowDir 换回 normalize(sunDir) 即逐位硬边旧档。
    // =====================================================
    vec3 shadowDir;
#if SUN_SHADOW_HARD
    shadowDir = normalize(sunDir);
#else
    {
        vec3 diskAxisN = normalize(sunDir);
        vec3 diskUp =
            abs(diskAxisN.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        vec3 diskAxisT = normalize(cross(diskUp, diskAxisN));
        vec3 diskAxisB = cross(diskAxisN, diskAxisT);

        float sunDiskAperture =
            (cam.sssParams.y > 1.0e-6)
                ? cam.sssParams.y
                : SUN_SHADOW_SOFT_APERTURE;

#ifndef SUN_SHADOW_BLUE_NOISE
#define SUN_SHADOW_BLUE_NOISE 1
#endif
#if SUN_SHADOW_BLUE_NOISE
        float sunDiskRand0 = glassBlueNoiseSample(
            uvec2(gl_LaunchIDEXT.xy), cam.frameCount, 71u + uint(si) * 2u);
        float sunDiskRand1 = glassBlueNoiseSample(
            uvec2(gl_LaunchIDEXT.xy), cam.frameCount, 72u + uint(si) * 2u);
        // shadowSamples > 1 时按扇区等分角空间（2-tap 落在太阳盘相对两半，免费降方差）
        float sunDiskAngle = 6.2831853 *
            ((float(si) + sunDiskRand0) / float(max(shadowSamples, 1)));
        float sunDiskRadius = sqrt(sunDiskRand1) * sunDiskAperture;
#else
        float sunDiskRand0 = rand(shadowSeed);
        float sunDiskRand1 = rand(shadowSeed);
        float sunDiskAngle = 6.2831853 * sunDiskRand0;
        float sunDiskRadius = sqrt(sunDiskRand1) * sunDiskAperture;
#endif

        shadowDir = normalize(
            diskAxisN +
            (diskAxisT * cos(sunDiskAngle) + diskAxisB * sin(sunDiskAngle)) *
                sunDiskRadius);
    }
#endif

    // 2026-11-11 形态1：唯一一条射线必须走太阳**中心**方向。否则原始 mask 会整体
    // 偏移半个半影（滤波的几何前提是"中心射线 + 解析半影"）。
    if(sunVisibilityFilterActive)
    {
        shadowDir = normalize(sunDir);
    }

    vec3 shadowOrigin =
        hitPos + primaryGeometricNormal * 0.015 + shadowDir * SUN_SHADOW_RAY_ORIGIN_OFFSET;


    hit.hitDistance =
        1.0;

    hit.albedoPacked = packAlbedoUnorm(
        vec3(1.0));

    hit.parallaxRayTravel =
        0.0;

    hit.glassAlpha = 0.0;

    hit.state = makeTraceState(RT_PURPOSE_GLASS_VISIBILITY);
    // [13] 太阳软影射线：循环体每根计一次（SER / 非 SER 两条分支共用同一口径，
    // 所以放在分支之前，而不是各自的 trace 调用旁）。
    perfRayStatAdd(13u, 1u);
    // 2026-10-23：可见性射线必须从 albedo = vec3(1.0) 起算 ——
    // 见 HANDOFF-2026-10-21-VERTEX-32B-TERRAIN.md §8。否则累乘起点是
    // 上一条射线的残留 albedo，逐帧不同 ⇒ 静止时被 DLSS-RR 当信号累积。
    hit.albedoPacked = packAlbedoUnorm(vec3(1.0));

    bool sunShadowHandledByQuery = false;
#if ORION_OPAQUE_VIS_FASTPATH && !defined(ORION_K2I_COMPOSE_COMPUTE)
    if(!sunVisibilityFilterActive)
    {
        float queryHitDist = 1.0;
        if(tryOpaqueSunVisibilityQuery(
            shadowOrigin,
            shadowDir,
            shadowRayMaxDistance,
            queryHitDist))
        {
            hit.hitDistance = queryHitDist;
            sunShadowHandledByQuery = true;
        }
    }
#endif

    if(!sunShadowHandledByQuery)
    {
    // 2026-11-27 (Batch AI): visibility-only sun shadow skips SER.
    {
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        traceRayEXT(
            topLevelAS,
            gl_RayFlagsTerminateOnFirstHitEXT |
            (sunVisibilityFilterActive
                ? 0u
                : gl_RayFlagsSkipClosestHitShaderEXT),
            g_secondaryCullMask,
            0,
            0,
            0,
            // Visibility starts along its actual outgoing ray, never along the
            // POM/normal-map shading normal.  The latter can cross a block edge
            // and turn a blocked sun ray into a false bright seam.
            shadowOrigin,
            SUN_SHADOW_RAY_TMIN,
            shadowDir,
            shadowRayMaxDistance,
            0
        );
    }
    }


    // =================================================
    // 2026-11-01 太阳可见性的体素保守门（10-02 机制延伸到日/月直射）。默认开启。
    //
    // 病灶（用户 2026-10-31 三个判据 + 2026-11-01 档 32 消融实证"AB有效，没缝了"）：
    // 亮线只出现在**不同高度的阶梯凹角**、共面交界没有、无材质包也有 —— 即
    // 2026-10-02 注释记录的同一几何缺陷（"相邻块四边形只在棱上共边，精度下可以
    // 从棱缝擦过去"）的**日/月直射版本**：贴着阶梯凹角共享棱擦过的低角度日/月
    // 可见性射线，穿过 1 格厚的阶梯实体误报"直射无遮挡" ⇒ 沿棱的日/月色亮线
    // （亮度随太阳高度角变化）。共面交界不可见的机理：擦进板内的射线会被板
    // 另一侧的面挡住（face-forward 材质着色为暗），而阶梯角射线贴棱穿过实体后
    // 到达的是真正朝向天空的通道。
    //
    // 修法与本地光 10-02 修法同源：硬件射线报"通"时，用体素 DDA 保守复检同一条
    // 路径 —— 穿实心体素（class==1）或贴棱/贴角通过（VPT_VOXEL_PATH_CORNER_EPS）
    // 判遮挡。体素 DDA 不依赖网格棱的水密性。
    //
    // 2026-11-01 转正：默认开启（档 0 即生效）。显式关闭臂 = 档 33
    // （ORION_PERF_ABLATION=alt:33，运行时 A/B，无需重编；档 33 原为空闲档）。
    // 档 32 同样保持开启（= 默认档），其宿主侧名字 glossyBudgetOff 是遗留
    // 空操作（kGlossyRayBudget 已是 10000，:18825），无冲突。
    // 16 格门限：漏光射线在最初 1~2 格内就会穿过阶梯实体，16 格覆盖充分，
    // 同时把户外每样本的 DDA 成本压到最低。
    // 已知副作用（与本地光 10-02 修复同类、可接受）：户外贴着体素棱/角通过的
    // 太阳射线会被保守判挡，棱线附近直射可能偏暗一档。
    // =================================================
    // 2026-11-05：体素保守门的射程从 16 格收到 3 格。
    // 病灶（用户截图：阳光照到的平地上一张网格）：波及判据用的
    // 8-7-1 误块（corner eps）在开阔地上也会命中——低太阳角时射线虚弹得很平，
    // 沿体素格朝线擦过去，每隔一格就被保守地判一次遮挡 ⇒ 暗线成网。
    // 漏光射线本来就只在最初 1~2 格内穿过阶梯实体，
    // 16 格走完只是多了 14 格误判机会。
    // A/B：-DSUN_SHADOW_VOXEL_GATE_RANGE=16.0 回到旧行为；=0 直接关掉。
#ifndef SUN_SHADOW_VOXEL_GATE_RANGE
#define SUN_SHADOW_VOXEL_GATE_RANGE 3.0
#endif
#ifndef SUN_SHADOW_VOXEL_GATE_MAX_DIST
#define SUN_SHADOW_VOXEL_GATE_MAX_DIST 48.0
#endif
// 注意：#if 只能算整数，所以开关用单独的 int 宏。
#ifndef SUN_SHADOW_VOXEL_GATE
#define SUN_SHADOW_VOXEL_GATE 1
#endif
#if SUN_SHADOW_VOXEL_GATE
    if(hit.hitDistance < 0.0 &&
       rtLodDistance < SUN_SHADOW_VOXEL_GATE_MAX_DIST &&
       cam.perfAblationStage.x != 33u &&
       vptVoxelPathBlocked(
           hitPos,
           hitPos + shadowDir * SUN_SHADOW_VOXEL_GATE_RANGE,
           ivec3(1000000, 1000000, 1000000),
           SUN_SHADOW_VOXEL_GATE_RANGE
       ))
    {
        // 体素保守门判遮挡：这次"硬件报通"的样本按被挡处理（不累加透射）。
        hit.hitDistance = 1.0;
    }
#endif

    if(hit.hitDistance < 0.0)
    {
        shadowTransmittance +=
            clamp(
                unpackAlbedoUnorm(hit.albedoPacked),
                vec3(0.0),
                vec3(1.0)
            );

    }
}


// 按实际发出的样本数归一化（0 样本时按 1 处理，避免 0/0）。
if(useNearShadowMap)
{
    // 2026-11-01 档 2：这一格的可见性来自近场图，没有射线样本可归一化。
    shadowTransmittance = vec3(nearShadowMapVisibility);
}
else
{
    shadowTransmittance /=
        max(
            float(shadowSamples),
            1.0
        );
}

// =====================================================
// 2026-11-11 形态1：写原始可见度 + 用上一帧的滤波结果替换可见度
//
// 本次唯一一条射线的结果：
//   miss ⇒ 全亮，hit.albedoPacked 是穿透玻璃累乘出来的颜色（彩色影子）；
//   hit  ⇒ 遮挡，hit.hitDistance 由 any-hit 写入的 gl_HitTEXT（遮挡物距离）。
// 上一帧的滤波结果在半分辨率 ping-pong 里，按 MV（cam.prevViewProj）重投影，
// 并用**视深一致性**做 disocclusion 判定；不通过就回落到本次原始值。
// =====================================================
if(sunVisibilityFilterActive)
{
    const bool sunRayMissed =
        hit.hitDistance < 0.0;

    const vec3 sunRayTint =
        clamp(
            unpackAlbedoUnorm(hit.albedoPacked),
            vec3(0.0),
            vec3(1.0));

    const float rawVisibility =
        sunRayMissed ? 1.0 : 0.0;

    const float sunBlockerDistance =
        sunRayMissed ? 512.0 : max(hit.hitDistance, 0.0);

    const vec3 sunVisCameraPos =
        cam.viewInverse[3].xyz;

    const vec3 sunVisForward =
        -normalize(cam.viewInverse[2].xyz);

    const float sunVisViewZ =
        max(dot(hitPos - sunVisCameraPos, sunVisForward), 0.0);

    imageStore(
        sunVisRaw,
        pixelCoord,
        vec4(rawVisibility, sunBlockerDistance, sunVisViewZ, 0.0));

    float sunVisibility = rawVisibility;

    if(cam.historyValid != 0u && sunVisViewZ > 1.0e-4)
    {
        const vec4 sunPrevClip =
            cam.prevViewProj * vec4(hitPos, 1.0);

        if(sunPrevClip.w > 1.0e-5)
        {
            const vec2 sunPrevUV =
                sunPrevClip.xy / sunPrevClip.w * vec2(0.5, -0.5) + vec2(0.5);

            if(sunPrevUV.x > 0.0 && sunPrevUV.x < 1.0 &&
               sunPrevUV.y > 0.0 && sunPrevUV.y < 1.0)
            {
                const ivec2 sunHistorySize =
                    imageSize(sunVisHistory[0]);

                const vec2 sunHalfCoord =
                    sunPrevUV * vec2(sunHistorySize) - vec2(0.5);

                const ivec2 sunBase = ivec2(floor(sunHalfCoord));
                const vec2 sunFrac = sunHalfCoord - vec2(sunBase);

                float filteredVisibility = 0.0;
                float filteredDepth = 0.0;
                float filterWeightSum = 0.0;

                for(int ty = 0; ty < 2; ++ty)
                {
                    for(int tx = 0; tx < 2; ++tx)
                    {
                        const ivec2 q = clamp(
                            sunBase + ivec2(tx, ty),
                            ivec2(0),
                            max(sunHistorySize - ivec2(1), ivec2(0)));

                        const vec4 h = imageLoad(
                            sunVisHistory[cam.historyReadIndex & 1u],
                            q);

                        const float w =
                            (tx == 0 ? (1.0 - sunFrac.x) : sunFrac.x) *
                            (ty == 0 ? (1.0 - sunFrac.y) : sunFrac.y);

                        filteredVisibility += h.x * w;
                        filteredDepth += h.y * w;
                        filterWeightSum += w;
                    }
                }

                // 重投影落到别的表面（视深不符）⇒ 当作 disocclusion。
                const bool depthAgrees =
                    filterWeightSum > 1.0e-5 &&
                    abs(filteredDepth / filterWeightSum - sunVisViewZ) <=
                        0.1 * sunVisViewZ;

                if(depthAgrees)
                {
                    sunVisibility =
                        clamp(filteredVisibility, 0.0, 1.0);
                }
            }
        }
    }

    shadowTransmittance =
        sunRayTint * sunVisibility;
}

// 2026-11-06 诊断（探针 D）：完全取消太阳遮挡。
// 用于判定“方块边沿的硬细黑线”到底在不在阴影项里：
//   线消失 ⇒ 是阴影项；线还在 ⇒ 是别的逐格乘子。
#ifndef SUN_SHADOW_DEBUG_NO_SHADOW
#define SUN_SHADOW_DEBUG_NO_SHADOW 0
#endif
#if SUN_SHADOW_DEBUG_NO_SHADOW
shadowTransmittance = vec3(1.0);
#endif

// One vertical visibility query is required only for a rain-active primary
// solid surface.  The existing GLASS_VISIBILITY Any-Hit path preserves
// glassAlpha=1 through clear/stained glass, so roofs and glass canopies keep
// blocking rain even if their RGB transmission is white.
//
// A wall only needs this query while its run-off is actually resolvable: up
// close.  Beyond the effect distance the wall stays dry rather than paying a
// ray per pixel, and rain fog already hides the difference there.
bool rainWallExposureQuery =
    rainWallEnabled() &&
    rainWallSurfaceCandidate &&
    rtLodDistance < rainWallExposureDistance();

if(
    (rainTopSurfaceCandidate || rainWallExposureQuery) &&
    getOverworldRainStrength() > 0.01
)
{
    // Start the wall ray slightly off the surface along its own normal so it
    // cannot re-intersect the face it started from.  The top-face ray keeps
    // its original fixed +Y offset.
    vec3 rainExposureOrigin =
        rainTopSurfaceCandidate
            ? hitPos + vec3(0.0, 0.002, 0.0)
            : hitPos + primaryGeometricNormal * 0.002;

    // The top-face query has always run to its full 10000 range, so it keeps
    // that behaviour.  The wall query is new and bounded instead: anything
    // overhead that could matter sits far closer than the cap, and the cap
    // stops an open-sky wall from paying a full-height traversal.
    float rainExposureTMax =
        rainTopSurfaceCandidate
            ? 10000.0
            : rainWallExposureRange();

    // =====================================================
    // Soft sky exposure
    //
    // This query used to be one binary ray: blocked overhead meant no
    // exposure at all, a miss meant full exposure.  The dry region was
    // therefore the exact silhouette of whatever stood overhead, its border
    // locked to block edges, and a single block above cut a whole face off
    // at once -- a hard-edged rectangle.
    //
    // Now the centre ray decides first: a miss still returns 1 outright, and
    // that is the overwhelmingly common case (open sky), so it keeps paying
    // a single long ray.  Only a blocked centre triggers a ring of taps in
    // the plane perpendicular to the query, and their escape ratio becomes
    // the exposure.  Exposure then varies across one face, the rim under an
    // overhang becomes a gradient roughly 2 * RAIN_EXPOSURE_SOFT_RADIUS wide,
    // and right angles of a roof silhouette are rounded by that radius.
    //
    // A tap whose origin lands in a solid column is left blocked: that column
    // genuinely cannot be rained on, which is the same verdict the single
    // ray reached, so no special case is needed for it.
    //
    // Setting RAIN_EXPOSURE_SOFT_DISTANCE to 0 restores the previous binary
    // behaviour exactly, since the ring never runs and the centre ray is the
    // whole query.
    // =====================================================
    const int RAIN_EXPOSURE_SOFT_TAPS = 8;
    const float RAIN_EXPOSURE_SOFT_RADIUS = 0.55;
    const float RAIN_EXPOSURE_SOFT_DISTANCE = 48.0;
    // 2026-10-23：这一族射线只寻址 bit 0x08 —— 所有普通实例都是 0xff（含 0x08），
    // 只有粒子/天气层被清掉了这一位（VulkanCore.cpp 的 TLAS 遮罩分支）。
    // 目的：雨丝不挡"天空可见度"（物理正确），同时让这根每像素 1~9 根的
    // 长射线不再穿过相机周围那块 7k 三角形的粒子 BLAS。
    const uint RAIN_EXPOSURE_RAY_MASK = 0x18u;

    uint rainExposureSoftFlags =
        gl_RayFlagsTerminateOnFirstHitEXT |
        gl_RayFlagsSkipClosestHitShaderEXT;

    hit.hitDistance =
        1.0;

    hit.albedoPacked = packAlbedoUnorm(
        vec3(1.0));

    hit.parallaxRayTravel =
        0.0;

    hit.glassAlpha = 0.0;

    hit.state =
        makeTraceState(RT_PURPOSE_GLASS_VISIBILITY);
    // 2026-10-23：可见性射线必须从 albedo = vec3(1.0) 起算 ——
    // 见 HANDOFF-2026-10-21-VERTEX-32B-TERRAIN.md §8。否则累乘起点是
    // 上一条射线的残留 albedo，逐帧不同 ⇒ 静止时被 DLSS-RR 当信号累积。
    hit.albedoPacked = packAlbedoUnorm(vec3(1.0));

    // 2026-11-27 (Batch AI): visibility-only rain exposure skips SER.
    {
        #ifdef ORION_RT_STAGE_COUNTERS
        rtCntRayIssued();
        #endif
        traceRayEXT(
            topLevelAS,
            rainExposureSoftFlags,
            RAIN_EXPOSURE_RAY_MASK,
            0,
            0,
            0,
            rainExposureOrigin,
            0.02,
            vec3(0.0, 1.0, 0.0),
            rainExposureTMax,
            0
        );
    }

    bool rainExposureCenterEscaped =
        payload.hitDistance < 0.0 &&
        payload.glassAlpha < 0.5;

    int rainExposureTaps =
        1;

    float rainExposureEscape =
        rainExposureCenterEscaped
            ? 1.0
            : 0.0;

    // Only a blocked centre is worth the ring: those rays are short, because
    // the blocker sits directly above the point being shaded.
    if(
        !rainExposureCenterEscaped &&
        rtLodDistance < RAIN_EXPOSURE_SOFT_DISTANCE
    )
    {
        // An upward face spreads its taps over the horizontal disc.  A wall
        // only spreads them along its own tangent: an offset along the wall
        // normal would push the origin inside the wall block and that column
        // would then read as self-occluded, halving every open wall's
        // wetness.
        vec3 rainExposureTapA =
            rainTopSurfaceCandidate
                ? vec3(1.0, 0.0, 0.0)
                : (
                    abs(primaryGeometricNormal.x) > 0.5
                        ? vec3(0.0, 0.0, 1.0)
                        : vec3(1.0, 0.0, 0.0)
                );

        vec3 rainExposureTapB =
            rainTopSurfaceCandidate
                ? vec3(0.0, 0.0, 1.0)
                : vec3(0.0, 0.0, 0.0);

        for(int ti = 0; ti < RAIN_EXPOSURE_SOFT_TAPS; ++ti)
        {
            // Half-offset angles spread the ring evenly instead of leaving a
            // tap on each axis, and collapse to a symmetric one-dimensional
            // kernel when the second tangent is empty.
            float tapAngle =
                (float(ti) + 0.5) *
                (6.2831853072 / float(RAIN_EXPOSURE_SOFT_TAPS));

            vec3 tapOrigin =
                rainExposureOrigin +
                (
                    rainExposureTapA * cos(tapAngle) +
                    rainExposureTapB * sin(tapAngle)
                ) *
                RAIN_EXPOSURE_SOFT_RADIUS;

            hit.hitDistance =
                1.0;

            hit.albedoPacked = packAlbedoUnorm(
                vec3(1.0));

            hit.parallaxRayTravel =
                0.0;

            hit.glassAlpha = 0.0;

            hit.state =
                makeTraceState(RT_PURPOSE_GLASS_VISIBILITY);
            // 2026-10-23：可见性射线必须从 albedo = vec3(1.0) 起算 ——
            // 见 HANDOFF-2026-10-21-VERTEX-32B-TERRAIN.md §8。否则累乘起点是
            // 上一条射线的残留 albedo，逐帧不同 ⇒ 静止时被 DLSS-RR 当信号累积。
            hit.albedoPacked = packAlbedoUnorm(vec3(1.0));

            // 2026-11-27 (Batch AI): visibility-only rain exposure ring skips SER.
            {
                #ifdef ORION_RT_STAGE_COUNTERS
                rtCntRayIssued();
                #endif
                traceRayEXT(
                    topLevelAS,
                    rainExposureSoftFlags,
                    RAIN_EXPOSURE_RAY_MASK,
                    0,
                    0,
                    0,
                    tapOrigin,
                    0.02,
                    vec3(0.0, 1.0, 0.0),
                    rainExposureTMax,
                    0
                );
            }

            if(
                payload.hitDistance < 0.0 &&
                payload.glassAlpha < 0.5
            )
            {
                rainExposureEscape +=
                    1.0;
            }
        }

        rainExposureTaps +=
            RAIN_EXPOSURE_SOFT_TAPS;
    }

    rainSkyExposure =
        clamp(
            rainExposureEscape /
                float(rainExposureTaps),
            0.0,
            1.0
        );
}


// POM self-shadow remains scalar and multiplies the RGB light path.
shadowTransmittance *=
    primaryPOMSelfShadow;


float shadowVisibility =
    max(
        max(
            shadowTransmittance.r,
            shadowTransmittance.g
        ),
        shadowTransmittance.b
    );

// Block 1: continuous rain wetness across every exposed solid top face.
// Spatial modulation keeps the film natural, while rain coverage is not
// limited to isolated puddle islands.
float rainGroundWetness =
    0.0;

if(
    rainTopSurfaceCandidate &&
    rainSkyExposure > 0.0
)
{
    const float REFERENCE_DEFAULT_POROSITY = 0.2;
    // 2026-11-21 修复③：0.4 → 0.9。旧档满潮湿只暗 albedo 的
    // wetness·√0.2·0.4 ≈ 18% —— 雨天全场景本来就暗，用户实测"地面潮湿
    // 几乎看不见"。提到 0.9 ⇒ 满潮湿 ~40% 变暗，与参考包的孔隙吸收
    // 上限口径一致。
    const float REFERENCE_POROSITY_ABSORPTION = 0.9;

    rainGroundWetness =
        getOverworldRainStrength() *
        rainSkyExposure;
    rainGroundWetness *=
        evaluateContinuousRainWetnessNoise(hitPos.xz);
    rainGroundWetness = clamp(
        rainGroundWetness *
            (2.05 - REFERENCE_DEFAULT_POROSITY) -
        REFERENCE_DEFAULT_POROSITY * 0.2,
        0.0,
        1.0
    );
    // The candidate is an axis-aligned geometric top face, so its equivalent
    // reference v_tbn[2].y is exactly 1.0.
    rainGroundWetness *=
        clamp(1.0 * 0.5 + 0.5, 0.0, 1.0);

    albedo *=
        1.0 -
        max(rainGroundWetness - shoreWetnessApplied, 0.0) *
        sqrt(REFERENCE_DEFAULT_POROSITY) *
        REFERENCE_POROSITY_ABSORPTION;
}

if(rainGroundWetness > 0.0)
{
    // This renderer stores physical roughness (0 = smooth), while the
    // reference pushes its packed smoothness channel toward 1.  Keep the
    // already verified smooth-film target and drive it with the continuous
    // continuous rain-film coverage.
    roughness = mix(roughness, 0.060, rainGroundWetness);
    normal = normalize(
        mix(
            normal,
            vec3(0.0, 1.0, 0.0),
            clamp(rainGroundWetness * 2.0 - 1.0, 0.0, 1.0)
        )
    );

    // Reuse the existing analytic rain-impact field.  It performs no texture
    // lookup, allocation, compute dispatch or ray query.  The 48-block limit
    // avoids its nine-cell gradient loop where the reference splash fade is
    // already visually small.
    if(rtLodDistance < 48.0)
    {
        vec2 rainPuddleRippleGradient =
            evaluateRainPuddleRippleGradient(hitPos.xz) *
            rainGroundWetness;

        normal = normalize(
            normal +
            vec3(
                -rainPuddleRippleGradient.x,
                0.0,
                -rainPuddleRippleGradient.y
            )
        );
    }

    NdotL = max(0.0, dot(normal, sunDir));

    // PBR-2 originally evaluated its tracing gate before this block, from
    // the dry payload roughness.  Re-evaluate only after rain has changed
    // roughness so the existing GGX path actually traces the wet film.
    pbr2ReflectionStrength = evaluatePBR2ReflectionStrength(
        roughness,
        f0MetalCode
    );
}

// =====================================================
// Block 2: rain run-off on exposed solid wall faces.
//
// Same no-allocation model as the wet ground: the pattern is evaluated from
// world-space coordinates only, so it allocates nothing, samples no texture
// and stays stable under RR/DLSS reprojection.  It runs on an already
// visible, rain-exposed vertical solid face, and it never overlaps block 1
// because a face cannot be both upward and vertical.
// =====================================================
float rainWallWetness =
    0.0;

float rainWallDripShape =
    0.0;

// Heartfelt 场的 (tangent, height) 梯度，仅在 drip 路径算出形状时非零，
// 应用处用它把法线沿切平面倾斜。
vec2 rainWallDripGradient =
    vec2(0.0);

if(
    rainWallEnabled() &&
    rainWallSurfaceCandidate &&
    rainSkyExposure > 0.0 &&
    rtLodDistance < rainWallWetnessDistance()
)
{
    // The wall is axis aligned, so its surface parameterisation is the
    // horizontal tangent axis plus world Y.  A screen-space domain would
    // swim as soon as the camera moves; a world-space one stays glued to the
    // block.
    //
    // Both faces of a one-block-thick wall share that (tangent, height)
    // domain, so without the facing offset they would show the identical
    // pattern wherever both are visible at once, for instance through a
    // doorway.  Folding the facing sign into the tangent coordinate breaks
    // that symmetry for one compare and one add.
    float faceOffset =
        primaryGeometricNormal.x + primaryGeometricNormal.z > 0.0
            ? 0.0
            : 31.7;

    vec2 wallUV =
        abs(primaryGeometricNormal.x) > 0.5
            ? vec2(hitPos.z + faceOffset, hitPos.y)
            : vec2(hitPos.x + faceOffset, hitPos.y);

    float rain =
        getOverworldRainStrength();

    // Static film: the same continuous noise the wet ground uses, evaluated
    // on (tangent, height) so wall and ground read as one rain field rather
    // than two separate effects.
    rainWallWetness =
        clamp(
            rain *
                rainSkyExposure *
                rainWallWetnessScale() *
                evaluateContinuousRainWetnessNoise(wallUV),
            0.0,
            1.0
        );

    // Each dial is a UBO read behind an inlined clamp; the compiler folds the
    // repeated loads of the same lane, so they are called at their use sites
    // rather than hoisted into locals that then need naming.
    if(
        rainWallDripDensity() > 0.0 &&
        rtLodDistance < rainWallDripDistance()
    )
    {
        float rainWallTime =
            uintBitsToFloat(cam.padding);

        // Heartfelt droplet field（2026-10-07：旧的水柱 trail 档已退役，这一段不再有
        // #if/#else 回退）。rainAmount drives the three layer weights
        // exactly like the original shader's rainAmount does; the density dial
        // multiplies into it so the slider still goes from "almost nothing"
        // to "full downpour".
        float rainAmount =
            clamp(rain * rainWallDripDensity() * 1.2, 0.0, 1.0);

        float layer0Weight =
            smoothstep(-0.5, 1.0, rainAmount) * 2.0;

        float layer1Weight =
            smoothstep(0.25, 0.75, rainAmount);

        float layer2Weight =
            smoothstep(0.0, 0.5, rainAmount);

        float dripTime =
            rainWallTime * RAIN_WALL_HEARTFELT_TIME_SCALE *
                rainWallDripSpeedScale();

        // 2026-10-07：大小旋钮每帧读一次（见 rainWallDripSize 的说明），三处调用共用。
        float dripSize =
            rainWallDripSize();

        vec2 heartField =
            heartfeltDrops(
                wallUV, dripTime, layer0Weight, layer1Weight, layer2Weight,
                dripSize);

        // 拖尾并入场域：shape（法线幅度 + roughness 档）与梯度共用这一份标量，
        // 拖尾因此既参与法线弯折（细长高光）也吃湿膜 roughness，而不是只有主水珠。
        float dripField =
            heartField.x + heartField.y * RAIN_WALL_HEARTFELT_TRAIL;

        // Sub-pixel droplets read as flicker at distance, so the whole
        // animated field fades out over the inner half of the drip range.
        float dripNearFade =
            1.0 - smoothstep(
                rainWallDripDistance() * 0.5,
                rainWallDripDistance(),
                rtLodDistance
            );

        rainWallDripShape =
            dripField * rain * rainSkyExposure * dripNearFade;

        if(rainWallDripShape > 0.001)
        {
            vec2 gradientEpsilon =
                vec2(RAIN_WALL_HEARTFELT_NORMAL_EPSILON, 0.0);

            vec2 offsetX =
                heartfeltDrops(
                    wallUV + gradientEpsilon,
                    dripTime,
                    layer0Weight,
                    layer1Weight,
                    layer2Weight,
                    dripSize
                );

            vec2 offsetY =
                heartfeltDrops(
                    wallUV + gradientEpsilon.yx,
                    dripTime,
                    layer0Weight,
                    layer1Weight,
                    layer2Weight,
                    dripSize
                );

            rainWallDripGradient =
                vec2(
                    offsetX.x + offsetX.y * RAIN_WALL_HEARTFELT_TRAIL -
                        dripField,
                    offsetY.x + offsetY.y * RAIN_WALL_HEARTFELT_TRAIL -
                        dripField
                );
        }
    }
}

if(rainWallWetness > 0.0)
{
    albedo *=
        1.0 -
        max(rainWallWetness - shoreWetnessApplied, 0.0) * RAIN_WALL_ALBEDO_DARKENING;

    roughness =
        mix(roughness, RAIN_WALL_FILM_ROUGHNESS, rainWallWetness);

    if(rainWallDripShape > 0.0)
    {
        float dripAmount =
            clamp(rainWallDripShape, 0.0, 1.0);

        // Droplets are geometry, not paint: bend the normal and let the
        // highlight do the work.  Darkening the albedo further would just
        // read as dirt.
        // 2026-10-07：旧档那条（整列往外顶的 bump）已随"水柱"模型删除，只剩这一条。
        // The field gradient tilts the normal across the droplet surface
        // (tangent plane), which is what gives the Heartfelt look its rounded
        // highlight instead of a uniform outward bulge.
        vec3 dripTangent =
            abs(primaryGeometricNormal.x) > 0.5
                ? vec3(0.0, 0.0, 1.0)
                : vec3(1.0, 0.0, 0.0);

        normal = normalize(
            normal -
            (dripTangent * rainWallDripGradient.x +
             vec3(0.0, 1.0, 0.0) * rainWallDripGradient.y) *
                (RAIN_WALL_HEARTFELT_BUMP * rainWallDripBumpScale() *
                 RAIN_VISIBILITY_GAIN)   // 2026-10-03 雨可见度 ×2
        );

        roughness =
            mix(roughness, RAIN_WALL_DRIP_ROUGHNESS, dripAmount);
    }

    NdotL = max(0.0, dot(normal, sunDir));

    // PBR-2 evaluated its tracing gate from the dry payload roughness before
    // this block.  Re-evaluate it here for the same reason block 1 does, so
    // the GGX path actually traces the wet wall.
    pbr2ReflectionStrength = evaluatePBR2ReflectionStrength(
        roughness,
        f0MetalCode
    );
}

// =====================================================
// A1 - GGX Direct Sun Lighting
//
// 保留：
// - 8 ray soft shadow
// - shadowVisibility
// - sun intensity 3.2
//
// 新增：
// - LabPBR F0
// - GGX NDF
// - Smith geometry
// - Schlick Fresnel
// - metallic / dielectric energy split
// =====================================================

vec3 directLight =
    vec3(0.0);


if(
    NdotL > 0.0 &&
    NdotV > 0.0 &&
    shadowVisibility > 0.0
)
{
    vec3 directL =
        normalize(
            sunDir
        );


    vec3 directH =
        normalize(
            V +
            directL
        );


    // -------------------------------------------------
    // Direct-light F0
    //
    // 这里单独使用完整 LabPBR metal 解码。
    // 不修改当前 GI 的 materialF0，
    // 因此不会提前影响 A3。
    // -------------------------------------------------

    vec3 directF0 =
        decodeBaseF0(
            f0MetalCode,
            albedo
        );


    float directVdotH =
        max(
            dot(
                V,
                directH
            ),
            0.0
        );


    BsdfTerms directBsdf =
        evalBSDFTerms(
            normal,
            V,
            directL,
            directF0,
            metallic,
            roughness,
            NdotV,
            true
        );


    // -------------------------------------------------
    // Energy split
    //
    // dielectric:
    // diffuse + specular
    //
    // metal:
    // diffuse -> 0
    // specular only
    // -------------------------------------------------

    // PBR-1B:
    // The current Primary path did NOT contain the helper's 1.2 scale.
    // Preserve that exact Primary brightness baseline and only replace
    // Lambert NdotL with the Burley angular/roughness response.
    vec3 directDiffuse =
        directBsdf.kD *
        albedo *
        evaluateBurleyDiffuseAngular(
            normal,
            V,
            directL,
            roughness
        );

#ifndef GI_OREN_NAYAR_RETRO
// 2026-10-05 粗糙漫反射（#7）：在 Burley 之上叠加 Oren-Nayar 的逆反射瓣
// （ qualitative 版的 B·cos(φ差) 项）。粗糙表面（砂岩/混凝土/粉末）在
// "光在观察者背后"的角度回光更亮、掠角更平 —— 与光滑塑料区分。光滑材质
// （roughness → 0）系数归零 ⇒ 逐位旧档。~15 ALU，零 trace。
#define GI_OREN_NAYAR_RETRO 1
#endif
#if GI_OREN_NAYAR_RETRO
    {
        float onSigma2 = roughness * roughness;
        float onB = 0.45 * onSigma2 / (onSigma2 + 0.09);
        if(onB > 1.0e-4)
        {
            vec3 onV = normalize(V - normal * NdotV);
            vec3 onL = normalize(directL - normal * NdotL);
            float onRetro = clamp(dot(onV, onL), 0.0, 1.0);
            directDiffuse +=
                directBsdf.kD *
                albedo *
                (onB * onRetro * NdotL * 0.35);
        }
    }
#endif


    vec3 directSpecular =
        directBsdf.specular *
        NdotL;


    if(max(shadowTransmittance.r, max(shadowTransmittance.g, shadowTransmittance.b)) > 1.0e-5)
    {
        vec3 sunRadiance =
            getTimeDrivenSkyLightColor() *
            3.2 *
            getTimeDrivenSkyLightRadianceScale() *
            shadowTransmittance *
            c3CloudSunTransmittance(hitPos + directL * 0.001, directL);


        directLight =
            sunRadiance *
            (
                directDiffuse +
                directSpecular
            );
    }
    else
    {
        directLight = vec3(0.0);
    }
}

#ifndef GI_FOLIAGE_BACKLIGHT
// 2026-10-05 薄片背光透射（#24）：树叶/花瓣（materialId 0.25，Java LEAVES/
// FLOWERS 标记）被太阳从背面照时呈现透亮颜色。透射色 ∝ albedo × 背光角 ×
// shadowTransmittance（与直射同一套玻璃染色/云影/太阳可见性口径，无新 trace）。
// 光源在正面时 backFacing = 0 ⇒ 逐位旧档；非树叶材质不进分支。
#define GI_FOLIAGE_BACKLIGHT 1
#endif
#if GI_FOLIAGE_BACKLIGHT
// 2026-10-19 树叶次表面散射两档（设置界面"次表面散射"按钮，CameraUBO.sssParams.x）：
//   0 = 太阳背光（旧档，逐位不变）：只在太阳高于地平线、叶面背光时加透射。
//   1 = 真次表面散射：叶面背半球一条余弦分布透射射线，miss 时按天空辐亮度
//       加透射光 —— 不依赖太阳直射，阴影/夜间同样生效。太阳背光项保留，
//       两档的直射穿叶能量一致，差别只在新增的天光透射。
// 两档共用 matId 0.25（Java LEAVES/FLOWERS 标记）。
if(matId > 0.2 && matId < 0.45)
{
#ifndef GI_FOLIAGE_SSS_SKY_STRENGTH
#define GI_FOLIAGE_SSS_SKY_STRENGTH 0.35
#endif
    if(cam.sssParams.x > 0.5)
    {
        // ---- 真 SSS：太阳穿叶（与旧档同式）+ 天光穿叶 ----
        if(sunDir.y > 0.02 && shadowVisibility > 0.0)
        {
            float foliageBackFacing = clamp(-dot(normal, sunDir), 0.0, 1.0);
            if(foliageBackFacing > 0.0)
            {
                directLight +=
                    albedo *
                    foliageBackFacing *
                    0.35 *
                    getTimeDrivenSkyLightColor() *
                    3.2 *
                    getTimeDrivenSkyLightRadianceScale() *
                    shadowTransmittance;
            }
        }

        // 背半球余弦采样：叶面背面（通常是叶底/背光面）朝天的方向。
        // 种子沿用主循环 seed 并混入帧号 ⇒ 每帧换方向，由 RR 时域收敛；
        // 不写回 seed，后面的 GI/AO 采样链不受影响。
        uint sssSeed =
            seed ^
            (gl_LaunchIDEXT.x * 1973u) ^
            (gl_LaunchIDEXT.y * 9277u) ^
            (cam.frameCount * 26699u) ^
            0x9e3779b9u;
        pcg_hash(sssSeed);

        vec3 sssDirection =
            normalize(
                sampleHemisphereCosine2D(
                    -normal,
                    vec2(
                        glassBlueNoiseSample(uvec2(pixelCoord), cam.frameCount, 65u),
                        glassBlueNoiseSample(uvec2(pixelCoord), cam.frameCount, 66u)
                    )
                )
            );

        // 射线必须跳过叶片自己所在的方块：叶方块在 RT 场景里是整格立方体，
        // 从命中面直接往背面打会在对面内壁（≤√3 格）自相交。用世界空间
        // AABB 退出距离把起点推到方块外 0.01 格 —— 相邻叶片仍会挡住射线。
        vec3 sssBlockMin =
            floor(hitPos - worldDir.xyz * 1.0e-3);
        vec3 sssExitPlane = vec3(
            sssDirection.x > 0.0 ? sssBlockMin.x + 1.0 : sssBlockMin.x,
            sssDirection.y > 0.0 ? sssBlockMin.y + 1.0 : sssBlockMin.y,
            sssDirection.z > 0.0 ? sssBlockMin.z + 1.0 : sssBlockMin.z
        );
        vec3 sssSafeDirection = vec3(
            abs(sssDirection.x) > 1.0e-5 ? sssDirection.x : 1.0e-5,
            abs(sssDirection.y) > 1.0e-5 ? sssDirection.y : 1.0e-5,
            abs(sssDirection.z) > 1.0e-5 ? sssDirection.z : 1.0e-5
        );
        float sssExitDistance = max(
            max(
                (sssExitPlane.x - hitPos.x) / sssSafeDirection.x,
                (sssExitPlane.y - hitPos.y) / sssSafeDirection.y
            ),
            (sssExitPlane.z - hitPos.z) / sssSafeDirection.z
        );

        vec3 sssOrigin =
            hitPos +
            sssDirection *
            (max(sssExitDistance, 0.0) + 0.01);

        hit.hitDistance = 1.0;
        // purpose 7 = 天空 miss：rmiss 跳过 24 步体积云行军，只回天空辐亮度。
        // 透射光因此不含云（云影由太阳穿叶项的 shadowTransmittance 承担）。
        hit.state = makeTraceState(RT_PURPOSE_PRIMARY_SKY);
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
            sssOrigin,
            0.001,
            sssDirection,
            10000.0,
            0
        );
        if(hit.hitDistance < 0.0)
        {
            directLight +=
                albedo *
                unpackAlbedoUnorm(hit.albedoPacked) *
                GI_FOLIAGE_SSS_SKY_STRENGTH;
        }
    }
    else if(sunDir.y > 0.02 && shadowVisibility > 0.0)
    {
        // ---- 太阳背光（旧档，逐位不变）----
        float foliageBackFacing = clamp(-dot(normal, sunDir), 0.0, 1.0);
        if(foliageBackFacing > 0.0)
        {
            directLight +=
                albedo *
                foliageBackFacing *
                0.35 *
                getTimeDrivenSkyLightColor() *
                3.2 *
                getTimeDrivenSkyLightRadianceScale() *
                shadowTransmittance;
        }
    }
}
#endif
#endif // ORION_K2_INDIRECT_STAGE（③：阴影/GGX/SSS 归 K2-D，K2-I 不编译）

#if defined(ORION_K2_DIRECT_SUN_STAGE)
                // ==========================================
                // ④ K2-SUN 不透明出口：太阳直射级在此结束。落 k2DirectImage
                //（rgb = directLight，a = rtaoFactor），局部光/镜面由 K2-LOC
                // **load-add-store** 叠加进同一张图（rgb 通道），a 原样携带。
                // K2-I 照旧整图载入，无感知拆分。
                // ==========================================
                imageStore(
                    k2DirectImage,
                    pixelCoord,
                    vec4(directLight, rtaoFactor)
                );
                continue;
#elif defined(ORION_K2_DIRECT_SUN_STAGE_A)
                // ==========================================
                // ⑤ K2-SUNA 不透明出口：本腿只产 RTAO（直射由 SUNB 求值）。
                // rgb 先落 0 占位（SUNB 稍后 load-add 叠加），a = rtaoFactor
                // 原样携带给 LOC/K2-I。
                // ==========================================
                imageStore(
                    k2DirectImage,
                    pixelCoord,
                    vec4(0.0, 0.0, 0.0, rtaoFactor)
                );
                continue;
#elif defined(ORION_K2_DIRECT_SUN_STAGE_B)
                // ==========================================
                // ⑤ K2-SUNB 不透明出口：阴影+GGX+植被SSS 的直射 **load-add**
                // 叠加进 SUNA 已落的占位（照 K2-LOC 出口模式，rgb 通道相加，
                // a 原样携带 rtaoFactor）。K2-LOC / K2-I 照旧无感知。
                // ==========================================
                const vec4 k2DirectAccum = imageLoad(k2DirectImage, pixelCoord);
                imageStore(
                    k2DirectImage,
                    pixelCoord,
                    vec4(k2DirectAccum.rgb + directLight, k2DirectAccum.a)
                );
                continue;
#endif


// =====================================================
// VLI-1.4 - Section voxel indirect local light
//
// No candidate search and no L7 local-light visibility rays remain on
// the primary path. The old emitter call stays only as a zero-cost
// compatibility fallback while Binding 17 contains its permanent empty
// sentinel page.
// =====================================================

// 判决实验 H2 分屏判据：右半屏 = true（保留自发光 / 关掉二级弹射），
// 左半屏 = false。必须在两个本地光项之前声明（GLSL 单遍编译）。
bool splitHalfRight =
    DBG_SPLIT_ABLATION &&
    (float(pixelCoord.x) >= 0.5 * float(rtLaunchSize().x));

// 2026-09-30 收口：本地光只剩经典 0.31 RIS 一种方案（ReSTIR DI 链已整链删除），
// 发射体邻接自发光（命中面**背后**体素的 emission，hitPos - normal*0.02）
// 无条件叠加：只在被看的面属于发光方块时非零 —— 它就是"光源方块自己亮"
// 的唯一来源；非发光面它恒为 0，无副作用。
#if !defined(ORION_K2_DIRECT_STAGE) && !defined(ORION_K2_DIRECT_SUN_STAGE) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_A) && !defined(ORION_K2_DIRECT_SUN_STAGE_B) \
    && !defined(ORION_K2_DIRECT_LOCAL_STAGE) && !defined(ORION_K2_INDIRECT_TRACE_ONLY) && !SHARC_UPDATE
vec3 localEmitterEmission = evaluateVLIEmitterRadianceAtHit(
    hitPos,
    normal,
    albedo
);
if(DBG_KILL_EMITTER_TERM)
{
    localEmitterEmission = vec3(0.0);
}
// 判决实验 H2 分屏：左半屏关掉无过滤自发光，右半屏保留它。
if(DBG_SPLIT_ABLATION)
{
    localEmitterEmission = splitHalfRight ? localEmitterEmission : vec3(0.0);
}
#endif

// ④ 局部光 RIS/二级弹射归 K2-LOC（K2-SUN 与 K2-LOC-GLOSSY 不编译）。
#if !defined(ORION_K2_INDIRECT_STAGE) && !defined(ORION_K2_DIRECT_SUN_STAGE) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_A) && !defined(ORION_K2_DIRECT_SUN_STAGE_B) \
    && !defined(ORION_K2_LOC_GLOSSY_ONLY)
// Opaque primary receivers use full ReSTIR DI. Glass and water retain their
// dedicated paths, so no transparent-layer history participates in reprojection.
// （Orion indirect.rgen.slang:57）。XOR 保留乘法乘积的逐位结构，会让
// 种子低位在屏幕上呈周期性 → 候选选择/可见性跟着周期化 → 整齐的点阵与线条。
// 本文件其余 5 处同类种子构造（3877/3917/7501/12441/15800）同步改为加法。
uint vptRisSeed = uint(pixelCoord.x) * 1973u +
    uint(pixelCoord.y) * 9277u ^ cam.frameCount * 26699u;
pcg_hash(vptRisSeed);
// Exact direct local light for the primary receiver, read from the same baked
// 16^3 x 6 block-face cache the emitter solve writes (with its own DDA
// visibility test). Until now this lookup was reached only from the
// glass-reflection branch, so a wall the camera looks at head-on received no
// direct local light at all -- what lit it was the neighbour-voxel bounce proxy
// and the emitter-adjacency glow, which is why a corner, a flat wall and a floor
// disagreed with each other.
//
// This IS the legacy bake read, i.e. exactly the block-face quantisation the
// migration exists to delete, so it is off whenever the ReSTIR DI chain is live.
// The const ternary folds at compile time, so the lookup costs nothing then.
//
// 2026-09-29：两个运行期方案都不再让它上主路径 —— 经典 0.31 的主路径直射
// 是 RIS（evaluateVPTRISDirectLighting，0.31 原样），不是面缓存直射；
// ReSTIR 模式由水库链负责。布尔恒 false，编译期折叠，代码保留供对照。
const bool VPT_LOCAL_EXACT_DIRECT_ON_PRIMARY = false;

vec3 localExactFaceDirect = VPT_LOCAL_EXACT_DIRECT_ON_PRIMARY
    ? evaluateVPTExactFaceDirectLighting(
        hitPos,
        normal,
        albedo
    )
    : vec3(0.0);

// 经典 0.31 RIS：直射在 raygen 里就地求值（返回值即 radiance）。
// 时域复用（LOCAL_RIS_PRIMARY_TEMPORAL）**默认关闭**。
//
// 2026-09-29 首次定案关闭：跨帧 EMA 反而更噪（主命中位置带 DLSS 的 Halton 帧
// 抖动，远处上一帧的世界空间偏移超过 VPT_SCREEN_POSITION_EPSILON(0.35)，历史
// 逐帧翻转）。
// 2026-09-30 带着抖动补偿再试一次（vptJitterCompensatedPositionEpsilon 已把
// 该容差按抖动差与视深放大）——**实测仍然不可行**：本地光本来经 DLSS-RR 处理后
// 表面是干净的，开了这一层反而在本地光照明面上出现结构性噪点。据此维持关闭。
//
// 注意：抖动补偿本身保留，它服务的是**透明接收路径**
// （evaluateTransparentReceiverDirectLighting 的时域分支，GI_TRANSPARENT_*
// 默认开启）——那条路径正是"玻璃里/玻璃后"的内容，抖动补偿让它的历史在相机
// 移动时不再被假否定。
//
// A/B：-DLOCAL_RIS_PRIMARY_TEMPORAL=1 可复现开启态。
#ifndef LOCAL_RIS_PRIMARY_TEMPORAL
#define LOCAL_RIS_PRIMARY_TEMPORAL 0
#endif
// 2026-10-26：直接光模式（cam.sparseParams.x）运行时二选一 ——
//   0 = 传统 RIS（下面 #else 的原路径，逐位不变）
//   1 = RTXDI 式时域 reservoir（WRS，见 evaluateVPTDirectLightReservoirTail）
// LOCAL_RIS_PRIMARY_TEMPORAL 仍是编译期 A/B 旧 EMA 路径，默认 0。
#ifndef VPT_SPARSE_KEEP_LOCAL_DIRECT
// 2026-11-07：改回 0（非活跃像素**跳过**本地直射）。
//
// 11-04 置 1 的理由是"对齐 Remix 的 enableRtxdiReuseForInactivePixels"，
// 但那说的是 **reservoir 复用/老化**，前提是 directLightMode=1 且确有 reservoir。
// 生产默认 directLightMode=0，既没有 reservoir 也没有复用，此时置 1 只剩副作用：
// 非活跃像素照样跑静态光 NEE，而 evaluateVPTRISDirectLightingSingle 里的
// `if(!shadingActive) return voxelDirect;` 只跳过**动态光源**那一小段。
//
// 实测后果：整条稀疏唯一真正跳过的是"动态光源 RIS"，而场景通常没有动态光源
// ⇒ 稀疏的净收益趋近于 0，正是"30% 测不出收益"的直接原因。
//
// 现在与 VPT_SPARSE_ALWAYS_WRITE=1 + 1/rate 补偿配对：
//   活跃像素：完整值 × 1/rate
//   非活跃：  本地直射为 0（照常写 image）
//   时域平均回到正确期望，无偏。
// A/B：-DVPT_SPARSE_KEEP_LOCAL_DIRECT=1 回到 11-04 的行为。
#define VPT_SPARSE_KEEP_LOCAL_DIRECT 0
#endif
bool runPrimaryDirectLight =
    vptPixelActive || (VPT_SPARSE_KEEP_LOCAL_DIRECT != 0);
#if !LOCAL_RIS_PRIMARY_TEMPORAL
// 稀疏渲染 × RTXDI：非活跃像素仍做 reservoir 验证/老化（对应官方
// enableRtxdiReuseForInactivePixels=true），tail 内不发着色射线。
if(!vptPixelActive && cam.sparseParams.x >= 0.5 && cam.sparseParams.z > 0.5)
{
    runPrimaryDirectLight = true;
}
#endif
vec3 localDirectLight = vec3(0.0);
vec3 primaryLocalGeomFace = vec3(0.0);
if(runPrimaryDirectLight)
{
#if LOCAL_RIS_PRIMARY_TEMPORAL
primaryLocalGeomFace = vptGeometricFaceNormalFromRay(hitPos, worldDir.xyz);
localDirectLight = evaluateVPTCandidateDirectTemporal(
#if VPT_TRANSPARENT_HISTORY_KEY
    // Opaque primary: the surface point IS the receiver, so it is its own key.
    hitPos,
#endif
    hitPos,
    normal,
    albedo,
    primaryLocalGeomFace,
    pixelCoord,
    ivec2(rtLaunchSize())
);
#else
// 本地直射余弦的几何面上界必须用**射线入面**恢复的面法线：payload.normal
// （以及主命中处存的 primaryGeometricNormal）是 rchit 里 tbn*tangentNormal
// 写出的着色法线，浮雕纹素会越过几何面地平线 —— 那正是无受光面亮纹的成因。
// 2026-10-07：带上主命中的 LabPBR 材质 —— materialF0 / metallic / roughness 都是本
// 作用域里**已经解码好**的那一份（见 18122 附近的 F0/Metal 解码块），不必重算，也不能
// 用太阳那段的 directF0（它在内层大括号里，这里看不到）。
primaryLocalGeomFace = vptGeometricFaceNormalFromRay(hitPos, worldDir.xyz);
localDirectLight = evaluateVPTRISDirectLightingPrimary(
    hitPos,
    normal,
    albedo,
    primaryLocalGeomFace,
    materialF0,
    metallic,
    roughness,
    cam.sparseParams.x >= 0.5,
    vptPixelActive,
    vptRisSeed
);
#endif
}
uint vptBounceSeed = uint(pixelCoord.x) * 31847u ^
    uint(pixelCoord.y) * 11213u ^ cam.frameCount * 6971u;
pcg_hash(vptBounceSeed);
// Keep the local-light secondary DDA on the same 10-chunk temporal budget as
// direct local lighting.  This avoids an unbudgeted extra voxel traversal in
// the distant tiers.  The classic 0.31 RIS chain always runs it: the
// neighbour-voxel bounce proxy is one of its three terms.
#ifndef VPT_LOCAL_SECONDARY_SHARC_LOD
#define VPT_LOCAL_SECONDARY_SHARC_LOD 1
#endif
#if VPT_LOCAL_SECONDARY_SHARC_LOD
const bool localSecSharcTrim = sharcQueryActive() && (cam.perfPrimary.z >= 2u);
bool runLocalSecondary = false;
float localSecondaryLodWeight = 1.0;
if(localSecSharcTrim)
{
    const float secScale = perfL10TierScale();
    if(rtLodDistance < 64.0 * secScale)
    {
        runLocalSecondary = true;
    }
    else if(rtLodDistance < 128.0 * secScale)
    {
        runLocalSecondary = rtLodHalfSample;
        localSecondaryLodWeight = 2.0;
    }
    else if(rtLodDistance < 160.0 * secScale)
    {
        runLocalSecondary = rtLodQuarterSample;
        localSecondaryLodWeight = 4.0;
    }
}
else
{
    runLocalSecondary = rtLodDistance < 64.0 * perfL10TierScale();
    if(rtLodDistance >= 64.0 * perfL10TierScale() && rtLodDistance < 128.0 * perfL10TierScale())
    {
        runLocalSecondary = rtLodHalfSample;
        localSecondaryLodWeight = 2.0;
    }
    else if(rtLodDistance >= 128.0 * perfL10TierScale() && rtLodDistance < 160.0 * perfL10TierScale())
    {
        runLocalSecondary = rtLodQuarterSample;
        localSecondaryLodWeight = 4.0;
    }
}
#else
bool runLocalSecondary = rtLodDistance < 64.0 * perfL10TierScale();
float localSecondaryLodWeight = 1.0;
if(rtLodDistance >= 64.0 * perfL10TierScale() && rtLodDistance < 128.0 * perfL10TierScale())
{
    runLocalSecondary = rtLodHalfSample;
    localSecondaryLodWeight = 2.0;
}
else if(rtLodDistance >= 128.0 * perfL10TierScale() && rtLodDistance < 160.0 * perfL10TierScale())
{
    runLocalSecondary = rtLodQuarterSample;
    localSecondaryLodWeight = 4.0;
}
#endif

// 2026-10-26 稀疏渲染：非活跃像素跳过本地光二级弹射（DDA）。
if(!vptPixelActive)
{
    runLocalSecondary = false;
}

vec3 localSecondaryLight = runLocalSecondary
    ? evaluateVPTLocalSecondaryBounce(
        hitPos,
        normal,
        albedo,
        vptBounceSeed
    ) * localSecondaryLodWeight
    : vec3(0.0);
if(DBG_KILL_SECONDARY_TERM)
{
    localSecondaryLight = vec3(0.0);
}
// 判决实验 H2 分屏：左半屏保留二级弹射，右半屏关掉它。
if(DBG_SPLIT_ABLATION)
{
    localSecondaryLight = splitHalfRight ? vec3(0.0) : localSecondaryLight;
}
#endif // ORION_K2_INDIRECT_STAGE（③：局部光 RIS/二级弹射归 K2-D）

// ④ 水面反射太阳/GGX 镜面/spec guide 快照归 K2-LOC（K2-SUN 与 K2-LOC-NEE 不编译）。
#if !defined(ORION_K2_INDIRECT_STAGE) && !defined(ORION_K2_DIRECT_SUN_STAGE) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_A) && !defined(ORION_K2_DIRECT_SUN_STAGE_B) \
    && !defined(ORION_K2_LOC_NEE_ONLY)
                    // =====================================================
// Explicit Sun -> Water -> Receiver reflection
// =====================================================
// The reflection interval is the user "水面反射间隔" setting
// (perfSecondary.z: 0 = off, 1 = every frame, 2/4 = every N frames).  On
// skipped frames the term is simply dropped; the temporal filters smooth the
// result, trading one visibility ray per skipped frame.
const float waterReflPlaneY = cam.waterSimSurfaceAndOrigin.x;
const bool waterReflPlaneReachable =
    (abs(cam.viewInverse[3].y - waterReflPlaneY) <= 128.0) &&
    (hitPos.y > waterReflPlaneY - 0.5) &&
    ((hitPos.y - waterReflPlaneY) <= 64.0 * sunDir.y + 1.5);
uint waterReflectionInterval =
    clamp(cam.perfSecondary.z, 0u, 4u);
bool runWaterReflection =
    (cam.fogMoodParams.w > 0.5) &&
    waterReflectionInterval > 0u &&
    (waterReflectionInterval == 1u ||
     (cam.frameCount % waterReflectionInterval) == 0u) &&
    waterReflPlaneReachable
#if WATER_REFLECT_SUN_OPT_V1
    && sunDir.y > 0.02
    && rtLodDistance < 96.0 * perfL10TierScale()
#endif
    ;
float waterReflectedSunFactor =
    runWaterReflection
        ? traceWaterReflectedSun(
            hitPos,
            normal,
            sunDir
        )
        : 0.0;

vec3 waterReflectedSunLight = vec3(0.0);
if(waterReflectedSunFactor > 1.0e-5)
{
    waterReflectedSunLight =
        getTimeDrivenSkyLightColor() *
        3.2 *
        getTimeDrivenSkyLightRadianceScale() *
        albedo *
        (1.0 - metallic) *
        waterReflectedSunFactor *
        c3CloudSunTransmittance(hitPos, sunDir);
}
    // =====================================================
// A2-1
// Independent ray-traced GGX glossy reflection
// =====================================================

vec3 glossyReflectionLight =
    vec3(0.0);

float glossySpecularHitDistance =
    65504.0;


// =====================================================
// A3-3
// Primary GGX glossy reflection
//
// Full Monte-Carlo weight:
//
//     BRDF * NdotL / PDF
//
// 当前 sampleGGX() 采样 GGX NDF half-vector：
//
//     PDF(H) = D(H) * NdotH
//
// reflection 映射后：
//
//     PDF(L) = PDF(H) / (4 * VdotH)
// =====================================================
bool runGlossyRT =
    pbr2ReflectionStrength > 0.0;


float glossyLodWeight =
    1.0;


// 10-chunk GGX reflection budget.  The former 48-block hard stop produced
// black metal/specular regions.  Interleaved estimates retain the expected
// reflected radiance through 10 chunks; no RT reflection is traced beyond it.
// The full-resolution distance is the user "光泽全采样距离" setting
// (perfSecondary.w, 16/32/64/96 blocks).
float glossyFullResDistance =
    float(clamp(cam.perfSecondary.w, 16u, 96u));

// =====================================================
// 2026-10-25：光泽反射降速级（0 = 全速 / 1 = 半速 / 2 = 四分之一 / 3 = 关）
//
// 距离级与**粗糙度**级合成一个整数再统一施加。为什么要合成而不是各写一个 if：
// rtLodGlossyHalfSample 与 rtLodGlossyQuarterSample 共用 rtLodRandom 的同一批
// bit，两个门先后 AND 时"命中率"不会变成 1/4 的两倍小，但 glossyLodWeight 会被
// 乘两次 ⇒ 过曝。一个级别对应一个权重，才是无偏的时间估计量。
//
// 粗糙度门槛的依据：noGlossy 消融在室内是 -4.27ms（rt=18.0），而室内距离全都
// < 16 格 ⇒ 距离级一格都没触发，全屏都在跑全速光追。宽波瓣（roughness >= 0.30）
// 的单样本 GGX 反射本来就由 RR 的时间累积重建，降到 1/2~1/4 速率几乎不可区分；
// 光滑面（绿玻璃板、镜面）保持全速，锐利反射不受影响。
// =====================================================
int glossySampleLevel =
    0;

if(
    rtLodDistance >= glossyFullResDistance &&
    rtLodDistance < 128.0 * perfL10TierScale()
)
{
    glossySampleLevel = 1;
}
else if(
    rtLodDistance >= 128.0 * perfL10TierScale() &&
    rtLodDistance < 160.0 * perfL10TierScale()
)
{
    glossySampleLevel = 2;
}
else if(rtLodDistance >= 160.0 * perfL10TierScale())
{
    glossySampleLevel = 3;
}

#ifndef VPT_GLOSSY_ROUGH_CUTOFF
#define VPT_GLOSSY_ROUGH_CUTOFF 1
#endif
#ifndef VPT_GLOSSY_DIST_ROUGH_CUTOFF
#define VPT_GLOSSY_DIST_ROUGH_CUTOFF 1
#endif
// 粗糙度级（ORION_GLOSSY_ROUGH_LOD）：只往上抬，不往回降。
if(cam.giHalfParams.w > 0.5f)
{
    if(metallic < 0.05 &&
       max(materialF0.r, max(materialF0.g, materialF0.b)) <= 0.06 &&
       VPT_GLOSSY_ROUGH_CUTOFF != 0 &&
       (roughness >= 0.80 ||
        (VPT_GLOSSY_DIST_ROUGH_CUTOFF != 0 &&
         ((rtLodDistance >= 40.0 * perfL10TierScale() && roughness >= 0.65) ||
          (rtLodDistance >= 72.0 * perfL10TierScale() && roughness >= 0.50)))))
    {
        glossySampleLevel = 3;
    }
    else if(roughness >= 0.65)
    {
        glossySampleLevel = max(glossySampleLevel, 2);
    }
    else if(roughness >= 0.30)
    {
        glossySampleLevel = max(glossySampleLevel, 1);
    }
}

if(glossySampleLevel == 1)
{
    runGlossyRT =
        runGlossyRT &&
        rtLodGlossyHalfSample;

    glossyLodWeight =
        2.0;
}
else if(glossySampleLevel == 2)
{
    runGlossyRT =
        runGlossyRT &&
        rtLodGlossyQuarterSample;

    glossyLodWeight =
        4.0;
}
else if(glossySampleLevel >= 3)
{
    runGlossyRT = false;
}


if(perfL10DisableGlossy())
{
    runGlossyRT =
        false;
}

// ============================================================================
// 2026-11-03（用户要求：做进 UI 开关）RT 光泽射线总开关
//
// cam.ssHybridParams.z < 0.5 ⇒ 这条 GGX 反射射线整段不跑，镜面完全交给屏幕
// 空间：RT 命中距离 > 12 区块归 SSR，射程内归 SSPT 的近场镜面
// （cpp/shaders/ss_hybrid.comp 的 ssptSpecularTrace）。
//
// 为什么这样接是安全的（两个初值正好是这个语义）：
//   · glossyReflectionLight 初值 vec3(0) ⇒ 分项 G-buffer 的 82.rgb = 0
//     ⇒ SSR/SSPT 的 "color - specTerm + ..." 退化成纯加，不会减掉不存在的量；
//   · glossySpecularHitDistance 初值 65504（= 未命中/天空）⇒ 它被写进
//     rrSpecularHitDistance 与 rrSpecularHitDistanceImage 后，SSR 的门槛
//     （> 12 区块）自动接管这些像素，RR 也按"天空反射"重投影，与内容一致。
//
// 这是本工程唯一能真正省下镜面射线成本的地方（打开时射线照跑）。
// ============================================================================
if(cam.ssHybridParams.z < 0.5f)
{
    runGlossyRT = false;
}

// 2026-10-26 稀疏渲染：非活跃像素跳过 GGX 光泽反射段。
//
// 2026-11-08：**默认关闭这条门控** —— 非活跃像素的 glossyReflectionLight 恒 0，
// 与 VPT_SPARSE_ALWAYS_WRITE=1 组合后就是「30% 的帧写 3.33× 反射、70% 的帧写 0」
// 的二值方波。GGX 反射是高频镜面项，RR 的时域累积跟不住这种方波（而 GI /
// 本地光 / 阴影是低频，能跟住）⇒ 实机表现为**反射的图形逐帧闪动**。
// 代价：反射射线回到逐像素全采样（10-30 口径约 6.29 ms，SER 之后更便宜）。
// A/B：-DVPT_SPARSE_GLOSSY=1 回到"非活跃像素不跑反射"（省射线，但会闪）。
#ifndef VPT_SPARSE_GLOSSY
#define VPT_SPARSE_GLOSSY 0
#endif
#if VPT_SPARSE_GLOSSY
if(!vptPixelActive)
{
    runGlossyRT = false;
}
#endif

vec3 glossyRayNormal =
    normal;


vec3 glossyH =
    vec3(0.0);


vec3 glossyDir =
    vec3(0.0);


float glossyNdotV =
    0.0;


float glossyNdotL =
    0.0;


float glossyNdotH =
    0.0;


float glossyVdotH =
    0.0;


// Avoid sampling GGX and constructing reflection state for pixels whose
// distance/material LOD already disabled glossy RT.  The old code paid this
// math cost before the runGlossyRT guard below, even when no ray was traced.
if(runGlossyRT)
{
    // [14] 光泽反射求值（每像素最多一次；真正的那根光线在本块之后才发）。
    perfRayStatAdd(14u, 1u);

    vec2 glossyBn = vec2(
        glassBlueNoiseSample(uvec2(pixelCoord), cam.frameCount, 41u),
        glassBlueNoiseSample(uvec2(pixelCoord), cam.frameCount, 42u)
    );
    glossyH =
        sampleGGXVNDF(
            glossyRayNormal,
            V,
            roughness,
            glossyBn
        );

    glossyDir =
        normalize(
            reflect(
                -V,
                glossyH
            )
        );

    glossyNdotV =
        max(
            dot(
                glossyRayNormal,
                V
            ),
            0.0
        );

    glossyNdotL =
        max(
            dot(
                glossyRayNormal,
                glossyDir
            ),
            0.0
        );

    glossyNdotH =
        max(
            dot(
                glossyRayNormal,
                glossyH
            ),
            0.0
        );

    glossyVdotH =
        max(
            dot(
                V,
                glossyH
            ),
            0.0
        );
}


if(
    runGlossyRT &&
    glossyNdotV > 1.0e-5 &&
    glossyNdotL > 1.0e-5 &&
    glossyNdotH > 1.0e-5 &&
    glossyVdotH > 1.0e-5
)
{
    // =================================================
    // LabPBR F0
    // =================================================

    vec3 glossyF0 =
        decodeBaseF0(
            f0MetalCode,
            albedo
        );


    vec3 glossyF =
        fresnelSchlick(
            glossyVdotH,
            glossyF0
        );


    // =================================================
    // GGX VNDF weight: F * G(V, L) / G1(V)
    // =================================================

    float glossyG =
        geometrySmith(
            glossyRayNormal,
            V,
            glossyDir,
            roughness
        );


    float glossyG1V =
        ggxVNDFMaskingG1(
            glossyNdotV,
            roughness
        );


    vec3 glossyWeight =
        glossyF *
        (glossyG / max(glossyG1V, 1.0e-5));


    vec3 incomingGlossyRadiance =
        tracePrimaryGlossyReflectionRadiance(
            hitPos,
            hitPos +
                glossyRayNormal * 0.001,
            glossyDir,
            sunDir,
            roughness,
            seed,
            glossySpecularHitDistance
        );


glossyReflectionLight =
    glossyWeight *
    incomingGlossyRadiance *
    glossyLodWeight;
}

if(s == 0 && matId <= 0.5)
{
#ifndef VPT_STABLE_SPEC_HIT_DISTANCE
// 1 = 粗糙面的镜面命中距离 guide 用相机到表面的距离（**默认**，逐帧稳定）。
//     0 = 旧行为：一律用随机 GGX 样本的命中距离。
#define VPT_STABLE_SPEC_HIT_DISTANCE 1
#endif
#if VPT_STABLE_SPEC_HIT_DISTANCE
    // 随机 GGX 样本的距离每帧在"近处墙 / 远处天空"之间跳 ⇒ RR 认为镜面事件
    // 每帧都换位置 ⇒ 历史被判失配 ⇒ 静止时颗粒永不收敛（阴影里最明显）。
    // 粗糙面的反射本来就模糊，用稳定的表面距离当 guide 更符合 RR 的契约。
#if RR_GUIDE_PURIFY_V1
    rrSpecularHitDistance =
        (roughness >= 0.5 || (!runGlossyRT && cam.ssHybridParams.z >= 0.5f))
            ? max(primaryHitDistance, 1.0)
            : glossySpecularHitDistance;
#else
    rrSpecularHitDistance =
        (primaryRoughness >= 0.5)
            ? max(primaryHitDistance, 1.0)
            : glossySpecularHitDistance;
#endif
#else
    rrSpecularHitDistance =
        glossySpecularHitDistance;
#endif
}
#endif // ORION_K2_INDIRECT_STAGE（③：水面太阳/镜面/spec guide 快照归 K2-D）

#ifdef ORION_K2_DIRECT_STAGE
                // ==========================================
                // ③ K2-D 不透明出口：直射级在此完整结束。落 k2DirectImage
                //（rgb = 直射三项 + 水面太阳，未缩放；a = rtaoFactor），
                // 镜面原始值与 spec guide 快照走 landing d 字段，然后交棒
                // K2-I（GI/稀疏合成/公共尾段全部由 K2-I 执行）。
                // ==========================================
                imageStore(
                    k2DirectImage,
                    pixelCoord,
                    vec4(
                        directLight +
                            localDirectLight +
                            localSecondaryLight +
                            waterReflectedSunLight,
                        rtaoFactor
                    )
                );
                const uint spatialLandingIdx = ptLandingRecordIndex(
                    uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
                    s
                );
                ptLandingStoreGlossyExtra(
                    spatialLandingIdx,
                    glossyReflectionLight,
                    rrSpecularHitDistance
                );
                if(cam.sparseParams.x >= 1.5 && s == 0 && runPrimaryDirectLight)
                {
                    ptLandingStoreSpatialMaterial(
                        spatialLandingIdx,
                        albedo,
                        roughness,
                        materialF0,
                        metallic,
                        normal,
                        primaryLocalGeomFace,
                        vptPixelActive
                    );
                }
                else
                {
                    ptLandingRecords[spatialLandingIdx].c.w = 0u;
                }
                continue;
#elif defined(ORION_K2_DIRECT_LOCAL_STAGE)
                const uint spatialLandingIdx = ptLandingRecordIndex(
                    uint(pixelCoord.y) * uint(size.x) + uint(pixelCoord.x),
                    s
                );
#if defined(ORION_K2_LOC_NEE_ONLY)
                // Batch U：K2-LOC-NEE 独立腿，只做局部光 NEE + 二级体素弹射，不碰 .d 镜面槽。
                const vec3 localAdd = localDirectLight + localSecondaryLight;
                if(any(greaterThan(localAdd, vec3(0.0))))
                {
                    vec4 k2DirectAccum = imageLoad(k2DirectImage, pixelCoord);
                    imageStore(
                        k2DirectImage,
                        pixelCoord,
                        vec4(k2DirectAccum.rgb + localAdd, k2DirectAccum.a)
                    );
                }
                if(cam.sparseParams.x >= 1.5 && s == 0 && runPrimaryDirectLight)
                {
                    ptLandingStoreSpatialMaterial(
                        spatialLandingIdx,
                        albedo,
                        roughness,
                        materialF0,
                        metallic,
                        normal,
                        primaryLocalGeomFace,
                        vptPixelActive
                    );
                }
                else
                {
                    ptLandingRecords[spatialLandingIdx].c.w = 0u;
                }
                continue;
#elif defined(ORION_K2_LOC_GLOSSY_ONLY)
                // Batch U：K2-LOC-GLOSSY 独立腿，只做水面太阳反射与主表面 Glossy，不碰 .c 槽。
                if(any(greaterThan(waterReflectedSunLight, vec3(0.0))))
                {
                    vec4 k2DirectAccum = imageLoad(k2DirectImage, pixelCoord);
                    imageStore(
                        k2DirectImage,
                        pixelCoord,
                        vec4(k2DirectAccum.rgb + waterReflectedSunLight, k2DirectAccum.a)
                    );
                }
                ptLandingStoreGlossyExtra(
                    spatialLandingIdx,
                    glossyReflectionLight,
                    rrSpecularHitDistance
                );
                continue;
#else
                // ==========================================
                // ④ K2-LOC 不透明出口：局部光/水面反射太阳叠加进 K2-SUN
                // 已落的直射和（**load-add-store 复用 binding 89**，a 原样
                // 携带 rtaoFactor）；镜面原始值与 spec guide 快照照旧走
                // landing d 字段。K2-I 照旧整图载入，无感知拆分。
                // ==========================================
                vec4 k2DirectAccum = imageLoad(k2DirectImage, pixelCoord);
                imageStore(
                    k2DirectImage,
                    pixelCoord,
                    vec4(
                        k2DirectAccum.rgb +
                            localDirectLight +
                            localSecondaryLight +
                            waterReflectedSunLight,
                        k2DirectAccum.a
                    )
                );
                ptLandingStoreGlossyExtra(
                    spatialLandingIdx,
                    glossyReflectionLight,
                    rrSpecularHitDistance
                );
                if(cam.sparseParams.x >= 1.5 && s == 0 && runPrimaryDirectLight)
                {
                    ptLandingStoreSpatialMaterial(
                        spatialLandingIdx,
                        albedo,
                        roughness,
                        materialF0,
                        metallic,
                        normal,
                        primaryLocalGeomFace,
                        vptPixelActive
                    );
                }
                else
                {
                    ptLandingRecords[spatialLandingIdx].c.w = 0u;
                }
                continue;
#endif
#endif
// ④ GI/稀疏合成/公共尾段：主变体 + K2-I 编译；K2-D/K2-SUN/K2-LOC 全部不编译。
#if !defined(ORION_K2_DIRECT_STAGE) && !defined(ORION_K2_DIRECT_SUN_STAGE) \
    && !defined(ORION_K2_DIRECT_LOCAL_STAGE) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_A) && !defined(ORION_K2_DIRECT_SUN_STAGE_B)
                // ==========================================
                // 步骤 5: GI Bounce (2 次弹射路径追踪)
                // ==========================================
                vec3 indirectLight = vec3(0.0);
                // DBG_LOCAL_BOUNCE_ONLY accumulator: second-vertex emitter hits
                // only (see its declaration at the top).
                vec3 dbgLocalBounceAccum = vec3(0.0);

                // =================================================
                // 2026-10-11 5b-3：查询**已从这里撤走**（原 5b-2 挂点）。
                //
                // 原做法（主命中命中缓存 ⇒ 整条 GI 改用它）实机表现为"**错位**"：
                // 内容是按体素量化的相邻表面/相邻方向的辐射度。排除了两个候选
                // （回路未改写 hitPos/normal/brdfWeight、回路内无 rr* 引导写入）
                // 之后，根因就是这个挂点自身违反了 SDK 文档
                // （docs/Integration.md 第 132/143 行）的两条：
                //   · 查询在 eligible hit 上，**typically excluding the primary hit**；
                //   · 段长 < 体素尺寸时必须继续追踪（主命中处段长为 0）。
                // ⇒ 查询已挪进 GI 回路、在**间接命中**处截断该路径（见下面循环内
                //    "5b-3：SHaRC 查询" 段）。主命中现在永远实追。
                // =================================================

                vec3 currentOrigin =
    hitPos + normal * 0.001;
                vec3 currentDir = bounceDir;
vec3 currentThroughput =
    brdfWeight;

// ============================================================================
// 参考：.tmp/ref/RTXGI/Samples/Pathtracer/Pathtracer.hlsl（三个 pass 在同一份源码里，
//       用 SHARC_UPDATE / SHARC_QUERY 宏分派 —— 本工程同构：同一份 rt_test.rgen 编两个变体）
//
//   · SharcInit 每个 sample 一次：参考 :435-436
//   · materialRoughnessPrev = 0：参考 :446。**初值 0 正是"排除主命中"的机制** ——
//     footprint 用它算，第一跳必为 0 ⇒ isValidHit=false（参考 :554-557）；
//     它在该顶点采样完下一段方向之后才累加：参考 :674。
//   · update 变体每 bounce 把 throughput 重置为 1：参考 :461-463 ⇒ 随后
//     SharcSetThroughput 收到的是"该段之后"的权重，而不是整条路径的累计权重。
//     （本工程不做重置，所以下面尾部传的是增量 nextBounceWeight*0.7，等价。）
// ============================================================================
// 2026-10-12：初值从 0 改为**主命中表面的粗糙度**。参考把初值设 0 的前提是
// "回路第一跳的射线来自相机（delta 射线，锥=0）"（Pathtracer.hlsl:446，它的主射线
// 在 bounce 循环内）；本工程主命中在回路**之前**处理，回路第一跳的射线来自主命中
// 表面的 BSDF 采样 ⇒ 来源锥宽 = 主命中的 roughness。把真实来源喂给参考的
// footprint 公式（公式本身一字不改），第一跳（间接命中，非 primary/visibility hit，
// 不违反 AGENTS.md:508）即可按参考自己的判据决定用不用缓存。
// GI 循环第一跳永远是纯漫反射余弦半球（镜面另在 K2-LOC 独立追踪）；对齐 NVIDIA
// Pathtracer.hlsl:675（diffuse lobe 记 1.0），近场墙角接触 GI 仍由
// payload.hitDistance > sharcVoxelSize * sqrt(3.0) 硬门槛保护，不会被误替换。
float materialRoughnessPrev = 1.0;

#if SHARC_UPDATE
SharcState sharcState;
SharcInit(sharcState);
#endif

// 参数结构体在回路外构造一次，update 与 query 两侧共用（参考把 SharcParameters 构造
// 放在 raygen 顶部：Pathtracer.hlsl:406-425）。
SharcParameters sharcParameters = sharcBuildParameters();

// =====================================================
// GI medium state
// =====================================================
bool currentInsideWater =
    false;

#if GI_NESTED_DIELECTRIC
// 2026-10-13：GI 走查当前所处的**介质优先级**（15 = 空气/无介质）。
// 与 currentInsideWater 是两条独立的状态：后者只驱动 Beer-Lambert 吸收，
// 前者只驱动"哪个界面拥有这块空间"。二者由各自的分支同步维护。
// 见 nestedPriorityOfHit() / nestedInterfaceAccepted() 的说明。
uint currentMediumPriority =
    RT_NESTED_PRIORITY_AIR;
#endif

const vec3 GI_WATER_ABSORPTION =
    waterAbsorptionColor();

int diffuseBounceCount =
    0;

int waterInterfaceStepCount =
    0;


// A pane does NOT consume a diffuse bounce -- it is a filter sheet, not a
// surface -- so a stack of panes would otherwise spin this loop forever.  Give
// glass the same kind of interface budget water already has.
int glassInterfaceStepCount =
    0;


// Perf-L10.1 stage 8: the bounce cap is switchable at runtime so the cost of the
// second bounce can be measured directly as a partial delta against the baseline
// window.  Deliberately not `const`: it is read from the UBO.
int MAX_DIFFUSE_BOUNCES =
    perfL10GiBounceCap();

// 2026-11-15 弹射分档（2026-11-15 晚起**普通模式同样生效**，不再要求 z=1）：
//   表面 ≤ y(默认 4 区块) → 用用户设定的弹射数（保持画面质量）
//   > y                    → 只 1 弹（用户指定：四区块以外一弹）
// 性能模式（z=1）额外保留：> x(6 区块) 0 弹 —— RT GI 整关，VLI 体素光 +
// 天空可见度兜底，镜面按 6~8 SSPT / >8 SSR 分档接管（见 ss_hybrid.comp）。
{
    const float perfPrimaryChunks = primaryHitDistance / 16.0;

    if(perfPrimaryChunks > max(0.5, cam.perfBandParams.y))
    {
        // 4 区块以外：只 1 弹。
        MAX_DIFFUSE_BOUNCES = min(MAX_DIFFUSE_BOUNCES, 1);
    }

    if(cam.perfBandParams.z > 0.5f &&
       perfPrimaryChunks > max(1.0, cam.perfBandParams.x))
    {
        // 性能模式独有：6 区块以外**不再跑 RT GI**（0 弹）。
        // 保留 1 弹的话"6 区块外不走 RT"就没有意义了（成本照付）。
        MAX_DIFFUSE_BOUNCES = 0;
    }
}


const int MAX_WATER_INTERFACE_STEPS =
    6;


// Panes traverse without spending a diffuse bounce, so this is the only cap on
// a ray that keeps entering sheet after sheet (a window wall, a double-glazed
// pane, a glass corridor).  Sized like the water budget: deep enough for any
// authored stack, shallow enough that a degenerate case still terminates.
//
// Every step of this budget is a full TLAS traversal, and no vanilla build
// stacks more than two panes plus the two faces of one block (four faces).
// Eight was the "never think about it" number; four is the authored maximum.
#ifndef MAX_GLASS_INTERFACE_STEPS
#define MAX_GLASS_INTERFACE_STEPS 4
#endif


// The pane's mirror lobe inside the GI walk, switchable on its own.
//
// OFF by default since 2026-09-25.  It is a 4%-probability branch carrying
// ~25x the weight, drawn once per pixel per frame on a path that has no
// temporal average of its own, so what it buys a room is a speckle that looks
// exactly like noise on every pane -- while the mirror a viewer actually sees
// is the one the camera path (tracePrimaryGenericTransparentRadiance) traces
// deterministically.  The GI branch only ever added a scattered estimate of
// that same lobe to the indirect term.
//
// -DGI_GLASS_SPECULAR_REFLECTION=1 restores the roulette for an A/B.
#ifndef GI_GLASS_SPECULAR_REFLECTION
#define GI_GLASS_SPECULAR_REFLECTION 0
#endif


// ⭐ 远场（>= 160 格）只跑 1 次弹射。
//   理由：远场占屏面积最大，而第二次弹射的收益全在近场 —— 远处 1 次弹射
//   已经能把"附近被太阳直射的地面反弹过来"这一项拿到，那正是背光面从纯黑
//   变成有色所需的最小信息。砍掉它让新开的三档（1/8~1/32）成本再减半。
//   ⚠ 必须放在 MAX_DIFFUSE_BOUNCES 之后（GLSL 先声明后使用）。
int giBounceLimit =
    rtLodDistance >= 160.0 * perfL10TierScale()
        ? min(MAX_DIFFUSE_BOUNCES, 1)
        : MAX_DIFFUSE_BOUNCES;

#if SHARC_UPDATE
    // 2026-10-25：update 路径只需写满 SHARC_PROPAGATION_DEPTH 个顶点
    // （cacheIndices 数组深度就是它），更深的弹射写进去也永远不会被 query 命中。
    // 之前 update 与主渲染共用 4 跳 ⇒ 一半以上的 update 成本是白付的。
    //
    // 2026-10-28 修正：原来是 `min(giBounceLimit, SHARC_PROPAGATION_DEPTH)`，
    // 于是用户把「GI 跳数」设成 1 时 update 也只跑 1 跳。而渲染侧从本日起把第二跳
    // 交给缓存（见下面 !SHARC_UPDATE 分支），缓存里"顶点 1 的入射辐射"只有在
    // update 追到顶点 2 时才包含第二跳的反弹 ⇒ 改成固定 SHARC_PROPAGATION_DEPTH：
    // 不受用户跳数影响，也不受上面那条"远场只跑 1 跳"的 LOD 折扣影响（远场缓存
    // 同样需要第二跳，否则远场接管后整片偏暗）。
    // 2026-11-03：用户把「GI 跳数」设成 Off 时，update 也不该再跑 ——
    // 渲染侧 giBounceLimit=0 根本不进回路，也就不会查缓存，update 纯属白付成本。
    giBounceLimit =
        (cam.perfPrimary.z == 0u)
            ? 0
            : SHARC_PROPAGATION_DEPTH;
#else
    // ========================================================================
    // 2026-10-28：SHaRC 接管第二跳。
    //
    // 参考实现（本地 _ref/dxvk-remix）的做法：
    //   · rtx/pass/sharc/sharc_integrator_utilities.slangh:126-199
    //     onHitBeforeDirectLighting() —— 查询挂在**每个 secondary hit** 上、位于
    //     直接光之前；命中即 `return false` 终止路径（:191）。
    //   · :167-175 光锥判据用 pathState.segmentHitDistance 与
    //     prevPerceptualRoughness；后者由 :1326 的
    //     onBounceEnd(firstHitPerceptualRoughness) 在**主命中顶点**播种。
    //   · Remix 不额外钳 bounce 数（High 预设 pathMaxBounces=2、Ultra=4，
    //     rtx_options.cpp:539/561）。"第二跳不被追"是**命中缓存后终止**的自然结果，
    //     不是单独一条钳制。
    //
    // 本文件的挂点与判据本来就与上面一致（见下面 "5b-3：SHaRC 查询" 段）。这里补的
    // 是 Remix 没有、本工程需要的一步：命中率覆盖不满全屏时，第二跳仍会被实追
    // （实测第二跳 11.2ms，VulkanCore.cpp:9312）。所以 query 活跃时把回路截到 1 跳
    // —— 第二跳起全部由缓存给。与 Remix 在**命中时**逐像素等价（Remix 在 bounce 1
    // 命中后同样不再发第二跳），未命中时是纯损失：那一份能量缺失，靠 update 把缓存
    // 热起来补回。⚠ 这是自造口径、不在参考实现里；A/B 与回退都用 ORION_SHARC=0。
    // ========================================================================
#ifndef SHARC_FORCE_ONE_BOUNCE_CLAMP
#define SHARC_FORCE_ONE_BOUNCE_CLAMP 0
#endif
#if SHARC_FORCE_ONE_BOUNCE_CLAMP
    if(sharcQueryActive())
    {
        giBounceLimit = min(giBounceLimit, 1);
    }
#else
    if(sharcQueryActive() && rtLodDistance > 32.0)
    {
        giBounceLimit = min(giBounceLimit, 1);
    }
#endif
#endif


// ============================================================================
// 2026-11-09：GI 可见性门控 —— 跳过"期望贡献为 0"的反弹射线
//
// 判据（两条同时成立才跳过）：
//   1. 太阳完全照不到：shadowTransmittance ≈ 0
//      （已内含玻璃染色 / 云影 / 太阳盘可见性，与直射同一套口径）；
//   2. 主命中点所在的局部光网格 cell 里**没有任何光源**。
// 两者同时成立时，一次反弹带回来的入射辐射期望为 0 —— 射线是纯浪费，
// 典型场景是封闭洞穴、全遮蔽室内、远离任何光源的深处。
//
// 为什么用 cell 计数而不是 fogGatherLocalLights()：后者要把邻域 1~8 个 cell
// 的灯全部 gather 进数组（几十次 buffer 读），而这里只需要"有没有"。一次
// localLightGridLookupCell 就是 O(1)。
//
// 开关：cam.giRayParams.z > 0.5（宿主 ORION_GI_VISIBILITY_GATE，默认开）。
// 回退：设 0 ⇒ giBounceLimit 一行都不改，逐位回到改动前。
// ============================================================================
if(cam.giRayParams.z > 0.5f && giBounceLimit > 0)
{
    const bool orionSunFullyBlocked =
        all(lessThan(shadowTransmittance, vec3(0.001)));

    if(orionSunFullyBlocked)
    {
        uint orionGateFirstLightIndex = 0u;
        uint orionGateCellLightCount = 0u;

        // currentOrigin 在 :4938 初始化为主命中点世界坐标（GI 回路第一跳的起点）。
        // 按局部光最大半径检查相邻 64³ 网格 cell，消除跨越 64 格边界时的 GI 硬截断；
        // 打空（cell 未建 / 越界）按"无光源"处理，不会多减能量。
        const ivec3 orionGateCenterCell = localLightGridCellCoord(currentOrigin);
        const int minCellX = localLightGridNeighborMinOffset(currentOrigin.x);
        const int maxCellX = localLightGridNeighborMaxOffset(currentOrigin.x);
        const int minCellY = localLightGridNeighborMinOffset(currentOrigin.y);
        const int maxCellY = localLightGridNeighborMaxOffset(currentOrigin.y);
        const int minCellZ = localLightGridNeighborMinOffset(currentOrigin.z);
        const int maxCellZ = localLightGridNeighborMaxOffset(currentOrigin.z);
        bool orionCellHasLight = false;
        for(int dy = minCellY; dy <= maxCellY && !orionCellHasLight; ++dy)
        {
            for(int dz = minCellZ; dz <= maxCellZ && !orionCellHasLight; ++dz)
            {
                for(int dx = minCellX; dx <= maxCellX; ++dx)
                {
                    if(localLightGridLookupCell(
                           orionGateCenterCell + ivec3(dx, dy, dz),
                           orionGateFirstLightIndex,
                           orionGateCellLightCount) &&
                       orionGateCellLightCount > 0u)
                    {
                        orionCellHasLight = true;
                        break;
                    }
                }
            }
        }

        if(!orionCellHasLight)
        {
            giBounceLimit = 0;
        }
    }
}

// [10]/[11] 远场像素分布（GI 在 384 格外被压到 1/32，见 23347 行注释）
if(rtLodDistance >= 384.0)
{
    perfRayStatAdd(11u, 1u);
}
else if(rtLodDistance >= 160.0)
{
    perfRayStatAdd(10u, 1u);
}


// =====================================================
// Diffuse GI distance sampling
//
// 分档与抽稀位见上面 rtLodDiffuseGiEnabled 的**唯一出口**（14245 行起）。
// 这里只做转发，不再自己判距离 —— 两份判断漂移过，后果是静默变黑。
// =====================================================

bool runDiffuseGI =
    rtLodDiffuseGiEnabled;

// 2026-10-26 稀疏渲染：非活跃像素跳过整条 diffuse GI 回路（含 SHaRC 查询）。
if(!vptPixelActive)
{
    runDiffuseGI = false;
}

// 2026-11-04 A1：GI 时域重投影需要知道"本帧是否产出了新鲜 GI"。
// 在稀疏抽签**之后**捕获：非活跃像素同样视作"跳过"，走历史（它们不写输出，
// 但历史槽保持温热，下一帧激活时无需等 re-trace）。
#if !SHARC_UPDATE && !GI_HALFRES
bool giTracedThisFrame =
    runDiffuseGI;
#endif


float diffuseGILodWeight =
    rtLodDiffuseGiWeight;

// ============================================================================
// 2026-10-24 半分辨率 GI（HANDOFF-2026-10-24-GI-HALFRES.md）
//
// 整条 GI 回路（本行到下面"结束 GI while"那个闭括号）用**运行时** uniform 分支包住：
//   cam.giHalfParams.x == 0 → 旧路径，逐语义等价于改动前（A/B 的基线臂）
//   cam.giHalfParams.x != 0 → 跳过，改从 giHalfImage 做深度感知上采样（见下面）
//
// 为什么不用 #if 编译期删除：整个评估阶段只允许**一次冷编**（HANDOFF §7），旧路径
// 必须能随时切回，否则做 A/B 要再等一次 15 分钟。代价是寄存器/occupancy 仍按
// "含 GI 回路"算 ⇒ 收益会打折。先拿到上界；确认值得后第二次冷编再改成编译期删除
// （那时已有数值记录，不再需要基线臂）。
//
// ⚠ 花括号**故意**从 diffuseGILodWeight 之后才开始：indirectLight（21855）、
//   dbgLocalBounceAccum（21858）与 diffuseGILodWeight 本身在回路结束后仍被
//   远场尾部读，必须留在外层作用域（2026-10-24 用声明扫描核实过，块内 depth-0
//   声明共 17 个，只有这 3 个在块外有引用）。
// ============================================================================
#if !GI_HALFRES
if (cam.giHalfParams.x == 0.0f)
#endif
{
#ifndef ORION_K2_INDIRECT_COMPOSE_ONLY
// 2026-10-11 5b-3：回路永远跑（主命中不再提前退出）。截断发生在**回路内部**的
// 间接命中处 —— 见下面 "5b-3：SHaRC 查询" 段。
// [1] 本像素是否跑 diffuse GI
if(runDiffuseGI)
{
    perfRayStatAdd(1u, 1u);
}

while(
    runDiffuseGI &&
    diffuseBounceCount <
    giBounceLimit
)
{
    // ------------------------------------------------------------------------
    // 2026-10-11 5b-3 诊断：回路体进入计数（lock[19]，1/256 散列抽样）
    // 用途：把 "到达=0" 的两种可能分开 ——
    //   · 回路根本没跑（此项 = 0）⇒ 查 runDiffuseGI / GI 跳数；
    //   · 回路跑了，但我插入点所在的那个分支没被走到（此项 > 0 而 到达 = 0）
    //     ⇒ 说明我把查询挂在了某条不常走的材料分支里，要换挂点。
    // 放在回路体第一条语句的位置（在俄罗斯轮盘之前），保证"进过回路"必被计到。
    // ------------------------------------------------------------------------
    {
        const bool sharcLoopDiagSampled =
            ((uint(pixelCoord.x) * 7u + uint(pixelCoord.y) * 13u) & 255u) == 0u;

        // ⚠ 2026-10-12 实机根因修复：同上面的 lock[12]，这里也必须先过 `sharcMode()`。
        // 它是回路体第一条语句、只挂空间散列 ⇒ ORION_SHARC=0 时同样会向**设备地址 0**
        // 做 atomicAdd ⇒ GPU 页错误 ⇒ pass 永不完成 ⇒ 5 秒信号量超时（进世界即崩）。
        // 两个计数器都是仪器，门控与查询同源后，"SHARC 关"这条路径才真正一行缓存都不碰。
        if(sharcMode() != 0u && sharcLoopDiagSampled)
        {
            RWStructuredBuffer_uint sharcLoopDiagCounter =
                RWStructuredBuffer_uint(sharcCacheAddress(3u));

            atomicAdd(sharcLoopDiagCounter.data[19], 1u);
        }
    }

                    if(giRouletteEnabledForVariant())
                    {
                        // Perf-L10.1 stage 11: Russian-roulette survival with energy
                        // compensation, mirroring the reference renderer's policy.
                        // Unbiased in expectation: surviving paths carry the weight of
                        // the terminating ones back, so only the variance changes.
                        // Uses a private RNG state so the main sampling stream is not
                        // perturbed and the stage stays comparable to the baseline.
                        {
                            // 2026-10-23：变量旧名 rrLuminance 是**误名** —— 这里算的是
                            // 最大分量 max(r,g,b)，不是亮度 0.2126r+0.7152g+0.0722b。
                            // 用最大分量而非亮度是刻意的：按亮度度量会让饱和色（纯蓝、
                            // 纯红）拿到偏低的生存率，颜色分量之间方差失衡。
                            //
                            // ⚠ 没有 etaScale（参考实现有 `× etaScale` 一项）：本工程玻璃
                            // 走薄壁 pane 判据、不做体内折射，进/出界面 η 相消 ⇒ 净
                            // etaScale ≡ 1。**不是"不需要"，是"恒等于 1"** —— 将来接厚
                            // 介质时，要同时补 etaScale 的累积与它在下面的度量里的乘法。
                            // 曲线对比与"为什么不照抄参考实现"见 docs/decisions/russian-roulette.md。
                            float rrMaxComponent = max(
                                max(currentThroughput.r, currentThroughput.g),
                                currentThroughput.b
                            );

                            float rrLow = clamp(
                                0.85 - sqrt(max(rrMaxComponent, 0.0)),
                                0.0,
                                1.0
                            );

                            float rrTermination = clamp(
                                rrLow * rrLow +
                                max(
                                    0.0,
                                    float(diffuseBounceCount) /
                                        float(giBounceLimit) - 0.4
                                ),
                                0.0,
                                1.0
                            );

                            // =================================================
                            // 2026-10-09 方差收敛（用户实测："GI 阴影在静止时呈噪点，
                            // 移动视角反而正常"）。
                            //
                            // 原下限 0.05 ⇒ 侥幸存活的那条路要背 **20 倍**权重。
                            // 一帧里只有极少数像素吃到这记 20× 尖峰，而 DLSS RR 的
                            // anti-firefly 夹紧又会把它按掉大半 —— 两头都不占：既没被
                            // 时间平均收敛掉，又留下一批"孤立、夹不掉"的亮点。移动时
                            // 像素不断换新、眼睛分辨不出；静止时它们一直杵在原地。
                            //
                            // 抬到 0.20（单跳最多 5× 权重）：方差按权重平方量级下降，
                            // 代价是终止得更少、省下的时间少一点。要回满血有两条路：
                            //   · 设置里把「GI 俄罗斯轮盘」关掉对比（会同时失去那 −5.9ms）；
                            //   · 或把这里调回 0.05。
                            // 注意本工程 rrState 取自**逐帧推进的 seed**（seed 含
                            // cam.frameCount），所以各弹射之间的掷骰本来就是独立的，
                            // 这里要治的纯粹是"权重大"这一件事，不是相关性。
                            // =================================================
                            // 2026-11-07 A/B：Remix 的 russianRouletteMaxContinueProbability
                            // （默认 0.9）—— 本工程现在只要 T ≥ 0.7225 就完全不终止；
                            // 加上限后每跳至少 (1-cap) 概率终止，亮路径权重 ≤1/(cap)。
                            // 默认 1.0 = 现行为（逐位不变）；A/B 用
                            // -DGI_RR_MAX_SURVIVAL=0.9 编第二份 SPV 对比。
#ifndef GI_RR_MAX_SURVIVAL
#define GI_RR_MAX_SURVIVAL 0.9
#endif
                            // stage 37 = 同局消融臂（上限 0.9），base 下等于 GI_RR_MAX_SURVIVAL
                            float rrSurvival = min(
                                max(1.0 - rrTermination, 0.20),
                                perfL10RrCapNeg90() ? 0.9 : GI_RR_MAX_SURVIVAL);

                            // =================================================
                            // 2026-11-07 掷骰换蓝噪声（确定性优化，无需跑局验证
                            // 正确性 —— 见 docs/decisions/russian-roulette.md
                            // "2026-11-07 GitHub 调研"节）。
                            //
                            // Remix 的 RR 掷骰就是蓝噪声
                            // （calculateRussianRouletteContinueProbability 的
                            // 调用点走 getNextSampleBlueNoise）。本工程原来用
                            // pcg_hash 白噪声：哪些像素被终止在空间上是纯随机
                            // 撒点，而存活者背的 5× 权重尖峰也按同一分布撒点
                            // ⇒ 2026-10-09 实测的"静止时 GI 阴影孤立亮点"正是
                            // 这批白噪声撒点的尖峰，DLSS-RR 的时域累积与空域
                            // 滤波都难吃掉。换成蓝噪声后，终止/存活在空间上
                            // 聚成蓝噪声分布，时域滤波面对其相关工作最好。
                            //
                            // 无偏性与期望路径长度都不变：rrSurvival 公式未动，
                            // 掷骰的边际分布仍是 [0,1) 均匀（0..255/255 共 254
                            // 级，min 钳制在 glassBlueNoiseSample 内），变的只是
                            // 掷骰的空间相关性。私有 RNG 语义保持：不读主流
                            // sampler 流（这里根本没有 sampler 流，原来就是
                            // seed 异或常数旁路出来的）。
                            //
                            // seedIndex = 弹射序号 + 37：每个弹射一档 R2 相位
                            // ⇒ 相邻弹射的终止决策在空间上去相关（Remix 用
                            // 递增计数器，同效）；+37 与玻璃选层链
                            // （rgen_transparent_primary 用 frameCount/64，低位
                            // 相位）错开。R2 对任意整数列等分布，偏移只影响相位。
                            // temporalIdx = cam.frameCount & 63 ⇒ 逐帧换层。
                            // 回退：编 SPV 时加 -DGI_RR_BLUE_NOISE=0。
                            // =================================================
#ifndef GI_RR_BLUE_NOISE
#define GI_RR_BLUE_NOISE 1
#endif
#if GI_RR_BLUE_NOISE
                            float rrRoll = glassBlueNoiseSample(
                                uvec2(pixelCoord),
                                cam.frameCount,
                                diffuseBounceCount + 37u);
#else
                            uint rrState = seed ^ 0x9e3779b9u;
                            float rrRoll = float(pcg_hash(rrState) & 0x00ffffffu)
                                         / float(0x01000000u);
#endif

                            if(rrRoll >= rrSurvival)
                            {
                                // [6] 被轮盘终止、根本没发出去的射线
                                perfRayStatAdd(6u, 1u);
                                break;
                            }

                            currentThroughput /= rrSurvival;
                        }
                    }

                    // [2] GI 射线数；[3]/[4] 按弹射序号；[7] 水里发出的
                    perfRayStatAdd(2u, 1u);
                    perfRayStatAdd(
                        diffuseBounceCount == 0 ? 3u : 4u,
                        1u
                    );
                    if(currentInsideWater)
                    {
                        perfRayStatAdd(7u, 1u);
                    }

                    hit.hitDistance = 1.0;
                    hit.state = makeTraceState(
                        currentInsideWater ? RT_PURPOSE_WATER_MEDIUM : RT_PURPOSE_FULL
                    );
                    hit.state |= RT_STATE_GI_SKIP_CLOUDS_BIT;

                    // 2026-10-25 A/B：GI 射线跳过 POM（含 POM 自阴影）。
                    // 命中侧只需让 getPOMDepthScale() 返回 0 即可走既有早退分支。
                    if(cam.giHalfParams.z > 0.5f)
                    {
                        hit.state |= RT_STATE_GI_NO_POM_BIT;
                    }
#if GI_SKIP_SUN_DISK
                    // MIS-A（见 RT_STATE_NO_SUN_DISK_BIT）：GI 回路发出的射线命中
                    // 天空时不要回传日盘 —— 太阳直射由 NEE 承担，两份并存在期望上
                    // 是净增量、在方差上是单点萤火虫。
                    hit.state |= RT_STATE_NO_SUN_DISK_BIT;
#endif

                    // 2026-11-01 stage 34（体素 DDA 探针，HANDOFF-2026-11-01-VOXELIZATION
                    // 步骤 0）：整段 BVH 遍历 + rchit 材质求值换成一次固定成本 DDA。
                    // payload.state 在上面已按本射线的口径播种（purpose / GI_NO_POM /
                    // NO_SUN_DISK），DDA 命中不再改写它 —— hitClass 位保持 0 =
                    // RT_HIT_OPAQUE ⇒ 下游走普通不透明 Lambert 分支，只是材质是近似的。
                    // 打空（30 格 / 64 步 / 未加载段）按 miss 回天空项，与真实方案的
                    // 远场行为一致。本档画面不正确是定义，不是 bug。
#if !SHARC_UPDATE && !defined(ORION_K2_INDIRECT_TRACE_ONLY)
                    if(perfL10VoxelDdaGi())
                    {
                        float ddaHitT;
                        vec3 ddaHitNormal;
                        if(traceVoxelGiProbeDDA(currentOrigin, currentDir,
                                                ddaHitT, ddaHitNormal))
                        {
                            payload.hitDistance = ddaHitT;
                            payload.normal = ddaHitNormal;
                            payload.albedoPacked = packAlbedoUnorm(vec3(0.5));
                            payload.roughness = 1.0;
                            payload.f0Metal = 0.0;
                            payload.emission = vec3(0.0);
                            payload.parallaxRayTravel = 0.0;
                            payload.pomSelfShadow = 1.0;
                            payload.pomMicroOcclusion = 0.0;
                            payload.glassAlpha = 0.0;
                        }
                        else
                        {
                            payload.hitDistance = -1.0;
                            payload.albedoPacked = packSkyRadianceUnorm(vec3(0.6));
                        }
                    }
                    else
#endif
                    {
#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
                    traceDiffuseSer(
                        currentOrigin,
                        currentDir,
                        uint(diffuseBounceCount) & 0x3u
                    );
#elif defined(RT_ENABLE_SER)
                    if(cam.perfSerAb.x != 0u)
                    {
                        traceDiffuseSer(
                            currentOrigin,
                            currentDir,
                            uint(diffuseBounceCount) & 0x3u
                        );
                    }
                    else
                    {
                        #ifdef ORION_RT_STAGE_COUNTERS
                        rtCntRayIssued();
                        #endif
                        traceRayEXT(topLevelAS, secondaryTraverseFlags(), g_secondaryCullMask, 0, 0, 0, currentOrigin, 0.001, currentDir, perfGiTraceMaxDistance(10000.0), 0);
                    }
#else
                    #ifdef ORION_RT_STAGE_COUNTERS
                    rtCntRayIssued();
                    #endif
                    traceRayEXT(topLevelAS, secondaryTraverseFlags(), g_secondaryCullMask, 0, 0, 0, currentOrigin, 0.001, currentDir, perfGiTraceMaxDistance(10000.0), 0);
#endif
                    }

if (payload.hitDistance < 0.0) {
    // [5] GI 未命中（打空 = 直接吃天空项）
    perfRayStatAdd(5u, 1u);
#if SHARC_UPDATE
    // 参考 Pathtracer.hlsl:470-483：未命中 ⇒ 把天空辐射度交给 SharcUpdateMiss，
    // 由它按每个**已记录顶点**的 sampleWeights 回填（这就是"到该顶点的入射辐射度"
    // 的来源）。天空值用本工程 miss 支自己那一份（unpackAlbedoUnorm(payload.albedoPacked) * 0.6），
    // 与紧接着的累加完全同一个量。
    vec3 sharcMissRadiance = unpackAlbedoUnorm(payload.albedoPacked) * 0.6;
    if(sharcState.pathLength > 0)
    {
        sharcMissRadiance = sharcClampUpdateRadiance(
            sharcParameters, sharcState.cacheIndices[0],
            sharcMissRadiance, vec3(sharcState.sampleWeights[0]),
            SHARC_FIREFLY_CLAMP_MULTIPLIER);
    }
    SharcUpdateMiss(sharcParameters, sharcState, sharcMissRadiance);
#endif
    indirectLight +=
        currentThroughput *
        unpackAlbedoUnorm(payload.albedoPacked) *
        0.6;
    break;
} else {
vec3 nextHitPos =
    currentOrigin +
    currentDir *
    payload.hitDistance;

vec3 nextNormal =
    normalize(
        payload.normal
    );

vec3 nextAlbedo =
    unpackAlbedoUnorm(payload.albedoPacked);

vec3 nextEmission =
    payload.emission;

float nextMaterialId =
    getLegacyMaterialId(payload.state);

float nextF0Metal =
    payload.f0Metal;
    float nextRoughness =
    max(
        payload.roughness,
        0.05
    );

#if SHARC_UPDATE
    // 参考 Pathtracer.hlsl:544-546：update pass 把粗糙度夹到阈值（默认 0.4，
    // 见 sharc_ubo.glsl 的 SHARC_ROUGHNESS_THRESHOLD）—— 缓存只用"足够漫反射"的
    // 表面来建。只影响 update 变体的采样；渲染变体不受影响。
    nextRoughness = max(SHARC_ROUGHNESS_THRESHOLD, nextRoughness);
#endif

float nextPOMSelfShadow =
    clamp(
        payload.pomSelfShadow,
        0.0,
        1.0
    );
// =====================================================
// Beer-Lambert for the segment we JUST travelled
// =====================================================

// Glass shares the 0.5 boundary of the legacy material-code range only by
// accident; it is a filter sheet, not a medium.  Keeping it out of this flag is
// what stops the Beer-Lambert segment below from treating a pane as an immersed
// interface -- and this same flag is what used to route every pane hit into the
// water branch that follows.
bool nextHitIsWater =
    nextMaterialId > 0.5 &&
    nextMaterialId < 2.5;

// 水背面命中意味着：
// 刚才这段射线是从水里面走到水面的。
bool hitWaterFromInside =
    nextMaterialId > 1.5;

bool segmentWasInsideWater =
    nextHitIsWater
        ? hitWaterFromInside
        : currentInsideWater;

if(segmentWasInsideWater)
{
    currentThroughput *=
        exp(
            -GI_WATER_ABSORPTION *
            payload.hitDistance
        );
}

#if GI_NESTED_DIELECTRIC
// =====================================================
// 2026-10-13：命中不透明体 ⇒ 刚才那段射线已经离开了半透明介质。
//
// 不清空的话介质态会一直残留（冰块的远面若被剔除，射线会直接打到它后面的石头），
// 之后一次"共面跳过"就可能跳错界面（例：残留的 3=冰 遇到 4=粘液 ⇒ 误判为
// "更弱的一体"而跳过）。nextMaterialId < 0.5 就是"这一跳既不是水也不是半透明"。
//
// 只清优先级，**不动** currentInsideWater —— 水的吸收账由它自己那条链维护
// （水下打到不透明方块时，水面那一跳仍会把介质态写回 14 或 15）。
// A/B：-DGI_NESTED_DIELECTRIC=0
// =====================================================
if(nextMaterialId < 0.5)
{
    currentMediumPriority =
        RT_NESTED_PRIORITY_AIR;
}
#endif

// =====================================================
// B (2026-09-25): GI traversal through glass
//
// Before this, a pane hit here fell straight into the water branch below: it
// consumed WATER_IOR / WATER_F0 / currentInsideWater and contributed nothing,
// so no GI path ever carried the dye.  Glass is a straight-through filter with
// exactly one Orion sheet per block, which is how the camera path
// (rt_test_translucent.rchit), the sun march
// (traceSunTransmittanceThroughWater) and the primary walkers
// (tracePrimaryTransparentRadiance, tracePrimaryGenericTransparentRadiance)
// already treat it.
//
// A pane is a mirror and a filter, and nothing else.  Two consequences:
//
//   * It gets the SAME thin-sheet factor pair as the camera path, from the
//     same function, so the reflection a GI path sees and the reflection the
//     camera sees cannot drift apart.
//   * It does NOT get an area-light term.  A smooth sheet re-radiates along
//     the mirror direction, not over a hemisphere: "sunlit ground seen
//     straight through the window" already arrives on the regular-transmission
//     lobe below, so a Lambertian emitter term would count that daylight a
//     second time, from a direction the glass never sends it.
//
// With the roulette enabled, each lobe divides by its own selection
// probability so the pair stays unbiased, and both keep the cost at ONE
// traversal per glass face -- the mirror branch redirects the existing ray
// instead of spawning a second one.  It is off by default (see
// GI_GLASS_SPECULAR_REFLECTION above): on a per-pixel-per-frame path the 4%
// branch is a 25x-weighted coin flip, and that variance is what a viewer sees
// as grain behind a pane.  Switched off, the pane dyes and transmits only and
// the mirror lobe is left entirely to the camera path.
// =====================================================
// =====================================================
// 2026-10-13（用户要求）：GI 侧的清玻璃也当空气
//
// 相机侧自 2026-10-07 起就是这条口径（tracePrimaryGenericTransparentRadiance 的
// clearGlassAsAir 分支，合成式在 `return mix(behindRadiance,
// clamp(glassTint,0,1) * litGlassTintScale, clamp(glassAlpha,0,1));`），
// 但 GI 走查只判 nextMaterialId >= 2.5 ⇒ 清玻璃在 GI 里仍被当"薄片染色过滤"
// （吃一遍薄片系列 + profile 权重），与相机侧不一致。这里把它对齐。
//
// litScale 的取光来源是**自创**：相机侧用 behindRadiance（玻璃后已经追出来的那份
// 辐射），而 GI 循环在命中玻璃那一刻还没有它。按用户选定，这里用
// "GI 已有的发光体/本地光取值 + 环境底光"，**零额外射线**。
//   已知局限：环境底光 getSurfaceAmbientRadiance() 是**全局**量，不会因为站在洞穴/
//   无光室内而变暗；能随内容变暗的只有"这一格本身是发光体"那一路。要真正的空间
//   响应需要在该点补一条可见性射线（用户明确选了不带射线的方案）。
//
// A/B：-DGI_CLEAR_GLASS_GI_AS_AIR=0 逐位回到旧档（清玻璃走下面的薄片染色）。
//      默认跟随 GI_CLEAR_GLASS_AS_AIR，与相机侧同一个开关。
// =====================================================
#ifndef GI_CLEAR_GLASS_GI_AS_AIR
#define GI_CLEAR_GLASS_GI_AS_AIR GI_CLEAR_GLASS_AS_AIR
#endif

// 与相机侧 clearGlassAsAir 分支的 TINT_LUMA_WEIGHTS / GLASS_TINT_LIGHT_REFERENCE
// 同值同义（那两个是函数内局部量，这里另起文件级常量，避免重名）。
const vec3 GLASS_GI_TINT_LUMA_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);
const float GLASS_GI_TINT_LIGHT_REFERENCE = 1.0;

if(nextMaterialId >= 2.5)
{
#if GI_NESTED_DIELECTRIC
    // =====================================================
    // 2026-10-13：嵌套介质优先级闸门
    //
    // 清玻璃放行：它被"当空气"（GI_CLEAR_GLASS_AS_AIR），本闸门完全不动它
    // ⇒ 与改动前逐位一致。这也是用户给的适用范围："除清玻璃块和板之外"。
    //
    //   · accepted = false ⇒ 我们正在更强的一体内部，这个更弱的共面界面不拥有
    //     这块空间：不染色、不打镜面、不折向，沿原方向穿过去。
    //   · accepted = true  ⇒ 与旧档完全同路（下面 2026-09-30 的水平面进出判定照旧）。
    //
    // 闸门放在步数预算**之前**：被跳过的界面不该消耗 MAX_GLASS_INTERFACE_STEPS。
    // A/B：-DGI_NESTED_DIELECTRIC=0
    // =====================================================
    if(!isClearGlassHit(payload.state))
    {
        if(
            !nestedInterfaceAccepted(
                nestedPriorityOfHit(payload.state),
                currentMediumPriority
            )
        )
        {
            currentOrigin =
                nextHitPos +
                currentDir * 0.001;

            continue;
        }
    }
#endif

    glassInterfaceStepCount +=
        1;

    if(
        glassInterfaceStepCount >
        MAX_GLASS_INTERFACE_STEPS
    )
    {
        break;
    }


    // =====================================================
    // 2026-09-30：水平玻璃面改按「入面 / 出面」判，不再只按「是不是物理底面」
    //
    // 旧判据 ID 3 = 入面（任何非底面）/ ID 4 = 物理底面，隐含一个前提：一块
    // 玻璃在这条射线上两个面都能被命中（先入后出）。而底面标记只由几何法线
    // 决定（geoNormal.y < -0.5），与射线方向无关 —— 于是**向上**走的射线
    // （接收点就在玻璃正下方）从底面**穿入**时被判成"出面"而跳过染料；它的
    // 对面（上表面）又通常被顶在玻璃上的方块背面剔除掉，而那个方块多半正是
    // 发光体本身 ⇒ 整块玻璃一次染料都没施加，GI 弹射把发光体的 emission
    // 原样穿过玻璃送给了正下方的接收点。
    //   症状：**任何方向都正常染色，只有玻璃正下方染不上色。**
    //
    // 水平面有确定的向外法线方向（物理底面 -Y / 顶面 +Y），所以进出可以精确
    // 判定：底面 + 射线向上 = 入面；顶面 + 射线向下 = 入面；其余 = 出面。
    // 出面只负责把射线带出方块 —— 一块玻璃仍然只施加一次染料（顶面被剔除时
    // 入面依然会施加，这正是本条修复要拿回来的那一份）。
    //
    // X/Z（竖直）面维持原判据（每次命中都施加）：本改动不去动水平方向的既有
    // 观感，因此"一块一过滤"在竖直穿越上仍是旧行为。
    //
    // 面的朝向用命中点的平面判定（哪个轴到相邻整数面的距离≈0），不用着色法线：
    // 着色法线可能被 PBR 法线贴图旋转，而玻璃几何面永远是轴对齐的（命中点本身
    // 是几何命中，不含 POM 位移）。
    // =====================================================
    vec3 glassHitFraction =
        fract(nextHitPos);
    vec3 glassHitPlaneDistance =
        min(
            glassHitFraction,
            vec3(1.0) - glassHitFraction
        );
    bool glassFaceIsHorizontal =
        glassHitPlaneDistance.y < glassHitPlaneDistance.x &&
        glassHitPlaneDistance.y < glassHitPlaneDistance.z;

    if(glassFaceIsHorizontal)
    {
        bool glassFaceIsBottom =
            (payload.state & RT_STATE_GLASS_BOTTOM_BIT) != 0u;

        bool glassFaceIsEntry =
            glassFaceIsBottom
                ? (currentDir.y > 0.0)
                : (currentDir.y < 0.0);

        if(!glassFaceIsEntry)
        {
            currentOrigin =
                nextHitPos +
                currentDir * 0.001;

            continue;
        }
    }


#if GI_CLEAR_GLASS_GI_AS_AIR
    // =====================================================
    // 2026-10-13：清玻璃当空气（GI 侧）—— 见文件上方那段总说明
    //
    // 相机侧是**直接合成**（mix），GI 是**累加**，等价的线性写法：
    //     indirectLight += throughput * sprite * litScale * a
    //     throughput    *= (1 - a)
    // a 是 0/1 二值（2026-10-09 精确取样后，见 rt_test_translucent.rahit 的说明）
    // 时它逐位等价于 mix：
    //     a = 1 的边框 ⇒ 加 tint×lit、后续权重归零（= 路径终止在边框上，实心）
    //     a = 0 的全透明心 ⇒ 什么都不加、权重不变（= 当空气）
    // 对连续的 a（voxy LOD 玻璃把 alpha 钳在 0.06~0.72）也正确，且不引入随机性。
    //
    // 位置：放在**水平面入/出面闸门之后** ⇒ 水平穿越时一块玻璃只加一次（出面
    // 已经被上面 continue 掉）。X/Z（竖直）面维持该闸门的既有口径（每次命中都
    // 施加），与那里既有的染料行为一致 —— 见上面 2026-09-30 那段。
    //
    // 走查不消耗漫反射弹数（与玻璃分支的既有约定一致），所以预算仍由上面的
    // glassInterfaceStepCount / MAX_GLASS_INTERFACE_STEPS 管。
    // =====================================================
    if(isClearGlassHit(payload.state))
    {
        float glassAirAlpha =
            clamp(payload.glassAlpha, 0.0, 1.0);

        // sprite × 顶点色 = 原版透明层要叠加的那份贴图色（相机侧叫 glassTint）。
        vec3 glassAirSprite =
            clamp(nextAlbedo, vec3(0.0), vec3(1.0));

        // 取光来源（自创，零额外射线）：GI 已有的发光体/本地光取值 + 环境底光。
        vec3 glassAirEnv =
            evaluateVLIEmitterRadianceAtHit(
                nextHitPos,
                nextNormal,
                nextAlbedo
            ) +
            getSurfaceAmbientRadiance();

        float glassAirLitScale =
            clamp(
                dot(glassAirEnv, GLASS_GI_TINT_LUMA_WEIGHTS) /
                GLASS_GI_TINT_LIGHT_REFERENCE,
                0.0,
                1.0
            );

        // 受光的贴图色：加性项（实心边框）。
        indirectLight +=
            currentThroughput *
            glassAirSprite *
            glassAirLitScale *
            glassAirAlpha;

        // 边框遮住它后面的光：全透明心 (a=0) 权重不变。
        currentThroughput *=
            (1.0 - glassAirAlpha);

        currentOrigin =
            nextHitPos +
            currentDir * 0.001;

        continue;
    }
#endif

    // Face the normal into the hemisphere the ray came from.  The entering
    // face of a block reports an outward normal, so this is normally a no-op;
    // it is here so reflect() and the offset cannot depend on how the mesh
    // happened to wind.
    vec3 paneNormal =
        nextNormal;

    if(dot(paneNormal, currentDir) > 0.0)
    {
        paneNormal =
            -paneNormal;
    }

    // The camera path's NdotV: an absolute cosine, because a pane is a
    // two-sided sheet and the block, not the hit face, decides which side the
    // daylight reaches.
    float paneCosTheta =
        clamp(
            -dot(currentDir, paneNormal),
            0.0,
            1.0
        );

    float paneFresnel =
        fresnelSchlickScalar(paneCosTheta, 0.04);

    // transmission_color: the pure dye.  unpackAlbedoUnorm(payload.albedoPacked) for a pane is already
    // the single-application colour (glassPaneTransmissionColor), and a profile
    // only attenuates energy -- never the dye -- so nothing else may be folded
    // in here.
    vec3 paneDye =
        clamp(nextAlbedo, vec3(0.0), vec3(1.0));

    // Same Orion thin-sheet split as the camera path: the internal
    // reflection series distributes the energy between the mirror lobe and the
    // straight transmission, so a nearly-opaque body collapses to plain Fresnel
    // reflection instead of darkening both lobes.
    vec3 paneReflectionFactor;
    vec3 paneTransmissionFactor;
    glassThinSheetFactorsScalarF(
        paneFresnel,
        paneDye,
        paneReflectionFactor,
        paneTransmissionFactor
    );

    // Thicker bodies (tinted glass / ice / slime / honey) attenuate the energy
    // that crosses them without darkening their dye; profile 0 -- clear and
    // stained glass -- is unattenuated at 1.0.
    float paneTransmissionWeight =
        getTransparentProfileTransmission(
            int(getTransparentProfile(payload.state))
        );

    float paneTransmissionProbability =
        1.0 - paneFresnel;


#if GI_GLASS_SPECULAR_REFLECTION
    if(rand(seed) < paneFresnel)
    {
        // Mirror lobe.  A reflection off the sheet never travels through the
        // coloured body, so the dye must NOT be applied on this branch.
        currentThroughput *=
            paneReflectionFactor /
            max(paneFresnel, 1.0e-3);

        currentOrigin =
            nextHitPos +
            paneNormal * 0.001;

        currentDir =
            normalize(
                reflect(
                    currentDir,
                    paneNormal
                )
            );

        continue;
    }
#else
    // The mirror lobe is switched off, so transmission is the only outcome and
    // carries the whole probability mass -- leaving the 4% reflection out of
    // the room instead of folding it into the transmission.
    paneTransmissionProbability =
        1.0;
#endif

    // Transmission lobe: straight through, dyed exactly once.
    currentThroughput *=
        paneTransmissionFactor *
        paneTransmissionWeight /
        max(paneTransmissionProbability, 1.0e-3);


    currentOrigin =
        nextHitPos +
        currentDir * 0.001;


    continue;
}


// =====================================================
// GI water reflection
//
// 只处理真实水面反射：
// 洞壁 -> 水面 -> 天空
//
// 如果反射方向被洞穴遮挡，就不会获得天空光。
// 因此完全封闭的地下水不会凭空照亮洞穴。
// =====================================================
if (nextMaterialId > 0.5)
{
#if GI_NESTED_DIELECTRIC
    // =====================================================
    // 2026-10-13：水界面的共面跳过
    //
    // 我们现在在一个**固体**半透明体内部（染色玻璃 / 遮光玻璃 / 冰 / 粘液 / 蜂蜜）
    // ⇒ 与它共面的那个水面界面不拥有这块空间，直接穿过去，既不折射也不反射。
    // 这一步只做"跳过"，**完全不动**下面既有的 currentInsideWater / exitingWater
    // 记账 ⇒ 不在这条链上的所有情形逐位不变。
    //
    // 为什么需要它：冰块的底面与水块的顶面是同位置的两个界面，旧档会给同一条
    // 真实边界算两次菲涅耳（两片共面面各一次）。固体的那个界面先被处理并"接管"
    // 空间，水的这一片就应当让位。
    // A/B：-DGI_NESTED_DIELECTRIC=0
    // =====================================================
    if(nestedInsideSolidMedium(currentMediumPriority))
    {
        currentOrigin =
            nextHitPos +
            currentDir * 0.001;

        continue;
    }
#endif

    waterInterfaceStepCount +=
        1;


    if(
        waterInterfaceStepCount >
        MAX_WATER_INTERFACE_STEPS
    )
    {
        break;
    }


    const float WATER_IOR =
        1.33;

    const float WATER_F0 =
        0.0200593;

    // =================================================
    // 1.0 = Air -> Water
    // 2.0 = Water -> Air
    // =================================================
    bool exitingWater =
        nextMaterialId > 1.5;

#if GI_NESTED_DIELECTRIC
    // 2026-10-13：维持介质态 —— 进入水记 14、离开水清空。与 currentInsideWater
    // 同源同向（exitingWater 的语义正是"刚才这段射线从水里走到水面"）。
    // 只写状态，不参与下面任何能量计算 ⇒ 不改变现有记账。
    currentMediumPriority =
        exitingWater
            ? RT_NESTED_PRIORITY_AIR
            : RT_NESTED_PRIORITY_WATER;
#endif

    vec3 interfaceNormal =
        waterInterfaceNormalForRay(
            nextNormal,
            currentDir
        );

    // 2026-11-20：Fresnel 余弦改用稳定版（水平水面取平面余弦 |dir.y|）。
    // 逐三角形的波浪法线在掠射视角下让 F→1、透射→0，水底整块变黑成三角；
    // 折射/反射的**方向**仍用 interfaceNormal（波浪法线），观感不变。
    // 详见 rgen_trace_helpers.glsl 的 waterFresnelCosine 说明。
    float cosTheta =
        waterFresnelCosine(
            currentDir,
            interfaceNormal
        );

    float waterFresnel =
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
        rand(seed);


    if(
        waterChoice <
        waterFresnel
    )
    {
        // =============================================
        // Reflection
        //
        // 没有跨介质。
        // Air 仍是 Air。
        // Water 仍是 Water。
        // =============================================
        currentInsideWater =
            exitingWater;

        currentOrigin =
            nextHitPos +
            interfaceNormal *
            0.001;

        currentDir =
            normalize(
                reflect(
                    currentDir,
                    interfaceNormal
                )
            );
    }
    else
    {
        // =============================================
        // Transmission / Refraction
        // =============================================
        float eta =
            exitingWater
                ? WATER_IOR
                : (1.0 / WATER_IOR);

        vec3 refractedDir =
            refract(
                currentDir,
                interfaceNormal,
                eta
            );

        // Water -> Air 可能发生 TIR。
        if(
            dot(
                refractedDir,
                refractedDir
            ) < 1.0e-6
        )
        {
            // 全反射，所以仍然留在原介质。
            currentInsideWater =
                exitingWater;

            currentOrigin =
                nextHitPos +
                interfaceNormal *
                0.001;

            currentDir =
                normalize(
                    reflect(
                        currentDir,
                        interfaceNormal
                    )
                );
        }
        else
        {
            // 真正跨过界面。
            //
            // Air -> Water:
            //     currentInsideWater = true
            //
            // Water -> Air:
            //     currentInsideWater = false
            currentInsideWater =
                !exitingWater;

            currentOrigin =
                nextHitPos -
                interfaceNormal *
                0.001;

            currentDir =
                normalize(
                    refractedDir
                );
        }
    }

    continue;
}
// LOCAL_LIGHT_RIS guard added 2026-09-25.  The primary path has always gated
// this VLI emitter-at-hit glow behind the switch (see localEmitterEmission),
// but the copy here ran unconditionally.  The function reads the voxel of the
// block behind the hit face and adds its emission colour with NO visibility
// ray and NO glass filter -- its own comment calls it "the only local-light
// contribution with neither".  Two consequences with the new chain live:
//   * duplicate: the ReSTIR DI chain already carries the emitter's light at
//     the primary, and the route-A NEE now carries it at this vertex;
//   * the corner leak: `hitPos - dominantFaceNormal * 0.02` can land in an
//     EMISSIVE NEIGHBOUR block where the interpolated normal tilts (corners),
//     and the term then paints that block's colour straight through the
//     geometry.  It only exists inside the GI loop, which is why the leak needs
//     "GI 跳数" > 0.
// nextEmission stays: the ray genuinely landed on that surface, so that
// surface's own emission is a real path, occluded by construction.
// 2026-09-30 收口：ReSTIR DI 链已删除，经典 0.31 RIS 链在 GI 顶点无条件叠加该项。
vec3 nextUnifiedEmission =
    nextEmission +
    evaluateVLIEmitterRadianceAtHit(
        nextHitPos,
        nextNormal,
        nextAlbedo
    );

// =====================================================
// 2026-10-24 R1：GI 命中处发光项的萤火虫钳制（对齐 DLSS-RR 输入口径）
//
// 病灶：GI 命中点的发光体没有 NEE（本工程只对太阳做 NEE），只能靠 BSDF 偶然
// 命中发光方块；2026-09-30 的注释写着"经典 0.31 RIS 链在 GI 顶点**无条件**叠加
// 该项"。单次命中即数十倍均值 ⇒ 落在 RR 时域核里既夹不掉、又会被它的
// anti-firefly 削掉能量，表现为"孤立亮点 + 该亮的地方反而暗"。
//
// 为什么不动轮盘下限：rrSurvival = max(1 - rrTermination, 0.20)，而
// rrTermination 由 0.85-sqrt(max(throughput)) 决定；albedo 0.15 时生存已是
// 0.79（权重 1.27×），下限 0.20 只在 albedo<0.05 的近黑表面才夹住 —— 抬它是空操作。
//
// 钳制的是"发光项"（自发光 + VLI 辉光）合计，不是整条路径：经过 albedo/throughput
// 调制的部分不动，所以背光面间接光不会整体变平。
// A/B：-DGI_FIREFLY_EMISSION_MAX=0 关闭（逐位回到改动前）。
// =====================================================
#ifndef GI_FIREFLY_EMISSION_MAX
#define GI_FIREFLY_EMISSION_MAX 4.0
#endif

    {
        const float emissionMax =
            float(GI_FIREFLY_EMISSION_MAX);

        if(emissionMax > 0.0)
        {
            nextUnifiedEmission =
                min(
                    nextUnifiedEmission,
                    vec3(emissionMax)
                );
        }
    }
// =====================================================
// A3-2
// Secondary hit PBR direct sun lighting
//
// 使用与现有 PBR direct-light helper 相同的：
// - LabPBR F0 / metal
// - GGX NDF
// - Smith geometry
// - Schlick Fresnel
// - dielectric / metal energy split
// - real sun visibility / water transmittance
// =====================================================

#if GI_BOUNCE_LAMBERT_DIRECT
vec3 bounceDirect =
    evaluateDirectSunLightingDiffuse(
        nextHitPos,
        nextNormal,
        nextAlbedo,
        nextF0Metal,
        nextPOMSelfShadow,
        sunDir
    );
#else
vec3 bounceDirect =
    evaluateDirectSunLighting(
        nextHitPos,
        nextNormal,

        // 当前 GI ray 从上一表面射向这里，
        // 所以 secondary surface 的 outgoing/view direction
        // 是 currentDir 的反方向。
        normalize(
            -currentDir
        ),

        nextAlbedo,
        nextRoughness,
        nextF0Metal,
        nextPOMSelfShadow,
        sunDir
    );
#endif

// =====================================================
// Sun -> Water -> GI secondary surface
//
// 之前这条显式水面反射只作用于 Primary surface。
// 这里把同样经过真实水面 + 太阳可见性验证的能量
// 接入 secondary hit，使它能够继续通过 GI 传给当前表面。
// =====================================================

int nextF0MetalCode =
    int(
        round(
            clamp(
                nextF0Metal,
                0.0,
                1.0
            ) *
            255.0
        )
    );


float nextMetallic =
    nextF0MetalCode >= 230
        ? 1.0
        : 0.0;


// 2026-10-07（W2）：这条 GI 顶点射线原来**不受**"水面反射间隔"约束。
//
// 上游主像素那条（见 waterReflectionInterval/runWaterReflection 的定义处）按
// cam.perfSecondary.z 每 N 帧才打一次，而这里是无条件的 ⇒ interval=4（出厂默认）
// 时省下的只是主像素那份，GI 顶点在 3/4 的帧里仍要付"一条 classify 射线 + 一条
// 水内太阳透射 march"（traceWaterReflectedSun 全程只有 receiverNdotL<=0 一个早退）。
//
// 判据与主路径**逐字同源重算**（同样的 UBO 输入 ⇒ 同样的值），这样整帧的这一项
// 要么都在、要么都不在 —— 时域滤波只面对一件事，比"半帧有半帧没有"更干净。
// 局部重算而不引用上游变量：两处未必在同一层大括号里，重算比跨作用域引用安全。
#if SHARC_UPDATE
vec3 bounceWaterReflectedSun = vec3(0.0);
#else
#ifndef GI_BOUNCE_WATER_SUN_OPT
#define GI_BOUNCE_WATER_SUN_OPT 1
#endif
uint bounceWaterReflectionInterval =
    clamp(cam.perfSecondary.z, 0u, 4u);
#if GI_BOUNCE_WATER_SUN_OPT
uint effectiveBounceWaterInterval =
    bounceWaterReflectionInterval == 0u ? 0u : max(bounceWaterReflectionInterval, 4u);
bool runBounceWaterReflection =
    diffuseBounceCount == 0 &&
    effectiveBounceWaterInterval > 0u &&
    (cam.frameCount % effectiveBounceWaterInterval) == 0u &&
    rtLodDistance < 48.0 * perfL10TierScale() &&
    payload.hitDistance < 32.0
#if WATER_REFLECT_SUN_OPT_V1
    && sunDir.y > 0.02
#endif
    ;
float bounceWaterReflectedSunFactor =
    runBounceWaterReflection
        ? traceWaterReflectedSun(
            nextHitPos,
            nextNormal,
            sunDir
        )
        : 0.0;

vec3 bounceWaterReflectedSun = vec3(0.0);
if(bounceWaterReflectedSunFactor > 1.0e-5)
{
    bounceWaterReflectedSun =
        getTimeDrivenSkyLightColor() *
        3.2 *
        getTimeDrivenSkyLightRadianceScale() *
        nextAlbedo *
        (1.0 - nextMetallic) *
        bounceWaterReflectedSunFactor *
        c3CloudSunTransmittance(nextHitPos, sunDir);
}
#else
bool runBounceWaterReflection =
    bounceWaterReflectionInterval > 0u &&
    (bounceWaterReflectionInterval == 1u ||
     (cam.frameCount % bounceWaterReflectionInterval) == 0u)
#if WATER_REFLECT_SUN_OPT_V1
    && sunDir.y > 0.02
    && rtLodDistance < 96.0 * perfL10TierScale()
#endif
    ;
float bounceWaterReflectedSunFactor =
    runBounceWaterReflection
        ? traceWaterReflectedSun(
            nextHitPos,
            nextNormal,
            sunDir
        )
        : 0.0;


vec3 bounceWaterReflectedSun =
    getTimeDrivenSkyLightColor() *
    3.2 *
    getTimeDrivenSkyLightRadianceScale() *
    nextAlbedo *
    (1.0 - nextMetallic) *
    bounceWaterReflectedSunFactor *
    c3CloudSunTransmittance(nextHitPos, sunDir);
#endif
#endif


// =====================================================
// LL-P3.3 (route A, 2026-09-24): explicit Local-Light NEE at the GI vertex.
//
// This replaces the G2B-1 / G2B-2 baseline, which accepted local-light energy
// ONLY when the BSDF path itself happened to land on an emitter.  Route C (the
// G2B directional guide) was measured and rejected before this: the guide can
// only deliver a bounce from the SECOND diffuse vertex, i.e. it forces
// "GI 跳数" >= 2, and that extra GI segment (whole scene ray + its own PBR
// shading + its own sun NEE) costs far more than the guide's ALU scan saves.
// This call instead runs on the vertex the GI path already guarantees -- the
// first one -- and costs exactly one shadow ray.
//
// It must stay on the NEW estimator (evaluateVPTRISDirectLighting ->
// ...Single): that samples the same per-section VPT source directory / alias
// table the ReSTIR DI chain uses.  What the retired G2B-2 rule forbade is
// reconnecting a GI hit to the OLD baked virtual emitter faces
// (evaluateVPTLocalSecondaryBounce, the block-face irradiance bake) -- retired
// data that would double count against the NRD composite, not this path.
//
// The private RNG state keeps the main sampling stream untouched, so the frame
// differs from the previous build ONLY by this term -- the same convention the
// Russian-roulette block above uses.
//
// Skipped when the receiver's own block is emissive: the estimator samples the
// emitter's volume, so a receiver inside an emitter would receive its own
// emission, which nextUnifiedEmission already added.  That is the same
// rejection g2bSurfaceBelongsToEmitter performed for the legacy guide.
// =====================================================

vec3 bounceLocalDiffuse =
    vec3(0.0);

// ============================================================================
//
// 参考：.tmp/ref/RTXGI/Samples/Pathtracer/Pathtracer.hlsl:548-565（查询块本体）、
//       :499-505（几何法线翻向入射方向）、:446/:554-557/:674（上一顶点粗糙度）。
//
// 位置与参考一致：**几何/材质求值之后、NEE 之前**。这一点此前被我搞错过两次 ——
// 先挂主命中（用体素平均值替换可见面 GI ⇒ 实测"错位"），后挂回路尾部（而
// `diffuseBounceCount >= giBounceLimit` 的 break 在采样之前，GI跳数=1 时尾部
// 根本到不了 ⇒ 实测"到达=0"）。参考的位置在 break 之前，必然被执行到。
//
// 与参考逐条对齐：
//   · 段长：`payload.hitDistance > voxelSize * sqrt(3.0)`（参考 :552）
//   · 光锥：alpha = min(prevRoughness, 0.99)^2，footprint > voxelSize（参考 :554-557）
//     —— prevRoughness 初值 0 ⇒ 第一跳 footprint=0 ⇒ **主命中自动被排除**（参考 :446）
//   · 法线：几何法线且翻成朝向入射方向（参考 :499-505）
//   · 用法：`indirectLight += 缓存值 * currentThroughput; break;`（参考 :562-564）
//
// 参考里没有、本工程特有的一处：水/玻璃命中走上面的独立分支（含 continue），
// 到不了这里 ⇒ 天然不查缓存。参考是无此分支的通用路径追踪器，故这里无对应物可抄。
//
// lock[11]/lock[12] 两个计数器是**仪器**（参考没有）：只回答"是否真的生效"。
// 这个工程被"静默失效"坑过多次，所以保留这两个数，其余自造诊断已全部删除。
// ============================================================================
{
    const uint sharcGridLevel = HashGridGetLevel(
        nextHitPos, sharcParameters.hashGridParameters);
    const float sharcVoxelSize = HashGridGetVoxelSize(
        sharcGridLevel, sharcParameters.hashGridParameters);

    bool sharcValidHit =
        payload.hitDistance > sharcVoxelSize * sqrt(3.0);

    materialRoughnessPrev = min(materialRoughnessPrev, 0.99);
    const float sharcAlpha = materialRoughnessPrev * materialRoughnessPrev;
    const float sharcFootprint = payload.hitDistance *
        sqrt(0.5 * sharcAlpha * sharcAlpha / (1.0 - sharcAlpha * sharcAlpha));
    sharcValidHit = sharcValidHit && sharcFootprint > sharcVoxelSize;

    // ⚠ 2026-10-12 实机根因修复：**必须加 `sharcMode() != 0u` 门控**。
    //
    // 这里的 lock[12] 是仪器计数器，但它落在 SHaRC 的 lock 缓冲里。原先只有空间散列
    // 门控、没有模式门控 —— 于是 ORION_SHARC=0 时：
    //   sharcCacheAddress(3u) 由 cam.sharcBuffersLo/Hi[3]（两者都是 0）拼出
    //   → **设备地址 0** → atomicAdd 写向地址 0 → GPU 页错误
    //   → 该 RT pass 永不完成 → RenderPearl 报
    //     "5s timeout reached when waiting for VK semaphore: BEGIN_RENDER_PASS
    //      Item in hand" ⇒ 进世界第 1~7 帧就崩。
    //
    // 实机佐证：同一份二进制，SHARC 开 ⇒ 地址有效、计数无害，能跑上千帧；
    //           SHARC 关 ⇒ 必崩（16:10、16:24 两局 frame 1~7）。这条修完两者都安全。
    // 教训：凡是访问缓存缓冲的仪器，门控必须与**查询本身**同源（sharcMode()），
    //       而不能只靠"采样率"这类无关条件。
    const bool sharcSampleDiag =
        sharcMode() != 0u &&
        ((uint(pixelCoord.x) * 7u + uint(pixelCoord.y) * 13u) & 255u) == 0u;
    if(sharcSampleDiag)
    {
        RWStructuredBuffer_uint sharcDiagCounter =
            RWStructuredBuffer_uint(sharcCacheAddress(3u));

        atomicAdd(sharcDiagCounter.data[12], 1u);
        if(sharcValidHit)
        {
            atomicAdd(sharcDiagCounter.data[13], 1u);
        }
    }

    if(sharcValidHit && sharcQueryActive())
    {
        SharcHitData sharcQueryHitData;
        sharcQueryHitData.positionWorld = nextHitPos;
        sharcQueryHitData.normalWorld =
            dot(nextNormal, currentDir) > 0.0 ? -nextNormal : nextNormal;
        sharcQueryHitData.materialDemodulation = max(nextAlbedo, vec3(0.05));
        sharcQueryHitData.emissive = nextUnifiedEmission;

        vec3 sharcRadiance = vec3(0.0);

        if(SharcGetCachedRadiance(
            sharcParameters, sharcQueryHitData, sharcRadiance, false))
        {
            indirectLight += sharcRadiance * currentThroughput;

            if(sharcSampleDiag)
            {
                RWStructuredBuffer_uint sharcQueryCounter =
                    RWStructuredBuffer_uint(sharcCacheAddress(3u));

                atomicAdd(sharcQueryCounter.data[11], 1u);
            }

            break; // 参考 :564：查到缓存即终止路径
        }
    }
}

ivec3 neeReceiverBlock =
    ivec3(
        floor(
            nextHitPos -
            vptDominantBlockFaceNormal(
                nextNormal
            ) *
            0.02
        )
    );

// The vertex must be front-facing before any NEE runs.  The estimator accepts
// a candidate through its own receiver-cosine test against this normal, so a
// back-facing hit -- a quad seen from behind, or a corner whose interpolated /
// quantised normal points away -- would let emitters from the FAR side of that
// surface through, which is exactly the "light leaking out of a corner"
// signature.  The loop already rejects back-facing vertices below, but only
// AFTER every term has been accumulated, so the test is repeated here.
bool neeVertexFrontFacing =
    dot(
        nextNormal,
        normalize(
            -currentDir
        )
    ) > 1.0e-5;

#if VPT_GI_VERTEX_LOCAL_LIGHT
if(
    (diffuseBounceCount < 2 || SHARC_UPDATE != 0) &&
    neeVertexFrontFacing &&
    ((
        vliReadSourceVoxel(
            neeReceiverBlock
        ) >>
        2u
    ) &
    15u) == 0u
)
{
    uint neeRngState =
        seed ^
        0x9e3779b9u;

    bounceLocalDiffuse =
        evaluateVPTRISDirectLighting(
            nextHitPos,
            nextNormal,
            nextAlbedo,
            neeRngState
        );
}
#endif


indirectLight +=
    currentThroughput *
    (
        nextUnifiedEmission +
        bounceDirect +
        bounceWaterReflectedSun +
        bounceLocalDiffuse
    );

#if SHARC_UPDATE
// ============================================================================
// 2026-10-11 5b-4：update 变体的 SharcUpdateHit —— 照参考 Pathtracer.hlsl:619-629
//
// 传入的 directLighting = **该顶点本段的入射辐射度（直接光 + 自发光，未被 throughput
// 加权）** —— 与上面累加括号里那一份逐字相同（参考里那份叫 sampleRadiance，因为在
// update pass 里它每 bounce 被重置为 0、throughput 被重置为 1：参考 :461-463）。
// 返回值 false ⇒ 照参考 :628-629 立即终止路径（缓存重采样已经给出该顶点的值）。
// ============================================================================
{
    SharcHitData sharcUpdateHitData;
    sharcUpdateHitData.positionWorld = nextHitPos;
    sharcUpdateHitData.normalWorld =
        dot(nextNormal, currentDir) > 0.0 ? -nextNormal : nextNormal;
    sharcUpdateHitData.materialDemodulation = max(nextAlbedo, vec3(0.05));
    sharcUpdateHitData.emissive = nextUnifiedEmission;

    // 与 update/pixel 选择同源：独立 RNG 状态，避免扰动本工程自己的采样流
    // （update 变体是独立 dispatch，本来也不影响成像路径）。
    uint sharcUpdateRngState = seed ^ 0x6d2b79f5u;
    const float sharcUpdateRandom =
        float(pcg_hash(sharcUpdateRngState) & 0x00ffffffu) /
        float(0x01000000u);

    const vec3 sharcUpdateRadiance =
        bounceDirect +
        bounceWaterReflectedSun +
        bounceLocalDiffuse;
    const vec3 sharcUpdateClampedRadiance = sharcClampUpdateRadiance(
        sharcParameters, sharcState.cacheIndices[0],
        sharcUpdateRadiance, vec3(sharcState.sampleWeights[0]),
        SHARC_FIREFLY_CLAMP_MULTIPLIER);

    const bool sharcUpdateCommitted =
        SharcUpdateHit(
            sharcParameters,
            sharcState,
            sharcUpdateHitData,
        sharcUpdateClampedRadiance,
            sharcUpdateRandom);

    // 仪器（参考没有）：lock[8] = 该顶点成功写入缓存；lock[10] = 返回 false。
    // ⚠ 注意 lock[10] 的语义与旧 piggyback 时代不同：SharcUpdateHit 返回 false
    //   有两种原因 —— 哈希插入失败（SharcCommon.h:591-592）**或**缓存重采样命中
    //   （:632，表示"该顶点直接用了缓存值、无需再追"）。缓存变热之后后者会占
    //   大头，所以 lock[10] 高**不代表**网格饱和；判读看 [SHARC STATS] 的说明。
    // 2026-11-25：纳入同一 1/256 空间哈希抽样门控，消除数万 update 线程的全局单地址原子热点
    if(((uint(pixelCoord.x) * 73856093u ^
         uint(pixelCoord.y) * 19349663u ^
         cam.frameCount * 83492791u) & 255u) == 0u)
    {
        RWStructuredBuffer_uint sharcUpdateCounter =
            RWStructuredBuffer_uint(sharcCacheAddress(3u));

        atomicAdd(sharcUpdateCounter.data[sharcUpdateCommitted ? 8u : 10u], 1u);
    }

    if(!sharcUpdateCommitted)
    {
        break;
    }
}
#endif

// DBG_LOCAL_BOUNCE_ONLY: the NEE term this vertex received, i.e. the
// local-light bounce (see its declaration at the top of the file).
if(DBG_LOCAL_BOUNCE_ONLY)
{
    dbgLocalBounceAccum +=
        currentThroughput *
        bounceLocalDiffuse;
}
// =====================================================
// A3-1
// PBR secondary GI transport
//
// Opaque secondary surface:
//
// dielectric:
//     Diffuse + GGX Specular
//
// metal:
//     GGX Specular only
//
// Water interface 仍由上面的独立路径处理。
// =====================================================

diffuseBounceCount +=
    1;


// 当前已经达到最大 surface bounce，
// 不需要再生成一条永远不会被 trace 的方向。
if(
    diffuseBounceCount >=
    giBounceLimit
)
{
    break;
}


vec3 nextV =
    normalize(
        -currentDir
    );


float nextNdotV =
    max(
        dot(
            nextNormal,
            nextV
        ),
        0.0
    );


if(nextNdotV <= 1.0e-5)
{
    break;
}


// -----------------------------------------------------
// LabPBR F0
// -----------------------------------------------------

vec3 nextF0 =
    decodeBaseF0(
        nextF0MetalCode,
        nextAlbedo
    );


// 用当前 viewing angle 的 Fresnel
// 作为 diffuse/specular mixture 的采样概率依据。
vec3 nextFForChoice =
    fresnelSchlick(
        nextNdotV,
        nextF0
    );


float nextFMax =
    max(
        max(
            nextFForChoice.r,
            nextFForChoice.g
        ),
        nextFForChoice.b
    );


// 金属没有 diffuse lobe，所以必须走 specular。
//
// 非金属根据 Fresnel 选择两种 lobe。
// 0.05 / 0.95 只影响采样概率和方差，
// 后面的 / probability 会补偿能量。
float nextSpecularProbability =
    nextMetallic > 0.5
        ? 1.0
        : clamp(
            nextFMax,
            0.05,
            0.95
        );


// =====================================================
// 2026-11-08 降寄存器档（刀2）：GI 弹射强制漫反射 lobe。
//
// executable_properties 拿不到寄存器读数（驱动不实现），改用结构减负：
// GI 顶点的 GGX D/G/F/PDF 整条 specular 支在这里变成死代码被编译期摘除，
// K2-I/主变体的最坏分支寄存器占用随之下降。能量口径：specProb 置 0 ⇒
// diffuse 概率恒 1，漫反射权重 kD*albedo 不再除以混合概率 = 纯漫反射
// GI 估计（镜面间接由独立 glossy 通道承担，与此处无偏性不冲突）。
// 金属顶点 kD=0 ⇒ 二次弹射贡献为 0（旧档金属走 GGX 镜面弹射，观感差异
// = 金属表面的二次间接反射消失）。
// 回退：-DGI_BOUNCE_DIFFUSE_ONLY=0 重编受影响 rgen。
// =====================================================
#ifndef GI_BOUNCE_DIFFUSE_ONLY
#define GI_BOUNCE_DIFFUSE_ONLY 1
#endif
#if GI_BOUNCE_DIFFUSE_ONLY
nextSpecularProbability = 0.0;
#endif


bool chooseNextSpecular =
    rand(seed) <
    nextSpecularProbability;

#if GI_BOUNCE_DIFFUSE_ONLY
// 结构性死支：rand() 对编译器是不透明函数，`rand()<0.0` 推不出 false，
// 必须显式常量化才能让 GGX D/G/F/PDF 整支被编译期摘除。
// rand(seed) 仍消耗一次，保持随机流与旧档对齐（A/B 噪声图案同源）。
rand(seed);
chooseNextSpecular = false;
#endif


vec3 nextBounceDir =
    vec3(0.0);


vec3 nextBounceWeight =
    vec3(0.0);


// =====================================================
// GGX Specular branch
// 2026-11-08 降寄存器档：GI_BOUNCE_DIFFUSE_ONLY=1 时整支物理移除
// （#if 拿出编译单元，SPIR-V 级即消失，不依赖驱动 DCE）。
// =====================================================

#if !GI_BOUNCE_DIFFUSE_ONLY
if(chooseNextSpecular)
{
    vec3 nextH =
        sampleGGXVNDF(
            nextNormal,
            nextV,
            nextRoughness,
            vec2(rand(seed), rand(seed))
        );


    float nextVdotH =
        max(
            dot(
                nextV,
                nextH
            ),
            0.0
        );


    float nextNdotH =
        max(
            dot(
                nextNormal,
                nextH
            ),
            0.0
        );


    nextBounceDir =
        normalize(
            reflect(
                -nextV,
                nextH
            )
        );


    float nextNdotL =
        max(
            dot(
                nextNormal,
                nextBounceDir
            ),
            0.0
        );


    if(
        nextVdotH <= 1.0e-5 ||
        nextNdotH <= 1.0e-5 ||
        nextNdotL <= 1.0e-5
    )
    {
        break;
    }


    vec3 nextF =
        fresnelSchlick(
            nextVdotH,
            nextF0
        );


    float nextG =
        geometrySmith(
            nextNormal,
            nextV,
            nextBounceDir,
            nextRoughness
        );


    float nextG1V =
        ggxVNDFMaskingG1(
            nextNdotV,
            nextRoughness
        );


    nextBounceWeight =
        (
            nextF *
            (nextG / max(nextG1V, 1.0e-5))
        ) /
        max(
            nextSpecularProbability,
            1.0e-5
        );
}
#endif // !GI_BOUNCE_DIFFUSE_ONLY
#if GI_BOUNCE_DIFFUSE_ONLY
#else
else
#endif


// =====================================================
// Diffuse branch
// =====================================================

{
    float nextDiffuseProbability =
        max(
            1.0 -
            nextSpecularProbability,
            1.0e-5
        );


    vec3 nextKD =
        (
            vec3(1.0) -
            nextFForChoice
        ) *
        (
            1.0 -
            nextMetallic
        );


    float g2bSecondaryMixtureWeight =
        1.0;


    // LL-P3.2 (route C): this is the vertex that DOES guide.  The ray leaving
    // the first hit surface is aimed at a local light, so an unoccluded sample
    // lands on the emitter and nextUnifiedEmission becomes the local-light
    // bounce (emitter -> this surface -> receiver).  Only reachable when
    // "GI 跳数" >= 2.
    g2bSampleDiffuseDirectionMIS(
        nextHitPos,
        nextNormal,
        seed,
        nextBounceDir,
        g2bSecondaryMixtureWeight,
        true
    );


    // Cosine hemisphere:
    //
    // BRDF = kD * albedo / PI
    // PDF  = NdotL / PI
    //
    // BRDF * NdotL / PDF
    // => kD * albedo
    nextBounceWeight =
        (
            nextKD *
            nextAlbedo *
            g2bSecondaryMixtureWeight
        ) /
        nextDiffuseProbability;
}


// =====================================================
// 保留当前已经稳定的 GI 总体能量尺度 0.7。
//
// A3-1 只验证 BRDF transport。
// 暂时不同时改变整体曝光 / GI 强度。
// =====================================================


currentThroughput *=
    nextBounceWeight *
    BSDF_GI_THROUGHPUT_SCALE;

#if SHARC_UPDATE
// 参考 Pathtracer.hlsl:738-744：采样完新方向、乘完 BRDF 权重**之后**，把段权重交给
// SharcSetThroughput（它会把该权重乘进所有已记录顶点的 sampleWeights —— 见
// SharcCommon.h:716-722）。本工程不重置 currentThroughput，所以这里传"增量"
// nextBounceWeight * BSDF_GI_THROUGHPUT_SCALE，与参考等价（参考在 update pass 每 bounce 把 throughput
// 重置为 1：Pathtracer.hlsl:461-463）。
SharcSetThroughput(sharcState, nextBounceWeight * BSDF_GI_THROUGHPUT_SCALE);
#endif

#if !SHARC_UPDATE
#ifndef GI_DEEP_RUSSIAN_ROULETTE
#define GI_DEEP_RUSSIAN_ROULETTE 1
#endif
#if GI_DEEP_RUSSIAN_ROULETTE
if(diffuseBounceCount >= 2)
{
    const float rrSurvive = clamp(
        max(currentThroughput.r, max(currentThroughput.g, currentThroughput.b)),
        0.05,
        0.95
    );
    const float rrU = float(pcg_hash(seed) & 0x00ffffffu) * (1.0 / 16777216.0);
    if(rrU > rrSurvive)
    {
        break;
    }
    currentThroughput /= rrSurvive;
}
#endif
#endif

// 参考 Pathtracer.hlsl:673-675：把**本顶点**的粗糙度累加进 materialRoughnessPrev，
// 供下一跳算光锥足迹用（漫反射顶点记 1.0，镜面顶点记其粗糙度）。
// 原句：materialRoughnessPrev += brdfType == DIFFUSE_TYPE ? 1.0f : material.roughness;
materialRoughnessPrev += chooseNextSpecular ? nextRoughness : 1.0;


currentOrigin =
    nextHitPos +
    nextNormal *
    0.001;


currentDir =
    nextBounceDir;


// 结束 opaque-hit else
}

// 结束 GI while
}
#else
if(runDiffuseGI)
{
    indirectLight = imageLoad(k2GiTraceImage, pixelCoord).rgb;
}
#endif
}   // 结束 2026-10-24 的 giHalfParams.x 运行时分支（起点见 diffuseGILodWeight 之后）
#if defined(ORION_K2_INDIRECT_TRACE_ONLY)
k2iGiTraceAccum += indirectLight;
k2iGiTraceAny = true;
continue;
#elif (defined(ORION_K2_INDIRECT_STAGE) || defined(ORION_K2_DIRECT_LOCAL_STAGE)) && !SHARC_UPDATE && !defined(ORION_K2_INDIRECT_COMPOSE_ONLY)
if(k2iCompactTraceActive)
{
    imageStore(
        k2GiTraceImage,
        pixelCoord,
        vec4(min(indirectLight, vec3(60000.0)), 1.0)
    );
    return;
}
#endif
#if SHARC_UPDATE
continue;
#endif
#if !defined(ORION_K2_INDIRECT_TRACE_ONLY) && !SHARC_UPDATE

#if GI_HALFRES
// ============================================================================
// 2026-10-24 半分辨率 GI 变体：写 giHalfImage 后**立即 return**。
// · rgb = 解调后的辐照度（÷brdfWeight）：全分辨率主变体乘回**自己的** brdfWeight，
//   于是半分辨率的材质权重不会被当成全分辨率的（2×2 内材质跳变不会直接漏光）。
//   下限 1e-3 只是防近黑表面把 16F 撑爆，量级上仍是"除以 brdfWeight"。
// · a = 主命中视深度（米）：与主变体用**同一公式**算，两次深度可直接相减。
// · 挂点在 `indirectLight *= diffuseGILodWeight` **之前**：远场 LOD 折扣是显示侧
//   近似，留在主变体按全分辨率像素自己乘（HANDOFF §6.5）。
// · ⚠ 必须在这里 return：24156 起的 RR guide 写入由主变体独占（§6.6）。
// · perfRayStatAdd 的口径在这里变成"半分辨率射线"（约 1/4），统计会失真（§6.8）。
// ============================================================================
{
    const float giHalfViewDepth =
        -(viewMatrix * vec4(hitPos, 1.0)).z;

    imageStore(
        giHalfImage,
        ivec2(gl_LaunchIDEXT.xy),
        vec4(
            // ⚠ 2026-10-25 实机首跑（满屏萤火虫）后**取消解调**：这里原来是
            //   indirectLight / max(brdfWeight, 1e-3)，主变体再乘回自己的 brdfWeight。
            // 机理：brdfWeight = (1-F)(1-metallic)·albedo·g2bPrimaryMixtureWeight，
            // 同一个 2×2 块内 4 个像素共享几乎同一份入射辐亮度，但 albedo 与 RIS 混合
            // 权重逐像素独立随机 ⇒ brdfWeight_p / brdfWeight_i 在同一平面上就能到
            // 1e3~1e4，且相位轮转让每个像素每帧读到不同邻居 ⇒ 逐像素随机放大 ⇒
            // 满屏萤火虫（RR 的 anti-firefly 只夹孤立亮点，夹不掉这个）。
            // 现在直接存 L：2×2 块内平均入射辐亮度正是本方案的原意，代价只有反照率
            // 边界轻微串色（有界 ~10 倍，不是指数）。
            // min() 仍保留，纯粹防 16F 溢出成 Inf。
            min(indirectLight, vec3(60000.0)),
            giHalfViewDepth
        )
    );
}
return;
#else
// giHalfParams.x != 0：间接光直接取半分辨率 GI 图的 3×3 深度感知平均。
// 远场尾部（下面的 diffuseGILodWeight / farSlopeOcclusion / 环境底）照旧乘在它上面。
// ⚠ 2026-10-25：这里**不再**乘回 brdfWeight（与半图侧的取消解调配对）。
if (cam.giHalfParams.x != 0.0f)
{
    indirectLight =
        upsampleGiHalf(
            pixelCoord,
            -(viewMatrix * vec4(hitPos, 1.0)).z);
}
#endif

// ============================================================================
// 2026-11-04 A1：GI 时域重投影（rgen_gi_temporal.glsl）。
//   开着：resolved 取代 traced，diffuseGILodWeight（纯速率补偿）置 1 ——
//   补偿职责由历史接管。关着：逐位旧行为。
//   SHARC_UPDATE / GI_HALFRES 变体不进这里（helper 已按宏整体摘除）。
// ============================================================================
#if !SHARC_UPDATE && !GI_HALFRES
if(giTemporalEnabled())
{
    indirectLight = giTemporalResolveAndStore(
        pixelCoord,
        giTracedThisFrame || cam.giHalfParams.x != 0.0f,
        primaryHitDistance >= 0.0,
        rtDebugPrimaryMotion,
        -(viewMatrix * vec4(hitPos, 1.0)).z,
        indirectLight,
        diffuseGILodWeight);

    diffuseGILodWeight = 1.0;
}
else
#endif
indirectLight *=
    diffuseGILodWeight;

// =====================================================
// 远场坡度遮蔽（2026-10-01）
//
// 远场的中尺度明暗几乎全靠"太阳直射 vs 全黑"两态：RTAO 半径 5.5 格、且在
// 160 格外被整体关闭（aoSamples = 0 ⇒ rtaoFactor 恒 1.0），GI 在 384 格外被
// diffuseGILodWeight 压到 0.03125。于是朝光的坡面与背光的坡面在远场里几乎
// 一样亮 —— 这正是"远处阴影很淡/泛白"的观感来源之一。
//
// 这里按法线的竖直分量补一层大尺度遮蔽：朝上的面基本不遮，垂直/朝下的面压到
// SUN_FAR_AO_FLOOR。它不是物理 AO（不参与遮挡计算、不影响直射），只是一层
// 让远场重新有"体积感"的朝向加权。仅在远场档生效，近场完全不变。
// =====================================================
// 独立门控（2026-10-01）。
//
// ⚠ 不再复用 farShadowActive：那个布尔量绑着 sunShadowFarParams.w，而远场阴影图
//   已经清 0 停用（ORION_SUNSHADOW_FAR=0），若还挂在它上面，这套**唯一**针对
//   "远场泛白"的改动会跟着一起失效。远场坡度遮蔽与阴影图无关，只按距离判定。
bool farSlopeOcclusionActive =
    rtLodDistance >= 160.0 * perfL10TierScale();

float farSlopeOcclusion =
    1.0;

if(farSlopeOcclusionActive)
{
    const float SUN_FAR_AO_FLOOR =
        0.45;

    farSlopeOcclusion =
        mix(
            SUN_FAR_AO_FLOOR,
            1.0,
            smoothstep(
                0.0,
                0.6,
                normal.y
            )
        );
}

// =====================================================
// 远场环境底跟随太阳（2026-10-01）
//
// farSlopeOcclusion 只按**法线朝向**压，平地上 normal.y ≈ 1 ⇒ 系数恒 1，什么
// 都不压 —— 而"远处泛白"最显眼的恰恰是平坦地面。真正缺的是间接光的遮蔽信息：
// RTAO 在 160 格外整体关闭（aoSamples = 0），GI 在 384 格外被压到 0.03125，
// 于是背光地面和朝光地面在远场几乎一样亮。
//
// 这里复用**已经打出去的那条**太阳可见性射线（shadowSamples 对 rtLodDistance
// ≥ 16 恒 1，见上方"远场档标记"）：可见 = 1，被挡 = 0。**零新增射线**，每像素
// 只多一个 dot + 一个 mix。效果是远场平坦的背光地面也会明显变暗，重新有明暗分区。
//
// SUN_FAR_SHADE_AMBIENT 是背光侧环境底的下限，防止压成死黑；不做成 0 是因为
// 远场只有一个样本（二值），到底会带一点硬边，留下限既压暗又不至于抖。
// =====================================================
float farAmbientSunTerm =
    1.0;

if(farSlopeOcclusionActive)
{
    const float SUN_FAR_SHADE_AMBIENT =
        0.55;

    float sunVisibility =
        dot(
            shadowTransmittance,
            vec3(1.0 / 3.0)
        );

    farAmbientSunTerm =
        mix(
            SUN_FAR_SHADE_AMBIENT,
            1.0,
            clamp(
                sunVisibility,
                0.0,
                1.0
            )
        );
}


// 2026-10-27 稀疏渲染的无偏缩放（Remix 的 1/rate，逐行照搬）。
// 施加范围 = 由稀疏射线产生的项（**能量项**）：
//   本地光 RIS / 二级 DDA / glossy / GI。
// 2026-11-02 起**不再包含太阳直射**：阴影（可见性项）已退出稀疏，满采样，
// 见上面 RTAO 那段的长注释（跳过可见性会把漏光放大成发光，草上最明显）。
// 不施加：
//   localExactFaceDirect（编译期恒 false 的面缓存直读）、
//   waterReflectedSunLight（按帧间隔采样，不受 vptPixelActive 门控）、
//   以及 litColor 之外的 emission / localEmitterEmission / ambientLight。
// Remix 的挂点是 demodulate 的 per-lobe radiance（demodulate.comp.slang:649-659 /
// 773-781），语义相同：只缩放被稀疏化的辐射度。
// scale == 1（稀疏关）时多乘一个 1.0，结果逐位不变。
// ⚠ 必须与 vptPixelActive 的变体门控一致（rt_test.rgen:21024 的
// `#if !SHARC_UPDATE && !GI_HALFRES`）：这两个变体**不做稀疏**，每像素全采样，
// 乘 1/rate 会让他们过亮 1/rate 倍 —— SHARC_UPDATE 会把过亮值写进 world-space
// 缓存，GI_HALFRES 会把过亮的 giHalfImage 上采样进主 pass。
#if !SHARC_UPDATE && !GI_HALFRES
const float sparseRadianceScale = vptSparseRadianceScale();
#else
const float sparseRadianceScale = 1.0;
#endif

// =====================================================
// 2026-11-02 稀疏项的 firefly 钳制（**必须在 ×1/rate 之前**）。
//
// 依据 RTX Remix：
//   src/dxvk/shaders/rtx/pass/demodulate/demodulate.comp.slang:770-781
//   "The sparse estimate is scaled by the inverse sampling rate after the clamps
//    above, so that the clamps act on the traced radiance."
// 以及 :649-659 的同类注释（primary 信号同样是 clamp 之后才 scale）。
//
// 之前本工程是"先求和 → 最后统一乘 1/rate"，2.5 倍把每个 outlier 一起放大、
// 再交给 RR ⇒ 静止时满屏散不掉的亮点。这里先把**稀疏项之和**（未缩放）算出来，
// 按其亮度给一个**统一标量增益**，再由所有稀疏项共用 —— 均匀缩放保持色度，
// 也保证下面 SS_GBUFFER_STORE 的分项与 litColor 仍是同一个线性关系
//（差分替换口径不漂移，见下方"必须与紧邻上面的 litColor 表达式逐字同源"）。
//
// 稀疏关闭（scale == 1）时 gain 恒 1 ⇒ 逐位回到改动前。
// A/B：-DVPT_SPARSE_PRESCALE_CLAMP=0 关闭。
// =====================================================
#ifndef VPT_SPARSE_PRESCALE_CLAMP
#define VPT_SPARSE_PRESCALE_CLAMP 1
#endif
#ifndef VPT_DIRECT_PRESCALE_KNEE
#define VPT_DIRECT_PRESCALE_KNEE 24.0
#endif
#ifndef VPT_DIRECT_PRESCALE_SOFT_CEIL
#define VPT_DIRECT_PRESCALE_SOFT_CEIL 64.0
#endif
#ifndef VPT_INDIRECT_PRESCALE_KNEE
#define VPT_INDIRECT_PRESCALE_KNEE 4.0
#endif
#ifndef VPT_INDIRECT_PRESCALE_SOFT_CEIL
#define VPT_INDIRECT_PRESCALE_SOFT_CEIL 12.0
#endif

const vec3 sparseDirectSumUnscaled =
#if VPT_SPARSE_SKIP_VISIBILITY
    directLight +
#endif
    localDirectLight +
    localSecondaryLight;

const float primarySpecularOcclusion = computeSpecularOcclusion(
    max(dot(normal, V), 0.0),
    rtaoFactor,
    roughness
);

const vec3 sparseIndirectSumUnscaled =
    // 2026-10-05 微结构反射遮蔽（#44）：凹槽 texel 的间接反射被凸起遮挡，
    // 按静态深度遮蔽折减（#12 的 GI 扣减同源）。
    glossyReflectionLight * primaryPOMMicroOcclusion * primarySpecularOcclusion +
    indirectLight *
    1.8 *
    rtaoFactor *
    farSlopeOcclusion *
    farAmbientSunTerm *
    primaryPOMMicroOcclusion;

float directPreScaleGain = 1.0;
float indirectPreScaleGain = 1.0;
#if VPT_SPARSE_PRESCALE_CLAMP
if(sparseRadianceScale > 1.0)
{
    const float directSumLuma = dot(
        max(sparseDirectSumUnscaled, vec3(0.0)),
        vec3(0.2126, 0.7152, 0.0722));
    const float directKnee = float(VPT_DIRECT_PRESCALE_KNEE);
    if(directSumLuma > directKnee)
    {
        const float headroom = max(float(VPT_DIRECT_PRESCALE_SOFT_CEIL) - directKnee, 0.5);
        const float excess = directSumLuma - directKnee;
        const float compressedLuma = directKnee + headroom * (excess / (excess + headroom));
        directPreScaleGain = compressedLuma / directSumLuma;
    }

    const float indirectSumLuma = dot(
        max(sparseIndirectSumUnscaled, vec3(0.0)),
        vec3(0.2126, 0.7152, 0.0722));
    const float indirectKnee = float(VPT_INDIRECT_PRESCALE_KNEE);
    if(indirectSumLuma > indirectKnee)
    {
        const float headroom = max(float(VPT_INDIRECT_PRESCALE_SOFT_CEIL) - indirectKnee, 0.5);
        const float excess = indirectSumLuma - indirectKnee;
        const float compressedLuma = indirectKnee + headroom * (excess / (excess + headroom));
        indirectPreScaleGain = compressedLuma / indirectSumLuma;
    }
}
#endif

const float sparseDirectTermScale = sparseRadianceScale * directPreScaleGain;
const float sparseIndirectTermScale = sparseRadianceScale * indirectPreScaleGain;

// 太阳直射的补偿取决于阴影是否参与稀疏：
//   SKIP_VISIBILITY=1（非活跃像素跳阴影）⇒ 该项被稀疏，必须乘 1/rate；
//   SKIP_VISIBILITY=0（阴影满采样）      ⇒ 每帧都是有效值，不乘。
#if VPT_SPARSE_SKIP_VISIBILITY
const float sparseSunScale = sparseDirectTermScale;
#else
const float sparseSunScale = 1.0;
#endif

// 反射项的 1/rate 补偿：只有它真的被稀疏（VPT_SPARSE_GLOSSY=1）时才该乘。
// 默认（0）反射逐像素满采样，乘了就是整屏镜面偏亮 1/rate 倍。
#if VPT_SPARSE_GLOSSY
const float sparseGlossyScale = sparseIndirectTermScale;
#else
const float sparseGlossyScale = 1.0;
#endif

vec3 litColor =
    directLight * sparseSunScale +
    localDirectLight * sparseDirectTermScale +
    localExactFaceDirect +
    localSecondaryLight * sparseDirectTermScale +
    waterReflectedSunLight +
    glossyReflectionLight * primaryPOMMicroOcclusion * primarySpecularOcclusion * sparseGlossyScale +
    indirectLight *
    1.8 *
    rtaoFactor *
    farSlopeOcclusion *
    farAmbientSunTerm *
    primaryPOMMicroOcclusion *
    sparseIndirectTermScale;

// ============================================================================
// SS_GBUFFER_STORE：2026-11-02 分项 G-buffer（binding 81 / 82）
//
// 这两条 store 必须与紧邻上面的 litColor 表达式**逐字同源**：屏幕空间 pass
// 用 "color - 这一项 + 屏幕空间那一项" 做差分替换，两边口径一旦漂移就会
// 整屏偏亮/偏暗。改 litColor 里间接/镜面两项时，这里必须同步改。
//
// · 不挂 perfL10DisableG2bLocalGuide：那档消融是为了给"guide 选择性 store"
//   定价，而屏幕空间 pass 一旦读到陈旧的分项就会把错误的值当成"要减掉的量"
//   ⇒ 画面被减黑，不属于可以消融的范围。
// · GI_HALFRES 变体在 26748 已 return；SHARC_UPDATE 的写入会被随后的渲染
//   pass 逐像素覆写（见 20697 的注释），所以两者都不需要额外门控。
//
// ⚠ 2026-11-02 修正：**必须挂 ssHybridParams 门控**。
//   这两张分项图（binding 81/82）只在 SSPT/SSR 开关 > 0 时创建
//   （State.cpp 的 g_ssptChunks / g_ssrChunks 默认 0 ⇒ 不建资源、不 dispatch），
//   而 ScreenSpace.cpp 的 updateSSHybridDescriptors() 在 view 还是
//   VK_NULL_HANDLE 时直接 return ⇒ binding 81/82 **从未被绑定过**。
//   无条件 imageStore 就是往未绑定的 storage image 描述符上写，属于未定义行为，
//   实测表现为进世界第 1 帧 VK_ERROR_DEVICE_LOST(-4)，直到下一处几何 fence
//   才报成 "submit Section BLAS/TLAS build failed" —— 崩点看似在 BLAS，
//   实际根因在这里。开关为 0 时没人读这两张图（ss pass 也不跑），门控无损。
// ============================================================================
if(cam.ssHybridParams.x > 0.5 || cam.ssHybridParams.y > 0.5)
{
    const vec3 ssIndirectTerm =
        indirectLight *
        1.8 *
        rtaoFactor *
        farSlopeOcclusion *
        farAmbientSunTerm *
        primaryPOMMicroOcclusion *
        sparseIndirectTermScale;

    const vec3 ssSpecularTerm =
        glossyReflectionLight *
        primaryPOMMicroOcclusion *
        primarySpecularOcclusion *
        sparseGlossyScale;

    imageStore(
        ssIndirectTermImage,
        pixelCoord,
        vec4(
            min(ssIndirectTerm, vec3(60000.0)),
            min(dot(indirectLight, vec3(1.0 / 3.0)), 60000.0)
        )
    );

    // 2026-11-03：a 通道改存**环境底辐亮度**（原来存的镜面权重没人消费）。
    // 唯一消费者是 ss_hybrid.comp 的 SSPT：它的射线出屏时找不到任何东西，
    // 必须退回一个"已知的光"，否则室内/贴墙处是黑洞。环境底是这里唯一
    // 物理上已知的量（用户可调，不乘自造系数）。
    imageStore(
        ssSpecularTermImage,
        pixelCoord,
        vec4(
            min(ssSpecularTerm, vec3(60000.0)),
            min(dot(getSurfaceAmbientRadiance(), vec3(1.0 / 3.0)), 60000.0)
        )
    );
}

// 2026-10-14 **临时诊断**（标记文件 surface_terms.flag ⇒ 车道 perfAblationStage.w = 1；
// 把三类表面光照分别编码成 RGB，用来**一次运行**定位"洞里被日落色照亮"来自哪一类：
//   R = 太阳直射（含 shadowTransmittance，即已含真射线的太阳可见性）
//   G = 局部/体素光（localDirectLight / localExactFaceDirect / localSecondaryLight）
//   B = 间接 + 镜面（indirectLight… + glossyReflectionLight…，后者与 POM 相乘）
// 判读：进洞站立，看洞里那片偏哪个通道 —— 偏哪个就是哪一类在漏。
// 说明：这里只覆盖 litColor，后面的"环境底"等项仍会照常相加（你的 Ambient=0.01
// 接近零，不影响判读）；×4 是为了在 tonemap 之后还看得清。
if(cam.perfAblationStage.w == 1u)
{
    litColor =
        vec3(
            dot(directLight, vec3(1.0 / 3.0)),
            dot(
                localDirectLight +
                    localExactFaceDirect +
                    localSecondaryLight,
                vec3(1.0 / 3.0)
            ),
            dot(
                waterReflectedSunLight +
                    glossyReflectionLight * primaryPOMMicroOcclusion * primarySpecularOcclusion +
                    indirectLight *
                        1.8 *
                        rtaoFactor *
                        farSlopeOcclusion *
                        farAmbientSunTerm *
                        primaryPOMMicroOcclusion,
                vec3(1.0 / 3.0)
            )
        ) *
        4.0;
}
// 2026-10-14 **临时诊断**（lane 2 / 标记文件 surface_shadowvis.flag）：
// 直接显示**太阳影透过率** `shadowTransmittance` 本身 —— 这是"洞里为什么偏红"的
// 最直接判据（`directLight` 的亮度正比于它）：
//   R = 透过率（1 = 那条射线报告"什么都没打到" ⇒ 太阳照进来）
//   G = 1 - 透过率（1 = 被几何挡住）
// 洞里应当是**绿**。若整片红 ⇒ 洞壁的太阳影射线打空了（几何/掩码/flag 三者之一）。
else if(cam.perfAblationStage.w == 2u)
{
    float shadowVis = clamp(dot(shadowTransmittance, vec3(1.0 / 3.0)), 0.0, 1.0);
    litColor = vec3(shadowVis, 1.0 - shadowVis, 0.0) * 4.0;
}

// 2026-10-14 **临时诊断**（lane 3 / 标记文件 surface_pom.flag）：视差浮雕的自阴影链。
//
//   为什么看这两个数：`directLight` 的太阳项在 19711 处被乘上 `primaryPOMSelfShadow`
//   （POM 自遮蔽），再由 19715 的 `shadowVisibility` 开门。几何遮挡那半已经量过
//   （lane 2：洞里绿 = 正常），所以浮雕"被点亮"的嫌疑只剩这个自遮蔽因子。
//
//   R = primaryPOMSelfShadow（0 = 浮雕被自遮蔽 ⇒ 正常；1 = 毫无自遮蔽 ⇒ 被压平点亮）
//   G = 视差命中深度（越亮 = 位移越深、POM 在生效；越暗 = depth01≈0 ⇒ 位移没起作用）
//
// 判读：
//   · R 亮 + G 亮 ⇒ 位移在生效，是高度场 march 没产出遮蔽（问题在 tracePOMSelfShadow）
//   · R 亮 + G 暗 ⇒ depth01≈0 ⇒ 撞上了 1493 的早退（位移本身没量到）
else if(cam.perfAblationStage.w == 3u)
{
    litColor = vec3(clamp(primaryPOMSelfShadow, 0.0, 1.0),
                    1.0 - clamp(primaryPOMMicroOcclusion, 0.0, 1.0), 0.0) * 4.0;
}

// 2026-10-14 **临时诊断**（lane 4 / 标记文件 surface_relief.flag）：浮雕法线细节。
//
// 前面两档把"遮挡"这一侧排干净了：几何影（lane 2）与浮雕自遮蔽（lane 3，日落朝阳面
// R 暗 = 正常）都对。所以"浮雕被点亮"若还成立，就只剩"着色用了哪条法线"：
//   · 用了法线贴图/POM 的细节法线 ⇒ 浮雕有凹凸明暗（正常）
//   · 只用了几何面法线 ⇒ NdotL 在整面方块上是常数 ⇒ **整面被均匀照亮 = 被压平点亮**
//
// 显示：
//   R = 阴影子法线与几何面法线的偏离（越亮 = 细节法线越强；全黑 = 只用了几何法线）
//   G = NdotL（直射项的几何形状：朝阳面亮）
//   B = 0
// 判读：浮雕处 R 全黑 ⇒ 细节法线没进着色（直射项被压平）；R 有花纹且与浮雕吻合 ⇒ 法线正常，
//       那"被点亮"就落在亮度/染色上，我再拆 sunRadiance 的三个因子。
else if(cam.perfAblationStage.w == 4u)
{
    float normalDeviation =
        clamp(length(vec3(normal) - vec3(primaryGeometricNormal)) * 2.0, 0.0, 1.0);
    litColor = vec3(normalDeviation, clamp(NdotL, 0.0, 1.0), 0.0) * 4.0;
}

// 判决实验 LL-P3.2：只显示第二顶点的发光体命中（本地光弹射）。
if(DBG_LOCAL_BOUNCE_ONLY)
{
    litColor =
        dbgLocalBounceAccum *
        diffuseGILodWeight *
        1.8 *
        rtaoFactor;
}


// =====================================================
// RTX setting: Ambient base brightness


// =====================================================
// RTX setting: Ambient base brightness
//
// Final lighting =
// existing lighting + albedo * ambient base brightness
//
// This is independent of:
// - direct sunlight visibility
// - GI
// - glossy reflection
// - water reflection
// - shadow state
// =====================================================

// The user-controlled value remains the dusk/overcast baseline. At high sun
// it is reduced so an occluded surface is visibly separated from one lit by
// the direct shadow ray instead of reading as uniformly cloudy.
//
// ⚠ 公式已抽到 getSurfaceAmbientBase()，DH 远场点灯用的是**同一个函数**。
//    要改就改那一个地方（改这里等于同时改远场，这正是要的）。
// 2026-10-09：改用 getSurfaceAmbientRadiance()（vec3）—— 它在末地额外加上银河
// 环境底光；其余维度逐位等于 getSurfaceAmbientBase()。
vec3 ambientRadiance = getSurfaceAmbientRadiance();


#ifndef SSPT_AMBIENT_AO_STRENGTH
// 2026-11-03（用户要求："让仅 SSPT + RTAO 做出光追的感觉"）
//
// 环境底现在也吃 RTAO。此前 rtaoFactor **只**乘间接光项
// （litColor 里 `indirectLight * 1.8 * rtaoFactor * ...`），而「GI 跳数 = Off」
// 之后 indirectLight = 0 ⇒ rtaoFactor × 0 = 0，AO 视觉上完全消失，关掉 GI
// 就只剩"平的直射光"，这正是"不像光追"的最大单项。
//
// 把 AO 压到环境底上，接触暗部（贴地 / 缝隙 / 凹陷 / 背光交界）才回来
// —— 这也是光追截图里最容易辨认的那一项，而且它**不花任何射线**
// （RTAO 本来就在跑）。
//
// 取值：1.0 = 全压（等价于标准的 sky ambient × AO）；0 = 回到旧行为。
// 想更淡就调小这个宏，不需要动别处。
#define SSPT_AMBIENT_AO_STRENGTH 1.0
#endif

vec3 ambientLight =
    albedo *
    ambientRadiance *
    mix(1.0, rtaoFactor, SSPT_AMBIENT_AO_STRENGTH) *
    farSlopeOcclusion *
    farAmbientSunTerm *
    primaryPOMMicroOcclusion;


sampleColor =
    emission +
    localEmitterEmission +
    litColor +
    ambientLight;
#if DBG_LOCAL_TERM_MAP_MACRO
        if(DBG_LOCAL_TERM_MAP)
        {
            // 第二轮通道组（含义见开关处说明）。R 取 .r 是因为 applyLocalGlassTint
            // 在表内已经把 RIS 直射折成了 (luma,0,0)。
            sampleColor =
                vec3(dbgTermLuma(emission)) +
                vec3(
                    localDirectLight.r,
                    dbgTermLuma(localEmitterEmission),
                    dbgTermLuma(indirectLight)
                );
#ifdef DBG_TERMMAP_B_SECTION
            // 临时诊断（用完删除）：B 通道 = Section 网格判据，同屏读两个量。
            //
            // 前两轮已确认：台阶只在 R(localDirectLight) 上；G(localEmitterEmission)
            // 跨边完全平；B(albedo) 只动 8~11% 而 R 动 93% ⇒ 与接收面反照率、发射体
            // 邻接自发光都无关。用户观察它「钉在世界上不动」（不跟相机走 ⇒ 排除
            // keepProbability / vptLocalLightRangeFade / rtLodDistance 这一族相机
            // 距离门）且「穿过多个表面且连续」（⇒ 排除材质/法线等表面属性，指向空间
            // 里一张与世界坐标对齐的固定平面）。
            //
            // 剩下最强嫌疑是 16 格 **Section 网格**：接收面跨边界 ⇒ receiverPage
            // 换页 ⇒ Binding 31 来源目录的 ±2 邻域整体平移、别名表换页、可提议发射体
            // 集合离散跳变（rt_test.rgen 2396-2425 / VulkanCore.cpp
            // rebuildVPTSourceDirectoryPageLocked）。
            //
            //   背景亮度 = 接收面所属 Section 的来源目录条目数 / 16（截断到 1；
            //              查不到页的 Section 记为 0，即全黑）
            //   细亮线   = 命中点落在 16 格 Section 边界上（三轴取最近者）
            //
            // 判读：
            //   · 台阶正好压在细亮线上 ⇒ 成因是 Section 网格
            //   · B 背景亮度在台阶两侧也跳 ⇒ 目录条目数确实离散变化
            //   · 两者都不成立 ⇒ 与 Section 网格无关，退到法线/余弦支再编探针
            ivec3 dbgSectionCoord = ivec3(floor(hitPos / 16.0));
            uint dbgSectionPage = 0u;
            float dbgSectionCountNorm = 0.0;
            if(vliLookupSectionPage(dbgSectionCoord, dbgSectionPage))
            {
                uint dbgSectionCount = min(
                    vptSourceDirectoryPagesBuffer.entries[
                        dbgSectionPage * VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE
                    ].pageIndex,
                    VPT_SOURCE_DIRECTORY_MAX_SECTIONS
                );
                dbgSectionCountNorm = min(float(dbgSectionCount) / 16.0, 1.0);
            }
            vec3 dbgSectionFrac = fract(hitPos / 16.0);
            vec3 dbgSectionDist = min(dbgSectionFrac, vec3(1.0) - dbgSectionFrac);
            float dbgSectionLine = 1.0 - smoothstep(
                0.0,
                0.0025,
                min(dbgSectionDist.x, min(dbgSectionDist.y, dbgSectionDist.z))
            );
            sampleColor =
                vec3(dbgTermLuma(emission)) +
                vec3(
                    localDirectLight.r,
                    dbgTermLuma(localEmitterEmission),
                    max(dbgSectionLine, dbgSectionCountNorm * 0.5)
                );
#endif
        }
#else
        // 第一轮通道组（已证不足以定位：R 是死通道；G/B 抽取正确、实测为 0）
        if(DBG_LOCAL_TERM_MAP)
        {
            sampleColor =
                vec3(dbgTermLuma(emission)) +
                vec3(
                    localExactFaceDirect.x,
                    localSecondaryLight.y,
                    localEmitterEmission.z
                );
        }
#endif
#endif // !ORION_K2_INDIRECT_TRACE_ONLY && !SHARC_UPDATE
#endif
#endif // ORION_K2_DIRECT_STAGE（③：GI/稀疏合成/SS 分项归 K2-I，K2-D 不编译）
// ④ K1 不透明管线摘除段结束。
            }
        }

// ④ aerial/累积段归 K2-I；K2-D/K2-SUN/K2-LOC/K2I-TRACE/SHARC 不编译。
#if !defined(ORION_K2_DIRECT_STAGE) && !defined(ORION_K2_DIRECT_SUN_STAGE) \
    && !defined(ORION_K2_DIRECT_LOCAL_STAGE) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_A) && !defined(ORION_K2_DIRECT_SUN_STAGE_B) \
    && !defined(ORION_K2_INDIRECT_TRACE_ONLY) && !SHARC_UPDATE
        // Apply the camera-to-first-visible-surface segment once.  Misses
        // already contain the physical sky from rmiss, and applying this
        // after all water/glass composition prevents double-fogging their
        // internal transport paths.
        //
        // 相机在水里时**整段**相机→表面都泡在水介质里，没有空气可加：这一句按
        // "整段都是空气"补一次散射和透过率，之后又被 applyUnderwaterWaterColumn
        // 的水柱再衰减一次 ⇒ 双层雾，多出来的那层还是天蓝色（空气散射取的是天空
        // 辐亮度）。水下的介质全部交给水柱那一个函数，这里整段跳过。
        // （远场那条 DH 分支不用改：那里的水柱长度会退化成
        // UNDERWATER_DEEP_COLUMN，水色吸收把 DH 的颜色连同它带的空气雾一起压没了。）
        if(primaryHitDistance > 0.0 && cam.waterSimState.w == 0u)
        {
            sampleColor = applyClearSkyAerialPerspective(
                sampleColor,
                origin.xyz,
                worldDir.xyz,
                primaryHitDistance
            );
        }

        accumulatedSampleColor += sampleColor;
#endif // ORION_K2_DIRECT_STAGE（③：aerial/累积归 K2-I，K2-D 每像素都提前 continue）
    }
#ifdef ORION_K2_INDIRECT_TRACE_ONLY
    if(k2iGiTraceAny)
    {
        imageStore(
            k2GiTraceImage,
            pixelCoord,
            vec4(min(k2iGiTraceAccum / float(SAMPLES_PER_FRAME), vec3(60000.0)), 1.0)
        );
    }
    return;
#endif
#if SHARC_UPDATE
    return;
#endif

// ④ 公共尾段（compose/guide/输出）归 K2-I；K2-D/K2-SUN/K2-LOC/K2I-TRACE/SHARC 不编译。
#if !defined(ORION_K2_DIRECT_STAGE) && !defined(ORION_K2_DIRECT_SUN_STAGE) \
    && !defined(ORION_K2_DIRECT_LOCAL_STAGE) \
    && !defined(ORION_K2_DIRECT_SUN_STAGE_A) && !defined(ORION_K2_DIRECT_SUN_STAGE_B) \
    && !defined(ORION_K2_INDIRECT_TRACE_ONLY) && !SHARC_UPDATE
    // ==========================================
    // Perf-1.1
    // Custom Temporal removed from RayGen.
    // DLSS Ray Reconstruction receives current noisy linear HDR.
    // ==========================================
    vec3 finalColor =
        accumulatedSampleColor /
        float(SAMPLES_PER_FRAME);

    float sampleLuminance =
        dot(
            finalColor,
            vec3(0.2126, 0.7152, 0.0722)
        );

    if(sampleLuminance > 6.0)
    {
        finalColor *=
            6.0 / sampleLuminance;
    }

    vec3 currentColor =
        finalColor;

// The early primary-depth write describes the first hit, which is correct for
// opaque terrain.  A transmitted receiver overrides it here so RR reprojects
// the same surface whose radiance is present in currentColor.
//
// 2026-09-30 方案 B (GI_GLASS_PANE_DEPTH_GUIDE): 玻璃片是例外。隔着玻璃面的
// 接收面在玻璃内部的视差/折射下逐帧跳动，RR 的深度一致性判据会把这份跳动读成
// disocclusion 并反复清历史，表现为"静止压得住、相机一动玻璃里就重新冒 1spp
// 噪点"。玻璃面自身是静止几何、重投影精确到亚像素；而且在世界上玻璃面上的
// 一个点唯一决定它后面的接收面，所以按玻璃面重投影后仍落回同一个玻璃面点，
// 接收面那套记录（法线 / 粗糙度 / 反照率）依旧自洽。
//
// 水面保持旧行为：水底接收面本身稳定，不吃这份代价。
//
// B①：本段对 K1/K2 都编入 —— 两者各写**互斥的像素集**（K1=玻璃/sky，
// K2=延期的地形/水/实体/粒子），不存在同址两写。旧 base-B 的 #ifndef 屏蔽
// 是错的：切点上移后 K1 根本没给 opaque 像素算过 guide。
bool rrUseReceiverDepthGuide =
    rrTransparentReceiverGuideValid;

#if GI_GLASS_PANE_DEPTH_GUIDE
if(rrTransparentReceiverIsPane
#if ORION_GLASS_REMIX_PSTR_GUIDES
   && !rrGlassPstrActive
#endif
)
{
    rrUseReceiverDepthGuide =
        false;
}
#endif

if(rrUseReceiverDepthGuide)
{
    vec4 receiverViewPos =
        viewMatrix *
        vec4(
            rrTransparentReceiverWorldPos,
            1.0
        );

    vec4 receiverClipPos =
        projectionMatrix *
        receiverViewPos;

    if(abs(receiverClipPos.w) > 1.0e-6)
    {
        float receiverDepth =
            clamp(
                receiverClipPos.z /
                receiverClipPos.w,
                0.0,
                1.0
            );

        imageStore(
            depthImage,
            pixelCoord,
            vec4(
                receiverDepth,
                0.0,
                0.0,
                0.0
            )
        );
    }

#if !GI_HALFRES && !SHARC_UPDATE
    if(cam.perfAblationStage.z != 0u &&
       dot(rrTransparentReceiverWorldPos, rrTransparentReceiverWorldPos) > 1.0e-8)
    {
        const vec4 receiverPrevClip =
            cam.prevViewProj * vec4(rrTransparentReceiverWorldPos, 1.0);
        const vec4 receiverCurClip =
            cam.viewProj * vec4(rrTransparentReceiverWorldPos, 1.0);

        if(abs(receiverPrevClip.w) > 1.0e-6 &&
           abs(receiverCurClip.w) > 1.0e-6)
        {
            const vec2 receiverPrevNdc =
                receiverPrevClip.xy / receiverPrevClip.w;
            const vec2 receiverCurNdc =
                receiverCurClip.xy / receiverCurClip.w;
            const vec2 receiverMotion =
                (receiverPrevNdc - receiverCurNdc) * vec2(0.5, -0.5);

            imageStore(
                dlssMotionImage,
                pixelCoord,
                vec4(receiverMotion, 0.0, 1.0)
            );
            rtDebugPrimaryMotion = receiverMotion;
        }
    }
#endif
}


// =====================================================
// RR-6.2 - Write dense guide buffers once per pixel
// =====================================================

// 2026-10-08：这 4 条 store 以前**无任何开关**，而 stage 5 的名字
// （DLL 表里是 "noG2bGuide"）在现源码下是空臂 —— perfL10DisableG2bLocalGuide()
// 唯一的调用点(:10180)被 LL_P3_ENABLE_G2B_LOCAL_GUIDE = false 短路掉了。
// 现在把 stage 5 接到"每像素无条件写 4 张 RR guide"这件事上，让 Tier-1
// 「guide 选择性 store」第一次有价格。**注意日志里的臂名仍是旧名**，要改名得等
// 下一次 C++ 改动顺手动 VulkanCore.cpp:1478 那张表（本轮不动 C++：另一个改动正在
// 同一个 TU 上跑，20:20 刚重编部署过）。
// 关掉后 RR 读到的是上一帧/未定义的 guide，画面会花，仅用于诊断。
if(!perfL10DisableG2bLocalGuide())
{
imageStore(
    rrDiffuseAlbedoImage,
    pixelCoord,
    vec4(
        rrDiffuseAlbedo,
#if GI_RR_DELTA_FLAG
        // alpha 通道携带"跨 delta（穿过玻璃）"标记给 rr_guide_relax.comp。
        // stable-plane 记录的 flags>>8 携带同一信息）。
        rrDeltaCrossedFlag
#else
        1.0
#endif
    )
);

imageStore(
    rrSpecularAlbedoImage,
    pixelCoord,
    vec4(
        rrSpecularAlbedo,
        1.0
    )
);

imageStore(
    rrNormalRoughnessImage,
    pixelCoord,
    rrNormalRoughness
);

imageStore(
    rrSpecularHitDistanceImage,
    pixelCoord,
    vec4(
        rrSpecularHitDistance,
        0.0,
        0.0,
        0.0
    )
);

// 2026-10-23：RR TransparencyHint。判据复用采样循环外已声明的两个每像素量：
//   rrDeltaCrossedFlag             1 = 内容来自"穿过玻璃"的路径
//   rrTransparentReceiverGuideValid 透明接收面接管了正面 guide 记录
// 任一为真 ⇒ 该像素的可见颜色不是主表面自身的颜色 ⇒ RR 不该按主表面历史平均。
// 不新增状态、不改变任何现有 guide 的语义。
// =====================================================
// 2026-10-25 **回退（用户实测否决）**：玻璃/透射像素退出 hint。
// 实测：hint=1 + 运动封顶（VPT_TRANSPARENT_MOTION_CAP）在**快速转视角**时
// 两级时域同时失效 —— rgen 侧滑移大 ⇒ 历史封顶≈1；RR 侧 hint ⇒ 不信任历史
// ⇒ 透过玻璃的内容直接变成原始 1spp，"非常大的噪点"（用户原话）。
// hint 改为**只标记粒子**：玻璃恢复完全按主表面历史平均（10-23 之前的处理）；
// 粒子是屏幕上的少数像素，且生灭语义本来就需要降信任。宿主开关保持默认开。
// =====================================================
imageStore(
    rrTransparencyHintImage,
    pixelCoord,
    vec4(
        rrParticlePixel ? 1.0 : 0.0,
        0.0,
        0.0,
        0.0
    )
);
}


// =====================================================
// RR-6.3 / RR-6.5 input contract
//
// DLSS Ray Reconstruction receives noisy linear HDR RT color.
// Custom temporal history stays available internally, but is not
// fed into RR. SDR display conversion will happen after RR.
// =====================================================

// =====================================================
// DH far-field composite - stage 1 (availability) + stage B (geometry/relight)
//
// 只在**我们的主射线 miss**（我们的 TLAS 在这个方向上没有几何）的像素上，
// 用 DH 的 LOD 接管；我们命中过的像素一律不动 —— 我们的真几何永远比 LOD 精确。
// 不写深度、不参与我们自己的 LOD。
//
// The gate is dhFarFieldParams.x, never the sample.  When DH is absent the host
// parks the block-atlas view in bindings 47/48, and an atlas texel is a
// perfectly legal "depth" that would otherwise pass the test and paint garbage.
//
// ⚠ 极性：DH 的 API 自报 `depthRange = ZERO_TO_POS_ONE  depthDirection =
// REVERSE_Z`（本次运行 latest.log 的 [DH] 段实测）⇒ **清屏值是 0.0（远），
// 被几何遮住的像素落在 (0, 1]**。所以"被覆盖"= 大于 0，不是小于 1。
//
// 两路独立判据同时成立才覆盖：
//   * DH 侧 —— 它的深度说这个像素被 LOD 几何遮住（见 dhFarDepthIsGeometry）
//   * 我们侧 —— 主射线 miss（dhFarFieldOurs，在采样循环里按 primaryHitDistance
//     置位；miss 的哨兵是 -1.0，见 rt_test.rmiss）。这条同时是兜底：万一 DH 那张
//     depth 其实是与原版共享的深度，近场也会 >0，而 DH 的 color 在近场只有它的
//     清屏色 —— 那就会把近处整片刷掉。
//
// ⚠ 不要再把"距离 > N 格"加回我们侧判据：那会在屏幕上画出一条以相机为中心的
//    等距线（平视=直线、俯视=圆弧），并且让 DH 的低模顶掉我们 64 格外的真几何。
//    完整理由见置位处（rt_test.rgen，dhFarFieldOurs = 1.0 那段）。
//
// =====================================================
// ⭐ stage B 的契约（09-23 第二次定案，别再退回"报警式"设计）
// =====================================================
// 远场几何 = **锚定在我们自己的主射线上**：
//     距离 distance  ← 逆 MVP（→ z 镜像 → 解析 reverse-Z 三级回落）
//     位置 position  = camPos + rayDir * distance
// 因此"重建点落在本像素主射线上"是恒等式，矩阵/约定/NaN 都不可能把远场打歪到
// 不可用，也就不再需要"落不上就刷品红"那种把整片远场丢掉的兜底 —— 上一版正是
// 那样，实测结果是整片远场变品红，什么都没看到。
//
// =====================================================
// ⭐ stage B 光照契约（09-23 第三次定案）
// =====================================================
// 远场**不是**"乘一个相对比值"，而是用**与近场同一条**光照模型重新点灯：
//     颜色 = DH 的图（当 albedo）× (直射 + 环境底)
// 直射与环境底都取自近场那两条表达式的同一个函数/同一个绝对尺度 ⇒ 昼、夜、
// 黄昏、雷雨两侧自动同步，交接处（我们数据边缘）不再有亮度台阶。
// 详见 main 末尾"3) 重打光"那一段的注释。
//
// =====================================================
// ⭐ 总开关（09-23 第四次定案，为**一次跑清归属**而加）
// =====================================================
// `cam.dhFarFieldLight.x < 0` ⇒ 整个合成块一次都不执行，远场像素保持我们自己的
// 颜色（miss 的天空色 / 近场色）。
// 用法：`-Dorion.dh.farLight.mode=-1`（Java 侧直接把它推下来，**不用重编 DLL**）。
// 这是回答"画面里某个现象是不是 DH 合成引入的"的**唯一**干净判据：
// 关掉它现象消失 = 我们的；还在 = DH 自己画的或与 DH 无关。
// =====================================================
int dhFarModeRequested = int(cam.dhFarFieldLight.x + 0.5);

// 2026-11-19 rasterLod（mode=14）：外门放宽 —— mode=14 时本项目光栅 color/depth
// 图替代 DH 贴图源，不再依赖 dhFarFieldParams.x（DH 纹理发布态）。其余 mode
// 的门不变（dhFarModeRequested 由 Java 推送，rasterLod 关闭时永远到不了 14）。
#ifndef ORION_K2_TRANSPORT
// ============================================================================
// Orion 内置自研远景 LOD（mode=16，零外部 Mod 依赖）
// 仅在主 RT 射线 miss (dhFarFieldOurs > 0.5) 且光栅线性距离 > 0.5 时生效；
// 近场真光追几何永远 100% 优先。使用与近场同源的太阳/天光/云影/水体/大气透视，
// 并同步更新 depthImage / dlssMotionImage / DLSS-RR guides 以实现雾与超分融合。
// ============================================================================
if (dhFarModeRequested == 16 && dhFarFieldOurs > 0.5) {
    float lodDist = orionLodLinearDistAt(pixelCoord);
    if (dhFarFinite(lodDist) && lodDist > 0.5) {
        vec3 lodCamPos;
        vec3 lodRayDir;
        dhFarPrimaryRay(pixelCoord, lodCamPos, lodRayDir);
        vec3 lodPos = lodCamPos + lodRayDir * lodDist;

        vec4 lodColorAo = orionLodColorAoAt(pixelCoord);
        vec4 lodNormalMat = orionLodNormalMatAt(pixelCoord);

        vec3 lodAlbedo = pow(max(lodColorAo.rgb, vec3(0.0)), vec3(2.2));
        float lodSkyAo = clamp(lodColorAo.a, 0.12, 1.0);
        vec3 lodN = orionDecodeOctNormal(lodNormalMat.rg);
        int lodMatClass = int(floor(lodNormalMat.b * 4.0 + 0.5));
        float lodAux = clamp(lodNormalMat.a, 0.0, 1.0);

        vec3 lodSunDir = normalize(getTimeDrivenSkyLightDirection());
        float lodCloudTrans = c3CloudSunTransmittance(lodPos + lodSunDir * 0.001, lodSunDir);
        vec3 lodSunRadiance =
            getTimeDrivenSkyLightColor() *
            3.2 *
            getTimeDrivenSkyLightRadianceScale() *
            lodCloudTrans;

        vec3 lodSkyZenith = samplePhysicalClearSkyForAerialPerspective(vec3(0.0, 1.0, 0.0));
        vec3 lodSkyHemiDir = normalize(mix(lodN, vec3(0.0, 1.0, 0.0), 0.45));
        lodSkyHemiDir.y = max(lodSkyHemiDir.y, 0.03);
        vec3 lodSkySample = samplePhysicalClearSkyForAerialPerspective(normalize(lodSkyHemiDir));
        vec3 lodSkyGi = max(mix(lodSkyZenith, lodSkySample, 0.55), lodSkyZenith * 0.50);

        vec3 lodAmbient = lodAlbedo * getSurfaceAmbientRadiance() * mix(0.75, 1.0, lodSkyAo);
        vec3 lodViewDir = normalize(-lodRayDir);
        vec3 lodLit = vec3(0.0);
        vec3 lodGuideNormal = lodN;
        float lodGuideRoughness = 0.95;
        vec3 lodGuideSpec = vec3(0.0);
        vec3 lodGuideDiffuse = lodAlbedo;

        if (lodMatClass == 4) {
            // 自发光方柱（岩浆/荧石等）
            lodLit = lodAlbedo * 4.2 + lodAmbient;
            lodGuideRoughness = 0.85;
        } else if (lodMatClass == 1) {
            // 远景水体：微浪法线 + Fresnel 天空反射 + GGX 太阳高光 + 水深 Beer-Lambert 吸收
            vec3 waterN = orionLodWaterWaveNormal(lodPos.xz, lodDist, float(cam.frameCount) * 0.016);
            lodGuideNormal = waterN;
            lodGuideRoughness = 0.08;
            lodGuideSpec = vec3(0.04);

            float NdotV = max(dot(waterN, lodViewDir), 0.02);
            float fresnel = clamp(0.02 + 0.98 * pow(1.0 - NdotV, 5.0), 0.02, 0.95);
            vec3 reflDir = reflect(lodRayDir, waterN);
            reflDir.y = max(reflDir.y, 0.02);
            vec3 reflSky = samplePhysicalClearSkyForAerialPerspective(normalize(reflDir))
                         * mix(0.55, 1.0, lodCloudTrans);

            BsdfTerms waterBsdf = evalBSDFTerms(
                waterN, lodViewDir, lodSunDir, vec3(0.02), 0.0, 0.08, NdotV, false
            );
            float waterNdotL = max(dot(waterN, lodSunDir), 0.0);
            vec3 waterSpec = waterBsdf.specular * waterNdotL * lodSunRadiance;

            vec3 deepTint = lodAlbedo * vec3(0.18, 0.36, 0.52);
            vec3 waterBodyColor = mix(deepTint, lodAlbedo * 0.72, exp(-lodAux * 4.5));
            vec3 waterScatter = waterBodyColor * (
                lodSunRadiance * waterNdotL * 0.32 +
                lodSkyZenith * (0.50 * 1.35) +
                getSurfaceAmbientRadiance()
            );
            lodGuideDiffuse = waterBodyColor;
            lodLit = mix(waterScatter, reflSky, fresnel * 0.90) + waterSpec;
        } else {
            // 地形 / 植被 / 冰雪
            float roughness = (lodMatClass == 3) ? 0.35 : ((lodMatClass == 2) ? 0.92 : 0.96);
            lodGuideRoughness = roughness;
            vec3 f0 = (lodMatClass == 3) ? vec3(0.05) : vec3(0.04);
            lodGuideSpec = (lodMatClass == 3) ? vec3(0.04) : vec3(0.0);

            float rawNdotL = dot(lodN, lodSunDir);
            float microShadow = smoothstep(-0.18, 0.25, rawNdotL) * mix(0.65, 1.0, lodSkyAo);
            float viewNdotL = max(dot(lodN, lodViewDir), 0.02);

            BsdfTerms lodBsdf = evalBSDFTerms(
                lodN, lodViewDir, lodSunDir, f0, 0.0, roughness, viewNdotL, false
            );
            vec3 lodDiffuseBrdf = lodBsdf.kD * lodAlbedo * evaluateBurleyDiffuseAngular(
                lodN, lodViewDir, lodSunDir, roughness
            );
            vec3 lodDirect = lodSunRadiance * microShadow * (
                lodDiffuseBrdf + lodBsdf.specular * max(rawNdotL, 0.0)
            );
            if (lodMatClass == 2) {
                lodDirect += lodSunRadiance * (0.16 * lodSkyAo) * lodAlbedo * max(-rawNdotL, 0.0);
            }
            float hemiLift = mix(0.72, 1.0, clamp(lodN.y * 0.5 + 0.5, 0.0, 1.0));
            vec3 lodIndirect = lodAlbedo * lodSkyGi * (0.6 * 1.8) * mix(0.62, 1.0, lodSkyAo) * hemiLift;
            lodLit = lodDirect + lodIndirect + lodAmbient;
        }

        lodLit = applyClearSkyAerialPerspective(lodLit, lodCamPos, lodRayDir, lodDist);
        currentColor = lodLit;
        dhFarFieldOursDistance = lodDist;

#if !GI_HALFRES && !SHARC_UPDATE
        vec4 lodCurClip = cam.viewProj * vec4(lodPos, 1.0);
        if (abs(lodCurClip.w) > 1.0e-4) {
            float lodRawDepth = clamp(lodCurClip.z / lodCurClip.w, 1.0e-5, 1.0);
            imageStore(depthImage, pixelCoord, vec4(lodRawDepth, 0.0, 0.0, 0.0));
        }

        if (cam.perfAblationStage.z != 0u) {
            vec4 lodPrevClip = cam.prevViewProj * vec4(lodPos, 1.0);
            if (abs(lodCurClip.w) > 1.0e-4 && abs(lodPrevClip.w) > 1.0e-4) {
                vec2 lodPrevNdc = lodPrevClip.xy / lodPrevClip.w;
                vec2 lodCurNdc = lodCurClip.xy / lodCurClip.w;
                vec2 lodMotion = (lodPrevNdc - lodCurNdc) * vec2(0.5, -0.5);
                imageStore(dlssMotionImage, pixelCoord, vec4(lodMotion, 0.0, 1.0));
                rtDebugPrimaryMotion = lodMotion;
            }
        }

        if (!perfL10DisableG2bLocalGuide()) {
#if GI_RR_DELTA_FLAG
            imageStore(rrDiffuseAlbedoImage, pixelCoord, vec4(lodGuideDiffuse, rrDeltaCrossedFlag));
#else
            imageStore(rrDiffuseAlbedoImage, pixelCoord, vec4(lodGuideDiffuse, 1.0));
#endif
            imageStore(rrSpecularAlbedoImage, pixelCoord, vec4(lodGuideSpec, 1.0));
            imageStore(rrNormalRoughnessImage, pixelCoord, vec4(lodGuideNormal, lodGuideRoughness));
            imageStore(rrSpecularHitDistanceImage, pixelCoord, vec4(lodDist, 0.0, 0.0, 0.0));
        }
#endif
    }
}
// K2 编出 DH 远场整块：它由 dhFarFieldOurs > 0.5 守卫，而 K2 只处理 opaque
// 像素（dhFarFieldOurs 恒 0），K1 已为 miss/水/玻璃像素做完 DH 合成。
else if ((cam.dhFarFieldParams.x > 0.5 || dhFarModeRequested == 14
        || dhFarModeRequested == 15)
    && dhFarModeRequested >= 0
    && dhFarFieldOurs > 0.5) {
    // 上下翻转与色彩空间的完整推导见 dhFarTextureUv 与下面的 dhFarGamma 注释。
    vec2 dhFarUv = dhFarTextureUv(pixelCoord);

    // ⚠ 变量名不能叫 dhFarDepth：那是 binding 48 的 sampler，同名局部变量会把它
    // 遮住，dhFarDepthAt 里的 texelFetch 会因类型不匹配而编译失败。
    float dhFarDepthCenter = dhFarDepthAt(pixelCoord);

    if (dhFarDepthIsGeometry(dhFarDepthCenter)) {
        // 2026-11-19 rasterLod（mode=14）：颜色源换成本项目光栅 pass 的 RGBA8 图
        //（binding 96，dhLodRasterColorAt）——替代 DH 贴图源（texture(dhFarColor)）。
        // 两者都是 display 编码的顶点色 ⇒ 下面的 gamma 解码 / 强度 / 雾机制
        // 原样复用。深度源已在 dhFarDepthAt 内按 mode=14 切换 ⇒ 上面的几何
        // 判据对两种源同构（depth ∈ (0,1]）。mode≠14 时条件与改前逐位一致。
        vec3 dhFarSampled =
            (dhFarModeRequested == 14)
                ? dhLodRasterColorAt(pixelCoord)
                : texture(dhFarColor, dhFarUv).rgb;

        // =====================================================
        // ⚠ 色彩空间定案（09-23，证据来自直接读 DH 3.3.2 的 jar）：
        //    DH 图里存的是**显示编码值**，所以这里必须 pow(2.2) 解回线性
        // =====================================================
        // 本 pass 的 currentColor 是**线性**值 —— 末端 rr_sdr_post.comp 才做
        // pow(c, 1/2.2) 编码成显示值。
        //
        // 三条硬证据（不再是推测）：
        //   1. `BlazeTextureWrapper.createColor` 用的格式是 **RGBA8_UNORM**（不是
        //      *_SRGB），`createDepth` 是 D32_FLOAT ⇒ sampler **不会**自动线性化，
        //      texture() 拿到的就是它在图里存的原始值。
        //   2. DH 的 `shaders/apply/blaze/frag.fsh`（把 LOD 合进 MC 主画面的那个）
        //      与 `shaders/copy/blaze/frag.fsh` 都是**裸 texture() 直通**，全 DH 包里
        //      搜不到任何 sRGB/gamma 变换（唯一一处 pow 是噪声调制）⇒ DH 的图必须与
        //      MC 的 LDR 主画面**同空间**，否则 DH 自己就显示错了。
        //   3. 我们管线末端才 pow(1/2.2) ⇒ MC 主画面存的是显示编码值（否则我们整套
        //      RT 画面会整体偏暗，实测不偏）。
        //   ⇒ DH 图 = 显示编码值 ⇒ 先 pow(2.2) 解回线性，末端再统一编码。
        //     此时末端 pow(1/2.2) 与它互为逆运算 ⇒ **远场逐字节等于 DH 自己的画面**，
        //     这才是 stage-1 要的"原封不动"；任何别的 g 都是在改写 DH 的画面
        //     （g=1.0 = 双重解码：偏亮、发灰、低对比）。
        //
        // 分屏对照已跑完（09-23 13:5x）：左半 g=1.0 泛白发灰、右半 g=2.2 是正常的
        // 暗色画面 ⇒ 定案 g=2.2。分屏分支已删除，这里是最终常数。
        const float dhFarGamma = 2.2;

        vec3 dhFarLinear =
            pow(
                max(dhFarSampled, vec3(0.0)),
                vec3(dhFarGamma)
            );

        // =====================================================
        // stage B：几何定光（mode >= 1 且矩阵已到达时才走）
        // =====================================================
        // mode 已在合成块外读好（那里还要用负值做总开关）。
        int dhFarMode = dhFarModeRequested;

        // =====================================================
        // mode=7：四联诊断（一屏读出 4 个读数，省一整局）
        // =====================================================
        // 屏幕按 x 四等分，从左到右依次是 mode 1 / 2 / 5 / 6：
        //   ① 误差图（绿=矩阵重建点落在主射线上 ⇒ "矩阵这条路通"）
        //   ② 法线（应当看到连贯的坡面）
        //   ⑤ 距离来源（蓝=解析首选 / 红=矩阵 / 绿=z 镜像 / 黄=长度兜底）
        //   ⑥ 距离灰度（远处亮；整片压成一个灰度 ⇒ 深度语义反了）
        // 先展开成这四档，后面所有分支（含下面 1/2/5/6 的诊断档判定）都不用改。
        if (dhFarMode == 7 || dhFarMode == 15) {
            float dhFarDiagSlot = clamp(
                (float(pixelCoord.x) + 0.5) / max(float(size.x), 1.0),
                0.0,
                0.9999
            );
            int dhFarDiagIndex = int(dhFarDiagSlot * 4.0);
            dhFarMode = (dhFarDiagIndex == 0) ? 1
                : (dhFarDiagIndex == 1 ? 2 : (dhFarDiagIndex == 2 ? 5 : 6));
        }

        // =====================================================
        // mode=9：左右分屏对照（一次回答"这些现象是不是我们引入的"）
        // =====================================================
        // 左半屏 = mode 0（DH 的原图直通，我们一点不碰）
        // 右半屏 = mode 4（默认档：几何定光 + 大气）
        // 转视角让远场同时出现在两半，比较：浅灰大块、颜色不同的线、色调差。
        // 左半有 = DH 自己画的；只在右半 = 我们引入的。
        if (dhFarMode == 9) {
            float dhFarSplitSlot = (float(pixelCoord.x) + 0.5) / max(float(size.x), 1.0);
            dhFarMode = (dhFarSplitSlot < 0.5) ? 0 : 4;
        }

        bool dhFarGeometryReady = cam.dhFarFieldParams.y > 0.5 && dhFarMode >= 1;

        if (!dhFarGeometryReady) {
            // stage-1 直通（mode 0，或矩阵还没到）
            currentColor = dhFarLinear;
        } else {
            // =====================================================
            // 本像素的主射线 + 相机位置（与 main 开头同一套映射）
            // =====================================================
            vec3 dhFarCameraPos;
            vec3 dhFarRayDir;
            dhFarPrimaryRay(pixelCoord, dhFarCameraPos, dhFarRayDir);

            // DH 自己的 near/far（C++ 每帧从 API 载荷带过来）。解析兜底全靠它，
            // 所以先夹到有意义的值，避免"矩阵没到 ⇒ near/far 全是 0 ⇒ 兜底除零"。
            float dhFarNear = max(cam.dhFarFieldParams.z, 0.5);
            float dhFarFar = max(cam.dhFarFieldParams.w, dhFarNear + 1.0);

            // =====================================================
            // 1) 变体自选：判据是**平移无关**的
            // =====================================================
            // 候选方向 = 反投影点 − 相机。透视矩阵下这个方向只由 ndc 的 xy 决定、
            // 与 z 无关 ⇒ 这条判据不接受 z 语义的影响；坐标系平移也不影响方向的
            // 一致性（它只挪点、不转方向）。所以它**只**检验真正需要现场确定的两件
            // 事：16 个 float 是行序还是列序、NDC 的 y 指向。
            //
            // 旁证（DH 3.3.2 的 jar 反汇编，可复核）：
            //   DhApiMat4f.putValuesInArray 是 arr[4i+j] = m_ij（行序）
            //   GlShaderProgram 用 glUniformMatrix4fv(loc, count, false, buf)
            //   DH 自己的着色器写 uCombinedMatrix * vec4(pos,1) / invMvmProj * ndc
            // ⇒ 行序数组 + transpose=false 在 GLSL 里读出来的 mat，直接 mat*vec4
            //    就是 DH 要的变换 ⇒ 变体位 0（不转置）理论正确。自检仍然照跑：
            //    它是独立验证，也是"矩阵真的送对了"的唯一在线证据。
            int dhFarForced = int(cam.dhFarFieldLight.w + 0.5);
            bool dhFarForcedVariant = dhFarForced >= 1 && dhFarForced <= 4;
            int dhFarFirst = dhFarForcedVariant ? dhFarForced - 1 : 0;
            int dhFarCount = dhFarForcedVariant ? 1 : 4;
            int dhFarVariant = dhFarFirst;
            // 诊断量：被选中的候选与主射线方向的一致性（1-cos）。0 = 完全落上。
            float dhFarError = 1.0e9;

            for (int dhFarCandidateIndex = 0;
                 dhFarCandidateIndex < dhFarCount;
                 ++dhFarCandidateIndex) {
                int dhFarCandidate = dhFarFirst + dhFarCandidateIndex;
                vec3 dhFarCandidatePoint = dhFarWorldPoint(
                    pixelCoord,
                    dhFarDepthCenter,
                    dhFarCandidate
                );
                vec3 dhFarCandidateOffset = dhFarCandidatePoint - dhFarCameraPos;
                float dhFarCandidateLength = length(dhFarCandidateOffset);

                if (!dhFarFinite(dhFarCandidateLength) || dhFarCandidateLength < 1.0e-4) {
                    continue;
                }

                float dhFarCandidateError =
                    1.0 - dot(dhFarCandidateOffset / dhFarCandidateLength, dhFarRayDir);

                if (dhFarFinite(dhFarCandidateError) && dhFarCandidateError < dhFarError) {
                    dhFarError = dhFarCandidateError;
                    dhFarVariant = dhFarCandidate;
                }
            }

            // =====================================================
            // 2) 距离（解析优先 + 矩阵兜底，见 dhFarDistanceFor）
            // =====================================================
            int dhFarDistanceSource = 3;
            float dhFarDistance = dhFarDistanceFor(
                pixelCoord,
                dhFarDepthCenter,
                dhFarCameraPos,
                dhFarRayDir,
                dhFarVariant,
                dhFarNear,
                dhFarFar,
                dhFarDistanceSource
            );

            // =====================================================
            // 3) 位置：**锚定在自己的主射线上**
            // =====================================================
            // 旧版把矩阵算出的世界点直接当位置。矩阵一旦不可信（NaN、约定不符、
            // z 语义不符），那个点可能整体飞出屏幕，只能靠"落不上就刷品红"兜底 ——
            // 结果就是整片远场变品红，什么都没看到，还白烧一轮。
            //
            // 现在矩阵只负责给**一个标量距离**，位置写成 camPos + rayDir * distance：
            // "重建点落在本像素主射线上"不再是需要检验的命题，而是恒等式。
            // 距离本身有三级回落，所以整条链路没有"失败"这个状态。
            vec3 dhFarPosition = dhFarCameraPos + dhFarRayDir * dhFarDistance;

            // =====================================================
            // 3.5) 天空/云护栏（09-23 第四次定案）
            // =====================================================
            // ⚠ 为什么必须加：我们主射线的 miss **同时包含天空和云**
            //    —— rmiss 里"主光线天空像素由计算云管线合成"，云不走 rmiss，
            //    所以云像素对我们来说就是一次普通的 miss ⇒ dhFarFieldOurs = 1
            //    ⇒ 合成块照跑。而 DH 的 color/depth 里**也**有它自己画的云
            //    （见 DH 的 `vanilla_fade`：`if (dhVertexWorldPos.y > uMaxLevelHeight)`
            //    那段"prevent MC clouds rendering behind DH clouds"），
            //    于是我们会把 DH 的云/天空涂到我们自己的天空上。
            //    实测表现：云层里出现成片的黑色多边形块（15:55 录像，t≈6~12s）。
            //
            // 判据取**世界高度上界**：原版地形不会高过 320（本版本 y = −64..320），
            // 取 480 留足余量；而云层在 1600 格以上，两者永不重叠。
            // 这样"远山高于相机"（合法的，山脉可以高过眼睛）不会被误杀 ——
            // 只有"重建点高到 480 格以上"才判成天空/云，那是云或垃圾，不是地形。
            bool dhFarPositionIsWorld =
                dhFarPosition.y <= 480.0 && dhFarPosition.y >= -512.0;

            // =====================================================
            // 4) 法线：相邻像素各自锚定，再叉积（DH 没有法线图）
            // =====================================================
            // 只走 +x / +y 两个方向，且两侧都必须"同面"（见 dhFarNeighborPoint）。
            // ⚠ 刻意**不做**反方向回退（-x / -y）：那会让轮廓两侧的像素用上坡度符号
            // 相反的法线 ⇒ 法线水平分量整体反号 ⇒ 沿轮廓出现一条"一边亮一边暗"的
            // 线。轮廓处（跨面、同面判据不成立）退回天顶法线就好 —— 那里本来也无法
            // 从单侧邻域判定朝向。
            vec3 dhFarPointRight;
            vec3 dhFarPointUp;
            bool dhFarHasRight = dhFarNeighborPoint(
                pixelCoord, ivec2(1, 0), dhFarDepthCenter,
                dhFarVariant, dhFarNear, dhFarFar, dhFarPointRight
            );
            bool dhFarHasUp = dhFarNeighborPoint(
                pixelCoord, ivec2(0, 1), dhFarDepthCenter,
                dhFarVariant, dhFarNear, dhFarFar, dhFarPointUp
            );

            vec3 dhFarNormal = vec3(0.0, 1.0, 0.0);

            if (dhFarHasRight && dhFarHasUp) {
                vec3 dhFarCross = cross(
                    dhFarPointRight - dhFarPosition,
                    dhFarPointUp - dhFarPosition
                );
                float dhFarCrossLength = length(dhFarCross);

                if (dhFarFinite(dhFarCrossLength) && dhFarCrossLength > 1.0e-8) {
                    dhFarNormal = dhFarCross / dhFarCrossLength;
                }
            }

            // 叉积的符号跟像素步进方向有关，用"法线必须朝向相机"定死。
            if (dot(dhFarNormal, dhFarPosition - dhFarCameraPos) > 0.0) {
                dhFarNormal = -dhFarNormal;
            }

            // =====================================================
            // 4.6) 倾角压制：**远场变亮的真正原因**（09-23 第四次定案）
            // =====================================================
            // 实测（15:19 / 15:55 两段录像 + PIL 定量）：
            //   近场地面（法线 = 天顶）在低太阳时显示值 ~20；
            //   远场（DH 图 + 我们的光）显示值 ~65 ⇒ **低太阳下远场比近场亮 3~10 倍**，
            //   于是我们数据边缘那条等距弧就成了全屏最扎眼的接缝。
            // 近场的地面是平的 ⇒ NdotL = sin(太阳高度角)，低太阳时只有 0.1 上下 ⇒ 近场很暗；
            // 远场的 NdotL 却接近 1 ⇒ 只可能是**重建法线不是天顶**。
            //
            // 原因在数据本身：DH 的 depth 在远场是"分带量化"的，相邻像素的深度差里
            // 量化台阶占了大头 ⇒ 叉积出来的平面几乎垂直于视线 ⇒ 法线 ≈ −rayDir
            // （水平指向相机）。它在**同一带内是连贯的**，所以远场看起来平滑、只是整体偏亮。
            //
            // 修法：地形 LOD 的坡度有限（真实地形极少超过 45°），所以把倾角按 n.y 平滑压回。
            //   n.y = 1（朝上）    ⇒ 原样（平地/缓坡的朝向信息完全保留）
            //   n.y = 0（竖直）    ⇒ 退化成天顶（不可信的方向被丢弃）
            // 这是单调的"只压不翻"，并且**不引入任何新的常数阈值**（没有开关可调错）。
            // 低太阳下远场因此回到与近场同一个量级；高太阳下平地两侧本来就都接近全亮。
            float dhFarUpness = clamp(dhFarNormal.y, 0.0, 1.0);
            dhFarNormal = normalize(
                mix(vec3(0.0, 1.0, 0.0), dhFarNormal, dhFarUpness)
            );

            // =====================================================
            // 4.7) 光照诊断量（mode 11 读）
            // =====================================================
            // 三个量一次成像，不用为"到底哪一项把远场点亮了"再跑一局：
            //   R = dhFarNdotL（重建法线 · 太阳方向）—— 平地不该超过太阳高度角
            //   G = 重打光后的亮度去色（4.8 那条真实光照路径，含阴影与 GI）——
            //       与近场同一量级才对；非零即"远场面确实被我们的光照到"
            //   B = 云透过率 —— 低太阳下由它决定"这片地本来在不在云影里"
            vec3 dhFarSunDir = normalize(getTimeDrivenSkyLightDirection());
            float dhFarNdotL = max(dot(dhFarNormal, dhFarSunDir), 0.0);
            float dhFarCloudTransmittance = c3CloudSunTransmittance(
                dhFarPosition + dhFarSunDir * 0.001,
                dhFarSunDir
            );

            // =====================================================
            // 4.8) 远场点灯：**接我们真正的光照路径**（09-25 第七次定案）
            // =====================================================
            // 旧的解析近似（getTimeDrivenSkyLightRadianceScale()×3.2×NdotL×云
            // + 环境底）是"远场几何不在 TLAS 里"那个年代的产物 —— 既没有阴影，
            // 也拿不到 GI，低太阳时与近场差一个量级（mode 12 注释里"黄昏压成
            // 一大坨黑影"那段就是这个缺口）。方案 c 落地后远场 LOD 几何已经在
            // TLAS 里（实例掩码 0xfe ⇒ 相机不可见、**次级射线可见**），所以远场
            // 像素现在拿得到与近场同一套东西，本段就照近场主路径逐项构造：
            //
            //   直射   = sunRadiance × (KD·albedo·Burley + specBRDF·NdotL)
            //            ← 与近场 16850-16863 同构（LOD 面没有 LabPBR 材质：
            //              介电 F0=0.04、metallic=0、roughness=1）。
            //            ⚠ sunRadiance 里**没有**阴影透过率 —— 远场不投地形阴影，
            //              理由与实测证据写在下面那条注释里（死黑硬阴影）。
            //   间接   = 1 条余弦 GI 射线，与 GI 循环同一套约定：
            //            miss ⇒ unpackAlbedoUnorm(payload.albedoPacked)（rmiss 写的物理天空）× 0.6
            //            命中 ⇒ payload.emission + evaluateDirectSunLighting(...)
            //            （逐字对照 17490 的 miss 项与 17933 的 bounceDirect）
            //            ← 这一项是低太阳下远场的**主要亮度来源**，也是"远场不再
            //              比近场暗一大截"的关键；它的起点/tMin 见该段注释。
            //   环境底 = albedo × getSurfaceAmbientBase()（近场同一个函数）
            //   albedo = DH 的图（dhFarLinear）—— "图像定色"那一半保持不变。
            //
            // ⚠ 三条**明确不评估**的项（是边界，不是遗漏）：
            //   ① RTAO（近场 rtaoFactor）：它的射线从 0.035 格偏移起步，而这里的
            //      着色点是 DH 深度**按带量化**重建出来的（见 4.6 的推导），落点
            //      可能在网格内部 ⇒ 把量化噪声变成随机自遮挡（黑斑）。要做得对
            //      得让着色点来自我方网格本身（换次级掩码再投一次主射线），
            //      那是独立的一步，先不混进来。
            //   ② 局部光（localDirectLight / localExactFaceDirect /
            //      localSecondaryLight）：它们服务的体素光源半径十几格，而远场
            //      窗口的起点已经在原版视距之外（≥64 格）。
            //   ③ 反射（glossyReflectionLight）：albedo 来自 DH 的图、没有粗糙度
            //      可言，硬加等于凭空的镜面。
            //
            // strength / gain 两个旋钮语义不变（strength = 我们的光占多少，
            // gain = 整体标度微调）。
            vec3 dhFarDirect = vec3(0.0);
            vec3 dhFarGi = vec3(0.0);
            vec3 dhFarAmbient = vec3(0.0);
            // 只在真正要用它的档上付射线成本。判据写成**排除表**而不是列举表：
            // 下面自带成像的档是 1/2/5/6/10/12（7/9 已在上面展开成它们，0 档更早
            // 就 return 走了），**其余所有落进重打光分支的档（3/4/13/…）都要它** ——
            // 列举表一旦漏掉某个档（13 方向采样就差点漏掉），那个档的远场会直接
            // 变黑，而且看起来像"光照没算出来"而不是"门闸写错了"。
            bool dhFarRelightWanted =
                !(dhFarMode == 1 ||
                  dhFarMode == 2 ||
                  dhFarMode == 5 ||
                  dhFarMode == 6 ||
                  dhFarMode == 10 ||
                  dhFarMode == 12);

            if (dhFarRelightWanted && dhFarPositionIsWorld) {
                vec3 dhFarAlbedo = dhFarLinear;
                vec3 dhFarViewDir = normalize(-dhFarRayDir);
                float dhFarViewNdotL = max(dot(dhFarNormal, dhFarViewDir), 0.0);

                // ---- 直射（结构照抄 16755-16863）------------------------------
                vec3 dhFarF0 = vec3(0.04);
                BsdfTerms dhFarBsdf =
                    evalBSDFTerms(
                        dhFarNormal,
                        dhFarViewDir,
                        dhFarSunDir,
                        dhFarF0,
                        0.0,
                        1.0,
                        dhFarViewNdotL,
                        false
                    );
                vec3 dhFarDiffuse =
                    dhFarBsdf.kD *
                    dhFarAlbedo *
                    evaluateBurleyDiffuseAngular(
                        dhFarNormal,
                        dhFarViewDir,
                        dhFarSunDir,
                        1.0
                    );
                vec3 dhFarSpecBRDF = dhFarBsdf.specular;

                // 阴影：**远场不投地形阴影**（09-25 第八次定案 —— 实测回来改的，别再打开）
                //
                // 曾经这里有一条真阴影射线（照近场 16099-16163），实测（用户 20:0x
                // 截图 + 像素统计）在远场造出成片**死黑硬阴影**：局部列均值 25~30，
                // 周围 60~80 —— 是"我们画的"，不是 DH 图里本来就有的。
                //
                // 根因不在射线参数，而在**剪影是假的**：
                //   着色点在 DH 的细网格上（深度重建），射线穿过的是我方粗网格
                //   （逐格盘半径 192 以内 1 格分辨率，以外 2~8 格步长、曲面平均、
                //   无侧墙 —— 见 DhLodMesher 的可变步长网格）。低太阳下影子极长，
                //   粗网格的剪影和细网格差出去的那几格，就足以把**本来向阳的坡**
                //   整片判成背光。抬射线起点、加大 tMin 只能治"起点落在网格内部"
                //   那一类，治不了"远处粗网格挡住太阳"这一类。
                //   ⇒ 只要射线穿的是粗网格，远场地形阴影就不可信；不是调参问题。
                //
                // 近场阴影完全不受影响（真几何）。DH 自己的 LOD 光照本来也不带地形
                // 阴影，所以这里的 1.0 与它一致，不会在近/远接缝多出亮度台阶。
                //
                // 想把远场阴影找回来的**前提**（两条都满足再开）：
                //   ① 着色点改为来自我方网格本身（同方向、次级掩码 0xff 再投一次），
                //      这样射线与着色面严格同源；
                //   ② 只对可信的那一圈开（内圈逐格盘半径）——粗环的步长下，剪影误差
                //      与影子长度同量级，怎么调都会漏。
                vec3 dhFarShadowTransmittance = vec3(1.0);

                vec3 dhFarSunRadiance =
                    getTimeDrivenSkyLightColor() *
                    3.2 *
                    getTimeDrivenSkyLightRadianceScale() *
                    dhFarShadowTransmittance *
                    dhFarCloudTransmittance;

                dhFarDirect =
                    dhFarSunRadiance *
                    (dhFarDiffuse + dhFarSpecBRDF * dhFarNdotL);

                // ---- 间接：1 条余弦 GI 射线 ----------------------------------
                {
                    uint dhFarGiSeed = seed ^ 0x9e3779b9u;
                    vec3 dhFarGiDir = sampleHemisphereCosine(
                        dhFarNormal,
                        dhFarGiSeed
                    );
                    // 起点抬 1 格 + tMin 1.5 格（09-25 第八次定案，与上面"远场不投地形阴影"
                    // 同一次实测）：着色点在 DH 细网格上、射线穿我方粗网格，点可能落在
                    // 网格内部 ⇒ 向上的天光会被自己的背面挡住 ⇒ GI 项归零 ⇒ 低太阳下
                    // （远场亮度主要靠 GI 撑）那片就是死黑。抬 1 格 + 跳过前 1.5 格
                    // 一起，能容掉约 2.5 格的"网格高于点"误差；代价是忽略起点 1.5 格内的
                    // 遮挡物 —— 在远场那点立体角可以忽略（200 格外 1.5 格 ≈ 0.4°）。
                    vec3 dhFarGiOrigin = dhFarPosition + dhFarNormal * 1.001;

                    hit.hitDistance = 1.0;
                    hit.state = makeTraceState(RT_PURPOSE_FULL);
                    hit.state |= RT_STATE_GI_SKIP_CLOUDS_BIT;
#if GI_SKIP_SUN_DISK
                    hit.state |= RT_STATE_NO_SUN_DISK_BIT;
#endif

                    #ifndef ORION_K2I_COMPOSE_COMPUTE
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
                        dhFarGiOrigin,
                        1.5,
                        dhFarGiDir,
                        10000.0,
                        0
                    );
                    #else
                    payload.hitDistance = -1.0;
                    #endif

                    if (payload.hitDistance < 0.0) {
                        // 与 GI 循环的 miss 项逐字一致（17490）：rmiss 已经把
                        // 物理天空（含云、日轮）写进 unpackAlbedoUnorm(payload.albedoPacked)。
                        dhFarGi = dhFarAlbedo * unpackAlbedoUnorm(payload.albedoPacked) * 0.6;
                    } else {
                        vec3 dhFarGiPos =
                            dhFarGiOrigin + dhFarGiDir * payload.hitDistance;
                        vec3 dhFarGiNormal = normalize(payload.normal);

                        dhFarGi = dhFarAlbedo * (
                            payload.emission +
                            evaluateDirectSunLighting(
                                dhFarGiPos,
                                dhFarGiNormal,
                                -dhFarGiDir,
                                unpackAlbedoUnorm(payload.albedoPacked),
                                max(payload.roughness, 0.05),
                                payload.f0Metal,
                                clamp(payload.pomSelfShadow, 0.0, 1.0),
                                dhFarSunDir
                            )
                        );
                    }
                }

                dhFarAmbient = dhFarAlbedo * getSurfaceAmbientRadiance();
            }

            // 与近场 litColor + ambientLight 两段同构（18435 与 18484）。
            // 1.8 就是近场 GI 的那个能量尺度，不改；AO 项见上面 ①。
            vec3 dhFarOurLit =
                dhFarDirect +
                dhFarGi * 1.8 +
                dhFarAmbient;

            // =====================================================
            // 安全网已经被"不可能失败"取代（09-23 第二次定案）
            // =====================================================
            // 旧版在这里判断 dhFarError < 0.02，落不上就整片刷**纯品红**并 return。
            // 实测那等于把整片远场丢掉（用户看到的正是"远场全品红"），而且品红会
            // 顺着 RR 的 guide 被当成"很远的静态面"留在历史里，越攒越不退。
            //
            // 现在：
            //   * 位置锚定在主射线上（见上）⇒ "落不上"这个状态不存在；
            //   * 距离有三级回落（矩阵 → z 镜像 → 解析）⇒ 一定给得出有限值；
            //   * 矩阵非有限在 C++ 就被丢弃（保留上一帧的好矩阵）。
            // 于是这里不再需要兜底分支。dhFarError 只作为**诊断量**保留：
            //   mode=1 把它画成红/绿误差图，用来回答"矩阵这条路到底通不通"；
            //   mode=5 把距离来源画成 RGB（红=矩阵 绿=z镜像 蓝=解析）。
            // 想看"矩阵是否可信"，看这两个模式，而不是看画面颜色。

            // =====================================================
            // 4.5) 接管权重：**没有权重** —— 进到这里就说明我们没命中
            // =====================================================
            // 这一版把"64 格硬开关 + 按深度差渐变"整体删掉了：
            //   * 我们命中（任意距离）⇒ 合成块根本不会执行（见 dhFarFieldOurs 的置位处），
            //     我们的真几何永远优先于 DH 的 LOD；
            //   * 我们没命中 ⇒ 这里就是远场，权重恒为 1。
            // 于是"等深硬线"与"两侧混色"这两件事同时在结构上不存在了。
            // 交接处剩下的视觉台阶只可能来自**两侧光照不一致** —— 那是重打光那一段
            // 要解决的问题（用与近场同一条光照模型），不是靠权重糊。

            // =====================================================
            // stage B：远场现在是"我们的"几何 ⇒ DLSS RR 的 guide 也必须是我们的
            // =====================================================
            // 前面那四个 imageStore（rrDiffuseAlbedo / rrSpecularAlbedo /
            // rrNormalRoughness / rrSpecularHitDistance）写的是**近场**命中的
            // 材质。远场像素的主射线其实 miss 了（primaryHitDistance = -1）或落在
            // 64 格之外，那些 guide 全是错的。
            //
            // 这不是小事：见 tracePrimaryGenericTransparentRadiance 上方那段注释
            // —— 当 guide 与真实内容不符时，RR 会把远端内容按**错误的 motion /
            // 65504 距离**去重投影，直接糊成一团。远场尤其明显，因为 DH 的图是
            // 一整片连续地形，糊起来就是"远场永远不清"。
            //
            // albedo 用 dhFarLinear（DH 那张已经解码回线性的颜色）：它本身是
            // "被光照过的地表色"，作为漫反射 albedo 的近似比近场的错误 albedo 好
            // 得多；roughness 给 1.0（LOD 地形没有高光细节），specular 为 0。
            if (dhFarMode >= 2 && dhFarPositionIsWorld) {
                rrDiffuseAlbedo = dhFarLinear;
                rrSpecularAlbedo = vec3(0.0);
                rrNormalRoughness = vec4(dhFarNormal, 1.0);
                // 用锚定后的距离，不用 |P − camPos|：两者在几何正确时相等，但锚定
                // 版本永远不会因为矩阵算歪而给出一个荒谬的 guide（那正是"远场糊成一
                // 团"的燃料）。
                rrSpecularHitDistance = dhFarDistance;

                // 重写一遍 guide（上面 16645 那四处写的是近场值，这里盖掉）。
                // 必须在这里显式写：rrDiffuseAlbedo 这几个是 main 的**局部变量**，
                // 没法抽成函数从别处调用。
                #if GI_RR_DELTA_FLAG
// alpha 通道 = "跨 delta（穿过玻璃）"标记，供 rr_guide_relax.comp 使用。
imageStore(rrDiffuseAlbedoImage, pixelCoord, vec4(rrDiffuseAlbedo, rrDeltaCrossedFlag));
#else
imageStore(rrDiffuseAlbedoImage, pixelCoord, vec4(rrDiffuseAlbedo, 1.0));
#endif
                imageStore(rrSpecularAlbedoImage, pixelCoord, vec4(rrSpecularAlbedo, 1.0));
                imageStore(rrNormalRoughnessImage, pixelCoord, rrNormalRoughness);
                imageStore(
                    rrSpecularHitDistanceImage,
                    pixelCoord,
                    vec4(rrSpecularHitDistance, 0.0, 0.0, 0.0)
                );
            }

            if (dhFarMode == 1) {
                // 自检图：绿 = 矩阵重建点精确落在主射线上（矩阵这条路通），红 = 没落上。
                // 这是"矩阵是否可信"的定量读数：整片绿 = 矩阵完全可信，此后 mode=3 的
                // 距离就是矩阵给的（最精确）；整片红也不影响出图（有解析兜底），
                // 只是距离改用解析式。
                float dhFarQuality = clamp(dhFarError * 8.0, 0.0, 1.0);
                currentColor = vec3(dhFarQuality, 1.0 - dhFarQuality, 0.0);
            } else if (dhFarMode == 2) {
                // 法线可视化：应当看到连贯的坡面朝向，而不是噪声。
                currentColor = dhFarNormal * 0.5 + vec3(0.5);
            } else if (dhFarMode == 5) {
                // 距离来源图（一次就能读出整条链路的健康度）：
                //   蓝 = 解析 reverse-Z（**首选**；lane 是方块值时它必对）
                //   红 = 逆 MVP 直接给的距离（解析失败时的矩阵兜底）
                //   绿 = z 取镜像才对得上（z 的正/反与矩阵不符）
                //   黄 = 长度兜底（连矩阵都失灵）
                currentColor = dhFarDistanceSource == 0
                    ? vec3(0.0, 0.0, 1.0)
                    : (dhFarDistanceSource == 1
                        ? vec3(1.0, 0.0, 0.0)
                        : (dhFarDistanceSource == 2 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 1.0, 0.0)));
            } else if (dhFarMode == 6) {
                // 距离灰度：近处暗、远处亮（按 DH 的 far 归一）。用来确认深度语义
                // 没有搞反 —— 搞反的话地形会整片压到同一个灰度。
                currentColor = vec3(clamp(dhFarDistance
                    / (dhFarFar > 4.0 ? dhFarFar : 2048.0), 0.0, 1.0));
            } else if (dhFarMode == 10) {
                // 覆盖掩码（回答"某个现象是不是我们涂上去的"）：
                //   白 = 我们在这里用 DH 的内容接管（且重建点在合理世界高度内）
                //   品红 = 接管了、但重建点在 480 格以上 ⇒ **我们在往天空/云里涂**
                //          （出现品红 = 护栏没拦住，或 DH 的图里确实有云）
                //   红 = DH 在这里没有几何，走了"补空气"分支
                // 其余像素 = 原画面压暗 25%，作为参照底。
                currentColor = dhFarPositionIsWorld
                    ? vec3(1.0)
                    : vec3(1.0, 0.0, 1.0);
            } else if (dhFarMode == 11) {
                // 光照诊断（三个量一次成像，回答"远场为什么这么暗/这么亮"）：
                //   R = NdotL（重建法线 · 太阳方向）—— 平地不该明显超过太阳高度角
                //   G = 4.8 那条真实光照路径算出的颜色**去色后的亮度** ——
                //       与近场同一量级才对；全黑 = 新光照根本没求值（档位/门闸不对）
                //   B = 云透过率 —— 低太阳下决定"这片地本来在不在云影里"
                currentColor = vec3(
                    clamp(dhFarNdotL, 0.0, 1.0),
                    clamp(dot(dhFarOurLit, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0),
                    clamp(dhFarCloudTransmittance, 0.0, 1.0)
                );
            } else if (dhFarMode == 12) {
                // =====================================================
                // 12 —— DH 原图直通 + 我们的晴空大气透视（回退档）
                // =====================================================
                // 不重打光。这是 stage-1 形态的**回退开关**：DH 的图原封不动贴上来，
                // 只叠我们的空气。它的历史理由（09-23 第五次定案）是：
                //   旧的"成品图 + 解析重打光"只有"直射近似 + 环境底"，而近场
                //   sampleColor 里还有 GI（indirectLight * 1.8 * rtaoFactor）等
                //   一大堆东西；黄昏太阳低、直射弱时近场靠 GI 撑亮，远场只剩环境底
                //   ⇒ 被压成 (11,13,8) 的极暗绿 —— 用户报的"一大坨黑影"。
                //
                // ⚠ 那个前提**已经不成立**（09-25 第七次定案）：远场 LOD 几何现在
                //    就在 TLAS 里（方案 c，掩码 0xfe），4.8 那条路径给远场算了真正的
                //    直射（含阴影）+ GI + 环境底 ⇒ 档 3/4 不再有"缺 GI"这个结构性
                //    缺口。本档保留只为对照：要回答"某个现象是不是我们点灯引入的"，
                //    在 12 与 4 之间切一次即可（12 全黑/全亮都算正常）。
                //
                // 大气透视仍然做：近场那句挂在 primaryHitDistance > 0.0 上，
                // 远场像素的主射线是 miss ⇒ 不在这里补，远场就永远没有空气，
                // 近场六十格内带雾、几百格外的山反而清清楚楚 —— 接缝看得见。
                // 距离先夹到 [0, dhFarFar]，重建出垃圾值时雾不会把整块涂满。
                currentColor = applyClearSkyAerialPerspective(
                    dhFarLinear,
                    dhFarCameraPos,
                    dhFarRayDir,
                    clamp(dhFarDistance, 0.0, dhFarFar)
                );
            } else if (dhFarPositionIsWorld) {
                // =====================================================
                // 3) 重打光：**我们真正的光照路径**（4.8 已经算好）
                // =====================================================
                // 这里**不再**是"成品图 × 解析近似"，而是 4.8 那段：
                //   直射（含真阴影射线）× 间接（1 条余弦 GI 射线）× 环境底，
                //   albedo 仍是 DH 的图（图像定色），cloud/NdotL 等量取自 4.7。
                // 逐项推导、以及三条**刻意不评估**的项（RTAO / 局部光 / 反射）
                // 都写在 4.8 的注释里 —— 要看"远场为什么是这个亮度"，看那里。
                //
                // ⚠ 旧注释里"阴影项远场拿不到（它不在 TLAS 里）"**已过期**：
                //    方案 c 之后远场 LOD 几何就在 TLAS 里（实例掩码 0xfe ⇒ 相机
                //    不可见、次级射线可见），所以本分支的阴影/GI 射线都能打到它。
                //    mode 11 的 G 通道读的是同一份 dhFarOurLit（读数=成像）。
                float dhFarStrength = clamp(cam.dhFarFieldLight.y, 0.0, 1.0);
                float dhFarGain = max(cam.dhFarFieldLight.z, 0.0);

                // strength = "我们的光占多少"（1 = 完全用我们的光，0 = DH 原图直通），
                // gain     = 整体标度微调（DH 焙进去的光照水平与我们的 albedo 不严格等标）。
                vec3 dhFarLit =
                    mix(dhFarLinear, dhFarOurLit, dhFarStrength) * dhFarGain;

                if (dhFarMode == 4 || dhFarMode == 14) {
                    // 我们的晴空大气透视接管远场，于是近场与远场处在同一套空气里，
                    // 而不是各说各话。（2026-11-19 rasterLod：mode=14 镜像 mode=4
                    // 的雾机制 —— 距离来自本项目光栅深度经同一套重建机器，见上。）
                    dhFarLit = applyClearSkyAerialPerspective(
                        dhFarLit,
                        dhFarCameraPos,
                        dhFarRayDir,
                        dhFarDistance
                    );
                }

                // 走到这里 = 我们的主射线在该像素上没有几何 ⇒ 远场是唯一内容，
                // 直接接管（近场那条 applyClearSkyAerialPerspective 在上面
                // primaryHitDistance > 0.0 的分支里，miss 时不会执行 ⇒ 不会双重雾化）。
                currentColor = dhFarLit;
            } else {
                // =====================================================
                // 4) 护栏拒绝时的兜底：**绝不能把天空色留在屏幕上**
                // =====================================================
                // 走到这里 = DH 有几何，但重建点落到地形高度之外（>480 格或 <-512 格）。
                // 只有两种可能：
                //   ① 矩阵/变体约定不对 ⇒ 距离是垃圾值 ⇒ 位置飞到天上或地下；
                //   ② DH 那张图里画的是**它自己的云**（DH 的 enableCloudRendering，
                //      配置注释原文 "a single layer of clouds, like vanilla Minecraft"）
                //      —— 云被我们当成了地面。
                //
                // ⚠ 旧写法什么都不写 ⇒ currentColor 保持主射线 miss 时的天空色 ⇒
                //    屏幕上一块**纯平、直线边界**的亮斑（就是用户报的"缺口/平板"）。
                //    改成回落 DH 自己的颜色（= stage-1 直通）：
                //    ① 时至少还是远场的画面，② 时就是 DH 的云本身，都不会凭空多出一块亮色。
                currentColor = dhFarLinear;
            }
        }
    } else {
        // =====================================================
        // DH 在这个像素没有几何 —— **什么都不写**
        // =====================================================
        // ⚠ 09-23 第五次定案：**整个"补空气"分支删除**。
        //
        // 旧写法：对朝下（rayDir.y < 0）的视线，用**该方位的地平线天空色**填。
        // 理由是"远距离地表本来就被大气消光成它"。这个理由**是错的**，因为
        // "DH 没有几何"和"那里是天空"是两件毫不相干的事。DH 主动不画 LOD 的
        // 情形至少有两种，而且**两个都随相机高度变化**：
        //
        //   ① `Culling.overdrawPrevention` 近裁剪。我们已把它压到 0.0，但
        //      `RenderUtil.getNearClipPlaneInBlocks()` 会对它做
        //      `clamp(0.05, v, 1.0)` ⇒ 实际 0.05 × (renderDistance×16) = 3.2 格。
        //   ② `RenderUtil.getHeightBasedNearClipOverrideBlockDistance()` ——
        //      **相机高过 `level.getMaxHeight() + 1000` 时**，它返回
        //      `playerY - (maxHeight + 1000)`，并且这个值**整个覆盖掉 ①**
        //      （字节码：override != -1 ⇒ value = override）。
        //      ⇒ 相机越高，DH 在它周围挖的圆洞半径越大（线性增长）。
        //      这就是用户报的"**升到一定高度**就是能看到那个**圆形裁剪区域**"。
        //   ③ 超出 DH 的 LOD 渲染半径，或那一片 LOD 尚未生成。
        //
        // 这些洞**本该由原版/我们的近场几何填满**。我们没有那份几何（近场只覆盖
        // 相机周围 renderDistance 格），所以唯一诚实的做法是**不写**：
        // 让主射线自己的结果留下（近场几何，或它真的 miss 时的天空）。
        // 反过来，往洞里涂"地平线天空色"一定会造出一块**与周围不同的纯平色斑**
        // —— 它的边界正好是 DH 那个圆的边界，于是看起来就像"世界被裁掉了一块"。
        //
        // mode 10：把"DH 无几何"的像素涂红，用来量这个洞到底有多大。
        // （现在红色区域就是洞本身，不再混入补空气的贡献。）
        if (dhFarModeRequested == 10) {
            currentColor = vec3(1.0, 0.0, 0.0);
        }
    }
}
#endif // ORION_K2_TRANSPORT（DH 远场合成，见上）

// 水下水柱放在**最后一次写出之前**，而不是中间累加的地方：DH 远场与调试模式会在
// 那之后整段替换 currentColor，只改累加处会让那些像素漏掉水柱衰减。
// 两个入参都在循环里赋值、在循环外声明（worldDir / primaryHitDistance 本身是
// 循环局部），见 dhFarFieldOursDistance 与 primaryViewDirectionForWater 的声明处。
// =====================================================
// B：K1/K2 输出尾部的归属。
//   K1 只为"没写过 landing"的像素（miss/水/玻璃）执行 —— opaque 像素的
//   currentColor 还没有辐射，写出去就是黑屏；这些量的 K1 份额已经在上面的
//   水柱/累积/DH/guide 各段做完。
//   K2 只为"载入了 landing"的像素（opaque）执行 —— 非 opaque 像素的全部
//   输出归 K1，K2 的 currentColor 恒 0，写出去同样会盖掉 K1 的正确结果。
//   主变体无条件执行（下方的裸 {} 只是加了个作用域，语义不变）。
#if defined(ORION_K1_PRIMARY)
    if(!anyLandingStored)
#endif
#if defined(ORION_K2_TRANSPORT)
    if(ptLandingActive)
#endif
{
currentColor = applyUnderwaterWaterColumn(
    currentColor,
    primaryViewDirectionForWater,
    dhFarFieldOursDistance
);

#if RR_STABLE_GLASS_RADIANCE
// Split the pane's analytic radiance off the RR input here -- the very last
// point before the colour is written -- so it passes through exactly the same
// underwater attenuation as the rest of the pixel and every stage upstream
// still sees the untouched colour.  Subtracting after a multiplicative column
// is identical to subtracting before it.
vec3 stableAttenuated =
    applyUnderwaterWaterColumn(
        rrStableGlassRadiance,
        primaryViewDirectionForWater,
        dhFarFieldOursDistance
    );

imageStore(
    rrStableRadianceImage,
    pixelCoord,
    vec4(
        max(stableAttenuated, vec3(0.0)),
        1.0
    )
);

currentColor =
    currentColor -
    stableAttenuated;
#endif

#if DBG_GLASS_HISTORY_PROBE
// Diagnostic view (see DBG_GLASS_HISTORY_PROBE).  Written as the DLSS-RR input,
// so the read is: red = history never reused, yellow/white = reused (brighter =
// longer history), and a dark blue tint means cam.historyValid was 0.
if(dbgGlassHistoryProbeWanted && dbgGlassHistoryProbe.x >= 0.0)
{
    currentColor = dbgGlassHistoryProbe;
}
#endif

// 2026-10-09 运动矢量调试视图（见文件顶部的说明；**运行期**开关，不走 #define）。
// 只在最终写出处覆盖颜色 ⇒ 完全不动上游光照/材质路径。
// 读的是本像素写进 MV 图的那个值（rtDebugPrimaryMotion），所以它验证的就是真实产物。
if(cam.perfAblationStage.y != 0u)
{
    currentColor = rtMotionDebugColor(
        rtDebugPrimaryMotion,
        cam.perfAblationStage.y
    );
}

#ifdef DBG_GI_TEMPORAL_PROBE
// A1 GI 时域诊断视图（见 rgen_gi_temporal.glsl 顶部）：未调用=品红，
// R=历史接受 G=MV 幅度×40 B=深度失配/容差。
if(g_dbgGiTemporal.x >= 0.0)
{
    currentColor = g_dbgGiTemporal;
}
#endif

#ifdef RT_DEBUG_IDENTITY
// 诊断档：主命中身份直接当颜色写出，绕开光照/自发光/色调映射。
//   R = fract(instanceIndex / 64)  大块连续（同一 section 内缓慢变化）
//   G = fract(primitiveIndex / 64) 每个三角形变化 ⇒ 细密但连续的梯度
//   B = bary.x                     三角形内连续
// 身份正确 ⇒ 地面是"每个三角形一块、片内平滑"的结构；
// 身份为垃圾 ⇒ 五颜六色的随机噪点，没有任何三角形结构。
if(g_debugIdentity.x >= 0.0)
{
    currentColor = g_debugIdentity * 0.35;
}
#endif

#ifdef RT_DEBUG_ALBEDO
// 早退分支编号按 3 bit 画成颜色（每通道 0/1，饱和但可辨）：
//   黑 0 = 走到了 albedo           红 1 = miss 守卫（不该出现）
//   绿 2 = rawAddr == 0            黄 3 = DISTANCE_ONLY
//   蓝 4 = voxy                    品红 5 = voxy 水
//   青 6 = DH 远场                 白 7 = entity
currentColor = vec3(
    float(g_debugBranch & 1u),
    float((g_debugBranch >> 1u) & 1u),
    float((g_debugBranch >> 2u) & 1u)
);
#endif

#ifdef RT_DEBUG_ATLAS
// 诊断档：与命中无关，直接用屏幕坐标把 blockAtlas 整张铺满画面。
// 左上四分之一强制品红 —— 用来验证"这个覆盖点到底有没有生效"：
//   左上不是品红 ⇒ 覆盖点没被执行，之前的 B/空画结论都不作数；
//   左上品红、其余能看到方块贴图 ⇒ binding 3 正确，问题在 UV；
//   左上品红、其余是噪声/纯色 ⇒ rgen 里 blockAtlas 不是方块图集。
{
    vec2 debugUV =
        (vec2(pixelCoord) + 0.5) / vec2(imageSize(image));

    currentColor = textureLod(blockAtlas, debugUV, 0.0).rgb;

    if(debugUV.x < 0.5 && debugUV.y < 0.5)
    {
        currentColor = vec3(1.0, 0.0, 1.0);
    }
}
#endif

// =====================================================
// 2026-10-27 稀疏渲染：非活跃像素不覆盖本帧辐射。
// Remix 的等价行为：integrate 按 compacted 列表启动 ⇒ 非活跃像素的 radiance
// buffer 没有任何线程写；demodulate 的非活跃线程直接 return
// （demodulate.comp.slang:862-874）。RR 因此读到的是"上一次写出"的值，
// 而不是以前那版写进去的 ambient + emissive（有偏的假观测）。
//
// 三个例外（全部来自 Remix）：
//   · cam.sparseParams.y <= 0：稀疏关闭 ⇒ 全写；
//   · cam.sparseParams.w <= 0.5：稀疏历史尚未有效（首帧 / 切档第一帧）⇒ 全写，
//     否则读到的是未初始化内容（g_rtOutputImage 每帧不清零）；
//   · primaryHitDistance < 0：主射线 miss（天空）不稀疏 —— Remix
//     enableSparsePrimaryRayMissComposition 默认 false。
// =====================================================
// =====================================================
// 2026-11-02：改成**恒写**。
//
// 依据 RTX Remix 的分层稀疏（不是"整像素稀疏"）：
//   · GBuffer / depth / MV / normal / roughness / albedo 全分辨率、每帧新写，
//     `isActivePixelFromMask` 只被 compact_active_pixels 用（GBuffer pass 不看 mask）；
//   · 稀疏的只有 lighting lobe，1/rate 也只乘在这些 lobe 上
//     （demodulate.comp.slang:649-659 / 773-781）；
//   · composite 是**全分辨率、每帧重新合成**的 ⇒ 非活跃像素的最终色每帧都更新，
//     只是参与合成的 radiance 是旧的。
//
// 本工程原来是"非活跃像素整像素不写 image"：连自发光 / 天空底 / 环境底一起停在
// 上一帧，而 depth / MV / guide 全是本帧新的 ⇒ RR 把"旧颜色 + 新 MV + 新 guide"
// 当成有效观测重投影，相机一动就是拖影、静止就是亮点累积。
//
// 恒写之后每像素每帧都有本帧样本：活跃像素给完整值，非活跃像素的稀疏项为 0，
// 由 ×1/rate 的补偿在时域上回到正确期望 —— 逐像素无偏，且 MV/guide 与颜色同帧。
// A/B：-DVPT_SPARSE_ALWAYS_WRITE=0 回到旧的"非活跃像素不写"。
// =====================================================
#ifndef VPT_SPARSE_ALWAYS_WRITE
// 2026-11-04 按 RTX Remix 口径回退：非活跃像素**不写**（保留上一帧值）。
// Remix: sparse_rendering.h:38-41 "active pixels store their GBuffer at their
// compacted slot, and inactive pixels store only the per-pixel outputs"。
// 此前恒写，等于每帧往非活跃像素里塞一个"直射/本地光被跳过"的半成品值 ⇒
// 阴影边界（亮暗对比最大处）显形为一条噪带。
// A/B：-DVPT_SPARSE_ALWAYS_WRITE=1 回到恒写。
//
// 2026-11-07 实测（把默认翻成 1 之后）：整屏**偏亮 0.72 log2 ≈ 1.65 倍**。
// 病因不在写不写，而在 1/rate 的施加范围（rgen_rng.glsl:86-96 的
// vptSparseRadianceScale() 是**全局**量，不按 vptPixelActive 门控）：
//   非活跃像素 runDiffuseGI=false（5087）、aoSamples=0 ⇒ rtaoFactor 恒 1（2693）、
//   shadowSamples=0 ⇒ directLight≈0（2955 注释 + 3373 归一化），
//   但 indirectLight 经 giTemporalResolveAndStore 拿到**满幅历史值**（6820），
//   于是 litColor 里 indirectLight*1.8*rtaoFactor*sparseTermScale 被放大 3.33 倍，
//   且丢掉 AO 遮蔽 —— 正是 2967 行早就警告过的"跳过可见性 ⇒ 漏光，1/rate 再放大成发光"。
// 恒写把这个过亮值每帧写进 70% 的像素 ⇒ 画面整体上浮。
// 数据：同一局 A-B-A（20:22-20:25，相机静止，meshSec 1527 对齐）
//   30%: trace 13.25 / avgLogLuma -5.891
//    0%: trace 14.25 / avgLogLuma -6.633
// 结论：ALWAYS_WRITE=1 与"1/rate 只在活跃像素上成立"这一前提互斥，退回 0。
//
// 2026-11-08：用户要求回到 2026-11-02 的稀疏行为，改回 1。
// 11-02 的组合是「恒写 + 蓝噪声独立抽签 + 无边缘抬升」：每个像素每帧都写一个
// 本帧样本（活跃=完整值×1/rate，非活跃=稀疏项为 0），时域平均无偏。
// 上面 11-07 记的 +0.72 log2 过亮是真实现象，代价一并接受（用户明确"不管画质"）。
#define VPT_SPARSE_ALWAYS_WRITE 1
#endif

#if VPT_SPARSE_ALWAYS_WRITE
const bool sparseWriteImage = true;
#else
const bool sparseWriteImage =
    vptPixelActiveOutside ||
    cam.sparseParams.y <= 0.0 ||
    cam.sparseParams.w <= 0.5 ||
    primaryRayMissed;
#endif

if(sparseWriteImage)
{
    imageStore(
        image,
        pixelCoord,
        vec4(
            max(
                currentColor,
                vec3(0.0)
            ),
            1.0
        )
    );
}
} // B：K1/K2 输出尾部作用域（K1 = !anyLandingStored，K2 = ptLandingActive，主变体恒真）
#endif // ORION_K2_DIRECT_STAGE（③：公共尾段/compose/guide/输出归 K2-I）

// RT 计数器：main 末尾统一把本 invocation 的局部射线/材质计数落盘（至多两次
// atomicAdd）。提前 return 的分支不会走到这里，其未flush 的计数不计入窗口汇总。
#ifdef ORION_RT_STAGE_COUNTERS
rtCntFlushRgen();
#endif
}
