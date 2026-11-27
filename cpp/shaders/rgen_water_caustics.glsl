
// ---- Analytic procedural water caustics -----------------------------------
//
// The wavefront-area density is a pure function of world position, so it is
// evaluated directly at the receiver instead of being baked into a 256x256
// window.  The physics is unchanged from the deleted `water_caustics.comp`:
// the refracted sun ray is traced from the flat water source plane onto a
// one-block-deep reference receiver plane and the area Jacobian of that map is
// inverted to get the light concentration.
//
// What changed is the domain.  The old map only existed inside a 32-block
// window centred on the player and faded to zero at that window's border, so
// the caustics formed a soft-edged patch that travelled with the camera.  Here
// the pattern extends as far as the sun reaches, with no border and no window.
//
// Cost: two Newton corrections plus a four-point stencil, i.e. six band-field
// evaluations, per lit underwater receiver.  Dry pixels never reach this code,
// and the interaction half is skipped unless the player's ripple simulation is
// live.
const float WATER_CAUSTIC_IOR = 1.33;
const float WATER_CAUSTIC_REFERENCE_DEPTH = 1.0;
// Same stencil half-width the compute pass used (32 blocks / 256 texels).  It
// doubles as the low-pass filter that keeps the density field from aliasing.
const float WATER_CAUSTIC_CELL = 0.125;
// The persistent ripple field rides on the natural surface as a lens; it is not
// a second wave set.  At its ~0.4-block wavelength its curvature is two orders
// of magnitude above the whole natural band sum, so its focusing is measured
// separately and scaled here before being added to the natural density.  1.0 is
// the raw Jacobian excess, which saturates the clamps into hard ribbons.
const float WATER_CAUSTIC_RIPPLE_LENS = 0.35;
// 2026-10-06 近水降本：焦散的距离闸（消费点见水体主分支）。水底在 16 格后
// 已被 Beer-Lambert 压到基本不可见（WATER_EXTINCTION_DISTANCE），远水的焦散
// 是纯浪费 —— 而它每个受光水底像素要 2 次牛顿 + 4 点 stencil（≈6 次波场求值）
// 外加一条 ≤6 步的太阳透射 march。回退：改大（如 1.0e9）即恒开。
//
// 2026-10-07（W1）：96 → 32。上面这段理由本身就写着"16 格后不可见"，
// 而闸取的是它的 **6 倍** —— 也就是说 16~96 格这一整段（水面看着最舒服的中距离，
// 通常就是湖面/海面正对镜头的面积）仍在逐像素付"2 次牛顿 + 4 点 stencil + 太阳
// march"的钱，画面上却什么都看不到。32 = 2×WATER_EXTINCTION_DISTANCE：留两倍
// 余量（斜视时透射率的可见边界比正视远一点），仍把最贵的那一段砍掉。
// 觉得近了就 48，想彻底回旧档 96；1.0e9 = 恒开。
// 2026-10-08：32 → 96（用户实测：**越过 32 格水面直接变黑/变暗**）。
// 32 是 2026-10-07（W1）从 96 收上来的，理由是"16 格后水底基本不可见 ⇒ 远水
// 焦散是纯浪费"。但实测**看得见**：32 格那个位置正好是湖面/海面正对镜头、
// 斜视最重的一段，也就是焦散最显眼的一段（斜视时透射率的可见边界远大于正视，
// 上面那段注释自己也承认"斜视时比正视远一点"，32 显然收得太狠）。
// ⇒ 回 96（注释里给的"旧档"值），保留 >96 那一段的降本。
//   · 再收一档：48    · 恒开：1.0e9    · 彻底回旧档：96（当前值）
const float WATER_CAUSTIC_MAX_DISTANCE = 96.0;

// ===========================================================================
// 2026-11-20：**太阳掠射角钳制（水底焦散）** —— 用户指认的回归项。
//
// 症状：太阳低（掠射）时水底出现"闪烁的三角形"。
//
// 成因（本文件的数学结构决定的）：密度 = 1/|det(dq/dp)|，其中 q 是折射光线
// 打到"参考接收面"上的落点。q 对源的导数里有一项 ∝ refDepth / incoming.y²，
// incoming.y 是**折射后**光线在竖直方向的分量 —— 太阳掠射时它趋近 0，于是
// 该导数项发散：
//   · 有的源点 det ≈ 0（极端聚焦 ⇒ 密度打到上限 6）
//   · 紧邻的源点 det 巨大（⇒ 密度趋近 0）
// 再经过下游 `pow(causticFocus, 1.35)` 的锐化和 `lodBias = -1.0` 的保锐，
// 这些近奇异的跳变就成了硬边碎片；波浪相位每帧变化 ⇒ 碎片在**三角形上**闪。
//
// 钳制（两条一起，都是"随掠射角把焦散溶回未扰动波前"）：
//   1) 图案溶解：grazing → 1 时把密度插值回 1.0（未扰动）。1.0 是不加光的
//      基准（见本文件末尾 "Density 1 is the unperturbed wavefront"），所以
//      只消失图案，不改变水底整体亮度 —— 不会出现"低太阳水底发黑"。
//   2) mip 加宽：焦散图按掠射角额外 `+2.0` 级模糊，把已经发散的高频压平。
//
// 门限取的是**折射后**仰角（receiverToSun.y），不是太阳本身仰角；两者差一个
// 折射率 1.33，太阳 8° 时折射后约 6°，所以 LO=0.10 / HI=0.34 大约对应
// 太阳 7.6° ~ 26°。
// 回退：把 WATER_CAUSTIC_GRAZING_CLAMP 改成 0（逐位回到改前行为）。
// ===========================================================================
#ifndef WATER_CAUSTIC_GRAZING_CLAMP
// 2026-11-20 注：这是我为"水底闪烁三角形"加的第一版猜测（掠射时把焦散图案
// 溶回未扰动）。用户确认目标不是它 ⇒ **默认关闭**，编译期整段消失。
// 需要 A/B 时置 1。
#define WATER_CAUSTIC_GRAZING_CLAMP 0
#endif
const float WATER_CAUSTIC_GRAZING_LO = 0.10;
const float WATER_CAUSTIC_GRAZING_HI = 0.34;
const float WATER_CAUSTIC_GRAZING_MAX_BLEND = 0.85;
const float WATER_CAUSTIC_GRAZING_LOD_BOOST = 2.0;

// 2026-11-21 水体性能优先 + 光学画质升级（每项独立可摘除开关）
#ifndef WATER_VOLUME_INSCATTER_V2
#define WATER_VOLUME_INSCATTER_V2 1
#endif
#ifndef WATER_RECEIVER_GI_ADAPTIVE_GATE
#define WATER_RECEIVER_GI_ADAPTIVE_GATE 1
#endif
#ifndef WATER_SSS_FORWARD_SIGN_AND_BACKLIT_FIX
#define WATER_SSS_FORWARD_SIGN_AND_BACKLIT_FIX 1
#endif
#ifndef WATER_CAUSTIC_FAST_INTERACTION_LENS
#define WATER_CAUSTIC_FAST_INTERACTION_LENS 1
#endif
#ifndef WATER_CAUSTIC_DEFERRED_SUN_TRACE
#define WATER_CAUSTIC_DEFERRED_SUN_TRACE 1
#endif
#ifndef WATER_CAUSTIC_DRIFT_AND_DEPTH_SYNC
#define WATER_CAUSTIC_DRIFT_AND_DEPTH_SYNC 1
#endif
#ifndef WATER_CAUSTIC_DEPTH_FOCUS
#define WATER_CAUSTIC_DEPTH_FOCUS 1
#endif

// per-invocation 入口光照缓存：rgen_main_body 算一次入口太阳透射与云影，
// 供 rgen_water_receiver（水下入散射）与 rgen_water_caustics（云影）复用并立即清零。
bool  g_waterEntrySunValid = false;
vec3  g_waterEntrySunTransmittance = vec3(1.0);
float g_waterEntryCloudShadow = 1.0;

// 0 = 太阳充足（逐位旧行为），1 = 完全掠射（图案溶解到未扰动）。
float causticGrazingBlend(float refractedSunY)
{
#if WATER_CAUSTIC_GRAZING_CLAMP
    return (1.0 - smoothstep(
        WATER_CAUSTIC_GRAZING_LO,
        WATER_CAUSTIC_GRAZING_HI,
        refractedSunY)) * WATER_CAUSTIC_GRAZING_MAX_BLEND;
#else
    return 0.0;
#endif
}

// The caustic map's own normal: the natural bands plus the interaction field
// with only its state/border mask.  It deliberately does *not* apply the
// interaction elevation gate here -- the caller weights the ripple's focusing
// excess by that gate instead.
vec3 causticWaveNormal(vec2 sourceXZ, bool includeInteraction)
{
    float cameraDistance;
 // 焦散与可见水面同源：用相机→该水面点的视线算掠射衰减。
    vec3 surfacePos = vec3(sourceXZ.x, cam.waterSimSurfaceAndOrigin.x, sourceXZ.y);
    float noV = abs(normalize(surfacePos - cam.viewInverse[3].xyz).y);
    float horizonFade = 1.0 - exp(-4.0 * noV);
    // 2026-10-18 方案 A：焦散与可见水面同源，mip 级也用屏幕足迹定。焦散无命中
    // 距离，用相机→该水面点的距离近似 rayDistance。
    float projScale = float(rtLaunchSize().y)
        / (2.0 * max(tan(cam.cameraParams.z * 0.5), 0.05));
    float causticRayDistance =
        max(length(surfacePos - cam.viewInverse[3].xyz), 1.0e-3);
    float waveLod =
        waterFftSlopeLodFromProj(surfacePos, causticRayDistance, projScale);
    vec2 naturalSlope = waterNaturalWaveGradient(sourceXZ, cameraDistance, horizonFade, waveLod);

    // 2026-10-02 参考重构（HPWater）：与可见水面用**同一个**混合公式
    //（单一真源契约：焦散必须与画面法线同源，否则焦散纹理会从波峰上漂走）。
    vec2 interactionSlope = vec2(0.0);
    if(includeInteraction)
    {
        float stateAndBorderMask;
        interactionSlope =
            waterInteractionSlope(sourceXZ, stateAndBorderMask) *
            stateAndBorderMask;
    }
    vec2 gradient = waterBlendWaveSlopes(
        naturalSlope, interactionSlope, waterWaveBlendMode());

    return normalize(vec3(-gradient.x, 1.0, -gradient.y));
}

vec2 projectCausticWavefront(
    vec2 sourceXZ,
    vec3 sunDirection,
    bool includeInteraction
)
{
    vec3 normal = causticWaveNormal(sourceXZ, includeInteraction);
    vec3 incoming = refract(-sunDirection, normal, 1.0 / WATER_CAUSTIC_IOR);
    if(incoming.y >= -1.0e-4)
    {
        return sourceXZ;
    }
    return sourceXZ +
        incoming.xz * (-WATER_CAUSTIC_REFERENCE_DEPTH / incoming.y);
}

// The output grid is the refracted receiver plane, not the original water
// source plane.  Invert the one-block wavefront projection with two fixed
// Newton-style corrections, then evaluate the local oldArea/newArea at the
// recovered source.
vec2 invertCausticWavefrontProjection(
    vec2 receiverPlaneXZ,
    vec3 sunDirection,
    bool includeInteraction
)
{
    vec2 sourceXZ = receiverPlaneXZ;
    for(int iteration = 0; iteration < 2; ++iteration)
    {
        vec2 projected =
            projectCausticWavefront(sourceXZ, sunDirection, includeInteraction);
        sourceXZ -= 0.85 * (projected - receiverPlaneXZ);
    }
    return sourceXZ;
}

// Evan's oldArea/newArea construction through a finite-difference Jacobian.
// The flat Jacobian is exactly one, so density is 1 / |det(dq/dp)| and density
// 1 means "unperturbed wavefront, no extra light".
float evaluateCausticWavefrontDensity(
    vec2 sourceXZ,
    vec3 sunDirection,
    bool includeInteraction
)
{
    vec2 qXP = projectCausticWavefront(
        sourceXZ + vec2(WATER_CAUSTIC_CELL, 0.0),
        sunDirection,
        includeInteraction
    );
    vec2 qXM = projectCausticWavefront(
        sourceXZ - vec2(WATER_CAUSTIC_CELL, 0.0),
        sunDirection,
        includeInteraction
    );
    vec2 qZP = projectCausticWavefront(
        sourceXZ + vec2(0.0, WATER_CAUSTIC_CELL),
        sunDirection,
        includeInteraction
    );
    vec2 qZM = projectCausticWavefront(
        sourceXZ - vec2(0.0, WATER_CAUSTIC_CELL),
        sunDirection,
        includeInteraction
    );

    vec2 dqdx = (qXP - qXM) / (2.0 * WATER_CAUSTIC_CELL);
    vec2 dqdz = (qZP - qZM) / (2.0 * WATER_CAUSTIC_CELL);
    float newArea = abs(dqdx.x * dqdz.y - dqdx.y * dqdz.x);
    return clamp(1.0 / max(newArea, 0.10), 0.20, 6.0);
}

// ── 批次3-P1（waterParams.x，0=关）：波峰透光（HPWater diffT 背光透射移植）──
// 阳光穿过薄波峰的强前向散射辉光：G_backlit = saturate(dot(N, -L))（背光坡面，
// HP 同构）× Beer(-absorption·水厚) × 前向窄瓣。窄瓣用 cos^700 代替 HP 的
// HG(g=0.9998)——半角约 2.6°、峰值归一，数值安全且强度可标定；只在"看向
// 太阳方位的水面"处非零，重现逆光时波峰/浅水整条发亮的银边透光。
vec3 evaluateWaterCrestBacklit(
    vec3 waterSurfacePos,
    vec3 surfaceNormal,
    vec3 rayDirection,
    vec3 sunDirectionLit,
    const in WaterTransmissionInfo receiver
)
{
    float strength = clamp(cam.waterParams.x, 0.0, 2.0);
    if(strength <= 0.001 || sunDirectionLit.y <= 0.02)
    {
        return vec3(0.0);
    }

    float backlitFace = clamp(dot(surfaceNormal, -sunDirectionLit), 0.0, 1.0);
    if(backlitFace <= 0.001)
    {
        return vec3(0.0);
    }

    // 水厚：有真实水下接收器时用其路径长度（浅水透光、深水被 Beer 压掉），
    // 无接收器（全反射/未达水底）用保守薄层 0.6 格（纯波峰尺度）。
    float thickness = receiver.reachedUnderwaterReceiver > 0.5
        ? clamp(receiver.waterPathLength, 0.05, 6.0)
        : 0.6;

    // 视线-太阳夹角窄瓣：视线压到太阳仰角附近时 dot(rayDir, L) → 1
    float cosViewSun = dot(rayDirection, sunDirectionLit);
#if WATER_SSS_FORWARD_SIGN_AND_BACKLIT_FIX
    // Physical wave-crest forward transmission cone (~18-25° half-angle): active at low sun
    // when viewing waves toward the sun azimuth, tinted by biome water scatter color.
    float phaseBacklit = pow(clamp(cosViewSun, 0.0, 1.0), 12.0);
    vec3 transmitted = waterScatterColorAt(waterSurfacePos.xz) *
        exp(-waterAbsorptionColorAt(waterSurfacePos.xz) * thickness * 1.5);
    vec3 sunRadiance =
        getTimeDrivenSkyLightColor() *
        getTimeDrivenSkyLightRadianceScale();

    return sunRadiance * backlitFace * phaseBacklit * transmitted *
        strength * 0.45;
#else
    float phaseBacklit = pow(clamp(cosViewSun, 0.0, 1.0), 700.0);

    vec3 transmitted = exp(-waterAbsorptionColorAt(waterSurfacePos.xz) * thickness * 1.5);
    vec3 sunRadiance =
        getTimeDrivenSkyLightColor() *
        getTimeDrivenSkyLightRadianceScale();

    return sunRadiance * backlitFace * phaseBacklit * transmitted *
        strength * 0.35;
#endif
    }

    // ===========================================================================
    // 2026-10-03 参考重构（HPWaterBSDFLibary.hlsl / WaterEvaluateBSDF Part 2-3）：
    // **薄层次表面散射 thinLayerSSS**（上面 P1 的窄瓣背光透射只覆盖了 diffT 的
    // backlitTransmission 那一半，这里补上完整的薄层 SSS）。
    //
    // 参考构成与常量：
    //   L_linear    = thickness * HPWATER_SSS_PATH_SCALE                 (20.0)
    //   L_nonlinear = thickness² * HPWATER_SSS_PATH_SCALE * (1+Lum(scatter))
    //   sssPath     = lerp(L_linear, L_nonlinear,
    //                      saturate(opticalDepth * HPWATER_SSS_NONLINEAR_STRENGTH))  (0.5)
    //   S_sss       = scatterColor * exp(-extinction * sssPath) * P_sss(dot(-V, L), _PhaseG)
    //   G_sss       = 1 - clampedNdotL        ← 正面弱化、背面主导
    //   thinLayerSSS= S_sss * G_sss * HPWATER_SSS_SCATTER_BOOST                       (2.0)
    //
    // 我们的映射（不新增 descriptor / UBO lane）：
    //   · thickness 直接用已有的逐格水厚 receiver.waterPathLength（与 P1 同一真源）；
    //   · 光程缩放取 1.5 —— 与既有 Beer 项 exp(-absorption*thickness*1.5) 同一单位，
    //     即"参考的 20（米）在我们的格单位下的等价"；
    //   · 散射色：我们只有一条水色真源 waterAbsorptionColor()，按 0.6 取散射分量
    //     （参考是 absorption / scatter 两条独立真源）；
    //   · 相位复用已有的 underwaterHGPhase(cos, g)，g 取参考的 0.8（宽瓣）；
    //   · opticalDepth 复用既有 Beer 指数；整个项由既有「波峰透光」旋钮
    //     cam.waterParams.x 统一驱动 ⇒ 0 = 关 = 逐位旧档。
    // 观感标定：末尾 * 1.4 与散射色 * 0.6 是首次实机标定系数；太亮/太暗先动
    // 「波峰透光」强度（0..2），再考虑这两个常数。
    // ===========================================================================
    vec3 evaluateWaterThinLayerSSS(
    vec3 waterSurfacePos,
    vec3 surfaceNormal,
    vec3 rayDirection,
    vec3 sunDirectionLit,
    const in WaterTransmissionInfo receiver
    )
    {
    float strength = clamp(cam.waterParams.x, 0.0, 2.0);
    if(strength <= 0.001 || sunDirectionLit.y <= 0.02)
    {
        return vec3(0.0);
    }

    // =====================================================
    // 2026-11-18（用户："次表面散射如果他们有就抄一下"）：
    // 换成 RTX Remix 的闭式单次前向散射 —— Hanrahan 1993 单层虚拟体积的一阶
    // 近似，dxvk-remix 的 src/dxvk/shaders/rtx/utility/brdf.slangh:734-781
    // （evalHanrahanSingleScatteringDiffuseTransmission）。零射线、零循环。
    //
    // 公式（他们注释里的 "First-Order forward scattered radiance equation"）：
    //   weight = albedo * (1-Fo) * (1-Fi) * (1/|cosO - cosI|)
    //            * HG(g, cosVL) * |T1 - T2|
    //   T1 = exp(-σ * d / cosO)，T2 = exp(-σ * d / cosI)
    // 当 cosO ≈ cosI 时退化为导数式（他们处理了奇异点）。
    //
    // 映射（全部沿用现成量，不改水的形状/法线/波形）：
    //   σ      = waterAbsorptionColor() + waterScatterColor()（每格消光，与
    //            tracePrimaryTransparentRadiance 的 exp(-WATER_ABSORPTION*d) 同口径）
    //   d      = 真实水程（receiver.waterPathLength），薄水兜底 0.6
    //   albedo = scatter / σ（单次散射反照率）
    //   Fo/Fi  = 水面 F0 的 Schlick 菲涅尔（入射/出射两侧）
    //   g      = cam.waterSssParams.z（UI"相位"滑杆）
    // 原"非线性光程 + ×2 能量补偿 + G_sss = 1-NdotL"经验项整体退役。
    // =====================================================
    const float nDotV = clamp(dot(surfaceNormal, -rayDirection), 0.0, 1.0);
    const float nDotL = clamp(dot(surfaceNormal, sunDirectionLit), 0.0, 1.0);

    if(nDotV < 1.0e-4 || nDotL < 1.0e-4)
    {
        return vec3(0.0);
    }

    float thickness = receiver.reachedUnderwaterReceiver > 0.5
        ? clamp(receiver.waterPathLength, 0.05, 6.0)
        : 0.6;

    const float kSssNonlinear = clamp(cam.waterSssParams.x, 0.0, 1.0);
    const float kSssBoost = clamp(cam.waterSssParams.y, 0.0, 4.0);
    const float kSssPhaseG = clamp(cam.waterSssParams.z, 0.0, 0.95);
    const float kSssStrength = clamp(cam.waterSssParams.w, 0.0, 2.0);

    const vec3 scatter = waterScatterColorAt(waterSurfacePos.xz);
    const vec3 extinction = waterAbsorptionColorAt(waterSurfacePos.xz) + scatter;
    const vec3 singleScatteringAlbedo = scatter / max(extinction, vec3(1.0e-5));

    // Fo/Fi：水面 F0 的 Schlick（不含 TIR；从上方看水面不会 TIR）。
    const float WATER_F0_SSS = 0.0200593;
    const float outsideFresnel =
        WATER_F0_SSS + (1.0 - WATER_F0_SSS) * pow(1.0 - nDotV, 5.0);
    const float insideFresnel =
        WATER_F0_SSS + (1.0 - WATER_F0_SSS) * pow(1.0 - nDotL, 5.0);

    // 两端 Beer-Lambert（d = 真实水程；cos 越小路径越长）。
    const vec3 firstAttenuation = exp(-extinction * thickness / nDotV);
    const vec3 secondAttenuation = exp(-extinction * thickness / nDotL);

#if WATER_SSS_FORWARD_SIGN_AND_BACKLIT_FIX
    // Match dxvk-remix brdf.slangh:742 (-v·l = dot(rayDirection, sunDirectionLit))
    // so positive anisotropy g peaks when viewing sunlit water toward the sun.
    const float cosTheta = clamp(dot(rayDirection, sunDirectionLit), -1.0, 1.0);
    const float phaseWeight = min(underwaterHGPhase(cosTheta, kSssPhaseG), 2.5);
#else
    const float cosTheta = clamp(dot(-rayDirection, sunDirectionLit), -1.0, 1.0);
    const float phaseWeight = underwaterHGPhase(cosTheta, kSssPhaseG);
#endif

    const float cosineDifference = nDotV - nDotL;

    vec3 btdf;
    if(abs(cosineDifference) >= 1.0e-4)
    {
        btdf =
            singleScatteringAlbedo * (1.0 - outsideFresnel) *
            (1.0 - insideFresnel) * (1.0 / abs(cosineDifference)) *
            phaseWeight * abs(firstAttenuation - secondAttenuation);
    }
    else
    {
        // 奇异点导数式（brdf.slangh:774-780 的同构处理）。
        btdf =
            singleScatteringAlbedo * (1.0 - outsideFresnel) *
            (1.0 - insideFresnel) *
            (extinction * thickness / max(nDotL * nDotL, 1.0e-5)) *
            phaseWeight * secondAttenuation;
    }

    // kSssNonlinear 保留为"水程放大量"（0 ⇒ 严格真实水程；1 ⇒ 2×）。
    btdf *= 1.0 + kSssNonlinear;

    vec3 sunRadiance =
        getTimeDrivenSkyLightColor() *
        getTimeDrivenSkyLightRadianceScale();

    // BTDF 不带入射余弦，这里补 n·l（与 Remix 调用方 throughput 的口径一致）。
    return sunRadiance * btdf * nDotL *
        (kSssBoost * strength * kSssStrength * 1.4);
    }

// 2026-10-19：焦散图的显式 mip 级。与 waterFftSlopeLodFromProj 同构，只是
// 纹素尺寸 = 平铺周期 / 1024（宿主 WATER_CAUSTIC_MAP_RESOLUTION，2026-11-15
// 起真的是 1024 —— 之前宿主是 512 而这里按 1024 算，LOD 白高 1 级，白送
// 一层 mip 模糊）。RT 阶段 texture() 的隐式 LOD 未定义 ⇒ 采样点一律显式给级。
float waterCausticMapLodFromProj(
    vec3 receiverPos,
    float rayDistance,
    float projScale)
{
    if(projScale <= 0.0)
    {
        return 0.0;
    }
    const float tilePeriod = max(cam.waterFftParams.x, 1.0e-3);
    const float texelWorld = tilePeriod / 1024.0;
    // 2026-11-16（用户反馈「焦散要线线分明」）：偏置 -0.75 → -1.0，中景再多
    // 保一级不进 mip 模糊，线条在更远的距离仍保持锐利；最远景仍由 maxLod
    // 兜底抗闪。回退 = 改回 -0.75。远景焦散出现闪烁就把这里往回收。
    const float lodBias = -1.0;
    const float maxLod = 6.0;
    vec3 toCam = receiverPos - cam.viewInverse[3].xyz;
    float dist = max(length(toCam), 1.0e-3);
    float noV = max(abs(toCam.y) / dist, 0.04);
    float worldPerPixel = max(rayDistance, 1.0e-3) / (projScale * noV);
    // 锐化：偏置 -0.75 级并把上限收到 6（原 10），近中景更锐，下界 0 保住薄膜纹。
    return clamp(
        log2(max(worldPerPixel / texelWorld, 1.0e-4)) + lodBias,
        0.0,
        maxLod);
}

vec3 evaluatePrimaryWaterReceiverCaustics(
    vec3 waterSurfacePosition,
    WaterTransmissionInfo receiver,
    vec3 sunDirection
)
{
    const float WATER_IOR = 1.33;
    const vec3 WATER_ABSORPTION = waterAbsorptionColorAt(waterSurfacePosition.xz);
    const vec3 WATER_GLASS_TINT = vec3(0.20, 0.75, 0.78);
    const float WATER_CAUSTIC_STRENGTH = waterStyleCausticStrength();

    if(WATER_CAUSTIC_STRENGTH <= 0.001)
    {
        return vec3(0.0);
    }

    if(
        receiver.reachedUnderwaterReceiver <= 0.5 ||
        receiver.waterPathLength <= 0.0 ||
        sunDirection.y <= 0.0
    )
    {
        return vec3(0.0);
    }

    vec3 receiverN = normalize(receiver.receiverNormal);
    vec3 receiverV = normalize(receiver.receiverViewDirection);
    vec3 L = normalize(sunDirection);
    float waterDepth =
        max(waterSurfacePosition.y - receiver.receiverWorldPos.y, 0.0);

    if(waterDepth < 0.08 || cam.waterSimSurfaceAndOrigin.w <= 0.0)
    {
        return vec3(0.0);
    }

    // The wave field lives on the flat water source plane.  Do not use the
    // camera ray's primary water normal here: the deformed refractive normals
    // already define the field, and folding the view-dependent normal back in
    // would make the caustics camera-dependent again.
    vec3 incomingWaterSun = refract(-L, vec3(0.0, 1.0, 0.0), 1.0 / WATER_IOR);
    if(dot(incomingWaterSun, incomingWaterSun) < 1.0e-6)
    {
        return vec3(0.0);
    }

    vec3 receiverToSun = normalize(-incomingWaterSun);
    float NdotL = max(dot(receiverN, receiverToSun), 0.0);
    float NdotV = max(dot(receiverN, receiverV), 0.0);
    if(NdotL <= 0.0 || NdotV <= 0.0 || receiverToSun.y <= 0.0)
    {
        return vec3(0.0);
    }

#if WATER_CAUSTIC_DEFERRED_SUN_TRACE
    vec3 cameraWaterTransmittance = receiver.cameraWaterColumnTransmittance;
    if(any(lessThan(cameraWaterTransmittance, vec3(0.0))))
    {
#if WATER_VOLUME_INSCATTER_V2
        const vec3 causticColumnExtinction =
            WATER_ABSORPTION * 1.35 +
            waterScatterColorAt(waterSurfacePosition.xz) * waterScatteringAmount() * 0.35;
        cameraWaterTransmittance = exp(-causticColumnExtinction * receiver.waterPathLength);
#else
        float cameraWaterFade = clamp(receiver.waterPathLength / 16.0, 0.0, 1.0);
        cameraWaterFade *= cameraWaterFade;
        cameraWaterTransmittance =
            exp(-WATER_ABSORPTION * receiver.waterPathLength);
        cameraWaterTransmittance = mix(
            cameraWaterTransmittance,
            cameraWaterTransmittance * WATER_GLASS_TINT,
            cameraWaterFade
        );
        cameraWaterTransmittance *= mix(1.0, 0.15, cameraWaterFade);
#endif
    }
    if(max(cameraWaterTransmittance.r, max(cameraWaterTransmittance.g, cameraWaterTransmittance.b)) < 0.008)
    {
        return vec3(0.0);
    }
#else
    vec3 sunTransmittance = traceSunTransmittanceThroughWaterEx(
        receiver.receiverWorldPos + receiverN * 0.01,
        receiverToSun,
        L
    );
    if(max(sunTransmittance.r, max(sunTransmittance.g, sunTransmittance.b)) <= 0.0)
    {
        return vec3(0.0);
    }
#endif

    vec2 flatProjectedWaterXZ =
        receiver.receiverWorldPos.xz +
        receiverToSun.xz *
        (waterDepth / max(receiverToSun.y, 0.06));
    // The density field is defined on the one-block-deep reference receiver
    // plane used by its inverse wavefront projection, not at the flat water
    // source coordinate.  Undo that shift so the pattern stays world-stable as
    // the actual receiver depth changes.
    vec2 causticReceiverPlaneXZ =
        flatProjectedWaterXZ -
        receiverToSun.xz * (1.0 / max(receiverToSun.y, 0.06));

    // ── 2026-10-19 参考重做：自然焦散 = 一次地图采样 ──
 // 做法：宿主每帧把 FFT 位移网格前向光栅化成接收面焦散图
    //（binding 69），这里只按接收面纹素足迹选 mip 再查一次。自然波的面积比、
    // 3×3 片拼接与三通道 IOR 色散都已经烘在图的 RGB 里。
    float projScale = float(rtLaunchSize().y) /
        (2.0 * max(tan(cam.cameraParams.z * 0.5), 0.05));
    float receiverRayDistance = max(
        length(receiver.receiverWorldPos - cam.viewInverse[3].xyz),
        1.0e-3);
    float causticMapLod = waterCausticMapLodFromProj(
        receiver.receiverWorldPos,
        receiverRayDistance,
        projScale);
    // 2026-11-20：太阳掠射角钳制 —— 掠射时焦散的雅可比趋近奇异，图案在
    // 像素级发散成硬边碎片（"水底闪烁三角形"）。这里按掠射程度额外糊 mip，
    // 让已经过采样的高频先被抹平；真正消掉碎片的是下面把密度插回 1.0。
    const float grazingBlend =
        causticGrazingBlend(receiverToSun.y);
#if WATER_CAUSTIC_GRAZING_CLAMP
    causticMapLod = min(
        causticMapLod + WATER_CAUSTIC_GRAZING_LOD_BOOST * grazingBlend,
        8.0);
#endif
#if WATER_CAUSTIC_DEPTH_FOCUS
    // Deep water broadens caustic focus bands smoothly via mip chain; shallow-to-mid stays crisp.
    float depthFocusLodBoost = smoothstep(2.2, 10.5, waterDepth) * 2.2;
    causticMapLod = min(causticMapLod + depthFocusLodBoost, 8.0);
#endif

    // 地图不可用（模块软失败 / FFT 关闭）时退回 1.0 = 未扰动波前，与旧
    // "密度 1 不加光" 的约定一致；安全阀 waterFftParams2.w 保证 binding 69
    // 不会被采样到未写入的描述符。
    //
    // 2026-11-20 泼溅重构：图的键从"源水面点"换成"接收面"。
    // 旧图逐纹素算 dq/dp 的密度，纹素存的是**源点** p 处的值，所以查
    // flatProjectedWaterXZ（源点）；新图是光子泼溅，光子按**落点**进桶，
    // 桶号就是接收面位置 ⇒ 必须查 causticReceiverPlaneXZ（一格雷诺面）。
    // 旧注释"用它会多带一个常量位移"对源键的图成立，对泼溅图恰好反了。
    //
    // 色散：单通道累加图（R32_UINT）没法在泼溅里拆三个 IOR —— 改成消费端
    // 三次微偏移采样，等同 HPWater 的非 RGB 路径（他们也是单通道 + 偏移）。
    vec3 naturalDensity3 = vec3(1.0);
    if(cam.waterFftParams.y > 0.0 && cam.waterFftParams2.w > 0.5)
    {
#if WATER_CAUSTIC_DRIFT_AND_DEPTH_SYNC
        const float causticTilePeriod = max(cam.waterFftParams.x, 1.0e-3);
        const float causticWaveTime =
            uintBitsToFloat(cam.padding) * clamp(cam.waterFftParams.w, 0.0, 4.0);
        // 2026-11-21 统一风场：与 water_wave_field.glsl 的 drift 逐位同源
        //（−windDir × 0.0629），否则焦散图会和水波错位。
        const vec2 causticDrift = -windDirXZ() * 0.0629 * causticWaveTime;
        const vec2 causticUv = causticReceiverPlaneXZ / causticTilePeriod + causticDrift;
        const float causticTexelUv = 1.0 / 1024.0;
        const float causticDispKnob = clamp(cam.waterParams.y, 0.0, 1.0);
        if(causticDispKnob > 0.001)
        {
            const float horizLen = length(receiverToSun.xz);
            const vec2 sunHorizDir = horizLen > 1.0e-3
                ? (receiverToSun.xz / horizLen)
                : vec2(0.70710678, 0.70710678);
            const float depthDispScale = clamp(waterDepth * 0.45, 0.15, 1.8);
            const vec2 dispersionUv =
                sunHorizDir * (0.65 * depthDispScale * causticTexelUv);
            naturalDensity3 = vec3(
                textureLod(waterCausticMap, causticUv + dispersionUv, causticMapLod).r,
                textureLod(waterCausticMap, causticUv, causticMapLod).r,
                textureLod(waterCausticMap, causticUv - dispersionUv, causticMapLod).r);
        }
        else
        {
            float monoDensity = textureLod(waterCausticMap, causticUv, causticMapLod).r;
            naturalDensity3 = vec3(monoDensity);
        }
#else
        const float causticTilePeriod = max(cam.waterFftParams.x, 1.0e-3);
        const vec2 causticUv = causticReceiverPlaneXZ / causticTilePeriod;
        // 一个纹素的 UV 步长（1024² 图，与宿主 WATER_CAUSTIC_MAP_RESOLUTION 同）。
        const float causticTexelUv = 1.0 / 1024.0;
        // 0.45 是旧图 params.w 的色散强度；0.55 纹素 ≈ 0.07 格，只在焦散线
        // 的陡边上出彩边，不会把线条本身拆开。
        const vec2 dispersionUv =
            vec2(0.55, 0.0) * (0.45 * causticTexelUv);
        naturalDensity3 = vec3(
            textureLod(waterCausticMap, causticUv, causticMapLod).r,
            textureLod(
                waterCausticMap, causticUv + dispersionUv, causticMapLod).r,
            textureLod(
                waterCausticMap, causticUv - dispersionUv, causticMapLod).r);
#endif
    }

#if WATER_CAUSTIC_FAST_INTERACTION_LENS
    if(cam.waterSimState.y != 0u && cam.waterSimSurfaceAndOrigin.w > 0.0)
    {
        float interactionElevationMatch = 1.0 - smoothstep(
            0.65,
            1.50,
            abs(waterSurfacePosition.y - cam.waterSimSurfaceAndOrigin.x)
        );
        if(interactionElevationMatch > 0.0)
        {
            float worldTexel = cam.waterSimSurfaceAndOrigin.w;
            vec2 simUv =
                (flatProjectedWaterXZ - cam.waterSimSurfaceAndOrigin.yz) /
                (worldTexel * 512.0);
            if(all(greaterThan(simUv, vec2(0.0))) && all(lessThan(simUv, vec2(1.0))))
            {
                vec2 simBorder = smoothstep(vec2(0.0), vec2(0.12), simUv) *
                    (1.0 - smoothstep(vec2(0.88), vec2(1.0), simUv));
                float simBorderMask = simBorder.x * simBorder.y;
                if(simBorderMask > 0.0)
                {
                    float stateLayer = float(cam.waterSimState.x);
                    vec2 uvStep = vec2(1.0 / 512.0);
                    float hC = texture(waterSimulationState, vec3(simUv, stateLayer)).r;
                    float hL = texture(waterSimulationState, vec3(simUv - vec2(uvStep.x, 0.0), stateLayer)).r;
                    float hR = texture(waterSimulationState, vec3(simUv + vec2(uvStep.x, 0.0), stateLayer)).r;
                    float hD = texture(waterSimulationState, vec3(simUv - vec2(0.0, uvStep.y), stateLayer)).r;
                    float hU = texture(waterSimulationState, vec3(simUv + vec2(0.0, uvStep.y), stateLayer)).r;
                    float laplacian =
                        (hL + hR + hD + hU - 4.0 * hC) / max(worldTexel * worldTexel, 1.0e-4);
                    float calibration = clamp(cam.waterWaveParams.x, 0.0, 1.0);
                    float simStrength = clamp(cam.waterWaveParams.y, 0.0, 2.0);
                    float interactionScale = mix(0.19, 0.5, calibration) * simStrength;
                    float effectiveDepth =
                        clamp(waterDepth, 0.25, 2.5) / max(receiverToSun.y, 0.35);
                    // Convex crest (laplacian < 0) converges refracted rays -> positive excess.
                    float convergence =
                        -laplacian * (0.248 * 2.0 * interactionScale) * effectiveDepth * 0.35;
                    float rippleExcess = convergence > 0.0
                        ? 2.5 * (1.0 - exp(-convergence / 2.5))
                        : max(convergence, -0.4);
                    naturalDensity3 +=
                        vec3(rippleExcess * simBorderMask * interactionElevationMatch);
                }
            }
        }
    }
#else
    float interactionElevationMatch = 1.0 - smoothstep(
        0.65,
        1.50,
        abs(waterSurfacePosition.y - cam.waterSimSurfaceAndOrigin.x)
    );
    interactionElevationMatch *= float(cam.waterSimState.y != 0u);
    if(interactionElevationMatch > 0.0)
    {
        vec2 naturalSourceXZ = invertCausticWavefrontProjection(
            causticReceiverPlaneXZ,
            sunDirection,
            false);
        float interactionDensity = evaluateCausticWavefrontDensity(
            naturalSourceXZ,
            sunDirection,
            true);
        naturalDensity3 += vec3(
            (interactionDensity - 1.0) *
            WATER_CAUSTIC_RIPPLE_LENS * interactionElevationMatch);
    }
#endif
#if 0
    // The ripple used to own a second wavefront inversion.  Its ~0.4-block
    // wavelength carries roughly a hundred times the natural band curvature,
    // which drives this damped fixed-point map past its 0 < F' < 2.35
    // convergence bound (the natural bands sit at F' ~ 1.27).  The recovered
    // source then scatters across the window and the density degenerates into
    // incoherent speckle instead of a coherent ripple projection.  Shielded as
    // the interface for a future multi-step or Hessian-corrected inversion.
    vec2 interactionSourceXZ = invertCausticWavefrontProjection(
        causticReceiverPlaneXZ,
        sunDirection,
        true
    );
#endif

    // 2026-11-20：太阳掠射角钳制的主体 —— 把发散掉的密度插值回 1.0（未扰动
    // 波前 = 不加光），图案随之溶解，水底整体亮度不变。放在色散/深度弥散之前，
    // 这三者都是对"图案"的后处理，钳制应当先于它们生效。
#if WATER_CAUSTIC_GRAZING_CLAMP
    naturalDensity3 = mix(naturalDensity3, vec3(1.0), grazingBlend);
#endif

    // ── 批次3-P3（waterParams.z，0=关）：焦散水深弥散 ──
    // HPWater"散射密度调制 mip 模糊"的物理版：深水处高频波前自然弥散，
    // 密度向 1（未扰动）回退；浅水保持锐利。0.45/m 为弥散速率。
    float depthDiffusion = clamp(cam.waterParams.z, 0.0, 1.0);
    if(depthDiffusion > 0.001)
    {
#if WATER_CAUSTIC_DEPTH_FOCUS
        naturalDensity3 = mix(naturalDensity3, vec3(1.0),
            depthDiffusion * (1.0 - exp(-waterDepth * 0.26)));
#else
        naturalDensity3 = mix(naturalDensity3, vec3(1.0),
            depthDiffusion * (1.0 - exp(-waterDepth * 0.45)));
#endif
    }

    float causticDispersion = clamp(cam.waterParams.y, 0.0, 1.0);
    if(causticDispersion < 0.999)
    {
        float densityLuminance = dot(
            naturalDensity3,
            vec3(0.2126, 0.7152, 0.0722));
        naturalDensity3 = mix(
            vec3(densityLuminance),
            naturalDensity3,
            causticDispersion);
    }

    vec3 causticFocus = max(naturalDensity3 - 1.0, vec3(0.0));
    causticFocus *= 0.88 + 0.24 * causticFocus;

#ifndef WATER_CAUSTIC_LINE_SHARPEN_HP
#define WATER_CAUSTIC_LINE_SHARPEN_HP 135
#endif
#if WATER_CAUSTIC_DEPTH_FOCUS
    float shallowRamp = smoothstep(0.06, 0.38, waterDepth);
    float focusSweetSpot =
        smoothstep(0.45, 1.4, waterDepth) * (1.0 - smoothstep(3.5, 8.0, waterDepth));
    float focusGamma = mix(1.12, 1.45, focusSweetSpot);
    causticFocus = pow(causticFocus, vec3(focusGamma)) * shallowRamp;
#elif WATER_CAUSTIC_LINE_SHARPEN_HP > 100
    causticFocus =
        pow(causticFocus, vec3(WATER_CAUSTIC_LINE_SHARPEN_HP / 100.0));
#endif
    if(max(causticFocus.x, max(causticFocus.y, causticFocus.z)) <= 1.0e-5)
    {
        return vec3(0.0);
    }

#ifndef WATER_CAUSTIC_REUSE_RECEIVER_SUN
#define WATER_CAUSTIC_REUSE_RECEIVER_SUN 1
#endif

#if WATER_CAUSTIC_DEFERRED_SUN_TRACE
    vec3 sunTransmittance;
#if WATER_CAUSTIC_REUSE_RECEIVER_SUN
    if(receiver.receiverSunTransmittanceValid > 0.5)
    {
        sunTransmittance = receiver.receiverSunTransmittance;
    }
    else
#endif
    {
        sunTransmittance = traceSunTransmittanceThroughWaterEx(
            receiver.receiverWorldPos + receiverN * 0.015,
            receiverToSun,
            L
        );
    }
    if(max(sunTransmittance.r, max(sunTransmittance.g, sunTransmittance.b)) <= 0.0)
    {
        return vec3(0.0);
    }
#endif

    int f0MetalCode = decodeF0MetalCode(receiver.receiverF0Metal);
    float metallic = decodeMetallicFromCode(f0MetalCode);
    float diffuseResponse = evaluateBurleyDiffuseAngular(
        receiverN,
        receiverV,
        receiverToSun,
        clamp(receiver.receiverRoughness, 0.04, 1.0)
    );

#if !WATER_CAUSTIC_DEFERRED_SUN_TRACE
    // 2026-11-16：相机侧水柱透射优先读 walker 逐段累乘的实值（含 GLASS_TINT
    // 混色与深水 0.15 压透）——多段水程（水+玻璃+水）时旧的单发公式与实际
    // 累乘不等，复用后以实值为准。负值哨兵 = 该 walker 未提供（提前耗尽回退
    // 等路径）⇒ 回退旧单发公式（逐位旧档）。回退 = 恢复为直接算这段公式。
    vec3 cameraWaterTransmittance = receiver.cameraWaterColumnTransmittance;
    if(any(lessThan(cameraWaterTransmittance, vec3(0.0))))
    {
        float cameraWaterFade = clamp(receiver.waterPathLength / 16.0, 0.0, 1.0);
        cameraWaterFade *= cameraWaterFade;
        cameraWaterTransmittance =
            exp(-WATER_ABSORPTION * receiver.waterPathLength);
        cameraWaterTransmittance = mix(
            cameraWaterTransmittance,
            cameraWaterTransmittance * WATER_GLASS_TINT,
            cameraWaterFade
        );
        cameraWaterTransmittance *= mix(1.0, 0.15, cameraWaterFade);
    }
#endif

    const bool sunVisNonZero =
        max(sunTransmittance.r, max(sunTransmittance.g, sunTransmittance.b)) > 1.0e-5;
#if WATER_CAUSTIC_DEFERRED_SUN_TRACE
    float causticCloudShadow = sunVisNonZero
        ? (g_waterEntrySunValid
            ? g_waterEntryCloudShadow
            : c3CloudSunTransmittance(waterSurfacePosition, L))
        : 0.0;
#else
    float causticCloudShadow = sunVisNonZero
        ? c3CloudSunTransmittance(waterSurfacePosition, receiverToSun)
        : 0.0;
#endif
    vec3 sunRadiance =
        getTimeDrivenSkyLightColor() *
        3.2 *
        getTimeDrivenSkyLightRadianceScale() *
        sunTransmittance *
        causticCloudShadow;

    float shallowFade = smoothstep(0.08, 0.55, waterDepth);
    return
        sunRadiance *
        cameraWaterTransmittance *
        receiver.receiverAlbedo *
        (1.0 - metallic) *
        diffuseResponse *
        (WATER_CAUSTIC_STRENGTH * causticFocus * shallowFade);
}

vec3 tracePrimaryTransparentRadiance(
    vec3 startOrigin,
    vec3 startDirection,
    bool startInsideWater,
    vec3 sunDirection,
    bool enableWaterReceiverGI,
    float waterGILodDistance,
    // History key: the camera-visible FIRST transparent surface's raw hit (see
    // TRANSPARENT_HISTORY_KEY_FIRST_SURFACE).  For a directly-hit water surface
    // the caller passes that hit point, which is what startOrigin already is.
    vec3 historyKeyPosition,
    // 2026-10-22: the caller measured this water surface point as fully sun
    // occluded, so the reflected sky must not re-add the sun's disk/aureole
    // through a gap above the blocker (bit 14/15, consumed by rt_test.rmiss).
    bool suppressSkySun,
    // 2026-11-16：反射瓣标记（与 rgen_water_receiver.glsl 的定义同步）。
    bool reflectionLobe,
    out WaterTransmissionInfo transmissionInfo
);