// Orion backend — Glass 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"




// 把这些图（连同被新 pass 只读的三张 RR guide）一次性转到 GENERAL。
// 与 prepareRRGuideForRayGen 同一套做法，区别是这里的图都由我自己管理布局，
// 所以判据用各自的 layout 变量，转完就地更新（幂等，不需要每帧重跑）。
void prepareGlassAlphaBlendImages(VkCommandBuffer cmd)
{
    VkImage images[8] =
    {
        g_alphaBlendGbufferImage,
        g_alphaBlendMetaImage,
        g_alphaBlendLightImage,
        g_alphaBlendRadianceImage,
        g_alphaBlendVolumeImage,
        g_dlssDepthImage,
        g_rrNormalRoughnessImage,
        g_rrDiffuseAlbedoImage
    };

    VkImageLayout* layouts[8] =
    {
        &g_alphaBlendGbufferLayout,
        &g_alphaBlendMetaLayout,
        &g_alphaBlendLightLayout,
        &g_alphaBlendRadianceLayout,
        &g_alphaBlendVolumeLayout,
        &g_dlssDepthLayout,
        &g_rrNormalRoughnessLayout,
        &g_rrDiffuseAlbedoLayout
    };

    VkImageMemoryBarrier barriers[8]{};
    uint32_t count = 0u;

    for(uint32_t i = 0u; i < 8u; ++i)
    {
        if(images[i] == VK_NULL_HANDLE ||
           *layouts[i] == VK_IMAGE_LAYOUT_GENERAL)
        {
            continue;
        }

        VkImageMemoryBarrier& barrier = barriers[count];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = *layouts[i];
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.srcAccessMask = 0u;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.image = images[i];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0u;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.baseArrayLayer = 0u;
        barrier.subresourceRange.layerCount = 1u;

        ++count;
    }

    if(count == 0u)
    {
        return;
    }

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u,
        0u, nullptr,
        0u, nullptr,
        count, barriers);

    for(uint32_t i = 0u; i < 8u; ++i)
    {
        if(images[i] != VK_NULL_HANDLE)
        {
            *layouts[i] = VK_IMAGE_LAYOUT_GENERAL;
        }
    }
}

// 2026-10-27：Remix 的时空蓝噪声（128x128x64 R8）。
//
// 数据来源：_ref/dxvk-remix/src/util/util_blueNoise_128x128x64.h（MIT），
// 已原样导出为 shaders/blue_noise_128x128x64.r8（1,048,576 字节 =
// 64 layer x 128 x 128）。文件缺失时用确定性哈希填一份，保证描述符永远有效
// （宁可分布退化，也不能给 raygen 一个空的 storage buffer）。
void createGlassBlueNoiseBuffer()
{
    if(g_glassBlueNoiseBuffer != VK_NULL_HANDLE || g_glassBlueNoiseReady)
    {
        return;
    }

    constexpr size_t kBlueNoiseBytes = 64u * 128u * 128u;
    std::vector<uint8_t> data;

    try
    {
        data = loadBinaryFile("shaders/blue_noise_128x128x64.r8");
    }
    catch(const std::exception& error)
    {
        RTLog(std::string("[GLASS ALPHA BLEND] blue noise missing, hashed placeholder: ") +
              error.what());
    }

    if(data.size() != kBlueNoiseBytes)
    {
        if(!data.empty())
        {
            RTLog("[GLASS ALPHA BLEND] blue noise size mismatch, hashed placeholder");
        }

        data.resize(kBlueNoiseBytes);

        uint32_t state = 0x9E3779B9u;
        for(size_t i = 0u; i < kBlueNoiseBytes; ++i)
        {
            state ^= state >> 16u;
            state *= 0x7FEB352Du;
            state ^= state >> 15u;
            state *= 0x846CA68Bu;
            state ^= state >> 16u;
            data[i] = static_cast<uint8_t>(state & 0xFFu);
        }
    }

    createBuffer(
        g_device,
        g_physicalDevice,
        static_cast<VkDeviceSize>(kBlueNoiseBytes),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_glassBlueNoiseBuffer,
        g_glassBlueNoiseMemory,
        true);

    void* mapped = nullptr;
    check(
        vkMapMemory(
            g_device, g_glassBlueNoiseMemory, 0u,
            static_cast<VkDeviceSize>(kBlueNoiseBytes), 0u, &mapped),
        "map blue noise buffer failed");

    std::memcpy(mapped, data.data(), kBlueNoiseBytes);
    vkUnmapMemory(g_device, g_glassBlueNoiseMemory);

    g_glassBlueNoiseReady = true;

    RTLog("[GLASS ALPHA BLEND] blue noise 128x128x64 uploaded");
}
void destroySparseActiveMaskImage()
{
    if(g_sparseActiveMaskView)
    {
        vkDestroyImageView(g_device, g_sparseActiveMaskView, nullptr);
        g_sparseActiveMaskView = VK_NULL_HANDLE;
    }

    if(g_sparseActiveMaskImage)
    {
        vkDestroyImage(g_device, g_sparseActiveMaskImage, nullptr);
        g_sparseActiveMaskImage = VK_NULL_HANDLE;
    }

    if(g_sparseActiveMaskMemory)
    {
        vkFreeMemory(g_device, g_sparseActiveMaskMemory, nullptr);
        g_sparseActiveMaskMemory = VK_NULL_HANDLE;
    }

    g_sparseActiveMaskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sparseActiveMaskWidth = 0u;
    g_sparseActiveMaskHeight = 0u;
    g_sparseActiveMaskNeedsClear = true;
}
bool ensureSparseActiveMaskImage(uint32_t renderWidth, uint32_t renderHeight)
{
    const uint32_t maskWidth = (renderWidth + 3u) / 4u;
    const uint32_t maskHeight = (renderHeight + 1u) / 2u;

    if(g_sparseActiveMaskImage != VK_NULL_HANDLE &&
       g_sparseActiveMaskWidth == maskWidth &&
       g_sparseActiveMaskHeight == maskHeight)
    {
        return true;
    }

    if(g_sparseActiveMaskImage != VK_NULL_HANDLE)
    {
        if(FGDeviceWaitIdle(g_device) != VK_SUCCESS)
        {
            return false;
        }

        destroySparseActiveMaskImage();
    }

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8_UINT;
    info.extent = { maskWidth, maskHeight, 1u };
    info.mipLevels = 1u;
    info.arrayLayers = 1u;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if(vkCreateImage(g_device, &info, nullptr, &g_sparseActiveMaskImage) != VK_SUCCESS)
    {
        g_sparseActiveMaskImage = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_sparseActiveMaskImage, &requirements);

    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = findMemoryType(
        g_physicalDevice,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if(vkAllocateMemory(g_device, &allocate, nullptr, &g_sparseActiveMaskMemory) != VK_SUCCESS)
    {
        destroySparseActiveMaskImage();
        return false;
    }

    if(vkBindImageMemory(
           g_device,
           g_sparseActiveMaskImage,
           g_sparseActiveMaskMemory,
           0u) != VK_SUCCESS)
    {
        destroySparseActiveMaskImage();
        return false;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_sparseActiveMaskImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UINT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;

    if(vkCreateImageView(g_device, &viewInfo, nullptr, &g_sparseActiveMaskView) != VK_SUCCESS)
    {
        destroySparseActiveMaskImage();
        return false;
    }

    g_sparseActiveMaskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sparseActiveMaskWidth = maskWidth;
    g_sparseActiveMaskHeight = maskHeight;
    g_sparseActiveMaskNeedsClear = true;

    RTLog("[SPARSE MASK] created " + std::to_string(maskWidth) + "x" +
          std::to_string(maskHeight) + " (4x2 blocks)");

    return true;
}
bool ensureSparseActiveMaskPipeline()
{
    if(g_sparseActiveMaskPipeline != VK_NULL_HANDLE)
    {
        return true;
    }

    if(g_sparseActiveMaskPipelineFailed)
    {
        return false;
    }

    std::vector<uint8_t> code;

    try
    {
        code = loadSpv("shaders/sparse_active_mask.comp.spv");
    }
    catch(const std::exception& error)
    {
        RTLog(std::string("[SPARSE MASK] missing shader: ") + error.what());
        g_sparseActiveMaskPipelineFailed = true;
        return false;
    }

    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = code.size();
    moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    if(vkCreateShaderModule(g_device, &moduleInfo, nullptr, &g_sparseActiveMaskShader) != VK_SUCCESS)
    {
        g_sparseActiveMaskPipelineFailed = true;
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[4]{};

    bindings[0].binding = 0u;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1u;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1u;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1u;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    // 2026-11-04：逐像素采样率（RTX Remix 口径，见 shader 的 SPARSE_EDGE_AWARE）。
    // binding 2 = 上一帧深度，binding 3 = 上一帧颜色（rtOutput）。
    // 两者只用于"高对比块抬到全速率"的判定，阴影边界正是靠**颜色**这一步判出来的
    // （阴影终止线两侧深度相同，只有亮度差）。
    bindings[2].binding = 2u;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1u;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[3].binding = 3u;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[3].descriptorCount = 1u;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo setLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setLayoutInfo.bindingCount = 4u;
    setLayoutInfo.pBindings = bindings;

    if(vkCreateDescriptorSetLayout(g_device, &setLayoutInfo, nullptr, &g_sparseActiveMaskSetLayout) != VK_SUCCESS)
    {
        g_sparseActiveMaskPipelineFailed = true;
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 1u;

    VkDescriptorPoolSize poolSizeBuffer{};
    poolSizeBuffer.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizeBuffer.descriptorCount = 1u;

    VkDescriptorPoolSize poolSizeSampled{};
    poolSizeSampled.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizeSampled.descriptorCount = 2u;

    VkDescriptorPoolSize poolSizes[3] = { poolSize, poolSizeBuffer, poolSizeSampled };

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;

    if(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_sparseActiveMaskPool) != VK_SUCCESS)
    {
        g_sparseActiveMaskPipelineFailed = true;
        return false;
    }

    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = g_sparseActiveMaskPool;
    allocateInfo.descriptorSetCount = 1u;
    allocateInfo.pSetLayouts = &g_sparseActiveMaskSetLayout;

    if(vkAllocateDescriptorSets(g_device, &allocateInfo, &g_sparseActiveMaskSet) != VK_SUCCESS)
    {
        g_sparseActiveMaskPipelineFailed = true;
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0u;
    pushRange.size = sizeof(SparseActiveMaskPush);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1u;
    pipelineLayoutInfo.pSetLayouts = &g_sparseActiveMaskSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1u;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;

    if(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr, &g_sparseActiveMaskPipelineLayout) != VK_SUCCESS)
    {
        g_sparseActiveMaskPipelineFailed = true;
        return false;
    }

    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = g_sparseActiveMaskShader;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = g_sparseActiveMaskPipelineLayout;

    if(vkCreateComputePipelines(
           g_device,
           VK_NULL_HANDLE,
           1u,
           &pipelineInfo,
           nullptr,
           &g_sparseActiveMaskPipeline) != VK_SUCCESS)
    {
        RTLog("[SPARSE MASK] pipeline create failed");
        g_sparseActiveMaskPipelineFailed = true;
        return false;
    }

    return true;
}

// 把 mask 同时写进 mask pass 自己的 set 与 RT 主 set 的 binding 78。
void updateSparseActiveMaskDescriptors()
{
    if(g_sparseActiveMaskView == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_sparseActiveMaskSet != VK_NULL_HANDLE)
    {
        VkDescriptorImageInfo maskInfo{};
        maskInfo.imageView = g_sparseActiveMaskView;
        maskInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo noiseInfo{};
        noiseInfo.buffer = g_glassBlueNoiseBuffer;
        noiseInfo.offset = 0u;
        noiseInfo.range = VK_WHOLE_SIZE;

        // 2026-11-04：逐像素采样率（RTX Remix 口径）。
        // 深度 = g_dlssDepthView，颜色 = g_rtOutputView（上一帧）。
        // 两者任一缺失时不写这两格 —— 此时 shader 的 SPARSE_EDGE_AWARE 也必须为 0，
        // 否则采到未绑定的描述符（未定义行为）。
        const bool edgeAwareBound =
            g_dlssDepthView != VK_NULL_HANDLE &&
            g_rtOutputView != VK_NULL_HANDLE;

        VkDescriptorImageInfo depthInfo{};
        depthInfo.sampler = getLinearSampler();
        depthInfo.imageView = g_dlssDepthView;
        depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo colorInfo{};
        colorInfo.sampler = getLinearSampler();
        colorInfo.imageView = g_rtOutputView;
        colorInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[4]{};

        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = g_sparseActiveMaskSet;
        writes[0].dstBinding = 0u;
        writes[0].descriptorCount = 1u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &maskInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = g_sparseActiveMaskSet;
        writes[1].dstBinding = 1u;
        writes[1].descriptorCount = 1u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &noiseInfo;

        uint32_t writeCount = 2u;

        if(edgeAwareBound)
        {
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = g_sparseActiveMaskSet;
            writes[2].dstBinding = 2u;
            writes[2].descriptorCount = 1u;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &depthInfo;

            writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[3].dstSet = g_sparseActiveMaskSet;
            writes[3].dstBinding = 3u;
            writes[3].descriptorCount = 1u;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[3].pImageInfo = &colorInfo;

            writeCount = 4u;
        }

        vkUpdateDescriptorSets(g_device, writeCount, writes, 0u, nullptr);
    }

    if(g_rayTracingDescriptorSet != VK_NULL_HANDLE)
    {
        VkDescriptorImageInfo maskInfo{};
        maskInfo.imageView = g_sparseActiveMaskView;
        maskInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = g_rayTracingDescriptorSet;
        write.dstBinding = 78u;
        write.descriptorCount = 1u;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &maskInfo;

        vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);
    }
}

// 每帧在 RT trace 之前调用：清首帧 → dispatch → 转到可读。
void recordSparseActiveMask(
    VkCommandBuffer cmd,
    uint32_t renderWidth,
    uint32_t renderHeight)
{
    if(cmd == VK_NULL_HANDLE ||
       renderWidth == 0u ||
       renderHeight == 0u)
    {
        return;
    }

    // 2026-11-06：采样率 <= 0 时本 pass 的输出没有任何消费者 —— 消费方
    // rgen_vpt_sparse.glsl:57-73 在 samplingRate<=0 时直接 return true，连
    // binding 78 的 mask 都不读。而 g_sparseEffectiveRate 由 Frame.cpp:389-391
    // 每帧从 sparseSamplingRateEffective() 写入，设置页「稀疏渲染采样率% = 0(关)」
    // 或 ORION_SPARSE_RATE=0 时恒为 0。
    // 此时整条 pass 是纯空转：2× vkUpdateDescriptorSets + 3× vkCmdPipelineBarrier
    // + 1 dispatch（1920x1080 下约 51.8 万线程）。
    // 首帧 clear 一并不做：图像保持未创建/UNDEFINED，needsClear 仍为真，等采样率
    // 真正 > 0 时照原路径"清 → dispatch → 转可读"，语义不变。
    if(g_sparseEffectiveRate.load(std::memory_order_relaxed) <= 0.0f)
    {
        return;
    }

    if(!ensureSparseActiveMaskImage(renderWidth, renderHeight))
    {
        return;
    }

    if(!g_glassBlueNoiseReady)
    {
        createGlassBlueNoiseBuffer();
    }

    if(!ensureSparseActiveMaskPipeline())
    {
        return;
    }

    updateSparseActiveMaskDescriptors();

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0u;
    range.levelCount = 1u;
    range.baseArrayLayer = 0u;
    range.layerCount = 1u;

    if(g_sparseActiveMaskNeedsClear)
    {
        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = 0u;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = g_sparseActiveMaskLayout;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = g_sparseActiveMaskImage;
        toTransfer.subresourceRange = range;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);

        VkClearColorValue clear{};
        clear.uint32[0] = 255u;

        vkCmdClearColorImage(
            cmd,
            g_sparseActiveMaskImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clear,
            1u,
            &range);

        VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image = g_sparseActiveMaskImage;
        toGeneral.subresourceRange = range;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &toGeneral);

        g_sparseActiveMaskLayout = VK_IMAGE_LAYOUT_GENERAL;
        g_sparseActiveMaskNeedsClear = false;
    }

    {
        VkImageMemoryBarrier beforeWrite{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        beforeWrite.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        beforeWrite.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        beforeWrite.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        beforeWrite.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        beforeWrite.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        beforeWrite.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        beforeWrite.image = g_sparseActiveMaskImage;
        beforeWrite.subresourceRange = range;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &beforeWrite);
    }

    SparseActiveMaskPush push{};
    push.width = renderWidth;
    push.height = renderHeight;
    push.frameIndex = static_cast<uint32_t>(g_sparseEffectiveFrameIndex);
    push.maskWidth = (renderWidth + 3u) / 4u;
    push.samplingRate = g_sparseEffectiveRate;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_sparseActiveMaskPipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_sparseActiveMaskPipelineLayout,
        0u, 1u, &g_sparseActiveMaskSet, 0u, nullptr);
    vkCmdPushConstants(
        cmd,
        g_sparseActiveMaskPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0u,
        sizeof(push),
        &push);

    vkCmdDispatch(
        cmd,
        (push.maskWidth + 7u) / 8u,
        (g_sparseActiveMaskHeight + 7u) / 8u,
        1u);

    VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toRead.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = g_sparseActiveMaskImage;
    toRead.subresourceRange = range;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &toRead);
}

// RT 主 set 的 73/74/75（raygen 写）。
void updateGlassAlphaBlendRayDescriptors()
{
    if(!g_glassBlueNoiseReady)
    {
        createGlassBlueNoiseBuffer();
    }

    if(g_rayTracingDescriptorSet != VK_NULL_HANDLE &&
       g_glassBlueNoiseBuffer != VK_NULL_HANDLE)
    {
        VkDescriptorBufferInfo sparseBlueNoiseInfo{};
        sparseBlueNoiseInfo.buffer = g_glassBlueNoiseBuffer;
        sparseBlueNoiseInfo.offset = 0u;
        sparseBlueNoiseInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet sparseBlueNoiseWrite{};
        sparseBlueNoiseWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sparseBlueNoiseWrite.dstSet = g_rayTracingDescriptorSet;
        sparseBlueNoiseWrite.dstBinding = 76u;
        sparseBlueNoiseWrite.descriptorCount = 1u;
        sparseBlueNoiseWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sparseBlueNoiseWrite.pBufferInfo = &sparseBlueNoiseInfo;

        vkUpdateDescriptorSets(g_device, 1u, &sparseBlueNoiseWrite, 0u, nullptr);
    }

    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_alphaBlendGbufferView == VK_NULL_HANDLE ||
       g_alphaBlendMetaView == VK_NULL_HANDLE ||
       g_alphaBlendLightView == VK_NULL_HANDLE ||
       g_alphaBlendVolumeView == VK_NULL_HANDLE ||
       g_glassBlueNoiseBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo infos[4]{};
    infos[0].imageView = g_alphaBlendGbufferView;
    infos[1].imageView = g_alphaBlendMetaView;
    infos[2].imageView = g_alphaBlendLightView;
    infos[3].imageView = g_alphaBlendVolumeView;

    for(uint32_t i = 0u; i < 4u; ++i)
    {
        infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    const uint32_t bindings[4] = { 73u, 74u, 75u, 77u };
    VkWriteDescriptorSet writes[5]{};

    for(uint32_t i = 0u; i < 4u; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding = bindings[i];
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }

    VkDescriptorBufferInfo blueNoiseInfo{};
    blueNoiseInfo.buffer = g_glassBlueNoiseBuffer;
    blueNoiseInfo.offset = 0u;
    blueNoiseInfo.range = VK_WHOLE_SIZE;

    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = g_rayTracingDescriptorSet;
    writes[4].dstBinding = 76u;
    writes[4].descriptorCount = 1u;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &blueNoiseInfo;

    vkUpdateDescriptorSets(g_device, 5u, writes, 0u, nullptr);

    g_glassAlphaBlendBoundSet = g_rayTracingDescriptorSet;
    g_glassAlphaBlendBoundRayGbuffer = g_alphaBlendGbufferView;
}

// 新 pass 自己的 set（8 张 storage image，独立于 RT 主 set）。
void updateGlassAlphaBlendPassDescriptors()
{
    if(g_glassAlphaBlendSet == VK_NULL_HANDLE ||
       g_rtOutputView == VK_NULL_HANDLE ||
       g_alphaBlendGbufferView == VK_NULL_HANDLE ||
       g_alphaBlendMetaView == VK_NULL_HANDLE ||
       g_alphaBlendLightView == VK_NULL_HANDLE ||
       g_alphaBlendRadianceView == VK_NULL_HANDLE ||
       g_dlssDepthView == VK_NULL_HANDLE ||
       g_rrNormalRoughnessView == VK_NULL_HANDLE ||
       g_rrDiffuseAlbedoView == VK_NULL_HANDLE ||
       g_cameraUboBuffer == VK_NULL_HANDLE ||
       g_alphaBlendVolumeView == VK_NULL_HANDLE ||
       g_alphaBlendPlaceholder3DView == VK_NULL_HANDLE ||
       g_glassBlueNoiseBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo infos[8]{};
    infos[0].imageView = g_rtOutputView;
    infos[1].imageView = g_alphaBlendGbufferView;
    infos[2].imageView = g_alphaBlendMetaView;
    infos[3].imageView = g_alphaBlendLightView;
    infos[4].imageView = g_alphaBlendRadianceView;
    infos[5].imageView = g_dlssDepthView;
    infos[6].imageView = g_rrNormalRoughnessView;
    infos[7].imageView = g_rrDiffuseAlbedoView;

    for(uint32_t i = 0u; i < 8u; ++i)
    {
        infos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkWriteDescriptorSet writes[16]{};

    for(uint32_t i = 0u; i < 8u; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_glassAlphaBlendSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &infos[i];
    }

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.offset = 0u;
    cameraInfo.range = VK_WHOLE_SIZE;

    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = g_glassAlphaBlendSet;
    writes[8].dstBinding = 8u;
    writes[8].descriptorCount = 1u;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[8].pBufferInfo = &cameraInfo;

    VkDescriptorBufferInfo blueNoiseInfo{};
    blueNoiseInfo.buffer = g_glassBlueNoiseBuffer;
    blueNoiseInfo.offset = 0u;
    blueNoiseInfo.range = VK_WHOLE_SIZE;

    writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[9].dstSet = g_glassAlphaBlendSet;
    writes[9].dstBinding = 9u;
    writes[9].descriptorCount = 1u;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[9].pBufferInfo = &blueNoiseInfo;

    VkDescriptorImageInfo volumeInfo{};
    volumeInfo.imageView = g_alphaBlendVolumeView;
    volumeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[10].dstSet = g_glassAlphaBlendSet;
    writes[10].dstBinding = 10u;
    writes[10].descriptorCount = 1u;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[10].pImageInfo = &volumeInfo;

    const bool shVolumeAvailable =
        g_volShYView[0] != VK_NULL_HANDLE && g_volShCoCgView[0] != VK_NULL_HANDLE;

    VkDescriptorImageInfo shYInfo{};
    shYInfo.imageView = shVolumeAvailable
        ? g_volShYView[0]
        : g_alphaBlendPlaceholder3DView;
    shYInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo shCoCgInfo{};
    shCoCgInfo.imageView = shVolumeAvailable
        ? g_volShCoCgView[0]
        : g_alphaBlendPlaceholder3DView;
    shCoCgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[11].dstSet = g_glassAlphaBlendSet;
    writes[11].dstBinding = 11u;
    writes[11].descriptorCount = 1u;
    writes[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[11].pImageInfo = &shYInfo;

    writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[12].dstSet = g_glassAlphaBlendSet;
    writes[12].dstBinding = 12u;
    writes[12].descriptorCount = 1u;
    writes[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[12].pImageInfo = &shCoCgInfo;

    VkDescriptorImageInfo diagRadianceInfo{};
    diagRadianceInfo.imageView =
        g_volRadianceView[0] != VK_NULL_HANDLE ? g_volRadianceView[0]
                                              : g_alphaBlendPlaceholder3DView;
    diagRadianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo diagAgeInfo{};
    diagAgeInfo.imageView =
        g_volAgeView[0] != VK_NULL_HANDLE ? g_volAgeView[0]
                                          : g_alphaBlendPlaceholderR8View;
    diagAgeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo diagReservoirInfo{};
    diagReservoirInfo.imageView =
        g_volReservoirView[0] != VK_NULL_HANDLE ? g_volReservoirView[0]
                                               : g_alphaBlendPlaceholderUintView;
    diagReservoirInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    writes[13].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[13].dstSet = g_glassAlphaBlendSet;
    writes[13].dstBinding = 13u;
    writes[13].descriptorCount = 1u;
    writes[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[13].pImageInfo = &diagRadianceInfo;

    writes[14].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[14].dstSet = g_glassAlphaBlendSet;
    writes[14].dstBinding = 14u;
    writes[14].descriptorCount = 1u;
    writes[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[14].pImageInfo = &diagAgeInfo;

    writes[15].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[15].dstSet = g_glassAlphaBlendSet;
    writes[15].dstBinding = 15u;
    writes[15].descriptorCount = 1u;
    writes[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[15].pImageInfo = &diagReservoirInfo;

    vkUpdateDescriptorSets(g_device, 16u, writes, 0u, nullptr);

    g_glassAlphaBlendBoundColor = g_rtOutputView;
    g_glassAlphaBlendBoundGbuffer = g_alphaBlendGbufferView;
    g_glassAlphaBlendBoundMeta = g_alphaBlendMetaView;
    g_glassAlphaBlendBoundLight = g_alphaBlendLightView;
    g_glassAlphaBlendBoundRadiance = g_alphaBlendRadianceView;
    g_glassAlphaBlendBoundDepth = g_dlssDepthView;
    g_glassAlphaBlendBoundNormal = g_rrNormalRoughnessView;
    g_glassAlphaBlendBoundAlbedo = g_rrDiffuseAlbedoView;
    g_glassAlphaBlendBoundShY = shVolumeAvailable
        ? g_volShYView[0]
        : g_alphaBlendPlaceholder3DView;
}

// set layout + pool + pipeline layout + 两个 compute pipeline。
// 失败一次就置 failed，不再重试（照 g_volRestirPipelineFailed 的做法）。
void ensureGlassAlphaBlendPipelines()
{
    if(g_glassAlphaBlendPipeline != VK_NULL_HANDLE &&
       g_glassAlphaFinishPipeline != VK_NULL_HANDLE)
    {
        return;
    }

    if(g_glassAlphaBlendPipelineFailed)
    {
        return;
    }

    try
    {
        if(g_glassAlphaBlendSetLayout == VK_NULL_HANDLE)
        {
            VkDescriptorSetLayoutBinding bindings[16]{};

            for(uint32_t i = 0u; i < 8u; ++i)
            {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                bindings[i].descriptorCount = 1u;
                bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }

            bindings[8].binding = 8u;
            bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[8].descriptorCount = 1u;
            bindings[8].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[9].binding = 9u;
            bindings[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[9].descriptorCount = 1u;
            bindings[9].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[10].binding = 10u;
            bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[10].descriptorCount = 1u;
            bindings[10].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[11].binding = 11u;
            bindings[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[11].descriptorCount = 1u;
            bindings[11].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[12].binding = 12u;
            bindings[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[12].descriptorCount = 1u;
            bindings[12].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[13].binding = 13u;
            bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[13].descriptorCount = 1u;
            bindings[13].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[14].binding = 14u;
            bindings[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[14].descriptorCount = 1u;
            bindings[14].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            bindings[15].binding = 15u;
            bindings[15].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[15].descriptorCount = 1u;
            bindings[15].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
            };
            layoutInfo.bindingCount = 16u;
            layoutInfo.pBindings = bindings;

            check(
                vkCreateDescriptorSetLayout(
                    g_device, &layoutInfo, nullptr, &g_glassAlphaBlendSetLayout),
                "glass alpha blend set layout failed");
        }

        if(g_glassAlphaBlendPool == VK_NULL_HANDLE)
        {
            VkDescriptorPoolSize poolSizes[3] = {
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 14u },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
                { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u }
            };
            VkDescriptorPoolCreateInfo poolInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
            };
            poolInfo.maxSets = 1u;
            poolInfo.poolSizeCount = 3u;
            poolInfo.pPoolSizes = poolSizes;

            check(
                vkCreateDescriptorPool(
                    g_device, &poolInfo, nullptr, &g_glassAlphaBlendPool),
                "glass alpha blend descriptor pool failed");
        }

        if(g_glassAlphaBlendSet == VK_NULL_HANDLE)
        {
            VkDescriptorSetAllocateInfo allocateInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
            };
            allocateInfo.descriptorPool = g_glassAlphaBlendPool;
            allocateInfo.descriptorSetCount = 1u;
            allocateInfo.pSetLayouts = &g_glassAlphaBlendSetLayout;

            check(
                vkAllocateDescriptorSets(
                    g_device, &allocateInfo, &g_glassAlphaBlendSet),
                "glass alpha blend descriptor set failed");
        }

        if(g_glassAlphaBlendPipelineLayout == VK_NULL_HANDLE)
        {
            VkPushConstantRange pushRange{};
            pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pushRange.offset = 0u;
            pushRange.size = sizeof(GlassAlphaBlendPush);

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
            };
            pipelineLayoutInfo.setLayoutCount = 1u;
            pipelineLayoutInfo.pSetLayouts = &g_glassAlphaBlendSetLayout;
            pipelineLayoutInfo.pushConstantRangeCount = 1u;
            pipelineLayoutInfo.pPushConstantRanges = &pushRange;

            check(
                vkCreatePipelineLayout(
                    g_device, &pipelineLayoutInfo, nullptr,
                    &g_glassAlphaBlendPipelineLayout),
                "glass alpha blend pipeline layout failed");
        }

        const struct
        {
            const char* path;
            VkPipeline* pipeline;
        } stages[3] =
        {
            { "shaders/glass_alpha_blend.comp.spv", &g_glassAlphaBlendPipeline },
            { "shaders/glass_alpha_finish.comp.spv", &g_glassAlphaFinishPipeline },
            { "shaders/vol_diag.comp.spv", &g_glassAlphaDiagPipeline }
        };

        for(uint32_t i = 0u; i < 3u; ++i)
        {
            if(*stages[i].pipeline != VK_NULL_HANDLE)
            {
                continue;
            }

            std::vector<uint8_t> stageCode = loadSpv(stages[i].path);

            RTLog(
                "[GLASS ALPHA BLEND] loaded " + std::string(stages[i].path) +
                " bytes=" + std::to_string(stageCode.size()));

            VkShaderModule module =
                createShaderModule(g_device, stageCode);

            VkPipelineShaderStageCreateInfo stage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
            };
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = module;
            stage.pName = "main";

            VkComputePipelineCreateInfo pipelineInfo{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
            };
            pipelineInfo.stage = stage;
            pipelineInfo.layout = g_glassAlphaBlendPipelineLayout;

            const VkResult result = vkCreateComputePipelines(
                g_device, getSharedPipelineCache(), 1u, &pipelineInfo, nullptr,
                stages[i].pipeline);

            vkDestroyShaderModule(g_device, module, nullptr);

            check(result, "glass alpha blend pipeline failed");
        }

        RTLog("[GLASS ALPHA BLEND] composite + finish pipelines ready");
    }
    catch(const std::exception& error)
    {
        g_glassAlphaBlendPipelineFailed = true;
        RTLog(std::string("[GLASS ALPHA BLEND] unavailable: ") + error.what());
    }
}

// 每帧建图（渲染分辨率）。alreadyValid 判据照 createRRGBufferImages：
// 句柄齐全且尺寸一致就直接返回；否则等设备空闲后整张重建。
void createGlassAlphaBlendImages(uint32_t width, uint32_t height)
{
    if(g_alphaBlendGbufferImage != VK_NULL_HANDLE &&
       g_alphaBlendMetaImage != VK_NULL_HANDLE &&
       g_alphaBlendLightImage != VK_NULL_HANDLE &&
       g_alphaBlendVolumeImage != VK_NULL_HANDLE &&
       g_alphaBlendRadianceImage != VK_NULL_HANDLE &&
       g_alphaBlendWidth == width &&
       g_alphaBlendHeight == height)
    {
        return;
    }

    if(g_alphaBlendGbufferImage != VK_NULL_HANDLE)
    {
        check(
            FGDeviceWaitIdle(g_device),
            "wait device before glass alpha blend recreate failed");
    }

    destroyHistoryImage(
        g_alphaBlendGbufferImage,
        g_alphaBlendGbufferMemory,
        g_alphaBlendGbufferView,
        g_alphaBlendGbufferLayout);

    destroyHistoryImage(
        g_alphaBlendMetaImage,
        g_alphaBlendMetaMemory,
        g_alphaBlendMetaView,
        g_alphaBlendMetaLayout);

    destroyHistoryImage(
        g_alphaBlendLightImage,
        g_alphaBlendLightMemory,
        g_alphaBlendLightView,
        g_alphaBlendLightLayout);

    destroyHistoryImage(
        g_alphaBlendVolumeImage,
        g_alphaBlendVolumeMemory,
        g_alphaBlendVolumeView,
        g_alphaBlendVolumeLayout);

    destroyHistoryImage(
        g_alphaBlendRadianceImage,
        g_alphaBlendRadianceMemory,
        g_alphaBlendRadianceView,
        g_alphaBlendRadianceLayout);

    createRRGuideImage(
        width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
        g_alphaBlendGbufferImage,
        g_alphaBlendGbufferMemory,
        g_alphaBlendGbufferView,
        g_alphaBlendGbufferLayout);

    createRRGuideImage(
        width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
        g_alphaBlendMetaImage,
        g_alphaBlendMetaMemory,
        g_alphaBlendMetaView,
        g_alphaBlendMetaLayout);

    createRRGuideImage(
        width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
        g_alphaBlendLightImage,
        g_alphaBlendLightMemory,
        g_alphaBlendLightView,
        g_alphaBlendLightLayout);

    createPlaceholder3DImage(VK_FORMAT_R8_UNORM,
        g_alphaBlendPlaceholderR8Image, g_alphaBlendPlaceholderR8Memory,
        g_alphaBlendPlaceholderR8View);
    createPlaceholder3DImage(VK_FORMAT_R32G32B32A32_UINT,
        g_alphaBlendPlaceholderUintImage, g_alphaBlendPlaceholderUintMemory,
        g_alphaBlendPlaceholderUintView);

    if(g_alphaBlendPlaceholder3DImage == VK_NULL_HANDLE)
    {
        VkImageCreateInfo placeholderInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        placeholderInfo.imageType = VK_IMAGE_TYPE_3D;
        placeholderInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        placeholderInfo.extent = { 1u, 1u, 1u };
        placeholderInfo.mipLevels = 1u;
        placeholderInfo.arrayLayers = 1u;
        placeholderInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        placeholderInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        placeholderInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        placeholderInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        check(vkCreateImage(g_device, &placeholderInfo, nullptr,
            &g_alphaBlendPlaceholder3DImage), "placeholder 3D image failed");

        VkMemoryRequirements placeholderRequirements{};
        vkGetImageMemoryRequirements(g_device, g_alphaBlendPlaceholder3DImage,
            &placeholderRequirements);

        VkMemoryAllocateInfo placeholderAllocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        placeholderAllocate.allocationSize = placeholderRequirements.size;
        placeholderAllocate.memoryTypeIndex = findMemoryType(
            g_physicalDevice, placeholderRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        check(vkAllocateMemory(g_device, &placeholderAllocate, nullptr,
            &g_alphaBlendPlaceholder3DMemory), "placeholder 3D memory failed");
        check(vkBindImageMemory(g_device, g_alphaBlendPlaceholder3DImage,
            g_alphaBlendPlaceholder3DMemory, 0u), "placeholder 3D bind failed");

        VkImageViewCreateInfo placeholderView{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        placeholderView.image = g_alphaBlendPlaceholder3DImage;
        placeholderView.viewType = VK_IMAGE_VIEW_TYPE_3D;
        placeholderView.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        placeholderView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        placeholderView.subresourceRange.levelCount = 1u;
        placeholderView.subresourceRange.layerCount = 1u;

        check(vkCreateImageView(g_device, &placeholderView, nullptr,
            &g_alphaBlendPlaceholder3DView), "placeholder 3D view failed");
    }

    createRRGuideImage(
        width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
        g_alphaBlendVolumeImage,
        g_alphaBlendVolumeMemory,
        g_alphaBlendVolumeView,
        g_alphaBlendVolumeLayout);

    createRRGuideImage(
        width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
        g_alphaBlendRadianceImage,
        g_alphaBlendRadianceMemory,
        g_alphaBlendRadianceView,
        g_alphaBlendRadianceLayout);

    g_alphaBlendWidth = width;
    g_alphaBlendHeight = height;

    g_glassAlphaBlendBoundSet = VK_NULL_HANDLE;
    g_glassAlphaBlendBoundGbuffer = VK_NULL_HANDLE;

    updateGlassAlphaBlendRayDescriptors();
    updateGlassAlphaBlendPassDescriptors();

    std::cerr << "[GLASS ALPHA BLEND] images created "
              << width << "x" << height << std::endl;
}

// trace 之前：把图转到 GENERAL，并把 74 号图清 0。
//
// 清 0 是**正确性**要求，不是优化：74 号图的 a 通道是"这一格本帧有没有半透明面"
// 的唯一判据。rgen 只给真正的半透明像素写它，剩下的必须读成 0，否则上一帧的
// 玻璃像素在这一帧被当成玻璃重新合成一遍（画面里会留下随相机移动的鬼影矩形）。
// 只清这一张（另一张 radiance 每帧被全量覆盖到有半透明像素的格子）。
void recordGlassAlphaBlendPrepare(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE ||
       g_alphaBlendMetaImage == VK_NULL_HANDLE)
    {
        return;
    }

    prepareGlassAlphaBlendImages(cmd);

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0u;
    range.levelCount = 1u;
    range.baseArrayLayer = 0u;
    range.layerCount = 1u;

    VkClearColorValue clearColor{};
    clearColor.float32[0] = 1.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;

    vkCmdClearColorImage(
        cmd,
        g_alphaBlendMetaImage,
        VK_IMAGE_LAYOUT_GENERAL,
        &clearColor,
        1u,
        &range);

    VkImageMemoryBarrier clearBarrier{};
    clearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    clearBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    clearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    clearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    clearBarrier.image = g_alphaBlendMetaImage;
    clearBarrier.subresourceRange = range;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u,
        0u, nullptr,
        0u, nullptr,
        1u, &clearBarrier);
}

// trace 之后：[2] 邻居搜索 + 滤波 + 合成 -> radiance；[3] 写回 rtOutput。
// 两个 dispatch 之间必须有一次 compute->compute 屏障：finish 读 radiance，
// 而 radiance 是 blend 写的（跨 invocation，不能靠"同像素自己读自己"糊过去）。
void recordGlassAlphaBlend(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height)
{
    if(cmd == VK_NULL_HANDLE ||
       g_alphaBlendRadianceImage == VK_NULL_HANDLE ||
       g_rtOutputImage == VK_NULL_HANDLE ||
       g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    if(!g_glassBlueNoiseReady)
    {
        createGlassBlueNoiseBuffer();
    }

    ensureGlassAlphaBlendPipelines();

    if(g_glassAlphaBlendPipeline == VK_NULL_HANDLE ||
       g_glassAlphaFinishPipeline == VK_NULL_HANDLE ||
       g_glassAlphaBlendSet == VK_NULL_HANDLE ||
       g_glassAlphaBlendPipelineLayout == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_glassAlphaBlendBoundRayGbuffer != g_alphaBlendGbufferView ||
       g_glassAlphaBlendBoundSet != g_rayTracingDescriptorSet)
    {
        updateGlassAlphaBlendRayDescriptors();
    }

    if(g_glassAlphaBlendBoundColor != g_rtOutputView ||
       g_glassAlphaBlendBoundGbuffer != g_alphaBlendGbufferView ||
       g_glassAlphaBlendBoundMeta != g_alphaBlendMetaView ||
       g_glassAlphaBlendBoundLight != g_alphaBlendLightView ||
       g_glassAlphaBlendBoundRadiance != g_alphaBlendRadianceView ||
       g_glassAlphaBlendBoundDepth != g_dlssDepthView ||
       g_glassAlphaBlendBoundNormal != g_rrNormalRoughnessView ||
       g_glassAlphaBlendBoundAlbedo != g_rrDiffuseAlbedoView ||
       g_glassAlphaBlendBoundShY != (g_volShYView[0] != VK_NULL_HANDLE
            ? g_volShYView[0]
            : g_alphaBlendPlaceholder3DView))
    {
        updateGlassAlphaBlendPassDescriptors();
    }

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0u;
    range.levelCount = 1u;
    range.baseArrayLayer = 0u;
    range.layerCount = 1u;

    {
        VkImageMemoryBarrier barriers[8]{};

        VkImage barrierImages[8] =
        {
            g_alphaBlendGbufferImage,
            g_alphaBlendMetaImage,
            g_alphaBlendLightImage,
            g_rtOutputImage,
            g_alphaBlendRadianceImage,
            g_dlssDepthImage,
            g_rrNormalRoughnessImage,
            g_rrDiffuseAlbedoImage
        };

        for(uint32_t i = 0u; i < 8u; ++i)
        {
            barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].image = barrierImages[i];
            barriers[i].subresourceRange = range;
        }

        barriers[4].srcAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[4].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr,
            0u, nullptr,
            8u, barriers);
    }

    GlassAlphaBlendPush push{};
    push.extentX = width;
    push.extentY = height;
    push.neighborIterations = 6u;
    push.frameIndex = g_glassAlphaBlendFrameIndex++;
    push.neighborRadius = 10.0f;
    push.neighborExpand = 1.6f;
    push.normalThreshold = 0.9f;
    push.depthThreshold = 0.1f;
    push.coplanarThreshold = 0.2f;
    push.shMaxDistance = VOL_RESTIR_MAX_DISTANCE;
    push.shDepthExponent = VOL_RESTIR_DEPTH_EXPONENT;
    push.shValid =
        (g_volShYView[0] != VK_NULL_HANDLE && g_volShCoCgView[0] != VK_NULL_HANDLE)
            ? 1.0f : 0.0f;
    push.neighborSearch = glassAlphaBlendNeighborSearchEnabled() ? 1u : 0u;
    push.enableFilter = glassAlphaBlendRadianceVolumeEnabled() ? 1u : 0u;
    push.pad0 = 1u;
    push.pad1 = 1u;
    push.shFroxelW = g_volFroxelW;
    push.shFroxelH = g_volFroxelH;
    push.shFroxelDepth = VOL_RESTIR_DEPTH;
    {
        const char* raw = std::getenv("ORION_GLASS_ALPHA_BLEND_DBG");
        const uint32_t glassDbg = (raw != nullptr && raw[0] != '\0')
            ? static_cast<uint32_t>(std::atoi(raw)) : 0u;

        static const uint32_t markerDbg = [] {
            static const uint32_t candidates[] = {1u, 6u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};

            for(uint32_t value : candidates)
            {
                const std::string name =
                    "glass_dbg_" + std::to_string(value) + ".flag";
                std::ifstream marker(ResolveRTXRuntimePath(name.c_str()));

                if(marker.good())
                {
                    RTLog("[GLASS DBG] 标记文件 " + name +
                          " => dbgMode=" + std::to_string(value));
                    return value;
                }
            }

            return 0u;
        }();

        const uint32_t diagMode = volDiagMode();

        if(glassDbg != 0u)
        {
            push.dbgMode = glassDbg;
        }
        else if(markerDbg != 0u)
        {
            push.dbgMode = markerDbg;
        }
        else
        {
            push.dbgMode = diagMode != 0u ? 1u + diagMode : 0u;
        }
    }

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_glassAlphaBlendPipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_glassAlphaBlendPipelineLayout,
        0u,
        1u,
        &g_glassAlphaBlendSet,
        0u,
        nullptr);
    vkCmdPushConstants(
        cmd,
        g_glassAlphaBlendPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0u,
        sizeof(push),
        &push);
    vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1u);

    {
        VkImageMemoryBarrier radianceBarrier{};
        radianceBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        radianceBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        radianceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        radianceBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        radianceBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        radianceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        radianceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        radianceBarrier.image = g_alphaBlendRadianceImage;
        radianceBarrier.subresourceRange = range;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr,
            0u, nullptr,
            1u, &radianceBarrier);
    }

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_glassAlphaFinishPipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_glassAlphaBlendPipelineLayout,
        0u,
        1u,
        &g_glassAlphaBlendSet,
        0u,
        nullptr);
    vkCmdPushConstants(
        cmd,
        g_glassAlphaBlendPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0u,
        sizeof(push),
        &push);
    vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1u);

    if(g_glassAlphaDiagPipeline != VK_NULL_HANDLE &&
       push.dbgMode >= 2u && push.dbgMode <= 5u)
    {
        VkImageMemoryBarrier diagBarrier{};
        diagBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        diagBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        diagBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        diagBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        diagBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        diagBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        diagBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        diagBarrier.image = g_rtOutputImage;
        diagBarrier.subresourceRange = range;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
            0u, nullptr, 0u, nullptr, 1u, &diagBarrier);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_glassAlphaDiagPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_glassAlphaBlendPipelineLayout, 0u, 1u, &g_glassAlphaBlendSet, 0u, nullptr);
        vkCmdPushConstants(cmd, g_glassAlphaBlendPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(push), &push);
        vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1u);
    }

    {
        VkImageMemoryBarrier finishBarrier{};
        finishBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        finishBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        finishBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        finishBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        finishBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        finishBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        finishBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        finishBarrier.image = g_rtOutputImage;
        finishBarrier.subresourceRange = range;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0u,
            0u, nullptr,
            0u, nullptr,
            1u, &finishBarrier);
    }
}
