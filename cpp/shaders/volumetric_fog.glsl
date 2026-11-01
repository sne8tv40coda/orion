#ifndef VOLUMETRIC_FOG_GLSL
#define VOLUMETRIC_FOG_GLSL

#include "fog_density_common.glsl"

#include "local_light_grid_common.glsl"

const float VFOG_QUALITY = 6.0;

const float FOG_AMBIENT_SHADOW_FLOOR = 0.35;

const int VFOG_SUNLIGHT_STEPS_MAX = 8;

struct FogResult
{
    vec3 scattering;
    float transmittance;
    float firstDistance;
    float opticalDepth;
    float meanDistance;
};

// 参考 VolumetricFog.glsl:75-79 的抖动。
// 参考在 TAA 分支用 BlueNoiseTemporal().y，非 TAA 分支用 bayer64()。本工程
// 的 4 个 clouds 变体并不都带 temporalJitter()，所以这里用参考**非 TAA 分支**
// 的固定 4x4 Bayer（bayer64 的左上 4x4 子块）：同一个像素永远拿同一个相位，
// 不会引入时间性闪烁；雾本身没有时域累积，逐帧抖动只会变成噪点。
// 2026-10-23：逐帧低差异相位（自创）。与 volumetric_clouds.comp 的
// temporalPhase() 同一招：黄金比序列在任意连续窗口内近似均匀铺满 [0,1)，
// 于是"沿视线的步进相位"在 3~5 帧内被既有 resolve 收敛掉，而不是靠白噪声。
// 只取低 10 位（同 v1 的理由：fp32 精度，几小时后仍精确）。
//
float fogTemporalPhase(float strength)
{
    if(strength <= 0.0)
    {
        return 0.0;
    }
    return strength * float(cam.frameCount & 0x3ffu) * 0.6180339887498949;
}

// 2026-10-31：4x4 Bayer 换成 Interleaved Gradient Noise（自创，标准做法）。
// 病征是"整片雾面上细密均匀、相机静止时钉死在屏幕上的纱窗"：Bayer 是
// **空间固定**的低分辨率图案（4 个云像素 = 8 个输出像素的周期），而雾是
// 低频均匀场，一旦去噪/时间累积没把它抹平就必然被看成网格。IGN 每个像素
// 的相位都不同、无重复周期，时间相位叠加后只会收敛成均匀值，不会再被识别
// 成图案；同时比 const int[16] 数组的动态索引便宜（部分驱动会把常量数组
// 降到 local memory 再去索引）。
float fogDitherPhase(ivec2 pixel, float temporalPhase)
{
    const float ign = fract(52.9829189 * fract(
        dot(vec2(pixel) + 0.5, vec2(0.06711056, 0.00583715))));
    return fract(ign + temporalPhase);
}

const float FOG_ANCHOR_REFERENCE_Y = 63.0;
const float FOG_ANCHOR_MAX_SHIFT = 128.0;

// 一次高度场取样，同时服务两件事：密度带的 Y 平移、雾浪的贴地剖面。
struct FogGround
{
    float height;
    float weight;
};
FogGround fogGroundAt(vec3 position)
{
    FogGround g;
    g.height = FOG_ANCHOR_REFERENCE_Y;
    g.weight = 0.0;

    float outsideDistance = 0.0;
    const float ground = terrainGroundHeight(position.xz, outsideDistance);
    if(ground < TERRAIN_HEIGHT_EMPTY * 0.5)
    {
        return g;
    }

    g.height = ground;
    g.weight = 1.0 - smoothstep(
        0.0, TERRAIN_HEIGHT_OUTSIDE_FADE_BLOCKS, outsideDistance);
    return g;
}

float fogGroundAnchorOffset(FogGround g, float strength)
{
    const float shift = clamp(
        g.height - FOG_ANCHOR_REFERENCE_Y,
        -FOG_ANCHOR_MAX_SHIFT,
        FOG_ANCHOR_MAX_SHIFT);
    return shift * clamp(strength * g.weight, 0.0, 1.0);
}

// 2026-10-31：雨天地面雾浪（cloudParams4.y/.z/.w + cloudParams9.w 那条链）
// 整段删除。用户实测"跟没有一样"，留着只是每步多 2 次 textureLod 与一次
// 3D 噪声形状计算。相关 UBO 车道保留（std140 布局不动），但已无消费者。
const float FOG_LIGHT_PHASE_SCALE = 0.62;
const float FOG_LIGHT_PHASE_FLOOR = 0.02;
const float FOG_LIGHT_PHASE_G = 0.40;

float fogLightPhase(vec3 toLight, vec3 viewDirection)
{
    const float distanceToLight = max(length(toLight), 1.0e-4);
    const float cosine = dot(-toLight / distanceToLight, viewDirection);
    return FOG_LIGHT_PHASE_FLOOR + FOG_LIGHT_PHASE_SCALE * hgPhase(cosine, FOG_LIGHT_PHASE_G);
}

// 主入口。前 17 个参数与 2026-10-19 B 方案一致；末尾三个是 2026-10-23 追加的
// 自创项（地面锚定 / 时域抖动 / 动态光源），全部"0 = 逐位旧档"。
// ===========================================================================
// 2026-10-23 修正：环境项**不能**取"视线方向的晴空辐亮度"
//
// 症状（用户报告）："有时候可以隔墙看到太阳所在位置的白光"。
// 根因：clearSkyInscatter() 里含 Mie 前向瓣，正对太阳时比天顶亮一到两个数量级。
// 我先前直接把它当环境入散射用，于是：
//   1) 雾变成有方向的 —— 朝太阳看时整层雾发白，白斑钉在太阳的屏幕位置上；
//   2) 环境项只吃 sunShadow 的"环境压暗地板"（FOG_AMBIENT_SHADOW_FLOOR = 0.35），
//      所以躲在墙后 / 洞里照样按 35% 发光 ⇒ "隔墙可见"。
// 物理上环境项是**半球积分**，不能用单方向辐亮度。这里改成对三个固定方向
// （天顶 / 太阳侧水平 0.2 仰角 / 反太阳侧水平 0.2 仰角）取 HG 加权平均 ——
// 与 v2/v4 的天光（D6）同一套口径，**不包含指向太阳的那一瓣**，
// 因此不会在太阳位置形成亮斑；同时保留"日出日落雾自己泛色"的效果。
// ===========================================================================
vec3 fogSkyAmbient(
    vec3 viewDirection,
    float sunElevation,
    vec3 lightDirection,
    vec3 directLightColor,
    float radianceScale,
    float weatherFactor)
{
    const vec3 sunSideDir = normalize(vec3(lightDirection.x, 0.20, lightDirection.z));
    const vec3 antiSunDir = normalize(vec3(-lightDirection.x, 0.20, -lightDirection.z));
    const vec3 radianceZenith = clearSkyInscatter(
        vec3(0.0, 1.0, 0.0), sunElevation, lightDirection,
        directLightColor, radianceScale, weatherFactor, vec3(1.0));
    const vec3 radianceSunSide = clearSkyInscatter(
        sunSideDir, sunElevation, lightDirection,
        directLightColor, radianceScale, weatherFactor, vec3(1.0));
    const vec3 radianceAntiSun = clearSkyInscatter(
        antiSunDir, sunElevation, lightDirection,
        directLightColor, radianceScale, weatherFactor, vec3(1.0));
    const float weightZenith = hgPhase(dot(viewDirection, vec3(0.0, 1.0, 0.0)), 0.25);
    const float weightSunSide = hgPhase(dot(viewDirection, sunSideDir), 0.25);
    const float weightAntiSun = hgPhase(dot(viewDirection, antiSunDir), 0.25);
    return (radianceZenith * weightZenith +
        radianceSunSide * weightSunSide +
        radianceAntiSun * weightAntiSun) /
        max(weightZenith + weightSunSide + weightAntiSun, 1.0e-4);
}
// 函数名 evaluateAnalyticFog 是旧名，当前内部已是参考的 march，不是解析闭式雾。
// fogDensityAt() 来自共享头 fog_density_common.glsl。
FogResult evaluateAnalyticFog(
    vec3 worldOrigin,
    vec3 worldDirection,
    float sceneDistance,
    vec3 sunRadiance,
    vec3 sunDirection,
    vec3 ambientRadiance,
    float rain,
    float enabled,
    float baseDensityParam,
    float heightA,
    float heightB,
    float anisotropyG,
    float sunStrength,
    float ambientStrength,
    float maxDistance,
    float startDistance,
    float sunStepsRaw,
    float rainBoost,
    float groundAnchor,
    float temporalJitter,
    float dynamicLightStrength)
{
    FogResult result;
    result.scattering = vec3(0.0);
    result.transmittance = 1.0;
    result.firstDistance = 0.0;
    result.opticalDepth = 0.0;
    result.meanDistance = 0.0;

    if(enabled < 0.5)
    {
        return result;
    }

    const float wetness = clamp(rain, 0.0, 1.0);

    const float baseDensity =
        max(baseDensityParam, 0.0) *
        (wetness * 0.15 + 0.08) /
        max(cam.cameraParams.y, 100.0);

    const float fogTimeFactor =
        mix(1.0, max(rainBoost, 0.0), wetness);

    if(fogTimeFactor <= 0.01)
    {
        return result;
    }

    const float heightMax = max(heightA, heightB);
    const float heightMin = min(heightA, heightB);

    const float fogFar = min(max(sceneDistance, 0.0), max(maxDistance, 1.0));
    const float fogNear = clamp(startDistance, 0.0, fogFar);

    result.firstDistance = fogNear;

    if(fogFar - fogNear <= 0.01)
    {
        return result;
    }

    const vec3 start = worldOrigin + worldDirection * fogNear;
    const vec3 end = worldOrigin + worldDirection * fogFar;
    const vec3 rayVector = end - start;

    const float rSteps = 1.0 / VFOG_QUALITY;
    // 提到循环外：原来循环里对同一条 rayVector 反复求 length（雾浪与均值距离各一次）。
    const float rayVectorLength = length(rayVector);
    const float rayLength = rayVectorLength * rSteps * 0.23979;

    const float noise = fogDitherPhase(
        ivec2(gl_GlobalInvocationID.xy), fogTemporalPhase(temporalJitter));

    const float VdotL = dot(worldDirection, sunDirection);
    const float phases = hgPhase(VdotL, anisotropyG) * 0.7 + 0.035;

    const int sunSteps = clamp(int(sunStepsRaw + 0.5), 1, VFOG_SUNLIGHT_STEPS_MAX);

    vec4 fogLightPosEmission[FOG_LIGHT_MAX];
    uint fogLightType[FOG_LIGHT_MAX];
    int fogLightCount = 0;
    float fogLightEnergyScale = 0.0;
    if(dynamicLightStrength > 0.0)
    {
        const vec3 gatherPoint = worldOrigin + worldDirection * min(fogFar, 24.0);
        fogLightCount = fogGatherLocalLights(
            gatherPoint, fogLightPosEmission, fogLightType);
        fogLightEnergyScale = 3.2 * LOCAL_LIGHT_ENERGY_SCALE *
            localLightIntensity() * max(dynamicLightStrength, 0.0);
    }
    vec3 lightDensityAccum = vec3(0.0);

    const bool needGroundSample = groundAnchor > 0.0;

    float transmittance = 1.0;
    float fogDensityAccum = 0.0;
    float rayDensityAccum = 0.0;
    float fogMeanDistanceAccum = 0.0;
    float fogMeanWeightAccum = 0.0;
    float fogLitAccum = 0.0;

    for(int i = 0; i < int(VFOG_QUALITY); ++i)
    {
        const float exponential = pow(11.0, (float(i) + noise) * rSteps);
        const float stepLength = exponential * 0.1 - 0.1;
        const float stepDensity = exponential * rayLength * 1.442695;

        const vec3 fogPos = start + stepLength * rayVector;

        FogGround ground;
        ground.height = FOG_ANCHOR_REFERENCE_Y;
        ground.weight = 0.0;
        if(needGroundSample)
        {
            ground = fogGroundAt(fogPos);
        }
        const float groundOffset = fogGroundAnchorOffset(ground, groundAnchor);

        float density = fogDensityAt(
            vec3(fogPos.x, fogPos.y - groundOffset, fogPos.z),
            baseDensity,
            fogTimeFactor,
            wetness,
            heightMax,
            heightMin);

        const float absorption = exp2(-density * stepDensity);

        float lightingDensity = 0.0;
        float lightingStepLength = VFOG_SUNLIGHT_STEPLENGTH;
        vec3 lightPos = fogPos;

        for(int j = 0; j < VFOG_SUNLIGHT_STEPS_MAX; ++j)
        {
            if(j >= sunSteps)
            {
                break;
            }

            lightingDensity += fogDensityAt(
                vec3(lightPos.x, lightPos.y - groundOffset, lightPos.z),
                baseDensity,
                fogTimeFactor,
                wetness,
                heightMax,
                heightMin) * lightingStepLength;

            lightPos += sunDirection * lightingStepLength;
            lightingStepLength *= 1.5;
        }

        float stepRayDensity =
            exp2(-lightingDensity * VFOG_SUNLIGHT_ABSORPTION) * phases;

        const float shadow = sunShadowFogVisibility(fogPos, sunDirection, i);

        const float integral = transmittance - absorption * transmittance;
        stepRayDensity *= integral;

        fogDensityAccum += integral;
        fogLitAccum += integral * shadow;
        rayDensityAccum += stepRayDensity * shadow;

        {
            const float sampleDistance = fogNear + stepLength * rayVectorLength;
            fogMeanDistanceAccum += integral * sampleDistance;
            fogMeanWeightAccum += integral;
        }

        for(int li = 0; li < FOG_LIGHT_MAX; ++li)
        {
            if(li >= fogLightCount)
            {
                break;
            }

            const vec3 toLight = fogLightPosEmission[li].xyz - fogPos;
            const float emissionLevel = clamp(fogLightPosEmission[li].w, 1.0, 15.0);
            // 2026-10-31：每盏灯有自己的档案（色温/亮度/距离），不再共用
            // "emission / 15 + emission × 2 格"的统一口径。
            const uint lightProfile = fogLightType[li];
            const float distanceToLight = length(toLight);
            if(distanceToLight > vptLocalLightRange(lightProfile, emissionLevel))
            {
                continue;
            }

            const float lightScale = fogLightEnergyScale *
                localLightProfileBrightness(lightProfile, emissionLevel) *
                vptLocalLightDistanceAttenuation(distanceToLight) *
                vptLocalLightRangeFade(distanceToLight, lightProfile, emissionLevel);

            lightDensityAccum += decodeLocalLightColor(fogLightType[li]) * lightScale *
                (fogLightPhase(toLight, worldDirection) * integral);
        }

        transmittance *= absorption;

        if(transmittance < 1.0e-4)
        {
            break;
        }
    }

    result.meanDistance = fogMeanWeightAccum > 1.0e-6
        ? fogMeanDistanceAccum / fogMeanWeightAccum
        : fogFar;

    const float litFraction = fogDensityAccum > 1.0e-6
        ? clamp(fogLitAccum / fogDensityAccum, 0.0, 1.0)
        : 1.0;
    const float ambientVisibility =
        mix(FOG_AMBIENT_SHADOW_FLOOR, 1.0, litFraction);

    const vec3 fogColor =
        ambientRadiance * fogDensityAccum *
        (0.3 * max(ambientStrength, 0.0)) * ambientVisibility;

    const vec3 rayColor =
        sunRadiance * rayDensityAccum *
        (1.2 * max(sunStrength, 0.0));

    result.scattering = fogColor + rayColor + lightDensityAccum;
    result.transmittance = transmittance;
    result.opticalDepth = -log(max(transmittance, 1.0e-6));

    return result;
}

#endif
