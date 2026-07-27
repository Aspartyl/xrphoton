#include "tonemap_pipeline.hpp"

#include "tonemap_spv.h"

#include <cstdint>
#include <iostream>

namespace xrphoton
{
TonemapPipeline::~TonemapPipeline()
{
    if (device == VK_NULL_HANDLE) {
        return;
    }

    (void)vkDeviceWaitIdle(device);

    if (pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline, nullptr);
        std::cout << "Destroyed Vulkan tonemap pipeline.\n";
    }
    if (shaderModule != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, shaderModule, nullptr);
    }
    if (pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    }
    if (descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    }
    if (descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
    }
}

VkResult createTonemapPipeline(TonemapPipeline* tonemap, VkDevice device)
{
    tonemap->device = device;

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (std::uint32_t index = 0; index < 2; ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{};
    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = 2;
    descriptorLayoutInfo.pBindings = bindings;

    VkResult result = vkCreateDescriptorSetLayout(
        device,
        &descriptorLayoutInfo,
        nullptr,
        &tonemap->descriptorSetLayout);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &tonemap->descriptorPool);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = tonemap->descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &tonemap->descriptorSetLayout;
    result = vkAllocateDescriptorSets(device, &allocateInfo, &tonemap->descriptorSet);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = sizeof(TonemapState);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &tonemap->descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    result = vkCreatePipelineLayout(
        device,
        &pipelineLayoutInfo,
        nullptr,
        &tonemap->pipelineLayout);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = tonemap_spv_sizeInBytes;
    moduleInfo.pCode = tonemap_spv;
    result = vkCreateShaderModule(device, &moduleInfo, nullptr, &tonemap->shaderModule);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = tonemap->shaderModule;
    stage.pName = "tonemapMain";

    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage = stage;
    pipelineInfo.layout = tonemap->pipelineLayout;
    result = vkCreateComputePipelines(
        device,
        VK_NULL_HANDLE,
        1,
        &pipelineInfo,
        nullptr,
        &tonemap->pipeline);
    if (result != VK_SUCCESS) {
        return result;
    }

    vkDestroyShaderModule(device, tonemap->shaderModule, nullptr);
    tonemap->shaderModule = VK_NULL_HANDLE;
    return VK_SUCCESS;
}

void writeTonemapDescriptorSet(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    VkImageView hdrRadianceImageView,
    VkImageView ldrOutputImageView)
{
    VkDescriptorImageInfo imageInfos[2]{};
    imageInfos[0].imageView = hdrRadianceImageView;
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageInfos[1].imageView = ldrOutputImageView;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t index = 0; index < 2; ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = descriptorSet;
        writes[index].dstBinding = index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[index].pImageInfo = &imageInfos[index];
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
}
}
