#pragma once

#include "texture_loader.hpp"

#include <cstdint>
#include <filesystem>

namespace xrphoton::texture_loader_detail
{
// Test seam for exercising the cumulative-cap boundary without allocating a
// 512 MiB fixture. Production callers use resolveSceneTextures(), which fixes
// this value to MaxSceneTextureBytes.
[[nodiscard]] ResolveTexturesResult resolveSceneTexturesWithByteLimit(
    SceneData* scene,
    const std::filesystem::path& textureRoot,
    uint64_t byteLimit);
}
