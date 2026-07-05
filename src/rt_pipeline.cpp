#include "rt_pipeline.hpp"

#include "vulkan_context.hpp"

#include <iostream>

#include <vulkan/vulkan.h>

// Build-generated: the embedded SPIR-V module holding all three ray tracing entry
// points (see the shader custom command in CMakeLists.txt).
#include "triangle_spv.h"

namespace xrphoton
{
RtPipeline::~RtPipeline()
{
    // A default-constructed owner never received a device and owns nothing.
    if (device == VK_NULL_HANDLE) {
        return;
    }

    // Self-contained idle wait: nothing here may still be referenced by in-flight work,
    // and this owner must not depend on another owner's destructor having waited first.
    (void)vkDeviceWaitIdle(device);

    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
        std::cout << "Destroyed Vulkan ray tracing pipeline.\n";
    }

    // Non-null only when a failure path bare-returned between module and pipeline
    // creation; the success path destroys it as soon as the pipeline exists.
    if (shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, shaderModule, nullptr);
        std::cout << "Destroyed Vulkan ray tracing shader module.\n";
    }

    if (pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        std::cout << "Destroyed Vulkan ray tracing pipeline layout.\n";
    }

    // The descriptor set is freed implicitly with the pool, never individually.
    if (descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        std::cout << "Destroyed Vulkan ray tracing descriptor pool.\n";
    }

    if (descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        std::cout << "Destroyed Vulkan ray tracing descriptor set layout.\n";
    }

    if (sbtBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, sbtBuffer, nullptr);
    }

    if (sbtBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, sbtBufferMemory, nullptr);
    }

    if (sbtBuffer != VK_NULL_HANDLE || sbtBufferMemory != VK_NULL_HANDLE) {
        std::cout << "Destroyed Vulkan shader binding table buffer.\n";
    }
}

VkResult createRtDescriptorSet(RtPipeline* rt, VkDevice device)
{
    // Adopt the device first, so every resource created below is torn down by
    // ~RtPipeline even when a later step here fails and the caller bare-returns.
    rt->device = device;

    // Both bindings are raygen-only: the miss and closest-hit shaders only read and
    // write the ray payload, never the scene or the output image directly.
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

    VkDescriptorSetLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCreateInfo.bindingCount = 2;
    layoutCreateInfo.pBindings = bindings;

    VkResult result = vkCreateDescriptorSetLayout(
        device,
        &layoutCreateInfo,
        nullptr,
        &rt->descriptorSetLayout);

    if (result != VK_SUCCESS) {
        return result;
    }

    // Sized for exactly the one set. No FREE_DESCRIPTOR_SET_BIT: the set is only ever
    // released with the pool, and the resize-time rewrite goes through
    // vkUpdateDescriptorSets, which does not need it.
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolCreateInfo{};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCreateInfo.maxSets = 1;
    poolCreateInfo.poolSizeCount = 2;
    poolCreateInfo.pPoolSizes = poolSizes;

    result = vkCreateDescriptorPool(device, &poolCreateInfo, nullptr, &rt->descriptorPool);

    if (result != VK_SUCCESS) {
        return result;
    }

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = rt->descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &rt->descriptorSetLayout;

    return vkAllocateDescriptorSets(device, &allocateInfo, &rt->descriptorSet);
}

void writeRtDescriptorSet(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkAccelerationStructureKHR tlas,
    VkImageView storageImageView)
{
    // Acceleration structures have no VkDescriptorImageInfo/BufferInfo form; the
    // handle rides in an extension struct chained through pNext, and the write's
    // descriptorCount is validated against its accelerationStructureCount.
    VkWriteDescriptorSetAccelerationStructureKHR tlasWrite{};
    tlasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    tlasWrite.accelerationStructureCount = 1;
    tlasWrite.pAccelerationStructures = &tlas;

    // GENERAL is the layout the raygen shader's image write requires; the frame path
    // transitions the storage image there before tracing, so the descriptor's declared
    // layout and the image's actual layout agree at trace time.
    VkDescriptorImageInfo storageImageInfo{};
    storageImageInfo.imageView = storageImageView;
    storageImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &tlasWrite;
    writes[0].dstSet = descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &storageImageInfo;

    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}

VkResult createRtPipeline(
    RtPipeline* rt,
    VkDevice device,
    const RayTracingFunctions& functions)
{
    VkPipelineLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCreateInfo.setLayoutCount = 1;
    layoutCreateInfo.pSetLayouts = &rt->descriptorSetLayout;

    VkResult result = vkCreatePipelineLayout(
        device,
        &layoutCreateInfo,
        nullptr,
        &rt->pipelineLayout);

    if (result != VK_SUCCESS) {
        return result;
    }

    VkShaderModuleCreateInfo moduleCreateInfo{};
    moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleCreateInfo.codeSize = triangle_spv_sizeInBytes;
    moduleCreateInfo.pCode = triangle_spv;

    // Parked in the owner (not local RAII) so the failure paths below can bare-return.
    result = vkCreateShaderModule(device, &moduleCreateInfo, nullptr, &rt->shaderModule);

    if (result != VK_SUCCESS) {
        return result;
    }

    // All three stages reference the one module; the entry-point names are the ones
    // the shader compile preserved (-fvk-use-entrypoint-name).
    VkPipelineShaderStageCreateInfo stages[3]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stages[0].module = rt->shaderModule;
    stages[0].pName = "rayGenMain";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
    stages[1].module = rt->shaderModule;
    stages[1].pName = "missMain";
    stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[2].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    stages[2].module = rt->shaderModule;
    stages[2].pName = "closestHitMain";

    // Group order is the SBT contract: 0 raygen, 1 miss, 2 hit. Every shader index a
    // group does not use must be VK_SHADER_UNUSED_KHR explicitly — zero-init would
    // leave 0, which is a valid stage index (the raygen stage).
    VkRayTracingShaderGroupCreateInfoKHR groups[3]{};
    for (VkRayTracingShaderGroupCreateInfoKHR& group : groups) {
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[0].generalShader = 0;
    groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[1].generalShader = 1;
    // Triangles-hit group with closest hit only: the geometry is OPAQUE, so an any-hit
    // shader would never run, and triangle intersection is fixed-function.
    groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[2].closestHitShader = 2;

    VkRayTracingPipelineCreateInfoKHR pipelineCreateInfo{};
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineCreateInfo.stageCount = 3;
    pipelineCreateInfo.pStages = stages;
    pipelineCreateInfo.groupCount = 3;
    pipelineCreateInfo.pGroups = groups;
    // Primary rays only; 1 is the spec-guaranteed minimum for
    // maxRayRecursionDepth, so no limit query is needed.
    pipelineCreateInfo.maxPipelineRayRecursionDepth = 1;
    pipelineCreateInfo.layout = rt->pipelineLayout;

    // No deferred host operation, no pipeline cache: one small pipeline built once
    // at startup.
    result = functions.createRayTracingPipelines(
        device,
        VK_NULL_HANDLE,
        VK_NULL_HANDLE,
        1,
        &pipelineCreateInfo,
        nullptr,
        &rt->pipeline);

    if (result != VK_SUCCESS) {
        return result;
    }

    // Modules are only linkage inputs; once the pipeline exists the module is dead
    // weight, so release it now rather than at teardown.
    vkDestroyShaderModule(device, rt->shaderModule, nullptr);
    rt->shaderModule = VK_NULL_HANDLE;

    return VK_SUCCESS;
}
}
