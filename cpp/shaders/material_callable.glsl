#ifndef MATERIAL_CALLABLE_GLSL
#define MATERIAL_CALLABLE_GLSL

// =============================================================================
// D1（材质 callable 化，2026-11-05）：材质求值的 callable 契约。
//
// rt_test_material.spv（由 .tmp/rgen_split/gen_material_callable.py 从
// rt_test.rchit 前言逐字节生成）独占材质层（三段 helper + material_resolve.glsl）。
// B① 时代 rgen 侧那份完整内联副本（原 rgen_k2_material.glsl）随之退役。
//
// ⚠ Vulkan callable 数据是**单向**的（caller → callee），callable 着色器没有
// "返回值"通道 —— callableDataEXT 是调用方的"传出"，callableDataInEXT 是
// callable 的"读入"，两者没有任何 callee→caller 的通路。材质字段的回传借用
// **landing 槽位**：K2 调用点手里有 (pixel,s) 槽号（该槽的 identity 记录已在
// ptLandingLoad 时读进 payload，dbgMotion 已带出，之后无人再读），
// matCallableResolveToLanding 把槽号交给 callable，callable 解析完把材质字段
// 以"结果映射"写回同一槽位，调用方读回 payload。零新增绑定、零新增显存。
//
// K1 rchit 的次级命中本轮**不走** callable：命中着色器拿不到样本号 s，没有
// 无冲突的槽位可借（同像素不同样本的 lane 并发）⇒ K1 保持 B① 的行内解析
// （rt_test.rchit），待后续给出 s 通道（如 payload 空闲位/scratch 环）再迁。
//
// matCallableResolveToLanding 引用调用方的 `payload` 与 rgen_k1_landing.glsl
// 的 PTLandingRecord/ptLandingRecords ⇒ 必须在这两者之后 include。
// =============================================================================

// 进（调用方写、callable 读）：
//   a = (hitDistance, slot, primitive, baryX)
//   b = (baryY, rayOrigin.xyz)
//   c = (rayDirection.xyz, _)
//   u = (state, landing 槽位, 0, 0)
// 同一个块在两个 TU 里类不同：callable TU（ORION_MATERIAL_CALLABLE，生成脚本
// 写入）读入，调用方 TU 写出 —— 结构必须逐字节一致。
#ifdef ORION_MATERIAL_CALLABLE
layout(location = 0) callableDataInEXT MaterialCallIn
#else
layout(location = 0) callableDataEXT MaterialCallIn
#endif
{
    vec4  a;
    vec4  b;
    vec4  c;
    uvec4 u;
} matIn;

// 结果映射（callable 写、调用方读回，借 PTLandingRecord 的 80B，类型按 B①
// 身份版记录：a=vec4 b/c=uvec4 d/e=vec4）：
//   a = (hitDistance, roughness, f0Metal, glassAlpha)
//   b = (bits(normal.xyz), bits(pomSelfShadow), _)
//   c = (albedoPacked, state, _, _)
//   d = (emission.xyz, pomMicroOcclusion, _, _)
//   e = (parallaxRayTravel, _, _, _)   // e.zw 的 dbgMotion 已在载入时读走

// 只在调用方 TU 编译：callable TU 不再发起 callable（glslang 对 callable
// 阶段的 executeCallableEXT 支持面窄），且 callable 主入口直接操作 matIn。
#ifndef ORION_MATERIAL_CALLABLE

#ifdef ORION_K2_NO_CALLABLE
// 2026-11-05 设备丢失二分桩（DLL env ORION_K2_VARIANT=1）：K2 其余不动，
// 只把 executeCallableEXT 拿掉。材质字段按"解析后"语义给常量 —— payload.normal
// 必须清零（identity 的 slot/primitive 不能被下游当资源索引误读），state 和
// hitDistance 保持 landing 载入值（transport 消费它们）。
void matCallableResolveToLanding(
    vec3 rayOrigin,
    vec3 rayDirection,
    uint landingSlot)
{
    // GLSL 没有 (void)x 丢弃写法；未用形参直接留着即可。
    payload.normal = vec3(0.0);
    payload.roughness = 1.0;
    payload.f0Metal = 0.0;
    payload.glassAlpha = 1.0;
    payload.pomSelfShadow = 1.0;
    payload.albedoPacked = packAlbedoUnorm(vec3(0.5));
    payload.emission = vec3(0.0);
    payload.pomMicroOcclusion = 1.0;
    payload.parallaxRayTravel = 0.0;
}
#else
void matCallableResolveToLanding(
    vec3 rayOrigin,
    vec3 rayDirection,
    uint landingSlot)
{
    matIn.a = vec4(payload.hitDistance, payload.normal);
    matIn.b = vec4(payload.roughness, rayOrigin);
    matIn.c = vec4(rayDirection, 0.0);
    matIn.u = uvec4(payload.state, landingSlot, 0u, 0u);

    // RT 计数器：每经 callable 解析一次材质计一次 material evals（rgen 侧漏斗，
    // 比数 rchit 少一层争用）。K1 / K2_NO_CALLABLE 内联或常量桩不走本漏斗，
    // 不计入本 lane（口径见 rt_stage_counters.glsl）。
    #ifdef ORION_RT_STAGE_COUNTERS
    rtCntMateval();
    #endif

    // glslang 的 executeCallableEXT 内建签名是 int 形参，uint 字面量重载解析失败。
    executeCallableEXT(0, 0);

    const PTLandingRecord result = ptLandingRecords[landingSlot];
    payload.hitDistance       = result.a.x;
    payload.roughness         = result.a.y;
    payload.f0Metal = result.a.z;
    payload.glassAlpha = result.a.w;
    payload.normal            = vec3(
        uintBitsToFloat(result.b.x),
        uintBitsToFloat(result.b.y),
        uintBitsToFloat(result.b.z));
    payload.pomSelfShadow = uintBitsToFloat(result.b.w);
    payload.albedoPacked      = result.c.x;
    payload.state             = result.c.y;
    payload.emission          = result.d.xyz;
    payload.pomMicroOcclusion = result.d.w;
    payload.parallaxRayTravel = uintBitsToFloat(result.c.z);
}
#endif // ORION_K2_NO_CALLABLE
#endif // !ORION_MATERIAL_CALLABLE
#endif // MATERIAL_CALLABLE_GLSL
