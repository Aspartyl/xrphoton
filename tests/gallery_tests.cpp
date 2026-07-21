#include "gallery.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

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

bool matrixNear(
    const glm::mat4& left,
    const glm::mat4& right,
    float tolerance = 1.0e-5f)
{
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            if (!nearly(left[column][row], right[column][row], tolerance)) {
                return false;
            }
        }
    }
    return true;
}

glm::mat4 translation(glm::vec3 offset)
{
    return glm::translate(glm::mat4{1.0f}, offset);
}

glm::mat4 scaledPlacement(glm::vec3 offset, glm::vec3 scale)
{
    return translation(offset) * glm::scale(glm::mat4{1.0f}, scale);
}

glm::mat4 rotatedPlacement(glm::vec3 offset, float degrees)
{
    return translation(offset)
        * glm::rotate(
            glm::mat4{1.0f},
            glm::radians(degrees),
            glm::vec3{0.0f, 1.0f, 0.0f});
}

void testGeneratedYardPolicy()
{
    xrphoton::GalleryLoadResult loaded = xrphoton::loadGalleryScene();
    expect(static_cast<bool>(loaded), "generated-only gallery yard loads successfully");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }

    const xrphoton::SceneData& scene = loaded.scene;
    expect(scene.meshes.size() == 5, "yard loads five required models/meshes");
    expect(scene.geometries.size() == 6, "yard loads six required geometry ranges");
    expect(scene.instances.size() == 13, "yard produces thirteen required placements");
    expect(scene.materials.size() == 6, "yard retains six required materials");

    constexpr std::array<std::uint32_t, 5> expectedFirstGeometries{0, 1, 2, 3, 4};
    constexpr std::array<std::uint32_t, 5> expectedGeometryCounts{1, 1, 1, 1, 2};
    if (scene.meshes.size() == expectedFirstGeometries.size()) {
        for (std::size_t index = 0; index < scene.meshes.size(); ++index) {
            expect(
                scene.meshes[index].firstGeometry == expectedFirstGeometries[index]
                    && scene.meshes[index].geometryCount == expectedGeometryCounts[index],
                "required model order and geometry ranges stay pinned");
        }
    }

    const std::array expectedTransforms{
        glm::mat4{1.0f},
        translation({6.0f, -0.01f, 9.85f}),
        rotatedPlacement({9.84f, -0.01f, 5.71f}, 90.0f),
        scaledPlacement({5.0f, 0.49f, 5.0f}, {2.0f, 1.0f, 2.0f}),
        scaledPlacement({5.0f, 0.115f, 1.59f}, {1.92f, 0.25f, 0.7f}),
        scaledPlacement({5.0f, 0.24f, 2.28f}, {1.94f, 0.5f, 0.7f}),
        scaledPlacement({5.0f, 0.365f, 2.97f}, {1.96f, 0.75f, 0.7f}),
        scaledPlacement({5.0f, 0.49f, 3.66f}, {1.98f, 1.0f, 0.7f}),
        rotatedPlacement({-3.0f, 0.49f, 4.0f}, 30.0f),
        xrphoton::yardAnimatedTransform(0.0),
        translation({-6.0f, 1.0f, 9.5f}),
        translation({-4.25f, 1.0f, 9.35f}),
        translation({-2.1f, 1.0f, 9.0f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(30.0f),
                glm::vec3{0.0f, 1.0f, 0.0f})
            * glm::scale(glm::mat4{1.0f}, glm::vec3{1.5f, 1.0f, 1.5f}),
    };
    constexpr std::array<std::uint32_t, 13> expectedMeshes{
        0, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 4, 4,
    };

    if (scene.instances.size() == expectedTransforms.size()) {
        for (std::size_t index = 0; index < scene.instances.size(); ++index) {
            expect(
                scene.instances[index].meshIndex == expectedMeshes[index],
                "yard placement references its pinned shared mesh");
            expect(
                matrixNear(scene.instances[index].transform, expectedTransforms[index]),
                "yard placement retains its pinned world transform");
        }
    }

    expect(loaded.animatedInstance == 9, "dynamic crate has the pinned flat instance index");
    if (loaded.animatedInstance < scene.instances.size()) {
        expect(
            scene.instances[loaded.animatedInstance].meshIndex == 2,
            "dynamic instance references the shared yard-box mesh");
        expect(
            matrixNear(
                scene.instances[loaded.animatedInstance].transform,
                xrphoton::yardAnimatedTransform(0.0)),
            "dynamic instance starts at the animation's t=0 pose");
    }

    expect(
        nearly(loaded.spawn.position.x, -7.0f)
            && nearly(loaded.spawn.position.y, 1.7f)
            && nearly(loaded.spawn.position.z, -7.0f),
        "yard spawn position stays pinned");
    expect(nearly(loaded.spawn.yaw, glm::radians(45.0f)), "yard spawn yaw stays pinned");
    expect(nearly(loaded.spawn.pitch, glm::radians(-5.0f)), "yard spawn pitch stays pinned");
}

void testYardAnimation()
{
    const glm::mat4 atStart = xrphoton::yardAnimatedTransform(0.0);
    expect(
        matrixNear(atStart, translation({3.0f, 0.9f, 0.0f})),
        "yard animation starts at the static-phase dynamic-crate pose");

    const glm::mat4 atOneSecond = xrphoton::yardAnimatedTransform(1.0);
    expect(
        nearly(atOneSecond[3][0], 3.0f * std::cos(0.6f))
            && nearly(atOneSecond[3][1], 0.9f)
            && nearly(atOneSecond[3][2], -3.0f * std::sin(0.6f)),
        "yard animation pins its orbit speed and counterclockwise direction");
    expect(
        matrixNear(
            glm::mat4{glm::mat3{atOneSecond}},
            glm::rotate(
                glm::mat4{1.0f},
                1.7f,
                glm::vec3{0.0f, 1.0f, 0.0f})),
        "yard animation pins its independent spin speed");

    constexpr std::array sampleTimes{
        -2.0,
        0.125,
        1.0,
        3.5,
        25.0,
        1.0e12,
    };
    for (double seconds : sampleTimes) {
        const glm::mat4 transform = xrphoton::yardAnimatedTransform(seconds);
        bool finite = true;
        for (std::size_t column = 0; column < 4; ++column) {
            for (std::size_t row = 0; row < 4; ++row) {
                finite = finite && std::isfinite(transform[column][row]);
            }
        }
        expect(finite, "yard animation remains finite at representative times");

        const float radius = std::sqrt(
            transform[3][0] * transform[3][0]
            + transform[3][2] * transform[3][2]);
        expect(
            nearly(radius, 3.0f) && nearly(transform[3][1], 0.9f),
            "yard animation stays on its radius-three orbit at fixed height");

        const glm::mat3 linear{transform};
        bool orthonormal = true;
        for (std::size_t column = 0; column < 3; ++column) {
            orthonormal = orthonormal
                && nearly(glm::dot(linear[column], linear[column]), 1.0f);
            for (std::size_t other = column + 1; other < 3; ++other) {
                orthonormal = orthonormal
                    && nearly(glm::dot(linear[column], linear[other]), 0.0f);
            }
        }
        expect(orthonormal, "yard animation's linear block remains orthonormal");
        expect(
            nearly(glm::determinant(linear), 1.0f),
            "yard animation remains a proper rotation with determinant +1");
    }
}
}

int main()
{
    testGeneratedYardPolicy();
    testYardAnimation();

    if (failureCount != 0) {
        std::cerr << failureCount << " gallery-policy test assertion(s) failed.\n";
        return 1;
    }
    return 0;
}
