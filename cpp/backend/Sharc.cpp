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

        auto createPipe =
            reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
                vkGetDeviceProcAddr(g_device, "vkCreateRayTracingPipelinesKHR"));
        if(!createPipe)
        {
            throw std::runtime_error("vkCreateRayTracingPipelinesKHR unavailable");
        }
        check(createPipe(g_device, VK_NULL_HANDLE, getSharedPipelineCache(), 1u,
                         &pci, nullptr, &g_sharcUpdatePipeline),
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

        auto createPipe =
            reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
                vkGetDeviceProcAddr(g_device, "vkCreateRayTracingPipelinesKHR"));
        if(!createPipe)
        {
            throw std::runtime_error("vkCreateRayTracingPipelinesKHR unavailable");
        }
        check(createPipe(g_device, VK_NULL_HANDLE, getSharedPipelineCache(), 1u,
                         &pci, nullptr, &g_giHalfPipeline),
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

        persistSharedPipelineCache();
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
