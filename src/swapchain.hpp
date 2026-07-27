#pragma once

#include <vector>

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include "vma_fwd.hpp"

namespace xrphoton
{
struct QueueFamilyIndices;

// Owns the resources recreated on resize. Its VkDevice is non-owning (borrowed from
// VulkanContext) and is used only to destroy the children below. Its destructor waits
// for the device to go idle before tearing down, so it is safe to declare a Swapchain
// after the VulkanContext it borrows from (it destructs first, before the device).
struct Swapchain
{
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;
    VkFormat imageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkSemaphore> renderFinishedSemaphores;
    // Resize-bound render targets. Ray tracing preserves scene radiance in the float
    // image; compute tonemapping writes the 8-bit linear image that presentation and
    // deterministic capture consume.
    VkImage hdrRadianceImage = VK_NULL_HANDLE;
    VmaAllocation hdrRadianceImageAllocation = nullptr;
    VkImageView hdrRadianceImageView = VK_NULL_HANDLE;
    VkImage ldrOutputImage = VK_NULL_HANDLE;
    VmaAllocation ldrOutputImageAllocation = nullptr;
    VkImageView ldrOutputImageView = VK_NULL_HANDLE;

    Swapchain() = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    ~Swapchain();
};

// Part of device suitability: true if the surface exposes at least one supported 8-bit
// sRGB + SRGB_NONLINEAR format and a present mode, supports the image usages the render
// path needs, and can blit from the storage output into that surface format. Kept here
// (rather than in vulkan_context) so physical-device selection and swapchain creation
// share one definition of "adequate swapchain support".
bool hasRequiredSwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface);

// Populate *swap with a fresh swapchain, its image views, one render-finished semaphore
// per image, and the HDR/LDR render targets. On failure the partially built resources
// are owned by *swap and torn down by ~Swapchain (or the next recreateSwapchain).
VkResult createSwapchainResources(
    Swapchain* swap,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    const QueueFamilyIndices& queueFamilies);

// Rebuild the swapchain after a resize / out-of-date surface: wait until the window has
// a drawable (non-zero) framebuffer, idle the device, destroy the old resources, then
// create new ones. Blocking on a minimized window is expected. If the window is asked to
// close while waiting, returns VK_SUCCESS with *swap left untouched (the old, possibly
// out-of-date resources remain valid); the caller is expected to exit its render loop
// rather than draw with them.
VkResult recreateSwapchain(
    Swapchain* swap,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VmaAllocator allocator,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    const QueueFamilyIndices& queueFamilies);
}
