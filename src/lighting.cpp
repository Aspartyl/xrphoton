#include "lighting.hpp"

#include <cmath>

#include <glm/geometric.hpp>

namespace xrphoton
{
namespace
{
constexpr float NormalizeEpsilonSquared = 1.0e-12f;
}

const DirectionalSun DefaultSun{
    .direction = {-0.35f, 0.9f, -0.25f},
    .radiance = {2.5f, 2.25f, 1.9f},
};

RaygenPushConstants makeRaygenPushConstants(
    const CameraPushConstants& camera,
    const DirectionalSun& sun,
    std::uint32_t frameIndex)
{
    RaygenPushConstants result{};
    result.camera = camera;

    const float squaredLength = glm::dot(sun.direction, sun.direction);
    if (std::isfinite(squaredLength)
        && squaredLength > NormalizeEpsilonSquared) {
        result.sunDirection =
            sun.direction * (1.0f / std::sqrt(squaredLength));
    }

    result.sunRadiance = sun.radiance;
    result.frameIndex = frameIndex;
    return result;
}
}
