

// =====================================================
// One-bounce water-bed diffuse GI
//
// Deterministic interface transport: all non-TIR interfaces refract and
// apply (1-F), while TIR reflects.  This removes the old roulette flicker
// without adding paths or changing water-surface normals.
// =====================================================
#ifndef WATER_VOLUME_INSCATTER_V2
#define WATER_VOLUME_INSCATTER_V2 1
#endif
#ifndef WATER_RECEIVER_GI_ADAPTIVE_GATE
#define WATER_RECEIVER_GI_ADAPTIVE_GATE 1
#endif

// ===========================================================================
// 2026-11-18：**水面像素的射线预算降级**（用户："靠近水面的超级掉帧"）。
//
// 实测（本局 18:5x）：同一场景，远离水面 k2sun=0.5 ms，靠近水面 0.5 → 5 → 20 → 36 ms，
// 全帧 41 ms。原因：水面像素的**整套着色**（反射/折射 walker、界面循环、
// 太阳透射 march、焦散 + SSS）都跑在 K2-SUN 这条腿里（rgen_main_body.glsl:1938
// 的"水像素的着色在此完整结束"），所以成本 ∝ 水的屏幕占比。
//
// 原有机制 perfL10WaterAdaptiveScale 只把**距离阈值**收紧（除以 0.35），
// 并不减少每条射线的步数 —— 近处水像素仍然 6 步全开，所以在 36 ms 时它救不了。
//
// 这里补一档按帧时间的**步数/预算**降级：
//   · 16 ms 以下：0（完全不介入，与改动前逐位相同）
//   · 16–30 ms：线性上升
//   · 30 ms 以上：1（walker 砍到 2 步、界面循环砍到 1 步、太阳 march 砍到 1 步）
// 帧时间恢复后会自己回到全质量，不需要重启。
// ===========================================================================
// perfL10WaterLoad() moved to rgen_trace_helpers.glsl (Batch AE).

#if !VPT_GLASS_DIELECTRIC_REWRITE
vec3 tracePrimaryWaterReceiverIndirectReworked(
    vec3 receiverPos,
    vec3 receiverNormal,
    vec3 receiverAlbedo,
    float lodDistance,
    vec3 sunDirection
#if GI_GLASS_RECEIVER_INDIRECT
    , bool startInsideWater
#endif
)
{
    uint lodState =
        uint(gl_LaunchIDEXT.x) * 73856093u ^
        uint(gl_LaunchIDEXT.y) * 19349663u ^
        cam.frameCount * 83492791u ^
        0x57474931u;
    uint lodRandom = pcg_hash(lodState);

    bool runWaterGI = true;
    float lodWeight = 1.0;
    // 2026-11-27 噪点回退：<32 格恢复全预算（棋盘格抽样会让水面起噪）。
    if(lodDistance < 32.0 * perfL10TierScale())
    {
    }
    else if(lodDistance < 64.0 * perfL10TierScale())
    {
        runWaterGI = (lodRandom & 1u) == 0u;
        lodWeight = 2.0;
    }
    else if(lodDistance < 128.0 * perfL10TierScale())
    {
        runWaterGI = (lodRandom & 1u) == 0u;
        lodWeight = 2.0;
    }
    else if(lodDistance < 160.0 * perfL10TierScale())
    {
        runWaterGI = (lodRandom & 3u) == 0u;
        lodWeight = 4.0;
    }
    else
    {
        runWaterGI = false;
    }
    if(!runWaterGI) return vec3(0.0);

    uint pathState = lodRandom ^ 0x57474942u;
    vec3 currentDirection =
        sampleHemisphereCosine2D(
            normalize(receiverNormal),
            vec2(
                glassBlueNoiseSample(rtLogicalPixelCoord(), cam.frameCount, 69u),
                glassBlueNoiseSample(rtLogicalPixelCoord(), cam.frameCount, 70u)
            )
        );
    vec3 currentOrigin = receiverPos + currentDirection * 0.001;
    vec3 throughput = receiverAlbedo * BSDF_GI_THROUGHPUT_SCALE;
#if GI_GLASS_RECEIVER_INDIRECT
    bool insideWater = startInsideWater;
#else
    bool insideWater = true;
#endif
    int waterInterfaceSteps = 0;

    const vec3 WATER_ABSORPTION = waterAbsorptionColorAt(receiverPos.xz);
    const vec3 WATER_SCATTER_COLOR = waterScatterColorAt(receiverPos.xz);

const float WATER_EXTINCTION_DISTANCE =
    16.0;
    const float WATER_IOR = 1.33;
    const float WATER_F0 = 0.0200593;
    // 2026-10-15 降本：水下 GI 界面步数 6→3（水路径成本账里"水下 GI walker
    // ≤7 条遍历"的那一刀）。相机在水上时这条 walker 只服务透过水面的接收器，
    // 影响小；潜水看波光时 GI 会略暗/略带状。回退 = 改回 6。
    const int MAX_WATER_INTERFACE_STEPS = 3;

    // 2026-11-18：重载时这个预算也砍到 1 步（见 perfL10WaterLoad）。
    // 3 步的原始理由是"水→第二透射面→接收面"这类 4 跳链路；重载下宁可少那
    // 一层嵌套，也不让整帧掉到 40 ms。
    // 2026-11-18（重构，Remix thin-walled 口径）：水下 GI 的界面步数固定 1。
    // 原来的 3 步理由是多界面链路（水→第二透射面→接收面），但每一跳都要一次
    // 完整 trace + 材质解析；在全分辨率下这是 K2-SUN 腿的主项之一。
    // 代价：只解析第一层界面（叠水/水后玻璃的 GI 会略暗），与 Remix 一致。
    // 回退：改回下面的 MAX_WATER_INTERFACE_STEPS。
    const int waterInterfaceBudget = 1;

    for(int pathStep = 0; pathStep <= waterInterfaceBudget; ++pathStep)
    {
        hit.hitDistance = 1.0;
        hit.state = makeTraceState(
            insideWater ? RT_PURPOSE_WATER_MEDIUM : RT_PURPOSE_FULL
        );
        hit.state |= RT_STATE_GI_SKIP_CLOUDS_BIT;
        if(cam.giHalfParams.z > 0.5f)
        {
            hit.state |= RT_STATE_GI_NO_POM_BIT;
        }
#if GI_SKIP_SUN_DISK
        hit.state |= RT_STATE_NO_SUN_DISK_BIT;
#endif
#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
        serTraceRay(
            currentOrigin,
            0.001,
            currentDirection,
            10000.0,
            secondaryTraverseFlags(),
            0xcfu,
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
                10000.0,
                secondaryTraverseFlags(),
                0xcfu,
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
                topLevelAS, secondaryTraverseFlags(), 0xcfu, 0, 0, 0,
                currentOrigin, 0.001, currentDirection, 10000.0, 0
            );
        }
#endif

        if(payload.hitDistance < 0.0)
        {
            return throughput * unpackAlbedoUnorm(payload.albedoPacked) * 1.8 * lodWeight;
        }

        float hitDistance = payload.hitDistance;
        vec3 hitPos = currentOrigin + currentDirection * hitDistance;
        float hitMaterialId = getLegacyMaterialId(payload.state);
        if(insideWater)
if(insideWater)
{
    vec3 extinction =
        exp(
            -WATER_ABSORPTION *
            hitDistance
        );

float scatterAmount = 0.0;

    throughput *= extinction;

    throughput =
        mix(
            throughput,
            WATER_SCATTER_COLOR,
            scatterAmount
        );
}

        if(hitMaterialId <= 0.5)
        {
            vec3 hitNormal = payload.normal;
            vec3 hitAlbedo = unpackAlbedoUnorm(payload.albedoPacked);
            vec3 incomingRadiance = evaluateOpaqueSurfaceRadiance(
                hitPos,
                hitNormal,
                -currentDirection,
                hitAlbedo,
                max(payload.roughness, 0.05),
                payload.f0Metal,
                payload.emission +
                    evaluateVLIEmitterRadianceAtHit(hitPos, hitNormal, hitAlbedo),
                payload.pomSelfShadow,
                sunDirection
            );
            uint localState = pathState ^ 0x5747494Cu;
            pcg_hash(localState);
            int bounceF0MetalCode =
                decodeF0MetalCode(payload.f0Metal);
            incomingRadiance +=
                evaluateVPTRISDirectLighting(
                    hitPos, hitNormal, hitAlbedo, vec3(0.0),
                    decodeBaseF0(bounceF0MetalCode, hitAlbedo),
                    decodeMetallicFromCode(bounceF0MetalCode),
                    max(payload.roughness, 0.05),
                    localState);
            return throughput * incomingRadiance * 1.8 * lodWeight;
        }

        if(hitMaterialId >= 2.5)
        {
            // Glass is a colour filter, never a water-medium transition.
            if(hitMaterialId < 3.5)
            {
                throughput *= glassSheetTransmission(
                    unpackAlbedoUnorm(payload.albedoPacked),
                    payload.normal,
                    currentDirection
                );
            }
            currentOrigin = hitPos + currentDirection * 0.001;
            continue;
        }

        if(++waterInterfaceSteps > MAX_WATER_INTERFACE_STEPS)
        {
            return vec3(0.0);
        }

        bool exitingWater = hitMaterialId > 1.5;
        // 2026-11-20（第二版）：掠射视线下界面法线收敛到平面，折射/反射/判定
        // 全部与三角形无关 —— 见 rgen_trace_helpers.glsl 的
        // waterInterfaceNormalForRay 说明。陡视角逐位保留波浪法线。
        vec3 interfaceNormal =
            waterInterfaceNormalForRay(payload.normal, currentDirection);
        // 2026-11-20：Fresnel 余弦改用稳定版（平面余弦），否则逐三角形的波浪
        // 法线在掠射视角下把 (1-F) 打到 0 ⇒ 该三角形下面的水底整块变黑
        // （用户报的"水底闪烁黑三角"）。见 rgen_trace_helpers.glsl 的同名说明。
        float cosTheta = waterFresnelCosine(currentDirection, interfaceNormal);
        float fresnelX = 1.0 - cosTheta;
        float fresnelX2 = fresnelX * fresnelX;
        float fresnel = WATER_F0 +
            (1.0 - WATER_F0) * (fresnelX2 * fresnelX2) * fresnelX;
        vec3 refractedDirection = refract(
            currentDirection,
            interfaceNormal,
            exitingWater ? WATER_IOR : (1.0 / WATER_IOR)
        );

        if(dot(refractedDirection, refractedDirection) < 1.0e-6)
        {
            currentOrigin = hitPos + interfaceNormal * 0.001;
            currentDirection = reflect(currentDirection, interfaceNormal);
            continue;
        }

        throughput *= 1.0 - fresnel;
        insideWater = !exitingWater;
        currentOrigin = hitPos - interfaceNormal * 0.001;
        currentDirection = refractedDirection;
    }

    return vec3(0.0);
}
#endif // !VPT_GLASS_DIELECTRIC_REWRITE

// =====================================================
// 2026-10-06 近水掉帧降本：反射/折射 walker 的步数距离分档。
//
// walker 的每一步都要打一条硬件射线并对命中面做完整着色（反射走
// RT_PURPOSE_FULL、水下走 RT_PURPOSE_WATER_MEDIUM）。近水时每水像素有两条
// walker（反射 + 折射）各 ≤8 步 ⇒ 16+ 条着色射线。
//
// 远水的实际观感信息量极低：水面在屏幕上只占细窄一条，折射内容（水底）已被
// Beer-Lambert 压到 16 格后基本不可见，且 RR/NRD 会从邻域重建。因此把步数按
// 距离分档 —— 近水（<48 格）保持 8 步不变，中距 5 步、远距 3 步。
// 截断与"步数自然用尽"同语义（返回已累积辐射），不是硬切黑。
//
// 回退：把三个常量都改回 8（或把整个函数体 return 8）即逐位回旧档。
// =====================================================
const float WATER_WALKER_FULL_DISTANCE  = 48.0;
const float WATER_WALKER_MID_DISTANCE   = 96.0;
// 2026-10-18 用户定档：水面反射/折射步数 8/5/3 → 3/2/1（降本；step0 是主射线
// 本身，故仍保留一次反射/折射，只砍嵌套续接）。回退：改回 8/5/3。
//
// =====================================================
// 2026-11-16：3/2/1 → 6/4/2（修「看到第二个透射面时不透明、发白光」）。
//
// 3/2/1 定档时 walker 还带着「缺一个 }」的死代码 bug（见 938 注释）：无论
// 步数给多少，实际每条射线只打 1 次 trace，3/2/1 从未被真正执行过。修复后
// 预算生效，「水 → 第二透射面（玻璃 2 面 / 另一片水）→ 接收面」这类 3~4 跳
// 链路在中/远档必然耗尽，耗尽回退读作不透明的固定白光（用户报告）。
// 6/4/2 给 4 跳链路留足余量；撞天空/不透明即 return 的像素成本不变
//（step0 就返回），只有跨界面链路多付 trace。回退：改回 3/2/1。
// =====================================================
const int   WATER_WALKER_STEPS_FULL     = 6;
const int   WATER_WALKER_STEPS_MID      = 4;
const int   WATER_WALKER_STEPS_FAR      = 2;

// =====================================================
// 2026-10-08（B）：水相关 LOD 的**帧时间反馈环**。
//
// 输入是宿主写进 CameraUBO.rtSampleParams.w 的**平滑后实测 GPU 帧时间**（ms）。
// 水像素是整帧最吃射线的像素（反射/折射两条 walker 各 ≤8 步 + 水底 GI 弹射 +
// 太阳透射 march ≤6 步 + 局部光 RIS 与阴影），而开销与水面的**屏幕占比**成正比
// ⇒ 一靠近水帧时间没有上限。这里按实测帧时间收紧水面那几道**距离闸**，
// 于是"贵"会自己收敛：
//
//   ≤12ms    ：scale = 1.0（不收紧，与改动前的距离档完全一致）
//   12→22ms  ：线性收到 0.35
//   >22ms    ：保持 0.35（下限；0.35 × 96 格焦散闸 ≈ 33.6 格 ≈ 旧的 32 格档）
//
// lane = 0（宿主未启用 / 拿不到 GPU 时间戳）⇒ 恒 1.0 ⇒ **与改动前逐位相同**。
// 回退：ORION_WATER_ADAPTIVE=0 让宿主发送 0。
//
// 用法是把距离**阈值**按 scale 收紧（写成一律"除以 scale"的形式），不改步数表
// 与档位表本身 ⇒ 每一档的语义不变，只是"什么时候进入该档"被提前。
// =====================================================
// perfL10WaterAdaptiveScale() moved to rgen_trace_helpers.glsl (Batch AE).

// =====================================================
// 2026-11-18：**水面像素的射线预算降级**（用户："靠近水面的超级掉帧"）。
//
// 实测（本局 18:5x）：同一场景，远离水面 k2sun=0.5 ms，靠近水面 0.5 → 5 → 20 → 36 ms，
// 全帧 41 ms。原因：水面像素的**整套着色**（反射/折射 walker、界面循环、
// 太阳透射 march、焦散 + SSS）都跑在 K2-SUN 这条腿里（rgen_main_body.glsl:1938
// 的"水像素的着色在此完整结束"），所以成本 ∝ 水的屏幕占比。
// 原有机制 perfL10WaterAdaptiveScale 只把**距离阈值**收紧（除以 0.35），
// 并不减少每条射线的步数 —— 近处水像素仍然 6 步全开，所以在 36 ms 时它救不了。
//
// ===========================================================================
// 2026-11-18（最终口径，对齐 RTX Remix 的 thin-walled 水面）：
// **单次事件**。这个函数现在恒返回 1，是一个编译期常量 —— 不引入参数、
// 不产生第二份内联副本（此前用"参数 + min()"的写法，NV 编译器在冷编时要几分钟）。
//
// 依据：dxvk-remix 的水面 = 一层薄壁半透明表面，
//   · 反射/透射在**一个事件**里按 Fresnel 解析合成（brdf.slangh:520-539 的
//     闭式几何级数，内部多次反射用级数求和而不是循环）；
//   · 折射射线靠 penetrateSurface **继续走同一条路径**，介质吸收按真实距离
//     Beer-Lambert 一次算完（translucent_surface_material_interaction.slangh:516-525）；
//   · 因此他们的水面代价与水的屏幕占比无关。
//
// 实测（本局 23:10）：水面占屏时 k2sun 1.5 → 6 → 12 → 20 → 23.6 ms，
// 全帧 31 ms。根因是这里按距离档跑 3–6 步 × 反射/折射两瓣。
//
// 代价（已知）：看穿两层以上的水/玻璃（叠水、水后玻璃）只解析第一层。
// 回退：把 return 1 改回下面的 tierSteps（旧档 = 距离分档 + 帧时间收紧）。
// ===========================================================================
#ifndef WATER_WALKER_TRANSMIT_STEPS
// 2026-11-25: 透射瓣（reflectionLobe=false）允许最多 4 步以穿透水下冰块/玻璃
//（水侧壁 -> 冰底面 -> 冰下水面 -> 海底 = 4 步）。开阔水域 step0 命中海底即 return，
// 零额外射线；反射瓣（reflectionLobe=true）维持 1 步，守住海面掠射性能。
// A/B：-DWATER_WALKER_TRANSMIT_STEPS=1 回到旧档。
#define WATER_WALKER_TRANSMIT_STEPS 4
#endif

int waterTransparentWalkerSteps(float lodDistance)
{
    return 1;
}

// =====================================================
// Primary water transmission radiance
// =====================================================
vec3 tracePrimaryTransparentRadiance(
    vec3 startOrigin,
    vec3 startDirection,
    bool startInsideWater,
    vec3 sunDirection,
    bool enableWaterReceiverGI,
    float waterGILodDistance,
    // 相机可见的第一层透明面命中点（历史键）。见 TRANSPARENT_HISTORY_KEY_FIRST_SURFACE。
    vec3 historyKeyPosition,
    // 2026-10-22: 水面点太阳完全不可见时，反射天空不得再从遮挡物上方漏回
    // 日盘/日冕（bit 14/15，消费者在 rt_test.rmiss）。
    bool suppressSkySun,
    // 2026-11-16：反射瓣标记（true = 本 walker 在为水面反射 lobe 服务）。
    // 反射瓣撞到**另一片水**时走镜面而不是折射进水 —— 见下方
    // WATER_REFLECTION_LOBE_MIRROR。透射瓣（false）行为不变。
    bool reflectionLobe,
    out WaterTransmissionInfo transmissionInfo
)
{
#if VPT_TRANSPARENT_HISTORY_KEY && TRANSPARENT_HISTORY_KEY_FIRST_SURFACE
    // 只有相机射线打到的那一层，针孔投影才精确落在本像素上。嵌套路径里
    // startOrigin 是水面/第二片玻璃，投影在别的像素上 ⇒ 历史逐帧被拒。
    vec3 transparentHistoryKey = historyKeyPosition;
#else
    vec3 transparentHistoryKey = startOrigin;
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
    // 2026-11-16：相机水柱透射哨兵（负值 = 本 walker 未提供，消费端回退
    // 旧的单发公式）。真正赋值见 opaque 接收器分支。
    transmissionInfo.cameraWaterColumnTransmittance = vec3(-1.0);
    transmissionInfo.receiverSunTransmittance = vec3(0.0);
    transmissionInfo.receiverSunTransmittanceValid = 0.0;

    const float WATER_IOR =
        1.33;


const vec3 WATER_ABSORPTION =
    waterAbsorptionColorAt(startOrigin.xz);

// 已确认：0~3 格保持高通透。
const float WATER_SHALLOW_DISTANCE =
    3.0;

// 已确认：约 16 格后水底基本不可见。
const float WATER_EXTINCTION_DISTANCE =
    16.0;

// e^-4 ≈ 1.8%。
// 从 3 -> 16 格累计约 4 个 scattering optical depths。
const float WATER_SCATTERING =
    waterScatteringAmount();

const vec3 WATER_SCATTER_COLOR = waterScatterColorAt(startOrigin.xz);


    const float WATER_F0 =
        0.0200593;


    const float RAY_BIAS =
        0.001;


    vec3 throughput =
        vec3(1.0);
        vec3 accumulatedWaterScattering =
    vec3(0.0);

    // 2026-11-16：相机水柱透射的逐段累乘器（与 throughput 收到的水体乘法
    // 同源：Beer 吸收 × GLASS_TINT 混色 × 深水 0.15 压透）。opaque 接收器
    // 分支把它存进 transmissionInfo.cameraWaterColumnTransmittance 供焦散
    // 复用 —— 焦散端的单发公式在多段水程时与实际累乘不等，复用后以实值为准。
    vec3 waterColumnFactor =
        vec3(1.0);


    vec3 currentOrigin =
        startOrigin;


    vec3 currentDirection =
        normalize(
            startDirection
        );


    // Preserve the incident direction that actually entered water.  It is
    // used only when the refracted continuation incorrectly misses along a
    // shallow water/block boundary.
    vec3 waterEntryDirection =
        currentDirection;


    bool hasWaterEntryDirection =
        startInsideWater;


    // 2026-10-03 水体散射的太阳可见性门控。相机已在水下（startInsideWater）
    // 时不 trace：那条路的水色由 applyUnderwaterWaterColumn 负责，保持 1.0
    // 等于不改它的行为。
    float waterSunVisibility =
        startInsideWater ? 1.0 : 0.0;

    bool waterSunVisibilityTraced =
        false;

    const bool entrySunValid = g_waterEntrySunValid;
    const vec3 entrySunTrans = g_waterEntrySunTransmittance;
    const float entryCloudShadow = g_waterEntryCloudShadow;
    g_waterEntrySunValid = false;


    bool insideWater =
        startInsideWater;

    bool crossedTranslucent = false;
    bool floatingWaterExited = false;
    // 2026-11-27: 反射瓣恒为 1 步超轻量路径；透射瓣按距离给足 3~4 步穿出悬空水柱/冰海，
    // UBO 掩蔽防 spirv-opt 展开膨胀。
    int distTransmitSteps = (waterGILodDistance < 48.0) ? 4 : 3;
    const int transmitWalkerSteps =
        (cam.frameCount != 0xFFFFFFFFu)
            ? distTransmitSteps
            : 0;
    const int reflWalkerSteps =
        (cam.frameCount != 0xFFFFFFFFu) ? 1 : 0;
    const int maxWalkerSteps =
        reflectionLobe
            ? reflWalkerSteps
            : transmitWalkerSteps;
    int walkerStepBudget = maxWalkerSteps;
    int internalWaterSkips = 0;

    for(
        int step = 0;
        step < walkerStepBudget;
        ++step
    )
    {
        hit.hitDistance =
            1.0;

        hit.state = makeTraceState(
            insideWater ? RT_PURPOSE_WATER_MEDIUM : RT_PURPOSE_FULL
        ) | RT_STATE_GI_NO_POM_BIT;
        if(reflectionLobe || (!insideWater && waterGILodDistance >= 32.0) || g_waterWalkerSkipClouds)
        {
            hit.state |= RT_STATE_GI_SKIP_CLOUDS_BIT;
        }

#if WATER_SKY_SUN_OCCLUSION
        // 2026-10-22: shadowed water reflection.  Re-applied every step because a
        // glass/water hit rebuilds the state through makeHitState(); any miss in
        // this walker must not re-introduce the sun disk or its ±8° aureole.
        if(suppressSkySun)
        {
            hit.state |=
                RT_STATE_NO_SUN_DISK_BIT |
                RT_STATE_NO_SUN_GLOW_BIT;
        }
#endif

#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
        serTraceRay(
            currentOrigin,
            0.01,
            currentDirection,
            10000.0,
            secondaryTraverseFlags(),
            0xcfu,
            step
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
                step
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


        // =============================================
        // Miss
        //
        // miss shader 的 unpackAlbedoUnorm(payload.albedoPacked) 在这里代表天空结果，
        // 这种情况下它才是 Radiance，可以直接使用。
        // =============================================
if(payload.hitDistance < 0.0)
{
    if(reflectionLobe)
    {
        transmissionInfo.waterPathLength = 65504.0;
        return accumulatedWaterScattering +
            throughput * unpackAlbedoUnorm(payload.albedoPacked);
    }

    if(insideWater && hasWaterEntryDirection)
    {
        hit.hitDistance =
            1.0;

        hit.state = makeTraceState(RT_PURPOSE_WATER_MEDIUM) | RT_STATE_GI_NO_POM_BIT;

#if defined(RT_ENABLE_SER) && ORION_SER_FUSED_ONLY
        serTraceRay(
            currentOrigin,
            0.01,
            waterEntryDirection,
            10000.0,
            secondaryTraverseFlags(),
            0xcfu,
            step
        );
#else
#ifdef RT_ENABLE_SER
        if(cam.perfSerAb.z != 0u)
        {
            serTraceRay(
                currentOrigin,
                0.01,
                waterEntryDirection,
                10000.0,
                secondaryTraverseFlags(),
                0xcfu,
                step
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
                waterEntryDirection,
                10000.0,
                0
            );
        }
#endif

        if(
            payload.hitDistance >= 0.0 &&
            getLegacyMaterialId(payload.state) <= 0.5
        )
        {
            vec3 fallbackHitPos =
                currentOrigin +
                waterEntryDirection *
                payload.hitDistance;

            vec3 fallbackHitNormal =
                payload.normal;
            vec3 fallbackAlbedo =
                unpackAlbedoUnorm(payload.albedoPacked);

            transmissionInfo.waterPathLength +=
                payload.hitDistance;
            transmissionInfo.receiverWorldPos = fallbackHitPos;
            transmissionInfo.receiverNormal = fallbackHitNormal;
            transmissionInfo.receiverViewDirection = -waterEntryDirection;
            transmissionInfo.receiverAlbedo = fallbackAlbedo;
            transmissionInfo.receiverRoughness = payload.roughness;
            transmissionInfo.receiverF0Metal = payload.f0Metal;
            transmissionInfo.reachedUnderwaterReceiver = 1.0;
            transmissionInfo.waterSunVisibility = waterSunVisibility;
            transmissionInfo.receiverSelfEmissionRadiance = vec3(0.0);

            vec3 fallbackLocalDirect =
                evaluateVPTExactFaceDirectLighting(
                    fallbackHitPos,
                    fallbackHitNormal,
                    fallbackAlbedo
                ) +
                evaluateDynamicLightDeterministicDirect(
                    fallbackHitPos,
                    fallbackHitNormal,
                    fallbackAlbedo,
                    waterGILodDistance
                );

            vec3 fallbackRadiance =
                evaluateOpaqueSurfaceRadianceInMedium(
                    fallbackHitPos,
                    fallbackHitNormal,
                    -waterEntryDirection,
                    fallbackAlbedo,
                    payload.roughness,
                    payload.f0Metal,
                    payload.emission +
                        evaluateVLIEmitterRadianceAtHit(
                            fallbackHitPos,
                            fallbackHitNormal,
                            fallbackAlbedo
                        ),
                    payload.pomSelfShadow,
                    sunDirection,
                    true
                ) +
                fallbackLocalDirect +
                fallbackAlbedo *
                    evaluateDeterministicIndirectAmbient(fallbackHitNormal, waterSunVisibility);

#if WATER_VOLUME_INSCATTER_V2
            const vec3 fallbackExtinction =
                WATER_ABSORPTION * 1.35 +
                WATER_SCATTER_COLOR * WATER_SCATTERING * 0.35;
            return
                accumulatedWaterScattering +
                throughput *
                exp(-fallbackExtinction * payload.hitDistance) *
                fallbackRadiance;
#else
            return
                throughput *
                exp(
                    -WATER_ABSORPTION *
                    payload.hitDistance
                ) *
                fallbackRadiance;
#endif
        }
    }

return
    accumulatedWaterScattering +
    throughput *
    unpackAlbedoUnorm(payload.albedoPacked);
}


        // =============================================
        // 必须立刻保存命中数据。
        //
        // 后面的太阳可见性检测会继续覆盖 payload。
        // =============================================

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


        float hitF0Metal =
            payload.f0Metal;
            float hitRoughness =
    payload.roughness;



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


// =====================================================
// WATER VOLUME TRANSPORT
//
// 0~3 blocks:
//   absorption only, high transparency.
//
// >3 blocks:
//   absorption + scattering extinction.
//
// Scattering is accumulated as radiance separately from
// bottom/surface throughput.
// =====================================================
if(insideWater)
{
    transmissionInfo.waterPathLength +=
        hitDistance;

#if WATER_VOLUME_INSCATTER_V2
    const vec3 sigmaA = WATER_ABSORPTION * 1.35;
    const vec3 sigmaS = WATER_SCATTER_COLOR * WATER_SCATTERING * 0.35;
    const vec3 sigmaT = sigmaA + sigmaS;

    if(cam.waterSimState.w == 0u)
    {
        const vec3 singleScatAlbedo = sigmaS / max(sigmaT, vec3(1.0e-5));
        const vec3 skyLightDir = getTimeDrivenSkyLightDirection();
        const float muV = max(abs(currentDirection.y), 0.15);
        const float sinAirSq = max(0.0, 1.0 - skyLightDir.y * skyLightDir.y);
        const float muS = sqrt(max(1.0 - sinAirSq / (WATER_IOR * WATER_IOR), 0.16));
        const float kDepth = 1.0 + muV / muS;

        const float scatterCos = dot(currentDirection, skyLightDir);
        vec3 volumePhase;
        if(cam.waterParams.w > 0.5)
        {
            float rayleigh = (3.0 / (4.0 * 3.14159265359)) *
                (1.0 + scatterCos * scatterCos);
            float mie = min(underwaterHGPhase(scatterCos, 0.65), 1.6);
            vec3 phase3 = vec3(0.18, 0.42, 1.0) * (rayleigh * 0.12) +
                vec3(mie * 0.88);
            volumePhase = mix(phase3, vec3(0.26), 0.45);
        }
        else
        {
            volumePhase = vec3(
                mix(min(underwaterHGPhase(scatterCos, 0.6), 1.4), 0.25, 0.45));
        }

        const float waterEntryCos = clamp(skyLightDir.y, 0.0, 1.0);
        const float waterEntryOmc = 1.0 - waterEntryCos;
        const float waterEntryOmc2 = waterEntryOmc * waterEntryOmc;
        const float waterEntryFresnel = WATER_F0 +
            (1.0 - WATER_F0) * (waterEntryOmc2 * waterEntryOmc2 * waterEntryOmc);

        const vec3 directIntegral =
            (vec3(1.0) - exp(-sigmaT * (kDepth * hitDistance))) / kDepth;
        const vec3 directSunVis = entrySunValid
            ? (entrySunTrans * entryCloudShadow)
            : vec3(waterSunVisibility);
        const vec3 directInScatter =
            getTimeDrivenSkyLightColor() * 3.2 * getTimeDrivenSkyLightRadianceScale() *
            (1.0 - waterEntryFresnel) * directSunVis *
            singleScatAlbedo * volumePhase * directIntegral;

        const float openSkyGate = entrySunValid
            ? clamp(max(entrySunTrans.r, max(entrySunTrans.g, entrySunTrans.b)), 0.0, 1.0)
            : waterSunVisibility;
        const vec3 skyZenith =
            samplePhysicalClearSkyForAerialPerspective(vec3(0.0, 1.0, 0.0));
        const vec3 ambientIntegral = vec3(1.0) - exp(-sigmaT * hitDistance);
        const vec3 ambientInScatter =
            skyZenith * 0.22 * openSkyGate * singleScatAlbedo * ambientIntegral;

        accumulatedWaterScattering += throughput * (directInScatter + ambientInScatter);
    }

    const vec3 absorption = exp(-sigmaT * hitDistance);
    throughput *= absorption;
    waterColumnFactor *= absorption;
#else
    if(cam.waterSimState.w == 0u)
    {
        vec3 volumeExtinction = WATER_ABSORPTION + WATER_SCATTER_COLOR;
        float scatterCos = dot(currentDirection, getTimeDrivenSkyLightDirection());
        vec3 volumePhase;
        if(cam.waterParams.w > 0.5)
        {
            float rayleigh = (3.0 / (4.0 * 3.14159265359)) *
                (1.0 + scatterCos * scatterCos);
            vec3 phase3 = vec3(0.18, 0.42, 1.0) * (rayleigh * 0.05) +
                vec3(underwaterHGPhase(scatterCos, 0.6) * 0.95);
            volumePhase = mix(phase3, vec3(1.0), 0.35);
        }
        else
        {
            volumePhase = vec3(underwaterHGPhase(scatterCos, 0.6));
        }
        vec3 stepInScatter = vec3(1.0) - exp(-volumeExtinction * hitDistance);
        float waterEntryCos = clamp(getTimeDrivenSkyLightDirection().y, 0.0, 1.0);
        float waterEntryOmc = 1.0 - waterEntryCos;
        float waterEntryOmc2 = waterEntryOmc * waterEntryOmc;
        float waterEntryFresnel = WATER_F0 +
            (1.0 - WATER_F0) * (waterEntryOmc2 * waterEntryOmc2 * waterEntryOmc);
        accumulatedWaterScattering +=
            WATER_SCATTER_COLOR * WATER_SCATTERING *
            getTimeDrivenSkyLightColor() * getTimeDrivenSkyLightRadianceScale() *
            volumePhase * stepInScatter * throughput *
            (1.0 - waterEntryFresnel) *
            waterSunVisibility *
            UNDERWATER_COLUMN_SCATTER_BOOST;
    }

    const vec3 WATER_GLASS_TINT =
        vec3(
            0.20,
            0.75,
            0.78
        );

    const float WATER_END_DISTANCE =
        16.0;

    float waterFade =
        clamp(
            transmissionInfo.waterPathLength /
            WATER_END_DISTANCE,
            0.0,
            1.0
        );

    waterFade =
        waterFade *
        waterFade;

    vec3 absorption =
        exp(
            -WATER_ABSORPTION *
            hitDistance
        );

    throughput *= absorption;

    throughput =
        mix(
            throughput,
            throughput * WATER_GLASS_TINT,
            waterFade
        );

    throughput *=
        mix(
            1.0,
            0.15,
            waterFade
        );

    waterColumnFactor *= absorption;
    waterColumnFactor =
        mix(
            waterColumnFactor,
            waterColumnFactor * WATER_GLASS_TINT,
            waterFade
        );
    waterColumnFactor *=
        mix(
            1.0,
            0.15,
            waterFade
        );
#endif
}


// W1: an opaque receiver attenuated below this level contributes less
// than 3% of its air radiance.  Skipping it avoids the direct-sun water
// march, the VPT/RIS local-direct evaluation and the water-receiver GI
// bounce for work the eye cannot resolve.
//
// 2026-10-03 修老 bug「光源+玻璃+水之后就看不见光了」：水体吸收 + 深水压暗
// + 玻璃染色三重衰减把 throughput 压进 3% 时，旧档在这里直接 return 水体
// 散射 —— 后面的灯被整段丢弃（洞穴里水体散射又已归零 ⇒ 纯黑）。
// 发射块携带 ~10× 场景辐亮度（rchit: albedo × emission × 10），3% 残留
// 仍有 ~0.3，暗环境里清晰可见。这里补回 emission 项：非发射接收器不受
// 影响（emission = 0，加 0），太阳直射/VPT/GI 依旧按阈值跳过（性能不变）。
if(max(throughput.r, max(throughput.g, throughput.b)) < 0.03)
{
    return accumulatedWaterScattering + throughput * hitEmission;
}


        // =============================================
// Opaque surface
// =============================================

if(hitMaterialId <= 0.5)
{
    vec3 unifiedHitEmission =
        hitEmission +
        evaluateVLIEmitterRadianceAtHit(
            hitPos,
            hitNormal,
            hitAlbedo
        );

    if(reflectionLobe)
    {
        transmissionInfo.waterPathLength = hitDistance;
        // 2026-11-27: 水面反射禁用 SHaRC 短窗有噪缓存查询，统一走 100% 确定性着色，
        // 防止平静水面倒影沸腾闪动。
        vec3 reflSurfaceRadiance =
            unifiedHitEmission +
            evaluateDirectSunLightingDiffuseInMedium(
                hitPos,
                hitNormal,
                hitAlbedo,
                hitF0Metal,
                hitPOMSelfShadow,
                sunDirection,
                insideWater
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
                waterGILodDistance
            ) +
            hitAlbedo * evaluateDeterministicIndirectAmbient(hitNormal, waterSunVisibility);

        return accumulatedWaterScattering + throughput * reflSurfaceRadiance;
    }

    transmissionInfo.receiverWorldPos = hitPos;
    transmissionInfo.receiverNormal = hitNormal;
    transmissionInfo.receiverViewDirection = -currentDirection;
    transmissionInfo.receiverAlbedo = hitAlbedo;
    transmissionInfo.receiverRoughness = hitRoughness;
    transmissionInfo.receiverF0Metal = hitF0Metal;
    transmissionInfo.reachedUnderwaterReceiver = 1.0;
    transmissionInfo.waterSunVisibility = waterSunVisibility;
    transmissionInfo.cameraWaterColumnTransmittance = waterColumnFactor;
    transmissionInfo.receiverSelfEmissionRadiance =
        throughput * unifiedHitEmission;

    const bool captureSunForCaustics = (!reflectionLobe && insideWater);
    if(captureSunForCaustics)
    {
        g_captureDirectSunTrans = true;
        g_capturedDirectSunValid = false;
    }
    vec3 surfaceRadiance =
        evaluateOpaqueSurfaceRadianceInMedium(
            hitPos,
            hitNormal,
            -currentDirection,
            hitAlbedo,
            hitRoughness,
            hitF0Metal,
            unifiedHitEmission,
            hitPOMSelfShadow,
            sunDirection,
            insideWater
        );
    if(captureSunForCaustics)
    {
        g_captureDirectSunTrans = false;
        if(g_capturedDirectSunValid)
        {
            transmissionInfo.receiverSunTransmittance = g_capturedDirectSunTrans;
            transmissionInfo.receiverSunTransmittanceValid = 1.0;
        }
    }

    float skyOpenFactor = entrySunValid
        ? clamp(max(entrySunTrans.r, max(entrySunTrans.g, entrySunTrans.b)), 0.0, 1.0)
        : waterSunVisibility;

    surfaceRadiance +=
        evaluateVPTExactFaceDirectLighting(
            hitPos,
            hitNormal,
            hitAlbedo
        ) +
        evaluateDynamicLightDeterministicDirect(
            hitPos,
            hitNormal,
            hitAlbedo,
            waterGILodDistance
        ) +
        hitAlbedo * evaluateDeterministicIndirectAmbient(hitNormal, skyOpenFactor);

#if WATER_VOLUME_INSCATTER_V2
    return
        accumulatedWaterScattering +
        throughput *
        surfaceRadiance;
#else
    return
        throughput *
        surfaceRadiance;
#endif
}

if(hitMaterialId >= 2.5)
{
    if(reflectionLobe)
    {
        transmissionInfo.waterPathLength = hitDistance;
        vec3 gDir = normalize(reflect(currentDirection, hitNormal));
        return accumulatedWaterScattering +
            throughput * mix(
                hitAlbedo * evaluateDeterministicIndirectAmbient(hitNormal, 0.8),
                samplePhysicalClearSkyForAerialPerspective(gDir),
                0.35
            );
    }

    if(hitMaterialId < 3.5)
    {
        throughput *=
            glassSheetTransmission(
                hitAlbedo,
                payload.normal,
                currentDirection
            );
    }

    crossedTranslucent = true;

    currentOrigin =
        hitPos +
        currentDirection *
        RAY_BIAS;

    continue;
}

        hitNormal =
            waterInterfaceNormalForRay(
                hitNormal,
                currentDirection
            );

        if(reflectionLobe)
        {
            vec3 mirrorNormal =
                dot(currentDirection, hitNormal) < 0.0
                    ? hitNormal
                    : -hitNormal;

            float mirrorCos =
                clamp(
                    -dot(
                        currentDirection,
                        mirrorNormal
                    ),
                    0.0,
                    1.0
                );

            float mirrorOmc = 1.0 - mirrorCos;
            float mirrorOmc2 = mirrorOmc * mirrorOmc;
            float mirrorFresnel =
                WATER_F0 +
                (1.0 - WATER_F0) *
                (mirrorOmc2 * mirrorOmc2 * mirrorOmc);

            vec3 mirrorReflDir =
                normalize(reflect(currentDirection, mirrorNormal));
            vec3 mirrorSky = mix(
                WATER_SCATTER_COLOR * getSurfaceAmbientRadiance() * 0.6,
                samplePhysicalClearSkyForAerialPerspective(mirrorReflDir),
                smoothstep(-0.05, 0.25, mirrorReflDir.y)
            );
            vec3 mirrorBase =
                hitAlbedo * evaluateDeterministicIndirectAmbient(mirrorNormal, 0.85);
            transmissionInfo.waterPathLength = hitDistance;
            return accumulatedWaterScattering +
                throughput * mix(mirrorBase, mirrorSky, mirrorFresnel);
        }

        // 2026-11-27: 流水柱/阶梯水流内部相邻方块正面水片直穿（最多 2 次，不扣 Fresnel、不耗步数）。
        if(!crossedTranslucent && insideWater && hitMaterialId < 1.5 && hitDistance < 2.5 && internalWaterSkips < 2)
        {
            currentOrigin = hitPos + currentDirection * 0.005;
            ++internalWaterSkips;
            --step;
            continue;
        }

        if(!reflectionLobe && !crossedTranslucent)
        {
            if(insideWater && payload.hitDistance < 2.5 && currentDirection.y < -0.05)
            {
                currentOrigin = hitPos + currentDirection * 0.005;
                continue;
            }
#ifndef WATER_FLOATING_EXIT_CONTINUE
#define WATER_FLOATING_EXIT_CONTINUE 1
#endif
#if WATER_FLOATING_EXIT_CONTINUE
            if(!(insideWater || floatingWaterExited))
            {
                break;
            }
            if(insideWater)
            {
                floatingWaterExited = true;
            }
            walkerStepBudget =
                max(
                    walkerStepBudget,
                    min(step + 2, 4)
                );
#else
            break;
#endif
        }

        bool exitingWater =
            hitMaterialId > 1.5;

        // =====================================================
        // 2026-11-13：介质状态优先于面分类。
        //
        // insideWater 记录的是"刚才这一段确实在水里走"，它是权威的：
        // 水面几何是体素外壳 ⇒ 在水里能命中的水面只能是**出射面**，
        // 在空气里能命中的只能是**入射面**。而面分类（hitMaterialId）由
        // 三角形绕序判定（material_resolve.glsl 的 dot(geoNormal, rayDirection)），
        // 本仓库已在玻璃那段记录过"绕序对 MC 半透明 quad 不可靠"
        //（material_resolve.glsl 2100-2104）。
        //
        // 一旦某次判反，insideWater 会永久错位：之后整段**空气**都被当成水
        // 累加 waterPathLength 并乘 Beer 吸收 ⇒ 水柱轮廓内一片暗蓝
        //（waterFade = (path/16)² ⇒ 十几格就 mix 到 WATER_GLASS_TINT 并 ×0.15）；
        // 反射射线走同一条 walker（rgen_main_body 的反射调用点）时还会累加
        // accumulatedWaterScattering ⇒ 乳白雾状的"反射白光"。
        //
        // 分类一致时（水平水面、绕序正常）两式逐位相同。
        // A/B：-DWATER_MEDIUM_STATE_AUTHORITY=0 回到只看面分类。
        // =====================================================
#ifndef WATER_MEDIUM_STATE_AUTHORITY
#define WATER_MEDIUM_STATE_AUTHORITY 1
#endif
#if WATER_MEDIUM_STATE_AUTHORITY
        exitingWater = insideWater;
        // 刚穿过水下冰/玻璃块后命中正面进水面（RT_WATER_AIR_TO_WATER, hitMaterialId < 1.5）时，
        // 射线是离开固体半透明块重新入水，不能按 insideWater=true 强判成水→空气出射（防假 TIR 弹回天空）。
        if(crossedTranslucent && hitMaterialId < 1.5)
        {
            exitingWater = false;
        }
#endif
        crossedTranslucent = false;

        if(!exitingWater)
        {
            waterEntryDirection =
                currentDirection;

            hasWaterEntryDirection =
                true;

            // =================================================================
            // 2026-10-03 修「水体散射/SSS 无太阳环境发光」（老 bug
            // 「光源+玻璃+水看不见光」的观感来源之一）：S_volume、波峰透光、
            // 薄层 SSS 的光源是 getTimeDrivenSkyLight* —— 只门控时间与天气，
            // 洞穴/室内水体在正午照样满功率发光。首次进入水体时对水面入口点
            // 做一次太阳可见性 trace（与焦散同一约定：miss = 可见，命中 =
            // 被遮挡；玻璃染色等遮挡体同样算遮挡，与 caustics 口径一致）。
            // =================================================================
            if(!waterSunVisibilityTraced)
            {
                waterSunVisibilityTraced = true;
                if(sunDirection.y > 0.0)
                {
                    hit.hitDistance = 1.0;
                    hit.state =
                        makeTraceState(RT_PURPOSE_VISIBILITY_ONLY);
#if RTAO_VISIBILITY_EARLYOUT
                    // 2026-11-16：水体入口太阳可见性的 inline ray query 可选臂
                    // （ORION_VIS_QUERY bit2，cam.visQueryAb.z）。默认 0 = 走下方既有
                    // traceRayEXT，逐位旧行为。同一 TLAS / cullMask 0xff / tmin 0.02 /
                    // 同方向同射程；旗标只用 TerminateOnFirstHit（不带
                    // SkipClosestHitShader，理由见 AO 臂注释）。未命中写 -1.0，与
                    // rt_test.rmiss 对 purpose 3 的写入值一致（下面按 hitDistance < 0.0
                    // 判穿透 = 水体可见太阳）。
                    if(cam.visQueryAb.z != 0u)
                    {
                        rayQueryEXT receiverSunVisQuery;
                        #ifdef ORION_RT_STAGE_COUNTERS
                        rtCntRayIssued();
                        #endif
                        rayQueryInitializeEXT(
                            receiverSunVisQuery,
                            topLevelAS,
                            gl_RayFlagsTerminateOnFirstHitEXT,
                            0xcfu,
                            hitPos + sunDirection * 0.001,
                            0.02,
                            sunDirection,
                            10000.0
                        );

                        int receiverSunVisGuard = 0;
                        // 防死循环上限（参照 volumetric_fog.glsl 的 proceedGuard）。
                        // 2026-11-16 修正：committed 交点必须在查询终止后读，不能在
                        // 循环体里判（TerminateOnFirstHit 命中即终止 ⇒ 循环体可能一次
                        // 都不进 ⇒ 恒判未命中 ⇒ 水体恒判能见到太阳）。
                        while(rayQueryProceedEXT(receiverSunVisQuery) &&
                              ++receiverSunVisGuard < 32)
                        {
                            if(rayQueryGetIntersectionTypeEXT(receiverSunVisQuery, false) ==
                               gl_RayQueryCandidateIntersectionTriangleEXT)
                            {
                                rayQueryConfirmIntersectionEXT(receiverSunVisQuery);
                            }
                        }
                        if(rayQueryGetIntersectionTypeEXT(receiverSunVisQuery, true) !=
                            gl_RayQueryCommittedIntersectionNoneEXT)
                        {
                            hit.hitDistance =
                                rayQueryGetIntersectionTEXT(receiverSunVisQuery, true);
                        }
                        else
                        {
                            hit.hitDistance = -1.0;
                        }
                    }
                    else
#endif
                    // 2026-11-27 (Batch AI): visibility-only shadow ray skips SER overhead.
                    {
                        #ifdef ORION_RT_STAGE_COUNTERS
                        rtCntRayIssued();
                        #endif
                        traceRayEXT(
                            topLevelAS,
                            gl_RayFlagsTerminateOnFirstHitEXT |
                                gl_RayFlagsSkipClosestHitShaderEXT,
                            0xcfu,
                            0,
                            0,
                            0,
                            hitPos + sunDirection * 0.001,
                            0.02,
                            sunDirection,
                            10000.0,
                            0
                        );
                    }
                    waterSunVisibility =
                        hit.hitDistance < 0.0 ? 1.0 : 0.0;
                }
                transmissionInfo.waterSunVisibility =
                    waterSunVisibility;
            }
        }


        float cosTheta =
            clamp(
                -dot(
                    currentDirection,
                    hitNormal
                ),
                0.0,
                1.0
            );


        float cosThetaOmc = 1.0 - cosTheta;
        float cosThetaOmc2 = cosThetaOmc * cosThetaOmc;
        float fresnel =
            WATER_F0 +
            (
                1.0 -
                WATER_F0
            ) *
            (cosThetaOmc2 * cosThetaOmc2 * cosThetaOmc);


        float eta =
            exitingWater
                ? WATER_IOR
                : (1.0 / WATER_IOR);


        vec3 transmittedDirection =
            refract(
                currentDirection,
                hitNormal,
                eta
            );


        // =============================================
        // Total internal reflection
        // =============================================

        if(
            dot(
                transmittedDirection,
                transmittedDirection
            ) < 1.0e-6
        )
        {
            if(exitingWater && abs(hitNormal.y) <= 0.5)
            {
                // 2026-11-27: 竖直瀑布/水柱侧壁 TIR 不弹回水柱内死循环，平滑穿出到空气。
                throughput *= (1.0 - min(fresnel, 0.55));
                insideWater = false;
                currentOrigin = hitPos - hitNormal * 0.005;
                currentDirection = normalize(
                    hasWaterEntryDirection ? waterEntryDirection : currentDirection);
                continue;
            }

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


            // 没有跨介质。
            continue;
        }


        // 这里没有再做随机 roulette，
        // 所以透射能量必须显式乘 (1-F)。
        throughput *=
            1.0 -
            fresnel;


        insideWater =
            !exitingWater;


        currentOrigin =
            hitPos -
            hitNormal *
            RAY_BIAS;


        currentDirection =
            normalize(
                transmittedDirection
            );
    }


    // 2026-11-27: 取消耗尽后的 32 次 rayQuery 天空探测与黑白垂直渐变带，
    // 步数耗尽时零射线平滑收尾。
    if(insideWater)
    {
        return accumulatedWaterScattering +
            throughput * WATER_SCATTER_COLOR * getSurfaceAmbientRadiance() * 0.6;
    }
    return accumulatedWaterScattering +
        throughput * getSurfaceAmbientRadiance() * 0.5;
}

// =====================================================
// A2-1
// Primary ray-traced glossy reflection radiance
//
// 只负责：
// Primary opaque surface
// -> GGX reflection ray
// -> sky / opaque surface
//
// 如果中途撞到水：
// 允许真实 reflection / refraction 穿过水界面，
// 但水界面本身不作为最终 Radiance。
// =====================================================