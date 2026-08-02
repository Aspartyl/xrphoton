#include "lighting.hpp"

namespace xrphoton
{
RaygenPushConstants makeRaygenPushConstants(
    const CameraPushConstants& camera,
    std::uint32_t frameIndex)
{
    const CameraJitter jitter = cameraJitterForFrame(frameIndex);
    RaygenPushConstants result{};
    result.camera = camera;
    result.frameIndex = frameIndex;
    result.cameraJitterX = jitter.x;
    result.cameraJitterY = jitter.y;
    return result;
}
}
