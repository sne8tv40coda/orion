// Orion backend — As 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

void rtxCacheRealASFunctions(VkDevice device)
{
    if(g_rtxRealASDevice == device)
    {
        return;
    }

    g_rtxRealASDevice = device;
    g_rtxRealCreateAS = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
    g_rtxRealDestroyAS = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
}
VkResult VKAPI_PTR rtxCountedCreateAccelerationStructureKHR(
    VkDevice device,
    const VkAccelerationStructureCreateInfoKHR* createInfo,
    const VkAllocationCallbacks* allocator,
    VkAccelerationStructureKHR* accelerationStructure)
{
    rtxCacheRealASFunctions(device);

    if(g_rtxRealCreateAS == nullptr)
    {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    const VkResult result =
        g_rtxRealCreateAS(device, createInfo, allocator, accelerationStructure);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.asCreate.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
void VKAPI_PTR rtxCountedDestroyAccelerationStructureKHR(
    VkDevice device,
    VkAccelerationStructureKHR accelerationStructure,
    const VkAllocationCallbacks* allocator)
{
    rtxCacheRealASFunctions(device);

    if(g_rtxRealDestroyAS == nullptr)
    {
        return;
    }

    g_rtxRealDestroyAS(device, accelerationStructure, allocator);

    if(rtxMemObjTrackEnabled())
    {
        g_rtxMemObj.asDestroy.fetch_add(1u, std::memory_order_relaxed);
    }
}
PFN_vkCreateAccelerationStructureKHR getCreateAccelerationStructure()
{
    if(g_device == VK_NULL_HANDLE)
    {
        return nullptr;
    }

    rtxCacheRealASFunctions(g_device);

    return g_rtxRealCreateAS != nullptr
        ? &rtxCountedCreateAccelerationStructureKHR
        : nullptr;
}
PFN_vkDestroyAccelerationStructureKHR getDestroyAccelerationStructure()
{
    if(g_device == VK_NULL_HANDLE)
    {
        return nullptr;
    }

    rtxCacheRealASFunctions(g_device);

    return g_rtxRealDestroyAS != nullptr
        ? &rtxCountedDestroyAccelerationStructureKHR
        : nullptr;
}
PFN_vkGetAccelerationStructureBuildSizesKHR getGetAccelerationStructureBuildSizes(){return g_device?reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(g_device,"vkGetAccelerationStructureBuildSizesKHR")):nullptr;}
PFN_vkGetAccelerationStructureDeviceAddressKHR getGetAccelerationStructureDeviceAddress(){return g_device?reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(g_device,"vkGetAccelerationStructureDeviceAddressKHR")):nullptr;}
void VKAPI_PTR rtxCountedCmdBuildAccelerationStructures(
    VkCommandBuffer commandBuffer,
    uint32_t infoCount,
    const VkAccelerationStructureBuildGeometryInfoKHR* buildInfos,
    const VkAccelerationStructureBuildRangeInfoKHR* const* buildRangeInfos)
{
    if(g_rtxRealCmdBuildASDevice != g_device)
    {
        g_rtxRealCmdBuildASDevice = g_device;
        g_rtxRealCmdBuildAS = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(g_device, "vkCmdBuildAccelerationStructuresKHR"));
    }

    if(g_rtxRealCmdBuildAS == nullptr)
    {
        return;
    }

    g_rtxRealCmdBuildAS(commandBuffer, infoCount, buildInfos, buildRangeInfos);

    if(rtxMemObjTrackEnabled())
    {
        g_rtxMemObj.asBuildCmd.fetch_add(
            static_cast<uint64_t>(infoCount),
            std::memory_order_relaxed
        );
    }
}
PFN_vkCmdBuildAccelerationStructuresKHR getCmdBuildAccelerationStructures()
{
    if(g_device == VK_NULL_HANDLE)
    {
        return nullptr;
    }

    if(g_rtxRealCmdBuildASDevice != g_device)
    {
        g_rtxRealCmdBuildASDevice = g_device;
        g_rtxRealCmdBuildAS = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            vkGetDeviceProcAddr(g_device, "vkCmdBuildAccelerationStructuresKHR"));
    }

    return g_rtxRealCmdBuildAS != nullptr
        ? &rtxCountedCmdBuildAccelerationStructures
        : nullptr;
}
PFN_vkCmdTraceRaysKHR getCmdTraceRays()
{
    static VkDevice cachedDevice = VK_NULL_HANDLE;
    static PFN_vkCmdTraceRaysKHR cachedFunction = nullptr;
    if(cachedDevice != g_device)
    {
        cachedDevice = g_device;
        cachedFunction = g_device
            ? reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
                vkGetDeviceProcAddr(g_device, "vkCmdTraceRaysKHR")
            )
            : nullptr;
    }
    return cachedFunction;
}
void rtxCacheRealMicromapFunctions(VkDevice device)
{
    if(g_rtxRealMMDevice == device)
    {
        return;
    }

    g_rtxRealMMDevice = device;
    g_rtxRealCreateMM = reinterpret_cast<PFN_vkCreateMicromapEXT>(
        vkGetDeviceProcAddr(device, "vkCreateMicromapEXT"));
    g_rtxRealDestroyMM = reinterpret_cast<PFN_vkDestroyMicromapEXT>(
        vkGetDeviceProcAddr(device, "vkDestroyMicromapEXT"));
}
VkResult VKAPI_PTR rtxCountedCreateMicromapEXT(
    VkDevice device,
    const VkMicromapCreateInfoEXT* createInfo,
    const VkAllocationCallbacks* allocator,
    VkMicromapEXT* micromap)
{
    rtxCacheRealMicromapFunctions(device);

    if(g_rtxRealCreateMM == nullptr)
    {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    const VkResult result =
        g_rtxRealCreateMM(device, createInfo, allocator, micromap);

    if(rtxMemObjTrackEnabled() && result == VK_SUCCESS)
    {
        g_rtxMemObj.mmCreate.fetch_add(1u, std::memory_order_relaxed);
    }

    return result;
}
void VKAPI_PTR rtxCountedDestroyMicromapEXT(
    VkDevice device,
    VkMicromapEXT micromap,
    const VkAllocationCallbacks* allocator)
{
    rtxCacheRealMicromapFunctions(device);

    if(g_rtxRealDestroyMM == nullptr)
    {
        return;
    }

    g_rtxRealDestroyMM(device, micromap, allocator);

    if(rtxMemObjTrackEnabled())
    {
        g_rtxMemObj.mmDestroy.fetch_add(1u, std::memory_order_relaxed);
    }
}
PFN_vkCreateMicromapEXT getCreateMicromap()
{
    if(g_device == VK_NULL_HANDLE)
    {
        return nullptr;
    }

    rtxCacheRealMicromapFunctions(g_device);

    return g_rtxRealCreateMM != nullptr
        ? &rtxCountedCreateMicromapEXT
        : nullptr;
}
PFN_vkDestroyMicromapEXT getDestroyMicromap()
{
    if(g_device == VK_NULL_HANDLE)
    {
        return nullptr;
    }

    rtxCacheRealMicromapFunctions(g_device);

    return g_rtxRealDestroyMM != nullptr
        ? &rtxCountedDestroyMicromapEXT
        : nullptr;
}
PFN_vkCmdBuildMicromapsEXT getCmdBuildMicromaps(){return g_device?reinterpret_cast<PFN_vkCmdBuildMicromapsEXT>(vkGetDeviceProcAddr(g_device,"vkCmdBuildMicromapsEXT")):nullptr;}
PFN_vkGetMicromapBuildSizesEXT getMicromapBuildSizes(){return g_device?reinterpret_cast<PFN_vkGetMicromapBuildSizesEXT>(vkGetDeviceProcAddr(g_device,"vkGetMicromapBuildSizesEXT")):nullptr;}
PFN_vkCmdPipelineBarrier2Omm getCmdPipelineBarrier2Omm()
{
    if(!g_device)
        return nullptr;
    auto fn = vkGetDeviceProcAddr(g_device, "vkCmdPipelineBarrier2");
    if(!fn)
        fn = vkGetDeviceProcAddr(g_device, "vkCmdPipelineBarrier2KHR");
    return reinterpret_cast<PFN_vkCmdPipelineBarrier2Omm>(fn);
}
void probeOpacityMicromapSupport()
{
    g_ommNativeEnabled = false;
    g_ommMax4StateSubdivisionLevel = 0;
    if(!g_instance || !g_physicalDevice || !g_device)
    {
        return;
    }

    auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2")
    );
    if(!properties2 || !getCreateMicromap() || !getDestroyMicromap() ||
       !getCmdBuildMicromaps() || !getMicromapBuildSizes() ||
       !getCmdPipelineBarrier2Omm())
    {
        RTLog("[OMM NATIVE] enabled=0 reason=missingEntryPoint");
        return;
    }

    VkPhysicalDeviceOpacityMicromapPropertiesEXT ommProps{};
    ommProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &ommProps;
    properties2(g_physicalDevice, &properties);

    g_ommMax4StateSubdivisionLevel = ommProps.maxOpacity4StateSubdivisionLevel;
    g_ommNativeEnabled =
        !(std::getenv("ORION_DISABLE_OMM") != nullptr &&
          std::getenv("ORION_DISABLE_OMM")[0] == '1') &&
        g_ommMax4StateSubdivisionLevel >= 4u;

    std::stringstream log;
    log << "[OMM NATIVE] enabled=" << (g_ommNativeEnabled ? 1 : 0)
        << " max4StateLevel=" << g_ommMax4StateSubdivisionLevel;
    RTLog(log.str());
}
void destroySectionOpacityMicromap(RTSectionAS& section)
{
    if(section.opacityMicromap != VK_NULL_HANDLE)
    {
        if(auto destroyMicromap = getDestroyMicromap())
            destroyMicromap(g_device, section.opacityMicromap, nullptr);
    }

    destroyPooledBuffer(section.ommScratchBuffer, section.ommScratchBinding);
    destroyPooledBuffer(section.ommStorageBuffer, section.ommStorageBinding);
    destroyPooledBuffer(section.ommTriangleBuffer, section.ommTriangleBinding);
    destroyPooledBuffer(section.ommDataBuffer, section.ommDataBinding);

    section.ommDataAddress = 0;
    section.ommTriangleAddress = 0;
    section.ommScratchAddress = 0;
    section.opacityMicromap = VK_NULL_HANDLE;
    section.ommUsage = {};
    section.hasOpacityMicromap = false;
}
void deferSectionOpacityMicromapBuildInputs(RTSectionAS& section)
{
    if(!section.hasOpacityMicromap ||
       (section.ommDataBuffer == VK_NULL_HANDLE &&
        section.ommTriangleBuffer == VK_NULL_HANDLE &&
        section.ommScratchBuffer == VK_NULL_HANDLE))
    {
        return;
    }

    DeferredSectionBuildResources::OmmBuildInputs deferred{};
    deferred.dataBuffer = section.ommDataBuffer;
    deferred.dataBinding = section.ommDataBinding;
    deferred.triangleBuffer = section.ommTriangleBuffer;
    deferred.triangleBinding = section.ommTriangleBinding;
    deferred.scratchBuffer = section.ommScratchBuffer;
    deferred.scratchBinding = section.ommScratchBinding;
    g_deferredSectionBuild.ommBuildInputs.push_back(deferred);

    section.ommDataBuffer = VK_NULL_HANDLE;
    section.ommDataBinding = {};
    section.ommDataAddress = 0;
    section.ommTriangleBuffer = VK_NULL_HANDLE;
    section.ommTriangleBinding = {};
    section.ommTriangleAddress = 0;
    section.ommScratchBuffer = VK_NULL_HANDLE;
    section.ommScratchBinding = {};
    section.ommScratchAddress = 0;
}


bool createSectionOpacityMicromap(
    RTSectionAS& section,
    const OmmCutoutInput& input
)
{
    constexpr uint32_t kExpectedLevel = 4u;
    constexpr uint32_t kExpectedBytesPerTriangle = 64u;
    constexpr VkDeviceAddress kInputAddressAlignment = 256u;

    if(!g_ommNativeEnabled || input.subdivisionLevel != kExpectedLevel ||
       input.triangleCount != section.triangleCount ||
       input.knownMicroTriangleCount == 0u ||
       input.data.size() != static_cast<size_t>(input.triangleCount) *
                            kExpectedBytesPerTriangle)
    {
        return false;
    }

    const VkBufferUsageFlags inputUsage =
        VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if(!createPooledBuffer(input.data.size(), inputUsage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true, section.ommDataBuffer, section.ommDataBinding))
    {
        RTLog("[OMM NATIVE] data buffer pool acquire failed fallback=anyHit");
        destroySectionOpacityMicromap(section);
        return false;
    }
    void* mapped = nullptr;
    check(vkMapMemory(g_device, section.ommDataBinding.memory,
        section.ommDataBinding.memoryOffset, input.data.size(),
        0, &mapped), "map Section OMM data failed");
    std::memcpy(mapped, input.data.data(), input.data.size());
    vkUnmapMemory(g_device, section.ommDataBinding.memory);
    section.ommDataAddress = getBufferDeviceAddress(g_device, section.ommDataBuffer);

    thread_local std::vector<VkMicromapTriangleEXT> trianglesScratch;
    trianglesScratch.clear();
    trianglesScratch.resize(input.triangleCount);
    VkMicromapTriangleEXT* triangles = trianglesScratch.data();
    for(uint32_t triangle = 0; triangle < input.triangleCount; ++triangle)
    {
        triangles[triangle].dataOffset = triangle * kExpectedBytesPerTriangle;
        triangles[triangle].subdivisionLevel = static_cast<uint16_t>(kExpectedLevel);
        triangles[triangle].format =
            static_cast<uint16_t>(VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT);
    }
    const VkDeviceSize triangleBytes =
        static_cast<VkDeviceSize>(input.triangleCount) * sizeof(VkMicromapTriangleEXT);
    if(!createPooledBuffer(triangleBytes, inputUsage,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true, section.ommTriangleBuffer, section.ommTriangleBinding))
    {
        RTLog("[OMM NATIVE] triangle buffer pool acquire failed fallback=anyHit");
        destroySectionOpacityMicromap(section);
        return false;
    }
    mapped = nullptr;
    check(vkMapMemory(g_device, section.ommTriangleBinding.memory,
        section.ommTriangleBinding.memoryOffset, triangleBytes,
        0, &mapped), "map Section OMM triangle records failed");
    std::memcpy(mapped, triangles, static_cast<size_t>(triangleBytes));
    vkUnmapMemory(g_device, section.ommTriangleBinding.memory);
    section.ommTriangleAddress =
        getBufferDeviceAddress(g_device, section.ommTriangleBuffer);

    if((section.ommDataAddress & (kInputAddressAlignment - 1u)) != 0u ||
       (section.ommTriangleAddress & (kInputAddressAlignment - 1u)) != 0u)
    {
        RTLog("[OMM NATIVE] sectionInputAlignment=0 fallback=anyHit");
        destroySectionOpacityMicromap(section);
        return false;
    }

    section.ommUsage.count = input.triangleCount;
    section.ommUsage.subdivisionLevel = kExpectedLevel;
    section.ommUsage.format = VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT;

    VkMicromapBuildInfoEXT buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
    buildInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    buildInfo.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
    buildInfo.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
    buildInfo.usageCountsCount = 1;
    buildInfo.pUsageCounts = &section.ommUsage;
    buildInfo.data.deviceAddress = section.ommDataAddress;
    buildInfo.triangleArray.deviceAddress = section.ommTriangleAddress;
    buildInfo.triangleArrayStride = sizeof(VkMicromapTriangleEXT);

    VkMicromapBuildSizesInfoEXT sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT;
    getMicromapBuildSizes()(g_device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &sizes);
    if(sizes.micromapSize == 0u || sizes.buildScratchSize == 0u)
    {
        RTLog("[OMM NATIVE] buildSizes=0 fallback=anyHit");
        destroySectionOpacityMicromap(section);
        return false;
    }

    if(!createPooledBuffer(sizes.micromapSize,
        VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        false, section.ommStorageBuffer, section.ommStorageBinding))
    {
        RTLog("[OMM NATIVE] storage buffer pool acquire failed fallback=anyHit");
        destroySectionOpacityMicromap(section);
        return false;
    }
    VkMicromapCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT;
    createInfo.buffer = section.ommStorageBuffer;
    createInfo.size = sizes.micromapSize;
    createInfo.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
    check(getCreateMicromap()(g_device, &createInfo, nullptr,
        &section.opacityMicromap), "create Section opacity micromap failed");

    if(!createPooledBuffer(sizes.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        false, section.ommScratchBuffer, section.ommScratchBinding))
    {
        RTLog("[OMM NATIVE] scratch buffer pool acquire failed fallback=anyHit");
        destroySectionOpacityMicromap(section);
        return false;
    }
    section.ommScratchAddress =
        getBufferDeviceAddress(g_device, section.ommScratchBuffer);
    section.hasOpacityMicromap = true;
    return true;
}


extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadCutoutOmmSection(
    JNIEnv*,
    jclass,
    jint sectionX,
    jint sectionY,
    jint sectionZ,
    jlong dataPointer,
    jint dataBytes,
    jint triangleCount,
    jint subdivisionLevel,
    jint knownMicroTriangleCount
)
{
    constexpr uint32_t kOmmLevel = 4u;
    constexpr uint32_t kOmmBytesPerTriangle = 64u;
    const uint64_t key = packSectionKey(sectionX, sectionY, sectionZ, 1);
    const uint64_t spatialKey = key & SPATIAL_SECTION_KEY_MASK;

    const bool valid =
        dataPointer != 0 &&
        triangleCount > 0 &&
        subdivisionLevel == static_cast<jint>(kOmmLevel) &&
        knownMicroTriangleCount > 0 &&
        static_cast<uint64_t>(triangleCount) <=
            std::numeric_limits<uint32_t>::max() / kOmmBytesPerTriangle &&
        static_cast<uint64_t>(knownMicroTriangleCount) <=
            static_cast<uint64_t>(triangleCount) * 256u &&
        dataBytes == static_cast<jint>(
            static_cast<uint64_t>(triangleCount) * kOmmBytesPerTriangle
        );

    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
    if(!valid)
    {
        if(g_cutoutOmmInputs.erase(key) != 0u)
        {
            g_dirtySectionKeys.insert(spatialKey);
            markWorldGeometryDirty();
        }
        return;
    }

    OmmCutoutInput input{};
    input.triangleCount = static_cast<uint32_t>(triangleCount);
    input.subdivisionLevel = kOmmLevel;
    input.knownMicroTriangleCount =
        static_cast<uint32_t>(knownMicroTriangleCount);
    const uint8_t* source = reinterpret_cast<const uint8_t*>(dataPointer);
    input.data.assign(source, source + static_cast<size_t>(dataBytes));

    auto existing = g_cutoutOmmInputs.find(key);
    if(existing != g_cutoutOmmInputs.end() &&
       existing->second.triangleCount == input.triangleCount &&
       existing->second.knownMicroTriangleCount == input.knownMicroTriangleCount &&
       existing->second.data == input.data)
    {
        return;
    }

    g_cutoutOmmInputs[key] = std::move(input);
    g_dirtySectionKeys.insert(spatialKey);
    markWorldGeometryDirty();
}
