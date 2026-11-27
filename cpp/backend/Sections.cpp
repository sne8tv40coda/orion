// Orion backend — Sections 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// 2026-11-15：顶点地址槽的**存活位图 + 空闲表去重**。
// 动机（用户 2026-11-15 报告）：每局出现"固定一区块大小、永不恢复"的洞，位置
// 每局不同；段级转储显示 zeroTri=0（几何都在）⇒ 不是"几何没到"，而是命中着色
// 读错了顶点表。段退役时会把 vertexAddressSlot 推回空闲表，若同一次退役被推两次
// （退休代际重叠/重复回收），两个活着的段会拿到同一个槽 ⇒ 该段着色读到别人的顶点
// 缓冲 ⇒ 位置/法线错乱，视觉上就是一块洞。
// 这里把"同一槽同时被两个活段持有"（[SLOT COLLISION]）与"同一槽被回收两次"
// （[SLOT DUP]）都变成可观测事件，并顺手把重复入表挡掉。
namespace
{
std::vector<uint64_t> g_sectionSlotLiveBits;

bool slotLiveSet(uint32_t slot)
{
    const size_t word = slot >> 6u;
    const uint64_t bit = 1ull << (slot & 63u);

    if(g_sectionSlotLiveBits.size() <= word)
    {
        g_sectionSlotLiveBits.resize(word + 1u, 0ull);
    }

    if((g_sectionSlotLiveBits[word] & bit) != 0ull)
    {
        return true;   // 已经活着 = 冲突
    }

    g_sectionSlotLiveBits[word] |= bit;
    return false;
}

bool slotLiveClear(uint32_t slot)
{
    const size_t word = slot >> 6u;

    if(g_sectionSlotLiveBits.size() <= word)
    {
        return false;
    }

    const uint64_t bit = 1ull << (slot & 63u);

    if((g_sectionSlotLiveBits[word] & bit) == 0ull)
    {
        return false;   // 本来就不活着 = 重复回收
    }

    g_sectionSlotLiveBits[word] &= ~bit;
    return true;
}

bool slotFreeListContains(uint32_t slot)
{
    for(const uint32_t existing : g_sectionVertexAddressFreeSlots)
    {
        if(existing == slot)
        {
            return true;
        }
    }

    return false;
}
} // namespace


// 2026-10-14：新建池槽时的容量取整（见下方 adopt 的说明）。
//
// 目的：把"连续变化"的批次尺寸钉到少数几档，让池命中从"看运气"变成"几乎恒命中"。
// 实测（2026-10-14 那一局，流式加载那几秒）批次尺寸依次是 6.7 / 7.1 / 7.9 / 10.6 /
// 12.5 / 13.3 / 17.1 / 18.7 / 21 / 22.4MB —— 全部未命中、全部真分配，而单次分配
// 13.5~49.9ms **且与大小无关**（分配 0.2MB 也要 41ms ⇒ 是驱动在腾挪驻留）⇒ 那 4 秒
// 里 21 次分配合计 418ms，正好就是顿挫窗口。
// 取整到 2 的幂（1MB 起）后，一到两档就能覆盖上面整个范围；再配合 adopt 的"只顶掉
// 更小的空闲槽"，池会在两三批之内收敛到一个"够大的常驻槽"，此后命中率≈100%。
VkDeviceSize sectionBuildPoolSlotBytes(VkDeviceSize requiredBytes)
{
    VkDeviceSize bytes = 1048576u;
    while(bytes < requiredBytes)
    {
        bytes <<= 1u;
    }

    return bytes;
}
void prewarmVertexStagingPool()
{
    if(g_device == VK_NULL_HANDLE || g_physicalDevice == VK_NULL_HANDLE)
    {
        return;
    }

    constexpr VkDeviceSize kPrewarmBytes = 32u * 1024u * 1024u;
    constexpr size_t kPrewarmCount = 4u;
    VkBuffer prewarmBuffers[kPrewarmCount]{};

    for(size_t i = 0u; i < kPrewarmCount; ++i)
    {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        createBuffer(
            g_device,
            g_physicalDevice,
            kPrewarmBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            buf,
            mem,
            true
        );
        g_vertexStagingPool.adopt(buf, mem, kPrewarmBytes);
        prewarmBuffers[i] = buf;
    }

    for(size_t i = 0u; i < kPrewarmCount; ++i)
    {
        g_vertexStagingPool.release(prewarmBuffers[i]);
    }

    RTLog("[STAGING PREWARM] preallocated 4x32MB HOST_VISIBLE staging slots");

    constexpr VkDeviceSize kSmallPrewarmBytes = 1024u * 1024u;
    constexpr size_t kSmallPrewarmCount = 8u;
    VkBuffer smallPrewarmBuffers[kSmallPrewarmCount]{};

    for(size_t i = 0u; i < kSmallPrewarmCount; ++i)
    {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        createBuffer(
            g_device,
            g_physicalDevice,
            kSmallPrewarmBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            buf,
            mem,
            true
        );
        g_vertexStagingPool.adopt(buf, mem, kSmallPrewarmBytes);
        smallPrewarmBuffers[i] = buf;
    }

    for(size_t i = 0u; i < kSmallPrewarmCount; ++i)
    {
        g_vertexStagingPool.release(smallPrewarmBuffers[i]);
    }

    RTLog("[STAGING PREWARM] preallocated 4x32MB + 8x1MB HOST_VISIBLE staging slots");

    constexpr VkDeviceSize kMediumPrewarmBytes = 8u * 1024u * 1024u;
    constexpr size_t kMediumPrewarmCount = 4u;
    VkBuffer mediumPrewarmBuffers[kMediumPrewarmCount]{};

    for(size_t i = 0u; i < kMediumPrewarmCount; ++i)
    {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        createBuffer(
            g_device,
            g_physicalDevice,
            kMediumPrewarmBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            buf,
            mem,
            true
        );
        g_vertexStagingPool.adopt(buf, mem, kMediumPrewarmBytes);
        mediumPrewarmBuffers[i] = buf;
    }

    for(size_t i = 0u; i < kMediumPrewarmCount; ++i)
    {
        g_vertexStagingPool.release(mediumPrewarmBuffers[i]);
    }

    RTLog("[STAGING PREWARM] preallocated 4x32MB + 4x8MB + 8x1MB HOST_VISIBLE staging slots");

    if(SECTION_MEMORY_POOL_ENABLED)
    {
        VkBuffer probeBuffer = VK_NULL_HANDLE;
        VkBufferCreateInfo probeInfo{
            VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
        };
        probeInfo.size = 65536u;
        probeInfo.usage =
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            (g_ommNativeEnabled
                ? VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT
                : 0u);
        probeInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if(vkCreateBuffer(g_device, &probeInfo, nullptr, &probeBuffer) == VK_SUCCESS)
        {
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(g_device, probeBuffer, &req);
            vkDestroyBuffer(g_device, probeBuffer, nullptr);
            probeBuffer = VK_NULL_HANDLE;

            const uint32_t hostMemoryType = findHostUploadMemoryType(
                g_physicalDevice,
                req.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            );
            const VkDeviceSize align =
                std::max<VkDeviceSize>(req.alignment, 256u);

            constexpr VkDeviceSize kOmmPrewarmBuckets[3] = {
                65536u,    // 64 KB chunks -> 32 MB slab (512 chunks)
                524288u,   // 512 KB chunks -> 64 MB slab (128 chunks)
                2097152u   // 2 MB chunks -> 64 MB slab (32 chunks)
            };

            size_t prewarmedCount = 0u;
            VkDeviceSize prewarmedBytes = 0u;
            for(VkDeviceSize bucketBytes : kOmmPrewarmBuckets)
            {
                uint32_t slabIndex = 0u;
                if(createSectionMemorySlab(
                        bucketBytes, align, hostMemoryType, slabIndex))
                {
                    const VkDeviceSize slabCap =
                        g_sectionMemorySlabs[slabIndex].capacity;
                    prewarmedBytes += slabCap;
                    g_sectionMemoryPoolKeptBytes += slabCap;
                    ++prewarmedCount;
                }
            }

            RTLog(
                "[OMM SLAB PREWARM] preallocated " +
                std::to_string(prewarmedCount) +
                " host-upload slabs (" +
                std::to_string(prewarmedBytes / 1048576u) +
                " MB)"
            );
        }
    }
}
void destroyPooledSectionBuildBuffers()
{
    g_vertexStagingPool.releaseAll();
    g_blasScratchPool.releaseAll();
}
void releaseDeferredSectionBuildResources()
{
    if(
        g_deferredSectionBuild.commandBuffer != VK_NULL_HANDLE &&
        g_rtCommandPool != VK_NULL_HANDLE
    )
    {
        vkFreeCommandBuffers(
            g_device,
            g_rtCommandPool,
            1,
            &g_deferredSectionBuild.commandBuffer
        );
    }


    g_blasScratchPool.release(
        g_deferredSectionBuild.blasScratchBuffer
    );


    g_vertexStagingPool.release(
        g_deferredSectionBuild.vertexStagingBuffer
    );

    for(auto& input : g_deferredSectionBuild.ommBuildInputs)
    {
        destroyPooledBuffer(input.scratchBuffer, input.scratchBinding);
        destroyPooledBuffer(input.triangleBuffer, input.triangleBinding);
        destroyPooledBuffer(input.dataBuffer, input.dataBinding);
    }


    g_deferredSectionBuild = {};
}

// =====================================================
// 2026-10-23 TLAS 实例剔除**档位**（移植 Iris 26.1 的"投射者视锥"）。
//
// 改动前：两个滑条 lane 各自兼任开关 —— 距离 > 0 才剔距离、边距 > 0 才剔视锥，
// 而视锥那一档用的是**相机视锥**：它会把"相机身后、但能挡住太阳的地形"一起从
// TLAS 里拿掉 ⇒ 太阳阴影图查不到遮挡 ⇒ 光柱穿墙（2026-10-11 用光空间保留盒
// 兜了一手，只兜 ±160 格，见 67309 起的长注释）。
//
// 现在把"跑哪些测试"和"参数取值"分开：
//   -1 = 旧行为（默认，未设 ORION_TLAS_CULL 时逐位不变）
//    0 = 全关
//    1 = 只做距离
//    2 = 只做相机视锥（5 面，旧语义，对次级射线不安全）
//    3 = 距离 + 相机视锥
//    4 = 距离 + **投射者视锥**（13 面，ADSC）
//    5 = **只对主射线做掩码剔除**（实例保留，清 mask 0x01）—— 光追里唯一
//        不会漏光的那一档：阴影/GI/反射/太阳阴影图仍然能命中被掩码的实例。
//        mode 4 仍会"删实例"，那对次级射线是**删遮挡体**，转头时因结果冻结
//        会漏光；要跑剔除又怕漏光就用 5。
//
// 4 的保证与边界（别写过头）：ADSC 保留的是"影子能落进画面"的**投射者**，
// 这一条对太阳阴影图（rt_sun_shadow.comp）是充分的 —— 实测 5 个太阳方位、
// 1200 个随机点，凡影子进视锥的点 0 漏剔（.tmp/check_caster_frustum.py）。
// 但镜面反射/GI 的次级射线方向不受光方向约束，可能打到相机身后、影子投不进
// 画面的几何 ⇒ 那一类要靠边距 lane[7] 外扩兜住（默认 0 = 不外扩）。
// A/B：ORION_TLAS_CULL=0..4 覆盖；距离仍由 lane[6] 给半径（0 = 不跑距离档）。
// =====================================================
int tlasCullMode()
{
    static const int s_mode = []() -> int
    {
        const char* raw = std::getenv("ORION_TLAS_CULL");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 4;
        }

        return std::min(std::max(std::atoi(raw), 0), 5);
    }();

    return s_mode;
}

// 2026-10-23 ±160 光空间保留盒的开关（A/B 用，默认开 = 现状）。
//
// 盒子当初（2026-10-11）是为了修"体积光穿墙"：相机视锥剔除会把"相机身后、
// 但能挡住太阳的地形"一起剔掉。ADSC 投射者视锥（:67519 起）用构造保证了
// 同一件事，所以对**近场太阳阴影**盒子是冗余的。
//
// 不能直接删，因为还有全向消费者吃相机背后的几何（见 patch_keepbox_switch.py
// 的注释）：高度场 ±256（云/体积光读）、水面模拟 48 格、焦散/GI/镜面反射。
// 所以关盒子的同时必须给一个近场保底球，两者一起用：
//     ORION_TLAS_CULL=4 ORION_TLAS_KEEPBOX=0 [ORION_TLAS_NEAR_GUARD=24]
bool tlasKeepBoxEnabled()
{
    static const bool s_enabled = []() -> bool
    {
        const char* raw = std::getenv("ORION_TLAS_KEEPBOX");

        if(raw == nullptr || raw[0] == '\0')
        {
            return false;
        }

        return raw[0] != '0';
    }();

    return s_enabled;
}

// 近场保底半径（格）：半径内的 Section 永不剔除。
// 默认 64 —— 安全覆盖 48 格水面模拟、30 格局部光、室内与近场 GI/反射。
float tlasNearGuardBlocks()
{
    static const float s_blocks = []() -> float
    {
        const char* raw = std::getenv("ORION_TLAS_NEAR_GUARD");

        if(raw == nullptr)
        {
            return 64.0f;
        }

        const float parsed = static_cast<float>(std::atof(raw));

        return parsed >= 0.0f ? parsed : 64.0f;
    }();

    return s_blocks;
}


void createTestVertexBuffer(){
    std::cout
    << "[VERTEX SIZE] "
    << sizeof(Vertex)
    << std::endl;
    const Vertex v[] = {
        { -500.0f, -500.0f, -50.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f },
        {  500.0f, -500.0f, -50.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f },
        {    0.0f,  500.0f, -50.0f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f }
    };

    createBuffer(g_device,g_physicalDevice,sizeof(v),
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    g_testVertexBuffer,g_testVertexMemory);

    void* mapped=nullptr;
    check(vkMapMemory(g_device,g_testVertexMemory,0,sizeof(v),0,&mapped),"Failed map vertex");
    memcpy(mapped,v,sizeof(v));
    Vertex* checkVertex = static_cast<Vertex*>(mapped);

    std::cout
        << "[CPU BUFFER CHECK] "
        << checkVertex[0].x << " "
        << checkVertex[0].y << " "
        << checkVertex[0].z
        << std::endl;
    vkUnmapMemory(g_device,g_testVertexMemory);
    g_testVertexDeviceAddress=getBufferDeviceAddress(g_device,g_testVertexBuffer);
    VkBufferDeviceAddressInfo checkInfo{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO
    };

    checkInfo.buffer = g_testVertexBuffer;

    auto addr2 = vkGetBufferDeviceAddress(
        g_device,
        &checkInfo
    );

    std::cerr
    <<"[BDA DOUBLE CHECK] "
    <<std::hex
    <<addr2
    <<std::endl;
    std::cerr
<< "[CREATE TEST BUFFER BDA] "
<< g_testVertexDeviceAddress
<< std::endl;
}


// Initialization-only fallback BLAS. It keeps the pre-world TLAS and the
// first descriptor write valid; once terrain Sections arrive, the dynamic
// multi-Section path replaces the TLAS before tracing the world.
void createBLAS()
{
    auto getBuildSizes =
        getGetAccelerationStructureBuildSizes();

    auto createAS =
        getCreateAccelerationStructure();

    auto getASAddress =
        getGetAccelerationStructureDeviceAddress();

    auto cmdBuild =
        getCmdBuildAccelerationStructures();


    if(
        !getBuildSizes ||
        !createAS ||
        !getASAddress ||
        !cmdBuild
    )
    {
        throw std::runtime_error(
            "initial BLAS Vulkan functions unavailable"
        );
    }


    VkAccelerationStructureGeometryTrianglesDataKHR triangles{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR
    };

    triangles.vertexFormat =
        VK_FORMAT_R32G32B32_SFLOAT;

    triangles.vertexData.deviceAddress =
        g_testVertexDeviceAddress;

    triangles.vertexStride =
        sizeof(Vertex);

    triangles.maxVertex =
        2;

    triangles.indexType =
        VK_INDEX_TYPE_NONE_KHR;


    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR
    };

    geometry.geometryType =
        VK_GEOMETRY_TYPE_TRIANGLES_KHR;

    geometry.flags = 0;
    geometry.geometry.triangles =
        triangles;


    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
    };

    buildInfo.type =
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    buildInfo.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

    buildInfo.mode =
        VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

    buildInfo.geometryCount = 1;
    buildInfo.pGeometries =
        &geometry;


    const uint32_t primitiveCount =
        1;


    VkAccelerationStructureBuildSizesInfoKHR sizesInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };

    getBuildSizes(
        g_device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo,
        &primitiveCount,
        &sizesInfo
    );


    createBuffer(
        g_device,
        g_physicalDevice,
        sizesInfo.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_blasBuffer,
        g_blasMemory
    );


    VkAccelerationStructureCreateInfoKHR createInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR
    };

    createInfo.buffer =
        g_blasBuffer;

    createInfo.size =
        sizesInfo.accelerationStructureSize;

    createInfo.type =
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    check(
        createAS(
            g_device,
            &createInfo,
            nullptr,
            &g_blas
        ),
        "create initial BLAS failed"
    );


    VkDeviceAddress scratchAddress = 0;

    if(sizesInfo.buildScratchSize > 0)
    {
        createBuffer(
            g_device,
            g_physicalDevice,
            sizesInfo.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            g_blasScratchBuffer,
            g_blasScratchMemory
        );

        scratchAddress =
            getBufferDeviceAddress(
                g_device,
                g_blasScratchBuffer
            );
    }


    buildInfo.dstAccelerationStructure =
        g_blas;

    buildInfo.scratchData.deviceAddress =
        scratchAddress;


    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount =
        primitiveCount;

    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] =
    {
        &range
    };


    VkCommandBufferAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
    };

    allocateInfo.commandPool =
        g_rtCommandPool;

    allocateInfo.level =
        VK_COMMAND_BUFFER_LEVEL_PRIMARY;

    allocateInfo.commandBufferCount =
        1;


    VkCommandBuffer cmd =
        VK_NULL_HANDLE;

    check(
        vkAllocateCommandBuffers(
            g_device,
            &allocateInfo,
            &cmd
        ),
        "allocate initial BLAS command buffer failed"
    );


    VkCommandBufferBeginInfo beginInfo{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
    };

    beginInfo.flags =
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    check(
        vkBeginCommandBuffer(
            cmd,
            &beginInfo
        ),
        "begin initial BLAS command buffer failed"
    );

    cmdBuild(
        cmd,
        1,
        &buildInfo,
        ranges
    );

    check(
        vkEndCommandBuffer(cmd),
        "end initial BLAS command buffer failed"
    );


    VkSubmitInfo submitInfo{
        VK_STRUCTURE_TYPE_SUBMIT_INFO
    };

    submitInfo.commandBufferCount =
        1;

    submitInfo.pCommandBuffers =
        &cmd;

    check(
        vkQueueSubmit(
            g_queue,
            1,
            &submitInfo,
            VK_NULL_HANDLE
        ),
        "submit initial BLAS build failed"
    );

    check(
        vkQueueWaitIdle(g_queue),
        "wait initial BLAS build failed"
    );

    vkFreeCommandBuffers(
        g_device,
        g_rtCommandPool,
        1,
        &cmd
    );


    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
    };

    addressInfo.accelerationStructure =
        g_blas;

    g_blasDeviceAddress =
        getASAddress(
            g_device,
            &addressInfo
        );
}
bool queryVramBudget(VramBudgetSample& out)
{
    out = VramBudgetSample{};

    if(g_physicalDevice == VK_NULL_HANDLE)
    {
        return false;
    }

    VkPhysicalDeviceMemoryBudgetPropertiesEXT memoryBudget{};
    memoryBudget.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    VkPhysicalDeviceMemoryProperties2 memoryProperties2{};
    memoryProperties2.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    memoryProperties2.pNext = &memoryBudget;
    vkGetPhysicalDeviceMemoryProperties2(g_physicalDevice, &memoryProperties2);

    for(uint32_t heapIndex = 0u;
        heapIndex < memoryProperties2.memoryProperties.memoryHeapCount;
        ++heapIndex)
    {
        if((memoryProperties2.memoryProperties.memoryHeaps[heapIndex].flags &
            VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0u)
        {
            continue;
        }

        out.usedMb +=
            static_cast<double>(memoryBudget.heapUsage[heapIndex]) / 1048576.0;
        out.budgetMb +=
            static_cast<double>(memoryBudget.heapBudget[heapIndex]) / 1048576.0;
    }

    return out.budgetMb > 0.0;
}

// 0 = 关 / 1 = 只做甲 / 2 = 甲 + 乙（**默认**）
//
// 2026-10-13：默认从 1 改成 2。理由：用户实测一期（slab 清理 + VLI 上限）**跑满了也压不住**
// ——"区块一高还是爆显存"，因为涨的是活的几何（meshSec 357 → 1137 → 峰值 2812）。
// 关掉乙就等于这次改动对他无效。乙的所有守卫都是保守的（保护半径 / 阴影保留盒 /
// 严格视锥 / 只处理真实世界高度 Section），且 `=1` 就是用户已经验证过的一期行为。
int vramGovernorMode()
{
    static const int cached = []()
    {
        const char* raw = std::getenv("ORION_VRAM_GOVERNOR");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 2;
        }

        const int parsed = std::atoi(raw);

        if(parsed <= 0)
        {
            return 0;
        }

        return parsed >= 2 ? 2 : 1;
    }();

    return cached;
}

// 阈值（0..1，越界即回落默认）。
//
// 2026-10-13：默认从 进 85% / 出 75% 下调为 **进 72% / 出 62%**。
// 依据（19:39 那一局，8GB 卡 / RTX 4070 Laptop，总显存 8188MB，驱动给本进程的预算
// 约 6945MB）：**ratio=0.849 时 DLSS 就已经 `eWarnOutOfVRAM`（该局 37 次）** ——
// 也就是 85% 这个阈值本身没给 DLSS-RR/帧生成留下它们需要的余量，等闸门在 85% 动作
// 已经晚了：它一动作，DLSS 早就分配失败、走查在 53798 放弃整帧 ⇒ 画面退回原版。
// 而 19:46 那局稳稳待在 0.558~0.656 且全程正常 ⇒ 72% 是高水位附近一个可用的目标。
// 两值都仍可用 ORION_VRAM_GOVERNOR_HIGH / _LOW 覆盖，便于继续标定。
double vramGovernorHighWater()
{
    static const double cached = []()
    {
        const char* raw = std::getenv("ORION_VRAM_GOVERNOR_HIGH");

        if(raw == nullptr || raw[0] == '\0')
        {
            // 2026-11-06f：0.80 → 0.82。实测 16 区块跑图的稳态 ratio 已经到 0.808，
            // 贴着 0.80 会让 over 在边缘反复进出（nonResident 又顶到 guard 3000）。
            // 0.82 仍低于注释记录的 DLSS eWarnOutOfVRAM 线（0.849）。
            // 2026-11-06c：0.72 → 0.80。
            // 0.72 是按"驱逐能降 used"标定的，但驱逐只释放 BLAS（实测 asBlasMB
            // 仅 144MB / usedMB 5077）⇒ ratio 只能降 2.8%，永远回不到 lowWater
            // 0.62 ⇒ over 永久为真、无限驱逐、视野内区块被清空（实测 nonResident
            // 4637 / blas 只剩 2038）。0.80 仍低于注释记录的 DLSS
            // eWarnOutOfVRAM 线（0.849），保留余量。
            // 2026-11-06t?0.82 -> 0.84?
            // ?????? 288 ?????? ~7300??????? 0.803~0.822?
            // ???? 0.82 ? over ???? keepCap ?? 0????????
            // ??? 56 ?????? 110ms?poolMiss 0?56 / allocMs 0?109.9??
            // 0.84 ???????? DLSS eWarnOutOfVRAM ? 0.849?
            return 0.84;
        }

        const double parsed = std::atof(raw);
        return (parsed > 0.0 && parsed <= 1.0) ? parsed : 0.84;
    }();

    return cached;
}
double vramGovernorLowWater()
{
    static const double cached = []()
    {
        const char* raw = std::getenv("ORION_VRAM_GOVERNOR_LOW");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 0.62;
        }

        const double parsed = std::atof(raw);
        return (parsed > 0.0 && parsed < 1.0) ? parsed : 0.62;
    }();

    return cached;
}

// =====================================================
// 2026-10-13：VLI 页池的容量上限改为**由预算反推**（甲·二级动作的容量侧）
//
// 旧上限是固定的 VLI_MAX_SECTION_PAGES = 4096 页，每页约 185KB（见 vliPoolBytesForCapacity
// 与 [VLI-1.4] 的 bytes= 字段）⇒ 顶满就是 ~758MB，**与显存预算完全无关**。
// 在实测 ~7GB 的预算下那是 11%，属于"悄悄吃掉"。
//
// 现在：上限页数 = budgetMB × fraction / 每页字节，再夹在
// [VLI_INITIAL_SECTION_PAGES, VLI_MAX_SECTION_PAGES] 之间。
//   ORION_VLI_BUDGET_FRACTION  默认 0.08（8%）
// 闸门不可用（驱动不支持 VK_EXT_memory_budget）时返回 VLI_MAX_SECTION_PAGES ⇒
// 与改动前逐位一致。
//
// ⚠ 这是**上限**：只决定"最多涨到多少页"，不主动缩容（缩容要动池缓冲的生命周期，
//   收益不值那个风险）。被上限挡住时：超预算走页淘汰（evictFarthestVLIPageLocked），
//   未超预算走旧行为（丢掉新 Section，见 applyVLISectionVoxelsLocked）。
// =====================================================
double vliBudgetFraction()
{
    static const double cached = []()
    {
        const char* raw = std::getenv("ORION_VLI_BUDGET_FRACTION");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 0.08;
        }

        const double parsed = std::atof(raw);
        return (parsed > 0.0 && parsed <= 0.5) ? parsed : 0.08;
    }();

    return cached;
}
uint32_t vliPageCapacityLimit()
{
    if(!g_vramGovernor.available || g_vramGovernor.budgetMb <= 0.0)
    {
        return VLI_MAX_SECTION_PAGES;
    }

    const double bytesPerPage =
        static_cast<double>(vliPoolBytesForCapacity(1u));

    if(bytesPerPage <= 0.0)
    {
        return VLI_MAX_SECTION_PAGES;
    }

    const double allowedBytes =
        g_vramGovernor.budgetMb * 1048576.0 * vliBudgetFraction();

    double pages = std::floor(allowedBytes / bytesPerPage);

    if(pages < static_cast<double>(VLI_INITIAL_SECTION_PAGES))
    {
        pages = static_cast<double>(VLI_INITIAL_SECTION_PAGES);
    }

    if(pages > static_cast<double>(VLI_MAX_SECTION_PAGES))
    {
        pages = static_cast<double>(VLI_MAX_SECTION_PAGES);
    }

    return static_cast<uint32_t>(pages);
}
uint32_t sectionPoolChunkWasteLog2()
{
    static const uint32_t cached = []() -> uint32_t
    {
        const char* raw =
            std::getenv("ORION_SECTION_CHUNK_WASTE_LOG2");

        if(raw == nullptr || raw[0] == '\0')
        {
            return SECTION_POOL_CHUNK_WASTE_LOG2_DEFAULT;
        }

        const long parsed = std::strtol(raw, nullptr, 10);

        if(parsed < 0)
        {
            return 0u;
        }

        if(parsed > 8)
        {
            return 8u;
        }

        return static_cast<uint32_t>(parsed);
    }();

    return cached;
}
VkDeviceSize sectionMemoryPoolEffectiveKeepBytes()
{
    if(!g_vramGovernor.available)
    {
        return SECTION_MEMORY_POOL_MAX_KEEP_BYTES;
    }

    return g_vramGovernor.over
        ? 0u
        : SECTION_MEMORY_POOL_RESTORED_KEEP_BYTES;
}
size_t sectionMemoryPoolEffectiveKeepSlabs()
{
    if(!g_vramGovernor.available)
    {
        return SECTION_MEMORY_POOL_MAX_KEEP_SLABS;
    }

    return g_vramGovernor.over
        ? 0u
        : SECTION_MEMORY_POOL_RESTORED_KEEP_SLABS;
}
VkDeviceSize sectionMemoryPoolBucketBytesByIndex(uint32_t bucket)
{
    const uint32_t exponent =
        SECTION_MEMORY_POOL_MIN_EXP + bucket / 2u;
    const VkDeviceSize base =
        static_cast<VkDeviceSize>(1u) << exponent;

    return (bucket % 2u) == 0u
        ? base
        : (base + base / 2u);
}
uint32_t sectionMemoryPoolBucketIndex(VkDeviceSize need)
{
    uint32_t bucket = 0u;

    while(
        bucket + 1u < SECTION_MEMORY_POOL_BUCKET_COUNT &&
        sectionMemoryPoolBucketBytesByIndex(bucket) < need
    )
    {
        ++bucket;
    }

    return bucket;
}
VkDeviceSize sectionMemoryPoolBucketBytes(VkDeviceSize need)
{
    return sectionMemoryPoolBucketBytesByIndex(
        sectionMemoryPoolBucketIndex(need)
    );
}

// 新建一块 slab（**唯一**真 vkAllocateMemory 的地方）。
// 成功返回 true 并写出 slab 下标；分配失败（典型 = OUT_OF_DEVICE_MEMORY）
// 返回 false，**不 throw**（见上方 SectionMemoryUnavailableError 的说明）。
bool createSectionMemorySlab(
    VkDeviceSize need,
    VkDeviceSize requiredAlignment,
    uint32_t memoryTypeIndex,
    uint32_t& outSlabIndex)
{
    const VkDeviceSize minChunk =
        std::max(need, requiredAlignment);

    VkDeviceSize chunkBytes =
        sectionMemoryPoolBucketBytes(minChunk);

    if(requiredAlignment > 1u &&
       (chunkBytes % requiredAlignment) != 0u)
    {
        chunkBytes =
            ((chunkBytes + requiredAlignment - 1u) / requiredAlignment) *
            requiredAlignment;
    }

    VkDeviceSize chunkCount =
        SECTION_MEMORY_SLAB_TARGET_BYTES / chunkBytes;

    if(chunkCount < 1u)
    {
        chunkCount = 1u;
    }

    if(chunkCount > SECTION_MEMORY_SLAB_MAX_CHUNKS)
    {
        chunkCount = SECTION_MEMORY_SLAB_MAX_CHUNKS;
    }

    if(!SECTION_MEMORY_POOL_ENABLED)
    {
        chunkCount = 1u;
    }

    VkMemoryAllocateFlagsInfo allocateFlags{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO
    };

    allocateFlags.flags =
        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };

    allocateInfo.pNext =
        &allocateFlags;

    allocateInfo.allocationSize =
        chunkBytes * chunkCount;

    allocateInfo.memoryTypeIndex =
        memoryTypeIndex;

    VkDeviceMemory memory =
        VK_NULL_HANDLE;

    const std::chrono::steady_clock::time_point allocateStart =
        std::chrono::steady_clock::now();

    const VkResult slabAllocResult =
        vkAllocateMemory(
            g_device,
            &allocateInfo,
            nullptr,
            &memory
        );

    if(slabAllocResult != VK_SUCCESS)
    {
        RTLogFlushed(
            std::string("[SECTION OOM] allocate Section memory slab failed VkResult=") +
            std::to_string(static_cast<int>(slabAllocResult)) +
            " bytes=" +
            std::to_string(
                static_cast<unsigned long long>(allocateInfo.allocationSize)
            )
        );
        return false;
    }

    g_sectionMemoryAllocateMs +=
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - allocateStart
        ).count();

    ++g_sectionMemoryAllocateCalls;

    uint32_t slabIndex = 0u;
    bool foundDeadSlot = false;

    for(size_t index = 0u; index < g_sectionMemorySlabs.size(); ++index)
    {
        if(g_sectionMemorySlabs[index].memory == VK_NULL_HANDLE)
        {
            slabIndex = static_cast<uint32_t>(index);
            foundDeadSlot = true;

            break;
        }
    }

    if(!foundDeadSlot)
    {
        slabIndex = static_cast<uint32_t>(g_sectionMemorySlabs.size());
        g_sectionMemorySlabs.push_back(SectionMemorySlab{});
    }

    SectionMemorySlab& slab =
        g_sectionMemorySlabs[slabIndex];

    slab.memory = memory;
    slab.chunkBytes = chunkBytes;
    slab.capacity = allocateInfo.allocationSize;

    g_sectionMemoryAllocateBytes +=
        static_cast<uint64_t>(allocateInfo.allocationSize);
    slab.memoryTypeIndex = memoryTypeIndex;
    slab.chunkCount = static_cast<uint32_t>(chunkCount);
    slab.liveChunks = 0u;
    slab.freeChunks.clear();
    slab.freeChunks.reserve(slab.chunkCount);

    for(uint32_t chunk = 0u; chunk < slab.chunkCount; ++chunk)
    {
        slab.freeChunks.push_back(chunk);
    }

    outSlabIndex = slabIndex;
    return true;
}


// 从已有 slab 里取一块 chunk。命中返回 true。
//
// 2026-10-10 新增 maxChunkWasteLog2：可选的"浪费上限"，只接受
// chunkBytes <= bucketBytes(max(need, 对齐)) << maxChunkWasteLog2 的 slab。
// 默认 32 = 不设限（几何请求一旦显式收紧就按上限走）。
// OMM 缓冲池化传 2：它的请求只有几十 KB，而几何 slab 的 chunk 动辄 MB 级，
// 不设限的话一个 36KB 的请求会从 4MB-chunk 的 slab 里切走整 4MB。
//
// 2026-10-13 修正：上限的基准必须是**本次请求**（minChunk），不能是循环变量。
// 旧写法是 slab.chunkBytes > (wantedChunkBytes << maxChunkWasteLog2)，而
// wantedChunkBytes 随 exponent 逐轮翻倍 ⇒ 任何一个超大 chunk 都能在更高的
// exponent 轮次上通过校验（循环一直跑到 SECTION_MEMORY_POOL_MAX_EXP），
// **于是上限等于不存在** —— OMM 那句"传 2 就不会切走整 4MB"其实从未生效，
// 几何这一路也就一路无上限地向上找大块。这正是上面 SECTION_POOL_CHUNK_
// WASTE_LOG2_DEFAULT 注释里 asWasteMB=1170.6MB 的来源。
bool takeSectionChunkFromSlabs(
    VkDeviceSize need,
    VkDeviceSize requiredAlignment,
    uint32_t memoryTypeIndex,
    VkDeviceMemory& outMemory,
    VkDeviceSize& outChunkBase,
    VkDeviceSize& outChunkBytes,
    uint32_t& outSlabIndex,
    uint32_t& outChunkIndex,
    uint32_t maxChunkWasteLog2)
{
    if(!SECTION_MEMORY_POOL_ENABLED)
    {
        return false;
    }

    const VkDeviceSize minChunk =
        std::max(need, requiredAlignment);

    const VkDeviceSize wasteCapBytes =
        maxChunkWasteLog2 >= 32u
            ? std::numeric_limits<VkDeviceSize>::max()
            : (sectionMemoryPoolBucketBytes(minChunk)
               << maxChunkWasteLog2);

    for(int pass = 0; pass < 2; ++pass)
    {
        const bool exactOnly = (pass == 0);

        for(
            uint32_t bucket = sectionMemoryPoolBucketIndex(minChunk);
            bucket < SECTION_MEMORY_POOL_BUCKET_COUNT;
            ++bucket
        )
        {
        const VkDeviceSize wantedChunkBytes =
            sectionMemoryPoolBucketBytesByIndex(bucket);

        if(wantedChunkBytes > wasteCapBytes)
        {
            break;
        }

        for(size_t index = 0u; index < g_sectionMemorySlabs.size(); ++index)
        {
            SectionMemorySlab& slab =
                g_sectionMemorySlabs[index];

            if(slab.memory == VK_NULL_HANDLE || slab.freeChunks.empty())
            {
                continue;
            }

            if(slab.memoryTypeIndex != memoryTypeIndex)
            {
                continue;
            }

            if(slab.chunkBytes < requiredAlignment ||
               (exactOnly
                    ? (slab.chunkBytes != wantedChunkBytes)
                    : (slab.chunkBytes < wantedChunkBytes)))
            {
                continue;
            }

            if(slab.chunkBytes > wasteCapBytes)
            {
                continue;
            }

            const uint32_t chunkIndex =
                slab.freeChunks.back();

            slab.freeChunks.pop_back();
            ++slab.liveChunks;

            outMemory = slab.memory;
            outChunkBase =
                static_cast<VkDeviceSize>(chunkIndex) * slab.chunkBytes;
            outChunkBytes = slab.chunkBytes;
            outSlabIndex = static_cast<uint32_t>(index);
            outChunkIndex = chunkIndex;

            ++g_sectionMemoryPoolHits;

            return true;
        }
        }
    }

    return false;
}


// 统一入口：先复用 chunk，未命中就建新 slab。
//
// maxChunkWasteLog2 见 takeSectionChunkFromSlabs：默认 32 = 不设限；OMM 池化传
// OMM_POOL_CHUNK_WASTE_LOG2，Section 顶点/BLAS 传 sectionPoolChunkWasteLog2()
// （默认 1，可用 ORION_SECTION_CHUNK_WASTE_LOG2 覆盖）。
bool acquireSectionMemory(
    VkDeviceSize need,
    VkDeviceSize requiredAlignment,
    uint32_t memoryTypeIndex,
    VkDeviceMemory& outMemory,
    VkDeviceSize& outChunkBase,
    VkDeviceSize& outChunkBytes,
    uint32_t& outSlabIndex,
    uint32_t& outChunkIndex,
    uint32_t maxChunkWasteLog2 )
{
    if(
        takeSectionChunkFromSlabs(
            need,
            requiredAlignment,
            memoryTypeIndex,
            outMemory,
            outChunkBase,
            outChunkBytes,
            outSlabIndex,
            outChunkIndex,
            maxChunkWasteLog2
        )
    )
    {
        return true;
    }

    ++g_sectionMemoryPoolMisses;

    uint32_t slabIndex = 0u;
    if(!createSectionMemorySlab(
            need,
            requiredAlignment,
            memoryTypeIndex,
            slabIndex
        ))
    {
        return false;
    }

    SectionMemorySlab& slab =
        g_sectionMemorySlabs[slabIndex];

    if(slab.freeChunks.empty())
    {
        return false;
    }

    const uint32_t chunkIndex =
        slab.freeChunks.back();

    slab.freeChunks.pop_back();
    ++slab.liveChunks;

    outMemory = slab.memory;
    outChunkBase =
        static_cast<VkDeviceSize>(chunkIndex) * slab.chunkBytes;
    outChunkBytes = slab.chunkBytes;
    outSlabIndex = slabIndex;
    outChunkIndex = chunkIndex;

    return true;
}

// 归还一个 chunk（调用点已证明 GPU 不再读它：退役环的 fence 已信号，见
// releaseRetirementSlot）。整块 slab 空掉时按保留策略决定留用还是真释放。
// 2026-11-23 CPU批②：slab 是否落在主机内存堆（非 device-local）。
// 内存属性按物理设备缓存一次；开关关闭时恒返回 false ⇒ 所有 slab 归设备类，逐位旧行为。
static bool sectionSlabIsHostHeap(uint32_t memoryTypeIndex)
{
    if constexpr(!CPUPERF_HOST_SLAB_KEEP)
    {
        return false;
    }

    static VkPhysicalDevice s_cachedPhysicalDevice = VK_NULL_HANDLE;
    static VkPhysicalDeviceMemoryProperties s_cachedProperties{};

    if(g_physicalDevice == VK_NULL_HANDLE)
    {
        return false;
    }

    if(s_cachedPhysicalDevice != g_physicalDevice)
    {
        vkGetPhysicalDeviceMemoryProperties(
            g_physicalDevice, &s_cachedProperties);
        s_cachedPhysicalDevice = g_physicalDevice;
    }

    if(memoryTypeIndex >= s_cachedProperties.memoryTypeCount)
    {
        return false;
    }

    const uint32_t heapIndex =
        s_cachedProperties.memoryTypes[memoryTypeIndex].heapIndex;

    return (s_cachedProperties.memoryHeaps[heapIndex].flags &
            VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0u;
}
static size_t sectionSlabClassKeepSlabs(bool hostClass)
{
    return hostClass
        ? SECTION_MEMORY_POOL_HOST_KEEP_SLABS
        : sectionMemoryPoolEffectiveKeepSlabs();
}
static VkDeviceSize sectionSlabClassKeepBytes(bool hostClass)
{
    return hostClass
        ? SECTION_MEMORY_POOL_HOST_KEEP_BYTES
        : sectionMemoryPoolEffectiveKeepBytes();
}
void releaseSectionMemory(
    uint32_t slabIndex,
    uint32_t chunkIndex)
{
    if(slabIndex >= g_sectionMemorySlabs.size())
    {
        return;
    }

    SectionMemorySlab& slab =
        g_sectionMemorySlabs[slabIndex];

    if(slab.memory == VK_NULL_HANDLE || chunkIndex >= slab.chunkCount)
    {
        return;
    }

    slab.freeChunks.push_back(chunkIndex);

    if(slab.liveChunks > 0u)
    {
        --slab.liveChunks;
    }

    if(slab.liveChunks != 0u)
    {
        return;
    }

    if(SECTION_MEMORY_POOL_ENABLED)
    {
        // 2026-11-23 CPU批②：按「主机堆 / 设备堆」分类计数，各用各的上限。
        // 开关关闭时 hostClass 恒 false、类内计数 = 全体计数 ⇒ 与旧判据逐位一致。
        const bool hostClass = sectionSlabIsHostHeap(slab.memoryTypeIndex);

        VkDeviceSize keptBytes = 0u;
        VkDeviceSize classKeptBytes = 0u;
        size_t classKeptSlabs = 0u;

        for(const SectionMemorySlab& other : g_sectionMemorySlabs)
        {
            if(other.memory != VK_NULL_HANDLE && other.liveChunks == 0u)
            {
                keptBytes += other.capacity;

                if(sectionSlabIsHostHeap(other.memoryTypeIndex) == hostClass)
                {
                    classKeptBytes += other.capacity;
                    ++classKeptSlabs;
                }
            }
        }

        g_sectionMemoryPoolKeptBytes = keptBytes;

        if(classKeptSlabs <= sectionSlabClassKeepSlabs(hostClass) &&
           classKeptBytes <= sectionSlabClassKeepBytes(hostClass))
        {
            return;
        }
    }

    g_sectionMemoryFreeBytes += static_cast<uint64_t>(slab.capacity);

    vkFreeMemory(
        g_device,
        slab.memory,
        nullptr
    );

    slab.memory = VK_NULL_HANDLE;
    slab.chunkBytes = 0u;
    slab.capacity = 0u;
    slab.chunkCount = 0u;
    slab.liveChunks = 0u;
    slab.freeChunks.clear();
}


// =====================================================
// 2026-10-13：主动归还空闲 slab（甲·一级动作）
//
// releaseSectionMemory() 只在**恰好有 chunk 被归还**时才会走到"整块空了"的判定，
// 所以它治不了"已经积了一批空闲 slab、但暂时没有新的归还发生"这种情形 —— 而闸门
// 要求的是越线时立刻还。这里按有效上限扫一遍，把超出的空闲 slab 真正 vkFreeMemory。
//
// 安全性：只碰 liveChunks == 0 的 slab。这些 slab 的 chunk 已全部由退役环（fence
// 语义）归还，GPU 不可能还在读它们 —— 与 releaseSectionMemory 里那条整块释放的
// 前提完全相同，没有引入新的时序约束。
//
// 返回值 = 本次真正还给驱动的 MB（供 [VRAM GOV] 记账）。
// =====================================================
double trimSectionMemoryPool()
{
    if(!SECTION_MEMORY_POOL_ENABLED)
    {
        return 0.0;
    }

    // 2026-11-23 CPU批②：两类（0=设备堆，1=主机堆）各自对照各自的上限。
    // 开关关闭时全部 slab 归 0 类、上限即旧的有效上限 ⇒ 行为与旧版一致。
    VkDeviceSize keptBytes = 0u;
    VkDeviceSize classKeptBytes[2] = {0u, 0u};
    size_t classKeptSlabs[2] = {0u, 0u};

    for(const SectionMemorySlab& slab : g_sectionMemorySlabs)
    {
        if(slab.memory != VK_NULL_HANDLE && slab.liveChunks == 0u)
        {
            const size_t cls =
                sectionSlabIsHostHeap(slab.memoryTypeIndex) ? 1u : 0u;

            keptBytes += slab.capacity;
            classKeptBytes[cls] += slab.capacity;
            ++classKeptSlabs[cls];
        }
    }

    const auto classUnderCap = [&](size_t cls)
    {
        return classKeptSlabs[cls] <= sectionSlabClassKeepSlabs(cls == 1u) &&
               classKeptBytes[cls] <= sectionSlabClassKeepBytes(cls == 1u);
    };

    if(classUnderCap(0u) && classUnderCap(1u))
    {
        g_sectionMemoryPoolKeptBytes = keptBytes;
        return 0.0;
    }

    double freedMb = 0.0;

    for(SectionMemorySlab& slab : g_sectionMemorySlabs)
    {
        if(classUnderCap(0u) && classUnderCap(1u))
        {
            break;
        }

        if(slab.memory == VK_NULL_HANDLE || slab.liveChunks != 0u)
        {
            continue;
        }

        const size_t cls =
            sectionSlabIsHostHeap(slab.memoryTypeIndex) ? 1u : 0u;

        if(classUnderCap(cls))
        {
            continue;
        }

        g_sectionMemoryFreeBytes += static_cast<uint64_t>(slab.capacity);
        freedMb += static_cast<double>(slab.capacity) / 1048576.0;

        vkFreeMemory(
            g_device,
            slab.memory,
            nullptr
        );

        keptBytes -= slab.capacity;
        classKeptBytes[cls] -= slab.capacity;
        --classKeptSlabs[cls];

        slab.memory = VK_NULL_HANDLE;
        slab.chunkBytes = 0u;
        slab.capacity = 0u;
        slab.chunkCount = 0u;
        slab.liveChunks = 0u;
        slab.freeChunks.clear();

        if(!g_vramGovernor.over)
        {
            break;
        }
    }

    g_sectionMemoryPoolKeptBytes = keptBytes;

    return freedMb;
}


// =====================================================
// 2026-10-13：闸门 tick 与日志（甲）
//
// 调用点只在"几何有脏活"的那一帧（rebuildWorldGeometry 里 % 600 计数器旁边）——
// 闸门只在我们要分配的时候才需要动作，空闲时它没有事可做，这样也就不必为它引入
// 一条新的每帧路径。
// =====================================================
void updateVramGovernor()
{
    if(vramGovernorMode() < 1)
    {
        g_vramGovernor.enabled = false;
        return;
    }

    g_vramGovernor.enabled = true;
    ++g_vramGovernor.tickCount;

    const bool needSample =
        g_vramGovernor.sampleCount == 0u ||
        (g_vramGovernor.tickCount % VRAM_GOVERNOR_SAMPLE_TICKS) == 0u;

    if(needSample)
    {
        VramBudgetSample sample;
        g_vramGovernor.available = queryVramBudget(sample);
        g_vramGovernor.usedMb = sample.usedMb;
        g_vramGovernor.budgetMb = sample.budgetMb;
        ++g_vramGovernor.sampleCount;
    }

    if(!g_vramGovernor.available)
    {
        return;
    }

    const double ratio =
        g_vramGovernor.usedMb / g_vramGovernor.budgetMb;

    if(!g_vramGovernor.over)
    {
        if(ratio > vramGovernorHighWater())
        {
            g_vramGovernor.over = true;
            ++g_vramGovernor.overEnterCount;
        }
    }
    else if(ratio < vramGovernorLowWater())
    {
        g_vramGovernor.over = false;
    }
    else
    {
        // 2026-11-06d：驱逐已饱和 ⇒ 退出 over。
        //
        // over 的滞回带（0.80 进 / 0.62 出）假设"驱逐能降 used"，但驱逐只释放
        // BLAS（asBlasMB 约 300MB / usedMB 4600），永远降不到 0.62 ⇒ over 常驻。
        // 常驻 over 的副作用是 keepCap=0：每次 tick 释放全部空 slab、下次重新分配
        // （实测 slabAlloc 49.7GB / slabFree 48.4GB、vkAllocN 1188），制造缺页。
        // 此时驱逐已被 nonResident guard 拦住，继续 over 只有坏处。
        static const size_t sNonResidentOverExit = []()
        {
            const char* raw = std::getenv("ORION_NONRESIDENT_GUARD");

            if(raw == nullptr || raw[0] == '\0')
            {
                return static_cast<size_t>(3000u);
            }

            const long parsed = std::atol(raw);

            return parsed >= 0
                ? static_cast<size_t>(parsed)
                : static_cast<size_t>(3000u);
        }();

        if(sNonResidentOverExit > 0u &&
           g_nonResidentSections.size() >= sNonResidentOverExit)
        {
            g_vramGovernor.over = false;
        }
    }

    g_vramGovernor.freedTotalMb += trimSectionMemoryPool();

    // 2026-11-23 ⑨：开关打开时只在 over 的进入沿裁一次，且不裁主机内存的顶点 staging。
    static bool s_governorWasOver = false;
    const bool governorOverEdge =
        g_vramGovernor.over && !s_governorWasOver;
    s_governorWasOver = g_vramGovernor.over;

    if(g_vramGovernor.over &&
       (!CPUPERF_GOV_TRIM_ON_ENTER_ONLY || governorOverEdge))
    {
        const VkDeviceSize stagingFreed =
            (CPUPERF_GOV_TRIM_ON_ENTER_ONLY
                ? static_cast<VkDeviceSize>(0u)
                : g_vertexStagingPool.trimIdleKeepingLargest()) +
            g_blasScratchPool.trimIdleKeepingLargest();

        g_vramGovernor.freedTotalMb +=
            static_cast<double>(stagingFreed) / 1048576.0;

        g_vramGovernor.freedTotalMb +=
            releaseHugeTextureUploadStaging();
    }
}
void logVramGovernor()
{
    if(!g_vramGovernor.enabled)
    {
        return;
    }

    const double ratio =
        g_vramGovernor.budgetMb > 0.0
            ? g_vramGovernor.usedMb / g_vramGovernor.budgetMb
            : 0.0;

    std::stringstream log;
    log
        << std::fixed
        << "[VRAM GOV] available=" << (g_vramGovernor.available ? 1 : 0)
        << " mode=" << vramGovernorMode()
        << " over=" << (g_vramGovernor.over ? 1 : 0)
        << std::setprecision(1)
        << " usedMB=" << g_vramGovernor.usedMb
        << " budgetMB=" << g_vramGovernor.budgetMb
        << std::setprecision(3)
        << " ratio=" << ratio
        << " samples=" << g_vramGovernor.sampleCount
        << " overEnters=" << g_vramGovernor.overEnterCount
        << std::setprecision(1)
        << " freedMB=" << g_vramGovernor.freedTotalMb
        << " slabKeptMB="
        << static_cast<double>(g_sectionMemoryPoolKeptBytes) / 1048576.0
        << " keepCapMB="
        << static_cast<double>(sectionMemoryPoolEffectiveKeepBytes()) / 1048576.0
        << " vliLimitPages=" << vliPageCapacityLimit()
        << " vliEvicted=" << g_vliGovernorEvictedPages
        << " oomSkips=" << g_sectionMemoryOomSkipCount
        << " vliIgnored=" << g_vliGovernorIgnoredSections
        << " nonResident=" << g_nonResidentSections.size()
        << " secEvicted=" << g_vramEvictedSections
        << " secRehydrated=" << g_vramRehydratedSections;

    RTLog(log.str());
}
bool vramProbeEnabled()
{
    static const bool enabled = []()
    {
        const char* raw = std::getenv("ORION_VRAM_PROBE");

        if(raw == nullptr || raw[0] == '\0')
        {
            return true;
        }

        return raw[0] != '0';
    }();

    return enabled;
}
int vramProbeWarnPercent()
{
    static const int cached = []()
    {
        const char* raw = std::getenv("ORION_VRAM_WARN");

        if(raw == nullptr || raw[0] == '\0')
        {
            return 90;
        }

        const int parsed = std::atoi(raw);

        return (parsed >= 50 && parsed <= 100)
            ? parsed
            : 90;
    }();

    return cached;
}
void tickVramProbe()
{
    if(!vramProbeEnabled())
    {
        return;
    }

    ++g_vramProbe.frames;

    const bool needSample =
        g_vramProbe.samples == 0u ||
        (g_vramProbe.frames % VRAM_PROBE_SAMPLE_FRAMES) == 0u;

    if(needSample)
    {
        VramBudgetSample sample;
        g_vramProbe.available = queryVramBudget(sample);

        if(g_vramProbe.available)
        {
            g_vramProbe.usedMb = sample.usedMb;
            g_vramProbe.budgetMb = sample.budgetMb;
            g_vramProbe.ratio =
                sample.budgetMb > 0.0
                    ? sample.usedMb / sample.budgetMb
                    : 0.0;
            ++g_vramProbe.samples;

            if(g_vramProbe.usedMb > g_vramProbe.peakUsedMb)
            {
                g_vramProbe.peakUsedMb = g_vramProbe.usedMb;
            }

            if(g_vramProbe.ratio > g_vramProbe.peakRatio)
            {
                g_vramProbe.peakRatio = g_vramProbe.ratio;
            }

            const double freeMb =
                g_vramProbe.budgetMb - g_vramProbe.usedMb;

            if(g_vramProbe.minFreeMb < 0.0 ||
               freeMb < g_vramProbe.minFreeMb)
            {
                g_vramProbe.minFreeMb = freeMb;
            }

            const double warnRatio =
                static_cast<double>(vramProbeWarnPercent()) / 100.0;

            if(g_vramProbe.ratio >= warnRatio)
            {
                if(!g_vramProbe.warned)
                {
                    g_vramProbe.warned = true;
                    ++g_vramProbe.warnEnters;

                    std::ostringstream warn;

                    warn
                        << std::fixed << std::setprecision(1)
                        << "[VRAM WARN] usedMB=" << g_vramProbe.usedMb
                        << " budgetMB=" << g_vramProbe.budgetMb
                        << " freeMB=" << freeMb
                        << std::setprecision(3)
                        << " ratio=" << g_vramProbe.ratio
                        << std::setprecision(1)
                        << " peakMB=" << g_vramProbe.peakUsedMb
                        << " frame=" << g_perfL10FrameSerial
                        << " oomSkips=" << g_sectionMemoryOomSkipCount
                        << " nonResident=" << g_nonResidentSections.size()
                        << " secEvicted=" << g_vramEvictedSections
                        << " vliEvicted=" << g_vliGovernorEvictedPages
                        << "  (= 越过 "
                        << vramProbeWarnPercent()
                        << "% 警戒线；freeMB 即距 OOM 的余量)";

                    RTLogFlushed(warn.str());
                }
            }
            else if(g_vramProbe.ratio < warnRatio - 0.02)
            {
                g_vramProbe.warned = false;
            }
        }
    }

    if((g_vramProbe.frames % VRAM_PROBE_LOG_FRAMES) == 0u)
    {
        std::ostringstream log;

        log
            << std::fixed << std::setprecision(1)
            << "[VRAM PROBE]"
            << " avail=" << (g_vramProbe.available ? 1 : 0)
            << " frames=" << g_vramProbe.frames
            << " samples=" << g_vramProbe.samples
            << " usedMB=" << g_vramProbe.usedMb
            << " budgetMB=" << g_vramProbe.budgetMb
            << " freeMB="
            << (g_vramProbe.budgetMb > 0.0
                    ? g_vramProbe.budgetMb - g_vramProbe.usedMb
                    : -1.0)
            << std::setprecision(3)
            << " ratio=" << g_vramProbe.ratio
            << std::setprecision(1)
            << " peakMB=" << g_vramProbe.peakUsedMb
            << std::setprecision(3)
            << " peakRatio=" << g_vramProbe.peakRatio
            << std::setprecision(1)
            << " minFreeMB=" << g_vramProbe.minFreeMb
            << " warnEnters=" << g_vramProbe.warnEnters
            << " oomSkips=" << g_sectionMemoryOomSkipCount
            << " nonResident=" << g_nonResidentSections.size()
            << " secEvicted=" << g_vramEvictedSections
            << " vliEvicted=" << g_vliGovernorEvictedPages
            << "  (= 每 30 帧采样一次；peakMB/minFreeMB 是全程极值；"
            << "oomSkips 一旦 >0 说明已经撞过 OUT_OF_DEVICE_MEMORY)";

        RTLog(log.str());
    }
}


// =====================================================
// 2026-10-13：recordFrameCommand 早退诊断（临时仪器）
//
// 现场：进世界正常跑一段后，**加载新区块那一刻**画面变回 Minecraft 原版，并且不再
// 自己恢复；换世界 / 换材质包能恢复。日志的分裂是决定性的 ——
//   · [PERF L10 GPU] / [PERF L10 CPU] / [PERF L10 CPUPHASE] 从该时刻起彻底消失
//     （它们打印在 recordFrameCommand 的最末尾，见 55022-55026）；
//   · 而几何/相机侧的标签（RT GEOMETRY / RT LOCAL LIGHTS / CULL DIAG / TLAS SKIP /
//     RT CAMPROBE）继续正常打到退出。
// ⇒ recordFrameCommand 在该时刻之后**再没走完过一帧**：它在 55022 之前提前 return，
//   于是 RR/DLSS 的输出再也没被 blit 进 MC 的主渲染目标，屏幕就露出原版光栅。
//
// 但本函数里共有 7 处静默早退（50227 / 50433 / 51775 / 52443 / 52460 / 53686 /
// 53719），全都不打日志，所以从日志读不出是哪一条。下面这组仪器给每一处加一条
// "判据变化时打一次"的标签，一次复现即可定案。
//
// 门控：ORION_LOG_FRAMESKIP=1 打开。不设时**一条都不打**，且不改变任何渲染行为
// （只包住原本就有的 return），符合本仓库对诊断开关的既有惯例。
//
// 另外打一条 5 秒一次的 [RT ENTRY] 心跳，用来把两种情形分开：
//   · 心跳还在、却没有 [PERF L10 *] ⇒ 确实是本函数内部早退（看下面的标签）；
//   · 心跳也没了 ⇒ 是 Java 侧不再调用本函数（要往 VulkanCommandEncoderMixin 查）。
// =====================================================
bool frameSkipLogEnabled()
{
    static const bool enabled = []() {
        const char* raw = std::getenv("ORION_LOG_FRAMESKIP");
        return raw != nullptr && raw[0] != '\0' && raw[0] != '0';
    }();
    return enabled;
}
void logFrameSkip(const char* reason, const std::string& detail)
{
    if(!frameSkipLogEnabled())
    {
        return;
    }

    static std::string last;

    const std::string now =
        std::string(reason) + " " + detail;

    if(now == last)
    {
        return;
    }

    last = now;
    RTLogFlushed(std::string("[RT SKIP] ") + now);
}
void maybePrintFrameCounters()
{
    if(!frameSkipLogEnabled())
    {
        return;
    }

    static uint64_t lastMs = 0u;

    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    if(nowMs - lastMs < 5000u)
    {
        return;
    }

    lastMs = nowMs;
    RTLogFlushed(
        std::string("[RT FRAME] enter=") +
            std::to_string(g_rtFrameEnterCount) +
        " done=" +
            std::to_string(g_rtFrameDoneCount)
    );
}
void logFrameEntryHeartbeat()
{
    ++g_rtFrameEnterCount;
    maybePrintFrameCounters();
}

// 只在函数真正的**唯一完整出口**上调用（见 endPerfL10GpuFrame 上面的注释：其它出口
// 都是提前 bail-out）。调不到这里 = 这一帧没走完。
void logFrameDoneHeartbeat()
{
    ++g_rtFrameDoneCount;
    maybePrintFrameCounters();
}

// 从 slab 池取一块 chunk，建一个绑定到它的 VkBuffer。
// preferHostUploadMemory=true 时选"非 DEVICE_LOCAL 的 HOST_VISIBLE"类型（见
// findHostUploadMemoryType）—— OMM 的 data/triangle 是主机写、设备只读一次。
bool createPooledBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags props,
    bool preferHostUploadMemory,
    VkBuffer& buffer,
    OmmBufferBinding& binding)
{
    buffer = VK_NULL_HANDLE;
    binding = {};

    VkBufferCreateInfo bufferInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    };

    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    check(
        vkCreateBuffer(g_device,&bufferInfo,nullptr,&buffer),
        "create pooled OMM buffer failed"
    );

    VkMemoryRequirements requirements{};

    vkGetBufferMemoryRequirements(g_device,buffer,&requirements);

    const uint32_t memoryTypeIndex = preferHostUploadMemory
        ? findHostUploadMemoryType(
            g_physicalDevice,requirements.memoryTypeBits,props)
        : findMemoryType(
            g_physicalDevice,requirements.memoryTypeBits,props);

    VkDeviceSize chunkBase = 0u;
    VkDeviceSize chunkBytes = 0u;

    if(
        !acquireSectionMemory(
            requirements.size,
            requirements.alignment,
            memoryTypeIndex,
            binding.memory,
            chunkBase,
            chunkBytes,
            binding.slab,
            binding.chunk,
            OMM_POOL_CHUNK_WASTE_LOG2
        )
    )
    {
        vkDestroyBuffer(g_device,buffer,nullptr);
        buffer = VK_NULL_HANDLE;
        binding = {};
        return false;
    }

    if(
        vkBindBufferMemory(
            g_device,buffer,binding.memory,chunkBase) != VK_SUCCESS
    )
    {
        releaseSectionMemory(binding.slab,binding.chunk);
        vkDestroyBuffer(g_device,buffer,nullptr);
        buffer = VK_NULL_HANDLE;
        binding = {};
        return false;
    }

    binding.memoryOffset = chunkBase;
    return true;
}

// 归还：销毁 VkBuffer（每次新建的那个）并把这个 chunk 还给池。
// **绝不 vkFreeMemory** —— binding.memory 是共享 slab，还有其他 Section 在用。
void destroyPooledBuffer(
    VkBuffer& buffer,
    OmmBufferBinding& binding)
{
    if(buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(g_device,buffer,nullptr);
        buffer = VK_NULL_HANDLE;
    }

    if(binding.slab != OMM_MEMORY_NO_CHUNK)
    {
        releaseSectionMemory(binding.slab,binding.chunk);
    }

    binding = {};
}
void destroySectionMemoryPool()
{
    const bool deviceAlive =
        g_device != VK_NULL_HANDLE;

    for(SectionMemorySlab& slab : g_sectionMemorySlabs)
    {
        if(deviceAlive && slab.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(
                g_device,
                slab.memory,
                nullptr
            );
        }

        slab = SectionMemorySlab{};
    }

    g_sectionMemorySlabs.clear();
    g_sectionMemoryPoolKeptBytes = 0;
}


// 剔除了提交逻辑，仅完成内存分配与 AS 对象句柄创建
static void destroySectionASImmediate(RTSectionAS& section)
{
    // 2026-11-14：A/B 开关 —— 只记录不真释放（会泄漏，仅供实验）。
    // 目的：如果「永不释放」能消除 5s 信号量超时，就确认崩溃是
    // 「释放了 GPU 仍在使用的 section 显存」，否则彻底排除这条方向。
    static const bool s_noSectionFree = []
    {
        const char* raw = std::getenv("ORION_NO_SECTION_FREE");

        return raw != nullptr && raw[0] == '1';
    }();

    if(s_noSectionFree)
    {
        rtGpuFreedRangeNote(
            section.blasAddress, section.blasBytes, "sectionBLAS(skipped)");

        section = {};
        return;
    }

    auto destroyAS =
        getDestroyAccelerationStructure();


    if(
        section.blas != VK_NULL_HANDLE &&
        destroyAS
    )
    {
        destroyAS(
            g_device,
            section.blas,
            nullptr
        );
    }


    if(section.blasBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(
            g_device,
            section.blasBuffer,
            nullptr
        );
    }


    if(section.blasMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            g_device,
            section.blasMemory,
            nullptr
        );
    }


    // 2026-11-14：释放前记录地址，供卡死看门狗回放（见 OrionInternal.h）。
    rtGpuFreedRangeNote(section.vertexAddress, section.vertexBytes, "sectionVB");
    rtGpuFreedRangeNote(section.blasAddress, section.blasBytes, "sectionBLAS");
    rtGpuFreedRangeNote(
        section.ommDataAddress, 0u, "ommData");
    rtGpuFreedRangeNote(
        section.ommTriangleAddress, 0u, "ommTriangle");
    rtGpuFreedRangeNote(
        section.ommScratchAddress, 0u, "ommScratch");

    if(section.vertexBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(
            g_device,
            section.vertexBuffer,
            nullptr
        );
    }


    if(section.hasSectionMemoryChunk)
    {
        releaseSectionMemory(
            section.sectionMemorySlab,
            section.sectionMemoryChunk
        );

        section.hasSectionMemoryChunk = false;
    }

    if(section.hasBlasMemoryChunk)
    {
        releaseSectionMemory(
            section.blasMemorySlab,
            section.blasMemoryChunk
        );

        section.hasBlasMemoryChunk = false;
    }


    destroySectionOpacityMicromap(section);

    section = {};
}


// =====================================================
// 2026-11-14：Section AS 延迟释放（崩溃修复）。
//
// 症状：进世界 20~30 分钟后随机崩，5s VK semaphore 超时，pass 名为
// "Item in hand"（同样位置已发生 5 次）。含义是 GPU 提交后 5s 未完成 ⇒
// 挂死，最典型的原因就是提交里引用了**已被释放**的 AS / micromap 显存。
//
// 本工程的退休槽系统（retireSectionAS + releaseRetirementSlot）是有围栏
// 证明的，但 destroySectionAS 是即时释放。只要有一条路径在 GPU 未完成时
// 走到即时销毁，就会产生上述挂死。这里把「无法证明 GPU 已完成」的销毁
// 一律挂到延迟表，等退休槽围栏 signal（证明队列已推进）后再真正释放：
//   · flushPendingSectionDestroys(false)：仅在本次 reap 至少释放了一个槽
//     （围栏 signal ⇒ 该提交之前的所有工作都已完成）且条目已过 2 帧时释放；
//   · flushPendingSectionDestroys(true)：队列已知 idle 时无条件释放。
// 表上限 512，超出即强制 vkQueueWaitIdle 后整批释放（有界、不会无限挂）。
// 诊断：每 60 帧打一行 [RT RETIRE]，出现即说明打断了即时释放。
// =====================================================
namespace
{
    struct PendingSectionDestroy
    {
        RTSectionAS section;
        uint64_t frameSerial = 0u;
    };

    std::vector<PendingSectionDestroy> g_pendingSectionDestroy;
    uint64_t g_pendingSectionDestroyDeferred = 0u;
    uint64_t g_pendingSectionDestroyFlushed = 0u;

    bool pendingSectionDestroyHasGpuResources(const RTSectionAS& section)
    {
        return section.blas != VK_NULL_HANDLE ||
               section.blasBuffer != VK_NULL_HANDLE ||
               section.vertexBuffer != VK_NULL_HANDLE ||
               section.opacityMicromap != VK_NULL_HANDLE ||
               section.hasSectionMemoryChunk ||
               section.hasBlasMemoryChunk;
    }

    void flushPendingSectionDestroys(bool force)
    {
        if(g_pendingSectionDestroy.empty())
        {
            return;
        }

        const uint64_t nowSerial = g_perfL10FrameSerial;
        size_t writeIndex = 0u;
        uint64_t flushed = 0u;
        const bool capped = !force && g_pendingSectionDestroy.size() <= 256u;
        const auto flushStart = std::chrono::steady_clock::now();

        for(size_t i = 0u; i < g_pendingSectionDestroy.size(); ++i)
        {
            PendingSectionDestroy& entry = g_pendingSectionDestroy[i];

            // 2 帧余量：条目最早在 2 帧前的提交里被引用，而 force=false 的
            // 调用点在「退休槽围栏已 signal」之后 ⇒ 那次提交已完成。
            // 2026-11-14：2 → 3 帧，与退休槽的 kRetirementMinFrameAge 对齐。
            const bool oldEnough =
                nowSerial >= entry.frameSerial + 3u;
            const bool budgetReached =
                capped &&
                flushed > 0u &&
                (flushed >= 16u ||
                 std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - flushStart
                 ).count() >= 1.5);

            if((force || oldEnough) && !budgetReached)
            {
                destroySectionASImmediate(entry.section);
                ++flushed;
            }
            else if(writeIndex != i)
            {
                g_pendingSectionDestroy[writeIndex] = std::move(entry);
                ++writeIndex;
            }
            else
            {
                ++writeIndex;
            }
        }

        g_pendingSectionDestroy.resize(writeIndex);
        g_pendingSectionDestroyFlushed += flushed;

        if(flushed > 0u && (nowSerial % 60u) == 0u)
        {
            RTLog(
                "[RT RETIRE] deferred=" +
                std::to_string(g_pendingSectionDestroyDeferred) +
                " flushed=" + std::to_string(g_pendingSectionDestroyFlushed) +
                " pending=" + std::to_string(g_pendingSectionDestroy.size()) +
                " force=" + std::string(force ? "1" : "0")
            );
        }
    }
}


void destroySectionAS(RTSectionAS& section)
{
    if(!pendingSectionDestroyHasGpuResources(section))
    {
        destroySectionASImmediate(section);
        return;
    }

    g_pendingSectionDestroy.push_back(
        PendingSectionDestroy{std::move(section), g_perfL10FrameSerial}
    );

    section = {};

    ++g_pendingSectionDestroyDeferred;

    if(g_pendingSectionDestroy.size() > 512u)
    {
        PERF_L10_TIME_STALL("pendingDestroyQueueIdle", check(
            vkQueueWaitIdle(g_queue),
            "wait RT queue before pending Section destroy flush failed"
        ));

        flushPendingSectionDestroys(true);
    }
}


// Moves a replaced Section AS out of the live map and into the retirement
// slot.  The resources stay alive until the rebuild drains the queue; freeing
// them earlier would release memory the GPU may still be traversing.
void retireSectionAS(RTSectionAS& section)
{
    g_sectionRetirement->sections.push_back(
        std::move(section)
    );

    section = {};

    g_sectionRetirement->retireFrameSerial =
        g_perfL10FrameSerial;

    g_sectionRetirement->occupied = true;
}



// Storage buffer + acceleration structure.  Also takes the placeholder
// generation's one-instance build input, which is only ever non-null while that
// placeholder is still the live generation.
void parkTlasStorage()
{
    g_sectionRetirement->tlas = g_tlas;
    g_tlas = VK_NULL_HANDLE;

    g_sectionRetirement->tlasBuffer = g_tlasBuffer;
    g_tlasBuffer = VK_NULL_HANDLE;

    g_sectionRetirement->tlasMemory = g_tlasMemory;
    g_tlasMemory = VK_NULL_HANDLE;

    g_sectionRetirement->tlasInstanceBuffer = g_tlasInstanceBuffer;
    g_tlasInstanceBuffer = VK_NULL_HANDLE;

    g_sectionRetirement->tlasInstanceMemory = g_tlasInstanceMemory;
    g_tlasInstanceMemory = VK_NULL_HANDLE;

    g_tlasAsSize = 0;
}


// Build scratch.  Grow-only: a larger buffer than the build needs is fine.
void parkTlasScratch()
{
    g_sectionRetirement->tlasScratchBuffer = g_tlasScratchBuffer;
    g_tlasScratchBuffer = VK_NULL_HANDLE;

    g_sectionRetirement->tlasScratchMemory = g_tlasScratchMemory;
    g_tlasScratchMemory = VK_NULL_HANDLE;

    g_tlasScratchAddress = 0;
    g_tlasScratchCapacity = 0;
}


// Host-visible multi-Section instance table, rewritten by every rebuild.
void parkTlasInstances()
{
    g_sectionRetirement->tlasInstancesBuffer = g_tlasInstancesBuffer;
    g_tlasInstancesBuffer = VK_NULL_HANDLE;

    g_sectionRetirement->tlasInstancesMemory = g_tlasInstancesMemory;
    g_tlasInstancesMemory = VK_NULL_HANDLE;

    g_tlasInstancesAddress = 0;
    g_tlasInstancesCapacity = 0;
}


// Destroys one parked generation.  The caller must already have proved the GPU
// is finished with it: either the slot's fence signaled (the queue is FIFO, so
// that covers every submission made before the build it was attached to) or the
// queue was drained outright.
void releaseRetirementSlot(
    uint32_t slotIndex
)
{
    SectionRetirementSlot& slot =
        g_sectionRetirementRing[slotIndex];

    if(!slot.occupied)
    {
        return;
    }

    auto destroyAS =
        getDestroyAccelerationStructure();


    if(
        slot.tlas != VK_NULL_HANDLE &&
        destroyAS
    )
    {
        destroyAS(
            g_device,
            slot.tlas,
            nullptr
        );
    }

    slot.tlas = VK_NULL_HANDLE;


    const VkBuffer parkedBuffers[] =
    {
        slot.tlasBuffer,
        slot.tlasScratchBuffer,
        slot.tlasInstanceBuffer,
        slot.tlasInstancesBuffer
    };

    const VkDeviceMemory parkedMemory[] =
    {
        slot.tlasMemory,
        slot.tlasScratchMemory,
        slot.tlasInstanceMemory,
        slot.tlasInstancesMemory
    };

    for(VkBuffer buffer : parkedBuffers)
    {
        if(buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(g_device, buffer, nullptr);
        }
    }

    for(VkDeviceMemory memory : parkedMemory)
    {
        if(memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, memory, nullptr);
        }
    }

    slot.tlasBuffer = VK_NULL_HANDLE;
    slot.tlasMemory = VK_NULL_HANDLE;
    slot.tlasScratchBuffer = VK_NULL_HANDLE;
    slot.tlasScratchMemory = VK_NULL_HANDLE;
    slot.tlasInstanceBuffer = VK_NULL_HANDLE;
    slot.tlasInstanceMemory = VK_NULL_HANDLE;
    slot.tlasInstancesBuffer = VK_NULL_HANDLE;
    slot.tlasInstancesMemory = VK_NULL_HANDLE;


    for(RTSectionAS& section : slot.sections)
    {
        if(section.hasVertexAddressSlot && section.vertexAddressSlot != 0u)
        {
            // 2026-11-15：见文件头的槽位竞态说明。回收两次 / 重复入表都留痕。
            const uint32_t freedSlot = section.vertexAddressSlot;

            if(!slotLiveClear(freedSlot))
            {
                static uint32_t sSlotDupLogCount = 0u;

                if((++sSlotDupLogCount % 20u) == 1u)
                {
                    RTLog(
                        "[SLOT DUP] slot=" + std::to_string(freedSlot) +
                        " sectionKey=" + std::to_string(section.sectionKey) +
                        " total=" + std::to_string(sSlotDupLogCount));
                }
            }

            if(!slotFreeListContains(freedSlot))
            {
                g_sectionVertexAddressFreeSlots.push_back(freedSlot);
            }
            else
            {
                static uint32_t sSlotDupFreeLogCount = 0u;

                if((++sSlotDupFreeLogCount % 20u) == 1u)
                {
                    RTLog(
                        "[SLOT DUP] freeList already holds slot=" +
                        std::to_string(freedSlot) +
                        " sectionKey=" + std::to_string(section.sectionKey) +
                        " total=" + std::to_string(sSlotDupFreeLogCount));
                }
            }
        }

        section.hasVertexAddressSlot = false;
        section.vertexAddressSlot = 0u;

        // 2026-11-14：退休槽的围栏已 signal（releaseRetirementSlot 的调用
        // 前提就是 GPU 已完成），这里保持即时销毁。
        destroySectionASImmediate(section);
    }

    slot.sections.clear();


    DeferredSectionBuildResources liveBuild =
        std::move(g_deferredSectionBuild);

    g_deferredSectionBuild =
        std::move(slot.deferredBuild);

    slot.deferredBuild = {};

    releaseDeferredSectionBuildResources();

    g_deferredSectionBuild =
        std::move(liveBuild);

    slot.occupied = false;
    slot.fencePending = false;
}


// Releases every parked generation whose fence has signaled.  Non-blocking, and
// this is what replaces the blocking vkQueueWaitIdle on the rebuild path.
void reapSignaledRetirements()
{
    if(!g_sectionRetirementFencesReady)
    {
        return;
    }

    bool anyRetirementReleased = false;

    for(
        uint32_t i = 0u;
        i < SECTION_RETIREMENT_RING_SIZE;
        ++i
    )
    {
        if(!g_sectionRetirementRing[i].fencePending)
        {
            continue;
        }

        // 2026-11-14：除了围栏 signal，还要求该槽已退休满 3 帧。
        // 依据：[GPU HANG] 快照显示释放与挂死发生在同一帧，说明只靠我们的
        // 围栏不够 —— 引擎自己的 command buffer 还在飞。
        constexpr uint64_t kRetirementMinFrameAge = 8u;

        if(g_perfL10FrameSerial <
           g_sectionRetirementRing[i].retireFrameSerial +
               kRetirementMinFrameAge)
        {
            continue;
        }

        if(
            vkGetFenceStatus(
                g_device,
                g_sectionRetirementFences[i]
            ) == VK_SUCCESS
        )
        {
            releaseRetirementSlot(
                i
            );

            anyRetirementReleased = true;
        }
    }

    // 2026-11-14：有围栏 signal ⇒ 该提交之前的所有 GPU 工作都已完成，可以
    // 安全释放延迟表里的旧段（见 destroySectionAS 处注释）。
    if(anyRetirementReleased)
    {
        flushPendingSectionDestroys(false);
    }
}


// Releases every parked generation unconditionally.  Only valid once the queue
// is known idle, i.e. after vkQueueWaitIdle() or FGDeviceWaitIdle().
void releaseAllRetirementSlots()
{
    for(
        uint32_t i = 0u;
        i < SECTION_RETIREMENT_RING_SIZE;
        ++i
    )
    {
        releaseRetirementSlot(
            i
        );
    }

    // 2026-11-14：调用点的前提是队列已 idle ⇒ 延迟表可整体释放。
    flushPendingSectionDestroys(true);
}


void ensureSectionRetirementFences()
{
    if(g_sectionRetirementFencesReady)
    {
        return;
    }

    VkFenceCreateInfo fenceInfo{
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };

    for(
        uint32_t i = 0u;
        i < SECTION_RETIREMENT_RING_SIZE;
        ++i
    )
    {
        check(
            vkCreateFence(
                g_device,
                &fenceInfo,
                nullptr,
                &g_sectionRetirementFences[i]
            ),
            "create Section retirement fence failed"
        );
    }

    g_sectionRetirementFencesReady = true;
}


void destroySectionRetirementFences()
{
    for(
        uint32_t i = 0u;
        i < SECTION_RETIREMENT_RING_SIZE;
        ++i
    )
    {
        if(g_sectionRetirementFences[i] != VK_NULL_HANDLE)
        {
            vkDestroyFence(
                g_device,
                g_sectionRetirementFences[i],
                nullptr
            );

            g_sectionRetirementFences[i] = VK_NULL_HANDLE;
        }

        g_sectionRetirementRing[i].fencePending = false;
    }

    g_sectionRetirementFencesReady = false;
}


// Picks the slot this rebuild parks into.  A free slot is reused directly; a
// slot whose fence has signaled is released and then reused.  If every slot is
// still in flight the queue is drained once - a bounded, self-healing fallback
// that cannot leak or park without limit.
uint32_t acquireRetirementSlot()
{
    ensureSectionRetirementFences();

    reapSignaledRetirements();


    for(
        uint32_t i = 0u;
        i < SECTION_RETIREMENT_RING_SIZE;
        ++i
    )
    {
        if(!g_sectionRetirementRing[i].occupied)
        {
            return i;
        }
    }


    PERF_L10_TIME_STALL("retirementQueueIdle", check(
        vkQueueWaitIdle(g_queue),
        "wait RT queue before Section retirement fallback failed"
    ));

    releaseAllRetirementSlots();


    return 0u;
}

// 2026-10-21：Section BLAS refit 闸门。见 RTSectionAS::refitCapable。
//
// 2026-10-29：**默认值改为开**。依据是同局（marker 热切换）A/B：
//   asCreateN/asRetireN 2228 → 617（−72%），asCreateMs 282 → 260（−7%），
//   asChunkMB 512.4 → 510.4（无内存代价），frame 19.51 → 19.41，
//   trace 15.55 → 15.27。此前"ALLOW_UPDATE 让树略大"的顾虑被实测否决。
//
// 三档语义：
//   1. ORION_AS_REFIT=1  —— 进程内恒开（启动读一次，忽略 marker）；
//   2. ORION_AS_REFIT=0  —— 进程内恒关（硬回退，优先级最高）；
//   3. 未设             —— 默认开；可用标记文件热切换：
//                          run/as_refit.off 存在 ⇒ 关（每 600 帧探测一次）。
//      旧的 run/as_refit.on 不再需要（默认即开），保留它也只是"开"。
//
// 热切换只影响**之后的**重建：已经在 refitCapable 状态下的 AS 继续可 refit，
// 新段则在闸门打开时才开始带 ALLOW_UPDATE。
bool sectionBLASRefitEnabled()
{
    static const int envMode = []() -> int
    {
        const char* raw = std::getenv("ORION_AS_REFIT");

        if(raw == nullptr || raw[0] == '\0')
        {
            return -1;
        }

        return (raw[0] == '0') ? 0 : 1;
    }();

    if(envMode >= 0)
    {
        return envMode == 1;
    }

    static bool markerState = true;
    static uint64_t lastProbeFrame = 0u;

    if(g_perfL10FrameSerial - lastProbeFrame >= 600u ||
       lastProbeFrame == 0u)
    {
        lastProbeFrame = g_perfL10FrameSerial;

        std::ifstream offMarker("as_refit.off");
        markerState = !offMarker.good();
    }

    return markerState;
}

// 2026-10-29 诊断闸门（ORION_INDEXED_TERRAIN=0 关掉索引化布局）。
//
// 与上面那句"必须同时重编 4 个 SPV"不冲突：着色器侧**两种布局都编在同一份
// SPV 里**，按顶点表项 `.w` 的最低位运行期分支（material_resolve.glsl:108
// `indexedQuads = (sectionAddressEntry.w & 1u) != 0u`）。所以只改这里 + 重编
// DLL 就能做 A/B；只有改"两布局的语义"时才需要碰 SPV。
//
// 用途：用户报"压显存之后方块之间有分界线"。索引化把每 quad 的 6 个展开顶点
// 收成 4 个角点（逐属性相同，见 uploadChunk 的 repackedVertices），代码上不
// 应该产生接缝，但 BLAS 用**共享 uint16 索引缓冲**（非 shader 读取），驱动侧
// 行为只有实测能排除 ⇒ 留一个单开关。
bool sectionIndexedTerrainEnvEnabled()
{
    static const bool enabled = []()
    {
        const char* raw = std::getenv("ORION_INDEXED_TERRAIN");

        const bool on =
            !(raw != nullptr &&
              (raw[0] == '0' || raw[0] == 'f' || raw[0] == 'F'));

        fprintf(
            stderr,
            "[GEO LAYOUT] indexedTerrain=%d (ORION_INDEXED_TERRAIN)\n",
            on ? 1 : 0
        );

        fflush(stderr);

        return on;
    }();

    return enabled;
}

// 懒创建：一次性填好 0,1,2, 0,2,3 图案。host-visible 足够（192KB，只被 BLAS 构建读取）。
VkDeviceAddress sectionQuadIndexAddress()
{
    if(g_sectionQuadIndexAddress != 0u)
    {
        return g_sectionQuadIndexAddress;
    }

    const VkDeviceSize indexCount =
        static_cast<VkDeviceSize>(SECTION_QUAD_INDEX_MAX_QUADS) * 6u;
    const VkDeviceSize indexBytes = indexCount * sizeof(uint16_t);

    VkBufferCreateInfo bufferInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    };
    bufferInfo.size = indexBytes;
    bufferInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    check(
        vkCreateBuffer(g_device, &bufferInfo, nullptr, &g_sectionQuadIndexBuffer),
        "create Section quad index buffer failed"
    );

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(g_device, g_sectionQuadIndexBuffer, &requirements);

    VkMemoryAllocateFlagsInfo flagsInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO
    };
    flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = findMemoryType(
        g_physicalDevice,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    allocateInfo.pNext = &flagsInfo;

    check(
        vkAllocateMemory(g_device, &allocateInfo, nullptr, &g_sectionQuadIndexMemory),
        "allocate Section quad index memory failed"
    );

    check(
        vkBindBufferMemory(g_device, g_sectionQuadIndexBuffer, g_sectionQuadIndexMemory, 0u),
        "bind Section quad index memory failed"
    );

    void* mapped = nullptr;
    check(
        vkMapMemory(g_device, g_sectionQuadIndexMemory, 0u, indexBytes, 0u, &mapped),
        "map Section quad index memory failed"
    );

    uint16_t* indices = static_cast<uint16_t*>(mapped);

    for(uint32_t quad = 0u; quad < SECTION_QUAD_INDEX_MAX_QUADS; ++quad)
    {
        const uint16_t base = static_cast<uint16_t>(quad * 4u);
        uint16_t* out = indices + static_cast<size_t>(quad) * 6u;
        out[0] = static_cast<uint16_t>(base + 0u);
        out[1] = static_cast<uint16_t>(base + 1u);
        out[2] = static_cast<uint16_t>(base + 2u);
        out[3] = static_cast<uint16_t>(base + 0u);
        out[4] = static_cast<uint16_t>(base + 2u);
        out[5] = static_cast<uint16_t>(base + 3u);
    }

    vkUnmapMemory(g_device, g_sectionQuadIndexMemory);

    g_sectionQuadIndexAddress = getBufferDeviceAddress(g_device, g_sectionQuadIndexBuffer);

    RTLog("[SECTION GEOM] shared quad index buffer created: quads="
        + std::to_string(SECTION_QUAD_INDEX_MAX_QUADS)
        + " bytes=" + std::to_string(indexBytes));

    return g_sectionQuadIndexAddress;
}

// =====================================================
// 2026-10-21 地形 32B：哪些 Section 用打包的 32B 顶点。
//
// 只有"近场地形四桶"可以：opaque / ommCutout / alpha / water。
// 排除项各有硬理由（详见 HANDOFF-2026-10-21-VERTEX-32B-TERRAIN.md §2.2）：
//   · LOD/DH 远场（VOXY_LOD_RESERVED_Y）：pad1 是 lodMarker / -(3+材质)，不是
//     雨面 0.25 ⇒ 折进 pad2 的 +64 后会被解成"雨面"；pad2 = -1 的无纹理表哨兵
//     会与折叠撞车（-1+64=63 < 64）；RGB 已乘 shade ⇒ RGBA8 量化有损。
//     ⚠ 不能只靠 class 位判：LOD_AS_CLASS_ALPHA == SECTION_AS_CLASS_OMM、
//       LOD_AS_CLASS_WATER == SECTION_AS_CLASS_ALPHA（14653-14684）。
//   · 实体 / 本地玩家 / 粒子：Java 侧按 48B 写，语义与地形不同（见 54860）。
//   · 索引化 SOLID（4 顶点/quad）：顶点数不是 3 的倍数，且 shader 侧路径未接。
//
// 判据必须放在函数内（而不是让调用点传参）：少接一次线，打包就会静默失效。
// =====================================================
bool sectionUsesPackedVertices(
    uint64_t sectionKey,
    bool indexedGeometry,
    size_t vertexCount)
{
    if(!SECTION_VERTEX_32B_ENABLED)
    {
        return false;
    }

    if((sectionKey & VOXY_LOD_RESERVED_Y) == VOXY_LOD_RESERVED_Y)
    {
        return false;
    }

    const uint64_t geometryClass =
        sectionKey & ~SPATIAL_SECTION_KEY_MASK;

    if(geometryClass != SECTION_AS_CLASS_OPAQUE &&
       geometryClass != SECTION_AS_CLASS_OMM &&
       geometryClass != SECTION_AS_CLASS_OMM_LEAVES &&
       geometryClass != SECTION_AS_CLASS_ALPHA &&
       geometryClass != SECTION_AS_CLASS_WATER)
    {
        return false;
    }

    if(vertexCount == 0u)
    {
        return false;
    }

    return indexedGeometry
        ? (vertexCount % 4u) == 0u
        : (vertexCount % 3u) == 0u;
}

// 2026-10-22 索引化地形判据（唯一实现，前向声明见 14162）。
//
// 硬前提：索引值上界 = 4*quads-1 必须装进 uint16 ⇒ quads ≤ 16384
// （SECTION_QUAD_INDEX_MAX_QUADS）。超过的段退回 6 顶点/quad 展开布局。
// 只有 OPAQUE（SOLID 层）走这条；OMM/cutout 与半透明层不进。
bool sectionUsesIndexedGeometry(
    uint64_t sectionKey,
    size_t vertexCount)
{
    return SECTION_INDEXED_TERRAIN_ENABLED &&
        sectionIndexedTerrainEnvEnabled() &&
        (sectionKey & ~SPATIAL_SECTION_KEY_MASK) == SECTION_AS_CLASS_OPAQUE &&
        vertexCount != 0u &&
        (vertexCount % 4u) == 0u &&
        vertexCount <= static_cast<size_t>(SECTION_QUAD_INDEX_MAX_VERTS);
}
VkDeviceSize createSectionBLAS(
    RTSectionAS& section,
    uint64_t sectionKey,
    std::span<const Vertex> vertices,
    bool opaqueGeometry,
    const OmmCutoutInput* ommInput,
    std::span<const float> vertexMotion ,
    bool allowUpdate )
{
    RTXMarkPhase("geometry:blas");

    // 2026-11-12：内部分段计时（只计时）。单段实测 1.7~3.0ms 全记在 createMs 里，
    // 这里把 buffer 创建 / buildSizes / 显存分配+bind / createAS / deviceAddress
    // 拆开，供 [RT SECTION SLOW] 归因。
    g_sectionBuildBufMs = 0.0;
    g_sectionBuildVkBufMs = 0.0;
    g_sectionBuildPrepMs = 0.0;
    g_sectionBuildOmmMs = 0.0;
    g_sectionBuildReqMs = 0.0;
    g_sectionBuildSizesMs = 0.0;
    g_sectionBuildMemMs = 0.0;
    g_sectionBuildAsMs = 0.0;
    g_sectionBuildAddrMs = 0.0;

    auto sectionBuildPhaseStart = std::chrono::steady_clock::now();

    const auto sectionBuildPhaseLap = [&sectionBuildPhaseStart]() -> double
    {
        const std::chrono::steady_clock::time_point lapNow =
            std::chrono::steady_clock::now();

        const double lapMs = std::chrono::duration<double, std::milli>(
            lapNow - sectionBuildPhaseStart).count();

        sectionBuildPhaseStart = lapNow;

        return lapMs;
    };

    g_rtxDetailSectionKey.store(sectionKey, std::memory_order_relaxed);
    g_rtxDetailSectionVerts.store(
        static_cast<uint64_t>(vertices.size()),
        std::memory_order_relaxed
    );

    const bool indexedGeometry =
        opaqueGeometry &&
        sectionUsesIndexedGeometry(sectionKey, vertices.size());

    if(indexedGeometry)
    {
        if(vertices.size() < 4u)
        {
            throw std::runtime_error(
                "Indexed Section BLAS needs at least one quad"
            );
        }
    }
    else if(
        vertices.size() < 3 ||
        (vertices.size() % 3) != 0
    )
    {
        throw std::runtime_error(
            "Section BLAS vertex count must be a non-zero multiple of 3"
        );
    }


    if(
        vertices.size() >
        static_cast<size_t>(
            std::numeric_limits<uint32_t>::max()
        )
    )
    {
        throw std::runtime_error(
            "Section BLAS vertex count exceeds uint32_t"
        );
    }


    section = {};
    section.sectionKey = sectionKey;
    section.indexedGeometry = indexedGeometry;
    section.opaqueGeometry = opaqueGeometry;

    const uint32_t sectionVertexStride =
        sectionUsesPackedVertices(sectionKey, indexedGeometry, vertices.size())
            ? static_cast<uint32_t>(sizeof(SectionVertex))
            : static_cast<uint32_t>(sizeof(Vertex));

    section.vertexStride = sectionVertexStride;
    section.packedSectionVertices =
        sectionVertexStride == static_cast<uint32_t>(sizeof(SectionVertex));

    section.triangleCount =
        indexedGeometry
            ? static_cast<uint32_t>((vertices.size() / 4u) * 2u)
            : static_cast<uint32_t>(vertices.size() / 3);

    section.vertexBytes =
        static_cast<VkDeviceSize>(
            vertices.size()
        ) *
        static_cast<VkDeviceSize>(sectionVertexStride);

    if(!vertexMotion.empty())
    {
        section.vertexMotionOffset =
            section.vertexBytes;

        section.vertexMotionFloats =
            static_cast<uint32_t>(
                vertices.size() * 3u
            );

        section.vertexBytes +=
            static_cast<VkDeviceSize>(
                section.vertexMotionFloats
            ) *
            sizeof(float);
    }

    if(!opaqueGeometry && ommInput)
    {
        const std::chrono::steady_clock::time_point ommStart =
            std::chrono::steady_clock::now();

        createSectionOpacityMicromap(section, *ommInput);

        g_sectionBuildOmmMs +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - ommStart
            ).count();
    }


    VkBufferCreateInfo vertexBufferInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    };

    vertexBufferInfo.size =
        section.vertexBytes;

    vertexBufferInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    vertexBufferInfo.sharingMode =
        VK_SHARING_MODE_EXCLUSIVE;


    g_sectionBuildPrepMs += sectionBuildPhaseLap();  // 函数开头 → vkCreateBuffer 之前（含 OMM）

    check(
        vkCreateBuffer(
            g_device,
            &vertexBufferInfo,
            nullptr,
            &section.vertexBuffer
        ),
        "create Section vertex buffer failed"
    );

    g_sectionBuildVkBufMs += sectionBuildPhaseLap();


    VkMemoryRequirements vertexRequirements{};

    (void)sectionBuildPhaseLap();  // 丢弃：从 vkGetBufferMemoryRequirements 开始计

    vkGetBufferMemoryRequirements(
        g_device,
        section.vertexBuffer,
        &vertexRequirements
    );

    g_sectionBuildReqMs += sectionBuildPhaseLap();
    g_sectionBuildBufMs =
        g_sectionBuildVkBufMs + g_sectionBuildReqMs;


    VkAccelerationStructureGeometryTrianglesDataKHR triangles{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR
    };

    triangles.vertexFormat =
        VK_FORMAT_R32G32B32_SFLOAT;

    triangles.vertexData.deviceAddress = 0;

    triangles.vertexStride =
        static_cast<VkDeviceSize>(section.vertexStride);

    if(indexedGeometry)
    {
        triangles.maxVertex =
            static_cast<uint32_t>(vertices.size()) - 1u;
        triangles.indexType = VK_INDEX_TYPE_UINT16;
        triangles.indexData.deviceAddress = sectionQuadIndexAddress();
    }
    else
    {
        triangles.maxVertex =
            section.triangleCount * 3u - 1u;
        triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    }


    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR
    };

    geometry.geometryType =
        VK_GEOMETRY_TYPE_TRIANGLES_KHR;

    geometry.flags =
        opaqueGeometry
            ? VK_GEOMETRY_OPAQUE_BIT_KHR
            : 0;

    geometry.geometry.triangles =
        triangles;

    VkAccelerationStructureTrianglesOpacityMicromapEXT ommAttachment{};
    if(section.hasOpacityMicromap)
    {
        ommAttachment.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
        ommAttachment.indexType = VK_INDEX_TYPE_NONE_KHR;
        ommAttachment.baseTriangle = 0u;
        ommAttachment.usageCountsCount = 1u;
        ommAttachment.pUsageCounts = &section.ommUsage;
        ommAttachment.micromap = section.opacityMicromap;
        geometry.geometry.triangles.pNext = &ommAttachment;
    }


    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
    };

    buildInfo.type =
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    const bool refitCapable =
        allowUpdate && !section.hasOpacityMicromap;

    const bool compactCapable =
        !refitCapable &&
        !section.hasOpacityMicromap &&
        getCmdWriteAccelerationStructuresProperties() != nullptr &&
        getCmdCopyAccelerationStructure() != nullptr;

    buildInfo.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        (section.hasOpacityMicromap
            ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_EXT
            : 0) |
        (refitCapable
            ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
            : 0) |
        (compactCapable
            ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR
            : 0);

    buildInfo.mode =
        VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;


    VkAccelerationStructureBuildSizesInfoKHR sizesInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };


    auto getBuildSizes =
        getGetAccelerationStructureBuildSizes();

    auto createAS =
        getCreateAccelerationStructure();

    auto getASAddress =
        getGetAccelerationStructureDeviceAddress();


    if(
        !getBuildSizes ||
        !createAS ||
        !getASAddress
    )
    {
        throw std::runtime_error(
            "Section BLAS Vulkan functions unavailable"
        );
    }


    const uint32_t primitiveCount =
        section.triangleCount;


    getBuildSizes(
        g_device,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo,
        &primitiveCount,
        &sizesInfo
    );

    g_sectionBuildSizesMs += sectionBuildPhaseLap();


    VkBufferCreateInfo blasBufferInfo{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO
    };

    section.blasBytes =
        sizesInfo.accelerationStructureSize;

    blasBufferInfo.size =
        sizesInfo.accelerationStructureSize;

    blasBufferInfo.usage =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    blasBufferInfo.sharingMode =
        VK_SHARING_MODE_EXCLUSIVE;


    check(
        vkCreateBuffer(
            g_device,
            &blasBufferInfo,
            nullptr,
            &section.blasBuffer
        ),
        "create Section BLAS storage buffer failed"
    );


    VkMemoryRequirements blasRequirements{};

    vkGetBufferMemoryRequirements(
        g_device,
        section.blasBuffer,
        &blasRequirements
    );


    const uint32_t commonMemoryTypeBits =
        vertexRequirements.memoryTypeBits &
        blasRequirements.memoryTypeBits;


    if(commonMemoryTypeBits == 0)
    {
        throw std::runtime_error(
            "Section vertex and BLAS buffers have no common memory type"
        );
    }


    const VkDeviceSize blasAlignment =
        std::max<VkDeviceSize>(
            blasRequirements.alignment,
            1
        );

    const bool useSeparateBlasChunk =
        compactCapable &&
        sizesInfo.accelerationStructureSize >= 16384u;

    if(useSeparateBlasChunk)
    {
        const uint32_t vertexMemoryTypeIndex =
            findMemoryType(
                g_physicalDevice,
                vertexRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            );
        const uint32_t blasMemoryTypeIndex =
            findMemoryType(
                g_physicalDevice,
                blasRequirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            );

        VkDeviceSize vertexChunkBase = 0u;
        if(!acquireSectionMemory(
                vertexRequirements.size,
                vertexRequirements.alignment,
                vertexMemoryTypeIndex,
                section.vertexMemory,
                vertexChunkBase,
                section.sectionMemoryChunkBytes,
                section.sectionMemorySlab,
                section.sectionMemoryChunk,
                sectionPoolChunkWasteLog2()))
        {
            throw SectionMemoryUnavailableError(
                "acquire Section vertex memory chunk failed"
            );
        }

        VkDeviceMemory blasChunkMemory = VK_NULL_HANDLE;
        VkDeviceSize blasChunkBase = 0u;
        if(!acquireSectionMemory(
                blasRequirements.size,
                blasAlignment,
                blasMemoryTypeIndex,
                blasChunkMemory,
                blasChunkBase,
                section.blasMemoryChunkBytes,
                section.blasMemorySlab,
                section.blasMemoryChunk,
                sectionPoolChunkWasteLog2()))
        {
            releaseSectionMemory(
                section.sectionMemorySlab,
                section.sectionMemoryChunk
            );
            throw SectionMemoryUnavailableError(
                "acquire Section BLAS memory chunk failed"
            );
        }

        section.hasSectionMemoryChunk = true;
        section.hasBlasMemoryChunk = true;
        section.compactCapable = true;
        section.blasCompacted = false;

        check(
            vkBindBufferMemory(
                g_device,
                section.vertexBuffer,
                section.vertexMemory,
                vertexChunkBase
            ),
            "bind Section vertex buffer memory failed"
        );

        check(
            vkBindBufferMemory(
                g_device,
                section.blasBuffer,
                blasChunkMemory,
                blasChunkBase
            ),
            "bind Section BLAS buffer memory failed"
        );
    }
    else
    {
        if(
            vertexRequirements.size >
            std::numeric_limits<VkDeviceSize>::max() -
                (blasAlignment - 1)
        )
        {
            throw std::runtime_error(
                "Section shared allocation alignment overflow"
            );
        }


        const VkDeviceSize blasMemoryOffset =
            (
                (
                    vertexRequirements.size +
                    blasAlignment -
                    1
                ) /
                blasAlignment
            ) *
            blasAlignment;


        if(
            blasMemoryOffset >
            std::numeric_limits<VkDeviceSize>::max() -
                blasRequirements.size
        )
        {
            throw std::runtime_error(
                "Section shared allocation size overflow"
            );
        }


        const VkDeviceSize sharedMemoryBytes =
            blasMemoryOffset +
            blasRequirements.size;

        const uint32_t sharedMemoryTypeIndex =
            findMemoryType(
                g_physicalDevice,
                commonMemoryTypeBits,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
            );

        VkDeviceSize chunkBase = 0u;

        if(
            !acquireSectionMemory(
                sharedMemoryBytes,
                std::max(
                    vertexRequirements.alignment,
                    blasRequirements.alignment
                ),
                sharedMemoryTypeIndex,
                section.vertexMemory,
                chunkBase,
                section.sectionMemoryChunkBytes,
                section.sectionMemorySlab,
                section.sectionMemoryChunk,
                sectionPoolChunkWasteLog2()
            )
        )
        {
            throw SectionMemoryUnavailableError(
                "acquire Section memory chunk failed"
            );
        }

        section.hasSectionMemoryChunk = true;
        section.hasBlasMemoryChunk = false;
        section.compactCapable = false;
        section.blasCompacted = false;


        check(
            vkBindBufferMemory(
                g_device,
                section.vertexBuffer,
                section.vertexMemory,
                chunkBase
            ),
            "bind Section vertex buffer memory failed"
        );


        check(
            vkBindBufferMemory(
                g_device,
                section.blasBuffer,
                section.vertexMemory,
                chunkBase + blasMemoryOffset
            ),
            "bind Section BLAS buffer memory failed"
        );
    }

    g_sectionBuildMemMs += sectionBuildPhaseLap();


    section.blasMemory =
        VK_NULL_HANDLE;


    section.vertexAddress =
        getBufferDeviceAddress(
            g_device,
            section.vertexBuffer
        );


    VkAccelerationStructureCreateInfoKHR createInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR
    };

    createInfo.buffer =
        section.blasBuffer;

    createInfo.offset = 0;

    createInfo.size =
        sizesInfo.accelerationStructureSize;

    createInfo.type =
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;


    check(
        createAS(
            g_device,
            &createInfo,
            nullptr,
            &section.blas
        ),
        "create Section BLAS failed"
    );

    g_sectionBuildAsMs += sectionBuildPhaseLap();


    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
    };

    addressInfo.accelerationStructure =
        section.blas;


    section.blasAddress =
        getASAddress(
            g_device,
            &addressInfo
        );

    g_sectionBuildAddrMs += sectionBuildPhaseLap();

    section.buildScratchSize =
        sizesInfo.buildScratchSize;

    section.refitCapable = refitCapable;
    section.updateScratchSize =
        refitCapable ? sizesInfo.updateScratchSize : 0u;

    return std::max(
        section.buildScratchSize,
        section.updateScratchSize
    );
}
void recordSectionBLASBuild(
    VkCommandBuffer cmd,
    RTSectionAS& section,
    VkDeviceAddress scratchAddress
)
{
    const bool updateMode = section.pendingRefit;
    section.pendingRefit = false;

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR
    };

    triangles.vertexFormat =
        VK_FORMAT_R32G32B32_SFLOAT;

    triangles.vertexData.deviceAddress =
        section.vertexAddress;

    triangles.vertexStride =
        static_cast<VkDeviceSize>(section.vertexStride);

    if(section.indexedGeometry)
    {
        triangles.maxVertex =
            static_cast<uint32_t>(
                section.vertexBytes / section.vertexStride) - 1u;
        triangles.indexType = VK_INDEX_TYPE_UINT16;
        triangles.indexData.deviceAddress = sectionQuadIndexAddress();
    }
    else
    {
        triangles.maxVertex =
            section.triangleCount * 3u - 1u;
        triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    }


    VkAccelerationStructureGeometryKHR geometry{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR
    };

    geometry.geometryType =
        VK_GEOMETRY_TYPE_TRIANGLES_KHR;

    // 2026-11-22 (Batch 2): Preserve VK_GEOMETRY_OPAQUE_BIT_KHR on actual build
    // (prepareSectionBLAS already requests it, previously zeroed here).
    // Rollback: set kSECTION_BLAS_OPAQUE_FLAG_FIX = false.
    static constexpr bool kSECTION_BLAS_OPAQUE_FLAG_FIX = true;
    geometry.flags =
        (kSECTION_BLAS_OPAQUE_FLAG_FIX && section.opaqueGeometry)
            ? VK_GEOMETRY_OPAQUE_BIT_KHR
            : 0;

    geometry.geometry.triangles =
        triangles;

    VkAccelerationStructureTrianglesOpacityMicromapEXT ommAttachment{};
    if(section.hasOpacityMicromap)
    {
        ommAttachment.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
        ommAttachment.indexType = VK_INDEX_TYPE_NONE_KHR;
        ommAttachment.baseTriangle = 0u;
        ommAttachment.usageCountsCount = 1u;
        ommAttachment.pUsageCounts = &section.ommUsage;
        ommAttachment.micromap = section.opacityMicromap;
        geometry.geometry.triangles.pNext = &ommAttachment;
    }


    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
    };

    buildInfo.type =
        VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

    buildInfo.flags =
        VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        (section.hasOpacityMicromap
            ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_EXT
            : 0) |
        (section.refitCapable
            ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
            : 0) |
        (section.compactCapable
            ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR
            : 0);

    buildInfo.mode =
        updateMode
            ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
            : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

    buildInfo.srcAccelerationStructure =
        updateMode
            ? section.blas
            : VK_NULL_HANDLE;

    buildInfo.dstAccelerationStructure =
        section.blas;

    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    buildInfo.scratchData.deviceAddress =
        scratchAddress;


    VkAccelerationStructureBuildRangeInfoKHR range{};

    range.primitiveCount =
        section.triangleCount;


    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] =
    {
        &range
    };


    auto cmdBuild =
        getCmdBuildAccelerationStructures();


    if(!cmdBuild)
    {
        throw std::runtime_error(
            "vkCmdBuildAccelerationStructuresKHR unavailable"
        );
    }

    if(section.hasOpacityMicromap)
    {
        auto cmdBuildMicromaps = getCmdBuildMicromaps();
        auto cmdBarrier2 = getCmdPipelineBarrier2Omm();
        if(!cmdBuildMicromaps || !cmdBarrier2)
            throw std::runtime_error("Opacity micromap build functions unavailable");

        VkMicromapBuildInfoEXT micromapBuild{};
        micromapBuild.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
        micromapBuild.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        micromapBuild.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
        micromapBuild.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
        micromapBuild.dstMicromap = section.opacityMicromap;
        micromapBuild.usageCountsCount = 1u;
        micromapBuild.pUsageCounts = &section.ommUsage;
        micromapBuild.data.deviceAddress = section.ommDataAddress;
        micromapBuild.scratchData.deviceAddress = section.ommScratchAddress;
        micromapBuild.triangleArray.deviceAddress = section.ommTriangleAddress;
        micromapBuild.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
        cmdBuildMicromaps(cmd, 1u, &micromapBuild);

        OmmMemoryBarrier2 micromapBarrier{};
        micromapBarrier.sType = OMM_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        micromapBarrier.srcStageMask = OMM_STAGE_MICROMAP_BUILD;
        micromapBarrier.srcAccessMask = OMM_ACCESS_MICROMAP_WRITE;
        micromapBarrier.dstStageMask = OMM_STAGE_AS_BUILD;
        micromapBarrier.dstAccessMask = OMM_ACCESS_AS_READ;
        OmmDependencyInfo dependency{};
        dependency.sType = OMM_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1u;
        dependency.pMemoryBarriers = &micromapBarrier;
        cmdBarrier2(cmd, &dependency);
    }


    cmdBuild(
        cmd,
        1,
        &buildInfo,
        ranges
    );
}
void recordSectionOmmBuildBatch(
    VkCommandBuffer cmd,
    std::span<RTSectionAS* const> sections
)
{
    if(cmd == VK_NULL_HANDLE || sections.empty())
    {
        return;
    }

    static std::vector<VkMicromapBuildInfoEXT> s_ommBuildInfos;
    s_ommBuildInfos.clear();

    for(RTSectionAS* section : sections)
    {
        if(section == nullptr ||
           !section->hasOpacityMicromap ||
           section->opacityMicromap == VK_NULL_HANDLE)
        {
            continue;
        }

        VkMicromapBuildInfoEXT micromapBuild{};
        micromapBuild.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
        micromapBuild.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        micromapBuild.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
        micromapBuild.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
        micromapBuild.dstMicromap = section->opacityMicromap;
        micromapBuild.usageCountsCount = 1u;
        micromapBuild.pUsageCounts = &section->ommUsage;
        micromapBuild.data.deviceAddress = section->ommDataAddress;
        micromapBuild.scratchData.deviceAddress = section->ommScratchAddress;
        micromapBuild.triangleArray.deviceAddress = section->ommTriangleAddress;
        micromapBuild.triangleArrayStride = sizeof(VkMicromapTriangleEXT);
        s_ommBuildInfos.push_back(micromapBuild);
    }

    if(s_ommBuildInfos.empty())
    {
        return;
    }

    auto cmdBuildMicromaps = getCmdBuildMicromaps();
    auto cmdBarrier2 = getCmdPipelineBarrier2Omm();
    if(!cmdBuildMicromaps || !cmdBarrier2)
    {
        throw std::runtime_error("Opacity micromap build functions unavailable");
    }

    cmdBuildMicromaps(
        cmd,
        static_cast<uint32_t>(s_ommBuildInfos.size()),
        s_ommBuildInfos.data()
    );

    OmmMemoryBarrier2 micromapBarrier{};
    micromapBarrier.sType = OMM_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    micromapBarrier.srcStageMask = OMM_STAGE_MICROMAP_BUILD;
    micromapBarrier.srcAccessMask = OMM_ACCESS_MICROMAP_WRITE;
    micromapBarrier.dstStageMask = OMM_STAGE_AS_BUILD;
    micromapBarrier.dstAccessMask = OMM_ACCESS_AS_READ;
    OmmDependencyInfo dependency{};
    dependency.sType = OMM_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1u;
    dependency.pMemoryBarriers = &micromapBarrier;
    cmdBarrier2(cmd, &dependency);
}
void recordSectionBLASBuildWave(
    VkCommandBuffer cmd,
    std::span<RTSectionAS* const> waveSections,
    std::span<const VkDeviceSize> waveScratchOffsets,
    VkDeviceAddress scratchBaseAddress
)
{
    if(cmd == VK_NULL_HANDLE || waveSections.empty())
    {
        return;
    }

    if(waveSections.size() != waveScratchOffsets.size())
    {
        throw std::runtime_error("BLAS wave scratch offset count mismatch");
    }

    auto cmdBuild = getCmdBuildAccelerationStructures();
    if(!cmdBuild)
    {
        throw std::runtime_error("vkCmdBuildAccelerationStructuresKHR unavailable");
    }

    const size_t count = waveSections.size();
    std::vector<VkAccelerationStructureGeometryKHR> geometries(count);
    std::vector<VkAccelerationStructureTrianglesOpacityMicromapEXT> ommAttachments(count);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> buildInfos(count);
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges(count);
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePtrs(count);

    static constexpr bool kSECTION_BLAS_OPAQUE_FLAG_FIX = true;

    for(size_t i = 0u; i < count; ++i)
    {
        RTSectionAS& section = *waveSections[i];
        const bool updateMode = section.pendingRefit;
        section.pendingRefit = false;

        VkAccelerationStructureGeometryTrianglesDataKHR triangles{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR
        };
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = section.vertexAddress;
        triangles.vertexStride = static_cast<VkDeviceSize>(section.vertexStride);

        if(section.indexedGeometry)
        {
            triangles.maxVertex =
                static_cast<uint32_t>(
                    section.vertexBytes / section.vertexStride) - 1u;
            triangles.indexType = VK_INDEX_TYPE_UINT16;
            triangles.indexData.deviceAddress = sectionQuadIndexAddress();
        }
        else
        {
            triangles.maxVertex = section.triangleCount * 3u - 1u;
            triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
        }

        if(section.hasOpacityMicromap)
        {
            ommAttachments[i].sType =
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT;
            ommAttachments[i].indexType = VK_INDEX_TYPE_NONE_KHR;
            ommAttachments[i].baseTriangle = 0u;
            ommAttachments[i].usageCountsCount = 1u;
            ommAttachments[i].pUsageCounts = &section.ommUsage;
            ommAttachments[i].micromap = section.opacityMicromap;
            triangles.pNext = &ommAttachments[i];
        }

        geometries[i] = VkAccelerationStructureGeometryKHR{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR
        };
        geometries[i].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometries[i].flags =
            (kSECTION_BLAS_OPAQUE_FLAG_FIX && section.opaqueGeometry)
                ? VK_GEOMETRY_OPAQUE_BIT_KHR
                : 0;
        geometries[i].geometry.triangles = triangles;

        buildInfos[i] = VkAccelerationStructureBuildGeometryInfoKHR{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
        };
        buildInfos[i].type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfos[i].flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
            (section.hasOpacityMicromap
                ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_DISABLE_OPACITY_MICROMAPS_EXT
                : 0) |
            (section.refitCapable
                ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                : 0) |
            (section.compactCapable
                ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR
                : 0);
        buildInfos[i].mode =
            updateMode
                ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfos[i].srcAccelerationStructure =
            updateMode
                ? section.blas
                : VK_NULL_HANDLE;
        buildInfos[i].dstAccelerationStructure = section.blas;
        buildInfos[i].geometryCount = 1u;
        buildInfos[i].pGeometries = &geometries[i];
        buildInfos[i].scratchData.deviceAddress =
            scratchBaseAddress + waveScratchOffsets[i];

        ranges[i] = VkAccelerationStructureBuildRangeInfoKHR{};
        ranges[i].primitiveCount = section.triangleCount;
        rangePtrs[i] = &ranges[i];
    }

    cmdBuild(
        cmd,
        static_cast<uint32_t>(count),
        buildInfos.data(),
        rangePtrs.data()
    );
}


namespace
{
    uint32_t s_sectionVertexAddressDirtyMin = 0xFFFFFFFFu;
    uint32_t s_sectionVertexAddressDirtyMax = 0u;
    uint32_t s_sectionRoleDirtyMin = 0xFFFFFFFFu;
    uint32_t s_sectionRoleDirtyMax = 0u;
    bool s_lodCoverageDirty = false;
}

void markLodCoverageDirty()
{
    s_lodCoverageDirty = true;
}

void destroySectionVertexAddressBuffer()
{
    if(
        g_sectionVertexAddressMapped != nullptr &&
        g_sectionVertexAddressStagingMemory != VK_NULL_HANDLE
    )
    {
        vkUnmapMemory(
            g_device,
            g_sectionVertexAddressStagingMemory
        );

        g_sectionVertexAddressMapped =
            nullptr;
    }


    if(g_sectionVertexAddressStagingBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(
            g_device,
            g_sectionVertexAddressStagingBuffer,
            nullptr
        );

        g_sectionVertexAddressStagingBuffer =
            VK_NULL_HANDLE;
    }


    if(g_sectionVertexAddressStagingMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            g_device,
            g_sectionVertexAddressStagingMemory,
            nullptr
        );

        g_sectionVertexAddressStagingMemory =
            VK_NULL_HANDLE;
    }


    if(g_sectionVertexAddressBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(
            g_device,
            g_sectionVertexAddressBuffer,
            nullptr
        );

        g_sectionVertexAddressBuffer =
            VK_NULL_HANDLE;
    }


    if(g_sectionVertexAddressMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            g_device,
            g_sectionVertexAddressMemory,
            nullptr
        );

        g_sectionVertexAddressMemory =
            VK_NULL_HANDLE;
    }


    g_sectionVertexAddressCapacity = 0;

    g_sectionVertexAddressNextSlot = 1u;
    g_sectionVertexAddressFreeSlots.clear();
    // 2026-11-15：槽位表重建 ⇒ 存活位图必须一起清空（见文件头槽位竞态说明）。
    g_sectionSlotLiveBits.clear();

    if(g_sectionRoleMapped != nullptr && g_sectionRoleStagingMemory != VK_NULL_HANDLE)
    {
        vkUnmapMemory(
            g_device,
            g_sectionRoleStagingMemory
        );

        g_sectionRoleMapped = nullptr;
    }


    if(g_sectionRoleStagingBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(
            g_device,
            g_sectionRoleStagingBuffer,
            nullptr
        );

        g_sectionRoleStagingBuffer = VK_NULL_HANDLE;
    }


    if(g_sectionRoleStagingMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            g_device,
            g_sectionRoleStagingMemory,
            nullptr
        );

        g_sectionRoleStagingMemory = VK_NULL_HANDLE;
    }


    if(g_sectionRoleBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(
            g_device,
            g_sectionRoleBuffer,
            nullptr
        );

        g_sectionRoleBuffer = VK_NULL_HANDLE;
    }


    if(g_sectionRoleMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            g_device,
            g_sectionRoleMemory,
            nullptr
        );

        g_sectionRoleMemory = VK_NULL_HANDLE;
    }


    if(g_lodCoverageMapped != nullptr && g_lodCoverageStagingMemory != VK_NULL_HANDLE)
    {
        vkUnmapMemory(
            g_device,
            g_lodCoverageStagingMemory
        );

        g_lodCoverageMapped = nullptr;
    }


    if(g_lodCoverageStagingBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(
            g_device,
            g_lodCoverageStagingBuffer,
            nullptr
        );

        g_lodCoverageStagingBuffer = VK_NULL_HANDLE;
    }


    if(g_lodCoverageStagingMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            g_device,
            g_lodCoverageStagingMemory,
            nullptr
        );

        g_lodCoverageStagingMemory = VK_NULL_HANDLE;
    }


    if(g_lodCoverageBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(
            g_device,
            g_lodCoverageBuffer,
            nullptr
        );

        g_lodCoverageBuffer = VK_NULL_HANDLE;
    }


    if(g_lodCoverageMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            g_device,
            g_lodCoverageMemory,
            nullptr
        );

        g_lodCoverageMemory = VK_NULL_HANDLE;
    }


    g_lodCoverageWordCount = 0;

    s_sectionVertexAddressDirtyMin = 0xFFFFFFFFu;
    s_sectionVertexAddressDirtyMax = 0u;
    s_sectionRoleDirtyMin = 0xFFFFFFFFu;
    s_sectionRoleDirtyMax = 0u;
    s_lodCoverageDirty = false;
}


void updateSectionVertexAddressDescriptor()
{
    if(
        g_rayTracingDescriptorSet == VK_NULL_HANDLE ||
        g_sectionVertexAddressBuffer == VK_NULL_HANDLE ||
        g_sectionVertexAddressCapacity == 0
    )
    {
        return;
    }


    VkDescriptorBufferInfo bufferInfo{};

    bufferInfo.buffer =
        g_sectionVertexAddressBuffer;

    bufferInfo.offset = 0;

    bufferInfo.range =
        static_cast<VkDeviceSize>(
            g_sectionVertexAddressCapacity
        ) *
        sizeof(GPUSectionVertexAddress);


    VkWriteDescriptorSet write{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
    };

    write.dstSet =
        g_rayTracingDescriptorSet;

    write.dstBinding = 21;
    write.descriptorCount = 1;

    write.descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    write.pBufferInfo =
        &bufferInfo;


    VkDescriptorBufferInfo roleInfo{};

    roleInfo.buffer = g_sectionRoleBuffer;
    roleInfo.offset = 0;
    roleInfo.range =
        static_cast<VkDeviceSize>(
            g_sectionVertexAddressCapacity
        ) *
        sizeof(uint32_t);


    VkDescriptorBufferInfo coverageInfo{};

    coverageInfo.buffer = g_lodCoverageBuffer;
    coverageInfo.offset = 0;
    coverageInfo.range =
        static_cast<VkDeviceSize>(sizeof(LodCoverageHeader)) +
        static_cast<VkDeviceSize>(g_lodCoverageWordCount) *
            sizeof(uint32_t);


    VkWriteDescriptorSet writes[3]{};

    for(int i = 0; i < 3; ++i)
    {
        writes[i].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

        writes[i].dstSet =
            g_rayTracingDescriptorSet;

        writes[i].descriptorCount = 1;

        writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }

    writes[0] = write;

    writes[1].dstBinding = 19;
    writes[1].pBufferInfo = &roleInfo;

    writes[2].dstBinding = 20;
    writes[2].pBufferInfo = &coverageInfo;


    vkUpdateDescriptorSets(
        g_device,
        3,
        writes,
        0,
        nullptr
    );
}


// Allocates the Section vertex address table once, at its full slot count, and
// keeps it mapped for the rest of the session.  Slot 0 is the placeholder that
// the initialization TLAS reads before any Section exists.
void createSectionVertexAddressTable()
{
    destroySectionVertexAddressBuffer();


    constexpr VkDeviceSize tableBytes =
        static_cast<VkDeviceSize>(
            SECTION_VERTEX_ADDRESS_SLOT_LIMIT
        ) *
        sizeof(GPUSectionVertexAddress);

    createBuffer(
        g_device,
        g_physicalDevice,
        tableBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_sectionVertexAddressBuffer,
        g_sectionVertexAddressMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        tableBytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_sectionVertexAddressStagingBuffer,
        g_sectionVertexAddressStagingMemory,
        true
    );


    void* mapped = nullptr;

    check(
        vkMapMemory(
            g_device,
            g_sectionVertexAddressStagingMemory,
            0,
            tableBytes,
            0,
            &mapped
        ),
        "map Section vertex address table failed"
    );

    g_sectionVertexAddressMapped =
        mapped;

    std::memset(
        g_sectionVertexAddressMapped,
        0,
        static_cast<size_t>(tableBytes)
    );


    g_sectionVertexAddressCapacity =
        SECTION_VERTEX_ADDRESS_SLOT_LIMIT;


    GPUSectionVertexAddress placeholder{};

    placeholder.addressLo =
        static_cast<uint32_t>(
            g_testVertexDeviceAddress &
            0xFFFFFFFFull
        );

    placeholder.addressHi =
        static_cast<uint32_t>(
            g_testVertexDeviceAddress >>
            32
        );

    static_cast<GPUSectionVertexAddress*>(
        g_sectionVertexAddressMapped
    )[0] = placeholder;
    s_sectionVertexAddressDirtyMin = 0u;
    s_sectionVertexAddressDirtyMax = SECTION_VERTEX_ADDRESS_SLOT_LIMIT;


    constexpr VkDeviceSize roleBytes =
        static_cast<VkDeviceSize>(
            SECTION_VERTEX_ADDRESS_SLOT_LIMIT
        ) *
        sizeof(uint32_t);

    createBuffer(
        g_device,
        g_physicalDevice,
        roleBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_sectionRoleBuffer,
        g_sectionRoleMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        roleBytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_sectionRoleStagingBuffer,
        g_sectionRoleStagingMemory,
        true
    );


    void* roleMapped = nullptr;

    check(
        vkMapMemory(
            g_device,
            g_sectionRoleStagingMemory,
            0,
            roleBytes,
            0,
            &roleMapped
        ),
        "map Section role table failed"
    );

    g_sectionRoleMapped = roleMapped;

    std::memset(
        g_sectionRoleMapped,
        0,
        static_cast<size_t>(roleBytes)
    );
    s_sectionRoleDirtyMin = 0u;
    s_sectionRoleDirtyMax = SECTION_VERTEX_ADDRESS_SLOT_LIMIT;


    g_lodCoverageWordCount =
        (static_cast<uint32_t>(LOD_COVERAGE_MAX_SIZE) *
             static_cast<uint32_t>(LOD_COVERAGE_MAX_SIZE) +
         3u) /
        4u;

    const VkDeviceSize coverageBytes =
        static_cast<VkDeviceSize>(sizeof(LodCoverageHeader)) +
        static_cast<VkDeviceSize>(g_lodCoverageWordCount) *
            sizeof(uint32_t);

    createBuffer(
        g_device,
        g_physicalDevice,
        coverageBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        g_lodCoverageBuffer,
        g_lodCoverageMemory
    );

    createBuffer(
        g_device,
        g_physicalDevice,
        coverageBytes,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_lodCoverageStagingBuffer,
        g_lodCoverageStagingMemory,
        true
    );


    void* coverageMapped = nullptr;

    check(
        vkMapMemory(
            g_device,
            g_lodCoverageStagingMemory,
            0,
            coverageBytes,
            0,
            &coverageMapped
        ),
        "map LOD coverage bitmap failed"
    );

    g_lodCoverageMapped = coverageMapped;

    // A zero header means sizeX == 0, which the shader reads as "no coverage
    // data yet" -- every far-field hit is then kept, i.e. no dedup.
    std::memset(
        g_lodCoverageMapped,
        0,
        static_cast<size_t>(coverageBytes)
    );
    s_lodCoverageDirty = true;


    updateSectionVertexAddressDescriptor();
}


// Writes one Section's vertex address into the slot it has just acquired.  The
// address is constant for the Section's lifetime, so this is the only write its
// slot ever receives.
void writeSectionVertexAddressSlot(
    const RTSectionAS& section
)
{
    if(
        g_sectionVertexAddressMapped == nullptr ||
        !section.hasVertexAddressSlot ||
        section.vertexAddressSlot >= SECTION_VERTEX_ADDRESS_SLOT_LIMIT ||
        section.vertexAddress == 0
    )
    {
        throw std::runtime_error(
            "Section vertex address slot write rejected"
        );
    }


    GPUSectionVertexAddress entry{};

    entry.addressLo =
        static_cast<uint32_t>(
            section.vertexAddress &
            0xFFFFFFFFull
        );

    entry.addressHi =
        static_cast<uint32_t>(
            section.vertexAddress >>
            32
        );

    entry.motionOffset =
        static_cast<uint32_t>(
            section.vertexMotionOffset
        );

    entry.vertexStride =
        section.indexedGeometry
            ? (section.vertexStride | 1u)
            : section.vertexStride;


    static_cast<GPUSectionVertexAddress*>(
        g_sectionVertexAddressMapped
    )[section.vertexAddressSlot] = entry;

    if(section.vertexAddressSlot < s_sectionVertexAddressDirtyMin)
    {
        s_sectionVertexAddressDirtyMin = section.vertexAddressSlot;
    }
    if(section.vertexAddressSlot + 1u > s_sectionVertexAddressDirtyMax)
    {
        s_sectionVertexAddressDirtyMax = section.vertexAddressSlot + 1u;
    }
}


// Hands a stable table slot to a Section that does not have one yet.  Recycled
// slots only enter the free list after a queue drain, and new slots come from
// the high-water mark that no instance can reference yet, so the single write
// below never disturbs an entry an in-flight frame could be reading.  A
// Section's entry therefore never has to move again.
//
// 2026-10-13：返回值 = 本 Section 这一轮是否拿到了槽。
//   槽位耗尽**不再 throw** —— 旧代码在这里直接抛
//   "Section vertex address table slots exhausted"，异常会终止整轮世界几何重建
//   （而它只由 1<<18 的上限或超大视距触发，属于可恢复的运行期状态，不是程序缺陷）。
//   现在改为：打一条限频告警 + 返回 false，调用点把这个 Section 从本轮的
//   orderedSections 里摘掉 ⇒ 它本轮不进 TLAS（表现与距离剔除相同，符合
//   "允许视距外缺失"的口径），下一次退役环 drain 回收槽位后自然恢复。
//   vertexAddress == 0 仍然 throw：那是真正的不变量破坏（顶点缓冲缺失），
//   历史上从未触发，不该被静默吞掉。
bool ensureSectionVertexAddressSlot(
    RTSectionAS& section
)
{
    if(section.hasVertexAddressSlot)
    {
        return true;
    }


    if(section.vertexAddress == 0)
    {
        throw std::runtime_error(
            "Section vertex address table contains an invalid entry"
        );
    }


    if(!g_sectionVertexAddressFreeSlots.empty())
    {
        section.vertexAddressSlot =
            g_sectionVertexAddressFreeSlots.back();

        g_sectionVertexAddressFreeSlots.pop_back();
    }
    else
    {
        if(
            g_sectionVertexAddressNextSlot >=
            SECTION_VERTEX_ADDRESS_SLOT_LIMIT
        )
        {
            static uint64_t exhaustedEvents = 0u;
            ++exhaustedEvents;

            if(exhaustedEvents == 1u || (exhaustedEvents % 600u) == 0u)
            {
                std::stringstream warn;
                warn
                    << "[SECTION AS] vertexAddressSlots exhausted"
                    << " limit=" << SECTION_VERTEX_ADDRESS_SLOT_LIMIT
                    << " events=" << exhaustedEvents
                    << " action=skipSectionThisFrame";
                RTLog(warn.str());
            }

            return false;
        }

        section.vertexAddressSlot =
            g_sectionVertexAddressNextSlot++;
    }


    // 2026-11-15：分配后立刻校验"该槽当前没有活着的持有者"。
    // 命中说明空闲表被污染（同一槽被回收两次），两个活段共用一张顶点表项 ⇒
    // 其中一个段着色会读到别人的顶点数据（视觉上就是一块洞）。留痕并换新槽。
    if(slotLiveSet(section.vertexAddressSlot))
    {
        static uint32_t sCollisionLogCount = 0u;

        if((++sCollisionLogCount % 20u) == 1u)
        {
            RTLog(
                "[SLOT COLLISION] slot=" +
                std::to_string(section.vertexAddressSlot) +
                " sectionKey=" + std::to_string(section.sectionKey) +
                " total=" + std::to_string(sCollisionLogCount) +
                " action=freshSlot");
        }

        if(g_sectionVertexAddressNextSlot <
           SECTION_VERTEX_ADDRESS_SLOT_LIMIT)
        {
            section.vertexAddressSlot =
                g_sectionVertexAddressNextSlot++;

            slotLiveSet(section.vertexAddressSlot);
        }
    }

    section.hasVertexAddressSlot =
        true;

    writeSectionVertexAddressSlot(
        section
    );

    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_clearChunkLayers(
    JNIEnv*,
    jclass,
    jint sectionX,
    jint sectionY,
    jint sectionZ,
    jint layerMask)
{
    constexpr int TERRAIN_LAYER_LIMIT = 5;

    if(layerMask == 0)
    {
        return JNI_FALSE;
    }

    bool removedAny = false;

    std::lock_guard<std::timed_mutex> lock(g_chunkMutex);

    for(int layerId = 0; layerId < TERRAIN_LAYER_LIMIT; ++layerId)
    {
        if((layerMask & (1 << layerId)) == 0)
        {
            continue;
        }

        const uint64_t chunkKey =
            packSectionKey(
                static_cast<int>(sectionX),
                static_cast<int>(sectionY),
                static_cast<int>(sectionZ),
                layerId
            );

        bool removedCachedLayer =
            g_chunkMeshes.erase(chunkKey) > 0u;

        if(layerId == 1)
        {
            removedCachedLayer =
                g_cutoutOmmInputs.erase(chunkKey) > 0u ||
                removedCachedLayer;
        }

        g_chunkRawPayloads.erase(chunkKey);
        g_chunkUploadDigests.erase(chunkKey);

        if(removedCachedLayer)
        {
            g_dirtySectionKeys.insert(
                chunkKey & SPATIAL_SECTION_KEY_MASK);
            removedAny = true;
        }
    }

    if(removedAny)
    {
        markWorldGeometryDirty();
    }

    return removedAny ? JNI_TRUE : JNI_FALSE;
}

// Publishes the role of the Section that owns this slot.  Written on every
// rebuild rather than once per slot, because a recycled slot can be handed to a
// Section of a different kind (and because a DH section can be removed and a
// fresh spatial key allocated for a different section in its place).
void writeSectionRoleSlot(const RTSectionAS& section)
{
    if(g_sectionRoleMapped == nullptr ||
       !section.hasVertexAddressSlot ||
       section.vertexAddressSlot >= SECTION_VERTEX_ADDRESS_SLOT_LIMIT)
    {
        return;
    }

    // 2026-11-06b：只在 role 真的变化时写 GPU 表。
    //
    // g_sectionRoleMapped 是主机可见映射（write-combined）。这里是**离散** 4B 写
    // （每个槽一处、槽序随机），一次写就要刷一次 WC 缓冲 —— 实测 ord 段
    // 3.04ms / 约 6000 段 ≈ 0.5µs 每次，正是这个特征。
    // 用普通内存里的影子数组做比较（缓存内读），命中则整帧几乎零写入。
    //
    // 正确性：
    //   · 影子以 g_sectionRoleMapped 指针为代际，映射重建（memset 0）时整体
    //     失效重填，保证每个槽至少写一次；
    //   · 槽位回收再分配给别的段时，若新 role 与旧值相同，GPU 表里本来就是该值，
    //     跳过是安全的；不同则照常写。
    static void* sRoleShadowBase = nullptr;
    static std::vector<uint32_t> sRoleShadow;

    if(sRoleShadowBase != g_sectionRoleMapped ||
       sRoleShadow.size() < SECTION_VERTEX_ADDRESS_SLOT_LIMIT)
    {
        sRoleShadow.assign(
            SECTION_VERTEX_ADDRESS_SLOT_LIMIT,
            0xFFFFFFFFu);
        sRoleShadowBase = g_sectionRoleMapped;
    }

    const uint32_t role = sectionRoleOf(section);
    uint32_t& shadow = sRoleShadow[section.vertexAddressSlot];

    if(shadow == role)
    {
        return;
    }

    shadow = role;

    static_cast<uint32_t*>(
        g_sectionRoleMapped
    )[section.vertexAddressSlot] = role;

    if(section.vertexAddressSlot < s_sectionRoleDirtyMin)
    {
        s_sectionRoleDirtyMin = section.vertexAddressSlot;
    }
    if(section.vertexAddressSlot + 1u > s_sectionRoleDirtyMax)
    {
        s_sectionRoleDirtyMax = section.vertexAddressSlot + 1u;
    }
}

void recordSectionTablesSync(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE)
    {
        return;
    }

    VkBufferMemoryBarrier barriers[3]{};
    uint32_t barrierCount = 0u;

    auto addBarrier = [&](VkBuffer buf, VkDeviceSize offset, VkDeviceSize size)
    {
        VkBufferMemoryBarrier& b = barriers[barrierCount++];
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.buffer = buf;
        b.offset = offset;
        b.size = size;
    };

    if(s_sectionVertexAddressDirtyMin < s_sectionVertexAddressDirtyMax &&
       g_sectionVertexAddressStagingBuffer != VK_NULL_HANDLE &&
       g_sectionVertexAddressBuffer != VK_NULL_HANDLE &&
       g_sectionVertexAddressCapacity > 0u)
    {
        const uint32_t clampedMax = std::min<uint32_t>(
            s_sectionVertexAddressDirtyMax,
            static_cast<uint32_t>(g_sectionVertexAddressCapacity));
        if(s_sectionVertexAddressDirtyMin < clampedMax)
        {
            const VkDeviceSize off =
                static_cast<VkDeviceSize>(s_sectionVertexAddressDirtyMin) *
                sizeof(GPUSectionVertexAddress);
            const VkDeviceSize len =
                static_cast<VkDeviceSize>(clampedMax - s_sectionVertexAddressDirtyMin) *
                sizeof(GPUSectionVertexAddress);
            const VkBufferCopy region{off, off, len};
            vkCmdCopyBuffer(
                cmd,
                g_sectionVertexAddressStagingBuffer,
                g_sectionVertexAddressBuffer,
                1u,
                &region);
            addBarrier(g_sectionVertexAddressBuffer, off, len);
        }
        s_sectionVertexAddressDirtyMin = 0xFFFFFFFFu;
        s_sectionVertexAddressDirtyMax = 0u;
    }

    if(s_sectionRoleDirtyMin < s_sectionRoleDirtyMax &&
       g_sectionRoleStagingBuffer != VK_NULL_HANDLE &&
       g_sectionRoleBuffer != VK_NULL_HANDLE &&
       g_sectionVertexAddressCapacity > 0u)
    {
        const uint32_t clampedMax = std::min<uint32_t>(
            s_sectionRoleDirtyMax,
            static_cast<uint32_t>(g_sectionVertexAddressCapacity));
        if(s_sectionRoleDirtyMin < clampedMax)
        {
            const VkDeviceSize off =
                static_cast<VkDeviceSize>(s_sectionRoleDirtyMin) *
                sizeof(uint32_t);
            const VkDeviceSize len =
                static_cast<VkDeviceSize>(clampedMax - s_sectionRoleDirtyMin) *
                sizeof(uint32_t);
            const VkBufferCopy region{off, off, len};
            vkCmdCopyBuffer(
                cmd,
                g_sectionRoleStagingBuffer,
                g_sectionRoleBuffer,
                1u,
                &region);
            addBarrier(g_sectionRoleBuffer, off, len);
        }
        s_sectionRoleDirtyMin = 0xFFFFFFFFu;
        s_sectionRoleDirtyMax = 0u;
    }

    if(s_lodCoverageDirty &&
       g_lodCoverageStagingBuffer != VK_NULL_HANDLE &&
       g_lodCoverageBuffer != VK_NULL_HANDLE &&
       g_lodCoverageWordCount > 0u)
    {
        const VkDeviceSize coverageBytes =
            static_cast<VkDeviceSize>(sizeof(LodCoverageHeader)) +
            static_cast<VkDeviceSize>(g_lodCoverageWordCount) * sizeof(uint32_t);
        const VkBufferCopy region{0u, 0u, coverageBytes};
        vkCmdCopyBuffer(
            cmd,
            g_lodCoverageStagingBuffer,
            g_lodCoverageBuffer,
            1u,
            &region);
        addBarrier(g_lodCoverageBuffer, 0u, coverageBytes);
        s_lodCoverageDirty = false;
    }

    if(barrierCount > 0u)
    {
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u,
            0u, nullptr,
            barrierCount, barriers,
            0u, nullptr);
    }
}

namespace
{
    constexpr uint32_t kBlasCompactPoolSize = 256u;
    constexpr uint32_t kMaxBlasCompactionsPerFrame = 6u;

    struct PendingBlasCompactQuery
    {
        uint64_t sectionKey = 0u;
        VkAccelerationStructureKHR origBlas = VK_NULL_HANDLE;
        VkDeviceSize origBytes = 0u;
        uint32_t queryIndex = 0u;
        uint64_t buildFrameSerial = 0u;
    };

    VkQueryPool s_blasCompactQueryPool = VK_NULL_HANDLE;
    std::vector<uint32_t> s_freeCompactQueries;
    std::vector<PendingBlasCompactQuery> s_pendingBlasCompactions;
    uint64_t s_totalBlasCompacted = 0u;
    uint64_t s_totalBlasSavedBytes = 0u;
}

void initBlasCompactionResources()
{
    if(s_blasCompactQueryPool != VK_NULL_HANDLE || g_device == VK_NULL_HANDLE)
    {
        return;
    }

    VkQueryPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    poolInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    poolInfo.queryCount = kBlasCompactPoolSize;

    if(vkCreateQueryPool(g_device, &poolInfo, nullptr, &s_blasCompactQueryPool) != VK_SUCCESS)
    {
        s_blasCompactQueryPool = VK_NULL_HANDLE;
        return;
    }

    s_freeCompactQueries.clear();
    s_freeCompactQueries.reserve(kBlasCompactPoolSize);
    for(uint32_t i = kBlasCompactPoolSize; i > 0u; --i)
    {
        s_freeCompactQueries.push_back(i - 1u);
    }
    s_pendingBlasCompactions.clear();
}

void destroyBlasCompactionResources()
{
    if(g_device != VK_NULL_HANDLE && s_blasCompactQueryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(g_device, s_blasCompactQueryPool, nullptr);
    }
    s_blasCompactQueryPool = VK_NULL_HANDLE;
    s_freeCompactQueries.clear();
    s_pendingBlasCompactions.clear();
    s_totalBlasCompacted = 0u;
    s_totalBlasSavedBytes = 0u;
}

void enqueueBlasCompactionQuery(VkCommandBuffer cmd, RTSectionAS& section)
{
    if(!section.compactCapable ||
       section.blasCompacted ||
       section.blas == VK_NULL_HANDLE ||
       cmd == VK_NULL_HANDLE)
    {
        return;
    }

    auto cmdWriteProps = getCmdWriteAccelerationStructuresProperties();
    if(!cmdWriteProps)
    {
        return;
    }

    initBlasCompactionResources();
    if(s_blasCompactQueryPool == VK_NULL_HANDLE || s_freeCompactQueries.empty())
    {
        return;
    }

    const uint32_t queryIndex = s_freeCompactQueries.back();
    s_freeCompactQueries.pop_back();

    vkCmdResetQueryPool(cmd, s_blasCompactQueryPool, queryIndex, 1u);
    cmdWriteProps(
        cmd,
        1u,
        &section.blas,
        VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
        s_blasCompactQueryPool,
        queryIndex
    );

    s_pendingBlasCompactions.push_back(PendingBlasCompactQuery{
        section.sectionKey,
        section.blas,
        section.blasBytes,
        queryIndex,
        g_perfL10FrameSerial
    });
}

bool recordPendingBlasCompactions(
    VkCommandBuffer cmd,
    std::vector<BlasCompactedAddressRemap>& outRemaps,
    bool heavyBuildFrame)
{
    outRemaps.clear();
    if(cmd == VK_NULL_HANDLE ||
       s_blasCompactQueryPool == VK_NULL_HANDLE ||
       s_pendingBlasCompactions.empty())
    {
        return false;
    }

    auto cmdCopyAS = getCmdCopyAccelerationStructure();
    auto createAS = getCreateAccelerationStructure();
    auto getASAddress = getGetAccelerationStructureDeviceAddress();
    if(!cmdCopyAS || !createAS || !getASAddress)
    {
        return false;
    }

    // Burst-amortize compaction during heavy multi-section terrain rebuilds so we do not
    // force a full TLAS MODE_BUILD every single flight frame; still drain when >=50% full.
    if(heavyBuildFrame &&
       s_pendingBlasCompactions.size() < (kBlasCompactPoolSize / 2u) &&
       ((g_perfL10FrameSerial & 15u) != 0u || s_pendingBlasCompactions.size() < 16u))
    {
        return false;
    }

    std::vector<PendingBlasCompactQuery> remaining;
    remaining.reserve(s_pendingBlasCompactions.size());
    uint32_t compactedThisFrame = 0u;

    const auto compactStart = std::chrono::steady_clock::now();

    for(size_t i = 0u; i < s_pendingBlasCompactions.size(); ++i)
    {
        const PendingBlasCompactQuery& entry = s_pendingBlasCompactions[i];
        const bool budgetExceeded =
            compactedThisFrame >= 1u &&
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - compactStart
            ).count() >= 0.8;
        if(g_perfL10FrameSerial < entry.buildFrameSerial + 2u ||
           compactedThisFrame >= kMaxBlasCompactionsPerFrame ||
           budgetExceeded)
        {
            remaining.push_back(entry);
            continue;
        }

        uint64_t compactedSize = 0u;
        const VkResult queryResult = vkGetQueryPoolResults(
            g_device,
            s_blasCompactQueryPool,
            entry.queryIndex,
            1u,
            sizeof(uint64_t),
            &compactedSize,
            sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT
        );

        if(queryResult == VK_NOT_READY)
        {
            remaining.push_back(entry);
            continue;
        }

        s_freeCompactQueries.push_back(entry.queryIndex);
        if(queryResult != VK_SUCCESS)
        {
            continue;
        }

        auto secIt = g_sectionBLAS.find(entry.sectionKey);
        if(secIt == g_sectionBLAS.end())
        {
            continue;
        }

        RTSectionAS& sec = secIt->second;
        if(sec.blas != entry.origBlas ||
           sec.blasCompacted ||
           !sec.hasBlasMemoryChunk)
        {
            continue;
        }

        sec.blasCompacted = true;

        if(compactedSize == 0u ||
           compactedSize + 4096u >= sec.blasBytes ||
           compactedSize > (sec.blasBytes * 7u) / 8u)
        {
            continue;
        }

        VkBuffer newBuf = VK_NULL_HANDLE;
        VkBufferCreateInfo bufInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufInfo.size = compactedSize;
        bufInfo.usage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if(vkCreateBuffer(g_device, &bufInfo, nullptr, &newBuf) != VK_SUCCESS)
        {
            continue;
        }

        VkMemoryRequirements memReq{};
        vkGetBufferMemoryRequirements(g_device, newBuf, &memReq);
        const uint32_t memType = findMemoryType(
            g_physicalDevice,
            memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );

        VkDeviceMemory newMem = VK_NULL_HANDLE;
        VkDeviceSize newOffset = 0u;
        VkDeviceSize newChunkBytes = 0u;
        uint32_t newSlab = 0u;
        uint32_t newChunk = 0u;
        if(!acquireSectionMemory(
                memReq.size,
                std::max<VkDeviceSize>(memReq.alignment, 256u),
                memType,
                newMem,
                newOffset,
                newChunkBytes,
                newSlab,
                newChunk,
                sectionPoolChunkWasteLog2()))
        {
            vkDestroyBuffer(g_device, newBuf, nullptr);
            continue;
        }

        if(vkBindBufferMemory(g_device, newBuf, newMem, newOffset) != VK_SUCCESS)
        {
            releaseSectionMemory(newSlab, newChunk);
            vkDestroyBuffer(g_device, newBuf, nullptr);
            continue;
        }

        VkAccelerationStructureKHR newAs = VK_NULL_HANDLE;
        VkAccelerationStructureCreateInfoKHR createInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR
        };
        createInfo.buffer = newBuf;
        createInfo.offset = 0u;
        createInfo.size = compactedSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if(createAS(g_device, &createInfo, nullptr, &newAs) != VK_SUCCESS)
        {
            releaseSectionMemory(newSlab, newChunk);
            vkDestroyBuffer(g_device, newBuf, nullptr);
            continue;
        }

        VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR
        };
        addrInfo.accelerationStructure = newAs;
        const VkDeviceAddress newAddr = getASAddress(g_device, &addrInfo);

        VkCopyAccelerationStructureInfoKHR copyInfo{
            VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR
        };
        copyInfo.src = sec.blas;
        copyInfo.dst = newAs;
        copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        cmdCopyAS(cmd, &copyInfo);

        RTSectionAS oldBlasOnly{};
        oldBlasOnly.blas = sec.blas;
        oldBlasOnly.blasBuffer = sec.blasBuffer;
        oldBlasOnly.blasAddress = sec.blasAddress;
        oldBlasOnly.blasBytes = sec.blasBytes;
        oldBlasOnly.blasMemorySlab = sec.blasMemorySlab;
        oldBlasOnly.blasMemoryChunk = sec.blasMemoryChunk;
        oldBlasOnly.blasMemoryChunkBytes = sec.blasMemoryChunkBytes;
        oldBlasOnly.hasBlasMemoryChunk = sec.hasBlasMemoryChunk;
        destroySectionAS(oldBlasOnly);

        const VkDeviceAddress oldAddr = sec.blasAddress;
        sec.blas = newAs;
        sec.blasBuffer = newBuf;
        sec.blasAddress = newAddr;
        sec.blasBytes = compactedSize;
        sec.blasMemorySlab = newSlab;
        sec.blasMemoryChunk = newChunk;
        sec.blasMemoryChunkBytes = newChunkBytes;
        sec.hasBlasMemoryChunk = true;

        outRemaps.push_back(BlasCompactedAddressRemap{oldAddr, newAddr});
        ++compactedThisFrame;
        ++s_totalBlasCompacted;
        s_totalBlasSavedBytes += (entry.origBytes - compactedSize);

        if(s_totalBlasCompacted == 64u || (s_totalBlasCompacted % 256u) == 0u)
        {
            RTLog(
                "[BLAS COMPACT] total=" + std::to_string(s_totalBlasCompacted) +
                " savedMB=" + std::to_string(static_cast<double>(s_totalBlasSavedBytes) / (1024.0 * 1024.0))
            );
        }
    }

    s_pendingBlasCompactions.swap(remaining);

    if(!outRemaps.empty())
    {
        VkMemoryBarrier compactBarrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        compactBarrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        compactBarrier.dstAccessMask =
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
            VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0u, 1u, &compactBarrier, 0u, nullptr, 0u, nullptr
        );
        return true;
    }

    return false;
}
