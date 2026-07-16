#pragma once

#include "scene.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace xrphoton
{
struct OgfxLoadResult
{
    SceneData scene;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error.empty();
    }
};

// The byte entry point makes the schema boundary deterministic and testable. Both
// entry points return model-owned SceneData only: OGFx has no instances or world
// placement, so the success result deliberately leaves instances and images empty.
[[nodiscard]] OgfxLoadResult decodeOgfxScene(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName = "<memory>");

[[nodiscard]] OgfxLoadResult loadOgfxModel(const std::filesystem::path& path);
}
