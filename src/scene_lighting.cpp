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
constexpr float TwoPi = 6.28318530717958647692f;
constexpr float NormalizeEpsilonSquared = 1.0e-12f;
constexpr glm::vec3 LuminanceWeights{0.2126f, 0.7152f, 0.0722f};

bool isFiniteNonnegative(const glm::vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z) && value.x >= 0.0f && value.y >= 0.0f
        && value.z >= 0.0f;
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
}

const SceneLighting DefaultSceneLighting{
    .sun = {
        .direction = {-0.35f, 0.9f, -0.25f},
        .irradiance = {2.5f, 2.25f, 1.9f},
    },
    .sky = {
        .zenithRadiance = {0.055f, 0.12f, 0.28f},
        .horizonRadiance = {0.32f, 0.38f, 0.46f},
        .enabled = true,
    },
    .lights = {},
    .lightCdf = {},
    .emitterLookup = {},
    .totalLightPower = 0.0f,
    .instanceCount = 0,
};

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
    if (output == nullptr
        || !isFiniteNonnegative(scene.sun.irradiance)
        || !isFiniteNonnegative(scene.sky.zenithRadiance)
        || !isFiniteNonnegative(scene.sky.horizonRadiance)) {
        return false;
    }

    FrameLighting packed{};
    packed.instanceCount = instanceCount;

    if (hasPower(scene.sun.irradiance)) {
        if (!normalizedFinite(scene.sun.direction, &packed.sunDirection)) {
            return false;
        }
        packed.sunIrradiance = scene.sun.irradiance;
    }

    const bool skyHasPower = scene.sky.enabled
        && (hasPower(scene.sky.zenithRadiance)
            || hasPower(scene.sky.horizonRadiance));
    const bool emitterTablesEmpty = scene.lights.empty() && scene.lightCdf.empty();
    const bool emittersHavePower = !scene.lights.empty()
        && checkedU32(scene.lights.size())
        && scene.lightCdf.size() == scene.lights.size()
        && std::isfinite(scene.totalLightPower)
        && scene.totalLightPower > 0.0f;
    if ((!emitterTablesEmpty && !emittersHavePower)
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
    if (emittersHavePower) {
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
    if (skyHasPower) {
        packed.skyZenith = scene.sky.zenithRadiance;
        packed.skyHorizon = scene.sky.horizonRadiance;
    }
    const float selectorCount = static_cast<float>(skyHasPower)
        + static_cast<float>(emittersHavePower);
    if (selectorCount > 0.0f) {
        packed.pSky = skyHasPower ? 1.0f / selectorCount : 0.0f;
        packed.pEmitters = emittersHavePower ? 1.0f / selectorCount : 0.0f;
    }
    packed.lightCount = static_cast<std::uint32_t>(scene.lights.size());
    packed.totalLightPower = scene.totalLightPower;

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
    return glm::mix(
        lighting.skyHorizon,
        lighting.skyZenith,
        std::clamp(direction.y, 0.0f, 1.0f));
}

float skyPdf(const glm::vec3& surfaceNormal, const glm::vec3& direction)
{
    return std::max(glm::dot(surfaceNormal, direction), 0.0f) * InversePi;
}

bool sampleSky(
    const FrameLighting& lighting,
    const glm::vec3& surfaceNormal,
    const glm::vec2& sample,
    SkySample* output)
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
    const glm::vec3 normal = surfaceNormal;

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

    const float radius = std::sqrt(sample.x);
    const float angle = TwoPi * sample.y;
    const glm::vec3 local{
        radius * std::cos(angle),
        radius * std::sin(angle),
        std::sqrt(std::max(0.0f, 1.0f - sample.x)),
    };
    const glm::vec3 direction = glm::normalize(
        tangent * local.x + bitangent * local.y + normal * local.z);

    *output = {
        .direction = direction,
        .radiance = evaluateSkyRadiance(lighting, direction),
        .pdf = skyPdf(normal, direction),
    };
    return output->pdf > 0.0f;
}
}
