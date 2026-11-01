// Orion backend — Nrd 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

VkFormat nrdFormatToVkFormat(nrd::Format format)
{
    switch(format)
    {
        case nrd::Format::R8_UNORM:              return VK_FORMAT_R8_UNORM;
        case nrd::Format::R8_SNORM:              return VK_FORMAT_R8_SNORM;
        case nrd::Format::R8_UINT:               return VK_FORMAT_R8_UINT;
        case nrd::Format::R8_SINT:               return VK_FORMAT_R8_SINT;
        case nrd::Format::RG8_UNORM:             return VK_FORMAT_R8G8_UNORM;
        case nrd::Format::RG8_SNORM:             return VK_FORMAT_R8G8_SNORM;
        case nrd::Format::RG8_UINT:              return VK_FORMAT_R8G8_UINT;
        case nrd::Format::RG8_SINT:              return VK_FORMAT_R8G8_SINT;
        case nrd::Format::RGBA8_UNORM:           return VK_FORMAT_R8G8B8A8_UNORM;
        case nrd::Format::RGBA8_SNORM:           return VK_FORMAT_R8G8B8A8_SNORM;
        case nrd::Format::RGBA8_UINT:            return VK_FORMAT_R8G8B8A8_UINT;
        case nrd::Format::RGBA8_SINT:            return VK_FORMAT_R8G8B8A8_SINT;
        case nrd::Format::RGBA8_SRGB:            return VK_FORMAT_R8G8B8A8_SRGB;
        case nrd::Format::R16_UNORM:             return VK_FORMAT_R16_UNORM;
        case nrd::Format::R16_SNORM:             return VK_FORMAT_R16_SNORM;
        case nrd::Format::R16_UINT:              return VK_FORMAT_R16_UINT;
        case nrd::Format::R16_SINT:              return VK_FORMAT_R16_SINT;
        case nrd::Format::R16_SFLOAT:            return VK_FORMAT_R16_SFLOAT;
        case nrd::Format::RG16_UNORM:            return VK_FORMAT_R16G16_UNORM;
        case nrd::Format::RG16_SNORM:            return VK_FORMAT_R16G16_SNORM;
        case nrd::Format::RG16_UINT:             return VK_FORMAT_R16G16_UINT;
        case nrd::Format::RG16_SINT:             return VK_FORMAT_R16G16_SINT;
        case nrd::Format::RG16_SFLOAT:           return VK_FORMAT_R16G16_SFLOAT;
        case nrd::Format::RGBA16_UNORM:          return VK_FORMAT_R16G16B16A16_UNORM;
        case nrd::Format::RGBA16_SNORM:          return VK_FORMAT_R16G16B16A16_SNORM;
        case nrd::Format::RGBA16_UINT:           return VK_FORMAT_R16G16B16A16_UINT;
        case nrd::Format::RGBA16_SINT:           return VK_FORMAT_R16G16B16A16_SINT;
        case nrd::Format::RGBA16_SFLOAT:         return VK_FORMAT_R16G16B16A16_SFLOAT;
        case nrd::Format::R32_UINT:              return VK_FORMAT_R32_UINT;
        case nrd::Format::R32_SINT:              return VK_FORMAT_R32_SINT;
        case nrd::Format::R32_SFLOAT:            return VK_FORMAT_R32_SFLOAT;
        case nrd::Format::RG32_UINT:             return VK_FORMAT_R32G32_UINT;
        case nrd::Format::RG32_SINT:             return VK_FORMAT_R32G32_SINT;
        case nrd::Format::RG32_SFLOAT:           return VK_FORMAT_R32G32_SFLOAT;
        case nrd::Format::RGB32_UINT:            return VK_FORMAT_R32G32B32A32_UINT;
        case nrd::Format::RGB32_SINT:            return VK_FORMAT_R32G32B32A32_SINT;
        case nrd::Format::RGB32_SFLOAT:          return VK_FORMAT_R32G32B32A32_SFLOAT;
        case nrd::Format::RGBA32_UINT:           return VK_FORMAT_R32G32B32A32_UINT;
        case nrd::Format::RGBA32_SINT:           return VK_FORMAT_R32G32B32A32_SINT;
        case nrd::Format::RGBA32_SFLOAT:         return VK_FORMAT_R32G32B32A32_SFLOAT;
        case nrd::Format::R10_G10_B10_A2_UNORM:  return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case nrd::Format::R10_G10_B10_A2_UINT:   return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case nrd::Format::R11_G11_B10_UFLOAT:    return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case nrd::Format::R9_G9_B9_E5_UFLOAT:    return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        default:                                 break;
    }

    return VK_FORMAT_UNDEFINED;
}
void nrdDestroyImage(
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout)
{
    if(view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, view, nullptr);
        view = VK_NULL_HANDLE;
    }

    if(image != VK_NULL_HANDLE)
    {
        vkDestroyImage(g_device, image, nullptr);
        image = VK_NULL_HANDLE;
    }

    if(memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }

    layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// NRD pool textures and the app-side inputs/outputs are all read and written as
// untyped storage images, so every one of them is created with STORAGE +
// SAMPLED and lives in GENERAL for its whole life.  That also matches how the
// rest of this backend handles long-lived images (see g_rtOutputImage), so no
// new layout-tracking rules are introduced.
bool nrdCreateImage2D(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout,
    const char* what)
{
    if(width == 0u || height == 0u)
    {
        RTLog(std::string("[NRD-1 CREATE] refused zero-size image for ") + what);
        return false;
    }

    if(format == VK_FORMAT_UNDEFINED)
    {
        RTLog(std::string("[NRD-1 CREATE] no Vulkan format for ") + what);
        return false;
    }

    VkFormatProperties formatProps{};
    vkGetPhysicalDeviceFormatProperties(g_physicalDevice, format, &formatProps);

    if((formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0u)
    {
        RTLog(
            std::string("[NRD-1 CREATE] format lacks STORAGE_IMAGE support for ") +
            what + " vkFormat=" + std::to_string(static_cast<int>(format))
        );
        return false;
    }

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1u};
    ci.mipLevels = 1u;
    ci.arrayLayers = 1u;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if(vkCreateImage(g_device, &ci, nullptr, &image) != VK_SUCCESS)
    {
        image = VK_NULL_HANDLE;
        RTLog(std::string("[NRD-1 CREATE] vkCreateImage failed for ") + what);
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, image, &requirements);

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(
        g_physicalDevice,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    if(vkAllocateMemory(g_device, &allocation, nullptr, &memory) != VK_SUCCESS)
    {
        memory = VK_NULL_HANDLE;
        nrdDestroyImage(image, memory, view, layout);
        RTLog(std::string("[NRD-1 CREATE] vkAllocateMemory failed for ") + what);
        return false;
    }

    if(vkBindImageMemory(g_device, image, memory, 0) != VK_SUCCESS)
    {
        nrdDestroyImage(image, memory, view, layout);
        RTLog(std::string("[NRD-1 CREATE] vkBindImageMemory failed for ") + what);
        return false;
    }

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0u;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.baseArrayLayer = 0u;
    viewInfo.subresourceRange.layerCount = 1u;

    if(vkCreateImageView(g_device, &viewInfo, nullptr, &view) != VK_SUCCESS)
    {
        view = VK_NULL_HANDLE;
        nrdDestroyImage(image, memory, view, layout);
        RTLog(std::string("[NRD-1 CREATE] vkCreateImageView failed for ") + what);
        return false;
    }

    layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

// The two immutable samplers NRD asks for (Sampler::NEAREST_CLAMP and
// Sampler::LINEAR_CLAMP).  Address mode is CLAMP_TO_EDGE and mip 0 is the only
// level, exactly as NRD's own integration layer sets them up.
VkSampler nrdCreateImmutableSampler(bool linear, const char* what)
{
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.minLod = 0.0f;
    ci.maxLod = 0.0f;
    ci.maxAnisotropy = 1.0f;
    ci.compareOp = VK_COMPARE_OP_NEVER;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;

    VkSampler sampler = VK_NULL_HANDLE;

    if(vkCreateSampler(g_device, &ci, nullptr, &sampler) != VK_SUCCESS)
    {
        RTLog(std::string("[NRD-1 CREATE] sampler creation failed for ") + what);
        return VK_NULL_HANDLE;
    }

    return sampler;
}

// Every image NRD touches is used as a GENERAL storage/sampled image, but
// nrdCreateImage2D leaves each one at VK_IMAGE_LAYOUT_UNDEFINED and nothing
// transitions them afterwards (the tracked layouts are only read at destroy).
// Describing an image as GENERAL while it was never transitioned into it makes
// the very first access undefined -- and that first access is the RayGen's
// store into IN_VIEWZ / IN_NORMAL_ROUGHNESS / IN_DIFF, i.e. the only writer the
// whole denoiser depends on.  The water-sim and caustic images in this file do
// the UNDEFINED -> GENERAL step explicitly for the same reason, so do it here
// once per NRD (re)creation.
void nrdTransitionImagesToGeneral()
{
    std::vector<VkImage> images;

    const VkImage appImages[7] =
    {
        g_nrdViewZImage,
        g_nrdNormalRoughnessImage,
        g_nrdMotionImage,
        g_nrdDiffRadianceHitDistImage,
        g_nrdOutDiffRadianceHitDistImage,
        g_nrdValidationImage,
        g_nrdMvProbeImage
    };

    for(VkImage image : appImages)
    {
        if(image != VK_NULL_HANDLE)
        {
            images.push_back(image);
        }
    }

    for(VkImage image : g_nrdPermanentImages)
    {
        if(image != VK_NULL_HANDLE)
        {
            images.push_back(image);
        }
    }

    for(VkImage image : g_nrdTransientImages)
    {
        if(image != VK_NULL_HANDLE)
        {
            images.push_back(image);
        }
    }

    if(images.empty() || g_rtCommandPool == VK_NULL_HANDLE)
    {
        return;
    }

    std::vector<VkImageMemoryBarrier> barriers(images.size());

    for(size_t i = 0u; i < images.size(); ++i)
    {
        VkImageMemoryBarrier& barrier = barriers[i];
        barrier = VkImageMemoryBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0u;
        barrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[i];
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0u;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.baseArrayLayer = 0u;
        barrier.subresourceRange.layerCount = 1u;
    }

    VkCommandBufferAllocateInfo allocation{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };
    allocation.commandPool = g_rtCommandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1u;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    if(vkAllocateCommandBuffers(g_device, &allocation, &commandBuffer) != VK_SUCCESS)
    {
        RTLog("[NRD-1 CREATE] layout transition command buffer failed");
        return;
    }

    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    check(
        vkBeginCommandBuffer(commandBuffer, &beginInfo),
        "NRD layout transition begin failed"
    );

    vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u,
        0u, nullptr,
        0u, nullptr,
        static_cast<uint32_t>(barriers.size()),
        barriers.data()
    );

    if(g_nrdMotionImage != VK_NULL_HANDLE)
    {
        VkClearColorValue motionClear{};
        motionClear.float32[0] = 0.0f;
        motionClear.float32[1] = 0.0f;
        motionClear.float32[2] = 0.0f;
        motionClear.float32[3] = 0.0f;

        VkImageSubresourceRange motionRange{};
        motionRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        motionRange.baseMipLevel = 0u;
        motionRange.levelCount = 1u;
        motionRange.baseArrayLayer = 0u;
        motionRange.layerCount = 1u;

        vkCmdClearColorImage(
            commandBuffer,
            g_nrdMotionImage,
            VK_IMAGE_LAYOUT_GENERAL,
            &motionClear,
            1u,
            &motionRange
        );

        VkImageMemoryBarrier motionReady{
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER
        };
        motionReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        motionReady.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        motionReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        motionReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        motionReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        motionReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        motionReady.image = g_nrdMotionImage;
        motionReady.subresourceRange = motionRange;

        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr,
            0u, nullptr,
            1u, &motionReady
        );
    }

    check(
        vkEndCommandBuffer(commandBuffer),
        "NRD layout transition end failed"
    );

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;

    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "NRD layout transition submit failed");
    check(vkQueueWaitIdle(g_queue), "NRD layout transition wait failed");

    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);

    g_nrdViewZLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_nrdNormalRoughnessLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_nrdMotionLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_nrdDiffRadianceHitDistLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_nrdOutDiffRadianceHitDistLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_nrdMvProbeLayout = VK_IMAGE_LAYOUT_GENERAL;

    for(VkImageLayout& layout : g_nrdPermanentLayouts)
    {
        layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    for(VkImageLayout& layout : g_nrdTransientLayouts)
    {
        layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    RTLog(
        std::string("[NRD-1 CREATE] transitioned ") +
        std::to_string(images.size()) +
        " NRD images UNDEFINED -> GENERAL"
    );
}

// Builds the one shared descriptor set layout NRD's pipelines were compiled
// against.  Binding numbers come from NRD's SPIRV_*REG_OFFSET build flags:
//   set 0 : sampled images from textureOffset (20), storage images from
//           storageTextureAndBufferOffset (3)
//   set 1 : samplers from samplerOffset (0), constant buffer at
//           constantBufferOffset (2) + constantBufferRegisterIndex (0)
bool createNRDDescriptorSetLayout()
{
    if(g_nrdDescriptorSetLayout != VK_NULL_HANDLE)
    {
        return true;
    }

    const uint32_t textureBase =
        g_nrdLibraryDesc.spirvBindingOffsets.textureOffset +
        g_nrdInstanceDesc.resourcesBaseRegisterIndex;

    const uint32_t storageBase =
        g_nrdLibraryDesc.spirvBindingOffsets.storageTextureAndBufferOffset +
        g_nrdInstanceDesc.resourcesBaseRegisterIndex;

    const uint32_t samplerBase =
        g_nrdLibraryDesc.spirvBindingOffsets.samplerOffset +
        g_nrdInstanceDesc.samplersBaseRegisterIndex;

    const uint32_t constantBufferBinding =
        g_nrdLibraryDesc.spirvBindingOffsets.constantBufferOffset +
        g_nrdInstanceDesc.constantBufferRegisterIndex;

    const uint32_t textureCount =
        std::max(1u, g_nrdInstanceDesc.descriptorPoolDesc.perSetTexturesMaxNum);

    const uint32_t storageCount =
        std::max(1u, g_nrdInstanceDesc.descriptorPoolDesc.perSetStorageTexturesMaxNum);

    if(storageBase + storageCount > textureBase)
    {
        RTLog(
            std::string("[NRD-1 CREATE] NRD descriptor ranges overlap: storage ") +
            std::to_string(storageBase) + "+" + std::to_string(storageCount) +
            " vs textures from " + std::to_string(textureBase)
        );
        return false;
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(
        textureCount + storageCount + 3u
    );

    for(uint32_t i = 0u; i < textureCount; ++i)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = textureBase + i;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        binding.descriptorCount = 1u;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings.push_back(binding);
    }

    for(uint32_t i = 0u; i < storageCount; ++i)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = storageBase + i;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binding.descriptorCount = 1u;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings.push_back(binding);
    }

    VkSampler immutableSamplers[static_cast<size_t>(nrd::Sampler::MAX_NUM)] =
    {
        g_nrdSamplerNearest,
        g_nrdSamplerLinear
    };

    std::vector<VkDescriptorSetLayoutBinding> constantBindings;
    constantBindings.reserve(g_nrdInstanceDesc.samplersNum + 1u);

    for(uint32_t i = 0u; i < g_nrdInstanceDesc.samplersNum; ++i)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = samplerBase + i;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        binding.descriptorCount = 1u;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        binding.pImmutableSamplers = &immutableSamplers[i];
        constantBindings.push_back(binding);
    }

    VkDescriptorSetLayoutBinding constantBufferBindingInfo{};
    constantBufferBindingInfo.binding = constantBufferBinding;
    constantBufferBindingInfo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    constantBufferBindingInfo.descriptorCount = 1u;
    constantBufferBindingInfo.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    constantBindings.push_back(constantBufferBindingInfo);

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr, &g_nrdDescriptorSetLayout) != VK_SUCCESS)
    {
        g_nrdDescriptorSetLayout = VK_NULL_HANDLE;
        RTLog("[NRD-1 CREATE] NRD descriptor set layout failed");
        return false;
    }

    VkDescriptorSetLayoutCreateInfo constantLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    constantLayoutInfo.bindingCount = static_cast<uint32_t>(constantBindings.size());
    constantLayoutInfo.pBindings = constantBindings.data();

    if(vkCreateDescriptorSetLayout(g_device, &constantLayoutInfo, nullptr, &g_nrdConstantSetLayout) != VK_SUCCESS)
    {
        g_nrdConstantSetLayout = VK_NULL_HANDLE;
        RTLog("[NRD-1 CREATE] NRD constant/sampler set layout failed");
        return false;
    }

    const VkDescriptorSetLayout setLayouts[2] =
    {
        g_nrdDescriptorSetLayout,
        g_nrdConstantSetLayout
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    pipelineLayoutInfo.setLayoutCount = 2u;
    pipelineLayoutInfo.pSetLayouts = setLayouts;

    if(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr, &g_nrdPipelineLayout) != VK_SUCCESS)
    {
        g_nrdPipelineLayout = VK_NULL_HANDLE;
        RTLog("[NRD-1 CREATE] NRD pipeline layout failed");
        return false;
    }

    RTLog(
        std::string("[NRD-1 CREATE] layouts ready: set0 resources textures=") +
        std::to_string(textureCount) + "@" + std::to_string(textureBase) +
        " storages=" + std::to_string(storageCount) + "@" + std::to_string(storageBase) +
        " set1 samplers@" + std::to_string(samplerBase) +
        " cb@" + std::to_string(constantBufferBinding)
    );

    return true;
}
bool createNRDPoolTextures()
{
    const uint32_t permanentCount = g_nrdInstanceDesc.permanentPoolSize;
    const uint32_t transientCount = g_nrdInstanceDesc.transientPoolSize;

    g_nrdPermanentImages.assign(permanentCount, VK_NULL_HANDLE);
    g_nrdPermanentMemories.assign(permanentCount, VK_NULL_HANDLE);
    g_nrdPermanentViews.assign(permanentCount, VK_NULL_HANDLE);
    g_nrdPermanentLayouts.assign(permanentCount, VK_IMAGE_LAYOUT_UNDEFINED);
    g_nrdPermanentDescs.resize(permanentCount);

    g_nrdTransientImages.assign(transientCount, VK_NULL_HANDLE);
    g_nrdTransientMemories.assign(transientCount, VK_NULL_HANDLE);
    g_nrdTransientViews.assign(transientCount, VK_NULL_HANDLE);
    g_nrdTransientLayouts.assign(transientCount, VK_IMAGE_LAYOUT_UNDEFINED);
    g_nrdTransientDescs.resize(transientCount);

    for(uint32_t i = 0u; i < permanentCount + transientCount; ++i)
    {
        const bool isPermanent = i < permanentCount;
        const nrd::TextureDesc& desc = isPermanent
            ? g_nrdInstanceDesc.permanentPool[i]
            : g_nrdInstanceDesc.transientPool[i - permanentCount];

        const uint32_t downsample = std::max<uint32_t>(1u, desc.downsampleFactor);
        const uint32_t poolWidth = (g_nrdWidth + downsample - 1u) / downsample;
        const uint32_t poolHeight = (g_nrdHeight + downsample - 1u) / downsample;
        const VkFormat format = nrdFormatToVkFormat(desc.format);

        const std::string what =
            std::string(isPermanent ? "permanent pool " : "transient pool ") +
            std::to_string(isPermanent ? i : (i - permanentCount));

        if(isPermanent)
        {
            g_nrdPermanentDescs[i] = desc;

            if(!nrdCreateImage2D(
                   poolWidth,
                   poolHeight,
                   format,
                   g_nrdPermanentImages[i],
                   g_nrdPermanentMemories[i],
                   g_nrdPermanentViews[i],
                   g_nrdPermanentLayouts[i],
                   what.c_str()))
            {
                return false;
            }
        }
        else
        {
            const uint32_t slot = i - permanentCount;
            g_nrdTransientDescs[slot] = desc;

            if(!nrdCreateImage2D(
                   poolWidth,
                   poolHeight,
                   format,
                   g_nrdTransientImages[slot],
                   g_nrdTransientMemories[slot],
                   g_nrdTransientViews[slot],
                   g_nrdTransientLayouts[slot],
                   what.c_str()))
            {
                return false;
            }
        }
    }

    return true;
}
bool createNRDPipelines()
{
    const uint32_t pipelineCount = g_nrdInstanceDesc.pipelinesNum;
    g_nrdPipelines.assign(pipelineCount, VK_NULL_HANDLE);

    for(uint32_t i = 0u; i < pipelineCount; ++i)
    {
        const nrd::PipelineDesc& desc = g_nrdInstanceDesc.pipelines[i];

        if(desc.computeShaderSPIRV.bytecode == nullptr || desc.computeShaderSPIRV.size == 0u)
        {
            RTLog(
                std::string("[NRD-1 CREATE] pipeline ") + std::to_string(i) +
                " has no SPIR-V"
            );
            return false;
        }

        VkShaderModuleCreateInfo moduleInfo{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
        };
        moduleInfo.codeSize = static_cast<size_t>(desc.computeShaderSPIRV.size);
        moduleInfo.pCode = static_cast<const uint32_t*>(desc.computeShaderSPIRV.bytecode);

        VkShaderModule module = VK_NULL_HANDLE;

        if(vkCreateShaderModule(g_device, &moduleInfo, nullptr, &module) != VK_SUCCESS)
        {
            module = VK_NULL_HANDLE;
            RTLog(
                std::string("[NRD-1 CREATE] shader module failed for pipeline ") +
                std::to_string(i)
            );
            return false;
        }

        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = NRD_SPIRV_ENTRY_POINT;

        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        pipelineInfo.stage = stage;
        pipelineInfo.layout = g_nrdPipelineLayout;

        VkResult result = vkCreateComputePipelines(
            g_device,
            getSharedPipelineCache(),
            1u,
            &pipelineInfo,
            nullptr,
            &g_nrdPipelines[i]
        );

        if(result != VK_SUCCESS && g_nrdInstanceDesc.shaderEntryPoint != nullptr)
        {
            stage.pName = g_nrdInstanceDesc.shaderEntryPoint;

            result = vkCreateComputePipelines(
                g_device,
                getSharedPipelineCache(),
                1u,
                &pipelineInfo,
                nullptr,
                &g_nrdPipelines[i]
            );
        }

        vkDestroyShaderModule(g_device, module, nullptr);

        if(result != VK_SUCCESS)
        {
            g_nrdPipelines[i] = VK_NULL_HANDLE;
            RTLog(
                std::string("[NRD-1 CREATE] vkCreateComputePipelines failed for pipeline ") +
                std::to_string(i) + " result=" + std::to_string(static_cast<int>(result)) +
                " name=" + std::string(desc.shaderIdentifier)
            );
            return false;
        }
    }

    return true;
}
bool createNRDAppTextures()
{
    if(!nrdCreateImage2D(
           g_nrdWidth,
           g_nrdHeight,
           VK_FORMAT_R32_SFLOAT,
           g_nrdViewZImage,
           g_nrdViewZMemory,
           g_nrdViewZView,
           g_nrdViewZLayout,
           "IN_VIEWZ"))
    {
        return false;
    }

    if(!nrdCreateImage2D(
           g_nrdWidth,
           g_nrdHeight,
           VK_FORMAT_A2B10G10R10_UNORM_PACK32,
           g_nrdNormalRoughnessImage,
           g_nrdNormalRoughnessMemory,
           g_nrdNormalRoughnessView,
           g_nrdNormalRoughnessLayout,
           "IN_NORMAL_ROUGHNESS"))
    {
        return false;
    }

    if(!nrdCreateImage2D(
           g_nrdWidth,
           g_nrdHeight,
           VK_FORMAT_R16G16_SFLOAT,
           g_nrdMotionImage,
           g_nrdMotionMemory,
           g_nrdMotionView,
           g_nrdMotionLayout,
           "IN_MV"))
    {
        return false;
    }

    if(!nrdCreateImage2D(
           g_nrdWidth,
           g_nrdHeight,
           VK_FORMAT_R16G16B16A16_SFLOAT,
           g_nrdDiffRadianceHitDistImage,
           g_nrdDiffRadianceHitDistMemory,
           g_nrdDiffRadianceHitDistView,
           g_nrdDiffRadianceHitDistLayout,
           "IN_DIFF_RADIANCE_HITDIST"))
    {
        return false;
    }

    if(!nrdCreateImage2D(
           g_nrdWidth,
           g_nrdHeight,
           VK_FORMAT_R16G16B16A16_SFLOAT,
           g_nrdOutDiffRadianceHitDistImage,
           g_nrdOutDiffRadianceHitDistMemory,
           g_nrdOutDiffRadianceHitDistView,
           g_nrdOutDiffRadianceHitDistLayout,
           "OUT_DIFF_RADIANCE_HITDIST"))
    {
        return false;
    }

    if(!nrdCreateImage2D(
           g_nrdWidth,
           g_nrdHeight,
           VK_FORMAT_R8G8B8A8_UNORM,
           g_nrdValidationImage,
           g_nrdValidationMemory,
           g_nrdValidationView,
           g_nrdValidationLayout,
           "OUT_VALIDATION"))
    {
        return false;
    }

    if(!nrdCreateImage2D(
           g_nrdWidth,
           g_nrdHeight,
           VK_FORMAT_R16G16B16A16_SFLOAT,
           g_nrdMvProbeImage,
           g_nrdMvProbeMemory,
           g_nrdMvProbeView,
           g_nrdMvProbeLayout,
           "MV_PROBE"))
    {
        return false;
    }

    return true;
}

// Publishes the four NRD textures into the shared RT descriptor set.  It runs
// after the textures exist, which is why it is not part of
// createRayTracingDescriptorPoolAndSet: NRD resources are created (and
// re-created on a render-resolution change) later than that set is allocated.
bool updateNRDDescriptors()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        RTLog("[NRD-1] RT descriptor set missing, cannot publish NRD textures");
        return false;
    }

    // 显式的 (binding, view) 表，替换掉原来那套 "前 4 个顺排、第 5 个落 55" 的
    // 位置推导：加了 binding 56 之后位置推导会很容易写错，而这里写错的表现是
    // 描述符错位、画面直接坏掉，不值得省这几行。
    struct NrdPublishEntry
    {
        uint32_t binding;
        VkImageView view;
    };

    NrdPublishEntry entries[7] =
    {
        { 50u, g_nrdViewZView },
        { 51u, g_nrdNormalRoughnessView },
        { 52u, g_nrdDiffRadianceHitDistView },
        { 53u, g_nrdOutDiffRadianceHitDistView },
        { 55u, g_nrdMotionView },
        { 56u, g_nrdValidationView },
        { 57u, g_nrdMvProbeView }
    };

    uint32_t selected[7]{};
    uint32_t selectedNum = 0u;

    for(uint32_t i = 0u; i < 4u; ++i)
    {
        selected[selectedNum++] = i;
    }

#if NRD_USE_REAL_MOTION_VECTORS
    selected[selectedNum++] = 4u;
#endif

    selected[selectedNum++] = 5u;
    selected[selectedNum++] = 6u;

    VkDescriptorImageInfo infos[7]{};

    for(uint32_t s = 0u; s < selectedNum; ++s)
    {
        const NrdPublishEntry& entry = entries[selected[s]];

        if(entry.view == VK_NULL_HANDLE)
        {
            RTLog(
                std::string("[NRD-1] missing NRD texture view for binding ") +
                std::to_string(entry.binding)
            );
            return false;
        }

        infos[s].imageView = entry.view;
        infos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkWriteDescriptorSet writes[7]{};

    for(uint32_t s = 0u; s < selectedNum; ++s)
    {
        writes[s].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[s].dstSet = g_rayTracingDescriptorSet;
        writes[s].dstBinding = entries[selected[s]].binding;
        writes[s].dstArrayElement = 0u;
        writes[s].descriptorCount = 1u;
        writes[s].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[s].pImageInfo = &infos[s];
    }

    vkUpdateDescriptorSets(g_device, selectedNum, writes, 0u, nullptr);

    return true;
}
bool createNRDConstantBuffer()
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(g_physicalDevice, &properties);

    const VkDeviceSize alignment = std::max<VkDeviceSize>(
        1u,
        properties.limits.minUniformBufferOffsetAlignment
    );

    const VkDeviceSize rawSize = static_cast<VkDeviceSize>(
        std::max(1u, g_nrdInstanceDesc.constantBufferMaxDataSize)
    );

    g_nrdConstantBufferStride =
        ((rawSize + alignment - 1u) / alignment) * alignment;

    const VkDeviceSize totalSize = g_nrdConstantBufferStride * NRD_MAX_DISPATCHES;

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = totalSize;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if(vkCreateBuffer(g_device, &bufferInfo, nullptr, &g_nrdConstantBuffer) != VK_SUCCESS)
    {
        g_nrdConstantBuffer = VK_NULL_HANDLE;
        RTLog("[NRD-1 CREATE] NRD constant buffer failed");
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(g_device, g_nrdConstantBuffer, &requirements);

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(
        g_physicalDevice,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    if(vkAllocateMemory(g_device, &allocation, nullptr, &g_nrdConstantBufferMemory) != VK_SUCCESS)
    {
        g_nrdConstantBufferMemory = VK_NULL_HANDLE;
        RTLog("[NRD-1 CREATE] NRD constant buffer memory failed");
        return false;
    }

    if(vkBindBufferMemory(g_device, g_nrdConstantBuffer, g_nrdConstantBufferMemory, 0) != VK_SUCCESS)
    {
        RTLog("[NRD-1 CREATE] NRD constant buffer bind failed");
        return false;
    }

    return true;
}
bool createNRDDescriptorSet()
{
    const uint32_t perSetTextures =
        std::max(1u, g_nrdInstanceDesc.descriptorPoolDesc.perSetTexturesMaxNum);

    const uint32_t perSetStorages =
        std::max(1u, g_nrdInstanceDesc.descriptorPoolDesc.perSetStorageTexturesMaxNum);

    const uint32_t samplersNum = std::max(1u, g_nrdInstanceDesc.samplersNum);

    VkDescriptorPoolSize poolSizes[4]{};

    poolSizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    poolSizes[0].descriptorCount = perSetTextures * NRD_MAX_DISPATCHES;

    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = perSetStorages * NRD_MAX_DISPATCHES;

    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[2].descriptorCount = NRD_MAX_DISPATCHES;

    poolSizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    poolSizes[3].descriptorCount = samplersNum * NRD_MAX_DISPATCHES;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 2u * NRD_MAX_DISPATCHES;
    poolInfo.poolSizeCount = 4u;
    poolInfo.pPoolSizes = poolSizes;

    if(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_nrdDescriptorPool) != VK_SUCCESS)
    {
        g_nrdDescriptorPool = VK_NULL_HANDLE;
        RTLog("[NRD-1 CREATE] NRD descriptor pool failed");
        return false;
    }

    // Allocated once and kept for the lifetime of the instance.  Nothing is
    // reset per frame, so a frame still in flight can never have its sets
    // rewritten underneath it.
    std::vector<VkDescriptorSetLayout> layouts(
        NRD_MAX_DISPATCHES,
        g_nrdDescriptorSetLayout
    );

    g_nrdDescriptorSets.assign(NRD_MAX_DISPATCHES, VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = g_nrdDescriptorPool;
    allocateInfo.descriptorSetCount = NRD_MAX_DISPATCHES;
    allocateInfo.pSetLayouts = layouts.data();

    if(vkAllocateDescriptorSets(g_device, &allocateInfo, g_nrdDescriptorSets.data()) != VK_SUCCESS)
    {
        g_nrdDescriptorSets.clear();
        RTLog("[NRD-1 CREATE] NRD descriptor set allocation failed");
        return false;
    }

    // SET 1: the constant buffer and the immutable samplers, one set per
    // dispatch so each dispatch can carry its own constant-buffer ring slot.
    std::vector<VkDescriptorSetLayout> constantLayouts(
        NRD_MAX_DISPATCHES,
        g_nrdConstantSetLayout
    );

    g_nrdConstantSets.assign(NRD_MAX_DISPATCHES, VK_NULL_HANDLE);

    VkDescriptorSetAllocateInfo constantAllocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    constantAllocateInfo.descriptorPool = g_nrdDescriptorPool;
    constantAllocateInfo.descriptorSetCount = NRD_MAX_DISPATCHES;
    constantAllocateInfo.pSetLayouts = constantLayouts.data();

    if(vkAllocateDescriptorSets(g_device, &constantAllocateInfo, g_nrdConstantSets.data()) != VK_SUCCESS)
    {
        g_nrdConstantSets.clear();
        RTLog("[NRD-1 CREATE] NRD constant set allocation failed");
        return false;
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = g_nrdConstantBuffer;
    bufferInfo.offset = 0u;
    bufferInfo.range = g_nrdInstanceDesc.constantBufferMaxDataSize;

    std::vector<VkWriteDescriptorSet> writes(NRD_MAX_DISPATCHES);

    for(uint32_t i = 0u; i < NRD_MAX_DISPATCHES; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_nrdConstantSets[i];
        writes[i].dstBinding =
            g_nrdLibraryDesc.spirvBindingOffsets.constantBufferOffset +
            g_nrdInstanceDesc.constantBufferRegisterIndex;
        writes[i].dstArrayElement = 0u;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].pBufferInfo = &bufferInfo;
    }

    vkUpdateDescriptorSets(
        g_device,
        NRD_MAX_DISPATCHES,
        writes.data(),
        0u,
        nullptr
    );

    return true;
}
void destroyNRDResources()
{
    g_nrdReady = false;

    if(g_device == VK_NULL_HANDLE)
    {
        g_nrdInstance = nullptr;
        return;
    }

    destroyNRDCompositePipeline();

    for(VkPipeline& pipeline : g_nrdPipelines)
    {
        if(pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(g_device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }

    g_nrdPipelines.clear();

    if(g_nrdDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_nrdDescriptorPool, nullptr);
        g_nrdDescriptorPool = VK_NULL_HANDLE;
    }

    g_nrdDescriptorSets.clear();
    g_nrdConstantSets.clear();

    if(g_nrdPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_nrdPipelineLayout, nullptr);
        g_nrdPipelineLayout = VK_NULL_HANDLE;
    }

    if(g_nrdDescriptorSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_nrdDescriptorSetLayout, nullptr);
        g_nrdDescriptorSetLayout = VK_NULL_HANDLE;
    }

    if(g_nrdConstantSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_nrdConstantSetLayout, nullptr);
        g_nrdConstantSetLayout = VK_NULL_HANDLE;
    }

    if(g_nrdConstantBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_nrdConstantBuffer, nullptr);
        g_nrdConstantBuffer = VK_NULL_HANDLE;
    }

    if(g_nrdConstantBufferMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_nrdConstantBufferMemory, nullptr);
        g_nrdConstantBufferMemory = VK_NULL_HANDLE;
    }

    g_nrdConstantBufferStride = 0u;
    g_nrdConstantBufferOffsets.clear();

    for(size_t i = 0u; i < g_nrdPermanentImages.size(); ++i)
    {
        nrdDestroyImage(
            g_nrdPermanentImages[i],
            g_nrdPermanentMemories[i],
            g_nrdPermanentViews[i],
            g_nrdPermanentLayouts[i]
        );
    }

    for(size_t i = 0u; i < g_nrdTransientImages.size(); ++i)
    {
        nrdDestroyImage(
            g_nrdTransientImages[i],
            g_nrdTransientMemories[i],
            g_nrdTransientViews[i],
            g_nrdTransientLayouts[i]
        );
    }

    g_nrdPermanentImages.clear();
    g_nrdPermanentMemories.clear();
    g_nrdPermanentViews.clear();
    g_nrdPermanentLayouts.clear();
    g_nrdPermanentDescs.clear();

    g_nrdTransientImages.clear();
    g_nrdTransientMemories.clear();
    g_nrdTransientViews.clear();
    g_nrdTransientLayouts.clear();
    g_nrdTransientDescs.clear();

    nrdDestroyImage(
        g_nrdViewZImage,
        g_nrdViewZMemory,
        g_nrdViewZView,
        g_nrdViewZLayout
    );

    nrdDestroyImage(
        g_nrdNormalRoughnessImage,
        g_nrdNormalRoughnessMemory,
        g_nrdNormalRoughnessView,
        g_nrdNormalRoughnessLayout
    );

    nrdDestroyImage(
        g_nrdMotionImage,
        g_nrdMotionMemory,
        g_nrdMotionView,
        g_nrdMotionLayout
    );

    nrdDestroyImage(
        g_nrdDiffRadianceHitDistImage,
        g_nrdDiffRadianceHitDistMemory,
        g_nrdDiffRadianceHitDistView,
        g_nrdDiffRadianceHitDistLayout
    );

    nrdDestroyImage(
        g_nrdOutDiffRadianceHitDistImage,
        g_nrdOutDiffRadianceHitDistMemory,
        g_nrdOutDiffRadianceHitDistView,
        g_nrdOutDiffRadianceHitDistLayout
    );

    nrdDestroyImage(
        g_nrdValidationImage,
        g_nrdValidationMemory,
        g_nrdValidationView,
        g_nrdValidationLayout
    );

    nrdDestroyImage(
        g_nrdMvProbeImage,
        g_nrdMvProbeMemory,
        g_nrdMvProbeView,
        g_nrdMvProbeLayout
    );

    if(g_nrdSamplerNearest != VK_NULL_HANDLE)
    {
        vkDestroySampler(g_device, g_nrdSamplerNearest, nullptr);
        g_nrdSamplerNearest = VK_NULL_HANDLE;
    }

    if(g_nrdSamplerLinear != VK_NULL_HANDLE)
    {
        vkDestroySampler(g_device, g_nrdSamplerLinear, nullptr);
        g_nrdSamplerLinear = VK_NULL_HANDLE;
    }

    if(g_nrdInstance != nullptr)
    {
        nrd::DestroyInstance(*g_nrdInstance);
        g_nrdInstance = nullptr;
    }

    g_nrdWidth = 0u;
    g_nrdHeight = 0u;
    g_nrdFrameIndex = 0u;
    g_nrdNeedsHistoryReset = true;
    g_nrdSettingsLogged = false;
}

// Full (re)creation.  NRD has no resize support at all (NRD README, "API"), so
// a render-resolution change - which this backend really does have, because the
// RR preset changes the render size - must destroy and rebuild the instance.
bool createNRDResources(uint32_t width, uint32_t height)
{
    if(!kNrdLocalDiEnabled)
    {
        return true;
    }

    if(g_device == VK_NULL_HANDLE || width == 0u || height == 0u)
    {
        return false;
    }

    g_nrdReady = false;
    g_nrdWidth = width;
    g_nrdHeight = height;

    const nrd::LibraryDesc* libraryDesc = nrd::GetLibraryDesc();

    if(libraryDesc == nullptr)
    {
        RTLog("[NRD-1 INIT] GetLibraryDesc returned null");
        return false;
    }

    g_nrdLibraryDesc = *libraryDesc;

#ifndef NRD_LOCAL_DI_USE_RELAX
#define NRD_LOCAL_DI_USE_RELAX 0
#endif

#if NRD_LOCAL_DI_USE_RELAX
    constexpr nrd::Denoiser NRD_LOCAL_DI_DENOISER = nrd::Denoiser::RELAX_DIFFUSE;
    constexpr const char* NRD_LOCAL_DI_DENOISER_NAME = "RELAX_DIFFUSE";
#else
    constexpr nrd::Denoiser NRD_LOCAL_DI_DENOISER = nrd::Denoiser::REBLUR_DIFFUSE;
    constexpr const char* NRD_LOCAL_DI_DENOISER_NAME = "REBLUR_DIFFUSE";
#endif

    bool supportsDiffuse = false;

    for(uint32_t i = 0u; i < g_nrdLibraryDesc.supportedDenoisersNum; ++i)
    {
        if(g_nrdLibraryDesc.supportedDenoisers[i] == NRD_LOCAL_DI_DENOISER)
        {
            supportsDiffuse = true;
            break;
        }
    }

    if(!supportsDiffuse)
    {
        RTLog(
            std::string("[NRD-1 INIT] this NRD build has no ") +
            NRD_LOCAL_DI_DENOISER_NAME
        );
        return false;
    }

    const nrd::DenoiserDesc denoiserDescs[] =
    {
        {g_nrdIdentifier, NRD_LOCAL_DI_DENOISER}
    };

    nrd::InstanceCreationDesc creationDesc{};
    creationDesc.denoisers = denoiserDescs;
    creationDesc.denoisersNum = 1u;

    if(nrd::CreateInstance(creationDesc, g_nrdInstance) != nrd::Result::SUCCESS)
    {
        g_nrdInstance = nullptr;
        RTLog("[NRD-1 INIT] nrd::CreateInstance failed");
        return false;
    }

    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*g_nrdInstance);

    if(instanceDesc == nullptr)
    {
        RTLog("[NRD-1 INIT] GetInstanceDesc returned null");
        destroyNRDResources();
        return false;
    }

    g_nrdInstanceDesc = *instanceDesc;

    RTLog(
        std::string("[NRD-1 INIT] version=") +
        std::to_string(g_nrdLibraryDesc.versionMajor) + "." +
        std::to_string(g_nrdLibraryDesc.versionMinor) + "." +
        std::to_string(g_nrdLibraryDesc.versionBuild) +
        " normalEncoding=" + std::to_string(static_cast<int>(g_nrdLibraryDesc.normalEncoding)) +
        " roughnessEncoding=" + std::to_string(static_cast<int>(g_nrdLibraryDesc.roughnessEncoding)) +
        " cbSpace=" + std::to_string(g_nrdInstanceDesc.constantBufferAndSamplersSpaceIndex) +
        " resSpace=" + std::to_string(g_nrdInstanceDesc.resourcesSpaceIndex) +
        " texOffset=" + std::to_string(g_nrdLibraryDesc.spirvBindingOffsets.textureOffset) +
        " storageOffset=" + std::to_string(g_nrdLibraryDesc.spirvBindingOffsets.storageTextureAndBufferOffset) +
        " samplerOffset=" + std::to_string(g_nrdLibraryDesc.spirvBindingOffsets.samplerOffset) +
        " cbOffset=" + std::to_string(g_nrdLibraryDesc.spirvBindingOffsets.constantBufferOffset) +
        " pipelines=" + std::to_string(g_nrdInstanceDesc.pipelinesNum) +
        " permanent=" + std::to_string(g_nrdInstanceDesc.permanentPoolSize) +
        " transient=" + std::to_string(g_nrdInstanceDesc.transientPoolSize) +
        " cbSize=" + std::to_string(g_nrdInstanceDesc.constantBufferMaxDataSize) +
        " perSetTextures=" + std::to_string(g_nrdInstanceDesc.descriptorPoolDesc.perSetTexturesMaxNum) +
        " perSetStorages=" + std::to_string(g_nrdInstanceDesc.descriptorPoolDesc.perSetStorageTexturesMaxNum) +
        " size=" + std::to_string(width) + "x" + std::to_string(height)
    );

    if(g_nrdInstanceDesc.constantBufferAndSamplersSpaceIndex != 1u ||
       g_nrdInstanceDesc.resourcesSpaceIndex != 0u)
    {
        RTLog("[NRD-1 INIT] unexpected register spaces, refusing to build a mismatched layout");
        destroyNRDResources();
        return false;
    }

    if(g_nrdInstanceDesc.descriptorPoolDesc.perSetStorageTexturesMaxNum == 0u ||
       g_nrdInstanceDesc.descriptorPoolDesc.perSetTexturesMaxNum == 0u)
    {
        RTLog("[NRD-1 INIT] empty descriptor pool description");
        destroyNRDResources();
        return false;
    }

    g_nrdSamplerNearest = nrdCreateImmutableSampler(false, "NEAREST_CLAMP");
    g_nrdSamplerLinear = nrdCreateImmutableSampler(true, "LINEAR_CLAMP");

    if(g_nrdSamplerNearest == VK_NULL_HANDLE || g_nrdSamplerLinear == VK_NULL_HANDLE)
    {
        destroyNRDResources();
        return false;
    }

    if(!createNRDDescriptorSetLayout() ||
       !createNRDConstantBuffer() ||
       !createNRDDescriptorSet() ||
       !createNRDPoolTextures() ||
       !createNRDPipelines() ||
       !createNRDAppTextures() ||
       !updateNRDDescriptors())
    {
        destroyNRDResources();
        return false;
    }

    nrdTransitionImagesToGeneral();

    try
    {
        createNRDCompositePipeline();
    }
    catch(...)
    {
        RTLog("[NRD-1 INIT] composite pipeline creation threw");
        destroyNRDResources();
        return false;
    }

    g_nrdFrameIndex = 0u;
    g_nrdNeedsHistoryReset = true;
    g_nrdReady = true;

    RTLog("[NRD-1 INIT] REBLUR_DIFFUSE ready");

    return true;
}

// Called once per frame before the RT pass, so a render-resolution change is
// picked up in the same place the DLSS/RR guide images are resized.
void ensureNRDResources(uint32_t width, uint32_t height)
{
    constexpr bool NRD_LOCAL_DI_ENABLED = true;
    if(!NRD_LOCAL_DI_ENABLED)
    {
        return;
    }

    if(width == 0u || height == 0u)
    {
        return;
    }

    if(g_nrdReady && width == g_nrdWidth && height == g_nrdHeight)
    {
        return;
    }

    if(g_nrdReady || g_nrdInstance != nullptr)
    {
        RTLog(
            std::string("[NRD-1 RESIZE] ") + std::to_string(g_nrdWidth) + "x" +
            std::to_string(g_nrdHeight) + " -> " + std::to_string(width) + "x" +
            std::to_string(height)
        );

        check(
            FGDeviceWaitIdle(g_device),
            "wait device before NRD resize failed"
        );

        destroyNRDResources();
    }

    createNRDResources(width, height);
}
void createNRDCompositePipeline()
{
    if(g_nrdCompositePipeline != VK_NULL_HANDLE)
    {
        return;
    }

    const std::vector<uint8_t> code =
        loadSpv("shaders/rt_nrd_composite.comp.spv");

    VkShaderModule shaderModule = createShaderModule(g_device, code);

    try
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size = sizeof(NRDCompositePushConstants);

        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        layoutInfo.setLayoutCount = 1u;
        layoutInfo.pSetLayouts = &g_rayTracingDescriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1u;
        layoutInfo.pPushConstantRanges = &pushRange;

        check(
            vkCreatePipelineLayout(
                g_device,
                &layoutInfo,
                nullptr,
                &g_nrdCompositePipelineLayout
            ),
            "NRD composite pipeline layout failed"
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
        pipelineInfo.layout = g_nrdCompositePipelineLayout;

        check(
            vkCreateComputePipelines(
                g_device,
                getSharedPipelineCache(),
                1u,
                &pipelineInfo,
                nullptr,
                &g_nrdCompositePipeline
            ),
            "NRD composite compute pipeline failed"
        );
    }
    catch(...)
    {
        if(g_nrdCompositePipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(
                g_device,
                g_nrdCompositePipelineLayout,
                nullptr
            );
            g_nrdCompositePipelineLayout = VK_NULL_HANDLE;
        }

        vkDestroyShaderModule(g_device, shaderModule, nullptr);
        throw;
    }

    vkDestroyShaderModule(g_device, shaderModule, nullptr);
    RTLog("[NRD-1] local DI composite pipeline created");
}
void destroyNRDCompositePipeline()
{
    if(g_nrdCompositePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_nrdCompositePipeline, nullptr);
        g_nrdCompositePipeline = VK_NULL_HANDLE;
    }

    if(g_nrdCompositePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(
            g_device,
            g_nrdCompositePipelineLayout,
            nullptr
        );
        g_nrdCompositePipelineLayout = VK_NULL_HANDLE;
    }
}

// Camera-space matrices for NRD.  NRD requires NON-jittered matrices and takes
// the jitter separately (NRD README, "OTHER"), which is exactly how this
// backend already treats them: CameraUBO::viewInverse / projInverse come from
// JOML un-jittered, and the DLSS jitter is a separate Halton offset in
// [-0.5; 0.5] produced by GetDLSSFrameJitter.
//
// Streamline's float4x4 stores rows in memory ("m[row][col]"), while NRD wants
// "vector is a column, layout is column-major", so the matrices are transposed
// on the way out.
void nrdFillColumnMajor(const sl::float4x4& m, float out[16])
{
    for(uint32_t row = 0u; row < 4u; ++row)
    {
        const sl::float4 r = m[row];

        out[0u * 4u + row] = r.x;
        out[1u * 4u + row] = r.y;
        out[2u * 4u + row] = r.z;
        out[3u * 4u + row] = r.w;
    }
}
void nrdBuildCommonSettings(
    nrd::CommonSettings& settings,
    uint32_t width,
    uint32_t height)
{
    settings = {};

    float currentJitterX = 0.0f;
    float currentJitterY = 0.0f;
    float previousJitterX = 0.0f;
    float previousJitterY = 0.0f;

    uint32_t jitterFrame = g_nrdFrameIndex;

    if(g_cameraUboMapped != nullptr)
    {
        const CameraUBO* cameraUbo =
            static_cast<const CameraUBO*>(g_cameraUboMapped);

        sl::float4x4 currentViewToWorld =
            JOMLColumnMajorToSLRowMajor(cameraUbo->viewInverse);

        sl::float4x4 previousViewToWorld =
            JOMLColumnMajorToSLRowMajor(cameraUbo->prevViewInverse);

        sl::float4x4 currentClipToView =
            JOMLColumnMajorToSLRowMajor(cameraUbo->projInverse);

        sl::float4x4 previousClipToView =
            JOMLColumnMajorToSLRowMajor(cameraUbo->prevProjInverse);

        sl::float4x4 currentViewToClip{};
        sl::float4x4 previousViewToClip{};
        sl::float4x4 currentWorldToView{};

        sl::matrixFullInvert(currentViewToClip, currentClipToView);
        sl::matrixFullInvert(previousViewToClip, previousClipToView);
        sl::matrixFullInvert(currentWorldToView, currentViewToWorld);

        nrdFillColumnMajor(currentViewToClip, settings.viewToClipMatrix);
        nrdFillColumnMajor(previousViewToClip, settings.viewToClipMatrixPrev);
        nrdFillColumnMajor(currentWorldToView, settings.worldToViewMatrix);

        sl::float4x4 previousWorldToView{};
        sl::matrixFullInvert(previousWorldToView, previousViewToWorld);
        nrdFillColumnMajor(previousWorldToView, settings.worldToViewMatrixPrev);

        previousJitterX = cameraUbo->historyPrevJitterX;
        previousJitterY = cameraUbo->historyPrevJitterY;
        jitterFrame = cameraUbo->frameCount;
    }

    GetDLSSFrameJitter(jitterFrame, currentJitterX, currentJitterY);

    settings.cameraJitter[0] = currentJitterX;
    settings.cameraJitter[1] = currentJitterY;
    settings.cameraJitterPrev[0] = previousJitterX;
    settings.cameraJitterPrev[1] = previousJitterY;

#if NRD_USE_REAL_MOTION_VECTORS
    settings.motionVectorScale[0] = 1.0f;
    settings.motionVectorScale[1] = 1.0f;
    settings.motionVectorScale[2] =
        NRD_USE_25D_MOTION_VECTORS ? 1.0f : 0.0f;
#ifndef NRD_MV_JITTER_BIAS
#define NRD_MV_JITTER_BIAS 0
#endif
#if NRD_MV_JITTER_BIAS
    settings.motionVectorBias[0] =
        (currentJitterX - previousJitterX) /
        std::max(1.0f, static_cast<float>(width));
    settings.motionVectorBias[1] =
        (currentJitterY - previousJitterY) /
        std::max(1.0f, static_cast<float>(height));
#else
    settings.motionVectorBias[0] = 0.0f;
    settings.motionVectorBias[1] = 0.0f;
#endif
    settings.motionVectorBias[2] = 0.0f;
    settings.isMotionVectorInWorldSpace = false;
#else
#if 0
    settings.motionVectorScale[0] = 0.0f;
    settings.motionVectorScale[1] = 0.0f;
    settings.motionVectorScale[2] = 0.0f;
    settings.motionVectorBias[0] = 0.0f;
    settings.motionVectorBias[1] = 0.0f;
    settings.motionVectorBias[2] = 0.0f;
    settings.isMotionVectorInWorldSpace = true;
#endif
#endif

    static bool mvSettingsLogged = false;

    const CameraUBO* mvLogProbe = g_cameraUboMapped != nullptr
        ? static_cast<const CameraUBO*>(g_cameraUboMapped)
        : nullptr;

    const bool mvLogReady =
        mvLogProbe != nullptr && mvLogProbe->frameCount >= 120u;

    if(!mvSettingsLogged && mvLogReady)
    {
        mvSettingsLogged = true;

        const CameraUBO* mvUbo = g_cameraUboMapped != nullptr
            ? static_cast<const CameraUBO*>(g_cameraUboMapped)
            : nullptr;

        RTLog(
            std::string("[NRD-1 MV] motionVectorScale=(") +
            std::to_string(settings.motionVectorScale[0]) + "," +
            std::to_string(settings.motionVectorScale[1]) + "," +
            std::to_string(settings.motionVectorScale[2]) + ")" +
            " isMotionVectorInWorldSpace=" +
            (settings.isMotionVectorInWorldSpace ? "1" : "0") +
            " jitter=(" + std::to_string(currentJitterX) + "," +
            std::to_string(currentJitterY) + ")" +
            " jitterPrev=(" + std::to_string(previousJitterX) + "," +
            std::to_string(previousJitterY) + ")" +
            " mvBias=(" + std::to_string(settings.motionVectorBias[0]) + "," +
            std::to_string(settings.motionVectorBias[1]) + ")" +
            " prevMatrix=CameraUBO.prevViewInverse/prevProjInverse(historyValid=" +
            std::to_string(mvUbo != nullptr ? mvUbo->historyValid : 0u) +
            ",frameCount=" +
            std::to_string(mvUbo != nullptr ? mvUbo->frameCount : 0u) + ")"
        );
    }

#if NRD_LOCAL_DI_CONFIDENCE
    settings.isHistoryConfidenceAvailable =
        g_nrdConfidenceViewThisFrame != VK_NULL_HANDLE;
#else
    settings.isHistoryConfidenceAvailable = false;
#endif

    settings.resourceSize[0] = static_cast<uint16_t>(width);
    settings.resourceSize[1] = static_cast<uint16_t>(height);
    settings.resourceSizePrev[0] = static_cast<uint16_t>(width);
    settings.resourceSizePrev[1] = static_cast<uint16_t>(height);
    settings.rectSize[0] = static_cast<uint16_t>(width);
    settings.rectSize[1] = static_cast<uint16_t>(height);
    settings.rectSizePrev[0] = static_cast<uint16_t>(width);
    settings.rectSizePrev[1] = static_cast<uint16_t>(height);

    settings.viewZScale = 1.0f;
    settings.frameIndex = g_nrdFrameIndex;

    settings.accumulationMode = g_nrdNeedsHistoryReset
        ? nrd::AccumulationMode::CLEAR_AND_RESTART
        : nrd::AccumulationMode::CONTINUE;

    settings.denoisingRange = 60000.0f;

    float disocclusionThresholdValue = 0.01f;

    {
        const char* envThreshold = std::getenv("RTX_NRD_DISOCCLUSION_THRESHOLD");

        if(envThreshold != nullptr && envThreshold[0] != '\0')
        {
            const float parsed = static_cast<float>(std::atof(envThreshold));

            if(parsed > 0.0f)
            {
                disocclusionThresholdValue = parsed;
            }
        }
    }

    settings.disocclusionThreshold = disocclusionThresholdValue;
    settings.disocclusionThresholdAlternate = disocclusionThresholdValue * 2.0f;

    if(!g_nrdDisocclusionLogged)
    {
        g_nrdDisocclusionLogged = true;

        RTLog(
            std::string("[NRD-1 DISOC] threshold=") +
            std::to_string(settings.disocclusionThreshold) +
            " alternate=" + std::to_string(settings.disocclusionThresholdAlternate)
        );
    }

    settings.isDisocclusionThresholdMixAvailable = false;

#if NRD_LOCAL_DI_VALIDATION
    settings.enableValidation = true;
#else
    settings.enableValidation = false;
#endif
}
void nrdBuildReblurSettings(nrd::ReblurSettings& settings)
{
    settings = {};

    settings.minMaterialForDiffuse = 0.0f;
    settings.minMaterialForSpecular = 0.0f;

    const float smoothedFps = 1000.0f / std::max(1.0f, g_nrdSmoothedFrameTimeMs);

    settings.maxAccumulatedFrameNum = std::min<uint32_t>(
        nrd::REBLUR_MAX_HISTORY_FRAME_NUM,
        nrd::GetMaxAccumulatedFrameNum(NRD_LOCAL_DI_ACCUMULATION_SECONDS, smoothedFps)
    );

    settings.maxFastAccumulatedFrameNum = std::min<uint32_t>(
        settings.maxFastAccumulatedFrameNum,
        settings.maxAccumulatedFrameNum
    );

    settings.historyFixFrameNum = std::min<uint32_t>(
        settings.historyFixFrameNum,
        settings.maxFastAccumulatedFrameNum
    );


    settings.hitDistanceReconstructionMode = NRD_LOCAL_DI_HIT_DIST_RECONSTRUCTION_5X5
        ? nrd::HitDistanceReconstructionMode::AREA_5X5
        : nrd::HitDistanceReconstructionMode::OFF;

    settings.antilagSettings.luminanceSigmaScale = 4.0f;
    settings.antilagSettings.luminanceSensitivity = 3.0f;

#ifndef NRD_LOCAL_DI_RETURN_HISTORY_LENGTH
#define NRD_LOCAL_DI_RETURN_HISTORY_LENGTH 1
#endif
#if NRD_LOCAL_DI_RETURN_HISTORY_LENGTH
    settings.returnHistoryLengthInsteadOfOcclusion = true;
#endif

    settings.diffusePrepassBlurRadius = 0.0f;
    settings.specularPrepassBlurRadius = 0.0f;

    settings.hitDistanceParameters = nrd::ReblurHitDistanceParameters{};

    if(!g_nrdSettingsLogged)
    {
        g_nrdSettingsLogged = true;

        RTLog(
            std::string("[NRD-1 TUNING] reblur accumulationSeconds=") +
            std::to_string(NRD_LOCAL_DI_ACCUMULATION_SECONDS) +
            " maxAccumulatedFrameNum=" + std::to_string(settings.maxAccumulatedFrameNum) +
            " maxFastAccumulatedFrameNum=" + std::to_string(settings.maxFastAccumulatedFrameNum) +
            " historyFixFrameNum=" + std::to_string(settings.historyFixFrameNum) +
            " fastHistoryClampingSigmaScale=" + std::to_string(settings.fastHistoryClampingSigmaScale) +
            " hitDistA=" + std::to_string(settings.hitDistanceParameters.A) +
            " hitDistB=" + std::to_string(settings.hitDistanceParameters.B) +
            " hitDistC=" + std::to_string(settings.hitDistanceParameters.C) +
            " diffPrepassBlurRadius=" + std::to_string(settings.diffusePrepassBlurRadius) +
            " specPrepassBlurRadius=" + std::to_string(settings.specularPrepassBlurRadius) +
            " antilagSigmaScale=" + std::to_string(settings.antilagSettings.luminanceSigmaScale) +
            " antilagSensitivity=" + std::to_string(settings.antilagSettings.luminanceSensitivity) +
            " minMaterialForDiffuse=" + std::to_string(settings.minMaterialForDiffuse) +
            " minMaterialForSpecular=" + std::to_string(settings.minMaterialForSpecular) +
            " smoothedFps=" + std::to_string(smoothedFps)
        );
    }
}

#if NRD_LOCAL_DI_USE_RELAX
//
// 两个"帧数"字段按官方 FPS 公式换算（NRD README:911-914）：
//   maxAccumulatedFrameNum = accumulationPeriodInSeconds * FPS。
// 用 scaleFrameCount 换算，等价于 accumulationSeconds = configured / 60，本函数照抄
// 该换算，所以 60fps 下结果就是 30 / 12。
//
// 注意：这与上面 REBLUR 分支的 NRD_LOCAL_DI_ACCUMULATION_SECONDS = 1.0 **不同**，
// 是刻意的：RELAX 这组按"60fps 基准帧数"换算，REBLUR 那边（HANDOFF §12.3）
// 按 NRD README 的推荐公式保留 1.0s，两件事各自的依据不同。
//
// specular* 几项在本实例（RELAX_DIFFUSE，无镜面通道）里是惰性的，但仍要填：
// historyFixFrameNum 的推导用的是 min(diffuseMaxFast, specularMaxFast) - 1，
// 那一项不是可有可无的装饰。
//
// 回退：把宿主 NRD_LOCAL_DI_USE_RELAX 改回 0（并同步两个 shader 的同名宏）。
// =============================================================================
void nrdBuildRelaxSettings(nrd::RelaxSettings& settings)
{
    settings = {};

    const float smoothedFps = 1000.0f / std::max(1.0f, g_nrdSmoothedFrameTimeMs);

    const auto framesFromReference60 = [smoothedFps](uint32_t referenceFrames)
    {
        return std::min<uint32_t>(
            nrd::RELAX_MAX_HISTORY_FRAME_NUM,
            nrd::GetMaxAccumulatedFrameNum(
                static_cast<float>(referenceFrames) / 60.0f, smoothedFps
            )
        );
    };

    settings.antilagSettings.accelerationAmount = 0.3f;
    settings.antilagSettings.resetAmount = 0.5f;
    settings.diffuseMaxAccumulatedFrameNum = framesFromReference60(30u);
    settings.specularMaxAccumulatedFrameNum = framesFromReference60(12u);
    settings.diffuseMaxFastAccumulatedFrameNum = 6u;
    settings.specularMaxFastAccumulatedFrameNum = 4u;
    settings.historyFixFrameNum = 3u;
    settings.fastHistoryClampingSigmaScale = 1.35f;
    settings.diffusePrepassBlurRadius = 0.0f;
    settings.specularPrepassBlurRadius = 0.0f;
    settings.minHitDistanceWeight = 0.05f;
    settings.diffusePhiLuminance = 2.5f;
    settings.specularPhiLuminance = 1.5f;
    settings.diffuseMinLuminanceWeight = 0.05f;
    settings.specularMinLuminanceWeight = 0.02f;
    settings.lobeAngleFraction = 0.35f;
    settings.roughnessFraction = 0.2f;
    settings.specularVarianceBoost = 0.15f;
    settings.atrousIterationNum = 3u;
    settings.depthThreshold = 0.003f;
    settings.minMaterialForDiffuse = 0.0f;
    settings.minMaterialForSpecular = 0.0f;
    settings.enableAntiFirefly = true;
    settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;

    settings.diffuseMaxFastAccumulatedFrameNum = std::min<uint32_t>(
        settings.diffuseMaxFastAccumulatedFrameNum,
        settings.diffuseMaxAccumulatedFrameNum
    );
    settings.specularMaxFastAccumulatedFrameNum = std::min<uint32_t>(
        settings.specularMaxFastAccumulatedFrameNum,
        settings.specularMaxAccumulatedFrameNum
    );

    const uint32_t minimumFastHistory = std::min<uint32_t>(
        settings.diffuseMaxFastAccumulatedFrameNum,
        settings.specularMaxFastAccumulatedFrameNum
    );
    settings.historyFixFrameNum = minimumFastHistory == 0u
        ? 0u
        : std::min<uint32_t>(settings.historyFixFrameNum, minimumFastHistory - 1u);

    if(!g_nrdSettingsLogged)
    {
        g_nrdSettingsLogged = true;

        RTLog(
            std::string("[NRD-1 TUNING] relax (reference-aligned)")
            + " diffMaxFrames=" + std::to_string(settings.diffuseMaxAccumulatedFrameNum)
            + " specMaxFrames=" + std::to_string(settings.specularMaxAccumulatedFrameNum)
            + " diffFastFrames=" + std::to_string(settings.diffuseMaxFastAccumulatedFrameNum)
            + " specFastFrames=" + std::to_string(settings.specularMaxFastAccumulatedFrameNum)
            + " historyFixFrameNum=" + std::to_string(settings.historyFixFrameNum)
            + " fastHistoryClampingSigmaScale=" + std::to_string(settings.fastHistoryClampingSigmaScale)
            + " atrousIterationNum=" + std::to_string(settings.atrousIterationNum)
            + " accelerationAmount=" + std::to_string(settings.antilagSettings.accelerationAmount)
            + " resetAmount=" + std::to_string(settings.antilagSettings.resetAmount)
            + " minMaterialForDiffuse=" + std::to_string(settings.minMaterialForDiffuse)
            + " minMaterialForSpecular=" + std::to_string(settings.minMaterialForSpecular)
            + " smoothedFps=" + std::to_string(smoothedFps)
        );
    }
}
#endif

#if NRD_LOCAL_DI_CONFIDENCE
bool createDiConfidenceResources(uint32_t width, uint32_t height)
{
    if(g_diGradientImage != VK_NULL_HANDLE &&
       g_diConfidenceImages[0] != VK_NULL_HANDLE &&
       g_diConfidenceImages[1] != VK_NULL_HANDLE &&
       g_diLumaImages[0] != VK_NULL_HANDLE &&
       g_diLumaImages[1] != VK_NULL_HANDLE &&
       width == g_diConfidenceWidth &&
       height == g_diConfidenceHeight)
    {
        return true;
    }

    if(g_diConfidenceReady ||
       g_diGradientImage != VK_NULL_HANDLE ||
       g_diConfidenceImages[0] != VK_NULL_HANDLE)
    {
        if(g_device != VK_NULL_HANDLE)
        {
            FGDeviceWaitIdle(g_device);
        }

        for(uint32_t i = 0u; i < 2u; ++i)
        {
            if(g_diConfidenceViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(g_device, g_diConfidenceViews[i], nullptr);
            }
            if(g_diConfidenceImages[i] != VK_NULL_HANDLE)
            {
                vkDestroyImage(g_device, g_diConfidenceImages[i], nullptr);
            }
            if(g_diConfidenceMemories[i] != VK_NULL_HANDLE)
            {
                vkFreeMemory(g_device, g_diConfidenceMemories[i], nullptr);
            }

            if(g_diLumaViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(g_device, g_diLumaViews[i], nullptr);
            }
            if(g_diLumaImages[i] != VK_NULL_HANDLE)
            {
                vkDestroyImage(g_device, g_diLumaImages[i], nullptr);
            }
            if(g_diLumaMemories[i] != VK_NULL_HANDLE)
            {
                vkFreeMemory(g_device, g_diLumaMemories[i], nullptr);
            }

            g_diConfidenceViews[i] = VK_NULL_HANDLE;
            g_diConfidenceImages[i] = VK_NULL_HANDLE;
            g_diConfidenceMemories[i] = VK_NULL_HANDLE;
            g_diLumaViews[i] = VK_NULL_HANDLE;
            g_diLumaImages[i] = VK_NULL_HANDLE;
            g_diLumaMemories[i] = VK_NULL_HANDLE;
        }

        if(g_diGradientView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(g_device, g_diGradientView, nullptr);
        }
        if(g_diGradientImage != VK_NULL_HANDLE)
        {
            vkDestroyImage(g_device, g_diGradientImage, nullptr);
        }
        if(g_diGradientMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, g_diGradientMemory, nullptr);
        }

        g_diGradientView = VK_NULL_HANDLE;
        g_diGradientImage = VK_NULL_HANDLE;
        g_diGradientMemory = VK_NULL_HANDLE;
        g_diConfidenceReady = false;
    }

    if(!nrdCreateImage2D(
           width, height, VK_FORMAT_R16_SFLOAT,
           g_diGradientImage, g_diGradientMemory, g_diGradientView,
           g_diGradientLayout, "DI_CONFIDENCE_GRADIENT"))
    {
        return false;
    }

    for(uint32_t i = 0u; i < 2u; ++i)
    {
        if(!nrdCreateImage2D(
               width, height, VK_FORMAT_R8_UNORM,
               g_diConfidenceImages[i], g_diConfidenceMemories[i],
               g_diConfidenceViews[i], g_diConfidenceLayouts[i],
               "DI_CONFIDENCE"))
        {
            return false;
        }

        if(!nrdCreateImage2D(
               width, height, VK_FORMAT_R16_SFLOAT,
               g_diLumaImages[i], g_diLumaMemories[i],
               g_diLumaViews[i], g_diLumaLayouts[i],
               "DI_CONFIDENCE_LUMA"))
        {
            return false;
        }
    }

    g_diConfidenceWidth = width;
    g_diConfidenceHeight = height;
    g_diConfidenceParity = 0u;
    g_diConfidenceReady = true;
    g_diConfidenceWarmupFrames = 2u;
    g_diConfidenceDescriptorsDirty = true;

    RTLog(
        std::string("[DI CONFIDENCE] resources created ") +
        std::to_string(width) + "x" + std::to_string(height)
    );

    return true;
}
bool createDiConfidencePipeline()
{
    if(g_diConfidencePipeline != VK_NULL_HANDLE &&
       g_diConfidenceSets[0] != VK_NULL_HANDLE &&
       g_diConfidenceSets[1] != VK_NULL_HANDLE)
    {
        return true;
    }

    std::vector<uint8_t> code;

    try
    {
        code = loadSpv("shaders/rt_di_confidence.comp.spv");
    }
    catch(const std::exception& loadError)
    {
        RTLog(
            std::string("[DI CONFIDENCE] shader load failed: ") + loadError.what()
        );
        return false;
    }

    if(code.empty())
    {
        RTLog("[DI CONFIDENCE] shader missing, NRD runs without confidence");
        return false;
    }

    VkDescriptorSetLayoutBinding bindings[9]{};
    for(uint32_t i = 0u; i < 9u; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1u;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 9u;
    layoutInfo.pBindings = bindings;

    check(
        vkCreateDescriptorSetLayout(
            g_device, &layoutInfo, nullptr, &g_diConfidenceSetLayout
        ),
        "di confidence set layout failed"
    );

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 2u * 9u;

    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    poolInfo.maxSets = 2u;
    poolInfo.poolSizeCount = 1u;
    poolInfo.pPoolSizes = &poolSize;

    check(
        vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_diConfidencePool),
        "di confidence pool failed"
    );

    VkDescriptorSetLayout layouts[2] = {
        g_diConfidenceSetLayout,
        g_diConfidenceSetLayout
    };

    VkDescriptorSetAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    allocateInfo.descriptorPool = g_diConfidencePool;
    allocateInfo.descriptorSetCount = 2u;
    allocateInfo.pSetLayouts = layouts;

    check(
        vkAllocateDescriptorSets(g_device, &allocateInfo, g_diConfidenceSets),
        "di confidence sets failed"
    );

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0u;
    pushRange.size = sizeof(DiConfidencePushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    pipelineLayoutInfo.setLayoutCount = 1u;
    pipelineLayoutInfo.pSetLayouts = &g_diConfidenceSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1u;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;

    check(
        vkCreatePipelineLayout(
            g_device, &pipelineLayoutInfo, nullptr, &g_diConfidencePipelineLayout
        ),
        "di confidence pipeline layout failed"
    );

    VkShaderModule module = createShaderModule(g_device, code);

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
    pipelineInfo.layout = g_diConfidencePipelineLayout;

    check(
        vkCreateComputePipelines(
            g_device, VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr,
            &g_diConfidencePipeline
        ),
        "di confidence pipeline failed"
    );

    vkDestroyShaderModule(g_device, module, nullptr);

    RTLog("[DI CONFIDENCE] pipeline created");
    return true;
}
void updateDiConfidenceDescriptors()
{
    if(g_diConfidenceSetLayout == VK_NULL_HANDLE ||
       g_diConfidenceSets[0] == VK_NULL_HANDLE ||
       g_diConfidenceSets[1] == VK_NULL_HANDLE ||
       g_diGradientView == VK_NULL_HANDLE ||
       g_diConfidenceViews[0] == VK_NULL_HANDLE ||
       g_diConfidenceViews[1] == VK_NULL_HANDLE ||
       g_diLumaViews[0] == VK_NULL_HANDLE ||
       g_diLumaViews[1] == VK_NULL_HANDLE ||
       g_nrdDiffRadianceHitDistView == VK_NULL_HANDLE ||
       g_nrdMotionView == VK_NULL_HANDLE ||
       g_nrdViewZView == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo infos[2][9]{};
    VkWriteDescriptorSet writes[2][9]{};

    for(uint32_t setIndex = 0u; setIndex < 2u; ++setIndex)
    {
        const uint32_t readIndex = setIndex;
        const uint32_t writeIndex = setIndex ^ 1u;

        const VkImageView views[9] = {
            g_nrdDiffRadianceHitDistView,
            g_nrdMotionView,
            g_nrdViewZView,
            g_diLumaViews[readIndex],
            g_diConfidenceViews[readIndex],
            g_diGradientView,
            g_diGradientView,
            g_diConfidenceViews[writeIndex],
            g_diLumaViews[writeIndex]
        };
        for(uint32_t slot = 0u; slot < 9u; ++slot)
        {
            infos[setIndex][slot].imageView = views[slot];
            infos[setIndex][slot].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            writes[setIndex][slot].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[setIndex][slot].dstSet = g_diConfidenceSets[setIndex];
            writes[setIndex][slot].dstBinding = slot;
            writes[setIndex][slot].dstArrayElement = 0u;
            writes[setIndex][slot].descriptorCount = 1u;
            writes[setIndex][slot].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[setIndex][slot].pImageInfo = &infos[setIndex][slot];
        }
    }

    vkUpdateDescriptorSets(g_device, 2u * 9u, &writes[0][0], 0u, nullptr);
}

// 返回本帧写出的 confidence 视图（NRD 绑它）；失败返回 VK_NULL_HANDLE。
VkImageView recordDiConfidence(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if(cmd == VK_NULL_HANDLE || width == 0u || height == 0u)
    {
        return VK_NULL_HANDLE;
    }

    g_diConfidenceReady = false;

    if(!createDiConfidenceResources(width, height))
    {
        return VK_NULL_HANDLE;
    }

    if(!createDiConfidencePipeline())
    {
        return VK_NULL_HANDLE;
    }

    if(g_diConfidenceDescriptorsDirty)
    {
        updateDiConfidenceDescriptors();
        g_diConfidenceDescriptorsDirty = false;
    }

    g_diConfidenceReady = true;

    if(g_diConfidencePipeline == VK_NULL_HANDLE || g_diConfidencePipelineLayout == VK_NULL_HANDLE)
    {
        return VK_NULL_HANDLE;
    }

    nrdImageBarrier(
        cmd, g_diGradientImage,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT
    );

    nrdImageBarrier(
        cmd, g_nrdDiffRadianceHitDistImage,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT
    );
    nrdImageBarrier(
        cmd, g_nrdMotionImage,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT
    );
    nrdImageBarrier(
        cmd, g_nrdViewZImage,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT
    );

    DiConfidencePushConstants push{};
    push.width = width;
    push.height = height;
    push.gradientSensitivity = NRD_DI_CONFIDENCE_SENSITIVITY;
    push.gradientThreshold = NRD_DI_CONFIDENCE_THRESHOLD;
    push.lumaEmaBeta = NRD_DI_CONFIDENCE_EMA_BETA;
    push.padding0 = 0.0f;
    push.temporalAlpha = g_diConfidenceWarmupFrames > 0u
        ? 1.0f
        : NRD_DI_CONFIDENCE_TEMPORAL_ALPHA;
    push.maxViewZ = NRD_DI_CONFIDENCE_MAX_VIEWZ;

    if(g_diConfidenceWarmupFrames > 0u)
    {
        --g_diConfidenceWarmupFrames;
    }

    const uint32_t groupCountX = (width + 7u) / 8u;
    const uint32_t groupCountY = (height + 7u) / 8u;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_diConfidencePipeline);
    vkCmdBindDescriptorSets(
        cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_diConfidencePipelineLayout,
        0u, 1u, &g_diConfidenceSets[g_diConfidenceParity], 0u, nullptr
    );

    push.mode = 0u;
    vkCmdPushConstants(
        cmd, g_diConfidencePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0u, sizeof(push), &push
    );
    vkCmdDispatch(cmd, groupCountX, groupCountY, 1u);

    nrdImageBarrier(
        cmd, g_diGradientImage,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT
    );

    push.mode = 1u;
    vkCmdPushConstants(
        cmd, g_diConfidencePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0u, sizeof(push), &push
    );
    vkCmdDispatch(cmd, groupCountX, groupCountY, 1u);

    return g_diConfidenceViews[g_diConfidenceParity ^ 1u];
}

#endif
// Resolves one DispatchDesc resource to the VkImageView it must be bound to.
VkImageView nrdResolveResourceView(const nrd::ResourceDesc& resource)
{
    if(resource.type == nrd::ResourceType::PERMANENT_POOL)
    {
        if(resource.indexInPool < g_nrdPermanentViews.size())
        {
            return g_nrdPermanentViews[resource.indexInPool];
        }

        return VK_NULL_HANDLE;
    }

    if(resource.type == nrd::ResourceType::TRANSIENT_POOL)
    {
        if(resource.indexInPool < g_nrdTransientViews.size())
        {
            return g_nrdTransientViews[resource.indexInPool];
        }

        return VK_NULL_HANDLE;
    }

    switch(resource.type)
    {
        case nrd::ResourceType::IN_VIEWZ:                    return g_nrdViewZView;
        case nrd::ResourceType::IN_NORMAL_ROUGHNESS:         return g_nrdNormalRoughnessView;
        case nrd::ResourceType::IN_MV:                       return g_nrdMotionView;
        case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:    return g_nrdDiffRadianceHitDistView;
#if NRD_LOCAL_DI_CONFIDENCE
        case nrd::ResourceType::IN_DIFF_CONFIDENCE:          return g_nrdConfidenceViewThisFrame;
#endif
        case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:   return g_nrdOutDiffRadianceHitDistView;
        case nrd::ResourceType::OUT_VALIDATION:              return g_nrdValidationView;
        default:                                             break;
    }

    return VK_NULL_HANDLE;
}

// Barrier helper.  NRD reads and writes everything as GENERAL storage images,
// and this backend keeps its RT images in GENERAL for their whole life, so only
// the access masks and the stages need ordering.
//
// The source stage set MUST include the ray tracing stage: the three NRD inputs
// are written by the final ReSTIR RayGen (rt_restir_final.rgen), not by a
// compute pass, and a barrier whose srcStageMask omits RAY_TRACING_SHADER does
// not order those stores against NRD's compute dispatches.  The similar
// hand-offs elsewhere in the frame (reservoir buffer, colour image) already
// name VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR for exactly this reason.
void nrdImageBarrier(
    VkCommandBuffer cmd,
    VkImage image,
    VkAccessFlags srcAccess,
    VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0u;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.baseArrayLayer = 0u;
    barrier.subresourceRange.layerCount = 1u;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_TRANSFER_BIT |
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );
}

// 建一张 DI 链的中间图，并把 UNDEFINED -> GENERAL 的转换挂进 barriers。
// nrdCreateImage2D 建出来的图停在 UNDEFINED（NRD 那边靠 nrdTransitionImagesToGeneral
// 补这一步），而本 pass 的所有屏障都假定 GENERAL，所以转换必须显式做。
bool createDiChainImage(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    const char* name,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout,
    VkImageMemoryBarrier* barriers,
    uint32_t& barrierCount)
{
    if(image != VK_NULL_HANDLE)
    {
        nrdDestroyImage(image, memory, view, layout);
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        view = VK_NULL_HANDLE;
        layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if(!nrdCreateImage2D(width, height, format, image, memory, view, layout, name))
    {
        return false;
    }

    if(image == VK_NULL_HANDLE || view == VK_NULL_HANDLE)
    {
        return false;
    }

    VkImageMemoryBarrier& barrier = barriers[barrierCount];
    barrier = VkImageMemoryBarrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = 0u;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0u;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.baseArrayLayer = 0u;
    barrier.subresourceRange.layerCount = 1u;
    ++barrierCount;

    layout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

// 时域的两条管线（rt_di_reproject.comp / rt_di_moments.comp）。
// 自有布局、自有池；不涉及共享 RT 布局。
void createDiTemporalPipelines()
{
    if(g_diReprojectPipeline != VK_NULL_HANDLE &&
       g_diMomentsPipeline != VK_NULL_HANDLE)
    {
        return;
    }

    {
        std::vector<uint8_t> code;

        try
        {
            code = loadSpv("shaders/rt_di_reproject.comp.spv");
        }
        catch(const std::exception& loadError)
        {
            RTLog(
                std::string("[DI REPROJECT] shader load failed: ") +
                loadError.what()
            );
            return;
        }

        if(code.empty())
        {
            RTLog("[DI REPROJECT] shader missing, staying on NRD");
            return;
        }

        VkShaderModule module = createShaderModule(g_device, code);

        VkDescriptorSetLayoutBinding bindings[13]{};
        for(uint32_t i = 0u; i < 13u; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1u;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        layoutInfo.bindingCount =
            static_cast<uint32_t>(sizeof(bindings) / sizeof(bindings[0]));
        layoutInfo.pBindings = bindings;
        check(
            vkCreateDescriptorSetLayout(
                g_device,
                &layoutInfo,
                nullptr,
                &g_diReprojectSetLayout
            ),
            "di reproject set layout failed"
        );

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = 2u * 13u;

        VkDescriptorPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        };
        poolInfo.maxSets = 2u;
        poolInfo.poolSizeCount = 1u;
        poolInfo.pPoolSizes = &poolSize;
        check(
            vkCreateDescriptorPool(
                g_device,
                &poolInfo,
                nullptr,
                &g_diReprojectPool
            ),
            "di reproject pool failed"
        );

        VkDescriptorSetLayout layouts[2] = {
            g_diReprojectSetLayout,
            g_diReprojectSetLayout
        };
        VkDescriptorSetAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        allocateInfo.descriptorPool = g_diReprojectPool;
        allocateInfo.descriptorSetCount = 2u;
        allocateInfo.pSetLayouts = layouts;
        check(
            vkAllocateDescriptorSets(
                g_device,
                &allocateInfo,
                g_diReprojectSets
            ),
            "di reproject sets failed"
        );

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size = sizeof(DiReprojectPushConstants);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_diReprojectSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(
            vkCreatePipelineLayout(
                g_device,
                &pipelineLayoutInfo,
                nullptr,
                &g_diReprojectPipelineLayout
            ),
            "di reproject pipeline layout failed"
        );

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
        pipelineInfo.layout = g_diReprojectPipelineLayout;
        check(
            vkCreateComputePipelines(
                g_device,
                VK_NULL_HANDLE,
                1,
                &pipelineInfo,
                nullptr,
                &g_diReprojectPipeline
            ),
            "di reproject pipeline failed"
        );

        vkDestroyShaderModule(g_device, module, nullptr);
    }

    {
        std::vector<uint8_t> code;

        try
        {
            code = loadSpv("shaders/rt_di_moments.comp.spv");
        }
        catch(const std::exception& loadError)
        {
            RTLog(
                std::string("[DI MOMENTS] shader load failed: ") +
                loadError.what()
            );
            return;
        }

        if(code.empty())
        {
            RTLog("[DI MOMENTS] shader missing, staying on NRD");
            return;
        }

        VkShaderModule module = createShaderModule(g_device, code);

        VkDescriptorSetLayoutBinding bindings[5]{};
        for(uint32_t i = 0u; i < 5u; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1u;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        layoutInfo.bindingCount =
            static_cast<uint32_t>(sizeof(bindings) / sizeof(bindings[0]));
        layoutInfo.pBindings = bindings;
        check(
            vkCreateDescriptorSetLayout(
                g_device,
                &layoutInfo,
                nullptr,
                &g_diMomentsSetLayout
            ),
            "di moments set layout failed"
        );

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = 2u * 5u;

        VkDescriptorPoolCreateInfo poolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        };
        poolInfo.maxSets = 2u;
        poolInfo.poolSizeCount = 1u;
        poolInfo.pPoolSizes = &poolSize;
        check(
            vkCreateDescriptorPool(
                g_device,
                &poolInfo,
                nullptr,
                &g_diMomentsPool
            ),
            "di moments pool failed"
        );

        VkDescriptorSetLayout layouts[2] = {
            g_diMomentsSetLayout,
            g_diMomentsSetLayout
        };
        VkDescriptorSetAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        allocateInfo.descriptorPool = g_diMomentsPool;
        allocateInfo.descriptorSetCount = 2u;
        allocateInfo.pSetLayouts = layouts;
        check(
            vkAllocateDescriptorSets(
                g_device,
                &allocateInfo,
                g_diMomentsSets
            ),
            "di moments sets failed"
        );

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size = sizeof(DiMomentsPushConstants);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_diMomentsSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(
            vkCreatePipelineLayout(
                g_device,
                &pipelineLayoutInfo,
                nullptr,
                &g_diMomentsPipelineLayout
            ),
            "di moments pipeline layout failed"
        );

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
        pipelineInfo.layout = g_diMomentsPipelineLayout;
        check(
            vkCreateComputePipelines(
                g_device,
                VK_NULL_HANDLE,
                1,
                &pipelineInfo,
                nullptr,
                &g_diMomentsPipeline
            ),
            "di moments pipeline failed"
        );

        vkDestroyShaderModule(g_device, module, nullptr);
    }

    RTLog("[DI TEMPORAL] reproject + moments pipelines created");
}
void createDirectAtrousPipeline()
{
    if(g_directAtrousPipeline != VK_NULL_HANDLE)
    {
        return;
    }

    std::vector<uint8_t> code;
    try
    {
        code = loadSpv("shaders/rt_direct_atrous.comp.spv");
    }
    catch(const std::exception& loadError)
    {
        RTLog(
            std::string("[DIRECT ATROUS] shader load failed: ") +
            loadError.what()
        );
        return;
    }

    if(code.empty())
    {
        RTLog("[DIRECT ATROUS] shader missing, staying on NRD");
        return;
    }

    VkShaderModule shaderModule = createShaderModule(g_device, code);

    VkDescriptorSetLayoutBinding bindings[6]{};
    for(uint32_t i = 0u; i < 6u; ++i)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1u;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = static_cast<uint32_t>(sizeof(bindings) / sizeof(bindings[0]));
    layoutInfo.pBindings = bindings;
    static_assert(
        sizeof(bindings) / sizeof(bindings[0]) == 6u,
        "direct atrous layout: keep this assert in sync with rt_direct_atrous.comp"
    );
    check(
        vkCreateDescriptorSetLayout(
            g_device,
            &layoutInfo,
            nullptr,
            &g_directAtrousSetLayout
        ),
        "direct atrous set layout failed"
    );

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 3u * 6u;

    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    poolInfo.maxSets = 3u;
    poolInfo.poolSizeCount = 1u;
    poolInfo.pPoolSizes = &poolSize;
    check(
        vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_directAtrousPool),
        "direct atrous pool failed"
    );

    VkDescriptorSetLayout layouts[3] = {
        g_directAtrousSetLayout,
        g_directAtrousSetLayout,
        g_directAtrousSetLayout
    };
    VkDescriptorSetAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    allocateInfo.descriptorPool = g_directAtrousPool;
    allocateInfo.descriptorSetCount = 3u;
    allocateInfo.pSetLayouts = layouts;
    check(
        vkAllocateDescriptorSets(g_device, &allocateInfo, g_directAtrousSets),
        "direct atrous sets failed"
    );

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0u;
    pushRange.size = sizeof(DirectAtrousPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    pipelineLayoutInfo.setLayoutCount = 1u;
    pipelineLayoutInfo.pSetLayouts = &g_directAtrousSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1u;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    check(
        vkCreatePipelineLayout(
            g_device,
            &pipelineLayoutInfo,
            nullptr,
            &g_directAtrousPipelineLayout
        ),
        "direct atrous pipeline layout failed"
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
    pipelineInfo.layout = g_directAtrousPipelineLayout;
    check(
        vkCreateComputePipelines(
            g_device,
            VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            &g_directAtrousPipeline
        ),
        "direct atrous pipeline failed"
    );

    vkDestroyShaderModule(g_device, shaderModule, nullptr);

    createDiTemporalPipelines();

    {
        std::vector<uint8_t> prepareCode;

        try
        {
            prepareCode = loadSpv("shaders/rt_di_prepare.comp.spv");
        }
        catch(const std::exception& loadError)
        {
            RTLog(
                std::string("[DI PREPARE] shader load failed: ") +
                loadError.what()
            );
            return;
        }

        if(prepareCode.empty())
        {
            RTLog("[DI PREPARE] shader missing, staying on NRD");
            return;
        }

        VkShaderModule prepareModule = createShaderModule(g_device, prepareCode);

        VkDescriptorSetLayoutBinding prepareBindings[3]{};
        for(uint32_t i = 0u; i < 3u; ++i)
        {
            prepareBindings[i].binding = i;
            prepareBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            prepareBindings[i].descriptorCount = 1u;
            prepareBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo prepareLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        prepareLayoutInfo.bindingCount = 3u;
        prepareLayoutInfo.pBindings = prepareBindings;
        check(
            vkCreateDescriptorSetLayout(
                g_device,
                &prepareLayoutInfo,
                nullptr,
                &g_diPrepareSetLayout
            ),
            "di prepare set layout failed"
        );

        VkDescriptorPoolSize preparePoolSize{};
        preparePoolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        preparePoolSize.descriptorCount = 3u;

        VkDescriptorPoolCreateInfo preparePoolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        };
        preparePoolInfo.maxSets = 1u;
        preparePoolInfo.poolSizeCount = 1u;
        preparePoolInfo.pPoolSizes = &preparePoolSize;
        check(
            vkCreateDescriptorPool(
                g_device,
                &preparePoolInfo,
                nullptr,
                &g_diPreparePool
            ),
            "di prepare pool failed"
        );

        VkDescriptorSetAllocateInfo prepareAllocate{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        prepareAllocate.descriptorPool = g_diPreparePool;
        prepareAllocate.descriptorSetCount = 1u;
        prepareAllocate.pSetLayouts = &g_diPrepareSetLayout;
        check(
            vkAllocateDescriptorSets(
                g_device,
                &prepareAllocate,
                &g_diPrepareSet
            ),
            "di prepare set failed"
        );

        VkPushConstantRange preparePushRange{};
        preparePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        preparePushRange.offset = 0u;
        preparePushRange.size = sizeof(DiPreparePushConstants);

        VkPipelineLayoutCreateInfo preparePipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        preparePipelineLayoutInfo.setLayoutCount = 1u;
        preparePipelineLayoutInfo.pSetLayouts = &g_diPrepareSetLayout;
        preparePipelineLayoutInfo.pushConstantRangeCount = 1u;
        preparePipelineLayoutInfo.pPushConstantRanges = &preparePushRange;
        check(
            vkCreatePipelineLayout(
                g_device,
                &preparePipelineLayoutInfo,
                nullptr,
                &g_diPreparePipelineLayout
            ),
            "di prepare pipeline layout failed"
        );

        VkPipelineShaderStageCreateInfo prepareStage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        prepareStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        prepareStage.module = prepareModule;
        prepareStage.pName = "main";

        VkComputePipelineCreateInfo preparePipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        preparePipelineInfo.stage = prepareStage;
        preparePipelineInfo.layout = g_diPreparePipelineLayout;
        check(
            vkCreateComputePipelines(
                g_device,
                VK_NULL_HANDLE,
                1,
                &preparePipelineInfo,
                nullptr,
                &g_diPreparePipeline
            ),
            "di prepare pipeline failed"
        );

        vkDestroyShaderModule(g_device, prepareModule, nullptr);
    }

    RTLog(
        "[DIRECT ATROUS] pipeline created (spatial chain = prepare + 5x atrous)"
    );
}
void updateDirectAtrousDescriptors()
{
    if(g_directAtrousSetLayout == VK_NULL_HANDLE ||
       g_directAtrousSets[0] == VK_NULL_HANDLE ||
       g_directAtrousSets[2] == VK_NULL_HANDLE ||
       g_diPrepareSetLayout == VK_NULL_HANDLE ||
       g_diPrepareSet == VK_NULL_HANDLE ||
       g_dlssDepthView == VK_NULL_HANDLE ||
       g_rrNormalRoughnessView == VK_NULL_HANDLE ||
       g_nrdDiffRadianceHitDistView == VK_NULL_HANDLE ||
       g_nrdOutDiffRadianceHitDistView == VK_NULL_HANDLE ||
       g_diLinearViews[0] == VK_NULL_HANDLE ||
       g_diLinearViews[1] == VK_NULL_HANDLE ||
       g_diVarianceViews[0] == VK_NULL_HANDLE ||
       g_diVarianceViews[1] == VK_NULL_HANDLE)
    {
        return;
    }

    const VkImageView atrousViews[3][6] = {
        {
            g_diLinearViews[0], g_diLinearViews[1],
            g_dlssDepthView, g_rrNormalRoughnessView,
            g_diVarianceViews[0], g_diVarianceViews[1]
        },
        {
            g_diLinearViews[1], g_diLinearViews[0],
            g_dlssDepthView, g_rrNormalRoughnessView,
            g_diVarianceViews[1], g_diVarianceViews[0]
        },
        {
            g_diLinearViews[0], g_nrdOutDiffRadianceHitDistView,
            g_dlssDepthView, g_rrNormalRoughnessView,
            g_diVarianceViews[0], g_diVarianceViews[1]
        }
    };
    VkDescriptorImageInfo atrousInfos[3][6]{};
    VkWriteDescriptorSet atrousWrites[3][6]{};

    for(uint32_t setIndex = 0u; setIndex < 3u; ++setIndex)
    {
        for(uint32_t slot = 0u; slot < 6u; ++slot)
        {
            atrousInfos[setIndex][slot].imageView =
                atrousViews[setIndex][slot];
            atrousInfos[setIndex][slot].imageLayout =
                VK_IMAGE_LAYOUT_GENERAL;

            atrousWrites[setIndex][slot].sType =
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            atrousWrites[setIndex][slot].dstSet =
                g_directAtrousSets[setIndex];
            atrousWrites[setIndex][slot].dstBinding = slot;
            atrousWrites[setIndex][slot].dstArrayElement = 0u;
            atrousWrites[setIndex][slot].descriptorCount = 1u;
            atrousWrites[setIndex][slot].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            atrousWrites[setIndex][slot].pImageInfo =
                &atrousInfos[setIndex][slot];
        }
    }

    vkUpdateDescriptorSets(
        g_device,
        3u * 6u,
        &atrousWrites[0][0],
        0u,
        nullptr
    );

    if(g_diReprojectSets[0] != VK_NULL_HANDLE &&
       g_diReprojectSets[1] != VK_NULL_HANDLE &&
       g_diMomentsSets[0] != VK_NULL_HANDLE &&
       g_diMomentsSets[1] != VK_NULL_HANDLE &&
       g_diHistoryColorViews[0] != VK_NULL_HANDLE &&
       g_diHistoryColorViews[1] != VK_NULL_HANDLE &&
       g_diHistoryDepthViews[0] != VK_NULL_HANDLE &&
       g_diHistoryDepthViews[1] != VK_NULL_HANDLE &&
       g_diHistoryNormalViews[0] != VK_NULL_HANDLE &&
       g_diHistoryNormalViews[1] != VK_NULL_HANDLE &&
       g_diHistoryLengthViews[0] != VK_NULL_HANDLE &&
       g_diHistoryLengthViews[1] != VK_NULL_HANDLE &&
       g_diMomentsViews[0] != VK_NULL_HANDLE &&
       g_diMomentsViews[1] != VK_NULL_HANDLE &&
       g_nrdMotionView != VK_NULL_HANDLE &&
       g_nrdViewZView != VK_NULL_HANDLE)
    {
        VkDescriptorImageInfo reprojectInfos[2][13]{};
        VkWriteDescriptorSet reprojectWrites[2][13]{};

        for(uint32_t setIndex = 0u; setIndex < 2u; ++setIndex)
        {
            const uint32_t readIndex = setIndex;
            const uint32_t writeIndex = setIndex ^ 1u;

            const VkImageView views[13] = {
                g_nrdDiffRadianceHitDistView,
                g_nrdMotionView,
                g_nrdViewZView,
                g_rrNormalRoughnessView,
                g_diHistoryColorViews[readIndex],
                g_diHistoryDepthViews[readIndex],
                g_diHistoryNormalViews[readIndex],
                g_diHistoryColorViews[writeIndex],
                g_diHistoryDepthViews[writeIndex],
                g_diHistoryNormalViews[writeIndex],
                g_diHistoryLengthViews[readIndex],
                g_diHistoryLengthViews[writeIndex],
                g_diLinearViews[0]
            };
            for(uint32_t slot = 0u; slot < 13u; ++slot)
            {
                reprojectInfos[setIndex][slot].imageView = views[slot];
                reprojectInfos[setIndex][slot].imageLayout =
                    VK_IMAGE_LAYOUT_GENERAL;

                reprojectWrites[setIndex][slot].sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                reprojectWrites[setIndex][slot].dstSet =
                    g_diReprojectSets[setIndex];
                reprojectWrites[setIndex][slot].dstBinding = slot;
                reprojectWrites[setIndex][slot].dstArrayElement = 0u;
                reprojectWrites[setIndex][slot].descriptorCount = 1u;
                reprojectWrites[setIndex][slot].descriptorType =
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                reprojectWrites[setIndex][slot].pImageInfo =
                    &reprojectInfos[setIndex][slot];
            }
        }

        vkUpdateDescriptorSets(
            g_device,
            2u * 13u,
            &reprojectWrites[0][0],
            0u,
            nullptr
        );

        VkDescriptorImageInfo momentsInfos[2][5]{};
        VkWriteDescriptorSet momentsWrites[2][5]{};

        for(uint32_t setIndex = 0u; setIndex < 2u; ++setIndex)
        {
            const uint32_t readIndex = setIndex;
            const uint32_t writeIndex = setIndex ^ 1u;

            const VkImageView views[5] = {
                g_diLinearViews[0],
                g_diMomentsViews[readIndex],
                g_diMomentsViews[writeIndex],
                g_diVarianceViews[0],
                g_diHistoryLengthViews[writeIndex]
            };
            for(uint32_t slot = 0u; slot < 5u; ++slot)
            {
                momentsInfos[setIndex][slot].imageView = views[slot];
                momentsInfos[setIndex][slot].imageLayout =
                    VK_IMAGE_LAYOUT_GENERAL;

                momentsWrites[setIndex][slot].sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                momentsWrites[setIndex][slot].dstSet =
                    g_diMomentsSets[setIndex];
                momentsWrites[setIndex][slot].dstBinding = slot;
                momentsWrites[setIndex][slot].dstArrayElement = 0u;
                momentsWrites[setIndex][slot].descriptorCount = 1u;
                momentsWrites[setIndex][slot].descriptorType =
                    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                momentsWrites[setIndex][slot].pImageInfo =
                    &momentsInfos[setIndex][slot];
            }
        }

        vkUpdateDescriptorSets(
            g_device,
            2u * 5u,
            &momentsWrites[0][0],
            0u,
            nullptr
        );
    }

    const VkImageView prepareViews[3] = {
        g_nrdDiffRadianceHitDistView,
        g_diLinearViews[0],
        g_diVarianceViews[0]
    };

    VkDescriptorImageInfo prepareInfos[3]{};
    VkWriteDescriptorSet prepareWrites[3]{};

    for(uint32_t slot = 0u; slot < 3u; ++slot)
    {
        prepareInfos[slot].imageView = prepareViews[slot];
        prepareInfos[slot].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        prepareWrites[slot].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        prepareWrites[slot].dstSet = g_diPrepareSet;
        prepareWrites[slot].dstBinding = slot;
        prepareWrites[slot].dstArrayElement = 0u;
        prepareWrites[slot].descriptorCount = 1u;
        prepareWrites[slot].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        prepareWrites[slot].pImageInfo = &prepareInfos[slot];
    }

    vkUpdateDescriptorSets(g_device, 3u, prepareWrites, 0u, nullptr);
}
void recordDirectAtrous(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height)
{
    if(cmd == VK_NULL_HANDLE || !g_directAtrousEnabled)
    {
        return;
    }

    createDirectAtrousPipeline();

    g_directAtrousReady = false;

    if(g_directAtrousPipeline == VK_NULL_HANDLE ||
       g_diReprojectPipeline == VK_NULL_HANDLE ||
       g_diMomentsPipeline == VK_NULL_HANDLE ||
       g_nrdDiffRadianceHitDistImage == VK_NULL_HANDLE ||
       g_nrdOutDiffRadianceHitDistImage == VK_NULL_HANDLE ||
       g_nrdMotionImage == VK_NULL_HANDLE ||
       g_nrdViewZImage == VK_NULL_HANDLE)
    {
        RTLog("[DI TEMPORAL] not ready, falling back to NRD");
        return;
    }

    if(g_directAtrousWidth != width || g_directAtrousHeight != height)
    {
        for(uint32_t i = 0u; i < 2u; ++i)
        {
            if(g_diLinearImages[i] != VK_NULL_HANDLE)
            {
                nrdDestroyImage(
                    g_diLinearImages[i],
                    g_diLinearMemories[i],
                    g_diLinearViews[i],
                    g_diLinearLayouts[i]
                );

                g_diLinearImages[i] = VK_NULL_HANDLE;
                g_diLinearMemories[i] = VK_NULL_HANDLE;
                g_diLinearViews[i] = VK_NULL_HANDLE;
                g_diLinearLayouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
            }

            if(g_diVarianceImages[i] != VK_NULL_HANDLE)
            {
                nrdDestroyImage(
                    g_diVarianceImages[i],
                    g_diVarianceMemories[i],
                    g_diVarianceViews[i],
                    g_diVarianceLayouts[i]
                );

                g_diVarianceImages[i] = VK_NULL_HANDLE;
                g_diVarianceMemories[i] = VK_NULL_HANDLE;
                g_diVarianceViews[i] = VK_NULL_HANDLE;
                g_diVarianceLayouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
            }
        }

        bool imagesReady = true;

        for(uint32_t i = 0u; i < 2u && imagesReady; ++i)
        {
            imagesReady = nrdCreateImage2D(
                width,
                height,
                VK_FORMAT_R16G16B16A16_SFLOAT,
                g_diLinearImages[i],
                g_diLinearMemories[i],
                g_diLinearViews[i],
                g_diLinearLayouts[i],
                "DI_LINEAR"
            );

            if(imagesReady)
            {
                imagesReady = nrdCreateImage2D(
                    width,
                    height,
                    VK_FORMAT_R16_SFLOAT,
                    g_diVarianceImages[i],
                    g_diVarianceMemories[i],
                    g_diVarianceViews[i],
                    g_diVarianceLayouts[i],
                    "DI_VARIANCE"
                );
            }
        }

        if(!imagesReady ||
           g_diLinearViews[0] == VK_NULL_HANDLE ||
           g_diLinearViews[1] == VK_NULL_HANDLE ||
           g_diVarianceViews[0] == VK_NULL_HANDLE ||
           g_diVarianceViews[1] == VK_NULL_HANDLE)
        {
            RTLog("[DIRECT ATROUS] image creation failed, staying on NRD");
            return;
        }

        {
            const VkImage fresh[4] = {
                g_diLinearImages[0],
                g_diLinearImages[1],
                g_diVarianceImages[0],
                g_diVarianceImages[1]
            };

            VkImageMemoryBarrier initial[4]{};

            for(uint32_t i = 0u; i < 4u; ++i)
            {
                initial[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                initial[i].srcAccessMask = 0u;
                initial[i].dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                initial[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                initial[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                initial[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                initial[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                initial[i].image = fresh[i];
                initial[i].subresourceRange.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                initial[i].subresourceRange.baseMipLevel = 0u;
                initial[i].subresourceRange.levelCount = 1u;
                initial[i].subresourceRange.baseArrayLayer = 0u;
                initial[i].subresourceRange.layerCount = 1u;
            }

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                4, initial
            );
        }

        for(uint32_t i = 0u; i < 2u; ++i)
        {
            g_diLinearLayouts[i] = VK_IMAGE_LAYOUT_GENERAL;
            g_diVarianceLayouts[i] = VK_IMAGE_LAYOUT_GENERAL;
        }

        {
            VkImageMemoryBarrier historyBarriers[10]{};
            uint32_t historyBarrierCount = 0u;
            bool historyReady = true;

            for(uint32_t i = 0u; i < 2u && historyReady; ++i)
            {
                historyReady =
                    createDiChainImage(
                        width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                        "DI_HIST_COLOR",
                        g_diHistoryColorImages[i], g_diHistoryColorMemories[i],
                        g_diHistoryColorViews[i], g_diHistoryColorLayouts[i],
                        historyBarriers, historyBarrierCount
                    ) &&
                    createDiChainImage(
                        width, height, VK_FORMAT_R32_SFLOAT,
                        "DI_HIST_DEPTH",
                        g_diHistoryDepthImages[i], g_diHistoryDepthMemories[i],
                        g_diHistoryDepthViews[i], g_diHistoryDepthLayouts[i],
                        historyBarriers, historyBarrierCount
                    ) &&
                    createDiChainImage(
                        width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                        "DI_HIST_NORMAL",
                        g_diHistoryNormalImages[i], g_diHistoryNormalMemories[i],
                        g_diHistoryNormalViews[i], g_diHistoryNormalLayouts[i],
                        historyBarriers, historyBarrierCount
                    ) &&
                    createDiChainImage(
                        width, height, VK_FORMAT_R16_UINT,
                        "DI_HIST_LENGTH",
                        g_diHistoryLengthImages[i], g_diHistoryLengthMemories[i],
                        g_diHistoryLengthViews[i], g_diHistoryLengthLayouts[i],
                        historyBarriers, historyBarrierCount
                    ) &&
                    createDiChainImage(
                        width, height, VK_FORMAT_R16G16B16A16_SFLOAT,
                        "DI_MOMENTS",
                        g_diMomentsImages[i], g_diMomentsMemories[i],
                        g_diMomentsViews[i], g_diMomentsLayouts[i],
                        historyBarriers, historyBarrierCount
                    );
            }

            if(!historyReady || historyBarrierCount != 10u)
            {
                RTLog(
                    "[DI TEMPORAL] history image creation failed, "
                    "staying on NRD"
                );
                return;
            }

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                historyBarrierCount, historyBarriers
            );

            g_diHistoryNeedsReset = true;
            g_diHistoryParity = 0u;
        }

        updateDirectAtrousDescriptors();
        g_directAtrousWidth = width;
        g_directAtrousHeight = height;

        RTLog(
            "[DIRECT ATROUS] dispatch " +
            std::to_string(width) + "x" + std::to_string(height) +
            " spatial chain: prepare + 5x atrous [1,2,4,8,16]"
        );
    }

    VkImageMemoryBarrier toCompute[2]{};
    const VkImage images[2] = {
        g_nrdDiffRadianceHitDistImage,
        g_rrNormalRoughnessImage
    };
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        toCompute[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toCompute[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        toCompute[i].dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        toCompute[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        toCompute[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toCompute[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCompute[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toCompute[i].image = images[i];
        toCompute[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toCompute[i].subresourceRange.baseMipLevel = 0u;
        toCompute[i].subresourceRange.levelCount = 1u;
        toCompute[i].subresourceRange.baseArrayLayer = 0u;
        toCompute[i].subresourceRange.layerCount = 1u;
    }

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        2, toCompute
    );

    const uint32_t groupCountX = (width + 7u) / 8u;
    const uint32_t groupCountY = (height + 7u) / 8u;

    const uint32_t historySetIndex = g_diHistoryParity & 1u;
    const uint32_t historyWriteIndex = historySetIndex ^ 1u;

    {
        DiReprojectPushConstants reprojectPush{};
        reprojectPush.width = width;
        reprojectPush.height = height;
        reprojectPush.resetHistory = g_diHistoryNeedsReset ? 1u : 0u;

        vkCmdBindPipeline(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_diReprojectPipeline
        );
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_diReprojectPipelineLayout,
            0u,
            1u,
            &g_diReprojectSets[historySetIndex],
            0u,
            nullptr
        );
        vkCmdPushConstants(
            cmd,
            g_diReprojectPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0u,
            sizeof(reprojectPush),
            &reprojectPush
        );
        vkCmdDispatch(cmd, groupCountX, groupCountY, 1u);
    }

    {
        const VkImage reprojectOutputs[2] = {
            g_diLinearImages[0],
            g_diHistoryLengthImages[historyWriteIndex]
        };

        VkImageMemoryBarrier toMoments[2]{};

        for(uint32_t i = 0u; i < 2u; ++i)
        {
            toMoments[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toMoments[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toMoments[i].dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            toMoments[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toMoments[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toMoments[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toMoments[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toMoments[i].image = reprojectOutputs[i];
            toMoments[i].subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            toMoments[i].subresourceRange.baseMipLevel = 0u;
            toMoments[i].subresourceRange.levelCount = 1u;
            toMoments[i].subresourceRange.baseArrayLayer = 0u;
            toMoments[i].subresourceRange.layerCount = 1u;
        }

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            2, toMoments
        );
    }

    {
        DiMomentsPushConstants momentsPush{};
        momentsPush.width = width;
        momentsPush.height = height;

        vkCmdBindPipeline(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_diMomentsPipeline
        );
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_diMomentsPipelineLayout,
            0u,
            1u,
            &g_diMomentsSets[historySetIndex],
            0u,
            nullptr
        );
        vkCmdPushConstants(
            cmd,
            g_diMomentsPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0u,
            sizeof(momentsPush),
            &momentsPush
        );
        vkCmdDispatch(cmd, groupCountX, groupCountY, 1u);
    }

    {
        const VkImage prepareOutputs[2] = {
            g_diLinearImages[0],
            g_diVarianceImages[0]
        };

        VkImageMemoryBarrier toAtrous[2]{};

        for(uint32_t i = 0u; i < 2u; ++i)
        {
            toAtrous[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toAtrous[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toAtrous[i].dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            toAtrous[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toAtrous[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toAtrous[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toAtrous[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toAtrous[i].image = prepareOutputs[i];
            toAtrous[i].subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            toAtrous[i].subresourceRange.baseMipLevel = 0u;
            toAtrous[i].subresourceRange.levelCount = 1u;
            toAtrous[i].subresourceRange.baseArrayLayer = 0u;
            toAtrous[i].subresourceRange.layerCount = 1u;
        }

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            2, toAtrous
        );
    }

    static const uint32_t atrousSteps[5] = { 1u, 2u, 4u, 8u, 16u };

    const VkImage stepColorOut[4] = {
        g_diLinearImages[1],
        g_diLinearImages[0],
        g_diLinearImages[1],
        g_diLinearImages[0]
    };
    const VkImage stepVarianceOut[4] = {
        g_diVarianceImages[1],
        g_diVarianceImages[0],
        g_diVarianceImages[1],
        g_diVarianceImages[0]
    };

    for(uint32_t step = 0u; step < 5u; ++step)
    {
        const bool isChainTail = (step == 4u);
        const uint32_t setIndex = isChainTail ? 2u : (step & 1u);

        DirectAtrousPushConstants push{};
        push.width = width;
        push.height = height;
        push.stepSize = atrousSteps[step];
        push.phiColor = 4.0f;
        push.encodeToYCoCg = isChainTail ? 1u : 0u;
        vkCmdBindPipeline(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_directAtrousPipeline
        );
        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_directAtrousPipelineLayout,
            0u,
            1u,
            &g_directAtrousSets[setIndex],
            0u,
            nullptr
        );
        vkCmdPushConstants(
            cmd,
            g_directAtrousPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0u,
            sizeof(push),
            &push
        );
        vkCmdDispatch(cmd, groupCountX, groupCountY, 1u);

        if(step < 4u)
        {
            const VkImage written[2] = {
                stepColorOut[step],
                stepVarianceOut[step]
            };

            VkImageMemoryBarrier between[2]{};

            for(uint32_t i = 0u; i < 2u; ++i)
            {
                between[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                between[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                between[i].dstAccessMask =
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                between[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                between[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                between[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                between[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                between[i].image = written[i];
                between[i].subresourceRange.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                between[i].subresourceRange.baseMipLevel = 0u;
                between[i].subresourceRange.levelCount = 1u;
                between[i].subresourceRange.baseArrayLayer = 0u;
                between[i].subresourceRange.layerCount = 1u;
            }

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                2, between
            );
        }
    }

    VkImageMemoryBarrier finalReady{};
    finalReady.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    finalReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    finalReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    finalReady.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    finalReady.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    finalReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    finalReady.image = g_nrdOutDiffRadianceHitDistImage;
    finalReady.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    finalReady.subresourceRange.baseMipLevel = 0u;
    finalReady.subresourceRange.levelCount = 1u;
    finalReady.subresourceRange.baseArrayLayer = 0u;
    finalReady.subresourceRange.layerCount = 1u;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &finalReady
    );

    g_diHistoryParity ^= 1u;
    g_diHistoryNeedsReset = false;

    g_directAtrousReady = true;
}

// Per-frame NRD dispatch.  Must run after the ReSTIR final RayGen (which writes
// IN_DIFF_RADIANCE_HITDIST, IN_VIEWZ and IN_NORMAL_ROUGHNESS) and before the
// composite pass that reads OUT_DIFF_RADIANCE_HITDIST.
void recordNRDLocalDenoiser(VkCommandBuffer cmd, uint32_t width, uint32_t height)
{
    if(!g_nrdReady || g_nrdInstance == nullptr || cmd == VK_NULL_HANDLE)
    {
        return;
    }

    if(width != g_nrdWidth || height != g_nrdHeight)
    {
        return;
    }

    if(g_nrdDescriptorSets.size() < NRD_MAX_DISPATCHES ||
       g_nrdConstantSets.size() < NRD_MAX_DISPATCHES ||
       g_nrdPipelines.empty())
    {
        return;
    }

    {
        static auto lastFrameTime = std::chrono::steady_clock::now();

        const auto now = std::chrono::steady_clock::now();

        const float frameTimeMs = std::chrono::duration<float, std::milli>(
            now - lastFrameTime
        ).count();

        lastFrameTime = now;

        if(frameTimeMs > 0.0f && frameTimeMs < 1000.0f)
        {
            const float smoothedFpsPrev = 1000.0f / std::max(1.0f, g_nrdSmoothedFrameTimeMs);
            const float n = smoothedFpsPrev * 0.2f;

            g_nrdSmoothedFrameTimeMs =
                g_nrdSmoothedFrameTimeMs +
                (frameTimeMs - g_nrdSmoothedFrameTimeMs) / (1.0f + n);
        }
    }

    nrd::CommonSettings commonSettings{};
    nrdBuildCommonSettings(commonSettings, width, height);

    static const int s_nrdDiagDiffW = []() -> int
    {
        const char* s = std::getenv("RTX_NRD_DIAG_DIFF_W");
        return s ? std::atoi(s) : 0;
    }();
    static const bool s_nrdDiagLogged = []() -> bool
    {
        RTLog(
            "[NRD-1 DIAG] RTX_NRD_DIAG_DIFF_W=" + std::to_string(s_nrdDiagDiffW) +
            "  (0=off 1=4tap*sizeQ 2=sizeQuality 3=4tap 4=historyLen)"
        );
        return true;
    }();
    (void)s_nrdDiagLogged;
    commonSettings.debug = static_cast<float>(s_nrdDiagDiffW);

    if(nrd::SetCommonSettings(*g_nrdInstance, commonSettings) != nrd::Result::SUCCESS)
    {
        RTLog("[NRD-1] SetCommonSettings failed");
        return;
    }

    nrd::ReblurSettings reblurSettings{};
    nrd::RelaxSettings relaxSettings{};
    const void* denoiserSettings = nullptr;

#if NRD_LOCAL_DI_USE_RELAX
    nrdBuildRelaxSettings(relaxSettings);

    denoiserSettings = &relaxSettings;
#else
    nrdBuildReblurSettings(reblurSettings);
    denoiserSettings = &reblurSettings;
#endif

    if(nrd::SetDenoiserSettings(*g_nrdInstance, g_nrdIdentifier, denoiserSettings) != nrd::Result::SUCCESS)
    {
        RTLog("[NRD-1] SetDenoiserSettings failed");
        return;
    }

    const nrd::Identifier identifiers[] = {g_nrdIdentifier};
    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchCount = 0u;

    if(nrd::GetComputeDispatches(*g_nrdInstance, identifiers, 1u, dispatches, dispatchCount) != nrd::Result::SUCCESS)
    {
        RTLog("[NRD-1] GetComputeDispatches failed");
        return;
    }

    if(dispatchCount == 0u || dispatches == nullptr)
    {
        return;
    }

    if(dispatchCount > NRD_MAX_DISPATCHES)
    {
        RTLog(
            std::string("[NRD-1] dispatch count ") + std::to_string(dispatchCount) +
            " exceeds the constant-buffer ring; skipping NRD this frame"
        );
        return;
    }

    nrdImageBarrier(
        cmd,
        g_nrdViewZImage,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT
    );
    nrdImageBarrier(
        cmd,
        g_nrdNormalRoughnessImage,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT
    );
    nrdImageBarrier(
        cmd,
        g_nrdDiffRadianceHitDistImage,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT
    );
    nrdImageBarrier(
        cmd,
        g_nrdMotionImage,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT
    );

    for(size_t i = 0u; i < g_nrdPermanentImages.size(); ++i)
    {
        nrdImageBarrier(
            cmd,
            g_nrdPermanentImages[i],
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
        );
    }

    for(size_t i = 0u; i < g_nrdTransientImages.size(); ++i)
    {
        nrdImageBarrier(
            cmd,
            g_nrdTransientImages[i],
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
        );
    }

    g_nrdConstantBufferOffsets.assign(dispatchCount, 0u);

    uint32_t previousSlot = 0u;
    bool previousSlotValid = false;

    for(uint32_t i = 0u; i < dispatchCount; ++i)
    {
        const nrd::DispatchDesc& dispatch = dispatches[i];

        uint32_t slot = i;

        if(dispatch.constantBufferDataMatchesPreviousDispatch && previousSlotValid)
        {
            slot = previousSlot;
        }

        g_nrdConstantBufferOffsets[i] = slot;
        previousSlot = slot;
        previousSlotValid = true;

        if(dispatch.constantBufferData == nullptr || dispatch.constantBufferDataSize == 0u)
        {
            continue;
        }

        if(dispatch.constantBufferDataMatchesPreviousDispatch && slot != i)
        {
            continue;
        }

        const VkDeviceSize offset = g_nrdConstantBufferStride * slot;

        if(offset + dispatch.constantBufferDataSize > g_nrdConstantBufferStride * NRD_MAX_DISPATCHES)
        {
            continue;
        }

        if(dispatch.constantBufferDataSize % 4u != 0u ||
           dispatch.constantBufferDataSize > 65536u)
        {
            RTLog(
                std::string("[NRD-1] constant buffer size ") +
                std::to_string(dispatch.constantBufferDataSize) +
                " cannot be uploaded with vkCmdUpdateBuffer"
            );
            continue;
        }

        vkCmdUpdateBuffer(
            cmd,
            g_nrdConstantBuffer,
            offset,
            dispatch.constantBufferDataSize,
            dispatch.constantBufferData
        );
    }

    if(dispatchCount > 0u)
    {
        VkBufferMemoryBarrier constantBufferReady{};
        constantBufferReady.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        constantBufferReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        constantBufferReady.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
        constantBufferReady.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        constantBufferReady.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        constantBufferReady.buffer = g_nrdConstantBuffer;
        constantBufferReady.offset = 0u;
        constantBufferReady.size = g_nrdConstantBufferStride * dispatchCount;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, nullptr,
            1, &constantBufferReady,
            0, nullptr
        );
    }

    for(uint32_t i = 0u; i < dispatchCount; ++i)
    {
        const nrd::DispatchDesc& dispatch = dispatches[i];

        if(dispatch.pipelineIndex >= g_nrdPipelines.size())
        {
            continue;
        }

        VkPipeline pipeline = g_nrdPipelines[dispatch.pipelineIndex];

        if(pipeline == VK_NULL_HANDLE)
        {
            continue;
        }

        std::vector<VkDescriptorImageInfo> imageInfos;
        std::vector<VkWriteDescriptorSet> writes;

        imageInfos.reserve(dispatch.resourcesNum);
        writes.reserve(dispatch.resourcesNum);

        uint32_t resourceIndex = 0u;
        bool resolvedAll = true;

        const nrd::PipelineDesc& pipelineDesc =
            g_nrdInstanceDesc.pipelines[dispatch.pipelineIndex];

        uint32_t textureIndex = 0u;
        uint32_t storageIndex = 0u;

        for(uint32_t rangeIndex = 0u; rangeIndex < pipelineDesc.resourceRangesNum; ++rangeIndex)
        {
            const nrd::ResourceRangeDesc& range = pipelineDesc.resourceRanges[rangeIndex];

            const bool isStorage =
                range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;

            const uint32_t baseBinding = isStorage
                ? g_nrdLibraryDesc.spirvBindingOffsets.storageTextureAndBufferOffset +
                      g_nrdInstanceDesc.resourcesBaseRegisterIndex
                : g_nrdLibraryDesc.spirvBindingOffsets.textureOffset +
                      g_nrdInstanceDesc.resourcesBaseRegisterIndex;

            for(uint32_t j = 0u; j < range.descriptorsNum; ++j)
            {
                if(resourceIndex >= dispatch.resourcesNum)
                {
                    resolvedAll = false;
                    break;
                }

                const nrd::ResourceDesc& resource = dispatch.resources[resourceIndex];
                ++resourceIndex;

                const VkImageView view = nrdResolveResourceView(resource);

                if(view == VK_NULL_HANDLE)
                {
                    resolvedAll = false;
                    continue;
                }

                VkDescriptorImageInfo info{};
                info.imageView = view;
                info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                imageInfos.push_back(info);

                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = g_nrdDescriptorSets[i];
                write.dstBinding = baseBinding + (isStorage ? storageIndex : textureIndex);
                write.dstArrayElement = 0u;
                write.descriptorCount = 1u;
                write.descriptorType = isStorage
                    ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                    : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                write.pImageInfo = nullptr;
                writes.push_back(write);
                if(isStorage)
                {
                    ++storageIndex;
                }
                else
                {
                    ++textureIndex;
                }
            }

            if(!resolvedAll)
            {
                break;
            }
        }

        if(!resolvedAll)
        {
            RTLog(
                std::string("[NRD-1] unresolved resource in dispatch ") +
                std::to_string(i)
            );
            continue;
        }

        for(size_t w = 0u; w < writes.size(); ++w)
        {
            writes[w].pImageInfo = &imageInfos[w];
        }

        if(!writes.empty())
        {
            vkUpdateDescriptorSets(
                g_device,
                static_cast<uint32_t>(writes.size()),
                writes.data(),
                0u,
                nullptr
            );
        }

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);

        const uint32_t dynamicOffset = static_cast<uint32_t>(
            g_nrdConstantBufferStride * g_nrdConstantBufferOffsets[i]
        );

        const VkDescriptorSet dispatchSets[2] =
        {
            g_nrdDescriptorSets[i],
            g_nrdConstantSets[i]
        };

        vkCmdBindDescriptorSets(
            cmd,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            g_nrdPipelineLayout,
            0u,
            2u,
            dispatchSets,
            1u,
            &dynamicOffset
        );

        vkCmdDispatch(
            cmd,
            static_cast<uint32_t>(dispatch.gridWidth),
            static_cast<uint32_t>(dispatch.gridHeight),
            1u
        );

        VkMemoryBarrier serialise{};
        serialise.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        serialise.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        serialise.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            1, &serialise,
            0, nullptr,
            0, nullptr
        );
    }

    nrdImageBarrier(
        cmd,
        g_nrdOutDiffRadianceHitDistImage,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT
    );

    nrdImageBarrier(
        cmd,
        g_nrdValidationImage,
        VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT
    );

    ++g_nrdFrameIndex;
    g_nrdNeedsHistoryReset = false;
}

// 每帧一次。当前没有 shader 消费者（2026-10-19 B 方案后雾与光柱都不再吃地形高度；
// 旧档贴地雾已退役），保留生产只为旧档兼容/后续复用。
void createRRGuideRelaxResources()
{
    if(g_rrGuideRelaxPipeline != VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorSetLayoutBinding bindings[8]{};
    for(uint32_t i = 0u; i < 5u; ++i)
    {
        bindings[i] = { i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    }
    bindings[5] = { 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[6] = { 6u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[7] = { 7u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 8u;
    layoutInfo.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
        &g_rrGuideRelaxSetLayout), "rr guide relax set layout failed");

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5u },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u }
    };
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_rrGuideRelaxDescriptorPool), "rr guide relax descriptor pool failed");

    const std::vector<uint8_t> code =
        loadSpv("shaders/rr_guide_relax.comp.spv");
    VkShaderModule shader = createShaderModule(g_device, code);
    try
    {
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size = sizeof(RRGuideRelaxPush);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_rrGuideRelaxSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_rrGuideRelaxPipelineLayout), "rr guide relax pipeline layout failed");
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
        pipelineInfo.layout = g_rrGuideRelaxPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &g_rrGuideRelaxPipeline),
            "rr guide relax pipeline failed");
    }
    catch(...)
    {
        vkDestroyShaderModule(g_device, shader, nullptr);
        throw;
    }
    vkDestroyShaderModule(g_device, shader, nullptr);
    RTLog("[RRRELAX] cross-delta guide roughness relaxation pipeline created");
}
void recordRRGuideRelax(VkCommandBuffer cmd)
{
    const bool relaxEnabled = g_rrGuideRelaxEnabled.load();
    const float farCloudDistance =
        (g_cloudPostRrComposite.load() || cloudFarBandSplitActive())
            ? 0.0f
            : std::max(g_rrFarCloudDistance.load(), 0.0f);
    const bool farCloudEnabled =
        farCloudDistance > 0.0f &&
        g_volumetricCloudResolvedView != VK_NULL_HANDLE &&
        g_volumetricCloudDepthView != VK_NULL_HANDLE;

    if(cmd == VK_NULL_HANDLE ||
       (!relaxEnabled && !farCloudEnabled) ||
       g_rrGuideRelaxPipeline == VK_NULL_HANDLE ||
       g_rrGuideRelaxPipelineLayout == VK_NULL_HANDLE ||
       g_rrGuideRelaxDescriptorPool == VK_NULL_HANDLE ||
       g_rrGBufferWidth == 0u || g_rrGBufferHeight == 0u ||
       g_rrNormalRoughnessImage == VK_NULL_HANDLE ||
       g_dlssDepthImage == VK_NULL_HANDLE ||
       g_rrDiffuseAlbedoImage == VK_NULL_HANDLE ||
       g_rrNormalRoughnessView == VK_NULL_HANDLE ||
       g_dlssDepthView == VK_NULL_HANDLE ||
       g_rrDiffuseAlbedoView == VK_NULL_HANDLE ||
       (farCloudEnabled &&
        (g_rrSpecularAlbedoView == VK_NULL_HANDLE ||
         g_rrSpecularHitDistanceView == VK_NULL_HANDLE ||
         g_cameraUboBuffer == VK_NULL_HANDLE)))
    {
        return;
    }

    if(g_rrGuideRelaxSet == VK_NULL_HANDLE)
    {
        VkDescriptorSetAllocateInfo setAllocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        setAllocation.descriptorPool = g_rrGuideRelaxDescriptorPool;
        setAllocation.descriptorSetCount = 1u;
        setAllocation.pSetLayouts = &g_rrGuideRelaxSetLayout;
        if(vkAllocateDescriptorSets(g_device, &setAllocation,
            &g_rrGuideRelaxSet) != VK_SUCCESS)
        {
            g_rrGuideRelaxSet = VK_NULL_HANDLE;
            return;
        }
        g_rrGuideRelaxCachedNormal = VK_NULL_HANDLE;
        g_rrGuideRelaxCachedDepth = VK_NULL_HANDLE;
        g_rrGuideRelaxCachedDiffuse = VK_NULL_HANDLE;
        g_rrGuideRelaxCachedSpecularAlbedo = VK_NULL_HANDLE;
        g_rrGuideRelaxCachedSpecularHitDistance = VK_NULL_HANDLE;
        g_rrGuideRelaxCachedCloudResolved = VK_NULL_HANDLE;
        g_rrGuideRelaxCachedCloudDepth = VK_NULL_HANDLE;
        g_rrGuideRelaxCachedCamera = VK_NULL_HANDLE;
        g_rrGuideRelaxCachedGeneration = UINT32_MAX;
    }

    if(g_rrGuideRelaxCachedGeneration != g_rrGuideViewGeneration ||
       g_rrGuideRelaxCachedNormal != g_rrNormalRoughnessView ||
       g_rrGuideRelaxCachedDepth != g_dlssDepthView ||
       g_rrGuideRelaxCachedDiffuse != g_rrDiffuseAlbedoView ||
       g_rrGuideRelaxCachedSpecularAlbedo != g_rrSpecularAlbedoView ||
       g_rrGuideRelaxCachedSpecularHitDistance != g_rrSpecularHitDistanceView ||
       g_rrGuideRelaxCachedCloudResolved != g_volumetricCloudResolvedView ||
       g_rrGuideRelaxCachedCloudDepth != g_volumetricCloudDepthView ||
       g_rrGuideRelaxCachedCamera != g_cameraUboBuffer)
    {
        VkDescriptorImageInfo guideInfos[5]{};
        guideInfos[0].imageView = g_rrNormalRoughnessView;
        guideInfos[1].imageView = g_dlssDepthView;
        guideInfos[2].imageView = g_rrDiffuseAlbedoView;
        guideInfos[3].imageView = g_rrSpecularHitDistanceView;
        guideInfos[4].imageView = g_rrSpecularAlbedoView;
        for(uint32_t i = 0u; i < 5u; ++i)
        {
            guideInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }

        const bool cloudSamplersWritable =
            g_volumetricCloudResolvedView != VK_NULL_HANDLE &&
            g_volumetricCloudDepthView != VK_NULL_HANDLE &&
            g_volumetricCloudSampler != VK_NULL_HANDLE;
        VkDescriptorImageInfo cloudResolvedInfo{};
        cloudResolvedInfo.sampler = g_volumetricCloudSampler;
        cloudResolvedInfo.imageView = g_volumetricCloudResolvedView;
        cloudResolvedInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo cloudDepthInfo{};
        cloudDepthInfo.sampler = g_volumetricCloudSampler;
        cloudDepthInfo.imageView = g_volumetricCloudDepthView;
        cloudDepthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo cameraInfo{};
        cameraInfo.buffer = g_cameraUboBuffer;
        cameraInfo.offset = 0u;
        cameraInfo.range = sizeof(CameraUBO);

        VkWriteDescriptorSet writes[8]{};
        for(uint32_t i = 0u; i < 5u; ++i)
        {
            writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_rrGuideRelaxSet, i, 0u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                &guideInfos[i], nullptr, nullptr };
        }
        uint32_t writeCount = 5u;
        if(cloudSamplersWritable)
        {
            writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr, g_rrGuideRelaxSet, 5u, 0u, 1u,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudResolvedInfo,
                nullptr, nullptr };
            writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr, g_rrGuideRelaxSet, 6u, 0u, 1u,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudDepthInfo,
                nullptr, nullptr };
        }
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_rrGuideRelaxSet, 7u, 0u, 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            nullptr, &cameraInfo, nullptr };
        vkUpdateDescriptorSets(g_device, writeCount, writes, 0u, nullptr);

        g_rrGuideRelaxCachedNormal = g_rrNormalRoughnessView;
        g_rrGuideRelaxCachedDepth = g_dlssDepthView;
        g_rrGuideRelaxCachedDiffuse = g_rrDiffuseAlbedoView;
        g_rrGuideRelaxCachedSpecularAlbedo = g_rrSpecularAlbedoView;
        g_rrGuideRelaxCachedSpecularHitDistance = g_rrSpecularHitDistanceView;
        g_rrGuideRelaxCachedCloudResolved = g_volumetricCloudResolvedView;
        g_rrGuideRelaxCachedCloudDepth = g_volumetricCloudDepthView;
        g_rrGuideRelaxCachedCamera = g_cameraUboBuffer;
        g_rrGuideRelaxCachedGeneration = g_rrGuideViewGeneration;
    }

    VkImageMemoryBarrier barriers[5]{};
    for(uint32_t i = 0u; i < 5u; ++i)
    {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.levelCount = 1u;
        barriers[i].subresourceRange.layerCount = 1u;
    }
    barriers[0].image = g_rrNormalRoughnessImage;
    barriers[1].image = g_dlssDepthImage;
    barriers[2].image = g_rrDiffuseAlbedoImage;
    barriers[3].image = g_rrSpecularHitDistanceImage;
    barriers[4].image = g_rrSpecularAlbedoImage;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
        5u, barriers);

    if(farCloudEnabled)
    {
        VkImageMemoryBarrier cloudBarriers[2]{};
        for(uint32_t i = 0u; i < 2u; ++i)
        {
            cloudBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            cloudBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            cloudBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            cloudBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            cloudBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            cloudBarriers[i].srcAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            cloudBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            cloudBarriers[i].subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            cloudBarriers[i].subresourceRange.levelCount = 1u;
            cloudBarriers[i].subresourceRange.layerCount = 1u;
        }
        cloudBarriers[0].image = g_volumetricCloudResolvedImage;
        cloudBarriers[1].image = g_volumetricCloudDepthImage;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            2u, cloudBarriers);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_rrGuideRelaxPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_rrGuideRelaxPipelineLayout, 0u, 1u, &g_rrGuideRelaxSet, 0u, nullptr);

    RRGuideRelaxPush push{};
    push.extentX = g_rrGBufferWidth;
    push.extentY = g_rrGBufferHeight;
    push.relaxScale = relaxEnabled ? 1.0f : 0.0f;
    push.minRoughness = 0.02f;
    push.normalFalloff = 1.0f;
    push.farCloudDistance = farCloudEnabled ? farCloudDistance : 0.0f;
    push.padding1 = 0.0f;
    push.padding2 = 0.0f;
    vkCmdPushConstants(cmd, g_rrGuideRelaxPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(push), &push);

    vkCmdDispatch(cmd,
        (g_rrGBufferWidth + 15u) / 16u,
        (g_rrGBufferHeight + 15u) / 16u,
        1u);

    for(uint32_t i = 0u; i < 5u; ++i)
    {
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr,
        5u, barriers);
}
void destroyRRGuideRelaxResources()
{
    if(g_rrGuideRelaxPipeline)
    {
        vkDestroyPipeline(g_device, g_rrGuideRelaxPipeline, nullptr);
        g_rrGuideRelaxPipeline = VK_NULL_HANDLE;
    }
    if(g_rrGuideRelaxPipelineLayout)
    {
        vkDestroyPipelineLayout(g_device, g_rrGuideRelaxPipelineLayout, nullptr);
        g_rrGuideRelaxPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_rrGuideRelaxDescriptorPool)
    {
        vkDestroyDescriptorPool(g_device, g_rrGuideRelaxDescriptorPool, nullptr);
        g_rrGuideRelaxDescriptorPool = VK_NULL_HANDLE;
    }
    if(g_rrGuideRelaxSetLayout)
    {
        vkDestroyDescriptorSetLayout(g_device, g_rrGuideRelaxSetLayout, nullptr);
        g_rrGuideRelaxSetLayout = VK_NULL_HANDLE;
    }
    g_rrGuideRelaxSet = VK_NULL_HANDLE;
    g_rrGuideRelaxCachedNormal = VK_NULL_HANDLE;
    g_rrGuideRelaxCachedDepth = VK_NULL_HANDLE;
    g_rrGuideRelaxCachedDiffuse = VK_NULL_HANDLE;
    g_rrGuideRelaxCachedSpecularAlbedo = VK_NULL_HANDLE;
    g_rrGuideRelaxCachedSpecularHitDistance = VK_NULL_HANDLE;
    g_rrGuideRelaxCachedCloudResolved = VK_NULL_HANDLE;
    g_rrGuideRelaxCachedCloudDepth = VK_NULL_HANDLE;
    g_rrGuideRelaxCachedCamera = VK_NULL_HANDLE;
    g_rrGuideRelaxCachedGeneration = UINT32_MAX;
}
