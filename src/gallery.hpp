#pragma once

#include "scene.hpp"

#include <string>

namespace xrphoton
{
struct GalleryLoadResult
{
    SceneData scene;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error.empty();
    }
};

// Load every configured bring-up asset through the generic OGFx path, merge its
// model records, and add the table-owned preview placements. The table is temporary
// scene policy; callers receive ordinary SceneData with no gallery-specific state.
[[nodiscard]] GalleryLoadResult loadGalleryScene();
}
