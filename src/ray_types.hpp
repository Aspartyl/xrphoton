#pragma once

#include <cstdint>

#ifndef XRPHOTON_RAY_TYPE_COUNT
#error "XRPHOTON_RAY_TYPE_COUNT must come from the shared CMake routing ABI"
#endif
#ifndef XRPHOTON_RADIANCE_RAY_TYPE
#error "XRPHOTON_RADIANCE_RAY_TYPE must come from the shared CMake routing ABI"
#endif
#ifndef XRPHOTON_SHADOW_RAY_TYPE
#error "XRPHOTON_SHADOW_RAY_TYPE must come from the shared CMake routing ABI"
#endif

namespace xrphoton
{
// These build-owned values drive the CPU SBT layout, TLAS instance offsets, scene
// capacity gate, and shader TraceRay routing. A disagreement between any of those
// sites is valid Vulkan that silently selects the wrong shader record.
inline constexpr std::uint32_t RadianceRayType = XRPHOTON_RADIANCE_RAY_TYPE;
inline constexpr std::uint32_t ShadowRayType = XRPHOTON_SHADOW_RAY_TYPE;
inline constexpr std::uint32_t RayTypeCount = XRPHOTON_RAY_TYPE_COUNT;
static_assert(RadianceRayType == 0);
static_assert(ShadowRayType == 1);
static_assert(ShadowRayType < RayTypeCount && RayTypeCount == 2);
}
