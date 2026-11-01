// Orion backend — Cleanup 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_cleanup(JNIEnv*,jclass){
    stopGeometryCaptureWorker();

    if(g_device){
        FGDeviceWaitIdle(g_device);

        destroyPerfL10GpuTimestampPool();

        destroyPerfL10RayStats();

        releaseAllRetirementSlots();

        destroySectionRetirementFences();

        releaseDeferredSectionBuildResources();
        destroyPooledSectionBuildBuffers();

        releaseTextureUploadScratch();

        if(g_rtCommandPool)
        {
            vkDestroyCommandPool(
                g_device,
                g_rtCommandPool,
                nullptr
            );
            g_rtCommandPool=VK_NULL_HANDLE;
        }
        if(g_rtOutputView){vkDestroyImageView(g_device,g_rtOutputView,nullptr);g_rtOutputView=VK_NULL_HANDLE;}
        if(g_rtOutputImage){vkDestroyImage(g_device,g_rtOutputImage,nullptr);g_rtOutputImage=VK_NULL_HANDLE;}
        if(g_rtOutputMemory){vkFreeMemory(g_device,g_rtOutputMemory,nullptr);g_rtOutputMemory=VK_NULL_HANDLE;}




        destroyHistoryImage(
            g_rrDiffuseAlbedoImage,
            g_rrDiffuseAlbedoMemory,
            g_rrDiffuseAlbedoView,
            g_rrDiffuseAlbedoLayout
        );


        destroyHistoryImage(
            g_rrSpecularAlbedoImage,
            g_rrSpecularAlbedoMemory,
            g_rrSpecularAlbedoView,
            g_rrSpecularAlbedoLayout
        );


        destroyHistoryImage(
            g_rrNormalRoughnessImage,
            g_rrNormalRoughnessMemory,
            g_rrNormalRoughnessView,
            g_rrNormalRoughnessLayout
        );


        destroyHistoryImage(
            g_rrSpecularHitDistanceImage,
            g_rrSpecularHitDistanceMemory,
            g_rrSpecularHitDistanceView,
            g_rrSpecularHitDistanceLayout
        );


        if(g_glassBlueNoiseBuffer)
        {
            vkDestroyBuffer(g_device, g_glassBlueNoiseBuffer, nullptr);
            g_glassBlueNoiseBuffer = VK_NULL_HANDLE;
        }

        if(g_glassBlueNoiseMemory)
        {
            vkFreeMemory(g_device, g_glassBlueNoiseMemory, nullptr);
            g_glassBlueNoiseMemory = VK_NULL_HANDLE;
        }

        g_glassBlueNoiseReady = false;

        if(g_sparseActiveMaskPipeline)
        {
            vkDestroyPipeline(g_device, g_sparseActiveMaskPipeline, nullptr);
            g_sparseActiveMaskPipeline = VK_NULL_HANDLE;
        }

        if(g_sparseActiveMaskPipelineLayout)
        {
            vkDestroyPipelineLayout(g_device, g_sparseActiveMaskPipelineLayout, nullptr);
            g_sparseActiveMaskPipelineLayout = VK_NULL_HANDLE;
        }

        if(g_sparseActiveMaskPool)
        {
            vkDestroyDescriptorPool(g_device, g_sparseActiveMaskPool, nullptr);
            g_sparseActiveMaskPool = VK_NULL_HANDLE;
            g_sparseActiveMaskSet = VK_NULL_HANDLE;
        }

        if(g_sparseActiveMaskSetLayout)
        {
            vkDestroyDescriptorSetLayout(g_device, g_sparseActiveMaskSetLayout, nullptr);
            g_sparseActiveMaskSetLayout = VK_NULL_HANDLE;
        }

        if(g_sparseActiveMaskShader)
        {
            vkDestroyShaderModule(g_device, g_sparseActiveMaskShader, nullptr);
            g_sparseActiveMaskShader = VK_NULL_HANDLE;
        }

        destroySparseActiveMaskImage();

        destroyHistoryImage(
            g_alphaBlendGbufferImage,
            g_alphaBlendGbufferMemory,
            g_alphaBlendGbufferView,
            g_alphaBlendGbufferLayout
        );

        destroyHistoryImage(
            g_alphaBlendMetaImage,
            g_alphaBlendMetaMemory,
            g_alphaBlendMetaView,
            g_alphaBlendMetaLayout
        );

        destroyHistoryImage(
            g_alphaBlendLightImage,
            g_alphaBlendLightMemory,
            g_alphaBlendLightView,
            g_alphaBlendLightLayout
        );

        destroyHistoryImage(
            g_alphaBlendRadianceImage,
            g_alphaBlendRadianceMemory,
            g_alphaBlendRadianceView,
            g_alphaBlendRadianceLayout
        );


        destroyHistoryImage(
            g_rrStableRadianceImage,
            g_rrStableRadianceMemory,
            g_rrStableRadianceView,
            g_rrStableRadianceLayout
        );


        destroyHistoryImage(
            g_giHalfImage,
            g_giHalfMemory,
            g_giHalfView,
            g_giHalfLayout
        );
        g_giHalfWidth = 0u;
        g_giHalfHeight = 0u;
        g_giHalfBoundView = VK_NULL_HANDLE;
        g_giHalfNeedsClear = false;
        destroyGiHalfPipeline();


        if(g_rrStableRadianceSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(
                g_device,
                g_rrStableRadianceSampler,
                nullptr
            );

            g_rrStableRadianceSampler =
                VK_NULL_HANDLE;
        }


        g_rrGBufferWidth =
            0;

        g_rrGBufferHeight =
            0;


        if(g_motionView)
        {
            vkDestroyImageView(
                g_device,
                g_motionView,
                nullptr
            );

            g_motionView =
                VK_NULL_HANDLE;
        }


        if(g_motionImage)
        {
            vkDestroyImage(
                g_device,
                g_motionImage,
                nullptr
            );

            g_motionImage =
                VK_NULL_HANDLE;
        }


        if(g_motionMemory)
        {
            vkFreeMemory(
                g_device,
                g_motionMemory,
                nullptr
            );

            g_motionMemory =
                VK_NULL_HANDLE;
        }


        g_motionWidth = 0;
        g_motionHeight = 0;

        g_motionLayout =
            VK_IMAGE_LAYOUT_UNDEFINED;


        if(g_dlssDepthView)
        {
            vkDestroyImageView(
                g_device,
                g_dlssDepthView,
                nullptr
            );

            g_dlssDepthView =
                VK_NULL_HANDLE;
        }


        if(g_dlssDepthImage)
        {
            vkDestroyImage(
                g_device,
                g_dlssDepthImage,
                nullptr
            );

            g_dlssDepthImage =
                VK_NULL_HANDLE;
        }


        if(g_dlssDepthMemory)
        {
            vkFreeMemory(
                g_device,
                g_dlssDepthMemory,
                nullptr
            );

            g_dlssDepthMemory =
                VK_NULL_HANDLE;
        }


        g_dlssDepthWidth = 0;
        g_dlssDepthHeight = 0;

        g_dlssDepthLayout =
            VK_IMAGE_LAYOUT_UNDEFINED;
        if(g_dlssOutputView)
        {
            vkDestroyImageView(
                g_device,
                g_dlssOutputView,
                nullptr
            );

            g_dlssOutputView =
                VK_NULL_HANDLE;
        }


        if(g_dlssOutputImage)
        {
            vkDestroyImage(
                g_device,
                g_dlssOutputImage,
                nullptr
            );

            g_dlssOutputImage =
                VK_NULL_HANDLE;
        }


        if(g_dlssOutputMemory)
        {
            vkFreeMemory(
                g_device,
                g_dlssOutputMemory,
                nullptr
            );

            g_dlssOutputMemory =
                VK_NULL_HANDLE;
        }


        g_dlssOutputWidth = 0;
        g_dlssOutputHeight = 0;

        g_dlssOutputLayout =
            VK_IMAGE_LAYOUT_UNDEFINED;


        destroyVPTDirectHistoryBuffers();
        destroyVLIResources();
        destroyLocalLightGridGpuBuffer();
        destroyLocalLightGpuBuffer();
        destroySectionVertexAddressBuffer();

        destroySharcBuffers();

        if(g_sharcResolvePipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(g_device, g_sharcResolvePipeline, nullptr);
            g_sharcResolvePipeline = VK_NULL_HANDLE;
        }

        if(g_sharcResolvePipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(
                g_device, g_sharcResolvePipelineLayout, nullptr);
            g_sharcResolvePipelineLayout = VK_NULL_HANDLE;
        }

        g_sharcResolvePipelineFailed = false;


        destroyVolumetricCloudResources();
        if(g_volDepthSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(g_device, g_volDepthSampler, nullptr);
            g_volDepthSampler = VK_NULL_HANDLE;
        }
        destroySunShadowResources();
        destroySunShadowFarResources();
        destroyTerrainHeightResources();
        destroyRRGuideRelaxResources();
        destroyCloudNoiseResources();
        destroyCloudBaseNoiseResources();
        destroyCloudV3Resources();
        destroyCloudV4Resources();
        destroySkyAtmosphereResources();
        destroyWaterDetailNormalResources();
        destroyEntityPbrFallbackResources();
        destroyWaterCausticResources();
        destroyWaterCausticMapResources();
#if 0
        destroyWaterFoamResources();
#endif
        destroyWaterSimulationResources();
        destroyWaterFftResources();
        destroyVoxyLodAtlasResources();

        if(g_rayTracingDescriptorPool){vkDestroyDescriptorPool(g_device,g_rayTracingDescriptorPool,nullptr);}

        if(g_cameraUboBuffer) {
            vkDestroyBuffer(g_device, g_cameraUboBuffer, nullptr);
            g_cameraUboBuffer = VK_NULL_HANDLE;
        }
        if(g_cameraUboMemory) {
            vkFreeMemory(g_device, g_cameraUboMemory, nullptr);
            g_cameraUboMemory = VK_NULL_HANDLE;
            g_cameraUboMapped = nullptr;
        }

        auto fn = getDestroyAccelerationStructure();
        if(g_tlas&&fn) fn(g_device,g_tlas,nullptr);
        g_tlas = VK_NULL_HANDLE;

        for(auto& entry : g_sectionBLAS)
        {
            destroySectionAS(
                entry.second
            );
        }

        g_sectionBLAS.clear();

        destroySectionMemoryPool();

        if(g_blas&&fn) fn(g_device,g_blas,nullptr);
        g_blas = VK_NULL_HANDLE;
        if(g_tlasScratchBuffer)vkDestroyBuffer(g_device,g_tlasScratchBuffer,nullptr);
        if(g_tlasScratchMemory)vkFreeMemory(g_device,g_tlasScratchMemory,nullptr);
        if(g_tlasInstanceBuffer)vkDestroyBuffer(g_device,g_tlasInstanceBuffer,nullptr);
        if(g_tlasInstanceMemory)vkFreeMemory(g_device,g_tlasInstanceMemory,nullptr);
        if(g_tlasInstancesBuffer)vkDestroyBuffer(g_device,g_tlasInstancesBuffer,nullptr);
        if(g_tlasInstancesMemory)vkFreeMemory(g_device,g_tlasInstancesMemory,nullptr);
        if(g_tlasBuffer)vkDestroyBuffer(g_device,g_tlasBuffer,nullptr);
        if(g_tlasMemory)vkFreeMemory(g_device,g_tlasMemory,nullptr);
        if(g_blasScratchBuffer)vkDestroyBuffer(g_device,g_blasScratchBuffer,nullptr);
        if(g_blasScratchMemory)vkFreeMemory(g_device,g_blasScratchMemory,nullptr);
        if(g_blasBuffer)vkDestroyBuffer(g_device,g_blasBuffer,nullptr);
        if(g_blasMemory)vkFreeMemory(g_device,g_blasMemory,nullptr);
        if(g_testVertexBuffer)vkDestroyBuffer(g_device,g_testVertexBuffer,nullptr);
        if(g_testVertexMemory)vkFreeMemory(g_device,g_testVertexMemory,nullptr);
        if(g_sbtBuffer)vkDestroyBuffer(g_device,g_sbtBuffer,nullptr);
destroySharcUpdatePipeline();
        if(g_sbtMemory)vkFreeMemory(g_device,g_sbtMemory,nullptr);
        destroyNRDResources();
        if(g_rayTracingPipeline)vkDestroyPipeline(g_device,g_rayTracingPipeline,nullptr);
        if(g_rayTracingPipelineLayout)vkDestroyPipelineLayout(g_device,g_rayTracingPipelineLayout,nullptr);
        if(g_rayTracingDescriptorSetLayout)vkDestroyDescriptorSetLayout(g_device,g_rayTracingDescriptorSetLayout,nullptr);
        if(g_sdrPostPipeline)
        {
            vkDestroyPipeline(
                g_device,
                g_sdrPostPipeline,
                nullptr
            );

            g_sdrPostPipeline =
                VK_NULL_HANDLE;

        }


        if(g_hdrPostPipeline)
        {
            vkDestroyPipeline(
                g_device,
                g_hdrPostPipeline,
                nullptr
            );

            g_hdrPostPipeline =
                VK_NULL_HANDLE;
        }


        if(g_sdrPostPipelineLayout)
        {
            vkDestroyPipelineLayout(
                g_device,
                g_sdrPostPipelineLayout,
                nullptr
            );

            g_sdrPostPipelineLayout =
                VK_NULL_HANDLE;
        }


        if(g_sdrPostDescriptorPool)
        {
            vkDestroyDescriptorPool(
                g_device,
                g_sdrPostDescriptorPool,
                nullptr
            );

            g_sdrPostDescriptorPool =
                VK_NULL_HANDLE;

            g_sdrPostDescriptorSet =
                VK_NULL_HANDLE;
        }


        if(g_sdrPostDescriptorSetLayout)
        {
            vkDestroyDescriptorSetLayout(
                g_device,
                g_sdrPostDescriptorSetLayout,
                nullptr
            );

            g_sdrPostDescriptorSetLayout =
                VK_NULL_HANDLE;
        }



        if(g_aeHistogramPipeline)
        {
            vkDestroyPipeline(
                g_device,
                g_aeHistogramPipeline,
                nullptr
            );

            g_aeHistogramPipeline =
                VK_NULL_HANDLE;
        }


        if(g_aeAdaptPipeline)
        {
            vkDestroyPipeline(
                g_device,
                g_aeAdaptPipeline,
                nullptr
            );

            g_aeAdaptPipeline =
                VK_NULL_HANDLE;
        }


        if(g_aeHistogramLayout)
        {
            vkDestroyPipelineLayout(
                g_device,
                g_aeHistogramLayout,
                nullptr
            );

            g_aeHistogramLayout =
                VK_NULL_HANDLE;
        }


        if(g_aeAdaptLayout)
        {
            vkDestroyPipelineLayout(
                g_device,
                g_aeAdaptLayout,
                nullptr
            );

            g_aeAdaptLayout =
                VK_NULL_HANDLE;
        }


        if(g_aeHistogramPool)
        {
            vkDestroyDescriptorPool(
                g_device,
                g_aeHistogramPool,
                nullptr
            );

            g_aeHistogramPool =
                VK_NULL_HANDLE;

            g_aeHistogramSet =
                VK_NULL_HANDLE;
        }


        if(g_aeAdaptPool)
        {
            vkDestroyDescriptorPool(
                g_device,
                g_aeAdaptPool,
                nullptr
            );

            g_aeAdaptPool =
                VK_NULL_HANDLE;

            g_aeAdaptSet =
                VK_NULL_HANDLE;
        }


        if(g_aeHistogramSetLayout)
        {
            vkDestroyDescriptorSetLayout(
                g_device,
                g_aeHistogramSetLayout,
                nullptr
            );

            g_aeHistogramSetLayout =
                VK_NULL_HANDLE;
        }


        if(g_aeAdaptSetLayout)
        {
            vkDestroyDescriptorSetLayout(
                g_device,
                g_aeAdaptSetLayout,
                nullptr
            );

            g_aeAdaptSetLayout =
                VK_NULL_HANDLE;
        }


        if(g_aeHistogramBuffer)
        {
            vkDestroyBuffer(
                g_device,
                g_aeHistogramBuffer,
                nullptr
            );

            g_aeHistogramBuffer =
                VK_NULL_HANDLE;
        }


        if(g_aeHistogramMemory)
        {
            vkFreeMemory(
                g_device,
                g_aeHistogramMemory,
                nullptr
            );

            g_aeHistogramMemory =
                VK_NULL_HANDLE;
        }


        if(g_aeStateBuffer)
        {
            vkDestroyBuffer(
                g_device,
                g_aeStateBuffer,
                nullptr
            );

            g_aeStateBuffer =
                VK_NULL_HANDLE;
        }


        if(g_aeStateMemory)
        {
            vkFreeMemory(
                g_device,
                g_aeStateMemory,
                nullptr
            );

            g_aeStateMemory =
                VK_NULL_HANDLE;
        }


        if(g_fgUIAlphaPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(
                g_device,
                g_fgUIAlphaPipeline,
                nullptr
            );

            g_fgUIAlphaPipeline =
                VK_NULL_HANDLE;
        }


        if(g_fgUIAlphaPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(
                g_device,
                g_fgUIAlphaPipelineLayout,
                nullptr
            );

            g_fgUIAlphaPipelineLayout =
                VK_NULL_HANDLE;
        }


        if(g_fgUIAlphaDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(
                g_device,
                g_fgUIAlphaDescriptorPool,
                nullptr
            );

            g_fgUIAlphaDescriptorPool =
                VK_NULL_HANDLE;

            g_fgUIAlphaDescriptorSet =
                VK_NULL_HANDLE;
        }


        if(g_fgUIAlphaDescriptorSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(
                g_device,
                g_fgUIAlphaDescriptorSetLayout,
                nullptr
            );

            g_fgUIAlphaDescriptorSetLayout =
                VK_NULL_HANDLE;
        }


        if(g_fgUISnapshotSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(
                g_device,
                g_fgUISnapshotSampler,
                nullptr
            );

            g_fgUISnapshotSampler =
                VK_NULL_HANDLE;
        }


        destroyFGUIImages();


        g_fgBackbufferFormat =
            VK_FORMAT_UNDEFINED;

        g_fgSnapshotFormat =
            VK_FORMAT_UNDEFINED;

        g_fgUIWidth =
            0;

        g_fgUIHeight =
            0;

        g_fgHudlessFrameIndex =
            UINT32_MAX;

        g_fgUIAlphaFrameIndex =
            UINT32_MAX;
    }
    g_rtOutputWidth=g_rtOutputHeight=0;g_rtOutputLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    g_rayTracingDescriptorPool=VK_NULL_HANDLE;g_rayTracingDescriptorSet=VK_NULL_HANDLE;
    g_rtDescriptorRingReady=false;g_rtDescriptorRingLastPublishUsedRing=false;
    g_rtDescriptorRingPool=VK_NULL_HANDLE;g_rtDescriptorRingCmdPool=VK_NULL_HANDLE;g_rtDescriptorRingCmd=VK_NULL_HANDLE;
    g_rtDescriptorRingLiveIndex=UINT32_MAX;g_rtDescriptorRingNext=0u;
    memset(g_rtDescriptorRingSets,0,sizeof(g_rtDescriptorRingSets));
    memset(g_rtDescriptorRingFences,0,sizeof(g_rtDescriptorRingFences));
    memset(g_rtDescriptorRingFencePending,0,sizeof(g_rtDescriptorRingFencePending));
    g_tlasScratchBuffer=VK_NULL_HANDLE;g_tlasScratchMemory=VK_NULL_HANDLE;g_tlasScratchCapacity=0;g_tlasAsSize=0;g_tlasInstancesCapacity=0;g_tlasInstanceCount=0;g_tlasInstanceBuffer=VK_NULL_HANDLE;g_tlasInstanceMemory=VK_NULL_HANDLE;g_tlasBuffer=VK_NULL_HANDLE;g_tlasMemory=VK_NULL_HANDLE;
    g_tlasInstancesBuffer=VK_NULL_HANDLE;g_tlasInstancesMemory=VK_NULL_HANDLE;g_tlasInstancesAddress=0;
    g_tlasDescriptorHandle=VK_NULL_HANDLE;
    g_blasScratchBuffer=VK_NULL_HANDLE;g_blasScratchMemory=VK_NULL_HANDLE;g_blasBuffer=VK_NULL_HANDLE;g_blasMemory=VK_NULL_HANDLE;
    g_blasDeviceAddress=g_blasScratchAddress=g_tlasDeviceAddress=g_tlasScratchAddress=g_tlasInstanceDeviceAddress=0;
    g_testVertexBuffer=VK_NULL_HANDLE;g_testVertexMemory=VK_NULL_HANDLE;g_sbtBuffer=VK_NULL_HANDLE;g_sbtMemory=VK_NULL_HANDLE;g_rayTracingPipeline=VK_NULL_HANDLE;g_rayTracingPipelineLayout=VK_NULL_HANDLE;g_rayTracingDescriptorSetLayout=VK_NULL_HANDLE;g_queue=VK_NULL_HANDLE;g_queueFamily=UINT32_MAX;
    g_testVertexDeviceAddress=0;g_sbtDeviceAddress=0;g_sbtSize=0;g_raygenOffset=0;g_hitOffset=0;g_missOffset=0;g_handleSizeAligned=0;
    g_sectionVertexAddressBuffer=VK_NULL_HANDLE;g_sectionVertexAddressMemory=VK_NULL_HANDLE;g_sectionVertexAddressCapacity=0;

    clearVLISections();

    {
        std::lock_guard<std::timed_mutex> lock(g_chunkMutex);
        g_chunkMeshes.clear();
        g_chunkMotion.clear();
        g_chunkRawPayloads.clear();
        g_chunkUploadDigests.clear();
        g_cutoutOmmInputs.clear();
        g_voxyLodSources.clear();
        g_voxyLodSpatialOwners.clear();
        g_dhLodSections.clear();
        g_dhLodSpatialOwners.clear();
        g_dhLodSpatialSnapshot.clear();
        g_dirtySectionKeys.clear();
        g_forceSectionASReset = false;
        g_worldGeometryDirty.store(false, std::memory_order_release);
    }
    g_asBuilt =
        false;
    g_nextSectionRebuildFrame = 0u;
    g_firstPendingSectionFrame = 0u;
    g_lastObservedSectionRevision =
        g_worldGeometryRevision.load(std::memory_order_acquire);

    g_hasPrevCameraMatrices =
        false;

    g_cameraHistoryValid =
        false;

    g_lastCameraFrame =
        -1;

    g_dlssReady =
        false;

    g_dlssRRSupported =
    false;

    g_dlssRRReady =
        false;

    g_dlssGSupported = false;
    g_dlssGReady = false;
    g_dlssGFeatureInitialized = false;
    g_dlssGLoadResult = sl::Result::eErrorFeatureNotSupported;
    g_slDLSSGSetOptions = nullptr;
    g_slDLSSGGetState = nullptr;


    if(g_slShutdown)
    {
        g_slShutdown();

        g_slShutdown =
            nullptr;
    }


    g_slInit =
        nullptr;

    g_slSetVulkanInfo =
        nullptr;

    g_slIsFeatureSupported =
        nullptr;

    g_slSetFeatureLoaded =
        nullptr;

    g_slGetFeatureFunction =
        nullptr;

    g_slGetFeatureRequirements =
        nullptr;

    g_slSetConstants =
        nullptr;

    g_slEvaluateFeature =
        nullptr;

    g_slGetNewFrameToken =
        nullptr;
    g_slSetTagForFrame =
    nullptr;

    g_slDLSSSetOptions =
        nullptr;

    g_slDLSSGetOptimalSettings =
        nullptr;


    g_slDLSSDSetOptions =
        nullptr;

    g_slDLSSDGetOptimalSettings =
        nullptr;


    g_slReflexSetOptions =
        nullptr;
    g_slReflexGetState =
    nullptr;
    g_slReflexSleep =
    nullptr;
    g_slPCLSetMarker =
    nullptr;


    g_currentSLFrameToken =
        nullptr;

    g_currentSLFrameIndex =
        0;

    g_nextSLFrameIndex =
        0;

    g_currentSLFrameValid =
        false;
    g_lastRenderSubmitStartFrame =
    UINT32_MAX;

    g_lastRenderSubmitEndFrame =
        UINT32_MAX;
    g_lastPresentStartFrame =
    UINT32_MAX;

    g_lastPresentEndFrame =
        UINT32_MAX;

    g_reflexSupported =
        false;

    g_pclSupported =
        false;

    g_reflexLowLatencyAvailable =
        false;

    g_reflexMode =
        1;


    g_slInitialized =
        false;


    if(g_slModule)
    {
        FreeLibrary(g_slModule);

        g_slModule =
            NULL;
    }
    g_slDeviceConfigured =
    false;


    g_slVkGetInstanceProcAddrProxy =
        nullptr;

    g_slVkGetDeviceProcAddrProxy =
        nullptr;


    g_slVkCreateWin32SurfaceKHRProxy =
        nullptr;

    g_slVkDestroySurfaceKHRProxy =
        nullptr;


    g_slVkCreateSwapchainKHRProxy =
        nullptr;

    g_slVkDestroySwapchainKHRProxy =
        nullptr;

    g_slVkGetSwapchainImagesKHRProxy =
        nullptr;

    g_slVkAcquireNextImageKHRProxy =
        nullptr;

    g_slVkQueuePresentKHRProxy =
        nullptr;

    g_slVkDeviceWaitIdleProxy =
        nullptr;

    releaseSharedPipelineCache();

    g_instance =
        VK_NULL_HANDLE;

    g_physicalDevice =
        VK_NULL_HANDLE;

    g_device =
        VK_NULL_HANDLE;
}
