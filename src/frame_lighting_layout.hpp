#pragma once

#include <cstdint>
#include <limits>

namespace xrphoton
{
// Vulkan-free arithmetic authority for the dynamic-uniform-buffer layout. Keeping
// it in ordinary integer types lets the headless suite prove every conversion before
// GpuLighting narrows an offset for vkCmdBindDescriptorSets.
struct FrameLightingBufferLayout
{
    std::uint64_t stride = 0;
    std::uint64_t byteSize = 0;
    std::uint32_t slotCount = 0;
};

[[nodiscard]] constexpr bool makeFrameLightingBufferLayout(
    std::uint64_t recordSize,
    std::uint64_t minimumAlignment,
    std::uint32_t slotCount,
    FrameLightingBufferLayout* output)
{
    if (output == nullptr || recordSize == 0 || slotCount == 0) {
        return false;
    }

    const std::uint64_t alignment = minimumAlignment == 0 ? 1 : minimumAlignment;
    const std::uint64_t remainder = recordSize % alignment;
    const std::uint64_t padding = remainder == 0 ? 0 : alignment - remainder;
    if (recordSize > std::numeric_limits<std::uint64_t>::max() - padding) {
        return false;
    }
    const std::uint64_t stride = recordSize + padding;
    if (stride > std::numeric_limits<std::uint64_t>::max() / slotCount) {
        return false;
    }

    *output = {
        .stride = stride,
        .byteSize = stride * slotCount,
        .slotCount = slotCount,
    };
    return true;
}

[[nodiscard]] constexpr bool makeFrameLightingDynamicOffset(
    const FrameLightingBufferLayout& layout,
    std::uint32_t frameSlot,
    std::uint32_t* output)
{
    if (output == nullptr
        || frameSlot >= layout.slotCount) {
        return false;
    }
    if (frameSlot != 0
        && layout.stride > std::numeric_limits<std::uint64_t>::max() / frameSlot) {
        return false;
    }
    const std::uint64_t offset = layout.stride * frameSlot;
    if (offset > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *output = static_cast<std::uint32_t>(offset);
    return true;
}
}
