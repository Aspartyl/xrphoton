#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

namespace xrphoton
{
// All-scalar layout keeps the C++ record identical to the shader's natural layout.
struct VertexAttributes
{
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};
static_assert(sizeof(VertexAttributes) == 20,
    "must match the VertexAttributes record in raytrace.slang");

struct SceneGeometry
{
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t materialIndex = 0;
    // Classification belongs to a geometry because one mesh can mix opaque
    // surfaces and alpha-tested cards that require different AS/SBT handling.
    bool alphaTested = false;
};

struct SceneMaterial
{
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t baseColorImage = 0;
    float alphaCutoff = 0.5f;
    // Exact logical texture reference carried by OGFx. Resolution assigns the
    // scene-global baseColorImage later; an empty string means no reference.
    std::string baseColorTexture;
};

struct SceneMesh
{
    uint32_t firstGeometry = 0;
    uint32_t geometryCount = 0;
};

struct SceneInstance
{
    uint32_t meshIndex = 0;
    glm::mat4 transform{1.0f};
};

struct SceneImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
};

// Plain CPU scene data: no Vulkan handles and no resource ownership. main() keeps it
// alive for the program lifetime so the dynamic-scene step can reuse its transforms.
struct SceneData
{
    std::vector<float> positions;
    std::vector<VertexAttributes> attributes;
    std::vector<uint32_t> indices;
    std::vector<SceneGeometry> geometries;
    std::vector<SceneMesh> meshes;
    std::vector<SceneInstance> instances;
    std::vector<SceneMaterial> materials;
    std::vector<SceneImage> images;
};

}
