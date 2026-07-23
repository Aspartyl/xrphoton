#pragma once

#include <cstddef>

#include <glm/vec3.hpp>

struct GLFWwindow;

namespace xrphoton
{
// 60 degrees in radians. Header-bound only because Camera's default member
// initializer needs it.
constexpr float DefaultVerticalFov = 1.0471976f;

// Persistent view state. main() owns separate values for the collision-aware player
// view and the collision-free camera; neither value owns an external resource.
struct Camera
{
    glm::vec3 position{0.0f, 0.0f, -2.0f};
    // yaw 0 / pitch 0 looks down world +Z, preserving the old startup view.
    float yaw = 0.0f;
    float pitch = 0.0f;
    float verticalFov = DefaultVerticalFov;

    double lastCursorX = 0.0;
    double lastCursorY = 0.0;
    bool cursorAnchorValid = false;
};

enum class CameraMode
{
    Player,
    Free,
};

[[nodiscard]] constexpr CameraMode toggledCameraMode(CameraMode mode)
{
    return mode == CameraMode::Player ? CameraMode::Free : CameraMode::Player;
}

// Start a free-camera session at the player's current eye position and view. The
// cursor anchor is deliberately discarded so the first free-camera frame cannot
// interpret a stale cursor delta as mouse movement.
inline void placeFreeCameraAtPlayerView(
    const Camera& playerCamera,
    Camera* freeCamera)
{
    if (freeCamera == nullptr) {
        return;
    }
    *freeCamera = playerCamera;
    freeCamera->cursorAnchorValid = false;
}

// Shared edge state for controls that switch between the two Camera values.
struct CameraControls
{
    bool modeToggleDown = false;
    bool jumpDown = false;

    [[nodiscard]] bool modeTogglePressed(bool down)
    {
        const bool pressed = down && !modeToggleDown;
        modeToggleDown = down;
        return pressed;
    }

    [[nodiscard]] bool jumpPressed(bool down)
    {
        const bool pressed = down && !jumpDown;
        jumpDown = down;
        return pressed;
    }
};

struct CameraUpdate
{
    glm::vec3 playerVelocity{};
    bool jumpRequested = false;
    bool crouched = false;
    bool toggleMode = false;
};

// The shader sees four float3 values at 16-byte offsets. The explicit pads pin the
// CPU payload to the same shape, and the offset asserts make that ABI visible.
struct CameraPushConstants
{
    glm::vec3 origin;  float pad0 = 0.0f;
    glm::vec3 forward; float pad1 = 0.0f;
    glm::vec3 right;   float pad2 = 0.0f;
    glm::vec3 up;      float pad3 = 0.0f;
};
static_assert(sizeof(CameraPushConstants) == 64,
    "must match the shader's push-constant block and stay within the 128-byte spec minimum");
static_assert(offsetof(CameraPushConstants, origin) == 0
    && offsetof(CameraPushConstants, forward) == 16
    && offsetof(CameraPushConstants, right) == 32
    && offsetof(CameraPushConstants, up) == 48,
    "field offsets are the shader ABI, not just the total size");

// Poll shared mouse/key input. Free mode directly advances the collision-free camera;
// player mode updates look state and returns a yaw-relative horizontal velocity for
// the fixed-step physics character. F1 is edge-detected through controls.
CameraUpdate updateCamera(
    Camera* camera,
    CameraControls* controls,
    GLFWwindow* window,
    float dt,
    CameraMode mode);

// Derive the frame's raygen payload from camera state and swapchain aspect ratio.
CameraPushConstants makeCameraPushConstants(const Camera& camera, float aspect);
}
