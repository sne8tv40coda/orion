// Orion backend — Built-in Far-Field LOD Engine (NativeLod)
// 零外部模组依赖、零卡顿/零尖峰（无运行时 vkDeviceWaitIdle、无 g_chunkMutex 争用）、
// 后台网格生成与区域磁盘缓存、Vulkan 硬件光栅 G-Buffer Pass。

#include "OrionInternal.h"
#include "TrackingHooks.h"

#include <filesystem>

namespace
{
constexpr uint32_t ORION_LOD_TILE_DIM = 16u;
constexpr uint32_t ORION_LOD_TILE_CELLS = ORION_LOD_TILE_DIM * ORION_LOD_TILE_DIM;
constexpr uint32_t ORION_LOD_CELL_BYTES = 16u;
constexpr uint32_t ORION_LOD_PAYLOAD_BYTES = ORION_LOD_TILE_CELLS * ORION_LOD_CELL_BYTES; // 4096

constexpr uint32_t ORION_LOD_MAX_LEVELS = 4u;
constexpr uint32_t ORION_LOD_MAX_SLOTS = 2048u;
constexpr uint32_t ORION_LOD_SLOT_VERTS = 3584u;
constexpr uint32_t ORION_LOD_SLOT_BYTES = ORION_LOD_SLOT_VERTS * 20u; // 71,680 B
constexpr VkDeviceSize ORION_LOD_POOL_BYTES =
    static_cast<VkDeviceSize>(ORION_LOD_MAX_SLOTS) * ORION_LOD_SLOT_BYTES; // ~140 MB

constexpr uint32_t ORION_LOD_STAGING_FRAMES = 3u;
constexpr VkDeviceSize ORION_LOD_FRAME_UPLOAD_CAP = 1536ull * 1024ull; // 1.5 MB / frame
constexpr uint32_t ORION_LOD_MAX_TILES_PER_FRAME = 20u;
constexpr uint64_t ORION_LOD_SLOT_RETIRE_FRAMES = 4u;
constexpr size_t ORION_LOD_REAL_CHUNK_CACHE_MAX = 6144u;
constexpr uint32_t ORION_LOD_REGION_MAGIC = 0x444C524Fu; // 'ORLD'
constexpr uint32_t ORION_LOD_REGION_VERSION = 3u;

#pragma pack(push, 1)
struct OrionLodColumn
{
    int16_t topY;
    int16_t subY;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t flags;      // bit0..2: matId, bit3: hasWater, bit4: valid
    uint8_t subR;
    uint8_t subG;
    uint8_t subB;
    uint8_t skyLight;
    uint8_t waterDepth;
    uint8_t pad0;
    uint8_t pad1;
    uint8_t pad2;
};
static_assert(sizeof(OrionLodColumn) == 16u, "OrionLodColumn must be 16 bytes");

struct OrionLodVertex
{
    float lx;
    float ly;
    float lz;
    uint32_t packedColorAo;
    uint32_t packedNormalMat;
};
static_assert(sizeof(OrionLodVertex) == 20u, "OrionLodVertex must be 20 bytes");
#pragma pack(pop)

struct OrionLodPushConstants
{
    float tileOriginX;
    float tileOriginY;
    float tileOriginZ;
    float tileStep;
};
static_assert(sizeof(OrionLodPushConstants) == 16u, "OrionLodPushConstants must be 16 bytes");

struct TileKey
{
    int32_t lod;
    int32_t tx;
    int32_t tz;

    bool operator==(const TileKey& other) const noexcept
    {
        return lod == other.lod && tx == other.tx && tz == other.tz;
    }
};

struct TileKeyHash
{
    size_t operator()(const TileKey& k) const noexcept
    {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(k.lod)) * 0x9e3779b97f4a7c15ull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(k.tx)) * 0xbf58476d1ce4e5b9ull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(k.tz)) * 0x94d049bb133111ebull;
        return static_cast<size_t>(h ^ (h >> 30u));
    }
};

struct ChunkKey
{
    int32_t cx;
    int32_t cz;
    bool operator==(const ChunkKey& o) const noexcept { return cx == o.cx && cz == o.cz; }
};

struct ChunkKeyHash
{
    size_t operator()(const ChunkKey& k) const noexcept
    {
        uint64_t h = (static_cast<uint64_t>(static_cast<uint32_t>(k.cx)) << 32u) |
                     static_cast<uint32_t>(k.cz);
        h ^= h >> 33u;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33u;
        return static_cast<size_t>(h);
    }
};

struct RealChunkEntry
{
    std::array<OrionLodColumn, ORION_LOD_TILE_CELLS> cols{};
    uint64_t lastTouchTick = 0u;
    bool dirtyDisk = false;
};

struct CpuTileData
{
    std::array<OrionLodColumn, ORION_LOD_TILE_CELLS> genCols{};
    std::array<OrionLodColumn, ORION_LOD_TILE_CELLS> realCols{};
    std::array<uint8_t, ORION_LOD_TILE_CELLS> hasGen{};
    std::array<uint8_t, ORION_LOD_TILE_CELLS> hasReal{};
    bool dirtyMesh = false;
    uint64_t generation = 0u;
};

struct PendingUploadTile
{
    TileKey key{};
    int16_t minY = 0;
    int16_t maxY = 0;
    uint64_t generation = 0u;
    std::vector<OrionLodVertex> vertices;
};

struct GpuTileResident
{
    TileKey key{};
    uint32_t slotId = UINT32_MAX;
    uint32_t vertexCount = 0u;
    int16_t minY = 0;
    int16_t maxY = 0;
    uint64_t lastUsedFrame = 0u;
};

struct RetiredSlot
{
    uint64_t retireAfterFrame = 0u;
    uint32_t slotId = UINT32_MAX;
};

struct PendingInputItem
{
    bool isRealChunk = false;
    int32_t lod = 0;
    int32_t x = 0;
    int32_t z = 0;
    bool isRealFlag = false;
    uint64_t epoch = 0u;
    std::array<OrionLodColumn, ORION_LOD_TILE_CELLS> cols{};
};

// ---------------------------------------------------------------------------
// 独立状态与零竞争锁域（绝不触碰 g_chunkMutex）
// ---------------------------------------------------------------------------
static std::mutex g_lodQueueMutex;
static std::condition_variable g_lodQueueCv;
static std::deque<PendingInputItem> g_lodInputQueue;
static std::string g_lodCacheDir;
static bool g_lodWorkerRunning = false;
static bool g_lodWorkerStop = false;
static std::thread g_lodWorkerThread;

static std::mutex g_lodUploadMutex;
static std::unordered_map<TileKey, PendingUploadTile, TileKeyHash> g_lodReadyUploads;

static std::atomic<bool> g_lodEnabled{false};
static std::atomic<int32_t> g_lodMaxDistanceChunks{96};
static std::atomic<int32_t> g_lodSeaLevel{63};
static std::atomic<int32_t> g_lodMinY{-64};
static std::atomic<int32_t> g_lodMaxY{320};
static std::atomic<int32_t> g_lodDimKind{0}; // 0=overworld, 1=nether, 2=end
static std::atomic<uint64_t> g_lodEpoch{1u};

static std::atomic<double> g_lodCamX{0.0};
static std::atomic<double> g_lodCamY{64.0};
static std::atomic<double> g_lodCamZ{0.0};
static std::atomic<int32_t> g_lodVanillaRd{12};

// 仅由渲染线程访问的 GPU 驻留状态
static std::unordered_map<TileKey, GpuTileResident, TileKeyHash> g_gpuTiles;
static std::vector<uint32_t> g_freeSlots;
static std::deque<RetiredSlot> g_retiredSlots;
static uint64_t g_renderedEpoch = 0u;
static uint64_t g_lodRenderFrame = 0u;

// Vulkan 资源
static VkImage g_orionLodColorImage = VK_NULL_HANDLE;
static VkDeviceMemory g_orionLodColorMemory = VK_NULL_HANDLE;
static VkImageView g_orionLodColorView = VK_NULL_HANDLE;

static VkImage g_orionLodNormalMatImage = VK_NULL_HANDLE;
static VkDeviceMemory g_orionLodNormalMatMemory = VK_NULL_HANDLE;
static VkImageView g_orionLodNormalMatView = VK_NULL_HANDLE;

static VkImage g_orionLodDepthOutImage = VK_NULL_HANDLE;
static VkDeviceMemory g_orionLodDepthOutMemory = VK_NULL_HANDLE;
static VkImageView g_orionLodDepthOutView = VK_NULL_HANDLE;

static VkImage g_orionLodHwDepthImage = VK_NULL_HANDLE;
static VkDeviceMemory g_orionLodHwDepthMemory = VK_NULL_HANDLE;
static VkImageView g_orionLodHwDepthView = VK_NULL_HANDLE;

static VkImage g_orionLodDummyRgba8Image = VK_NULL_HANDLE;
static VkDeviceMemory g_orionLodDummyRgba8Memory = VK_NULL_HANDLE;
static VkImageView g_orionLodDummyRgba8View = VK_NULL_HANDLE;

static VkImage g_orionLodDummyR32fImage = VK_NULL_HANDLE;
static VkDeviceMemory g_orionLodDummyR32fMemory = VK_NULL_HANDLE;
static VkImageView g_orionLodDummyR32fView = VK_NULL_HANDLE;

static uint32_t g_orionLodWidth = 0u;
static uint32_t g_orionLodHeight = 0u;

static VkRenderPass g_orionLodRenderPass = VK_NULL_HANDLE;
static VkFramebuffer g_orionLodFramebuffer = VK_NULL_HANDLE;
static VkDescriptorSetLayout g_orionLodSetLayout = VK_NULL_HANDLE;
static VkDescriptorPool g_orionLodDescriptorPool = VK_NULL_HANDLE;
static VkDescriptorSet g_orionLodDescriptorSet = VK_NULL_HANDLE;
static VkPipelineLayout g_orionLodPipelineLayout = VK_NULL_HANDLE;
static VkPipeline g_orionLodPipeline = VK_NULL_HANDLE;

static VkBuffer g_orionLodPoolBuffer = VK_NULL_HANDLE;
static VkDeviceMemory g_orionLodPoolMemory = VK_NULL_HANDLE;

static VkBuffer g_orionLodStagingBuffer[ORION_LOD_STAGING_FRAMES]{};
static VkDeviceMemory g_orionLodStagingMemory[ORION_LOD_STAGING_FRAMES]{};
static void* g_orionLodStagingMapped[ORION_LOD_STAGING_FRAMES]{};

static VkDescriptorSet g_orionLodBoundRtSet = VK_NULL_HANDLE;
static bool g_orionLodBoundActive = false;

// ---------------------------------------------------------------------------
// 纯工具函数（数学、编码、网格）
// ---------------------------------------------------------------------------
static inline int32_t floorDiv(int32_t a, int32_t b)
{
    int32_t q = a / b;
    int32_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
    {
        --q;
    }
    return q;
}

static inline int32_t floorMod(int32_t a, int32_t b)
{
    int32_t r = a % b;
    if (r < 0)
    {
        r += (b < 0) ? -b : b;
    }
    return r;
}

static inline int32_t lodStepForLevel(int32_t lod)
{
    return 2 << std::clamp(lod, 0, static_cast<int32_t>(ORION_LOD_MAX_LEVELS - 1u));
}

static inline int32_t lodTileBlocksForLevel(int32_t lod)
{
    return lodStepForLevel(lod) * static_cast<int32_t>(ORION_LOD_TILE_DIM);
}

static inline uint32_t packRgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8u) |
           (static_cast<uint32_t>(b) << 16u) |
           (static_cast<uint32_t>(a) << 24u);
}

static uint32_t encodeOctNormalAndMat(
    float nx, float ny, float nz,
    uint8_t matClass, uint8_t auxByte)
{
    const float sum = std::abs(nx) + std::abs(ny) + std::abs(nz);
    float ex = 0.0f;
    float ey = 0.0f;
    if (sum > 1.0e-6f)
    {
        const float inv = 1.0f / sum;
        ex = nx * inv;
        ey = ny * inv;
        const float ez = nz * inv;
        if (ez < 0.0f)
        {
            const float ox = (1.0f - std::abs(ey)) * (ex >= 0.0f ? 1.0f : -1.0f);
            const float oy = (1.0f - std::abs(ex)) * (ey >= 0.0f ? 1.0f : -1.0f);
            ex = ox;
            ey = oy;
        }
    }
    const auto toByte = [](float v) -> uint8_t {
        const float u = std::clamp(v * 0.5f + 0.5f, 0.0f, 1.0f);
        return static_cast<uint8_t>(std::lround(u * 255.0f));
    };
    const uint8_t matByte = static_cast<uint8_t>(std::min<uint32_t>(matClass, 4u) * 64u);
    return packRgba8(toByte(ex), toByte(ey), matByte, auxByte);
}

static inline bool colValid(const OrionLodColumn& c)
{
    return (c.flags & 0x10u) != 0u && c.topY > -32000;
}

static inline OrionLodColumn pickEffectiveCol(const CpuTileData& t, size_t idx)
{
    if (t.hasReal[idx] && colValid(t.realCols[idx]))
    {
        return t.realCols[idx];
    }
    if (t.hasGen[idx] && colValid(t.genCols[idx]))
    {
        return t.genCols[idx];
    }
    OrionLodColumn empty{};
    empty.topY = -32768;
    empty.subY = -32768;
    return empty;
}

static void pushQuad(
    std::vector<OrionLodVertex>& out,
    float x0, float y0, float z0,
    float x1, float y1, float z1,
    float x2, float y2, float z2,
    float x3, float y3, float z3,
    uint32_t packedColorAo,
    uint32_t packedNormalMat)
{
    out.push_back({x0, y0, z0, packedColorAo, packedNormalMat});
    out.push_back({x1, y1, z1, packedColorAo, packedNormalMat});
    out.push_back({x2, y2, z2, packedColorAo, packedNormalMat});

    out.push_back({x0, y0, z0, packedColorAo, packedNormalMat});
    out.push_back({x2, y2, z2, packedColorAo, packedNormalMat});
    out.push_back({x3, y3, z3, packedColorAo, packedNormalMat});
}

static PendingUploadTile buildTileMesh(
    const TileKey& key,
    const CpuTileData& tileData,
    uint64_t generation)
{
    PendingUploadTile result;
    result.key = key;
    result.generation = generation;

    std::array<OrionLodColumn, ORION_LOD_TILE_CELLS> cols{};
    int validCount = 0;
    int16_t tileMinY = 32767;
    int16_t tileMaxY = -32768;

    for (size_t i = 0; i < ORION_LOD_TILE_CELLS; ++i)
    {
        cols[i] = pickEffectiveCol(tileData, i);
        if (colValid(cols[i]))
        {
            ++validCount;
            tileMinY = std::min(tileMinY, std::min(cols[i].topY, cols[i].subY));
            tileMaxY = std::max(tileMaxY, cols[i].topY);
        }
    }

    if (validCount == 0)
    {
        return result;
    }

    const int32_t step = lodStepForLevel(key.lod);
    const float fStep = static_cast<float>(step);
    const float skirtDepth = std::max(12.0f, fStep * 2.25f);
    result.minY = static_cast<int16_t>(std::max<int32_t>(-512, static_cast<int32_t>(tileMinY) - static_cast<int32_t>(skirtDepth)));
    result.maxY = tileMaxY;

    // 预留顶面 + 常见悬崖侧壁空间；若极端破碎地形接近容量上限则提高侧墙门槛防溢出
    result.vertices.reserve(static_cast<size_t>(validCount) * 10u);

    auto getCol = [&](int gx, int gz) -> const OrionLodColumn* {
        if (gx < 0 || gx >= static_cast<int>(ORION_LOD_TILE_DIM) ||
            gz < 0 || gz >= static_cast<int>(ORION_LOD_TILE_DIM))
        {
            return nullptr;
        }
        const OrionLodColumn& c = cols[static_cast<size_t>(gz) * ORION_LOD_TILE_DIM + static_cast<size_t>(gx)];
        return colValid(c) ? &c : nullptr;
    };

    // 1. 生成全部有效网格的顶面（顶面总数最多 256*6 = 1536 顶点，远低于 3584 上限）
    for (int gz = 0; gz < static_cast<int>(ORION_LOD_TILE_DIM); ++gz)
    {
        for (int gx = 0; gx < static_cast<int>(ORION_LOD_TILE_DIM); ++gx)
        {
            const OrionLodColumn* c = getCol(gx, gz);
            if (!c)
            {
                continue;
            }
            const uint8_t matId = static_cast<uint8_t>(c->flags & 0x07u);
            const bool isWater = (matId == 1u) || ((c->flags & 0x08u) != 0u);
            const float h = static_cast<float>(c->topY) + (isWater ? -0.06f : 0.0f);

            float nx = 0.0f, ny = 1.0f, nz = 0.0f;
            float slopeOcc = 0.0f;
            if (!isWater)
            {
                const OrionLodColumn* cL = getCol(gx - 1, gz);
                const OrionLodColumn* cR = getCol(gx + 1, gz);
                const OrionLodColumn* cU = getCol(gx, gz - 1);
                const OrionLodColumn* cD = getCol(gx, gz + 1);
                const float hL = cL ? static_cast<float>(cL->topY) : h;
                const float hR = cR ? static_cast<float>(cR->topY) : h;
                const float hU = cU ? static_cast<float>(cU->topY) : h;
                const float hD = cD ? static_cast<float>(cD->topY) : h;

                const float ddx = hL - hR;
                const float ddy = 2.0f * fStep;
                const float ddz = hU - hD;
                const float invLen = 1.0f / std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                nx = ddx * invLen;
                ny = ddy * invLen;
                nz = ddz * invLen;

                slopeOcc = (std::max(0.0f, hL - h) + std::max(0.0f, hR - h) +
                            std::max(0.0f, hU - h) + std::max(0.0f, hD - h)) * 0.25f;
            }

            const float sky01 = static_cast<float>(c->skyLight) * (1.0f / 255.0f);
            const float ao = std::clamp((0.68f + 0.32f * sky01) * (1.0f - std::min(0.28f, slopeOcc / std::max(6.0f, fStep * 1.8f))), 0.55f, 1.0f);
            const uint8_t aoByte = static_cast<uint8_t>(std::lround(ao * 255.0f));
            const uint32_t colorAo = packRgba8(c->r, c->g, c->b, aoByte);
            const uint32_t normMat = encodeOctNormalAndMat(nx, ny, nz, matId, c->waterDepth);

            const float x0 = static_cast<float>(gx) * fStep;
            const float z0 = static_cast<float>(gz) * fStep;
            const float x1 = x0 + fStep;
            const float z1 = z0 + fStep;

            pushQuad(
                result.vertices,
                x0, h, z0,
                x0, h, z1,
                x1, h, z1,
                x1, h, z0,
                colorAo,
                normMat);
        }
    }

    // 2. 生成悬崖侧墙 + 瓦片边界裙边（杜绝高度落差与环接缝漏天）
    struct NeighborOffset { int dx; int dz; float nx; float nz; };
    static constexpr NeighborOffset kDirs[4] = {
        {-1,  0, -1.0f,  0.0f},
        { 1,  0,  1.0f,  0.0f},
        { 0, -1,  0.0f, -1.0f},
        { 0,  1,  0.0f,  1.0f}
    };

    // 根据已用顶点数动态决定最小起墙阈值，保证绝对不超单 Slot 容量
    for (int gz = 0; gz < static_cast<int>(ORION_LOD_TILE_DIM); ++gz)
    {
        for (int gx = 0; gx < static_cast<int>(ORION_LOD_TILE_DIM); ++gx)
        {
            if (result.vertices.size() + 6u > ORION_LOD_SLOT_VERTS)
            {
                break;
            }
            const OrionLodColumn* c = getCol(gx, gz);
            if (!c)
            {
                continue;
            }
            const uint8_t matId = static_cast<uint8_t>(c->flags & 0x07u);
            const bool isWater = (matId == 1u) || ((c->flags & 0x08u) != 0u);
            const float h = static_cast<float>(c->topY) + (isWater ? -0.06f : 0.0f);

            const float x0 = static_cast<float>(gx) * fStep;
            const float z0 = static_cast<float>(gz) * fStep;
            const float x1 = x0 + fStep;
            const float z1 = z0 + fStep;

            const size_t remainingVerts = ORION_LOD_SLOT_VERTS - result.vertices.size();
            const float minWallDrop = (remainingVerts < 512u) ? 2.5f : 0.6f;

            for (const auto& dir : kDirs)
            {
                if (result.vertices.size() + 6u > ORION_LOD_SLOT_VERTS)
                {
                    break;
                }
                const int ngx = gx + dir.dx;
                const int ngz = gz + dir.dz;
                const OrionLodColumn* nc = getCol(ngx, ngz);
                float bot = h - skirtDepth;
                bool drawSide = false;
                if (nc == nullptr)
                {
                    // 边界或邻格空白：裙边向下封底至海平面以下，彻底消除悬崖底部漏空白洞
                    const float seaFloor = static_cast<float>(g_lodSeaLevel.load()) - 8.0f;
                    bot = std::max(h - 140.0f, std::min(h - skirtDepth, seaFloor));
                    drawSide = true;
                }
                else
                {
                    const uint8_t nMat = static_cast<uint8_t>(nc->flags & 0x07u);
                    const bool nWater = (nMat == 1u) || ((nc->flags & 0x08u) != 0u);
                    const float nh = static_cast<float>(nc->topY) + (nWater ? -0.06f : 0.0f);
                    if (h - nh >= minWallDrop)
                    {
                        bot = std::max(nh - 0.5f, h - 160.0f);
                        drawSide = true;
                    }
                }
                if (!drawSide)
                {
                    continue;
                }

                // 悬崖使用次表面/石质色并柔和明暗，防止背光变成黑墙
                const uint8_t sr = static_cast<uint8_t>(std::lround(c->subR * 0.94f));
                const uint8_t sg = static_cast<uint8_t>(std::lround(c->subG * 0.94f));
                const uint8_t sb = static_cast<uint8_t>(std::lround(c->subB * 0.94f));
                const uint8_t aoByte = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(c->skyLight) * 235 / 255, 165, 245));
                const uint32_t sideColorAo = packRgba8(sr, sg, sb, aoByte);
                const uint8_t sideMat = (matId == 4u) ? 4u : 0u;
                const uint32_t sideNormMat = encodeOctNormalAndMat(dir.nx, 0.0f, dir.nz, sideMat, 255u);

                float sx0, sz0, sx1, sz1;
                if (dir.dx < 0)      { sx0 = x0; sz0 = z1; sx1 = x0; sz1 = z0; }
                else if (dir.dx > 0) { sx0 = x1; sz0 = z0; sx1 = x1; sz1 = z1; }
                else if (dir.dz < 0) { sx0 = x0; sz0 = z0; sx1 = x1; sz1 = z0; }
                else                 { sx0 = x1; sz0 = z1; sx1 = x0; sz1 = z1; }

                pushQuad(
                    result.vertices,
                    sx0, h,   sz0,
                    sx0, bot, sz0,
                    sx1, bot, sz1,
                    sx1, h,   sz1,
                    sideColorAo,
                    sideNormMat);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// 磁盘区域文件 (.orlod) 读写
// ---------------------------------------------------------------------------
static std::string makeRegionFilePath(const std::string& cacheDir, int32_t rx, int32_t rz)
{
    if (cacheDir.empty())
    {
        return {};
    }
    std::string p = cacheDir;
    if (p.back() != '/' && p.back() != '\\')
    {
        p.push_back('/');
    }
    p += "r." + std::to_string(rx) + "." + std::to_string(rz) + ".orlod";
    return p;
}

static void saveRegionFile(
    const std::string& filePath,
    const std::array<uint8_t, 256>& present,
    const std::vector<std::array<OrionLodColumn, ORION_LOD_TILE_CELLS>>& chunkCols)
{
    if (filePath.empty())
    {
        return;
    }
    try
    {
        const std::filesystem::path target(filePath);
        std::filesystem::create_directories(target.parent_path());
        const std::string tmpPath = filePath + ".tmp";
        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                return;
            }
            const uint32_t header[2] = {ORION_LOD_REGION_MAGIC, ORION_LOD_REGION_VERSION};
            out.write(reinterpret_cast<const char*>(header), sizeof(header));
            out.write(reinterpret_cast<const char*>(present.data()), present.size());
            for (int i = 0; i < 256; ++i)
            {
                if (present[static_cast<size_t>(i)] != 0u)
                {
                    out.write(reinterpret_cast<const char*>(chunkCols[static_cast<size_t>(i)].data()),
                              ORION_LOD_PAYLOAD_BYTES);
                }
            }
            out.flush();
        }
        std::error_code ec;
        std::filesystem::rename(tmpPath, target, ec);
        if (ec)
        {
            std::filesystem::remove(target, ec);
            std::filesystem::rename(tmpPath, target, ec);
        }
    }
    catch (...)
    {
    }
}

static bool loadRegionFile(
    const std::string& filePath,
    std::array<uint8_t, 256>& outPresent,
    std::vector<std::array<OrionLodColumn, ORION_LOD_TILE_CELLS>>& outChunkCols)
{
    outPresent.fill(0u);
    if (outChunkCols.size() < 256u)
    {
        outChunkCols.resize(256u);
    }
    if (filePath.empty())
    {
        return false;
    }
    try
    {
        std::ifstream in(filePath, std::ios::binary);
        if (!in.is_open())
        {
            return false;
        }
        uint32_t header[2]{};
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!in || header[0] != ORION_LOD_REGION_MAGIC || header[1] != ORION_LOD_REGION_VERSION)
        {
            return false;
        }
        in.read(reinterpret_cast<char*>(outPresent.data()), outPresent.size());
        if (!in)
        {
            outPresent.fill(0u);
            return false;
        }
        for (int i = 0; i < 256; ++i)
        {
            if (outPresent[static_cast<size_t>(i)] != 0u)
            {
                in.read(reinterpret_cast<char*>(outChunkCols[static_cast<size_t>(i)].data()),
                        ORION_LOD_PAYLOAD_BYTES);
                if (!in)
                {
                    outPresent.fill(0u);
                    return false;
                }
            }
        }
        return true;
    }
    catch (...)
    {
        outPresent.fill(0u);
        return false;
    }
}

// ---------------------------------------------------------------------------
// 后台工作线程：实机聚合、缓存持久化、异步网格构建
// ---------------------------------------------------------------------------
static void lodWorkerMain()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    uint64_t activeEpoch = 0u;
    std::string activeCacheDir;
    uint64_t touchSeq = 0u;
    std::unordered_map<ChunkKey, RealChunkEntry, ChunkKeyHash> realChunks;
    std::unordered_map<TileKey, CpuTileData, TileKeyHash> cpuTiles;
    std::unordered_set<TileKey, TileKeyHash> dirtyTiles;
    std::unordered_set<ChunkKey, ChunkKeyHash> dirtyRegions;
    std::unordered_set<ChunkKey, ChunkKeyHash> loadedRegions;
    std::vector<std::array<OrionLodColumn, ORION_LOD_TILE_CELLS>> regionScratch(256u);

    auto resetWorkerWorld = [&](uint64_t newEpoch, const std::string& newDir) {
        activeEpoch = newEpoch;
        activeCacheDir = newDir;
        realChunks.clear();
        cpuTiles.clear();
        dirtyTiles.clear();
        dirtyRegions.clear();
        loadedRegions.clear();
        std::lock_guard<std::mutex> ul(g_lodUploadMutex);
        g_lodReadyUploads.clear();
    };

    auto applyRealChunkToTiles = [&](int32_t cx, int32_t cz, const std::array<OrionLodColumn, ORION_LOD_TILE_CELLS>& chunkCols) {
        for (int32_t lod = 0; lod < static_cast<int32_t>(ORION_LOD_MAX_LEVELS); ++lod)
        {
            const int32_t step = lodStepForLevel(lod);
            const int32_t tileBlocks = lodTileBlocksForLevel(lod);
            const int32_t wx0 = cx * 16;
            const int32_t wz0 = cz * 16;
            const int32_t tx = floorDiv(wx0, tileBlocks);
            const int32_t tz = floorDiv(wz0, tileBlocks);
            const TileKey tkey{lod, tx, tz};
            CpuTileData& tile = cpuTiles[tkey];

            // 每个真实区块 16x16 覆盖若干个 step x step 网格（至少 1x1，最大 8x8 对 lod=0 step=2）
            const int32_t cellsPerChunk = std::max(1, 16 / step);
            const int32_t gxBase = floorMod(wx0, tileBlocks) / step;
            const int32_t gzBase = floorMod(wz0, tileBlocks) / step;

            for (int dzCell = 0; dzCell < cellsPerChunk; ++dzCell)
            {
                for (int dxCell = 0; dxCell < cellsPerChunk; ++dxCell)
                {
                    const int gx = gxBase + dxCell;
                    const int gz = gzBase + dzCell;
                    if (gx < 0 || gx >= static_cast<int>(ORION_LOD_TILE_DIM) ||
                        gz < 0 || gz >= static_cast<int>(ORION_LOD_TILE_DIM))
                    {
                        continue;
                    }
                    // 对齐到区块内部该单元中心（若 step > 16 仅贡献自己所在的子块，选取区块中位点采样或平均值）
                    const int lx0 = (step >= 16) ? 0 : (dxCell * step);
                    const int lz0 = (step >= 16) ? 0 : (dzCell * step);
                    const int sampleSpan = std::min(16, step);
                    const int lx = std::clamp(lx0 + sampleSpan / 2, 0, 15);
                    const int lz = std::clamp(lz0 + sampleSpan / 2, 0, 15);

                    OrionLodColumn chosen = chunkCols[static_cast<size_t>(lz) * 16u + static_cast<size_t>(lx)];
                    if (!colValid(chosen))
                    {
                        // 回退搜寻该窗口内首个有效列
                        for (int sz = lz0; sz < lz0 + sampleSpan && !colValid(chosen); ++sz)
                        {
                            for (int sx = lx0; sx < lx0 + sampleSpan; ++sx)
                            {
                                const OrionLodColumn& cand = chunkCols[static_cast<size_t>(sz) * 16u + static_cast<size_t>(sx)];
                                if (colValid(cand))
                                {
                                    chosen = cand;
                                    break;
                                }
                            }
                        }
                    }

                    if (colValid(chosen))
                    {
                        const size_t tIdx = static_cast<size_t>(gz) * ORION_LOD_TILE_DIM + static_cast<size_t>(gx);
                        tile.realCols[tIdx] = chosen;
                        tile.hasReal[tIdx] = 1u;
                    }
                }
            }
            tile.dirtyMesh = true;
            dirtyTiles.insert(tkey);
        }
    };

    auto evictExcessRealChunks = [&]() {
        if (realChunks.size() <= ORION_LOD_REAL_CHUNK_CACHE_MAX)
        {
            return;
        }
        std::vector<std::pair<uint64_t, ChunkKey>> order;
        order.reserve(realChunks.size());
        for (const auto& kv : realChunks)
        {
            if (!kv.second.dirtyDisk)
            {
                order.push_back({kv.second.lastTouchTick, kv.first});
            }
        }
        std::sort(order.begin(), order.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        const size_t targetDrop = realChunks.size() - ORION_LOD_REAL_CHUNK_CACHE_MAX + 512u;
        for (size_t i = 0; i < std::min(order.size(), targetDrop); ++i)
        {
            realChunks.erase(order[i].second);
        }
    };

    auto flushOneDirtyRegion = [&]() {
        if (dirtyRegions.empty() || activeCacheDir.empty())
        {
            dirtyRegions.clear();
            return;
        }
        const ChunkKey rkey = *dirtyRegions.begin();
        dirtyRegions.erase(dirtyRegions.begin());

        std::array<uint8_t, 256> present{};
        const std::string path = makeRegionFilePath(activeCacheDir, rkey.cx, rkey.cz);
        loadRegionFile(path, present, regionScratch);

        const int32_t baseCx = rkey.cx * 16;
        const int32_t baseCz = rkey.cz * 16;
        bool anyWritten = false;
        for (int lz = 0; lz < 16; ++lz)
        {
            for (int lx = 0; lx < 16; ++lx)
            {
                auto it = realChunks.find(ChunkKey{baseCx + lx, baseCz + lz});
                if (it != realChunks.end())
                {
                    const size_t idx = static_cast<size_t>(lz) * 16u + static_cast<size_t>(lx);
                    present[idx] = 1u;
                    regionScratch[idx] = it->second.cols;
                    it->second.dirtyDisk = false;
                    anyWritten = true;
                }
            }
        }
        if (anyWritten)
        {
            saveRegionFile(path, present, regionScratch);
        }
        loadedRegions.insert(rkey);
    };

    auto tryLoadNearbyCachedRegion = [&]() {
        if (activeCacheDir.empty() || loadedRegions.size() > 64u)
        {
            return;
        }
        const int32_t camCx = floorDiv(static_cast<int32_t>(std::floor(g_lodCamX.load())), 16);
        const int32_t camCz = floorDiv(static_cast<int32_t>(std::floor(g_lodCamZ.load())), 16);
        const int32_t camRx = floorDiv(camCx, 16);
        const int32_t camRz = floorDiv(camCz, 16);
        const int32_t rRadius = std::clamp((g_lodMaxDistanceChunks.load() + 15) / 16, 1, 8);

        for (int dz = -rRadius; dz <= rRadius; ++dz)
        {
            for (int dx = -rRadius; dx <= rRadius; ++dx)
            {
                const ChunkKey rk{camRx + dx, camRz + dz};
                if (loadedRegions.count(rk))
                {
                    continue;
                }
                loadedRegions.insert(rk);
                const std::string path = makeRegionFilePath(activeCacheDir, rk.cx, rk.cz);
                if (!std::filesystem::exists(path))
                {
                    continue;
                }
                std::array<uint8_t, 256> present{};
                if (loadRegionFile(path, present, regionScratch))
                {
                    const int32_t baseCx = rk.cx * 16;
                    const int32_t baseCz = rk.cz * 16;
                    for (int lz = 0; lz < 16; ++lz)
                    {
                        for (int lx = 0; lx < 16; ++lx)
                        {
                            const size_t idx = static_cast<size_t>(lz) * 16u + static_cast<size_t>(lx);
                            if (present[idx] == 0u)
                            {
                                continue;
                            }
                            const ChunkKey ck{baseCx + lx, baseCz + lz};
                            if (realChunks.find(ck) == realChunks.end())
                            {
                                RealChunkEntry entry{};
                                entry.cols = regionScratch[idx];
                                entry.lastTouchTick = ++touchSeq;
                                entry.dirtyDisk = false;
                                realChunks.emplace(ck, entry);
                            }
                            applyRealChunkToTiles(ck.cx, ck.cz, regionScratch[idx]);
                        }
                    }
                }
                return; // 每次循环最多读 1 个 region 文件，保持轻量响应
            }
        }
    };

    auto lastFlushTime = std::chrono::steady_clock::now();

    while (true)
    {
        std::vector<PendingInputItem> batch;
        uint64_t targetEpoch = g_lodEpoch.load();
        std::string targetCacheDir;

        {
            std::unique_lock<std::mutex> ql(g_lodQueueMutex);
            if (!g_lodWorkerStop && g_lodInputQueue.empty() && dirtyTiles.empty())
            {
                g_lodQueueCv.wait_for(ql, std::chrono::milliseconds(80));
            }
            if (g_lodWorkerStop)
            {
                break;
            }
            targetCacheDir = g_lodCacheDir;
            targetEpoch = g_lodEpoch.load();
            const size_t takeCount = std::min<size_t>(g_lodInputQueue.size(), 64u);
            for (size_t i = 0; i < takeCount; ++i)
            {
                batch.push_back(std::move(g_lodInputQueue.front()));
                g_lodInputQueue.pop_front();
            }
        }

        if (targetEpoch != activeEpoch || targetCacheDir != activeCacheDir)
        {
            // 先把旧世界的脏 region 写回磁盘再重置
            while (!dirtyRegions.empty() && !activeCacheDir.empty())
            {
                flushOneDirtyRegion();
            }
            resetWorkerWorld(targetEpoch, targetCacheDir);
        }

        if (!g_lodEnabled.load())
        {
            continue;
        }

        for (const auto& item : batch)
        {
            if (item.epoch != activeEpoch)
            {
                continue;
            }
            if (item.isRealChunk)
            {
                const ChunkKey ck{item.x, item.z};
                RealChunkEntry& rc = realChunks[ck];
                rc.cols = item.cols;
                rc.lastTouchTick = ++touchSeq;
                rc.dirtyDisk = true;
                dirtyRegions.insert(ChunkKey{floorDiv(item.x, 16), floorDiv(item.z, 16)});
                applyRealChunkToTiles(item.x, item.z, item.cols);
            }
            else
            {
                const TileKey tkey{item.lod, item.x, item.z};
                CpuTileData& tile = cpuTiles[tkey];
                if (item.isRealFlag)
                {
                    tile.realCols = item.cols;
                    tile.hasReal.fill(1u);
                }
                else
                {
                    tile.genCols = item.cols;
                    tile.hasGen.fill(1u);
                }
                tile.dirtyMesh = true;
                dirtyTiles.insert(tkey);
            }
        }

        if (batch.empty() && !dirtyTiles.empty())
        {
            // 空闲时顺便按需从磁盘加载 1 个历史探索区域
            tryLoadNearbyCachedRegion();
        }
        else if (batch.empty() && dirtyTiles.empty())
        {
            tryLoadNearbyCachedRegion();
        }

        // 每批次构建最多 16 个最接近相机的脏瓦片，避免占满上传缓冲
        if (!dirtyTiles.empty())
        {
            const double cxWorld = g_lodCamX.load();
            const double czWorld = g_lodCamZ.load();

            std::vector<std::pair<double, TileKey>> sorted;
            sorted.reserve(dirtyTiles.size());
            for (const TileKey& tk : dirtyTiles)
            {
                const double size = static_cast<double>(lodTileBlocksForLevel(tk.lod));
                const double centerX = (static_cast<double>(tk.tx) + 0.5) * size;
                const double centerZ = (static_cast<double>(tk.tz) + 0.5) * size;
                const double dx = centerX - cxWorld;
                const double dz = centerZ - czWorld;
                sorted.push_back({dx * dx + dz * dz, tk});
            }
            std::sort(sorted.begin(), sorted.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            std::vector<PendingUploadTile> built;
            const size_t buildLimit = std::min<size_t>(sorted.size(), 16u);
            built.reserve(buildLimit);
            for (size_t i = 0; i < buildLimit; ++i)
            {
                const TileKey tk = sorted[i].second;
                dirtyTiles.erase(tk);
                auto it = cpuTiles.find(tk);
                if (it == cpuTiles.end())
                {
                    continue;
                }
                it->second.dirtyMesh = false;
                it->second.generation += 1u;
                PendingUploadTile mesh = buildTileMesh(tk, it->second, it->second.generation);
                if (!mesh.vertices.empty())
                {
                    built.push_back(std::move(mesh));
                }
            }

            if (!built.empty() && targetEpoch == g_lodEpoch.load())
            {
                std::lock_guard<std::mutex> ul(g_lodUploadMutex);
                for (auto& m : built)
                {
                    g_lodReadyUploads[m.key] = std::move(m);
                }
            }
        }

        evictExcessRealChunks();

        const auto now = std::chrono::steady_clock::now();
        if (!dirtyRegions.empty() && (now - lastFlushTime) >= std::chrono::milliseconds(1500))
        {
            flushOneDirtyRegion();
            lastFlushTime = now;
        }
    }

    while (!dirtyRegions.empty() && !activeCacheDir.empty())
    {
        flushOneDirtyRegion();
    }
}

static void ensureLodWorkerStarted()
{
    if (!g_lodWorkerRunning)
    {
        g_lodWorkerStop = false;
        g_lodWorkerThread = std::thread(lodWorkerMain);
        g_lodWorkerRunning = true;
    }
}

// ---------------------------------------------------------------------------
// Vulkan 图像/管线创建与销毁（绝不在游玩移动中调用 WaitIdle）
// ---------------------------------------------------------------------------
static void createLodImage2D(
    uint32_t width, uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImageAspectFlags aspect,
    VkImage& outImage,
    VkDeviceMemory& outMemory,
    VkImageView& outView)
{
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {width, height, 1u};
    ii.mipLevels = 1u;
    ii.arrayLayers = 1u;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(g_device, &ii, nullptr, &outImage), "orion lod create image failed");

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(g_device, outImage, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(g_physicalDevice, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &ai, nullptr, &outMemory), "orion lod alloc image memory failed");
    check(vkBindImageMemory(g_device, outImage, outMemory, 0u), "orion lod bind image memory failed");

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = outImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1u;
    vi.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &vi, nullptr, &outView), "orion lod create image view failed");
}

static void destroyLodImage2D(VkImage& img, VkDeviceMemory& mem, VkImageView& view)
{
    if (view != VK_NULL_HANDLE) { vkDestroyImageView(g_device, view, nullptr); view = VK_NULL_HANDLE; }
    if (img != VK_NULL_HANDLE)  { vkDestroyImage(g_device, img, nullptr);      img = VK_NULL_HANDLE; }
    if (mem != VK_NULL_HANDLE)  { vkFreeMemory(g_device, mem, nullptr);        mem = VK_NULL_HANDLE; }
}

static void transitionDummyImagesToGeneral()
{
    if (g_orionLodDummyRgba8Image == VK_NULL_HANDLE || g_orionLodDummyR32fImage == VK_NULL_HANDLE)
    {
        return;
    }
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_rtCommandPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1u;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &cai, &cmd), "orion lod dummy cmd alloc failed");

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bi), "orion lod dummy cmd begin failed");

    VkImageMemoryBarrier barriers[2]{};
    for (int i = 0; i < 2; ++i)
    {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    barriers[0].image = g_orionLodDummyRgba8Image;
    barriers[1].image = g_orionLodDummyR32fImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 2u, barriers);

    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    vkCmdClearColorImage(cmd, g_orionLodDummyRgba8Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1u, &range);
    vkCmdClearColorImage(cmd, g_orionLodDummyR32fImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1u, &range);

    for (int i = 0; i < 2; ++i)
    {
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 2u, barriers);

    check(vkEndCommandBuffer(cmd), "orion lod dummy cmd end failed");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1u;
    si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(g_queue, 1u, &si, VK_NULL_HANDLE), "orion lod dummy submit failed");
    check(vkQueueWaitIdle(g_queue), "orion lod dummy wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &cmd);
}

static void destroyOrionLodRenderTargets()
{
    if (g_orionLodFramebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(g_device, g_orionLodFramebuffer, nullptr);
        g_orionLodFramebuffer = VK_NULL_HANDLE;
    }
    destroyLodImage2D(g_orionLodColorImage, g_orionLodColorMemory, g_orionLodColorView);
    destroyLodImage2D(g_orionLodNormalMatImage, g_orionLodNormalMatMemory, g_orionLodNormalMatView);
    destroyLodImage2D(g_orionLodDepthOutImage, g_orionLodDepthOutMemory, g_orionLodDepthOutView);
    destroyLodImage2D(g_orionLodHwDepthImage, g_orionLodHwDepthMemory, g_orionLodHwDepthView);
    g_orionLodWidth = 0u;
    g_orionLodHeight = 0u;
    g_orionLodBoundRtSet = VK_NULL_HANDLE;
}

static void ensureOrionLodRenderPassAndPipeline()
{
    if (g_orionLodPipeline != VK_NULL_HANDLE)
    {
        return;
    }

    VkAttachmentDescription attachments[4]{};
    // Color0: RGBA8 albedo + skyAO
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    // Color1: RGBA8 OctNormal + matClass + aux
    attachments[1] = attachments[0];

    // Color2: R32F linear distance (0 = sky)
    attachments[2] = attachments[0];
    attachments[2].format = VK_FORMAT_R32_SFLOAT;

    // Depth: D32_SFLOAT Reverse-Z
    attachments[3].format = VK_FORMAT_D32_SFLOAT;
    attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[3].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRefs[3]{
        {0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {1u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        {2u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}
    };
    VkAttachmentReference depthRef{3u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 3u;
    subpass.pColorAttachments = colorRefs;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0u;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    deps[1].srcSubpass = 0u;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpInfo.attachmentCount = 4u;
    rpInfo.pAttachments = attachments;
    rpInfo.subpassCount = 1u;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 2u;
    rpInfo.pDependencies = deps;
    check(vkCreateRenderPass(g_device, &rpInfo, nullptr, &g_orionLodRenderPass),
        "orion lod render pass failed");

    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0u;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1u;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo dslInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslInfo.bindingCount = 1u;
    dslInfo.pBindings = &uboBinding;
    check(vkCreateDescriptorSetLayout(g_device, &dslInfo, nullptr, &g_orionLodSetLayout),
        "orion lod set layout failed");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u};
    VkDescriptorPoolCreateInfo dpInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpInfo.maxSets = 1u;
    dpInfo.poolSizeCount = 1u;
    dpInfo.pPoolSizes = &poolSize;
    check(vkCreateDescriptorPool(g_device, &dpInfo, nullptr, &g_orionLodDescriptorPool),
        "orion lod desc pool failed");

    VkDescriptorSetAllocateInfo dsAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsAlloc.descriptorPool = g_orionLodDescriptorPool;
    dsAlloc.descriptorSetCount = 1u;
    dsAlloc.pSetLayouts = &g_orionLodSetLayout;
    check(vkAllocateDescriptorSets(g_device, &dsAlloc, &g_orionLodDescriptorSet),
        "orion lod desc set alloc failed");

    VkDescriptorBufferInfo camInfo{};
    camInfo.buffer = g_cameraUboBuffer;
    camInfo.offset = 0u;
    camInfo.range = sizeof(CameraUBO);
    VkWriteDescriptorSet camWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    camWrite.dstSet = g_orionLodDescriptorSet;
    camWrite.dstBinding = 0u;
    camWrite.descriptorCount = 1u;
    camWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    camWrite.pBufferInfo = &camInfo;
    vkUpdateDescriptorSets(g_device, 1u, &camWrite, 0u, nullptr);

    VkPushConstantRange pcRange{};
    pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pcRange.offset = 0u;
    pcRange.size = sizeof(OrionLodPushConstants);

    VkPipelineLayoutCreateInfo plInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plInfo.setLayoutCount = 1u;
    plInfo.pSetLayouts = &g_orionLodSetLayout;
    plInfo.pushConstantRangeCount = 1u;
    plInfo.pPushConstantRanges = &pcRange;
    check(vkCreatePipelineLayout(g_device, &plInfo, nullptr, &g_orionLodPipelineLayout),
        "orion lod pipeline layout failed");

    const std::vector<uint8_t> vertSpv = loadSpv("shaders/orion_lod.vert.spv");
    const std::vector<uint8_t> fragSpv = loadSpv("shaders/orion_lod.frag.spv");
    VkShaderModule vertMod = createShaderModule(g_device, vertSpv);
    VkShaderModule fragMod = createShaderModule(g_device, fragSpv);

    try
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertMod;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragMod;
        stages[1].pName = "main";

        VkVertexInputBindingDescription vBind{};
        vBind.binding = 0u;
        vBind.stride = sizeof(OrionLodVertex);
        vBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription vAttrs[3]{};
        vAttrs[0] = {0u, 0u, VK_FORMAT_R32G32B32_SFLOAT, 0u};
        vAttrs[1] = {1u, 0u, VK_FORMAT_R32_UINT, offsetof(OrionLodVertex, packedColorAo)};
        vAttrs[2] = {2u, 0u, VK_FORMAT_R32_UINT, offsetof(OrionLodVertex, packedNormalMat)};

        VkPipelineVertexInputStateCreateInfo viState{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        viState.vertexBindingDescriptionCount = 1u;
        viState.pVertexBindingDescriptions = &vBind;
        viState.vertexAttributeDescriptionCount = 3u;
        viState.pVertexAttributeDescriptions = vAttrs;

        VkPipelineInputAssemblyStateCreateInfo iaState{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vpState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vpState.viewportCount = 1u;
        vpState.scissorCount = 1u;

        VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynState.dynamicStateCount = 2u;
        dynState.pDynamicStates = dynStates;

        VkPipelineRasterizationStateCreateInfo rsState{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rsState.polygonMode = VK_POLYGON_MODE_FILL;
        rsState.cullMode = VK_CULL_MODE_NONE;
        rsState.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rsState.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo msState{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        msState.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo dsState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dsState.depthTestEnable = VK_TRUE;
        dsState.depthWriteEnable = VK_TRUE;
        dsState.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendAttachmentState cbAtts[3]{};
        for (int i = 0; i < 3; ++i)
        {
            cbAtts[i].blendEnable = VK_FALSE;
            cbAtts[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                       VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT |
                                       VK_COLOR_COMPONENT_A_BIT;
        }
        VkPipelineColorBlendStateCreateInfo cbState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbState.attachmentCount = 3u;
        cbState.pAttachments = cbAtts;

        VkGraphicsPipelineCreateInfo gpInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpInfo.stageCount = 2u;
        gpInfo.pStages = stages;
        gpInfo.pVertexInputState = &viState;
        gpInfo.pInputAssemblyState = &iaState;
        gpInfo.pViewportState = &vpState;
        gpInfo.pRasterizationState = &rsState;
        gpInfo.pMultisampleState = &msState;
        gpInfo.pDepthStencilState = &dsState;
        gpInfo.pColorBlendState = &cbState;
        gpInfo.pDynamicState = &dynState;
        gpInfo.layout = g_orionLodPipelineLayout;
        gpInfo.renderPass = g_orionLodRenderPass;
        gpInfo.subpass = 0u;

        check(vkCreateGraphicsPipelines(g_device, getSharedPipelineCache(), 1u, &gpInfo, nullptr, &g_orionLodPipeline),
            "orion lod graphics pipeline failed");
    }
    catch (...)
    {
        vkDestroyShaderModule(g_device, vertMod, nullptr);
        vkDestroyShaderModule(g_device, fragMod, nullptr);
        throw;
    }

    vkDestroyShaderModule(g_device, vertMod, nullptr);
    vkDestroyShaderModule(g_device, fragMod, nullptr);
}

static void ensureOrionLodSlotPoolAndStaging()
{
    if (g_orionLodPoolBuffer != VK_NULL_HANDLE)
    {
        return;
    }

    createBuffer(
        g_device, g_physicalDevice, ORION_LOD_POOL_BYTES,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_orionLodPoolBuffer, g_orionLodPoolMemory);

    for (uint32_t i = 0u; i < ORION_LOD_STAGING_FRAMES; ++i)
    {
        createBuffer(
            g_device, g_physicalDevice, ORION_LOD_FRAME_UPLOAD_CAP,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            g_orionLodStagingBuffer[i], g_orionLodStagingMemory[i], true);
        check(vkMapMemory(g_device, g_orionLodStagingMemory[i], 0u,
                ORION_LOD_FRAME_UPLOAD_CAP, 0u, &g_orionLodStagingMapped[i]),
            "orion lod staging map failed");
    }

    g_freeSlots.clear();
    g_freeSlots.reserve(ORION_LOD_MAX_SLOTS);
    for (uint32_t i = ORION_LOD_MAX_SLOTS; i > 0u; --i)
    {
        g_freeSlots.push_back(i - 1u);
    }
}

static void updateOrionLodRtDescriptorsIfNeeded(bool useActiveImages)
{
    if (g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }
    if (g_dhRasterLodEnabled || dhTexturesAvailable())
    {
        return;
    }
    if (g_orionLodBoundRtSet == g_rayTracingDescriptorSet &&
        g_orionLodBoundActive == useActiveImages)
    {
        return;
    }

    VkImageView colorView = useActiveImages && g_orionLodColorView != VK_NULL_HANDLE
        ? g_orionLodColorView : g_orionLodDummyRgba8View;
    VkImageView depthView = useActiveImages && g_orionLodDepthOutView != VK_NULL_HANDLE
        ? g_orionLodDepthOutView : g_orionLodDummyR32fView;
    VkImageView normView = useActiveImages && g_orionLodNormalMatView != VK_NULL_HANDLE
        ? g_orionLodNormalMatView : g_orionLodDummyRgba8View;

    if (colorView == VK_NULL_HANDLE || depthView == VK_NULL_HANDLE || normView == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo infos[3]{};
    infos[0].imageView = colorView;
    infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    infos[1].imageView = depthView;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    infos[2].imageView = normView;
    infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    const uint32_t ringCount = g_rtDescriptorRingReady ? RT_DESCRIPTOR_RING_SIZE : 1u;
    for (uint32_t r = 0u; r < ringCount; ++r)
    {
        VkDescriptorSet dstSet = g_rtDescriptorRingReady
            ? g_rtDescriptorRingSets[r]
            : g_rayTracingDescriptorSet;
        if (dstSet == VK_NULL_HANDLE)
        {
            continue;
        }
        VkWriteDescriptorSet writes[3]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dstSet,
            96u, 0u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[0], nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dstSet,
            97u, 0u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[1], nullptr, nullptr};
        writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, dstSet,
            101u, 0u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[2], nullptr, nullptr};
        vkUpdateDescriptorSets(g_device, 3u, writes, 0u, nullptr);
    }

    g_orionLodBoundRtSet = g_rayTracingDescriptorSet;
    g_orionLodBoundActive = useActiveImages;
}

// ---------------------------------------------------------------------------
// 视锥裁剪（基于 CameraUBO.viewProj 提取 6 个平面）
// ---------------------------------------------------------------------------
struct FrustumPlane
{
    float a, b, c, d;
};

static void extractFrustumPlanes(const float m[16], FrustumPlane planes[6])
{
    // GLSL 列主序：m[col*4 + row]，row r = (m[0+r], m[4+r], m[8+r], m[12+r])
    auto row = [&](int r, int col) { return m[col * 4 + r]; };
    planes[0] = {row(3, 0) + row(0, 0), row(3, 1) + row(0, 1), row(3, 2) + row(0, 2), row(3, 3) + row(0, 3)};
    planes[1] = {row(3, 0) - row(0, 0), row(3, 1) - row(0, 1), row(3, 2) - row(0, 2), row(3, 3) - row(0, 3)};
    planes[2] = {row(3, 0) + row(1, 0), row(3, 1) + row(1, 1), row(3, 2) + row(1, 2), row(3, 3) + row(1, 3)};
    planes[3] = {row(3, 0) - row(1, 0), row(3, 1) - row(1, 1), row(3, 2) - row(1, 2), row(3, 3) - row(1, 3)};
    planes[4] = {row(3, 0) + row(2, 0), row(3, 1) + row(2, 1), row(3, 2) + row(2, 2), row(3, 3) + row(2, 3)};
    // 无限远渲染不使用远平面裁剪 planes[5]
    planes[5] = {0.0f, 0.0f, 0.0f, 1.0f};
}

static bool aabbInsideFrustum(
    const FrustumPlane planes[5],
    float minX, float minY, float minZ,
    float maxX, float maxY, float maxZ)
{
    for (int i = 0; i < 5; ++i)
    {
        const FrustumPlane& p = planes[i];
        const float px = (p.a >= 0.0f) ? maxX : minX;
        const float py = (p.b >= 0.0f) ? maxY : minY;
        const float pz = (p.c >= 0.0f) ? maxZ : minZ;
        if (p.a * px + p.b * py + p.c * pz + p.d < 0.0f)
        {
            return false;
        }
    }
    return true;
}

static uint32_t allocateGpuSlotNoStall(uint64_t curFrame, double camX, double camZ)
{
    if (!g_freeSlots.empty())
    {
        const uint32_t slot = g_freeSlots.back();
        g_freeSlots.pop_back();
        return slot;
    }

    // 池满时，挑选距离相机最远且上一帧未画的 Tile 进行平滑 LRU 淘汰
    auto victim = g_gpuTiles.end();
    double worstScore = -1.0;
    for (auto it = g_gpuTiles.begin(); it != g_gpuTiles.end(); ++it)
    {
        const double size = static_cast<double>(lodTileBlocksForLevel(it->first.lod));
        const double cx = (static_cast<double>(it->first.tx) + 0.5) * size;
        const double cz = (static_cast<double>(it->first.tz) + 0.5) * size;
        const double distSq = (cx - camX) * (cx - camX) + (cz - camZ) * (cz - camZ);
        const double ageBonus = static_cast<double>(curFrame - it->second.lastUsedFrame) * 65536.0;
        const double score = distSq + ageBonus;
        if (score > worstScore)
        {
            worstScore = score;
            victim = it;
        }
    }

    if (victim == g_gpuTiles.end())
    {
        return UINT32_MAX;
    }

    const uint32_t reclaimed = victim->second.slotId;
    g_gpuTiles.erase(victim);
    // 立即返回刚释放的 slot？为绝不踩到同一帧已绘制的 slot，放进延迟队列！
    g_retiredSlots.push_back({curFrame + ORION_LOD_SLOT_RETIRE_FRAMES, reclaimed});
    return UINT32_MAX;
}
} // namespace

// ---------------------------------------------------------------------------
// 公共后端接口（由 Frame.cpp / Cleanup.cpp / Streamline.cpp 调用）
// ---------------------------------------------------------------------------
bool orionBuiltinLodActive()
{
    return g_lodEnabled.load() &&
           !g_dhRasterLodEnabled &&
           !dhTexturesAvailable() &&
           g_orionLodPipeline != VK_NULL_HANDLE;
}

void createOrionLodResources(uint32_t renderWidth, uint32_t renderHeight)
{
    if (g_device == VK_NULL_HANDLE || g_physicalDevice == VK_NULL_HANDLE)
    {
        return;
    }

    if (g_orionLodDummyRgba8Image == VK_NULL_HANDLE)
    {
        createLodImage2D(1u, 1u, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            g_orionLodDummyRgba8Image, g_orionLodDummyRgba8Memory, g_orionLodDummyRgba8View);
        createLodImage2D(1u, 1u, VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            g_orionLodDummyR32fImage, g_orionLodDummyR32fMemory, g_orionLodDummyR32fView);
        transitionDummyImagesToGeneral();
    }

    const bool wantFullTargets = g_lodEnabled.load() && !g_dhRasterLodEnabled && !dhTexturesAvailable();
    if (!wantFullTargets)
    {
        updateOrionLodRtDescriptorsIfNeeded(false);
        return;
    }

    const uint32_t w = std::max(renderWidth, 1u);
    const uint32_t h = std::max(renderHeight, 1u);

    ensureOrionLodRenderPassAndPipeline();
    ensureOrionLodSlotPoolAndStaging();

    if (g_orionLodWidth != w || g_orionLodHeight != h || g_orionLodFramebuffer == VK_NULL_HANDLE)
    {
        if (g_orionLodFramebuffer != VK_NULL_HANDLE)
        {
            FGDeviceWaitIdle(g_device);
            destroyOrionLodRenderTargets();
        }
        createLodImage2D(w, h, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            g_orionLodColorImage, g_orionLodColorMemory, g_orionLodColorView);
        createLodImage2D(w, h, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            g_orionLodNormalMatImage, g_orionLodNormalMatMemory, g_orionLodNormalMatView);
        createLodImage2D(w, h, VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            g_orionLodDepthOutImage, g_orionLodDepthOutMemory, g_orionLodDepthOutView);
        createLodImage2D(w, h, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            g_orionLodHwDepthImage, g_orionLodHwDepthMemory, g_orionLodHwDepthView);

        VkImageView fbViews[4] = {
            g_orionLodColorView,
            g_orionLodNormalMatView,
            g_orionLodDepthOutView,
            g_orionLodHwDepthView
        };
        VkFramebufferCreateInfo fbInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.renderPass = g_orionLodRenderPass;
        fbInfo.attachmentCount = 4u;
        fbInfo.pAttachments = fbViews;
        fbInfo.width = w;
        fbInfo.height = h;
        fbInfo.layers = 1u;
        check(vkCreateFramebuffer(g_device, &fbInfo, nullptr, &g_orionLodFramebuffer),
            "orion lod framebuffer failed");

        g_orionLodWidth = w;
        g_orionLodHeight = h;
        g_orionLodBoundRtSet = VK_NULL_HANDLE;

        RTLog("[ORION-LOD] 硬件光栅 G-Buffer 就绪 " + std::to_string(w) + "x" + std::to_string(h) +
              " (池=" + std::to_string(ORION_LOD_MAX_SLOTS) + " slots / " +
              std::to_string(ORION_LOD_POOL_BYTES / (1024ull * 1024ull)) + "MB)");
    }

    updateOrionLodRtDescriptorsIfNeeded(true);
}

void recordOrionLodPass(VkCommandBuffer cmd)
{
    if (cmd == VK_NULL_HANDLE || !orionBuiltinLodActive() ||
        g_orionLodFramebuffer == VK_NULL_HANDLE ||
        g_orionLodPoolBuffer == VK_NULL_HANDLE ||
        g_orionLodWidth == 0u || g_orionLodHeight == 0u)
    {
        return;
    }

    updateOrionLodRtDescriptorsIfNeeded(true);

    ++g_lodRenderFrame;
    const uint64_t curEpoch = g_lodEpoch.load();
    if (g_renderedEpoch != curEpoch)
    {
        for (const auto& kv : g_gpuTiles)
        {
            g_retiredSlots.push_back({g_lodRenderFrame + ORION_LOD_SLOT_RETIRE_FRAMES, kv.second.slotId});
        }
        g_gpuTiles.clear();
        g_renderedEpoch = curEpoch;
    }

    // 回收已安全度过 4 帧飞行期的旧 Slot
    while (!g_retiredSlots.empty() && g_retiredSlots.front().retireAfterFrame <= g_lodRenderFrame)
    {
        const uint32_t s = g_retiredSlots.front().slotId;
        g_retiredSlots.pop_front();
        if (s < ORION_LOD_MAX_SLOTS)
        {
            g_freeSlots.push_back(s);
        }
    }

    const double camX = g_lodCamX.load();
    const double camY = g_lodCamY.load();
    const double camZ = g_lodCamZ.load();
    const int32_t maxDistChunks = g_lodMaxDistanceChunks.load();
    const int32_t vanillaRd = g_lodVanillaRd.load();

    // 1. 非阻塞尝试搬运本帧上限内的就绪 Tile（绝不阻塞渲染线程）
    const uint32_t ringIdx = static_cast<uint32_t>(g_lodRenderFrame % ORION_LOD_STAGING_FRAMES);
    uint8_t* stagingPtr = static_cast<uint8_t*>(g_orionLodStagingMapped[ringIdx]);
    VkBuffer stagingBuf = g_orionLodStagingBuffer[ringIdx];
    VkDeviceSize stagingUsed = 0u;
    std::vector<PendingUploadTile> toUpload;

    {
        std::unique_lock<std::mutex> lock(g_lodUploadMutex, std::try_to_lock);
        if (lock.owns_lock() && !g_lodReadyUploads.empty())
        {
            std::vector<std::pair<double, TileKey>> ranked;
            ranked.reserve(g_lodReadyUploads.size());
            for (const auto& kv : g_lodReadyUploads)
            {
                const double size = static_cast<double>(lodTileBlocksForLevel(kv.first.lod));
                const double txCenter = (static_cast<double>(kv.first.tx) + 0.5) * size;
                const double tzCenter = (static_cast<double>(kv.first.tz) + 0.5) * size;
                const double dx = txCenter - camX;
                const double dz = tzCenter - camZ;
                ranked.push_back({dx * dx + dz * dz, kv.first});
            }
            std::sort(ranked.begin(), ranked.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            for (size_t i = 0; i < ranked.size() && toUpload.size() < ORION_LOD_MAX_TILES_PER_FRAME; ++i)
            {
                auto it = g_lodReadyUploads.find(ranked[i].second);
                if (it == g_lodReadyUploads.end())
                {
                    continue;
                }
                const VkDeviceSize bytes = static_cast<VkDeviceSize>(it->second.vertices.size()) * sizeof(OrionLodVertex);
                if (bytes == 0u || bytes > ORION_LOD_SLOT_BYTES)
                {
                    g_lodReadyUploads.erase(it);
                    continue;
                }
                if (stagingUsed + bytes > ORION_LOD_FRAME_UPLOAD_CAP)
                {
                    break;
                }
                toUpload.push_back(std::move(it->second));
                g_lodReadyUploads.erase(it);
                stagingUsed += bytes;
            }
        }
    }

    bool copiedAny = false;
    VkDeviceSize copyOffset = 0u;
    std::vector<VkBufferCopy> copies;
    copies.reserve(toUpload.size());
    for (const auto& tile : toUpload)
    {
        // 旧 slot 推迟 4 帧释放，新数据写进新 slot，保证 zero hazard / zero spike
        auto oldIt = g_gpuTiles.find(tile.key);
        if (oldIt != g_gpuTiles.end())
        {
            g_retiredSlots.push_back({g_lodRenderFrame + ORION_LOD_SLOT_RETIRE_FRAMES, oldIt->second.slotId});
            g_gpuTiles.erase(oldIt);
        }
        const uint32_t newSlot = allocateGpuSlotNoStall(g_lodRenderFrame, camX, camZ);
        if (newSlot == UINT32_MAX)
        {
            // 若池暂满且需要等待退避回收，把该 Tile 跳过（下次会再申请）
            continue;
        }
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(tile.vertices.size()) * sizeof(OrionLodVertex);
        std::memcpy(stagingPtr + copyOffset, tile.vertices.data(), static_cast<size_t>(bytes));

        VkBufferCopy copy{};
        copy.srcOffset = copyOffset;
        copy.dstOffset = static_cast<VkDeviceSize>(newSlot) * ORION_LOD_SLOT_BYTES;
        copy.size = bytes;
        copies.push_back(copy);
        copyOffset += bytes;

        GpuTileResident res{};
        res.key = tile.key;
        res.slotId = newSlot;
        res.vertexCount = static_cast<uint32_t>(tile.vertices.size());
        res.minY = tile.minY;
        res.maxY = tile.maxY;
        res.lastUsedFrame = g_lodRenderFrame;
        g_gpuTiles[tile.key] = res;
        copiedAny = true;
    }

    if (copiedAny && !copies.empty())
    {
        vkCmdCopyBuffer(cmd, stagingBuf, g_orionLodPoolBuffer,
            static_cast<uint32_t>(copies.size()), copies.data());

        VkBufferMemoryBarrier bufBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bufBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bufBarrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        bufBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bufBarrier.buffer = g_orionLodPoolBuffer;
        bufBarrier.offset = 0u;
        bufBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
            0u, 0u, nullptr, 1u, &bufBarrier, 0u, nullptr);
    }

    // 2. 视锥剔除 + LOD 父子遮挡判定 + 由近到远排序（最大化 Early-Z）
    FrustumPlane frustum[6]{};
    bool hasFrustum = false;
    if (g_cameraUboMapped != nullptr)
    {
        const CameraUBO* ubo = static_cast<const CameraUBO*>(g_cameraUboMapped);
        extractFrustumPlanes(ubo->viewProj, frustum);
        hasFrustum = true;
    }

    const float outerRadiusBlocks = static_cast<float>(std::max(32, maxDistChunks) * 16);
    const float vanillaBlocks = static_cast<float>(std::max(4, vanillaRd) * 16);
    // 完全被近场包裹的最内半径（留 3 区块兜底重叠，因为 rgen 只在 miss 时才绘 LOD）
    const float innerFullSkipRadius = std::max(0.0f, vanillaBlocks - 48.0f);

    struct DrawItem
    {
        double distSq;
        const GpuTileResident* tile;
    };
    std::vector<DrawItem> drawList;
    drawList.reserve(g_gpuTiles.size());

    auto allChildrenResident = [&](int32_t lod, int32_t tx, int32_t tz) -> bool {
        if (lod <= 0) return false;
        for (int dz = 0; dz < 2; ++dz)
        {
            for (int dx = 0; dx < 2; ++dx)
            {
                if (g_gpuTiles.find(TileKey{lod - 1, tx * 2 + dx, tz * 2 + dz}) == g_gpuTiles.end())
                {
                    return false;
                }
            }
        }
        return true;
    };

    for (auto& kv : g_gpuTiles)
    {
        GpuTileResident& res = kv.second;
        if (res.vertexCount == 0u || res.slotId >= ORION_LOD_MAX_SLOTS)
        {
            continue;
        }
        const int32_t tileBlocks = lodTileBlocksForLevel(res.key.lod);
        const float minX = static_cast<float>(res.key.tx) * static_cast<float>(tileBlocks);
        const float minZ = static_cast<float>(res.key.tz) * static_cast<float>(tileBlocks);
        const float maxX = minX + static_cast<float>(tileBlocks);
        const float maxZ = minZ + static_cast<float>(tileBlocks);
        const float minY = static_cast<float>(res.minY);
        const float maxY = static_cast<float>(res.maxY) + 4.0f;

        // 距相机最近点与最远点距离（XZ 平面）
        const float clampedX = std::clamp(static_cast<float>(camX), minX, maxX);
        const float clampedZ = std::clamp(static_cast<float>(camZ), minZ, maxZ);
        const float nearDx = clampedX - static_cast<float>(camX);
        const float nearDz = clampedZ - static_cast<float>(camZ);
        const float nearDistSq = nearDx * nearDx + nearDz * nearDz;

        if (nearDistSq > (outerRadiusBlocks + tileBlocks) * (outerRadiusBlocks + tileBlocks))
        {
            continue;
        }

        const float farDx = std::max(std::abs(minX - static_cast<float>(camX)), std::abs(maxX - static_cast<float>(camX)));
        const float farDz = std::max(std::abs(minZ - static_cast<float>(camZ)), std::abs(maxZ - static_cast<float>(camZ)));
        const float farDistSq = farDx * farDx + farDz * farDz;
        if (farDistSq < innerFullSkipRadius * innerFullSkipRadius)
        {
            continue;
        }

        // 若更精细子层级全部驻留且整体距离落在细层级覆盖圈内，跳过父层，无则父层自动兜底！
        const float childCoverRadius = (res.key.lod == 1)
            ? std::max(260.0f, vanillaBlocks + 160.0f)
            : (res.key.lod == 2 ? std::max(640.0f, outerRadiusBlocks * 0.45f)
                                : std::max(1152.0f, outerRadiusBlocks * 0.72f));
        if (res.key.lod > 0 && farDistSq < childCoverRadius * childCoverRadius &&
            allChildrenResident(res.key.lod, res.key.tx, res.key.tz))
        {
            continue;
        }

        if (hasFrustum && !aabbInsideFrustum(frustum, minX, minY, minZ, maxX, maxY, maxZ))
        {
            continue;
        }

        const double cx = 0.5 * (minX + maxX) - camX;
        const double cy = 0.5 * (minY + maxY) - camY;
        const double cz = 0.5 * (minZ + maxZ) - camZ;
        res.lastUsedFrame = g_lodRenderFrame;
        drawList.push_back({cx * cx + cy * cy + cz * cz, &res});
    }

    std::sort(drawList.begin(), drawList.end(),
        [](const DrawItem& a, const DrawItem& b) { return a.distSq < b.distSq; });

    // 3. 开始硬件光栅 RenderPass（哪怕 drawList 为空也 clear 一次确保无残留远场像素）
    VkClearValue clears[4]{};
    clears[0].color.float32[0] = 0.0f;
    clears[0].color.float32[1] = 0.0f;
    clears[0].color.float32[2] = 0.0f;
    clears[0].color.float32[3] = 1.0f;
    clears[1].color.float32[0] = 0.5f;
    clears[1].color.float32[1] = 0.5f;
    clears[1].color.float32[2] = 0.0f;
    clears[1].color.float32[3] = 0.0f;
    clears[2].color.float32[0] = 0.0f;
    clears[3].depthStencil.depth = 0.0f; // Reverse-Z far clear

    VkRenderPassBeginInfo rpBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpBegin.renderPass = g_orionLodRenderPass;
    rpBegin.framebuffer = g_orionLodFramebuffer;
    rpBegin.renderArea = {{0, 0}, {g_orionLodWidth, g_orionLodHeight}};
    rpBegin.clearValueCount = 4u;
    rpBegin.pClearValues = clears;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    if (!drawList.empty())
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_orionLodPipeline);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(g_orionLodWidth);
        viewport.height = static_cast<float>(g_orionLodHeight);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0u, 1u, &viewport);

        VkRect2D scissor{{0, 0}, {g_orionLodWidth, g_orionLodHeight}};
        vkCmdSetScissor(cmd, 0u, 1u, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            g_orionLodPipelineLayout, 0u, 1u, &g_orionLodDescriptorSet, 0u, nullptr);

        const VkDeviceSize vbOffset = 0u;
        vkCmdBindVertexBuffers(cmd, 0u, 1u, &g_orionLodPoolBuffer, &vbOffset);

        for (const auto& item : drawList)
        {
            const GpuTileResident* res = item.tile;
            const int32_t tileBlocks = lodTileBlocksForLevel(res->key.lod);
            OrionLodPushConstants pc{};
            pc.tileOriginX = static_cast<float>(res->key.tx * tileBlocks);
            pc.tileOriginY = 0.0f;
            pc.tileOriginZ = static_cast<float>(res->key.tz * tileBlocks);
            pc.tileStep = static_cast<float>(lodStepForLevel(res->key.lod));

            vkCmdPushConstants(cmd, g_orionLodPipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT, 0u, sizeof(pc), &pc);

            const uint32_t firstVertex = res->slotId * ORION_LOD_SLOT_VERTS;
            vkCmdDraw(cmd, res->vertexCount, 1u, firstVertex, 0u);
        }
    }

    vkCmdEndRenderPass(cmd);
}

void destroyOrionLodResources()
{
    if (g_lodWorkerRunning)
    {
        {
            std::lock_guard<std::mutex> ql(g_lodQueueMutex);
            g_lodWorkerStop = true;
        }
        g_lodQueueCv.notify_all();
        if (g_lodWorkerThread.joinable())
        {
            g_lodWorkerThread.join();
        }
        g_lodWorkerRunning = false;
    }

    if (g_device != VK_NULL_HANDLE)
    {
        destroyOrionLodRenderTargets();
        destroyLodImage2D(g_orionLodDummyRgba8Image, g_orionLodDummyRgba8Memory, g_orionLodDummyRgba8View);
        destroyLodImage2D(g_orionLodDummyR32fImage, g_orionLodDummyR32fMemory, g_orionLodDummyR32fView);

        if (g_orionLodPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(g_device, g_orionLodPipeline, nullptr);
            g_orionLodPipeline = VK_NULL_HANDLE;
        }
        if (g_orionLodPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(g_device, g_orionLodPipelineLayout, nullptr);
            g_orionLodPipelineLayout = VK_NULL_HANDLE;
        }
        if (g_orionLodDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(g_device, g_orionLodDescriptorPool, nullptr);
            g_orionLodDescriptorPool = VK_NULL_HANDLE;
            g_orionLodDescriptorSet = VK_NULL_HANDLE;
        }
        if (g_orionLodSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(g_device, g_orionLodSetLayout, nullptr);
            g_orionLodSetLayout = VK_NULL_HANDLE;
        }
        if (g_orionLodRenderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(g_device, g_orionLodRenderPass, nullptr);
            g_orionLodRenderPass = VK_NULL_HANDLE;
        }

        for (uint32_t i = 0u; i < ORION_LOD_STAGING_FRAMES; ++i)
        {
            if (g_orionLodStagingMapped[i] != nullptr)
            {
                vkUnmapMemory(g_device, g_orionLodStagingMemory[i]);
                g_orionLodStagingMapped[i] = nullptr;
            }
            if (g_orionLodStagingBuffer[i] != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(g_device, g_orionLodStagingBuffer[i], nullptr);
                g_orionLodStagingBuffer[i] = VK_NULL_HANDLE;
            }
            if (g_orionLodStagingMemory[i] != VK_NULL_HANDLE)
            {
                vkFreeMemory(g_device, g_orionLodStagingMemory[i], nullptr);
                g_orionLodStagingMemory[i] = VK_NULL_HANDLE;
            }
        }
        if (g_orionLodPoolBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(g_device, g_orionLodPoolBuffer, nullptr);
            g_orionLodPoolBuffer = VK_NULL_HANDLE;
        }
        if (g_orionLodPoolMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, g_orionLodPoolMemory, nullptr);
            g_orionLodPoolMemory = VK_NULL_HANDLE;
        }
    }

    g_gpuTiles.clear();
    g_freeSlots.clear();
    g_retiredSlots.clear();
    g_orionLodBoundRtSet = VK_NULL_HANDLE;
    g_orionLodBoundActive = false;
}

void clearOrionLodState()
{
    g_lodEpoch.fetch_add(1u);
    {
        std::lock_guard<std::mutex> ql(g_lodQueueMutex);
        g_lodInputQueue.clear();
    }
    {
        std::lock_guard<std::mutex> ul(g_lodUploadMutex);
        g_lodReadyUploads.clear();
    }
    g_lodQueueCv.notify_one();
}

// ---------------------------------------------------------------------------
// JNI 入口
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_configureOrionLod(
    JNIEnv* env, jclass,
    jboolean enabled, jint maxDistanceChunks,
    jint seaLevel, jint minY, jint maxY, jint dimKind, jstring cacheDir)
{
    ensureLodWorkerStarted();

    const bool en = (enabled == JNI_TRUE);
    const int32_t clampedDist = std::clamp<int32_t>(maxDistanceChunks, 32, 256);

    g_lodEnabled.store(en);
    g_lodMaxDistanceChunks.store(clampedDist);
    g_lodSeaLevel.store(seaLevel);
    g_lodMinY.store(minY);
    g_lodMaxY.store(maxY);
    g_lodDimKind.store(dimKind);

    std::string dirStr;
    if (cacheDir != nullptr)
    {
        const char* utf = env->GetStringUTFChars(cacheDir, nullptr);
        if (utf != nullptr)
        {
            dirStr.assign(utf);
            env->ReleaseStringUTFChars(cacheDir, utf);
        }
    }

    bool changedWorld = false;
    {
        std::lock_guard<std::mutex> ql(g_lodQueueMutex);
        if (g_lodCacheDir != dirStr)
        {
            g_lodCacheDir = std::move(dirStr);
            changedWorld = true;
        }
    }
    if (changedWorld)
    {
        clearOrionLodState();
    }
}

extern "C" JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateOrionLodCamera(
    JNIEnv*, jclass,
    jdouble camX, jdouble camY, jdouble camZ, jint vanillaRenderDistanceChunks)
{
    g_lodCamX.store(camX);
    g_lodCamY.store(camY);
    g_lodCamZ.store(camZ);
    g_lodVanillaRd.store(vanillaRenderDistanceChunks);
}

extern "C" JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_submitOrionLodRealChunk(
    JNIEnv* env, jclass,
    jint chunkX, jint chunkZ, jbyteArray columnData)
{
    if (!g_lodEnabled.load() || columnData == nullptr)
    {
        return;
    }
    if (env->GetArrayLength(columnData) != static_cast<jsize>(ORION_LOD_PAYLOAD_BYTES))
    {
        return;
    }
    ensureLodWorkerStarted();

    PendingInputItem item;
    item.isRealChunk = true;
    item.x = chunkX;
    item.z = chunkZ;
    item.epoch = g_lodEpoch.load();
    env->GetByteArrayRegion(columnData, 0, ORION_LOD_PAYLOAD_BYTES,
        reinterpret_cast<jbyte*>(item.cols.data()));

    {
        std::lock_guard<std::mutex> ql(g_lodQueueMutex);
        if (g_lodInputQueue.size() < 512u)
        {
            g_lodInputQueue.push_back(std::move(item));
        }
    }
    g_lodQueueCv.notify_one();
}

extern "C" JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_submitOrionLodTile(
    JNIEnv* env, jclass,
    jint lodLevel, jint tileX, jint tileZ, jbyteArray cellData, jboolean isReal)
{
    if (!g_lodEnabled.load() || cellData == nullptr ||
        lodLevel < 0 || lodLevel >= static_cast<jint>(ORION_LOD_MAX_LEVELS))
    {
        return;
    }
    if (env->GetArrayLength(cellData) != static_cast<jsize>(ORION_LOD_PAYLOAD_BYTES))
    {
        return;
    }
    ensureLodWorkerStarted();

    PendingInputItem item;
    item.isRealChunk = false;
    item.lod = lodLevel;
    item.x = tileX;
    item.z = tileZ;
    item.isRealFlag = (isReal == JNI_TRUE);
    item.epoch = g_lodEpoch.load();
    env->GetByteArrayRegion(cellData, 0, ORION_LOD_PAYLOAD_BYTES,
        reinterpret_cast<jbyte*>(item.cols.data()));

    {
        std::lock_guard<std::mutex> ql(g_lodQueueMutex);
        if (g_lodInputQueue.size() < 512u)
        {
            g_lodInputQueue.push_back(std::move(item));
        }
    }
    g_lodQueueCv.notify_one();
}

extern "C" JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_clearOrionLod(
    JNIEnv*, jclass)
{
    clearOrionLodState();
}
