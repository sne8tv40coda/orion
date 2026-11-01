// Orion backend — Shadow 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


// ---------------------------------------------------------------
// 太阳阴影图：资源、描述符、管线
//
// 完整动机见 SUN_SHADOW_RESOLUTION 处的注释与 cpp/shaders/sun_shadow.glsl。
// 这里只强调两点工程约束：
//   1. 尺寸恒为定尺（当前 1024²），与窗口无关，所以这套资源不进体积云的重建路径；
//   2. 它需要 ray query，而 ray query 是**独立扩展**（不被
//      VK_KHR_ray_tracing_pipeline 蕴含），设备侧由 Java 的
//      VulkanBackendMixin 启用。这里再查一次物理设备能力，不支持时
//      安静地退回"只有云影"的旧行为，而不是创建一条必然报错的管线。
// ---------------------------------------------------------------
void createSunShadowResources()
{
    if(g_sunShadowPipeline != VK_NULL_HANDLE)
    {
        return;
    }
    if(g_physicalDevice == VK_NULL_HANDLE || g_device == VK_NULL_HANDLE ||
       g_instance == VK_NULL_HANDLE)
    {
        return;
    }

    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2"));
    if(getFeatures2 == nullptr)
    {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if(getFeatures2 != nullptr)
    {
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
        };
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &rayQueryFeatures;
        getFeatures2(g_physicalDevice, &features2);
        if(rayQueryFeatures.rayQuery != VK_TRUE)
        {
            RTLog("[SUNSHADOW] ray query unsupported on this device; "
                "volumetric light/fog keep cloud shadows only");
            return;
        }
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(g_physicalDevice, VK_FORMAT_R32_SFLOAT,
        &formatProperties);
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        RTLog("[SUNSHADOW] R32_SFLOAT cannot be both stored and sampled; disabled");
        return;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;
    imageInfo.extent = { SUN_SHADOW_RESOLUTION, SUN_SHADOW_RESOLUTION, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_sunShadowImage),
        "sun shadow image create failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_sunShadowImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_sunShadowMemory),
        "sun shadow image allocate failed");
    check(vkBindImageMemory(g_device, g_sunShadowImage, g_sunShadowMemory, 0u),
        "sun shadow image bind failed");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_sunShadowImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_sunShadowView),
        "sun shadow image view create failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_sunShadowSampler),
        "sun shadow sampler create failed");

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 3u;
    layoutInfo.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
        &g_sunShadowSetLayout), "sun shadow set layout failed");

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_sunShadowDescriptorPool), "sun shadow descriptor pool failed");

    VkDescriptorSetAllocateInfo setAllocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    setAllocation.descriptorPool = g_sunShadowDescriptorPool;
    setAllocation.descriptorSetCount = 1u;
    setAllocation.pSetLayouts = &g_sunShadowSetLayout;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_sunShadowSet),
        "sun shadow descriptor set allocation failed");

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    VkDescriptorImageInfo shadowStorageInfo{};
    shadowStorageInfo.imageView = g_sunShadowView;
    shadowStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_sunShadowSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_sunShadowSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &shadowStorageInfo, nullptr, nullptr };
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

    if(g_tlas != VK_NULL_HANDLE)
    {
        VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        tlasInfo.accelerationStructureCount = 1u;
        tlasInfo.pAccelerationStructures = &g_tlas;
        VkWriteDescriptorSet tlasWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        tlasWrite.pNext = &tlasInfo;
        tlasWrite.dstSet = g_sunShadowSet;
        tlasWrite.dstBinding = 1u;
        tlasWrite.descriptorCount = 1u;
        tlasWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);
        g_sunShadowTlasHandle = g_tlas;
    }

    const std::vector<uint8_t> code = loadSpv("shaders/rt_sun_shadow.comp.spv");
    VkShaderModule shader = createShaderModule(g_device, code);
    try
    {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_sunShadowSetLayout;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_sunShadowPipelineLayout), "sun shadow pipeline layout failed");
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
        pipelineInfo.layout = g_sunShadowPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &g_sunShadowPipeline), "sun shadow pipeline failed");
    }
    catch(...)
    {
        vkDestroyShaderModule(g_device, shader, nullptr);
        destroySunShadowResources();
        throw;
    }
    vkDestroyShaderModule(g_device, shader, nullptr);
    g_sunShadowLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sunShadowAvailable.store(true);
    RTLog("[SUNSHADOW] 256x256 sun shadow map ready (ray query, 65536 rays/frame)");
}
bool sunShadowForceAlways()
{
    static const bool forced =
        (std::getenv("ORION_SUN_SHADOW_ALWAYS") != nullptr);
    return forced;
}

// 2026-11-01：相位阈值的太阳高度自适应（修"夕阳时阴影抖"）。
//
// 病灶：固定 EPS = 1e-4 对应 8.3 Hz 重建（太阳 1200 s 走完 1.0 相位）。
// 同样的 0.12 s，阴影边缘的位移 ∝ 1 / sin²(太阳高度)：
//   60° → 0.008 格（看不见）；10° → 0.29 格；5° → 0.82 格（h=10 格时）
// 于是夕阳时呈现"一格一跳"的阶梯状抖动，而正午完全正常。
//
// 修法：只在低角度收紧阈值（提高重建频率）。
//   高度 >= 0.35（约 20°）→ 1e-4   （8 Hz，与改动前逐位一致）
//   高度 <= 0.05（约 3.5°）→ 2e-5   （约 42 Hz，位移降到 ~0.16 格）
// 单次重建 1024² × 320 格 ≈ 0.5 ms，42 Hz 摊到每帧约 2.2 ms，
// 且只发生在夕阳那几分钟；正午与夜晚逐位不变。
static float sunShadowPhaseEpsForHeight(float sunHeight)
{
    const float t = std::clamp(
        (sunHeight - 0.05f) / (0.35f - 0.05f), 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return 2.0e-5f + (SUN_SHADOW_PHASE_EPS - 2.0e-5f) * smooth;
}

// 返回 true = 这一帧需要重建，并顺便把"当前签名"提交进去。
bool sunShadowSignatureChanged(
    float texelWorld,
    float& builtPhase,
    uint64_t& builtRevision,
    float builtCenter[3]
)
{
    if(sunShadowForceAlways() || g_cameraUboMapped == nullptr)
    {
        return true;
    }

    const CameraUBO* ubo =
        static_cast<const CameraUBO*>(g_cameraUboMapped);
    const float phase = ubo->historyParamsPadding1;
    const uint64_t revision =
        probeGeometryRevision();

    // 与 sun_shadow.glsl 的 sunShadowLightDirection() 同一个公式（同一份
    // historyParams.w），所以这里的"太阳高度"与生产者的正交基严格一致。
    float phaseEps = SUN_SHADOW_PHASE_EPS;
    {
        const float wrapped = phase - std::floor(phase);
        const float sunHeight = 0.81915204429f *
            std::sin(wrapped * 6.28318530718f);
        phaseEps = sunShadowPhaseEpsForHeight(sunHeight);
    }

    float center[3];
    for(int axis = 0; axis < 3; ++axis)
    {
        const float position = ubo->viewInverse[12 + axis];
        center[axis] = std::floor(position / texelWorld) * texelWorld;
    }

    const bool changed =
        builtRevision != revision ||
        std::fabs(phase - builtPhase) > phaseEps ||
        center[0] != builtCenter[0] ||
        center[1] != builtCenter[1] ||
        center[2] != builtCenter[2];

    if(changed)
    {
        builtRevision = revision;
        builtPhase = phase;
        builtCenter[0] = center[0];
        builtCenter[1] = center[1];
        builtCenter[2] = center[2];
    }

    return changed;
}
void recordSunShadowMap(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE || !g_sunShadowAvailable.load() ||
       g_sunShadowPipeline == VK_NULL_HANDLE || g_sunShadowSet == VK_NULL_HANDLE ||
       g_sunShadowImage == VK_NULL_HANDLE || g_tlas == VK_NULL_HANDLE ||
       g_sunShadowTlasHandle != g_tlas)
    {
        return;
    }

    if(!sunShadowSignatureChanged(
        SUN_SHADOW_NEAR_TEXEL_WORLD,
        g_sunShadowBuiltPhase,
        g_sunShadowBuiltRevision,
        g_sunShadowBuiltCenter))
    {
        return;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = g_sunShadowLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = g_sunShadowLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.image = g_sunShadowImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(cmd,
        g_sunShadowLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
    g_sunShadowLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_sunShadowPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_sunShadowPipelineLayout, 0u, 1u, &g_sunShadowSet, 0u, nullptr);
    const uint32_t groups =
        (SUN_SHADOW_RESOLUTION + SUN_SHADOW_WORKGROUP_SIZE - 1u) /
        SUN_SHADOW_WORKGROUP_SIZE;
    vkCmdDispatch(cmd, groups, groups, 1u);

    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
}
void destroySunShadowResources()
{
    if(g_sunShadowPipeline) vkDestroyPipeline(g_device, g_sunShadowPipeline, nullptr);
    if(g_sunShadowPipelineLayout) vkDestroyPipelineLayout(g_device, g_sunShadowPipelineLayout, nullptr);
    if(g_sunShadowDescriptorPool) vkDestroyDescriptorPool(g_device, g_sunShadowDescriptorPool, nullptr);
    if(g_sunShadowSetLayout) vkDestroyDescriptorSetLayout(g_device, g_sunShadowSetLayout, nullptr);
    if(g_sunShadowSampler) vkDestroySampler(g_device, g_sunShadowSampler, nullptr);
    if(g_sunShadowView) vkDestroyImageView(g_device, g_sunShadowView, nullptr);
    if(g_sunShadowImage) vkDestroyImage(g_device, g_sunShadowImage, nullptr);
    if(g_sunShadowMemory) vkFreeMemory(g_device, g_sunShadowMemory, nullptr);
    g_sunShadowPipeline = VK_NULL_HANDLE;
    g_sunShadowPipelineLayout = VK_NULL_HANDLE;
    g_sunShadowDescriptorPool = VK_NULL_HANDLE;
    g_sunShadowSet = VK_NULL_HANDLE;
    g_sunShadowSetLayout = VK_NULL_HANDLE;
    g_sunShadowSampler = VK_NULL_HANDLE;
    g_sunShadowView = VK_NULL_HANDLE;
    g_sunShadowImage = VK_NULL_HANDLE;
    g_sunShadowMemory = VK_NULL_HANDLE;
    g_sunShadowLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sunShadowTlasHandle = VK_NULL_HANDLE;
    g_sunShadowAvailable.store(false);
}

// ---------------------------------------------------------------
// 远场太阳阴影图：资源、描述符、管线（2026-10-01）
//
// 与上面近场那张逐条同构，只有三处不同：
//   1. 尺寸 1024²、extent 由 CameraUBO 的 sunShadowFarParams 下发（生产者从 UBO
//      读，与 rgen 消费者同源），不是编译期常量；
//   2. 消费者是 rgen（远场分支），所以在 RT 派发**之前**记录 —— 近场那张是给
//      体积 pass 的，可以留在原位；
//   3. 后置屏障的 dstStage 必须含 RAY_TRACING_SHADER_BIT_KHR。
//
// 资源照建（描述符必须始终有效），关掉时只是不派发、且 UBO 的 w 置 0，
// rgen 远场分支全短路。开关：
//   ORION_SUNSHADOW_FAR=0  → 关
//   ORION_SUNSHADOW_FAR=1  → 强制开（2026-10-24 之前的默认行为）
//   未设                   → **自适应：仅在 DH 远场合成块开着时派发**
//
// 2026-10-24 为什么要自适应默认关：这张图的消费者是 rgen 的远场分支，而远场
// 遮挡体只可能来自 DH 的 LOD 实例。没有 DH 时，视距以外的几何根本不在 TLAS 里
// （距离剔除 120 格 + 视距 128 格），1024² = 1,048,576 根射线全部 miss ——
// 纯开销。判据用 g_dhFarFieldMode > 0.5，与实例 mask 特判（:68722）同一口径。
//
// 注意：不能退回"static 只读一次"，因为 DH 合成块可以在运行时开关；按帧求值
// 只是两次读 + 一次比较，且派发/UBO 两条路径本来就每帧问一次。
bool sunShadowFarEnabled()
{
    static const int s_override = []() -> int
    {
        const char* raw = std::getenv("ORION_SUNSHADOW_FAR");

        if(raw == nullptr)
        {
            return -1;
        }

        return (std::string(raw) != "0") ? 1 : 0;
    }();

    if(sunShadowFarAbEnabled())
    {
        return sunShadowFarAbArm() != 0u;
    }

    const bool enabled =
        (s_override >= 0) ? (s_override != 0) : (g_dhFarFieldMode > 0.5f);

    static std::atomic<int> s_lastLoggedState{-1};

    const int state = enabled ? 1 : 0;

    if(s_lastLoggedState.exchange(state, std::memory_order_relaxed) != state)
    {
        RTLog(
            std::string("[SUNSHADOW-FAR] dispatch=") + (enabled ? "on" : "off") +
            " dhFarFieldMode=" + std::to_string(g_dhFarFieldMode) +
            (s_override >= 0 ? " (env override)" : " (auto: needs DH composite)")
        );
    }

    return enabled;
}

// 诊断旋钮（2026-10-01，B1）：本地光聚合缓冲（bindings 17/18）的每帧重建是否执行。
//
// 动机：recordFrameCommand 里那句注释断言 "The light list is not consumed yet
// ... so this changes no pixels"，但 shader 侧 localLightBuffer / localLightGridBuffer
// 的读取点分布在好几簇函数里，静态断定需要一条条追调用图。这里给一个运行期开关，
// 让"跳过重建"这条臂能被实测比对，而不是只靠读代码。
//
// 静态追查的结论（供比对）：所有读取点都不可达 ——
//   · evaluatePrimaryLocalLights(8773) / evaluateSecondaryDiffuseLocalLights(9697)
//     / evaluateLocalEmitterRadianceAtHit(6384) / evaluateLocalEmitterSurfaceRadiance(6546)
//     全文件无调用点；
//   · g2b* 引导簇的入口被 !LL_P3_ENABLE_G2B_LOCAL_GUIDE 短路，而该常量 = false，
//     编译期恒真 ⇒ 引导分支整段被 DCE；
//   · rt_test.rchit / rt_test_translucent.rchit 只在 binding 17 上声明，从不读取。
// 也就是说这个开关预期"开了等于没开"。若实测有像素差 ⇒ 上面哪条追错了。
//
// ORION_LOCAL_LIGHT_REBUILD=0 ⇒ 跳过每帧刷新（初始化那一次仍在 setup 里执行，
// binding 不会悬空）。只读一次，改完必须重启客户端。
// 未设或非 "0" = 照旧重建（默认）。
bool localLightRebuildEnabled()
{
    static const bool enabled = []() {
        const char* raw = std::getenv("ORION_LOCAL_LIGHT_REBUILD");
        if(raw == nullptr)
        {
            return false;
        }
        return std::string(raw) != "0";
    }();
    return enabled;
}

// 盒半宽（格）。默认 SUN_SHADOW_FAR_EXTENT_XY = 384，1024 分辨率下 0.75 格/纹素
// （与近场同密度）。环境变量 ORION_SUNSHADOW_FAR_EXTENT 可在**不重编**的情况下
// 调宽/调窄：调宽 = 覆盖更远的山体投影，代价是纹素变粗（阴影细节下降），射线也
// 更长。extent 走 CameraUBO，生产者与消费者同时跟着变，不会对不上。
float sunShadowFarExtent()
{
    static const float extent = []() {
        const char* raw = std::getenv("ORION_SUNSHADOW_FAR_EXTENT");
        if(raw == nullptr)
        {
            return SUN_SHADOW_FAR_EXTENT_XY;
        }
        const float parsed = static_cast<float>(std::atof(raw));
        return parsed >= 64.0f ? parsed : SUN_SHADOW_FAR_EXTENT_XY;
    }();
    return extent;
}
void createSunShadowFarResources()
{
    if(g_sunShadowFarPipeline != VK_NULL_HANDLE)
    {
        return;
    }
    if(g_physicalDevice == VK_NULL_HANDLE || g_device == VK_NULL_HANDLE ||
       g_instance == VK_NULL_HANDLE)
    {
        return;
    }

    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2"));
    if(getFeatures2 == nullptr)
    {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if(getFeatures2 != nullptr)
    {
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
        };
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &rayQueryFeatures;
        getFeatures2(g_physicalDevice, &features2);
        if(rayQueryFeatures.rayQuery != VK_TRUE)
        {
            RTLog("[SUNSHADOW-FAR] ray query unsupported; far shadow map disabled");
            return;
        }
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(g_physicalDevice, VK_FORMAT_R32_SFLOAT,
        &formatProperties);
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        RTLog("[SUNSHADOW-FAR] R32_SFLOAT cannot be both stored and sampled; disabled");
        return;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;
    imageInfo.extent = { SUN_SHADOW_FAR_RESOLUTION, SUN_SHADOW_FAR_RESOLUTION, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_sunShadowFarImage),
        "sun shadow far image create failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_sunShadowFarImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_sunShadowFarMemory),
        "sun shadow far image allocate failed");
    check(vkBindImageMemory(g_device, g_sunShadowFarImage, g_sunShadowFarMemory, 0u),
        "sun shadow far image bind failed");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_sunShadowFarImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_sunShadowFarView),
        "sun shadow far image view create failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_sunShadowFarSampler),
        "sun shadow far sampler create failed");

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 3u;
    layoutInfo.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
        &g_sunShadowFarSetLayout), "sun shadow far set layout failed");

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_sunShadowFarDescriptorPool), "sun shadow far descriptor pool failed");

    VkDescriptorSetAllocateInfo setAllocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    setAllocation.descriptorPool = g_sunShadowFarDescriptorPool;
    setAllocation.descriptorSetCount = 1u;
    setAllocation.pSetLayouts = &g_sunShadowFarSetLayout;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_sunShadowFarSet),
        "sun shadow far descriptor set allocation failed");

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    VkDescriptorImageInfo shadowStorageInfo{};
    shadowStorageInfo.imageView = g_sunShadowFarView;
    shadowStorageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_sunShadowFarSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_sunShadowFarSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &shadowStorageInfo, nullptr, nullptr };
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

    if(g_tlas != VK_NULL_HANDLE)
    {
        VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        tlasInfo.accelerationStructureCount = 1u;
        tlasInfo.pAccelerationStructures = &g_tlas;
        VkWriteDescriptorSet tlasWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        tlasWrite.pNext = &tlasInfo;
        tlasWrite.dstSet = g_sunShadowFarSet;
        tlasWrite.dstBinding = 1u;
        tlasWrite.descriptorCount = 1u;
        tlasWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);
        g_sunShadowFarTlasHandle = g_tlas;
    }

    const std::vector<uint8_t> code = loadSpv("shaders/rt_sun_shadow_far.comp.spv");
    VkShaderModule shader = createShaderModule(g_device, code);
    try
    {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_sunShadowFarSetLayout;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_sunShadowFarPipelineLayout), "sun shadow far pipeline layout failed");
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
        pipelineInfo.layout = g_sunShadowFarPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &g_sunShadowFarPipeline),
            "sun shadow far pipeline failed");
    }
    catch(...)
    {
        vkDestroyShaderModule(g_device, shader, nullptr);
        destroySunShadowFarResources();
        throw;
    }
    vkDestroyShaderModule(g_device, shader, nullptr);
    g_sunShadowFarLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sunShadowFarAvailable.store(true);
    RTLog("[SUNSHADOW-FAR] 1024x1024 far sun shadow map ready (extent 384, 1048576 rays/frame)");
}

// 每帧一次。必须在 **RT 派发之前**调用 —— rgen 的远场分支采样它。
// 这是与近场图（在体积 pass 之前）唯一的位置差异。
void recordSunShadowFarMap(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE || !g_sunShadowFarAvailable.load() ||
       !sunShadowFarEnabled() ||
       g_sunShadowFarPipeline == VK_NULL_HANDLE || g_sunShadowFarSet == VK_NULL_HANDLE ||
       g_sunShadowFarImage == VK_NULL_HANDLE || g_tlas == VK_NULL_HANDLE ||
       g_sunShadowFarTlasHandle != g_tlas)
    {
        return;
    }

    if(!sunShadowSignatureChanged(
        2.0f * sunShadowFarExtent() /
            static_cast<float>(SUN_SHADOW_FAR_RESOLUTION),
        g_sunShadowFarBuiltPhase,
        g_sunShadowFarBuiltRevision,
        g_sunShadowFarBuiltCenter))
    {
        return;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = g_sunShadowFarLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = g_sunShadowFarLayout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0u : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.image = g_sunShadowFarImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(cmd,
        g_sunShadowFarLayout == VK_IMAGE_LAYOUT_UNDEFINED
            ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
    g_sunShadowFarLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_sunShadowFarPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_sunShadowFarPipelineLayout, 0u, 1u, &g_sunShadowFarSet, 0u, nullptr);
    const uint32_t groups =
        (SUN_SHADOW_FAR_RESOLUTION + SUN_SHADOW_FAR_WORKGROUP_SIZE - 1u) /
        SUN_SHADOW_FAR_WORKGROUP_SIZE;
    vkCmdDispatch(cmd, groups, groups, 1u);

    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
}
void destroySunShadowFarResources()
{
    if(g_sunShadowFarPipeline) vkDestroyPipeline(g_device, g_sunShadowFarPipeline, nullptr);
    if(g_sunShadowFarPipelineLayout) vkDestroyPipelineLayout(g_device, g_sunShadowFarPipelineLayout, nullptr);
    if(g_sunShadowFarDescriptorPool) vkDestroyDescriptorPool(g_device, g_sunShadowFarDescriptorPool, nullptr);
    if(g_sunShadowFarSetLayout) vkDestroyDescriptorSetLayout(g_device, g_sunShadowFarSetLayout, nullptr);
    if(g_sunShadowFarSampler) vkDestroySampler(g_device, g_sunShadowFarSampler, nullptr);
    if(g_sunShadowFarView) vkDestroyImageView(g_device, g_sunShadowFarView, nullptr);
    if(g_sunShadowFarImage) vkDestroyImage(g_device, g_sunShadowFarImage, nullptr);
    if(g_sunShadowFarMemory) vkFreeMemory(g_device, g_sunShadowFarMemory, nullptr);
    g_sunShadowFarPipeline = VK_NULL_HANDLE;
    g_sunShadowFarPipelineLayout = VK_NULL_HANDLE;
    g_sunShadowFarDescriptorPool = VK_NULL_HANDLE;
    g_sunShadowFarSet = VK_NULL_HANDLE;
    g_sunShadowFarSetLayout = VK_NULL_HANDLE;
    g_sunShadowFarSampler = VK_NULL_HANDLE;
    g_sunShadowFarView = VK_NULL_HANDLE;
    g_sunShadowFarImage = VK_NULL_HANDLE;
    g_sunShadowFarMemory = VK_NULL_HANDLE;
    g_sunShadowFarLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_sunShadowFarTlasHandle = VK_NULL_HANDLE;
    g_sunShadowFarAvailable.store(false);
}

// ---------------------------------------------------------------
// 地形高度场：资源、描述符、管线
//
// 与太阳阴影图逐条同构（见上面那段注释与 cpp/shaders/terrain_height.glsl）：
// 定尺、不进体积云的重建路径、需要 ray query 时静默降级。唯一多出来
// 的一步是首帧把整张图清成哨兵 —— 理由见 recordTerrainHeightMap。
// ---------------------------------------------------------------
void createTerrainHeightResources()
{
    if(g_terrainHeightPipeline != VK_NULL_HANDLE)
    {
        return;
    }
    if(g_physicalDevice == VK_NULL_HANDLE || g_device == VK_NULL_HANDLE ||
       g_instance == VK_NULL_HANDLE)
    {
        return;
    }

    auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2"));
    if(getFeatures2 == nullptr)
    {
        getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }
    if(getFeatures2 != nullptr)
    {
        VkPhysicalDeviceRayQueryFeaturesKHR rayQueryFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR
        };
        VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features2.pNext = &rayQueryFeatures;
        getFeatures2(g_physicalDevice, &features2);
        if(rayQueryFeatures.rayQuery != VK_TRUE)
        {
            RTLog("[TERRAINHEIGHT] ray query unsupported on this device; "
                "volumetric fog has no ground height field");
            return;
        }
    }

    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(g_physicalDevice, VK_FORMAT_R32_SFLOAT,
        &formatProperties);
    const VkFormatFeatureFlags requiredFeatures =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures)
    {
        RTLog("[TERRAINHEIGHT] R32_SFLOAT cannot be both stored and sampled; disabled");
        return;
    }

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R32_SFLOAT;
    imageInfo.extent = { TERRAIN_HEIGHT_RESOLUTION, TERRAIN_HEIGHT_RESOLUTION, 1u };
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &g_terrainHeightImage),
        "terrain height image create failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, g_terrainHeightImage, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(g_physicalDevice,
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &g_terrainHeightMemory),
        "terrain height image allocate failed");
    check(vkBindImageMemory(g_device, g_terrainHeightImage, g_terrainHeightMemory, 0u),
        "terrain height image bind failed");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = g_terrainHeightImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R32_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &g_terrainHeightView),
        "terrain height image view create failed");

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_terrainHeightSampler),
        "terrain height sampler create failed");

    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    bindings[2] = { 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
        VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 3u;
    layoutInfo.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(g_device, &layoutInfo, nullptr,
        &g_terrainHeightSetLayout), "terrain height set layout failed");

    VkDescriptorPoolSize poolSizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u }
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 3u;
    poolInfo.pPoolSizes = poolSizes;
    check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr,
        &g_terrainHeightDescriptorPool), "terrain height descriptor pool failed");

    VkDescriptorSetAllocateInfo setAllocation{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    setAllocation.descriptorPool = g_terrainHeightDescriptorPool;
    setAllocation.descriptorSetCount = 1u;
    setAllocation.pSetLayouts = &g_terrainHeightSetLayout;
    check(vkAllocateDescriptorSets(g_device, &setAllocation, &g_terrainHeightSet),
        "terrain height descriptor set allocation failed");

    VkDescriptorBufferInfo cameraInfo{};
    cameraInfo.buffer = g_cameraUboBuffer;
    cameraInfo.range = sizeof(CameraUBO);
    VkDescriptorImageInfo storageInfo{};
    storageInfo.imageView = g_terrainHeightView;
    storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2]{};
    writes[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_terrainHeightSet, 0u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cameraInfo, nullptr };
    writes[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
        g_terrainHeightSet, 2u, 0u, 1u,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &storageInfo, nullptr, nullptr };
    vkUpdateDescriptorSets(g_device, 2u, writes, 0u, nullptr);

    if(g_tlas != VK_NULL_HANDLE)
    {
        VkWriteDescriptorSetAccelerationStructureKHR tlasInfo{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR
        };
        tlasInfo.accelerationStructureCount = 1u;
        tlasInfo.pAccelerationStructures = &g_tlas;
        VkWriteDescriptorSet tlasWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        tlasWrite.pNext = &tlasInfo;
        tlasWrite.dstSet = g_terrainHeightSet;
        tlasWrite.dstBinding = 1u;
        tlasWrite.descriptorCount = 1u;
        tlasWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(g_device, 1u, &tlasWrite, 0u, nullptr);
        g_terrainHeightTlasHandle = g_tlas;
    }

    const std::vector<uint8_t> code = loadSpv("shaders/rt_terrain_height.comp.spv");
    VkShaderModule shader = createShaderModule(g_device, code);
    try
    {
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };
        pipelineLayoutInfo.setLayoutCount = 1u;
        pipelineLayoutInfo.pSetLayouts = &g_terrainHeightSetLayout;
        check(vkCreatePipelineLayout(g_device, &pipelineLayoutInfo, nullptr,
            &g_terrainHeightPipelineLayout), "terrain height pipeline layout failed");
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
        pipelineInfo.layout = g_terrainHeightPipelineLayout;
        check(vkCreateComputePipelines(g_device, getSharedPipelineCache(), 1u,
            &pipelineInfo, nullptr, &g_terrainHeightPipeline),
            "terrain height pipeline failed");
    }
    catch(...)
    {
        vkDestroyShaderModule(g_device, shader, nullptr);
        destroyTerrainHeightResources();
        throw;
    }
    vkDestroyShaderModule(g_device, shader, nullptr);
    g_terrainHeightLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_terrainHeightAvailable.store(true);
    RTLog("[TERRAINHEIGHT] 512x512 ground height field ready (ray query, 262144 rays/frame)");
}
void recordTerrainHeightMap(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE || g_terrainHeightImage == VK_NULL_HANDLE)
    {
        return;
    }

    if(g_terrainHeightLayout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        VkImageMemoryBarrier clearBarrier{};
        clearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        clearBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        clearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearBarrier.srcAccessMask = 0u;
        clearBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.image = g_terrainHeightImage;
        clearBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearBarrier.subresourceRange.levelCount = 1u;
        clearBarrier.subresourceRange.layerCount = 1u;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
            &clearBarrier);
        const VkClearColorValue emptyHeight = {{
            TERRAIN_HEIGHT_EMPTY_SENTINEL, 0.0f, 0.0f, 0.0f }};
        vkCmdClearColorImage(cmd, g_terrainHeightImage, VK_IMAGE_LAYOUT_GENERAL,
            &emptyHeight, 1u, &clearBarrier.subresourceRange);
        clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
            &clearBarrier);
        g_terrainHeightLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    if(!g_terrainHeightAvailable.load() ||
       g_terrainHeightPipeline == VK_NULL_HANDLE || g_terrainHeightSet == VK_NULL_HANDLE ||
       g_tlas == VK_NULL_HANDLE || g_terrainHeightTlasHandle != g_tlas)
    {
        return;
    }

    const uint64_t terrainHeightRevision =
        probeGeometryRevision();
    const bool terrainHeightForceAlways =
        std::getenv("ORION_TERRAIN_HEIGHT_ALWAYS") != nullptr;

    constexpr float kTerrainHeightCenterQuantum = 2.0f * 256.0f / 512.0f;

    float terrainHeightCenterX = 0.0f;
    float terrainHeightCenterZ = 0.0f;

    if(g_cameraUboMapped != nullptr)
    {
        const CameraUBO* heightUbo =
            static_cast<const CameraUBO*>(g_cameraUboMapped);

        terrainHeightCenterX =
            std::floor(heightUbo->viewInverse[12] / kTerrainHeightCenterQuantum) *
            kTerrainHeightCenterQuantum;
        terrainHeightCenterZ =
            std::floor(heightUbo->viewInverse[14] / kTerrainHeightCenterQuantum) *
            kTerrainHeightCenterQuantum;
    }

    if(!terrainHeightForceAlways &&
       g_terrainHeightBuilt &&
       g_terrainHeightBuiltRevision == terrainHeightRevision &&
       g_terrainHeightBuiltCenter[0] == terrainHeightCenterX &&
       g_terrainHeightBuiltCenter[1] == terrainHeightCenterZ)
    {
        return;
    }

    g_terrainHeightBuiltCenter[0] = terrainHeightCenterX;
    g_terrainHeightBuiltCenter[1] = terrainHeightCenterZ;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = g_terrainHeightLayout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.image = g_terrainHeightImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1u;
    barrier.subresourceRange.layerCount = 1u;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
    g_terrainHeightLayout = VK_IMAGE_LAYOUT_GENERAL;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_terrainHeightPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_terrainHeightPipelineLayout, 0u, 1u, &g_terrainHeightSet, 0u, nullptr);
    const uint32_t groups =
        (TERRAIN_HEIGHT_RESOLUTION + TERRAIN_HEIGHT_WORKGROUP_SIZE - 1u) /
        TERRAIN_HEIGHT_WORKGROUP_SIZE;
    vkCmdDispatch(cmd, groups, groups, 1u);
    g_terrainHeightBuilt = true;
    g_terrainHeightBuiltRevision = terrainHeightRevision;

    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u,
        &barrier);
}
void destroyTerrainHeightResources()
{
    g_terrainHeightBuilt = false;
    g_terrainHeightBuiltRevision = 0u;
    if(g_terrainHeightPipeline) vkDestroyPipeline(g_device, g_terrainHeightPipeline, nullptr);
    if(g_terrainHeightPipelineLayout) vkDestroyPipelineLayout(g_device, g_terrainHeightPipelineLayout, nullptr);
    if(g_terrainHeightDescriptorPool) vkDestroyDescriptorPool(g_device, g_terrainHeightDescriptorPool, nullptr);
    if(g_terrainHeightSetLayout) vkDestroyDescriptorSetLayout(g_device, g_terrainHeightSetLayout, nullptr);
    if(g_terrainHeightSampler) vkDestroySampler(g_device, g_terrainHeightSampler, nullptr);
    if(g_terrainHeightView) vkDestroyImageView(g_device, g_terrainHeightView, nullptr);
    if(g_terrainHeightImage) vkDestroyImage(g_device, g_terrainHeightImage, nullptr);
    if(g_terrainHeightMemory) vkFreeMemory(g_device, g_terrainHeightMemory, nullptr);
    g_terrainHeightPipeline = VK_NULL_HANDLE;
    g_terrainHeightPipelineLayout = VK_NULL_HANDLE;
    g_terrainHeightDescriptorPool = VK_NULL_HANDLE;
    g_terrainHeightSet = VK_NULL_HANDLE;
    g_terrainHeightSetLayout = VK_NULL_HANDLE;
    g_terrainHeightSampler = VK_NULL_HANDLE;
    g_terrainHeightView = VK_NULL_HANDLE;
    g_terrainHeightImage = VK_NULL_HANDLE;
    g_terrainHeightMemory = VK_NULL_HANDLE;
    g_terrainHeightLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    g_terrainHeightTlasHandle = VK_NULL_HANDLE;
    g_terrainHeightAvailable.store(false);
}
void ensureSunShadowFarDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    const bool farReady = g_sunShadowFarAvailable.load() &&
        g_sunShadowFarView != VK_NULL_HANDLE;

    VkImageView view = farReady ? g_sunShadowFarView : g_atlasImageView;
    VkSampler sampler = farReady ? g_sunShadowFarSampler : g_atlasSampler;

    if(view == VK_NULL_HANDLE)
    {
        g_sunShadowFarPublishedView = VK_NULL_HANDLE;
        g_sunShadowFarPublishedReady = false;
        return;
    }

    if(g_sunShadowFarPublishedView == view &&
       g_sunShadowFarPublishedReady == farReady)
    {
        return;
    }

    FGDeviceWaitIdle(g_device);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = view;
    imageInfo.sampler = sampler;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 58u;
    write.dstArrayElement = 0u;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);

    g_sunShadowFarPublishedView = view;
    g_sunShadowFarPublishedReady = farReady;

    RTLog(std::string("[SUNSHADOW-FAR] 描述符已写 binding58=") +
        (farReady ? "1024² 远场图" : "回落图集（UBO 门 0）"));
}
void ensureSunShadowNearDescriptor()
{
    if(g_rayTracingDescriptorSet == VK_NULL_HANDLE)
    {
        return;
    }

    const bool nearReady = g_sunShadowAvailable.load() &&
        g_sunShadowView != VK_NULL_HANDLE;

    VkImageView view = nearReady ? g_sunShadowView : g_atlasImageView;
    VkSampler sampler = nearReady ? g_sunShadowSampler : g_atlasSampler;

    if(view == VK_NULL_HANDLE)
    {
        g_sunShadowNearPublishedView = VK_NULL_HANDLE;
        g_sunShadowNearPublishedReady = false;
        return;
    }

    if(g_sunShadowNearPublishedView == view &&
       g_sunShadowNearPublishedReady == nearReady)
    {
        return;
    }

    FGDeviceWaitIdle(g_device);

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = view;
    imageInfo.sampler = sampler;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = g_rayTracingDescriptorSet;
    write.dstBinding = 79u;
    write.dstArrayElement = 0u;
    write.descriptorCount = 1u;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(g_device, 1u, &write, 0u, nullptr);

    g_sunShadowNearPublishedView = view;
    g_sunShadowNearPublishedReady = nearReady;

    RTLog(std::string("[SUNSHADOW-NEAR] 描述符已写 binding79=") +
        (nearReady ? "1024² 近场图" : "回落图集"));
}
