#version 460
#extension GL_GOOGLE_include_directive : require

// ============================================================================
// Orion Built-in Far-Field LOD Rasterizer - Vertex Shader
//
// Reads per-tile world-relative vertices (tileOrigin push constant + local xyz
// to preserve sub-millimeter precision at arbitrarily large world coordinates).
// Projects with Orion's exact CameraUBO.viewProj so FOV, aspect ratio, camera
// tilt/bobbing and DLSS jitter match the primary RT rays pixel-for-pixel.
//
// Clip-space conventions:
//   - CameraUBO.viewProj produces OpenGL/MC NDC where Y=+1 is screen top.
//   - Vulkan raster framebuffer has Y=-1 at top -> negate clip.y.
//   - Infinite reverse-Z: gl_Position.z = LOD_NEAR_Z (0.5), w = origClip.w,
//     clear depth = 0.0, compareOp = GREATER_OR_EQUAL -> zero far-plane clip
//     all the way past 4096+ blocks with high depth precision.
// ============================================================================

layout(set = 0, binding = 0) uniform CameraProperties
{
    #include "camera_ubo.glsl"
} cam;

layout(push_constant) uniform OrionLodPush
{
    vec4 tileOriginAndFlags; // xyz = tile world origin, w = unused
} pc;

layout(location = 0) in vec3 inLocalPos;
layout(location = 1) in uint inPackedColorAo;
layout(location = 2) in uint inPackedNormalMat;

layout(location = 0) out vec4 vColorAo;
layout(location = 1) out vec4 vNormalMat;
layout(location = 2) out vec3 vRelPos;

vec4 orionUnpackRgba8(uint packedVal)
{
    return vec4(
        float(packedVal & 0xFFu),
        float((packedVal >> 8u) & 0xFFu),
        float((packedVal >> 16u) & 0xFFu),
        float((packedVal >> 24u) & 0xFFu)
    ) * (1.0 / 255.0);
}

void main()
{
    vec3 camPos = cam.viewInverse[3].xyz;
    vec3 worldPos = pc.tileOriginAndFlags.xyz + inLocalPos;
    vRelPos = worldPos - camPos;

    vec4 origClip = cam.viewProj * vec4(worldPos, 1.0);
    const float lodNearZ = 0.5;
    gl_Position = vec4(origClip.x, -origClip.y, lodNearZ, origClip.w);

    vColorAo = orionUnpackRgba8(inPackedColorAo);
    vNormalMat = orionUnpackRgba8(inPackedNormalMat);
}
