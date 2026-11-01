// Orion backend — Streamline 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"



bool ResolveSLVulkanProxies()
{
    if(
        !g_slModule ||
        g_instance == VK_NULL_HANDLE ||
        g_device == VK_NULL_HANDLE
    )
    {
        return false;
    }


    if(!g_slVkGetInstanceProcAddrProxy)
    {
        g_slVkGetInstanceProcAddrProxy =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                GetProcAddress(
                    g_slModule,
                    "vkGetInstanceProcAddr"
                )
            );
    }


    if(!g_slVkGetDeviceProcAddrProxy)
    {
        g_slVkGetDeviceProcAddrProxy =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                GetProcAddress(
                    g_slModule,
                    "vkGetDeviceProcAddr"
                )
            );
    }


    if(
        !g_slVkGetInstanceProcAddrProxy ||
        !g_slVkGetDeviceProcAddrProxy
    )
    {
        return false;
    }


    if(!g_slVkCreateWin32SurfaceKHRProxy)
    {
        g_slVkCreateWin32SurfaceKHRProxy =
            reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
                g_slVkGetInstanceProcAddrProxy(
                    g_instance,
                    "vkCreateWin32SurfaceKHR"
                )
            );
    }


    if(!g_slVkDestroySurfaceKHRProxy)
    {
        g_slVkDestroySurfaceKHRProxy =
            reinterpret_cast<PFN_vkDestroySurfaceKHR>(
                g_slVkGetInstanceProcAddrProxy(
                    g_instance,
                    "vkDestroySurfaceKHR"
                )
            );
    }


    if(!g_slVkCreateSwapchainKHRProxy)
    {
        g_slVkCreateSwapchainKHRProxy =
            reinterpret_cast<PFN_vkCreateSwapchainKHR>(
                g_slVkGetDeviceProcAddrProxy(
                    g_device,
                    "vkCreateSwapchainKHR"
                )
            );
    }


    if(!g_slVkDestroySwapchainKHRProxy)
    {
        g_slVkDestroySwapchainKHRProxy =
            reinterpret_cast<PFN_vkDestroySwapchainKHR>(
                g_slVkGetDeviceProcAddrProxy(
                    g_device,
                    "vkDestroySwapchainKHR"
                )
            );
    }


    if(!g_slVkGetSwapchainImagesKHRProxy)
    {
        g_slVkGetSwapchainImagesKHRProxy =
            reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
                g_slVkGetDeviceProcAddrProxy(
                    g_device,
                    "vkGetSwapchainImagesKHR"
                )
            );
    }


    if(!g_slVkAcquireNextImageKHRProxy)
    {
        g_slVkAcquireNextImageKHRProxy =
            reinterpret_cast<PFN_vkAcquireNextImageKHR>(
                g_slVkGetDeviceProcAddrProxy(
                    g_device,
                    "vkAcquireNextImageKHR"
                )
            );
    }


    if(!g_slVkQueuePresentKHRProxy)
    {
        g_slVkQueuePresentKHRProxy =
            reinterpret_cast<PFN_vkQueuePresentKHR>(
                g_slVkGetDeviceProcAddrProxy(
                    g_device,
                    "vkQueuePresentKHR"
                )
            );
    }


    if(!g_slVkDeviceWaitIdleProxy)
    {
        g_slVkDeviceWaitIdleProxy =
            reinterpret_cast<PFN_vkDeviceWaitIdle>(
                g_slVkGetDeviceProcAddrProxy(
                    g_device,
                    "vkDeviceWaitIdle"
                )
            );
    }


    const bool ready =
        g_slVkCreateWin32SurfaceKHRProxy &&
        g_slVkDestroySurfaceKHRProxy &&
        g_slVkCreateSwapchainKHRProxy &&
        g_slVkDestroySwapchainKHRProxy &&
        g_slVkGetSwapchainImagesKHRProxy &&
        g_slVkAcquireNextImageKHRProxy &&
        g_slVkQueuePresentKHRProxy &&
        g_slVkDeviceWaitIdleProxy;


    static bool printed =
        false;


    if(!printed)
    {
        fprintf(
            stderr,
            "[FG-3 PROXY]"
            " ready=%d"
            " surfaceCreate=%d"
            " surfaceDestroy=%d"
            " swapCreate=%d"
            " swapDestroy=%d"
            " getImages=%d"
            " acquire=%d"
            " present=%d"
            " deviceWaitIdle=%d\n",

            ready ? 1 : 0,

            g_slVkCreateWin32SurfaceKHRProxy ? 1 : 0,
            g_slVkDestroySurfaceKHRProxy ? 1 : 0,

            g_slVkCreateSwapchainKHRProxy ? 1 : 0,
            g_slVkDestroySwapchainKHRProxy ? 1 : 0,

            g_slVkGetSwapchainImagesKHRProxy ? 1 : 0,
            g_slVkAcquireNextImageKHRProxy ? 1 : 0,
            g_slVkQueuePresentKHRProxy ? 1 : 0,
            g_slVkDeviceWaitIdleProxy ? 1 : 0
        );

        fflush(stderr);

        printed = true;
    }


    return ready;
}


VkResult FGDeviceWaitIdle(
    VkDevice device
)
{
    const bool useProxy =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies() &&
        g_slVkDeviceWaitIdleProxy;


    VkResult result;


    if(useProxy)
    {
        result =
            g_slVkDeviceWaitIdleProxy(
                device
            );
    }
    else
    {
        result =
            ::vkDeviceWaitIdle(
                device
            );
    }


    static bool printed =
        false;


    if(!printed)
    {
        fprintf(
            stderr,
            "[FG-3 DEVICE WAIT IDLE]"
            " proxy=%d"
            " result=%d\n",

            useProxy ? 1 : 0,
            static_cast<int>(result)
        );

        fflush(stderr);

        printed = true;
    }


    return result;
}
void StreamlineLogCallback(sl::LogType type,const char* msg){
    const char* s="UNKNOWN";
    if(type==sl::LogType::eInfo)s="INFO";else if(type==sl::LogType::eWarn)s="WARN";else if(type==sl::LogType::eError)s="ERROR";
    std::cerr
    << "[Streamline "
    << s
    << "] "
    << (msg ? msg : "")
    << std::endl;

    if(type != sl::LogType::eInfo)
    {
        std::ostringstream slLog;

        slLog
            << "[SL " << s << "] "
            << (msg ? msg : "");

        RTLogFlushed(slLog.str());
    }
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_probeHDRSurface(
    JNIEnv*,
    jclass,
    jlong surfaceHandle)
{
    if(
        g_physicalDevice == VK_NULL_HANDLE ||
        surfaceHandle == 0
    )
    {
        fprintf(
            stderr,
            "[HDR-2.2] INVALID HANDLE"
            " physicalDevice=%p"
            " surface=0x%llx\n",
            (void*)g_physicalDevice,
            static_cast<unsigned long long>(
                surfaceHandle
            )
        );

        fflush(stderr);

        return JNI_FALSE;
    }


    VkSurfaceKHR surface =
        reinterpret_cast<VkSurfaceKHR>(
            static_cast<uintptr_t>(
                surfaceHandle
            )
        );


    uint32_t formatCount =
        0;


    VkResult result =
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            g_physicalDevice,
            surface,
            &formatCount,
            nullptr
        );


    fprintf(
        stderr,
        "[HDR-2.2]"
        " firstQuery=%d"
        " formatCount=%u\n",
        static_cast<int>(
            result
        ),
        formatCount
    );


    if(
        result != VK_SUCCESS ||
        formatCount == 0
    )
    {
        fflush(stderr);

        return JNI_FALSE;
    }


    std::vector<VkSurfaceFormatKHR>
        formats(
            formatCount
        );


    result =
        vkGetPhysicalDeviceSurfaceFormatsKHR(
            g_physicalDevice,
            surface,
            &formatCount,
            formats.data()
        );


    fprintf(
        stderr,
        "========== [HDR-2.2 SURFACE FORMATS BEGIN] ==========\n"
    );


    bool hdr10Found =
        false;


    for(
        uint32_t i = 0;
        i < formatCount;
        ++i
    )
    {
        const VkFormat format =
            formats[i].format;


        const VkColorSpaceKHR colorSpace =
            formats[i].colorSpace;


        const bool isHDR10 =
            colorSpace ==
            VK_COLOR_SPACE_HDR10_ST2084_EXT;


        if(isHDR10)
        {
            hdr10Found =
                true;
        }


        fprintf(
            stderr,
            "[HDR-2.2 FORMAT %u]"
            " VkFormat=%d"
            " ColorSpace=%d"
            " HDR10_ST2084=%d\n",
            i,
            static_cast<int>(
                format
            ),
            static_cast<int>(
                colorSpace
            ),
            isHDR10
                ? 1
                : 0
        );
    }


    fprintf(
        stderr,
        "[HDR-2.2 RESULT]"
        " HDR10_ST2084_SUPPORTED=%d\n",
        hdr10Found
            ? 1
            : 0
    );


    g_hdrSurfaceSupported.store(hdr10Found);


    fprintf(
        stderr,
        "========== [HDR-2.2 SURFACE FORMATS END] ==========\n"
    );


    fflush(stderr);


    return result == VK_SUCCESS
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_setStreamlineQueuePlan(
    JNIEnv*,
    jclass,
    jint graphicsFamily,
    jint graphicsStartIndex,
    jint computeFamily,
    jint computeStartIndex,
    jint opticalFlowFamily,
    jint opticalFlowStartIndex,
    jboolean useNativeOpticalFlow
)
{
    g_slQueuePlanReady =
        false;


    if(
        graphicsFamily < 0 ||
        graphicsStartIndex < 0 ||
        computeFamily < 0 ||
        computeStartIndex < 0
    )
    {
        fprintf(
            stderr,
            "[FG-2.11 QUEUE BRIDGE]"
            " INVALID"
            " G=%d:%d"
            " C=%d:%d\n",
            graphicsFamily,
            graphicsStartIndex,
            computeFamily,
            computeStartIndex
        );

        fflush(stderr);
        return;
    }


    const bool nativeOF =
        useNativeOpticalFlow == JNI_TRUE;


    if(
        nativeOF &&
        (
            opticalFlowFamily < 0 ||
            opticalFlowStartIndex < 0
        )
    )
    {
        fprintf(
            stderr,
            "[FG-2.11 QUEUE BRIDGE]"
            " INVALID_NATIVE_OF"
            " OF=%d:%d\n",
            opticalFlowFamily,
            opticalFlowStartIndex
        );

        fflush(stderr);
        return;
    }


    g_slGraphicsQueueFamily =
        static_cast<uint32_t>(
            graphicsFamily
        );

    g_slGraphicsQueueIndex =
        static_cast<uint32_t>(
            graphicsStartIndex
        );


    g_slComputeQueueFamily =
        static_cast<uint32_t>(
            computeFamily
        );

    g_slComputeQueueIndex =
        static_cast<uint32_t>(
            computeStartIndex
        );


    g_slUseNativeOpticalFlow =
        nativeOF;


    if(nativeOF)
    {
        g_slOpticalFlowQueueFamily =
            static_cast<uint32_t>(
                opticalFlowFamily
            );

        g_slOpticalFlowQueueIndex =
            static_cast<uint32_t>(
                opticalFlowStartIndex
            );
    }
    else
    {
        g_slOpticalFlowQueueFamily =
            UINT32_MAX;

        g_slOpticalFlowQueueIndex =
            UINT32_MAX;
    }


    g_slQueuePlanReady =
        true;


    fprintf(
        stderr,
        "[FG-2.11 QUEUE BRIDGE]"
        " ready=1"
        " G=%u:%u"
        " C=%u:%u"
        " OF=%u:%u"
        " nativeOF=%d"
        " requiredG=%u"
        " requiredC=%u"
        " requiredOF=%u\n",

        g_slGraphicsQueueFamily,
        g_slGraphicsQueueIndex,

        g_slComputeQueueFamily,
        g_slComputeQueueIndex,

        g_slOpticalFlowQueueFamily,
        g_slOpticalFlowQueueIndex,

        g_slUseNativeOpticalFlow
            ? 1
            : 0,

        g_fgGraphicsQueuesRequired,
        g_fgComputeQueuesRequired,
        g_fgOpticalFlowQueuesRequired
    );

    fflush(stderr);
}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_clearWorldGeometry(JNIEnv* env, jclass clazz)
{
    {
        std::lock_guard<std::timed_mutex>
            lock(
                g_chunkMutex
            );

        g_chunkMeshes.clear();
        g_chunkMotion.clear();
        g_chunkRawPayloads.clear();
        g_chunkUploadDigests.clear();
        g_cutoutOmmInputs.clear();
        g_voxyLodSources.clear();
        g_voxyLodSpatialOwners.clear();

        clearDhLodSpatialTablesLocked();

        g_dirtySectionKeys.clear();

        g_lastLocalPlayerASUpdateFrame = UINT64_MAX;
        g_localPlayerASUpdatePending = false;
        g_lastDynamicRTASUpdateFrame = UINT64_MAX;
        g_dynamicRTASUpdatePending = false;

        g_forceSectionASReset =
            true;

        markWorldGeometryDirty();
    }


    {
        std::lock_guard<std::mutex>
            lightLock(
                g_lightMutex
            );

        g_lightSections.clear();

        g_totalLocalLights =
            0;
    }


    g_localLightsDirty =
        true;


    clearVLISections();


    RTLog(
        "[C++] Global World Geometry + RT Local Lights + VLI Cleared."
    );
}
// =====================================================
// Frame generation base frame-rate cap
// =====================================================
//
// Refresh rate of the display the desktop currently runs on.  This game is
// borderless fullscreen, so the current desktop video mode is the display's
// real refresh rate.
uint32_t QueryDisplayRefreshHz()
{
    DEVMODEW mode{};

    mode.dmSize =
        sizeof(mode);


    if(
        !EnumDisplaySettingsW(
            nullptr,
            ENUM_CURRENT_SETTINGS,
            &mode
        ) ||
        mode.dmDisplayFrequency <= 1
    )
    {
        return 0;
    }


    return static_cast<uint32_t>(
        mode.dmDisplayFrequency
    );
}


// Resolves the request into the Hz handed to Reflex.  Automatic pairing is
// refresh / multiplier: with N frames presented per rendered one, rendering
// faster than that only produces frames the pacer has to discard, while the
// latency of the extra rendering is kept.
uint32_t ResolveFgBaseFrameLimitHz()
{
    if(g_fgFullscreenMenuOpen.load())
    {
        return 0;
    }


    const int request =
        g_fgBaseFrameLimitRequest;


    if(request == 0)
    {
        return 0;
    }


    if(request > 0)
    {
        return static_cast<uint32_t>(
            request
        );
    }


    const uint32_t refresh =
        QueryDisplayRefreshHz();


    if(refresh == 0)
    {
        return 0;
    }


    const int requestedMultiplier =
        g_fgMultiplierRequested.load();


    const uint32_t multiplier =
        requestedMultiplier >= 2
            ? static_cast<uint32_t>(
                  requestedMultiplier
              )
            : 1u;


    const uint32_t hz =
        refresh / multiplier;


    return hz == 0
        ? 1u
        : hz;
}


bool ApplyReflexMode(
    int mode)
{
    std::lock_guard<std::mutex>
        lock(g_reflexMutex);


    if(!g_reflexSupported ||
       !g_slReflexSetOptions)
    {
        return false;
    }


    sl::ReflexMode reflexMode;


    switch(mode)
    {
        case 0:
        {
            reflexMode =
                sl::ReflexMode::eOff;

            break;
        }

        case 1:
        {
            reflexMode =
                sl::ReflexMode::eLowLatency;

            break;
        }

        case 2:
        {
            reflexMode =
                sl::ReflexMode::eLowLatencyWithBoost;

            break;
        }

        default:
        {
            return false;
        }
    }


    sl::ReflexOptions options{};

    options.mode =
        reflexMode;

    const uint32_t baseLimitHz =
        ResolveFgBaseFrameLimitHz();

    options.frameLimitUs =
        baseLimitHz == 0
            ? 0u
            : static_cast<uint32_t>(
                  1000000.0 /
                  static_cast<double>(
                      baseLimitHz
                  )
              );

    options.useMarkersToOptimize =
        true;

    options.virtualKey =
        0u;

    g_fgBaseFrameLimitHz.store(
        baseLimitHz
    );


    sl::Result result =
        g_slReflexSetOptions(
            options
        );


    if(result != sl::Result::eOk)
    {
        fprintf(
            stderr,
            "[REFLEX] SetOptions FAILED mode=%d result=%d\n",
            mode,
            static_cast<int>(result)
        );
        {
            std::ostringstream reflexFailLog;

            reflexFailLog
                << "[REFLEX] SetOptions FAILED"
                << " mode=" << mode
                << " result=" << static_cast<int>(result);

            RTLogFlushed(reflexFailLog.str());
        }

        return false;
    }


    g_reflexMode =
        mode;


    fprintf(
        stderr,
        "[REFLEX] mode applied=%d baseLimitHz=%u frameLimitUs=%u\n",
        mode,
        baseLimitHz,
        options.frameLimitUs
    );

    fflush(stderr);


    {
        std::ostringstream reflexLog;

        reflexLog
            << "[REFLEX]"
            << " mode=" << mode
            << " baseLimitHz=" << baseLimitHz
            << " frameLimitUs=" << options.frameLimitUs
            << " request=" << g_fgBaseFrameLimitRequest;

        RTLog(reflexLog.str());
    }


    return true;
}


// Recomputes and re-pushes the cap.  Called whenever the inputs it depends on
// change - the multiplier, or the request itself - because Reflex only sees the
// value that was sent with the last slReflexSetOptions call.
void RefreshFgBaseFrameLimit()
{
    const uint32_t hz =
        ResolveFgBaseFrameLimitHz();


    g_fgBaseFrameLimitHz.store(
        hz
    );


    if(g_reflexMode == 0)
    {
        return;
    }


    ApplyReflexMode(
        g_reflexMode
    );
}
bool SetCurrentPCLMarker(
    sl::PCLMarker marker)
{
    if(!g_pclSupported ||
       !g_slPCLSetMarker ||
       !g_currentSLFrameValid ||
       g_currentSLFrameToken == nullptr)
    {
        return false;
    }


    sl::Result result =
        g_slPCLSetMarker(
            marker,
            *g_currentSLFrameToken
        );


    if(result != sl::Result::eOk)
    {
        fprintf(
            stderr,
            "[PCL MARKER] FAILED"
            " marker=%u"
            " frame=%u"
            " result=%d\n",
            static_cast<unsigned int>(
                marker
            ),
            g_currentSLFrameIndex,
            static_cast<int>(
                result
            )
        );


        return false;
    }


    return true;
}


bool MarkRenderSubmitStart()
{
    if(!g_currentSLFrameValid ||
       g_currentSLFrameToken == nullptr)
    {
        return false;
    }


    if(g_lastRenderSubmitStartFrame ==
       g_currentSLFrameIndex)
    {
        return true;
    }


    if(!SetCurrentPCLMarker(
        sl::PCLMarker::eRenderSubmitStart
    ))
    {
        return false;
    }


    g_lastRenderSubmitStartFrame =
        g_currentSLFrameIndex;


    static bool printed =
        false;


    if(!printed)
    {
        fprintf(
            stderr,
            "[PCL] RenderSubmitStart ACTIVE frame=%u\n",
            g_currentSLFrameIndex
        );

        printed =
            true;
    }


    return true;
}


bool MarkRenderSubmitEnd()
{
    if(!g_currentSLFrameValid ||
       g_currentSLFrameToken == nullptr)
    {
        return false;
    }


    if(g_lastRenderSubmitStartFrame !=
       g_currentSLFrameIndex)
    {
        return false;
    }


    if(g_lastRenderSubmitEndFrame ==
       g_currentSLFrameIndex)
    {
        return true;
    }


    if(!SetCurrentPCLMarker(
        sl::PCLMarker::eRenderSubmitEnd
    ))
    {
        return false;
    }


    g_lastRenderSubmitEndFrame =
        g_currentSLFrameIndex;


    static bool printed =
        false;


    if(!printed)
    {
        fprintf(
            stderr,
            "[PCL] RenderSubmitEnd ACTIVE frame=%u\n",
            g_currentSLFrameIndex
        );

        printed =
            true;
    }


    return true;
}
// =====================================================
// 2026-10-22 显存口径采样：三个 feature 的 estimatedVRAMUsageInBytes
// 120 帧一次，零分配，放在 present 路径（DLSS-G 的 GetState 要求呈现线程）。
// =====================================================
void SampleSlEstimatedVram()
{
    static uint32_t sampleFrameCounter = 0u;

    if((sampleFrameCounter++ % 120u) != 0u)
    {
        return;
    }

    if(g_dlssReady && g_slDLSSGetState)
    {
        sl::DLSSState dlssState{};

        if(g_slDLSSGetState(g_dlssViewport, dlssState) == sl::Result::eOk)
        {
            g_slVramDlssBytes.store(dlssState.estimatedVRAMUsageInBytes);
        }
    }

    if(g_dlssRRReady && g_slDLSSDGetState)
    {
        sl::DLSSDState dlssdState{};

        if(g_slDLSSDGetState(g_dlssViewport, dlssdState) == sl::Result::eOk)
        {
            g_slVramDlssdBytes.store(dlssdState.estimatedVRAMUsageInBytes);
        }
    }

    if(g_dlssGReady && g_slDLSSGGetState)
    {
        sl::DLSSGState dlssgState{};

        if(g_slDLSSGGetState(g_dlssViewport, dlssgState, nullptr) == sl::Result::eOk)
        {
            g_slVramDlssgBytes.store(dlssgState.estimatedVRAMUsageInBytes);
        }
    }
}


bool MarkPresentStart()
{
    SampleSlEstimatedVram();

    if(!g_currentSLFrameValid ||
       g_currentSLFrameToken == nullptr)
    {
        return false;
    }


    if(g_lastRenderSubmitEndFrame !=
       g_currentSLFrameIndex)
    {
        return false;
    }


    if(g_lastPresentStartFrame ==
       g_currentSLFrameIndex)
    {
        return true;
    }


    if(!SetCurrentPCLMarker(
        sl::PCLMarker::ePresentStart
    ))
    {
        return false;
    }


    g_lastPresentStartFrame =
        g_currentSLFrameIndex;


    static bool printed =
        false;


    if(!printed)
    {
        fprintf(
            stderr,
            "[PCL] PresentStart ACTIVE frame=%u\n",
            g_currentSLFrameIndex
        );

        printed =
            true;
    }


    return true;
}


bool MarkPresentEnd()
{
    if(!g_currentSLFrameValid ||
       g_currentSLFrameToken == nullptr)
    {
        return false;
    }


    if(g_lastPresentStartFrame !=
       g_currentSLFrameIndex)
    {
        return false;
    }


    if(g_lastPresentEndFrame ==
       g_currentSLFrameIndex)
    {
        return true;
    }


    if(!SetCurrentPCLMarker(
        sl::PCLMarker::ePresentEnd
    ))
    {
        return false;
    }


    g_lastPresentEndFrame =
        g_currentSLFrameIndex;


    static bool printed =
        false;


    if(!printed)
    {
        fprintf(
            stderr,
            "[PCL] PresentEnd ACTIVE frame=%u\n",
            g_currentSLFrameIndex
        );

        printed =
            true;
    }


    return true;
}


bool BeginStreamlineFrame()
{
    if(!g_slInitialized ||
       !g_reflexSupported ||
       !g_slGetNewFrameToken ||
       !g_slReflexSleep)
    {
        g_currentSLFrameToken =
            nullptr;

        g_currentSLFrameValid =
            false;

        return false;
    }


    const uint32_t frameIndex =
        g_nextSLFrameIndex++;


    sl::FrameToken* frameToken =
        nullptr;


    sl::Result tokenResult =
        g_slGetNewFrameToken(
            frameToken,
            &frameIndex
        );


    if(tokenResult != sl::Result::eOk ||
       frameToken == nullptr)
    {
        g_currentSLFrameToken =
            nullptr;

        g_currentSLFrameValid =
            false;


        fprintf(
            stderr,
            "[REFLEX FRAME] GetFrameToken FAILED"
            " frame=%u result=%d\n",
            frameIndex,
            static_cast<int>(
                tokenResult
            )
        );


        return false;
    }


    g_currentSLFrameToken =
        frameToken;

    g_currentSLFrameIndex =
        frameIndex;

    g_currentSLFrameValid =
        true;


    sl::Result sleepResult =
        g_slReflexSleep(
            *g_currentSLFrameToken
        );


    if(sleepResult != sl::Result::eOk)
    {
        fprintf(
            stderr,
            "[REFLEX SLEEP] FAILED"
            " frame=%u result=%d\n",
            g_currentSLFrameIndex,
            static_cast<int>(
                sleepResult
            )
        );
    }
    else
    {
        static bool printedSleepSuccess =
            false;


        if(!printedSleepSuccess)
        {
            fprintf(
                stderr,
                "[REFLEX SLEEP] ACTIVE"
                " frame=%u result=0\n",
                g_currentSLFrameIndex
            );


            printedSleepSuccess =
                true;
        }
    }


    MaybeReleaseFgResourcesOnIdle();


    return true;
}
void initStreamlineInternal()
{
    if(g_device == VK_NULL_HANDLE)
    {
        fprintf(
            stderr,
            "[SL] No Vulkan device yet, skip\n"
        );

        fflush(stderr);
        return;
    }


    if(g_slDeviceConfigured)
    {
        fprintf(
            stderr,
            "[FG-3 EARLY VK]"
            " alreadyConfigured=1\n"
        );

        fflush(stderr);
        return;
    }


    fprintf(
        stderr,
        "[CPP] initStreamlineInternal ENTER\n"
    );
    fprintf(stderr,"[POINT 0] g_device=%p\n",(void*)g_device);
    VkDevice savedDevice = g_device;
    VkInstance savedInstance = g_instance;
    VkPhysicalDevice savedPhysical = g_physicalDevice;
    VkQueue savedQueue = g_queue;
    uint32_t savedQueueFamily = g_queueFamily;

    fprintf(stderr,
    "[SAVE BEFORE DLL] addr=%p value=%p\n",
    (void*)&g_device,
    (void*)g_device);

    fflush(stderr);

    if(!g_slInitialized || !g_slModule)
    {
        fprintf(
            stderr,
            "[SL] ERROR: Streamline was not initialized early before VkDevice creation\n"
        );

        fflush(stderr);
        return;
    }


    fprintf(
        stderr,
        "[SL] Early initialization confirmed, now configuring Vulkan device\n"
    );

    fflush(stderr);


    g_slInit=reinterpret_cast<PFN_slInit>(
        GetProcAddress(g_slModule,"slInit")
    );

    g_slShutdown=reinterpret_cast<PFN_slShutdown>(
        GetProcAddress(g_slModule,"slShutdown")
    );

    g_slSetVulkanInfo=reinterpret_cast<PFN_slSetVulkanInfo>(
        GetProcAddress(g_slModule,"slSetVulkanInfo")
    );

    g_slIsFeatureSupported=reinterpret_cast<PFN_slIsFeatureSupported>(
        GetProcAddress(g_slModule,"slIsFeatureSupported")
    );

    g_slSetFeatureLoaded=reinterpret_cast<PFN_slSetFeatureLoaded>(
        GetProcAddress(g_slModule,"slSetFeatureLoaded")
    );
    g_slGetFeatureFunction =
    reinterpret_cast<PFN_slGetFeatureFunction>(
        GetProcAddress(
            g_slModule,
            "slGetFeatureFunction"
        )
    );


    fprintf(stderr,
    "[SL FUNC CHECK] slGetFeatureFunction=%p\n",
    (void*)g_slGetFeatureFunction);

    fflush(stderr);

    g_slSetConstants =
reinterpret_cast<PFN_slSetConstants>(
    GetProcAddress(
        g_slModule,
        "slSetConstants"
    )
);
    g_slEvaluateFeature =
reinterpret_cast<PFN_slEvaluateFeature>(
    GetProcAddress(
        g_slModule,
        "slEvaluateFeature")
        );
    g_slGetNewFrameToken =
    reinterpret_cast<PFN_slGetNewFrameToken>(
        GetProcAddress(
            g_slModule,
            "slGetNewFrameToken"
        )
    );
    g_slSetTagForFrame =
    reinterpret_cast<PFN_slSetTagForFrame>(
        GetProcAddress(
            g_slModule,
            "slSetTagForFrame"
        )
    );
    g_slFreeResources =
    reinterpret_cast<PFN_slFreeResources>(
        GetProcAddress(
            g_slModule,
            "slFreeResources"
        )
    );



#define CHECK_SL_FUNC(x)\
if(!(x))\
{\
    std::cerr<<"SL MISSING FUNCTION: "<<#x<<std::endl;\
}


    CHECK_SL_FUNC(g_slInit);
    CHECK_SL_FUNC(g_slShutdown);
    CHECK_SL_FUNC(g_slSetVulkanInfo);
    CHECK_SL_FUNC(g_slSetFeatureLoaded);
    CHECK_SL_FUNC(g_slSetConstants);
    CHECK_SL_FUNC(g_slEvaluateFeature);
    CHECK_SL_FUNC(g_slGetNewFrameToken);
    CHECK_SL_FUNC(g_slSetTagForFrame);
    fprintf(
    stderr,
    "[FG-4.2 TAG API]"
    " slSetTagForFrame=%p\n",
    reinterpret_cast<void*>(
        g_slSetTagForFrame
    )
);

    fflush(stderr);

    std::cerr
<< "[SL FUNC]"
<< "\n slInit="
<< g_slInit
<< "\n slShutdown="
<< g_slShutdown
<< "\n slSetVulkanInfo="
<< g_slSetVulkanInfo
<< "\n slIsFeatureSupported="
<< g_slIsFeatureSupported
    << "\n slSetFeatureLoaded="
    << g_slSetFeatureLoaded
    << "\n slSetConstants="
    << g_slSetConstants
    << "\n slDLSSSetOptions="
    << g_slDLSSSetOptions
    << "\n slEvaluateFeature="
<< g_slGetNewFrameToken
<< std::endl;

    fprintf(stderr,
"[AFTER GetProcAddress] g_device=%p\n",
(void*)g_device);

    fflush(stderr);


    if(!g_slInit)
    {


        if(!g_slSetConstants)
        {
            std::cerr
            << "[SL] slSetConstants missing"
            << std::endl;
        }
        std::cerr
        << "[Native C++] Failed to extract slInit."
        << std::endl;
        return;
    }
    if(!g_slEvaluateFeature)
    {
        fprintf(stderr,
        "[SL] slEvaluateFeature missing\n");
        fflush(stderr);
        return;
    }

    sl::Result r =
        sl::Result::eOk;


    std::cout
        << "[SL] slInit already completed during early initialization"
        << std::endl;


if(!g_slQueuePlanReady)
{
    fprintf(
        stderr,
        "[FG-2.12 SL VKINFO]"
        " queuePlanReady=0"
        " ABORT\n"
    );

    fflush(stderr);
    return;
}


sl::VulkanInfo vkInfo{};

vkInfo.device =
    savedDevice;

vkInfo.instance =
    savedInstance;

vkInfo.physicalDevice =
    savedPhysical;



vkInfo.graphicsQueueFamily =
    g_slGraphicsQueueFamily;

vkInfo.graphicsQueueIndex =
    g_slGraphicsQueueIndex;



vkInfo.computeQueueFamily =
    g_slComputeQueueFamily;

vkInfo.computeQueueIndex =
    g_slComputeQueueIndex;



vkInfo.useNativeOpticalFlowMode =
    g_slUseNativeOpticalFlow;


if(g_slUseNativeOpticalFlow)
{
    vkInfo.opticalFlowQueueFamily =
        g_slOpticalFlowQueueFamily;

    vkInfo.opticalFlowQueueIndex =
        g_slOpticalFlowQueueIndex;
}


vkInfo.graphicsQueueCreateFlags =
    0;

vkInfo.computeQueueCreateFlags =
    0;

vkInfo.opticalFlowQueueCreateFlags =
    0;


fprintf(
    stderr,
    "[FG-2.12 SL VKINFO]"
    " ready=1"
    " G=%u:%u"
    " C=%u:%u"
    " OF=%u:%u"
    " nativeOF=%d"
    " flags=0/0/0\n",

    vkInfo.graphicsQueueFamily,
    vkInfo.graphicsQueueIndex,

    vkInfo.computeQueueFamily,
    vkInfo.computeQueueIndex,

    vkInfo.opticalFlowQueueFamily,
    vkInfo.opticalFlowQueueIndex,

    vkInfo.useNativeOpticalFlowMode
        ? 1
        : 0
);

fflush(stderr);


sl::Result vkResult =
    g_slSetVulkanInfo(
        vkInfo
    );


fprintf(
    stderr,
    "[FG-2.12 SL SET VULKAN INFO]"
    " result=%d\n",
    static_cast<int>(
        vkResult
    )
);

fflush(stderr);



    std::cout << "[SL] slSetVulkanInfo result=" << (int)vkResult << std::endl;
    if (vkResult != sl::Result::eOk)
    {
        return;
    }
    sl::AdapterInfo ai{};

    ai.vkPhysicalDevice =
        g_physicalDevice;


    sl::Result dlssSupport =
        g_slIsFeatureSupported(
            sl::kFeatureDLSS,
            ai
        );


    sl::Result dlssRRSupport =
        g_slIsFeatureSupported(
            sl::kFeatureDLSS_RR,
            ai
        );


    sl::Result reflexSupport =
        g_slIsFeatureSupported(
            sl::kFeatureReflex,
            ai
        );
    sl::Result dlssGSupport =
    g_slIsFeatureSupported(
        sl::kFeatureDLSS_G,
        ai
    );


    sl::Result pclSupport =
        g_slIsFeatureSupported(
            sl::kFeaturePCL,
            ai
        );


    g_dlssRRSupported =
        dlssRRSupport ==
        sl::Result::eOk;
    g_dlssGSupported =
    dlssGSupport ==
    sl::Result::eOk;

    fprintf(
    stderr,
    "[FG-1 SUPPORT]"
    " result=%d"
    " supported=%d\n",

    static_cast<int>(
        dlssGSupport
    ),

    g_dlssGSupported
        ? 1
        : 0
);

    fflush(stderr);

    {
        std::ostringstream fgSupportLog;

        fgSupportLog
            << "[FG-1 SUPPORT]"
            << " result=" << static_cast<int>(dlssGSupport)
            << " supported=" << (g_dlssGSupported ? 1 : 0);

        RTLog(fgSupportLog.str());
    }

    g_reflexSupported =
        reflexSupport == sl::Result::eOk;

    g_pclSupported =
        pclSupport == sl::Result::eOk;

    if(g_reflexSupported)
    {
        void* reflexSetOptionsFn =
            nullptr;

        sl::Result setOptionsFnResult =
            g_slGetFeatureFunction(
                sl::kFeatureReflex,
                "slReflexSetOptions",
                reflexSetOptionsFn
            );


        g_slReflexSetOptions =
            reinterpret_cast<PFN_slReflexSetOptions>(
                reflexSetOptionsFn
            );


        void* reflexGetStateFn =
            nullptr;

        sl::Result getStateFnResult =
            g_slGetFeatureFunction(
                sl::kFeatureReflex,
                "slReflexGetState",
                reflexGetStateFn
            );


        g_slReflexGetState =
            reinterpret_cast<PFN_slReflexGetState>(
                reflexGetStateFn
            );
        void* reflexSleepFn =
    nullptr;

        sl::Result sleepFnResult =
            g_slGetFeatureFunction(
                sl::kFeatureReflex,
                "slReflexSleep",
                reflexSleepFn
            );


        g_slReflexSleep =
            reinterpret_cast<PFN_slReflexSleep>(
                reflexSleepFn
            );


        fprintf(
            stderr,
            "[REFLEX FUNC]"
            " SetOptions result=%d ptr=%p"
            " GetState result=%d ptr=%p"
            " Sleep result=%d ptr=%p\n",

            static_cast<int>(
                setOptionsFnResult
            ),

            reinterpret_cast<void*>(
                g_slReflexSetOptions
            ),

            static_cast<int>(
                getStateFnResult
            ),

            reinterpret_cast<void*>(
                g_slReflexGetState
            ),

            static_cast<int>(
                sleepFnResult
            ),

            reinterpret_cast<void*>(
                g_slReflexSleep
            )
        );;
        {
            std::ostringstream reflexFuncLog;

            reflexFuncLog
                << "[REFLEX FUNC]"
                << " SetOptions result=" << static_cast<int>(setOptionsFnResult)
                << " ptr=" << reinterpret_cast<void*>(g_slReflexSetOptions)
                << " GetState result=" << static_cast<int>(getStateFnResult)
                << " ptr=" << reinterpret_cast<void*>(g_slReflexGetState)
                << " Sleep result=" << static_cast<int>(sleepFnResult)
                << " ptr=" << reinterpret_cast<void*>(g_slReflexSleep);

            RTLog(reflexFuncLog.str());
        }
    }
    if(g_reflexSupported &&
   g_slReflexGetState)
    {
        sl::ReflexState state{};


        sl::Result stateResult =
            g_slReflexGetState(
                state
            );


        if(stateResult == sl::Result::eOk)
        {
            g_reflexLowLatencyAvailable =
                state.lowLatencyAvailable;


            fprintf(
                stderr,
                "[REFLEX STATE] result=%d lowLatencyAvailable=%d latencyReportAvailable=%d\n",
                static_cast<int>(stateResult),
                state.lowLatencyAvailable ? 1 : 0,
                state.latencyReportAvailable ? 1 : 0
            );
        }
        else
        {
            g_reflexLowLatencyAvailable =
                false;


            fprintf(
                stderr,
                "[REFLEX STATE] FAILED result=%d\n",
                static_cast<int>(stateResult)
            );
        }
    }


    if(g_reflexLowLatencyAvailable)
    {
        ApplyReflexMode(
            1
        );
    }
    else if(g_reflexSupported)
    {
        ApplyReflexMode(
            0
        );
    }


    if(g_pclSupported &&
       g_slGetFeatureFunction)
    {
        void* pclSetMarkerFn =
            nullptr;


        sl::Result pclSetMarkerFnResult =
            g_slGetFeatureFunction(
                sl::kFeaturePCL,
                "slPCLSetMarker",
                pclSetMarkerFn
            );


        g_slPCLSetMarker =
            reinterpret_cast<PFN_slPCLSetMarker>(
                pclSetMarkerFn
            );


        fprintf(
            stderr,
            "[PCL FUNC] SetMarker result=%d ptr=%p\n",
            static_cast<int>(
                pclSetMarkerFnResult
            ),
            reinterpret_cast<void*>(
                g_slPCLSetMarker
            )
        );
    }
    fprintf(
        stderr,
        "[SL SUPPORT]"
        " DLSS=%d"
        " DLSS_RR=%d"
        " Reflex=%d"
        " PCL=%d\n",

        static_cast<int>(
            dlssSupport
        ),

        static_cast<int>(
            dlssRRSupport
        ),

        static_cast<int>(
            reflexSupport
        ),

        static_cast<int>(
            pclSupport
        )
    );
    {
        std::ostringstream slSupportLog;

        slSupportLog
            << "[SL SUPPORT]"
            << " DLSS=" << static_cast<int>(dlssSupport)
            << " DLSS_RR=" << static_cast<int>(dlssRRSupport)
            << " Reflex=" << static_cast<int>(reflexSupport)
            << " PCL=" << static_cast<int>(pclSupport);

        RTLog(slSupportLog.str());
    }

    sl::Result dlssLoad =
        sl::Result::eErrorFeatureNotSupported;


    if(dlssSupport == sl::Result::eOk &&
       kRequestDlssSrFeature)
    {
        dlssLoad =
            g_slSetFeatureLoaded(
                sl::kFeatureDLSS,
                true
            );

        fprintf(
            stderr,
            "[SL] DLSS loaded result=%d\n",
            static_cast<int>(dlssLoad)
        );
    }
    if (dlssLoad == sl::Result::eOk)
    {
        void* dlssSetOptionsFn = nullptr;

        sl::Result featureFnResult =
            g_slGetFeatureFunction(
                sl::kFeatureDLSS,
                "slDLSSSetOptions",
                dlssSetOptionsFn
            );

        g_slDLSSSetOptions =
            reinterpret_cast<PFN_slDLSSSetOptions>(
                dlssSetOptionsFn
            );
        void* dlssOptimalFn = nullptr;

        sl::Result optimalFnResult =
            g_slGetFeatureFunction(
                sl::kFeatureDLSS,
                "slDLSSGetOptimalSettings",
                dlssOptimalFn
            );

        g_slDLSSGetOptimalSettings =
            reinterpret_cast<PFN_slDLSSGetOptimalSettings>(
                dlssOptimalFn
            );

        void* dlssGetStateFn = nullptr;

        g_slGetFeatureFunction(
            sl::kFeatureDLSS,
            "slDLSSGetState",
            dlssGetStateFn
        );

        g_slDLSSGetState =
            reinterpret_cast<PFN_slDLSSGetState>(
                dlssGetStateFn
            );

        std::cerr
            << "[SL] GetFeatureFunction slDLSSGetOptimalSettings result="
            << std::dec
            << (int)optimalFnResult
            << " ptr="
            << g_slDLSSGetOptimalSettings
            << std::endl;

        std::cerr
            << "[SL] GetFeatureFunction slDLSSSetOptions result="
            << (int)featureFnResult
            << " ptr="
            << g_slDLSSSetOptions
            << std::endl;


        if(featureFnResult == sl::Result::eOk &&
           g_slDLSSSetOptions != nullptr)
        {
            g_dlssReady = true;

            std::cout
                << "[DLSS] READY = TRUE"
                << std::endl;
        }
        else
        {
            g_dlssReady = false;

            std::cerr
                << "[DLSS] READY = FALSE - slDLSSSetOptions unavailable"
                << std::endl;
        }
    }
    else
    {
        g_dlssReady = false;

        std::cout
            << "[DLSS] READY = FALSE"
            << std::endl;
    }

sl::Result dlssRRLoad =
    sl::Result::eErrorFeatureNotSupported;


if(dlssRRSupport == sl::Result::eOk)
{
    dlssRRLoad =
        g_slSetFeatureLoaded(
            sl::kFeatureDLSS_RR,
            true
        );


    fprintf(
        stderr,
        "[SL RR] DLSS_RR loaded result=%d\n",
        static_cast<int>(
            dlssRRLoad
        )
    );
}


if(dlssRRLoad == sl::Result::eOk)
{
    void* dlssdSetOptionsFn =
        nullptr;


    sl::Result rrSetOptionsFnResult =
        g_slGetFeatureFunction(
            sl::kFeatureDLSS_RR,
            "slDLSSDSetOptions",
            dlssdSetOptionsFn
        );


    g_slDLSSDSetOptions =
        reinterpret_cast<
            PFN_slDLSSDSetOptions
        >(
            dlssdSetOptionsFn
        );


    void* dlssdOptimalFn =
        nullptr;


    sl::Result rrOptimalFnResult =
        g_slGetFeatureFunction(
            sl::kFeatureDLSS_RR,
            "slDLSSDGetOptimalSettings",
            dlssdOptimalFn
        );


    g_slDLSSDGetOptimalSettings =
        reinterpret_cast<
            PFN_slDLSSDGetOptimalSettings
        >(
            dlssdOptimalFn
        );


    void* dlssdGetStateFn =
        nullptr;


    g_slGetFeatureFunction(
        sl::kFeatureDLSS_RR,
        "slDLSSDGetState",
        dlssdGetStateFn
    );


    g_slDLSSDGetState =
        reinterpret_cast<
            PFN_slDLSSDGetState
        >(
            dlssdGetStateFn
        );


    fprintf(
        stderr,
        "[SL RR FUNC]"
        " slDLSSDSetOptions"
        " result=%d ptr=%p\n",

        static_cast<int>(
            rrSetOptionsFnResult
        ),

        reinterpret_cast<void*>(
            g_slDLSSDSetOptions
        )
    );


    fprintf(
        stderr,
        "[SL RR FUNC]"
        " slDLSSDGetOptimalSettings"
        " result=%d ptr=%p\n",

        static_cast<int>(
            rrOptimalFnResult
        ),

        reinterpret_cast<void*>(
            g_slDLSSDGetOptimalSettings
        )
    );


    g_dlssRRReady =
        rrSetOptionsFnResult ==
            sl::Result::eOk &&
        rrOptimalFnResult ==
            sl::Result::eOk &&
        g_slDLSSDSetOptions !=
            nullptr &&
        g_slDLSSDGetOptimalSettings !=
            nullptr;


    fprintf(
        stderr,
        "[SL RR] READY=%d\n",
        g_dlssRRReady
            ? 1
            : 0
    );
}
else
{
    g_dlssRRReady =
        false;


    fprintf(
        stderr,
        "[SL RR] READY=0"
        " feature load unavailable\n"
    );
}





    std::cout
    << "[Native C++] slSetVulkanInfo result: "
    << (int)vkResult
    << std::endl;



    printf(
    "[SL RESULT] init=%d vulkan=%d\n",
    (int)r,
    (int)vkResult
    );

    {
        std::ostringstream slResultLog;

        slResultLog
            << "[SL RESULT]"
            << " init=" << (int)r
            << " vulkan=" << (int)vkResult;

        RTLog(slResultLog.str());
    }
    g_slDeviceConfigured =
    vkResult ==
    sl::Result::eOk;


    const bool proxyReady =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies();


    fprintf(
        stderr,
        "[FG-3 EARLY VK]"
        " configured=%d"
        " proxies=%d\n",

        g_slDeviceConfigured
            ? 1
            : 0,

        proxyReady
            ? 1
            : 0
    );

    fflush(stderr);

    }

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_prepareStreamlineVulkan(
    JNIEnv*,
    jclass,
    jlong instanceHandle,
    jlong physicalDeviceHandle,
    jlong deviceHandle
)
{
    if(
        instanceHandle == 0 ||
        physicalDeviceHandle == 0 ||
        deviceHandle == 0
    )
    {
        return JNI_FALSE;
    }


    g_instance =
        reinterpret_cast<VkInstance>(
            instanceHandle
        );

    g_physicalDevice =
        reinterpret_cast<VkPhysicalDevice>(
            physicalDeviceHandle
        );

    g_device =
        reinterpret_cast<VkDevice>(
            deviceHandle
        );


    initStreamlineInternal();


    const bool ready =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies();


    fprintf(
        stderr,
        "[FG-3 PREPARE]"
        " configured=%d"
        " proxyReady=%d\n",

        g_slDeviceConfigured ? 1 : 0,
        ready ? 1 : 0
    );

    fflush(stderr);


    return ready
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_slCreateWin32Surface(
    JNIEnv*,
    jclass,
    jlong instanceHandle,
    jlong hwndHandle,
    jlong surfaceOutAddress
)
{
    VkInstance instance =
        reinterpret_cast<VkInstance>(
            instanceHandle
        );


    VkSurfaceKHR* surfaceOut =
        reinterpret_cast<VkSurfaceKHR*>(
            static_cast<uintptr_t>(
                surfaceOutAddress
            )
        );


    VkWin32SurfaceCreateInfoKHR ci{
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR
    };


    ci.hinstance =
        GetModuleHandleW(
            nullptr
        );

    ci.hwnd =
        reinterpret_cast<HWND>(
            static_cast<uintptr_t>(
                hwndHandle
            )
        );


    const bool useProxy =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies() &&
        g_slVkCreateWin32SurfaceKHRProxy;


    VkResult result =
        useProxy
            ? g_slVkCreateWin32SurfaceKHRProxy(
                instance,
                &ci,
                nullptr,
                surfaceOut
            )
            : ::vkCreateWin32SurfaceKHR(
                instance,
                &ci,
                nullptr,
                surfaceOut
            );


    fprintf(
        stderr,
        "[FG-3 SURFACE CREATE]"
        " proxy=%d"
        " result=%d\n",

        useProxy ? 1 : 0,
        static_cast<int>(result)
    );

    fflush(stderr);


    return static_cast<jint>(
        result
    );
}


extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_slDestroySurface(
    JNIEnv*,
    jclass,
    jlong instanceHandle,
    jlong surfaceHandle
)
{
    VkInstance instance =
        reinterpret_cast<VkInstance>(
            instanceHandle
        );

    VkSurfaceKHR surface =
        reinterpret_cast<VkSurfaceKHR>(
            static_cast<uintptr_t>(
                surfaceHandle
            )
        );


    const bool useProxy =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies() &&
        g_slVkDestroySurfaceKHRProxy;


    if(useProxy)
    {
        g_slVkDestroySurfaceKHRProxy(
            instance,
            surface,
            nullptr
        );
    }
    else
    {
        ::vkDestroySurfaceKHR(
            instance,
            surface,
            nullptr
        );
    }


    fprintf(
        stderr,
        "[FG-3 SURFACE DESTROY]"
        " proxy=%d\n",

        useProxy ? 1 : 0
    );

    fflush(stderr);
}


// ============================================================
// 2026-10-29 OPT-EXT⑥：HDR 静态元数据上报
//
// 现状：HDR-2 已经把 swapchain 强制成 A2B10G10R10 + HDR10_ST2084（见
// [HDR-2.3 SWAPCHAIN] 那条），pq 编码在 rr_hdr_encode.comp 里做，但从未调用
// vkSetHdrMetadataEXT ⇒ 显示器拿到的是"没有母带信息的 HDR10"，PQ 到实际
// nits 的映射只能由驱动/系统猜。
//
// 【数值是自创的】规范只定义字段语义，没有推荐值。这里取：
//   · 原色 = BT.2020（HDR10 母带常色域）、白点 = D65
//   · maxLuminance 1000 / minLuminance 0.05 nits（HDR10 母带常见档）
//   · MaxCLL 1000 / MaxFALL 400（保守，宁小不大 —— 报大了会过曝）
// 若之后实测显示器峰值不同（或编码器上限改了），只需改这四个常数。
// ============================================================
void applyHdrStaticMetadata(VkDevice device, VkSwapchainKHR swapchain)
{
    if(device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE)
    {
        return;
    }

    auto setHdrMetadata = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
        vkGetDeviceProcAddr(device, "vkSetHdrMetadataEXT"));

    if(setHdrMetadata == nullptr)
    {
        fprintf(
            stderr,
            "[HDR-2.5 METADATA] VK_EXT_hdr_metadata not enabled\n"
        );

        fflush(stderr);
        return;
    }

    VkHdrMetadataEXT metadata{};
    metadata.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT;
    metadata.displayPrimaryRed.x = 0.708f;
    metadata.displayPrimaryRed.y = 0.292f;
    metadata.displayPrimaryGreen.x = 0.170f;
    metadata.displayPrimaryGreen.y = 0.797f;
    metadata.displayPrimaryBlue.x = 0.131f;
    metadata.displayPrimaryBlue.y = 0.046f;
    metadata.whitePoint.x = 0.3127f;
    metadata.whitePoint.y = 0.3290f;
    metadata.maxLuminance = 1000.0f;
    metadata.minLuminance = 0.05f;
    metadata.maxContentLightLevel = 1000.0f;
    metadata.maxFrameAverageLightLevel = 400.0f;

    setHdrMetadata(device, 1u, &swapchain, &metadata);

    fprintf(
        stderr,
        "[HDR-2.5 METADATA] applied primaries=BT2020 white=D65"
        " maxLum=1000 minLum=0.05 MaxCLL=1000 MaxFALL=400\n"
    );

    fflush(stderr);
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_slCreateSwapchain(
    JNIEnv*,
    jclass,
    jlong deviceHandle,
    jlong createInfoAddress,
    jlong swapchainOutAddress
)
{
    VkDevice device =
        reinterpret_cast<VkDevice>(
            deviceHandle
        );


    const auto* createInfo =
        reinterpret_cast<
            const VkSwapchainCreateInfoKHR*
        >(
            static_cast<uintptr_t>(
                createInfoAddress
            )
        );


    VkSwapchainKHR* swapchainOut =
        reinterpret_cast<VkSwapchainKHR*>(
            static_cast<uintptr_t>(
                swapchainOutAddress
            )
        );


    const VkSwapchainCreateInfoKHR* effectiveCreateInfo =
        createInfo;

    VkSwapchainCreateInfoKHR hdrCreateInfo{};

    if(hdrOutputActive() && createInfo)
    {
        hdrCreateInfo =
            *createInfo;

        hdrCreateInfo.imageFormat =
            VK_FORMAT_A2B10G10R10_UNORM_PACK32;

        hdrCreateInfo.imageColorSpace =
            VK_COLOR_SPACE_HDR10_ST2084_EXT;

        effectiveCreateInfo =
            &hdrCreateInfo;

        fprintf(
            stderr,
            "[HDR-2.3 SWAPCHAIN]"
            " force A2B10G10R10 + HDR10_ST2084"
            " was=%d/%d\n",

            static_cast<int>(createInfo->imageFormat),
            static_cast<int>(createInfo->imageColorSpace)
        );

        fflush(stderr);
    }


    const bool useProxy =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies() &&
        g_slVkCreateSwapchainKHRProxy;


    VkResult result =
        useProxy
            ? g_slVkCreateSwapchainKHRProxy(
                device,
                effectiveCreateInfo,
                nullptr,
                swapchainOut
            )
            : ::vkCreateSwapchainKHR(
                device,
                effectiveCreateInfo,
                nullptr,
                swapchainOut
            );


    fprintf(
        stderr,
        "[FG-3 SWAPCHAIN CREATE]"
        " proxy=%d"
        " result=%d\n",

        useProxy ? 1 : 0,
        static_cast<int>(result)
    );

    fflush(stderr);


    if(
        result == VK_SUCCESS &&
        swapchainOut != nullptr &&
        *swapchainOut != VK_NULL_HANDLE &&
        hdrOutputActive()
    )
    {
        applyHdrStaticMetadata(device, *swapchainOut);
    }


    return static_cast<jint>(
        result
    );
}


extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_slDestroySwapchain(
    JNIEnv*,
    jclass,
    jlong deviceHandle,
    jlong swapchainHandle
)
{
    VkDevice device =
        reinterpret_cast<VkDevice>(
            deviceHandle
        );


    VkSwapchainKHR swapchain =
        reinterpret_cast<VkSwapchainKHR>(
            static_cast<uintptr_t>(
                swapchainHandle
            )
        );


    FGDeviceWaitIdle(
        device
    );


    const bool useProxy =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies() &&
        g_slVkDestroySwapchainKHRProxy;


    if(useProxy)
    {
        g_slVkDestroySwapchainKHRProxy(
            device,
            swapchain,
            nullptr
        );
    }
    else
    {
        ::vkDestroySwapchainKHR(
            device,
            swapchain,
            nullptr
        );
    }


    fprintf(
        stderr,
        "[FG-3 SWAPCHAIN DESTROY]"
        " proxy=%d\n",

        useProxy ? 1 : 0
    );

    fflush(stderr);
}


extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_slGetSwapchainImages(
    JNIEnv*,
    jclass,
    jlong deviceHandle,
    jlong swapchainHandle,
    jlong countAddress,
    jlong imagesAddress
)
{
    VkDevice device =
        reinterpret_cast<VkDevice>(
            deviceHandle
        );


    VkSwapchainKHR swapchain =
        reinterpret_cast<VkSwapchainKHR>(
            static_cast<uintptr_t>(
                swapchainHandle
            )
        );


    uint32_t* count =
        reinterpret_cast<uint32_t*>(
            static_cast<uintptr_t>(
                countAddress
            )
        );


    VkImage* images =
        imagesAddress != 0
            ? reinterpret_cast<VkImage*>(
                static_cast<uintptr_t>(
                    imagesAddress
                )
            )
            : nullptr;


    const bool useProxy =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies() &&
        g_slVkGetSwapchainImagesKHRProxy;


    VkResult result =
        useProxy
            ? g_slVkGetSwapchainImagesKHRProxy(
                device,
                swapchain,
                count,
                images
            )
            : ::vkGetSwapchainImagesKHR(
                device,
                swapchain,
                count,
                images
            );


    if(images != nullptr)
    {
        fprintf(
            stderr,
            "[FG-3 GET IMAGES]"
            " proxy=%d"
            " result=%d"
            " count=%u\n",

            useProxy ? 1 : 0,
            static_cast<int>(result),
            count ? *count : 0
        );

        fflush(stderr);
    }


    return static_cast<jint>(
        result
    );
}


extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_slAcquireNextImage(
    JNIEnv*,
    jclass,
    jlong deviceHandle,
    jlong swapchainHandle,
    jlong timeout,
    jlong semaphoreHandle,
    jlong fenceHandle,
    jlong imageIndexAddress
)
{
    VkDevice device =
        reinterpret_cast<VkDevice>(
            deviceHandle
        );


    VkSwapchainKHR swapchain =
        reinterpret_cast<VkSwapchainKHR>(
            static_cast<uintptr_t>(
                swapchainHandle
            )
        );


    VkSemaphore semaphore =
        reinterpret_cast<VkSemaphore>(
            static_cast<uintptr_t>(
                semaphoreHandle
            )
        );


    VkFence fence =
        reinterpret_cast<VkFence>(
            static_cast<uintptr_t>(
                fenceHandle
            )
        );


    uint32_t* imageIndex =
        reinterpret_cast<uint32_t*>(
            static_cast<uintptr_t>(
                imageIndexAddress
            )
        );


    const bool useProxy =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies() &&
        g_slVkAcquireNextImageKHRProxy;


    VkResult result =
        useProxy
            ? g_slVkAcquireNextImageKHRProxy(
                device,
                swapchain,
                static_cast<uint64_t>(timeout),
                semaphore,
                fence,
                imageIndex
            )
            : ::vkAcquireNextImageKHR(
                device,
                swapchain,
                static_cast<uint64_t>(timeout),
                semaphore,
                fence,
                imageIndex
            );


    static bool printed =
        false;


    if(!printed)
    {
        fprintf(
            stderr,
            "[FG-3 ACQUIRE]"
            " proxy=%d"
            " result=%d"
            " index=%u\n",

            useProxy ? 1 : 0,
            static_cast<int>(result),
            imageIndex ? *imageIndex : UINT32_MAX
        );

        fflush(stderr);

        printed = true;
    }


    return static_cast<jint>(
        result
    );
}


extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_slQueuePresent(
    JNIEnv*,
    jclass,
    jlong queueHandle,
    jlong presentInfoAddress
)
{
    VkQueue queue =
        reinterpret_cast<VkQueue>(
            queueHandle
        );


    const VkPresentInfoKHR* presentInfo =
        reinterpret_cast<
            const VkPresentInfoKHR*
        >(
            static_cast<uintptr_t>(
                presentInfoAddress
            )
        );


    const bool useProxy =
        g_slDeviceConfigured &&
        ResolveSLVulkanProxies() &&
        g_slVkQueuePresentKHRProxy;

    if(!useProxy && g_slInitialized)
    {
        static bool warnedNoProxyPresent = false;

        if(!warnedNoProxyPresent)
        {
            warnedNoProxyPresent = true;

            fprintf(
                stderr,
                "[FG-3 PRESENT] WARNING: present bypasses Streamline proxy\n"
            );

            fflush(stderr);
        }
    }


    static bool fg43Enabled =
        false;
    static bool fg43AppliedFrameSync =
        true;
    static int fg43AppliedMultiplier =
        0;
    static uint32_t fg43AppliedGeneratedMax =
        0;

    {
        uint32_t fgGateMask =
            0u;

        if(g_isInWorld)
            fgGateMask |= 1u << 0;
        if(g_dlssGReady)
            fgGateMask |= 1u << 1;
        if(g_slDLSSGSetOptions != nullptr)
            fgGateMask |= 1u << 2;
        if(g_slDLSSGGetState != nullptr)
            fgGateMask |= 1u << 3;
        if(g_currentSLFrameValid)
            fgGateMask |= 1u << 4;
        if(g_currentSLFrameToken != nullptr)
            fgGateMask |= 1u << 5;
        if(g_fgHudlessFrameIndex == g_currentSLFrameIndex)
            fgGateMask |= 1u << 6;
        if(g_fgUIAlphaFrameIndex == g_currentSLFrameIndex)
            fgGateMask |= 1u << 7;

        if(fgGateMask != g_fgGateLastMask)
        {
            std::ostringstream fgGateLog;

            if(fgGateMask == 0xffu)
            {
                fgGateLog
                    << "[FG GATE]"
                    << " frame=" << g_perfL10FrameSerial
                    << " all=true";
            }
            else
            {
                fgGateLog
                    << "[FG GATE]"
                    << " frame=" << g_perfL10FrameSerial
                    << " inWorld=" << ((fgGateMask & (1u << 0)) ? 1 : 0)
                    << " dlssGReady=" << ((fgGateMask & (1u << 1)) ? 1 : 0)
                    << " setFn=" << ((fgGateMask & (1u << 2)) ? 1 : 0)
                    << " getFn=" << ((fgGateMask & (1u << 3)) ? 1 : 0)
                    << " frameValid=" << ((fgGateMask & (1u << 4)) ? 1 : 0)
                    << " token=" << ((fgGateMask & (1u << 5)) ? 1 : 0)
                    << " hudlessMatch=" << ((fgGateMask & (1u << 6)) ? 1 : 0)
                    << " uiMatch=" << ((fgGateMask & (1u << 7)) ? 1 : 0);

                if((fgGateMask & (1u << 6)) == 0u)
                {
                    fgGateLog
                        << " hudlessMatch=0"
                        << " hudlessIdx=" << g_fgHudlessFrameIndex
                        << " slIdx=" << g_currentSLFrameIndex;
                }

                if((fgGateMask & (1u << 7)) == 0u)
                {
                    fgGateLog
                        << " uiIdx=" << g_fgUIAlphaFrameIndex;
                }
            }

            RTLog(fgGateLog.str());

            g_fgGateLastMask = fgGateMask;
        }
    }

    const int fgMultiplierRequested =
        g_fgMultiplierRequested.load();

    const bool fgShapeChangeHold =
        g_fgForceOffFrames.load() != 0u;

    const bool fgDesiredOn =
        g_isInWorld &&
        !g_fgFullscreenMenuOpen.load() &&
        !fgShapeChangeHold &&
        fgMultiplierRequested >= 2;

    if(fgShapeChangeHold)
    {
        g_fgForceOffFrames.fetch_sub(1u);
    }

    if(
        (!fg43Enabled ||
         fg43AppliedFrameSync != g_fgFrameSyncEnabled ||
         fg43AppliedMultiplier != fgMultiplierRequested ||
         fg43AppliedGeneratedMax != g_fgGeneratedMaxSeen.load()) &&
        fgDesiredOn &&
        g_dlssGReady &&
        g_slDLSSGSetOptions &&
        g_slDLSSGGetState &&
        g_currentSLFrameValid &&
        g_currentSLFrameToken != nullptr &&
        g_fgHudlessFrameIndex == g_currentSLFrameIndex &&
        g_fgUIAlphaFrameIndex == g_currentSLFrameIndex
    )
    {
        sl::DLSSGState preState{};

        sl::Result preStateResult =
            g_slDLSSGGetState(
                g_dlssViewport,
                preState,
                nullptr
            );


        if(
            preStateResult == sl::Result::eOk &&
            preState.numFramesToGenerateMax >= 1
        )
        {
            const uint32_t fgGeneratedMax =
                preState.numFramesToGenerateMax;

            const uint32_t fgGeneratedRequested =
                fgMultiplierRequested >= 2
                    ? static_cast<uint32_t>(fgMultiplierRequested - 1)
                    : 1u;

            uint32_t refreshMultiplierCap = 6u;

            if(g_fgRefreshCapEnabled.load())
            {
                const uint32_t refreshHz = QueryDisplayRefreshHz();

                if(refreshHz > 0u)
                {
                    refreshMultiplierCap =
                        refreshHz <= 60u ? 4u
                            : (refreshHz <= 75u ? 5u : 6u);
                }
            }

            const uint32_t fgGeneratedByRefresh =
                refreshMultiplierCap > 1u ? refreshMultiplierCap - 1u : 1u;

            const uint32_t fgGeneratedCap =
                fgGeneratedMax < fgGeneratedByRefresh
                    ? fgGeneratedMax
                    : fgGeneratedByRefresh;

            const uint32_t fgGeneratedApplied =
                fgGeneratedRequested > fgGeneratedCap
                    ? fgGeneratedCap
                    : fgGeneratedRequested;

            sl::DLSSGOptions options{};

            options.mode =
                sl::DLSSGMode::eOn;

            options.numFramesToGenerate =
                fgGeneratedApplied;

            g_fgGeneratedMaxSeen.store(
                fgGeneratedMax
            );

            options.enableUserInterfaceRecomposition =
                sl::Boolean::eTrue;

            if(g_fgFrameSyncEnabled)
            {
                options.queueParallelismMode =
                    sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
            }


            sl::Result setResult =
                g_slDLSSGSetOptions(
                    g_dlssViewport,
                    options
                );


            fprintf(
                stderr,
                "[FG-4.3 ENABLE]"
                " frame=%u"
                " setResult=%d"
                " preStateResult=%d"
                " status=%u"
                " maxGenerated=%u"
                " requestedGenerated=%u"
                " requestedMultiplier=%d"
                " uiRecomp=1"
                " frameSync=%d\n",

                g_currentSLFrameIndex,

                static_cast<int>(
                    setResult
                ),

                static_cast<int>(
                    preStateResult
                ),

                static_cast<unsigned int>(
                    preState.status
                ),

                preState.numFramesToGenerateMax,

                fgGeneratedApplied,

                fgMultiplierRequested,

                g_fgFrameSyncEnabled ? 1 : 0
            );

            fflush(stderr);

            {
                std::ostringstream fgEnableLog;

                fgEnableLog
                    << "[FG-4.3 ENABLE]"
                    << " frame=" << g_currentSLFrameIndex
                    << " setResult=" << static_cast<int>(setResult)
                    << " preStateResult=" << static_cast<int>(preStateResult)
                    << " status=" << static_cast<unsigned int>(preState.status)
                    << " maxGenerated=" << preState.numFramesToGenerateMax
                    << " requestedGenerated=" << fgGeneratedApplied
                    << " requestedMultiplier=" << fgMultiplierRequested
                    << " uiRecomp=1"
                    << " frameSync=" << (g_fgFrameSyncEnabled ? 1 : 0)
                    << " qp=blockPresenting"
                    << " baseLimitHz=" << g_fgBaseFrameLimitHz.load()
                    << " markersOpt=1";

                RTLog(fgEnableLog.str());
            }


            if(setResult == sl::Result::eOk)
            {
                fg43Enabled =
                    true;
                fg43AppliedFrameSync =
                    g_fgFrameSyncEnabled;
                fg43AppliedMultiplier =
                    fgMultiplierRequested;
                fg43AppliedGeneratedMax =
                    fgGeneratedMax;
            }
        }
    }


    if(
        !fgDesiredOn &&
        fg43Enabled &&
        g_dlssGReady &&
        g_slDLSSGSetOptions
    )
    {
        sl::DLSSGOptions offOptions{};

        offOptions.mode =
            sl::DLSSGMode::eOff;

        offOptions.flags |=
            sl::DLSSGFlags::eRetainResourcesWhenOff;

        sl::Result offResult =
            g_slDLSSGSetOptions(
                g_dlssViewport,
                offOptions
            );

        if(offResult == sl::Result::eOk)
        {
            fg43Enabled =
                false;
        }


        fprintf(
            stderr,
            "[FG-4.4 MENU OFF]"
            " frame=%u"
            " offResult=%d"
            " inWorld=%d"
            " menuOpen=%d\n",
            g_currentSLFrameIndex,
            static_cast<int>(offResult),
            g_isInWorld ? 1 : 0,
            g_fgFullscreenMenuOpen.load() ? 1 : 0
        );

        fflush(stderr);


        {
            std::ostringstream fgMenuOffLog;

            fgMenuOffLog
                << "[FG-4.4 MENU OFF]"
                << " frame=" << g_currentSLFrameIndex
                << " offResult=" << static_cast<int>(offResult)
                << " inWorld=" << (g_isInWorld ? 1 : 0)
                << " menuOpen=" << (g_fgFullscreenMenuOpen.load() ? 1 : 0);

            RTLog(fgMenuOffLog.str());
        }
    }


    VkResult result =
        useProxy
            ? g_slVkQueuePresentKHRProxy(
                queue,
                presentInfo
            )
            : ::vkQueuePresentKHR(
                queue,
                presentInfo
            );
    static VkResult lastPresentResult = VK_SUCCESS;
    if(result != VK_SUCCESS && result != lastPresentResult)
    {
        fprintf(
            stderr,
            "[FG-4.3 PRESENT]"
            " frame=%u"
            " proxy=%d"
            " result=%d\n",
            g_currentSLFrameIndex,
            useProxy ? 1 : 0,
            static_cast<int>(result)
        );

        fflush(stderr);
    }

    lastPresentResult = result;


    static bool printed =
        false;


    if(!printed)
    {
        fprintf(
            stderr,
            "[FG-3 PRESENT]"
            " proxy=%d"
            " result=%d\n",

            useProxy ? 1 : 0,
            static_cast<int>(result)
        );

        fflush(stderr);

        {
            std::ostringstream fgPresentLog;

            fgPresentLog
                << "[FG-3 PRESENT]"
                << " proxy=" << (useProxy ? 1 : 0)
                << " result=" << static_cast<int>(result);

            RTLog(fgPresentLog.str());
        }

        printed = true;
    }


    return static_cast<jint>(
        result
    );
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setReflexMode(
    JNIEnv*,
    jclass,
    jint mode)
{
    return ApplyReflexMode(
        static_cast<int>(mode)
    )
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_isReflexAvailable(
    JNIEnv*,
    jclass)
{
    return g_reflexLowLatencyAvailable
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getReflexMode(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(
        g_reflexMode
    );
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setFGFrameSync(
    JNIEnv*,
    jclass,
    jboolean enabled)
{
    g_fgFrameSyncEnabled = enabled == JNI_TRUE;

    fprintf(
        stderr,
        "[FG FRAME SYNC] enabled=%d\n",
        g_fgFrameSyncEnabled ? 1 : 0
    );
    fflush(stderr);

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_setMotionVectorScaleMode(
    JNIEnv*, jclass, jint mode)
{
    const uint32_t clamped =
        static_cast<uint32_t>(mode < 0 ? 0 : (mode > 3 ? 3 : mode));

    g_mvScaleMode.store(clamped);
    g_motionVectorsEnabled.store(clamped == 0u ? 0u : 1u);

    return static_cast<jint>(clamped);
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getMotionVectorScaleMode(
    JNIEnv*, jclass)
{
    return static_cast<jint>(g_mvScaleMode.load());
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setFGRefreshCapEnabled(
    JNIEnv*, jclass, jboolean enabled)
{
    g_fgRefreshCapEnabled.store(enabled == JNI_TRUE);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_getFGRefreshCapEnabled(
    JNIEnv*, jclass)
{
    return g_fgRefreshCapEnabled.load() ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setStreamlineDevPluginsEnabled(
    JNIEnv*, jclass, jboolean enabled)
{
    g_slDevPluginsRequested = enabled == JNI_TRUE;

    fprintf(
        stderr,
        "[SL DEV PLUGINS] requested=%d\n",
        g_slDevPluginsRequested ? 1 : 0
    );

    fflush(stderr);

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_pclTriggerFlash(
    JNIEnv*, jclass)
{
    if(!g_pclSupported ||
       !g_slPCLSetMarker ||
       !g_slReflexGetState ||
       !g_currentSLFrameValid ||
       g_currentSLFrameToken == nullptr)
    {
        return JNI_FALSE;
    }

    sl::ReflexState reflexState{};

    if(g_slReflexGetState(reflexState) != sl::Result::eOk ||
       !reflexState.flashIndicatorDriverControlled)
    {
        return JNI_FALSE;
    }

    return SetCurrentPCLMarker(sl::PCLMarker::eTriggerFlash)
               ? JNI_TRUE
               : JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_pclLatencyPing(
    JNIEnv*, jclass)
{
    if(!g_pclSupported ||
       !g_slPCLSetMarker ||
       !g_currentSLFrameValid ||
       g_currentSLFrameToken == nullptr)
    {
        return JNI_FALSE;
    }

    return SetCurrentPCLMarker(sl::PCLMarker::ePCLatencyPing)
               ? JNI_TRUE
               : JNI_FALSE;
}

// =====================================================
// 2026-10-22 显存：DLSS-G 内部资源 + 宿主 FG 快照的"长期关闭"回收
//
// 官方 DLSS-G §4：短时间关闭（菜单）→ eOff 即可；长时间关闭（用户关掉）
// → eOff + slFreeResources。只统计"倍率 < 2"的帧，菜单不计。
// A2：显存超预算时阈值从 600 帧压到 60 帧。
// A1：插件资源归还后，宿主三张快照（hudless/final/UIAlpha）一起销毁；
//     用户重新开启 FG 时由 ensureFGUIResources 重建。
// =====================================================
void MaybeReleaseFgResourcesOnIdle()
{
    static uint32_t consecutiveDisabledFrames = 0u;
    static bool resourcesReleased = false;

    if(g_fgMultiplierRequested.load() >= 2)
    {
        consecutiveDisabledFrames = 0u;
        resourcesReleased = false;
        return;
    }

    if(resourcesReleased || !g_slFreeResources || !g_dlssGReady)
    {
        return;
    }

    const bool vramOverBudget =
        g_vramGovernor.available && g_vramGovernor.over;

    const uint32_t releaseAfterFrames =
        vramOverBudget ? 60u : 600u;

    ++consecutiveDisabledFrames;

    if(consecutiveDisabledFrames < releaseAfterFrames)
    {
        return;
    }

    sl::Result releaseResult =
        g_slFreeResources(sl::kFeatureDLSS_G, g_dlssViewport);

    fprintf(
        stderr,
        "[FG RES] release result=%d frames=%u vramOver=%d\n",
        static_cast<int>(releaseResult),
        consecutiveDisabledFrames,
        vramOverBudget ? 1 : 0
    );

    fflush(stderr);

    RTLog(
        std::string("[FG RES] release DLSS-G resources result=") +
        std::to_string(static_cast<int>(releaseResult)));

    if(releaseResult != sl::Result::eOk)
    {
        return;
    }

    resourcesReleased = true;
    consecutiveDisabledFrames = 0u;

    if(!g_fgSnapshotsReleased &&
       (g_fgHudlessImage != VK_NULL_HANDLE ||
        g_fgFinalImage != VK_NULL_HANDLE ||
        g_fgUIAlphaImage != VK_NULL_HANDLE))
    {
        if(FGDeviceWaitIdle(g_device) == VK_SUCCESS)
        {
            destroyFGUIImages();

            g_fgSnapshotsReleased = true;
            g_fgTagsValid = false;

            fprintf(
                stderr,
                "[FG RES] snapshots released (hudless/final/uiAlpha)\n"
            );

            fflush(stderr);
        }
    }
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setFGMultiplier(
    JNIEnv*,
    jclass,
    jint multiplier)
{
    const int requested =
        static_cast<int>(
            multiplier
        );


    if(
        requested != 0 &&
        requested != 2 &&
        requested != 3 &&
        requested != 4 &&
        requested != 6
    )
    {
        fprintf(
            stderr,
            "[FG MULTIPLIER] rejected=%d\n",
            requested
        );
        fflush(stderr);

        return JNI_FALSE;
    }


    g_fgMultiplierRequested.store(
        requested
    );

    RefreshFgBaseFrameLimit();


    {
        std::ostringstream fgMultiplierLog;

        fgMultiplierLog
            << "[FG MULTIPLIER]"
            << " requested="
            << (requested == 0
                    ? std::string("off")
                    : std::to_string(requested) + "x")
            << " max=" << (g_fgGeneratedMaxSeen.load() + 1u) << "x";

        RTLog(fgMultiplierLog.str());
    }

    fprintf(
        stderr,
        "[FG MULTIPLIER] requested=%dx max=%ux\n",
        requested,
        static_cast<unsigned int>(
            g_fgGeneratedMaxSeen.load() + 1u
        )
    );
    fflush(stderr);

    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getFGMultiplierMax(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(
        g_fgGeneratedMaxSeen.load() + 1u
    );
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setFGBaseFrameLimit(
    JNIEnv*,
    jclass,
    jint hz)
{
    const int request =
        static_cast<int>(
            hz
        );


    if(request < -1 || request > 480)
    {
        fprintf(
            stderr,
            "[FG BASE LIMIT] rejected=%d\n",
            request
        );

        fflush(stderr);

        return JNI_FALSE;
    }


    g_fgBaseFrameLimitRequest =
        request;

    RefreshFgBaseFrameLimit();


    {
        std::ostringstream fgBaseLimitLog;

        fgBaseLimitLog
            << "[FG BASE LIMIT]"
            << " request=" << request
            << " resolvedHz=" << g_fgBaseFrameLimitHz.load()
            << " multiplier=" << g_fgMultiplierRequested.load() << "x";

        RTLog(fgBaseLimitLog.str());
    }


    return JNI_TRUE;
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_setFullscreenMenuOpen(
    JNIEnv*,
    jclass,
    jboolean open)
{
    const bool menuOpen =
        open == JNI_TRUE;


    if(g_fgFullscreenMenuOpen.exchange(menuOpen) == menuOpen)
    {
        return JNI_TRUE;
    }


    RefreshFgBaseFrameLimit();


    {
        std::ostringstream fgMenuLog;

        fgMenuLog
            << "[FG MENU]"
            << " open=" << (menuOpen ? 1 : 0)
            << " baseLimitHz=" << g_fgBaseFrameLimitHz.load();

        RTLog(fgMenuLog.str());
    }


    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getFGBaseFrameLimit(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(
        g_fgBaseFrameLimitHz.load()
    );
}

extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getDisplayRefreshRateHz(
    JNIEnv*,
    jclass)
{
    return static_cast<jint>(
        QueryDisplayRefreshHz()
    );
}

extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_beginStreamlineFrame(
    JNIEnv*,
    jclass)
{
    const LeakProbeScope leakProbeScope;


    return BeginStreamlineFrame()
        ? JNI_TRUE
        : JNI_FALSE;
}
extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_markSimulationStart(
    JNIEnv*,
    jclass)
{
    const LeakProbeScope leakProbeScope;


    const bool result =
        SetCurrentPCLMarker(
            sl::PCLMarker::eSimulationStart
        );


    if(result)
    {
        static bool printed =
            false;


        if(!printed)
        {
            fprintf(
                stderr,
                "[PCL] SimulationStart ACTIVE frame=%u\n",
                g_currentSLFrameIndex
            );

            printed =
                true;
        }
    }


    return result
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_markSimulationEnd(
    JNIEnv*,
    jclass)
{
    const bool result =
        SetCurrentPCLMarker(
            sl::PCLMarker::eSimulationEnd
        );


    if(result)
    {
        static bool printed =
            false;


        if(!printed)
        {
            fprintf(
                stderr,
                "[PCL] SimulationEnd ACTIVE frame=%u\n",
                g_currentSLFrameIndex
            );

            printed =
                true;
        }
    }


    return result
        ? JNI_TRUE
        : JNI_FALSE;
}
extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_markRenderSubmitStart(
    JNIEnv*,
    jclass)
{
    return MarkRenderSubmitStart()
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_markRenderSubmitEnd(
    JNIEnv*,
    jclass)
{
    return MarkRenderSubmitEnd()
        ? JNI_TRUE
        : JNI_FALSE;
}
extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_markPresentStart(
    JNIEnv*,
    jclass)
{
    return MarkPresentStart()
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_markPresentEnd(
    JNIEnv*,
    jclass)
{
    return MarkPresentEnd()
        ? JNI_TRUE
        : JNI_FALSE;
}
jobjectArray
FGMakeJavaStringArray(
    JNIEnv* env,
    const std::vector<std::string>& values
)
{
    jclass stringClass =
        env->FindClass(
            "java/lang/String"
        );


    if(!stringClass)
    {
        return nullptr;
    }


    jobjectArray result =
        env->NewObjectArray(
            static_cast<jsize>(
                values.size()
            ),
            stringClass,
            nullptr
        );


    if(!result)
    {
        env->DeleteLocalRef(
            stringClass
        );

        return nullptr;
    }


    for(
        size_t i = 0;
        i < values.size();
        ++i
    )
    {
        jstring value =
            env->NewStringUTF(
                values[i].c_str()
            );


        if(value)
        {
            env->SetObjectArrayElement(
                result,
                static_cast<jsize>(i),
                value
            );


            env->DeleteLocalRef(
                value
            );
        }
    }


    env->DeleteLocalRef(
        stringClass
    );


    return result;
}
extern "C"
JNIEXPORT jboolean JNICALL Java_com_example_vulkan_NativeVulkan_isFGRequirementsReady(
    JNIEnv*,
    jclass
)
{
    return g_fgRequirementsReady
        ? JNI_TRUE
        : JNI_FALSE;
}


extern "C"
JNIEXPORT jobjectArray JNICALL Java_com_example_vulkan_NativeVulkan_getFGInstanceExtensions(
    JNIEnv* env,
    jclass
)
{
    return FGMakeJavaStringArray(
        env,
        g_fgInstanceExtensions
    );
}


extern "C"
JNIEXPORT jobjectArray JNICALL Java_com_example_vulkan_NativeVulkan_getFGDeviceExtensions(
    JNIEnv* env,
    jclass
)
{
    return FGMakeJavaStringArray(
        env,
        g_fgDeviceExtensions
    );
}


extern "C"
JNIEXPORT jobjectArray JNICALL Java_com_example_vulkan_NativeVulkan_getFGVulkan12Features(
    JNIEnv* env,
    jclass
)
{
    return FGMakeJavaStringArray(
        env,
        g_fgFeatures12
    );
}


extern "C"
JNIEXPORT jobjectArray JNICALL Java_com_example_vulkan_NativeVulkan_getFGVulkan13Features(
    JNIEnv* env,
    jclass
)
{
    return FGMakeJavaStringArray(
        env,
        g_fgFeatures13
    );
}


extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getFGGraphicsQueuesRequired(
    JNIEnv*,
    jclass
)
{
    return static_cast<jint>(
        g_fgGraphicsQueuesRequired
    );
}


extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getFGComputeQueuesRequired(
    JNIEnv*,
    jclass
)
{
    return static_cast<jint>(
        g_fgComputeQueuesRequired
    );
}


extern "C"
JNIEXPORT jint JNICALL Java_com_example_vulkan_NativeVulkan_getFGOpticalFlowQueuesRequired(
    JNIEnv*,
    jclass
)
{
    return static_cast<jint>(
        g_fgOpticalFlowQueuesRequired
    );
}
extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_initStreamlineEarly(
    JNIEnv*,
    jclass)
{
    fprintf(
        stderr,
        "[SL EARLY] initStreamlineEarly ENTER device=%p\n",
        (void*)g_device
    );

    fflush(stderr);


    if(g_slInitialized)
    {
        fprintf(
            stderr,
            "[SL EARLY] already initialized\n"
        );

        fflush(stderr);
        return;
    }


    LoadMfgUnlockModuleIfPresent();


    if(!g_slModule)
    {
        const std::string runtimeDirectory =
            GetRTXBackendDirectory();

        std::string devPluginDirectory;

        if(g_slDevPluginsRequested && !runtimeDirectory.empty())
        {
            const std::string candidate =
                runtimeDirectory + "\\sl_debug_dev";

            const std::string candidateInterposer =
                candidate + "\\sl.interposer.dll";

            if(GetFileAttributesA(
                   candidateInterposer.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                devPluginDirectory = candidate;
            }
            else
            {
                fprintf(
                    stderr,
                    "[SL EARLY PATH] dev plugins requested but %s"
                    " is missing -> falling back to production\n",
                    candidateInterposer.c_str()
                );

                fflush(stderr);
            }
        }


        g_slActivePluginDir = devPluginDirectory;

        const std::string interposerPath =
            devPluginDirectory.empty()
                ? ResolveRTXRuntimePath("sl.interposer.dll")
                : devPluginDirectory + "\\sl.interposer.dll";


        fprintf(
            stderr,
            "[SL EARLY PATH]"
            " backendDir=%s"
            " devPlugins=%d"
            " interposer=%s\n",
            runtimeDirectory.c_str(),
            devPluginDirectory.empty() ? 0 : 1,
            interposerPath.c_str()
        );

        fflush(stderr);


        const std::string searchDirectory =
            devPluginDirectory.empty()
                ? runtimeDirectory
                : devPluginDirectory;

        if(!searchDirectory.empty())
        {
            if(
                !SetDllDirectoryA(
                    searchDirectory.c_str()
                )
            )
            {
                fprintf(
                    stderr,
                    "[SL EARLY PATH]"
                    " SetDllDirectoryA failed error=%lu\n",
                    GetLastError()
                );

                fflush(stderr);
            }
        }


        g_slModule =
            LoadLibraryExA(
                interposerPath.c_str(),
                nullptr,
                LOAD_WITH_ALTERED_SEARCH_PATH
            );
    }


    if(!g_slModule)
    {
        fprintf(
            stderr,
            "[SL EARLY]"
            " LoadLibraryEx sl.interposer.dll failed"
            " error=%lu"
            " resolved=%s\n",
            GetLastError(),
            ResolveRTXRuntimePath(
                "sl.interposer.dll"
            ).c_str()
        );

        fflush(stderr);
        return;
    }


    g_slInit =
        reinterpret_cast<PFN_slInit>(
            GetProcAddress(
                g_slModule,
                "slInit"
            )
        );


    g_slShutdown =
        reinterpret_cast<PFN_slShutdown>(
            GetProcAddress(
                g_slModule,
                "slShutdown"
            )
        );
    g_slGetFeatureRequirements =
    reinterpret_cast<PFN_slGetFeatureRequirements>(
        GetProcAddress(
            g_slModule,
            "slGetFeatureRequirements"
        )
    );


    if(!g_slInit)
    {
        fprintf(
            stderr,
            "[SL EARLY] slInit missing\n"
        );

        fflush(stderr);
        return;
    }
    if(!g_slGetFeatureRequirements)
    {
        fprintf(
            stderr,
            "[SL EARLY] slGetFeatureRequirements missing\n"
        );

        fflush(stderr);
        return;
    }


    const sl::Feature features[] =
    {
        sl::kFeatureDLSS,
        sl::kFeatureDLSS_RR,
        sl::kFeatureDLSS_G,
        sl::kFeatureReflex,
        sl::kFeaturePCL
    };
    constexpr uint32_t slAllFeatureCount =
        static_cast<uint32_t>(sizeof(features) / sizeof(features[0]));

    const char* const slAllFeatureNames[slAllFeatureCount] =
    {
        "dlss",
        "dlss_rr",
        "dlss_g",
        "reflex",
        "pcl"
    };

    std::vector<sl::Feature> slSelectedFeatures;
    {
        static const std::string slFeatureRequest = []() -> std::string
        {
            const char* raw =
                std::getenv("ORION_SL_FEATURES");

            return raw == nullptr
                       ? std::string("all")
                       : std::string(raw);
        }();

        if(slFeatureRequest.empty() || slFeatureRequest == "all")
        {
            for(uint32_t i = 0u; i < slAllFeatureCount; ++i)
            {
                if(!kRequestDlssSrFeature &&
                   features[i] == sl::kFeatureDLSS)
                {
                    continue;
                }

                slSelectedFeatures.push_back(features[i]);
            }
        }
        else if(slFeatureRequest != "none")
        {
            std::size_t begin = 0u;

            while(begin <= slFeatureRequest.size())
            {
                const std::size_t comma =
                    slFeatureRequest.find(',', begin);

                std::string token = slFeatureRequest.substr(
                    begin,
                    comma == std::string::npos
                        ? std::string::npos
                        : comma - begin);

                while(!token.empty() &&
                      (token.front() == ' ' || token.front() == '\t'))
                {
                    token.erase(token.begin());
                }

                while(!token.empty() &&
                      (token.back() == ' ' || token.back() == '\t'))
                {
                    token.pop_back();
                }

                for(uint32_t i = 0u; i < slAllFeatureCount; ++i)
                {
                    if(token == slAllFeatureNames[i])
                    {
                        slSelectedFeatures.push_back(features[i]);

                        break;
                    }
                }

                if(comma == std::string::npos)
                {
                    break;
                }

                begin = comma + 1u;
            }
        }

        fprintf(
            stderr,
            "[SL EARLY] ORION_SL_FEATURES=\"%s\" -> %zu of %u feature(s)\n",
            slFeatureRequest.c_str(),
            slSelectedFeatures.size(),
            slAllFeatureCount
        );

        fflush(stderr);
    }

    sl::Preferences pref{};


    static std::wstring slPluginDirWide;
    static const wchar_t* slPluginPaths[1] = {nullptr};

    if(!g_slActivePluginDir.empty())
    {
        const int wideLength =
            MultiByteToWideChar(
                CP_UTF8,
                0,
                g_slActivePluginDir.c_str(),
                -1,
                nullptr,
                0
            );

        if(wideLength > 1)
        {
            slPluginDirWide.assign(
                static_cast<size_t>(wideLength - 1),
                L'\0'
            );

            MultiByteToWideChar(
                CP_UTF8,
                0,
                g_slActivePluginDir.c_str(),
                -1,
                slPluginDirWide.data(),
                wideLength
            );

            slPluginPaths[0] =
                slPluginDirWide.c_str();

            pref.pathsToPlugins =
                slPluginPaths;

            pref.numPathsToPlugins =
                1;
        }
    }


    pref.logMessageCallback =
        StreamlineLogCallback;


    pref.showConsole =
        false;


    pref.logLevel =
        sl::LogLevel::eOff;


    pref.renderAPI =
        sl::RenderAPI::eVulkan;


    pref.applicationId =
        231313132;


    pref.featuresToLoad =
        slSelectedFeatures.data();


    pref.numFeaturesToLoad =
        static_cast<uint32_t>(slSelectedFeatures.size());


    pref.flags =
        sl::PreferenceFlags::eDisableCLStateTracking |
        sl::PreferenceFlags::eUseManualHooking |
        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.flags |=
        sl::PreferenceFlags::eUseManualHooking;
    pref.flags |=
    sl::PreferenceFlags::eUseFrameBasedResourceTagging;


    fprintf(
        stderr,
        "[SL EARLY] BEFORE slInit device=%p manualHooking=1\n",
        (void*)g_device
    );

    fflush(stderr);


    sl::Result result =
        g_slInit(
            pref,
            sl::kSDKVersion
        );


    fprintf(
        stderr,
        "[SL EARLY] slInit result=%d device=%p\n",
        (int)result,
        (void*)g_device
    );

    fflush(stderr);


    if(result != sl::Result::eOk)
    {
        return;
    }


    g_slInitialized = true;


    fprintf(
        stderr,
        "[SL EARLY] INITIALIZED SUCCESSFULLY\n"
    );

    fflush(stderr);

sl::FeatureRequirements requirements{};


sl::Result requirementsResult =
    g_slGetFeatureRequirements(
        sl::kFeatureDLSS,
        requirements
    );


fprintf(
    stderr,
    "[SL REQ] slGetFeatureRequirements(DLSS) result=%d\n",
    (int)requirementsResult
);

fflush(stderr);


if(requirementsResult == sl::Result::eOk)
{
    fprintf(
        stderr,
        "[SL REQ QUEUES] graphics=%u compute=%u opticalFlow=%u\n",
        requirements.vkNumGraphicsQueuesRequired,
        requirements.vkNumComputeQueuesRequired,
        requirements.vkNumOpticalFlowQueuesRequired
    );


    fprintf(
        stderr,
        "[SL REQ INSTANCE EXT] count=%u\n",
        requirements.vkNumInstanceExtensions
    );


    for(uint32_t i = 0;
        i < requirements.vkNumInstanceExtensions;
        ++i)
    {
        const char* extensionName =
            requirements.vkInstanceExtensions
            ? requirements.vkInstanceExtensions[i]
            : nullptr;

        fprintf(
            stderr,
            "[SL REQ INSTANCE EXT %u] %s\n",
            i,
            extensionName
                ? extensionName
                : "<null>"
        );
    }


    fprintf(
        stderr,
        "[SL REQ DEVICE EXT] count=%u\n",
        requirements.vkNumDeviceExtensions
    );


    for(uint32_t i = 0;
        i < requirements.vkNumDeviceExtensions;
        ++i)
    {
        const char* extensionName =
            requirements.vkDeviceExtensions
            ? requirements.vkDeviceExtensions[i]
            : nullptr;

        fprintf(
            stderr,
            "[SL REQ DEVICE EXT %u] %s\n",
            i,
            extensionName
                ? extensionName
                : "<null>"
        );
    }


    fprintf(
        stderr,
        "[SL REQ VK12] count=%u\n",
        requirements.vkNumFeatures12
    );


    for(uint32_t i = 0;
        i < requirements.vkNumFeatures12;
        ++i)
    {
        const char* featureName =
            requirements.vkFeatures12
            ? requirements.vkFeatures12[i]
            : nullptr;

        fprintf(
            stderr,
            "[SL REQ VK12 %u] %s\n",
            i,
            featureName
                ? featureName
                : "<null>"
        );
    }


    fprintf(
        stderr,
        "[SL REQ VK13] count=%u\n",
        requirements.vkNumFeatures13
    );


    for(uint32_t i = 0;
        i < requirements.vkNumFeatures13;
        ++i)
    {
        const char* featureName =
            requirements.vkFeatures13
            ? requirements.vkFeatures13[i]
            : nullptr;

        fprintf(
            stderr,
            "[SL REQ VK13 %u] %s\n",
            i,
            featureName
                ? featureName
                : "<null>"
        );
    }


    fflush(stderr);
}
else
{
    fprintf(
        stderr,
        "[SL REQ] FAILED - do not modify Vulkan requirements yet\n"
    );

    fflush(stderr);
}
sl::FeatureRequirements fgRequirements{};


sl::Result fgRequirementsResult =
    g_slGetFeatureRequirements(
        sl::kFeatureDLSS_G,
        fgRequirements
    );


fprintf(
    stderr,
    "[FG-2.1 REQ]"
    " result=%d"
    " flags=0x%llX"
    " maxViewports=%u\n",

    static_cast<int>(
        fgRequirementsResult
    ),

    static_cast<unsigned long long>(
        fgRequirements.flags
    ),

    fgRequirements.maxNumViewports
);

fflush(stderr);


if(fgRequirementsResult == sl::Result::eOk)
{

g_fgRequirementsReady =
    false;


g_fgInstanceExtensions.clear();
g_fgDeviceExtensions.clear();
g_fgFeatures12.clear();
g_fgFeatures13.clear();


g_fgGraphicsQueuesRequired =
    fgRequirements.vkNumGraphicsQueuesRequired;

g_fgComputeQueuesRequired =
    fgRequirements.vkNumComputeQueuesRequired;

g_fgOpticalFlowQueuesRequired =
    fgRequirements.vkNumOpticalFlowQueuesRequired;



if(fgRequirements.vkInstanceExtensions)
{
    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumInstanceExtensions;
        ++i
    )
    {
        const char* name =
            fgRequirements.vkInstanceExtensions[i];


        if(name && name[0] != '\0')
        {
            g_fgInstanceExtensions.emplace_back(
                name
            );
        }
    }
}



if(fgRequirements.vkDeviceExtensions)
{
    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumDeviceExtensions;
        ++i
    )
    {
        const char* name =
            fgRequirements.vkDeviceExtensions[i];


        if(name && name[0] != '\0')
        {
            g_fgDeviceExtensions.emplace_back(
                name
            );
        }
    }
}



if(fgRequirements.vkFeatures12)
{
    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumFeatures12;
        ++i
    )
    {
        const char* name =
            fgRequirements.vkFeatures12[i];


        if(name && name[0] != '\0')
        {
            g_fgFeatures12.emplace_back(
                name
            );
        }
    }
}



if(fgRequirements.vkFeatures13)
{
    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumFeatures13;
        ++i
    )
    {
        const char* name =
            fgRequirements.vkFeatures13[i];


        if(name && name[0] != '\0')
        {
            g_fgFeatures13.emplace_back(
                name
            );
        }
    }
}


g_fgRequirementsReady =
    true;


fprintf(
    stderr,
    "[FG-2.4 SNAPSHOT]"
    " ready=1"
    " instanceExt=%zu"
    " deviceExt=%zu"
    " vk12=%zu"
    " vk13=%zu"
    " graphics=%u"
    " compute=%u"
    " opticalFlow=%u\n",

    g_fgInstanceExtensions.size(),
    g_fgDeviceExtensions.size(),
    g_fgFeatures12.size(),
    g_fgFeatures13.size(),

    g_fgGraphicsQueuesRequired,
    g_fgComputeQueuesRequired,
    g_fgOpticalFlowQueuesRequired
);

fflush(stderr);
    fprintf(
        stderr,
        "[FG-2.1 QUEUES]"
        " graphics=%u"
        " compute=%u"
        " opticalFlow=%u\n",

        fgRequirements.vkNumGraphicsQueuesRequired,
        fgRequirements.vkNumComputeQueuesRequired,
        fgRequirements.vkNumOpticalFlowQueuesRequired
    );



    fprintf(
        stderr,
        "[FG-2.1 INSTANCE EXT]"
        " count=%u\n",

        fgRequirements.vkNumInstanceExtensions
    );


    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumInstanceExtensions;
        ++i
    )
    {
        const char* extensionName =
            fgRequirements.vkInstanceExtensions
                ? fgRequirements.vkInstanceExtensions[i]
                : nullptr;


        fprintf(
            stderr,
            "[FG-2.1 INSTANCE EXT %u] %s\n",

            i,

            extensionName
                ? extensionName
                : "<null>"
        );
    }



    fprintf(
        stderr,
        "[FG-2.1 DEVICE EXT]"
        " count=%u\n",

        fgRequirements.vkNumDeviceExtensions
    );


    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumDeviceExtensions;
        ++i
    )
    {
        const char* extensionName =
            fgRequirements.vkDeviceExtensions
                ? fgRequirements.vkDeviceExtensions[i]
                : nullptr;


        fprintf(
            stderr,
            "[FG-2.1 DEVICE EXT %u] %s\n",

            i,

            extensionName
                ? extensionName
                : "<null>"
        );
    }



    fprintf(
        stderr,
        "[FG-2.1 VK12]"
        " count=%u\n",

        fgRequirements.vkNumFeatures12
    );


    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumFeatures12;
        ++i
    )
    {
        const char* featureName =
            fgRequirements.vkFeatures12
                ? fgRequirements.vkFeatures12[i]
                : nullptr;


        fprintf(
            stderr,
            "[FG-2.1 VK12 %u] %s\n",

            i,

            featureName
                ? featureName
                : "<null>"
        );
    }



    fprintf(
        stderr,
        "[FG-2.1 VK13]"
        " count=%u\n",

        fgRequirements.vkNumFeatures13
    );


    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumFeatures13;
        ++i
    )
    {
        const char* featureName =
            fgRequirements.vkFeatures13
                ? fgRequirements.vkFeatures13[i]
                : nullptr;


        fprintf(
            stderr,
            "[FG-2.1 VK13 %u] %s\n",

            i,

            featureName
                ? featureName
                : "<null>"
        );
    }


    fflush(stderr);
}
else
{g_fgRequirementsReady =
    false;

    g_fgInstanceExtensions.clear();
    g_fgDeviceExtensions.clear();
    g_fgFeatures12.clear();
    g_fgFeatures13.clear();

    g_fgGraphicsQueuesRequired = 0;
    g_fgComputeQueuesRequired = 0;
    g_fgOpticalFlowQueuesRequired = 0;
    fprintf(
        stderr,
        "[FG-2.1 REQ FAILED]"
        " result=%d\n",

        static_cast<int>(
            fgRequirementsResult
        )
    );

    fflush(stderr);
}

sl::FeatureRequirements rrRequirements{};


sl::Result rrRequirementsResult =
    g_slGetFeatureRequirements(
        sl::kFeatureDLSS_RR,
        rrRequirements
    );


fprintf(
    stderr,
    "[SL RR REQ] slGetFeatureRequirements(DLSS_RR) result=%d\n",
    static_cast<int>(
        rrRequirementsResult
    )
);

fflush(stderr);


if(rrRequirementsResult == sl::Result::eOk)
{
    fprintf(
        stderr,
        "[SL RR REQ QUEUES]"
        " graphics=%u"
        " compute=%u"
        " opticalFlow=%u\n",
        rrRequirements.vkNumGraphicsQueuesRequired,
        rrRequirements.vkNumComputeQueuesRequired,
        rrRequirements.vkNumOpticalFlowQueuesRequired
    );


    fprintf(
        stderr,
        "[SL RR REQ INSTANCE EXT] count=%u\n",
        rrRequirements.vkNumInstanceExtensions
    );


    for(
        uint32_t i = 0;
        i < rrRequirements.vkNumInstanceExtensions;
        ++i
    )
    {
        const char* extensionName =
            rrRequirements.vkInstanceExtensions
                ? rrRequirements.vkInstanceExtensions[i]
                : nullptr;


        fprintf(
            stderr,
            "[SL RR REQ INSTANCE EXT %u] %s\n",
            i,
            extensionName
                ? extensionName
                : "<null>"
        );
    }


    fprintf(
        stderr,
        "[SL RR REQ DEVICE EXT] count=%u\n",
        rrRequirements.vkNumDeviceExtensions
    );


    for(
        uint32_t i = 0;
        i < rrRequirements.vkNumDeviceExtensions;
        ++i
    )
    {
        const char* extensionName =
            rrRequirements.vkDeviceExtensions
                ? rrRequirements.vkDeviceExtensions[i]
                : nullptr;


        fprintf(
            stderr,
            "[SL RR REQ DEVICE EXT %u] %s\n",
            i,
            extensionName
                ? extensionName
                : "<null>"
        );
    }


    fprintf(
        stderr,
        "[SL RR REQ VK12] count=%u\n",
        rrRequirements.vkNumFeatures12
    );


    for(
        uint32_t i = 0;
        i < rrRequirements.vkNumFeatures12;
        ++i
    )
    {
        const char* featureName =
            rrRequirements.vkFeatures12
                ? rrRequirements.vkFeatures12[i]
                : nullptr;


        fprintf(
            stderr,
            "[SL RR REQ VK12 %u] %s\n",
            i,
            featureName
                ? featureName
                : "<null>"
        );
    }


    fprintf(
        stderr,
        "[SL RR REQ VK13] count=%u\n",
        rrRequirements.vkNumFeatures13
    );


    for(
        uint32_t i = 0;
        i < rrRequirements.vkNumFeatures13;
        ++i
    )
    {
        const char* featureName =
            rrRequirements.vkFeatures13
                ? rrRequirements.vkFeatures13[i]
                : nullptr;


        fprintf(
            stderr,
            "[SL RR REQ VK13 %u] %s\n",
            i,
            featureName
                ? featureName
                : "<null>"
        );
    }


    fflush(stderr);
}
else
{
    fprintf(
        stderr,
        "[SL RR REQ] FAILED"
        " - do not modify Vulkan requirements yet\n"
    );

    fflush(stderr);

sl::FeatureRequirements fgRequirements{};


sl::Result fgRequirementsResult =
    g_slGetFeatureRequirements(
        sl::kFeatureDLSS_G,
        fgRequirements
    );


fprintf(
    stderr,
    "[FG-2.1 REQ]"
    " result=%d"
    " flags=0x%llX"
    " maxViewports=%u\n",

    static_cast<int>(
        fgRequirementsResult
    ),

    static_cast<unsigned long long>(
        fgRequirements.flags
    ),

    fgRequirements.maxNumViewports
);

fflush(stderr);


if(fgRequirementsResult == sl::Result::eOk)
{

    fprintf(
        stderr,
        "[FG-2.1 QUEUES]"
        " graphics=%u"
        " compute=%u"
        " opticalFlow=%u\n",

        fgRequirements.vkNumGraphicsQueuesRequired,
        fgRequirements.vkNumComputeQueuesRequired,
        fgRequirements.vkNumOpticalFlowQueuesRequired
    );



    fprintf(
        stderr,
        "[FG-2.1 INSTANCE EXT]"
        " count=%u\n",

        fgRequirements.vkNumInstanceExtensions
    );


    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumInstanceExtensions;
        ++i
    )
    {
        const char* extensionName =
            fgRequirements.vkInstanceExtensions
                ? fgRequirements.vkInstanceExtensions[i]
                : nullptr;


        fprintf(
            stderr,
            "[FG-2.1 INSTANCE EXT %u] %s\n",

            i,

            extensionName
                ? extensionName
                : "<null>"
        );
    }



    fprintf(
        stderr,
        "[FG-2.1 DEVICE EXT]"
        " count=%u\n",

        fgRequirements.vkNumDeviceExtensions
    );


    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumDeviceExtensions;
        ++i
    )
    {
        const char* extensionName =
            fgRequirements.vkDeviceExtensions
                ? fgRequirements.vkDeviceExtensions[i]
                : nullptr;


        fprintf(
            stderr,
            "[FG-2.1 DEVICE EXT %u] %s\n",

            i,

            extensionName
                ? extensionName
                : "<null>"
        );
    }



    fprintf(
        stderr,
        "[FG-2.1 VK12]"
        " count=%u\n",

        fgRequirements.vkNumFeatures12
    );


    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumFeatures12;
        ++i
    )
    {
        const char* featureName =
            fgRequirements.vkFeatures12
                ? fgRequirements.vkFeatures12[i]
                : nullptr;


        fprintf(
            stderr,
            "[FG-2.1 VK12 %u] %s\n",

            i,

            featureName
                ? featureName
                : "<null>"
        );
    }



    fprintf(
        stderr,
        "[FG-2.1 VK13]"
        " count=%u\n",

        fgRequirements.vkNumFeatures13
    );


    for(
        uint32_t i = 0;
        i < fgRequirements.vkNumFeatures13;
        ++i
    )
    {
        const char* featureName =
            fgRequirements.vkFeatures13
                ? fgRequirements.vkFeatures13[i]
                : nullptr;


        fprintf(
            stderr,
            "[FG-2.1 VK13 %u] %s\n",

            i,

            featureName
                ? featureName
                : "<null>"
        );
    }


    fflush(stderr);
}
else
{
    fprintf(
        stderr,
        "[FG-2.1 REQ FAILED]"
        " result=%d\n",

        static_cast<int>(
            fgRequirementsResult
        )
    );

    fflush(stderr);
}}

}

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_setFGBackbufferFormat(
    JNIEnv*,
    jclass,
    jint vkFormat
)
{
    const VkFormat newFormat =
        static_cast<VkFormat>(
            vkFormat
        );


    if(
        newFormat ==
        VK_FORMAT_UNDEFINED
    )
    {
        fprintf(
            stderr,
            "[FG-4.1 FORMAT] invalid=0\n"
        );

        fflush(stderr);

        return;
    }


    if(
        g_fgBackbufferFormat !=
        newFormat
    )
    {
        g_fgBackbufferFormat =
            newFormat;


        fprintf(
            stderr,
            "[FG-4.1 FORMAT]"
            " backbufferFormat=%d\n",

            static_cast<int>(
                g_fgBackbufferFormat
            )
        );

        fflush(stderr);
    }
}



extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_captureFGFinalAndBuildUIAlpha(
    JNIEnv*,
    jclass,
    jlong commandBufferHandle,
    jlong finalColorImageHandle,
    jint width,
    jint height
)
{
    if(
        commandBufferHandle == 0 ||

        finalColorImageHandle == 0 ||

        width <= 0 ||

        height <= 0
    )
    {
        return;
    }


    if(
        !g_currentSLFrameValid ||

        g_fgHudlessFrameIndex !=
            g_currentSLFrameIndex
    )
    {
        return;
    }


    const uint32_t finalWidth =
        static_cast<uint32_t>(
            width
        );

    const uint32_t finalHeight =
        static_cast<uint32_t>(
            height
        );


    if(
        finalWidth !=
            g_fgUIWidth ||

        finalHeight !=
            g_fgUIHeight ||

        g_fgHudlessImage ==
            VK_NULL_HANDLE ||

        g_fgFinalImage ==
            VK_NULL_HANDLE ||

        g_fgUIAlphaImage ==
            VK_NULL_HANDLE ||

        g_fgUIAlphaPipeline ==
            VK_NULL_HANDLE ||

        g_fgUIAlphaPipelineLayout ==
            VK_NULL_HANDLE ||

        g_fgUIAlphaDescriptorSet ==
            VK_NULL_HANDLE
    )
    {
        return;
    }


    VkCommandBuffer cmd =
        reinterpret_cast<VkCommandBuffer>(
            static_cast<uintptr_t>(
                commandBufferHandle
            )
        );


    VkImage finalColorImage =
        reinterpret_cast<VkImage>(
            static_cast<uintptr_t>(
                finalColorImageHandle
            )
        );



    VkImageMemoryBarrier
        finalToTransfer{};

    finalToTransfer.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    finalToTransfer.oldLayout =
        g_fgFinalLayout;

    finalToTransfer.newLayout =
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    finalToTransfer.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    finalToTransfer.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    finalToTransfer.srcAccessMask =
        g_fgFinalLayout ==
            VK_IMAGE_LAYOUT_UNDEFINED
                ? 0
                : VK_ACCESS_SHADER_READ_BIT;

    finalToTransfer.dstAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT;

    finalToTransfer.image =
        g_fgFinalImage;

    finalToTransfer.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    finalToTransfer.subresourceRange.baseMipLevel =
        0;

    finalToTransfer.subresourceRange.levelCount =
        1;

    finalToTransfer.subresourceRange.baseArrayLayer =
        0;

    finalToTransfer.subresourceRange.layerCount =
        1;


    vkCmdPipelineBarrier(
        cmd,

        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,

        0,

        0,
        nullptr,

        0,
        nullptr,

        1,
        &finalToTransfer
    );


    g_fgFinalLayout =
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;



    VkImageBlit finalBlit{};


    finalBlit.srcSubresource.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    finalBlit.srcSubresource.mipLevel =
        0;

    finalBlit.srcSubresource.baseArrayLayer =
        0;

    finalBlit.srcSubresource.layerCount =
        1;


    finalBlit.srcOffsets[0] =
    {
        0,
        0,
        0
    };

    finalBlit.srcOffsets[1] =
    {
        static_cast<int32_t>(
            finalWidth
        ),

        static_cast<int32_t>(
            finalHeight
        ),

        1
    };


    finalBlit.dstSubresource.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    finalBlit.dstSubresource.mipLevel =
        0;

    finalBlit.dstSubresource.baseArrayLayer =
        0;

    finalBlit.dstSubresource.layerCount =
        1;


    finalBlit.dstOffsets[0] =
    {
        0,

        static_cast<int32_t>(
            finalHeight
        ),

        0
    };

    finalBlit.dstOffsets[1] =
    {
        static_cast<int32_t>(
            finalWidth
        ),

        0,

        1
    };


    vkCmdBlitImage(
        cmd,

        finalColorImage,
        VK_IMAGE_LAYOUT_GENERAL,

        g_fgFinalImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,

        1,
        &finalBlit,

        VK_FILTER_NEAREST
    );



    VkImageMemoryBarrier
        computeBarriers[3]{};


    computeBarriers[0].sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    computeBarriers[0].oldLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    computeBarriers[0].newLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    computeBarriers[0].srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    computeBarriers[0].dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    computeBarriers[0].srcAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT;

    computeBarriers[0].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT;

    computeBarriers[0].image =
        g_fgHudlessImage;

    computeBarriers[0].subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    computeBarriers[0].subresourceRange.baseMipLevel =
        0;

    computeBarriers[0].subresourceRange.levelCount =
        1;

    computeBarriers[0].subresourceRange.baseArrayLayer =
        0;

    computeBarriers[0].subresourceRange.layerCount =
        1;


    computeBarriers[1].sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    computeBarriers[1].oldLayout =
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    computeBarriers[1].newLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    computeBarriers[1].srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    computeBarriers[1].dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    computeBarriers[1].srcAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT;

    computeBarriers[1].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT;

    computeBarriers[1].image =
        g_fgFinalImage;

    computeBarriers[1].subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    computeBarriers[1].subresourceRange.baseMipLevel =
        0;

    computeBarriers[1].subresourceRange.levelCount =
        1;

    computeBarriers[1].subresourceRange.baseArrayLayer =
        0;

    computeBarriers[1].subresourceRange.layerCount =
        1;


    computeBarriers[2].sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    computeBarriers[2].oldLayout =
        g_fgUIAlphaLayout;

    computeBarriers[2].newLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    computeBarriers[2].srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    computeBarriers[2].dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    computeBarriers[2].srcAccessMask =
        g_fgUIAlphaLayout ==
            VK_IMAGE_LAYOUT_UNDEFINED
                ? 0
                : VK_ACCESS_SHADER_WRITE_BIT;

    computeBarriers[2].dstAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT;

    computeBarriers[2].image =
        g_fgUIAlphaImage;

    computeBarriers[2].subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    computeBarriers[2].subresourceRange.baseMipLevel =
        0;

    computeBarriers[2].subresourceRange.levelCount =
        1;

    computeBarriers[2].subresourceRange.baseArrayLayer =
        0;

    computeBarriers[2].subresourceRange.layerCount =
        1;


    vkCmdPipelineBarrier(
        cmd,

        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,

        0,

        0,
        nullptr,

        0,
        nullptr,

        3,
        computeBarriers
    );


    g_fgFinalLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    g_fgUIAlphaLayout =
        VK_IMAGE_LAYOUT_GENERAL;



    vkCmdBindPipeline(
        cmd,

        VK_PIPELINE_BIND_POINT_COMPUTE,

        g_fgUIAlphaPipeline
    );


    vkCmdBindDescriptorSets(
        cmd,

        VK_PIPELINE_BIND_POINT_COMPUTE,

        g_fgUIAlphaPipelineLayout,

        0,
        1,
        &g_fgUIAlphaDescriptorSet,

        0,
        nullptr
    );


    vkCmdDispatch(
        cmd,

        (
            finalWidth +
            7
        ) / 8,

        (
            finalHeight +
            7
        ) / 8,

        1
    );



    VkImageMemoryBarrier
        alphaReady{};

    alphaReady.sType =
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

    alphaReady.oldLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    alphaReady.newLayout =
        VK_IMAGE_LAYOUT_GENERAL;

    alphaReady.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    alphaReady.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;

    alphaReady.srcAccessMask =
        VK_ACCESS_SHADER_WRITE_BIT;

    alphaReady.dstAccessMask =
        VK_ACCESS_MEMORY_READ_BIT;

    alphaReady.image =
        g_fgUIAlphaImage;

    alphaReady.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    alphaReady.subresourceRange.baseMipLevel =
        0;

    alphaReady.subresourceRange.levelCount =
        1;

    alphaReady.subresourceRange.baseArrayLayer =
        0;

    alphaReady.subresourceRange.layerCount =
        1;


    vkCmdPipelineBarrier(
        cmd,

        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,

        0,

        0,
        nullptr,

        0,
        nullptr,

        1,
        &alphaReady
    );


    g_fgUIAlphaFrameIndex =
        g_currentSLFrameIndex;

if(
    g_slSetTagForFrame &&
    g_currentSLFrameToken != nullptr &&
    g_fgHudlessFrameIndex == g_currentSLFrameIndex &&
    g_fgUIAlphaFrameIndex == g_currentSLFrameIndex
)
{

    sl::Resource fgDepth(
        sl::ResourceType::eTex2d,
        (void*)g_dlssDepthImage,
        (void*)g_dlssDepthMemory,
        (void*)g_dlssDepthView,
        VK_IMAGE_LAYOUT_GENERAL
    );

    fgDepth.width =
        g_dlssDepthWidth;

    fgDepth.height =
        g_dlssDepthHeight;

    fgDepth.nativeFormat =
        static_cast<uint32_t>(
            VK_FORMAT_R32_SFLOAT
        );

    fgDepth.mipLevels = 1;
    fgDepth.arrayLayers = 1;
    fgDepth.flags = 0;

    fgDepth.usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;



    sl::Resource fgMotion(
        sl::ResourceType::eTex2d,
        (void*)g_motionImage,
        (void*)g_motionMemory,
        (void*)g_motionView,
        VK_IMAGE_LAYOUT_GENERAL
    );

    fgMotion.width =
        g_motionWidth;

    fgMotion.height =
        g_motionHeight;

    fgMotion.nativeFormat =
        static_cast<uint32_t>(
            VK_FORMAT_R16G16_SFLOAT
        );

    fgMotion.mipLevels = 1;
    fgMotion.arrayLayers = 1;
    fgMotion.flags = 0;

    fgMotion.usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;



    sl::Resource fgHudless(
        sl::ResourceType::eTex2d,
        (void*)g_fgHudlessImage,
        (void*)g_fgHudlessMemory,
        (void*)g_fgHudlessView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    );

    fgHudless.width =
        g_fgUIWidth;

    fgHudless.height =
        g_fgUIHeight;

    fgHudless.nativeFormat =
        static_cast<uint32_t>(
            g_fgSnapshotFormat
        );

    fgHudless.mipLevels = 1;
    fgHudless.arrayLayers = 1;
    fgHudless.flags = 0;

    fgHudless.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;



    sl::Resource fgUIAlpha(
        sl::ResourceType::eTex2d,
        (void*)g_fgUIAlphaImage,
        (void*)g_fgUIAlphaMemory,
        (void*)g_fgUIAlphaView,
        VK_IMAGE_LAYOUT_GENERAL
    );

    fgUIAlpha.width =
        g_fgUIWidth;

    fgUIAlpha.height =
        g_fgUIHeight;

    fgUIAlpha.nativeFormat =
        static_cast<uint32_t>(
            VK_FORMAT_R32_SFLOAT
        );

    fgUIAlpha.mipLevels = 1;
    fgUIAlpha.arrayLayers = 1;
    fgUIAlpha.flags = 0;

    fgUIAlpha.usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;



    sl::Extent renderExtent{};

    renderExtent.left = 0;
    renderExtent.top = 0;
    renderExtent.width =
        g_dlssDepthWidth;
    renderExtent.height =
        g_dlssDepthHeight;


    sl::Extent finalExtent{};

    finalExtent.left = 0;
    finalExtent.top = 0;
    finalExtent.width =
        g_fgUIWidth;
    finalExtent.height =
        g_fgUIHeight;



    sl::ResourceTag depthTag(
        &fgDepth,
        sl::kBufferTypeDepth,
        sl::ResourceLifecycle::eValidUntilPresent,
        &renderExtent
    );

    sl::ResourceTag motionTag(
        &fgMotion,
        sl::kBufferTypeMotionVectors,
        sl::ResourceLifecycle::eValidUntilPresent,
        &renderExtent
    );

    sl::ResourceTag hudlessTag(
        &fgHudless,
        sl::kBufferTypeHUDLessColor,
        sl::ResourceLifecycle::eValidUntilPresent,
        &finalExtent
    );

    sl::ResourceTag uiAlphaTag(
        &fgUIAlpha,
        sl::kBufferTypeUIAlpha,
        sl::ResourceLifecycle::eValidUntilPresent,
        &finalExtent
    );


    sl::ResourceTag fgTags[] =
    {
        depthTag,
        motionTag,
        hudlessTag,
        uiAlphaTag
    };


    sl::Result tagResult =
        g_slSetTagForFrame(
            *g_currentSLFrameToken,
            g_dlssViewport,
            fgTags,
            4,
            nullptr
        );


    static bool printed =
        false;

    if(!printed ||
       tagResult != sl::Result::eOk)
    {
        fprintf(
            stderr,
            "[FG-4.2 TAG]"
            " frame=%u"
            " result=%d"
            " depth=%ux%u"
            " motion=%ux%u"
            " hudless=%ux%u"
            " uiAlpha=%ux%u\n",

            g_currentSLFrameIndex,
            static_cast<int>(tagResult),

            g_dlssDepthWidth,
            g_dlssDepthHeight,

            g_motionWidth,
            g_motionHeight,

            g_fgUIWidth,
            g_fgUIHeight,

            g_fgUIWidth,
            g_fgUIHeight
        );

        fflush(stderr);

        printed = true;
    }


    g_fgTagsValid = tagResult == sl::Result::eOk;
}
else if(
    g_slSetTagForFrame &&
    g_currentSLFrameToken != nullptr &&
    g_fgTagsValid &&
    (!g_isInWorld || g_fgFullscreenMenuOpen.load())
)
{
    sl::ResourceTag clearTags[] =
    {
        sl::ResourceTag{nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent},
        sl::ResourceTag{nullptr, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent}
    };

    sl::Result clearResult =
        g_slSetTagForFrame(
            *g_currentSLFrameToken,
            g_dlssViewport,
            clearTags,
            4,
            nullptr
        );

    static bool printedClear = false;

    if(!printedClear || clearResult != sl::Result::eOk)
    {
        fprintf(
            stderr,
            "[FG-4.2 TAG CLEAR] frame=%u result=%d inWorld=%d menu=%d\n",
            g_currentSLFrameIndex,
            static_cast<int>(clearResult),
            g_isInWorld ? 1 : 0,
            g_fgFullscreenMenuOpen.load() ? 1 : 0
        );

        fflush(stderr);

        printedClear = true;
    }

    g_fgTagsValid = false;
}


    static uint32_t
        lastPrintedWidth =
            0;

    static uint32_t
        lastPrintedHeight =
            0;


    if(
        lastPrintedWidth !=
            finalWidth ||

        lastPrintedHeight !=
            finalHeight
    )
    {
        fprintf(
            stderr,
            "[FG-4.1 UI ALPHA]"
            " frame=%u"
            " size=%ux%u"
            " binaryRGBCompare=1\n",

            g_currentSLFrameIndex,

            finalWidth,
            finalHeight
        );

        fflush(stderr);


        lastPrintedWidth =
            finalWidth;

        lastPrintedHeight =
            finalHeight;
    }

}
float DLSSHaltonValue(
    uint32_t index,
    uint32_t base
)
{
    float f = 1.0f;
    float result = 0.0f;

    uint32_t i = index;

    while(i > 0u)
    {
        f /= static_cast<float>(base);

        result +=
            f *
            static_cast<float>(
                i % base
            );

        i /= base;
    }

    return result;
}


void GetDLSSFrameJitter(
    uint32_t frameIndex,
    float& outX,
    float& outY
)
{
#ifndef RESTIR_TEST_NO_JITTER
#define RESTIR_TEST_NO_JITTER 0
#endif

    static const bool runtimeNoJitter = []
    {
        const char* value = std::getenv("RESTIR_NO_JITTER");

        if(value != nullptr && value[0] != '\0' && value[0] != '0')
        {
            RTLog("[JITTER] no-jitter test ACTIVE (env RESTIR_NO_JITTER)");
            return true;
        }

        std::ifstream marker(
            ResolveRTXRuntimePath("restir_no_jitter.flag"),
            std::ios::binary
        );

        if(marker.good())
        {
            RTLog(
                std::string("[JITTER] no-jitter test ACTIVE (flag file: ") +
                ResolveRTXRuntimePath("restir_no_jitter.flag") + ")"
            );
            return true;
        }

        RTLog("[JITTER] no-jitter test inactive (jitter on)");
        return false;
    }();

#if RESTIR_TEST_NO_JITTER
    const bool noJitter = true;
#else
    const bool noJitter = runtimeNoJitter;
#endif

    if(noJitter)
    {
        outX = 0.0f;
        outY = 0.0f;
        return;
    }

    constexpr uint32_t PHASE_COUNT =
        32u;

    const uint32_t sequenceIndex =
        (frameIndex % PHASE_COUNT) +
        1u;


    outX =
        DLSSHaltonValue(
            sequenceIndex,
            2u
        ) -
        0.5f;


    outY =
        DLSSHaltonValue(
            sequenceIndex,
            3u
        ) -
        0.5f;
}
