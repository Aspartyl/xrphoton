#pragma once

#include "scene.hpp"
#include <cstddef>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

namespace xrphoton
{
struct GallerySpawn
{
    glm::vec3 position{};
    float yaw = 0.0f;
    float pitch = 0.0f;
};

struct GalleryLoadResult
{
    SceneData scene;
    std::string error;
    // Flat instance indices whose transforms are produced by PhysicsWorld. The
    // generated yard always contributes the crate; configured rigid assets append
    // their entries in placement order.
    std::vector<std::size_t> dynamicInstances;
    GallerySpawn spawn;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error.empty();
    }
};

enum class GallerySceneProfile
{
    Complete,
    EstimatorReference,
    FurnaceReference,
};

// Load every configured asset through the generic OGFx path, merge its model records,
// and add the table-owned yard placements. EstimatorReference retains only required,
// exact-transport placements so fixed statistical regions do not depend on local
// optional assets or P3's approximate intervening-Glass visibility.
// FurnaceReference is the isolated, code-owned P5 energy proof.
[[nodiscard]] GalleryLoadResult loadGalleryScene(
    GallerySceneProfile profile = GallerySceneProfile::Complete);
}
