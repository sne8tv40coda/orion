// Orion backend — Device 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"


extern "C"
JNIEXPORT void JNICALL Java_com_example_vulkan_NativeVulkan_initDeviceHandlesNative(JNIEnv*,jclass,jlong instanceHandle,jlong physicalDeviceHandle,jlong deviceHandle,jlong queueHandle,jint queueFamily,jlong,jlong){
    fprintf(stderr,
"[CPP TEST] initDeviceHandlesNative ENTER\n");
    fflush(stderr);
    g_instance=reinterpret_cast<VkInstance>(instanceHandle);
    g_physicalDevice=reinterpret_cast<VkPhysicalDevice>(physicalDeviceHandle);
    g_device=reinterpret_cast<VkDevice>(deviceHandle);
    g_queue=reinterpret_cast<VkQueue>(queueHandle);
    g_queueFamily=static_cast<uint32_t>(queueFamily);
    fprintf(stderr,
"[BEFORE SL INIT]\n");
    fflush(stderr);
    initStreamlineInternal();
    fprintf(stderr,
    "[AFTER SL INIT]\n");
    fflush(stderr);

    if(deviceInitMilestone("1-after-initStreamlineInternal"))
    {
        return;
    }
    fprintf(stderr,
"[CHECK AFTER SAVE] g_device=%p\n",
(void*)g_device);

    fflush(stderr);

    fprintf(stderr,
    "[CHECK INSIDE SL INIT] addr=%p value=%p\n",
    (void*)&g_device,
    (void*)g_device);

    fflush(stderr);
    fprintf(stderr,
        "[CPP] Vulkan handles received:\n"
        " instance=%p\n"
        " physicalDevice=%p\n"
        " device=%p\n"
        " queue=%p\n"
        " queueFamily=%u\n",
        (void*)g_instance,
        (void*)g_physicalDevice,
        (void*)g_device,
        (void*)g_queue,
        g_queueFamily);

    fflush(stderr);


    fprintf(stderr,
"[RT CMD POOL ENTER]\n");
    fflush(stderr);

    createRTCommandPool();
    fprintf(stderr,
    "[RT CMD POOL DONE]\n");
    fflush(stderr);

    if(deviceInitMilestone("2-after-createRTCommandPool"))
    {
        return;
    }

    if(!g_instance||!g_physicalDevice||!g_device||!g_queue){std::cerr<<"[Native C++] Invalid Vulkan handles passed from Java."<<std::endl;return;}
    try{
        probeOpacityMicromapSupport();
        probeSerSupport();

        if(deviceInitMilestone("3-after-OMM-SER-probes"))
        {
            return;
        }

        std::vector<uint8_t> rgen;
        bool serRaygenLoaded = false;
        const bool serShaderRequested =
            g_serEnabled.load() && g_serFeatureSupported && g_serHardwareReordering;
        if(serShaderRequested)
        {
            try
            {
                rgen = loadSpv("shaders/rt_test_ser.rgen.spv");
                serRaygenLoaded = true;
            }
            catch(const std::exception& error)
            {
                RTLog(std::string("[SER-3 PIPELINE] requested=1 loaded=0 fallback=traceRay reason=") + error.what());
            }
        }
        if(!serRaygenLoaded)
        {
            rgen = loadSpv("shaders/rt_test.rgen.spv");
        }
        {
            std::stringstream log;
            log << "[SER-3 PIPELINE] requested="
                << (serShaderRequested ? 1 : 0)
                << " loaded=" << (serRaygenLoaded ? 1 : 0)
                << " mode=" << (serRaygenLoaded ? "secondaryGI_SER" : "traceRayFallback");
            RTLog(log.str());
        }

        RTLog("[BUILD] orion-rt tag=2026-10-05-cutout-offset-cull dh=local-light-classic-031-only+nrd-chain-disabled+mv-doc-shielded");

        if(deviceInitMilestone("5-before-pipeline-build"))
        {
            return;
        }

        auto chit=loadSpv("shaders/rt_test.rchit.spv");
        auto miss=loadSpv("shaders/rt_test.rmiss.spv");
        auto ahit=loadSpv("shaders/rt_test.rahit.spv");
        auto translucentChit=loadSpv("shaders/rt_test_translucent.rchit.spv");
        auto translucentAhit=loadSpv("shaders/rt_test_translucent.rahit.spv");
        auto comp=loadSpv("shaders/rt_vpt_irradiance.comp.spv");


        g_rayTracingDescriptorSetLayout=createRTDescriptorSetLayout();
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount=1;
        pli.pSetLayouts=&g_rayTracingDescriptorSetLayout;
        check(vkCreatePipelineLayout(g_device,&pli,nullptr,&g_rayTracingPipelineLayout),"Failed to create RT pipeline layout");

        VkShaderModule mods[6]={
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE
        };
        try{
            mods[0]=createShaderModule(g_device,rgen);
            mods[1]=createShaderModule(g_device,chit);
            mods[2]=createShaderModule(g_device,miss);
            mods[3]=createShaderModule(g_device,ahit);
            mods[4]=createShaderModule(g_device,translucentChit);
            mods[5]=createShaderModule(g_device,translucentAhit);

            if(deviceInitMilestone("6b-after-shader-modules"))
            {
                for(auto m:mods)if(m)vkDestroyShaderModule(g_device,m,nullptr);

                return;
            }

            VkPipelineShaderStageCreateInfo
                stages[6]{};


            stages[0]={
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                mods[0],
                "main",
                nullptr
            };


            stages[1]={
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                mods[1],
                "main",
                nullptr
            };


            stages[2]={
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_MISS_BIT_KHR,
                mods[2],
                "main",
                nullptr
            };


            stages[3]={
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                mods[3],
                "main",
                nullptr
            };

            stages[4]={
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                mods[4],
                "main",
                nullptr
            };

            stages[5]={
                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr,
                0,
                VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                mods[5],
                "main",
                nullptr
            };

            VkRayTracingShaderGroupCreateInfoKHR
     groups[5]{};

            groups[0]={VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,nullptr,VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,0,VK_SHADER_UNUSED_KHR,VK_SHADER_UNUSED_KHR,VK_SHADER_UNUSED_KHR};

            groups[1]={VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,nullptr,VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,VK_SHADER_UNUSED_KHR,1,3,VK_SHADER_UNUSED_KHR};

            groups[2]={VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,nullptr,VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,2,VK_SHADER_UNUSED_KHR,VK_SHADER_UNUSED_KHR,VK_SHADER_UNUSED_KHR};

            groups[3]={VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,nullptr,VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,VK_SHADER_UNUSED_KHR,4,5,VK_SHADER_UNUSED_KHR};

            groups[4]={VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,nullptr,VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,VK_SHADER_UNUSED_KHR,1,3,VK_SHADER_UNUSED_KHR};
            VkRayTracingPipelineCreateInfoKHR pci{VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
            pci.stageCount=6;
            pci.pStages=stages;
            pci.groupCount=5;
            pci.pGroups=groups;
            pci.maxPipelineRayRecursionDepth=1;
            pci.layout=g_rayTracingPipelineLayout;
            if(g_ommNativeEnabled)
            {
                pci.flags |=
                    VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
            }

            const VkPipelineCache rtPipelineCache =
                rtPipelineCacheDisabled()
                    ? VK_NULL_HANDLE
                    : getSharedPipelineCache();

            if(rtPipelineDisableOptimization())
            {
                pci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
            }

            // 2026-11-02 P0（冷编分解）：走计时包装，参数（cache / deferredOperation=null
            // / 1 条 createInfo）与原调用逐位一致，只多一条 [PERF PIPE] kind=rt 日志。
            check(OrionCreateRayTracingPipelineTimed(
                      "main",
                      1u,
                      &pci,
                      rtPipelineCache,
                      &g_rayTracingPipeline),
                  "Failed to create RT pipeline");

            persistSharedPipelineCache();
            RTLog(
                "[RT TRANSLUCENT SPV]"
                " group3=rt_test_translucent.rchit+rahit"
                " group4=rt_test.rchit+rahit"
            );
        }catch(...){
            for(auto m:mods)if(m)vkDestroyShaderModule(g_device,m,nullptr);
            throw;
        }
        for(auto m:mods)if(m)vkDestroyShaderModule(g_device,m,nullptr);

        if(deviceInitMilestone("6-after-pipeline-build"))
        {
            return;
        }

        std::cout<<"[RT] createTestVertexBuffer"<<std::endl;
        createTestVertexBuffer();

        std::cout<<"[RT] createBLAS"<<std::endl;
        createBLAS();

        std::cout<<"[RT] createTLAS"<<std::endl;
        createTLAS();

        std::cout<<"[RT] createRayTracingSBT"<<std::endl;
        createRayTracingSBT();

        if(deviceInitMilestone("4-after-RT-core-BLAS-TLAS-SBT"))
        {
            return;
        }

        std::cout << "[RT] ensureSharcUpdatePipeline" << std::endl;
        ensureSharcUpdatePipeline();

        std::cout<<"[RT] createCameraUBO"<<std::endl;
        createCameraUBO();

        std::cout<<"[RT] createDescriptor"<<std::endl;
        createRayTracingDescriptorPoolAndSet();

        std::cout << "[TYNDALL CLOUD] create static 3D noise resources" << std::endl;
        createCloudNoiseResources();
        createCloudBaseNoiseResources();
        createCloudV3Resources();
        createCloudV4Resources();

        std::cout << "[WATER NORMAL] create mipmapped detail normal" << std::endl;
        createWaterDetailNormalResources();

        std::cout << "[ENTITY PBR] create neutral fallback image" << std::endl;
        createEntityPbrFallbackResources();


        std::cout
            << "[WATER SIM] create Eclipse-style player water pipeline"
            << std::endl;
        createWaterSimulationPipeline();

        std::cout
            << "[WATER FFT] create Phillips spectrum water pipeline"
            << std::endl;
        createWaterFftResources();
        updateWaterFftRayDescriptor();

        std::cout
            << "[WATER CAUSTIC MAP] create refracted-grid caustic map"
            << std::endl;
        createWaterCausticMapResources();

#if 0
        std::cout
            << "[WATER FOAM] create persistent foam map"
            << std::endl;
        createWaterFoamResources();
#endif

        std::cout
            << "[WATER CAUSTIC] create wavefront-area caustic pipeline"
            << std::endl;
        createWaterCausticPipeline();


        std::cout<<"[VLI-1.4] create voxel compute pipeline"<<std::endl;
        createVLIComputePipeline();


        std::cout
            << "[RR-6.5.2] create SDR post pipeline"
            << std::endl;

        createSDRPostPipeline();

        std::cout
            << "[RR-6.6] create HDR10 display pipeline"
            << std::endl;

        try
        {
            createHDRPostPipeline();
        }
        catch(const std::exception& e)
        {
            fprintf(
                stderr,
                "[RR-6.6 PIPELINE] FAILED: %s\n",
                e.what()
            );

            g_hdrPostPipeline =
                VK_NULL_HANDLE;
        }


        std::cout
            << "[RR-7] create auto exposure buffers"
            << std::endl;

        createAeStateBuffers();


        std::cout
            << "[RR-7] create auto exposure pipelines"
            << std::endl;

        try
        {
            createAutoExposurePipeline();
        }
        catch(const std::exception& e)
        {
            fprintf(
                stderr,
                "[RR-7 PIPELINE] FAILED: %s\n",
                e.what()
            );

            RTLog(
                std::string("[RR-7 INIT] FAILED: ") +
                e.what()
            );

            g_aeEnabled =
                false;
        }


        {
            std::ostringstream aeInitLog;

            aeInitLog
                << "[RR-7 INIT]"

                << " active="
                << (
                    g_aeHistogramPipeline != VK_NULL_HANDLE &&
                    g_aeAdaptPipeline != VK_NULL_HANDLE &&
                    g_aeHistogramSet != VK_NULL_HANDLE &&
                    g_aeAdaptSet != VK_NULL_HANDLE &&
                    g_aeHistogramBuffer != VK_NULL_HANDLE &&
                    g_aeStateBuffer != VK_NULL_HANDLE
                        ? 1
                        : 0
                )

                << " histPipe="
                << (g_aeHistogramPipeline != VK_NULL_HANDLE ? 1 : 0)

                << " adaptPipe="
                << (g_aeAdaptPipeline != VK_NULL_HANDLE ? 1 : 0)

                << " histSet="
                << (g_aeHistogramSet != VK_NULL_HANDLE ? 1 : 0)

                << " adaptSet="
                << (g_aeAdaptSet != VK_NULL_HANDLE ? 1 : 0)

                << " histBuf="
                << (g_aeHistogramBuffer != VK_NULL_HANDLE ? 1 : 0)

                << " stateBuf="
                << (g_aeStateBuffer != VK_NULL_HANDLE ? 1 : 0)

                << " enabled="
                << (g_aeEnabled.load() ? 1 : 0)

                << " maxEV="
                << g_aeMaxEV;

            RTLog(aeInitLog.str());
        }


        std::cout
            << "[FG-4.1] create UI Alpha pipeline"
            << std::endl;

        try
        {
            createFGUIAlphaPipeline();
        }
        catch(const std::exception& e)
        {
            fprintf(
                stderr,
                "[FG-4.1 PIPELINE] FAILED: %s\n",
                e.what()
            );

            fflush(stderr);
        }


        std::cout
            << "[Native C++] RT initialization completed."
            << std::endl;
    }
    catch(const std::exception& e)
    {
        std::cerr<<"[Native C++] RT initialization failed: "<<e.what()<<std::endl;
    }

}
