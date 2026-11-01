// Orion backend — Particles 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_beginParticleFrame(
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
    g_particleFrameVertices.clear();
    g_particleFrameMotion.clear();
    g_particleFrameOpen = true;
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadParticleBatch(
    JNIEnv*,
    jclass,
    jlong vertexPointer,
    jint vertexCount,
    jint vertexStride,
    jlong motionPointer
)
{
    if(vertexPointer == 0 || vertexCount <= 0 ||
       vertexStride != static_cast<jint>(sizeof(Vertex)) ||
       (vertexCount % 3) != 0)
    {
        return;
    }

    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    if(!g_particleFrameOpen)
    {
        return;
    }

    const float* motionSource = motionPointer != 0
        ? reinterpret_cast<const float*>(motionPointer)
        : nullptr;

    const Vertex* source = reinterpret_cast<const Vertex*>(vertexPointer);
    const size_t oldSize = g_particleFrameVertices.size();
    g_particleFrameVertices.resize(oldSize + static_cast<size_t>(vertexCount));
    g_particleFrameMotion.resize(
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
        g_particleFrameVertices[oldSize + static_cast<size_t>(index)] = vertex;

        if(motionSource != nullptr)
        {
            const size_t motionIndex =
                (oldSize + static_cast<size_t>(index)) * 3u;
            g_particleFrameMotion[motionIndex + 0u] = motionSource[index * 3 + 0];
            g_particleFrameMotion[motionIndex + 1u] = motionSource[index * 3 + 1];
            g_particleFrameMotion[motionIndex + 2u] = motionSource[index * 3 + 2];
        }
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_endParticleFrame(
    JNIEnv*,
    jclass
)
{
    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    if(!g_particleFrameOpen)
    {
        return;
    }
    g_particleFrameOpen = false;

    const auto current = g_chunkMeshes.find(PARTICLE_RT_LAYER_KEY);
    const bool verticesChanged = current == g_chunkMeshes.end()
        ? !g_particleFrameVertices.empty()
        : !sameVertexPayload(current->second, g_particleFrameVertices);

    const bool motionReset =
        publishDynamicLayerMotion(
            PARTICLE_RT_LAYER_KEY,
            g_particleFrameMotion,
            g_particleFrameVertices.size(),
            verticesChanged
        );

    const bool changed = verticesChanged || motionReset;

    const bool particleAtlasesReady =
        g_particleTextureViews[0] != VK_NULL_HANDLE &&
        g_particleTextureViews[1] != VK_NULL_HANDLE &&
        g_particleTextureViews[2] != VK_NULL_HANDLE &&
        g_particleTextureSamplers[0] != VK_NULL_HANDLE &&
        g_particleTextureSamplers[1] != VK_NULL_HANDLE &&
        g_particleTextureSamplers[2] != VK_NULL_HANDLE;

    if(!g_particleFrameVertices.empty() && !g_particleDebugEpisodeActive)
    {
        ++g_particleDebugEpisodeSerial;
        std::stringstream log;
        log
            << "[RT PARTICLES P1]"
            << " episode=" << g_particleDebugEpisodeSerial
            << " phase=start"
            << " vertices=" << g_particleFrameVertices.size()
            << " quads=" << (g_particleFrameVertices.size() / 6u)
            << " triangles=" << (g_particleFrameVertices.size() / 3u)
            << " binding37Ready=" << (particleAtlasesReady ? 1 : 0)
            << " queuedTLAS=" << (changed ? 1 : 0);
        RTLog(log.str());
        g_particleDebugEpisodeActive = true;
    }
    else if(g_particleFrameVertices.empty() && g_particleDebugEpisodeActive)
    {
        std::stringstream log;
        log
            << "[RT PARTICLES P1]"
            << " episode=" << g_particleDebugEpisodeSerial
            << " phase=end"
            << " binding37Ready=" << (particleAtlasesReady ? 1 : 0)
            << " queuedTLAS=" << (changed ? 1 : 0);
        RTLog(log.str());
        g_particleDebugEpisodeActive = false;
    }

    if(changed)
    {
        if(g_particleFrameVertices.empty())
        {
            g_chunkMeshes.erase(PARTICLE_RT_LAYER_KEY);
        }
        else
        {
            std::vector<Vertex>& cached =
                g_chunkMeshes[PARTICLE_RT_LAYER_KEY];
            cached.swap(g_particleFrameVertices);
            g_particleFrameVertices.clear();
        }
    }

    if(requestDynamicRTASRebuild(changed))
    {
        g_dirtySectionKeys.insert(ENTITY_RT_SPATIAL_KEY);
        markWorldGeometryDirty(false);
        g_entityRebuildRequested.store(true, std::memory_order_release);
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateParticleTexture(
    JNIEnv*,
    jclass,
    jint textureSlot,
    jlong imageView,
    jlong sampler
)
{
    if(textureSlot < 1 || textureSlot > static_cast<jint>(PARTICLE_RT_TEXTURE_SLOT_COUNT) ||
       imageView == 0 || sampler == 0)
    {
        return;
    }
    const uint32_t slot = static_cast<uint32_t>(textureSlot - 1);
    const VkImageView newView = reinterpret_cast<VkImageView>(imageView);
    const VkSampler newSampler = reinterpret_cast<VkSampler>(sampler);
    if(g_particleTextureViews[slot] != newView || g_particleTextureSamplers[slot] != newSampler)
    {
        g_particleTextureViews[slot] = newView;
        g_particleTextureSamplers[slot] = newSampler;
        g_particleTexturesDirty = true;
    }
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateRainStreakTexture(
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
    if(g_rainStreakTextureView != newView || g_rainStreakTextureSampler != newSampler)
    {
        g_rainStreakTextureView = newView;
        g_rainStreakTextureSampler = newSampler;
        g_rainStreakTextureDirty = true;
    }
}
