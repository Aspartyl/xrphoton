#include "frame_lighting_layout.hpp"
#include "scene.hpp"
#include "scene_lighting.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace
{
int failureCount = 0;
constexpr float TestPi = 3.14159265358979323846f;

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

xrphoton::SceneData triangleScene(bool emissive = true)
{
    xrphoton::SceneData scene;
    scene.positions = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
    };
    scene.indices = {0, 1, 2};
    scene.materials.resize(1);
    if (emissive) {
        scene.materials[0].emission[0] = 2.0f;
        scene.materials[0].emission[1] = 2.0f;
        scene.materials[0].emission[2] = 2.0f;
    }
    scene.geometries.push_back({
        .firstVertex = 0,
        .vertexCount = 3,
        .firstIndex = 0,
        .indexCount = 3,
        .materialIndex = 0,
    });
    scene.meshes.push_back({.firstGeometry = 0, .geometryCount = 1});
    scene.instances.push_back({.meshIndex = 0});
    return scene;
}

void testEmitterRecordAbi()
{
    expect(
        sizeof(xrphoton::LightRecord) == 64
            && alignof(xrphoton::LightRecord) == 16
            && offsetof(xrphoton::LightRecord, v0) == 0
            && offsetof(xrphoton::LightRecord, edge1) == 16
            && offsetof(xrphoton::LightRecord, edge2) == 32
            && offsetof(xrphoton::LightRecord, emission) == 48,
        "LightRecord has its exact four-lane storage-buffer ABI");
    expect(
        sizeof(xrphoton::EmitterLookupRecord) == 16
            && offsetof(xrphoton::EmitterLookupRecord, first) == 0
            && offsetof(xrphoton::EmitterLookupRecord, count) == 4,
        "EmitterLookupRecord has its exact four-uint ABI");
}

void testEmitterDistribution()
{
    xrphoton::SceneData equalScene = triangleScene();
    equalScene.positions.insert(equalScene.positions.end(), {
        2.0f, 0.0f, 0.0f,
        3.0f, 0.0f, 0.0f,
        2.0f, 1.0f, 0.0f,
    });
    equalScene.indices.insert(equalScene.indices.end(), {3, 4, 5});
    equalScene.geometries[0].vertexCount = 6;
    equalScene.geometries[0].indexCount = 6;

    xrphoton::SceneLighting lighting = xrphoton::DefaultSceneLighting;
    std::string error;
    expect(
        xrphoton::buildSceneLighting(equalScene, {}, &lighting, &error),
        "equal-power emitter scene builds");
    expect(
        error.empty() && lighting.lights.size() == 2
            && lighting.lightCdf.size() == 2
            && nearly(lighting.lights[0].area, 0.5f)
            && nearly(lighting.lights[0].pTriangle, 0.5f)
            && nearly(lighting.lights[1].pTriangle, 0.5f)
            && nearly(lighting.lightCdf[0], 0.5f)
            && lighting.lightCdf[1] == 1.0f
            && nearly(lighting.totalLightPower, 2.0f),
        "equal-power triangles produce a uniform monotonic CDF");
    expect(
        nearly(lighting.lights[0].v0, {0.0f, 0.0f, 0.0f})
            && nearly(lighting.lights[0].edge1, {1.0f, 0.0f, 0.0f})
            && nearly(lighting.lights[0].edge2, {0.0f, 1.0f, 0.0f})
            && lighting.lights[0].flags == 0
            && lighting.lights[0].materialIndex == 0,
        "light records preserve world triangle edges and reserved fields");

    equalScene.positions[12] = 4.0f;
    expect(
        xrphoton::buildSceneLighting(equalScene, {}, &lighting, &error)
            && nearly(lighting.lights[0].pTriangle, 1.0f / 3.0f)
            && nearly(lighting.lights[1].pTriangle, 2.0f / 3.0f)
            && nearly(lighting.lightCdf[0], 1.0f / 3.0f)
            && lighting.lightCdf[1] == 1.0f,
        "unequal triangle power produces proportional probabilities and CDF");
}

void testEmitterLookupAndSelectors()
{
    xrphoton::SceneData scene = triangleScene();
    scene.instances.push_back({
        .meshIndex = 0,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{5.0f, 0.0f, 0.0f}),
    });
    xrphoton::SceneLighting lighting = xrphoton::DefaultSceneLighting;
    std::string error;
    expect(
        xrphoton::buildSceneLighting(scene, {}, &lighting, &error),
        "repeated static placement emitter scene builds");
    expect(
        lighting.lights.size() == 2 && lighting.emitterLookup.size() == 4
            && lighting.emitterLookup[0].first == 2
            && lighting.emitterLookup[0].count == 1
            && lighting.emitterLookup[1].first == 3
            && lighting.emitterLookup[1].count == 1
            && lighting.emitterLookup[2].first == 0
            && lighting.emitterLookup[2].count == 1
            && lighting.emitterLookup[3].first == 1
            && lighting.emitterLookup[3].count == 1
            && nearly(lighting.lights[1].v0, {5.0f, 0.0f, 0.0f}),
        "repeated mesh placements receive distinct headers, ranges, and world lights");
    expect(
        lighting.instanceCount == 2
            && xrphoton::validateEmitterLookup(
                lighting.emitterLookup,
                lighting.instanceCount,
                static_cast<std::uint32_t>(lighting.lights.size())),
        "constructed repeated-placement lookup validates independently");

    std::vector<xrphoton::EmitterLookupRecord> malformed =
        lighting.emitterLookup;
    malformed[0].reserved0 = 1;
    expect(
        !xrphoton::validateEmitterLookup(malformed, 2, 2),
        "nonzero lookup header reservation is rejected");
    malformed = lighting.emitterLookup;
    malformed[0].first = 1;
    expect(
        !xrphoton::validateEmitterLookup(malformed, 2, 2),
        "lookup range pointing into the header region is rejected");
    malformed = lighting.emitterLookup;
    malformed[2].count = 2;
    expect(
        !xrphoton::validateEmitterLookup(malformed, 2, 2),
        "lookup light range exceeding the light table is rejected");
    malformed = lighting.emitterLookup;
    malformed[3].first = 0;
    expect(
        !xrphoton::validateEmitterLookup(malformed, 2, 2),
        "overlapping lookup light ranges are rejected");
    malformed = lighting.emitterLookup;
    malformed[2].first = xrphoton::NoEmitterLight;
    malformed[2].count = 1;
    expect(
        !xrphoton::validateEmitterLookup(malformed, 2, 2),
        "nonempty non-emitter sentinel is rejected");
    malformed = lighting.emitterLookup;
    malformed.pop_back();
    expect(
        !xrphoton::validateEmitterLookup(malformed, 2, 2),
        "truncated lookup range table is rejected");

    lighting.emitterScale = 1.0f;
    xrphoton::FrameLighting packed;
    expect(
        xrphoton::makeFrameLighting(lighting, 2, &packed)
            && packed.lightCount == 2 && packed.instanceCount == 2
            && nearly(packed.totalLightPower, 2.0f)
            && packed.pSky == 0.5f && packed.pEmitters == 0.5f,
        "mixed sky and emitters split one normalized selector");
    lighting.sky.enabled = false;
    expect(
        xrphoton::makeFrameLighting(lighting, 2, &packed)
            && packed.pSky == 0.0f && packed.pEmitters == 1.0f,
        "emitters become the sole selector branch when sky is disabled");

    xrphoton::SceneData darkScene = triangleScene(false);
    lighting = xrphoton::DefaultSceneLighting;
    expect(
        xrphoton::buildSceneLighting(darkScene, {}, &lighting, &error)
            && lighting.lights.empty() && lighting.lightCdf.empty()
            && lighting.totalLightPower == 0.0f
            && lighting.emitterLookup.size() == 2
            && lighting.emitterLookup[0].first == 1
            && lighting.emitterLookup[0].count == 1
            && lighting.emitterLookup[1].first == xrphoton::NoEmitterLight
            && lighting.emitterLookup[1].count == 0
            && xrphoton::validateEmitterLookup(
                lighting.emitterLookup,
                lighting.instanceCount,
                0)
            && xrphoton::makeFrameLighting(lighting, 1, &packed)
            && packed.pSky == 1.0f && packed.pEmitters == 0.0f,
        "zero-emitter scenes retain a valid lookup and sky-only selector");
}

void testEmitterRejections()
{
    std::string error;
    xrphoton::SceneLighting lighting = xrphoton::DefaultSceneLighting;
    const xrphoton::SceneData valid = triangleScene();
    expect(
        xrphoton::buildSceneLighting(valid, {}, &lighting, &error),
        "rejection fixture first builds successfully");
    const float preservedPower = lighting.totalLightPower;
    const std::size_t preservedLights = lighting.lights.size();
    const std::array<std::size_t, 1> dynamicInstance{0};
    const std::array<std::size_t, 1> invalidDynamicInstance{1};
    const std::array<std::size_t, 2> duplicateDynamicInstance{0, 0};

    expect(
        !xrphoton::buildSceneLighting(
            valid,
            dynamicInstance,
            &lighting,
            &error)
            && error.find("dynamic") != std::string::npos
            && lighting.totalLightPower == preservedPower
            && lighting.lights.size() == preservedLights,
        "dynamic emitters are loudly rejected transactionally");
    xrphoton::SceneData invalid = valid;
    invalid.geometries[0].alphaTested = true;
    expect(
        !xrphoton::buildSceneLighting(invalid, {}, &lighting, &error)
            && error.find("alpha-tested") != std::string::npos,
        "alpha-tested emitters are loudly rejected");
    expect(
        !xrphoton::buildSceneLighting(
            valid,
            invalidDynamicInstance,
            &lighting,
            &error),
        "out-of-range dynamic instance is rejected");
    expect(
        !xrphoton::buildSceneLighting(
            valid,
            duplicateDynamicInstance,
            &lighting,
            &error),
        "duplicate dynamic instance is rejected");

    invalid = valid;
    invalid.instances[0].meshIndex = 1;
    expect(
        !xrphoton::buildSceneLighting(invalid, {}, &lighting, &error),
        "out-of-range mesh reference is rejected");
    invalid = valid;
    invalid.meshes[0].firstGeometry = 1;
    expect(
        !xrphoton::buildSceneLighting(invalid, {}, &lighting, &error),
        "out-of-range geometry range is rejected");
    invalid = valid;
    invalid.geometries[0].materialIndex = 1;
    expect(
        !xrphoton::buildSceneLighting(invalid, {}, &lighting, &error),
        "out-of-range material reference is rejected");
    invalid = valid;
    invalid.geometries[0].firstIndex = 1;
    expect(
        !xrphoton::buildSceneLighting(invalid, {}, &lighting, &error),
        "out-of-range emitter index span is rejected");
    invalid = valid;
    invalid.indices[2] = 3;
    expect(
        !xrphoton::buildSceneLighting(invalid, {}, &lighting, &error),
        "out-of-range local emitter vertex is rejected");
    invalid = valid;
    invalid.positions.resize(8);
    expect(
        !xrphoton::buildSceneLighting(invalid, {}, &lighting, &error),
        "truncated emitter position span is rejected");
    invalid = valid;
    invalid.instances[0].transform[0][3] = 1.0f;
    expect(
        !xrphoton::buildSceneLighting(invalid, {}, &lighting, &error),
        "projective emitter transform is rejected");
    invalid = valid;
    invalid.instances[0].transform[0][0] = 0.0f;
    expect(
        !xrphoton::buildSceneLighting(invalid, {}, &lighting, &error),
        "singular emitter transform is rejected");
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
            && offsetof(xrphoton::FrameLighting, daylightBlend) == 176
            && offsetof(xrphoton::FrameLighting, emitterScale) == 180,
        "FrameLighting member groups have their exact std140 offsets");

    expect(xrphoton::hasValidFrameLightingFlags(0), "MIS flags are valid");
    expect(
        xrphoton::hasValidFrameLightingFlags(
            xrphoton::FrameLightingPerezSkyBit
            | xrphoton::FrameLightingGlassBit
            | xrphoton::FrameLightingFullSphereSkyBit
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
        nearly(
            packed.sunCosineHalfAngle,
            std::cos(xrphoton::SunAngularRadiusRadians))
            && nearly(
                packed.sunSolidAngle,
                2.0f * TestPi
                    * (1.0f - packed.sunCosineHalfAngle),
                1.0e-7f)
            && packed.sunSolidAngle > 0.0f,
        "P4 publishes the authored finite solar disc and exact solid angle");
    expect(
        packed.pSky == 1.0f && packed.pEmitters == 0.0f
            && packed.lightCount == 0 && packed.instanceCount == 37u
            && packed.flags == xrphoton::FrameLightingPerezSkyBit,
        "the enabled sky is the sole non-delta selector branch");
    expect(
        packed.skyPerezA != glm::vec4{}
            && packed.skyPerezB != glm::vec4{}
            && packed.skyPerezC != glm::vec4{}
            && packed.skyPerezD != glm::vec4{}
            && packed.skyPerezE != glm::vec4{}
            && packed.skyPerezA.w == 0.0f
            && packed.skyPerezB.w == 0.0f
            && packed.skyPerezC.w == 0.0f
            && packed.skyPerezD.w == 0.0f
            && packed.skyPerezE.w == 0.0f
            && packed.nightZenith != glm::vec4{}
            && packed.nightZenith.w == 0.0f
            && packed.daylightBlend == 1.0f
            && packed.emitterScale == 0.0f
            && packed.reserved1 == 0
            && packed.reserved2 == 0,
        "noon fills the Perez lanes, switches emitters off, and preserves zero words");

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
            && packed.skyHorizon == glm::vec3{}
            && (packed.flags & xrphoton::FrameLightingPerezSkyBit) == 0,
        "an explicitly disabled sky removes its branch and radiance");

    xrphoton::SceneLighting zeroPower = xrphoton::DefaultSceneLighting;
    zeroPower.sky.zenithRadiance.z = 0.0f;
    zeroPower.sky.horizonRadiance = {};
    zeroPower.sky.nightZenithRadiance = {};
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
    invalid = xrphoton::DefaultSceneLighting;
    invalid.sky.perezCoefficients[2].y =
        std::numeric_limits<float>::quiet_NaN();
    expect(
        !xrphoton::makeFrameLighting(invalid, 0u, &packed)
            && packed.instanceCount == 91u,
        "non-finite Perez coefficients are rejected transactionally");
    invalid = xrphoton::DefaultSceneLighting;
    invalid.sun.angularRadiusRadians = 1.6f;
    expect(
        !xrphoton::makeFrameLighting(invalid, 0u, &packed)
            && packed.instanceCount == 91u,
        "an invalid solar angular radius is rejected transactionally");
    invalid = xrphoton::DefaultSceneLighting;
    invalid.emitterScale = 1.01f;
    expect(
        !xrphoton::makeFrameLighting(invalid, 0u, &packed)
            && packed.instanceCount == 91u,
        "an invalid time-driven emitter scale is rejected transactionally");
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
    const glm::vec3 perezZenith = xrphoton::evaluateSkyRadiance(
        lighting,
        {0.0f, 1.0f, 0.0f});
    const glm::vec3 perezHorizon = xrphoton::evaluateSkyRadiance(
        lighting,
        {1.0f, 0.0f, 0.0f});
    expect(
        perezZenith.x >= 0.0f && perezZenith.y >= 0.0f
            && perezZenith.z >= 0.0f && perezHorizon.x >= 0.0f
            && perezHorizon.y >= 0.0f && perezHorizon.z >= 0.0f
            && perezZenith != perezHorizon
            && xrphoton::evaluateSkyRadiance(lighting, {0.0f, -0.5f, 0.0f})
                == glm::vec3{},
        "Perez evaluation is finite/nonnegative and retains the black ground hemisphere");

    xrphoton::SceneLighting fallback;
    fallback.sky = {
        .zenithRadiance = {0.055f, 0.12f, 0.28f},
        .horizonRadiance = {0.32f, 0.38f, 0.46f},
        .enabled = true,
    };
    xrphoton::FrameLighting fallbackFrame;
    expect(
        xrphoton::makeFrameLighting(fallback, 0u, &fallbackFrame)
            && fallbackFrame.flags == 0
            && nearly(
                xrphoton::evaluateSkyRadiance(
                    fallbackFrame,
                    {0.0f, 1.0f, 0.0f}),
                fallbackFrame.skyZenith)
            && nearly(
                xrphoton::evaluateSkyRadiance(
                    fallbackFrame,
                    {1.0f, 0.0f, 0.0f}),
                fallbackFrame.skyHorizon)
            && nearly(
                xrphoton::evaluateSkyRadiance(
                    fallbackFrame,
                    {0.0f, 0.5f, 0.0f}),
                (fallbackFrame.skyZenith + fallbackFrame.skyHorizon) * 0.5f),
        "the one sky entry point preserves the explicit pre-P4 gradient fallback");

    xrphoton::FrameLighting furnaceFrame;
    expect(
        xrphoton::makeFrameLighting(
            xrphoton::FurnaceSceneLighting,
            0u,
            &furnaceFrame)
            && furnaceFrame.sunIrradiance == glm::vec3{}
            && furnaceFrame.lightCount == 0
            && furnaceFrame.pSky == 1.0f
            && furnaceFrame.pEmitters == 0.0f
            && furnaceFrame.emitterScale == 0.0f
            && furnaceFrame.flags
                == xrphoton::FrameLightingFullSphereSkyBit
            && xrphoton::evaluateSkyRadiance(
                furnaceFrame, {0.0f, 1.0f, 0.0f}) == glm::vec3{1.0f}
            && xrphoton::evaluateSkyRadiance(
                furnaceFrame, {1.0f, 0.0f, 0.0f}) == glm::vec3{1.0f}
            && xrphoton::evaluateSkyRadiance(
                furnaceFrame, {0.0f, -1.0f, 0.0f}) == glm::vec3{1.0f},
        "furnace lighting is constant over the full sphere with no other source");

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

void testTimeOfDayAndPerezKnownAnswers()
{
    xrphoton::SceneLighting lighting = xrphoton::DefaultSceneLighting;
    expect(
        xrphoton::updateSceneLightingTimeOfDay(12.0f, &lighting),
        "solar noon configures the time-of-day authority");
    xrphoton::FrameLighting noon;
    expect(
        xrphoton::makeFrameLighting(lighting, 0u, &noon)
            && (noon.flags & xrphoton::FrameLightingPerezSkyBit) != 0
            && noon.daylightBlend == 1.0f
            && noon.sunDirection.y > 0.9f
            && noon.sunSolidAngle > 0.0f,
        "solar noon publishes a powered disc and full Perez daylight");
    expect(
        nearly(glm::vec3{noon.skyPerezA}, {-0.3171f, -0.3109f, -0.9269f})
            && nearly(glm::vec3{noon.skyPerezB}, {-0.1987f, -0.2758f, -0.6387f})
            && nearly(glm::vec3{noon.skyPerezC}, {0.2113f, 0.1865f, 5.2570f})
            && nearly(glm::vec3{noon.skyPerezD}, {-1.0912f, -1.7860f, -2.2153f})
            && nearly(glm::vec3{noon.skyPerezE}, {0.0353f, 0.0202f, 0.1693f}),
        "turbidity-three Perez coefficients match the published Preetham fits");
    expect(
        nearly(noon.sunDirection, {0.0f, 0.92009521f, -0.39169487f})
            && nearly(noon.skyZenith, {0.25801855f, 0.26523608f, 0.49829715f})
            && nearly(
                xrphoton::evaluateSkyRadiance(noon, {0.0f, 1.0f, 0.0f}),
                {0.35828340f, 0.50221324f, 0.87205797f})
            && nearly(
                xrphoton::evaluateSkyRadiance(noon, {1.0f, 0.0f, 0.0f}),
                {0.36114889f, 0.33534327f, 0.37611270f})
            && nearly(
                xrphoton::evaluateSkyRadiance(
                    noon,
                    {0.0f, 0.5f, 0.8660254f}),
                {0.15202090f, 0.27763247f, 0.49231640f}),
        "solar-noon xyY and linear-RGB evaluations match pinned known answers");

    xrphoton::FrameLighting midnight;
    expect(
        xrphoton::updateSceneLightingTimeOfDay(0.0f, &lighting)
            && xrphoton::makeFrameLighting(lighting, 0u, &midnight)
            && midnight.daylightBlend == 0.0f
            && midnight.emitterScale == 1.0f
            && midnight.sunIrradiance == glm::vec3{}
            && midnight.sunSolidAngle == 0.0f
            && nearly(
                xrphoton::evaluateSkyRadiance(
                    midnight,
                    {0.0f, 1.0f, 0.0f}),
                glm::vec3{midnight.nightZenith})
            && nearly(
                xrphoton::evaluateSkyRadiance(
                    midnight,
                    {1.0f, 0.0f, 0.0f}),
                midnight.skyHorizon),
        "midnight disables the sun and evaluates only the authored night gradient");

    xrphoton::FrameLighting twilight;
    expect(
        xrphoton::updateSceneLightingTimeOfDay(3.8f, &lighting)
            && xrphoton::makeFrameLighting(lighting, 0u, &twilight)
            && twilight.daylightBlend > 0.0f
            && twilight.daylightBlend < 1.0f
            && nearly(
                twilight.emitterScale,
                1.0f - twilight.daylightBlend)
            && twilight.sunIrradiance == glm::vec3{}
            && nearly(twilight.sunDirection.y, 0.0f, 1.0e-5f),
        "civil twilight crossfades the daylight sky and permanent yard emitters");

    constexpr float SweepHours[] = {
        0.0f, 3.4f, 3.8f, 4.2f, 6.0f, 12.0f,
        18.0f, 19.8f, 20.6f, 23.0f,
    };
    for (float hours : SweepHours) {
        xrphoton::FrameLighting frame;
        expect(
            xrphoton::updateSceneLightingTimeOfDay(hours, &lighting)
                && xrphoton::makeFrameLighting(lighting, 0u, &frame),
            "the dawn-to-night sweep packs every time sample");
        constexpr glm::vec3 Directions[] = {
            {0.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f},
            {0.0f, 0.5f, 0.8660254f},
        };
        for (const glm::vec3& direction : Directions) {
            const glm::vec3 radiance = xrphoton::evaluateSkyRadiance(
                frame,
                direction);
            expect(
                std::isfinite(radiance.x) && std::isfinite(radiance.y)
                    && std::isfinite(radiance.z) && radiance.x >= 0.0f
                    && radiance.y >= 0.0f && radiance.z >= 0.0f,
                "the time sweep keeps every sky known direction finite/nonnegative");
        }
    }

    xrphoton::SceneData scene = triangleScene();
    lighting = xrphoton::DefaultSceneLighting;
    std::string error;
    expect(
        xrphoton::buildSceneLighting(scene, {}, &lighting, &error),
        "time-of-day preservation fixture builds one emitter");
    const float lightArea = lighting.lights[0].area;
    const float finalCdf = lighting.lightCdf.back();
    const std::size_t lookupSize = lighting.emitterLookup.size();
    expect(
        xrphoton::updateSceneLightingTimeOfDay(0.0f, &lighting)
            && lighting.lights.size() == 1
            && lighting.lights[0].area == lightArea
            && lighting.lightCdf.back() == finalCdf
            && lighting.emitterLookup.size() == lookupSize
            && lighting.emitterScale == 1.0f,
        "per-frame time updates preserve tables while changing emitter intensity");

    const xrphoton::SceneLighting unchanged = lighting;
    expect(
        !xrphoton::updateSceneLightingTimeOfDay(-0.01f, &lighting)
            && !xrphoton::updateSceneLightingTimeOfDay(24.0f, &lighting)
            && !xrphoton::updateSceneLightingTimeOfDay(
                std::numeric_limits<float>::infinity(),
                &lighting)
            && !xrphoton::updateSceneLightingTimeOfDay(12.0f, nullptr)
            && lighting.sun.direction == unchanged.sun.direction
            && lighting.lights.size() == unchanged.lights.size(),
        "invalid time input is rejected without mutating the lighting authority");
}

void testFiniteSunSamplingAndEnergy()
{
    xrphoton::FrameLighting lighting;
    expect(
        xrphoton::makeFrameLighting(
            xrphoton::DefaultSceneLighting,
            0u,
            &lighting),
        "finite-sun test lighting packs");
    constexpr glm::vec2 Samples[] = {
        {0.0f, 0.0f},
        {0.25f, 0.125f},
        {0.5f, 0.75f},
        {0.999f, 0.999f},
    };
    for (const glm::vec2& randomSample : Samples) {
        xrphoton::SunSample sampled;
        expect(
            xrphoton::sampleSun(lighting, randomSample, &sampled)
                && !sampled.delta
                && nearly(glm::length(sampled.direction), 1.0f)
                && glm::dot(lighting.sunDirection, sampled.direction)
                    >= lighting.sunCosineHalfAngle - 1.0e-6f
                && nearly(sampled.pdf, 1.0f / lighting.sunSolidAngle, 1.0e-2f)
                && nearly(
                    sampled.radiance * lighting.sunSolidAngle,
                    lighting.sunIrradiance,
                    1.0e-4f)
                && nearly(
                    sampled.pdf,
                    xrphoton::sunPdf(lighting, sampled.direction),
                    1.0e-2f)
                && nearly(
                    sampled.radiance,
                    xrphoton::evaluateSunRadiance(
                        lighting,
                        sampled.direction),
                    1.0e-3f),
            "uniform-cone sun samples round-trip evaluation/PDF and preserve energy");
    }
    expect(
        xrphoton::sunPdf(lighting, -lighting.sunDirection) == 0.0f
            && xrphoton::evaluateSunRadiance(
                lighting,
                -lighting.sunDirection) == glm::vec3{},
        "directions outside the solar disc have no sun density or radiance");

    xrphoton::SceneLighting wideSun = xrphoton::DefaultSceneLighting;
    wideSun.sun.angularRadiusRadians = 2.0f * TestPi / 180.0f;
    xrphoton::FrameLighting wideFrame;
    xrphoton::SunSample wideSample;
    expect(
        xrphoton::makeFrameLighting(wideSun, 0u, &wideFrame)
            && xrphoton::sampleSun(wideFrame, {0.5f, 0.5f}, &wideSample)
            && wideFrame.sunSolidAngle > lighting.sunSolidAngle
            && nearly(
                wideSample.radiance * wideFrame.sunSolidAngle,
                wideFrame.sunIrradiance,
                1.0e-4f),
        "widening the disc lowers radiance while preserving mean incident sun energy");

    xrphoton::SceneLighting deltaSun = xrphoton::DefaultSceneLighting;
    deltaSun.sun.angularRadiusRadians = 0.0f;
    xrphoton::FrameLighting deltaFrame;
    xrphoton::SunSample deltaSample;
    expect(
        xrphoton::makeFrameLighting(deltaSun, 0u, &deltaFrame)
            && deltaFrame.sunCosineHalfAngle == 1.0f
            && deltaFrame.sunSolidAngle == 0.0f
            && xrphoton::sampleSun(deltaFrame, {0.75f, 0.25f}, &deltaSample)
            && deltaSample.delta && deltaSample.pdf == 1.0f
            && deltaSample.direction == deltaFrame.sunDirection
            && deltaSample.radiance == deltaFrame.sunIrradiance
            && xrphoton::sunPdf(deltaFrame, deltaFrame.sunDirection) == 0.0f,
        "zero angular radius retains the exact delta-sun compatibility contract");
}

void testGlassSamplingAndShadowApproximation()
{
    xrphoton::FrameLighting lighting;
    expect(
        xrphoton::makeFrameLighting(
            xrphoton::DefaultSceneLighting,
            0u,
            &lighting),
        "glass sky test lighting packs");
    const glm::vec3 normal{0.0f, 1.0f, 0.0f};
    xrphoton::SkySample front;
    xrphoton::SkySample back;
    expect(
        xrphoton::sampleSky(lighting, normal, {0.25f, 0.125f}, &front, true)
            && xrphoton::sampleSky(lighting, normal, {0.75f, 0.125f}, &back, true)
            && glm::dot(normal, front.direction) > 0.0f
            && glm::dot(normal, back.direction) < 0.0f
            && nearly(front.pdf, back.pdf)
            && nearly(
                front.pdf,
                xrphoton::skyPdf(normal, front.direction, true)),
        "Glass sky sampling is an equal mixture over both shading hemispheres");

    const float normalFresnel = xrphoton::dielectricFresnel(
        1.0f,
        1.0f,
        xrphoton::GlassIor);
    expect(
        nearly(normalFresnel, 0.04f)
            && xrphoton::dielectricFresnel(
                0.5f,
                xrphoton::GlassIor,
                1.0f) == 1.0f,
        "exact Glass Fresnel pins normal incidence and internal total reflection");
    const glm::vec3 attenuation = xrphoton::glassShadowAttenuation(
        {0.25f, 0.5f, 1.0f},
        1.0f,
        true);
    expect(
        nearly(attenuation.x, 0.24f)
            && nearly(attenuation.y, 0.48f)
            && nearly(attenuation.z, 0.96f)
            && xrphoton::glassShadowAttenuation(
                {1.0f, 1.0f, 1.0f},
                0.5f,
                false) == glm::vec3{},
        "straight Glass shadow attenuation applies tint/Fresnel and respects TIR");
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

void testEmitterPdfAndEstimatorFlags()
{
    expect(
        nearly(xrphoton::emitterSolidAnglePdf(0.5f, 0.25f, 2.0f, 8.0f, 0.5f), 1.0f),
        "emitter area density converts to the expected solid-angle PDF");
    expect(
        xrphoton::emitterSolidAnglePdf(0.0f, 0.25f, 2.0f, 8.0f, 0.5f) == 0.0f
            && xrphoton::emitterSolidAnglePdf(0.5f, 0.25f, 0.0f, 8.0f, 0.5f)
                == 0.0f
            && xrphoton::emitterSolidAnglePdf(0.5f, 0.25f, 2.0f, 8.0f, 0.0f)
                == 0.0f,
        "absent selection, area, or facing produces no competing emitter PDF");
    expect(
        xrphoton::estimatorFlags(xrphoton::EstimatorMode::Mis) == 0
            && xrphoton::estimatorFlags(xrphoton::EstimatorMode::Nee)
                == (1u << xrphoton::FrameLightingEstimatorShift)
            && xrphoton::estimatorFlags(xrphoton::EstimatorMode::Bsdf)
                == (2u << xrphoton::FrameLightingEstimatorShift),
        "reference estimator names map exactly onto the FrameLighting flag ABI");
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
    testEmitterRecordAbi();
    testEmitterDistribution();
    testEmitterLookupAndSelectors();
    testEmitterRejections();
    testFrameAbiAndFlags();
    testSelectorPacking();
    testSkyEvaluationSamplingAndPdf();
    testTimeOfDayAndPerezKnownAnswers();
    testFiniteSunSamplingAndEnergy();
    testGlassSamplingAndShadowApproximation();
    testMisWeights();
    testEmitterPdfAndEstimatorFlags();
    testDynamicBufferMath();

    if (failureCount != 0) {
        std::cerr << failureCount << " scene-lighting assertion(s) failed.\n";
        return 1;
    }
    std::cout << "scene lighting tests passed\n";
    return 0;
}
