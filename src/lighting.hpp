#pragma once

#include "camera.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <glm/vec3.hpp>

namespace xrphoton
{
// Per-dispatch state consumed by raygen. CameraPushConstants remains the stable
// 64-byte prefix; the explicit scalar after each vec3 pins the complete payload to
// the same 16-byte member boundaries used by the shader.
struct RaygenPushConstants
{
    CameraPushConstants camera;
    glm::vec3 sunDirection; float pad0 = 0.0f;
    glm::vec3 sunRadiance;  std::uint32_t frameIndex = 0;
};
static_assert(std::is_standard_layout_v<RaygenPushConstants>,
    "offsetof requires the CPU mirror to remain standard-layout");
static_assert(sizeof(RaygenPushConstants) == 96,
    "raygen push constants must stay within the 128-byte Vulkan minimum");
static_assert(offsetof(RaygenPushConstants, camera) == 0
    && offsetof(RaygenPushConstants, sunDirection) == 64
    && offsetof(RaygenPushConstants, sunRadiance) == 80
    && offsetof(RaygenPushConstants, frameIndex) == 92,
    "field offsets are the shader ABI, not just the total size");

// Engine-neutral directional-light input. Direction points from a surface toward
// the sun; makeRaygenPushConstants owns normalization at the GPU boundary.
struct DirectionalSun
{
    glm::vec3 direction{};
    glm::vec3 radiance{};
};

extern const DirectionalSun DefaultSun;

// Preserve the already-built camera payload byte-for-byte, normalize a finite
// nondegenerate sun direction, and attach the per-frame sampling index.
[[nodiscard]] RaygenPushConstants makeRaygenPushConstants(
    const CameraPushConstants& camera,
    const DirectionalSun& sun,
    std::uint32_t frameIndex);

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
