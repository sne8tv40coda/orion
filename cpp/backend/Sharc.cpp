// Orion backend — Sharc 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"



// =====================================================
// 2026-10-11 SHaRC（Spatially Hashed Radiance Cache，NVIDIA RTX SDK SHARC v1.8.3）
//
// 接的是 bindings 65..68（见 createRTDescriptorSetLayout 里的说明）。四块缓冲都是
// device-local，按 v1.8.3 默认布局 40B/元素 + lock 4B/元素：
//   hashEntries 8B | accumulation 16B | resolved 16B | lock 4B  ⇒ 44B/元素
// 元素数默认 2^22 = 4M ⇒ 176MB，**一次性给足、之后不再改变**（元素数是哈希网格
// 分辨率的一部分，扩容等于丢掉整个缓存，没有收益）。
//
// ⚠ 初始清零**不在这里做**：世界初始化路径里插一次性 submit + vkQueueWaitIdle 会
// 阻塞渲染线程（历史上那次 53.3ms 的停顿就是这么来的）。改为在**第一个启用 SHaRC
// 的帧**把 vkCmdFillBuffer 录进本帧 cmd；在那之前 g_sharcZeroed=false，
// 着色器模式被强制为 0（完全不碰缓存）。
//
// 运行时开关照 perfTimestampsEnabled 的写法：缓存 getenv，只读一次。
//   ORION_SHARC=1            启用 SHaRC（默认关；关时连那 176MB 都不分配）
//   ORION_SHARC_SCALE=12     体素尺度（HashGridParameters::sceneScale，默认 12）
//   ORION_SHARC_ENTRIES=21   元素数的 2 的幂指数（默认 22 = 4M；21 = 2M/88MB）
// 这些开关后续会镜像到设置界面（RTXSettingsConfig → NativeVulkan → UBO）。
// 2026-10-28：从"只读一次的 getenv 缓存"改成三级解析，与 directLightMode 同构：
//   ORION_SHARC 显式 1 → 强制开；显式 0 → 强制关（保留实验/回退入口）；
//   未设 → 跟设置页的「间接光模式」（g_indirectModeOverride == 1）。
// 之所以必须能读到设置项：SHaRC 模式下要 update + query + 缓冲一起开。
// ⚠ 仍**不是热切换**：四块缓冲（176MB）与第二条 RT 管线在 native init 时创建。
//   运行时切到 1 只会让 shader 拿到 sharcMode()==0，查询不生效；不会崩，
//   因为所有缓存访问都与 sharcMode() 同源门控（见 rgen:25484 的教训）。
int sharcEnvState()
{
    static const int cached = []() -> int
    {
        const char* raw = std::getenv("ORION_SHARC");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 0;
        }
        return raw[0] != '0' ? 1 : 2;
    }();
    return cached;
}
bool sharcEnabled()
{
    const int env = sharcEnvState();

    if(env == 1)
    {
        return true;
    }

    if(env == 2)
    {
        return false;
    }

    return g_indirectModeOverride.load(std::memory_order_relaxed) == 1;
}
float sharcSceneScale()
{
    static const float cached = []()
    {
        const char* raw = std::getenv("ORION_SHARC_SCALE");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 50.0f;
        }

        const float parsed = static_cast<float>(std::atof(raw));
        return parsed > 0.0f ? parsed : 50.0f;
    }();

    return cached;
}
uint32_t sharcEntryCount()
{
    static const uint32_t cached = []()
    {
        const char* raw = std::getenv("ORION_SHARC_ENTRIES");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 22u;
        }
        const int parsed = std::atoi(raw);
        if(parsed < 18)
        {
            return 18u;
        }
        if(parsed > 24)
        {
            return 24u;
        }

        return static_cast<uint32_t>(parsed);
    }();

    return cached;
}
// ---------------------------------------------------------------------------
// 2026-10-11 5b-1：Resolve pass 与 update 开关
//
// ORION_SHARC_UPDATE=0 可以单独关掉 update（保留 Resolve pass 与清零）—— 用来把
// "update 的成本"与"Resolve 的成本"分开量。默认跟随 ORION_SHARC。
// ---------------------------------------------------------------------------
bool sharcUpdateEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_SHARC_UPDATE");

        if(raw == nullptr || raw[0] == '\0')
        {
            return sharcEnabled();
        }

        return raw[0] != '0';
    }();

    return cached;
}

// ORION_SHARC_QUERY=0 单独关掉 query（保留 update 与 Resolve）—— 用来把"query
// 本身带来的变化"与"缓存被写入"分开：3（全开）→ 4（只 update）的差就是 query 的贡献。
bool sharcQueryEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_SHARC_QUERY");

        if(raw == nullptr || raw[0] == '\0')
        {
            return sharcEnabled();
        }

        return raw[0] != '0';
    }();

    return cached;
}

// ORION_SHARC_AB=1：在**同一局里交替** "query 开（模式 3）" 与 "query 关（模式 4）"，
// 每 120 帧一换，换臂时打一行 [SHARC AB] —— 目的是把"要跑两局才能对比"变成"跑一局
// 同时拿到两臂"，并且能直接与 [PERF L10 GPU] 的窗口对上（靠日志时间戳）。
// 这是照本工程既有的 SER A/B（perfSerAb 每窗口翻转）的做法，不是新机制。
bool sharcAbEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_SHARC_AB");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();

    return cached;
}

// 模式 lane 的唯一生产者（CameraUBO.sharcCacheParams.w）。档位表见 sharc_ubo.glsl：
//   0 = 一行缓存都不许碰（未启用 / 还没清零）
//   1 = 5a 寻址探针（一次性）
//   2 = 只 query
//   3 = update + query
//   4 = 只 update
// ⚠ 清零之前绝不能进 2/3/4：Resolve 会把未初始化数据当成已有样本并进 resolved。
uint32_t sharcModeLane()
{
    if(g_sharcProbeStage == 1u)
    {
        return 1u;
    }

    if(!g_sharcZeroed || !g_sharcBuffersReady)
    {
        return 0u;
    }

    const bool sharcUpdate = sharcUpdateEnabled();
    const bool sharcQuery = sharcQueryEnabled();

    if(sharcAbEnabled() && sharcUpdate)
    {
        const uint32_t sharcArm =
            static_cast<uint32_t>(
                (g_perfL10FrameSerial / SHARC_AB_FRAMES_PER_ARM) % 2u);

        if(sharcArm != g_sharcAbLoggedArm)
        {
            g_sharcAbLoggedArm = sharcArm;

            char line[200];
            _snprintf_s(
                line, sizeof(line), _TRUNCATE,
                "[SHARC AB] arm=%s mode=%u frame=%llu"
                "（A = query 开 / B = query 关，update 两臂都在跑）",
                sharcArm == 0u ? "A" : "B",
                sharcArm == 0u ? 3u : 4u,
                static_cast<unsigned long long>(g_perfL10FrameSerial));
            RTLogFlushed(line);
        }

        return sharcArm == 0u ? 3u : 4u;
    }

    if(sharcUpdate && sharcQuery)
    {
        return 3u;
    }

    if(sharcUpdate)
    {
        return 4u;
    }

    return sharcQuery ? 2u : 0u;
}




// =====================================================
// 2026-10-23：TransparencyHint 的描述符（binding 71）
//
// 用 handle 缓存：图在分辨率变化时销毁重建，句柄一变就重写；稳态每帧零成本。
// 放在 createRRGuideImage 之前，是因为 createRRGBufferImages 与本函数同属
// "建 RR 资源"这一层，就近更容易看出两者的生命周期是一致的。
// =====================================================
void updateRRTransparencyHintRayDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_rrTransparencyHintView == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo info{};
    info.imageView = g_rrTransparencyHintView;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 71;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &info;

    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
}
void createRRGuideImage(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout)
{
    VkImageCreateInfo ci{
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
    };


    ci.imageType =
        VK_IMAGE_TYPE_2D;

    ci.format =
        format;

    ci.extent =
    {
        width,
        height,
        1
    };

    ci.mipLevels = 1;
    ci.arrayLayers = 1;

    ci.samples =
        VK_SAMPLE_COUNT_1_BIT;

    ci.tiling =
        VK_IMAGE_TILING_OPTIMAL;


    ci.usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;


    ci.initialLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;


    check(
        vkCreateImage(
            g_device,
            &ci,
            nullptr,
            &image
        ),
        "create RR guide image failed"
    );


    VkMemoryRequirements mr{};

    vkGetImageMemoryRequirements(
        g_device,
        image,
        &mr
    );


    VkMemoryAllocateInfo ai{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };


    ai.allocationSize =
        mr.size;

    ai.memoryTypeIndex =
        findMemoryType(
            g_physicalDevice,
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );


    check(
        vkAllocateMemory(
            g_device,
            &ai,
            nullptr,
            &memory
        ),
        "allocate RR guide image memory failed"
    );


    check(
        vkBindImageMemory(
            g_device,
            image,
            memory,
            0
        ),
        "bind RR guide image memory failed"
    );


    VkImageViewCreateInfo vi{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
    };


    vi.image =
        image;

    vi.viewType =
        VK_IMAGE_VIEW_TYPE_2D;

    vi.format =
        format;


    vi.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = 1;

    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 1;


    check(
        vkCreateImageView(
            g_device,
            &vi,
            nullptr,
            &view
        ),
        "create RR guide image view failed"
    );


    layout =
        VK_IMAGE_LAYOUT_UNDEFINED;
}



void updateRRGBufferDescriptors()
{
    if(
        g_rayTracingDescriptorSet ==
            VK_NULL_HANDLE ||

        g_rrDiffuseAlbedoView ==
            VK_NULL_HANDLE ||

        g_rrSpecularAlbedoView ==
            VK_NULL_HANDLE ||

        g_rrNormalRoughnessView ==
            VK_NULL_HANDLE ||

        g_rrSpecularHitDistanceView ==
            VK_NULL_HANDLE ||

        g_rrStableRadianceView ==
            VK_NULL_HANDLE
    )
    {
        return;
    }


    VkDescriptorImageInfo infos[5]{};


    infos[0].imageView =
        g_rrDiffuseAlbedoView;

    infos[1].imageView =
        g_rrSpecularAlbedoView;

    infos[2].imageView =
        g_rrNormalRoughnessView;

    infos[3].imageView =
        g_rrSpecularHitDistanceView;

    infos[4].imageView =
        g_rrStableRadianceView;


    for(uint32_t i = 0; i < 5; ++i)
    {
        infos[i].imageLayout =
            VK_IMAGE_LAYOUT_GENERAL;
    }


    const uint32_t bindings[5] =
    {
        13,
        14,
        15,
        16,
        54
    };


    VkWriteDescriptorSet writes[5]{};


    for(uint32_t i = 0; i < 5; ++i)
    {
        writes[i].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

        writes[i].dstSet =
            g_rayTracingDescriptorSet;

        writes[i].dstBinding =
            bindings[i];

        writes[i].dstArrayElement =
            0;

        writes[i].descriptorCount =
            1;

        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        writes[i].pImageInfo =
            &infos[i];
    }


    vkUpdateDescriptorSets(
        g_device,
        5,
        writes,
        0,
        nullptr
    );


    std::cerr
        << "[RR GBUFFER DESCRIPTOR]"
        << " bindings=13,14,15,16,54"
        << std::endl;
}



// The display transform's half of the analytic-pane handshake.  Written both
// next to each set's own update and from createRRGBufferImages, because the
// sets are built before the pane image and its sampler exist.  Safe to call at
// any time: it returns without touching Vulkan until all three handles are
// live, and re-writing the same binding is idempotent.
void writeStableRadianceDisplayBinding(
    VkDescriptorSet set,
    const char* label)
{
    (void)label;

    if(
        set == VK_NULL_HANDLE ||
        g_rrStableRadianceView == VK_NULL_HANDLE ||
        g_rrStableRadianceSampler == VK_NULL_HANDLE
    )
    {
        return;
    }

    VkDescriptorImageInfo info{};
    info.sampler = g_rrStableRadianceSampler;
    info.imageView = g_rrStableRadianceView;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
    };

    write.dstSet = set;
    write.dstBinding = 2;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;

    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
}
void createRRGBufferImages(
    uint32_t width,
    uint32_t height)
{
    const bool alreadyValid =
        g_rrDiffuseAlbedoImage !=
            VK_NULL_HANDLE &&

        g_rrSpecularAlbedoImage !=
            VK_NULL_HANDLE &&

        g_rrNormalRoughnessImage !=
            VK_NULL_HANDLE &&

        g_rrSpecularHitDistanceImage !=
            VK_NULL_HANDLE &&

        g_rrStableRadianceImage !=
            VK_NULL_HANDLE &&

        g_rrTransparencyHintImage !=
            VK_NULL_HANDLE &&

        g_rrGBufferWidth ==
            width &&

        g_rrGBufferHeight ==
            height;


    if(alreadyValid)
    {
        return;
    }


    const bool hasOldResources =
        g_rrDiffuseAlbedoImage !=
            VK_NULL_HANDLE ||

        g_rrSpecularAlbedoImage !=
            VK_NULL_HANDLE ||

        g_rrNormalRoughnessImage !=
            VK_NULL_HANDLE ||

        g_rrSpecularHitDistanceImage !=
            VK_NULL_HANDLE ||

        g_rrStableRadianceImage !=
            VK_NULL_HANDLE ||

        g_rrTransparencyHintImage !=
            VK_NULL_HANDLE;


    if(hasOldResources)
    {
        check(
            FGDeviceWaitIdle(
                g_device
            ),
            "wait device before RR GBuffer recreate failed"
        );
    }


    destroyHistoryImage(
        g_rrDiffuseAlbedoImage,
        g_rrDiffuseAlbedoMemory,
        g_rrDiffuseAlbedoView,
        g_rrDiffuseAlbedoLayout
    );


    destroyHistoryImage(
        g_rrSpecularAlbedoImage,
        g_rrSpecularAlbedoMemory,
        g_rrSpecularAlbedoView,
        g_rrSpecularAlbedoLayout
    );


    destroyHistoryImage(
        g_rrNormalRoughnessImage,
        g_rrNormalRoughnessMemory,
        g_rrNormalRoughnessView,
        g_rrNormalRoughnessLayout
    );


    destroyHistoryImage(
        g_rrSpecularHitDistanceImage,
        g_rrSpecularHitDistanceMemory,
        g_rrSpecularHitDistanceView,
        g_rrSpecularHitDistanceLayout
    );


    destroyHistoryImage(
        g_rrStableRadianceImage,
        g_rrStableRadianceMemory,
        g_rrStableRadianceView,
        g_rrStableRadianceLayout
    );


    destroyHistoryImage(
        g_rrTransparencyHintImage,
        g_rrTransparencyHintMemory,
        g_rrTransparencyHintView,
        g_rrTransparencyHintLayout
    );


    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_rrDiffuseAlbedoImage,
        g_rrDiffuseAlbedoMemory,
        g_rrDiffuseAlbedoView,
        g_rrDiffuseAlbedoLayout
    );


    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_rrSpecularAlbedoImage,
        g_rrSpecularAlbedoMemory,
        g_rrSpecularAlbedoView,
        g_rrSpecularAlbedoLayout
    );


    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_rrNormalRoughnessImage,
        g_rrNormalRoughnessMemory,
        g_rrNormalRoughnessView,
        g_rrNormalRoughnessLayout
    );


    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R32_SFLOAT,
        g_rrSpecularHitDistanceImage,
        g_rrSpecularHitDistanceMemory,
        g_rrSpecularHitDistanceView,
        g_rrSpecularHitDistanceLayout
    );


    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_rrStableRadianceImage,
        g_rrStableRadianceMemory,
        g_rrStableRadianceView,
        g_rrStableRadianceLayout
    );


    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R8_UNORM,
        g_rrTransparencyHintImage,
        g_rrTransparencyHintMemory,
        g_rrTransparencyHintView,
        g_rrTransparencyHintLayout
    );


    if(g_rrStableRadianceSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(
            g_device,
            g_rrStableRadianceSampler,
            nullptr
        );

        g_rrStableRadianceSampler = VK_NULL_HANDLE;
    }

    VkSamplerCreateInfo stableSamplerInfo{
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO
    };

    stableSamplerInfo.magFilter = VK_FILTER_LINEAR;
    stableSamplerInfo.minFilter = VK_FILTER_LINEAR;
    stableSamplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    stableSamplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    stableSamplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    stableSamplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    stableSamplerInfo.minLod = 0.0f;
    stableSamplerInfo.maxLod = 0.0f;

    check(
        vkCreateSampler(
            g_device,
            &stableSamplerInfo,
            nullptr,
            &g_rrStableRadianceSampler
        ),
        "create RR stable radiance sampler failed"
    );


    g_rrGBufferWidth =
        width;

    g_rrGBufferHeight =
        height;


    updateRRGBufferDescriptors();


    writeStableRadianceDisplayBinding(
        g_sdrPostDescriptorSet,
        "sdrpost"
    );

    writeStableRadianceDisplayBinding(
        g_hdrEncodeDescriptorSet,
        "hdrencode"
    );


    std::cerr
        << "[RR GBUFFER] created "
        << width
        << "x"
        << height
        << std::endl;

    ++g_rrGuideViewGeneration;
}


// =====================================================
// 2026-10-24 半分辨率 GI（HANDOFF-2026-10-24-GI-HALFRES.md）
//
// 图的内容与口径（两处必须逐字一致）：
//   rt_test.rgen 的 GI 变体写  vec4(indirectLight / max(brdfWeight,1e-3),
//                                   主命中视深度/米)
//   rt_test.rgen 的主变体读  3×3、w = 1/(1e-3 + |z0 - z1|)、再乘回 brdfWeight
//
// 生命周期照 createRRGuideImage：分辨率变化（DLSS 动态分辨率 / 窗口缩放）时整张
// 重建；重建后 g_giHalfNeedsClear 置位，由 recordGiHalfPass 在本帧命令里清一次。
// 图很小（707×400 内部渲染 ⇒ 354×200 RGBA16F ≈ 0.57MB），重建代价可忽略。
// =====================================================
void updateGiHalfRayDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_giHalfView == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo info{};
    info.imageView = g_giHalfView;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 72;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &info;

    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
}


void createGiHalfImage(uint32_t width, uint32_t height)
{
    if(!giHalfEnabled())
    {
        return;
    }

    const uint32_t halfWidth = std::max(1u, (width + 1u) / 2u);
    const uint32_t halfHeight = std::max(1u, (height + 1u) / 2u);

    if(g_giHalfImage != VK_NULL_HANDLE &&
       g_giHalfWidth == halfWidth &&
       g_giHalfHeight == halfHeight)
    {
        return;
    }

    if(g_giHalfImage != VK_NULL_HANDLE)
    {
        check(FGDeviceWaitIdle(g_device),
              "wait device before GI half image recreate failed");
        destroyHistoryImage(
            g_giHalfImage,
            g_giHalfMemory,
            g_giHalfView,
            g_giHalfLayout);
        g_giHalfBoundView = VK_NULL_HANDLE;
    }

    createRRGuideImage(
        halfWidth,
        halfHeight,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_giHalfImage,
        g_giHalfMemory,
        g_giHalfView,
        g_giHalfLayout);

    g_giHalfWidth = halfWidth;
    g_giHalfHeight = halfHeight;
    g_giHalfNeedsClear = true;

    updateGiHalfRayDescriptor();
    g_giHalfBoundView = g_giHalfView;

    std::cerr << "[GI HALF] giHalfImage created "
              << halfWidth << "x" << halfHeight
              << " (render " << width << "x" << height << ")" << std::endl;
}


// ============================================================================
// 2026-11-04 A1：GI 第一反弹时域重投影历史（binding 86/87）。
// 渲染分辨率两张 rgba16f ping-pong；rgb = raw GI（乘 diffuseGILodWeight 之前的
// 域），a = 主命中视深度（a=0 = 无效，clear 天然给出）。
// 生命周期照 createGiHalfImage：分辨率变化整组重建并置 needsClear，
// 由 prepareGiTemporalHistoryImages 在本帧命令里清一次。1440p 下两张 ≈ 59MB。
// ============================================================================
void createGiTemporalImages(uint32_t width, uint32_t height)
{
    if(width == 0u || height == 0u)
    {
        return;
    }

    if(g_giTemporalHistoryImages[0] != VK_NULL_HANDLE &&
       g_giTemporalHistoryWidth == width &&
       g_giTemporalHistoryHeight == height)
    {
        return;
    }

    if(g_giTemporalHistoryImages[0] != VK_NULL_HANDLE)
    {
        check(
            FGDeviceWaitIdle(g_device),
            "wait device before GI temporal history recreate failed"
        );
        for(uint32_t i = 0u; i < 2u; ++i)
        {
            destroyHistoryImage(
                g_giTemporalHistoryImages[i],
                g_giTemporalHistoryMemories[i],
                g_giTemporalHistoryViews[i],
                g_giTemporalHistoryLayouts[i]
            );
        }
    }

    for(uint32_t i = 0u; i < 2u; ++i)
    {
        createRRGuideImage(
            width,
            height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            g_giTemporalHistoryImages[i],
            g_giTemporalHistoryMemories[i],
            g_giTemporalHistoryViews[i],
            g_giTemporalHistoryLayouts[i]
        );
        // createRRGuideImage 不重置宿主侧布局影子；重建后实际布局是 UNDEFINED，
        // 残留的 GENERAL 会让 prepare 跳过 UNDEFINED→GENERAL 转换（验证层报错）。
        g_giTemporalHistoryLayouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;

        VkDescriptorImageInfo info{};
        info.imageView = g_giTemporalHistoryViews[i];
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = g_rayTracingDescriptorSet;
        write.dstBinding = 86u + i;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &info;

        vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    }

    g_giTemporalHistoryWidth = width;
    g_giTemporalHistoryHeight = height;
    g_giTemporalHistoryNeedsClear = true;

    std::cerr << "[GI TEMPORAL] history created " << width << "x" << height
              << " (ping-pong, ~"
              << (static_cast<uint64_t>(width) * height * 8ull * 2ull) / (1024ull * 1024ull)
              << "MB)" << std::endl;
}


// 与 prepareVPTDirectHistoryBuffers 同型：needsClear 时先 transfer 清再放屏障。
// 之后跨帧的读写依赖帧间信号量的内存依赖（vptDirectHistory 同此约定）。
void prepareGiTemporalHistoryImages(VkCommandBuffer cmd)
{
    if(
        cmd == VK_NULL_HANDLE ||
        g_giTemporalHistoryImages[0] == VK_NULL_HANDLE ||
        g_giTemporalHistoryImages[1] == VK_NULL_HANDLE
    )
    {
        return;
    }

    if(g_giTemporalHistoryNeedsClear)
    {
        VkClearColorValue clearColor{};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1u;
        range.layerCount = 1u;

        for(uint32_t i = 0u; i < 2u; ++i)
        {
            if(g_giTemporalHistoryLayouts[i] != VK_IMAGE_LAYOUT_GENERAL)
            {
                VkImageMemoryBarrier toGeneral{
                    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                toGeneral.srcAccessMask = 0u;
                toGeneral.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toGeneral.oldLayout = g_giTemporalHistoryLayouts[i];
                toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                toGeneral.image = g_giTemporalHistoryImages[i];
                toGeneral.subresourceRange = range;

                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0,
                    0,
                    nullptr,
                    0,
                    nullptr,
                    1,
                    &toGeneral
                );
                g_giTemporalHistoryLayouts[i] = VK_IMAGE_LAYOUT_GENERAL;
            }

            vkCmdClearColorImage(
                cmd,
                g_giTemporalHistoryImages[i],
                VK_IMAGE_LAYOUT_GENERAL,
                &clearColor,
                1u,
                &range
            );
        }
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = g_giTemporalHistoryNeedsClear
        ? VK_ACCESS_TRANSFER_WRITE_BIT
        : VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;

    for(uint32_t i = 0u; i < 2u; ++i)
    {
        barrier.image = g_giTemporalHistoryImages[i];
        vkCmdPipelineBarrier(
            cmd,
            g_giTemporalHistoryNeedsClear
                ? VK_PIPELINE_STAGE_TRANSFER_BIT
                : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier
        );
    }

    g_giTemporalHistoryNeedsClear = false;
}


void destroySharcBuffers()
{
    if(g_sharcHashEntriesBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_sharcHashEntriesBuffer, nullptr);
        g_sharcHashEntriesBuffer = VK_NULL_HANDLE;
    }
    if(g_sharcHashEntriesMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_sharcHashEntriesMemory, nullptr);
        g_sharcHashEntriesMemory = VK_NULL_HANDLE;
    }

    if(g_sharcAccumulationBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_sharcAccumulationBuffer, nullptr);
        g_sharcAccumulationBuffer = VK_NULL_HANDLE;
    }
    if(g_sharcAccumulationMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_sharcAccumulationMemory, nullptr);
        g_sharcAccumulationMemory = VK_NULL_HANDLE;
    }

    if(g_sharcResolvedBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_sharcResolvedBuffer, nullptr);
        g_sharcResolvedBuffer = VK_NULL_HANDLE;
    }
    if(g_sharcResolvedMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_sharcResolvedMemory, nullptr);
        g_sharcResolvedMemory = VK_NULL_HANDLE;
    }

    if(g_sharcLockBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_sharcLockBuffer, nullptr);
        g_sharcLockBuffer = VK_NULL_HANDLE;
    }
    if(g_sharcLockMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_sharcLockMemory, nullptr);
        g_sharcLockMemory = VK_NULL_HANDLE;
    }

    g_sharcEntryCountResolved = 0u;
    g_sharcBuffersReady = false;
    g_sharcZeroed = false;

    g_sharcHashEntriesAddress = 0u;
    g_sharcAccumulationAddress = 0u;
    g_sharcResolvedAddress = 0u;
    g_sharcLockAddress = 0u;

    if(g_sharcProbeStagingBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_sharcProbeStagingBuffer, nullptr);
        g_sharcProbeStagingBuffer = VK_NULL_HANDLE;
    }
    if(g_sharcProbeStagingMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_sharcProbeStagingMemory, nullptr);
        g_sharcProbeStagingMemory = VK_NULL_HANDLE;
    }

    g_sharcProbeStage = 0u;
    g_sharcProbeTickCount = 0u;
    g_sharcProbeArmTick = 0u;
}

// 创建四块缓存缓冲（幂等）。失败时置 g_sharcCreateFailed 并让 g_sharcBuffersReady
// 保持 false ⇒ 此后不再重试，着色器的模式也永远进不到"用缓存"那一档，因而不会去
// 访问未写描述符的 65..68（未写描述符被访问是 UB）。
//
// 这里**不清零**：清零放在第一个启用帧（vkCmdFillBuffer 录进本帧 cmd），避免世界
// 初始化时插一次性 submit + wait。见文件上方 SHaRC 块里的说明。
void ensureSharcBuffers()
{
    if(g_sharcBuffersReady || g_sharcCreateFailed)
    {
        return;
    }

    if(
        g_device == VK_NULL_HANDLE ||
        g_physicalDevice == VK_NULL_HANDLE ||
        !sharcEnabled()
    )
    {
        return;
    }

    const uint32_t entries =
        static_cast<uint32_t>(1u) << sharcEntryCount();

    const VkBufferUsageFlags sharcUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    const VkMemoryPropertyFlags sharcProps =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    const VkDeviceSize hashBytes =
        static_cast<VkDeviceSize>(entries) * 8u;
    const VkDeviceSize accumulationBytes =
        static_cast<VkDeviceSize>(entries) * 16u;
    const VkDeviceSize resolvedBytes =
        static_cast<VkDeviceSize>(entries) * 16u;
    const VkDeviceSize lockBytes =
        static_cast<VkDeviceSize>(entries) * 4u;

    try
    {
        createBuffer(
            g_device, g_physicalDevice, hashBytes, sharcUsage, sharcProps,
            g_sharcHashEntriesBuffer, g_sharcHashEntriesMemory);

        createBuffer(
            g_device, g_physicalDevice, accumulationBytes, sharcUsage, sharcProps,
            g_sharcAccumulationBuffer, g_sharcAccumulationMemory);

        createBuffer(
            g_device, g_physicalDevice, resolvedBytes, sharcUsage, sharcProps,
            g_sharcResolvedBuffer, g_sharcResolvedMemory);

        createBuffer(
            g_device, g_physicalDevice, lockBytes, sharcUsage, sharcProps,
            g_sharcLockBuffer, g_sharcLockMemory);

        g_sharcHashEntriesAddress =
            getBufferDeviceAddress(g_device, g_sharcHashEntriesBuffer);
        g_sharcAccumulationAddress =
            getBufferDeviceAddress(g_device, g_sharcAccumulationBuffer);
        g_sharcResolvedAddress =
            getBufferDeviceAddress(g_device, g_sharcResolvedBuffer);
        g_sharcLockAddress =
            getBufferDeviceAddress(g_device, g_sharcLockBuffer);
    }
    catch(...)
    {
        destroySharcBuffers();
        g_sharcCreateFailed = true;

        RTLog("[SHARC] 资源创建失败（显存不足？）⇒ 本次会话停用 SHaRC");
        return;
    }

    g_sharcEntryCountResolved = entries;
    g_sharcBuffersReady = true;

    {
        std::ostringstream log;
        log << "[SHARC] 缓存资源已创建"
            << " entries=" << entries
            << " hashMB=" << (static_cast<double>(hashBytes) / 1048576.0)
            << " accumMB=" << (static_cast<double>(accumulationBytes) / 1048576.0)
            << " resolvedMB=" << (static_cast<double>(resolvedBytes) / 1048576.0)
            << " lockMB=" << (static_cast<double>(lockBytes) / 1048576.0)
            << " totalMB=" << (static_cast<double>(hashBytes + accumulationBytes + resolvedBytes + lockBytes) / 1048576.0)
            << " scale=" << sharcSceneScale()
            << " 待首个启用帧清零";
        RTLog(log.str());
    }
}

// 把四块缓存写进 bindings 65..68。必须在 ensureSharcBuffers() 之后、且
// g_rayTracingDescriptorSet 有效时调用。
void updateSharcDescriptors()
{
    if(
        g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
        !g_sharcBuffersReady
    )
    {
        return;
    }

    const VkBuffer buffers[4] = {
        g_sharcHashEntriesBuffer,
        g_sharcAccumulationBuffer,
        g_sharcResolvedBuffer,
        g_sharcLockBuffer
    };

    const uint32_t bindings[4] = { 65u, 66u, 67u, 68u };

    VkDescriptorBufferInfo infos[4]{};
    VkWriteDescriptorSet writes[4]{};

    for(uint32_t index = 0u; index < 4u; ++index)
    {
        infos[index].buffer = buffers[index];
        infos[index].offset = 0u;
        infos[index].range = VK_WHOLE_SIZE;

        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = g_rayTracingDescriptorSet;
        writes[index].dstBinding = bindings[index];
        writes[index].descriptorCount = 1u;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &infos[index];
    }

    vkUpdateDescriptorSets(g_device, 4u, writes, 0u, nullptr);
}



// 默认跟随 ORION_SHARC（开了 SHaRC 就顺手做一次探针）。要跳过设
// ORION_SHARC_PROBE=0。
bool sharcProbeEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_SHARC_PROBE");

        if(raw == nullptr || raw[0] == '\0')
        {
            return sharcEnabled();
        }

        return raw[0] != '0';
    }();

    return cached;
}

// 一次性提交的公共部分：分配 → begin → record() → end → submit → 等队列空闲 → 释放。
// 用函数指针而不是两个重复函数，是为了保证两处（写魔数 / 回读）的提交结构逐字一致。
void sharcProbeOneShotSubmit(
    void (*record)(VkCommandBuffer),
    const char* label)
{
    VkCommandBufferAllocateInfo allocation{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    allocation.commandPool = g_rtCommandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1u;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    check(
        vkAllocateCommandBuffers(g_device, &allocation, &commandBuffer),
        label
    );

    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    check(vkBeginCommandBuffer(commandBuffer, &beginInfo), label);

    record(commandBuffer);

    check(vkEndCommandBuffer(commandBuffer), label);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;

    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), label);
    check(vkQueueWaitIdle(g_queue), label);

    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
}
void sharcProbeRecordMagic(VkCommandBuffer commandBuffer)
{
    const uint32_t hashWords[2] = {
        SHARC_PROBE_HASH_LO, SHARC_PROBE_HASH_HI
    };
    const uint32_t accumulationWords[4] = {
        SHARC_PROBE_ACCUM_X, 0x34343434u, 0x35353535u, 0x36363636u
    };
    const uint32_t resolvedWords[4] = {
        SHARC_PROBE_RESOLVED_W0,
        SHARC_PROBE_RESOLVED_W1,
        SHARC_PROBE_RESOLVED_SAMPLE,
        SHARC_PROBE_RESOLVED_SAMPLE_EXT
    };

    vkCmdUpdateBuffer(
        commandBuffer, g_sharcHashEntriesBuffer, 0u, 8u, hashWords);
    vkCmdUpdateBuffer(
        commandBuffer, g_sharcAccumulationBuffer, 0u, 16u, accumulationWords);
    vkCmdUpdateBuffer(
        commandBuffer, g_sharcResolvedBuffer, 0u, 16u, resolvedWords);
    vkCmdUpdateBuffer(
        commandBuffer, g_sharcLockBuffer, 0u, 4u, &SHARC_PROBE_LOCK_MAGIC);

    const VkBuffer probeBuffers[4] = {
        g_sharcHashEntriesBuffer,
        g_sharcAccumulationBuffer,
        g_sharcResolvedBuffer,
        g_sharcLockBuffer
    };

    VkBufferMemoryBarrier barriers[4]{};

    for(uint32_t index = 0u; index < 4u; ++index)
    {
        barriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[index].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[index].dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[index].buffer = probeBuffers[index];
        barriers[index].offset = 0u;
        barriers[index].size = VK_WHOLE_SIZE;
    }

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u,
        0u, nullptr,
        4u, barriers,
        0u, nullptr);
}
void sharcProbeRecordReadBack(VkCommandBuffer commandBuffer)
{
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = g_sharcLockBuffer;
    barrier.offset = 0u;
    barrier.size = 28u;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u,
        0u, nullptr,
        1u, &barrier,
        0u, nullptr);

    VkBufferCopy copy{};
    copy.srcOffset = 0u;
    copy.dstOffset = 0u;
    copy.size = 28u;

    vkCmdCopyBuffer(
        commandBuffer,
        g_sharcLockBuffer,
        g_sharcProbeStagingBuffer,
        1u,
        &copy);
}

// 回读 + 逐项比对 + 打一行结论。锁缓冲在 HASH_GRID_ENABLE_64_BIT_ATOMICS=1（SDK 默认，
// 本工程就用这个）时完全不被缓存逻辑使用，所以拿它当回读槽不会影响任何东西。
void sharcProbeReadBackAndReport()
{
    sharcProbeOneShotSubmit(
        sharcProbeRecordReadBack,
        "SHaRC probe read-back submit failed");

    uint32_t words[7] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u };

    void* mapped = nullptr;
    check(
        vkMapMemory(g_device, g_sharcProbeStagingMemory, 0u, 28u, 0u, &mapped),
        "map SHaRC probe staging failed");

    std::memcpy(words, mapped, 28u);
    vkUnmapMemory(g_device, g_sharcProbeStagingMemory);

    const uint32_t expected[7] = {
        SHARC_PROBE_LOCK_MAGIC,
        SHARC_PROBE_HASH_LO,
        SHARC_PROBE_HASH_HI,
        SHARC_PROBE_ACCUM_X,
        SHARC_PROBE_RESOLVED_SAMPLE,
        SHARC_PROBE_RESOLVED_SAMPLE_EXT,
        g_sharcEntryCountResolved
    };
    uint32_t mismatches = 0u;

    for(uint32_t index = 0u; index < 7u; ++index)
    {
        if(words[index] != expected[index])
        {
            ++mismatches;
        }
    }

    char line[460];
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[SHARC PROBE] lock0=%08X hashLo=%08X hashHi=%08X accumX=%08X "
        "resolvedSample=%08X resolvedSampleExt=%08X capacity=%u "
        "| expected %08X %08X %08X %08X %08X %08X %u "
        "| verdict=%s mismatch=%u",
        words[0], words[1], words[2], words[3], words[4], words[5], words[6],
        expected[0], expected[1], expected[2], expected[3], expected[4],
        expected[5], expected[6],
        mismatches == 0u ? "PASS" : "FAIL",
        mismatches);

    RTLogFlushed(line);

    if(mismatches != 0u)
    {
        RTLogFlushed(
            "[SHARC PROBE] FAIL 的含义：上面逐项与期望不符的那个字段就是错的寻址环节 "
            "—— lock0 不符 = lock 地址错；hashLo/hashHi 不符 = hashEntries 地址或 "
            "uint64 的 lo/hi 顺序错；accumX 不符 = accumulation 地址错；"
            "resolvedSample 不符 = resolved 地址或 SharcPackedData 的字节偏移错；"
            "capacity 不符 = UBO 标量 lane 与着色器读法不一致。");
    }
}

// 回读用的 host-visible staging（5a 探针与 5b 统计共用一份）。
// 2026-10-11：64 → 128 字节。5b-3 的逐环节诊断需要 lock[12..19] 八个槽，
// 64 字节只够到 lock[15]；128 字节给到 lock[31]，后面调参还有余量。
void ensureSharcStaging()
{
    if(g_sharcProbeStagingBuffer != VK_NULL_HANDLE)
    {
        return;
    }

    createBuffer(
        g_device,
        g_physicalDevice,
        128u,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_sharcProbeStagingBuffer,
        g_sharcProbeStagingMemory,
        true);
}
void sharcProbeTick()
{
    if(!sharcProbeEnabled() || g_sharcProbeStage >= 2u)
    {
        return;
    }

    if(!g_sharcBuffersReady)
    {
        return;
    }

    ++g_sharcProbeTickCount;

    if(g_sharcProbeStage == 0u)
    {
        ensureSharcStaging();

        sharcProbeOneShotSubmit(
            sharcProbeRecordMagic,
            "SHaRC probe magic submit failed");

        g_sharcProbeStage = 1u;
        g_sharcProbeArmTick = g_sharcProbeTickCount;

        RTLogFlushed(
            "[SHARC PROBE] armed: magic written into element 0 of "
            "hashEntries / accumulation / resolved / lock; mode lane = 1 "
            "for the next 3 frames");

        return;
    }

    if(g_sharcProbeTickCount - g_sharcProbeArmTick < 3u)
    {
        return;
    }

    sharcProbeReadBackAndReport();

    g_sharcProbeStage = 2u;
}



// 四块缓存缓冲的「全缓冲」屏障，供上面三条依赖复用。
void sharcCacheBarrier(
    VkCommandBuffer cmd,
    VkPipelineStageFlags srcStage,
    VkPipelineStageFlags dstStage)
{
    const VkBuffer sharcBuffers[4] = {
        g_sharcHashEntriesBuffer,
        g_sharcAccumulationBuffer,
        g_sharcResolvedBuffer,
        g_sharcLockBuffer
    };

    VkBufferMemoryBarrier barriers[4]{};

    for(uint32_t index = 0u; index < 4u; ++index)
    {
        barriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[index].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[index].dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[index].buffer = sharcBuffers[index];
        barriers[index].offset = 0u;
        barriers[index].size = VK_WHOLE_SIZE;
    }

    vkCmdPipelineBarrier(
        cmd, srcStage, dstStage, 0u, 0u, nullptr, 4u, barriers, 0u, nullptr);
}

// 首次清零。录进**本帧命令缓冲**而不是单独 submit + vkQueueWaitIdle：世界初始化
// 路径里插一次性 submit + wait 会阻塞渲染线程（本工程历史上那次 53.3ms 停顿就是
// 这么来的）。代价是那**一帧**多填充 176MB，之后就恒为 true。
//
// 必须在 RT pass 之前调用：本帧的 update 一进来就要往清零过的缓存里写。
void recordSharcZeroFillIfNeeded(VkCommandBuffer cmd)
{
    if(g_sharcZeroed || !g_sharcBuffersReady)
    {
        return;
    }

    if(sharcProbeEnabled() && g_sharcProbeStage < 2u)
    {
        return;
    }

    vkCmdFillBuffer(cmd, g_sharcHashEntriesBuffer, 0u, VK_WHOLE_SIZE, 0u);
    vkCmdFillBuffer(cmd, g_sharcAccumulationBuffer, 0u, VK_WHOLE_SIZE, 0u);
    vkCmdFillBuffer(cmd, g_sharcResolvedBuffer, 0u, VK_WHOLE_SIZE, 0u);
    vkCmdFillBuffer(cmd, g_sharcLockBuffer, 0u, VK_WHOLE_SIZE, 0u);

    const VkBuffer sharcBuffers[4] = {
        g_sharcHashEntriesBuffer,
        g_sharcAccumulationBuffer,
        g_sharcResolvedBuffer,
        g_sharcLockBuffer
    };

    VkBufferMemoryBarrier barriers[4]{};

    for(uint32_t index = 0u; index < 4u; ++index)
    {
        barriers[index].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barriers[index].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[index].dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[index].buffer = sharcBuffers[index];
        barriers[index].offset = 0u;
        barriers[index].size = VK_WHOLE_SIZE;
    }

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, nullptr, 4u, barriers, 0u, nullptr);

    g_sharcZeroed = true;

    RTLogFlushed(
        "[SHARC] 缓存首次清零已录进本帧命令缓冲（hash 32MB + accum 64MB + "
        "resolved 64MB + lock 16MB 一次性 vkCmdFillBuffer）；"
        "模式 lane 从本帧起 = 3（update+query）或 2（只 query）");
}

// Resolve 管线。复用 set 0 的**现有**描述符集布局（g_rayTracingDescriptorSetLayout）：
// 这个 pass 只读相机 UBO（binding 2），三块缓存是经 UBO 的设备地址以 buffer_reference
// 进来的 —— 所以不需要新的描述符集布局 / 池。
void ensureSharcResolvePipeline()
{
    if(
        g_sharcResolvePipeline != VK_NULL_HANDLE ||
        g_sharcResolvePipelineFailed
    )
    {
        return;
    }

    if(
        g_device == VK_NULL_HANDLE ||
        g_rayTracingDescriptorSetLayout == VK_NULL_HANDLE ||
        !sharcEnabled()
    )
    {
        return;
    }

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    pipelineLayoutInfo.setLayoutCount = 1u;
    pipelineLayoutInfo.pSetLayouts = &g_rayTracingDescriptorSetLayout;

    try
    {
        check(
            vkCreatePipelineLayout(
                g_device, &pipelineLayoutInfo, nullptr,
                &g_sharcResolvePipelineLayout),
            "SHaRC resolve pipeline layout failed");

        const std::vector<uint8_t> code =
            loadSpv("shaders/rt_sharc_resolve.comp.spv");
        VkShaderModule shader = createShaderModule(g_device, code);

        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        pipelineInfo.stage = stage;
        pipelineInfo.layout = g_sharcResolvePipelineLayout;

        const VkResult result = vkCreateComputePipelines(
            g_device, getSharedPipelineCache(), 1u, &pipelineInfo, nullptr,
            &g_sharcResolvePipeline);

        vkDestroyShaderModule(g_device, shader, nullptr);

        check(result, "SHaRC resolve pipeline failed");

        RTLogFlushed(
            "[SHARC] Resolve 管线已创建（rt_sharc_resolve.comp.spv，"
            "复用 set 0 布局，local_size=64）");
    }
    catch(...)
    {
        g_sharcResolvePipelineFailed = true;
        g_sharcResolvePipeline = VK_NULL_HANDLE;
        RTLogFlushed("[SHARC] Resolve 管线创建失败 ⇒ 本次会话不跑 Resolve pass");
    }
}
void destroySharcUpdatePipeline()
{
    if(g_sharcUpdateSbtBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_sharcUpdateSbtBuffer, nullptr);
        g_sharcUpdateSbtBuffer = VK_NULL_HANDLE;
    }
    if(g_sharcUpdateSbtMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_sharcUpdateSbtMemory, nullptr);
        g_sharcUpdateSbtMemory = VK_NULL_HANDLE;
    }
    if(g_sharcUpdatePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_sharcUpdatePipeline, nullptr);
        g_sharcUpdatePipeline = VK_NULL_HANDLE;
    }
}
void ensureSharcUpdatePipeline()
{
    if(g_sharcUpdatePipeline != VK_NULL_HANDLE || g_sharcUpdatePipelineFailed)
    {
        return;
    }

    if(!sharcEnabled())
    {
        return;
    }
    if(g_rayTracingPipelineLayout == VK_NULL_HANDLE || g_handleSizeAligned == 0u)
    {
        return;
    }

    RTLog("[SHARC] update pass：开始创建第二 RT 管线（驱动编译着色器，可能阻塞数秒）");

    try
    {
        auto updateRgen = loadSpv("shaders/rt_test_sharc_update.rgen.spv");
        auto chit = loadSpv("shaders/rt_test.rchit.spv");
        auto miss = loadSpv("shaders/rt_test.rmiss.spv");
        auto ahit = loadSpv("shaders/rt_test.rahit.spv");
        auto translucentChit = loadSpv("shaders/rt_test_translucent.rchit.spv");
        auto translucentAhit = loadSpv("shaders/rt_test_translucent.rahit.spv");

        VkShaderModule mods[6]{};
        const char* entry = "main";
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        auto makeModule = [&](const std::vector<uint8_t>& code, VkShaderModule& out)
        {
            mci.codeSize = code.size();
            mci.pCode = reinterpret_cast<const uint32_t*>(code.data());
            check(vkCreateShaderModule(g_device, &mci, nullptr, &out),
                  "Failed to create SHaRC update shader module");
        };
        makeModule(updateRgen, mods[0]);
        makeModule(chit, mods[1]);
        makeModule(miss, mods[2]);
        makeModule(ahit, mods[3]);
        makeModule(translucentChit, mods[4]);
        makeModule(translucentAhit, mods[5]);

        VkPipelineShaderStageCreateInfo stages[6]{};
        const VkShaderStageFlagBits stageKinds[6] = {
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR };
        for(uint32_t i = 0u; i < 6u; ++i)
        {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = stageKinds[i];
            stages[i].module = mods[i];
            stages[i].pName = entry;
        }

        VkRayTracingShaderGroupCreateInfoKHR groups[5]{};
        groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0u;
        groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;
        groups[1] = groups[0];
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[1].generalShader = VK_SHADER_UNUSED_KHR;
        groups[1].closestHitShader = 1u;
        groups[1].anyHitShader = 3u;
        groups[2] = groups[0];
        groups[2].generalShader = 2u;
        groups[3] = groups[1];
        groups[3].closestHitShader = 4u;
        groups[3].anyHitShader = 5u;
        groups[4] = groups[1];

        VkRayTracingPipelineCreateInfoKHR pci{
            VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pci.stageCount = 6u;
        pci.pStages = stages;
        pci.groupCount = 5u;
        pci.pGroups = groups;
        pci.maxPipelineRayRecursionDepth = 1u;
        pci.layout = g_rayTracingPipelineLayout;
        if(g_ommNativeEnabled)
        {
            pci.flags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
        }

        // 2026-11-02 P0（冷编分解）：走计时包装，cache / deferredOperation / 1 条
        // createInfo 与原调用逐位一致，只多一条 [PERF PIPE] kind=rt 日志。
        check(OrionCreateRayTracingPipelineTimed(
                  "sharc-update",
                  1u,
                  &pci,
                  getSharedPipelineCache(),
                  &g_sharcUpdatePipeline),
              "Failed to create SHaRC update RT pipeline");

        for(uint32_t i = 0u; i < 6u; ++i)
        {
            vkDestroyShaderModule(g_device, mods[i], nullptr);
        }

        auto getHandles =
            reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
                vkGetDeviceProcAddr(g_device,
                                    "vkGetRayTracingShaderGroupHandlesKHR"));
        if(!getHandles)
        {
            throw std::runtime_error(
                "vkGetRayTracingShaderGroupHandlesKHR unavailable");
        }

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
        rtProps.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 props2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &rtProps;
        vkGetPhysicalDeviceProperties2(g_physicalDevice, &props2);

        const uint32_t hs = rtProps.shaderGroupHandleSize;
        const uint32_t ha = rtProps.shaderGroupHandleAlignment;
        const uint32_t ba = rtProps.shaderGroupBaseAlignment;
        const uint32_t handleAligned = ((hs + ha - 1u) / ha) * ha;
        const uint32_t raygenOffset = 0u;
        const uint32_t missOffset =
            ((raygenOffset + handleAligned + ba - 1u) / ba) * ba;
        const uint32_t hitOffset =
            ((missOffset + handleAligned + ba - 1u) / ba) * ba;
        const uint32_t sbtSize =
            ((hitOffset + SBT_HIT_RECORD_COUNT * handleAligned + ba - 1u) / ba) *
            ba;

        const uint32_t sharcRaygenGroup = 0u;
        const uint32_t sharcMissGroup = 2u;
        const uint32_t sharcOpaqueGroup = 1u;
        const uint32_t sharcAlphaGroup = 3u;
        const uint32_t sharcWaterGroup = 4u;
        const uint32_t sharcGroupCount = 5u;

        std::vector<uint8_t> handles((size_t)sharcGroupCount * hs, 0);
        check(getHandles(g_device, g_sharcUpdatePipeline, 0u,
                         sharcGroupCount, handles.size(), handles.data()),
              "Failed to get SHaRC update RT shader handles");

        std::vector<uint8_t> data((size_t)sbtSize, 0);
        std::memcpy(data.data() + raygenOffset,
                    handles.data() + sharcRaygenGroup * hs, hs);
        std::memcpy(data.data() + missOffset,
                    handles.data() + sharcMissGroup * hs, hs);
        const uint32_t recordGroups[SBT_HIT_RECORD_COUNT] = {
            sharcOpaqueGroup, sharcOpaqueGroup, sharcAlphaGroup,
            sharcWaterGroup, sharcOpaqueGroup };
        for(uint32_t record = 0u; record < SBT_HIT_RECORD_COUNT; ++record)
        {
            std::memcpy(data.data() + hitOffset + record * handleAligned,
                        handles.data() + recordGroups[record] * hs, hs);
        }

        createBuffer(g_device, g_physicalDevice, sbtSize,
                     VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_sharcUpdateSbtBuffer, g_sharcUpdateSbtMemory, true);

        void* sbtMapped = nullptr;
        check(vkMapMemory(g_device, g_sharcUpdateSbtMemory, 0u, sbtSize, 0u,
                          &sbtMapped),
              "map SHaRC update SBT failed");
        std::memcpy(sbtMapped, data.data(), sbtSize);
        vkUnmapMemory(g_device, g_sharcUpdateSbtMemory);

        const uint64_t sbtAddress =
            getBufferDeviceAddress(g_device, g_sharcUpdateSbtBuffer);
        g_sharcUpdateRaygenRegion.deviceAddress = sbtAddress + raygenOffset;
        g_sharcUpdateRaygenRegion.stride = handleAligned;
        g_sharcUpdateRaygenRegion.size = handleAligned;
        g_sharcUpdateMissRegion.deviceAddress = sbtAddress + missOffset;
        g_sharcUpdateMissRegion.stride = handleAligned;
        g_sharcUpdateMissRegion.size = handleAligned;
        g_sharcUpdateHitRegion.deviceAddress = sbtAddress + hitOffset;
        g_sharcUpdateHitRegion.stride = handleAligned;
        g_sharcUpdateHitRegion.size =
            SBT_HIT_RECORD_COUNT * handleAligned;
        g_sharcUpdateCallableRegion.deviceAddress = 0u;
        g_sharcUpdateCallableRegion.stride = 0u;
        g_sharcUpdateCallableRegion.size = 0u;

        RTLog("[SHARC] update pass 管线已创建（rt_test_sharc_update.rgen.spv，"
              "满分辨率 dispatch、shader 内 5×5 选块 = 4%）");
    }
    catch(const std::exception& error)
    {
        RTLog(std::string("[SHARC] update pass 管线创建失败（本会话不再重试，"
                          "查询将查不到缓存 ⇒ 渲染退回实追）：") + error.what());
        destroySharcUpdatePipeline();
        g_sharcUpdatePipelineFailed = true;
        g_sharcUpdatePipeline = VK_NULL_HANDLE;
    }
}
void recordSharcUpdate(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if(!g_sharcZeroed || !g_sharcBuffersReady)
    {
        return;
    }
    if(g_rayTracingPipelineLayout == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    // 2026-11-06：ORION_SHARC_UPDATE=0 此前只影响 shader 内的 lane 取值，宿主仍然
    // 每帧发满 dispatch（1080p 下 384x216 = 82,944 条 RT 路径），"关掉"是空跑。
    // 这里补上 host 侧闸门，让该 env 真正省掉这次 trace。
    if(!sharcUpdateEnabled())
    {
        return;
    }

    ensureSharcUpdatePipeline();
    if(g_sharcUpdatePipeline == VK_NULL_HANDLE)
    {
        return;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      g_sharcUpdatePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            g_rayTracingPipelineLayout, 0u, 1u,
                            &g_rayTracingDescriptorSet, 0u, nullptr);

    auto trace = getCmdTraceRays();
    if(!trace)
    {
        return;
    }

    const uint32_t sharcUpdateBlock = 5u;
    const uint32_t sharcUpdateWidth =
        std::max(1u, (width + sharcUpdateBlock - 1u) / sharcUpdateBlock);
    const uint32_t sharcUpdateHeight =
        std::max(1u, (height + sharcUpdateBlock - 1u) / sharcUpdateBlock);

    trace(cmd, &g_sharcUpdateRaygenRegion, &g_sharcUpdateMissRegion,
          &g_sharcUpdateHitRegion, &g_sharcUpdateCallableRegion,
          sharcUpdateWidth, sharcUpdateHeight, 1u);

    sharcCacheBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                      VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
}
void recordSharcResolve(VkCommandBuffer cmd)
{
    if(!g_sharcZeroed || !g_sharcBuffersReady)
    {
        return;
    }

    ensureSharcResolvePipeline();

    if(g_sharcResolvePipeline == VK_NULL_HANDLE)
    {
        return;
    }

    // 2026-11-06：Resolve 是"消耗性读取"——SharcResolveEntry 把 accumulation 归一化
    // 写进 resolved 并推进 sampleData / staleFrameNum 计数
    // （shader 侧 sharc/SharcCommon.h:826-950），被跳过的帧不会丢样本，只是把累积
    // 留到下一次 Resolve 一起合并。
    // 成本：hash 32MB + accumulation 64MB + resolved 64MB + lock 16MB = 176MB，
    // 全量扫描（1<<22 entry ⇒ 65536 个 workgroup）的显存流量地板约 0.20~0.25 ms/帧。
    // ORION_SHARC_RESOLVE_PARITY=1 回到每帧（A/B 用），默认 2 = 隔帧。
    static const uint32_t sSharcResolveParity = []()
    {
        const char* raw = std::getenv("ORION_SHARC_RESOLVE_PARITY");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 2u;
        }
        const int parsed = std::atoi(raw);
        if(parsed < 1)
        {
            return 1u;
        }
        return static_cast<uint32_t>(parsed > 4 ? 4 : parsed);
    }();
    static uint32_t sSharcResolveTick = 0u;

    if(sSharcResolveParity > 1u &&
       (sSharcResolveTick++ % sSharcResolveParity) != 0u)
    {
        return;
    }

    sharcCacheBarrier(
        cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_sharcResolvePipeline);

    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_sharcResolvePipelineLayout,
        0u, 1u, &g_rayTracingDescriptorSet,
        0u, nullptr);

    const uint32_t sharcResolveGroups =
        (g_sharcEntryCountResolved + 63u) / 64u;

    vkCmdDispatch(cmd, sharcResolveGroups, 1u, 1u);

    sharcCacheBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}
void destroyGiHalfPipeline()
{
    if(g_giHalfSbtBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_giHalfSbtBuffer, nullptr);
        g_giHalfSbtBuffer = VK_NULL_HANDLE;
    }
    if(g_giHalfSbtMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_giHalfSbtMemory, nullptr);
        g_giHalfSbtMemory = VK_NULL_HANDLE;
    }
    if(g_giHalfPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_giHalfPipeline, nullptr);
        g_giHalfPipeline = VK_NULL_HANDLE;
    }
}
void ensureGiHalfPipeline()
{
    if(g_giHalfPipeline != VK_NULL_HANDLE || g_giHalfPipelineFailed)
    {
        return;
    }

    if(!giHalfEnabled())
    {
        return;
    }

    if(g_rayTracingPipelineLayout == VK_NULL_HANDLE || g_handleSizeAligned == 0u)
    {
        return;
    }

    std::cerr << "[GI HALF] 开始创建半分辨率 GI 管线（驱动编译着色器，可能阻塞数秒）"
              << std::endl;

    try
    {
        auto giHalfRgen = loadSpv("shaders/rt_test_gi_half.rgen.spv");
        auto chit = loadSpv("shaders/rt_test.rchit.spv");
        auto miss = loadSpv("shaders/rt_test.rmiss.spv");
        auto ahit = loadSpv("shaders/rt_test.rahit.spv");
        auto translucentChit = loadSpv("shaders/rt_test_translucent.rchit.spv");
        auto translucentAhit = loadSpv("shaders/rt_test_translucent.rahit.spv");

        VkShaderModule mods[6]{};
        const char* entry = "main";
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        auto makeModule = [&](const std::vector<uint8_t>& code, VkShaderModule& out)
        {
            mci.codeSize = code.size();
            mci.pCode = reinterpret_cast<const uint32_t*>(code.data());
            check(vkCreateShaderModule(g_device, &mci, nullptr, &out),
                  "Failed to create GI half shader module");
        };
        makeModule(giHalfRgen, mods[0]);
        makeModule(chit, mods[1]);
        makeModule(miss, mods[2]);
        makeModule(ahit, mods[3]);
        makeModule(translucentChit, mods[4]);
        makeModule(translucentAhit, mods[5]);

        VkPipelineShaderStageCreateInfo stages[6]{};
        const VkShaderStageFlagBits stageKinds[6] = {
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR };
        for(uint32_t i = 0u; i < 6u; ++i)
        {
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = stageKinds[i];
            stages[i].module = mods[i];
            stages[i].pName = entry;
        }

        VkRayTracingShaderGroupCreateInfoKHR groups[5]{};
        groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0u;
        groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;
        groups[1] = groups[0];
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[1].generalShader = VK_SHADER_UNUSED_KHR;
        groups[1].closestHitShader = 1u;
        groups[1].anyHitShader = 3u;
        groups[2] = groups[0];
        groups[2].generalShader = 2u;
        groups[3] = groups[1];
        groups[3].closestHitShader = 4u;
        groups[3].anyHitShader = 5u;
        groups[4] = groups[1];

        VkRayTracingPipelineCreateInfoKHR pci{
            VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pci.stageCount = 6u;
        pci.pStages = stages;
        pci.groupCount = 5u;
        pci.pGroups = groups;
        pci.maxPipelineRayRecursionDepth = 1u;
        pci.layout = g_rayTracingPipelineLayout;
        if(g_ommNativeEnabled)
        {
            pci.flags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
        }

        // 2026-11-02 P0（冷编分解）：同 sharc-update，只加计时不改参数。
        check(OrionCreateRayTracingPipelineTimed(
                  "gi-half",
                  1u,
                  &pci,
                  getSharedPipelineCache(),
                  &g_giHalfPipeline),
              "Failed to create GI half RT pipeline");

        for(uint32_t i = 0u; i < 6u; ++i)
        {
            vkDestroyShaderModule(g_device, mods[i], nullptr);
        }

        auto getHandles =
            reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
                vkGetDeviceProcAddr(g_device,
                                    "vkGetRayTracingShaderGroupHandlesKHR"));
        if(!getHandles)
        {
            throw std::runtime_error(
                "vkGetRayTracingShaderGroupHandlesKHR unavailable");
        }

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
        rtProps.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 props2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &rtProps;
        vkGetPhysicalDeviceProperties2(g_physicalDevice, &props2);

        const uint32_t hs = rtProps.shaderGroupHandleSize;
        const uint32_t ha = rtProps.shaderGroupHandleAlignment;
        const uint32_t ba = rtProps.shaderGroupBaseAlignment;
        const uint32_t handleAligned = ((hs + ha - 1u) / ha) * ha;
        const uint32_t raygenOffset = 0u;
        const uint32_t missOffset =
            ((raygenOffset + handleAligned + ba - 1u) / ba) * ba;
        const uint32_t hitOffset =
            ((missOffset + handleAligned + ba - 1u) / ba) * ba;
        const uint32_t sbtSize =
            ((hitOffset + SBT_HIT_RECORD_COUNT * handleAligned + ba - 1u) / ba) *
            ba;

        const uint32_t giRaygenGroup = 0u;
        const uint32_t giMissGroup = 2u;
        const uint32_t giOpaqueGroup = 1u;
        const uint32_t giAlphaGroup = 3u;
        const uint32_t giWaterGroup = 4u;
        const uint32_t giGroupCount = 5u;

        std::vector<uint8_t> handles((size_t)giGroupCount * hs, 0);
        check(getHandles(g_device, g_giHalfPipeline, 0u,
                         giGroupCount, handles.size(), handles.data()),
              "Failed to get GI half RT shader handles");

        std::vector<uint8_t> data((size_t)sbtSize, 0);
        std::memcpy(data.data() + raygenOffset,
                    handles.data() + giRaygenGroup * hs, hs);
        std::memcpy(data.data() + missOffset,
                    handles.data() + giMissGroup * hs, hs);
        const uint32_t recordGroups[SBT_HIT_RECORD_COUNT] = {
            giOpaqueGroup, giOpaqueGroup, giAlphaGroup,
            giWaterGroup, giOpaqueGroup };
        for(uint32_t record = 0u; record < SBT_HIT_RECORD_COUNT; ++record)
        {
            std::memcpy(data.data() + hitOffset + record * handleAligned,
                        handles.data() + recordGroups[record] * hs, hs);
        }

        createBuffer(g_device, g_physicalDevice, sbtSize,
                     VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_giHalfSbtBuffer, g_giHalfSbtMemory, true);

        void* sbtMapped = nullptr;
        check(vkMapMemory(g_device, g_giHalfSbtMemory, 0u, sbtSize, 0u,
                          &sbtMapped),
              "map GI half SBT failed");
        std::memcpy(sbtMapped, data.data(), sbtSize);
        vkUnmapMemory(g_device, g_giHalfSbtMemory);

        const uint64_t sbtAddress =
            getBufferDeviceAddress(g_device, g_giHalfSbtBuffer);
        g_giHalfRaygenRegion.deviceAddress = sbtAddress + raygenOffset;
        g_giHalfRaygenRegion.stride = handleAligned;
        g_giHalfRaygenRegion.size = handleAligned;
        g_giHalfMissRegion.deviceAddress = sbtAddress + missOffset;
        g_giHalfMissRegion.stride = handleAligned;
        g_giHalfMissRegion.size = handleAligned;
        g_giHalfHitRegion.deviceAddress = sbtAddress + hitOffset;
        g_giHalfHitRegion.stride = handleAligned;
        g_giHalfHitRegion.size = SBT_HIT_RECORD_COUNT * handleAligned;
        g_giHalfCallableRegion.deviceAddress = 0u;
        g_giHalfCallableRegion.stride = 0u;
        g_giHalfCallableRegion.size = 0u;

        std::cerr << "[GI HALF] 管线已创建（rt_test_gi_half.rgen.spv，"
                     "半分辨率 dispatch = (w+1)/2 × (h+1)/2）" << std::endl;

        // 2026-11-06：不在这帧同步 fwrite 64~256MB blob
        // （PIPELINE_CACHE_SEED_BUDGET_BYTES）。交给 maybePersistSharedPipelineCache
        // 的 60 帧去抖 + 大小去重（PipelineCache.cpp:726-758），把冷编结束那一帧
        // 30~400ms 的写盘尖峰摊掉。
        g_pipelineCachePersistCountdown = 60u;
    }
    catch(const std::exception& error)
    {
        std::cerr << "[GI HALF] 管线创建失败（本会话不再重试，GI 回到全分辨率）："
                  << error.what() << std::endl;
        destroyGiHalfPipeline();
        g_giHalfPipelineFailed = true;
        g_giHalfPipeline = VK_NULL_HANDLE;
    }
}
void recordGiHalfPass(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if(!giHalfEnabled())
    {
        return;
    }
    if(g_giHalfImage == VK_NULL_HANDLE ||
       g_rayTracingPipelineLayout == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    ensureGiHalfPipeline();
    if(g_giHalfPipeline == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_giHalfBoundView != g_giHalfView)
    {
        updateGiHalfRayDescriptor();
        g_giHalfBoundView = g_giHalfView;
    }

    clearGiHalfImageIfNeeded(cmd);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      g_giHalfPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            g_rayTracingPipelineLayout, 0u, 1u,
                            &g_rayTracingDescriptorSet, 0u, nullptr);

    auto trace = getCmdTraceRays();
    if(!trace)
    {
        return;
    }

    const uint32_t halfWidth = std::max(1u, (width + 1u) / 2u);
    const uint32_t halfHeight = std::max(1u, (height + 1u) / 2u);
    trace(cmd, &g_giHalfRaygenRegion, &g_giHalfMissRegion,
          &g_giHalfHitRegion, &g_giHalfCallableRegion,
          halfWidth, halfHeight, 1u);

    VkImageMemoryBarrier giBarrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    giBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    giBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    giBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    giBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    giBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    giBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    giBarrier.image = g_giHalfImage;
    giBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    giBarrier.subresourceRange.baseMipLevel = 0u;
    giBarrier.subresourceRange.levelCount = 1u;
    giBarrier.subresourceRange.baseArrayLayer = 0u;
    giBarrier.subresourceRange.layerCount = 1u;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &giBarrier);
}

// =====================================================
// 2026-11-05 B：K1(主视线)/K2(传输) 两发拆分。
// K1 只做主光线求交，opaque 像素在切割点把 payload 落进 landing SSBO
// (binding 88, 5×vec4=80B) 后提前收束（输出尾按 !anyLandingStored 跳过）；
// K2 读回 landing 继续 GPU 传输段（输出尾按 ptLandingActive 执行）。
// 非 opaque 像素 K1 落 invalid 并照旧完整出图，K2 的 ptLandingLoad 失败跳过
// —— 两侧像素集互斥且由各自代码路径自行判定，不依赖 C++ 记账。
// 管线/SBT/SER 选择全部照抄 ensureGiHalfPipeline 前例。
// =====================================================
namespace
{
VkDescriptorSet s_k1LandingBoundSet = VK_NULL_HANDLE;
void writeK1LandingDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_k1LandingBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorBufferInfo info{};
    info.buffer = g_k1LandingBuffer;
    info.offset = 0u;
    info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 88;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;

    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);
    s_k1LandingBoundSet = g_rayTracingDescriptorSet;
}
}
void destroyK1LandingBuffer()
{
    if(g_k1LandingBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_k1LandingBuffer, nullptr);
        g_k1LandingBuffer = VK_NULL_HANDLE;
    }
    if(g_k1LandingMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_k1LandingMemory, nullptr);
        g_k1LandingMemory = VK_NULL_HANDLE;
    }
    g_k1LandingWidth = 0u;
    g_k1LandingHeight = 0u;
    g_k1LandingSlots = 0u;
    s_k1LandingBoundSet = VK_NULL_HANDLE;
}
void ensureK1LandingBuffer(uint32_t width, uint32_t height, uint32_t slots)
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       width == 0u || height == 0u || slots == 0u)
    {
        return;
    }
    if(g_k1LandingBuffer != VK_NULL_HANDLE &&
       g_k1LandingWidth == width && g_k1LandingHeight == height &&
       g_k1LandingSlots == slots)
    {
        return;
    }

    // 老缓冲可能还有在飞引用
    FGDeviceWaitIdle(g_device);
    destroyK1LandingBuffer();

    const VkDeviceSize bytes =
        (VkDeviceSize)width * height * slots * 80ull;
    createBuffer(g_device, g_physicalDevice, bytes,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 g_k1LandingBuffer, g_k1LandingMemory, false);
    if(g_k1LandingBuffer == VK_NULL_HANDLE)
    {
        std::cerr << "[K1/K2] landing 缓冲创建失败（" << bytes << " 字节）"
                  << std::endl;
        return;
    }
    g_k1LandingWidth = width;
    g_k1LandingHeight = height;
    g_k1LandingSlots = slots;

    writeK1LandingDescriptor();
    std::cerr << "[K1/K2] landing 缓冲就绪 " << width << "x" << height
              << " x" << slots << "spp (" << bytes << " 字节, binding 88)"
              << std::endl;
}
void destroyK1K2Pipeline()
{
    if(g_k1k2SbtBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_k1k2SbtBuffer, nullptr);
        g_k1k2SbtBuffer = VK_NULL_HANDLE;
    }
    if(g_k1k2SbtMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_k1k2SbtMemory, nullptr);
        g_k1k2SbtMemory = VK_NULL_HANDLE;
    }
    if(g_k1Pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k1Pipeline, nullptr);
        g_k1Pipeline = VK_NULL_HANDLE;
    }
    if(g_k2Pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2Pipeline, nullptr);
        g_k2Pipeline = VK_NULL_HANDLE;
    }
    if(g_k2dPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2dPipeline, nullptr);
        g_k2dPipeline = VK_NULL_HANDLE;
    }
    if(g_k2iPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2iPipeline, nullptr);
        g_k2iPipeline = VK_NULL_HANDLE;
    }
}

// ============================================================================
// ③ K2-D→K2-I 直射级交接图（binding 89，rgba32f，渲染分辨率）。
//   K2-D 在水/不透明出口写（rgb = 直射三项和或水的完整着色，a = rtaoFactor），
//   K2-I 在 RTAO 桩/水支读。同帧内 K2-D 完全覆盖 K2-I 会读的像素集
//   （两者都只在 landing 有效像素上运行）⇒ 无需清理，也不需要跨帧历史。
// 生命周期照 createGiHalfImage：分辨率变化整张重建。
// ============================================================================
static VkDescriptorSet s_k2DirectBoundSet = VK_NULL_HANDLE;
void updateK2DirectRayDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_k2DirectView == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo info{};
    info.imageView = g_k2DirectView;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 89;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &info;

    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
}

void createK2DirectImage(uint32_t width, uint32_t height)
{
    if(!k2DirectIndirectSplitEnabled() || width == 0u || height == 0u)
    {
        return;
    }

    if(g_k2DirectImage != VK_NULL_HANDLE &&
       g_k2DirectWidth == width &&
       g_k2DirectHeight == height)
    {
        // 描述符集可能被环形机制重建过，binding 89 需要随动
        if(s_k2DirectBoundSet != g_rayTracingDescriptorSet)
        {
            updateK2DirectRayDescriptor();
            s_k2DirectBoundSet = g_rayTracingDescriptorSet;
        }
        return;
    }

    if(g_k2DirectImage != VK_NULL_HANDLE)
    {
        check(FGDeviceWaitIdle(g_device),
              "wait device before K2 direct image recreate failed");
        destroyHistoryImage(
            g_k2DirectImage,
            g_k2DirectMemory,
            g_k2DirectView,
            g_k2DirectLayout);
        s_k2DirectBoundSet = VK_NULL_HANDLE;
    }

    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R32G32B32A32_SFLOAT,
        g_k2DirectImage,
        g_k2DirectMemory,
        g_k2DirectView,
        g_k2DirectLayout);

    g_k2DirectWidth = width;
    g_k2DirectHeight = height;

    updateK2DirectRayDescriptor();
    s_k2DirectBoundSet = g_rayTracingDescriptorSet;

    std::cerr << "[K1/K2] k2DirectImage created "
              << width << "x" << height << std::endl;
}

void destroyK2DirectImage()
{
    if(g_k2DirectImage != VK_NULL_HANDLE)
    {
        destroyHistoryImage(
            g_k2DirectImage,
            g_k2DirectMemory,
            g_k2DirectView,
            g_k2DirectLayout);
    }
    g_k2DirectWidth = 0u;
    g_k2DirectHeight = 0u;
    s_k2DirectBoundSet = VK_NULL_HANDLE;
}

void ensureK1K2Pipelines()
{
    const bool k2Split = k2DirectIndirectSplitEnabled();
    const bool k2TransportReady =
        k2Split
            ? (g_k2dPipeline != VK_NULL_HANDLE && g_k2iPipeline != VK_NULL_HANDLE)
            : (g_k2Pipeline != VK_NULL_HANDLE);
    if((g_k1Pipeline != VK_NULL_HANDLE && k2TransportReady) ||
       g_k1K2PipelineFailed)
    {
        return;
    }
    if(!k1k2SplitEnabled())
    {
        return;
    }
    if(g_rayTracingPipelineLayout == VK_NULL_HANDLE || g_handleSizeAligned == 0u)
    {
        return;
    }

    std::cerr << "[K1/K2] 开始创建 K1/K2 拆分管线（驱动编译着色器，可能阻塞数秒）"
              << std::endl;

    try
    {
        // SER 选择条件照抄 Device.cpp 主管线：env + 特性 + 硬件重排全开才用 _ser spv
        // 二分开关（2026-11-05 设备丢失排查）：ORION_K1K2_NO_SER=1 强制 plain
        const bool k1k2NoSer =
            [](){
                const char* raw = std::getenv("ORION_K1K2_NO_SER");
                return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
            }();
        const bool serShaderRequested =
            !k1k2NoSer &&
            g_serEnabled.load() && g_serFeatureSupported && g_serHardwareReordering;
        auto loadRgen = [serShaderRequested](
                            const char* serPath, const char* plainPath)
        {
            if(serShaderRequested)
            {
                try
                {
                    return loadSpv(serPath);
                }
                catch(const std::exception&)
                {
                    RTLog(std::string("[K1/K2] ser spv 加载失败，回退 plain: ") +
                          serPath);
                }
            }
            return loadSpv(plainPath);
        };
        auto k1Code = loadRgen("shaders/rt_test_k1_ser.rgen.spv",
                               "shaders/rt_test_k1.rgen.spv");
        // 2026-11-05 设备丢失二分：ORION_K2_VARIANT=1 → nocall 桩版（不执行
        // callable）；=2 → B① 内联材质版（D1 之前的 k2 spv，基线 32d637b6）。
        // 两个诊断变体不分 SER；=0/未设走正常 D1 双变体。
        const int k2Variant = [](){
            const char* raw = std::getenv("ORION_K2_VARIANT");
            if(raw == nullptr || raw[0] == '\0')
            {
                return 0;
            }
            return raw[0] == '1' ? 1 : (raw[0] == '2' ? 2 : 0);
        }();
        RTLog("[K1/K2] ORION_K2_VARIANT=" + std::to_string(k2Variant));
        // ③ K2-D/K2-I 再拆分（默认开，ORION_K2_SPLIT=0 回退）：传输级再分直射级与间接级，
        // 各自只编译自己那半 ⇒ 驱动编译与 spill 都只看最坏分支的一半。
        // 2026-11-06 -3 定案：INITIALIZATION_FAILED = 同会话连续巨型冷编译的
        // 驱动资源耗尽（对照：main+k1 命中缓存时 k2d_ser 冷编成功），非
        // shader/SER 问题；PipelineDiag 已加失败重试。这里恢复 SER 优先；
        // SER 档最终仍失败时在下方降级 plain 重建，拆分模式不连坐。
        const bool k2SerProbe =
            [](){
                const char* raw = std::getenv("ORION_K2_SER_PROBE");
                return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
            }();
        auto k2dCode = k2Split
            ? loadRgen("shaders/rt_test_k2d_ser.rgen.spv",
                       "shaders/rt_test_k2d.rgen.spv")
            : std::vector<uint8_t>{};
        auto k2iCode = k2Split
            ? loadRgen("shaders/rt_test_k2i_ser.rgen.spv",
                       "shaders/rt_test_k2i.rgen.spv")
            : std::vector<uint8_t>{};
        if(k2SerProbe && k2Split)
        {
            auto fnv32 = [](const std::vector<uint8_t>& b){
                uint32_t h = 0x811c9dc5u;
                for(uint8_t c : b)
                {
                    h = (h ^ c) * 0x01000193u;
                }
                return h;
            };
            char buf[160]{};
            std::snprintf(buf, sizeof(buf),
                          "[K2SERPROBE] k2d bytes=%zu fnv=%08x | k2i bytes=%zu fnv=%08x",
                          k2dCode.size(), fnv32(k2dCode),
                          k2iCode.size(), fnv32(k2iCode));
            RTLog(buf);
        }
        auto k2Code =
            k2Split
                ? std::vector<uint8_t>{}
                : k2Variant == 1
                    ? loadSpv("shaders/rt_test_k2_nocall.rgen.spv")
                    : k2Variant == 2
                        ? loadSpv("shaders/rt_test_k2_inline.rgen.spv")
                        : loadRgen("shaders/rt_test_k2_ser.rgen.spv",
                                   "shaders/rt_test_k2.rgen.spv");
        // B①：K1 的主命中组用精简 rchit（只落身份）；K2 与玻璃命中组用全功能版。
        auto chit = loadSpv("shaders/rt_test.rchit.spv");
        auto k1Chit = loadSpv("shaders/rt_test_k1.rchit.spv");
        auto miss = loadSpv("shaders/rt_test.rmiss.spv");
        auto ahit = loadSpv("shaders/rt_test.rahit.spv");
        auto translucentChit = loadSpv("shaders/rt_test_translucent.rchit.spv");
        auto translucentAhit = loadSpv("shaders/rt_test_translucent.rahit.spv");
        // D1：材质层 callable（K2 的 landing 解析经它执行；K1 不用）
        auto materialCode = loadSpv("shaders/rt_test_material.spv");

        VkShaderModule mods[11]{};
        const char* entry = "main";
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        auto makeModule = [&](const std::vector<uint8_t>& code, VkShaderModule& out)
        {
            mci.codeSize = code.size();
            mci.pCode = reinterpret_cast<const uint32_t*>(code.data());
            check(vkCreateShaderModule(g_device, &mci, nullptr, &out),
                  "Failed to create K1/K2 shader module");
        };
        makeModule(k1Code, mods[0]);
        if(k2Split)
        {
            makeModule(k2dCode, mods[9]);
            makeModule(k2iCode, mods[10]);
        }
        else
        {
            makeModule(k2Code, mods[1]);
        }
        makeModule(chit, mods[2]);
        makeModule(k1Chit, mods[3]);
        makeModule(miss, mods[4]);
        makeModule(ahit, mods[5]);
        makeModule(translucentChit, mods[6]);
        makeModule(translucentAhit, mods[7]);
        makeModule(materialCode, mods[8]);

        // 每条管线的 stage 0 换成自己的 rgen；stage 1（主命中组 chit）K1 用精简版；
        // stage 6 = 材质 callable（两条管线共用同一模块，SBT 单记录即可）
        VkPipelineShaderStageCreateInfo stages1[7]{};
        VkPipelineShaderStageCreateInfo stages2[7]{};
        const VkShaderStageFlagBits stageKinds[7] = {
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            VK_SHADER_STAGE_CALLABLE_BIT_KHR };
        const uint32_t stageModules[7] = { 0u, 2u, 4u, 5u, 6u, 7u, 8u };
        for(uint32_t i = 0u; i < 7u; ++i)
        {
            stages1[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages1[i].stage = stageKinds[i];
            stages1[i].module = mods[stageModules[i]];
            stages1[i].pName = entry;
            stages2[i] = stages1[i];
        }
        // 传输管线 rgen：默认 = K2 单发；拆分 = 先 K2-D 后 K2-I（创建前切换）
        stages2[0].module = k2Split ? mods[9] : mods[1];
        // 主命中组（含水）走精简 chit；玻璃命中组（stage 4/5）两条管线共用全功能版
        stages1[1].module = mods[3];

        VkRayTracingShaderGroupCreateInfoKHR groups[6]{};
        groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0u;
        groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;
        groups[1] = groups[0];
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[1].generalShader = VK_SHADER_UNUSED_KHR;
        groups[1].closestHitShader = 1u;
        groups[1].anyHitShader = 3u;
        groups[2] = groups[0];
        groups[2].generalShader = 2u;
        groups[3] = groups[1];
        groups[3].closestHitShader = 4u;
        groups[3].anyHitShader = 5u;
        groups[4] = groups[1];
        // D1：材质 callable 组（general = stage 6）
        groups[5] = groups[0];
        groups[5].generalShader = 6u;

        VkRayTracingPipelineCreateInfoKHR pci{
            VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pci.stageCount = 7u;
        pci.pStages = stages1;
        pci.groupCount = 6u;
        pci.pGroups = groups;
        pci.maxPipelineRayRecursionDepth = 1u;
        pci.layout = g_rayTracingPipelineLayout;
        if(g_ommNativeEnabled)
        {
            pci.flags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
        }

        check(OrionCreateRayTracingPipelineTimed(
                  "k1-primary", 1u, &pci, getSharedPipelineCache(),
                  &g_k1Pipeline),
              "Failed to create K1 RT pipeline");

        if(k2Split)
        {
            // ③ 拆分模式：K2-D（直射级）与 K2-I（间接级）两条管线，stages2
            // 其余 stage 与单发 K2 完全一致（rgen 已在上方切换为 mods[9]）。
            pci.pStages = stages2;
            auto tryCreateSplit = [&](const char* tag,
                                      VkPipelineCache cache,
                                      VkPipeline* out) -> VkResult
            {
                VkRayTracingPipelineCreateInfoKHR p = pci;
                p.pStages = stages2;
                return OrionCreateRayTracingPipelineTimed(
                    tag, 1u, &p, cache, out);
            };
            VkResult k2dR = tryCreateSplit(
                "k2d-direct", getSharedPipelineCache(), &g_k2dPipeline);
            if(k2dR != VK_SUCCESS && k2SerProbe)
            {
                char buf[192]{};
                std::snprintf(buf, sizeof(buf),
                              "[K2SERPROBE] k2d 主尝试 result=%d "
                              "stageCount=7 groupCount=6 flags=0x%x recursion=%u "
                              "layout=%p cache=%p",
                              static_cast<int>(k2dR), pci.flags,
                              pci.maxPipelineRayRecursionDepth,
                              reinterpret_cast<void*>(pci.layout),
                              reinterpret_cast<void*>(getSharedPipelineCache()));
                RTLog(buf);
                VkResult rNocache = tryCreateSplit(
                    "k2d-probe-nocache", VK_NULL_HANDLE, &g_k2dPipeline);
                RTLog(std::string("[K2SERPROBE] k2d nocache result=") +
                      std::to_string(static_cast<int>(rNocache)));
                if(rNocache != VK_SUCCESS)
                {
                    VkPipelineCache fresh = VK_NULL_HANDLE;
                    VkPipelineCacheCreateInfo fci{
                        VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
                    vkCreatePipelineCache(
                        g_device, &fci, nullptr, &fresh);
                    VkResult rFresh = tryCreateSplit(
                        "k2d-probe-freshcache", fresh, &g_k2dPipeline);
                    RTLog(std::string("[K2SERPROBE] k2d freshcache result=") +
                          std::to_string(static_cast<int>(rFresh)));
                    vkDestroyPipelineCache(g_device, fresh, nullptr);
                }
                if(g_k2dPipeline == VK_NULL_HANDLE)
                {
                    VkPipelineCreateFlags savedFlags = pci.flags;
                    pci.flags &=
                        ~VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
                    VkResult rNoOmm = tryCreateSplit(
                        "k2d-probe-noomm", getSharedPipelineCache(),
                        &g_k2dPipeline);
                    RTLog(std::string("[K2SERPROBE] k2d noomm result=") +
                          std::to_string(static_cast<int>(rNoOmm)));
                    pci.flags = savedFlags;
                }
                if(g_k2dPipeline != VK_NULL_HANDLE)
                {
                    RTLog("[K2SERPROBE] k2d 采用探针成功档");
                }
            }
            if(g_k2dPipeline == VK_NULL_HANDLE && serShaderRequested)
            {
                // SER 档最终失败（含 wrapper 内重试）→ 降级 plain 重建，
                // 保住拆分模式，只丢这两条传输腿的 SER。
                vkDestroyShaderModule(g_device, mods[9], nullptr);
                makeModule(loadSpv("shaders/rt_test_k2d.rgen.spv"), mods[9]);
                stages2[0].module = mods[9];
                RTLog("[K1/K2] k2d SER 档创建失败，降级 plain 重建");
                VkResult rPlain = tryCreateSplit(
                    "k2d-plain", getSharedPipelineCache(), &g_k2dPipeline);
                if(rPlain != VK_SUCCESS)
                {
                    check(rPlain, "Failed to create K2-D RT pipeline");
                }
            }
            if(g_k2dPipeline == VK_NULL_HANDLE)
            {
                check(k2dR, "Failed to create K2-D RT pipeline");
            }
            stages2[0].module = mods[10];
            VkResult k2iR = tryCreateSplit(
                "k2i-indirect", getSharedPipelineCache(), &g_k2iPipeline);
            if(k2iR != VK_SUCCESS && k2SerProbe)
            {
                VkResult rNocache = tryCreateSplit(
                    "k2i-probe-nocache", VK_NULL_HANDLE, &g_k2iPipeline);
                RTLog(std::string("[K2SERPROBE] k2i nocache result=") +
                      std::to_string(static_cast<int>(rNocache)));
                if(g_k2iPipeline == VK_NULL_HANDLE)
                {
                    VkPipelineCreateFlags savedFlags = pci.flags;
                    pci.flags &=
                        ~VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
                    VkResult rNoOmm = tryCreateSplit(
                        "k2i-probe-noomm", getSharedPipelineCache(),
                        &g_k2iPipeline);
                    RTLog(std::string("[K2SERPROBE] k2i noomm result=") +
                          std::to_string(static_cast<int>(rNoOmm)));
                    pci.flags = savedFlags;
                }
                if(g_k2iPipeline != VK_NULL_HANDLE)
                {
                    RTLog("[K2SERPROBE] k2i 采用探针成功档");
                }
            }
            if(g_k2iPipeline == VK_NULL_HANDLE && serShaderRequested)
            {
                vkDestroyShaderModule(g_device, mods[10], nullptr);
                makeModule(loadSpv("shaders/rt_test_k2i.rgen.spv"), mods[10]);
                stages2[0].module = mods[10];
                RTLog("[K1/K2] k2i SER 档创建失败，降级 plain 重建");
                VkResult rPlain = tryCreateSplit(
                    "k2i-plain", getSharedPipelineCache(), &g_k2iPipeline);
                if(rPlain != VK_SUCCESS)
                {
                    check(rPlain, "Failed to create K2-I RT pipeline");
                }
            }
            if(g_k2iPipeline == VK_NULL_HANDLE)
            {
                check(k2iR, "Failed to create K2-I RT pipeline");
            }
        }
        else
        {
            pci.pStages = stages2;
            check(OrionCreateRayTracingPipelineTimed(
                      "k2-transport", 1u, &pci, getSharedPipelineCache(),
                      &g_k2Pipeline),
                  "Failed to create K2 RT pipeline");
        }

        for(uint32_t i = 0u; i < 11u; ++i)
        {
            vkDestroyShaderModule(g_device, mods[i], nullptr);
        }

        auto getHandles =
            reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
                vkGetDeviceProcAddr(g_device,
                                    "vkGetRayTracingShaderGroupHandlesKHR"));
        if(!getHandles)
        {
            throw std::runtime_error(
                "vkGetRayTracingShaderGroupHandlesKHR unavailable");
        }

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
        rtProps.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 props2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &rtProps;
        vkGetPhysicalDeviceProperties2(g_physicalDevice, &props2);

        const uint32_t hs = rtProps.shaderGroupHandleSize;
        const uint32_t ha = rtProps.shaderGroupHandleAlignment;
        const uint32_t ba = rtProps.shaderGroupBaseAlignment;
        const uint32_t handleAligned = ((hs + ha - 1u) / ha) * ha;
        // 单 SBT 双 raygen 槽：K1@0，K2@对齐后的下一槽
        const uint32_t raygen1Offset = 0u;
        const uint32_t raygen2Offset =
            ((raygen1Offset + handleAligned + ba - 1u) / ba) * ba;
        // ③ 拆分模式：追加 K2-I raygen 槽、K2-D/K2-I 各自的 miss/hit/callable
        // 记录集（句柄按管线区分，不能互借）；默认模式偏移逐字节同旧布局。
        const uint32_t raygen3Offset =
            k2Split
                ? ((raygen2Offset + handleAligned + ba - 1u) / ba) * ba
                : 0u;
        const uint32_t missOffset =
            (((k2Split ? raygen3Offset : raygen2Offset) + handleAligned + ba - 1u) /
             ba) *
            ba;
        const uint32_t miss2dOffset =
            k2Split ? ((missOffset + handleAligned + ba - 1u) / ba) * ba : 0u;
        const uint32_t miss2iOffset =
            k2Split
                ? ((miss2dOffset + handleAligned + ba - 1u) / ba) * ba
                : 0u;
        const uint32_t hitOffset =
            (((k2Split ? miss2iOffset : missOffset) + handleAligned + ba - 1u) /
             ba) *
            ba;
        // D1：callable 记录（两条管线共用同一 callable 模块 ⇒ 单记录）。
        // 2026-11-05 修正：hit 记录**两份** —— K1 的 group1 是精简 rchit，
        // K2 的 group1 是全功能 rchit，共用一份会把 K2 的次级命中也路由到
        // 精简版（不算材质）⇒ 传输全黑。hit1 供 K1，hit2 供 K2（拆分模式下
        // hit2 = K2-D，hit3 = K2-I）。
        const uint32_t hit2Offset =
            ((hitOffset + SBT_HIT_RECORD_COUNT * handleAligned + ba - 1u) / ba) *
            ba;
        const uint32_t hit3Offset =
            k2Split
                ? ((hit2Offset + SBT_HIT_RECORD_COUNT * handleAligned + ba - 1u) /
                   ba) *
                      ba
                : 0u;
        const uint32_t callableOffset =
            (((k2Split ? hit3Offset : hit2Offset) +
              SBT_HIT_RECORD_COUNT * handleAligned + ba - 1u) /
             ba) *
            ba;
        const uint32_t callable2Offset =
            k2Split ? ((callableOffset + handleAligned + ba - 1u) / ba) * ba : 0u;
        const uint32_t sbtSize =
            (((k2Split ? callable2Offset : callableOffset) + handleAligned + ba - 1u) /
             ba) *
            ba;

        const uint32_t groupCount = 6u;
        std::vector<uint8_t> handles1((size_t)groupCount * hs, 0);
        std::vector<uint8_t> handles2((size_t)groupCount * hs, 0);
        std::vector<uint8_t> handles3((size_t)groupCount * hs, 0);
        check(getHandles(g_device, g_k1Pipeline, 0u,
                         groupCount, handles1.size(), handles1.data()),
              "Failed to get K1 RT shader handles");
        if(k2Split)
        {
            // ③ 拆分：handles2 = K2-D，handles3 = K2-I
            check(getHandles(g_device, g_k2dPipeline, 0u,
                             groupCount, handles2.size(), handles2.data()),
                  "Failed to get K2-D RT shader handles");
            check(getHandles(g_device, g_k2iPipeline, 0u,
                             groupCount, handles3.size(), handles3.data()),
                  "Failed to get K2-I RT shader handles");
        }
        else
        {
            check(getHandles(g_device, g_k2Pipeline, 0u,
                             groupCount, handles2.size(), handles2.data()),
                  "Failed to get K2 RT shader handles");
        }

        std::vector<uint8_t> data((size_t)sbtSize, 0);
        std::memcpy(data.data() + raygen1Offset, handles1.data(), hs);
        std::memcpy(data.data() + raygen2Offset, handles2.data(), hs);
        if(k2Split)
        {
            std::memcpy(data.data() + raygen3Offset,
                        handles3.data(), hs);
        }
        std::memcpy(data.data() + missOffset,
                    handles1.data() + 2u * hs, hs);
        if(k2Split)
        {
            std::memcpy(data.data() + miss2dOffset,
                        handles2.data() + 2u * hs, hs);
            std::memcpy(data.data() + miss2iOffset,
                        handles3.data() + 2u * hs, hs);
        }
        // hit 记录组与 giHalf 相同：opaque, opaque, alpha, water, opaque
        // K1 用 handles1（group1=精简 rchit），K2 用 handles2（group1=全功能
        // rchit）；玻璃组两管线同模块，两份取值相同。
        const uint32_t recordGroups[SBT_HIT_RECORD_COUNT] = {
            1u, 1u, 3u, 4u, 1u };
        for(uint32_t record = 0u; record < SBT_HIT_RECORD_COUNT; ++record)
        {
            std::memcpy(data.data() + hitOffset + record * handleAligned,
                        handles1.data() + recordGroups[record] * hs, hs);
            std::memcpy(data.data() + hit2Offset + record * handleAligned,
                        handles2.data() + recordGroups[record] * hs, hs);
            if(k2Split)
            {
                std::memcpy(data.data() + hit3Offset + record * handleAligned,
                            handles3.data() + recordGroups[record] * hs, hs);
            }
        }
        std::memcpy(data.data() + callableOffset,
                    handles2.data() + 5u * hs, hs);
        if(k2Split)
        {
            std::memcpy(data.data() + callable2Offset,
                        handles3.data() + 5u * hs, hs);
        }

        createBuffer(g_device, g_physicalDevice, sbtSize,
                     VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_k1k2SbtBuffer, g_k1k2SbtMemory, true);

        void* sbtMapped = nullptr;
        check(vkMapMemory(g_device, g_k1k2SbtMemory, 0u, sbtSize, 0u,
                          &sbtMapped),
              "map K1/K2 SBT failed");
        std::memcpy(sbtMapped, data.data(), sbtSize);
        vkUnmapMemory(g_device, g_k1k2SbtMemory);

        const uint64_t sbtAddress =
            getBufferDeviceAddress(g_device, g_k1k2SbtBuffer);

        // miss/hit 两组管线完全同句柄，region 两份取值相同
        for(int p = 0; p < 2; ++p)
        {
            VkStridedDeviceAddressRegionKHR& raygen =
                p == 0 ? g_k1RaygenRegion : g_k2RaygenRegion;
            raygen.deviceAddress = sbtAddress +
                (p == 0 ? raygen1Offset : raygen2Offset);
            raygen.stride = handleAligned;
            raygen.size = handleAligned;
        }
        g_k1MissRegion.deviceAddress = sbtAddress + missOffset;
        g_k1MissRegion.stride = handleAligned;
        g_k1MissRegion.size = handleAligned;
        g_k1HitRegion.deviceAddress = sbtAddress + hitOffset;
        g_k1HitRegion.stride = handleAligned;
        g_k1HitRegion.size = SBT_HIT_RECORD_COUNT * handleAligned;
        if(k2Split)
        {
            // ③ 拆分：K2-D/K2-I 各自的 region（单发 K2 的 region 不再使用）
            g_k2dRaygenRegion.deviceAddress = sbtAddress + raygen2Offset;
            g_k2dRaygenRegion.stride = handleAligned;
            g_k2dRaygenRegion.size = handleAligned;
            g_k2iRaygenRegion.deviceAddress = sbtAddress + raygen3Offset;
            g_k2iRaygenRegion.stride = handleAligned;
            g_k2iRaygenRegion.size = handleAligned;
            g_k2dMissRegion.deviceAddress = sbtAddress + miss2dOffset;
            g_k2dMissRegion.stride = handleAligned;
            g_k2dMissRegion.size = handleAligned;
            g_k2iMissRegion.deviceAddress = sbtAddress + miss2iOffset;
            g_k2iMissRegion.stride = handleAligned;
            g_k2iMissRegion.size = handleAligned;
            g_k2dHitRegion.deviceAddress = sbtAddress + hit2Offset;
            g_k2dHitRegion.stride = handleAligned;
            g_k2dHitRegion.size = SBT_HIT_RECORD_COUNT * handleAligned;
            g_k2iHitRegion.deviceAddress = sbtAddress + hit3Offset;
            g_k2iHitRegion.stride = handleAligned;
            g_k2iHitRegion.size = SBT_HIT_RECORD_COUNT * handleAligned;
            g_k2dCallableRegion.deviceAddress = sbtAddress + callableOffset;
            g_k2dCallableRegion.stride = handleAligned;
            g_k2dCallableRegion.size = handleAligned;
            g_k2iCallableRegion.deviceAddress = sbtAddress + callable2Offset;
            g_k2iCallableRegion.stride = handleAligned;
            g_k2iCallableRegion.size = handleAligned;
        }
        else
        {
            g_k2MissRegion = g_k1MissRegion;
            g_k2HitRegion.deviceAddress = sbtAddress + hit2Offset;
            g_k2HitRegion.stride = handleAligned;
            g_k2HitRegion.size = SBT_HIT_RECORD_COUNT * handleAligned;
        }
        // D1：材质 callable 记录（两条管线共用；K1 的 rgen 不发起 callable，
        // region 挂上对 K1 无影响）
        g_k1CallableRegion.deviceAddress = sbtAddress + callableOffset;
        g_k1CallableRegion.stride = handleAligned;
        g_k1CallableRegion.size = handleAligned;
        if(!k2Split)
        {
            g_k2CallableRegion = g_k1CallableRegion;
        }

        {
            std::ostringstream dbg;
            dbg << "[K1/K2] SBT addr=0x" << std::hex << sbtAddress
                << " hs=" << std::dec << hs << " ha=" << ha << " ba=" << ba
                << " rg1=" << raygen1Offset << " rg2=" << raygen2Offset
                << " miss=" << missOffset << " hit1=" << hitOffset
                << " hit2=" << hit2Offset
                << " call=" << callableOffset << " size=" << sbtSize;
            RTLog(dbg.str());
        }

        std::cerr << "[K1/K2] 管线已创建（k1=主视线落盘, k2=读盘续算传输）"
                  << std::endl;

        // 2026-11-06：同 GI HALF，改为交给 maybePersist 去抖。
        g_pipelineCachePersistCountdown = 60u;
    }
    catch(const std::exception& error)
    {
        std::cerr << "[K1/K2] 管线创建失败（本会话不再重试，回退单发主管线）："
                  << error.what() << std::endl;
        destroyK1K2Pipeline();
        g_k1K2PipelineFailed = true;
        g_k1Pipeline = VK_NULL_HANDLE;
        g_k2Pipeline = VK_NULL_HANDLE;
    }
}
bool traceK1K2(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if(!k1k2SplitEnabled())
    {
        return false;
    }
    if(g_rayTracingPipeline == VK_NULL_HANDLE ||
       g_rayTracingPipelineLayout == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return false;
    }

    ensureK1K2Pipelines();
    if(g_k1Pipeline == VK_NULL_HANDLE || g_k2Pipeline == VK_NULL_HANDLE)
    {
        return false;
    }

    // slotCount 两侧同式：max(1, int(clamp(spp,0.5,4.0)+0.5))
    const float spp = std::clamp(g_perfRtSpp.load(), 0.5f, 4.0f);
    const uint32_t slots = (uint32_t)std::max(1, (int)(spp + 0.5f));
    ensureK1LandingBuffer(width, height, slots);
    if(g_k1LandingBuffer == VK_NULL_HANDLE)
    {
        return false;
    }
    // 描述符集若被重建，binding 88 需要重写
    if(s_k1LandingBoundSet != g_rayTracingDescriptorSet)
    {
        writeK1LandingDescriptor();
    }

    auto trace = getCmdTraceRays();
    if(!trace)
    {
        return false;
    }

    // 崩溃二分（2026-11-05）：首个 K1/K2 dispatch 在驱动里 AV（读 0x1c8），
    // 头 3 帧逐调用打点定位是 bind/trace/barrier 哪一步。
    // ORION_K1K2_K1_ONLY=1：只录 K1，跳过 K2（测故障在 K1 侧还是 K2 侧）。
    static const bool k1k2K1Only = [](){
        const char* raw = std::getenv("ORION_K1K2_K1_ONLY");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();
    static int s_k1k2TraceLog = 0;
    const bool logTrace = s_k1k2TraceLog < 3;
    if(logTrace) ++s_k1k2TraceLog;
    if(logTrace) RTLog("[K1/K2] pre bind-k1");

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      g_k1Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            g_rayTracingPipelineLayout, 0u, 1u,
                            &g_rayTracingDescriptorSet, 0u, nullptr);
    if(logTrace) RTLog("[K1/K2] pre trace-k1");
    trace(cmd, &g_k1RaygenRegion, &g_k1MissRegion,
          &g_k1HitRegion, &g_k1CallableRegion, width, height, 1u);
    if(logTrace) RTLog("[K1/K2] post trace-k1");

    if(k1k2K1Only)
    {
        if(logTrace) RTLog("[K1/K2] K1_ONLY: 跳过 K2");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          g_rayTracingPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                g_rayTracingPipelineLayout, 0u, 1u,
                                &g_rayTracingDescriptorSet, 0u, nullptr);
        return true;
    }

    if(logTrace) RTLog("[K1/K2] pre bind-k2");

    // K1→K2 的交接不止 landing 一个 buffer：K1 还写了 depth/MV/guide 等
    // 图像，K2 的传输段要读（A1 时域重投影读 MV）。只对 landing 设 buffer
    // barrier 时图像侧是竞态 ⇒ K2 随机读到陈旧/半写 MV ⇒ 重投影扭曲+噪点。
    // 全局 memory barrier 一次覆盖全部 buffer/图像。
    VkMemoryBarrier k1k2Barrier{
        VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    k1k2Barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    k1k2Barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);

    // ③ K2-D/K2-I 再拆分（默认开，ORION_K2_SPLIT=0 回退）：K1→K2D（直射级）→barrier→
    // K2I（间接级，负责 GI/稀疏合成/公共尾段/最终输出）。两次交接都过同一
    // 全局 barrier（landing d 字段与 k2DirectImage 都是上一级的写）。
    if(k2DirectIndirectSplitEnabled())
    {
        if(g_k2dPipeline == VK_NULL_HANDLE || g_k2iPipeline == VK_NULL_HANDLE)
        {
            return false;
        }
        createK2DirectImage(width, height);

        if(logTrace) RTLog("[K1/K2] pre bind-k2d");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          g_k2dPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                g_rayTracingPipelineLayout, 0u, 1u,
                                &g_rayTracingDescriptorSet, 0u, nullptr);
        trace(cmd, &g_k2dRaygenRegion, &g_k2dMissRegion,
              &g_k2dHitRegion, &g_k2dCallableRegion, width, height, 1u);
        if(logTrace) RTLog("[K1/K2] post trace-k2d");

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);

        if(logTrace) RTLog("[K1/K2] pre bind-k2i");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          g_k2iPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                g_rayTracingPipelineLayout, 0u, 1u,
                                &g_rayTracingDescriptorSet, 0u, nullptr);
        trace(cmd, &g_k2iRaygenRegion, &g_k2iMissRegion,
              &g_k2iHitRegion, &g_k2iCallableRegion, width, height, 1u);
        if(logTrace) RTLog("[K1/K2] post trace-k2i");

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          g_rayTracingPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                g_rayTracingPipelineLayout, 0u, 1u,
                                &g_rayTracingDescriptorSet, 0u, nullptr);
        return true;
    }

    if(logTrace) RTLog("[K1/K2] pre bind-k2");

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      g_k2Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            g_rayTracingPipelineLayout, 0u, 1u,
                            &g_rayTracingDescriptorSet, 0u, nullptr);
    if(logTrace) RTLog("[K1/K2] pre trace-k2");
    trace(cmd, &g_k2RaygenRegion, &g_k2MissRegion,
          &g_k2HitRegion, &g_k2CallableRegion, width, height, 1u);
    if(logTrace) RTLog("[K1/K2] post trace-k2");

    // K1/K2 两发录完，把主管线绑回去——后续 Frame 代码假设主管线在绑
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      g_rayTracingPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            g_rayTracingPipelineLayout, 0u, 1u,
                            &g_rayTracingDescriptorSet, 0u, nullptr);
    return true;
}
void sharcStatsRecordReadBack(VkCommandBuffer commandBuffer)
{
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = g_sharcLockBuffer;
    barrier.offset = 0u;
    barrier.size = 128u;
    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 1u, &barrier, 0u, nullptr);

    VkBufferCopy copy{};
    copy.srcOffset = 0u;
    copy.dstOffset = 0u;
    copy.size = 128u;

    vkCmdCopyBuffer(
        commandBuffer,
        g_sharcLockBuffer,
        g_sharcProbeStagingBuffer,
        1u,
        &copy);
}
void sharcStatsTick()
{
    if(
        g_sharcStatsDone ||
        !g_sharcZeroed ||
        !g_sharcBuffersReady
    )
    {
        return;
    }

    ++g_sharcStatsFrameTick;

    if(g_sharcStatsFrameTick < SHARC_STATS_FRAMES)
    {
        return;
    }

    ensureSharcStaging();

    if(g_sharcProbeStagingBuffer == VK_NULL_HANDLE)
    {
        g_sharcStatsDone = true;
        return;
    }

    sharcProbeOneShotSubmit(
        sharcStatsRecordReadBack,
        "SHaRC stats read-back submit failed");

    uint32_t words[32] = { 0u };

    void* mapped = nullptr;
    check(
        vkMapMemory(g_device, g_sharcProbeStagingMemory, 0u, 128u, 0u, &mapped),
        "map SHaRC stats staging failed");

    std::memcpy(words, mapped, 128u);
    vkUnmapMemory(g_device, g_sharcProbeStagingMemory);

    const double sharcUpdatesPerFrame =
        static_cast<double>(words[8]) /
        static_cast<double>(g_sharcStatsFrameTick);
    const double sharcFailsPerFrame =
        static_cast<double>(words[10]) /
        static_cast<double>(g_sharcStatsFrameTick);
    const double sharcHitsPerFrame =
        static_cast<double>(words[11]) /
        static_cast<double>(g_sharcStatsFrameTick);

    const auto sharcSampledPerFrame =
        [](uint32_t value, uint32_t frames) -> double
        {
            return frames != 0u
                ? 256.0 * static_cast<double>(value) / static_cast<double>(frames)
                : 0.0;
        };

    char line[1200];
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[SHARC STATS] frames=%u | GI跳数=%u | update=%.0f/frame fails=%.0f/frame | "
        "queryHits=%.0f/frame | query到达=%.0f/frame | lock0=%08X"
        " | 5b-4 口径：update = update pass 每帧成功写入缓存的**顶点数**（≈选中像素数"
        "×平均路径顶点数，选中像素 ≈ 渲染像素/25）；fails = SharcUpdateHit 返回 false，"
        "有两种原因——哈希插入失败 或 缓存重采样命中，缓存变热后后者占大头，"
        "fails 高不代表网格饱和；query到达 = 渲染 pass 走到查询点的次数（lock[12]，"
        "1/256 抽样 ×256 还原）；queryHits = 真正命中缓存并截断路径的次数"
        "（lock[11]，全量）。判读：queryHits>0 即 update→render→resolve 整条链通"
        "（前提 GI跳数≥2）；query到达>0 而 queryHits=0 ⇒ 守卫通过但查不到"
        "（缓存没被填充 或 存取键不一致）",
        g_sharcStatsFrameTick,
        g_perfGiBounces.load(),
        sharcUpdatesPerFrame,
        sharcFailsPerFrame,
        sharcHitsPerFrame,
        sharcSampledPerFrame(words[12], g_sharcStatsFrameTick),
        words[0]);

    RTLogFlushed(line);

    g_sharcStatsDone = true;
}
