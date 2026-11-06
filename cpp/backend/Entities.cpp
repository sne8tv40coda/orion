// Orion backend — Entities 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// -------------------------------------------------------------------------
// 2026-10-09 实体 LabPBR 中性兜底图（binding 63/64）
// -------------------------------------------------------------------------
// 1×1 RGBA8，内容恒为 (0,0,0,255)。存在的唯一理由：descriptor 必须指向合法视图，
// 而"这一槽没有 `_n`/`_s`"要能用**内容**表达（rgb 全 0 ⇒ 着色器判为无贴图）。
// 法线图与高光图的中性值都是 rgb=0 / a=1，所以两处共用同一张图。
// 与 createWaterDetailNormalResources 同一套一次性上传写法（staging + 一次性
// 命令缓冲，跑在 RT 初始化时，不在帧内）。
void createEntityPbrFallbackResources()
{
    if(g_entityPbrFallbackView != VK_NULL_HANDLE)
    {
        return;
    }

    const uint8_t texel[4] = { 0u, 0u, 0u, 255u };

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    createBuffer(g_device, g_physicalDevice, sizeof(texel), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        stagingBuffer, stagingMemory);
    void* mapped = nullptr;
    check(vkMapMemory(g_device, stagingMemory, 0u, sizeof(texel), 0u, &mapped),
        "entity pbr fallback staging map failed");
    std::memcpy(mapped, texel, sizeof(texel));
    vkUnmapMemory(g_device, stagingMemory);

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {1u, 1u, 1u};
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_entityPbrFallbackImage),
        "entity pbr fallback create image failed");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_entityPbrFallbackImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_entityPbrFallbackMemory),
        "entity pbr fallback allocate failed");
    check(vkBindImageMemory(g_device, g_entityPbrFallbackImage, g_entityPbrFallbackMemory, 0u),
        "entity pbr fallback bind failed");

    VkCommandBufferAllocateInfo commandAllocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    commandAllocation.commandPool = g_rtCommandPool;
    commandAllocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandAllocation.commandBufferCount = 1u;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(g_device, &commandAllocation, &commandBuffer),
        "entity pbr fallback command allocation failed");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &begin), "entity pbr fallback command begin failed");

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_entityPbrFallbackImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1u;
    copy.imageExtent = {1u, 1u, 1u};
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, g_entityPbrFallbackImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);

    check(vkEndCommandBuffer(commandBuffer), "entity pbr fallback command end failed");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commandBuffer;
    check(vkQueueSubmit(g_queue, 1u, &submit, VK_NULL_HANDLE), "entity pbr fallback submit failed");
    check(vkQueueWaitIdle(g_queue), "entity pbr fallback upload wait failed");
    vkFreeCommandBuffers(g_device, g_rtCommandPool, 1u, &commandBuffer);
    vkDestroyBuffer(g_device, stagingBuffer, nullptr);
    vkFreeMemory(g_device, stagingMemory, nullptr);

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_entityPbrFallbackImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_entityPbrFallbackView),
        "entity pbr fallback view failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_entityPbrFallbackSampler),
        "entity pbr fallback sampler failed");

    RTLog("[ENTITY PBR] neutral 1x1 fallback created (normal/specular = rgb 0)");
}
void destroyEntityPbrFallbackResources()
{
    if(g_entityPbrFallbackSampler) vkDestroySampler(g_device, g_entityPbrFallbackSampler, nullptr);
    if(g_entityPbrFallbackView) vkDestroyImageView(g_device, g_entityPbrFallbackView, nullptr);
    if(g_entityPbrFallbackImage) vkDestroyImage(g_device, g_entityPbrFallbackImage, nullptr);
    if(g_entityPbrFallbackMemory) vkFreeMemory(g_device, g_entityPbrFallbackMemory, nullptr);
    g_entityPbrFallbackSampler = VK_NULL_HANDLE;
    g_entityPbrFallbackView = VK_NULL_HANDLE;
    g_entityPbrFallbackImage = VK_NULL_HANDLE;
    g_entityPbrFallbackMemory = VK_NULL_HANDLE;
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_beginEntityFrame(
    JNIEnv*,
    jclass
)
{
    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    if(g_cameraUboMapped)
    {
        const CameraUBO* camera =
            static_cast<const CameraUBO*>(g_cameraUboMapped);
        g_dynamicCaptureCameraX = camera->viewInverse[12];
        g_dynamicCaptureCameraY = camera->viewInverse[13];
        g_dynamicCaptureCameraZ = camera->viewInverse[14];
    }
    else
    {
        g_dynamicCaptureCameraX = 0.0f;
        g_dynamicCaptureCameraY = 0.0f;
        g_dynamicCaptureCameraZ = 0.0f;
    }
    g_entityFrameVertices.clear();
    g_localPlayerFrameVertices.clear();
    g_handFrameVertices.clear();
    g_entityFrameMotion.clear();
    g_localPlayerFrameMotion.clear();
    g_handFrameMotion.clear();
    g_entityFrameOpen = true;
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadEntityBatch(
    JNIEnv*,
    jclass,
    jint textureSlot,
    jlong vertexPointer,
    jint vertexCount,
    jint vertexStride,
    jlong motionPointer
)
{
    if(
        textureSlot < 1 ||
        textureSlot > static_cast<jint>(ENTITY_RT_TEXTURE_SLOT_COUNT) ||
        vertexPointer == 0 ||
        vertexCount <= 0 ||
        vertexStride != static_cast<jint>(sizeof(Vertex)) ||
        (vertexCount % 3) != 0
    )
    {
        return;
    }

    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    if(!g_entityFrameOpen)
    {
        return;
    }

    const Vertex* source =
        reinterpret_cast<const Vertex*>(vertexPointer);

    const float* motionSource = motionPointer != 0
        ? reinterpret_cast<const float*>(motionPointer)
        : nullptr;

    // pad1 编码批次归属（Java 侧 EntityRtCapture.emit 写入）：
    //   0.0 = 普通实体 / -1.0 = 自发光 / +1.0 = 本地玩家代理 / +2.0 = 第一人称手部
    const bool handBatch = source[0].pad1 > 1.5f;
    const bool localPlayerProxy = !handBatch && source[0].pad1 > 0.5f;
    std::vector<Vertex>& target = handBatch
        ? g_handFrameVertices
        : (localPlayerProxy ? g_localPlayerFrameVertices : g_entityFrameVertices);
    std::vector<float>& motionTarget = handBatch
        ? g_handFrameMotion
        : (localPlayerProxy ? g_localPlayerFrameMotion : g_entityFrameMotion);
    const size_t oldSize = target.size();
    target.resize(
        oldSize + static_cast<size_t>(vertexCount)
    );
    motionTarget.resize(
        (oldSize + static_cast<size_t>(vertexCount)) * 3u,
        0.0f
    );

    for(int index = 0; index < vertexCount; ++index)
    {
        Vertex vertex = source[index];
        vertex.x += g_dynamicCaptureCameraX;
        vertex.y += g_dynamicCaptureCameraY;
        vertex.z += g_dynamicCaptureCameraZ;
        vertex.materialId = 0.0f;
        const float incomingPad2 = source[index].pad2;
        const float classFraction =
            incomingPad2 > 0.0f
                ? incomingPad2 - std::floor(incomingPad2)
                : 0.0f;
        vertex.pad2 = static_cast<float>(textureSlot) + classFraction;
        target[oldSize + static_cast<size_t>(index)] = vertex;

        if(motionSource != nullptr)
        {
            const size_t motionIndex =
                (oldSize + static_cast<size_t>(index)) * 3u;
            motionTarget[motionIndex + 0u] = motionSource[index * 3 + 0];
            motionTarget[motionIndex + 1u] = motionSource[index * 3 + 1];
            motionTarget[motionIndex + 2u] = motionSource[index * 3 + 2];
        }
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_endEntityFrame(
    JNIEnv*,
    jclass
)
{
    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    if(!g_entityFrameOpen)
    {
        return;
    }

    g_entityFrameOpen = false;

    // #16 探针：帧间隔 24-93ms 而 recordFrameCommand 只 1-12ms，怀疑点在
    // Java→JNI 每帧全量 FNV。此计时只在超过 3ms 时打 [PERF L10 STALL]，
    // 不超 = 该嫌疑排除。
    uint64_t entityFrameHash = 0u;
    uint64_t localPlayerFrameHash = 0u;
    uint64_t handFrameHash = 0u;
    PERF_L10_TIME_STALL("entityFrameVertexHash", {
        entityFrameHash = hashVertexPayload(g_entityFrameVertices);
        localPlayerFrameHash = hashVertexPayload(g_localPlayerFrameVertices);
        handFrameHash = hashVertexPayload(g_handFrameVertices);
    });

    const auto currentEntity = g_chunkMeshes.find(ENTITY_RT_LAYER_KEY);
    const auto currentLocalPlayer = g_chunkMeshes.find(LOCAL_PLAYER_RT_LAYER_KEY);
    const bool entityChanged = currentEntity == g_chunkMeshes.end()
        ? !g_entityFrameVertices.empty()
        : !(g_entityCachedHashValid &&
            g_entityCachedHash == entityFrameHash &&
            currentEntity->second.size() == g_entityFrameVertices.size());
    const bool localPlayerChanged = currentLocalPlayer == g_chunkMeshes.end()
        ? !g_localPlayerFrameVertices.empty()
        : !(g_localPlayerCachedHashValid &&
            g_localPlayerCachedHash == localPlayerFrameHash &&
            currentLocalPlayer->second.size() ==
                g_localPlayerFrameVertices.size());

    // 2026-11-06：手部层改为与实体层同口径的指纹判等。
    //
    // 旧写法是 `!g_handFrameVertices.empty()`（原注释称"顶点是相机相对的、每帧都在
    // 变"），但 uploadEntityBatch 在构造时已经把相机位置加回世界空间（见本文件的
    // `vertex.x += g_dynamicCaptureCameraX`），所以"相机不动 + 手部动画静止"（站立、
    // 看物品栏、AFK、暂停）时这一层的世界坐标逐位不变，指纹必然命中。
    // 而 handChanged 会把 g_entityRebuildRequested 置真（见下方 requestRebuild 段），
    // Frame.cpp 的帧首又让它**绕过** 10 帧静默窗直接 rebuildWorldGeometry(24) ⇒
    // 手里有东西时整轮世界几何重建退化成每帧执行（全部段遍历 + 全量 sort +
    // role 表重写 + TLAS 全量 BUILD）。判等成本：手部顶点只有几十~几百个，
    // FNV 8 字节步进 < 20µs。
    const auto currentHand = g_chunkMeshes.find(HAND_RT_LAYER_KEY);
    const bool handChanged = currentHand == g_chunkMeshes.end()
        ? !g_handFrameVertices.empty()
        : !(g_handCachedHashValid &&
            g_handCachedHash == handFrameHash &&
            currentHand->second.size() == g_handFrameVertices.size());

    // 观测（2026-11-06）：每 300 次 endEntityFrame 打一行手部层判等命中率。
    // 走 RTLog（缓冲、不 flush、不 fprintf），稳态开销可忽略；只用来确认这次改动
    // 是否真的把"每帧全量重建"变成了"静止时跳过"。
    {
        static uint32_t sHandProbeFrames = 0u;
        static uint32_t sHandProbeChanged = 0u;

        ++sHandProbeFrames;
        if(handChanged)
        {
            ++sHandProbeChanged;
        }

        if(sHandProbeFrames >= 300u)
        {
            RTLog(
                "[HAND LAYER] frames=" + std::to_string(sHandProbeFrames) +
                " changed=" + std::to_string(sHandProbeChanged) +
                " unchanged=" +
                std::to_string(sHandProbeFrames - sHandProbeChanged));
            sHandProbeFrames = 0u;
            sHandProbeChanged = 0u;
        }
    }

    const bool entityMotionReset =
        publishDynamicLayerMotion(
            ENTITY_RT_LAYER_KEY,
            g_entityFrameMotion,
            g_entityFrameVertices.size(),
            entityChanged
        );
    const bool localPlayerMotionReset =
        publishDynamicLayerMotion(
            LOCAL_PLAYER_RT_LAYER_KEY,
            g_localPlayerFrameMotion,
            g_localPlayerFrameVertices.size(),
            localPlayerChanged
        );

    const bool changed =
        entityChanged ||
        localPlayerChanged ||
        handChanged ||
        entityMotionReset ||
        localPlayerMotionReset;

    if(
        !changed &&
        !g_localPlayerASUpdatePending &&
        !g_dynamicRTASUpdatePending
    )
    {
        return;
    }

    if(entityChanged)
    {
        if(g_entityFrameVertices.empty())
        {
            g_chunkMeshes.erase(ENTITY_RT_LAYER_KEY);
            g_entityCachedHashValid = false;
        }
        else
        {
            std::vector<Vertex>& cached =
                g_chunkMeshes[ENTITY_RT_LAYER_KEY];
            cached.swap(g_entityFrameVertices);

            g_entityCachedHash = entityFrameHash;
            g_entityCachedHashValid = true;

            g_entityFrameVertices.clear();
        }
    }

    if(localPlayerChanged)
    {
        if(g_localPlayerFrameVertices.empty())
        {
            g_chunkMeshes.erase(LOCAL_PLAYER_RT_LAYER_KEY);
            g_localPlayerCachedHashValid = false;
        }
        else
        {
            std::vector<Vertex>& cached =
                g_chunkMeshes[LOCAL_PLAYER_RT_LAYER_KEY];
            cached.swap(g_localPlayerFrameVertices);

            g_localPlayerCachedHash = localPlayerFrameHash;
            g_localPlayerCachedHashValid = true;

            g_localPlayerFrameVertices.clear();
        }
    }

    if(handChanged)
    {
        if(g_handFrameVertices.empty())
        {
            // 手部层消失（切第三人称 / 打开物品栏）：旧写法会把上一次的几何永远
            // 留在表里，现在与实体层同口径地删掉。
            g_chunkMeshes.erase(HAND_RT_LAYER_KEY);
            g_handCachedHashValid = false;
        }
        else
        {
            std::vector<Vertex>& cached = g_chunkMeshes[HAND_RT_LAYER_KEY];
            cached.swap(g_handFrameVertices);

            g_handCachedHash = handFrameHash;
            g_handCachedHashValid = true;
        }

        g_handFrameVertices.clear();
    }

    bool requestRebuild =
        requestDynamicRTASRebuild(entityChanged || entityMotionReset);
    if(handChanged)
    {
        requestRebuild = true;
    }
    if(localPlayerChanged || localPlayerMotionReset)
    {
        g_localPlayerASUpdatePending = true;
    }

    if(g_localPlayerASUpdatePending)
    {
        const bool firstUpdate =
            g_lastLocalPlayerASUpdateFrame == UINT64_MAX;
        const bool updateDue =
            firstUpdate ||
            g_perfL10FrameSerial >=
                g_lastLocalPlayerASUpdateFrame +
                LOCAL_PLAYER_AS_UPDATE_INTERVAL;
        if(updateDue)
        {
            requestRebuild = true;
            g_localPlayerASUpdatePending = false;
            g_lastLocalPlayerASUpdateFrame = g_perfL10FrameSerial;
        }
    }

    if(requestRebuild &&
       (localPlayerChanged || g_localPlayerASUpdatePending))
    {
        g_localPlayerASUpdatePending = false;
        g_lastLocalPlayerASUpdateFrame = g_perfL10FrameSerial;
    }

    if(requestRebuild)
    {
        g_dirtySectionKeys.insert(ENTITY_RT_SPATIAL_KEY);
        markWorldGeometryDirty(false);
        g_entityRebuildRequested.store(true, std::memory_order_release);
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateEntityTexture(
    JNIEnv*,
    jclass,
    jint textureSlot,
    jlong imageView,
    jlong sampler
)
{
    if(
        textureSlot < 1 ||
        textureSlot > static_cast<jint>(ENTITY_RT_TEXTURE_SLOT_COUNT) ||
        imageView == 0 ||
        sampler == 0
    )
    {
        return;
    }

    const uint32_t slot = static_cast<uint32_t>(textureSlot - 1);
    const VkImageView newView = reinterpret_cast<VkImageView>(imageView);
    const VkSampler newSampler = reinterpret_cast<VkSampler>(sampler);

    if(
        g_entityTextureViews[slot] != newView ||
        g_entityTextureSamplers[slot] != newSampler
    )
    {
        g_entityTextureViews[slot] = newView;
        g_entityTextureSamplers[slot] = newSampler;
        g_entityTexturesDirty = true;
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateEntityPbrTexture(
    JNIEnv*,
    jclass,
    jint textureSlot,
    jlong normalView,
    jlong normalSampler,
    jlong specularView,
    jlong specularSampler
)
{
    if(
        textureSlot < 1 ||
        textureSlot > static_cast<jint>(ENTITY_RT_TEXTURE_SLOT_COUNT)
    )
    {
        return;
    }

    const uint32_t slot = static_cast<uint32_t>(textureSlot - 1);
    const VkImageView newNormalView = reinterpret_cast<VkImageView>(normalView);
    const VkSampler newNormalSampler = reinterpret_cast<VkSampler>(normalSampler);
    const VkImageView newSpecularView = reinterpret_cast<VkImageView>(specularView);
    const VkSampler newSpecularSampler = reinterpret_cast<VkSampler>(specularSampler);

    if(
        g_entityNormalViews[slot] != newNormalView ||
        g_entityNormalSamplers[slot] != newNormalSampler ||
        g_entitySpecularViews[slot] != newSpecularView ||
        g_entitySpecularSamplers[slot] != newSpecularSampler
    )
    {
        g_entityNormalViews[slot] = newNormalView;
        g_entityNormalSamplers[slot] = newNormalSampler;
        g_entitySpecularViews[slot] = newSpecularView;
        g_entitySpecularSamplers[slot] = newSpecularSampler;
        g_entityPbrTexturesDirty = true;
    }
}
