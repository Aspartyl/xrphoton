#include "physics.hpp"

#include "scene.hpp"

#ifdef XRPHOTON_PHYSICS_TESTING
#include "physics_test_access.hpp"
#endif

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Math/Math.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Body/MotionQuality.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace xrphoton
{
namespace
{
constexpr JPH::ObjectLayer LayerStatic = 0;
constexpr JPH::ObjectLayer LayerMoving = 1;

constexpr JPH::BroadPhaseLayer BroadPhaseStatic{0};
constexpr JPH::BroadPhaseLayer BroadPhaseMoving{1};
constexpr JPH::uint BroadPhaseLayerCount = 2;

constexpr JPH::uint MaximumBodies = 1024;
constexpr JPH::uint BodyMutexCount = 0;
constexpr JPH::uint MaximumBodyPairs = 1024;
constexpr JPH::uint MaximumContactConstraints = 1024;
constexpr std::size_t TempAllocatorBytes = 10 * 1024 * 1024;

constexpr float TransformTolerance = 1.0e-4f;
constexpr float AxisUnitTolerance = 1.0e-5f;
constexpr float DefaultFriction = 0.5f;
constexpr float DefaultRestitution = 0.0f;
constexpr float ConvexRadiusFraction = 0.1f;
constexpr float MaximumLinearVelocity = 500.0f;

// StaticCompoundShape uses uint byte counts for several temporary arrays.
// Keep pathological hand-built recipes comfortably below every multiplication
// and below the signed indexes used while building its tree.
constexpr std::uint32_t MaximumCollidersPerBody = 65'535;

std::once_flag joltHooksOnce;
std::mutex registrationMutex;
std::size_t activeWorldCount = 0;
thread_local bool updateStatusAssertMayReport = false;

class UpdateStatusAssertScope
{
public:
    UpdateStatusAssertScope()
        : previous{updateStatusAssertMayReport}
    {
        updateStatusAssertMayReport = true;
    }

    UpdateStatusAssertScope(const UpdateStatusAssertScope&) = delete;
    UpdateStatusAssertScope& operator=(const UpdateStatusAssertScope&) = delete;

    ~UpdateStatusAssertScope()
    {
        updateStatusAssertMayReport = previous;
    }

private:
    bool previous = false;
};

void traceJolt(const char* format, ...)
{
    std::array<char, 2048> buffer{};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer.data(), buffer.size(), format, arguments);
    va_end(arguments);
    std::cerr << "Jolt: " << buffer.data() << '\n';
}

#ifdef JPH_ENABLE_ASSERTS
bool assertJolt(
    const char* expression,
    const char* message,
    const char* file,
    JPH::uint line)
{
    constexpr std::string_view updateExpression =
        "errors == EPhysicsUpdateError::None";
    constexpr std::string_view updateMessage =
        "An error occurred during the physics update, see EPhysicsUpdateError for more information";
    const std::string_view fileName = file != nullptr ? file : "";
    if (updateStatusAssertMayReport
        && expression != nullptr
        && std::string_view{expression} == updateExpression
        && message != nullptr
        && std::string_view{message} == updateMessage
        && (fileName.ends_with("Jolt/Physics/PhysicsSystem.cpp")
            || fileName.ends_with("Jolt\\Physics\\PhysicsSystem.cpp"))) {
        // Debug Jolt asserts on its documented status return before the caller can
        // inspect it. This one exact assertion is the status channel, not an engine
        // invariant; stepPhysics diagnoses the returned complete mask below.
        return false;
    }

    std::cerr << "Jolt assertion at " << file << ':' << line << ": ("
              << expression << ") " << (message != nullptr ? message : "") << '\n';
    return true;
}
#endif

void installJoltHooks()
{
    JPH::RegisterDefaultAllocator();
    JPH::Trace = traceJolt;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = assertJolt;
#endif
}

void acquireRegistration()
{
    std::call_once(joltHooksOnce, installJoltHooks);

    std::lock_guard lock{registrationMutex};
    if (activeWorldCount == 0) {
        if (JPH::Factory::sInstance != nullptr) {
            throw std::runtime_error("Jolt factory is already owned outside physics");
        }

        std::unique_ptr<JPH::Factory> factory = std::make_unique<JPH::Factory>();
        JPH::Factory::sInstance = factory.get();
        try {
            JPH::RegisterTypes();
        } catch (...) {
            JPH::Factory::sInstance = nullptr;
            throw;
        }
        factory.release();
    }
    ++activeWorldCount;
}

void releaseRegistration() noexcept
{
    std::lock_guard lock{registrationMutex};
    if (activeWorldCount == 0) {
        return;
    }

    --activeWorldCount;
    if (activeWorldCount == 0) {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

class RegistrationLease
{
public:
    RegistrationLease()
    {
        acquireRegistration();
        held = true;
    }

    RegistrationLease(const RegistrationLease&) = delete;
    RegistrationLease& operator=(const RegistrationLease&) = delete;

    ~RegistrationLease() noexcept
    {
        if (held) {
            releaseRegistration();
        }
    }

private:
    bool held = false;
};

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    JPH::uint GetNumBroadPhaseLayers() const override
    {
        return BroadPhaseLayerCount;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return layer == LayerStatic ? BroadPhaseStatic : BroadPhaseMoving;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BroadPhaseStatic ? "STATIC" : "MOVING";
    }
#endif
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override
    {
        if (first == LayerStatic) {
            return second == LayerMoving;
        }
        return first == LayerMoving && (second == LayerStatic || second == LayerMoving);
    }
};

class ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(
        JPH::ObjectLayer objectLayer,
        JPH::BroadPhaseLayer broadPhaseLayer) const override
    {
        if (objectLayer == LayerStatic) {
            return broadPhaseLayer == BroadPhaseMoving;
        }
        return objectLayer == LayerMoving
            && (broadPhaseLayer == BroadPhaseStatic
                || broadPhaseLayer == BroadPhaseMoving);
    }
};

unsigned workerCount()
{
    const unsigned hardwareThreads = std::thread::hardware_concurrency();
    return hardwareThreads <= 1 ? 0 : std::min(2u, hardwareThreads - 1);
}

bool finite(float value)
{
    return std::isfinite(value);
}

bool finiteVec3(const glm::vec3& value)
{
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finiteJoltVec3(JPH::Vec3Arg value)
{
    return finite(value.GetX())
        && finite(value.GetY())
        && finite(value.GetZ());
}

bool coordinateInJoltRange(double value)
{
    return std::isfinite(value)
        && std::abs(value) <= static_cast<double>(JPH::cLargeFloat);
}

bool vec3InJoltRange(JPH::Vec3Arg value)
{
    return coordinateInJoltRange(value.GetX())
        && coordinateInJoltRange(value.GetY())
        && coordinateInJoltRange(value.GetZ());
}

bool validJoltBounds(const JPH::AABox& bounds)
{
    return bounds.IsValid()
        && vec3InJoltRange(bounds.mMin)
        && vec3InJoltRange(bounds.mMax);
}

bool finiteMassProperties(const JPH::MassProperties& properties)
{
    if (!finite(properties.mMass) || properties.mMass <= 0.0f) {
        return false;
    }
    for (JPH::uint column = 0; column < 4; ++column) {
        const JPH::Vec4 values = properties.mInertia.GetColumn4(column);
        if (!finite(values.GetX())
            || !finite(values.GetY())
            || !finite(values.GetZ())
            || !finite(values.GetW())) {
            return false;
        }
    }
    return true;
}

bool finiteMatrix(const glm::mat4& matrix)
{
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            if (!finite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool affineRow(const glm::mat4& matrix)
{
    return std::abs(matrix[0][3]) <= TransformTolerance
        && std::abs(matrix[1][3]) <= TransformTolerance
        && std::abs(matrix[2][3]) <= TransformTolerance
        && std::abs(matrix[3][3] - 1.0f) <= TransformTolerance;
}

double linearDeterminant(const glm::mat4& matrix)
{
    const double m00 = matrix[0][0];
    const double m01 = matrix[1][0];
    const double m02 = matrix[2][0];
    const double m10 = matrix[0][1];
    const double m11 = matrix[1][1];
    const double m12 = matrix[2][1];
    const double m20 = matrix[0][2];
    const double m21 = matrix[1][2];
    const double m22 = matrix[2][2];
    return m00 * (m11 * m22 - m12 * m21)
        - m01 * (m10 * m22 - m12 * m20)
        + m02 * (m10 * m21 - m11 * m20);
}

bool finiteAffineNonsingular(const glm::mat4& matrix)
{
    if (!finiteMatrix(matrix) || !affineRow(matrix)) {
        return false;
    }
    const double determinant = linearDeterminant(matrix);
    return std::isfinite(determinant) && determinant != 0.0;
}

bool properRigidTransform(const glm::mat4& matrix)
{
    if (!finiteAffineNonsingular(matrix)) {
        return false;
    }

    for (std::size_t column = 0; column < 3; ++column) {
        const glm::vec3 basis{matrix[column]};
        if (std::abs(glm::dot(basis, basis) - 1.0f) > TransformTolerance) {
            return false;
        }
        for (std::size_t other = column + 1; other < 3; ++other) {
            const glm::vec3 otherBasis{matrix[other]};
            if (std::abs(glm::dot(basis, otherBasis)) > TransformTolerance) {
                return false;
            }
        }
    }

    return std::abs(linearDeterminant(matrix) - 1.0) <= TransformTolerance;
}

bool checkedRange(std::size_t first, std::size_t count, std::size_t size)
{
    return first <= size && count <= size - first;
}

JPH::Vec3 toJoltVec3(const glm::vec3& value)
{
    return JPH::Vec3(value.x, value.y, value.z);
}

JPH::RVec3 toJoltPosition(const glm::vec3& value)
{
    return JPH::RVec3(value.x, value.y, value.z);
}

JPH::Quat normalizedJoltQuat(const glm::quat& value)
{
    const double length = std::sqrt(
        static_cast<double>(value.x) * value.x
        + static_cast<double>(value.y) * value.y
        + static_cast<double>(value.z) * value.z
        + static_cast<double>(value.w) * value.w);
    return JPH::Quat(
        static_cast<float>(value.x / length),
        static_cast<float>(value.y / length),
        static_cast<float>(value.z / length),
        static_cast<float>(value.w / length));
}

JPH::Quat transformRotation(const glm::mat4& transform)
{
    return normalizedJoltQuat(glm::quat_cast(glm::mat3{transform}));
}

glm::mat4 toGlmMat4(JPH::RMat44Arg source)
{
    const JPH::Vec3 axisX = source.GetAxisX();
    const JPH::Vec3 axisY = source.GetAxisY();
    const JPH::Vec3 axisZ = source.GetAxisZ();
    const JPH::RVec3 translation = source.GetTranslation();

    glm::mat4 result{1.0f};
    result[0] = glm::vec4{axisX.GetX(), axisX.GetY(), axisX.GetZ(), 0.0f};
    result[1] = glm::vec4{axisY.GetX(), axisY.GetY(), axisY.GetZ(), 0.0f};
    result[2] = glm::vec4{axisZ.GetX(), axisZ.GetY(), axisZ.GetZ(), 0.0f};
    result[3] = glm::vec4{
        static_cast<float>(translation.GetX()),
        static_cast<float>(translation.GetY()),
        static_cast<float>(translation.GetZ()),
        1.0f};
    return result;
}

bool reportShapeError(
    const JPH::ShapeSettings::ShapeResult& result,
    std::string_view owner)
{
    std::cerr << "Physics: failed to create " << owner << " shape";
    if (result.HasError()) {
        std::cerr << ": " << result.GetError();
    }
    std::cerr << ".\n";
    return false;
}

bool validateCollider(const ScenePhysicsCollider& collider, std::size_t index)
{
    if (!finiteVec3(collider.center)
        || !finiteVec3(collider.centerOfMass)
        || !finite(collider.mass)
        || collider.mass <= 0.0f) {
        std::cerr << "Physics: collider[" << index
                  << "] has invalid common fields.\n";
        return false;
    }

    switch (collider.shape) {
    case ScenePhysicsShape::Cylinder: {
        if (!finiteVec3(collider.axis)
            || !finite(collider.height)
            || collider.height <= 0.0f
            || !finite(collider.radius)
            || collider.radius <= 0.0f) {
            std::cerr << "Physics: cylinder collider[" << index
                      << "] has invalid dimensions or axis.\n";
            return false;
        }
        const double axisLengthSquared =
            static_cast<double>(collider.axis.x) * collider.axis.x
            + static_cast<double>(collider.axis.y) * collider.axis.y
            + static_cast<double>(collider.axis.z) * collider.axis.z;
        if (!std::isfinite(axisLengthSquared) || axisLengthSquared <= 0.0) {
            std::cerr << "Physics: cylinder collider[" << index
                      << "] has a zero or unrepresentable axis.\n";
            return false;
        }
        return true;
    }
    case ScenePhysicsShape::Box: {
        if (!finiteVec3(collider.halfExtents)
            || collider.halfExtents.x <= 0.0f
            || collider.halfExtents.y <= 0.0f
            || collider.halfExtents.z <= 0.0f
            || !finite(collider.orientation.x)
            || !finite(collider.orientation.y)
            || !finite(collider.orientation.z)
            || !finite(collider.orientation.w)) {
            std::cerr << "Physics: box collider[" << index
                      << "] has invalid extents or orientation.\n";
            return false;
        }
        const double quaternionLengthSquared =
            static_cast<double>(collider.orientation.x) * collider.orientation.x
            + static_cast<double>(collider.orientation.y) * collider.orientation.y
            + static_cast<double>(collider.orientation.z) * collider.orientation.z
            + static_cast<double>(collider.orientation.w) * collider.orientation.w;
        if (!std::isfinite(quaternionLengthSquared)
            || std::abs(quaternionLengthSquared - 1.0) > TransformTolerance) {
            std::cerr << "Physics: box collider[" << index
                      << "] orientation is not normalized.\n";
            return false;
        }
        return true;
    }
    default:
        std::cerr << "Physics: collider[" << index << "] has an unknown shape.\n";
        return false;
    }
}

const ScenePhysicsBody* uniqueBodyForMesh(
    const SceneData& scene,
    std::size_t meshIndex)
{
    const ScenePhysicsBody* result = nullptr;
    for (const ScenePhysicsBody& body : scene.physicsBodies) {
        if (body.meshIndex == meshIndex) {
            if (result != nullptr) {
                return nullptr;
            }
            result = &body;
        }
    }
    return result;
}

bool validateSceneForPhysics(
    const SceneData& scene,
    std::span<const std::size_t> dynamicInstances,
    std::vector<bool>* dynamicMask)
{
    if (scene.instances.size() > MaximumBodies) {
        std::cerr << "Physics: scene has " << scene.instances.size()
                  << " bodies, exceeding the capacity of " << MaximumBodies << ".\n";
        return false;
    }
    if ((scene.positions.size() % 3) != 0) {
        std::cerr << "Physics: position scalar count is not divisible by three.\n";
        return false;
    }

    dynamicMask->assign(scene.instances.size(), false);
    for (std::size_t index : dynamicInstances) {
        if (index >= scene.instances.size()) {
            std::cerr << "Physics: dynamic instance index " << index
                      << " is out of range.\n";
            return false;
        }
        if ((*dynamicMask)[index]) {
            std::cerr << "Physics: dynamic instance index " << index
                      << " is duplicated.\n";
            return false;
        }
        (*dynamicMask)[index] = true;
    }

    if (scene.physicsBodies.empty() != scene.physicsColliders.empty()) {
        std::cerr << "Physics: body and collider arrays must both be empty or nonempty.\n";
        return false;
    }

    std::size_t expectedCollider = 0;
    for (std::size_t index = 0; index < scene.physicsBodies.size(); ++index) {
        const ScenePhysicsBody& body = scene.physicsBodies[index];
        if (body.meshIndex >= scene.meshes.size()
            || body.colliderCount == 0
            || body.colliderCount > MaximumCollidersPerBody
            || body.firstCollider != expectedCollider
            || !checkedRange(
                body.firstCollider,
                body.colliderCount,
                scene.physicsColliders.size())
            || !finite(body.mass)
            || body.mass <= 0.0f
            || !finiteVec3(body.centerOfMass)) {
            std::cerr << "Physics: body recipe[" << index << "] is invalid.\n";
            return false;
        }
        expectedCollider += body.colliderCount;
    }
    if (expectedCollider != scene.physicsColliders.size()) {
        std::cerr << "Physics: body recipes do not partition the collider array.\n";
        return false;
    }
    for (std::size_t index = 0; index < scene.physicsColliders.size(); ++index) {
        if (!validateCollider(scene.physicsColliders[index], index)) {
            return false;
        }
    }

    const std::size_t vertexCount = scene.positions.size() / 3;
    for (std::size_t index = 0; index < scene.geometries.size(); ++index) {
        const SceneGeometry& geometry = scene.geometries[index];
        if (!checkedRange(geometry.firstVertex, geometry.vertexCount, vertexCount)
            || !checkedRange(geometry.firstIndex, geometry.indexCount, scene.indices.size())
            || (geometry.indexCount % 3) != 0) {
            std::cerr << "Physics: geometry[" << index << "] has invalid ranges.\n";
            return false;
        }
        for (std::size_t slot = 0; slot < geometry.indexCount; ++slot) {
            if (scene.indices[geometry.firstIndex + slot] >= geometry.vertexCount) {
                std::cerr << "Physics: geometry[" << index
                          << "] has an out-of-range local index.\n";
                return false;
            }
        }
    }
    for (std::size_t index = 0; index < scene.meshes.size(); ++index) {
        const SceneMesh& mesh = scene.meshes[index];
        if (!checkedRange(
                mesh.firstGeometry,
                mesh.geometryCount,
                scene.geometries.size())) {
            std::cerr << "Physics: mesh[" << index << "] has an invalid geometry range.\n";
            return false;
        }
    }

    for (std::size_t index = 0; index < scene.instances.size(); ++index) {
        const SceneInstance& instance = scene.instances[index];
        if (instance.meshIndex >= scene.meshes.size()) {
            std::cerr << "Physics: instance[" << index
                      << "] has an out-of-range mesh reference.\n";
            return false;
        }
        if (!finiteAffineNonsingular(instance.transform)) {
            std::cerr << "Physics: instance[" << index
                      << "] transform is not finite, affine and nonsingular.\n";
            return false;
        }
        if ((*dynamicMask)[index]) {
            if (!properRigidTransform(instance.transform)) {
                std::cerr << "Physics: dynamic instance[" << index
                          << "] transform is not a proper rigid transform.\n";
                return false;
            }
            if (uniqueBodyForMesh(scene, instance.meshIndex) == nullptr) {
                std::cerr << "Physics: dynamic instance[" << index
                          << "] must have exactly one body recipe.\n";
                return false;
            }
        }
    }
    return true;
}

bool normalizedCylinderAxis(const glm::vec3& source, JPH::Vec3* result)
{
    const double x = source.x;
    const double y = source.y;
    const double z = source.z;
    const double maximum = std::max({std::abs(x), std::abs(y), std::abs(z)});
    if (!std::isfinite(maximum) || maximum <= 0.0) {
        return false;
    }

    const double scaledX = x / maximum;
    const double scaledY = y / maximum;
    const double scaledZ = z / maximum;
    const double length = std::sqrt(
        scaledX * scaledX + scaledY * scaledY + scaledZ * scaledZ);
    if (!std::isfinite(length) || length <= 0.0) {
        return false;
    }

    const std::array<float, 3> normalized{
        static_cast<float>(scaledX / length),
        static_cast<float>(scaledY / length),
        static_cast<float>(scaledZ / length),
    };
    if (!finite(normalized[0]) || !finite(normalized[1]) || !finite(normalized[2])) {
        return false;
    }
    const double floatLengthSquared =
        static_cast<double>(normalized[0]) * normalized[0]
        + static_cast<double>(normalized[1]) * normalized[1]
        + static_cast<double>(normalized[2]) * normalized[2];
    if (std::abs(floatLengthSquared - 1.0) > AxisUnitTolerance) {
        return false;
    }

    *result = JPH::Vec3{normalized[0], normalized[1], normalized[2]};
    return true;
}

bool checkedFloat(double value, float* result)
{
    if (!std::isfinite(value)
        || std::abs(value) > std::numeric_limits<float>::max()) {
        return false;
    }
    *result = static_cast<float>(value);
    return finite(*result);
}

bool validateRawShapeProperties(
    const JPH::Shape& shape,
    std::string_view owner)
{
    if (!finiteMassProperties(shape.GetMassProperties())) {
        std::cerr << "Physics: " << owner
                  << " shape has non-finite or nonpositive mass properties.\n";
        return false;
    }
    return true;
}

bool createPrimitiveShape(
    const ScenePhysicsCollider& collider,
    std::size_t colliderIndex,
    JPH::ShapeRefC* shape,
    JPH::Quat* orientation)
{
    JPH::ShapeSettings::ShapeResult result;
    if (collider.shape == ScenePhysicsShape::Box) {
        const float smallestHalfDimension = std::min({
            collider.halfExtents.x,
            collider.halfExtents.y,
            collider.halfExtents.z,
        });
        const float convexRadius = std::min(
            JPH::cDefaultConvexRadius,
            ConvexRadiusFraction * smallestHalfDimension);
        JPH::BoxShapeSettings settings{toJoltVec3(collider.halfExtents), convexRadius};
        result = settings.Create();
        *orientation = normalizedJoltQuat(collider.orientation);
    } else {
        JPH::Vec3 axis;
        if (!normalizedCylinderAxis(collider.axis, &axis)) {
            std::cerr << "Physics: cylinder collider[" << colliderIndex
                      << "] axis failed robust normalization.\n";
            return false;
        }
        const float halfHeight = collider.height * 0.5f;
        if (!finite(halfHeight) || halfHeight <= 0.0f) {
            std::cerr << "Physics: cylinder collider[" << colliderIndex
                      << "] half-height underflows or overflows float.\n";
            return false;
        }
        const float convexRadius = std::min(
            JPH::cDefaultConvexRadius,
            ConvexRadiusFraction * std::min(halfHeight, collider.radius));
        JPH::CylinderShapeSettings settings{halfHeight, collider.radius, convexRadius};
        result = settings.Create();
        *orientation = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), axis);
    }

    if (!result.IsValid()) {
        return reportShapeError(result, "primitive");
    }
    *shape = result.Get();
    return validateRawShapeProperties(**shape, "primitive");
}

struct DynamicChild
{
    JPH::ShapeRefC shape;
    JPH::Quat orientation;
    JPH::Vec3 positionCom;
    float mass = 0.0f;
};

bool preflightCompoundCenter(
    std::span<const DynamicChild> children,
    JPH::Vec3* centerOfMass)
{
    float totalMass = 0.0f;
    std::array<float, 3> weightedSum{};

    for (const DynamicChild& child : children) {
        float nextMass;
        if (!checkedFloat(
                static_cast<double>(totalMass) + child.mass,
                &nextMass)) {
            std::cerr << "Physics: compound child masses overflow float.\n";
            return false;
        }
        totalMass = nextMass;

        const std::array components{
            child.positionCom.GetX(),
            child.positionCom.GetY(),
            child.positionCom.GetZ(),
        };
        for (std::size_t axis = 0; axis < components.size(); ++axis) {
            float weighted;
            float accumulated;
            if (!checkedFloat(
                    static_cast<double>(components[axis]) * child.mass,
                    &weighted)
                || !checkedFloat(
                    static_cast<double>(weightedSum[axis]) + weighted,
                    &accumulated)) {
                std::cerr << "Physics: compound center-of-mass accumulation "
                             "overflows float.\n";
                return false;
            }
            weightedSum[axis] = accumulated;
        }
    }

    std::array<float, 3> components{};
    for (std::size_t axis = 0; axis < components.size(); ++axis) {
        if (!checkedFloat(
                static_cast<double>(weightedSum[axis]) / totalMass,
                &components[axis])) {
            std::cerr << "Physics: compound geometric center of mass is invalid.\n";
            return false;
        }
    }
    *centerOfMass = JPH::Vec3{components[0], components[1], components[2]};
    return true;
}

bool validateCompoundChildBounds(
    std::span<const DynamicChild> children,
    JPH::Vec3Arg compoundCenter)
{
    for (const DynamicChild& child : children) {
        std::array<float, 3> adjustedComponents{};
        const std::array childComponents{
            child.positionCom.GetX(),
            child.positionCom.GetY(),
            child.positionCom.GetZ(),
        };
        const std::array centerComponents{
            compoundCenter.GetX(),
            compoundCenter.GetY(),
            compoundCenter.GetZ(),
        };
        for (std::size_t axis = 0; axis < adjustedComponents.size(); ++axis) {
            if (!checkedFloat(
                    static_cast<double>(childComponents[axis])
                        - centerComponents[axis],
                    &adjustedComponents[axis])) {
                std::cerr << "Physics: compound child offset overflows float.\n";
                return false;
            }
        }

        const JPH::Vec3 adjustedPosition{
            adjustedComponents[0],
            adjustedComponents[1],
            adjustedComponents[2],
        };
        const JPH::AABox bounds = child.shape->GetWorldSpaceBounds(
            JPH::Mat44::sRotationTranslation(
                child.orientation,
                adjustedPosition),
            JPH::Vec3::sOne());
        if (!validJoltBounds(bounds)) {
            std::cerr << "Physics: compound child has invalid or "
                         "out-of-range local bounds.\n";
            return false;
        }
    }
    return true;
}

bool createDynamicShape(
    const SceneData& scene,
    const ScenePhysicsBody& recipe,
    JPH::ShapeRefC* shape)
{
    if (recipe.colliderCount == 1) {
        const std::size_t colliderIndex = recipe.firstCollider;
        const ScenePhysicsCollider& collider = scene.physicsColliders[colliderIndex];
        JPH::ShapeRefC primitive;
        JPH::Quat orientation;
        if (!createPrimitiveShape(
                collider,
                colliderIndex,
                &primitive,
                &orientation)) {
            return false;
        }

        JPH::RotatedTranslatedShapeSettings settings{
            toJoltVec3(collider.center),
            orientation,
            primitive.GetPtr(),
        };
        JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (!result.IsValid()) {
            return reportShapeError(result, "rotated-translated dynamic");
        }
        *shape = result.Get();
    } else {
        std::vector<DynamicChild> children;
        children.reserve(recipe.colliderCount);
        for (std::size_t offset = 0; offset < recipe.colliderCount; ++offset) {
            const std::size_t colliderIndex = recipe.firstCollider + offset;
            const ScenePhysicsCollider& collider = scene.physicsColliders[colliderIndex];
            JPH::ShapeRefC primitive;
            JPH::Quat orientation;
            if (!createPrimitiveShape(
                    collider,
                    colliderIndex,
                    &primitive,
                    &orientation)) {
                return false;
            }

            const JPH::Vec3 positionCom = toJoltVec3(collider.center)
                + orientation * primitive->GetCenterOfMass();
            if (!finiteJoltVec3(positionCom)) {
                std::cerr << "Physics: compound child center overflows float.\n";
                return false;
            }
            children.push_back({
                .shape = primitive,
                .orientation = orientation,
                .positionCom = positionCom,
                .mass = primitive->GetMassProperties().mMass,
            });
        }

        JPH::Vec3 compoundCenter;
        if (!preflightCompoundCenter(children, &compoundCenter)
            || !validateCompoundChildBounds(children, compoundCenter)) {
            return false;
        }

        JPH::StaticCompoundShapeSettings settings;
        for (std::size_t offset = 0; offset < children.size(); ++offset) {
            const ScenePhysicsCollider& collider =
                scene.physicsColliders[recipe.firstCollider + offset];
            settings.AddShape(
                toJoltVec3(collider.center),
                children[offset].orientation,
                children[offset].shape.GetPtr());
        }

        JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (!result.IsValid()) {
            return reportShapeError(result, "compound dynamic");
        }
        *shape = result.Get();
    }

    const JPH::Vec3 geometricCenter = (*shape)->GetCenterOfMass();
    if (!finiteJoltVec3(geometricCenter)) {
        std::cerr << "Physics: dynamic shape has an invalid geometric center of mass.\n";
        return false;
    }

    const std::array geometricComponents{
        geometricCenter.GetX(),
        geometricCenter.GetY(),
        geometricCenter.GetZ(),
    };
    const std::array authoredComponents{
        recipe.centerOfMass.x,
        recipe.centerOfMass.y,
        recipe.centerOfMass.z,
    };
    std::array<float, 3> offsetComponents{};
    for (std::size_t axis = 0; axis < offsetComponents.size(); ++axis) {
        if (!checkedFloat(
                static_cast<double>(authoredComponents[axis])
                    - geometricComponents[axis],
                &offsetComponents[axis])) {
            std::cerr << "Physics: authored center-of-mass offset overflows float.\n";
            return false;
        }
    }
    const JPH::Vec3 offset{
        offsetComponents[0],
        offsetComponents[1],
        offsetComponents[2],
    };
    if (offset.GetX() != 0.0f || offset.GetY() != 0.0f || offset.GetZ() != 0.0f) {
        JPH::OffsetCenterOfMassShapeSettings settings{offset, shape->GetPtr()};
        JPH::ShapeSettings::ShapeResult result = settings.Create();
        if (!result.IsValid()) {
            return reportShapeError(result, "center-of-mass offset");
        }
        *shape = result.Get();
    }

    const JPH::Vec3 finalCenter = (*shape)->GetCenterOfMass();
    const JPH::AABox localBounds = (*shape)->GetLocalBounds();
    if (!finiteJoltVec3(finalCenter) || !validJoltBounds(localBounds)) {
        std::cerr << "Physics: dynamic shape has invalid or out-of-range "
                     "local geometry.\n";
        return false;
    }
    return validateRawShapeProperties(**shape, "dynamic");
}
}

struct PhysicsWorld::State
{
    RegistrationLease registration;
    JPH::TempAllocatorImpl tempAllocator;
    JPH::JobSystemThreadPool jobSystem;
    BroadPhaseLayerInterface broadPhaseLayers;
    ObjectVsBroadPhaseLayerFilter objectVsBroadPhase;
    ObjectLayerPairFilter objectLayerPairs;
    JPH::PhysicsSystem physicsSystem;

    SceneData* scene = nullptr;
    std::size_t positionCount = 0;
    std::size_t indexCount = 0;
    std::size_t geometryCount = 0;
    std::size_t meshCount = 0;
    std::size_t bodyRecipeCount = 0;
    std::size_t colliderRecipeCount = 0;
    std::size_t instanceCount = 0;
    std::vector<std::uint32_t> instanceMeshes;

    std::vector<JPH::BodyID> allBodies;
    std::vector<JPH::BodyID> dynamicBodyByInstance;
    std::vector<std::size_t> dynamicInstances;
    std::vector<glm::mat4> transformScratch;
    double accumulator = 0.0;
    bool failed = false;

    explicit State(SceneData* boundScene)
        : tempAllocator{TempAllocatorBytes}
        , scene{boundScene}
        , positionCount{boundScene->positions.size()}
        , indexCount{boundScene->indices.size()}
        , geometryCount{boundScene->geometries.size()}
        , meshCount{boundScene->meshes.size()}
        , bodyRecipeCount{boundScene->physicsBodies.size()}
        , colliderRecipeCount{boundScene->physicsColliders.size()}
        , instanceCount{boundScene->instances.size()}
    {
        // Init after default construction so a thread-creation exception still
        // unwinds through JobSystemThreadPool's destructor and joins workers
        // that were started successfully before the failure.
        jobSystem.Init(
            JPH::cMaxPhysicsJobs,
            JPH::cMaxPhysicsBarriers,
            static_cast<int>(workerCount()));

        physicsSystem.Init(
            MaximumBodies,
            BodyMutexCount,
            MaximumBodyPairs,
            MaximumContactConstraints,
            broadPhaseLayers,
            objectVsBroadPhase,
            objectLayerPairs);

        instanceMeshes.reserve(boundScene->instances.size());
        for (const SceneInstance& instance : boundScene->instances) {
            instanceMeshes.push_back(instance.meshIndex);
        }
        allBodies.reserve(boundScene->instances.size());
        dynamicBodyByInstance.resize(boundScene->instances.size());
    }

    ~State() noexcept
    {
        JPH::BodyInterface& bodies = physicsSystem.GetBodyInterface();
        for (const JPH::BodyID body : allBodies) {
            bodies.RemoveBody(body);
            bodies.DestroyBody(body);
        }
    }
};

namespace
{
bool validateBodyShapeBounds(
    const JPH::Shape& shape,
    JPH::RVec3Arg modelOrigin,
    JPH::QuatArg rotation,
    bool rotationIndependent,
    std::string_view owner)
{
    const JPH::Vec3 localCenter = shape.GetCenterOfMass();
    if (!finiteJoltVec3(localCenter)) {
        std::cerr << "Physics: " << owner
                  << " body shape has an invalid center of mass.\n";
        return false;
    }

    const JPH::Vec3 rotatedCenter = rotation * localCenter;
    const std::array<double, 3> centerComponents{
        static_cast<double>(modelOrigin.GetX()) + rotatedCenter.GetX(),
        static_cast<double>(modelOrigin.GetY()) + rotatedCenter.GetY(),
        static_cast<double>(modelOrigin.GetZ()) + rotatedCenter.GetZ(),
    };
    for (const double component : centerComponents) {
        if (!coordinateInJoltRange(component)) {
            std::cerr << "Physics: " << owner
                      << " body center is outside Jolt's supported range.\n";
            return false;
        }
    }

    const JPH::RVec3 positionCom = modelOrigin + rotatedCenter;
    if (!coordinateInJoltRange(positionCom.GetX())
        || !coordinateInJoltRange(positionCom.GetY())
        || !coordinateInJoltRange(positionCom.GetZ())) {
        std::cerr << "Physics: " << owner
                  << " body center rounds outside Jolt's supported range.\n";
        return false;
    }
    const JPH::RMat44 centerTransform =
        JPH::RMat44::sRotationTranslation(rotation, positionCom);
    const JPH::AABox worldBounds = shape.GetWorldSpaceBounds(
        centerTransform,
        JPH::Vec3::sOne());
    if (!validJoltBounds(worldBounds)) {
        std::cerr << "Physics: " << owner
                  << " body has invalid or out-of-range world bounds.\n";
        return false;
    }

    if (rotationIndependent) {
        const JPH::AABox localBounds = shape.GetLocalBounds();
        if (!validJoltBounds(localBounds)) {
            std::cerr << "Physics: " << owner
                      << " body has invalid local bounds.\n";
            return false;
        }
        const double extentX = std::max(
            std::abs(static_cast<double>(localBounds.mMin.GetX())),
            std::abs(static_cast<double>(localBounds.mMax.GetX())));
        const double extentY = std::max(
            std::abs(static_cast<double>(localBounds.mMin.GetY())),
            std::abs(static_cast<double>(localBounds.mMax.GetY())));
        const double extentZ = std::max(
            std::abs(static_cast<double>(localBounds.mMin.GetZ())),
            std::abs(static_cast<double>(localBounds.mMax.GetZ())));
        const double radius = std::hypot(extentX, extentY, extentZ);
        for (const double component : centerComponents) {
            if (!std::isfinite(radius)
                || std::abs(component) + radius > JPH::cLargeFloat) {
                std::cerr << "Physics: " << owner
                          << " body can rotate outside Jolt's supported range.\n";
                return false;
            }
        }
    }
    return true;
}

bool validateDynamicBodyMass(
    const JPH::BodyCreationSettings& settings,
    std::size_t instanceIndex)
{
    const JPH::MassProperties properties = settings.GetMassProperties();
    if (!finiteMassProperties(properties)) {
        std::cerr << "Physics: dynamic instance[" << instanceIndex
                  << "] has invalid scaled mass or inertia.\n";
        return false;
    }

    const double inverseMassValue = 1.0 / static_cast<double>(properties.mMass);
    if (!std::isfinite(inverseMassValue)
        || inverseMassValue > std::numeric_limits<float>::max()
        || inverseMassValue < std::numeric_limits<float>::denorm_min()) {
        std::cerr << "Physics: dynamic instance[" << instanceIndex
                  << "] has invalid scaled mass or inertia.\n";
        return false;
    }

    double inertiaNormSquared = 0.0;
    for (JPH::uint column = 0; column < 3; ++column) {
        const JPH::Vec3 values = properties.mInertia.GetColumn3(column);
        inertiaNormSquared +=
            static_cast<double>(values.GetX()) * values.GetX()
            + static_cast<double>(values.GetY()) * values.GetY()
            + static_cast<double>(values.GetZ()) * values.GetZ();
    }
    if (!std::isfinite(inertiaNormSquared)
        || inertiaNormSquared <= 1.0e-12) {
        std::cerr << "Physics: dynamic instance[" << instanceIndex
                  << "] has near-zero scaled inertia.\n";
        return false;
    }

    JPH::Mat44 inertiaRotation;
    JPH::Vec3 principalMoments;
    const bool decomposed = properties.DecomposePrincipalMomentsOfInertia(
        inertiaRotation,
        principalMoments);
    if (!decomposed) {
        std::cerr << "Physics: dynamic instance[" << instanceIndex
                  << "] inertia cannot be decomposed.\n";
        return false;
    }
    if (principalMoments.IsNearZero()) {
        std::cerr << "Physics: dynamic instance[" << instanceIndex
                  << "] decomposes to near-zero principal inertia.\n";
        return false;
    }
    const std::array moments{
        principalMoments.GetX(),
        principalMoments.GetY(),
        principalMoments.GetZ(),
    };
    for (const float moment : moments) {
        if (!finite(moment) || moment <= 0.0f) {
            std::cerr << "Physics: dynamic instance[" << instanceIndex
                      << "] has a nonpositive principal inertia.\n";
            return false;
        }
        const double inverseMoment = 1.0 / static_cast<double>(moment);
        if (!std::isfinite(inverseMoment)
            || inverseMoment > std::numeric_limits<float>::max()
            || inverseMoment < std::numeric_limits<float>::denorm_min()) {
            std::cerr << "Physics: dynamic instance[" << instanceIndex
                      << "] has an unrepresentable inverse principal inertia.\n";
            return false;
        }
    }
    return true;
}

bool topologyMatches(const PhysicsWorld::State& state)
{
    const SceneData& scene = *state.scene;
    if (scene.positions.size() != state.positionCount
        || scene.indices.size() != state.indexCount
        || scene.geometries.size() != state.geometryCount
        || scene.meshes.size() != state.meshCount
        || scene.physicsBodies.size() != state.bodyRecipeCount
        || scene.physicsColliders.size() != state.colliderRecipeCount
        || scene.instances.size() != state.instanceCount) {
        return false;
    }
    for (std::size_t index = 0; index < state.instanceMeshes.size(); ++index) {
        if (scene.instances[index].meshIndex != state.instanceMeshes[index]) {
            return false;
        }
    }
    return true;
}

bool validateLiveWorld(PhysicsWorld::State* state, std::string_view operation)
{
    if (state->failed) {
        std::cerr << "Physics: " << operation
                  << " rejected because the world is terminally failed.\n";
        return false;
    }
    if (!topologyMatches(*state)) {
        state->failed = true;
        std::cerr << "Physics: " << operation
                  << " detected a bound-scene topology change; the world is now terminal.\n";
        return false;
    }
    return true;
}

JPH::BodyID dynamicBody(
    const PhysicsWorld::State& state,
    std::size_t instanceIndex)
{
    if (instanceIndex >= state.dynamicBodyByInstance.size()) {
        return JPH::BodyID{};
    }
    return state.dynamicBodyByInstance[instanceIndex];
}

bool createStaticBody(
    PhysicsWorld::State* state,
    const SceneInstance& instance,
    std::size_t instanceIndex)
{
    const SceneData& scene = *state->scene;
    const SceneMesh& mesh = scene.meshes[instance.meshIndex];
    JPH::VertexList vertices;
    JPH::IndexedTriangleList triangles;
    const bool reflected = linearDeterminant(instance.transform) < 0.0;

    for (std::size_t meshOffset = 0; meshOffset < mesh.geometryCount; ++meshOffset) {
        const SceneGeometry& geometry =
            scene.geometries[mesh.firstGeometry + meshOffset];
        if (vertices.size()
            > static_cast<std::size_t>(std::numeric_limits<JPH::uint32>::max())
                - geometry.vertexCount) {
            std::cerr << "Physics: static instance[" << instanceIndex
                      << "] has too many collision vertices.\n";
            return false;
        }
        const JPH::uint32 destinationVertexBase =
            static_cast<JPH::uint32>(vertices.size());

        vertices.reserve(vertices.size() + geometry.vertexCount);
        for (std::size_t localVertex = 0;
             localVertex < geometry.vertexCount;
             ++localVertex) {
            const std::size_t sceneVertex = geometry.firstVertex + localVertex;
            const glm::vec4 transformed = instance.transform * glm::vec4{
                scene.positions[sceneVertex * 3],
                scene.positions[sceneVertex * 3 + 1],
                scene.positions[sceneVertex * 3 + 2],
                1.0f,
            };
            if (!finite(transformed.x)
                || !finite(transformed.y)
                || !finite(transformed.z)
                || !finite(transformed.w)) {
                std::cerr << "Physics: static instance[" << instanceIndex
                          << "] produces a non-finite baked vertex.\n";
                return false;
            }
            if (!coordinateInJoltRange(transformed.x)
                || !coordinateInJoltRange(transformed.y)
                || !coordinateInJoltRange(transformed.z)) {
                std::cerr << "Physics: static instance[" << instanceIndex
                          << "] produces a vertex outside Jolt's supported range.\n";
                return false;
            }
            vertices.emplace_back(transformed.x, transformed.y, transformed.z);
        }

        triangles.reserve(triangles.size() + geometry.indexCount / 3);
        for (std::size_t slot = 0; slot < geometry.indexCount; slot += 3) {
            JPH::uint32 first = destinationVertexBase
                + scene.indices[geometry.firstIndex + slot];
            JPH::uint32 second = destinationVertexBase
                + scene.indices[geometry.firstIndex + slot + 1];
            JPH::uint32 third = destinationVertexBase
                + scene.indices[geometry.firstIndex + slot + 2];
            if (reflected) {
                std::swap(second, third);
            }
            triangles.emplace_back(first, second, third, 0);
        }
    }

    JPH::MeshShapeSettings shapeSettings{std::move(vertices), std::move(triangles)};
    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (!shapeResult.IsValid()) {
        std::cerr << "Physics: static instance[" << instanceIndex << "] ";
        return reportShapeError(shapeResult, "mesh");
    }

    JPH::ShapeRefC shape = shapeResult.Get();
    JPH::BodyCreationSettings bodySettings{
        shape.GetPtr(),
        JPH::RVec3::sZero(),
        JPH::Quat::sIdentity(),
        JPH::EMotionType::Static,
        LayerStatic,
    };
    bodySettings.mFriction = DefaultFriction;
    bodySettings.mRestitution = DefaultRestitution;
    if (!validateBodyShapeBounds(
            *shape,
            bodySettings.mPosition,
            bodySettings.mRotation,
            false,
            "static")) {
        return false;
    }

    JPH::BodyInterface& bodies = state->physicsSystem.GetBodyInterface();
    const JPH::BodyID id = bodies.CreateAndAddBody(
        bodySettings,
        JPH::EActivation::DontActivate);
    if (id.IsInvalid()) {
        std::cerr << "Physics: failed to create static body for instance["
                  << instanceIndex << "].\n";
        return false;
    }
    state->allBodies.push_back(id);
    return true;
}

bool createDynamicBody(
    PhysicsWorld::State* state,
    const SceneInstance& instance,
    std::size_t instanceIndex)
{
    const SceneData& scene = *state->scene;
    const ScenePhysicsBody* recipe = uniqueBodyForMesh(scene, instance.meshIndex);
    if (recipe == nullptr) {
        return false;
    }

    JPH::ShapeRefC shape;
    if (!createDynamicShape(scene, *recipe, &shape)) {
        std::cerr << "Physics: dynamic instance[" << instanceIndex
                  << "] shape creation failed.\n";
        return false;
    }

    const glm::vec3 position{instance.transform[3]};
    JPH::BodyCreationSettings bodySettings{
        shape.GetPtr(),
        toJoltPosition(position),
        transformRotation(instance.transform),
        JPH::EMotionType::Dynamic,
        LayerMoving,
    };
    bodySettings.mFriction = DefaultFriction;
    bodySettings.mRestitution = DefaultRestitution;
    bodySettings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    bodySettings.mMaxLinearVelocity = MaximumLinearVelocity;
    bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = recipe->mass;
    if (!validateBodyShapeBounds(
            *shape,
            bodySettings.mPosition,
            bodySettings.mRotation,
            true,
            "dynamic")
        || !validateDynamicBodyMass(bodySettings, instanceIndex)) {
        return false;
    }

    JPH::BodyInterface& bodies = state->physicsSystem.GetBodyInterface();
    const JPH::BodyID id = bodies.CreateAndAddBody(
        bodySettings,
        JPH::EActivation::Activate);
    if (id.IsInvalid()) {
        std::cerr << "Physics: failed to create dynamic body for instance["
                  << instanceIndex << "].\n";
        return false;
    }

    state->allBodies.push_back(id);
    state->dynamicBodyByInstance[instanceIndex] = id;
    state->dynamicInstances.push_back(instanceIndex);
    return true;
}
}

PhysicsWorld::~PhysicsWorld() noexcept
{
    delete state;
    state = nullptr;
}

bool createPhysicsWorld(
    PhysicsWorld* world,
    SceneData* scene,
    std::span<const std::size_t> dynamicInstances)
{
    if (world == nullptr) {
        std::cerr << "Physics: cannot create a null world owner.\n";
        return false;
    }
    if (world->state != nullptr) {
        std::cerr << "Physics: cannot initialize an already initialized world.\n";
        return false;
    }
    if (scene == nullptr) {
        std::cerr << "Physics: cannot bind a null scene.\n";
        return false;
    }

    try {
        std::vector<bool> dynamicMask;
        if (!validateSceneForPhysics(*scene, dynamicInstances, &dynamicMask)) {
            return false;
        }

        std::unique_ptr<PhysicsWorld::State> candidate =
            std::make_unique<PhysicsWorld::State>(scene);
        candidate->dynamicInstances.reserve(dynamicInstances.size());
        candidate->transformScratch.resize(dynamicInstances.size());

        for (std::size_t index = 0; index < scene->instances.size(); ++index) {
            const bool created = dynamicMask[index]
                ? createDynamicBody(candidate.get(), scene->instances[index], index)
                : createStaticBody(candidate.get(), scene->instances[index], index);
            if (!created) {
                return false;
            }
        }

        candidate->physicsSystem.OptimizeBroadPhase();
        world->state = candidate.release();
        return true;
    } catch (const std::bad_alloc&) {
        std::cerr << "Physics: world creation ran out of memory.\n";
    } catch (const std::length_error&) {
        std::cerr << "Physics: world creation exceeded a container limit.\n";
    } catch (const std::system_error& error) {
        std::cerr << "Physics: world creation failed to start its job system: "
                  << error.what() << ".\n";
    } catch (const std::exception& error) {
        std::cerr << "Physics: world creation failed: " << error.what() << ".\n";
    } catch (...) {
        std::cerr << "Physics: world creation failed with an unknown exception.\n";
    }
    return false;
}

bool stepPhysics(PhysicsWorld* world, float frameDt)
{
    if (world == nullptr || world->state == nullptr) {
        std::cerr << "Physics: cannot step a null or uninitialized world.\n";
        return false;
    }
    PhysicsWorld::State& state = *world->state;
    if (!validateLiveWorld(&state, "step")) {
        return false;
    }
    if (!finite(frameDt) || frameDt < 0.0f) {
        std::cerr << "Physics: frame delta must be finite and nonnegative.\n";
        return false;
    }

    const double fixedDt = static_cast<double>(PhysicsFixedDt);
    state.accumulator += std::min(
        static_cast<double>(frameDt),
        static_cast<double>(PhysicsMaxFrameDt));

    std::size_t stepCount = 0;
    while (state.accumulator >= fixedDt && stepCount < 6) {
        JPH::EPhysicsUpdateError error;
        {
            UpdateStatusAssertScope updateAssertScope;
            error = state.physicsSystem.Update(
                PhysicsFixedDt,
                1,
                &state.tempAllocator,
                &state.jobSystem);
        }
        if (error != JPH::EPhysicsUpdateError::None) {
            state.failed = true;
            std::cerr << "Physics: Jolt update failed with error mask "
                      << static_cast<JPH::uint32>(error)
                      << "; the world is now terminal.\n";
            return false;
        }
        state.accumulator -= fixedDt;
        ++stepCount;
    }
    if (state.accumulator >= fixedDt) {
        state.failed = true;
        std::cerr << "Physics: fixed-step catch-up bound was exceeded; "
                     "the world is now terminal.\n";
        return false;
    }

    JPH::BodyInterface& bodies = state.physicsSystem.GetBodyInterface();
    for (std::size_t index = 0; index < state.dynamicInstances.size(); ++index) {
        const std::size_t instanceIndex = state.dynamicInstances[index];
        const glm::mat4 transform = toGlmMat4(
            bodies.GetWorldTransform(state.dynamicBodyByInstance[instanceIndex]));
        if (!properRigidTransform(transform)) {
            state.failed = true;
            std::cerr << "Physics: solver output for dynamic instance["
                      << instanceIndex
                      << "] is not a finite proper rigid transform; "
                         "the world is now terminal.\n";
            return false;
        }
        state.transformScratch[index] = transform;
    }

    for (std::size_t index = 0; index < state.dynamicInstances.size(); ++index) {
        state.scene->instances[state.dynamicInstances[index]].transform =
            state.transformScratch[index];
    }
    return true;
}

bool setPhysicsBodyLinearVelocity(
    PhysicsWorld* world,
    std::size_t instanceIndex,
    std::array<float, 3> velocity)
{
    if (world == nullptr || world->state == nullptr) {
        std::cerr << "Physics: cannot set velocity on a null or uninitialized world.\n";
        return false;
    }
    PhysicsWorld::State& state = *world->state;
    if (!validateLiveWorld(&state, "velocity update")) {
        return false;
    }
    if (!finite(velocity[0]) || !finite(velocity[1]) || !finite(velocity[2])) {
        std::cerr << "Physics: linear velocity must contain only finite values.\n";
        return false;
    }
    const JPH::BodyID body = dynamicBody(state, instanceIndex);
    if (body.IsInvalid()) {
        std::cerr << "Physics: instance index " << instanceIndex
                  << " is not dynamic in this world.\n";
        return false;
    }

    const double lengthSquared =
        static_cast<double>(velocity[0]) * velocity[0]
        + static_cast<double>(velocity[1]) * velocity[1]
        + static_cast<double>(velocity[2]) * velocity[2];
    const double maximumSquared =
        static_cast<double>(MaximumLinearVelocity) * MaximumLinearVelocity;
    if (lengthSquared > maximumSquared) {
        const double scale = MaximumLinearVelocity / std::sqrt(lengthSquared);
        for (float& component : velocity) {
            component = static_cast<float>(
                static_cast<double>(component) * scale);
        }
    }

    JPH::BodyInterface& bodies = state.physicsSystem.GetBodyInterface();
    bodies.SetLinearVelocity(body, JPH::Vec3{velocity[0], velocity[1], velocity[2]});
    bodies.ActivateBody(body);
    return true;
}

bool queryPhysicsBodyActive(
    const PhysicsWorld* world,
    std::size_t instanceIndex,
    bool* active)
{
    if (world == nullptr || world->state == nullptr) {
        std::cerr << "Physics: cannot query a null or uninitialized world.\n";
        return false;
    }
    PhysicsWorld::State& state = *world->state;
    if (!validateLiveWorld(&state, "activity query")) {
        return false;
    }
    if (active == nullptr) {
        std::cerr << "Physics: activity query output is null.\n";
        return false;
    }
    const JPH::BodyID body = dynamicBody(state, instanceIndex);
    if (body.IsInvalid()) {
        std::cerr << "Physics: instance index " << instanceIndex
                  << " is not dynamic in this world.\n";
        return false;
    }

    const bool result = state.physicsSystem.GetBodyInterface().IsActive(body);
    *active = result;
    return true;
}

#ifdef XRPHOTON_PHYSICS_TESTING
bool setPhysicsBodyMotionQualityForTest(
    PhysicsWorld* world,
    std::size_t instanceIndex,
    PhysicsTestMotionQuality quality)
{
    if (world == nullptr || world->state == nullptr) {
        std::cerr << "Physics: cannot set motion quality on a null or uninitialized world.\n";
        return false;
    }
    PhysicsWorld::State& state = *world->state;
    if (!validateLiveWorld(&state, "test motion-quality update")) {
        return false;
    }
    const JPH::BodyID body = dynamicBody(state, instanceIndex);
    if (body.IsInvalid()) {
        std::cerr << "Physics: instance index " << instanceIndex
                  << " is not dynamic in this world.\n";
        return false;
    }

    JPH::EMotionQuality joltQuality;
    switch (quality) {
    case PhysicsTestMotionQuality::Discrete:
        joltQuality = JPH::EMotionQuality::Discrete;
        break;
    case PhysicsTestMotionQuality::LinearCast:
        joltQuality = JPH::EMotionQuality::LinearCast;
        break;
    default:
        std::cerr << "Physics: test motion quality is invalid.\n";
        return false;
    }
    state.physicsSystem.GetBodyInterface().SetMotionQuality(body, joltQuality);
    return true;
}
#endif
}
