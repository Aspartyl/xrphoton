#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace xrphoton
{
struct DirectionalSun
{
    // Direction points from a surface toward the sun. Irradiance is the incident
    // delta-light coefficient; an all-zero value disables the sun.
    glm::vec3 direction{};
    glm::vec3 irradiance{};
};

struct AnalyticSky
{
    glm::vec3 zenithRadiance{};
    glm::vec3 horizonRadiance{};
    bool enabled = true;
};

// Vulkan-free scene light authority. Later phases extend this with static light
// declarations; GPU allocation and publication remain GpuLighting's responsibility.
struct SceneLighting
{
    DirectionalSun sun{};
    AnalyticSky sky{};
};

extern const SceneLighting DefaultSceneLighting;

constexpr std::uint32_t FrameLightingPerezSkyBit = 1u << 0u;
constexpr std::uint32_t FrameLightingGlassBit = 1u << 1u;
constexpr std::uint32_t FrameLightingEstimatorShift = 2u;
constexpr std::uint32_t FrameLightingEstimatorMask = 3u << FrameLightingEstimatorShift;
constexpr std::uint32_t FrameLightingKnownFlags =
    FrameLightingPerezSkyBit | FrameLightingGlassBit | FrameLightingEstimatorMask;
// Estimator modes are ABI reservations validated now and consumed by P2c's linear-HDR
// reference path; Phase 1 publishes MIS mode zero and does not expose a partial toggle.

// Exact std140 CPU mirror for binding 5. The scalar after each vec3 fills its
// 16-byte lane explicitly, so this is stable with ordinary (unaligned) GLM types.
struct alignas(16) FrameLighting
{
    glm::vec3 sunDirection{}; float sunCosineHalfAngle = 1.0f;
    glm::vec3 sunIrradiance{}; float sunSolidAngle = 0.0f;
    glm::vec3 skyZenith{}; float pSky = 0.0f;
    glm::vec3 skyHorizon{}; float pEmitters = 0.0f;
    std::uint32_t lightCount = 0;
    std::uint32_t instanceCount = 0;
    float totalLightPower = 0.0f;
    std::uint32_t flags = 0;
    glm::vec4 skyPerezA{};
    glm::vec4 skyPerezB{};
    glm::vec4 skyPerezC{};
    glm::vec4 skyPerezD{};
    glm::vec4 skyPerezE{};
    glm::vec4 nightZenith{};
    float daylightBlend = 0.0f;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t reserved2 = 0;
};
static_assert(std::is_standard_layout_v<FrameLighting>);
static_assert(alignof(FrameLighting) == 16);
static_assert(sizeof(FrameLighting) == 192);
static_assert(offsetof(FrameLighting, sunDirection) == 0
    && offsetof(FrameLighting, sunCosineHalfAngle) == 12
    && offsetof(FrameLighting, sunIrradiance) == 16
    && offsetof(FrameLighting, sunSolidAngle) == 28
    && offsetof(FrameLighting, skyZenith) == 32
    && offsetof(FrameLighting, pSky) == 44
    && offsetof(FrameLighting, skyHorizon) == 48
    && offsetof(FrameLighting, pEmitters) == 60
    && offsetof(FrameLighting, lightCount) == 64
    && offsetof(FrameLighting, instanceCount) == 68
    && offsetof(FrameLighting, totalLightPower) == 72
    && offsetof(FrameLighting, flags) == 76
    && offsetof(FrameLighting, skyPerezA) == 80
    && offsetof(FrameLighting, skyPerezB) == 96
    && offsetof(FrameLighting, skyPerezC) == 112
    && offsetof(FrameLighting, skyPerezD) == 128
    && offsetof(FrameLighting, skyPerezE) == 144
    && offsetof(FrameLighting, nightZenith) == 160
    && offsetof(FrameLighting, daylightBlend) == 176
    && offsetof(FrameLighting, reserved0) == 180
    && offsetof(FrameLighting, reserved1) == 184
    && offsetof(FrameLighting, reserved2) == 188);

[[nodiscard]] constexpr bool hasValidFrameLightingFlags(std::uint32_t flags)
{
    const std::uint32_t estimator =
        (flags & FrameLightingEstimatorMask) >> FrameLightingEstimatorShift;
    return (flags & ~FrameLightingKnownFlags) == 0 && estimator != 3u;
}

// Validate and pack one immutable GPU publication. On failure output is unchanged.
[[nodiscard]] bool makeFrameLighting(
    const SceneLighting& scene,
    std::uint32_t instanceCount,
    FrameLighting* output);

struct SkySample
{
    glm::vec3 direction{};
    glm::vec3 radiance{};
    float pdf = 0.0f;
};

[[nodiscard]] glm::vec3 evaluateSkyRadiance(
    const FrameLighting& lighting,
    const glm::vec3& direction);
[[nodiscard]] float skyPdf(
    const glm::vec3& surfaceNormal,
    const glm::vec3& direction);
[[nodiscard]] bool sampleSky(
    const FrameLighting& lighting,
    const glm::vec3& surfaceNormal,
    const glm::vec2& sample,
    SkySample* output);

[[nodiscard]] constexpr float powerHeuristic(float firstPdf, float secondPdf)
{
    const float firstSquared = firstPdf * firstPdf;
    const float secondSquared = secondPdf * secondPdf;
    const float denominator = firstSquared + secondSquared;
    return firstSquared / (denominator > 1.0e-20f ? denominator : 1.0e-20f);
}
}
