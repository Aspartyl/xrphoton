#include "gallery.hpp"
#include "scene_lighting.hpp"

#ifndef XRPHOTON_GALLERY_TEST_EXPECTATION
#define XRPHOTON_GALLERY_TEST_EXPECTATION 0
#endif

#if XRPHOTON_GALLERY_TEST_EXPECTATION == 3
#include "ogfx.hpp"
#endif

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#if XRPHOTON_GALLERY_TEST_EXPECTATION == 3
#include <filesystem>
#include <fstream>
#endif
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#if XRPHOTON_GALLERY_TEST_EXPECTATION == 3
#include <utility>
#endif
#include <vector>

#include <glm/geometric.hpp>
#include <glm/common.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

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

struct WorldBounds
{
    glm::vec3 minimum{std::numeric_limits<float>::max()};
    glm::vec3 maximum{std::numeric_limits<float>::lowest()};
    bool valid = false;
};

bool instanceWorldBounds(
    const xrphoton::SceneData& scene,
    std::size_t instanceIndex,
    WorldBounds* bounds)
{
    if (bounds == nullptr || instanceIndex >= scene.instances.size()) {
        return false;
    }
    *bounds = {};
    const xrphoton::SceneInstance& instance = scene.instances[instanceIndex];
    if (instance.meshIndex >= scene.meshes.size()) {
        return false;
    }
    const xrphoton::SceneMesh& mesh = scene.meshes[instance.meshIndex];
    for (std::uint64_t geometryOffset = 0;
         geometryOffset < mesh.geometryCount;
         ++geometryOffset) {
        const std::uint64_t geometryIndex =
            static_cast<std::uint64_t>(mesh.firstGeometry) + geometryOffset;
        if (geometryIndex >= scene.geometries.size()) {
            return false;
        }
        const xrphoton::SceneGeometry& geometry =
            scene.geometries[static_cast<std::size_t>(geometryIndex)];
        for (std::uint64_t vertexOffset = 0;
             vertexOffset < geometry.vertexCount;
             ++vertexOffset) {
            const std::uint64_t vertex =
                static_cast<std::uint64_t>(geometry.firstVertex) + vertexOffset;
            const std::uint64_t scalar = vertex * 3;
            if (scalar + 2 >= scene.positions.size()) {
                return false;
            }
            const glm::vec4 world = instance.transform * glm::vec4{
                scene.positions[static_cast<std::size_t>(scalar)],
                scene.positions[static_cast<std::size_t>(scalar + 1)],
                scene.positions[static_cast<std::size_t>(scalar + 2)],
                1.0f,
            };
            const glm::vec3 position{world};
            bounds->minimum = glm::min(bounds->minimum, position);
            bounds->maximum = glm::max(bounds->maximum, position);
            bounds->valid = true;
        }
    }
    return bounds->valid;
}

bool boundsOverlap(const WorldBounds& left, const WorldBounds& right)
{
    return left.minimum.x < right.maximum.x && left.maximum.x > right.minimum.x
        && left.minimum.y < right.maximum.y && left.maximum.y > right.minimum.y
        && left.minimum.z < right.maximum.z && left.maximum.z > right.minimum.z;
}

bool instanceClashes(const xrphoton::SceneData& scene, std::size_t instanceIndex)
{
    WorldBounds target;
    if (!instanceWorldBounds(scene, instanceIndex, &target)) {
        return true;
    }
    for (std::size_t otherIndex = 0;
         otherIndex < scene.instances.size();
         ++otherIndex) {
        if (otherIndex == instanceIndex) {
            continue;
        }
        WorldBounds other;
        if (!instanceWorldBounds(scene, otherIndex, &other)
            || boundsOverlap(target, other)) {
            return true;
        }
    }
    return false;
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
    constexpr std::size_t ExpectedMeshCount = 10;
    constexpr std::size_t ExpectedGeometryCount = 11;
    constexpr std::size_t ExpectedInstanceCount = 18;
    constexpr std::size_t ExpectedMaterialCount = 11;
    constexpr std::size_t ExpectedPhysicsCount = 2;
#else
    constexpr std::size_t ExpectedMeshCount = 9;
    constexpr std::size_t ExpectedGeometryCount = 10;
    constexpr std::size_t ExpectedInstanceCount = 17;
    constexpr std::size_t ExpectedMaterialCount = 10;
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
    constexpr std::array<float, 4> expectedGlassRoughness{0.02f, 0.10f, 0.22f, 0.55f};
    constexpr std::array<std::array<float, 3>, 4> expectedGlassTints{{
        {0.96f, 0.99f, 1.00f},
        {0.55f, 0.82f, 1.00f},
        {1.00f, 0.68f, 0.32f},
        {0.55f, 0.58f, 0.62f},
    }};
    bool glassMaterialsMatch = xrphoton::sceneHasGlass(scene);
    for (std::size_t offset = 0; offset < expectedGlassRoughness.size(); ++offset) {
        const std::size_t materialIndex = 3 + offset;
        const std::size_t geometryIndex = 3 + offset;
        glassMaterialsMatch = glassMaterialsMatch
            && scene.materials[materialIndex].materialClass
                == xrphoton::SceneMaterialClass::Glass
            && nearly(
                scene.materials[materialIndex].perceptualRoughness,
                expectedGlassRoughness[offset])
            && nearly(
                scene.materials[materialIndex].baseColorFactor[0],
                expectedGlassTints[offset][0])
            && nearly(
                scene.materials[materialIndex].baseColorFactor[1],
                expectedGlassTints[offset][1])
            && nearly(
                scene.materials[materialIndex].baseColorFactor[2],
                expectedGlassTints[offset][2])
            && xrphoton::geometryRequiresAnyHit(
                scene.geometries[geometryIndex], scene.materials[materialIndex])
            && !scene.geometries[geometryIndex].alphaTested;
    }
    expect(
        glassMaterialsMatch,
        "day yard retains four Glass roughness variants with non-alpha any-hit routing");

    bool glassPlacementsMatch = scene.instances.size() >= 14;
    for (std::size_t offset = 0; offset < 4 && glassPlacementsMatch; ++offset) {
        const std::size_t instanceIndex = 10 + offset;
        WorldBounds bounds;
        glassPlacementsMatch = scene.instances[instanceIndex].meshIndex == 3 + offset
            && matrixNear(
                scene.instances[instanceIndex].transform,
                translation({-2.5f, 0.0f, -3.8f}))
            && instanceWorldBounds(scene, instanceIndex, &bounds)
            && nearly(bounds.minimum.y, 0.0f)
            && nearly(bounds.maximum.y, 2.6f)
            && !instanceClashes(scene, instanceIndex);
    }
    expect(
        glassPlacementsMatch,
        "day Glass showcase stands on the ground without intersecting another object");

    constexpr std::array<std::uint32_t, 9> expectedFirstGeometries{
        0, 1, 2, 3, 4, 5, 6, 7, 8};
    constexpr std::array<std::uint32_t, 9> expectedGeometryCounts{
        1, 1, 1, 1, 1, 1, 1, 1, 2};
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
        translation({-2.5f, 0.0f, -3.8f}),
        translation({-2.5f, 0.0f, -3.8f}),
        translation({-2.5f, 0.0f, -3.8f}),
        translation({-2.5f, 0.0f, -3.8f}),
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
        0, 1, 1, 2, 2, 2, 2, 2, 2, 2, 3, 4, 5, 6, 7, 8, 8,
    };
#if XRPHOTON_GALLERY_TEST_EXPECTATION == 1
    expectedTransforms.push_back(dynamicBarrelSpawn());
    expectedMeshes.push_back(9);
#elif XRPHOTON_GALLERY_TEST_EXPECTATION == 4
    expectedTransforms.push_back(dynamicTailSpawn());
    expectedMeshes.push_back(9);
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
    expectedDynamicInstances.push_back(17);
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

void testNightYardPolicy()
{
#if XRPHOTON_GALLERY_TEST_EXPECTATION == 0
    xrphoton::GalleryLoadResult loaded = xrphoton::loadGalleryScene(
        xrphoton::ScenePreset::Night);
    expect(static_cast<bool>(loaded), "night yard loads successfully");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }
    bool nightGlassIsClear = xrphoton::sceneHasGlass(loaded.scene);
    for (std::size_t instanceIndex = 10;
         instanceIndex < 14 && nightGlassIsClear;
         ++instanceIndex) {
        nightGlassIsClear = !instanceClashes(loaded.scene, instanceIndex);
    }
    expect(
        loaded.scene.meshes.size() == 12
            && loaded.scene.geometries.size() == 13
            && loaded.scene.materials.size() == 13
            && loaded.scene.instances.size() == 38
            && nightGlassIsClear,
        "night yard retains every non-overlapping Glass showcase panel and adds its emitter set");
    xrphoton::SceneLighting lighting = xrphoton::makeSceneLightingPreset(
        xrphoton::ScenePreset::Night);
    std::string error;
    expect(
        xrphoton::buildSceneLighting(
            loaded.scene,
            loaded.dynamicInstances,
            &lighting,
            &error),
        "night yard emitter tables build");
    xrphoton::FrameLighting packed;
    expect(
        error.empty() && lighting.lights.size() == 42
            && lighting.instanceCount == 38
            && xrphoton::makeFrameLighting(lighting, 38, &packed)
            && packed.sunIrradiance == glm::vec3{}
            && packed.lightCount == 42
            && packed.pSky == 0.5f && packed.pEmitters == 0.5f,
        "night lighting disables the sun and publishes forty-two emitter triangles");
#endif
}

}

int main()
{
    testGeneratedYardPolicy();
    testNightYardPolicy();

    if (failureCount != 0) {
        std::cerr << failureCount << " gallery-policy test assertion(s) failed.\n";
        return 1;
    }
    return 0;
}
