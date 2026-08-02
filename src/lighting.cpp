#include "lighting.hpp"

namespace xrphoton
{
RaygenPushConstants makeRaygenPushConstants(
    const CameraPushConstants& camera,
    std::uint32_t frameIndex,
    std::uint32_t samplesPerPixel)
{
    const std::uint32_t acceptedSamplesPerPixel =
        isSupportedSamplesPerPixel(samplesPerPixel)
        ? samplesPerPixel
        : DefaultSamplesPerPixel;
    const CameraJitter jitter = cameraJitterForSample(
        frameIndex,
        acceptedSamplesPerPixel,
        0);
    RaygenPushConstants result{};
    result.camera = camera;
    result.frameIndex = frameIndex;
    result.cameraJitterX = jitter.x;
    result.cameraJitterY = jitter.y;
    result.samplesPerPixel = acceptedSamplesPerPixel;
    return result;
}
}
