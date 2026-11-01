// Orion backend — Water 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

void updateWaterSimulationRayDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_waterSimView == VK_NULL_HANDLE ||
       g_waterSimSampler == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo info{};
    info.sampler = g_waterSimSampler;
    info.imageView = g_waterSimView;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 33;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
}
void updateWaterCausticRayDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_waterCausticView == VK_NULL_HANDLE ||
       g_waterCausticSampler == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo info{};
    info.sampler = g_waterCausticSampler;
    info.imageView = g_waterCausticView;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 34;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
}
void updateWaterDetailNormalRayDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_waterDetailNormalView == VK_NULL_HANDLE ||
       g_waterDetailNormalSampler == VK_NULL_HANDLE)
    {
        return;
    }
    VkDescriptorImageInfo info{};
    info.sampler = g_waterDetailNormalSampler;
    info.imageView = g_waterDetailNormalView;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 35;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
}

// A compact, tileable tangent-space normal map is generated once on the CPU.
// It replaces the unstable analytical shading slope, while the physical macro
// wave and the player simulation remain in their existing GPU paths.
void createWaterDetailNormalResources()
{
    if constexpr(!kWATER_DETAIL_NORMAL_ENABLED)
    {
        RTLog("[WATER NORMAL] 已停用（binding 35 无活跃着色器采样）：跳过创建，省 0.35 MB");
        return;
    }

    if(g_waterDetailNormalImage != VK_NULL_HANDLE)
    {
        return;
    }

    std::vector<std::vector<uint8_t>> mipPixels(WATER_DETAIL_NORMAL_MIP_LEVELS);
    uint32_t extent = WATER_DETAIL_NORMAL_RESOLUTION;
    mipPixels[0].resize(static_cast<size_t>(extent) * extent * 4u);
    for(uint32_t y = 0; y < extent; ++y)
    for(uint32_t x = 0; x < extent; ++x)
    {
        const float u = 6.28318530718f * (static_cast<float>(x) + 0.5f) / extent;
        const float v = 6.28318530718f * (static_cast<float>(y) + 0.5f) / extent;
        const float dx = 0.72f * std::cos(u * 5.0f + v * 2.0f) +
                         0.34f * std::sin(u * 9.0f - v * 7.0f) +
                         0.18f * std::cos(u * 15.0f + v * 11.0f);
        const float dy = 0.66f * std::sin(v * 6.0f - u * 3.0f) +
                         0.31f * std::cos(v * 10.0f + u * 8.0f) +
                         0.17f * std::sin(v * 17.0f - u * 13.0f);
        const float length = std::sqrt(dx * dx + dy * dy + 1.0f);
        const size_t index = (static_cast<size_t>(y) * extent + x) * 4u;
        mipPixels[0][index + 0] = static_cast<uint8_t>((dx / length * 0.5f + 0.5f) * 255.0f);
        mipPixels[0][index + 1] = static_cast<uint8_t>((dy / length * 0.5f + 0.5f) * 255.0f);
        mipPixels[0][index + 2] = static_cast<uint8_t>((1.0f / length * 0.5f + 0.5f) * 255.0f);
        mipPixels[0][index + 3] = 255u;
    }
    for(uint32_t level = 1u; level < WATER_DETAIL_NORMAL_MIP_LEVELS; ++level)
    {
        const uint32_t previousExtent = std::max(1u, WATER_DETAIL_NORMAL_RESOLUTION >> (level - 1u));
        const uint32_t currentExtent = std::max(1u, WATER_DETAIL_NORMAL_RESOLUTION >> level);
        mipPixels[level].resize(static_cast<size_t>(currentExtent) * currentExtent * 4u);
        for(uint32_t y = 0; y < currentExtent; ++y)
        for(uint32_t x = 0; x < currentExtent; ++x)
        {
            float nx = 0.0f, ny = 0.0f, nz = 0.0f;
            for(uint32_t oy = 0; oy < 2u; ++oy)
            for(uint32_t ox = 0; ox < 2u; ++ox)
            {
                const size_t src = (static_cast<size_t>((y * 2u + oy) % previousExtent) * previousExtent +
                                    ((x * 2u + ox) % previousExtent)) * 4u;
                nx += static_cast<float>(mipPixels[level - 1u][src + 0]) / 255.0f * 2.0f - 1.0f;
                ny += static_cast<float>(mipPixels[level - 1u][src + 1]) / 255.0f * 2.0f - 1.0f;
                nz += static_cast<float>(mipPixels[level - 1u][src + 2]) / 255.0f * 2.0f - 1.0f;
            }
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            const size_t dst = (static_cast<size_t>(y) * currentExtent + x) * 4u;
            mipPixels[level][dst + 0] = static_cast<uint8_t>((nx / length * 0.5f + 0.5f) * 255.0f);
            mipPixels[level][dst + 1] = static_cast<uint8_t>((ny / length * 0.5f + 0.5f) * 255.0f);
            mipPixels[level][dst + 2] = static_cast<uint8_t>((nz / length * 0.5f + 0.5f) * 255.0f);
            mipPixels[level][dst + 3] = 255u;
        }
    }
    VkDeviceSize totalBytes = 0u;
    std::vector<VkDeviceSize> offsets(WATER_DETAIL_NORMAL_MIP_LEVELS);
    for(uint32_t level = 0u; level < WATER_DETAIL_NORMAL_MIP_LEVELS; ++level)
    {
        offsets[level] = totalBytes;
        totalBytes += mipPixels[level].size();
    }
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(g_device, g_physicalDevice, totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* mapped = nullptr;
    check(vkMapMemory(g_device, stagingMemory, 0u, totalBytes, 0u, &mapped), "water normal map staging map failed");
    for(uint32_t level = 0u; level < WATER_DETAIL_NORMAL_MIP_LEVELS; ++level)
        std::memcpy(static_cast<uint8_t*>(mapped) + offsets[level], mipPixels[level].data(), mipPixels[level].size());
    vkUnmapMemory(g_device, stagingMemory);

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {WATER_DETAIL_NORMAL_RESOLUTION, WATER_DETAIL_NORMAL_RESOLUTION, 1u};
    imageInfo.mipLevels = WATER_DETAIL_NORMAL_MIP_LEVELS;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_waterDetailNormalImage), "water normal map create image failed");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_waterDetailNormalImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_waterDetailNormalMemory), "water normal map allocate failed");
    check(vkBindImageMemory(g_device, g_waterDetailNormalImage, g_waterDetailNormalMemory, 0u), "water normal map bind failed");

    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = g_rtCommandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &commandAllocation, &commandBuffer), "water normal map command allocation failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "water normal map command begin failed");
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_waterDetailNormalImage; barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = WATER_DETAIL_NORMAL_MIP_LEVELS; barrier.subresourceRange.layerCount = 1u;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    std::vector<VkBufferImageCopy> copies(WATER_DETAIL_NORMAL_MIP_LEVELS);
    for(uint32_t level = 0u; level < WATER_DETAIL_NORMAL_MIP_LEVELS; ++level) {
        copies[level].bufferOffset = offsets[level]; copies[level].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[level].imageSubresource.mipLevel = level; copies[level].imageSubresource.layerCount = 1u;
        const uint32_t levelExtent = std::max(1u, WATER_DETAIL_NORMAL_RESOLUTION >> level);
        copies[level].imageExtent = {levelExtent, levelExtent, 1u};
    }
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_waterDetailNormalImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        WATER_DETAIL_NORMAL_MIP_LEVELS, copies.data());
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    check(vkEndCommandBuffer(commandBuffer), "water normal map command end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount = 1u; submit.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "water normal map submit failed");
    check(vkQueueWaitIdle(g_queue), "water normal map upload wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    vkDestroyBuffer(g_device, stagingBuffer, nullptr); vkFreeMemory(g_device, stagingMemory, nullptr);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_waterDetailNormalImage; viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; viewInfo.subresourceRange.levelCount = WATER_DETAIL_NORMAL_MIP_LEVELS; viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_waterDetailNormalView), "water normal map view failed");
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR; samplerInfo.minFilter = VK_FILTER_LINEAR; samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT; samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT; samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.minLod = 0.0f; samplerInfo.maxLod = static_cast<float>(WATER_DETAIL_NORMAL_MIP_LEVELS - 1u);
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_waterDetailNormalSampler), "water normal map sampler failed");
    updateWaterDetailNormalRayDescriptor();
}
void destroyWaterDetailNormalResources()
{
    if(g_waterDetailNormalSampler) vkDestroySampler(g_device, g_waterDetailNormalSampler, nullptr);
    if(g_waterDetailNormalView) vkDestroyImageView(g_device, g_waterDetailNormalView, nullptr);
    if(g_waterDetailNormalImage) vkDestroyImage(g_device, g_waterDetailNormalImage, nullptr);
    if(g_waterDetailNormalMemory) vkFreeMemory(g_device, g_waterDetailNormalMemory, nullptr);
    g_waterDetailNormalSampler = VK_NULL_HANDLE; g_waterDetailNormalView = VK_NULL_HANDLE;
    g_waterDetailNormalImage = VK_NULL_HANDLE; g_waterDetailNormalMemory = VK_NULL_HANDLE;
}
void createWaterCausticResources()
{
    if(g_waterCausticImage != VK_NULL_HANDLE)
    {
        return;
    }

    createWaterSimulationResources();

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(
        g_physicalDevice,
        VK_FORMAT_R16G16_SFLOAT,
        &formatProperties
    );
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) !=
       requiredFeatures)
    {
        throw std::runtime_error(
            "VK_FORMAT_R16G16_SFLOAT cannot be sampled and stored for water caustics"
        );
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16_SFLOAT;
    imageInfo.extent = {
        WATER_CAUSTIC_RESOLUTION,
        WATER_CAUSTIC_RESOLUTION,
        1u
    };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(
        vkCreateImage(g_device, &imageInfo, nullptr, &g_waterCausticImage),
        "water caustic create image failed"
    );

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_waterCausticImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(
        g_physicalDevice,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );
    check(
        vkAllocateMemory(g_device, &allocation, nullptr, &g_waterCausticMemory),
        "water caustic allocate image memory failed"
    );
    check(
        vkBindImageMemory(g_device, g_waterCausticImage, g_waterCausticMemory, 0),
        "water caustic bind image memory failed"
    );

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_waterCausticImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0u;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.baseArrayLayer = 0u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(
        vkCreateImageView(g_device, &viewInfo, nullptr, &g_waterCausticView),
        "water caustic create image view failed"
    );

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    check(
        vkCreateSampler(g_device, &samplerInfo, nullptr, &g_waterCausticSampler),
        "water caustic create sampler failed"
    );

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0u;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1u;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1u;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1u;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2u;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1u;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 3u;
    layoutInfo.pBindings = bindings;
    check(
        vkCreateDescriptorSetLayout(
            g_device,
            &layoutInfo,
            nullptr,
            &g_waterCausticDescriptorSetLayout
        ),
        "water caustic create descriptor layout failed"
    );

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u }
    };
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(
        vkCreateDescriptorPool(
            g_device,
            &poolInfo,
            nullptr,
            &g_waterCausticDescriptorPool
        ),
        "water caustic create descriptor pool failed"
    );

    VkDescriptorSetAllocateInfo allocationInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    allocationInfo.descriptorPool = g_waterCausticDescriptorPool;
    allocationInfo.descriptorSetCount = 1u;
    allocationInfo.pSetLayouts = &g_waterCausticDescriptorSetLayout;
    check(
        vkAllocateDescriptorSets(
            g_device,
            &allocationInfo,
            &g_waterCausticDescriptorSet
        ),
        "water caustic allocate descriptor set failed"
    );

    VkDescriptorImageInfo stateInfo{};
    stateInfo.sampler = g_waterSimSampler;
    stateInfo.imageView = g_waterSimView;
    stateInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo causticInfo{};
    causticInfo.imageView = g_waterCausticView;
    causticInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);

    VkWriteDescriptorSet writes[3]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = g_waterCausticDescriptorSet;
    writes[0].dstBinding = 0u;
    writes[0].descriptorCount = 1u;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &stateInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = g_waterCausticDescriptorSet;
    writes[1].dstBinding = 1u;
    writes[1].descriptorCount = 1u;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &causticInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = g_waterCausticDescriptorSet;
    writes[2].dstBinding = 2u;
    writes[2].descriptorCount = 1u;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &cameraInfo;
    vkUpdateDescriptorSets(g_device, 3u, writes, 0u, nullptr);

    g_waterCausticLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    updateWaterCausticRayDescriptor();
}
void createWaterCausticPipeline()
{
    if constexpr(!kWATER_CAUSTIC_PASS_ENABLED)
    {
        RTLog("[WATER CAUSTIC] 已停用（pass 短路 + binding 34 无活跃着色器采样）：跳过资源创建，省 0.26 MB");
        return;
    }

    if(g_waterCausticPipeline != VK_NULL_HANDLE)
    {
        return;
    }

    createWaterCausticResources();
    const std::vector<uint8_t> code =
        loadSpv("shaders/water_caustics.comp.spv");
    VkShaderModule shaderModule = createShaderModule(g_device, code);

    try
    {
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        layoutInfo.setLayoutCount = 1u;
        layoutInfo.pSetLayouts = &g_waterCausticDescriptorSetLayout;
        check(
            vkCreatePipelineLayout(
                g_device,
                &layoutInfo,
                nullptr,
                &g_waterCausticPipelineLayout
            ),
            "water caustic create pipeline layout failed"
        );

        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shaderModule;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        pipelineInfo.stage = stage;
        pipelineInfo.layout = g_waterCausticPipelineLayout;
        check(
            vkCreateComputePipelines(
                g_device,
                getSharedPipelineCache(),
                1u,
                &pipelineInfo,
                nullptr,
                &g_waterCausticPipeline
            ),
            "water caustic create pipeline failed"
        );
    }
    catch(...)
    {
        if(g_waterCausticPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(g_device, g_waterCausticPipelineLayout, nullptr);
            g_waterCausticPipelineLayout = VK_NULL_HANDLE;
        }
        vkDestroyShaderModule(g_device, shaderModule, nullptr);
        throw;
    }

    vkDestroyShaderModule(g_device, shaderModule, nullptr);
    RTLog("[WATER CAUSTIC] 256x256 wavefront-area compute pipeline created");
}
// 写入 UBO 尾块 waterFftParams（偏移 1280，8 个 float）。**y = 0 是安全阀**：
// 用户在设置里关掉「海浪」时恒写 0，着色端（Seascape 与 FFT 皆然）直接早退。
// 2026-10-03 方案 B：y 改为 **GUI 口径**（1 = 出厂），FFT 回退路径在宿主侧把
// 视觉倍率乘回 120（见 waterFftDispatch），着色器两分支拿到同一口径。
void populateWaterFftParams(float* params)
{
    if(params == nullptr)
    {
        return;
    }
    const bool enabled = g_waterFftEnabled.load();
    params[0] = g_waterFftTilePeriod.load();
    params[1] = enabled ? g_waterFftAmplitude.load() : 0.0f;
    params[2] = g_waterFftChoppiness.load();
    params[3] = g_waterFftWindSpeed.load() / 7.0f;
    params[4] = g_waterFftWindDirection.load();
    params[5] = static_cast<float>(g_mirrorBounceCount.load());
    params[6] = (enabled && g_waterFftAvailable.load())
        ? g_waterFftSwellGain.load() : 0.0f;
    params[7] = (enabled && g_waterFftAvailable.load() &&
                 g_waterCausticMapAvailable.load()) ? 1.0f : 0.0f;
}

// 把斜率图绑到 RT set 的 binding 59（水面着色消费端）。创建成功后才调用。
void updateWaterFftRayDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_waterFftSlopeView == VK_NULL_HANDLE ||
       g_waterFftSlopeSampler == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo info{};
    info.sampler = g_waterFftSlopeSampler;
    info.imageView = g_waterFftSlopeView;
    info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 59;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    RTLog("[WATER FFT] slope map bound to RT binding 59");
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setWaterFftParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    if(params == nullptr || env->GetArrayLength(params) < 1) return JNI_FALSE;
    const jsize count = std::min<jsize>(env->GetArrayLength(params), 5);
    jfloat values[5] = {1.0f, 1.0f, 1.0f, 10.0f, 1.0f};
    env->GetFloatArrayRegion(params, 0, count, values);
    for(int i = 0; i < count; ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_waterFftEnabled.store(values[0] != 0.0f);
    g_waterFftAmplitude.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 3.0f));
    g_waterFftChoppiness.store(
        std::min(std::max(static_cast<float>(values[2]), 0.0f), 3.0f));
    g_waterFftTilePeriod.store(
        std::min(std::max(static_cast<float>(values[3]), 2.0f), 512.0f));
    g_waterFftDetail.store(
        std::min(std::max(static_cast<float>(values[4]), 0.0f), 3.0f));
    return JNI_TRUE;
}

// 通用：compute → compute 的依赖（FFT 每趟之间都要）
void waterFftPassBarrier(VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u,
        1u, &barrier,
        0u, nullptr,
        0u, nullptr
    );
}

// 一趟调度：mode 0 谱生成 / 1 蝶形 / 2 Y 位反转 / 3 finalize
void waterFftDispatch(
    VkCommandBuffer cmd,
    int32_t sourceLayer,
    int32_t destinationLayer,
    int32_t stageLength,
    int32_t mode,
    int32_t axis,
    int32_t detailSourceLayer ,
    float texelWorldSizeOverride ,
    float detailTexelWorldSize ,
    float detailVisualScale ,
    float minWaveRadius )
{
    const float baseAmplitude = g_waterFftAmplitude.load();
    WaterFftPushConstants push{};
    push.sourceLayer = sourceLayer;
    push.destinationLayer = destinationLayer;
    push.stageLength = stageLength;
    push.mode = mode;
    push.axis = axis;
    push.detailSourceLayer = detailSourceLayer;
    push.timeSeconds = g_waterFftTimeSeconds;
    push.windSpeed = 0.0f;
    push.rainStrength = g_waterWeatherRain.load();
    push.spectrumScale = 0.0f;
    push.visualScale = baseAmplitude;
    push.choppiness = g_waterFftChoppiness.load();
    push.texelWorldSize = texelWorldSizeOverride > 0.0f
        ? texelWorldSizeOverride
        : g_waterFftTilePeriod.load() / float(WATER_FFT_RESOLUTION);
    push.detailTexelWorldSize = detailTexelWorldSize;
    push.detailVisualScale = detailVisualScale;
    push.minWaveRadius = minWaveRadius;

    vkCmdPushConstants(
        cmd,
        g_waterFftPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0u,
        static_cast<uint32_t>(sizeof(push)),
        &push
    );
    const uint32_t groups =
        (WATER_FFT_RESOLUTION + WATER_FFT_LOCAL_SIZE - 1u) / WATER_FFT_LOCAL_SIZE;
    vkCmdDispatch(cmd, groups, groups, 1u);
}

// 描述符写入（两个 storage image）。只在创建时调一次：整条链不变，不需要逐帧
// 重写（飞行中的帧永远看到一致的绑定）。
void updateWaterFftDescriptors()
{
    if(g_waterFftDescriptorSet == VK_NULL_HANDLE ||
       g_waterFftStateView == VK_NULL_HANDLE ||
       g_waterFftSlopeStorageView == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo stateInfo{};
    stateInfo.imageView = g_waterFftStateView;
    stateInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo slopeInfo{};
    slopeInfo.imageView = g_waterFftSlopeStorageView;
    slopeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = g_waterFftDescriptorSet;
    writes[0].dstBinding = 0u;
    writes[0].descriptorCount = 1u;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &stateInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = g_waterFftDescriptorSet;
    writes[1].dstBinding = 1u;
    writes[1].descriptorCount = 1u;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &slopeInfo;
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);
}

// 创建（init 期调用）。**软失败**：任何一步失败都只记日志并保持禁用。
void createWaterFftResources()
{
    if constexpr(!kWATER_FFT_PASSES_ENABLED)
    {
        RTLog("[WATER FFT] 已停用（kWATER_FFT_PASSES_ENABLED=false）：跳过整链创建");
        return;
    }

    if(g_waterFftPipeline != VK_NULL_HANDLE || g_waterFftAvailable.load())
    {
        return;
    }

    try
    {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(
            g_physicalDevice, VK_FORMAT_R32G32_SFLOAT, &formatProperties);
        if((formatProperties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0u)
        {
            RTLog("[WATER FFT] R32G32_SFLOAT cannot be stored; feature disabled");
            return;
        }
        vkGetPhysicalDeviceFormatProperties(
            g_physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &formatProperties);
        if((formatProperties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0u ||
           (formatProperties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0u)
        {
            RTLog("[WATER FFT] RGBA16F storage/linear unsupported; feature disabled");
            return;
        }

        VkImageCreateInfo stateInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        stateInfo.imageType = VK_IMAGE_TYPE_2D;
        stateInfo.format = VK_FORMAT_R32G32_SFLOAT;
        stateInfo.extent = { WATER_FFT_RESOLUTION, WATER_FFT_RESOLUTION, 1u };
        stateInfo.mipLevels = 1u;
        stateInfo.arrayLayers = WATER_FFT_STATE_LAYERS;
        stateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        stateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        stateInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        stateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        stateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(g_device, &stateInfo, nullptr, &g_waterFftStateImage),
            "vkCreateImage waterFftState");

        VkMemoryRequirements stateRequirements{};
        vkGetImageMemoryRequirements(g_device, g_waterFftStateImage, &stateRequirements);
        VkMemoryAllocateInfo stateAllocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        stateAllocate.allocationSize = stateRequirements.size;
        stateAllocate.memoryTypeIndex = findMemoryType(
            g_physicalDevice, stateRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &stateAllocate, nullptr, &g_waterFftStateMemory),
            "vkAllocateMemory waterFftState");
        check(vkBindImageMemory(
            g_device, g_waterFftStateImage, g_waterFftStateMemory, 0u),
            "vkBindImageMemory waterFftState");

        VkImageViewCreateInfo stateViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        stateViewInfo.image = g_waterFftStateImage;
        stateViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        stateViewInfo.format = VK_FORMAT_R32G32_SFLOAT;
        stateViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        stateViewInfo.subresourceRange.levelCount = 1u;
        stateViewInfo.subresourceRange.layerCount = WATER_FFT_STATE_LAYERS;
        check(vkCreateImageView(g_device, &stateViewInfo, nullptr, &g_waterFftStateView),
            "vkCreateImageView waterFftState");

        VkImageCreateInfo slopeInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        slopeInfo.imageType = VK_IMAGE_TYPE_2D;
        slopeInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        slopeInfo.extent = { WATER_FFT_RESOLUTION, WATER_FFT_RESOLUTION, 1u };
        slopeInfo.mipLevels = WATER_FFT_SLOPE_MIPS;
        slopeInfo.arrayLayers = 1u;
        slopeInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        slopeInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        slopeInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        slopeInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        slopeInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(g_device, &slopeInfo, nullptr, &g_waterFftSlopeImage),
            "vkCreateImage waterFftSlope");

        VkMemoryRequirements slopeRequirements{};
        vkGetImageMemoryRequirements(g_device, g_waterFftSlopeImage, &slopeRequirements);
        VkMemoryAllocateInfo slopeAllocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        slopeAllocate.allocationSize = slopeRequirements.size;
        slopeAllocate.memoryTypeIndex = findMemoryType(
            g_physicalDevice, slopeRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &slopeAllocate, nullptr, &g_waterFftSlopeMemory),
            "vkAllocateMemory waterFftSlope");
        check(vkBindImageMemory(
            g_device, g_waterFftSlopeImage, g_waterFftSlopeMemory, 0u),
            "vkBindImageMemory waterFftSlope");

        VkImageViewCreateInfo slopeViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        slopeViewInfo.image = g_waterFftSlopeImage;
        slopeViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        slopeViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        slopeViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        slopeViewInfo.subresourceRange.levelCount = WATER_FFT_SLOPE_MIPS;
        slopeViewInfo.subresourceRange.layerCount = 1u;
        check(vkCreateImageView(g_device, &slopeViewInfo, nullptr, &g_waterFftSlopeView),
            "vkCreateImageView waterFftSlope");

        VkImageViewCreateInfo slopeStorageViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        slopeStorageViewInfo.image = g_waterFftSlopeImage;
        slopeStorageViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        slopeStorageViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        slopeStorageViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        slopeStorageViewInfo.subresourceRange.baseMipLevel = 0u;
        slopeStorageViewInfo.subresourceRange.levelCount = 1u;
        slopeStorageViewInfo.subresourceRange.layerCount = 1u;
        check(vkCreateImageView(g_device, &slopeStorageViewInfo, nullptr, &g_waterFftSlopeStorageView),
            "vkCreateImageView waterFftSlope storage");

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(WATER_FFT_SLOPE_MIPS);
        check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_waterFftSlopeSampler),
            "vkCreateSampler waterFftSlope");

        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0u;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1u;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1u;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1u;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 2u;
        layoutInfo.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(
            g_device, &layoutInfo, nullptr, &g_waterFftSetLayout),
            "vkCreateDescriptorSetLayout waterFft");

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = 2u;
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1u;
        poolInfo.poolSizeCount = 1u;
        poolInfo.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(
            g_device, &poolInfo, nullptr, &g_waterFftDescriptorPool),
            "vkCreateDescriptorPool waterFft");

        VkDescriptorSetAllocateInfo setAllocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAllocate.descriptorPool = g_waterFftDescriptorPool;
        setAllocate.descriptorSetCount = 1u;
        setAllocate.pSetLayouts = &g_waterFftSetLayout;
        check(vkAllocateDescriptorSets(
            g_device, &setAllocate, &g_waterFftDescriptorSet),
            "vkAllocateDescriptorSets waterFft");

        updateWaterFftDescriptors();

        const std::vector<uint8_t> code = loadSpv("shaders/water_fft.comp.spv");
        VkShaderModule shaderModule = createShaderModule(g_device, code);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size = static_cast<uint32_t>(sizeof(WaterFftPushConstants));
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_waterFftSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(vkCreatePipelineLayout(
            g_device, &pipelineLayoutInfo, nullptr, &g_waterFftPipelineLayout),
            "vkCreatePipelineLayout waterFft");

        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shaderModule;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = g_waterFftPipelineLayout;
        check(vkCreateComputePipelines(
            g_device, getSharedPipelineCache(), 1u, &pipelineInfo,
            nullptr, &g_waterFftPipeline),
            "vkCreateComputePipelines waterFft");
        vkDestroyShaderModule(g_device, shaderModule, nullptr);

        g_waterFftAvailable.store(true);
        RTLog("[WATER FFT] Phillips spectrum pipeline created ("
            + std::to_string(WATER_FFT_RESOLUTION) + "x" + std::to_string(WATER_FFT_RESOLUTION)
            + ", 31 dispatches/update, " + std::to_string(g_waterFftRateHz.load()) + " Hz,"
            + " tile=" + std::to_string(g_waterFftTilePeriod.load())
            + " wind=" + std::to_string(g_waterFftWindSpeed.load())
            + " amp=" + std::to_string(g_waterFftAmplitude.load()) + ")");
    }
    catch(const std::exception& error)
    {
        RTLog(std::string("[WATER FFT] creation failed, feature disabled: ") + error.what());
        g_waterFftAvailable.store(false);
    }
}

// 逐帧调度：按 rateHz 限频；到点才跑整条链（基础 17 趟 + 细节 17 趟，末趟合并 finalize）
void recordWaterFftUpdate(VkCommandBuffer cmd)
{
    g_waterFftUpdatedThisFrame = false;
    if(!kWATER_FFT_PASSES_ENABLED || !g_waterFftAvailable.load() ||
       !g_waterFftEnabled.load() || cmd == VK_NULL_HANDLE)
    {
        return;
    }
    if(g_waterFftPipeline == VK_NULL_HANDLE || g_waterFftDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if(!g_waterFftLastUpdateValid)
    {
        g_waterFftLastUpdate = now;
        g_waterFftLastUpdateValid = true;
    }
    const float elapsed = std::chrono::duration<float>(
        now - g_waterFftLastUpdate).count();
    const float interval = 1.0f / std::max(g_waterFftRateHz.load(), 1.0f);
    if(elapsed < interval)
    {
        return;
    }
    g_waterFftLastUpdate = now;
    g_waterFftTimeSeconds += std::min(elapsed, 0.1f);
    g_waterFftUpdatedThisFrame = true;

    if(!g_waterFftLayoutReady)
    {
        VkImageMemoryBarrier initial[2]{};
        initial[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        initial[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        initial[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        initial[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        initial[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        initial[0].image = g_waterFftStateImage;
        initial[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        initial[0].subresourceRange.levelCount = 1u;
        initial[0].subresourceRange.layerCount = WATER_FFT_STATE_LAYERS;
        initial[0].srcAccessMask = 0u;
        initial[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        initial[1] = initial[0];
        initial[1].image = g_waterFftSlopeImage;
        initial[1].subresourceRange.levelCount = WATER_FFT_SLOPE_MIPS;
        initial[1].subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr,
            0u, nullptr,
            2u, initial
        );
        g_waterFftLayoutReady = true;
    }
    else
    {
        VkImageMemoryBarrier slopeBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        slopeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        slopeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        slopeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        slopeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        slopeBarrier.image = g_waterFftSlopeImage;
        slopeBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        slopeBarrier.subresourceRange.levelCount = WATER_FFT_SLOPE_MIPS;
        slopeBarrier.subresourceRange.layerCount = 1u;
        slopeBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        slopeBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr,
            0u, nullptr,
            1u, &slopeBarrier
        );
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_waterFftPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_waterFftPipelineLayout,
        0u, 1u, &g_waterFftDescriptorSet, 0u, nullptr);

    int32_t layer = 0;

    waterFftDispatch(cmd, -1, layer, 0, 0, 0);
    waterFftPassBarrier(cmd);
    layer ^= 1;

    for(uint32_t stage = 0u; stage < WATER_FFT_LOG2; ++stage)
    {
        const int32_t length = static_cast<int32_t>(2u << stage);
        waterFftDispatch(cmd, layer ^ 1, layer, length, 1, 0);
        waterFftPassBarrier(cmd);
        layer ^= 1;
    }

    waterFftDispatch(cmd, layer ^ 1, layer, 0, 2, 0);
    waterFftPassBarrier(cmd);
    layer ^= 1;

    for(uint32_t stage = 0u; stage < WATER_FFT_LOG2; ++stage)
    {
        const int32_t length = static_cast<int32_t>(2u << stage);
        waterFftDispatch(cmd, layer ^ 1, layer, length, 1, 1);
        waterFftPassBarrier(cmd);
        layer ^= 1;
    }

    const float detailTexel =
        kWATER_FFT_DETAIL_TILE / float(WATER_FFT_RESOLUTION);
    auto detailDispatch = [&](int32_t src, int32_t dst, int32_t stage, int32_t mode, int32_t axis)
    {
        waterFftDispatch(
            cmd, src, dst, stage, mode, axis,
            -1, detailTexel, 0.0f, 0.0f, kWATER_FFT_DETAIL_MIN_WAVE_RADIUS);
        waterFftPassBarrier(cmd);
    };

    int32_t detailLayer = 2;
    detailDispatch(-1, detailLayer, 0, 0, 0);
    detailLayer ^= 1;
    for(uint32_t stage = 0u; stage < WATER_FFT_LOG2; ++stage)
    {
        const int32_t length = static_cast<int32_t>(2u << stage);
        detailDispatch(detailLayer ^ 1, detailLayer, length, 1, 0);
        detailLayer ^= 1;
    }
    detailDispatch(detailLayer ^ 1, detailLayer, 0, 2, 0);
    detailLayer ^= 1;
    for(uint32_t stage = 0u; stage < WATER_FFT_LOG2; ++stage)
    {
        const int32_t length = static_cast<int32_t>(2u << stage);
        detailDispatch(detailLayer ^ 1, detailLayer, length, 1, 1);
        detailLayer ^= 1;
    }

    waterFftDispatch(
        cmd, layer, -1, 0, 3, 0,
        detailLayer,
        -1.0f,
        detailTexel,
        g_waterFftAmplitude.load() * kWATER_FFT_DETAIL_BOOST *
            g_waterFftDetail.load(),
        0.0f);
    g_waterFftWorkingLayer = layer;

    VkImageMemoryBarrier preMip{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    preMip.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    preMip.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    preMip.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preMip.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preMip.image = g_waterFftSlopeImage;
    preMip.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    preMip.subresourceRange.baseMipLevel = 0u;
    preMip.subresourceRange.levelCount = WATER_FFT_SLOPE_MIPS;
    preMip.subresourceRange.layerCount = 1u;
    preMip.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preMip.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u,
        0u, nullptr,
        0u, nullptr,
        1u, &preMip
    );

    for(uint32_t level = 1u; level < WATER_FFT_SLOPE_MIPS; ++level)
    {
        const uint32_t srcSize = WATER_FFT_RESOLUTION >> (level - 1u);
        const uint32_t dstSize = WATER_FFT_RESOLUTION >> level;

        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1u;
        blit.srcSubresource.layerCount = 1u;
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(srcSize),
                              static_cast<int32_t>(srcSize), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.layerCount = 1u;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(dstSize),
                              static_cast<int32_t>(dstSize), 1};
        vkCmdBlitImage(
            cmd,
            g_waterFftSlopeImage, VK_IMAGE_LAYOUT_GENERAL,
            g_waterFftSlopeImage, VK_IMAGE_LAYOUT_GENERAL,
            1u, &blit, VK_FILTER_LINEAR
        );

        VkImageMemoryBarrier midMip{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        midMip.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        midMip.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        midMip.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        midMip.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        midMip.image = g_waterFftSlopeImage;
        midMip.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        midMip.subresourceRange.baseMipLevel = level;
        midMip.subresourceRange.levelCount = 1u;
        midMip.subresourceRange.layerCount = 1u;
        midMip.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        midMip.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u,
            0u, nullptr,
            0u, nullptr,
            1u, &midMip
        );
    }

    VkImageMemoryBarrier consumeBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    consumeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    consumeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    consumeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    consumeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    consumeBarrier.image = g_waterFftSlopeImage;
    consumeBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    consumeBarrier.subresourceRange.baseMipLevel = 0u;
    consumeBarrier.subresourceRange.levelCount = WATER_FFT_SLOPE_MIPS;
    consumeBarrier.subresourceRange.layerCount = 1u;
    consumeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    consumeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u,
        0u, nullptr,
        0u, nullptr,
        1u, &consumeBarrier
    );
}
void destroyWaterFftResources()
{
    g_waterFftAvailable.store(false);
    g_waterFftLayoutReady = false;
    g_waterFftLastUpdateValid = false;
    if(g_waterFftPipeline){vkDestroyPipeline(g_device,g_waterFftPipeline,nullptr);g_waterFftPipeline=VK_NULL_HANDLE;}
    if(g_waterFftPipelineLayout){vkDestroyPipelineLayout(g_device,g_waterFftPipelineLayout,nullptr);g_waterFftPipelineLayout=VK_NULL_HANDLE;}
    if(g_waterFftDescriptorPool){vkDestroyDescriptorPool(g_device,g_waterFftDescriptorPool,nullptr);g_waterFftDescriptorPool=VK_NULL_HANDLE;g_waterFftDescriptorSet=VK_NULL_HANDLE;}
    if(g_waterFftSetLayout){vkDestroyDescriptorSetLayout(g_device,g_waterFftSetLayout,nullptr);g_waterFftSetLayout=VK_NULL_HANDLE;}
    if(g_waterFftSlopeSampler){vkDestroySampler(g_device,g_waterFftSlopeSampler,nullptr);g_waterFftSlopeSampler=VK_NULL_HANDLE;}
    if(g_waterFftSlopeView){vkDestroyImageView(g_device,g_waterFftSlopeView,nullptr);g_waterFftSlopeView=VK_NULL_HANDLE;}
    if(g_waterFftSlopeStorageView){vkDestroyImageView(g_device,g_waterFftSlopeStorageView,nullptr);g_waterFftSlopeStorageView=VK_NULL_HANDLE;}
    if(g_waterFftSlopeImage){vkDestroyImage(g_device,g_waterFftSlopeImage,nullptr);g_waterFftSlopeImage=VK_NULL_HANDLE;}
    if(g_waterFftSlopeMemory){vkFreeMemory(g_device,g_waterFftSlopeMemory,nullptr);g_waterFftSlopeMemory=VK_NULL_HANDLE;}
    if(g_waterFftStateView){vkDestroyImageView(g_device,g_waterFftStateView,nullptr);g_waterFftStateView=VK_NULL_HANDLE;}
    if(g_waterFftStateImage){vkDestroyImage(g_device,g_waterFftStateImage,nullptr);g_waterFftStateImage=VK_NULL_HANDLE;}
    if(g_waterFftStateMemory){vkFreeMemory(g_device,g_waterFftStateMemory,nullptr);g_waterFftStateMemory=VK_NULL_HANDLE;}
}
// 把焦散图绑到 RT set 的 binding 69（折射）/ 70（反射）。创建成功后调用一次。
void updateWaterCausticMapRayDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_waterCausticMapView == VK_NULL_HANDLE ||
       g_waterCausticMapReflectView == VK_NULL_HANDLE ||
       g_waterCausticMapSampler == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo info[2]{};
    info[0].sampler = g_waterCausticMapSampler;
    info[0].imageView = g_waterCausticMapView;
    info[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    info[1].sampler = g_waterCausticMapSampler;
    info[1].imageView = g_waterCausticMapReflectView;
    info[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding = 69u + i;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &info[i];
    }
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);
    RTLog("[WATER CAUSTIC MAP] bound to RT bindings 69 (refract) / 70 (reflect)");
}
void destroyWaterCausticMapResources()
{
    g_waterCausticMapAvailable.store(false);
    if(g_waterCausticMapPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_waterCausticMapPipeline, nullptr);
        g_waterCausticMapPipeline = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapStorageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterCausticMapStorageView, nullptr);
        g_waterCausticMapStorageView = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_waterCausticMapPipelineLayout, nullptr);
        g_waterCausticMapPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_waterCausticMapDescriptorPool, nullptr);
        g_waterCausticMapDescriptorPool = VK_NULL_HANDLE;
        g_waterCausticMapDescriptorSet = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_waterCausticMapSetLayout, nullptr);
        g_waterCausticMapSetLayout = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapFramebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(g_device, g_waterCausticMapFramebuffer, nullptr);
        g_waterCausticMapFramebuffer = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapReflectFramebuffer != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(g_device, g_waterCausticMapReflectFramebuffer, nullptr);
        g_waterCausticMapReflectFramebuffer = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapRenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(g_device, g_waterCausticMapRenderPass, nullptr);
        g_waterCausticMapRenderPass = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(g_device, g_waterCausticMapSampler, nullptr);
        g_waterCausticMapSampler = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterCausticMapView, nullptr);
        g_waterCausticMapView = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapMip0View != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterCausticMapMip0View, nullptr);
        g_waterCausticMapMip0View = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapReflectView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterCausticMapReflectView, nullptr);
        g_waterCausticMapReflectView = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapReflectMip0View != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterCausticMapReflectMip0View, nullptr);
        g_waterCausticMapReflectMip0View = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(g_device, g_waterCausticMapImage, nullptr);
        g_waterCausticMapImage = VK_NULL_HANDLE;
    }
    if(g_waterCausticMapMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_waterCausticMapMemory, nullptr);
        g_waterCausticMapMemory = VK_NULL_HANDLE;
    }
}
void createWaterCausticMapResources()
{
    if(g_waterCausticMapAvailable.load())
    {
        return;
    }
    if(!g_waterFftAvailable.load() ||
       g_waterFftSlopeView == VK_NULL_HANDLE ||
       g_waterFftSlopeSampler == VK_NULL_HANDLE)
    {
        RTLog("[WATER CAUSTIC MAP] FFT slope map unavailable; caustic map disabled");
        return;
    }

    try
    {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(
            g_physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &formatProperties);
        const VkFormatFeatureFlags requiredFeatures =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((formatProperties.optimalTilingFeatures & requiredFeatures) !=
           requiredFeatures)
        {
            RTLog("[WATER CAUSTIC MAP] RGBA16F color-attachment blend/linear unsupported; "
                  "caustic map disabled");
            return;
        }

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.extent = {
            WATER_CAUSTIC_MAP_RESOLUTION,
            WATER_CAUSTIC_MAP_RESOLUTION,
            1u
        };
        imageInfo.mipLevels = WATER_CAUSTIC_MAP_MIPS;
        imageInfo.arrayLayers = WATER_CAUSTIC_MAP_LAYERS;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_STORAGE_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(g_device, &imageInfo, nullptr, &g_waterCausticMapImage),
            "water caustic map create image failed");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(g_device, g_waterCausticMapImage, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(
            g_physicalDevice, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr, &g_waterCausticMapMemory),
            "water caustic map allocate image memory failed");
        check(vkBindImageMemory(
            g_device, g_waterCausticMapImage, g_waterCausticMapMemory, 0u),
            "water caustic map bind image memory failed");

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = g_waterCausticMapImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0u;
        viewInfo.subresourceRange.levelCount = WATER_CAUSTIC_MAP_MIPS;
        viewInfo.subresourceRange.baseArrayLayer = WATER_CAUSTIC_MAP_LAYER_REFRACT;
        viewInfo.subresourceRange.layerCount = 1u;
        check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_waterCausticMapView),
            "water caustic map create view failed");

        viewInfo.subresourceRange.baseArrayLayer = WATER_CAUSTIC_MAP_LAYER_REFLECT;
        check(vkCreateImageView(
            g_device, &viewInfo, nullptr, &g_waterCausticMapReflectView),
            "water caustic map create reflect view failed");

        viewInfo.subresourceRange.levelCount = 1u;
        viewInfo.subresourceRange.baseArrayLayer = WATER_CAUSTIC_MAP_LAYER_REFRACT;
        check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_waterCausticMapMip0View),
            "water caustic map create mip0 view failed");
        viewInfo.subresourceRange.baseArrayLayer = WATER_CAUSTIC_MAP_LAYER_REFLECT;
        check(vkCreateImageView(
            g_device, &viewInfo, nullptr, &g_waterCausticMapReflectMip0View),
            "water caustic map create reflect mip0 view failed");

        // 2026-11-01：compute 生产者的写入目标 —— 两层一起绑定成 2D array 的
        // mip0 视图，一次 dispatch 出折射 + 反射。
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.subresourceRange.baseMipLevel = 0u;
        viewInfo.subresourceRange.levelCount = 1u;
        viewInfo.subresourceRange.baseArrayLayer = WATER_CAUSTIC_MAP_LAYER_REFRACT;
        viewInfo.subresourceRange.layerCount = WATER_CAUSTIC_MAP_LAYERS;
        check(vkCreateImageView(
            g_device, &viewInfo, nullptr, &g_waterCausticMapStorageView),
            "water caustic map create storage array view failed");

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(WATER_CAUSTIC_MAP_MIPS - 1u);
        check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_waterCausticMapSampler),
            "water caustic map create sampler failed");

        {
            VkCommandBufferAllocateInfo commandAllocation{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commandAllocation.commandPool = g_rtCommandPool;
            commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commandAllocation.commandBufferCount = 1u;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            check(vkAllocateCommandBuffers(
                g_device, &commandAllocation, &commandBuffer),
                "water caustic map init command allocation failed");
            VkCommandBufferBeginInfo begin{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(commandBuffer, &begin),
                "water caustic map init command begin failed");

            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = g_waterCausticMapImage;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0u;
            barrier.subresourceRange.levelCount = WATER_CAUSTIC_MAP_MIPS;
            barrier.subresourceRange.layerCount = WATER_CAUSTIC_MAP_LAYERS;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

            VkClearColorValue clear{};
            clear.float32[0] = 1.0f;
            clear.float32[1] = 1.0f;
            clear.float32[2] = 1.0f;
            clear.float32[3] = 1.0f;
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel = 0u;
            range.levelCount = WATER_CAUSTIC_MAP_MIPS;
            range.baseArrayLayer = 0u;
            range.layerCount = WATER_CAUSTIC_MAP_LAYERS;
            vkCmdClearColorImage(
                commandBuffer, g_waterCausticMapImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &clear, 1u, &range);

            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

            check(vkEndCommandBuffer(commandBuffer),
                "water caustic map init command end failed");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1u;
            submit.pCommandBuffers = &commandBuffer;
            check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE),
                "water caustic map init submit failed");
            check(vkQueueWaitIdle(g_queue),
                "water caustic map init wait failed");
            vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
        }

        VkAttachmentDescription attachment{};
        attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkAttachmentReference colorReference{};
        colorReference.attachment = 0u;
        colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1u;
        subpass.pColorAttachments = &colorReference;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0u;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0u;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{
            VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        renderPassInfo.attachmentCount = 1u;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1u;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1u;
        renderPassInfo.pDependencies = &dependency;
        check(vkCreateRenderPass(
            g_device, &renderPassInfo, nullptr, &g_waterCausticMapRenderPass),
            "water caustic map create render pass failed");

        VkFramebufferCreateInfo framebufferInfo{
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = g_waterCausticMapRenderPass;
        framebufferInfo.attachmentCount = 1u;
        framebufferInfo.pAttachments = &g_waterCausticMapMip0View;
        framebufferInfo.width = WATER_CAUSTIC_MAP_RESOLUTION;
        framebufferInfo.height = WATER_CAUSTIC_MAP_RESOLUTION;
        framebufferInfo.layers = 1u;
        check(vkCreateFramebuffer(
            g_device, &framebufferInfo, nullptr, &g_waterCausticMapFramebuffer),
            "water caustic map create framebuffer failed");
        framebufferInfo.pAttachments = &g_waterCausticMapReflectMip0View;
        check(vkCreateFramebuffer(
            g_device, &framebufferInfo, nullptr,
            &g_waterCausticMapReflectFramebuffer),
            "water caustic map create reflect framebuffer failed");

        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0u;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1u;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1u;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1u;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 2u;
        layoutInfo.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(
            g_device, &layoutInfo, nullptr, &g_waterCausticMapSetLayout),
            "water caustic map create descriptor set layout failed");

        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 1u;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[1].descriptorCount = 1u;
        VkDescriptorPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1u;
        poolInfo.poolSizeCount = 2u;
        poolInfo.pPoolSizes = poolSizes;
        check(vkCreateDescriptorPool(
            g_device, &poolInfo, nullptr, &g_waterCausticMapDescriptorPool),
            "water caustic map create descriptor pool failed");

        VkDescriptorSetAllocateInfo setAllocate{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAllocate.descriptorPool = g_waterCausticMapDescriptorPool;
        setAllocate.descriptorSetCount = 1u;
        setAllocate.pSetLayouts = &g_waterCausticMapSetLayout;
        check(vkAllocateDescriptorSets(
            g_device, &setAllocate, &g_waterCausticMapDescriptorSet),
            "water caustic map allocate descriptor set failed");

        VkDescriptorImageInfo slopeInfo{};
        slopeInfo.sampler = g_waterFftSlopeSampler;
        slopeInfo.imageView = g_waterFftSlopeView;
        slopeInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo storageInfo{};
        storageInfo.sampler = VK_NULL_HANDLE;
        storageInfo.imageView = g_waterCausticMapStorageView;
        storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = g_waterCausticMapDescriptorSet;
        writes[0].dstBinding = 0u;
        writes[0].descriptorCount = 1u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &slopeInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = g_waterCausticMapDescriptorSet;
        writes[1].dstBinding = 1u;
        writes[1].descriptorCount = 1u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &storageInfo;
        vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size =
            static_cast<uint32_t>(sizeof(WaterCausticMapPushConstants));
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_waterCausticMapSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(vkCreatePipelineLayout(
            g_device, &pipelineLayoutInfo, nullptr,
            &g_waterCausticMapPipelineLayout),
            "water caustic map create pipeline layout failed");

        const std::vector<uint8_t> computeCode =
            loadSpv("shaders/water_caustic_map.comp.spv");
        VkShaderModule computeModule = createShaderModule(g_device, computeCode);

        try
        {
            VkPipelineShaderStageCreateInfo stageInfo{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stageInfo.module = computeModule;
            stageInfo.pName = "main";
            VkComputePipelineCreateInfo pipelineInfo{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = stageInfo;
            pipelineInfo.layout = g_waterCausticMapPipelineLayout;
            check(vkCreateComputePipelines(
                g_device, getSharedPipelineCache(), 1u, &pipelineInfo,
                nullptr, &g_waterCausticMapPipeline),
                "water caustic map create compute pipeline failed");
        }
        catch(...)
        {
            vkDestroyShaderModule(g_device, computeModule, nullptr);
            throw;
        }
        vkDestroyShaderModule(g_device, computeModule, nullptr);

        g_waterCausticMapAvailable.store(true);
        updateWaterCausticMapRayDescriptor();
        RTLog("[WATER CAUSTIC MAP] compute map created ("
            + std::to_string(WATER_CAUSTIC_MAP_RESOLUTION) + "x"
            + std::to_string(WATER_CAUSTIC_MAP_RESOLUTION) + ", "
            + std::to_string(WATER_CAUSTIC_MAP_MIPS) + " mips, "
            + std::to_string(WATER_CAUSTIC_MAP_LAYERS)
            + " layers, 5-tap jacobian, single IOR + dispersion)");
    }
    catch(const std::exception& error)
    {
        RTLog(std::string("[WATER CAUSTIC MAP] creation failed, feature disabled: ")
            + error.what());
        destroyWaterCausticMapResources();
    }
}

// ===========================================================================
// 2026-10-28 焦散图独立时钟（ORION_WATER_CAUSTIC_HZ）
//
// 为什么：这条链原本只能跟着 FFT 的 30 Hz 时钟跑（g_waterFftUpdatedThisFrame），
// 而"帧越慢 ⇒ 每帧都命中 30 Hz 节拍 ⇒ 每帧都重建"会把帧时间钉在 ~30 ms。
// 实测（2026-10-28 水边一局，perf/step0/perf-20261028-moving_*.log）：
// 焦散图重建 5.82 ms = 整帧 21%，与主 RayGen 12.6 ms 同级，而当时 FFT 只有
// 0.07 ms、水面波方程模拟 ≈0 —— 整条水链的开销几乎全在这一张图上。
//
// 取值（与 docs/env-switches.md 的约定一致）：
//   · 未设 / 空 ⇒ -1 = **不节流**，完全等于闸门上线前的行为（跟随 FFT 时钟）
//   · >0        ⇒ 该频率上限（10 ⇒ 每 100 ms 最多重建一次）
//   · 0         ⇒ 永不重建（保留已有图；进世界时图是清空的，水下会没有光斑）
// 只读一次，改完必须重启客户端。
// ===========================================================================
float waterCausticMapHz()
{
    static const float cached = []() -> float
    {
        const char* raw = std::getenv("ORION_WATER_CAUSTIC_HZ");

        if(raw == nullptr || raw[0] == '\0')
        {
            // 2026-11-01：默认 30 Hz（原来 -1 = 不节流 = 每帧重建）。
            return 30.0f;
        }

        const float value = static_cast<float>(std::atof(raw));

        return value < 0.0f ? -1.0f : value;
    }();

    return cached;
}

// true = 本帧不重建。只在**真正要录**的位置调用（见 recordWaterCausticMap 的
// 调用点），否则维度门/雨天门会白白消耗配额，雨停后要多等一个周期才更新。
bool waterCausticMapSkipThisFrame()
{
    const float hz = waterCausticMapHz();

    if(hz < 0.0f)
    {
        return false;
    }
    if(hz <= 0.0f)
    {
        return true;
    }
    static double s_lastSeconds = -1.0e9;

    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if(seconds - s_lastSeconds < 1.0 / static_cast<double>(hz))
    {
        return true;
    }

    s_lastSeconds = seconds;

    return false;
}


// 逐帧调度：只在 FFT 斜率图**本帧真的重写**时重建（同一 30 Hz 时钟）。
// 2026-10-23：雨天直接跳过（见下面的雨天门）。
// 2026-10-28：再叠一层 ORION_WATER_CAUSTIC_HZ 的独立时钟（默认不节流）。
void recordWaterCausticMap(VkCommandBuffer cmd)
{
    const bool fftUpdated = g_waterFftUpdatedThisFrame;
    g_waterFftUpdatedThisFrame = false;

    // 2026-11-01 warm-up：进世界的第一帧无条件跑一次。
    // 首次 vkCmdDispatch 到一张从未写过的 storage image 会触发内存驻留、
    // descriptor 首次解析与 GPU 从夜间低频爬升 —— 实测就是日出首帧的 441ms。
    // 业界做法（UE FShaderPipelineCache / Unity GraphicsStateCollection /
    // ncnn PipelineCache）都是把这类"第一次"成本挪到加载阶段。
    static bool s_warmupDone = false;
    static const bool s_warmupDisabled = []() -> bool
    {
        const char* raw = std::getenv("ORION_WATER_CAUSTIC_WARMUP");
        return raw != nullptr && raw[0] == '0' && raw[1] == '\0';
    }();
    const bool warmup = (!s_warmupDone && !s_warmupDisabled);
    if(warmup)
    {
        s_warmupDone = true;
    }

    if((!warmup && !fftUpdated) ||
       cmd == VK_NULL_HANDLE ||
       !g_waterCausticMapAvailable.load() ||
       g_waterCausticMapPipeline == VK_NULL_HANDLE ||
       g_waterCausticMapStorageView == VK_NULL_HANDLE ||
       g_waterCausticMapPipelineLayout == VK_NULL_HANDLE ||
       g_waterCausticMapDescriptorSet == VK_NULL_HANDLE ||
       g_cameraUboMapped == nullptr)
    {
        return;
    }

    CameraUBO* ubo = static_cast<CameraUBO*>(g_cameraUboMapped);
    if(!warmup && ubo->weatherParams[2] > 1.5f)
    {
        return;
    }
    if(!warmup && ubo->weatherParams[0] > 0.05f)
    {
        return;
    }

    const float sunPhase = ubo->historyParamsPadding1;
    const float sunAngle =
        (sunPhase - std::floor(sunPhase)) * 6.28318530718f;
    float sunX = std::cos(sunAngle);
    float sunY = 0.81915204429f * std::sin(sunAngle);
    float sunZ = 0.57357643635f * std::sin(sunAngle);
    const float sunLength = std::sqrt(
        sunX * sunX + sunY * sunY + sunZ * sunZ);
    if(sunLength <= 1.0e-6f)
    {
        return;
    }
    sunX /= sunLength;
    sunY /= sunLength;
    sunZ /= sunLength;
    // 2026-11-01：门限从 0 抬到 0.08（约 4.6°）。日出瞬间 sunY 刚过 0 时这条
    // pass 从"整夜不跑"变成"每帧跑"，实测首帧 441ms（GPU 冷状态 + 千万顶点）。
    // 抬到 0.08 后冷启动不再直接撞在最贵的角度上。
    if(warmup && sunY <= 0.08f)
    {
        // warm-up 帧可能正好是夜里：给一个合法的朝上方向，只为点亮资源。
        sunX = 0.0f;
        sunY = 1.0f;
        sunZ = 0.0f;
    }
    else if(sunY <= 0.08f)
    {
        return;
    }

    // 附近有水才跑（判据沿用 recordWaterCaustics：玩家在水里，或与水面高差
    // 不超过 32 格）。没有水的地方这张图没有任何消费者，纯浪费。
    if(!warmup)
    {
        const bool playerInWater = ubo->playerInWater > 0.5f;
        const float surfaceDelta = ubo->playerY - ubo->waterSurfaceY;
        if(!playerInWater &&
           g_waterSimKeepAliveSeconds <= 0.0f &&
           std::fabs(surfaceDelta) > WATER_CAUSTIC_NEAR_WATER_Y_RANGE)
        {
            return;
        }

        if(waterCausticMapSkipThisFrame())
        {
            return;
        }
    }

    const float tilePeriod = std::max(ubo->waterFftParams[0], 1.0f);

    VkImageMemoryBarrier slopeBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    slopeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    slopeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    slopeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    slopeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    slopeBarrier.image = g_waterFftSlopeImage;
    slopeBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    slopeBarrier.subresourceRange.baseMipLevel = 0u;
    slopeBarrier.subresourceRange.levelCount = WATER_FFT_SLOPE_MIPS;
    slopeBarrier.subresourceRange.layerCount = 1u;
    slopeBarrier.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    slopeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &slopeBarrier);

    VkImageMemoryBarrier mapBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    mapBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    mapBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    mapBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mapBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mapBarrier.image = g_waterCausticMapImage;
    mapBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    mapBarrier.subresourceRange.baseMipLevel = 0u;
    mapBarrier.subresourceRange.levelCount = WATER_CAUSTIC_MAP_MIPS;
    mapBarrier.subresourceRange.layerCount = WATER_CAUSTIC_MAP_LAYERS;
    mapBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    mapBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &mapBarrier);

    // 2026-11-01：一次 dispatch 出两层（z = 0 折射 / 1 反射）。
    // params.y = 折射参考深度，params.z = 反射参考高度，params.w = 色散强度。
    WaterCausticMapPushConstants push{};
    push.sunDirection[0] = sunX;
    push.sunDirection[1] = sunY;
    push.sunDirection[2] = sunZ;
    push.sunDirection[3] = 0.0f;
    push.params[0] = tilePeriod;
    push.params[1] = WATER_CAUSTIC_MAP_REFERENCE_DEPTH;
    push.params[2] = WATER_REFLECT_CAUSTIC_MAP_HEIGHT;
    push.params[3] = 0.45f;

    vkCmdBindPipeline(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_waterCausticMapPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_waterCausticMapPipelineLayout,
        0u, 1u, &g_waterCausticMapDescriptorSet, 0u, nullptr);
    vkCmdPushConstants(
        cmd,
        g_waterCausticMapPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0u,
        static_cast<uint32_t>(sizeof(push)),
        &push);

    const uint32_t causticGroups = (WATER_CAUSTIC_MAP_RESOLUTION + 7u) / 8u;
    vkCmdDispatch(
        cmd, causticGroups, causticGroups, WATER_CAUSTIC_MAP_LAYERS);

    VkImageMemoryBarrier preMip{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    preMip.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    preMip.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    preMip.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preMip.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preMip.image = g_waterCausticMapImage;
    preMip.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    preMip.subresourceRange.baseMipLevel = 0u;
    preMip.subresourceRange.levelCount = WATER_CAUSTIC_MAP_MIPS;
    preMip.subresourceRange.layerCount = WATER_CAUSTIC_MAP_LAYERS;
    preMip.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preMip.dstAccessMask =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &preMip);

    for(uint32_t layer = 0u; layer < WATER_CAUSTIC_MAP_LAYERS; ++layer)
    {
        for(uint32_t level = 1u; level < WATER_CAUSTIC_MAP_MIPS; ++level)
        {
            const uint32_t srcSize =
                WATER_CAUSTIC_MAP_RESOLUTION >> (level - 1u);
            const uint32_t dstSize = WATER_CAUSTIC_MAP_RESOLUTION >> level;

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = level - 1u;
            blit.srcSubresource.baseArrayLayer = layer;
            blit.srcSubresource.layerCount = 1u;
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {static_cast<int32_t>(srcSize),
                                  static_cast<int32_t>(srcSize), 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = level;
            blit.dstSubresource.baseArrayLayer = layer;
            blit.dstSubresource.layerCount = 1u;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {static_cast<int32_t>(dstSize),
                                  static_cast<int32_t>(dstSize), 1};
            vkCmdBlitImage(
                cmd,
                g_waterCausticMapImage, VK_IMAGE_LAYOUT_GENERAL,
                g_waterCausticMapImage, VK_IMAGE_LAYOUT_GENERAL,
                1u, &blit, VK_FILTER_LINEAR);

            VkImageMemoryBarrier midMip{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            midMip.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            midMip.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            midMip.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            midMip.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            midMip.image = g_waterCausticMapImage;
            midMip.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            midMip.subresourceRange.baseMipLevel = level;
            midMip.subresourceRange.levelCount = 1u;
            midMip.subresourceRange.baseArrayLayer = layer;
            midMip.subresourceRange.layerCount = 1u;
            midMip.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            midMip.dstAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0u, 0u, nullptr, 0u, nullptr, 1u, &midMip);
        }
    }

    VkImageMemoryBarrier consumeBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    consumeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    consumeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    consumeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    consumeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    consumeBarrier.image = g_waterCausticMapImage;
    consumeBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    consumeBarrier.subresourceRange.baseMipLevel = 0u;
    consumeBarrier.subresourceRange.levelCount = WATER_CAUSTIC_MAP_MIPS;
    consumeBarrier.subresourceRange.layerCount = WATER_CAUSTIC_MAP_LAYERS;
    consumeBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    consumeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &consumeBarrier);
}

#if 0
constexpr uint32_t WATER_FOAM_RESOLUTION = 512u;
constexpr uint32_t WATER_FOAM_LAYERS = 2u;
constexpr uint32_t WATER_FOAM_MIPS = 6u;
constexpr float WATER_FOAM_WORLD_SIZE = 192.0f;
constexpr float WATER_FOAM_TEXEL_WORLD_SIZE =
    WATER_FOAM_WORLD_SIZE / static_cast<float>(WATER_FOAM_RESOLUTION);
constexpr uint32_t WATER_FOAM_LOCAL_SIZE = 16u;
constexpr float WATER_FOAM_UPDATE_SECONDS = 1.0f / 30.0f;

// 与 water_foam.comp 的 WaterFoamPush 逐字对应（6 × vec4 = 96 B）。
struct WaterFoamPushConstants
{
    int32_t layersAndScroll[4];
    float timing[4];
    float simState[4];
    float foamFrame[4];
    float knobs[4];
    float knobs2[4];
};
static_assert(sizeof(WaterFoamPushConstants) == 96u,
    "WaterFoamPushConstants must match the std430 layout of the GLSL push block");
static VkImage g_waterFoamImage = VK_NULL_HANDLE;
static VkDeviceMemory g_waterFoamMemory = VK_NULL_HANDLE;
static VkImageView g_waterFoamView = VK_NULL_HANDLE;
static VkImageView g_waterFoamStorageView = VK_NULL_HANDLE;
static VkSampler g_waterFoamSampler = VK_NULL_HANDLE;
static VkImage g_waterFoamInjectionImage = VK_NULL_HANDLE;
static VkDeviceMemory g_waterFoamInjectionMemory = VK_NULL_HANDLE;
static VkImageView g_waterFoamInjectionView = VK_NULL_HANDLE;
static VkDescriptorSetLayout g_waterFoamSetLayout = VK_NULL_HANDLE;
static VkDescriptorPool g_waterFoamDescriptorPool = VK_NULL_HANDLE;
static VkDescriptorSet g_waterFoamDescriptorSet = VK_NULL_HANDLE;
static VkPipelineLayout g_waterFoamPipelineLayout = VK_NULL_HANDLE;
static VkPipeline g_waterFoamPipeline = VK_NULL_HANDLE;
static std::atomic<bool> g_waterFoamAvailable{false};
static bool g_waterFoamLayoutReady = false;
static uint32_t g_waterFoamReadLayer = 0u;
static bool g_waterFoamOriginValid = false;
static float g_waterFoamOriginX = 0.0f;
static float g_waterFoamOriginZ = 0.0f;
static std::chrono::steady_clock::time_point g_waterFoamLastUpdate{};
static bool g_waterFoamLastUpdateValid = false;
static bool g_waterFoamLoggedFirstDispatch = false;
static bool waterFoamFeatureEnabled()
{
    static const bool cached = []() -> bool
    {
        const char* raw = std::getenv("ORION_WATER_FOAM");
        if(raw == nullptr)
        {
            return true;
        }
        return std::atoi(raw) != 0;
    }();
    return cached;
}

// 把泡沫图绑到 RT set 的 binding 73（采样）/ 74（岸线注入）。创建成功后调用一次。
static void updateWaterFoamRayDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_waterFoamView == VK_NULL_HANDLE ||
       g_waterFoamSampler == VK_NULL_HANDLE ||
       g_waterFoamInjectionView == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo info[2]{};
    info[0].sampler = g_waterFoamSampler;
    info[0].imageView = g_waterFoamView;
    info[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    info[1].imageView = g_waterFoamInjectionView;
    info[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding = 73u + i;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = (i == 0u)
            ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &info[i];
    }
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);
    RTLog("[WATER FOAM] foam map bound to RT bindings 73 (sample) / 74 (shore injection)");
}

static void destroyWaterFoamResources()
{
    g_waterFoamAvailable.store(false);
    g_waterFoamLayoutReady = false;
    g_waterFoamOriginValid = false;
    if(g_waterFoamPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_waterFoamPipeline, nullptr);
        g_waterFoamPipeline = VK_NULL_HANDLE;
    }
    if(g_waterFoamPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_waterFoamPipelineLayout, nullptr);
        g_waterFoamPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_waterFoamDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_waterFoamDescriptorPool, nullptr);
        g_waterFoamDescriptorPool = VK_NULL_HANDLE;
        g_waterFoamDescriptorSet = VK_NULL_HANDLE;
    }
    if(g_waterFoamSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_waterFoamSetLayout, nullptr);
        g_waterFoamSetLayout = VK_NULL_HANDLE;
    }
    if(g_waterFoamSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(g_device, g_waterFoamSampler, nullptr);
        g_waterFoamSampler = VK_NULL_HANDLE;
    }
    if(g_waterFoamView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterFoamView, nullptr);
        g_waterFoamView = VK_NULL_HANDLE;
    }
    if(g_waterFoamStorageView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterFoamStorageView, nullptr);
        g_waterFoamStorageView = VK_NULL_HANDLE;
    }
    if(g_waterFoamInjectionView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterFoamInjectionView, nullptr);
        g_waterFoamInjectionView = VK_NULL_HANDLE;
    }
    if(g_waterFoamImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(g_device, g_waterFoamImage, nullptr);
        g_waterFoamImage = VK_NULL_HANDLE;
    }
    if(g_waterFoamMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_waterFoamMemory, nullptr);
        g_waterFoamMemory = VK_NULL_HANDLE;
    }
    if(g_waterFoamInjectionImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(g_device, g_waterFoamInjectionImage, nullptr);
        g_waterFoamInjectionImage = VK_NULL_HANDLE;
    }
    if(g_waterFoamInjectionMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_waterFoamInjectionMemory, nullptr);
        g_waterFoamInjectionMemory = VK_NULL_HANDLE;
    }
}

static void createWaterFoamResources()
{
    if(g_waterFoamAvailable.load())
    {
        return;
    }
    if(!waterFoamFeatureEnabled())
    {
        RTLog("[WATER FOAM] disabled by ORION_WATER_FOAM");
        return;
    }
    if(!g_waterFftAvailable.load() ||
       g_waterFftSlopeView == VK_NULL_HANDLE ||
       g_waterFftSlopeSampler == VK_NULL_HANDLE ||
       g_waterSimView == VK_NULL_HANDLE ||
       g_waterSimSampler == VK_NULL_HANDLE)
    {
        RTLog("[WATER FOAM] FFT/simulation inputs unavailable; foam map disabled");
        return;
    }

    try
    {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(
            g_physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &formatProperties);
        const VkFormatFeatureFlags foamFeatures =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((formatProperties.optimalTilingFeatures & foamFeatures) != foamFeatures)
        {
            RTLog("[WATER FOAM] RGBA16F storage/sampled/linear unsupported; disabled");
            return;
        }
        vkGetPhysicalDeviceFormatProperties(
            g_physicalDevice, VK_FORMAT_R32_UINT, &formatProperties);
        const VkFormatFeatureFlags injectionFeatures =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
            VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
        if((formatProperties.optimalTilingFeatures & injectionFeatures) !=
           injectionFeatures)
        {
            RTLog("[WATER FOAM] R32_UINT storage atomics unsupported; disabled");
            return;
        }

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        imageInfo.extent = {
            WATER_FOAM_RESOLUTION, WATER_FOAM_RESOLUTION, 1u};
        imageInfo.mipLevels = WATER_FOAM_MIPS;
        imageInfo.arrayLayers = WATER_FOAM_LAYERS;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(g_device, &imageInfo, nullptr, &g_waterFoamImage),
            "water foam map create image failed");

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(g_device, g_waterFoamImage, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(
            g_physicalDevice, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr, &g_waterFoamMemory),
            "water foam map allocate image memory failed");
        check(vkBindImageMemory(
            g_device, g_waterFoamImage, g_waterFoamMemory, 0u),
            "water foam map bind image memory failed");

        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = g_waterFoamImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0u;
        viewInfo.subresourceRange.levelCount = WATER_FOAM_MIPS;
        viewInfo.subresourceRange.baseArrayLayer = 0u;
        viewInfo.subresourceRange.layerCount = WATER_FOAM_LAYERS;
        check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_waterFoamView),
            "water foam map create view failed");

        viewInfo.subresourceRange.levelCount = 1u;
        check(vkCreateImageView(
            g_device, &viewInfo, nullptr, &g_waterFoamStorageView),
            "water foam map create storage view failed");

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(WATER_FOAM_MIPS - 1u);
        check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_waterFoamSampler),
            "water foam map create sampler failed");

        VkImageCreateInfo injectionInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        injectionInfo.imageType = VK_IMAGE_TYPE_2D;
        injectionInfo.format = VK_FORMAT_R32_UINT;
        injectionInfo.extent = {
            WATER_FOAM_RESOLUTION, WATER_FOAM_RESOLUTION, 1u};
        injectionInfo.mipLevels = 1u;
        injectionInfo.arrayLayers = 1u;
        injectionInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        injectionInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        injectionInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        injectionInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        injectionInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(
            g_device, &injectionInfo, nullptr, &g_waterFoamInjectionImage),
            "water foam injection create image failed");

        VkMemoryRequirements injectionRequirements{};
        vkGetImageMemoryRequirements(
            g_device, g_waterFoamInjectionImage, &injectionRequirements);
        VkMemoryAllocateInfo injectionAllocation{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        injectionAllocation.allocationSize = injectionRequirements.size;
        injectionAllocation.memoryTypeIndex = findMemoryType(
            g_physicalDevice, injectionRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(
            g_device, &injectionAllocation, nullptr, &g_waterFoamInjectionMemory),
            "water foam injection allocate image memory failed");
        check(vkBindImageMemory(
            g_device, g_waterFoamInjectionImage,
            g_waterFoamInjectionMemory, 0u),
            "water foam injection bind image memory failed");

        VkImageViewCreateInfo injectionViewInfo{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        injectionViewInfo.image = g_waterFoamInjectionImage;
        injectionViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        injectionViewInfo.format = VK_FORMAT_R32_UINT;
        injectionViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        injectionViewInfo.subresourceRange.baseMipLevel = 0u;
        injectionViewInfo.subresourceRange.levelCount = 1u;
        injectionViewInfo.subresourceRange.baseArrayLayer = 0u;
        injectionViewInfo.subresourceRange.layerCount = 1u;
        check(vkCreateImageView(
            g_device, &injectionViewInfo, nullptr, &g_waterFoamInjectionView),
            "water foam injection create view failed");

        {
            VkCommandBufferAllocateInfo commandAllocation{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commandAllocation.commandPool = g_rtCommandPool;
            commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commandAllocation.commandBufferCount = 1u;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
            check(vkAllocateCommandBuffers(
                g_device, &commandAllocation, &commandBuffer),
                "water foam init command allocation failed");
            VkCommandBufferBeginInfo begin{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(commandBuffer, &begin),
                "water foam init command begin failed");

            VkImageMemoryBarrier initial[2]{};
            initial[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            initial[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            initial[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            initial[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            initial[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            initial[0].image = g_waterFoamImage;
            initial[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            initial[0].subresourceRange.baseMipLevel = 0u;
            initial[0].subresourceRange.levelCount = WATER_FOAM_MIPS;
            initial[0].subresourceRange.baseArrayLayer = 0u;
            initial[0].subresourceRange.layerCount = WATER_FOAM_LAYERS;
            initial[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            initial[1] = initial[0];
            initial[1].image = g_waterFoamInjectionImage;
            initial[1].subresourceRange.levelCount = 1u;
            initial[1].subresourceRange.layerCount = 1u;
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0u, 0u, nullptr, 0u, nullptr, 2u, initial);

            VkClearColorValue clear{};
            VkImageSubresourceRange clearRange{};
            clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clearRange.baseMipLevel = 0u;
            clearRange.levelCount = WATER_FOAM_MIPS;
            clearRange.baseArrayLayer = 0u;
            clearRange.layerCount = WATER_FOAM_LAYERS;
            vkCmdClearColorImage(
                commandBuffer, g_waterFoamImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &clear, 1u, &clearRange);

            clearRange.levelCount = 1u;
            clearRange.layerCount = 1u;
            vkCmdClearColorImage(
                commandBuffer, g_waterFoamInjectionImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &clear, 1u, &clearRange);

            for(uint32_t i = 0u; i < 2u; ++i)
            {
                initial[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                initial[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                initial[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                initial[i].dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                0u, 0u, nullptr, 0u, nullptr, 2u, initial);

            check(vkEndCommandBuffer(commandBuffer),
                "water foam init command end failed");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1u;
            submit.pCommandBuffers = &commandBuffer;
            check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE),
                "water foam init submit failed");
            check(vkQueueWaitIdle(g_queue),
                "water foam init wait failed");
            vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
        }

        VkDescriptorSetLayoutBinding bindings[5]{};
        bindings[0].binding = 0u;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1u;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1u;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1u;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2] = bindings[0];
        bindings[2].binding = 2u;
        bindings[3] = bindings[0];
        bindings[3].binding = 3u;
        bindings[4].binding = 4u;
        bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[4].descriptorCount = 1u;
        bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = 5u;
        layoutInfo.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(
            g_device, &layoutInfo, nullptr, &g_waterFoamSetLayout),
            "water foam create descriptor set layout failed");

        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 3u;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[1].descriptorCount = 2u;
        VkDescriptorPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1u;
        poolInfo.poolSizeCount = 2u;
        poolInfo.pPoolSizes = poolSizes;
        check(vkCreateDescriptorPool(
            g_device, &poolInfo, nullptr, &g_waterFoamDescriptorPool),
            "water foam create descriptor pool failed");

        VkDescriptorSetAllocateInfo setAllocate{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setAllocate.descriptorPool = g_waterFoamDescriptorPool;
        setAllocate.descriptorSetCount = 1u;
        setAllocate.pSetLayouts = &g_waterFoamSetLayout;
        check(vkAllocateDescriptorSets(
            g_device, &setAllocate, &g_waterFoamDescriptorSet),
            "water foam allocate descriptor set failed");

        VkDescriptorImageInfo infos[5]{};
        infos[0].sampler = g_waterFoamSampler;
        infos[0].imageView = g_waterFoamView;
        infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[1].imageView = g_waterFoamStorageView;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[2].sampler = g_waterSimSampler;
        infos[2].imageView = g_waterSimView;
        infos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[3].sampler = g_waterFftSlopeSampler;
        infos[3].imageView = g_waterFftSlopeView;
        infos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[4].imageView = g_waterFoamInjectionView;
        infos[4].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[5]{};
        for(uint32_t i = 0u; i < 5u; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = g_waterFoamDescriptorSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1u;
            writes[i].descriptorType = (i == 1u || i == 4u)
                ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(g_device, 5u, writes, 0u, nullptr);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size =
            static_cast<uint32_t>(sizeof(WaterFoamPushConstants));
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_waterFoamSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(vkCreatePipelineLayout(
            g_device, &pipelineLayoutInfo, nullptr, &g_waterFoamPipelineLayout),
            "water foam create pipeline layout failed");

        const std::vector<uint8_t> code =
            loadSpv("shaders/water_foam.comp.spv");
        VkShaderModule shaderModule = createShaderModule(g_device, code);
        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shaderModule;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = g_waterFoamPipelineLayout;
        check(vkCreateComputePipelines(
            g_device, getSharedPipelineCache(), 1u, &pipelineInfo,
            nullptr, &g_waterFoamPipeline),
            "water foam create compute pipeline failed");
        vkDestroyShaderModule(g_device, shaderModule, nullptr);

        g_waterFoamReadLayer = 0u;
        g_waterFoamOriginValid = false;
        g_waterFoamLayoutReady = true;
        g_waterFoamAvailable.store(true);
        updateWaterFoamRayDescriptor();
        RTLog("[WATER FOAM] persistent foam map created ("
            + std::to_string(WATER_FOAM_RESOLUTION) + "x"
            + std::to_string(WATER_FOAM_RESOLUTION) + ", "
            + std::to_string(WATER_FOAM_MIPS) + " mips, "
            + std::to_string(WATER_FOAM_LAYERS) + " layers, world "
            + std::to_string(static_cast<int>(WATER_FOAM_WORLD_SIZE)) + "m, "
            + "texel " + std::to_string(WATER_FOAM_TEXEL_WORLD_SIZE) + "m)");
    }
    catch(const std::exception& error)
    {
        RTLog(std::string("[WATER FOAM] creation failed, feature disabled: ")
            + error.what());
        destroyWaterFoamResources();
    }
}

// 每 tick（30 Hz）：滚动重映射 + 耗散 + 三类注入 → 另一层，然后重建 mip。
// 消费端（rt_test.rgen）只在下一帧 updateCamera 发布新读层后才切过去。
static void recordWaterFoam(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE ||
       !g_waterFoamAvailable.load() ||
       !g_waterFoamLayoutReady ||
       g_waterFoamPipeline == VK_NULL_HANDLE ||
       g_waterFoamDescriptorSet == VK_NULL_HANDLE ||
       g_cameraUboMapped == nullptr)
    {
        return;
    }

    CameraUBO* ubo = static_cast<CameraUBO*>(g_cameraUboMapped);
    const auto now = std::chrono::steady_clock::now();
    if(!g_waterFoamLastUpdateValid)
    {
        g_waterFoamLastUpdate = now;
        g_waterFoamLastUpdateValid = true;
    }
    const float elapsed = std::chrono::duration<float>(
        now - g_waterFoamLastUpdate).count();
    if(elapsed < WATER_FOAM_UPDATE_SECONDS)
    {
        return;
    }
    g_waterFoamLastUpdate = now;
    const float deltaSeconds = std::min(elapsed, 0.1f);

    const float targetX =
        ubo->playerX - 0.5f * WATER_FOAM_WORLD_SIZE;
    const float targetZ =
        ubo->playerZ - 0.5f * WATER_FOAM_WORLD_SIZE;
    const float snappedX =
        std::floor(targetX / WATER_FOAM_TEXEL_WORLD_SIZE) *
        WATER_FOAM_TEXEL_WORLD_SIZE;
    const float snappedZ =
        std::floor(targetZ / WATER_FOAM_TEXEL_WORLD_SIZE) *
        WATER_FOAM_TEXEL_WORLD_SIZE;

    int32_t scrollX = 0;
    int32_t scrollZ = 0;
    bool reset = !g_waterFoamOriginValid;
    if(!reset)
    {
        scrollX = static_cast<int32_t>(std::lround(
            (snappedX - g_waterFoamOriginX) / WATER_FOAM_TEXEL_WORLD_SIZE));
        scrollZ = static_cast<int32_t>(std::lround(
            (snappedZ - g_waterFoamOriginZ) / WATER_FOAM_TEXEL_WORLD_SIZE));
        if(std::abs(scrollX) >= static_cast<int32_t>(WATER_FOAM_RESOLUTION / 2u) ||
           std::abs(scrollZ) >= static_cast<int32_t>(WATER_FOAM_RESOLUTION / 2u))
        {
            scrollX = 0;
            scrollZ = 0;
            reset = true;
        }
    }
    g_waterFoamOriginX = snappedX;
    g_waterFoamOriginZ = snappedZ;
    g_waterFoamOriginValid = true;

    const uint32_t sourceLayer = g_waterFoamReadLayer;
    const uint32_t destinationLayer = sourceLayer ^ 1u;

    VkImageMemoryBarrier foamBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    foamBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    foamBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    foamBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    foamBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    foamBarrier.image = g_waterFoamImage;
    foamBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    foamBarrier.subresourceRange.baseMipLevel = 0u;
    foamBarrier.subresourceRange.levelCount = WATER_FOAM_MIPS;
    foamBarrier.subresourceRange.baseArrayLayer = 0u;
    foamBarrier.subresourceRange.layerCount = WATER_FOAM_LAYERS;
    foamBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    foamBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    VkImageMemoryBarrier injectionBarrier{foamBarrier};
    injectionBarrier.image = g_waterFoamInjectionImage;
    injectionBarrier.subresourceRange.levelCount = 1u;
    injectionBarrier.subresourceRange.layerCount = 1u;
    injectionBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    injectionBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

    VkImageMemoryBarrier inputs[2] = {foamBarrier, injectionBarrier};
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 2u, inputs);

    const bool simAvailable =
        ubo->waterSimEnabled > 0u &&
        g_waterSimView != VK_NULL_HANDLE;
    if(simAvailable)
    {
        VkImageMemoryBarrier simBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        simBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        simBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        simBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        simBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        simBarrier.image = g_waterSimImage;
        simBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        simBarrier.subresourceRange.baseMipLevel = 0u;
        simBarrier.subresourceRange.levelCount = 1u;
        simBarrier.subresourceRange.baseArrayLayer = 0u;
        simBarrier.subresourceRange.layerCount = 2u;
        simBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        simBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &simBarrier);
    }

    const bool fftAvailable =
        g_waterFftEnabled.load() && ubo->waterFftParams[1] > 0.0f;
    if(fftAvailable)
    {
        VkImageMemoryBarrier slopeBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        slopeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        slopeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        slopeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        slopeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        slopeBarrier.image = g_waterFftSlopeImage;
        slopeBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        slopeBarrier.subresourceRange.baseMipLevel = 0u;
        slopeBarrier.subresourceRange.levelCount = WATER_FFT_SLOPE_MIPS;
        slopeBarrier.subresourceRange.layerCount = 1u;
        slopeBarrier.srcAccessMask =
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        slopeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &slopeBarrier);
    }

    WaterFoamPushConstants push{};
    push.layersAndScroll[0] = static_cast<int32_t>(sourceLayer);
    push.layersAndScroll[1] = static_cast<int32_t>(destinationLayer);
    push.layersAndScroll[2] = scrollX;
    push.layersAndScroll[3] = scrollZ;
    push.timing[0] = deltaSeconds;
    push.timing[1] = simAvailable ? 1.0f : 0.0f;
    push.timing[2] = fftAvailable ? 1.0f : 0.0f;
    push.timing[3] = reset ? 1.0f : 0.0f;
    push.simState[0] = ubo->waterSimOriginX;
    push.simState[1] = ubo->waterSimOriginZ;
    push.simState[2] = ubo->waterSimTexelWorldSize;
    push.simState[3] = static_cast<float>(ubo->waterSimReadLayer);
    push.foamFrame[0] = g_waterFoamOriginX;
    push.foamFrame[1] = g_waterFoamOriginZ;
    push.foamFrame[2] = WATER_FOAM_TEXEL_WORLD_SIZE;
    push.foamFrame[3] = 1.0f;
    push.knobs[0] = 0.8f;
    push.knobs[1] = 1.0f;
    push.knobs[2] = 0.55f;
    push.knobs[3] = 1.0f;
    push.knobs2[0] = 2.0f;
    push.knobs2[1] = std::max(ubo->waterFftParams[0], 1.0f);
    push.knobs2[2] = kWATER_FFT_DETAIL_TILE;
    push.knobs2[3] = 4.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_waterFoamPipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_waterFoamPipelineLayout,
        0u, 1u, &g_waterFoamDescriptorSet, 0u, nullptr);
    vkCmdPushConstants(
        cmd,
        g_waterFoamPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0u,
        static_cast<uint32_t>(sizeof(push)),
        &push);
    vkCmdDispatch(
        cmd,
        (WATER_FOAM_RESOLUTION + WATER_FOAM_LOCAL_SIZE - 1u) /
            WATER_FOAM_LOCAL_SIZE,
        (WATER_FOAM_RESOLUTION + WATER_FOAM_LOCAL_SIZE - 1u) /
            WATER_FOAM_LOCAL_SIZE,
        1u);

    if(!g_waterFoamLoggedFirstDispatch)
    {
        RTLog("[WATER FOAM] first dispatch origin=("
            + std::to_string(g_waterFoamOriginX) + ","
            + std::to_string(g_waterFoamOriginZ) + ") texel="
            + std::to_string(WATER_FOAM_TEXEL_WORLD_SIZE)
            + " sim=" + std::to_string(simAvailable ? 1 : 0)
            + " fft=" + std::to_string(fftAvailable ? 1 : 0));
        g_waterFoamLoggedFirstDispatch = true;
    }

    VkImageMemoryBarrier preMip{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    preMip.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    preMip.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    preMip.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preMip.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preMip.image = g_waterFoamImage;
    preMip.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    preMip.subresourceRange.baseMipLevel = 0u;
    preMip.subresourceRange.levelCount = WATER_FOAM_MIPS;
    preMip.subresourceRange.baseArrayLayer = destinationLayer;
    preMip.subresourceRange.layerCount = 1u;
    preMip.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    preMip.dstAccessMask =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &preMip);

    for(uint32_t level = 1u; level < WATER_FOAM_MIPS; ++level)
    {
        const uint32_t srcSize = WATER_FOAM_RESOLUTION >> (level - 1u);
        const uint32_t dstSize = WATER_FOAM_RESOLUTION >> level;

        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1u;
        blit.srcSubresource.baseArrayLayer = destinationLayer;
        blit.srcSubresource.layerCount = 1u;
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(srcSize),
                              static_cast<int32_t>(srcSize), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.baseArrayLayer = destinationLayer;
        blit.dstSubresource.layerCount = 1u;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {static_cast<int32_t>(dstSize),
                              static_cast<int32_t>(dstSize), 1};
        vkCmdBlitImage(
            cmd,
            g_waterFoamImage, VK_IMAGE_LAYOUT_GENERAL,
            g_waterFoamImage, VK_IMAGE_LAYOUT_GENERAL,
            1u, &blit, VK_FILTER_LINEAR);

        VkImageMemoryBarrier midMip{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        midMip.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        midMip.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        midMip.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        midMip.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        midMip.image = g_waterFoamImage;
        midMip.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        midMip.subresourceRange.baseMipLevel = level;
        midMip.subresourceRange.levelCount = 1u;
        midMip.subresourceRange.baseArrayLayer = destinationLayer;
        midMip.subresourceRange.layerCount = 1u;
        midMip.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        midMip.dstAccessMask =
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &midMip);
    }

    VkImageMemoryBarrier consume[2]{};
    consume[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    consume[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    consume[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    consume[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    consume[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    consume[0].image = g_waterFoamImage;
    consume[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    consume[0].subresourceRange.baseMipLevel = 0u;
    consume[0].subresourceRange.levelCount = WATER_FOAM_MIPS;
    consume[0].subresourceRange.baseArrayLayer = 0u;
    consume[0].subresourceRange.layerCount = WATER_FOAM_LAYERS;
    consume[0].srcAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    consume[0].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    consume[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    consume[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    consume[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    consume[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    consume[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    consume[1].image = g_waterFoamInjectionImage;
    consume[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    consume[1].subresourceRange.levelCount = 1u;
    consume[1].subresourceRange.layerCount = 1u;
    consume[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    consume[1].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 2u, consume);

    g_waterFoamReadLayer = destinationLayer;
}
#endif
void createWaterSimulationResources()
{
    if(g_waterSimImage != VK_NULL_HANDLE)
    {
        return;
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(
        g_physicalDevice,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        &formatProperties
    );
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) !=
       requiredFeatures)
    {
        throw std::runtime_error(
            "VK_FORMAT_R16G16B16A16_SFLOAT cannot be sampled and stored for water simulation"
        );
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = { WATER_SIM_RESOLUTION, WATER_SIM_RESOLUTION, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 2u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(
        vkCreateImage(g_device, &imageInfo, nullptr, &g_waterSimImage),
        "water simulation create image failed"
    );

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_waterSimImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(
        g_physicalDevice,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );
    check(
        vkAllocateMemory(g_device, &allocation, nullptr, &g_waterSimMemory),
        "water simulation allocate image memory failed"
    );
    check(
        vkBindImageMemory(g_device, g_waterSimImage, g_waterSimMemory, 0),
        "water simulation bind image memory failed"
    );

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_waterSimImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0u;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.baseArrayLayer = 0u;
    viewInfo.subresourceRange.layerCount = 2u;
    check(
        vkCreateImageView(g_device, &viewInfo, nullptr, &g_waterSimView),
        "water simulation create image view failed"
    );

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    check(
        vkCreateSampler(g_device, &samplerInfo, nullptr, &g_waterSimSampler),
        "water simulation create sampler failed"
    );

    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 4u;
    layoutInfo.pBindings = bindings;
    check(
        vkCreateDescriptorSetLayout(
            g_device,
            &layoutInfo,
            nullptr,
            &g_waterSimDescriptorSetLayout
        ),
        "water simulation create descriptor layout failed"
    );

    VkDescriptorPoolSize poolSizes[4] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2u }
    };
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 4u;
    poolInfo.pPoolSizes = poolSizes;
    check(
        vkCreateDescriptorPool(
            g_device,
            &poolInfo,
            nullptr,
            &g_waterSimDescriptorPool
        ),
        "water simulation create descriptor pool failed"
    );

    VkDescriptorSetAllocateInfo allocationInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    allocationInfo.descriptorPool = g_waterSimDescriptorPool;
    allocationInfo.descriptorSetCount = 1u;
    allocationInfo.pSetLayouts = &g_waterSimDescriptorSetLayout;
    check(
        vkAllocateDescriptorSets(g_device, &allocationInfo, &g_waterSimDescriptorSet),
        "water simulation allocate descriptor set failed"
    );

    {
        VkBufferCreateInfo entityBufferInfo{
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
        };
        entityBufferInfo.size = sizeof(WaterSimEntityUBO);
        entityBufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        entityBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(
            vkCreateBuffer(
                g_device,
                &entityBufferInfo,
                nullptr,
                &g_waterSimEntityBuffer
            ),
            "water simulation create entity buffer failed"
        );
        VkMemoryRequirements entityRequirements{};
        vkGetBufferMemoryRequirements(
            g_device,
            g_waterSimEntityBuffer,
            &entityRequirements
        );
        VkMemoryAllocateInfo entityAllocation{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
        };
        entityAllocation.allocationSize = entityRequirements.size;
        entityAllocation.memoryTypeIndex = findMemoryType(
            g_physicalDevice,
            entityRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );
        check(
            vkAllocateMemory(
                g_device,
                &entityAllocation,
                nullptr,
                &g_waterSimEntityMemory
            ),
            "water simulation allocate entity memory failed"
        );
        check(
            vkBindBufferMemory(
                g_device,
                g_waterSimEntityBuffer,
                g_waterSimEntityMemory,
                0
            ),
            "water simulation bind entity memory failed"
        );
        check(
            vkMapMemory(
                g_device,
                g_waterSimEntityMemory,
                0,
                sizeof(WaterSimEntityUBO),
                0,
                &g_waterSimEntityMapped
            ),
            "water simulation map entity memory failed"
        );
        std::memset(g_waterSimEntityMapped, 0, sizeof(WaterSimEntityUBO));
    }

    VkDescriptorImageInfo sampledInfo{};
    sampledInfo.sampler = g_waterSimSampler;
    sampledInfo.imageView = g_waterSimView;
    sampledInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo storageInfo{};
    storageInfo.imageView = g_waterSimView;
    storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    VkDescriptorBufferInfo entityInfo{};
    entityInfo.buffer = g_waterSimEntityBuffer;
    entityInfo.range = sizeof(WaterSimEntityUBO);

    VkWriteDescriptorSet writes[4]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = g_waterSimDescriptorSet;
    writes[0].dstBinding = 0u;
    writes[0].descriptorCount = 1u;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &sampledInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = g_waterSimDescriptorSet;
    writes[1].dstBinding = 1u;
    writes[1].descriptorCount = 1u;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &storageInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = g_waterSimDescriptorSet;
    writes[2].dstBinding = 2u;
    writes[2].descriptorCount = 1u;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &cameraInfo;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = g_waterSimDescriptorSet;
    writes[3].dstBinding = 3u;
    writes[3].descriptorCount = 1u;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &entityInfo;
    vkUpdateDescriptorSets(g_device, 4u, writes, 0u, nullptr);


    g_waterSimLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_waterSimNeedsClear = true;
    updateWaterSimulationRayDescriptor();
}
void createWaterSimulationPipeline()
{
    if(g_waterSimPipeline != VK_NULL_HANDLE)
    {
        return;
    }

    createWaterSimulationResources();
    const std::vector<uint8_t> code =
        loadSpv("shaders/water_sim.comp.spv");
    VkShaderModule shaderModule = createShaderModule(g_device, code);

    try
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.size = sizeof(WaterSimPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        layoutInfo.setLayoutCount = 1u;
        layoutInfo.pSetLayouts = &g_waterSimDescriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1u;
        layoutInfo.pPushConstantRanges = &pushRange;
        check(
            vkCreatePipelineLayout(
                g_device,
                &layoutInfo,
                nullptr,
                &g_waterSimPipelineLayout
            ),
            "water simulation create pipeline layout failed"
        );

        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shaderModule;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        pipelineInfo.stage = stage;
        pipelineInfo.layout = g_waterSimPipelineLayout;
        check(
            vkCreateComputePipelines(
                g_device,
                getSharedPipelineCache(),
                1u,
                &pipelineInfo,
                nullptr,
                &g_waterSimPipeline
            ),
            "water simulation create pipeline failed"
        );
    }
    catch(...)
    {
        if(g_waterSimPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(g_device, g_waterSimPipelineLayout, nullptr);
            g_waterSimPipelineLayout = VK_NULL_HANDLE;
        }
        vkDestroyShaderModule(g_device, shaderModule, nullptr);
        throw;
    }

    vkDestroyShaderModule(g_device, shaderModule, nullptr);
    RTLog("[WATER SIM] 512x512 RGBA16F shallow-water compute pipeline created");
}
void recordWaterSimulation(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE ||
       g_waterSimPipeline == VK_NULL_HANDLE ||
       g_waterSimDescriptorSet == VK_NULL_HANDLE ||
       g_cameraUboMapped == nullptr)
    {
        return;
    }

    CameraUBO* ubo = static_cast<CameraUBO*>(g_cameraUboMapped);
    const bool playerInWater = ubo->playerInWater > 0.5f;
    const bool windKeepsSimAlive = waterWindStrength() > 0.0f;
    if(!windKeepsSimAlive && !playerInWater && !g_waterSimPendingImpulse &&
       g_waterSimKeepAliveSeconds <= 0.0f)
    {
        if(!g_waterSimLoggedAwaitingWater)
        {
            RTLog("[WATER SIM] idle: awaiting player water entry");
            g_waterSimLoggedAwaitingWater = true;
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if(!g_waterSimLastUpdateValid)
    {
        g_waterSimLastUpdate = now;
        g_waterSimLastUpdateValid = true;
    }
    const float elapsed = std::chrono::duration<float>(
        now - g_waterSimLastUpdate
    ).count();
    if(elapsed < waterSimUpdateSeconds())
    {
        return;
    }
    g_waterSimLastUpdate = now;
    const float deltaSeconds = std::min(elapsed, 0.1f);

    if(playerInWater || g_waterSimPendingImpulse)
    {
        g_waterSimKeepAliveSeconds = 8.0f;
    }
    else
    {
        g_waterSimKeepAliveSeconds = std::max(
            0.0f,
            g_waterSimKeepAliveSeconds - deltaSeconds
        );
    }

    const float nextOriginX = ubo->playerX - 0.5f * WATER_SIM_WORLD_SIZE;
    const float nextOriginZ = ubo->playerZ - 0.5f * WATER_SIM_WORLD_SIZE;
    int32_t scrollX = 0;
    int32_t scrollZ = 0;
    if(!g_waterSimOriginValid)
    {
        g_waterSimOriginX = nextOriginX;
        g_waterSimOriginZ = nextOriginZ;
        g_waterSimOriginValid = true;
        g_waterSimNeedsClear = true;
    }
    else
    {
        scrollX = static_cast<int32_t>(std::lround(
            (nextOriginX - g_waterSimOriginX) / WATER_SIM_TEXEL_WORLD_SIZE
        ));
        scrollZ = static_cast<int32_t>(std::lround(
            (nextOriginZ - g_waterSimOriginZ) / WATER_SIM_TEXEL_WORLD_SIZE
        ));
        if(std::abs(scrollX) >= static_cast<int32_t>(WATER_SIM_RESOLUTION / 2u) ||
           std::abs(scrollZ) >= static_cast<int32_t>(WATER_SIM_RESOLUTION / 2u))
        {
            scrollX = 0;
            scrollZ = 0;
            g_waterSimNeedsClear = true;
        }
        g_waterSimOriginX = nextOriginX;
        g_waterSimOriginZ = nextOriginZ;
    }

    ubo->waterSurfaceY = g_waterSimLastSurfaceY;
    ubo->waterSimOriginX = g_waterSimOriginX;
    ubo->waterSimOriginZ = g_waterSimOriginZ;
    ubo->waterSimTexelWorldSize = WATER_SIM_TEXEL_WORLD_SIZE;
    ubo->waterSimReadLayer = g_waterSimReadLayer;
    ubo->waterSimEnabled =
        (playerInWater || g_waterSimPendingImpulse ||
         g_waterSimKeepAliveSeconds > 0.0f ||
         waterWindStrength() > 0.0f) ? 1u : 0u;

    VkImageMemoryBarrier imageBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imageBarrier.image = g_waterSimImage;
    imageBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.baseMipLevel = 0u;
    imageBarrier.subresourceRange.levelCount = 1u;
    imageBarrier.subresourceRange.baseArrayLayer = 0u;
    imageBarrier.subresourceRange.layerCount = 2u;

    if(g_waterSimLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageBarrier.srcAccessMask = 0u;
        imageBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT |
                                     VK_ACCESS_SHADER_READ_BIT |
                                     VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr, 0u, nullptr, 1u, &imageBarrier
        );
        g_waterSimLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    if(g_waterSimNeedsClear)
    {
        VkClearColorValue clearValue{};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1u;
        range.layerCount = 2u;
        vkCmdClearColorImage(
            cmd,
            g_waterSimImage,
            VK_IMAGE_LAYOUT_GENERAL,
            &clearValue,
            1u,
            &range
        );
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                     VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr, 0u, nullptr, 1u, &imageBarrier
        );
        g_waterSimNeedsClear = false;
    }
    else
    {
        imageBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                     VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr, 0u, nullptr, 1u, &imageBarrier
        );
    }

    WaterSimPushConstants push{};
    push.sourceLayer = g_waterSimReadLayer;
    push.destinationLayer = g_waterSimReadLayer ^ 1u;
    push.hasImpulse = g_waterSimPendingImpulse ? 1u : 0u;
    push.playerInWater = playerInWater ? 1u : 0u;
    push.deltaSeconds = deltaSeconds;
    push.impulseX = g_waterSimPendingImpulseX;
    push.impulseZ = g_waterSimPendingImpulseZ;
    push.impulseStrength = g_waterSimPendingImpulseStrength;
    push.scrollX = scrollX;
    push.scrollZ = scrollZ;
    push.waveSpongeWidth = g_waterSimSpongeWidth.load();
    push.waveReservedObstacle = waterWindStrength();
    push.waveShapeScale = g_waterWaveLengthScale.load();
    push.waveShapeAngle = g_waterWaveDirection.load();
    push.waveShapeCrest = g_waterWaveCrestSharpness.load();
    push.waveShapeWarp = g_waterWaveWarpStrength.load();
    push.waveGaussianSource = g_waterSimGaussianSource.load();
    push.waveDampingScale = g_waterSimDampingScale.load();
    push.waveSpeed = g_waterSimPropagationSpeed.load();

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_waterSimPipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_waterSimPipelineLayout,
        0u,
        1u,
        &g_waterSimDescriptorSet,
        0u,
        nullptr
    );
    if(g_waterSimEntityMapped != nullptr)
    {
        WaterSimEntityUBO* entities =
            static_cast<WaterSimEntityUBO*>(g_waterSimEntityMapped);
        std::lock_guard<std::mutex> guard(g_waterSimEntityMutex);
        int32_t impulseCount = 0;
        while(!g_waterSimImpulseQueue.empty() &&
              impulseCount < static_cast<int32_t>(WATER_SIM_MAX_ENTITIES))
        {
            const WaterSimImpulseEvent event = g_waterSimImpulseQueue.front();
            g_waterSimImpulseQueue.pop_front();
            entities->impulses[impulseCount][0] = event.x;
            entities->impulses[impulseCount][1] = event.z;
            entities->impulses[impulseCount][2] = event.strength;
            entities->impulses[impulseCount][3] = 0.0f;
            ++impulseCount;
        }
        entities->counts[0] = impulseCount;
        entities->counts[1] = g_waterSimBoatCount;
        entities->counts[2] = 0;
        entities->counts[3] = 0;
        for(int32_t boat = 0; boat < g_waterSimBoatCount; ++boat)
        {
            const int32_t source = boat * 5;
            entities->boats[boat][0] = g_waterSimBoatData[source + 0];
            entities->boats[boat][1] = g_waterSimBoatData[source + 1];
            entities->boats[boat][2] = g_waterSimBoatData[source + 2];
            entities->boats[boat][3] = g_waterSimBoatData[source + 3];
            entities->boatParams[boat][0] = g_waterSimBoatData[source + 4];
            entities->boatParams[boat][1] = 0.0f;
            entities->boatParams[boat][2] = 0.0f;
            entities->boatParams[boat][3] = 0.0f;
        }
    }

    push.padding0 = 0u;
    vkCmdPushConstants(
        cmd,
        g_waterSimPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0u,
        sizeof(WaterSimPushConstants),
        &push
    );
    vkCmdDispatch(
        cmd,
        (WATER_SIM_RESOLUTION + 15u) / 16u,
        (WATER_SIM_RESOLUTION + 15u) / 16u,
        1u
    );

    if(!g_waterSimLoggedFirstDispatch)
    {
        RTLog(
            "[WATER SIM] first dispatch"
            " playerInWater=" + std::to_string(playerInWater ? 1 : 0) +
            " impulse=" + std::to_string(g_waterSimPendingImpulse ? 1 : 0) +
            " surfaceY=" + std::to_string(g_waterSimLastSurfaceY)
        );
        g_waterSimLoggedFirstDispatch = true;
    }

    imageBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u,
        0u, nullptr, 0u, nullptr, 1u, &imageBarrier
    );

    g_waterSimReadLayer = push.destinationLayer;
    ubo->waterSimReadLayer = g_waterSimReadLayer;

    if(!g_waterSimLoggedSampleContract)
    {
        std::ostringstream log;
        log
            << "[WATER SIM] sample contract"
            << " enabled=" << ubo->waterSimEnabled
            << " readLayer=" << g_waterSimReadLayer
            << " origin=(" << ubo->waterSimOriginX
            << "," << ubo->waterSimOriginZ << ")"
            << " texel=" << ubo->waterSimTexelWorldSize
            << " surfaceY=" << ubo->waterSurfaceY;
        RTLog(log.str());
        g_waterSimLoggedSampleContract = true;
    }

    g_waterSimPendingImpulse = false;
}
void recordWaterCaustics(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE ||
       g_waterCausticPipeline == VK_NULL_HANDLE ||
       g_waterCausticDescriptorSet == VK_NULL_HANDLE ||
       g_cameraUboMapped == nullptr)
    {
        return;
    }

    CameraUBO* ubo = static_cast<CameraUBO*>(g_cameraUboMapped);
    const auto now = std::chrono::steady_clock::now();
    if(g_waterCausticLastUpdateValid &&
       std::chrono::duration<float>(now - g_waterCausticLastUpdate).count() <
           WATER_CAUSTIC_UPDATE_SECONDS)
    {
        return;
    }
    g_waterCausticLastUpdate = now;
    g_waterCausticLastUpdateValid = true;

    if(ubo->weatherParams[2] > 1.5f)
    {
        return;
    }
    const bool playerInWater = ubo->playerInWater > 0.5f;
    const float surfaceDelta = ubo->playerY - ubo->waterSurfaceY;
    if(!playerInWater &&
       g_waterSimKeepAliveSeconds <= 0.0f &&
       std::abs(surfaceDelta) > WATER_CAUSTIC_NEAR_WATER_Y_RANGE)
    {
        return;
    }

    if(ubo->waterSimEnabled == 0u)
    {
        g_waterSimOriginX = ubo->playerX - 0.5f * WATER_SIM_WORLD_SIZE;
        g_waterSimOriginZ = ubo->playerZ - 0.5f * WATER_SIM_WORLD_SIZE;
        g_waterSimOriginValid = true;
        ubo->waterSimOriginX = g_waterSimOriginX;
        ubo->waterSimOriginZ = g_waterSimOriginZ;
        ubo->waterSimTexelWorldSize = WATER_SIM_TEXEL_WORLD_SIZE;
    }

    VkImageMemoryBarrier causticBarrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
    };
    causticBarrier.image = g_waterCausticImage;
    causticBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    causticBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    causticBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    causticBarrier.subresourceRange.baseMipLevel = 0u;
    causticBarrier.subresourceRange.levelCount = 1u;
    causticBarrier.subresourceRange.baseArrayLayer = 0u;
    causticBarrier.subresourceRange.layerCount = 1u;

    if(g_waterCausticLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        causticBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        causticBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        causticBarrier.srcAccessMask = 0u;
        causticBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr, 0u, nullptr, 1u, &causticBarrier
        );
        g_waterCausticLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    else
    {
        causticBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        causticBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        causticBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        causticBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr, 0u, nullptr, 1u, &causticBarrier
        );
    }

    VkImageMemoryBarrier stateBarrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
    };
    stateBarrier.image = g_waterSimImage;
    stateBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    stateBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    stateBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    stateBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    stateBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                 VK_ACCESS_SHADER_WRITE_BIT;
    stateBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    stateBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    stateBarrier.subresourceRange.baseMipLevel = 0u;
    stateBarrier.subresourceRange.levelCount = 1u;
    stateBarrier.subresourceRange.baseArrayLayer = 0u;
    stateBarrier.subresourceRange.layerCount = 2u;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u,
        0u, nullptr, 0u, nullptr, 1u, &stateBarrier
    );

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_waterCausticPipeline);
    vkCmdBindDescriptorSets(
        cmd,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        g_waterCausticPipelineLayout,
        0u,
        1u,
        &g_waterCausticDescriptorSet,
        0u,
        nullptr
    );
    vkCmdDispatch(
        cmd,
        (WATER_CAUSTIC_RESOLUTION + 15u) / 16u,
        (WATER_CAUSTIC_RESOLUTION + 15u) / 16u,
        1u
    );

    causticBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    causticBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    causticBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    causticBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u,
        0u, nullptr, 0u, nullptr, 1u, &causticBarrier
    );
}
void destroyWaterCausticResources()
{
    if(g_waterCausticPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_waterCausticPipeline, nullptr);
        g_waterCausticPipeline = VK_NULL_HANDLE;
    }
    if(g_waterCausticPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_waterCausticPipelineLayout, nullptr);
        g_waterCausticPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_waterCausticDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_waterCausticDescriptorPool, nullptr);
        g_waterCausticDescriptorPool = VK_NULL_HANDLE;
        g_waterCausticDescriptorSet = VK_NULL_HANDLE;
    }
    if(g_waterCausticDescriptorSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(
            g_device,
            g_waterCausticDescriptorSetLayout,
            nullptr
        );
        g_waterCausticDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if(g_waterCausticSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(g_device, g_waterCausticSampler, nullptr);
        g_waterCausticSampler = VK_NULL_HANDLE;
    }
    if(g_waterCausticView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterCausticView, nullptr);
        g_waterCausticView = VK_NULL_HANDLE;
    }
    if(g_waterCausticImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(g_device, g_waterCausticImage, nullptr);
        g_waterCausticImage = VK_NULL_HANDLE;
    }
    if(g_waterCausticMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_waterCausticMemory, nullptr);
        g_waterCausticMemory = VK_NULL_HANDLE;
    }
    g_waterCausticLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_waterCausticLastUpdateValid = false;
}
void destroyWaterSimulationResources()
{
    if(g_waterSimEntityMapped != nullptr)
    {
        vkUnmapMemory(g_device, g_waterSimEntityMemory);
        g_waterSimEntityMapped = nullptr;
    }
    if(g_waterSimEntityBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_waterSimEntityBuffer, nullptr);
        g_waterSimEntityBuffer = VK_NULL_HANDLE;
    }
    if(g_waterSimEntityMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_waterSimEntityMemory, nullptr);
        g_waterSimEntityMemory = VK_NULL_HANDLE;
    }
    if(g_waterSimPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_waterSimPipeline, nullptr);
        g_waterSimPipeline = VK_NULL_HANDLE;
    }
    if(g_waterSimPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_waterSimPipelineLayout, nullptr);
        g_waterSimPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_waterSimDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_waterSimDescriptorPool, nullptr);
        g_waterSimDescriptorPool = VK_NULL_HANDLE;
        g_waterSimDescriptorSet = VK_NULL_HANDLE;
    }
    if(g_waterSimDescriptorSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(
            g_device,
            g_waterSimDescriptorSetLayout,
            nullptr
        );
        g_waterSimDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if(g_waterSimSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(g_device, g_waterSimSampler, nullptr);
        g_waterSimSampler = VK_NULL_HANDLE;
    }
    if(g_waterSimView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_waterSimView, nullptr);
        g_waterSimView = VK_NULL_HANDLE;
    }
    if(g_waterSimImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(g_device, g_waterSimImage, nullptr);
        g_waterSimImage = VK_NULL_HANDLE;
    }
    if(g_waterSimMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_waterSimMemory, nullptr);
        g_waterSimMemory = VK_NULL_HANDLE;
    }

    g_waterSimLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_waterSimReadLayer = 0u;
    g_waterSimNeedsClear = true;
    g_waterSimPlayerStateValid = false;
    g_waterSimPlayerWasInWater = false;
    g_waterSimPendingImpulse = false;
    g_waterSimOriginValid = false;
    g_waterSimLastUpdateValid = false;
    g_waterSimKeepAliveSeconds = 0.0f;
    g_waterSimLoggedAwaitingWater = false;
    g_waterSimLoggedFirstDispatch = false;
    g_waterSimLoggedSampleContract = false;
}

extern "C"
// 2026-10-17 实体交互波：掉落物/船的入水（离水）一次性脉冲。只入队，不直接碰
// UBO —— 30 Hz tick 在 recordWaterSimulation 里统一消费，避免与渲染线程竞态。
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_queueWaterImpulse(
    JNIEnv* env,
    jclass clazz,
    jfloat x,
    jfloat z,
    jfloat strength)
{
    std::lock_guard<std::mutex> guard(g_waterSimEntityMutex);
    if(g_waterSimImpulseQueue.size() >= WATER_SIM_MAX_QUEUED_IMPULSES)
    {
        g_waterSimImpulseQueue.pop_front();
    }
    g_waterSimImpulseQueue.push_back({x, z, strength});
}

extern "C"
// 2026-10-17 实体交互波：船的持续移动波源列表，每帧整批替换。
// data 布局：每条船 5 个 float（x, z, vx, vz, 幅值倍率），count ≤ 8。
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateWaterBoats(
    JNIEnv* env,
    jclass clazz,
    jfloatArray data,
    jint count)
{
    const int32_t clamped = std::clamp(
        static_cast<int32_t>(count),
        0,
        static_cast<int32_t>(WATER_SIM_MAX_ENTITIES)
    );
    std::lock_guard<std::mutex> guard(g_waterSimEntityMutex);
    g_waterSimBoatCount = clamped;
    if(clamped > 0)
    {
        env->GetFloatArrayRegion(
            data,
            0,
            clamped * 5,
            g_waterSimBoatData
        );
    }
}
