// Orion backend — VolRestir 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"



// 2026-10-26：体积光 ReSTIR 开关（只读一次环境变量）。
// 体积光 ReSTIR 总开关。三级解析：
//   1) GUI 覆盖（设置界面「体积光 ReSTIR」）；
//   2) 环境变量 ORION_VOL_RESTIR（启动时读一次，供无 GUI 的批处理/CI 用）；
//   3) 出厂关闭。
// GUI 覆盖优先，且每次调用都读 —— 开关可以在游戏内直接切。
uint32_t volRestirEnabled()
{
    const uint32_t overrideValue =
        g_volRestirGuiOverride.load(std::memory_order_relaxed);

    if(overrideValue != 0xFFFFFFFFu)
    {
        return overrideValue != 0u ? 1u : 0u;
    }

    static const uint32_t cached = []() -> uint32_t
    {
        const char* raw = std::getenv("ORION_VOL_RESTIR");

        if(raw != nullptr && raw[0] == '0' && raw[1] == '\0')
        {
            return 0u;
        }

        return 1u;
    }();

    return cached;
}

// 2026-10-27：体积 ReSTIR / SH 缓存的诊断视图开关。
//   0 = 关；1..4 = 显示 SH 辐照度 / 体积 radiance / age / reservoir 有效性
uint32_t volDiagMode()
{
    static const uint32_t cached = []() -> uint32_t
    {
        const char* raw = std::getenv("ORION_VOL_DIAG");
        if(raw == nullptr)
        {
            return 0u;
        }
        const int value = std::atoi(raw);
        return (value >= 1 && value <= 4) ? static_cast<uint32_t>(value) : 0u;
    }();
    return cached;
}
void createVolumeRestir3DImage(
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    VkFormat format,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};

    info.imageType = VK_IMAGE_TYPE_3D;
    info.format = format;
    info.extent = {width, height, depth};
    info.mipLevels = 1u;
    info.arrayLayers = 1u;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    check(vkCreateImage(g_device, &info, nullptr, &image), "vol restir image create failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, image, &requirements);

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(
        g_physicalDevice, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    check(vkAllocateMemory(g_device, &allocation, nullptr, &memory), "vol restir memory failed");
    check(vkBindImageMemory(g_device, image, memory, 0u), "vol restir bind failed");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;

    check(vkCreateImageView(g_device, &viewInfo, nullptr, &view), "vol restir view failed");
}
void destroyVolumeRestirResources()
{
    if(g_device != VK_NULL_HANDLE && g_volRestirPipeline != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(g_device);
    }

    if(g_device != VK_NULL_HANDLE && g_volRadianceSampler != VK_NULL_HANDLE)
    {
        vkDestroySampler(g_device, g_volRadianceSampler, nullptr);
        g_volRadianceSampler = VK_NULL_HANDLE;
    }

    for(uint32_t i = 0u; i < 2u; ++i)
    {
        if(g_volRadianceView[i] != VK_NULL_HANDLE) { vkDestroyImageView(g_device, g_volRadianceView[i], nullptr); g_volRadianceView[i] = VK_NULL_HANDLE; }
        if(g_volRadianceImage[i] != VK_NULL_HANDLE) { vkDestroyImage(g_device, g_volRadianceImage[i], nullptr); g_volRadianceImage[i] = VK_NULL_HANDLE; }
        if(g_volRadianceMemory[i] != VK_NULL_HANDLE) { vkFreeMemory(g_device, g_volRadianceMemory[i], nullptr); g_volRadianceMemory[i] = VK_NULL_HANDLE; }

        if(g_volShYView[i] != VK_NULL_HANDLE) { vkDestroyImageView(g_device, g_volShYView[i], nullptr); g_volShYView[i] = VK_NULL_HANDLE; }
        if(g_volShYImage[i] != VK_NULL_HANDLE) { vkDestroyImage(g_device, g_volShYImage[i], nullptr); g_volShYImage[i] = VK_NULL_HANDLE; }
        if(g_volShYMemory[i] != VK_NULL_HANDLE) { vkFreeMemory(g_device, g_volShYMemory[i], nullptr); g_volShYMemory[i] = VK_NULL_HANDLE; }
        if(g_volShCoCgView[i] != VK_NULL_HANDLE) { vkDestroyImageView(g_device, g_volShCoCgView[i], nullptr); g_volShCoCgView[i] = VK_NULL_HANDLE; }
        if(g_volShCoCgImage[i] != VK_NULL_HANDLE) { vkDestroyImage(g_device, g_volShCoCgImage[i], nullptr); g_volShCoCgImage[i] = VK_NULL_HANDLE; }
        if(g_volShCoCgMemory[i] != VK_NULL_HANDLE) { vkFreeMemory(g_device, g_volShCoCgMemory[i], nullptr); g_volShCoCgMemory[i] = VK_NULL_HANDLE; }
        if(g_volAgeView[i] != VK_NULL_HANDLE) { vkDestroyImageView(g_device, g_volAgeView[i], nullptr); g_volAgeView[i] = VK_NULL_HANDLE; }
        if(g_volAgeImage[i] != VK_NULL_HANDLE) { vkDestroyImage(g_device, g_volAgeImage[i], nullptr); g_volAgeImage[i] = VK_NULL_HANDLE; }
        if(g_volAgeMemory[i] != VK_NULL_HANDLE) { vkFreeMemory(g_device, g_volAgeMemory[i], nullptr); g_volAgeMemory[i] = VK_NULL_HANDLE; }

        if(g_volReservoirView[i] != VK_NULL_HANDLE) { vkDestroyImageView(g_device, g_volReservoirView[i], nullptr); g_volReservoirView[i] = VK_NULL_HANDLE; }
        if(g_volReservoirImage[i] != VK_NULL_HANDLE) { vkDestroyImage(g_device, g_volReservoirImage[i], nullptr); g_volReservoirImage[i] = VK_NULL_HANDLE; }
        if(g_volReservoirMemory[i] != VK_NULL_HANDLE) { vkFreeMemory(g_device, g_volReservoirMemory[i], nullptr); g_volReservoirMemory[i] = VK_NULL_HANDLE; }
    }

    if(g_volRestirPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_volRestirPipeline, nullptr);
        g_volRestirPipeline = VK_NULL_HANDLE;
    }
    if(g_volRestirPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_volRestirPipelineLayout, nullptr);
        g_volRestirPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_volRestirDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_volRestirDescriptorPool, nullptr);
        g_volRestirDescriptorPool = VK_NULL_HANDLE;
        g_volRestirSet = VK_NULL_HANDLE;
    }
    if(g_volRestirSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_volRestirSetLayout, nullptr);
        g_volRestirSetLayout = VK_NULL_HANDLE;
    }

    if(g_volRestirTemporalPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_volRestirTemporalPipeline, nullptr);
        g_volRestirTemporalPipeline = VK_NULL_HANDLE;
    }
    if(g_volRestirSpatialPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_volRestirSpatialPipeline, nullptr);
        g_volRestirSpatialPipeline = VK_NULL_HANDLE;
    }
    if(g_volRestirTemporalPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_volRestirTemporalPipelineLayout, nullptr);
        g_volRestirTemporalPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_volRestirSpatialPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_volRestirSpatialPipelineLayout, nullptr);
        g_volRestirSpatialPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_volRestirTemporalDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_volRestirTemporalDescriptorPool, nullptr);
        g_volRestirTemporalDescriptorPool = VK_NULL_HANDLE;
        g_volRestirTemporalSet = VK_NULL_HANDLE;
    }
    if(g_volRestirSpatialDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_volRestirSpatialDescriptorPool, nullptr);
        g_volRestirSpatialDescriptorPool = VK_NULL_HANDLE;
        g_volRestirSpatialSet = VK_NULL_HANDLE;
    }
    if(g_volRestirTemporalSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_volRestirTemporalSetLayout, nullptr);
        g_volRestirTemporalSetLayout = VK_NULL_HANDLE;
    }
    if(g_volRestirSpatialSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_volRestirSpatialSetLayout, nullptr);
        g_volRestirSpatialSetLayout = VK_NULL_HANDLE;
    }

    if(g_volRestirIntegratePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_volRestirIntegratePipeline, nullptr);
        g_volRestirIntegratePipeline = VK_NULL_HANDLE;
    }
    if(g_volRestirCompositePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_volRestirCompositePipeline, nullptr);
        g_volRestirCompositePipeline = VK_NULL_HANDLE;
    }
    if(g_volRestirIntegratePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_volRestirIntegratePipelineLayout, nullptr);
        g_volRestirIntegratePipelineLayout = VK_NULL_HANDLE;
    }
    if(g_volRestirCompositePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_volRestirCompositePipelineLayout, nullptr);
        g_volRestirCompositePipelineLayout = VK_NULL_HANDLE;
    }
    if(g_volRestirIntegrateDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_volRestirIntegrateDescriptorPool, nullptr);
        g_volRestirIntegrateDescriptorPool = VK_NULL_HANDLE;
        g_volRestirIntegrateSet = VK_NULL_HANDLE;
    }
    if(g_volRestirCompositeDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_volRestirCompositeDescriptorPool, nullptr);
        g_volRestirCompositeDescriptorPool = VK_NULL_HANDLE;
        g_volRestirCompositeSet = VK_NULL_HANDLE;
    }
    if(g_volRestirIntegrateSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_volRestirIntegrateSetLayout, nullptr);
        g_volRestirIntegrateSetLayout = VK_NULL_HANDLE;
    }
    if(g_volRestirCompositeSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_volRestirCompositeSetLayout, nullptr);
        g_volRestirCompositeSetLayout = VK_NULL_HANDLE;
    }

    if(g_volRestirVisibilityPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(g_device, g_volRestirVisibilityPipeline, nullptr);
        g_volRestirVisibilityPipeline = VK_NULL_HANDLE;
    }
    if(g_volRestirVisibilityPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(g_device, g_volRestirVisibilityPipelineLayout, nullptr);
        g_volRestirVisibilityPipelineLayout = VK_NULL_HANDLE;
    }
    if(g_volRestirVisibilityDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_device, g_volRestirVisibilityDescriptorPool, nullptr);
        g_volRestirVisibilityDescriptorPool = VK_NULL_HANDLE;
        g_volRestirVisibilitySet = VK_NULL_HANDLE;
    }
    if(g_volRestirVisibilitySetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(g_device, g_volRestirVisibilitySetLayout, nullptr);
        g_volRestirVisibilitySetLayout = VK_NULL_HANDLE;
    }

    g_volRestirLayoutReady = false;

    g_volRestirBoundLightBuffer = VK_NULL_HANDLE;
    g_volRestirBoundGridBuffer = VK_NULL_HANDLE;
    g_volRestirBoundVisibilityTlas = VK_NULL_HANDLE;
    g_volRestirBoundVisibilityLightBuffer = VK_NULL_HANDLE;
    g_volRestirBoundVisibilityGridBuffer = VK_NULL_HANDLE;
    g_volRestirBoundIntegrateLightBuffer = VK_NULL_HANDLE;
    g_volRestirBoundIntegrateGridBuffer = VK_NULL_HANDLE;
    g_volRestirBoundIntegrateTlas = VK_NULL_HANDLE;


    g_volFroxelW = 0u;
    g_volFroxelH = 0u;
    g_volRestirW = 0u;
    g_volRestirH = 0u;
    g_volWriteIndex = 0u;
}
void createVolumeRestirResources(uint32_t renderWidth, uint32_t renderHeight)
{
    if(volRestirEnabled() == 0u)
    {
        return;
    }

    const uint32_t froxelW = std::max(1u, (renderWidth + VOL_FROXEL_DOWNSCALE - 1u) / VOL_FROXEL_DOWNSCALE);
    const uint32_t froxelH = std::max(1u, (renderHeight + VOL_FROXEL_DOWNSCALE - 1u) / VOL_FROXEL_DOWNSCALE);
    const uint32_t restirW = std::max(1u, (froxelW + VOL_RESTIR_DOWNSCALE - 1u) / VOL_RESTIR_DOWNSCALE);
    const uint32_t restirH = std::max(1u, (froxelH + VOL_RESTIR_DOWNSCALE - 1u) / VOL_RESTIR_DOWNSCALE);

    if(g_volFroxelW == froxelW && g_volFroxelH == froxelH &&
       g_volRadianceImage[1] != VK_NULL_HANDLE)
    {
        return;
    }

    destroyVolumeRestirResources();

    g_volFroxelW = froxelW;
    g_volFroxelH = froxelH;
    g_volRestirW = restirW;
    g_volRestirH = restirH;

    for(uint32_t i = 0u; i < 2u; ++i)
    {
        createVolumeRestir3DImage(froxelW, froxelH, VOL_RESTIR_DEPTH,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            g_volRadianceImage[i], g_volRadianceMemory[i], g_volRadianceView[i]);

        createVolumeRestir3DImage(froxelW, froxelH, VOL_RESTIR_DEPTH,
            VK_FORMAT_R8_UNORM,
            g_volAgeImage[i], g_volAgeMemory[i], g_volAgeView[i]);

        createVolumeRestir3DImage(froxelW, froxelH, VOL_RESTIR_DEPTH,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            g_volShYImage[i], g_volShYMemory[i], g_volShYView[i]);

        createVolumeRestir3DImage(froxelW, froxelH, VOL_RESTIR_DEPTH,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            g_volShCoCgImage[i], g_volShCoCgMemory[i], g_volShCoCgView[i]);

        createVolumeRestir3DImage(restirW, restirH, VOL_RESTIR_DEPTH,
            VK_FORMAT_R32G32B32A32_UINT,
            g_volReservoirImage[i], g_volReservoirMemory[i], g_volReservoirView[i]);
    }

    g_volShNeedsInit = true;

    volDepthSampler();

    RTLog(
        "[VOL RESTIR] resources created froxel=" + std::to_string(froxelW) + "x" +
        std::to_string(froxelH) + "x" + std::to_string(VOL_RESTIR_DEPTH) +
        " restir=" + std::to_string(restirW) + "x" + std::to_string(restirH) +
        "x" + std::to_string(VOL_RESTIR_DEPTH));
}

// 3D froxel 辐亮度的采样器。**不能**复用云噪声的 g_cloudShapeNoiseSampler：
// 那个 sampler 三个轴都是 ADDRESS_MODE_REPEAT，三线性在最后一层（froxelW≈0.992）
// 会把权重分给"第 64 片"，REPEAT 于是绕回第 0 片 —— 贴脸的光渗到 90 格远处。
// 这里必须是 CLAMP_TO_EDGE；mip 模式取 NEAREST 且 maxLod=0（只读 LOD 0）。
VkSampler volRadianceSampler()
{
    if(g_volRadianceSampler == VK_NULL_HANDLE && g_device != VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.minLod = 0.0f;
        info.maxLod = 0.0f;
        vkCreateSampler(g_device, &info, nullptr, &g_volRadianceSampler);
    }

    return g_volRadianceSampler;
}

// S2：INITIAL 阶段的描述符与管线。只建一次（资源重建时随 destroy 一起清空）。
void createVolumeRestirPipeline()
{
    if(g_volRestirPipeline != VK_NULL_HANDLE || g_volRestirPipelineFailed)
    {
        return;
    }
    if(g_volReservoirView[0] == VK_NULL_HANDLE || g_volRadianceView[0] == VK_NULL_HANDLE ||
       g_localLightGpuBuffer == VK_NULL_HANDLE || g_localLightGridGpuBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    try
    {
        VkDescriptorSetLayoutBinding bindings[5]{};
        bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[1] = { 12u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[2] = { 13u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[3] = { 14u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[4] = { 15u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        layoutInfo.bindingCount = 5u;
        layoutInfo.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
            &g_volRestirSetLayout), "vol restir set layout failed");

        VkDescriptorPoolSize poolSizes[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2u }
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1u;
        poolInfo.poolSizeCount = 3u;
        poolInfo.pPoolSizes = poolSizes;
        check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
            &g_volRestirDescriptorPool), "vol restir descriptor pool failed");

        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool = g_volRestirDescriptorPool;
        allocateInfo.descriptorSetCount = 1u;
        allocateInfo.pSetLayouts = &g_volRestirSetLayout;
        check(vkAllocateDescriptorSets(g_device, &allocateInfo, &g_volRestirSet),
            "vol restir descriptor set failed");

        VkDescriptorBufferInfo cameraInfo{};
        cameraInfo.buffer = g_cameraUboBuffer;
        cameraInfo.offset = 0u;
        cameraInfo.range = sizeof(CameraUBO);

        VkDescriptorBufferInfo lightInfo{};
        lightInfo.buffer = g_localLightGpuBuffer;
        lightInfo.offset = 0u;
        lightInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo gridInfo{};
        gridInfo.buffer = g_localLightGridGpuBuffer;
        gridInfo.offset = 0u;
        gridInfo.range = VK_WHOLE_SIZE;

        VkDescriptorImageInfo reservoirInfo{};
        reservoirInfo.imageView = g_volReservoirView[0];
        reservoirInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo radianceInfo{};
        radianceInfo.imageView = g_volRadianceView[0];
        radianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[5]{};
        for(uint32_t i = 0u; i < 5u; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = g_volRestirSet;
            writes[i].descriptorCount = 1u;
        }
        writes[0].dstBinding = 0u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &cameraInfo;
        writes[1].dstBinding = 12u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &lightInfo;
        writes[2].dstBinding = 13u;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &gridInfo;
        writes[3].dstBinding = 14u;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[3].pImageInfo = &reservoirInfo;
        writes[4].dstBinding = 15u;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[4].pImageInfo = &radianceInfo;

        vkUpdateDescriptorSets(g_device, 5u, writes, 0u, nullptr);

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size = sizeof(VolRestirPushConstants);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_volRestirSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_volRestirPipelineLayout), "vol restir pipeline layout failed");

        const std::vector<uint8_t> code = loadSpv("shaders/vol_restir.comp.spv");
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
        pipelineInfo.layout = g_volRestirPipelineLayout;
        const VkResult pipelineResult = vkCreateComputePipelines(g_device,
            getSharedPipelineCache(), 1u, &pipelineInfo, nullptr, &g_volRestirPipeline);
        vkDestroyShaderModule(g_device, module, nullptr);
        check(pipelineResult, "vol restir pipeline failed");

        RTLog("[VOL RESTIR] initial pipeline ready");
    }
    catch(const std::exception& error)
    {
        g_volRestirPipelineFailed = true;
        RTLog(std::string("[VOL RESTIR] pipeline unavailable: ") + error.what());
    }
}

// S3：temporal + spatial 的描述符与管线。只建一次（资源重建时随 destroy 清空）。
// 两个 pass 的绑定布局一致（0 = UBO，1 = 当前帧 reservoir，2 = 上一帧 reservoir），
// 差别只在 shader 里谁读谁写。
// 2026-10-26：descriptor 提交体检。NVIDIA 驱动对 NULL sampler / NULL imageView /
// 零初始化槽位不报错、直接 access violation，连 VkResult 都不给。这里统一拦。
void vkGuardUpdate(
    uint32_t count,
    VkWriteDescriptorSet* writes,
    const char* tag)
{
    uint32_t safe = 0u;

    for(uint32_t i = 0u; i < count; ++i)
    {
        VkWriteDescriptorSet w = writes[i];

        if(w.sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET ||
           w.descriptorType == 0u ||
           w.descriptorCount == 0u)
        {
            RTLogFlushed(std::string("[VK GUARD] ") + tag + " slot " + std::to_string(i) + " uninitialized");
            continue;
        }

        bool bad = false;

        if(w.pImageInfo != nullptr)
        {
            for(uint32_t k = 0u; k < w.descriptorCount; ++k)
            {
                if(w.pImageInfo[k].imageView == VK_NULL_HANDLE)
                {
                    RTLogFlushed(std::string("[VK GUARD] ") + tag + " binding " + std::to_string(w.dstBinding) + " NULL imageView");
                    bad = true;
                }

                if((w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                    w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) &&
                   w.pImageInfo[k].sampler == VK_NULL_HANDLE)
                {
                    RTLogFlushed(std::string("[VK GUARD] ") + tag + " binding " + std::to_string(w.dstBinding) + " NULL sampler");
                    bad = true;
                }
            }
        }
        else if(w.pBufferInfo == nullptr &&
                w.pTexelBufferView == nullptr &&
                w.pNext == nullptr)
        {
            RTLogFlushed(std::string("[VK GUARD] ") + tag + " binding " + std::to_string(w.dstBinding) + " no data pointer");
            bad = true;
        }

        if(bad)
        {
            continue;
        }

        writes[safe++] = w;
    }

    vkUpdateDescriptorSets(g_device, safe, writes, 0u, nullptr);
}
void createVolumeRestirChainPipelines()
{
    if(g_volRestirTemporalPipeline != VK_NULL_HANDLE || g_volRestirPipelineFailed)
    {
        return;
    }
    if(g_volReservoirView[0] == VK_NULL_HANDLE || g_volReservoirView[1] == VK_NULL_HANDLE ||
       g_cameraUboBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    try
    {
        auto createStage = [](
            const char* spvPath,
            VkDescriptorSetLayout& setLayout,
            VkDescriptorPool& pool,
            VkDescriptorSet& set,
            VkPipelineLayout& pipelineLayout,
            VkPipeline& pipeline)
        {
            VkDescriptorSetLayoutBinding bindings[3]{};
            bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

            VkDescriptorSetLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
            };
            layoutInfo.bindingCount = 3u;
            layoutInfo.pBindings = bindings;
            check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
                &setLayout), "vol restir chain set layout failed");

            VkDescriptorPoolSize poolSizes[2] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2u }
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1u;
            poolInfo.poolSizeCount = 2u;
            poolInfo.pPoolSizes = poolSizes;
            check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
                &pool), "vol restir chain descriptor pool failed");

            VkDescriptorSetAllocateInfo allocateInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
            };
            allocateInfo.descriptorPool = pool;
            allocateInfo.descriptorSetCount = 1u;
            allocateInfo.pSetLayouts = &setLayout;
            check(vkAllocateDescriptorSets(g_device, &allocateInfo, &set),
                "vol restir chain descriptor set failed");

            VkDescriptorBufferInfo cameraInfo{};
            cameraInfo.buffer = g_cameraUboBuffer;
            cameraInfo.offset = 0u;
            cameraInfo.range = sizeof(CameraUBO);

            VkDescriptorImageInfo currentInfo{};
            currentInfo.imageView = g_volReservoirView[0];
            currentInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo historyInfo{};
            historyInfo.imageView = g_volReservoirView[1];
            historyInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[3]{};
            for(uint32_t i = 0u; i < 3u; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].descriptorCount = 1u;
            }
            writes[0].dstBinding = 0u;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &cameraInfo;
            writes[1].dstBinding = 1u;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo = &currentInfo;
            writes[2].dstBinding = 2u;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[2].pImageInfo = &historyInfo;

            vkGuardUpdate(3u, writes, "vol");

            VkPushConstantRange pushRange{};
            pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pushRange.offset = 0u;
            pushRange.size = VOL_RESTIR_CHAIN_PUSH_BYTES;

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
            };
            pipelineLayoutInfo.setLayoutCount = 1u;
            pipelineLayoutInfo.pSetLayouts = &setLayout;
            pipelineLayoutInfo.pushConstantRangeCount = 1u;
            pipelineLayoutInfo.pPushConstantRanges = &pushRange;
            check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
                &pipelineLayout), "vol restir chain pipeline layout failed");

            const std::vector<uint8_t> code = loadSpv(spvPath);
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
            pipelineInfo.layout = pipelineLayout;
            const VkResult pipelineResult = vkCreateComputePipelines(g_device,
                getSharedPipelineCache(), 1u, &pipelineInfo, nullptr, &pipeline);
            vkDestroyShaderModule(g_device, module, nullptr);
            check(pipelineResult, "vol restir chain pipeline failed");
        };

        createStage("shaders/vol_restir_temporal.comp.spv",
            g_volRestirTemporalSetLayout, g_volRestirTemporalDescriptorPool,
            g_volRestirTemporalSet, g_volRestirTemporalPipelineLayout,
            g_volRestirTemporalPipeline);

        createStage("shaders/vol_restir_spatial.comp.spv",
            g_volRestirSpatialSetLayout, g_volRestirSpatialDescriptorPool,
            g_volRestirSpatialSet, g_volRestirSpatialPipelineLayout,
            g_volRestirSpatialPipeline);

        RTLog("[VOL RESTIR] temporal + spatial pipelines ready");
    }
    catch(const std::exception& error)
    {
        g_volRestirPipelineFailed = true;
        RTLog(std::string("[VOL RESTIR] chain pipeline unavailable: ") + error.what());
    }
}

// S3：在 INITIAL 之后接 temporal → spatial。两个 pass 都是 restir 网格尺寸。
void recordVolumeRestirChain(VkCommandBuffer cmd)
{
    if(g_volRestirTemporalPipeline == VK_NULL_HANDLE ||
       g_volRestirSpatialPipeline == VK_NULL_HANDLE ||
       g_volRestirTemporalSet == VK_NULL_HANDLE ||
       g_volRestirSpatialSet == VK_NULL_HANDLE ||
       g_volRestirW == 0u || g_volRestirH == 0u)
    {
        return;
    }

    const uint32_t groupCountX = (g_volRestirW + 7u) / 8u;
    const uint32_t groupCountY = (g_volRestirH + 7u) / 8u;

    {
        VkImageMemoryBarrier barriers[2]{};
        for(uint32_t i = 0u; i < 2u; ++i)
        {
            VkImageMemoryBarrier& barrier = barriers[i];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1u;
            barrier.subresourceRange.layerCount = 1u;
        }
        barriers[0].image = g_volReservoirImage[0];
        barriers[1].image = g_volReservoirImage[1];

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
            0u, nullptr, 0u, nullptr, 2u, barriers);

        VolRestirTemporalPushConstants pushConstants{};
        pushConstants.froxelSize[0] = static_cast<int32_t>(g_volFroxelW);
        pushConstants.froxelSize[1] = static_cast<int32_t>(g_volFroxelH);
        pushConstants.froxelSize[2] = static_cast<int32_t>(VOL_RESTIR_DEPTH);
        pushConstants.froxelSize[3] = static_cast<int32_t>(VOL_RESTIR_DOWNSCALE);
        pushConstants.temporalParams[0] = VOL_RESTIR_MAX_DISTANCE;
        pushConstants.temporalParams[1] = VOL_RESTIR_DEPTH_EXPONENT;
        pushConstants.temporalParams[2] = static_cast<float>(VOL_RESTIR_MAX_SAMPLES);
        pushConstants.temporalParams[3] = 0.0f;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volRestirTemporalPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_volRestirTemporalPipelineLayout, 0u, 1u, &g_volRestirTemporalSet, 0u, nullptr);
        vkCmdPushConstants(cmd, g_volRestirTemporalPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0u, VOL_RESTIR_CHAIN_PUSH_BYTES, &pushConstants);
        vkCmdDispatch(cmd, groupCountX, groupCountY, VOL_RESTIR_DEPTH);
    }

    {
        VkImageMemoryBarrier barriers[2]{};
        for(uint32_t i = 0u; i < 2u; ++i)
        {
            VkImageMemoryBarrier& barrier = barriers[i];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1u;
            barrier.subresourceRange.layerCount = 1u;
        }
        barriers[0].image = g_volReservoirImage[0];
        barriers[1].image = g_volReservoirImage[1];

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
            0u, nullptr, 0u, nullptr, 2u, barriers);

        VolRestirSpatialPushConstants pushConstants{};
        pushConstants.spatialParams[0] = 1.0f;
        pushConstants.spatialParams[1] = 0.0f;
        pushConstants.spatialParams[2] = 0.0f;
        pushConstants.spatialParams[3] = 0.0f;
        pushConstants.reserved[0] = 0.0f;
        pushConstants.reserved[1] = 0.0f;
        pushConstants.reserved[2] = 0.0f;
        pushConstants.reserved[3] = 0.0f;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volRestirSpatialPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_volRestirSpatialPipelineLayout, 0u, 1u, &g_volRestirSpatialSet, 0u, nullptr);
        vkCmdPushConstants(cmd, g_volRestirSpatialPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0u, VOL_RESTIR_CHAIN_PUSH_BYTES, &pushConstants);
        vkCmdDispatch(cmd, groupCountX, groupCountY, VOL_RESTIR_DEPTH);
    }
}

// S4：integrate 与 composite 的描述符/管线。
//   integrate：0 = UBO, 1 = 最终 reservoir（R1）, 12/13 = 灯表/网格, 15 = froxel 辐亮度
//   composite：0 = UBO, 1 = 场景深度, 2 = froxel 辐亮度（sampler3D）, 3 = 输出
void createVolumeRestirOutputPipelines()
{
    if(g_volRestirIntegratePipeline != VK_NULL_HANDLE || g_volRestirPipelineFailed)
    {
        return;
    }
    if(g_volRadianceView[0] == VK_NULL_HANDLE || g_volReservoirView[1] == VK_NULL_HANDLE ||
       g_dlssDepthView == VK_NULL_HANDLE || g_godRayTargetView == VK_NULL_HANDLE ||
       g_tlas == VK_NULL_HANDLE ||
       g_localLightGpuBuffer == VK_NULL_HANDLE || g_localLightGridGpuBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    try
    {
        {
            VkDescriptorSetLayoutBinding bindings[12]{};
            bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[2] = { 12u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[3] = { 13u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[4] = { 15u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[5] = { 16u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[6] = { 17u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[7] = { 18u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[8] = { 19u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[9] = { 20u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[10] = { 21u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[11] = { 22u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

            VkDescriptorSetLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
            };
            layoutInfo.bindingCount = 12u;
            layoutInfo.pBindings = bindings;
            check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
                &g_volRestirIntegrateSetLayout), "vol restir integrate set layout failed");

            VkDescriptorPoolSize poolSizes[4] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
                { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u },
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8u },
                { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u }
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1u;
            poolInfo.poolSizeCount = 4u;
            poolInfo.pPoolSizes = poolSizes;
            check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
                &g_volRestirIntegrateDescriptorPool), "vol restir integrate pool failed");

            VkDescriptorSetAllocateInfo allocateInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
            };
            allocateInfo.descriptorPool = g_volRestirIntegrateDescriptorPool;
            allocateInfo.descriptorSetCount = 1u;
            allocateInfo.pSetLayouts = &g_volRestirIntegrateSetLayout;
            check(vkAllocateDescriptorSets(g_device, &allocateInfo,
                &g_volRestirIntegrateSet), "vol restir integrate set failed");

            VkDescriptorBufferInfo cameraInfo{};
            cameraInfo.buffer = g_cameraUboBuffer;
            cameraInfo.offset = 0u;
            cameraInfo.range = sizeof(CameraUBO);

            VkDescriptorBufferInfo lightInfo{};
            lightInfo.buffer = g_localLightGpuBuffer;
            lightInfo.offset = 0u;
            lightInfo.range = VK_WHOLE_SIZE;

            VkDescriptorBufferInfo gridInfo{};
            gridInfo.buffer = g_localLightGridGpuBuffer;
            gridInfo.offset = 0u;
            gridInfo.range = VK_WHOLE_SIZE;

            VkDescriptorImageInfo reservoirInfo{};
            reservoirInfo.imageView = g_volReservoirView[1];
            reservoirInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo radianceInfo{};
            radianceInfo.imageView = g_volRadianceView[0];
            radianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo shShYPrevInfo{};
            shShYPrevInfo.imageView = g_volShYView[1];
            shShYPrevInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo shCoCgPrevInfo{};
            shCoCgPrevInfo.imageView = g_volShCoCgView[1];
            shCoCgPrevInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo shAgePrevInfo{};
            shAgePrevInfo.imageView = g_volAgeView[1];
            shAgePrevInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo shYCurInfo{};
            shYCurInfo.imageView = g_volShYView[0];
            shYCurInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo shCoCgCurInfo{};
            shCoCgCurInfo.imageView = g_volShCoCgView[0];
            shCoCgCurInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo shAgeCurInfo{};
            shAgeCurInfo.imageView = g_volAgeView[0];
            shAgeCurInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSetAccelerationStructureKHR tlasWrite{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
            };
            tlasWrite.accelerationStructureCount = 1u;
            tlasWrite.pAccelerationStructures = &g_tlas;

            VkWriteDescriptorSet writes[12]{};
            for(uint32_t i = 0u; i < 12u; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = g_volRestirIntegrateSet;
                writes[i].descriptorCount = 1u;
            }

            writes[5].dstBinding = 16u;
            writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[5].pImageInfo = &shShYPrevInfo;
            writes[6].dstBinding = 17u;
            writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[6].pImageInfo = &shCoCgPrevInfo;
            writes[7].dstBinding = 18u;
            writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[7].pImageInfo = &shAgePrevInfo;
            writes[8].dstBinding = 19u;
            writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[8].pImageInfo = &shYCurInfo;
            writes[9].dstBinding = 20u;
            writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[9].pImageInfo = &shCoCgCurInfo;
            writes[10].dstBinding = 21u;
            writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[10].pImageInfo = &shAgeCurInfo;
            writes[11].dstBinding = 22u;
            writes[11].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            writes[11].pNext = &tlasWrite;
            writes[0].dstBinding = 0u;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &cameraInfo;
            writes[1].dstBinding = 1u;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[1].pImageInfo = &reservoirInfo;
            writes[2].dstBinding = 12u;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo = &lightInfo;
            writes[3].dstBinding = 13u;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[3].pBufferInfo = &gridInfo;
            writes[4].dstBinding = 15u;
            writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[4].pImageInfo = &radianceInfo;

            vkGuardUpdate(12u, writes, "vol");

            g_volRestirBoundIntegrateLightBuffer = g_localLightGpuBuffer;
            g_volRestirBoundIntegrateGridBuffer = g_localLightGridGpuBuffer;
            g_volRestirBoundIntegrateTlas = g_tlas;

            VkPushConstantRange pushRange{};
            pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pushRange.offset = 0u;
            pushRange.size = sizeof(VolRestirIntegratePushConstants);

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
            };
            pipelineLayoutInfo.setLayoutCount = 1u;
            pipelineLayoutInfo.pSetLayouts = &g_volRestirIntegrateSetLayout;
            pipelineLayoutInfo.pushConstantRangeCount = 1u;
            pipelineLayoutInfo.pPushConstantRanges = &pushRange;
            check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
                &g_volRestirIntegratePipelineLayout), "vol restir integrate layout failed");

            const std::vector<uint8_t> code = loadSpv("shaders/vol_restir_integrate.comp.spv");
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
            pipelineInfo.layout = g_volRestirIntegratePipelineLayout;
            const VkResult result = vkCreateComputePipelines(g_device,
                getSharedPipelineCache(), 1u, &pipelineInfo, nullptr,
                &g_volRestirIntegratePipeline);
            vkDestroyShaderModule(g_device, module, nullptr);
            check(result, "vol restir integrate pipeline failed");

            RTLog("[VOL RESTIR] vol restir integrate pipeline ok");
        }

        {
            VkDescriptorSetLayoutBinding bindings[4]{};
            bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            bindings[3] = { 3u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

            VkDescriptorSetLayoutCreateInfo layoutInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
            };
            layoutInfo.bindingCount = 4u;
            layoutInfo.pBindings = bindings;
            check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
                &g_volRestirCompositeSetLayout), "vol restir composite set layout failed");

            VkDescriptorPoolSize poolSizes[3] = {
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u },
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
            };
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = 1u;
            poolInfo.poolSizeCount = 3u;
            poolInfo.pPoolSizes = poolSizes;
            check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
                &g_volRestirCompositeDescriptorPool), "vol restir composite pool failed");

            VkDescriptorSetAllocateInfo allocateInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
            };
            allocateInfo.descriptorPool = g_volRestirCompositeDescriptorPool;
            allocateInfo.descriptorSetCount = 1u;
            allocateInfo.pSetLayouts = &g_volRestirCompositeSetLayout;
            check(vkAllocateDescriptorSets(g_device, &allocateInfo,
                &g_volRestirCompositeSet), "vol restir composite set failed");

            VkDescriptorBufferInfo cameraInfo{};
            cameraInfo.buffer = g_cameraUboBuffer;
            cameraInfo.offset = 0u;
            cameraInfo.range = sizeof(CameraUBO);

            VkDescriptorImageInfo depthInfo{};
            depthInfo.sampler = volDepthSampler();
            depthInfo.imageView = g_dlssDepthView;
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo radianceInfo{};
            radianceInfo.sampler = volRadianceSampler();
            radianceInfo.imageView = g_volRadianceView[0];
            radianceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkDescriptorImageInfo outputInfo{};
            outputInfo.imageView = g_godRayTargetView;
            outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[4]{};
            for(uint32_t i = 0u; i < 4u; ++i)
            {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = g_volRestirCompositeSet;
                writes[i].descriptorCount = 1u;
            }
            writes[0].dstBinding = 0u;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &cameraInfo;
            writes[1].dstBinding = 1u;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].pImageInfo = &depthInfo;
            writes[2].dstBinding = 2u;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &radianceInfo;
            writes[3].dstBinding = 3u;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[3].pImageInfo = &outputInfo;

            vkGuardUpdate(4u, writes, "vol");

            VkPushConstantRange pushRange{};
            pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pushRange.offset = 0u;
            pushRange.size = sizeof(VolRestirCompositePushConstants);

            VkPipelineLayoutCreateInfo pipelineLayoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
            };
            pipelineLayoutInfo.setLayoutCount = 1u;
            pipelineLayoutInfo.pSetLayouts = &g_volRestirCompositeSetLayout;
            pipelineLayoutInfo.pushConstantRangeCount = 1u;
            pipelineLayoutInfo.pPushConstantRanges = &pushRange;
            check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
                &g_volRestirCompositePipelineLayout), "vol restir composite layout failed");

            const std::vector<uint8_t> code = loadSpv("shaders/vol_restir_composite.comp.spv");
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
            pipelineInfo.layout = g_volRestirCompositePipelineLayout;
            const VkResult result = vkCreateComputePipelines(g_device,
                getSharedPipelineCache(), 1u, &pipelineInfo, nullptr,
                &g_volRestirCompositePipeline);
            vkDestroyShaderModule(g_device, module, nullptr);
            check(result, "vol restir composite pipeline failed");

            RTLog("[VOL RESTIR] vol restir composite pipeline ok");
        }

        RTLog("[VOL RESTIR] integrate + composite pipelines ready");
    }
    catch(const std::exception& error)
    {
        g_volRestirPipelineFailed = true;
        RTLog(std::string("[VOL RESTIR] output pipeline unavailable: ") + error.what());
    }
}

// S3b：可见性 pass 的描述符与管线。TLAS 首帧可能还没建好，因此每次录制时
// 尝试一次（g_tlas 就绪后自然建起）；失败一次就置 g_volRestirPipelineFailed。
void createVolumeRestirVisibilityPipeline()
{
    if(g_volRestirVisibilityPipeline != VK_NULL_HANDLE || g_volRestirPipelineFailed)
    {
        return;
    }
    if(g_tlas == VK_NULL_HANDLE || g_volReservoirView[1] == VK_NULL_HANDLE ||
       g_localLightGpuBuffer == VK_NULL_HANDLE || g_localLightGridGpuBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    try
    {
        VkDescriptorSetLayoutBinding bindings[5]{};
        bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[3] = { 12u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[4] = { 13u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

        VkDescriptorSetLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        layoutInfo.bindingCount = 5u;
        layoutInfo.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
            &g_volRestirVisibilitySetLayout), "vol restir visibility set layout failed");

        VkDescriptorPoolSize poolSizes[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2u },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1u;
        poolInfo.poolSizeCount = 3u;
        poolInfo.pPoolSizes = poolSizes;
        check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
            &g_volRestirVisibilityDescriptorPool), "vol restir visibility pool failed");

        VkDescriptorSetAllocateInfo allocateInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        allocateInfo.descriptorPool = g_volRestirVisibilityDescriptorPool;
        allocateInfo.descriptorSetCount = 1u;
        allocateInfo.pSetLayouts = &g_volRestirVisibilitySetLayout;
        check(vkAllocateDescriptorSets(g_device, &allocateInfo,
            &g_volRestirVisibilitySet), "vol restir visibility set failed");

        VkDescriptorBufferInfo cameraInfo{};
        cameraInfo.buffer = g_cameraUboBuffer;
        cameraInfo.offset = 0u;
        cameraInfo.range = sizeof(CameraUBO);

        VkDescriptorBufferInfo lightInfo{};
        lightInfo.buffer = g_localLightGpuBuffer;
        lightInfo.offset = 0u;
        lightInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo gridInfo{};
        gridInfo.buffer = g_localLightGridGpuBuffer;
        gridInfo.offset = 0u;
        gridInfo.range = VK_WHOLE_SIZE;

        VkDescriptorImageInfo reservoirInfo{};
        reservoirInfo.imageView = g_volReservoirView[1];
        reservoirInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        tlasWrite.accelerationStructureCount = 1u;
        tlasWrite.pAccelerationStructures = &g_tlas;

        VkWriteDescriptorSet writes[5]{};
        for(uint32_t i = 0u; i < 5u; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = g_volRestirVisibilitySet;
            writes[i].descriptorCount = 1u;
        }
        writes[0].dstBinding = 0u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &cameraInfo;
        writes[1].dstBinding = 1u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &reservoirInfo;
        writes[2].dstBinding = 2u;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[2].pNext = &tlasWrite;
        writes[3].dstBinding = 12u;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &lightInfo;
        writes[4].dstBinding = 13u;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[4].pBufferInfo = &gridInfo;

        vkGuardUpdate(5u, writes, "vol");

        g_volRestirBoundVisibilityTlas = g_tlas;
        g_volRestirBoundVisibilityLightBuffer = g_localLightGpuBuffer;
        g_volRestirBoundVisibilityGridBuffer = g_localLightGridGpuBuffer;

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0u;
        pushRange.size = sizeof(VolRestirIntegratePushConstants);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_volRestirVisibilitySetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1u;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_volRestirVisibilityPipelineLayout), "vol restir visibility layout failed");

        const std::vector<uint8_t> code =
            loadSpv("shaders/vol_restir_visibility.comp.spv");
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
        pipelineInfo.layout = g_volRestirVisibilityPipelineLayout;
        const VkResult result = vkCreateComputePipelines(g_device,
            getSharedPipelineCache(), 1u, &pipelineInfo, nullptr,
            &g_volRestirVisibilityPipeline);
        vkDestroyShaderModule(g_device, module, nullptr);
        check(result, "vol restir visibility pipeline failed");

        RTLog("[VOL RESTIR] vol restir visibility pipeline ok");

        RTLog("[VOL RESTIR] visibility pipeline ready");
    }
    catch(const std::exception& error)
    {
        g_volRestirPipelineFailed = true;
        RTLog(std::string("[VOL RESTIR] visibility pipeline unavailable: ") + error.what());
    }
}

// S3b：可见性 dispatch。网格与 spatial 相同（restir 尺寸）。
void recordVolumeRestirVisibility(VkCommandBuffer cmd)
{
    if(g_volRestirVisibilityPipeline == VK_NULL_HANDLE ||
       g_volRestirVisibilitySet == VK_NULL_HANDLE ||
       g_volRestirW == 0u || g_volRestirH == 0u)
    {
        return;
    }

    if(g_tlas == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_volRestirBoundVisibilityTlas != g_tlas ||
       g_volRestirBoundVisibilityLightBuffer != g_localLightGpuBuffer ||
       g_volRestirBoundVisibilityGridBuffer != g_localLightGridGpuBuffer)
    {
        VkDescriptorBufferInfo lightInfo{};
        lightInfo.buffer = g_localLightGpuBuffer;
        lightInfo.offset = 0u;
        lightInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo gridInfo{};
        gridInfo.buffer = g_localLightGridGpuBuffer;
        gridInfo.offset = 0u;
        gridInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        tlasWrite.accelerationStructureCount = 1u;
        tlasWrite.pAccelerationStructures = &g_tlas;

        VkWriteDescriptorSet writes[3]{};
        for(uint32_t i = 0u; i < 3u; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = g_volRestirVisibilitySet;
            writes[i].descriptorCount = 1u;
        }
        writes[0].dstBinding = 2u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[0].pNext = &tlasWrite;
        writes[1].dstBinding = 12u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &lightInfo;
        writes[2].dstBinding = 13u;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &gridInfo;

        vkGuardUpdate(3u, writes, "vol");

        g_volRestirBoundVisibilityTlas = g_tlas;
        g_volRestirBoundVisibilityLightBuffer = g_localLightGpuBuffer;
        g_volRestirBoundVisibilityGridBuffer = g_localLightGridGpuBuffer;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_volReservoirImage[1];
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
        0u, nullptr, 0u, nullptr, 1u, &barrier);

    VolRestirIntegratePushConstants pushConstants{};
    pushConstants.froxelSize[0] = static_cast<int32_t>(g_volFroxelW);
    pushConstants.froxelSize[1] = static_cast<int32_t>(g_volFroxelH);
    pushConstants.froxelSize[2] = static_cast<int32_t>(VOL_RESTIR_DEPTH);
    pushConstants.froxelSize[3] = static_cast<int32_t>(VOL_RESTIR_DOWNSCALE);
    pushConstants.volumeParams[0] = VOL_RESTIR_MAX_DISTANCE;
    pushConstants.volumeParams[1] = VOL_RESTIR_DEPTH_EXPONENT;
    pushConstants.volumeParams[2] = 0.0f;
    pushConstants.volumeParams[3] = 0.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volRestirVisibilityPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volRestirVisibilityPipelineLayout, 0u, 1u, &g_volRestirVisibilitySet, 0u, nullptr);
    vkCmdPushConstants(cmd, g_volRestirVisibilityPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(VolRestirIntegratePushConstants), &pushConstants);
    vkCmdDispatch(cmd,
        (g_volRestirW + 7u) / 8u,
        (g_volRestirH + 7u) / 8u,
        VOL_RESTIR_DEPTH);
}

// S4：froxel 入散射。dispatch 尺寸是 **froxel** 网格（不是 restir 网格）。
void recordVolumeRestirIntegrate(VkCommandBuffer cmd)
{
    if(g_volRestirIntegratePipeline == VK_NULL_HANDLE || g_volRestirIntegrateSet == VK_NULL_HANDLE ||
       g_volFroxelW == 0u || g_volFroxelH == 0u)
    {
        return;
    }

    if(g_tlas == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_volRestirBoundIntegrateLightBuffer != g_localLightGpuBuffer ||
       g_volRestirBoundIntegrateGridBuffer != g_localLightGridGpuBuffer ||
       g_volRestirBoundIntegrateTlas != g_tlas)
    {
        VkDescriptorBufferInfo lightInfo{};
        lightInfo.buffer = g_localLightGpuBuffer;
        lightInfo.offset = 0u;
        lightInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo gridInfo{};
        gridInfo.buffer = g_localLightGridGpuBuffer;
        gridInfo.offset = 0u;
        gridInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSetAccelerationStructureKHR tlasWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        tlasWrite.accelerationStructureCount = 1u;
        tlasWrite.pAccelerationStructures = &g_tlas;

        VkWriteDescriptorSet writes[3]{};
        for(uint32_t i = 0u; i < 3u; ++i)
        {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = g_volRestirIntegrateSet;
            writes[i].descriptorCount = 1u;
        }
        writes[0].dstBinding = 12u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &lightInfo;
        writes[1].dstBinding = 13u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &gridInfo;
        writes[2].dstBinding = 22u;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes[2].pNext = &tlasWrite;

        vkGuardUpdate(3u, writes, "vol");

        g_volRestirBoundIntegrateLightBuffer = g_localLightGpuBuffer;
        g_volRestirBoundIntegrateGridBuffer = g_localLightGridGpuBuffer;
        g_volRestirBoundIntegrateTlas = g_tlas;
    }

    VkImageMemoryBarrier barriers[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        VkImageMemoryBarrier& barrier = barriers[i];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.layerCount = 1u;
    }
    barriers[0].image = g_volReservoirImage[1];
    barriers[1].image = g_volRadianceImage[0];

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
        0u, nullptr, 0u, nullptr, 2u, barriers);

    VolRestirIntegratePushConstants pushConstants{};
    pushConstants.froxelSize[0] = static_cast<int32_t>(g_volFroxelW);
    pushConstants.froxelSize[1] = static_cast<int32_t>(g_volFroxelH);
    pushConstants.froxelSize[2] = static_cast<int32_t>(VOL_RESTIR_DEPTH);
    pushConstants.froxelSize[3] = static_cast<int32_t>(VOL_RESTIR_DOWNSCALE);
    pushConstants.volumeParams[0] = VOL_RESTIR_MAX_DISTANCE;
    pushConstants.volumeParams[1] = VOL_RESTIR_DEPTH_EXPONENT;
    pushConstants.volumeParams[2] = 0.0f;
    pushConstants.volumeParams[3] = 0.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volRestirIntegratePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volRestirIntegratePipelineLayout, 0u, 1u, &g_volRestirIntegrateSet, 0u, nullptr);
    vkCmdPushConstants(cmd, g_volRestirIntegratePipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(VolRestirIntegratePushConstants), &pushConstants);
    vkCmdDispatch(cmd,
        (g_volFroxelW + 7u) / 8u,
        (g_volFroxelH + 7u) / 8u,
        VOL_RESTIR_DEPTH);

    VkImageMemoryBarrier readBarrier{};
    readBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    readBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    readBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    readBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    readBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    readBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readBarrier.image = g_volRadianceImage[0];
    readBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    readBarrier.subresourceRange.levelCount = 1u;
    readBarrier.subresourceRange.layerCount = 1u;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
        0u, nullptr, 0u, nullptr, 1u, &readBarrier);

    {
        VkImageSubresourceRange shRange{};
        shRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        shRange.baseMipLevel = 0u;
        shRange.levelCount = 1u;
        shRange.baseArrayLayer = 0u;
        shRange.layerCount = 1u;

        VkClearColorValue shZero{};
        shZero.float32[0] = 0.0f;
        shZero.float32[1] = 0.0f;
        shZero.float32[2] = 0.0f;
        shZero.float32[3] = 0.0f;

        VkImageMemoryBarrier shPrepare[3]{};
        for(uint32_t i = 0u; i < 3u; ++i)
        {
            shPrepare[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            shPrepare[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            shPrepare[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            shPrepare[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            shPrepare[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            shPrepare[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            shPrepare[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            shPrepare[i].subresourceRange = shRange;
        }
        shPrepare[0].image = g_volShYImage[0];
        shPrepare[1].image = g_volShCoCgImage[0];
        shPrepare[2].image = g_volAgeImage[0];

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u,
            0u, nullptr, 0u, nullptr, 3u, shPrepare);

        if(g_volShNeedsInit)
        {
            vkCmdClearColorImage(cmd, g_volShYImage[1], VK_IMAGE_LAYOUT_GENERAL,
                &shZero, 1u, &shRange);
            vkCmdClearColorImage(cmd, g_volShCoCgImage[1], VK_IMAGE_LAYOUT_GENERAL,
                &shZero, 1u, &shRange);
            vkCmdClearColorImage(cmd, g_volAgeImage[1], VK_IMAGE_LAYOUT_GENERAL,
                &shZero, 1u, &shRange);
            g_volShNeedsInit = false;
        }

        VkImageCopy shRegion{};
        shRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        shRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u };
        shRegion.extent.width = g_volFroxelW;
        shRegion.extent.height = g_volFroxelH;
        shRegion.extent.depth = VOL_RESTIR_DEPTH;

        vkCmdCopyImage(cmd, g_volShYImage[0], VK_IMAGE_LAYOUT_GENERAL,
            g_volShYImage[1], VK_IMAGE_LAYOUT_GENERAL, 1u, &shRegion);
        vkCmdCopyImage(cmd, g_volShCoCgImage[0], VK_IMAGE_LAYOUT_GENERAL,
            g_volShCoCgImage[1], VK_IMAGE_LAYOUT_GENERAL, 1u, &shRegion);
        vkCmdCopyImage(cmd, g_volAgeImage[0], VK_IMAGE_LAYOUT_GENERAL,
            g_volAgeImage[1], VK_IMAGE_LAYOUT_GENERAL, 1u, &shRegion);

        VkImageMemoryBarrier shDone[3]{};
        for(uint32_t i = 0u; i < 3u; ++i)
        {
            shDone[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            shDone[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            shDone[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            shDone[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            shDone[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            shDone[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            shDone[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            shDone[i].subresourceRange = shRange;
        }
        shDone[0].image = g_volShYImage[1];
        shDone[1].image = g_volShCoCgImage[1];
        shDone[2].image = g_volAgeImage[1];

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
            0u, nullptr, 0u, nullptr, 3u, shDone);
    }
}

// S4：屏幕积分，输出到 g_godRayTargetImage（随后由 god_ray_composite 做
// scene * a + rgb）。**必须**排在旧体积光之后调用，否则会被它写的"中性值"覆盖。
void recordVolumeRestirScreenComposite(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE || volRestirEnabled() == 0u)
    {
        return;
    }
    if(g_volRestirCompositePipeline == VK_NULL_HANDLE || g_volRestirCompositeSet == VK_NULL_HANDLE ||
       g_godRayWidth == 0u || g_godRayHeight == 0u || g_godRayTargetImage == VK_NULL_HANDLE)
    {
        return;
    }

    // 首帧（或刚重建图像时）layout 还是 UNDEFINED：这种 oldLayout 下
    // srcAccessMask 必须为 0、source stage 必须是 TOP_OF_PIPE，否则是 VUID 且
    // 首帧存在潜在 hazard（本 pass 现在每帧最先碰这张图）。
    const bool targetHadLayout =
        (g_godRayTargetLayout != VK_IMAGE_LAYOUT_UNDEFINED);

    VkImageMemoryBarrier targetBarrier{};
    targetBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    targetBarrier.srcAccessMask =
        targetHadLayout ? VK_ACCESS_SHADER_WRITE_BIT : 0u;
    targetBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    targetBarrier.oldLayout = g_godRayTargetLayout;
    targetBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    targetBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    targetBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    targetBarrier.image = g_godRayTargetImage;
    targetBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    targetBarrier.subresourceRange.levelCount = 1u;
    targetBarrier.subresourceRange.layerCount = 1u;

    vkCmdPipelineBarrier(cmd,
        targetHadLayout ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                        : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
        0u, nullptr, 0u, nullptr, 1u, &targetBarrier);

    {
        // 场景深度：本 pass 现在排在 recordVolumetricClouds 之前（见调用点注释），
        // 此时深度只被 RT pass 写过，recordVolumetricClouds 里那条
        // RAY_TRACING -> COMPUTE 的读屏障还没执行。这里补一条，与调用位置无关地
        // 保证 textureLod(sceneDepth) 的读后写顺序。
        VkImageMemoryBarrier depthBarrier{};
        depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depthBarrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        depthBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        depthBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depthBarrier.image = g_dlssDepthImage;
        depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        depthBarrier.subresourceRange.levelCount = 1u;
        depthBarrier.subresourceRange.layerCount = 1u;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
            0u, nullptr, 0u, nullptr, 1u, &depthBarrier);
    }

    VolRestirCompositePushConstants pushConstants{};
    pushConstants.params[0] = VOL_RESTIR_MAX_DISTANCE;
    pushConstants.params[1] = VOL_RESTIR_DEPTH_EXPONENT;
    pushConstants.params[2] = static_cast<float>(VOL_RESTIR_COMPOSITE_STEPS);
    pushConstants.params[3] = VOL_RESTIR_EXTINCTION_SCALE;
    pushConstants.grid[0] = static_cast<int32_t>(g_volFroxelW);
    pushConstants.grid[1] = static_cast<int32_t>(g_volFroxelH);
    pushConstants.grid[2] = static_cast<int32_t>(VOL_RESTIR_DEPTH);
    pushConstants.grid[3] = 0;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volRestirCompositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volRestirCompositePipelineLayout, 0u, 1u, &g_volRestirCompositeSet, 0u, nullptr);
    vkCmdPushConstants(cmd, g_volRestirCompositePipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(VolRestirCompositePushConstants), &pushConstants);
    vkCmdDispatch(cmd,
        (g_godRayWidth + 7u) / 8u,
        (g_godRayHeight + 7u) / 8u,
        1u);

    g_godRayTargetLayout = VK_IMAGE_LAYOUT_GENERAL;
}

// S2：INITIAL 阶段 dispatch。S3 起在其后接 temporal / spatial。
void recordVolumeRestir(VkCommandBuffer cmd, uint32_t renderWidth, uint32_t renderHeight)
{
    if(cmd == VK_NULL_HANDLE || volRestirEnabled() == 0u)
    {
        return;
    }

    createVolumeRestirResources(renderWidth, renderHeight);
    createVolumeRestirPipeline();

    if(g_volRestirPipeline == VK_NULL_HANDLE || g_volRestirSet == VK_NULL_HANDLE ||
       g_volRestirW == 0u || g_volRestirH == 0u)
    {
        return;
    }

    if(g_volRestirBoundLightBuffer != g_localLightGpuBuffer ||
       g_volRestirBoundGridBuffer != g_localLightGridGpuBuffer)
    {
        VkDescriptorBufferInfo lightInfo{};
        lightInfo.buffer = g_localLightGpuBuffer;
        lightInfo.offset = 0u;
        lightInfo.range = VK_WHOLE_SIZE;

        VkDescriptorBufferInfo gridInfo{};
        gridInfo.buffer = g_localLightGridGpuBuffer;
        gridInfo.offset = 0u;
        gridInfo.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = g_volRestirSet;
        writes[0].dstBinding = 12u;
        writes[0].descriptorCount = 1u;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].pBufferInfo = &lightInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = g_volRestirSet;
        writes[1].dstBinding = 13u;
        writes[1].descriptorCount = 1u;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &gridInfo;

        vkGuardUpdate(2u, writes, "vol");

        g_volRestirBoundLightBuffer = g_localLightGpuBuffer;
        g_volRestirBoundGridBuffer = g_localLightGridGpuBuffer;
    }

    VkImageMemoryBarrier imageBarriers[3]{};
    for(uint32_t i = 0u; i < 3u; ++i)
    {
        VkImageMemoryBarrier& barrier = imageBarriers[i];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = g_volRestirLayoutReady ? VK_ACCESS_SHADER_WRITE_BIT : 0u;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = g_volRestirLayoutReady
            ? VK_IMAGE_LAYOUT_GENERAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.layerCount = 1u;
    }
    imageBarriers[0].image = g_volReservoirImage[0];
    imageBarriers[1].image = g_volRadianceImage[0];
    imageBarriers[2].image = g_volReservoirImage[1];

    vkCmdPipelineBarrier(cmd,
        g_volRestirLayoutReady
            ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
        0u, nullptr, 0u, nullptr, 3u, imageBarriers);

    g_volRestirLayoutReady = true;

    VolRestirPushConstants pushConstants{};
    pushConstants.froxelSize[0] = static_cast<int32_t>(g_volFroxelW);
    pushConstants.froxelSize[1] = static_cast<int32_t>(g_volFroxelH);
    pushConstants.froxelSize[2] = static_cast<int32_t>(VOL_RESTIR_DEPTH);
    pushConstants.froxelSize[3] = static_cast<int32_t>(VOL_RESTIR_DOWNSCALE);
    pushConstants.volumeParams[0] = VOL_RESTIR_MAX_DISTANCE;
    pushConstants.volumeParams[1] = VOL_RESTIR_DEPTH_EXPONENT;
    pushConstants.volumeParams[2] = 0.0f;
    pushConstants.volumeParams[3] = 0.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volRestirPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volRestirPipelineLayout, 0u, 1u, &g_volRestirSet, 0u, nullptr);
    vkCmdPushConstants(cmd, g_volRestirPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
        0u, sizeof(VolRestirPushConstants), &pushConstants);

    vkCmdDispatch(cmd,
        (g_volRestirW + 7u) / 8u,
        (g_volRestirH + 7u) / 8u,
        VOL_RESTIR_DEPTH);

    VkImageMemoryBarrier readBarriers[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        VkImageMemoryBarrier& barrier = readBarriers[i];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1u;
        barrier.subresourceRange.layerCount = 1u;
    }
    readBarriers[0].image = g_volReservoirImage[0];
    readBarriers[1].image = g_volRadianceImage[0];

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u,
        0u, nullptr, 0u, nullptr, 2u, readBarriers);

    createVolumeRestirChainPipelines();
    recordVolumeRestirChain(cmd);

    createVolumeRestirVisibilityPipeline();
    recordVolumeRestirVisibility(cmd);

    createVolumeRestirOutputPipelines();
    recordVolumeRestirIntegrate(cmd);
}
