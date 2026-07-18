#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "vma_fwd.hpp"

namespace xrphoton
{
struct SceneData;
struct RayTracingFunctions;

// Binding 4 in raytrace.slang has this fixed width. Keeping the CPU constant beside
// the owned textures makes descriptor-limit and scene-size checks share one value.
constexpr uint32_t MaxSceneTextures = 1024;

// Device-address records consumed by raytrace.slang. Explicit offsets make the
// CPU/shader ABI fail at compile time instead of becoming a silent GPU read bug.
struct GeometryRecord
{
    VkDeviceAddress indexAddress = 0;
    VkDeviceAddress positionAddress = 0;
    VkDeviceAddress attributeAddress = 0;
    uint32_t materialIndex = 0;
    uint32_t pad0 = 0;
};
static_assert(sizeof(GeometryRecord) == 32);
static_assert(offsetof(GeometryRecord, indexAddress) == 0
    && offsetof(GeometryRecord, positionAddress) == 8
    && offsetof(GeometryRecord, attributeAddress) == 16
    && offsetof(GeometryRecord, materialIndex) == 24);

struct MaterialRecord
{
    float baseColorFactor[4] = {};
    uint32_t baseColorTexture = 0;
    float alphaCutoff = 0.0f;
    uint32_t pad0 = 0;
    uint32_t pad1 = 0;
};
static_assert(sizeof(MaterialRecord) == 32);
static_assert(offsetof(MaterialRecord, baseColorFactor) == 0
    && offsetof(MaterialRecord, baseColorTexture) == 16
    && offsetof(MaterialRecord, alphaCutoff) == 20);

struct SceneTexture
{
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
};

struct GpuScene
{
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;

    VkBuffer positionBuffer = VK_NULL_HANDLE;
    VkBuffer attributeBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkBuffer geometryRecordBuffer = VK_NULL_HANDLE;
    VkBuffer materialBuffer = VK_NULL_HANDLE;
    VmaAllocation positionAllocation = nullptr;
    VmaAllocation attributeAllocation = nullptr;
    VmaAllocation indexAllocation = nullptr;
    VmaAllocation geometryRecordAllocation = nullptr;
    VmaAllocation materialAllocation = nullptr;

    VkDeviceAddress positionBufferAddress = 0;
    VkDeviceAddress attributeBufferAddress = 0;
    VkDeviceAddress indexBufferAddress = 0;

    std::vector<SceneTexture> textures;
    VkSampler textureSampler = VK_NULL_HANDLE;

    GpuScene() = default;
    GpuScene(const GpuScene&) = delete;
    GpuScene& operator=(const GpuScene&) = delete;
    ~GpuScene();
};

// Device-selection backstops for binding 4 and the exact image creation profile used
// by createGpuScene. Both are repeated at the resource boundary where scene-dependent
// values (image count and extents) become available.
bool hasRequiredSceneTextureDescriptorLimits(const VkPhysicalDeviceLimits& limits);
struct SceneTextureFormatSupport
{
    bool rgba8Srgb = false;
    bool bc1RgbaSrgb = false;
    bool bc3Srgb = false;

    [[nodiscard]] bool isComplete() const
    {
        return rgba8Srgb && bc1RgbaSrgb && bc3Srgb;
    }
};
SceneTextureFormatSupport queryRequiredSceneTextureFormatSupport(
    VkPhysicalDevice physicalDevice);

VkResult createGpuScene(
    GpuScene* gpu,
    const SceneData& scene,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    const RayTracingFunctions& functions,
    VkCommandBuffer commandBuffer,
    VkQueue traceQueue,
    VkFence fence);
}
