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

    const int mode = g_indirectModeOverride.load(std::memory_order_relaxed);
    return mode != 0;
}
float sharcSceneScale()
{
    static const float cached = []()
    {
        const char* raw = std::getenv("ORION_SHARC_SCALE");
        if(raw == nullptr || raw[0] == '\0')
        {
            return 24.0f;
        }

        const float parsed = static_cast<float>(std::atof(raw));
        return parsed > 0.0f ? parsed : 24.0f;
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
            return 20u;
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
uint32_t sharcUpdateBlockForResolution(uint32_t width, uint32_t height)
{
    const double pixels = static_cast<double>(std::max(1u, width)) *
                          static_cast<double>(std::max(1u, height));
    const uint32_t block = static_cast<uint32_t>(
        std::lround(std::sqrt(pixels / 14000.0)));
    return std::clamp(block, 2u, 8u);
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

    uint32_t resolvedMode = 0u;
    if(sharcUpdate && sharcQuery)
    {
        resolvedMode = 3u;
    }
    else if(sharcUpdate)
    {
        resolvedMode = 4u;
    }
    else if(sharcQuery)
    {
        resolvedMode = 2u;
    }

    if(resolvedMode == 0u)
    {
        return 0u;
    }

    const uint32_t block =
        sharcUpdateBlockForResolution(g_rtOutputWidth, g_rtOutputHeight);
    return resolvedMode | (block << 8u);
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

        g_rrSpecularMotionImage !=
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

        g_rrSpecularMotionImage !=
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
        g_rrSpecularMotionImage,
        g_rrSpecularMotionMemory,
        g_rrSpecularMotionView,
        g_rrSpecularMotionLayout
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
        VK_FORMAT_R16G16_SFLOAT,
        g_rrSpecularMotionImage,
        g_rrSpecularMotionMemory,
        g_rrSpecularMotionView,
        g_rrSpecularMotionLayout
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


// ============================================================================
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

    VkPushConstantRange resolvePushRange{};
    resolvePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    resolvePushRange.offset = 0u;
    resolvePushRange.size = 8u;

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    pipelineLayoutInfo.setLayoutCount = 1u;
    pipelineLayoutInfo.pSetLayouts = &g_rayTracingDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1u;
    pipelineLayoutInfo.pPushConstantRanges = &resolvePushRange;

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
        const bool k1k2NoSer =
            [](){
                const char* raw = std::getenv("ORION_K1K2_NO_SER");
                return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
            }();
        const bool serShaderRequested =
            !k1k2NoSer && serShaderVariantWanted();
        std::vector<uint8_t> updateRgen;
        if(serShaderRequested)
        {
            try
            {
                updateRgen = loadSpv("shaders/rt_test_sharc_update_ser.rgen.spv");
            }
            catch(const std::exception&)
            {
                RTLog("[SHARC] ser spv 加载失败，回退 plain: shaders/rt_test_sharc_update_ser.rgen.spv");
            }
        }
        if(updateRgen.empty())
        {
            updateRgen = loadSpv("shaders/rt_test_sharc_update.rgen.spv");
        }
        auto chit = loadSpv("shaders/rt_test.rchit.spv");
        auto miss = loadSpv("shaders/rt_test_nocloud.rmiss.spv");
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

        uploadDeviceLocalBufferOnce(
            data.data(),
            sbtSize,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            g_sharcUpdateSbtBuffer,
            g_sharcUpdateSbtMemory);

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
              "K2-I 轻量化变体 + 自适应块级选点）");
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
namespace
{
    uint32_t sharcResolveParityValue()
    {
        static const uint32_t sSharcResolveParity = []()
        {
            const char* raw = std::getenv("ORION_SHARC_RESOLVE_PARITY");
            if(raw == nullptr || raw[0] == '\0')
            {
                return 1u;
            }
            const int parsed = std::atoi(raw);
            if(parsed < 1)
            {
                return 1u;
            }
            return static_cast<uint32_t>(parsed > 4 ? 4 : parsed);
        }();
        return sSharcResolveParity;
    }

    uint32_t s_sharcResolveTick = 0u;

    bool sharcResolveDueThisFrame()
    {
        if(!g_sharcZeroed || !g_sharcBuffersReady)
        {
            return false;
        }
        const uint32_t parity = sharcResolveParityValue();
        return parity <= 1u || (s_sharcResolveTick % parity) == 0u;
    }

    VkPipelineLayout s_k2iCompactPipelineLayout = VK_NULL_HANDLE;

    struct CompactSlotHeaderHost
    {
        uint32_t width;
        uint32_t height;
        uint32_t depth;
        uint32_t mode;
        uint32_t activeCount;
        uint32_t baseElemOffset;
        uint32_t pad0;
        uint32_t pad1;
    };
    static_assert(
        sizeof(CompactSlotHeaderHost) == 32u,
        "CompactSlotHeaderHost must match 32-byte shader header");

    void recordCompactIndirectTrace(
        VkCommandBuffer cmd,
        uint32_t width,
        uint32_t height,
        VkPipeline compactPipeline,
        VkPipeline rtPipeline,
        const VkStridedDeviceAddressRegionKHR* raygenRegion,
        const VkStridedDeviceAddressRegionKHR* missRegion,
        const VkStridedDeviceAddressRegionKHR* hitRegion,
        const VkStridedDeviceAddressRegionKHR* callableRegion,
        PFN_vkCmdTraceRaysIndirectKHR traceIndirect,
        uint32_t compactMode = 1u,
        uint32_t dispatchX = 0u,
        uint32_t dispatchY = 0u,
        uint32_t slotIndex = 0u);
    void recordCompactBufferDisabledFallback(
        VkCommandBuffer cmd,
        uint32_t height,
        uint32_t slotIndex = 0u);
}

void recordSharcUpdateImpl(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height,
    bool skipTrailingBarrier)
{
    if(!g_sharcZeroed || !g_sharcBuffersReady || g_matReuseBuffer == VK_NULL_HANDLE)
    {
        return;
    }
    if(g_rayTracingPipelineLayout == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    if(!sharcUpdateEnabled())
    {
        return;
    }

    ensureSharcUpdatePipeline();
    if(g_sharcUpdatePipeline == VK_NULL_HANDLE)
    {
        return;
    }

    auto trace = getCmdTraceRays();
    if(!trace)
    {
        return;
    }

    createK2DirectImage(width, height);
    ensureK2iCompactResources(width, height);

    const uint32_t sharcUpdateBlock =
        sharcUpdateBlockForResolution(width, height);
    const uint32_t sharcUpdateWidth =
        std::max(1u, (width + sharcUpdateBlock - 1u) / sharcUpdateBlock);
    const uint32_t sharcUpdateHeight =
        std::max(1u, (height + sharcUpdateBlock - 1u) / sharcUpdateBlock);

    auto traceIndirect = getCmdTraceRaysIndirect();
    const bool useCompact =
        traceRaysIndirectSupported() &&
        traceIndirect != nullptr &&
        g_sharcCompactPipeline != VK_NULL_HANDLE &&
        s_k2iCompactPipelineLayout != VK_NULL_HANDLE &&
        g_k2iCompactBuffer != VK_NULL_HANDLE &&
        g_k2iCompactBufferAddress != 0u &&
        g_k2DirectView != VK_NULL_HANDLE;

    {
        ScopedVkDebugLabel sharcUpLabel(cmd, "SHaRC-Update");
        if(useCompact)
        {
            recordCompactIndirectTrace(
                cmd, width, height, g_sharcCompactPipeline, g_sharcUpdatePipeline,
                &g_sharcUpdateRaygenRegion, &g_sharcUpdateMissRegion,
                &g_sharcUpdateHitRegion, &g_sharcUpdateCallableRegion,
                traceIndirect, 1u,
                (sharcUpdateWidth + 15u) / 16u, (sharcUpdateHeight + 15u) / 16u, 5u);
        }
        else
        {
            recordCompactBufferDisabledFallback(cmd, height, 5u);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                              g_sharcUpdatePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                    g_rayTracingPipelineLayout, 0u, 1u,
                                    &g_rayTracingDescriptorSet, 0u, nullptr);
            trace(cmd, &g_sharcUpdateRaygenRegion, &g_sharcUpdateMissRegion,
                  &g_sharcUpdateHitRegion, &g_sharcUpdateCallableRegion,
                  sharcUpdateWidth, sharcUpdateHeight, 1u);
        }
    }

    if(!skipTrailingBarrier)
    {
        sharcCacheBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                          VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR);
    }
}
void recordSharcUpdate(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    recordSharcUpdateImpl(cmd, width, height, false);
}
static void recordSharcResolveImpl(VkCommandBuffer cmd, bool skipLeadingBarrier)
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
    // ORION_SHARC_RESOLVE_PARITY=1 回到每帧（默认 1 = 拆灯/熄灯即时响应）。
    const uint32_t parity = sharcResolveParityValue();
    if(parity > 1u && (s_sharcResolveTick++ % parity) != 0u)
    {
        return;
    }

    if(!skipLeadingBarrier)
    {
        sharcCacheBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    {
        ScopedVkDebugLabel sharcResLabel(cmd, "SHaRC-Resolve");
        vkCmdBindPipeline(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_sharcResolvePipeline);

        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_sharcResolvePipelineLayout,
            0u, 1u, &g_rayTracingDescriptorSet,
            0u, nullptr);

        const uint32_t sliceStride = (parity <= 1u) ? 2u : 1u;
        const uint32_t sliceOffset = (sliceStride > 1u)
            ? static_cast<uint32_t>(g_perfL10FrameSerial & 1u)
            : 0u;
        const uint32_t sharcPush[2] = { sliceStride, sliceOffset };
        vkCmdPushConstants(
            cmd, g_sharcResolvePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0u, sizeof(sharcPush), sharcPush);

        const uint32_t sliceEntries =
            (g_sharcEntryCountResolved + sliceStride - 1u) / sliceStride;
        const uint32_t sharcResolveGroups =
            (sliceEntries + 255u) / 256u;

        vkCmdDispatch(cmd, sharcResolveGroups, 1u, 1u);
    }

    sharcCacheBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
}
void recordSharcResolve(VkCommandBuffer cmd)
{
    recordSharcResolveImpl(cmd, false);
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
        (VkDeviceSize)width * height * slots * 64ull;
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

// ============================================================================
// P2（2026-11-17）：K2 三腿材质结果中转 buffer（binding 94）。
//
// 仅 ORION_MAT_REUSE_LEGS=1 时分配；关闭时 g_matReuseBuffer 保持空，着色器走原
// resolveHitMaterial 路径、不访问本槽位（与 binding 88/89 同一套"共享 layout
// 声明、未绑定时不被引用"的约定）。记录 64 B（4×uvec4，整条 resolve 后 payload
// 材质快照，浮点按位序存），下标与 landing 记录同式：pixelLinear * slots + s。
// 生命周期照 landing 缓冲：分辨率 / spp 变化整张重建；描述符集被环形机制重建后
// 随动重写。
// ============================================================================
namespace
{
VkDescriptorSet s_matReuseBoundSet = VK_NULL_HANDLE;
void writeMatReuseDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_matReuseBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorBufferInfo info{};
    info.buffer = g_matReuseBuffer;
    info.offset = 0u;
    info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 94;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;

    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);
    s_matReuseBoundSet = g_rayTracingDescriptorSet;
}
}
void destroyMatReuseBuffer()
{
    if(g_matReuseBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_matReuseBuffer, nullptr);
        g_matReuseBuffer = VK_NULL_HANDLE;
    }
    if(g_matReuseMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_matReuseMemory, nullptr);
        g_matReuseMemory = VK_NULL_HANDLE;
    }
    g_matReuseWidth = 0u;
    g_matReuseHeight = 0u;
    g_matReuseSlots = 0u;
    s_matReuseBoundSet = VK_NULL_HANDLE;
}
void ensureMatReuseBuffer(uint32_t width, uint32_t height, uint32_t slots)
{
    // 仅"复用开关打开"且"④ 三腿 / ⑤ 四腿模式"才分配。③ 两腿模式没有 SUN(A)
    // 生产者腿，中转记录无人写；此模式下不分配、着色器走原路径（与 UBO lane0
    // 的有效闸门同口径）。⑤ 四腿模式生产者 = SUNA。
    if(!matReuseLegsEnabled() ||
       !(k2FourSplitEnabled() || k2ThreeSplitEnabled()))
    {
        if(g_matReuseBuffer != VK_NULL_HANDLE)
        {
            FGDeviceWaitIdle(g_device);
            destroyMatReuseBuffer();
        }
        return;
    }

    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       width == 0u || height == 0u || slots == 0u)
    {
        return;
    }
    if(g_matReuseBuffer != VK_NULL_HANDLE &&
       g_matReuseWidth == width && g_matReuseHeight == height &&
       g_matReuseSlots == slots)
    {
        // 描述符集可能被环形机制重建过，binding 94 需要随动。
        if(s_matReuseBoundSet != g_rayTracingDescriptorSet)
        {
            writeMatReuseDescriptor();
        }
        return;
    }

    // 老缓冲可能还有在飞引用
    FGDeviceWaitIdle(g_device);
    destroyMatReuseBuffer();

    const VkDeviceSize bytes =
        (VkDeviceSize)width * height * slots * 32ull;
    createBuffer(g_device, g_physicalDevice, bytes,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 g_matReuseBuffer, g_matReuseMemory, false);
    if(g_matReuseBuffer == VK_NULL_HANDLE)
    {
        std::cerr << "[P2 复用] 中转缓冲创建失败（" << bytes << " 字节）"
                  << std::endl;
        return;
    }
    g_matReuseWidth = width;
    g_matReuseHeight = height;
    g_matReuseSlots = slots;

    writeMatReuseDescriptor();
    std::cerr << "[P2 复用] 中转缓冲就绪 " << width << "x" << height
              << " x" << slots << "spp (" << bytes << " 字节, binding 94)"
              << std::endl;
}
// ============================================================================
// 本地光 NEE 可见性跨腿缓存（binding 95）
//
// 目的：同一帧里 K1 / K2LOC / K2I 三条腿会对**同一个接收点、同一个选中光样本**
// 各打一条同样的硬件可见性射线（局 F 实测这条链合计 1.57 ms）。这里把射线的返回
// 透射率按 (pixel, 本 invocation 内第几次 NEE 调用) 存下来，后续腿命中即复用。
//
// 安全边界：记录里带 64 位键（receiverOrigin / selectedSample /
// targetVoxelEntryDistance / cullMask 的位哈希）和本帧标记，**只有两者都逐位相同
// 才复用**；不同就自己打射线并把结果写回。所以键不匹配只损失收益，不会算错；
// 键相同时是同一条线段、同一帧静态 TLAS，遍历结果必然相同。
//
// 记录 32 B（8×uint32）：[0]keyLo [1]keyHi [2..4]transmittance.xyz 位
// [5]frameTag [6]callIndex(诊断) [7]保留。下标 = pixelLinear * 4 + callIndex。
// 仅 ORION_LOCAL_NEE_CACHE=1 时分配；关闭时保持空，着色器闸门关着不访问本槽位。
// ============================================================================
namespace
{
VkDescriptorSet s_neeVisCacheBoundSet = VK_NULL_HANDLE;
void writeNeeVisCacheDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_neeVisCacheBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorBufferInfo info{};
    info.buffer = g_neeVisCacheBuffer;
    info.offset = 0u;
    info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 95;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;

    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);
    s_neeVisCacheBoundSet = g_rayTracingDescriptorSet;
}
} // namespace

void destroyNeeVisCacheBuffer()
{
    if(g_neeVisCacheBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_neeVisCacheBuffer, nullptr);
        g_neeVisCacheBuffer = VK_NULL_HANDLE;
    }
    if(g_neeVisCacheMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_neeVisCacheMemory, nullptr);
        g_neeVisCacheMemory = VK_NULL_HANDLE;
    }
    g_neeVisCacheWidth = 0u;
    g_neeVisCacheHeight = 0u;
    s_neeVisCacheBoundSet = VK_NULL_HANDLE;
}

void ensureNeeVisCacheBuffer(uint32_t width, uint32_t height)
{
    if(!localNeeVisCacheEnabled())
    {
        if(g_neeVisCacheBuffer != VK_NULL_HANDLE)
        {
            FGDeviceWaitIdle(g_device);
            destroyNeeVisCacheBuffer();
        }
        return;
    }

    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       width == 0u || height == 0u)
    {
        return;
    }
    if(g_neeVisCacheBuffer != VK_NULL_HANDLE &&
       g_neeVisCacheWidth == width && g_neeVisCacheHeight == height)
    {
        if(s_neeVisCacheBoundSet != g_rayTracingDescriptorSet)
        {
            writeNeeVisCacheDescriptor();
        }
        return;
    }

    FGDeviceWaitIdle(g_device);
    destroyNeeVisCacheBuffer();

    // 每像素 4 个槽位（最多覆盖"主命中 + 2 次弹射"这条链；超出即退回自己打射线）。
    const VkDeviceSize bytes =
        (VkDeviceSize)width * height * 4ull * 32ull;
    createBuffer(g_device, g_physicalDevice, bytes,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                 g_neeVisCacheBuffer, g_neeVisCacheMemory, false);
    if(g_neeVisCacheBuffer == VK_NULL_HANDLE)
    {
        RTLog("[本地NEE缓存] 缓冲创建失败（" + std::to_string(bytes) + " 字节）");
        return;
    }
    g_neeVisCacheWidth = width;
    g_neeVisCacheHeight = height;

    writeNeeVisCacheDescriptor();
    RTLog(
        "[本地NEE缓存] 就绪 " + std::to_string(width) + "x" +
        std::to_string(height) + " x4 槽 (" + std::to_string(bytes) +
        " 字节, binding 95)");
}

// ============================================================================
// 2026-11-24: K2-I 稀疏活跃像素紧凑间接调度资源（binding 92 / 100）
// ============================================================================
namespace
{
VkDescriptorSet s_k2iCompactBoundSet = VK_NULL_HANDLE;
bool s_k2iComposePipelineFailed = false;

void writeK2iCompactDescriptors()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_k2GiTraceView == VK_NULL_HANDLE ||
       g_k2iCompactBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageView = g_k2GiTraceView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = g_k2iCompactBuffer;
    bufInfo.offset = 0u;
    bufInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = g_rayTracingDescriptorSet;
    writes[0].dstBinding = 92u;
    writes[0].descriptorCount = 1u;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &imgInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = g_rayTracingDescriptorSet;
    writes[1].dstBinding = 100u;
    writes[1].descriptorCount = 1u;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufInfo;

    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);
    s_k2iCompactBoundSet = g_rayTracingDescriptorSet;
}
} // namespace

void destroyK2iCompactResources()
{
    if(g_k2iCompactPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2iCompactPipeline, nullptr);
        g_k2iCompactPipeline = VK_NULL_HANDLE;
    }
    if(g_k2locCompactPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2locCompactPipeline, nullptr);
        g_k2locCompactPipeline = VK_NULL_HANDLE;
    }
    if(g_k1GlassCompactPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k1GlassCompactPipeline, nullptr);
        g_k1GlassCompactPipeline = VK_NULL_HANDLE;
    }
    if(g_sharcCompactPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_sharcCompactPipeline, nullptr);
        g_sharcCompactPipeline = VK_NULL_HANDLE;
    }
    if(g_k2MegaCompactPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2MegaCompactPipeline, nullptr);
        g_k2MegaCompactPipeline = VK_NULL_HANDLE;
    }
    if(s_k2iCompactPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, s_k2iCompactPipelineLayout, nullptr);
        s_k2iCompactPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_k2iCompactBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_k2iCompactBuffer, nullptr);
        g_k2iCompactBuffer = VK_NULL_HANDLE;
    }
    if(g_k2iCompactMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_k2iCompactMemory, nullptr);
        g_k2iCompactMemory = VK_NULL_HANDLE;
    }
    g_k2iCompactBufferAddress = 0u;
    g_k2iCompactCapacityPixels = 0u;
    if(g_k2GiTraceImage != VK_NULL_HANDLE)
    {
        destroyHistoryImage(
            g_k2GiTraceImage,
            g_k2GiTraceMemory,
            g_k2GiTraceView,
            g_k2GiTraceLayout);
    }
    g_k2GiTraceWidth = 0u;
    g_k2GiTraceHeight = 0u;
    s_k2iCompactBoundSet = VK_NULL_HANDLE;
    g_k2iCompactPipelineFailed = false;
    g_k2locCompactPipelineFailed = false;
    g_k1GlassCompactPipelineFailed = false;
    g_sharcCompactPipelineFailed = false;
    g_k2MegaCompactPipelineFailed = false;
}

void ensureK2iCompactResources(uint32_t width, uint32_t height)
{
    if(!k2DirectIndirectSplitEnabled() ||
       g_device == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSetLayout == VK_NULL_HANDLE ||
       width == 0u || height == 0u)
    {
        return;
    }

    const uint32_t pixels = width * height;
    const bool imageMatches =
        g_k2GiTraceImage != VK_NULL_HANDLE &&
        g_k2GiTraceWidth == width &&
        g_k2GiTraceHeight == height;
    const bool bufferMatches =
        g_k2iCompactBuffer != VK_NULL_HANDLE &&
        g_k2iCompactCapacityPixels >= pixels &&
        g_k2iCompactBufferAddress != 0u;

    if(!imageMatches || !bufferMatches)
    {
        FGDeviceWaitIdle(g_device);
        if(!imageMatches)
        {
            if(g_k2GiTraceImage != VK_NULL_HANDLE)
            {
                destroyHistoryImage(
                    g_k2GiTraceImage,
                    g_k2GiTraceMemory,
                    g_k2GiTraceView,
                    g_k2GiTraceLayout);
            }
            createRRGuideImage(
                width,
                height,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                g_k2GiTraceImage,
                g_k2GiTraceMemory,
                g_k2GiTraceView,
                g_k2GiTraceLayout);
            g_k2GiTraceWidth = width;
            g_k2GiTraceHeight = height;
            s_k2iCompactBoundSet = VK_NULL_HANDLE;
        }
        if(!bufferMatches)
        {
            if(g_k2iCompactBuffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(g_device, g_k2iCompactBuffer, nullptr);
                g_k2iCompactBuffer = VK_NULL_HANDLE;
            }
            if(g_k2iCompactMemory != VK_NULL_HANDLE)
            {
                vkFreeMemory(g_device, g_k2iCompactMemory, nullptr);
                g_k2iCompactMemory = VK_NULL_HANDLE;
            }
            g_k2iCompactBufferAddress = 0u;
            g_k2iCompactCapacityPixels = 0u;
            const VkDeviceSize bytes =
                6ull * sizeof(CompactSlotHeaderHost) +
                6ull * static_cast<VkDeviceSize>(pixels) * 4ull;
            createBuffer(
                g_device,
                g_physicalDevice,
                bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                g_k2iCompactBuffer,
                g_k2iCompactMemory,
                false);
            if(g_k2iCompactBuffer != VK_NULL_HANDLE)
            {
                g_k2iCompactBufferAddress = getBufferDeviceAddress(g_device, g_k2iCompactBuffer);
                g_k2iCompactCapacityPixels = pixels;
                s_k2iCompactBoundSet = VK_NULL_HANDLE;
            }
        }
    }

    if(s_k2iCompactBoundSet != g_rayTracingDescriptorSet)
    {
        writeK2iCompactDescriptors();
    }

    if(s_k2iCompactPipelineLayout == VK_NULL_HANDLE &&
       (!g_k2iCompactPipelineFailed || !g_k2locCompactPipelineFailed ||
        !g_k1GlassCompactPipelineFailed || !g_sharcCompactPipelineFailed ||
        !g_k2MegaCompactPipelineFailed || !s_k2iComposePipelineFailed))
    {
        try
        {
            VkPipelineLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            layoutInfo.setLayoutCount = 1u;
            layoutInfo.pSetLayouts = &g_rayTracingDescriptorSetLayout;
            check(
                vkCreatePipelineLayout(
                    g_device, &layoutInfo, nullptr, &s_k2iCompactPipelineLayout),
                "K2 compact/compose compute pipeline layout failed");
        }
        catch(const std::exception& error)
        {
            s_k2iCompactPipelineLayout = VK_NULL_HANDLE;
            g_k2iCompactPipelineFailed = true;
            g_k2locCompactPipelineFailed = true;
            g_k1GlassCompactPipelineFailed = true;
            g_sharcCompactPipelineFailed = true;
            g_k2MegaCompactPipelineFailed = true;
            s_k2iComposePipelineFailed = true;
            RTLog(std::string("[K2-I COMPACT] compute layout 创建失败: ") + error.what());
        }
    }

    if(s_k2iCompactPipelineLayout != VK_NULL_HANDLE)
    {
        auto ensureCompactComputePipeline =
            [](const char* spvPath, const char* tag, VkPipeline& pipe, bool& failed)
        {
            if(pipe != VK_NULL_HANDLE || failed)
            {
                return;
            }
            try
            {
                const std::vector<uint8_t> code = loadSpv(spvPath);
                VkShaderModule shader = createShaderModule(g_device, code);

                VkPipelineShaderStageCreateInfo stage{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                stage.module = shader;
                stage.pName = "main";

                VkComputePipelineCreateInfo pipeInfo{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                pipeInfo.stage = stage;
                pipeInfo.layout = s_k2iCompactPipelineLayout;

                const VkResult result = vkCreateComputePipelines(
                    g_device, getSharedPipelineCache(), 1u, &pipeInfo, nullptr, &pipe);
                vkDestroyShaderModule(g_device, shader, nullptr);
                check(result, "K2 compact/compose compute pipeline failed");
                RTLog(std::string(tag) + " 就绪 (" + spvPath + ")");
            }
            catch(const std::exception& error)
            {
                pipe = VK_NULL_HANDLE;
                failed = true;
                RTLog(std::string(tag) + " 创建失败，回退常规调度: " + error.what());
            }
        };

        ensureCompactComputePipeline(
            "shaders/vpt_sparse_compact.comp.spv",
            "[K2-I COMPACT] compute 管线",
            g_k2iCompactPipeline,
            g_k2iCompactPipelineFailed);
        ensureCompactComputePipeline(
            "shaders/vpt_k2loc_compact.comp.spv",
            "[K2-LOC COMPACT] compute 管线",
            g_k2locCompactPipeline,
            g_k2locCompactPipelineFailed);
        ensureCompactComputePipeline(
            "shaders/vpt_k1_glass_compact.comp.spv",
            "[K1-GLASS COMPACT] compute 管线",
            g_k1GlassCompactPipeline,
            g_k1GlassCompactPipelineFailed);
        ensureCompactComputePipeline(
            "shaders/vpt_sharc_compact.comp.spv",
            "[SHARC COMPACT] compute 管线",
            g_sharcCompactPipeline,
            g_sharcCompactPipelineFailed);
        ensureCompactComputePipeline(
            "shaders/vpt_k2_mega_compact.comp.spv",
            "[K2-MEGA COMPACT] compute 管线",
            g_k2MegaCompactPipeline,
            g_k2MegaCompactPipelineFailed);
        ensureCompactComputePipeline(
            "shaders/rt_test_k2i_compose.comp.spv",
            "[K2-I COMPACT] compose compute 管线",
            g_k2iComposePipeline,
            s_k2iComposePipelineFailed);
    }
}

void destroyK1K2Pipeline()
{
    if(g_k2iComposeSbtBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_k2iComposeSbtBuffer, nullptr);
        g_k2iComposeSbtBuffer = VK_NULL_HANDLE;
    }
    if(g_k2iComposeSbtMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_k2iComposeSbtMemory, nullptr);
        g_k2iComposeSbtMemory = VK_NULL_HANDLE;
    }
    if(g_k2iComposePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2iComposePipeline, nullptr);
        g_k2iComposePipeline = VK_NULL_HANDLE;
    }
    g_k2iComposeRaygenRegion = {};
    s_k2iComposePipelineFailed = false;
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
    if(g_k1GlassPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k1GlassPipeline, nullptr);
        g_k1GlassPipeline = VK_NULL_HANDLE;
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
    if(g_k2sunPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2sunPipeline, nullptr);
        g_k2sunPipeline = VK_NULL_HANDLE;
    }
    // ⑤ 四腿拆分：SUNA/SUNB 与三腿 SUN 互斥存活，销毁时一并兜底。
    if(g_k2sunAPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2sunAPipeline, nullptr);
        g_k2sunAPipeline = VK_NULL_HANDLE;
    }
    if(g_k2sunBPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2sunBPipeline, nullptr);
        g_k2sunBPipeline = VK_NULL_HANDLE;
    }
    if(g_k2locPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2locPipeline, nullptr);
        g_k2locPipeline = VK_NULL_HANDLE;
    }
    if(g_k2locNeePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2locNeePipeline, nullptr);
        g_k2locNeePipeline = VK_NULL_HANDLE;
    }
    if(g_k2locGlossyPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_k2locGlossyPipeline, nullptr);
        g_k2locGlossyPipeline = VK_NULL_HANDLE;
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

    // 2026-11-22 (Batch 2): RGBA32F -> RGBA16F halves binding 89 bandwidth (6 full-screen ops/frame).
    // Rollback: restore VK_FORMAT_R32G32B32A32_SFLOAT + rgba32f in rt_test.rgen:320.
    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
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
    // ④ K2-D 再拆分：三腿 SUN/LOC/I（ORION_K2_SPLIT2=0 回退 ③ 两腿 D/I）
    const bool k2Three = k2Split && k2ThreeSplitEnabled();
    // ⑤ K2-SUN 再拆分：四腿 SUNA/SUNB/LOC/I（ORION_K2_SPLIT3=0 回退 ④ 三腿）
    const bool k2Four = k2Three && k2FourSplitEnabled();
    const bool k2TransportReady =
        k2Four
            ? (g_k2sunAPipeline != VK_NULL_HANDLE &&
               g_k2sunBPipeline != VK_NULL_HANDLE &&
               g_k2locNeePipeline != VK_NULL_HANDLE &&
               g_k2locGlossyPipeline != VK_NULL_HANDLE &&
               g_k2iPipeline != VK_NULL_HANDLE)
            : k2Three
                ? (g_k2sunPipeline != VK_NULL_HANDLE &&
                   g_k2locPipeline != VK_NULL_HANDLE &&
                   g_k2iPipeline != VK_NULL_HANDLE)
                : k2Split
                    ? (g_k2dPipeline != VK_NULL_HANDLE && g_k2iPipeline != VK_NULL_HANDLE)
                    : (g_k2Pipeline != VK_NULL_HANDLE);
    if((g_k1Pipeline != VK_NULL_HANDLE && g_k1GlassPipeline != VK_NULL_HANDLE && k2TransportReady) ||
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
        // SER 选择条件与 Device.cpp 主管线同源（serShaderVariantWanted()：env + 特性，
        // 2026-11-07 起不再额外要求硬件重排提示，见 Settings.cpp 的说明）
        // 二分开关（2026-11-05 设备丢失排查）：ORION_K1K2_NO_SER=1 强制 plain
        const bool k1k2NoSer =
            [](){
                const char* raw = std::getenv("ORION_K1K2_NO_SER");
                return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
            }();
        const bool serShaderRequested =
            !k1k2NoSer && serShaderVariantWanted();
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
        // 2026-11-27 (Batch AI): Per-leg SER assignment:
        //   Coherent primary / visibility-only legs (k1, k2sunB, k2loc_nee) -> plain
        //   Divergent shading legs (k1_glass, k2sunA, k2loc_glossy, k2i) -> fused-only SER
        auto k1Code = loadSpv("shaders/rt_test_k1.rgen.spv");
        auto k1GlassCode = loadRgen("shaders/rt_test_k1_glass_ser.rgen.spv",
                                    "shaders/rt_test_k1_glass.rgen.spv");
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
        const bool k2SerProbe =
            [](){
                const char* raw = std::getenv("ORION_K2_SER_PROBE");
                return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
            }();
        auto k2dCode = (k2Split && !k2Three)
            ? loadRgen("shaders/rt_test_k2d_ser.rgen.spv",
                       "shaders/rt_test_k2d.rgen.spv")
            : std::vector<uint8_t>{};
        auto k2iCode = k2Split
            ? loadRgen("shaders/rt_test_k2i_ser.rgen.spv",
                       "shaders/rt_test_k2i.rgen.spv")
            : std::vector<uint8_t>{};
        auto k2sunCode = k2Three && !k2Four
            ? loadRgen("shaders/rt_test_k2sun_ser.rgen.spv",
                       "shaders/rt_test_k2sun.rgen.spv")
            : std::vector<uint8_t>{};
        auto k2sunACode = k2Four
            ? loadRgen("shaders/rt_test_k2sunA_ser.rgen.spv",
                       "shaders/rt_test_k2sunA.rgen.spv")
            : std::vector<uint8_t>{};
        auto k2sunBCode = k2Four
            ? loadSpv("shaders/rt_test_k2sunB.rgen.spv")
            : std::vector<uint8_t>{};
        auto k2locCode = (k2Three && !k2Four)
            ? loadRgen("shaders/rt_test_k2loc_ser.rgen.spv",
                       "shaders/rt_test_k2loc.rgen.spv")
            : std::vector<uint8_t>{};
        auto k2locNeeCode = k2Four
            ? loadSpv("shaders/rt_test_k2loc_nee.rgen.spv")
            : std::vector<uint8_t>{};
        auto k2locGlossyCode = k2Four
            ? loadRgen("shaders/rt_test_k2loc_glossy_ser.rgen.spv",
                       "shaders/rt_test_k2loc_glossy.rgen.spv")
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
        // B①：K1 的主命中组用极简 primary rchit（仅落身份+MV）；K1 次级射线用 k1Chit；K2 与玻璃命中组用全功能版。
        auto chit = loadSpv("shaders/rt_test.rchit.spv");
        auto k1Chit = loadSpv("shaders/rt_test_k1.rchit.spv");
        auto k1PrimaryChit = loadSpv("shaders/rt_test_k1_primary.rchit.spv");
        auto miss = loadSpv("shaders/rt_test.rmiss.spv");
        auto nocloudMiss = loadSpv("shaders/rt_test_nocloud.rmiss.spv");
        auto ahit = loadSpv("shaders/rt_test.rahit.spv");
        auto translucentChit = loadSpv("shaders/rt_test_translucent.rchit.spv");
        auto translucentAhit = loadSpv("shaders/rt_test_translucent.rahit.spv");
        // D1：材质 callable（仅非拆分的单发 K2 挂；所有 k2Split 档都走内联
        // ORION_K2_MATERIAL_INLINE，绝不调 executeCallableEXT，摘掉 145 KB 死模块与 SBT 槽位）
        const bool includeMaterialCallable = !k2Split;
        auto materialCode = includeMaterialCallable
            ? loadSpv("shaders/rt_test_material.spv")
            : std::vector<uint8_t>{};

        VkShaderModule mods[18]{};
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
        makeModule(k1GlassCode, mods[14]);
        if(k2Four)
        {
            // Batch U：SUNA / SUNB / LOC_NEE / LOC_GLOSSY / K2I
            makeModule(k2sunACode, mods[9]);
            makeModule(k2sunBCode, mods[10]);
            makeModule(k2locNeeCode, mods[15]);
            makeModule(k2locGlossyCode, mods[16]);
            makeModule(k2iCode, mods[12]);
        }
        else if(k2Three)
        {
            makeModule(k2sunCode, mods[9]);
            makeModule(k2locCode, mods[10]);
            makeModule(k2iCode, mods[11]);
        }
        else if(k2Split)
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
        VkShaderModule k1PrimaryChitMod = VK_NULL_HANDLE;
        makeModule(k1PrimaryChit, k1PrimaryChitMod);
        makeModule(miss, mods[4]);
        makeModule(ahit, mods[5]);
        makeModule(translucentChit, mods[6]);
        makeModule(translucentAhit, mods[7]);
        if(includeMaterialCallable)
        {
            makeModule(materialCode, mods[8]);
        }
        makeModule(nocloudMiss, mods[13]);

        // 每条管线的 stage 0 换成自己的 rgen；stage 1（主命中组 chit）K1 用精简版；
        // stage 6 = 材质 callable（仅单发 K2 启用，k2Split 全部走内联材质）
        const uint32_t stageCount = includeMaterialCallable ? 7u : 6u;
        const uint32_t groupCount = includeMaterialCallable ? 6u : 5u;
        VkPipelineShaderStageCreateInfo stages1[7]{};
        VkPipelineShaderStageCreateInfo stages2[7]{};
        const VkShaderStageFlagBits stageKinds[7] = {
            VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
            VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            VK_SHADER_STAGE_CALLABLE_BIT_KHR };
        const uint32_t stageModules[7] = { 0u, 2u, 4u, 5u, 6u, 7u, 8u };
        for(uint32_t i = 0u; i < stageCount; ++i)
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
        if(includeMaterialCallable)
        {
            // D1：材质 callable 组（general = stage 6）
            groups[5] = groups[0];
            groups[5].generalShader = 6u;
        }

        VkRayTracingPipelineCreateInfoKHR pci{
            VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pci.stageCount = stageCount;
        pci.pStages = stages1;
        pci.groupCount = groupCount;
        pci.pGroups = groups;
        pci.maxPipelineRayRecursionDepth = 1u;
        pci.layout = g_rayTracingPipelineLayout;
        if(g_ommNativeEnabled)
        {
            pci.flags |= VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
        }

        if(OrionPipelineStatsCaptureEnabled())
        {
            pci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        }

        VkPipelineShaderStageCreateInfo k1Stages[8]{};
        for(uint32_t i = 0u; i < stageCount; ++i)
        {
            k1Stages[i] = stages1[i];
        }
        const uint32_t k1PrimaryStageIdx = stageCount;
        k1Stages[k1PrimaryStageIdx].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        k1Stages[k1PrimaryStageIdx].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        k1Stages[k1PrimaryStageIdx].module = k1PrimaryChitMod;
        k1Stages[k1PrimaryStageIdx].pName = entry;
        const uint32_t k1StageCount = stageCount + 1u;

        VkRayTracingShaderGroupCreateInfoKHR k1Groups[7]{};
        for(uint32_t i = 0u; i < groupCount; ++i)
        {
            k1Groups[i] = groups[i];
        }
        const uint32_t k1PrimaryGroupIdx = groupCount;
        k1Groups[k1PrimaryGroupIdx] = groups[1];
        k1Groups[k1PrimaryGroupIdx].closestHitShader = k1PrimaryStageIdx;
        const uint32_t k1GroupCount = groupCount + 1u;

        VkRayTracingPipelineCreateInfoKHR k1Pci = pci;
        k1Pci.stageCount = k1StageCount;
        k1Pci.pStages = k1Stages;
        k1Pci.groupCount = k1GroupCount;
        k1Pci.pGroups = k1Groups;

        RTLogFlushed("[K1/K2] leg 1/6 k1-primary compiling...");
        check(OrionCreateRayTracingPipelineTimed(
                  "k1-primary", 1u, &k1Pci, getSharedPipelineCache(),
                  &g_k1Pipeline),
              "Failed to create K1 RT pipeline");
        OrionLogPipelineStats("k1-primary", g_k1Pipeline);

        // Batch U: K1-Glass 独立稀疏腿（保留 k1Chit + translucentChit + 含云 miss）
        stages1[0].module = mods[14];
        {
            RTLogFlushed("[K1/K2] leg k1-glass compiling...");
            VkRayTracingPipelineCreateInfoKHR glassPci = pci;
            glassPci.pStages = stages1;
            VkResult rGlass = OrionCreateRayTracingPipelineTimed(
                "k1-glass", 1u, &glassPci, getSharedPipelineCache(),
                &g_k1GlassPipeline);
            if(g_k1GlassPipeline == VK_NULL_HANDLE && serShaderRequested)
            {
                vkDestroyShaderModule(g_device, mods[14], nullptr);
                makeModule(loadSpv("shaders/rt_test_k1_glass.rgen.spv"), mods[14]);
                stages1[0].module = mods[14];
                RTLog("[K1/K2] k1-glass SER 档创建失败，降级 plain 重建");
                rGlass = OrionCreateRayTracingPipelineTimed(
                    "k1-glass-plain", 1u, &glassPci, getSharedPipelineCache(),
                    &g_k1GlassPipeline);
            }
            if(g_k1GlassPipeline == VK_NULL_HANDLE)
            {
                check(rGlass, "Failed to create k1-glass RT pipeline");
            }
            OrionLogPipelineStats("k1-glass", g_k1GlassPipeline);
        }

        if(k2Split)
        {
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
            if(k2Three)
            {
                struct K2LegDef
                {
                    const char* tag;
                    const char* plainSpv;
                    VkShaderModule* mod;
                    VkPipeline* out;
                    bool useNocloudMiss;
                };
                const K2LegDef k2LegsFour[] = {
                    {"k2sunA-direct", "shaders/rt_test_k2sunA.rgen.spv",
                     &mods[9], &g_k2sunAPipeline, false},
                    {"k2sunB-direct", "shaders/rt_test_k2sunB.rgen.spv",
                     &mods[10], &g_k2sunBPipeline, true},
                    {"k2loc-nee", "shaders/rt_test_k2loc_nee.rgen.spv",
                     &mods[15], &g_k2locNeePipeline, true},
                    {"k2loc-glossy", "shaders/rt_test_k2loc_glossy.rgen.spv",
                     &mods[16], &g_k2locGlossyPipeline, false},
                    {"k2i-indirect", "shaders/rt_test_k2i.rgen.spv",
                     &mods[12], &g_k2iPipeline, true},
                };
                const K2LegDef k2LegsThree[] = {
                    {"k2sun-direct", "shaders/rt_test_k2sun.rgen.spv",
                     &mods[9], &g_k2sunPipeline, false},
                    {"k2loc-local", "shaders/rt_test_k2loc.rgen.spv",
                     &mods[10], &g_k2locPipeline, false},
                    {"k2i-indirect", "shaders/rt_test_k2i.rgen.spv",
                     &mods[11], &g_k2iPipeline, true},
                };
                const K2LegDef* k2Legs =
                    k2Four ? k2LegsFour : k2LegsThree;
                const uint32_t k2LegCountCreate =
                    k2Four ? 5u : 3u;
                for(uint32_t legIdx = 0u; legIdx < k2LegCountCreate; ++legIdx)
                {
                    const K2LegDef& leg = k2Legs[legIdx];
                    RTLogFlushed(
                        std::string("[K1/K2] leg ") +
                        std::to_string(legIdx + 2u) + "/" +
                        (k2Four ? "6" : "4") + " " + leg.tag +
                        " compiling...");
                    stages2[0].module = *leg.mod;
                    stages2[2].module = leg.useNocloudMiss ? mods[13] : mods[4];
                    VkResult r = tryCreateSplit(
                        leg.tag, getSharedPipelineCache(), leg.out);
                    if(*leg.out == VK_NULL_HANDLE && serShaderRequested)
                    {
                        vkDestroyShaderModule(g_device, *leg.mod, nullptr);
                        makeModule(loadSpv(leg.plainSpv), *leg.mod);
                        stages2[0].module = *leg.mod;
                        RTLog(std::string("[K1/K2] ") + leg.tag +
                              " SER 档创建失败，降级 plain 重建");
                        r = tryCreateSplit(
                            (std::string(leg.tag) + "-plain").c_str(),
                            getSharedPipelineCache(), leg.out);
                    }
                    if(*leg.out == VK_NULL_HANDLE)
                    {
                        check(r, (std::string("Failed to create ") +
                                  leg.tag + " RT pipeline").c_str());
                    }
                }
            }
            else
            {
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
            stages2[2].module = mods[13];
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
            } // ④ 两腿创建分支收口（三腿分支见上方 k2Three）
        }
        else
        {
            pci.pStages = stages2;
            check(OrionCreateRayTracingPipelineTimed(
                      "k2-transport", 1u, &pci, getSharedPipelineCache(),
                      &g_k2Pipeline),
                  "Failed to create K2 RT pipeline");
        }

        if(k2Four)
        {
            OrionLogPipelineStats("k2sunA-direct", g_k2sunAPipeline);
            OrionLogPipelineStats("k2sunB-direct", g_k2sunBPipeline);
            OrionLogPipelineStats("k2loc-nee", g_k2locNeePipeline);
            OrionLogPipelineStats("k2loc-glossy", g_k2locGlossyPipeline);
            OrionLogPipelineStats("k2i-indirect", g_k2iPipeline);
        }
        else if(k2Three)
        {
            OrionLogPipelineStats("k2sun-direct", g_k2sunPipeline);
            OrionLogPipelineStats("k2loc-local", g_k2locPipeline);
            OrionLogPipelineStats("k2i-indirect", g_k2iPipeline);
        }
        else if(k2Split)
        {
            OrionLogPipelineStats("k2d-direct", g_k2dPipeline);
            OrionLogPipelineStats("k2i-indirect", g_k2iPipeline);
        }
        else
        {
            OrionLogPipelineStats("k2-transport", g_k2Pipeline);
        }

        for(uint32_t i = 0u; i < 18u; ++i)
        {
            vkDestroyShaderModule(g_device, mods[i], nullptr);
        }
        vkDestroyShaderModule(g_device, k1PrimaryChitMod, nullptr);

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
        // ④ SBT 按腿泛化：K1 之后每条 K2 腿各占一套 raygen/miss/hit/callable
        // 记录（句柄按管线区分，不能互借）。腿表：⑤ 四腿=SUNA/SUNB/LOC/I，
        // 三腿=SUN/LOC/I，两腿=D/I（③ 回退档），单发=K2。
        constexpr uint32_t kMaxExtraLegs = 6u;
        VkPipeline k2LegPipelines[kMaxExtraLegs]{};
        VkStridedDeviceAddressRegionKHR* k2LegRaygen[kMaxExtraLegs]{};
        VkStridedDeviceAddressRegionKHR* k2LegMiss[kMaxExtraLegs]{};
        VkStridedDeviceAddressRegionKHR* k2LegHit[kMaxExtraLegs]{};
        VkStridedDeviceAddressRegionKHR* k2LegCallable[kMaxExtraLegs]{};
        uint32_t k2LegCount = 0u;
        auto pushExtraLeg = [&](VkPipeline pipe,
                                VkStridedDeviceAddressRegionKHR* rg,
                                VkStridedDeviceAddressRegionKHR* ms,
                                VkStridedDeviceAddressRegionKHR* ht,
                                VkStridedDeviceAddressRegionKHR* cb)
        {
            k2LegPipelines[k2LegCount] = pipe;
            k2LegRaygen[k2LegCount] = rg;
            k2LegMiss[k2LegCount] = ms;
            k2LegHit[k2LegCount] = ht;
            k2LegCallable[k2LegCount] = cb;
            ++k2LegCount;
        };
        pushExtraLeg(g_k1GlassPipeline, &g_k1GlassRaygenRegion, &g_k1GlassMissRegion,
                     &g_k1GlassHitRegion, &g_k1GlassCallableRegion);
        if(k2Four)
        {
            pushExtraLeg(g_k2sunAPipeline, &g_k2sunARaygenRegion, &g_k2sunAMissRegion,
                         &g_k2sunAHitRegion, &g_k2sunACallableRegion);
            pushExtraLeg(g_k2sunBPipeline, &g_k2sunBRaygenRegion, &g_k2sunBMissRegion,
                         &g_k2sunBHitRegion, &g_k2sunBCallableRegion);
            pushExtraLeg(g_k2locNeePipeline, &g_k2locNeeRaygenRegion, &g_k2locNeeMissRegion,
                         &g_k2locNeeHitRegion, &g_k2locNeeCallableRegion);
            pushExtraLeg(g_k2locGlossyPipeline, &g_k2locGlossyRaygenRegion, &g_k2locGlossyMissRegion,
                         &g_k2locGlossyHitRegion, &g_k2locGlossyCallableRegion);
            pushExtraLeg(g_k2iPipeline, &g_k2iRaygenRegion, &g_k2iMissRegion,
                         &g_k2iHitRegion, &g_k2iCallableRegion);
        }
        else if(k2Three)
        {
            pushExtraLeg(g_k2sunPipeline, &g_k2sunRaygenRegion, &g_k2sunMissRegion,
                         &g_k2sunHitRegion, &g_k2sunCallableRegion);
            pushExtraLeg(g_k2locPipeline, &g_k2locRaygenRegion, &g_k2locMissRegion,
                         &g_k2locHitRegion, &g_k2locCallableRegion);
            pushExtraLeg(g_k2iPipeline, &g_k2iRaygenRegion, &g_k2iMissRegion,
                         &g_k2iHitRegion, &g_k2iCallableRegion);
        }
        else if(k2Split)
        {
            pushExtraLeg(g_k2dPipeline, &g_k2dRaygenRegion, &g_k2dMissRegion,
                         &g_k2dHitRegion, &g_k2dCallableRegion);
            pushExtraLeg(g_k2iPipeline, &g_k2iRaygenRegion, &g_k2iMissRegion,
                         &g_k2iHitRegion, &g_k2iCallableRegion);
        }
        else
        {
            pushExtraLeg(g_k2Pipeline, &g_k2RaygenRegion, &g_k2MissRegion,
                         &g_k2HitRegion, &g_k2CallableRegion);
        }

        const auto alignTo = [ba](uint32_t v)
        {
            return ((v + ba - 1u) / ba) * ba;
        };
        const uint32_t raygen1Offset = 0u;
        uint32_t sbtCursor = alignTo(raygen1Offset + handleAligned);
        uint32_t legRaygenOffset[kMaxExtraLegs]{};
        uint32_t legMissOffset[kMaxExtraLegs]{};
        uint32_t legHitOffset[kMaxExtraLegs]{};
        uint32_t legCallableOffset[kMaxExtraLegs]{};
        for(uint32_t leg = 0u; leg < k2LegCount; ++leg)
        {
            legRaygenOffset[leg] = sbtCursor;
            sbtCursor = alignTo(sbtCursor + handleAligned);
        }
        const uint32_t missOffset = sbtCursor;
        sbtCursor = alignTo(sbtCursor + handleAligned);
        for(uint32_t leg = 0u; leg < k2LegCount; ++leg)
        {
            legMissOffset[leg] = sbtCursor;
            sbtCursor = alignTo(sbtCursor + handleAligned);
        }
        const uint32_t k1HitRecordCount = 2u * SBT_HIT_RECORD_COUNT;
        const uint32_t hitOffset = sbtCursor;
        sbtCursor =
            alignTo(sbtCursor + k1HitRecordCount * handleAligned);
        for(uint32_t leg = 0u; leg < k2LegCount; ++leg)
        {
            legHitOffset[leg] = sbtCursor;
            sbtCursor =
                alignTo(sbtCursor + SBT_HIT_RECORD_COUNT * handleAligned);
        }
        uint32_t callableOffset = 0u;
        if(includeMaterialCallable)
        {
            callableOffset = sbtCursor;
            sbtCursor = alignTo(sbtCursor + handleAligned);
            for(uint32_t leg = 0u; leg < k2LegCount; ++leg)
            {
                legCallableOffset[leg] = sbtCursor;
                sbtCursor = alignTo(sbtCursor + handleAligned);
            }
        }
        const uint32_t sbtSize = sbtCursor;

        std::vector<uint8_t> handles1((size_t)k1GroupCount * hs, 0);
        std::vector<uint8_t> legHandles[kMaxExtraLegs]{
            std::vector<uint8_t>((size_t)groupCount * hs, 0),
            std::vector<uint8_t>((size_t)groupCount * hs, 0),
            std::vector<uint8_t>((size_t)groupCount * hs, 0),
            std::vector<uint8_t>((size_t)groupCount * hs, 0),
            std::vector<uint8_t>((size_t)groupCount * hs, 0),
            std::vector<uint8_t>((size_t)groupCount * hs, 0)};
        check(getHandles(g_device, g_k1Pipeline, 0u,
                         k1GroupCount, handles1.size(), handles1.data()),
              "Failed to get K1 RT shader handles");
        for(uint32_t leg = 0u; leg < k2LegCount; ++leg)
        {
            check(getHandles(g_device, k2LegPipelines[leg], 0u,
                             groupCount, legHandles[leg].size(),
                             legHandles[leg].data()),
                  "Failed to get K2-leg RT shader handles");
        }

        std::vector<uint8_t> data((size_t)sbtSize, 0);
        std::memcpy(data.data() + raygen1Offset, handles1.data(), hs);
        std::memcpy(data.data() + missOffset,
                    handles1.data() + 2u * hs, hs);
        // hit 记录组与 giHalf 相同：opaque, opaque, alpha, water, opaque
        const uint32_t recordGroups[SBT_HIT_RECORD_COUNT] = {
            1u, 1u, 3u, 4u, 1u };
        const uint32_t k1PrimaryRecordGroups[SBT_HIT_RECORD_COUNT] = {
            k1PrimaryGroupIdx, k1PrimaryGroupIdx, 3u,
            k1PrimaryGroupIdx, k1PrimaryGroupIdx };
        for(uint32_t record = 0u; record < SBT_HIT_RECORD_COUNT; ++record)
        {
            std::memcpy(data.data() + hitOffset + record * handleAligned,
                        handles1.data() + recordGroups[record] * hs, hs);
            std::memcpy(
                data.data() + hitOffset +
                    (SBT_HIT_RECORD_COUNT + record) * handleAligned,
                handles1.data() + k1PrimaryRecordGroups[record] * hs, hs);
        }
        if(includeMaterialCallable)
        {
            std::memcpy(data.data() + callableOffset,
                        legHandles[1].data() + 5u * hs, hs);
        }
        for(uint32_t leg = 0u; leg < k2LegCount; ++leg)
        {
            std::memcpy(data.data() + legRaygenOffset[leg],
                        legHandles[leg].data(), hs);
            std::memcpy(data.data() + legMissOffset[leg],
                        legHandles[leg].data() + 2u * hs, hs);
            for(uint32_t record = 0u; record < SBT_HIT_RECORD_COUNT; ++record)
            {
                std::memcpy(
                    data.data() + legHitOffset[leg] + record * handleAligned,
                    legHandles[leg].data() + recordGroups[record] * hs, hs);
            }
            if(includeMaterialCallable)
            {
                std::memcpy(data.data() + legCallableOffset[leg],
                            legHandles[leg].data() + 5u * hs, hs);
            }
        }

        uploadDeviceLocalBufferOnce(
            data.data(),
            sbtSize,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            g_k1k2SbtBuffer,
            g_k1k2SbtMemory);

        const uint64_t sbtAddress =
            getBufferDeviceAddress(g_device, g_k1k2SbtBuffer);

        g_k1RaygenRegion.deviceAddress = sbtAddress + raygen1Offset;
        g_k1RaygenRegion.stride = handleAligned;
        g_k1RaygenRegion.size = handleAligned;
        g_k1MissRegion.deviceAddress = sbtAddress + missOffset;
        g_k1MissRegion.stride = handleAligned;
        g_k1MissRegion.size = handleAligned;
        g_k1HitRegion.deviceAddress = sbtAddress + hitOffset;
        g_k1HitRegion.stride = handleAligned;
        g_k1HitRegion.size = k1HitRecordCount * handleAligned;
        if(includeMaterialCallable)
        {
            g_k1CallableRegion.deviceAddress = sbtAddress + callableOffset;
            g_k1CallableRegion.stride = handleAligned;
            g_k1CallableRegion.size = handleAligned;
        }
        else
        {
            g_k1CallableRegion = {};
        }
        for(uint32_t leg = 0u; leg < k2LegCount; ++leg)
        {
            k2LegRaygen[leg]->deviceAddress = sbtAddress + legRaygenOffset[leg];
            k2LegRaygen[leg]->stride = handleAligned;
            k2LegRaygen[leg]->size = handleAligned;
            k2LegMiss[leg]->deviceAddress = sbtAddress + legMissOffset[leg];
            k2LegMiss[leg]->stride = handleAligned;
            k2LegMiss[leg]->size = handleAligned;
            k2LegHit[leg]->deviceAddress = sbtAddress + legHitOffset[leg];
            k2LegHit[leg]->stride = handleAligned;
            k2LegHit[leg]->size = SBT_HIT_RECORD_COUNT * handleAligned;
            if(includeMaterialCallable)
            {
                k2LegCallable[leg]->deviceAddress =
                    sbtAddress + legCallableOffset[leg];
                k2LegCallable[leg]->stride = handleAligned;
                k2LegCallable[leg]->size = handleAligned;
            }
            else
            {
                *k2LegCallable[leg] = {};
            }
        }
        if(!k2Split)
        {
            g_k2CallableRegion = g_k1CallableRegion;
        }

        {
            std::ostringstream dbg;
            dbg << "[K1/K2] SBT addr=0x" << std::hex << sbtAddress
                << " hs=" << std::dec << hs << " ha=" << ha << " ba=" << ba
                << " rg1=" << raygen1Offset << " miss=" << missOffset
                << " hit1=" << hitOffset << " call=" << callableOffset
                << " legs=" << k2LegCount << " size=" << sbtSize;
            RTLog(dbg.str());
        }

        std::cerr << "[K1/K2] 管线已创建（k1=主视线落盘, k2=读盘续算传输）"
                  << std::endl;

        // 2026-11-06：同 GI HALF，改为交给 maybePersist 去抖。
        g_pipelineCachePersistCountdown = 60u;
    }
    catch(const std::exception& error)
    {
        // 2026-11-22 批A：失败必须同时进 RTLog。原来只写 std::cerr（不进
        // rt_debug.log），2026-11-21 夜三连败时 rt_debug.log 里"查无死因"
        // 的根因之一——排查时只看 rt_debug.log 会误判成"编译静默消失"。
        std::cerr << "[K1/K2] 管线创建失败（本会话不再重试；主管线已退役，整局无 RT）："
                  << error.what() << std::endl;
        RTLogFlushed(
            std::string("[K1/K2] 管线创建失败（本会话不再重试；整局无 RT）：") +
            error.what());
        destroyK1K2Pipeline();
        g_k1K2PipelineFailed = true;
        g_k1Pipeline = VK_NULL_HANDLE;
        g_k2Pipeline = VK_NULL_HANDLE;
    }
}
namespace
{
void recordCompactIndirectTrace(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height,
    VkPipeline compactPipeline,
    VkPipeline rtPipeline,
    const VkStridedDeviceAddressRegionKHR* raygenRegion,
    const VkStridedDeviceAddressRegionKHR* missRegion,
    const VkStridedDeviceAddressRegionKHR* hitRegion,
    const VkStridedDeviceAddressRegionKHR* callableRegion,
    PFN_vkCmdTraceRaysIndirectKHR traceIndirect,
    uint32_t compactMode,
    uint32_t dispatchX,
    uint32_t dispatchY,
    uint32_t slotIndex)
{
    VkMemoryBarrier preUpdateBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    preUpdateBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preUpdateBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 1u, &preUpdateBarrier, 0u, nullptr, 0u, nullptr);

    if(slotIndex == 2u || slotIndex == 3u)
    {
        const uint32_t cap = g_k2iCompactCapacityPixels;
        const CompactSlotHeaderHost pairHdr[2] = {
            { 0u, 1u, 1u, (slotIndex == 2u) ? compactMode : 0u, 0u, 2u * cap, 0u, 0u },
            { 0u, 1u, 1u, (slotIndex == 3u) ? compactMode : 0u, 0u, 3u * cap, 0u, 0u },
        };
        vkCmdUpdateBuffer(
            cmd, g_k2iCompactBuffer,
            2ull * sizeof(CompactSlotHeaderHost), sizeof(pairHdr), pairHdr);
    }
    else
    {
        const CompactSlotHeaderHost hdr{
            0u, 1u, 1u, compactMode, 0u,
            slotIndex * g_k2iCompactCapacityPixels, 0u, 0u
        };
        vkCmdUpdateBuffer(
            cmd, g_k2iCompactBuffer,
            static_cast<VkDeviceSize>(slotIndex) * sizeof(CompactSlotHeaderHost),
            sizeof(hdr), &hdr);
    }

    VkMemoryBarrier xferToComp{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    xferToComp.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    xferToComp.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 1u, &xferToComp, 0u, nullptr, 0u, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compactPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_k2iCompactPipelineLayout,
        0u, 1u, &g_rayTracingDescriptorSet, 0u, nullptr);
    const uint32_t gx = dispatchX > 0u ? dispatchX : (width + 15u) / 16u;
    const uint32_t gy = dispatchY > 0u ? dispatchY : (height + 15u) / 16u;
    vkCmdDispatch(cmd, gx, gy, 1u);

    VkMemoryBarrier compToTrace{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    compToTrace.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    compToTrace.dstAccessMask =
        VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
        VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 1u, &compToTrace, 0u, nullptr, 0u, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, rtPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        g_rayTracingPipelineLayout, 0u, 1u, &g_rayTracingDescriptorSet,
        0u, nullptr);
    traceIndirect(
        cmd, raygenRegion, missRegion, hitRegion, callableRegion,
        g_k2iCompactBufferAddress +
            static_cast<VkDeviceSize>(slotIndex) * sizeof(CompactSlotHeaderHost));
}

void recordCompactBufferDisabledFallback(
    VkCommandBuffer cmd,
    uint32_t height,
    uint32_t slotIndex)
{
    if(g_k2iCompactBuffer == VK_NULL_HANDLE)
    {
        return;
    }
    VkMemoryBarrier preUpdate{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    preUpdate.srcAccessMask = 0u;
    preUpdate.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 1u, &preUpdate, 0u, nullptr, 0u, nullptr);

    const CompactSlotHeaderHost hdr{
        0u, height, 1u, 0u, 0u, slotIndex * g_k2iCompactCapacityPixels, 0u, 0u
    };
    vkCmdUpdateBuffer(
        cmd, g_k2iCompactBuffer,
        static_cast<VkDeviceSize>(slotIndex) * sizeof(CompactSlotHeaderHost),
        sizeof(hdr), &hdr);

    VkMemoryBarrier xferToTrace{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    xferToTrace.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    xferToTrace.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 1u, &xferToTrace, 0u, nullptr, 0u, nullptr);
}

void recordK1GlassDispatch(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height,
    uint32_t slots,
    PFN_vkCmdTraceRaysKHR trace)
{
    ensureK2iCompactResources(width, height);
    auto traceIndirect = getCmdTraceRaysIndirect();
    const bool useCompact =
        traceRaysIndirectSupported() &&
        traceIndirect != nullptr &&
        g_k1GlassCompactPipeline != VK_NULL_HANDLE &&
        s_k2iCompactPipelineLayout != VK_NULL_HANDLE &&
        g_k2iCompactBuffer != VK_NULL_HANDLE &&
        g_k2iCompactBufferAddress != 0u &&
        g_k2DirectView != VK_NULL_HANDLE &&
        slots == 1u;

    ScopedVkDebugLabel glassLabel(cmd, "K1-Glass");
    if(useCompact)
    {
        recordCompactIndirectTrace(
            cmd, width, height, g_k1GlassCompactPipeline, g_k1GlassPipeline,
            &g_k1GlassRaygenRegion, &g_k1GlassMissRegion,
            &g_k1GlassHitRegion, &g_k1GlassCallableRegion,
            traceIndirect, 1u, 0u, 0u, 0u);
        return;
    }

    recordCompactBufferDisabledFallback(cmd, height, 0u);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, g_k1GlassPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        g_rayTracingPipelineLayout, 0u, 1u, &g_rayTracingDescriptorSet,
        0u, nullptr);
    trace(
        cmd, &g_k1GlassRaygenRegion, &g_k1GlassMissRegion,
        &g_k1GlassHitRegion, &g_k1GlassCallableRegion, width, height, 1u);
}

void recordK2locDispatch(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height,
    uint32_t slots,
    PFN_vkCmdTraceRaysKHR trace)
{
    ensureK2iCompactResources(width, height);

    static const bool k2locCompactEnabled = []()
    {
        const char* raw = std::getenv("ORION_K2LOC_COMPACT");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();
    static bool s_k2locCompactModeLogged = false;

    const int sparsePct = sparseRatePercent();
    auto traceIndirect = getCmdTraceRaysIndirect();
    const bool zeroSceneLights =
        (g_vptTotalEmitterCount.load(std::memory_order_relaxed) == 0u) &&
        (g_localLightGpuCount == 0u) &&
        (g_dynamicLightUpload.count == 0u);
    const bool useCompact =
        (k2locCompactEnabled || zeroSceneLights) &&
        traceRaysIndirectSupported() &&
        traceIndirect != nullptr &&
        g_k2locCompactPipeline != VK_NULL_HANDLE &&
        s_k2iCompactPipelineLayout != VK_NULL_HANDLE &&
        g_k2iCompactBuffer != VK_NULL_HANDLE &&
        g_k2iCompactBufferAddress != 0u &&
        g_sparseActiveMaskImage != VK_NULL_HANDLE &&
        g_matReuseBuffer != VK_NULL_HANDLE &&
        matReuseLegsEnabled() &&
        (zeroSceneLights || (sparsePct > 0 && sparsePct < 100)) &&
        slots == 1u &&
        g_cameraUboHost.giHalfParams[0] == 0.0f;

    if(!s_k2locCompactModeLogged)
    {
        s_k2locCompactModeLogged = true;
        RTLog(
            std::string("[K2-LOC COMPACT] active=") + (useCompact ? "1" : "0") +
            " zeroLights=" + (zeroSceneLights ? "1" : "0") +
            " indirectSup=" + (traceRaysIndirectSupported() ? "1" : "0") +
            " sparsePct=" + std::to_string(sparsePct) +
            " matReuse=" + (g_matReuseBuffer != VK_NULL_HANDLE ? "1" : "0"));
    }

    if(useCompact)
    {
        recordCompactIndirectTrace(
            cmd, width, height, g_k2locCompactPipeline, g_k2locPipeline,
            &g_k2locRaygenRegion, &g_k2locMissRegion,
            &g_k2locHitRegion, &g_k2locCallableRegion, traceIndirect,
            zeroSceneLights ? 2u : 1u, 0u, 0u, 3u);
        return;
    }

    recordCompactBufferDisabledFallback(cmd, height, 3u);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, g_k2locPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        g_rayTracingPipelineLayout, 0u, 1u, &g_rayTracingDescriptorSet,
        0u, nullptr);
    trace(
        cmd, &g_k2locRaygenRegion, &g_k2locMissRegion,
        &g_k2locHitRegion, &g_k2locCallableRegion, width, height, 1u);
}

void recordK2locSplitDispatch(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height,
    uint32_t slots,
    PFN_vkCmdTraceRaysKHR trace)
{
    ensureK2iCompactResources(width, height);

    const int sparsePct = sparseRatePercent();
    auto traceIndirect = getCmdTraceRaysIndirect();
    const bool zeroSceneLights =
        (g_vptTotalEmitterCount.load(std::memory_order_relaxed) == 0u) &&
        (g_localLightGpuCount == 0u) &&
        (g_dynamicLightUpload.count == 0u);
    const bool compactCommon =
        traceRaysIndirectSupported() &&
        traceIndirect != nullptr &&
        g_k2locCompactPipeline != VK_NULL_HANDLE &&
        s_k2iCompactPipelineLayout != VK_NULL_HANDLE &&
        g_k2iCompactBuffer != VK_NULL_HANDLE &&
        g_k2iCompactBufferAddress != 0u &&
        g_matReuseBuffer != VK_NULL_HANDLE &&
        matReuseLegsEnabled() &&
        slots == 1u &&
        g_cameraUboHost.giHalfParams[0] == 0.0f;

    if(!zeroSceneLights)
    {
        ScopedVkDebugLabel neeLabel(cmd, "K2-Loc-NEE");
        const bool useNeeCompact =
            compactCommon &&
            g_sparseActiveMaskImage != VK_NULL_HANDLE &&
            sparsePct > 0 && sparsePct < 100 &&
            g_cameraUboHost.sparseParams[0] < 0.5f;
        if(useNeeCompact)
        {
            recordCompactIndirectTrace(
                cmd, width, height, g_k2locCompactPipeline, g_k2locNeePipeline,
                &g_k2locNeeRaygenRegion, &g_k2locNeeMissRegion,
                &g_k2locNeeHitRegion, &g_k2locNeeCallableRegion,
                traceIndirect, 3u, 0u, 0u, 2u);
        }
        else
        {
            recordCompactBufferDisabledFallback(cmd, height, 2u);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, g_k2locNeePipeline);
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                g_rayTracingPipelineLayout, 0u, 1u, &g_rayTracingDescriptorSet,
                0u, nullptr);
            trace(
                cmd, &g_k2locNeeRaygenRegion, &g_k2locNeeMissRegion,
                &g_k2locNeeHitRegion, &g_k2locNeeCallableRegion, width, height, 1u);
        }

        VkMemoryBarrier neeToGlossy{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        neeToGlossy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        neeToGlossy.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 1u, &neeToGlossy, 0u, nullptr, 0u, nullptr);
    }

    {
        ScopedVkDebugLabel glossyLabel(cmd, "K2-Loc-Glossy");
        if(compactCommon)
        {
            recordCompactIndirectTrace(
                cmd, width, height, g_k2locCompactPipeline, g_k2locGlossyPipeline,
                &g_k2locGlossyRaygenRegion, &g_k2locGlossyMissRegion,
                &g_k2locGlossyHitRegion, &g_k2locGlossyCallableRegion,
                traceIndirect, 2u, 0u, 0u, 3u);
        }
        else
        {
            recordCompactBufferDisabledFallback(cmd, height, 3u);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, g_k2locGlossyPipeline);
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                g_rayTracingPipelineLayout, 0u, 1u, &g_rayTracingDescriptorSet,
                0u, nullptr);
            trace(
                cmd, &g_k2locGlossyRaygenRegion, &g_k2locGlossyMissRegion,
                &g_k2locGlossyHitRegion, &g_k2locGlossyCallableRegion, width, height, 1u);
        }
    }
}

void recordK2iComposePass(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if(g_k2iComposePipeline != VK_NULL_HANDLE &&
       s_k2iCompactPipelineLayout != VK_NULL_HANDLE)
    {
        ScopedVkDebugLabel k2iComposeLabel(cmd, "K2-I-Compose");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_k2iComposePipeline);
        vkCmdBindDescriptorSets(
            cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            s_k2iCompactPipelineLayout, 0u, 1u, &g_rayTracingDescriptorSet,
            0u, nullptr);
        vkCmdDispatch(cmd, (width + 15u) / 16u, (height + 15u) / 16u, 1u);

        VkMemoryBarrier composeToDownstream{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        composeToDownstream.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        composeToDownstream.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u, 1u, &composeToDownstream, 0u, nullptr, 0u, nullptr);
    }
}

void recordK2iDispatch(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height,
    uint32_t slots,
    PFN_vkCmdTraceRaysKHR trace,
    bool skipTrace = false)
{
    ensureK2iCompactResources(width, height);

    static const bool k2iCompactEnabled = []()
    {
        const char* raw = std::getenv("ORION_K2I_COMPACT");
        return raw == nullptr || raw[0] != '0';
    }();
    static bool s_k2iCompactModeLogged = false;

    const int sparsePct = sparseRatePercent();
    auto traceIndirect = getCmdTraceRaysIndirect();
    const bool giHalfActive = (g_cameraUboHost.giHalfParams[0] != 0.0f);
    const bool useCompact =
        k2iCompactEnabled &&
        traceRaysIndirectSupported() &&
        traceIndirect != nullptr &&
        g_k2iCompactPipeline != VK_NULL_HANDLE &&
        s_k2iCompactPipelineLayout != VK_NULL_HANDLE &&
        g_k2GiTraceImage != VK_NULL_HANDLE &&
        g_k2iCompactBuffer != VK_NULL_HANDLE &&
        g_k2iCompactBufferAddress != 0u &&
        g_sparseActiveMaskImage != VK_NULL_HANDLE &&
        sparsePct > 0 &&
        slots == 1u;

    if(!s_k2iCompactModeLogged && sparsePct > 0)
    {
        s_k2iCompactModeLogged = true;
        RTLog(
            std::string("[K2-I COMPACT] active=") + (useCompact ? "1" : "0") +
            " indirectSup=" + (traceRaysIndirectSupported() ? "1" : "0") +
            " sparsePct=" + std::to_string(sparsePct) +
            " slots=" + std::to_string(slots));
    }

    if(!skipTrace && !giHalfActive)
    {
        ScopedVkDebugLabel k2iTraceLabel(cmd, "K2-I-Trace");
        if(useCompact)
        {
            recordCompactIndirectTrace(
                cmd, width, height, g_k2iCompactPipeline, g_k2iPipeline,
                &g_k2iRaygenRegion, &g_k2iMissRegion,
                &g_k2iHitRegion, &g_k2iCallableRegion, traceIndirect,
                (sparsePct < 100) ? 1u : 2u, 0u, 0u, 4u);
        }
        else
        {
            recordCompactBufferDisabledFallback(cmd, height, 4u);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, g_k2iPipeline);
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                g_rayTracingPipelineLayout, 0u, 1u, &g_rayTracingDescriptorSet,
                0u, nullptr);
            trace(
                cmd, &g_k2iRaygenRegion, &g_k2iMissRegion,
                &g_k2iHitRegion, &g_k2iCallableRegion, width, height, 1u);
        }

        VkMemoryBarrier traceToCompose{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        traceToCompose.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        traceToCompose.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 1u, &traceToCompose, 0u, nullptr, 0u, nullptr);
    }

    recordK2iComposePass(cmd, width, height);
}
} // namespace

bool traceK1K2(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if(!k1k2SplitEnabled())
    {
        return false;
    }
    if((!g_rtMainPipelineLazy.load(std::memory_order_acquire) &&
        g_rayTracingPipeline == VK_NULL_HANDLE) ||
       g_rayTracingPipelineLayout == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return false;
    }

    ensureK1K2Pipelines();
    {
        const bool k2Split = k2DirectIndirectSplitEnabled();
        const bool k2Three = k2Split && k2ThreeSplitEnabled();
        const bool k2Four = k2Three && k2FourSplitEnabled();
        const bool transportReady =
            k2Four
                ? (g_k2sunAPipeline != VK_NULL_HANDLE &&
                   g_k2sunBPipeline != VK_NULL_HANDLE &&
                   g_k2locNeePipeline != VK_NULL_HANDLE &&
                   g_k2locGlossyPipeline != VK_NULL_HANDLE &&
                   g_k2iPipeline != VK_NULL_HANDLE)
                : k2Three
                    ? (g_k2sunPipeline != VK_NULL_HANDLE &&
                       g_k2locPipeline != VK_NULL_HANDLE &&
                       g_k2iPipeline != VK_NULL_HANDLE)
                    : k2Split
                        ? (g_k2dPipeline != VK_NULL_HANDLE &&
                           g_k2iPipeline != VK_NULL_HANDLE)
                        : (g_k2Pipeline != VK_NULL_HANDLE);
        if(g_k1Pipeline == VK_NULL_HANDLE ||
           g_k1GlassPipeline == VK_NULL_HANDLE ||
           !transportReady)
        {
            return false;
        }
    }

    const float spp = std::clamp(g_perfRtSpp.load(), 0.5f, 4.0f);
    const uint32_t slots = (uint32_t)std::max(1, (int)(spp + 0.5f));
    ensureK1LandingBuffer(width, height, slots);
    if(g_k1LandingBuffer == VK_NULL_HANDLE)
    {
        return false;
    }
    if(s_k1LandingBoundSet != g_rayTracingDescriptorSet)
    {
        writeK1LandingDescriptor();
    }

    ensureMatReuseBuffer(width, height, slots);
    if(g_matReuseBuffer != VK_NULL_HANDLE &&
       s_matReuseBoundSet != g_rayTracingDescriptorSet)
    {
        writeMatReuseDescriptor();
    }

    ensureNeeVisCacheBuffer(width, height);
    if(g_neeVisCacheBuffer != VK_NULL_HANDLE &&
       s_neeVisCacheBoundSet != g_rayTracingDescriptorSet)
    {
        writeNeeVisCacheDescriptor();
    }

    if(k2DirectIndirectSplitEnabled())
    {
        createK2DirectImage(width, height);
    }

    auto trace = getCmdTraceRays();
    if(!trace)
    {
        return false;
    }

    static const bool k1k2K1Only = [](){
        const char* raw = std::getenv("ORION_K1K2_K1_ONLY");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();
    static int s_k1k2TraceLog = 0;
    const bool logTrace = s_k1k2TraceLog < 3;
    if(logTrace) ++s_k1k2TraceLog;
    if(logTrace) RTLog("[K1/K2] pre bind-k1");

    rtCheckpoint(cmd, "k1k2:pre-k1");

    {
        ScopedVkDebugLabel k1PrimLabel(cmd, "K1-Primary");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          g_k1Pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                g_rayTracingPipelineLayout, 0u, 1u,
                                &g_rayTracingDescriptorSet, 0u, nullptr);
        if(logTrace) RTLog("[K1/K2] pre trace-k1");
        trace(cmd, &g_k1RaygenRegion, &g_k1MissRegion,
              &g_k1HitRegion, &g_k1CallableRegion, width, height, 1u);
    }
    {
        VkMemoryBarrier k1GlassBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        k1GlassBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        k1GlassBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 1u, &k1GlassBarrier, 0u, nullptr, 0u, nullptr);
        recordK1GlassDispatch(cmd, width, height, slots, trace);
    }
    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K1);
    rtCheckpoint(cmd, "k1k2:post-k1");
    if(logTrace) RTLog("[K1/K2] post trace-k1");

    if(k1k2K1Only)
    {
        if(logTrace) RTLog("[K1/K2] K1_ONLY: 跳过 K2");
        rtCheckpoint(cmd, "k1k2:k1only-skip-k2");
        if(g_rayTracingPipeline != VK_NULL_HANDLE)
        {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          g_rayTracingPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                g_rayTracingPipelineLayout, 0u, 1u,
                                &g_rayTracingDescriptorSet, 0u, nullptr);
        }
        return true;
    }

    if(logTrace) RTLog("[K1/K2] pre bind-k2");

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

    if(k2DirectIndirectSplitEnabled())
    {
        if(k2FourSplitEnabled())
        {
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2SUN);
            if(g_k2sunAPipeline == VK_NULL_HANDLE ||
               g_k2sunBPipeline == VK_NULL_HANDLE ||
               g_k2locNeePipeline == VK_NULL_HANDLE ||
               g_k2locGlossyPipeline == VK_NULL_HANDLE ||
               g_k2iPipeline == VK_NULL_HANDLE)
            {
                return false;
            }
            createK2DirectImage(width, height);

            {
                ScopedVkDebugLabel sunALabel(cmd, "K2-SunA");
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                  g_k2sunAPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                        g_rayTracingPipelineLayout, 0u, 1u,
                                        &g_rayTracingDescriptorSet, 0u, nullptr);
                rtCheckpoint(cmd, "k1k2:trace-k2suna");
                trace(cmd, &g_k2sunARaygenRegion, &g_k2sunAMissRegion,
                      &g_k2sunAHitRegion, &g_k2sunACallableRegion, width, height, 1u);
            }
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2SUNA);
            rtCheckpoint(cmd, "k1k2:post-k2suna");
            if(logTrace) RTLog("[K1/K2] post trace-k2suna");

            ensureK2iCompactResources(width, height);
            const uint32_t giBounces =
                g_perfGiBounces.load(std::memory_order_relaxed);
            if(giBounces > 0u &&
               g_sharcZeroed &&
               g_sharcBuffersReady &&
               g_matReuseBuffer != VK_NULL_HANDLE &&
               sharcUpdateEnabled())
            {
                ensureSharcUpdatePipeline();
            }
            const bool sharcActive =
                (giBounces > 0u) &&
                g_sharcZeroed &&
                g_sharcBuffersReady &&
                sharcUpdateEnabled() &&
                (g_sharcUpdatePipeline != VK_NULL_HANDLE);

            auto traceIndirect = getCmdTraceRaysIndirect();
            const bool useMegaCompact =
                traceRaysIndirectSupported() &&
                traceIndirect != nullptr &&
                g_k2MegaCompactPipeline != VK_NULL_HANDLE &&
                s_k2iCompactPipelineLayout != VK_NULL_HANDLE &&
                g_k2iCompactBuffer != VK_NULL_HANDLE &&
                g_k2iCompactBufferAddress != 0u &&
                g_k2DirectView != VK_NULL_HANDLE &&
                g_k2GiTraceView != VK_NULL_HANDLE &&
                g_sparseActiveMaskImage != VK_NULL_HANDLE &&
                g_matReuseBuffer != VK_NULL_HANDLE &&
                matReuseLegsEnabled() &&
                slots == 1u &&
                g_cameraUboHost.giHalfParams[0] == 0.0f;

            if(useMegaCompact)
            {
                const int sparsePct = sparseRatePercent();
                const bool zeroSceneLights =
                    (g_vptTotalEmitterCount.load(std::memory_order_relaxed) == 0u) &&
                    (g_localLightGpuCount == 0u) &&
                    (g_dynamicLightUpload.count == 0u);
                const uint32_t neeMode = zeroSceneLights
                    ? 0u
                    : ((sparsePct > 0 && sparsePct < 100 &&
                        g_cameraUboHost.sparseParams[0] < 0.5f) ? 1u : 2u);
                const uint32_t giMode = (giBounces == 0u)
                    ? 0u
                    : ((sparsePct > 0 && sparsePct < 100) ? 1u : 2u);
                const uint32_t cap = g_k2iCompactCapacityPixels;

                const CompactSlotHeaderHost megaHdrs[5] = {
                    { 0u, 1u, 1u, 1u, 0u, 1u * cap, 0u, 0u },
                    { 0u, 1u, 1u, neeMode, 0u, 2u * cap, 0u, 0u },
                    { 0u, 1u, 1u, 1u, 0u, 3u * cap, 0u, 0u },
                    { 0u, 1u, 1u, giMode, 0u, 4u * cap, 0u, 0u },
                    { 0u, 1u, 1u, sharcActive ? 1u : 0u, 0u, 5u * cap, 0u, 0u },
                };

                VkMemoryBarrier preUpdateBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                preUpdateBarrier.srcAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
                preUpdateBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0u, 1u, &preUpdateBarrier, 0u, nullptr, 0u, nullptr);

                vkCmdUpdateBuffer(
                    cmd, g_k2iCompactBuffer,
                    sizeof(CompactSlotHeaderHost), sizeof(megaHdrs), megaHdrs);

                VkMemoryBarrier xferToComp{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                xferToComp.srcAccessMask =
                    VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                xferToComp.dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_TRANSFER_BIT |
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0u, 1u, &xferToComp, 0u, nullptr, 0u, nullptr);

                {
                    ScopedVkDebugLabel megaLabel(cmd, "K2-Mega-Compact");
                    vkCmdBindPipeline(
                        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_k2MegaCompactPipeline);
                    vkCmdBindDescriptorSets(
                        cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                        s_k2iCompactPipelineLayout, 0u, 1u,
                        &g_rayTracingDescriptorSet, 0u, nullptr);
                    vkCmdDispatch(
                        cmd, (width + 15u) / 16u, (height + 15u) / 16u, 1u);
                }

                VkMemoryBarrier compToTrace{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                compToTrace.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                compToTrace.dstAccessMask =
                    VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                    VK_ACCESS_SHADER_READ_BIT |
                    VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    0u, 1u, &compToTrace, 0u, nullptr, 0u, nullptr);

                // Overlap SHaRC-Update concurrently with K2-SunB / K2-Loc-NEE /
                // K2-Loc-Glossy: SHaRC-Update writes only accumulation while the
                // three legs never write SHaRC accumulation and ptLandingLoad
                // reads only .a/.b/.e.zw.
                if(sharcActive)
                {
                    ScopedVkDebugLabel sharcUpLabel(cmd, "SHaRC-Update");
                    vkCmdBindPipeline(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_sharcUpdatePipeline);
                    vkCmdBindDescriptorSets(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_rayTracingPipelineLayout, 0u, 1u,
                        &g_rayTracingDescriptorSet, 0u, nullptr);
                    traceIndirect(
                        cmd, &g_sharcUpdateRaygenRegion, &g_sharcUpdateMissRegion,
                        &g_sharcUpdateHitRegion, &g_sharcUpdateCallableRegion,
                        g_k2iCompactBufferAddress +
                            5ull * sizeof(CompactSlotHeaderHost));
                }

                {
                    ScopedVkDebugLabel sunBLabel(cmd, "K2-SunB");
                    vkCmdBindPipeline(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_k2sunBPipeline);
                    vkCmdBindDescriptorSets(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_rayTracingPipelineLayout, 0u, 1u,
                        &g_rayTracingDescriptorSet, 0u, nullptr);
                    rtCheckpoint(cmd, "k1k2:trace-k2sunb");
                    traceIndirect(
                        cmd, &g_k2sunBRaygenRegion, &g_k2sunBMissRegion,
                        &g_k2sunBHitRegion, &g_k2sunBCallableRegion,
                        g_k2iCompactBufferAddress +
                            1ull * sizeof(CompactSlotHeaderHost));
                }
                writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2SUNB);
                rtCheckpoint(cmd, "k1k2:post-k2sunb");
                if(logTrace) RTLog("[K1/K2] post trace-k2sunb");

                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);

                rtCheckpoint(cmd, "k1k2:trace-k2loc");
                if(!zeroSceneLights)
                {
                    ScopedVkDebugLabel neeLabel(cmd, "K2-Loc-NEE");
                    vkCmdBindPipeline(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_k2locNeePipeline);
                    vkCmdBindDescriptorSets(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_rayTracingPipelineLayout, 0u, 1u,
                        &g_rayTracingDescriptorSet, 0u, nullptr);
                    traceIndirect(
                        cmd, &g_k2locNeeRaygenRegion, &g_k2locNeeMissRegion,
                        &g_k2locNeeHitRegion, &g_k2locNeeCallableRegion,
                        g_k2iCompactBufferAddress +
                            2ull * sizeof(CompactSlotHeaderHost));

                    VkMemoryBarrier neeToGlossy{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    neeToGlossy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    neeToGlossy.dstAccessMask =
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(
                        cmd,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                        0u, 1u, &neeToGlossy, 0u, nullptr, 0u, nullptr);
                }

                {
                    ScopedVkDebugLabel glossyLabel(cmd, "K2-Loc-Glossy");
                    vkCmdBindPipeline(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_k2locGlossyPipeline);
                    vkCmdBindDescriptorSets(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_rayTracingPipelineLayout, 0u, 1u,
                        &g_rayTracingDescriptorSet, 0u, nullptr);
                    traceIndirect(
                        cmd, &g_k2locGlossyRaygenRegion, &g_k2locGlossyMissRegion,
                        &g_k2locGlossyHitRegion, &g_k2locGlossyCallableRegion,
                        g_k2iCompactBufferAddress +
                            3ull * sizeof(CompactSlotHeaderHost));
                }
                writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2LOC);
                rtCheckpoint(cmd, "k1k2:post-k2loc");
                if(logTrace) RTLog("[K1/K2] post trace-k2loc");

                bool k1k2BarrierDone = false;
                if(g_vptRestirSpatialActiveFrame &&
                   g_vptRestirSpatialPipeline != VK_NULL_HANDLE)
                {
                    recordVPTRestirSpatialPass(cmd, width, height);
                }
                else
                {
                    vkCmdPipelineBarrier(
                        cmd,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);
                    k1k2BarrierDone = true;
                }

                rtCheckpoint(cmd, "k1k2:trace-k2i");
                if(sharcActive)
                {
                    recordSharcResolveImpl(cmd, k1k2BarrierDone);
                }
                if(giBounces > 0u)
                {
                    ScopedVkDebugLabel k2iTraceLabel(cmd, "K2-I-Trace");
                    vkCmdBindPipeline(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_k2iPipeline);
                    vkCmdBindDescriptorSets(
                        cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                        g_rayTracingPipelineLayout, 0u, 1u,
                        &g_rayTracingDescriptorSet, 0u, nullptr);
                    traceIndirect(
                        cmd, &g_k2iRaygenRegion, &g_k2iMissRegion,
                        &g_k2iHitRegion, &g_k2iCallableRegion,
                        g_k2iCompactBufferAddress +
                            4ull * sizeof(CompactSlotHeaderHost));

                    VkMemoryBarrier traceToCompose{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    traceToCompose.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    traceToCompose.dstAccessMask =
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    vkCmdPipelineBarrier(
                        cmd,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0u, 1u, &traceToCompose, 0u, nullptr, 0u, nullptr);
                }
                recordK2iComposePass(cmd, width, height);
                writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2I);
                rtCheckpoint(cmd, "k1k2:post-k2i");
                if(logTrace) RTLog("[K1/K2] post trace-k2i");
            }
            else
            {
                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);

                recordCompactBufferDisabledFallback(cmd, height, 1u);
                {
                    ScopedVkDebugLabel sunBLabel(cmd, "K2-SunB");
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                      g_k2sunBPipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                            g_rayTracingPipelineLayout, 0u, 1u,
                                            &g_rayTracingDescriptorSet, 0u, nullptr);
                    rtCheckpoint(cmd, "k1k2:trace-k2sunb");
                    trace(cmd, &g_k2sunBRaygenRegion, &g_k2sunBMissRegion,
                          &g_k2sunBHitRegion, &g_k2sunBCallableRegion, width, height, 1u);
                }
                writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2SUNB);
                rtCheckpoint(cmd, "k1k2:post-k2sunb");
                if(logTrace) RTLog("[K1/K2] post trace-k2sunb");

                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);

                rtCheckpoint(cmd, "k1k2:trace-k2loc");
                recordK2locSplitDispatch(cmd, width, height, slots, trace);
                writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2LOC);
                rtCheckpoint(cmd, "k1k2:post-k2loc");
                if(logTrace) RTLog("[K1/K2] post trace-k2loc");

                if(g_vptRestirSpatialActiveFrame)
                {
                    recordVPTRestirSpatialPass(cmd, width, height);
                }
                else
                {
                    vkCmdPipelineBarrier(
                        cmd,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);
                }

                rtCheckpoint(cmd, "k1k2:trace-k2i");
                if(giBounces > 0u)
                {
                    if(g_sharcZeroed && g_sharcBuffersReady)
                    {
                        const bool skipBarrier =
                            sharcResolveDueThisFrame() &&
                            g_sharcResolvePipeline != VK_NULL_HANDLE;
                        recordSharcUpdateImpl(cmd, width, height, skipBarrier);
                        recordSharcResolve(cmd);
                    }
                }
                recordK2iDispatch(cmd, width, height, slots, trace, giBounces == 0u);
                writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2I);
                rtCheckpoint(cmd, "k1k2:post-k2i");
                if(logTrace) RTLog("[K1/K2] post trace-k2i");
            }

            if(g_rayTracingPipeline != VK_NULL_HANDLE)
            {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                              g_rayTracingPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                    g_rayTracingPipelineLayout, 0u, 1u,
                                    &g_rayTracingDescriptorSet, 0u, nullptr);
            }
            return true;
        }

        if(k2ThreeSplitEnabled())
        {
            if(g_k2sunPipeline == VK_NULL_HANDLE ||
               g_k2locPipeline == VK_NULL_HANDLE ||
               g_k2iPipeline == VK_NULL_HANDLE)
            {
                return false;
            }
            createK2DirectImage(width, height);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                              g_k2sunPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                    g_rayTracingPipelineLayout, 0u, 1u,
                                    &g_rayTracingDescriptorSet, 0u, nullptr);
            rtCheckpoint(cmd, "k1k2:trace-k2sun");
            trace(cmd, &g_k2sunRaygenRegion, &g_k2sunMissRegion,
                  &g_k2sunHitRegion, &g_k2sunCallableRegion, width, height, 1u);
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2SUN);
            rtCheckpoint(cmd, "k1k2:post-k2sun");
            if(logTrace) RTLog("[K1/K2] post trace-k2sun");

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);

            rtCheckpoint(cmd, "k1k2:trace-k2loc");
            recordK2locDispatch(cmd, width, height, slots, trace);
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2LOC);
            rtCheckpoint(cmd, "k1k2:post-k2loc");
            if(logTrace) RTLog("[K1/K2] post trace-k2loc");

            if(g_vptRestirSpatialActiveFrame)
            {
                recordVPTRestirSpatialPass(cmd, width, height);
            }
            else
            {
                vkCmdPipelineBarrier(
                    cmd,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);
            }

            // 2026-11-18：GI 跳数为 0 时这条间接腿**整条跳过**。
            // 三腿模式里 K2-I 只负责间接（SUN 写直射、LOC 追加局部光），bounces=0
            // 时它写的是恒零增量；而 createK2DirectImage 每帧新建，SUN 已经写过
            // 全屏直射 ⇒ 不跑等价。用户把"GI 跳数"拉到 0 时，这里本来白付
            // 1.8~2.2 ms（实测占全帧 26%）。
            rtCheckpoint(cmd, "k1k2:trace-k2i");
            const uint32_t giBounces3 = g_perfGiBounces.load(std::memory_order_relaxed);
            if(giBounces3 > 0u)
            {
                if(g_sharcZeroed && g_sharcBuffersReady)
                {
                    const bool skipBarrier =
                        sharcResolveDueThisFrame() &&
                        g_sharcResolvePipeline != VK_NULL_HANDLE;
                    recordSharcUpdateImpl(cmd, width, height, skipBarrier);
                    recordSharcResolve(cmd);
                }
            }
            recordK2iDispatch(cmd, width, height, slots, trace, giBounces3 == 0u);
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_K2I);
            rtCheckpoint(cmd, "k1k2:post-k2i");
            if(logTrace) RTLog("[K1/K2] post trace-k2i");

            if(g_rayTracingPipeline != VK_NULL_HANDLE)
            {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                              g_rayTracingPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                    g_rayTracingPipelineLayout, 0u, 1u,
                                    &g_rayTracingDescriptorSet, 0u, nullptr);
            }
            return true;
        }

        if(g_k2dPipeline == VK_NULL_HANDLE || g_k2iPipeline == VK_NULL_HANDLE)
        {
            return false;
        }
        createK2DirectImage(width, height);

        if(logTrace) RTLog("[K1/K2] pre bind-k2d");
        rtCheckpoint(cmd, "k1k2:trace-k2d");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          g_k2dPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                g_rayTracingPipelineLayout, 0u, 1u,
                                &g_rayTracingDescriptorSet, 0u, nullptr);
        trace(cmd, &g_k2dRaygenRegion, &g_k2dMissRegion,
              &g_k2dHitRegion, &g_k2dCallableRegion, width, height, 1u);
        rtCheckpoint(cmd, "k1k2:post-k2d");
        if(logTrace) RTLog("[K1/K2] post trace-k2d");

        if(g_vptRestirSpatialActiveFrame)
        {
            recordVPTRestirSpatialPass(cmd, width, height);
        }
        else
        {
            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                0u, 1u, &k1k2Barrier, 0u, nullptr, 0u, nullptr);
        }

        if(logTrace) RTLog("[K1/K2] pre bind-k2i");
        if(g_perfGiBounces.load(std::memory_order_relaxed) > 0u &&
           g_sharcZeroed && g_sharcBuffersReady)
        {
            const bool skipBarrier =
                sharcResolveDueThisFrame() &&
                g_sharcResolvePipeline != VK_NULL_HANDLE;
            recordSharcUpdateImpl(cmd, width, height, skipBarrier);
            recordSharcResolve(cmd);
        }
        rtCheckpoint(cmd, "k1k2:trace-k2i-2leg");
        recordK2iDispatch(cmd, width, height, slots, trace);
        rtCheckpoint(cmd, "k1k2:post-k2i-2leg");
        if(logTrace) RTLog("[K1/K2] post trace-k2i");

        // 后续 Frame 代码假设主管线在绑；懒编局 main 不存在，跳过绑回
        if(g_rayTracingPipeline != VK_NULL_HANDLE)
        {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                          g_rayTracingPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                                g_rayTracingPipelineLayout, 0u, 1u,
                                &g_rayTracingDescriptorSet, 0u, nullptr);
        }
        return true;
    }

    if(logTrace) RTLog("[K1/K2] pre bind-k2");

    rtCheckpoint(cmd, "k1k2:trace-k2");

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      g_k2Pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            g_rayTracingPipelineLayout, 0u, 1u,
                            &g_rayTracingDescriptorSet, 0u, nullptr);
    if(logTrace) RTLog("[K1/K2] pre trace-k2");
    trace(cmd, &g_k2RaygenRegion, &g_k2MissRegion,
          &g_k2HitRegion, &g_k2CallableRegion, width, height, 1u);
    rtCheckpoint(cmd, "k1k2:post-k2");
    if(logTrace) RTLog("[K1/K2] post trace-k2");

    // K1/K2 两发录完，把主管线绑回去——后续 Frame 代码假设主管线在绑
    if(g_rayTracingPipeline != VK_NULL_HANDLE)
    {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                      g_rayTracingPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                            g_rayTracingPipelineLayout, 0u, 1u,
                            &g_rayTracingDescriptorSet, 0u, nullptr);
    }
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

    const auto sharcSampledPerFrame =
        [](uint32_t value, uint32_t frames) -> double
        {
            return frames != 0u
                ? 256.0 * static_cast<double>(value) / static_cast<double>(frames)
                : 0.0;
        };

    const double sharcUpdatesPerFrame =
        sharcSampledPerFrame(words[8], g_sharcStatsFrameTick);
    const double sharcFailsPerFrame =
        sharcSampledPerFrame(words[10], g_sharcStatsFrameTick);
    const double sharcHitsPerFrame =
        sharcSampledPerFrame(words[11], g_sharcStatsFrameTick);

    const double sharcReachedPerFrame =
        sharcSampledPerFrame(words[12], g_sharcStatsFrameTick);
    const double sharcEligiblePerFrame =
        sharcSampledPerFrame(words[13], g_sharcStatsFrameTick);
    const double sharcHitRateRaw = sharcReachedPerFrame > 0.0
        ? (sharcHitsPerFrame * 100.0 / sharcReachedPerFrame)
        : 0.0;
    const double sharcHitRateElig = sharcEligiblePerFrame > 0.0
        ? (sharcHitsPerFrame * 100.0 / sharcEligiblePerFrame)
        : 0.0;

    char line[1400];
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[SHARC STATS] frames=%u | GI跳数=%u | block=%ux%u | update=%.0f/frame fails=%.0f/frame | "
        "queryHits=%.0f/frame | queryEligible=%.0f/frame | query到达=%.0f/frame | "
        "hitRateRaw=%.1f%% hitRateElig=%.1f%% | lock0=%08X",
        g_sharcStatsFrameTick,
        g_perfGiBounces.load(),
        sharcUpdateBlockForResolution(g_rtOutputWidth, g_rtOutputHeight),
        sharcUpdateBlockForResolution(g_rtOutputWidth, g_rtOutputHeight),
        sharcUpdatesPerFrame,
        sharcFailsPerFrame,
        sharcHitsPerFrame,
        sharcEligiblePerFrame,
        sharcReachedPerFrame,
        sharcHitRateRaw,
        sharcHitRateElig,
        words[0]);

    RTLogFlushed(line);

    g_sharcStatsDone = true;
}
