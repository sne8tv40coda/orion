#pragma once

// ---------------------------------------------------------------------
// OrionInternal.h —— Orion Vulkan 后端的共享内部头文件。
//
// 2026-11-01 规范化拆分（原单文件 cpp/VulkanCore.cpp，约 7.1 万行）：
//   * 本头文件包含全部共享类型、constexpr 常量、全局变量 extern 声明
//     与函数原型；各模块的变量定义集中在 State.cpp（保持原声明顺序，
//     以复现单编译单元的静态初始化次序）。
//   * vkAllocateMemory 等 15 个 Vulkan 入口的记账劫持宏在 TrackingHooks.h，
//     除 Tracking.cpp（记账实现本身）外每个模块都包含它。
//   * 本头文件仅供 backend/*.cpp 内部使用，不对外安装。
// ---------------------------------------------------------------------

#define VK_ENABLE_BETA_EXTENSIONS
#define VK_USE_PLATFORM_WIN32_KHR

#include <jni.h>
#include <vulkan/vulkan.h>
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <array>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <mutex>
#include <sl.h>
#include <sl_helpers_vk.h>
#include <sl_matrix_helpers.h>
#include <sl_core_types.h>
#include "sl_reflex.h"
#include "sl_pcl.h"
#include <stdexcept>
#include "sl_dlss.h"
#include "sl_dlss_d.h"
#include "sl_dlss_g.h"
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <sstream>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <thread>
#include <condition_variable>
#include <iomanip>

#include "NRD.h"
#ifndef NRD_USE_REAL_MOTION_VECTORS
#define NRD_USE_REAL_MOTION_VECTORS 1
#endif

#ifndef NRD_USE_25D_MOTION_VECTORS
#define NRD_USE_25D_MOTION_VECTORS 1
#endif

#ifndef VK_EXT_opacity_micromap
#define VK_EXT_opacity_micromap 1
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkMicromapEXT)
typedef enum VkMicromapTypeEXT {
    VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT = 0,
} VkMicromapTypeEXT;
typedef enum VkBuildMicromapModeEXT {
    VK_BUILD_MICROMAP_MODE_BUILD_EXT = 0,
} VkBuildMicromapModeEXT;
typedef enum VkOpacityMicromapFormatEXT {
    VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT = 2,
} VkOpacityMicromapFormatEXT;
typedef VkFlags VkBuildMicromapFlagsEXT;
typedef VkFlags VkMicromapCreateFlagsEXT;
typedef struct VkMicromapUsageEXT {
    uint32_t count;
    uint32_t subdivisionLevel;
    uint32_t format;
} VkMicromapUsageEXT;
typedef struct VkMicromapBuildInfoEXT {
    VkStructureType sType;
    const void* pNext;
    VkMicromapTypeEXT type;
    VkBuildMicromapFlagsEXT flags;
    VkBuildMicromapModeEXT mode;
    VkMicromapEXT dstMicromap;
    uint32_t usageCountsCount;
    const VkMicromapUsageEXT* pUsageCounts;
    const VkMicromapUsageEXT* const* ppUsageCounts;
    VkDeviceOrHostAddressConstKHR data;
    VkDeviceOrHostAddressKHR scratchData;
    VkDeviceOrHostAddressConstKHR triangleArray;
    VkDeviceSize triangleArrayStride;
} VkMicromapBuildInfoEXT;
typedef struct VkMicromapCreateInfoEXT {
    VkStructureType sType;
    const void* pNext;
    VkMicromapCreateFlagsEXT createFlags;
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize size;
    VkMicromapTypeEXT type;
    VkDeviceAddress deviceAddress;
} VkMicromapCreateInfoEXT;
typedef struct VkMicromapBuildSizesInfoEXT {
    VkStructureType sType;
    const void* pNext;
    VkDeviceSize micromapSize;
    VkDeviceSize buildScratchSize;
    VkBool32 discardable;
} VkMicromapBuildSizesInfoEXT;
typedef struct VkPhysicalDeviceOpacityMicromapPropertiesEXT {
    VkStructureType sType;
    void* pNext;
    uint32_t maxOpacity2StateSubdivisionLevel;
    uint32_t maxOpacity4StateSubdivisionLevel;
} VkPhysicalDeviceOpacityMicromapPropertiesEXT;
typedef struct VkAccelerationStructureTrianglesOpacityMicromapEXT {
    VkStructureType sType;
    void* pNext;
    VkIndexType indexType;
    VkDeviceOrHostAddressConstKHR indexBuffer;
    VkDeviceSize indexStride;
    uint32_t baseTriangle;
    uint32_t usageCountsCount;
    const VkMicromapUsageEXT* pUsageCounts;
    const VkMicromapUsageEXT* const* ppUsageCounts;
    VkMicromapEXT micromap;
} VkAccelerationStructureTrianglesOpacityMicromapEXT;
typedef struct VkMicromapTriangleEXT {
    uint32_t dataOffset;
    uint16_t subdivisionLevel;
    uint16_t format;
} VkMicromapTriangleEXT;
typedef VkResult (VKAPI_PTR *PFN_vkCreateMicromapEXT)(VkDevice,const VkMicromapCreateInfoEXT*,const VkAllocationCallbacks*,VkMicromapEXT*);
typedef void (VKAPI_PTR *PFN_vkDestroyMicromapEXT)(VkDevice,VkMicromapEXT,const VkAllocationCallbacks*);
typedef void (VKAPI_PTR *PFN_vkCmdBuildMicromapsEXT)(VkCommandBuffer,uint32_t,const VkMicromapBuildInfoEXT*);
typedef void (VKAPI_PTR *PFN_vkGetMicromapBuildSizesEXT)(VkDevice,VkAccelerationStructureBuildTypeKHR,const VkMicromapBuildInfoEXT*,VkMicromapBuildSizesInfoEXT*);
static constexpr VkStructureType VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT = static_cast<VkStructureType>(1000396000);
static constexpr VkStructureType VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_EXT = static_cast<VkStructureType>(1000396006);
static constexpr VkStructureType VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT = static_cast<VkStructureType>(1000396007);
static constexpr VkStructureType VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT = static_cast<VkStructureType>(1000396008);
static constexpr VkStructureType VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT = static_cast<VkStructureType>(1000396009);
static constexpr VkBufferUsageFlags VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT = 0x00800000;
static constexpr VkBufferUsageFlags VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT = 0x01000000;
static constexpr VkBuildMicromapFlagsEXT VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT = 0x00000001;
static constexpr VkPipelineCreateFlags VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT = 0x01000000;
#endif

#ifndef VK_EXT_ray_tracing_invocation_reorder
#define VK_EXT_ray_tracing_invocation_reorder 1
typedef struct VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT {
    VkStructureType sType;
    void* pNext;
    VkBool32 rayTracingInvocationReorder;
} VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT;
typedef enum VkRayTracingInvocationReorderModeEXT {
    VK_RAY_TRACING_INVOCATION_REORDER_MODE_NONE_EXT = 0,
    VK_RAY_TRACING_INVOCATION_REORDER_MODE_REORDER_EXT = 1,
} VkRayTracingInvocationReorderModeEXT;
typedef struct VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT {
    VkStructureType sType;
    void* pNext;
    VkRayTracingInvocationReorderModeEXT rayTracingInvocationReorderReorderingHint;
    uint32_t maxShaderBindingTableRecordIndex;
} VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT;
static constexpr VkStructureType
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT =
        static_cast<VkStructureType>(1000582000);
static constexpr VkStructureType
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_PROPERTIES_EXT =
        static_cast<VkStructureType>(1000582001);
static constexpr const char*
    VK_EXT_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME =
        "VK_EXT_ray_tracing_invocation_reorder";
#endif
#include <sl_core_types.h>

#include <exception>
inline constexpr int RTX_PHASE_RING = 16;
struct VTRTexture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkSampler sampler;
};


inline constexpr uint32_t PERF_L10_ABLATION_OFF = 0xffffffffu;
inline constexpr uint32_t PERF_L10_ABLATION_SWEEP = 0xfffffffeu;
inline constexpr uint32_t PERF_L10_ABLATION_ALT_FLAG = 0x80000000u;
inline constexpr uint32_t PERF_L10_ABLATION_ALT_MAX = 16u;


inline constexpr uint64_t PERF_L10_ABLATION_FRAMES_PER_STAGE = 120u;
inline constexpr uint32_t PERF_L10_ABLATION_STAGE_COUNT = 36u;
inline constexpr bool PERF_L10_ENABLE_SECTION_REBUILD_LOGS = false;
inline constexpr bool PERF_L10_ENABLE_LIGHT_REFRESH_LOGS = false;
inline constexpr uint32_t PERF_L10_GPU_TIMESTAMP_RING_SIZE = 12u;

// Frame pass boundaries, in recording order.  Scope i spans boundary i -> i+1,
// so the scopes tile the frame and their times sum to the frame time.
enum PerfL10GpuBoundary : uint32_t
{
    PERF_L10_GPU_B_FRAME = 0u,
    PERF_L10_GPU_B_VLI,
    PERF_L10_GPU_B_FAR,
    PERF_L10_GPU_B_SETUP,
    PERF_L10_GPU_B_WSIM,
    PERF_L10_GPU_B_WFFT,
    PERF_L10_GPU_B_WCAUS,
    PERF_L10_GPU_B_CAUS,
    PERF_L10_GPU_B_CLOUDPIPE,
    PERF_L10_GPU_B_RTPIPE,
    PERF_L10_GPU_B_SFAR,
    PERF_L10_GPU_B_SHARCUP,
    PERF_L10_GPU_B_SKY,
    PERF_L10_GPU_B_GI,
    PERF_L10_GPU_B_TRACE,
    PERF_L10_GPU_B_RT,
    PERF_L10_GPU_B_SNEAR,
    PERF_L10_GPU_B_HEIGHT,
    PERF_L10_GPU_B_CLOUD,
    PERF_L10_GPU_B_DLSS,
    PERF_L10_GPU_B_SDR,
    PERF_L10_GPU_B_BLIT,
    PERF_L10_GPU_B_FG,
    PERF_L10_GPU_B_TAIL,
    PERF_L10_GPU_BOUNDARY_COUNT
};
inline constexpr uint32_t PERF_L10_GPU_SCOPE_COUNT =
    PERF_L10_GPU_BOUNDARY_COUNT - 1u;
inline constexpr const char* PERF_L10_GPU_SCOPE_NAMES[PERF_L10_GPU_SCOPE_COUNT] =
{
    "vli",
    "far",
    "setup",
    "wsim",
    "wfft",
    "wcaus",
    "caus",
    "cloudpipe",
    "rtpipe",
    "sfar",
    "sharcup",
    "sky",
    "gi",
    "trace",
    "rt",
    "snear",
    "height",
    "cloud",
    "dlss",
    "sdr",
    "blit",
    "fg",
    "tail"
};
struct PerfL10GpuTimestampSlot
{
    bool used = false;
    uint64_t frameSerial = 0u;
    uint32_t renderWidth = 0u;
    uint32_t renderHeight = 0u;
    uint32_t stage = 0u;
    uint32_t serToggle = 0u;
    uint32_t serPaths = 0u;
    uint32_t serMode = 0u;
};

// Sliding-window summary.  Sums and maxima are kept per pass so one busy pass
// cannot hide behind the frame average.  Logged once per window.
struct PerfL10GpuPassWindow
{
    uint64_t samples = 0u;
    uint32_t stage = 0u;
    uint32_t serToggle = 0u;
    uint32_t serPaths = 0u;
    uint32_t serMode = 0u;
    uint32_t abFarArm = 0u;
    float giMaxDistance = 0.0f;
    float pomDepth = 0.0f;
    double sumScopeMs[PERF_L10_GPU_SCOPE_COUNT]{};
    double maxScopeMs[PERF_L10_GPU_SCOPE_COUNT]{};
    double sumFrameMs = 0.0;
    double maxFrameMs = 0.0;
};
inline constexpr uint64_t PERF_L10_GPU_WINDOW_FRAMES = 60u;
inline constexpr uint64_t SECTION_REBUILD_MIN_FRAME_INTERVAL = 6u;
inline constexpr uint64_t SECTION_REBUILD_QUIET_FRAMES = 10u;
inline constexpr uint64_t SECTION_REBUILD_MAX_DEFERRAL_FRAMES = 60u;
inline constexpr size_t SECTION_REBUILD_MAX_SECTIONS_PER_BATCH = 24u;
inline constexpr uint32_t SECTION_REBUILD_FENCE_WAIT_MS = 2u;
inline constexpr double SECTION_REBUILD_TIME_BUDGET_MS = 5.0;
inline constexpr double SECTION_REBUILD_TIME_BUDGET_ADAPTIVE_MAX_MS = 12.0;
inline constexpr double SECTION_REBUILD_TIME_BUDGET_GPU_FRACTION = 0.70;
inline constexpr uint64_t SECTION_REBUILD_CONTINUE_FRAMES = 3u;


inline constexpr bool PERF_L10_ENABLE_SER_AB = false;
inline constexpr uint64_t PERF_L10_SER_AB_WINDOW_FRAMES = 60u;


inline constexpr bool PERF_L10_ENABLE_SER_PATHS_AB = true;


inline constexpr bool PERF_L10_ENABLE_SER_MODE_AB = false;
inline constexpr bool PERF_L10_ENABLE_RTAO_SKIP_AB = false;

// Sliding-window accumulator for the CPU frame interval and the CPU work time
// of the frame-recording function.  Interval is the wall gap between frames;
// work is the time the recording function itself consumed.  interval - work is
// the time spent waiting on the GPU, so the pair decides CPU-bound vs GPU-bound
// without a full-frame GPU timestamp.  Logged once per window.
struct PerfL10CpuFrameWindow
{
    uint64_t samples = 0u;
    double sumMs = 0.0;
    double minMs = 0.0;
    double maxMs = 0.0;
    double sumWorkMs = 0.0;
    double maxWorkMs = 0.0;
};
inline constexpr uint64_t PERF_L10_CPU_FRAME_WINDOW_FRAMES = 60u;


inline constexpr uint32_t PERF_L10_ABLATION_PIN_DISABLED = 0xffffffffu;


inline constexpr bool PERF_L10_ENABLE_STALL_LOG = true;
inline constexpr double PERF_L10_STALL_LOG_THRESHOLD_MS = 3.0;

#define PERF_L10_TIME_STALL(tag, expression)        \
    do {                                            \
        PerfL10StallTimer perfL10StallTimer(tag);   \
        (void)perfL10StallTimer;                    \
        expression;                                 \
    } while(false)


inline constexpr uint64_t PERF_L10_TEXTURE_PUBLISH_MIN_FRAME_INTERVAL = 8u;


inline constexpr bool PERF_L10_ENABLE_CPU_PHASES = true;
enum PerfL10CpuPhase : uint32_t
{
    PERF_L10_CPU_P_PUBLISH = 0u,
    PERF_L10_CPU_P_GEOMETRY,
    PERF_L10_CPU_P_VLI,
    PERF_L10_CPU_P_FAR,
    PERF_L10_CPU_P_SETUP,
    PERF_L10_CPU_P_WATER,
    PERF_L10_CPU_P_RT,
    PERF_L10_CPU_P_CLOUD,
    PERF_L10_CPU_P_DLSS,
    PERF_L10_CPU_P_POST,
    PERF_L10_CPU_PHASE_COUNT
};
inline constexpr const char* PERF_L10_CPU_PHASE_NAMES[PERF_L10_CPU_PHASE_COUNT] =
{
    "publish",
    "geometry",
    "vli",
    "far",
    "setup",
    "water",
    "rt",
    "cloud",
    "dlss",
    "post"
};
struct PerfL10CpuPhaseWindow
{
    uint64_t samples = 0u;
    double sumMs[PERF_L10_CPU_PHASE_COUNT]{};
    double maxMs[PERF_L10_CPU_PHASE_COUNT]{};
    double sumFrameMs = 0.0;
    double maxFrameMs = 0.0;
};
// === 新增：Camera UBO 全局变量与结构体 ===
// 严格遵守 16 字节对齐的 UBO 内存布局
struct CameraUBO {
    float viewInverse[16];
    float projInverse[16];
    uint32_t vertexBufferAddrLo;
    uint32_t vertexBufferAddrHi;
    uint32_t frameCount;
    uint32_t padding;
    float cameraNear;
    float cameraFar;
    float cameraFov;
    float cameraParamsPadding;

    uint32_t historyReadIndex;
    uint32_t historyValid;
    uint32_t historyPadding0;
    uint32_t historyPadding1;

    uint32_t perfAoNearSamples;
    uint32_t perfAoMidSamples;
    uint32_t perfGiBounces;
    uint32_t perfSunShadowSamples;

    uint32_t perfLocalLightRisCandidates;
    uint32_t perfLocalLightAreaSamples;
    uint32_t perfWaterReflectionInterval;
    uint32_t perfGlossyFullDistance;

    float prevViewInverse[16];
    float prevProjInverse[16];

    float historyPrevJitterX;
    float historyPrevJitterY;
    float historyParamsPadding0;
    float historyParamsPadding1;

    float playerX;
    float playerY;
    float playerZ;
    float playerInWater;

    float playerVelocityX;
    float playerVelocityY;
    float playerVelocityZ;
    float playerHorizontalSpeed;

    float waterSurfaceY;
    float waterSimOriginX;
    float waterSimOriginZ;
    float waterSimTexelWorldSize;

    uint32_t waterSimReadLayer;
    uint32_t waterSimEnabled;
    uint32_t waterSimImpulsePending;
    uint32_t cameraInWater;

    float waterSimImpulseX;
    float waterSimImpulseZ;
    float waterSimImpulseStrength;
    float waterSimImpulseSurfaceY;

    float weatherParams[4];

    float cloudParams[40];
    float skyParams[8];
    float godRayParams[12];
    float rainWallParams[8];
    float waterStyleParams[8];

    uint32_t perfSerAb[4];

    float dhFarFieldParams[4];
    float dhFarFieldInvMvp[16];
    float dhFarFieldLight[4];
    float dhFarFieldMvp[16];

    float sunShadowFarParams[4];

    float highCloudParams[20];

    float cloudMsParams[4];

    float cloudMarchParams[4];

    float waterParams[4];

    float cloudPhiParams[4];

    float cloudLightParams[4];

    float cloudShapeParams[4];

    float cloudMoonParams[4];

    float cloudShape2Params[4];

    float cloudAmbientParams[4];

    float cloudMsParams2[4];

    float cloudLightParams2[4];

    float cloudProfileParams[4];

    float cloudDetailParams[4];

    float waterWaveParams[4];

    float waterWaveShapeParams[4];
    float waterSssParams[4];
    float waterFftParams[8];
    float auroraParams[4];

    float rtSampleParams[4];

    uint32_t perfAblationStage[4];

    float galaxyParams[4];

    float fogExtrasParams[4];
    float cloudTemporalParams[4];
    float cloudAerialParams[4];

    uint32_t sharcBuffersLo[4];
    uint32_t sharcBuffersHi[4];

    uint32_t sharcCacheParams[4];

    float cloudSkyHueParams[4];

    float cloudPhysicsParams[4];

    float pomParams[4];

    float sssParams[4];

    float ueCloudA[4];
    float ueCloudB[4];

    float giHalfParams[4];

    float giRayParams[4];

    float hpShapeA[4];
    float hpShapeB[4];
    float hpShapeC[4];
    float hpShapeD[4];
    float hpLightA[4];
    float hpLightB[4];
    float hpMiscA[4];

    float sparseParams[4];

    // ── 2026-11-01 天空散射模型切换（追加在末尾；中间插入会平移全部已读偏移）──
    //   x = 0 = Bruneton 预计算 LUT（出厂），1 = Hillaire 结构（逐像素 march）
    //   y = Hillaire 分支的辐亮度标定倍率（1 = 出厂；两模型量纲不完全一致）
    //   z/w = 保留，写 0
    // 消费方：cpp/shaders/sky_atmosphere.comp（模型选择）
    //         + cpp/shaders/sky_hillaire_common.glsl（标定倍率）。
    float skyModelParams[4];

    // ── 2026-11-02 屏幕空间降级链（SSR + SSPT）──（追加在末尾）
    //   x = SSPT 射程（区块，0 = 关；UI 滑条 0..12）
    //   y = SSR 射程上限（区块，0 = 关；硬性下限 12 ⇒ 只有 >12 区块的
    //       镜面命中才允许被 SSR 接手，判据在 ss_hybrid.comp 里逐像素成立）
    //   z/w = 保留，写 0
    float ssHybridParams[4];
};
static_assert(offsetof(CameraUBO, cloudParams) == 448,
    "cloudParams must stay at offset 448: shaders place it right after weatherParams");
static_assert(offsetof(CameraUBO, skyParams) == 608,
    "skyParams must stay at offset 608: sky_atmosphere.comp and rt_test.rmiss read it there");
static_assert(offsetof(CameraUBO, godRayParams) == 640,
    "godRayParams must stay at offset 640, immediately after skyParams; "
    "volumetric_light.comp declares cloudParams9 -> skyParams0/1 -> godRayParams0/1/2");
static_assert(offsetof(CameraUBO, rainWallParams) == 688,
    "rainWallParams must stay at offset 688, immediately after godRayParams; "
    "rt_test.rgen declares cloudParams9 -> skyParams -> godRayParams0/1/2 -> "
    "rainWallParams0/1, and a block may not skip a field and continue");
static_assert(offsetof(CameraUBO, waterStyleParams) == 720,
    "waterStyleParams must stay at offset 720, immediately after rainWallParams; "
    "the water shaders declare the full tail chain in order");
static_assert(offsetof(CameraUBO, perfSerAb) == 768 - 16,
    "perfSerAb must stay immediately after waterStyleParams at offset 752; "
    "rt_test.rgen declares the tail chain waterStyleParams -> perfSerAb -> "
    "dhFarFieldParams in order");
static_assert(offsetof(CameraUBO, dhFarFieldInvMvp) == 784,
    "dhFarFieldInvMvp must start at 784 (16-byte aligned, right after "
    "dhFarFieldParams, which ends at 784): rt_test.rgen declares mat4 there in "
    "std140 order");
static_assert(offsetof(CameraUBO, dhFarFieldLight) == 848,
    "dhFarFieldLight must stay immediately after the DH inverse MVP; "
    "rt_test.rgen reads mode/strength/gain/variant from it");
static_assert(offsetof(CameraUBO, dhFarFieldMvp) == 864,
    "dhFarFieldMvp must start at 864, immediately after dhFarFieldLight; "
    "rt_test.rgen declares mat4 dhFarFieldMvp there in std140 order");
static_assert(offsetof(CameraUBO, sunShadowFarParams) == 928,
    "sunShadowFarParams must start at 928, immediately after dhFarFieldMvp; "
    "rt_test.rgen and rt_sun_shadow_far.comp declare vec4 there in std140 order");
static_assert(offsetof(CameraUBO, highCloudParams) == 944,
    "highCloudParams must start at 944, immediately after sunShadowFarParams; "
    "volumetric_clouds.comp and rt_test.rmiss declare the full tail chain down to "
    "highCloudParams0..4 in std140 order, and a block may not skip a field");
static_assert(offsetof(CameraUBO, cloudMsParams) == 1024,
    "cloudMsParams must start at 1024, immediately after highCloudParams; "
    "volumetric_clouds.comp declares it right after highCloudParams0..4 in "
    "std140 order (rt_test.rmiss stops early at highCloudParams4, which is fine)");
static_assert(offsetof(CameraUBO, cloudMarchParams) == 1040,
    "cloudMarchParams must start at 1040, immediately after cloudMsParams; "
    "volumetric_clouds.comp declares it right after cloudMsParams in std140 order");
static_assert(offsetof(CameraUBO, waterParams) == 1056,
    "waterParams must start at 1056, immediately after cloudMarchParams; "
    "rt_test.rgen declares the bridge lanes (highCloudParams0..4, cloudMsParams, "
    "cloudMarchParams) and then waterParams in std140 order");
static_assert(offsetof(CameraUBO, cloudPhiParams) == 1072,
    "cloudPhiParams must start at 1072, immediately after waterParams; "
    "volumetric_clouds.comp declares waterParams then cloudPhiParams in std140 "
    "order (it is the only consumer, so this tail append moves nothing else)");
static_assert(offsetof(CameraUBO, cloudLightParams) == 1088,
    "cloudLightParams must start at 1088, immediately after cloudPhiParams; "
    "volumetric_clouds.comp declares cloudPhiParams then cloudLightParams in "
    "std140 order (it is the only consumer)");
static_assert(offsetof(CameraUBO, cloudShapeParams) == 1104,
    "cloudShapeParams must start at 1104, immediately after cloudLightParams; "
    "volumetric_clouds.comp, rt_test.rgen, rt_test.rmiss and volumetric_light.comp "
    "all declare through this block (the shared density module reads it)");
static_assert(offsetof(CameraUBO, cloudMoonParams) == 1120,
    "cloudMoonParams must start at 1120, immediately after cloudShapeParams");
static_assert(offsetof(CameraUBO, cloudShape2Params) == 1136,
    "cloudShape2Params must start at 1136, immediately after cloudMoonParams; "
    "all four density consumers declare through it");
static_assert(offsetof(CameraUBO, cloudAmbientParams) == 1152,
    "cloudAmbientParams must start at 1152, immediately after cloudShape2Params; "
    "volumetric_clouds.comp declares the whole tail chain in std140 order");
static_assert(offsetof(CameraUBO, cloudMsParams2) == 1168,
    "cloudMsParams2 must start at 1168, immediately after cloudAmbientParams");
static_assert(offsetof(CameraUBO, cloudLightParams2) == 1184,
    "cloudLightParams2 must start at 1184, immediately after cloudMsParams2");
static_assert(offsetof(CameraUBO, cloudProfileParams) == 1200,
    "cloudProfileParams must start at 1200, immediately after cloudLightParams2; "
    "all four density consumers declare through it (tyndall_cloud_field reads it)");
static_assert(offsetof(CameraUBO, cloudDetailParams) == 1216,
    "cloudDetailParams must start at 1216, immediately after cloudProfileParams; "
    "all four density consumers declare through it (tyndall_cloud_field reads it)");
static_assert(offsetof(CameraUBO, waterWaveParams) == 1232,
    "waterWaveParams must start at 1232, immediately after cloudDetailParams; "
    "rt_test.rchit declares the whole bridge chain to reach it");
static_assert(offsetof(CameraUBO, waterWaveShapeParams) == 1248,
    "waterWaveShapeParams must start at 1248, immediately after waterWaveParams; "
    "rt_test.rchit and rt_test.rgen both declare the bridge chain to reach it");
static_assert(offsetof(CameraUBO, waterSssParams) == 1264,
    "waterSssParams must start at 1264, immediately after waterWaveShapeParams; "
    "rt_test.rgen declares the whole tail chain in std140 order");
static_assert(offsetof(CameraUBO, waterFftParams) == 1280,
    "waterFftParams must start at 1280, immediately after waterSssParams; "
    "rt_test.rchit and rt_test.rgen declare the whole tail chain in std140 order");
static_assert(offsetof(CameraUBO, auroraParams) == 1312,
    "auroraParams must start at 1312, immediately after waterFftParams; "
    "rt_test.rmiss declares the whole tail chain in std140 order");
static_assert(offsetof(CameraUBO, rtSampleParams) == 1328,
    "rtSampleParams must start at 1328, immediately after auroraParams; "
    "rt_test.rgen declares it as the new tail");
static_assert(offsetof(CameraUBO, perfAblationStage) == 1344,
    "perfAblationStage must start at 1344, immediately after rtSampleParams; "
    "rt_test.rgen is its only consumer and declares uvec4 there as the last "
    "member of the tail chain (std140: a block may not skip a field)");
static_assert(offsetof(CameraUBO, galaxyParams) == 1360,
    "galaxyParams must start at 1360, immediately after perfAblationStage; "
    "rt_test.rmiss is its only consumer and declares the bridge lanes "
    "(rtSampleParams, perfAblationStage) before it in std140 order");
static_assert(offsetof(CameraUBO, fogExtrasParams) == 1376,
    "fogExtrasParams must start at 1376, immediately after galaxyParams; "
    "volumetric_clouds.comp is its only consumer and reads it through the shared "
    "camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, cloudTemporalParams) == 1392,
    "cloudTemporalParams must start at 1392, immediately after fogExtrasParams; "
    "volumetric_clouds.comp and volumetric_cloud_resolve.comp read it through the "
    "shared camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, cloudAerialParams) == 1408,
    "cloudAerialParams must start at 1408, immediately after cloudTemporalParams; "
    "rt_test.rgen and volumetric_clouds.comp read it through the shared "
    "camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, sharcBuffersLo) == 1424,
    "sharcBuffersLo must start at 1424, immediately after cloudAerialParams; "
    "rt_test.rgen declares the SHaRC address lanes there through the shared "
    "camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, sharcBuffersHi) == 1440,
    "sharcBuffersHi must start at 1440, immediately after sharcBuffersLo; "
    "the high 32 bits of the four 64-bit SHaRC cache addresses live here");
static_assert(offsetof(CameraUBO, sharcCacheParams) == 1456,
    "sharcCacheParams must start at 1456, immediately after sharcBuffersHi; "
    "rt_test.rgen reads sceneScale / capacity / radianceScale from it");
static_assert(offsetof(CameraUBO, cloudSkyHueParams) == 1472,
    "cloudSkyHueParams must start at 1472, immediately after sharcCacheParams; "
    "volumetric_clouds.comp reads the sunset sky-hue correction lanes from it "
    "through the shared camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, cloudPhysicsParams) == 1488,
    "cloudPhysicsParams must start at 1488, immediately after cloudSkyHueParams; "
    "volumetric_clouds.comp reads the 2026-10-15 cloud ground-reflection and "
    "visual-fallback lanes through the shared camera_ubo.glsl declaration");
static_assert(offsetof(CameraUBO, pomParams) == 1504,
    "pomParams must start at 1504, immediately after cloudPhysicsParams; "
    "rt_test.rchit reads the POM height-field sampling switch (smooth/sharp) "
    "through the shared camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, sssParams) == 1520,
    "sssParams must start at 1520, immediately after pomParams; "
    "rt_test.rgen reads the foliage subsurface-scattering mode switch "
    "through the shared camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, ueCloudA) == 1536,
    "ueCloudA must start at 1536: camera_ubo.glsl puts it right after "
    "sssParams, and volumetric_clouds_v4.comp reads the eight UE-model lanes "
    "from 1536 / 1552 (a host struct that stops at 1536 makes those reads "
    "out-of-range and they come back as 0 => black clouds)");
static_assert(offsetof(CameraUBO, ueCloudB) == 1552,
    "ueCloudB must start at 1552, immediately after ueCloudA");
static_assert(offsetof(CameraUBO, giHalfParams) == 1568,
    "giHalfParams must start at 1568, immediately after ueCloudB: 2026-10-24 "
    "half-resolution GI switch lives in lane x and rt_test.rgen reads it "
    "through the shared camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, giRayParams) == 1584,
    "giRayParams must start at 1584, immediately after giHalfParams: 2026-10-25 "
    "near-field GI sample-rate selector lives in lane x and rt_test.rgen reads "
    "it through the shared camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, sparseParams) == 1712,
    "sparseParams must start at 1712, immediately after hpMiscA: 2026-10-26 "
    "direct-light mode (RTDI temporal reservoir) lives in lane x and the "
    "sparse-rendering active sampling rate in lane y; rt_test.rgen reads them "
    "through the shared camera_ubo.glsl block declaration");
static_assert(offsetof(CameraUBO, skyModelParams) == 1728,
    "skyModelParams must start at 1728, immediately after sparseParams: "
    "2026-11-01 sky scattering model switch (lane x: 0 = Bruneton LUT, "
    "1 = Hillaire structure) + Hillaire radiance calibration (lane y)");
static_assert(offsetof(CameraUBO, ssHybridParams) == 1744,
    "ssHybridParams must start at 1744, immediately after skyModelParams: "
    "2026-11-02 screen-space degradation chain (lane x = SSPT range in chunks "
    "0..12, lane y = SSR range ceiling in chunks, hard floor 12) is read by "
    "cpp/shaders/ss_hybrid.comp through the shared camera_ubo.glsl block");
static_assert(sizeof(CameraUBO) == 1760,
    "CameraUBO size must match the std140 size of the shaders' CameraProperties "
    "block (1712 -> 1728 when 2026-10-26 sparseParams (direct-light mode + "
    "sparse rendering rate) was appended at the tail; 1600 -> 1712 when "
    "2026-10-25 hpShapeA..hpMiscA (HPVolumeCloud v5 full parameter tail block) "
    "were appended at the tail; 1520 -> 1536 when 2026-10-19 sssParams "
    "(foliage subsurface-scattering mode switch) was appended at the tail; "
    "1504 -> 1520 when 2026-10-18 pomParams "
    "(POM height-field smooth/sharp sampling switch) was appended at the tail; "
    "1488 -> 1504 when cloudPhysicsParams (ground reflection / visual "
    "fallback parameters) was appended at the tail; 1472 -> 1488 when "
    "2026-10-14 cloudSkyHueParams "
    "(sunset sky-hue correction: hue decoupling + daylight energy falloff) was "
    "appended at the tail; 1456 -> 1472 when 2026-10-11 sharcCacheParams (SHaRC "
    "scalar parameters) was appended; 1424 -> 1456 when sharcBuffersLo/Hi (SHaRC "
    "cache device addresses, 2 x uvec4) were appended at the tail; "
    "1296 -> 1312 when waterFftParams grew to 8 floats: 方案 B Seascape; "
    "1312 -> 1328 when 2026-10-06 auroraParams was appended at the tail; "
    "1328 -> 1344 when 2026-10-06 rtSampleParams (user spp) was appended; "
    "1344 -> 1360 when 2026-10-07 perfAblationStage (Perf-L10 RT stage sweep) "
    "was appended; 1360 -> 1376 when 2026-10-08 galaxyParams (End galaxy "
    "size/tilt/yaw sliders) was appended; 1376 -> 1408 when 2026-10-10 "
    "fogExtrasParams + cloudTemporalParams were "
    "appended; 1408 -> 1424 when 2026-10-10 cloudAerialParams (#1 cloud aerial "
    "perspective + #2 ground occlusion attenuation) was "
    "appended)");
inline constexpr uint32_t WATER_SIM_RESOLUTION = 512u;
inline constexpr float WATER_SIM_WORLD_SIZE = 48.0f;
inline constexpr float WATER_SIM_TEXEL_WORLD_SIZE =
    WATER_SIM_WORLD_SIZE / static_cast<float>(WATER_SIM_RESOLUTION);
inline constexpr float WATER_SIM_UPDATE_SECONDS_DEFAULT = 1.0f / 15.0f;
inline constexpr uint32_t WATER_SIM_MAX_ENTITIES = 8u;
inline constexpr uint32_t WATER_SIM_MAX_QUEUED_IMPULSES = 64u;
struct WaterSimEntityUBO
{
    int32_t counts[4];
    float impulses[WATER_SIM_MAX_ENTITIES][4];
    float boats[WATER_SIM_MAX_ENTITIES][4];
    float boatParams[WATER_SIM_MAX_ENTITIES][4];
};
struct WaterSimImpulseEvent { float x, z, strength; };
inline constexpr bool kWATER_CAUSTIC_PASS_ENABLED = false;
inline constexpr bool kWATER_DETAIL_NORMAL_ENABLED = false;
inline constexpr uint32_t WATER_CAUSTIC_RESOLUTION = 256u;
inline constexpr float WATER_CAUSTIC_UPDATE_SECONDS = 1.0f / 30.0f;
inline constexpr float WATER_CAUSTIC_NEAR_WATER_Y_RANGE = 32.0f;
inline constexpr uint32_t WATER_DETAIL_NORMAL_RESOLUTION = 256u;
inline constexpr uint32_t WATER_DETAIL_NORMAL_MIP_LEVELS = 9u;
inline constexpr uint32_t CLOUD_SHAPE_NOISE_RESOLUTION = 64u;
inline constexpr uint32_t CLOUD_CURL_NOISE_RESOLUTION = 64u;
inline constexpr uint32_t CLOUD_BASE_NOISE_RESOLUTION = 64u;
inline constexpr uint32_t CLOUD_V4_SHAPE_SIZE = 128u;
inline constexpr uint32_t CLOUD_V4_BASE_SIZE = 128u;
inline constexpr uint32_t CLOUD_V4_ATLAS_SIZE = 512u;
inline constexpr uint32_t CLOUD_V4_WORLEY_SIZE = 64u;
inline constexpr uint32_t CLOUD_V4_BASE_MIPS = 4u;
inline constexpr uint32_t CLOUD_V4_WORLEY_MIPS = 4u;
inline constexpr uint32_t kCloudAtrousIterations = 4u;
inline constexpr uint32_t VOL_RESTIR_DEPTH = 64u;
inline constexpr uint32_t VOL_FROXEL_DOWNSCALE = 8u;
inline constexpr uint32_t VOL_RESTIR_DOWNSCALE = 4u;
// 2026-11-04：16 -> 32。
//
// 病灶（用户报"ReSTIR 体积光穿墙"）：composite 只有 16 步，却要跨
// VOL_RESTIR_DEPTH = 64 层 froxel ⇒ 每步 4 层。远端每层 0.5 格，于是每步跨度
// 2 格，比一格墙还厚：marchViewZ 虽然被 sceneDistance 截断，但步内的 froxelW
// 采样点已经跨进墙后那一层，把墙后房间的灯算到墙前的雾上。
// 32 步时每步 2 层、远端 1 格，误差落回墙体内部（由墙自遮挡吸收）。
// 成本：god ray 那张图（低于全屏）的 march 次数 ×2。
// 若仍可见穿墙：-DVOL_RESTIR_COMPOSITE_STEPS 提到 64（每步 1 层），成本 ×4。
inline constexpr uint32_t VOL_RESTIR_COMPOSITE_STEPS = 32u;
inline constexpr float VOL_RESTIR_EXTINCTION_SCALE = 1.0f;
inline constexpr uint32_t VOL_RESTIR_MAX_SAMPLES = 2u;
inline constexpr uint32_t VOL_RESTIR_CHAIN_PUSH_BYTES = 32u;

// 2026-11-01：96 -> 32，修体素穿墙。
// 本地光的有效传播距离被 vptLocalLightRange() 封顶在 VPT_LOCAL_MAX_DISTANCE = 30
// 格（local_light_grid_common.glsl），所以 32 格以外的 froxel 辐亮度恒为零 ——
// 保留 96 只是把深度切片做厚：片厚 = maxDistance * 2z / slices^2，指数 2 时远端
// 每片 1.9~3 格，比一格墙还厚 ⇒ 切片中心落在墙后那种 froxel 会看到屋里的灯，
// 被 composite 取到墙前的雾上（典型体素穿墙）。32 时远端每片 1.0 格，误差落在
// 墙体内部、由墙自遮挡吸收。
// 该常量同时决定 composite / integrate / temporal 与玻璃查表的口径，改一处即全局一致。
inline constexpr float VOL_RESTIR_MAX_DISTANCE = 32.0f;
inline constexpr float VOL_RESTIR_DEPTH_EXPONENT = 2.0f;
inline constexpr uint32_t SUN_SHADOW_RESOLUTION = 1024u;
inline constexpr uint32_t SUN_SHADOW_WORKGROUP_SIZE = 8u;
inline constexpr float SUN_SHADOW_EXTENT_XY = 160.0f;
inline constexpr float SUN_SHADOW_EXTENT_Z = 160.0f;
inline constexpr uint32_t SUN_SHADOW_FAR_RESOLUTION = 1024u;
inline constexpr uint32_t SUN_SHADOW_FAR_WORKGROUP_SIZE = 8u;
inline constexpr float SUN_SHADOW_FAR_EXTENT_XY = 384.0f;
inline constexpr float SUN_SHADOW_FAR_EXTENT_Z = 384.0f;
inline constexpr uint32_t TERRAIN_HEIGHT_RESOLUTION = 512u;
inline constexpr uint32_t TERRAIN_HEIGHT_WORKGROUP_SIZE = 8u;
inline constexpr float TERRAIN_HEIGHT_EMPTY_SENTINEL = -1.0e30f;

// 必须与 cpp/shaders/rr_guide_relax.comp 的 RRGuideRelaxPush 逐字节一致。
struct RRGuideRelaxPush
{
    uint32_t extentX;
    uint32_t extentY;
    float relaxScale;
    float minRoughness;
    float normalFalloff;
    float farCloudDistance;
    float padding1;
    float padding2;
};
inline constexpr uint32_t SKY_SCATTER_RESOLUTION_X = 256u;
inline constexpr uint32_t SKY_SCATTER_RESOLUTION_Y = 128u;
struct WaterSimPushConstants
{
    uint32_t sourceLayer;
    uint32_t destinationLayer;
    uint32_t hasImpulse;
    uint32_t playerInWater;
    float deltaSeconds;
    float impulseX;
    float impulseZ;
    float impulseStrength;
    int32_t scrollX;
    int32_t scrollZ;
    uint32_t padding0;
    uint32_t padding1;
    float waveSpongeWidth;
    float waveReservedObstacle;
    float waveGaussianSource;
    float waveDampingScale;
    float waveSpeed;
    float waveShapeScale;
    float waveShapeAngle;
    float waveShapeCrest;
    float waveShapeWarp;
};
inline constexpr uint32_t OMM_MEMORY_NO_CHUNK = 0xFFFFFFFFu;
struct OmmBufferBinding
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memoryOffset = 0u;
    uint32_t slab = OMM_MEMORY_NO_CHUNK;
    uint32_t chunk = 0u;
};
struct RTSectionAS
{
    uint64_t sectionKey = 0;

    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkDeviceAddress vertexAddress = 0;

    VkBuffer blasBuffer = VK_NULL_HANDLE;
    VkDeviceMemory blasMemory = VK_NULL_HANDLE;
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;

    VkDeviceAddress blasAddress = 0;

    uint32_t triangleCount = 0;
    VkDeviceSize vertexBytes = 0;
    VkDeviceSize buildScratchSize = 0;

    VkDeviceSize blasBytes = 0;

    bool refitCapable = false;
    VkDeviceSize updateScratchSize = 0;
    bool pendingRefit = false;

    bool indexedGeometry = false;

    bool opaqueGeometry = false;

    uint32_t vertexStride = 48u;
    bool packedSectionVertices = false;

    VkDeviceSize vertexMotionOffset = 0;
    uint32_t vertexMotionFloats = 0;

    uint32_t sectionMemorySlab = 0;
    uint32_t sectionMemoryChunk = 0;
    VkDeviceSize sectionMemoryChunkBytes = 0;
    bool hasSectionMemoryChunk = false;

    uint32_t vertexAddressSlot = 0;
    bool hasVertexAddressSlot = false;

    VkBuffer ommDataBuffer = VK_NULL_HANDLE;
    OmmBufferBinding ommDataBinding;
    VkDeviceAddress ommDataAddress = 0;
    VkBuffer ommTriangleBuffer = VK_NULL_HANDLE;
    OmmBufferBinding ommTriangleBinding;
    VkDeviceAddress ommTriangleAddress = 0;
    VkBuffer ommStorageBuffer = VK_NULL_HANDLE;
    OmmBufferBinding ommStorageBinding;
    VkBuffer ommScratchBuffer = VK_NULL_HANDLE;
    OmmBufferBinding ommScratchBinding;
    VkDeviceAddress ommScratchAddress = 0;
    VkMicromapEXT opacityMicromap = VK_NULL_HANDLE;
    VkMicromapUsageEXT ommUsage{};
    bool hasOpacityMicromap = false;
};
struct OmmCutoutInput
{
    std::vector<uint8_t> data;
    uint32_t triangleCount = 0;
    uint32_t subdivisionLevel = 0;
    uint32_t knownMicroTriangleCount = 0;
};
struct GPUSectionVertexAddress
{
    uint32_t addressLo = 0;
    uint32_t addressHi = 0;

    uint32_t motionOffset = 0;
    uint32_t vertexStride = 0;
};
static_assert(
    sizeof(GPUSectionVertexAddress) == 16,
    "GPUSectionVertexAddress must remain exactly 16 bytes (4 x uint32)"
);
inline constexpr uint32_t SECTION_VERTEX_ADDRESS_SLOT_LIMIT = 1u << 18;
inline constexpr uint32_t SECTION_ROLE_TERRAIN  = 0u;
inline constexpr uint32_t SECTION_ROLE_VOXY_LOD = 1u;
inline constexpr uint32_t SECTION_ROLE_DH_FAR   = 2u;

// =====================================================
// Per-chunk-column near-field coverage bitmap (binding 20)
// =====================================================
//
// Mirror of the reference pack's VistaCoverage: 8 bits per chunk column,
// packed four columns to a word, LSB-first.
//   bit0 = near-field geometry for this column is loaded
//   bit1..7 reserved (the reference pack's fallback / border bits have no
//           consumer here: this engine never ignores near-field geometry, so
//           the shader mirrors bit0 only -- see cpp/shaders/lod_coverage.glsl)
// A far-field (DH) hit whose column is already covered by near-field geometry
// is the redundant second copy of that geometry and is dropped in the any-hit
// shaders.  Columns outside the published grid read back as 0.
struct LodCoverageHeader
{
    int32_t originChunkX = 0;
    int32_t originChunkZ = 0;
    int32_t sizeX = 0;
    int32_t sizeZ = 0;
};
inline constexpr int32_t LOD_COVERAGE_MAX_SIZE = 256;
inline constexpr uint32_t LOD_COVERAGE_FLAG_NEAR_READY = 1u;

// A Section rebuild is submitted ahead of the frame's RT command buffer on
// the same graphics queue.  Its staging and BLAS scratch allocations must
// remain alive until that queue reaches the submission, but the CPU does not
// need to wait for it in the submitting frame.  They are released at the
// beginning of the next rebuild, after the retirement fence is signaled.
struct DeferredSectionBuildResources
{
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkBuffer blasScratchBuffer = VK_NULL_HANDLE;
    VkDeviceMemory blasScratchMemory = VK_NULL_HANDLE;
    VkDeviceSize blasScratchSize = 0;
    VkBuffer vertexStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexStagingMemory = VK_NULL_HANDLE;
    VkDeviceSize vertexStagingSize = 0;
    struct OmmBuildInputs
    {
        VkBuffer dataBuffer = VK_NULL_HANDLE;
        OmmBufferBinding dataBinding;
        VkBuffer triangleBuffer = VK_NULL_HANDLE;
        OmmBufferBinding triangleBinding;
        VkBuffer scratchBuffer = VK_NULL_HANDLE;
        OmmBufferBinding scratchBinding;
    };
    std::vector<OmmBuildInputs> ommBuildInputs;
};

// =====================================================
// 2026-10-11 Section 重建缓冲池：单槽 → 多槽（治"周期性 20~45ms 顿挫"）
//
// 现场（2026-10-11 高压场景一局的 [PERF L10 ALLOC]，它只打 >=1ms 的分配）：
//   frame=778  ms=19.92 request=3508208 usage=0x1 host
//   frame=5022 ms=45.15 request=2212568 usage=0x1 host
//   frame=5547 ms=28.25 request=3106808 usage=0x1 host
// 三条的 usage=0x1 就是本池的顶点 staging（TRANSFER_SRC + 系统内存）；而同期
// 8~12MB 的分配只要 1.0~1.9ms —— 慢的不是大小，是这块内存**被反复申请/释放**
// 之后驱动主机分配器的状态。与之对应的是新的 [RT GEOMETRY SLOW] 分段日志里
// staging 段占 20.5/45.8/28.9ms（那几帧的 sections=1，几百 KB 的拷贝不可能是这个数）。
//
// 根因是单槽池：取用时把槽取空并置 VK_NULL_HANDLE，而一块缓冲要等退休 fence
// 才能归还；高压场景重建间隔（~30/s）远小于在飞寿命 ⇒ 每次重建都是"池空 →
// 新建"，每次归还又是"销毁上一个 + 停泊这个" ⇒ 稳态约 30 次/秒的 free+alloc
// （2~12MB）。这也是 [RT MEM] 里那条与玩法无关的固定速率私有提交增长的来源。
//
// 现在：每类一个 N 槽池，**只增不减**；取用挑"最小够用"的空闲槽；归还只清
// inFlight。只有"没有任何空闲槽够大"时才新建一块并登记进池 —— 且**绝不销毁
// 正在飞的那块**。稳态下（预热后）分配/释放次数降到 0。
struct RtxSectionBuildBufferSlot
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0u;
    bool inFlight = false;
};

// =====================================================
// Perf-SR.1 - parked Section generation
// =====================================================
// A rebuild replaces the TLAS and rewrites Section BLAS objects, but the
// previous generation is still referenced by whatever the GPU is executing, so
// it cannot be destroyed during the rebuild.  The old code therefore drained
// the RT queue with vkQueueWaitIdle() before touching anything, which
// serialised the CPU-side rebuild behind the GPU.
//
// Instead the replaced generation is parked here while the CPU assembles its
// replacement, and the queue is drained only once that replacement is ready to
// be submitted, just before vkQueueSubmit().  The drain therefore overlaps the
// whole rebuild, which is where the saving comes from; the slot is emptied
// immediately after it.
struct SectionRetirementSlot
{
    bool occupied = false;
    std::vector<RTSectionAS> sections;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    VkBuffer tlasBuffer = VK_NULL_HANDLE;
    VkDeviceMemory tlasMemory = VK_NULL_HANDLE;
    VkBuffer tlasScratchBuffer = VK_NULL_HANDLE;
    VkDeviceMemory tlasScratchMemory = VK_NULL_HANDLE;
    VkBuffer tlasInstanceBuffer = VK_NULL_HANDLE;
    VkDeviceMemory tlasInstanceMemory = VK_NULL_HANDLE;
    VkBuffer tlasInstancesBuffer = VK_NULL_HANDLE;
    VkDeviceMemory tlasInstancesMemory = VK_NULL_HANDLE;
    DeferredSectionBuildResources deferredBuild;

    bool fencePending = false;
};


inline constexpr uint32_t SECTION_RETIREMENT_RING_SIZE = 3u;


inline constexpr bool kNrdLocalDiEnabled = false;

#ifndef NRD_LOCAL_DI_VALIDATION
#define NRD_LOCAL_DI_VALIDATION 1
#endif
inline constexpr float AE_DEFAULT_KEY_EV = -2.4739f;
inline constexpr uint32_t AE_HISTOGRAM_BIN_COUNT = 256u;
inline constexpr float AE_HDR_CLOUD_WHITE_SCALE = 4.0f;
struct AePushConstants
{
    float dtSec;
    float keyEV;
    float minEV;
    float maxEV;
    float adaptUpRate;
    float adaptDownRate;
    float trimLow;
    float trimHigh;
    int32_t enabled;
    float displayWhiteScale;
};

// 必须与 glass_alpha_blend.comp / glass_alpha_finish.comp 的
// push_constant 块**逐字节一致**（64 B）。
struct GlassAlphaBlendPush
{
    uint32_t extentX;
    uint32_t extentY;
    uint32_t neighborIterations;
    uint32_t frameIndex;
    float neighborRadius;
    float neighborExpand;
    float normalThreshold;
    float depthThreshold;
    float coplanarThreshold;
    float shMaxDistance;
    float shDepthExponent;
    float shValid;
    uint32_t neighborSearch;
    uint32_t enableFilter;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t shFroxelW;
    uint32_t shFroxelH;
    uint32_t shFroxelDepth;
    uint32_t dbgMode;
};
static_assert(
    sizeof(GlassAlphaBlendPush) == 80u,
    "GlassAlphaBlendPush: shader push_constant block is 80 B");
inline constexpr bool kRequestDlssSrFeature = false;


typedef sl::Result (*PFN_slDLSSGSetOptions)(
    const sl::ViewportHandle&,
    const sl::DLSSGOptions&
);
typedef sl::Result (*PFN_slDLSSGGetState)(
    const sl::ViewportHandle&,
    sl::DLSSGState&,
    const sl::DLSSGOptions*
);
inline constexpr int UE_CLOUD_PARAM_COUNT = 8;

// =====================================================
// 2026-10-06 远云分带合成（**已退役** —— 用户要求"让云回归 RR 的控制"，见
// cloudFarBandSplitActive() 的说明；下面的接线保留，改一个 constexpr 即可复活）
//
// 原设计：把云层按**距离**切成两带，走两条不同的合成路径：
//   · 近带（云距离 < split）→ 仍在 RR **之前**合成。近云边缘是"云缘闪"的来源，
//     走 RR 才有时域去噪 ⇒ 避开已退役的 A 方案（整层 post-RR）的闪。
//   · 远带（云距离 >= split）→ 在 RR **之后**、按**输出分辨率**合成。绕过 RR 的
//     上采样 ⇒ 远云清晰度与 RR 档位无关；远云低频、本身不闪。
//
// 远带的分辨率由「云分辨率下限」(g_cloudResFloorPercent) 提供：它把云 march 抬到
// 渲染分辨率之上（原本是纯浪费，现在被远带用上了）。合成器按 imageSize 自己决定
// 1:1 / 上采样 / 降采样，无需额外参数。
//
// 启用条件：分界距离 > 0 **且** 云分辨率下限 > 0（否则远带没有分辨率优势，整层
// 照旧走 pre-RR = 逐位旧档）。分界距离复用「远云RR引导距离」那个旋钮。
//
// 本模式下「远云 RR guide 补写」必须关掉：远云已经不在 RR 的颜色输入里，那份
// guide 会把 RR 拿到的东西谎报成"不透明的云面"，让 RR 按错误深度重投影
//（与整层 post-RR 模式同一个理由，见 recordRRGuideRelax）。
// =====================================================
struct CloudCompositePush
{
    float splitDistance;
    float mode;
};
typedef sl::Result (*PFN_slInit)(const sl::Preferences&,uint64_t);
typedef sl::Result (*PFN_slGetFeatureFunction)(
    sl::Feature,
    const char*,
    void*&
);
typedef sl::Result (*PFN_slGetFeatureRequirements)(
    sl::Feature,
    sl::FeatureRequirements&
);


typedef sl::Result (*PFN_slShutdown)();
typedef sl::Result (*PFN_slSetVulkanInfo)(const sl::VulkanInfo&);
typedef sl::Result (*PFN_slIsFeatureSupported)(sl::Feature,const sl::AdapterInfo&);
typedef sl::Result (*PFN_slSetFeatureLoaded)(sl::Feature,bool);
typedef sl::Result (*PFN_slEvaluateFeature)(
    sl::Feature,
    const sl::FrameToken&,
    const sl::BaseStructure**,
    uint32_t,
    sl::CommandBuffer*
);
typedef sl::Result (*PFN_slGetNewFrameToken)(
    sl::FrameToken*& frameToken,
    const uint32_t* frameIndex
);
typedef sl::Result (*PFN_slSetTagForFrame)(
    const sl::FrameToken&,
    const sl::ViewportHandle&,
    const sl::ResourceTag*,
    uint32_t,
    sl::CommandBuffer*
);
typedef sl::Result (*PFN_slSetConstants)(
    const sl::Constants&,
    const sl::FrameToken&,
    const sl::ViewportHandle&
);
typedef sl::Result (*PFN_slReflexSetOptions)(
    const sl::ReflexOptions&
);
typedef sl::Result (*PFN_slReflexGetState)(
    sl::ReflexState&
);
typedef sl::Result (*PFN_slReflexSleep)(
    const sl::FrameToken&
);
typedef sl::Result (*PFN_slPCLSetMarker)(
    sl::PCLMarker,
    const sl::FrameToken&
);
typedef sl::Result (*PFN_slDLSSSetOptions)(
    const sl::ViewportHandle&,
    const sl::DLSSOptions&
);
typedef sl::Result (*PFN_slDLSSGetOptimalSettings)(
    const sl::DLSSOptions&,
    sl::DLSSOptimalSettings&
);



typedef sl::Result (*PFN_slDLSSDSetOptions)(
    const sl::ViewportHandle&,
    const sl::DLSSDOptions&
);


typedef sl::Result (*PFN_slDLSSDGetOptimalSettings)(
    const sl::DLSSDOptions&,
    sl::DLSSDOptimalSettings&
);
typedef sl::Result (*PFN_slSetConstants)(
    const sl::Constants&,
    const sl::FrameToken&,
    const sl::ViewportHandle&
);
typedef sl::Result (*PFN_slFreeResources)(
    sl::Feature,
    const sl::ViewportHandle&
);
typedef sl::Result (*PFN_slDLSSGetState)(
    const sl::ViewportHandle&,
    sl::DLSSState&
);
typedef sl::Result (*PFN_slDLSSDGetState)(
    const sl::ViewportHandle&,
    sl::DLSSDState&
);
struct RtxMemLiveRecord
{
    uint64_t size = 0u;
    bool deviceLocal = false;
    uint64_t usage = 0u;
    bool hasUsage = false;
};

// 2026-10-10 抖动归因表：按 usage 汇总"累计分配/释放"与"当前存活"。
//
// 为什么需要：第二轮的实测里 vkAllocN 从 4770 涨到 33097（101 秒）⇒ **约 280 次
// vkAllocateMemory/秒**、约 50MB/s 的分配吞吐，而 vkLiveN 几乎不动（672→678）。
// 也就是活的没有泄漏，但在**高频抖动**；同时 [RT MEM] 的 privateMB 仍以 29MB/s 涨，
// 且日志里出现"1KB 的分配要 30.64ms"——这是驱动分配器在退化，不是活引用在漏。
// 抖动按 usage 归并后就能指认是哪个子系统在反复要/还。
struct RtxBufferChurnStats
{
    uint64_t allocCount = 0u;
    uint64_t allocBytes = 0u;
    uint64_t liveCount = 0u;
    uint64_t liveBytes = 0u;
};
struct RtxMemAccountSnapshot
{
    uint64_t allocCount = 0u;
    uint64_t freeCount = 0u;
    uint64_t freeUnknownCount = 0u;
    uint64_t allocBytes = 0u;
    uint64_t freeBytes = 0u;
    uint64_t liveBytes = 0u;
    uint64_t liveDeviceLocalBytes = 0u;
    uint64_t liveHostBytes = 0u;
    uint64_t livePeakBytes = 0u;
    size_t liveCount = 0u;
};

// =====================================================
// 2026-10-15 对象创建总账：计数器 + 9 个包装（判读见 g_device 声明处）
//
// 计数器用 relaxed 原子而不是像 g_rtxMemAllocCount 那样配互斥锁：这 9 个 API 的
// 调用点在渲染线程与工作线程上都有（命令缓冲那条在世界几何重建路径上），而这里
// 只是加一个整数，没有任何需要与其它状态保持一致的复合不变量 ⇒ 原子足够、且不引入
// 新的锁序问题（g_chunkMutex 的锁序约定不受影响）。
// =====================================================
struct RtxMemObjCounters
{
    std::atomic<uint64_t> descPool{0u};
    std::atomic<uint64_t> descSet{0u};
    std::atomic<uint64_t> cmdPool{0u};
    std::atomic<uint64_t> cmdBuf{0u};
    std::atomic<uint64_t> layout{0u};
    std::atomic<uint64_t> shader{0u};
    std::atomic<uint64_t> pipe{0u};
    std::atomic<uint64_t> view{0u};
    std::atomic<uint64_t> sampler{0u};

    std::atomic<uint64_t> cmdBufFree{0u};
    std::atomic<uint64_t> fence{0u};
    std::atomic<uint64_t> semaphore{0u};
    std::atomic<uint64_t> queryPool{0u};

    std::atomic<uint64_t> asCreate{0u};
    std::atomic<uint64_t> asDestroy{0u};
    std::atomic<uint64_t> asBuildCmd{0u};
    std::atomic<uint64_t> mmCreate{0u};
    std::atomic<uint64_t> mmDestroy{0u};
};
inline constexpr uint32_t PERF_L10_RAY_STAT_COUNT = 15u;
inline constexpr uint32_t PERF_L10_RAY_STAT_RING_SIZE = 8u;
inline constexpr uint64_t PERF_L10_RAY_STAT_WINDOW_FRAMES = 60u;

// Must stay in the same order as the shader's PERF_L10_RAY_STAT_* constants.
enum PerfL10RayStatIndex : uint32_t
{
    PERF_L10_RAY_STAT_INSTRUMENTED_PIXELS = 0u,
    PERF_L10_RAY_STAT_GI_PIXELS,
    PERF_L10_RAY_STAT_GI_RAYS,
    PERF_L10_RAY_STAT_GI_BOUNCE0_RAYS,
    PERF_L10_RAY_STAT_GI_BOUNCE1_RAYS,
    PERF_L10_RAY_STAT_GI_MISS_RAYS,
    PERF_L10_RAY_STAT_GI_ZERO_THROUGHPUT_RAYS,
    PERF_L10_RAY_STAT_GI_WATER_RAYS,
    PERF_L10_RAY_STAT_AO_PIXELS,
    PERF_L10_RAY_STAT_AO_RAYS,
    PERF_L10_RAY_STAT_FAR_LOD_PIXELS,
    PERF_L10_RAY_STAT_VERY_FAR_LOD_PIXELS,
    PERF_L10_RAY_STAT_LOCAL_LIGHT_RAYS,
    PERF_L10_RAY_STAT_SUN_SHADOW_RAYS,
    PERF_L10_RAY_STAT_GLOSSY_RAYS
};

// Mirrors PerfL10GpuTimestampSlot: a slot is folded exactly once, so a frame
// that bails out before its copy cannot be folded again on the next reuse.
struct PerfL10RayStatsSlot
{
    bool used = false;
    uint64_t frameSerial = 0u;
};

// ============================================================================
// 2026-10-29（P2a）：PBR 图集「按 sprite 生成 mip」—— ORION_PBR_SPRITE_MIP（默认开）
//
// 病灶：图集是一张连续纹理，sprite 之间没有 padding。整图 box（vkCmdBlitImage 逐级
// 1/2）在第 l 级把 2^l 个原 texel 平均成一个，于是**双线性足迹在方块边界处会跨进
// 邻居 sprite**，渗色宽度 = 1 个 mip texel = 2^l 个原 texel：
//     mip0 ≈ 1/128 方格（肉眼不可见，这就是"以前没有"）
//     mip4 ≈ 16/128 = 1/8 方格（明显的一条接缝带）
// 用户报的"方块与方块之间的分界线"就是它。
//
// 做法：逐级只对**每个 sprite 矩形内部**做 2x2 box，然后把矩形外扩 1 圈、用最近的
// 内部像素填充（只填未被内部写过的像素，避免破坏邻居）。这样任意一级的边界处取到的
// 都是本 sprite 自己的边缘值 ⇒ 跨 sprite 渗色为 0。
//
// 为什么放在这里而不是 shader 里内缩 UV：内缩会让边缘纹理被压缩且与 POM 的 UV 改写
// 打架；而且真因在 mip 数据本身。
//
// 回退：ORION_PBR_SPRITE_MIP=0 逐位回到原来的整图 blit 路径（旧代码未删）。
// ============================================================================
struct PbrSpriteRect
{
    uint32_t x = 0u;
    uint32_t y = 0u;
    uint32_t w = 0u;
    uint32_t h = 0u;
};

// 32 × 4 字节 = 128：与着色器 PostPush 逐字对应。
// 2026-10-23 追加 3 个色彩增益（lane 5..7）；
// 2026-10-26 追加「色彩分级」24 lane（lane 8..31）—— 128 字节正好是 Vulkan 对
// push constant 的**保底上限**（maxPushConstantsSize >= 128），所以到此为止，
// 再加参数必须换 UBO，不要再往这里塞。
struct PostProcessingPush
{
    int32_t tonemapMode;
    float exposureStrength;
    float contrast;
    float saturation;
    float gamma;
    float gainR;
    float gainG;
    float gainB;

    float manualEV;
    float blackPoint;
    float whitePoint;
    float gradeContrast;
    float gradePivot;
    float liftR;
    float liftG;
    float liftB;
    float gradeGammaR;
    float gradeGammaG;
    float gradeGammaB;
    float gradeGainR;
    float gradeGainG;
    float gradeGainB;
    float shadowHue;
    float shadowStrength;
    float highlightHue;
    float highlightStrength;
    float vibrance;
    float fade;
    float vignette;
    float vignetteRadius;
    float hueRotate;
    float tintGreen;
};
static_assert(sizeof(PostProcessingPush) == 128u,
    "PostProcessingPush must match the GLSL PostPush layout (32 lanes)");

// VK_EXT_opacity_micromap requires a Synchronization2 dependency between the
// micromap build write and the AS build read.  These private ABI mirrors keep
// the backend compatible with Vulkan headers which predate Synchronization2.
struct OmmMemoryBarrier2
{
    VkStructureType sType;
    const void* pNext;
    uint64_t srcStageMask;
    uint64_t srcAccessMask;
    uint64_t dstStageMask;
    uint64_t dstAccessMask;
};
struct OmmDependencyInfo
{
    VkStructureType sType;
    const void* pNext;
    VkDependencyFlags dependencyFlags;
    uint32_t memoryBarrierCount;
    const OmmMemoryBarrier2* pMemoryBarriers;
    uint32_t bufferMemoryBarrierCount;
    const void* pBufferMemoryBarriers;
    uint32_t imageMemoryBarrierCount;
    const void* pImageMemoryBarriers;
};
using PFN_vkCmdPipelineBarrier2Omm = void (VKAPI_PTR *)(
    VkCommandBuffer,
    const OmmDependencyInfo*
);
inline constexpr VkStructureType OMM_STRUCTURE_TYPE_MEMORY_BARRIER_2 =
    static_cast<VkStructureType>(1000314000);
inline constexpr VkStructureType OMM_STRUCTURE_TYPE_DEPENDENCY_INFO =
    static_cast<VkStructureType>(1000314001);
inline constexpr uint64_t OMM_STAGE_MICROMAP_BUILD = 0x40000000ull;
inline constexpr uint64_t OMM_ACCESS_MICROMAP_WRITE = 0x200000000000ull;
inline constexpr uint64_t OMM_STAGE_AS_BUILD = 0x02000000ull;
inline constexpr uint64_t OMM_ACCESS_AS_READ = 0x00200000ull;


// Strictly matches the global GLSL scalar Vertex ABI: 12 floats / 48 bytes.
struct Vertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
    float materialId;
    float pad1;
    float pad2;
};
static_assert(sizeof(Vertex) == 48, "RT Vertex ABI must remain 12 floats");

// =====================================================
// 2026-10-21 地形 32B 顶点（Sodium CompactChunkVertex 思路，**只用于地形桶**）
//
// 48B → 32B：颜色压成 RGBA8（它本来就是从 8 位字节扩展来的，无损），
// pad1（雨面标记，只有 0 / 0.25）折进 pad2 的高偏移。
//
// ⚠ CPU 侧（g_chunkMeshes / SectionGeometrySplit / VPT / 分类）仍然是 48B `Vertex`，
//   只有 **GPU 顶点缓冲**换布局 ⇒ Java 不动、实体/粒子（Java 按 48B 写）不动。
//
// 开关必须与 SPV 的 `-DSECTION_VERTEX_32B=1` **同时生效**：shader 侧没有 stride 参数，
// 布局是编译期定的。只改一侧 = 颜色/材质整体错位。
// =====================================================
struct SectionVertex
{
    float x, y, z;
    float u, v;
    uint32_t color;
    float materialId;
    float pad2;
};
static_assert(sizeof(SectionVertex) == 32, "Section vertex ABI must be 32 bytes");
inline constexpr float SECTION_VERTEX_PAD1_BIAS = 64.0f;
inline constexpr bool SECTION_VERTEX_32B_ENABLED = true;
inline constexpr uint32_t kStainedGlassDyeRgbById[16] =
{
    15790320u, 15435844u, 12801229u,  6719955u,
    14602026u,  4312372u, 14188952u,  4408131u,
    11250603u,  2651799u,  8073150u,  2437522u,
     5320730u,  3887386u, 11743532u,  1973019u
};

// Voxy has no Minecraft 16^3 Section identity.  Its source key is therefore
// mapped into a reserved pseudo-section band (Y=32766, outside legal world
// height) before entering the existing Section BLAS/TLAS scheduler.  The
// source map remains separate so versioning/removal never relies on hashes.
struct VoxyLodSource
{
    uint64_t version = 0;
    uint64_t spatialKey = 0;
};
inline constexpr uint64_t VOXY_LOD_RESERVED_Y = 0x7ffeull << 44ull;
inline constexpr uint64_t VOXY_LOD_SPATIAL_LOW_MASK = (1ull << 44ull) - 1ull;


inline constexpr uint64_t ENTITY_RT_SPATIAL_KEY =
    (0x7fffull << 44ull) |
    (0x3fffffull << 22ull) |
    0x3fffffull;
inline constexpr uint64_t ENTITY_RT_LAYER_KEY =
    (5ull << 60ull) | ENTITY_RT_SPATIAL_KEY;
inline constexpr uint64_t LOCAL_PLAYER_RT_LAYER_KEY =
    (6ull << 60ull) | ENTITY_RT_SPATIAL_KEY;
// 2026-11-03：第一人称手部层（见 SECTION_AS_CLASS_HAND 处的说明）。
inline constexpr uint64_t HAND_RT_LAYER_KEY =
    (8ull << 60ull) | ENTITY_RT_SPATIAL_KEY;
inline constexpr uint64_t PARTICLE_RT_LAYER_KEY =
    (7ull << 60ull) | ENTITY_RT_SPATIAL_KEY;
// 2026-11-03：8 -> 1。手部（本地玩家代理 + 手持物）是**逐帧骨骼动画**，
// 但 BLAS 原先每 8 帧才重建一次 ⇒ RT 里的手是"冻结 8 帧、跳一下"，
// 表现为手部动画时有时无 / 顿挫。手部顶点数很小（手臂 + 手持物），
// 每帧重建的提交开销远小于一个错误的观感。
inline constexpr uint64_t LOCAL_PLAYER_AS_UPDATE_INTERVAL = 1u;
inline constexpr uint32_t ENTITY_RT_TEXTURE_SLOT_COUNT = 128u;
inline constexpr uint32_t PARTICLE_RT_TEXTURE_SLOT_COUNT = 3u;

// 2026-10-31：上传去重从"锁内全量 memcmp"改成"锁外算摘要 + 锁内比摘要"。
//
// g_chunkMutex 是所有 Java 几何上传线程（Sodium 的多个 chunk builder 线程）、
// 几何捕获线程、DH/Voxy 上传与渲染线程共用的**一把**锁。原来 uploadChunk 在锁内
// 做了两次 O(字节) 的比较：raw payload 的 memcmp、以及重排后顶点的
// sameVertexPayload。20KB 级载荷 = 每次几 µs 的**串行**时间，被所有上传线程排队
// 放大，正是"持有 g_chunkMutex 挡着 Java 几何上传"的来源。
//
// 这里改成 C2ME 的 cache.remove(pos, expectedFuture) 同款语义：比"身份"而不是比
// "内容"。摘要在锁外算（各 Java 线程并行），锁内只剩哈希表查找 + 3 个整数比较。
//
// 副产品：g_chunkRawPayloads 的整份副本不再需要（它只为 memcmp 而存在，
// rawMB ≈ meshMB），几何 CPU 内存直接减半。
struct ChunkUploadDigest
{
    uint64_t rawHash = 0u;
    uint64_t meshHash = 0u;
    uint64_t rawBytes = 0u;
    uint64_t meshVertices = 0u;
};
inline constexpr uint64_t SPATIAL_SECTION_KEY_MASK =
    (1ull << 60ull) - 1ull;
inline constexpr uint32_t SECTION_BUCKET_REGION_CHUNKS_LOG2 =
    2u;
inline constexpr uint32_t SECTION_BUCKET_MAX_MEMBERS =
    24u;
inline constexpr uint64_t SECTION_AS_CLASS_OPAQUE = 1ull << 60ull;
inline constexpr uint64_t SECTION_AS_CLASS_OMM    = 2ull << 60ull;
inline constexpr uint64_t SECTION_AS_CLASS_ALPHA  = 3ull << 60ull;
inline constexpr uint64_t SECTION_AS_CLASS_WATER  = 4ull << 60ull;
inline constexpr uint64_t SECTION_AS_CLASS_ENTITY = 5ull << 60ull;
inline constexpr uint64_t SECTION_AS_CLASS_LOCAL_PLAYER = 6ull << 60ull;
inline constexpr uint64_t SECTION_AS_CLASS_PARTICLE = 7ull << 60ull;
// 2026-11-03：第一人称手部（手臂 + 手持物）独立成层。
//
// 它需要一对互相矛盾的可见性：**主相机要看得见**（不然手上什么都没有），
// 但**次级射线必须打不到**（不然会在世界里投出一截手臂影子 —— 而 viewmodel
// 本来就不参与世界阴影，且它的坐标是相机相对的，影子会随视角乱飘）。
// 现有的位组合不出来：0xff 两者都可见、0x02 主相机不可见、0xfb 两者都可见。
// 所以新开一位 0x04：
//   instance.mask = 0x04
//   主相机射线   0x01 -> 0x05   （0x01 | 0x04）
//   所有次级射线 0xff -> 0xfb   （0xff & ~0x04）
inline constexpr uint64_t SECTION_AS_CLASS_HAND = 8ull << 60ull;
inline constexpr uint64_t LOD_AS_CLASS_ALPHA = 2ull << 60ull;
inline constexpr uint64_t LOD_AS_CLASS_WATER = 3ull << 60ull;
inline constexpr uint32_t SBT_RECORD_OPAQUE = 0u;
inline constexpr uint32_t SBT_RECORD_OMM    = 1u;
inline constexpr uint32_t SBT_RECORD_ALPHA  = 2u;
inline constexpr uint32_t SBT_RECORD_WATER  = 3u;
inline constexpr uint32_t SBT_RECORD_ENTITY = 4u;
inline constexpr uint32_t SBT_HIT_RECORD_COUNT = 5u;
struct SectionGeometrySplit
{
    std::vector<Vertex> opaque;
    std::vector<Vertex> ommCutout;
    std::vector<Vertex> alpha;
    std::vector<Vertex> water;
    std::vector<Vertex> entity;
    std::vector<Vertex> localPlayer;
    std::vector<Vertex> particle;
    OmmCutoutInput ommInput;
    bool hasOmmInput = false;

    std::vector<float> entityMotion;
    std::vector<float> localPlayerMotion;
    std::vector<float> particleMotion;

    std::vector<SectionVertex> opaquePacked;
    std::vector<SectionVertex> ommCutoutPacked;
    std::vector<SectionVertex> alphaPacked;
    std::vector<SectionVertex> waterPacked;
};

// =======================================================================
// 2026-10-08 几何捕获线程（BLAS 异步化·第一步）。
//
// rebuildWorldGeometry 互斥锁内的"分类拷贝"（把 g_chunkMeshes 的 16 个 layer
// 按 water/alpha/OMM 路由进 SectionGeometrySplit）是流式期间渲染线程的大头，
// 且持有 g_chunkMutex 挡着 Java 几何上传。本线程把这段搬到渲染线程之外：
// 只做 CPU 侧分类拷贝，零 Vulkan 调用；产出的 split 队列由渲染线程在原
// rebuildWorldGeometry 路径消费（createSectionBLAS/构建/TLAS 全部原地不动）。
//
// 实体聚合键（ENTITY_RT_SPATIAL_KEY，entity/localPlayer/particle 三个动态
// 聚合共用一个空间键）不经过本线程：它们逐帧动，必须留在渲染线程同步捕获
// 保证新鲜度。
//
// 锁序（全局约定）：g_chunkMutex -> g_capturedSectionsMutex。
// 回退：ORION_SYNC_GEOMETRY=1 时不启动本线程，渲染线程走原同步捕获。
// =======================================================================
struct CapturedSectionUpdate
{
    uint64_t spatialKey = 0u;
    SectionGeometrySplit split;
};

// 2026-10-31：捕获批大小**保持 4**（曾试过 8，已回退）。
//
// 实测（_ab_B.log / _wake.log / _cap250.log 三段）：
//   · queued 在所有采样里恒为 0 —— 捕获流水线从未积压，限制几何产出的是渲染侧
//     5~12ms 预算，不是捕获吞吐。所以"多抓一倍"换来的是空转产出；
//   · 锁的尾巴由**单次持锁的绝对工作量**决定：4 -> 8 直接把 capHoldMs p95
//     从 21ms 顶到 30.6ms、capWaitMs p95 7 -> 21.6ms。
// 结论：批大小越小尾巴越短，吞吐由别处决定 ⇒ 回到 4。
inline constexpr size_t GEOMETRY_CAPTURE_MAX_PER_WAKE = 4u;
inline constexpr size_t GEOMETRY_CAPTURE_MAX_QUEUED = 256u;



struct LocalLightRecord
{
    int32_t worldX;
    int32_t worldY;
    int32_t worldZ;
    int32_t emissionLevel;
    int32_t typeCode;
};


static_assert(
    sizeof(LocalLightRecord) == 20,
    "LocalLightRecord must remain exactly 20 bytes"
);



struct GPULocalLightHeader
{
    uint32_t lightCount;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};


static_assert(
    sizeof(GPULocalLightHeader) == 16,
    "GPULocalLightHeader must remain exactly 16 bytes"
);


struct GPULocalLightRecord
{
    float worldX;
    float worldY;
    float worldZ;
    float emissionLevel;

    uint32_t typeCode;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};


static_assert(
    sizeof(GPULocalLightRecord) == 32,
    "GPULocalLightRecord must remain exactly 32 bytes"
);



inline constexpr int32_t
    LOCAL_LIGHT_GRID_CELL_SIZE = 64;


inline constexpr uint32_t
    LOCAL_LIGHT_MAX_PER_CELL = 32u;


inline constexpr uint32_t
    LOCAL_LIGHT_GRID_SLOT_WORDS = 8u;


struct GPULocalLightGridHeader
{
    uint32_t occupiedCellCount;
    uint32_t lightIndexCount;
    uint32_t hashCapacity;
    uint32_t slotStrideWords;
};


static_assert(
    sizeof(GPULocalLightGridHeader) == 16,
    "GPULocalLightGridHeader must remain exactly 16 bytes"
);


struct GPULocalLightGridSlot
{
    int32_t cellX;
    int32_t cellY;
    int32_t cellZ;
    uint32_t occupied;

    uint32_t firstLightIndex;
    uint32_t lightCount;
    uint32_t pad0;
    uint32_t pad1;
};


static_assert(
    sizeof(GPULocalLightGridSlot) == 32,
    "GPULocalLightGridSlot must remain exactly 32 bytes"
);


inline constexpr uint32_t
    LOCAL_LIGHT_GPU_MAX_LIGHTS = 32768u;


inline constexpr uint32_t
    LOCAL_LIGHT_GRID_MAX_HASH_SLOTS = 65536u;


inline constexpr VkDeviceSize
    LOCAL_LIGHT_GPU_FIXED_BYTES =
        sizeof(GPULocalLightHeader) +
        static_cast<VkDeviceSize>(
            LOCAL_LIGHT_GPU_MAX_LIGHTS
        ) *
        sizeof(GPULocalLightRecord);


inline constexpr VkDeviceSize
    LOCAL_LIGHT_GRID_FIXED_BYTES =
        sizeof(GPULocalLightGridHeader) +
        static_cast<VkDeviceSize>(
            LOCAL_LIGHT_GRID_MAX_HASH_SLOTS
        ) *
        sizeof(GPULocalLightGridSlot) +
        static_cast<VkDeviceSize>(
            LOCAL_LIGHT_GPU_MAX_LIGHTS
        ) *
        sizeof(uint32_t);


// =====================================================
// VPT-2.2 - screen-space temporal history for the primary
// voxel DDA candidate.  Self-contained: it has its own
// representation, independent of any local-DI reservoir.
// Four std430 vec4 lanes = 64 bytes per render-resolution pixel.
// Lane 3 (reservoirX..) backs the RTXDI-style WRS direct-light reservoir;
// the EMA path leaves it untouched.
// =====================================================
struct VPTDirectHistoryGPU
{
    float radianceX;
    float radianceY;
    float radianceZ;
    float sampleCount;

    float hitPositionX;
    float hitPositionY;
    float hitPositionZ;
    float valid;

    float normalX;
    float normalY;
    float normalZ;
    float padding;

    float reservoirX;
    float reservoirY;
    float reservoirZ;
    float reservoirW;
};
static_assert(
    sizeof(VPTDirectHistoryGPU) == 64,
    "VPTDirectHistoryGPU must remain exactly four std430 vec4 lanes"
);



inline constexpr uint32_t VLI_MAX_SECTION_PAGES = 4096u;
// 2026-10-31：512 -> 1024。
// 实测：进世界后 21 秒 livePages 就顶到 512，触发 512->1024 的增长（持 g_vliMutex
// 8.55ms，VLI worker 排队 12ms、渲染线程 wait 15.8ms）；再 17 秒后顶到 1024。
// 把初值提到 1024 后，常见视野距离下的一次增长直接消失；代价是常驻 +
// 94.8MB VRAM（本机 4.0/7.2GB，余量充足）。
inline constexpr uint32_t VLI_INITIAL_SECTION_PAGES = 1024u;
inline constexpr uint32_t VLI_PAGE_TABLE_CAPACITY = 8192u;
inline constexpr uint32_t VLI_SOURCE_WORDS = 16u * 16u * 16u;
inline constexpr uint32_t VLI_PROBE_EDGE = 9u;
inline constexpr uint32_t VLI_PROBE_WORDS = VLI_PROBE_EDGE * VLI_PROBE_EDGE * VLI_PROBE_EDGE;
inline constexpr uint32_t VPT_DIRECTION_COUNT = 6u;
inline constexpr uint32_t VPT_DIRECTIONAL_CACHE_WORDS =
    VPT_DIRECTION_COUNT * VLI_PROBE_WORDS;
inline constexpr uint32_t VLI_PAGE_WORDS =
    VLI_SOURCE_WORDS + VPT_DIRECTIONAL_CACHE_WORDS;
inline constexpr uint32_t VPT_SURFACE_FACES_PER_SECTION =
    VLI_SOURCE_WORDS * VPT_DIRECTION_COUNT;
inline constexpr uint32_t VPT_PROBES_PER_PAGE_PER_FRAME = VLI_PROBE_WORDS;
inline constexpr uint32_t VPT_EMITTER_HEADER_WORDS = 4u;
inline constexpr uint32_t VPT_MAX_EMITTERS_PER_SECTION = VLI_SOURCE_WORDS;
inline constexpr uint32_t VPT_EMITTER_PAGE_WORDS =
    VPT_EMITTER_HEADER_WORDS + VPT_MAX_EMITTERS_PER_SECTION;
inline constexpr uint32_t VPT_RELATED_EMITTERS_PER_SECTION = 8u;
inline constexpr uint32_t VPT_SOURCE_DIRECTORY_RADIUS = 2u;
inline constexpr uint32_t VPT_SOURCE_DIRECTORY_MAX_SECTIONS =
    (VPT_SOURCE_DIRECTORY_RADIUS * 2u + 1u) *
    (VPT_SOURCE_DIRECTORY_RADIUS * 2u + 1u) *
    (VPT_SOURCE_DIRECTORY_RADIUS * 2u + 1u);
inline constexpr uint32_t VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE =
    1u + VPT_SOURCE_DIRECTORY_MAX_SECTIONS;
static_assert(
    VLI_PAGE_WORDS >= VLI_SOURCE_WORDS + VPT_DIRECTIONAL_CACHE_WORDS,
    "VLI page must contain source voxels and directional irradiance probes"
);
static_assert(
    VPT_EMITTER_PAGE_WORDS * sizeof(uint32_t) <= 65536u,
    "VPT emitter page must fit one vkCmdUpdateBuffer operation"
);
struct VLITableSlot
{
    int32_t sectionX = 0;
    int32_t sectionY = 0;
    int32_t sectionZ = 0;
    uint32_t pagePlusOne = 0;
};
static_assert(sizeof(VLITableSlot) == 16, "VLI table slot must be 16 bytes");
struct VPTRelatedEmitterGPU
{
    int32_t worldX = 0;
    int32_t worldY = 0;
    int32_t worldZ = 0;
    uint32_t emissionAndType = 0;
};
static_assert(
    sizeof(VPTRelatedEmitterGPU) == 16,
    "VPTRelatedEmitterGPU must map to one std430 uvec4"
);
struct VPTSourceDirectoryEntryGPU
{
    uint32_t pageIndex = 0u;
    uint32_t emitterCountAndOffset = 0u;
    uint32_t aliasThresholdQ16 = 0u;
    uint32_t aliasIndex = 0u;
    float selectionProbability = 0.0f;
};
static_assert(
    sizeof(VPTSourceDirectoryEntryGPU) == 20,
    "VPTSourceDirectoryEntryGPU must map to the std430 Alias entry"
);
struct VLISectionCpuPage
{
    int32_t sectionX = 0;
    int32_t sectionY = 0;
    int32_t sectionZ = 0;
    uint32_t pageIndex = 0;
    uint32_t tableSlot = 0;

    uint64_t lastUseTick = 0u;
    std::vector<uint32_t> sourceWords;
    std::vector<uint32_t> emitterIndexWords;
    std::vector<uint32_t> emitterAliasWords;
    std::array<VPTRelatedEmitterGPU, VPT_RELATED_EMITTERS_PER_SECTION>
        relatedEmitters{};
    std::array<VPTSourceDirectoryEntryGPU,
        VPT_SOURCE_DIRECTORY_ENTRIES_PER_PAGE> sourceDirectory{};
};

// SectionCompiler worker threads only own the CPU-side Section payload.  A
// Section that arrives while the pool is at its current capacity is parked here
// with its payload and registered on the render thread right after the pool has
// grown.  The Section is never dropped, it is only delayed by a frame.
struct VLIPendingSection
{
    uint64_t sectionKey = 0u;
    int32_t sectionX = 0;
    int32_t sectionY = 0;
    int32_t sectionZ = 0;
    std::vector<uint32_t> sourceWords;
    std::vector<LocalLightRecord> lights;
};
inline constexpr uint64_t VLI_INVALID_SECTION_KEY = ~static_cast<uint64_t>(0);
// VPT-3.3: 定义两阶段 Compute 执行协议结构体 (8 字节)
struct VPTPushConstants
{
    uint32_t eventSerial;
    uint32_t passIndex;
};


// 显存预算采样。原来是**内联**在 logNativeMemoryReport() 里的；抽出来是为了让闸门
// 复用**同一份口径** —— 两处各算一次迟早会漂移，本文件已经吃过这种亏（见
// rtLodDiffuseGiEnabled 的"唯一出口"注释）。原处现在改为调用本函数。
// 驱动不支持 VK_EXT_memory_budget 时两个字段保持 0，闸门据此判定"不可用"并全程不动作
// （⇒ 回到改动前的行为，不是"猜一个值"）。
struct VramBudgetSample
{
    double usedMb = 0.0;
    double budgetMb = 0.0;
};
inline constexpr uint64_t VRAM_GOVERNOR_SAMPLE_TICKS = 30u;
struct VramGovernorState
{
    bool available = false;
    bool enabled = false;
    bool over = false;
    double usedMb = 0.0;
    double budgetMb = 0.0;
    uint64_t tickCount = 0u;
    uint64_t sampleCount = 0u;
    uint64_t overEnterCount = 0u;
    double freedTotalMb = 0.0;
};




inline constexpr bool SECTION_MEMORY_POOL_ENABLED = true;
inline constexpr uint32_t SECTION_MEMORY_POOL_MIN_EXP = 16u;
inline constexpr uint32_t SECTION_MEMORY_POOL_MAX_EXP = 36u;
inline constexpr uint32_t SECTION_MEMORY_POOL_BUCKET_COUNT =
    (SECTION_MEMORY_POOL_MAX_EXP - SECTION_MEMORY_POOL_MIN_EXP + 1u) * 2u;
inline constexpr VkDeviceSize SECTION_MEMORY_POOL_MAX_KEEP_BYTES =
    1073741824u;
inline constexpr size_t SECTION_MEMORY_POOL_MAX_KEEP_SLABS = 32u;
inline constexpr VkDeviceSize SECTION_MEMORY_SLAB_TARGET_BYTES =
    67108864u;
inline constexpr uint32_t SECTION_MEMORY_SLAB_MAX_CHUNKS = 512u;
inline constexpr uint32_t SECTION_POOL_CHUNK_WASTE_LOG2_DEFAULT = 1u;
struct SectionMemorySlab
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize chunkBytes = 0;
    VkDeviceSize capacity = 0;
    uint32_t memoryTypeIndex = 0;
    uint32_t chunkCount = 0;
    uint32_t liveChunks = 0;
    std::vector<uint32_t> freeChunks;
};
inline constexpr VkDeviceSize SECTION_MEMORY_POOL_RESTORED_KEEP_BYTES =
    268435456u;
inline constexpr size_t SECTION_MEMORY_POOL_RESTORED_KEEP_SLABS = 4u;

// =====================================================
// 2026-10-15（OOM 崩溃修复）：显存/内存耗尽专用异常 + 跳过计数。
//
// 依据：run/rt_debug.log 里同一条 "allocate Section memory slab failed
// VkResult=-2"（VK_ERROR_OUT_OF_DEVICE_MEMORY）已出现 6 次（今天 12:18/13:47
// 两次发生在本会话任何改动之前 ⇒ 与仪表改动无关），每次都因 check() 的
// runtime_error 沿调用链无人接而 terminate（hs_err: Render thread,
// EXCEPTION_UNCAUGHT_CXX_EXCEPTION）。崩溃前 [VRAM GOV] over=1 / ratio≈1.09：
// 超支的 ~10% 里有相当部分是**其它进程**占的（vkLiveMB≈3.5G 而 usedMB≈7.4G），
// 闸门把自己的池全还了也压不下去 ⇒ 这是环境性失败，正确响应是降级
// （跳过本块、进非常驻表等补传），不是死进程——与 OMM 路的先例
// （pool acquire failed ⇒ log + fallback=anyHit）同一取向。
//
// 它与"真 bug"必须区分：Section 构建在 catch 里只对这一种异常降级，
// 其余异常仍照旧终止 —— 吞掉真 bug 会让画面静默地错下去。
// =====================================================
struct SectionMemoryUnavailableError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};
inline constexpr uint64_t VRAM_PROBE_SAMPLE_FRAMES = 30u;
inline constexpr uint64_t VRAM_PROBE_LOG_FRAMES = 120u;
struct VramProbeState
{
    bool available = false;
    bool warned = false;
    double usedMb = 0.0;
    double budgetMb = 0.0;
    double ratio = 0.0;
    double peakUsedMb = 0.0;
    double peakRatio = 0.0;
    double minFreeMb = -1.0;
    uint64_t frames = 0u;
    uint64_t samples = 0u;
    uint64_t warnEnters = 0u;
};
inline constexpr uint32_t OMM_POOL_CHUNK_WASTE_LOG2 = 32u;
inline constexpr uint32_t SECTION_QUAD_INDEX_MAX_QUADS = 16384u;
inline constexpr uint32_t SECTION_QUAD_INDEX_MAX_VERTS =
    SECTION_QUAD_INDEX_MAX_QUADS * 4u;
inline constexpr bool SECTION_INDEXED_TERRAIN_ENABLED = true;
struct SparseActiveMaskPush
{
    uint32_t width;
    uint32_t height;
    uint32_t frameIndex;
    uint32_t maskWidth;
    float samplingRate;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};
inline constexpr uint32_t BLOOM_MIP_COUNT = 5u;
struct BloomMip
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0u;
    uint32_t height = 0u;
};

// 合成 pass 的 push constant（4 字节，与 bloom_composite.comp 的 BloomCompositePush 对齐）。
struct BloomCompositePush
{
    float strength;
};
static_assert(sizeof(BloomCompositePush) == 4u,
    "BloomCompositePush must stay 4 bytes (shader-side layout)");


inline constexpr uint32_t DYNAMIC_LIGHT_MAX_RECORDS = 128u;
struct DynamicLightRecord
{
    float worldX;
    float worldY;
    float worldZ;
    float emission;
    uint32_t typeCode;
    uint32_t pad0;
    uint32_t pad1;
    uint32_t pad2;
};
static_assert(sizeof(DynamicLightRecord) == 32u,
    "dynamic light record must stay 32 bytes (shader-side layout)");
struct DynamicLightUpload
{
    uint32_t count;
    float intensity;
    uint32_t revision;
    uint32_t reserved;
    DynamicLightRecord records[DYNAMIC_LIGHT_MAX_RECORDS];
};
inline constexpr VkDeviceSize DYNAMIC_LIGHT_BUFFER_BYTES = sizeof(DynamicLightUpload);


inline constexpr uint32_t RT_DESCRIPTOR_RING_SIZE = 4u;
// 2026-10-22：gather 的扁平暂存。
//
// 原来每页 emplace_back(pageIndex, std::vector) 会为 sourceWords /
// emitterIndexWords / emitterAliasWords 各分配一次 16KB —— 254 页就是 760 次
// 16KB 堆分配，而工作线程在同一时刻做同样的事，堆锁互相拖 ⇒ 实测持锁侧
// 6~10ms、另一侧 lockWait 16~24ms。
//
// 现在改成：一个跨帧复用的 arena（clear 保留容量 ⇒ 稳态零分配）+ 每条目一个
// (pageIndex, wordOffset, wordCount) 描述符。数据仍是渲染线程私有快照，
// 不引用解锁后可能被淘汰的页指针。
struct VliUploadRef
{
    uint32_t pageIndex = 0u;
    uint32_t wordOffset = 0u;
    uint32_t wordCount = 0u;
};
inline constexpr uint32_t NRD_MAX_DISPATCHES = 64u;
struct NRDCompositePushConstants
{
    uint32_t width;
    uint32_t height;
};
inline constexpr float NRD_LOCAL_DI_ACCUMULATION_SECONDS = 1.0f;
inline constexpr float NRD_LOCAL_DI_FAST_HISTORY_CLAMP_SIGMA = 1.5f;
inline constexpr bool NRD_LOCAL_DI_HIT_DIST_RECONSTRUCTION_5X5 = true;
#ifndef NRD_LOCAL_DI_CONFIDENCE
#define NRD_LOCAL_DI_CONFIDENCE 1
#endif
inline constexpr float NRD_DI_CONFIDENCE_SENSITIVITY = 4.0f;
inline constexpr float NRD_DI_CONFIDENCE_THRESHOLD = 0.35f;
inline constexpr float NRD_DI_CONFIDENCE_EMA_BETA = 0.10f;
inline constexpr float NRD_DI_CONFIDENCE_TEMPORAL_ALPHA = 0.25f;
inline constexpr float NRD_DI_CONFIDENCE_MAX_VIEWZ = 60000.0f;
struct DiConfidencePushConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t mode;
    float gradientSensitivity;
    float gradientThreshold;
    float temporalAlpha;
    float maxViewZ;
    float lumaEmaBeta;
    float padding0;
};
struct DirectAtrousPushConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t stepSize;
    float phiColor;
    uint32_t encodeToYCoCg;
};
struct DiReprojectPushConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t resetHistory;
};
struct DiMomentsPushConstants
{
    uint32_t width;
    uint32_t height;
};
struct DiPreparePushConstants
{
    uint32_t width;
    uint32_t height;
};
inline constexpr float SUN_SHADOW_PHASE_EPS = 1.0e-4f;
inline constexpr float SUN_SHADOW_NEAR_TEXEL_WORLD = 2.0f * 160.0f / 256.0f;

// S2：INITIAL 阶段的 push constant。与 vol_restir.comp 的 VolRestirPush 逐字段对应。
struct VolRestirPushConstants
{
    int32_t froxelSize[4];
    float volumeParams[4];
};

// S3：temporal / spatial 的 push constant。两者共用 32 字节范围（见
// VOL_RESTIR_CHAIN_PUSH_BYTES），字段按各自的 shader 块体对应。
struct VolRestirTemporalPushConstants
{
    int32_t froxelSize[4];
    float temporalParams[4];
};
struct VolRestirSpatialPushConstants
{
    float spatialParams[4];
    float reserved[4];
};

// S4：froxel 入散射与屏幕积分的 push constant。
struct VolRestirIntegratePushConstants
{
    int32_t froxelSize[4];
    float volumeParams[4];
};
struct VolRestirCompositePushConstants
{
    float params[4];
    int32_t grid[4];
};

// =====================================================
// 2026-09-30 大气重写（方案 A）：Bruneton 预计算 LUT 资源。
// LUT：transmittance 256×64、irradiance/deltaIrradiance 64×16、
// 散射 3D 256×128×32（scattering/singleMie = rgba32f 累加，
// delta 四件 = rgba16f 中间）。启动时一次性执行 22 个 pass 派发，
// 此后 sky_atmosphere.comp 只读采样。开关：设置里的「物理天空」
// （skyParams1.y）ON = Bruneton、OFF = 旧 legacy 渐变。
// =====================================================
struct BrunetonLutImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView sampledView = VK_NULL_HANDLE;
    VkImageView storageView = VK_NULL_HANDLE;
};
inline constexpr uint32_t WATER_FFT_RESOLUTION = 256u;
static_assert(WATER_FFT_RESOLUTION == 256u, "shader WATER_FFT_N is 256");
inline constexpr uint32_t WATER_FFT_LOG2 = 8u;
inline constexpr uint32_t WATER_FFT_LOCAL_SIZE = 8u;
inline constexpr uint32_t WATER_FFT_SLOPE_MIPS = WATER_FFT_LOG2 + 1u;
inline constexpr uint32_t WATER_FFT_STATE_LAYERS = 4u;
inline constexpr float kWATER_FFT_DETAIL_TILE = 32.0f;
inline constexpr float kWATER_FFT_DETAIL_MIN_WAVE_RADIUS = 32.0f;
inline constexpr float kWATER_FFT_DETAIL_BOOST = 1.5f;
inline constexpr float kWATER_FFT_SPECTRUM_SCALE = 8.0f;

// 16 × 4 字节 = 64：与着色器 WaterFftPush 逐字对应（顺序/类型都不能动）
struct WaterFftPushConstants
{
    int32_t sourceLayer;
    int32_t destinationLayer;
    int32_t stageLength;
    int32_t mode;
    int32_t axis;
    int32_t detailSourceLayer;
    float timeSeconds;
    float windSpeed;
    float rainStrength;
    float spectrumScale;
    float visualScale;
    float choppiness;
    float texelWorldSize;
    float detailTexelWorldSize;
    float detailVisualScale;
    float minWaveRadius;
};
static_assert(sizeof(WaterFftPushConstants) == 64u,
    "WaterFftPushConstants must match the std430 layout of the GLSL push block");
inline constexpr bool kWATER_FFT_PASSES_ENABLED = true;

// 2026-11-01 重构：顶点拉伸光栅 -> 逐 texel compute。分辨率 1024 -> 512
// （texel 数 1/4），mip 11 -> 7（512 -> 8）。
inline constexpr uint32_t WATER_CAUSTIC_MAP_RESOLUTION = 512u;
inline constexpr uint32_t WATER_CAUSTIC_MAP_GRID = 256u;
inline constexpr uint32_t WATER_CAUSTIC_MAP_MIPS = 7u;
static_assert(WATER_CAUSTIC_MAP_RESOLUTION == 512u,
    "water_caustic_map.comp 的 CAUSTIC_RESOLUTION 必须与这里一致");
static_assert((WATER_CAUSTIC_MAP_RESOLUTION >> (WATER_CAUSTIC_MAP_MIPS - 1u)) == 8u,
    "WATER_CAUSTIC_MAP_MIPS must run 512 -> 8");
static_assert(WATER_CAUSTIC_MAP_GRID == 256u,
    "water_caustic_map.vert 的 GRID 是 256（旧路径常量，仅留档）");
inline constexpr float WATER_CAUSTIC_MAP_REFERENCE_DEPTH = 1.0f;
// 2026-11-01：4 -> 16。用户要求反射焦散覆盖到水面以上 32 格；参考面取中位
// 16 格，消费端再按实际高度补偿（见 rt_test.rgen 的 reflection 采样点）。
inline constexpr float WATER_REFLECT_CAUSTIC_MAP_HEIGHT = 16.0f;
inline constexpr uint32_t WATER_CAUSTIC_MAP_LAYER_REFRACT = 0u;
inline constexpr uint32_t WATER_CAUSTIC_MAP_LAYER_REFLECT = 1u;
inline constexpr uint32_t WATER_CAUSTIC_MAP_LAYERS = 2u;

// 与 water_caustic_map.vert / .frag 的 push 块逐字对应（2 × vec4 = 32 B）。
struct WaterCausticMapPushConstants
{
    float sunDirection[4];
    float params[4];
};
static_assert(sizeof(WaterCausticMapPushConstants) == 32u,
    "WaterCausticMapPushConstants must match the GLSL push block");
inline constexpr float VOXY_LOD_SHADE_COMPRESS = 0.45f;

// quad 首三角形的外向法线 → 主轴 + 朝向（面都是轴对齐的，用于判断是不是侧面）。
struct VoxyQuadFace
{
    int axis;
    bool positive;
};
inline constexpr uint64_t DH_LOD_SALT = 0x44484c4f444c4f44ull;

// One record per DH section (2026-10-01: per-section, not one merged mesh).
//   sectionKey = Distant Horizons' own DhSectionPos encoding.  It is a stable
//                identity for "that piece of far terrain" across rebuilds, so
//                it can serve as the source key of the voxy-style allocator.
//   version    = bumped by Java every time the section is re-converted (its
//                per-section serial plus the texture-ready bit).
struct DhLodSection
{
    uint64_t version = 0;
    uint64_t spatialKey = 0;
};
inline constexpr float VRAM_EVICT_HYSTERESIS = 1.25f;
inline constexpr int64_t SECTION_PRELOOP_LOCK_FALLBACK_MS = 12;

// -------- 共享工具头（bc7enc.h 无 include guard，只在此包含一次） --------

#include <bc7enc.h>


// ------------------------- 函数原型 -------------------------

std::string GetRTXBackendDirectory();
bool IsAbsoluteWindowsPath(
    const char* path
);
std::string ResolveRTXRuntimePath(
    const char* path
);
void LoadMfgUnlockModuleIfPresent();
void RTLogWrite(
    const std::string& msg,
    bool flush
);
void RTLog(
    const std::string& msg
);
void RTLogFlushed(
    const std::string& msg
);
// ---------------------------------------------------------------------------
// 2026-11-02 P0：管线冷编耗时分项观测（实现见 PipelineDiag.cpp）
//
// 目的：把"改一个 SPV 就要等十几分钟"拆成 cache 加载 / compute 管线 / RT 管线
// 三部分，输出统一前缀 [PERF PIPE]，与既有 [PERF PC.*] 同一 stderr 出口，
// 便于对齐时间轴。只做观测，不改任何创建参数与返回码。
//
// compute 侧不需要逐个改调用点：TrackingHooks.h 已把 vkCreateComputePipelines
// 宏到 rtxTrackedCreateComputePipelines，Tracking.cpp 里那一处计时即覆盖全部
// 40+ 个创建点。
// ---------------------------------------------------------------------------
uint64_t OrionPipelineSpvHash(
    const void* data,
    size_t size
);
void OrionPipelineRegisterSpvName(
    uint64_t hash,
    const char* name
);
void OrionPipelineRegisterShaderModule(
    VkShaderModule module,
    uint64_t spvHash
);
const char* OrionPipelineSpvNameOfModule(
    VkShaderModule module
);
void OrionPipelineLogCreate(
    const char* kind,
    const char* spvName,
    bool cacheUsed,
    double milliseconds,
    VkResult result
);
void OrionPipelineLogCachePhase(
    const char* phase,
    double milliseconds,
    size_t bytes,
    const char* detail
);
VkResult OrionCreateRayTracingPipelineTimed(
    const char* label,
    uint32_t createInfoCount,
    const VkRayTracingPipelineCreateInfoKHR* pCreateInfos,
    VkPipelineCache pipelineCache,
    VkPipeline* pPipelines
);
void RTXTerminateHandler();
void RTXMarkPhase(const char* name);
std::string RTXHex(uint64_t value);
std::string RTXModuleOfPc(uintptr_t pc);
void RTXDumpCrashContext(const std::string& header);
LONG WINAPI RTXUnhandledExceptionFilter(EXCEPTION_POINTERS* info);
std::string ComputeRTPipelineCacheKey();
std::string GetPipelineCacheFilePath();
std::vector<std::string> FindNewestOtherPipelineCaches(
    const std::string& keepPath,
    uint32_t maxCount
);
bool LoadPipelineCacheFile(
    const std::string& path,
    std::vector<uint8_t>& outData);
void DeleteStalePipelineCaches();
VkPipelineCache getSharedPipelineCache();
void persistSharedPipelineCache();
void maybePersistSharedPipelineCache();
void releaseSharedPipelineCache();
uint32_t perfL10AblationRequestedStage();
bool perfL10AblationEnabled();
bool perfL10AblationAltMode(uint32_t request);
uint32_t perfL10AblationStage();
bool perfTimestampsEnabled();
bool sunShadowFarAbEnabled();
uint32_t sunShadowFarAbArm();
void RTXHeartbeatTick();
size_t sectionRebuildBatchCapOverride();
size_t effectiveSectionRebuildBatchCap();
double sectionRebuildTimeBudgetMs();
double smoothedGpuFrameMs();
bool waterAdaptiveEnabled();
bool perfL10SerLadderEnabled();
uint32_t perfL10SerLadderValue();
void updatePerfL10SerPathToggles();
void updatePerfL10SerToggle();
void samplePerfL10CpuFrame();
void finishPerfL10CpuFrame();
void destroyPerfL10GpuTimestampPool();
bool ensurePerfL10GpuTimestampPool();
float giRayMaxDistanceConfigured();
bool giRayMaxDistanceAbEnabled();
float giRayMaxDistanceEffective();
void logPerfL10GpuPassWindow(
    uint64_t frameSerial,
    uint32_t renderWidth,
    uint32_t renderHeight
);
void pollPerfL10GpuTimestampSlot(
    uint32_t slotIndex
);
void writePerfL10GpuBoundary(
    VkCommandBuffer commandBuffer,
    PerfL10GpuBoundary boundary
);
void setPerfL10GpuRenderSize(
    uint32_t renderWidth,
    uint32_t renderHeight
);
void beginPerfL10GpuFrame(
    VkCommandBuffer commandBuffer
);
void endPerfL10GpuFrame(
    VkCommandBuffer commandBuffer
);
void updatePerfL10AblationStage();
void perfL10ReportStall(const char* tag, double milliseconds);
void logPerfL10CpuPhaseWindow();
void perfL10CpuPhaseBeginFrame();
void perfL10CpuPhaseAdvance(uint32_t nextPhase);
void perfL10CpuPhaseEndFrame();
float waterSimUpdateSeconds();
bool volumetricCloudV3Requested();
VkDeviceSize sectionBuildPoolSlotBytes(VkDeviceSize requiredBytes);
void destroyPooledSectionBuildBuffers();
void releaseDeferredSectionBuildResources();
bool hdrOutputActive();
bool glassAlphaBlendEnvFlag(const char* name);
bool glassAlphaBlendEnvDisabled(const char* name);
bool glassAlphaBlendNeighborSearchEnabled();
bool glassAlphaBlendRadianceVolumeEnabled();
bool giHalfRotateEnabled();
int giNearRateLevel();
bool glossyRoughLodEnabled();
bool giNoPomEnabled();
bool giTemporalEnabled();
int directLightMode();
int sparseRatePercent();
int rtxdiReuseInactiveEnabled();
bool giHalfEnabled();
bool k1k2SplitEnabled();
bool rrTransparencyHintEnabled();
bool ResolveSLVulkanProxies();
VkResult FGDeviceWaitIdle(
    VkDevice device
);
float sparseSamplingRateEffective();
bool cloudFarBandSplitActive();
float cloudFarBandSplitDistance();
float effectiveCloudFarFadeDistance();
void populateCloudParams(float* params);
void populateSkyParams(float* params);
void populateSkyModelParams(float* params);
void populateGalaxyParams(float* params);
void populateGodRayParams(float* params);
void populateGroundMistParams(float* params);
void populateHighCloudParams(float* params);
void populateCloudMsParams(float* params);
void populateCloudMarchParams(float* params);
void populateCloudPhiParams(float* params);
void populateCloudLightParams(float* params);
void populateCloudShapeParams(float* params);
void populateCloudMoonParams(float* params);
void populateUeCloudParams(float* cloudA, float* cloudB);
void populateCloudShape2Params(float* params);
void populateCloudAmbientParams(float* params);
void populateCloudMsParams2(float* params);
void populateCloudLightParams2(float* params);
void populateCloudProfileParams(float* params);
void populateCloudDetailParams(float* params);
void populateFogExtrasParams(float* params);
void populateCloudTemporalParams(float* params);
void populateCloudAerialParams(float* params);
void populateCloudSkyHueParams(float* params);
void populateCloudPhysicsParams(float* params);
void populateWaterParams(float* params);
void populateRainWallParams(float* params);
float waterWindStrength();
void populateWaterSssParams(float* params);
void populateWaterWaveParams(float* params);
void populateWaterWaveShapeParams(float* params);
void populateWaterStyleParams(float* params);
bool pbrAtlasMipLodEnabled();
void ensurePbrAtlasSampler();
bool dhFarFieldMatrixAvailable();
bool dhTexturesAvailable();
VkSampler getLinearSampler();
void StreamlineLogCallback(sl::LogType type,const char* msg);
uint32_t findMemoryType(VkPhysicalDevice pd,uint32_t filter,VkMemoryPropertyFlags props);
uint32_t findHostUploadMemoryType(VkPhysicalDevice pd,uint32_t filter,VkMemoryPropertyFlags props);
void check(VkResult r,const char* s);
sl::float4x4 JOMLColumnMajorToSLRowMajor(
    const float m[16]
);
void createRTCommandPool();
bool rtxMemTypeIsDeviceLocal(uint32_t memoryTypeIndex);
bool rtxMemPriorityEnabled();
float rtxMemPriorityForAllocation(const VkMemoryAllocateInfo& allocateInfo);
VkResult rtxTrackedAllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* allocateInfo,
    const VkAllocationCallbacks* allocator,
    VkDeviceMemory* memory);
void rtxTrackedFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* allocator);
void rtxNoteBufferMemory(
    uint64_t usage,
    uint64_t bytes,
    VkDeviceMemory memory);
std::string rtxBufferChurnTopReport(size_t topN);
RtxMemAccountSnapshot rtxMemAccountSnapshot();
bool rtxMemObjTrackEnabled();
VkResult rtxTrackedCreateDescriptorPool(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDescriptorPool* pool);
VkResult rtxTrackedAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* allocateInfo,
    VkDescriptorSet* descriptorSets);
VkResult rtxTrackedCreateCommandPool(
    VkDevice device,
    const VkCommandPoolCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkCommandPool* commandPool);
VkResult rtxTrackedAllocateCommandBuffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* allocateInfo,
    VkCommandBuffer* commandBuffers);
VkResult rtxTrackedCreatePipelineLayout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkPipelineLayout* pipelineLayout);
VkResult rtxTrackedCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkShaderModule* shaderModule);
VkResult rtxTrackedCreateComputePipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* createInfos,
    const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines);
VkResult rtxTrackedCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkImageView* view);
VkResult rtxTrackedCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkSampler* sampler);
void rtxTrackedFreeCommandBuffers(
    VkDevice device,
    VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const VkCommandBuffer* commandBuffers);
VkResult rtxTrackedCreateFence(
    VkDevice device,
    const VkFenceCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkFence* fence);
VkResult rtxTrackedCreateSemaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkSemaphore* semaphore);
VkResult rtxTrackedCreateQueryPool(
    VkDevice device,
    const VkQueryPoolCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator,
    VkQueryPool* queryPool);
void createBuffer(VkDevice d,VkPhysicalDevice pd,VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags props,VkBuffer& buffer,VkDeviceMemory& memory,bool preferHostUploadMemory=false);
bool perfL10RayStatsEnabled();
VkDeviceSize perfL10RayStatsBytes();
void destroyPerfL10RayStats();
bool ensurePerfL10RayStats();
void logPerfL10RayStatsWindow();
void beginPerfL10RayStats(
    VkCommandBuffer commandBuffer
);
void endPerfL10RayStats(
    VkCommandBuffer commandBuffer
);
uint64_t makeSectionKey(
    int x,
    int y,
    int z
);
void destroyTextureUploadStaging();
void* acquireTextureUploadStaging(VkDeviceSize bytes);
double releaseHugeTextureUploadStaging();
void releaseTextureUploadScratch();
bool pbrSpriteMipEnabled();
void pbrDownsampleLevelBySprite(
    const uint8_t* prev, uint32_t prevW, uint32_t prevH,
    uint8_t* cur, uint32_t curW, uint32_t curH,
    uint32_t level,
    std::vector<uint8_t>& written);
void uploadPbrSpriteMippedToVRAM(
    void* pixels, uint32_t width, uint32_t height,
    VkImage& image, VkDeviceMemory& memory, VkImageView& view);
bool pbrBc7Enabled();
bool pbrBc7PartEnabled(bool isNormalTexture);
bool pbrBc7Supported();
void pbrBc7ParamsInit(bc7enc_compress_block_params& params);
void pbrEncodeBc7BlockRows(
    const uint8_t* rgba, uint32_t width, uint32_t height,
    uint32_t blocksX, uint8_t* dst,
    const bc7enc_compress_block_params& params,
    uint32_t blockRow0, uint32_t blockRow1);
void pbrEncodeBc7Level(
    const uint8_t* rgba, uint32_t width, uint32_t height,
    uint8_t* dst, const bc7enc_compress_block_params& params);
void uploadPbrBc7SpriteMipped(
    void* pixels, uint32_t width, uint32_t height,
    VkImage& image, VkDeviceMemory& memory, VkImageView& view);
void uploadTextureToVRAM(void* pixels, uint32_t width, uint32_t height, VkImage& image, VkDeviceMemory& memory, VkImageView& view);
VkDeviceAddress getBufferDeviceAddress(VkDevice d,VkBuffer b);
std::vector<uint8_t> loadBinaryFile(const char* path);
std::vector<uint8_t> loadSpv(const char* path);
VkShaderModule createShaderModule(VkDevice d,const std::vector<uint8_t>& code);
int tlasCullMode();
bool tlasKeepBoxEnabled();
float tlasNearGuardBlocks();
void mat4Mul(const float a[16], const float b[16], float out[16]);
bool mat4Invert(const float in[16], float out[16]);
void createSDRPostPipeline();
void updateSDRPostDescriptor();
void createHDRPostPipeline();
void createAeStateBuffers();
void createAutoExposurePipeline();
void createHDREncodePipeline();
void updateHDREncodeDescriptor();
void createHDR10OutputImage(uint32_t width, uint32_t height);
void destroyHDREncodeResources();
void destroyFGImage(
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout
);
void destroyFGUIImages();
void createFGImage(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view
);
void updateFGUIAlphaDescriptor();
void createFGUIAlphaPipeline();
bool ensureFGUIResources(
    uint32_t width,
    uint32_t height
);
void rtxCacheRealASFunctions(VkDevice device);
VkResult VKAPI_PTR rtxCountedCreateAccelerationStructureKHR(
    VkDevice device,
    const VkAccelerationStructureCreateInfoKHR* createInfo,
    const VkAllocationCallbacks* allocator,
    VkAccelerationStructureKHR* accelerationStructure);
void VKAPI_PTR rtxCountedDestroyAccelerationStructureKHR(
    VkDevice device,
    VkAccelerationStructureKHR accelerationStructure,
    const VkAllocationCallbacks* allocator);
PFN_vkCreateAccelerationStructureKHR getCreateAccelerationStructure();
PFN_vkDestroyAccelerationStructureKHR getDestroyAccelerationStructure();
PFN_vkGetAccelerationStructureBuildSizesKHR getGetAccelerationStructureBuildSizes();
PFN_vkGetAccelerationStructureDeviceAddressKHR getGetAccelerationStructureDeviceAddress();
void VKAPI_PTR rtxCountedCmdBuildAccelerationStructures(
    VkCommandBuffer commandBuffer,
    uint32_t infoCount,
    const VkAccelerationStructureBuildGeometryInfoKHR* buildInfos,
    const VkAccelerationStructureBuildRangeInfoKHR* const* buildRangeInfos);
PFN_vkCmdBuildAccelerationStructuresKHR getCmdBuildAccelerationStructures();
PFN_vkCmdTraceRaysKHR getCmdTraceRays();
void rtxCacheRealMicromapFunctions(VkDevice device);
VkResult VKAPI_PTR rtxCountedCreateMicromapEXT(
    VkDevice device,
    const VkMicromapCreateInfoEXT* createInfo,
    const VkAllocationCallbacks* allocator,
    VkMicromapEXT* micromap);
void VKAPI_PTR rtxCountedDestroyMicromapEXT(
    VkDevice device,
    VkMicromapEXT micromap,
    const VkAllocationCallbacks* allocator);
PFN_vkCreateMicromapEXT getCreateMicromap();
PFN_vkDestroyMicromapEXT getDestroyMicromap();
PFN_vkCmdBuildMicromapsEXT getCmdBuildMicromaps();
PFN_vkGetMicromapBuildSizesEXT getMicromapBuildSizes();
PFN_vkCmdPipelineBarrier2Omm getCmdPipelineBarrier2Omm();
void probeOpacityMicromapSupport();
void probeSerSupport();
uint32_t packSectionVertexColor(
    float r,
    float g,
    float b,
    float a);
SectionVertex makeSectionVertex(const Vertex& source);
void applyStainedGlassDyeToRTVertex(
    const uint8_t materialMarker,
    Vertex& vertex
);
void chunkLockNoteMax(
    std::atomic<uint64_t>& slot,
    uint64_t nanos);
void chunkCmpNoteMax(
    std::atomic<uint64_t>& slot,
    uint64_t nanos);
double chunkCmpMsSince(
    const std::chrono::steady_clock::time_point& from);
void chunkCmpReport();
bool chunkMemcmpTimed(
    const void* lhs,
    const void* rhs,
    size_t bytes,
    std::atomic<uint64_t>& slot);
bool publishDynamicLayerMotion(
    uint64_t layerKey,
    std::vector<float>& frameMotion,
    size_t vertexCount,
    bool verticesChanged);
bool requestDynamicRTASRebuild(bool changed);
void invalidateDynamicTextureBindingsForAtlasReload();
uint64_t chunkPayloadDigest64(
    const void* data,
    size_t bytes,
    uint64_t seed);
uint64_t hashVertexPayload(
    const std::vector<Vertex>& payload
);
bool sameVertexPayload(
    const std::vector<Vertex>& a,
    const std::vector<Vertex>& b
);
int32_t rtSignExtendBits(uint64_t raw, uint32_t bits);
int32_t rtFloorDivPow2(int32_t value, uint32_t shift);
uint64_t rtSectionBucketKeyOf(uint64_t classKey);
void rtLogSectionBucketStats();
int visibleSectionCullMode();
uint64_t probeGeometryRevision();
void markWorldGeometryDirty(bool terrainChanged = true);
bool captureQueueCursorEnabled();
void resetCapturedSectionsLocked();
void notifyGeometryCaptureWorker();
uint64_t geometryCaptureWakeMicros();
size_t geometryCapturePerWake();
bool captureTimeGateEnabled();
void geometryAppendSectionVertices(
    std::vector<Vertex>& destination,
    const std::vector<Vertex>& source
);
void geometryAppendSectionMotion(
    std::vector<float>& destination,
    const std::vector<float>* source,
    size_t vertexCount);
void packSectionSplitBucketsLocked(
    uint64_t spatialSectionKey,
    SectionGeometrySplit& split);
void geometryCaptureSectionLocked(
    uint64_t spatialSectionKey,
    SectionGeometrySplit& split
);
bool dirtySectionNearestOrderEnabled();
void geometryCaptureWorkerLoop();
void ensureGeometryCaptureWorker();
void stopGeometryCaptureWorker();
bool sameLocalLightPayload(
    const std::vector<LocalLightRecord>& a,
    const std::vector<LocalLightRecord>& b
);
int sharcEnvState();
bool sharcEnabled();
float sharcSceneScale();
uint32_t sharcEntryCount();
bool sharcUpdateEnabled();
bool sharcQueryEnabled();
bool sharcAbEnabled();
uint32_t sharcModeLane();
uint32_t localLightGridHash32(
    int32_t x,
    int32_t y,
    int32_t z
);
VkDeviceSize vliPoolBytesForCapacity(uint32_t capacity);
double vliLockHoldLogThresholdMs();
uint64_t vliChunkColumnKey(int32_t chunkX, int32_t chunkZ);
void vliChunkIndexAdd(int32_t sectionX, int32_t sectionZ, uint64_t sectionKey);
void vliChunkIndexRemove(int32_t sectionX, int32_t sectionZ, uint64_t sectionKey);
void vliHotRecordAndReport(uint64_t sectionKey, uint64_t ordinal);
bool vliHashGateEnabled();
uint64_t vliHashWords(const uint32_t* words, size_t count);
bool vliUploadHashUnchanged(uint64_t sectionKey, uint64_t hash);
void vliUploadHashStore(uint64_t sectionKey, uint64_t hash);
void vliUploadHashForget(uint64_t sectionKey);
VLISectionCpuPage* findVLISectionPageByIndexLocked(uint32_t pageIndex);
void createTestVertexBuffer();
void createBLAS();
void populateHpParams(CameraUBO* ubo);
void createCameraUBO();
void destroySectionOpacityMicromap(RTSectionAS& section);
void deferSectionOpacityMicromapBuildInputs(RTSectionAS& section);
bool queryVramBudget(VramBudgetSample& out);
int vramGovernorMode();
double vramGovernorHighWater();
double vramGovernorLowWater();
double vliBudgetFraction();
uint32_t vliPageCapacityLimit();
uint32_t sectionPoolChunkWasteLog2();
VkDeviceSize sectionMemoryPoolEffectiveKeepBytes();
size_t sectionMemoryPoolEffectiveKeepSlabs();
VkDeviceSize sectionMemoryPoolBucketBytesByIndex(uint32_t bucket);
uint32_t sectionMemoryPoolBucketIndex(VkDeviceSize need);
VkDeviceSize sectionMemoryPoolBucketBytes(VkDeviceSize need);
bool createSectionMemorySlab(
    VkDeviceSize need,
    VkDeviceSize requiredAlignment,
    uint32_t memoryTypeIndex,
    uint32_t& outSlabIndex);
bool takeSectionChunkFromSlabs(
    VkDeviceSize need,
    VkDeviceSize requiredAlignment,
    uint32_t memoryTypeIndex,
    VkDeviceMemory& outMemory,
    VkDeviceSize& outChunkBase,
    VkDeviceSize& outChunkBytes,
    uint32_t& outSlabIndex,
    uint32_t& outChunkIndex,
    uint32_t maxChunkWasteLog2);
bool acquireSectionMemory(
    VkDeviceSize need,
    VkDeviceSize requiredAlignment,
    uint32_t memoryTypeIndex,
    VkDeviceMemory& outMemory,
    VkDeviceSize& outChunkBase,
    VkDeviceSize& outChunkBytes,
    uint32_t& outSlabIndex,
    uint32_t& outChunkIndex,
    uint32_t maxChunkWasteLog2 = 32u);
void releaseSectionMemory(
    uint32_t slabIndex,
    uint32_t chunkIndex);
double trimSectionMemoryPool();
void updateVramGovernor();
void logVramGovernor();
bool vramProbeEnabled();
int vramProbeWarnPercent();
void tickVramProbe();
bool frameSkipLogEnabled();
void logFrameSkip(const char* reason, const std::string& detail);
void maybePrintFrameCounters();
void logFrameEntryHeartbeat();
void logFrameDoneHeartbeat();
bool createPooledBuffer(
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags props,
    bool preferHostUploadMemory,
    VkBuffer& buffer,
    OmmBufferBinding& binding);
void destroyPooledBuffer(
    VkBuffer& buffer,
    OmmBufferBinding& binding);
void destroySectionMemoryPool();
void destroySectionAS(RTSectionAS& section);
void retireSectionAS(RTSectionAS& section);
void parkTlasStorage();
void parkTlasScratch();
void parkTlasInstances();
void releaseRetirementSlot(
    uint32_t slotIndex
);
void reapSignaledRetirements();
void releaseAllRetirementSlots();
void ensureSectionRetirementFences();
void destroySectionRetirementFences();
uint32_t acquireRetirementSlot();
bool createSectionOpacityMicromap(
    RTSectionAS& section,
    const OmmCutoutInput& input
);
bool sectionBLASRefitEnabled();
bool sectionIndexedTerrainEnvEnabled();
VkDeviceAddress sectionQuadIndexAddress();
bool sectionUsesPackedVertices(
    uint64_t sectionKey,
    bool indexedGeometry,
    size_t vertexCount);
bool sectionUsesIndexedGeometry(
    uint64_t sectionKey,
    size_t vertexCount);
VkDeviceSize createSectionBLAS(
    RTSectionAS& section,
    uint64_t sectionKey,
    const std::vector<Vertex>& vertices,
    bool opaqueGeometry,
    const OmmCutoutInput* ommInput,
    const std::vector<float>* vertexMotion = nullptr,
    bool allowUpdate = false
);
void recordSectionBLASBuild(
    VkCommandBuffer cmd,
    RTSectionAS& section,
    VkDeviceAddress scratchAddress
);
void destroySectionVertexAddressBuffer();
void updateSectionVertexAddressDescriptor();
void createSectionVertexAddressTable();
void writeSectionVertexAddressSlot(
    const RTSectionAS& section
);
bool ensureSectionVertexAddressSlot(
    RTSectionAS& section
);
void createTLAS();
VkDescriptorSetLayout createRTDescriptorSetLayout();
void destroyHistoryImage(
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout);
void updateRRTransparencyHintRayDescriptor();
void createRRGuideImage(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout);
void updateRRGBufferDescriptors();

// 【2026-11-02】屏幕空间降级链（SSR + SSPT）—— 实现见 cpp/backend/ScreenSpace.cpp
bool ssHybridSsptEnabled();
bool ssHybridSsrEnabled();
bool ssHybridEnabled();
void createSSHybridResources(
    uint32_t width,
    uint32_t height);
void updateSSHybridDescriptors();
void ensureSSHybridPipeline();
void recordSSHybridPass(
    VkCommandBuffer commandBuffer,
    uint32_t width,
    uint32_t height);
void recordSSHybridHistoryCopy(
    VkCommandBuffer commandBuffer);
void destroySSHybridResources();
void writeStableRadianceDisplayBinding(
    VkDescriptorSet set,
    const char* label);
void createRRGBufferImages(
    uint32_t width,
    uint32_t height);
void updateGiHalfRayDescriptor();
void createGiHalfImage(uint32_t width, uint32_t height);
void createGiTemporalImages(uint32_t width, uint32_t height);
void prepareGiTemporalHistoryImages(VkCommandBuffer cmd);
void prepareGlassAlphaBlendImages(VkCommandBuffer cmd);
void createGlassBlueNoiseBuffer();
void destroySparseActiveMaskImage();
bool ensureSparseActiveMaskImage(uint32_t renderWidth, uint32_t renderHeight);
bool ensureSparseActiveMaskPipeline();
void updateSparseActiveMaskDescriptors();
void recordSparseActiveMask(
    VkCommandBuffer cmd,
    uint32_t renderWidth,
    uint32_t renderHeight);
void updateGlassAlphaBlendRayDescriptors();
void updateGlassAlphaBlendPassDescriptors();
void ensureGlassAlphaBlendPipelines();
void createGlassAlphaBlendImages(uint32_t width, uint32_t height);
void recordGlassAlphaBlendPrepare(VkCommandBuffer cmd);
void recordGlassAlphaBlend(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height);
void createPlaceholder3DImage(
    VkFormat format,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view);
void clearGiHalfImageIfNeeded(VkCommandBuffer cmd);
void createRTOutputImage(uint32_t width,uint32_t height);
void createDLSSDepthImage(
    uint32_t width,
    uint32_t height);
void createMotionImage(uint32_t width,uint32_t height);
void destroyBloomResources();
void createBloomMipImage(BloomMip& mip, uint32_t width, uint32_t height);
void createBloomResources(uint32_t outputWidth, uint32_t outputHeight);
void recordBloom(VkCommandBuffer cmd);
void createDLSSOutputImage(uint32_t width,uint32_t height);
void destroyLocalLightGpuBuffer();
void destroyLocalLightGridGpuBuffer();
int32_t localLightGridFloorDiv32(
    int32_t value
);
void ensureDynamicLightBuffer();
void updateDynamicLightDescriptor();
void recordDynamicLightUpload(VkCommandBuffer cmd);
void updateLocalLightDescriptor();
void updateLocalLightGridDescriptor();
void updateCloudLocalLightDescriptors();
void destroySharcBuffers();
void ensureSharcBuffers();
void updateSharcDescriptors();
bool sharcProbeEnabled();
void sharcProbeOneShotSubmit(
    void (*record)(VkCommandBuffer),
    const char* label);
void sharcProbeRecordMagic(VkCommandBuffer commandBuffer);
void sharcProbeRecordReadBack(VkCommandBuffer commandBuffer);
void sharcProbeReadBackAndReport();
void ensureSharcStaging();
void sharcProbeTick();
void sharcCacheBarrier(
    VkCommandBuffer cmd,
    VkPipelineStageFlags srcStage,
    VkPipelineStageFlags dstStage);
void recordSharcZeroFillIfNeeded(VkCommandBuffer cmd);
void ensureSharcResolvePipeline();
void destroySharcUpdatePipeline();
void ensureSharcUpdatePipeline();
void recordSharcUpdate(VkCommandBuffer cmd, uint32_t width, uint32_t height);
void recordSharcResolve(VkCommandBuffer cmd);
void destroyGiHalfPipeline();
void ensureGiHalfPipeline();
void recordGiHalfPass(VkCommandBuffer cmd, uint32_t width, uint32_t height);
void sharcStatsRecordReadBack(VkCommandBuffer commandBuffer);
void sharcStatsTick();
void destroyVPTDirectHistoryBuffers();
void updateVPTDirectHistoryDescriptors();
void createVPTDirectHistoryBuffers(uint32_t width, uint32_t height);
void prepareVPTDirectHistoryBuffers(VkCommandBuffer cmd);
void rebuildLocalLightGpuBuffer();
// 2026-11-06 #13 退役：三者为 bisect 时代描述符环残留（publish 零调用点），
// Vli.cpp 里原体外层 #if 0，壳恒禁用；勿再调用或复用 env RTX_ATLAS_RING。
bool rtDescriptorRingEnabledByEnv();
void createRtDescriptorRing();
bool rtDescriptorRingPublish();
void createRayTracingDescriptorPoolAndSet();
VkDeviceSize poolByteSizeForCapacity(
    uint32_t capacity,
    uint32_t poolIndex
);
void updateVLIDescriptors();
void createVLIPoolBuffers();
void destroyVLIPoolBuffers();
void createVLILightResources();
void growVLIPageCapacityLocked(uint32_t requiredPages);
uint32_t vliHashSectionCoord(
    int32_t x,
    int32_t y,
    int32_t z
);
uint32_t vliFindTableSlot(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    bool& found
);
void markVLINeighborhoodDirty(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ
);
std::vector<uint32_t> buildVPTEmitterIndexWords(
    const std::vector<uint32_t>& sourceWords,
    uint32_t revision
);
std::vector<uint32_t> buildVPTEmitterIndexWordsFromLocalLights(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    const std::vector<LocalLightRecord>& lights,
    uint32_t revision
);
std::vector<uint32_t> buildVPTEmitterIndexWordsForSection(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    const std::vector<uint32_t>& sourceWords,
    const std::vector<LocalLightRecord>& localLights,
    uint32_t revision
);
std::vector<uint32_t> buildVPTEmitterAliasWords(
    const std::vector<uint32_t>& emitterWords
);
float vptDistanceSquaredToSectionBounds(
    const VPTRelatedEmitterGPU& emitter,
    const VLISectionCpuPage& receiver
);
uint32_t packVPTSourceDirectoryMetadata(
    uint32_t emitterCount,
    int32_t offsetX,
    int32_t offsetY,
    int32_t offsetZ
);
void rebuildVPTSourceDirectoryPageLocked(VLISectionCpuPage& receiver);
void rebuildVPTCandidatePageLocked(VLISectionCpuPage& receiver);
void rebuildVPTCandidateNeighborhoodLocked(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ
);
bool applyVLISectionVoxelsLocked(
    uint64_t sectionKey,
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    std::vector<uint32_t>& incoming,
    std::vector<uint32_t>& emitterIndexWords,
    std::vector<uint32_t>& emitterAliasWords,
    // 2026-11-06e：非空时只把受影响的邻域页索引收进这个集合，由调用方在批量
    // 结束后统一重建一次（按页去重）；为空时保持旧行为（立刻重建本次邻域）。
    std::unordered_set<uint32_t>* deferredNeighborhoodPages = nullptr
);
void refreshVPTEmitterSectionFromLocalLights(
    int32_t sectionX,
    int32_t sectionY,
    int32_t sectionZ,
    const std::vector<LocalLightRecord>& localLights
);
double vliPendingSectionBudgetMs();
uint32_t vliPendingSectionLimit();
void beginVliUploadArena();
void appendVliUpload(
    std::vector<VliUploadRef>& refs,
    uint32_t pageIndex,
    const std::vector<uint32_t>& words);
void recordVLIUpdates(VkCommandBuffer cmd);
VkFormat nrdFormatToVkFormat(nrd::Format format);
void nrdDestroyImage(
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout);
bool nrdCreateImage2D(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout,
    const char* what);
VkSampler nrdCreateImmutableSampler(bool linear, const char* what);
void nrdTransitionImagesToGeneral();
bool createNRDDescriptorSetLayout();
bool createNRDPoolTextures();
bool createNRDPipelines();
bool createNRDAppTextures();
bool updateNRDDescriptors();
bool createNRDConstantBuffer();
bool createNRDDescriptorSet();
void destroyNRDResources();
bool createNRDResources(uint32_t width, uint32_t height);
void ensureNRDResources(uint32_t width, uint32_t height);
void createNRDCompositePipeline();
void destroyNRDCompositePipeline();
void nrdFillColumnMajor(const sl::float4x4& m, float out[16]);
void nrdBuildCommonSettings(
    nrd::CommonSettings& settings,
    uint32_t width,
    uint32_t height);
void nrdBuildReblurSettings(nrd::ReblurSettings& settings);
void nrdBuildRelaxSettings(nrd::RelaxSettings& settings);
bool createDiConfidenceResources(uint32_t width, uint32_t height);
bool createDiConfidencePipeline();
void updateDiConfidenceDescriptors();
VkImageView recordDiConfidence(VkCommandBuffer cmd, uint32_t width, uint32_t height);
VkImageView nrdResolveResourceView(const nrd::ResourceDesc& resource);
void nrdImageBarrier(
    VkCommandBuffer cmd,
    VkImage image,
    VkAccessFlags srcAccess,
    VkAccessFlags dstAccess);
bool createDiChainImage(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    const char* name,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout,
    VkImageMemoryBarrier* barriers,
    uint32_t& barrierCount);
void createDiTemporalPipelines();
void createDirectAtrousPipeline();
void updateDirectAtrousDescriptors();
void recordDirectAtrous(
    VkCommandBuffer cmd,
    uint32_t width,
    uint32_t height);
void recordNRDLocalDenoiser(VkCommandBuffer cmd, uint32_t width, uint32_t height);
void createVLIComputePipeline();
void destroyVLIResources();
void updateRTOutputDescriptor();
void updateWaterSimulationRayDescriptor();
void updateWaterCausticRayDescriptor();
void updateWaterDetailNormalRayDescriptor();
void createWaterDetailNormalResources();
void destroyWaterDetailNormalResources();
void createEntityPbrFallbackResources();
void destroyEntityPbrFallbackResources();
uint32_t cloudNoiseHash(uint32_t value);
float cloudNoiseUnit(uint32_t x, uint32_t y, uint32_t z, uint32_t seed);
float cloudNoiseFade(float value);
float cloudNoiseLerp(float a, float b, float t);
uint32_t cloudNoiseWrap(int value, int period);
float cloudValueNoise(float x, float y, float z, int period, uint32_t seed);
float cloudFbmNoise(float x, float y, float z, uint32_t seed);
float cloudCellularNoise(float x, float y, float z, int period, uint32_t seed);
uint8_t cloudNoiseByte(float value);
void updateCloudNoiseRayDescriptors();
void createCloudNoiseResources();
void createCloudBaseNoiseResources();
void destroyCloudBaseNoiseResources();
std::vector<uint8_t> readCloudV3Asset(const char* relativePath, size_t expectedBytes);
VkSampler createCloudV3Sampler();
void uploadCloudV3Image(
    VkImageType imageType,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    const std::vector<uint8_t>& pixels,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkSampler& sampler);
void createCloudV3Resources();
void cloudV4DebugLog(const char* text);
int cloudModelRequested();
void createCloudV4Resources();
void destroyCloudV4Resources();
void destroyCloudV3Resources();
void destroyCloudNoiseResources();
void createSunShadowResources();
bool sunShadowForceAlways();
bool sunShadowSignatureChanged(
    float texelWorld,
    float& builtPhase,
    uint64_t& builtRevision,
    float builtCenter[3]
);
void recordSunShadowMap(VkCommandBuffer cmd);
void destroySunShadowResources();
bool sunShadowFarEnabled();
bool localLightRebuildEnabled();
float sunShadowFarExtent();
void createSunShadowFarResources();
void recordSunShadowFarMap(VkCommandBuffer cmd);
void destroySunShadowFarResources();
void createTerrainHeightResources();
void createRRGuideRelaxResources();
void recordRRGuideRelax(VkCommandBuffer cmd);
void destroyRRGuideRelaxResources();
void recordTerrainHeightMap(VkCommandBuffer cmd);
void destroyTerrainHeightResources();
bool volumetricCloudBindingsMatchCurrentSizes(
    uint32_t width,
    uint32_t height);
VkSampler volDepthSampler();
void createVolumetricCloudResources(uint32_t width, uint32_t height);
void createVolumetricCloudPipeline(uint32_t width, uint32_t height);
void setCloudCompositeTarget(VkImageView target);
uint64_t cloudSpvStampOf(const char* relative);
uint64_t cloudSpvStampAll();
void hotReloadVolumetricCloudPipelines();
void maybeHotReloadVolumetricCloudShaders();
void recordVolumetricClouds(VkCommandBuffer cmd);
void recordCloudComposite(VkCommandBuffer cmd);
uint32_t volRestirEnabled();
uint32_t volDiagMode();
void createVolumeRestir3DImage(
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    VkFormat format,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view);
void destroyVolumeRestirResources();
void createVolumeRestirResources(uint32_t renderWidth, uint32_t renderHeight);
VkSampler volRadianceSampler();
void createVolumeRestirPipeline();
void vkGuardUpdate(
    uint32_t count,
    VkWriteDescriptorSet* writes,
    const char* tag);
void createVolumeRestirChainPipelines();
void recordVolumeRestirChain(VkCommandBuffer cmd);
void createVolumeRestirOutputPipelines();
void createVolumeRestirVisibilityPipeline();
void recordVolumeRestirVisibility(VkCommandBuffer cmd);
void recordVolumeRestirIntegrate(VkCommandBuffer cmd);
void recordVolumeRestirScreenComposite(VkCommandBuffer cmd);
void recordVolumeRestir(VkCommandBuffer cmd, uint32_t renderWidth, uint32_t renderHeight);
void logDeviceFaultDiagnostics(const char* where);
void destroyVolumetricCloudResources();
void updateSkyAtmosphereStarMapDescriptor();
void updateSkyAtmosphereEndSkyAtlasDescriptor();
void updateSkyRayDescriptors();
void destroyBrunetonResources();
void createBrunetonLutImage(BrunetonLutImage& dst, VkImageType type,
    uint32_t width, uint32_t height, uint32_t depth, VkFormat format);
void destroyBrunetonLutImage(BrunetonLutImage& img);
void releaseBrunetonPrecomputeOnlyLuts();
void createBrunetonResources();
void recordBrunetonPrecompute(VkCommandBuffer cmd);
void executeBrunetonPrecompute();
void createSkyAtmosphereResources();
void createSkyAtmospherePipeline();
void recordSkyAtmosphere(VkCommandBuffer cmd);
void destroySkyAtmosphereResources();
void createWaterCausticResources();
void createWaterCausticPipeline();
void populateWaterFftParams(float* params);
void updateWaterFftRayDescriptor();
void waterFftPassBarrier(VkCommandBuffer cmd);
void waterFftDispatch(
    VkCommandBuffer cmd,
    int32_t sourceLayer,
    int32_t destinationLayer,
    int32_t stageLength,
    int32_t mode,
    int32_t axis,
    int32_t detailSourceLayer = -1,
    float texelWorldSizeOverride = -1.0f,
    float detailTexelWorldSize = 0.0f,
    float detailVisualScale = 0.0f,
    float minWaveRadius = 0.0f);
void updateWaterFftDescriptors();
void createWaterFftResources();
void recordWaterFftUpdate(VkCommandBuffer cmd);
void destroyWaterFftResources();
void updateWaterCausticMapRayDescriptor();
void destroyWaterCausticMapResources();
void createWaterCausticMapResources();
float waterCausticMapHz();
bool waterCausticMapSkipThisFrame();
void recordWaterCausticMap(VkCommandBuffer cmd);
void createWaterSimulationResources();
void createWaterSimulationPipeline();
void recordWaterSimulation(VkCommandBuffer cmd);
void recordWaterCaustics(VkCommandBuffer cmd);
void destroyWaterCausticResources();
void destroyWaterSimulationResources();
void createRayTracingSBT();
uint32_t QueryDisplayRefreshHz();
uint32_t ResolveFgBaseFrameLimitHz();
bool ApplyReflexMode(
    int mode);
void RefreshFgBaseFrameLimit();
bool SetCurrentPCLMarker(
    sl::PCLMarker marker);
bool MarkRenderSubmitStart();
bool MarkRenderSubmitEnd();
void SampleSlEstimatedVram();
bool MarkPresentStart();
bool MarkPresentEnd();
bool BeginStreamlineFrame();
void initStreamlineInternal();
void applyHdrStaticMetadata(VkDevice device, VkSwapchainKHR swapchain);
void MaybeReleaseFgResourcesOnIdle();
uint64_t leakProbeCommitBytes();
bool leakProbeEnabled();
void leakProbeFrameBegin();
void leakProbeFrameEnd();
uint32_t deviceInitStopStep();
bool rtPipelineCacheDisabled();
bool rtPipelineDisableOptimization();
bool deviceInitMilestone(const char* name);
uint32_t rtPipeRecycleSeconds();
bool recreateMainRtPipeline();
void recycleMainRtPipeline();
// 2026-11-02 P1：SPV 热重载轮询（ORION_RT_PIPE_HOT_RELOAD=1）。
// 检测 run/shaders 下 8 个 RT 管线 SPV 的 mtime/size 变化，稳定后重建主 RT 管线
// 与 SBT（失败回滚）。不改变任何渲染语义；不设环境变量时每帧只做一次 bool 判断。
void pollRtPipelineHotReload();
// 2026-11-02 P1.5：主 RT 管线的分片异步重建（VK_KHR_deferred_host_operations）。
// 设备由 renderpearl 创建，扩展可能未启用，故先探测函数指针；
// 不可用时 supported() 返回 false，调用方回退到同步的 recreateMainRtPipeline()。
bool asyncRtPipelineRebuildSupported();
bool beginAsyncRtPipelineRebuild(const char* reason);
bool asyncRtPipelineRebuildActive();
void pollAsyncRtPipelineRebuild();
jobjectArray
FGMakeJavaStringArray(
    JNIEnv* env,
    const std::vector<std::string>& values
);
bool sectionRebuildFenceGateEnabled();
uint64_t packSectionKey(int x, int y, int z, int layerId);
void clearVLISections();
void eraseVLITableSlot(uint32_t eraseSlot);
bool evictFarthestVLIPageLocked(uint32_t& outPageIndex);
bool uploadRawMoveGateEnabled();
float opaqueShellOffsetMeters();
bool chunkRawStoreEnabled();
bool chunkRawPayloadUnchangedLocked(
    uint64_t chunkKey,
    const uint8_t* rawMemory,
    size_t rawPayloadBytes,
    uint64_t rawDigest);
bool chunkMeshPayloadUnchangedLocked(
    uint64_t chunkKey,
    const std::vector<Vertex>& repackedVertices,
    uint64_t meshDigest);
void chunkStoreRawPayloadLocked(
    uint64_t chunkKey,
    std::vector<uint8_t>& localRaw,
    const uint8_t* rawMemory,
    size_t rawPayloadBytes);
void chunkStoreUploadDigestLocked(
    uint64_t chunkKey,
    size_t meshVertices,
    uint64_t rawDigest,
    size_t rawPayloadBytes,
    uint64_t meshDigest);
uint16_t voxyU16Le(const uint8_t* bytes);
uint64_t mixVoxyLodKey(uint64_t value);
uint64_t allocateVoxyLodSpatialKeyLocked(uint64_t sourceKey);
bool copyVoxyLodArray(JNIEnv* env, jbyteArray array, std::vector<uint8_t>& output);
VoxyQuadFace voxyQuadFaceOf(const uint8_t* quadBytes);
float voxyQuadFaceTint(const uint8_t* quadBytes);
void appendVoxyLodQuads(
    const std::vector<uint8_t>& input,
    bool transparentPass,
    int originX,
    int originY,
    int originZ,
    int width,
    std::vector<Vertex>& opaque,
    std::vector<Vertex>& alpha);
uint64_t allocateDhLodSpatialKeyLocked(uint64_t sectionKey);
bool isDhLodSpatialKey(uint64_t spatialKey);
void refreshDhLodSpatialSnapshotLocked();
void appendDhLodQuads(
    const std::vector<uint8_t>& input,
    int originX,
    int originY,
    int originZ,
    int width,
    std::vector<Vertex>& opaque,
    std::vector<Vertex>& alpha);
void clearDhLodSpatialTablesLocked();
void destroyVoxyLodAtlasResources();
void ensureDhTextureDescriptors();
void ensureSunShadowFarDescriptor();
void ensureSunShadowNearDescriptor();
void recordDhTextureBarrier(VkCommandBuffer cmd);
void applyRenderThreadPriorityOnce();
int32_t sectionKeyColumnX(uint64_t spatialKey);
int32_t sectionKeyColumnZ(uint64_t spatialKey);
uint32_t sectionRoleOf(const RTSectionAS& section);
void writeSectionRoleSlot(const RTSectionAS& section);
void rebuildLodCoverage(
    const std::vector<RTSectionAS*>& orderedSections
);
void logNativeMemoryReport();
float vramEvictKeepRadius();
size_t vramEvictPerPass();
size_t vramRehydratePerPass();
void vramSectionCenterFromSpatialKey(
    uint64_t spatialKey,
    float& outX,
    float& outY,
    float& outZ
);
bool vramWorldHeightSection(uint64_t spatialKey);
int64_t geometryPreLoopLockWaitMs();
bool rebuildWorldGeometry(
    size_t maxSectionUpdates = std::numeric_limits<size_t>::max()
);
float DLSSHaltonValue(
    uint32_t index,
    uint32_t base
);
void GetDLSSFrameJitter(
    uint32_t frameIndex,
    float& outX,
    float& outY
);

// --------------------- 全局变量 extern 声明 ---------------------




extern "C" IMAGE_DOS_HEADER __ImageBase;


extern std::ofstream rtLogFile;


extern std::mutex g_rtLogMutex;
extern std::atomic<uint64_t> g_rtxPhaseSeq;
extern std::atomic<uint64_t> g_rtxPhaseStamp[RTX_PHASE_RING];
extern std::atomic<const char*> g_rtxPhaseName[RTX_PHASE_RING];
extern std::atomic<const char*> g_rtxPhaseCurrent;
extern std::atomic<uint64_t> g_rtxLastFrameSerial;
extern std::atomic<uint64_t> g_rtxDetailSectionKey;
extern std::atomic<uint64_t> g_rtxDetailSectionVerts;
extern std::atomic<uint64_t> g_rtxDetailInstances;
extern LPTOP_LEVEL_EXCEPTION_FILTER g_rtxPreviousExceptionFilter;
extern bool g_isInWorld;
inline constexpr const uint32_t MAX_TEXTURES = 4096;
extern uint32_t g_currentAlbedoIndex;
extern uint32_t g_currentNormalIndex;
extern uint32_t g_currentSpecularIndex;
extern VkImage g_normalAtlasImage;
extern VkDeviceMemory g_normalAtlasMemory;
extern VkImageView g_normalAtlasView;
extern VkImage g_specularAtlasImage;
extern VkDeviceMemory g_specularAtlasMemory;
extern VkImageView g_specularAtlasView;
extern bool g_pbrAtlasesDirty;
extern std::vector<VTRTexture> g_albedoTextures;
extern std::vector<VTRTexture> g_normalTextures;
extern std::vector<VTRTexture> g_specularTextures;
extern VkInstance g_instance;
extern VkPhysicalDevice g_physicalDevice;
extern VkDevice g_device;
extern VkQueue g_queue;
extern uint32_t g_queueFamily;
extern VkPipelineCache g_pipelineCache;
inline constexpr const char* const RT_PIPELINE_SPV_FILES[] = {
    "shaders\\rt_test.rgen.spv",
    "shaders\\rt_test_ser.rgen.spv",
    "shaders\\rt_test_k1.rgen.spv",
    "shaders\\rt_test_k1_ser.rgen.spv",
    "shaders\\rt_test_k2.rgen.spv",
    "shaders\\rt_test_k2_ser.rgen.spv",
    // 2026-11-05 设备丢失二分变体（ORION_K2_VARIANT 门控加载，仅诊断用）
    "shaders\\rt_test_k2_nocall.rgen.spv",
    "shaders\\rt_test_k2_inline.rgen.spv",
    // ③ K2-D/K2-I 直射/间接拆分（默认加载，ORION_K2_SPLIT=0 回退）
    "shaders\\rt_test_k2d.rgen.spv",
    "shaders\\rt_test_k2d_ser.rgen.spv",
    "shaders\\rt_test_k2i.rgen.spv",
    "shaders\\rt_test_k2i_ser.rgen.spv",
    // D1：材质层 callable（gen_material_callable.py 从 rt_test.rchit 生成）
    "shaders\\rt_test_material.spv",
    "shaders\\rt_test.rchit.spv",
    "shaders\\rt_test_k1.rchit.spv",
    "shaders\\rt_test.rmiss.spv",
    "shaders\\rt_test.rahit.spv",
    "shaders\\rt_test_translucent.rchit.spv",
    "shaders\\rt_test_translucent.rahit.spv",
    "shaders\\rr_guide_relax.comp.spv",
};
// 2026-11-02 P0（冷编分解）：原值 64 MiB 小于实测缓存体积 —— 日志里
// [PERF PC.1] pipeline cache saved bytes=96642292，下一次启动就命中
// "oversized, starting empty"，等于**什么都没改也要全量冷编**。
// 抬到 256 MiB 只是把"整体丢弃"的触发线推到实际体积之上；它仍是保护，
// 不是目标值。真要判断缓存该多大，看 [PERF PIPE] kind=load/save 的 ms 与 bytes。
inline constexpr const size_t PIPELINE_CACHE_MAX_RESTORE_BYTES =
    256u * 1024u * 1024u;



inline constexpr const uint32_t PIPELINE_CACHE_KEEP_SEEDS = 3u;
// 与 MAX_RESTORE 同口径：seed 预算低于单个缓存体积时，FindNewestOtherPipelineCaches
// 会把唯一有用的那份 96 MiB 种子直接跳过（budget 不够），退化成空缓存启动。
inline constexpr const size_t PIPELINE_CACHE_SEED_BUDGET_BYTES =
    256u * 1024u * 1024u;
extern size_t g_pipelineCachePersistedBytes;
extern uint32_t g_pipelineCachePersistCountdown;
inline constexpr const uint32_t PIPELINE_CACHE_PERSIST_INTERVAL = 1800;
extern std::vector<uint32_t> g_perfL10AblationAltStages;
inline constexpr const char* const PERF_L10_ABLATION_STAGE_NAMES
    [PERF_L10_ABLATION_STAGE_COUNT] =
{
    "base",
    "noRtao",
    "noGlossy",
    "noGi",
    "noRtaoGlossyGi",
    "noG2bGuide",
    "noEmitterScan",
    "noG2bEmitter",
    "gi1Bounce",
    "ao2of4",
    "shadow1of2",
    "rrGi",
    "march1",
    "tierHalf",
    "rrOff",
    "spare15",
    "noVoxelDda",
    "noMatEval",
    "noPom",
    "noPomSelfShadow",
    "noSpecular",
    "noEmissive",
    "noNormalResample",
    "noAlbedoFetch",
    "noLabPbrMath",
    "glossyTMax48",
    "glossyTMax96",
    "secMipBias2",
    "noMatMainOnly",
    "noMatNoGlossy",
    "noMatNoGi",
    "secOmm4State",
    "glossyBudgetOff",
    "emissiveFetch",
    "voxelDdaGi",
    "glossyTMax192"
};
extern uint32_t g_perfL10AblationStage;
extern VkQueryPool g_perfL10GpuTimestampPool;
extern double g_perfL10TimestampPeriodNs;
extern uint64_t g_perfL10FrameSerial;
extern size_t g_sectionRebuildBatchCap;
extern bool g_sectionRebuildInterrupted;
extern uint64_t g_sectionRebuildContinueFrame;
extern uint64_t g_nextSectionRebuildFrame;
extern uint64_t g_firstPendingSectionFrame;
extern uint64_t g_lastObservedSectionRevision;
extern uint64_t g_geometryRebuildCallTotal;
extern uint64_t g_geometryFenceDeferredCount;
extern uint64_t g_geometryCountersLastCaptured;
extern uint64_t g_geometryCountersLastCalls;
extern double g_geometryMaxSectionMs;
extern double g_geometryMaxPreLoopMs;
extern double g_geometryMaxBuildLoopMs;
extern double g_geometryMaxTailMs;
extern PerfL10GpuTimestampSlot
    g_perfL10GpuTimestampSlots[PERF_L10_GPU_TIMESTAMP_RING_SIZE];
extern int32_t g_perfL10GpuActiveSlot;
extern PerfL10GpuPassWindow g_perfL10GpuPassWindow;
extern std::atomic<double> g_sectionRebuildBudgetMirrorMs;
extern uint32_t g_perfL10SerToggle;
extern uint32_t g_perfL10SerVisToggle;
extern uint32_t g_perfL10SerRadToggle;
extern uint32_t g_perfL10SerPrimToggle;
extern uint32_t g_perfL10SerPathsPhase;
extern uint32_t g_perfL10SerMode;
extern uint32_t g_perfL10RtaoSkipToggle;
extern std::chrono::steady_clock::time_point g_perfL10CpuFramePrev;
extern bool g_perfL10CpuFrameHavePrev;
extern std::chrono::steady_clock::time_point g_perfL10CpuWorkStart;
extern PerfL10CpuFrameWindow g_perfL10CpuFrameWindow;


extern uint32_t g_perfL10GpuWindowsLogged;
extern std::atomic<float> g_perfL10SppForLog;
extern std::atomic<float> g_perfL10GiMaxForLog;
extern std::atomic<float> g_perfL10PomForLog;


extern uint32_t g_perfL10BoundaryMask;
extern uint64_t g_perfL10NextTexturePublishFrame;
extern PerfL10CpuPhaseWindow g_perfL10CpuPhaseWindow;
extern uint32_t g_perfL10CpuPhaseCursor;
extern std::chrono::steady_clock::time_point g_perfL10CpuPhaseStart;


extern VkCommandPool g_rtCommandPool;
extern uint32_t g_sbtBaseAlignment;
extern VkBuffer g_sbtBuffer;
extern VkBuffer g_cameraUboBuffer;
extern VkDeviceMemory g_cameraUboMemory;
extern void* g_cameraUboMapped;
extern CameraUBO g_cameraUboHost;
extern sl::float4x4 g_prevCameraViewToWorld;
extern sl::float4x4 g_prevCameraViewToClip;
extern bool g_hasPrevCameraMatrices;
extern VkDeviceMemory g_sbtMemory;
extern VkDeviceSize g_sbtSize;
extern VkDeviceAddress g_sbtDeviceAddress;
extern uint32_t g_raygenOffset, g_hitOffset, g_missOffset, g_handleSizeAligned;
extern VkStridedDeviceAddressRegionKHR g_raygenRegion;
extern VkStridedDeviceAddressRegionKHR g_missRegion;
extern VkStridedDeviceAddressRegionKHR g_hitRegion;
extern VkStridedDeviceAddressRegionKHR g_callableRegion;
extern VkImage g_waterSimImage;
extern VkDeviceMemory g_waterSimMemory;
extern VkImageView g_waterSimView;
extern VkSampler g_waterSimSampler;
extern VkDescriptorSetLayout g_waterSimDescriptorSetLayout;
extern VkDescriptorPool g_waterSimDescriptorPool;
extern VkDescriptorSet g_waterSimDescriptorSet;
extern VkPipelineLayout g_waterSimPipelineLayout;
extern VkPipeline g_waterSimPipeline;
extern VkImageLayout g_waterSimLayout;
extern uint32_t g_waterSimReadLayer;
extern bool g_waterSimNeedsClear;
extern bool g_waterSimPlayerStateValid;
extern bool g_waterSimPlayerWasInWater;
extern bool g_waterSimPendingImpulse;
extern float g_waterSimPendingImpulseX;
extern float g_waterSimPendingImpulseZ;
extern float g_waterSimPendingImpulseStrength;
extern float g_waterSimLastSurfaceY;
extern bool g_waterSimOriginValid;
extern float g_waterSimOriginX;
extern float g_waterSimOriginZ;
extern std::chrono::steady_clock::time_point g_waterSimLastUpdate;
extern bool g_waterSimLastUpdateValid;
extern float g_waterSimKeepAliveSeconds;
extern bool g_waterSimLoggedAwaitingWater;
extern bool g_waterSimLoggedFirstDispatch;
extern bool g_waterSimLoggedSampleContract;
extern VkBuffer g_waterSimEntityBuffer;
extern VkDeviceMemory g_waterSimEntityMemory;
extern void* g_waterSimEntityMapped;
extern std::mutex g_waterSimEntityMutex;
extern std::deque<WaterSimImpulseEvent> g_waterSimImpulseQueue;
extern float g_waterSimBoatData[WATER_SIM_MAX_ENTITIES * 5];
extern int32_t g_waterSimBoatCount;
extern const std::chrono::steady_clock::time_point g_waterWaveEpoch;
extern std::atomic<bool> g_waterCausticMapAvailable;
extern VkImage g_cloudShapeNoiseImage;
extern VkDeviceMemory g_cloudShapeNoiseMemory;
extern VkImageView g_cloudShapeNoiseView;
extern VkSampler g_cloudShapeNoiseSampler;
extern VkImage g_cloudCurlNoiseImage;
extern VkDeviceMemory g_cloudCurlNoiseMemory;
extern VkImageView g_cloudCurlNoiseView;
extern VkSampler g_cloudCurlNoiseSampler;
extern VkImage g_cloudBaseNoiseImage;
extern VkDeviceMemory g_cloudBaseNoiseMemory;
extern VkImageView g_cloudBaseNoiseView;
extern VkSampler g_cloudBaseNoiseSampler;
extern VkImage g_cloudV3ShapeImage;
extern VkDeviceMemory g_cloudV3ShapeMemory;
extern VkImageView g_cloudV3ShapeView;
extern VkSampler g_cloudV3ShapeSampler;
extern VkImage g_cloudV3DetailImage;
extern VkDeviceMemory g_cloudV3DetailMemory;
extern VkImageView g_cloudV3DetailView;
extern VkSampler g_cloudV3DetailSampler;
extern VkImage g_cloudV3CurlImage;
extern VkDeviceMemory g_cloudV3CurlMemory;
extern VkImageView g_cloudV3CurlView;
extern VkSampler g_cloudV3CurlSampler;
extern VkImage g_cloudV3WeatherImage;
extern VkDeviceMemory g_cloudV3WeatherMemory;
extern VkImageView g_cloudV3WeatherView;
extern VkSampler g_cloudV3WeatherSampler;
extern VkImage g_cloudV4AtlasImage;
extern VkDeviceMemory g_cloudV4AtlasMemory;
extern VkImageView g_cloudV4AtlasView;
extern VkSampler g_cloudV4AtlasSampler;
extern VkImage g_cloudV4WorleyImage;
extern VkImage g_cloudV4BaseImage;
extern VkDeviceMemory g_cloudV4BaseMemory;
extern VkImageView g_cloudV4BaseView;
extern VkDeviceMemory g_cloudV4WorleyMemory;
extern VkImageView g_cloudV4WorleyView;
extern VkSampler g_cloudV4WorleySampler;
extern VkSampler g_cloudV4BaseSampler;
extern VkImage g_cloudV4ShapeImage;
extern VkDeviceMemory g_cloudV4ShapeMemory;
extern VkImageView g_cloudV4ShapeView;
extern VkSampler g_cloudV4ShapeSampler;
extern VkImage g_volumetricCloudImage;
extern VkDeviceMemory g_volumetricCloudMemory;
extern VkImageView g_volumetricCloudView;
extern VkImage g_volumetricCloudHistoryImage;
extern VkDeviceMemory g_volumetricCloudHistoryMemory;
extern VkImageView g_volumetricCloudHistoryView;
extern VkImage g_volumetricCloudResolvedImage;
extern VkDeviceMemory g_volumetricCloudResolvedMemory;
extern VkImageView g_volumetricCloudResolvedView;
extern VkImage g_volumetricCloudDepthImage;
extern VkDeviceMemory g_volumetricCloudDepthMemory;
extern VkImageView g_volumetricCloudDepthView;
extern VkImage g_volumetricCloudHistoryDepthImage;
extern VkDeviceMemory g_volumetricCloudHistoryDepthMemory;
extern VkImageView g_volumetricCloudHistoryDepthView;
extern VkSampler g_volumetricCloudSampler;
extern VkImageLayout g_volumetricCloudLayout;
extern VkImageLayout g_volumetricCloudHistoryLayout;
extern VkImageLayout g_volumetricCloudResolvedLayout;
extern VkImageLayout g_volumetricCloudDepthLayout;
extern VkImageLayout g_volumetricCloudHistoryDepthLayout;
extern std::atomic<bool> g_volumetricCloudHistoryNeedsClear;
extern uint32_t g_volumetricCloudWidth;
extern uint32_t g_volumetricCloudHeight;
extern uint32_t g_volumetricCloudBoundRenderWidth;
extern uint32_t g_volumetricCloudBoundRenderHeight;
extern uint32_t g_volumetricCloudBoundOutputWidth;
extern uint32_t g_volumetricCloudBoundOutputHeight;
extern VkDescriptorSetLayout g_volumetricCloudRenderSetLayout;
extern VkDescriptorSetLayout g_volumetricCloudResolveSetLayout;
extern VkDescriptorSetLayout g_volumetricCloudCompositeSetLayout;
extern VkDescriptorPool g_volumetricCloudDescriptorPool;
extern VkDescriptorSet g_volumetricCloudRenderSet;
extern VkDescriptorSet g_volumetricCloudResolveSet;
extern VkDescriptorSet g_volumetricCloudCompositeSet;
extern VkImageView g_volumetricCloudCompositeTargetView;
extern VkPipelineLayout g_volumetricCloudRenderPipelineLayout;
extern VkPipelineLayout g_volumetricCloudResolvePipelineLayout;
extern VkPipelineLayout g_volumetricCloudCompositePipelineLayout;
extern VkPipeline g_volumetricCloudRenderPipelineV3;
extern VkPipeline g_volumetricCloudResolvePipeline;
extern VkPipeline g_volumetricCloudCompositePipeline;
extern VkDescriptorSetLayout g_volumetricCloudAtrousSetLayout;
extern VkDescriptorPool g_volumetricCloudAtrousDescriptorPool;
extern VkDescriptorSet g_volumetricCloudAtrousSetForward;
extern VkDescriptorSet g_volumetricCloudAtrousSetBackward;
extern VkPipelineLayout g_volumetricCloudAtrousPipelineLayout;
extern VkPipeline g_volumetricCloudAtrousPipeline;
extern VkDescriptorSetLayout g_volumetricLightSetLayout;
extern VkDescriptorSet g_volumetricLightSet;
extern VkPipelineLayout g_volumetricLightPipelineLayout;
extern VkPipeline g_volumetricLightPipeline;
extern std::atomic<uint32_t> g_volRestirEnabled;
extern std::atomic<uint32_t> g_volRestirGuiOverride;
extern std::atomic<uint32_t> g_cameraInWater;
extern VkImage g_volRadianceImage[2];
extern VkDeviceMemory g_volRadianceMemory[2];
extern VkImageView g_volRadianceView[2];
extern VkImage g_volShYImage[2];
extern VkDeviceMemory g_volShYMemory[2];
extern VkImageView g_volShYView[2];
extern VkImage g_volShCoCgImage[2];
extern VkDeviceMemory g_volShCoCgMemory[2];
extern VkImageView g_volShCoCgView[2];
extern bool g_volShNeedsInit;
extern VkImage g_volAgeImage[2];
extern VkDeviceMemory g_volAgeMemory[2];
extern VkImageView g_volAgeView[2];
extern VkImage g_volReservoirImage[2];
extern VkDeviceMemory g_volReservoirMemory[2];
extern VkImageView g_volReservoirView[2];
extern VkSampler g_volDepthSampler;
extern VkSampler g_volRadianceSampler;
extern uint32_t g_volFroxelW;
extern uint32_t g_volFroxelH;
extern uint32_t g_volRestirW;
extern uint32_t g_volRestirH;
extern uint32_t g_volWriteIndex;
extern VkDescriptorSetLayout g_volRestirSetLayout;
extern VkDescriptorPool g_volRestirDescriptorPool;
extern VkDescriptorSet g_volRestirSet;
extern VkPipelineLayout g_volRestirPipelineLayout;
extern VkPipeline g_volRestirPipeline;
extern bool g_volRestirPipelineFailed;
extern bool g_volRestirLayoutReady;
extern VkBuffer g_volRestirBoundLightBuffer;
extern VkBuffer g_volRestirBoundGridBuffer;
extern VkAccelerationStructureKHR g_volRestirBoundVisibilityTlas;
extern VkBuffer g_volRestirBoundVisibilityLightBuffer;
extern VkBuffer g_volRestirBoundVisibilityGridBuffer;
extern VkBuffer g_volRestirBoundIntegrateLightBuffer;
extern VkBuffer g_volRestirBoundIntegrateGridBuffer;
extern VkAccelerationStructureKHR g_volRestirBoundIntegrateTlas;
extern VkDescriptorSetLayout g_volRestirTemporalSetLayout;
extern VkDescriptorPool g_volRestirTemporalDescriptorPool;
extern VkDescriptorSet g_volRestirTemporalSet;
extern VkPipelineLayout g_volRestirTemporalPipelineLayout;
extern VkPipeline g_volRestirTemporalPipeline;
extern VkDescriptorSetLayout g_volRestirSpatialSetLayout;
extern VkDescriptorPool g_volRestirSpatialDescriptorPool;
extern VkDescriptorSet g_volRestirSpatialSet;
extern VkPipelineLayout g_volRestirSpatialPipelineLayout;
extern VkPipeline g_volRestirSpatialPipeline;
extern VkDescriptorSetLayout g_volRestirIntegrateSetLayout;
extern VkDescriptorPool g_volRestirIntegrateDescriptorPool;
extern VkDescriptorSet g_volRestirIntegrateSet;
extern VkPipelineLayout g_volRestirIntegratePipelineLayout;
extern VkPipeline g_volRestirIntegratePipeline;
extern VkDescriptorSetLayout g_volRestirCompositeSetLayout;
extern VkDescriptorPool g_volRestirCompositeDescriptorPool;
extern VkDescriptorSet g_volRestirCompositeSet;
extern VkPipelineLayout g_volRestirCompositePipelineLayout;
extern VkPipeline g_volRestirCompositePipeline;
extern VkDescriptorSetLayout g_volRestirVisibilitySetLayout;
extern VkDescriptorPool g_volRestirVisibilityDescriptorPool;
extern VkDescriptorSet g_volRestirVisibilitySet;
extern VkPipelineLayout g_volRestirVisibilityPipelineLayout;
extern VkPipeline g_volRestirVisibilityPipeline;


extern VkImage g_godRayTargetImage;
extern VkDeviceMemory g_godRayTargetMemory;
extern VkImageView g_godRayTargetView;
extern VkImage g_godRayHistoryImage;
extern VkDeviceMemory g_godRayHistoryMemory;
extern VkImageView g_godRayHistoryView;
extern VkImageLayout g_godRayTargetLayout;
extern VkImageLayout g_godRayHistoryLayout;
extern std::atomic<bool> g_godRayHistoryNeedsClear;
extern uint32_t g_godRayWidth;
extern uint32_t g_godRayHeight;
extern VkDescriptorSetLayout g_godRayCompositeSetLayout;
extern VkDescriptorPool g_godRayDescriptorPool;
extern VkDescriptorSet g_godRayCompositeSet;
extern VkPipelineLayout g_godRayCompositePipelineLayout;
extern VkPipeline g_godRayCompositePipeline;
extern VkImage g_sunShadowImage;
extern VkDeviceMemory g_sunShadowMemory;
extern VkImageView g_sunShadowView;
extern VkSampler g_sunShadowSampler;
extern VkImageLayout g_sunShadowLayout;
extern VkDescriptorSetLayout g_sunShadowSetLayout;
extern VkDescriptorSet g_sunShadowSet;
extern VkDescriptorPool g_sunShadowDescriptorPool;
extern VkPipelineLayout g_sunShadowPipelineLayout;
extern VkPipeline g_sunShadowPipeline;
extern std::atomic<bool> g_sunShadowAvailable;
extern VkAccelerationStructureKHR g_sunShadowTlasHandle;
extern VkImage g_sunShadowFarImage;
extern VkDeviceMemory g_sunShadowFarMemory;
extern VkImageView g_sunShadowFarView;
extern VkSampler g_sunShadowFarSampler;
extern VkImageLayout g_sunShadowFarLayout;
extern VkDescriptorSetLayout g_sunShadowFarSetLayout;
extern VkDescriptorSet g_sunShadowFarSet;
extern VkDescriptorPool g_sunShadowFarDescriptorPool;
extern VkPipelineLayout g_sunShadowFarPipelineLayout;
extern VkPipeline g_sunShadowFarPipeline;
extern std::atomic<bool> g_sunShadowFarAvailable;
extern VkAccelerationStructureKHR g_sunShadowFarTlasHandle;
extern VkImage g_terrainHeightImage;
extern VkDeviceMemory g_terrainHeightMemory;
extern VkImageView g_terrainHeightView;
extern VkSampler g_terrainHeightSampler;
extern VkImageLayout g_terrainHeightLayout;
extern VkDescriptorSetLayout g_terrainHeightSetLayout;
extern VkDescriptorSet g_terrainHeightSet;
extern VkDescriptorPool g_terrainHeightDescriptorPool;
extern VkPipelineLayout g_terrainHeightPipelineLayout;
extern VkPipeline g_terrainHeightPipeline;
extern std::atomic<bool> g_terrainHeightAvailable;
extern VkAccelerationStructureKHR g_terrainHeightTlasHandle;
extern bool g_terrainHeightBuilt;
extern uint64_t g_terrainHeightBuiltRevision;
extern float g_terrainHeightBuiltCenter[2];
extern VkDescriptorSetLayout g_rrGuideRelaxSetLayout;
extern VkDescriptorSet g_rrGuideRelaxSet;
extern VkDescriptorPool g_rrGuideRelaxDescriptorPool;
extern VkPipelineLayout g_rrGuideRelaxPipelineLayout;
extern VkPipeline g_rrGuideRelaxPipeline;
extern std::atomic<bool> g_rrGuideRelaxEnabled;
extern VkImageView g_rrGuideRelaxCachedNormal;
extern VkImageView g_rrGuideRelaxCachedDepth;
extern VkImageView g_rrGuideRelaxCachedDiffuse;
extern VkImageView g_rrGuideRelaxCachedSpecularAlbedo;
extern VkImageView g_rrGuideRelaxCachedSpecularHitDistance;
extern VkImageView g_rrGuideRelaxCachedCloudResolved;
extern VkImageView g_rrGuideRelaxCachedCloudDepth;
extern VkBuffer g_rrGuideRelaxCachedCamera;
extern uint32_t g_rrGuideViewGeneration;
extern uint32_t g_rrGuideRelaxCachedGeneration;
extern std::atomic<float> g_rrFarCloudDistance;
extern std::atomic<bool> g_godRayEnabled;
extern std::atomic<int> g_godRaySteps;
extern std::atomic<int> g_godRayOcclusionSamples;
extern std::atomic<float> g_godRayIntensity;
extern std::atomic<float> g_godRayHistoryWeight;
extern VkImage g_skyScatteringImage;
extern VkDeviceMemory g_skyScatteringMemory;
extern VkImageView g_skyScatteringView;
extern VkImage g_skyTransmittanceImage;
extern VkDeviceMemory g_skyTransmittanceMemory;
extern VkImageView g_skyTransmittanceView;
extern VkSampler g_skyAtmosphereSampler;
extern VkDescriptorSetLayout g_skyAtmosphereSetLayout;
extern VkDescriptorPool g_skyAtmosphereDescriptorPool;
extern VkDescriptorSet g_skyAtmosphereDescriptorSet;
extern VkPipelineLayout g_skyAtmospherePipelineLayout;
extern VkPipeline g_skyAtmospherePipeline;
extern VkImageLayout g_skyScatteringLayout;
extern VkImageLayout g_skyTransmittanceLayout;
extern VkImage g_waterDetailNormalImage;
extern VkDeviceMemory g_waterDetailNormalMemory;
extern VkImageView g_waterDetailNormalView;
extern VkSampler g_waterDetailNormalSampler;
extern VkImage g_waterCausticImage;
extern VkDeviceMemory g_waterCausticMemory;
extern VkImageView g_waterCausticView;
extern VkSampler g_waterCausticSampler;
extern VkDescriptorSetLayout g_waterCausticDescriptorSetLayout;
extern VkDescriptorPool g_waterCausticDescriptorPool;
extern VkDescriptorSet g_waterCausticDescriptorSet;
extern VkPipelineLayout g_waterCausticPipelineLayout;
extern VkPipeline g_waterCausticPipeline;
extern VkImageLayout g_waterCausticLayout;
extern std::chrono::steady_clock::time_point g_waterCausticLastUpdate;
extern bool g_waterCausticLastUpdateValid;
extern VkBuffer g_testVertexBuffer;
extern VkDeviceMemory g_testVertexMemory;
extern VkDeviceAddress g_testVertexDeviceAddress;
extern VkBuffer g_blasBuffer;
extern VkDeviceMemory g_blasMemory;
extern VkAccelerationStructureKHR g_blas;
extern VkDeviceAddress g_blasDeviceAddress;
extern VkBuffer g_blasScratchBuffer;
extern std::unordered_map<uint64_t, OmmCutoutInput> g_cutoutOmmInputs;
extern bool g_ommNativeEnabled;
extern uint32_t g_ommMax4StateSubdivisionLevel;
extern bool g_serExtAdvertised;
extern bool g_serFeatureSupported;
extern bool g_serHardwareReordering;
extern bool g_serNvAdvertised;
extern bool g_serNvHintReorder;
extern std::atomic_bool g_serEnabled;


extern std::unordered_map<uint64_t, RTSectionAS>
    g_sectionBLAS;
extern std::unordered_set<uint64_t> g_nonResidentSections;
extern uint64_t g_vramEvictedSections;
extern uint64_t g_vramRehydratedSections;



extern VkBuffer g_tlasInstancesBuffer;
extern VkDeviceMemory g_tlasInstancesMemory;
extern VkDeviceAddress g_tlasInstancesAddress;
extern VkBuffer g_sectionVertexAddressBuffer;
extern VkDeviceMemory g_sectionVertexAddressMemory;
extern size_t g_sectionVertexAddressCapacity;
extern uint32_t g_sectionVertexAddressNextSlot;
extern std::vector<uint32_t> g_sectionVertexAddressFreeSlots;
extern void* g_sectionVertexAddressMapped;
extern VkBuffer g_sectionRoleBuffer;
extern VkDeviceMemory g_sectionRoleMemory;
extern void* g_sectionRoleMapped;
extern VkBuffer g_lodCoverageBuffer;
extern VkDeviceMemory g_lodCoverageMemory;
extern void* g_lodCoverageMapped;
extern uint32_t g_lodCoverageWordCount;
extern VkDeviceMemory g_blasScratchMemory;
extern VkDeviceAddress g_blasScratchAddress;
extern VkBuffer g_tlasBuffer;
extern VkDeviceMemory g_tlasMemory;
extern VkAccelerationStructureKHR g_tlas;
extern VkAccelerationStructureKHR g_tlasDescriptorHandle;
extern VkDeviceAddress g_tlasDeviceAddress;
extern VkBuffer g_tlasInstanceBuffer;
extern VkDeviceMemory g_tlasInstanceMemory;
extern VkDeviceAddress g_tlasInstanceDeviceAddress;
extern VkBuffer g_tlasScratchBuffer;
extern VkDeviceMemory g_tlasScratchMemory;
extern VkDeviceAddress g_tlasScratchAddress;
extern VkDeviceSize g_tlasScratchCapacity;
extern VkDeviceSize g_tlasAsSize;
extern VkDeviceSize g_tlasInstancesCapacity;
extern size_t g_tlasInstanceCount;


extern DeferredSectionBuildResources
    g_deferredSectionBuild;
extern uint64_t g_sectionBuildPoolAllocCount;
extern uint64_t g_sectionBuildPoolReuseCount;
extern std::array<
    SectionRetirementSlot,
    SECTION_RETIREMENT_RING_SIZE
> g_sectionRetirementRing;
extern std::array<
    VkFence,
    SECTION_RETIREMENT_RING_SIZE
> g_sectionRetirementFences;
extern bool g_sectionRetirementFencesReady;
extern uint32_t g_currentRetirementSlot;
extern SectionRetirementSlot*
    g_sectionRetirement;
extern VkDescriptorPool g_rayTracingDescriptorPool;
extern VkDescriptorSet g_rayTracingDescriptorSet;
extern VkDescriptorSetLayout g_rayTracingDescriptorSetLayout;
extern VkPipelineLayout g_rayTracingPipelineLayout;
extern VkPipeline g_rayTracingPipeline;
extern bool g_nrdReady;
extern nrd::Instance* g_nrdInstance;
extern uint32_t g_nrdWidth;
extern uint32_t g_nrdHeight;
extern uint32_t g_nrdIdentifier;
extern nrd::LibraryDesc g_nrdLibraryDesc;
extern nrd::InstanceDesc g_nrdInstanceDesc;
extern VkBuffer g_nrdConstantBuffer;
extern VkDeviceMemory g_nrdConstantBufferMemory;
extern VkDeviceSize g_nrdConstantBufferStride;
extern VkSampler g_nrdSamplerNearest;
extern VkSampler g_nrdSamplerLinear;
extern VkDescriptorSetLayout g_nrdDescriptorSetLayout;
extern VkDescriptorSetLayout g_nrdConstantSetLayout;
extern VkPipelineLayout g_nrdPipelineLayout;
extern VkDescriptorPool g_nrdDescriptorPool;
extern std::vector<VkDescriptorSet> g_nrdDescriptorSets;
extern std::vector<VkDescriptorSet> g_nrdConstantSets;
extern std::vector<VkPipeline> g_nrdPipelines;
extern std::vector<VkImage> g_nrdPermanentImages;
extern std::vector<VkDeviceMemory> g_nrdPermanentMemories;
extern std::vector<VkImageView> g_nrdPermanentViews;
extern std::vector<VkImageLayout> g_nrdPermanentLayouts;
extern std::vector<nrd::TextureDesc> g_nrdPermanentDescs;
extern std::vector<VkImage> g_nrdTransientImages;
extern std::vector<VkDeviceMemory> g_nrdTransientMemories;
extern std::vector<VkImageView> g_nrdTransientViews;
extern std::vector<VkImageLayout> g_nrdTransientLayouts;
extern std::vector<nrd::TextureDesc> g_nrdTransientDescs;
extern VkImage g_nrdViewZImage;
extern VkDeviceMemory g_nrdViewZMemory;
extern VkImageView g_nrdViewZView;
extern VkImageLayout g_nrdViewZLayout;
extern VkImage g_nrdNormalRoughnessImage;
extern VkDeviceMemory g_nrdNormalRoughnessMemory;
extern VkImageView g_nrdNormalRoughnessView;
extern VkImageLayout g_nrdNormalRoughnessLayout;
extern VkImage g_nrdMotionImage;
extern VkDeviceMemory g_nrdMotionMemory;
extern VkImageView g_nrdMotionView;
extern VkImageLayout g_nrdMotionLayout;
extern VkImage g_nrdDiffRadianceHitDistImage;
extern VkDeviceMemory g_nrdDiffRadianceHitDistMemory;
extern VkImageView g_nrdDiffRadianceHitDistView;
extern VkImageLayout g_nrdDiffRadianceHitDistLayout;
extern VkImage g_nrdOutDiffRadianceHitDistImage;
extern VkDeviceMemory g_nrdOutDiffRadianceHitDistMemory;
extern VkImageView g_nrdOutDiffRadianceHitDistView;
extern VkImageLayout g_nrdOutDiffRadianceHitDistLayout;
extern VkImage g_nrdValidationImage;
extern VkDeviceMemory g_nrdValidationMemory;
extern VkImageView g_nrdValidationView;
extern VkImageLayout g_nrdValidationLayout;
extern VkImage g_nrdMvProbeImage;
extern VkDeviceMemory g_nrdMvProbeMemory;
extern VkImageView g_nrdMvProbeView;
extern VkImageLayout g_nrdMvProbeLayout;
extern VkPipelineLayout g_nrdCompositePipelineLayout;
extern VkPipeline g_nrdCompositePipeline;
extern uint32_t g_nrdFrameIndex;
extern bool g_nrdNeedsHistoryReset;
extern bool g_nrdSettingsLogged;
extern bool g_nrdDisocclusionLogged;
extern float g_nrdSmoothedFrameTimeMs;


extern VkDescriptorPool
    g_sdrPostDescriptorPool;
extern VkDescriptorSet
    g_sdrPostDescriptorSet;
extern VkDescriptorSetLayout
    g_sdrPostDescriptorSetLayout;
extern VkPipelineLayout
    g_sdrPostPipelineLayout;
extern VkPipeline
    g_sdrPostPipeline;
extern std::atomic<bool> g_hdrRequested;
extern std::atomic<bool> g_hdrSurfaceSupported;
extern VkDescriptorPool  g_hdrEncodeDescriptorPool;
extern VkDescriptorSet   g_hdrEncodeDescriptorSet;
extern VkDescriptorSetLayout g_hdrEncodeDescriptorSetLayout;
extern VkPipelineLayout  g_hdrEncodePipelineLayout;
extern VkPipeline        g_hdrEncodePipeline;
extern VkImage        g_hdr10Image;
extern VkDeviceMemory g_hdr10Memory;
extern VkImageView    g_hdr10View;
extern VkImageLayout  g_hdr10Layout;
extern uint32_t       g_hdr10Width;
extern uint32_t       g_hdr10Height;


extern VkPipeline
    g_hdrPostPipeline;
extern bool
    g_hdrPresentationWasActive;


extern std::atomic_bool g_aeEnabled;
extern std::atomic<float> g_aeCompensationEV;
extern float g_aeMinEV;
extern float g_aeMaxEV;
extern float g_aeAdaptUpRate;
extern float g_aeAdaptDownRate;
extern float g_aeTrimLow;
extern float g_aeTrimHigh;
extern VkBuffer g_aeHistogramBuffer;
extern VkDeviceMemory g_aeHistogramMemory;
extern VkBuffer g_aeStateBuffer;
extern VkDeviceMemory g_aeStateMemory;
extern float* g_aeStateCpu;
extern VkDescriptorSetLayout g_aeHistogramSetLayout;
extern VkDescriptorPool g_aeHistogramPool;
extern VkDescriptorSet g_aeHistogramSet;
extern VkPipelineLayout g_aeHistogramLayout;
extern VkPipeline g_aeHistogramPipeline;
extern VkDescriptorSetLayout g_aeAdaptSetLayout;
extern VkDescriptorPool g_aeAdaptPool;
extern VkDescriptorSet g_aeAdaptSet;
extern VkPipelineLayout g_aeAdaptLayout;
extern VkPipeline g_aeAdaptPipeline;
extern std::chrono::steady_clock::time_point g_aeLastTick;
extern bool g_aeHasLastTick;
extern bool g_aeReported;
extern VkImage g_rtOutputImage;
extern VkImage g_dlssOutputImage;
extern VkImage g_dlssDepthImage;
extern VkDeviceMemory g_dlssDepthMemory;
extern VkImageView g_dlssDepthView;
extern VkDeviceMemory g_dlssOutputMemory;
extern VkImageView g_dlssOutputView;
extern sl::ViewportHandle g_dlssViewport;
extern VkImage g_motionImage;
extern VkDeviceMemory g_motionMemory;
extern VkImageView g_motionView;
extern uint32_t g_motionWidth;
extern uint32_t g_motionHeight;



extern VkImage g_rrStableRadianceImage;
extern VkDeviceMemory g_rrStableRadianceMemory;
extern VkImageView g_rrStableRadianceView;
extern VkImageLayout g_rrStableRadianceLayout;
extern VkSampler g_rrStableRadianceSampler;
extern VkImage g_rrDiffuseAlbedoImage;
extern VkDeviceMemory g_rrDiffuseAlbedoMemory;
extern VkImageView g_rrDiffuseAlbedoView;
extern VkImageLayout g_rrDiffuseAlbedoLayout;


extern VkImage g_rrSpecularAlbedoImage;
extern VkDeviceMemory g_rrSpecularAlbedoMemory;
extern VkImageView g_rrSpecularAlbedoView;
extern VkImageLayout g_rrSpecularAlbedoLayout;


extern VkImage g_rrNormalRoughnessImage;
extern VkDeviceMemory g_rrNormalRoughnessMemory;
extern VkImageView g_rrNormalRoughnessView;
extern VkImageLayout g_rrNormalRoughnessLayout;


extern VkImage g_rrSpecularHitDistanceImage;
extern VkDeviceMemory g_rrSpecularHitDistanceMemory;
extern VkImageView g_rrSpecularHitDistanceView;
extern VkImageLayout g_rrSpecularHitDistanceLayout;


extern VkImage g_rrTransparencyHintImage;
extern VkDeviceMemory g_rrTransparencyHintMemory;
extern VkImageView g_rrTransparencyHintView;
extern VkImageLayout g_rrTransparencyHintLayout;
extern VkImage g_giHalfImage;
extern VkDeviceMemory g_giHalfMemory;
extern VkImageView g_giHalfView;
extern VkImageLayout g_giHalfLayout;
extern uint32_t g_giHalfWidth;
extern uint32_t g_giHalfHeight;
extern bool g_giHalfNeedsClear;
extern VkImageView g_giHalfBoundView;

// 2026-11-04 A1：GI 第一反弹时域重投影历史（binding 86/87，ping-pong 两半）。
extern VkImage g_giTemporalHistoryImages[2];
extern VkDeviceMemory g_giTemporalHistoryMemories[2];
extern VkImageView g_giTemporalHistoryViews[2];
extern VkImageLayout g_giTemporalHistoryLayouts[2];
extern uint32_t g_giTemporalHistoryWidth;
extern uint32_t g_giTemporalHistoryHeight;
extern bool g_giTemporalHistoryNeedsClear;


extern VkImage g_alphaBlendGbufferImage;
extern VkDeviceMemory g_alphaBlendGbufferMemory;
extern VkImageView g_alphaBlendGbufferView;
extern VkImageLayout g_alphaBlendGbufferLayout;
extern VkImage g_alphaBlendMetaImage;
extern VkDeviceMemory g_alphaBlendMetaMemory;
extern VkImageView g_alphaBlendMetaView;
extern VkImageLayout g_alphaBlendMetaLayout;
extern VkImage g_alphaBlendLightImage;
extern VkDeviceMemory g_alphaBlendLightMemory;
extern VkImageView g_alphaBlendLightView;
extern VkImageLayout g_alphaBlendLightLayout;
extern VkImage g_alphaBlendVolumeImage;
extern VkDeviceMemory g_alphaBlendVolumeMemory;
extern VkImageView g_alphaBlendVolumeView;
extern VkImageLayout g_alphaBlendVolumeLayout;
extern VkImage g_alphaBlendRadianceImage;
extern VkDeviceMemory g_alphaBlendRadianceMemory;
extern VkImageView g_alphaBlendRadianceView;
extern VkImageLayout g_alphaBlendRadianceLayout;
extern VkImage g_alphaBlendPlaceholder3DImage;
extern VkDeviceMemory g_alphaBlendPlaceholder3DMemory;
extern VkImageView g_alphaBlendPlaceholder3DView;
extern VkPipeline g_glassAlphaDiagPipeline;
extern VkImage g_alphaBlendPlaceholderR8Image;
extern VkDeviceMemory g_alphaBlendPlaceholderR8Memory;
extern VkImageView g_alphaBlendPlaceholderR8View;
extern VkImage g_alphaBlendPlaceholderUintImage;
extern VkDeviceMemory g_alphaBlendPlaceholderUintMemory;
extern VkImageView g_alphaBlendPlaceholderUintView;
extern uint32_t g_alphaBlendWidth;
extern uint32_t g_alphaBlendHeight;
extern VkDescriptorSetLayout g_glassAlphaBlendSetLayout;
extern VkDescriptorPool g_glassAlphaBlendPool;
extern VkDescriptorSet g_glassAlphaBlendSet;
extern VkPipelineLayout g_glassAlphaBlendPipelineLayout;
extern VkPipeline g_glassAlphaBlendPipeline;
extern VkPipeline g_glassAlphaFinishPipeline;
extern bool g_glassAlphaBlendPipelineFailed;
extern uint32_t g_glassAlphaBlendFrameIndex;
extern VkBuffer g_glassBlueNoiseBuffer;
extern VkDeviceMemory g_glassBlueNoiseMemory;
extern bool g_glassBlueNoiseReady;
extern VkDescriptorSet g_glassAlphaBlendBoundSet;
extern VkImageView g_glassAlphaBlendBoundRayGbuffer;
extern VkImageView g_glassAlphaBlendBoundColor;
extern VkImageView g_glassAlphaBlendBoundGbuffer;
extern VkImageView g_glassAlphaBlendBoundMeta;
extern VkImageView g_glassAlphaBlendBoundLight;
extern VkImageView g_glassAlphaBlendBoundRadiance;
extern VkImageView g_glassAlphaBlendBoundDepth;
extern VkImageView g_glassAlphaBlendBoundNormal;
extern VkImageView g_glassAlphaBlendBoundAlbedo;
extern VkImageView g_glassAlphaBlendBoundShY;
extern std::atomic<int> g_giNearRateOverride;
extern std::atomic<int> g_glossyRoughLodOverride;
extern std::atomic<int> g_directLightModeOverride;
extern std::atomic<int> g_indirectModeOverride;
extern std::atomic<int> g_sparseRatePercentOverride;
extern std::atomic<int> g_rtxdiReuseInactiveOverride;


extern uint32_t g_rrGBufferWidth;
extern uint32_t g_rrGBufferHeight;


extern uint32_t g_dlssOutputWidth;
extern uint32_t g_dlssOutputHeight;
extern uint32_t g_dlssDepthWidth;
extern uint32_t g_dlssDepthHeight;
extern VkDeviceMemory g_rtOutputMemory;
extern VkImageView g_rtOutputView;
extern uint32_t g_rtOutputWidth, g_rtOutputHeight;
extern VkImageLayout g_rtOutputLayout;
extern VkImageLayout g_dlssOutputLayout;
extern VkImageLayout g_dlssDepthLayout;
extern VkImageLayout g_motionLayout;


extern bool g_asBuilt;
extern HMODULE g_slModule;
extern bool g_slDevPluginsRequested;
extern std::string g_slActivePluginDir;
extern VkFormat
    g_fgBackbufferFormat;
extern VkFormat
    g_fgSnapshotFormat;


extern VkImage
    g_fgHudlessImage;
extern VkDeviceMemory
    g_fgHudlessMemory;
extern VkImageView
    g_fgHudlessView;
extern VkImageLayout
    g_fgHudlessLayout;


extern VkImage
    g_fgFinalImage;
extern VkDeviceMemory
    g_fgFinalMemory;
extern VkImageView
    g_fgFinalView;
extern VkImageLayout
    g_fgFinalLayout;


extern VkImage
    g_fgUIAlphaImage;
extern VkDeviceMemory
    g_fgUIAlphaMemory;
extern VkImageView
    g_fgUIAlphaView;
extern VkImageLayout
    g_fgUIAlphaLayout;


extern uint32_t
    g_fgUIWidth;
extern uint32_t
    g_fgUIHeight;


extern uint32_t
    g_fgHudlessFrameIndex;
extern uint32_t
    g_fgUIAlphaFrameIndex;


extern VkSampler
    g_fgUISnapshotSampler;


extern VkDescriptorPool
    g_fgUIAlphaDescriptorPool;
extern VkDescriptorSet
    g_fgUIAlphaDescriptorSet;
extern VkDescriptorSetLayout
    g_fgUIAlphaDescriptorSetLayout;
extern VkPipelineLayout
    g_fgUIAlphaPipelineLayout;
extern VkPipeline
    g_fgUIAlphaPipeline;
extern bool
    g_slDeviceConfigured;


extern PFN_vkGetInstanceProcAddr
    g_slVkGetInstanceProcAddrProxy;
extern PFN_vkGetDeviceProcAddr
    g_slVkGetDeviceProcAddrProxy;


extern PFN_vkCreateWin32SurfaceKHR
    g_slVkCreateWin32SurfaceKHRProxy;
extern PFN_vkDestroySurfaceKHR
    g_slVkDestroySurfaceKHRProxy;


extern PFN_vkCreateSwapchainKHR
    g_slVkCreateSwapchainKHRProxy;
extern PFN_vkDestroySwapchainKHR
    g_slVkDestroySwapchainKHRProxy;
extern PFN_vkGetSwapchainImagesKHR
    g_slVkGetSwapchainImagesKHRProxy;
extern PFN_vkAcquireNextImageKHR
    g_slVkAcquireNextImageKHRProxy;
extern PFN_vkQueuePresentKHR
    g_slVkQueuePresentKHRProxy;
extern PFN_vkDeviceWaitIdle
    g_slVkDeviceWaitIdleProxy;
extern bool g_dlssReady;
extern bool g_dlssRRSupported;
extern bool g_dlssRRReady;
extern std::atomic<bool> g_sparseRateHistoryReset;
extern std::atomic<bool> g_sparseHistoryValid;
extern bool g_dlssGSupported;
extern bool g_dlssGReady;
extern std::atomic<uint32_t> g_mvScaleMode;
extern std::atomic<bool> g_fgRefreshCapEnabled;
extern bool g_fgTagsValid;
extern std::atomic<uint32_t> g_fgForceOffFrames;
extern bool g_fgSnapshotsReleased;
extern bool g_dlssGFeatureInitialized;
extern sl::Result g_dlssGLoadResult;
extern bool g_fgFrameSyncEnabled;
extern std::atomic<int> g_fgMultiplierRequested;
extern std::atomic<uint32_t> g_fgGeneratedMaxSeen;
extern int g_fgBaseFrameLimitRequest;
extern std::atomic<uint32_t> g_fgBaseFrameLimitHz;
extern std::atomic<bool> g_fgFullscreenMenuOpen;
extern uint32_t g_fgGateLastMask;
extern bool
    g_fgRequirementsReady;


extern std::vector<std::string>
    g_fgInstanceExtensions;
extern std::vector<std::string>
    g_fgDeviceExtensions;
extern std::vector<std::string>
    g_fgFeatures12;
extern std::vector<std::string>
    g_fgFeatures13;


extern uint32_t
    g_fgGraphicsQueuesRequired;
extern uint32_t
    g_fgComputeQueuesRequired;
extern uint32_t
    g_fgOpticalFlowQueuesRequired;
extern bool
    g_slQueuePlanReady;


extern uint32_t
    g_slGraphicsQueueFamily;
extern uint32_t
    g_slGraphicsQueueIndex;


extern uint32_t
    g_slComputeQueueFamily;
extern uint32_t
    g_slComputeQueueIndex;


extern uint32_t
    g_slOpticalFlowQueueFamily;
extern uint32_t
    g_slOpticalFlowQueueIndex;


extern bool
    g_slUseNativeOpticalFlow;


extern PFN_slDLSSGSetOptions
    g_slDLSSGSetOptions;
extern PFN_slDLSSGGetState
    g_slDLSSGGetState;
extern std::atomic<int> g_rrMode;
extern std::atomic<int> g_upscalerMode;
extern std::atomic<int> g_srMode;
extern std::atomic<float>
    g_noDirectLightAmbient;
extern std::atomic<float>
    g_pomDepth;
extern std::atomic<float>
    g_pomSharp;

// =====================================================
// 【2026-11-02】屏幕空间降级链（SSR + SSPT）的设置与资源
//
// 两条滑条是**同一条链**的两个入口，默认都为 0（关 ⇒ 宿主不建图、不建管线、
// 不 dispatch，画面与接进来之前逐位一致）。
//   g_ssptChunks = SSPT 射程（区块，UI 0..12）
//   g_ssrChunks  = SSR 射程上限（区块，UI 硬性下限 12、0 = 关）
// 实现与口径见 cpp/backend/ScreenSpace.cpp 的头部注释。
// =====================================================
extern std::atomic<float> g_ssptChunks;
extern std::atomic<float> g_ssrChunks;

// 2026-11-03（UI 开关）：RT 光泽射线总开关。1 = 开（出厂），0 = 关。
// 写进 CameraUBO.ssHybridParams.z，rgen 读到 < 0.5 就不再发那条 GGX 反射射线，
// 镜面交给 SSR（远场）/ SSPT（射程内）—— 这是"仅 SSPT + RTAO"配置的入口。
extern std::atomic<float> g_ssRtGlossy;

// 分项 G-buffer（rt_test.rgen 写、ss_hybrid.comp 读）
extern VkImage g_ssIndirectTermImage;
extern VkDeviceMemory g_ssIndirectTermMemory;
extern VkImageView g_ssIndirectTermView;
extern VkImageLayout g_ssIndirectTermLayout;

extern VkImage g_ssSpecularTermImage;
extern VkDeviceMemory g_ssSpecularTermMemory;
extern VkImageView g_ssSpecularTermView;
extern VkImageLayout g_ssSpecularTermLayout;

// 本帧 RT 输出副本（被替换前的基准色）与上一帧最终色（屏幕空间辐射源）
extern VkImage g_ssBaseColorImage;
extern VkDeviceMemory g_ssBaseColorMemory;
extern VkImageView g_ssBaseColorView;
extern VkImageLayout g_ssBaseColorLayout;

extern VkImage g_ssPrevColorImage;
extern VkDeviceMemory g_ssPrevColorMemory;
extern VkImageView g_ssPrevColorView;
extern VkImageLayout g_ssPrevColorLayout;

// Hi-Z 深度金字塔（2026-11-02 第二轮）：storage buffer 里的扁平 mip 链，
// 布局公式见 ss_hzb.comp 头注释（levelWidth(k)=ceil(W/2^k)，逐级累加偏移）。
extern VkBuffer g_ssHzbBuffer;
extern VkDeviceMemory g_ssHzbMemory;
extern uint32_t g_ssHzbLevelCount;
extern VkPipeline g_ssHzbPipeline;

extern uint32_t g_ssHybridWidth;
extern uint32_t g_ssHybridHeight;
extern bool g_ssHybridHistoryValid;
extern VkDescriptorSet g_ssHybridBoundSet;
extern VkPipelineLayout g_ssHybridPipelineLayout;
extern VkPipeline g_ssHybridPipeline;
extern bool g_ssHybridPipelineFailed;
extern std::atomic<float>
    g_sssMode;
extern std::atomic<float> g_ueCloudParams[UE_CLOUD_PARAM_COUNT];

// 2026-10-31：语义从"最远距离（格）"改成"整体距离系数"（1.0 = 标准）。
// 每个光源方块的基础传播距离在 local_light_profile.glsl 的档案表里，
// 这里只做整体缩放，乘积在着色器里封顶 30 格。
extern std::atomic<float>
    g_localLightMaxDistance;
extern std::atomic<float>
    g_localLightIntensity;
extern std::atomic<bool> g_cloudEnabled;
extern std::atomic<float> g_cloudBottom;
extern std::atomic<float> g_cloudThickness;
extern std::atomic<float> g_cloudCoverage;
extern std::atomic<float> g_cloudDensity;
extern std::atomic<float> g_cloudDetailStrength;
extern std::atomic<float> g_cloudWindSpeed;
extern std::atomic<float> g_cloudEvolution;
extern std::atomic<uint32_t> g_cloudQuality;
extern std::atomic<float> g_cloudSize;
extern std::atomic<float> g_cloudDistribution;
extern std::atomic<float> g_cloudBaseFeather;
extern std::atomic<float> g_cloudSunlightStrength;
extern std::atomic<float> g_cloudPhiIntensity;
extern std::atomic<float> g_cloudPhiDepthPow;
extern std::atomic<float> g_cloudPhiDepthBias;
extern std::atomic<float> g_cloudMsTailStrength;
extern std::atomic<float> g_cloudSunReddening;
extern std::atomic<float> g_cloudAdaptiveLightRange;
extern std::atomic<float> g_cloudFarFadeDistance;
extern std::atomic<float> g_planeCloudEnabled;
extern std::atomic<float> g_planeCloudHeightOffset;
extern std::atomic<float> g_planeCloudScale;
extern std::atomic<float> g_planeCloudSpeed;
extern std::atomic<float> g_planeCloudDensity;
extern std::atomic<float> g_planeCloudCoverage;
extern std::atomic<float> g_planeCloudOctaves;
extern std::atomic<float> g_planeCloudFadeDistance;
extern std::atomic<float> g_planeCloudBrightness;
extern std::atomic<uint32_t> g_cloudResFloorPercent;
extern std::atomic<bool> g_cloudPostRrComposite;
extern std::atomic<float> g_cloudFarStepDensity;
extern std::atomic<bool> g_cloudFadeFollowsRrMode;
extern std::atomic<float> g_cloudTwoTierStep;
extern std::atomic<float> g_cloudMeanDepthReproject;
extern std::atomic<float> g_cloudTopStretch;
extern std::atomic<float> g_cloudDetileStrength;
extern std::atomic<float> g_cloudType;
extern std::atomic<float> g_cloudErosionScale;
extern std::atomic<float> g_hpDetailStrengthCu;
extern std::atomic<float> g_hpDetailStrengthTcu;
extern std::atomic<float> g_hpDetailStrengthCb;
extern std::atomic<float> g_hpBottomSmoothHeight;
extern std::atomic<float> g_hpWispyEdgeWidth;
extern std::atomic<float> g_hpWispyReach;
extern std::atomic<float> g_hpWispyTopHeight;
extern std::atomic<float> g_hpWispyTopHardness;
extern std::atomic<float> g_hpWispyWeight;
extern std::atomic<float> g_hpBillowyWeight;
extern std::atomic<float> g_hpLoCoverTopMax;
extern std::atomic<float> g_hpLoCoverTopStr;
extern std::atomic<float> g_hpCovMapScale;
extern std::atomic<float> g_hpScWorleyStrength;
extern std::atomic<float> g_hpScHeightScale;
extern std::atomic<float> g_hpScCellThickStr;
extern std::atomic<float> g_hpForwardEcc;
extern std::atomic<float> g_hpBackwardEcc;
extern std::atomic<float> g_hpPhiFwdIntensity;
extern std::atomic<float> g_hpPhiFwdDepthPow;
extern std::atomic<float> g_hpPhiFwdDepthBias;
extern std::atomic<float> g_hpPhiFwdCompress;
extern std::atomic<float> g_hpScatterOdScale;
extern std::atomic<float> g_hpScatterCurvePow;
extern std::atomic<float> g_hpMsEccentricity;
extern std::atomic<float> g_hpAoUpwardScale;
extern std::atomic<float> g_hpFadeStart;
extern std::atomic<float> g_hpPowderIntensity;
extern std::atomic<float> g_cloudWeatherScale;
extern std::atomic<float> g_cloudSilver;
extern std::atomic<float> g_cloudViewDistance;
extern std::atomic<float> g_cloudPhiBoundaryConfidence;
extern std::atomic<float> g_cloudPhiMsBuildScale;
extern std::atomic<float> g_cloudPhiCompress;
extern std::atomic<float> g_cloudPhiConeRatio;
extern std::atomic<float> g_cloudSourceOdScale;
extern std::atomic<float> g_cloudSourceCurvePow;
extern std::atomic<float> g_cloudAoUpwardScale;
extern std::atomic<float> g_cloudSunsetAmbientTint;
extern std::atomic<float> g_cloudBottomSmoothHeight;
extern std::atomic<float> g_cloudBottomSmoothPow;
extern std::atomic<float> g_cloudTwoTierThreshold;
extern std::atomic<float> g_cloudMoonStrength;
extern std::atomic<float> g_cloudMoonPhase;
extern std::atomic<float> g_cloudDetailScale;
extern std::atomic<float> g_cloudDetailContrast;
extern std::atomic<float> g_cloudProfileCurve;
extern std::atomic<float> g_cloudAmbientTopMul;
extern std::atomic<float> g_cloudAmbientBottomMul;
extern std::atomic<float> g_cloudAmbientSunSide;
extern std::atomic<float> g_cloudAmbientHueFollow;
extern std::atomic<float> g_cloudSkyHueCool;
extern std::atomic<float> g_cloudSkyHueEnergy;
extern std::atomic<float> g_cloudSkyDirStrength;
extern std::atomic<float> g_cloudAmbientUnify;
extern std::atomic<float> g_cloudGroundAlbedo;
extern std::atomic<float> g_cloudGroundBounce;
extern std::atomic<float> g_cloudSunlightResidual;
extern std::atomic<float> g_cloudPhaseFloor;
extern std::atomic<float> g_cloudScatterTint;
extern std::atomic<float> g_cloudMsOctaves;
extern std::atomic<float> g_cloudMsAttenuation;
extern std::atomic<float> g_cloudMsEccentricity;
extern std::atomic<float> g_cloudSunPathReddening;
extern std::atomic<float> g_cloudAtmExtinction;
extern std::atomic<float> g_cloudPowderMode;
extern std::atomic<float> g_fogDynamicLightStrength;
extern std::atomic<float> g_fogGroundAnchorStrength;
extern std::atomic<float> g_fogSkyAmbientStrength;
extern std::atomic<float> g_fogTemporalJitterStrength;
extern std::atomic<bool> g_cloudFogLightDescriptorsReady;
extern VkDescriptorSet g_cloudFogLightSetBound;
extern VkBuffer g_cloudFogLightListBound;
extern VkBuffer g_cloudFogLightGridBound;
extern std::atomic<float> g_cloudSkyVisStrength;
extern std::atomic<float> g_cloudSkyVisSteps;
extern std::atomic<float> g_cloudTemporalDamp;
extern std::atomic<float> g_cloudBottomOcclusion;
extern std::atomic<float> g_cloudAerialStrength;
extern std::atomic<float> g_groundAerialStrength;
extern std::atomic<float> g_groundAerialOcclusion;
extern std::atomic<float> g_cloudProfileType;
extern std::atomic<float> g_cloudProfileTypeSpread;
extern std::atomic<float> g_cloudWispyStrength;
extern std::atomic<float> g_cloudWispyEdgeWidth;
extern std::atomic<float> g_cloudWispyReach;
extern std::atomic<float> g_cloudWispyTopHeight;
extern std::atomic<float> g_waterCrestBacklit;
extern std::atomic<float> g_waterCausticDispersion;
extern std::atomic<float> g_waterCausticDepthDiffusion;
extern std::atomic<float> g_waterPhysicalPhase;
extern std::atomic<float> g_sunDiskLuminance;
extern std::atomic<float> g_sunHaloStrength;
extern std::atomic<float> g_sunAngularDiameterDegrees;
extern std::atomic<float> g_starIntensity;
extern std::atomic<float> g_sunTintStrength;
extern std::atomic<bool> g_skyScatterEnabled;
extern std::atomic<float> g_skyScatterIntensity;
extern std::atomic<float> g_ozoneStrength;
extern std::atomic<float> g_cloudPowderStrength;

// 2026-11-01 天空散射模型（0 = Bruneton 预计算 LUT，1 = Hillaire 结构）
// + A 档辐亮度标定倍率。车道 = CameraUBO.skyModelParams.x / .y，
// 设置界面经 Java_com_example_vulkan_NativeVulkan_setSkyModelSettings 下发。
extern std::atomic<int> g_skyModel;
extern std::atomic<float> g_skyModelScale;
// 2026-11-01 追加两个大气可调项（车道 = CameraUBO.skyModelParams.z / .w）：
//   g_skyNightAmbient = 夜间天空底光倍率（0 = 关，1 = 出厂）
//   g_skyRainDim      = 雨天/雷暴天空压暗倍率（0 = 不压暗，1 = 出厂，>1 更狠）
extern std::atomic<float> g_skyNightAmbient;
extern std::atomic<float> g_skyRainDim;
extern std::atomic<float> g_auroraStrength;
extern std::atomic<float> g_auroraBrightness;
extern std::atomic<float> g_auroraFlowSpeed;
extern std::atomic<float> g_galaxyScale;
extern std::atomic<float> g_galaxyTiltDegrees;
extern std::atomic<float> g_galaxyAzimuthDegrees;
extern std::atomic<float> g_galaxyMix;
extern std::atomic<float> g_cloudSunForwardScattering;
extern std::atomic<float> g_fogBaseDensity;
extern std::atomic<float> g_fogHeightBlock;
extern std::atomic<float> g_fogScaleHeight;
extern std::atomic<float> g_fogAnisotropyG;
extern std::atomic<float> g_fogSunStrength;
extern std::atomic<float> g_fogAmbientStrength;
extern std::atomic<float> g_fogMaxDistance;
extern std::atomic<float> g_fogStartDistance;
extern std::atomic<uint32_t> g_fogSunSteps;
extern std::atomic<bool> g_fogEnabled;
extern std::atomic<float> g_fogAmount;
extern std::atomic<float> g_fogCenterHeight;
extern std::atomic<float> g_fogThickness;
extern std::atomic<float> g_fogScale;
extern std::atomic<float> g_fogSpeed;
extern std::atomic<uint32_t> g_fogDetailLayers;
extern std::atomic<float> g_fogViewDistance;
extern std::atomic<uint32_t> g_fogSteps;
extern std::atomic<float> g_fogExtinction;
extern std::atomic<float> g_fogRainBoost;
extern std::atomic<float> g_fogDawnBoost;
extern std::atomic<bool> g_fogGroundMistEnabled;
extern std::atomic<float> g_fogGroundMistHeight;
extern std::atomic<float> g_fogGroundMistExtinction;
extern std::atomic<float> g_fogGroundMistDistance;
// 2026-10-31：g_fogWisp* 四项随"雨天地面雾浪"整链删除（用户实测无效果）。
extern std::atomic<float> g_fogRainMediumMix;
extern float g_cloudWorldAnchorX;
extern float g_cloudWorldAnchorZ;
extern float g_cloudWorldAnchorDimension;
extern bool g_cloudWorldAnchorValid;
extern std::atomic<bool> g_rainWallEnabled;
extern std::atomic<float> g_rainWallWetness;
extern std::atomic<float> g_rainWallDripDensity;
extern std::atomic<float> g_rainWallDripSpeed;
extern std::atomic<float> g_rainWallDripSize;
extern std::atomic<float> g_rainWallDripBump;
extern std::atomic<float> g_rainWallDistance;
extern std::atomic<float> g_waterStyleWaveScale;
extern std::atomic<float> g_waterStyleTintMix;
extern std::atomic<float> g_waterStyleDensityScale;
extern std::atomic<float> g_waterStyleCausticStrength;
extern std::atomic<float> g_waterStyleWaveSpeed;
extern std::atomic<float> g_waterStyleSwellScale;
extern std::atomic<float> g_waterStyleMidScale;
extern std::atomic<float> g_waterStyleFineScale;
extern std::atomic<float> g_waterWaveCalibration;
extern std::atomic<float> g_waterWaveStrength;
extern std::atomic<float> g_waterWaveBlend;
extern std::atomic<float> g_waterReflectCausticStrength;
extern std::atomic<float> g_waterSimSpongeWidth;
extern std::atomic<float> g_waterSimGaussianSource;
extern std::atomic<float> g_waterSimDampingScale;
extern std::atomic<float> g_waterSimPropagationSpeed;
extern std::atomic<float> g_waterSssNonlinear;
extern std::atomic<float> g_waterSssBoost;
extern std::atomic<float> g_waterSssPhaseG;
extern std::atomic<float> g_waterSssStrength;
extern std::atomic<float> g_waterWaveLengthScale;
extern std::atomic<float> g_waterWaveDirection;
extern std::atomic<float> g_waterWaveCrestSharpness;
extern std::atomic<float> g_waterWaveWarpStrength;
extern std::atomic<float> g_waterWaveFbmWavelength;
extern std::atomic<float> g_waterWaveFbmWarp;
extern std::atomic<float> g_waterWaveFbmOctaves;
extern std::atomic<float> g_waterWaveFbmWaves;
extern std::atomic<uint32_t> g_perfAoNearSamples;
extern std::atomic<uint32_t> g_perfAoMidSamples;
extern std::atomic<uint32_t> g_perfGiBounces;
extern std::atomic<uint32_t> g_perfSunShadowSamples;
extern std::atomic<uint32_t> g_perfLocalLightRisCandidates;
extern std::atomic<uint32_t> g_perfLocalLightAreaSamples;
extern std::atomic<uint32_t> g_perfWaterReflectionInterval;
extern std::atomic<uint32_t> g_perfGlossyFullDistance;
extern std::atomic<uint64_t> g_perfDynamicRtasInterval;
extern std::atomic<float> g_perfRtSpp;


extern std::atomic<uint32_t> g_giRoulette;
extern std::atomic<uint32_t> g_motionDebugView;
extern std::atomic<uint32_t> g_motionVectorsEnabled;
extern std::atomic<uint32_t> g_sunMarchFarDistance;
extern bool g_reflexSupported;
extern bool g_pclSupported;
extern bool g_reflexLowLatencyAvailable;
extern int g_reflexMode;
extern std::mutex g_reflexMutex;



extern sl::FrameToken*
    g_currentSLFrameToken;
extern uint32_t
    g_currentSLFrameIndex;
extern uint32_t
    g_nextSLFrameIndex;
extern bool
    g_currentSLFrameValid;
extern uint32_t
    g_lastRenderSubmitStartFrame;
extern uint32_t
    g_lastRenderSubmitEndFrame;
extern uint32_t
    g_lastPresentStartFrame;
extern uint32_t
    g_lastPresentEndFrame;


extern bool g_slInitialized;
extern PFN_slGetFeatureFunction g_slGetFeatureFunction;
extern PFN_slGetFeatureRequirements
    g_slGetFeatureRequirements;


extern PFN_slInit g_slInit;
extern PFN_slShutdown g_slShutdown;
extern PFN_slSetVulkanInfo g_slSetVulkanInfo;
extern PFN_slIsFeatureSupported g_slIsFeatureSupported;
extern PFN_slSetFeatureLoaded g_slSetFeatureLoaded;
extern PFN_slReflexSetOptions
    g_slReflexSetOptions;
extern PFN_slReflexGetState
    g_slReflexGetState;
extern PFN_slReflexSleep
    g_slReflexSleep;
extern PFN_slPCLSetMarker
    g_slPCLSetMarker;
extern PFN_slDLSSSetOptions g_slDLSSSetOptions;
extern PFN_slDLSSGetOptimalSettings
    g_slDLSSGetOptimalSettings;
extern PFN_slDLSSDSetOptions
    g_slDLSSDSetOptions;
extern PFN_slDLSSDGetOptimalSettings
    g_slDLSSDGetOptimalSettings;


extern PFN_slEvaluateFeature g_slEvaluateFeature;
extern sl::Constants g_slConstants;
extern PFN_slSetConstants g_slSetConstants;
extern PFN_slGetNewFrameToken g_slGetNewFrameToken;
extern PFN_slSetTagForFrame
    g_slSetTagForFrame;
extern PFN_slFreeResources g_slFreeResources;
extern PFN_slDLSSGetState g_slDLSSGetState;
extern PFN_slDLSSDGetState g_slDLSSDGetState;
extern std::atomic<uint64_t> g_slVramDlssBytes;
extern std::atomic<uint64_t> g_slVramDlssdBytes;
extern std::atomic<uint64_t> g_slVramDlssgBytes;
extern VkImageView g_atlasImageView;
extern VkSampler g_atlasSampler;
extern bool g_atlasDirty;
extern VkSampler g_pbrAtlasSampler;
extern VkImageView g_atlasImageViewAtReload;
extern VkSampler g_atlasSamplerAtReload;
extern bool g_atlasReloadAwaitingPublish;
extern VkImage     g_dhColorImage;
extern VkImageView g_dhColorView;
extern VkSampler   g_dhColorSampler;
extern VkImage     g_dhDepthImage;
extern VkImageView g_dhDepthView;
extern VkSampler   g_dhDepthSampler;
extern VkImage     g_dhAtlasImage;
extern VkImageView g_dhAtlasView;
extern VkSampler   g_dhAtlasSampler;
extern int         g_dhTextureWidth;
extern int         g_dhTextureHeight;
extern bool        g_dhTexturesValid;
extern bool        g_dhTexturesSameDevice;
extern uint64_t    g_dhTexturesRevision;
extern uint64_t    g_dhTexturesBoundRevision;
extern bool        g_dhFallbackWritten;
extern VkImageView g_dhFallbackAtlasView;
extern bool        g_dhAtlasUsable;
extern uint64_t    g_dhAtlasRevision;
extern uint64_t    g_dhAtlasBoundRevision;
extern float g_dhFarFieldInvMvp[16];
extern float g_dhFarFieldMvp[16];
extern float g_dhFarFieldMode;
extern float g_dhFarFieldStrength;
extern float g_dhFarFieldGain;
extern float g_dhFarFieldVariant;
extern float g_dhFarFieldNear;
extern float g_dhFarFieldFar;
extern bool  g_dhFarFieldMatrixValid;
extern int   g_dhFarFieldPushes;
extern int   g_dhFarFieldLogs;
extern int   g_dhFarFieldRejects;
extern int   g_dhFarFieldRejectLogs;
extern bool g_dynamicTextureBindingsNeedAtlasRefresh;
extern bool g_dynamicTextureBindingsCanPublish;
extern std::mutex g_rtxMemAccountMutex;
extern std::unordered_map<VkDeviceMemory, RtxMemLiveRecord> g_rtxMemLive;
extern std::unordered_map<uint64_t, RtxBufferChurnStats> g_rtxBufferChurn;
extern uint64_t g_rtxMemAllocCount;
extern uint64_t g_rtxMemFreeCount;
extern uint64_t g_rtxMemFreeUnknownCount;
extern uint64_t g_rtxMemAllocBytesTotal;
extern uint64_t g_rtxMemFreeBytesTotal;
extern uint64_t g_rtxMemLiveBytes;
extern uint64_t g_rtxMemLiveDeviceLocalBytes;
extern uint64_t g_rtxMemLiveHostBytes;
extern uint64_t g_rtxMemLivePeakBytes;
extern RtxMemObjCounters g_rtxMemObj;
extern VkBuffer g_perfL10RayStatsBuffer;
extern VkDeviceMemory g_perfL10RayStatsMemory;
extern VkBuffer g_perfL10RayStatsStagingBuffer;
extern VkDeviceMemory g_perfL10RayStatsStagingMemory;
extern uint32_t* g_perfL10RayStatsStagingMapped;
extern uint64_t g_perfL10RayStatsSums[PERF_L10_RAY_STAT_COUNT];
extern uint64_t g_perfL10RayStatsWindowFrames;
extern uint32_t g_perfL10RayStatsActiveSlot;
extern PerfL10RayStatsSlot
    g_perfL10RayStatsSlots[PERF_L10_RAY_STAT_RING_SIZE];
extern VkBuffer g_textureUploadStaging;
extern VkDeviceMemory g_textureUploadStagingMemory;
extern void* g_textureUploadStagingMapped;
extern VkDeviceSize g_textureUploadStagingBytes;
extern VkCommandBuffer g_textureUploadCommandBuffer;
extern VkFence g_textureUploadFence;
extern bool g_textureUploadFencePending;
extern std::vector<PbrSpriteRect> g_pbrSpriteRects;
extern bool g_pbrBc7AllowForNextUpload;
extern std::atomic<int>   g_postTonemapMode;
extern std::atomic<float> g_postExposureStrength;
extern std::atomic<float> g_postContrast;
extern std::atomic<float> g_postSaturation;
extern std::atomic<float> g_postGamma;
extern std::atomic<float> g_postGainR;
extern std::atomic<float> g_postGainG;
extern std::atomic<float> g_postGainB;
extern std::atomic<int>   g_mirrorBounceCount;
extern std::atomic<int>   g_tlasCullDistance;
extern std::atomic<int>   g_tlasFrustumCullMargin;
extern std::atomic<float> g_postBloomStrength;
extern std::atomic<float> g_cgExposureEV;
extern std::atomic<float> g_cgBlackPoint;
extern std::atomic<float> g_cgWhitePoint;
extern std::atomic<float> g_cgContrast;
extern std::atomic<float> g_cgPivot;
extern std::atomic<float> g_cgLiftR;
extern std::atomic<float> g_cgLiftG;
extern std::atomic<float> g_cgLiftB;
extern std::atomic<float> g_cgGammaR;
extern std::atomic<float> g_cgGammaG;
extern std::atomic<float> g_cgGammaB;
extern std::atomic<float> g_cgGainR;
extern std::atomic<float> g_cgGainG;
extern std::atomic<float> g_cgGainB;
extern std::atomic<float> g_cgShadowHue;
extern std::atomic<float> g_cgShadowStrength;
extern std::atomic<float> g_cgHighlightHue;
extern std::atomic<float> g_cgHighlightStrength;
extern std::atomic<float> g_cgVibrance;
extern std::atomic<float> g_cgFade;
extern std::atomic<float> g_cgVignette;
extern std::atomic<float> g_cgVignetteRadius;
extern std::atomic<float> g_cgHueRotate;
extern std::atomic<float> g_cgTintGreen;
extern PFN_vkCreateAccelerationStructureKHR g_rtxRealCreateAS;
extern PFN_vkDestroyAccelerationStructureKHR g_rtxRealDestroyAS;
extern VkDevice g_rtxRealASDevice;
extern PFN_vkCmdBuildAccelerationStructuresKHR g_rtxRealCmdBuildAS;
extern VkDevice g_rtxRealCmdBuildASDevice;
extern PFN_vkCreateMicromapEXT g_rtxRealCreateMM;
extern PFN_vkDestroyMicromapEXT g_rtxRealDestroyMM;
extern VkDevice g_rtxRealMMDevice;
extern std::timed_mutex g_chunkMutex;
extern std::atomic<uint64_t> g_chunkLockMaxWaitNanos;
extern std::atomic<uint64_t> g_chunkLockMaxHoldNanos;
extern std::atomic<uint64_t> g_captureLockMaxHoldNanos;
extern std::atomic<uint64_t> g_captureLockMaxWaitNanos;
extern std::atomic<uint64_t> g_chunkCmpRaw1MaxNanos;
extern std::atomic<uint64_t> g_chunkCmpRaw2MaxNanos;
extern std::atomic<uint64_t> g_chunkCmpMeshMaxNanos;
extern std::atomic<uint32_t> g_chunkCmpCount;
extern std::atomic<uint64_t> g_chunkUploadCalls;
extern std::atomic<uint64_t> g_chunkUploadDedup;
extern std::atomic<uint64_t> g_chunkCmpLastBytes;
extern std::atomic<uint64_t> g_chunkCmpLastVerts;
extern std::unordered_map<uint64_t, std::vector<Vertex>> g_chunkMeshes;
extern std::unordered_map<uint64_t, std::vector<float>> g_chunkMotion;
extern std::unordered_map<uint64_t, VoxyLodSource> g_voxyLodSources;
extern std::unordered_map<uint64_t, uint64_t> g_voxyLodSpatialOwners;
extern std::vector<Vertex> g_entityFrameVertices;
extern std::vector<Vertex> g_localPlayerFrameVertices;
extern std::vector<Vertex> g_handFrameVertices;
extern uint64_t g_entityCachedHash;
extern bool g_entityCachedHashValid;
extern uint64_t g_localPlayerCachedHash;
extern bool g_localPlayerCachedHashValid;
extern uint64_t g_handCachedHash;
extern bool g_handCachedHashValid;
extern std::vector<Vertex> g_particleFrameVertices;
extern std::vector<float> g_entityFrameMotion;
extern std::vector<float> g_localPlayerFrameMotion;
extern std::vector<float> g_handFrameMotion;
extern std::vector<float> g_particleFrameMotion;
extern float g_dynamicCaptureCameraX;
extern float g_dynamicCaptureCameraY;
extern float g_dynamicCaptureCameraZ;
extern bool g_entityFrameOpen;
extern bool g_particleFrameOpen;
extern std::atomic<bool> g_entityRebuildRequested;
extern uint64_t g_lastDynamicRTASUpdateFrame;
extern bool g_dynamicRTASUpdatePending;
extern uint64_t g_lastLocalPlayerASUpdateFrame;
extern bool g_localPlayerASUpdatePending;
extern VkImageView g_entityTextureViews[ENTITY_RT_TEXTURE_SLOT_COUNT];
extern VkSampler g_entityTextureSamplers[ENTITY_RT_TEXTURE_SLOT_COUNT];
extern bool g_entityTexturesDirty;
extern VkImageView g_entityNormalViews[ENTITY_RT_TEXTURE_SLOT_COUNT];
extern VkSampler g_entityNormalSamplers[ENTITY_RT_TEXTURE_SLOT_COUNT];
extern VkImageView g_entitySpecularViews[ENTITY_RT_TEXTURE_SLOT_COUNT];
extern VkSampler g_entitySpecularSamplers[ENTITY_RT_TEXTURE_SLOT_COUNT];
extern bool g_entityPbrTexturesDirty;
extern VkImage g_entityPbrFallbackImage;
extern VkDeviceMemory g_entityPbrFallbackMemory;
extern VkImageView g_entityPbrFallbackView;
extern VkSampler g_entityPbrFallbackSampler;
extern VkImageView g_particleTextureViews[PARTICLE_RT_TEXTURE_SLOT_COUNT];
extern VkSampler g_particleTextureSamplers[PARTICLE_RT_TEXTURE_SLOT_COUNT];
extern bool g_particleTexturesDirty;
extern VkImageView g_rainStreakTextureView;
extern VkSampler g_rainStreakTextureSampler;
extern bool g_rainStreakTextureDirty;
extern VkImageView g_starMapTextureView;
extern VkSampler g_starMapTextureSampler;
extern bool g_starMapTextureDirty;
extern VkImageView g_endSkyAtlasView;
extern VkSampler g_endSkyAtlasSampler;
extern bool g_endSkyAtlasDirty;
extern VkImage g_voxyAtlasImage;
extern VkDeviceMemory g_voxyAtlasMemory;
extern VkImageView g_voxyAtlasView;
extern VkSampler g_voxyAtlasSampler;
extern uint64_t g_voxyAtlasContentHash;
extern VkDescriptorSet g_voxyAtlasDescriptorSet;
extern uint32_t g_voxyAtlasSize;
extern bool g_particleDebugEpisodeActive;
extern uint64_t g_particleDebugEpisodeSerial;
extern bool g_rainPuddleLoggedGeometry;
extern std::unordered_map<uint64_t, std::vector<uint8_t>>
    g_chunkRawPayloads;
extern std::unordered_map<uint64_t, ChunkUploadDigest> g_chunkUploadDigests;
extern std::mutex g_visibleSectionKeysMutex;
extern std::vector<uint64_t> g_visibleSectionKeys;
extern std::atomic<uint32_t> g_visibleSectionKeyCount;
extern std::unordered_set<uint64_t> g_dirtySectionKeys;
extern std::atomic<bool> g_forceSectionASReset;
extern std::atomic<bool> g_worldGeometryDirty;
extern std::atomic<uint64_t> g_worldGeometryRevision;
extern std::atomic<uint64_t> g_terrainGeometryRevision;
extern std::mutex g_capturedSectionsMutex;
extern std::vector<CapturedSectionUpdate> g_capturedSections;
extern size_t g_capturedSectionsHead;
extern std::thread g_geometryCaptureThread;
extern std::mutex g_geometryCaptureStateMutex;
extern std::condition_variable g_geometryCaptureWake;

// 2026-10-31：脏标记 → 捕获线程的唤醒信号。
//   g_captureWorkRevision：每来一次脏标记就 +1，消费者拿它做等待谓词；
//   g_captureWorkerWaiting：消费者真的睡着时才置位，避免每次上传都做一次
//   notify（C2ME 也是只在"空→非空"这一条边上通知）。
extern std::atomic<uint64_t> g_captureWorkRevision;
extern std::atomic<bool> g_captureWorkerWaiting;
extern bool g_geometryCaptureStop;
extern bool g_geometryCaptureStarted;

// 2026-10-31：脏 Section 取序的观测计数，只有捕获线程读写，不需要原子。
extern uint64_t g_dirtyOrderWakeCount;
extern uint64_t g_dirtyOrderScanKeyMax;
extern uint64_t g_dirtyOrderScanMicrosMax;
extern std::atomic<uint64_t> g_geometryCapturedSectionTotal;
extern uint64_t g_geometryAdoptedSectionTotal;
extern std::atomic<uint64_t> g_geometryForceResetCount;


extern std::mutex
    g_lightMutex;


extern std::unordered_map<
    uint64_t,
    std::vector<LocalLightRecord>
> g_lightSections;


extern size_t
    g_totalLocalLights;


extern VkBuffer
    g_localLightGpuBuffer;


extern VkDeviceMemory
    g_localLightGpuMemory;


extern size_t
    g_localLightGpuCount;


extern std::atomic<bool>
    g_localLightsDirty;
extern VkBuffer g_sharcHashEntriesBuffer;
extern VkDeviceMemory g_sharcHashEntriesMemory;
extern VkBuffer g_sharcAccumulationBuffer;
extern VkDeviceMemory g_sharcAccumulationMemory;
extern VkBuffer g_sharcResolvedBuffer;
extern VkDeviceMemory g_sharcResolvedMemory;
extern VkBuffer g_sharcLockBuffer;
extern VkDeviceMemory g_sharcLockMemory;
extern uint32_t g_sharcEntryCountResolved;
extern bool g_sharcBuffersReady;
extern bool g_sharcZeroed;
extern bool g_sharcCreateFailed;
extern uint64_t g_sharcHashEntriesAddress;
extern uint64_t g_sharcAccumulationAddress;
extern uint64_t g_sharcResolvedAddress;
extern uint64_t g_sharcLockAddress;
extern VkBuffer g_sharcProbeStagingBuffer;
extern VkDeviceMemory g_sharcProbeStagingMemory;
extern uint32_t g_sharcProbeStage;
extern uint32_t g_sharcProbeTickCount;
extern uint32_t g_sharcProbeArmTick;
inline constexpr const uint32_t SHARC_PROBE_HASH_LO = 0x22222222u;
inline constexpr const uint32_t SHARC_PROBE_HASH_HI = 0x11111111u;
inline constexpr const uint32_t SHARC_PROBE_ACCUM_X = 0x33333333u;
inline constexpr const uint32_t SHARC_PROBE_RESOLVED_W0 = 0x44444444u;
inline constexpr const uint32_t SHARC_PROBE_RESOLVED_W1 = 0x45454545u;
inline constexpr const uint32_t SHARC_PROBE_RESOLVED_SAMPLE = 0x46464646u;
inline constexpr const uint32_t SHARC_PROBE_RESOLVED_SAMPLE_EXT = 0x47474747u;
inline constexpr const uint32_t SHARC_PROBE_LOCK_MAGIC = 0x5A170001u;
inline constexpr const uint32_t SHARC_AB_FRAMES_PER_ARM = 120u;
extern uint32_t g_sharcAbLoggedArm;
extern VkPipelineLayout g_sharcResolvePipelineLayout;
extern VkPipeline g_sharcResolvePipeline;
extern bool g_sharcResolvePipelineFailed;


extern VkBuffer
    g_localLightGridGpuBuffer;


extern VkDeviceMemory
    g_localLightGridGpuMemory;


extern size_t
    g_localLightGridCellCount;


extern size_t
    g_localLightGridIndexCount;


extern size_t
    g_localLightGridHashCapacity;


extern bool
    g_cameraHistoryValid;


extern int32_t
    g_lastCameraFrame;
extern float
    g_prevCameraViewInverse[16];
extern float
    g_prevCameraProjInverse[16];
extern bool
    g_hasPrevCameraInverse;


extern uint32_t
    g_localLightRevision;
extern VkBuffer g_vptDirectHistoryBuffers[2];
extern VkDeviceMemory g_vptDirectHistoryMemories[2];
extern uint32_t g_vptDirectHistoryWidth;
extern uint32_t g_vptDirectHistoryHeight;
extern VkDeviceSize g_vptDirectHistoryBytes;
extern bool g_vptDirectHistoryNeedsClear;
// 2026-11-02：per-tile best-light（NEE-AT/RTXDI 的 RTXDI_FindGradientSampleAndBestLight
// 等价物）。一块 buffer 分两半，按 cam.historyReadIndex 做 ping-pong；每 tile 一个
// r32ui，内容是打包键 (量化亮度 << 7) | (tile 内像素索引 << 1) | 帧奇偶。
extern VkBuffer g_vptBestLightBuffer;
extern VkDeviceMemory g_vptBestLightMemory;
extern VkDeviceSize g_vptBestLightBytes;
extern uint32_t g_vptBestLightTileColumns;
extern uint32_t g_vptBestLightTileRows;
extern std::mutex g_vliMutex;
extern std::unordered_map<uint64_t, VLISectionCpuPage> g_vliSections;
extern std::unordered_map<uint64_t, std::vector<uint64_t>> g_vliChunkSectionIndex;
extern std::vector<VLITableSlot> g_vliTableCpu;
extern std::vector<uint32_t> g_vliFreePages;
extern uint32_t g_vliNextPage;
extern uint32_t g_vliPageCapacity;
extern bool g_vliGrowthRequested;
extern std::vector<VLIPendingSection> g_vliPendingSections;
extern std::mutex g_vliUploadHashMutex;
extern std::atomic<uint64_t> g_vliUploadTotal;
extern std::mutex g_vliHotMutex;
extern std::unordered_map<uint64_t, uint32_t> g_vliHotCounts;
extern std::atomic<uint64_t> g_vliUploadGated;
extern std::atomic<uint64_t> g_vliUploadApplied;
extern std::unordered_map<uint64_t, uint64_t> g_vliUploadHashByKey;
extern std::vector<uint64_t> g_vliPageSectionKeys;
extern uint64_t g_vliPageUseTick;
extern uint64_t g_vliGovernorEvictedPages;
extern uint64_t g_vliGovernorIgnoredSections;


extern std::unordered_set<uint32_t> g_vliDirtySourcePages;
extern std::unordered_set<uint32_t> g_vliDirtyTableSlots;
extern std::unordered_set<uint32_t> g_vliDirtySolveTableSlots;
extern std::unordered_set<uint32_t> g_vptDirtyEmitterPages;
extern std::unordered_set<uint32_t> g_vptDirtyEmitterAliasPages;
extern std::unordered_set<uint32_t> g_vptDirtyCandidatePages;
extern std::unordered_set<uint32_t> g_vptDirtySourceDirectoryPages;
extern uint32_t g_vptSourceDirectoryRevision;
extern uint32_t g_vptEmitterRevision;
extern VkBuffer g_vliPageTableBuffer;
extern VkDeviceMemory g_vliPageTableMemory;
extern VkBuffer g_vliPageBuffer;
extern VkDeviceMemory g_vliPageMemory;
extern VkBuffer g_vliScratchBuffer;
extern VkDeviceMemory g_vliScratchMemory;
extern VkBuffer g_vptSurfaceCacheBuffer;
extern VkDeviceMemory g_vptSurfaceCacheMemory;
extern VkBuffer g_vliWorkListBuffer;
extern VkDeviceMemory g_vliWorkListMemory;
extern VkBuffer g_vptEmitterIndexBuffer;
extern VkDeviceMemory g_vptEmitterIndexMemory;
extern VkBuffer g_vptEmitterAliasBuffer;
extern VkDeviceMemory g_vptEmitterAliasMemory;
extern VkBuffer g_vptCandidateBuffer;
extern VkDeviceMemory g_vptCandidateMemory;
extern VkBuffer g_vptSourceDirectoryBuffer;
extern VkDeviceMemory g_vptSourceDirectoryMemory;
extern VkPipelineLayout g_vliComputePipelineLayout;
extern VkPipeline g_vliComputePipeline;
extern bool g_vliNeedsInitialClear;
extern uint32_t g_vptTemporalFrame;
extern VramGovernorState g_vramGovernor;
extern std::vector<SectionMemorySlab>
    g_sectionMemorySlabs;
extern VkDeviceSize g_sectionMemoryPoolKeptBytes;
extern uint64_t g_sectionMemoryPoolHits;
extern uint64_t g_sectionMemoryPoolMisses;
extern uint64_t g_sectionMemoryAllocateCalls;
extern double g_sectionMemoryAllocateMs;
extern uint64_t g_sectionMemoryAllocateBytes;
extern uint64_t g_sectionMemoryFreeBytes;
extern uint64_t g_sectionASCreateCalls;
extern uint64_t g_sectionASRetireCalls;
extern double g_sectionASCreateMs;
extern double g_sectionASRetireMs;
extern uint64_t g_sectionASRebuildSameTris;
extern uint64_t g_sectionASRefitCandidateHits;
extern uint64_t g_sectionASRefitHits;
extern uint64_t g_sectionMemoryOomSkipCount;
extern VramProbeState g_vramProbe;
extern uint64_t g_rtFrameEnterCount;
extern uint64_t g_rtFrameDoneCount;
extern VkBuffer g_sectionQuadIndexBuffer;
extern VkDeviceMemory g_sectionQuadIndexMemory;
extern VkDeviceAddress g_sectionQuadIndexAddress;
extern std::atomic<uint32_t> g_sparseEffectiveFrameIndex;
extern std::atomic<float> g_sparseEffectiveRate;
extern VkImage g_sparseActiveMaskImage;
extern VkDeviceMemory g_sparseActiveMaskMemory;
extern VkImageView g_sparseActiveMaskView;
extern VkImageLayout g_sparseActiveMaskLayout;
extern uint32_t g_sparseActiveMaskWidth;
extern uint32_t g_sparseActiveMaskHeight;
extern bool g_sparseActiveMaskNeedsClear;
extern VkShaderModule g_sparseActiveMaskShader;
extern VkDescriptorSetLayout g_sparseActiveMaskSetLayout;
extern VkDescriptorPool g_sparseActiveMaskPool;
extern VkDescriptorSet g_sparseActiveMaskSet;
extern VkPipelineLayout g_sparseActiveMaskPipelineLayout;
extern VkPipeline g_sparseActiveMaskPipeline;
extern bool g_sparseActiveMaskPipelineFailed;
extern BloomMip g_bloomMips[BLOOM_MIP_COUNT];
extern VkSampler g_bloomSampler;
extern VkDescriptorSetLayout g_bloomPrefilterSetLayout;
extern VkDescriptorSetLayout g_bloomBlurSetLayout;
extern VkDescriptorSetLayout g_bloomCompositeSetLayout;
extern VkDescriptorPool g_bloomDescriptorPool;
extern VkDescriptorSet g_bloomPrefilterSet;
extern VkDescriptorSet g_bloomDownsampleSets[BLOOM_MIP_COUNT];
extern VkDescriptorSet g_bloomUpsampleSets[BLOOM_MIP_COUNT];
extern VkDescriptorSet g_bloomCompositeSet;
extern VkPipelineLayout g_bloomPrefilterPipelineLayout;
extern VkPipelineLayout g_bloomBlurPipelineLayout;
extern VkPipelineLayout g_bloomCompositePipelineLayout;
extern VkPipeline g_bloomPrefilterPipeline;
extern VkPipeline g_bloomDownsamplePipeline;
extern VkPipeline g_bloomUpsamplePipeline;
extern VkPipeline g_bloomCompositePipeline;
extern VkBuffer g_dynamicLightBuffer;
extern VkDeviceMemory g_dynamicLightMemory;
extern DynamicLightUpload g_dynamicLightUpload;
extern bool g_dynamicLightsDirty;
extern VkPipeline g_sharcUpdatePipeline;
extern VkBuffer g_sharcUpdateSbtBuffer;
extern VkDeviceMemory g_sharcUpdateSbtMemory;
extern VkStridedDeviceAddressRegionKHR g_sharcUpdateRaygenRegion;
extern VkStridedDeviceAddressRegionKHR g_sharcUpdateMissRegion;
extern VkStridedDeviceAddressRegionKHR g_sharcUpdateHitRegion;
extern VkStridedDeviceAddressRegionKHR g_sharcUpdateCallableRegion;
extern bool g_sharcUpdatePipelineFailed;


extern VkPipeline g_giHalfPipeline;
extern VkBuffer g_giHalfSbtBuffer;
extern VkDeviceMemory g_giHalfSbtMemory;
extern VkStridedDeviceAddressRegionKHR g_giHalfRaygenRegion;
extern VkStridedDeviceAddressRegionKHR g_giHalfMissRegion;
extern VkStridedDeviceAddressRegionKHR g_giHalfHitRegion;
extern VkStridedDeviceAddressRegionKHR g_giHalfCallableRegion;
extern bool g_giHalfPipelineFailed;

// 2026-11-05 B：K1(主视线)/K2(传输) 拆分
extern VkPipeline g_k1Pipeline;
extern VkPipeline g_k2Pipeline;
// ③ K2-D/K2-I 直射/间接再拆分（默认开；ORION_K2_SPLIT=0 时与 g_k2Pipeline 互斥创建）
extern VkPipeline g_k2dPipeline;
extern VkPipeline g_k2iPipeline;
extern VkStridedDeviceAddressRegionKHR g_k2dRaygenRegion;
extern VkStridedDeviceAddressRegionKHR g_k2dMissRegion;
extern VkStridedDeviceAddressRegionKHR g_k2dHitRegion;
extern VkStridedDeviceAddressRegionKHR g_k2dCallableRegion;
extern VkStridedDeviceAddressRegionKHR g_k2iRaygenRegion;
extern VkStridedDeviceAddressRegionKHR g_k2iMissRegion;
extern VkStridedDeviceAddressRegionKHR g_k2iHitRegion;
extern VkStridedDeviceAddressRegionKHR g_k2iCallableRegion;
// ③ K2-D→K2-I 直射级交接图（binding 89，rgba32f，全分辨率）
extern VkImage g_k2DirectImage;
extern VkDeviceMemory g_k2DirectMemory;
extern VkImageView g_k2DirectView;
extern VkImageLayout g_k2DirectLayout;
extern uint32_t g_k2DirectWidth;
extern uint32_t g_k2DirectHeight;
extern VkBuffer g_k1LandingBuffer;
extern VkDeviceMemory g_k1LandingMemory;
extern VkBuffer g_k1k2SbtBuffer;
extern VkDeviceMemory g_k1k2SbtMemory;
extern VkStridedDeviceAddressRegionKHR g_k1RaygenRegion;
extern VkStridedDeviceAddressRegionKHR g_k1MissRegion;
extern VkStridedDeviceAddressRegionKHR g_k1HitRegion;
extern VkStridedDeviceAddressRegionKHR g_k1CallableRegion;
extern VkStridedDeviceAddressRegionKHR g_k2RaygenRegion;
extern VkStridedDeviceAddressRegionKHR g_k2MissRegion;
extern VkStridedDeviceAddressRegionKHR g_k2HitRegion;
extern VkStridedDeviceAddressRegionKHR g_k2CallableRegion;
extern bool g_k1K2PipelineFailed;
extern uint32_t g_k1LandingWidth;
extern uint32_t g_k1LandingHeight;
extern uint32_t g_k1LandingSlots;

void ensureK1K2Pipelines();
void destroyK1K2Pipeline();
void ensureK1LandingBuffer(uint32_t width, uint32_t height, uint32_t slots);
void destroyK1LandingBuffer();
void createK2DirectImage(uint32_t width, uint32_t height);
void destroyK2DirectImage();
// ③ K2-D/K2-I 再拆分（默认开，ORION_K2_SPLIT=0 回退）：K1→K2D(直射)→K2I(间接) 三发
bool k2DirectIndirectSplitEnabled();
// 返回 true 表示 K1→K2 两发已录完并已重新绑定主管线；false 表示回退单发主管线
bool traceK1K2(VkCommandBuffer cmd, uint32_t width, uint32_t height);


inline constexpr const uint32_t SHARC_STATS_FRAMES = 300u;
extern uint32_t g_sharcStatsFrameTick;
extern bool g_sharcStatsDone;
extern VkDescriptorPool g_rtDescriptorRingPool;
extern VkDescriptorSet g_rtDescriptorRingSets[RT_DESCRIPTOR_RING_SIZE];
extern VkFence g_rtDescriptorRingFences[RT_DESCRIPTOR_RING_SIZE];
extern bool g_rtDescriptorRingFencePending[RT_DESCRIPTOR_RING_SIZE];
extern uint32_t g_rtDescriptorRingNext;
extern uint32_t g_rtDescriptorRingLiveIndex;
extern VkCommandPool g_rtDescriptorRingCmdPool;
extern VkCommandBuffer g_rtDescriptorRingCmd;
extern bool g_rtDescriptorRingReady;
extern bool g_rtDescriptorRingLastPublishUsedRing;
extern uint32_t g_rtDescriptorRingUsedMask;
extern size_t g_vliPendingBacklogForLog;
extern double g_vliMsIndex;
extern double g_vliMsAlias;
extern double g_vliMsApply;
extern double g_vliMsGather;
extern double g_vliMsUpload;
extern double g_vliMsLockWait;
extern double g_vliMsSolve;
extern size_t g_vliGatherPages;
extern std::vector<uint32_t> g_vliUploadArena;
extern std::vector<VliUploadRef> g_vliSourceUploads;
extern std::vector<VliUploadRef> g_vliEmitterUploads;
extern std::vector<VliUploadRef> g_vliEmitterAliasUploads;


inline constexpr const char* const NRD_SPIRV_ENTRY_POINT = "main";
extern std::vector<uint32_t> g_nrdConstantBufferOffsets;
extern VkPipeline g_diConfidencePipeline;
extern VkPipelineLayout g_diConfidencePipelineLayout;
extern VkDescriptorSetLayout g_diConfidenceSetLayout;
extern VkDescriptorPool g_diConfidencePool;
extern VkDescriptorSet g_diConfidenceSets[2];
extern VkImage g_diGradientImage;
extern VkDeviceMemory g_diGradientMemory;
extern VkImageView g_diGradientView;
extern VkImageLayout g_diGradientLayout;
extern VkImage g_diConfidenceImages[2];
extern VkDeviceMemory g_diConfidenceMemories[2];
extern VkImageView g_diConfidenceViews[2];
extern VkImageLayout g_diConfidenceLayouts[2];
extern VkImage g_diLumaImages[2];
extern VkDeviceMemory g_diLumaMemories[2];
extern VkImageView g_diLumaViews[2];
extern VkImageLayout g_diLumaLayouts[2];
extern uint32_t g_diConfidenceWidth;
extern uint32_t g_diConfidenceHeight;
extern uint32_t g_diConfidenceParity;
extern bool g_diConfidenceReady;
extern uint32_t g_diConfidenceWarmupFrames;
extern bool g_diConfidenceDescriptorsDirty;
extern VkImageView g_nrdConfidenceViewThisFrame;
extern bool g_directAtrousEnabled;
extern bool g_directAtrousReady;
extern VkPipeline g_directAtrousPipeline;
extern VkPipelineLayout g_directAtrousPipelineLayout;
extern VkDescriptorSetLayout g_directAtrousSetLayout;
extern VkDescriptorPool g_directAtrousPool;
extern VkDescriptorSet g_directAtrousSets[3];
extern uint32_t g_directAtrousWidth;
extern uint32_t g_directAtrousHeight;
extern VkImage g_diLinearImages[2];
extern VkDeviceMemory g_diLinearMemories[2];
extern VkImageView g_diLinearViews[2];
extern VkImageLayout g_diLinearLayouts[2];
extern VkImage g_diVarianceImages[2];
extern VkDeviceMemory g_diVarianceMemories[2];
extern VkImageView g_diVarianceViews[2];
extern VkImageLayout g_diVarianceLayouts[2];
extern VkPipeline g_diReprojectPipeline;
extern VkPipelineLayout g_diReprojectPipelineLayout;
extern VkDescriptorSetLayout g_diReprojectSetLayout;
extern VkDescriptorPool g_diReprojectPool;
extern VkDescriptorSet g_diReprojectSets[2];
extern VkPipeline g_diMomentsPipeline;
extern VkPipelineLayout g_diMomentsPipelineLayout;
extern VkDescriptorSetLayout g_diMomentsSetLayout;
extern VkDescriptorPool g_diMomentsPool;
extern VkDescriptorSet g_diMomentsSets[2];
extern VkImage g_diHistoryColorImages[2];
extern VkDeviceMemory g_diHistoryColorMemories[2];
extern VkImageView g_diHistoryColorViews[2];
extern VkImageLayout g_diHistoryColorLayouts[2];
extern VkImage g_diHistoryDepthImages[2];
extern VkDeviceMemory g_diHistoryDepthMemories[2];
extern VkImageView g_diHistoryDepthViews[2];
extern VkImageLayout g_diHistoryDepthLayouts[2];
extern VkImage g_diHistoryNormalImages[2];
extern VkDeviceMemory g_diHistoryNormalMemories[2];
extern VkImageView g_diHistoryNormalViews[2];
extern VkImageLayout g_diHistoryNormalLayouts[2];
extern VkImage g_diHistoryLengthImages[2];
extern VkDeviceMemory g_diHistoryLengthMemories[2];
extern VkImageView g_diHistoryLengthViews[2];
extern VkImageLayout g_diHistoryLengthLayouts[2];
extern VkImage g_diMomentsImages[2];
extern VkDeviceMemory g_diMomentsMemories[2];
extern VkImageView g_diMomentsViews[2];
extern VkImageLayout g_diMomentsLayouts[2];
extern uint32_t g_diHistoryParity;
extern bool g_diHistoryNeedsReset;
extern VkPipeline g_diPreparePipeline;
extern VkPipelineLayout g_diPreparePipelineLayout;
extern VkDescriptorSetLayout g_diPrepareSetLayout;
extern VkDescriptorPool g_diPreparePool;
extern VkDescriptorSet g_diPrepareSet;
extern uint64_t g_sunShadowBuiltRevision;
extern float g_sunShadowBuiltPhase;
extern float g_sunShadowBuiltCenter[3];
extern uint64_t g_sunShadowFarBuiltRevision;
extern float g_sunShadowFarBuiltPhase;
extern float g_sunShadowFarBuiltCenter[3];
extern uint64_t g_cloudSpvStampSeen;
extern int g_cloudSpvWatchCountdown;
extern BrunetonLutImage g_brTransmittance;
extern BrunetonLutImage g_brIrradiance;
extern BrunetonLutImage g_brDeltaIrradiance;
extern BrunetonLutImage g_brScattering;
extern BrunetonLutImage g_brSingleMie;
extern BrunetonLutImage g_brDeltaRayleigh;
extern BrunetonLutImage g_brDeltaMie;
extern BrunetonLutImage g_brDeltaMultiple;
extern BrunetonLutImage g_brDeltaScatteringDensity;
extern VkSampler g_brLutSampler;
extern VkDescriptorSetLayout g_brSetLayout;
extern VkDescriptorPool g_brPool;
extern VkDescriptorSet g_brSet;
extern VkPipelineLayout g_brLayout;
extern VkPipeline g_brPipelines[6];
extern bool g_brPrecomputed;
extern VkBuffer g_brReadbackBuffer;
extern VkDeviceMemory g_brReadbackMemory;
extern bool g_brPrecomputeOnlyReleased;
extern VkImage g_brPlaceholderImage;
extern VkDeviceMemory g_brPlaceholderMemory;
extern VkImageView g_brPlaceholderView;
extern VkImage g_waterFftStateImage;
extern VkDeviceMemory g_waterFftStateMemory;
extern VkImageView g_waterFftStateView;
extern VkImage g_waterFftSlopeImage;
extern VkDeviceMemory g_waterFftSlopeMemory;
extern VkImageView g_waterFftSlopeView;
extern VkImageView g_waterFftSlopeStorageView;
extern VkSampler g_waterFftSlopeSampler;
extern VkDescriptorSetLayout g_waterFftSetLayout;
extern VkDescriptorPool g_waterFftDescriptorPool;
extern VkDescriptorSet g_waterFftDescriptorSet;
extern VkPipelineLayout g_waterFftPipelineLayout;
extern VkPipeline g_waterFftPipeline;
extern bool g_waterFftLayoutReady;
extern int32_t g_waterFftWorkingLayer;
extern std::chrono::steady_clock::time_point g_waterFftLastUpdate;
extern bool g_waterFftLastUpdateValid;
extern float g_waterFftTimeSeconds;
extern std::atomic<bool> g_waterFftAvailable;
extern bool g_waterFftUpdatedThisFrame;
extern std::atomic<float> g_waterFftWindSpeed;
extern std::atomic<float> g_waterFftWindDirection;
extern std::atomic<float> g_waterFftTilePeriod;
extern std::atomic<float> g_waterFftAmplitude;
extern std::atomic<float> g_waterFftChoppiness;
extern std::atomic<float> g_waterFftDetail;
extern std::atomic<float> g_waterFftRateHz;
extern std::atomic<float> g_waterFftSwellGain;
extern std::atomic<float> g_waterWeatherRain;
extern std::atomic<bool>  g_waterFftEnabled;
inline constexpr const float kWATER_CAUSTIC_MAP_IORS[3] = {1.3315f, 1.3335f, 1.3365f};
extern VkImage g_waterCausticMapImage;
extern VkDeviceMemory g_waterCausticMapMemory;
extern VkImageView g_waterCausticMapView;
extern VkImageView g_waterCausticMapMip0View;
extern VkImageView g_waterCausticMapReflectView;
extern VkImageView g_waterCausticMapReflectMip0View;
extern VkSampler g_waterCausticMapSampler;
extern VkRenderPass g_waterCausticMapRenderPass;
extern VkFramebuffer g_waterCausticMapFramebuffer;
extern VkFramebuffer g_waterCausticMapReflectFramebuffer;
extern VkDescriptorSetLayout g_waterCausticMapSetLayout;
extern VkDescriptorPool g_waterCausticMapDescriptorPool;
extern VkDescriptorSet g_waterCausticMapDescriptorSet;
extern VkPipelineLayout g_waterCausticMapPipelineLayout;
extern VkImageView g_waterCausticMapStorageView;
extern VkPipeline g_waterCausticMapPipeline;
extern int64_t g_leakProbeInsideBytes;
extern int64_t g_leakProbeOutsideBytes;
extern uint64_t g_leakProbeEnterBytes;
extern uint64_t g_leakProbeLastExitBytes;
extern uint32_t g_leakProbeFrameCount;
extern int g_leakProbeDepth;
extern std::chrono::steady_clock::time_point g_leakProbeWindowStart;
extern uint32_t g_deviceInitStep;
extern VkPipeline g_rtPipelinePendingDestroy;
extern VkBuffer g_rtSbtPendingDestroyBuffer;
extern VkDeviceMemory g_rtSbtPendingDestroyMemory;
extern bool g_rtPipeRecycleArmed;
extern std::chrono::steady_clock::time_point g_rtPipeLastRecycle;


extern uint64_t g_vliTableReinsertTotal;
extern std::unordered_map<uint64_t, DhLodSection> g_dhLodSections;
extern std::unordered_map<uint64_t, uint64_t> g_dhLodSpatialOwners;
extern std::vector<uint64_t> g_dhLodSpatialSnapshot;
extern VkImageView g_sunShadowFarPublishedView;
extern bool g_sunShadowFarPublishedReady;

// 2026-11-01：近场太阳阴影图 -> RT set 的 binding 79。
// 与 far 版同一套 handle 缓存 + 变化才写，稳态零开销。
extern VkImageView g_sunShadowNearPublishedView;
extern bool g_sunShadowNearPublishedReady;
extern uint32_t g_geometryPreLoopLockSkips;
extern double g_geometryMaxPreLoopLockWaitMs;
extern double g_geometryMaxCountersLogMs;

// -------- 带内联方法体的类型（需位于原型与 extern 之后） --------



// RAII so an early return inside the timed region still reports.  Copying is
// disabled because a copied timer would report twice.
struct PerfL10StallTimer
{
    const char* tag;
    std::chrono::steady_clock::time_point start;

    explicit PerfL10StallTimer(const char* name)
        : tag(name),
          start(std::chrono::steady_clock::now())
    {
    }

    PerfL10StallTimer(const PerfL10StallTimer&) = delete;
    PerfL10StallTimer& operator=(const PerfL10StallTimer&) = delete;

    ~PerfL10StallTimer()
    {
        const double milliseconds =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start
            ).count();

        perfL10ReportStall(tag, milliseconds);
    }
};
struct RtxSectionBuildBufferPool
{
    std::vector<RtxSectionBuildBufferSlot> slots;

    // 取一个空闲且 >= requiredBytes 的槽（best fit，避免小批占住大块），
    // 成功则标记在飞并返回其副本。
    bool tryAcquire(
        VkDeviceSize requiredBytes,
        RtxSectionBuildBufferSlot& out)
    {
        size_t best = slots.size();

        for(size_t index = 0u; index < slots.size(); ++index)
        {
            const RtxSectionBuildBufferSlot& slot = slots[index];

            if(slot.inFlight || slot.bytes < requiredBytes)
            {
                continue;
            }

            if(best == slots.size() || slot.bytes < slots[best].bytes)
            {
                best = index;
            }
        }

        if(best == slots.size())
        {
            return false;
        }

        slots[best].inFlight = true;
        out = slots[best];

        return true;
    }

    // 登记一块刚建好的缓冲（调用方已 createBuffer）。优先顶掉一个**空闲**槽
    // （顶掉的只是空闲缓冲，销毁它安全），否则追加。返回登记后的槽副本。
    RtxSectionBuildBufferSlot adopt(
        VkBuffer buffer,
        VkDeviceMemory memory,
        VkDeviceSize bytes)
    {
        RtxSectionBuildBufferSlot slot;
        slot.buffer = buffer;
        slot.memory = memory;
        slot.bytes = bytes;
        slot.inFlight = true;

        size_t victim = slots.size();

        for(size_t index = 0u; index < slots.size(); ++index)
        {
            if(slots[index].inFlight)
            {
                continue;
            }

            if(slots[index].bytes >= slot.bytes)
            {
                continue;
            }

            if(victim == slots.size() ||
               slots[index].bytes < slots[victim].bytes)
            {
                victim = index;
            }
        }

        if(victim != slots.size())
        {
            if(slots[victim].buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(g_device, slots[victim].buffer, nullptr);
            }
            if(slots[victim].memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(g_device, slots[victim].memory, nullptr);
            }

            slots[victim] = slot;

            return slot;
        }

        slots.push_back(slot);

        return slot;
    }

    // 归还某个在飞槽（按缓冲句柄查找）。查不到说明它不属于本池，直接销毁以免泄漏。
    void release(VkBuffer buffer)
    {
        if(buffer == VK_NULL_HANDLE)
        {
            return;
        }

        for(RtxSectionBuildBufferSlot& slot : slots)
        {
            if(slot.buffer == buffer)
            {
                slot.inFlight = false;
                return;
            }
        }

        vkDestroyBuffer(g_device, buffer, nullptr);
    }

    void releaseAll()
    {
        for(RtxSectionBuildBufferSlot& slot : slots)
        {
            if(slot.buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(g_device, slot.buffer, nullptr);
            }
            if(slot.memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(g_device, slot.memory, nullptr);
            }
        }

        slots.clear();
    }

    // =====================================================
    // 2026-10-13（乙-4）：越线时释放池里**空闲**槽的暂存块，只保留最大的一块。
    //
    // 现场：`[RT MEM]` 的 churnTop 里 `use=0x1`（纯 VK_BUFFER_USAGE_TRANSFER_SRC_BIT）
    // 长期挂着 `liveN=3 liveMB=820.5`，而 `vkLiveHostMB=828.7` 基本就等于它 ⇒ **本模组
    // 全部的主机可见内存都是这一类纯 TRANSFER_SRC 暂存**，其中量级对得上的只有本节顶点
    // 构建 staging 池（原注释自称"只增不减"）。它每块是一个重建批次的 totalStagingBytes，
    // 高视距下可到数百 MB；池会一直留着它们。
    //
    // 安全性（与 slab 池 trimSectionMemoryPool 的时序前提完全相同）：槽变成空闲**只**发生
    // 在 release()，而 release() 只从 releaseDeferredSectionBuildResources() 调用 ——
    // 那是退休环的 fence 已经信号之后（见 releaseRetirementSlot 的说明）⇒ 池里的空闲块
    // 不可能仍被在途提交引用，可以安全销毁。在飞槽一律不碰。
    //
    // 保留最大的一块，而不是全释放：下一批多半还能命中，否则立刻变成"每批新建"的 churn，
    // 反而把分配耗时打回改动前。
    // =====================================================
    VkDeviceSize trimIdleKeepingLargest()
    {
        size_t largestIdle = slots.size();

        for(size_t index = 0u; index < slots.size(); ++index)
        {
            if(slots[index].inFlight)
            {
                continue;
            }

            if(largestIdle == slots.size() ||
               slots[index].bytes > slots[largestIdle].bytes)
            {
                largestIdle = index;
            }
        }

        VkDeviceSize freed = 0u;

        for(size_t index = 0u; index < slots.size();)
        {
            if(index == largestIdle || slots[index].inFlight)
            {
                ++index;
                continue;
            }

            RtxSectionBuildBufferSlot& slot = slots[index];

            if(slot.buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(g_device, slot.buffer, nullptr);
            }
            if(slot.memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(g_device, slot.memory, nullptr);
            }

            freed += slot.bytes;

            slots.erase(
                slots.begin() + static_cast<std::ptrdiff_t>(index)
            );
        }

        return freed;
    }

    // 2026-10-13（乙-4）：只读明细，进 [RT MEM]。
    void stats(
        size_t& outCount,
        VkDeviceSize& outTotalBytes,
        size_t& outInFlight) const
    {
        outCount = slots.size();
        outTotalBytes = 0u;
        outInFlight = 0u;

        for(const RtxSectionBuildBufferSlot& slot : slots)
        {
            outTotalBytes += slot.bytes;

            if(slot.inFlight)
            {
                outInFlight += 1u;
            }
        }
    }
};
class ChunkLockTimingGuard
{
public:
    ChunkLockTimingGuard()
        : waitStart_(std::chrono::steady_clock::now()),
          lock_(g_chunkMutex),
          holdStart_(std::chrono::steady_clock::now())
    {
    }

    ~ChunkLockTimingGuard()
    {
        const auto now = std::chrono::steady_clock::now();

        chunkLockNoteMax(
            g_chunkLockMaxHoldNanos,
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - holdStart_
                ).count()));

        chunkLockNoteMax(
            g_chunkLockMaxWaitNanos,
            static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    holdStart_ - waitStart_
                ).count()));
    }

    ChunkLockTimingGuard(const ChunkLockTimingGuard&) = delete;
    ChunkLockTimingGuard& operator=(const ChunkLockTimingGuard&) = delete;

private:
    std::chrono::steady_clock::time_point waitStart_;
    std::lock_guard<std::timed_mutex> lock_;
    std::chrono::steady_clock::time_point holdStart_;
};


struct LocalLightGridCellCoord
{
    int32_t x;
    int32_t y;
    int32_t z;


    bool operator==(
        const LocalLightGridCellCoord& other
    ) const noexcept
    {
        return
            x == other.x &&
            y == other.y &&
            z == other.z;
    }
};


struct LocalLightGridCellCoordHash
{
    size_t operator()(
        const LocalLightGridCellCoord& coord
    ) const noexcept
    {
        return static_cast<size_t>(
            localLightGridHash32(
                coord.x,
                coord.y,
                coord.z
            )
        );
    }
};
struct VliLockHoldProbe
{
    const char* site;
    std::chrono::steady_clock::time_point begin;
    bool marked = false;

    explicit VliLockHoldProbe(const char* name)
        : site(name)
    {
    }

    // 取到锁之后调用：只有从这一刻起才算"持锁时长"。
    // （第一版在 ctor 就记时，测出来的是"等锁 + 持有"，会把别的线程堵住的时间
    //   算到本站点头上 —— 2026-10-24 实测被这一点误导过一次。）
    void mark()
    {
        begin = std::chrono::steady_clock::now();
        marked = true;
    }

    ~VliLockHoldProbe()
    {
        if(!marked)
        {
            return;
        }

        const double heldMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - begin
            ).count();

        if(heldMs < vliLockHoldLogThresholdMs())
        {
            return;
        }

        RTLog(
            std::string("[VLI LOCK] site=") + site +
            " held=" + std::to_string(heldMs)
        );
    }
};

// RAII：往任意"每帧必经"的 JNI 函数体顶部放一行即可，不必去找它的每个 return。
struct LeakProbeScope
{
    LeakProbeScope()
    {
        leakProbeFrameBegin();
    }

    ~LeakProbeScope()
    {
        leakProbeFrameEnd();
    }
};


// 2026-10-22：VLI 工作线程持锁分项探针。只在 hold >= 8ms 时落盘。
//
// 为什么放在 lock_guard **之前**声明：析构顺序是反的，先解锁再析构，
// 所以 RTLog 不会发生在临界区里（否则等于自己把持锁时间又拉长）。
// apply 由剩余量推出（apply 里有 return，单独闭合不方便）。
struct VliWorkerHoldProbe
{
    const std::chrono::steady_clock::time_point begin =
        std::chrono::steady_clock::now();

    double lockWaitMs = 0.0;
    double cmpMs = 0.0;
    double lightsMs = 0.0;
    double indexMs = 0.0;

    static double sinceMs(const std::chrono::steady_clock::time_point& from)
    {
        return std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - from
        ).count();
    }

    ~VliWorkerHoldProbe()
    {
        const double holdMs = sinceMs(begin);

        if(holdMs < 8.0)
        {
            return;
        }

        const double applyMs =
            holdMs - lockWaitMs - cmpMs - lightsMs - indexMs;

        RTLog(
            "[VLI WORKER] hold=" + std::to_string(holdMs) +
            " lockWait=" + std::to_string(lockWaitMs) +
            " cmp=" + std::to_string(cmpMs) +
            " lights=" + std::to_string(lightsMs) +
            " index=" + std::to_string(indexMs) +
            " apply=" + std::to_string(applyMs));
    }
};
extern RtxSectionBuildBufferPool g_vertexStagingPool;
extern RtxSectionBuildBufferPool g_blasScratchPool;
