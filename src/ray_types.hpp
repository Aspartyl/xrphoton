#pragma once

#include <cstdint>

#ifndef XRPHOTON_RAY_TYPE_COUNT
#error "XRPHOTON_RAY_TYPE_COUNT must come from the shared CMake build definition"
#endif

namespace xrphoton
{
// One build-owned value drives the CPU SBT layout, TLAS instance offsets, scene
// capacity gate, and the shader TraceRay multiplier. A disagreement between any of
// those sites is valid Vulkan that silently selects the wrong shader record.
inline constexpr uint32_t RayTypeCount = XRPHOTON_RAY_TYPE_COUNT;
static_assert(RayTypeCount > 0);
}
