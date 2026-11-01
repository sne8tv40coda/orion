// Orion backend — Clouds 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// -------------------------------------------------------------------------
// Tyndall cloud-noise resources
// -------------------------------------------------------------------------
// The reference cloud implementations sample a persistent shape/detail field
// and a decorrelating curl field.  Generate both once, upload once, and never
// touch them again during normal rendering or resource-pack reloads.
uint32_t cloudNoiseHash(uint32_t value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}
float cloudNoiseUnit(uint32_t x, uint32_t y, uint32_t z, uint32_t seed)
{
    uint32_t value = x * 0x8da6b343u ^ y * 0xd8163841u ^
                     z * 0xcb1ab31fu ^ seed;
    return static_cast<float>(cloudNoiseHash(value) & 0x00ffffffu) /
        static_cast<float>(0x01000000u);
}
float cloudNoiseFade(float value)
{
    return value * value * (3.0f - 2.0f * value);
}
float cloudNoiseLerp(float a, float b, float t)
{
    return a + (b - a) * t;
}
uint32_t cloudNoiseWrap(int value, int period)
{
    value %= period;
    return static_cast<uint32_t>(value < 0 ? value + period : value);
}

// A small periodic value-noise basis is enough here: the GPU's trilinear
// sampler handles interpolation between uploaded texels, while this produces
// coherent cloud masses instead of filtering independent white-noise voxels.
float cloudValueNoise(float x, float y, float z, int period, uint32_t seed)
{
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const float tx = cloudNoiseFade(x - static_cast<float>(x0));
    const float ty = cloudNoiseFade(y - static_cast<float>(y0));
    const float tz = cloudNoiseFade(z - static_cast<float>(z0));
    const auto sample = [period, seed](int sx, int sy, int sz)
    {
        return cloudNoiseUnit(cloudNoiseWrap(sx, period), cloudNoiseWrap(sy, period),
            cloudNoiseWrap(sz, period), seed);
    };
    const float x00 = cloudNoiseLerp(sample(x0, y0, z0), sample(x0 + 1, y0, z0), tx);
    const float x10 = cloudNoiseLerp(sample(x0, y0 + 1, z0), sample(x0 + 1, y0 + 1, z0), tx);
    const float x01 = cloudNoiseLerp(sample(x0, y0, z0 + 1), sample(x0 + 1, y0, z0 + 1), tx);
    const float x11 = cloudNoiseLerp(sample(x0, y0 + 1, z0 + 1), sample(x0 + 1, y0 + 1, z0 + 1), tx);
    return cloudNoiseLerp(cloudNoiseLerp(x00, x10, ty), cloudNoiseLerp(x01, x11, ty), tz);
}
float cloudFbmNoise(float x, float y, float z, uint32_t seed)
{
    return 0.50f * cloudValueNoise(x * 4.0f, y * 4.0f, z * 4.0f, 4, seed) +
           0.30f * cloudValueNoise(x * 9.0f, y * 9.0f, z * 9.0f, 9, seed ^ 0x9e3779b9u) +
           0.20f * cloudValueNoise(x * 19.0f, y * 19.0f, z * 19.0f, 19, seed ^ 0x85ebca6bu);
}
float cloudCellularNoise(float x, float y, float z, int period, uint32_t seed)
{
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const int iz = static_cast<int>(std::floor(z));
    const float fx = x - static_cast<float>(ix);
    const float fy = y - static_cast<float>(iy);
    const float fz = z - static_cast<float>(iz);
    float nearest = 3.0f;
    for(int dz = -1; dz <= 1; ++dz)
    for(int dy = -1; dy <= 1; ++dy)
    for(int dx = -1; dx <= 1; ++dx)
    {
        const int cx = ix + dx, cy = iy + dy, cz = iz + dz;
        const uint32_t wx = cloudNoiseWrap(cx, period);
        const uint32_t wy = cloudNoiseWrap(cy, period);
        const uint32_t wz = cloudNoiseWrap(cz, period);
        const float px = cloudNoiseUnit(wx, wy, wz, seed);
        const float py = cloudNoiseUnit(wx, wy, wz, seed ^ 0x68bc21ebu);
        const float pz = cloudNoiseUnit(wx, wy, wz, seed ^ 0x02e5be93u);
        const float ox = static_cast<float>(dx) + px - fx;
        const float oy = static_cast<float>(dy) + py - fy;
        const float oz = static_cast<float>(dz) + pz - fz;
        nearest = std::min(nearest, ox * ox + oy * oy + oz * oz);
    }
    return std::max(0.0f, 1.0f - std::sqrt(nearest) * 0.577350269f);
}
uint8_t cloudNoiseByte(float value)
{
    const float clamped = std::max(0.0f, std::min(1.0f, value));
    return static_cast<uint8_t>(clamped * 255.0f + 0.5f);
}
void updateCloudNoiseRayDescriptors()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_cloudShapeNoiseView == VK_NULL_HANDLE ||
       g_cloudShapeNoiseSampler == VK_NULL_HANDLE ||
       g_cloudCurlNoiseView == VK_NULL_HANDLE ||
       g_cloudCurlNoiseSampler == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorImageInfo infos[2]{};
    const bool useV4Shape = (g_cloudV4ShapeView != VK_NULL_HANDLE &&
        g_cloudV4ShapeSampler != VK_NULL_HANDLE);
    infos[0].sampler = useV4Shape ? g_cloudV4ShapeSampler : g_cloudShapeNoiseSampler;
    infos[0].imageView = useV4Shape ? g_cloudV4ShapeView : g_cloudShapeNoiseView;
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].sampler = g_cloudCurlNoiseSampler;
    infos[1].imageView = g_cloudCurlNoiseView;
    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_rayTracingDescriptorSet;
        writes[i].dstBinding = 38u + i;
        writes[i].descriptorCount = 1u;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);
}
void createCloudNoiseResources()
{
    if(g_cloudShapeNoiseImage != VK_NULL_HANDLE)
    {
        return;
    }

    const size_t shapeTexelCount = static_cast<size_t>(CLOUD_SHAPE_NOISE_RESOLUTION) *
        CLOUD_SHAPE_NOISE_RESOLUTION * CLOUD_SHAPE_NOISE_RESOLUTION;
    const size_t curlTexelCount = static_cast<size_t>(CLOUD_CURL_NOISE_RESOLUTION) *
        CLOUD_CURL_NOISE_RESOLUTION * CLOUD_CURL_NOISE_RESOLUTION;
    std::vector<uint8_t> shapePixels(shapeTexelCount * 4u);
    std::vector<uint8_t> curlPixels(curlTexelCount * 4u);

    for(uint32_t z = 0u; z < CLOUD_SHAPE_NOISE_RESOLUTION; ++z)
    for(uint32_t y = 0u; y < CLOUD_SHAPE_NOISE_RESOLUTION; ++y)
    for(uint32_t x = 0u; x < CLOUD_SHAPE_NOISE_RESOLUTION; ++x)
    {
        const size_t index = (static_cast<size_t>(z) * CLOUD_SHAPE_NOISE_RESOLUTION *
            CLOUD_SHAPE_NOISE_RESOLUTION + static_cast<size_t>(y) *
            CLOUD_SHAPE_NOISE_RESOLUTION + x) * 4u;
        const float fx = static_cast<float>(x) / static_cast<float>(CLOUD_SHAPE_NOISE_RESOLUTION);
        const float fy = static_cast<float>(y) / static_cast<float>(CLOUD_SHAPE_NOISE_RESOLUTION);
        const float fz = static_cast<float>(z) / static_cast<float>(CLOUD_SHAPE_NOISE_RESOLUTION);
        const float macro = cloudFbmNoise(fx, fy, fz, 0x13579bdfu);
        const float billow = 1.0f - std::abs(2.0f * cloudFbmNoise(fx + 0.37f, fy + 0.11f, fz + 0.71f, 0x2468ace1u) - 1.0f);
        shapePixels[index + 0u] = cloudNoiseByte(macro);
        shapePixels[index + 1u] = cloudNoiseByte(0.55f * macro + 0.45f * billow);
        shapePixels[index + 2u] = cloudNoiseByte(cloudCellularNoise(fx * 7.0f, fy * 7.0f, fz * 7.0f, 7, 0x9e3779b9u));
        shapePixels[index + 3u] = cloudNoiseByte(cloudCellularNoise(fx * 15.0f, fy * 15.0f, fz * 15.0f, 15, 0xa341316cu));
    }

    {
        const std::string shapeOverridePath =
            ResolveRTXRuntimePath("cloud_noise_shape_64.rgba8");
        if(FILE* fp = std::fopen(shapeOverridePath.empty() ? "cloud_noise_shape_64.rgba8"
            : shapeOverridePath.c_str(), "rb"))
        {
            std::vector<uint8_t> ref(shapeTexelCount * 4u);
            const size_t got = std::fread(ref.data(), 1u, ref.size(), fp);
            std::fclose(fp);

            if(got == ref.size())
            {
                for(size_t i = 0u; i < shapeTexelCount; ++i)
                {
                    shapePixels[i * 4u + 0u] = ref[i * 4u + 0u];
                    shapePixels[i * 4u + 1u] = ref[i * 4u + 1u];
                }

                std::fprintf(stderr,
                    "[CLOUD NOISE] shape R/G <- run/cloud_noise_shape_64.rgba8 (%zu texels)\n",
                    shapeTexelCount);
                std::fflush(stderr);
            }
            else
            {
                std::fprintf(stderr,
                    "[CLOUD NOISE] reference shape noise size mismatch (%zu vs %zu), keeping procedural\n",
                    got, ref.size());
                std::fflush(stderr);
            }
        }
    }

    const uint32_t n = CLOUD_CURL_NOISE_RESOLUTION;
    for(uint32_t z = 0u; z < n; ++z)
    for(uint32_t y = 0u; y < n; ++y)
    for(uint32_t x = 0u; x < n; ++x)
    {
        const uint32_t xp = (x + 1u) % n, xm = (x + n - 1u) % n;
        const uint32_t yp = (y + 1u) % n, ym = (y + n - 1u) % n;
        const uint32_t zp = (z + 1u) % n, zm = (z + n - 1u) % n;
        const uint32_t sx = 0x7f4a7c15u, sy = 0x6a09e667u, sz = 0xbb67ae85u;
        const auto potential = [n](uint32_t px, uint32_t py, uint32_t pz, uint32_t seed)
        {
            return cloudValueNoise(static_cast<float>(px) * 5.0f / static_cast<float>(n),
                static_cast<float>(py) * 5.0f / static_cast<float>(n),
                static_cast<float>(pz) * 5.0f / static_cast<float>(n), 5, seed);
        };
        const float dFzDy = potential(x, yp, z, sz) - potential(x, ym, z, sz);
        const float dFyDz = potential(x, y, zp, sy) - potential(x, y, zm, sy);
        const float dFxDz = potential(x, y, zp, sx) - potential(x, y, zm, sx);
        const float dFzDx = potential(xp, y, z, sz) - potential(xm, y, z, sz);
        const float dFyDx = potential(xp, y, z, sy) - potential(xm, y, z, sy);
        const float dFxDy = potential(x, yp, z, sx) - potential(x, ym, z, sx);
        float curlX = dFzDy - dFyDz;
        float curlY = dFxDz - dFzDx;
        float curlZ = dFyDx - dFxDy;
        const float length = std::sqrt(curlX * curlX + curlY * curlY + curlZ * curlZ);
        if(length > 1.0e-6f)
        {
            curlX /= length;
            curlY /= length;
            curlZ /= length;
        }
        const size_t index = (static_cast<size_t>(z) * n * n +
            static_cast<size_t>(y) * n + x) * 4u;
        curlPixels[index + 0u] = cloudNoiseByte(curlX * 0.5f + 0.5f);
        curlPixels[index + 1u] = cloudNoiseByte(curlY * 0.5f + 0.5f);
        curlPixels[index + 2u] = cloudNoiseByte(curlZ * 0.5f + 0.5f);
        curlPixels[index + 3u] = 255u;
    }

    {
        const std::string curlOverridePath =
            ResolveRTXRuntimePath("cloud_noise_curl_64.rgba8");
        if(FILE* fp = std::fopen(curlOverridePath.empty() ? "cloud_noise_curl_64.rgba8"
            : curlOverridePath.c_str(), "rb"))
        {
            std::vector<uint8_t> ref(curlTexelCount * 4u);
            const size_t got = std::fread(ref.data(), 1u, ref.size(), fp);
            std::fclose(fp);

            if(got == ref.size())
            {
                for(size_t i = 0u; i < curlTexelCount; ++i)
                {
                    curlPixels[i * 4u + 0u] = ref[i * 4u + 0u];
                    curlPixels[i * 4u + 1u] = ref[i * 4u + 1u];
                    curlPixels[i * 4u + 2u] = ref[i * 4u + 2u];
                }

                std::fprintf(stderr,
                    "[CLOUD NOISE] curl RGB <- run/cloud_noise_curl_64.rgba8 (%zu texels)\n",
                    curlTexelCount);
                std::fflush(stderr);
            }
        }
    }

    const auto buildMipChain = [](const std::vector<uint8_t>& base, uint32_t extent,
        std::vector<size_t>& levelOffsets, std::vector<size_t>& levelSizes)
    {
        std::vector<uint8_t> chain;
        std::vector<uint8_t> current = base;
        for(uint32_t dim = extent;;)
        {
            const size_t bytes = static_cast<size_t>(dim) * dim * dim * 4u;
            levelOffsets.push_back(chain.size());
            levelSizes.push_back(bytes);
            chain.insert(chain.end(), current.begin(), current.end());
            if(dim <= 1u)
            {
                break;
            }
            const uint32_t next = dim / 2u;
            std::vector<uint8_t> reduced(static_cast<size_t>(next) * next * next * 4u);
            for(uint32_t z = 0u; z < next; ++z)
            for(uint32_t y = 0u; y < next; ++y)
            for(uint32_t x = 0u; x < next; ++x)
            {
                uint32_t accumulator[4] = {0u, 0u, 0u, 0u};
                for(uint32_t dz = 0u; dz < 2u; ++dz)
                for(uint32_t dy = 0u; dy < 2u; ++dy)
                for(uint32_t dx = 0u; dx < 2u; ++dx)
                {
                    const size_t sourceIndex = ((static_cast<size_t>(2u * z + dz) * dim +
                        (2u * y + dy)) * dim + (2u * x + dx)) * 4u;
                    for(uint32_t channel = 0u; channel < 4u; ++channel)
                    {
                        accumulator[channel] += current[sourceIndex + channel];
                    }
                }
                const size_t destinationIndex = ((static_cast<size_t>(z) * next + y) * next + x) * 4u;
                for(uint32_t channel = 0u; channel < 4u; ++channel)
                {
                    reduced[destinationIndex + channel] =
                        static_cast<uint8_t>((accumulator[channel] + 4u) / 8u);
                }
            }
            current.swap(reduced);
            dim = next;
        }
        return chain;
    };

    std::vector<size_t> shapeMipOffsets, shapeMipSizes, curlMipOffsets, curlMipSizes;
    const std::vector<uint8_t> shapeChain = buildMipChain(
        shapePixels, CLOUD_SHAPE_NOISE_RESOLUTION, shapeMipOffsets, shapeMipSizes);
    const std::vector<uint8_t> curlChain = buildMipChain(
        curlPixels, CLOUD_CURL_NOISE_RESOLUTION, curlMipOffsets, curlMipSizes);
    const uint32_t shapeMipLevels = static_cast<uint32_t>(shapeMipSizes.size());
    const uint32_t curlMipLevels = static_cast<uint32_t>(curlMipSizes.size());

    const VkDeviceSize shapeBytes = static_cast<VkDeviceSize>(shapeChain.size());
    const VkDeviceSize curlBytes = static_cast<VkDeviceSize>(curlChain.size());
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(g_device, g_physicalDevice, shapeBytes + curlBytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* mapped = nullptr;
    check(vkMapMemory(g_device, stagingMemory, 0u, shapeBytes + curlBytes, 0u, &mapped),
        "cloud noise staging map failed");
    std::memcpy(mapped, shapeChain.data(), shapeChain.size());
    std::memcpy(static_cast<uint8_t*>(mapped) + shapeBytes, curlChain.data(), curlChain.size());
    vkUnmapMemory(g_device, stagingMemory);

    auto createVolume = [](uint32_t extent, uint32_t mipLevels, VkImage& image, VkDeviceMemory& memory,
                           const char* createError, const char* allocateError, const char* bindError)
    {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_3D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {extent, extent, extent};
        info.mipLevels = mipLevels;
        info.arrayLayers = 1u;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(g_device, &info, nullptr, &image), createError);
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(g_device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr, &memory), allocateError);
        check(vkBindImageMemory(g_device, image, memory, 0u), bindError);
    };
    createVolume(CLOUD_SHAPE_NOISE_RESOLUTION, shapeMipLevels, g_cloudShapeNoiseImage,
        g_cloudShapeNoiseMemory, "cloud shape image create failed",
        "cloud shape image allocate failed", "cloud shape image bind failed");
    createVolume(CLOUD_CURL_NOISE_RESOLUTION, curlMipLevels, g_cloudCurlNoiseImage,
        g_cloudCurlNoiseMemory, "cloud curl image create failed",
        "cloud curl image allocate failed", "cloud curl image bind failed");

    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = g_rtCommandPool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &allocation, &commandBuffer),
        "cloud noise command allocation failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "cloud noise command begin failed");

    VkImageMemoryBarrier barriers[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = i == 0u ? g_cloudShapeNoiseImage : g_cloudCurlNoiseImage;
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.levelCount = i == 0u ? shapeMipLevels : curlMipLevels;
        barriers[i].subresourceRange.layerCount = 1u;
        barriers[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, barriers);

    std::vector<VkBufferImageCopy> copies;
    copies.reserve(shapeMipSizes.size() + curlMipSizes.size());
    for(size_t level = 0u; level < shapeMipSizes.size(); ++level)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = shapeMipOffsets[level];
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = static_cast<uint32_t>(level);
        copy.imageSubresource.layerCount = 1u;
        const uint32_t extent = std::max(1u, CLOUD_SHAPE_NOISE_RESOLUTION >> static_cast<uint32_t>(level));
        copy.imageExtent = {extent, extent, extent};
        copies.push_back(copy);
    }
    for(size_t level = 0u; level < curlMipSizes.size(); ++level)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = shapeBytes + curlMipOffsets[level];
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = static_cast<uint32_t>(level);
        copy.imageSubresource.layerCount = 1u;
        const uint32_t extent = std::max(1u, CLOUD_CURL_NOISE_RESOLUTION >> static_cast<uint32_t>(level));
        copy.imageExtent = {extent, extent, extent};
        copies.push_back(copy);
    }
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_cloudShapeNoiseImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, shapeMipLevels, copies.data());
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_cloudCurlNoiseImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, curlMipLevels, copies.data() + shapeMipLevels);
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 2u, barriers);
    check(vkEndCommandBuffer(commandBuffer), "cloud noise command end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "cloud noise submit failed");
    check(vkQueueWaitIdle(g_queue), "cloud noise upload wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    vkDestroyBuffer(g_device, stagingBuffer, nullptr);
    vkFreeMemory(g_device, stagingMemory, nullptr);

    auto createViewAndSampler = [](VkImage image, uint32_t mipLevels, VkImageView& view, VkSampler& sampler,
                                   const char* viewError, const char* samplerError)
    {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.layerCount = 1u;
        check(vkCreateImageView(g_device, &viewInfo, nullptr, &view), viewError);
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(mipLevels - 1u);
        check(vkCreateSampler(g_device, &samplerInfo, nullptr, &sampler), samplerError);
    };
    createViewAndSampler(g_cloudShapeNoiseImage, shapeMipLevels, g_cloudShapeNoiseView,
        g_cloudShapeNoiseSampler, "cloud shape view create failed", "cloud shape sampler create failed");
    createViewAndSampler(g_cloudCurlNoiseImage, curlMipLevels, g_cloudCurlNoiseView,
        g_cloudCurlNoiseSampler, "cloud curl view create failed", "cloud curl sampler create failed");
    updateCloudNoiseRayDescriptors();
    RTLog("[CLOUD] Tyndall 128^3 shape/detail + 64^3 curl fields uploaded (mip-mapped, shape=" +
        std::to_string(shapeMipLevels) + " curl=" + std::to_string(curlMipLevels) + " levels)");
}

// -------------------------------------------------------------------------
// 体积云 v2：2D 基础覆盖噪声（binding 9）
//
// 参考 Clouds.fsh:97-100 用 noisetex 的 .g/.r 做 2D 底覆盖（两次频程、随风漂移），
// 并在 VC_LOCAL_COVERAGE 里用同一张图的 .x 当超低频局部覆盖图。Iris 的 noisetex
// 是 64² RG、REPEAT + 模糊；本引擎没有这个 uniform，这里生成一张统计等价的
// 64² RG8（.r 宽频周期 fBm / .g worley 胞元），采样方式与参考一致。
// -------------------------------------------------------------------------
void createCloudBaseNoiseResources()
{
    if(g_cloudBaseNoiseImage != VK_NULL_HANDLE)
    {
        return;
    }

    const uint32_t dim = CLOUD_BASE_NOISE_RESOLUTION;
    std::vector<uint8_t> base(static_cast<size_t>(dim) * dim * 4u);

    bool loadedReferenceNoise = false;
    const std::string baseNoisePath = ResolveRTXRuntimePath("cloud_base_noise_64.rgba8");
    if(FILE* baseNoiseFile = std::fopen(baseNoisePath.empty() ? "cloud_base_noise_64.rgba8"
        : baseNoisePath.c_str(), "rb"))
    {
        const size_t got = std::fread(base.data(), 1u, base.size(), baseNoiseFile);
        std::fclose(baseNoiseFile);
        loadedReferenceNoise = got == base.size();
        if(!loadedReferenceNoise)
        {
            std::fill(base.begin(), base.end(), 0u);
        }
    }
    if(loadedReferenceNoise)
    {
        RTLog("[CLOUD NOISE] base coverage noise = reference noisetex (64x64 RGBA8)");
    }
    else
    {
        RTLog("[CLOUD NOISE] base coverage noise = procedural fallback (reference asset missing)");
        for(uint32_t y = 0u; y < dim; ++y)
        for(uint32_t x = 0u; x < dim; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dim);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dim);
            const float broad = 0.50f * cloudValueNoise(u * 6.0f, v * 6.0f, 0.5f, 6, 0x1f123bb5u) +
                                0.30f * cloudValueNoise(u * 13.0f, v * 13.0f, 0.5f, 13, 0x2c1b3c6du) +
                                0.20f * cloudValueNoise(u * 27.0f, v * 27.0f, 0.5f, 27, 0x4a7c15f3u);
            const float worley = cloudCellularNoise(u * 8.0f, v * 8.0f, 0.5f, 8, 0x9e3779b9u);
            const size_t index = (static_cast<size_t>(y) * dim + x) * 4u;
            base[index + 0u] = cloudNoiseByte(broad);
            base[index + 1u] = cloudNoiseByte(worley);
            base[index + 2u] = cloudNoiseByte(0.5f * (broad + worley));
            base[index + 3u] = 255u;
        }
    }

    std::vector<size_t> levelOffsets;
    std::vector<size_t> levelSizes;
    std::vector<uint8_t> chain;
    std::vector<uint8_t> current = base;
    for(uint32_t width = dim;;)
    {
        const size_t bytes = static_cast<size_t>(width) * width * 4u;
        levelOffsets.push_back(chain.size());
        levelSizes.push_back(bytes);
        chain.insert(chain.end(), current.begin(), current.end());
        if(width <= 1u)
        {
            break;
        }
        const uint32_t next = width / 2u;
        std::vector<uint8_t> reduced(static_cast<size_t>(next) * next * 4u);
        for(uint32_t y = 0u; y < next; ++y)
        for(uint32_t x = 0u; x < next; ++x)
        {
            uint32_t accumulator[4] = {0u, 0u, 0u, 0u};
            for(uint32_t dy = 0u; dy < 2u; ++dy)
            for(uint32_t dx = 0u; dx < 2u; ++dx)
            {
                const uint32_t sx = (2u * x + dx) % width;
                const uint32_t sy = (2u * y + dy) % width;
                const size_t sourceIndex = (static_cast<size_t>(sy) * width + sx) * 4u;
                for(uint32_t channel = 0u; channel < 4u; ++channel)
                {
                    accumulator[channel] += current[sourceIndex + channel];
                }
            }
            const size_t destinationIndex = (static_cast<size_t>(y) * next + x) * 4u;
            for(uint32_t channel = 0u; channel < 4u; ++channel)
            {
                reduced[destinationIndex + channel] =
                    static_cast<uint8_t>((accumulator[channel] + 2u) / 4u);
            }
        }
        current.swap(reduced);
        width = next;
    }
    const uint32_t mipLevels = static_cast<uint32_t>(levelSizes.size());

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {dim, dim, 1u};
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_cloudBaseNoiseImage),
        "cloud base noise image create failed");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_cloudBaseNoiseImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_cloudBaseNoiseMemory),
        "cloud base noise image allocate failed");
    check(vkBindImageMemory(g_device, g_cloudBaseNoiseImage, g_cloudBaseNoiseMemory, 0u),
        "cloud base noise image bind failed");

    const VkDeviceSize chainBytes = static_cast<VkDeviceSize>(chain.size());
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(g_device, g_physicalDevice, chainBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* mapped = nullptr;
    check(vkMapMemory(g_device, stagingMemory, 0u, chainBytes, 0u, &mapped),
        "cloud base noise staging map failed");
    std::memcpy(mapped, chain.data(), chain.size());
    vkUnmapMemory(g_device, stagingMemory);

    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = g_rtCommandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &commandAllocation, &commandBuffer),
        "cloud base noise command allocation failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "cloud base noise command begin failed");

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_cloudBaseNoiseImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.layerCount = 1u;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    std::vector<VkBufferImageCopy> copies;
    copies.reserve(mipLevels);
    for(uint32_t level = 0u; level < mipLevels; ++level)
    {
        VkBufferImageCopy copy{};
        copy.bufferOffset = levelOffsets[level];
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = level;
        copy.imageSubresource.layerCount = 1u;
        const uint32_t extent = std::max(1u, dim >> level);
        copy.imageExtent = {extent, extent, 1u};
        copies.push_back(copy);
    }
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_cloudBaseNoiseImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, mipLevels, copies.data());
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    check(vkEndCommandBuffer(commandBuffer), "cloud base noise command end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "cloud base noise submit failed");
    check(vkQueueWaitIdle(g_queue), "cloud base noise upload wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    vkDestroyBuffer(g_device, stagingBuffer, nullptr);
    vkFreeMemory(g_device, stagingMemory, nullptr);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_cloudBaseNoiseImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudBaseNoiseView),
        "cloud base noise view create failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(mipLevels - 1u);
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudBaseNoiseSampler),
        "cloud base noise sampler create failed");

    RTLog("[CLOUD NOISE] 64x64 base coverage noise uploaded (" +
        std::to_string(mipLevels) + " levels)");
}
void destroyCloudBaseNoiseResources()
{
    if(g_cloudBaseNoiseSampler) vkDestroySampler(g_device, g_cloudBaseNoiseSampler, nullptr);
    if(g_cloudBaseNoiseView) vkDestroyImageView(g_device, g_cloudBaseNoiseView, nullptr);
    if(g_cloudBaseNoiseImage) vkDestroyImage(g_device, g_cloudBaseNoiseImage, nullptr);
    if(g_cloudBaseNoiseMemory) vkFreeMemory(g_device, g_cloudBaseNoiseMemory, nullptr);
    g_cloudBaseNoiseSampler = VK_NULL_HANDLE;
    g_cloudBaseNoiseView = VK_NULL_HANDLE;
    g_cloudBaseNoiseImage = VK_NULL_HANDLE;
    g_cloudBaseNoiseMemory = VK_NULL_HANDLE;
}

// -------------------------------------------------------------------------
// 体积云 v3的四张资产
//
// 与 v2 的资产**不是同一套语义**：这里的 128³ 形状体是
// r = Perlin-Worley、g/b/a = 三级 Worley FBM，
// 32³ 细节体是三级 Worley（:549-554），天气图 r=覆盖度/g=云型（:520-537）。
// 因此 v3 不能复用 v2 的 3D 纹理绑定，必须走独立视图。
// -------------------------------------------------------------------------
std::vector<uint8_t> readCloudV3Asset(const char* relativePath, size_t expectedBytes)
{
    std::vector<uint8_t> data;
    const std::string resolved = ResolveRTXRuntimePath(relativePath);
    FILE* file = std::fopen(resolved.empty() ? relativePath : resolved.c_str(), "rb");
    if(!file)
    {
        return data;
    }
    data.resize(expectedBytes);
    const size_t got = std::fread(data.data(), 1u, expectedBytes, file);
    std::fclose(file);
    if(got != expectedBytes)
    {
        data.clear();
    }
    return data;
}
VkSampler createCloudV3Sampler()
{
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &sampler),
        "cloud v3 sampler create failed");
    return sampler;
}
void uploadCloudV3Image(
    VkImageType imageType,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    const std::vector<uint8_t>& pixels,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkSampler& sampler)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = imageType;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {width, height, imageType == VK_IMAGE_TYPE_3D ? depth : 1u};
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &image), "cloud v3 image create failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &memory), "cloud v3 memory failed");
    check(vkBindImageMemory(g_device, image, memory, 0u), "cloud v3 bind failed");

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(g_device, g_physicalDevice, static_cast<VkDeviceSize>(pixels.size()),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* mapped = nullptr;
    check(vkMapMemory(g_device, stagingMemory, 0u, static_cast<VkDeviceSize>(pixels.size()), 0u, &mapped),
        "cloud v3 staging map failed");
    std::memcpy(mapped, pixels.data(), pixels.size());
    vkUnmapMemory(g_device, stagingMemory);

    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = g_rtCommandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &commandAllocation, &commandBuffer),
        "cloud v3 command failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "cloud v3 begin failed");

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1u;
    copy.imageExtent = imageInfo.extent;
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    check(vkEndCommandBuffer(commandBuffer), "cloud v3 end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "cloud v3 submit failed");
    check(vkQueueWaitIdle(g_queue), "cloud v3 upload wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    vkDestroyBuffer(g_device, stagingBuffer, nullptr);
    vkFreeMemory(g_device, stagingMemory, nullptr);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = imageType == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &view), "cloud v3 view failed");
    sampler = createCloudV3Sampler();
}
void createCloudV3Resources()
{
    if(g_cloudV3ShapeImage != VK_NULL_HANDLE)
    {
        return;
    }

    const std::vector<uint8_t> shape = readCloudV3Asset("cloud_v3_shape_128.rgba8",
        128u * 128u * 128u * 4u);
    const std::vector<uint8_t> detail = readCloudV3Asset("cloud_v3_detail_32.rgba8",
        32u * 32u * 32u * 4u);
    const std::vector<uint8_t> curl = readCloudV3Asset("cloud_v3_curl_128.rgba8",
        128u * 128u * 4u);
    const std::vector<uint8_t> weather = readCloudV3Asset("cloud_v3_weather_512.rgba8",
        512u * 512u * 4u);

    if(shape.empty() || detail.empty() || curl.empty() || weather.empty())
    {
        RTLog("[CLOUD V3] v3 assets missing -> v3 unavailable "
            "(run tools/make_cloud_v3_assets.py)");
        return;
    }

    uploadCloudV3Image(VK_IMAGE_TYPE_3D, 128u, 128u, 128u, shape,
        g_cloudV3ShapeImage, g_cloudV3ShapeMemory, g_cloudV3ShapeView, g_cloudV3ShapeSampler);
    uploadCloudV3Image(VK_IMAGE_TYPE_3D, 32u, 32u, 32u, detail,
        g_cloudV3DetailImage, g_cloudV3DetailMemory, g_cloudV3DetailView, g_cloudV3DetailSampler);
    uploadCloudV3Image(VK_IMAGE_TYPE_2D, 128u, 128u, 1u, curl,
        g_cloudV3CurlImage, g_cloudV3CurlMemory, g_cloudV3CurlView, g_cloudV3CurlSampler);
    uploadCloudV3Image(VK_IMAGE_TYPE_2D, 512u, 512u, 1u, weather,
        g_cloudV3WeatherImage, g_cloudV3WeatherMemory, g_cloudV3WeatherView, g_cloudV3WeatherSampler);

    RTLog("[CLOUD V3] v3 assets uploaded (128^3 shape / 32^3 detail / 128^2 curl / 512^2 weather)");
}

// -------------------------------------------------------------------------
// 体积云 v4 资源：建两张图 + 用 cloud_bevy_noise.comp 烘焙一次
// -------------------------------------------------------------------------
// 2026-10-18 诊断：C++ 的 stdout/stderr 不进 latest.log，只能写文件。
// CWD 是 run/，所以这个文件出现在 run/cloud_v4_debug.log。
void cloudV4DebugLog(const char* text)
{
    if(FILE* file = std::fopen("cloud_v4_debug.log", "a"))
    {
        std::fprintf(file, "%s\n", text);
        std::fclose(file);
    }
}

// 体积云模型选择（2026-10-25）：缺省 = v5（Schneider 重写版）；
// 环境变量 ORION_CLOUD_MODEL=ue 回退 v4 的 UE 风格实现（渲染与烘焙都回）。
// 只在进程启动时读一次；运行期切换不支持（烘焙资产不同）。
int cloudModelRequested()
{
    return 0;
}
void createCloudV4Resources()
{
    {
        static const bool skipCloudV4 = [] {
            std::ifstream marker(ResolveRTXRuntimePath("skip_cloud_v4.flag"));
            return marker.good();
        }();
        if(skipCloudV4)
        {
            cloudV4DebugLog("SKIPPED by skip_cloud_v4.flag");
            return;
        }
    }
    cloudV4DebugLog("enter createCloudV4Resources");
    if(g_cloudV4AtlasImage != VK_NULL_HANDLE)
    {
        return;
    }

    VkImageCreateInfo atlasInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    atlasInfo.imageType = VK_IMAGE_TYPE_2D;
    atlasInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    atlasInfo.extent = {CLOUD_V4_ATLAS_SIZE, CLOUD_V4_ATLAS_SIZE, 1u};
    atlasInfo.mipLevels = 1u;
    atlasInfo.arrayLayers = 1u;
    atlasInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    atlasInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    atlasInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    atlasInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &atlasInfo, nullptr, &g_cloudV4AtlasImage),
        "cloud v4 atlas create failed");

    VkImageCreateInfo worleyInfo = atlasInfo;
    worleyInfo.imageType = VK_IMAGE_TYPE_3D;
    worleyInfo.extent = {CLOUD_V4_WORLEY_SIZE, CLOUD_V4_WORLEY_SIZE, CLOUD_V4_WORLEY_SIZE};
    worleyInfo.mipLevels = CLOUD_V4_WORLEY_MIPS;
    worleyInfo.usage = atlasInfo.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    check(vkCreateImage(g_device, &worleyInfo, nullptr, &g_cloudV4WorleyImage),
        "cloud v4 worley create failed");

    VkImageCreateInfo baseInfo = atlasInfo;
    baseInfo.imageType = VK_IMAGE_TYPE_3D;
    baseInfo.extent = {CLOUD_V4_BASE_SIZE, CLOUD_V4_BASE_SIZE, CLOUD_V4_BASE_SIZE};
    baseInfo.mipLevels = CLOUD_V4_BASE_MIPS;
    baseInfo.usage = atlasInfo.usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    check(vkCreateImage(g_device, &baseInfo, nullptr, &g_cloudV4BaseImage),
        "cloud v4 base volume create failed");

    VkImageCreateInfo shapeInfo = atlasInfo;
    shapeInfo.imageType = VK_IMAGE_TYPE_3D;
    shapeInfo.extent = {CLOUD_V4_SHAPE_SIZE, CLOUD_V4_SHAPE_SIZE, CLOUD_V4_SHAPE_SIZE};
    check(vkCreateImage(g_device, &shapeInfo, nullptr, &g_cloudV4ShapeImage),
        "cloud v4 shape volume create failed");

    auto allocate = [](VkImage image, VkDeviceMemory& memory)
    {
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(g_device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = findMemoryType(g_physicalDevice, requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(g_device, &allocation, nullptr, &memory),
            "cloud v4 memory failed");
        check(vkBindImageMemory(g_device, image, memory, 0u), "cloud v4 bind failed");
    };
    allocate(g_cloudV4AtlasImage, g_cloudV4AtlasMemory);
    allocate(g_cloudV4WorleyImage, g_cloudV4WorleyMemory);
    allocate(g_cloudV4BaseImage, g_cloudV4BaseMemory);
    allocate(g_cloudV4ShapeImage, g_cloudV4ShapeMemory);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    viewInfo.image = g_cloudV4AtlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudV4AtlasView),
        "cloud v4 atlas view failed");
    viewInfo.image = g_cloudV4WorleyImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.subresourceRange.levelCount = CLOUD_V4_WORLEY_MIPS;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudV4WorleyView),
        "cloud v4 worley view failed");
    viewInfo.image = g_cloudV4BaseImage;
    viewInfo.subresourceRange.levelCount = CLOUD_V4_BASE_MIPS;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudV4BaseView),
        "cloud v4 base volume view failed");
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.image = g_cloudV4ShapeImage;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_cloudV4ShapeView),
        "cloud v4 shape volume view failed");
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudV4AtlasSampler),
        "cloud v4 atlas sampler failed");
    samplerInfo.maxLod = static_cast<float>(CLOUD_V4_WORLEY_MIPS - 1u);
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudV4WorleySampler),
        "cloud v4 worley sampler failed");
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudV4BaseSampler),
        "cloud v4 base sampler failed");
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_cloudV4ShapeSampler),
        "cloud v4 shape sampler failed");

    VkDescriptorSetLayoutBinding bakeBindings[4]{};
    bakeBindings[0] = {0u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bakeBindings[1] = {1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bakeBindings[2] = {2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bakeBindings[3] = {3u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo bakeLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    bakeLayoutInfo.bindingCount = 4u;
    bakeLayoutInfo.pBindings = bakeBindings;
    VkDescriptorSetLayout bakeSetLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(g_device, &bakeLayoutInfo, nullptr, &bakeSetLayout),
        "cloud v4 bake layout failed");

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = 16u;
    VkPipelineLayoutCreateInfo bakePipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    bakePipelineLayoutInfo.setLayoutCount = 1u;
    bakePipelineLayoutInfo.pSetLayouts = &bakeSetLayout;
    bakePipelineLayoutInfo.pushConstantRangeCount = 1u;
    bakePipelineLayoutInfo.pPushConstantRanges = &range;
    VkPipelineLayout bakePipelineLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(g_device, &bakePipelineLayoutInfo, nullptr, &bakePipelineLayout),
        "cloud v4 bake pipeline layout failed");

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4u};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 1u;
    poolInfo.pPoolSizes = &poolSize;
    VkDescriptorPool bakePool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &bakePool),
        "cloud v4 bake pool failed");

    VkDescriptorSetAllocateInfo bakeAlloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    bakeAlloc.descriptorPool = bakePool;
    bakeAlloc.descriptorSetCount = 1u;
    bakeAlloc.pSetLayouts = &bakeSetLayout;
    VkDescriptorSet bakeSet = VK_NULL_HANDLE;
    check(vkAllocateDescriptorSets(g_device, &bakeAlloc, &bakeSet), "cloud v4 bake set failed");

    VkDescriptorImageInfo atlasStorage{VK_NULL_HANDLE, g_cloudV4AtlasView,
        VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo worleyStorage{VK_NULL_HANDLE, g_cloudV4WorleyView,
        VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo baseStorage{VK_NULL_HANDLE, g_cloudV4BaseView,
        VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo shapeStorage{VK_NULL_HANDLE, g_cloudV4ShapeView,
        VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[4]{};
    for(uint32_t index = 0u; index < 4u; ++index)
    {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = bakeSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1u;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    writes[0].pImageInfo = &atlasStorage;
    writes[1].pImageInfo = &worleyStorage;
    writes[2].pImageInfo = &baseStorage;
    writes[3].pImageInfo = &shapeStorage;
    vkUpdateDescriptorSets(g_device, 4u, writes, 0u, nullptr);

    VkPipeline bakePipeline = VK_NULL_HANDLE;
    try
    {
        const std::vector<uint8_t> code = loadSpv("shaders/cloud_bevy_noise.comp.spv");
        VkShaderModule module = createShaderModule(g_device, code);
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = bakePipelineLayout;
        const VkResult result = vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &bakePipeline);
        vkDestroyShaderModule(g_device, module, nullptr);
        if(result != VK_SUCCESS)
        {
            bakePipeline = VK_NULL_HANDLE;
        }
    }
    catch(const std::exception& error)
    {
        RTLog(std::string("[CLOUD V4] bake shader unavailable (") + error.what() + ")");
        bakePipeline = VK_NULL_HANDLE;
    }

    if(bakePipeline == VK_NULL_HANDLE)
    {
        RTLog("[CLOUD V4] noise bake skipped -> v4 will sample empty textures");
        return;
    }

    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = g_rtCommandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &commandAllocation, &commandBuffer),
        "cloud v4 bake command failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "cloud v4 bake begin failed");

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.image = g_cloudV4AtlasImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4WorleyImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4BaseImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4ShapeImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    struct { float mode[4]; } push{};
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, bakePipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, bakePipelineLayout, 0u,
        1u, &bakeSet, 0u, nullptr);
    push.mode[0] = 0.0f;
    vkCmdPushConstants(commandBuffer, bakePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u,
        &push);
    vkCmdDispatch(commandBuffer, CLOUD_V4_ATLAS_SIZE / 8u, CLOUD_V4_ATLAS_SIZE / 8u, 1u);
    push.mode[0] = 1.0f;
    vkCmdPushConstants(commandBuffer, bakePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u,
        &push);
    vkCmdDispatch(commandBuffer, CLOUD_V4_WORLEY_SIZE / 8u, CLOUD_V4_WORLEY_SIZE / 8u,
        CLOUD_V4_WORLEY_SIZE);
    push.mode[0] = 2.0f;
    vkCmdPushConstants(commandBuffer, bakePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u,
        &push);
    vkCmdDispatch(commandBuffer, CLOUD_V4_BASE_SIZE / 8u, CLOUD_V4_BASE_SIZE / 8u,
        CLOUD_V4_BASE_SIZE);
    push.mode[0] = 3.0f;
    vkCmdPushConstants(commandBuffer, bakePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u,
        &push);
    vkCmdDispatch(commandBuffer, CLOUD_V4_SHAPE_SIZE / 8u, CLOUD_V4_SHAPE_SIZE / 8u,
        CLOUD_V4_SHAPE_SIZE);

    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.image = g_cloudV4AtlasImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4WorleyImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4BaseImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
    barrier.image = g_cloudV4ShapeImage;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    {
        VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toGeneral.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toGeneral.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        toGeneral.subresourceRange.layerCount = 1u;
        toGeneral.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        const VkImage images[3] = {g_cloudV4AtlasImage, g_cloudV4WorleyImage, g_cloudV4BaseImage};
        for(const VkImage image : images)
        {
            toGeneral.image = image;
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
                &toGeneral);
        }
    }

    VkPipeline v5NoisePipeline = VK_NULL_HANDLE;
    const bool v5Model = cloudModelRequested() != 1;
    if(v5Model)
    {
        try
        {
            const std::vector<uint8_t> v5Code = loadSpv("shaders/cloud_v5_noise.comp.spv");
            VkShaderModule v5Module = createShaderModule(g_device, v5Code);
            VkPipelineShaderStageCreateInfo v5Stage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            v5Stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            v5Stage.module = v5Module;
            v5Stage.pName = "main";
            VkComputePipelineCreateInfo v5Info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            v5Info.stage = v5Stage;
            v5Info.layout = bakePipelineLayout;
            const VkResult v5Result = vkCreateComputePipelines(g_device,
                getSharedPipelineCache(), 1u, &v5Info, nullptr, &v5NoisePipeline);
            vkDestroyShaderModule(g_device, v5Module, nullptr);
            if(v5Result == VK_SUCCESS)
            {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    v5NoisePipeline);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                    bakePipelineLayout, 0u, 1u, &bakeSet, 0u, nullptr);
                struct { float mode[4]; } v5Push{};
                v5Push.mode[0] = 0.0f;
                vkCmdPushConstants(commandBuffer, bakePipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u, &v5Push);
                vkCmdDispatch(commandBuffer, CLOUD_V4_ATLAS_SIZE / 8u,
                    CLOUD_V4_ATLAS_SIZE / 8u, 1u);
                v5Push.mode[0] = 1.0f;
                vkCmdPushConstants(commandBuffer, bakePipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u, &v5Push);
                vkCmdDispatch(commandBuffer, CLOUD_V4_WORLEY_SIZE / 8u,
                    CLOUD_V4_WORLEY_SIZE / 8u, CLOUD_V4_WORLEY_SIZE);
                v5Push.mode[0] = 2.0f;
                vkCmdPushConstants(commandBuffer, bakePipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u, &v5Push);
                vkCmdDispatch(commandBuffer, CLOUD_V4_BASE_SIZE / 8u,
                    CLOUD_V4_BASE_SIZE / 8u, CLOUD_V4_BASE_SIZE);
                RTLog("[CLOUD V5] assets baked: weather " +
                    std::to_string(CLOUD_V4_ATLAS_SIZE) + "^2 + detail " +
                    std::to_string(CLOUD_V4_WORLEY_SIZE) + "^3 + base " +
                    std::to_string(CLOUD_V4_BASE_SIZE) + "^3 (mips " +
                    std::to_string(CLOUD_V4_WORLEY_MIPS) + "/" +
                    std::to_string(CLOUD_V4_BASE_MIPS) + ")");
            }
            else
            {
                RTLog("[CLOUD V5] noise pipeline creation failed; v5 will sample stale assets");
            }
        }
        catch(const std::exception& error)
        {
            RTLog(std::string("[CLOUD V5] noise shader unavailable (") + error.what() + ")");
        }
    }
    {
        const VkImage mipImages[2] = {g_cloudV4WorleyImage, g_cloudV4BaseImage};
        const uint32_t mipLevels[2] = {CLOUD_V4_WORLEY_MIPS, CLOUD_V4_BASE_MIPS};
        const uint32_t mipExtents[2] = {CLOUD_V4_WORLEY_SIZE, CLOUD_V4_BASE_SIZE};
        const VkImageLayout bakeLayout = v5Model ? VK_IMAGE_LAYOUT_GENERAL
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        for(int volume = 0; volume < 2; ++volume)
        {
            if(mipImages[volume] == VK_NULL_HANDLE)
                continue;
            VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toTransfer.oldLayout = bakeLayout;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.levelCount = 1u;
            toTransfer.subresourceRange.layerCount = 1u;
            toTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            toTransfer.image = mipImages[volume];
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);

            if(mipLevels[volume] > 1u)
            {
                VkImageMemoryBarrier restMips = toTransfer;
                restMips.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                restMips.srcAccessMask = 0u;
                restMips.subresourceRange.baseMipLevel = 1u;
                restMips.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &restMips);
            }

            for(uint32_t level = 1u; level < mipLevels[volume]; ++level)
            {
                const uint32_t srcExtent = std::max(1u, mipExtents[volume] >> (level - 1u));
                const uint32_t dstExtent = std::max(1u, mipExtents[volume] >> level);
                VkImageBlit blit{};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = level - 1u;
                blit.srcSubresource.layerCount = 1u;
                blit.srcOffsets[1] = {static_cast<int32_t>(srcExtent),
                    static_cast<int32_t>(srcExtent), static_cast<int32_t>(srcExtent)};
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = level;
                blit.dstSubresource.layerCount = 1u;
                blit.dstOffsets[1] = {static_cast<int32_t>(dstExtent),
                    static_cast<int32_t>(dstExtent), static_cast<int32_t>(dstExtent)};
                vkCmdBlitImage(commandBuffer,
                    mipImages[volume], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    mipImages[volume], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1u, &blit, VK_FILTER_LINEAR);

                VkImageMemoryBarrier nextLevel{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                nextLevel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                nextLevel.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                nextLevel.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                nextLevel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                nextLevel.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                nextLevel.subresourceRange.baseMipLevel = level;
                nextLevel.subresourceRange.levelCount = 1u;
                nextLevel.subresourceRange.layerCount = 1u;
                nextLevel.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                nextLevel.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                nextLevel.image = mipImages[volume];
                vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
                    &nextLevel);
            }

            VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toRead.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            toRead.subresourceRange.layerCount = 1u;
            toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            toRead.image = mipImages[volume];
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toRead);
        }
    }

    check(vkEndCommandBuffer(commandBuffer), "cloud v4 bake end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    const auto bakeStart = std::chrono::steady_clock::now();
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "cloud v4 bake submit failed");
    check(vkQueueWaitIdle(g_queue), "cloud v4 bake wait failed");
    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - bakeStart).count();
        char timing[128];
        std::snprintf(timing, sizeof(timing), "bake wall time = %lld ms (TDR limit ~2000)",
            static_cast<long long>(ms));
        cloudV4DebugLog(timing);
    }
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);

    vkDestroyPipeline(g_device, bakePipeline, nullptr);
    vkDestroyPipelineLayout(g_device, bakePipelineLayout, nullptr);
    vkDestroyDescriptorPool(g_device, bakePool, nullptr);
    vkDestroyDescriptorSetLayout(g_device, bakeSetLayout, nullptr);

    RTLog("[CLOUD V4] bevy clouds: atlas " + std::to_string(CLOUD_V4_ATLAS_SIZE) + "^2 + worley " +
        std::to_string(CLOUD_V4_WORLEY_SIZE) + "^3 + shape " +
        std::to_string(CLOUD_V4_SHAPE_SIZE) + "^3 baked (MIT, evroon/bevy-volumetric-clouds)");

#if 0
    {
        const uint32_t probeSize = CLOUD_V4_BASE_SIZE;
        const VkDeviceSize probeBytes = static_cast<VkDeviceSize>(probeSize) * probeSize * 16u;
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = probeBytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer probeBuffer = VK_NULL_HANDLE;
        if(vkCreateBuffer(g_device, &bufferInfo, nullptr, &probeBuffer) == VK_SUCCESS)
        {
            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(g_device, probeBuffer, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryType(g_physicalDevice, requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDeviceMemory probeMemory = VK_NULL_HANDLE;
            if(vkAllocateMemory(g_device, &allocation, nullptr, &probeMemory) == VK_SUCCESS)
            {
                vkBindBufferMemory(g_device, probeBuffer, probeMemory, 0u);
                VkCommandBufferAllocateInfo probeAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                probeAlloc.commandPool = g_rtCommandPool;
                probeAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                probeAlloc.commandBufferCount = 1u;
                VkCommandBuffer probeCmd = VK_NULL_HANDLE;
                vkAllocateCommandBuffers(g_device, &probeAlloc, &probeCmd);
                VkCommandBufferBeginInfo probeBegin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                probeBegin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(probeCmd, &probeBegin);
                VkImageMemoryBarrier probeBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                probeBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                probeBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                probeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                probeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                probeBarrier.image = g_cloudV4BaseImage;
                probeBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                probeBarrier.subresourceRange.levelCount = 1u;
                probeBarrier.subresourceRange.layerCount = 1u;
                probeBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                probeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                vkCmdPipelineBarrier(probeCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &probeBarrier);
                VkBufferImageCopy region{};
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.layerCount = 1u;
                region.imageOffset = {0, 0, static_cast<int32_t>(probeSize / 2u)};
                region.imageExtent = {probeSize, probeSize, 1u};
                vkCmdCopyImageToBuffer(probeCmd, g_cloudV4BaseImage,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probeBuffer, 1u, &region);
                probeBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                probeBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                probeBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                probeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(probeCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &probeBarrier);
                vkEndCommandBuffer(probeCmd);
                VkSubmitInfo probeSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                probeSubmit.commandBufferCount = 1u;
                probeSubmit.pCommandBuffers = &probeCmd;
                vkQueueSubmit(g_queue, 1u, &probeSubmit, VK_NULL_HANDLE);
                vkQueueWaitIdle(g_queue);

                void* mapped = nullptr;
                if(vkMapMemory(g_device, probeMemory, 0u, VK_WHOLE_SIZE, 0u, &mapped) == VK_SUCCESS)
                {
                    const uint16_t* halfs = static_cast<const uint16_t*>(mapped);
                    const size_t count = static_cast<size_t>(probeSize) * probeSize * 4u;
                    size_t nonZero = 0u;
                    uint16_t maximum = 0u;
                    for(size_t i = 0; i < count; ++i)
                    {
                        if(halfs[i] != 0u) ++nonZero;
                        if(halfs[i] > maximum) maximum = halfs[i];
                    }
                    char line[192];
                    std::snprintf(line, sizeof(line),
                        "base slice z=%u: nonzero=%zu/%zu (%.1f%%), maxHalf=0x%04x",
                        probeSize / 2u, nonZero, count,
                        100.0 * double(nonZero) / double(count), int(maximum));
                    cloudV4DebugLog(line);
                    vkUnmapMemory(g_device, probeMemory);
                }
                vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &probeCmd);
                vkFreeMemory(g_device, probeMemory, nullptr);
            }
            vkDestroyBuffer(g_device, probeBuffer, nullptr);
        }
    }
#endif

    RTLog("[CLOUD V4] base " + std::to_string(CLOUD_V4_BASE_SIZE) +
        "^3 Perlin-Worley baked -> binding 9 (binding 1 untouched: shared)");
    cloudV4DebugLog(("baked base=" + std::to_string(CLOUD_V4_BASE_SIZE) +
        " worley=" + std::to_string(CLOUD_V4_WORLEY_SIZE) +
        " atlasView=" + (g_cloudV4AtlasView ? "ok" : "null") +
        " baseView=" + (g_cloudV4BaseView ? "ok" : "null")).c_str());
    cloudV4DebugLog("done createCloudV4Resources");
}
void destroyCloudV4Resources()
{
    if(g_cloudV4AtlasSampler) vkDestroySampler(g_device, g_cloudV4AtlasSampler, nullptr);
    if(g_cloudV4WorleySampler) vkDestroySampler(g_device, g_cloudV4WorleySampler, nullptr);
    if(g_cloudV4AtlasView) vkDestroyImageView(g_device, g_cloudV4AtlasView, nullptr);
    if(g_cloudV4WorleyView) vkDestroyImageView(g_device, g_cloudV4WorleyView, nullptr);
    if(g_cloudV4AtlasImage) vkDestroyImage(g_device, g_cloudV4AtlasImage, nullptr);
    if(g_cloudV4WorleyImage) vkDestroyImage(g_device, g_cloudV4WorleyImage, nullptr);
    if(g_cloudV4AtlasMemory) vkFreeMemory(g_device, g_cloudV4AtlasMemory, nullptr);
    if(g_cloudV4WorleyMemory) vkFreeMemory(g_device, g_cloudV4WorleyMemory, nullptr);
    g_cloudV4AtlasSampler = VK_NULL_HANDLE;
    g_cloudV4WorleySampler = VK_NULL_HANDLE;
    g_cloudV4AtlasView = VK_NULL_HANDLE;
    g_cloudV4WorleyView = VK_NULL_HANDLE;
    g_cloudV4AtlasImage = VK_NULL_HANDLE;
    g_cloudV4WorleyImage = VK_NULL_HANDLE;
    g_cloudV4AtlasMemory = VK_NULL_HANDLE;
    g_cloudV4WorleyMemory = VK_NULL_HANDLE;
}
void destroyCloudV3Resources()
{
    if(g_cloudV3ShapeSampler) vkDestroySampler(g_device, g_cloudV3ShapeSampler, nullptr);
    if(g_cloudV3ShapeView) vkDestroyImageView(g_device, g_cloudV3ShapeView, nullptr);
    if(g_cloudV3ShapeImage) vkDestroyImage(g_device, g_cloudV3ShapeImage, nullptr);
    if(g_cloudV3ShapeMemory) vkFreeMemory(g_device, g_cloudV3ShapeMemory, nullptr);
    if(g_cloudV3DetailSampler) vkDestroySampler(g_device, g_cloudV3DetailSampler, nullptr);
    if(g_cloudV3DetailView) vkDestroyImageView(g_device, g_cloudV3DetailView, nullptr);
    if(g_cloudV3DetailImage) vkDestroyImage(g_device, g_cloudV3DetailImage, nullptr);
    if(g_cloudV3DetailMemory) vkFreeMemory(g_device, g_cloudV3DetailMemory, nullptr);
    if(g_cloudV3CurlSampler) vkDestroySampler(g_device, g_cloudV3CurlSampler, nullptr);
    if(g_cloudV3CurlView) vkDestroyImageView(g_device, g_cloudV3CurlView, nullptr);
    if(g_cloudV3CurlImage) vkDestroyImage(g_device, g_cloudV3CurlImage, nullptr);
    if(g_cloudV3CurlMemory) vkFreeMemory(g_device, g_cloudV3CurlMemory, nullptr);
    if(g_cloudV3WeatherSampler) vkDestroySampler(g_device, g_cloudV3WeatherSampler, nullptr);
    if(g_cloudV3WeatherView) vkDestroyImageView(g_device, g_cloudV3WeatherView, nullptr);
    if(g_cloudV3WeatherImage) vkDestroyImage(g_device, g_cloudV3WeatherImage, nullptr);
    if(g_cloudV3WeatherMemory) vkFreeMemory(g_device, g_cloudV3WeatherMemory, nullptr);
    g_cloudV3ShapeSampler = VK_NULL_HANDLE;
    g_cloudV3ShapeView = VK_NULL_HANDLE;
    g_cloudV3ShapeImage = VK_NULL_HANDLE;
    g_cloudV3ShapeMemory = VK_NULL_HANDLE;
    g_cloudV3DetailSampler = VK_NULL_HANDLE;
    g_cloudV3DetailView = VK_NULL_HANDLE;
    g_cloudV3DetailImage = VK_NULL_HANDLE;
    g_cloudV3DetailMemory = VK_NULL_HANDLE;
    g_cloudV3CurlSampler = VK_NULL_HANDLE;
    g_cloudV3CurlView = VK_NULL_HANDLE;
    g_cloudV3CurlImage = VK_NULL_HANDLE;
    g_cloudV3CurlMemory = VK_NULL_HANDLE;
    g_cloudV3WeatherSampler = VK_NULL_HANDLE;
    g_cloudV3WeatherView = VK_NULL_HANDLE;
    g_cloudV3WeatherImage = VK_NULL_HANDLE;
    g_cloudV3WeatherMemory = VK_NULL_HANDLE;
}
void destroyCloudNoiseResources()
{
    if(g_cloudShapeNoiseSampler && g_cloudShapeNoiseSampler != g_cloudV4ShapeSampler)
        vkDestroySampler(g_device, g_cloudShapeNoiseSampler, nullptr);
    if(g_cloudShapeNoiseView && g_cloudShapeNoiseView != g_cloudV4ShapeView)
        vkDestroyImageView(g_device, g_cloudShapeNoiseView, nullptr);
    if(g_cloudShapeNoiseImage) vkDestroyImage(g_device, g_cloudShapeNoiseImage, nullptr);
    if(g_cloudShapeNoiseMemory) vkFreeMemory(g_device, g_cloudShapeNoiseMemory, nullptr);
    if(g_cloudCurlNoiseSampler) vkDestroySampler(g_device, g_cloudCurlNoiseSampler, nullptr);
    if(g_cloudCurlNoiseView) vkDestroyImageView(g_device, g_cloudCurlNoiseView, nullptr);
    if(g_cloudCurlNoiseImage) vkDestroyImage(g_device, g_cloudCurlNoiseImage, nullptr);
    if(g_cloudCurlNoiseMemory) vkFreeMemory(g_device, g_cloudCurlNoiseMemory, nullptr);
    g_cloudShapeNoiseSampler = VK_NULL_HANDLE;
    g_cloudShapeNoiseView = VK_NULL_HANDLE;
    g_cloudShapeNoiseImage = VK_NULL_HANDLE;
    g_cloudShapeNoiseMemory = VK_NULL_HANDLE;
    g_cloudCurlNoiseSampler = VK_NULL_HANDLE;
    g_cloudCurlNoiseView = VK_NULL_HANDLE;
    g_cloudCurlNoiseImage = VK_NULL_HANDLE;
    g_cloudCurlNoiseMemory = VK_NULL_HANDLE;
}

// 云的图与描述符集是否仍与"当前窗口尺寸"一致 —— 见 g_volumetricCloudBoundRenderWidth
// 的说明。云图尺寸相同但渲染分辨率变了时也必须重建（描述符/光柱目标跟着变）。
bool volumetricCloudBindingsMatchCurrentSizes(
    uint32_t width,
    uint32_t height)
{
    return
        g_volumetricCloudWidth == width &&
        g_volumetricCloudHeight == height &&
        g_volumetricCloudBoundRenderWidth == g_rtOutputWidth &&
        g_volumetricCloudBoundRenderHeight == g_rtOutputHeight &&
        g_volumetricCloudBoundOutputWidth == g_dlssOutputWidth &&
        g_volumetricCloudBoundOutputHeight == g_dlssOutputHeight;
}

// 2026-10-26：场景深度的 nearest + clamp 采样器。
// 体积云的 depth binding（render set 5 / composite 2）与体积光的 sceneDepth
// （light set 3）都用它。原先只在 createVolumeRestirResources 里懒建，而云的
// 描述符集在**更早**的 createVolumetricCloudResources 里就要写它 ⇒ 首帧拿到
// VK_NULL_HANDLE，被 vkGuard 挡下不写 ⇒ 深度绑定空、体积光恒为 0
// （日志里的 "binding 2/5/3 NULL sampler"）。改成"谁先要谁建"。
// 生命周期：会话级，运行期不销毁 —— destroyVolumeRestirResources 会随 froxel
// 尺寸变化跑，而云的描述符集仍持有旧句柄，销毁即悬空。
VkSampler volDepthSampler()
{
    if(g_volDepthSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        samplerInfo.compareOp = VK_COMPARE_OP_NEVER;

        check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_volDepthSampler),
            "vol depth sampler failed");
        RTLogFlushed("[VOL] depth sampler created");
    }
    return g_volDepthSampler;
}
void createVolumetricCloudResources(uint32_t width, uint32_t height)
{
    const uint32_t cloudWidth = width;
    const uint32_t cloudHeight = height;
    if(g_volumetricCloudImage != VK_NULL_HANDLE &&
       volumetricCloudBindingsMatchCurrentSizes(cloudWidth, cloudHeight))
    {
        return;
    }
    if(g_volumetricCloudImage != VK_NULL_HANDLE)
    {
        destroyVolumetricCloudResources();
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
            "VK_FORMAT_R16G16B16A16_SFLOAT cannot be sampled and stored for volumetric clouds"
        );
    }
    vkGetPhysicalDeviceFormatProperties(g_physicalDevice, VK_FORMAT_R32_SFLOAT, &formatProperties);
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        throw std::runtime_error(
            "VK_FORMAT_R32_SFLOAT cannot be sampled and stored for volumetric cloud depth"
        );
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = { cloudWidth, cloudHeight, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudImage),
        "volumetric cloud image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39630");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudMemory),
        "volumetric cloud image allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39639");
    check(vkBindImageMemory(g_device, g_volumetricCloudImage,
        g_volumetricCloudMemory, 0u), "volumetric cloud image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39641");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_volumetricCloudImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudView),
        "volumetric cloud image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39651");

    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudHistoryImage),
        "volumetric cloud history image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39654");
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudHistoryImage, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudHistoryMemory),
        "volumetric cloud history memory allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39660");
    check(vkBindImageMemory(g_device, g_volumetricCloudHistoryImage,
        g_volumetricCloudHistoryMemory, 0u), "volumetric cloud history image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39662");
    viewInfo.image = g_volumetricCloudHistoryImage;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudHistoryView),
        "volumetric cloud history image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39665");

    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudResolvedImage),
        "volumetric cloud resolved image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39668");
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudResolvedImage, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudResolvedMemory),
        "volumetric cloud resolved image allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39674");
    check(vkBindImageMemory(g_device, g_volumetricCloudResolvedImage,
        g_volumetricCloudResolvedMemory, 0u), "volumetric cloud resolved image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39676");
    viewInfo.image = g_volumetricCloudResolvedImage;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudResolvedView),
        "volumetric cloud resolved image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39679");

    imageInfo.format = VK_FORMAT_R32G32_SFLOAT;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudDepthImage),
        "volumetric cloud depth image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39684");
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudDepthImage, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudDepthMemory),
        "volumetric cloud depth image allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39690");
    check(vkBindImageMemory(g_device, g_volumetricCloudDepthImage,
        g_volumetricCloudDepthMemory, 0u), "volumetric cloud depth image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39692");

    viewInfo.image = g_volumetricCloudDepthImage;
    viewInfo.format = VK_FORMAT_R32G32_SFLOAT;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudDepthView),
        "volumetric cloud depth image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39697");

    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_volumetricCloudHistoryDepthImage),
        "volumetric cloud history depth image create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39700");
    vkGetImageMemoryRequirements(g_device, g_volumetricCloudHistoryDepthImage, &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_volumetricCloudHistoryDepthMemory),
        "volumetric cloud history depth image allocate failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39706");
    check(vkBindImageMemory(g_device, g_volumetricCloudHistoryDepthImage,
        g_volumetricCloudHistoryDepthMemory, 0u), "volumetric cloud history depth image bind failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39708");

    viewInfo.image = g_volumetricCloudHistoryDepthImage;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_volumetricCloudHistoryDepthView),
        "volumetric cloud history depth image view create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39712");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_volumetricCloudSampler),
        "volumetric cloud sampler create failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39723");

    {
        const uint32_t godRayWidth = std::max(1u, g_rtOutputWidth);
        const uint32_t godRayHeight = std::max(1u, g_rtOutputHeight);
        VkImageCreateInfo godRayImageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        godRayImageInfo.imageType = VK_IMAGE_TYPE_2D;
        godRayImageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        godRayImageInfo.extent = { godRayWidth, godRayHeight, 1u };
        godRayImageInfo.mipLevels = 1u;
        godRayImageInfo.arrayLayers = 1u;
        godRayImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        godRayImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        godRayImageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        godRayImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkImageViewCreateInfo godRayViewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        godRayViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        godRayViewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        godRayViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        godRayViewInfo.subresourceRange.levelCount = 1u;
        godRayViewInfo.subresourceRange.layerCount = 1u;

        struct GodRayImageBind
        {
            VkImage* image;
            VkDeviceMemory* memory;
            VkImageView* view;
        };
        const GodRayImageBind godRayImages[2] = {
            { &g_godRayTargetImage, &g_godRayTargetMemory, &g_godRayTargetView },
            { &g_godRayHistoryImage, &g_godRayHistoryMemory, &g_godRayHistoryView }
        };
        for(const GodRayImageBind& bind : godRayImages)
        {
            check(vkCreateImage(g_device, &godRayImageInfo, nullptr, bind.image),
                "god ray image create failed");
                RTLogFlushed("[VOLCLOUD] passed check at L39773");
            VkMemoryRequirements godRayRequirements{};
            vkGetImageMemoryRequirements(g_device, *bind.image, &godRayRequirements);
            VkMemoryAllocateInfo godRayAllocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            godRayAllocation.allocationSize = godRayRequirements.size;
            godRayAllocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
                godRayRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(g_device, &godRayAllocation, nullptr, bind.memory),
                "god ray image allocate failed");
                RTLogFlushed("[VOLCLOUD] passed check at L39781");
            check(vkBindImageMemory(g_device, *bind.image, *bind.memory, 0u),
                "god ray image bind failed");
                RTLogFlushed("[VOLCLOUD] passed check at L39783");
            godRayViewInfo.image = *bind.image;
            check(vkCreateImageView(g_device, &godRayViewInfo, nullptr, bind.view),
                "god ray image view create failed");
                RTLogFlushed("[VOLCLOUD] passed check at L39786");
        }
        g_godRayWidth = godRayWidth;
        g_godRayHeight = godRayHeight;
        g_godRayTargetLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        g_godRayHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        g_godRayHistoryNeedsClear.store(true);
    }

    VkDescriptorSetLayoutBinding renderBindings[14]{};
    renderBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[3] = { 3u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[4] = { 4u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[5] = { 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[6] = { 6u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[7] = { 7u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[8] = { 8u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[9] = { 9u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[10] = { 10u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[11] = { 11u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[12] = { 12u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    renderBindings[13] = { 13u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo renderLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    renderLayoutInfo.bindingCount = 14u;
    renderLayoutInfo.pBindings = renderBindings;
    check(vkCreateDescriptorSetLayout(g_device, &renderLayoutInfo, nullptr,
        &g_volumetricCloudRenderSetLayout), "volumetric cloud render set layout failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39846");

    VkDescriptorSetLayoutBinding resolveBindings[6]{};
    resolveBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    for(uint32_t i = 1u; i < 5u; ++i)
    {
        resolveBindings[i] = { i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    }
    resolveBindings[5] = { 5u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo resolveLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    resolveLayoutInfo.bindingCount = 6u;
    resolveLayoutInfo.pBindings = resolveBindings;
    check(vkCreateDescriptorSetLayout(g_device, &resolveLayoutInfo, nullptr,
        &g_volumetricCloudResolveSetLayout), "volumetric cloud resolve set layout failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39864");

    VkDescriptorSetLayoutBinding compositeBindings[7]{};
    compositeBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[3] = { 3u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[4] = { 4u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[5] = { 5u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    compositeBindings[6] = { 6u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo compositeLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    compositeLayoutInfo.bindingCount = 7u;
    compositeLayoutInfo.pBindings = compositeBindings;
    check(vkCreateDescriptorSetLayout(g_device, &compositeLayoutInfo, nullptr,
        &g_volumetricCloudCompositeSetLayout), "volumetric cloud composite set layout failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39893");

    VkDescriptorSetLayoutBinding lightBindings[7]{};
    lightBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[3] = { 3u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[4] = { 4u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[5] = { 8u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    lightBindings[6] = { 9u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo lightLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    lightLayoutInfo.bindingCount = 7u;
    lightLayoutInfo.pBindings = lightBindings;
    check(vkCreateDescriptorSetLayout(g_device, &lightLayoutInfo, nullptr,
        &g_volumetricLightSetLayout), "volumetric light set layout failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39970");

    VkDescriptorPoolSize poolSizes[5] = {
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4u },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 21u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6u },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3u }
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 4u;
    poolInfo.poolSizeCount = 5u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_volumetricCloudDescriptorPool), "volumetric cloud descriptor pool failed");
        RTLogFlushed("[VOLCLOUD] passed check at L39999");

    {
        VkDescriptorSetLayoutBinding godRayBindings[4]{};
        godRayBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        godRayBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        godRayBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        godRayBindings[3] = { 3u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo godRayLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        godRayLayoutInfo.bindingCount = 4u;
        godRayLayoutInfo.pBindings = godRayBindings;
        check(vkCreateDescriptorSetLayout(g_device, &godRayLayoutInfo, nullptr,
            &g_godRayCompositeSetLayout), "god ray composite set layout failed");
            RTLogFlushed("[VOLCLOUD] passed check at L40026");

        VkDescriptorPoolSize godRayPoolSizes[3] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
        };
        VkDescriptorPoolCreateInfo godRayPoolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        };
        godRayPoolInfo.maxSets = 1u;
        godRayPoolInfo.poolSizeCount = 3u;
        godRayPoolInfo.pPoolSizes = godRayPoolSizes;
        check(vkCreateDescriptorPool(g_device, &godRayPoolInfo, nullptr,
            &g_godRayDescriptorPool), "god ray composite pool failed");
            RTLogFlushed("[VOLCLOUD] passed check at L40040");

        VkDescriptorSetAllocateInfo godRaySetAllocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        godRaySetAllocation.descriptorPool = g_godRayDescriptorPool;
        godRaySetAllocation.descriptorSetCount = 1u;
        godRaySetAllocation.pSetLayouts = &g_godRayCompositeSetLayout;
        check(vkAllocateDescriptorSets(g_device, &godRaySetAllocation,
            &g_godRayCompositeSet), "god ray composite set allocation failed");
            RTLogFlushed("[VOLCLOUD] passed check at L40049");

        VkDescriptorImageInfo godRayInputInfo{};
        godRayInputInfo.sampler = g_volumetricCloudSampler;
        godRayInputInfo.imageView = g_godRayTargetView;
        godRayInputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo godRaySceneInfo{};
        godRaySceneInfo.imageView = g_rtOutputView;
        godRaySceneInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        godRaySceneInfo.sampler = g_volumetricCloudSampler;
        VkDescriptorImageInfo godRayDepthInfo{};
        godRayDepthInfo.sampler = g_volumetricCloudSampler;
        godRayDepthInfo.imageView = g_dlssDepthView;
        godRayDepthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorBufferInfo godRayCameraInfo{};
        godRayCameraInfo.buffer = g_cameraUboBuffer;
        godRayCameraInfo.range = sizeof(CameraUBO);
        VkWriteDescriptorSet godRayWrites[4]{};
        godRayWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_godRayCompositeSet, 0u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &godRayInputInfo, nullptr, nullptr };
        godRayWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_godRayCompositeSet, 1u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &godRaySceneInfo, nullptr, nullptr };
        godRayWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_godRayCompositeSet, 2u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &godRayDepthInfo, nullptr, nullptr };
        godRayWrites[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_godRayCompositeSet, 3u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &godRayCameraInfo, nullptr };
        vkUpdateDescriptorSets(g_device, 4u, godRayWrites, 0u, nullptr);
    }

    VkDescriptorSetLayout setLayouts[4] = {
        g_volumetricCloudRenderSetLayout, g_volumetricCloudResolveSetLayout,
        g_volumetricCloudCompositeSetLayout, g_volumetricLightSetLayout
    };
    VkDescriptorSet descriptorSets[4]{};
    VkDescriptorSetAllocateInfo setAllocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    setAllocation.descriptorPool = g_volumetricCloudDescriptorPool;
    setAllocation.descriptorSetCount = 4u;
    setAllocation.pSetLayouts = setLayouts;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, descriptorSets),
        "volumetric cloud descriptor set allocation failed");
        RTLogFlushed("[VOLCLOUD] passed check at L40094");
    g_volumetricCloudRenderSet = descriptorSets[0];
    g_volumetricCloudResolveSet = descriptorSets[1];
    g_volumetricCloudCompositeSet = descriptorSets[2];
    g_volumetricLightSet = descriptorSets[3];

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    const bool cloudV3Active = volumetricCloudV3Requested() && g_cloudV3ShapeImage != VK_NULL_HANDLE;
    VkDescriptorImageInfo shapeInfo{};
    shapeInfo.sampler = cloudV3Active ? g_cloudV3ShapeSampler : g_cloudShapeNoiseSampler;
    shapeInfo.imageView = cloudV3Active ? g_cloudV3ShapeView : g_cloudShapeNoiseView;
    shapeInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo curlInfo{};
    curlInfo.sampler = cloudV3Active ? g_cloudV3DetailSampler : g_cloudCurlNoiseSampler;
    curlInfo.imageView = cloudV3Active ? g_cloudV3DetailView : g_cloudCurlNoiseView;
    curlInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo v3CurlInfo{};
    v3CurlInfo.sampler = g_cloudV3CurlView ? g_cloudV3CurlSampler : g_cloudBaseNoiseSampler;
    v3CurlInfo.imageView = g_cloudV3CurlView ? g_cloudV3CurlView : g_cloudBaseNoiseView;
    v3CurlInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo v3WeatherInfo{};
    v3WeatherInfo.sampler = g_cloudV3WeatherView ? g_cloudV3WeatherSampler : g_cloudBaseNoiseSampler;
    v3WeatherInfo.imageView = g_cloudV3WeatherView ? g_cloudV3WeatherView : g_cloudBaseNoiseView;
    v3WeatherInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(g_cloudV4AtlasView != VK_NULL_HANDLE)
    {
        v3WeatherInfo.sampler = g_cloudV4AtlasSampler;
        v3WeatherInfo.imageView = g_cloudV4AtlasView;
    }

    VkDescriptorImageInfo cloudBaseNoiseInfo{};
    cloudBaseNoiseInfo.sampler = g_cloudBaseNoiseSampler;
    cloudBaseNoiseInfo.imageView = g_cloudBaseNoiseView;
    cloudBaseNoiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if(g_cloudV4BaseView != VK_NULL_HANDLE)
    {
        cloudBaseNoiseInfo.sampler = g_cloudV4BaseSampler;
        cloudBaseNoiseInfo.imageView = g_cloudV4BaseView;
        v3CurlInfo.sampler = g_cloudV4WorleySampler;
        v3CurlInfo.imageView = g_cloudV4WorleyView;
    }
    VkDescriptorImageInfo cloudRawInfo{};
    cloudRawInfo.sampler = g_volumetricCloudSampler;
    cloudRawInfo.imageView = g_volumetricCloudView;
    cloudRawInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudStorageInfo{};
    cloudStorageInfo.imageView = g_volumetricCloudView;
    cloudStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudResolvedInfo{};
    cloudResolvedInfo.sampler = g_volumetricCloudSampler;
    cloudResolvedInfo.imageView = g_volumetricCloudResolvedView;
    cloudResolvedInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudResolvedStorageInfo{};
    cloudResolvedStorageInfo.imageView = g_volumetricCloudResolvedView;
    cloudResolvedStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudHistoryInfo{};
    cloudHistoryInfo.sampler = g_volumetricCloudSampler;
    cloudHistoryInfo.imageView = g_volumetricCloudHistoryView;
    cloudHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudDepthStorageInfo{};
    cloudDepthStorageInfo.imageView = g_volumetricCloudDepthView;
    cloudDepthStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudDepthInfo{};
    cloudDepthInfo.sampler = g_volumetricCloudSampler;
    cloudDepthInfo.imageView = g_volumetricCloudDepthView;
    cloudDepthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo cloudHistoryDepthInfo{};
    cloudHistoryDepthInfo.sampler = g_volumetricCloudSampler;
    cloudHistoryDepthInfo.imageView = g_volumetricCloudHistoryDepthView;
    cloudHistoryDepthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo sceneStorageInfo{};
    sceneStorageInfo.imageView = g_rtOutputView;
    sceneStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo dlssOutputStorageInfo{};
    dlssOutputStorageInfo.imageView = g_dlssOutputView != VK_NULL_HANDLE
        ? g_dlssOutputView : g_rtOutputView;
    dlssOutputStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo depthInfo{};
    depthInfo.sampler = g_volumetricCloudSampler;
    depthInfo.imageView = g_dlssDepthView;
    depthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    depthInfo.sampler = volDepthSampler();
    VkDescriptorBufferInfo aeStateInfo{};
    aeStateInfo.buffer = g_aeStateBuffer;
    aeStateInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo fogLightListInfo{};
    fogLightListInfo.buffer = g_localLightGpuBuffer;
    fogLightListInfo.offset = 0u;
    fogLightListInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo fogLightGridInfo{};
    fogLightGridInfo.buffer = g_localLightGridGpuBuffer;
    fogLightGridInfo.offset = 0u;
    fogLightGridInfo.range = VK_WHOLE_SIZE;
    VkDescriptorImageInfo sunShadowInfo{};
    sunShadowInfo.sampler = g_sunShadowSampler;
    sunShadowInfo.imageView = g_sunShadowView;
    sunShadowInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const bool sunShadowBound = g_sunShadowView != VK_NULL_HANDLE &&
        g_sunShadowSampler != VK_NULL_HANDLE;
    VkDescriptorImageInfo sunShadowFarInfo{};
    sunShadowFarInfo.sampler = g_sunShadowFarSampler;
    sunShadowFarInfo.imageView = g_sunShadowFarView;
    sunShadowFarInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const bool sunShadowFarBound = g_sunShadowFarView != VK_NULL_HANDLE &&
        g_sunShadowFarSampler != VK_NULL_HANDLE;
    VkDescriptorImageInfo terrainHeightInfo{};
    terrainHeightInfo.sampler = g_terrainHeightSampler;
    terrainHeightInfo.imageView = g_terrainHeightView;
    terrainHeightInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    const bool terrainHeightBound = g_terrainHeightView != VK_NULL_HANDLE &&
        g_terrainHeightSampler != VK_NULL_HANDLE;
    VkDescriptorImageInfo godRayHistoryInfo{};
    godRayHistoryInfo.sampler = g_volumetricCloudSampler;
    godRayHistoryInfo.imageView = g_godRayHistoryView;
    godRayHistoryInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[36]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 1u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shapeInfo, nullptr, nullptr };
    writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &curlInfo, nullptr, nullptr };
    writes[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 3u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &cloudStorageInfo, nullptr, nullptr };
    writes[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 4u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &cloudDepthStorageInfo, nullptr, nullptr };
    writes[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[6] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 1u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudRawInfo, nullptr, nullptr };
    writes[7] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudHistoryInfo, nullptr, nullptr };
    writes[8] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 3u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudDepthInfo, nullptr, nullptr };
    writes[9] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 4u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudHistoryDepthInfo, nullptr, nullptr };
    writes[10] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudResolveSet, 5u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &cloudResolvedStorageInfo, nullptr, nullptr };
    writes[11] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudResolvedInfo, nullptr, nullptr };
    writes[12] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 1u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &sceneStorageInfo, nullptr, nullptr };
    writes[13] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr };
    writes[14] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 3u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[15] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 5u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr };
    writes[16] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[17] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 1u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shapeInfo, nullptr, nullptr };
    writes[18] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &curlInfo, nullptr, nullptr };
    writes[19] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 3u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, nullptr, nullptr };
    VkDescriptorImageInfo godRayStorageInfo{};
    godRayStorageInfo.imageView = g_godRayTargetView;
    godRayStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[20] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 4u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &godRayStorageInfo, nullptr, nullptr };
    writes[21] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 4u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &aeStateInfo, nullptr };
    writes[22] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 5u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudDepthInfo, nullptr, nullptr };
    writes[23] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudCompositeSet, 6u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dlssOutputStorageInfo, nullptr, nullptr };
    writes[24] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 9u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudBaseNoiseInfo, nullptr, nullptr };
    writes[25] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 10u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &v3CurlInfo, nullptr, nullptr };
    writes[26] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 11u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &v3WeatherInfo, nullptr, nullptr };
    uint32_t writeCount = 27u;
    if(sunShadowBound)
    {
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 6u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sunShadowInfo, nullptr, nullptr };
    }
    if(sunShadowFarBound)
    {
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 8u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sunShadowFarInfo, nullptr, nullptr };
    }
    if(terrainHeightBound)
    {
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 7u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &terrainHeightInfo, nullptr, nullptr };
    }
    writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricLightSet, 8u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &godRayHistoryInfo, nullptr, nullptr };
    if(fogLightListInfo.buffer != VK_NULL_HANDLE &&
       fogLightGridInfo.buffer != VK_NULL_HANDLE)
    {
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 12u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &fogLightListInfo, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudRenderSet, 13u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &fogLightGridInfo, nullptr };
        g_cloudFogLightSetBound = g_volumetricCloudRenderSet;
        g_cloudFogLightListBound = g_localLightGpuBuffer;
        g_cloudFogLightGridBound = g_localLightGridGpuBuffer;
        g_cloudFogLightDescriptorsReady.store(true);
    }
    writeCount = writeCount < 36u ? writeCount : 36u;

    {
        uint32_t safeCount = 0u;

        for(uint32_t i = 0u; i < writeCount; ++i)
        {
            VkWriteDescriptorSet w = writes[i];

            if(w.sType != VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET ||
               w.descriptorType == 0u ||
               w.descriptorCount == 0u)
            {
                RTLogFlushed("[VK GUARD] cloud: skip uninitialized slot " + std::to_string(i));
                continue;
            }

            bool bad = false;

            if(w.pImageInfo != nullptr)
            {
                for(uint32_t k = 0u; k < w.descriptorCount; ++k)
                {
                    if(w.pImageInfo[k].imageView == VK_NULL_HANDLE)
                    {
                        RTLogFlushed("[VK GUARD] cloud: binding " + std::to_string(w.dstBinding) + " NULL imageView");
                        bad = true;
                    }

                    if((w.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                        w.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) &&
                       w.pImageInfo[k].sampler == VK_NULL_HANDLE)
                    {
                        RTLogFlushed("[VK GUARD] cloud: binding " + std::to_string(w.dstBinding) + " NULL sampler");
                        bad = true;
                    }
                }
            }

            if(bad)
            {
                continue;
            }

            writes[safeCount++] = w;
        }

        vkUpdateDescriptorSets(g_device, safeCount, writes, 0u, nullptr);
    }
    {
        VkDescriptorSetLayoutBinding atrousBindings[3]{};
        atrousBindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        atrousBindings[1] = { 1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        atrousBindings[2] = { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo atrousSetLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };
        atrousSetLayoutInfo.bindingCount = 3u;
        atrousSetLayoutInfo.pBindings = atrousBindings;
        check(vkCreateDescriptorSetLayout(g_device, &atrousSetLayoutInfo, nullptr,
            &g_volumetricCloudAtrousSetLayout), "cloud atrous set layout failed");

        VkDescriptorPoolSize atrousPoolSizes[2] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4u },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2u }
        };
        VkDescriptorPoolCreateInfo atrousPoolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        };
        atrousPoolInfo.maxSets = 2u;
        atrousPoolInfo.poolSizeCount = 2u;
        atrousPoolInfo.pPoolSizes = atrousPoolSizes;
        check(vkCreateDescriptorPool(g_device, &atrousPoolInfo, nullptr,
            &g_volumetricCloudAtrousDescriptorPool), "cloud atrous descriptor pool failed");

        VkDescriptorSetLayout atrousSetLayouts[2] = {
            g_volumetricCloudAtrousSetLayout, g_volumetricCloudAtrousSetLayout
        };
        VkDescriptorSet atrousSets[2]{};
        VkDescriptorSetAllocateInfo atrousSetAllocation{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        atrousSetAllocation.descriptorPool = g_volumetricCloudAtrousDescriptorPool;
        atrousSetAllocation.descriptorSetCount = 2u;
        atrousSetAllocation.pSetLayouts = atrousSetLayouts;
        check(vkAllocateDescriptorSets(g_device, &atrousSetAllocation, atrousSets),
            "cloud atrous descriptor set allocation failed");
        g_volumetricCloudAtrousSetForward = atrousSets[0];
        g_volumetricCloudAtrousSetBackward = atrousSets[1];

        VkDescriptorImageInfo atrousResolvedInfo{};
        atrousResolvedInfo.sampler = g_volumetricCloudSampler;
        atrousResolvedInfo.imageView = g_volumetricCloudResolvedView;
        atrousResolvedInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo atrousRawInfo{};
        atrousRawInfo.sampler = g_volumetricCloudSampler;
        atrousRawInfo.imageView = g_volumetricCloudView;
        atrousRawInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo atrousDepthInfo{};
        atrousDepthInfo.sampler = g_volumetricCloudSampler;
        atrousDepthInfo.imageView = g_volumetricCloudDepthView;
        atrousDepthInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo atrousResolvedStorageInfo{};
        atrousResolvedStorageInfo.imageView = g_volumetricCloudResolvedView;
        atrousResolvedStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkDescriptorImageInfo atrousRawStorageInfo{};
        atrousRawStorageInfo.imageView = g_volumetricCloudView;
        atrousRawStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet atrousWrites[6]{};
        atrousWrites[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetForward, 0u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atrousResolvedInfo, nullptr, nullptr };
        atrousWrites[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetForward, 1u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atrousDepthInfo, nullptr, nullptr };
        atrousWrites[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetForward, 2u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &atrousRawStorageInfo, nullptr, nullptr };
        atrousWrites[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetBackward, 0u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atrousRawInfo, nullptr, nullptr };
        atrousWrites[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetBackward, 1u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atrousDepthInfo, nullptr, nullptr };
        atrousWrites[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_volumetricCloudAtrousSetBackward, 2u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &atrousResolvedStorageInfo, nullptr, nullptr };
        vkUpdateDescriptorSets(g_device, 6u, atrousWrites, 0u, nullptr);
    }
    g_volumetricCloudCompositeTargetView = g_rtOutputView;
    g_volumetricCloudWidth = cloudWidth;
    g_volumetricCloudHeight = cloudHeight;
    g_volumetricCloudBoundRenderWidth = g_rtOutputWidth;
    g_volumetricCloudBoundRenderHeight = g_rtOutputHeight;
    g_volumetricCloudBoundOutputWidth = g_dlssOutputWidth;
    g_volumetricCloudBoundOutputHeight = g_dlssOutputHeight;
    g_volumetricCloudLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudResolvedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryNeedsClear.store(true);
    g_cloudFogLightSetBound = VK_NULL_HANDLE;
    g_cloudFogLightListBound = VK_NULL_HANDLE;
    g_cloudFogLightGridBound = VK_NULL_HANDLE;
    g_cloudFogLightDescriptorsReady.store(false);
    ++g_rrGuideViewGeneration;
}
void createVolumetricCloudPipeline(uint32_t width, uint32_t height)
{
    const uint32_t cloudWidth = width;
    const uint32_t cloudHeight = height;
    if(g_volumetricCloudRenderPipelineLayout != VK_NULL_HANDLE &&
       volumetricCloudBindingsMatchCurrentSizes(cloudWidth, cloudHeight))
    {
        return;
    }
    createSunShadowResources();
    createSunShadowFarResources();
    createTerrainHeightResources();
    createRRGuideRelaxResources();
    createVolumetricCloudResources(width, height);
    const std::vector<uint8_t> resolveCode = loadSpv("shaders/volumetric_cloud_resolve.comp.spv");
    const std::vector<uint8_t> compositeCode = loadSpv("shaders/volumetric_cloud_composite.comp.spv");
    const std::vector<uint8_t> lightCode = loadSpv("shaders/volumetric_light.comp.spv");
    VkShaderModule resolveShader = createShaderModule(g_device, resolveCode);
    VkShaderModule compositeShader = createShaderModule(g_device, compositeCode);
    VkShaderModule lightShader = createShaderModule(g_device, lightCode);
    try
    {
        VkPipelineLayoutCreateInfo renderLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        renderLayoutInfo.setLayoutCount = 1u;
        renderLayoutInfo.pSetLayouts = &g_volumetricCloudRenderSetLayout;
        check(vkCreatePipelineLayout(g_device, &renderLayoutInfo, nullptr,
            &g_volumetricCloudRenderPipelineLayout), "volumetric cloud render pipeline layout failed");
        VkPipelineLayoutCreateInfo resolveLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        resolveLayoutInfo.setLayoutCount = 1u;
        resolveLayoutInfo.pSetLayouts = &g_volumetricCloudResolveSetLayout;
        check(vkCreatePipelineLayout(g_device, &resolveLayoutInfo, nullptr,
            &g_volumetricCloudResolvePipelineLayout), "volumetric cloud resolve pipeline layout failed");
        VkPipelineLayoutCreateInfo compositeLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        compositeLayoutInfo.setLayoutCount = 1u;
        compositeLayoutInfo.pSetLayouts = &g_volumetricCloudCompositeSetLayout;
        VkPushConstantRange compositePushRange{};
        compositePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        compositePushRange.offset = 0u;
        compositePushRange.size = sizeof(CloudCompositePush);
        compositeLayoutInfo.pushConstantRangeCount = 1u;
        compositeLayoutInfo.pPushConstantRanges = &compositePushRange;
        check(vkCreatePipelineLayout(g_device, &compositeLayoutInfo, nullptr,
            &g_volumetricCloudCompositePipelineLayout), "volumetric cloud composite pipeline layout failed");
        VkPipelineLayoutCreateInfo lightLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        lightLayoutInfo.setLayoutCount = 1u;
        lightLayoutInfo.pSetLayouts = &g_volumetricLightSetLayout;
        check(vkCreatePipelineLayout(g_device, &lightLayoutInfo, nullptr,
            &g_volumetricLightPipelineLayout), "volumetric light pipeline layout failed");
        VkPipelineShaderStageCreateInfo renderStage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        renderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        renderStage.module = resolveShader;
        renderStage.pName = "main";
        if(g_cloudV4AtlasView != VK_NULL_HANDLE &&
           g_volumetricCloudRenderPipelineV3 == VK_NULL_HANDLE)
        {
            try
            {
                const std::vector<uint8_t> renderCodeV3 = loadSpv(
                    "shaders/volumetric_clouds_v5.comp.spv");
                VkShaderModule renderShaderV3 = createShaderModule(g_device, renderCodeV3);
                VkPipelineShaderStageCreateInfo renderStageV3{
                    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
                };
                renderStageV3.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                renderStageV3.module = renderShaderV3;
                renderStageV3.pName = "main";
                VkComputePipelineCreateInfo renderPipelineInfoV3{
                    VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
                };
                renderPipelineInfoV3.stage = renderStageV3;
                renderPipelineInfoV3.layout = g_volumetricCloudRenderPipelineLayout;
                const VkResult v3Result = vkCreateComputePipelines(g_device,
                    getSharedPipelineCache(), 1u, &renderPipelineInfoV3, nullptr,
                    &g_volumetricCloudRenderPipelineV3);
                vkDestroyShaderModule(g_device, renderShaderV3, nullptr);
                if(v3Result != VK_SUCCESS)
                {
                    g_volumetricCloudRenderPipelineV3 = VK_NULL_HANDLE;
                    RTLog("[CLOUD V5] pipeline creation failed (" +
                        std::to_string(static_cast<int>(v3Result)) +
                        "), no cloud render pass this session");
                }
                else
                {
                    RTLog("[CLOUD V5] v5 cloud pipeline created");
                }
            }
            catch(const std::exception& error)
            {
                g_volumetricCloudRenderPipelineV3 = VK_NULL_HANDLE;
                RTLog(std::string("[CLOUD V5] shader unavailable (") + error.what() +
                    "), no cloud render pass this session");
            }
        }
        VkPipelineShaderStageCreateInfo resolveStage = renderStage;
        resolveStage.module = resolveShader;
        VkComputePipelineCreateInfo resolvePipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        resolvePipelineInfo.stage = resolveStage;
        resolvePipelineInfo.layout = g_volumetricCloudResolvePipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u, &resolvePipelineInfo,
            nullptr, &g_volumetricCloudResolvePipeline), "volumetric cloud resolve pipeline failed");
        VkPipelineShaderStageCreateInfo compositeStage = renderStage;
        compositeStage.module = compositeShader;
        VkComputePipelineCreateInfo compositePipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        compositePipelineInfo.stage = compositeStage;
        compositePipelineInfo.layout = g_volumetricCloudCompositePipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u, &compositePipelineInfo,
            nullptr, &g_volumetricCloudCompositePipeline), "volumetric cloud composite pipeline failed");
        VkPipelineShaderStageCreateInfo lightStage = renderStage;
        lightStage.module = lightShader;
        VkComputePipelineCreateInfo lightPipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        lightPipelineInfo.stage = lightStage;
        lightPipelineInfo.layout = g_volumetricLightPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u, &lightPipelineInfo,
            nullptr, &g_volumetricLightPipeline), "volumetric light pipeline failed");

        const std::vector<uint8_t> godRayCompositeCode =
            loadSpv("shaders/god_ray_composite.comp.spv");
        VkShaderModule godRayCompositeShader =
            createShaderModule(g_device, godRayCompositeCode);
        VkPipelineLayoutCreateInfo godRayLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        godRayLayoutInfo.setLayoutCount = 1u;
        godRayLayoutInfo.pSetLayouts = &g_godRayCompositeSetLayout;
        check(vkCreatePipelineLayout(g_device, &godRayLayoutInfo, nullptr,
            &g_godRayCompositePipelineLayout), "god ray composite pipeline layout failed");
        VkPipelineShaderStageCreateInfo godRayStage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };
        godRayStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        godRayStage.module = godRayCompositeShader;
        godRayStage.pName = "main";
        VkComputePipelineCreateInfo godRayPipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        godRayPipelineInfo.stage = godRayStage;
        godRayPipelineInfo.layout = g_godRayCompositePipelineLayout;
        const VkResult godRayPipelineResult = vkCreateComputePipelines(
            g_device, getSharedPipelineCache(), 1u, &godRayPipelineInfo, nullptr,
            &g_godRayCompositePipeline);
        vkDestroyShaderModule(g_device, godRayCompositeShader, nullptr);
        check(godRayPipelineResult, "god ray composite pipeline failed");

        {
            const std::vector<uint8_t> atrousCode =
                loadSpv("shaders/volumetric_cloud_atrous.comp.spv");
            VkShaderModule atrousShader = createShaderModule(g_device, atrousCode);
            VkPushConstantRange atrousPushRange{};
            atrousPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            atrousPushRange.offset = 0u;
            atrousPushRange.size = 16u;
            VkPipelineLayoutCreateInfo atrousPipelineLayoutInfo{
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
            };
            atrousPipelineLayoutInfo.setLayoutCount = 1u;
            atrousPipelineLayoutInfo.pSetLayouts = &g_volumetricCloudAtrousSetLayout;
            atrousPipelineLayoutInfo.pushConstantRangeCount = 1u;
            atrousPipelineLayoutInfo.pPushConstantRanges = &atrousPushRange;
            check(vkCreatePipelineLayout(g_device, &atrousPipelineLayoutInfo, nullptr,
                &g_volumetricCloudAtrousPipelineLayout), "cloud atrous pipeline layout failed");
            VkPipelineShaderStageCreateInfo atrousStage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
            };
            atrousStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            atrousStage.module = atrousShader;
            atrousStage.pName = "main";
            VkComputePipelineCreateInfo atrousPipelineInfo{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
            };
            atrousPipelineInfo.stage = atrousStage;
            atrousPipelineInfo.layout = g_volumetricCloudAtrousPipelineLayout;
            const VkResult atrousPipelineResult = vkCreateComputePipelines(
                g_device, getSharedPipelineCache(), 1u, &atrousPipelineInfo, nullptr,
                &g_volumetricCloudAtrousPipeline);
            vkDestroyShaderModule(g_device, atrousShader, nullptr);
            check(atrousPipelineResult, "cloud atrous pipeline failed");
        }
    }
    catch(...)
    {
            vkDestroyShaderModule(g_device, resolveShader, nullptr);
        vkDestroyShaderModule(g_device, compositeShader, nullptr);
        vkDestroyShaderModule(g_device, lightShader, nullptr);
        destroyVolumetricCloudResources();
        throw;
    }
    vkDestroyShaderModule(g_device, resolveShader, nullptr);
    vkDestroyShaderModule(g_device, compositeShader, nullptr);
    vkDestroyShaderModule(g_device, lightShader, nullptr);
    RTLog("[CLOUD] full-resolution world-anchored render, temporal resolve, and composite pipelines created");
}

// 把云合成 pass 的目标图（描述符 binding 1）指向 target，只在真的变了才写一次
// 描述符。两条路径共用同一个描述符集：旧档指向 g_rtOutputView（RR 的输入），
// 远云剥离档指向 g_dlssOutputView（RR 的输出，见 recordCloudComposite），
// 所以模式来回切时必须两边都来改绑，否则会把云合成到上一档的图里。
void setCloudCompositeTarget(VkImageView target)
{
    if(target == VK_NULL_HANDLE ||
       g_volumetricCloudCompositeSet == VK_NULL_HANDLE ||
       g_volumetricCloudCompositeTargetView == target)
    {
        return;
    }
    VkDescriptorImageInfo targetInfo{};
    targetInfo.imageView = target;
    targetInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet targetWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    targetWrite.dstSet = g_volumetricCloudCompositeSet;
    targetWrite.dstBinding = 1u;
    targetWrite.descriptorCount = 1u;
    targetWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    targetWrite.pImageInfo = &targetInfo;
    vkUpdateDescriptorSets(g_device, 1u, &targetWrite, 0u, nullptr);
    g_volumetricCloudCompositeTargetView = target;
}

// ═══════════════════════════════════════════════════════════════════════════
// 2026-10-27 云着色器**热重载**：改完 SPV 不用再冷启游戏。
//
// 为什么原来必须重启：管线只在 createVolumetricCloudPipeline() 里建一次，而那个
// 函数被 volumetricCloudBindingsMatchCurrentSizes() 挡住（尺寸不变就直接 return）。
//
// 这里的做法：每 ~20 帧 stat 一次这 4 个 SPV 的修改时间，一变就用**现有布局**
// 重建对应的 compute pipeline，并清掉云的时域历史（避免旧画面残留）。
//
// 覆盖范围：volumetric_clouds_v5 / cloud_resolve / cloud_composite / volumetric_light
// —— 也就是"改密度装配、廓形、受光、合成"这些都能即时看。
// 不覆盖：cloud_v5_noise（烘焙器）。改 hpRenderBase/Detail/Weather 这类**烘焙函数**
// 仍需要重启（3D 体积在启动时烘一次，重烘要重建 image + 描述符，风险大不值当）。
// ═══════════════════════════════════════════════════════════════════════════
uint64_t cloudSpvStampOf(const char* relative)
{
    const std::string path = ResolveRTXRuntimePath(relative);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
    {
        return 0ull;
    }
    return (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32)
        | static_cast<uint64_t>(data.ftLastWriteTime.dwLowDateTime);
}
uint64_t cloudSpvStampAll()
{
    static const char* const kFiles[4] = {
        "shaders/volumetric_clouds_v5.comp.spv",
        "shaders/volumetric_cloud_resolve.comp.spv",
        "shaders/volumetric_cloud_composite.comp.spv",
        "shaders/volumetric_light.comp.spv",
    };
    uint64_t stamp = 1469598103934665603ull;
    for(int i = 0; i < 4; ++i)
    {
        stamp ^= cloudSpvStampOf(kFiles[i]);
        stamp *= 1099511628211ull;
    }
    return stamp;
}
void hotReloadVolumetricCloudPipelines()
{
    if(g_device == VK_NULL_HANDLE) return;
    if(g_volumetricCloudRenderPipelineLayout == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(g_device);

    bool any = false;
    auto replace = [&any](const char* spv, VkPipelineLayout layout,
                          VkPipeline* target, const char* tag)
    {
        try
        {
            const std::vector<uint8_t> code = loadSpv(spv);
            VkShaderModule module = createShaderModule(g_device, code);
            VkPipelineShaderStageCreateInfo stage{
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
            };
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = module;
            stage.pName = "main";
            VkComputePipelineCreateInfo info{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
            };
            info.stage = stage;
            info.layout = layout;
            VkPipeline fresh = VK_NULL_HANDLE;
            const VkResult result = vkCreateComputePipelines(g_device,
                getSharedPipelineCache(), 1u, &info, nullptr, &fresh);
            vkDestroyShaderModule(g_device, module, nullptr);
            if(result == VK_SUCCESS && fresh != VK_NULL_HANDLE)
            {
                if(*target != VK_NULL_HANDLE)
                {
                    vkDestroyPipeline(g_device, *target, nullptr);
                }
                *target = fresh;
                any = true;
                RTLog(std::string("[CLOUD HOT] reloaded ") + tag);
            }
            else
            {
                RTLog(std::string("[CLOUD HOT] create failed ") + tag +
                    " (" + std::to_string(static_cast<int>(result)) + ")，保留旧管线");
            }
        }
        catch(const std::exception& error)
        {
            RTLog(std::string("[CLOUD HOT] ") + tag + " 读取失败: " + error.what());
        }
    };

    replace("shaders/volumetric_clouds_v5.comp.spv",
        g_volumetricCloudRenderPipelineLayout, &g_volumetricCloudRenderPipelineV3, "render");
    replace("shaders/volumetric_cloud_resolve.comp.spv",
        g_volumetricCloudResolvePipelineLayout, &g_volumetricCloudResolvePipeline, "resolve");
    replace("shaders/volumetric_cloud_composite.comp.spv",
        g_volumetricCloudCompositePipelineLayout, &g_volumetricCloudCompositePipeline, "composite");
    replace("shaders/volumetric_light.comp.spv",
        g_volumetricLightPipelineLayout, &g_volumetricLightPipeline, "light");

    if(any)
    {
        g_volumetricCloudHistoryNeedsClear.store(true);
        RTLog("[CLOUD HOT] cloud history cleared");
    }
}
void maybeHotReloadVolumetricCloudShaders()
{
    if(++g_cloudSpvWatchCountdown < 20) return;
    g_cloudSpvWatchCountdown = 0;
    const uint64_t now = cloudSpvStampAll();
    if(g_cloudSpvStampSeen == 0ull)
    {
        g_cloudSpvStampSeen = now;
        return;
    }
    if(now == g_cloudSpvStampSeen) return;
    g_cloudSpvStampSeen = now;
    hotReloadVolumetricCloudPipelines();
}
void recordVolumetricClouds(VkCommandBuffer cmd)
{
    maybeHotReloadVolumetricCloudShaders();
    if(cmd == VK_NULL_HANDLE || g_volumetricCloudRenderPipelineV3 == VK_NULL_HANDLE ||
       g_volumetricCloudResolvePipeline == VK_NULL_HANDLE ||
       g_volumetricCloudCompositePipeline == VK_NULL_HANDLE)
    {
        if(cmd != VK_NULL_HANDLE)
        {
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SNEAR);
            writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_HEIGHT);
        }

        return;
    }

    updateCloudLocalLightDescriptors();
    recordSunShadowMap(cmd);

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_SNEAR);

    recordTerrainHeightMap(cmd);

    writePerfL10GpuBoundary(cmd, PERF_L10_GPU_B_HEIGHT);
    VkImageMemoryBarrier cloudBarriers[3]{};
    cloudBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    cloudBarriers[0].oldLayout = g_volumetricCloudLayout;
    cloudBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    cloudBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cloudBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    cloudBarriers[0].srcAccessMask = g_volumetricCloudLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : VK_ACCESS_SHADER_READ_BIT;
    cloudBarriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    cloudBarriers[0].image = g_volumetricCloudImage;
    cloudBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cloudBarriers[0].subresourceRange.levelCount = 1u;
    cloudBarriers[0].subresourceRange.layerCount = 1u;
    cloudBarriers[1] = cloudBarriers[0];
    cloudBarriers[1].oldLayout = g_volumetricCloudDepthLayout;
    cloudBarriers[1].srcAccessMask = g_volumetricCloudDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : VK_ACCESS_SHADER_READ_BIT;
    cloudBarriers[1].image = g_volumetricCloudDepthImage;
    cloudBarriers[2] = cloudBarriers[0];
    cloudBarriers[2].oldLayout = g_volumetricCloudResolvedLayout;
    cloudBarriers[2].srcAccessMask = g_volumetricCloudResolvedLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : VK_ACCESS_SHADER_READ_BIT;
    cloudBarriers[2].image = g_volumetricCloudResolvedImage;
    vkCmdPipelineBarrier(cmd,
        g_volumetricCloudLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            g_volumetricCloudDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            g_volumetricCloudResolvedLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 3u, cloudBarriers);
    g_volumetricCloudLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_volumetricCloudDepthLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_volumetricCloudResolvedLayout = VK_IMAGE_LAYOUT_GENERAL;

    if(g_volumetricCloudHistoryNeedsClear.exchange(false))
    {
        VkImageMemoryBarrier historyBarriers[2]{};
        historyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        historyBarriers[0].oldLayout = g_volumetricCloudHistoryLayout;
        historyBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        historyBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        historyBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        historyBarriers[0].srcAccessMask = g_volumetricCloudHistoryLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0u : VK_ACCESS_SHADER_READ_BIT;
        historyBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        historyBarriers[0].image = g_volumetricCloudHistoryImage;
        historyBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        historyBarriers[0].subresourceRange.levelCount = 1u;
        historyBarriers[0].subresourceRange.layerCount = 1u;
        historyBarriers[1] = historyBarriers[0];
        historyBarriers[1].oldLayout = g_volumetricCloudHistoryDepthLayout;
        historyBarriers[1].srcAccessMask = g_volumetricCloudHistoryDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0u : VK_ACCESS_SHADER_READ_BIT;
        historyBarriers[1].image = g_volumetricCloudHistoryDepthImage;
        vkCmdPipelineBarrier(cmd,
            g_volumetricCloudHistoryLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                g_volumetricCloudHistoryDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED
                ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, historyBarriers);
        const VkClearColorValue emptyCloud = {{0.0f, 0.0f, 0.0f, 1.0f}};
        vkCmdClearColorImage(cmd, g_volumetricCloudHistoryImage, VK_IMAGE_LAYOUT_GENERAL,
            &emptyCloud, 1u, &historyBarriers[0].subresourceRange);
        const VkClearColorValue emptyCloudDepth = {{1000000.0f, 0.0f, 0.0f, 0.0f}};
        vkCmdClearColorImage(cmd, g_volumetricCloudHistoryDepthImage, VK_IMAGE_LAYOUT_GENERAL,
            &emptyCloudDepth, 1u, &historyBarriers[1].subresourceRange);
        for(uint32_t i = 0u; i < 2u; ++i)
        {
            historyBarriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            historyBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, historyBarriers);
        g_volumetricCloudHistoryLayout = VK_IMAGE_LAYOUT_GENERAL;
        g_volumetricCloudHistoryDepthLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkImageMemoryBarrier sceneBarriers[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        sceneBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sceneBarriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        sceneBarriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        sceneBarriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sceneBarriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sceneBarriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        sceneBarriers[i].dstAccessMask = i == 0u
            ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
            : VK_ACCESS_SHADER_READ_BIT;
        sceneBarriers[i].image = i == 0u ? g_rtOutputImage : g_dlssDepthImage;
        sceneBarriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        sceneBarriers[i].subresourceRange.levelCount = 1u;
        sceneBarriers[i].subresourceRange.layerCount = 1u;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, sceneBarriers);

    const VkPipeline cloudRenderPipeline = g_volumetricCloudRenderPipelineV3;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudRenderPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volumetricCloudRenderPipelineLayout, 0u, 1u, &g_volumetricCloudRenderSet,
        0u, nullptr);
    vkCmdDispatch(cmd, (g_volumetricCloudWidth + 7u) / 8u,
        (g_volumetricCloudHeight + 7u) / 8u, 1u);

    VkImageMemoryBarrier resolveBarriers[2]{};
    resolveBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    resolveBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    resolveBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    resolveBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resolveBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    resolveBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    resolveBarriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    resolveBarriers[0].image = g_volumetricCloudImage;
    resolveBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    resolveBarriers[0].subresourceRange.levelCount = 1u;
    resolveBarriers[0].subresourceRange.layerCount = 1u;
    resolveBarriers[1] = resolveBarriers[0];
    resolveBarriers[1].image = g_volumetricCloudDepthImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, resolveBarriers);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volumetricCloudResolvePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volumetricCloudResolvePipelineLayout, 0u, 1u, &g_volumetricCloudResolveSet,
        0u, nullptr);
    vkCmdDispatch(cmd, (g_volumetricCloudWidth + 7u) / 8u,
        (g_volumetricCloudHeight + 7u) / 8u, 1u);

    if(g_cloudPostRrComposite.load() &&
       kCloudAtrousIterations > 0u &&
       g_volumetricCloudAtrousPipeline != VK_NULL_HANDLE &&
       g_volumetricCloudAtrousSetForward != VK_NULL_HANDLE &&
       g_volumetricCloudAtrousSetBackward != VK_NULL_HANDLE &&
       g_volumetricCloudWidth > 0u && g_volumetricCloudHeight > 0u)
    {
        static const int atrousStrides[8] = { 1, 2, 4, 8, 16, 32, 64, 128 };
        const uint32_t atrousGroupsX = (g_volumetricCloudWidth + 7u) / 8u;
        const uint32_t atrousGroupsY = (g_volumetricCloudHeight + 7u) / 8u;
        for(uint32_t iteration = 0u; iteration < kCloudAtrousIterations; ++iteration)
        {
            VkMemoryBarrier atrousBarrier{};
            atrousBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            atrousBarrier.srcAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            atrousBarrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &atrousBarrier,
                0u, nullptr, 0u, nullptr);

            const VkDescriptorSet atrousSet = (iteration & 1u) == 0u
                ? g_volumetricCloudAtrousSetForward
                : g_volumetricCloudAtrousSetBackward;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                g_volumetricCloudAtrousPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                g_volumetricCloudAtrousPipelineLayout, 0u, 1u, &atrousSet, 0u, nullptr);
            struct CloudAtrousPush
            {
                int32_t stride;
                float phiDepthRel;
                float phiAlpha;
                float phiLum;
            } atrousPush;
            atrousPush.stride = atrousStrides[iteration];
            atrousPush.phiDepthRel = 0.06f;
            atrousPush.phiAlpha = 0.20f;
            atrousPush.phiLum = 1.5f;
            vkCmdPushConstants(cmd, g_volumetricCloudAtrousPipelineLayout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(atrousPush), &atrousPush);
            vkCmdDispatch(cmd, atrousGroupsX, atrousGroupsY, 1u);
        }
    }

    VkImageMemoryBarrier copyBarriers[4]{};
    copyBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    copyBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    copyBarriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    copyBarriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    copyBarriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    copyBarriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    copyBarriers[0].image = g_volumetricCloudResolvedImage;
    copyBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyBarriers[0].subresourceRange.levelCount = 1u;
    copyBarriers[0].subresourceRange.layerCount = 1u;
    copyBarriers[1] = copyBarriers[0];
    copyBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    copyBarriers[1].image = g_volumetricCloudDepthImage;
    copyBarriers[2] = copyBarriers[0];
    copyBarriers[2].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    copyBarriers[2].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copyBarriers[2].image = g_volumetricCloudHistoryImage;
    copyBarriers[3] = copyBarriers[2];
    copyBarriers[3].image = g_volumetricCloudHistoryDepthImage;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 4u, copyBarriers);

    VkImageCopy historyCopy{};
    historyCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    historyCopy.srcSubresource.layerCount = 1u;
    historyCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    historyCopy.dstSubresource.layerCount = 1u;
    historyCopy.extent = {g_volumetricCloudWidth, g_volumetricCloudHeight, 1u};
    vkCmdCopyImage(cmd, g_volumetricCloudResolvedImage, VK_IMAGE_LAYOUT_GENERAL,
        g_volumetricCloudHistoryImage, VK_IMAGE_LAYOUT_GENERAL, 1u, &historyCopy);
    vkCmdCopyImage(cmd, g_volumetricCloudDepthImage, VK_IMAGE_LAYOUT_GENERAL,
        g_volumetricCloudHistoryDepthImage, VK_IMAGE_LAYOUT_GENERAL, 1u, &historyCopy);

    for(uint32_t i = 0u; i < 2u; ++i)
    {
        copyBarriers[i].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        copyBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    for(uint32_t i = 2u; i < 4u; ++i)
    {
        copyBarriers[i].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        copyBarriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 4u, copyBarriers);

    if(!g_cloudPostRrComposite.load() && g_aeStateBuffer != VK_NULL_HANDLE)
    {
        VkBufferMemoryBarrier aeStateBarrier{};
        aeStateBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        aeStateBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aeStateBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        aeStateBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        aeStateBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        aeStateBarrier.buffer = g_aeStateBuffer;
        aeStateBarrier.offset = 0u;
        aeStateBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 1u,
            &aeStateBarrier, 0u, nullptr);
    }

    if(!g_cloudPostRrComposite.load())
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volumetricCloudCompositePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_volumetricCloudCompositePipelineLayout, 0u, 1u, &g_volumetricCloudCompositeSet,
            0u, nullptr);
        CloudCompositePush compositePush{};
        compositePush.splitDistance =
            cloudFarBandSplitActive() ? cloudFarBandSplitDistance() : 0.0f;
        compositePush.mode = 0.0f;
        vkCmdPushConstants(cmd, g_volumetricCloudCompositePipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(compositePush), &compositePush);
        vkCmdDispatch(cmd, (g_rtOutputWidth + 7u) / 8u,
            (g_rtOutputHeight + 7u) / 8u, 1u);
    }

    // 2026-11-01：这里原来整块被 `volRestirEnabled() == 0u` 门控 —— 于是 ReSTIR
    // 一开，god_ray_composite 也一起被跳过，而它是**唯一**把 g_godRayTargetImage
    // 变成像素的地方。结果：recordVolumeRestirScreenComposite 写进去的 ReSTIR 体积
    // 光没有任何消费者，整条链对画面贡献为 0；屏幕上看到的局部光只能来自
    // volumetric_fog.glsl 那条没有遮挡判定的旧路径 —— 也就是"漏光"。
    // 现在拆成两层：体积光 march 仍只在旧路径跑（ReSTIR 模式下由 vol_restir_* 取代），
    // god_ray_composite 两种模式都执行。
    const bool volRestirCompositeReady =
        (volRestirEnabled() == 0u ||
         (g_volRestirCompositePipeline != VK_NULL_HANDLE &&
          g_volRestirCompositeSet != VK_NULL_HANDLE &&
          g_volRestirIntegratePipeline != VK_NULL_HANDLE));

    if(g_godRayEnabled.load() && g_tlas != VK_NULL_HANDLE &&
       volRestirCompositeReady &&
       g_godRayCompositePipeline != VK_NULL_HANDLE &&
       g_godRayCompositeSet != VK_NULL_HANDLE &&
       g_godRayTargetImage != VK_NULL_HANDLE &&
       g_godRayWidth > 0u && g_godRayHeight > 0u)
    {
        VkImageMemoryBarrier godRayBarriers[2]{};
        for(VkImageMemoryBarrier& barrier : godRayBarriers)
        {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = 1u;
            barrier.subresourceRange.layerCount = 1u;
        }
        godRayBarriers[0].srcAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        godRayBarriers[0].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        godRayBarriers[0].image = g_godRayTargetImage;
        godRayBarriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        godRayBarriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        godRayBarriers[1].image = g_godRayHistoryImage;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u,
            godRayBarriers);

        if(g_godRayHistoryNeedsClear.exchange(false) &&
           g_godRayHistoryImage != VK_NULL_HANDLE)
        {
            VkImageSubresourceRange godRayRange{};
            godRayRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            godRayRange.levelCount = 1u;
            godRayRange.layerCount = 1u;
            const VkClearColorValue godRayNeutral{{0.0f, 0.0f, 0.0f, 1.0f}};
            vkCmdClearColorImage(cmd, g_godRayHistoryImage,
                VK_IMAGE_LAYOUT_GENERAL, &godRayNeutral, 1u, &godRayRange);
            VkImageMemoryBarrier godRayClearBarrier{};
            godRayClearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            godRayClearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            godRayClearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            godRayClearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            godRayClearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            godRayClearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            godRayClearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            godRayClearBarrier.image = g_godRayHistoryImage;
            godRayClearBarrier.subresourceRange = godRayRange;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
                &godRayClearBarrier);
        }

        {
            static VkDescriptorSet lastGodRayTlasSet = VK_NULL_HANDLE;
            static VkAccelerationStructureKHR lastGodRayTlas = VK_NULL_HANDLE;

            if(g_tlas != VK_NULL_HANDLE &&
               (g_volumetricLightSet != lastGodRayTlasSet ||
                g_tlas != lastGodRayTlas))
            {
                VkAccelerationStructureKHR tlasHandle = g_tlas;
                VkWriteDescriptorSetAccelerationStructureKHR tlasDescriptor{};
                tlasDescriptor.sType =
                    VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                tlasDescriptor.accelerationStructureCount = 1u;
                tlasDescriptor.pAccelerationStructures = &tlasHandle;

                VkWriteDescriptorSet tlasWrite{};
                tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                tlasWrite.pNext = &tlasDescriptor;
                tlasWrite.dstSet = g_volumetricLightSet;
                tlasWrite.dstBinding = 9u;
                tlasWrite.descriptorCount = 1u;
                tlasWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

                vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);

                RTLog(
                    "[GODRAY TLAS] binding 9 rebound set=" +
                    std::to_string(
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(g_volumetricLightSet))) +
                    " tlas=" +
                    std::to_string(
                        static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(g_tlas))) +
                    " instances=" +
                    std::to_string(g_tlasInstanceCount)
                );

                lastGodRayTlasSet = g_volumetricLightSet;
                lastGodRayTlas = g_tlas;
            }
        }

        {
            VkMemoryBarrier asReadBarrier{};
            asReadBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            asReadBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            asReadBarrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;

            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0u,
                1u,
                &asReadBarrier,
                0u,
                nullptr,
                0u,
                nullptr
            );
        }

        // 射线自检只在旧体积光路径上跑：ReSTIR 模式保持原样跳过，避免每帧多一次
        // 1x1 dispatch + host 可见内存的 barrier。
        if(volRestirEnabled() == 0u)
        {
            static VkBuffer selfTestBuffer = VK_NULL_HANDLE;
            static VkDeviceMemory selfTestMemory = VK_NULL_HANDLE;
            static float* selfTestMapped = nullptr;
            static VkDescriptorSetLayout selfTestSetLayout = VK_NULL_HANDLE;
            static VkDescriptorPool selfTestPool = VK_NULL_HANDLE;
            static VkDescriptorSet selfTestSet = VK_NULL_HANDLE;
            static VkPipelineLayout selfTestPipelineLayout = VK_NULL_HANDLE;
            static VkPipeline selfTestPipeline = VK_NULL_HANDLE;
            static bool selfTestBroken = false;
            static VkDescriptorSet selfTestBoundSet = VK_NULL_HANDLE;
            static VkAccelerationStructureKHR selfTestBoundTlas = VK_NULL_HANDLE;
            static uint32_t selfTestLastLoggedSerial = 0xFFFFFFFFu;

            if(!selfTestBroken && selfTestPipeline == VK_NULL_HANDLE)
            {
                try
                {
                    createBuffer(
                        g_device,
                        g_physicalDevice,
                        64u,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        selfTestBuffer,
                        selfTestMemory
                    );

                    void* mapped = nullptr;
                    check(
                        vkMapMemory(g_device, selfTestMemory, 0u, VK_WHOLE_SIZE, 0u,
                            &mapped),
                        "ray self test map failed"
                    );
                    selfTestMapped = static_cast<float*>(mapped);

                    VkDescriptorSetLayoutBinding bindings[3]{};
                    bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                    bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
                    bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
                        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

                    VkDescriptorSetLayoutCreateInfo setLayoutInfo{
                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
                    };
                    setLayoutInfo.bindingCount = 3u;
                    setLayoutInfo.pBindings = bindings;
                    check(
                        vkCreateDescriptorSetLayout(g_device, &setLayoutInfo, nullptr,
                            &selfTestSetLayout),
                        "ray self test set layout failed"
                    );

                    VkDescriptorPoolSize poolSizes[3] = {
                        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
                        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u },
                        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u }
                    };
                    VkDescriptorPoolCreateInfo poolInfo{
                        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
                    };
                    poolInfo.maxSets = 1u;
                    poolInfo.poolSizeCount = 3u;
                    poolInfo.pPoolSizes = poolSizes;
                    check(
                        vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
                            &selfTestPool),
                        "ray self test pool failed"
                    );

                    VkDescriptorSetAllocateInfo allocInfo{
                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
                    };
                    allocInfo.descriptorPool = selfTestPool;
                    allocInfo.descriptorSetCount = 1u;
                    allocInfo.pSetLayouts = &selfTestSetLayout;
                    check(
                        vkAllocateDescriptorSets(g_device, &allocInfo, &selfTestSet),
                        "ray self test descriptor set failed"
                    );

                    VkPipelineLayoutCreateInfo layoutInfo{
                        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
                    };
                    layoutInfo.setLayoutCount = 1u;
                    layoutInfo.pSetLayouts = &selfTestSetLayout;
                    check(
                        vkCreatePipelineLayout(g_device, &layoutInfo, nullptr,
                            &selfTestPipelineLayout),
                        "ray self test pipeline layout failed"
                    );

                    const std::vector<uint8_t> code =
                        loadSpv("shaders/ray_selftest.comp.spv");
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
                    pipelineInfo.layout = selfTestPipelineLayout;

                    const VkResult pipelineResult = vkCreateComputePipelines(
                        g_device,
                        getSharedPipelineCache(),
                        1u,
                        &pipelineInfo,
                        nullptr,
                        &selfTestPipeline
                    );
                    vkDestroyShaderModule(g_device, module, nullptr);
                    check(pipelineResult, "ray self test pipeline failed");

                    RTLog("[RAYTEST] pipeline ready");
                }
                catch(...)
                {
                    selfTestBroken = true;
                    RTLog("[RAYTEST] setup failed (见 stderr)");
                }
            }

            if(selfTestPipeline != VK_NULL_HANDLE &&
               g_tlas != VK_NULL_HANDLE &&
               g_cameraUboBuffer != VK_NULL_HANDLE &&
               selfTestMapped != nullptr)
            {
                if(selfTestSet != selfTestBoundSet || g_tlas != selfTestBoundTlas)
                {
                    VkAccelerationStructureKHR tlasHandle = g_tlas;
                    VkWriteDescriptorSetAccelerationStructureKHR asi{
                        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
                    };
                    asi.accelerationStructureCount = 1u;
                    asi.pAccelerationStructures = &tlasHandle;

                    VkDescriptorBufferInfo resultInfo{};
                    resultInfo.buffer = selfTestBuffer;
                    resultInfo.offset = 0u;
                    resultInfo.range = VK_WHOLE_SIZE;

                    VkDescriptorBufferInfo cameraInfo{};
                    cameraInfo.buffer = g_cameraUboBuffer;
                    cameraInfo.offset = 0u;
                    cameraInfo.range = VK_WHOLE_SIZE;

                    VkWriteDescriptorSet writes[3]{};
                    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asi,
                        selfTestSet, 0u, 0u, 1u,
                        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                        nullptr, nullptr, nullptr };
                    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                        selfTestSet, 1u, 0u, 1u,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &resultInfo, nullptr };
                    writes[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                        selfTestSet, 2u, 0u, 1u,
                        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };

                    vkUpdateDescriptorSets(g_device, 3u, writes, 0u, nullptr);

                    selfTestBoundSet = selfTestSet;
                    selfTestBoundTlas = g_tlas;
                }

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, selfTestPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                    selfTestPipelineLayout, 0u, 1u, &selfTestSet, 0u, nullptr);
                vkCmdDispatch(cmd, 1u, 1u, 1u);

                VkMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                hostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_HOST_BIT, 0u, 1u, &hostBarrier, 0u, nullptr, 0u,
                    nullptr);

                if(g_perfL10FrameSerial % 600u == 0u &&
                   selfTestLastLoggedSerial != g_perfL10FrameSerial)
                {
                    selfTestLastLoggedSerial = g_perfL10FrameSerial;

                    RTLog(
                        "[RAYTEST] camY=" +
                        std::to_string(selfTestMapped[7]) +
                        " | 向下射线命中(1/0) m04[TS]=" +
                        std::to_string(selfTestMapped[0]) +
                        " m04[T]=" + std::to_string(selfTestMapped[1]) +
                        " m04[none]=" + std::to_string(selfTestMapped[2]) +
                        " mFF[TS]=" + std::to_string(selfTestMapped[3]) +
                        " mFF[T]=" + std::to_string(selfTestMapped[4]) +
                        " mFF[none]=" + std::to_string(selfTestMapped[5]) +
                        " | 组合0命中距离=" + std::to_string(selfTestMapped[6])
                    );
                }
            }
        }

        if(volRestirEnabled() == 0u &&
           g_volumetricLightPipeline != VK_NULL_HANDLE &&
           g_volumetricLightSet != VK_NULL_HANDLE)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volumetricLightPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                g_volumetricLightPipelineLayout, 0u, 1u, &g_volumetricLightSet, 0u, nullptr);
            vkCmdDispatch(cmd, (g_godRayWidth + 7u) / 8u, (g_godRayHeight + 7u) / 8u, 1u);
        }

        VkImageMemoryBarrier godRayReadBarrier{};
        godRayReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        godRayReadBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        godRayReadBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        godRayReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        godRayReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        godRayReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        godRayReadBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        godRayReadBarrier.image = g_godRayTargetImage;
        godRayReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        godRayReadBarrier.subresourceRange.levelCount = 1u;
        godRayReadBarrier.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &godRayReadBarrier);

        VkImageMemoryBarrier godRaySceneBarrier = godRayReadBarrier;
        godRaySceneBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        godRaySceneBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        godRaySceneBarrier.image = g_rtOutputImage;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
            &godRaySceneBarrier);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_godRayCompositePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_godRayCompositePipelineLayout, 0u, 1u, &g_godRayCompositeSet, 0u, nullptr);
        vkCmdDispatch(cmd, (g_rtOutputWidth + 7u) / 8u, (g_rtOutputHeight + 7u) / 8u, 1u);

        if(volRestirEnabled() == 0u &&
           g_godRayHistoryImage != VK_NULL_HANDLE &&
           g_godRayWidth == g_rtOutputWidth &&
           g_godRayHeight == g_rtOutputHeight)
        {
            VkImageCopy godRayCopy{};
            godRayCopy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            godRayCopy.srcSubresource.layerCount = 1u;
            godRayCopy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            godRayCopy.dstSubresource.layerCount = 1u;
            godRayCopy.extent = { g_godRayWidth, g_godRayHeight, 1u };
            vkCmdCopyImage(cmd, g_godRayTargetImage, VK_IMAGE_LAYOUT_GENERAL,
                g_godRayHistoryImage, VK_IMAGE_LAYOUT_GENERAL, 1u, &godRayCopy);
        }
    }
}

// =====================================================
// 2026-10-06 远云剥离 RR：云合成搬到 RR evaluate 之后
//
// 与 recordVolumetricClouds 尾部那一段是同一个 pass（同一套 pipeline、描述符集
// 与着色器），差别只有两点：目标图从 RR 的**输入**（g_rtOutputImage，render
// 分辨率）换成 RR 的**输出**（g_dlssOutputView，输出分辨率），dispatch 尺寸跟着
// 换。云层这时也在输出分辨率（见主循环对 createVolumetricCloudPipeline 的调用），
// 所以是 1:1 合成，完全不经过 RR 的上采样 ⇒ 清晰度与 RR 档位无关。
//
// 只能录在 g_slEvaluateFeature 之后：在那之前 g_dlssOutputView 里还是上一帧的内容。
// =====================================================
void recordCloudComposite(VkCommandBuffer cmd)
{
    if(!(g_cloudPostRrComposite.load() || cloudFarBandSplitActive()))
    {
        return;
    }
    if(cmd == VK_NULL_HANDLE ||
       g_volumetricCloudCompositePipeline == VK_NULL_HANDLE ||
       g_volumetricCloudCompositePipelineLayout == VK_NULL_HANDLE ||
       g_volumetricCloudCompositeSet == VK_NULL_HANDLE ||
       g_volumetricCloudResolvedImage == VK_NULL_HANDLE ||
       g_dlssOutputImage == VK_NULL_HANDLE ||
       g_dlssOutputView == VK_NULL_HANDLE ||
       g_dlssOutputWidth == 0u || g_dlssOutputHeight == 0u)
    {
        return;
    }


    VkImageMemoryBarrier barriers[2]{};
    for(uint32_t i = 0u; i < 2u; ++i)
    {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.levelCount = 1u;
        barriers[i].subresourceRange.layerCount = 1u;
        barriers[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    barriers[0].image = g_volumetricCloudResolvedImage;
    barriers[1].image = g_dlssOutputImage;
    barriers[1].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr,
        2u, barriers);

    if(g_aeStateBuffer != VK_NULL_HANDLE)
    {
        VkBufferMemoryBarrier aeStateBarrier{};
        aeStateBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        aeStateBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aeStateBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        aeStateBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        aeStateBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        aeStateBarrier.buffer = g_aeStateBuffer;
        aeStateBarrier.offset = 0u;
        aeStateBarrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 1u,
            &aeStateBarrier, 0u, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_volumetricCloudCompositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_volumetricCloudCompositePipelineLayout, 0u, 1u, &g_volumetricCloudCompositeSet,
        0u, nullptr);
    CloudCompositePush compositePush{};
    compositePush.splitDistance =
        cloudFarBandSplitActive() ? cloudFarBandSplitDistance() : 0.0f;
    compositePush.mode = cloudFarBandSplitActive() ? 1.0f : 0.0f;
    vkCmdPushConstants(cmd, g_volumetricCloudCompositePipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(compositePush), &compositePush);
    vkCmdDispatch(cmd, (g_dlssOutputWidth + 7u) / 8u,
        (g_dlssOutputHeight + 7u) / 8u, 1u);
}
void destroyVolumetricCloudResources()
{
    if(g_device != VK_NULL_HANDLE)
    {
        const VkResult idleResult = vkDeviceWaitIdle(g_device);
        if(idleResult != VK_SUCCESS)
        {
            RTLog("[CLOUD] vkDeviceWaitIdle before rebuild returned " +
                std::to_string(static_cast<int>(idleResult)));
            logDeviceFaultDiagnostics("cloudRebuild/vkDeviceWaitIdle");
        }
    }
    if(g_volumetricCloudRenderPipelineV3) vkDestroyPipeline(g_device, g_volumetricCloudRenderPipelineV3, nullptr);
    if(g_volumetricCloudResolvePipeline) vkDestroyPipeline(g_device, g_volumetricCloudResolvePipeline, nullptr);
    if(g_volumetricCloudCompositePipeline) vkDestroyPipeline(g_device, g_volumetricCloudCompositePipeline, nullptr);
    if(g_volumetricLightPipeline) vkDestroyPipeline(g_device, g_volumetricLightPipeline, nullptr);
    if(g_volumetricCloudRenderPipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricCloudRenderPipelineLayout, nullptr);
    if(g_volumetricCloudResolvePipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricCloudResolvePipelineLayout, nullptr);
    if(g_volumetricCloudCompositePipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricCloudCompositePipelineLayout, nullptr);
    if(g_volumetricLightPipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricLightPipelineLayout, nullptr);
    if(g_volumetricCloudDescriptorPool) vkDestroyDescriptorPool(g_device, g_volumetricCloudDescriptorPool, nullptr);
    if(g_volumetricCloudRenderSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricCloudRenderSetLayout, nullptr);
    if(g_volumetricCloudResolveSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricCloudResolveSetLayout, nullptr);
    if(g_volumetricCloudCompositeSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricCloudCompositeSetLayout, nullptr);
    if(g_volumetricLightSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricLightSetLayout, nullptr);
    if(g_volumetricCloudAtrousPipeline) vkDestroyPipeline(g_device, g_volumetricCloudAtrousPipeline, nullptr);
    if(g_volumetricCloudAtrousPipelineLayout) vkDestroyPipelineLayout(g_device, g_volumetricCloudAtrousPipelineLayout, nullptr);
    if(g_volumetricCloudAtrousDescriptorPool) vkDestroyDescriptorPool(g_device, g_volumetricCloudAtrousDescriptorPool, nullptr);
    if(g_volumetricCloudAtrousSetLayout) vkDestroyDescriptorSetLayout(g_device, g_volumetricCloudAtrousSetLayout, nullptr);
    if(g_volumetricCloudSampler) vkDestroySampler(g_device, g_volumetricCloudSampler, nullptr);
    if(g_volumetricCloudHistoryDepthView) vkDestroyImageView(g_device, g_volumetricCloudHistoryDepthView, nullptr);
    if(g_volumetricCloudHistoryDepthImage) vkDestroyImage(g_device, g_volumetricCloudHistoryDepthImage, nullptr);
    if(g_volumetricCloudHistoryDepthMemory) vkFreeMemory(g_device, g_volumetricCloudHistoryDepthMemory, nullptr);
    if(g_volumetricCloudDepthView) vkDestroyImageView(g_device, g_volumetricCloudDepthView, nullptr);
    if(g_volumetricCloudDepthImage) vkDestroyImage(g_device, g_volumetricCloudDepthImage, nullptr);
    if(g_volumetricCloudDepthMemory) vkFreeMemory(g_device, g_volumetricCloudDepthMemory, nullptr);
    if(g_volumetricCloudHistoryView) vkDestroyImageView(g_device, g_volumetricCloudHistoryView, nullptr);
    if(g_volumetricCloudHistoryImage) vkDestroyImage(g_device, g_volumetricCloudHistoryImage, nullptr);
    if(g_volumetricCloudHistoryMemory) vkFreeMemory(g_device, g_volumetricCloudHistoryMemory, nullptr);
    if(g_volumetricCloudResolvedView) vkDestroyImageView(g_device, g_volumetricCloudResolvedView, nullptr);
    if(g_volumetricCloudResolvedImage) vkDestroyImage(g_device, g_volumetricCloudResolvedImage, nullptr);
    if(g_volumetricCloudResolvedMemory) vkFreeMemory(g_device, g_volumetricCloudResolvedMemory, nullptr);
    if(g_volumetricCloudView) vkDestroyImageView(g_device, g_volumetricCloudView, nullptr);
    if(g_volumetricCloudImage) vkDestroyImage(g_device, g_volumetricCloudImage, nullptr);
    if(g_volumetricCloudMemory) vkFreeMemory(g_device, g_volumetricCloudMemory, nullptr);
    g_volumetricCloudImage = VK_NULL_HANDLE;
    g_volumetricCloudMemory = VK_NULL_HANDLE;
    g_volumetricCloudView = VK_NULL_HANDLE;
    g_volumetricCloudHistoryImage = VK_NULL_HANDLE;
    g_volumetricCloudHistoryMemory = VK_NULL_HANDLE;
    g_volumetricCloudHistoryView = VK_NULL_HANDLE;
    g_volumetricCloudResolvedImage = VK_NULL_HANDLE;
    g_volumetricCloudResolvedMemory = VK_NULL_HANDLE;
    g_volumetricCloudResolvedView = VK_NULL_HANDLE;
    g_volumetricCloudDepthImage = VK_NULL_HANDLE;
    g_volumetricCloudDepthMemory = VK_NULL_HANDLE;
    g_volumetricCloudDepthView = VK_NULL_HANDLE;
    g_volumetricCloudHistoryDepthImage = VK_NULL_HANDLE;
    g_volumetricCloudHistoryDepthMemory = VK_NULL_HANDLE;
    g_volumetricCloudHistoryDepthView = VK_NULL_HANDLE;
    g_volumetricCloudSampler = VK_NULL_HANDLE;
    ++g_rrGuideViewGeneration;
    if(g_godRayCompositePipeline) vkDestroyPipeline(g_device, g_godRayCompositePipeline, nullptr);
    if(g_godRayCompositePipelineLayout) vkDestroyPipelineLayout(g_device, g_godRayCompositePipelineLayout, nullptr);
    if(g_godRayDescriptorPool) vkDestroyDescriptorPool(g_device, g_godRayDescriptorPool, nullptr);
    if(g_godRayCompositeSetLayout) vkDestroyDescriptorSetLayout(g_device, g_godRayCompositeSetLayout, nullptr);
    if(g_godRayHistoryView) vkDestroyImageView(g_device, g_godRayHistoryView, nullptr);
    if(g_godRayHistoryImage) vkDestroyImage(g_device, g_godRayHistoryImage, nullptr);
    if(g_godRayHistoryMemory) vkFreeMemory(g_device, g_godRayHistoryMemory, nullptr);
    if(g_godRayTargetView) vkDestroyImageView(g_device, g_godRayTargetView, nullptr);
    if(g_godRayTargetImage) vkDestroyImage(g_device, g_godRayTargetImage, nullptr);
    if(g_godRayTargetMemory) vkFreeMemory(g_device, g_godRayTargetMemory, nullptr);

    g_volumetricCloudRenderSetLayout = VK_NULL_HANDLE;
    g_volumetricCloudResolveSetLayout = VK_NULL_HANDLE;
    g_volumetricCloudCompositeSetLayout = VK_NULL_HANDLE;
    g_volumetricCloudDescriptorPool = VK_NULL_HANDLE;
    g_volumetricCloudRenderSet = VK_NULL_HANDLE;
    g_volumetricCloudResolveSet = VK_NULL_HANDLE;
    g_volumetricCloudCompositeSet = VK_NULL_HANDLE;
    g_volumetricCloudCompositeTargetView = VK_NULL_HANDLE;
    g_volumetricCloudRenderPipelineLayout = VK_NULL_HANDLE;
    g_volumetricCloudResolvePipelineLayout = VK_NULL_HANDLE;
    g_volumetricCloudCompositePipelineLayout = VK_NULL_HANDLE;
    g_volumetricCloudRenderPipelineV3 = VK_NULL_HANDLE;
    g_volumetricCloudResolvePipeline = VK_NULL_HANDLE;
    g_volumetricCloudCompositePipeline = VK_NULL_HANDLE;
    g_volumetricCloudAtrousPipeline = VK_NULL_HANDLE;
    g_volumetricCloudAtrousPipelineLayout = VK_NULL_HANDLE;
    g_volumetricCloudAtrousDescriptorPool = VK_NULL_HANDLE;
    g_volumetricCloudAtrousSetLayout = VK_NULL_HANDLE;
    g_volumetricCloudAtrousSetForward = VK_NULL_HANDLE;
    g_volumetricCloudAtrousSetBackward = VK_NULL_HANDLE;
    g_volumetricLightPipeline = VK_NULL_HANDLE;
    g_volumetricLightPipelineLayout = VK_NULL_HANDLE;
    g_volumetricLightSetLayout = VK_NULL_HANDLE;
    g_volumetricLightSet = VK_NULL_HANDLE;
    g_volumetricCloudWidth = 0u;
    g_volumetricCloudHeight = 0u;
    g_volumetricCloudBoundRenderWidth = 0u;
    g_volumetricCloudBoundRenderHeight = 0u;
    g_volumetricCloudBoundOutputWidth = 0u;
    g_volumetricCloudBoundOutputHeight = 0u;
    g_volumetricCloudLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudResolvedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_volumetricCloudHistoryNeedsClear.store(true);
    g_godRayCompositePipeline = VK_NULL_HANDLE;
    g_godRayCompositePipelineLayout = VK_NULL_HANDLE;
    g_godRayDescriptorPool = VK_NULL_HANDLE;
    g_godRayCompositeSetLayout = VK_NULL_HANDLE;
    g_godRayCompositeSet = VK_NULL_HANDLE;
    g_godRayTargetView = VK_NULL_HANDLE;
    g_godRayTargetImage = VK_NULL_HANDLE;
    g_godRayTargetMemory = VK_NULL_HANDLE;
    g_godRayHistoryView = VK_NULL_HANDLE;
    g_godRayHistoryImage = VK_NULL_HANDLE;
    g_godRayHistoryMemory = VK_NULL_HANDLE;
    g_godRayTargetLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_godRayHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_godRayHistoryNeedsClear.store(true);
    g_godRayWidth = 0u;
    g_godRayHeight = 0u;
}
