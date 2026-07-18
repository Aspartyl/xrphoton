#include "gallery.hpp"

#include "ogfx_loader.hpp"
#include "scene_assembly.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

namespace xrphoton
{
namespace
{
struct GalleryAsset
{
    const char* name;
    const char8_t* ogfxPath;
    bool optional;
};

struct GalleryPlacement
{
    uint32_t assetIndex;
    glm::mat4 transform;
};

struct LoadedGalleryAsset
{
    bool loaded = false;
    uint32_t firstMesh = 0;
    uint32_t meshCount = 0;
    uint32_t firstMaterial = 0;
    uint32_t materialCount = 0;
};

constexpr std::array GalleryAssets{
    GalleryAsset{
        .name = "test_quad",
        .ogfxPath = XRPHOTON_TEST_QUAD_ASSET_PATH,
        .optional = false,
    },
};

const std::array GalleryPlacements{
    GalleryPlacement{
        .assetIndex = 0,
        .transform = glm::mat4{1.0f},
    },
};

GalleryLoadResult fail(std::string error)
{
    return {
        .scene = {},
        .error = std::move(error),
    };
}

std::string entryDiagnosticPrefix(const GalleryAsset& asset)
{
    const std::filesystem::path path{asset.ogfxPath};
    const std::u8string utf8Path = path.u8string();
    return "Gallery entry '" + std::string(asset.name) + "' ("
        + std::string(
            reinterpret_cast<const char*>(utf8Path.data()),
            utf8Path.size())
        + "): ";
}

const char* countLabel(std::size_t count, const char* singular, const char* plural)
{
    return count == 1 ? singular : plural;
}
}

GalleryLoadResult loadGalleryScene()
{
    try {
        SceneData scene{};
        std::array<LoadedGalleryAsset, GalleryAssets.size()> loadedAssets{};
        std::string assemblyError;

        for (std::size_t assetIndex = 0;
             assetIndex < GalleryAssets.size();
             ++assetIndex) {
            const GalleryAsset& asset = GalleryAssets[assetIndex];
            if (asset.ogfxPath == nullptr || asset.ogfxPath[0] == u8'\0') {
                if (asset.optional) {
                    std::cout << "Gallery entry '" << asset.name
                              << "': skipped (not configured).\n";
                    continue;
                }
                return fail(
                    "Gallery entry '" + std::string(asset.name)
                    + "': required OGFx path is not configured");
            }

            const std::filesystem::path path{asset.ogfxPath};
            OgfxLoadResult loaded = loadOgfxModel(path);
            if (!loaded) {
                return fail(entryDiagnosticPrefix(asset) + loaded.error);
            }

            const std::size_t meshCount = loaded.scene.meshes.size();
            const std::size_t geometryCount = loaded.scene.geometries.size();
            const std::size_t materialCount = loaded.scene.materials.size();
            const std::size_t firstMesh = scene.meshes.size();
            const std::size_t firstMaterial = scene.materials.size();

            if (!appendSceneModel(&scene, std::move(loaded.scene), &assemblyError)) {
                return fail(entryDiagnosticPrefix(asset) + assemblyError);
            }

            // appendSceneModel has already checked the uint32 record ceilings.
            LoadedGalleryAsset& metadata = loadedAssets[assetIndex];
            metadata.loaded = true;
            metadata.firstMesh = static_cast<uint32_t>(firstMesh);
            metadata.meshCount = static_cast<uint32_t>(meshCount);
            metadata.firstMaterial = static_cast<uint32_t>(firstMaterial);
            metadata.materialCount = static_cast<uint32_t>(materialCount);

            std::cout << "Gallery entry '" << asset.name << "': loaded ("
                      << meshCount << ' '
                      << countLabel(meshCount, "mesh", "meshes") << ", "
                      << geometryCount << ' '
                      << countLabel(geometryCount, "geometry", "geometries") << ", "
                      << materialCount << ' '
                      << countLabel(materialCount, "material", "materials")
                      << ").\n";
        }

        for (std::size_t placementIndex = 0;
             placementIndex < GalleryPlacements.size();
             ++placementIndex) {
            const GalleryPlacement& placement = GalleryPlacements[placementIndex];
            if (placement.assetIndex >= loadedAssets.size()) {
                return fail(
                    "Gallery placement[" + std::to_string(placementIndex)
                    + "].assetIndex " + std::to_string(placement.assetIndex)
                    + " is outside " + std::to_string(loadedAssets.size())
                    + " assets");
            }

            const LoadedGalleryAsset& asset = loadedAssets[placement.assetIndex];
            if (!asset.loaded) {
                continue;
            }
            for (uint64_t localMesh = 0; localMesh < asset.meshCount; ++localMesh) {
                const uint64_t meshIndex =
                    static_cast<uint64_t>(asset.firstMesh) + localMesh;
                if (meshIndex > std::numeric_limits<uint32_t>::max()) {
                    return fail(
                        "Gallery placement[" + std::to_string(placementIndex)
                        + "] mesh index exceeds UINT32_MAX");
                }
                if (!appendSceneInstance(
                        &scene,
                        static_cast<uint32_t>(meshIndex),
                        placement.transform,
                        &assemblyError)) {
                    return fail(
                        "Gallery placement[" + std::to_string(placementIndex)
                        + "]: " + assemblyError);
                }
            }
        }

        if (!validateAssembledScene(scene, &assemblyError)) {
            return fail("Gallery scene validation failed: " + assemblyError);
        }
        return {
            .scene = std::move(scene),
            .error = {},
        };
    } catch (const std::bad_alloc&) {
        return fail("Gallery scene assembly failed: resource allocation failed");
    } catch (const std::length_error&) {
        return fail("Gallery scene assembly failed: resource allocation failed");
    } catch (const std::filesystem::filesystem_error& exception) {
        return fail(
            "Gallery scene assembly failed: filesystem path error: "
            + std::string(exception.what()));
    }
}
}
