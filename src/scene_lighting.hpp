#pragma once

#include "estimator_mode.hpp"

#include <array>
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
    // direct-normal coefficient; an all-zero value disables the sun. A zero angular
    // radius preserves the original delta-light estimator exactly.
    glm::vec3 direction{};
    glm::vec3 irradiance{};
    float angularRadiusRadians = 0.0f;
};

struct AnalyticSky
{
    // The fallback sky interprets these as linear-RGB zenith/horizon radiance. The
    // Perez sky interprets zenithRadiance as (x, y, Y) and horizonRadiance as the
    // authored linear-RGB night horizon. This dual meaning preserves the pre-P4
    // gradient as an explicit fallback without adding a second shader entry point.
    glm::vec3 zenithRadiance{};
    glm::vec3 horizonRadiance{};
    std::array<glm::vec3, 5> perezCoefficients{};
    glm::vec3 nightZenithRadiance{};
    float daylightBlend = 0.0f;
    bool perezEnabled = false;
    // Acceptance environments can illuminate both world hemispheres. Production
    // skies leave this false so geometry, rather than fictitious ground radiance,
    // supplies light below the horizon.
    bool fullSphere = false;
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
    float emitterScale = 1.0f;
    std::uint32_t instanceCount = 0;
};

extern const SceneLighting DefaultSceneLighting;
extern const SceneLighting FurnaceSceneLighting;

inline constexpr float DefaultTimeOfDayHours = 12.0f;
inline constexpr float SunAngularRadiusRadians = 0.00471238898f; // 0.27 degrees

// Update the mutable analytic environment and its inverse daylight emitter scale. The
// fixed latitude/date/turbidity constants live in the implementation until level
// weather data has a real owner; emitter records and their immutable distributions
// are preserved byte-for-byte.
// Hours use [0, 24), making bad external/CLI state fail rather than wrap silently.
[[nodiscard]] bool updateSceneLightingTimeOfDay(
    float timeOfDayHours,
    SceneLighting* lighting);

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
constexpr std::uint32_t FrameLightingFullSphereSkyBit = 1u << 4u;
constexpr std::uint32_t FrameLightingKnownFlags =
    FrameLightingPerezSkyBit | FrameLightingGlassBit
    | FrameLightingEstimatorMask | FrameLightingFullSphereSkyBit;
// Estimator modes drive P2c's linear-HDR reference path. Interactive and ordinary
// capture retain MIS mode zero; only reference capture exposes the diagnostic modes.

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
    float emitterScale = 0.0f;
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
    && offsetof(FrameLighting, emitterScale) == 180
    && offsetof(FrameLighting, reserved1) == 184
    && offsetof(FrameLighting, reserved2) == 188);

[[nodiscard]] constexpr bool hasValidFrameLightingFlags(std::uint32_t flags)
{
    const std::uint32_t estimator =
        (flags & FrameLightingEstimatorMask) >> FrameLightingEstimatorShift;
    return (flags & ~FrameLightingKnownFlags) == 0 && estimator != 3u;
}

[[nodiscard]] constexpr std::uint32_t estimatorFlags(EstimatorMode estimator)
{
    return static_cast<std::uint32_t>(estimator) << FrameLightingEstimatorShift;
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

struct SunSample
{
    glm::vec3 direction{};
    glm::vec3 radiance{};
    float pdf = 0.0f;
    bool delta = false;
};

[[nodiscard]] glm::vec3 evaluateSkyRadiance(
    const FrameLighting& lighting,
    const glm::vec3& direction);
[[nodiscard]] float skyPdf(
    const glm::vec3& surfaceNormal,
    const glm::vec3& direction,
    bool twoSided = false);
[[nodiscard]] bool sampleSky(
    const FrameLighting& lighting,
    const glm::vec3& surfaceNormal,
    const glm::vec2& sample,
    SkySample* output,
    bool twoSided = false);
[[nodiscard]] glm::vec3 evaluateSunRadiance(
    const FrameLighting& lighting,
    const glm::vec3& direction);
[[nodiscard]] float sunPdf(
    const FrameLighting& lighting,
    const glm::vec3& direction);
[[nodiscard]] bool sampleSun(
    const FrameLighting& lighting,
    const glm::vec2& sample,
    SunSample* output);

inline constexpr float GlassIor = 1.5f;
[[nodiscard]] float dielectricFresnel(
    float cosineI,
    float etaI,
    float etaT);
// P3b's deliberately approximate NEE visibility rule: keep the straight shadow
// direction, tint transmitted energy, and account for interface Fresnel loss.
[[nodiscard]] glm::vec3 glassShadowAttenuation(
    const glm::vec3& tint,
    float cosineI,
    bool entering);

[[nodiscard]] constexpr float powerHeuristic(float firstPdf, float secondPdf)
{
    const float firstSquared = firstPdf * firstPdf;
    const float secondSquared = secondPdf * secondPdf;
    const float denominator = firstSquared + secondSquared;
    return firstSquared / (denominator > 1.0e-20f ? denominator : 1.0e-20f);
}

[[nodiscard]] float emitterSolidAnglePdf(
    float pEmitters,
    float pTriangle,
    float area,
    float distanceSquared,
    float lightCosine);
}
