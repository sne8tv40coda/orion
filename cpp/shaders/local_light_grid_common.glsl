#ifndef LOCAL_LIGHT_GRID_COMMON_GLSL
#define LOCAL_LIGHT_GRID_COMMON_GLSL

// 光源档案表（色温/发射色、满级亮度、基础传播距离）。雾、体素 GI 与主 raygen
// 共用同一份，避免再出现"rgen 与雾两份调色板取值不同"的历史问题。
#include "local_light_profile.glsl"

// ---- binding 12：本地光表（与 rgen 的 binding 17 同一 buffer）----
// 记录格式与 C++ 的 GPULocalLightRecord 逐字节对应（32 字节）：
//   worldPosEmission = (blockX, blockY, blockZ, emission 0..15)
//   meta.x = typeCode（decode 用；0 = 未分类）
struct LocalLightGPU
{
    vec4 worldPosEmission;
    uvec4 meta;
};

layout(std430, binding = 12, set = 0) readonly buffer FogLocalLightBuffer
{
    uvec4 header;
    LocalLightGPU lights[];
} fogLightBuffer;
layout(std430, binding = 13, set = 0) readonly buffer FogLocalLightGridBuffer
{
    uint words[];
} fogLightGrid;

const float LOCAL_LIGHT_GRID_CELL_SIZE = 64.0;
const float LOCAL_LIGHT_MAX_RANGE = 30.0;
const float VPT_LOCAL_MAX_DISTANCE = 30.0;
const float VPT_LOCAL_FADE_START_FRACTION = 0.40;
const float LOCAL_LIGHT_ENERGY_SCALE = 0.35;

const int FOG_LIGHT_MAX = 8;

uint localLightGridHash32(ivec3 cellCoord)
{
    uint h = uint(cellCoord.x) * 0x8da6b343u;
    h ^= uint(cellCoord.y) * 0xd8163841u;
    h ^= uint(cellCoord.z) * 0xcb1ab31fu;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    return h;
}

ivec3 localLightGridCellCoord(vec3 worldPos)
{
    return ivec3(floor(worldPos / LOCAL_LIGHT_GRID_CELL_SIZE));
}

int localLightGridNeighborMinOffset(float worldComponent)
{
    float cellBase = floor(worldComponent / LOCAL_LIGHT_GRID_CELL_SIZE) *
        LOCAL_LIGHT_GRID_CELL_SIZE;
    return worldComponent - cellBase < LOCAL_LIGHT_MAX_RANGE ? -1 : 0;
}

int localLightGridNeighborMaxOffset(float worldComponent)
{
    float cellBase = floor(worldComponent / LOCAL_LIGHT_GRID_CELL_SIZE) *
        LOCAL_LIGHT_GRID_CELL_SIZE;
    return cellBase + LOCAL_LIGHT_GRID_CELL_SIZE - worldComponent <
        LOCAL_LIGHT_MAX_RANGE ? 1 : 0;
}

uint localLightGridPackedIndexBase()
{
    return 4u + fogLightGrid.words[2] * 8u;
}

bool localLightGridLookupCell(
    ivec3 cellCoord,
    out uint firstLightIndex,
    out uint lightCount)
{
    uint hashCapacity = fogLightGrid.words[2];
    if(hashCapacity == 0u)
    {
        firstLightIndex = 0u;
        lightCount = 0u;
        return false;
    }

    uint hashMask = hashCapacity - 1u;
    uint slotIndex = localLightGridHash32(cellCoord) & hashMask;

    for(uint probe = 0u; probe < hashCapacity; ++probe)
    {
        uint slotBase = 4u + slotIndex * 8u;
        if(fogLightGrid.words[slotBase + 3u] == 0u)
        {
            firstLightIndex = 0u;
            lightCount = 0u;
            return false;
        }
        if(int(fogLightGrid.words[slotBase]) == cellCoord.x &&
           int(fogLightGrid.words[slotBase + 1u]) == cellCoord.y &&
           int(fogLightGrid.words[slotBase + 2u]) == cellCoord.z)
        {
            firstLightIndex = fogLightGrid.words[slotBase + 4u];
            lightCount = fogLightGrid.words[slotBase + 5u];
            return true;
        }
        slotIndex = (slotIndex + 1u) & hashMask;
    }

    firstLightIndex = 0u;
    lightCount = 0u;
    return false;
}

// 用户旋钮「本地光距离系数」（0.25 ~ 2.0，1.0 = 标准）。旧 DLL 恒写 0
// （reserved 位）⇒ 回落 1.0，双向兼容。
float localLightRangeScale()
{
    float user = cam.rainWallParams1.w;
    return (user >= 0.05 && user <= 8.0) ? user : 1.0;
}

// 传播距离 = 该光源档案的基础距离 × 用户系数，最后封顶 30 格。
// 2026-10-31：不再用"emission × 2 格"的统一口径 —— 火把、岩浆、信标各自在
// local_light_profile.glsl 里有物理上合理的距离（岩浆 26 格、火把 11 格…）。
float vptLocalLightRange(uint typeCode, float emissionLevel)
{
    float base = localLightProfileBaseRange(typeCode, emissionLevel);
    return clamp(base * localLightRangeScale(), 1.5, VPT_LOCAL_MAX_DISTANCE);
}

float vptLocalLightRangeFade(
    float distanceToLight,
    uint typeCode,
    float emissionLevel)
{
    float range = vptLocalLightRange(typeCode, emissionLevel);
    return 1.0 - smoothstep(
        range * VPT_LOCAL_FADE_START_FRACTION,
        range,
        distanceToLight);
}

float vptLocalLightDistanceAttenuation(float distanceToLight)
{
    return 0.55 / (1.0 + 0.18 * distanceToLight +
        0.025 * distanceToLight * distanceToLight);
}

// 发射色：查光源档案表（色温或给定发射色），rgen 与雾共用同一份。
vec3 decodeLocalLightColor(uint typeCode)
{
    return localLightProfileColor(typeCode);
}
// rgen:1318 localLightIntensity()：用户滑杆「本地光源照射强度」，主机写的是
// intensity + 1.0，这里减 1 还原；车道没下发过（0）时退回 1.0。
float localLightIntensity()
{
    float user = uintBitsToFloat(cam.historyPadding1) - 1.0;
    return (user >= 0.0 && user <= 16.0) ? user : 1.0;
}

// 把 gatherPos 所在网格邻域（1 到 8 个 cell）里的灯收进固定数组。
// posEmission[i].w = emission 等级（0..15，未缩放）。
//
// 2026-10-26：本函数改为**转调** fogGatherLocalLightsIndexed()（多输出全局灯索引），
// 保持原签名不变 ⇒ 体积云 v2~v5 的调用点不需要改。索引版的存在理由见下方注释。
// 前置声明：索引版定义在本文件末尾（转调者在前，GLSL 需要先见到原型）。
int fogGatherLocalLightsIndexed(
    vec3 gatherPos,
    out vec4 posEmission[FOG_LIGHT_MAX],
    out uint typeCode[FOG_LIGHT_MAX],
    out uint globalIndex[FOG_LIGHT_MAX]);

int fogGatherLocalLights(
    vec3 gatherPos,
    out vec4 posEmission[FOG_LIGHT_MAX],
    out uint typeCode[FOG_LIGHT_MAX])
{
    uint unusedGlobalIndex[FOG_LIGHT_MAX];

    return fogGatherLocalLightsIndexed(gatherPos, posEmission, typeCode, unusedGlobalIndex);
}

// 与 fogGatherLocalLights 同序同口径，额外输出每盏灯在**灯表里的全局下标**。
//
// 为什么需要它：ReSTIR 的 reservoir 要跨帧（temporal）和跨 froxel（spatial）传递
// 选中样本的身份。若只存"本次收集数组里的第几个"，那个下标只对产生它的 froxel
// 有意义 —— 换一个 froxel 或换一帧就指向另一盏灯，合并结果无意义，integrate
// 阶段也无法据此查回灯的辐亮度。全局下标是唯一稳定的身份。
int fogGatherLocalLightsIndexed(
    vec3 gatherPos,
    out vec4 posEmission[FOG_LIGHT_MAX],
    out uint typeCode[FOG_LIGHT_MAX],
    out uint globalIndex[FOG_LIGHT_MAX])
{
    uint lightCount = fogLightBuffer.header.x;
    if(lightCount == 0u || fogLightGrid.words[2] == 0u)
    {
        return 0;
    }

    ivec3 centerCell = localLightGridCellCoord(gatherPos);
    uint packedIndexBase = localLightGridPackedIndexBase();

    int minCellX = localLightGridNeighborMinOffset(gatherPos.x);
    int maxCellX = localLightGridNeighborMaxOffset(gatherPos.x);
    int minCellY = localLightGridNeighborMinOffset(gatherPos.y);
    int maxCellY = localLightGridNeighborMaxOffset(gatherPos.y);
    int minCellZ = localLightGridNeighborMinOffset(gatherPos.z);
    int maxCellZ = localLightGridNeighborMaxOffset(gatherPos.z);

    int gathered = 0;
    for(int cellZ = minCellZ; cellZ <= maxCellZ; ++cellZ)
    {
        for(int cellY = minCellY; cellY <= maxCellY; ++cellY)
        {
            for(int cellX = minCellX; cellX <= maxCellX; ++cellX)
            {
                uint firstLightIndex = 0u;
                uint cellLightCount = 0u;
                if(!localLightGridLookupCell(
                    centerCell + ivec3(cellX, cellY, cellZ),
                    firstLightIndex,
                    cellLightCount))
                {
                    continue;
                }

                for(uint candidateOffset = 0u;
                    candidateOffset < cellLightCount;
                    ++candidateOffset)
                {
                    uint lightIndex = fogLightGrid.words[
                        packedIndexBase + firstLightIndex + candidateOffset];
                    if(lightIndex >= lightCount)
                    {
                        continue;
                    }
                    LocalLightGPU candidate = fogLightBuffer.lights[lightIndex];
                    posEmission[gathered] = candidate.worldPosEmission;
                    typeCode[gathered] = candidate.meta.x;
                    globalIndex[gathered] = lightIndex;
                    ++gathered;
                    if(gathered >= FOG_LIGHT_MAX)
                    {
                        return gathered;
                    }
                }
            }
        }
    }

    return gathered;
}

#endif
