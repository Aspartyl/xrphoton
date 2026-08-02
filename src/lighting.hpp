#pragma once

#include "camera.hpp"

#include <array>
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
    std::uint32_t samplesPerPixel = 1;
};
static_assert(std::is_standard_layout_v<RaygenPushConstants>,
    "offsetof requires the CPU mirror to remain standard-layout");
static_assert(sizeof(RaygenPushConstants) == 80,
    "raygen push constants must stay within the 128-byte Vulkan minimum");
static_assert(offsetof(RaygenPushConstants, camera) == 0
    && offsetof(RaygenPushConstants, frameIndex) == 64
    && offsetof(RaygenPushConstants, cameraJitterX) == 68
    && offsetof(RaygenPushConstants, cameraJitterY) == 72
    && offsetof(RaygenPushConstants, samplesPerPixel) == 76,
    "field offsets are the shader ABI, not just the total size");

constexpr std::uint32_t DefaultSamplesPerPixel = 1;
constexpr std::uint32_t MaximumSamplesPerPixel = 16;

[[nodiscard]] constexpr bool isSupportedSamplesPerPixel(std::uint32_t value)
{
    return value == 1 || value == 2 || value == 4 || value == 8 || value == 16;
}

[[nodiscard]] constexpr std::uint32_t nextSamplesPerPixel(std::uint32_t value)
{
    switch (value) {
    case 1:
        return 2;
    case 2:
        return 4;
    case 4:
        return 8;
    case 8:
        return 16;
    default:
        return 1;
    }
}

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

struct CameraJitter
{
    float x = 0.0f;
    float y = 0.0f;
};

constexpr std::uint32_t CameraJitterPermutationSeed = 0x4a495454u;
constexpr std::size_t CameraJitterPeriod = 16;

[[nodiscard]] consteval std::array<std::uint8_t, CameraJitterPeriod>
makeCameraJitterCellPermutation()
{
    std::array<std::uint8_t, CameraJitterPeriod> cells{};
    for (std::size_t index = 0; index < cells.size(); ++index) {
        cells[index] = static_cast<std::uint8_t>(index);
    }

    std::uint32_t state = CameraJitterPermutationSeed;
    for (std::size_t remaining = cells.size(); remaining > 1; --remaining) {
        state = pcgHash(state);
        const std::size_t selected = state % remaining;
        const std::uint8_t temporary = cells[remaining - 1];
        cells[remaining - 1] = cells[selected];
        cells[selected] = temporary;
    }
    return cells;
}

// A pinned fixed-seed permutation visits every 4x4 subpixel-cell center exactly
// once before repeating. The offset is frame-global: it must not consume or alter
// the per-pixel PCG path stream.
inline constexpr auto CameraJitterCellPermutation =
    makeCameraJitterCellPermutation();

[[nodiscard]] constexpr CameraJitter cameraJitterForFrame(
    std::uint32_t frameIndex)
{
    const std::uint8_t cell =
        CameraJitterCellPermutation[frameIndex % CameraJitterPeriod];
    constexpr float CellWidth = 1.0f / 4.0f;
    return {
        (static_cast<float>(cell % 4u) + 0.5f) * CellWidth - 0.5f,
        (static_cast<float>(cell / 4u) + 0.5f) * CellWidth - 0.5f,
    };
}

// Select the first path's cell in the global jitter sequence. The shader assigns
// following paths to the subsequent distinct cells and deterministically scrambles
// their positions within those cells without adding per-sample push data.
[[nodiscard]] constexpr CameraJitter cameraJitterForSample(
    std::uint32_t frameIndex,
    std::uint32_t samplesPerPixel,
    std::uint32_t sampleIndex)
{
    return cameraJitterForFrame(frameIndex * samplesPerPixel + sampleIndex);
}

// Preserve the camera prefix byte-for-byte and attach the authoritative temporal
// view state selected from the frame index.
[[nodiscard]] RaygenPushConstants makeRaygenPushConstants(
    const CameraPushConstants& camera,
    std::uint32_t frameIndex,
    std::uint32_t samplesPerPixel = DefaultSamplesPerPixel);
}
