#include "scene.hpp"

#include <glm/ext/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

namespace xrphoton
{
namespace
{
SceneData buildProceduralQuad()
{
    SceneData scene{};

    // Four shared vertices and two indexed triangles form one XY-plane quad.
    scene.positions = {
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.5f,  0.5f, 0.0f,
        -0.5f,  0.5f, 0.0f,
    };
    scene.attributes = {
        {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
    };
    scene.indices = {
        0, 1, 2,
        0, 2, 3,
    };
    scene.geometries.push_back({
        .firstVertex = 0,
        .vertexCount = 4,
        .firstIndex = 0,
        .indexCount = 6,
        .materialIndex = 0,
        .alphaTested = false,
    });
    scene.meshes.push_back({
        .firstGeometry = 0,
        .geometryCount = 1,
    });

    SceneInstance instance{};
    // Rotate about X so the Z-parallel normal visibly tilts; a Z rotation would leave
    // it unchanged and fail to probe the shader's object-to-world normal transform.
    instance.transform = glm::rotate(
        glm::mat4(1.0f),
        glm::radians(45.0f),
        glm::vec3(1.0f, 0.0f, 0.0f));
    scene.instances.push_back(instance);
    scene.materials.emplace_back();

    return scene;
}
}

SceneData createProceduralSceneData()
{
    return buildProceduralQuad();
}
}
