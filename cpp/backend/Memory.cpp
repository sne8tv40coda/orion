// Orion backend — Memory 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

uint32_t findMemoryType(VkPhysicalDevice pd,uint32_t filter,VkMemoryPropertyFlags props){
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd,&mp);


    for(uint32_t i=0;i<mp.memoryTypeCount;i++)
    {
        if((filter&(1<<i))&&(mp.memoryTypes[i].propertyFlags&props)==props)
        {
            return i;
        }
    }

    throw std::runtime_error("Failed to find suitable memory type");
}

// =====================================================
// 2026-10-09（内存/卡顿根因修复）：**上传类**缓冲的内存类型选择。
//
// 病灶：上面的 findMemoryType() 取的是**下标最小**的匹配项。而 Vulkan 的类型表
// 通常把 `DEVICE_LOCAL | HOST_VISIBLE`（BAR / window 堆 —— 没开 ReBAR 时只有
// 256MB）排在前面，把"系统内存里的 host-visible"排在后面。于是本工程**所有**
// HOST_VISIBLE 缓冲都被塞进那块小堆，其中两类是"主机写、设备只读一次"的上传缓冲：
//   · Section 的 OMM 输入（三角形数 × 64B，实测 ~1.1MB/section，每个 CUTOUT
//     Section 一份；[RT MEM] 的 omm= 峰值 835 ⇒ 量级约 1GB）
//   · 纹理上传 staging（`capacity = bytes*1.5 + 64KB`，实测单次 7~20MB）
//
// 现场证据（[PERF L10 ALLOC]，2026-10-09 那一场）：
//     usage=0x820000（OMM 输入）request=1186432  ms=41.26
//     usage=0x820000（OMM 输入）request=1038848  ms=54.77
//     usage=0x1（纹理 staging） request=7~20MB   ms=3.2~10.7（十几连发）
//     usage=0x1                request=36576     ms=9.02   ← 36KB 也要 9ms
// 最后一行是关键：36KB 的分配要 9ms，只可能是 BAR 堆被超额认购后驱动在整理/驱逐。
// 而溢出的部分落到系统内存 ⇒ [RT MEM] 里 rssMB/privateMB 涨，而原生各结构全都有界。
//
// 所以：上传缓冲应当放在**系统内存**里（这正是 staging 内存的用途）。这里显式
// 优先选"满足 props 且**不在 DEVICE_LOCAL 堆**"的类型；平台没有这种类型时
// 逐位退回原选择器，行为与改动前完全一致。
//
// 只给上传/构建输入用，**不要**给每帧被光线追踪反复读的缓冲（本地光、动态光、
// 地址表、TLAS 实例表）用 —— 那些走 PCIe 会变慢，保持现状。
uint32_t findHostUploadMemoryType(VkPhysicalDevice pd,uint32_t filter,VkMemoryPropertyFlags props){
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(pd,&mp);

    for(uint32_t i=0;i<mp.memoryTypeCount;i++)
    {
        if((filter&(1u<<i))==0u)
        {
            continue;
        }
        if((mp.memoryTypes[i].propertyFlags&props)!=props)
        {
            continue;
        }
        if((mp.memoryHeaps[mp.memoryTypes[i].heapIndex].flags&
            VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)!=0u)
        {
            continue;
        }
        return i;
    }

    return findMemoryType(pd,filter,props);
}
void check(VkResult r,const char* s){ if(r != VK_SUCCESS) { const std::string msg = std::string("[CHECK FAILED] ") + s + " VkResult=" + std::to_string((int)r); RTLogFlushed(msg); throw std::runtime_error(msg); } }
sl::float4x4 JOMLColumnMajorToSLRowMajor(
    const float m[16]
)
{
    sl::float4x4 out{};

    out[0] = sl::float4(
        m[0],
        m[1],
        m[2],
        m[3]
    );

    out[1] = sl::float4(
        m[4],
        m[5],
        m[6],
        m[7]
    );

    out[2] = sl::float4(
        m[8],
        m[9],
        m[10],
        m[11]
    );

    out[3] = sl::float4(
        m[12],
        m[13],
        m[14],
        m[15]
    );

    return out;
}
void createRTCommandPool()
{
    VkCommandPoolCreateInfo ci{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO
    };

    ci.queueFamilyIndex = g_queueFamily;

    ci.flags =
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;


    check(
        vkCreateCommandPool(
            g_device,
            &ci,
            nullptr,
            &g_rtCommandPool
        ),
        "Failed to create RT command pool"
    );


}
void createBuffer(VkDevice d,VkPhysicalDevice pd,VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags props,VkBuffer& buffer,VkDeviceMemory& memory,bool preferHostUploadMemory){
    const std::chrono::steady_clock::time_point perfL10AllocStart = std::chrono::steady_clock::now();
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=usage;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(d,&bi,nullptr,&buffer),"Failed to create buffer");
    VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(d,buffer,&mr);
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;
    ai.memoryTypeIndex = preferHostUploadMemory
        ? findHostUploadMemoryType(pd,mr.memoryTypeBits,props)
        : findMemoryType(pd,mr.memoryTypeBits,props);
    if(usage&VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT){fi.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;ai.pNext=&fi;}
    VkResult r=vkAllocateMemory(d,&ai,nullptr,&memory);
    if(r!=VK_SUCCESS){
        vkDestroyBuffer(d,buffer,nullptr);buffer=VK_NULL_HANDLE;
        RTLogFlushed(
            std::string("[ALLOC FAIL] VkResult=") + std::to_string(static_cast<int>(r)) +
            " request=" + std::to_string(static_cast<unsigned long long>(size)) +
            " aligned=" + std::to_string(static_cast<unsigned long long>(mr.size)) +
            " usage=0x" + std::to_string(static_cast<unsigned long long>(usage)));
        check(r,"Failed to allocate buffer memory");
    }
    r=vkBindBufferMemory(d,buffer,memory,0);
    if(r!=VK_SUCCESS){vkFreeMemory(d,memory,nullptr);vkDestroyBuffer(d,buffer,nullptr);memory=VK_NULL_HANDLE;buffer=VK_NULL_HANDLE;check(r,"Failed to bind buffer memory");}

    rtxNoteBufferMemory(
        static_cast<uint64_t>(usage),
        static_cast<uint64_t>(mr.size),
        memory
    );

    {
        const double perfL10AllocMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - perfL10AllocStart).count();
        if(perfL10AllocMs >= 1.0)
        {
            VkPhysicalDeviceMemoryProperties allocMemoryProperties{};
            vkGetPhysicalDeviceMemoryProperties(pd,&allocMemoryProperties);
            const uint32_t allocHeapIndex =
                allocMemoryProperties.memoryTypes[ai.memoryTypeIndex].heapIndex;
            const bool allocHeapIsDeviceLocal =
                (allocMemoryProperties.memoryHeaps[allocHeapIndex].flags &
                 VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u;

            std::ostringstream allocLog;
            allocLog << std::fixed << std::setprecision(2)
                << "[PERF L10 ALLOC] frame=" << g_perfL10FrameSerial
                << " ms=" << perfL10AllocMs
                << " request=" << static_cast<unsigned long long>(size)
                << " aligned=" << static_cast<unsigned long long>(mr.size)
                << " usage=0x" << std::hex
                << static_cast<unsigned long long>(usage) << std::dec
                << " type=" << ai.memoryTypeIndex
                << " heap=" << allocHeapIndex
                << " deviceLocal=" << (allocHeapIsDeviceLocal ? 1 : 0);
            RTLogFlushed(allocLog.str());
        }
    }
}

// ---------------------------------------------------------------------------
// 2026-10-04 TLAS 视锥剔除的数学件。viewInverse / projInverse 是 JOML 列主序
// （m[col*4+row]，CAMPROBE 处 m[12..14]=相机位置的判读同口径）。
// ---------------------------------------------------------------------------
void mat4Mul(const float a[16], const float b[16], float out[16])
{
    for(int col = 0; col < 4; ++col)
    {
        for(int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for(int k = 0; k < 4; ++k)
            {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}
bool mat4Invert(const float in[16], float out[16])
{
    float inv[16];
    float det;

    inv[0]  =  in[5] * in[10] * in[15] - in[5] * in[11] * in[14] - in[9] * in[6] * in[15] + in[9] * in[7] * in[14] + in[13] * in[6] * in[11] - in[13] * in[7] * in[10];
    inv[4]  = -in[4] * in[10] * in[15] + in[4] * in[11] * in[14] + in[8] * in[6] * in[15] - in[8] * in[7] * in[14] - in[12] * in[6] * in[11] + in[12] * in[7] * in[10];
    inv[8]  =  in[4] * in[9] * in[15] - in[4] * in[11] * in[13] - in[8] * in[5] * in[15] + in[8] * in[7] * in[13] + in[12] * in[5] * in[11] - in[12] * in[7] * in[9];
    inv[12] = -in[4] * in[9] * in[14] + in[4] * in[10] * in[13] + in[8] * in[5] * in[14] - in[8] * in[6] * in[13] - in[12] * in[5] * in[10] + in[12] * in[6] * in[9];
    inv[1]  = -in[1] * in[10] * in[15] + in[1] * in[11] * in[14] + in[9] * in[2] * in[15] - in[9] * in[3] * in[14] - in[13] * in[2] * in[11] + in[13] * in[3] * in[10];
    inv[5]  =  in[0] * in[10] * in[15] - in[0] * in[11] * in[14] - in[8] * in[2] * in[15] + in[8] * in[3] * in[14] + in[12] * in[2] * in[11] - in[12] * in[3] * in[10];
    inv[9]  = -in[0] * in[9] * in[15] + in[0] * in[11] * in[13] + in[8] * in[1] * in[15] - in[8] * in[3] * in[13] - in[12] * in[1] * in[11] + in[12] * in[3] * in[9];
    inv[13] =  in[0] * in[9] * in[14] - in[0] * in[10] * in[13] - in[8] * in[1] * in[14] + in[8] * in[2] * in[13] + in[12] * in[1] * in[10] - in[12] * in[2] * in[9];
    inv[2]  =  in[1] * in[6] * in[15] - in[1] * in[7] * in[14] - in[5] * in[2] * in[15] + in[5] * in[3] * in[14] + in[13] * in[2] * in[7] - in[13] * in[3] * in[6];
    inv[6]  = -in[0] * in[6] * in[15] + in[0] * in[7] * in[14] + in[4] * in[2] * in[15] - in[4] * in[3] * in[14] - in[12] * in[2] * in[7] + in[12] * in[3] * in[6];
    inv[10] =  in[0] * in[5] * in[15] - in[0] * in[7] * in[13] - in[4] * in[1] * in[15] + in[4] * in[3] * in[13] + in[12] * in[1] * in[7] - in[12] * in[3] * in[5];
    inv[14] = -in[0] * in[5] * in[14] + in[0] * in[6] * in[13] + in[4] * in[1] * in[14] - in[4] * in[2] * in[13] - in[12] * in[1] * in[6] + in[12] * in[2] * in[5];
    inv[3]  = -in[1] * in[6] * in[11] + in[1] * in[7] * in[10] + in[5] * in[2] * in[11] - in[5] * in[3] * in[10] - in[9] * in[2] * in[7] + in[9] * in[3] * in[6];
    inv[7]  =  in[0] * in[6] * in[11] - in[0] * in[7] * in[10] - in[4] * in[2] * in[11] + in[4] * in[3] * in[10] + in[8] * in[2] * in[7] - in[8] * in[3] * in[6];
    inv[11] = -in[0] * in[5] * in[11] + in[0] * in[7] * in[9] + in[4] * in[1] * in[11] - in[4] * in[3] * in[9] - in[8] * in[1] * in[7] + in[8] * in[3] * in[5];
    inv[15] =  in[0] * in[5] * in[10] - in[0] * in[6] * in[9] - in[4] * in[1] * in[10] + in[4] * in[2] * in[9] + in[8] * in[1] * in[6] - in[8] * in[2] * in[5];

    det = in[0] * inv[0] + in[1] * inv[4] + in[2] * inv[8] + in[3] * inv[12];

    if(det == 0.0f || !std::isfinite(det))
    {
        return false;
    }

    const float invDet = 1.0f / det;
    for(int i = 0; i < 16; ++i)
    {
        out[i] = inv[i] * invDet;
        if(!std::isfinite(out[i]))
        {
            return false;
        }
    }
    return true;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setPostProcessingParams(
    JNIEnv* env, jclass, jfloatArray params)
{
    const jsize paramCount = params != nullptr ? env->GetArrayLength(params) : 0;
    if(paramCount < 7) return JNI_FALSE;
    jfloat values[12] = {};
    env->GetFloatArrayRegion(params, 0, std::min<jsize>(paramCount, 12), values);
    for(int i = 0; i < std::min<jsize>(paramCount, 12); ++i)
    {
        if(!std::isfinite(static_cast<float>(values[i]))) return JNI_FALSE;
    }
    g_postTonemapMode.store(
        std::min(std::max(static_cast<int>(values[0] + 0.5f), 0), 6));
    g_postExposureStrength.store(
        std::min(std::max(static_cast<float>(values[1]), 0.0f), 1.0f));
    g_postContrast.store(
        std::min(std::max(static_cast<float>(values[2]), 0.5f), 2.0f));
    g_postSaturation.store(
        std::min(std::max(static_cast<float>(values[3]), 0.0f), 2.0f));
    g_postGamma.store(
        std::min(std::max(static_cast<float>(values[4]), 1.6f), 2.8f));
    g_mirrorBounceCount.store(
        std::min(std::max(static_cast<int>(values[5] + 0.5f), 1), 8));
    g_tlasCullDistance.store(
        std::min(std::max(static_cast<int>(values[6] + 0.5f), 0), 512));
    g_tlasFrustumCullMargin.store(
        paramCount >= 8
            ? std::min(std::max(static_cast<int>(values[7] + 0.5f), 0), 256)
            : 0);
    g_postBloomStrength.store(
        paramCount >= 9
            ? std::min(std::max(static_cast<float>(values[8]), 0.0f), 2.0f)
            : 0.0f);
    g_postGainR.store(
        paramCount >= 12
            ? std::min(std::max(static_cast<float>(values[9]), 0.25f), 2.5f)
            : 1.0f);
    g_postGainG.store(
        paramCount >= 12
            ? std::min(std::max(static_cast<float>(values[10]), 0.25f), 2.5f)
            : 1.0f);
    g_postGainB.store(
        paramCount >= 12
            ? std::min(std::max(static_cast<float>(values[11]), 0.25f), 2.5f)
            : 1.0f);
    return JNI_TRUE;
}

// === 新增：创建 Camera UBO ===
// 2026-10-26：28 个 HP 参数（CameraUBO 尾块 hpShapeA..hpMiscA）的**唯一**写入点。
// 必须逐帧调用 —— 原来这段只写在 createCameraUBO() 里（启动时执行一次），
// 于是面板里 28 条 HP 滑条全部失效（改的只是原子量，UBO 永远停在初值）。
void populateHpParams(CameraUBO* ubo)
{
    ubo->hpShapeA[0] = g_hpDetailStrengthCu.load();
    ubo->hpShapeA[1] = g_hpDetailStrengthTcu.load();
    ubo->hpShapeA[2] = g_hpDetailStrengthCb.load();
    ubo->hpShapeA[3] = g_hpBottomSmoothHeight.load();
    ubo->hpShapeB[0] = g_hpWispyEdgeWidth.load();
    ubo->hpShapeB[1] = g_hpWispyReach.load();
    ubo->hpShapeB[2] = g_hpWispyTopHeight.load();
    ubo->hpShapeB[3] = g_hpWispyTopHardness.load();
    ubo->hpShapeC[0] = g_hpWispyWeight.load();
    ubo->hpShapeC[1] = g_hpBillowyWeight.load();
    ubo->hpShapeC[2] = g_hpLoCoverTopMax.load();
    ubo->hpShapeC[3] = g_hpLoCoverTopStr.load();
    ubo->hpShapeD[0] = g_hpCovMapScale.load();
    ubo->hpShapeD[1] = g_hpScWorleyStrength.load();
    ubo->hpShapeD[2] = g_hpScHeightScale.load();
    ubo->hpShapeD[3] = g_hpScCellThickStr.load();
    ubo->hpLightA[0] = g_hpForwardEcc.load();
    ubo->hpLightA[1] = g_hpBackwardEcc.load();
    ubo->hpLightA[2] = g_hpPhiFwdIntensity.load();
    ubo->hpLightA[3] = g_hpPhiFwdDepthPow.load();
    ubo->hpLightB[0] = g_hpPhiFwdDepthBias.load();
    ubo->hpLightB[1] = g_hpPhiFwdCompress.load();
    ubo->hpLightB[2] = g_hpScatterOdScale.load();
    ubo->hpLightB[3] = g_hpScatterCurvePow.load();
    ubo->hpMiscA[0] = g_hpMsEccentricity.load();
    ubo->hpMiscA[1] = g_hpAoUpwardScale.load();
    ubo->hpMiscA[2] = g_hpFadeStart.load();
    ubo->hpMiscA[3] = g_hpPowderIntensity.load();
}
void createCameraUBO() {
    createBuffer(
        g_device,
        g_physicalDevice,
        sizeof(CameraUBO),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_cameraUboBuffer,
        g_cameraUboMemory
    );
    g_cameraUboMapped = &g_cameraUboHost;
    CameraUBO* ubo = static_cast<CameraUBO*>(g_cameraUboMapped);
    ubo->vertexBufferAddrLo = static_cast<uint32_t>(g_testVertexDeviceAddress & 0xFFFFFFFF);
    ubo->vertexBufferAddrHi = static_cast<uint32_t>(g_testVertexDeviceAddress >> 32);
    ubo->padding = 0u;

    ubo->dhFarFieldParams[0] = 0.0f;
    ubo->dhFarFieldParams[1] = 0.0f;
    ubo->dhFarFieldParams[2] = 0.0f;
    ubo->dhFarFieldParams[3] = 0.0f;

    for(int dhMatrixInitIndex = 0; dhMatrixInitIndex < 16; ++dhMatrixInitIndex)
    {
        ubo->dhFarFieldInvMvp[dhMatrixInitIndex] = 0.0f;
        ubo->dhFarFieldMvp[dhMatrixInitIndex] = 0.0f;
    }

    ubo->dhFarFieldLight[0] = 0.0f;
    ubo->dhFarFieldLight[1] = 0.0f;
    ubo->dhFarFieldLight[2] = 1.0f;
    ubo->dhFarFieldLight[3] = 0.0f;

    ubo->historyParamsPadding0 =
        g_pomDepth.load();

    ubo->historyParamsPadding1 =
        0.0f;

    ubo->pomParams[0] =
        g_pomSharp.load();
    ubo->pomParams[1] = 0.0f;
    ubo->pomParams[2] = 0.0f;
    ubo->pomParams[3] = 0.0f;

    ubo->sssParams[0] =
        g_sssMode.load();
    ubo->sssParams[1] = 0.0f;
    ubo->sssParams[2] = 0.0f;
    ubo->sssParams[3] = 0.0f;

    ubo->giHalfParams[0] = 0.0f;
    ubo->giHalfParams[1] = 0.0f;
    ubo->giHalfParams[2] = 0.0f;
    ubo->giHalfParams[3] = 0.0f;

    ubo->giRayParams[0] = 0.0f;
    ubo->giRayParams[1] = 0.0f;
    ubo->giRayParams[2] = 0.0f;
    ubo->giRayParams[3] = 0.0f;

    ubo->sparseParams[0] = 0.0f;
    ubo->sparseParams[1] = 0.0f;
    ubo->sparseParams[2] = 1.0f;
    ubo->sparseParams[3] = 0.0f;

    populateHpParams(ubo);

    ubo->playerX = 0.0f;
    ubo->playerY = 0.0f;
    ubo->playerZ = 0.0f;
    ubo->playerInWater = 0.0f;
    ubo->playerVelocityX = 0.0f;
    ubo->playerVelocityY = 0.0f;
    ubo->playerVelocityZ = 0.0f;
    ubo->playerHorizontalSpeed = 0.0f;
    ubo->waterSurfaceY = 0.0f;
    ubo->waterSimOriginX = 0.0f;
    ubo->waterSimOriginZ = 0.0f;
    ubo->waterSimTexelWorldSize = WATER_SIM_TEXEL_WORLD_SIZE;
    ubo->waterSimReadLayer = 0u;
    ubo->waterSimEnabled = 0u;
    ubo->waterSimImpulsePending = 0u;
    ubo->cameraInWater = 0u;
    ubo->waterSimImpulseX = 0.0f;
    ubo->waterSimImpulseZ = 0.0f;
    ubo->waterSimImpulseStrength = 0.0f;
    ubo->waterSimImpulseSurfaceY = 0.0f;

    ubo->weatherParams[0] = 0.0f;
    ubo->weatherParams[1] = 0.0f;
    ubo->weatherParams[2] = 1.0f;
    ubo->weatherParams[3] = 1.0f;
    ubo->auroraParams[0] = 0.0f;
    ubo->auroraParams[1] = 1.0f;
    ubo->auroraParams[2] = 1.0f;
    ubo->auroraParams[3] = 0.0f;
    ubo->rtSampleParams[0] = 1.0f;
    ubo->rtSampleParams[1] = 1.0f;
    ubo->rtSampleParams[2] = 32.0f;
    ubo->rtSampleParams[3] = 0.0f;
    populateCloudParams(ubo->cloudParams);
    populateSkyParams(ubo->skyParams);
    populateSkyModelParams(ubo->skyModelParams);
    populateGalaxyParams(ubo->galaxyParams);
    populateGodRayParams(ubo->godRayParams);
    populateGroundMistParams(ubo->godRayParams + 4);
    populateRainWallParams(ubo->rainWallParams);
    populateWaterStyleParams(ubo->waterStyleParams);
    populateHighCloudParams(ubo->highCloudParams);
    populateCloudMsParams(ubo->cloudMsParams);
    populateCloudMarchParams(ubo->cloudMarchParams);
    populateWaterParams(ubo->waterParams);
    populateCloudPhiParams(ubo->cloudPhiParams);
    populateCloudLightParams(ubo->cloudLightParams);
    populateCloudShapeParams(ubo->cloudShapeParams);
    populateCloudMoonParams(ubo->cloudMoonParams);
    populateCloudShape2Params(ubo->cloudShape2Params);
    populateCloudAmbientParams(ubo->cloudAmbientParams);
    populateCloudMsParams2(ubo->cloudMsParams2);
    populateCloudLightParams2(ubo->cloudLightParams2);
    populateCloudProfileParams(ubo->cloudProfileParams);
    populateCloudDetailParams(ubo->cloudDetailParams);
    populateFogExtrasParams(ubo->fogExtrasParams);
    populateCloudTemporalParams(ubo->cloudTemporalParams);
    populateCloudAerialParams(ubo->cloudAerialParams);

    populateCloudSkyHueParams(ubo->cloudSkyHueParams);
    populateCloudPhysicsParams(ubo->cloudPhysicsParams);
    populateUeCloudParams(ubo->ueCloudA, ubo->ueCloudB);

    {
        const uint64_t sharcAddresses[4] = {
            g_sharcHashEntriesAddress,
            g_sharcAccumulationAddress,
            g_sharcResolvedAddress,
            g_sharcLockAddress
        };

        for(uint32_t sharcIndex = 0u; sharcIndex < 4u; ++sharcIndex)
        {
            ubo->sharcBuffersLo[sharcIndex] =
                static_cast<uint32_t>(sharcAddresses[sharcIndex] & 0xFFFFFFFFull);
            ubo->sharcBuffersHi[sharcIndex] =
                static_cast<uint32_t>(sharcAddresses[sharcIndex] >> 32u);
        }

        const float sharcSceneScaleValue = sharcSceneScale();

        const float sharcRadianceScaleValue = 1000.0f;
        uint32_t sharcSceneScaleBits = 0u;
        uint32_t sharcRadianceScaleBits = 0u;
        std::memcpy(&sharcSceneScaleBits, &sharcSceneScaleValue, sizeof(uint32_t));
        std::memcpy(&sharcRadianceScaleBits, &sharcRadianceScaleValue, sizeof(uint32_t));

        ubo->sharcCacheParams[0] =
            g_sharcBuffersReady ? sharcSceneScaleBits : 0u;
        ubo->sharcCacheParams[1] =
            g_sharcBuffersReady ? g_sharcEntryCountResolved : 0u;
        ubo->sharcCacheParams[2] =
            g_sharcBuffersReady ? sharcRadianceScaleBits : 0u;
        ubo->sharcCacheParams[3] = sharcModeLane();
    }
    populateWaterWaveParams(ubo->waterWaveParams);
    populateWaterWaveShapeParams(ubo->waterWaveShapeParams);
    populateWaterSssParams(ubo->waterSssParams);
    populateWaterFftParams(ubo->waterFftParams);
}
