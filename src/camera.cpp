#include "camera.hpp"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>

namespace xrphoton
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr float TwoPi = 2.0f * Pi;
constexpr float MoveSpeed = 3.0f;
constexpr float SprintMultiplier = 4.0f;
constexpr float MouseSensitivity = 0.002f;
constexpr float PitchLimit = Pi * 89.0f / 180.0f;
constexpr float NormalizeEpsilonSquared = 1.0e-12f;
constexpr Vec3 WorldUp{0.0f, 1.0f, 0.0f};

struct CameraBasis
{
    Vec3 forward;
    Vec3 right;
    Vec3 up;
};

Vec3 add(Vec3 lhs, Vec3 rhs)
{
    return {
        lhs.x + rhs.x,
        lhs.y + rhs.y,
        lhs.z + rhs.z,
    };
}

Vec3 subtract(Vec3 lhs, Vec3 rhs)
{
    return {
        lhs.x - rhs.x,
        lhs.y - rhs.y,
        lhs.z - rhs.z,
    };
}

Vec3 scale(Vec3 value, float scalar)
{
    return {
        value.x * scalar,
        value.y * scalar,
        value.z * scalar,
    };
}

float lengthSquared(Vec3 value)
{
    return value.x * value.x + value.y * value.y + value.z * value.z;
}

Vec3 cross(Vec3 lhs, Vec3 rhs)
{
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

Vec3 normalize(Vec3 value)
{
    const float squaredLength = lengthSquared(value);

    if (squaredLength <= NormalizeEpsilonSquared) {
        return {};
    }

    return scale(value, 1.0f / std::sqrt(squaredLength));
}

float wrapYaw(float yaw)
{
    float wrapped = std::fmod(yaw + Pi, TwoPi);

    if (wrapped <= 0.0f) {
        wrapped += TwoPi;
    }

    return wrapped - Pi;
}

CameraBasis makeCameraBasis(float yaw, float pitch)
{
    const float clampedPitch = std::clamp(pitch, -PitchLimit, PitchLimit);
    const float cosPitch = std::cos(clampedPitch);
    const Vec3 forward{
        cosPitch * std::sin(yaw),
        std::sin(clampedPitch),
        cosPitch * std::cos(yaw),
    };
    const Vec3 right = normalize(cross(WorldUp, forward));
    const Vec3 up = cross(forward, right);

    return {forward, right, up};
}

bool keyPressed(GLFWwindow* window, int key)
{
    return glfwGetKey(window, key) == GLFW_PRESS;
}

bool mouseButtonPressed(GLFWwindow* window, int button)
{
    return glfwGetMouseButton(window, button) == GLFW_PRESS;
}

bool cursorCaptured(GLFWwindow* window)
{
    return glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
}

void setCursorCaptured(GLFWwindow* window, bool captured)
{
    glfwSetInputMode(
        window,
        GLFW_CURSOR,
        captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);

    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        glfwSetInputMode(
            window,
            GLFW_RAW_MOUSE_MOTION,
            captured ? GLFW_TRUE : GLFW_FALSE);
    }
}

} // namespace

void updateCamera(Camera* camera, GLFWwindow* window, float dt)
{
    if (camera == nullptr || window == nullptr) {
        return;
    }

    if (cursorCaptured(window) && keyPressed(window, GLFW_KEY_ESCAPE)) {
        setCursorCaptured(window, false);
        camera->cursorAnchorValid = false;
        return;
    }

    if (!cursorCaptured(window)) {
        camera->cursorAnchorValid = false;

        if (mouseButtonPressed(window, GLFW_MOUSE_BUTTON_LEFT)) {
            setCursorCaptured(window, true);
        }

        return;
    }

    double cursorX = 0.0;
    double cursorY = 0.0;
    glfwGetCursorPos(window, &cursorX, &cursorY);

    if (!camera->cursorAnchorValid) {
        camera->lastCursorX = cursorX;
        camera->lastCursorY = cursorY;
        camera->cursorAnchorValid = true;
    } else {
        const double deltaX = cursorX - camera->lastCursorX;
        const double deltaY = cursorY - camera->lastCursorY;

        camera->yaw = wrapYaw(
            camera->yaw + static_cast<float>(deltaX) * MouseSensitivity);
        camera->pitch = std::clamp(
            camera->pitch - static_cast<float>(deltaY) * MouseSensitivity,
            -PitchLimit,
            PitchLimit);

        camera->lastCursorX = cursorX;
        camera->lastCursorY = cursorY;
    }

    const CameraBasis basis = makeCameraBasis(camera->yaw, camera->pitch);

    Vec3 movement{};

    if (keyPressed(window, GLFW_KEY_W)) {
        movement = add(movement, basis.forward);
    }
    if (keyPressed(window, GLFW_KEY_S)) {
        movement = subtract(movement, basis.forward);
    }
    if (keyPressed(window, GLFW_KEY_A)) {
        movement = subtract(movement, basis.right);
    }
    if (keyPressed(window, GLFW_KEY_D)) {
        movement = add(movement, basis.right);
    }
    if (keyPressed(window, GLFW_KEY_SPACE)) {
        movement = add(movement, WorldUp);
    }
    if (keyPressed(window, GLFW_KEY_LEFT_CONTROL)) {
        movement = subtract(movement, WorldUp);
    }

    if (lengthSquared(movement) <= NormalizeEpsilonSquared) {
        return;
    }

    float speed = MoveSpeed;

    if (keyPressed(window, GLFW_KEY_LEFT_SHIFT)) {
        speed *= SprintMultiplier;
    }

    const Vec3 delta = scale(normalize(movement), speed * std::max(dt, 0.0f));
    camera->position = add(camera->position, delta);
}

CameraPushConstants makeCameraPushConstants(const Camera& camera, float aspect)
{
    const CameraBasis basis = makeCameraBasis(camera.yaw, camera.pitch);
    const float halfFovScale = std::tan(camera.verticalFov * 0.5f);

    return {
        .origin = camera.position,
        .forward = basis.forward,
        .right = scale(basis.right, halfFovScale * aspect),
        .up = scale(basis.up, halfFovScale),
    };
}
}
