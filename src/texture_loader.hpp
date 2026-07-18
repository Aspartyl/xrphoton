#pragma once

#include "scene.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xrphoton
{
// Maximum total size of the decoded mip-0 payloads owned by one CPU scene.
inline constexpr uint64_t MaxSceneTextureBytes = uint64_t{512} * 1024 * 1024;

// Map one canonical, extensionless X-Ray logical texture name to its local
// root-relative DDS path. This function performs no filesystem access.
[[nodiscard]] bool resolveLogicalTexturePath(
    std::string_view logicalReference,
    std::filesystem::path* relativePath,
    std::string* error);

struct TextureLoadResult
{
    SceneImage image;
    std::string error;

    explicit operator bool() const { return error.empty(); }
};

// Read and strictly validate the Phase-4 DDS profile, retaining mip 0 only.
[[nodiscard]] TextureLoadResult loadTextureFile(const std::filesystem::path& path);

struct ResolveTexturesResult
{
    std::string error;
    std::optional<uint32_t> failedMaterial;

    explicit operator bool() const { return error.empty(); }
};

// Resolve all material references in deterministic first-use order. Image zero
// is always the generated opaque-white fallback on success.
[[nodiscard]] ResolveTexturesResult resolveSceneTextures(
    SceneData* scene,
    const std::filesystem::path& textureRoot);
}
