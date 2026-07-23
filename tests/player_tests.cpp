#include "camera.hpp"
#include "player.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>

#include <glm/geometric.hpp>

namespace
{
int failureCount = 0;

void expect(bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failureCount;
    }
}

bool nearly(float left, float right, float tolerance = 1.0e-5f)
{
    return std::abs(left - right) <= tolerance;
}
}

int main()
{
    xrphoton::CameraControls controls;
    expect(
        controls.modeTogglePressed(true),
        "initial F1 press toggles camera mode");
    expect(
        !controls.modeTogglePressed(true),
        "holding F1 does not repeatedly toggle camera mode");
    expect(
        !controls.modeTogglePressed(false)
            && controls.modeTogglePressed(true),
        "releasing and pressing F1 toggles again");
    expect(
        xrphoton::toggledCameraMode(xrphoton::CameraMode::Player)
                == xrphoton::CameraMode::Free
            && xrphoton::toggledCameraMode(xrphoton::CameraMode::Free)
                == xrphoton::CameraMode::Player,
        "camera mode switches in both directions");

    xrphoton::Camera playerCamera;
    xrphoton::Camera freeCamera;
    playerCamera.position = {1.0f, 2.0f, 3.0f};
    playerCamera.yaw = 0.25f;
    playerCamera.pitch = -0.5f;
    playerCamera.verticalFov = 0.75f;
    playerCamera.cursorAnchorValid = true;
    freeCamera.position = {-4.0f, 5.0f, -6.0f};
    freeCamera.yaw = -1.0f;
    xrphoton::placeFreeCameraAtPlayerView(playerCamera, &freeCamera);
    expect(
        freeCamera.position == playerCamera.position
            && freeCamera.yaw == playerCamera.yaw
            && freeCamera.pitch == playerCamera.pitch
            && freeCamera.verticalFov == playerCamera.verticalFov
            && !freeCamera.cursorAnchorValid,
        "free camera starts at the complete current player view");

    freeCamera.position = {100.0f, 200.0f, 300.0f};
    freeCamera.yaw = 2.0f;
    playerCamera.position = {7.0f, 8.0f, 9.0f};
    playerCamera.yaw = -0.75f;
    xrphoton::placeFreeCameraAtPlayerView(playerCamera, &freeCamera);
    expect(
        freeCamera.position == playerCamera.position
            && freeCamera.yaw == playerCamera.yaw,
        "each free-camera entry replaces its previous pose with the latest player view");

    const glm::vec3 forward =
        xrphoton::playerHorizontalVelocity(0.0f, 1.0f, 0.0f, false);
    expect(
        nearly(forward.x, 0.0f)
            && nearly(forward.y, 0.0f)
            && nearly(forward.z, xrphoton::PlayerRunSpeed),
        "yaw-zero forward input runs down world +Z");

    const glm::vec3 right =
        xrphoton::playerHorizontalVelocity(0.0f, 0.0f, 1.0f, false);
    expect(
        nearly(right.x, xrphoton::PlayerRunSpeed)
            && nearly(right.y, 0.0f)
            && nearly(right.z, 0.0f),
        "yaw-zero right input strafes down world +X");

    constexpr float HalfPi = 1.57079632679489661923f;
    const glm::vec3 turned =
        xrphoton::playerHorizontalVelocity(HalfPi, 1.0f, 0.0f, false);
    expect(
        nearly(turned.x, xrphoton::PlayerRunSpeed)
            && nearly(turned.z, 0.0f),
        "positive ninety-degree yaw turns forward toward world +X");

    const glm::vec3 diagonal =
        xrphoton::playerHorizontalVelocity(0.0f, 1.0f, 1.0f, false);
    expect(
        nearly(glm::length(diagonal), xrphoton::PlayerRunSpeed),
        "diagonal input is normalized to run speed");

    const glm::vec3 sprint =
        xrphoton::playerHorizontalVelocity(0.0f, 1.0f, 0.0f, true);
    expect(
        nearly(
            glm::length(sprint),
            xrphoton::PlayerRunSpeed * xrphoton::PlayerSprintMultiplier),
        "sprint applies the shared four-times speed multiplier");

    const glm::vec3 crouch =
        xrphoton::playerHorizontalVelocity(0.0f, 1.0f, 0.0f, true, true);
    expect(
        nearly(glm::length(crouch), xrphoton::PlayerCrouchSpeed),
        "crouching uses crouch speed even while sprint is held");
    expect(
        xrphoton::PlayerCrouchEyeHeight < xrphoton::PlayerEyeHeight,
        "crouching lowers the player viewpoint");

    expect(
        xrphoton::playerHorizontalVelocity(0.0f, 0.0f, 0.0f, false)
            == glm::vec3{},
        "zero input produces zero velocity");
    expect(
        xrphoton::playerHorizontalVelocity(
            std::numeric_limits<float>::quiet_NaN(),
            1.0f,
            0.0f,
            false) == glm::vec3{},
        "non-finite input cannot poison player velocity");

    if (failureCount != 0) {
        std::cerr << failureCount << " player test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "player tests passed\n";
    return 0;
}
