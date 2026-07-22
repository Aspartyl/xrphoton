#include "ogfx.hpp"
#include "ogfx_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#ifndef XRPHOTON_TEST_YARD_GROUND_ASSET_PATH
#error "XRPHOTON_TEST_YARD_GROUND_ASSET_PATH must name the generated ground OGFx"
#endif
#ifndef XRPHOTON_TEST_YARD_WALL_ASSET_PATH
#error "XRPHOTON_TEST_YARD_WALL_ASSET_PATH must name the generated wall OGFx"
#endif
#ifndef XRPHOTON_TEST_YARD_BOX_ASSET_PATH
#error "XRPHOTON_TEST_YARD_BOX_ASSET_PATH must name the generated box OGFx"
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

xrphoton::ogfx::Model makeQuad()
{
    xrphoton::ogfx::Model model{};
    model.positions = {
        {-0.5f, -0.5f, 0.0f},
        { 0.5f, -0.5f, 0.0f},
        { 0.5f,  0.5f, 0.0f},
        {-0.5f,  0.5f, 0.0f},
    };
    model.attributes = {
        {1.0f, 0.0f, 0.0f, -1.0f, 2.0f},
        {0.0f, 1.0f, 0.0f,  3.0f, 4.0f},
        {0.0f, 0.0f, 1.0f,  5.0f, 6.0f},
        {1.0f, 1.0f, 1.0f,  7.0f, 8.0f},
    };
    model.indices = {0, 1, 2, 0, 2, 3};
    model.geometries.push_back({0, 4, 0, 6, 0, false});
    model.meshes.push_back({0, 1});
    model.materials.emplace_back();
    model.materials[0].baseColorFactor = {0.25f, 0.5f, 0.75f, 0.875f};
    model.materials[0].alphaCutoff = 0.375f;
    return model;
}

xrphoton::ogfx::Model makeOpaqueTwoGeometryModel()
{
    xrphoton::ogfx::Model model{};
    model.positions = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {2.0f, 0.0f, 0.0f},
        {3.0f, 0.0f, 0.0f},
        {2.0f, 1.0f, 0.0f},
    };
    model.attributes.assign(
        6,
        xrphoton::ogfx::VertexAttributes{0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
    model.indices = {0, 1, 2, 0, 1, 2};
    model.geometries = {
        {0, 3, 0, 3, 0, false},
        {3, 3, 3, 3, 1, false},
    };
    model.meshes = {{0, 2}};
    model.materials.resize(2);
    model.materials[0].baseColorFactor = {0.125f, 0.25f, 0.75f, 1.0f};
    model.materials[0].baseColorTexture = "ston\\first";
    model.materials[1].baseColorFactor = {0.25f, 0.75f, 0.125f, 1.0f};
    model.materials[1].baseColorTexture = "ston\\second";
    return model;
}

bool sceneIsEmpty(const xrphoton::SceneData& scene)
{
    return scene.positions.empty()
        && scene.attributes.empty()
        && scene.indices.empty()
        && scene.geometries.empty()
        && scene.meshes.empty()
        && scene.physicsBodies.empty()
        && scene.physicsColliders.empty()
        && scene.instances.empty()
        && scene.materials.empty()
        && scene.images.empty();
}

struct Vec3
{
    float x;
    float y;
    float z;
};

Vec3 positionAt(const xrphoton::SceneData& scene, std::size_t vertex)
{
    return {
        scene.positions[vertex * 3],
        scene.positions[vertex * 3 + 1],
        scene.positions[vertex * 3 + 2],
    };
}

Vec3 subtract(Vec3 left, Vec3 right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 cross(Vec3 left, Vec3 right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

float dot(Vec3 left, Vec3 right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

void testGeneratedYardAsset(
    const std::filesystem::path& path,
    std::string_view name,
    Vec3 expectedMinimum,
    Vec3 expectedMaximum,
    const std::array<float, 4>& expectedColor,
    bool expectedPhysicsRecipe)
{
    const xrphoton::OgfxLoadResult loaded = xrphoton::loadOgfxModel(path);
    expect(static_cast<bool>(loaded),
        std::string("generated ") + std::string(name) + " asset loads");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }

    const xrphoton::SceneData& scene = loaded.scene;
    const bool canonicalShape = scene.positions.size() == 24 * 3
        && scene.attributes.size() == 24
        && scene.indices.size() == 36
        && scene.geometries.size() == 1
        && scene.meshes.size() == 1
        && scene.materials.size() == 1;
    expect(canonicalShape,
        std::string(name) + " has one 24-vertex/36-index box mesh and material");
    if (!canonicalShape) {
        return;
    }

    const xrphoton::SceneGeometry& geometry = scene.geometries[0];
    expect(geometry.firstVertex == 0
            && geometry.vertexCount == 24
            && geometry.firstIndex == 0
            && geometry.indexCount == 36
            && geometry.materialIndex == 0
            && !geometry.alphaTested,
        std::string(name) + " geometry is the canonical opaque box range");
    expect(scene.meshes[0].firstGeometry == 0
            && scene.meshes[0].geometryCount == 1,
        std::string(name) + " owns exactly one geometry");
    expect(scene.instances.empty() && scene.images.empty(),
        std::string(name) + " model contains no placement or runtime image state");
    if (expectedPhysicsRecipe) {
        expect(scene.physicsBodies.size() == 1
                && scene.physicsColliders.size() == 1,
            std::string(name) + " carries one body and one collider recipe");
        if (scene.physicsBodies.size() == 1
            && scene.physicsColliders.size() == 1) {
            const xrphoton::ScenePhysicsBody& body = scene.physicsBodies[0];
            const xrphoton::ScenePhysicsCollider& collider =
                scene.physicsColliders[0];
            expect(body.meshIndex == 0
                    && body.firstCollider == 0
                    && body.colliderCount == 1
                    && body.mass == 30.0f
                    && body.centerOfMass.x == 0.0f
                    && body.centerOfMass.y == 0.0f
                    && body.centerOfMass.z == 0.0f,
                std::string(name) + " body recipe owns its only mesh and collider");
            expect(collider.shape == xrphoton::ScenePhysicsShape::Box
                    && collider.center.x == 0.0f
                    && collider.center.y == 0.0f
                    && collider.center.z == 0.0f
                    && collider.orientation.w == 1.0f
                    && collider.orientation.x == 0.0f
                    && collider.orientation.y == 0.0f
                    && collider.orientation.z == 0.0f
                    && collider.halfExtents.x == 0.5f
                    && collider.halfExtents.y == 0.5f
                    && collider.halfExtents.z == 0.5f
                    && collider.mass == 30.0f
                    && collider.centerOfMass.x == 0.0f
                    && collider.centerOfMass.y == 0.0f
                    && collider.centerOfMass.z == 0.0f
                    && collider.material.empty(),
                std::string(name) + " collider is the complete generated box recipe");
        }
    } else {
        expect(scene.physicsBodies.empty() && scene.physicsColliders.empty(),
            std::string(name) + " intentionally carries no rigid-physics recipe");
    }

    std::vector<std::uint32_t> expectedIndices;
    expectedIndices.reserve(36);
    for (std::uint32_t face = 0; face < 6; ++face) {
        const std::uint32_t first = face * 4;
        expectedIndices.insert(expectedIndices.end(), {
            first, first + 1, first + 2,
            first, first + 2, first + 3,
        });
    }
    expect(scene.indices == expectedIndices,
        std::string(name) + " retains the canonical six-face indexed topology");

    Vec3 minimum{
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
    };
    Vec3 maximum{
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    for (std::size_t vertex = 0; vertex < 24; ++vertex) {
        const Vec3 position = positionAt(scene, vertex);
        minimum.x = std::min(minimum.x, position.x);
        minimum.y = std::min(minimum.y, position.y);
        minimum.z = std::min(minimum.z, position.z);
        maximum.x = std::max(maximum.x, position.x);
        maximum.y = std::max(maximum.y, position.y);
        maximum.z = std::max(maximum.z, position.z);
    }
    expect(minimum.x == expectedMinimum.x
            && minimum.y == expectedMinimum.y
            && minimum.z == expectedMinimum.z
            && maximum.x == expectedMaximum.x
            && maximum.y == expectedMaximum.y
            && maximum.z == expectedMaximum.z,
        std::string(name) + " retains its exact model-space bounds");

    const xrphoton::SceneMaterial& material = scene.materials[0];
    expect(material.baseColorFactor[0] == expectedColor[0]
            && material.baseColorFactor[1] == expectedColor[1]
            && material.baseColorFactor[2] == expectedColor[2]
            && material.baseColorFactor[3] == expectedColor[3]
            && material.baseColorImage == 0
            && material.alphaCutoff == 0.5f
            && material.baseColorTexture.empty(),
        std::string(name) + " retains its untextured opaque material");

    constexpr std::array<std::array<float, 3>, 6> ExpectedNormals{{
        {-1.0f,  0.0f,  0.0f},
        { 1.0f,  0.0f,  0.0f},
        { 0.0f, -1.0f,  0.0f},
        { 0.0f,  1.0f,  0.0f},
        { 0.0f,  0.0f, -1.0f},
        { 0.0f,  0.0f,  1.0f},
    }};
    constexpr std::array<std::array<float, 2>, 4> ExpectedUvs{{
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
    }};
    bool attributesMatch = true;
    for (std::size_t face = 0; face < ExpectedNormals.size(); ++face) {
        for (std::size_t corner = 0; corner < ExpectedUvs.size(); ++corner) {
            const xrphoton::VertexAttributes& attributes =
                scene.attributes[face * 4 + corner];
            attributesMatch = attributesMatch
                && attributes.nx == ExpectedNormals[face][0]
                && attributes.ny == ExpectedNormals[face][1]
                && attributes.nz == ExpectedNormals[face][2]
                && attributes.u == ExpectedUvs[corner][0]
                && attributes.v == ExpectedUvs[corner][1];
        }
    }
    expect(attributesMatch,
        std::string(name) + " retains per-face outward normals and 0..1 UVs");

    const Vec3 center{
        (expectedMinimum.x + expectedMaximum.x) * 0.5f,
        (expectedMinimum.y + expectedMaximum.y) * 0.5f,
        (expectedMinimum.z + expectedMaximum.z) * 0.5f,
    };
    bool windingIsOutward = true;
    for (std::size_t triangle = 0; triangle < scene.indices.size(); triangle += 3) {
        const std::uint32_t index0 = scene.indices[triangle];
        const std::uint32_t index1 = scene.indices[triangle + 1];
        const std::uint32_t index2 = scene.indices[triangle + 2];
        const Vec3 p0 = positionAt(scene, index0);
        const Vec3 p1 = positionAt(scene, index1);
        const Vec3 p2 = positionAt(scene, index2);
        const Vec3 geometricNormal = cross(subtract(p1, p0), subtract(p2, p0));
        const xrphoton::VertexAttributes& declared = scene.attributes[index0];
        const Vec3 declaredNormal{declared.nx, declared.ny, declared.nz};
        const Vec3 centroid{
            (p0.x + p1.x + p2.x) / 3.0f,
            (p0.y + p1.y + p2.y) / 3.0f,
            (p0.z + p1.z + p2.z) / 3.0f,
        };
        windingIsOutward = windingIsOutward
            && dot(geometricNormal, declaredNormal) > 0.0f
            && dot(geometricNormal, subtract(centroid, center)) > 0.0f;
    }
    expect(windingIsOutward,
        std::string(name) + " triangles wind outward on every face");
}

void testGeneratedYardAssets()
{
    testGeneratedYardAsset(
        std::filesystem::path(XRPHOTON_TEST_YARD_GROUND_ASSET_PATH),
        "yard ground",
        {-10.0f, -0.4f, -10.0f},
        { 10.0f,  0.0f,  10.0f},
        {0.42f, 0.42f, 0.45f, 1.0f},
        false);
    testGeneratedYardAsset(
        std::filesystem::path(XRPHOTON_TEST_YARD_WALL_ASSET_PATH),
        "yard wall",
        {-4.0f, 0.0f, -0.15f},
        { 4.0f, 3.0f,  0.15f},
        {0.55f, 0.24f, 0.18f, 1.0f},
        false);
    testGeneratedYardAsset(
        std::filesystem::path(XRPHOTON_TEST_YARD_BOX_ASSET_PATH),
        "yard box",
        {-0.5f, -0.5f, -0.5f},
        { 0.5f,  0.5f,  0.5f},
        {0.80f, 0.62f, 0.22f, 1.0f},
        true);
}

void testSceneConversion()
{
    const xrphoton::ogfx::SerializeResult serialized =
        xrphoton::ogfx::serializeModel(makeQuad(), "loader-source");
    expect(static_cast<bool>(serialized), "runtime-loader source serializes");
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return;
    }

    const xrphoton::OgfxLoadResult loaded =
        xrphoton::decodeOgfxScene(serialized.bytes, "quad.ogfx");
    expect(static_cast<bool>(loaded), "runtime adapter decodes the canonical quad");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }

    constexpr float expectedPositions[]{
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.5f,  0.5f, 0.0f,
        -0.5f,  0.5f, 0.0f,
    };
    expect(
        loaded.scene.positions
            == std::vector<float>(std::begin(expectedPositions), std::end(expectedPositions)),
        "runtime positions are flattened field-by-field");
    constexpr std::array expectedAttributes{
        xrphoton::VertexAttributes{1.0f, 0.0f, 0.0f, -1.0f, 2.0f},
        xrphoton::VertexAttributes{0.0f, 1.0f, 0.0f,  3.0f, 4.0f},
        xrphoton::VertexAttributes{0.0f, 0.0f, 1.0f,  5.0f, 6.0f},
        xrphoton::VertexAttributes{1.0f, 1.0f, 1.0f,  7.0f, 8.0f},
    };
    expect(loaded.scene.attributes.size() == expectedAttributes.size(),
        "runtime owns four attribute records");
    bool attributesMatch = loaded.scene.attributes.size() == expectedAttributes.size();
    for (std::size_t index = 0;
         attributesMatch && index < expectedAttributes.size();
         ++index) {
        const xrphoton::VertexAttributes& actual = loaded.scene.attributes[index];
        const xrphoton::VertexAttributes& expected = expectedAttributes[index];
        attributesMatch = actual.nx == expected.nx
            && actual.ny == expected.ny
            && actual.nz == expected.nz
            && actual.u == expected.u
            && actual.v == expected.v;
    }
    expect(attributesMatch, "runtime attributes retain every normal and UV field");
    expect(loaded.scene.indices == std::vector<std::uint32_t>({0, 1, 2, 0, 2, 3}),
        "runtime owns the six geometry-local indices");
    expect(loaded.scene.geometries.size() == 1
            && loaded.scene.geometries[0].vertexCount == 4
            && loaded.scene.geometries[0].indexCount == 6
            && !loaded.scene.geometries[0].alphaTested,
        "runtime geometry range is reconstructed");
    expect(loaded.scene.meshes.size() == 1
            && loaded.scene.meshes[0].firstGeometry == 0
            && loaded.scene.meshes[0].geometryCount == 1,
        "runtime mesh grouping is reconstructed");
    expect(loaded.scene.materials.size() == 1
            && loaded.scene.materials[0].baseColorFactor[0] == 0.25f
            && loaded.scene.materials[0].baseColorFactor[1] == 0.5f
            && loaded.scene.materials[0].baseColorFactor[2] == 0.75f
            && loaded.scene.materials[0].baseColorFactor[3] == 0.875f
            && loaded.scene.materials[0].baseColorImage == 0
            && loaded.scene.materials[0].baseColorTexture.empty()
            && loaded.scene.materials[0].alphaCutoff == 0.375f,
        "runtime material fields include an empty texture-reference carrier");
    expect(loaded.scene.instances.empty(), "OGFx decoding creates no world instances");
    expect(loaded.scene.images.empty(), "the untextured adapter fixture creates no images");
    expect(loaded.scene.physicsBodies.empty()
            && loaded.scene.physicsColliders.empty(),
        "an OGFx model without a recipe creates no runtime physics records");

    std::vector<std::uint8_t> malformed = serialized.bytes;
    malformed[0] = 'X';
    const xrphoton::OgfxLoadResult rejected =
        xrphoton::decodeOgfxScene(malformed, "bad.ogfx");
    expect(!rejected, "runtime adapter propagates decoder failure");
    expect(sceneIsEmpty(rejected.scene), "runtime adapter exposes no partial SceneData");
    expect(rejected.error.find("bad.ogfx") != std::string::npos,
        "runtime adapter preserves the decoder diagnostic");
}

void testMultiRecordSceneConversion()
{
    const xrphoton::ogfx::SerializeResult serialized =
        xrphoton::ogfx::serializeModel(
            makeOpaqueTwoGeometryModel(),
            "multi-record-loader-source");
    expect(static_cast<bool>(serialized), "multi-record runtime-loader source serializes");
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return;
    }

    const xrphoton::OgfxLoadResult loaded = xrphoton::decodeOgfxScene(
        serialized.bytes,
        "multi-record.ogfx");
    expect(static_cast<bool>(loaded),
        "runtime adapter accepts an opaque multi-record model");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }

    expect(loaded.scene.positions.size() == 18
            && loaded.scene.attributes.size() == 6
            && loaded.scene.indices
                == std::vector<std::uint32_t>({0, 1, 2, 0, 1, 2}),
        "runtime adapter preserves both geometry streams");
    expect(loaded.scene.geometries.size() == 2
            && loaded.scene.geometries[0].firstVertex == 0
            && loaded.scene.geometries[0].materialIndex == 0
            && loaded.scene.geometries[1].firstVertex == 3
            && loaded.scene.geometries[1].firstIndex == 3
            && loaded.scene.geometries[1].materialIndex == 1
            && !loaded.scene.geometries[0].alphaTested
            && !loaded.scene.geometries[1].alphaTested,
        "runtime adapter preserves two opaque geometry records and material indices");
    expect(loaded.scene.meshes.size() == 1
            && loaded.scene.meshes[0].firstGeometry == 0
            && loaded.scene.meshes[0].geometryCount == 2,
        "runtime adapter preserves a multi-geometry mesh range");
    expect(loaded.scene.materials.size() == 2
            && loaded.scene.materials[0].baseColorFactor[2] == 0.75f
            && loaded.scene.materials[1].baseColorFactor[1] == 0.75f
            && loaded.scene.materials[0].baseColorTexture == "ston\\first"
            && loaded.scene.materials[1].baseColorTexture == "ston\\second"
            && loaded.scene.materials[0].baseColorImage == 0
            && loaded.scene.materials[1].baseColorImage == 0,
        "runtime adapter preserves both textured material records before resolution");
    expect(loaded.scene.instances.empty() && loaded.scene.images.empty(),
        "multi-record OGFx decoding still creates no placements or images");
}

void testPhysicsRecipeBoundary()
{
    xrphoton::ogfx::Model model = makeQuad();
    model.physicsBodies.push_back({
        .firstCollider = 0,
        .colliderCount = 2,
        .mass = 17.0f,
        .centerOfMass = {0.25f, -0.5f, 0.75f},
    });
    model.physicsColliders.push_back({
        .shapeType = xrphoton::ogfx::PhysicsShapeType::Box,
        .flags = 0,
        .material = "objects\\tail",
        .sourceNode = "box-source-node-is-not-runtime-data",
        .center = {1.0f, 2.0f, 3.0f},
        .orientation = {0.18257418f, 0.36514837f, 0.54772258f, 0.73029673f},
        .halfExtents = {0.125f, 0.25f, 0.5f},
        .mass = 7.0f,
        .centerOfMass = {-1.0f, -2.0f, -3.0f},
    });
    model.physicsColliders.push_back({
        .shapeType = xrphoton::ogfx::PhysicsShapeType::Cylinder,
        .flags = 0,
        .material = "objects\\barrel",
        .sourceNode = "cylinder-source-node-is-not-runtime-data",
        .center = {-4.0f, 5.0f, -6.0f},
        .axis = {1.0f, 2.0f, 3.0f},
        .height = 1.25f,
        .radius = 0.375f,
        .mass = 10.0f,
        .centerOfMass = {4.0f, -5.0f, 6.0f},
    });

    const xrphoton::ogfx::SerializeResult serialized =
        xrphoton::ogfx::serializeModel(model, "physics-loader-source");
    expect(static_cast<bool>(serialized),
        "runtime-loader source with optional physics metadata serializes");
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return;
    }

    const xrphoton::OgfxLoadResult loaded = xrphoton::decodeOgfxScene(
        serialized.bytes,
        "physics-runtime-boundary.ogfx");
    expect(static_cast<bool>(loaded),
        "runtime adapter accepts a model carrying validated physics recipes");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }
    expect(loaded.scene.positions.size() == 12
            && loaded.scene.attributes.size() == 4
            && loaded.scene.indices.size() == 6
            && loaded.scene.geometries.size() == 1
            && loaded.scene.meshes.size() == 1
            && loaded.scene.materials.size() == 1,
        "physics recipes leave the model's render records unchanged");
    expect(loaded.scene.physicsBodies.size() == 1,
        "runtime adapter copies the body recipe");
    if (loaded.scene.physicsBodies.size() == 1) {
        const xrphoton::ScenePhysicsBody& body = loaded.scene.physicsBodies[0];
        expect(body.meshIndex == 0
                && body.firstCollider == 0
                && body.colliderCount == 2
                && body.mass == 17.0f
                && body.centerOfMass.x == 0.25f
                && body.centerOfMass.y == -0.5f
                && body.centerOfMass.z == 0.75f,
            "body recipe copies ownership, mass, and aggregate center of mass");
    }
    expect(loaded.scene.physicsColliders.size() == 2,
        "runtime adapter copies both box and cylinder collider recipes");
    if (loaded.scene.physicsColliders.size() == 2) {
        const xrphoton::ScenePhysicsCollider& box =
            loaded.scene.physicsColliders[0];
        expect(box.shape == xrphoton::ScenePhysicsShape::Box
                && box.center.x == 1.0f
                && box.center.y == 2.0f
                && box.center.z == 3.0f
                && box.halfExtents.x == 0.125f
                && box.halfExtents.y == 0.25f
                && box.halfExtents.z == 0.5f
                && box.mass == 7.0f
                && box.centerOfMass.x == -1.0f
                && box.centerOfMass.y == -2.0f
                && box.centerOfMass.z == -3.0f
                && box.material == "objects\\tail",
            "box collider copies every runtime-owned scalar and string field");
        expect(box.orientation.x == model.physicsColliders[0].orientation.x
                && box.orientation.y == model.physicsColliders[0].orientation.y
                && box.orientation.z == model.physicsColliders[0].orientation.z
                && box.orientation.w == model.physicsColliders[0].orientation.w,
            "OGFx (x,y,z,w) orientation crosses explicitly into GLM (w,x,y,z)");

        const xrphoton::ScenePhysicsCollider& cylinder =
            loaded.scene.physicsColliders[1];
        expect(cylinder.shape == xrphoton::ScenePhysicsShape::Cylinder
                && cylinder.center.x == -4.0f
                && cylinder.center.y == 5.0f
                && cylinder.center.z == -6.0f
                && cylinder.axis.x == 1.0f
                && cylinder.axis.y == 2.0f
                && cylinder.axis.z == 3.0f
                && cylinder.height == 1.25f
                && cylinder.radius == 0.375f
                && cylinder.mass == 10.0f
                && cylinder.centerOfMass.x == 4.0f
                && cylinder.centerOfMass.y == -5.0f
                && cylinder.centerOfMass.z == 6.0f
                && cylinder.material == "objects\\barrel",
            "cylinder collider copies axis, dimensions, mass, COM, and material");
    }
    expect(loaded.scene.instances.empty() && loaded.scene.images.empty(),
        "physics recipes create neither world placement nor runtime image state");
}

void testMultiMeshPhysicsRecipeRejected()
{
    xrphoton::ogfx::Model model = makeOpaqueTwoGeometryModel();
    model.meshes = {{0, 1}, {1, 1}};
    model.physicsBodies.push_back({
        .firstCollider = 0,
        .colliderCount = 1,
        .mass = 1.0f,
        .centerOfMass = {},
    });
    model.physicsColliders.push_back({
        .shapeType = xrphoton::ogfx::PhysicsShapeType::Box,
        .flags = 0,
        .material = {},
        .sourceNode = {},
        .halfExtents = {0.5f, 0.5f, 0.5f},
        .mass = 1.0f,
    });

    const xrphoton::ogfx::SerializeResult serialized =
        xrphoton::ogfx::serializeModel(model, "multi-mesh-physics-source");
    expect(static_cast<bool>(serialized),
        "a valid OGFx recipe can still describe a model with two meshes");
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return;
    }

    const xrphoton::OgfxLoadResult loaded = xrphoton::decodeOgfxScene(
        serialized.bytes,
        "multi-mesh-physics.ogfx");
    expect(!loaded,
        "runtime adapter rejects ambiguous model-scoped physics ownership across two meshes");
    expect(sceneIsEmpty(loaded.scene),
        "multi-mesh physics rejection publishes no partial SceneData");
    expect(loaded.error.find("rigid-physics mesh ownership") != std::string::npos
            && loaded.error.find("exactly 1 mesh") != std::string::npos,
        "multi-mesh physics diagnostic names the single-mesh ownership rule");
}

void testFileBoundary()
{
    const xrphoton::ogfx::SerializeResult serialized =
        xrphoton::ogfx::serializeModel(makeQuad(), "file-source");
    if (!serialized) {
        expect(false, "file-loader source serializes");
        return;
    }

    const std::filesystem::path path =
        std::filesystem::current_path()
        / std::filesystem::path(u8"xrphoton-ogfx-loader-é-test.ogfx");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(serialized.bytes.data()),
            static_cast<std::streamsize>(serialized.bytes.size()));
        expect(static_cast<bool>(output), "test fixture writes to its temporary file");
    }

    const xrphoton::OgfxLoadResult loaded = xrphoton::loadOgfxModel(path);
    expect(static_cast<bool>(loaded), "filesystem wrapper loads a complete OGFx file");
    expect(loaded.scene.positions.size() == 12, "filesystem wrapper delegates to scene decoding");

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    expect(!removeError, "temporary OGFx fixture is removed");

    const xrphoton::OgfxLoadResult missing = xrphoton::loadOgfxModel(path);
    expect(!missing, "filesystem wrapper rejects a missing file");
    expect(sceneIsEmpty(missing.scene), "missing-file failure has no partial scene");
    expect(missing.error.find("file open") != std::string::npos,
        "missing-file diagnostic identifies the file-open field");
}
}

int main()
{
    testGeneratedYardAssets();
    testSceneConversion();
    testMultiRecordSceneConversion();
    testPhysicsRecipeBoundary();
    testMultiMeshPhysicsRecipeRejected();
    testFileBoundary();

    if (failureCount != 0) {
        std::cerr << failureCount << " OGFx runtime-loader test assertion(s) failed.\n";
        return 1;
    }
    return 0;
}
