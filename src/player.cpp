#include "player.hpp"

#include <cmath>

#include <glm/geometric.hpp>

namespace xrphoton
{
namespace
{
constexpr float NormalizeEpsilonSquared = 1.0e-12f;
}

glm::vec3 playerHorizontalVelocity(
    float yaw,
    float forwardAxis,
    float rightAxis,
    bool sprint,
    bool crouched)
{
    if (!std::isfinite(yaw)
        || !std::isfinite(forwardAxis)
        || !std::isfinite(rightAxis)) {
        return {};
    }

    const glm::vec3 forward{std::sin(yaw), 0.0f, std::cos(yaw)};
    const glm::vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const glm::vec3 direction = forwardAxis * forward + rightAxis * right;
    const float lengthSquared = glm::dot(direction, direction);
    if (lengthSquared <= NormalizeEpsilonSquared) {
        return {};
    }

    const float speed = crouched
        ? PlayerCrouchSpeed
        : PlayerRunSpeed * (sprint ? PlayerSprintMultiplier : 1.0f);
    return direction * (speed / std::sqrt(lengthSquared));
}
}
