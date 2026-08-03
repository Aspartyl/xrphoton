#include "denoise_pipeline.hpp"

#include "denoise.hpp"
#include "denoise_spv.h"

#include <cstdint>
#include <iostream>

namespace xrphoton
{
DenoisePipeline::~DenoisePipeline()
{
    if (device == VK_NULL_HANDLE) {
        return;
    }

    (void)vkDeviceWaitIdle(device);

    if (clampPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, clampPipeline, nullptr);
    }
    if (atrousPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, atrousPipeline, nullptr);
    }
    if (variancePipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, variancePipeline, nullptr);
        std::cout << "Destroyed Vulkan denoise pipelines.\n";
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

VkResult createDenoisePipeline(DenoisePipeline* denoise, VkDevice device)
{
    denoise->device = device;

    // Binding order matches denoise.slang: HDR radiance, normal + depth, albedo,
    // working input, working output.
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (std::uint32_t index = 0; index < 5; ++index) {
        bindings[index].binding = index;
        bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[index].descriptorCount = 1;
        bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{};
    descriptorLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorLayoutInfo.bindingCount = 5;
    descriptorLayoutInfo.pBindings = bindings;

    VkResult result = vkCreateDescriptorSetLayout(
        device,
        &descriptorLayoutInfo,
        nullptr,
        &denoise->descriptorSetLayout);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = 10;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    result = vkCreateDescriptorPool(
        device,
        &poolInfo,
        nullptr,
        &denoise->descriptorPool);
    if (result != VK_SUCCESS) {
        return result;
    }

    const VkDescriptorSetLayout setLayouts[2] = {
        denoise->descriptorSetLayout,
        denoise->descriptorSetLayout,
    };
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = denoise->descriptorPool;
    allocateInfo.descriptorSetCount = 2;
    allocateInfo.pSetLayouts = setLayouts;
    result = vkAllocateDescriptorSets(
        device,
        &allocateInfo,
        denoise->descriptorSets);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = sizeof(DenoisePushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &denoise->descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    result = vkCreatePipelineLayout(
        device,
        &pipelineLayoutInfo,
        nullptr,
        &denoise->pipelineLayout);
    if (result != VK_SUCCESS) {
        return result;
    }

    VkShaderModuleCreateInfo moduleInfo{};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    moduleInfo.codeSize = denoise_spv_sizeInBytes;
    moduleInfo.pCode = denoise_spv;
    result = vkCreateShaderModule(
        device,
        &moduleInfo,
        nullptr,
        &denoise->shaderModule);
    if (result != VK_SUCCESS) {
        return result;
    }

    const char* entryPoints[3] = {"clampMain", "varianceMain", "atrousMain"};
    VkPipeline* pipelines[3] = {
        &denoise->clampPipeline,
        &denoise->variancePipeline,
        &denoise->atrousPipeline,
    };
    for (std::uint32_t index = 0; index < 3; ++index) {
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = denoise->shaderModule;
        stage.pName = entryPoints[index];

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage = stage;
        pipelineInfo.layout = denoise->pipelineLayout;
        result = vkCreateComputePipelines(
            device,
            VK_NULL_HANDLE,
            1,
            &pipelineInfo,
            nullptr,
            pipelines[index]);
        if (result != VK_SUCCESS) {
            return result;
        }
    }

    vkDestroyShaderModule(device, denoise->shaderModule, nullptr);
    denoise->shaderModule = VK_NULL_HANDLE;
    return VK_SUCCESS;
}

void writeDenoiseDescriptorSets(
    VkDevice device,
    const VkDescriptorSet descriptorSets[2],
    VkImageView hdrRadianceImageView,
    VkImageView gbufferNormalDepthImageView,
    VkImageView gbufferAlbedoImageView,
    VkImageView workingImageViewA,
    VkImageView workingImageViewB)
{
    // Set 0 reads A and writes B; set 1 the reverse. The first three bindings are
    // shared. Every image is used in GENERAL: the frame path transitions them
    // there before any denoise dispatch.
    VkDescriptorImageInfo imageInfos[2][5]{};
    for (std::uint32_t setIndex = 0; setIndex < 2; ++setIndex) {
        imageInfos[setIndex][0].imageView = hdrRadianceImageView;
        imageInfos[setIndex][1].imageView = gbufferNormalDepthImageView;
        imageInfos[setIndex][2].imageView = gbufferAlbedoImageView;
        imageInfos[setIndex][3].imageView =
            setIndex == 0 ? workingImageViewA : workingImageViewB;
        imageInfos[setIndex][4].imageView =
            setIndex == 0 ? workingImageViewB : workingImageViewA;
        for (std::uint32_t binding = 0; binding < 5; ++binding) {
            imageInfos[setIndex][binding].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
    }

    VkWriteDescriptorSet writes[10]{};
    for (std::uint32_t setIndex = 0; setIndex < 2; ++setIndex) {
        for (std::uint32_t binding = 0; binding < 5; ++binding) {
            VkWriteDescriptorSet& write = writes[setIndex * 5 + binding];
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = descriptorSets[setIndex];
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.pImageInfo = &imageInfos[setIndex][binding];
        }
    }
    vkUpdateDescriptorSets(device, 10, writes, 0, nullptr);
}
}
