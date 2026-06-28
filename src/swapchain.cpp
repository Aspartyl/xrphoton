#include "swapchain.hpp"

#include "vulkan_context.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace xrphoton
{
namespace
{
// Image usages the swapchain images must support: TRANSFER_DST for the clear/blit that
// produces each frame, and COLOR_ATTACHMENT to keep the door open for attachment-based
// rendering later. A surface lacking either is rejected.
constexpr VkImageUsageFlags RequiredSwapchainImageUsage =
    VK_IMAGE_USAGE_TRANSFER_DST_BIT
    | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

// Everything queried about a surface in one shot. `valid` is false if any of the
// underlying queries failed, so callers can treat a half-filled struct as "unsupported".
struct SwapchainSupportDetails
{
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
    bool valid = false;
};

// Gather surface capabilities, formats, and present modes. Returns with valid == false
// (the default) on the first failing query; valid is set only after all three succeed.
SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
    SwapchainSupportDetails support{};

    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        physicalDevice,
        surface,
        &support.capabilities);

    if (result != VK_SUCCESS) {
        return support;
    }

    uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        physicalDevice,
        surface,
        &formatCount,
        nullptr);

    if (result != VK_SUCCESS) {
        return support;
    }

    support.formats.resize(formatCount);

    if (formatCount > 0) {
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            physicalDevice,
            surface,
            &formatCount,
            support.formats.data());

        if (result != VK_SUCCESS) {
            return support;
        }
    }

    uint32_t presentModeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        physicalDevice,
        surface,
        &presentModeCount,
        nullptr);

    if (result != VK_SUCCESS) {
        return support;
    }

    support.presentModes.resize(presentModeCount);

    if (presentModeCount > 0) {
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            physicalDevice,
            surface,
            &presentModeCount,
            support.presentModes.data());

        if (result != VK_SUCCESS) {
            return support;
        }
    }

    support.valid = true;
    return support;
}

// Prefer 8-bit BGRA sRGB (the common, correctly gamma-managed choice); otherwise accept
// whatever the surface lists first. Callers guarantee `formats` is non-empty.
VkSurfaceFormatKHR chooseSwapchainSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
{
    for (const VkSurfaceFormatKHR& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB
            && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }

    return formats[0];
}

// Prefer mailbox (low-latency, no tearing) when available; fall back to FIFO, which the
// spec guarantees is always supported.
VkPresentModeKHR chooseSwapchainPresentMode(const std::vector<VkPresentModeKHR>& presentModes)
{
    for (VkPresentModeKHR presentMode : presentModes) {
        if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return presentMode;
        }
    }

    return VK_PRESENT_MODE_FIFO_KHR;
}

// Resolve the swapchain extent. When the surface dictates a fixed size, currentExtent is
// that size; the special value 0xFFFFFFFF means "pick your own", in which case we take
// the window's framebuffer size clamped to the surface's min/max. Returns false if the
// framebuffer has zero area (e.g. minimized), which the caller treats as "try later".
bool chooseSwapchainExtent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    GLFWwindow* window,
    VkExtent2D* extent)
{
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        *extent = capabilities.currentExtent;
        return true;
    }

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    if (framebufferWidth <= 0 || framebufferHeight <= 0) {
        return false;
    }

    extent->width = std::clamp(
        static_cast<uint32_t>(framebufferWidth),
        capabilities.minImageExtent.width,
        capabilities.maxImageExtent.width);
    extent->height = std::clamp(
        static_cast<uint32_t>(framebufferHeight),
        capabilities.minImageExtent.height,
        capabilities.maxImageExtent.height);

    return true;
}

// Pick a composite-alpha mode the surface supports, preferring OPAQUE (the window is
// not blended with whatever is behind it). The list is in priority order.
VkCompositeAlphaFlagBitsKHR chooseSwapchainCompositeAlpha(VkCompositeAlphaFlagsKHR supportedCompositeAlpha)
{
    constexpr VkCompositeAlphaFlagBitsKHR PreferredCompositeAlphaModes[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
    };

    for (VkCompositeAlphaFlagBitsKHR compositeAlpha : PreferredCompositeAlphaModes) {
        if ((supportedCompositeAlpha & compositeAlpha) != 0) {
            return compositeAlpha;
        }
    }

    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

// Create the VkSwapchainKHR itself and read back its images, recording the chosen
// format and extent into *swap. Image views and semaphores are added by the caller.
VkResult createSwapchain(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    const QueueFamilyIndices& queueFamilies,
    Swapchain* swap)
{
    const SwapchainSupportDetails support = querySwapchainSupport(physicalDevice, surface);

    if (!support.valid || support.formats.empty() || support.presentModes.empty()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    if ((support.capabilities.supportedUsageFlags & RequiredSwapchainImageUsage)
        != RequiredSwapchainImageUsage) {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    const VkSurfaceFormatKHR surfaceFormat = chooseSwapchainSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = chooseSwapchainPresentMode(support.presentModes);

    VkExtent2D extent{};
    if (!chooseSwapchainExtent(support.capabilities, window, &extent)) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Request one more than the minimum so the app isn't forced to wait on the driver
    // releasing an image. maxImageCount == 0 means "no upper bound"; otherwise clamp.
    uint32_t imageCount = support.capabilities.minImageCount + 1;

    if (support.capabilities.maxImageCount > 0
        && imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = RequiredSwapchainImageUsage;

    // If trace and present are different families, the images are written by one and
    // presented by the other, so they must be shared CONCURRENT across both. When the
    // families coincide, EXCLUSIVE is both valid and faster.
    const uint32_t queueFamilyIndices[] = {
        queueFamilies.traceFamily,
        queueFamilies.presentFamily,
    };

    if (queueFamilies.traceFamily != queueFamilies.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = static_cast<uint32_t>(std::size(queueFamilyIndices));
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = chooseSwapchainCompositeAlpha(support.capabilities.supportedCompositeAlpha);
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = VK_NULL_HANDLE;

    VkResult result = vkCreateSwapchainKHR(device, &createInfo, nullptr, &swap->swapchain);

    if (result != VK_SUCCESS) {
        return result;
    }

    result = vkGetSwapchainImagesKHR(device, swap->swapchain, &imageCount, nullptr);

    if (result != VK_SUCCESS) {
        return result;
    }

    swap->images.resize(imageCount);

    result = vkGetSwapchainImagesKHR(device, swap->swapchain, &imageCount, swap->images.data());

    if (result != VK_SUCCESS) {
        return result;
    }

    swap->imageFormat = surfaceFormat.format;
    swap->extent = extent;

    return VK_SUCCESS;
}

// Create one 2D color image view per swapchain image. On any failure every view created
// so far is destroyed and the output cleared, so the caller never sees a partial set.
VkResult createSwapchainImageViews(
    VkDevice device,
    const std::vector<VkImage>& swapchainImages,
    VkFormat swapchainImageFormat,
    std::vector<VkImageView>* swapchainImageViews)
{
    swapchainImageViews->clear();
    swapchainImageViews->resize(swapchainImages.size(), VK_NULL_HANDLE);

    for (size_t imageIndex = 0; imageIndex < swapchainImages.size(); ++imageIndex) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapchainImages[imageIndex];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapchainImageFormat;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        const VkResult result = vkCreateImageView(
            device,
            &createInfo,
            nullptr,
            &(*swapchainImageViews)[imageIndex]);

        if (result != VK_SUCCESS) {
            for (VkImageView imageView : *swapchainImageViews) {
                if (imageView != VK_NULL_HANDLE) {
                    vkDestroyImageView(device, imageView, nullptr);
                }
            }

            swapchainImageViews->clear();
            return result;
        }
    }

    return VK_SUCCESS;
}

// Destroy every render-finished semaphore and empty the vector. Null-guarded so it is
// safe to call on a partially constructed set during error cleanup.
void destroyRenderFinishedSemaphores(VkDevice device, std::vector<VkSemaphore>* semaphores)
{
    for (VkSemaphore semaphore : *semaphores) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
    }

    semaphores->clear();
}

// Create one render-finished semaphore per swapchain image. These are per-image (not
// per-frame-in-flight) because a present is signaled against the specific acquired
// image. Rolls back all semaphores on failure.
VkResult createRenderFinishedSemaphores(
    VkDevice device,
    size_t semaphoreCount,
    std::vector<VkSemaphore>* semaphores)
{
    VkSemaphoreCreateInfo semaphoreCreateInfo{};
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    semaphores->clear();
    semaphores->resize(semaphoreCount, VK_NULL_HANDLE);

    for (size_t semaphoreIndex = 0; semaphoreIndex < semaphoreCount; ++semaphoreIndex) {
        const VkResult result = vkCreateSemaphore(
            device,
            &semaphoreCreateInfo,
            nullptr,
            &(*semaphores)[semaphoreIndex]);

        if (result != VK_SUCCESS) {
            destroyRenderFinishedSemaphores(device, semaphores);
            return result;
        }
    }

    return VK_SUCCESS;
}

// Single teardown path shared by recreateSwapchain and ~Swapchain: destroy children in
// reverse creation order and reset *swap to its empty state. Does not touch the
// borrowed (non-owning) device handle. Callers are responsible for device idle first.
void destroySwapchainResources(Swapchain* swap)
{
    VkDevice device = swap->device;

    destroyRenderFinishedSemaphores(device, &swap->renderFinishedSemaphores);

    for (VkImageView imageView : swap->imageViews) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, imageView, nullptr);
        }
    }

    swap->imageViews.clear();

    if (swap->swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swap->swapchain, nullptr);
        swap->swapchain = VK_NULL_HANDLE;
    }

    swap->images.clear();
    swap->imageFormat = VK_FORMAT_UNDEFINED;
    swap->extent = {};
}

// Block (pumping events) until the window has a non-zero framebuffer, e.g. while it is
// minimized, since a zero-area swapchain cannot be created. Returns false if the window
// is asked to close while waiting, so the caller can bail out of recreation.
bool waitForDrawableFramebuffer(GLFWwindow* window)
{
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    while (!glfwWindowShouldClose(window)
        && (framebufferWidth <= 0 || framebufferHeight <= 0)) {
        glfwWaitEvents();
        glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
    }

    return !glfwWindowShouldClose(window);
}

} // namespace

bool hasRequiredSwapchainSupport(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
{
    const SwapchainSupportDetails support = querySwapchainSupport(physicalDevice, surface);

    return support.valid
        && !support.formats.empty()
        && !support.presentModes.empty()
        && (support.capabilities.supportedUsageFlags & RequiredSwapchainImageUsage)
            == RequiredSwapchainImageUsage;
}

VkResult createSwapchainResources(
    Swapchain* swap,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    const QueueFamilyIndices& queueFamilies)
{
    // Set the (non-owning) device first, before any child object is created, so a
    // partial failure below still cleans up via destroySwapchainResources / ~Swapchain.
    swap->device = device;

    VkResult result = createSwapchain(
        physicalDevice,
        device,
        surface,
        window,
        queueFamilies,
        swap);

    if (result != VK_SUCCESS) {
        return result;
    }

    result = createSwapchainImageViews(
        device,
        swap->images,
        swap->imageFormat,
        &swap->imageViews);

    if (result != VK_SUCCESS) {
        return result;
    }

    return createRenderFinishedSemaphores(
        device,
        swap->images.size(),
        &swap->renderFinishedSemaphores);
}

VkResult recreateSwapchain(
    Swapchain* swap,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkSurfaceKHR surface,
    GLFWwindow* window,
    const QueueFamilyIndices& queueFamilies)
{
    if (!waitForDrawableFramebuffer(window)) {
        return VK_SUCCESS;
    }

    VkResult result = vkDeviceWaitIdle(device);

    if (result != VK_SUCCESS) {
        return result;
    }

    destroySwapchainResources(swap);

    result = createSwapchainResources(
        swap,
        physicalDevice,
        device,
        surface,
        window,
        queueFamilies);

    if (result != VK_SUCCESS) {
        destroySwapchainResources(swap);
        return result;
    }

    std::cout << "Recreated Vulkan swapchain with "
              << swap->images.size() << " images ("
              << swap->extent.width << 'x'
              << swap->extent.height << ").\n";

    return VK_SUCCESS;
}

Swapchain::~Swapchain()
{
    // A default-constructed / already-moved-from Swapchain never received a device and
    // owns nothing, so there is nothing to wait on or destroy.
    if (device == VK_NULL_HANDLE) {
        return;
    }

    // Ensure no in-flight frame still references these resources before destroying them.
    (void)vkDeviceWaitIdle(device);

    // Capture what existed up front so the post-teardown log lines reflect the resources
    // that were actually present (destroySwapchainResources clears the containers).

    const bool hadRenderFinishedSemaphores = !renderFinishedSemaphores.empty();
    const bool hadImageViews = !imageViews.empty();
    const bool hadSwapchain = swapchain != VK_NULL_HANDLE;

    destroySwapchainResources(this);

    if (hadRenderFinishedSemaphores) {
        std::cout << "Destroyed Vulkan render-finished semaphores.\n";
    }
    if (hadImageViews) {
        std::cout << "Destroyed Vulkan swapchain image views.\n";
    }
    if (hadSwapchain) {
        std::cout << "Destroyed Vulkan swapchain.\n";
    }
}
}
