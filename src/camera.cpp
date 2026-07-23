#include "camera.hpp"

#include "player.hpp"

#include <algorithm>
#include <cmath>

#include <GLFW/glfw3.h>
#include <glm/geometric.hpp>

namespace xrphoton
{
namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr float TwoPi = 2.0f * Pi;
constexpr float MouseSensitivity = 0.002f;
constexpr float PitchLimit = Pi * 89.0f / 180.0f;
constexpr float NormalizeEpsilonSquared = 1.0e-12f;
constexpr glm::vec3 WorldUp{0.0f, 1.0f, 0.0f};

struct CameraBasis
{
    glm::vec3 forward;
    glm::vec3 right;
    glm::vec3 up;
};

glm::vec3 normalizeOrZero(glm::vec3 value)
{
    const float squaredLength = glm::dot(value, value);

    if (squaredLength <= NormalizeEpsilonSquared) {
        return {};
    }

    return glm::normalize(value);
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

    // Preserve the bring-up shader's screen mapping: at yaw 0, world +X maps
    // screen-right and world +Y maps screen-up while the camera looks down +Z.
    const glm::vec3 forward{
        cosPitch * std::sin(yaw),
        std::sin(clampedPitch),
        cosPitch * std::cos(yaw),
    };
    const glm::vec3 right = normalizeOrZero(glm::cross(WorldUp, forward));
    const glm::vec3 up = glm::cross(forward, right);

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

CameraUpdate updateCamera(
    Camera* camera,
    CameraControls* controls,
    GLFWwindow* window,
    float dt,
    CameraMode mode)
{
    CameraUpdate update{};
    if (camera == nullptr || controls == nullptr || window == nullptr) {
        return update;
    }

    const bool modeToggleDown = keyPressed(window, GLFW_KEY_F1);
    update.toggleMode = controls->modeTogglePressed(modeToggleDown);

    const bool jumpDown = keyPressed(window, GLFW_KEY_SPACE);
    const bool jumpPressed = controls->jumpPressed(jumpDown);

    if (cursorCaptured(window) && keyPressed(window, GLFW_KEY_ESCAPE)) {
        setCursorCaptured(window, false);
        camera->cursorAnchorValid = false;
        return update;
    }

    if (!cursorCaptured(window)) {
        camera->cursorAnchorValid = false;

        if (mouseButtonPressed(window, GLFW_MOUSE_BUTTON_LEFT)) {
            setCursorCaptured(window, true);
        }

        return update;
    }

    update.jumpRequested = mode == CameraMode::Player && jumpPressed;

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

    const float forwardAxis =
        (keyPressed(window, GLFW_KEY_W) ? 1.0f : 0.0f)
        - (keyPressed(window, GLFW_KEY_S) ? 1.0f : 0.0f);
    const float rightAxis =
        (keyPressed(window, GLFW_KEY_D) ? 1.0f : 0.0f)
        - (keyPressed(window, GLFW_KEY_A) ? 1.0f : 0.0f);
    const bool sprint = keyPressed(window, GLFW_KEY_LEFT_SHIFT);
    const bool crouched = keyPressed(window, GLFW_KEY_LEFT_CONTROL);

    if (mode == CameraMode::Player) {
        update.crouched = crouched;
        update.playerVelocity = playerHorizontalVelocity(
            camera->yaw,
            forwardAxis,
            rightAxis,
            sprint,
            crouched);
        return update;
    }

    glm::vec3 movement{};
    movement += forwardAxis * basis.forward;
    movement += rightAxis * basis.right;
    if (keyPressed(window, GLFW_KEY_SPACE)) {
        movement += WorldUp;
    }
    if (crouched) {
        movement -= WorldUp;
    }

    if (glm::dot(movement, movement) <= NormalizeEpsilonSquared) {
        return update;
    }

    const float speed = PlayerRunSpeed
        * (sprint ? PlayerSprintMultiplier : 1.0f);

    camera->position += normalizeOrZero(movement) * speed * std::max(dt, 0.0f);
    return update;
}

CameraPushConstants makeCameraPushConstants(const Camera& camera, float aspect)
{
    const CameraBasis basis = makeCameraBasis(camera.yaw, camera.pitch);
    const float halfFovScale = std::tan(camera.verticalFov * 0.5f);

    return {
        .origin = camera.position,
        .forward = basis.forward,
        .right = basis.right * halfFovScale * aspect,
        .up = basis.up * halfFovScale,
    };
}
}
