#pragma once

#include "scene.hpp"

#include <cstddef>
#include <string>

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
    // A successful yard load always identifies exactly one single-mesh placement.
    std::size_t animatedInstance = 0;
    GallerySpawn spawn;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error.empty();
    }
};

// Deterministic temporary motion policy for the yard's one rigid instance. Physics
// can later replace this producer without changing SceneData or the renderer seam.
[[nodiscard]] glm::mat4 yardAnimatedTransform(double seconds);

// Load every configured asset through the generic OGFx path, merge its model
// records, and add the table-owned yard placements. The table is temporary scene
// policy; callers receive ordinary SceneData plus the spawn/motion policy handles.
[[nodiscard]] GalleryLoadResult loadGalleryScene();
}
