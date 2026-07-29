#pragma once

#include "camera.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace xrphoton
{
// View-only per-dispatch state consumed by raygen. Lighting is published through
// FrameLighting, so neither source can silently diverge from the other.
struct RaygenPushConstants
{
    CameraPushConstants camera;
    std::uint32_t frameIndex = 0;
    float cameraJitterX = 0.0f;
    float cameraJitterY = 0.0f;
    std::uint32_t reserved0 = 0;
};
static_assert(std::is_standard_layout_v<RaygenPushConstants>,
    "offsetof requires the CPU mirror to remain standard-layout");
static_assert(sizeof(RaygenPushConstants) == 80,
    "raygen push constants must stay within the 128-byte Vulkan minimum");
static_assert(offsetof(RaygenPushConstants, camera) == 0
    && offsetof(RaygenPushConstants, frameIndex) == 64
    && offsetof(RaygenPushConstants, cameraJitterX) == 68
    && offsetof(RaygenPushConstants, cameraJitterY) == 72
    && offsetof(RaygenPushConstants, reserved0) == 76,
    "field offsets are the shader ABI, not just the total size");

// Preserve the camera prefix byte-for-byte and attach temporal view state. Phase 1
// passes zero jitter; the pinned fields reserve the later temporal-camera ABI.
[[nodiscard]] RaygenPushConstants makeRaygenPushConstants(
    const CameraPushConstants& camera,
    std::uint32_t frameIndex,
    float cameraJitterX = 0.0f,
    float cameraJitterY = 0.0f);

// Small stateless PCG permutation used as the CPU reference for the matching Slang
// RNG. Unsigned overflow is intentional and defined.
[[nodiscard]] constexpr std::uint32_t pcgHash(std::uint32_t value)
{
    const std::uint32_t state = value * 747796405u + 2891336453u;
    const std::uint32_t word =
        ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Advance the caller-owned state and map its high 24 bits exactly into [0, 1).
[[nodiscard]] constexpr float rngNextFloat(std::uint32_t& state)
{
    state = pcgHash(state);
    return static_cast<float>(state >> 8u) * (1.0f / 16777216.0f);
}
}
