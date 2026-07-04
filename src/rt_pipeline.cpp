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
}
