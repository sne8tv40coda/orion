// Orion backend — RtPipeline 模块。
// 2026-11-01 规范化拆分：本文件由单文件 VulkanCore.cpp 机械拆出，
// 函数体保持逐字不变；共享声明见 OrionInternal.h。

#include "OrionInternal.h"
#include "TrackingHooks.h"

void createTLAS(){
    std::cout << "[RT] createTLAS ENTER\n";
    std::cout
<< "TLAS BUILT FLAG="
<< g_asBuilt
<< "\n";
    auto sizes=getGetAccelerationStructureBuildSizes();auto createAS=getCreateAccelerationStructure();auto getAddr=getGetAccelerationStructureDeviceAddress();
    if(!sizes||!createAS||!getAddr)throw std::runtime_error("TLAS functions unavailable");

    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0]=1.0f;
    inst.transform.matrix[1][1]=1.0f;
    inst.transform.matrix[2][2]=1.0f;

    inst.mask=0xff;
    inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
     std::cout
<< "BLAS ADDRESS="
<< g_blasDeviceAddress
<< "\n";
    inst.accelerationStructureReference=g_blasDeviceAddress;
    createBuffer(
    g_device,
    g_physicalDevice,
    sizeof(inst),
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    g_tlasInstanceBuffer,
    g_tlasInstanceMemory
);
    void* mapped=nullptr;check(vkMapMemory(g_device,g_tlasInstanceMemory,0,sizeof(inst),0,&mapped),"Failed to map TLAS instance buffer");std::memcpy(mapped,&inst,sizeof(inst));vkUnmapMemory(g_device,g_tlasInstanceMemory);
    g_tlasInstanceDeviceAddress=getBufferDeviceAddress(g_device,g_tlasInstanceBuffer);
    VkAccelerationStructureGeometryInstancesDataKHR id{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};id.data.deviceAddress=g_tlasInstanceDeviceAddress;
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};geo.geometryType=VK_GEOMETRY_TYPE_INSTANCES_KHR;geo.geometry.instances=id;
    VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};bi.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;bi.flags=VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;bi.geometryCount=1;bi.pGeometries=&geo;bi.mode=VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    uint32_t pc=1;VkAccelerationStructureBuildSizesInfoKHR si{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};sizes(g_device,VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,&bi,&pc,&si);
    createBuffer(g_device,g_physicalDevice,si.accelerationStructureSize,VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,g_tlasBuffer,g_tlasMemory);
    g_tlasAsSize=si.accelerationStructureSize;
    VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};aci.buffer=g_tlasBuffer;aci.size=si.accelerationStructureSize;aci.type=VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;check(createAS(g_device,&aci,nullptr,&g_tlas),"Failed to create TLAS");
     std::cout
    << "[RT INIT] TLAS created = "
    << g_tlas
    << "\n";
    createBuffer(g_device,g_physicalDevice,si.buildScratchSize,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,g_tlasScratchBuffer,g_tlasScratchMemory);
    g_tlasScratchCapacity=si.buildScratchSize;
    g_tlasScratchAddress=getBufferDeviceAddress(g_device,g_tlasScratchBuffer);
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};ai.accelerationStructure=g_tlas;g_tlasDeviceAddress=getAddr(g_device,&ai);VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
    VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
};

buildInfo.type =
    VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

buildInfo.flags =
    VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

buildInfo.mode =
    VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;

buildInfo.dstAccelerationStructure =
    g_tlas;

buildInfo.geometryCount = 1;

buildInfo.pGeometries =
    &geo;

buildInfo.scratchData.deviceAddress =
    g_tlasScratchAddress;


VkAccelerationStructureBuildRangeInfoKHR range{};

range.primitiveCount = 1;


VkCommandBufferAllocateInfo cai{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO
};

cai.commandPool =
    g_rtCommandPool;

cai.level =
    VK_COMMAND_BUFFER_LEVEL_PRIMARY;

cai.commandBufferCount = 1;


VkCommandBuffer cmd;

check(
    vkAllocateCommandBuffers(
        g_device,
        &cai,
        &cmd
    ),
    "allocate TLAS command buffer failed"
);


VkCommandBufferBeginInfo bi2{
    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
};

bi2.flags =
    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;


check(
    vkBeginCommandBuffer(
        cmd,
        &bi2
    ),
    "begin TLAS cmd failed"
);


auto build =
reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
    vkGetDeviceProcAddr(
        g_device,
        "vkCmdBuildAccelerationStructuresKHR"
    )
);


if(!build)
    throw std::runtime_error(
        "vkCmdBuildAccelerationStructuresKHR missing"
    );


const VkAccelerationStructureBuildRangeInfoKHR* ranges[] =
{
    &range
};


build(
    cmd,
    1,
    &buildInfo,
    ranges
);


check(
    vkEndCommandBuffer(cmd),
    "end TLAS cmd failed"
);


VkSubmitInfo submit{
    VK_STRUCTURE_TYPE_SUBMIT_INFO
};

submit.commandBufferCount = 1;
submit.pCommandBuffers = &cmd;


check(
    vkQueueSubmit(
        g_queue,
        1,
        &submit,
        VK_NULL_HANDLE
    ),
    "submit TLAS build failed"
);
     std::cout
<< "[RT] TLAS SUBMITTED\n";


PERF_L10_TIME_STALL("tlasQueueIdle", check(
    vkQueueWaitIdle(
        g_queue
    ),
    "wait TLAS build failed"
));
    std::cout
<< "[RT] TLAS FINISHED\n";


    vkFreeCommandBuffers(
        g_device,
        g_rtCommandPool,
        1,
        &cmd
    );

    std::cout
<< "[RT INIT] TLAS BUILD COMPLETE\n";
}
VkDescriptorSetLayout createRTDescriptorSetLayout(){
    VkDescriptorSetLayoutBinding b[80]{};

    b[0].binding=0; b[0].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR; b[0].descriptorCount=1; b[0].stageFlags=VK_SHADER_STAGE_RAYGEN_BIT_KHR|VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    b[1].binding=1; b[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[1].descriptorCount=1; b[1].stageFlags=VK_SHADER_STAGE_RAYGEN_BIT_KHR|VK_SHADER_STAGE_COMPUTE_BIT;
    b[2].binding=2; b[2].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; b[2].descriptorCount=1;
    b[2].stageFlags=VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;
    b[3].binding=3; b[3].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[3].descriptorCount=1; b[3].stageFlags=VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    b[4].binding=4; b[4].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[4].descriptorCount=1; b[4].stageFlags=VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    b[5].binding=5; b[5].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[5].descriptorCount=1; b[5].stageFlags=VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[6].binding=6; b[6].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[6].descriptorCount=1; b[6].stageFlags=VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    b[7].binding = 7;
    b[7].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[7].descriptorCount = 1;
    b[7].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    b[8].binding = 8;
    b[8].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[8].descriptorCount = 1;
    b[8].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[9].binding = 9;
    b[9].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[9].descriptorCount = 1;
    b[9].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[10].binding = 10;
    b[10].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[10].descriptorCount = 1;
    b[10].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[11].binding = 11;
    b[11].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[11].descriptorCount = 1;
    b[11].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[12].binding = 12;
    b[12].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[12].descriptorCount = 1;
    b[12].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[13].binding = 13;
    b[13].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[13].descriptorCount = 1;
    b[13].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[14].binding = 14;
    b[14].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[14].descriptorCount = 1;
    b[14].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[15].binding = 15;
    b[15].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[15].descriptorCount = 1;
    b[15].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[16].binding = 16;
    b[16].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[16].descriptorCount = 1;
    b[16].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[17].binding = 17;
    b[17].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[17].descriptorCount = 1;
    b[17].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR |
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;


    b[18].binding = 18;
    b[18].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[18].descriptorCount = 1;
    b[18].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[19].binding = 21;
    b[19].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[19].descriptorCount = 1;
    b[19].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR |
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR;


    b[20].binding = 22;
    b[20].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[20].descriptorCount = 1;
    b[20].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;

    b[21].binding = 23;
    b[21].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[21].descriptorCount = 1;
    b[21].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;

    b[22].binding = 24;
    b[22].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[22].descriptorCount = 1;
    b[22].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[23].binding = 25;
    b[23].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[23].descriptorCount = 1;
    b[23].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    b[24].binding = 26;
    b[24].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[24].descriptorCount = 1;
    b[24].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_COMPUTE_BIT;

    b[25].binding = 27;
    b[25].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[25].descriptorCount = 1;
    b[25].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_COMPUTE_BIT;


    b[26].binding = 28;
    b[26].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[26].descriptorCount = 1;
    b[26].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR |
        VK_SHADER_STAGE_COMPUTE_BIT;


    b[27].binding = 29;
    b[27].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[27].descriptorCount = 1;
    b[27].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT |
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    b[28].binding = 30;
    b[28].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[28].descriptorCount = 1;
    b[28].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT |
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[29].binding = 31;
    b[29].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[29].descriptorCount = 1;
    b[29].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[30].binding = 32;
    b[30].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[30].descriptorCount = 1;
    b[30].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[31].binding = 33;
    b[31].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[31].descriptorCount = 1;
    b[31].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    b[32].binding = 34;
    b[32].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[32].descriptorCount = 1;
    b[32].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[33].binding = 35;
    b[33].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[33].descriptorCount = 1;
    b[33].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[34].binding = 36;
    b[34].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[34].descriptorCount = ENTITY_RT_TEXTURE_SLOT_COUNT;
    b[34].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                       VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[35].binding = 37;
    b[35].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[35].descriptorCount = PARTICLE_RT_TEXTURE_SLOT_COUNT;
    b[35].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                       VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[36].binding = 38;
    b[36].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[36].descriptorCount = 1;
    b[36].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_MISS_BIT_KHR;

    b[37].binding = 39;
    b[37].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[37].descriptorCount = 1;
    b[37].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_MISS_BIT_KHR;

    b[38].binding = 40;
    b[38].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[38].descriptorCount = 1;
    b[38].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                       VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[39].binding = 42;
    b[39].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[39].descriptorCount = 1;
    b[39].stageFlags = VK_SHADER_STAGE_MISS_BIT_KHR |
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[40].binding = 43;
    b[40].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[40].descriptorCount = 1;
    b[40].stageFlags = VK_SHADER_STAGE_MISS_BIT_KHR;

    b[41].binding = 44;
    b[41].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[41].descriptorCount = 1;
    b[41].stageFlags = VK_SHADER_STAGE_MISS_BIT_KHR;

    b[42].binding = 45;
    b[42].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[42].descriptorCount = 1;
    b[42].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    b[43].binding = 46;
    b[43].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[43].descriptorCount = 1;
    b[43].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[44].binding = 47;
    b[44].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[44].descriptorCount = 1;
    b[44].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[45].binding = 48;
    b[45].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[45].descriptorCount = 1;
    b[45].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[46].binding = 49;
    b[46].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[46].descriptorCount = 1;
    b[46].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
        VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[47].binding = 50;
    b[47].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[47].descriptorCount = 1;
    b[47].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_COMPUTE_BIT;

    b[48].binding = 51;
    b[48].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[48].descriptorCount = 1;
    b[48].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_COMPUTE_BIT;

    b[49].binding = 52;
    b[49].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[49].descriptorCount = 1;
    b[49].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_COMPUTE_BIT;

    b[50].binding = 53;
    b[50].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[50].descriptorCount = 1;
    b[50].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    b[51].binding = 54;
    b[51].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[51].descriptorCount = 1;
    b[51].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[52].binding = 55;
    b[52].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[52].descriptorCount = 1;
    b[52].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;

    b[53].binding = 56;
    b[53].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[53].descriptorCount = 1;
    b[53].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    b[54].binding = 57;
    b[54].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[54].descriptorCount = 1;
    b[54].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;

    b[55].binding = 41;
    b[55].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[55].descriptorCount = 1;
    b[55].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT |
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[56].binding = 19;
    b[56].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[56].descriptorCount = 1;
    b[56].stageFlags = VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                       VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[57].binding = 20;
    b[57].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[57].descriptorCount = 1;
    b[57].stageFlags = VK_SHADER_STAGE_ANY_HIT_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    b[58].binding = 58;
    b[58].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[58].descriptorCount = 1;
    b[58].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[59].binding = 59;
    b[59].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[59].descriptorCount = 1;
    b[59].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    b[60].binding = 60;
    b[60].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[60].descriptorCount = 1;
    b[60].stageFlags = VK_SHADER_STAGE_MISS_BIT_KHR;

    b[61].binding = 61;
    b[61].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[61].descriptorCount = 1;
    b[61].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

    b[62].binding = 62;
    b[62].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[62].descriptorCount = 1;
    b[62].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[63].binding = 63;
    b[63].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[63].descriptorCount = ENTITY_RT_TEXTURE_SLOT_COUNT;
    b[63].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                       VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[64].binding = 64;
    b[64].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[64].descriptorCount = ENTITY_RT_TEXTURE_SLOT_COUNT;
    b[64].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                       VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[65].binding = 65;
    b[65].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[65].descriptorCount = 1;
    b[65].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_COMPUTE_BIT;

    b[66].binding = 66;
    b[66].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[66].descriptorCount = 1;
    b[66].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_COMPUTE_BIT;

    b[67].binding = 67;
    b[67].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[67].descriptorCount = 1;
    b[67].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_COMPUTE_BIT;

    b[68].binding = 68;
    b[68].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[68].descriptorCount = 1;
    b[68].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                       VK_SHADER_STAGE_COMPUTE_BIT;

    b[69].binding = 69;
    b[69].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[69].descriptorCount = 1;
    b[69].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[70].binding = 70;
    b[70].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[70].descriptorCount = 1;
    b[70].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[71].binding = 71;
    b[71].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[71].descriptorCount = 1;
    b[71].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[72].binding = 72;
    b[72].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[72].descriptorCount = 1;
    b[72].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

#if 0
    b[73].binding = 73;
    b[73].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[73].descriptorCount = 1;
    b[73].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[74].binding = 74;
    b[74].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[74].descriptorCount = 1;
    b[74].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
#endif

    b[73].binding = 73;
    b[73].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[73].descriptorCount = 1;
    b[73].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[74].binding = 74;
    b[74].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[74].descriptorCount = 1;
    b[74].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[75].binding = 75;
    b[75].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[75].descriptorCount = 1;
    b[75].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;

    b[76].binding = 76;
    b[76].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[76].descriptorCount = 1;
    b[76].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[77].binding = 77;
    b[77].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[77].descriptorCount = 1;
    b[77].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    b[78].binding = 78;
    b[78].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[78].descriptorCount = 1;
    b[78].stageFlags =
        VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_COMPUTE_BIT;

    // 2026-11-01：近场太阳阴影图（1024²、160 格半径、8 Hz 重建）。
    // 低太阳角时 rgen 用它替代"每像素一条 160 格逐像素射线"。
    b[79].binding = 79;
    b[79].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[79].descriptorCount = 1;
    b[79].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;


    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    static_assert(
        sizeof(b) / sizeof(b[0]) == 80u,
        "RT descriptor set layout: update this assert together with b[]"
    );
    ci.bindingCount = static_cast<uint32_t>(sizeof(b) / sizeof(b[0]));
    ci.pBindings = b;

    VkDescriptorSetLayout l=VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(g_device,&ci,nullptr,&l),"Failed to create descriptor set layout");
    return l;
}
void destroyHistoryImage(
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view,
    VkImageLayout& layout)
{
    if(view)
    {
        vkDestroyImageView(
            g_device,
            view,
            nullptr
        );

        view =
            VK_NULL_HANDLE;
    }


    if(image)
    {
        vkDestroyImage(
            g_device,
            image,
            nullptr
        );

        image =
            VK_NULL_HANDLE;
    }


    if(memory)
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
void createRayTracingSBT(){
    constexpr uint32_t SBT_GROUP_RAYGEN = 0u;
    constexpr uint32_t SBT_GROUP_OPAQUE = 1u;
    constexpr uint32_t SBT_GROUP_MISS = 2u;
    constexpr uint32_t SBT_GROUP_ALPHA = 3u;
    constexpr uint32_t SBT_GROUP_WATER = 4u;
    constexpr uint32_t SBT_PIPELINE_GROUP_COUNT = 5u;
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rp{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR
    };

    VkPhysicalDeviceProperties2 props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2
    };
    props.pNext = &rp;

    auto fn =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2")
        );

    if(!fn)
        throw std::runtime_error("vkGetPhysicalDeviceProperties2 unavailable");

    fn(g_physicalDevice, &props);

    uint32_t hs = rp.shaderGroupHandleSize;
    uint32_t ha = rp.shaderGroupHandleAlignment;
    uint32_t ba = rp.shaderGroupBaseAlignment;

    g_sbtBaseAlignment = ba;

    auto alignUp = [](uint32_t v, uint32_t a){
        return (v + a - 1) & ~(a - 1);
    };

    g_handleSizeAligned = alignUp(hs, ha);

    g_raygenOffset = 0;

    g_missOffset =
        alignUp(
            g_raygenOffset + g_handleSizeAligned,
            ba
        );

    g_hitOffset =
        alignUp(
            g_missOffset + g_handleSizeAligned,
            ba
        );

    g_sbtSize =
        alignUp(
            g_hitOffset +
            SBT_HIT_RECORD_COUNT * g_handleSizeAligned,
            ba
        );

    uint32_t count = SBT_PIPELINE_GROUP_COUNT;
    std::vector<uint8_t> handles((size_t)count * hs);

    auto get =
        reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
            vkGetDeviceProcAddr(g_device, "vkGetRayTracingShaderGroupHandlesKHR")
        );

    if(!get)
        throw std::runtime_error("vkGetRayTracingShaderGroupHandlesKHR unavailable");

    check(
        get(
            g_device,
            g_rayTracingPipeline,
            0,
            count,
            handles.size(),
            handles.data()
        ),
        "Failed to get RT shader handles"
    );

    std::vector<uint8_t> data((size_t)g_sbtSize, 0);

    // group[0] = primary raygen
    std::memcpy(
        data.data() + g_raygenOffset,
        handles.data() + SBT_GROUP_RAYGEN * hs,
        hs
    );

    // group[2] = miss
    std::memcpy(
        data.data() + g_missOffset,
        handles.data() + SBT_GROUP_MISS * hs,
        hs
    );

    const uint32_t recordGroups[SBT_HIT_RECORD_COUNT] = {
        SBT_GROUP_OPAQUE,
        SBT_GROUP_OPAQUE,
        SBT_GROUP_ALPHA,
        SBT_GROUP_WATER,
        SBT_GROUP_OPAQUE
    };
    for(uint32_t record = 0u; record < SBT_HIT_RECORD_COUNT; ++record)
    {
        std::memcpy(
            data.data() + g_hitOffset + record * g_handleSizeAligned,
            handles.data() + recordGroups[record] * hs,
            hs
        );
    }

    createBuffer(
        g_device,
        g_physicalDevice,
        g_sbtSize,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        g_sbtBuffer,
        g_sbtMemory
    );

    void* mapped = nullptr;
    check(
        vkMapMemory(
            g_device,
            g_sbtMemory,
            0,
            g_sbtSize,
            0,
            &mapped
        ),
        "Failed to map SBT memory"
    );

    std::memcpy(mapped, data.data(), data.size());
    vkUnmapMemory(g_device, g_sbtMemory);

    g_sbtDeviceAddress =
        getBufferDeviceAddress(g_device, g_sbtBuffer);

    g_raygenRegion.deviceAddress =
        g_sbtDeviceAddress + g_raygenOffset;
    g_raygenRegion.stride =
        g_handleSizeAligned;
    g_raygenRegion.size =
        g_handleSizeAligned;

    g_missRegion.deviceAddress =
        g_sbtDeviceAddress + g_missOffset;
    g_missRegion.stride =
        g_handleSizeAligned;
    g_missRegion.size =
        g_handleSizeAligned;

    g_hitRegion.deviceAddress =
        g_sbtDeviceAddress + g_hitOffset;
    g_hitRegion.stride =
        g_handleSizeAligned;
    g_hitRegion.size =
        SBT_HIT_RECORD_COUNT * g_handleSizeAligned;

    g_callableRegion.deviceAddress = 0;
    g_callableRegion.stride = 0;
    g_callableRegion.size = 0;

    RTLog(
        "[RT TRANSLUCENT SBT] groups=6"
        " opaqueRecord=0->group1"
        " alphaRecord=2->group3"
        " waterRecord=3->group4"
        " hitRecords=4"
    );
}

// 2026-10-16 诊断闸门 ORION_RT_PIPE_NO_CACHE（默认关 = 现状）：
// 建 RT 管线时不传 pipeline cache（deferredOperation 仍为 null，其余逐位不变）。
// 依据：本文件 :833-849 自述——缓存 blob 曾让首次 vkCreateRayTracingPipelinesKHR
// 从 3-8 秒劣化到 130-243 秒，说明这条缓存路径在驱动侧代价极高；而实测"泄漏由
// 建 RT 管线这一步打开"（STOP=5 段 0.0MB/s、STOP=6 段 +35.0MB/s）。此闸门用来判
// 驱动侧的持续记账是否由缓存参数引起。
// 注意：vendored 参考树（cpp/deps/**/*.cpp）里没有任何 vkCreateRayTracingPipelinesKHR
// 调用（0 命中），即此处**无官方样本可照抄**；本闸门属自创诊断，唯一依据是
// cpp/deps/OMM/external/donut/nvrhi/thirdparty/Vulkan-Headers/registry/vk.xml:15213
// 对 deferredOperation/pipelineCache 的语义定义。
bool rtPipelineCacheDisabled()
{
    static const bool disabled = []() -> bool
    {
        const char* raw =
            std::getenv("ORION_RT_PIPE_NO_CACHE");

        return raw != nullptr
               && (std::strcmp(raw, "1") == 0 || std::strcmp(raw, "true") == 0);
    }();

    return disabled;
}

// 2026-10-16 诊断闸门 ORION_RT_PIPE_NO_OPT（默认关 = 现状）：
// 建 RT 管线时加 VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT（官方规范里的标准旗标，
// 见 cpp/deps/OMM/external/donut/nvrhi/thirdparty/Vulkan-Headers/include/vulkan/
// vulkan_core.h 的 VkPipelineCreateFlagBits 定义）。用途：判别驱动侧那 ~35MB/s 的
// 持续记账是否发生在"驱动对 RT 管线做优化/编译"这一段。
// 说明：vendored 树里没有 RT 管线创建的官方样本可照抄（cpp/deps/**/*.cpp 0 命中），
// 故此项属自创诊断，依据仅为规范旗标语义。
bool rtPipelineDisableOptimization()
{
    static const bool disabled = []() -> bool
    {
        const char* raw =
            std::getenv("ORION_RT_PIPE_NO_OPT");

        return raw != nullptr
               && (std::strcmp(raw, "1") == 0 || std::strcmp(raw, "true") == 0);
    }();

    return disabled;
}
bool deviceInitMilestone(const char* name)
{
    ++g_deviceInitStep;

    const uint32_t stopStep = deviceInitStopStep();

    const bool stopping =
        stopStep != 0u && g_deviceInitStep >= stopStep;

    fprintf(
        stderr,
        "[DEV STEP] %u %s%s\n",
        g_deviceInitStep,
        name,
        stopping ? "  <-- STOP" : ""
    );

    fflush(stderr);

    return stopping;
}
uint32_t rtPipeRecycleSeconds()
{
    static const uint32_t seconds = []() -> uint32_t
    {
        const char* raw =
            std::getenv("ORION_RT_PIPE_RECYCLE_SEC");

        return raw == nullptr
                   ? 0u
                   : static_cast<uint32_t>(std::strtoul(raw, nullptr, 10));
    }();

    return seconds;
}

// 按 initDeviceHandlesNative 里的主管线（:47460-47499 那一带）逐位重建一条同样的管线。
bool recreateMainRtPipeline()
{
    if(g_device == VK_NULL_HANDLE ||
       g_rayTracingPipelineLayout == VK_NULL_HANDLE)
    {
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[6]{};
    const VkShaderStageFlagBits kinds[6] = {
        VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
        VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
        VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, VK_SHADER_STAGE_ANY_HIT_BIT_KHR};
    const char* spvNames[6] = {
        "shaders/rt_test.rgen.spv", "shaders/rt_test.rchit.spv",
        "shaders/rt_test.rmiss.spv", "shaders/rt_test.rahit.spv",
        "shaders/rt_test_translucent.rchit.spv", "shaders/rt_test_translucent.rahit.spv"};

    VkShaderModule mods[6] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE,
                              VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    for(uint32_t i = 0u; i < 6u; ++i)
    {
        auto spv = loadSpv(spvNames[i]);
        mods[i] = createShaderModule(g_device, spv);
        stages[i] = VkPipelineShaderStageCreateInfo{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0u, kinds[i], mods[i], "main", nullptr};
    }

    VkRayTracingShaderGroupCreateInfoKHR groups[5]{};
    groups[0] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
                 VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 0u,
                 VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
    groups[1] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
                 VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
                 VK_SHADER_UNUSED_KHR, 1u, 3u, VK_SHADER_UNUSED_KHR};
    groups[2] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
                 VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR, 2u,
                 VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR, VK_SHADER_UNUSED_KHR};
    groups[3] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
                 VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
                 VK_SHADER_UNUSED_KHR, 4u, 5u, VK_SHADER_UNUSED_KHR};
    groups[4] = {VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR, nullptr,
                 VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
                 VK_SHADER_UNUSED_KHR, 1u, 3u, VK_SHADER_UNUSED_KHR};

    VkRayTracingPipelineCreateInfoKHR pci{
        VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
    pci.stageCount = 6u;
    pci.pStages = stages;
    pci.groupCount = 5u;
    pci.pGroups = groups;
    pci.maxPipelineRayRecursionDepth = 1u;
    pci.layout = g_rayTracingPipelineLayout;

    if(g_ommNativeEnabled)
    {
        pci.flags |=
            VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT;
    }

    auto createPipe =
        reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
            vkGetDeviceProcAddr(g_device, "vkCreateRayTracingPipelinesKHR"));

    VkPipeline fresh = VK_NULL_HANDLE;
    VkResult recreateResult = VK_ERROR_UNKNOWN;
    const auto recreateStart = std::chrono::steady_clock::now();

    if(createPipe != nullptr)
    {
        recreateResult = createPipe(
            g_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1u,
            &pci, nullptr, &fresh);
    }

    const double recreateMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - recreateStart
        ).count();

    for(VkShaderModule m : mods)
    {
        if(m != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(g_device, m, nullptr);
        }
    }

    if(recreateResult != VK_SUCCESS || fresh == VK_NULL_HANDLE)
    {
        fprintf(stderr,
                "[RT PIPE RECYCLE] recreate FAILED result=%d ms=%.1f\n",
                static_cast<int>(recreateResult), recreateMs);
        fflush(stderr);

        return false;
    }

    vkDeviceWaitIdle(g_device);

    const VkPipeline oldPipeline = g_rayTracingPipeline;
    const VkBuffer oldSbtBuffer = g_sbtBuffer;
    const VkDeviceMemory oldSbtMemory = g_sbtMemory;

    g_rayTracingPipeline = fresh;
    g_sbtBuffer = VK_NULL_HANDLE;
    g_sbtMemory = VK_NULL_HANDLE;

    bool sbtRebuilt = false;

    try
    {
        createRayTracingSBT();
        sbtRebuilt = g_sbtBuffer != VK_NULL_HANDLE;
    }
    catch(...)
    {
        sbtRebuilt = false;
    }

    if(!sbtRebuilt)
    {
        g_sbtBuffer = oldSbtBuffer;
        g_sbtMemory = oldSbtMemory;
        g_rayTracingPipeline = oldPipeline;
        vkDeviceWaitIdle(g_device);
        vkDestroyPipeline(g_device, fresh, nullptr);

        fprintf(stderr, "[RT PIPE RECYCLE] SBT rebuild FAILED -> rolled back\n");
        fflush(stderr);

        return false;
    }

    g_rtPipelinePendingDestroy = oldPipeline;
    g_rtSbtPendingDestroyBuffer = oldSbtBuffer;
    g_rtSbtPendingDestroyMemory = oldSbtMemory;

    fprintf(stderr, "[RT PIPE RECYCLE] swapped ms=%.1f (pipeline + SBT)\n", recreateMs);
    fflush(stderr);

    return true;
}
void recycleMainRtPipeline()
{
    const uint32_t intervalSec = rtPipeRecycleSeconds();

    if(intervalSec == 0u)
    {
        return;
    }

    if(g_device != VK_NULL_HANDLE)
    {
        if(g_rtPipelinePendingDestroy != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(g_device, g_rtPipelinePendingDestroy, nullptr);
            g_rtPipelinePendingDestroy = VK_NULL_HANDLE;
        }

        if(g_rtSbtPendingDestroyBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(g_device, g_rtSbtPendingDestroyBuffer, nullptr);
            g_rtSbtPendingDestroyBuffer = VK_NULL_HANDLE;
        }

        if(g_rtSbtPendingDestroyMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(g_device, g_rtSbtPendingDestroyMemory, nullptr);
            g_rtSbtPendingDestroyMemory = VK_NULL_HANDLE;
        }
    }

    if(g_rayTracingPipeline == VK_NULL_HANDLE)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();

    if(!g_rtPipeRecycleArmed)
    {
        g_rtPipeRecycleArmed = true;
        g_rtPipeLastRecycle = now;

        recreateMainRtPipeline();

        return;
    }

    if(std::chrono::duration<double>(now - g_rtPipeLastRecycle).count() <
       static_cast<double>(intervalSec))
    {
        return;
    }

    g_rtPipeLastRecycle = now;

    recreateMainRtPipeline();
}
