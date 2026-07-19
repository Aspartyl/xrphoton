#include "rt_pipeline.hpp"

#include "camera.hpp"
#include "gpu_scene.hpp"
#include "scene.hpp"
#include "vulkan_context.hpp"
#include "vk_mem_alloc.h"

#include <cstdint>
#include <cstring>
#include <array>
#include <iostream>
#include <vector>

#include <vulkan/vulkan.h>

// Build-generated: the embedded SPIR-V module holding all four ray tracing entry
// points (see the shader custom command in CMakeLists.txt).
#include "raytrace_spv.h"

namespace xrphoton
{
namespace
{
// Group order is a serialized-in-memory contract with the SBT builder below.
constexpr uint32_t RaygenGroup = 0;
constexpr uint32_t MissGroup = 1;
constexpr uint32_t OpaqueHitGroup = 2;
constexpr uint32_t AlphaTestedHitGroup = 3;
constexpr uint32_t GroupCount = 4;

// This pipeline currently supplies one miss shader and one radiance variant of each
// hit class. Raising the build-owned constant must add the shadow variants atomically.
static_assert(RayTypeCount == 1);

// Round a value up to the next multiple of alignment, valid for any alignment. The
// AS build's bit-mask alignUp is only correct for powers of two — spec-guaranteed for
// shaderGroupHandleAlignment but *not* for shaderGroupBaseAlignment, whose
// description says only "required alignment", so the SBT math uses this form
// throughout.
VkDeviceSize roundUpToMultiple(VkDeviceSize value, VkDeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

} // namespace

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

    if (sbtBuffer != VK_NULL_HANDLE || sbtBufferAllocation != nullptr) {
        vmaDestroyBuffer(allocator, sbtBuffer, sbtBufferAllocation);
        std::cout << "Destroyed Vulkan shader binding table buffer.\n";
    }
}

VkResult createRtDescriptorSet(RtPipeline* rt, VkDevice device)
{
    // Adopt the device first, so every resource created below is torn down by
    // ~RtPipeline even when a later step here fails and the caller bare-returns.
    rt->device = device;

    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    const VkShaderStageFlags hitStageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR
        | VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = hitStageFlags;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = hitStageFlags;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4].descriptorCount = MaxSceneTextures;
    bindings[4].stageFlags = hitStageFlags;

    VkDescriptorSetLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCreateInfo.bindingCount = 5;
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
    VkDescriptorPoolSize poolSizes[4]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[1].descriptorCount = 1;
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[2].descriptorCount = 2;
    poolSizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[3].descriptorCount = MaxSceneTextures;

    VkDescriptorPoolCreateInfo poolCreateInfo{};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCreateInfo.maxSets = 1;
    poolCreateInfo.poolSizeCount = 4;
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

void writeSceneDescriptorSet(
    VkDevice device,
    VkDescriptorSet descriptorSet,
    const GpuScene& gpuScene)
{
    VkDescriptorBufferInfo bufferInfos[2]{};
    bufferInfos[0].buffer = gpuScene.geometryRecordBuffer;
    bufferInfos[0].range = VK_WHOLE_SIZE;
    bufferInfos[1].buffer = gpuScene.materialBuffer;
    bufferInfos[1].range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[3]{};
    for (uint32_t index = 0; index < 2; ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = descriptorSet;
        writes[index].dstBinding = 2 + index;
        writes[index].descriptorCount = 1;
        writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[index].pBufferInfo = &bufferInfos[index];
    }

    std::array<VkDescriptorImageInfo, MaxSceneTextures> textureInfos{};
    const VkImageView fallbackView = gpuScene.textures[0].view;
    for (uint32_t index = 0; index < MaxSceneTextures; ++index) {
        textureInfos[index].sampler = gpuScene.textureSampler;
        textureInfos[index].imageView = index < gpuScene.textures.size()
            ? gpuScene.textures[index].view
            : fallbackView;
        textureInfos[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = descriptorSet;
    writes[2].dstBinding = 4;
    writes[2].descriptorCount = MaxSceneTextures;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = textureInfos.data();

    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
}

VkResult createRtPipeline(
    RtPipeline* rt,
    VkDevice device,
    const RayTracingFunctions& functions)
{
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    pushRange.offset = 0;
    pushRange.size = sizeof(CameraPushConstants);

    VkPipelineLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutCreateInfo.setLayoutCount = 1;
    layoutCreateInfo.pSetLayouts = &rt->descriptorSetLayout;
    layoutCreateInfo.pushConstantRangeCount = 1;
    layoutCreateInfo.pPushConstantRanges = &pushRange;

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
    moduleCreateInfo.codeSize = raytrace_spv_sizeInBytes;
    moduleCreateInfo.pCode = raytrace_spv;

    // Parked in the owner (not local RAII) so the failure paths below can bare-return.
    result = vkCreateShaderModule(device, &moduleCreateInfo, nullptr, &rt->shaderModule);

    if (result != VK_SUCCESS) {
        return result;
    }

    // All four stages reference the one module; the entry-point names are the ones
    // the shader compile preserved (-fvk-use-entrypoint-name).
    VkPipelineShaderStageCreateInfo stages[4]{};
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
    stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[3].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    stages[3].module = rt->shaderModule;
    stages[3].pName = "anyHitMain";

    // Group order is the SBT contract: 0 raygen, 1 miss, 2 opaque hit, 3 alpha-tested
    // hit. Every shader index a
    // group does not use must be VK_SHADER_UNUSED_KHR explicitly — zero-init would
    // leave 0, which is a valid stage index (the raygen stage).
    VkRayTracingShaderGroupCreateInfoKHR groups[GroupCount]{};
    for (VkRayTracingShaderGroupCreateInfoKHR& group : groups) {
        group.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    groups[RaygenGroup].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[RaygenGroup].generalShader = 0;
    groups[MissGroup].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[MissGroup].generalShader = 1;
    groups[OpaqueHitGroup].type =
        VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[OpaqueHitGroup].closestHitShader = 2;
    // Both classes share closest-hit shading. Only alpha-tested ranges pay for the
    // any-hit stage, selected through their per-geometry SBT records.
    groups[AlphaTestedHitGroup].type =
        VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
    groups[AlphaTestedHitGroup].closestHitShader = 2;
    groups[AlphaTestedHitGroup].anyHitShader = 3;

    VkRayTracingPipelineCreateInfoKHR pipelineCreateInfo{};
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    pipelineCreateInfo.stageCount = 4;
    pipelineCreateInfo.pStages = stages;
    pipelineCreateInfo.groupCount = GroupCount;
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

VkResult buildShaderBindingTable(
    RtPipeline* rt,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    const RayTracingFunctions& functions,
    const SceneData& scene)
{
    // Adopt before the first VMA allocation so a later failure can bare-return and
    // still release the SBT through ~RtPipeline.
    rt->allocator = allocator;

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties{};
    rtProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &rtProperties;

    vkGetPhysicalDeviceProperties2(physicalDevice, &properties);

    if (scene.geometries.empty()) {
        std::cerr << "Cannot build a shader binding table for an empty geometry set.\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    constexpr uint64_t MaximumGeometryCount =
        ((uint64_t{1} << 24) - 1) / RayTypeCount + 1;
    if (scene.geometries.size() > MaximumGeometryCount) {
        std::cerr << "Scene geometry count " << scene.geometries.size()
                  << " exceeds the RayTypeCount-scaled 24-bit SBT offset limit "
                  << MaximumGeometryCount << ".\n";
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Records contain handles only. Every region's device address must additionally
    // be baseAlignment-aligned, hence the rounded region offsets.
    const VkDeviceSize handleSize = rtProperties.shaderGroupHandleSize;
    const VkDeviceSize recordStride =
        roundUpToMultiple(handleSize, rtProperties.shaderGroupHandleAlignment);
    const VkDeviceSize baseAlignment = rtProperties.shaderGroupBaseAlignment;

    const VkDeviceSize raygenOffset = 0;
    const VkDeviceSize missOffset = roundUpToMultiple(recordStride, baseAlignment);
    const VkDeviceSize missSize = RayTypeCount * recordStride;
    const VkDeviceSize hitOffset =
        missOffset + roundUpToMultiple(missSize, baseAlignment);
    const VkDeviceSize hitRecordCount =
        static_cast<VkDeviceSize>(scene.geometries.size()) * RayTypeCount;
    const VkDeviceSize hitSize = hitRecordCount * recordStride;
    const VkDeviceSize tableSize = hitOffset + hitSize;

    std::vector<uint8_t> handles(GroupCount * handleSize);

    VkResult result = functions.getRayTracingShaderGroupHandles(
        device,
        rt->pipeline,
        0,
        GroupCount,
        handles.size(),
        handles.data());

    if (result != VK_SUCCESS) {
        return result;
    }

    // baseAlignment - 1 bytes of slack so the table can start at an aligned device
    // address inside the buffer — the VUIDs constrain the regions' device addresses,
    // not their buffer offsets (the same trick as the AS build scratch).
    result = createBuffer(
        allocator,
        tableSize + baseAlignment - 1,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR
            | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
            | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        &rt->sbtBuffer,
        &rt->sbtBufferAllocation);

    if (result != VK_SUCCESS) {
        return result;
    }

    VkBufferDeviceAddressInfo addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    addressInfo.buffer = rt->sbtBuffer;

    const VkDeviceAddress bufferAddress = functions.getBufferDeviceAddress(device, &addressInfo);
    const VkDeviceAddress tableAddress = roundUpToMultiple(bufferAddress, baseAlignment);
    // The CPU writes below must shift by the same delta the device address was rounded
    // by — otherwise the handles land at unaligned offsets while the regions point at
    // the aligned ones, and the GPU reads garbage records with no validation error.
    const VkDeviceSize alignmentDelta = tableAddress - bufferAddress;

    VmaAllocationInfo allocationInfo{};
    vmaGetAllocationInfo(allocator, rt->sbtBufferAllocation, &allocationInfo);
    if (allocationInfo.pMappedData == nullptr) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }

    uint8_t* table = static_cast<uint8_t*>(allocationInfo.pMappedData) + alignmentDelta;
    std::memcpy(
        table + raygenOffset,
        handles.data() + RaygenGroup * handleSize,
        handleSize);
    for (uint32_t rayType = 0; rayType < RayTypeCount; ++rayType) {
        std::memcpy(
            table + missOffset + static_cast<VkDeviceSize>(rayType) * recordStride,
            handles.data() + MissGroup * handleSize,
            handleSize);
    }
    for (VkDeviceSize recordIndex = 0; recordIndex < hitRecordCount; ++recordIndex) {
        const std::size_t geometryIndex =
            static_cast<std::size_t>(recordIndex / RayTypeCount);
        const uint32_t groupIndex = scene.geometries[geometryIndex].alphaTested
            ? AlphaTestedHitGroup
            : OpaqueHitGroup;
        std::memcpy(
            table + hitOffset + recordIndex * recordStride,
            handles.data() + static_cast<VkDeviceSize>(groupIndex) * handleSize,
            handleSize);
    }

    // Persistent coherent mapping: no explicit flush or unmap is needed.

    // The raygen region's size must equal its stride. Miss records are indexed by ray
    // type; hit records interleave ray types inside each flat geometry index.
    rt->raygenRegion.deviceAddress = tableAddress + raygenOffset;
    rt->raygenRegion.stride = recordStride;
    rt->raygenRegion.size = recordStride;
    rt->missRegion.deviceAddress = tableAddress + missOffset;
    rt->missRegion.stride = recordStride;
    rt->missRegion.size = missSize;
    rt->hitRegion.deviceAddress = tableAddress + hitOffset;
    rt->hitRegion.stride = recordStride;
    rt->hitRegion.size = hitSize;
    // Empty, but pointing at the table base: the current VUID (03692) unconditionally
    // requires a valid SBT-buffer address with no zero-region exception, and reusing
    // the existing buffer satisfies the strict reading for free (the common {0,0,0}
    // idiom relies on validation-layer leniency).
    rt->callableRegion.deviceAddress = tableAddress;
    rt->callableRegion.stride = 0;
    rt->callableRegion.size = 0;

    return VK_SUCCESS;
}
}
