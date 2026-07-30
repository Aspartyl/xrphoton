#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace xrphoton
{
struct SceneData;

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

// Exact storage-buffer records mirrored by records.slang. LightRecord stores one
// immutable world-space emitting triangle. EmitterLookupRecord maps a TLAS instance
// and its geometry-local index back to the corresponding consecutive light records.
struct alignas(16) LightRecord
{
    glm::vec3 v0{}; float area = 0.0f;
    glm::vec3 edge1{}; float pTriangle = 0.0f;
    glm::vec3 edge2{}; std::uint32_t flags = 0;
    glm::vec3 emission{}; std::uint32_t materialIndex = 0;
};
static_assert(std::is_standard_layout_v<LightRecord>);
static_assert(alignof(LightRecord) == 16 && sizeof(LightRecord) == 64);
static_assert(offsetof(LightRecord, v0) == 0
    && offsetof(LightRecord, area) == 12
    && offsetof(LightRecord, edge1) == 16
    && offsetof(LightRecord, pTriangle) == 28
    && offsetof(LightRecord, edge2) == 32
    && offsetof(LightRecord, flags) == 44
    && offsetof(LightRecord, emission) == 48
    && offsetof(LightRecord, materialIndex) == 60);

constexpr std::uint32_t NoEmitterLight = UINT32_MAX;
struct EmitterLookupRecord
{
    std::uint32_t first = NoEmitterLight;
    std::uint32_t count = 0;
    std::uint32_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
};
static_assert(std::is_standard_layout_v<EmitterLookupRecord>);
static_assert(sizeof(EmitterLookupRecord) == 16);
static_assert(offsetof(EmitterLookupRecord, first) == 0
    && offsetof(EmitterLookupRecord, count) == 4
    && offsetof(EmitterLookupRecord, reserved0) == 8
    && offsetof(EmitterLookupRecord, reserved1) == 12);

// Vulkan-free scene light authority. The analytic lights remain mutable per-frame;
// emitting triangles and their lookup/distribution are rebuilt from immutable static
// placements and then uploaded once by GpuLighting.
struct SceneLighting
{
    DirectionalSun sun{};
    AnalyticSky sky{};
    std::vector<LightRecord> lights;
    std::vector<float> lightCdf;
    std::vector<EmitterLookupRecord> emitterLookup;
    float totalLightPower = 0.0f;
    std::uint32_t instanceCount = 0;
};

extern const SceneLighting DefaultSceneLighting;

// Preserve lighting's analytic configuration and transactionally rebuild all derived
// emitter state. Dynamic and alpha-tested emitters are deliberately rejected because
// P2's records are immutable and its triangle-area PDF assumes fully opaque geometry.
[[nodiscard]] bool buildSceneLighting(
    const SceneData& scene,
    std::span<const std::size_t> dynamicInstances,
    SceneLighting* lighting,
    std::string* error);

// Validate the exact two-level lookup and its referenced light ranges without Vulkan.
// This is repeated at the upload boundary so malformed caller-built tables never
// become shader-visible even though buildSceneLighting itself only emits valid data.
[[nodiscard]] bool validateEmitterLookup(
    std::span<const EmitterLookupRecord> lookup,
    std::uint32_t instanceCount,
    std::uint32_t lightCount);

constexpr std::uint32_t FrameLightingPerezSkyBit = 1u << 0u;
constexpr std::uint32_t FrameLightingGlassBit = 1u << 1u;
constexpr std::uint32_t FrameLightingEstimatorShift = 2u;
constexpr std::uint32_t FrameLightingEstimatorMask = 3u << FrameLightingEstimatorShift;
constexpr std::uint32_t FrameLightingKnownFlags =
    FrameLightingPerezSkyBit | FrameLightingGlassBit | FrameLightingEstimatorMask;
// Estimator modes are ABI reservations validated now and consumed by P2c's linear-HDR
// reference path; P2b still publishes MIS mode zero and exposes no partial toggle.

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
