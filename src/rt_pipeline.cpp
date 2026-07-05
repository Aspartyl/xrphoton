#include "rt_pipeline.hpp"

#include <iostream>

#include <vulkan/vulkan.h>

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
}
