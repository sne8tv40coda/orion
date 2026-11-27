

// ===========================================================================
// 2026-11-20：**太阳可见性射线的起点偏移必须沿法线**（修"低太阳水床黑块"）
//
// 病灶：原来起点是 `hitPos + L * 0.001` —— 只沿**太阳方向**抬。垂直抬升量是
// `0.001 * sin(太阳仰角)`：仰角 30° 只有 5e-4 格，仰角 3° 只剩 5e-5 格。
// 也就是说太阳越低，射线越贴着面出发。掠射时射线与面几乎平行，浮点精度内
// 立刻命中自己的方块/水床表面 ⇒ 自遮挡 ⇒ sunTransmittance = 0 ⇒ 该点直射太阳
// 全灭，读作**成块的纯黑**。
//
// 三个观测特征全部吻合：
//   · 只在太阳低时出现（仰角高时射线本来就离开表面）；
//   · 边界沿方块/区块边缘（自遮挡发生在体素面之间）；
//   · 相机潜入水里后消失（水下不走这条 receiver 着色路径）。
//
// 修法：额外沿**表面法线**抬一个固定格数。垂直距离从此与太阳仰角无关，
// 掠射时也能稳定离开表面；0.02 格远小于方块尺度，不会产生可见的
// peter-panning（阴影外扩）。
// A/B：-DSUN_RAY_NORMAL_BIAS=0.0 回到只看太阳方向偏移的旧档。
// ===========================================================================
#ifndef SUN_RAY_NORMAL_BIAS
#define SUN_RAY_NORMAL_BIAS 0.02
#endif

// 2026-11-27 (Batch AE): Optional scalar capture of sunTransmittance for
// immediate reuse by underwater caustics (avoids duplicate water-sun trace).
bool g_captureDirectSunTrans = false;
bool g_capturedDirectSunValid = false;
vec3 g_capturedDirectSunTrans = vec3(0.0);

vec3 evaluateDirectSunLightingInMedium(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 viewDir,
    vec3 hitAlbedo,
    float hitRoughness,
    float hitF0Metal,
    float pomSelfShadow,
    vec3 sunDirection,
    bool startInWater
)
{
    vec3 N =
        normalize(
            hitNormal
        );

    vec3 V =
        normalize(
            viewDir
        );

    vec3 L =
        normalize(
            sunDirection
        );

    float NdotL =
        max(
            dot(N, L),
            0.0
        );

    float NdotV =
        max(
            dot(N, V),
            0.0
        );

    if(
        NdotL <= 0.0 ||
        NdotV <= 0.0
    )
    {
        return vec3(0.0);
    }

    const vec3 sunRayOrigin =
        hitPos +
        N * SUN_RAY_NORMAL_BIAS +
        L * 0.001;

    vec3 sunTransmittance = startInWater
        ? traceSunTransmittanceThroughWater(sunRayOrigin, L)
        : traceSunTransmittanceAir(sunRayOrigin, L);

    if(g_captureDirectSunTrans)
    {
        g_capturedDirectSunTrans = sunTransmittance;
        g_capturedDirectSunValid = true;
    }

    if(max(sunTransmittance.r, max(sunTransmittance.g, sunTransmittance.b)) <= 1.0e-5)
    {
        return vec3(0.0);
    }

    vec3 sunLightColor =
        getTimeDrivenSkyLightColor() *
        3.2 *
        getTimeDrivenSkyLightRadianceScale() *
        sunTransmittance *
        c3CloudSunTransmittance(sunRayOrigin, L) *
        clamp(
            pomSelfShadow,
            0.0,
            1.0
        );


    int f0MetalCode =
        decodeF0MetalCode(
            hitF0Metal
        );

    float metallic =
        decodeMetallicFromCode(
            f0MetalCode
        );

    vec3 F0 =
        decodeBaseF0(
            f0MetalCode,
            hitAlbedo
        );


    float roughness =
        clamp(
            hitRoughness,
            0.04,
            1.0
        );


    BsdfTerms bsdf =
        evalBSDFTerms(
            N,
            V,
            L,
            F0,
            metallic,
            roughness,
            NdotV,
            false
        );


    // PBR-1B:
    // Keep this helper path's existing BSDF_SUN_DIFFUSE_SCALE.
    // Only replace Lambert NdotL with Burley angular response.
    vec3 diffuseTerm =
        bsdf.kD *
        hitAlbedo *
        BSDF_SUN_DIFFUSE_SCALE *
        evaluateBurleyDiffuseAngular(
            N,
            V,
            L,
            roughness
        );


    vec3 specularTerm =
        bsdf.specular *
        NdotL;


    return
        sunLightColor *
        (
            diffuseTerm +
            specularTerm
        );
}

vec3 evaluateDirectSunLighting(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 viewDir,
    vec3 hitAlbedo,
    float hitRoughness,
    float hitF0Metal,
    float pomSelfShadow,
    vec3 sunDirection
)
{
    return evaluateDirectSunLightingInMedium(
        hitPos,
        hitNormal,
        viewDir,
        hitAlbedo,
        hitRoughness,
        hitF0Metal,
        pomSelfShadow,
        sunDirection,
        false
    );
}

#ifndef GI_BOUNCE_LAMBERT_DIRECT
#define GI_BOUNCE_LAMBERT_DIRECT 1
#endif

vec3 evaluateDirectSunLightingDiffuseInMedium(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 hitAlbedo,
    float hitF0Metal,
    float pomSelfShadow,
    vec3 sunDirection,
    bool startInWater
)
{
    vec3 N =
        normalize(
            hitNormal
        );

    vec3 L =
        normalize(
            sunDirection
        );

    float NdotL =
        max(
            dot(N, L),
            0.0
        );

    if(NdotL <= 0.0)
    {
        return vec3(0.0);
    }

    const vec3 sunRayOrigin =
        hitPos +
        N * SUN_RAY_NORMAL_BIAS +
        L * 0.001;

    vec3 sunTransmittance = startInWater
        ? traceSunTransmittanceThroughWater(sunRayOrigin, L)
        : traceSunTransmittanceAir(sunRayOrigin, L);

    if(max(sunTransmittance.r, max(sunTransmittance.g, sunTransmittance.b)) <= 1.0e-5)
    {
        return vec3(0.0);
    }

    vec3 sunLightColor =
        getTimeDrivenSkyLightColor() *
        3.2 *
        getTimeDrivenSkyLightRadianceScale() *
        sunTransmittance *
        c3CloudSunTransmittance(sunRayOrigin, L) *
        clamp(
            pomSelfShadow,
            0.0,
            1.0
        );

    int f0MetalCode =
        decodeF0MetalCode(
            hitF0Metal
        );

    float metallic =
        decodeMetallicFromCode(
            f0MetalCode
        );

    float diffuseScale =
        metallic < 0.5 ? 0.96 : 0.25;

    return sunLightColor * (hitAlbedo * diffuseScale * BSDF_SUN_DIFFUSE_SCALE * NdotL);
}

vec3 evaluateDirectSunLightingDiffuse(
    vec3 hitPos,
    vec3 hitNormal,
    vec3 hitAlbedo,
    float hitF0Metal,
    float pomSelfShadow,
    vec3 sunDirection
)
{
    return evaluateDirectSunLightingDiffuseInMedium(
        hitPos,
        hitNormal,
        hitAlbedo,
        hitF0Metal,
        pomSelfShadow,
        sunDirection,
        false
    );
}


// =====================================================
// RT Local Lights - L4
//
// Primary opaque surface only.
//
// Per pixel:
//   1) scan uploaded local lights,
//   2) retain the strongest 4 approximate contributors,
//   3) trace at most 4 visibility rays,
//   4) evaluate the same Cook-Torrance GGX model used by
//      current direct-sun lighting.
//
// L4 deliberately uses neutral white light. Per-block RGB/type
// mapping is deferred to the next Local Light section.
// =====================================================

const int MAX_PRIMARY_LOCAL_LIGHTS =
    4;


// =====================================================
// L7.3 B - Secondary diffuse Local Light GI budget
//
// Keep the strongest 2 local lights.
// Each selected light uses exactly 1 real 2.0x2.0 cube-area
// surface sample and at most 1 visibility ray.
// =====================================================
const int MAX_SECONDARY_LOCAL_LIGHTS =
    2;


// =============================================================================
// 2026-11-11 太阳可见度屏幕空间滤波（软影 A 方案，形态 1）的两个存储图。
// set 0 与 RT 管线共享（见 RtPipeline.cpp 的 b[90]/b[91]）：
//   90 = sunVisRaw     全分辨率 rgba16f，(可见度, 遮挡物距离, 线性 viewZ, —)
//   91 = sunVisHistory  半分辨率 rgba16f，两层 ping-pong
// 只在 rgen 侧使用；ORION_SUNVIS 关闭时这两个描述符没人访问。
// =============================================================================
layout(set = 0, binding = 90, rgba16f) uniform image2D sunVisRaw;
layout(set = 0, binding = 91, rgba16f) uniform image2D sunVisHistory[2];