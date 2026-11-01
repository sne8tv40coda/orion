#ifndef HIT_PAYLOAD_GLSL
#define HIT_PAYLOAD_GLSL

// =============================================================================
// 2026-10-31 payload 瘦身第二阶段（68 B -> 52 B）：
//   vec3 normal  -> uint normalPacked   （八面体 2 x 16 bit，省 8 B）
//   vec3 albedo  -> uint albedoPacked   （unorm8 x 4，省 8 B）
// 精度：法线用 16 bit/轴（亚度级），albedo 用 8 bit/通道（源贴图本来就是 8 bit，
// 且 AnyHit 的玻璃染色累积只会乘 <=1 的系数，不会越界）⇒ 无可见损失。
// emission 保持 vec3：它是 HDR，8 bit 编码会损失动态范围，收益不抵风险。
// =============================================================================
uint packNormalOctahedral(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-8);
    vec2 e = n.xy;
    if (n.z < 0.0)
    {
        e = (1.0 - abs(n.yx)) *
            vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    uvec2 q = uvec2(clamp(e * 0.5 + 0.5, vec2(0.0), vec2(1.0)) * 65535.0 + 0.5);
    return q.x | (q.y << 16u);
}

vec3 unpackNormalOctahedral(uint p)
{
    vec2 e = vec2(uvec2(p & 0xffffu, p >> 16u)) / 65535.0 * 2.0 - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) *
               vec2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// =============================================================================
// 2026-10-31 HDR 修复：**alpha 车道改作"范围缩放"**（旧代码恒填 1.0 且全仓无人读）。
//
// 病征（用户）："天空怎么感觉这么暗"，出现在 3a37220（payload 124B -> 64B）之后。
// 机制：同一个字段既装材质色（0..1），也装 **rmiss 的天空辐射（HDR，>1）**：
//     旧：payload.albedo = clamp(finalColor, 0.0, 2.5);      // vec3，能到 2.5
//     新：payload.albedoPacked = packAlbedoUnorm(clamp(finalColor, 0.0, 2.5));
//   而 packUnorm4x8 **把每个通道钳到 [0,1]** ⇒ 那句 clamp(…, 2.5) 成了死参数，
//   天空 1.0~2.5 的部分（地平线辉光、日晕、亮云边）被整段削平，
//   同时整幅天空被量化到 1/255 台阶 ⇒ 视觉上就是"天空变暗、变平、带色带"。
//   受影响最大的正是天空 —— 材质 albedo 本来就 ≤1，只有天空会超过 1。
//
// 修法（零调用点改动、零 payload 增长）：把归一化的**缩放**塞进没人用的 alpha 车道。
//   · 材质色（命中）：alpha = 0 ⇒ 解码缩放 1.0，与改动前逐位同值；
//   · 天空（miss）  ：alpha = (scale-1)/(2.5-1)，scale = clamp(峰值, 1, 2.5)
//                     ⇒ 1.0~2.5 的范围被完整还原，上限与旧档 clamp(…,2.5) 一致。
// 所有 16 处 `unpackAlbedoUnorm(payload.albedoPacked)` 天空读取点因此**自动**拿到
// HDR，无需逐个改（这也是没有选"再加一个 vec3 天空字段"的原因：那要改十几处，
// 且 payload 会重新变大）。
// 已知残留：dim 天空（夜间 ~0.02）仍是 8 bit 绝对量化，色带风险与修复前相同；
// 要根治需要独立 HDR 通道，等 payload 预算再议。
// =============================================================================
const float SKY_ALBEDO_PACK_RANGE_MAX = 2.5;

uint packAlbedoUnorm(vec3 a)
{
    // alpha = 0 ⇒ unpackAlbedoUnorm 的缩放为 1（材质色路径，行为不变）。
    return packUnorm4x8(vec4(clamp(a, vec3(0.0), vec3(1.0)), 0.0));
}

// 天空辐射专用打包：按峰值归一化到 [0,1]，峰值写进 alpha。
uint packSkyRadianceUnorm(vec3 c)
{
    const vec3 clamped = clamp(c, vec3(0.0), vec3(SKY_ALBEDO_PACK_RANGE_MAX));
    const float peak = max(clamped.r, max(clamped.g, clamped.b));
    const float scale = clamp(peak, 1.0, SKY_ALBEDO_PACK_RANGE_MAX);
    return packUnorm4x8(vec4(
        clamped / scale,
        (scale - 1.0) / (SKY_ALBEDO_PACK_RANGE_MAX - 1.0)));
}

vec3 unpackAlbedoUnorm(uint p)
{
    const vec4 v = unpackUnorm4x8(p);
    // alpha == 0（材质色）⇒ 1.0；天空 ⇒ 还原 1.0 ~ 2.5 倍范围。
    return v.rgb * (1.0 + v.a * (SKY_ALBEDO_PACK_RANGE_MAX - 1.0));
}

struct HitPayload {
    float hitDistance;
    // 2026-10-31 第二阶段（normal/albedo 压缩）实测**全屏变黑**，已回退成 vec3。
    // 原因未定位（八面体编码与 unorm8 编码都可能是元凶），两处同时改无法区分。
    // 复现路径：hit_payload.glsl 的 packNormalOctahedral/packAlbedoUnorm 保留在下方，
    // 但字段改回 vec3 ⇒ 打包版本从 *.bak_pre_pack_20261031 与 git 历史恢复后逐个字段重试。
    vec3  normal;
    // 2026-10-31 单字段试验（第一阶段）：albedo -> unorm8x4。
    // 与 normal 的八面体压缩分两次做，避免上次"两个一起改导致全黑却无法区分"。
    uint  albedoPacked;
    uint state;
    float roughness;
    float f0Metal;
    vec3 emission;
    float parallaxRayTravel;
    float pomSelfShadow;
    float pomMicroOcclusion;
    float glassAlpha;

};

#endif
