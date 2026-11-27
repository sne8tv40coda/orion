// Orion backend — Geometry 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include <atomic>
#include <thread>
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
                    : "SERHitObjectOnlyNoReorderHint")
                : "fallbackTraceRay"))
        // 2026-11-07：variant 是"实际会不会加载 *_ser.rgen.spv"，与硬件重排提示
        // 解耦（规范只把重排当提示；见 Settings.cpp serRequireReorderHint）。
        << " variant=" << (serShaderVariantWanted() ? 1 : 0)
        << " requireReorder=" << (serRequireReorderHint() ? 1 : 0)
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
namespace
{
// 4 路 FNV 的区间版本：只读 [begin, end)。begin 必须 32B 对齐（分块由下方保证）。
uint64_t hashVertexBytesRange(
    const uint8_t* bytes,
    size_t begin,
    size_t end)
{
    uint64_t h0 = 0x9E3779B97F4A7C15ull;
    uint64_t h1 = 0xC2B2AE3D27D4EB4Full;
    uint64_t h2 = 0x165667B19E3779F9ull;
    uint64_t h3 = 0x27D4EB2F165667C5ull;

    size_t offset = begin;

    while(offset + 32u <= end)
    {
        uint64_t w0 = 0u;
        uint64_t w1 = 0u;
        uint64_t w2 = 0u;
        uint64_t w3 = 0u;

        std::memcpy(&w0, bytes + offset + 0u, sizeof(w0));
        std::memcpy(&w1, bytes + offset + 8u, sizeof(w1));
        std::memcpy(&w2, bytes + offset + 16u, sizeof(w2));
        std::memcpy(&w3, bytes + offset + 24u, sizeof(w3));

        h0 = (h0 ^ w0) * 0x100000001B3ull;
        h1 = (h1 ^ w1) * 0x100000001B3ull;
        h2 = (h2 ^ w2) * 0x100000001B3ull;
        h3 = (h3 ^ w3) * 0x100000001B3ull;

        offset += 32u;
    }

    while(offset + 8u <= end)
    {
        uint64_t word = 0u;
        std::memcpy(&word, bytes + offset, sizeof(word));

        h0 = (h0 ^ word) * 0x100000001B3ull;

        offset += 8u;
    }

    uint64_t tail = 0u;

    for(size_t i = 0u; offset + i < end; ++i)
    {
        tail |= static_cast<uint64_t>(bytes[offset + i]) << (8u * i);
    }

    uint64_t hash = h0;
    hash *= 0x100000001B3ull; hash ^= h1;
    hash *= 0x100000001B3ull; hash ^= h2;
    hash *= 0x100000001B3ull; hash ^= h3;
    hash *= 0x100000001B3ull; hash ^= tail;

    return hash;
}
} // namespace

uint64_t hashVertexPayload(
    const std::vector<Vertex>& payload
)
{
    const uint8_t* const bytes =
        reinterpret_cast<const uint8_t*>(payload.data());

    const size_t totalBytes =
        payload.size() * sizeof(Vertex);

    // 2026-11-12：分块并行。
    // 实测该函数在物品密集区稳定 30.4ms/帧（~107MB ⇒ 3.5GB/s），4 路 FNV 已消除
    // 乘法依赖链，剩下的是单核读吞吐，因此按 32B 对齐切 4 块、3 个 worker + 本线程。
    // 分块规则固定 ⇒ 哈希确定；输出值与旧的串行版本不同，但只用于自比较。
    constexpr size_t kParts = 4u;

    if(totalBytes < (4u << 20))
    {
        uint64_t hash = 0x9E3779B97F4A7C15ull ^
            static_cast<uint64_t>(payload.size());

        hash ^= hashVertexBytesRange(bytes, 0u, totalBytes);
        hash *= 0x100000001B3ull;
        hash ^= hash >> 32u;

        return hash;
    }

    const size_t chunkBytes =
        ((totalBytes / kParts) + 31u) & ~static_cast<size_t>(31u);

    uint64_t part[kParts] = {};
    std::thread workers[kParts - 1u];

    for(size_t partIndex = 0u; partIndex + 1u < kParts; ++partIndex)
    {
        const size_t partBegin = partIndex * chunkBytes;
        const size_t partEnd =
            std::min(partBegin + chunkBytes, totalBytes);

        workers[partIndex] = std::thread(
            [&part, bytes, partIndex, partBegin, partEnd]()
            {
                part[partIndex] =
                    hashVertexBytesRange(bytes, partBegin, partEnd);
            });
    }

    part[kParts - 1u] = hashVertexBytesRange(
        bytes,
        std::min((kParts - 1u) * chunkBytes, totalBytes),
        totalBytes);

    for(std::thread& worker : workers)
    {
        worker.join();
    }

    uint64_t hash = 0x9E3779B97F4A7C15ull ^
        static_cast<uint64_t>(payload.size());

    for(size_t partIndex = 0u; partIndex < kParts; ++partIndex)
    {
        hash ^= part[partIndex];
        hash *= 0x100000001B3ull;
        hash ^= hash >> 29u;
    }

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
// 2026-11-23 CPU批①：实体层判等。语义与「哈希相等 + size 相等」一致（逐位比较，
// 与 FNV 一样把 -0.0/+0.0、NaN 位型差异当作变化），但：
//   · size 不等直接返回，不再先算完整层哈希；
//   · 动画中的层通常首段就不等 ⇒ 首 256KB 探测即退出；
//   · 静止大层 4 路并行 memcmp（每路 1MB 步进检查停止标志），单路吞吐远高于 FNV。
bool sameVertexPayloadParallel(
    const std::vector<Vertex>& a,
    const std::vector<Vertex>& b
)
{
    if(a.size() != b.size())
    {
        return false;
    }

    if(a.empty())
    {
        return true;
    }

    const uint8_t* const pa = reinterpret_cast<const uint8_t*>(a.data());
    const uint8_t* const pb = reinterpret_cast<const uint8_t*>(b.data());
    const size_t totalBytes = a.size() * sizeof(Vertex);

    if(totalBytes >= 16384u)
    {
        constexpr size_t kProbeBytes = 4096u;
        if(std::memcmp(pa, pb, kProbeBytes) != 0)
        {
            return false;
        }
        const size_t midOffset =
            ((totalBytes / 2u) - (kProbeBytes / 2u)) & ~static_cast<size_t>(63u);
        if(std::memcmp(pa + midOffset, pb + midOffset, kProbeBytes) != 0)
        {
            return false;
        }
        if(std::memcmp(
               pa + (totalBytes - kProbeBytes),
               pb + (totalBytes - kProbeBytes),
               kProbeBytes) != 0)
        {
            return false;
        }
    }

    return std::memcmp(pa, pb, totalBytes) == 0;
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
// 2026-11-07f：捕获路径的"冷分配"计数（诊断用）。
// 只在 destination 容量不足、必须扩容时累加。
uint64_t g_captureGrowCount = 0ull;
uint64_t g_captureGrowBytes = 0ull;

// 2026-11-07g：按桶拆分——一次扩容到底发生在哪个桶，决定是池没命中
// 还是只命中了一个偏小的 buffer。
//   0 opaque / 1 alpha / 2 water / 3 ommCutout /
//   4 entity / 5 localPlayer / 6 particle / 7 other
constexpr size_t CAPTURE_GROW_BUCKET_COUNT = 8u;
uint64_t g_captureGrowBucketCount[CAPTURE_GROW_BUCKET_COUNT] = {};
uint64_t g_captureGrowBucketBytes[CAPTURE_GROW_BUCKET_COUNT] = {};

// 2026-11-07h：扩容前 destination 是不是空的。这是"池没命中/拿到空壳"
// 与"命中了但槽偏小"的唯一判据，决定下一步是改取槽策略还是改池布局。
uint64_t g_captureGrowFromEmpty = 0ull;
uint64_t g_captureGrowFromSmall = 0ull;

// 2026-11-13: water layer (layerId == 3) vertices whose vertex material
// marker was not 1 and had to be forced to 1.  See the layer 3 branch of
// geometryCaptureSectionLocked for the full explanation.  After this fix the
// counter must stay 0 on a healthy build -- a non-zero value means the
// upstream vertex writer never emitted marker 1 for that face at all.
uint64_t g_waterLayerMarkerFixVertices = 0ull;

void geometryAppendSectionVertices(
    std::vector<Vertex>& destination,
    const std::vector<Vertex>& source,
    int bucket
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
        const size_t targetCapacity = std::max(requiredSize, geometricCapacity);

        // 2026-11-07f：这次填充必须新分配。
        ++g_captureGrowCount;
        const uint64_t grownBytes =
            static_cast<uint64_t>(
                (targetCapacity - destination.capacity()) * sizeof(Vertex));
        g_captureGrowBytes += grownBytes;

        // 2026-11-07g：分桶归因。
        const size_t bucketSlot =
            (bucket >= 0 &&
             static_cast<size_t>(bucket) < CAPTURE_GROW_BUCKET_COUNT)
                ? static_cast<size_t>(bucket)
                : CAPTURE_GROW_BUCKET_COUNT - 1u;
        ++g_captureGrowBucketCount[bucketSlot];
        g_captureGrowBucketBytes[bucketSlot] += grownBytes;

        if(destination.capacity() == 0u)
        {
            ++g_captureGrowFromEmpty;
        }
        else
        {
            ++g_captureGrowFromSmall;
        }

        destination.reserve(targetCapacity);
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
    packOne(SECTION_AS_CLASS_OMM_LEAVES, split.ommLeaves, split.ommLeavesPacked);
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
    // =======================================================================
    // 2026-11-19 rasterLod（挂载点②，原规格行号 Geometry.cpp:1999/1102）：
    // DH 键不再进 BLAS 捕获 —— 远场改由 dh_lod_raster.comp 光栅投影接管。
    // 本函数是分类路由的唯一实现，三个消费者（捕获线程 / 增量批次 /
    // forceReset）共用 ⇒ 单点门控即全覆盖。空 split 走下游
    // rebuildGeometryClass 的 vertices.empty() 分支 = retire 旧 BLAS、不建新。
    // 旗标关闭时本分支不执行，与改前逐位一致。
    // =======================================================================
    if(g_dhRasterLodEnabled && isDhLodSpatialKey(spatialSectionKey))
    {
        SectionGeometrySplit emptySplit;
        split = std::move(emptySplit);
        return;
    }

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
            geometryAppendSectionVertices(split.localPlayer, layerEntry.second, 5);
        }
        else if(layerEntry.first == HAND_RT_LAYER_KEY)
        {
            geometryAppendSectionVertices(split.hand, layerEntry.second, 7);
            geometryAppendSectionMotion(
                split.handMotion,
                layerMotion,
                layerEntry.second.size()
            );
        }
        else if(isEntityRtLayerKey(layerEntry.first))
        {
            geometryAppendSectionVertices(split.entity, layerEntry.second, 4);
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
            geometryAppendSectionVertices(split.particle, layerEntry.second, 6);
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
            geometryAppendSectionVertices(split.opaque, layerEntry.second, 0);
        }
        else if(layerId == 1u)
        {
            const std::vector<Vertex>& layer = layerEntry.second;
            auto ommIt = g_cutoutOmmInputs.find(layerEntry.first);
            const bool validOmm =
                g_ommNativeEnabled &&
                ommIt != g_cutoutOmmInputs.end() &&
                ommIt->second.subdivisionLevel == 4u &&
                ommIt->second.knownMicroTriangleCount > 0u &&
                ommIt->second.triangleCount == layer.size() / 3u;

            size_t leafVertexCount = 0u;
            size_t glassVertexCount = 0u;
            for(size_t v = 0u; v + 2u < layer.size(); v += 3u)
            {
                if(std::fabs(layer[v].materialId - 0.26f) < 0.005f)
                {
                    leafVertexCount += 3u;
                }
                else if(layer[v].materialId >= 1.95f && layer[v].materialId < 2.5f)
                {
                    glassVertexCount += 3u;
                }
            }

            if(leafVertexCount == 0u && glassVertexCount == 0u)
            {
                if(validOmm)
                {
                    geometryAppendSectionVertices(split.ommCutout, layer, 3);
                    split.ommInput = ommIt->second;
                    split.hasOmmInput = true;
                }
                else
                {
                    geometryAppendSectionVertices(split.alpha, layer, 1);
                }
            }
            else
            {
                constexpr uint32_t kOmmBytesPerTri = 64u;
                const size_t triCount = layer.size() / 3u;
                const size_t leafTriCount = leafVertexCount / 3u;
                const size_t glassTriCount = glassVertexCount / 3u;
                const size_t nonLeafTriCount = triCount - leafTriCount - glassTriCount;
                const size_t nonLeafVertCount = nonLeafTriCount * 3u;

                split.ommLeaves.reserve(split.ommLeaves.size() + leafVertexCount);
                split.alpha.reserve(
                    split.alpha.size() +
                    glassVertexCount +
                    (validOmm ? 0u : nonLeafVertCount));
                if(validOmm)
                {
                    split.ommCutout.reserve(split.ommCutout.size() + nonLeafVertCount);
                    split.ommInput.data.reserve(nonLeafTriCount * kOmmBytesPerTri);
                    split.ommLeavesInput.data.reserve(leafTriCount * kOmmBytesPerTri);
                }

                uint32_t cutoutTris = 0u;
                uint32_t leafTris = 0u;
                for(size_t tri = 0u; tri < triCount; ++tri)
                {
                    const size_t base = tri * 3u;
                    const bool isLeafTri = std::fabs(layer[base].materialId - 0.26f) < 0.005f;
                    const bool isGlassTri =
                        !isLeafTri &&
                        layer[base].materialId >= 1.95f &&
                        layer[base].materialId < 2.5f;
                    if(isGlassTri)
                    {
                        split.alpha.insert(
                            split.alpha.end(),
                            layer.begin() + base,
                            layer.begin() + base + 3u
                        );
                    }
                    else if(isLeafTri)
                    {
                        split.ommLeaves.insert(
                            split.ommLeaves.end(),
                            layer.begin() + base,
                            layer.begin() + base + 3u
                        );
                        if(validOmm)
                        {
                            const size_t ommOff = tri * kOmmBytesPerTri;
                            split.ommLeavesInput.data.insert(
                                split.ommLeavesInput.data.end(),
                                ommIt->second.data.begin() + ommOff,
                                ommIt->second.data.begin() + ommOff + kOmmBytesPerTri
                            );
                            ++leafTris;
                        }
                    }
                    else
                    {
                        std::vector<Vertex>& dst = validOmm ? split.ommCutout : split.alpha;
                        dst.insert(
                            dst.end(),
                            layer.begin() + base,
                            layer.begin() + base + 3u
                        );
                        if(validOmm)
                        {
                            const size_t ommOff = tri * kOmmBytesPerTri;
                            split.ommInput.data.insert(
                                split.ommInput.data.end(),
                                ommIt->second.data.begin() + ommOff,
                                ommIt->second.data.begin() + ommOff + kOmmBytesPerTri
                            );
                            ++cutoutTris;
                        }
                    }
                }

                if(validOmm)
                {
                    if(cutoutTris > 0u)
                    {
                        split.ommInput.triangleCount = cutoutTris;
                        split.ommInput.subdivisionLevel = 4u;
                        split.ommInput.knownMicroTriangleCount = cutoutTris * 256u;
                        split.hasOmmInput = true;
                    }
                    else
                    {
                        split.ommInput.data.clear();
                        split.ommInput.triangleCount = 0u;
                        split.ommInput.subdivisionLevel = 0u;
                        split.ommInput.knownMicroTriangleCount = 0u;
                        split.hasOmmInput = false;
                    }

                    if(leafTris > 0u)
                    {
                        split.ommLeavesInput.triangleCount = leafTris;
                        split.ommLeavesInput.subdivisionLevel = 4u;
                        split.ommLeavesInput.knownMicroTriangleCount = leafTris * 256u;
                        split.hasOmmLeavesInput = true;
                    }
                    else
                    {
                        split.ommLeavesInput.data.clear();
                        split.ommLeavesInput.triangleCount = 0u;
                        split.ommLeavesInput.subdivisionLevel = 0u;
                        split.ommLeavesInput.knownMicroTriangleCount = 0u;
                        split.hasOmmLeavesInput = false;
                    }
                }
            }
        }
        else if(layerId == 3u)
        {
            // =====================================================
            // 2026-11-13: water layer identity overrides the vertex marker.
            //
            // Symptom (user report): a suspended water column / waterfall
            // has a textured, sun-reactive but fully OPAQUE blue side face,
            // while the horizontal bottom/top face of the same water body is
            // correctly transparent.  The only difference between the two in
            // this code base is the vertex material marker: horizontal faces
            // carry marker 1 -> RT_HIT_WATER, vertical faces carry something
            // else -> materialId 0 -> RT_HIT_OPAQUE -> shaded as opaque
            // terrain with the water sprite colour.
            //
            // Why forcing it here is unambiguous: layer 3 is the dedicated
            // water layer.  Routing already sends the WHOLE layer into
            // split.water (SECTION_AS_CLASS_WATER -> WATER hit group), so by
            // construction every triangle of this layer is a water interface.
            // Faces whose marker already is 1 are bit-identical.
            //
            // A/B: revert to the single geometryAppendSectionVertices call.
            // =====================================================
            const size_t waterBegin =
                split.water.size();
            geometryAppendSectionVertices(split.water, layerEntry.second, 2);
            const size_t waterEnd =
                split.water.size();

            uint64_t markerFixed = 0ull;
            for(size_t index = waterBegin; index < waterEnd; ++index)
            {
                if(split.water[index].materialId != 1.0f)
                {
                    split.water[index].materialId = 1.0f;
                    ++markerFixed;
                }
            }

            if(markerFixed > 0ull)
            {
                g_waterLayerMarkerFixVertices += markerFixed;

                // Report at most once per 4096 vertices so the log stays
                // readable while still proving the marker really was missing.
                static uint64_t reportedVertices = 0ull;
                if(g_waterLayerMarkerFixVertices - reportedVertices >= 4096ull)
                {
                    reportedVertices = g_waterLayerMarkerFixVertices;
                    RTLog(
                        "[RT WATER LAYER] forced materialId=1 total=" +
                        std::to_string(g_waterLayerMarkerFixVertices) +
                        " vertices (water layer marker was not 1)");
                }
            }
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

// ---------------------------------------------------------------------------
// 2026-11-07i：捕获结果的帧级 arena（见 OrionInternal.h 的 CaptureArenaSegment）。
//
// 目的：把 split 的持有窗口从"整个 rebuildWorldGeometry"缩短到"一次捕获"。
// 数据在捕获点 memcpy 进 arena 后 split 立刻还池，池的稳态大小就不再被
// 峰值并发钉死（此前实测池只有 13~24 个槽、命中率 80%）。
//
// 分块分配（块 4MB、段不跨块）：块地址稳定，所以 build/staging 期间持有的
// span 不会因为扩容而悬垂。超过一块的段单独分配 bigChunk。
// ---------------------------------------------------------------------------
uint64_t g_captureArenaSegments = 0u;
uint64_t g_captureArenaBytes = 0u;
uint64_t g_captureArenaResets = 0u;
uint64_t g_captureArenaSkipResets = 0u;
uint64_t g_captureArenaChunks = 0u;
uint64_t g_captureArenaEpoch = 1u;
uint64_t g_captureArenaStaleSegments = 0u;

namespace
{
// 2026-11-07k：4MB -> 8MB。一段"顶点 + 运动矢量 + 32B packed"在最大段
// （约 5 万顶点）下会超过 4MB，于是每次都走 bigChunk（new/delete 反复 churn，
// 实测 arenaChunks 累计 374 次）。8MB 覆盖绝大多数段，bigChunk 只留极端兜底。
constexpr size_t CAPTURE_ARENA_CHUNK_BYTES = 8u << 20;

struct CaptureArena
{
    std::vector<std::unique_ptr<uint8_t[]>> chunks;
    std::vector<std::unique_ptr<uint8_t[]>> bigChunks;
    size_t writeChunk = 0u;
    size_t writeOffset = 0u;

    void reset()
    {
        writeChunk = 0u;
        writeOffset = 0u;
        bigChunks.clear();
    }

    void* allocate(size_t bytes)
    {
        if(bytes == 0u)
        {
            return nullptr;
        }

        constexpr size_t ARENA_ALIGNMENT = 64u;
        const size_t aligned = (bytes + ARENA_ALIGNMENT - 1u) &
            ~(ARENA_ALIGNMENT - 1u);

        if(aligned > CAPTURE_ARENA_CHUNK_BYTES)
        {
            std::unique_ptr<uint8_t[]> big(new uint8_t[aligned]);
            void* const pointer = big.get();
            bigChunks.push_back(std::move(big));

            // 诊断口径 = 当前常驻块数（不是累计分配次数）。
            g_captureArenaChunks = chunks.size() + bigChunks.size();
            return pointer;
        }

        if(writeChunk >= chunks.size())
        {
            chunks.emplace_back(new uint8_t[CAPTURE_ARENA_CHUNK_BYTES]);
        }

        if(writeOffset + aligned > CAPTURE_ARENA_CHUNK_BYTES)
        {
            // 段不跨块，换下一块。
            ++writeChunk;
            writeOffset = 0u;

            if(writeChunk >= chunks.size())
            {
                chunks.emplace_back(new uint8_t[CAPTURE_ARENA_CHUNK_BYTES]);
            }
        }

        uint8_t* const pointer = chunks[writeChunk].get() + writeOffset;
        writeOffset += aligned;
        g_captureArenaChunks = chunks.size() + bigChunks.size();
        return pointer;
    }
};

std::mutex g_captureArenaMutex;
CaptureArena g_captureArena;
uint32_t g_captureArenaWriters = 0u;
}

namespace
{
// 2026-11-07k：一次分配、切片拷贝。原先每个容器各自 allocate 一次，
// 一段最多 14 次分配（锁内推进 14 次、读时 14 个互不相邻的区域）。
// 合并成一次后：分配次数 14 -> 1，且整段数据连续，build/staging 的
// 缓存局部性也更好。
constexpr size_t CAPTURE_ARENA_PIECE_ALIGNMENT = 64u;

inline size_t captureArenaAlignedBytes(size_t bytes)
{
    return (bytes + CAPTURE_ARENA_PIECE_ALIGNMENT - 1u) &
        ~(CAPTURE_ARENA_PIECE_ALIGNMENT - 1u);
}

template<typename T>
size_t captureArenaPlanPiece(
    const std::vector<T>& source,
    size_t& totalBytes)
{
    const size_t offset = totalBytes;
    totalBytes += captureArenaAlignedBytes(source.size() * sizeof(T));
    return offset;
}

template<typename T>
void captureArenaCopyPiece(
    const std::vector<T>& source,
    uint8_t* base,
    size_t offset,
    std::span<const T>& out)
{
    if(source.empty())
    {
        out = std::span<const T>{};
        return;
    }

    T* const destination = reinterpret_cast<T*>(base + offset);
    std::memcpy(destination, source.data(), source.size() * sizeof(T));
    out = std::span<const T>(destination, source.size());
}
}

CaptureArenaSegment captureArenaCommit(const SectionGeometrySplit& split)
{
    std::lock_guard<std::mutex> lock(g_captureArenaMutex);

    CaptureArenaSegment segment;
    segment.arenaEpoch = g_captureArenaEpoch;
    segment.ommInput = split.ommInput;
    segment.hasOmmInput = split.hasOmmInput;
    segment.ommLeavesInput = split.ommLeavesInput;
    segment.hasOmmLeavesInput = split.hasOmmLeavesInput;

    size_t totalBytes = 0u;

    const size_t opaqueOffset =
        captureArenaPlanPiece(split.opaque, totalBytes);
    const size_t ommOffset =
        captureArenaPlanPiece(split.ommCutout, totalBytes);
    const size_t ommLeavesOffset =
        captureArenaPlanPiece(split.ommLeaves, totalBytes);
    const size_t alphaOffset =
        captureArenaPlanPiece(split.alpha, totalBytes);
    const size_t waterOffset =
        captureArenaPlanPiece(split.water, totalBytes);
    const size_t entityOffset =
        captureArenaPlanPiece(split.entity, totalBytes);
    const size_t localPlayerOffset =
        captureArenaPlanPiece(split.localPlayer, totalBytes);
    const size_t particleOffset =
        captureArenaPlanPiece(split.particle, totalBytes);
    const size_t handOffset =
        captureArenaPlanPiece(split.hand, totalBytes);

    const size_t entityMotionOffset =
        captureArenaPlanPiece(split.entityMotion, totalBytes);
    const size_t localPlayerMotionOffset =
        captureArenaPlanPiece(split.localPlayerMotion, totalBytes);
    const size_t particleMotionOffset =
        captureArenaPlanPiece(split.particleMotion, totalBytes);
    const size_t handMotionOffset =
        captureArenaPlanPiece(split.handMotion, totalBytes);

    const size_t opaquePackedOffset =
        captureArenaPlanPiece(split.opaquePacked, totalBytes);
    const size_t ommPackedOffset =
        captureArenaPlanPiece(split.ommCutoutPacked, totalBytes);
    const size_t ommLeavesPackedOffset =
        captureArenaPlanPiece(split.ommLeavesPacked, totalBytes);
    const size_t alphaPackedOffset =
        captureArenaPlanPiece(split.alphaPacked, totalBytes);
    const size_t waterPackedOffset =
        captureArenaPlanPiece(split.waterPacked, totalBytes);

    if(totalBytes == 0u)
    {
        ++g_captureArenaSegments;
        return segment;
    }

    uint8_t* const base =
        static_cast<uint8_t*>(g_captureArena.allocate(totalBytes));

    captureArenaCopyPiece(split.opaque, base, opaqueOffset, segment.opaque);
    captureArenaCopyPiece(split.ommCutout, base, ommOffset, segment.ommCutout);
    captureArenaCopyPiece(split.ommLeaves, base, ommLeavesOffset, segment.ommLeaves);
    captureArenaCopyPiece(split.alpha, base, alphaOffset, segment.alpha);
    captureArenaCopyPiece(split.water, base, waterOffset, segment.water);
    captureArenaCopyPiece(split.entity, base, entityOffset, segment.entity);
    captureArenaCopyPiece(
        split.localPlayer, base, localPlayerOffset, segment.localPlayer);
    captureArenaCopyPiece(
        split.particle, base, particleOffset, segment.particle);
    captureArenaCopyPiece(
        split.hand, base, handOffset, segment.hand);

    captureArenaCopyPiece(
        split.entityMotion, base, entityMotionOffset, segment.entityMotion);
    captureArenaCopyPiece(
        split.localPlayerMotion,
        base,
        localPlayerMotionOffset,
        segment.localPlayerMotion);
    captureArenaCopyPiece(
        split.particleMotion, base, particleMotionOffset, segment.particleMotion);
    captureArenaCopyPiece(
        split.handMotion, base, handMotionOffset, segment.handMotion);

    captureArenaCopyPiece(
        split.opaquePacked, base, opaquePackedOffset, segment.opaquePacked);
    captureArenaCopyPiece(
        split.ommCutoutPacked, base, ommPackedOffset, segment.ommCutoutPacked);
    captureArenaCopyPiece(
        split.ommLeavesPacked, base, ommLeavesPackedOffset, segment.ommLeavesPacked);
    captureArenaCopyPiece(
        split.alphaPacked, base, alphaPackedOffset, segment.alphaPacked);
    captureArenaCopyPiece(
        split.waterPacked, base, waterPackedOffset, segment.waterPacked);

    g_captureArenaBytes += totalBytes;

    ++g_captureArenaSegments;

    return segment;
}

void captureArenaBeginWrite()
{
    std::lock_guard<std::mutex> lock(g_captureArenaMutex);
    ++g_captureArenaWriters;
}

void captureArenaEndWrite()
{
    std::lock_guard<std::mutex> lock(g_captureArenaMutex);

    if(g_captureArenaWriters != 0u)
    {
        --g_captureArenaWriters;
    }
}

bool captureArenaTryReset(size_t inFlightSegments)
{
    std::lock_guard<std::mutex> lock(g_captureArenaMutex);

    if(g_captureArenaWriters != 0u || inFlightSegments != 0u)
    {
        ++g_captureArenaSkipResets;
        return false;
    }

    g_captureArena.reset();
    ++g_captureArenaResets;
    ++g_captureArenaEpoch;
    return true;
}

// ---------------------------------------------------------------------------
// 2026-11-06r：split 缓冲池（见 OrionInternal.h 的说明）。
// 池本身用一把独立的小锁：生产者在捕获线程、消费者在渲染线程，
// 都不持有 g_chunkMutex 之外的东西，不会与区块锁形成嵌套。
// ---------------------------------------------------------------------------
namespace
{
std::mutex g_sectionSplitPoolMutex;
std::vector<SectionGeometrySplit> g_sectionSplitPool;
// 2026-11-07h：24 -> 32 -> 96。真实并发 = 捕获线程一次唤醒连捕的段数
// （队列上限 GEOMETRY_CAPTURE_MAX_QUEUED=256，每段独占一个槽直到渲染线程消费）
// + 渲染线程每帧捕获的段数。实测 spMiss 586 / 600 帧、池被打到 0，
// 说明槽位远少于在途段数。单槽上限约 200KB~1MB ⇒ 96 槽 ≈ 20~96MB 常驻，
// 换来的是稳态不再每次捕获都走系统分配器。
constexpr size_t SECTION_SPLIT_POOL_LIMIT = 96u;

// 按总容量打分，用于 max-fit 取槽。
size_t sectionSplitPoolScore(const SectionGeometrySplit& split)
{
    return split.opaque.capacity() +
        split.ommCutout.capacity() +
        split.ommLeaves.capacity() +
        split.alpha.capacity() +
        split.water.capacity() +
        split.entity.capacity() +
        split.localPlayer.capacity() +
        split.particle.capacity();
}
}

// 2026-11-07g：split 池本身的命中统计（此前完全不可见，
// 日志里的 poolHit/poolMiss 是 SectionMemory 显存池，不是这个池）。
// 定义在文件作用域（非匿名 namespace），供 counters 行读取。
uint64_t g_sectionSplitPoolTakeHits = 0u;
uint64_t g_sectionSplitPoolTakeMisses = 0u;
uint64_t g_sectionSplitPoolReturns = 0u;
uint64_t g_sectionSplitPoolReturnRejects = 0u;
uint64_t g_sectionSplitPoolReturnDrops = 0u;
uint64_t g_sectionSplitPoolSize = 0u;
uint64_t g_sectionSplitPoolStarveSkips = 0u;

bool tryTakeSectionSplitFromPool(SectionGeometrySplit& out)
{
    std::lock_guard<std::mutex> lock(g_sectionSplitPoolMutex);

    if(g_sectionSplitPool.empty())
    {
        ++g_sectionSplitPoolTakeMisses;
        g_sectionSplitPoolSize = 0u;
        return false;
    }

    // 2026-11-07g：max-fit 取槽。原先取 LIFO 末尾那个 = 随机 slot，
    // 而 section 顶点数分布很宽（几十 ~ 上万），随机 slot 的容量只有约
    // 2/3 概率够 ⇒ 实测命中率 93% 但每次捕获仍 resize 1.13 次
    // （growBk 合计 1184 / captured 1045）。取容量最大的槽后，大段总命中；
    // 小段用大 buffer 不浪费——这些槽位本来就要常驻复用。
    size_t bestIndex = 0u;
    size_t bestScore = sectionSplitPoolScore(g_sectionSplitPool[0]);
    for(size_t poolIndex = 1u; poolIndex < g_sectionSplitPool.size(); ++poolIndex)
    {
        const size_t score = sectionSplitPoolScore(g_sectionSplitPool[poolIndex]);
        if(score > bestScore)
        {
            bestScore = score;
            bestIndex = poolIndex;
        }
    }

    ++g_sectionSplitPoolTakeHits;
    out = std::move(g_sectionSplitPool[bestIndex]);
    // 注意不要自移动赋值：MSVC 的 vector move assignment 会先释放目标再窃取源，
    // 同一对象上执行会让容量凭空消失。
    if(bestIndex + 1u != g_sectionSplitPool.size())
    {
        g_sectionSplitPool[bestIndex] = std::move(g_sectionSplitPool.back());
    }
    g_sectionSplitPool.pop_back();
    g_sectionSplitPoolSize = g_sectionSplitPool.size();
    return true;
}

SectionGeometrySplit takeSectionSplitFromPool()
{
    SectionGeometrySplit split;

    if(!tryTakeSectionSplitFromPool(split))
    {
        return SectionGeometrySplit{};
    }

    return split;
}
void returnSectionSplitToPool(SectionGeometrySplit&& split)
{
    // 只回收还有容量的：move 之后留下的空壳进池没有意义，反而挤掉有用的。
    const bool hasCapacity =
        split.opaque.capacity() != 0u ||
        split.ommCutout.capacity() != 0u ||
        split.ommLeaves.capacity() != 0u ||
        split.alpha.capacity() != 0u ||
        split.water.capacity() != 0u ||
        split.entity.capacity() != 0u ||
        split.localPlayer.capacity() != 0u ||
        split.particle.capacity() != 0u ||
        split.hand.capacity() != 0u;

    if(!hasCapacity)
    {
        ++g_sectionSplitPoolReturnRejects;
        return;
    }

    split.opaque.clear();
    split.ommCutout.clear();
    split.ommLeaves.clear();
    split.alpha.clear();
    split.water.clear();
    split.entity.clear();
    split.localPlayer.clear();
    split.particle.clear();
    split.hand.clear();
    split.entityMotion.clear();
    split.localPlayerMotion.clear();
    split.particleMotion.clear();
    split.handMotion.clear();
    split.opaquePacked.clear();
    split.ommCutoutPacked.clear();
    split.ommLeavesPacked.clear();
    split.alphaPacked.clear();
    split.waterPacked.clear();
    split.hasOmmInput = false;
    split.ommInput = OmmCutoutInput{};
    split.hasOmmLeavesInput = false;
    split.ommLeavesInput = OmmCutoutInput{};

    std::lock_guard<std::mutex> lock(g_sectionSplitPoolMutex);

    if(g_sectionSplitPool.size() >= SECTION_SPLIT_POOL_LIMIT)
    {
        // 2026-11-07h：池满时"以大换小"。原先直接丢弃——刚训练好的大槽被扔掉，
        // 下一个大段又要冷分配（growSmall 的主要来源之一）。
        size_t weakestIndex = 0u;
        size_t weakestScore = sectionSplitPoolScore(g_sectionSplitPool[0]);
        for(size_t poolIndex = 1u; poolIndex < g_sectionSplitPool.size(); ++poolIndex)
        {
            const size_t score = sectionSplitPoolScore(g_sectionSplitPool[poolIndex]);
            if(score < weakestScore)
            {
                weakestScore = score;
                weakestIndex = poolIndex;
            }
        }

        if(sectionSplitPoolScore(split) <= weakestScore)
        {
            ++g_sectionSplitPoolReturnDrops;
            return;
        }

        ++g_sectionSplitPoolReturns;
        g_sectionSplitPool[weakestIndex] = std::move(split);
        g_sectionSplitPoolSize = g_sectionSplitPool.size();
        return;
    }

    ++g_sectionSplitPoolReturns;
    g_sectionSplitPool.push_back(std::move(split));
    g_sectionSplitPoolSize = g_sectionSplitPool.size();
}

// ---------------------------------------------------------------------------
// 2026-11-07c：g_chunkMutex 持锁时长探针（诊断用）。
//
// 渲染线程的 `pCap`（等区块锁）实测 8.27ms avg / 24.33ms max，是当前最大单项。
// 全项目该锁有 36 个使用点，先覆盖最可能的两处，超 3ms 才打。
// ---------------------------------------------------------------------------
namespace
{
struct ChunkMutexHoldProbe
{
    const char* site;
    std::chrono::steady_clock::time_point begin;

    explicit ChunkMutexHoldProbe(const char* name)
        : site(name), begin(std::chrono::steady_clock::now())
    {
    }

    ~ChunkMutexHoldProbe()
    {
        const double heldMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();

        if(heldMs < 3.0)
        {
            return;
        }

        RTLog(
            std::string("[CHUNK LOCK] site=") + site +
            " held=" + std::to_string(heldMs) +
            " grow=" + std::to_string(g_captureGrowCount) +
            " growMB=" + std::to_string(
                static_cast<double>(g_captureGrowBytes) / 1048576.0));
    }
};
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
    //
    // 2026-11-07i：元素改为 PendingCapture——split 只在"填 → 打包 → commit"
    // 这段时间里活着，之后立刻还池；队列里传的是 arena 视图。
    struct PendingCapture
    {
        uint64_t spatialKey = 0u;
        SectionGeometrySplit split;
        CaptureArenaSegment segment;
    };
    std::vector<PendingCapture> pendingPack;

    for(;;)
    {
        bool capturedAnything = false;
        bool moreWorkAvailable = false;
        // 2026-11-07h：本轮唤醒因 split 池空而中止（不再冷分配）。
        bool poolStarved = false;
        bool nearestOrderUsed = false;
        size_t dirtyKeysAtScan = 0u;
        uint64_t orderScanMicros = 0u;
        double slowestCaptureMs = 0.0;
        uint64_t slowestCaptureKey = 0ull;
        size_t slowestCaptureVertices = 0u;
        uint64_t slowestCaptureCycles = 0ull;
        const auto captureWaitStart = std::chrono::steady_clock::now();
        {
            // 2026-11-23 锁竞争批⑦：声明在 lock_guard 之前 ⇒ 解锁之后才恢复优先级。
            const ScopedLockHolderPriorityBoost captureLockBoost;
            std::lock_guard<std::timed_mutex> chunkLock(g_chunkMutex);
            // 2026-11-07c：必须在 lock_guard 之后声明，析构顺序保证它先于解锁。
            const ChunkMutexHoldProbe chunkHoldProbe("capture");
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

                    // 2026-11-07h：池空时不冷分配，直接结束本轮唤醒，把 dirty key
                    // 留给 4ms 后的下一次唤醒（下次唤醒时渲染线程多半已归还）。
                    // 一次冷分配 ≈ 200KB 新页，是 capHoldMs 的主要成分。
                    SectionGeometrySplit pooledSplit;

                    // 兜底：连续 32 次唤醒都取不到槽（在途 split 长期占满池，
                    // 例如刚进世界时捕获线程一次唤醒连捕上百段），允许一次
                    // 冷分配，避免世界加载被无限期饿住。
                    static uint32_t sPoolStarveWakes = 0u;

                    if(!tryTakeSectionSplitFromPool(pooledSplit))
                    {
                        ++g_sectionSplitPoolStarveSkips;

                        if(++sPoolStarveWakes < 32u)
                        {
                            poolStarved = true;
                            break;
                        }

                        sPoolStarveWakes = 0u;
                    }
                    else
                    {
                        sPoolStarveWakes = 0u;
                    }

                    PendingCapture update;
                    update.spatialKey = spatialKey;
                    // 2026-11-06r：从池里取热 buffer，避免每段新建 vector 的缺页。
                    update.split = std::move(pooledSplit);
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
                            update.split.ommLeaves.size() +
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
                    !poolStarved &&
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
            for(PendingCapture& update : pendingPack)
            {
                packSectionSplitBucketsLocked(update.spatialKey, update.split);
            }

            // 2026-11-07i：搬进 arena 后立刻还池。split 的持有窗口到此为止，
            // 不再横跨到渲染线程消费完（那是池稳态只有 13~24 个槽的根因）。
            captureArenaBeginWrite();

            for(PendingCapture& update : pendingPack)
            {
                update.segment = captureArenaCommit(update.split);
                returnSectionSplitToPool(std::move(update.split));
            }

            {
                std::lock_guard<std::mutex> capturedLock(
                    g_capturedSectionsMutex);

                for(PendingCapture& update : pendingPack)
                {
                    CapturedSectionUpdate entry;
                    entry.spatialKey = update.spatialKey;
                    entry.segment = update.segment;
                    g_capturedSections.push_back(std::move(entry));
                }
            }

            captureArenaEndWrite();

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
    registerChunkColumnKeyLocked(chunkKey);
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
    registerChunkColumnKeyLocked(chunkKey);
}

static constexpr bool kWATER_DEDUP_BACKWARD_QUADS = true;
static std::atomic<uint64_t> g_waterBackwardQuadsDropped{0ull};
static std::atomic<uint64_t> g_waterBackwardQuadsLogged{0ull};

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
        // 2026-11-15：这条路径会**擦掉**该段的缓存几何。Java 侧上传成功后会把
        // 哈希记进 SectionUploadDedup，之后同内容不再重发 ⇒ 若这里误判（例如
        // 某条上传路径的 stride 不是 28），表现就是"固定一区块大小的永久洞"。
        // 以前完全静默，现在带坐标留痕（限频）。
        {
            static uint32_t sUploadEraseLogCount = 0u;

            if((++sUploadEraseLogCount % 50u) == 1u)
            {
                RTLog(
                    "[CHUNK UPLOAD ERASE] section=(" +
                    std::to_string(sectionX) + "," +
                    std::to_string(sectionY) + "," +
                    std::to_string(sectionZ) + ") layer=" +
                    std::to_string(layerId) + " count=" +
                    std::to_string(vertexCount) + " ptr=" +
                    std::to_string(vertexPointer) + " stride=" +
                    std::to_string(vertexStride) + " total=" +
                    std::to_string(sUploadEraseLogCount));
            }
        }

        std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
        const ChunkMutexHoldProbe chunkHoldProbe("uploadErase");

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

    if (vertexCount % 4 != 0)
    {
        // 2026-11-15：以前这里是**静默 return** —— 不存几何、不标脏、不打日志。
        // 而 Java 侧（SectionCompilerMixin）在上传返回后会把这个内容记进
        // SectionUploadDedup，之后同内容被 shouldSkip 拦住不再重发 ⇒ 该段
        // 永远拿不到几何，表现就是"固定一区块大小的永久洞"（用户 2026-11-15
        // 报告）。这里改为：留痕 + 重新标脏（至少让它有机会用旧数据补上），
        // 便于一局内定位到底是哪条上传路径送来了非 4 倍数顶点数。
        {
            static uint32_t sMod4RejectLogCount = 0u;

            if((++sMod4RejectLogCount % 20u) == 1u)
            {
                RTLog(
                    "[CHUNK UPLOAD REJECT] reason=mod4 section=(" +
                    std::to_string(sectionX) + "," +
                    std::to_string(sectionY) + "," +
                    std::to_string(sectionZ) + ") layer=" +
                    std::to_string(layerId) + " count=" +
                    std::to_string(vertexCount) + " stride=" +
                    std::to_string(vertexStride) + " total=" +
                    std::to_string(sMod4RejectLogCount));
            }
        }

        {
            std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
            g_dirtySectionKeys.insert(spatialSectionKey);
            markWorldGeometryDirty();
        }

        return;
    }

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

    const auto sameVertexPos = [](const Vertex& a, const Vertex& b) -> bool
    {
        return std::fabs(a.x - b.x) <= 1.0e-5f &&
               std::fabs(a.y - b.y) <= 1.0e-5f &&
               std::fabs(a.z - b.z) <= 1.0e-5f;
    };

    size_t writeQuad = 0u;
    bool hasPrevWaterQuad = false;
    Vertex prevWaterQuad[4]{};
    uint64_t droppedWaterQuadsInChunk = 0ull;

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
            else if(materialMarker == 26u)
            {
                quadVerts[v].materialId =
                    0.26f;
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

        const bool isGlassFamilyQuad =
            quadVerts[0].materialId >= 1.95f && quadVerts[0].materialId < 2.5f;
        if(layerId == 1 && !isGlassFamilyQuad)
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

        const bool isWaterQuad =
            !indexedSolidQuads &&
            (layerId == 3 ||
             (quadVerts[0].materialId == 1.0f &&
              quadVerts[1].materialId == 1.0f &&
              quadVerts[2].materialId == 1.0f &&
              quadVerts[3].materialId == 1.0f));

        if(kWATER_DEDUP_BACKWARD_QUADS && isWaterQuad)
        {
            const bool isReversedDuplicate =
                hasPrevWaterQuad &&
                ((sameVertexPos(quadVerts[0], prevWaterQuad[0]) &&
                  sameVertexPos(quadVerts[1], prevWaterQuad[3]) &&
                  sameVertexPos(quadVerts[2], prevWaterQuad[2]) &&
                  sameVertexPos(quadVerts[3], prevWaterQuad[1])) ||
                 (sameVertexPos(quadVerts[0], prevWaterQuad[3]) &&
                  sameVertexPos(quadVerts[1], prevWaterQuad[2]) &&
                  sameVertexPos(quadVerts[2], prevWaterQuad[1]) &&
                  sameVertexPos(quadVerts[3], prevWaterQuad[0])));

            if(isReversedDuplicate)
            {
                ++droppedWaterQuadsInChunk;
                hasPrevWaterQuad = false;
                continue;
            }

            for(int v = 0; v < 4; ++v)
            {
                prevWaterQuad[v] = quadVerts[v];
            }
            hasPrevWaterQuad = true;
        }
        else
        {
            hasPrevWaterQuad = false;
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
            repackedVertices[writeQuad * 6u + 0u] = quadVerts[0];
            repackedVertices[writeQuad * 6u + 1u] = quadVerts[1];
            repackedVertices[writeQuad * 6u + 2u] = quadVerts[2];
            repackedVertices[writeQuad * 6u + 3u] = quadVerts[0];
            repackedVertices[writeQuad * 6u + 4u] = quadVerts[2];
            repackedVertices[writeQuad * 6u + 5u] = quadVerts[3];
            ++writeQuad;
        }
    }

    if(!indexedSolidQuads && writeQuad != static_cast<size_t>(quadCount))
    {
        repackedVertices.resize(writeQuad * 6u);
    }

    if(droppedWaterQuadsInChunk > 0ull)
    {
        const uint64_t totalDropped =
            g_waterBackwardQuadsDropped.fetch_add(
                droppedWaterQuadsInChunk,
                std::memory_order_relaxed
            ) + droppedWaterQuadsInChunk;
        uint64_t prevLogged =
            g_waterBackwardQuadsLogged.load(std::memory_order_relaxed);
        if(prevLogged == 0ull || totalDropped - prevLogged >= 65536ull)
        {
            if(g_waterBackwardQuadsLogged.compare_exchange_strong(
                   prevLogged,
                   totalDropped,
                   std::memory_order_relaxed))
            {
                RTLog(
                    "[WATER DEDUP] dropped coplanar backward water quads total=" +
                    std::to_string(totalDropped) +
                    " (lastChunk=" +
                    std::to_string(droppedWaterQuadsInChunk) +
                    ")");
            }
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
        registerChunkColumnKeyLocked(chunkKey);

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
    // 2026-11-19 rasterLod（挂载点④，原规格行号 Sections.cpp:3962 /
    // Geometry.cpp:3054）：DH 角色不再入 binding 19 角色表 —— 由挂载点②
    // （geometryCaptureSectionLocked 跳 DH 键）结构性保证：DH 键的 BLAS
    // 根本不会被创建，writeSectionRoleSlot 因此不会被 DH 节调用，本函数
    // 不会遇到 DH 键。旗标关闭时行为与改前逐位一致（本注释不改变行为）。
    if((section.sectionKey & ~SPATIAL_SECTION_KEY_MASK) == SECTION_AS_CLASS_HAND)
    {
        return SECTION_ROLE_HAND;
    }

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
