#include "scene_lighting.hpp"

#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

namespace xrphoton
{
namespace
{
constexpr float InversePi = 0.31830988618379067154f;
constexpr float Pi = 3.14159265358979323846f;
constexpr float TwoPi = 6.28318530717958647692f;
constexpr float NormalizeEpsilonSquared = 1.0e-12f;
constexpr glm::vec3 LuminanceWeights{0.2126f, 0.7152f, 0.0722f};
constexpr float DegreesToRadians = Pi / 180.0f;
constexpr float ModelLatitudeRadians = 46.5f * DegreesToRadians;
constexpr float ModelSolarDeclinationRadians = 23.44f * DegreesToRadians;
constexpr float ModelTurbidity = 3.0f;
// Preetham returns zenith luminance in kcd/m^2; this exposure-independent conversion
// maps it into the renderer's established relative-radiance scale.
constexpr float SkyLuminanceScale = 0.04f;
constexpr glm::vec3 ZenithSunIrradiance{2.5f, 2.25f, 1.9f};
constexpr glm::vec3 NightZenithRadiance{0.0015f, 0.003f, 0.008f};
constexpr glm::vec3 NightHorizonRadiance{0.006f, 0.008f, 0.014f};

bool isFiniteNonnegative(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && value.x >= 0.0f && value.y >= 0.0f
        && value.z >= 0.0f;
}

bool isFinite(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

bool hasPower(const glm::vec3& value)
{
    return value.x > 0.0f || value.y > 0.0f || value.z > 0.0f;
}

bool fail(std::string* error, const std::string& message)
{
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool checkedRange(
    std::size_t first,
    std::size_t count,
    std::size_t size)
{
    return first <= size && count <= size - first;
}

bool finiteAffineNonsingular(const glm::mat4& transform)
{
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            if (!std::isfinite(transform[column][row])) {
                return false;
            }
        }
    }
    constexpr float AffineTolerance = 1.0e-4f;
    if (std::abs(transform[0][3]) > AffineTolerance
        || std::abs(transform[1][3]) > AffineTolerance
        || std::abs(transform[2][3]) > AffineTolerance
        || std::abs(transform[3][3] - 1.0f) > AffineTolerance) {
        return false;
    }
    const double m00 = transform[0][0];
    const double m01 = transform[1][0];
    const double m02 = transform[2][0];
    const double m10 = transform[0][1];
    const double m11 = transform[1][1];
    const double m12 = transform[2][1];
    const double m20 = transform[0][2];
    const double m21 = transform[1][2];
    const double m22 = transform[2][2];
    const double determinant = m00 * (m11 * m22 - m12 * m21)
        - m01 * (m10 * m22 - m12 * m20)
        + m02 * (m10 * m21 - m11 * m20);
    return std::isfinite(determinant) && determinant != 0.0;
}

bool checkedU32(std::size_t value)
{
    return value <= std::numeric_limits<std::uint32_t>::max();
}

bool normalizedFinite(const glm::vec3& value, glm::vec3* output)
{
    const float squaredLength = glm::dot(value, value);
    if (!std::isfinite(squaredLength) || squaredLength <= NormalizeEpsilonSquared) {
        return false;
    }
    *output = value * (1.0f / std::sqrt(squaredLength));
    return std::isfinite(output->x) && std::isfinite(output->y)
        && std::isfinite(output->z);
}

bool isUnitFinite(const glm::vec3& value)
{
    const float squaredLength = glm::dot(value, value);
    return std::isfinite(squaredLength)
        && std::abs(squaredLength - 1.0f) <= 1.0e-4f;
}

float smoothstep(float edge0, float edge1, float value)
{
    const float parameter = std::clamp(
        (value - edge0) / (edge1 - edge0),
        0.0f,
        1.0f);
    return parameter * parameter * (3.0f - 2.0f * parameter);
}

glm::vec3 solarDirection(float timeOfDayHours)
{
    // Fixed local ENU frame: +X east, +Y up, +Z north. The representative
    // latitude and summer-solstice declination are temporary level/weather policy.
    const float hourAngle = (timeOfDayHours - 12.0f) * 15.0f * DegreesToRadians;
    const float sinLatitude = std::sin(ModelLatitudeRadians);
    const float cosLatitude = std::cos(ModelLatitudeRadians);
    const float sinDeclination = std::sin(ModelSolarDeclinationRadians);
    const float cosDeclination = std::cos(ModelSolarDeclinationRadians);
    return glm::normalize(glm::vec3{
        -cosDeclination * std::sin(hourAngle),
        sinLatitude * sinDeclination
            + cosLatitude * cosDeclination * std::cos(hourAngle),
        cosLatitude * sinDeclination
            - sinLatitude * cosDeclination * std::cos(hourAngle),
    });
}

std::array<glm::vec3, 5> preethamPerezCoefficients(float turbidity)
{
    // Components are (x chromaticity, y chromaticity, luminance Y), matching the
    // original Preetham/Shirley/Smits SIGGRAPH 1999 fits (DOI 10.1145/311535.311545).
    // Packing xyY keeps the closed form exact; conversion to linear sRGB happens only
    // after directional evaluation.
    return {{
        {-0.0193f * turbidity - 0.2592f,
         -0.0167f * turbidity - 0.2608f,
          0.1787f * turbidity - 1.4630f},
        {-0.0665f * turbidity + 0.0008f,
         -0.0950f * turbidity + 0.0092f,
         -0.3554f * turbidity + 0.4275f},
        {-0.0004f * turbidity + 0.2125f,
         -0.0079f * turbidity + 0.2102f,
         -0.0227f * turbidity + 5.3251f},
        {-0.0641f * turbidity - 0.8989f,
         -0.0441f * turbidity - 1.6537f,
          0.1206f * turbidity - 2.5771f},
        {-0.0033f * turbidity + 0.0452f,
         -0.0109f * turbidity + 0.0529f,
         -0.0670f * turbidity + 0.3703f},
    }};
}

glm::vec3 preethamZenithXyY(float turbidity, float solarZenith)
{
    const float theta2 = solarZenith * solarZenith;
    const float theta3 = theta2 * solarZenith;
    const float turbidity2 = turbidity * turbidity;
    const float x =
        (0.00165f * theta3 - 0.00374f * theta2
            + 0.00208f * solarZenith) * turbidity2
        + (-0.02902f * theta3 + 0.06377f * theta2
            - 0.03202f * solarZenith + 0.00394f) * turbidity
        + (0.11693f * theta3 - 0.21196f * theta2
            + 0.06052f * solarZenith + 0.25885f);
    const float y =
        (0.00275f * theta3 - 0.00610f * theta2
            + 0.00316f * solarZenith) * turbidity2
        + (-0.04214f * theta3 + 0.08970f * theta2
            - 0.04153f * solarZenith + 0.00516f) * turbidity
        + (0.15346f * theta3 - 0.26756f * theta2
            + 0.06669f * solarZenith + 0.26688f);
    const float chi = (4.0f / 9.0f - turbidity / 120.0f)
        * (Pi - 2.0f * solarZenith);
    const float luminance = ((4.0453f * turbidity - 4.9710f) * std::tan(chi)
        - 0.2155f * turbidity + 2.4192f) * SkyLuminanceScale;
    return {x, y, std::max(luminance, 0.0f)};
}

glm::vec3 attenuatedSunIrradiance(float solarElevationRadians)
{
    const float elevationDegrees = solarElevationRadians / DegreesToRadians;
    if (elevationDegrees <= 0.0f) {
        return {};
    }

    // Kasten-Young relative optical air mass plus a compact RGB extinction fit. The
    // normalization keeps ZenithSunIrradiance's existing incident-energy meaning at
    // zenith while producing the expected warm, dim disc near the horizon.
    const float airMass = 1.0f / (
        std::sin(solarElevationRadians)
        + 0.50572f * std::pow(elevationDegrees + 6.07995f, -1.6364f));
    const float relativePath = std::max(airMass - 1.0f, 0.0f);
    constexpr glm::vec3 OpticalDepth{0.035f, 0.07f, 0.14f};
    const glm::vec3 transmittance{
        std::exp(-OpticalDepth.x * relativePath),
        std::exp(-OpticalDepth.y * relativePath),
        std::exp(-OpticalDepth.z * relativePath),
    };
    const float horizonVisibility = smoothstep(
        0.0f,
        1.0f,
        elevationDegrees);
    return ZenithSunIrradiance * transmittance * horizonVisibility;
}

bool configureTimeOfDay(float timeOfDayHours, SceneLighting* lighting)
{
    if (lighting == nullptr || !std::isfinite(timeOfDayHours)
        || timeOfDayHours < 0.0f || timeOfDayHours >= 24.0f) {
        return false;
    }

    const glm::vec3 actualSunDirection = solarDirection(timeOfDayHours);
    const float solarElevation = std::asin(std::clamp(
        actualSunDirection.y,
        -1.0f,
        1.0f));
    const float daylightBlend = smoothstep(
        -6.0f * DegreesToRadians,
        0.0f,
        solarElevation);

    glm::vec3 modelSunDirection = actualSunDirection;
    if (modelSunDirection.y < 0.0f && daylightBlend > 0.0f) {
        // Freeze Preetham at its horizon-domain boundary through civil twilight.
        // Only the blend changes below zero elevation; the model is not extrapolated.
        modelSunDirection.y = 0.0f;
        modelSunDirection = glm::normalize(modelSunDirection);
    }
    const float modelSolarZenith = std::acos(std::clamp(
        modelSunDirection.y,
        0.0f,
        1.0f));

    const DirectionalSun sun{
        .direction = modelSunDirection,
        .irradiance = attenuatedSunIrradiance(solarElevation),
        .angularRadiusRadians = SunAngularRadiusRadians,
    };
    const AnalyticSky sky{
        .zenithRadiance = preethamZenithXyY(
            ModelTurbidity,
            modelSolarZenith),
        .horizonRadiance = NightHorizonRadiance,
        .perezCoefficients = preethamPerezCoefficients(ModelTurbidity),
        .nightZenithRadiance = NightZenithRadiance,
        .daylightBlend = daylightBlend,
        .perezEnabled = true,
        .enabled = true,
    };
    // Publish only the mutable analytic state. In particular, a per-frame time update
    // must not copy or allocate the immutable light/CDF/lookup tables.
    lighting->sun = sun;
    lighting->sky = sky;
    lighting->emitterScale = 1.0f - daylightBlend;
    return true;
}

SceneLighting makeLightingAtTime(float timeOfDayHours)
{
    SceneLighting lighting;
    [[maybe_unused]] const bool configured = configureTimeOfDay(
        timeOfDayHours,
        &lighting);
    return lighting;
}

glm::vec3 perezDistribution(
    const glm::vec3& coefficientA,
    const glm::vec3& coefficientB,
    const glm::vec3& coefficientC,
    const glm::vec3& coefficientD,
    const glm::vec3& coefficientE,
    float cosineTheta,
    float gamma,
    float cosineGamma)
{
    cosineTheta = std::max(cosineTheta, 0.01f);
    glm::vec3 result;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result[channel] = (1.0f + coefficientA[channel]
                * std::exp(coefficientB[channel] / cosineTheta))
            * (1.0f + coefficientC[channel]
                * std::exp(coefficientD[channel] * gamma)
                + coefficientE[channel] * cosineGamma * cosineGamma);
    }
    return result;
}

glm::vec3 xyYToLinearRgb(const glm::vec3& xyY)
{
    const float chromaticityY = std::max(xyY.y, 1.0e-4f);
    const float luminanceY = std::max(xyY.z, 0.0f);
    const float tristimulusX = xyY.x * luminanceY / chromaticityY;
    const float tristimulusZ = std::max(
        0.0f,
        (1.0f - xyY.x - xyY.y) * luminanceY / chromaticityY);
    return glm::max(glm::vec3{
        3.2406f * tristimulusX - 1.5372f * luminanceY
            - 0.4986f * tristimulusZ,
        -0.9689f * tristimulusX + 1.8758f * luminanceY
            + 0.0415f * tristimulusZ,
        0.0557f * tristimulusX - 0.2040f * luminanceY
            + 1.0570f * tristimulusZ,
    }, glm::vec3{});
}

bool validPerezSky(const AnalyticSky& sky)
{
    if (!isFinite(sky.zenithRadiance)
        || !isFiniteNonnegative(sky.horizonRadiance)
        || !isFiniteNonnegative(sky.nightZenithRadiance)
        || !std::isfinite(sky.daylightBlend)
        || sky.daylightBlend < 0.0f || sky.daylightBlend > 1.0f
        || sky.zenithRadiance.x < 0.0f || sky.zenithRadiance.x > 1.0f
        || sky.zenithRadiance.y <= 0.0f || sky.zenithRadiance.y > 1.0f
        || sky.zenithRadiance.x + sky.zenithRadiance.y > 1.0f
        || sky.zenithRadiance.z < 0.0f) {
        return false;
    }
    for (const glm::vec3& coefficients : sky.perezCoefficients) {
        if (!isFinite(coefficients)) {
            return false;
        }
    }
    return true;
}
}

const SceneLighting DefaultSceneLighting = makeLightingAtTime(DefaultTimeOfDayHours);

bool updateSceneLightingTimeOfDay(
    float timeOfDayHours,
    SceneLighting* lighting)
{
    return configureTimeOfDay(timeOfDayHours, lighting);
}

bool validateEmitterLookup(
    std::span<const EmitterLookupRecord> lookup,
    std::uint32_t instanceCount,
    std::uint32_t lightCount)
{
    if (!checkedU32(lookup.size()) || lookup.size() < instanceCount) {
        return false;
    }

    std::size_t expectedRange = instanceCount;
    std::size_t expectedLight = 0;
    for (std::uint32_t instanceIndex = 0;
         instanceIndex < instanceCount;
         ++instanceIndex) {
        const EmitterLookupRecord& header = lookup[instanceIndex];
        if (header.reserved0 != 0 || header.reserved1 != 0
            || header.first != expectedRange
            || !checkedRange(header.first, header.count, lookup.size())) {
            return false;
        }
        for (std::uint32_t geometryOffset = 0;
             geometryOffset < header.count;
             ++geometryOffset) {
            const EmitterLookupRecord& range =
                lookup[static_cast<std::size_t>(header.first) + geometryOffset];
            const bool sentinel = range.first == NoEmitterLight;
            if (range.reserved0 != 0 || range.reserved1 != 0
                || (sentinel && range.count != 0)
                || (!sentinel
                    && (range.count == 0 || range.first != expectedLight
                        || !checkedRange(range.first, range.count, lightCount)))) {
                return false;
            }
            if (!sentinel) {
                expectedLight += range.count;
            }
        }
        expectedRange += header.count;
    }
    return expectedRange == lookup.size() && expectedLight == lightCount;
}

bool buildSceneLighting(
    const SceneData& scene,
    std::span<const std::size_t> dynamicInstances,
    SceneLighting* lighting,
    std::string* error)
{
    if (lighting == nullptr) {
        return fail(error, "SceneLighting output is null.");
    }
    if (!checkedU32(scene.instances.size())
        || !checkedU32(scene.geometries.size())
        || !checkedU32(scene.materials.size())) {
        return fail(error, "Scene lighting input exceeds its 32-bit GPU ABI.");
    }

    std::vector<bool> isDynamic(scene.instances.size(), false);
    for (const std::size_t instanceIndex : dynamicInstances) {
        if (instanceIndex >= scene.instances.size()) {
            return fail(error, "Dynamic scene-lighting instance is out of range.");
        }
        if (isDynamic[instanceIndex]) {
            return fail(error, "Dynamic scene-lighting instance is duplicated.");
        }
        isDynamic[instanceIndex] = true;
    }

    SceneLighting built = *lighting;
    built.lights.clear();
    built.lightCdf.clear();
    built.emitterLookup.clear();
    built.totalLightPower = 0.0f;
    built.instanceCount = static_cast<std::uint32_t>(scene.instances.size());
    built.emitterLookup.resize(scene.instances.size());
    std::vector<double> lightWeights;

    for (std::size_t instanceIndex = 0;
         instanceIndex < scene.instances.size();
         ++instanceIndex) {
        const SceneInstance& instance = scene.instances[instanceIndex];
        if (instance.meshIndex >= scene.meshes.size()) {
            return fail(error, "Scene-lighting instance mesh is out of range.");
        }
        if (!finiteAffineNonsingular(instance.transform)) {
            return fail(
                error,
                "Scene-lighting instance transform is not finite, affine and nonsingular.");
        }
        const SceneMesh& mesh = scene.meshes[instance.meshIndex];
        if (!checkedRange(
                mesh.firstGeometry,
                mesh.geometryCount,
                scene.geometries.size())) {
            return fail(error, "Scene-lighting mesh geometry range is invalid.");
        }
        if (!checkedU32(built.emitterLookup.size())
            || static_cast<std::size_t>(mesh.geometryCount)
                > std::numeric_limits<std::uint32_t>::max()
                    - built.emitterLookup.size()) {
            return fail(error, "Emitter lookup header exceeds its 32-bit ABI.");
        }

        built.emitterLookup[instanceIndex] = {
            .first = static_cast<std::uint32_t>(built.emitterLookup.size()),
            .count = mesh.geometryCount,
        };

        for (std::size_t geometryOffset = 0;
             geometryOffset < mesh.geometryCount;
             ++geometryOffset) {
            const SceneGeometry& geometry =
                scene.geometries[mesh.firstGeometry + geometryOffset];
            if (geometry.materialIndex >= scene.materials.size()) {
                return fail(error, "Emitter geometry material is out of range.");
            }
            const SceneMaterial& material = scene.materials[geometry.materialIndex];
            const glm::vec3 emission{
                material.emission[0],
                material.emission[1],
                material.emission[2],
            };
            if (!isFiniteNonnegative(emission)) {
                return fail(error, "Emitter material has invalid emission.");
            }
            if (!hasPower(emission)) {
                built.emitterLookup.push_back({});
                continue;
            }
            if (isDynamic[instanceIndex]) {
                return fail(error, "Emission on a dynamic placement is unsupported.");
            }
            if (geometry.alphaTested) {
                return fail(error, "Emission on alpha-tested geometry is unsupported.");
            }
            if (geometry.indexCount == 0 || geometry.indexCount % 3 != 0
                || !checkedRange(
                    geometry.firstIndex,
                    geometry.indexCount,
                    scene.indices.size())) {
                return fail(error, "Emitter geometry index range is invalid.");
            }
            const std::uint32_t triangleCount = geometry.indexCount / 3;
            if (!checkedU32(built.lights.size())
                || static_cast<std::size_t>(triangleCount)
                    > std::numeric_limits<std::uint32_t>::max()
                        - built.lights.size()) {
                return fail(error, "Emitter triangle range exceeds its 32-bit ABI.");
            }

            EmitterLookupRecord range{
                .first = static_cast<std::uint32_t>(built.lights.size()),
                .count = triangleCount,
            };
            const double luminance = static_cast<double>(
                glm::dot(emission, LuminanceWeights));
            for (std::uint32_t indexOffset = 0;
                 indexOffset < geometry.indexCount;
                 indexOffset += 3) {
                glm::vec3 worldVertices[3]{};
                for (std::uint32_t corner = 0; corner < 3; ++corner) {
                    const std::size_t sceneIndex =
                        static_cast<std::size_t>(geometry.firstIndex)
                        + indexOffset + corner;
                    const std::uint32_t localVertex = scene.indices[sceneIndex];
                    if (localVertex >= geometry.vertexCount
                        || geometry.firstVertex
                            > std::numeric_limits<std::uint32_t>::max() - localVertex) {
                        return fail(error, "Emitter triangle vertex is out of range.");
                    }
                    const std::size_t absoluteVertex =
                        static_cast<std::size_t>(geometry.firstVertex) + localVertex;
                    if (absoluteVertex > std::numeric_limits<std::size_t>::max() / 3
                        || !checkedRange(absoluteVertex * 3, 3, scene.positions.size())) {
                        return fail(error, "Emitter position range is invalid.");
                    }
                    const std::size_t position = absoluteVertex * 3;
                    const glm::vec4 world = instance.transform * glm::vec4{
                        scene.positions[position],
                        scene.positions[position + 1],
                        scene.positions[position + 2],
                        1.0f,
                    };
                    if (!std::isfinite(world.x) || !std::isfinite(world.y)
                        || !std::isfinite(world.z)) {
                        return fail(error, "Emitter world position is invalid.");
                    }
                    // Match VkTransformMatrixKHR exactly: its 3x4 affine transform
                    // ignores the homogeneous row rather than dividing by world.w.
                    worldVertices[corner] = glm::vec3{world};
                }

                const glm::vec3 edge1 = worldVertices[1] - worldVertices[0];
                const glm::vec3 edge2 = worldVertices[2] - worldVertices[0];
                const float area = 0.5f * glm::length(glm::cross(edge1, edge2));
                const double weight = static_cast<double>(area) * luminance;
                if (!std::isfinite(area) || area <= 0.0f
                    || !std::isfinite(weight) || weight <= 0.0) {
                    return fail(error, "Emitter triangle has invalid world-space power.");
                }
                built.lights.push_back({
                    .v0 = worldVertices[0],
                    .area = area,
                    .edge1 = edge1,
                    .edge2 = edge2,
                    .emission = emission,
                    .materialIndex = geometry.materialIndex,
                });
                lightWeights.push_back(weight);
            }
            built.emitterLookup.push_back(range);
        }
    }

    if (built.lights.size() != lightWeights.size()
        || !checkedU32(built.lights.size())
        || !checkedU32(built.emitterLookup.size())) {
        return fail(error, "Emitter light table exceeds its 32-bit ABI.");
    }

    double totalPower = 0.0;
    for (const double weight : lightWeights) {
        totalPower += weight;
    }
    if (!std::isfinite(totalPower)
        || totalPower > std::numeric_limits<float>::max()) {
        return fail(error, "Total emitter power exceeds the FrameLighting ABI.");
    }
    if (!lightWeights.empty() && totalPower <= 0.0) {
        return fail(error, "Emitter distribution has zero total power.");
    }

    built.totalLightPower = static_cast<float>(totalPower);
    built.lightCdf.reserve(lightWeights.size());
    double cumulativePower = 0.0;
    float previousCdf = 0.0f;
    for (std::size_t lightIndex = 0; lightIndex < lightWeights.size(); ++lightIndex) {
        cumulativePower += lightWeights[lightIndex];
        float cdf = static_cast<float>(cumulativePower / totalPower);
        if (lightIndex + 1 == lightWeights.size()) {
            cdf = 1.0f;
        }
        if (!std::isfinite(cdf) || cdf < previousCdf || cdf > 1.0f) {
            return fail(error, "Emitter CDF is not monotonic.");
        }
        // This is the probability represented by the float CDF that the shader
        // actually searches, including any interval that collapsed during rounding.
        built.lights[lightIndex].pTriangle = cdf - previousCdf;
        built.lightCdf.push_back(cdf);
        previousCdf = cdf;
    }

    // Re-validate the compact lookup independently of its construction. This is the
    // same bounded walk closest-hit will perform once P2c consumes binding 8.
    if (!validateEmitterLookup(
            built.emitterLookup,
            built.instanceCount,
            static_cast<std::uint32_t>(built.lights.size()))) {
        return fail(error, "Emitter lookup structure is invalid.");
    }

    if (error != nullptr) {
        error->clear();
    }
    *lighting = std::move(built);
    return true;
}

bool makeFrameLighting(
    const SceneLighting& scene,
    std::uint32_t instanceCount,
    FrameLighting* output)
{
    if (output == nullptr || !isFiniteNonnegative(scene.sun.irradiance)
        || !std::isfinite(scene.sun.angularRadiusRadians)
        || scene.sun.angularRadiusRadians < 0.0f
        || scene.sun.angularRadiusRadians >= 0.5f * Pi
        || !std::isfinite(scene.emitterScale)
        || scene.emitterScale < 0.0f || scene.emitterScale > 1.0f
        || (scene.sky.perezEnabled
                ? !validPerezSky(scene.sky)
                : (!isFiniteNonnegative(scene.sky.zenithRadiance)
                    || !isFiniteNonnegative(scene.sky.horizonRadiance)))) {
        return false;
    }

    FrameLighting packed{};
    packed.instanceCount = instanceCount;

    const bool sunHasPower = hasPower(scene.sun.irradiance);
    const bool daylightNeedsSunDirection = scene.sky.enabled
        && scene.sky.perezEnabled && scene.sky.daylightBlend > 0.0f;
    if (sunHasPower || daylightNeedsSunDirection) {
        if (!normalizedFinite(scene.sun.direction, &packed.sunDirection)) {
            return false;
        }
    }
    if (sunHasPower) {
        packed.sunIrradiance = scene.sun.irradiance;
        if (scene.sun.angularRadiusRadians > 0.0f) {
            packed.sunCosineHalfAngle = std::cos(
                scene.sun.angularRadiusRadians);
            packed.sunSolidAngle = TwoPi
                * (1.0f - packed.sunCosineHalfAngle);
            if (!std::isfinite(packed.sunSolidAngle)
                || packed.sunSolidAngle <= 0.0f) {
                return false;
            }
        }
    }

    const bool perezDaylightHasPower = scene.sky.perezEnabled
        && scene.sky.daylightBlend > 0.0f
        && scene.sky.zenithRadiance.z > 0.0f;
    const bool perezNightHasPower = scene.sky.perezEnabled
        && scene.sky.daylightBlend < 1.0f
        && (hasPower(scene.sky.nightZenithRadiance)
            || hasPower(scene.sky.horizonRadiance));
    const bool skyHasPower = scene.sky.enabled
        && (scene.sky.perezEnabled
            ? (perezDaylightHasPower || perezNightHasPower)
            : (hasPower(scene.sky.zenithRadiance)
                || hasPower(scene.sky.horizonRadiance)));
    const bool emitterTablesEmpty = scene.lights.empty() && scene.lightCdf.empty();
    const bool emitterTablesHavePower = !scene.lights.empty()
        && checkedU32(scene.lights.size())
        && scene.lightCdf.size() == scene.lights.size()
        && std::isfinite(scene.totalLightPower)
        && scene.totalLightPower > 0.0f;
    if ((!emitterTablesEmpty && !emitterTablesHavePower)
        || (emitterTablesEmpty && scene.totalLightPower != 0.0f)) {
        return false;
    }
    if ((!scene.emitterLookup.empty() && scene.instanceCount != instanceCount)
        || !validateEmitterLookup(
            scene.emitterLookup,
            scene.instanceCount,
            static_cast<std::uint32_t>(scene.lights.size()))) {
        return false;
    }
    if (emitterTablesHavePower) {
        float previousCdf = 0.0f;
        for (std::size_t lightIndex = 0;
             lightIndex < scene.lightCdf.size();
             ++lightIndex) {
            const float cdf = scene.lightCdf[lightIndex];
            if (!std::isfinite(cdf) || cdf < previousCdf || cdf > 1.0f
                || scene.lights[lightIndex].pTriangle != cdf - previousCdf) {
                return false;
            }
            previousCdf = cdf;
        }
        if (scene.lightCdf.back() != 1.0f) {
            return false;
        }
    }
    const bool emittersHavePower = emitterTablesHavePower
        && scene.emitterScale > 0.0f;
    if (skyHasPower) {
        packed.skyZenith = scene.sky.zenithRadiance;
        packed.skyHorizon = scene.sky.horizonRadiance;
        if (scene.sky.perezEnabled) {
            packed.skyPerezA = glm::vec4{scene.sky.perezCoefficients[0], 0.0f};
            packed.skyPerezB = glm::vec4{scene.sky.perezCoefficients[1], 0.0f};
            packed.skyPerezC = glm::vec4{scene.sky.perezCoefficients[2], 0.0f};
            packed.skyPerezD = glm::vec4{scene.sky.perezCoefficients[3], 0.0f};
            packed.skyPerezE = glm::vec4{scene.sky.perezCoefficients[4], 0.0f};
            packed.nightZenith = glm::vec4{
                scene.sky.nightZenithRadiance,
                0.0f};
            packed.daylightBlend = scene.sky.daylightBlend;
            packed.flags |= FrameLightingPerezSkyBit;
        }
    }
    const float selectorCount = static_cast<float>(skyHasPower)
        + static_cast<float>(emittersHavePower);
    if (selectorCount > 0.0f) {
        packed.pSky = skyHasPower ? 1.0f / selectorCount : 0.0f;
        packed.pEmitters = emittersHavePower ? 1.0f / selectorCount : 0.0f;
    }
    packed.lightCount = static_cast<std::uint32_t>(scene.lights.size());
    packed.totalLightPower = scene.totalLightPower * scene.emitterScale;
    packed.emitterScale = scene.emitterScale;

    if (!hasValidFrameLightingFlags(packed.flags)) {
        return false;
    }
    *output = packed;
    return true;
}

glm::vec3 evaluateSkyRadiance(
    const FrameLighting& lighting,
    const glm::vec3& direction)
{
    if (direction.y < 0.0f) {
        return {};
    }
    if ((lighting.flags & FrameLightingPerezSkyBit) != 0) {
        const float upperY = std::clamp(direction.y, 0.0f, 1.0f);
        const glm::vec3 nightRadiance = glm::mix(
            lighting.skyHorizon,
            glm::vec3{lighting.nightZenith},
            upperY);
        if (lighting.daylightBlend <= 0.0f) {
            return nightRadiance;
        }

        const float cosineGamma = std::clamp(
            glm::dot(direction, lighting.sunDirection),
            -1.0f,
            1.0f);
        const float gamma = std::acos(cosineGamma);
        const glm::vec3 numerator = perezDistribution(
            glm::vec3{lighting.skyPerezA},
            glm::vec3{lighting.skyPerezB},
            glm::vec3{lighting.skyPerezC},
            glm::vec3{lighting.skyPerezD},
            glm::vec3{lighting.skyPerezE},
            upperY,
            gamma,
            cosineGamma);
        const float cosineSunZenith = std::clamp(
            lighting.sunDirection.y,
            0.0f,
            1.0f);
        const float sunZenith = std::acos(cosineSunZenith);
        const glm::vec3 denominator = perezDistribution(
            glm::vec3{lighting.skyPerezA},
            glm::vec3{lighting.skyPerezB},
            glm::vec3{lighting.skyPerezC},
            glm::vec3{lighting.skyPerezD},
            glm::vec3{lighting.skyPerezE},
            1.0f,
            sunZenith,
            cosineSunZenith);
        const glm::vec3 xyY = lighting.skyZenith * numerator
            / glm::max(denominator, glm::vec3{1.0e-6f});
        return glm::mix(
            nightRadiance,
            xyYToLinearRgb(xyY),
            lighting.daylightBlend);
    }
    return glm::mix(
        lighting.skyHorizon,
        lighting.skyZenith,
        std::clamp(direction.y, 0.0f, 1.0f));
}

float skyPdf(
    const glm::vec3& surfaceNormal,
    const glm::vec3& direction,
    bool twoSided)
{
    const float cosine = glm::dot(surfaceNormal, direction);
    return (twoSided ? 0.5f * std::abs(cosine) : std::max(cosine, 0.0f))
        * InversePi;
}

bool sampleSky(
    const FrameLighting& lighting,
    const glm::vec3& surfaceNormal,
    const glm::vec2& sample,
    SkySample* output,
    bool twoSided)
{
    if (output == nullptr || lighting.pSky <= 0.0f
        || sample.x < 0.0f || sample.x >= 1.0f
        || sample.y < 0.0f || sample.y >= 1.0f) {
        return false;
    }

    // Mirror cosineSampleHemisphere's shader contract exactly: its input is already
    // a unit shading normal. Do not silently normalize only the CPU reference.
    if (!isUnitFinite(surfaceNormal)) {
        return false;
    }
    const bool opposite = twoSided && sample.x >= 0.5f;
    const glm::vec3 normal = opposite ? -surfaceNormal : surfaceNormal;
    const glm::vec2 hemisphereSample{
        twoSided ? std::fmod(sample.x * 2.0f, 1.0f) : sample.x,
        sample.y,
    };

    const float sign = normal.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + normal.z);
    const float b = normal.x * normal.y * a;
    const glm::vec3 tangent{
        1.0f + sign * normal.x * normal.x * a,
        sign * b,
        -sign * normal.x,
    };
    const glm::vec3 bitangent{
        b,
        sign + normal.y * normal.y * a,
        -normal.y,
    };

    const float radius = std::sqrt(hemisphereSample.x);
    const float angle = TwoPi * hemisphereSample.y;
    const glm::vec3 local{
        radius * std::cos(angle),
        radius * std::sin(angle),
        std::sqrt(std::max(0.0f, 1.0f - hemisphereSample.x)),
    };
    const glm::vec3 direction = glm::normalize(
        tangent * local.x + bitangent * local.y + normal * local.z);

    *output = {
        .direction = direction,
        .radiance = evaluateSkyRadiance(lighting, direction),
        .pdf = skyPdf(surfaceNormal, direction, twoSided),
    };
    return output->pdf > 0.0f;
}

float sunPdf(
    const FrameLighting& lighting,
    const glm::vec3& direction)
{
    if (!std::isfinite(lighting.sunSolidAngle)
        || lighting.sunSolidAngle <= 0.0f
        || !hasPower(lighting.sunIrradiance)
        || !isUnitFinite(lighting.sunDirection)
        || !isUnitFinite(direction)
        || glm::dot(lighting.sunDirection, direction)
            < lighting.sunCosineHalfAngle) {
        return 0.0f;
    }
    return 1.0f / lighting.sunSolidAngle;
}

glm::vec3 evaluateSunRadiance(
    const FrameLighting& lighting,
    const glm::vec3& direction)
{
    return sunPdf(lighting, direction) > 0.0f
        ? lighting.sunIrradiance / lighting.sunSolidAngle
        : glm::vec3{};
}

bool sampleSun(
    const FrameLighting& lighting,
    const glm::vec2& sample,
    SunSample* output)
{
    if (output == nullptr || !hasPower(lighting.sunIrradiance)
        || !isUnitFinite(lighting.sunDirection)
        || sample.x < 0.0f || sample.x >= 1.0f
        || sample.y < 0.0f || sample.y >= 1.0f) {
        return false;
    }
    if (lighting.sunSolidAngle == 0.0f
        && lighting.sunCosineHalfAngle == 1.0f) {
        *output = {
            .direction = lighting.sunDirection,
            .radiance = lighting.sunIrradiance,
            .pdf = 1.0f,
            .delta = true,
        };
        return true;
    }
    if (!std::isfinite(lighting.sunSolidAngle)
        || lighting.sunSolidAngle <= 0.0f
        || !std::isfinite(lighting.sunCosineHalfAngle)
        || lighting.sunCosineHalfAngle < 0.0f
        || lighting.sunCosineHalfAngle >= 1.0f) {
        return false;
    }

    const float cosineTheta = glm::mix(
        1.0f,
        lighting.sunCosineHalfAngle,
        sample.x);
    const float sineTheta = std::sqrt(std::max(
        0.0f,
        1.0f - cosineTheta * cosineTheta));
    const float phi = TwoPi * sample.y;
    const float sign = lighting.sunDirection.z >= 0.0f ? 1.0f : -1.0f;
    const float a = -1.0f / (sign + lighting.sunDirection.z);
    const float b = lighting.sunDirection.x * lighting.sunDirection.y * a;
    const glm::vec3 tangent{
        1.0f + sign * lighting.sunDirection.x * lighting.sunDirection.x * a,
        sign * b,
        -sign * lighting.sunDirection.x,
    };
    const glm::vec3 bitangent{
        b,
        sign + lighting.sunDirection.y * lighting.sunDirection.y * a,
        -lighting.sunDirection.y,
    };
    const glm::vec3 direction = glm::normalize(
        tangent * (sineTheta * std::cos(phi))
        + bitangent * (sineTheta * std::sin(phi))
        + lighting.sunDirection * cosineTheta);
    *output = {
        .direction = direction,
        .radiance = lighting.sunIrradiance / lighting.sunSolidAngle,
        .pdf = 1.0f / lighting.sunSolidAngle,
        .delta = false,
    };
    return true;
}

float dielectricFresnel(float cosineI, float etaI, float etaT)
{
    if (!std::isfinite(cosineI) || !std::isfinite(etaI)
        || !std::isfinite(etaT) || etaI <= 0.0f || etaT <= 0.0f) {
        return 1.0f;
    }
    cosineI = std::clamp(std::abs(cosineI), 0.0f, 1.0f);
    const float eta = etaI / etaT;
    const float sineSquaredT = eta * eta
        * std::max(0.0f, 1.0f - cosineI * cosineI);
    if (sineSquaredT >= 1.0f) {
        return 1.0f;
    }
    const float cosineT = std::sqrt(std::max(0.0f, 1.0f - sineSquaredT));
    const float parallel = (etaT * cosineI - etaI * cosineT)
        / std::max(etaT * cosineI + etaI * cosineT, 1.0e-8f);
    const float perpendicular = (etaI * cosineI - etaT * cosineT)
        / std::max(etaI * cosineI + etaT * cosineT, 1.0e-8f);
    return 0.5f * (parallel * parallel + perpendicular * perpendicular);
}

glm::vec3 glassShadowAttenuation(
    const glm::vec3& tint,
    float cosineI,
    bool entering)
{
    const float etaI = entering ? 1.0f : GlassIor;
    const float etaT = entering ? GlassIor : 1.0f;
    const float transmission = 1.0f
        - dielectricFresnel(cosineI, etaI, etaT);
    return glm::clamp(tint, glm::vec3{}, glm::vec3{1.0f}) * transmission;
}

float emitterSolidAnglePdf(
    float pEmitters,
    float pTriangle,
    float area,
    float distanceSquared,
    float lightCosine)
{
    if (!std::isfinite(pEmitters) || !std::isfinite(pTriangle)
        || !std::isfinite(area) || !std::isfinite(distanceSquared)
        || !std::isfinite(lightCosine)
        || pEmitters <= 0.0f || pTriangle <= 0.0f || area <= 0.0f
        || distanceSquared < 0.0f || lightCosine <= 0.0f) {
        return 0.0f;
    }
    const float pdf = pEmitters * pTriangle * distanceSquared
        / (area * lightCosine);
    return std::isfinite(pdf) ? pdf : 0.0f;
}
}
