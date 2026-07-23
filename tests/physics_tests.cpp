#include "physics.hpp"
#include "physics_test_access.hpp"
#include "scene.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

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

bool matrixBitsEqual(const glm::mat4& left, const glm::mat4& right)
{
    return std::memcmp(&left, &right, sizeof(glm::mat4)) == 0;
}

bool instancesBitsEqual(
    const std::vector<xrphoton::SceneInstance>& left,
    const std::vector<xrphoton::SceneInstance>& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].meshIndex != right[index].meshIndex
            || !matrixBitsEqual(left[index].transform, right[index].transform)) {
            return false;
        }
    }
    return true;
}

glm::mat4 translation(glm::vec3 offset)
{
    return glm::translate(glm::mat4{1.0f}, offset);
}

std::uint32_t addMesh(
    xrphoton::SceneData* scene,
    std::span<const glm::vec3> vertices,
    std::span<const std::uint32_t> indices)
{
    const std::uint32_t firstVertex =
        static_cast<std::uint32_t>(scene->positions.size() / 3);
    const std::uint32_t firstIndex =
        static_cast<std::uint32_t>(scene->indices.size());
    for (const glm::vec3 vertex : vertices) {
        scene->positions.push_back(vertex.x);
        scene->positions.push_back(vertex.y);
        scene->positions.push_back(vertex.z);
    }
    scene->indices.insert(scene->indices.end(), indices.begin(), indices.end());

    const std::uint32_t firstGeometry =
        static_cast<std::uint32_t>(scene->geometries.size());
    scene->geometries.push_back({
        .firstVertex = firstVertex,
        .vertexCount = static_cast<std::uint32_t>(vertices.size()),
        .firstIndex = firstIndex,
        .indexCount = static_cast<std::uint32_t>(indices.size()),
        .materialIndex = 0,
        .alphaTested = false,
    });
    const std::uint32_t meshIndex = static_cast<std::uint32_t>(scene->meshes.size());
    scene->meshes.push_back({
        .firstGeometry = firstGeometry,
        .geometryCount = 1,
    });
    return meshIndex;
}

std::uint32_t addGroundMesh(xrphoton::SceneData* scene, float halfExtent = 10.0f)
{
    const std::array vertices{
        glm::vec3{-halfExtent, 0.0f, -halfExtent},
        glm::vec3{-halfExtent, 0.0f, halfExtent},
        glm::vec3{halfExtent, 0.0f, halfExtent},
        glm::vec3{halfExtent, 0.0f, -halfExtent},
    };
    constexpr std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
    return addMesh(scene, vertices, indices);
}

std::uint32_t addTwoGeometryGroundMesh(xrphoton::SceneData* scene)
{
    const std::uint32_t firstGeometry =
        static_cast<std::uint32_t>(scene->geometries.size());
    const std::array patchStarts{-2.0f, 0.0f};
    constexpr std::array<std::uint32_t, 6> localIndices{0, 1, 2, 0, 2, 3};
    for (const float startX : patchStarts) {
        const std::uint32_t firstVertex =
            static_cast<std::uint32_t>(scene->positions.size() / 3);
        const std::uint32_t firstIndex =
            static_cast<std::uint32_t>(scene->indices.size());
        const std::array vertices{
            glm::vec3{startX, 0.0f, -2.0f},
            glm::vec3{startX, 0.0f, 2.0f},
            glm::vec3{startX + 2.0f, 0.0f, 2.0f},
            glm::vec3{startX + 2.0f, 0.0f, -2.0f},
        };
        for (const glm::vec3 vertex : vertices) {
            scene->positions.push_back(vertex.x);
            scene->positions.push_back(vertex.y);
            scene->positions.push_back(vertex.z);
        }
        scene->indices.insert(
            scene->indices.end(),
            localIndices.begin(),
            localIndices.end());
        scene->geometries.push_back({
            .firstVertex = firstVertex,
            .vertexCount = static_cast<std::uint32_t>(vertices.size()),
            .firstIndex = firstIndex,
            .indexCount = static_cast<std::uint32_t>(localIndices.size()),
            .materialIndex = 0,
            .alphaTested = false,
        });
    }

    const std::uint32_t meshIndex = static_cast<std::uint32_t>(scene->meshes.size());
    scene->meshes.push_back({
        .firstGeometry = firstGeometry,
        .geometryCount = 2,
    });
    return meshIndex;
}

std::uint32_t addClosedBoxMesh(
    xrphoton::SceneData* scene,
    glm::vec3 halfExtents)
{
    const float x = halfExtents.x;
    const float y = halfExtents.y;
    const float z = halfExtents.z;
    const std::array vertices{
        glm::vec3{-x, -y, -z},
        glm::vec3{x, -y, -z},
        glm::vec3{x, y, -z},
        glm::vec3{-x, y, -z},
        glm::vec3{-x, -y, z},
        glm::vec3{x, -y, z},
        glm::vec3{x, y, z},
        glm::vec3{-x, y, z},
    };
    constexpr std::array<std::uint32_t, 36> indices{
        0, 3, 2, 0, 2, 1,
        4, 5, 6, 4, 6, 7,
        0, 4, 7, 0, 7, 3,
        1, 2, 6, 1, 6, 5,
        0, 1, 5, 0, 5, 4,
        3, 7, 6, 3, 6, 2,
    };
    return addMesh(scene, vertices, indices);
}

void addBoxRecipe(
    xrphoton::SceneData* scene,
    std::uint32_t meshIndex,
    glm::vec3 halfExtents,
    float mass = 30.0f,
    glm::vec3 center = {},
    glm::quat orientation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
    glm::vec3 centerOfMass = {})
{
    const std::uint32_t firstCollider =
        static_cast<std::uint32_t>(scene->physicsColliders.size());
    xrphoton::ScenePhysicsCollider collider{};
    collider.shape = xrphoton::ScenePhysicsShape::Box;
    collider.center = center;
    collider.orientation = orientation;
    collider.halfExtents = halfExtents;
    collider.mass = mass;
    collider.centerOfMass = centerOfMass;
    scene->physicsColliders.push_back(std::move(collider));
    scene->physicsBodies.push_back({
        .meshIndex = meshIndex,
        .firstCollider = firstCollider,
        .colliderCount = 1,
        .mass = mass,
        .centerOfMass = centerOfMass,
    });
}

void addCylinderRecipe(
    xrphoton::SceneData* scene,
    std::uint32_t meshIndex,
    std::vector<xrphoton::ScenePhysicsCollider> colliders,
    float mass,
    glm::vec3 centerOfMass)
{
    const std::uint32_t firstCollider =
        static_cast<std::uint32_t>(scene->physicsColliders.size());
    for (xrphoton::ScenePhysicsCollider& collider : colliders) {
        scene->physicsColliders.push_back(std::move(collider));
    }
    scene->physicsBodies.push_back({
        .meshIndex = meshIndex,
        .firstCollider = firstCollider,
        .colliderCount = static_cast<std::uint32_t>(colliders.size()),
        .mass = mass,
        .centerOfMass = centerOfMass,
    });
}

xrphoton::ScenePhysicsCollider cylinderCollider(
    glm::vec3 center,
    glm::vec3 axis,
    float height,
    float radius,
    float mass = 10.0f)
{
    xrphoton::ScenePhysicsCollider collider{};
    collider.shape = xrphoton::ScenePhysicsShape::Cylinder;
    collider.center = center;
    collider.axis = axis;
    collider.height = height;
    collider.radius = radius;
    collider.mass = mass;
    collider.centerOfMass = center;
    return collider;
}

std::size_t addInstance(
    xrphoton::SceneData* scene,
    std::uint32_t meshIndex,
    const glm::mat4& transform)
{
    scene->instances.push_back({meshIndex, transform});
    return scene->instances.size() - 1;
}

struct DynamicScene
{
    xrphoton::SceneData scene;
    std::size_t dynamicIndex = 0;
};

DynamicScene makeSettlingScene(
    glm::vec3 halfExtents = glm::vec3{0.5f},
    glm::mat4 spawn = translation({0.0f, 2.5f, 0.0f}),
    glm::vec3 colliderCenter = {},
    glm::quat colliderOrientation = glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
    glm::vec3 centerOfMass = {})
{
    DynamicScene result{};
    const std::uint32_t ground = addGroundMesh(&result.scene);
    const std::uint32_t box = addClosedBoxMesh(&result.scene, halfExtents);
    addBoxRecipe(
        &result.scene,
        box,
        halfExtents,
        30.0f,
        colliderCenter,
        colliderOrientation,
        centerOfMass);
    addInstance(&result.scene, ground, glm::mat4{1.0f});
    result.dynamicIndex = addInstance(&result.scene, box, spawn);
    return result;
}

DynamicScene makeCylinderScene(glm::vec3 axis, float height, float radius)
{
    DynamicScene result{};
    const std::uint32_t ground = addGroundMesh(&result.scene);
    const std::uint32_t dynamicMesh =
        addClosedBoxMesh(&result.scene, glm::vec3{0.5f});
    addCylinderRecipe(
        &result.scene,
        dynamicMesh,
        {cylinderCollider({}, axis, height, radius)},
        10.0f,
        {});
    addInstance(&result.scene, ground, glm::mat4{1.0f});
    result.dynamicIndex = addInstance(
        &result.scene,
        dynamicMesh,
        translation({0.0f, 1.5f, 0.0f}));
    return result;
}

bool createSingleDynamicWorld(
    xrphoton::PhysicsWorld* world,
    DynamicScene* fixture)
{
    const std::array indices{fixture->dynamicIndex};
    return xrphoton::createPhysicsWorld(world, &fixture->scene, indices);
}

bool expectSingleDynamicCreationFailure(
    DynamicScene fixture,
    std::string_view description)
{
    xrphoton::PhysicsWorld world;
    const bool rejected = !createSingleDynamicWorld(&world, &fixture);
    expect(rejected, description);
    expect(world.state == nullptr, "failed creation leaves its owner empty");
    return rejected && world.state == nullptr;
}

bool stepFrames(
    xrphoton::PhysicsWorld* world,
    std::size_t frameCount,
    float frameDt = xrphoton::PhysicsFixedDt)
{
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        if (!xrphoton::stepPhysics(world, frameDt)) {
            return false;
        }
    }
    return true;
}

xrphoton::SceneData makeCharacterScene(bool withWall)
{
    xrphoton::SceneData scene;
    const std::uint32_t ground = addGroundMesh(&scene, 20.0f);
    addInstance(&scene, ground, glm::mat4{1.0f});
    if (withWall) {
        const std::uint32_t wall = addClosedBoxMesh(
            &scene,
            glm::vec3{0.1f, 1.5f, 2.0f});
        addInstance(
            &scene,
            wall,
            translation({0.0f, 1.5f, 0.0f}));
    }
    return scene;
}

xrphoton::SceneData makeCharacterStepScene()
{
    xrphoton::SceneData scene = makeCharacterScene(false);
    constexpr float StepHeight = 0.4f;
    const std::uint32_t step = addClosedBoxMesh(
        &scene,
        glm::vec3{3.0f, StepHeight * 0.5f, 2.0f});
    addInstance(
        &scene,
        step,
        translation({3.0f, StepHeight * 0.5f, 0.0f}));
    return scene;
}

xrphoton::SceneData makeCharacterCrouchScene()
{
    xrphoton::SceneData scene = makeCharacterScene(false);
    const std::uint32_t ceiling = addClosedBoxMesh(
        &scene,
        glm::vec3{3.0f, 0.1f, 2.0f});
    addInstance(
        &scene,
        ceiling,
        translation({3.0f, 1.4f, 0.0f}));
    return scene;
}

bool createCharacterWorld(
    xrphoton::PhysicsWorld* world,
    xrphoton::SceneData* scene,
    std::array<float, 3> feetPosition)
{
    return xrphoton::createPhysicsWorld(
            world,
            scene,
            std::span<const std::size_t>{})
        && xrphoton::createPhysicsCharacter(world, feetPosition);
}

bool queryCharacter(
    const xrphoton::PhysicsWorld* world,
    std::array<float, 3>* position)
{
    return xrphoton::queryPhysicsCharacterPosition(world, position);
}

bool queryCharacterCrouched(
    const xrphoton::PhysicsWorld* world,
    bool* crouched)
{
    return xrphoton::queryPhysicsCharacterCrouched(world, crouched);
}

void testCharacterContractsAndGrounding()
{
    expect(
        !xrphoton::createPhysicsCharacter(nullptr, {0.0f, 0.0f, 0.0f}),
        "null world rejects character creation");
    xrphoton::PhysicsWorld emptyWorld;
    expect(
        !xrphoton::createPhysicsCharacter(&emptyWorld, {0.0f, 0.0f, 0.0f}),
        "uninitialized world rejects character creation");

    xrphoton::SceneData scene = makeCharacterScene(false);
    xrphoton::PhysicsWorld world;
    expect(
        xrphoton::createPhysicsWorld(
            &world,
            &scene,
            std::span<const std::size_t>{}),
        "static character fixture world is created");

    std::array<float, 3> sentinel{11.0f, 12.0f, 13.0f};
    expect(
        !queryCharacter(&world, &sentinel)
            && sentinel == std::array<float, 3>{11.0f, 12.0f, 13.0f},
        "query before character creation fails without touching output");
    expect(
        !xrphoton::createPhysicsCharacter(
            &world,
            {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}),
        "non-finite character spawn is rejected recoverably");
    expect(
        xrphoton::createPhysicsCharacter(&world, {0.0f, 0.5f, 0.0f}),
        "valid character is created after a rejected spawn");
    expect(
        !xrphoton::createPhysicsCharacter(&world, {0.0f, 0.5f, 0.0f}),
        "a second character is rejected");
    expect(
        !xrphoton::setPhysicsCharacterInput(
            &world,
            {std::numeric_limits<float>::infinity(), 0.0f},
            false),
        "non-finite character movement is rejected");
    expect(
        !queryCharacter(&world, nullptr),
        "null character-position output is rejected");
    expect(
        !queryCharacterCrouched(&world, nullptr),
        "null character-stance output is rejected");

    std::array<float, 3> suspendedStart{};
    expect(
        queryCharacter(&world, &suspendedStart)
            && xrphoton::setPhysicsCharacterEnabled(&world, false)
            && xrphoton::setPhysicsCharacterInput(&world, {3.0f, 0.0f}, true)
            && stepFrames(&world, 30),
        "disabled airborne character ignores movement, gravity and queued jump");
    std::array<float, 3> suspendedEnd{};
    expect(
        queryCharacter(&world, &suspendedEnd)
            && suspendedEnd == suspendedStart,
        "disabled character position is exactly stationary");
    expect(
        xrphoton::setPhysicsCharacterEnabled(&world, true),
        "character resumes after free-camera suspension");

    expect(
        xrphoton::setPhysicsCharacterInput(&world, {0.0f, 0.0f}, false)
            && stepFrames(&world, 120),
        "character falls and settles under fixed-step gravity");
    std::array<float, 3> settled{};
    expect(queryCharacter(&world, &settled), "settled character position is queryable");
    expect(
        std::abs(settled[1]) <= 0.03f,
        "character feet settle on the ground plane");

    expect(
        xrphoton::setPhysicsCharacterInput(&world, {0.0f, 0.0f}, true)
            && xrphoton::setPhysicsCharacterEnabled(&world, false)
            && stepFrames(&world, 2),
        "switching away freezes the character and clears a queued jump");
    std::array<float, 3> queuedJumpFrozen{};
    expect(
        queryCharacter(&world, &queuedJumpFrozen)
            && queuedJumpFrozen == settled,
        "queued jump cannot move a suspended character");
    expect(
        xrphoton::setPhysicsCharacterEnabled(&world, true)
            && xrphoton::setPhysicsCharacterInput(&world, {0.0f, 0.0f}, false)
            && xrphoton::stepPhysics(&world, xrphoton::PhysicsFixedDt),
        "character resumes without replaying the cleared jump");
    std::array<float, 3> resumed{};
    expect(
        queryCharacter(&world, &resumed)
            && resumed[1] <= settled[1] + 0.02f,
        "resuming does not replay a jump requested before suspension");

    expect(
        xrphoton::setPhysicsCharacterInput(&world, {0.0f, 0.0f}, true)
            && xrphoton::stepPhysics(&world, xrphoton::PhysicsFixedDt),
        "grounded character accepts a jump request");
    std::array<float, 3> jumped{};
    expect(queryCharacter(&world, &jumped), "jumped character position is queryable");
    expect(
        jumped[1] > resumed[1] + 0.04f,
        "jump request moves the grounded character upward");
}

void testCharacterMovementAndWallCollision()
{
    xrphoton::SceneData openScene = makeCharacterScene(false);
    xrphoton::PhysicsWorld openWorld;
    expect(
        createCharacterWorld(
            &openWorld,
            &openScene,
            {-5.0f, 0.0f, 0.0f}),
        "open character world is created");
    expect(
        stepFrames(&openWorld, 2)
            && xrphoton::setPhysicsCharacterInput(
                &openWorld,
                {3.0f, 0.0f},
                false),
        "open character receives run velocity");
    std::array<float, 3> runStart{};
    expect(queryCharacter(&openWorld, &runStart), "run start is queryable");
    expect(stepFrames(&openWorld, 60), "character runs for one second");
    std::array<float, 3> runEnd{};
    expect(queryCharacter(&openWorld, &runEnd), "run end is queryable");
    expect(
        nearly(runEnd[0] - runStart[0], 3.0f, 0.08f),
        "three-metre-per-second input moves three metres in one second");

    expect(
        xrphoton::setPhysicsCharacterInput(&openWorld, {0.0f, 0.0f}, false)
            && stepFrames(&openWorld, 60),
        "zero horizontal input stops a grounded character");
    std::array<float, 3> stopped{};
    expect(queryCharacter(&openWorld, &stopped), "stopped position is queryable");
    expect(
        nearly(stopped[0], runEnd[0], 1.0e-3f)
            && nearly(stopped[2], runEnd[2], 1.0e-3f),
        "zero input clears horizontal character motion");

    xrphoton::SceneData wallScene = makeCharacterScene(true);
    xrphoton::PhysicsWorld wallWorld;
    expect(
        createCharacterWorld(
            &wallWorld,
            &wallScene,
            {-2.0f, 0.0f, 0.0f}),
        "wall character world is created");
    expect(
        xrphoton::setPhysicsCharacterInput(&wallWorld, {12.0f, 0.0f}, false)
            && stepFrames(&wallWorld, 120),
        "sprinting character advances into the wall");
    std::array<float, 3> wallStop{};
    expect(queryCharacter(&wallWorld, &wallStop), "wall-stop position is queryable");
    expect(
        wallStop[0] >= -0.52f && wallStop[0] <= -0.43f,
        "capsule stops at the wall face with radius and padding clearance");
    expect(
        std::abs(wallStop[2]) <= 1.0e-3f,
        "head-on wall collision does not introduce sideways drift");
}

void testCharacterStairStep()
{
    xrphoton::SceneData scene = makeCharacterStepScene();
    xrphoton::PhysicsWorld world;
    expect(
        createCharacterWorld(&world, &scene, {-2.0f, 0.0f, 0.0f})
            && stepFrames(&world, 2)
            && xrphoton::setPhysicsCharacterInput(
                &world,
                {3.0f, 0.0f},
                false)
            && stepFrames(&world, 90),
        "character advances toward the configured 0.4-m step");

    std::array<float, 3> position{};
    expect(queryCharacter(&world, &position), "stair-step position is queryable");
    expect(
        position[0] > 1.0f
            && nearly(position[1], 0.4f, 0.04f)
            && std::abs(position[2]) <= 1.0e-3f,
        "character climbs and remains grounded on the 0.4-m step");
}

void testCharacterCrouchAndBlockedStand()
{
    xrphoton::SceneData scene = makeCharacterCrouchScene();
    xrphoton::PhysicsWorld world;
    expect(
        createCharacterWorld(&world, &scene, {-2.0f, 0.0f, 0.0f})
            && stepFrames(&world, 2)
            && xrphoton::setPhysicsCharacterInput(
                &world,
                {3.0f, 0.0f},
                false,
                true)
            && stepFrames(&world, 90),
        "crouched character enters the low-ceiling fixture");

    std::array<float, 3> underCeiling{};
    bool crouched = false;
    expect(
        queryCharacter(&world, &underCeiling)
            && queryCharacterCrouched(&world, &crouched)
            && crouched
            && underCeiling[0] > 0.5f,
        "short capsule moves beneath the 1.3-m ceiling");

    expect(
        xrphoton::setPhysicsCharacterInput(
            &world,
            {0.0f, 0.0f},
            false,
            false)
            && stepFrames(&world, 2)
            && queryCharacterCrouched(&world, &crouched)
            && crouched,
        "standing is blocked while the full-height capsule does not fit");

    expect(
        xrphoton::setPhysicsCharacterInput(
            &world,
            {-3.0f, 0.0f},
            false,
            true)
            && stepFrames(&world, 90)
            && xrphoton::setPhysicsCharacterInput(
                &world,
                {0.0f, 0.0f},
                false,
                false)
            && stepFrames(&world, 2)
            && queryCharacterCrouched(&world, &crouched)
            && !crouched,
        "character stands after crouching back out from under the ceiling");
}

void testCharacterPushStrength()
{
    xrphoton::SceneData scene;
    const std::uint32_t ground = addGroundMesh(&scene, 20.0f);
    const glm::vec3 boxHalfExtents{0.5f};
    const std::uint32_t box = addClosedBoxMesh(&scene, boxHalfExtents);
    addBoxRecipe(&scene, box, boxHalfExtents, 30.0f);
    addInstance(&scene, ground, glm::mat4{1.0f});
    const std::size_t boxInstance = addInstance(
        &scene,
        box,
        translation({0.0f, 0.5f, 0.0f}));

    xrphoton::PhysicsWorld world;
    const std::array dynamicInstances{boxInstance};
    expect(
        xrphoton::createPhysicsWorld(&world, &scene, dynamicInstances)
            && xrphoton::createPhysicsCharacter(&world, {-2.0f, 0.0f, 0.0f})
            && stepFrames(&world, 2)
            && xrphoton::setPhysicsCharacterInput(
                &world,
                {3.0f, 0.0f},
                false)
            && stepFrames(&world, 120),
        "character pushes against a 30-kg dynamic crate");
    expect(
        scene.instances[boxInstance].transform[3].x > 0.5f,
        "configured character strength moves the 30-kg crate");
}

void testSettleAndSleep()
{
    DynamicScene fixture = makeSettlingScene();
    xrphoton::PhysicsWorld world;
    expect(createSingleDynamicWorld(&world, &fixture), "settle world is created");
    if (world.state == nullptr) {
        return;
    }

    expect(stepFrames(&world, 600), "settle world advances for ten seconds");
    const glm::mat4& transform = fixture.scene.instances[fixture.dynamicIndex].transform;
    expect(
        std::abs(transform[3][1] - 0.5f) <= 0.01f,
        "falling crate settles at its half-height above the ground");
    bool active = true;
    expect(
        xrphoton::queryPhysicsBodyActive(&world, fixture.dynamicIndex, &active),
        "settled crate activity can be queried");
    expect(!active, "settled crate sleeps");
    expect(
        xrphoton::setPhysicsBodyLinearVelocity(
            &world,
            fixture.dynamicIndex,
            {0.0f, 0.0f, 0.0f}),
        "setting velocity on a sleeping crate succeeds");
    expect(
        xrphoton::queryPhysicsBodyActive(&world, fixture.dynamicIndex, &active)
            && active,
        "setting velocity explicitly wakes a sleeping body");
}

xrphoton::SceneData makeDegenerateStaticScene()
{
    xrphoton::SceneData scene{};
    const std::array vertices{
        glm::vec3{0.0f, 0.0f, 0.0f},
        glm::vec3{1.0f, 0.0f, 0.0f},
        glm::vec3{2.0f, 0.0f, 0.0f},
    };
    constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
    const std::uint32_t mesh = addMesh(&scene, vertices, indices);
    addInstance(&scene, mesh, glm::mat4{1.0f});
    return scene;
}

void testLifecycleEpochs()
{
    {
        xrphoton::SceneData staticScene{};
        const std::uint32_t ground = addGroundMesh(&staticScene);
        addInstance(&staticScene, ground, glm::mat4{1.0f});
        xrphoton::PhysicsWorld staticWorld;
        expect(
            xrphoton::createPhysicsWorld(
                &staticWorld,
                &staticScene,
                std::span<const std::size_t>{}),
            "empty dynamic set creates a valid static-only world");
        expect(
            xrphoton::stepPhysics(&staticWorld, xrphoton::PhysicsFixedDt),
            "static-only world advances");
    }

    {
        DynamicScene survivorFixture = makeSettlingScene();
        xrphoton::PhysicsWorld survivor;
        expect(
            createSingleDynamicWorld(&survivor, &survivorFixture),
            "first overlapping world is created");

        {
            DynamicScene secondFixture = makeSettlingScene();
            xrphoton::PhysicsWorld second;
            expect(
                createSingleDynamicWorld(&second, &secondFixture),
                "second overlapping world is created");
        }

        xrphoton::SceneData degenerateScene = makeDegenerateStaticScene();
        xrphoton::PhysicsWorld candidate;
        expect(
            !xrphoton::createPhysicsWorld(
                &candidate,
                &degenerateScene,
                std::span<const std::size_t>{}),
            "post-registration degenerate mesh failure rolls back");
        expect(candidate.state == nullptr, "rolled-back candidate remains empty");
        expect(
            xrphoton::stepPhysics(&survivor, xrphoton::PhysicsFixedDt),
            "surviving overlapping world remains usable after rollback");
    }

    DynamicScene freshFixture = makeSettlingScene();
    xrphoton::PhysicsWorld fresh;
    expect(
        createSingleDynamicWorld(&fresh, &freshFixture),
        "world can be created in a later registration epoch");
    expect(
        xrphoton::stepPhysics(&fresh, xrphoton::PhysicsFixedDt),
        "later-epoch world advances");
}

void testNullAndUninitializedContracts()
{
    DynamicScene fixture = makeSettlingScene();
    const std::array dynamicIndices{fixture.dynamicIndex};
    expect(
        !xrphoton::createPhysicsWorld(nullptr, &fixture.scene, dynamicIndices),
        "null world owner is rejected");

    xrphoton::PhysicsWorld world;
    expect(
        !xrphoton::createPhysicsWorld(&world, nullptr, dynamicIndices),
        "null scene is rejected");
    expect(!xrphoton::stepPhysics(nullptr, 0.0f), "null world step is rejected");
    expect(!xrphoton::stepPhysics(&world, 0.0f), "uninitialized world step is rejected");
    expect(
        !xrphoton::setPhysicsBodyLinearVelocity(nullptr, 0, {0.0f, 0.0f, 0.0f}),
        "null world velocity update is rejected");
    expect(
        !xrphoton::setPhysicsBodyLinearVelocity(&world, 0, {0.0f, 0.0f, 0.0f}),
        "uninitialized world velocity update is rejected");
    bool active = true;
    expect(
        !xrphoton::queryPhysicsBodyActive(nullptr, 0, &active) && active,
        "null world query preserves its output");
    expect(
        !xrphoton::queryPhysicsBodyActive(&world, 0, &active) && active,
        "uninitialized world query preserves its output");
}

void testCreationInputContracts()
{
    DynamicScene base = makeSettlingScene();

    {
        DynamicScene fixture = base;
        xrphoton::PhysicsWorld world;
        const std::array duplicate{fixture.dynamicIndex, fixture.dynamicIndex};
        expect(
            !xrphoton::createPhysicsWorld(&world, &fixture.scene, duplicate),
            "duplicate dynamic indices are rejected");
        expect(world.state == nullptr, "duplicate-index failure leaves owner empty");
    }
    {
        DynamicScene fixture = base;
        xrphoton::PhysicsWorld world;
        const std::array outOfRange{fixture.scene.instances.size()};
        expect(
            !xrphoton::createPhysicsWorld(&world, &fixture.scene, outOfRange),
            "out-of-range dynamic index is rejected");
        expect(world.state == nullptr, "out-of-range failure leaves owner empty");
    }

    {
        DynamicScene fixture = base;
        fixture.scene.physicsBodies[0].mass = 0.0f;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "nonpositive body mass is rejected");
    }
    {
        DynamicScene fixture = base;
        fixture.scene.physicsColliders[0].mass =
            std::numeric_limits<float>::quiet_NaN();
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "non-finite collider mass is rejected");
    }
    {
        DynamicScene fixture = base;
        fixture.scene.physicsColliders[0].halfExtents.x = 0.0f;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "zero box half extent is rejected");
    }
    {
        DynamicScene fixture = base;
        fixture.scene.physicsColliders[0].orientation =
            glm::quat{0.5f, 0.0f, 0.0f, 0.0f};
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "non-unit box orientation is rejected");
    }
    {
        DynamicScene fixture = base;
        fixture.scene.physicsColliders[0].shape =
            static_cast<xrphoton::ScenePhysicsShape>(99);
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "unknown collider shape is rejected");
    }
    {
        DynamicScene fixture = base;
        xrphoton::ScenePhysicsCollider& collider = fixture.scene.physicsColliders[0];
        collider.shape = xrphoton::ScenePhysicsShape::Cylinder;
        collider.axis = glm::vec3{0.0f};
        collider.height = 1.0f;
        collider.radius = 0.5f;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "zero cylinder axis is rejected");
    }
    {
        DynamicScene fixture = base;
        fixture.scene.physicsBodies[0].firstCollider = 1;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "nonpartitioning collider range is rejected");
    }
    {
        DynamicScene fixture = base;
        fixture.scene.physicsColliders.clear();
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "body/collider array asymmetry is rejected");
    }

    const std::array invalidTransforms{
        glm::scale(glm::mat4{1.0f}, glm::vec3{-1.0f, 1.0f, 1.0f}),
        glm::scale(glm::mat4{1.0f}, glm::vec3{2.0f, 1.0f, 1.0f}),
        [] {
            glm::mat4 value{1.0f};
            value[1][0] = 0.25f;
            return value;
        }(),
        [] {
            glm::mat4 value{1.0f};
            value[0][3] = 0.1f;
            return value;
        }(),
    };
    constexpr std::array descriptions{
        std::string_view{"reflected dynamic transform is rejected"},
        std::string_view{"scaled dynamic transform is rejected"},
        std::string_view{"sheared dynamic transform is rejected"},
        std::string_view{"projective dynamic transform is rejected"},
    };
    for (std::size_t index = 0; index < invalidTransforms.size(); ++index) {
        DynamicScene fixture = base;
        fixture.scene.instances[fixture.dynamicIndex].transform = invalidTransforms[index];
        expectSingleDynamicCreationFailure(std::move(fixture), descriptions[index]);
    }

    {
        xrphoton::SceneData scene{};
        const std::uint32_t mesh = addGroundMesh(&scene);
        addInstance(
            &scene,
            mesh,
            glm::scale(glm::mat4{1.0f}, glm::vec3{1.0f, 0.0f, 1.0f}));
        xrphoton::PhysicsWorld world;
        expect(
            !xrphoton::createPhysicsWorld(
                &world,
                &scene,
                std::span<const std::size_t>{}),
            "singular static transform is rejected");
        expect(world.state == nullptr, "invalid static transform leaves owner empty");
    }
}

void testNumericSafetyPreflights()
{
    {
        DynamicScene fixture = makeSettlingScene();
        fixture.scene.physicsBodies[0].colliderCount = 65'536;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "compound collider counts are capped before Jolt uint byte-count arithmetic");
    }
    {
        DynamicScene fixture = makeCylinderScene(
            {0.0f, 1.0f, 0.0f},
            std::numeric_limits<float>::denorm_min(),
            0.5f);
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "positive cylinder heights whose half-height underflows are rejected");
    }
    {
        DynamicScene fixture = makeSettlingScene(glm::vec3{1.0e10f});
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "primitive shapes with overflowing inertia are rejected");
    }
    {
        DynamicScene fixture = makeSettlingScene(
            glm::vec3{std::numeric_limits<float>::denorm_min()});
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "primitive shapes with underflowed volume are rejected");
    }
    {
        DynamicScene fixture = makeSettlingScene();
        fixture.scene.instances[fixture.dynamicIndex].transform =
            translation({2.0e15f, 2.5f, 0.0f});
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "huge finite dynamic translations are rejected before broad-phase insertion");
    }
    {
        xrphoton::SceneData scene{};
        const std::uint32_t ground = addGroundMesh(&scene);
        addInstance(&scene, ground, translation({2.0e15f, 0.0f, 0.0f}));
        xrphoton::PhysicsWorld world;
        expect(
            !xrphoton::createPhysicsWorld(
                &world,
                &scene,
                std::span<const std::size_t>{}),
            "huge finite static translations are rejected before shape insertion");
        expect(world.state == nullptr, "huge static translation leaves owner empty");
    }
    {
        xrphoton::SceneData scene{};
        const std::uint32_t ground = addGroundMesh(&scene, 2.0e15f);
        addInstance(&scene, ground, glm::mat4{1.0f});
        xrphoton::PhysicsWorld world;
        expect(
            !xrphoton::createPhysicsWorld(
                &world,
                &scene,
                std::span<const std::size_t>{}),
            "huge finite static mesh bounds are rejected before shape insertion");
        expect(world.state == nullptr, "huge static bounds leave owner empty");
    }
    {
        DynamicScene fixture = makeSettlingScene();
        const float maximum = std::numeric_limits<float>::max();
        fixture.scene.physicsColliders[0].center.x = -maximum;
        fixture.scene.physicsBodies[0].centerOfMass.x = maximum;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "opposite finite centers whose authored COM offset overflows are rejected");
    }
    {
        DynamicScene fixture = makeSettlingScene();
        xrphoton::ScenePhysicsCollider second = fixture.scene.physicsColliders[0];
        fixture.scene.physicsColliders[0].center.x =
            std::numeric_limits<float>::max();
        second.center.x = -std::numeric_limits<float>::max();
        fixture.scene.physicsColliders.push_back(second);
        fixture.scene.physicsBodies[0].colliderCount = 2;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "compound geometric-COM accumulation overflow is rejected before shape creation");
    }
    {
        DynamicScene fixture = makeSettlingScene();
        xrphoton::ScenePhysicsCollider second = fixture.scene.physicsColliders[0];
        fixture.scene.physicsColliders[0].center.x = 1.1e15f;
        second.center.x = -1.1e15f;
        fixture.scene.physicsColliders.push_back(second);
        fixture.scene.physicsBodies[0].colliderCount = 2;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "compound child bounds outside Jolt's range are rejected before tree creation");
    }
    {
        DynamicScene fixture = makeSettlingScene();
        xrphoton::ScenePhysicsCollider& first = fixture.scene.physicsColliders[0];
        first.center.x = 1.0e15f;
        first.halfExtents = glm::vec3{50.0f};
        xrphoton::ScenePhysicsCollider second = first;
        second.center.x = -1.0e15f;
        fixture.scene.physicsColliders.push_back(second);
        fixture.scene.physicsBodies[0].colliderCount = 2;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "compound parallel-axis inertia overflow is rejected before body creation");
    }
    {
        DynamicScene fixture = makeSettlingScene();
        fixture.scene.physicsBodies[0].mass =
            std::numeric_limits<float>::denorm_min();
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "authored masses with non-finite float reciprocals are rejected");
    }
    {
        DynamicScene fixture = makeSettlingScene(glm::vec3{5.0f});
        fixture.scene.physicsBodies[0].mass = 5.0e-39f;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "near-zero scaled inertia is rejected before Jolt's fallback can overflow");
    }
    {
        DynamicScene fixture = makeSettlingScene({1.0e-10f, 1.0f, 1.0f});
        fixture.scene.physicsBodies[0].mass =
            std::numeric_limits<float>::max();
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "authored mass scaling that overflows inertia is rejected");
    }
    {
        DynamicScene fixture = makeSettlingScene({5.0e14f, 5.0e-30f, 5.0e-30f});
        fixture.scene.physicsBodies[0].mass = 1.0e-20f;
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "singular scaled principal inertia is rejected before body creation");
    }
    {
        DynamicScene fixture = makeCylinderScene(
            {0.0f, 1.0f, 0.0f},
            1.8e15f,
            1.0e-15f);
        fixture.scene.instances[fixture.dynamicIndex].transform =
            translation({2.0e14f, 0.0f, 0.0f});
        expectSingleDynamicCreationFailure(
            std::move(fixture),
            "dynamic bounds account conservatively for later body rotation");
    }
}

void testFiniteVelocityClamp()
{
    DynamicScene hugeFixture = makeSettlingScene();
    DynamicScene cappedFixture = hugeFixture;
    xrphoton::PhysicsWorld hugeWorld;
    xrphoton::PhysicsWorld cappedWorld;
    expect(createSingleDynamicWorld(&hugeWorld, &hugeFixture), "huge-velocity world is created");
    expect(createSingleDynamicWorld(&cappedWorld, &cappedFixture), "velocity-clamp control world is created");
    if (hugeWorld.state == nullptr || cappedWorld.state == nullptr) {
        return;
    }

    const std::vector<xrphoton::SceneInstance> snapshot = hugeFixture.scene.instances;
    expect(
        xrphoton::setPhysicsBodyLinearVelocity(
            &hugeWorld,
            hugeFixture.dynamicIndex,
            {std::numeric_limits<float>::max(), 0.0f, 0.0f}),
        "huge finite velocity is accepted and robustly clamped");
    expect(
        instancesBitsEqual(snapshot, hugeFixture.scene.instances),
        "velocity clamping publishes no transform before a physics step");
    expect(
        xrphoton::setPhysicsBodyLinearVelocity(
            &cappedWorld,
            cappedFixture.dynamicIndex,
            {500.0f, 0.0f, 0.0f}),
        "velocity-clamp control receives the explicit maximum");
    expect(
        xrphoton::stepPhysics(&hugeWorld, xrphoton::PhysicsFixedDt)
            && xrphoton::stepPhysics(&cappedWorld, xrphoton::PhysicsFixedDt),
        "clamped and control worlds advance");
    expect(
        matrixNear(
            hugeFixture.scene.instances[hugeFixture.dynamicIndex].transform,
            cappedFixture.scene.instances[cappedFixture.dynamicIndex].transform,
            1.0e-5f),
        "huge finite velocity follows the explicit 500 m/s clamp policy");
}

void testRecoverableOperationContracts()
{
    DynamicScene fixture = makeSettlingScene();
    DynamicScene control = fixture;
    xrphoton::PhysicsWorld world;
    xrphoton::PhysicsWorld controlWorld;
    expect(createSingleDynamicWorld(&world, &fixture), "contract world is created");
    expect(
        createSingleDynamicWorld(&controlWorld, &control),
        "contract control world is created");
    if (world.state == nullptr || controlWorld.state == nullptr) {
        return;
    }

    const std::vector<xrphoton::SceneInstance> snapshot = fixture.scene.instances;
    expect(
        !xrphoton::stepPhysics(&world, -1.0f),
        "negative frame delta is recoverably rejected");
    expect(
        !xrphoton::stepPhysics(&world, std::numeric_limits<float>::infinity()),
        "infinite frame delta is recoverably rejected");
    expect(
        !xrphoton::stepPhysics(
            &world,
            std::numeric_limits<float>::quiet_NaN()),
        "NaN frame delta is recoverably rejected");
    expect(
        !xrphoton::setPhysicsBodyLinearVelocity(
            &world,
            fixture.dynamicIndex,
            {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}),
        "non-finite velocity is recoverably rejected");
    expect(
        !xrphoton::setPhysicsBodyLinearVelocity(
            &world,
            fixture.scene.instances.size(),
            {1.0f, 0.0f, 0.0f}),
        "invalid velocity instance is recoverably rejected");

    bool sentinel = true;
    expect(
        !xrphoton::queryPhysicsBodyActive(
            &world,
            fixture.scene.instances.size(),
            &sentinel)
            && sentinel,
        "invalid activity query preserves sentinel output");
    expect(
        !xrphoton::queryPhysicsBodyActive(
            &world,
            fixture.dynamicIndex,
            nullptr),
        "null activity output is recoverably rejected");

    const std::array dynamicIndices{fixture.dynamicIndex};
    expect(
        !xrphoton::createPhysicsWorld(&world, &fixture.scene, dynamicIndices),
        "initialized owner reuse is rejected");
    expect(
        instancesBitsEqual(snapshot, fixture.scene.instances),
        "recoverable failures publish no scene transforms");

    expect(
        xrphoton::stepPhysics(&world, xrphoton::PhysicsFixedDt),
        "world remains usable after recoverable failures");
    expect(
        xrphoton::stepPhysics(&controlWorld, xrphoton::PhysicsFixedDt),
        "untouched control world advances");
    expect(
        matrixBitsEqual(
            fixture.scene.instances[fixture.dynamicIndex].transform,
            control.scene.instances[control.dynamicIndex].transform),
        "recoverable failures leave accumulator and body state unchanged");
}

void testClampAndAccumulatorBoundary()
{
    {
        DynamicScene largeFixture = makeSettlingScene();
        DynamicScene maxFixture = largeFixture;
        xrphoton::PhysicsWorld largeWorld;
        xrphoton::PhysicsWorld maxWorld;
        expect(createSingleDynamicWorld(&largeWorld, &largeFixture), "large-dt world is created");
        expect(createSingleDynamicWorld(&maxWorld, &maxFixture), "max-dt world is created");
        expect(xrphoton::stepPhysics(&largeWorld, 1000.0f), "large finite dt is accepted");
        expect(
            xrphoton::stepPhysics(&maxWorld, xrphoton::PhysicsMaxFrameDt),
            "maximum frame dt is accepted");
        expect(
            matrixBitsEqual(
                largeFixture.scene.instances[largeFixture.dynamicIndex].transform,
                maxFixture.scene.instances[maxFixture.dynamicIndex].transform),
            "physics clamps large finite dt to PhysicsMaxFrameDt internally");
    }

    {
        DynamicScene boundaryFixture = makeSettlingScene();
        DynamicScene sixStepFixture = boundaryFixture;
        xrphoton::PhysicsWorld boundaryWorld;
        xrphoton::PhysicsWorld sixStepWorld;
        expect(
            createSingleDynamicWorld(&boundaryWorld, &boundaryFixture),
            "accumulator-boundary world is created");
        expect(
            createSingleDynamicWorld(&sixStepWorld, &sixStepFixture),
            "six-step control world is created");
        expect(
            xrphoton::stepPhysics(
                &boundaryWorld,
                std::nextafter(xrphoton::PhysicsFixedDt, 0.0f)),
            "just-below-fixed dt is accepted");
        expect(
            xrphoton::stepPhysics(&boundaryWorld, xrphoton::PhysicsMaxFrameDt),
            "near-step remainder plus max dt stays within catch-up bound");
        expect(stepFrames(&sixStepWorld, 6), "six-step control advances");
        expect(
            matrixBitsEqual(
                boundaryFixture.scene.instances[boundaryFixture.dynamicIndex].transform,
                sixStepFixture.scene.instances[sixStepFixture.dynamicIndex].transform),
            "boundary sequence performs exactly six fixed updates");
    }
}

void testTopologyFailureIsTerminal()
{
    DynamicScene fixture = makeSettlingScene();
    xrphoton::PhysicsWorld world;
    expect(createSingleDynamicWorld(&world, &fixture), "topology-failure world is created");
    if (world.state == nullptr) {
        return;
    }

    fixture.scene.positions.push_back(0.0f);
    expect(
        !xrphoton::stepPhysics(&world, xrphoton::PhysicsFixedDt),
        "position-array topology change is detected");
    fixture.scene.positions.pop_back();

    expect(
        !xrphoton::stepPhysics(&world, xrphoton::PhysicsFixedDt),
        "restoring topology does not revive a terminal world");
    expect(
        !xrphoton::setPhysicsBodyLinearVelocity(
            &world,
            fixture.dynamicIndex,
            {1.0f, 0.0f, 0.0f}),
        "terminal world rejects velocity changes");
    bool sentinel = true;
    expect(
        !xrphoton::queryPhysicsBodyActive(
            &world,
            fixture.dynamicIndex,
            &sentinel)
            && sentinel,
        "terminal world query preserves sentinel output");

    DynamicScene meshFixture = makeSettlingScene();
    xrphoton::PhysicsWorld meshWorld;
    expect(
        createSingleDynamicWorld(&meshWorld, &meshFixture),
        "mesh-reference topology world is created");
    const std::uint32_t originalMesh =
        meshFixture.scene.instances[meshFixture.dynamicIndex].meshIndex;
    meshFixture.scene.instances[meshFixture.dynamicIndex].meshIndex = 0;
    bool active = false;
    expect(
        !xrphoton::queryPhysicsBodyActive(
            &meshWorld,
            meshFixture.dynamicIndex,
            &active),
        "same-size instance mesh-reference mutation is detected");
    meshFixture.scene.instances[meshFixture.dynamicIndex].meshIndex = originalMesh;
    expect(
        !xrphoton::stepPhysics(&meshWorld, 0.0f),
        "restoring a mesh reference does not revive the world");
}

void testModelOriginAndOffCenterBox()
{
    const glm::mat4 spawn = translation({1.0f, 2.0f, -3.0f})
        * glm::rotate(
            glm::mat4{1.0f},
            glm::radians(31.0f),
            glm::vec3{1.0f, 0.0f, 0.0f});
    DynamicScene fixture = makeSettlingScene(
        {0.25f, 0.5f, 0.75f},
        spawn,
        {0.2f, 0.3f, -0.1f},
        glm::angleAxis(glm::radians(90.0f), glm::vec3{0.0f, 1.0f, 0.0f}),
        {0.4f, 0.0f, 0.0f});
    xrphoton::PhysicsWorld world;
    expect(
        createSingleDynamicWorld(&world, &fixture),
        "off-center oriented box world is created");
    expect(xrphoton::stepPhysics(&world, 0.0f), "zero-time physics step succeeds");
    expect(
        matrixNear(
            fixture.scene.instances[fixture.dynamicIndex].transform,
            spawn,
            1.0e-5f),
        "zero-time writeback preserves the authored model-origin pose");

    DynamicScene contactFixture = makeSettlingScene(
        {0.25f, 0.2f, 0.25f},
        translation({0.0f, 1.5f, 0.0f}),
        {0.0f, 0.3f, 0.0f},
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        {});
    xrphoton::PhysicsWorld contactWorld;
    expect(
        createSingleDynamicWorld(&contactWorld, &contactFixture),
        "off-center contact-height world is created");
    expect(stepFrames(&contactWorld, 600), "off-center box settles");
    expect(
        std::abs(
            contactFixture.scene.instances[contactFixture.dynamicIndex].transform[3][1]
            - (-0.1f))
            <= 0.015f,
        "collider-local center controls the model-origin contact height");
}

void testBoxQuaternionOrder()
{
    DynamicScene fixture{};
    const std::uint32_t wall = addClosedBoxMesh(
        &fixture.scene,
        glm::vec3{0.005f, 10.0f, 10.0f});
    const std::uint32_t dynamicMesh = addClosedBoxMesh(
        &fixture.scene,
        glm::vec3{0.1f, 0.1f, 0.4f});
    addBoxRecipe(
        &fixture.scene,
        dynamicMesh,
        {0.1f, 0.1f, 0.4f},
        10.0f,
        {},
        glm::angleAxis(glm::radians(90.0f), glm::vec3{0.0f, 1.0f, 0.0f}));
    addInstance(&fixture.scene, wall, glm::mat4{1.0f});
    fixture.dynamicIndex = addInstance(
        &fixture.scene,
        dynamicMesh,
        translation({-0.6f, 0.0f, 0.0f}));

    xrphoton::PhysicsWorld world;
    expect(createSingleDynamicWorld(&world, &fixture), "quaternion-order world is created");
    expect(
        xrphoton::setPhysicsBodyLinearVelocity(
            &world,
            fixture.dynamicIndex,
            {20.0f, 0.0f, 0.0f}),
        "quaternion-order body receives velocity");
    expect(
        xrphoton::stepPhysics(&world, xrphoton::PhysicsFixedDt),
        "quaternion-order body advances through its wall cast");
    expect(
        fixture.scene.instances[fixture.dynamicIndex].transform[3][0] <= -0.35f,
        "+90-degree Y box maps its long local +Z extent onto world X");
}

void testCylinderAxesAndCompoundChildren()
{
    const float subnormal = std::numeric_limits<float>::denorm_min();
    const std::array axes{
        glm::vec3{7.0f, 0.0f, 0.0f},
        glm::vec3{0.0f, -3.0f, 0.0f},
        glm::vec3{0.0f, 0.0f, subnormal},
    };
    constexpr std::array expectedHeights{0.2f, 0.4f, 0.2f};
    constexpr std::array descriptions{
        std::string_view{"non-unit cylinder axis is normalized"},
        std::string_view{"antiparallel cylinder axis is normalized"},
        std::string_view{"subnormal cylinder axis is normalized without underflow"},
    };
    for (std::size_t index = 0; index < axes.size(); ++index) {
        DynamicScene fixture = makeCylinderScene(axes[index], 0.8f, 0.2f);
        xrphoton::PhysicsWorld world;
        expect(createSingleDynamicWorld(&world, &fixture), descriptions[index]);
        expect(stepFrames(&world, 600), "axis-bridging cylinder settles");
        expect(
            std::abs(
                fixture.scene.instances[fixture.dynamicIndex].transform[3][1]
                - expectedHeights[index])
                <= 0.03f,
            "cylinder orientation produces its expected contact height");
    }

    DynamicScene compound{};
    const std::uint32_t ground = addGroundMesh(&compound.scene);
    const std::uint32_t dynamicMesh =
        addClosedBoxMesh(&compound.scene, glm::vec3{0.5f});
    std::vector<xrphoton::ScenePhysicsCollider> colliders;
    colliders.push_back(cylinderCollider(
        {0.0f, -0.1f, 0.0f},
        {9.0f, 0.0f, 0.0f},
        0.8f,
        0.2f));
    colliders.push_back(cylinderCollider(
        {0.0f, 0.3f, 0.0f},
        {subnormal, 0.0f, 0.0f},
        0.8f,
        0.2f));
    colliders.push_back(cylinderCollider(
        {0.0f, 0.8f, 0.0f},
        {0.0f, -2.0f, 0.0f},
        0.5f,
        0.1f));
    addCylinderRecipe(
        &compound.scene,
        dynamicMesh,
        std::move(colliders),
        30.0f,
        {0.0f, -0.1f, 0.0f});
    addInstance(&compound.scene, ground, glm::mat4{1.0f});
    compound.dynamicIndex = addInstance(
        &compound.scene,
        dynamicMesh,
        translation({0.0f, 1.5f, 0.0f}));

    xrphoton::PhysicsWorld compoundWorld;
    expect(
        createSingleDynamicWorld(&compoundWorld, &compound),
        "multi-axis compound-cylinder world is created");
    expect(stepFrames(&compoundWorld, 600), "compound-cylinder body settles");
    expect(
        std::abs(
            compound.scene.instances[compound.dynamicIndex].transform[3][1]
            - 0.3f)
            <= 0.025f,
        "compound child translations and rotations control contact height");
}

DynamicScene makeComSupportScene(glm::vec3 centerOfMass)
{
    DynamicScene result{};
    const std::uint32_t support = addGroundMesh(&result.scene);
    const std::uint32_t dynamicMesh = addClosedBoxMesh(
        &result.scene,
        glm::vec3{0.25f, 0.5f, 0.25f});
    addBoxRecipe(
        &result.scene,
        dynamicMesh,
        {0.25f, 0.5f, 0.25f},
        10.0f,
        {},
        glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
        centerOfMass);
    addInstance(&result.scene, support, glm::mat4{1.0f});
    result.dynamicIndex = addInstance(
        &result.scene,
        dynamicMesh,
        translation({0.0f, 0.65f, 0.0f}));
    return result;
}

void testAggregateCenterOfMassTorque()
{
    DynamicScene centered = makeComSupportScene({0.0f, 0.0f, 0.0f});
    DynamicScene offset = makeComSupportScene({0.4f, 0.0f, 0.0f});
    xrphoton::PhysicsWorld centeredWorld;
    xrphoton::PhysicsWorld offsetWorld;
    expect(
        createSingleDynamicWorld(&centeredWorld, &centered),
        "centered-COM support world is created");
    expect(
        createSingleDynamicWorld(&offsetWorld, &offset),
        "outside-support-COM world is created");
    expect(stepFrames(&centeredWorld, 120), "centered-COM control advances");
    expect(stepFrames(&offsetWorld, 120), "outside-support-COM body advances");

    const float centeredUp =
        centered.scene.instances[centered.dynamicIndex].transform[1][1];
    const float offsetUp = offset.scene.instances[offset.dynamicIndex].transform[1][1];
    expect(centeredUp > 0.999f, "centered-COM control remains upright");
    expect(offsetUp < 0.9f, "authored COM outside support produces tipping torque");
}

void testGeometryLocalIndexRebasing()
{
    DynamicScene fixture{};
    const std::uint32_t ground = addTwoGeometryGroundMesh(&fixture.scene);
    const std::uint32_t dynamicMesh =
        addClosedBoxMesh(&fixture.scene, glm::vec3{0.2f});
    addBoxRecipe(&fixture.scene, dynamicMesh, glm::vec3{0.2f}, 5.0f);
    addInstance(&fixture.scene, ground, glm::mat4{1.0f});
    fixture.dynamicIndex = addInstance(
        &fixture.scene,
        dynamicMesh,
        translation({1.0f, 1.5f, 0.0f}));

    xrphoton::PhysicsWorld world;
    expect(
        createSingleDynamicWorld(&world, &fixture),
        "two-geometry static mesh world is created");
    expect(stepFrames(&world, 600), "box over second geometry settles");
    expect(
        std::abs(
            fixture.scene.instances[fixture.dynamicIndex].transform[3][1]
            - 0.18f)
            <= 0.015f,
        "second geometry uses geometry-local indices rebased into the Jolt mesh");
}

void testReflectedStaticMesh()
{
    xrphoton::SceneData scene{};
    const std::uint32_t support = addClosedBoxMesh(
        &scene,
        glm::vec3{0.75f, 0.25f, 0.75f});
    const std::uint32_t dynamicMesh =
        addClosedBoxMesh(&scene, glm::vec3{0.2f});
    addBoxRecipe(&scene, dynamicMesh, glm::vec3{0.2f}, 5.0f);

    addInstance(&scene, support, translation({-2.0f, -0.25f, 0.0f}));
    addInstance(
        &scene,
        support,
        translation({2.0f, -0.25f, 0.0f})
            * glm::scale(glm::mat4{1.0f}, glm::vec3{-1.0f, 1.0f, 1.0f}));
    const std::size_t positiveDynamic = addInstance(
        &scene,
        dynamicMesh,
        translation({-2.0f, 1.5f, 0.0f}));
    const std::size_t reflectedDynamic = addInstance(
        &scene,
        dynamicMesh,
        translation({2.0f, 1.5f, 0.0f}));
    const std::array dynamicIndices{positiveDynamic, reflectedDynamic};

    xrphoton::PhysicsWorld world;
    expect(
        xrphoton::createPhysicsWorld(&world, &scene, dynamicIndices),
        "positive and reflected closed-mesh supports are created");
    expect(stepFrames(&world, 600), "boxes settle on both support variants");
    const glm::mat4& positive = scene.instances[positiveDynamic].transform;
    const glm::mat4& reflected = scene.instances[reflectedDynamic].transform;
    expect(
        std::abs(positive[3][1] - reflected[3][1]) <= 0.005f
            && std::abs(positive[3][1] - 0.18f) <= 0.015f,
        "reflected mesh rewind preserves the positive mesh collision surface");
}

void testOpenMeshSidedness()
{
    DynamicScene front = makeSettlingScene(
        glm::vec3{0.05f},
        translation({0.0f, 1.0f, 0.0f}));
    xrphoton::PhysicsWorld frontWorld;
    expect(createSingleDynamicWorld(&frontWorld, &front), "front-face world is created");
    expect(stepFrames(&frontWorld, 300), "front-face falling box advances");
    expect(
        front.scene.instances[front.dynamicIndex].transform[3][1] >= 0.035f,
        "open mesh blocks motion into its authored front face");

    DynamicScene back = makeSettlingScene(
        glm::vec3{0.05f},
        translation({0.0f, -0.1f, 0.0f}));
    xrphoton::PhysicsWorld backWorld;
    expect(createSingleDynamicWorld(&backWorld, &back), "back-face world is created");
    expect(
        xrphoton::setPhysicsBodyLinearVelocity(
            &backWorld,
            back.dynamicIndex,
            {0.0f, 20.0f, 0.0f}),
        "back-face body receives upward velocity");
    expect(
        xrphoton::stepPhysics(&backWorld, xrphoton::PhysicsFixedDt),
        "back-face crossing step succeeds");
    expect(
        back.scene.instances[back.dynamicIndex].transform[3][1] >= 0.20f,
        "open mesh permits passage through its back face");
}

void testStaticMeshPreparationFailures()
{
    {
        xrphoton::SceneData scene = makeDegenerateStaticScene();
        xrphoton::PhysicsWorld world;
        expect(
            !xrphoton::createPhysicsWorld(
                &world,
                &scene,
                std::span<const std::size_t>{}),
            "all-degenerate static mesh is rejected transactionally");
        expect(world.state == nullptr, "degenerate mesh leaves owner empty");
    }
    {
        xrphoton::SceneData scene{};
        const float maximum = std::numeric_limits<float>::max();
        const std::array vertices{
            glm::vec3{maximum, 0.0f, 0.0f},
            glm::vec3{maximum, 1.0f, 0.0f},
            glm::vec3{maximum, 0.0f, 1.0f},
        };
        constexpr std::array<std::uint32_t, 3> indices{0, 1, 2};
        const std::uint32_t mesh = addMesh(&scene, vertices, indices);
        addInstance(
            &scene,
            mesh,
            glm::scale(glm::mat4{1.0f}, glm::vec3{2.0f}));
        xrphoton::PhysicsWorld world;
        expect(
            !xrphoton::createPhysicsWorld(
                &world,
                &scene,
                std::span<const std::size_t>{}),
            "finite static inputs whose multiplication overflows are rejected");
        expect(world.state == nullptr, "bake-overflow failure leaves owner empty");
    }
}

void testDeterminism()
{
    const glm::mat4 spawn = translation({0.0f, 2.5f, 0.0f})
        * glm::rotate(
            glm::mat4{1.0f},
            glm::radians(17.0f),
            glm::vec3{0.0f, 0.0f, 1.0f});
    DynamicScene first = makeSettlingScene(glm::vec3{0.5f}, spawn);
    DynamicScene second = first;
    xrphoton::PhysicsWorld firstWorld;
    xrphoton::PhysicsWorld secondWorld;
    expect(createSingleDynamicWorld(&firstWorld, &first), "first deterministic world is created");
    expect(createSingleDynamicWorld(&secondWorld, &second), "second deterministic world is created");
    if (firstWorld.state == nullptr || secondWorld.state == nullptr) {
        return;
    }

    bool identical = true;
    for (std::size_t frame = 0; frame < 600; ++frame) {
        if (!xrphoton::stepPhysics(&firstWorld, xrphoton::PhysicsFixedDt)
            || !xrphoton::stepPhysics(&secondWorld, xrphoton::PhysicsFixedDt)) {
            identical = false;
            break;
        }
        if (!matrixBitsEqual(
                first.scene.instances[first.dynamicIndex].transform,
                second.scene.instances[second.dynamicIndex].transform)) {
            identical = false;
            break;
        }
    }
    expect(
        identical,
        "identical worlds produce bit-identical transforms through contact and settling");
}

void testTerminalUpdateFailure()
{
    xrphoton::SceneData scene{};
    const std::uint32_t dynamicMesh =
        addClosedBoxMesh(&scene, glm::vec3{0.5f});
    addBoxRecipe(&scene, dynamicMesh, glm::vec3{0.5f}, 10.0f);

    std::vector<std::size_t> dynamicIndices;
    constexpr std::size_t bodyCount = 48;
    dynamicIndices.reserve(bodyCount);
    for (std::size_t index = 0; index < bodyCount; ++index) {
        dynamicIndices.push_back(addInstance(
            &scene,
            dynamicMesh,
            translation({0.0f, 2.0f, 0.0f})));
    }

    xrphoton::PhysicsWorld world;
    expect(
        xrphoton::createPhysicsWorld(&world, &scene, dynamicIndices),
        "overlapping-body capacity fixture is created");
    if (world.state == nullptr) {
        return;
    }
    const std::vector<xrphoton::SceneInstance> snapshot = scene.instances;
    expect(
        !xrphoton::stepPhysics(&world, xrphoton::PhysicsFixedDt),
        "body-pair capacity exhaustion is reported as an update failure");
    expect(
        instancesBitsEqual(snapshot, scene.instances),
        "terminal update failure publishes no transforms");
    expect(
        !xrphoton::stepPhysics(&world, xrphoton::PhysicsFixedDt),
        "terminal update failure forbids retry");
    expect(
        !xrphoton::setPhysicsBodyLinearVelocity(
            &world,
            dynamicIndices.front(),
            {1.0f, 0.0f, 0.0f}),
        "terminal update failure rejects later control calls");
    bool sentinel = true;
    expect(
        !xrphoton::queryPhysicsBodyActive(
            &world,
            dynamicIndices.front(),
            &sentinel)
            && sentinel,
        "terminal update failure query preserves sentinel output");
}

DynamicScene makeCcdScene()
{
    DynamicScene result{};
    const std::uint32_t wall = addClosedBoxMesh(
        &result.scene,
        glm::vec3{0.005f, 10.0f, 10.0f});
    const std::uint32_t dynamicMesh = addClosedBoxMesh(
        &result.scene,
        glm::vec3{0.025f, 0.05f, 0.05f});
    addBoxRecipe(
        &result.scene,
        dynamicMesh,
        {0.025f, 0.05f, 0.05f},
        1.0f);
    addInstance(&result.scene, wall, glm::mat4{1.0f});
    result.dynamicIndex = addInstance(
        &result.scene,
        dynamicMesh,
        translation({-0.1f, 0.0f, 0.0f}));
    return result;
}

void testCcdDiscrimination()
{
    DynamicScene linear = makeCcdScene();
    DynamicScene discrete = linear;
    xrphoton::PhysicsWorld linearWorld;
    xrphoton::PhysicsWorld discreteWorld;
    expect(
        createSingleDynamicWorld(&linearWorld, &linear),
        "LinearCast CCD fixture is created");
    expect(
        createSingleDynamicWorld(&discreteWorld, &discrete),
        "Discrete CCD negative control is created");
    expect(
        xrphoton::setPhysicsBodyMotionQualityForTest(
            &discreteWorld,
            discrete.dynamicIndex,
            xrphoton::PhysicsTestMotionQuality::Discrete),
        "test seam selects Discrete motion quality");
    expect(
        xrphoton::setPhysicsBodyLinearVelocity(
            &linearWorld,
            linear.dynamicIndex,
            {20.0f, 0.0f, 0.0f}),
        "LinearCast body receives tunneling velocity");
    expect(
        xrphoton::setPhysicsBodyLinearVelocity(
            &discreteWorld,
            discrete.dynamicIndex,
            {20.0f, 0.0f, 0.0f}),
        "Discrete body receives tunneling velocity");
    expect(
        xrphoton::stepPhysics(&linearWorld, xrphoton::PhysicsFixedDt),
        "LinearCast CCD fixture advances once");
    expect(
        xrphoton::stepPhysics(&discreteWorld, xrphoton::PhysicsFixedDt),
        "Discrete CCD negative control advances once");

    const float linearX = linear.scene.instances[linear.dynamicIndex].transform[3][0];
    const float discreteX =
        discrete.scene.instances[discrete.dynamicIndex].transform[3][0];
    expect(
        linearX <= -0.02275f,
        "LinearCast stops the thin fast box at the thin wall");
    expect(
        discreteX >= 0.20f,
        "Discrete negative control tunnels completely through the wall");
}
}

int main()
{
    testCharacterContractsAndGrounding();
    testCharacterMovementAndWallCollision();
    testCharacterStairStep();
    testCharacterCrouchAndBlockedStand();
    testCharacterPushStrength();
    testSettleAndSleep();
    testLifecycleEpochs();
    testNullAndUninitializedContracts();
    testCreationInputContracts();
    testNumericSafetyPreflights();
    testFiniteVelocityClamp();
    testRecoverableOperationContracts();
    testClampAndAccumulatorBoundary();
    testTopologyFailureIsTerminal();
    testModelOriginAndOffCenterBox();
    testBoxQuaternionOrder();
    testCylinderAxesAndCompoundChildren();
    testAggregateCenterOfMassTorque();
    testGeometryLocalIndexRebasing();
    testReflectedStaticMesh();
    testOpenMeshSidedness();
    testStaticMeshPreparationFailures();
    testDeterminism();
    testTerminalUpdateFailure();
    testCcdDiscrimination();

    if (failureCount != 0) {
        std::cerr << failureCount << " physics test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "physics tests passed\n";
    return 0;
}
