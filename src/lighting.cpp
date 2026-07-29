#include "lighting.hpp"

namespace xrphoton
{
RaygenPushConstants makeRaygenPushConstants(
    const CameraPushConstants& camera,
    std::uint32_t frameIndex,
    float cameraJitterX,
    float cameraJitterY)
{
    RaygenPushConstants result{};
    result.camera = camera;
    result.frameIndex = frameIndex;
    result.cameraJitterX = cameraJitterX;
    result.cameraJitterY = cameraJitterY;
    return result;
}
}
