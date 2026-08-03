#include "gallery.hpp"

#include "furnace.hpp"
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
    // This generated asset is not part of the gameplay yard's loaded model set.
    bool denoiseProbeOnly = false;
};

struct GalleryPlacement
{
    uint32_t assetIndex;
    glm::mat4 transform;
    bool dynamic = false;
    // Acceptance-only placements exist solely for the GBufferProbe profile; the
    // ordinary yard omits them so gameplay and Off-mode capture hashes stay
    // byte-identical across denoising phases.
    bool probeOnly = false;
    bool denoiseProbeOnly = false;
};

enum GalleryAssetIndex : uint32_t
{
    YardGroundAsset = 0,
    YardWallAsset,
    YardBoxAsset,
    GlassPanelAsset,
    DenoiseGlassSphereAsset,
    QuadAsset,
    TexturedProbeAsset,
    WedgeAsset,
    PlitkaAsset,
    BlenderPyramidAsset,
    BlenderSphereAsset,
    BlenderSmoothSphereAsset,
    BlenderShinySphereAsset,
    BlenderDynamicGlassSphereAsset,
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
    uint32_t physicsBodyCount = 0;
};

const GallerySpawn YardSpawn{
    .position = {-7.0f, 1.7f, -7.0f},
    .yaw = glm::radians(45.0f),
    .pitch = glm::radians(-5.0f),
};

const GallerySpawn FurnaceSpawn{
    .position = {0.0f, 0.0f, 0.0f},
};

GalleryLoadResult fail(std::string error);

SceneMaterialClass furnaceMaterialClass(FurnaceMaterialKind kind)
{
    switch (kind) {
    case FurnaceMaterialKind::Metal:
        return SceneMaterialClass::Metal;
    case FurnaceMaterialKind::Glass:
        return SceneMaterialClass::Glass;
    default:
        return SceneMaterialClass::Dielectric;
    }
}

GalleryLoadResult makeFurnaceScene()
{
    SceneData scene;
    // Twenty-four vertices keep face normals exact at the cube's hard edges. Each
    // case reuses this immutable cube range through a material-specific geometry.
    constexpr std::array<glm::vec3, 24> Positions{
        // +X
        glm::vec3{0.5f, -0.5f, -0.5f}, glm::vec3{0.5f, 0.5f, -0.5f},
        glm::vec3{0.5f, 0.5f, 0.5f}, glm::vec3{0.5f, -0.5f, 0.5f},
        // -X
        glm::vec3{-0.5f, -0.5f, 0.5f}, glm::vec3{-0.5f, 0.5f, 0.5f},
        glm::vec3{-0.5f, 0.5f, -0.5f}, glm::vec3{-0.5f, -0.5f, -0.5f},
        // +Y
        glm::vec3{-0.5f, 0.5f, -0.5f}, glm::vec3{-0.5f, 0.5f, 0.5f},
        glm::vec3{0.5f, 0.5f, 0.5f}, glm::vec3{0.5f, 0.5f, -0.5f},
        // -Y
        glm::vec3{-0.5f, -0.5f, 0.5f}, glm::vec3{-0.5f, -0.5f, -0.5f},
        glm::vec3{0.5f, -0.5f, -0.5f}, glm::vec3{0.5f, -0.5f, 0.5f},
        // +Z
        glm::vec3{0.5f, -0.5f, 0.5f}, glm::vec3{0.5f, 0.5f, 0.5f},
        glm::vec3{-0.5f, 0.5f, 0.5f}, glm::vec3{-0.5f, -0.5f, 0.5f},
        // -Z, facing the camera
        glm::vec3{-0.5f, -0.5f, -0.5f}, glm::vec3{-0.5f, 0.5f, -0.5f},
        glm::vec3{0.5f, 0.5f, -0.5f}, glm::vec3{0.5f, -0.5f, -0.5f},
    };
    constexpr std::array<glm::vec3, 6> Normals{
        glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{-1.0f, 0.0f, 0.0f},
        glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{0.0f, -1.0f, 0.0f},
        glm::vec3{0.0f, 0.0f, 1.0f}, glm::vec3{0.0f, 0.0f, -1.0f},
    };
    constexpr std::array<std::uint32_t, 6> FaceIndices{0, 1, 2, 0, 2, 3};

    scene.positions.reserve(Positions.size() * 3);
    scene.attributes.reserve(Positions.size());
    scene.indices.reserve(Normals.size() * FaceIndices.size());
    for (std::size_t vertex = 0; vertex < Positions.size(); ++vertex) {
        const glm::vec3 position = Positions[vertex];
        const glm::vec3 normal = Normals[vertex / 4];
        scene.positions.insert(
            scene.positions.end(), {position.x, position.y, position.z});
        scene.attributes.push_back({
            .nx = normal.x,
            .ny = normal.y,
            .nz = normal.z,
            .u = static_cast<float>(vertex % 4 == 2 || vertex % 4 == 3),
            .v = static_cast<float>(vertex % 4 >= 2),
        });
    }
    for (std::size_t face = 0; face < Normals.size(); ++face) {
        for (const std::uint32_t localIndex : FaceIndices) {
            scene.indices.push_back(
                static_cast<std::uint32_t>(face * 4) + localIndex);
        }
    }

    constexpr std::array<float, FurnaceRoughnessCount> XPositions{-4.1f, 0.0f, 4.1f};
    constexpr std::array<float, FurnaceMaterialClassCount> YPositions{2.31f, 0.0f, -2.31f};
    scene.materials.reserve(FurnaceCases.size());
    scene.geometries.reserve(FurnaceCases.size());
    scene.meshes.reserve(FurnaceCases.size());
    scene.instances.reserve(FurnaceCases.size());
    for (std::size_t caseIndex = 0; caseIndex < FurnaceCases.size(); ++caseIndex) {
        const FurnaceCase& furnaceCase = FurnaceCases[caseIndex];
        SceneMaterial material;
        material.perceptualRoughness = furnaceCase.perceptualRoughness;
        material.materialClass = furnaceMaterialClass(furnaceCase.material);
        scene.materials.push_back(material);
        scene.geometries.push_back({
            .firstVertex = 0,
            .vertexCount = static_cast<std::uint32_t>(Positions.size()),
            .firstIndex = 0,
            .indexCount = static_cast<std::uint32_t>(scene.indices.size()),
            .materialIndex = static_cast<std::uint32_t>(caseIndex),
        });
        scene.meshes.push_back({
            .firstGeometry = static_cast<std::uint32_t>(caseIndex),
            .geometryCount = 1,
        });
        const std::size_t materialRow = caseIndex / FurnaceRoughnessCount;
        const std::size_t roughnessColumn = caseIndex % FurnaceRoughnessCount;
        scene.instances.push_back({
            .meshIndex = static_cast<std::uint32_t>(caseIndex),
            .transform = glm::translate(
                    glm::mat4{1.0f},
                    glm::vec3{
                        XPositions[roughnessColumn],
                        YPositions[materialRow],
                        8.5f})
                * glm::scale(glm::mat4{1.0f}, glm::vec3{2.4f, 1.7f, 1.0f}),
        });
    }

    std::string error;
    if (!validateAssembledScene(scene, &error)) {
        return fail("Furnace scene validation failed: " + error);
    }
    const ResolveTexturesResult textures = resolveSceneTexturesFromRoots(&scene, {});
    if (!textures) {
        return fail("Furnace texture resolution failed: " + textures.error);
    }
    return {
        .scene = std::move(scene),
        .error = {},
        .dynamicInstances = {},
        .spawn = FurnaceSpawn,
    };
}

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
        .name = "test_glass_panel",
        .ogfxPath = XRPHOTON_TEST_GLASS_PANEL_ASSET_PATH,
        .optional = false,
        .configurationName = "XRPHOTON_TEST_GLASS_PANEL_ASSET_PATH",
    },
    GalleryAsset{
        .name = "denoise_glass_sphere",
        .ogfxPath = XRPHOTON_DENOISE_GLASS_SPHERE_ASSET_PATH,
        .optional = false,
        .configurationName = "XRPHOTON_DENOISE_GLASS_SPHERE_ASSET_PATH",
        .denoiseProbeOnly = true,
    },
    GalleryAsset{
        .name = "test_quad",
        .ogfxPath = XRPHOTON_TEST_QUAD_ASSET_PATH,
        .optional = false,
        .configurationName = "XRPHOTON_TEST_QUAD_ASSET_PATH",
    },
    GalleryAsset{
        .name = "textured_probe",
        .ogfxPath = XRPHOTON_TEST_TEXTURED_PROBE_ASSET_PATH,
        .optional = false,
        .configurationName = "XRPHOTON_TEST_TEXTURED_PROBE_ASSET_PATH",
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
        .name = "yard_shiny_sphere",
        .ogfxPath = XRPHOTON_GALLERY_BLENDER_SHINY_SPHERE_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_BLENDER_SHINY_SPHERE_OGFX",
    },
    GalleryAsset{
        .name = "dynamic_glass_sphere",
        .ogfxPath = XRPHOTON_GALLERY_BLENDER_DYNAMIC_GLASS_SPHERE_OGFX,
        .optional = true,
        .configurationName = "XRPHOTON_GALLERY_BLENDER_DYNAMIC_GLASS_SPHERE_OGFX",
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
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{3.0f, 2.5f, 0.0f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(25.0f),
                glm::vec3{0.0f, 1.0f, 0.0f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(12.0f),
                glm::vec3{1.0f, 0.0f, 0.0f}),
        .dynamic = true,
    },
    GalleryPlacement{
        .assetIndex = GlassPanelAsset,
        // Four closed vertical slabs form a ground-mounted material display in the
        // open south-west strip. Their combined bounds stay clear of the large sphere,
        // optional west-side props, player spawn, and every night-only emitter.
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-2.5f, 0.0f, -3.8f}),
    },
    GalleryPlacement{
        .assetIndex = DenoiseGlassSphereAsset,
        // Static, generated, and centered on the fixed capture view. It never
        // enters the gameplay yard or depends on owner-local Blender assets.
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{
                             DenoiseProbeGlassSphereCenter[0],
                             DenoiseProbeGlassSphereCenter[1],
                             DenoiseProbeGlassSphereCenter[2]})
            * glm::scale(
                glm::mat4{1.0f},
                glm::vec3{DenoiseProbeGlassSphereRadius}),
        .denoiseProbeOnly = true,
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
        .assetIndex = TexturedProbeAsset,
        // The DXT1-sampled probe card faces the spawn from the open south-west
        // strip, west of the glass-panel row's sight lines, so the G-buffer
        // oracle's albedo pixel stays permanently unoccluded. Placed before every
        // optional entry to keep its flat instance index configuration-stable,
        // and probe-only so the ordinary yard image never changes.
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-6.4f, 0.9f, -3.0f}),
        .probeOnly = true,
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
        .assetIndex = BlenderShinySphereAsset,
        // A five-metre-diameter chrome sphere anchors the otherwise open
        // south-east quarter. Its three-metre south margin and wider open
        // approaches leave a safe, complete walking loop around the prop.
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{4.5f, 2.5f, -4.5f})
            * glm::scale(
                glm::mat4{1.0f},
                glm::vec3{2.5f}),
    },
    GalleryPlacement{
        .assetIndex = BlenderDynamicGlassSphereAsset,
        // The asset is authored with its bottom at local Y=0. This open west-side
        // patch gives the player room to push and roll it without intersecting the
        // glass-panel row, static probes, spawn, or night-only lights.
        .transform = glm::translate(
            glm::mat4{1.0f},
            glm::vec3{-5.5f, 0.0f, -0.5f}),
        .dynamic = true,
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
        .transform = glm::translate(
                         glm::mat4{1.0f},
                         glm::vec3{4.2f, 0.6f, 9.2f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(20.0f),
                glm::vec3{0.0f, 0.0f, 1.0f}),
        .dynamic = true,
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
        .dynamic = true,
    },
};

GalleryLoadResult fail(std::string error)
{
    return {
        .scene = {},
        .error = std::move(error),
        .dynamicInstances = {},
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

bool appendYardEmitterModel(
    SceneData* scene,
    std::uint32_t* firstMesh,
    std::string* error)
{
    if (scene == nullptr || firstMesh == nullptr || error == nullptr
        || scene->meshes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    SceneData model;
    const glm::vec3 emissions[] = {
        {28.0f, 15.0f, 6.0f},
        {45.0f, 10.0f, 1.5f},
        {5.0f, 14.0f, 38.0f},
    };
    for (const glm::vec3 emission : emissions) {
        const std::uint32_t firstVertex =
            static_cast<std::uint32_t>(model.positions.size() / 3);
        const std::uint32_t firstIndex =
            static_cast<std::uint32_t>(model.indices.size());
        const std::uint32_t materialIndex =
            static_cast<std::uint32_t>(model.materials.size());
        const std::uint32_t geometryIndex =
            static_cast<std::uint32_t>(model.geometries.size());
        model.positions.insert(model.positions.end(), {
            -0.5f, -0.5f, 0.0f,
             0.5f, -0.5f, 0.0f,
             0.5f,  0.5f, 0.0f,
            -0.5f,  0.5f, 0.0f,
        });
        for (std::uint32_t vertex = 0; vertex < 4; ++vertex) {
            model.attributes.push_back({
                .nx = 0.0f,
                .ny = 0.0f,
                .nz = 1.0f,
                .u = vertex == 1 || vertex == 2 ? 1.0f : 0.0f,
                .v = vertex >= 2 ? 1.0f : 0.0f,
            });
        }
        model.indices.insert(model.indices.end(), {0, 1, 2, 0, 2, 3});
        model.geometries.push_back({
            .firstVertex = firstVertex,
            .vertexCount = 4,
            .firstIndex = firstIndex,
            .indexCount = 6,
            .materialIndex = materialIndex,
        });
        SceneMaterial material;
        material.baseColorFactor[0] = emission.x / 45.0f;
        material.baseColorFactor[1] = emission.y / 45.0f;
        material.baseColorFactor[2] = emission.z / 45.0f;
        material.emission[0] = emission.x;
        material.emission[1] = emission.y;
        material.emission[2] = emission.z;
        model.materials.push_back(std::move(material));
        model.meshes.push_back({
            .firstGeometry = geometryIndex,
            .geometryCount = 1,
        });
    }

    *firstMesh = static_cast<std::uint32_t>(scene->meshes.size());
    return appendSceneModel(scene, std::move(model), error);
}

bool appendYardEmitters(SceneData* scene, std::string* error)
{
    std::uint32_t firstMesh = 0;
    if (!appendYardEmitterModel(scene, &firstMesh, error)) {
        return false;
    }

    struct EmitterPlacement
    {
        std::uint32_t localMesh;
        glm::vec3 position;
        glm::vec3 scale;
        float yawDegrees;
        float pitchDegrees;
    };
    const EmitterPlacement placements[] = {
        // Keep wall lamps visibly detached from the wall's inner faces. Coplanar
        // emitters make finite NEE segments and BSDF-hit rays see different endpoint
        // geometry, invalidating the estimator-consistency proof.
        {0, {-6.5f, 2.2f, 9.60f}, {0.35f, 0.35f, 1.0f}, 180.0f, 0.0f},
        {0, {-4.5f, 2.2f, 9.60f}, {0.35f, 0.35f, 1.0f}, 180.0f, 0.0f},
        {0, {-2.5f, 2.2f, 9.60f}, {0.35f, 0.35f, 1.0f}, 180.0f, 0.0f},
        {0, {-0.5f, 2.2f, 9.60f}, {0.35f, 0.35f, 1.0f}, 180.0f, 0.0f},
        {0, { 1.5f, 2.2f, 9.60f}, {0.35f, 0.35f, 1.0f}, 180.0f, 0.0f},
        {0, { 3.5f, 2.2f, 9.60f}, {0.35f, 0.35f, 1.0f}, 180.0f, 0.0f},
        {0, { 5.5f, 2.2f, 9.60f}, {0.35f, 0.35f, 1.0f}, 180.0f, 0.0f},
        {0, { 7.5f, 2.2f, 9.60f}, {0.35f, 0.35f, 1.0f}, 180.0f, 0.0f},
        {0, {9.59f, 2.2f,  8.5f}, {0.35f, 0.35f, 1.0f}, -90.0f, 0.0f},
        {0, {9.59f, 2.2f,  6.5f}, {0.35f, 0.35f, 1.0f}, -90.0f, 0.0f},
        {0, {9.59f, 2.2f,  4.5f}, {0.35f, 0.35f, 1.0f}, -90.0f, 0.0f},
        {0, {9.59f, 2.2f,  2.5f}, {0.35f, 0.35f, 1.0f}, -90.0f, 0.0f},
        {0, {9.59f, 2.2f,  0.5f}, {0.35f, 0.35f, 1.0f}, -90.0f, 0.0f},
        {0, {9.59f, 2.2f, -1.5f}, {0.35f, 0.35f, 1.0f}, -90.0f, 0.0f},
        {0, {4.1f, 1.4f, 4.2f}, {0.25f, 0.25f, 1.0f}, 225.0f, 0.0f},
        {0, {5.9f, 1.4f, 4.2f}, {0.25f, 0.25f, 1.0f}, 135.0f, 0.0f},
        {1, {0.0f, 0.04f, 1.5f}, {1.2f, 1.2f, 1.0f}, 0.0f, -90.0f},
        {2, {-3.5f, 0.8f, 2.0f}, {0.45f, 0.45f, 1.0f}, 45.0f, 0.0f},
        {2, {-2.7f, 1.2f, 2.8f}, {0.35f, 0.35f, 1.0f}, 45.0f, 0.0f},
        {2, {-4.2f, 1.5f, 3.2f}, {0.30f, 0.30f, 1.0f}, 45.0f, 0.0f},
        {2, {-3.2f, 1.9f, 3.8f}, {0.25f, 0.25f, 1.0f}, 45.0f, 0.0f},
    };

    for (const EmitterPlacement& placement : placements) {
        const glm::mat4 transform = glm::translate(
                glm::mat4{1.0f},
                placement.position)
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(placement.yawDegrees),
                glm::vec3{0.0f, 1.0f, 0.0f})
            * glm::rotate(
                glm::mat4{1.0f},
                glm::radians(placement.pitchDegrees),
                glm::vec3{1.0f, 0.0f, 0.0f})
            * glm::scale(glm::mat4{1.0f}, placement.scale);
        if (!appendSceneInstance(
                scene,
                firstMesh + placement.localMesh,
                transform,
                error)) {
            return false;
        }
    }
    return true;
}
}

GalleryLoadResult loadGalleryScene(
    GallerySceneProfile profile)
{
    try {
        if (profile == GallerySceneProfile::FurnaceReference) {
            return makeFurnaceScene();
        }
        SceneData scene{};
        std::array<LoadedGalleryAsset, GalleryAssets.size()> loadedAssets{};
        std::string assemblyError;

        for (std::size_t assetIndex = 0;
             assetIndex < GalleryAssets.size();
             ++assetIndex) {
            const GalleryAsset& asset = GalleryAssets[assetIndex];
            if (asset.denoiseProbeOnly
                && profile != GallerySceneProfile::DenoiseProbe) {
                continue;
            }
            if (profile == GallerySceneProfile::EstimatorReference
                && asset.optional) {
                std::cout << "Gallery entry '" << asset.name
                          << "': skipped (estimator-reference profile).\n";
                continue;
            }
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
            const std::size_t physicsBodyCount = loaded.scene.physicsBodies.size();
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
            metadata.physicsBodyCount = static_cast<uint32_t>(physicsBodyCount);
        }

        std::vector<std::size_t> dynamicInstances;
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
            if (placement.probeOnly
                && profile != GallerySceneProfile::GBufferProbe) {
                continue;
            }
            if (placement.denoiseProbeOnly
                && profile != GallerySceneProfile::DenoiseProbe) {
                continue;
            }
            if (profile == GallerySceneProfile::EstimatorReference) {
                bool usesApproximateGlassVisibility = false;
                for (std::uint64_t materialOffset = 0;
                     materialOffset < asset.materialCount;
                     ++materialOffset) {
                    const std::uint64_t materialIndex =
                        static_cast<std::uint64_t>(asset.firstMaterial)
                        + materialOffset;
                    if (materialIndex < scene.materials.size()
                        && scene.materials[static_cast<std::size_t>(materialIndex)]
                               .materialClass == SceneMaterialClass::Glass) {
                        usesApproximateGlassVisibility = true;
                        break;
                    }
                }
                // P3's straight-line visibility through intervening Glass is a
                // deliberate production approximation. Omitting Glass placements
                // keeps the MIS/NEE/BSDF acceptance profiles on one exact integrand.
                if (usesApproximateGlassVisibility) {
                    continue;
                }
            }
            if (placement.dynamic) {
                if (asset.meshCount != 1) {
                    return fail(
                        "Gallery placement[" + std::to_string(placementIndex)
                        + "] is dynamic but its asset contains "
                        + std::to_string(asset.meshCount) + " meshes; expected exactly 1");
                }
                if (asset.physicsBodyCount != 1) {
                    return fail(
                        "Gallery placement[" + std::to_string(placementIndex)
                        + "] is dynamic but its asset contains "
                        + std::to_string(asset.physicsBodyCount)
                        + " physics bodies; expected exactly 1");
                }
                dynamicInstances.push_back(scene.instances.size());
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

        if (dynamicInstances.empty()) {
            return fail("Gallery yard did not produce a dynamic scene instance");
        }

        if (!appendYardEmitters(&scene, &assemblyError)) {
            return fail("Gallery emitters failed: " + assemblyError);
        }

        if (!validateAssembledScene(scene, &assemblyError)) {
            return fail("Gallery scene validation failed: " + assemblyError);
        }

        const std::array textureRoots{
            std::filesystem::path{XRPHOTON_PROBE_TEXTURE_ROOT},
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
            .dynamicInstances = std::move(dynamicInstances),
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
