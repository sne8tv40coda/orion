#include "OrionInternal.h"
#include "TrackingHooks.h"

// ============================================================================
// 2026-11-02 屏幕空间降级链：SSR（镜面反射）+ SSPT（间接光路径追踪）
//
// 用户需求（2026-11-02 原文口径）
// ---------------------------------------------------------------------------
//   · 直接把 SSPT 与 SSR 接入，走 G-buffer；
//   · SSPT 射程 0..12 区块自由可调；
//   · SSR 硬性约束：只有 > 12 区块的镜面命中才允许它接手
//     （为以后做降级处理留出可替换的远场段）；
//   · 两项都在设置里给滑条，默认关闭。
//
// 数据从哪来（G-buffer 两层）
// ---------------------------------------------------------------------------
//   既有 RR guide 组（rt_test.rgen 每帧无条件写，见 cpp/shaders/rt_test.rgen
//   的 27222 起的 guide store 块）：
//     深度 image2D depthImage（binding 7）、法线+粗糙度（15）、
//     漫反射反照率（13）、镜面反照率（14）、镜面命中距离（16）、
//     运动矢量（62）、当前帧颜色（1）。
//   2026-11-02 新增的分项 G-buffer（rgen 写、ss_hybrid.comp 读）：
//     81 = 间接光项贡献（rgb）+ 该项入射辐亮度（a）
//     82 = 镜面反射项贡献（rgb）+ 镜面权重（a）
//   新增 81/82 的唯一目的：SSR/SSPT 要**替换** RT 的对应分项而不是叠加。
//   只靠 guide 组时无法把"间接/镜面"从最终颜色里分离出来，叠加必然双计。
//
// 为什么这个 pass 直接写 g_rtOutputImage（而不是另写一张再合成）
// ---------------------------------------------------------------------------
//   它先由 g_ssBaseColorImage 把本帧颜色拷一份（TRANSFER），随后 dispatch
//   读 base、写 g_rtOutputImage —— 读和写落在**不同图**上，没有同址竞争；
//   这样就不需要再一条合成 pass，也不需要给 RR 改输入。
//
// 降噪口径（2026-11-02 修正）
// ---------------------------------------------------------------------------
//   本工程的降噪器是 **DLSS-RR**：kNrdLocalDiEnabled 是编译期 false
//   （OrionInternal.h:1051），NRD 本地 DI 整条链不参与编译。所以这条 pass
//   必须挂在 RR evaluate **之前**，并且把 SSR 的命中距离写回 RR 的
//   "镜面命中距离" guide（binding 16）—— RR 用那个距离重投影反射内容，
//   内容换成屏幕空间射线却不更新距离就会拖影。见 ss_hybrid.comp 的写入处。
//
// 默认关闭时的代价 = 0
// ---------------------------------------------------------------------------
//   两条滑条都为 0 时 createSSHybridResources() 直接返回 ⇒ 不建 4 张图、
//   不建管线；recordSSHybridPass() 直接返回 ⇒ 不录任何命令（连拷贝都不录）。
//
// 挂点（Frame.cpp）
// ---------------------------------------------------------------------------
//   createSSHybridResources   = createRRGBufferImages 之后（同尺寸同生命周期）
//   recordSSHybridPass        = recordSharcResolve 之后、直接光 atrous / RR 之前
//                               （此时 rgen 的 guide/深度/颜色都是本帧值）
//   recordSSHybridHistoryCopy = 降噪合成之后（历史色用于下一帧的屏幕空间辐射源）
//
// 回退：把两条滑条调回 0（设置界面），或删掉 Frame.cpp 里的三处调用。
// ============================================================================

namespace
{

// 单位：1 区块 = 16 格
constexpr float SS_CHUNK_METERS = 16.0f;

// SSR 的硬性下限（用户 2026-11-02：SSR 只在 > 12 区块的镜面命中上出现）。
// 这里的下限只是宿主侧的日志/钳制口径，真正的逐像素判据在 ss_hybrid.comp
// 的 ssShouldRunSsr() 里（读 rrSpecularHitDistanceImage）。
constexpr float SS_SSR_MIN_CHUNKS = 12.0f;

// 我的 compute 管线用的是 g_rayTracingDescriptorSetLayout；RT 管线重建会连带
// 重建这个布局，所以记下"建管线时用的是哪一份"，句柄变了就重建自己的管线。
VkDescriptorSetLayout sSSHybridPipelineSetLayout = VK_NULL_HANDLE;

// ============================================================================
// Hi-Z 深度金字塔的布局（2026-11-02 第二轮）
//
// 这三个函数与 ss_hzb.comp / ss_hybrid.comp 里的同名逻辑**必须逐字等价**：
//   levelWidth(k)  = ceil(width  / 2^k)
//   levelHeight(k) = ceil(height / 2^k)
//   offset(k)      = Σ_{i<k} levelWidth(i) * levelHeight(i)，offset(1) = 0
// 任何一侧改了，另一侧就会读到错位的格子（表现为 SSR 忽明忽暗，不崩）。
// ============================================================================
constexpr uint32_t SS_HZB_MAX_LEVEL = 6u;

uint32_t ssHzbLevelWidth(uint32_t width, uint32_t level)
{
    return (width + (1u << level) - 1u) >> level;
}

uint32_t ssHzbLevelHeight(uint32_t height, uint32_t level)
{
    return (height + (1u << level) - 1u) >> level;
}

uint32_t ssHzbLevelCount(uint32_t width, uint32_t height)
{
    uint32_t levels = 0u;

    for(uint32_t level = 1u; level <= SS_HZB_MAX_LEVEL; ++level)
    {
        if(ssHzbLevelWidth(width, level) == 0u ||
           ssHzbLevelHeight(height, level) == 0u)
        {
            break;
        }

        levels = level;
    }

    return levels;
}

uint32_t ssHzbOffset(uint32_t width, uint32_t height, uint32_t level)
{
    uint32_t offset = 0u;

    for(uint32_t k = 1u; k < level; ++k)
    {
        offset += ssHzbLevelWidth(width, k) * ssHzbLevelHeight(height, k);
    }

    return offset;
}

void ssBufferBarrier(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkAccessFlags sourceAccess,
    VkAccessFlags destinationAccess)
{
    VkBufferMemoryBarrier barrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
    };

    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer;
    barrier.offset = 0u;
    barrier.size = VK_WHOLE_SIZE;

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u,
        0u,
        nullptr,
        1u,
        &barrier,
        0u,
        nullptr);
}

void destroySSHzbBuffer()
{
    if(g_device != VK_NULL_HANDLE)
    {
        if(g_ssHzbBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(g_device, g_ssHzbBuffer, nullptr);
            g_ssHzbBuffer = VK_NULL_HANDLE;
        }

        if(g_ssHzbMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, g_ssHzbMemory, nullptr);
            g_ssHzbMemory = VK_NULL_HANDLE;
        }
    }

    g_ssHzbLevelCount = 0u;
}

void createSSHzbBuffer(uint32_t width, uint32_t height)
{
    if(g_device == VK_NULL_HANDLE || width == 0u || height == 0u)
    {
        return;
    }

    const uint32_t levels = ssHzbLevelCount(width, height);

    g_ssHzbLevelCount = levels;

    if(levels == 0u)
    {
        return;
    }

    const uint64_t floatCount =
        static_cast<uint64_t>(ssHzbOffset(width, height, levels)) +
        static_cast<uint64_t>(ssHzbLevelWidth(width, levels)) *
            static_cast<uint64_t>(ssHzbLevelHeight(height, levels));

    const VkDeviceSize byteSize =
        static_cast<VkDeviceSize>(floatCount) * sizeof(float);

    VkBufferCreateInfo info{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    };
    info.size = byteSize;
    info.usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    check(
        vkCreateBuffer(g_device, &info, nullptr, &g_ssHzbBuffer),
        "create screen-space Hi-Z buffer failed");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(g_device, g_ssHzbBuffer, &requirements);

    VkMemoryAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = findMemoryType(
        g_physicalDevice,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    check(
        vkAllocateMemory(g_device, &allocateInfo, nullptr, &g_ssHzbMemory),
        "allocate screen-space Hi-Z memory failed");

    check(
        vkBindBufferMemory(g_device, g_ssHzbBuffer, g_ssHzbMemory, 0u),
        "bind screen-space Hi-Z memory failed");

    RTLogFlushed(
        "[SS] Hi-Z 深度金字塔缓冲已创建 levels=" + std::to_string(levels) +
        " bytes=" + std::to_string(static_cast<uint64_t>(byteSize)));
}

void ssImageBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags sourceStage,
    VkPipelineStageFlags destinationStage,
    VkImage image,
    VkImageLayout& trackedLayout,
    VkImageLayout newLayout,
    VkAccessFlags sourceAccess,
    VkAccessFlags destinationAccess)
{
    VkImageMemoryBarrier barrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
    };

    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = trackedLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0u;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.baseArrayLayer = 0u;
    barrier.subresourceRange.layerCount = 1u;

    vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage,
        destinationStage,
        0u,
        0u,
        nullptr,
        0u,
        nullptr,
        1u,
        &barrier);

    trackedLayout = newLayout;
}

// 把一张图从"任何先前用途"变成"compute 可读"。用 ALL_COMMANDS +
// MEMORY_WRITE 是为了覆盖 rgen 的 storage 写 / transfer 拷贝 / 清图三种来源，
// 免得每张图各写一条口径不同的屏障（这条链只在开启时才录制，多同步无感）。
void ssMakeReadable(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout& trackedLayout)
{
    ssImageBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        image,
        trackedLayout,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT);
}

}   // namespace

// ============================================================================
// 设置读取
// ============================================================================
bool ssHybridSsptEnabled()
{
    return g_ssptChunks.load() > 0.5f;
}

bool ssHybridSsrEnabled()
{
    return g_ssrChunks.load() > 0.5f;
}

bool ssHybridEnabled()
{
    return ssHybridSsptEnabled() || ssHybridSsrEnabled();
}

// ============================================================================
// 描述符：binding 81..84 写进既有的 RT set（该 set 已被 sharc resolve 等
// compute pass 复用，所以不需要新池）
// ============================================================================
void updateSSHybridDescriptors()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_ssHybridBoundSet == g_rayTracingDescriptorSet)
    {
        return;
    }

    if(g_ssIndirectTermView == VK_NULL_HANDLE ||
       g_ssSpecularTermView == VK_NULL_HANDLE ||
       g_ssBaseColorView == VK_NULL_HANDLE ||
       g_ssPrevColorView == VK_NULL_HANDLE ||
       g_ssHzbBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo infos[4]{};

    infos[0].imageView = g_ssIndirectTermView;
    infos[1].imageView = g_ssSpecularTermView;
    infos[2].imageView = g_ssBaseColorView;
    infos[3].imageView = g_ssPrevColorView;

    for(uint32_t i = 0u; i < 4u; ++i)
    {
        infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    const uint32_t bindings[4] =
    {
        81u,   // 间接光项（rgen 写 / ss_hybrid 读）
        82u,   // 镜面反射项（rgen 写 / ss_hybrid 读）
        83u,   // 本帧颜色副本
        84u    // 上一帧最终色
    };

    VkWriteDescriptorSet writes[5]{};

    for(uint32_t i = 0u; i < 4u; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding = bindings[i];
        writes[i].dstArrayElement = 0u;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }

    VkDescriptorBufferInfo hzbInfo{};
    hzbInfo.buffer = g_ssHzbBuffer;
    hzbInfo.offset = 0u;
    hzbInfo.range = VK_WHOLE_SIZE;

    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = g_rayTracingDescriptorSet;
    writes[4].dstBinding = 85u;
    writes[4].dstArrayElement = 0u;
    writes[4].descriptorCount = 1u;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &hzbInfo;

    vkUpdateDescriptorSets(g_device, 5u, writes, 0u, nullptr);

    g_ssHybridBoundSet = g_rayTracingDescriptorSet;

    RTLogFlushed(
        "[SS] 分项 G-buffer 描述符已写入 bindings=81,82,83,84 + Hi-Z 85（复用 RT set 0）");
}

// ============================================================================
// 管线
// ============================================================================
void ensureSSHybridPipeline()
{
    if(g_ssHybridPipeline != VK_NULL_HANDLE &&
       g_ssHzbPipeline != VK_NULL_HANDLE &&
       sSSHybridPipelineSetLayout == g_rayTracingDescriptorSetLayout)
    {
        return;
    }

    if(g_ssHybridPipelineFailed)
    {
        return;
    }

    if(g_device == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSetLayout == VK_NULL_HANDLE)
    {
        return;
    }

    // RT 布局换过 ⇒ 旧管线引用的是已销毁的布局，先拆掉
    if(g_ssHybridPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_ssHybridPipeline, nullptr);
        g_ssHybridPipeline = VK_NULL_HANDLE;
    }

    if(g_ssHzbPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_ssHzbPipeline, nullptr);
        g_ssHzbPipeline = VK_NULL_HANDLE;
    }

    if(g_ssHybridPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_ssHybridPipelineLayout, nullptr);
        g_ssHybridPipelineLayout = VK_NULL_HANDLE;
    }

    try
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        // 两条管线共用这段区间：
        //   ss_hybrid.comp 的 SSHybridPush = 16 字节（width/height/frameIndex/hzbLevels）
        //   ss_hzb.comp   的 SSHZBPush    = 32 字节（源/目标偏移与尺寸）
        // 区间大小按两者较大者给，共享一个 pipeline layout。
        pushRange.size = 32u;

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_rayTracingDescriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;

        check(
            vkCreatePipelineLayout(
                g_device,
                &pipelineLayoutInfo,
                nullptr,
                &g_ssHybridPipelineLayout),
            "ss hybrid pipeline layout failed");

        const std::vector<uint8_t> code =
            loadSpv("shaders/ss_hybrid.comp.spv");

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
        pipelineInfo.layout = g_ssHybridPipelineLayout;

        const VkResult result = vkCreateComputePipelines(
            g_device,
            getSharedPipelineCache(),
            1u,
            &pipelineInfo,
            nullptr,
            &g_ssHybridPipeline);

        vkDestroyShaderModule(g_device, shader, nullptr);

        check(result, "ss hybrid compute pipeline failed");

        // ------------------------------------------------------------------
        // Hi-Z 建塔管线（2026-11-02 第二轮）。同一个 set layout + 同一段 push
        // 区间，所以不需要额外的描述符集或布局。
        // ------------------------------------------------------------------
        const std::vector<uint8_t> hzbCode =
            loadSpv("shaders/ss_hzb.comp.spv");

        VkShaderModule hzbShader = createShaderModule(g_device, hzbCode);

        VkPipelineShaderStageCreateInfo hzbStage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        hzbStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        hzbStage.module = hzbShader;
        hzbStage.pName = "main";

        VkComputePipelineCreateInfo hzbPipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        hzbPipelineInfo.stage = hzbStage;
        hzbPipelineInfo.layout = g_ssHybridPipelineLayout;

        const VkResult hzbResult = vkCreateComputePipelines(
            g_device,
            getSharedPipelineCache(),
            1u,
            &hzbPipelineInfo,
            nullptr,
            &g_ssHzbPipeline);

        vkDestroyShaderModule(g_device, hzbShader, nullptr);

        check(hzbResult, "ss hzb compute pipeline failed");

        sSSHybridPipelineSetLayout = g_rayTracingDescriptorSetLayout;

        RTLogFlushed(
            "[SS] ss_hybrid 管线已创建（SSR + SSPT，复用 RT set 0，local_size=8x8）");
    }
    catch(...)
    {
        g_ssHybridPipelineFailed = true;
        g_ssHybridPipeline = VK_NULL_HANDLE;
        sSSHybridPipelineSetLayout = VK_NULL_HANDLE;

        RTLogFlushed("[SS] ss_hybrid 管线创建失败 ⇒ 本次会话关闭屏幕空间降级链");
    }
}

// ============================================================================
// 资源
// ============================================================================
void createSSHybridResources(uint32_t width, uint32_t height)
{
    if(!ssHybridEnabled())
    {
        // 默认关闭：一张图都不建
        return;
    }

    if(g_device == VK_NULL_HANDLE || width == 0u || height == 0u)
    {
        return;
    }

    const bool alreadyValid =
        g_ssIndirectTermImage != VK_NULL_HANDLE &&
        g_ssSpecularTermImage != VK_NULL_HANDLE &&
        g_ssBaseColorImage != VK_NULL_HANDLE &&
        g_ssPrevColorImage != VK_NULL_HANDLE &&
        g_ssHybridWidth == width &&
        g_ssHybridHeight == height;

    if(alreadyValid)
    {
        updateSSHybridDescriptors();
        ensureSSHybridPipeline();
        return;
    }

    const bool hasOldResources =
        g_ssIndirectTermImage != VK_NULL_HANDLE ||
        g_ssSpecularTermImage != VK_NULL_HANDLE ||
        g_ssBaseColorImage != VK_NULL_HANDLE ||
        g_ssPrevColorImage != VK_NULL_HANDLE;

    if(hasOldResources)
    {
        check(
            FGDeviceWaitIdle(g_device),
            "wait device before screen-space resources recreate failed");
    }

    destroyHistoryImage(
        g_ssIndirectTermImage,
        g_ssIndirectTermMemory,
        g_ssIndirectTermView,
        g_ssIndirectTermLayout);

    destroyHistoryImage(
        g_ssSpecularTermImage,
        g_ssSpecularTermMemory,
        g_ssSpecularTermView,
        g_ssSpecularTermLayout);

    destroyHistoryImage(
        g_ssBaseColorImage,
        g_ssBaseColorMemory,
        g_ssBaseColorView,
        g_ssBaseColorLayout);

    destroyHistoryImage(
        g_ssPrevColorImage,
        g_ssPrevColorMemory,
        g_ssPrevColorView,
        g_ssPrevColorLayout);

    // 全部 rgba16f：与 g_rtOutputImage 同格式 ⇒ 可以直接 vkCmdCopyImage
    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_ssIndirectTermImage,
        g_ssIndirectTermMemory,
        g_ssIndirectTermView,
        g_ssIndirectTermLayout);

    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_ssSpecularTermImage,
        g_ssSpecularTermMemory,
        g_ssSpecularTermView,
        g_ssSpecularTermLayout);

    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_ssBaseColorImage,
        g_ssBaseColorMemory,
        g_ssBaseColorView,
        g_ssBaseColorLayout);

    createRRGuideImage(
        width,
        height,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        g_ssPrevColorImage,
        g_ssPrevColorMemory,
        g_ssPrevColorView,
        g_ssPrevColorLayout);

    // Hi-Z 金字塔缓冲：尺寸随分辨率变 ⇒ 跟 4 张图一起重建
    destroySSHzbBuffer();
    createSSHzbBuffer(width, height);

    g_ssHybridWidth = width;
    g_ssHybridHeight = height;

    // 新图的内容未定义 ⇒ 历史色必须在第一帧清黑，否则 SSR 会采样到垃圾
    g_ssHybridHistoryValid = false;

    // 图换了 ⇒ 描述符必须重写（view 句柄变了）
    g_ssHybridBoundSet = VK_NULL_HANDLE;

    updateSSHybridDescriptors();
    ensureSSHybridPipeline();

    RTLogFlushed(
        "[SS] 屏幕空间降级链资源已创建 " +
        std::to_string(width) + "x" + std::to_string(height) +
        "（分项 G-buffer + base 副本 + 历史色，全 rgba16f）");
}

void destroySSHybridResources()
{
    if(g_device != VK_NULL_HANDLE)
    {
        if(g_ssHybridPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(g_device, g_ssHybridPipeline, nullptr);
            g_ssHybridPipeline = VK_NULL_HANDLE;
        }

        if(g_ssHzbPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(g_device, g_ssHzbPipeline, nullptr);
            g_ssHzbPipeline = VK_NULL_HANDLE;
        }

        if(g_ssHybridPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(g_device, g_ssHybridPipelineLayout, nullptr);
            g_ssHybridPipelineLayout = VK_NULL_HANDLE;
        }

        destroySSHzbBuffer();

        destroyHistoryImage(
            g_ssIndirectTermImage,
            g_ssIndirectTermMemory,
            g_ssIndirectTermView,
            g_ssIndirectTermLayout);

        destroyHistoryImage(
            g_ssSpecularTermImage,
            g_ssSpecularTermMemory,
            g_ssSpecularTermView,
            g_ssSpecularTermLayout);

        destroyHistoryImage(
            g_ssBaseColorImage,
            g_ssBaseColorMemory,
            g_ssBaseColorView,
            g_ssBaseColorLayout);

        destroyHistoryImage(
            g_ssPrevColorImage,
            g_ssPrevColorMemory,
            g_ssPrevColorView,
            g_ssPrevColorLayout);
    }

    g_ssHybridWidth = 0u;
    g_ssHybridHeight = 0u;
    g_ssHybridHistoryValid = false;
    g_ssHybridBoundSet = VK_NULL_HANDLE;
    g_ssHybridPipelineFailed = false;
    sSSHybridPipelineSetLayout = VK_NULL_HANDLE;
}

// ============================================================================
// 录制
// ============================================================================
void recordSSHybridPass(
    VkCommandBuffer commandBuffer,
    uint32_t width,
    uint32_t height)
{
    if(!ssHybridEnabled())
    {
        return;
    }

    if(commandBuffer == VK_NULL_HANDLE ||
       g_ssHybridPipeline == VK_NULL_HANDLE ||
       g_ssHzbPipeline == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_rtOutputImage == VK_NULL_HANDLE ||
       g_ssHzbBuffer == VK_NULL_HANDLE ||
       g_glassBlueNoiseReady == false)
    {
        // 蓝噪声（binding 76）由 Glass.cpp 上传；没就绪就不跑，
        // 否则会读到未写入的描述符。
        return;
    }

    if(g_ssHybridWidth != width || g_ssHybridHeight != height)
    {
        // 资源还没跟上本帧尺寸（重尺寸那一帧）：本帧不跑，下一帧资源就位
        return;
    }

    // ------------------------------------------------------------------
    // 1) 本帧颜色 → base 副本（读/写落在不同图，dispatch 才不会同址竞争）
    // ------------------------------------------------------------------
    ssImageBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        g_rtOutputImage,
        g_rtOutputLayout,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);

    ssImageBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        g_ssBaseColorImage,
        g_ssBaseColorLayout,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    // 历史色第一次使用：清黑（否则 SSR 采样到未定义内容）
    if(!g_ssHybridHistoryValid)
    {
        ssImageBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            g_ssPrevColorImage,
            g_ssPrevColorLayout,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);

        VkClearColorValue clearValue{};
        clearValue.float32[0] = 0.0f;
        clearValue.float32[1] = 0.0f;
        clearValue.float32[2] = 0.0f;
        clearValue.float32[3] = 0.0f;

        VkImageSubresourceRange clearRange{};
        clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearRange.baseMipLevel = 0u;
        clearRange.levelCount = 1u;
        clearRange.baseArrayLayer = 0u;
        clearRange.layerCount = 1u;

        vkCmdClearColorImage(
            commandBuffer,
            g_ssPrevColorImage,
            VK_IMAGE_LAYOUT_GENERAL,
            &clearValue,
            1u,
            &clearRange);

        g_ssHybridHistoryValid = true;

        RTLogFlushed("[SS] 历史色首帧已清黑（SSR/SSPT 首帧无屏幕空间辐射源）");
    }

    {
        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.mipLevel = 0u;
        region.srcSubresource.baseArrayLayer = 0u;
        region.srcSubresource.layerCount = 1u;
        region.dstSubresource = region.srcSubresource;
        region.extent.width = width;
        region.extent.height = height;
        region.extent.depth = 1u;

        vkCmdCopyImage(
            commandBuffer,
            g_rtOutputImage,
            VK_IMAGE_LAYOUT_GENERAL,
            g_ssBaseColorImage,
            VK_IMAGE_LAYOUT_GENERAL,
            1u,
            &region);
    }

    // ------------------------------------------------------------------
    // 2) 让 dispatch 能读全部输入、能写 g_rtOutputImage
    // ------------------------------------------------------------------
    ssMakeReadable(commandBuffer, g_ssBaseColorImage, g_ssBaseColorLayout);
    ssMakeReadable(commandBuffer, g_ssPrevColorImage, g_ssPrevColorLayout);

    // ------------------------------------------------------------------
    // 2b) G-buffer 的可见性依赖：这些图是本帧 rgen 在 RAY_TRACING 阶段写的，
    //     compute 读它们必须有一条 RAY_TRACING/COMPUTE-write → COMPUTE-read 的
    //     内存依赖（它们早就在 GENERAL 布局，这里只补内存可见性，不换布局）。
    //     漏掉这条不会崩，但会读到上一帧的 guide ⇒ SSR/SSPT 整体滞后一帧。
    // ------------------------------------------------------------------
    {
        struct SSGBufferSource
        {
            VkImage image;
            VkImageLayout* layout;
        };

        const SSGBufferSource sources[8] =
        {
            { g_dlssDepthImage, &g_dlssDepthLayout },
            { g_rrNormalRoughnessImage, &g_rrNormalRoughnessLayout },
            { g_rrDiffuseAlbedoImage, &g_rrDiffuseAlbedoLayout },
            { g_rrSpecularAlbedoImage, &g_rrSpecularAlbedoLayout },
            { g_rrSpecularHitDistanceImage, &g_rrSpecularHitDistanceLayout },
            { g_motionImage, &g_motionLayout },
            { g_ssIndirectTermImage, &g_ssIndirectTermLayout },
            { g_ssSpecularTermImage, &g_ssSpecularTermLayout }
        };

        VkImageMemoryBarrier gbufferBarriers[8]{};

        uint32_t count = 0u;

        for(uint32_t i = 0u; i < 8u; ++i)
        {
            if(sources[i].image == VK_NULL_HANDLE)
            {
                continue;
            }

            VkImageMemoryBarrier& barrier = gbufferBarriers[count++];

            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            // 其中 g_rrSpecularHitDistanceImage 本 pass 还要**写**（SSR 替换后
            // 回写屏幕空间命中距离给 RR 重投影），所以读权限里带上写。
            barrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barrier.oldLayout = *sources[i].layout;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = sources[i].image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0u;
            barrier.subresourceRange.levelCount = 1u;
            barrier.subresourceRange.baseArrayLayer = 0u;
            barrier.subresourceRange.layerCount = 1u;

            *sources[i].layout = VK_IMAGE_LAYOUT_GENERAL;
        }

        if(count > 0u)
        {
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0u,
                0u,
                nullptr,
                0u,
                nullptr,
                count,
                gbufferBarriers);
        }
    }

    // ------------------------------------------------------------------
    // 2c) Hi-Z 深度金字塔（2026-11-02 第二轮）：逐级 max 归约，
    //     每级之间一条 buffer 屏障（同级多写的依赖）。
    //     level 1 的源是深度图，level k 的源是 k-1 级。
    // ------------------------------------------------------------------
    if(g_ssHzbLevelCount >= 1u)
    {
        vkCmdBindPipeline(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_ssHzbPipeline);

        vkCmdBindDescriptorSets(
            commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_ssHybridPipelineLayout,
            0u,
            1u,
            &g_rayTracingDescriptorSet,
            0u,
            nullptr);

        struct SSHZBPush
        {
            uint32_t sourceIsDepth;
            uint32_t sourceOffset;
            uint32_t destinationOffset;
            uint32_t destinationWidth;
            uint32_t destinationHeight;
            uint32_t sourceWidth;
            uint32_t sourceHeight;
            uint32_t reserved;
        };

        for(uint32_t level = 1u; level <= g_ssHzbLevelCount; ++level)
        {
            const uint32_t destinationWidth =
                ssHzbLevelWidth(width, level);
            const uint32_t destinationHeight =
                ssHzbLevelHeight(height, level);

            SSHZBPush push{};
            push.sourceIsDepth = (level == 1u) ? 1u : 0u;
            push.sourceOffset =
                (level == 1u) ? 0u : ssHzbOffset(width, height, level - 1u);
            push.destinationOffset = ssHzbOffset(width, height, level);
            push.destinationWidth = destinationWidth;
            push.destinationHeight = destinationHeight;
            push.sourceWidth =
                (level == 1u) ? width : ssHzbLevelWidth(width, level - 1u);
            push.sourceHeight =
                (level == 1u) ? height : ssHzbLevelHeight(height, level - 1u);
            push.reserved = 0u;

            vkCmdPushConstants(
                commandBuffer,
                g_ssHybridPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT,
                0u,
                sizeof(push),
                &push);

            vkCmdDispatch(
                commandBuffer,
                (destinationWidth + 7u) / 8u,
                (destinationHeight + 7u) / 8u,
                1u);

            if(level < g_ssHzbLevelCount)
            {
                ssBufferBarrier(
                    commandBuffer,
                    g_ssHzbBuffer,
                    VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            }
        }

        ssBufferBarrier(
            commandBuffer,
            g_ssHzbBuffer,
            VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    }

    ssImageBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        g_rtOutputImage,
        g_rtOutputLayout,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_SHADER_WRITE_BIT);

    // ------------------------------------------------------------------
    // 3) dispatch
    // ------------------------------------------------------------------
    vkCmdBindPipeline(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_ssHybridPipeline);

    vkCmdBindDescriptorSets(
        commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_ssHybridPipelineLayout,
        0u,
        1u,
        &g_rayTracingDescriptorSet,
        0u,
        nullptr);

    struct SSHybridPush
    {
        uint32_t width;
        uint32_t height;
        uint32_t frameIndex;
        uint32_t hzbLevels;
    };

    SSHybridPush push{};
    push.width = width;
    push.height = height;
    // 帧号由着色器直接读 cam.frameCount（蓝噪声层索引），这里留 0 占位
    push.frameIndex = 0u;
    // Hi-Z 可用级数；0 时着色器退回固定步长（不会越界读缓冲）
    push.hzbLevels = g_ssHzbLevelCount;

    vkCmdPushConstants(
        commandBuffer,
        g_ssHybridPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0u,
        sizeof(push),
        &push);

    vkCmdDispatch(
        commandBuffer,
        (width + 7u) / 8u,
        (height + 7u) / 8u,
        1u);

    // ------------------------------------------------------------------
    // 4) 让下游（NRD / 云 / DLSS）看到写出的颜色
    // ------------------------------------------------------------------
    ssImageBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        g_rtOutputImage,
        g_rtOutputLayout,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

    // 描述符/管线可能因为 RT 重建而需要补写（稳态是两次指针比较）
    updateSSHybridDescriptors();
    ensureSSHybridPipeline();
}

void recordSSHybridHistoryCopy(VkCommandBuffer commandBuffer)
{
    if(!ssHybridEnabled())
    {
        return;
    }

    if(commandBuffer == VK_NULL_HANDLE ||
       g_rtOutputImage == VK_NULL_HANDLE ||
       g_ssPrevColorImage == VK_NULL_HANDLE ||
       g_ssHybridWidth == 0u ||
       g_ssHybridHeight == 0u ||
       !g_ssHybridHistoryValid)
    {
        return;
    }

    ssImageBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        g_rtOutputImage,
        g_rtOutputLayout,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);

    ssImageBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        g_ssPrevColorImage,
        g_ssPrevColorLayout,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    {
        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.mipLevel = 0u;
        region.srcSubresource.baseArrayLayer = 0u;
        region.srcSubresource.layerCount = 1u;
        region.dstSubresource = region.srcSubresource;
        region.extent.width = g_ssHybridWidth;
        region.extent.height = g_ssHybridHeight;
        region.extent.depth = 1u;

        vkCmdCopyImage(
            commandBuffer,
            g_rtOutputImage,
            VK_IMAGE_LAYOUT_GENERAL,
            g_ssPrevColorImage,
            VK_IMAGE_LAYOUT_GENERAL,
            1u,
            &region);
    }

    ssImageBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        g_rtOutputImage,
        g_rtOutputLayout,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

    ssImageBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        g_ssPrevColorImage,
        g_ssPrevColorLayout,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
}
