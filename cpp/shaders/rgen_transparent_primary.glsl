
vec3 tracePrimaryGenericTransparentRadiance(
    // 入射面命中点（2026-10-05 起回归入射面语义）：镜面反射射线与太阳 GGX
    // 高光的起点。此前电介质模型把出射点传进来，反射线从介质内部出发、
    // 自命中玻璃入射面 ⇒ 表面反射恒为黑（消费者对玻璃命中返回 0）。
    vec3 glassSurfacePosition,
    // 2026-10-05 介质走查的出射点：透射频线的真实起点（平行板 = 真实折射
    // 出射点；侧面 TIR 弹射后的出射点）。旧档把出射点塞进
    // glassSurfacePosition、反射因此自命中 —— 两个语义从此分开。
    vec3 continuationOrigin,
    // 透射频线的出射方向：平行板出射 = 入射方向；侧面 TIR 弹射后 = 弹射
    // 回平行面再出射的方向。薄片 Fresnel 拆分的 N·V 仍用入射面法线与该
    // 方向 —— 平行板出射时与视线逐位一致，侧面出射（掠射少数）为一阶近似。
    vec3 rayDirection,
    vec3 glassNormal,
    vec3 glassTint,
    float glassAlpha,
    // 2026-10-05 粗糙透射/磨砂（#28）：玻璃的 LabPBR 粗糙度（透射方向锥形
    // 抖动的锥角来源；致密体档案 profile ≥ 1 才生效，清玻璃/染色玻璃不受
    // 影响 —— "直透射、不折射"既定要求保留）。
    float glassRoughness,
    int transparentProfile,
    // 2026-11-02 起本参数只在回退档（-DVPT_GLASS_DIELECTRIC_REWRITE=0）里被消费：
    // 重写档下普通玻璃同样走电介质，贴图 alpha 只当覆盖率（不再"当空气"）。
    bool clearGlassAsAir,
    vec3 sunDirection,
    // History key for the behind-glass receiver's local direct light: the
    // camera ray's RAW intersection with the pane, i.e. WITHOUT the POM /
    // parallax travel that glassSurfacePosition carries.  The relief travel is
    // quantised per texture texel, so using it as the key makes the position
    // match fail in the relief's own pattern (measured: isolated failures every
    // ~7.5 px, i.e. one glass texel on screen -- see VPT_TRANSPARENT_HISTORY_KEY).
    vec3 historyKeyPosition,
    #if GI_GLASS_RECEIVER_INDIRECT
    // Camera-to-pane distance.  Only the receiver-GI distance gate below uses
    // it; the pane's own identity never depends on how far the camera is.
    float cameraToGlassDistance,
#endif
    out WaterTransmissionInfo transmissionInfo
#if RR_STABLE_GLASS_RADIANCE
    , out vec3 stableGlassRadiance
#endif
){
#if RR_STABLE_GLASS_RADIANCE
    stableGlassRadiance = vec3(0.0);
#endif

    // ===== DBG_LADDER_A: 玻璃主分支入口可达性 → 亮橙 =====
    if(DBG_LADDER_A) { return vec3(1.0, 0.5, 0.0); }

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

    // 2026-11-02 半透明重写：本函数后半（alpha blend / 玻璃链 / 剖面不透明度 /
    // 清玻璃当空气）在重写档下整段不执行。旧代码保留，供 -DVPT_GLASS_DIELECTRIC_REWRITE=0
    // 逐位回退。普通玻璃同样走这里 —— 贴图 alpha 只当覆盖率。
#if VPT_GLASS_DIELECTRIC_REWRITE
    return tracePrimaryDielectricGlassRadiance(
        glassSurfacePosition,
        rayDirection,
        glassNormal,
        glassTint,
        glassRoughness,
        transparentProfile,
        sunDirection,
        historyKeyPosition,
        transmissionInfo
#if RR_STABLE_GLASS_RADIANCE
        , stableGlassRadiance
#endif
    );
#else

    vec3 straightDirection =
        normalize(
            rayDirection
        );

#ifndef GI_ROUGH_TRANSMISSION
// 2026-10-05 粗糙透射/磨砂（#28）总闸（默认 1）。
#define GI_ROUGH_TRANSMISSION 1
#endif
#ifndef GI_ROUGH_TRANSMISSION_MAX_ANGLE
// 满粗糙度的锥半角（弧度，≈20°）：磨砂玻璃/浑浊冰的后方模糊量。
#define GI_ROUGH_TRANSMISSION_MAX_ANGLE 0.35
#endif
#ifndef GI_ROUGH_TRANSMISSION_MAX_DISTANCE
// 两区块（32 格）距离闸：更远的磨砂不可见，抖动直接跳过（用户授权）。
#define GI_ROUGH_TRANSMISSION_MAX_DISTANCE 32.0
#endif
#if GI_ROUGH_TRANSMISSION
    // 磨砂：致密体档案（profile ≥ 1 = 染色玻璃以外的 tinted/ice/slime/honey）
    // 按 LabPBR 粗糙度把透射方向在锥内抖动。抖动种子**不含 frameCount**
    // ⇒ 磨砂图案逐帧稳定（静态磨砂质感，不闪烁）；同一 ray 的后续使用
    //（continuation 起飞方向）自动继承。清玻璃/染色玻璃 profile 0 ⇒ 逐位旧档。
    if(
        transparentProfile >= 1 &&
        glassRoughness > 0.02 &&
        length(glassSurfacePosition - cam.viewInverse[3].xyz) <=
            GI_ROUGH_TRANSMISSION_MAX_DISTANCE
    )
    {
        uint frostSeed =
            uint(gl_LaunchIDEXT.x) * 1973u ^
            uint(gl_LaunchIDEXT.y) * 9277u ^
            0x9E3779B9u;
        frostSeed = frostSeed ^ (frostSeed >> 16u);
        frostSeed = frostSeed * 0x7FEB352Du;
        frostSeed = frostSeed ^ (frostSeed >> 15u);
        float frostR1 = float(frostSeed & 0xFFFFu) * (1.0 / 65535.0);
        float frostR2 = float((frostSeed >> 16u) & 0xFFFFu) * (1.0 / 65535.0);
        float frostAngle = sqrt(frostR1) *
            (GI_ROUGH_TRANSMISSION_MAX_ANGLE * clamp(glassRoughness, 0.0, 1.0));
        float frostPhi = 6.28318530718 * frostR2;
        vec3 frostT = normalize(
            abs(straightDirection.y) < 0.99
                ? cross(straightDirection, vec3(0.0, 1.0, 0.0))
                : cross(straightDirection, vec3(1.0, 0.0, 0.0)));
        vec3 frostB = cross(straightDirection, frostT);
        straightDirection = normalize(
            straightDirection * cos(frostAngle) +
            (frostT * cos(frostPhi) + frostB * sin(frostPhi)) *
                sin(frostAngle));
    }
#endif

    // 2026-10-05：透射频线从**介质走查的出射点**起飞（glassSurfacePosition
    // 已回归入射面语义，供反射/太阳高光使用）。
    vec3 startOrigin =
        continuationOrigin +
        straightDirection *
        0.01;


    hit.hitDistance =
        1.0;

#ifndef ORION_GLASS_SINGLE_PASS
// 2026-10-25 阶段 D（默认 1）：主射线透射不再"跳过玻璃 + 递归处理第二/三层"，
// 而是像 dxvk-remix 的 geometryPSRResolver（geometry_resolver.slangh:3169-3213）
// 那样：玻璃在遍历里被消耗（any-hit 逐面乘薄片透射），第一个不透明面才是接收面。
// 置 0 = 回到 PASSTHROUGH + 递归旧档。
#define ORION_GLASS_SINGLE_PASS 1
#endif
#if ORION_GLASS_SINGLE_PASS
#if ORION_GLASS_CHAIN_BIT
    // purpose 用 FULL(0)，链条身份由 bit 18 承载（见常量处说明）。
    hit.state = makeTraceState(RT_PURPOSE_FULL) |
        RT_STATE_PRIMARY_TRANSMISSION_BIT;
#else
    hit.state = makeTraceState(RT_PURPOSE_PRIMARY_TRANSMISSION);
#endif
    // 唯一入口：链状态（累乘量 / 介质状态 / 面预算）只在这里初始化。
    // 2026-10-25 事故：这三项曾靠手写播种，漏掉后 any-hit 读到垃圾预算，
    // 第一片玻璃被当遮挡接受 ⇒ 玻璃变不透明。现在只有一个入口。
    glassChainBegin(hit);

// 2026-10-28：本守卫必须包含 SURFACE_RADIANCE —— 那条宏守卫着 update() 调用
// 与 77 号图写入，而 reset 只挂 DEFERRED。两者不同值时会出现
// "reset 每帧把 74 号图写成 invalid、update 被剔除" 的半死状态，
// 整条 alpha blend 链静默空转（症状：改任何参数画面都不变）。详见共享头注释。
#if VPT_GLASS_ALPHA_BLEND && VPT_GLASS_ALPHA_BLEND_DEFERRED_COMPOSITE && VPT_GLASS_ALPHA_BLEND_SURFACE_RADIANCE
#if VPT_GLASS_DETERMINISTIC_CHAIN
    // =====================================================
    // 2026-10-31（第五轮）：确定性链起点 —— 契约见 glass_chain_common.glsl。
    // 只写骨架，不写层内容：
    //   74.x = 1 − a0（最近层的背景透射率已计入，深层权重因此天然正确）；
    //   74.y 保持 0（无效）—— 最近层内容在 trace 返回后回填；清玻璃早退
    //   路径走不到回填 ⇒ 天然不产生合成（10-28 双重合成教训的守护）。
    // a0 与两个 rahit 的 chainAlpha 同式（GLASS_FIXED_ALPHA 与剖面表取大）。
    // 链射线剔除背面 ⇒ 每个玻璃体只有入射面进 any-hit，无需出入面配对。
    // 抽签口径（蓝噪声阈值 + lane 5/6 排查档）完整保留在 #else 分支。
    // =====================================================
    glassAlphaBlendChainResetDeterministic(
        ivec2(gl_LaunchIDEXT.xy),
        max(GLASS_FIXED_ALPHA,
            1.0 - clamp(glassChainProfileAlpha(uint(transparentProfile)),
                0.0, 1.0)));
#else
    // 原文 geometry_resolver.slangh:3238：链起点处
    //   alphaBlendSurface.randomThreshold = getNextSampleBlueNoise(randomState);
    // 本引擎没有蓝噪声纹理 ⇒ 用同源整数哈希（等价物：同分布、逐帧变化，
    // 但不是同一张蓝噪声图；换真蓝噪声是后续项，已记录在文档）。
    {
        // 原文 geometry_resolver.slangh:3238 的 randomThreshold 取值。
        // randomState 来自 createRNG(pixelIdx, temporalIdx, sampleOffset)，
        // 其中 rng.seed 初值 = temporalIdx / RNG_SEED_FRAME_RANGE + sampleOffset
        // （noise.slangh:177）。本引擎每条主射线只有 1 个样本 ⇒ sampleOffset = 0。
        const uint alphaBlendNoiseSeed =
            cam.frameCount / 64u;

        float alphaBlendThreshold =
            glassBlueNoiseSample(
                uvec2(gl_LaunchIDEXT.xy),
                cam.frameCount,
                alphaBlendNoiseSeed);

        // =====================================================
        // 2026-10-28：选层抖动的**逐项排查档**（免重编，标记文件驱动）
        //
        // randomThreshold 逐帧变化 ⇒ update() 每帧可能选中不同的一层
        // （alpha_blend_surface.slangh:144）。Remix 靠 RR 的时域累积吃平，
        // 我们这边的玻璃像素 guide 在 pane / receiver 之间切换，时域不一定收敛。
        // 把阈值钉死后：若玻璃面上的颗粒消失 ⇒ 根因就是选层抖动。
        //
        //   glass_layer_first.flag ⇒ lane 5 : threshold = 0
        //        ⇒ 0 >= 1 - bgT 只在 bgT >= 1 时成立 ⇒ 恒选第一层（确定性）
        //   glass_layer_last.flag  ⇒ lane 6 : threshold = 1
        //        ⇒ 1 >= 1 - bgT 恒成立 ⇒ 恒选最后一层（确定性，另一种对照）
        // =====================================================
        if(cam.perfAblationStage.w == 5u)
        {
            alphaBlendThreshold = 0.0;
        }
        else if(cam.perfAblationStage.w == 6u)
        {
            alphaBlendThreshold = 1.0;
        }

        glassAlphaBlendSurfaceReset(
            ivec2(gl_LaunchIDEXT.xy),
            alphaBlendThreshold);
    }
#endif
#endif

    // =====================================================
    // 2026-10-26：起点可能**仍在入射玻璃体内** —— 这是"玻璃/冰不透"的第二个机制。
    //
    // glassSlabExitPoint() 为了防"透过玻璃穿墙"会把出射点夹回入射格
    // （距远面只剩 0.05 格，见 glass_dielectric_common.glsl 的推导），再加 0.01
    // 的起点偏移后，射线其实还在这一格里。于是链遇到的**第一个面是同一块玻璃的
    // 出射面**，却被状态机当成"进入面"又乘了一次 ⇒ 单块玻璃被吸收两次
    // （致密体档案的 profile 衰减同样乘两次）⇒ 观感明显不透。
    //
    // 判据：几何光程 thickness/cosT 比"入射面 → 实际起点"的距离长 ⇒ 起点在体内。
    // 只在真的在体内时把链初始化成"已在介质中"：第一个面按出射面跳过，
    // 之后其它玻璃体的进入面照常乘一次。
    // =====================================================
    {
        const float chainSlabThickness =
            glassSlabThicknessFromFace(glassSurfacePosition, glassNormal);
        const float chainCosInside =
            max(abs(dot(normalize(glassNormal), straightDirection)), 1.0e-4);
        const float chainGeomExitDistance =
            chainSlabThickness / chainCosInside;
        const float chainStartDistance =
            length(continuationOrigin - glassSurfacePosition);

        if(chainGeomExitDistance > chainStartDistance + 0.02)
        {
        }
    }
#else
    hit.state = makeTraceState(RT_PURPOSE_PASSTHROUGH);
#endif


#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
    serTraceRay(
        startOrigin,
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
            startOrigin,
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
            startOrigin,
            0.01,
            straightDirection,
            10000.0,
            0
        );
    }
#endif


    vec3 behindRadiance =
        vec3(0.0);
    // 2026-10-26：链终止在天空（miss）时背景自己就是光源，
    // 不能再做"behindRadiance / 接收面 albedo"的辐照度还原。
    bool behindReachedSky = false;

#if ORION_GLASS_SINGLE_PASS
#ifndef ORION_GLASS_CHAIN_DEBUG
// 1 = 链的"漏面自检"直接画品红（本该永远看不到）。
#define ORION_GLASS_CHAIN_DEBUG 0
#endif
#if ORION_GLASS_CHAIN_DEBUG
    // 链只应在"不透明面 / 天空"上结束。停在半透明体上 = any-hit 漏吃了一个面，
    // 也就是 2026-10-25"玻璃不透"的形态。画品红比截图猜快。
    if(hit.hitDistance >= 0.0 &&
       glassChainIsTranslucentMaterial(getLegacyMaterialId(hit.state)))
    {
        return vec3(1.0, 0.0, 1.0);
    }
#endif

    // 必须在这里取走：下面 evaluateDirectSunLighting / RIS 都会复用同一个 payload。
    // 第一片玻璃不在这里（由 main 的 glassTransmission 负责），所以只含 2..N 片。
    vec3 paneChainTransmittance =
        clamp(vec3(1.0), vec3(0.0), vec3(1.0));
    // 2026-10-26：链内各层自己的 alpha blend 贡献（见 hit_payload.glsl）。
    // 与上面同一处取走 —— 后面任何 traceRay 都会覆盖 payload。
    vec3 paneChainSurfaceWeight =
        max(vec3(0.0), vec3(0.0));
#else
    vec3 paneChainTransmittance = vec3(1.0);
    vec3 paneChainSurfaceWeight = vec3(0.0);
#endif


    if(payload.hitDistance < 0.0)
    {
        // Dedicated -4 miss path returns real sky radiance.
        behindRadiance =
            unpackAlbedoUnorm(payload.albedoPacked);
        behindReachedSky = true;
    }
    else
    {
        float hitDistance =
            payload.hitDistance;
            // =================================================
        // 2026-10-10：「透过玻璃/半透明看接收面」这条路径不再吃 POM 浮雕位移。
        //
        // 现象：透过彩色玻璃看**另一个**玻璃/半透明体（或看冰面下的东西）时，
        // 画面呈**逐格结节**，"形状跟 POM 差不多"。
        //
        // 机制：这里拿到的是玻璃后那个接收面的世界点，而它原先把接收面**自己的**
        // POM 位移行程（payload.parallaxRayTravel，逐纹理 texel 量化、上限 0.2 格）
        // 加了进去。玻璃会把这段亚像素级的量化**放大着**呈现出来 ⇒ 逐 texel 的结节。
        // 同一类口径错误今天已修过一处（历史身份判据，见 A-3）：**POM 位移只属于
        // 着色，不该进几何推导链**。这里就是这条路径上的另一处。
        //
        // 走查里第二层及以后的界面**不再重算板厚**（glassSlabThicknessFromFace 全
        // 文件只有 3 个调用点，主路径两处用的都是不含 POM 的命中点）⇒ 逐层折射不是
        // 元凶，剩下的就是这一处。
        //
        // 代价：隔着玻璃/水看浮雕面（砖墙纹理等）时，浮雕的立体起伏会变平一些。
        // 回退：-DGLASS_RECEIVER_NO_POM=0 ⇒ 逐位回到改动前。
        // =================================================
// 2026-10-10 **回退**：本开关曾默认 1（把"透过玻璃看到的接收面"的 POM 位移清零），
// 实测导致"玻璃全是噪点" ⇒ 立刻退回默认 0（逐位回到改动前的行为）。
// 教训与 A-3 那条相同：**接收面的位置量同时被历史键/判据链消费**，
// 单独改一处就会让两侧口径不一致 ⇒ 历史全拒 ⇒ 1 spp ⇒ 满屏噪点。
// 保留开关本身，便于日后在**探针确认**之后再做 A/B。
#ifndef GLASS_RECEIVER_NO_POM
#define GLASS_RECEIVER_NO_POM 0
#endif

        float hitParallaxRayTravel =
    max(
        payload.parallaxRayTravel,
        0.0
    );

#if GLASS_RECEIVER_NO_POM
        hitParallaxRayTravel =
            0.0;
#endif


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
    startOrigin +
    straightDirection *
    (
        hitDistance +
        hitParallaxRayTravel
    );


// =====================================================
// 2026-10-17（TRANSPARENT_HISTORY_KEY_FIRST_SURFACE 的续篇）：**法线键**也必须
// 与位置键同源 —— 用相机可见的**第一层**透明面法线，而不是等撞到接收器时才写。
//
// 10-17 已把位置键（history key position）一路换成第一层透明面命中点，理由是
// "同一个第一层表面点唯一决定它后面那条链的接收面"。同一条论据对**身份法线**同样
// 成立，但当时只补了位置：
//
//   · 单层路径（玻璃 → 不透明接收器）：原本要走到下面 if(hitMaterialId <= 0.5)
//     分支才写 vptTransparentSurfaceNormal ⇒ 有玻璃面法线，判据稳定；
//   · 嵌套路径（玻璃 → 水面 / 玻璃 → 第二片半透明界面 → 第三面）：走 14337
//     （水）或 14405（玻璃）分支，**从不写**这份法线 ⇒
//     evaluateVPTCandidateDirectTemporal 的 VPT_TRANSPARENT_HISTORY_PANE_NORMAL
//     判据回落到"接收面几何法线"。而相机是**穿过折射**看到那个接收面的，
//     视向微动被放大成"换了一格面" ⇒ 判据逐帧假否定 ⇒ 历史按
//     VPT_TRANSPARENT_HISTORY_NORMAL_DECAY 每帧减半 ⇒ 等效 1~2 spp 白噪。
//     这就是"透过第一层没事、透过第二个半透明面看第三面有噪点"的机制。
//
// 统一提到分支之前写一次：入参与原 14247 那处**完全相同**（historyKeyPosition +
// straightDirection，且此刻磨砂抖动已经应用），所以单层路径逐位不变；只有原本
// 拿不到法线的嵌套路径第一次拿到它。
// A/B：-DTRANSPARENT_HISTORY_PANE_NORMAL_NESTED=0 ⇒ 回到"只在接收器分支里写"。
// =====================================================
#ifndef TRANSPARENT_HISTORY_PANE_NORMAL_NESTED
#define TRANSPARENT_HISTORY_PANE_NORMAL_NESTED 1
#endif
#if TRANSPARENT_HISTORY_PANE_NORMAL_NESTED
vptTransparentSurfaceNormal =
    vptGeometricFaceNormalFromRay(historyKeyPosition, straightDirection);
#endif


// =====================================================
// 2026-10-27（严格搬，algorithm/resolve.slangh:471-479）
//   if (opacity >= cb.resolveOpaquenessThreshold)  // 254/255
//   { resolveVertexFinalHit(...); return; }         // 按**不透明表面**完整着色
// 本轮以前这里只做了"不记录、当作遮挡返回"，于是这类面落到 rgen 的
// `hitAlbedo * ambient` 兜底 —— 那不是原文的 final hit。
// 判据用 glassBlendAlpha（贴图 alpha x 顶点色 alpha，见
// material_resolve_glass.glsl:1548）；**不能**用 glassAlpha：它对整个玻璃族
// 恒为 1.0（那只表示"覆盖率"）。
// =====================================================
#ifndef GLASS_ALPHA_BLEND_MAX_ALPHA
#define GLASS_ALPHA_BLEND_MAX_ALPHA 0.3
#endif
// 2026-10-27：判据必须用**钳制后**的 alpha，与两个 rahit 的 chainAlpha 同口径。
//   0.0 是贴图原始值，边框 texel 为 1.0；而侧面掠射时
//   边框铺满整个投影 ⇒ 整个侧面都被判成"不透明表面"走完整着色 ⇒ 侧面实。
//   钳制后（<= MAX_ALPHA < 254/255）任何玻璃面都不会再走这条分支。
#ifndef GLASS_BEHIND_MIN_RADIANCE
// 透过玻璃看到的背景辐射下限（见 deferred 分支 return 处的说明）。
#define GLASS_BEHIND_MIN_RADIANCE 0.04
#endif

// 2026-10-27：玻璃族一律走玻璃路径（见文件顶部补丁说明）。
//
// 原判据 min(glassBlendAlpha, MAX_ALPHA) >= 254/255 依赖贴图 texel 的 alpha：
// 玻璃贴图的边框 texel 是 1.0，紧贴墙、正面对着看时整面都落在边框上
// ⇒ 整个面被判成"不透明表面"做完整着色 ⇒ 一片实心色。
// 这正是"45° 看是透的、正对内部看邻面是不透的"—— 角度变了，采到的 texel 变了。
const bool hitIsOpaqueGlass = false;

if(hitMaterialId <= 0.5 || hitIsOpaqueGlass)
{
    transmissionInfo.receiverWorldPos = hitPos;
    transmissionInfo.receiverNormal = hitNormal;
    transmissionInfo.receiverViewDirection = -straightDirection;
    transmissionInfo.receiverAlbedo = hitAlbedo;
    transmissionInfo.receiverRoughness = hitRoughness;
    transmissionInfo.receiverF0Metal = hitF0Metal;
    transmissionInfo.reachedUnderwaterReceiver = 1.0;
    transmissionInfo.waterSunVisibility = 1.0;

    // 2026-09-30 收口：ReSTIR DI 链已删除，玻璃接收面与其他路径统一，
    // 直接叠加 Emitter-adjacency glow（保留 hitEmission）。
    vec3 unifiedHitEmission =
        hitEmission +
        evaluateVLIEmitterRadianceAtHit(
            hitPos,
            hitNormal,
            hitAlbedo
        );


    // 2026-10-09：同上面二手 GI 那一处 —— 内联底光 + 末地银河环境增量。
    vec3 ambientRadiance =
                vec3(clamp(
                    cam.cameraParams.w,
                    0.0,
                    1.0
                )) + endGalaxyAmbientLift();


            vec3 directAndEmission =
                evaluateOpaqueSurfaceRadiance(
                    hitPos,
                    hitNormal,
                    -straightDirection,
                    hitAlbedo,
                    hitRoughness,
                    hitF0Metal,
                    unifiedHitEmission,
                    hitPOMSelfShadow,
                    sunDirection
                );


// Glass receivers use the same Binding 31 Section-directory RIS path as a
// normal opaque primary hit.  Binding 29's legacy Top-8 list could omit an
// otherwise visible nearby emitter and incorrectly return black behind glass.
// The RIS proposals remain cheap; only the selected emitter performs the one
// final GLASS_VISIBILITY hardware trace.
//
// They also get the same screen-space history.  Using the bare RIS estimator
// here -- with the opaque pixel one block away averaging over
// VPT_SCREEN_MAX_HISTORY frames -- is what made everything behind a pane read
// as noise.
// ===== DBG_LADDER_B: VPTRIS 调用点可达 → 纯青 =====
if(DBG_LADDER_B) { return vec3(0.0, 1.0, 1.0); }

// =====================================================
// Ablation 2026-09-25 18:0x - "sparse bright specks behind glass".
//
// The screenshot shows isolated white/coloured points on a dark ground, not
// the soft grain a weak denoiser leaves.  That shape means large-magnitude
// samples drawn rarely, i.e. an ESTIMATOR with huge variance, not a guide or
// filter problem -- which is why the guide work (route A) changed nothing.
//
// behindRadiance is the sum of four terms and only one of them is averaged by
// NRD.  Each switch below kills exactly one, so a single run localises the
// specks:
//
//   DBG_KILL_BEHIND_SUN      - directAndEmission (sun + emission, no estimator)
//   DBG_KILL_BEHIND_LOCAL    - exists above (8-frame VPT RIS)
//   DBG_KILL_BEHIND_AMBIENT  - exists above (albedo * ambient, no estimator)
//   DBG_KILL_BEHIND_GI       - the one cosine-weighted bounce, 1-spp, and it
//                              is added AFTER NRD has already consumed the
//                              pixel: no switch above can remove it.
//
// If the specks survive every switch above, the GI bounce owns them.
// =====================================================
const bool DBG_KILL_BEHIND_SUN = false;      // 2026-10-28 临时判决
// Build-time ablation switch (see the banner above): -DDBG_KILL_BEHIND_GI_MACRO=1
// removes the 1-spp cosine bounce the receiver adds AFTER NRD has already
// consumed the pixel -- the one term no denoiser can reach.
// ⚠️ 2026-10-01 实测记录（本开关当前是**惰性**的，别再用它做消融）：
// 它守护的那条 1spp 余弦弹射（tracePrimaryWaterReceiverIndirectReworked）整段
// 位于 `#if GI_GLASS_RECEIVER_INDIRECT`（默认 0，见 10657 处的说明）之内 ⇒
// **根本不参与编译**。实测：把本宏 0 → 1 重新编译，SPV 与之前**逐字节相同**
// （MD5 未变）——这就是证据。所以"玻璃后只有三项"：
//     behindRadiance = directAndEmission + localEntityIrradiance + ambientTerm
// 其中**唯一的估计量**是 localEntityIrradiance
// （evaluateTransparentReceiverDirectLighting，8 帧屏幕空间 VPT 历史）。
// 要定位剩下三项谁带颗粒，用 DBG_BEHIND_SPLIT_MACRO（分色），不是本开关。
#ifndef DBG_KILL_BEHIND_GI_MACRO
#define DBG_KILL_BEHIND_GI_MACRO 0
#endif
const bool DBG_KILL_BEHIND_GI = (DBG_KILL_BEHIND_GI_MACRO != 0);

// Per-term glass readout for the speck hunt (see the return site below):
// R = transmitted, G = mirror reflection, B = analytic sun gloss.
// -DGLASS_DEBUG_SPLIT=1 paints that triple over the whole screen.
#ifndef GLASS_DEBUG_SPLIT
#define GLASS_DEBUG_SPLIT 0
#endif

// 2026-09-30 收口：ReSTIR DI 链已删除，玻璃接收面直射恒走经典 8 帧
// 屏幕空间估计 evaluateTransparentReceiverDirectLighting。
// 透明面（玻璃片）自身的法线：它落在主射线上，是与接收面无关的静止几何面，
// 拿它做历史身份判据 —— 见 VPT_TRANSPARENT_HISTORY_PANE_NORMAL。
// 2026-10-17：本行的设置已统一提到分支之前（见 TRANSPARENT_HISTORY_PANE_NORMAL_NESTED），
// 嵌套路径才能同样拿到第一层玻璃面法线。只在 A/B 关掉那一档时保留旧位置。
#if !TRANSPARENT_HISTORY_PANE_NORMAL_NESTED
vptTransparentSurfaceNormal =
    vptGeometricFaceNormalFromRay(historyKeyPosition, straightDirection);
#endif
#ifndef ORION_GLASS_RECEIVER_ESTIMATOR_V2
// 2026-10-25 阶段 A 第 1 条（默认 0 = 旧档；A/B 时置 1）：
//   0 = evaluateTransparentReceiverDirectLighting（8~96 帧世界空间 VPT 历史）
//   1 = 主不透明面用的 evaluateVPTRISDirectLighting（每帧 RIS + 动态光源）
//
// 为什么值得再试一次（14707-14710 那段注释说"裸 RIS 在这里是噪点"）：
// 当时接收面既没有 depth guide、也没有 MV（RR 只能按玻璃面的历史重投影），
// 裸 RIS 当然只能是一层颗粒。现在接收面已经有：
//   · depthImage 覆盖（24357 起）；
//   · albedo / normal / roughness guide（20119-20206）；
//   · 与深度一致的 MV（-DORION_GLASS_RECEIVER_MV，2026-10-25 落地）；
// 也就是它终于是个"正常表面"，RR 能自己时域累积 ⇒ 这一条才有资格重新 A/B。
// 若开 1 后噪点反而变差，说明 RR 仍未把它当表面处理，回 0 即可。
#define ORION_GLASS_RECEIVER_ESTIMATOR_V2 1
#endif

#if ORION_GLASS_RECEIVER_ESTIMATOR_V2
#ifndef ORION_GLASS_RECEIVER_RIS_SAMPLES
// 每帧独立样本数（rngState 链式推进 ⇒ 样本互不相关）。默认 1 = 与主不透明面一致。
#define ORION_GLASS_RECEIVER_RIS_SAMPLES 1
#endif
    uint receiverRisState =
        uint(gl_LaunchIDEXT.x) * 1973u +
        uint(gl_LaunchIDEXT.y) * 9277u ^
        cam.frameCount * 26699u;
    pcg_hash(receiverRisState);

    vec3 localEntityIrradiance = vec3(0.0);
    for(int receiverSample = 0;
        receiverSample < ORION_GLASS_RECEIVER_RIS_SAMPLES;
        ++receiverSample)
    {
        // f0 = vec3(-1.0) ⇒ 移动光源退回纯 Lambert（与 3603 的 4 参重载同口径）；
        // 方块光那条 RIS 链完全不受影响，与主不透明面调用的是同一个函数。
        localEntityIrradiance +=
            evaluateVPTRISDirectLighting(
                hitPos,
                hitNormal,
                hitAlbedo,
                vptGeometricFaceNormalFromRay(hitPos, straightDirection),
                vec3(-1.0), -1.0, 1.0,
                receiverRisState);
    }
    localEntityIrradiance *= 1.0 / float(ORION_GLASS_RECEIVER_RIS_SAMPLES);
#else
vec3 localEntityIrradiance =
    evaluateTransparentReceiverDirectLighting(
        // History key: the camera ray's raw pane intersection (no POM travel).
        historyKeyPosition,
        // Geometric face of the receiver: bounds the local-light cosine by the
        // real face instead of the relief texel normal.
        vptGeometricFaceNormalFromRay(hitPos, straightDirection),
        hitPos,
        hitNormal,
        hitAlbedo,
        0u
    );
#endif


            // ===== 判决实验 BEHIND（2026-09-22 20:50）=====
            // 把玻璃后接收面的三项分别标色，一次看清天花板白光由谁主导：
            //   R 通道 = directAndEmission        （payload.emission + 太阳/环境）
            //   G 通道 = localEntityIrradiance    （本地光 RIS，有玻璃染色）
            //   B 通道 = ambient 底光             （纯 albedo 环境，无染色）
            vec3 ambientTerm = hitAlbedo * ambientRadiance;

            // =====================================================
            // 2026-10-28：behind 三项的**运行时逐项杀掉**（免重编）
            //
            // 背景：2026-10-28 之前那次"杀 localEntityIrradiance 无变化"的判决
            //   是在 alpha blend 死链（SURFACE_RADIANCE=0）上做的 —— 那时玻璃像素
            //   根本没进这条分支，判决无效。链路修好后必须重做。
            //
            // perfAblationStage.w（标记文件 ⇒ lane）：
            //   8  = 只杀 localEntityIrradiance（本地光 RIS + 8 帧历史，**唯一估计量**）
            //   9  = 只杀 directAndEmission（太阳直射 + 自发光，解析、无估计量）
            //   10 = 只杀 ambientTerm（albedo × 环境底光，查表、无估计量）
            //   11 = 三项全杀（behindRadiance 恒 0）
            //
            // 判据：颗粒消失的那一档就是载体。
            // =====================================================
            const uint behindKillLane = cam.perfAblationStage.w;

            if(DBG_KILL_BEHIND_SUN || behindKillLane == 9u || behindKillLane == 11u)
            {
                directAndEmission = vec3(0.0);
            }
#if DBG_BEHIND_SPLIT_MACRO
            {
                // 拆到底：R=自发光 G=纯太阳直射(遮挡后) B=环境底光。
                // 玻璃在室内、室内无太阳 ⇒ G 应为 0。若 G 闪/非零 ⇒
                // traceSunTransmittanceThroughWater 的遮挡在室内失效。
                // ⚠ 必须用 #if 而不是 if(const bool)：evaluateDirectSunLighting
                // 内部有 trace（副作用），编译器无法消除，会污染 payload。
                vec3 sunOnly =
                    evaluateDirectSunLighting(
                        hitPos,
                        hitNormal,
                        -straightDirection,
                        hitAlbedo,
                        hitRoughness,
                        hitF0Metal,
                        hitPOMSelfShadow,
                        sunDirection
                    );
                // 2026-10-01：把 R 通道换成**唯一的估计量** localEntityIrradiance
                // （本地光 RIS + 8 帧屏幕空间 VPT 历史）。原来的 R=自发光 挪到 G，
                // 太阳+环境底光并到 B。理由：用户报"颗粒与采样数/候选数无关、且
                // 降噪链换了个遍都没用"，那三项里只有 R 是每帧随机、需要时域平均的
                // 估计量；先看它是不是颗粒的载体。
                //   R = localEntityIrradiance   （估计量：RIS + 8 帧历史）
                //   G = 自发光 unifiedHitEmission（无估计量）
                //   B = 太阳直射(遮挡后) + 环境底光（无估计量）
                return vec3(
                    dbgTermLuma(localEntityIrradiance),
                    dbgTermLuma(unifiedHitEmission),
                    dbgTermLuma(sunOnly + ambientTerm)
                );
            }
#endif
            if(DBG_KILL_BEHIND_AMBIENT ||
               behindKillLane == 10u || behindKillLane == 11u)
            {
                ambientTerm = vec3(0.0);
            }
            if(DBG_KILL_BEHIND_LOCAL ||
               behindKillLane == 8u || behindKillLane == 11u)
            {
                localEntityIrradiance = vec3(0.0);
            }
            if(DBG_KILL_BEHIND_DIRECT)  { directAndEmission = vec3(0.0); }

            behindRadiance =
                directAndEmission +
                localEntityIrradiance +
                ambientTerm;

#if GI_GLASS_RECEIVER_INDIRECT && !defined(ORION_K1_GLASS_ONLY)
            // The receiver is an ordinary opaque surface, so give it the same
            // single cosine-weighted bounce the water receiver has always
            // added.  Without it the receiver keeps only its direct sun and the
            // albedo * noDirectLightAmbient floor, which is why a corridor wall
            // behind a pane is black while the same wall seen directly is lit
            // by the bounce off the sunlit floor.
            //
            // `false` = start OUTSIDE water.  A pane is air, not a medium, and
            // a walker that starts inside water would run the whole bounce -
            // and its sky term - through Beer-Lambert water absorption.
            if(!DBG_KILL_BEHIND_GI)
            {
            behindRadiance +=
                tracePrimaryWaterReceiverIndirectReworked(
                    hitPos,
                    hitNormal,
                    hitAlbedo,
                    cameraToGlassDistance,
                    sunDirection,
                    false
                );
            }
#endif
        }
        else if(
            hitMaterialId > 0.5 &&
            glassChainIsTranslucentMaterial(hitMaterialId)
        )
        {
            // 2026-10-26：这里原来写 `< 2.5`，而半透明面的 id 是 3.0/4.0
            // （见 glass_chain_common.glsl 的说明）⇒ 它们会掉进最后的
            // `else { behindRadiance = hitAlbedo; }`，表现为"纯色实心"。
            // 现在并入介质分支：按界面继续透射，而不是当成无光照 albedo。
            //
            // A camera ray through glass has reached real water.  Do not
            // return water's base texture (which makes triangle-shaped
            // patches); cross this interface and continue with the water
            // transport instead.
            bool exitingWater =
                hitMaterialId > 1.5;

            float eta =
                exitingWater
                    ? 1.33
                    : (1.0 / 1.33);

            vec3 refractedDirection =
                refract(
                    straightDirection,
                    hitNormal,
                    eta
                );

            bool totalInternalReflection =
                dot(
                    refractedDirection,
                    refractedDirection
                ) < 1.0e-6;

            if(
                totalInternalReflection
            )
            {
                refractedDirection =
                    reflect(
                        straightDirection,
                        hitNormal
                    );
            }

            WaterTransmissionInfo nestedWaterInfo;
#if defined(ORION_K1_GLASS_ONLY)
            behindRadiance = hitAlbedo * getSurfaceAmbientRadiance();
#else
            behindRadiance =
                tracePrimaryTransparentRadiance(
                    totalInternalReflection
                        ? hitPos +
                            hitNormal *
                            0.001
                        : hitPos -
                            hitNormal *
                        0.001,
                    normalize(
                        refractedDirection
                    ),
                    totalInternalReflection
                        ? exitingWater
                        : !exitingWater,
                    sunDirection,
                    false,
                    0.0,
                    // 历史键仍用**相机打到的第一层**（本片玻璃），不用水面点：
                    // 水面在玻璃后、还带折射横移，投影不在本像素上。
                    historyKeyPosition,
                    false,
                    // 2026-11-16：透射瓣（行为与旧档逐位一致）。
                    false,
                    nestedWaterInfo
                );
#endif

            transmissionInfo = nestedWaterInfo;
        }
        // 2026-10-25 清理：原 "hitMaterialId >= 2.5"（第二层玻璃面）递归回退已删除。
        // 阶段 D 起玻璃在任何命中之前就被 any-hit 消耗（逐面累乘透射率），
        // closest hit 只可能是不透明面或水 ⇒ 该分支永远不可达。
        else if(glassChainIsTranslucentMaterial(hitMaterialId))
        {
            // 走到这里说明介质分支被跳过（水/玻璃但几何状态异常）：
            // 至少不要退化成"无光照 albedo"，用环境项给一个中性结果。
            behindRadiance =
                hitAlbedo * getSurfaceAmbientRadiance();
        }
        else
        {
            behindRadiance =
                hitAlbedo;
        }
    }

    // 2026-10-27：上一版的"背景辐射下限"已撤回 —— 用户指出玻璃紧贴墙，
    // 背景根本不是天空，behindRadiance 不为 0，该假设不成立。

    // =====================================================
    // 2026-10-07（用户要求）：普通玻璃「当空气」= 保留原版纹理，只去折射与反射
    // =====================================================
    // 走查已经把"穿过玻璃看到的景象"累积进 behindRadiance（走查用 PASSTHROUGH，
    // 忽略一切玻璃 ⇒ 这份辐射本来就是穿过去的景象）。普通玻璃在这里收尾：
    //
    //   ✔ 纹理保留 —— 用 sprite × 顶点色当**原版透明层**的叠加色画在背景上
    //      （玻璃板的花纹/边框、玻璃块的条纹），见下面 2026-10-08 那条。
    //   ✘ 折射 —— 平板出射点位移已在调用点关掉（glassWalkExitPosition = 命中点），
    //      透射频线从原处沿原方向起飞。
    //   ✘ 反射 —— 那份 F0=0.04 的确定性镜面射线（含解析天空短路）与太阳 GGX 高光
    //      都不再执行。
    //
    // 刻意不走 glassThinSheetFactorsScalarF 的菲涅耳拆分：那一步会把 4% 能量划给
    // 反射，而这里没有反射 ⇒ 能量应当全部透过（少掉的那 4% 提亮正好落在透射里）。
    //
    // transmissionInfo 已由走查填好（玻璃后接收面的本地光与 RR guide 照旧消费），
    // 两个 lobe 权重按"纯透射"导出，供主分支的 guide 策略使用。
    //
    // 2026-10-08（用户要求）：旧档是"背后景象 × 贴图"（把 glassTint 当透射率乘上
    // 去），贴图的 RGB 会给背后景象整体压一层色。原版 glass 走
    // RenderLayer.translucent，用的是 glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)，
    // 即 颜色 = 背景×(1−a) + 贴图×a —— 贴图自己的 alpha 决定它盖住背景多少，
    // alpha=0 的 texel 完全不动背景。这里换成同一条公式：边框/条纹按原样画上去，
    // 中间透明处背景保持原色。
    //
    // glassAlpha 就是 sprite 的 alpha（rt_test_translucent.rchit 里
    // hit.glassAlpha = clamp(albedoColor.a,0,1)）。这条 mix 对"贴图是否预乘
    // alpha"两种图集口径都成立：预乘时 tint=色×a ⇒ 退化成同一个算式。
    // computeGlassBlockTransmission 在本档不再被调用（它的染色 pow 只服务染色玻璃）。
    //
    // 开关：-DGI_CLEAR_GLASS_AS_AIR=0 时 isClearGlassHit() 恒 false ⇒ 逐位回到
    // 旧档（含反射/菲涅耳的完整电介质）。染色玻璃走下面那条路，两处都不受影响。
    if(clearGlassAsAir)
    {
#if GI_GLASS_DOMINANT_GUIDE
        glassLobeReflectionWeight = vec3(0.0);
        glassLobeTransmissionWeight = vec3(1.0);
        glassLobeWeightsValid = true;
#endif
        // =================================================
        // 2026-10-10（方案 d）：加性项必须是**受光照的**贴图色。
        //
        // 原版 RenderLayer.translucent 的精灵色在混合前会乘上 lightmap（方块光+天光），
        // 所以暗房间里的玻璃混上去的是"暗的精灵色"。这里此前混的是**生贴图色**
        // ⇒ 玻璃板边框（贴图里本来就画得更白）在暗环境下恒亮。清玻璃档的反射是
        // 整个关掉的（见上面 13715 那条），所以这份加性项是唯一能加亮度的地方。
        //
        // 本引擎没有 lightmap（环境底光注释 4182 明说它是"与时间无关的基线"），
        // 所以用**玻璃后那个面已经算出的光照**当代理 —— 语义正是原版的"玻璃与它
        // 后面的墙受同样的光"。两个必须注意的点：
        //   ① behindRadiance 的三项都乘过接收器 albedo ⇒ 直接拿它当光量会让
        //      "黑曜石墙后面的玻璃"在白天也发暗 ✗。这里**除以接收器 albedo 的亮度**
        //      把 albedo 约掉，得到该处的光照量级。
        //   ② 再按参考亮度归一到 0..1：白天趋近/超过参考 ⇒ 钳到 1 ⇒ **白天观感不变**；
        //      夜晚/洞穴/室内 ⇒ 光量小 ⇒ 边框跟着暗下去（这才是要修的现象）。
        //
        // ⚠ 若白天实测玻璃边框比改动前暗，把 GLASS_TINT_LIGHT_REFERENCE **调小**
        //   （它就是这一个旋钮）；若夜间仍偏亮则调大。localEntityIrradiance 是否
        //   已被 albedo 乘过没有逐行确认，若未乘则此处会略微高估夜间光量。
        // =================================================
        const float GLASS_TINT_LIGHT_REFERENCE = 1.0;

        const vec3 TINT_LUMA_WEIGHTS =
            vec3(0.2126, 0.7152, 0.0722);

        // ⚠ 这里必须用 transmissionInfo.receiverAlbedo 而不是 hitAlbedo：
        //   hitAlbedo 在本分支**不在作用域内**（13770 编译期 undeclared），
        //   而 receiverAlbedo 正是走查在接收器处填下的 albedo（13109-13113），
        //   语义相同且权威。某些路径下它可能是 vec3(0)（12998/13237 的初始化）
        //   ⇒ 下面的 0.02 下限会把它导向"比值很大 ⇒ 钳到 1 ⇒ 观感不变"，
        //   是安全退化，不会出 NaN。
        //
        // =================================================
        // 2026-10-24 第二轮：取光来源改为**确定性**量（与 GI 侧清玻璃分支
        // GI_CLEAR_GLASS_GI_AS_AIR，2026-10-13 同源：evaluateVLIEmitterRadianceAtHit
        // + getSurfaceAmbientRadiance）。
        //
        // 旧档拿 behindRadiance 的亮度当光量代理 —— 它含本地光 RIS 的时域残差，
        // 而下面的除以 receiverAlbedoLuma 把残差按 1/albedo 放大：深色接收面
        // （黑石/深色木框）上 litScale 被推到 1 饱和，恰好落在玻璃贴图的白色
        // 边框纹素上 ⇒ 静止时沿玻璃行边界的一排饱和白点（用户截图里的点线）。
        // 残差减半只让"刚饱和"的边缘点少一点，饱和点本身纹丝不动 —— 这就是
        // 第二轮钳制"看不出变化"的原因。确定性来源没有估计残差，放大路径
        // 整个消失。标定不变（reference 仍 1.0，GI 侧同值）。
        // =================================================
        vec3 glassLitProxyEnv =
            evaluateVLIEmitterRadianceAtHit(
                transmissionInfo.receiverWorldPos,
                transmissionInfo.receiverNormal,
                transmissionInfo.receiverAlbedo
            ) +
            getSurfaceAmbientRadiance();

        float receiverLightLuma =
            dot(glassLitProxyEnv, TINT_LUMA_WEIGHTS);

        float litGlassTintScale =
            clamp(
                receiverLightLuma / GLASS_TINT_LIGHT_REFERENCE,
                0.0,
                1.0
            );

        return mix(
            behindRadiance,
            clamp(glassTint, vec3(0.0), vec3(1.0)) *
                litGlassTintScale,
            clamp(glassAlpha, 0.0, 1.0)
        );
    }


    // =====================================================
    // Mirror-glass transmission. The pane keeps straight transport and the
    // Orion thin-sheet filter, but intentionally does not consume LabPBR
    // roughness, metalness, or POM self-shadowing. Its reflective lobe is a
    // single deterministic mirror ray with a dielectric F0.
    // =====================================================

    vec3 blockTransmission =
        computeGlassBlockTransmission(
            glassTint,
            glassAlpha,
            transparentProfile
        );

    // Orion thinTransmissionWeight: the transmission strength is applied
    // OUTSIDE the thin-sheet series, never folded into the tint.  A dense
    // body (tinted glass / honey) attenuates transmitted energy without
    // darkening its dye.
    float transmissionWeight =
        getTransparentProfileTransmission(transparentProfile);


    vec3 N =
        normalize(
            glassNormal
        );


    vec3 V =
        normalize(
            -straightDirection
        );


    float NdotV =
        clamp(
            abs(
                dot(
                    N,
                    V
                )
            ),
            0.0,
            1.0
        );


    const vec3 glassF0 = vec3(0.04);


    // View-angle Fresnel enters the Orion thin-sheet energy split: the
    // internal-reflection geometric series distributes energy between the
    // straight transmission and the mirror reflection.  glassF0 is uniform
    // across RGB, so the Fresnel factor is scalar.
    float viewFresnel =
        fresnelSchlickScalar(
            NdotV,
            0.04
        );


    // 2026-10-25 清理：旧的经验薄片链路（glassThinSheetFactorsScalarF +
    // pow(tint, path-1) 光程修正）已由下面的物理薄片模型取代 ——
    // 它原先算完就被覆盖，属于纯浪费的 ALU，这里只留声明。
    vec3 glassReflection;
    vec3 glassTransmission;

    // 2026-10-25 清理：旧的 pow(tint, path-1) 光程修正与 glassSegmentTransmittance
    // 已由物理薄片模型（按真实厚度 / cosT 做 Beer-Lambert）取代。
    // 这里只保留"入射格之后的其他染色格"的体素 DDA 方向（零射线）。
    const float glassDyeIor =
        glassIorForProfile(transparentProfile);

    vec3 dyeNormal = normalize(glassNormal);
    if(dot(straightDirection, dyeNormal) > 0.0)
    {
        dyeNormal = -dyeNormal;
    }

#if GI_GLASS_WALK && GI_GLASS_DIELECTRIC
    // v3 弯折的起飞方向已经是体内折射方向 ⇒ 不能再折一次。
    vec3 glassInsideDirection = straightDirection;
#else
    vec3 glassInsideDirection =
        refract(straightDirection, dyeNormal, 1.0 / glassDyeIor);
    if(dot(glassInsideDirection, glassInsideDirection) < 1.0e-8)
    {
        glassInsideDirection = straightDirection;
    }
    glassInsideDirection = normalize(glassInsideDirection);
#endif

    // Keep a direct-sun GGX highlight for the pane, but use a fixed mirror
    // regularizer rather than material/POM roughness. A literal zero
    // roughness makes the microfacet distribution singular.
    const float GLASS_MIRROR_SUN_ROUGHNESS = 0.04;
    vec3 surfaceSpecular = vec3(0.0);
    vec3 L = normalize(sunDirection);
    float NdotL = max(dot(N, L), 0.0);
    if(NdotL > 0.0 && NdotV > 0.0)
    {
        vec3 H = normalize(V + L);
        float VdotH = max(dot(V, H), 0.0);
        float F = fresnelSchlickScalar(VdotH, 0.04);
        float D = distributionGGX(N, H, GLASS_MIRROR_SUN_ROUGHNESS);
        float G = geometrySmith(
            N, V, L, GLASS_MIRROR_SUN_ROUGHNESS
        );
        float specularBRDF = (D * G * F) /
            max(4.0 * NdotV * NdotL, 1.0e-4);
        vec3 sunTransmittance = traceSunTransmittanceThroughWater(
            glassSurfacePosition + N * 0.035, L
        );
        vec3 sunRadiance =
            getTimeDrivenSkyLightColor() *
            3.2 *
            getTimeDrivenSkyLightRadianceScale() *
            sunTransmittance *
            c3CloudSunTransmittance(glassSurfacePosition + N * 0.035, L);
        surfaceSpecular = sunRadiance * specularBRDF * NdotL;
    }
#if VPT_GLASS_ALPHA_BLEND && !VPT_GLASS_ALPHA_BLEND_PANE_SPECULAR
    // 同上：(1-a) 已是全部透射能量，再叠太阳 GGX 高光属于重复计能量，
    // 且高光只落在直面太阳的面上 => 与 alpha blend 的逐面均匀观感冲突。
    surfaceSpecular = vec3(0.0);
#endif


    // 2026-10-25 清理：两段 DBG_SHOW_GLASSTINT 判决块删除（其中 stage 3 引用了
    // 已删除的 sheetTransmission）。玻璃色相/浓度的读数改用 DBG_BEHIND_SPLIT_MACRO。

    // =====================================================
    // 2026-10-25 阶段 B：物理薄片滤波（Remix thin-walled 同构）
    // 旧经验链路已在此次清理中删除，本段不再是"覆盖"，而是唯一实现。
    //
    // 上面的经验链路（sheetTransmission × 光程指数）仍会跑一遍，结果在此处被
    // 覆盖。口径对齐 dxvk-remix：
    //   transmission = atten·(1-Fo)·(1-Fi)/(1-(atten·Fi)^2)
    //   reflection   = Fo + Fi·atten^2·(1-Fo)·(1-Fi)/(1-(atten·Fi)^2)
    //   atten        = transmittanceColor^(thickness/cosT)
    // 其中 thickness 由 glassSlabThicknessFromFace() 从命中面解析给出（板 0.125 /
    // 整块 1.0），cosT 由 Snell 折射角给出 ⇒ 斜视光程自然变长，
    // 不再需要 pow(tint, path-1) 那条手工修正。
    // =====================================================
    {
        const float v2Thickness =
            glassSlabThicknessFromFace(glassSurfacePosition, glassNormal);

        vec3 v2Transmission;
        vec3 v2Reflection;

        glassThinWallFactors(
            blockTransmission,
            v2Thickness,
            NdotV,
            glassIorForProfile(transparentProfile),
            0.04,
            v2Transmission,
            v2Reflection);

        // 与 Orion surfaceDeltaLobes() 同一分工：致密体（蜂蜜/遮光玻璃）用
        // transmissionWeight 压低透射、把反射抬回裸 Fresnel。
        glassTransmission = v2Transmission * transmissionWeight;
        glassReflection = mix(vec3(viewFresnel), v2Reflection, transmissionWeight);

#if VPT_GLASS_ALPHA_BLEND
        // 2026-10-26 对齐 RTX Remix：MC 的 translucent 层是 alpha blend，
        // 用源 alpha 而不是"薄壁染料吸收"（推导见 glass_chain_common.glsl 的共享头）。
        // 致密体（遮光玻璃/冰/粘液/蜂蜜）的"不透明度"来自剖面表：a = 1 − transmission。
        // Remix 侧这个值是材质里 authored 的 opacity；我们这里由 profile 表等价给出。
        const float alphaBlendOpacity = max(
            glassAlpha,
            1.0 - clamp(transmissionWeight, 0.0, 1.0));
        glassTransmission = glassAlphaBlendTransmission(glassTint, alphaBlendOpacity);
#if VPT_GLASS_ALPHA_BLEND_PANE_SPECULAR
        // A/B 档：保留裸 Fresnel 反射（旧行为）。
        glassReflection = vec3(viewFresnel);
#else
        // 2026-10-26 修正：alpha blend 表面**没有**反射项。
        //   Remix: AlphaBlendSurface::getAlphaBlendSurface() 只取 albedo x opacity；
        //   合成式 out = a*C_surface + (1-a)*C_bg 里 (1-a) 已是全部透射能量。
        // 此前保留裸 Fresnel，而 abs(N·V) 在掠射时 ->1 => 侧面被镜面反射盖成
        // "不透明纯色"，正是"一个玻璃块只有正对的透明面透明"。
        glassReflection = vec3(0.0);
#endif
#endif

#if GI_GLASS_RUN_DYE && !VPT_GLASS_ALPHA_BLEND
        // 2026-10-26：alpha blend 口径下每层已按 (1−a)+a·tint 乘过；这条多格染料
        // 累计会把同一染料再乘一遍（旧"透过彩色玻璃发黑"的成因之一）。
        // 入射格之后的其他染色格：体素页表 DDA（零射线），与物理模型不重叠
        // —— 物理模型只算"当前这一片"的实际光程。
        if(transparentProfile == 0)
        {
            glassTransmission *=
                glassRunDyeAfterFirstCell(glassSurfacePosition, glassInsideDirection);
        }
#endif

#if GI_GLASS_DOMINANT_GUIDE
        glassLobeReflectionWeight = glassReflection;
        glassLobeTransmissionWeight = glassTransmission;
        glassLobeWeightsValid = true;
#endif
    }

#if VPT_GLASS_ALPHA_BLEND && VPT_GLASS_ALPHA_BLEND_SURFACE_RADIANCE
    // 2026-10-26（第三轮，对齐 Remix 的 alpha blend 合成）：
    //   C_out = C_bg * PI(1-a_i) + SUM_i E_bg * tint_i * a_i * PI_{j<i}(1-a_j)
    // 第一层的 a/tint 在这里重算（与上面 glassTransmission 的口径同源），
    // 第 2..N 层由 any-hit 累进 paneChainSurfaceWeight。
    // 2026-10-26：a = glassAlpha（= 贴图 texel alpha x 顶点色 alpha），
    // 与 Remix-MC 的 ATLAS_TRANSLUCENT_BLEND = textureTimesVertexColor() 同源
    // （BlockTextureManager.java:64）。致密体用剖面表抬高不透明度。
    // 2026-10-27：与两个 rahit 同口径的 alpha 上限（贴图边框 alpha=1 的问题，
    // 见 rahit 里 chainAlpha 处的说明）。
    // 宏在共享头里定义，但本行早于那个 include，所以就地给同值兜底。
#ifndef GLASS_ALPHA_BLEND_MAX_ALPHA
#define GLASS_ALPHA_BLEND_MAX_ALPHA 0.3
#endif
    // 2026-10-27：与 rahit 同口径 —— 玻璃族用固定 alpha，不再取贴图 alpha。
    //
    // 2026-10-28 诊断：置 VPT_GLASS_ALPHA_BLEND_CONST_ALPHA=1 时把 a 直接钉成
    // GLASS_FIXED_ALPHA，**完全不看 transmissionWeight**。
    // 实机截图（glass_dbg=1 的 G 通道 = a）显示出射面上密布"绿点" = 局部 a 高于 0.3，
    // 而 a 只可能由 profile 抬高 ⇒ 怀疑出射面的 payload.state 剖面位是残留值。
    // 本档用来一刀判定：钉成常量后绿点消失 ⇒ 就是 profile/a 抖动。
    // 2026-10-29 用户定档：保留"能透视"的半透明效果 ⇒ a 统一取 GLASS_FIXED_ALPHA。
    // （曾经的抖动来自 profile 位在部分面上读成 0/1，导致同一块玻璃半透半实。）
    #ifndef VPT_GLASS_ALPHA_BLEND_CONST_ALPHA
    #define VPT_GLASS_ALPHA_BLEND_CONST_ALPHA 1
    #endif
#if VPT_GLASS_ALPHA_BLEND_CONST_ALPHA
    const float blendAlpha0 = GLASS_FIXED_ALPHA;
#else
    const float blendAlpha0 = clamp(
        max(GLASS_FIXED_ALPHA,
            1.0 - clamp(transmissionWeight, 0.0, 1.0)),
        0.0, 1.0);
#endif
    const vec3 blendTint0 =
        clamp(glassTint, vec3(0.0), vec3(1.0));

    // 第一层玻璃面到相机的距离（74.a）。只被屏幕空间邻居搜索的"共面/深度"
    // 判据消费，所以它必须与相机射线同源：用 rayDirection（含磨砂抖动）而不是
    // glassSurfacePosition 的 POM 位移量 —— 后者逐 texel 跳变，会让判据在
    // 浮雕图案里假否定（与 historyKeyPosition 那条 2026-10-17 的结论同因）。
    const float blendPaneHitT =
        max(
            dot(
                glassSurfacePosition - cam.viewInverse[3].xyz,
                normalize(rayDirection)),
            1.0e-3);

#if VPT_GLASS_ALPHA_BLEND_PANE_ILLUMINATION
    // C_surface 的光照取自**玻璃表面自己的位置**（与 clearGlassAsAir 分支同源）：
    //   太阳 NEE（1 条带遮挡的射线）+ 环境底光 + VLI 点光源（零射线，缓存）。
    // 上一版用"背景接收面的辐照度"当代理，但背景可能是 50 格外的墙、或者是天空 ——
    // 侧面斜视时背后正是天空 => E 远超实际受光 => 整面过曝成纯色。
    vec3 paneIllumination =
        evaluateDirectSunLighting(
            glassSurfacePosition,
            N,
            V,
            vec3(1.0),
            0.06,
            0.0,
            1.0,
            sunDirection) +
        getSurfaceAmbientRadiance() +
        evaluateVLIEmitterRadianceAtHit(
            glassSurfacePosition, N, vec3(1.0));

    vec3 blendSurfaceWeight =
        blendTint0 * blendAlpha0 * paneChainTransmittance +
        paneChainSurfaceWeight;

    vec3 transmittedRadiance =
        behindRadiance * ((1.0 - blendAlpha0) * paneChainTransmittance) +
        paneIllumination * blendSurfaceWeight;

#ifndef DBG_GLASS_ALPHA_BLEND
// 2026-10-26 定位用（默认 0）：把半透明的三个决定量画成 RGB。
//   R = blendAlpha0                 第一层 alpha（贴图 texel alpha × profile 不透明度）
//   G = 链内 2..N 层的权重和         斜视穿过更多层时变大
//   B = luma(paneIllumination)×0.25  C_surface 的受光强度
// 用法：-DDBG_GLASS_ALPHA_BLEND=1 重编 rt_test.rgen 的四个变体。
// 判读：白蓝的面 R≈1 => alpha 本身是 1（texel/marker）；绿 => 层数叠加；蓝 => 受光过亮。
#define DBG_GLASS_ALPHA_BLEND 0
#endif
#if DBG_GLASS_ALPHA_BLEND
    return vec3(
        blendAlpha0,
        clamp(max(paneChainSurfaceWeight.r,
                  max(paneChainSurfaceWeight.g,
                      paneChainSurfaceWeight.b)), 0.0, 1.0),
        clamp(dot(paneIllumination, vec3(0.2126, 0.7152, 0.0722)) * 0.25,
              0.0, 1.0));
#endif
#if VPT_GLASS_ALPHA_BLEND_DEFERRED_COMPOSITE
        // 2026-10-26（第四轮，严格搬）：相机直接命中的这个面是**第一层候选**，
        // 与 any-hit 调同一个 update()（原文 alpha_blend_surface.slangh:128-158）。
        // 颜色按 getAlphaBlendSurface():117-123 的解调口径（albedo / opacity）。
        glassAlphaBlendSurfaceUpdate(
            ivec2(gl_LaunchIDEXT.xy),
            vec4(blendTint0 / max(blendAlpha0, 1.0e-3), blendAlpha0),
            N,
            blendPaneHitT,
            0.0);
        // radiance volume 兜底（binding 77）：
        // Remix 用 evalVolumetricNEE(VolumeFilteredRadianceY/CoCg, ...) * surface.color
        // （composite_alpha_blend.comp.slang:259-267）。本引擎没有 SH-froxel，
        // 用**已有的体素面辐照度缓存**换算成同量纲的 volumeRadiance：
        //   evaluateVPTExactFaceDirectLighting(pos, normal, albedo = 1)
        //     = 0.92/pi * E  （VPT_EXACT_FACE_DIFFUSE_SCALE，rt_test.rgen:2060）
        // 消费端同样乘 surface albedo，所以传 albedo = 1 即可。
        imageStore(
            glassAlphaBlendVolumeImage,
            ivec2(gl_LaunchIDEXT.xy),
            vec4(
                evaluateVPTExactFaceDirectLighting(
                    transmissionInfo.receiverWorldPos,
                    transmissionInfo.receiverNormal,
                    vec3(1.0)),
                1.0));

        // 返回**未合成**的背景辐射：合成由 compute pass 按
        //   out = radiance * backgroundAlpha + alphaBlendOutput
        // （composite.comp.slang:812）完成。
        //
        // 2026-10-27：不允许纯黑。背景为 0 时合成退化成 a*tint*E（纯玻璃色），
        // 而侧面正对着背光/阴影面 ⇒ 玻璃在最该透明的地方反而像实心块。
        // 这里只保证"有背景可透"，不是把阴影整体提亮。
        return max(behindRadiance, vec3(GLASS_BEHIND_MIN_RADIANCE));
#endif
#else
    // E_bg：背景接收面的**入射辐照度**（Remix 的 demodulated 直接光）。
    // behindRadiance 的三项都乘过接收面 albedo（见 16302 的说明），逐通道除回去。
    // 0.05 下限防除零（深色面），64 上限纯属护栏（太阳直射辐照度约 20~30）。
    vec3 behindIrradiance;
    if(behindReachedSky)
    {
        // Remix 的邻居搜索显式跳过 miss（composite_alpha_blend.comp.slang:174-187
        // 的 `!isMiss`），找不到邻居时回落到 radiance volume
        // （stochasticAlphaBlendUseRadianceVolume）。本引擎没有体积辐照度网格，
        // 用同一个世界空间环境辐照度兜底。
        // ⚠ 不能用天空自身的辐射：那会让"背后是天空"的玻璃面亮到发白，
        //   正是"侧面看不见后面"的直接成因（2026-10-26 实测）。
        behindIrradiance = getSurfaceAmbientRadiance();
    }
    else
    {
        // 降级（本仓库无屏幕空间邻居搜索，见下方标注）：
        // Remix 用 PrimaryDirectDiffuseRadianceHitDistance[neighbor]（已 demodulate），
        // 这里用“透射射线命中的接收面”的 radiance / albedo 代替。
        behindIrradiance =
            behindRadiance / max(transmissionInfo.receiverAlbedo, vec3(0.05));
    }

    vec3 blendSurfaceWeight =
        blendTint0 * blendAlpha0 * paneChainTransmittance +
        paneChainSurfaceWeight;

    vec3 transmittedRadiance =
        behindRadiance * ((1.0 - blendAlpha0) * paneChainTransmittance) +
        behindIrradiance * blendSurfaceWeight;
#if VPT_GLASS_ALPHA_BLEND_DEFERRED_COMPOSITE
#if !GI_HALFRES
    // =====================================================
    // 2026-10-31（第六轮，椒盐噪点真根因）：**变体坐标腐蚀门控**。
    //
    // GI_HALFRES 变体的 dispatch 是 (w+1)/2×(h+1)/2，gl_LaunchIDEXT 是
    // **半分辨率坐标**（本文件 GI_HALFRES pixelCoord 派生处的注释警告过），
    // 但它同样执行完整主射线着色（含玻璃路径）。此前（含抽签口径）：
    //   · reset / update / WriteNearest 以 launchID 写 73/74 ⇒ 像素 (i,j) 被
    //     像素 (2i,2j) 的玻璃状态覆写 ⇒ 合成错位；
    //   · 77 号图的 pane 时域 RIS 以 launchID 索引 26/27 历史 ⇒ 像素 (2i,2j)
    //     的 pane 历史写进 (i,j) 槽位 ⇒ 主变体在 (i,j) 读历史必然位置失配
    //     ⇒ 历史永久重置 ⇒ 染色项永远只有单帧 RIS 噪声 = 密集椒盐噪点
    //     （染色亮处青椒点 / 暗背景处黑椒点，2026-10-31 晚截图形态）。
    // GI 变体的 GI 计算不需要 G-buffer / pane 受光 ⇒ 这些写入整体跳过：
    //   74.y 由宿主每帧 clear 保持 0 ⇒ gi 触及的像素天然不参与合成；
    //   主变体（全分辨率、后行）完整覆写自己负责的像素。
    // 顺带收益：gi 变体每个半分辨率像素省 ~6 条射线（太阳 NEE + RIS）。
    // =====================================================
#if VPT_GLASS_DETERMINISTIC_CHAIN
    // 最近层确定性回填：74.x 已由 reset 置 1−a0、any-hit 又累乘了深层 ⇒
    // 不动；rgb 是加法（深层累积已就位，w_0/a_div = 1 ⇒ 本层贡献恰为
    // tint0）；74.y 在这里才置位 ⇒ deferred 合成自此生效。
    glassAlphaBlendChainWriteNearest(
        ivec2(gl_LaunchIDEXT.xy),
        blendTint0,
        N,
        blendPaneHitT);

    // 77 号图换血（固定形状噪点的病根修复）：
    // 旧内容 evaluateVPTExactFaceDirectLighting = 16^3×6 面缓存的逐方块面
    // 量化烘焙 —— 查表、逐帧不变、在 RR 之后合成 ⇒ 静态噪点且任何 RR 挡位
    // 都无效（2026-10-31 诊断结论；09-30"采样数怎么调都不动"与 10-01
    // "the squares the user sees" 同源）。
    // 新内容 = pane 自身位置的三项受光（albedo=1 的辐照度，tint 在消费端乘）：
    //   太阳 NEE（解析确定性）+ 环境底光（解析确定性）
    //   + 本地光时域 RIS：evaluateTransparentReceiverDirectLighting 复用
    //   26/27 世界空间历史（4 RIS 提案 + 4 可见性射线 + 1 动态光，96 帧历史
    //   + 火萤钳制 + 运动自适应封顶）。键 = historyKeyPosition（pane 原始命中
    //   点，该参数的本职），身份法线 = pane 几何法线 —— 正是这套机制的设计场景。
    // 量纲：blend.comp 的 ×(1/π) 完成兰伯特重建；旧 bake 存 0.92/π·E 又被
    // 除一次 π，本就量纲错位，本口径顺带修正（玻璃染色亮度会略变，A/B 确认）。
    // 回滚：VPT_GLASS_PANE_ILLUMINATION_TEMPORAL=0 ⇒ 回 bake 读。
    #ifndef VPT_GLASS_PANE_ILLUMINATION_TEMPORAL
    #define VPT_GLASS_PANE_ILLUMINATION_TEMPORAL 1
    #endif
    #if VPT_GLASS_PANE_ILLUMINATION_TEMPORAL
    imageStore(
        glassAlphaBlendVolumeImage,
        ivec2(gl_LaunchIDEXT.xy),
        vec4(
            evaluateDirectSunLighting(
                glassSurfacePosition,
                N,
                V,
                vec3(1.0),
                0.06,
                0.0,
                1.0,
                sunDirection) +
            getSurfaceAmbientRadiance() +
            evaluateTransparentReceiverDirectLighting(
                historyKeyPosition,
                glassNormal,
                glassSurfacePosition,
                N,
                vec3(1.0),
                0x676C6173u),
            1.0));
    #else
    imageStore(
        glassAlphaBlendVolumeImage,
        ivec2(gl_LaunchIDEXT.xy),
        vec4(
            evaluateVPTExactFaceDirectLighting(
                transmissionInfo.receiverWorldPos,
                transmissionInfo.receiverNormal,
                vec3(1.0)),
            1.0));
    #endif
#else
    // 2026-10-26（第四轮，严格搬）：默认路径（PANE_ILLUMINATION=0）下
    // 主命中面同样是第一层候选，见上面 PANE_ILLUMINATION 分支的说明。
    glassAlphaBlendSurfaceUpdate(
        ivec2(gl_LaunchIDEXT.xy),
        vec4(blendTint0 / max(blendAlpha0, 1.0e-3), blendAlpha0),
        N,
        blendPaneHitT,
        0.0);

    // radiance volume 兜底（binding 77），说明见上面 PANE_ILLUMINATION 分支。
    imageStore(
        glassAlphaBlendVolumeImage,
        ivec2(gl_LaunchIDEXT.xy),
        vec4(
            evaluateVPTExactFaceDirectLighting(
                transmissionInfo.receiverWorldPos,
                transmissionInfo.receiverNormal,
                vec3(1.0)),
            1.0));
#endif
#else
    // GI_HALFRES 变体：跳过全部 73/74/75/77 写入（坐标腐蚀，见上注释）。
    // 74.y 由宿主 clear 保持 0 ⇒ gi 覆盖的像素不参与 deferred 合成。
#endif

    // 2026-10-27：透过玻璃看到的背景不允许纯黑。
    // 背景为 0 时 alpha/折射合成都退化成"纯玻璃色"，
    // 而侧面正对着背光/阴影面 ⇒ 玻璃在最该透明的地方反而像实心块。
    // 这不是把阴影整体提亮：只在玻璃这条路径上保证有背景可透。
    behindRadiance = max(behindRadiance, vec3(GLASS_BEHIND_MIN_RADIANCE));

    return behindRadiance;
#endif
#endif
#else
    vec3 transmittedRadiance =
        behindRadiance *
        glassTransmission *
        paneChainTransmittance;
#endif

    // One deterministic mirror ray returns the first opaque scene surface.
    // Its existing PBR/VLI/cached-local radiance is weighted by the
    // Orion thin-sheet reflection factor instead of a glass GGX lobe.
    //
    // 2026-10-05 低 Fresnel 解析反射：正入射附近薄片反射权重 ≈ 4-8%，一条
    // 镜面 trace 只为这 ~5% 服务。反射方向朝上（大概率天空）且权重低于阈值
    // 时，用预计算天空散射纹理（与 miss 着色器同一张 equirect，binding 42）
    // 解析取值。朝地不跳 —— 可能反射地形，无法解析。被跳过时
    // rrGlassReflectionHitDistance 保持初值 65504：解析天空本就是无穷远
    // 内容，RR 按"天空反射"语义重投影是正确的（旧值反而错误地指向pane）。
    //
    // ⚠ 2026-10-07 修正：上面那句"朝上 ⇒ 大概率天空"**不成立**（天花板/屋檐/
    // 树冠/山脊都在地平线以上），而这条路径又不做任何遮挡检测 ⇒ 玻璃面上出现
    // 一个天光色半圆（低 Fresnel 圆 ∩ 朝上半平面）。现在"朝上"只当**候选**，
    // 真正生效前还要过 glassMirrorPathIsClear() 的可见性探针。
    vec3 glassMirrorDirection = reflect(-V, N);
#ifndef GI_GLASS_ANALYTIC_REFLECTION
// 低 Fresnel 解析反射总闸（默认 1；置 0 回"每像素一条镜面 trace"旧档）。
#define GI_GLASS_ANALYTIC_REFLECTION 1
#endif
#ifndef GI_GLASS_ANALYTIC_REFLECTION_MAX_WEIGHT
// 反射权重峰值低于此值才跳（≈ N·V ≳ 0.5 的正面视锥）。太薄会吃掉掠射
// 的强 Fresnel 反射，太厚省不到 trace —— 0.09 ≈ 薄片反射在正入射附近的
// 上界（F0=0.04 的薄片系列）。
#define GI_GLASS_ANALYTIC_REFLECTION_MAX_WEIGHT 0.09
#endif
#ifndef GI_GLASS_ANALYTIC_REFLECTION_MIN_UP
#define GI_GLASS_ANALYTIC_REFLECTION_MIN_UP 0.05
#endif
#ifndef GI_GLASS_ANALYTIC_REFLECTION_PROBE
// 2026-10-07 可见性探针总闸（默认 1）。置 0 = 回到"朝上即天空"的旧判据 ——
// 那会在玻璃面上留下天光色半圆（见 glassMirrorPathIsClear 的说明）。
#define GI_GLASS_ANALYTIC_REFLECTION_PROBE 1
#endif
#ifndef GI_GLASS_ANALYTIC_PROBE_DISTANCE
// 探针距离（格）：这段内出现任何几何体就放弃解析天空、改走真实镜面 trace。
// 48 覆盖室内天花板/屋檐/树冠/邻近建筑。更远的山坡仍会被近似成天空，但那是
// 5% 权重级别的误差，且**不再有硬边形状** —— 旧档的问题是形状本身。
#define GI_GLASS_ANALYTIC_PROBE_DISTANCE 48.0
#endif
#if GI_GLASS_ANALYTIC_REFLECTION
    float glassReflectionPeak =
        max(glassReflection.r, max(glassReflection.g, glassReflection.b));
    // "候选" = 旧判据（低权重 ∩ 朝上）。是否真的走解析天空还要过探针，
    // 见下面 surfaceSceneReflection 处的显式 if。
    bool glassReflectionAnalyticCandidate =
        glassReflectionPeak < GI_GLASS_ANALYTIC_REFLECTION_MAX_WEIGHT &&
        glassMirrorDirection.y > GI_GLASS_ANALYTIC_REFLECTION_MIN_UP;
#else
    const bool glassReflectionAnalyticCandidate = false;
#endif

    vec3 surfaceSceneReflection = vec3(0.0);
    // 候选（低权重 ∩ 朝上）还要过可见性探针：只有探针在 PROBE_DISTANCE 格内确实
    // 没命中，才允许用解析天空；命中 ⇒ 退回真实镜面 trace（那才是正确内容）。
    // 写成显式 if（不依赖 && 的短路语义）以保证"非候选像素一条探针都不打"。
    bool glassReflectionAnalytic = false;
#if GI_GLASS_ANALYTIC_REFLECTION
#if GI_GLASS_ANALYTIC_REFLECTION_PROBE
    if(glassReflectionAnalyticCandidate)
    {
        glassReflectionAnalytic =
            glassMirrorPathIsClear(
                glassSurfacePosition,
                glassMirrorDirection,
                GI_GLASS_ANALYTIC_PROBE_DISTANCE
            );
    }
#else
    glassReflectionAnalytic = glassReflectionAnalyticCandidate;
#endif
#endif
    if(glassReflectionAnalytic)
    {
        surfaceSceneReflection =
            textureLod(
                skyScatteringTex,
                skyLutDirectionToUv(glassMirrorDirection),
                0.0
            ).rgb *
            glassReflection;
    }
    else
    {
        surfaceSceneReflection =
            traceGlassSceneReflectionOneRay(
                glassSurfacePosition,
                N,
                V,
                sunDirection
            ) *
            glassReflection;
    }


#if RR_STABLE_GLASS_RADIANCE
    // Both of these are closed form -- one deterministic mirror ray and one
    // analytic GGX evaluation -- so this is the zero-variance half of the
    // pane.  It leaves through the out parameter instead of the return value
    // and is added back after DLSS RR.
    stableGlassRadiance =
        surfaceSceneReflection +
        surfaceSpecular;

#if GLASS_DEBUG_SPLIT
    // Per-term readout for the speck hunt.  Each channel is one term, scaled
    // so an ordinary value lands around 0.5:
    //   R = transmittedRadiance   (behindRadiance * glassTransmission)
    //   G = surfaceSceneReflection (deterministic mirror ray)
    //   B = surfaceSpecular        (analytic GGX against the sun)
    // A channel that sparkles between frames names the term that owns the
    // specks, with no argument about guides or denoisers.
    return vec3(
        clamp(dbgTermLuma(transmittedRadiance) * 0.5, 0.0, 1.0),
        clamp(dbgTermLuma(surfaceSceneReflection) * 0.5, 0.0, 1.0),
        clamp(dbgTermLuma(surfaceSpecular) * 0.5, 0.0, 1.0)
    );
#else
    return
        transmittedRadiance +
        stableGlassRadiance;
#endif
#else
    return
        transmittedRadiance +
        surfaceSceneReflection +
        surfaceSpecular;
#endif
#endif
}