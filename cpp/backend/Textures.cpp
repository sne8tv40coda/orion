// Orion backend — Textures 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// 2026-10-29：maxLod 放开（修"高光一块一块"）的**运行期回退闸**。
//   ORION_PBR_MIP 未设 / 非 '0' 开头 => 放开（默认，修复生效）
//   ORION_PBR_MIP=0            => maxLod 回 0.0f（逐位回到改动前的"永远 mip0"）
// 只在建采样器时读一次（与 sharcEnabled 等同一种写法），不影响逐帧成本。
bool pbrAtlasMipLodEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_PBR_MIP");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}
void ensurePbrAtlasSampler()
{
    if(g_pbrAtlasSampler != VK_NULL_HANDLE || g_device == VK_NULL_HANDLE)
    {
        return;
    }

    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = VK_FILTER_LINEAR;
    info.minFilter = VK_FILTER_LINEAR;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.mipLodBias = 0.0f;
    info.anisotropyEnable = VK_FALSE;
    info.maxAnisotropy = 1.0f;
    info.compareEnable = VK_FALSE;
    info.compareOp = VK_COMPARE_OP_NEVER;
    info.minLod = 0.0f;
    info.maxLod =
        pbrAtlasMipLodEnabled() ? VK_LOD_CLAMP_NONE : 0.0f;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;

    if(vkCreateSampler(g_device, &info, nullptr, &g_pbrAtlasSampler) != VK_SUCCESS)
    {
        g_pbrAtlasSampler = VK_NULL_HANDLE;
        RTLog("[PBR SAMPLER] linear _n/_s sampler creation failed; using atlas sampler");
        return;
    }

    RTLog(
        std::string("[PBR SAMPLER] linear _n/_s sampler created "
                    "(mip LINEAR, maxLod=") +
        (pbrAtlasMipLodEnabled() ? "NONE" : "0/ORION_PBR_MIP=0") +
        ", clamp-to-edge, aniso=off)");
}
bool dhFarFieldMatrixAvailable()
{
    return g_dhFarFieldMatrixValid;
}

// P1 合成阶段才会用到；现在只由 publishDhTextures 写入，供后续 pass 读取。
bool dhTexturesAvailable()
{
    return g_dhTexturesValid && g_dhTexturesSameDevice;
}
VkSampler getLinearSampler()
{
    return g_atlasSampler;
}

extern "C" JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_updateAtlas(
    JNIEnv* env, jclass clazz, jlong imageView, jlong sampler)
{
    const bool atlasChanged =
        g_atlasImageView != (VkImageView)imageView ||
        g_atlasSampler != (VkSampler)sampler;

    if (atlasChanged) {
        g_atlasImageView = (VkImageView)imageView;
        g_atlasSampler = (VkSampler)sampler;
        g_atlasDirty = true;

        std::stringstream ps;
        ps << "[ATLAS HIJACK] C++ Received Atlas: ImageView=0x" << std::hex << imageView
           << ", Sampler=0x" << sampler;
        RTLog(ps.str());
    }

    if(g_dynamicTextureBindingsNeedAtlasRefresh)
    {
        g_atlasDirty = true;
        g_dynamicTextureBindingsCanPublish = true;

        if(g_atlasReloadAwaitingPublish)
        {
            g_atlasReloadAwaitingPublish = false;

            std::stringstream reloadLog;
            reloadLog
                << "[ATLAS HIJACK] reload publish"
                << " handleChanged=" << (atlasChanged ? 1 : 0)
                << " recycled="
                << ((VkImageView)imageView == g_atlasImageViewAtReload &&
                    (VkSampler)sampler == g_atlasSamplerAtReload ? 1 : 0)
                << " ImageView=0x" << std::hex << imageView;
            RTLog(reloadLog.str());
        }
    }
}

// Distant Horizons 远场纹理句柄入口。
//
// 为什么单独开一个入口、而不是复用 updateAtlas：DH 的这三张纹理与 MC 的区块图集
// 生命周期完全不同（DH 会在换世界/改渲染距离时重建，且尺寸是窗口分辨率而不是图集
// 尺寸），把状态混在一起以后会很难分辨是谁把句柄弄脏的。
//
// 返回值 = "这套句柄可以用"。false 的可能原因有三种，日志里会分别写明：
//   1) 句柄为零（DH 那边还没创建）；
//   2) 我们自己的 g_device 还没有被打入（initDeviceHandlesNative 未调用）；
//   3) 设备不同源——这种情况下句柄不能用于我们的 descriptor，必须走外部内存方案。
extern "C" JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_publishDhTextures(
    JNIEnv*, jclass,
    jlong colorImage, jlong colorView, jlong colorSampler,
    jlong depthImage, jlong depthView, jlong depthSampler,
    jlong atlasImage, jlong atlasView, jlong atlasSampler,
    jlong dhDevice, jint width, jint height)
{
    if(colorImage == 0 || colorView == 0 || depthView == 0)
    {
        RTLogFlushed(
            "[DH TEX] 收到不完整句柄组（color image/view 或 depth view 为零），忽略"
        );

        return JNI_FALSE;
    }

    const bool dhHandlesChanged =
        g_dhColorView    != reinterpret_cast<VkImageView>(colorView) ||
        g_dhColorSampler != reinterpret_cast<VkSampler>(colorSampler) ||
        g_dhDepthView    != reinterpret_cast<VkImageView>(depthView) ||
        g_dhDepthSampler != reinterpret_cast<VkSampler>(depthSampler) ||
        g_dhAtlasView    != reinterpret_cast<VkImageView>(atlasView) ||
        g_dhAtlasSampler != reinterpret_cast<VkSampler>(atlasSampler) ||
        !g_dhTexturesValid;

    g_dhColorImage   = reinterpret_cast<VkImage>(colorImage);
    g_dhColorView    = reinterpret_cast<VkImageView>(colorView);
    g_dhColorSampler = reinterpret_cast<VkSampler>(colorSampler);
    g_dhDepthImage   = reinterpret_cast<VkImage>(depthImage);
    g_dhDepthView    = reinterpret_cast<VkImageView>(depthView);
    g_dhDepthSampler = reinterpret_cast<VkSampler>(depthSampler);
    g_dhAtlasImage   = reinterpret_cast<VkImage>(atlasImage);
    g_dhAtlasView    = reinterpret_cast<VkImageView>(atlasView);
    g_dhAtlasSampler = reinterpret_cast<VkSampler>(atlasSampler);
    g_dhTextureWidth  = static_cast<int>(width);
    g_dhTextureHeight = static_cast<int>(height);
    g_dhTexturesValid = true;

    if(dhHandlesChanged)
    {
        ++g_dhTexturesRevision;
    }

    const bool haveOwnDevice = (g_device != VK_NULL_HANDLE);
    g_dhTexturesSameDevice =
        haveOwnDevice &&
        dhDevice != 0 &&
        dhDevice == static_cast<jlong>(
            reinterpret_cast<uintptr_t>(g_device)
        );

    std::ostringstream dhLog;
    dhLog << "[DH TEX] 收到远场纹理 color(image=0x" << std::hex << colorImage
          << " view=0x" << colorView << " sampler=0x" << colorSampler
          << ") depth(view=0x" << depthView << ") atlas(view=0x" << atlasView
          << std::dec << ") 尺寸=" << width << "x" << height
          << " DH设备=0x" << std::hex << dhDevice
          << " 我们的设备=0x" << reinterpret_cast<uintptr_t>(g_device)
          << std::dec
          << " ⇒ " << (g_dhTexturesSameDevice
                 ? "同一设备，可直接建 descriptor"
                 : (haveOwnDevice
                        ? "★设备不同源，句柄不能直接用"
                        : "★我们自己的设备句柄尚未打入（initDeviceHandlesNative 未调用）"));

    RTLogFlushed(dhLog.str());

    g_dhAtlasUsable = g_dhTexturesSameDevice;

    return g_dhTexturesSameDevice ? JNI_TRUE : JNI_FALSE;
}

// Distant Horizons：只推送 blockRatioAtlas 句柄（2026-09-30）。
//
// 与 publishDhTextures 的区别：那个要求 color/depth 成套（只有方案 c "采样 DH
// 画面"才需要），这里只收 atlas —— 远场顶点的 textureTileId 采样（rchit binding
// 49）只需要它。DH 的图集在 rendererMode=DISABLED 时不会由 DH 自己创建（它的
// BlazeDhTerrainRenderer 从不执行），改由 Java 侧直接调
// BlazeBlockTextureAtlas.uploadPendingTiles() 建出来，再从这里推给 C++。
//
// 三个判定与 publishDhTextures 一致：句柄为零 / 我们自己的设备未打入 / 设备不同源。
// 返回值 = "这张 atlas 可以用"。
extern "C" JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_publishDhBlockRatioAtlas(
    JNIEnv*, jclass,
    jlong atlasImage, jlong atlasView, jlong atlasSampler,
    jlong dhDevice)
{
    if(atlasView == 0 || atlasSampler == 0)
    {
        RTLogFlushed(
            "[DH TEX] 收到空的 blockRatioAtlas 句柄（view/sampler 为零），忽略"
        );

        return JNI_FALSE;
    }

    const bool atlasChanged =
        g_dhAtlasView    != reinterpret_cast<VkImageView>(atlasView) ||
        g_dhAtlasSampler != reinterpret_cast<VkSampler>(atlasSampler);

    g_dhAtlasImage   = reinterpret_cast<VkImage>(atlasImage);
    g_dhAtlasView    = reinterpret_cast<VkImageView>(atlasView);
    g_dhAtlasSampler = reinterpret_cast<VkSampler>(atlasSampler);

    const bool haveOwnDevice = (g_device != VK_NULL_HANDLE);
    g_dhAtlasUsable =
        haveOwnDevice &&
        dhDevice != 0 &&
        dhDevice == static_cast<jlong>(
            reinterpret_cast<uintptr_t>(g_device)
        );

    if(g_dhAtlasUsable && atlasChanged)
    {
        ++g_dhAtlasRevision;
    }

    std::ostringstream log;
    log << "[DH TEX] 收到 blockRatioAtlas(image=0x" << std::hex << atlasImage
        << " view=0x" << atlasView << " sampler=0x" << atlasSampler
        << " DH设备=0x" << dhDevice
        << " 我们的设备=0x" << reinterpret_cast<uintptr_t>(g_device)
        << std::dec << ") ⇒ "
        << (g_dhAtlasUsable
                ? "同一设备，仅 atlas 可用"
                : (haveOwnDevice
                       ? "★设备不同源，句柄不能直接用"
                       : "★我们自己的设备句柄尚未打入（initDeviceHandlesNative 未调用）"));

    RTLogFlushed(log.str());

    return g_dhAtlasUsable ? JNI_TRUE : JNI_FALSE;
}

// ---------------------------------------------------------------------------
// DH stage B: per-frame far-field geometry payload.
//
// Layout of the incoming float[24] (Java side: DhFarFieldGeometry):
//   [0..15] DH's inverse model-view-projection matrix, exactly as
//           DhApiMat4f.getValuesAsArray() hands it over - the raygen resolves
//           the storage order itself, see the mode/variant lanes.
//   [16]    stage mode       (0 off, 1 error map, 2 normals, 3 relight, 4 relight+atmosphere)
//   [17]    relight strength
//   [18]    relight gain
//   [19]    variant override (0 auto, 1..4 forced)
//   [20]    DH near clip plane (blocks, DH recomputes it every frame)
//   [21]    DH far clip plane  (blocks)
//   [22..23] reserved
//
// An all-zero matrix is the "invalidate" signal: Java sends it on world change,
// and the far field then falls back to the stage-1 passthrough instead of
// reconstructing from a dead matrix.
//
// Returns whether the payload was accepted.  Zero Vulkan calls.
// ---------------------------------------------------------------------------
extern "C" JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_updateDhFarField(
    JNIEnv* env,
    jclass,
    jfloatArray payload)
{
    if(payload == nullptr)
    {
        return JNI_FALSE;
    }

    const jsize length = env->GetArrayLength(payload);

    if(length < 24)
    {
        RTLogFlushed(
            "[DH MAT] 载荷过短（" + std::to_string(static_cast<int>(length))
            + " < 24），忽略"
        );

        return JNI_FALSE;
    }

    const jsize copyLength = length < 40 ? 24 : 40;
    float values[40] = {};
    env->GetFloatArrayRegion(payload, 0, copyLength, values);

    bool anyMatrixValue = false;
    bool allFinite      = std::isfinite(values[20]) && std::isfinite(values[21]);

    for(int i = 0; i < 16; ++i)
    {
        if(values[i] != 0.0f)
        {
            anyMatrixValue = true;
        }

        if(!std::isfinite(values[i]))
        {
            allFinite = false;
        }
    }

    if(!allFinite)
    {
        ++g_dhFarFieldRejects;

        if(g_dhFarFieldRejectLogs < 3)
        {
            ++g_dhFarFieldRejectLogs;

            std::ostringstream rejectLog;
            rejectLog << "[DH MAT] 丢弃非有限推送（第 " << g_dhFarFieldRejects << " 次）"
                      << " near=" << values[20]
                      << " far=" << values[21]
                      << " v0=" << values[0]
                      << " v5=" << values[5]
                      << " v15=" << values[15]
                      << " ⇒ 保持上一帧的好矩阵，远场几何不受影响";

            RTLogFlushed(rejectLog.str());
        }

        return JNI_FALSE;
    }

    if(!anyMatrixValue)
    {
        if(g_dhFarFieldMatrixValid)
        {
            RTLogFlushed("[DH MAT] 收到全零矩阵（换世界/卸载）⇒ 远场几何阶段关闭，回落 stage-1 直通");
        }

        g_dhFarFieldMatrixValid = false;
        g_dhFarFieldPushes = 0;
        g_dhFarFieldRejects = 0;

        return JNI_FALSE;
    }

    for(int i = 0; i < 16; ++i)
    {
        g_dhFarFieldInvMvp[i] = values[i];
    }

    g_dhFarFieldMode     = values[16];
    g_dhFarFieldStrength = values[17];
    g_dhFarFieldGain     = values[18];
    g_dhFarFieldVariant  = values[19];
    g_dhFarFieldNear     = values[20];
    g_dhFarFieldFar      = values[21];
    if(length >= 40)
    {
        for(int i = 0; i < 16; ++i)
        {
            g_dhFarFieldMvp[i] = values[24 + i];
        }
    }
    else
    {
        for(int i = 0; i < 16; ++i)
        {
            g_dhFarFieldMvp[i] = 0.0f;
        }
    }
    g_dhFarFieldMatrixValid = true;
    ++g_dhFarFieldPushes;

    if(g_dhFarFieldLogs < 3)
    {
        ++g_dhFarFieldLogs;

        std::ostringstream matLog;
        matLog << "[DH MAT] 收到 DH 逆 MVP（第 " << g_dhFarFieldPushes << " 次）"
               << " near=" << g_dhFarFieldNear
               << " far=" << g_dhFarFieldFar
               << " mode=" << g_dhFarFieldMode
               << " strength=" << g_dhFarFieldStrength
               << " gain=" << g_dhFarFieldGain
               << " variant=" << g_dhFarFieldVariant
               << " 取样 v0=" << g_dhFarFieldInvMvp[0]
               << " v5=" << g_dhFarFieldInvMvp[5]
               << " v10=" << g_dhFarFieldInvMvp[10]
               << " v15=" << g_dhFarFieldInvMvp[15]
               << " 正向矩阵=" << (g_dhFarFieldMvp[0] != 0.0f ? "有(mode 13 方向采样可用)" : "无(退回屏幕直通)")
               << " 已丢弃非有限=" << g_dhFarFieldRejects
               << "（顺序由 rgen 自检，不在此假设）";

        RTLogFlushed(matLog.str());
    }

    return JNI_TRUE;
}

// 2026-10-14：把常驻暂存彻底收掉（解映射 + 销毁缓冲 + 释放内存 + 清零记账）。
// 逻辑逐位取自 acquireTextureUploadStaging 里原有的那段销毁，抽出来给下面两处共用。
void destroyTextureUploadStaging()
{
    if(g_textureUploadStaging == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_textureUploadStagingMapped != nullptr)
    {
        vkUnmapMemory(g_device, g_textureUploadStagingMemory);
        g_textureUploadStagingMapped = nullptr;
    }

    vkDestroyBuffer(g_device, g_textureUploadStaging, nullptr);
    vkFreeMemory(g_device, g_textureUploadStagingMemory, nullptr);
    g_textureUploadStaging = VK_NULL_HANDLE;
    g_textureUploadStagingMemory = VK_NULL_HANDLE;
    g_textureUploadStagingBytes = 0u;
}

// 取一块 ≥ bytes 的常驻暂存映射（不够大就重分配一次，按 1.5 倍 + 64KB 留余量，
// 免得相邻两次重载（图集尺寸会变）来回重建）。返回 nullptr 表示映射失败。
void* acquireTextureUploadStaging(VkDeviceSize bytes)
{
    if(g_textureUploadStaging != VK_NULL_HANDLE &&
       g_textureUploadStagingBytes >= bytes &&
       g_textureUploadStagingMapped != nullptr)
    {
        return g_textureUploadStagingMapped;
    }

    if(g_textureUploadStaging != VK_NULL_HANDLE)
    {
        destroyTextureUploadStaging();
    }

    const VkDeviceSize capacity =
        bytes + (bytes / 2u) + 65536u;

    createBuffer(
        g_device, g_physicalDevice, capacity,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_textureUploadStaging, g_textureUploadStagingMemory, true
    );

    void* mapped = nullptr;

    if(vkMapMemory(g_device, g_textureUploadStagingMemory, 0, capacity, 0, &mapped) != VK_SUCCESS)
    {
        return nullptr;
    }

    g_textureUploadStagingMapped = mapped;
    g_textureUploadStagingBytes = capacity;
    return g_textureUploadStagingMapped;
}

// 2026-10-14：越线时归还**超大**的纹理上传常驻暂存。返回归还的 MB（供 [VRAM GOV] 记账）。
//
// 更正一条旧结论：2026-10-13（乙-4）把 churnTop 里 `use=0x1 liveN=3 liveMB≈820` 判给了
// g_vertexStagingPool。2026-10-14 实测**否掉了它** —— [RT MEM] 的 vtxStage 只报
// `2/32.0MB`（池化修复之后），而 churnTop 仍是 `use=0x1 liveN=3 liveMB=808.2`
// 且 `vkLiveHostMB≈816` ⇒ 那 808MB 里约 768MB 是**本文件上方这块纹理上传暂存**：
// capacity = 图像字节 × 1.5 + 64KB，按一张 ~512MB 的大图算就是 ~768MB，并且它
// **常驻到进程结束**（原注释"按需增长、保持映射"是为免重载期间反复重建，代价是把
// 约 12% 的显存预算永久占住）。这也正是"换低配材质包就正常"的直接机制。
//
// 为什么要收：2026-10-14 那一局 [VRAM GOV] **全程 over=1**（ratio 0.95~1.006、
// keepCapMB=0）⇒ 空 slab 一律立刻释放、每次分配都要驱动腾挪 ⇒ 实测单次分配
// 10~50ms **且与大小无关**（分配 0.2MB 也要 41ms）⇒ 区块加载时被放大成顿挫。
// 把这 768MB 还回去，是把"常驻总量从悬崖边拉回来"的**单笔最大**一步。
//
// 安全性：这块暂存只被 uploadTextureToVRAM 的那次拷贝引用，而该提交已登记在
// g_textureUploadFencePending 上 ⇒ 这里**先把它等掉**再销毁（若早已信号则是微秒级），
// 语义与"下一次进入 uploadTextureToVRAM 时的等待"完全相同，没有引入新的时序约定。
// 上传是加载期行为（uploadPBRAtlases），下次重载会按需重建。
// 只收大块（> 64MB）：小块保留常驻，避免小幅重载来回重建。
double releaseHugeTextureUploadStaging()
{
    constexpr VkDeviceSize kKeepResidentBytes = 67108864u;
    if(g_textureUploadStaging == VK_NULL_HANDLE ||
       g_textureUploadStagingBytes <= kKeepResidentBytes)
    {
        return 0.0;
    }

    if(g_textureUploadFencePending &&
       g_textureUploadFence != VK_NULL_HANDLE)
    {
        check(
            vkWaitForFences(
                g_device,
                1,
                &g_textureUploadFence,
                VK_TRUE,
                std::numeric_limits<uint64_t>::max()
            ),
            "wait texture upload fence before releasing staging failed"
        );

        g_textureUploadFencePending = false;
    }

    const double freedMb =
        static_cast<double>(g_textureUploadStagingBytes) / 1048576.0;

    RTLog(
        "[TEX SCRATCH] releasing resident upload staging MB=" +
        std::to_string(freedMb)
    );

    destroyTextureUploadStaging();

    return freedMb;
}

// === 新增：统一的物理显存泵入管线 ===
// 2026-10-07（M1）：与命令池/设备同生共死。必须在 Java_..._cleanup 里、
// **命令池销毁之前**调用：命令缓冲属于 g_rtCommandPool（池一销毁它就失效），
// 暂存缓冲与 fence 属于设备。只销毁而不置空的话，第二次进世界时那三个 lazy 判据
// 会因为句柄非空而跳过重建 ⇒ 悬空句柄。
void releaseTextureUploadScratch()
{
    if(g_device != VK_NULL_HANDLE)
    {
        if(g_textureUploadFence != VK_NULL_HANDLE)
        {
            vkDestroyFence(g_device, g_textureUploadFence, nullptr);
        }

        if(g_textureUploadCommandBuffer != VK_NULL_HANDLE &&
           g_rtCommandPool != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(
                g_device,
                g_rtCommandPool,
                1u,
                &g_textureUploadCommandBuffer
            );
        }

        if(g_textureUploadStaging != VK_NULL_HANDLE)
        {
            if(g_textureUploadStagingMapped != nullptr)
            {
                vkUnmapMemory(g_device, g_textureUploadStagingMemory);
            }

            vkDestroyBuffer(g_device, g_textureUploadStaging, nullptr);
            vkFreeMemory(g_device, g_textureUploadStagingMemory, nullptr);
        }
    }

    g_textureUploadFence = VK_NULL_HANDLE;
    g_textureUploadCommandBuffer = VK_NULL_HANDLE;
    g_textureUploadStaging = VK_NULL_HANDLE;
    g_textureUploadStagingMemory = VK_NULL_HANDLE;
    g_textureUploadStagingMapped = nullptr;
    g_textureUploadStagingBytes = 0u;
}
bool pbrSpriteMipEnabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_PBR_SPRITE_MIP");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}

// 单级：按 sprite 矩形做 2x2 box，再把矩形外扩 1 圈填边缘。
void pbrDownsampleLevelBySprite(
    const uint8_t* prev, uint32_t prevW, uint32_t prevH,
    uint8_t* cur, uint32_t curW, uint32_t curH,
    uint32_t level,
    std::vector<uint8_t>& written)
{
    written.assign(size_t(curW) * size_t(curH), 0u);

    for (const PbrSpriteRect& rect : g_pbrSpriteRects)
    {
        const uint32_t rx = rect.x >> level;
        const uint32_t ry = rect.y >> level;

        if (rx >= curW || ry >= curH)
        {
            continue;
        }

        const uint32_t rw = std::max(1u, rect.w >> level);
        const uint32_t rh = std::max(1u, rect.h >> level);
        const uint32_t cw = std::min(rw, curW - rx);
        const uint32_t ch = std::min(rh, curH - ry);

        for (uint32_t y = 0u; y < ch; ++y)
        {
            const uint32_t sy0 = std::min(prevH - 1u, (ry + y) * 2u);
            const uint32_t sy1 = std::min(prevH - 1u, sy0 + 1u);

            for (uint32_t x = 0u; x < cw; ++x)
            {
                const uint32_t sx0 = std::min(prevW - 1u, (rx + x) * 2u);
                const uint32_t sx1 = std::min(prevW - 1u, sx0 + 1u);

                const uint8_t* p00 = prev + (size_t(sy0) * prevW + sx0) * 4u;
                const uint8_t* p10 = prev + (size_t(sy0) * prevW + sx1) * 4u;
                const uint8_t* p01 = prev + (size_t(sy1) * prevW + sx0) * 4u;
                const uint8_t* p11 = prev + (size_t(sy1) * prevW + sx1) * 4u;

                uint8_t* d = cur + (size_t(ry + y) * curW + (rx + x)) * 4u;

                for (uint32_t c = 0u; c < 4u; ++c)
                {
                    const uint32_t sum =
                        uint32_t(p00[c]) + uint32_t(p10[c]) +
                        uint32_t(p01[c]) + uint32_t(p11[c]);
                    d[c] = static_cast<uint8_t>((sum + 2u) / 4u);
                }

                written[size_t(ry + y) * curW + (rx + x)] = 1u;
            }
        }
    }

    for (const PbrSpriteRect& rect : g_pbrSpriteRects)
    {
        const uint32_t rx = rect.x >> level;
        const uint32_t ry = rect.y >> level;

        if (rx >= curW || ry >= curH)
        {
            continue;
        }

        const uint32_t rw = std::max(1u, rect.w >> level);
        const uint32_t rh = std::max(1u, rect.h >> level);
        const uint32_t cw = std::min(rw, curW - rx);
        const uint32_t ch = std::min(rh, curH - ry);

        const int32_t x0 = int32_t(rx) - 1;
        const int32_t y0 = int32_t(ry) - 1;
        const int32_t x1 = int32_t(rx + cw);
        const int32_t y1 = int32_t(ry + ch);

        for (int32_t y = y0; y <= y1; ++y)
        {
            if (y < 0 || y >= int32_t(curH))
            {
                continue;
            }

            for (int32_t x = x0; x <= x1; ++x)
            {
                if (x < 0 || x >= int32_t(curW))
                {
                    continue;
                }

                if (written[size_t(y) * curW + size_t(x)] != 0u)
                {
                    continue;
                }

                const int32_t cx =
                    x < int32_t(rx)
                        ? int32_t(rx)
                        : (x >= int32_t(rx + cw - 1u) ? int32_t(rx + cw - 1u) : x);
                const int32_t cy =
                    y < int32_t(ry)
                        ? int32_t(ry)
                        : (y >= int32_t(ry + ch - 1u) ? int32_t(ry + ch - 1u) : y);

                const uint8_t* s = cur + (size_t(cy) * curW + size_t(cx)) * 4u;
                uint8_t* d = cur + (size_t(y) * curW + size_t(x)) * 4u;

                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = s[3];
            }
        }
    }
}

// 按 sprite 生成 mip 链并上传（RGBA8）。失败时清掉半成品，由调用方决定回退。
void uploadPbrSpriteMippedToVRAM(
    void* pixels, uint32_t width, uint32_t height,
    VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    auto fail = [&](const char* why)
    {
        RTLog(std::string("[PBR SPRITE MIP] ") + why);
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(g_device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
        if (image != VK_NULL_HANDLE)
        {
            vkDestroyImage(g_device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
    };

    if (width < 2u || height < 2u || g_pbrSpriteRects.empty())
    {
        fail("degenerate input");
        return;
    }

    uint32_t mipLevels = 1u;
    for (uint32_t s = std::max(width, height); s > 1u; s >>= 1u)
    {
        ++mipLevels;
    }

    std::vector<VkDeviceSize> offsets(mipLevels, 0u);
    VkDeviceSize totalBytes = 0u;
    for (uint32_t l = 0u; l < mipLevels; ++l)
    {
        offsets[l] = totalBytes;
        const uint32_t lw = std::max(1u, width >> l);
        const uint32_t lh = std::max(1u, height >> l);
        totalBytes += VkDeviceSize(lw) * VkDeviceSize(lh) * 4u;
    }

    if (g_textureUploadFencePending && g_textureUploadFence != VK_NULL_HANDLE)
    {
        check(
            vkWaitForFences(
                g_device, 1u, &g_textureUploadFence, VK_TRUE,
                std::numeric_limits<uint64_t>::max()),
            "wait previous texture upload fence failed");
        g_textureUploadFencePending = false;
    }

    void* stagingMapped = acquireTextureUploadStaging(totalBytes);
    if (stagingMapped == nullptr)
    {
        fail("staging map failed");
        return;
    }

    uint8_t* stagingBytes = static_cast<uint8_t*>(stagingMapped);
    const uint8_t* sourceBytes = static_cast<const uint8_t*>(pixels);
    const size_t mip0Bytes = size_t(width) * size_t(height) * 4u;

    std::memcpy(stagingBytes + offsets[0], sourceBytes, mip0Bytes);

    std::vector<uint8_t> prev(sourceBytes, sourceBytes + mip0Bytes);
    std::vector<uint8_t> cur;
    std::vector<uint8_t> written;

    for (uint32_t l = 1u; l < mipLevels; ++l)
    {
        const uint32_t lw = std::max(1u, width >> l);
        const uint32_t lh = std::max(1u, height >> l);

        cur.assign(size_t(lw) * size_t(lh) * 4u, 0u);

        pbrDownsampleLevelBySprite(
            prev.data(), std::max(1u, width >> (l - 1u)), std::max(1u, height >> (l - 1u)),
            cur.data(), lw, lh, l, written);

        std::memcpy(stagingBytes + offsets[l], cur.data(), cur.size());
        prev.swap(cur);
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1u;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1u;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(g_device, &imageInfo, nullptr, &image) != VK_SUCCESS)
    {
        image = VK_NULL_HANDLE;
        fail("vkCreateImage failed");
        return;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(g_device, image, &memReq);

    uint32_t typeIndex = 0xFFFFFFFFu;
    try
    {
        typeIndex = findMemoryType(
            g_physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    catch (const std::exception& e)
    {
        RTLog(std::string("[PBR SPRITE MIP] no memory type: ") + e.what());
    }

    if (typeIndex == 0xFFFFFFFFu)
    {
        fail("no DEVICE_LOCAL memory type");
        return;
    }

    VkMemoryAllocateInfo memAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memAlloc.allocationSize = memReq.size;
    memAlloc.memoryTypeIndex = typeIndex;

    if (vkAllocateMemory(g_device, &memAlloc, nullptr, &memory) != VK_SUCCESS)
    {
        memory = VK_NULL_HANDLE;
        fail("vkAllocateMemory failed");
        return;
    }

    if (vkBindImageMemory(g_device, image, memory, 0u) != VK_SUCCESS)
    {
        fail("vkBindImageMemory failed");
        return;
    }

    if (g_textureUploadCommandBuffer == VK_NULL_HANDLE)
    {
        VkCommandBufferAllocateInfo cbAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandPool = g_rtCommandPool;
        cbAlloc.commandBufferCount = 1u;

        if (vkAllocateCommandBuffers(g_device, &cbAlloc, &g_textureUploadCommandBuffer) != VK_SUCCESS)
        {
            g_textureUploadCommandBuffer = VK_NULL_HANDLE;
            fail("vkAllocateCommandBuffers failed");
            return;
        }
    }

    VkCommandBuffer cmd = g_textureUploadCommandBuffer;
    vkResetCommandBuffer(cmd, 0u);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.baseMipLevel = 0u;
    toDst.subresourceRange.levelCount = mipLevels;
    toDst.subresourceRange.baseArrayLayer = 0u;
    toDst.subresourceRange.layerCount = 1u;
    toDst.srcAccessMask = 0u;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &toDst);

    std::vector<VkBufferImageCopy> regions(mipLevels);
    for (uint32_t l = 0u; l < mipLevels; ++l)
    {
        regions[l] = VkBufferImageCopy{};
        regions[l].bufferOffset = offsets[l];
        regions[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[l].imageSubresource.mipLevel = l;
        regions[l].imageSubresource.baseArrayLayer = 0u;
        regions[l].imageSubresource.layerCount = 1u;
        regions[l].imageExtent.width = std::max(1u, width >> l);
        regions[l].imageExtent.height = std::max(1u, height >> l);
        regions[l].imageExtent.depth = 1u;
    }

    vkCmdCopyBufferToImage(
        cmd,
        g_textureUploadStaging,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        mipLevels,
        regions.data());

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &toRead);

    vkEndCommandBuffer(cmd);

    if (g_textureUploadFence == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

        if (vkCreateFence(g_device, &fenceInfo, nullptr, &g_textureUploadFence) != VK_SUCCESS)
        {
            g_textureUploadFence = VK_NULL_HANDLE;
            fail("vkCreateFence failed");
            return;
        }
    }

    vkResetFences(g_device, 1u, &g_textureUploadFence);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &cmd;

    if (vkQueueSubmit(g_queue, 1u, &submit, g_textureUploadFence) != VK_SUCCESS)
    {
        fail("vkQueueSubmit failed");
        return;
    }

    g_textureUploadFencePending = true;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0u;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0u;
    viewInfo.subresourceRange.layerCount = 1u;

    if (vkCreateImageView(g_device, &viewInfo, nullptr, &view) != VK_SUCCESS)
    {
        view = VK_NULL_HANDLE;
        fail("vkCreateImageView failed");
        return;
    }

    {
        const double totalMb = static_cast<double>(totalBytes) / 1048576.0;
        char line[320];
        std::snprintf(
            line, sizeof(line),
            "[PBR SPRITE MIP] sprite-aligned mip chain uploaded. size=%ux%u levels=%u "
            "rects=%u bytesMB=%.1f",
            width, height, mipLevels,
            static_cast<unsigned>(g_pbrSpriteRects.size()), totalMb);
        RTLog(line);
    }
}

// ============================================================================
// 2026-10-29（P2b）：PBR 图集 BC7 压缩 —— ORION_PBR_BC7（默认开）
//
// 为什么是 BC7：8 bpp（RGBA8 是 32 bpp）⇒ 图集显存 4×。8192² 两张：
//   RGBA8 + mip ≈ 682 MiB  ->  BC7 + mip ≈ 171 MiB
// 与 RTX Remix 的口径一致（`rtx_texture.h:217` 用 BC7_UNORM，不是 _SRGB）。
//
// 编码器：bc7enc_rdo（Richard Geldreich，MIT / public domain，
// cpp/external/bc7enc/）。它的行为正好契合我们两张图集：
//   · 块内 alpha 全 255 ⇒ 用 mode 1（无 alpha 段，解码 alpha 精确 255）
//     —— specular 的 `255 = 无自发光` 语义因此**不会被插值破坏**；
//   · 块内有 alpha<255 ⇒ 强制 mode 6（8-bit alpha 端点 + 7-bit index）
//     —— normal 的 POM 高度（a 通道）拿到 BC7 能给的最好精度。
// 参数用 **linear weights**（不用默认的 YCbCr 感知加权）：这是法线/粗糙度数据
// 纹理，不是颜色。
//
// mip 链复用 P2a 的「按 sprite 矩形降采样」：跨 sprite 渗色在压缩前就已经消掉，
// BC7 只是把每级数据换成块编码 —— 两个改动正交，各自可单独回退。
// ============================================================================
bool pbrBc7Enabled()
{
    static const bool cached = []()
    {
        const char* raw = std::getenv("ORION_PBR_BC7");
        return raw == nullptr || raw[0] == '\0' || raw[0] != '0';
    }();

    return cached;
}

// 2026-10-29：二分开关 —— ORION_PBR_BC7_PARTS=normal|specular 只压其中一张。
// 用途：定位"材质变亮"到底来自 normal（法线/AO/POM 高度）还是 specular
// （粗糙度/F0/自发光掩码）。未设或其它值 = 两张都压。
bool pbrBc7PartEnabled(bool isNormalTexture)
{
    static const int partMode = []()
    {
        const char* raw = std::getenv("ORION_PBR_BC7_PARTS");

        if (raw == nullptr || raw[0] == '\0')
        {
            return 0;
        }
        if (std::strcmp(raw, "normal") == 0)
        {
            return 1;
        }

        if (std::strcmp(raw, "specular") == 0)
        {
            return 2;
        }

        return 0;
    }();

    if (partMode == 0)
    {
        return true;
    }

    return partMode == 1 ? isNormalTexture : !isNormalTexture;
}
bool pbrBc7Supported()
{
    static const bool cached = []()
    {
        if (g_physicalDevice == VK_NULL_HANDLE)
        {
            return false;
        }

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(g_physicalDevice, &features);
        return features.textureCompressionBC == VK_TRUE;
    }();

    return cached;
}
void pbrBc7ParamsInit(bc7enc_compress_block_params& params)
{
    bc7enc_compress_block_params_init(&params);

    bc7enc_compress_block_params_init_linear_weights(&params);

    params.m_max_partitions = 16u;
    params.m_uber_level = 0u;
    params.m_mode17_partition_estimation_filterbank = true;
}

// 单线程编码 [blockRow0, blockRow1) 这一段块行（边界不足 4 的像素用 clamp 复制）。
void pbrEncodeBc7BlockRows(
    const uint8_t* rgba, uint32_t width, uint32_t height,
    uint32_t blocksX, uint8_t* dst,
    const bc7enc_compress_block_params& params,
    uint32_t blockRow0, uint32_t blockRow1)
{
    uint8_t block[64];

    bc7enc_compress_block_params opaqueParams = params;
    opaqueParams.m_mode_mask = 1u << 1u;

    for (uint32_t by = blockRow0; by < blockRow1; ++by)
    {
        for (uint32_t bx = 0u; bx < blocksX; ++bx)
        {
            for (uint32_t py = 0u; py < 4u; ++py)
            {
                uint32_t sy = by * 4u + py;

                if (sy >= height)
                {
                    sy = height - 1u;
                }

                for (uint32_t px = 0u; px < 4u; ++px)
                {
                    uint32_t sx = bx * 4u + px;

                    if (sx >= width)
                    {
                        sx = width - 1u;
                    }

                    std::memcpy(
                        block + (size_t(py) * 4u + px) * 4u,
                        rgba + (size_t(sy) * width + sx) * 4u,
                        4u);
                }
            }

            bool opaqueAlpha = true;

            for (uint32_t i = 0u; i < 16u; ++i)
            {
                if (block[i * 4u + 3u] != 255u)
                {
                    opaqueAlpha = false;
                    break;
                }
            }

            bc7enc_compress_block(
                dst + (size_t(by) * blocksX + bx) * 16u,
                block,
                opaqueAlpha ? &opaqueParams : &params);
        }
    }
}

// 2026-10-29：把一整级 RGBA8 编码成 BC7 块，**按块行分片并行**。
//
// 实测单线程 16384x8192 全链 57 秒/张（两张 111 秒，渲染线程全程阻塞），
// 而 bc7enc 的查找表只在 bc7enc_compress_block_init() 里填一次、之后只读
// （bc7enc.cpp:109/112 的 g_bc7_mode_1/7_optimal_endpoints），
// 所以并行只受 CPU 核数限制。16 线程预期 3~5 秒/张。
//
// 每级 spawn 一次（mip 链必须逐级串行：第 l 级依赖第 l-1 级的结果），
// 线程创建开销相对每级几十毫秒的编码可忽略。
void pbrEncodeBc7Level(
    const uint8_t* rgba, uint32_t width, uint32_t height,
    uint8_t* dst, const bc7enc_compress_block_params& params)
{
    const uint32_t blocksX = (width + 3u) / 4u;
    const uint32_t blocksY = (height + 3u) / 4u;

    uint32_t threads = std::thread::hardware_concurrency();

    if (threads == 0u)
    {
        threads = 1u;
    }

    threads = std::min<uint32_t>(threads, 16u);
    threads = std::min<uint32_t>(threads, blocksY);

    if (threads <= 1u)
    {
        pbrEncodeBc7BlockRows(
            rgba, width, height, blocksX, dst, params, 0u, blocksY);
        return;
    }

    const uint32_t rowsPerThread = (blocksY + threads - 1u) / threads;

    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (uint32_t t = 0u; t < threads; ++t)
    {
        const uint32_t row0 = t * rowsPerThread;
        const uint32_t row1 = std::min(blocksY, row0 + rowsPerThread);

        if (row0 >= row1)
        {
            break;
        }

        workers.emplace_back(
            [rgba, width, height, blocksX, dst, &params, row0, row1]()
            {
                // 2026-10-31：BC7 编码是渲染线程上的阻塞工作，分片线程与它并行
                // 时若同优先级抢核，最慢分片反而被拖长（整体耗时由最慢分片决定）。
                SetThreadPriority(
                    GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

                pbrEncodeBc7BlockRows(
                    rgba, width, height, blocksX, dst, params, row0, row1);
            });
    }

    for (std::thread& worker : workers)
    {
        worker.join();
    }
}

// 按 sprite 生成 mip 链 + 逐级 BC7 编码 + 上传。失败返回空 view，由调用方回退。
void uploadPbrBc7SpriteMipped(
    void* pixels, uint32_t width, uint32_t height,
    VkImage& image, VkDeviceMemory& memory, VkImageView& view)
{
    auto fail = [&](const char* why)
    {
        RTLog(std::string("[PBR BC7] ") + why);
        if (view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(g_device, view, nullptr);
            view = VK_NULL_HANDLE;
        }
        if (memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, memory, nullptr);
            memory = VK_NULL_HANDLE;
        }
        if (image != VK_NULL_HANDLE)
        {
            vkDestroyImage(g_device, image, nullptr);
            image = VK_NULL_HANDLE;
        }
    };

    if (width < 4u || height < 4u || g_pbrSpriteRects.empty())
    {
        fail("degenerate input");
        return;
    }

    static const bool bc7EncoderReady = []()
    {
        bc7enc_compress_block_init();
        return true;
    }();
    (void)bc7EncoderReady;

    bc7enc_compress_block_params params;
    pbrBc7ParamsInit(params);

    {
        const uint8_t* probe = static_cast<const uint8_t*>(pixels);
        const size_t pixelCount = size_t(width) * size_t(height);
        size_t exact255 = 0u;

        for (size_t i = 0u; i < pixelCount; ++i)
        {
            if (probe[i * 4u + 3u] == 255u)
            {
                ++exact255;
            }
        }

        RTLog("[PBR BC7] src alpha: ==255 " + std::to_string(exact255) +
              " / " + std::to_string(pixelCount) +
              " (!=255 " + std::to_string(pixelCount - exact255) + ")");
    }

    uint32_t mipLevels = 1u;
    for (uint32_t s = std::max(width, height); s > 1u; s >>= 1u)
    {
        ++mipLevels;
    }

    std::vector<VkDeviceSize> offsets(mipLevels, 0u);
    VkDeviceSize totalBytes = 0u;

    for (uint32_t l = 0u; l < mipLevels; ++l)
    {
        offsets[l] = totalBytes;

        const uint32_t lw = std::max(1u, width >> l);
        const uint32_t lh = std::max(1u, height >> l);
        const uint32_t bx = (lw + 3u) / 4u;
        const uint32_t by = (lh + 3u) / 4u;

        totalBytes += VkDeviceSize(bx) * VkDeviceSize(by) * 16u;
    }

    if (g_textureUploadFencePending && g_textureUploadFence != VK_NULL_HANDLE)
    {
        check(
            vkWaitForFences(
                g_device, 1u, &g_textureUploadFence, VK_TRUE,
                std::numeric_limits<uint64_t>::max()),
            "wait previous texture upload fence failed");
        g_textureUploadFencePending = false;
    }

    void* stagingMapped = acquireTextureUploadStaging(totalBytes);
    if (stagingMapped == nullptr)
    {
        fail("staging map failed");
        return;
    }

    uint8_t* stagingBytes = static_cast<uint8_t*>(stagingMapped);

    {
        const auto encodeStart = std::chrono::steady_clock::now();

        const uint8_t* sourceBytes = static_cast<const uint8_t*>(pixels);
        const size_t mip0Bytes = size_t(width) * size_t(height) * 4u;

        std::vector<uint8_t> prev(sourceBytes, sourceBytes + mip0Bytes);
        std::vector<uint8_t> cur;
        std::vector<uint8_t> written;

        pbrEncodeBc7Level(
            prev.data(), width, height, stagingBytes + offsets[0], params);

        for (uint32_t l = 1u; l < mipLevels; ++l)
        {
            const uint32_t lw = std::max(1u, width >> l);
            const uint32_t lh = std::max(1u, height >> l);

            cur.assign(size_t(lw) * size_t(lh) * 4u, 0u);

            pbrDownsampleLevelBySprite(
                prev.data(), std::max(1u, width >> (l - 1u)), std::max(1u, height >> (l - 1u)),
                cur.data(), lw, lh, l, written);

            pbrEncodeBc7Level(cur.data(), lw, lh, stagingBytes + offsets[l], params);
            prev.swap(cur);
        }

        const double encodeMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - encodeStart).count();

        RTLog("[PBR BC7] encoded size=" + std::to_string(width) + "x" + std::to_string(height) +
              " levels=" + std::to_string(mipLevels) +
              " rects=" + std::to_string(g_pbrSpriteRects.size()) +
              " compressedMB=" + std::to_string(double(totalBytes) / 1048576.0) +
              " encodeMs=" + std::to_string(encodeMs));
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1u;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1u;
    imageInfo.format = VK_FORMAT_BC7_UNORM_BLOCK;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(g_device, &imageInfo, nullptr, &image) != VK_SUCCESS)
    {
        image = VK_NULL_HANDLE;
        fail("vkCreateImage(BC7) failed");
        return;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(g_device, image, &memReq);

    uint32_t typeIndex = 0xFFFFFFFFu;
    try
    {
        typeIndex = findMemoryType(
            g_physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    catch (const std::exception& e)
    {
        RTLog(std::string("[PBR BC7] no memory type: ") + e.what());
    }

    if (typeIndex == 0xFFFFFFFFu)
    {
        fail("no DEVICE_LOCAL memory type");
        return;
    }

    VkMemoryAllocateInfo memAlloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memAlloc.allocationSize = memReq.size;
    memAlloc.memoryTypeIndex = typeIndex;

    if (vkAllocateMemory(g_device, &memAlloc, nullptr, &memory) != VK_SUCCESS)
    {
        memory = VK_NULL_HANDLE;
        fail("vkAllocateMemory failed");
        return;
    }

    if (vkBindImageMemory(g_device, image, memory, 0u) != VK_SUCCESS)
    {
        fail("vkBindImageMemory failed");
        return;
    }

    if (g_textureUploadCommandBuffer == VK_NULL_HANDLE)
    {
        VkCommandBufferAllocateInfo cbAlloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbAlloc.commandPool = g_rtCommandPool;
        cbAlloc.commandBufferCount = 1u;

        if (vkAllocateCommandBuffers(g_device, &cbAlloc, &g_textureUploadCommandBuffer) != VK_SUCCESS)
        {
            g_textureUploadCommandBuffer = VK_NULL_HANDLE;
            fail("vkAllocateCommandBuffers failed");
            return;
        }
    }

    VkCommandBuffer cmd = g_textureUploadCommandBuffer;
    vkResetCommandBuffer(cmd, 0u);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image = image;
    toDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toDst.subresourceRange.baseMipLevel = 0u;
    toDst.subresourceRange.levelCount = mipLevels;
    toDst.subresourceRange.baseArrayLayer = 0u;
    toDst.subresourceRange.layerCount = 1u;
    toDst.srcAccessMask = 0u;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &toDst);

    std::vector<VkBufferImageCopy> regions(mipLevels);
    for (uint32_t l = 0u; l < mipLevels; ++l)
    {
        regions[l] = VkBufferImageCopy{};
        regions[l].bufferOffset = offsets[l];
        regions[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[l].imageSubresource.mipLevel = l;
        regions[l].imageSubresource.baseArrayLayer = 0u;
        regions[l].imageSubresource.layerCount = 1u;
        regions[l].imageExtent.width = std::max(1u, width >> l);
        regions[l].imageExtent.height = std::max(1u, height >> l);
        regions[l].imageExtent.depth = 1u;
    }

    vkCmdCopyBufferToImage(
        cmd,
        g_textureUploadStaging,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        mipLevels,
        regions.data());

    VkImageMemoryBarrier toRead = toDst;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &toRead);

    vkEndCommandBuffer(cmd);

    if (g_textureUploadFence == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};

        if (vkCreateFence(g_device, &fenceInfo, nullptr, &g_textureUploadFence) != VK_SUCCESS)
        {
            g_textureUploadFence = VK_NULL_HANDLE;
            fail("vkCreateFence failed");
            return;
        }
    }

    vkResetFences(g_device, 1u, &g_textureUploadFence);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &cmd;

    if (vkQueueSubmit(g_queue, 1u, &submit, g_textureUploadFence) != VK_SUCCESS)
    {
        fail("vkQueueSubmit failed");
        return;
    }

    g_textureUploadFencePending = true;

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_BC7_UNORM_BLOCK;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0u;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0u;
    viewInfo.subresourceRange.layerCount = 1u;

    if (vkCreateImageView(g_device, &viewInfo, nullptr, &view) != VK_SUCCESS)
    {
        view = VK_NULL_HANDLE;
        fail("vkCreateImageView(BC7) failed");
        return;
    }

    RTLog("[PBR BC7] atlas uploaded as BC7_UNORM (mip chain, sprite-aligned)");
}
void uploadTextureToVRAM(void* pixels, uint32_t width, uint32_t height, VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    if (pixels == nullptr || width == 0u || height == 0u) {
        RTLog("[PBR VRAM] invalid pixel payload, upload skipped");
        return;
    }

    if (g_device == VK_NULL_HANDLE ||
        g_physicalDevice == VK_NULL_HANDLE ||
        g_queue == VK_NULL_HANDLE ||
        g_rtCommandPool == VK_NULL_HANDLE) {
        RTLog("[PBR VRAM] device/queue/pool unavailable, upload skipped");
        return;
    }

    if (pbrSpriteMipEnabled() && !g_pbrSpriteRects.empty())
    {
        if (pbrBc7Enabled() && g_pbrBc7AllowForNextUpload && pbrBc7Supported())
        {
            uploadPbrBc7SpriteMipped(pixels, width, height, image, memory, view);

            if (view != VK_NULL_HANDLE)
            {
                return;
            }

            RTLog("[PBR BC7] fell back to RGBA8 sprite-mip path");
        }

        uploadPbrSpriteMippedToVRAM(pixels, width, height, image, memory, view);

        if (view != VK_NULL_HANDLE)
        {
            return;
        }

        RTLog("[PBR SPRITE MIP] fell back to whole-atlas blit path");
    }

    auto abortUpload = [&](const std::string& why) {
        RTLog("[PBR VRAM] " + why);
        if (view != VK_NULL_HANDLE)   { vkDestroyImageView(g_device, view, nullptr); view = VK_NULL_HANDLE; }
        if (memory != VK_NULL_HANDLE) { vkFreeMemory(g_device, memory, nullptr);     memory = VK_NULL_HANDLE; }
        if (image != VK_NULL_HANDLE)  { vkDestroyImage(g_device, image, nullptr);    image = VK_NULL_HANDLE; }
    };

    VkDeviceSize imageSize = static_cast<VkDeviceSize>(width) * height * 4;

    if(g_textureUploadFencePending &&
       g_textureUploadFence != VK_NULL_HANDLE)
    {
        PERF_L10_TIME_STALL(
            "textureUploadQueueIdle",
            check(
                vkWaitForFences(
                    g_device,
                    1u,
                    &g_textureUploadFence,
                    VK_TRUE,
                    std::numeric_limits<uint64_t>::max()
                ),
                "wait previous texture upload fence failed"
            ));

        g_textureUploadFencePending = false;
    }

    void* stagingMapped =
        acquireTextureUploadStaging(imageSize);

    if(stagingMapped == nullptr)
    {
        RTLog("[PBR VRAM] staging map failed, upload skipped");
        return;
    }

    std::memcpy(stagingMapped, pixels, static_cast<size_t>(imageSize));

    VkBuffer stagingBuffer = g_textureUploadStaging;

    uint32_t mipLevels = 1u;
    for(uint32_t mipSize = std::max(width, height); mipSize > 1u; mipSize >>= 1u)
    {
        ++mipLevels;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateImage(g_device, &imageInfo, nullptr, &image);
    if (result != VK_SUCCESS) {
        image = VK_NULL_HANDLE;
        abortUpload("vkCreateImage failed VkResult=" + std::to_string(static_cast<int>(result)));
        return;
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(g_device, image, &memRequirements);

    uint32_t memoryTypeIndex = 0xFFFFFFFFu;
    try {
        memoryTypeIndex = findMemoryType(g_physicalDevice, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } catch (const std::exception& e) {
        RTLog(std::string("[PBR VRAM] no DEVICE_LOCAL memory type: ") + e.what());
    }
    if (memoryTypeIndex == 0xFFFFFFFFu) {
        abortUpload("no DEVICE_LOCAL memory type, upload skipped");
        return;
    }

    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;

    result = vkAllocateMemory(g_device, &allocInfo, nullptr, &memory);
    if (result != VK_SUCCESS) {
        memory = VK_NULL_HANDLE;
        abortUpload("vkAllocateMemory failed VkResult=" + std::to_string(static_cast<int>(result)));
        return;
    }

    result = vkBindImageMemory(g_device, image, memory, 0);
    if (result != VK_SUCCESS) {
        abortUpload("vkBindImageMemory failed VkResult=" + std::to_string(static_cast<int>(result)));
        return;
    }

    if(g_textureUploadCommandBuffer == VK_NULL_HANDLE)
    {
        VkCommandBufferAllocateInfo allocInfoCmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfoCmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfoCmd.commandPool = g_rtCommandPool;
        allocInfoCmd.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(g_device, &allocInfoCmd, &g_textureUploadCommandBuffer);
        if (result != VK_SUCCESS) {
            g_textureUploadCommandBuffer = VK_NULL_HANDLE;
            abortUpload("vkAllocateCommandBuffers failed VkResult=" + std::to_string(static_cast<int>(result)));
            return;
        }
    }

    VkCommandBuffer commandBuffer = g_textureUploadCommandBuffer;
    vkResetCommandBuffer(commandBuffer, 0);

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    if(mipLevels > 1u)
    {
        VkImageMemoryBarrier mip0Read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        mip0Read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mip0Read.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        mip0Read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mip0Read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mip0Read.image = image;
        mip0Read.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        mip0Read.subresourceRange.baseMipLevel = 0;
        mip0Read.subresourceRange.levelCount = 1;
        mip0Read.subresourceRange.baseArrayLayer = 0;
        mip0Read.subresourceRange.layerCount = 1;
        mip0Read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mip0Read.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &mip0Read
        );

        for(uint32_t level = 1u; level < mipLevels; ++level)
        {
            const uint32_t srcWidth = std::max(1u, width >> (level - 1u));
            const uint32_t srcHeight = std::max(1u, height >> (level - 1u));
            const uint32_t dstWidth = std::max(1u, width >> level);
            const uint32_t dstHeight = std::max(1u, height >> level);

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = level - 1u;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {
                static_cast<int32_t>(srcWidth),
                static_cast<int32_t>(srcHeight),
                1
            };
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = level;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {
                static_cast<int32_t>(dstWidth),
                static_cast<int32_t>(dstHeight),
                1
            };

            vkCmdBlitImage(
                commandBuffer,
                image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit,
                VK_FILTER_LINEAR
            );

            VkImageMemoryBarrier midMip{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            midMip.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            midMip.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            midMip.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            midMip.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            midMip.image = image;
            midMip.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            midMip.subresourceRange.baseMipLevel = level;
            midMip.subresourceRange.levelCount = 1;
            midMip.subresourceRange.baseArrayLayer = 0;
            midMip.subresourceRange.layerCount = 1;
            midMip.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            midMip.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &midMip
            );
        }
    }

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(commandBuffer);

    if(g_textureUploadFence == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        result = vkCreateFence(g_device, &fenceInfo, nullptr, &g_textureUploadFence);
        if (result != VK_SUCCESS) {
            g_textureUploadFence = VK_NULL_HANDLE;
            abortUpload("vkCreateFence failed VkResult=" + std::to_string(static_cast<int>(result)));
            return;
        }
    }

    vkResetFences(g_device, 1u, &g_textureUploadFence);

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    result = vkQueueSubmit(g_queue, 1, &submitInfo, g_textureUploadFence);
    if (result != VK_SUCCESS) {
        abortUpload("vkQueueSubmit failed VkResult=" + std::to_string(static_cast<int>(result)));
        return;
    }
    g_textureUploadFencePending = true;


    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(g_device, &viewInfo, nullptr, &view);
    if (result != VK_SUCCESS) {
        view = VK_NULL_HANDLE;
        abortUpload("vkCreateImageView failed VkResult=" + std::to_string(static_cast<int>(result)));
        return;
    }
}



extern "C"

JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_uploadPBRAtlases(
    JNIEnv* env, jclass clazz, jlong normalPtr, jlong specularPtr, jint width, jint height,
    jintArray spriteRects, jint spriteRectCount)
{
    if (normalPtr == 0 || specularPtr == 0 || width <= 0 || height <= 0) return;

    g_pbrSpriteRects.clear();

    if (spriteRects != nullptr && spriteRectCount > 0)
    {
        const jsize arrayLen = env->GetArrayLength(spriteRects);
        const jsize wanted = jsize(spriteRectCount) * 4;
        const jsize usable = arrayLen < wanted ? arrayLen : wanted;

        if (usable > 0)
        {
            std::vector<jint> raw(static_cast<size_t>(usable), 0);
            env->GetIntArrayRegion(spriteRects, 0, usable, raw.data());

            g_pbrSpriteRects.reserve(static_cast<size_t>(usable / 4));

            for (jsize i = 0; i + 3 < usable; i += 4)
            {
                if (raw[i + 2] <= 0 || raw[i + 3] <= 0)
                {
                    continue;
                }

                PbrSpriteRect rect;
                rect.x = static_cast<uint32_t>(raw[i] < 0 ? 0 : raw[i]);
                rect.y = static_cast<uint32_t>(raw[i + 1] < 0 ? 0 : raw[i + 1]);
                rect.w = static_cast<uint32_t>(raw[i + 2]);
                rect.h = static_cast<uint32_t>(raw[i + 3]);
                g_pbrSpriteRects.push_back(rect);
            }
        }

        env->DeleteLocalRef(spriteRects);
    }

    if (g_device == VK_NULL_HANDLE ||
        g_physicalDevice == VK_NULL_HANDLE ||
        g_rtCommandPool == VK_NULL_HANDLE ||
        g_queue == VK_NULL_HANDLE)
    {
        RTLog("[PBR VRAM] device/queue/pool not ready, upload skipped");
        return;
    }

    try
    {
        FGDeviceWaitIdle(g_device);

        invalidateDynamicTextureBindingsForAtlasReload();

        if (g_normalAtlasView)
        {
            vkDestroyImageView(
                g_device,
                g_normalAtlasView,
                nullptr
            );

            g_normalAtlasView =
                VK_NULL_HANDLE;
        }

        if (g_normalAtlasImage)
        {
            vkDestroyImage(
                g_device,
                g_normalAtlasImage,
                nullptr
            );

            g_normalAtlasImage =
                VK_NULL_HANDLE;
        }

        if (g_normalAtlasMemory)
        {
            vkFreeMemory(
                g_device,
                g_normalAtlasMemory,
                nullptr
            );

            g_normalAtlasMemory =
                VK_NULL_HANDLE;
        }


        if (g_specularAtlasView)
        {
            vkDestroyImageView(
                g_device,
                g_specularAtlasView,
                nullptr
            );

            g_specularAtlasView =
                VK_NULL_HANDLE;
        }

        if (g_specularAtlasImage)
        {
            vkDestroyImage(
                g_device,
                g_specularAtlasImage,
                nullptr
            );

            g_specularAtlasImage =
                VK_NULL_HANDLE;
        }

        if (g_specularAtlasMemory)
        {
            vkFreeMemory(
                g_device,
                g_specularAtlasMemory,
                nullptr
            );

            g_specularAtlasMemory =
                VK_NULL_HANDLE;
        }

        void* normalPixels = reinterpret_cast<void*>(normalPtr);
        void* specularPixels = reinterpret_cast<void*>(specularPtr);
        g_pbrBc7AllowForNextUpload = pbrBc7PartEnabled(true);
        uploadTextureToVRAM(normalPixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), g_normalAtlasImage, g_normalAtlasMemory, g_normalAtlasView);

        g_pbrBc7AllowForNextUpload = pbrBc7PartEnabled(false);
        uploadTextureToVRAM(specularPixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), g_specularAtlasImage, g_specularAtlasMemory, g_specularAtlasView);

        g_pbrAtlasesDirty = true;

        RTLog("[PBR VRAM] CPU -> GPU Native VRAM Copy Success. Pending Descriptor Update.");

        releaseHugeTextureUploadStaging();
    }
    catch (const std::exception& e)
    {
        RTLogFlushed(std::string("[PBR VRAM] uploadPBRAtlases aborted: ") + e.what());
    }
    catch (...)
    {
        RTLogFlushed("[PBR VRAM] uploadPBRAtlases aborted: unknown exception");
    }
}
