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

// Create the world's one invisible, capsule-shaped virtual character. Position is
// the character's feet/model origin in world space; the character is not a SceneData
// instance and therefore does not add render geometry or a TLAS instance.
[[nodiscard]] bool createPhysicsCharacter(
    PhysicsWorld* world,
    std::array<float, 3> position);

// Store movement intent for the next fixed physics steps. Horizontal velocity is
// world-space X/Z in m/s. Jump requests are latched until a fixed step consumes them,
// so a short input tap cannot be lost on a render frame that performs no physics step.
// Crouch is a held stance request; standing is deferred while overhead space is blocked.
[[nodiscard]] bool setPhysicsCharacterInput(
    PhysicsWorld* world,
    std::array<float, 2> horizontalVelocity,
    bool jumpRequested,
    bool crouched = false);

// Suspend/resume the virtual character independently of the rigid-body world. A
// suspended character is completely stationary; resuming refreshes contacts before
// the next fixed movement step.
[[nodiscard]] bool setPhysicsCharacterEnabled(
    PhysicsWorld* world,
    bool enabled);

// Return the character's feet/model-origin position. Logically read-only apart from
// the same terminal topology guard used by the other live-world queries.
[[nodiscard]] bool queryPhysicsCharacterPosition(
    const PhysicsWorld* world,
    std::array<float, 3>* position);

// Return the character's actual stance. It can remain crouched after the input is
// released when changing back to the standing capsule would intersect geometry.
[[nodiscard]] bool queryPhysicsCharacterCrouched(
    const PhysicsWorld* world,
    bool* crouched);

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
