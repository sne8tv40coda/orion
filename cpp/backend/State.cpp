// Orion backend — State 模块：全部文件级全局变量的定义。
// 全局变量集中在同一个编译单元并保持原始声明顺序，
// 以精确复现拆分前单翻译单元的静态初始化/析构次序。

#include "OrionInternal.h"
#include "TrackingHooks.h"




extern "C" IMAGE_DOS_HEADER __ImageBase;


std::ofstream rtLogFile(
    ResolveRTXRuntimePath("rt_debug.log"),
    std::ios::app
);


std::mutex g_rtLogMutex;

namespace
{
struct RTXTerminateInstaller
{
    RTXTerminateInstaller()
    {
        std::set_terminate(&RTXTerminateHandler);
    }
};
const RTXTerminateInstaller g_rtxTerminateInstaller{};
}
std::atomic<uint64_t> g_rtxPhaseSeq{0};
std::atomic<uint64_t> g_rtxPhaseStamp[RTX_PHASE_RING]{};
std::atomic<const char*> g_rtxPhaseName[RTX_PHASE_RING]{};
std::atomic<const char*> g_rtxPhaseCurrent{"startup"};
std::atomic<uint64_t> g_rtxLastFrameSerial{0};
std::atomic<uint64_t> g_rtxDetailSectionKey{0};
std::atomic<uint64_t> g_rtxDetailSectionVerts{0};
std::atomic<uint64_t> g_rtxDetailInstances{0};
LPTOP_LEVEL_EXCEPTION_FILTER g_rtxPreviousExceptionFilter = nullptr;

namespace
{
struct RTXCrashFilterInstaller
{
    RTXCrashFilterInstaller()
    {
        g_rtxPreviousExceptionFilter =
            SetUnhandledExceptionFilter(&RTXUnhandledExceptionFilter);

        RTXMarkPhase("startup");
    }
};
const RTXCrashFilterInstaller g_rtxCrashFilterInstaller{};
}
bool g_isInWorld = false;
uint32_t g_currentAlbedoIndex = 0;
uint32_t g_currentNormalIndex = 0;
uint32_t g_currentSpecularIndex = 0;
VkImage g_normalAtlasImage = VK_NULL_HANDLE;
VkDeviceMemory g_normalAtlasMemory = VK_NULL_HANDLE;
VkImageView g_normalAtlasView = VK_NULL_HANDLE;
VkImage g_specularAtlasImage = VK_NULL_HANDLE;
VkDeviceMemory g_specularAtlasMemory = VK_NULL_HANDLE;
VkImageView g_specularAtlasView = VK_NULL_HANDLE;
bool g_pbrAtlasesDirty = false;
std::vector<VTRTexture> g_albedoTextures;
std::vector<VTRTexture> g_normalTextures;
std::vector<VTRTexture> g_specularTextures;
VkInstance g_instance=VK_NULL_HANDLE;
VkPhysicalDevice g_physicalDevice=VK_NULL_HANDLE;
VkDevice g_device=VK_NULL_HANDLE;
VkQueue g_queue=VK_NULL_HANDLE;
uint32_t g_queueFamily=UINT32_MAX;
VkPipelineCache g_pipelineCache = VK_NULL_HANDLE;
size_t g_pipelineCachePersistedBytes = 0;
uint32_t g_pipelineCachePersistCountdown = 60;
std::vector<uint32_t> g_perfL10AblationAltStages;
uint32_t g_perfL10AblationStage = 0u;
VkQueryPool g_perfL10GpuTimestampPool = VK_NULL_HANDLE;
double g_perfL10TimestampPeriodNs = 0.0;
uint64_t g_perfL10FrameSerial = 0u;
size_t g_sectionRebuildBatchCap = SECTION_REBUILD_MAX_SECTIONS_PER_BATCH;
bool g_sectionRebuildInterrupted = false;
uint64_t g_sectionRebuildContinueFrame = 0u;
uint64_t g_nextSectionRebuildFrame = 0u;
uint64_t g_firstPendingSectionFrame = 0u;
uint64_t g_lastObservedSectionRevision = 0u;
uint64_t g_geometryRebuildCallTotal = 0u;
uint64_t g_geometryFenceDeferredCount = 0u;
uint64_t g_geometryCountersLastCaptured = 0u;
uint64_t g_geometryCountersLastCalls = 0u;
double g_geometryMaxSectionMs = 0.0;
double g_geometryMaxPreLoopMs = 0.0;
double g_geometryMaxBuildLoopMs = 0.0;
double g_geometryMaxTailMs = 0.0;
PerfL10GpuTimestampSlot
    g_perfL10GpuTimestampSlots[PERF_L10_GPU_TIMESTAMP_RING_SIZE]{};
int32_t g_perfL10GpuActiveSlot = -1;
PerfL10GpuPassWindow g_perfL10GpuPassWindow{};
std::atomic<double> g_sectionRebuildBudgetMirrorMs{0.0};
uint32_t g_perfL10SerToggle = 0u;
uint32_t g_perfL10SerVisToggle = 0u;
uint32_t g_perfL10SerRadToggle = 0u;
uint32_t g_perfL10SerPrimToggle = 0u;
uint32_t g_perfL10SerPathsPhase = 0u;
uint32_t g_perfL10SerMode = 0u;
uint32_t g_perfL10RtaoSkipToggle = 0u;
std::chrono::steady_clock::time_point g_perfL10CpuFramePrev{};
bool g_perfL10CpuFrameHavePrev = false;
std::chrono::steady_clock::time_point g_perfL10CpuWorkStart{};
PerfL10CpuFrameWindow g_perfL10CpuFrameWindow{};


uint32_t g_perfL10GpuWindowsLogged = 0u;
std::atomic<float> g_perfL10SppForLog{1.0f};
std::atomic<float> g_perfL10GiMaxForLog{0.0f};
std::atomic<float> g_perfL10PomForLog{0.05f};


uint32_t g_perfL10BoundaryMask = 0u;
uint64_t g_perfL10NextTexturePublishFrame = 0u;
PerfL10CpuPhaseWindow g_perfL10CpuPhaseWindow{};
uint32_t g_perfL10CpuPhaseCursor = PERF_L10_CPU_PHASE_COUNT;
std::chrono::steady_clock::time_point g_perfL10CpuPhaseStart{};


VkCommandPool g_rtCommandPool=VK_NULL_HANDLE;
uint32_t g_sbtBaseAlignment = 0;
VkBuffer g_sbtBuffer=VK_NULL_HANDLE;
VkBuffer g_cameraUboBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_cameraUboMemory = VK_NULL_HANDLE;
void* g_cameraUboMapped = nullptr;
CameraUBO g_cameraUboHost;
sl::float4x4 g_prevCameraViewToWorld{};
sl::float4x4 g_prevCameraViewToClip{};
bool g_hasPrevCameraMatrices = false;
VkDeviceMemory g_sbtMemory=VK_NULL_HANDLE;
VkDeviceSize g_sbtSize=0;
VkDeviceAddress g_sbtDeviceAddress=0;
uint32_t g_raygenOffset=0,g_hitOffset=0,g_missOffset=0,g_handleSizeAligned=0;
VkStridedDeviceAddressRegionKHR g_raygenRegion{};
VkStridedDeviceAddressRegionKHR g_missRegion{};
VkStridedDeviceAddressRegionKHR g_hitRegion{};
VkStridedDeviceAddressRegionKHR g_callableRegion{};
VkImage g_waterSimImage = VK_NULL_HANDLE;
VkDeviceMemory g_waterSimMemory = VK_NULL_HANDLE;
VkImageView g_waterSimView = VK_NULL_HANDLE;
VkSampler g_waterSimSampler = VK_NULL_HANDLE;
VkDescriptorSetLayout g_waterSimDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_waterSimDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_waterSimDescriptorSet = VK_NULL_HANDLE;
VkPipelineLayout g_waterSimPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_waterSimPipeline = VK_NULL_HANDLE;
VkImageLayout g_waterSimLayout = VK_IMAGE_LAYOUT_UNDEFINED;
uint32_t g_waterSimReadLayer = 0u;
bool g_waterSimNeedsClear = true;
bool g_waterSimPlayerStateValid = false;
bool g_waterSimPlayerWasInWater = false;
bool g_waterSimPendingImpulse = false;
float g_waterSimPendingImpulseX = 0.0f;
float g_waterSimPendingImpulseZ = 0.0f;
float g_waterSimPendingImpulseStrength = 0.0f;
float g_waterSimLastSurfaceY = 0.0f;
bool g_waterSimOriginValid = false;
float g_waterSimOriginX = 0.0f;
float g_waterSimOriginZ = 0.0f;
std::chrono::steady_clock::time_point g_waterSimLastUpdate{};
bool g_waterSimLastUpdateValid = false;
float g_waterSimKeepAliveSeconds = 0.0f;
bool g_waterSimLoggedAwaitingWater = false;
bool g_waterSimLoggedFirstDispatch = false;
bool g_waterSimLoggedSampleContract = false;
VkBuffer g_waterSimEntityBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_waterSimEntityMemory = VK_NULL_HANDLE;
void* g_waterSimEntityMapped = nullptr;
std::mutex g_waterSimEntityMutex;
std::deque<WaterSimImpulseEvent> g_waterSimImpulseQueue;
float g_waterSimBoatData[WATER_SIM_MAX_ENTITIES * 5] = {};
int32_t g_waterSimBoatCount = 0;
const std::chrono::steady_clock::time_point g_waterWaveEpoch =
    std::chrono::steady_clock::now();
std::atomic<bool> g_waterCausticMapAvailable{false};
VkImage g_cloudShapeNoiseImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudShapeNoiseMemory = VK_NULL_HANDLE;
VkImageView g_cloudShapeNoiseView = VK_NULL_HANDLE;
VkSampler g_cloudShapeNoiseSampler = VK_NULL_HANDLE;
VkImage g_cloudCurlNoiseImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudCurlNoiseMemory = VK_NULL_HANDLE;
VkImageView g_cloudCurlNoiseView = VK_NULL_HANDLE;
VkSampler g_cloudCurlNoiseSampler = VK_NULL_HANDLE;
VkImage g_cloudBaseNoiseImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudBaseNoiseMemory = VK_NULL_HANDLE;
VkImageView g_cloudBaseNoiseView = VK_NULL_HANDLE;
VkSampler g_cloudBaseNoiseSampler = VK_NULL_HANDLE;
VkImage g_cloudV3ShapeImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudV3ShapeMemory = VK_NULL_HANDLE;
VkImageView g_cloudV3ShapeView = VK_NULL_HANDLE;
VkSampler g_cloudV3ShapeSampler = VK_NULL_HANDLE;
VkImage g_cloudV3DetailImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudV3DetailMemory = VK_NULL_HANDLE;
VkImageView g_cloudV3DetailView = VK_NULL_HANDLE;
VkSampler g_cloudV3DetailSampler = VK_NULL_HANDLE;
VkImage g_cloudV3CurlImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudV3CurlMemory = VK_NULL_HANDLE;
VkImageView g_cloudV3CurlView = VK_NULL_HANDLE;
VkSampler g_cloudV3CurlSampler = VK_NULL_HANDLE;
VkImage g_cloudV3WeatherImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudV3WeatherMemory = VK_NULL_HANDLE;
VkImageView g_cloudV3WeatherView = VK_NULL_HANDLE;
VkSampler g_cloudV3WeatherSampler = VK_NULL_HANDLE;
VkImage g_cloudV4AtlasImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudV4AtlasMemory = VK_NULL_HANDLE;
VkImageView g_cloudV4AtlasView = VK_NULL_HANDLE;
VkSampler g_cloudV4AtlasSampler = VK_NULL_HANDLE;
VkImage g_cloudV4WorleyImage = VK_NULL_HANDLE;
VkImage g_cloudV4BaseImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudV4BaseMemory = VK_NULL_HANDLE;
VkImageView g_cloudV4BaseView = VK_NULL_HANDLE;
VkDeviceMemory g_cloudV4WorleyMemory = VK_NULL_HANDLE;
VkImageView g_cloudV4WorleyView = VK_NULL_HANDLE;
VkSampler g_cloudV4WorleySampler = VK_NULL_HANDLE;
VkSampler g_cloudV4BaseSampler = VK_NULL_HANDLE;
VkImage g_cloudV4ShapeImage = VK_NULL_HANDLE;
VkDeviceMemory g_cloudV4ShapeMemory = VK_NULL_HANDLE;
VkImageView g_cloudV4ShapeView = VK_NULL_HANDLE;
VkSampler g_cloudV4ShapeSampler = VK_NULL_HANDLE;
VkImage g_volumetricCloudImage = VK_NULL_HANDLE;
VkDeviceMemory g_volumetricCloudMemory = VK_NULL_HANDLE;
VkImageView g_volumetricCloudView = VK_NULL_HANDLE;
VkImage g_volumetricCloudHistoryImage = VK_NULL_HANDLE;
VkDeviceMemory g_volumetricCloudHistoryMemory = VK_NULL_HANDLE;
VkImageView g_volumetricCloudHistoryView = VK_NULL_HANDLE;
VkImage g_volumetricCloudResolvedImage = VK_NULL_HANDLE;
VkDeviceMemory g_volumetricCloudResolvedMemory = VK_NULL_HANDLE;
VkImageView g_volumetricCloudResolvedView = VK_NULL_HANDLE;
VkImage g_volumetricCloudDepthImage = VK_NULL_HANDLE;
VkDeviceMemory g_volumetricCloudDepthMemory = VK_NULL_HANDLE;
VkImageView g_volumetricCloudDepthView = VK_NULL_HANDLE;
VkImage g_volumetricCloudHistoryDepthImage = VK_NULL_HANDLE;
VkDeviceMemory g_volumetricCloudHistoryDepthMemory = VK_NULL_HANDLE;
VkImageView g_volumetricCloudHistoryDepthView = VK_NULL_HANDLE;
VkSampler g_volumetricCloudSampler = VK_NULL_HANDLE;
VkImageLayout g_volumetricCloudLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_volumetricCloudHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_volumetricCloudResolvedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_volumetricCloudDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_volumetricCloudHistoryDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
std::atomic<bool> g_volumetricCloudHistoryNeedsClear{true};
uint32_t g_volumetricCloudWidth = 0u;
uint32_t g_volumetricCloudHeight = 0u;
uint32_t g_volumetricCloudBoundRenderWidth = 0u;
uint32_t g_volumetricCloudBoundRenderHeight = 0u;
uint32_t g_volumetricCloudBoundOutputWidth = 0u;
uint32_t g_volumetricCloudBoundOutputHeight = 0u;
VkDescriptorSetLayout g_volumetricCloudRenderSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_volumetricCloudResolveSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_volumetricCloudCompositeSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_volumetricCloudDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_volumetricCloudRenderSet = VK_NULL_HANDLE;
VkDescriptorSet g_volumetricCloudResolveSet = VK_NULL_HANDLE;
VkDescriptorSet g_volumetricCloudCompositeSet = VK_NULL_HANDLE;
VkImageView g_volumetricCloudCompositeTargetView = VK_NULL_HANDLE;
VkPipelineLayout g_volumetricCloudRenderPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout g_volumetricCloudResolvePipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout g_volumetricCloudCompositePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_volumetricCloudRenderPipelineV3 = VK_NULL_HANDLE;
VkPipeline g_volumetricCloudResolvePipeline = VK_NULL_HANDLE;
VkPipeline g_volumetricCloudCompositePipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout g_volumetricCloudAtrousSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_volumetricCloudAtrousDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_volumetricCloudAtrousSetForward = VK_NULL_HANDLE;
VkDescriptorSet g_volumetricCloudAtrousSetBackward = VK_NULL_HANDLE;
VkPipelineLayout g_volumetricCloudAtrousPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_volumetricCloudAtrousPipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout g_volumetricLightSetLayout = VK_NULL_HANDLE;
VkDescriptorSet g_volumetricLightSet = VK_NULL_HANDLE;
VkPipelineLayout g_volumetricLightPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_volumetricLightPipeline = VK_NULL_HANDLE;
std::atomic<uint32_t> g_volRestirEnabled{0u};
std::atomic<uint32_t> g_volRestirGuiOverride{0xFFFFFFFFu};
std::atomic<uint32_t> g_cameraInWater{0u};
VkImage g_volRadianceImage[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkDeviceMemory g_volRadianceMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImageView g_volRadianceView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImage g_volShYImage[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkDeviceMemory g_volShYMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImageView g_volShYView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImage g_volShCoCgImage[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkDeviceMemory g_volShCoCgMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImageView g_volShCoCgView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
bool g_volShNeedsInit = true;
VkImage g_volAgeImage[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkDeviceMemory g_volAgeMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImageView g_volAgeView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImage g_volReservoirImage[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkDeviceMemory g_volReservoirMemory[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkImageView g_volReservoirView[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
VkSampler g_volDepthSampler = VK_NULL_HANDLE;
VkSampler g_volRadianceSampler = VK_NULL_HANDLE;
uint32_t g_volFroxelW = 0u;
uint32_t g_volFroxelH = 0u;
uint32_t g_volRestirW = 0u;
uint32_t g_volRestirH = 0u;
uint32_t g_volWriteIndex = 0u;
VkDescriptorSetLayout g_volRestirSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_volRestirDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_volRestirSet = VK_NULL_HANDLE;
VkPipelineLayout g_volRestirPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_volRestirPipeline = VK_NULL_HANDLE;
bool g_volRestirPipelineFailed = false;
bool g_volRestirLayoutReady = false;
VkBuffer g_volRestirBoundLightBuffer = VK_NULL_HANDLE;
VkBuffer g_volRestirBoundGridBuffer = VK_NULL_HANDLE;
VkAccelerationStructureKHR g_volRestirBoundVisibilityTlas = VK_NULL_HANDLE;
VkBuffer g_volRestirBoundVisibilityLightBuffer = VK_NULL_HANDLE;
VkBuffer g_volRestirBoundVisibilityGridBuffer = VK_NULL_HANDLE;
VkBuffer g_volRestirBoundIntegrateLightBuffer = VK_NULL_HANDLE;
VkBuffer g_volRestirBoundIntegrateGridBuffer = VK_NULL_HANDLE;
VkAccelerationStructureKHR g_volRestirBoundIntegrateTlas = VK_NULL_HANDLE;
VkDescriptorSetLayout g_volRestirTemporalSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_volRestirTemporalDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_volRestirTemporalSet = VK_NULL_HANDLE;
VkPipelineLayout g_volRestirTemporalPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_volRestirTemporalPipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout g_volRestirSpatialSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_volRestirSpatialDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_volRestirSpatialSet = VK_NULL_HANDLE;
VkPipelineLayout g_volRestirSpatialPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_volRestirSpatialPipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout g_volRestirIntegrateSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_volRestirIntegrateDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_volRestirIntegrateSet = VK_NULL_HANDLE;
VkPipelineLayout g_volRestirIntegratePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_volRestirIntegratePipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout g_volRestirCompositeSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_volRestirCompositeDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_volRestirCompositeSet = VK_NULL_HANDLE;
VkPipelineLayout g_volRestirCompositePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_volRestirCompositePipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout g_volRestirVisibilitySetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_volRestirVisibilityDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_volRestirVisibilitySet = VK_NULL_HANDLE;
VkPipelineLayout g_volRestirVisibilityPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_volRestirVisibilityPipeline = VK_NULL_HANDLE;


VkImage g_godRayTargetImage = VK_NULL_HANDLE;
VkDeviceMemory g_godRayTargetMemory = VK_NULL_HANDLE;
VkImageView g_godRayTargetView = VK_NULL_HANDLE;
VkImage g_godRayHistoryImage = VK_NULL_HANDLE;
VkDeviceMemory g_godRayHistoryMemory = VK_NULL_HANDLE;
VkImageView g_godRayHistoryView = VK_NULL_HANDLE;
VkImageLayout g_godRayTargetLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_godRayHistoryLayout = VK_IMAGE_LAYOUT_UNDEFINED;
std::atomic<bool> g_godRayHistoryNeedsClear{true};
uint32_t g_godRayWidth = 0u;
uint32_t g_godRayHeight = 0u;
VkDescriptorSetLayout g_godRayCompositeSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_godRayDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_godRayCompositeSet = VK_NULL_HANDLE;
VkPipelineLayout g_godRayCompositePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_godRayCompositePipeline = VK_NULL_HANDLE;
VkImage g_sunShadowImage = VK_NULL_HANDLE;
VkDeviceMemory g_sunShadowMemory = VK_NULL_HANDLE;
VkImageView g_sunShadowView = VK_NULL_HANDLE;
VkSampler g_sunShadowSampler = VK_NULL_HANDLE;
VkImageLayout g_sunShadowLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkDescriptorSetLayout g_sunShadowSetLayout = VK_NULL_HANDLE;
VkDescriptorSet g_sunShadowSet = VK_NULL_HANDLE;
VkDescriptorPool g_sunShadowDescriptorPool = VK_NULL_HANDLE;
VkPipelineLayout g_sunShadowPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_sunShadowPipeline = VK_NULL_HANDLE;
std::atomic<bool> g_sunShadowAvailable{false};
VkAccelerationStructureKHR g_sunShadowTlasHandle = VK_NULL_HANDLE;
VkImage g_sunShadowFarImage = VK_NULL_HANDLE;
VkDeviceMemory g_sunShadowFarMemory = VK_NULL_HANDLE;
VkImageView g_sunShadowFarView = VK_NULL_HANDLE;
VkSampler g_sunShadowFarSampler = VK_NULL_HANDLE;
VkImageLayout g_sunShadowFarLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkDescriptorSetLayout g_sunShadowFarSetLayout = VK_NULL_HANDLE;
VkDescriptorSet g_sunShadowFarSet = VK_NULL_HANDLE;
VkDescriptorPool g_sunShadowFarDescriptorPool = VK_NULL_HANDLE;
VkPipelineLayout g_sunShadowFarPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_sunShadowFarPipeline = VK_NULL_HANDLE;
std::atomic<bool> g_sunShadowFarAvailable{false};
VkAccelerationStructureKHR g_sunShadowFarTlasHandle = VK_NULL_HANDLE;
VkImage g_terrainHeightImage = VK_NULL_HANDLE;
VkDeviceMemory g_terrainHeightMemory = VK_NULL_HANDLE;
VkImageView g_terrainHeightView = VK_NULL_HANDLE;
VkSampler g_terrainHeightSampler = VK_NULL_HANDLE;
VkImageLayout g_terrainHeightLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkDescriptorSetLayout g_terrainHeightSetLayout = VK_NULL_HANDLE;
VkDescriptorSet g_terrainHeightSet = VK_NULL_HANDLE;
VkDescriptorPool g_terrainHeightDescriptorPool = VK_NULL_HANDLE;
VkPipelineLayout g_terrainHeightPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_terrainHeightPipeline = VK_NULL_HANDLE;
std::atomic<bool> g_terrainHeightAvailable{false};
VkAccelerationStructureKHR g_terrainHeightTlasHandle = VK_NULL_HANDLE;
bool g_terrainHeightBuilt = false;
uint64_t g_terrainHeightBuiltRevision = 0u;
float g_terrainHeightBuiltCenter[2] = { 0.0f, 0.0f };
VkDescriptorSetLayout g_rrGuideRelaxSetLayout = VK_NULL_HANDLE;
VkDescriptorSet g_rrGuideRelaxSet = VK_NULL_HANDLE;
VkDescriptorPool g_rrGuideRelaxDescriptorPool = VK_NULL_HANDLE;
VkPipelineLayout g_rrGuideRelaxPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_rrGuideRelaxPipeline = VK_NULL_HANDLE;
std::atomic<bool> g_rrGuideRelaxEnabled{true};
VkImageView g_rrGuideRelaxCachedNormal = VK_NULL_HANDLE;
VkImageView g_rrGuideRelaxCachedDepth = VK_NULL_HANDLE;
VkImageView g_rrGuideRelaxCachedDiffuse = VK_NULL_HANDLE;
VkImageView g_rrGuideRelaxCachedSpecularAlbedo = VK_NULL_HANDLE;
VkImageView g_rrGuideRelaxCachedSpecularHitDistance = VK_NULL_HANDLE;
VkImageView g_rrGuideRelaxCachedCloudResolved = VK_NULL_HANDLE;
VkImageView g_rrGuideRelaxCachedCloudDepth = VK_NULL_HANDLE;
VkBuffer g_rrGuideRelaxCachedCamera = VK_NULL_HANDLE;
uint32_t g_rrGuideViewGeneration = 0u;
uint32_t g_rrGuideRelaxCachedGeneration = 0u;
std::atomic<float> g_rrFarCloudDistance{4000.0f};
std::atomic<bool> g_godRayEnabled{true};
std::atomic<int> g_godRaySteps{20};
std::atomic<int> g_godRayOcclusionSamples{3};
std::atomic<float> g_godRayIntensity{1.0f};
std::atomic<float> g_godRayHistoryWeight{0.92f};
VkImage g_skyScatteringImage = VK_NULL_HANDLE;
VkDeviceMemory g_skyScatteringMemory = VK_NULL_HANDLE;
VkImageView g_skyScatteringView = VK_NULL_HANDLE;
VkImage g_skyTransmittanceImage = VK_NULL_HANDLE;
VkDeviceMemory g_skyTransmittanceMemory = VK_NULL_HANDLE;
VkImageView g_skyTransmittanceView = VK_NULL_HANDLE;
VkSampler g_skyAtmosphereSampler = VK_NULL_HANDLE;
VkDescriptorSetLayout g_skyAtmosphereSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_skyAtmosphereDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_skyAtmosphereDescriptorSet = VK_NULL_HANDLE;
VkPipelineLayout g_skyAtmospherePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_skyAtmospherePipeline = VK_NULL_HANDLE;
VkImageLayout g_skyScatteringLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_skyTransmittanceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_waterDetailNormalImage = VK_NULL_HANDLE;
VkDeviceMemory g_waterDetailNormalMemory = VK_NULL_HANDLE;
VkImageView g_waterDetailNormalView = VK_NULL_HANDLE;
VkSampler g_waterDetailNormalSampler = VK_NULL_HANDLE;
VkImage g_waterCausticImage = VK_NULL_HANDLE;
VkDeviceMemory g_waterCausticMemory = VK_NULL_HANDLE;
VkImageView g_waterCausticView = VK_NULL_HANDLE;
VkSampler g_waterCausticSampler = VK_NULL_HANDLE;
VkDescriptorSetLayout g_waterCausticDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_waterCausticDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_waterCausticDescriptorSet = VK_NULL_HANDLE;
VkPipelineLayout g_waterCausticPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_waterCausticPipeline = VK_NULL_HANDLE;
VkImageLayout g_waterCausticLayout = VK_IMAGE_LAYOUT_UNDEFINED;
std::chrono::steady_clock::time_point g_waterCausticLastUpdate{};
bool g_waterCausticLastUpdateValid = false;
VkBuffer g_testVertexBuffer=VK_NULL_HANDLE;
VkDeviceMemory g_testVertexMemory=VK_NULL_HANDLE;
VkDeviceAddress g_testVertexDeviceAddress=0;
VkBuffer g_blasBuffer=VK_NULL_HANDLE;
VkDeviceMemory g_blasMemory=VK_NULL_HANDLE;
VkAccelerationStructureKHR g_blas=VK_NULL_HANDLE;
VkDeviceAddress g_blasDeviceAddress=0;
VkBuffer g_blasScratchBuffer=VK_NULL_HANDLE;
std::unordered_map<uint64_t, OmmCutoutInput> g_cutoutOmmInputs;
bool g_ommNativeEnabled = false;
uint32_t g_ommMax4StateSubdivisionLevel = 0;
bool g_serExtAdvertised = false;
bool g_serFeatureSupported = false;
bool g_serHardwareReordering = false;
bool g_serNvAdvertised = false;
bool g_serNvHintReorder = false;
std::atomic_bool g_serEnabled{true};


std::unordered_map<uint64_t, RTSectionAS>
    g_sectionBLAS;
std::unordered_set<uint64_t> g_nonResidentSections;
uint64_t g_vramEvictedSections = 0u;
uint64_t g_vramRehydratedSections = 0u;



VkBuffer g_tlasInstancesBuffer =
    VK_NULL_HANDLE;
VkDeviceMemory g_tlasInstancesMemory =
    VK_NULL_HANDLE;
VkDeviceAddress g_tlasInstancesAddress = 0;
VkBuffer g_sectionVertexAddressBuffer =
    VK_NULL_HANDLE;
VkDeviceMemory g_sectionVertexAddressMemory =
    VK_NULL_HANDLE;
size_t g_sectionVertexAddressCapacity = 0;
uint32_t g_sectionVertexAddressNextSlot = 1u;
std::vector<uint32_t> g_sectionVertexAddressFreeSlots;
void* g_sectionVertexAddressMapped = nullptr;
VkBuffer g_sectionRoleBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_sectionRoleMemory = VK_NULL_HANDLE;
void* g_sectionRoleMapped = nullptr;
VkBuffer g_lodCoverageBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_lodCoverageMemory = VK_NULL_HANDLE;
void* g_lodCoverageMapped = nullptr;
uint32_t g_lodCoverageWordCount = 0;
VkDeviceMemory g_blasScratchMemory=VK_NULL_HANDLE;
VkDeviceAddress g_blasScratchAddress=0;
VkBuffer g_tlasBuffer=VK_NULL_HANDLE;
VkDeviceMemory g_tlasMemory=VK_NULL_HANDLE;
VkAccelerationStructureKHR g_tlas=VK_NULL_HANDLE;
VkAccelerationStructureKHR g_tlasDescriptorHandle=VK_NULL_HANDLE;
VkDeviceAddress g_tlasDeviceAddress=0;
VkBuffer g_tlasInstanceBuffer=VK_NULL_HANDLE;
VkDeviceMemory g_tlasInstanceMemory=VK_NULL_HANDLE;
VkDeviceAddress g_tlasInstanceDeviceAddress=0;
VkBuffer g_tlasScratchBuffer=VK_NULL_HANDLE;
VkDeviceMemory g_tlasScratchMemory=VK_NULL_HANDLE;
VkDeviceAddress g_tlasScratchAddress=0;
VkDeviceSize g_tlasScratchCapacity=0;
VkDeviceSize g_tlasAsSize=0;
VkDeviceSize g_tlasInstancesCapacity=0;
size_t g_tlasInstanceCount=0;


DeferredSectionBuildResources
    g_deferredSectionBuild;
RtxSectionBuildBufferPool g_vertexStagingPool;
RtxSectionBuildBufferPool g_blasScratchPool;
uint64_t g_sectionBuildPoolAllocCount = 0u;
uint64_t g_sectionBuildPoolReuseCount = 0u;
std::array<
    SectionRetirementSlot,
    SECTION_RETIREMENT_RING_SIZE
> g_sectionRetirementRing;
std::array<
    VkFence,
    SECTION_RETIREMENT_RING_SIZE
> g_sectionRetirementFences{};
bool g_sectionRetirementFencesReady = false;
uint32_t g_currentRetirementSlot = 0u;
SectionRetirementSlot*
    g_sectionRetirement =
        &g_sectionRetirementRing[0];
VkDescriptorPool g_rayTracingDescriptorPool=VK_NULL_HANDLE;
VkDescriptorSet g_rayTracingDescriptorSet=VK_NULL_HANDLE;
VkDescriptorSetLayout g_rayTracingDescriptorSetLayout=VK_NULL_HANDLE;
VkPipelineLayout g_rayTracingPipelineLayout=VK_NULL_HANDLE;
VkPipeline g_rayTracingPipeline=VK_NULL_HANDLE;
bool g_nrdReady = false;
nrd::Instance* g_nrdInstance = nullptr;
uint32_t g_nrdWidth = 0u;
uint32_t g_nrdHeight = 0u;
uint32_t g_nrdIdentifier = 1u;
nrd::LibraryDesc g_nrdLibraryDesc{};
nrd::InstanceDesc g_nrdInstanceDesc{};
VkBuffer g_nrdConstantBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_nrdConstantBufferMemory = VK_NULL_HANDLE;
VkDeviceSize g_nrdConstantBufferStride = 0;
VkSampler g_nrdSamplerNearest = VK_NULL_HANDLE;
VkSampler g_nrdSamplerLinear = VK_NULL_HANDLE;
VkDescriptorSetLayout g_nrdDescriptorSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_nrdConstantSetLayout = VK_NULL_HANDLE;
VkPipelineLayout g_nrdPipelineLayout = VK_NULL_HANDLE;
VkDescriptorPool g_nrdDescriptorPool = VK_NULL_HANDLE;
std::vector<VkDescriptorSet> g_nrdDescriptorSets;
std::vector<VkDescriptorSet> g_nrdConstantSets;
std::vector<VkPipeline> g_nrdPipelines;
std::vector<VkImage> g_nrdPermanentImages;
std::vector<VkDeviceMemory> g_nrdPermanentMemories;
std::vector<VkImageView> g_nrdPermanentViews;
std::vector<VkImageLayout> g_nrdPermanentLayouts;
std::vector<nrd::TextureDesc> g_nrdPermanentDescs;
std::vector<VkImage> g_nrdTransientImages;
std::vector<VkDeviceMemory> g_nrdTransientMemories;
std::vector<VkImageView> g_nrdTransientViews;
std::vector<VkImageLayout> g_nrdTransientLayouts;
std::vector<nrd::TextureDesc> g_nrdTransientDescs;
VkImage g_nrdViewZImage = VK_NULL_HANDLE;
VkDeviceMemory g_nrdViewZMemory = VK_NULL_HANDLE;
VkImageView g_nrdViewZView = VK_NULL_HANDLE;
VkImageLayout g_nrdViewZLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_nrdNormalRoughnessImage = VK_NULL_HANDLE;
VkDeviceMemory g_nrdNormalRoughnessMemory = VK_NULL_HANDLE;
VkImageView g_nrdNormalRoughnessView = VK_NULL_HANDLE;
VkImageLayout g_nrdNormalRoughnessLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_nrdMotionImage = VK_NULL_HANDLE;
VkDeviceMemory g_nrdMotionMemory = VK_NULL_HANDLE;
VkImageView g_nrdMotionView = VK_NULL_HANDLE;
VkImageLayout g_nrdMotionLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_nrdDiffRadianceHitDistImage = VK_NULL_HANDLE;
VkDeviceMemory g_nrdDiffRadianceHitDistMemory = VK_NULL_HANDLE;
VkImageView g_nrdDiffRadianceHitDistView = VK_NULL_HANDLE;
VkImageLayout g_nrdDiffRadianceHitDistLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_nrdOutDiffRadianceHitDistImage = VK_NULL_HANDLE;
VkDeviceMemory g_nrdOutDiffRadianceHitDistMemory = VK_NULL_HANDLE;
VkImageView g_nrdOutDiffRadianceHitDistView = VK_NULL_HANDLE;
VkImageLayout g_nrdOutDiffRadianceHitDistLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_nrdValidationImage = VK_NULL_HANDLE;
VkDeviceMemory g_nrdValidationMemory = VK_NULL_HANDLE;
VkImageView g_nrdValidationView = VK_NULL_HANDLE;
VkImageLayout g_nrdValidationLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_nrdMvProbeImage = VK_NULL_HANDLE;
VkDeviceMemory g_nrdMvProbeMemory = VK_NULL_HANDLE;
VkImageView g_nrdMvProbeView = VK_NULL_HANDLE;
VkImageLayout g_nrdMvProbeLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkPipelineLayout g_nrdCompositePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_nrdCompositePipeline = VK_NULL_HANDLE;
uint32_t g_nrdFrameIndex = 0u;
bool g_nrdNeedsHistoryReset = true;
bool g_nrdSettingsLogged = false;
bool g_nrdDisocclusionLogged = false;
float g_nrdSmoothedFrameTimeMs = 1000.0f / 60.0f;


VkDescriptorPool
    g_sdrPostDescriptorPool =
        VK_NULL_HANDLE;
VkDescriptorSet
    g_sdrPostDescriptorSet =
        VK_NULL_HANDLE;
VkDescriptorSetLayout
    g_sdrPostDescriptorSetLayout =
        VK_NULL_HANDLE;
VkPipelineLayout
    g_sdrPostPipelineLayout =
        VK_NULL_HANDLE;
VkPipeline
    g_sdrPostPipeline =
        VK_NULL_HANDLE;
std::atomic<bool> g_hdrRequested{false};
std::atomic<bool> g_hdrSurfaceSupported{false};
VkDescriptorPool  g_hdrEncodeDescriptorPool  = VK_NULL_HANDLE;
VkDescriptorSet   g_hdrEncodeDescriptorSet   = VK_NULL_HANDLE;
VkDescriptorSetLayout g_hdrEncodeDescriptorSetLayout = VK_NULL_HANDLE;
VkPipelineLayout  g_hdrEncodePipelineLayout   = VK_NULL_HANDLE;
VkPipeline        g_hdrEncodePipeline          = VK_NULL_HANDLE;
VkImage        g_hdr10Image    = VK_NULL_HANDLE;
VkDeviceMemory g_hdr10Memory   = VK_NULL_HANDLE;
VkImageView    g_hdr10View     = VK_NULL_HANDLE;
VkImageLayout  g_hdr10Layout   = VK_IMAGE_LAYOUT_UNDEFINED;
uint32_t       g_hdr10Width    = 0;
uint32_t       g_hdr10Height   = 0;


VkPipeline
    g_hdrPostPipeline =
        VK_NULL_HANDLE;
bool
    g_hdrPresentationWasActive =
        false;


std::atomic_bool g_aeEnabled{true};
std::atomic<float> g_aeCompensationEV{0.0f};
float g_aeMinEV = -4.0f;
float g_aeMaxEV = 1.0f;
float g_aeAdaptUpRate = 1.5f;
float g_aeAdaptDownRate = 3.0f;
float g_aeTrimLow = 0.02f;
float g_aeTrimHigh = 0.98f;
VkBuffer g_aeHistogramBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_aeHistogramMemory = VK_NULL_HANDLE;
VkBuffer g_aeStateBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_aeStateMemory = VK_NULL_HANDLE;
float* g_aeStateCpu = nullptr;
VkDescriptorSetLayout g_aeHistogramSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_aeHistogramPool = VK_NULL_HANDLE;
VkDescriptorSet g_aeHistogramSet = VK_NULL_HANDLE;
VkPipelineLayout g_aeHistogramLayout = VK_NULL_HANDLE;
VkPipeline g_aeHistogramPipeline = VK_NULL_HANDLE;
VkDescriptorSetLayout g_aeAdaptSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_aeAdaptPool = VK_NULL_HANDLE;
VkDescriptorSet g_aeAdaptSet = VK_NULL_HANDLE;
VkPipelineLayout g_aeAdaptLayout = VK_NULL_HANDLE;
VkPipeline g_aeAdaptPipeline = VK_NULL_HANDLE;
std::chrono::steady_clock::time_point g_aeLastTick{};
bool g_aeHasLastTick = false;
bool g_aeReported = false;
VkImage g_rtOutputImage=VK_NULL_HANDLE;
VkImage g_dlssOutputImage = VK_NULL_HANDLE;
VkImage g_dlssDepthImage;
VkDeviceMemory g_dlssDepthMemory;
VkImageView g_dlssDepthView;
VkDeviceMemory g_dlssOutputMemory = VK_NULL_HANDLE;
VkImageView g_dlssOutputView = VK_NULL_HANDLE;
sl::ViewportHandle g_dlssViewport(0);
VkImage g_motionImage = VK_NULL_HANDLE;
VkDeviceMemory g_motionMemory = VK_NULL_HANDLE;
VkImageView g_motionView = VK_NULL_HANDLE;
uint32_t g_motionWidth = 0;
uint32_t g_motionHeight = 0;



VkImage g_rrStableRadianceImage = VK_NULL_HANDLE;
VkDeviceMemory g_rrStableRadianceMemory = VK_NULL_HANDLE;
VkImageView g_rrStableRadianceView = VK_NULL_HANDLE;
VkImageLayout g_rrStableRadianceLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;
VkSampler g_rrStableRadianceSampler = VK_NULL_HANDLE;
VkImage g_rrDiffuseAlbedoImage = VK_NULL_HANDLE;
VkDeviceMemory g_rrDiffuseAlbedoMemory = VK_NULL_HANDLE;
VkImageView g_rrDiffuseAlbedoView = VK_NULL_HANDLE;
VkImageLayout g_rrDiffuseAlbedoLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;


VkImage g_rrSpecularAlbedoImage = VK_NULL_HANDLE;
VkDeviceMemory g_rrSpecularAlbedoMemory = VK_NULL_HANDLE;
VkImageView g_rrSpecularAlbedoView = VK_NULL_HANDLE;
VkImageLayout g_rrSpecularAlbedoLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;


VkImage g_rrNormalRoughnessImage = VK_NULL_HANDLE;
VkDeviceMemory g_rrNormalRoughnessMemory = VK_NULL_HANDLE;
VkImageView g_rrNormalRoughnessView = VK_NULL_HANDLE;
VkImageLayout g_rrNormalRoughnessLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;


VkImage g_rrSpecularHitDistanceImage = VK_NULL_HANDLE;
VkDeviceMemory g_rrSpecularHitDistanceMemory = VK_NULL_HANDLE;
VkImageView g_rrSpecularHitDistanceView = VK_NULL_HANDLE;
VkImageLayout g_rrSpecularHitDistanceLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;


VkImage g_rrTransparencyHintImage = VK_NULL_HANDLE;
VkDeviceMemory g_rrTransparencyHintMemory = VK_NULL_HANDLE;
VkImageView g_rrTransparencyHintView = VK_NULL_HANDLE;
VkImageLayout g_rrTransparencyHintLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_giHalfImage = VK_NULL_HANDLE;
VkDeviceMemory g_giHalfMemory = VK_NULL_HANDLE;
VkImageView g_giHalfView = VK_NULL_HANDLE;
VkImageLayout g_giHalfLayout = VK_IMAGE_LAYOUT_UNDEFINED;
uint32_t g_giHalfWidth = 0u;
uint32_t g_giHalfHeight = 0u;
bool g_giHalfNeedsClear = false;
VkImageView g_giHalfBoundView = VK_NULL_HANDLE;


VkImage g_alphaBlendGbufferImage = VK_NULL_HANDLE;
VkDeviceMemory g_alphaBlendGbufferMemory = VK_NULL_HANDLE;
VkImageView g_alphaBlendGbufferView = VK_NULL_HANDLE;
VkImageLayout g_alphaBlendGbufferLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_alphaBlendMetaImage = VK_NULL_HANDLE;
VkDeviceMemory g_alphaBlendMetaMemory = VK_NULL_HANDLE;
VkImageView g_alphaBlendMetaView = VK_NULL_HANDLE;
VkImageLayout g_alphaBlendMetaLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_alphaBlendLightImage = VK_NULL_HANDLE;
VkDeviceMemory g_alphaBlendLightMemory = VK_NULL_HANDLE;
VkImageView g_alphaBlendLightView = VK_NULL_HANDLE;
VkImageLayout g_alphaBlendLightLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_alphaBlendVolumeImage = VK_NULL_HANDLE;
VkDeviceMemory g_alphaBlendVolumeMemory = VK_NULL_HANDLE;
VkImageView g_alphaBlendVolumeView = VK_NULL_HANDLE;
VkImageLayout g_alphaBlendVolumeLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_alphaBlendRadianceImage = VK_NULL_HANDLE;
VkDeviceMemory g_alphaBlendRadianceMemory = VK_NULL_HANDLE;
VkImageView g_alphaBlendRadianceView = VK_NULL_HANDLE;
VkImageLayout g_alphaBlendRadianceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_alphaBlendPlaceholder3DImage = VK_NULL_HANDLE;
VkDeviceMemory g_alphaBlendPlaceholder3DMemory = VK_NULL_HANDLE;
VkImageView g_alphaBlendPlaceholder3DView = VK_NULL_HANDLE;
VkPipeline g_glassAlphaDiagPipeline = VK_NULL_HANDLE;
VkImage g_alphaBlendPlaceholderR8Image = VK_NULL_HANDLE;
VkDeviceMemory g_alphaBlendPlaceholderR8Memory = VK_NULL_HANDLE;
VkImageView g_alphaBlendPlaceholderR8View = VK_NULL_HANDLE;
VkImage g_alphaBlendPlaceholderUintImage = VK_NULL_HANDLE;
VkDeviceMemory g_alphaBlendPlaceholderUintMemory = VK_NULL_HANDLE;
VkImageView g_alphaBlendPlaceholderUintView = VK_NULL_HANDLE;
uint32_t g_alphaBlendWidth = 0u;
uint32_t g_alphaBlendHeight = 0u;
VkDescriptorSetLayout g_glassAlphaBlendSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_glassAlphaBlendPool = VK_NULL_HANDLE;
VkDescriptorSet g_glassAlphaBlendSet = VK_NULL_HANDLE;
VkPipelineLayout g_glassAlphaBlendPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_glassAlphaBlendPipeline = VK_NULL_HANDLE;
VkPipeline g_glassAlphaFinishPipeline = VK_NULL_HANDLE;
bool g_glassAlphaBlendPipelineFailed = false;
uint32_t g_glassAlphaBlendFrameIndex = 0u;
VkBuffer g_glassBlueNoiseBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_glassBlueNoiseMemory = VK_NULL_HANDLE;
bool g_glassBlueNoiseReady = false;
VkDescriptorSet g_glassAlphaBlendBoundSet = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundRayGbuffer = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundColor = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundGbuffer = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundMeta = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundLight = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundRadiance = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundDepth = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundNormal = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundAlbedo = VK_NULL_HANDLE;
VkImageView g_glassAlphaBlendBoundShY = VK_NULL_HANDLE;
std::atomic<int> g_giNearRateOverride{-1};
std::atomic<int> g_glossyRoughLodOverride{-1};
std::atomic<int> g_directLightModeOverride{-1};
std::atomic<int> g_indirectModeOverride{-1};
std::atomic<int> g_sparseRatePercentOverride{-1};
std::atomic<int> g_rtxdiReuseInactiveOverride{-1};


uint32_t g_rrGBufferWidth = 0;
uint32_t g_rrGBufferHeight = 0;


uint32_t g_dlssOutputWidth = 0;
uint32_t g_dlssOutputHeight = 0;
uint32_t g_dlssDepthWidth = 0;
uint32_t g_dlssDepthHeight = 0;
VkDeviceMemory g_rtOutputMemory=VK_NULL_HANDLE;
VkImageView g_rtOutputView=VK_NULL_HANDLE;
uint32_t g_rtOutputWidth=0,g_rtOutputHeight=0;
VkImageLayout g_rtOutputLayout=VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_dlssOutputLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_dlssDepthLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;
VkImageLayout g_motionLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;


bool g_asBuilt=false;
HMODULE g_slModule=NULL;
bool g_slDevPluginsRequested = false;
std::string g_slActivePluginDir;
VkFormat
    g_fgBackbufferFormat =
        VK_FORMAT_UNDEFINED;
VkFormat
    g_fgSnapshotFormat =
        VK_FORMAT_UNDEFINED;


VkImage
    g_fgHudlessImage =
        VK_NULL_HANDLE;
VkDeviceMemory
    g_fgHudlessMemory =
        VK_NULL_HANDLE;
VkImageView
    g_fgHudlessView =
        VK_NULL_HANDLE;
VkImageLayout
    g_fgHudlessLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;


VkImage
    g_fgFinalImage =
        VK_NULL_HANDLE;
VkDeviceMemory
    g_fgFinalMemory =
        VK_NULL_HANDLE;
VkImageView
    g_fgFinalView =
        VK_NULL_HANDLE;
VkImageLayout
    g_fgFinalLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;


VkImage
    g_fgUIAlphaImage =
        VK_NULL_HANDLE;
VkDeviceMemory
    g_fgUIAlphaMemory =
        VK_NULL_HANDLE;
VkImageView
    g_fgUIAlphaView =
        VK_NULL_HANDLE;
VkImageLayout
    g_fgUIAlphaLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;


uint32_t
    g_fgUIWidth =
        0;
uint32_t
    g_fgUIHeight =
        0;


uint32_t
    g_fgHudlessFrameIndex =
        UINT32_MAX;
uint32_t
    g_fgUIAlphaFrameIndex =
        UINT32_MAX;


VkSampler
    g_fgUISnapshotSampler =
        VK_NULL_HANDLE;


VkDescriptorPool
    g_fgUIAlphaDescriptorPool =
        VK_NULL_HANDLE;
VkDescriptorSet
    g_fgUIAlphaDescriptorSet =
        VK_NULL_HANDLE;
VkDescriptorSetLayout
    g_fgUIAlphaDescriptorSetLayout =
        VK_NULL_HANDLE;
VkPipelineLayout
    g_fgUIAlphaPipelineLayout =
        VK_NULL_HANDLE;
VkPipeline
    g_fgUIAlphaPipeline =
        VK_NULL_HANDLE;
bool
    g_slDeviceConfigured = false;


PFN_vkGetInstanceProcAddr
    g_slVkGetInstanceProcAddrProxy = nullptr;
PFN_vkGetDeviceProcAddr
    g_slVkGetDeviceProcAddrProxy = nullptr;


PFN_vkCreateWin32SurfaceKHR
    g_slVkCreateWin32SurfaceKHRProxy = nullptr;
PFN_vkDestroySurfaceKHR
    g_slVkDestroySurfaceKHRProxy = nullptr;


PFN_vkCreateSwapchainKHR
    g_slVkCreateSwapchainKHRProxy = nullptr;
PFN_vkDestroySwapchainKHR
    g_slVkDestroySwapchainKHRProxy = nullptr;
PFN_vkGetSwapchainImagesKHR
    g_slVkGetSwapchainImagesKHRProxy = nullptr;
PFN_vkAcquireNextImageKHR
    g_slVkAcquireNextImageKHRProxy = nullptr;
PFN_vkQueuePresentKHR
    g_slVkQueuePresentKHRProxy = nullptr;
PFN_vkDeviceWaitIdle
    g_slVkDeviceWaitIdleProxy = nullptr;
bool g_dlssReady = false;
bool g_dlssRRSupported = false;
bool g_dlssRRReady = false;
std::atomic<bool> g_sparseRateHistoryReset{false};
std::atomic<bool> g_sparseHistoryValid{false};
bool g_dlssGSupported = false;
bool g_dlssGReady = false;
std::atomic<uint32_t> g_mvScaleMode{1u};
std::atomic<bool> g_fgRefreshCapEnabled{true};
bool g_fgTagsValid = false;
std::atomic<uint32_t> g_fgForceOffFrames{0};
bool g_fgSnapshotsReleased = false;
bool g_dlssGFeatureInitialized = false;
sl::Result g_dlssGLoadResult = sl::Result::eErrorFeatureNotSupported;
bool g_fgFrameSyncEnabled = true;
std::atomic<int> g_fgMultiplierRequested{2};
std::atomic<uint32_t> g_fgGeneratedMaxSeen{1};
int g_fgBaseFrameLimitRequest = -1;
std::atomic<uint32_t> g_fgBaseFrameLimitHz{0};
std::atomic<bool> g_fgFullscreenMenuOpen{false};
uint32_t g_fgGateLastMask = 0xffffffffu;
bool
    g_fgRequirementsReady = false;


std::vector<std::string>
    g_fgInstanceExtensions;
std::vector<std::string>
    g_fgDeviceExtensions;
std::vector<std::string>
    g_fgFeatures12;
std::vector<std::string>
    g_fgFeatures13;


uint32_t
    g_fgGraphicsQueuesRequired = 0;
uint32_t
    g_fgComputeQueuesRequired = 0;
uint32_t
    g_fgOpticalFlowQueuesRequired = 0;
bool
    g_slQueuePlanReady = false;


uint32_t
    g_slGraphicsQueueFamily = UINT32_MAX;
uint32_t
    g_slGraphicsQueueIndex = UINT32_MAX;


uint32_t
    g_slComputeQueueFamily = UINT32_MAX;
uint32_t
    g_slComputeQueueIndex = UINT32_MAX;


uint32_t
    g_slOpticalFlowQueueFamily = UINT32_MAX;
uint32_t
    g_slOpticalFlowQueueIndex = UINT32_MAX;


bool
    g_slUseNativeOpticalFlow = false;


PFN_slDLSSGSetOptions
    g_slDLSSGSetOptions = nullptr;
PFN_slDLSSGGetState
    g_slDLSSGGetState = nullptr;
std::atomic<int> g_rrMode{1};
std::atomic<int> g_upscalerMode{0};
std::atomic<int> g_srMode{2};
std::atomic<float>
    g_noDirectLightAmbient{0.10f};
std::atomic<float>
    g_pomDepth{0.10f};
std::atomic<float>
    g_pomSharp{0.0f};
std::atomic<float>
    g_sssMode{0.0f};

// =====================================================
// 【2026-11-02】屏幕空间降级链（SSR + SSPT）
// 默认 0 = 关（不建资源、不 dispatch）。定义集中在这里，遵循
// backend/README.md 的「全局变量只在 State.cpp 定义」契约。
// =====================================================
std::atomic<float> g_ssptChunks{0.0f};
std::atomic<float> g_ssrChunks{0.0f};

VkImage g_ssIndirectTermImage = VK_NULL_HANDLE;
VkDeviceMemory g_ssIndirectTermMemory = VK_NULL_HANDLE;
VkImageView g_ssIndirectTermView = VK_NULL_HANDLE;
VkImageLayout g_ssIndirectTermLayout = VK_IMAGE_LAYOUT_UNDEFINED;

VkImage g_ssSpecularTermImage = VK_NULL_HANDLE;
VkDeviceMemory g_ssSpecularTermMemory = VK_NULL_HANDLE;
VkImageView g_ssSpecularTermView = VK_NULL_HANDLE;
VkImageLayout g_ssSpecularTermLayout = VK_IMAGE_LAYOUT_UNDEFINED;

VkImage g_ssBaseColorImage = VK_NULL_HANDLE;
VkDeviceMemory g_ssBaseColorMemory = VK_NULL_HANDLE;
VkImageView g_ssBaseColorView = VK_NULL_HANDLE;
VkImageLayout g_ssBaseColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

VkImage g_ssPrevColorImage = VK_NULL_HANDLE;
VkDeviceMemory g_ssPrevColorMemory = VK_NULL_HANDLE;
VkImageView g_ssPrevColorView = VK_NULL_HANDLE;
VkImageLayout g_ssPrevColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;

uint32_t g_ssHybridWidth = 0u;
uint32_t g_ssHybridHeight = 0u;
bool g_ssHybridHistoryValid = false;
VkDescriptorSet g_ssHybridBoundSet = VK_NULL_HANDLE;
VkPipelineLayout g_ssHybridPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_ssHybridPipeline = VK_NULL_HANDLE;
bool g_ssHybridPipelineFailed = false;

VkBuffer g_ssHzbBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_ssHzbMemory = VK_NULL_HANDLE;
uint32_t g_ssHzbLevelCount = 0u;
VkPipeline g_ssHzbPipeline = VK_NULL_HANDLE;
std::atomic<float> g_ueCloudParams[UE_CLOUD_PARAM_COUNT] = {
    {1.0f}, {0.5f}, {0.5f}, {1.0f}, {0.15f}, {1.0f}, {0.35f}, {1.0f}
};

// 2026-10-31：语义从"最远距离（格）"改成"整体距离系数"（1.0 = 标准）。
// 每个光源方块的基础传播距离在 local_light_profile.glsl 的档案表里，
// 这里只做整体缩放，乘积在着色器里封顶 30 格。
std::atomic<float>
    g_localLightMaxDistance{1.0f};
std::atomic<float>
    g_localLightIntensity{1.0f};
std::atomic<bool> g_cloudEnabled{true};
std::atomic<float> g_cloudBottom{1300.0f};
std::atomic<float> g_cloudThickness{2100.0f};
std::atomic<float> g_cloudCoverage{0.48f};
std::atomic<float> g_cloudDensity{2.40f};
std::atomic<float> g_cloudDetailStrength{0.14f};
std::atomic<float> g_cloudWindSpeed{10.0f};
std::atomic<float> g_cloudEvolution{0.80f};
std::atomic<uint32_t> g_cloudQuality{1u};
std::atomic<float> g_cloudSize{1.0f};
std::atomic<float> g_cloudDistribution{0.50f};
std::atomic<float> g_cloudBaseFeather{0.10f};
std::atomic<float> g_cloudSunlightStrength{1.0f};
std::atomic<float> g_cloudPhiIntensity{1.5f};
std::atomic<float> g_cloudPhiDepthPow{1.0f};
std::atomic<float> g_cloudPhiDepthBias{0.0f};
std::atomic<float> g_cloudMsTailStrength{0.5f};
std::atomic<float> g_cloudSunReddening{0.0f};
std::atomic<float> g_cloudAdaptiveLightRange{2000.0f};
std::atomic<float> g_cloudFarFadeDistance{80000.0f};
std::atomic<float> g_planeCloudEnabled{0.0f};
std::atomic<float> g_planeCloudHeightOffset{6000.0f};
std::atomic<float> g_planeCloudScale{1.0f};
std::atomic<float> g_planeCloudSpeed{1.0f};
std::atomic<float> g_planeCloudDensity{4.0f};
std::atomic<float> g_planeCloudCoverage{0.45f};
std::atomic<float> g_planeCloudOctaves{5.0f};
std::atomic<float> g_planeCloudFadeDistance{100000.0f};
std::atomic<float> g_planeCloudBrightness{1.0f};
std::atomic<uint32_t> g_cloudResFloorPercent{0u};
std::atomic<bool> g_cloudPostRrComposite{true};
std::atomic<float> g_cloudFarStepDensity{0.45f};
std::atomic<bool> g_cloudFadeFollowsRrMode{true};
std::atomic<float> g_cloudTwoTierStep{1.0f};
std::atomic<float> g_cloudMeanDepthReproject{1.0f};
std::atomic<float> g_cloudTopStretch{0.0f};
std::atomic<float> g_cloudDetileStrength{10.0f};
std::atomic<float> g_cloudType{0.8f};
std::atomic<float> g_cloudErosionScale{1.0f};
std::atomic<float> g_hpDetailStrengthCu{0.55f};
std::atomic<float> g_hpDetailStrengthTcu{0.70f};
std::atomic<float> g_hpDetailStrengthCb{0.85f};
std::atomic<float> g_hpBottomSmoothHeight{0.08f};
std::atomic<float> g_hpWispyEdgeWidth{0.40f};
std::atomic<float> g_hpWispyReach{0.15f};
std::atomic<float> g_hpWispyTopHeight{0.75f};
std::atomic<float> g_hpWispyTopHardness{2.0f};
std::atomic<float> g_hpWispyWeight{0.45f};
std::atomic<float> g_hpBillowyWeight{0.45f};
std::atomic<float> g_hpLoCoverTopMax{2.2f};
std::atomic<float> g_hpLoCoverTopStr{1.0f};
std::atomic<float> g_hpCovMapScale{1.0f};
std::atomic<float> g_hpScWorleyStrength{0.25f};
std::atomic<float> g_hpScHeightScale{0.35f};
std::atomic<float> g_hpScCellThickStr{0.6f};
std::atomic<float> g_hpForwardEcc{0.85f};
std::atomic<float> g_hpBackwardEcc{0.30f};
std::atomic<float> g_hpPhiFwdIntensity{0.6f};
std::atomic<float> g_hpPhiFwdDepthPow{1.0f};
std::atomic<float> g_hpPhiFwdDepthBias{0.0f};
std::atomic<float> g_hpPhiFwdCompress{1.0f};
std::atomic<float> g_hpScatterOdScale{0.10f};
std::atomic<float> g_hpScatterCurvePow{1.0f};
std::atomic<float> g_hpMsEccentricity{0.5f};
std::atomic<float> g_hpAoUpwardScale{1.0f};
std::atomic<float> g_hpFadeStart{0.30f};
std::atomic<float> g_hpPowderIntensity{1.0f};
std::atomic<float> g_cloudWeatherScale{0.75f};
std::atomic<float> g_cloudSilver{0.08f};
std::atomic<float> g_cloudViewDistance{25000.0f};
std::atomic<float> g_cloudPhiBoundaryConfidence{1.0f};
std::atomic<float> g_cloudPhiMsBuildScale{1.0f};
std::atomic<float> g_cloudPhiCompress{1.5f};
std::atomic<float> g_cloudPhiConeRatio{2.0f};
std::atomic<float> g_cloudSourceOdScale{1.0f};
std::atomic<float> g_cloudSourceCurvePow{1.0f};
std::atomic<float> g_cloudAoUpwardScale{1.0f};
std::atomic<float> g_cloudSunsetAmbientTint{0.6f};
std::atomic<float> g_cloudBottomSmoothHeight{0.22f};
std::atomic<float> g_cloudBottomSmoothPow{1.0f};
std::atomic<float> g_cloudTwoTierThreshold{0.004f};
std::atomic<float> g_cloudMoonStrength{1.0f};
std::atomic<float> g_cloudMoonPhase{1.0f};
std::atomic<float> g_cloudDetailScale{1.0f};
std::atomic<float> g_cloudDetailContrast{1.0f};
std::atomic<float> g_cloudProfileCurve{1.0f};
std::atomic<float> g_cloudAmbientTopMul{1.0f};
std::atomic<float> g_cloudAmbientBottomMul{1.0f};
std::atomic<float> g_cloudAmbientSunSide{0.35f};
std::atomic<float> g_cloudAmbientHueFollow{1.0f};
std::atomic<float> g_cloudSkyHueCool{0.65f};
std::atomic<float> g_cloudSkyHueEnergy{0.50f};
std::atomic<float> g_cloudSkyDirStrength{0.0f};
std::atomic<float> g_cloudAmbientUnify{0.0f};
std::atomic<float> g_cloudGroundAlbedo{0.20f};
std::atomic<float> g_cloudGroundBounce{1.0f};
std::atomic<float> g_cloudSunlightResidual{0.0f};
std::atomic<float> g_cloudPhaseFloor{0.0f};
std::atomic<float> g_cloudScatterTint{0.35f};
std::atomic<float> g_cloudMsOctaves{2.0f};
std::atomic<float> g_cloudMsAttenuation{0.5f};
std::atomic<float> g_cloudMsEccentricity{0.5f};
std::atomic<float> g_cloudSunPathReddening{0.6f};
std::atomic<float> g_cloudAtmExtinction{1.0f};
std::atomic<float> g_cloudPowderMode{1.0f};
std::atomic<float> g_fogDynamicLightStrength{1.0f};
std::atomic<float> g_fogGroundAnchorStrength{1.0f};
std::atomic<float> g_fogSkyAmbientStrength{1.0f};
std::atomic<float> g_fogTemporalJitterStrength{1.0f};
std::atomic<bool> g_cloudFogLightDescriptorsReady{false};
VkDescriptorSet g_cloudFogLightSetBound = VK_NULL_HANDLE;
VkBuffer g_cloudFogLightListBound = VK_NULL_HANDLE;
VkBuffer g_cloudFogLightGridBound = VK_NULL_HANDLE;
std::atomic<float> g_cloudSkyVisStrength{0.0f};
std::atomic<float> g_cloudSkyVisSteps{0.0f};
std::atomic<float> g_cloudTemporalDamp{0.0f};
std::atomic<float> g_cloudBottomOcclusion{0.0f};
std::atomic<float> g_cloudAerialStrength{0.0f};
std::atomic<float> g_groundAerialStrength{0.0f};
std::atomic<float> g_groundAerialOcclusion{0.0f};
std::atomic<float> g_cloudProfileType{0.25f};
std::atomic<float> g_cloudProfileTypeSpread{0.0f};
std::atomic<float> g_cloudWispyStrength{0.25f};
std::atomic<float> g_cloudWispyEdgeWidth{0.0f};
std::atomic<float> g_cloudWispyReach{0.0f};
std::atomic<float> g_cloudWispyTopHeight{0.0f};
std::atomic<float> g_waterCrestBacklit{1.0f};
std::atomic<float> g_waterCausticDispersion{0.4f};
std::atomic<float> g_waterCausticDepthDiffusion{0.5f};
std::atomic<float> g_waterPhysicalPhase{0.0f};
std::atomic<float> g_sunDiskLuminance{1.20f};
std::atomic<float> g_sunHaloStrength{0.18f};
std::atomic<float> g_sunAngularDiameterDegrees{0.53f};
std::atomic<float> g_starIntensity{1.0f};
std::atomic<float> g_sunTintStrength{0.6f};
std::atomic<bool> g_skyScatterEnabled{true};
std::atomic<float> g_skyScatterIntensity{1.0f};
std::atomic<float> g_ozoneStrength{1.0f};
std::atomic<float> g_cloudPowderStrength{1.0f};

// 2026-11-01 天空散射模型（0 = Bruneton 预计算 LUT，1 = Hillaire 结构）
// + A 档辐亮度标定倍率。车道 = CameraUBO.skyModelParams.x / .y，
// 设置界面经 Java_com_example_vulkan_NativeVulkan_setSkyModelSettings 下发。
std::atomic<int> g_skyModel{0};
std::atomic<float> g_skyModelScale{1.0f};
// 2026-11-01 追加两个大气可调项（车道 = CameraUBO.skyModelParams.z / .w）：
//   g_skyNightAmbient = 夜间天空底光倍率（0 = 关，1 = 出厂）
//   g_skyRainDim      = 雨天/雷暴天空压暗倍率（0 = 不压暗，1 = 出厂，>1 更狠）
std::atomic<float> g_skyNightAmbient{1.0f};
std::atomic<float> g_skyRainDim{1.0f};
std::atomic<float> g_auroraStrength{0.0f};
std::atomic<float> g_auroraBrightness{1.0f};
std::atomic<float> g_auroraFlowSpeed{1.0f};
std::atomic<float> g_galaxyScale{0.85f};
std::atomic<float> g_galaxyTiltDegrees{30.0f};
std::atomic<float> g_galaxyAzimuthDegrees{-45.0f};
std::atomic<float> g_galaxyMix{0.5f};
std::atomic<float> g_cloudSunForwardScattering{1.0f};
std::atomic<float> g_fogBaseDensity{1.0f};
std::atomic<float> g_fogHeightBlock{65.0f};
std::atomic<float> g_fogScaleHeight{50.0f};
std::atomic<float> g_fogAnisotropyG{0.65f};
std::atomic<float> g_fogSunStrength{1.0f};
std::atomic<float> g_fogAmbientStrength{1.0f};
std::atomic<float> g_fogMaxDistance{672.0f};
std::atomic<float> g_fogStartDistance{0.0f};
std::atomic<uint32_t> g_fogSunSteps{4u};
std::atomic<bool> g_fogEnabled{true};
std::atomic<float> g_fogAmount{0.88f};
std::atomic<float> g_fogCenterHeight{80.0f};
std::atomic<float> g_fogThickness{128.0f};
std::atomic<float> g_fogScale{1200.0f};
std::atomic<float> g_fogSpeed{1.0f};
std::atomic<uint32_t> g_fogDetailLayers{2u};
std::atomic<float> g_fogViewDistance{256.0f};
std::atomic<uint32_t> g_fogSteps{16u};
std::atomic<float> g_fogExtinction{0.020f};
std::atomic<float> g_fogRainBoost{2.0f};
std::atomic<float> g_fogDawnBoost{1.8f};
std::atomic<bool> g_fogGroundMistEnabled{true};
std::atomic<float> g_fogGroundMistHeight{1.0f};
std::atomic<float> g_fogGroundMistExtinction{0.15f};
std::atomic<float> g_fogGroundMistDistance{48.0f};
// 2026-10-31：g_fogWisp* 四项随"雨天地面雾浪"整链删除（用户实测无效果）。
std::atomic<float> g_fogRainMediumMix{1.0f};
float g_cloudWorldAnchorX = 0.0f;
float g_cloudWorldAnchorZ = 0.0f;
float g_cloudWorldAnchorDimension = 0.0f;
bool g_cloudWorldAnchorValid = false;
std::atomic<bool> g_rainWallEnabled{true};
std::atomic<float> g_rainWallWetness{1.0f};
std::atomic<float> g_rainWallDripDensity{0.38f};
std::atomic<float> g_rainWallDripSpeed{1.0f};
std::atomic<float> g_rainWallDripSize{1.0f};
std::atomic<float> g_rainWallDripBump{1.0f};
std::atomic<float> g_rainWallDistance{64.0f};
std::atomic<float> g_waterStyleWaveScale{1.0f};
std::atomic<float> g_waterStyleTintMix{1.0f};
std::atomic<float> g_waterStyleDensityScale{1.0f};
std::atomic<float> g_waterStyleCausticStrength{2.0f};
std::atomic<float> g_waterStyleWaveSpeed{1.0f};
std::atomic<float> g_waterStyleSwellScale{1.0f};
std::atomic<float> g_waterStyleMidScale{1.0f};
std::atomic<float> g_waterStyleFineScale{1.0f};
std::atomic<float> g_waterWaveCalibration{1.0f};
std::atomic<float> g_waterWaveStrength{1.0f};
std::atomic<float> g_waterWaveBlend{1.0f};
std::atomic<float> g_waterReflectCausticStrength{4.0f};
std::atomic<float> g_waterSimSpongeWidth{0.10f};
std::atomic<float> g_waterSimGaussianSource{1.0f};
std::atomic<float> g_waterSimDampingScale{1.0f};
std::atomic<float> g_waterSimPropagationSpeed{1.0f};
std::atomic<float> g_waterSssNonlinear{0.5f};
std::atomic<float> g_waterSssBoost{2.0f};
std::atomic<float> g_waterSssPhaseG{0.8f};
std::atomic<float> g_waterSssStrength{1.0f};
std::atomic<float> g_waterWaveLengthScale{1.0f};
std::atomic<float> g_waterWaveDirection{0.0f};
std::atomic<float> g_waterWaveCrestSharpness{1.0f};
std::atomic<float> g_waterWaveWarpStrength{1.0f};
std::atomic<float> g_waterWaveFbmWavelength{3.5f};
std::atomic<float> g_waterWaveFbmWarp{0.90f};
std::atomic<float> g_waterWaveFbmOctaves{12.0f};
std::atomic<float> g_waterWaveFbmWaves{8.0f};
std::atomic<uint32_t> g_perfAoNearSamples{2u};
std::atomic<uint32_t> g_perfAoMidSamples{1u};
std::atomic<uint32_t> g_perfGiBounces{1u};
std::atomic<uint32_t> g_perfSunShadowSamples{2u};
std::atomic<uint32_t> g_perfLocalLightRisCandidates{4u};
std::atomic<uint32_t> g_perfLocalLightAreaSamples{2u};
std::atomic<uint32_t> g_perfWaterReflectionInterval{1u};
std::atomic<uint32_t> g_perfGlossyFullDistance{64u};
std::atomic<uint64_t> g_perfDynamicRtasInterval{2u};
std::atomic<float> g_perfRtSpp{1.0f};


std::atomic<uint32_t> g_giRoulette{1u};
std::atomic<uint32_t> g_motionDebugView{0u};
std::atomic<uint32_t> g_motionVectorsEnabled{1u};
std::atomic<uint32_t> g_sunMarchFarDistance{32u};
bool g_reflexSupported = false;
bool g_pclSupported = false;
bool g_reflexLowLatencyAvailable = false;
int g_reflexMode = 1;
std::mutex g_reflexMutex;



sl::FrameToken*
    g_currentSLFrameToken = nullptr;
uint32_t
    g_currentSLFrameIndex = 0;
uint32_t
    g_nextSLFrameIndex = 0;
bool
    g_currentSLFrameValid = false;
uint32_t
    g_lastRenderSubmitStartFrame = UINT32_MAX;
uint32_t
    g_lastRenderSubmitEndFrame = UINT32_MAX;
uint32_t
    g_lastPresentStartFrame = UINT32_MAX;
uint32_t
    g_lastPresentEndFrame = UINT32_MAX;


bool g_slInitialized = false;
PFN_slGetFeatureFunction g_slGetFeatureFunction=nullptr;
PFN_slGetFeatureRequirements
    g_slGetFeatureRequirements = nullptr;


PFN_slInit g_slInit=nullptr;
PFN_slShutdown g_slShutdown=nullptr;
PFN_slSetVulkanInfo g_slSetVulkanInfo=nullptr;
PFN_slIsFeatureSupported g_slIsFeatureSupported=nullptr;
PFN_slSetFeatureLoaded g_slSetFeatureLoaded=nullptr;
PFN_slReflexSetOptions
    g_slReflexSetOptions = nullptr;
PFN_slReflexGetState
    g_slReflexGetState = nullptr;
PFN_slReflexSleep
    g_slReflexSleep = nullptr;
PFN_slPCLSetMarker
    g_slPCLSetMarker = nullptr;
PFN_slDLSSSetOptions g_slDLSSSetOptions = nullptr;
PFN_slDLSSGetOptimalSettings
    g_slDLSSGetOptimalSettings = nullptr;
PFN_slDLSSDSetOptions
    g_slDLSSDSetOptions = nullptr;
PFN_slDLSSDGetOptimalSettings
    g_slDLSSDGetOptimalSettings = nullptr;


PFN_slEvaluateFeature g_slEvaluateFeature=nullptr;
sl::Constants g_slConstants{};
PFN_slSetConstants g_slSetConstants=nullptr;
PFN_slGetNewFrameToken g_slGetNewFrameToken=nullptr;
PFN_slSetTagForFrame
    g_slSetTagForFrame = nullptr;
PFN_slFreeResources g_slFreeResources = nullptr;
PFN_slDLSSGetState g_slDLSSGetState = nullptr;
PFN_slDLSSDGetState g_slDLSSDGetState = nullptr;
std::atomic<uint64_t> g_slVramDlssBytes{0};
std::atomic<uint64_t> g_slVramDlssdBytes{0};
std::atomic<uint64_t> g_slVramDlssgBytes{0};
VkImageView g_atlasImageView = VK_NULL_HANDLE;
VkSampler g_atlasSampler = VK_NULL_HANDLE;
bool g_atlasDirty = false;
VkSampler g_pbrAtlasSampler = VK_NULL_HANDLE;
VkImageView g_atlasImageViewAtReload = VK_NULL_HANDLE;
VkSampler g_atlasSamplerAtReload = VK_NULL_HANDLE;
bool g_atlasReloadAwaitingPublish = false;
VkImage     g_dhColorImage   = VK_NULL_HANDLE;
VkImageView g_dhColorView    = VK_NULL_HANDLE;
VkSampler   g_dhColorSampler = VK_NULL_HANDLE;
VkImage     g_dhDepthImage   = VK_NULL_HANDLE;
VkImageView g_dhDepthView    = VK_NULL_HANDLE;
VkSampler   g_dhDepthSampler = VK_NULL_HANDLE;
VkImage     g_dhAtlasImage   = VK_NULL_HANDLE;
VkImageView g_dhAtlasView    = VK_NULL_HANDLE;
VkSampler   g_dhAtlasSampler = VK_NULL_HANDLE;
int         g_dhTextureWidth  = 0;
int         g_dhTextureHeight = 0;
bool        g_dhTexturesValid = false;
bool        g_dhTexturesSameDevice = false;
uint64_t    g_dhTexturesRevision = 0;
uint64_t    g_dhTexturesBoundRevision = 0;
bool        g_dhFallbackWritten = false;
VkImageView g_dhFallbackAtlasView = VK_NULL_HANDLE;
bool        g_dhAtlasUsable   = false;
uint64_t    g_dhAtlasRevision = 0;
uint64_t    g_dhAtlasBoundRevision = 0;
float g_dhFarFieldInvMvp[16]   = {};
float g_dhFarFieldMvp[16]      = {};
float g_dhFarFieldMode         = -1.0f;
float g_dhFarFieldStrength     = 0.65f;
float g_dhFarFieldGain         = 1.0f;
float g_dhFarFieldVariant      = 0.0f;
float g_dhFarFieldNear         = 0.0f;
float g_dhFarFieldFar          = 0.0f;
bool  g_dhFarFieldMatrixValid  = false;
int   g_dhFarFieldPushes       = 0;
int   g_dhFarFieldLogs         = 0;
int   g_dhFarFieldRejects      = 0;
int   g_dhFarFieldRejectLogs   = 0;
bool g_dynamicTextureBindingsNeedAtlasRefresh = false;
bool g_dynamicTextureBindingsCanPublish = false;
std::mutex g_rtxMemAccountMutex;
std::unordered_map<VkDeviceMemory, RtxMemLiveRecord> g_rtxMemLive;
std::unordered_map<uint64_t, RtxBufferChurnStats> g_rtxBufferChurn;
uint64_t g_rtxMemAllocCount = 0u;
uint64_t g_rtxMemFreeCount = 0u;
uint64_t g_rtxMemFreeUnknownCount = 0u;
uint64_t g_rtxMemAllocBytesTotal = 0u;
uint64_t g_rtxMemFreeBytesTotal = 0u;
uint64_t g_rtxMemLiveBytes = 0u;
uint64_t g_rtxMemLiveDeviceLocalBytes = 0u;
uint64_t g_rtxMemLiveHostBytes = 0u;
uint64_t g_rtxMemLivePeakBytes = 0u;
RtxMemObjCounters g_rtxMemObj;
VkBuffer g_perfL10RayStatsBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_perfL10RayStatsMemory = VK_NULL_HANDLE;
VkBuffer g_perfL10RayStatsStagingBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_perfL10RayStatsStagingMemory = VK_NULL_HANDLE;
uint32_t* g_perfL10RayStatsStagingMapped = nullptr;
uint64_t g_perfL10RayStatsSums[PERF_L10_RAY_STAT_COUNT]{};
uint64_t g_perfL10RayStatsWindowFrames = 0u;
uint32_t g_perfL10RayStatsActiveSlot = 0u;
PerfL10RayStatsSlot
    g_perfL10RayStatsSlots[PERF_L10_RAY_STAT_RING_SIZE]{};
VkBuffer g_textureUploadStaging = VK_NULL_HANDLE;
VkDeviceMemory g_textureUploadStagingMemory = VK_NULL_HANDLE;
void* g_textureUploadStagingMapped = nullptr;
VkDeviceSize g_textureUploadStagingBytes = 0u;
VkCommandBuffer g_textureUploadCommandBuffer = VK_NULL_HANDLE;
VkFence g_textureUploadFence = VK_NULL_HANDLE;
bool g_textureUploadFencePending = false;
std::vector<PbrSpriteRect> g_pbrSpriteRects;
bool g_pbrBc7AllowForNextUpload = true;
std::atomic<int>   g_postTonemapMode{0};
std::atomic<float> g_postExposureStrength{0.6f};
std::atomic<float> g_postContrast{1.35f};
std::atomic<float> g_postSaturation{1.08f};
std::atomic<float> g_postGamma{2.2f};
std::atomic<float> g_postGainR{1.0f};
std::atomic<float> g_postGainG{1.0f};
std::atomic<float> g_postGainB{1.0f};
std::atomic<int>   g_mirrorBounceCount{1};
std::atomic<int>   g_tlasCullDistance{0};
std::atomic<int>   g_tlasFrustumCullMargin{0};
std::atomic<float> g_postBloomStrength{0.0f};
std::atomic<float> g_cgExposureEV{0.00f};
std::atomic<float> g_cgBlackPoint{0.00f};
std::atomic<float> g_cgWhitePoint{1.00f};
std::atomic<float> g_cgContrast{1.00f};
std::atomic<float> g_cgPivot{0.18f};
std::atomic<float> g_cgLiftR{0.00f};
std::atomic<float> g_cgLiftG{0.00f};
std::atomic<float> g_cgLiftB{0.00f};
std::atomic<float> g_cgGammaR{1.00f};
std::atomic<float> g_cgGammaG{1.00f};
std::atomic<float> g_cgGammaB{1.00f};
std::atomic<float> g_cgGainR{1.00f};
std::atomic<float> g_cgGainG{1.00f};
std::atomic<float> g_cgGainB{1.00f};
std::atomic<float> g_cgShadowHue{200.00f};
std::atomic<float> g_cgShadowStrength{0.00f};
std::atomic<float> g_cgHighlightHue{45.00f};
std::atomic<float> g_cgHighlightStrength{0.00f};
std::atomic<float> g_cgVibrance{0.00f};
std::atomic<float> g_cgFade{0.00f};
std::atomic<float> g_cgVignette{0.00f};
std::atomic<float> g_cgVignetteRadius{0.60f};
std::atomic<float> g_cgHueRotate{0.00f};
std::atomic<float> g_cgTintGreen{0.00f};
PFN_vkCreateAccelerationStructureKHR g_rtxRealCreateAS = nullptr;
PFN_vkDestroyAccelerationStructureKHR g_rtxRealDestroyAS = nullptr;
VkDevice g_rtxRealASDevice = VK_NULL_HANDLE;
PFN_vkCmdBuildAccelerationStructuresKHR g_rtxRealCmdBuildAS = nullptr;
VkDevice g_rtxRealCmdBuildASDevice = VK_NULL_HANDLE;
PFN_vkCreateMicromapEXT g_rtxRealCreateMM = nullptr;
PFN_vkDestroyMicromapEXT g_rtxRealDestroyMM = nullptr;
VkDevice g_rtxRealMMDevice = VK_NULL_HANDLE;
std::timed_mutex g_chunkMutex;
std::atomic<uint64_t> g_chunkLockMaxWaitNanos{0u};
std::atomic<uint64_t> g_chunkLockMaxHoldNanos{0u};
std::atomic<uint64_t> g_captureLockMaxHoldNanos{0u};
std::atomic<uint64_t> g_captureLockMaxWaitNanos{0u};
std::atomic<uint64_t> g_chunkCmpRaw1MaxNanos{0};
std::atomic<uint64_t> g_chunkCmpRaw2MaxNanos{0};
std::atomic<uint64_t> g_chunkCmpMeshMaxNanos{0};
std::atomic<uint32_t> g_chunkCmpCount{0};
std::atomic<uint64_t> g_chunkUploadCalls{0};
std::atomic<uint64_t> g_chunkUploadDedup{0};
std::atomic<uint64_t> g_chunkCmpLastBytes{0};
std::atomic<uint64_t> g_chunkCmpLastVerts{0};
std::unordered_map<uint64_t, std::vector<Vertex>> g_chunkMeshes;
std::unordered_map<uint64_t, std::vector<float>> g_chunkMotion;
std::unordered_map<uint64_t, VoxyLodSource> g_voxyLodSources;
std::unordered_map<uint64_t, uint64_t> g_voxyLodSpatialOwners;
std::vector<Vertex> g_entityFrameVertices;
std::vector<Vertex> g_localPlayerFrameVertices;
uint64_t g_entityCachedHash = 0u;
bool g_entityCachedHashValid = false;
uint64_t g_localPlayerCachedHash = 0u;
bool g_localPlayerCachedHashValid = false;
std::vector<Vertex> g_particleFrameVertices;
std::vector<float> g_entityFrameMotion;
std::vector<float> g_localPlayerFrameMotion;
std::vector<float> g_particleFrameMotion;
float g_dynamicCaptureCameraX = 0.0f;
float g_dynamicCaptureCameraY = 0.0f;
float g_dynamicCaptureCameraZ = 0.0f;
bool g_entityFrameOpen = false;
bool g_particleFrameOpen = false;
std::atomic<bool> g_entityRebuildRequested{false};
uint64_t g_lastDynamicRTASUpdateFrame = UINT64_MAX;
bool g_dynamicRTASUpdatePending = false;
uint64_t g_lastLocalPlayerASUpdateFrame = UINT64_MAX;
bool g_localPlayerASUpdatePending = false;
VkImageView g_entityTextureViews[ENTITY_RT_TEXTURE_SLOT_COUNT]{};
VkSampler g_entityTextureSamplers[ENTITY_RT_TEXTURE_SLOT_COUNT]{};
bool g_entityTexturesDirty = false;
VkImageView g_entityNormalViews[ENTITY_RT_TEXTURE_SLOT_COUNT]{};
VkSampler g_entityNormalSamplers[ENTITY_RT_TEXTURE_SLOT_COUNT]{};
VkImageView g_entitySpecularViews[ENTITY_RT_TEXTURE_SLOT_COUNT]{};
VkSampler g_entitySpecularSamplers[ENTITY_RT_TEXTURE_SLOT_COUNT]{};
bool g_entityPbrTexturesDirty = false;
VkImage g_entityPbrFallbackImage = VK_NULL_HANDLE;
VkDeviceMemory g_entityPbrFallbackMemory = VK_NULL_HANDLE;
VkImageView g_entityPbrFallbackView = VK_NULL_HANDLE;
VkSampler g_entityPbrFallbackSampler = VK_NULL_HANDLE;
VkImageView g_particleTextureViews[PARTICLE_RT_TEXTURE_SLOT_COUNT]{};
VkSampler g_particleTextureSamplers[PARTICLE_RT_TEXTURE_SLOT_COUNT]{};
bool g_particleTexturesDirty = false;
VkImageView g_rainStreakTextureView = VK_NULL_HANDLE;
VkSampler g_rainStreakTextureSampler = VK_NULL_HANDLE;
bool g_rainStreakTextureDirty = false;
VkImageView g_starMapTextureView = VK_NULL_HANDLE;
VkSampler g_starMapTextureSampler = VK_NULL_HANDLE;
bool g_starMapTextureDirty = false;
VkImageView g_endSkyAtlasView = VK_NULL_HANDLE;
VkSampler g_endSkyAtlasSampler = VK_NULL_HANDLE;
bool g_endSkyAtlasDirty = false;
VkImage g_voxyAtlasImage = VK_NULL_HANDLE;
VkDeviceMemory g_voxyAtlasMemory = VK_NULL_HANDLE;
VkImageView g_voxyAtlasView = VK_NULL_HANDLE;
VkSampler g_voxyAtlasSampler = VK_NULL_HANDLE;
uint64_t g_voxyAtlasContentHash = 0u;
VkDescriptorSet g_voxyAtlasDescriptorSet = VK_NULL_HANDLE;
uint32_t g_voxyAtlasSize = 256u;
bool g_particleDebugEpisodeActive = false;
uint64_t g_particleDebugEpisodeSerial = 0u;
bool g_rainPuddleLoggedGeometry = false;
std::unordered_map<uint64_t, std::vector<uint8_t>>
    g_chunkRawPayloads;
std::unordered_map<uint64_t, ChunkUploadDigest> g_chunkUploadDigests;
std::mutex g_visibleSectionKeysMutex;
std::vector<uint64_t> g_visibleSectionKeys;
std::atomic<uint32_t> g_visibleSectionKeyCount{0u};
std::unordered_set<uint64_t> g_dirtySectionKeys;
std::atomic<bool> g_forceSectionASReset{false};
std::atomic<bool> g_worldGeometryDirty{false};
std::atomic<uint64_t> g_worldGeometryRevision{0u};
std::atomic<uint64_t> g_terrainGeometryRevision{0u};
std::mutex g_capturedSectionsMutex;
std::vector<CapturedSectionUpdate> g_capturedSections;
size_t g_capturedSectionsHead = 0u;
std::thread g_geometryCaptureThread;
std::mutex g_geometryCaptureStateMutex;
std::condition_variable g_geometryCaptureWake;

// 2026-10-31：脏标记 → 捕获线程的唤醒信号。
//   g_captureWorkRevision：每来一次脏标记就 +1，消费者拿它做等待谓词；
//   g_captureWorkerWaiting：消费者真的睡着时才置位，避免每次上传都做一次
//   notify（C2ME 也是只在"空→非空"这一条边上通知）。
std::atomic<uint64_t> g_captureWorkRevision{0u};
std::atomic<bool> g_captureWorkerWaiting{false};
bool g_geometryCaptureStop = false;
bool g_geometryCaptureStarted = false;

// 2026-10-31：脏 Section 取序的观测计数，只有捕获线程读写，不需要原子。
uint64_t g_dirtyOrderWakeCount = 0u;
uint64_t g_dirtyOrderScanKeyMax = 0u;
uint64_t g_dirtyOrderScanMicrosMax = 0u;
std::atomic<uint64_t> g_geometryCapturedSectionTotal{0u};
uint64_t g_geometryAdoptedSectionTotal = 0u;
std::atomic<uint64_t> g_geometryForceResetCount{0u};


std::mutex
    g_lightMutex;


std::unordered_map<
    uint64_t,
    std::vector<LocalLightRecord>
> g_lightSections;


size_t
    g_totalLocalLights = 0;


VkBuffer
    g_localLightGpuBuffer =
        VK_NULL_HANDLE;


VkDeviceMemory
    g_localLightGpuMemory =
        VK_NULL_HANDLE;


size_t
    g_localLightGpuCount = 0;


std::atomic<bool>
    g_localLightsDirty = true;
VkBuffer g_sharcHashEntriesBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_sharcHashEntriesMemory = VK_NULL_HANDLE;
VkBuffer g_sharcAccumulationBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_sharcAccumulationMemory = VK_NULL_HANDLE;
VkBuffer g_sharcResolvedBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_sharcResolvedMemory = VK_NULL_HANDLE;
VkBuffer g_sharcLockBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_sharcLockMemory = VK_NULL_HANDLE;
uint32_t g_sharcEntryCountResolved = 0u;
bool g_sharcBuffersReady = false;
bool g_sharcZeroed = false;
bool g_sharcCreateFailed = false;
uint64_t g_sharcHashEntriesAddress = 0u;
uint64_t g_sharcAccumulationAddress = 0u;
uint64_t g_sharcResolvedAddress = 0u;
uint64_t g_sharcLockAddress = 0u;
VkBuffer g_sharcProbeStagingBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_sharcProbeStagingMemory = VK_NULL_HANDLE;
uint32_t g_sharcProbeStage = 0u;
uint32_t g_sharcProbeTickCount = 0u;
uint32_t g_sharcProbeArmTick = 0u;
uint32_t g_sharcAbLoggedArm = 0xFFFFFFFFu;
VkPipelineLayout g_sharcResolvePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_sharcResolvePipeline = VK_NULL_HANDLE;
bool g_sharcResolvePipelineFailed = false;


VkBuffer
    g_localLightGridGpuBuffer =
        VK_NULL_HANDLE;


VkDeviceMemory
    g_localLightGridGpuMemory =
        VK_NULL_HANDLE;


size_t
    g_localLightGridCellCount = 0;


size_t
    g_localLightGridIndexCount = 0;


size_t
    g_localLightGridHashCapacity = 0;


bool
    g_cameraHistoryValid = false;


int32_t
    g_lastCameraFrame = -1;
float
    g_prevCameraViewInverse[16] = {};
float
    g_prevCameraProjInverse[16] = {};
bool
    g_hasPrevCameraInverse = false;


uint32_t
    g_localLightRevision = 1u;
VkBuffer g_vptDirectHistoryBuffers[2] =
{
    VK_NULL_HANDLE,
    VK_NULL_HANDLE
};
VkDeviceMemory g_vptDirectHistoryMemories[2] =
{
    VK_NULL_HANDLE,
    VK_NULL_HANDLE
};
uint32_t g_vptDirectHistoryWidth = 0u;
uint32_t g_vptDirectHistoryHeight = 0u;
VkDeviceSize g_vptDirectHistoryBytes = 0;
bool g_vptDirectHistoryNeedsClear = false;
VkBuffer g_vptBestLightBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vptBestLightMemory = VK_NULL_HANDLE;
VkDeviceSize g_vptBestLightBytes = 0;
uint32_t g_vptBestLightTileColumns = 0u;
uint32_t g_vptBestLightTileRows = 0u;
std::mutex g_vliMutex;
std::unordered_map<uint64_t, VLISectionCpuPage> g_vliSections;
std::unordered_map<uint64_t, std::vector<uint64_t>> g_vliChunkSectionIndex;
std::vector<VLITableSlot> g_vliTableCpu(VLI_PAGE_TABLE_CAPACITY);
std::vector<uint32_t> g_vliFreePages;
uint32_t g_vliNextPage = 0u;
uint32_t g_vliPageCapacity = VLI_INITIAL_SECTION_PAGES;
bool g_vliGrowthRequested = false;
std::vector<VLIPendingSection> g_vliPendingSections;
std::mutex g_vliUploadHashMutex;
std::atomic<uint64_t> g_vliUploadTotal{0};
std::mutex g_vliHotMutex;
std::unordered_map<uint64_t, uint32_t> g_vliHotCounts;
std::atomic<uint64_t> g_vliUploadGated{0};
std::atomic<uint64_t> g_vliUploadApplied{0};
std::unordered_map<uint64_t, uint64_t> g_vliUploadHashByKey;
std::vector<uint64_t> g_vliPageSectionKeys(
    VLI_INITIAL_SECTION_PAGES,
    VLI_INVALID_SECTION_KEY
);
uint64_t g_vliPageUseTick = 0u;
uint64_t g_vliGovernorEvictedPages = 0u;
uint64_t g_vliGovernorIgnoredSections = 0u;


std::unordered_set<uint32_t> g_vliDirtySourcePages;
std::unordered_set<uint32_t> g_vliDirtyTableSlots;
std::unordered_set<uint32_t> g_vliDirtySolveTableSlots;
std::unordered_set<uint32_t> g_vptDirtyEmitterPages;
std::unordered_set<uint32_t> g_vptDirtyEmitterAliasPages;
std::unordered_set<uint32_t> g_vptDirtyCandidatePages;
std::unordered_set<uint32_t> g_vptDirtySourceDirectoryPages;
uint32_t g_vptSourceDirectoryRevision = 1u;
uint32_t g_vptEmitterRevision = 1u;
VkBuffer g_vliPageTableBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vliPageTableMemory = VK_NULL_HANDLE;
VkBuffer g_vliPageBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vliPageMemory = VK_NULL_HANDLE;
VkBuffer g_vliScratchBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vliScratchMemory = VK_NULL_HANDLE;
VkBuffer g_vptSurfaceCacheBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vptSurfaceCacheMemory = VK_NULL_HANDLE;
VkBuffer g_vliWorkListBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vliWorkListMemory = VK_NULL_HANDLE;
VkBuffer g_vptEmitterIndexBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vptEmitterIndexMemory = VK_NULL_HANDLE;
VkBuffer g_vptEmitterAliasBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vptEmitterAliasMemory = VK_NULL_HANDLE;
VkBuffer g_vptCandidateBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vptCandidateMemory = VK_NULL_HANDLE;
VkBuffer g_vptSourceDirectoryBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_vptSourceDirectoryMemory = VK_NULL_HANDLE;
VkPipelineLayout g_vliComputePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_vliComputePipeline = VK_NULL_HANDLE;
bool g_vliNeedsInitialClear = true;
uint32_t g_vptTemporalFrame = 0u;
VramGovernorState g_vramGovernor;
std::vector<SectionMemorySlab>
    g_sectionMemorySlabs;
VkDeviceSize g_sectionMemoryPoolKeptBytes = 0;
uint64_t g_sectionMemoryPoolHits = 0;
uint64_t g_sectionMemoryPoolMisses = 0;
uint64_t g_sectionMemoryAllocateCalls = 0;
double g_sectionMemoryAllocateMs = 0.0;
uint64_t g_sectionMemoryAllocateBytes = 0u;
uint64_t g_sectionMemoryFreeBytes = 0u;
uint64_t g_sectionASCreateCalls = 0u;
uint64_t g_sectionASRetireCalls = 0u;
double g_sectionASCreateMs = 0.0;
double g_sectionASRetireMs = 0.0;
uint64_t g_sectionASRebuildSameTris = 0u;
uint64_t g_sectionASRefitCandidateHits = 0u;
uint64_t g_sectionASRefitHits = 0u;
uint64_t g_sectionMemoryOomSkipCount = 0u;
VramProbeState g_vramProbe;
uint64_t g_rtFrameEnterCount = 0u;
uint64_t g_rtFrameDoneCount = 0u;
VkBuffer g_sectionQuadIndexBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_sectionQuadIndexMemory = VK_NULL_HANDLE;
VkDeviceAddress g_sectionQuadIndexAddress = 0u;
std::atomic<uint32_t> g_sparseEffectiveFrameIndex{0u};
std::atomic<float> g_sparseEffectiveRate{0.0f};
VkImage g_sparseActiveMaskImage = VK_NULL_HANDLE;
VkDeviceMemory g_sparseActiveMaskMemory = VK_NULL_HANDLE;
VkImageView g_sparseActiveMaskView = VK_NULL_HANDLE;
VkImageLayout g_sparseActiveMaskLayout = VK_IMAGE_LAYOUT_UNDEFINED;
uint32_t g_sparseActiveMaskWidth = 0u;
uint32_t g_sparseActiveMaskHeight = 0u;
bool g_sparseActiveMaskNeedsClear = true;
VkShaderModule g_sparseActiveMaskShader = VK_NULL_HANDLE;
VkDescriptorSetLayout g_sparseActiveMaskSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_sparseActiveMaskPool = VK_NULL_HANDLE;
VkDescriptorSet g_sparseActiveMaskSet = VK_NULL_HANDLE;
VkPipelineLayout g_sparseActiveMaskPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_sparseActiveMaskPipeline = VK_NULL_HANDLE;
bool g_sparseActiveMaskPipelineFailed = false;
BloomMip g_bloomMips[BLOOM_MIP_COUNT];
VkSampler g_bloomSampler = VK_NULL_HANDLE;
VkDescriptorSetLayout g_bloomPrefilterSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_bloomBlurSetLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_bloomCompositeSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_bloomDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_bloomPrefilterSet = VK_NULL_HANDLE;
VkDescriptorSet g_bloomDownsampleSets[BLOOM_MIP_COUNT]{};
VkDescriptorSet g_bloomUpsampleSets[BLOOM_MIP_COUNT]{};
VkDescriptorSet g_bloomCompositeSet = VK_NULL_HANDLE;
VkPipelineLayout g_bloomPrefilterPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout g_bloomBlurPipelineLayout = VK_NULL_HANDLE;
VkPipelineLayout g_bloomCompositePipelineLayout = VK_NULL_HANDLE;
VkPipeline g_bloomPrefilterPipeline = VK_NULL_HANDLE;
VkPipeline g_bloomDownsamplePipeline = VK_NULL_HANDLE;
VkPipeline g_bloomUpsamplePipeline = VK_NULL_HANDLE;
VkPipeline g_bloomCompositePipeline = VK_NULL_HANDLE;
VkBuffer g_dynamicLightBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_dynamicLightMemory = VK_NULL_HANDLE;
DynamicLightUpload g_dynamicLightUpload{};
bool g_dynamicLightsDirty = false;
VkPipeline g_sharcUpdatePipeline = VK_NULL_HANDLE;
VkBuffer g_sharcUpdateSbtBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_sharcUpdateSbtMemory = VK_NULL_HANDLE;
VkStridedDeviceAddressRegionKHR g_sharcUpdateRaygenRegion{};
VkStridedDeviceAddressRegionKHR g_sharcUpdateMissRegion{};
VkStridedDeviceAddressRegionKHR g_sharcUpdateHitRegion{};
VkStridedDeviceAddressRegionKHR g_sharcUpdateCallableRegion{};
bool g_sharcUpdatePipelineFailed = false;


VkPipeline g_giHalfPipeline = VK_NULL_HANDLE;
VkBuffer g_giHalfSbtBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_giHalfSbtMemory = VK_NULL_HANDLE;
VkStridedDeviceAddressRegionKHR g_giHalfRaygenRegion{};
VkStridedDeviceAddressRegionKHR g_giHalfMissRegion{};
VkStridedDeviceAddressRegionKHR g_giHalfHitRegion{};
VkStridedDeviceAddressRegionKHR g_giHalfCallableRegion{};
bool g_giHalfPipelineFailed = false;
uint32_t g_sharcStatsFrameTick = 0u;
bool g_sharcStatsDone = false;
VkDescriptorPool g_rtDescriptorRingPool = VK_NULL_HANDLE;
VkDescriptorSet g_rtDescriptorRingSets[RT_DESCRIPTOR_RING_SIZE] = {};
VkFence g_rtDescriptorRingFences[RT_DESCRIPTOR_RING_SIZE] = {};
bool g_rtDescriptorRingFencePending[RT_DESCRIPTOR_RING_SIZE] = {};
uint32_t g_rtDescriptorRingNext = 0u;
uint32_t g_rtDescriptorRingLiveIndex = UINT32_MAX;
VkCommandPool g_rtDescriptorRingCmdPool = VK_NULL_HANDLE;
VkCommandBuffer g_rtDescriptorRingCmd = VK_NULL_HANDLE;
bool g_rtDescriptorRingReady = false;
bool g_rtDescriptorRingLastPublishUsedRing = false;
uint32_t g_rtDescriptorRingUsedMask = 0u;
size_t g_vliPendingBacklogForLog = 0u;
double g_vliMsIndex = 0.0;
double g_vliMsAlias = 0.0;
double g_vliMsApply = 0.0;
double g_vliMsGather = 0.0;
double g_vliMsUpload = 0.0;
double g_vliMsLockWait = 0.0;
double g_vliMsSolve = 0.0;
size_t g_vliGatherPages = 0u;
std::vector<uint32_t> g_vliUploadArena;
std::vector<VliUploadRef> g_vliSourceUploads;
std::vector<VliUploadRef> g_vliEmitterUploads;
std::vector<VliUploadRef> g_vliEmitterAliasUploads;
std::vector<uint32_t> g_nrdConstantBufferOffsets;
VkPipeline g_diConfidencePipeline = VK_NULL_HANDLE;
VkPipelineLayout g_diConfidencePipelineLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_diConfidenceSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_diConfidencePool = VK_NULL_HANDLE;
VkDescriptorSet g_diConfidenceSets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImage g_diGradientImage = VK_NULL_HANDLE;
VkDeviceMemory g_diGradientMemory = VK_NULL_HANDLE;
VkImageView g_diGradientView = VK_NULL_HANDLE;
VkImageLayout g_diGradientLayout = VK_IMAGE_LAYOUT_UNDEFINED;
VkImage g_diConfidenceImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkDeviceMemory g_diConfidenceMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageView g_diConfidenceViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageLayout g_diConfidenceLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED
};
VkImage g_diLumaImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkDeviceMemory g_diLumaMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageView g_diLumaViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageLayout g_diLumaLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED
};
uint32_t g_diConfidenceWidth = 0u;
uint32_t g_diConfidenceHeight = 0u;
uint32_t g_diConfidenceParity = 0u;
bool g_diConfidenceReady = false;
uint32_t g_diConfidenceWarmupFrames = 0u;
bool g_diConfidenceDescriptorsDirty = true;
VkImageView g_nrdConfidenceViewThisFrame = VK_NULL_HANDLE;
bool g_directAtrousEnabled = false;
bool g_directAtrousReady = false;
VkPipeline g_directAtrousPipeline = VK_NULL_HANDLE;
VkPipelineLayout g_directAtrousPipelineLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_directAtrousSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_directAtrousPool = VK_NULL_HANDLE;
VkDescriptorSet g_directAtrousSets[3] = {
    VK_NULL_HANDLE,
    VK_NULL_HANDLE,
    VK_NULL_HANDLE
};
uint32_t g_directAtrousWidth = 0u;
uint32_t g_directAtrousHeight = 0u;
VkImage g_diLinearImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkDeviceMemory g_diLinearMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageView g_diLinearViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageLayout g_diLinearLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED
};
VkImage g_diVarianceImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkDeviceMemory g_diVarianceMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageView g_diVarianceViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageLayout g_diVarianceLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED
};
VkPipeline g_diReprojectPipeline = VK_NULL_HANDLE;
VkPipelineLayout g_diReprojectPipelineLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_diReprojectSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_diReprojectPool = VK_NULL_HANDLE;
VkDescriptorSet g_diReprojectSets[2] = {
    VK_NULL_HANDLE,
    VK_NULL_HANDLE
};
VkPipeline g_diMomentsPipeline = VK_NULL_HANDLE;
VkPipelineLayout g_diMomentsPipelineLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_diMomentsSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_diMomentsPool = VK_NULL_HANDLE;
VkDescriptorSet g_diMomentsSets[2] = {
    VK_NULL_HANDLE,
    VK_NULL_HANDLE
};
VkImage g_diHistoryColorImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkDeviceMemory g_diHistoryColorMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageView g_diHistoryColorViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageLayout g_diHistoryColorLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED
};
VkImage g_diHistoryDepthImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkDeviceMemory g_diHistoryDepthMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageView g_diHistoryDepthViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageLayout g_diHistoryDepthLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED
};
VkImage g_diHistoryNormalImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkDeviceMemory g_diHistoryNormalMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageView g_diHistoryNormalViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageLayout g_diHistoryNormalLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED
};
VkImage g_diHistoryLengthImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkDeviceMemory g_diHistoryLengthMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageView g_diHistoryLengthViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageLayout g_diHistoryLengthLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED
};
VkImage g_diMomentsImages[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkDeviceMemory g_diMomentsMemories[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageView g_diMomentsViews[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
VkImageLayout g_diMomentsLayouts[2] = {
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_UNDEFINED
};
uint32_t g_diHistoryParity = 0u;
bool g_diHistoryNeedsReset = true;
VkPipeline g_diPreparePipeline = VK_NULL_HANDLE;
VkPipelineLayout g_diPreparePipelineLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_diPrepareSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_diPreparePool = VK_NULL_HANDLE;
VkDescriptorSet g_diPrepareSet = VK_NULL_HANDLE;
uint64_t g_sunShadowBuiltRevision = ~0ull;
float g_sunShadowBuiltPhase = 0.0f;
float g_sunShadowBuiltCenter[3] = { 0.0f, 0.0f, 0.0f };
uint64_t g_sunShadowFarBuiltRevision = ~0ull;
float g_sunShadowFarBuiltPhase = 0.0f;
float g_sunShadowFarBuiltCenter[3] = { 0.0f, 0.0f, 0.0f };
uint64_t g_cloudSpvStampSeen = 0ull;
int g_cloudSpvWatchCountdown = 0;
BrunetonLutImage g_brTransmittance;
BrunetonLutImage g_brIrradiance;
BrunetonLutImage g_brDeltaIrradiance;
BrunetonLutImage g_brScattering;
BrunetonLutImage g_brSingleMie;
BrunetonLutImage g_brDeltaRayleigh;
BrunetonLutImage g_brDeltaMie;
BrunetonLutImage g_brDeltaMultiple;
BrunetonLutImage g_brDeltaScatteringDensity;
VkSampler g_brLutSampler = VK_NULL_HANDLE;
VkDescriptorSetLayout g_brSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_brPool = VK_NULL_HANDLE;
VkDescriptorSet g_brSet = VK_NULL_HANDLE;
VkPipelineLayout g_brLayout = VK_NULL_HANDLE;
VkPipeline g_brPipelines[6] = {};
bool g_brPrecomputed = false;
VkBuffer g_brReadbackBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_brReadbackMemory = VK_NULL_HANDLE;
bool g_brPrecomputeOnlyReleased = false;
VkImage g_brPlaceholderImage = VK_NULL_HANDLE;
VkDeviceMemory g_brPlaceholderMemory = VK_NULL_HANDLE;
VkImageView g_brPlaceholderView = VK_NULL_HANDLE;
VkImage g_waterFftStateImage = VK_NULL_HANDLE;
VkDeviceMemory g_waterFftStateMemory = VK_NULL_HANDLE;
VkImageView g_waterFftStateView = VK_NULL_HANDLE;
VkImage g_waterFftSlopeImage = VK_NULL_HANDLE;
VkDeviceMemory g_waterFftSlopeMemory = VK_NULL_HANDLE;
VkImageView g_waterFftSlopeView = VK_NULL_HANDLE;
VkImageView g_waterFftSlopeStorageView = VK_NULL_HANDLE;
VkSampler g_waterFftSlopeSampler = VK_NULL_HANDLE;
VkDescriptorSetLayout g_waterFftSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_waterFftDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_waterFftDescriptorSet = VK_NULL_HANDLE;
VkPipelineLayout g_waterFftPipelineLayout = VK_NULL_HANDLE;
VkPipeline g_waterFftPipeline = VK_NULL_HANDLE;
bool g_waterFftLayoutReady = false;
int32_t g_waterFftWorkingLayer = 0;
std::chrono::steady_clock::time_point g_waterFftLastUpdate{};
bool g_waterFftLastUpdateValid = false;
float g_waterFftTimeSeconds = 0.0f;
std::atomic<bool> g_waterFftAvailable{false};
bool g_waterFftUpdatedThisFrame = false;
std::atomic<float> g_waterFftWindSpeed{7.0f};
std::atomic<float> g_waterFftWindDirection{45.0f};
std::atomic<float> g_waterFftTilePeriod{128.0f};
std::atomic<float> g_waterFftAmplitude{1.0f};
std::atomic<float> g_waterFftChoppiness{1.0f};
std::atomic<float> g_waterFftDetail{1.0f};
std::atomic<float> g_waterFftRateHz{30.0f};
std::atomic<float> g_waterFftSwellGain{1.0f};
std::atomic<float> g_waterWeatherRain{0.0f};
std::atomic<bool>  g_waterFftEnabled{true};
VkImage g_waterCausticMapImage = VK_NULL_HANDLE;
VkDeviceMemory g_waterCausticMapMemory = VK_NULL_HANDLE;
VkImageView g_waterCausticMapView = VK_NULL_HANDLE;
VkImageView g_waterCausticMapMip0View = VK_NULL_HANDLE;
VkImageView g_waterCausticMapReflectView = VK_NULL_HANDLE;
VkImageView g_waterCausticMapReflectMip0View = VK_NULL_HANDLE;
VkSampler g_waterCausticMapSampler = VK_NULL_HANDLE;
VkRenderPass g_waterCausticMapRenderPass = VK_NULL_HANDLE;
VkFramebuffer g_waterCausticMapFramebuffer = VK_NULL_HANDLE;
VkFramebuffer g_waterCausticMapReflectFramebuffer = VK_NULL_HANDLE;
VkDescriptorSetLayout g_waterCausticMapSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_waterCausticMapDescriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_waterCausticMapDescriptorSet = VK_NULL_HANDLE;
VkPipelineLayout g_waterCausticMapPipelineLayout = VK_NULL_HANDLE;
VkImageView g_waterCausticMapStorageView = VK_NULL_HANDLE;
VkPipeline g_waterCausticMapPipeline = VK_NULL_HANDLE;
int64_t g_leakProbeInsideBytes = 0;
int64_t g_leakProbeOutsideBytes = 0;
uint64_t g_leakProbeEnterBytes = 0u;
uint64_t g_leakProbeLastExitBytes = 0u;
uint32_t g_leakProbeFrameCount = 0u;
int g_leakProbeDepth = 0;
std::chrono::steady_clock::time_point g_leakProbeWindowStart{};
uint32_t g_deviceInitStep = 0u;
VkPipeline g_rtPipelinePendingDestroy = VK_NULL_HANDLE;
VkBuffer g_rtSbtPendingDestroyBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_rtSbtPendingDestroyMemory = VK_NULL_HANDLE;
bool g_rtPipeRecycleArmed = false;
std::chrono::steady_clock::time_point g_rtPipeLastRecycle{};


uint64_t g_vliTableReinsertTotal = 0u;
std::unordered_map<uint64_t, DhLodSection> g_dhLodSections;
std::unordered_map<uint64_t, uint64_t> g_dhLodSpatialOwners;
std::vector<uint64_t> g_dhLodSpatialSnapshot;
VkImageView g_sunShadowFarPublishedView = VK_NULL_HANDLE;
bool g_sunShadowFarPublishedReady = false;

// 2026-11-01：近场太阳阴影图 -> RT set 的 binding 79。
// 与 far 版同一套 handle 缓存 + 变化才写，稳态零开销。
VkImageView g_sunShadowNearPublishedView = VK_NULL_HANDLE;
bool g_sunShadowNearPublishedReady = false;
uint32_t g_geometryPreLoopLockSkips = 0u;
double g_geometryMaxPreLoopLockWaitMs = 0.0;
double g_geometryMaxCountersLogMs = 0.0;
