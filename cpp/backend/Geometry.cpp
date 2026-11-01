// Orion backend — Geometry 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"



uint64_t makeSectionKey(
    int x,
    int y,
    int z
)
{
    uint64_t ux =
        static_cast<uint64_t>(
            static_cast<uint32_t>(x)
        );

    uint64_t uy =
        static_cast<uint64_t>(
            static_cast<uint32_t>(y)
        );

    uint64_t uz =
        static_cast<uint64_t>(
            static_cast<uint32_t>(z)
        );


    return
        (ux << 42) |
        (uy << 21) |
        uz;
}

// SER-1: Capability-only probe.  The extension must be enabled in the Java
// VkDeviceCreateInfo feature chain before its shaders can use Hit Objects;
// do not attempt to mutate an already-created logical device here.
void probeSerSupport()
{
    g_serExtAdvertised = false;
    g_serFeatureSupported = false;
    g_serHardwareReordering = false;
    g_serNvAdvertised = false;
    g_serNvHintReorder = false;

    if(!g_instance || !g_physicalDevice)
    {
        RTLog("[SER-1 PROBE] ext=0 feature=0 reason=missingPhysicalDevice");
        return;
    }

    uint32_t extensionCount = 0;
    VkResult extensionResult = vkEnumerateDeviceExtensionProperties(
        g_physicalDevice, nullptr, &extensionCount, nullptr);
    if(extensionResult != VK_SUCCESS)
    {
        RTLog("[SER-1 PROBE] ext=0 feature=0 reason=enumerateFailed");
        return;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    extensionResult = vkEnumerateDeviceExtensionProperties(
        g_physicalDevice, nullptr, &extensionCount, extensions.data());
    if(extensionResult != VK_SUCCESS)
    {
        RTLog("[SER-1 PROBE] ext=0 feature=0 reason=enumerateListFailed");
        return;
    }

    for(const VkExtensionProperties& extension : extensions)
    {
        if(std::strcmp(
            extension.extensionName,
            VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) == 0)
        {
            g_serExtAdvertised = true;
            break;
        }
    }

    for(const VkExtensionProperties& extension : extensions)
    {
        if(std::strcmp(
            extension.extensionName,
            VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME) == 0)
        {
            g_serNvAdvertised = true;
            break;
        }
    }

    if(g_serExtAdvertised)
    {
        auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2"));
        if(!getFeatures2)
        {
            getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2KHR"));
        }

        if(getFeatures2)
        {
            VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT serFeatures{};
            serFeatures.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 features2{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features2.pNext = &serFeatures;
            getFeatures2(g_physicalDevice, &features2);
            g_serFeatureSupported =
                serFeatures.rayTracingInvocationReorder == VK_TRUE;
        }

        auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2"));
        if(!getProperties2)
        {
            getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2KHR"));
        }
        if(getProperties2)
        {
            VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT serProperties{};
            serProperties.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_PROPERTIES_EXT;
            VkPhysicalDeviceRayTracingInvocationReorderPropertiesNV serPropertiesNv{};
            serPropertiesNv.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_PROPERTIES_NV;
            serProperties.pNext = &serPropertiesNv;
            VkPhysicalDeviceProperties2 properties2{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties2.pNext = &serProperties;
            getProperties2(g_physicalDevice, &properties2);
            g_serHardwareReordering =
                serProperties.rayTracingInvocationReorderReorderingHint ==
                    VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_EXT ||
                serPropertiesNv.rayTracingInvocationReorderReorderingHint ==
                    VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_NV;
            g_serNvHintReorder =
                serPropertiesNv.rayTracingInvocationReorderReorderingHint ==
                VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_NV;
        }
    }

    std::stringstream log;
    log << "[SER-1 PROBE]"
        << " ext=" << (g_serExtAdvertised ? 1 : 0)
        << " feature=" << (g_serFeatureSupported ? 1 : 0)
        << " hardwareReorder=" << (g_serHardwareReordering ? 1 : 0)
        << " deviceEnable=javaManaged"
        << " enabled=" << (g_serEnabled.load() ? 1 : 0)
        << " action="
        << (!g_serEnabled.load()
            ? "disabledByConfig"
            : (g_serFeatureSupported
                ? (g_serHardwareReordering
                    ? "SER3Eligible"
                    : "SERNoReorderHintPlainRaygen")
                : "fallbackTraceRay"))
        << " extNv=" << (g_serNvAdvertised ? 1 : 0)
        << " hintNv=" << (g_serNvHintReorder ? 1 : 0);
    RTLog(log.str());
}
uint32_t packSectionVertexColor(
    float r,
    float g,
    float b,
    float a)
{
    auto toByte = [](float value) -> uint32_t
    {
        const float clamped =
            value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
    };

    return toByte(r) |
        (toByte(g) << 8) |
        (toByte(b) << 16) |
        (toByte(a) << 24);
}
SectionVertex makeSectionVertex(const Vertex& source)
{
    SectionVertex out;
    out.x = source.x;
    out.y = source.y;
    out.z = source.z;
    out.u = source.u;
    out.v = source.v;
    out.color = packSectionVertexColor(
        source.r,
        source.g,
        source.b,
        source.a);
    out.materialId = source.materialId;
    out.pad2 =
        source.pad1 > 0.0f
            ? (source.pad2 + SECTION_VERTEX_PAD1_BIAS)
            : source.pad2;

    return out;
}


void applyStainedGlassDyeToRTVertex(
    const uint8_t materialMarker,
    Vertex& vertex
)
{
    if(materialMarker < 3u || materialMarker > 18u)
    {
        return;
    }

    const uint32_t rgb =
        kStainedGlassDyeRgbById[
            static_cast<uint32_t>(materialMarker - 3u)
        ];

    vertex.r =
        static_cast<float>((rgb >> 16u) & 0xffu) /
        255.0f;

    vertex.g =
        static_cast<float>((rgb >> 8u) & 0xffu) /
        255.0f;

    vertex.b =
        static_cast<float>(rgb & 0xffu) /
        255.0f;
}
void chunkLockNoteMax(
    std::atomic<uint64_t>& slot,
    uint64_t nanos)
{
    uint64_t prev = slot.load(std::memory_order_relaxed);

    while(nanos > prev &&
          !slot.compare_exchange_weak(
              prev,
              nanos,
              std::memory_order_relaxed))
    {
    }
}
void chunkCmpNoteMax(
    std::atomic<uint64_t>& slot,
    uint64_t nanos)
{
    uint64_t prev = slot.load(std::memory_order_relaxed);

    while(nanos > prev &&
          !slot.compare_exchange_weak(
              prev,
              nanos,
              std::memory_order_relaxed))
    {
    }
}
double chunkCmpMsSince(
    const std::chrono::steady_clock::time_point& from)
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - from
    ).count();
}
void chunkCmpReport()
{
    const uint32_t count = g_chunkCmpCount.fetch_add(1u);
    if((count % 256u) != 255u)
    {
        return;
    }

    RTLog(
        "[CHUNK CMP] n=256"
        " maxRaw1=" + std::to_string(
            g_chunkCmpRaw1MaxNanos.exchange(0u) / 1000000.0) +
        " maxRaw2=" + std::to_string(
            g_chunkCmpRaw2MaxNanos.exchange(0u) / 1000000.0) +
        " maxMesh=" + std::to_string(
            g_chunkCmpMeshMaxNanos.exchange(0u) / 1000000.0) +
        " lastBytes=" + std::to_string(
            g_chunkCmpLastBytes.load()) +
        " lastVerts=" + std::to_string(
            g_chunkCmpLastVerts.load()) +
        " calls=" + std::to_string(g_chunkUploadCalls.load()) +
        " dedup=" + std::to_string(g_chunkUploadDedup.load())
    );
}
bool chunkMemcmpTimed(
    const void* lhs,
    const void* rhs,
    size_t bytes,
    std::atomic<uint64_t>& slot)
{
    const auto t0 = std::chrono::steady_clock::now();
    const bool equal = std::memcmp(lhs, rhs, bytes) == 0;

    chunkCmpNoteMax(
        slot,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0
            ).count()
        )
    );
    g_chunkCmpLastBytes.store(static_cast<uint64_t>(bytes));
    chunkCmpReport();

    return equal;
}

// 把某一动态层的位移暂存落到 g_chunkMotion。**必须每帧都调用**，不能跟着顶点
// 一起去重：顶点逐字节没变时（实体的 "changed==false" 早退路径）位移恒为 0，
// 若沿用上一帧那份非零位移，MV 会朝反方向拖。
//
// =====================================================
// 2026-10-17 返回值 = "必须强制重建一次才能让 GPU 追上这个 0"。
//
// 病灶（此前一直存在，且只在"动完停下"时出现）：这里把 g_chunkMotion 改成全 0，
// 但**没有任何人因此触发重建** —— `endEntityFrame` 的 `changed` 只看顶点是否变化。
// 于是：
//   · CPU 侧缓存 = 0（看起来已经修好了）；
//   · GPU 顶点缓冲的位移尾段 = 上一帧那份**非零**值（没人重写它）；
//   · shader 读的是 GPU ⇒ 那个实体拿到一个**恒定不变**的错误 MV。
// 表现：实体停下之后拖影不消失，直到它再动一次（那时顶点变化 ⇒ 重建 ⇒ 尾段被重写）。
//
// 判据只取"旧缓存里有非零值"这一件事：一旦被清零，下一帧就不再返回 true
// ⇒ 不会每帧都重建（自限一次）。
// =====================================================
bool publishDynamicLayerMotion(
    uint64_t layerKey,
    std::vector<float>& frameMotion,
    size_t vertexCount,
    bool verticesChanged)
{
    if(vertexCount == 0u)
    {
        g_chunkMotion.erase(layerKey);
        frameMotion.clear();
        return false;
    }

    if(
        !verticesChanged ||
        frameMotion.size() != vertexCount * 3u
    )
    {
        bool staleNonZero = false;

        auto existing = g_chunkMotion.find(layerKey);

        if(
            existing != g_chunkMotion.end() &&
            existing->second.size() == vertexCount * 3u
        )
        {
            for(const float value : existing->second)
            {
                if(value != 0.0f)
                {
                    staleNonZero = true;
                    break;
                }
            }

            std::fill(
                existing->second.begin(),
                existing->second.end(),
                0.0f
            );
        }
        else
        {
            g_chunkMotion[layerKey].assign(vertexCount * 3u, 0.0f);
        }

        frameMotion.clear();

        return staleNonZero;
    }

    std::vector<float>& cached = g_chunkMotion[layerKey];
    cached.swap(frameMotion);
    frameMotion.clear();

    return false;
}
bool requestDynamicRTASRebuild(bool changed)
{
    g_dynamicRTASUpdatePending |= changed;
    if(!g_dynamicRTASUpdatePending)
    {
        return false;
    }

    if(
        g_lastDynamicRTASUpdateFrame != UINT64_MAX &&
        g_perfL10FrameSerial <
            g_lastDynamicRTASUpdateFrame + g_perfDynamicRtasInterval.load()
    )
    {
        return false;
    }

    g_dynamicRTASUpdatePending = false;
    g_lastDynamicRTASUpdateFrame = g_perfL10FrameSerial;
    return true;
}
void invalidateDynamicTextureBindingsForAtlasReload()
{
    for(uint32_t slot = 0u; slot < ENTITY_RT_TEXTURE_SLOT_COUNT; ++slot)
    {
        g_entityTextureViews[slot] = VK_NULL_HANDLE;
        g_entityTextureSamplers[slot] = VK_NULL_HANDLE;
        g_entityNormalViews[slot] = VK_NULL_HANDLE;
        g_entityNormalSamplers[slot] = VK_NULL_HANDLE;
        g_entitySpecularViews[slot] = VK_NULL_HANDLE;
        g_entitySpecularSamplers[slot] = VK_NULL_HANDLE;
    }

    for(uint32_t slot = 0u; slot < PARTICLE_RT_TEXTURE_SLOT_COUNT; ++slot)
    {
        g_particleTextureViews[slot] = VK_NULL_HANDLE;
        g_particleTextureSamplers[slot] = VK_NULL_HANDLE;
    }

    g_entityTexturesDirty = false;
    g_entityPbrTexturesDirty = false;
    g_particleTexturesDirty = false;
    g_rainStreakTextureView = VK_NULL_HANDLE;
    g_rainStreakTextureSampler = VK_NULL_HANDLE;
    g_rainStreakTextureDirty = false;
    g_starMapTextureView = VK_NULL_HANDLE;
    g_starMapTextureSampler = VK_NULL_HANDLE;
    g_starMapTextureDirty = false;
    g_endSkyAtlasView = VK_NULL_HANDLE;
    g_endSkyAtlasSampler = VK_NULL_HANDLE;
    g_endSkyAtlasDirty = false;

    g_atlasImageViewAtReload = g_atlasImageView;
    g_atlasSamplerAtReload = g_atlasSampler;
    g_atlasReloadAwaitingPublish = g_atlasImageView != VK_NULL_HANDLE;
    g_atlasImageView = VK_NULL_HANDLE;

    g_dynamicTextureBindingsNeedAtlasRefresh = true;
    g_dynamicTextureBindingsCanPublish = false;
}

// 8 字节一步的 FNV-1a（FNV 乘子 + 异或右移），与 endEntityFrame 里那份内容哈希
// 同一口径；对 20KB 载荷约等于一次 memcmp，但发生在调用方线程上，不占全局锁。
uint64_t chunkPayloadDigest64(
    const void* data,
    size_t bytes,
    uint64_t seed)
{
    const uint8_t* const p = static_cast<const uint8_t*>(data);

    uint64_t hash = 1469598103934665603ull ^ seed;

    size_t i = 0u;

    for(; i + 8u <= bytes; i += 8u)
    {
        uint64_t word = 0u;
        std::memcpy(&word, p + i, sizeof(word));

        hash ^= word;
        hash *= 1099511628211ull;
        hash ^= hash >> 29u;
    }

    if(i < bytes)
    {
        uint64_t tail = 0u;
        std::memcpy(&tail, p + i, bytes - i);
        hash ^= tail;
        hash *= 1099511628211ull;
    }

    hash ^= hash >> 32u;

    return hash;
}

// SectionCompiler can submit an already compiled layer again while the
// underlying Section has not changed.  Do not turn that duplicate upload into
// a BLAS/TLAS rebuild.  Every Vertex field is explicitly populated in
// uploadChunk(), so byte-wise comparison is both exact and stable here.
// 2026-10-22（第 8 条）：每帧"实体/本地玩家载荷变没变"的判等，从 O(字节) 的
// memcmp 换成 64 位内容哈希。
//
// 为什么：endEntityFrame 每帧都在 g_chunkMutex 内对这两份载荷各做一次全量
// memcmp（两份都要读一遍）。载荷规模随视野内实体数增长，而它同时是渲染线程
// 成本 + 持锁时间（正是第 3 条那个 82ms 尖峰的排队来源之一）。
// 哈希按 8 字节一步（FNV 乘子 + 异或右移），比逐字节 memcmp 快约 5~8 倍，
// 且只读**一份**（memcmp 要读两份）。
//
// 语义：碰撞概率 2^-64；万一碰撞，该层几何晚一帧更新（下一帧内容变化即自愈，
// 静态实体本来也不变）。这里换的是"判等成本"，不是判等结论的精度来源。
uint64_t hashVertexPayload(
    const std::vector<Vertex>& payload
)
{
    uint64_t hash =
        0x9E3779B97F4A7C15ull ^
        static_cast<uint64_t>(payload.size());

    const uint8_t* const bytes =
        reinterpret_cast<const uint8_t*>(payload.data());

    const size_t totalBytes =
        payload.size() * sizeof(Vertex);

    size_t offset = 0u;

    while(offset + 8u <= totalBytes)
    {
        uint64_t word = 0u;
        std::memcpy(&word, bytes + offset, sizeof(word));

        hash ^= word;
        hash *= 0x100000001B3ull;
        hash ^= hash >> 29u;

        offset += 8u;
    }

    uint64_t tail = 0u;

    for(size_t i = 0u; offset + i < totalBytes; ++i)
    {
        tail |= static_cast<uint64_t>(bytes[offset + i]) << (8u * i);
    }

    hash ^= tail;
    hash *= 0x100000001B3ull;
    hash ^= hash >> 32u;

    return hash;
}
bool sameVertexPayload(
    const std::vector<Vertex>& a,
    const std::vector<Vertex>& b
)
{
    return
        a.size() == b.size() &&
        (
            a.empty() ||
            std::memcmp(
                a.data(),
                b.data(),
                a.size() * sizeof(Vertex)
            ) == 0
        );
}
int32_t rtSignExtendBits(uint64_t raw, uint32_t bits)
{
    const uint64_t mask = (1ull << bits) - 1ull;
    const uint64_t signBit = 1ull << (bits - 1u);
    const uint64_t value = raw & mask;

    if((value & signBit) == 0ull)
    {
        return static_cast<int32_t>(value);
    }

    return static_cast<int32_t>(
        static_cast<uint32_t>(value | ~mask));
}
int32_t rtFloorDivPow2(int32_t value, uint32_t shift)
{
    return value >= 0
        ? (value >> shift)
        : ~((~value) >> shift);
}
uint64_t rtSectionBucketKeyOf(uint64_t classKey)
{
    const uint64_t spatialKey = classKey & SPATIAL_SECTION_KEY_MASK;

    const uint32_t geometryClass =
        static_cast<uint32_t>((classKey >> 60u) & 0xFull);

    const int32_t regionX = rtFloorDivPow2(
        rtSignExtendBits(spatialKey & 0x3FFFFFull, 22u),
        SECTION_BUCKET_REGION_CHUNKS_LOG2);

    const int32_t regionZ = rtFloorDivPow2(
        rtSignExtendBits((spatialKey >> 22u) & 0x3FFFFFull, 22u),
        SECTION_BUCKET_REGION_CHUNKS_LOG2);

    const int32_t regionY = rtFloorDivPow2(
        rtSignExtendBits((spatialKey >> 44u) & 0xFFFFull, 16u),
        SECTION_BUCKET_REGION_CHUNKS_LOG2);

    uint64_t bucketKey = static_cast<uint64_t>(geometryClass & 0x7u);

    bucketKey |= static_cast<uint64_t>(
        static_cast<uint32_t>(regionX + 0x100000)) << 3u;
    bucketKey |= static_cast<uint64_t>(
        static_cast<uint32_t>(regionZ + 0x100000)) << 25u;
    bucketKey |= static_cast<uint64_t>(
        static_cast<uint32_t>(regionY + 0x1000)) << 47u;

    return bucketKey;
}
void rtLogSectionBucketStats()
{
    if(g_sectionBLAS.empty())
    {
        return;
    }

    std::unordered_map<uint64_t, uint32_t> bucketMembers;
    bucketMembers.reserve(g_sectionBLAS.size());

    for(const auto& entry : g_sectionBLAS)
    {
        ++bucketMembers[rtSectionBucketKeyOf(entry.first)];
    }

    size_t maxMembers = 0u;
    size_t overLimit = 0u;
    size_t memberSum = 0u;

    for(const auto& bucket : bucketMembers)
    {
        const size_t members = static_cast<size_t>(bucket.second);

        memberSum += members;

        if(members > maxMembers)
        {
            maxMembers = members;
        }

        if(members > SECTION_BUCKET_MAX_MEMBERS)
        {
            ++overLimit;
        }
    }

    std::ostringstream line;
    line
        << "[MERGE] sections=" << g_sectionBLAS.size()
        << " buckets=" << bucketMembers.size()
        << " avgMembers="
        << (memberSum / std::max<size_t>(bucketMembers.size(), 1u))
        << " maxMembers=" << maxMembers
        << " over" << SECTION_BUCKET_MAX_MEMBERS << "=" << overLimit
        << " (region=" << (1u << SECTION_BUCKET_REGION_CHUNKS_LOG2)
        << " chunks)";

    RTLog(line.str());
}

// =====================================================
// 2026-10-24 TLAS 主射线掩码剔除（项 1：原版可见性图）
//
// 数据源：Java 侧每帧把 LevelRenderer.visibleSections()（26.3 源码 :1289）
// 的 Section 坐标推下来。那份表由 LevelExtractor.applyFrustum（:472）经
// SectionOcclusionGraph.addSectionsInFrustum 的 Octree 遍历填充，只含"从相机
// 出发可达"的 Section ⇒ 它排除的 Section 不可能成为主射线的 first hit。
//
// 只清 mask 的 bit0（主射线），实例保留在 TLAS 里 ⇒ 反射/GI/阴影/体积光/
// 雨探针（0xff / 0x04 / 0x08）的行为逐位不变。这是整个改动的安全前提：
// 被剔的几何对次级射线仍然存在，不会漏光、不会漏反射。
//
// 模式（环境变量 ORION_VIS_CULL）：
//   0 = 关（逐位回到改动前）
//   1 = 只统计（默认）：日志里的 visCand= 就是"若开启会清掉多少实例"
//   2 = 生效
// 默认取 1 而不是 2：「可见性图与本后端的几何集合是否一致」这一条还没有实机
// 数据（原版图只覆盖原版 raster 会画的 Section），先让数字说话，确认后再切 2。
// =====================================================
int visibleSectionCullMode()
{
    static const int mode = []()
    {
        const char* raw = std::getenv("ORION_VIS_CULL");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 1;
        }

        return std::atoi(raw);
    }();

    return mode;
}
uint64_t probeGeometryRevision()
{
    static const bool s_terrainOnly = []() -> bool
    {
        const char* raw = std::getenv("ORION_PROBES_TERRAIN_REV");

        return !(raw != nullptr && raw[0] == '0');
    }();

    return s_terrainOnly
        ? g_terrainGeometryRevision.load(std::memory_order_acquire)
        : g_worldGeometryRevision.load(std::memory_order_acquire);
}
void markWorldGeometryDirty(bool terrainChanged )
{
    g_worldGeometryRevision.fetch_add(1u, std::memory_order_release);

    if(terrainChanged)
    {
        g_terrainGeometryRevision.fetch_add(1u, std::memory_order_release);
    }

    g_worldGeometryDirty.store(true, std::memory_order_release);

    notifyGeometryCaptureWorker();
}
bool captureQueueCursorEnabled()
{
    static const bool enabled = []()
    {
        const char* raw = std::getenv("ORION_CAPTURE_CURSOR");

        return !(raw != nullptr &&
                 (raw[0] == '0' || raw[0] == 'f' || raw[0] == 'F'));
    }();

    return enabled;
}

// 调用方必须已持 g_capturedSectionsMutex。
void resetCapturedSectionsLocked()
{
    g_capturedSections.clear();
    g_capturedSectionsHead = 0u;
}
void notifyGeometryCaptureWorker()
{
    g_captureWorkRevision.fetch_add(1u, std::memory_order_release);

    if(g_captureWorkerWaiting.load(std::memory_order_acquire))
    {
        g_geometryCaptureWake.notify_one();
    }
}

// 2026-10-31：两项捕获节流参数做成运行时可调，调参不必再等一次冷编。
//   ORION_CAPTURE_WAKE_US  ：有活时每批之后让出的微秒数（默认 4000 ≈ 改前那套
//                            4ms 轮询）。0 = "立刻重抢锁"，实测把 upWaitMs p95
//                            顶到 42ms；250 只是折中（30.1ms），不如轮询。
//                            既然 queued 恒 0、吞吐本来就不是瓶颈，就退回轮询。
//   ORION_CAPTURE_PER_WAKE ：每次持锁最多抓几段（默认 4，上限 64）。
uint64_t geometryCaptureWakeMicros()
{
    static const uint64_t cached = []() -> uint64_t
    {
        const char* raw = std::getenv("ORION_CAPTURE_WAKE_US");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 4000ull;
        }

        const long parsed = std::strtol(raw, nullptr, 10);

        if(parsed < 0)
        {
            return 0ull;
        }

        return static_cast<uint64_t>(std::min<long>(parsed, 5000));
    }();

    return cached;
}
size_t geometryCapturePerWake()
{
    static const size_t cached = []() -> size_t
    {
        const char* raw = std::getenv("ORION_CAPTURE_PER_WAKE");

        if(raw == nullptr || raw[0] == '\0')
        {
            return GEOMETRY_CAPTURE_MAX_PER_WAKE;
        }

        const long parsed = std::strtol(raw, nullptr, 10);

        if(parsed < 1)
        {
            return GEOMETRY_CAPTURE_MAX_PER_WAKE;
        }

        return static_cast<size_t>(std::min<long>(parsed, 64));
    }();

    return cached;
}

// 2026-10-15 捕获时间闸的总闸。ORION_CAPTURE_TIME_GATE=0 退回旧行为（单次唤醒
// 只受 MAX_PER_WAKE 条数约束、不受时间约束），用于 A/B 对照与快速回退。
bool captureTimeGateEnabled()
{
    static const bool enabled = []()
    {
        const char* raw = std::getenv("ORION_CAPTURE_TIME_GATE");
        return !(raw != nullptr && raw[0] == '0');
    }();

    return enabled;
}
void geometryAppendSectionVertices(
    std::vector<Vertex>& destination,
    const std::vector<Vertex>& source
)
{
    if(source.empty())
    {
        return;
    }
    const size_t requiredSize = destination.size() + source.size();
    if(destination.capacity() < requiredSize)
    {
        const size_t geometricCapacity =
            destination.capacity() > std::numeric_limits<size_t>::max() / 2u
                ? requiredSize
                : destination.capacity() * 2u;
        destination.reserve(std::max(requiredSize, geometricCapacity));
    }
    destination.insert(destination.end(), source.begin(), source.end());
}

// =====================================================
// 2026-10-09 运动矢量：把某一动态层的"上一帧位移"（相机相对）追加进 split。
//
// 唯一的硬约束是**逐顶点配对**：destination 的长度必须变成 vertexCount*3，
// 也就是与刚刚 append 进去的顶点数严格对齐。长度不够（旧数据 / 该批没带位移）
// 就补 0 —— 绝不允许"顶点多、位移少"，那会让 GPU 侧按索引读到位移错位的数据
// （表现：移动物体的残影张冠李戴、且随帧漂移）。
// =====================================================
void geometryAppendSectionMotion(
    std::vector<float>& destination,
    const std::vector<float>* source,
    size_t vertexCount)
{
    const size_t requiredCount =
        vertexCount * 3u;

    destination.reserve(
        destination.size() + requiredCount
    );

    if(source != nullptr && !source->empty())
    {
        const size_t copyCount =
            std::min(source->size(), requiredCount);

        destination.insert(
            destination.end(),
            source->begin(),
            source->begin() + copyCount
        );

        destination.insert(
            destination.end(),
            requiredCount - copyCount,
            0.0f
        );

        return;
    }

    destination.insert(
        destination.end(),
        requiredCount,
        0.0f
    );
}

// 锁内约定：调用方必须持有 g_chunkMutex（与 uploadChunk 同一把）。
//
// =====================================================
// 2026-10-21（第 2 条）：把 48B → 32B 的地形打包从渲染线程搬到分类拷贝这一步。
//
// 这里跑在几何捕获线程上（ORION_SYNC_GEOMETRY=1 时退化为渲染线程，行为不变、
// 只是没有收益），对四个地形桶各扫一遍顶点。渲染线程的 staging 段随后只剩
// memcpy —— 原来那段逐顶点 makeSectionVertex 在 [RT GEOMETRY SLOW] 里单列
// `staging=`：200 个慢帧样本均值 2.97ms、峰值 72.61ms。
//
// 判据用的是同一份 sectionUsesPackedVertices()（唯一实现，见 19066），
// class 位与 rebuildGeometryClass 的填法逐项一致。即使将来判据在某处漂移，
// 消费端还有 `packed.size() == vertices.size()` 校验兜底 ⇒ 只会退回慢路径。
//
// 2026-10-22 索引化地形打开后，"32B 打包"与"4 顶点/quad"是**同时成立**的
// （4×32B = 128B/quad，对比展开的 6×32B = 192B/quad）⇒ 这里的 indexed 标志
// 必须与 createSectionBLAS 同源推导：用 sectionUsesIndexedGeometry()。
// =====================================================
void packSectionSplitBucketsLocked(
    uint64_t spatialSectionKey,
    SectionGeometrySplit& split)
{
    const auto packOne =
        [spatialSectionKey](
            uint64_t classBits,
            const std::vector<Vertex>& source,
            std::vector<SectionVertex>& destination)
        {
            if(!source.empty() &&
               sectionUsesPackedVertices(
                   spatialSectionKey | classBits,
                   sectionUsesIndexedGeometry(
                       spatialSectionKey | classBits,
                       source.size()),
                   source.size()))
            {
                destination.resize(source.size());

                for(size_t vertexIndex = 0u;
                    vertexIndex < source.size();
                    ++vertexIndex)
                {
                    destination[vertexIndex] =
                        makeSectionVertex(source[vertexIndex]);
                }

                return;
            }

            destination.clear();
        };

    packOne(SECTION_AS_CLASS_OPAQUE, split.opaque, split.opaquePacked);
    packOne(SECTION_AS_CLASS_OMM, split.ommCutout, split.ommCutoutPacked);
    packOne(SECTION_AS_CLASS_ALPHA, split.alpha, split.alphaPacked);
    packOne(SECTION_AS_CLASS_WATER, split.water, split.waterPacked);
}

// **分类路由的唯一实现**（layerId 路由 + OMM 合法性判定 + 半透明层 marker-1 水分离）。
// 三个消费者都走它：
//   1. geometryCaptureWorkerLoop —— 捕获线程，产出 g_capturedSections；
//   2. rebuildWorldGeometry 的增量批次（按脏 spatialKey 枚举 / 仅实体聚合的快路）；
//   3. rebuildWorldGeometry 的 forceReset 全量重建（按键分组后逐键调用）。
//
// 2026-10-08 之前它是**三份手抄副本**（注释还写着"必须一起改"）。收拢时发现第三份
//（forceReset）在半透明混装层上漏了 marker-1 水分离（整层按 alpha 处理），已随之修正。
void geometryCaptureSectionLocked(
    uint64_t spatialSectionKey,
    SectionGeometrySplit& split
)
{
    for(uint64_t layerIndex = 0ull; layerIndex < 16ull; ++layerIndex)
    {
        auto layerIt = g_chunkMeshes.find(
            (layerIndex << 60ull) | spatialSectionKey
        );
        if(layerIt == g_chunkMeshes.end())
        {
            continue;
        }
        const auto& layerEntry = *layerIt;
        const uint32_t layerId = static_cast<uint32_t>(layerIndex);

        const auto layerMotionIt =
            g_chunkMotion.find(layerEntry.first);
        const std::vector<float>* layerMotion =
            layerMotionIt != g_chunkMotion.end()
                ? &layerMotionIt->second
                : nullptr;

        static const uint32_t mvLayerMask = []()
        {
            const char* raw = std::getenv("ORION_MV_LAYERS");

            if(raw == nullptr || raw[0] == '\0')
            {
                return 1u;
            }

            const int value = std::atoi(raw);

            return static_cast<uint32_t>(value < 0 ? 0 : (value > 3 ? 3 : value));
        }();

        if(layerEntry.first == LOCAL_PLAYER_RT_LAYER_KEY)
        {
            geometryAppendSectionVertices(split.localPlayer, layerEntry.second);
        }
        else if(layerEntry.first == ENTITY_RT_LAYER_KEY)
        {
            geometryAppendSectionVertices(split.entity, layerEntry.second);
            if((mvLayerMask & 1u) != 0u)
            {
                geometryAppendSectionMotion(
                    split.entityMotion,
                    layerMotion,
                    layerEntry.second.size()
                );
            }
        }
        else if(layerEntry.first == PARTICLE_RT_LAYER_KEY)
        {
            geometryAppendSectionVertices(split.particle, layerEntry.second);
            if((mvLayerMask & 2u) != 0u)
            {
                geometryAppendSectionMotion(
                    split.particleMotion,
                    layerMotion,
                    layerEntry.second.size()
                );
            }
        }
        else if(layerId == 0u)
        {
            geometryAppendSectionVertices(split.opaque, layerEntry.second);
        }
        else if(layerId == 1u)
        {
            auto ommIt = g_cutoutOmmInputs.find(layerEntry.first);
            const bool validOmm =
                g_ommNativeEnabled &&
                ommIt != g_cutoutOmmInputs.end() &&
                ommIt->second.subdivisionLevel == 4u &&
                ommIt->second.knownMicroTriangleCount > 0u &&
                ommIt->second.triangleCount == layerEntry.second.size() / 3u;
            if(validOmm)
            {
                geometryAppendSectionVertices(split.ommCutout, layerEntry.second);
                split.ommInput = ommIt->second;
                split.hasOmmInput = true;
            }
            else
            {
                geometryAppendSectionVertices(split.alpha, layerEntry.second);
            }
        }
        else if(layerId == 3u)
        {
            geometryAppendSectionVertices(split.water, layerEntry.second);
        }
        else
        {
            const std::vector<Vertex>& layer = layerEntry.second;
            size_t waterVertexCount = 0u;
            size_t alphaVertexCount = 0u;
            for(size_t vertex = 0u; vertex + 2u < layer.size(); vertex += 3u)
            {
                const bool waterTriangle =
                    layer[vertex + 0u].materialId == 1.0f &&
                    layer[vertex + 1u].materialId == 1.0f &&
                    layer[vertex + 2u].materialId == 1.0f;
                (waterTriangle ? waterVertexCount : alphaVertexCount) += 3u;
            }
            split.water.reserve(split.water.size() + waterVertexCount);
            split.alpha.reserve(split.alpha.size() + alphaVertexCount);
            for(size_t vertex = 0u; vertex + 2u < layer.size(); vertex += 3u)
            {
                const bool waterTriangle =
                    layer[vertex + 0u].materialId == 1.0f &&
                    layer[vertex + 1u].materialId == 1.0f &&
                    layer[vertex + 2u].materialId == 1.0f;
                std::vector<Vertex>& destination =
                    waterTriangle ? split.water : split.alpha;
                destination.insert(
                    destination.end(),
                    layer.begin() + vertex,
                    layer.begin() + vertex + 3u
                );
            }
        }
    }
}

// 2026-10-31：脏 Section 的全局取序开关。
//   nearest（默认）：在全量脏集合里挑最近的候选；
//   hash：回退到旧行为（只对迭代器顺序的前 16 个排序），用于 A/B。
bool dirtySectionNearestOrderEnabled()
{
    static const bool enabled = []() -> bool
    {
        const char* raw = std::getenv("ORION_SECTION_ORDER");

        if(raw == nullptr || raw[0] == '\0')
        {
            return true;
        }

        if(raw[0] == '0' || raw[0] == 'h' || raw[0] == 'H')
        {
            return false;
        }

        return true;
    }();

    return enabled;
}
void geometryCaptureWorkerLoop()
{
    // 2026-10-31：捕获线程整轮都只在 g_chunkMutex 的间隙里干活，与 Java 的
    // Sodium 几何上传、渲染线程抢核。降到 BELOW_NORMAL 后它不再和渲染线程
    // 抢同一档时间片（C2ME 的对应做法：worker 用 THREAD_PRIORITY_LOWEST +
    // power throttling，见 c2me-base/common/threadpriority/ThreadPriorityPresets）。
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    fprintf(
        stderr,
        "[THREAD RC] geometryCaptureWorkerLoop tid=%lu\n",
        static_cast<unsigned long>(GetCurrentThreadId())
    );
    fflush(stderr);

    // 2026-10-31：pendingPack 提到循环外复用。捕获频率已经按"有活立刻续跑"
    // 提高（一秒几百轮），每轮重新分配一个 vector 就成了噪音；容量保留，
    // 每轮末尾 clear()。
    std::vector<CapturedSectionUpdate> pendingPack;

    for(;;)
    {
        bool capturedAnything = false;
        bool moreWorkAvailable = false;
        bool nearestOrderUsed = false;
        size_t dirtyKeysAtScan = 0u;
        uint64_t orderScanMicros = 0u;
        double slowestCaptureMs = 0.0;
        uint64_t slowestCaptureKey = 0ull;
        size_t slowestCaptureVertices = 0u;
        uint64_t slowestCaptureCycles = 0ull;
        const auto captureWaitStart = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::timed_mutex> chunkLock(g_chunkMutex);
            const auto captureHoldStart = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> capturedLock(g_capturedSectionsMutex);

            if(!g_forceSectionASReset.load(std::memory_order_acquire) &&
               !g_dirtySectionKeys.empty() &&
               ((g_capturedSections.size() - g_capturedSectionsHead) +
                   pendingPack.size()) <
                   GEOMETRY_CAPTURE_MAX_QUEUED)
            {
                size_t taken = 0u;
                const auto captureBudgetWakeStart =
                    std::chrono::steady_clock::now();

                struct DirtySectionCandidate
                {
                    float distSq;
                    uint64_t key;
                };

                // 2026-10-31：脏 Section 取序从"哈希序前 16 个里挑最近"改成
                // "全量脏集合里挑最近的 16 个"。
                //
                // 旧实现只对迭代器顺序的前 16 个候选排序，而每次唤醒只取 4 个
                // （GEOMETRY_CAPTURE_MAX_PER_WAKE）；erase 掉这 4 个之后下一轮
                // 又是哈希序的下 16 个 ⇒ 全局顺序实际等于随机：飞行 / 传送 /
                // 世界加载时，脚下和天边的 Section 拿到相同的重建机会，表现为
                // "脚下的洞要等好几秒才补上"。
                //
                // 分两条通道，不融合成一个标量（避免两类 Section 互相饿死）：
                //   · 真实世界高度的地形段：按到玩家的距离排；
                //   · voxy/DH LOD 与动态实体聚合：X/Z 是哈希分配的槽位、距离无
                //     意义（见 vramWorldHeightSection 的注释），统一给
                //     DIRTY_FARFIELD_DIST_SQ（≈96 格外），于是近处地形优先、
                //     远景在近处清空后跟上。
                //
                // 扫描成本有硬上限：脏集合超过 DIRTY_SCAN_BUDGET 时按等步长抽样，
                // 保证持锁时间与脏集合规模解耦 —— 这是它能跑在 g_chunkMutex 里的
                // 前提。
                constexpr size_t DIRTY_CANDIDATE_LIMIT = 16u;
                constexpr size_t DIRTY_SCAN_BUDGET = 4096u;
                constexpr float DIRTY_FARFIELD_DIST_SQ = 96.0f * 96.0f;

                DirtySectionCandidate candidates[DIRTY_CANDIDATE_LIMIT];
                size_t candidateCount = 0u;
                size_t worstIndex = 0u;

                const auto refreshWorstCandidate = [&]()
                {
                    worstIndex = 0u;

                    for(size_t i = 1u; i < candidateCount; ++i)
                    {
                        if(candidates[i].distSq >
                           candidates[worstIndex].distSq)
                        {
                            worstIndex = i;
                        }
                    }
                };

                {
                    const CameraUBO* const orderingUbo =
                        g_cameraUboMapped != nullptr
                            ? static_cast<const CameraUBO*>(g_cameraUboMapped)
                            : nullptr;

                    const float playerX =
                        orderingUbo != nullptr ? orderingUbo->playerX : 0.0f;
                    const float playerY =
                        orderingUbo != nullptr ? orderingUbo->playerY : 0.0f;
                    const float playerZ =
                        orderingUbo != nullptr ? orderingUbo->playerZ : 0.0f;

                    nearestOrderUsed = dirtySectionNearestOrderEnabled();
                    dirtyKeysAtScan = g_dirtySectionKeys.size();

                    size_t scanStride = 1u;

                    if(nearestOrderUsed &&
                       dirtyKeysAtScan > DIRTY_SCAN_BUDGET)
                    {
                        scanStride =
                            (dirtyKeysAtScan + DIRTY_SCAN_BUDGET - 1u) /
                            DIRTY_SCAN_BUDGET;
                    }

                    const auto orderScanStart =
                        std::chrono::steady_clock::now();

                    size_t visitedKeys = 0u;

                    for(uint64_t key : g_dirtySectionKeys)
                    {
                        if(scanStride > 1u &&
                           ((visitedKeys++ % scanStride) != 0u))
                        {
                            continue;
                        }

                        if(key == ENTITY_RT_SPATIAL_KEY)
                        {
                            continue;
                        }

                        float distSq = DIRTY_FARFIELD_DIST_SQ;

                        if(vramWorldHeightSection(key))
                        {
                            float centerX = 0.0f;
                            float centerY = 0.0f;
                            float centerZ = 0.0f;
                            vramSectionCenterFromSpatialKey(
                                key, centerX, centerY, centerZ);

                            const float dx = centerX - playerX;
                            const float dy = centerY - playerY;
                            const float dz = centerZ - playerZ;
                            distSq = dx * dx + dy * dy + dz * dz;
                        }

                        if(!nearestOrderUsed)
                        {
                            // 旧行为（A/B 对照）：只取迭代器顺序的前 16 个。
                            if(candidateCount >= DIRTY_CANDIDATE_LIMIT)
                            {
                                break;
                            }

                            candidates[candidateCount].distSq = distSq;
                            candidates[candidateCount].key = key;
                            ++candidateCount;
                            continue;
                        }

                        if(candidateCount < DIRTY_CANDIDATE_LIMIT)
                        {
                            candidates[candidateCount].distSq = distSq;
                            candidates[candidateCount].key = key;

                            if(candidateCount == 0u ||
                               distSq > candidates[worstIndex].distSq)
                            {
                                worstIndex = candidateCount;
                            }

                            ++candidateCount;
                        }
                        else if(distSq < candidates[worstIndex].distSq)
                        {
                            candidates[worstIndex].distSq = distSq;
                            candidates[worstIndex].key = key;
                            refreshWorstCandidate();
                        }
                    }

                    orderScanMicros = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - orderScanStart
                        ).count());

                    std::sort(
                        candidates,
                        candidates + candidateCount,
                        [](const DirtySectionCandidate& a,
                           const DirtySectionCandidate& b)
                        {
                            return a.distSq < b.distSq;
                        });
                }

                for(size_t candidateIndex = 0u;
                    candidateIndex < candidateCount &&
                    taken < geometryCapturePerWake();
                    ++candidateIndex)
                {
                    if(taken != 0u && captureTimeGateEnabled())
                    {
                        const double captureElapsedMs =
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() -
                                    captureBudgetWakeStart
                            ).count();

                        const double budgetMs =
                            g_sectionRebuildBudgetMirrorMs.load(
                                std::memory_order_relaxed);

                        if(captureElapsedMs >=
                           (budgetMs > 0.0
                                ? budgetMs
                                : SECTION_REBUILD_TIME_BUDGET_MS))
                        {
                            break;
                        }
                    }

                    const uint64_t spatialKey =
                        candidates[candidateIndex].key;
                    CapturedSectionUpdate update;
                    update.spatialKey = spatialKey;
                    const auto sectionCaptureStart =
                        std::chrono::steady_clock::now();
                    ULONG64 sectionCaptureCyclesStart = 0ull;
                    QueryThreadCycleTime(
                        GetCurrentThread(),
                        &sectionCaptureCyclesStart);
                    geometryCaptureSectionLocked(spatialKey, update.split);
                    ULONG64 sectionCaptureCyclesEnd = 0ull;
                    QueryThreadCycleTime(
                        GetCurrentThread(),
                        &sectionCaptureCyclesEnd);
                    const double sectionCaptureMs =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                                sectionCaptureStart
                        ).count();
                    if(sectionCaptureMs > slowestCaptureMs)
                    {
                        slowestCaptureMs = sectionCaptureMs;
                        slowestCaptureKey = spatialKey;
                        slowestCaptureCycles =
                            sectionCaptureCyclesEnd - sectionCaptureCyclesStart;
                        slowestCaptureVertices =
                            update.split.opaque.size() +
                            update.split.ommCutout.size() +
                            update.split.alpha.size() +
                            update.split.water.size() +
                            update.split.entity.size() +
                            update.split.localPlayer.size() +
                            update.split.particle.size();
                    }
                    pendingPack.push_back(std::move(update));
                    g_geometryCapturedSectionTotal.fetch_add(
                        1u, std::memory_order_relaxed);
                    g_dirtySectionKeys.erase(spatialKey);
                    ++taken;
                }
                capturedAnything =
                    capturedAnything || !pendingPack.empty();

                moreWorkAvailable =
                    taken > 0u &&
                    !g_dirtySectionKeys.empty() &&
                    ((g_capturedSections.size() - g_capturedSectionsHead) <
                        GEOMETRY_CAPTURE_MAX_QUEUED);
            }

            chunkLockNoteMax(
                g_captureLockMaxHoldNanos,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - captureHoldStart
                    ).count()));

            chunkLockNoteMax(
                g_captureLockMaxWaitNanos,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        captureHoldStart - captureWaitStart
                    ).count()));
        }

        if(!pendingPack.empty())
        {
            for(CapturedSectionUpdate& update : pendingPack)
            {
                packSectionSplitBucketsLocked(update.spatialKey, update.split);
            }

            {
                std::lock_guard<std::mutex> capturedLock(
                    g_capturedSectionsMutex);

                for(CapturedSectionUpdate& update : pendingPack)
                {
                    g_capturedSections.push_back(std::move(update));
                }
            }

            pendingPack.clear();
            capturedAnything = true;
        }

        if(slowestCaptureMs >=
           static_cast<double>(PERF_L10_STALL_LOG_THRESHOLD_MS))
        {
            std::stringstream capsLog;
            capsLog
                << "[RT GEOMETRY CAPSLOW]"
                << " frame=" << g_perfL10FrameSerial
                << " key=" << slowestCaptureKey
                << " ms=" << std::fixed << std::setprecision(2)
                << slowestCaptureMs
                << " Mcycles="
                << (static_cast<double>(slowestCaptureCycles) / 1.0e6)
                << " verts=" << slowestCaptureVertices;
            RTLogFlushed(capsLog.str());
        }

        if(capturedAnything)
        {
            markWorldGeometryDirty();
        }

        if(dirtyKeysAtScan > 0u &&
           (++g_dirtyOrderWakeCount % 3000u) == 0u)
        {
            if(dirtyKeysAtScan > g_dirtyOrderScanKeyMax)
            {
                g_dirtyOrderScanKeyMax = dirtyKeysAtScan;
            }

            if(orderScanMicros > g_dirtyOrderScanMicrosMax)
            {
                g_dirtyOrderScanMicrosMax = orderScanMicros;
            }

            std::stringstream orderLog;

            orderLog
                << "[RT GEOMETRY ORDER]"
                << " mode=" << (nearestOrderUsed ? "nearest" : "hash")
                << " dirty=" << dirtyKeysAtScan
                << " scanMaxKeys=" << g_dirtyOrderScanKeyMax
                << " scanMaxUs=" << g_dirtyOrderScanMicrosMax
                << " captured="
                << g_geometryCapturedSectionTotal.load();

            RTLogFlushed(orderLog.str());
        }

        std::unique_lock<std::mutex> wakeLock(g_geometryCaptureStateMutex);

        // 2026-10-31：有活就立刻接着抓，不再固定睡 4ms（旧行为把吞吐钉死在
        // 4 段 / 4ms ≈ 1000 段/秒，世界加载/传送时队列灌不满）。
        //
        // 没活时也不靠 4ms 轮询：生产者（markWorldGeometryDirty）推进
        // g_captureWorkRevision 并在本线程真的在睡时 notify 一次，4ms 只是兜底
        // —— 万一某条脏路径没经过 markWorldGeometryDirty，最坏退回旧行为。
        const uint64_t seenRevision =
            g_captureWorkRevision.load(std::memory_order_acquire);

        if(moreWorkAvailable && !g_geometryCaptureStop)
        {
            // 2026-10-31：有活也不"立刻重抢锁"，而是让出 ORION_CAPTURE_WAKE_US
            // 微秒（默认 4000 ≈ 改前的 4ms 轮询）。
            //
            // 这个窗口是 capHoldMs/upWaitMs 的主要旋钮，实测 upWaitMs p95：
            //   4000µs(轮询) 16ms  <  250µs 30.1ms  <  0µs 42.2ms
            // 而 queued 恒为 0，说明多抓的那些段并没有人等着用 —— 白付锁成本。
            //
            // 用带谓词的等待而不是 sleep：中途收到 stop 会立即退出。
            g_geometryCaptureWake.wait_for(
                wakeLock,
                std::chrono::microseconds(geometryCaptureWakeMicros()),
                []{ return g_geometryCaptureStop; });
        }
        else if(!g_geometryCaptureStop)
        {
            g_captureWorkerWaiting.store(true, std::memory_order_release);

            // 置位后再判一次谓词：wait_for 的谓词本身会先求值一次，所以
            // "置位前刚好被 notify" 这一边也安全（不过是少睡一觉）。
            g_geometryCaptureWake.wait_for(
                wakeLock,
                std::chrono::milliseconds(4),
                [seenRevision]
                {
                    return g_geometryCaptureStop ||
                           g_captureWorkRevision.load(
                               std::memory_order_acquire) != seenRevision;
                });

            g_captureWorkerWaiting.store(false, std::memory_order_release);
        }
        const bool stopRequested = g_geometryCaptureStop;
        wakeLock.unlock();
        if(stopRequested)
        {
            return;
        }
    }
}
void ensureGeometryCaptureWorker()
{
    if(g_geometryCaptureStarted)
    {
        return;
    }
    const char* syncFlag = std::getenv("ORION_SYNC_GEOMETRY");
    if(syncFlag != nullptr && syncFlag[0] == '1')
    {
        RTLog("[RT GEOMETRY] ORION_SYNC_GEOMETRY=1 -- capture worker disabled");
        return;
    }
    g_geometryCaptureStop = false;
    g_geometryCaptureStarted = true;
    try
    {
        g_geometryCaptureThread = std::thread(geometryCaptureWorkerLoop);
        RTLog("[RT GEOMETRY] capture worker started");
    }
    catch(...)
    {
        g_geometryCaptureStarted = false;
        RTLog("[RT GEOMETRY] capture worker start failed -- sync capture");
    }
}
void stopGeometryCaptureWorker()
{
    if(!g_geometryCaptureStarted)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_geometryCaptureStateMutex);
        g_geometryCaptureStop = true;
    }
    g_geometryCaptureWake.notify_all();
    if(g_geometryCaptureThread.joinable())
    {
        g_geometryCaptureThread.join();
    }
    g_geometryCaptureStarted = false;
    {
        std::lock_guard<std::mutex> capturedLock(g_capturedSectionsMutex);
        resetCapturedSectionsLocked();
    }
    RTLog(
        "[RT GEOMETRY] capture summary: captured=" +
        std::to_string(g_geometryCapturedSectionTotal.load()) +
        " adopted=" + std::to_string(g_geometryAdoptedSectionTotal) +
        " forceResets=" + std::to_string(g_geometryForceResetCount.load()) +
        " sections"
    );
}


bool sameLocalLightPayload(
    const std::vector<LocalLightRecord>& a,
    const std::vector<LocalLightRecord>& b
)
{
    return
        a.size() == b.size() &&
        (
            a.empty() ||
            std::memcmp(
                a.data(),
                b.data(),
                a.size() * sizeof(LocalLightRecord)
            ) == 0
        );
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_setInWorld(
    JNIEnv* env,
    jclass clazz,
    jboolean inWorld)
{
    g_isInWorld = inWorld;

    if(!inWorld)
    {
        g_hasPrevCameraMatrices =
            false;

        g_cameraHistoryValid =
            false;

        g_lastCameraFrame =
            -1;
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_setVisibleSections(
    JNIEnv* env,
    jclass,
    jintArray coords,
    jint count)
{
    std::vector<uint64_t> keys;

    if(env != nullptr && coords != nullptr && count > 0)
    {
        const jsize wanted = static_cast<jsize>(count) * 3;

        if(env->GetArrayLength(coords) >= wanted)
        {
            jint* raw = env->GetIntArrayElements(coords, nullptr);

            if(raw != nullptr)
            {
                keys.reserve(static_cast<size_t>(count));

                for(jint i = 0; i < count; ++i)
                {
                    keys.push_back(
                        packSectionKey(
                            static_cast<int>(raw[i * 3 + 0]),
                            static_cast<int>(raw[i * 3 + 1]),
                            static_cast<int>(raw[i * 3 + 2]),
                            0
                        )
                    );
                }

                env->ReleaseIntArrayElements(coords, raw, JNI_ABORT);
            }
        }
    }

    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    const uint32_t keyCount = static_cast<uint32_t>(keys.size());

    {
        std::lock_guard<std::mutex> lock(g_visibleSectionKeysMutex);
        g_visibleSectionKeys.swap(keys);
    }

    g_visibleSectionKeyCount.store(keyCount);
}
uint64_t packSectionKey(int x, int y, int z, int layerId) {
    return (static_cast<uint64_t>(layerId & 0xF) << 60) |
           (static_cast<uint64_t>(y & 0xFFFF) << 44) |
           (static_cast<uint64_t>(z & 0x3FFFFF) << 22) |
           (static_cast<uint64_t>(x & 0x3FFFFF));
}

// 2026-10-15 uploadChunk 锁内拷贝外移的总闸。ORION_UPLOAD_RAW_MOVE_GATE=0 退回
// 旧行为（raw 副本仍在 g_chunkMutex 内分配+拷贝），用于 A/B 对照与快速回退 ——
// 这样这条改动不需要重新编译就能关掉。
bool uploadRawMoveGateEnabled()
{
    static const bool enabled = []()
    {
        const char* raw = std::getenv("ORION_UPLOAD_RAW_MOVE_GATE");
        return !(raw != nullptr && raw[0] == '0');
    }();

    return enabled;
}

// 2026-10-31：不透明面外扩壳实验闸门（ORION_OPAQUE_SHELL_OFFSET，单位=格，默认 0=关）。
// 验证目标：「方块交界亮缝 = hairline 几何缝漏光」（arm1 索引化 / arm2 POM /
// arm3 GI 均已排除，漏光亮度随游戏内时间走 = 天空/日月光）。机制与紧邻的
// cutout 层 CUTOUT_NORMAL_OFFSET=0.002 分支同款（RTX-Remix 的 shell offset 手法）：
// SOLID 层每个 quad 沿自身法线整体外扩 offset。凹交界处两曲面相交 ⇒ 封死；
// 凸角处开出 ~offset 缺口（亚纹素，实验可接受）。默认 0 = 逐位现状。
float opaqueShellOffsetMeters()
{
    static const float offset = []() -> float
    {
        const char* raw = std::getenv("ORION_OPAQUE_SHELL_OFFSET");
        const float parsed =
            (raw == nullptr || raw[0] == '\0')
                ? 0.0f
                : static_cast<float>(std::atof(raw));
        fprintf(
            stderr,
            "[GEO LAYOUT] opaqueShellOffset=%.4f (ORION_OPAQUE_SHELL_OFFSET)\n",
            parsed
        );
        fflush(stderr);
        return parsed;
    }();
    return offset;
}

// 2026-10-31：摘要去重总闸。ORION_UPLOAD_RAW_STORE=1 完整退回旧的
// "raw 副本 + memcmp" 路径（含内存占用），用于 A/B 对照与零重编译回退。
bool chunkRawStoreEnabled()
{
    static const bool enabled = []()
    {
        const char* raw = std::getenv("ORION_UPLOAD_RAW_STORE");
        return raw != nullptr && raw[0] == '1';
    }();

    return enabled;
}

// 以下四个助手都要求调用方已持有 g_chunkMutex。

bool chunkRawPayloadUnchangedLocked(
    uint64_t chunkKey,
    const uint8_t* rawMemory,
    size_t rawPayloadBytes,
    uint64_t rawDigest)
{
    if(chunkRawStoreEnabled())
    {
        const auto rawIt = g_chunkRawPayloads.find(chunkKey);

        return
            rawIt != g_chunkRawPayloads.end() &&
            rawIt->second.size() == rawPayloadBytes &&
            chunkMemcmpTimed(
                rawIt->second.data(),
                rawMemory,
                rawPayloadBytes,
                g_chunkCmpRaw1MaxNanos
            ) == 0;
    }

    const auto digestIt = g_chunkUploadDigests.find(chunkKey);

    return
        digestIt != g_chunkUploadDigests.end() &&
        digestIt->second.rawBytes == static_cast<uint64_t>(rawPayloadBytes) &&
        digestIt->second.rawHash == rawDigest;
}
bool chunkMeshPayloadUnchangedLocked(
    uint64_t chunkKey,
    const std::vector<Vertex>& repackedVertices,
    uint64_t meshDigest)
{
    if(chunkRawStoreEnabled())
    {
        const auto existingIt = g_chunkMeshes.find(chunkKey);

        return
            existingIt != g_chunkMeshes.end() &&
            sameVertexPayload(existingIt->second, repackedVertices);
    }

    // 摘要必须与几何同时存在：DH/Voxy/清空路径会单独擦 g_chunkMeshes
    // （不经过 uploadChunk），只命中摘要会得到"有摘要没几何"的假去重。
    const auto digestIt = g_chunkUploadDigests.find(chunkKey);

    return
        digestIt != g_chunkUploadDigests.end() &&
        g_chunkMeshes.find(chunkKey) != g_chunkMeshes.end() &&
        digestIt->second.meshVertices ==
            static_cast<uint64_t>(repackedVertices.size()) &&
        digestIt->second.meshHash == meshDigest;
}
void chunkStoreRawPayloadLocked(
    uint64_t chunkKey,
    std::vector<uint8_t>& localRaw,
    const uint8_t* rawMemory,
    size_t rawPayloadBytes)
{
    if(!chunkRawStoreEnabled())
    {
        return;
    }

    if(uploadRawMoveGateEnabled())
    {
        g_chunkRawPayloads[chunkKey] = std::move(localRaw);
    }
    else
    {
        g_chunkRawPayloads[chunkKey] =
            std::vector<uint8_t>(
                rawMemory,
                rawMemory + rawPayloadBytes
            );
    }
}
void chunkStoreUploadDigestLocked(
    uint64_t chunkKey,
    size_t meshVertices,
    uint64_t rawDigest,
    size_t rawPayloadBytes,
    uint64_t meshDigest)
{
    if(chunkRawStoreEnabled())
    {
        return;
    }

    ChunkUploadDigest& digest = g_chunkUploadDigests[chunkKey];

    digest.rawHash = rawDigest;
    digest.rawBytes = static_cast<uint64_t>(rawPayloadBytes);
    digest.meshHash = meshDigest;
    digest.meshVertices = static_cast<uint64_t>(meshVertices);
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadChunk(
        JNIEnv* env,
        jclass clazz,
        jint sectionX,
        jint sectionY,
        jint sectionZ,
        jint layerId,
        jlong vertexPointer,
        jint vertexCount,
        jint vertexStride)
{
    uint64_t chunkKey =
    packSectionKey(
        sectionX,
        sectionY,
        sectionZ,
        layerId
    );

    const uint64_t spatialSectionKey =
        chunkKey &
        SPATIAL_SECTION_KEY_MASK;

    g_chunkUploadCalls.fetch_add(1u, std::memory_order_relaxed);

    if (vertexCount == 0 || vertexPointer == 0 || vertexStride != 28) {
        std::lock_guard<std::timed_mutex> lock(g_chunkMutex);

        bool removedCachedLayer =
            g_chunkMeshes.erase(chunkKey) > 0u;

        if(layerId == 1)
        {
            removedCachedLayer =
                g_cutoutOmmInputs.erase(chunkKey) > 0u ||
                removedCachedLayer;
        }

        g_chunkRawPayloads.erase(chunkKey);
        g_chunkUploadDigests.erase(chunkKey);

        if(removedCachedLayer)
        {
            g_dirtySectionKeys.insert(spatialSectionKey);
            markWorldGeometryDirty();
        }
        return;
    }

    if (vertexCount % 4 != 0) return;

    const uint8_t* const rawMemory =
        reinterpret_cast<const uint8_t*>(vertexPointer);

    const size_t rawPayloadBytes =
        static_cast<size_t>(vertexCount) *
        static_cast<size_t>(vertexStride);

    const uint64_t rawDigest =
        chunkRawStoreEnabled()
            ? 0ull
            : chunkPayloadDigest64(
                  rawMemory,
                  rawPayloadBytes,
                  0x9E3779B97F4A7C15ull);

    {
        ChunkLockTimingGuard chunkLockTiming;

        // mesh 必须存在：DH/Voxy/清空路径会单独擦 g_chunkMeshes，
        // 只命中摘要会得到"有摘要没几何"的假去重。
        if(
            g_chunkMeshes.find(chunkKey) != g_chunkMeshes.end() &&
            chunkRawPayloadUnchangedLocked(
                chunkKey,
                rawMemory,
                rawPayloadBytes,
                rawDigest
            )
        )
        {
            g_chunkUploadDedup.fetch_add(1u, std::memory_order_relaxed);
            return;
        }
    }

    int quadCount = vertexCount / 4;

    const bool indexedSolidQuads =
        layerId == 0u &&
        sectionUsesIndexedGeometry(
            spatialSectionKey | SECTION_AS_CLASS_OPAQUE,
            static_cast<size_t>(quadCount) * 4u);

    int triVertexCount = indexedSolidQuads ? quadCount * 4 : quadCount * 6;

    std::vector<Vertex> repackedVertices(triVertexCount);
    uint32_t rainPuddleCandidateQuads = 0u;
    uint32_t rainPuddleMalformedQuads = 0u;

    for (int q = 0; q < quadCount; ++q) {
        Vertex quadVerts[4];
        uint32_t rainPuddleMarkedVertices = 0u;

        for (int v = 0; v < 4; ++v) {
            const uint8_t* currentRawVertex =
                rawMemory + ((q * 4 + v) * vertexStride);

            float lx = *reinterpret_cast<const float*>(currentRawVertex + 0);
            float ly = *reinterpret_cast<const float*>(currentRawVertex + 4);
            float lz = *reinterpret_cast<const float*>(currentRawVertex + 8);
            quadVerts[v].x = lx + (sectionX * 16.0f);
            quadVerts[v].y = ly + (sectionY * 16.0f);
            quadVerts[v].z = lz + (sectionZ * 16.0f);

            const uint8_t* colorPtr = currentRawVertex + 12;
            quadVerts[v].r = colorPtr[0] / 255.0f;
            quadVerts[v].g = colorPtr[1] / 255.0f;
            quadVerts[v].b = colorPtr[2] / 255.0f;
            quadVerts[v].a = colorPtr[3] / 255.0f;

            quadVerts[v].u = *reinterpret_cast<const float*>(currentRawVertex + 16);
            quadVerts[v].v = *reinterpret_cast<const float*>(currentRawVertex + 20);

            const uint8_t materialMarker =
                *(currentRawVertex + 27);

            const uint32_t rawLightCoords =
                *reinterpret_cast<const uint32_t*>(currentRawVertex + 24);

            const float glassConnectMask =
                static_cast<float>((rawLightCoords >> 8) & 0x1Fu);

            const bool rainPuddleCandidate = materialMarker == 23u;
            rainPuddleMarkedVertices += rainPuddleCandidate ? 1u : 0u;



            if(materialMarker == 1u)
            {
                quadVerts[v].materialId =
                    1.0f;
            }
            else if(
                materialMarker >= 2u &&
                materialMarker <= 22u
            )
            {
                if(materialMarker <= 18u)
                {
                    quadVerts[v].materialId =
                        2.0f;

                    applyStainedGlassDyeToRTVertex(
                        materialMarker,
                        quadVerts[v]
                    );
                }
                else
                {
                    quadVerts[v].materialId =
                        2.0f +
                        0.1f *
                        static_cast<float>(materialMarker - 18u);
                }
            }
            else if(materialMarker == 25u)
            {
                quadVerts[v].materialId =
                    0.25f;
            }
            else if(materialMarker == 24u)
            {
                quadVerts[v].materialId =
                    6.0f;
            }
            else
            {
                quadVerts[v].materialId =
                    0.0f;
            }

            quadVerts[v].pad1 = rainPuddleCandidate ? 0.25f : 0.0f;
            quadVerts[v].pad2 = glassConnectMask;
        }

        if(rainPuddleMarkedVertices == 4u)
        {
            ++rainPuddleCandidateQuads;
        }
        else if(rainPuddleMarkedVertices != 0u)
        {
            ++rainPuddleMalformedQuads;
        }

        if(layerId == 1)
        {
            const float edge1X = quadVerts[1].x - quadVerts[0].x;
            const float edge1Y = quadVerts[1].y - quadVerts[0].y;
            const float edge1Z = quadVerts[1].z - quadVerts[0].z;
            const float edge2X = quadVerts[2].x - quadVerts[0].x;
            const float edge2Y = quadVerts[2].y - quadVerts[0].y;
            const float edge2Z = quadVerts[2].z - quadVerts[0].z;
            const float quadNX = edge1Y * edge2Z - edge1Z * edge2Y;
            const float quadNY = edge1Z * edge2X - edge1X * edge2Z;
            const float quadNZ = edge1X * edge2Y - edge1Y * edge2X;
            const float quadNormalLength =
                std::sqrt(
                    quadNX * quadNX +
                    quadNY * quadNY +
                    quadNZ * quadNZ);

            if(quadNormalLength > 1.0e-6f)
            {
                constexpr float CUTOUT_NORMAL_OFFSET = 0.002f;
                const float offsetX =
                    quadNX / quadNormalLength * CUTOUT_NORMAL_OFFSET;
                const float offsetY =
                    quadNY / quadNormalLength * CUTOUT_NORMAL_OFFSET;
                const float offsetZ =
                    quadNZ / quadNormalLength * CUTOUT_NORMAL_OFFSET;

                for(int v = 0; v < 4; ++v)
                {
                    quadVerts[v].x += offsetX;
                    quadVerts[v].y += offsetY;
                    quadVerts[v].z += offsetZ;
                }
            }
        }

        if(layerId == 0)
        {
            const float opaqueShellOffset = opaqueShellOffsetMeters();
            if(opaqueShellOffset > 0.0f)
            {
                // 2026-10-31 arm5: in-plane dilation (replaces arm4's
                // normal translation). Each corner moves outward along the
                // normalized sum of its two adjacent in-plane edge
                // directions, so every EDGE line shifts outward while the
                // quad stays in its own plane. Coplanar neighbours overlap,
                // concave junctions cross, convex edges overlap -- no notch
                // anywhere (arm4's convex V-grooves are gone).
                int cornerPrev = 3;
                for(int corner = 0; corner < 4; ++corner)
                {
                    const int cornerNext =
                        (corner == 3) ? 0 : (corner + 1);

                    const float toPrevX =
                        quadVerts[cornerPrev].x - quadVerts[corner].x;
                    const float toPrevY =
                        quadVerts[cornerPrev].y - quadVerts[corner].y;
                    const float toPrevZ =
                        quadVerts[cornerPrev].z - quadVerts[corner].z;
                    const float toNextX =
                        quadVerts[cornerNext].x - quadVerts[corner].x;
                    const float toNextY =
                        quadVerts[cornerNext].y - quadVerts[corner].y;
                    const float toNextZ =
                        quadVerts[cornerNext].z - quadVerts[corner].z;

                    const float toPrevLength =
                        std::sqrt(
                            toPrevX * toPrevX +
                            toPrevY * toPrevY +
                            toPrevZ * toPrevZ);
                    const float toNextLength =
                        std::sqrt(
                            toNextX * toNextX +
                            toNextY * toNextY +
                            toNextZ * toNextZ);

                    if(toPrevLength > 1.0e-6f && toNextLength > 1.0e-6f)
                    {
                        const float inwardX =
                            toPrevX / toPrevLength + toNextX / toNextLength;
                        const float inwardY =
                            toPrevY / toPrevLength + toNextY / toNextLength;
                        const float inwardZ =
                            toPrevZ / toPrevLength + toNextZ / toNextLength;
                        const float inwardLength =
                            std::sqrt(
                                inwardX * inwardX +
                                inwardY * inwardY +
                                inwardZ * inwardZ);

                        if(inwardLength > 1.0e-6f)
                        {
                            quadVerts[corner].x -=
                                inwardX / inwardLength * opaqueShellOffset;
                            quadVerts[corner].y -=
                                inwardY / inwardLength * opaqueShellOffset;
                            quadVerts[corner].z -=
                                inwardZ / inwardLength * opaqueShellOffset;
                        }
                    }

                    cornerPrev = corner;
                }
            }
        }

        if(indexedSolidQuads)
        {
            repackedVertices[q * 4 + 0] = quadVerts[0];
            repackedVertices[q * 4 + 1] = quadVerts[1];
            repackedVertices[q * 4 + 2] = quadVerts[2];
            repackedVertices[q * 4 + 3] = quadVerts[3];
        }
        else
        {
            repackedVertices[q * 6 + 0] = quadVerts[0];
            repackedVertices[q * 6 + 1] = quadVerts[1];
            repackedVertices[q * 6 + 2] = quadVerts[2];
            repackedVertices[q * 6 + 3] = quadVerts[0];
            repackedVertices[q * 6 + 4] = quadVerts[2];
            repackedVertices[q * 6 + 5] = quadVerts[3];
        }
    }

    const uint64_t meshDigest =
        chunkRawStoreEnabled()
            ? 0ull
            : chunkPayloadDigest64(
                  repackedVertices.data(),
                  repackedVertices.size() * sizeof(Vertex),
                  0xC2B2AE3D27D4EB4Full);

    if(!g_rainPuddleLoggedGeometry && rainPuddleCandidateQuads != 0u)
    {
        std::ostringstream log;
        log
            << "[RAIN PUDDLE] geometry"
            << " section=(" << sectionX << "," << sectionY << "," << sectionZ
            << ") layer=" << layerId
            << " candidateQuads=" << rainPuddleCandidateQuads
            << " rayHitTriangles=" << (rainPuddleCandidateQuads * 2u)
            << " malformedQuads=" << rainPuddleMalformedQuads;
        RTLog(log.str());
        g_rainPuddleLoggedGeometry = true;
    }

    std::vector<uint8_t> localRaw;
    const uint8_t* rawDedupSource = rawMemory;
    if(uploadRawMoveGateEnabled())
    {
        localRaw.assign(rawMemory, rawMemory + rawPayloadBytes);
        rawDedupSource = localRaw.data();
    }

    {
        ChunkLockTimingGuard chunkLockTiming;

        const auto existingIt =
            g_chunkMeshes.find(chunkKey);

        // raw 载荷判等：摘要模式下是 O(1) 的整数比较（摘要在锁外算好），
        // ORION_UPLOAD_RAW_STORE=1 时仍走原来的 memcmp。
        if(
            chunkRawPayloadUnchangedLocked(
                chunkKey,
                rawDedupSource,
                rawPayloadBytes,
                rawDigest
            )
        )
        {
            g_chunkUploadDedup.fetch_add(1u, std::memory_order_relaxed);
            return;
        }

        bool existingMeshEqual = false;

        if(existingIt != g_chunkMeshes.end())
        {
            const auto meshCmpT0 = std::chrono::steady_clock::now();
            existingMeshEqual =
                chunkMeshPayloadUnchangedLocked(
                    chunkKey,
                    repackedVertices,
                    meshDigest
                );
            chunkCmpNoteMax(
                g_chunkCmpMeshMaxNanos,
                static_cast<uint64_t>(
                    chunkCmpMsSince(meshCmpT0) * 1000000.0
                )
            );
            g_chunkCmpLastVerts.store(
                static_cast<uint64_t>(repackedVertices.size())
            );
            chunkCmpReport();
        }

        if(existingMeshEqual)
        {
            g_chunkUploadDedup.fetch_add(1u, std::memory_order_relaxed);

            chunkStoreRawPayloadLocked(
                chunkKey,
                localRaw,
                rawMemory,
                rawPayloadBytes
            );
            chunkStoreUploadDigestLocked(
                chunkKey,
                repackedVertices.size(),
                rawDigest,
                rawPayloadBytes,
                meshDigest
            );

            return;
        }

        const size_t repackedVertexCount = repackedVertices.size();

        g_chunkMeshes[chunkKey] = std::move(repackedVertices);

        chunkStoreRawPayloadLocked(
            chunkKey,
            localRaw,
            rawMemory,
            rawPayloadBytes
        );
        chunkStoreUploadDigestLocked(
            chunkKey,
            repackedVertexCount,
            rawDigest,
            rawPayloadBytes,
            meshDigest
        );

        g_dirtySectionKeys.insert(spatialSectionKey);
        markWorldGeometryDirty();
    }
}


// packSectionKey stores x in bits 0..21 and z in bits 22..43 as masked 22-bit
// two's-complement fields, so both need an explicit sign extension back.
int32_t sectionKeyColumnX(uint64_t spatialKey)
{
    const uint32_t raw =
        static_cast<uint32_t>(spatialKey & 0x3FFFFFull);

    return static_cast<int32_t>(raw << 10) >> 10;
}
int32_t sectionKeyColumnZ(uint64_t spatialKey)
{
    const uint32_t raw =
        static_cast<uint32_t>((spatialKey >> 22) & 0x3FFFFFull);

    return static_cast<int32_t>(raw << 10) >> 10;
}

// The single source of truth for "what kind of geometry is this instance".
// The two LOD pseudo-section families share one reserved-Y band, so the band
// only says "LOD"; the DH key set separates DH from Voxy inside it.
//
// DH membership is answered from g_dhLodSpatialSnapshot, not from a single
// "current" key: since 2026-10-01 the far field is uploaded per section and
// therefore owns many keys at once.
uint32_t sectionRoleOf(const RTSectionAS& section)
{
    const bool lodSpatialKey =
        (section.sectionKey & VOXY_LOD_RESERVED_Y) == VOXY_LOD_RESERVED_Y;

    if(!lodSpatialKey)
    {
        return SECTION_ROLE_TERRAIN;
    }

    const uint64_t instanceSpatialKey =
        section.sectionKey & SPATIAL_SECTION_KEY_MASK;

    return isDhLodSpatialKey(instanceSpatialKey)
        ? SECTION_ROLE_DH_FAR
        : SECTION_ROLE_VOXY_LOD;
}
