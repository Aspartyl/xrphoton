#pragma once

#include "physics.hpp"

namespace xrphoton
{
enum class PhysicsTestMotionQuality
{
    Discrete,
    LinearCast,
};

[[nodiscard]] bool setPhysicsBodyMotionQualityForTest(
    PhysicsWorld* world,
    std::size_t instanceIndex,
    PhysicsTestMotionQuality quality);
}
