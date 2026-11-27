#version 460

// ============================================================================
// Orion Built-in Far-Field LOD Rasterizer - Fragment Shader
//
// Outputs compact G-buffer to 3 attachments + D32_SFLOAT hardware depth:
//   outColorAo   (RGBA8,  binding 96)  : rgb = sRGB albedo, a = baked skyAO
//   outNormalMat (RGBA8,  binding 101) : rg = Oct normal, b = matClass/4, a = waterDepth/rough
//   outLinearDist(R32F,   binding 97)  : Euclidean distance to camera (0.0 = sky)
// ============================================================================

layout(location = 0) in vec4 vColorAo;
layout(location = 1) in vec4 vNormalMat;
layout(location = 2) in vec3 vRelPos;

layout(location = 0) out vec4 outColorAo;
layout(location = 1) out vec4 outNormalMat;
layout(location = 2) out float outLinearDist;

void main()
{
    outColorAo = vColorAo;
    outNormalMat = vNormalMat;
    outLinearDist = max(length(vRelPos), 0.5);
}
