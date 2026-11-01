#pragma once

// ---------------------------------------------------------------------
// TrackingHooks.h —— Vulkan 入口记账劫持宏。
//
// 原 VulkanCore.cpp 用 #define vkAllocateMemory rtxTrackedAllocateMemory 等
// 宏把本文件内出现的 Vulkan 分配/对象创建调用整体改道到 rtxTracked* 包装，
// 形成显存与对象创建的总账。拆分后除 Tracking.cpp（记账实现本身，必须
// 调用真实入口）外，每个模块都包含本头文件，语义与拆分前一致。
// ---------------------------------------------------------------------


#define vkAllocateMemory rtxTrackedAllocateMemory
#define vkFreeMemory rtxTrackedFreeMemory

#define vkCreateDescriptorPool rtxTrackedCreateDescriptorPool
#define vkAllocateDescriptorSets rtxTrackedAllocateDescriptorSets
#define vkCreateCommandPool rtxTrackedCreateCommandPool
#define vkAllocateCommandBuffers rtxTrackedAllocateCommandBuffers
#define vkCreatePipelineLayout rtxTrackedCreatePipelineLayout
#define vkCreateShaderModule rtxTrackedCreateShaderModule
#define vkCreateComputePipelines rtxTrackedCreateComputePipelines
#define vkCreateImageView rtxTrackedCreateImageView
#define vkCreateSampler rtxTrackedCreateSampler

#define vkFreeCommandBuffers rtxTrackedFreeCommandBuffers
#define vkCreateFence rtxTrackedCreateFence
#define vkCreateSemaphore rtxTrackedCreateSemaphore
#define vkCreateQueryPool rtxTrackedCreateQueryPool

#define vkAllocateMemory rtxTrackedAllocateMemory
#define vkFreeMemory rtxTrackedFreeMemory

#define vkCreateDescriptorPool rtxTrackedCreateDescriptorPool
#define vkAllocateDescriptorSets rtxTrackedAllocateDescriptorSets
#define vkCreateCommandPool rtxTrackedCreateCommandPool
#define vkAllocateCommandBuffers rtxTrackedAllocateCommandBuffers
#define vkCreatePipelineLayout rtxTrackedCreatePipelineLayout
#define vkCreateShaderModule rtxTrackedCreateShaderModule
#define vkCreateComputePipelines rtxTrackedCreateComputePipelines
#define vkCreateImageView rtxTrackedCreateImageView
#define vkCreateSampler rtxTrackedCreateSampler

#define vkFreeCommandBuffers rtxTrackedFreeCommandBuffers
#define vkCreateFence rtxTrackedCreateFence
#define vkCreateSemaphore rtxTrackedCreateSemaphore
#define vkCreateQueryPool rtxTrackedCreateQueryPool
