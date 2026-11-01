// Orion backend — Sky 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// 2026-10-06：星图也进天空 LUT（见 sky_atmosphere.comp / star_map_common.glsl）。
// 星图由 Java 侧异步发布、资源重载会换视图，所以不能只在创建期写一次描述符。
void updateSkyAtmosphereStarMapDescriptor()
{
    if(g_skyAtmosphereDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }
    VkDescriptorImageInfo starMapInfo{};
    starMapInfo.imageView = g_starMapTextureView != VK_NULL_HANDLE
        ? g_starMapTextureView : g_atlasImageView;
    starMapInfo.sampler = g_starMapTextureSampler != VK_NULL_HANDLE
        ? g_starMapTextureSampler : g_atlasSampler;
    starMapInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = g_skyAtmosphereDescriptorSet;
    write.dstBinding = 6u;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &starMapInfo;
    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);
}

// 2026-10-09：末地银河图集也进天空 LUT（binding 7，见 sky_atmosphere.comp 的
// 末地分支与 end_sky_common.glsl）。与星图同理：图集由 Java 侧异步发布、资源
// 重载会换视图 ⇒ 不能只在创建期写一次描述符。句柄没到达时保持创建期的兜底
// （方块图集），绝不留下未写项。
void updateSkyAtmosphereEndSkyAtlasDescriptor()
{
    if(g_skyAtmosphereDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }
    VkDescriptorImageInfo endSkyAtlasInfo{};
    endSkyAtlasInfo.imageView = g_endSkyAtlasView != VK_NULL_HANDLE
        ? g_endSkyAtlasView : g_atlasImageView;
    endSkyAtlasInfo.sampler = g_endSkyAtlasSampler != VK_NULL_HANDLE
        ? g_endSkyAtlasSampler : g_atlasSampler;
    endSkyAtlasInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = g_skyAtmosphereDescriptorSet;
    write.dstBinding = 7u;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &endSkyAtlasInfo;
    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);
}
void updateSkyRayDescriptors()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_skyScatteringView == VK_NULL_HANDLE ||
       g_skyTransmittanceView == VK_NULL_HANDLE ||
       g_skyAtmosphereSampler == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo infos[2]{};
    infos[0].sampler = g_skyAtmosphereSampler;
    infos[0].imageView = g_skyScatteringView;
    infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    infos[1].sampler = g_skyAtmosphereSampler;
    infos[1].imageView = g_skyTransmittanceView;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding = 42u + i;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);
}
void destroyBrunetonResources()
{
    for(VkPipeline p : g_brPipelines)
    {
        if(p != VK_NULL_HANDLE) vkDestroyPipeline(g_device, p, nullptr);
    }
    if(g_brLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(g_device, g_brLayout, nullptr);
    if(g_brPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(g_device, g_brPool, nullptr);
    if(g_brSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(g_device, g_brSetLayout, nullptr);
    if(g_brLutSampler != VK_NULL_HANDLE) vkDestroySampler(g_device, g_brLutSampler, nullptr);
    BrunetonLutImage* images[] = {
        &g_brTransmittance, &g_brIrradiance, &g_brDeltaIrradiance,
        &g_brScattering, &g_brSingleMie, &g_brDeltaRayleigh,
        &g_brDeltaMie, &g_brDeltaMultiple, &g_brDeltaScatteringDensity };
    for(BrunetonLutImage* img : images)
    {
        if(img->sampledView != VK_NULL_HANDLE) vkDestroyImageView(g_device, img->sampledView, nullptr);
        if(img->storageView != VK_NULL_HANDLE) vkDestroyImageView(g_device, img->storageView, nullptr);
        if(img->image != VK_NULL_HANDLE) vkDestroyImage(g_device, img->image, nullptr);
        if(img->memory != VK_NULL_HANDLE) vkFreeMemory(g_device, img->memory, nullptr);
        *img = {};
    }
    g_brPrecomputed = false;

    if(g_brPlaceholderView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(g_device, g_brPlaceholderView, nullptr);
        g_brPlaceholderView = VK_NULL_HANDLE;
    }
    if(g_brPlaceholderImage != VK_NULL_HANDLE)
    {
        vkDestroyImage(g_device, g_brPlaceholderImage, nullptr);
        g_brPlaceholderImage = VK_NULL_HANDLE;
    }
    if(g_brPlaceholderMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_brPlaceholderMemory, nullptr);
        g_brPlaceholderMemory = VK_NULL_HANDLE;
    }
    if(g_brReadbackBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_brReadbackBuffer, nullptr);
        g_brReadbackBuffer = VK_NULL_HANDLE;
    }
    if(g_brReadbackMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_brReadbackMemory, nullptr);
        g_brReadbackMemory = VK_NULL_HANDLE;
    }
    g_brPrecomputeOnlyReleased = false;
}
void createBrunetonLutImage(BrunetonLutImage& dst, VkImageType type,
    uint32_t width, uint32_t height, uint32_t depth, VkFormat format)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = type;
    info.format = format;
    info.extent = {width, height, depth};
    info.mipLevels = 1u;
    info.arrayLayers = 1u;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(g_device, &info, nullptr, &dst.image),
        "bruneton lut image create failed");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, dst.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &dst.memory),
        "bruneton lut memory allocate failed");
    check(vkBindImageMemory(g_device, dst.image, dst.memory, 0u),
        "bruneton lut memory bind failed");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = dst.image;
    view.viewType = type == VK_IMAGE_TYPE_3D
        ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1u;
    view.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &view, nullptr, &dst.sampledView),
        "bruneton lut sampled view failed");
    check(vkCreateImageView(g_device, &view, nullptr, &dst.storageView),
        "bruneton lut storage view failed");
}

// ===========================================================================
// 2026-10-04 显存回收：预计算专用资源（4 张 delta 3D LUT + 取证回读缓冲）
//
// Bruneton 的 22 趟预计算需要 4 张 delta 中间 LUT
// （deltaIrradiance 64x16 RGBA32F + deltaRayleigh / deltaMie / deltaMultiple /
//   deltaScatteringDensity 各 256x128x32 RGBA16F，合计 33.55 MB），
// 外加一个 1 MB 的取证回读缓冲。**运行期没有任何 pass 再使用它们**：
//   · 预计算在 executeBrunetonPrecompute() 里以 vkQueueWaitIdle 收口，
//     所以在此点释放是同步安全的 —— 不是"赌 GPU 已经跑完"；
//   · g_brSet 只在 recordBrunetonPrecompute 内部被绑定/使用；运行期天空走的是
//     g_skyAtmosphereDescriptorSet 的 transmittance / scattering / singleMie 三张；
//   · g_brPrecomputed 一旦置位就不再复位（只有 destroyBrunetonResources 会），
//     因此不存在"释放之后又跑一次预计算"的路径。
//
// 释放后把 g_brSet 里指向它们的 12 个绑定改指一张 1x1x1 占位图：将来若有人给 Sky
// 着色器补上这些绑定，读到的是零，而不是已释放的显存（use-after-free）。
// 一键回退：注释掉 executeBrunetonPrecompute() 末尾的 releaseBrunetonPrecomputeOnlyLuts()。
// ===========================================================================
void destroyBrunetonLutImage(BrunetonLutImage& img)
{
    if(img.sampledView != VK_NULL_HANDLE) vkDestroyImageView(g_device, img.sampledView, nullptr);
    if(img.storageView != VK_NULL_HANDLE) vkDestroyImageView(g_device, img.storageView, nullptr);
    if(img.image != VK_NULL_HANDLE) vkDestroyImage(g_device, img.image, nullptr);
    if(img.memory != VK_NULL_HANDLE) vkFreeMemory(g_device, img.memory, nullptr);
    img = {};
}
void releaseBrunetonPrecomputeOnlyLuts()
{
    if(g_brPrecomputeOnlyReleased || g_brDeltaRayleigh.image == VK_NULL_HANDLE)
    {
        return;
    }
    g_brPrecomputeOnlyReleased = true;

    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_3D;
        info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        info.extent = {1u, 1u, 1u};
        info.mipLevels = 1u;
        info.arrayLayers = 1u;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(g_device, &info, nullptr, &g_brPlaceholderImage),
            "bruneton placeholder image create failed");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(g_device, g_brPlaceholderImage, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr, &g_brPlaceholderMemory),
            "bruneton placeholder memory allocate failed");
        check(vkBindImageMemory(g_device, g_brPlaceholderImage, g_brPlaceholderMemory, 0u),
            "bruneton placeholder memory bind failed");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = g_brPlaceholderImage;
        view.viewType = VK_IMAGE_VIEW_TYPE_3D;
        view.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1u;
        view.subresourceRange.layerCount = 1u;
        check(vkCreateImageView(g_device, &view, nullptr, &g_brPlaceholderView),
            "bruneton placeholder view create failed");
    }

    VkDescriptorImageInfo descriptors[12]{};
    VkWriteDescriptorSet writes[12]{};
    const int combinedBindings[7] = { 3, 4, 5, 15, 16, 17, 21 };
    const int storageBindings[5] = { 10, 11, 12, 18, 19 };
    uint32_t writeCount = 0u;
    for(int binding : combinedBindings)
    {
        descriptors[writeCount].sampler = g_brLutSampler;
        descriptors[writeCount].imageView = g_brPlaceholderView;
        descriptors[writeCount].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_brSet, static_cast<uint32_t>(binding), 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            &descriptors[writeCount], nullptr, nullptr };
        ++writeCount;
    }
    for(int binding : storageBindings)
    {
        descriptors[writeCount].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        descriptors[writeCount].imageView = g_brPlaceholderView;
        writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_brSet, static_cast<uint32_t>(binding), 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            &descriptors[writeCount], nullptr, nullptr };
        ++writeCount;
    }
    if(g_brSet != VK_NULL_HANDLE)
    {
        vkUpdateDescriptorSets(g_device, writeCount, writes, 0u, nullptr);
    }

    destroyBrunetonLutImage(g_brDeltaIrradiance);
    destroyBrunetonLutImage(g_brDeltaRayleigh);
    destroyBrunetonLutImage(g_brDeltaMie);
    destroyBrunetonLutImage(g_brDeltaMultiple);
    destroyBrunetonLutImage(g_brDeltaScatteringDensity);
    if(g_brReadbackBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device, g_brReadbackBuffer, nullptr);
        g_brReadbackBuffer = VK_NULL_HANDLE;
    }
    if(g_brReadbackMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(g_device, g_brReadbackMemory, nullptr);
        g_brReadbackMemory = VK_NULL_HANDLE;
    }

    RTLog("[SKY] 预计算专用资源已释放：4x delta LUT (33.55 MB) + readback (1.0 MB)；"
          "g_brSet 的 12 个绑定已改指 1x1x1 占位图");
}
void createBrunetonResources()
{
    if(g_brTransmittance.image != VK_NULL_HANDLE)
    {
        return;
    }
    RTLog("[SKY] bruneton resources: creating LUT images");
    createBrunetonLutImage(g_brTransmittance, VK_IMAGE_TYPE_2D,
        256u, 64u, 1u, VK_FORMAT_R32G32B32A32_SFLOAT);
    createBrunetonLutImage(g_brIrradiance, VK_IMAGE_TYPE_2D,
        64u, 16u, 1u, VK_FORMAT_R32G32B32A32_SFLOAT);
    createBrunetonLutImage(g_brDeltaIrradiance, VK_IMAGE_TYPE_2D,
        64u, 16u, 1u, VK_FORMAT_R32G32B32A32_SFLOAT);
    createBrunetonLutImage(g_brScattering, VK_IMAGE_TYPE_3D,
        256u, 128u, 32u, VK_FORMAT_R32G32B32A32_SFLOAT);
    createBrunetonLutImage(g_brSingleMie, VK_IMAGE_TYPE_3D,
        256u, 128u, 32u, VK_FORMAT_R32G32B32A32_SFLOAT);
    createBrunetonLutImage(g_brDeltaRayleigh, VK_IMAGE_TYPE_3D,
        256u, 128u, 32u, VK_FORMAT_R16G16B16A16_SFLOAT);
    createBrunetonLutImage(g_brDeltaMie, VK_IMAGE_TYPE_3D,
        256u, 128u, 32u, VK_FORMAT_R16G16B16A16_SFLOAT);
    createBrunetonLutImage(g_brDeltaMultiple, VK_IMAGE_TYPE_3D,
        256u, 128u, 32u, VK_FORMAT_R16G16B16A16_SFLOAT);
    createBrunetonLutImage(g_brDeltaScatteringDensity, VK_IMAGE_TYPE_3D,
        256u, 128u, 32u, VK_FORMAT_R16G16B16A16_SFLOAT);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_brLutSampler),
        "bruneton lut sampler create failed");

    VkDescriptorSetLayoutBinding finalBindings[18]{};
    finalBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[2] = { 3u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[3] = { 4u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[4] = { 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[5] = { 15u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[6] = { 16u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[7] = { 17u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[8] = { 21u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[9] = { 10u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[10] = { 11u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[11] = { 12u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[12] = { 13u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[13] = { 14u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[14] = { 18u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[15] = { 19u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[16] = { 20u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    finalBindings[17] = { 22u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 18u;
    layoutInfo.pBindings = finalBindings;
    check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
        &g_brSetLayout), "bruneton set layout failed");

    VkDescriptorPoolSize poolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 9u }
    };
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 2u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_brPool),
        "bruneton descriptor pool failed");

    VkDescriptorSetAllocateInfo setAllocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAllocation.descriptorPool = g_brPool;
    setAllocation.descriptorSetCount = 1u;
    setAllocation.pSetLayouts = &g_brSetLayout;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_brSet),
        "bruneton descriptor set allocate failed");

    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = 1024u * 1024u;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(g_device, &bufferInfo, nullptr, &g_brReadbackBuffer),
        "bruneton readback buffer create failed");
    VkMemoryRequirements readbackRequirements{};
    vkGetBufferMemoryRequirements(g_device, g_brReadbackBuffer,
        &readbackRequirements);
    VkMemoryAllocateInfo readbackAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    readbackAlloc.allocationSize = readbackRequirements.size;
    readbackAlloc.memoryTypeIndex = findMemoryType(g_physicalDevice,
        readbackRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(g_device, &readbackAlloc, nullptr,
        &g_brReadbackMemory), "bruneton readback memory allocate failed");
    check(vkBindBufferMemory(g_device, g_brReadbackBuffer,
        g_brReadbackMemory, 0u), "bruneton readback memory bind failed");
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.offset = 0u;
    pushRange.size = 4 * sizeof(int32_t);
    VkPipelineLayoutCreateInfo layoutCreateInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutCreateInfo.pSetLayouts = &g_brSetLayout;
    layoutCreateInfo.setLayoutCount = 1u;
    layoutCreateInfo.pushConstantRangeCount = 1u;
    layoutCreateInfo.pPushConstantRanges = &pushRange;
    check(vkCreatePipelineLayout(g_device, &layoutCreateInfo, nullptr,
        &g_brLayout), "bruneton pipeline layout failed");

    const char* passNames[6] = {
        "precompute_transmittance.comp.spv",
        "precompute_direct_irradiance.comp.spv",
        "precompute_single_scattering.comp.spv",
        "precompute_scattering_density.comp.spv",
        "precompute_indirect_irradiance.comp.spv",
        "precompute_multiple_scattering.comp.spv" };
    for(int i = 0; i < 6; ++i)
    {
        const std::vector<uint8_t> code = loadSpv(
            ("shaders/" + std::string(passNames[i])).c_str());
        VkShaderModuleCreateInfo moduleInfo{
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = code.size();
        moduleInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule shader = VK_NULL_HANDLE;
        check(vkCreateShaderModule(g_device, &moduleInfo, nullptr, &shader),
            "bruneton shader module failed");
        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = shader;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = g_brLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &g_brPipelines[i]),
            "bruneton pipeline failed");
        vkDestroyShaderModule(g_device, shader, nullptr);
    }

    VkDescriptorImageInfo infos[18]{};
    const std::pair<int, VkImageView*> combinedMap[9] = {
        { 0, &g_brTransmittance.sampledView },
        { 1, &g_brIrradiance.sampledView },
        { 3, &g_brDeltaRayleigh.sampledView },
        { 4, &g_brDeltaMie.sampledView },
        { 5, &g_brDeltaMultiple.sampledView },
        { 15, &g_brDeltaRayleigh.sampledView },
        { 16, &g_brDeltaMie.sampledView },
        { 17, &g_brDeltaMultiple.sampledView },
        { 21, &g_brDeltaScatteringDensity.sampledView } };
    const std::pair<int, VkImageView*> storageMap[9] = {
        { 10, &g_brDeltaIrradiance.storageView },
        { 11, &g_brDeltaRayleigh.storageView },
        { 12, &g_brDeltaMie.storageView },
        { 13, &g_brScattering.storageView },
        { 14, &g_brSingleMie.storageView },
        { 18, &g_brDeltaScatteringDensity.storageView },
        { 19, &g_brDeltaMultiple.storageView },
        { 20, &g_brIrradiance.storageView },
        { 22, &g_brTransmittance.storageView } };
    VkWriteDescriptorSet writes[18]{};
    int writeCount = 0;
    for(const auto& entry : combinedMap)
    {
        infos[writeCount].sampler = g_brLutSampler;
        infos[writeCount].imageView = *entry.second;
        infos[writeCount].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_brSet, static_cast<uint32_t>(entry.first), 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            &infos[writeCount], nullptr, nullptr };
        ++writeCount;
    }
    for(const auto& entry : storageMap)
    {
        infos[writeCount].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        infos[writeCount].imageView = *entry.second;
        writes[writeCount] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_brSet, static_cast<uint32_t>(entry.first), 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            &infos[writeCount], nullptr, nullptr };
        ++writeCount;
    }
    vkUpdateDescriptorSets(g_device,
        static_cast<uint32_t>(writeCount), writes, 0u, nullptr);
    RTLog("[SKY] bruneton resources: images+descriptors+6 pipelines ready");
}
void recordBrunetonPrecompute(VkCommandBuffer cmd)
{
    struct BrPass {
        int pipeline;
        int additive;
        int order;
        int output;
        uint32_t groups[3];
    };
    const BrPass base[] = {
        { 0, 0, 0, 0, { 32u, 8u, 1u } },
        { 1, 0, 0, 0, { 8u, 2u, 1u } },
        { 1, 0, 0, 1, { 8u, 2u, 1u } },
        { 2, 0, 0, 0, { 32u, 16u, 32u } },
        { 2, 0, 0, 1, { 32u, 16u, 32u } },
        { 2, 0, 0, 2, { 32u, 16u, 32u } },
        { 2, 0, 0, 3, { 32u, 16u, 32u } },
    };
    VkMemoryBarrier memoryBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT;
    BrunetonLutImage* lutImages[] = { &g_brTransmittance, &g_brIrradiance,
        &g_brDeltaIrradiance, &g_brScattering, &g_brSingleMie,
        &g_brDeltaRayleigh, &g_brDeltaMie, &g_brDeltaMultiple,
        &g_brDeltaScatteringDensity };
    for(BrunetonLutImage* img : lutImages)
    {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img->image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1u;
        b.subresourceRange.layerCount = 1u;
        b.srcAccessMask = 0u;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &b);
    }

    int32_t push[4] = { 0, 0, 0, 0 };
    auto run = [&](const BrPass& pass) {
        push[0] = 0;
        push[1] = pass.additive;
        push[2] = pass.order;
        push[3] = pass.output;
        vkCmdPushConstants(cmd, g_brLayout, VK_SHADER_STAGE_COMPUTE_BIT,
            0u, sizeof(push), push);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_brPipelines[pass.pipeline]);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_brLayout, 0u, 1u, &g_brSet, 0u, nullptr);
        vkCmdDispatch(cmd, pass.groups[0], pass.groups[1], pass.groups[2]);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &memoryBarrier,
            0u, nullptr, 0u, nullptr);
    };
    for(const BrPass& pass : base)
    {
        run(pass);
    }
    for(int order = 2; order <= 4; ++order)
    {
        const BrPass density = { 3, 0, order, 0, { 32u, 16u, 32u } };
        const BrPass indirectDelta = { 4, 0, order, 0, { 8u, 2u, 1u } };
        const BrPass indirectAccum = { 4, 1, order, 1, { 8u, 2u, 1u } };
        const BrPass multipleDelta = { 5, 0, 0, 0, { 32u, 16u, 32u } };
        const BrPass multipleAccum = { 5, 1, 0, 1, { 32u, 16u, 32u } };
        run(density);
        run(indirectDelta);
        run(indirectAccum);
        run(multipleDelta);
        run(multipleAccum);
    }

    auto transitionToSrc = [&](VkImage image) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1u;
        b.subresourceRange.layerCount = 1u;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &b);
    };
    auto transitionBack = [&](VkImage image) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1u;
        b.subresourceRange.layerCount = 1u;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
            1u, &b);
    };
    transitionToSrc(g_brTransmittance.image);
    transitionToSrc(g_brScattering.image);
    VkBufferImageCopy regions[2]{};
    regions[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    regions[0].imageSubresource.layerCount = 1u;
    regions[0].imageExtent = {256u, 64u, 1u};
    regions[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    regions[1].imageSubresource.layerCount = 1u;
    regions[1].imageOffset = {0u, 0u, 16u};
    regions[1].imageExtent = {256u, 128u, 1u};
    regions[1].bufferOffset = 512u * 1024u;
    vkCmdCopyImageToBuffer(cmd, g_brTransmittance.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_brReadbackBuffer, 1u,
        &regions[0]);
    vkCmdCopyImageToBuffer(cmd, g_brScattering.image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_brReadbackBuffer, 1u,
        &regions[1]);
    transitionBack(g_brTransmittance.image);
    transitionBack(g_brScattering.image);
}
void executeBrunetonPrecompute()
{
    VkCommandBufferAllocateInfo allocation{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = g_rtCommandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &allocation, &commandBuffer),
        "bruneton precompute command buffer failed");
    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &beginInfo),
        "bruneton precompute begin failed");
    {
        void* prefill = nullptr;
        if(vkMapMemory(g_device, g_brReadbackMemory, 0u, VK_WHOLE_SIZE, 0u,
            &prefill) == VK_SUCCESS)
        {
            uint32_t* words = static_cast<uint32_t*>(prefill);
            for(uint32_t i = 0; i < 1024u * 1024u / 4u; ++i)
            {
                words[i] = 0xDEADBEEFu;
            }
            vkUnmapMemory(g_device, g_brReadbackMemory);
        }
    }
    recordBrunetonPrecompute(commandBuffer);
    check(vkEndCommandBuffer(commandBuffer), "bruneton precompute end failed");
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1u;
    submitInfo.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submitInfo, VK_NULL_HANDLE),
        "bruneton precompute submit failed");
    check(vkQueueWaitIdle(g_queue), "bruneton precompute wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    g_brPrecomputed = true;

    void* mapped = nullptr;
    if(vkMapMemory(g_device, g_brReadbackMemory, 0u, VK_WHOLE_SIZE, 0u,
        &mapped) == VK_SUCCESS)
    {
        const uint32_t* words = static_cast<const uint32_t*>(mapped);
        if(words[0] == 0xDEADBEEFu && words[4096] == 0xDEADBEEFu)
        {
            RTLog("[SKY] LUT readback: 魔数原样 —— 拷贝从未写入（图空/派发断链）");
        }
        else
        {
            RTLog("[SKY] LUT readback: 魔数被覆盖 —— 拷贝执行了");
        }
        const float* t = static_cast<const float*>(mapped);
        const float* sc = static_cast<const float*>(mapped) +
            (512u * 1024u / 4u);
        char line[512];
        for(int py : {0, 32, 63})
        {
            const int idx = (py * 256 + 128) * 4;
            std::snprintf(line, sizeof(line),
                "[SKY] LUT transmittance[y=%d,x=128] = %.4f %.4f %.4f",
                py, t[idx], t[idx + 1], t[idx + 2]);
            RTLog(line);
        }
        for(int px : {128, 255})
        {
            for(int py : {0, 64, 127})
            {
                const int idx = (py * 256 + px) * 4;
                std::snprintf(line, sizeof(line),
                    "[SKY] LUT scattering[z=16,y=%d,x=%d] = %.6f %.6f %.6f",
                    py, px, sc[idx], sc[idx + 1], sc[idx + 2]);
                RTLog(line);
            }
        }
        vkUnmapMemory(g_device, g_brReadbackMemory);
    }
    RTLog("[SKY] Bruneton precompute LUT done");

    releaseBrunetonPrecomputeOnlyLuts();
}
void createSkyAtmosphereResources()
{
    if(g_skyScatteringImage != VK_NULL_HANDLE)
    {
        return;
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(
        g_physicalDevice, VK_FORMAT_R16G16B16A16_SFLOAT, &formatProperties
    );
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        throw std::runtime_error(
            "VK_FORMAT_R16G16B16A16_SFLOAT cannot be sampled and stored for the sky atmosphere"
        );
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = { SKY_SCATTER_RESOLUTION_X, SKY_SCATTER_RESOLUTION_Y, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    auto createImage = [&](VkImage& image, VkDeviceMemory& memory, VkImageView& view,
                           const char* imageError, const char* allocateError, const char* viewError)
    {
        check(vkCreateImage(g_device, &imageInfo, nullptr, &image), imageError);
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(g_device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr, &memory), allocateError);
        check(vkBindImageMemory(g_device, image, memory, 0u), allocateError);
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1u;
        viewInfo.subresourceRange.layerCount = 1u;
        check(vkCreateImageView(g_device, &viewInfo, nullptr, &view), viewError);
    };
    createImage(g_skyScatteringImage, g_skyScatteringMemory, g_skyScatteringView,
        "sky scattering image create failed", "sky scattering image allocate failed",
        "sky scattering view create failed");
    createImage(g_skyTransmittanceImage, g_skyTransmittanceMemory, g_skyTransmittanceView,
        "sky transmittance image create failed", "sky transmittance image allocate failed",
        "sky transmittance view create failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_skyAtmosphereSampler),
        "sky atmosphere sampler create failed");

    VkDescriptorSetLayoutBinding bindings[8]{};
    bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[3] = { 3u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[4] = { 4u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[5] = { 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[6] = { 6u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[7] = { 7u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    static_assert(
        sizeof(bindings) / sizeof(bindings[0]) == 8u,
        "sky atmosphere set layout: update this assert with bindings[]"
    );
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 8u;
    layoutInfo.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
        &g_skyAtmosphereSetLayout), "sky atmosphere set layout failed");

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2u },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5u }
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_skyAtmosphereDescriptorPool), "sky atmosphere descriptor pool failed");

    VkDescriptorSetAllocateInfo setAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAllocation.descriptorPool = g_skyAtmosphereDescriptorPool;
    setAllocation.descriptorSetCount = 1u;
    setAllocation.pSetLayouts = &g_skyAtmosphereSetLayout;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_skyAtmosphereDescriptorSet),
        "sky atmosphere descriptor set allocation failed");

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    createBrunetonResources();

    VkDescriptorImageInfo scatteringInfo{};
    scatteringInfo.imageView = g_skyScatteringView;
    scatteringInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo transmittanceInfo{};
    transmittanceInfo.imageView = g_skyTransmittanceView;
    transmittanceInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo brunetonInfos[3]{};
    const VkImageView brunetonViews[3] = {
        g_brTransmittance.sampledView, g_brScattering.sampledView,
        g_brSingleMie.sampledView };
    for(int i = 0; i < 3; ++i)
    {
        brunetonInfos[i].sampler = g_brLutSampler;
        brunetonInfos[i].imageView = brunetonViews[i];
        brunetonInfos[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkDescriptorImageInfo starMapInfo{};
    starMapInfo.imageView = g_starMapTextureView != VK_NULL_HANDLE
        ? g_starMapTextureView : g_atlasImageView;
    starMapInfo.sampler = g_starMapTextureSampler != VK_NULL_HANDLE
        ? g_starMapTextureSampler : g_atlasSampler;
    starMapInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo endSkyAtlasInfo{};
    endSkyAtlasInfo.imageView = g_endSkyAtlasView != VK_NULL_HANDLE
        ? g_endSkyAtlasView : g_atlasImageView;
    endSkyAtlasInfo.sampler = g_endSkyAtlasSampler != VK_NULL_HANDLE
        ? g_endSkyAtlasSampler : g_atlasSampler;
    endSkyAtlasInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[8] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_skyAtmosphereDescriptorSet, 0u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_skyAtmosphereDescriptorSet, 1u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &scatteringInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_skyAtmosphereDescriptorSet, 2u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &transmittanceInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_skyAtmosphereDescriptorSet, 3u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &brunetonInfos[0], nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_skyAtmosphereDescriptorSet, 4u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &brunetonInfos[1], nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_skyAtmosphereDescriptorSet, 5u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &brunetonInfos[2], nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_skyAtmosphereDescriptorSet, 6u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &starMapInfo, nullptr, nullptr },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_skyAtmosphereDescriptorSet, 7u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &endSkyAtlasInfo, nullptr, nullptr }
    };
    vkUpdateDescriptorSets(g_device, 8u, writes, 0u, nullptr);

    updateSkyRayDescriptors();
    RTLog("[SKY] equirectangular scattering/transmittance targets created (256x128)");
    if(!g_brPrecomputed)
    {
        executeBrunetonPrecompute();
    }
}
void createSkyAtmospherePipeline()
{
    if(g_skyAtmospherePipeline != VK_NULL_HANDLE)
    {
        return;
    }
    createSkyAtmosphereResources();
    const std::vector<uint8_t> code = loadSpv("shaders/sky_atmosphere.comp.spv");
    VkShaderModule shader = createShaderModule(g_device, code);
    try
    {
        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        layoutInfo.setLayoutCount = 1u;
        layoutInfo.pSetLayouts = &g_skyAtmosphereSetLayout;
        check(vkCreatePipelineLayout(g_device, &layoutInfo, nullptr,
            &g_skyAtmospherePipelineLayout), "sky atmosphere pipeline layout failed");
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
        pipelineInfo.layout = g_skyAtmospherePipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u, &pipelineInfo,
            nullptr, &g_skyAtmospherePipeline), "sky atmosphere pipeline failed");
    }
    catch(...)
    {
        vkDestroyShaderModule(g_device, shader, nullptr);
        destroySkyAtmosphereResources();
        throw;
    }
    vkDestroyShaderModule(g_device, shader, nullptr);
    updateSkyRayDescriptors();
    RTLog("[SKY] atmosphere single-scattering pipeline created");
}
void recordSkyAtmosphere(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE || g_skyAtmospherePipeline == VK_NULL_HANDLE)
    {
        return;
    }

    VkImageMemoryBarrier barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].oldLayout = g_skyScatteringLayout;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].srcAccessMask = g_skyScatteringLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : VK_ACCESS_SHADER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barriers[0].image = g_skyScatteringImage;
    barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barriers[0].subresourceRange.levelCount = 1u;
    barriers[0].subresourceRange.layerCount = 1u;
    barriers[1] = barriers[0];
    barriers[1].oldLayout = g_skyTransmittanceLayout;
    barriers[1].srcAccessMask = g_skyTransmittanceLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : VK_ACCESS_SHADER_READ_BIT;
    barriers[1].image = g_skyTransmittanceImage;
    vkCmdPipelineBarrier(cmd,
        g_skyScatteringLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            g_skyTransmittanceLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, barriers);
    g_skyScatteringLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_skyTransmittanceLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_skyAtmospherePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_skyAtmospherePipelineLayout, 0u, 1u, &g_skyAtmosphereDescriptorSet,
        0u, nullptr);
    vkCmdDispatch(cmd, (SKY_SCATTER_RESOLUTION_X + 7u) / 8u,
        (SKY_SCATTER_RESOLUTION_Y + 7u) / 8u, 1u);

    for(uint32_t i = 0u; i < 2u; ++i)
    {
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0u, 0u, nullptr, 0u, nullptr, 2u, barriers);
}
void destroySkyAtmosphereResources()
{
    if(g_skyAtmospherePipeline) vkDestroyPipeline(g_device, g_skyAtmospherePipeline, nullptr);
    if(g_skyAtmospherePipelineLayout) vkDestroyPipelineLayout(g_device, g_skyAtmospherePipelineLayout, nullptr);
    if(g_skyAtmosphereDescriptorPool) vkDestroyDescriptorPool(g_device, g_skyAtmosphereDescriptorPool, nullptr);
    if(g_skyAtmosphereSetLayout) vkDestroyDescriptorSetLayout(g_device, g_skyAtmosphereSetLayout, nullptr);
    if(g_skyAtmosphereSampler) vkDestroySampler(g_device, g_skyAtmosphereSampler, nullptr);
    if(g_skyScatteringView) vkDestroyImageView(g_device, g_skyScatteringView, nullptr);
    if(g_skyScatteringImage) vkDestroyImage(g_device, g_skyScatteringImage, nullptr);
    if(g_skyScatteringMemory) vkFreeMemory(g_device, g_skyScatteringMemory, nullptr);
    if(g_skyTransmittanceView) vkDestroyImageView(g_device, g_skyTransmittanceView, nullptr);
    if(g_skyTransmittanceImage) vkDestroyImage(g_device, g_skyTransmittanceImage, nullptr);
    if(g_skyTransmittanceMemory) vkFreeMemory(g_device, g_skyTransmittanceMemory, nullptr);
    g_skyAtmospherePipeline = VK_NULL_HANDLE;
    g_skyAtmospherePipelineLayout = VK_NULL_HANDLE;
    g_skyAtmosphereDescriptorPool = VK_NULL_HANDLE;
    g_skyAtmosphereSetLayout = VK_NULL_HANDLE;
    g_skyAtmosphereDescriptorSet = VK_NULL_HANDLE;
    g_skyAtmosphereSampler = VK_NULL_HANDLE;
    g_skyScatteringView = VK_NULL_HANDLE;
    g_skyScatteringImage = VK_NULL_HANDLE;
    g_skyScatteringMemory = VK_NULL_HANDLE;
    g_skyTransmittanceView = VK_NULL_HANDLE;
    g_skyTransmittanceImage = VK_NULL_HANDLE;
    g_skyTransmittanceMemory = VK_NULL_HANDLE;
    g_skyScatteringLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_skyTransmittanceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateStarMapTexture(
    JNIEnv*,
    jclass,
    jlong imageView,
    jlong sampler
)
{
    if(imageView == 0 || sampler == 0)
    {
        return;
    }
    const VkImageView newView = reinterpret_cast<VkImageView>(imageView);
    const VkSampler newSampler = reinterpret_cast<VkSampler>(sampler);
    if(g_starMapTextureView != newView || g_starMapTextureSampler != newSampler)
    {
        g_starMapTextureView = newView;
        g_starMapTextureSampler = newSampler;
        g_starMapTextureDirty = true;
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateEndSkyAtlasTexture(
    JNIEnv*,
    jclass,
    jlong imageView,
    jlong sampler
)
{
    if(imageView == 0 || sampler == 0)
    {
        return;
    }
    const VkImageView newView = reinterpret_cast<VkImageView>(imageView);
    const VkSampler newSampler = reinterpret_cast<VkSampler>(sampler);
    if(g_endSkyAtlasView != newView || g_endSkyAtlasSampler != newSampler)
    {
        g_endSkyAtlasView = newView;
        g_endSkyAtlasSampler = newSampler;
        g_endSkyAtlasDirty = true;
    }
}
