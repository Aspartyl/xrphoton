#pragma once

#include "scene.hpp"

#include <cstdint>
#include <string>

namespace xrphoton
{
// Append one model's CPU records to a scene, rebasing every stored cross-record
// offset. OGFx models carry no placement or resolved images, so both corresponding
// source arrays must be empty. Failure leaves the destination scene's values intact.
[[nodiscard]] bool appendSceneModel(
    SceneData* scene,
    SceneData&& model,
    std::string* error);

// Add one world placement after its referenced mesh has been assembled.
[[nodiscard]] bool appendSceneInstance(
    SceneData* scene,
    uint32_t meshIndex,
    const glm::mat4& transform,
    std::string* error);

// Validate whole-scene invariants required by TLAS construction but not covered by
// per-model OGFx validation or the GPU-buffer upload boundary.
[[nodiscard]] bool validateAssembledScene(
    const SceneData& scene,
    std::string* error);
}
