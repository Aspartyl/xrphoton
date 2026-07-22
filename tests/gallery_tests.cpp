#include "gallery.hpp"
#include "ogfx.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#ifndef XRPHOTON_GALLERY_TEST_EXPECTATION
#define XRPHOTON_GALLERY_TEST_EXPECTATION 0
#endif

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

glm::mat4 dynamicCrateSpawn()
{
    return translation({3.0f, 2.5f, 0.0f})
        * glm::rotate(
            glm::mat4{1.0f},
            glm::radians(25.0f),
            glm::vec3{0.0f, 1.0f, 0.0f})
        * glm::rotate(
            glm::mat4{1.0f},
            glm::radians(12.0f),
            glm::vec3{1.0f, 0.0f, 0.0f});
}

#if XRPHOTON_GALLERY_TEST_EXPECTATION == 1
glm::mat4 dynamicBarrelSpawn()
{
    return translation({4.2f, 0.6f, 9.2f})
        * glm::rotate(
            glm::mat4{1.0f},
            glm::radians(20.0f),
            glm::vec3{0.0f, 0.0f, 1.0f});
}
#endif

#if XRPHOTON_GALLERY_TEST_EXPECTATION == 4
glm::mat4 dynamicTailSpawn()
{
    return translation({5.0f, 1.26821f, 5.0f})
        * glm::rotate(
            glm::mat4{1.0f},
            glm::radians(90.0f),
            glm::vec3{1.0f, 0.0f, 0.0f});
}
#endif

#if XRPHOTON_GALLERY_TEST_EXPECTATION == 3
bool writeMultiMeshPhysicsFixture()
{
    using namespace xrphoton::ogfx;

    Model model{};
    model.positions = {
        {-1.0f, 0.0f, 0.0f},
        { 0.0f, 1.0f, 0.0f},
        { 1.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 1.0f},
        { 0.0f, 1.0f, 1.0f},
        { 1.0f, 0.0f, 1.0f},
    };
    model.attributes.assign(
        model.positions.size(),
        VertexAttributes{0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
    model.indices = {0, 2, 1, 0, 2, 1};
    model.geometries = {
        Geometry{0, 3, 0, 3, 0, false},
        Geometry{3, 3, 3, 3, 0, false},
    };
    model.meshes = {Mesh{0, 1}, Mesh{1, 1}};
    model.materials.emplace_back();
    model.physicsBodies.push_back({
        .firstCollider = 0,
        .colliderCount = 1,
        .mass = 10.0f,
        .centerOfMass = {},
    });
    PhysicsCollider collider{};
    collider.shapeType = PhysicsShapeType::Box;
    collider.halfExtents = {0.5f, 0.5f, 0.5f};
    collider.mass = 10.0f;
    model.physicsColliders.push_back(std::move(collider));

    const std::filesystem::path path{XRPHOTON_GALLERY_BARREL_OGFX};
    const SerializeResult serialized = serializeModel(model, path.string());
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return false;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) {
        std::cerr << "Failed to create gallery fixture directory: "
                  << directoryError.message() << '\n';
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(serialized.bytes.data()),
        static_cast<std::streamsize>(serialized.bytes.size()));
    output.close();
    return static_cast<bool>(output);
}
#endif

void testGeneratedYardPolicy()
{
#if XRPHOTON_GALLERY_TEST_EXPECTATION == 3
    expect(writeMultiMeshPhysicsFixture(), "multi-mesh physics fixture is generated");
    if (failureCount != 0) {
        return;
    }
#endif

    xrphoton::GalleryLoadResult loaded = xrphoton::loadGalleryScene();

#if XRPHOTON_GALLERY_TEST_EXPECTATION == 2
    expect(!loaded, "a recipe-less optional dynamic placement is rejected");
    expect(
        loaded.error.find("is dynamic") != std::string::npos
            && loaded.error.find("0 physics bodies") != std::string::npos,
        "recipe-less dynamic rejection names the policy violation");
    return;
#elif XRPHOTON_GALLERY_TEST_EXPECTATION == 3
    expect(!loaded, "a multi-mesh physics-carrying dynamic asset is rejected");
    expect(
        loaded.error.find("bochka_close_1") != std::string::npos
            && loaded.error.find("rigid-physics mesh ownership") != std::string::npos
            && loaded.error.find("exactly 1 mesh") != std::string::npos,
        "multi-mesh dynamic rejection identifies the configured gallery entry");
    return;
#endif

    expect(static_cast<bool>(loaded), "gallery yard loads successfully");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }

    const xrphoton::SceneData& scene = loaded.scene;
#if XRPHOTON_GALLERY_TEST_EXPECTATION == 1 \
    || XRPHOTON_GALLERY_TEST_EXPECTATION == 4
    constexpr std::size_t ExpectedMeshCount = 6;
    constexpr std::size_t ExpectedGeometryCount = 7;
    constexpr std::size_t ExpectedInstanceCount = 14;
    constexpr std::size_t ExpectedMaterialCount = 7;
    constexpr std::size_t ExpectedPhysicsCount = 2;
#else
    constexpr std::size_t ExpectedMeshCount = 5;
    constexpr std::size_t ExpectedGeometryCount = 6;
    constexpr std::size_t ExpectedInstanceCount = 13;
    constexpr std::size_t ExpectedMaterialCount = 6;
    constexpr std::size_t ExpectedPhysicsCount = 1;
#endif
    expect(scene.meshes.size() == ExpectedMeshCount, "yard loads the expected model set");
    expect(
        scene.geometries.size() == ExpectedGeometryCount,
        "yard loads the expected geometry ranges");
    expect(
        scene.instances.size() == ExpectedInstanceCount,
        "yard produces the expected placements");
    expect(
        scene.materials.size() == ExpectedMaterialCount,
        "yard retains the expected materials");
    expect(
        scene.physicsBodies.size() == ExpectedPhysicsCount
            && scene.physicsColliders.size() == ExpectedPhysicsCount,
        "yard retains one complete rigid recipe per loaded dynamic model");

    constexpr std::array<std::uint32_t, 5> expectedFirstGeometries{0, 1, 2, 3, 4};
    constexpr std::array<std::uint32_t, 5> expectedGeometryCounts{1, 1, 1, 1, 2};
    if (scene.meshes.size() >= expectedFirstGeometries.size()) {
        for (std::size_t index = 0; index < expectedFirstGeometries.size(); ++index) {
            expect(
                scene.meshes[index].firstGeometry == expectedFirstGeometries[index]
                    && scene.meshes[index].geometryCount == expectedGeometryCounts[index],
                "required model order and geometry ranges stay pinned");
        }
    }

    std::vector<glm::mat4> expectedTransforms{
        glm::mat4{1.0f},
        translation({6.0f, -0.01f, 9.85f}),
        rotatedPlacement({9.84f, -0.01f, 5.71f}, 90.0f),
        scaledPlacement({5.0f, 0.49f, 5.0f}, {2.0f, 1.0f, 2.0f}),
        scaledPlacement({5.0f, 0.115f, 1.59f}, {1.92f, 0.25f, 0.7f}),
        scaledPlacement({5.0f, 0.24f, 2.28f}, {1.94f, 0.5f, 0.7f}),
        scaledPlacement({5.0f, 0.365f, 2.97f}, {1.96f, 0.75f, 0.7f}),
        scaledPlacement({5.0f, 0.49f, 3.66f}, {1.98f, 1.0f, 0.7f}),
        rotatedPlacement({-3.0f, 0.49f, 4.0f}, 30.0f),
        dynamicCrateSpawn(),
        translation({-6.0f, 1.0f, 9.5f}),
        translation({-4.25f, 1.0f, 9.35f}),
        translation({-2.1f, 1.0f, 9.0f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(30.0f),
                glm::vec3{0.0f, 1.0f, 0.0f})
            * glm::scale(glm::mat4{1.0f}, glm::vec3{1.5f, 1.0f, 1.5f}),
    };
    std::vector<std::uint32_t> expectedMeshes{
        0, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 4, 4,
    };
#if XRPHOTON_GALLERY_TEST_EXPECTATION == 1
    expectedTransforms.push_back(dynamicBarrelSpawn());
    expectedMeshes.push_back(5);
#elif XRPHOTON_GALLERY_TEST_EXPECTATION == 4
    expectedTransforms.push_back(dynamicTailSpawn());
    expectedMeshes.push_back(5);
#endif

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

    std::vector<std::size_t> expectedDynamicInstances{9};
#if XRPHOTON_GALLERY_TEST_EXPECTATION == 1 \
    || XRPHOTON_GALLERY_TEST_EXPECTATION == 4
    expectedDynamicInstances.push_back(13);
#endif
    expect(
        loaded.dynamicInstances == expectedDynamicInstances,
        "dynamic placements retain their pinned flat instance order");
    if (!loaded.dynamicInstances.empty()
        && loaded.dynamicInstances[0] < scene.instances.size()) {
        const std::size_t crateInstance = loaded.dynamicInstances[0];
        expect(
            scene.instances[crateInstance].meshIndex == 2,
            "dynamic instance references the shared yard-box mesh");
        expect(
            matrixNear(scene.instances[crateInstance].transform, dynamicCrateSpawn()),
            "dynamic crate starts at its pitched falling-body spawn");
    }

    if (!scene.physicsBodies.empty() && !scene.physicsColliders.empty()) {
        const xrphoton::ScenePhysicsBody& body = scene.physicsBodies[0];
        const xrphoton::ScenePhysicsCollider& collider = scene.physicsColliders[0];
        expect(
            body.meshIndex == 2 && body.firstCollider == 0
                && body.colliderCount == 1 && body.mass == 30.0f
                && body.centerOfMass == glm::vec3{0.0f},
            "generated crate body recipe is bound to the shared yard-box mesh");
        expect(
            collider.shape == xrphoton::ScenePhysicsShape::Box
                && collider.center == glm::vec3{0.0f}
                && collider.axis == glm::vec3{0.0f, 1.0f, 0.0f}
                && collider.height == 0.0f
                && collider.radius == 0.0f
                && collider.orientation == glm::quat{1.0f, 0.0f, 0.0f, 0.0f}
                && collider.halfExtents == glm::vec3{0.5f}
                && collider.mass == 30.0f
                && collider.centerOfMass == glm::vec3{0.0f}
                && collider.material.empty(),
            "generated crate collider recipe reaches gallery SceneData exactly");
    }

    expect(
        nearly(loaded.spawn.position.x, -7.0f)
            && nearly(loaded.spawn.position.y, 1.7f)
            && nearly(loaded.spawn.position.z, -7.0f),
        "yard spawn position stays pinned");
    expect(nearly(loaded.spawn.yaw, glm::radians(45.0f)), "yard spawn yaw stays pinned");
    expect(nearly(loaded.spawn.pitch, glm::radians(-5.0f)), "yard spawn pitch stays pinned");
}
}

int main()
{
    testGeneratedYardPolicy();

    if (failureCount != 0) {
        std::cerr << failureCount << " gallery-policy test assertion(s) failed.\n";
        return 1;
    }
    return 0;
}
