#pragma once

#include <cstddef>

struct GLFWwindow;

namespace xrphoton
{
// Minimal vector type for the camera basis and push-constant payload. Helper
// operations stay private to camera.cpp until a broader math module earns its keep.
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 60 degrees in radians. Header-bound only because Camera's default member
// initializer needs it.
constexpr float DefaultVerticalFov = 1.0471976f;

// Persistent fly-camera state, plus the cursor-tracking fields updateCamera needs
// across frames. A plain value struct owned by main(), not an RAII owner.
struct Camera
{
    Vec3 position{0.0f, 0.0f, -2.0f};
    // yaw 0 / pitch 0 looks down world +Z, preserving the old startup view.
    float yaw = 0.0f;
    float pitch = 0.0f;
    float verticalFov = DefaultVerticalFov;

    double lastCursorX = 0.0;
    double lastCursorY = 0.0;
    bool cursorAnchorValid = false;
};

// The shader sees four float3 values at 16-byte offsets. The explicit pads pin the
// CPU payload to the same shape, and the offset asserts make that ABI visible.
struct CameraPushConstants
{
    Vec3 origin;  float pad0 = 0.0f;
    Vec3 forward; float pad1 = 0.0f;
    Vec3 right;   float pad2 = 0.0f;
    Vec3 up;      float pad3 = 0.0f;
};
static_assert(sizeof(CameraPushConstants) == 64,
    "must match the shader's push-constant block and stay within the 128-byte spec minimum");
static_assert(offsetof(CameraPushConstants, forward) == 16
    && offsetof(CameraPushConstants, right) == 32
    && offsetof(CameraPushConstants, up) == 48,
    "field offsets are the shader ABI, not just the total size");

// Poll input and advance the camera by dt seconds. Escape releases the cursor,
// left click recaptures it, and the camera is frozen while the cursor is released.
void updateCamera(Camera* camera, GLFWwindow* window, float dt);

// Derive the frame's raygen payload from camera state and swapchain aspect ratio.
CameraPushConstants makeCameraPushConstants(const Camera& camera, float aspect);
}
