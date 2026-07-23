#pragma once

#include <glm/vec3.hpp>

namespace xrphoton
{
inline constexpr float PlayerEyeHeight = 1.7f;
inline constexpr float PlayerCrouchEyeHeight = 1.1f;
inline constexpr float PlayerRunSpeed = 3.0f;
inline constexpr float PlayerCrouchSpeed = 1.5f;
inline constexpr float PlayerSprintMultiplier = 4.0f;

// Convert camera-relative digital movement axes into a normalized, world-space X/Z
// velocity. Pitch is intentionally absent so looking up or down cannot make the
// character fly or slow down.
[[nodiscard]] glm::vec3 playerHorizontalVelocity(
    float yaw,
    float forwardAxis,
    float rightAxis,
    bool sprint,
    bool crouched = false);
}
