// Orion backend — Lights 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"



uint32_t localLightGridHash32(
    int32_t x,
    int32_t y,
    int32_t z
)
{
    uint32_t h =
        static_cast<uint32_t>(x) * 0x8da6b343u;

    h ^=
        static_cast<uint32_t>(y) * 0xd8163841u;

    h ^=
        static_cast<uint32_t>(z) * 0xcb1ab31fu;

    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;

    return h;
}


void destroyLocalLightGpuBuffer()
{
    if(
        g_localLightGpuBuffer !=
        VK_NULL_HANDLE
    )
    {
        vkDestroyBuffer(
            g_device,
            g_localLightGpuBuffer,
            nullptr
        );

        g_localLightGpuBuffer =
            VK_NULL_HANDLE;
    }


    if(
        g_localLightGpuMemory !=
        VK_NULL_HANDLE
    )
    {
        vkFreeMemory(
            g_device,
            g_localLightGpuMemory,
            nullptr
        );

        g_localLightGpuMemory =
            VK_NULL_HANDLE;
    }


    g_localLightGpuCount =
        0;
}


void destroyLocalLightGridGpuBuffer()
{
    if(
        g_localLightGridGpuBuffer !=
        VK_NULL_HANDLE
    )
    {
        vkDestroyBuffer(
            g_device,
            g_localLightGridGpuBuffer,
            nullptr
        );

        g_localLightGridGpuBuffer =
            VK_NULL_HANDLE;
    }


    if(
        g_localLightGridGpuMemory !=
        VK_NULL_HANDLE
    )
    {
        vkFreeMemory(
            g_device,
            g_localLightGridGpuMemory,
            nullptr
        );

        g_localLightGridGpuMemory =
            VK_NULL_HANDLE;
    }


    g_localLightGridCellCount =
        0;

    g_localLightGridIndexCount =
        0;

    g_localLightGridHashCapacity =
        0;
}


int32_t localLightGridFloorDiv32(
    int32_t value
)
{
    const int64_t wideValue =
        static_cast<int64_t>(value);


    if(wideValue >= 0)
    {
        return static_cast<int32_t>(
            wideValue /
            LOCAL_LIGHT_GRID_CELL_SIZE
        );
    }


    return static_cast<int32_t>(
        -(
            (-wideValue +
                LOCAL_LIGHT_GRID_CELL_SIZE - 1) /
            LOCAL_LIGHT_GRID_CELL_SIZE
        )
    );
}

// binding 61 的描述符必须**无条件**有效：着色器要先读头部才知道有几条光源，
// 而"描述符从没写过"与"条数 0"在 Vulkan 里是两件事（未写描述符被访问是未定义行为）。
// 所以初始化时就建好这块 buffer 并写好描述符，内容全 0 = 零光源、零成本。
void ensureDynamicLightBuffer()
{
    if(g_dynamicLightBuffer != VK_NULL_HANDLE || g_device == VK_NULL_HANDLE)
    {
        return;
    }

    createBuffer(
        g_device,
        g_physicalDevice,
        DYNAMIC_LIGHT_BUFFER_BYTES,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_dynamicLightBuffer,
        g_dynamicLightMemory
    );
}
void updateDynamicLightDescriptor()
{
    ensureDynamicLightBuffer();

    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
       g_dynamicLightBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    VkDescriptorBufferInfo info{};
    info.buffer = g_dynamicLightBuffer;
    info.offset = 0;
    info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 61u;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;

    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);
}

// 每帧在相机 UBO 之后、任何消费者之前录制（与相机的 vkCmdUpdateBuffer 同一窗口）。
// 内容没变就完全不上传 —— 站着不动举火把时是零开销。
void recordDynamicLightUpload(VkCommandBuffer cmd)
{
    if(!g_dynamicLightsDirty ||
       cmd == VK_NULL_HANDLE ||
       g_dynamicLightBuffer == VK_NULL_HANDLE)
    {
        return;
    }

    ++g_dynamicLightUpload.revision;

    const VkDeviceSize bytes = 16u +
        static_cast<VkDeviceSize>(g_dynamicLightUpload.count) *
        sizeof(DynamicLightRecord);

    vkCmdUpdateBuffer(
        cmd,
        g_dynamicLightBuffer,
        0u,
        bytes,
        &g_dynamicLightUpload
    );

    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = g_dynamicLightBuffer;
    barrier.offset = 0u;
    barrier.size = DYNAMIC_LIGHT_BUFFER_BYTES;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0u, 0u, nullptr, 1u,
        &barrier, 0u, nullptr);

    g_dynamicLightsDirty = false;
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadDynamicLights(
    JNIEnv*,
    jclass,
    jlong recordPointer,
    jint recordCount,
    jfloat intensity)
{
    const uint32_t requested = recordCount <= 0
        ? 0u
        : static_cast<uint32_t>(recordCount);
    const uint32_t count = requested > DYNAMIC_LIGHT_MAX_RECORDS
        ? DYNAMIC_LIGHT_MAX_RECORDS
        : requested;

    const float requestedIntensity =
        (intensity >= 0.0f && intensity <= 16.0f) ? intensity : 1.0f;

    const float encodedIntensity = requestedIntensity + 1.0f;

    const DynamicLightRecord* incoming =
        (count > 0u && recordPointer != 0)
            ? reinterpret_cast<const DynamicLightRecord*>(recordPointer)
            : nullptr;

    const bool sameRecords =
        g_dynamicLightUpload.count == count &&
        (count == 0u ||
         std::memcmp(g_dynamicLightUpload.records, incoming,
             static_cast<size_t>(count) * sizeof(DynamicLightRecord)) == 0);

    if(sameRecords && g_dynamicLightUpload.intensity == encodedIntensity)
    {
        return;
    }

    g_dynamicLightUpload.count = count;
    g_dynamicLightUpload.intensity = encodedIntensity;

    if(count > 0u && incoming != nullptr)
    {
        std::memcpy(
            g_dynamicLightUpload.records,
            incoming,
            static_cast<size_t>(count) * sizeof(DynamicLightRecord)
        );
    }

    g_dynamicLightsDirty = true;
}
void updateLocalLightDescriptor()
{
    if(
        g_rayTracingDescriptorSet ==
            VK_NULL_HANDLE ||

        g_localLightGpuBuffer ==
            VK_NULL_HANDLE
    )
    {
        return;
    }


    VkDescriptorBufferInfo
        localLightInfo{};

    localLightInfo.buffer =
        g_localLightGpuBuffer;

    localLightInfo.offset =
        0;

    localLightInfo.range =
        VK_WHOLE_SIZE;


    VkWriteDescriptorSet
        write{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
        };

    write.dstSet =
        g_rayTracingDescriptorSet;

    write.dstBinding =
        17;

    write.descriptorCount =
        1;

    write.descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    write.pBufferInfo =
        &localLightInfo;


    vkUpdateDescriptorSets(
        g_device,
        1,
        &write,
        0,
        nullptr
    );
}


void updateLocalLightGridDescriptor()
{
    if(
        g_rayTracingDescriptorSet ==
            VK_NULL_HANDLE ||

        g_localLightGridGpuBuffer ==
            VK_NULL_HANDLE
    )
    {
        return;
    }


    VkDescriptorBufferInfo
        gridInfo{};

    gridInfo.buffer =
        g_localLightGridGpuBuffer;

    gridInfo.offset =
        0;

    gridInfo.range =
        VK_WHOLE_SIZE;


    VkWriteDescriptorSet
        write{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
        };

    write.dstSet =
        g_rayTracingDescriptorSet;

    write.dstBinding =
        18;

    write.descriptorCount =
        1;

    write.descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    write.pBufferInfo =
        &gridInfo;


    vkUpdateDescriptorSets(
        g_device,
        1,
        &write,
        0, nullptr
    );
}


// 2026-10-23：体积雾的动态光源绑定（体积云 render set 的 binding 12/13）。
// 与 rgen 的 binding 17/18 是**同一对 buffer**，只是另一个 descriptor set ——
// 于是"雾里亮的火把"与"rgen 里给表面打光的火把"结构上必然是同一盏灯。
// 只在句柄真的变了（本地光表扩容重建 / 云描述符集重建）时重写：描述符集可能
// 仍被在途帧使用，那时无条件改写是未定义行为。
void updateCloudLocalLightDescriptors()
{
    if(g_volumetricCloudRenderSet == VK_NULL_HANDLE ||
       g_localLightGpuBuffer == VK_NULL_HANDLE ||
       g_localLightGridGpuBuffer == VK_NULL_HANDLE)
    {
        return;
    }
    if(g_cloudFogLightSetBound == g_volumetricCloudRenderSet &&
       g_cloudFogLightListBound == g_localLightGpuBuffer &&
       g_cloudFogLightGridBound == g_localLightGridGpuBuffer)
    {
        return;
    }

    VkDescriptorBufferInfo lightListInfo{};
    lightListInfo.buffer = g_localLightGpuBuffer;
    lightListInfo.offset = 0u;
    lightListInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo lightGridInfo{};
    lightGridInfo.buffer = g_localLightGridGpuBuffer;
    lightGridInfo.offset = 0u;
    lightGridInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 12u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &lightListInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_volumetricCloudRenderSet, 13u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &lightGridInfo, nullptr };
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

    g_cloudFogLightSetBound = g_volumetricCloudRenderSet;
    g_cloudFogLightListBound = g_localLightGpuBuffer;
    g_cloudFogLightGridBound = g_localLightGridGpuBuffer;
    g_cloudFogLightDescriptorsReady.store(true);
}


void rebuildLocalLightGpuBuffer()
{
    if(
        g_device ==
            VK_NULL_HANDLE ||

        g_physicalDevice ==
            VK_NULL_HANDLE ||

        g_queue ==
            VK_NULL_HANDLE ||

        g_rayTracingDescriptorSet ==
            VK_NULL_HANDLE
    )
    {
        return;
    }


    const auto perfL10RebuildStart =
        std::chrono::steady_clock::now();

    double perfL10QueueIdleMilliseconds =
        0.0;

    size_t droppedLightCount =
        0u;


    static VkDeviceSize
        g_localLightGpuCapacityBytes =
            LOCAL_LIGHT_GPU_FIXED_BYTES;

    static VkDeviceSize
        g_localLightGridCapacityBytes =
            LOCAL_LIGHT_GRID_FIXED_BYTES;


    std::vector<GPULocalLightRecord>
        gpuLights;


    std::unordered_map<
        LocalLightGridCellCoord,
        std::vector<uint32_t>,
        LocalLightGridCellCoordHash
    > gridCells;


    {
        std::lock_guard<std::mutex>
            lock(
                g_lightMutex
            );

        gpuLights.reserve(
            g_totalLocalLights
        );

        std::vector<uint64_t>
            orderedSectionKeys;

        orderedSectionKeys.reserve(
            g_lightSections.size()
        );

        for(const auto& sectionEntry : g_lightSections)
        {
            orderedSectionKeys.push_back(
                sectionEntry.first
            );
        }

        std::sort(
            orderedSectionKeys.begin(),
            orderedSectionKeys.end()
        );

        for(uint64_t sectionKey : orderedSectionKeys)
        {
            const auto sectionIt =
                g_lightSections.find(sectionKey);

            if(sectionIt == g_lightSections.end())
            {
                continue;
            }

            for(
                const LocalLightRecord& light :
                sectionIt->second
            )
            {
                if(
                    gpuLights.size() >=
                    static_cast<size_t>(
                        LOCAL_LIGHT_GPU_MAX_LIGHTS
                    )
                )
                {
                    ++droppedLightCount;

                    continue;
                }

                GPULocalLightRecord
                    gpuLight{};

                gpuLight.worldX =
                    static_cast<float>(
                        light.worldX
                    );

                gpuLight.worldY =
                    static_cast<float>(
                        light.worldY
                    );

                gpuLight.worldZ =
                    static_cast<float>(
                        light.worldZ
                    );

                gpuLight.emissionLevel =
                    static_cast<float>(
                        light.emissionLevel
                    );

                gpuLight.typeCode =
                    static_cast<uint32_t>(
                        std::max(
                            light.typeCode,
                            0
                        )
                    );


                if(
                    gpuLights.size() >=
                    static_cast<size_t>(
                        std::numeric_limits<uint32_t>::max()
                    )
                )
                {
                    throw std::runtime_error(
                        "RT Local Lights exceed uint32 GPU index capacity"
                    );
                }


                const uint32_t lightIndex =
                    static_cast<uint32_t>(
                        gpuLights.size()
                    );


                const LocalLightGridCellCoord cellCoord{
                    localLightGridFloorDiv32(
                        light.worldX
                    ),
                    localLightGridFloorDiv32(
                        light.worldY
                    ),
                    localLightGridFloorDiv32(
                        light.worldZ
                    )
                };


                gridCells[cellCoord].push_back(
                    lightIndex
                );

                gpuLights.push_back(
                    gpuLight
                );
            }
        }
    }


    if(
        gridCells.size() >
        static_cast<size_t>(
            std::numeric_limits<uint32_t>::max()
        )
    )
    {
        throw std::runtime_error(
            "RT Local-Light grid exceeds uint32 cell capacity"
        );
    }


    const size_t requiredHashSlots =
        std::max<size_t>(
            1,
            gridCells.size() * 2
        );


    size_t hashCapacity =
        1;


    while(hashCapacity < requiredHashSlots)
    {
        if(
            hashCapacity >
            static_cast<size_t>(
                std::numeric_limits<uint32_t>::max()
            ) /
                2
        )
        {
            throw std::runtime_error(
                "RT Local-Light grid hash capacity overflow"
            );
        }


        hashCapacity *=
            2;
    }


    thread_local std::vector<GPULocalLightGridSlot>
        gridSlotsScratch;
    gridSlotsScratch.resize(
        hashCapacity
    );
    std::memset(
        gridSlotsScratch.data(),
        0,
        gridSlotsScratch.size() * sizeof(GPULocalLightGridSlot)
    );
    std::vector<GPULocalLightGridSlot>& gridSlots = gridSlotsScratch;


    std::vector<uint32_t>
        gridLightIndices;

    gridLightIndices.reserve(
        gpuLights.size()
    );


    const size_t hashMask =
        hashCapacity - 1;

    std::vector<LocalLightGridCellCoord>
        orderedGridCells;

    orderedGridCells.reserve(
        gridCells.size()
    );

    for(const auto& cellEntry : gridCells)
    {
        orderedGridCells.push_back(
            cellEntry.first
        );
    }

    std::sort(
        orderedGridCells.begin(),
        orderedGridCells.end(),
        [](
            const LocalLightGridCellCoord& a,
            const LocalLightGridCellCoord& b
        )
        {
            if(a.x != b.x)
            {
                return a.x < b.x;
            }

            if(a.y != b.y)
            {
                return a.y < b.y;
            }

            return a.z < b.z;
        }
    );

    std::vector<uint32_t>
        cellLightIndicesScratch;

    uint32_t
        cappedCellCount = 0u;


    for(const LocalLightGridCellCoord& gridCoord : orderedGridCells)
    {
        const auto cellIt =
            gridCells.find(gridCoord);

        if(cellIt == gridCells.end())
        {
            continue;
        }

        const LocalLightGridCellCoord& coord =
            cellIt->first;

        const std::vector<uint32_t>& cellLightIndices =
            cellIt->second;


        const uint32_t* packedCellFirst =
            cellLightIndices.data();

        size_t packedCellCount =
            cellLightIndices.size();

        if(
            LOCAL_LIGHT_MAX_PER_CELL > 0u &&
            packedCellCount >
                static_cast<size_t>(
                    LOCAL_LIGHT_MAX_PER_CELL
                )
        )
        {
            ++cappedCellCount;

            cellLightIndicesScratch.assign(
                cellLightIndices.begin(),
                cellLightIndices.end()
            );

            std::partial_sort(
                cellLightIndicesScratch.begin(),
                cellLightIndicesScratch.begin() +
                    static_cast<std::ptrdiff_t>(
                        LOCAL_LIGHT_MAX_PER_CELL
                    ),
                cellLightIndicesScratch.end(),
                [&gpuLights](
                    uint32_t left,
                    uint32_t right
                )
                {
                    const float leftEmission =
                        gpuLights[left].emissionLevel;

                    const float rightEmission =
                        gpuLights[right].emissionLevel;

                    if(leftEmission != rightEmission)
                    {
                        return leftEmission > rightEmission;
                    }

                    return left < right;
                }
            );

            packedCellFirst =
                cellLightIndicesScratch.data();

            packedCellCount =
                static_cast<size_t>(
                    LOCAL_LIGHT_MAX_PER_CELL
                );
        }


        const uint32_t firstLightIndex =
            static_cast<uint32_t>(
                gridLightIndices.size()
            );


        if(
            packedCellCount >
            static_cast<size_t>(
                std::numeric_limits<uint32_t>::max()
            )
        )
        {
            throw std::runtime_error(
                "RT Local-Light grid cell exceeds uint32 count capacity"
            );
        }


        gridLightIndices.insert(
            gridLightIndices.end(),
            packedCellFirst,
            packedCellFirst + packedCellCount
        );


        size_t slotIndex =
            static_cast<size_t>(
                localLightGridHash32(
                    coord.x,
                    coord.y,
                    coord.z
                )
            ) &
            hashMask;


        while(gridSlots[slotIndex].occupied != 0u)
        {
            slotIndex =
                (slotIndex + 1) &
                hashMask;
        }


        GPULocalLightGridSlot& slot =
            gridSlots[slotIndex];

        slot.cellX =
            coord.x;

        slot.cellY =
            coord.y;

        slot.cellZ =
            coord.z;

        slot.occupied =
            1u;

        slot.firstLightIndex =
            firstLightIndex;

        slot.lightCount =
            static_cast<uint32_t>(
                packedCellCount
            );
    }


    ++g_localLightRevision;

    if(g_localLightRevision == 0u)
    {
        g_localLightRevision = 1u;
    }


    const GPULocalLightHeader
        header{
            static_cast<uint32_t>(
                gpuLights.size()
            ),
            g_localLightRevision,
            0u,
            0u
        };


    const VkDeviceSize
        totalBytes =
            static_cast<VkDeviceSize>(
                sizeof(GPULocalLightHeader) +
                gpuLights.size() *
                sizeof(GPULocalLightRecord)
            );


    const GPULocalLightGridHeader
        gridHeader{
            static_cast<uint32_t>(
                gridCells.size()
            ),
            static_cast<uint32_t>(
                gridLightIndices.size()
            ),
            static_cast<uint32_t>(
                hashCapacity
            ),
            LOCAL_LIGHT_GRID_SLOT_WORDS
        };


    const VkDeviceSize
        gridTotalBytes =
            static_cast<VkDeviceSize>(
                sizeof(GPULocalLightGridHeader) +
                gridSlots.size() *
                sizeof(GPULocalLightGridSlot) +
                gridLightIndices.size() *
                sizeof(uint32_t)
            );


    const bool needsLocalLightGrow =
        g_localLightGpuBuffer ==
            VK_NULL_HANDLE ||
        totalBytes >
            g_localLightGpuCapacityBytes;

    const bool needsGridGrow =
        g_localLightGridGpuBuffer ==
            VK_NULL_HANDLE ||
        gridTotalBytes >
            g_localLightGridCapacityBytes;

    if(needsLocalLightGrow || needsGridGrow)
    {
        if(
            g_localLightGpuBuffer !=
                VK_NULL_HANDLE ||

            g_localLightGridGpuBuffer !=
                VK_NULL_HANDLE
        )
        {
            const auto perfL10QueueIdleStart =
                std::chrono::steady_clock::now();

            PERF_L10_TIME_STALL("localLightQueueIdle", check(
                vkQueueWaitIdle(
                    g_queue
                ),
                "wait RT queue before Local-Light buffer rebuild failed"
            ));

            perfL10QueueIdleMilliseconds =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() -
                    perfL10QueueIdleStart
                ).count();
        }


        if(needsLocalLightGrow)
        {
            destroyLocalLightGpuBuffer();

            g_localLightGpuCapacityBytes =
                LOCAL_LIGHT_GPU_FIXED_BYTES;

            createBuffer(
                g_device,
                g_physicalDevice,
                g_localLightGpuCapacityBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                g_localLightGpuBuffer,
                g_localLightGpuMemory
            );
        }


        if(needsGridGrow)
        {
            destroyLocalLightGridGpuBuffer();

            g_localLightGridCapacityBytes =
                LOCAL_LIGHT_GRID_FIXED_BYTES;

            createBuffer(
                g_device,
                g_physicalDevice,
                g_localLightGridCapacityBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                g_localLightGridGpuBuffer,
                g_localLightGridGpuMemory
            );
        }
    }


    void* mapped =
        nullptr;

    check(
        vkMapMemory(
            g_device,
            g_localLightGpuMemory,
            0,
            totalBytes,
            0,
            &mapped
        ),
        "RT Local Lights L3 map failed"
    );


    std::memcpy(
        mapped,
        &header,
        sizeof(GPULocalLightHeader)
    );


    if(
        !gpuLights.empty()
    )
    {
        std::memcpy(
            static_cast<uint8_t*>(
                mapped
            ) +
            sizeof(GPULocalLightHeader),
            gpuLights.data(),
            gpuLights.size() *
            sizeof(GPULocalLightRecord)
        );
    }


    vkUnmapMemory(
        g_device,
        g_localLightGpuMemory
    );


    void* gridMapped =
        nullptr;

    check(
        vkMapMemory(
            g_device,
            g_localLightGridGpuMemory,
            0,
            gridTotalBytes,
            0,
            &gridMapped
        ),
        "RT Local-Light Grid P2B-1 map failed"
    );


    std::memcpy(
        gridMapped,
        &gridHeader,
        sizeof(GPULocalLightGridHeader)
    );


    std::memcpy(
        static_cast<uint8_t*>(
            gridMapped
        ) +
            sizeof(GPULocalLightGridHeader),
        gridSlots.data(),
        gridSlots.size() *
        sizeof(GPULocalLightGridSlot)
    );


    if(!gridLightIndices.empty())
    {
        std::memcpy(
            static_cast<uint8_t*>(
                gridMapped
            ) +
                sizeof(GPULocalLightGridHeader) +
                gridSlots.size() *
                sizeof(GPULocalLightGridSlot),
            gridLightIndices.data(),
            gridLightIndices.size() *
            sizeof(uint32_t)
        );
    }


    vkUnmapMemory(
        g_device,
        g_localLightGridGpuMemory
    );


    updateLocalLightDescriptor();

    updateLocalLightGridDescriptor();

    updateCloudLocalLightDescriptors();


    g_localLightGpuCount =
        gpuLights.size();

    g_localLightGridCellCount =
        gridCells.size();

    g_localLightGridIndexCount =
        gridLightIndices.size();

    g_localLightGridHashCapacity =
        hashCapacity;

    g_localLightsDirty =
        false;


    static uint32_t localLightLogThrottle = 0u;
    const bool localLightLogDue =
        (localLightLogThrottle++ % 120u) == 0u;

    if(localLightLogDue)
    {
        std::stringstream ss;

        ss
            << "[RT LOCAL LIGHTS L3 GPU]"
            << " lights="
            << g_localLightGpuCount
            << " bytes="
            << static_cast<unsigned long long>(
                totalBytes
            )
            << " cellCapped="
            << cappedCellCount
            << " droppedLights="
            << static_cast<unsigned long long>(
                droppedLightCount
            )
            << " capLights="
            << LOCAL_LIGHT_GPU_MAX_LIGHTS;

        RTLog(
            ss.str()
        );


        std::stringstream gridLog;

        gridLog
            << "[RT LOCAL GRID P2B-1]"
            << " cells="
            << g_localLightGridCellCount
            << " indices="
            << g_localLightGridIndexCount
            << " hashCapacity="
            << g_localLightGridHashCapacity
            << " cellSize="
            << LOCAL_LIGHT_GRID_CELL_SIZE
            << " maxPerCell="
            << LOCAL_LIGHT_MAX_PER_CELL
            << " bytes="
            << static_cast<unsigned long long>(
                gridTotalBytes
            )
            << " binding=18";

        RTLog(
            gridLog.str()
        );
    }


    const double perfL10TotalMilliseconds =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() -
            perfL10RebuildStart
        ).count();

    if(localLightLogDue)
    {
        std::stringstream perfL10Log;
        perfL10Log
            << "[PERF L10 LOCAL REBUILD]"
            << " frame=" << g_perfL10FrameSerial
            << " totalCpuMs=" << std::fixed << std::setprecision(3)
            << perfL10TotalMilliseconds
            << " queueIdleMs=" << perfL10QueueIdleMilliseconds
            << " lights=" << g_localLightGpuCount
            << " gridCells=" << g_localLightGridCellCount
            << " gridBytes="
            << static_cast<unsigned long long>(gridTotalBytes);

        RTLog(
            perfL10Log.str()
        );
    }
}
