#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace xrphoton
{
struct SceneData;

// The renderer and headless tests both advance this one fixed-rate simulation.
inline constexpr float PhysicsFixedDt = 1.0f / 60.0f;
inline constexpr float PhysicsMaxFrameDt = 0.1f;

struct PhysicsWorld
{
    // Defined in physics.cpp so no Jolt type enters this header graph.
    struct State;
    State* state = nullptr;

    PhysicsWorld() = default;
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    ~PhysicsWorld() noexcept;
};

// Bind a scene and transactionally create one body per instance. The dynamic
// indices must be in range and unique; every other instance becomes static.
[[nodiscard]] bool createPhysicsWorld(
    PhysicsWorld* world,
    SceneData* scene,
    std::span<const std::size_t> dynamicInstances);

// Advance the bound scene at PhysicsFixedDt and atomically publish all dynamic
// model-origin transforms after every required Jolt update succeeds.
[[nodiscard]] bool stepPhysics(
    PhysicsWorld* world,
    float frameDt);

// Backend-neutral runtime control and inspection; instanceIndex must identify
// one of the dynamic instances supplied when the world was created. Finite
// velocity magnitudes above the world's 500 m/s cap are clamped robustly.
[[nodiscard]] bool setPhysicsBodyLinearVelocity(
    PhysicsWorld* world,
    std::size_t instanceIndex,
    std::array<float, 3> velocity);

// Logically read-only for a valid world; detecting forbidden bound-scene
// topology drift still trips the world's permanent terminal-failure guard.
[[nodiscard]] bool queryPhysicsBodyActive(
    const PhysicsWorld* world,
    std::size_t instanceIndex,
    bool* active);
}
