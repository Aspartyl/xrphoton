#include "gallery.hpp"

#include "ogfx_loader.hpp"
#include "scene_assembly.hpp"
#include "texture_loader.hpp"

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

#include <glm/gtc/matrix_transform.hpp>

namespace xrphoton
{
namespace
{
struct GalleryAsset
{
    const char* name;
    const char8_t* ogfxPath;
    bool optional;
    // Naming each row's CMake setting keeps optional skip lines actionable without
    // introducing asset-specific branches in the loader.
    const char* configurationName;
};

struct GalleryPlacement
{
    uint32_t assetIndex;
    glm::mat4 transform;
};

enum GalleryAssetIndex : uint32_t
{
    QuadAsset = 0,
    WedgeAsset,
    PlitkaAsset,
    BlenderPyramidAsset,
    BlenderSphereAsset,
};

struct LoadedGalleryAsset
{
    bool loaded = false;
    uint32_t firstMesh = 0;
    uint32_t meshCount = 0;
    uint32_t geometryCount = 0;
    uint32_t firstMaterial = 0;
    uint32_t materialCount = 0;
};

constexpr std::array GalleryAssets{
    GalleryAsset{
        .name = "test_quad",
        .ogfxPath = XRPHOTON_TEST_QUAD_ASSET_PATH,
        .optional = false,
        .configurationName = "XRPHOTON_TEST_QUAD_ASSET_PATH",
    },
    GalleryAsset{
        .name = "test_wedge",
        .ogfxPath = XRPHOTON_TEST_WEDGE_ASSET_PATH,
        .optional = false,
        .configurationName = "XRPHOTON_TEST_WEDGE_ASSET_PATH",
    },
    GalleryAsset{
        .name = "plitka1",
        .ogfxPath = XRPHOTON_GALLERY_PLITKA_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_PLITKA_OGFX",
    },
    GalleryAsset{
        .name = "test_pyramid",
        .ogfxPath = XRPHOTON_GALLERY_BLENDER_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_BLENDER_OGFX",
    },
    GalleryAsset{
        .name = "test_sphere",
        .ogfxPath = XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX",
    },
};
static_assert(BlenderSphereAsset + 1 == GalleryAssets.size());

const std::array GalleryPlacements{
    // Place every preview's vertical center on the y=0 screen row and keep every
    // placement origin at z=5.5. These X positions come from the actual compiled
    // vertices: at the startup 16:9 camera they center the complete six-placement
    // row and leave a 0.06-NDC gap between every adjacent silhouette.
    GalleryPlacement{
        .assetIndex = QuadAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-6.469370f, 0.0f, 5.5f}),
    },
    GalleryPlacement{
        .assetIndex = PlitkaAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-5.505742f, -1.431630f, 5.5f}),
    },
    GalleryPlacement{
        .assetIndex = BlenderPyramidAsset,
        // Perspective-center the source Y=[0, 2] bounds on the preview row.
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-1.673251f, -0.928571f, 5.5f}),
    },
    GalleryPlacement{
        .assetIndex = BlenderSphereAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{0.788628f, 0.0f, 5.5f}),
    },
    GalleryPlacement{
        .assetIndex = WedgeAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{3.123528f, 0.0f, 5.5f}),
    },
    GalleryPlacement{
        .assetIndex = WedgeAsset,
        // GLM applies the rightmost operation first: scale in model space,
        // rotate around world-up, then move the result into the gallery row.
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{5.548693f, -0.026433f, 5.5f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(30.0f),
                glm::vec3{0.0f, 1.0f, 0.0f})
            * glm::scale(
                glm::mat4{1.0f},
                glm::vec3{1.5f, 1.0f, 1.5f}),
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

std::size_t resolvedTextureCount(
    const SceneData& scene,
    const LoadedGalleryAsset& asset)
{
    std::size_t count = 0;
    const std::size_t materialEnd =
        static_cast<std::size_t>(asset.firstMaterial) + asset.materialCount;
    for (std::size_t materialIndex = asset.firstMaterial;
         materialIndex < materialEnd;
         ++materialIndex) {
        const uint32_t imageIndex = scene.materials[materialIndex].baseColorImage;
        if (imageIndex == 0) {
            continue;
        }

        bool firstUseInAsset = true;
        for (std::size_t previous = asset.firstMaterial;
             previous < materialIndex;
             ++previous) {
            if (scene.materials[previous].baseColorImage == imageIndex) {
                firstUseInAsset = false;
                break;
            }
        }
        if (firstUseInAsset) {
            ++count;
        }
    }
    return count;
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
                              << "': skipped (" << asset.configurationName
                              << " not configured).\n";
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
            metadata.geometryCount = static_cast<uint32_t>(geometryCount);
            metadata.firstMaterial = static_cast<uint32_t>(firstMaterial);
            metadata.materialCount = static_cast<uint32_t>(materialCount);
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

        const std::filesystem::path textureRoot{XRPHOTON_GALLERY_TEXTURE_ROOT};
        const ResolveTexturesResult textures = resolveSceneTextures(&scene, textureRoot);
        if (!textures) {
            if (textures.failedMaterial.has_value()) {
                const uint64_t failedMaterial = *textures.failedMaterial;
                for (std::size_t assetIndex = 0;
                     assetIndex < loadedAssets.size();
                     ++assetIndex) {
                    const LoadedGalleryAsset& loadedAsset = loadedAssets[assetIndex];
                    const uint64_t materialEnd =
                        static_cast<uint64_t>(loadedAsset.firstMaterial)
                        + loadedAsset.materialCount;
                    if (loadedAsset.loaded
                        && failedMaterial >= loadedAsset.firstMaterial
                        && failedMaterial < materialEnd) {
                        return fail(
                            entryDiagnosticPrefix(GalleryAssets[assetIndex])
                            + textures.error);
                    }
                }
                return fail(
                    "Gallery texture resolution failed for unowned material["
                    + std::to_string(failedMaterial) + "]: " + textures.error);
            }
            return fail("Gallery texture resolution failed: " + textures.error);
        }

        for (std::size_t assetIndex = 0;
             assetIndex < loadedAssets.size();
             ++assetIndex) {
            const LoadedGalleryAsset& loadedAsset = loadedAssets[assetIndex];
            if (!loadedAsset.loaded) {
                continue;
            }
            const std::size_t textureCount =
                resolvedTextureCount(scene, loadedAsset);
            std::cout << "Gallery entry '" << GalleryAssets[assetIndex].name
                      << "': loaded (" << loadedAsset.meshCount << ' '
                      << countLabel(loadedAsset.meshCount, "mesh", "meshes") << ", "
                      << loadedAsset.geometryCount << ' '
                      << countLabel(
                             loadedAsset.geometryCount,
                             "geometry",
                             "geometries")
                      << ", " << loadedAsset.materialCount << ' '
                      << countLabel(
                             loadedAsset.materialCount,
                             "material",
                             "materials")
                      << ", " << textureCount << ' '
                      << countLabel(textureCount, "resolved texture", "resolved textures")
                      << ").\n";
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
