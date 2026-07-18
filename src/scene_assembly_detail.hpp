#pragma once

#include <cstdint>
#include <string>

namespace xrphoton::scene_assembly_detail
{
// Plain counts keep the uint32/24-bit boundary checks testable without allocating
// multi-gigabyte vectors. Production assembly derives both records from real vectors.
struct SceneElementCounts
{
    uint64_t vertices = 0;
    uint64_t indices = 0;
    uint64_t geometries = 0;
    uint64_t meshes = 0;
    uint64_t materials = 0;
};

[[nodiscard]] bool validateSceneAppendCounts(
    const SceneElementCounts& destination,
    const SceneElementCounts& source,
    std::string* error);
}
