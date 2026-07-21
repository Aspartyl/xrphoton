#include "gallery.hpp"

#include "ogfx_loader.hpp"
#include "scene_assembly.hpp"
#include "texture_loader.hpp"

#include <array>
#include <cmath>
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
    bool animated = false;
};

enum GalleryAssetIndex : uint32_t
{
    YardGroundAsset = 0,
    YardWallAsset,
    YardBoxAsset,
    QuadAsset,
    WedgeAsset,
    PlitkaAsset,
    BlenderPyramidAsset,
    BlenderSphereAsset,
    BlenderSmoothSphereAsset,
    BlenderLeafCardAsset,
    BarrelAsset,
    RemadeBarrelAsset,
    CustomBarrelAsset,
    PseudodogTailAsset,
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

constexpr double Pi = 3.14159265358979323846;
constexpr double OrbitRadius = 3.0;
constexpr double OrbitHeight = 0.9;
constexpr double OrbitRadiansPerSecond = 0.6;
constexpr double SpinRadiansPerSecond = 1.7;

const GallerySpawn YardSpawn{
    .position = {-7.0f, 1.7f, -7.0f},
    .yaw = glm::radians(45.0f),
    .pitch = glm::radians(-5.0f),
};

constexpr std::array GalleryAssets{
    GalleryAsset{
        .name = "test_yard_ground",
        .ogfxPath = XRPHOTON_TEST_YARD_GROUND_ASSET_PATH,
        .optional = false,
        .configurationName = "XRPHOTON_TEST_YARD_GROUND_ASSET_PATH",
    },
    GalleryAsset{
        .name = "test_yard_wall",
        .ogfxPath = XRPHOTON_TEST_YARD_WALL_ASSET_PATH,
        .optional = false,
        .configurationName = "XRPHOTON_TEST_YARD_WALL_ASSET_PATH",
    },
    GalleryAsset{
        .name = "test_yard_box",
        .ogfxPath = XRPHOTON_TEST_YARD_BOX_ASSET_PATH,
        .optional = false,
        .configurationName = "XRPHOTON_TEST_YARD_BOX_ASSET_PATH",
    },
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
    GalleryAsset{
        .name = "test_smooth_sphere",
        .ogfxPath = XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX",
    },
    GalleryAsset{
        .name = "test_leaf_card",
        .ogfxPath = XRPHOTON_GALLERY_BLENDER_LEAF_CARD_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_BLENDER_LEAF_CARD_OGFX",
    },
    GalleryAsset{
        .name = "bochka_close_1",
        .ogfxPath = XRPHOTON_GALLERY_BARREL_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_BARREL_OGFX",
    },
    GalleryAsset{
        .name = "remade_bochka_close_1",
        .ogfxPath = XRPHOTON_GALLERY_REMADE_BARREL_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_REMADE_BARREL_OGFX",
    },
    GalleryAsset{
        .name = "custom_stalker_barrel",
        .ogfxPath = XRPHOTON_GALLERY_CUSTOM_BARREL_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_CUSTOM_BARREL_OGFX",
    },
    GalleryAsset{
        .name = "item_psevdodog_tail",
        .ogfxPath = XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX",
    },
};
static_assert(PseudodogTailAsset + 1 == GalleryAssets.size());

const std::array GalleryPlacements{
    GalleryPlacement{
        .assetIndex = YardGroundAsset,
        .transform = glm::mat4{1.0f},
    },
    GalleryPlacement{
        .assetIndex = YardWallAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{6.0f, -0.01f, 9.85f}),
    },
    GalleryPlacement{
        .assetIndex = YardWallAsset,
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{9.84f, -0.01f, 5.71f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(90.0f),
                glm::vec3{0.0f, 1.0f, 0.0f}),
    },
    GalleryPlacement{
        .assetIndex = YardBoxAsset,
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{5.0f, 0.49f, 5.0f})
            * glm::scale(
                glm::mat4{1.0f},
                glm::vec3{2.0f, 1.0f, 2.0f}),
    },
    GalleryPlacement{
        .assetIndex = YardBoxAsset,
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{5.0f, 0.115f, 1.59f})
            * glm::scale(
                glm::mat4{1.0f},
                glm::vec3{1.92f, 0.25f, 0.7f}),
    },
    GalleryPlacement{
        .assetIndex = YardBoxAsset,
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{5.0f, 0.24f, 2.28f})
            * glm::scale(
                glm::mat4{1.0f},
                glm::vec3{1.94f, 0.5f, 0.7f}),
    },
    GalleryPlacement{
        .assetIndex = YardBoxAsset,
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{5.0f, 0.365f, 2.97f})
            * glm::scale(
                glm::mat4{1.0f},
                glm::vec3{1.96f, 0.75f, 0.7f}),
    },
    GalleryPlacement{
        .assetIndex = YardBoxAsset,
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{5.0f, 0.49f, 3.66f})
            * glm::scale(
                glm::mat4{1.0f},
                glm::vec3{1.98f, 1.0f, 0.7f}),
    },
    GalleryPlacement{
        .assetIndex = YardBoxAsset,
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{-3.0f, 0.49f, 4.0f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(30.0f),
                glm::vec3{0.0f, 1.0f, 0.0f}),
    },
    GalleryPlacement{
        .assetIndex = YardBoxAsset,
        .transform = yardAnimatedTransform(0.0),
        .animated = true,
    },
    // Keep the low-level probes along the north-west edge, outside the yard's
    // central movement area but visible from the deliberate spawn.
    GalleryPlacement{
        .assetIndex = QuadAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-6.0f, 1.0f, 9.5f}),
    },
    GalleryPlacement{
        .assetIndex = PlitkaAsset,
        // Rotate the authored shallow Z depth against the east wall and bury the
        // base by one centimetre without changing the source scale.
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{9.64f, -0.01f, 7.0f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(90.0f),
                glm::vec3{0.0f, 1.0f, 0.0f}),
    },
    GalleryPlacement{
        .assetIndex = BlenderPyramidAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-8.0f, -0.01f, 1.5f}),
    },
    GalleryPlacement{
        .assetIndex = BlenderSphereAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-8.0f, 0.99f, 4.5f}),
    },
    GalleryPlacement{
        .assetIndex = BlenderSmoothSphereAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-8.0f, 0.99f, 7.0f}),
    },
    GalleryPlacement{
        .assetIndex = BlenderLeafCardAsset,
        // Turn the card's -Z face toward the south-west spawn while keeping it
        // clear of the east wall's inner face.
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{8.5f, -0.01f, 2.5f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(45.0f),
                glm::vec3{0.0f, 1.0f, 0.0f}),
    },
    GalleryPlacement{
        .assetIndex = WedgeAsset,
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-4.25f, 1.0f, 9.35f}),
    },
    GalleryPlacement{
        .assetIndex = WedgeAsset,
        // GLM applies the rightmost operation first: scale in model space,
        // rotate around world-up, then move the result along the probe shelf.
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{-2.1f, 1.0f, 9.0f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(30.0f),
                glm::vec3{0.0f, 1.0f, 0.0f})
            * glm::scale(
                glm::mat4{1.0f},
                glm::vec3{1.5f, 1.0f, 1.5f}),
    },
    GalleryPlacement{
        .assetIndex = BarrelAsset,
        // Translation-only placements preserve the three barrels' scale-faithful
        // comparison against the north wall.
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{4.2f, 0.0f, 9.2f}),
    },
    GalleryPlacement{
        .assetIndex = RemadeBarrelAsset,
        // The replacement deliberately preserves the SoC barrel's authored
        // dimensions; translate only, so the adjacent pair is a scale-faithful
        // visual quality comparison.
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{5.1f, 0.0f, 9.2f}),
    },
    GalleryPlacement{
        .assetIndex = CustomBarrelAsset,
        // This is a new design rather than a remake, but it intentionally keeps
        // the same believable one-metre drum scale. Translate only so all three
        // barrels remain an honest side-by-side comparison.
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{6.0f, 0.0f, 9.2f}),
    },
    GalleryPlacement{
        .assetIndex = PseudodogTailAsset,
        // Turn the long Z axis upward and rest its rotated lower bound just inside
        // the platform top; rotation does not alter authored scale.
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{5.0f, 1.26821f, 5.0f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(90.0f),
                glm::vec3{1.0f, 0.0f, 0.0f}),
    },
};

GalleryLoadResult fail(std::string error)
{
    return {
        .scene = {},
        .error = std::move(error),
        .animatedInstance = 0,
        .spawn = YardSpawn,
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

glm::mat4 yardAnimatedTransform(double seconds)
{
    const double orbitAngle = std::remainder(
        seconds,
        2.0 * Pi / OrbitRadiansPerSecond) * OrbitRadiansPerSecond;
    const double spinAngle = std::remainder(
        seconds,
        2.0 * Pi / SpinRadiansPerSecond) * SpinRadiansPerSecond;

    const glm::vec3 position{
        static_cast<float>(OrbitRadius * std::cos(orbitAngle)),
        static_cast<float>(OrbitHeight),
        static_cast<float>(-OrbitRadius * std::sin(orbitAngle)),
    };
    return glm::translate(glm::mat4{1.0f}, position)
        * glm::rotate(
            glm::mat4{1.0f},
            static_cast<float>(spinAngle),
            glm::vec3{0.0f, 1.0f, 0.0f});
}

GalleryLoadResult loadGalleryScene()
{
    try {
        std::size_t animatedPlacementCount = 0;
        for (const GalleryPlacement& placement : GalleryPlacements) {
            if (placement.animated) {
                ++animatedPlacementCount;
            }
        }
        if (animatedPlacementCount != 1) {
            return fail(
                "Gallery yard must contain exactly one animated placement; found "
                + std::to_string(animatedPlacementCount));
        }

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

        std::size_t animatedInstance = 0;
        bool animatedInstanceRecorded = false;
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
                if (placement.animated) {
                    return fail(
                        "Gallery placement[" + std::to_string(placementIndex)
                        + "] is animated but its asset is not configured");
                }
                continue;
            }
            if (placement.animated) {
                if (asset.meshCount != 1) {
                    return fail(
                        "Gallery placement[" + std::to_string(placementIndex)
                        + "] is animated but its asset contains "
                        + std::to_string(asset.meshCount) + " meshes; expected exactly 1");
                }
                animatedInstance = scene.instances.size();
                animatedInstanceRecorded = true;
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

        if (!animatedInstanceRecorded) {
            return fail("Gallery yard did not produce its animated scene instance");
        }

        if (!validateAssembledScene(scene, &assemblyError)) {
            return fail("Gallery scene validation failed: " + assemblyError);
        }

        const std::array textureRoots{
            std::filesystem::path{XRPHOTON_GALLERY_AUTHORED_TEXTURE_ROOT},
            std::filesystem::path{XRPHOTON_GALLERY_TEXTURE_ROOT},
        };
        const ResolveTexturesResult textures = resolveSceneTexturesFromRoots(
            &scene, textureRoots);
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
            .animatedInstance = animatedInstance,
            .spawn = YardSpawn,
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
