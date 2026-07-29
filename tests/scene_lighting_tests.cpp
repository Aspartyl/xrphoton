#include "frame_lighting_layout.hpp"
#include "scene_lighting.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

#include <glm/geometric.hpp>

namespace
{
int failureCount = 0;

void expect(bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failureCount;
    }
}

bool nearly(float left, float right, float tolerance = 1.0e-5f)
{
    return std::abs(left - right) <= tolerance;
}

bool nearly(const glm::vec3& left, const glm::vec3& right, float tolerance = 1.0e-5f)
{
    return nearly(left.x, right.x, tolerance)
        && nearly(left.y, right.y, tolerance)
        && nearly(left.z, right.z, tolerance);
}

void testFrameAbiAndFlags()
{
    expect(
        sizeof(xrphoton::FrameLighting) == 192
            && alignof(xrphoton::FrameLighting) == 16,
        "FrameLighting has the pinned 192-byte, 16-byte-aligned ABI");
    expect(
        offsetof(xrphoton::FrameLighting, sunDirection) == 0
            && offsetof(xrphoton::FrameLighting, sunIrradiance) == 16
            && offsetof(xrphoton::FrameLighting, skyZenith) == 32
            && offsetof(xrphoton::FrameLighting, skyHorizon) == 48
            && offsetof(xrphoton::FrameLighting, lightCount) == 64
            && offsetof(xrphoton::FrameLighting, skyPerezA) == 80
            && offsetof(xrphoton::FrameLighting, nightZenith) == 160
            && offsetof(xrphoton::FrameLighting, daylightBlend) == 176,
        "FrameLighting member groups have their exact std140 offsets");

    expect(xrphoton::hasValidFrameLightingFlags(0), "MIS flags are valid");
    expect(
        xrphoton::hasValidFrameLightingFlags(
            xrphoton::FrameLightingPerezSkyBit
            | xrphoton::FrameLightingGlassBit
            | (1u << xrphoton::FrameLightingEstimatorShift)),
        "known feature bits and NEE-only mode are valid");
    expect(
        xrphoton::hasValidFrameLightingFlags(
            2u << xrphoton::FrameLightingEstimatorShift),
        "BSDF-only mode is valid");
    expect(
        !xrphoton::hasValidFrameLightingFlags(
            3u << xrphoton::FrameLightingEstimatorShift),
        "reserved estimator mode is rejected");
    expect(
        !xrphoton::hasValidFrameLightingFlags(1u << 8u),
        "unknown upper flag bits are rejected");
}

void testSelectorPacking()
{
    xrphoton::FrameLighting packed;
    expect(
        xrphoton::makeFrameLighting(
            xrphoton::DefaultSceneLighting,
            37u,
            &packed),
        "default scene lighting packs successfully");
    expect(
        nearly(glm::length(packed.sunDirection), 1.0f)
            && packed.sunDirection.y > 0.0f
            && packed.sunIrradiance
                == xrphoton::DefaultSceneLighting.sun.irradiance,
        "default sun is normalized and published as irradiance");
    expect(
        packed.sunCosineHalfAngle == 1.0f
            && packed.sunSolidAngle == 0.0f,
        "Phase 1 publishes the sun as a delta light");
    expect(
        packed.pSky == 1.0f && packed.pEmitters == 0.0f
            && packed.lightCount == 0 && packed.instanceCount == 37u
            && packed.flags == 0,
        "the enabled sky is the sole non-delta selector branch");
    expect(
        packed.skyPerezA == glm::vec4{}
            && packed.skyPerezB == glm::vec4{}
            && packed.skyPerezC == glm::vec4{}
            && packed.skyPerezD == glm::vec4{}
            && packed.skyPerezE == glm::vec4{}
            && packed.nightZenith == glm::vec4{}
            && packed.daylightBlend == 0.0f
            && packed.reserved0 == 0
            && packed.reserved1 == 0
            && packed.reserved2 == 0,
        "all later-phase coefficients and reserved words are zero in Phase 1");

    xrphoton::SceneLighting brighterSun = xrphoton::DefaultSceneLighting;
    brighterSun.sun.irradiance *= 1000.0f;
    xrphoton::FrameLighting brighterPacked;
    expect(
        xrphoton::makeFrameLighting(brighterSun, 0u, &brighterPacked)
            && brighterPacked.pSky == packed.pSky
            && brighterPacked.pEmitters == packed.pEmitters,
        "delta-sun power cannot perturb non-delta selector probabilities");

    xrphoton::SceneLighting disabled = xrphoton::DefaultSceneLighting;
    disabled.sky.enabled = false;
    expect(
        xrphoton::makeFrameLighting(disabled, 0u, &packed)
            && packed.pSky == 0.0f && packed.pEmitters == 0.0f
            && packed.skyZenith == glm::vec3{}
            && packed.skyHorizon == glm::vec3{},
        "an explicitly disabled sky removes its branch and radiance");

    xrphoton::SceneLighting zeroPower = xrphoton::DefaultSceneLighting;
    zeroPower.sky.zenithRadiance = {};
    zeroPower.sky.horizonRadiance = {};
    expect(
        xrphoton::makeFrameLighting(zeroPower, 0u, &packed)
            && packed.pSky == 0.0f && packed.pEmitters == 0.0f,
        "a zero-power sky removes its selector branch");

    xrphoton::SceneLighting invalid = xrphoton::DefaultSceneLighting;
    invalid.sun.direction = {};
    packed.instanceCount = 91u;
    expect(
        !xrphoton::makeFrameLighting(invalid, 0u, &packed)
            && packed.instanceCount == 91u,
        "a powered sun with a degenerate direction is rejected transactionally");
    invalid = xrphoton::DefaultSceneLighting;
    invalid.sky.horizonRadiance.x = -1.0f;
    expect(
        !xrphoton::makeFrameLighting(invalid, 0u, &packed)
            && packed.instanceCount == 91u,
        "negative radiance is rejected without modifying output");
}

void testSkyEvaluationSamplingAndPdf()
{
    xrphoton::FrameLighting lighting;
    expect(
        xrphoton::makeFrameLighting(
            xrphoton::DefaultSceneLighting,
            0u,
            &lighting),
        "sky test lighting packs");
    expect(
        nearly(
            xrphoton::evaluateSkyRadiance(lighting, {0.0f, 1.0f, 0.0f}),
            lighting.skyZenith)
            && nearly(
                xrphoton::evaluateSkyRadiance(lighting, {1.0f, 0.0f, 0.0f}),
                lighting.skyHorizon)
            && xrphoton::evaluateSkyRadiance(lighting, {0.0f, -0.5f, 0.0f})
                == glm::vec3{}
            && nearly(
                xrphoton::evaluateSkyRadiance(lighting, {0.0f, 0.5f, 0.0f}),
                (lighting.skyZenith + lighting.skyHorizon) * 0.5f),
        "sky evaluation reproduces the black ground hemisphere and upper gradient");

    const glm::vec3 normal = glm::normalize(glm::vec3{0.3f, 0.9f, -0.2f});
    constexpr glm::vec2 Samples[] = {
        {0.0f, 0.0f},
        {0.125f, 0.75f},
        {0.5f, 0.25f},
        {0.999f, 0.999f},
    };
    for (const glm::vec2& randomSample : Samples) {
        xrphoton::SkySample sampled;
        expect(
            xrphoton::sampleSky(lighting, normal, randomSample, &sampled),
            "enabled sky returns a sample");
        expect(
            nearly(glm::length(sampled.direction), 1.0f)
                && glm::dot(normal, sampled.direction) > 0.0f,
            "sky samples lie on the normalized shading hemisphere");
        expect(
            nearly(sampled.pdf, xrphoton::skyPdf(normal, sampled.direction))
                && nearly(
                    sampled.radiance,
                    xrphoton::evaluateSkyRadiance(lighting, sampled.direction)),
            "sky sample evaluation and PDF round-trip");
    }
    xrphoton::SkySample rejectedSample;
    expect(
        !xrphoton::sampleSky(
            lighting,
            normal * 2.0f,
            {0.5f, 0.5f},
            &rejectedSample),
        "the CPU sky sampler enforces the shader's unit-normal contract");

    // Fixed midpoint quadrature over the sphere: integral(pdf dOmega) must be one.
    constexpr std::uint32_t ThetaSteps = 256;
    constexpr std::uint32_t PhiSteps = 256;
    constexpr double Pi = 3.14159265358979323846;
    const double dTheta = Pi / ThetaSteps;
    const double dPhi = 2.0 * Pi / PhiSteps;
    double integral = 0.0;
    for (std::uint32_t thetaIndex = 0; thetaIndex < ThetaSteps; ++thetaIndex) {
        const double theta = (thetaIndex + 0.5) * dTheta;
        const float sine = static_cast<float>(std::sin(theta));
        const float cosine = static_cast<float>(std::cos(theta));
        for (std::uint32_t phiIndex = 0; phiIndex < PhiSteps; ++phiIndex) {
            const double phi = (phiIndex + 0.5) * dPhi;
            const glm::vec3 direction{
                sine * static_cast<float>(std::cos(phi)),
                cosine,
                sine * static_cast<float>(std::sin(phi)),
            };
            integral += xrphoton::skyPdf({0.0f, 1.0f, 0.0f}, direction)
                * sine * dTheta * dPhi;
        }
    }
    expect(
        std::abs(integral - 1.0) < 5.0e-5,
        "fixed-seed sky-PDF quadrature integrates to one");
}

void testMisWeights()
{
    expect(
        nearly(xrphoton::powerHeuristic(1.0f, 1.0f), 0.5f),
        "equal PDFs receive symmetric half weights");
    expect(
        nearly(
            xrphoton::powerHeuristic(0.2f, 0.7f)
                + xrphoton::powerHeuristic(0.7f, 0.2f),
            1.0f),
        "swapping nonzero PDFs produces complementary weights");
    expect(
        xrphoton::powerHeuristic(2.0f, 0.0f) == 1.0f
            && xrphoton::powerHeuristic(0.0f, 2.0f) == 0.0f,
        "a zero competing density leaves the valid technique unweighted");
    expect(
        xrphoton::powerHeuristic(0.0f, 0.0f) == 0.0f,
        "two absent techniques produce zero weight");
}

void testDynamicBufferMath()
{
    xrphoton::FrameLightingBufferLayout layout;
    expect(
        xrphoton::makeFrameLightingBufferLayout(192, 1, 2, &layout)
            && layout.stride == 192 && layout.byteSize == 384,
        "one-byte alignment leaves FrameLighting tightly packed");
    expect(
        xrphoton::makeFrameLightingBufferLayout(192, 256, 2, &layout)
            && layout.stride == 256 && layout.byteSize == 512,
        "representative Vulkan alignment rounds the slot stride");
    std::uint32_t offset = 99;
    expect(
        xrphoton::makeFrameLightingDynamicOffset(layout, 0, &offset)
            && offset == 0
            && xrphoton::makeFrameLightingDynamicOffset(layout, 1, &offset)
            && offset == 256,
        "both in-flight slots produce exact dynamic offsets");
    expect(
        !xrphoton::makeFrameLightingDynamicOffset(layout, 2, &offset),
        "an out-of-range frame slot is rejected");

    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    expect(
        !xrphoton::makeFrameLightingBufferLayout(maximum, 2, 1, &layout),
        "alignment padding overflow is rejected");
    expect(
        !xrphoton::makeFrameLightingBufferLayout(maximum / 2 + 1, 1, 2, &layout),
        "total allocation multiplication overflow is rejected");
    layout = {
        .stride = static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max()) + 1,
        .byteSize = maximum,
        .slotCount = 2,
    };
    offset = 77;
    expect(
        !xrphoton::makeFrameLightingDynamicOffset(layout, 1, &offset)
            && offset == 77,
        "a dynamic offset wider than uint32 is rejected transactionally");
}
}

int main()
{
    testFrameAbiAndFlags();
    testSelectorPacking();
    testSkyEvaluationSamplingAndPdf();
    testMisWeights();
    testDynamicBufferMath();

    if (failureCount != 0) {
        std::cerr << failureCount << " scene-lighting assertion(s) failed.\n";
        return 1;
    }
    std::cout << "scene lighting tests passed\n";
    return 0;
}
