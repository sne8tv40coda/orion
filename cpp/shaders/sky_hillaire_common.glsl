#ifndef SKY_HILLAIRE_COMMON_GLSL
#define SKY_HILLAIRE_COMMON_GLSL

// =============================================================================
// sky_hillaire_common.glsl —— A 方案（Hillaire 结构）的天空散射，运行时可切。
//
// 参考实现（照抄，MIT）：refs/lib_godot_clayjohn/cloud_sky/sky-lut.glsl
//   作者 Fernando García Liñán（原帖 shadertoy.com/view/msXXDS），Godot 侧移植。
//   结构 = Hillaire《A Scalable and Production Ready Sky and Atmosphere Rendering
//   Technique》(EGSR 2020) 的"运行时逐像素单次散射 + 解析多次散射近似"：
//     · 逆散射用 Frostbite 的能量守恒解析积分
//         S_int = (S - S * T_step) / σ_ext        （参考实现同一条式子）
//     · 多次散射不用 MS LUT，用"地面二次散射 + 经验拟合"两项近似
//
// 与本工程的**三处刻意的偏差**（不是"自创"，是必要的适配，逐条说明）：
//   1) 介质常数改用本工程 Bruneton 那套（cpp/shaders/bruneton/atmosphere.glsl:19-40）：
//      β_R=(0.006962,0.016270,0.044685)、标高 8 km；β_M=0.003996 / 消光 0.00444、
//      标高 1.2 km、g=0.8；臭氧 25 km 三角层 β_O=(0.00065,0.001881,0.000085)；
//      地面反照率 0.1；太阳辐照度 (1.474,1.8504,1.91198)。
//      理由：下面第 2 条要复用同一张 transmittance LUT，介质必须同源；否则两个
//      模型连"同一时刻同一太阳高度"都不一致，A/B 就没有意义。
//   2) 透过率**不另建 Hillaire 的 transmittance LUT**，直接用现成的 Bruneton
//      256×64 LUT（bruneton/runtime.glsl 的 GetTransmittance /
//      GetTransmittanceToTopAtmosphereBoundary）。省一套资源 + 一次 startup 派发，
//      物理量相同。参考实现里的 transmittance_from_lut(lut, cos, h01) 是它自己
//      的线性映射，本工程是 (r, mu) 非线性映射 ⇒ 用现成函数取同样的量。
//   3) 光谱 4 波段 → 线性 sRGB 的矩阵按参考实现保留（下方矩阵原样），
//      但本工程输出是 3 通道，故直接抄它 transpose 后的 4×3 形式。
//
// 与 Bruneton 分支的**唯一实质差别**：B 模型用 4 阶 delta 展开的多次散射 LUT，
// A 模型用逐像素 march（含真实行星阴影）+ 解析多次散射近似。输出契约完全相同
// （同一张 256×128 equirect + 同一张 transmittance），所以 rt_test.rmiss 与所有
// 下游消费者零改动。
// =============================================================================

// 参考实现：IN_SCATTERING_STEPS = 30。我们的 LUT 是 256×128 = 32k texel，
// 30 步 × (1 次 LUT 取样 + 多次散射解析式) 的成本可以忽略；取 32 便于对齐。
#ifndef SKY_HILLAIRE_STEPS
#define SKY_HILLAIRE_STEPS 32
#endif

const float SKY_HIL_PI = 3.14159265358979323846;
const float SKY_HIL_INV_PI = 0.31830988618379067154;
const float SKY_HIL_INV_4PI = 0.25 * 0.31830988618379067154;
const float SKY_HIL_PHASE_ISOTROPIC = SKY_HIL_INV_4PI;
const float SKY_HIL_RAYLEIGH_PHASE_SCALE = (3.0 / 16.0) * SKY_HIL_INV_PI;
const float SKY_HIL_G = 0.8;
const float SKY_HIL_GG = SKY_HIL_G * SKY_HIL_G;

// 介质（与 bruneton/atmosphere.glsl 的 ATMOSPHERE 同源，见文件头偏差 1）
const vec3 SKY_HIL_SOLAR_IRRADIANCE = vec3(1.474, 1.8504, 1.91198);
const vec3 SKY_HIL_RAYLEIGH_SCATTERING = vec3(0.006962, 0.016270, 0.044685);
const float SKY_HIL_RAYLEIGH_SCALE_HEIGHT = 8.0;
const vec3 SKY_HIL_MIE_SCATTERING = vec3(0.003996);
const vec3 SKY_HIL_MIE_EXTINCTION = vec3(0.00444);
const float SKY_HIL_MIE_SCALE_HEIGHT = 1.2;
const vec3 SKY_HIL_OZONE_ABSORPTION = vec3(0.00065, 0.001881, 0.000085);
const float SKY_HIL_GROUND_ALBEDO = 0.1;

// 参考实现：linear_srgb_from_spectral_samples() 的 4×3 矩阵（原样搬，MIT）。
const mat4x3 SKY_HIL_SPECTRAL_TO_RGB = mat4x3(
    137.672389239975, -8.632904716299537, -1.7181567391931372,
    32.549094028629234, 91.29801417199785, -12.005406444382531,
    -38.91428392614275, 34.31665471469816, 29.89044807197628,
    8.572844237945445, -11.103384660054624, 117.47585277566478);

// ---------------------------------------------------------------------------
// 参考实现：ray_sphere_intersection()（逐行照抄，含"射线背离球体"的早退判据）
// ---------------------------------------------------------------------------
float skyHillaireRaySphere(vec3 origin, vec3 direction, float radius)
{
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;

    if(c > 0.0 && b > 0.0)
    {
        return -1.0;
    }

    float d = b * b - c;

    if(d < 0.0)
    {
        return -1.0;
    }

    if(d > b * b)
    {
        return -b + sqrt(d);
    }

    return -b - sqrt(d);
}

// 参考实现：molecular_phase_function() / aerosol_phase_function()
float skyHillaireRayleighPhase(float cosine)
{
    return SKY_HIL_RAYLEIGH_PHASE_SCALE * (1.0 + cosine * cosine);
}

float skyHillaireMiePhase(float cosine)
{
    float denominator = 1.0 + SKY_HIL_GG + 2.0 * SKY_HIL_G * cosine;
    return SKY_HIL_INV_4PI * (1.0 - SKY_HIL_GG) /
        (denominator * sqrt(denominator));
}

// 参考实现：get_atmosphere_collision_coefficients() 的密度剖面部分，
// 换成 ATMOSPHERE 里的三组剖面（Rayleigh exp(-h/8)、Mie exp(-h/1.2)、
// 臭氧三角层 25 km ± 15 km）。
void skyHillaireCoefficients(
    float altitudeKm,
    out vec3 scattering,
    out vec3 extinction)
{
    float h = max(altitudeKm, 0.0);

    float rayleighDensity = exp(-h / SKY_HIL_RAYLEIGH_SCALE_HEIGHT);
    float mieDensity = exp(-h / SKY_HIL_MIE_SCALE_HEIGHT);
    // 25 km 三角层（bruneton/atmosphere.glsl:34-36 的两层合起来的等效形状）
    float ozoneDensity = clamp(1.0 - abs(h - 25.0) / 15.0, 0.0, 1.0);

    vec3 rayleighScattering = SKY_HIL_RAYLEIGH_SCATTERING * rayleighDensity;
    vec3 mieScattering = SKY_HIL_MIE_SCATTERING * mieDensity;
    vec3 mieExtinction = SKY_HIL_MIE_EXTINCTION * mieDensity;
    vec3 ozoneAbsorption = SKY_HIL_OZONE_ABSORPTION * ozoneDensity;

    scattering = rayleighScattering + mieScattering;
    extinction = rayleighScattering + mieExtinction + ozoneAbsorption;
}

// ---------------------------------------------------------------------------
// 参考实现：get_multiple_scattering()（原式照抄）
//   与参考实现的差别只在"透过率从哪来"：它读自己的 LUT，我们读 Bruneton 的
//   （文件头偏差 2）。三个量一一对应：
//     T_to_ground       = 地面沿太阳方向到大气顶
//     T_ground_to_sample= 天顶方向 地面→采样点（用两个"到大气顶"的比值）
//   omega = 地面在采样点处张开的立体角。
// ---------------------------------------------------------------------------
vec3 skyHillaireMultipleScattering(
    float cosineSunZenith,
    float radiusKm,
    float altitudeKm)
{
    float planetRadius = ATMOSPHERE.bottom_radius;
    float topRadius = ATMOSPHERE.top_radius;

    float omega = 2.0 * SKY_HIL_PI *
        (1.0 - sqrt(max(radiusKm * radiusKm - planetRadius * planetRadius, 0.0))
            / max(radiusKm, 1.0e-4));

    DimensionlessSpectrum toGround = GetTransmittanceToTopAtmosphereBoundary(
        ATMOSPHERE,
        brunetonTransmittance,
        planetRadius,
        max(cosineSunZenith, 0.0));

    DimensionlessSpectrum groundToTop = GetTransmittanceToTopAtmosphereBoundary(
        ATMOSPHERE,
        brunetonTransmittance,
        planetRadius,
        1.0);

    DimensionlessSpectrum sampleToTop = GetTransmittance(
        ATMOSPHERE,
        brunetonTransmittance,
        radiusKm,
        1.0,
        max(topRadius - radiusKm, 0.0),
        false);

    vec3 groundToSample = groundToTop /
        max(sampleToTop, DimensionlessSpectrum(1.0e-4));

    // 2nd order scattering from the ground（参考实现原式）
    vec3 ground = SKY_HIL_PHASE_ISOTROPIC * omega *
        (SKY_HIL_GROUND_ALBEDO / SKY_HIL_PI) *
        toGround * groundToSample *
        max(cosineSunZenith, 0.0);

    // 参考实现里这句是 Earth 参数下的经验拟合（note 原文："Fit of Earth's
    // multiple scattering coming from other points in the atmosphere"）。
    // 我们换了介质常数，所以它是**近似值**，但仍按下式保留（见文件头说明）。
    vec3 ms = 0.02 * vec3(0.217, 0.347, 0.594) *
        (1.0 / (1.0 + 5.0 * exp(-17.92 * cosineSunZenith)));

    return ms + ground;
}

// ---------------------------------------------------------------------------
// 主入口：沿相机射线单次散射 march（含真实行星阴影），同时带出 Mie 分量
// （aureole，水面反射与 skyAureoleRatio 要用）。
// 输出 spectral / spectralAureole 与 Bruneton 分支的 `spectral` 同量纲
// （太阳辐照度 × 散射系数 × 相位，未乘 SKY_SPECTRAL_RADIANCE_TO_LUMINANCE）。
// ---------------------------------------------------------------------------
void skyHillaireEvaluate(
    float cameraAltitudeKm,
    vec3 rayDirection,
    vec3 sunDirection,
    out vec3 spectral,
    out vec3 spectralAureole)
{
    float planetRadius = ATMOSPHERE.bottom_radius;
    float topRadius = ATMOSPHERE.top_radius;

    vec3 origin = vec3(0.0, planetRadius + cameraAltitudeKm, 0.0);

    float atmosphereDistance = skyHillaireRaySphere(origin, rayDirection, topRadius);
    float groundDistance = skyHillaireRaySphere(origin, rayDirection, planetRadius);

    float marchLength = atmosphereDistance;

    if(groundDistance > 0.0 && (marchLength < 0.0 || groundDistance < marchLength))
    {
        marchLength = groundDistance;
    }

    spectral = vec3(0.0);
    spectralAureole = vec3(0.0);

    if(marchLength <= 0.0)
    {
        return;
    }

    float stepLength = marchLength / float(SKY_HILLAIRE_STEPS);

    float cosinePhase = dot(-rayDirection, sunDirection);
    float rayleighPhase = skyHillaireRayleighPhase(cosinePhase);
    float miePhase = skyHillaireMiePhase(cosinePhase);

    vec3 throughput = vec3(1.0);

    for(int i = 0; i < SKY_HILLAIRE_STEPS; ++i)
    {
        float t = (float(i) + 0.5) * stepLength;
        vec3 position = origin + rayDirection * t;

        float radius = length(position);
        float altitude = radius - planetRadius;
        float cosineSunZenith = clamp(dot(position / radius, sunDirection), -1.0, 1.0);

        vec3 scattering;
        vec3 extinction;
        skyHillaireCoefficients(altitude, scattering, extinction);

        // 太阳方向透过率：太阳在地平线下时 GetTransmittanceToSun 返回 0，
        // 与 Bruneton 分支的夜间行为一致（参考实现同样靠它出夜色）。
        DimensionlessSpectrum transmittanceToSun = GetTransmittanceToSun(
            ATMOSPHERE, brunetonTransmittance, radius, cosineSunZenith);

        vec3 multipleScattering = skyHillaireMultipleScattering(
            cosineSunZenith, radius, altitude);

        // 参考实现：S = sun * (β_R*(phase_R*T + ms) + β_M*(phase_M*T + ms))
        vec3 rayleighSource = SKY_HIL_RAYLEIGH_SCATTERING *
            (rayleighPhase * transmittanceToSun + multipleScattering);
        vec3 mieSource = SKY_HIL_MIE_SCATTERING *
            (miePhase * transmittanceToSun + multipleScattering);

        vec3 source = SKY_HIL_SOLAR_IRRADIANCE * (rayleighSource + mieSource);

        vec3 stepTransmittance = exp(-stepLength * extinction);

        // 能量守恒解析积分（参考实现同式）
        vec3 integratedSource = (source - source * stepTransmittance) /
            max(extinction, vec3(1.0e-7));

        spectral += throughput * integratedSource;

        // aureole = 只留 Mie 单次散射那一份（供 skyAureoleRatio）
        vec3 aureoleSource = SKY_HIL_SOLAR_IRRADIANCE *
            SKY_HIL_MIE_SCATTERING * (miePhase * transmittanceToSun);
        spectralAureole += throughput *
            ((aureoleSource - aureoleSource * stepTransmittance) /
                max(extinction, vec3(1.0e-7)));

        throughput *= stepTransmittance;
    }
}

#endif // SKY_HILLAIRE_COMMON_GLSL
