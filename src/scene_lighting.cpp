#include "scene_lighting.hpp"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace xrphoton
{
namespace
{
constexpr float InversePi = 0.31830988618379067154f;
constexpr float TwoPi = 6.28318530717958647692f;
constexpr float NormalizeEpsilonSquared = 1.0e-12f;

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
};

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
    if (skyHasPower) {
        packed.skyZenith = scene.sky.zenithRadiance;
        packed.skyHorizon = scene.sky.horizonRadiance;
        packed.pSky = 1.0f;
    }

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
