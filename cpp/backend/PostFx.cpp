// Orion backend — PostFx 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

void createSDRPostPipeline()
{

    VkDescriptorSetLayoutBinding bindings[3]{};

    bindings[0].binding =
        0;

    bindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    bindings[0].descriptorCount =
        1;

    bindings[0].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    bindings[1].binding =
        1;

    bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    bindings[1].descriptorCount =
        1;

    bindings[1].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    bindings[2].binding =
        2;

    bindings[2].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    bindings[2].descriptorCount =
        1;

    bindings[2].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };

    layoutInfo.bindingCount =
        3;

    layoutInfo.pBindings =
        bindings;


    check(
        vkCreateDescriptorSetLayout(
            g_device,
            &layoutInfo,
            nullptr,
            &g_sdrPostDescriptorSetLayout
        ),
        "create SDR post descriptor layout failed"
    );



    VkDescriptorPoolSize poolSizes[3]{};

    poolSizes[0].type =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    poolSizes[0].descriptorCount =
        1;


    poolSizes[1].type =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    poolSizes[1].descriptorCount =
        1;


    poolSizes[2].type =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    poolSizes[2].descriptorCount =
        1;


    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };

    poolInfo.maxSets =
        1;

    poolInfo.poolSizeCount =
        3;

    poolInfo.pPoolSizes =
        poolSizes;


    check(
        vkCreateDescriptorPool(
            g_device,
            &poolInfo,
            nullptr,
            &g_sdrPostDescriptorPool
        ),
        "create SDR post descriptor pool failed"
    );


    VkDescriptorSetAllocateInfo setInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };

    setInfo.descriptorPool =
        g_sdrPostDescriptorPool;

    setInfo.descriptorSetCount =
        1;

    setInfo.pSetLayouts =
        &g_sdrPostDescriptorSetLayout;


    check(
        vkAllocateDescriptorSets(
            g_device,
            &setInfo,
            &g_sdrPostDescriptorSet
        ),
        "allocate SDR post descriptor set failed"
    );



    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };

    pipelineLayoutInfo.setLayoutCount =
        1;

    pipelineLayoutInfo.pSetLayouts =
        &g_sdrPostDescriptorSetLayout;

    VkPushConstantRange postPushRange{};
    postPushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    postPushRange.offset = 0u;
    postPushRange.size = static_cast<uint32_t>(sizeof(PostProcessingPush));
    pipelineLayoutInfo.pushConstantRangeCount = 1u;
    pipelineLayoutInfo.pPushConstantRanges = &postPushRange;

    check(
        vkCreatePipelineLayout(
            g_device,
            &pipelineLayoutInfo,
            nullptr,
            &g_sdrPostPipelineLayout
        ),
        "create SDR post pipeline layout failed"
    );



    auto code =
        loadSpv(
            "shaders/rr_sdr_post.comp.spv"
        );


    VkShaderModule shaderModule =
        createShaderModule(
            g_device,
            code
        );


    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
    };

    stage.stage =
        VK_SHADER_STAGE_COMPUTE_BIT;

    stage.module =
        shaderModule;

    stage.pName =
        "main";


    VkComputePipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
    };

    pipelineInfo.stage =
        stage;

    pipelineInfo.layout =
        g_sdrPostPipelineLayout;


    VkResult result =
        vkCreateComputePipelines(
            g_device,
            getSharedPipelineCache(),
            1,
            &pipelineInfo,
            nullptr,
            &g_sdrPostPipeline
        );


    vkDestroyShaderModule(
        g_device,
        shaderModule,
        nullptr
    );


    check(
        result,
        "create SDR post compute pipeline failed"
    );


    fprintf(
        stderr,
        "[RR-6.5.2] SDR post pipeline READY\n"
    );

    fflush(stderr);
}
void updateSDRPostDescriptor()
{
    if(
        g_sdrPostDescriptorSet ==
            VK_NULL_HANDLE ||

        g_dlssOutputView ==
            VK_NULL_HANDLE
    )
    {
        return;
    }


    VkDescriptorImageInfo imageInfo{};

    imageInfo.imageView =
        g_dlssOutputView;

    imageInfo.imageLayout =
        VK_IMAGE_LAYOUT_GENERAL;


    VkWriteDescriptorSet writes[3]{};

    writes[0].sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

    writes[0].dstSet =
        g_sdrPostDescriptorSet;

    writes[0].dstBinding =
        0;

    writes[0].descriptorCount =
        1;

    writes[0].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    writes[0].pImageInfo =
        &imageInfo;



    VkDescriptorBufferInfo stateInfo{};

    stateInfo.buffer =
        g_aeStateBuffer;

    stateInfo.offset =
        0;

    stateInfo.range =
        VK_WHOLE_SIZE;


    uint32_t writeCount =
        1;


    if(g_aeStateBuffer != VK_NULL_HANDLE)
    {
        writes[1].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

        writes[1].dstSet =
            g_sdrPostDescriptorSet;

        writes[1].dstBinding =
            1;

        writes[1].descriptorCount =
            1;

        writes[1].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        writes[1].pBufferInfo =
            &stateInfo;

        writeCount =
            2;
    }


    writeStableRadianceDisplayBinding(
        g_sdrPostDescriptorSet,
        "sdrpost"
    );


    vkUpdateDescriptorSets(
        g_device,
        writeCount,
        writes,
        0,
        nullptr
    );



    if(g_aeHistogramSet != VK_NULL_HANDLE)
    {
        VkWriteDescriptorSet histogramWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
        };

        histogramWrite.dstSet =
            g_aeHistogramSet;

        histogramWrite.dstBinding =
            0;

        histogramWrite.descriptorCount =
            1;

        histogramWrite.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        histogramWrite.pImageInfo =
            &imageInfo;


        vkUpdateDescriptorSets(
            g_device,
            1,
            &histogramWrite,
            0,
            nullptr
        );
    }
}


void createHDRPostPipeline()
{
    if(
        g_sdrPostPipelineLayout ==
            VK_NULL_HANDLE ||

        g_hdrPostPipeline !=
            VK_NULL_HANDLE
    )
    {
        return;
    }


    auto code =
        loadSpv(
            "shaders/rr_hdr_encode.comp.spv"
        );


    VkShaderModule shaderModule =
        createShaderModule(
            g_device,
            code
        );


    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
    };

    stage.stage =
        VK_SHADER_STAGE_COMPUTE_BIT;

    stage.module =
        shaderModule;

    stage.pName =
        "main";


    VkComputePipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
    };

    pipelineInfo.stage =
        stage;

    pipelineInfo.layout =
        g_sdrPostPipelineLayout;


    VkResult result =
        vkCreateComputePipelines(
            g_device,
            getSharedPipelineCache(),
            1,
            &pipelineInfo,
            nullptr,
            &g_hdrPostPipeline
        );


    vkDestroyShaderModule(
        g_device,
        shaderModule,
        nullptr
    );


    check(
        result,
        "create HDR10 display compute pipeline failed"
    );


    fprintf(
        stderr,
        "[RR-6.6] HDR10 display pipeline READY\n"
    );

    fflush(stderr);
}



void createAeStateBuffers()
{
    if(g_aeStateBuffer != VK_NULL_HANDLE)
    {
        return;
    }

    const VkDeviceSize histogramBytes =
        VkDeviceSize(AE_HISTOGRAM_BIN_COUNT) *
        sizeof(uint32_t);

    const VkDeviceSize stateBytes =
        6u *
        sizeof(uint32_t);


    createBuffer(
        g_device,
        g_physicalDevice,
        histogramBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_aeHistogramBuffer,
        g_aeHistogramMemory
    );


    createBuffer(
        g_device,
        g_physicalDevice,
        stateBytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_aeStateBuffer,
        g_aeStateMemory
    );


    {

        void* histogramMapped =
            nullptr;

        check(
            vkMapMemory(
                g_device,
                g_aeHistogramMemory,
                0,
                histogramBytes,
                0,
                &histogramMapped
            ),
            "map AE histogram buffer failed"
        );

        memset(
            histogramMapped,
            0,
            size_t(histogramBytes)
        );

        vkUnmapMemory(
            g_device,
            g_aeHistogramMemory
        );


        void* stateMapped =
            nullptr;

        check(
            vkMapMemory(
                g_device,
                g_aeStateMemory,
                0,
                stateBytes,
                0,
                &stateMapped
            ),
            "map AE state buffer failed"
        );

        memset(
            stateMapped,
            0,
            size_t(stateBytes)
        );

        static_cast<float*>(stateMapped)[4] =
            1.0f;

        g_aeStateCpu =
            static_cast<float*>(stateMapped);
    }
}
void createAutoExposurePipeline()
{
    if(
        g_aeHistogramPipeline !=
            VK_NULL_HANDLE
    )
    {
        return;
    }


    createAeStateBuffers();



    VkDescriptorSetLayoutBinding histogramBindings[2]{};

    histogramBindings[0].binding =
        0;

    histogramBindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    histogramBindings[0].descriptorCount =
        1;

    histogramBindings[0].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    histogramBindings[1].binding =
        1;

    histogramBindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    histogramBindings[1].descriptorCount =
        1;

    histogramBindings[1].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    VkDescriptorSetLayoutCreateInfo histogramLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };

    histogramLayoutInfo.bindingCount =
        2;

    histogramLayoutInfo.pBindings =
        histogramBindings;

    check(
        vkCreateDescriptorSetLayout(
            g_device,
            &histogramLayoutInfo,
            nullptr,
            &g_aeHistogramSetLayout
        ),
        "create AE histogram descriptor layout failed"
    );


    VkDescriptorPoolSize histogramPoolSizes[2]{};

    histogramPoolSizes[0].type =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    histogramPoolSizes[0].descriptorCount =
        1;

    histogramPoolSizes[1].type =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    histogramPoolSizes[1].descriptorCount =
        1;


    VkDescriptorPoolCreateInfo histogramPoolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };

    histogramPoolInfo.maxSets =
        1;

    histogramPoolInfo.poolSizeCount =
        2;

    histogramPoolInfo.pPoolSizes =
        histogramPoolSizes;

    check(
        vkCreateDescriptorPool(
            g_device,
            &histogramPoolInfo,
            nullptr,
            &g_aeHistogramPool
        ),
        "create AE histogram descriptor pool failed"
    );


    VkDescriptorSetAllocateInfo histogramSetInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };

    histogramSetInfo.descriptorPool =
        g_aeHistogramPool;

    histogramSetInfo.descriptorSetCount =
        1;

    histogramSetInfo.pSetLayouts =
        &g_aeHistogramSetLayout;

    check(
        vkAllocateDescriptorSets(
            g_device,
            &histogramSetInfo,
            &g_aeHistogramSet
        ),
        "allocate AE histogram descriptor set failed"
    );



    {
        VkDescriptorBufferInfo histogramInfo{};

        histogramInfo.buffer =
            g_aeHistogramBuffer;

        histogramInfo.offset =
            0;

        histogramInfo.range =
            VK_WHOLE_SIZE;


        VkWriteDescriptorSet histogramWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
        };

        histogramWrite.dstSet =
            g_aeHistogramSet;

        histogramWrite.dstBinding =
            1;

        histogramWrite.descriptorCount =
            1;

        histogramWrite.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        histogramWrite.pBufferInfo =
            &histogramInfo;

        vkUpdateDescriptorSets(
            g_device,
            1,
            &histogramWrite,
            0,
            nullptr
        );
    }


    VkPipelineLayoutCreateInfo histogramPipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };

    histogramPipelineLayoutInfo.setLayoutCount =
        1;

    histogramPipelineLayoutInfo.pSetLayouts =
        &g_aeHistogramSetLayout;

    check(
        vkCreatePipelineLayout(
            g_device,
            &histogramPipelineLayoutInfo,
            nullptr,
            &g_aeHistogramLayout
        ),
        "create AE histogram pipeline layout failed"
    );



    VkDescriptorSetLayoutBinding adaptBindings[2]{};

    adaptBindings[0].binding =
        0;

    adaptBindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    adaptBindings[0].descriptorCount =
        1;

    adaptBindings[0].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    adaptBindings[1].binding =
        1;

    adaptBindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    adaptBindings[1].descriptorCount =
        1;

    adaptBindings[1].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    VkDescriptorSetLayoutCreateInfo adaptLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };

    adaptLayoutInfo.bindingCount =
        2;

    adaptLayoutInfo.pBindings =
        adaptBindings;

    check(
        vkCreateDescriptorSetLayout(
            g_device,
            &adaptLayoutInfo,
            nullptr,
            &g_aeAdaptSetLayout
        ),
        "create AE adapt descriptor layout failed"
    );


    VkDescriptorPoolSize adaptPoolSizes[1]{};

    adaptPoolSizes[0].type =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

    adaptPoolSizes[0].descriptorCount =
        2;


    VkDescriptorPoolCreateInfo adaptPoolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };

    adaptPoolInfo.maxSets =
        1;

    adaptPoolInfo.poolSizeCount =
        1;

    adaptPoolInfo.pPoolSizes =
        adaptPoolSizes;

    check(
        vkCreateDescriptorPool(
            g_device,
            &adaptPoolInfo,
            nullptr,
            &g_aeAdaptPool
        ),
        "create AE adapt descriptor pool failed"
    );


    VkDescriptorSetAllocateInfo adaptSetInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };

    adaptSetInfo.descriptorPool =
        g_aeAdaptPool;

    adaptSetInfo.descriptorSetCount =
        1;

    adaptSetInfo.pSetLayouts =
        &g_aeAdaptSetLayout;

    check(
        vkAllocateDescriptorSets(
            g_device,
            &adaptSetInfo,
            &g_aeAdaptSet
        ),
        "allocate AE adapt descriptor set failed"
    );


    {
        VkDescriptorBufferInfo histogramInfo{};

        histogramInfo.buffer =
            g_aeHistogramBuffer;

        histogramInfo.offset =
            0;

        histogramInfo.range =
            VK_WHOLE_SIZE;


        VkDescriptorBufferInfo stateInfo{};

        stateInfo.buffer =
            g_aeStateBuffer;

        stateInfo.offset =
            0;

        stateInfo.range =
            VK_WHOLE_SIZE;


        VkWriteDescriptorSet adaptWrites[2]{};

        adaptWrites[0].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

        adaptWrites[0].dstSet =
            g_aeAdaptSet;

        adaptWrites[0].dstBinding =
            0;

        adaptWrites[0].descriptorCount =
            1;

        adaptWrites[0].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        adaptWrites[0].pBufferInfo =
            &histogramInfo;


        adaptWrites[1].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

        adaptWrites[1].dstSet =
            g_aeAdaptSet;

        adaptWrites[1].dstBinding =
            1;

        adaptWrites[1].descriptorCount =
            1;

        adaptWrites[1].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

        adaptWrites[1].pBufferInfo =
            &stateInfo;


        vkUpdateDescriptorSets(
            g_device,
            2,
            adaptWrites,
            0,
            nullptr
        );
    }


    VkPushConstantRange adaptPushRange{};

    adaptPushRange.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;

    adaptPushRange.offset =
        0;

    adaptPushRange.size =
        sizeof(AePushConstants);


    VkPipelineLayoutCreateInfo adaptPipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };

    adaptPipelineLayoutInfo.setLayoutCount =
        1;

    adaptPipelineLayoutInfo.pSetLayouts =
        &g_aeAdaptSetLayout;

    adaptPipelineLayoutInfo.pushConstantRangeCount =
        1;

    adaptPipelineLayoutInfo.pPushConstantRanges =
        &adaptPushRange;

    check(
        vkCreatePipelineLayout(
            g_device,
            &adaptPipelineLayoutInfo,
            nullptr,
            &g_aeAdaptLayout
        ),
        "create AE adapt pipeline layout failed"
    );



    auto createAePipeline =
        [](const char* spvPath,
           VkPipelineLayout layout,
           VkPipeline& pipeline,
           const char* failMessage)
    {
        auto code =
            loadSpv(
                spvPath
            );


        VkShaderModule shaderModule =
            createShaderModule(
                g_device,
                code
            );


        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };

        stage.stage =
            VK_SHADER_STAGE_COMPUTE_BIT;

        stage.module =
            shaderModule;

        stage.pName =
            "main";


        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };

        pipelineInfo.stage =
            stage;

        pipelineInfo.layout =
            layout;


        VkResult result =
            vkCreateComputePipelines(
                g_device,
                getSharedPipelineCache(),
                1,
                &pipelineInfo,
                nullptr,
                &pipeline
            );


        vkDestroyShaderModule(
            g_device,
            shaderModule,
            nullptr
        );


        check(
            result,
            failMessage
        );
    };


    createAePipeline(
        "shaders/rr_ae_histogram.comp.spv",
        g_aeHistogramLayout,
        g_aeHistogramPipeline,
        "create AE histogram compute pipeline failed"
    );


    createAePipeline(
        "shaders/rr_ae_adapt.comp.spv",
        g_aeAdaptLayout,
        g_aeAdaptPipeline,
        "create AE adapt compute pipeline failed"
    );


    updateSDRPostDescriptor();


    fprintf(
        stderr,
        "[RR-7] Auto exposure pipelines READY bins=%u keyEV=%.4f range=[%.2f,%.2f] up=%.2f down=%.2f\n",
        AE_HISTOGRAM_BIN_COUNT,
        double(AE_DEFAULT_KEY_EV),
        double(g_aeMinEV),
        double(g_aeMaxEV),
        double(g_aeAdaptUpRate),
        double(g_aeAdaptDownRate)
    );

    fflush(stderr);
}


void createHDREncodePipeline()
{
    VkDescriptorSetLayoutBinding bindings[3]{};

    bindings[0].binding = 0;
    bindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[1].binding = 1;
    bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    bindings[2].binding = 2;
    bindings[2].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
    };
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;

    check(
        vkCreateDescriptorSetLayout(
            g_device,
            &layoutInfo,
            nullptr,
            &g_hdrEncodeDescriptorSetLayout
        ),
        "create HDR encode descriptor layout failed"
    );

    VkDescriptorPoolSize poolSizes[3]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[2].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
    };
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 3;
    poolInfo.pPoolSizes = poolSizes;

    check(
        vkCreateDescriptorPool(
            g_device,
            &poolInfo,
            nullptr,
            &g_hdrEncodeDescriptorPool
        ),
        "create HDR encode descriptor pool failed"
    );

    VkDescriptorSetAllocateInfo setInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    setInfo.descriptorPool = g_hdrEncodeDescriptorPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &g_hdrEncodeDescriptorSetLayout;

    check(
        vkAllocateDescriptorSets(
            g_device,
            &setInfo,
            &g_hdrEncodeDescriptorSet
        ),
        "allocate HDR encode descriptor set failed"
    );

    VkPipelineLayoutCreateInfo plInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
    };
    plInfo.setLayoutCount = 1;
    plInfo.pSetLayouts = &g_hdrEncodeDescriptorSetLayout;

    check(
        vkCreatePipelineLayout(
            g_device,
            &plInfo,
            nullptr,
            &g_hdrEncodePipelineLayout
        ),
        "create HDR encode pipeline layout failed"
    );

    auto code = loadSpv("shaders/rr_hdr_encode.comp.spv");
    VkShaderModule shaderModule =
        createShaderModule(g_device, code);

    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
    };
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = shaderModule;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
    };
    pipelineInfo.stage = stage;
    pipelineInfo.layout = g_hdrEncodePipelineLayout;

    VkResult result =
        vkCreateComputePipelines(
            g_device,
            getSharedPipelineCache(),
            1,
            &pipelineInfo,
            nullptr,
            &g_hdrEncodePipeline
        );

    vkDestroyShaderModule(g_device, shaderModule, nullptr);

    check(
        result,
        "create HDR encode compute pipeline failed"
    );

    fprintf(stderr, "[HDR-2] HDR encode pipeline READY\n");
    fflush(stderr);
}
void updateHDREncodeDescriptor()
{
    if(
        g_hdrEncodeDescriptorSet == VK_NULL_HANDLE ||
        g_dlssOutputView == VK_NULL_HANDLE ||
        g_hdr10View == VK_NULL_HANDLE
    )
    {
        return;
    }

    VkDescriptorImageInfo inputInfo{};
    inputInfo.imageView = g_dlssOutputView;
    inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    inputInfo.sampler = getLinearSampler();

    VkDescriptorImageInfo outputInfo{};
    outputInfo.imageView = g_hdr10View;
    outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};

    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = g_hdrEncodeDescriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &inputInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = g_hdrEncodeDescriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outputInfo;

    vkUpdateDescriptorSets(g_device, 2, writes, 0, nullptr);

    writeStableRadianceDisplayBinding(
        g_hdrEncodeDescriptorSet,
        "hdrencode"
    );
}
void createHDR10OutputImage(uint32_t width, uint32_t height)
{
    if(width == 0 || height == 0)
    {
        return;
    }

    if(
        g_hdr10Image != VK_NULL_HANDLE &&
        g_hdr10Width == width &&
        g_hdr10Height == height
    )
    {
        return;
    }

    check(
        FGDeviceWaitIdle(g_device),
        "wait device before HDR10 recreate failed"
    );

    if(g_hdr10View)
    {
        vkDestroyImageView(g_device, g_hdr10View, nullptr);
        g_hdr10View = VK_NULL_HANDLE;
    }
    if(g_hdr10Image)
    {
        vkDestroyImage(g_device, g_hdr10Image, nullptr);
        g_hdr10Image = VK_NULL_HANDLE;
    }
    if(g_hdr10Memory)
    {
        vkFreeMemory(g_device, g_hdr10Memory, nullptr);
        g_hdr10Memory = VK_NULL_HANDLE;
    }

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    ci.extent = { width, height, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    check(
        vkCreateImage(g_device, &ci, nullptr, &g_hdr10Image),
        "create HDR10 intermediate image failed"
    );

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(g_device, g_hdr10Image, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = findMemoryType(
        g_physicalDevice,
        mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );
    check(
        vkAllocateMemory(g_device, &ai, nullptr, &g_hdr10Memory),
        "allocate HDR10 intermediate memory failed"
    );
    check(
        vkBindImageMemory(g_device, g_hdr10Image, g_hdr10Memory, 0),
        "bind HDR10 intermediate memory failed"
    );

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_hdr10Image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    check(
        vkCreateImageView(g_device, &vi, nullptr, &g_hdr10View),
        "create HDR10 intermediate view failed"
    );

    g_hdr10Width = width;
    g_hdr10Height = height;
    g_hdr10Layout = VK_IMAGE_LAYOUT_UNDEFINED;

    updateHDREncodeDescriptor();

    std::cerr
        << "[HDR-2] HDR10 intermediate created "
        << width << "x" << height
        << std::endl;
}
void destroyHDREncodeResources()
{
    if(g_hdrEncodePipeline)
    {
        vkDestroyPipeline(g_device, g_hdrEncodePipeline, nullptr);
        g_hdrEncodePipeline = VK_NULL_HANDLE;
    }
    if(g_hdrEncodePipelineLayout)
    {
        vkDestroyPipelineLayout(g_device, g_hdrEncodePipelineLayout, nullptr);
        g_hdrEncodePipelineLayout = VK_NULL_HANDLE;
    }
    if(g_hdrEncodeDescriptorPool)
    {
        vkDestroyDescriptorPool(g_device, g_hdrEncodeDescriptorPool, nullptr);
        g_hdrEncodeDescriptorPool = VK_NULL_HANDLE;
        g_hdrEncodeDescriptorSet = VK_NULL_HANDLE;
    }
    if(g_hdrEncodeDescriptorSetLayout)
    {
        vkDestroyDescriptorSetLayout(
            g_device,
            g_hdrEncodeDescriptorSetLayout,
            nullptr
        );
        g_hdrEncodeDescriptorSetLayout = VK_NULL_HANDLE;
    }
    if(g_hdr10View)
    {
        vkDestroyImageView(g_device, g_hdr10View, nullptr);
        g_hdr10View = VK_NULL_HANDLE;
    }
    if(g_hdr10Image)
    {
        vkDestroyImage(g_device, g_hdr10Image, nullptr);
        g_hdr10Image = VK_NULL_HANDLE;
    }
    if(g_hdr10Memory)
    {
        vkFreeMemory(g_device, g_hdr10Memory, nullptr);
        g_hdr10Memory = VK_NULL_HANDLE;
    }
    g_hdr10Width = 0;
    g_hdr10Height = 0;
    g_hdr10Layout = VK_IMAGE_LAYOUT_UNDEFINED;
}
void destroyFGImage(
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout
)
{
    if(view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(
            g_device,
            view,
            nullptr
        );

        view =
            VK_NULL_HANDLE;
    }


    if(image != VK_NULL_HANDLE)
    {
        vkDestroyImage(
            g_device,
            image,
            nullptr
        );

        image =
            VK_NULL_HANDLE;
    }


    if(memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            g_device,
            memory,
            nullptr
        );

        memory =
            VK_NULL_HANDLE;
    }


    layout =
        VK_IMAGE_LAYOUT_UNDEFINED;
}


void destroyFGUIImages()
{
    destroyFGImage(
        g_fgHudlessImage,
        g_fgHudlessMemory,
        g_fgHudlessView,
        g_fgHudlessLayout
    );


    destroyFGImage(
        g_fgFinalImage,
        g_fgFinalMemory,
        g_fgFinalView,
        g_fgFinalLayout
    );


    destroyFGImage(
        g_fgUIAlphaImage,
        g_fgUIAlphaMemory,
        g_fgUIAlphaView,
        g_fgUIAlphaLayout
    );


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


void createFGImage(
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view
)
{
    VkImageCreateInfo imageInfo{
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
    };


    imageInfo.imageType =
        VK_IMAGE_TYPE_2D;

    imageInfo.format =
        format;

    imageInfo.extent =
    {
        width,
        height,
        1
    };

    imageInfo.mipLevels =
        1;

    imageInfo.arrayLayers =
        1;

    imageInfo.samples =
        VK_SAMPLE_COUNT_1_BIT;

    imageInfo.tiling =
        VK_IMAGE_TILING_OPTIMAL;

    imageInfo.usage =
        usage;

    imageInfo.sharingMode =
        VK_SHARING_MODE_EXCLUSIVE;

    imageInfo.initialLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;


    check(
        vkCreateImage(
            g_device,
            &imageInfo,
            nullptr,
            &image
        ),
        "FG-4.1 create image failed"
    );


    VkMemoryRequirements memoryRequirements{};

    vkGetImageMemoryRequirements(
        g_device,
        image,
        &memoryRequirements
    );


    VkMemoryAllocateInfo allocationInfo{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };


    allocationInfo.allocationSize =
        memoryRequirements.size;

    allocationInfo.memoryTypeIndex =
        findMemoryType(
            g_physicalDevice,
            memoryRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );


    check(
        vkAllocateMemory(
            g_device,
            &allocationInfo,
            nullptr,
            &memory
        ),
        "FG-4.1 allocate image memory failed"
    );


    check(
        vkBindImageMemory(
            g_device,
            image,
            memory,
            0
        ),
        "FG-4.1 bind image memory failed"
    );


    VkImageViewCreateInfo viewInfo{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
    };


    viewInfo.image =
        image;

    viewInfo.viewType =
        VK_IMAGE_VIEW_TYPE_2D;

    viewInfo.format =
        format;


    viewInfo.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    viewInfo.subresourceRange.baseMipLevel =
        0;

    viewInfo.subresourceRange.levelCount =
        1;

    viewInfo.subresourceRange.baseArrayLayer =
        0;

    viewInfo.subresourceRange.layerCount =
        1;


    check(
        vkCreateImageView(
            g_device,
            &viewInfo,
            nullptr,
            &view
        ),
        "FG-4.1 create image view failed"
    );
}


void updateFGUIAlphaDescriptor()
{
    if(
        g_fgUIAlphaDescriptorSet ==
            VK_NULL_HANDLE ||

        g_fgUISnapshotSampler ==
            VK_NULL_HANDLE ||

        g_fgHudlessView ==
            VK_NULL_HANDLE ||

        g_fgFinalView ==
            VK_NULL_HANDLE ||

        g_fgUIAlphaView ==
            VK_NULL_HANDLE
    )
    {
        return;
    }


    VkDescriptorImageInfo hudlessInfo{};

    hudlessInfo.sampler =
        g_fgUISnapshotSampler;

    hudlessInfo.imageView =
        g_fgHudlessView;

    hudlessInfo.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;


    VkDescriptorImageInfo finalInfo{};

    finalInfo.sampler =
        g_fgUISnapshotSampler;

    finalInfo.imageView =
        g_fgFinalView;

    finalInfo.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;


    VkDescriptorImageInfo alphaInfo{};

    alphaInfo.imageView =
        g_fgUIAlphaView;

    alphaInfo.imageLayout =
        VK_IMAGE_LAYOUT_GENERAL;


    VkWriteDescriptorSet writes[3]{};


    writes[0].sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

    writes[0].dstSet =
        g_fgUIAlphaDescriptorSet;

    writes[0].dstBinding =
        0;

    writes[0].descriptorCount =
        1;

    writes[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    writes[0].pImageInfo =
        &hudlessInfo;


    writes[1].sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

    writes[1].dstSet =
        g_fgUIAlphaDescriptorSet;

    writes[1].dstBinding =
        1;

    writes[1].descriptorCount =
        1;

    writes[1].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    writes[1].pImageInfo =
        &finalInfo;


    writes[2].sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

    writes[2].dstSet =
        g_fgUIAlphaDescriptorSet;

    writes[2].dstBinding =
        2;

    writes[2].descriptorCount =
        1;

    writes[2].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    writes[2].pImageInfo =
        &alphaInfo;


    vkUpdateDescriptorSets(
        g_device,
        3,
        writes,
        0,
        nullptr
    );
}


void createFGUIAlphaPipeline()
{
    if(
        g_fgUIAlphaPipeline !=
        VK_NULL_HANDLE
    )
    {
        return;
    }


    VkDescriptorSetLayoutBinding
        bindings[3]{};


    bindings[0].binding =
        0;

    bindings[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    bindings[0].descriptorCount =
        1;

    bindings[0].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    bindings[1].binding =
        1;

    bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    bindings[1].descriptorCount =
        1;

    bindings[1].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    bindings[2].binding =
        2;

    bindings[2].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    bindings[2].descriptorCount =
        1;

    bindings[2].stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;


    VkDescriptorSetLayoutCreateInfo
        descriptorLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        };


    descriptorLayoutInfo.bindingCount =
        3;

    descriptorLayoutInfo.pBindings =
        bindings;


    check(
        vkCreateDescriptorSetLayout(
            g_device,
            &descriptorLayoutInfo,
            nullptr,
            &g_fgUIAlphaDescriptorSetLayout
        ),
        "FG-4.1 create descriptor layout failed"
    );


    VkDescriptorPoolSize
        poolSizes[2]{};


    poolSizes[0].type =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;

    poolSizes[0].descriptorCount =
        2;


    poolSizes[1].type =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    poolSizes[1].descriptorCount =
        1;


    VkDescriptorPoolCreateInfo
        poolInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        };


    poolInfo.maxSets =
        1;

    poolInfo.poolSizeCount =
        2;

    poolInfo.pPoolSizes =
        poolSizes;


    check(
        vkCreateDescriptorPool(
            g_device,
            &poolInfo,
            nullptr,
            &g_fgUIAlphaDescriptorPool
        ),
        "FG-4.1 create descriptor pool failed"
    );


    VkDescriptorSetAllocateInfo
        descriptorAllocateInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };


    descriptorAllocateInfo.descriptorPool =
        g_fgUIAlphaDescriptorPool;

    descriptorAllocateInfo.descriptorSetCount =
        1;

    descriptorAllocateInfo.pSetLayouts =
        &g_fgUIAlphaDescriptorSetLayout;


    check(
        vkAllocateDescriptorSets(
            g_device,
            &descriptorAllocateInfo,
            &g_fgUIAlphaDescriptorSet
        ),
        "FG-4.1 allocate descriptor set failed"
    );


    VkSamplerCreateInfo samplerInfo{
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO
    };


    samplerInfo.magFilter =
        VK_FILTER_NEAREST;

    samplerInfo.minFilter =
        VK_FILTER_NEAREST;

    samplerInfo.mipmapMode =
        VK_SAMPLER_MIPMAP_MODE_NEAREST;

    samplerInfo.addressModeU =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    samplerInfo.addressModeV =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    samplerInfo.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    samplerInfo.mipLodBias =
        0.0f;

    samplerInfo.anisotropyEnable =
        VK_FALSE;

    samplerInfo.maxAnisotropy =
        1.0f;

    samplerInfo.compareEnable =
        VK_FALSE;

    samplerInfo.compareOp =
        VK_COMPARE_OP_ALWAYS;

    samplerInfo.minLod =
        0.0f;

    samplerInfo.maxLod =
        0.0f;

    samplerInfo.borderColor =
        VK_BORDER_COLOR_INT_OPAQUE_BLACK;

    samplerInfo.unnormalizedCoordinates =
        VK_FALSE;


    check(
        vkCreateSampler(
            g_device,
            &samplerInfo,
            nullptr,
            &g_fgUISnapshotSampler
        ),
        "FG-4.1 create sampler failed"
    );


    VkPipelineLayoutCreateInfo
        pipelineLayoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO
        };


    pipelineLayoutInfo.setLayoutCount =
        1;

    pipelineLayoutInfo.pSetLayouts =
        &g_fgUIAlphaDescriptorSetLayout;


    check(
        vkCreatePipelineLayout(
            g_device,
            &pipelineLayoutInfo,
            nullptr,
            &g_fgUIAlphaPipelineLayout
        ),
        "FG-4.1 create pipeline layout failed"
    );


    const std::vector<uint8_t> shaderCode =
        loadSpv(
            "shaders/fg_ui_mask.comp.spv"
        );


    VkShaderModule shaderModule =
        createShaderModule(
            g_device,
            shaderCode
        );


    VkPipelineShaderStageCreateInfo
        stageInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO
        };


    stageInfo.stage =
        VK_SHADER_STAGE_COMPUTE_BIT;

    stageInfo.module =
        shaderModule;

    stageInfo.pName =
        "main";


    VkComputePipelineCreateInfo
        pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };


    pipelineInfo.stage =
        stageInfo;

    pipelineInfo.layout =
        g_fgUIAlphaPipelineLayout;


    VkResult pipelineResult =
        vkCreateComputePipelines(
            g_device,
            getSharedPipelineCache(),
            1,
            &pipelineInfo,
            nullptr,
            &g_fgUIAlphaPipeline
        );


    vkDestroyShaderModule(
        g_device,
        shaderModule,
        nullptr
    );


    check(
        pipelineResult,
        "FG-4.1 create compute pipeline failed"
    );


    fprintf(
        stderr,
        "[FG-4.1 PIPELINE] READY\n"
    );

    fflush(stderr);
}


bool ensureFGUIResources(
    uint32_t width,
    uint32_t height
)
{
    if(
        g_device ==
            VK_NULL_HANDLE ||

        g_physicalDevice ==
            VK_NULL_HANDLE ||

        width == 0 ||

        height == 0 ||

        g_fgBackbufferFormat ==
            VK_FORMAT_UNDEFINED ||

        g_fgUIAlphaPipeline ==
            VK_NULL_HANDLE ||

        g_fgUIAlphaDescriptorSet ==
            VK_NULL_HANDLE ||

        g_fgUISnapshotSampler ==
            VK_NULL_HANDLE
    )
    {
        return false;
    }


    if(
        g_fgHudlessImage !=
            VK_NULL_HANDLE &&

        g_fgFinalImage !=
            VK_NULL_HANDLE &&

        g_fgUIAlphaImage !=
            VK_NULL_HANDLE &&

        g_fgSnapshotFormat ==
            g_fgBackbufferFormat &&

        g_fgUIWidth ==
            width &&

        g_fgUIHeight ==
            height
    )
    {
        return true;
    }


    VkFormatProperties snapshotProperties{};

    vkGetPhysicalDeviceFormatProperties(
        g_physicalDevice,
        g_fgBackbufferFormat,
        &snapshotProperties
    );


    const VkFormatFeatureFlags
        snapshotRequired =
            VK_FORMAT_FEATURE_BLIT_DST_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;


    if(
        (
            snapshotProperties.optimalTilingFeatures &
            snapshotRequired
        ) !=
        snapshotRequired
    )
    {
        fprintf(
            stderr,
            "[FG-4.1 RESOURCES]"
            " unsupported snapshot format=%d"
            " features=0x%X\n",

            static_cast<int>(
                g_fgBackbufferFormat
            ),

            static_cast<unsigned int>(
                snapshotProperties.optimalTilingFeatures
            )
        );

        fflush(stderr);

        return false;
    }


    VkFormatProperties alphaProperties{};

    vkGetPhysicalDeviceFormatProperties(
        g_physicalDevice,
        VK_FORMAT_R32_SFLOAT,
        &alphaProperties
    );


    const VkFormatFeatureFlags
        alphaRequired =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;


    if(
        (
            alphaProperties.optimalTilingFeatures &
            alphaRequired
        ) !=
        alphaRequired
    )
    {
        fprintf(
            stderr,
            "[FG-4.1 RESOURCES]"
            " R32_SFLOAT unsupported"
            " features=0x%X\n",

            static_cast<unsigned int>(
                alphaProperties.optimalTilingFeatures
            )
        );

        fflush(stderr);

        return false;
    }

    if(
        g_fgHudlessImage !=
            VK_NULL_HANDLE ||

        g_fgFinalImage !=
            VK_NULL_HANDLE ||

        g_fgUIAlphaImage !=
            VK_NULL_HANDLE
    )
    {
        const VkResult idleResult =
            FGDeviceWaitIdle(
                g_device
            );


        if(
            idleResult !=
            VK_SUCCESS
        )
        {
            fprintf(
                stderr,
                "[FG-4.1 RESOURCES]"
                " waitIdle failed result=%d\n",

                static_cast<int>(
                    idleResult
                )
            );

            fflush(stderr);

            return false;
        }
    }


    destroyFGUIImages();


    try
    {
        createFGImage(
            width,
            height,
            g_fgBackbufferFormat,

            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,

            g_fgHudlessImage,
            g_fgHudlessMemory,
            g_fgHudlessView
        );


        createFGImage(
            width,
            height,
            g_fgBackbufferFormat,

            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,

            g_fgFinalImage,
            g_fgFinalMemory,
            g_fgFinalView
        );


        createFGImage(
            width,
            height,
            VK_FORMAT_R32_SFLOAT,

            VK_IMAGE_USAGE_STORAGE_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,

            g_fgUIAlphaImage,
            g_fgUIAlphaMemory,
            g_fgUIAlphaView
        );
    }
    catch(const std::exception& e)
    {
        fprintf(
            stderr,
            "[FG-4.1 RESOURCES]"
            " create failed: %s\n",
            e.what()
        );

        fflush(stderr);

        destroyFGUIImages();

        return false;
    }


    g_fgSnapshotFormat =
        g_fgBackbufferFormat;

    g_fgUIWidth =
        width;

    g_fgUIHeight =
        height;


    g_fgHudlessLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;

    g_fgFinalLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;

    g_fgUIAlphaLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;


    updateFGUIAlphaDescriptor();


    fprintf(
        stderr,
        "[FG-4.1 RESOURCES]"
        " size=%ux%u"
        " snapshotFormat=%d"
        " uiAlphaFormat=R32_SFLOAT\n",

        width,
        height,

        static_cast<int>(
            g_fgSnapshotFormat
        )
    );

    fflush(stderr);


    return true;
}


// 2026-10-27：1x1x1 的 3D 占位图（storage image 的格式必须与 shader 声明一致）。
void createPlaceholder3DImage(
    VkFormat format,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view)
{
    if(image != VK_NULL_HANDLE)
    {
        return;
    }

    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_3D;
    info.format = format;
    info.extent = { 1u, 1u, 1u };
    info.mipLevels = 1u;
    info.arrayLayers = 1u;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(g_device, &info, nullptr, &image), "placeholder 3D image failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, image, &requirements);

    VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = findMemoryType(
        g_physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocate, nullptr, &memory), "placeholder 3D memory failed");
    check(vkBindImageMemory(g_device, image, memory, 0u), "placeholder 3D bind failed");

    VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &view), "placeholder 3D view failed");
}

// 首次使用前的清零。放在帧命令里而不是建图时（建图路径没有现成的 cmd）。
// 三件事必须按顺序：UNDEFINED→TRANSFER_DST（丢内容）→ clear → TRANSFER_DST→GENERAL。
void clearGiHalfImageIfNeeded(VkCommandBuffer cmd)
{
    if(!g_giHalfNeedsClear || g_giHalfImage == VK_NULL_HANDLE)
    {
        return;
    }

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0u;
    range.levelCount = 1u;
    range.baseArrayLayer = 0u;
    range.layerCount = 1u;

    VkImageMemoryBarrier toTransfer{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toTransfer.srcAccessMask = 0u;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = g_giHalfImage;
    toTransfer.subresourceRange = range;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
            VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);

    VkClearColorValue clear{};
    clear.float32[0] = 0.0f;
    clear.float32[1] = 0.0f;
    clear.float32[2] = 0.0f;
    clear.float32[3] = 60000.0f;
    vkCmdClearColorImage(
        cmd,
        g_giHalfImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &clear,
        1u,
        &range);

    VkImageMemoryBarrier toGeneral{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toGeneral.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toGeneral.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = g_giHalfImage;
    toGeneral.subresourceRange = range;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        0u, 0u, nullptr, 0u, nullptr, 1u, &toGeneral);

    g_giHalfLayout = VK_IMAGE_LAYOUT_GENERAL;
    g_giHalfNeedsClear = false;
}


void createRTOutputImage(uint32_t width,uint32_t height){
    if(g_rtOutputImage &&
       (g_rtOutputWidth != width ||
        g_rtOutputHeight != height))
    {
        g_hasPrevCameraMatrices =
            false;

        g_cameraHistoryValid =
            false;

        g_lastCameraFrame =
            -1;
    }


    std::cerr
<< "[RT IMAGE CREATE ENTER] old="
<< g_rtOutputImage
<< " size="
<< g_rtOutputWidth
<< "x"
<< g_rtOutputHeight
<< " new="
<< width
<< "x"
<< height
<< std::endl;
    if(g_rtOutputImage &&
       g_rtOutputWidth == width &&
       g_rtOutputHeight == height)
    {
        return;
    }


    if(g_rtOutputImage != VK_NULL_HANDLE)
    {
        check(
            FGDeviceWaitIdle(g_device),
            "wait device before RT output recreate failed"
        );
    }


    if(g_rtOutputView)
    {
        vkDestroyImageView(
            g_device,
            g_rtOutputView,
            nullptr
        );

        g_rtOutputView =
            VK_NULL_HANDLE;
    }
    if(g_rtOutputImage){vkDestroyImage(g_device,g_rtOutputImage,nullptr);g_rtOutputImage=VK_NULL_HANDLE;}
    if(g_rtOutputMemory){vkFreeMemory(g_device,g_rtOutputMemory,nullptr);g_rtOutputMemory=VK_NULL_HANDLE;}
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R16G16B16A16_SFLOAT;;ii.extent={width,height,1};ii.mipLevels=1;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=
    VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(g_device,&ii,nullptr,&g_rtOutputImage),"Failed to create RT output image");
    std::cout
<<"[RT OUTPUT] image created "
<<width<<"x"<<height
<<"\n";
    VkMemoryRequirements mr{};vkGetImageMemoryRequirements(g_device,g_rtOutputImage,&mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=mr.size;ai.memoryTypeIndex=findMemoryType(g_physicalDevice,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkResult allocResult =
vkAllocateMemory(
    g_device,
    &ai,
    nullptr,
    &g_rtOutputMemory
);

    if(allocResult!=VK_SUCCESS)
    {
         std::cout
        <<"[RT OUTPUT] allocate failed "
        <<allocResult
        <<" size="
        <<ai.allocationSize
        <<"\n";

        vkDestroyImage(
            g_device,
            g_rtOutputImage,
            nullptr
        );

        g_rtOutputImage=VK_NULL_HANDLE;

        return;
    }check(vkBindImageMemory(g_device,g_rtOutputImage,g_rtOutputMemory,0),"Failed to bind RT output image");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=g_rtOutputImage;
    vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=VK_FORMAT_R16G16B16A16_SFLOAT;vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;vi.subresourceRange.levelCount=1;vi.subresourceRange.layerCount=1;
    check(vkCreateImageView(g_device,&vi,nullptr,&g_rtOutputView),"Failed to create RT output view");


    updateRTOutputDescriptor();

    std::cout
        << "[RT] updateRTOutputDescriptor called\n";

    g_rtOutputWidth = width;
    g_rtOutputHeight = height;


    g_rtOutputLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;
}
void createDLSSDepthImage(
    uint32_t width,
    uint32_t height)
{
    check(
        FGDeviceWaitIdle(g_device),
        "wait device before DLSS depth recreate failed"
    );


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


    g_dlssDepthLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;


    VkImageCreateInfo ci{
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
    };

    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format =
        VK_FORMAT_R32_SFLOAT;
    ci.extent = {width,height,1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;

    ci.usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;


    check(
        vkCreateImage(
            g_device,
            &ci,
            nullptr,
            &g_dlssDepthImage
        ),
        "create depth image failed"
    );


    VkMemoryRequirements mr{};

    vkGetImageMemoryRequirements(
        g_device,
        g_dlssDepthImage,
        &mr
    );


    VkMemoryAllocateInfo ai{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };

    ai.allocationSize = mr.size;

    ai.memoryTypeIndex =
        findMemoryType(
            g_physicalDevice,
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );


    check(
        vkAllocateMemory(
            g_device,
            &ai,
            nullptr,
            &g_dlssDepthMemory
        ),
        "allocate depth memory failed"
    );


    check(
        vkBindImageMemory(
            g_device,
            g_dlssDepthImage,
            g_dlssDepthMemory,
            0
        ),
        "bind depth memory failed"
    );


    VkImageViewCreateInfo vi{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
    };

    vi.image = g_dlssDepthImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32_SFLOAT;

    vi.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;


    check(
    vkCreateImageView(
        g_device,
        &vi,
        nullptr,
        &g_dlssDepthView
    ),
    "create depth view failed"
);



    VkDescriptorImageInfo depthDescriptorInfo{};

    depthDescriptorInfo.imageView =
        g_dlssDepthView;

    depthDescriptorInfo.imageLayout =
        VK_IMAGE_LAYOUT_GENERAL;


    VkWriteDescriptorSet depthWrite{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
    };

    depthWrite.dstSet =
        g_rayTracingDescriptorSet;

    depthWrite.dstBinding =
        7;

    depthWrite.dstArrayElement =
        0;

    depthWrite.descriptorCount =
        1;

    depthWrite.descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    depthWrite.pImageInfo =
        &depthDescriptorInfo;


    vkUpdateDescriptorSets(
        g_device,
        1,
        &depthWrite,
        0,
        nullptr
    );


    std::cerr
        << "[DLSS DEPTH DESCRIPTOR]"
        << " binding=7"
        << " view="
        << g_dlssDepthView
        << " size="
        << width
        << "x"
        << height
        << " layout=GENERAL"
        << std::endl;


    g_dlssDepthWidth = width;
    g_dlssDepthHeight = height;

    g_dlssDepthLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;


    std::cout
        << "[DLSS] Depth image created "
        << width<<"x"<<height
        << std::endl;

    ++g_rrGuideViewGeneration;
}
void createMotionImage(uint32_t width,uint32_t height)
{
    if(g_motionImage &&
       g_motionWidth==width &&
       g_motionHeight==height)
        return;


    if(g_motionView != VK_NULL_HANDLE ||
       g_motionImage != VK_NULL_HANDLE ||
       g_motionMemory != VK_NULL_HANDLE)
    {
        check(
            FGDeviceWaitIdle(g_device),
            "wait device before DLSS motion recreate failed"
        );

        g_hasPrevCameraMatrices =
            false;
    }


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


    VkImageCreateInfo ci{
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
    };

    ci.imageType = VK_IMAGE_TYPE_2D;

    ci.format = VK_FORMAT_R16G16_SFLOAT;

    ci.extent =
    {
        width,
        height,
        1
    };

    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;

    ci.usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    ci.initialLayout =
        VK_IMAGE_LAYOUT_GENERAL;


    VkFormatProperties motionFormatProps{};

    vkGetPhysicalDeviceFormatProperties(
        g_physicalDevice,
        VK_FORMAT_R16G16_SFLOAT,
        &motionFormatProps
    );

    if((motionFormatProps.optimalTilingFeatures &
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT) == 0)
    {
        throw std::runtime_error(
            "VK_FORMAT_R16G16_SFLOAT does not support TRANSFER_DST"
        );
    }


    check(
        vkCreateImage(
            g_device,
            &ci,
            nullptr,
            &g_motionImage
        ),
        "create motion image failed"
    );



    VkMemoryRequirements mr{};

    vkGetImageMemoryRequirements(
        g_device,
        g_motionImage,
        &mr
    );


    VkMemoryAllocateInfo ai{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };

    ai.allocationSize = mr.size;

    ai.memoryTypeIndex =
        findMemoryType(
            g_physicalDevice,
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );


    check(
        vkAllocateMemory(
            g_device,
            &ai,
            nullptr,
            &g_motionMemory
        ),
        "motion memory failed"
    );


    check(
        vkBindImageMemory(
            g_device,
            g_motionImage,
            g_motionMemory,
            0
        ),
        "bind motion failed"
    );



    VkImageViewCreateInfo vi{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
    };

    vi.image=g_motionImage;
    vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=VK_FORMAT_R16G16_SFLOAT;

    vi.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    vi.subresourceRange.levelCount=1;
    vi.subresourceRange.layerCount=1;


    check(
        vkCreateImageView(
            g_device,
            &vi,
            nullptr,
            &g_motionView
        ),
        "motion view failed"
    );


    if(g_rayTracingDescriptorSet != VK_NULL_HANDLE)
    {
        VkDescriptorImageInfo motionDescriptorInfo{};

        motionDescriptorInfo.imageView =
            g_motionView;

        motionDescriptorInfo.imageLayout =
            VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet motionWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET
        };

        motionWrite.dstSet =
            g_rayTracingDescriptorSet;

        motionWrite.dstBinding =
            62;

        motionWrite.descriptorCount =
            1;

        motionWrite.descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        motionWrite.pImageInfo =
            &motionDescriptorInfo;

        vkUpdateDescriptorSets(
            g_device,
            1,
            &motionWrite,
            0,
            nullptr
        );
    }


    g_motionWidth=width;
    g_motionHeight=height;
    g_motionLayout =
    VK_IMAGE_LAYOUT_GENERAL;


    std::cerr
    << "[DLSS] Motion image created "
    << width<<"x"<<height
    << std::endl;
}


void destroyBloomResources()
{
    vkDestroyPipeline(g_device, g_bloomPrefilterPipeline, nullptr);
    vkDestroyPipeline(g_device, g_bloomDownsamplePipeline, nullptr);
    vkDestroyPipeline(g_device, g_bloomUpsamplePipeline, nullptr);
    vkDestroyPipeline(g_device, g_bloomCompositePipeline, nullptr);
    g_bloomPrefilterPipeline = VK_NULL_HANDLE;
    g_bloomDownsamplePipeline = VK_NULL_HANDLE;
    g_bloomUpsamplePipeline = VK_NULL_HANDLE;
    g_bloomCompositePipeline = VK_NULL_HANDLE;

    vkDestroyPipelineLayout(g_device, g_bloomPrefilterPipelineLayout, nullptr);
    vkDestroyPipelineLayout(g_device, g_bloomBlurPipelineLayout, nullptr);
    vkDestroyPipelineLayout(g_device, g_bloomCompositePipelineLayout, nullptr);
    g_bloomPrefilterPipelineLayout = VK_NULL_HANDLE;
    g_bloomBlurPipelineLayout = VK_NULL_HANDLE;
    g_bloomCompositePipelineLayout = VK_NULL_HANDLE;

    vkDestroyDescriptorPool(g_device, g_bloomDescriptorPool, nullptr);
    g_bloomDescriptorPool = VK_NULL_HANDLE;

    vkDestroyDescriptorSetLayout(g_device, g_bloomPrefilterSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(g_device, g_bloomBlurSetLayout, nullptr);
    vkDestroyDescriptorSetLayout(g_device, g_bloomCompositeSetLayout, nullptr);
    g_bloomPrefilterSetLayout = VK_NULL_HANDLE;
    g_bloomBlurSetLayout = VK_NULL_HANDLE;
    g_bloomCompositeSetLayout = VK_NULL_HANDLE;

    g_bloomPrefilterSet = VK_NULL_HANDLE;
    g_bloomCompositeSet = VK_NULL_HANDLE;

    for(uint32_t level = 0u; level < BLOOM_MIP_COUNT; ++level)
    {
        g_bloomDownsampleSets[level] = VK_NULL_HANDLE;
        g_bloomUpsampleSets[level] = VK_NULL_HANDLE;
    }

    for(uint32_t level = 0u; level < BLOOM_MIP_COUNT; ++level)
    {
        if(g_bloomMips[level].view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(g_device, g_bloomMips[level].view, nullptr);
        }

        if(g_bloomMips[level].image != VK_NULL_HANDLE)
        {
            vkDestroyImage(g_device, g_bloomMips[level].image, nullptr);
        }

        if(g_bloomMips[level].memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, g_bloomMips[level].memory, nullptr);
        }

        g_bloomMips[level] = BloomMip{};
    }
}
void createBloomMipImage(BloomMip& mip, uint32_t width, uint32_t height)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    imageInfo.extent = {width, height, 1u};
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(g_device, &imageInfo, nullptr, &mip.image), "bloom mip image failed");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g_device, mip.image, &requirements);

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = findMemoryType(
        g_physicalDevice, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g_device, &allocation, nullptr, &mip.memory), "bloom mip memory failed");
    check(vkBindImageMemory(g_device, mip.image, mip.memory, 0u), "bloom mip bind failed");

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = mip.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1u;
    viewInfo.subresourceRange.layerCount = 1u;
    check(vkCreateImageView(g_device, &viewInfo, nullptr, &mip.view), "bloom mip view failed");

    mip.width = width;
    mip.height = height;
}

// 尺寸键控：在 createDLSSOutputImage 里调用（与输出图同寿命、同 resize 时机）。
void createBloomResources(uint32_t outputWidth, uint32_t outputHeight)
{
    check(FGDeviceWaitIdle(g_device), "wait device before bloom recreate failed");

    destroyBloomResources();

    if(outputWidth == 0u || outputHeight == 0u)
    {
        return;
    }

    for(uint32_t level = 0u; level < BLOOM_MIP_COUNT; ++level)
    {
        const uint32_t divisor = 1u << (level + 1u);
        const uint32_t width = std::max(1u, outputWidth / divisor);
        const uint32_t height = std::max(1u, outputHeight / divisor);
        createBloomMipImage(g_bloomMips[level], width, height);
    }
    if(g_bloomSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(g_device, &samplerInfo, nullptr, &g_bloomSampler), "bloom sampler failed");
    }

    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 2u;
        info.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(g_device, &info, nullptr, &g_bloomPrefilterSetLayout),
            "bloom prefilter set layout failed");
    }

    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 2u;
        info.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(g_device, &info, nullptr, &g_bloomBlurSetLayout),
            "bloom blur set layout failed");
    }

    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        bindings[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = 2u;
        info.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(g_device, &info, nullptr, &g_bloomCompositeSetLayout),
            "bloom composite set layout failed");
    }

    {
        VkDescriptorPoolSize poolSizes[2] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u + (BLOOM_MIP_COUNT - 1u) * 2u + 1u },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u + (BLOOM_MIP_COUNT - 1u) * 2u + 1u }
        };
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = (BLOOM_MIP_COUNT - 1u) * 2u + 2u;
        poolInfo.poolSizeCount = 2u;
        poolInfo.pPoolSizes = poolSizes;
        check(vkCreateDescriptorPool(g_device, &poolInfo, nullptr, &g_bloomDescriptorPool),
            "bloom descriptor pool failed");
    }

    {
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool = g_bloomDescriptorPool;

        allocation.descriptorSetCount = 1u;
        allocation.pSetLayouts = &g_bloomPrefilterSetLayout;
        check(vkAllocateDescriptorSets(g_device, &allocation, &g_bloomPrefilterSet),
            "bloom prefilter set failed");

        for(uint32_t level = 1u; level < BLOOM_MIP_COUNT; ++level)
        {
            allocation.pSetLayouts = &g_bloomBlurSetLayout;
            check(vkAllocateDescriptorSets(g_device, &allocation, &g_bloomDownsampleSets[level]),
                "bloom downsample set failed");
            check(vkAllocateDescriptorSets(g_device, &allocation, &g_bloomUpsampleSets[level]),
                "bloom upsample set failed");
        }

        allocation.pSetLayouts = &g_bloomCompositeSetLayout;
        check(vkAllocateDescriptorSets(g_device, &allocation, &g_bloomCompositeSet),
            "bloom composite set failed");
    }

    {
        VkDescriptorImageInfo sceneInfo{};
        sceneInfo.sampler = g_bloomSampler;
        sceneInfo.imageView = g_dlssOutputView;
        sceneInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo mipInfo[BLOOM_MIP_COUNT]{};
        for(uint32_t level = 0u; level < BLOOM_MIP_COUNT; ++level)
        {
            mipInfo[level].sampler = g_bloomSampler;
            mipInfo[level].imageView = g_bloomMips[level].view;
            mipInfo[level].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }

        VkWriteDescriptorSet writes[(BLOOM_MIP_COUNT - 1u) * 4u + 4u]{};
        uint32_t writeCount = 0u;

        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_bloomPrefilterSet, 0u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sceneInfo, nullptr, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_bloomPrefilterSet, 1u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &mipInfo[0], nullptr, nullptr };

        for(uint32_t level = 1u; level < BLOOM_MIP_COUNT; ++level)
        {
            writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_bloomDownsampleSets[level], 0u, 0u, 1u,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mipInfo[level - 1u], nullptr, nullptr };
            writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_bloomDownsampleSets[level], 1u, 0u, 1u,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &mipInfo[level], nullptr, nullptr };
            writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_bloomUpsampleSets[level], 0u, 0u, 1u,
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mipInfo[level], nullptr, nullptr };
            writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                g_bloomUpsampleSets[level], 1u, 0u, 1u,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &mipInfo[level - 1u], nullptr, nullptr };
        }

        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_bloomCompositeSet, 0u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mipInfo[0], nullptr, nullptr };
        writes[writeCount++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
            g_bloomCompositeSet, 1u, 0u, 1u,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &sceneInfo, nullptr, nullptr };

        vkUpdateDescriptorSets(g_device, writeCount, writes, 0u, nullptr);
    }

    {
        VkPipelineLayoutCreateInfo prefilterLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        prefilterLayoutInfo.setLayoutCount = 1u;
        prefilterLayoutInfo.pSetLayouts = &g_bloomPrefilterSetLayout;
        check(vkCreatePipelineLayout(g_device, &prefilterLayoutInfo, nullptr,
            &g_bloomPrefilterPipelineLayout), "bloom prefilter layout failed");

        VkPipelineLayoutCreateInfo blurLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        blurLayoutInfo.setLayoutCount = 1u;
        blurLayoutInfo.pSetLayouts = &g_bloomBlurSetLayout;
        check(vkCreatePipelineLayout(g_device, &blurLayoutInfo, nullptr,
            &g_bloomBlurPipelineLayout), "bloom blur layout failed");

        VkPushConstantRange compositePushRange{};
        compositePushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        compositePushRange.offset = 0u;
        compositePushRange.size = static_cast<uint32_t>(sizeof(BloomCompositePush));
        VkPipelineLayoutCreateInfo compositeLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        compositeLayoutInfo.setLayoutCount = 1u;
        compositeLayoutInfo.pSetLayouts = &g_bloomCompositeSetLayout;
        compositeLayoutInfo.pushConstantRangeCount = 1u;
        compositeLayoutInfo.pPushConstantRanges = &compositePushRange;
        check(vkCreatePipelineLayout(g_device, &compositeLayoutInfo, nullptr,
            &g_bloomCompositePipelineLayout), "bloom composite layout failed");

        const char* shaderPaths[4] = {
            "shaders/bloom_prefilter.comp.spv",
            "shaders/bloom_downsample.comp.spv",
            "shaders/bloom_upsample.comp.spv",
            "shaders/bloom_composite.comp.spv"
        };
        VkPipelineLayout pipelineLayouts[4] = {
            g_bloomPrefilterPipelineLayout,
            g_bloomBlurPipelineLayout,
            g_bloomBlurPipelineLayout,
            g_bloomCompositePipelineLayout
        };
        VkPipeline* pipelines[4] = {
            &g_bloomPrefilterPipeline,
            &g_bloomDownsamplePipeline,
            &g_bloomUpsamplePipeline,
            &g_bloomCompositePipeline
        };

        for(uint32_t index = 0u; index < 4u; ++index)
        {
            const std::vector<uint8_t> code = loadSpv(shaderPaths[index]);
            VkShaderModule shader = createShaderModule(g_device, code);
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            stage.module = shader;
            stage.pName = "main";
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = stage;
            pipelineInfo.layout = pipelineLayouts[index];
            const VkResult result = vkCreateComputePipelines(
                g_device, getSharedPipelineCache(), 1u, &pipelineInfo, nullptr, pipelines[index]);
            vkDestroyShaderModule(g_device, shader, nullptr);
            check(result, "bloom pipeline failed");
        }
    }

    g_hasPrevCameraMatrices = false;

    std::cout << "[BLOOM] resources created " << g_bloomMips[0].width << "x" << g_bloomMips[0].height
        << " mips=" << BLOOM_MIP_COUNT << std::endl;
}

// 录在显示变换之前（见上面那段口径说明）。strength = 0 时合成 pass 整屏早退。
void recordBloom(VkCommandBuffer cmd)
{
    if(cmd == VK_NULL_HANDLE ||
       g_dlssOutputImage == VK_NULL_HANDLE ||
       g_dlssOutputWidth == 0u ||
       g_dlssOutputHeight == 0u)
    {
        return;
    }

    const uint32_t halfWidth = std::max(1u, g_dlssOutputWidth / 2u);
    const uint32_t halfHeight = std::max(1u, g_dlssOutputHeight / 2u);

    if(g_bloomMips[0].image == VK_NULL_HANDLE ||
       g_bloomMips[0].width != halfWidth ||
       g_bloomMips[0].height != halfHeight)
    {
        createBloomResources(g_dlssOutputWidth, g_dlssOutputHeight);
    }

    if(g_bloomPrefilterPipeline == VK_NULL_HANDLE ||
       g_bloomCompositePipeline == VK_NULL_HANDLE ||
       g_bloomMips[0].image == VK_NULL_HANDLE)
    {
        return;
    }

    {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_bloomPrefilterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_bloomPrefilterPipelineLayout, 0u, 1u, &g_bloomPrefilterSet, 0u, nullptr);
    vkCmdDispatch(cmd,
        (g_bloomMips[0].width + 7u) / 8u,
        (g_bloomMips[0].height + 7u) / 8u, 1u);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_bloomDownsamplePipeline);
    for(uint32_t level = 1u; level < BLOOM_MIP_COUNT; ++level)
    {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_bloomBlurPipelineLayout, 0u, 1u, &g_bloomDownsampleSets[level], 0u, nullptr);
        vkCmdDispatch(cmd,
            (g_bloomMips[level].width + 7u) / 8u,
            (g_bloomMips[level].height + 7u) / 8u, 1u);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_bloomUpsamplePipeline);
    for(uint32_t level = BLOOM_MIP_COUNT; level-- > 1u; )
    {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &barrier, 0u, nullptr, 0u, nullptr);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            g_bloomBlurPipelineLayout, 0u, 1u, &g_bloomUpsampleSets[level], 0u, nullptr);
        vkCmdDispatch(cmd,
            (g_bloomMips[level - 1u].width + 7u) / 8u,
            (g_bloomMips[level - 1u].height + 7u) / 8u, 1u);
    }

    BloomCompositePush push{};
    push.strength = g_postBloomStrength.load();

    VkMemoryBarrier compositeBarrier{};
    compositeBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    compositeBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    compositeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 1u, &compositeBarrier, 0u, nullptr, 0u, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_bloomCompositePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_bloomCompositePipelineLayout, 0u, 1u, &g_bloomCompositeSet, 0u, nullptr);
    vkCmdPushConstants(cmd, g_bloomCompositePipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(BloomCompositePush), &push);
    vkCmdDispatch(cmd,
        (g_dlssOutputWidth + 7u) / 8u,
        (g_dlssOutputHeight + 7u) / 8u, 1u);
}
void createDLSSOutputImage(uint32_t width,uint32_t height)
{
    check(
        FGDeviceWaitIdle(g_device),
        "wait device before DLSS output recreate failed"
    );


    g_hasPrevCameraMatrices =
        false;


    if(g_dlssOutputView)
    {
        vkDestroyImageView(
            g_device,
            g_dlssOutputView,
            nullptr
        );

        g_dlssOutputView = VK_NULL_HANDLE;
    }


    if(g_dlssOutputImage)
    {
        vkDestroyImage(
            g_device,
            g_dlssOutputImage,
            nullptr
        );

        g_dlssOutputImage = VK_NULL_HANDLE;
    }


    if(g_dlssOutputMemory)
    {
        vkFreeMemory(
            g_device,
            g_dlssOutputMemory,
            nullptr
        );

        g_dlssOutputMemory = VK_NULL_HANDLE;
    }


    VkImageCreateInfo ci{
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
    };

    ci.imageType =
        VK_IMAGE_TYPE_2D;

    ci.format =
        VK_FORMAT_R16G16B16A16_SFLOAT;

    ci.extent =
    {
        width,
        height,
        1
    };

    ci.mipLevels = 1;
    ci.arrayLayers = 1;

    ci.samples =
        VK_SAMPLE_COUNT_1_BIT;

    ci.tiling =
        VK_IMAGE_TILING_OPTIMAL;


    ci.usage =
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;


    ci.initialLayout =
        VK_IMAGE_LAYOUT_UNDEFINED;


    check(
        vkCreateImage(
            g_device,
            &ci,
            nullptr,
            &g_dlssOutputImage
        ),
        "create DLSS output image failed"
    );

    VkMemoryRequirements mr{};

    vkGetImageMemoryRequirements(
        g_device,
        g_dlssOutputImage,
        &mr
    );


    VkMemoryAllocateInfo ai{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO
    };

    ai.allocationSize =
        mr.size;

    ai.memoryTypeIndex =
        findMemoryType(
            g_physicalDevice,
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
        );


    check(
        vkAllocateMemory(
            g_device,
            &ai,
            nullptr,
            &g_dlssOutputMemory
        ),
        "allocate DLSS output memory failed"
    );


    check(
        vkBindImageMemory(
            g_device,
            g_dlssOutputImage,
            g_dlssOutputMemory,
            0
        ),
        "bind DLSS output memory failed"
    );


    VkImageViewCreateInfo vi{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
    };

    vi.image =
        g_dlssOutputImage;

    vi.viewType =
        VK_IMAGE_VIEW_TYPE_2D;

    vi.format =
        VK_FORMAT_R16G16B16A16_SFLOAT;


    vi.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;

    vi.subresourceRange.levelCount=1;
    vi.subresourceRange.layerCount=1;


    check(
        vkCreateImageView(
            g_device,
            &vi,
            nullptr,
            &g_dlssOutputView
        ),
        "create DLSS output view failed"
    );
    updateSDRPostDescriptor();


    g_dlssOutputWidth=width;
    g_dlssOutputHeight=height;
    g_dlssOutputLayout =
    VK_IMAGE_LAYOUT_UNDEFINED;


    std::cerr
    << "[DLSS] Output image created "
    << width
    << "x"
    << height
    << std::endl;
}
