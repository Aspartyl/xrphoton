#pragma once

#include <vulkan/vulkan.h>

// Keep VMA's large single-header implementation out of project headers. These
// declarations exactly match vk_mem_alloc.h and are sufficient for borrowed
// allocator/allocation handles and the createBuffer interface.
typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;
typedef VkFlags VmaAllocationCreateFlags;
