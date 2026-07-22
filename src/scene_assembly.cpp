#include "scene_assembly.hpp"

#include "ray_types.hpp"
#include "scene_assembly_detail.hpp"

#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace xrphoton
{
namespace
{
constexpr uint64_t MaximumUint32 = std::numeric_limits<uint32_t>::max();
constexpr uint64_t SbtOffsetLimit = uint64_t{1} << 24;
// Both instanceCustomIndex and instanceShaderBindingTableRecordOffset are 24-bit.
// The latter addresses the first of RayTypeCount interleaved records per geometry.
constexpr uint64_t MaximumGeometryCount =
    (SbtOffsetLimit - 1) / RayTypeCount + 1;
static_assert((MaximumGeometryCount - 1) * RayTypeCount < SbtOffsetLimit);

bool reject(std::string* error, std::string diagnostic)
{
    if (error != nullptr) {
        *error = std::move(diagnostic);
    }
    return false;
}

bool rejectAllocation(std::string* error)
{
    return reject(error, "scene assembly: resource allocation failed");
}

void clearError(std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
}

bool checkedTotal(
    uint64_t destination,
    uint64_t source,
    uint64_t limit,
    bool limitIsExclusive,
    std::string_view field,
    std::string* error)
{
    if (source > std::numeric_limits<uint64_t>::max() - destination) {
        return reject(
            error,
            "scene assembly: combined " + std::string(field) + " count overflows uint64");
    }

    const uint64_t total = destination + source;
    const bool exceedsLimit = limitIsExclusive ? total >= limit : total > limit;
    if (exceedsLimit) {
        return reject(
            error,
            "scene assembly: combined " + std::string(field) + " count "
                + std::to_string(total) + (limitIsExclusive ? " must be below " : " exceeds ")
                + std::to_string(limit));
    }
    return true;
}

template<typename T>
void reserveForAppend(std::vector<T>* destination, std::size_t sourceSize)
{
    if (sourceSize > destination->max_size() - destination->size()) {
        throw std::length_error("scene append exceeds vector max_size");
    }
    destination->reserve(destination->size() + sourceSize);
}

template<typename T>
void appendMoved(std::vector<T>* destination, std::vector<T>* source)
{
    destination->insert(
        destination->end(),
        std::make_move_iterator(source->begin()),
        std::make_move_iterator(source->end()));
}

bool checkRebasedValue(
    uint32_t value,
    uint64_t base,
    std::string_view field,
    std::size_t index,
    std::string* error)
{
    const uint64_t rebased = static_cast<uint64_t>(value) + base;
    if (rebased > MaximumUint32) {
        return reject(
            error,
            "scene assembly: " + std::string(field) + "[" + std::to_string(index)
                + "] rebases past UINT32_MAX");
    }
    return true;
}

scene_assembly_detail::SceneElementCounts elementCounts(const SceneData& scene)
{
    return {
        .vertices = static_cast<uint64_t>(scene.attributes.size()),
        .indices = static_cast<uint64_t>(scene.indices.size()),
        .geometries = static_cast<uint64_t>(scene.geometries.size()),
        .meshes = static_cast<uint64_t>(scene.meshes.size()),
        .materials = static_cast<uint64_t>(scene.materials.size()),
        .physicsBodies = static_cast<uint64_t>(scene.physicsBodies.size()),
        .physicsColliders = static_cast<uint64_t>(scene.physicsColliders.size()),
    };
}

bool validPositionAttributeShape(
    const SceneData& scene,
    std::string_view owner,
    std::string* error)
{
    if ((scene.positions.size() % 3) != 0) {
        return reject(
            error,
            "scene assembly: " + std::string(owner)
                + " position scalar count must be divisible by 3");
    }
    if ((scene.positions.size() / 3) != scene.attributes.size()) {
        return reject(
            error,
            "scene assembly: " + std::string(owner)
                + " position and attribute counts must match");
    }
    return true;
}

bool finiteTransform(const glm::mat4& transform, std::size_t instanceIndex, std::string* error)
{
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            if (!std::isfinite(transform[column][row])) {
                return reject(
                    error,
                    "scene assembly: instance[" + std::to_string(instanceIndex)
                        + "].transform[" + std::to_string(column) + "]["
                        + std::to_string(row) + "] is not finite");
            }
        }
    }

    constexpr double AffineTolerance = 1.0e-4;
    for (std::size_t column = 0; column < 4; ++column) {
        const double expected = column == 3 ? 1.0 : 0.0;
        if (std::abs(static_cast<double>(transform[column][3]) - expected)
            > AffineTolerance) {
            return reject(
                error,
                "scene assembly: instance[" + std::to_string(instanceIndex)
                    + "].transform homogeneous row must be within 1e-4 of (0,0,0,1)");
        }
    }
    return true;
}

bool finiteVec3(const glm::vec3& value)
{
    return std::isfinite(value.x)
        && std::isfinite(value.y)
        && std::isfinite(value.z);
}

bool finitePositive(float value)
{
    return std::isfinite(value) && value > 0.0f;
}

bool validatePhysicsRecipes(const SceneData& scene, std::string* error)
{
    if (scene.physicsBodies.empty() != scene.physicsColliders.empty()) {
        return reject(
            error,
            "scene assembly: physics body and collider arrays must both be empty or both be nonempty");
    }

    uint64_t expectedFirstCollider = 0;
    for (std::size_t index = 0; index < scene.physicsBodies.size(); ++index) {
        const ScenePhysicsBody& body = scene.physicsBodies[index];
        if (static_cast<uint64_t>(body.meshIndex) >= scene.meshes.size()) {
            return reject(
                error,
                "scene assembly: physicsBodies[" + std::to_string(index)
                    + "].meshIndex " + std::to_string(body.meshIndex) + " is outside "
                    + std::to_string(scene.meshes.size()) + " meshes");
        }
        if (body.colliderCount == 0) {
            return reject(
                error,
                "scene assembly: physicsBodies[" + std::to_string(index)
                    + "].colliderCount must be nonzero");
        }
        if (body.firstCollider != expectedFirstCollider) {
            return reject(
                error,
                "scene assembly: physicsBodies[" + std::to_string(index)
                    + "].firstCollider must equal the next partition offset "
                    + std::to_string(expectedFirstCollider));
        }
        const uint64_t colliderEnd =
            static_cast<uint64_t>(body.firstCollider) + body.colliderCount;
        if (colliderEnd > scene.physicsColliders.size()) {
            return reject(
                error,
                "scene assembly: physicsBodies[" + std::to_string(index)
                    + "] collider range exceeds "
                    + std::to_string(scene.physicsColliders.size()) + " colliders");
        }
        if (!finitePositive(body.mass)) {
            return reject(
                error,
                "scene assembly: physicsBodies[" + std::to_string(index)
                    + "].mass must be finite and positive");
        }
        if (!finiteVec3(body.centerOfMass)) {
            return reject(
                error,
                "scene assembly: physicsBodies[" + std::to_string(index)
                    + "].centerOfMass must contain finite values");
        }
        expectedFirstCollider = colliderEnd;
    }
    if (expectedFirstCollider != scene.physicsColliders.size()) {
        return reject(
            error,
            "scene assembly: final physics collider partition end must equal collider count "
                + std::to_string(scene.physicsColliders.size()));
    }

    for (std::size_t index = 0; index < scene.physicsColliders.size(); ++index) {
        const ScenePhysicsCollider& collider = scene.physicsColliders[index];
        if (collider.shape != ScenePhysicsShape::Cylinder
            && collider.shape != ScenePhysicsShape::Box) {
            return reject(
                error,
                "scene assembly: physicsColliders[" + std::to_string(index)
                    + "].shape must be cylinder or box");
        }
        if (!finiteVec3(collider.center)) {
            return reject(
                error,
                "scene assembly: physicsColliders[" + std::to_string(index)
                    + "].center must contain finite values");
        }
        if (!finitePositive(collider.mass)) {
            return reject(
                error,
                "scene assembly: physicsColliders[" + std::to_string(index)
                    + "].mass must be finite and positive");
        }
        if (!finiteVec3(collider.centerOfMass)) {
            return reject(
                error,
                "scene assembly: physicsColliders[" + std::to_string(index)
                    + "].centerOfMass must contain finite values");
        }

        if (collider.shape == ScenePhysicsShape::Cylinder) {
            if (!finiteVec3(collider.axis)) {
                return reject(
                    error,
                    "scene assembly: physicsColliders[" + std::to_string(index)
                        + "].axis must contain finite values");
            }
            const double axisX = collider.axis.x;
            const double axisY = collider.axis.y;
            const double axisZ = collider.axis.z;
            const double axisLengthSquared =
                axisX * axisX + axisY * axisY + axisZ * axisZ;
            if (!std::isfinite(axisLengthSquared) || axisLengthSquared <= 0.0) {
                return reject(
                    error,
                    "scene assembly: physicsColliders[" + std::to_string(index)
                        + "].axis length squared must be finite and positive");
            }
            if (!finitePositive(collider.height)) {
                return reject(
                    error,
                    "scene assembly: physicsColliders[" + std::to_string(index)
                        + "].height must be finite and positive");
            }
            if (!finitePositive(collider.radius)) {
                return reject(
                    error,
                    "scene assembly: physicsColliders[" + std::to_string(index)
                        + "].radius must be finite and positive");
            }
        } else {
            const glm::quat& orientation = collider.orientation;
            const double orientationLengthSquared =
                static_cast<double>(orientation.x) * orientation.x
                + static_cast<double>(orientation.y) * orientation.y
                + static_cast<double>(orientation.z) * orientation.z
                + static_cast<double>(orientation.w) * orientation.w;
            if (!std::isfinite(orientationLengthSquared)
                || std::abs(orientationLengthSquared - 1.0) > 1.0e-4) {
                return reject(
                    error,
                    "scene assembly: physicsColliders[" + std::to_string(index)
                        + "].orientation length squared must be finite and within 1e-4 of 1");
            }
            if (!finiteVec3(collider.halfExtents)
                || !finitePositive(collider.halfExtents.x)
                || !finitePositive(collider.halfExtents.y)
                || !finitePositive(collider.halfExtents.z)) {
                return reject(
                    error,
                    "scene assembly: physicsColliders[" + std::to_string(index)
                        + "].halfExtents must contain finite positive values");
            }
        }
    }
    return true;
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
}

namespace scene_assembly_detail
{
bool validateSceneAppendCounts(
    const SceneElementCounts& destination,
    const SceneElementCounts& source,
    std::string* error)
{
    clearError(error);
    try {
        return checkedTotal(
                   destination.vertices,
                   source.vertices,
                   MaximumUint32,
                   false,
                   "vertex",
                   error)
            && checkedTotal(
                destination.indices,
                source.indices,
                MaximumUint32,
                false,
                "index",
                error)
            && checkedTotal(
                destination.geometries,
                source.geometries,
                MaximumGeometryCount,
                false,
                "geometry",
                error)
            && checkedTotal(
                destination.meshes,
                source.meshes,
                MaximumUint32,
                false,
                "mesh",
                error)
            && checkedTotal(
                destination.materials,
                source.materials,
                MaximumUint32,
                false,
                "material",
                error)
            && checkedTotal(
                destination.physicsBodies,
                source.physicsBodies,
                MaximumUint32,
                false,
                "physics body",
                error)
            && checkedTotal(
                destination.physicsColliders,
                source.physicsColliders,
                MaximumUint32,
                false,
                "physics collider",
                error);
    } catch (const std::bad_alloc&) {
        return rejectAllocation(error);
    } catch (const std::length_error&) {
        return rejectAllocation(error);
    }
}
}

bool appendSceneModel(SceneData* scene, SceneData&& model, std::string* error)
{
    clearError(error);
    try {
        if (scene == nullptr) {
            return reject(error, "scene assembly: destination scene is null");
        }
        if (scene == &model) {
            return reject(
                error,
                "scene assembly: destination scene and incoming model must not alias");
        }
        if (!model.instances.empty()) {
            return reject(error, "scene assembly: incoming model instances must be empty");
        }
        if (!model.images.empty()) {
            return reject(error, "scene assembly: incoming model images must be empty");
        }
        if (!scene->images.empty()) {
            return reject(error, "scene assembly: models must be appended before scene images resolve");
        }
        if (!validPositionAttributeShape(*scene, "destination scene", error)
            || !validPositionAttributeShape(model, "incoming model", error)) {
            return false;
        }
        for (std::size_t index = 0; index < model.materials.size(); ++index) {
            if (model.materials[index].baseColorImage != 0) {
                return reject(
                    error,
                    "scene assembly: incoming material[" + std::to_string(index)
                        + "].baseColorImage must be 0 before scene resolution");
            }
        }

        const scene_assembly_detail::SceneElementCounts destinationCounts =
            elementCounts(*scene);
        const scene_assembly_detail::SceneElementCounts sourceCounts =
            elementCounts(model);
        if (!scene_assembly_detail::validateSceneAppendCounts(
                destinationCounts,
                sourceCounts,
                error)) {
            return false;
        }

        for (std::size_t index = 0; index < model.geometries.size(); ++index) {
            const SceneGeometry& geometry = model.geometries[index];
            if (!checkRebasedValue(
                    geometry.firstVertex,
                    destinationCounts.vertices,
                    "geometry.firstVertex",
                    index,
                    error)
                || !checkRebasedValue(
                    geometry.firstIndex,
                    destinationCounts.indices,
                    "geometry.firstIndex",
                    index,
                    error)
                || !checkRebasedValue(
                    geometry.materialIndex,
                    destinationCounts.materials,
                    "geometry.materialIndex",
                    index,
                    error)) {
                return false;
            }
        }
        for (std::size_t index = 0; index < model.meshes.size(); ++index) {
            if (!checkRebasedValue(
                    model.meshes[index].firstGeometry,
                    destinationCounts.geometries,
                    "mesh.firstGeometry",
                    index,
                    error)) {
                return false;
            }
        }
        for (std::size_t index = 0; index < model.physicsBodies.size(); ++index) {
            const ScenePhysicsBody& body = model.physicsBodies[index];
            if (!checkRebasedValue(
                    body.meshIndex,
                    destinationCounts.meshes,
                    "physicsBody.meshIndex",
                    index,
                    error)
                || !checkRebasedValue(
                    body.firstCollider,
                    destinationCounts.physicsColliders,
                    "physicsBody.firstCollider",
                    index,
                    error)) {
                return false;
            }
        }

        // Reserve every destination before rebasing or moving any source value. Once
        // these calls succeed, all appended record types are nothrow-movable and no
        // later step can expose a partial destination scene.
        reserveForAppend(&scene->positions, model.positions.size());
        reserveForAppend(&scene->attributes, model.attributes.size());
        reserveForAppend(&scene->indices, model.indices.size());
        reserveForAppend(&scene->geometries, model.geometries.size());
        reserveForAppend(&scene->meshes, model.meshes.size());
        reserveForAppend(&scene->materials, model.materials.size());
        reserveForAppend(&scene->physicsBodies, model.physicsBodies.size());
        reserveForAppend(&scene->physicsColliders, model.physicsColliders.size());

        static_assert(std::is_nothrow_move_constructible_v<SceneMaterial>);
        static_assert(std::is_nothrow_move_constructible_v<ScenePhysicsCollider>);
        for (SceneGeometry& geometry : model.geometries) {
            geometry.firstVertex += static_cast<uint32_t>(destinationCounts.vertices);
            geometry.firstIndex += static_cast<uint32_t>(destinationCounts.indices);
            geometry.materialIndex += static_cast<uint32_t>(destinationCounts.materials);
        }
        for (SceneMesh& mesh : model.meshes) {
            mesh.firstGeometry += static_cast<uint32_t>(destinationCounts.geometries);
        }
        for (ScenePhysicsBody& body : model.physicsBodies) {
            body.meshIndex += static_cast<uint32_t>(destinationCounts.meshes);
            body.firstCollider +=
                static_cast<uint32_t>(destinationCounts.physicsColliders);
        }

        appendMoved(&scene->positions, &model.positions);
        appendMoved(&scene->attributes, &model.attributes);
        appendMoved(&scene->indices, &model.indices);
        appendMoved(&scene->geometries, &model.geometries);
        appendMoved(&scene->meshes, &model.meshes);
        appendMoved(&scene->materials, &model.materials);
        appendMoved(&scene->physicsBodies, &model.physicsBodies);
        appendMoved(&scene->physicsColliders, &model.physicsColliders);
        return true;
    } catch (const std::bad_alloc&) {
        return rejectAllocation(error);
    } catch (const std::length_error&) {
        return rejectAllocation(error);
    }
}

bool appendSceneInstance(
    SceneData* scene,
    uint32_t meshIndex,
    const glm::mat4& transform,
    std::string* error)
{
    clearError(error);
    try {
        if (scene == nullptr) {
            return reject(error, "scene assembly: destination scene is null");
        }
        if (static_cast<uint64_t>(meshIndex) >= scene->meshes.size()) {
            return reject(
                error,
                "scene assembly: instance meshIndex " + std::to_string(meshIndex)
                    + " is outside " + std::to_string(scene->meshes.size()) + " meshes");
        }
        reserveForAppend(&scene->instances, 1);
        scene->instances.push_back({
            .meshIndex = meshIndex,
            .transform = transform,
        });
        return true;
    } catch (const std::bad_alloc&) {
        return rejectAllocation(error);
    } catch (const std::length_error&) {
        return rejectAllocation(error);
    }
}

bool validateAssembledScene(const SceneData& scene, std::string* error)
{
    clearError(error);
    try {
        if (!validPositionAttributeShape(scene, "assembled scene", error)) {
            return false;
        }
        const scene_assembly_detail::SceneElementCounts counts = elementCounts(scene);
        const scene_assembly_detail::SceneElementCounts empty{};
        if (!scene_assembly_detail::validateSceneAppendCounts(empty, counts, error)) {
            return false;
        }
        if (scene.instances.empty()) {
            return reject(error, "scene assembly: assembled scene must contain at least one instance");
        }
        if (!validatePhysicsRecipes(scene, error)) {
            return false;
        }

        for (std::size_t index = 0; index < scene.meshes.size(); ++index) {
            const SceneMesh& mesh = scene.meshes[index];
            if (mesh.geometryCount == 0) {
                return reject(
                    error,
                    "scene assembly: mesh[" + std::to_string(index)
                        + "].geometryCount must be nonzero");
            }
            const uint64_t rangeEnd =
                static_cast<uint64_t>(mesh.firstGeometry) + mesh.geometryCount;
            if (rangeEnd > scene.geometries.size()) {
                return reject(
                    error,
                    "scene assembly: mesh[" + std::to_string(index)
                        + "] geometry range exceeds "
                        + std::to_string(scene.geometries.size()) + " geometries");
            }
        }

        for (std::size_t index = 0; index < scene.instances.size(); ++index) {
            const SceneInstance& instance = scene.instances[index];
            if (static_cast<uint64_t>(instance.meshIndex) >= scene.meshes.size()) {
                return reject(
                    error,
                    "scene assembly: instance[" + std::to_string(index) + "].meshIndex "
                        + std::to_string(instance.meshIndex) + " is outside "
                        + std::to_string(scene.meshes.size()) + " meshes");
            }
            if (!finiteTransform(instance.transform, index, error)) {
                return false;
            }
            const double determinant = linearDeterminant(instance.transform);
            if (!std::isfinite(determinant) || determinant == 0.0) {
                return reject(
                    error,
                    "scene assembly: instance[" + std::to_string(index)
                        + "] transform has a zero-determinant 3x3 linear block");
            }
        }
        return true;
    } catch (const std::bad_alloc&) {
        return rejectAllocation(error);
    } catch (const std::length_error&) {
        return rejectAllocation(error);
    }
}
}
