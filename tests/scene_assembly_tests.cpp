#include "ogfx.hpp"
#include "ogfx_loader.hpp"
#include "scene_assembly.hpp"
#include "scene_assembly_detail.hpp"
#include "ray_types.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

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

bool contains(std::string_view text, std::string_view fragment)
{
    return text.find(fragment) != std::string_view::npos;
}

bool sameAttribute(
    const xrphoton::VertexAttributes& left,
    const xrphoton::VertexAttributes& right)
{
    return left.nx == right.nx
        && left.ny == right.ny
        && left.nz == right.nz
        && left.u == right.u
        && left.v == right.v;
}

bool sameGeometry(
    const xrphoton::SceneGeometry& left,
    const xrphoton::SceneGeometry& right)
{
    return left.firstVertex == right.firstVertex
        && left.vertexCount == right.vertexCount
        && left.firstIndex == right.firstIndex
        && left.indexCount == right.indexCount
        && left.materialIndex == right.materialIndex
        && left.alphaTested == right.alphaTested;
}

bool sameMesh(const xrphoton::SceneMesh& left, const xrphoton::SceneMesh& right)
{
    return left.firstGeometry == right.firstGeometry
        && left.geometryCount == right.geometryCount;
}

bool sameVec3(const glm::vec3& left, const glm::vec3& right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool samePhysicsBody(
    const xrphoton::ScenePhysicsBody& left,
    const xrphoton::ScenePhysicsBody& right)
{
    return left.meshIndex == right.meshIndex
        && left.firstCollider == right.firstCollider
        && left.colliderCount == right.colliderCount
        && left.mass == right.mass
        && sameVec3(left.centerOfMass, right.centerOfMass);
}

bool samePhysicsCollider(
    const xrphoton::ScenePhysicsCollider& left,
    const xrphoton::ScenePhysicsCollider& right)
{
    return left.shape == right.shape
        && sameVec3(left.center, right.center)
        && sameVec3(left.axis, right.axis)
        && left.height == right.height
        && left.radius == right.radius
        && left.orientation.w == right.orientation.w
        && left.orientation.x == right.orientation.x
        && left.orientation.y == right.orientation.y
        && left.orientation.z == right.orientation.z
        && sameVec3(left.halfExtents, right.halfExtents)
        && left.mass == right.mass
        && sameVec3(left.centerOfMass, right.centerOfMass)
        && left.material == right.material;
}

bool sameMatrix(const glm::mat4& left, const glm::mat4& right)
{
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            if (left[column][row] != right[column][row]) {
                return false;
            }
        }
    }
    return true;
}

bool sameInstance(
    const xrphoton::SceneInstance& left,
    const xrphoton::SceneInstance& right)
{
    return left.meshIndex == right.meshIndex
        && sameMatrix(left.transform, right.transform);
}

bool sameMaterial(
    const xrphoton::SceneMaterial& left,
    const xrphoton::SceneMaterial& right)
{
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (left.baseColorFactor[channel] != right.baseColorFactor[channel]) {
            return false;
        }
    }
    return left.baseColorImage == right.baseColorImage
        && left.alphaCutoff == right.alphaCutoff
        && left.baseColorTexture == right.baseColorTexture;
}

bool sameImage(const xrphoton::SceneImage& left, const xrphoton::SceneImage& right)
{
    return left.format == right.format
        && left.width == right.width
        && left.height == right.height
        && left.pixels == right.pixels;
}

template<typename T, typename Compare>
bool sameRecords(
    const std::vector<T>& left,
    const std::vector<T>& right,
    Compare compare)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!compare(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

bool sameScene(const xrphoton::SceneData& left, const xrphoton::SceneData& right)
{
    return left.positions == right.positions
        && sameRecords(left.attributes, right.attributes, sameAttribute)
        && left.indices == right.indices
        && sameRecords(left.geometries, right.geometries, sameGeometry)
        && sameRecords(left.meshes, right.meshes, sameMesh)
        && sameRecords(left.physicsBodies, right.physicsBodies, samePhysicsBody)
        && sameRecords(
            left.physicsColliders,
            right.physicsColliders,
            samePhysicsCollider)
        && sameRecords(left.instances, right.instances, sameInstance)
        && sameRecords(left.materials, right.materials, sameMaterial)
        && sameRecords(left.images, right.images, sameImage);
}

template<typename T>
std::vector<T> concatenate(const std::vector<T>& first, const std::vector<T>& second)
{
    std::vector<T> result = first;
    result.insert(result.end(), second.begin(), second.end());
    return result;
}

xrphoton::SceneData makeTriangleModel(
    float positionBase = 0.0f,
    std::string texture = "metal\\triangle_albedo")
{
    xrphoton::SceneData model{};
    model.positions = {
        positionBase + 0.0f, 0.0f, 0.0f,
        positionBase + 1.0f, 0.0f, 0.0f,
        positionBase + 0.0f, 1.0f, 0.0f,
    };
    model.attributes = {
        {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
    };
    model.indices = {0, 1, 2};
    model.geometries.push_back({
        .firstVertex = 0,
        .vertexCount = 3,
        .firstIndex = 0,
        .indexCount = 3,
        .materialIndex = 0,
        .alphaTested = false,
    });
    model.meshes.push_back({
        .firstGeometry = 0,
        .geometryCount = 1,
    });
    xrphoton::SceneMaterial material{};
    material.baseColorFactor[0] = 0.125f;
    material.baseColorFactor[1] = 0.25f;
    material.baseColorFactor[2] = 0.5f;
    material.baseColorFactor[3] = 1.0f;
    material.alphaCutoff = 0.375f;
    material.baseColorTexture = std::move(texture);
    model.materials.push_back(std::move(material));
    return model;
}

xrphoton::ScenePhysicsCollider makeBoxCollider(
    float mass = 3.0f,
    std::string material = "physics\\box")
{
    return {
        .shape = xrphoton::ScenePhysicsShape::Box,
        .center = {0.25f, 0.5f, 0.75f},
        .orientation = {0.5f, 0.5f, 0.5f, 0.5f},
        .halfExtents = {0.25f, 0.5f, 0.75f},
        .mass = mass,
        .centerOfMass = {0.125f, 0.25f, 0.375f},
        .material = std::move(material),
    };
}

xrphoton::ScenePhysicsCollider makeCylinderCollider(
    float mass = 5.0f,
    std::string material = "physics\\cylinder")
{
    return {
        .shape = xrphoton::ScenePhysicsShape::Cylinder,
        .center = {-0.25f, -0.5f, -0.75f},
        .axis = {1.0f, 2.0f, 3.0f},
        .height = 1.25f,
        .radius = 0.375f,
        .mass = mass,
        .centerOfMass = {-0.125f, -0.25f, -0.375f},
        .material = std::move(material),
    };
}

void addPhysicsRecipe(
    xrphoton::SceneData* model,
    std::vector<xrphoton::ScenePhysicsCollider> colliders,
    float bodyMass = 8.0f,
    glm::vec3 bodyCenterOfMass = {0.1f, 0.2f, 0.3f})
{
    model->physicsBodies.push_back({
        .meshIndex = 0,
        .firstCollider = 0,
        .colliderCount = static_cast<std::uint32_t>(colliders.size()),
        .mass = bodyMass,
        .centerOfMass = bodyCenterOfMass,
    });
    model->physicsColliders = std::move(colliders);
}

xrphoton::SceneData makeTwoGeometryModel()
{
    xrphoton::SceneData model{};
    model.positions = {
        10.0f, 0.0f, 0.0f,
        11.0f, 0.0f, 0.0f,
        10.0f, 1.0f, 0.0f,
        12.0f, 0.0f, 0.0f,
        13.0f, 0.0f, 0.0f,
        13.0f, 1.0f, 0.0f,
        12.0f, 1.0f, 0.0f,
    };
    model.attributes = {
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 1.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 1.0f, 1.0f},
        {0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
    };
    model.indices = {0, 1, 2, 0, 1, 2, 0, 2, 3};
    model.geometries = {
        xrphoton::SceneGeometry{0, 3, 0, 3, 0, false},
        xrphoton::SceneGeometry{3, 4, 3, 6, 1, true},
    };
    model.meshes.push_back({
        .firstGeometry = 0,
        .geometryCount = 2,
    });

    xrphoton::SceneMaterial firstMaterial{};
    firstMaterial.baseColorFactor[0] = 0.2f;
    firstMaterial.baseColorFactor[1] = 0.3f;
    firstMaterial.baseColorFactor[2] = 0.4f;
    firstMaterial.baseColorTexture = "stone\\first";
    model.materials.push_back(std::move(firstMaterial));

    xrphoton::SceneMaterial secondMaterial{};
    secondMaterial.baseColorFactor[0] = 0.6f;
    secondMaterial.baseColorFactor[1] = 0.7f;
    secondMaterial.baseColorFactor[2] = 0.8f;
    secondMaterial.alphaCutoff = 0.625f;
    secondMaterial.baseColorTexture = "stone\\second";
    model.materials.push_back(std::move(secondMaterial));
    return model;
}

xrphoton::SceneData makeValidAssembledScene()
{
    xrphoton::SceneData scene = makeTriangleModel();
    addPhysicsRecipe(
        &scene,
        {makeBoxCollider(), makeCylinderCollider()});
    scene.instances.push_back({
        .meshIndex = 0,
        .transform = glm::mat4(1.0f),
    });
    return scene;
}

xrphoton::ogfx::Model makeRuntimeModel(float positionBase, bool quad)
{
    xrphoton::ogfx::Model model{};
    model.positions = {
        {positionBase + 0.0f, 0.0f, 0.0f},
        {positionBase + 1.0f, 0.0f, 0.0f},
        {positionBase + 0.0f, 1.0f, 0.0f},
    };
    model.attributes = {
        {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
    };
    model.indices = {0, 1, 2};
    if (quad) {
        model.positions.push_back({positionBase + 1.0f, 1.0f, 0.0f});
        model.attributes.push_back({0.0f, 0.0f, 1.0f, 1.0f, 1.0f});
        model.indices = {0, 1, 2, 1, 3, 2};
    }
    model.geometries.push_back({
        .firstVertex = 0,
        .vertexCount = static_cast<std::uint32_t>(model.positions.size()),
        .firstIndex = 0,
        .indexCount = static_cast<std::uint32_t>(model.indices.size()),
        .materialIndex = 0,
        .alphaTested = false,
    });
    model.meshes.push_back({0, 1});
    model.materials.emplace_back();
    model.materials[0].baseColorFactor = quad
        ? std::array<float, 4>{0.1f, 0.2f, 0.3f, 1.0f}
        : std::array<float, 4>{0.7f, 0.6f, 0.5f, 1.0f};
    model.materials[0].baseColorTexture = quad
        ? "runtime\\second"
        : "runtime\\first";
    model.physicsBodies.push_back({
        .firstCollider = 0,
        .colliderCount = quad ? 2u : 1u,
        .mass = quad ? 11.0f : 5.0f,
        .centerOfMass = quad
            ? xrphoton::ogfx::Position{0.2f, 0.3f, 0.4f}
            : xrphoton::ogfx::Position{-0.2f, -0.3f, -0.4f},
    });
    model.physicsColliders.push_back({
        .shapeType = xrphoton::ogfx::PhysicsShapeType::Cylinder,
        .flags = 0,
        .material = quad ? "runtime\\cylinder-second" : "runtime\\cylinder-first",
        .sourceNode = {},
        .center = {0.0f, 0.5f, 0.0f},
        .axis = {1.0f, 2.0f, 3.0f},
        .height = 1.0f,
        .radius = 0.25f,
        .mass = quad ? 6.0f : 5.0f,
        .centerOfMass = {0.0f, 0.5f, 0.0f},
    });
    if (quad) {
        model.physicsColliders.push_back({
            .shapeType = xrphoton::ogfx::PhysicsShapeType::Box,
            .flags = 0,
            .material = "runtime\\box-second",
            .sourceNode = {},
            .center = {0.25f, 0.0f, -0.25f},
            .orientation = {0.5f, 0.5f, 0.5f, 0.5f},
            .halfExtents = {0.2f, 0.3f, 0.4f},
            .mass = 5.0f,
            .centerOfMass = {0.25f, 0.0f, -0.25f},
        });
    }
    return model;
}

bool appendForTest(
    xrphoton::SceneData* scene,
    xrphoton::SceneData model,
    std::string_view description)
{
    std::string error;
    const bool appended = xrphoton::appendSceneModel(scene, std::move(model), &error);
    expect(appended, description);
    if (!appended) {
        std::cerr << error << '\n';
    }
    return appended;
}

void testIdentityAppend()
{
    xrphoton::SceneData model = makeTwoGeometryModel();
    addPhysicsRecipe(&model, {makeBoxCollider(7.0f, "identity\\box")}, 7.0f);
    const xrphoton::SceneData expected = model;
    xrphoton::SceneData scene{};
    std::string error = "stale diagnostic";

    const bool appended = xrphoton::appendSceneModel(&scene, std::move(model), &error);
    expect(appended, "one model appends into an empty scene");
    expect(error.empty(), "successful identity append clears an old diagnostic");
    expect(sameScene(scene, expected), "empty-scene append preserves every model field exactly");
}

void testCompleteTwoModelRebase()
{
    xrphoton::SceneData first = makeTriangleModel();
    addPhysicsRecipe(&first, {makeBoxCollider(3.0f, "first\\box")}, 3.0f);
    xrphoton::SceneData second = makeTwoGeometryModel();
    addPhysicsRecipe(
        &second,
        {
            makeCylinderCollider(5.0f, "second\\cylinder"),
            makeBoxCollider(7.0f, "second\\box"),
        },
        12.0f,
        {-0.4f, 0.5f, -0.6f});
    xrphoton::SceneData scene{};
    if (!appendForTest(&scene, first, "first hand-built model appends")
        || !appendForTest(&scene, second, "second hand-built model appends")) {
        return;
    }

    expect(
        scene.positions == concatenate(first.positions, second.positions),
        "positions concatenate without changing scalar values");
    expect(
        sameRecords(
            scene.attributes,
            concatenate(first.attributes, second.attributes),
            sameAttribute),
        "attributes concatenate without changing their fields");
    expect(
        scene.indices == concatenate(first.indices, second.indices),
        "geometry-local index values concatenate without rebasing");

    expect(scene.geometries.size() == 3, "two models contribute all three geometries");
    if (scene.geometries.size() == 3) {
        expect(
            sameGeometry(scene.geometries[0], first.geometries[0]),
            "first geometry remains unchanged");
        expect(
            sameGeometry(
                scene.geometries[1],
                xrphoton::SceneGeometry{3, 3, 3, 3, 1, false}),
            "second model first geometry rebases vertex, index, and material offsets");
        expect(
            sameGeometry(
                scene.geometries[2],
                xrphoton::SceneGeometry{6, 4, 6, 6, 2, true}),
            "second model second geometry rebases every offset and retains alpha classification");
    }

    expect(scene.meshes.size() == 2, "two models contribute two meshes");
    if (scene.meshes.size() == 2) {
        expect(sameMesh(scene.meshes[0], {0, 1}), "first mesh remains unchanged");
        expect(sameMesh(scene.meshes[1], {1, 2}), "second mesh firstGeometry rebases");
    }

    expect(scene.materials.size() == 3, "all materials append");
    if (scene.materials.size() == 3) {
        expect(sameMaterial(scene.materials[0], first.materials[0]),
            "first material remains unchanged");
        expect(sameMaterial(scene.materials[1], second.materials[0]),
            "second model first material appends as-is");
        expect(sameMaterial(scene.materials[2], second.materials[1]),
            "second model second material appends as-is");
    }
    expect(scene.physicsBodies.size() == 2,
        "both models contribute their body recipes");
    if (scene.physicsBodies.size() == 2) {
        expect(samePhysicsBody(scene.physicsBodies[0], first.physicsBodies[0]),
            "first body ownership remains unchanged");
        xrphoton::ScenePhysicsBody expectedSecond = second.physicsBodies[0];
        expectedSecond.meshIndex = 1;
        expectedSecond.firstCollider = 1;
        expect(samePhysicsBody(scene.physicsBodies[1], expectedSecond),
            "second body rebases both mesh and first-collider ownership");
    }
    expect(scene.physicsColliders.size() == 3,
        "all physics colliders concatenate without changing recipe fields");
    if (scene.physicsColliders.size() == 3) {
        expect(samePhysicsCollider(
                   scene.physicsColliders[0], first.physicsColliders[0])
                && samePhysicsCollider(
                    scene.physicsColliders[1], second.physicsColliders[0])
                && samePhysicsCollider(
                    scene.physicsColliders[2], second.physicsColliders[1]),
            "collider shape, transforms, dimensions, mass, COM, and material stay exact");
    }
    expect(scene.instances.empty(), "model merging invents no instances");
    expect(scene.images.empty(), "model merging invents no images");
}

void testRepeatedRebases()
{
    const xrphoton::SceneData first = makeTriangleModel(0.0f, "repeat\\first");
    const xrphoton::SceneData second = makeTwoGeometryModel();
    const xrphoton::SceneData third = makeTriangleModel(20.0f, "repeat\\third");
    xrphoton::SceneData scene{};
    if (!appendForTest(&scene, first, "three-model first append")
        || !appendForTest(&scene, second, "three-model second append")
        || !appendForTest(&scene, third, "three-model third append")) {
        return;
    }

    expect(scene.geometries.size() == 4, "three appends retain four geometry records");
    if (scene.geometries.size() == 4) {
        expect(
            sameGeometry(
                scene.geometries[3],
                xrphoton::SceneGeometry{10, 3, 12, 3, 3, false}),
            "third model rebases against both earlier models");
    }
    expect(scene.meshes.size() == 3, "three appends retain three mesh records");
    if (scene.meshes.size() == 3) {
        expect(sameMesh(scene.meshes[2], {3, 1}),
            "third mesh rebases against all earlier geometries");
    }
    expect(scene.materials.size() == 4, "three appends retain four materials");
    expect(
        scene.positions
            == concatenate(concatenate(first.positions, second.positions), third.positions),
        "three appends retain raw position order");
    expect(
        scene.indices == concatenate(concatenate(first.indices, second.indices), third.indices),
        "three appends retain geometry-local index order");
}

void testDuplicateIndependence()
{
    const xrphoton::SceneData model = makeTriangleModel(4.0f, "duplicate\\shared_name");
    xrphoton::SceneData scene{};
    if (!appendForTest(&scene, model, "duplicate first append")
        || !appendForTest(&scene, model, "duplicate second append")) {
        return;
    }

    expect(scene.geometries.size() == 2
            && sameGeometry(scene.geometries[1], {3, 3, 3, 3, 1, false}),
        "duplicate model receives independent rebased geometry records");
    expect(scene.meshes.size() == 2 && sameMesh(scene.meshes[1], {1, 1}),
        "duplicate model receives an independent mesh record");
    expect(scene.materials.size() == 2, "duplicate model creates two material values");
    if (scene.materials.size() == 2) {
        scene.materials[0].baseColorTexture[0] = 'X';
        expect(
            scene.materials[1].baseColorTexture == "duplicate\\shared_name",
            "editing one duplicate texture string cannot alter the other");
    }
    if (scene.positions.size() == 18) {
        scene.positions[0] = -999.0f;
        expect(scene.positions[9] == 4.0f,
            "editing one duplicate vertex cannot alter the other copy");
    }
}

void testExactTextureStrings()
{
    constexpr std::string_view firstName = "ston\\Stena_Marbl_m_03_back";
    constexpr std::string_view secondName = "props\\barrel-FUEL_01";
    xrphoton::SceneData first = makeTriangleModel(0.0f, std::string(firstName));
    xrphoton::SceneData second = makeTwoGeometryModel();
    second.materials[0].baseColorTexture = secondName;
    second.materials[1].baseColorTexture.clear();

    xrphoton::SceneData scene{};
    if (!appendForTest(&scene, first, "textured first model appends")
        || !appendForTest(&scene, second, "textured second model appends")) {
        return;
    }

    expect(scene.materials.size() == 3, "textured models contribute three materials");
    if (scene.materials.size() == 3) {
        expect(scene.materials[0].baseColorTexture == firstName,
            "mixed-case texture reference survives byte-exact");
        expect(scene.materials[1].baseColorTexture == secondName,
            "second texture reference survives byte-exact");
        expect(scene.materials[2].baseColorTexture.empty(),
            "an empty texture reference remains empty");
    }
}

void testRuntimeLoadedModels()
{
    const xrphoton::ogfx::SerializeResult firstSerialized =
        xrphoton::ogfx::serializeModel(makeRuntimeModel(0.0f, false), "first-runtime-model");
    const xrphoton::ogfx::SerializeResult secondSerialized =
        xrphoton::ogfx::serializeModel(makeRuntimeModel(10.0f, true), "second-runtime-model");
    expect(static_cast<bool>(firstSerialized), "first end-to-end model serializes");
    expect(static_cast<bool>(secondSerialized), "second end-to-end model serializes");
    if (!firstSerialized || !secondSerialized) {
        if (!firstSerialized) {
            std::cerr << firstSerialized.error << '\n';
        }
        if (!secondSerialized) {
            std::cerr << secondSerialized.error << '\n';
        }
        return;
    }

    xrphoton::OgfxLoadResult firstLoaded =
        xrphoton::decodeOgfxScene(firstSerialized.bytes, "first-runtime-model.ogfx");
    xrphoton::OgfxLoadResult secondLoaded =
        xrphoton::decodeOgfxScene(secondSerialized.bytes, "second-runtime-model.ogfx");
    expect(
        static_cast<bool>(firstLoaded),
        "first serialized model loads through the runtime adapter");
    expect(
        static_cast<bool>(secondLoaded),
        "second serialized model loads through the runtime adapter");
    if (!firstLoaded || !secondLoaded) {
        if (!firstLoaded) {
            std::cerr << firstLoaded.error << '\n';
        }
        if (!secondLoaded) {
            std::cerr << secondLoaded.error << '\n';
        }
        return;
    }

    xrphoton::SceneData scene{};
    if (!appendForTest(
            &scene, std::move(firstLoaded.scene), "first runtime-loaded model appends")
        || !appendForTest(
            &scene, std::move(secondLoaded.scene), "second runtime-loaded model appends")) {
        return;
    }

    expect(scene.geometries.size() == 2, "runtime-loaded merge retains both geometries");
    if (scene.geometries.size() == 2) {
        expect(
            sameGeometry(scene.geometries[0], {0, 3, 0, 3, 0, false}),
            "first runtime-loaded geometry stays at its original offsets");
        expect(
            sameGeometry(scene.geometries[1], {3, 4, 3, 6, 1, false}),
            "second runtime-loaded geometry rebases through the real loader path");
    }
    expect(scene.meshes.size() == 2 && sameMesh(scene.meshes[1], {1, 1}),
        "runtime-loaded second mesh rebases through the real loader path");
    expect(scene.materials.size() == 2
            && scene.materials[0].baseColorFactor[0] == 0.7f
            && scene.materials[1].baseColorFactor[0] == 0.1f
            && scene.materials[0].baseColorTexture == "runtime\\first"
            && scene.materials[1].baseColorTexture == "runtime\\second",
        "runtime-loaded material and texture identities remain distinct after rebasing");
    expect(scene.physicsBodies.size() == 2
            && scene.physicsBodies[0].meshIndex == 0
            && scene.physicsBodies[0].firstCollider == 0
            && scene.physicsBodies[0].colliderCount == 1
            && scene.physicsBodies[1].meshIndex == 1
            && scene.physicsBodies[1].firstCollider == 1
            && scene.physicsBodies[1].colliderCount == 2,
        "runtime-loaded body recipes rebase mesh and collider ownership end to end");
    expect(scene.physicsColliders.size() == 3
            && scene.physicsColliders[0].shape
                == xrphoton::ScenePhysicsShape::Cylinder
            && scene.physicsColliders[0].material == "runtime\\cylinder-first"
            && scene.physicsColliders[1].shape
                == xrphoton::ScenePhysicsShape::Cylinder
            && scene.physicsColliders[1].material == "runtime\\cylinder-second"
            && scene.physicsColliders[2].shape == xrphoton::ScenePhysicsShape::Box
            && scene.physicsColliders[2].material == "runtime\\box-second"
            && scene.physicsColliders[2].orientation.w == 0.5f
            && scene.physicsColliders[2].orientation.x == 0.5f,
        "runtime-loaded collider recipes retain order, shapes, materials, and quaternion fields");
}

void testInstanceAppend()
{
    xrphoton::SceneData scene{};
    if (!appendForTest(&scene, makeTriangleModel(), "instance test first model appends")
        || !appendForTest(&scene, makeTwoGeometryModel(), "instance test second model appends")) {
        return;
    }

    glm::mat4 translated(1.0f);
    translated[3][0] = 7.0f;
    std::string error = "stale";
    expect(xrphoton::appendSceneInstance(&scene, 0, glm::mat4(1.0f), &error),
        "first mesh accepts an identity instance");
    expect(error.empty(), "successful instance append clears an old diagnostic");
    expect(xrphoton::appendSceneInstance(&scene, 1, translated, &error),
        "second mesh accepts a translated instance");
    expect(xrphoton::appendSceneInstance(&scene, 0, translated, &error),
        "one mesh can be shared by a second instance");
    expect(scene.instances.size() == 3, "three valid placements append");
    if (scene.instances.size() == 3) {
        expect(scene.instances[0].meshIndex == 0, "first placement retains mesh index zero");
        expect(scene.instances[1].meshIndex == 1
                && sameMatrix(scene.instances[1].transform, translated),
            "second placement retains its mesh and transform");
        expect(scene.instances[2].meshIndex == 0,
            "third placement shares the first mesh");
    }

    const xrphoton::SceneData beforeRejection = scene;
    expect(!xrphoton::appendSceneInstance(&scene, 2, glm::mat4(1.0f), &error),
        "mesh index equal to mesh count is rejected");
    expect(contains(error, "meshIndex 2"), "invalid-instance diagnostic names mesh index 2");
    expect(sameScene(scene, beforeRejection),
        "invalid instance append leaves the scene unchanged");
}

void testYardShapedAssembly()
{
    xrphoton::SceneData scene{};
    if (!appendForTest(
            &scene,
            makeTriangleModel(0.0f, "yard\\ground"),
            "yard ground model appends")
        || !appendForTest(
            &scene,
            makeTriangleModel(10.0f, "yard\\wall"),
            "yard wall model appends")
        || !appendForTest(
            &scene,
            makeTriangleModel(20.0f, "yard\\box"),
            "yard box model appends")
        || !appendForTest(
            &scene,
            makeTriangleModel(30.0f, "yard\\quad"),
            "yard quad model appends")
        || !appendForTest(
            &scene,
            makeTwoGeometryModel(),
            "yard wedge model appends")) {
        return;
    }

    expect(scene.meshes.size() == 5, "yard-shaped merge contains five meshes");
    expect(scene.geometries.size() == 6, "yard-shaped merge contains six geometries");
    expect(scene.materials.size() == 6, "yard-shaped merge contains six materials");

    const auto translate = [](glm::vec3 offset) {
        return glm::translate(glm::mat4{1.0f}, offset);
    };
    const auto scaled = [&translate](glm::vec3 offset, glm::vec3 scale) {
        return translate(offset) * glm::scale(glm::mat4{1.0f}, scale);
    };
    const std::array placements{
        xrphoton::SceneInstance{0, glm::mat4{1.0f}},
        xrphoton::SceneInstance{1, translate({6.0f, -0.01f, 9.85f})},
        xrphoton::SceneInstance{
            1,
            translate({9.84f, -0.01f, 5.71f})
                * glm::rotate(
                    glm::mat4{1.0f},
                    glm::radians(90.0f),
                    glm::vec3{0.0f, 1.0f, 0.0f})},
        xrphoton::SceneInstance{2, scaled({5.0f, 0.49f, 5.0f}, {2.0f, 1.0f, 2.0f})},
        xrphoton::SceneInstance{2, scaled({5.0f, 0.115f, 1.59f}, {1.92f, 0.25f, 0.7f})},
        xrphoton::SceneInstance{2, scaled({5.0f, 0.24f, 2.28f}, {1.94f, 0.5f, 0.7f})},
        xrphoton::SceneInstance{2, scaled({5.0f, 0.365f, 2.97f}, {1.96f, 0.75f, 0.7f})},
        xrphoton::SceneInstance{2, scaled({5.0f, 0.49f, 3.66f}, {1.98f, 1.0f, 0.7f})},
        xrphoton::SceneInstance{
            2,
            translate({-3.0f, 0.49f, 4.0f})
                * glm::rotate(
                    glm::mat4{1.0f},
                    glm::radians(30.0f),
                    glm::vec3{0.0f, 1.0f, 0.0f})},
        xrphoton::SceneInstance{2, translate({3.0f, 0.9f, 0.0f})},
        xrphoton::SceneInstance{3, translate({-6.0f, 1.0f, 9.5f})},
        xrphoton::SceneInstance{4, translate({-4.25f, 1.0f, 9.35f})},
        xrphoton::SceneInstance{
            4,
            translate({-2.1f, 1.0f, 9.0f})
                * glm::rotate(
                    glm::mat4{1.0f},
                    glm::radians(30.0f),
                    glm::vec3{0.0f, 1.0f, 0.0f})
                * glm::scale(glm::mat4{1.0f}, glm::vec3{1.5f, 1.0f, 1.5f})},
    };

    std::string error;
    for (const xrphoton::SceneInstance& placement : placements) {
        if (!xrphoton::appendSceneInstance(
                &scene,
                placement.meshIndex,
                placement.transform,
                &error)) {
            expect(false, "yard-shaped placement appends");
            std::cerr << error << '\n';
            return;
        }
    }

    expect(scene.instances.size() == 13, "yard-shaped assembly contains thirteen instances");
    std::size_t boxInstanceCount = 0;
    for (const xrphoton::SceneInstance& instance : scene.instances) {
        if (instance.meshIndex == 2) {
            ++boxInstanceCount;
        }
    }
    expect(boxInstanceCount == 7, "one yard-box mesh is shared by seven placements");
    expect(
        sameRecords(scene.instances, std::vector(placements.begin(), placements.end()), sameInstance),
        "yard-shaped assembly preserves placement order, transforms, and shared mesh indices");
    expect(
        xrphoton::validateAssembledScene(scene, &error),
        "complete yard-shaped assembled scene passes final validation");
    if (!error.empty()) {
        std::cerr << error << '\n';
    }
}

void expectValidationRejected(
    xrphoton::SceneData scene,
    std::string_view fragment,
    std::string_view description)
{
    std::string error;
    const bool valid = xrphoton::validateAssembledScene(scene, &error);
    expect(!valid, description);
    expect(contains(error, fragment),
        std::string(description) + " diagnostic names " + std::string(fragment));
}

void testFinalValidation()
{
    xrphoton::SceneData valid = makeValidAssembledScene();
    std::string error = "stale";
    expect(xrphoton::validateAssembledScene(valid, &error),
        "complete assembled scene passes final validation");
    expect(error.empty(), "successful final validation clears an old diagnostic");

    xrphoton::SceneData twoBodyPartition = valid;
    twoBodyPartition.physicsBodies[0].colliderCount = 1;
    twoBodyPartition.physicsBodies.push_back({
        .meshIndex = 0,
        .firstCollider = 1,
        .colliderCount = 1,
        .mass = 5.0f,
        .centerOfMass = {-0.1f, -0.2f, -0.3f},
    });
    expect(xrphoton::validateAssembledScene(twoBodyPartition, &error),
        "multiple bodies may exactly partition one collider array in order");

    xrphoton::SceneData subnormalAxis = valid;
    subnormalAxis.physicsColliders[1].axis = {
        std::numeric_limits<float>::denorm_min(),
        0.0f,
        0.0f,
    };
    expect(xrphoton::validateAssembledScene(subnormalAxis, &error),
        "double-precision axis validation accepts a nonzero subnormal cylinder axis");

    xrphoton::SceneData noInstances = valid;
    noInstances.instances.clear();
    expectValidationRejected(
        std::move(noInstances), "at least one instance", "empty instance list is rejected");

    xrphoton::SceneData emptyMesh = valid;
    emptyMesh.meshes[0].geometryCount = 0;
    expectValidationRejected(
        std::move(emptyMesh), "mesh[0].geometryCount", "empty mesh range is rejected");

    xrphoton::SceneData outOfBoundsMesh = valid;
    outOfBoundsMesh.meshes[0].firstGeometry = 1;
    expectValidationRejected(
        std::move(outOfBoundsMesh),
        "mesh[0] geometry range",
        "out-of-bounds mesh range is rejected");

    xrphoton::SceneData invalidInstance = valid;
    invalidInstance.instances[0].meshIndex = 1;
    expectValidationRejected(
        std::move(invalidInstance),
        "instance[0].meshIndex 1",
        "out-of-bounds instance mesh is rejected");

    xrphoton::SceneData bodyWithoutColliders = valid;
    bodyWithoutColliders.physicsColliders.clear();
    expectValidationRejected(
        std::move(bodyWithoutColliders),
        "body and collider arrays",
        "a physics body array without colliders is rejected");

    xrphoton::SceneData collidersWithoutBody = valid;
    collidersWithoutBody.physicsBodies.clear();
    expectValidationRejected(
        std::move(collidersWithoutBody),
        "body and collider arrays",
        "a physics collider array without bodies is rejected");

    xrphoton::SceneData invalidBodyMesh = valid;
    invalidBodyMesh.physicsBodies[0].meshIndex = 1;
    expectValidationRejected(
        std::move(invalidBodyMesh),
        "physicsBodies[0].meshIndex 1",
        "out-of-bounds physics body mesh ownership is rejected");

    xrphoton::SceneData emptyBody = valid;
    emptyBody.physicsBodies[0].colliderCount = 0;
    expectValidationRejected(
        std::move(emptyBody),
        "physicsBodies[0].colliderCount",
        "a body owning zero colliders is rejected");

    xrphoton::SceneData partitionGap = valid;
    partitionGap.physicsBodies[0].firstCollider = 1;
    expectValidationRejected(
        std::move(partitionGap),
        "physicsBodies[0].firstCollider",
        "a collider partition gap at the first body is rejected");

    xrphoton::SceneData partitionOverflow = valid;
    partitionOverflow.physicsBodies[0].colliderCount = 3;
    expectValidationRejected(
        std::move(partitionOverflow),
        "collider range exceeds",
        "a body collider range beyond the array is rejected");

    xrphoton::SceneData partitionTail = valid;
    partitionTail.physicsBodies[0].colliderCount = 1;
    expectValidationRejected(
        std::move(partitionTail),
        "final physics collider partition end",
        "unowned colliders at the end of the partition are rejected");

    xrphoton::SceneData invalidBodyMass = valid;
    invalidBodyMass.physicsBodies[0].mass = 0.0f;
    expectValidationRejected(
        std::move(invalidBodyMass),
        "physicsBodies[0].mass",
        "a non-positive body mass is rejected");

    xrphoton::SceneData nonFiniteBodyMass = valid;
    nonFiniteBodyMass.physicsBodies[0].mass =
        std::numeric_limits<float>::infinity();
    expectValidationRejected(
        std::move(nonFiniteBodyMass),
        "physicsBodies[0].mass",
        "a non-finite body mass is rejected");

    xrphoton::SceneData invalidBodyCom = valid;
    invalidBodyCom.physicsBodies[0].centerOfMass.y =
        std::numeric_limits<float>::quiet_NaN();
    expectValidationRejected(
        std::move(invalidBodyCom),
        "physicsBodies[0].centerOfMass",
        "a non-finite aggregate center of mass is rejected");

    xrphoton::SceneData invalidShape = valid;
    invalidShape.physicsColliders[0].shape =
        static_cast<xrphoton::ScenePhysicsShape>(99);
    expectValidationRejected(
        std::move(invalidShape),
        "physicsColliders[0].shape",
        "an unknown runtime collider shape is rejected");

    xrphoton::SceneData invalidColliderCenter = valid;
    invalidColliderCenter.physicsColliders[0].center.z =
        std::numeric_limits<float>::infinity();
    expectValidationRejected(
        std::move(invalidColliderCenter),
        "physicsColliders[0].center",
        "a non-finite collider center is rejected");

    xrphoton::SceneData invalidColliderMass = valid;
    invalidColliderMass.physicsColliders[0].mass = -1.0f;
    expectValidationRejected(
        std::move(invalidColliderMass),
        "physicsColliders[0].mass",
        "a non-positive collider mass is rejected");

    xrphoton::SceneData invalidColliderCom = valid;
    invalidColliderCom.physicsColliders[1].centerOfMass.x =
        std::numeric_limits<float>::quiet_NaN();
    expectValidationRejected(
        std::move(invalidColliderCom),
        "physicsColliders[1].centerOfMass",
        "a non-finite collider center of mass is rejected");

    xrphoton::SceneData invalidAxisComponents = valid;
    invalidAxisComponents.physicsColliders[1].axis.x =
        std::numeric_limits<float>::infinity();
    expectValidationRejected(
        std::move(invalidAxisComponents),
        "physicsColliders[1].axis",
        "a non-finite cylinder axis component is rejected");

    xrphoton::SceneData zeroAxis = valid;
    zeroAxis.physicsColliders[1].axis = {};
    expectValidationRejected(
        std::move(zeroAxis),
        "axis length squared",
        "a zero-length cylinder axis is rejected");

    xrphoton::SceneData invalidHeight = valid;
    invalidHeight.physicsColliders[1].height = 0.0f;
    expectValidationRejected(
        std::move(invalidHeight),
        "physicsColliders[1].height",
        "a non-positive cylinder height is rejected");

    xrphoton::SceneData invalidRadius = valid;
    invalidRadius.physicsColliders[1].radius =
        std::numeric_limits<float>::quiet_NaN();
    expectValidationRejected(
        std::move(invalidRadius),
        "physicsColliders[1].radius",
        "a non-finite cylinder radius is rejected");

    xrphoton::SceneData invalidOrientation = valid;
    invalidOrientation.physicsColliders[0].orientation.w = 0.25f;
    expectValidationRejected(
        std::move(invalidOrientation),
        "orientation length squared",
        "a box quaternion outside the normalization tolerance is rejected");

    xrphoton::SceneData nonFiniteOrientation = valid;
    nonFiniteOrientation.physicsColliders[0].orientation.x =
        std::numeric_limits<float>::infinity();
    expectValidationRejected(
        std::move(nonFiniteOrientation),
        "orientation length squared",
        "a non-finite box quaternion is rejected");

    xrphoton::SceneData invalidHalfExtents = valid;
    invalidHalfExtents.physicsColliders[0].halfExtents.y = 0.0f;
    expectValidationRejected(
        std::move(invalidHalfExtents),
        "physicsColliders[0].halfExtents",
        "a non-positive box half extent is rejected");

    xrphoton::SceneData nonFiniteHalfExtents = valid;
    nonFiniteHalfExtents.physicsColliders[0].halfExtents.z =
        std::numeric_limits<float>::quiet_NaN();
    expectValidationRejected(
        std::move(nonFiniteHalfExtents),
        "physicsColliders[0].halfExtents",
        "a non-finite box half extent is rejected");

    xrphoton::SceneData nonFinite = valid;
    nonFinite.instances[0].transform[2][1] = std::numeric_limits<float>::quiet_NaN();
    expectValidationRejected(
        std::move(nonFinite),
        "instance[0].transform[2][1] is not finite",
        "non-finite transform element is rejected");

    xrphoton::SceneData singular = valid;
    singular.instances[0].transform[0][0] = 0.0f;
    expectValidationRejected(
        std::move(singular),
        "instance[0] transform has a zero-determinant",
        "singular transform is rejected");

    xrphoton::SceneData projective = valid;
    projective.instances[0].transform[1][3] = 2.0e-4f;
    expectValidationRejected(
        std::move(projective),
        "homogeneous row",
        "a transform outside the affine homogeneous-row tolerance is rejected");

    xrphoton::SceneData reflected = valid;
    reflected.instances[0].transform[0][0] = -1.0f;
    expect(xrphoton::validateAssembledScene(reflected, &error),
        "a finite affine transform with a nonzero negative determinant is accepted");
}

void expectAppendRejectedUnchanged(
    xrphoton::SceneData model,
    std::string_view diagnosticFragment,
    std::string_view description,
    xrphoton::SceneData scene = makeTriangleModel(100.0f, "target\\unchanged"))
{
    const xrphoton::SceneData before = scene;
    std::string error;
    const bool appended = xrphoton::appendSceneModel(&scene, std::move(model), &error);
    expect(!appended, description);
    expect(contains(error, diagnosticFragment),
        std::string(description) + " has its named diagnostic");
    expect(sameScene(scene, before),
        std::string(description) + " leaves destination values unchanged");
}

void testModelPreconditionsAreTransactional()
{
    xrphoton::SceneData withInstance = makeTriangleModel();
    withInstance.instances.push_back({0, glm::mat4(1.0f)});
    expectAppendRejectedUnchanged(
        std::move(withInstance), "incoming model instances", "incoming instance precondition");

    xrphoton::SceneData withImage = makeTriangleModel();
    withImage.images.push_back({
        .format = xrphoton::SceneImageFormat::Rgba8Srgb,
        .width = 1,
        .height = 1,
        .pixels = {255, 255, 255, 255},
    });
    expectAppendRejectedUnchanged(
        std::move(withImage), "incoming model images", "incoming image precondition");

    xrphoton::SceneData nonDivisiblePositions = makeTriangleModel();
    nonDivisiblePositions.positions.pop_back();
    expectAppendRejectedUnchanged(
        std::move(nonDivisiblePositions),
        "position scalar count must be divisible by 3",
        "non-divisible position count precondition");

    xrphoton::SceneData mismatchedCounts = makeTriangleModel();
    mismatchedCounts.attributes.pop_back();
    expectAppendRejectedUnchanged(
        std::move(mismatchedCounts),
        "position and attribute counts must match",
        "position/attribute mismatch precondition");

    xrphoton::SceneData resolvedMaterial = makeTriangleModel();
    resolvedMaterial.materials[0].baseColorImage = 1;
    expectAppendRejectedUnchanged(
        std::move(resolvedMaterial),
        "incoming material[0].baseColorImage",
        "resolved per-model image index precondition");

    xrphoton::SceneData scene = makeTriangleModel(100.0f, "target\\with_image");
    scene.images.push_back({
        .format = xrphoton::SceneImageFormat::Rgba8Srgb,
        .width = 1,
        .height = 1,
        .pixels = {255, 255, 255, 255},
    });
    const xrphoton::SceneData before = scene;
    std::string error;
    expect(!xrphoton::appendSceneModel(&scene, makeTriangleModel(), &error),
        "model append after scene image resolution is rejected");
    expect(contains(error, "before scene images resolve"),
        "resolved destination image diagnostic names ordering rule");
    expect(sameScene(scene, before),
        "destination-image rejection leaves destination values unchanged");

    xrphoton::SceneData aliased = makeTriangleModel(200.0f, "target\\aliased");
    const xrphoton::SceneData beforeAliasRejection = aliased;
    expect(!xrphoton::appendSceneModel(&aliased, std::move(aliased), &error),
        "a scene cannot append itself as an incoming model");
    expect(contains(error, "must not alias"),
        "self-append diagnostic names the aliasing rule");
    expect(sameScene(aliased, beforeAliasRejection),
        "self-append rejection leaves destination values unchanged");
}

void testRebaseOffsetOverflowIsTransactional()
{
    constexpr std::uint32_t maximumUint32 =
        std::numeric_limits<std::uint32_t>::max();

    xrphoton::SceneData firstVertexOverflow = makeTriangleModel();
    firstVertexOverflow.geometries[0].firstVertex = maximumUint32;
    expectAppendRejectedUnchanged(
        std::move(firstVertexOverflow),
        "geometry.firstVertex[0] rebases past UINT32_MAX",
        "firstVertex rebase overflow");

    xrphoton::SceneData firstIndexOverflow = makeTriangleModel();
    firstIndexOverflow.geometries[0].firstIndex = maximumUint32;
    expectAppendRejectedUnchanged(
        std::move(firstIndexOverflow),
        "geometry.firstIndex[0] rebases past UINT32_MAX",
        "firstIndex rebase overflow");

    xrphoton::SceneData materialIndexOverflow = makeTriangleModel();
    materialIndexOverflow.geometries[0].materialIndex = maximumUint32;
    expectAppendRejectedUnchanged(
        std::move(materialIndexOverflow),
        "geometry.materialIndex[0] rebases past UINT32_MAX",
        "materialIndex rebase overflow");

    xrphoton::SceneData firstGeometryOverflow = makeTriangleModel();
    firstGeometryOverflow.meshes[0].firstGeometry = maximumUint32;
    expectAppendRejectedUnchanged(
        std::move(firstGeometryOverflow),
        "mesh.firstGeometry[0] rebases past UINT32_MAX",
        "firstGeometry rebase overflow");

    xrphoton::SceneData physicsMeshOverflow = makeTriangleModel();
    addPhysicsRecipe(&physicsMeshOverflow, {makeBoxCollider()});
    physicsMeshOverflow.physicsBodies[0].meshIndex = maximumUint32;
    expectAppendRejectedUnchanged(
        std::move(physicsMeshOverflow),
        "physicsBody.meshIndex[0] rebases past UINT32_MAX",
        "physics body mesh-index rebase overflow");

    xrphoton::SceneData physicsColliderOverflow = makeTriangleModel();
    addPhysicsRecipe(&physicsColliderOverflow, {makeBoxCollider()});
    physicsColliderOverflow.physicsBodies[0].firstCollider = maximumUint32;
    xrphoton::SceneData physicsDestination =
        makeTriangleModel(100.0f, "target\\physics");
    addPhysicsRecipe(
        &physicsDestination,
        {makeCylinderCollider(5.0f, "target\\cylinder")});
    expectAppendRejectedUnchanged(
        std::move(physicsColliderOverflow),
        "physicsBody.firstCollider[0] rebases past UINT32_MAX",
        "physics body collider-offset rebase overflow",
        std::move(physicsDestination));
}

void expectCountRejected(
    const xrphoton::scene_assembly_detail::SceneElementCounts& destination,
    const xrphoton::scene_assembly_detail::SceneElementCounts& source,
    std::string_view diagnosticFragment,
    std::string_view description)
{
    std::string error;
    const bool accepted = xrphoton::scene_assembly_detail::validateSceneAppendCounts(
        destination, source, &error);
    expect(!accepted, description);
    expect(contains(error, diagnosticFragment),
        std::string(description) + " has its named diagnostic");
}

void testCountBoundaries()
{
    using Counts = xrphoton::scene_assembly_detail::SceneElementCounts;
    using CountMember = std::uint64_t Counts::*;
    constexpr std::uint64_t maximumUint32 = std::numeric_limits<std::uint32_t>::max();
    constexpr std::array<std::pair<CountMember, std::string_view>, 6> uint32Fields{{
        {&Counts::vertices, "vertex"},
        {&Counts::indices, "index"},
        {&Counts::meshes, "mesh"},
        {&Counts::materials, "material"},
        {&Counts::physicsBodies, "physics body"},
        {&Counts::physicsColliders, "physics collider"},
    }};

    for (const auto& [field, name] : uint32Fields) {
        Counts destination{};
        Counts source{};
        destination.*field = maximumUint32 - 7;
        source.*field = 7;
        std::string error = "stale";
        expect(
            xrphoton::scene_assembly_detail::validateSceneAppendCounts(
                destination, source, &error),
            std::string(name) + " total exactly UINT32_MAX is accepted");
        expect(error.empty(), std::string(name) + " exact-boundary success clears diagnostic");

        destination.*field = maximumUint32;
        source.*field = 1;
        expectCountRejected(
            destination,
            source,
            name,
            std::string(name) + " total above UINT32_MAX is rejected");
    }

    constexpr std::uint64_t geometryLimit =
        ((std::uint64_t{1} << 24) - 1) / xrphoton::RayTypeCount + 1;
    Counts destination{};
    Counts source{};
    destination.geometries = geometryLimit - 1;
    source.geometries = 1;
    std::string error;
    expect(
        xrphoton::scene_assembly_detail::validateSceneAppendCounts(
            destination, source, &error),
        "the largest geometry total whose RayTypeCount-scaled SBT offset fits is accepted");

    source.geometries = 2;
    expectCountRejected(
        destination,
        source,
        "geometry",
        "the first geometry total whose RayTypeCount-scaled SBT offset does not fit is rejected");
    source.geometries = 3;
    expectCountRejected(
        destination,
        source,
        "geometry",
        "larger geometry totals beyond the scaled 24-bit SBT limit are rejected");

    destination = {};
    source = {};
    destination.vertices = std::numeric_limits<std::uint64_t>::max();
    source.vertices = 1;
    expectCountRejected(
        destination,
        source,
        "vertex count overflows uint64",
        "uint64 count addition overflow is rejected");
}
}

int main()
{
    testIdentityAppend();
    testCompleteTwoModelRebase();
    testRepeatedRebases();
    testDuplicateIndependence();
    testExactTextureStrings();
    testRuntimeLoadedModels();
    testInstanceAppend();
    testYardShapedAssembly();
    testFinalValidation();
    testModelPreconditionsAreTransactional();
    testRebaseOffsetOverflowIsTransactional();
    testCountBoundaries();

    if (failureCount != 0) {
        std::cerr << failureCount << " scene-assembly test assertion(s) failed.\n";
        return 1;
    }
    return 0;
}
