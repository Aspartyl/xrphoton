#pragma once

#include <cstdint>

namespace xrphoton
{
// Runtime denoiser modes (TEMPORAL_DENOISING_PLAN.md). One filter core: Off records
// no denoise passes, Spatial filters each frame alone. Spatiotemporal joins the enum
// with plan phase D3.
enum class DenoiseMode : std::uint32_t
{
    Off = 0,
    Spatial = 1,
};

[[nodiscard]] constexpr DenoiseMode nextDenoiseMode(DenoiseMode mode)
{
    return mode == DenoiseMode::Off ? DenoiseMode::Spatial : DenoiseMode::Off;
}

[[nodiscard]] constexpr const char* denoiseModeName(DenoiseMode mode)
{
    return mode == DenoiseMode::Spatial ? "spatial" : "off";
}

// CPU/shader ABI for one a-trous iteration: the dilation stride and whether this
// final iteration remodulates albedo and writes the HDR radiance image instead of
// the ping-pong output.
struct DenoisePushConstants
{
    std::uint32_t stride = 1;
    std::uint32_t remodulate = 0;
};
static_assert(sizeof(DenoisePushConstants) == 8,
    "must match the DenoisePush block in denoise.slang");

// Five wavelet iterations at strides 1/2/4/8/16; compile-time by design, no
// runtime quality knobs beyond the mode itself.
inline constexpr std::uint32_t DenoiseAtrousIterationCount = 5;

inline constexpr std::uint32_t DenoiseLocalSizeX = 8;
inline constexpr std::uint32_t DenoiseLocalSizeY = 8;

struct DenoiseDispatch
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

// Match the shader's [numthreads(8, 8, 1)] and round partial edge tiles up; the
// shader bounds-checks before touching any storage image.
constexpr DenoiseDispatch makeDenoiseDispatch(std::uint32_t width, std::uint32_t height)
{
    return {
        width / DenoiseLocalSizeX + (width % DenoiseLocalSizeX != 0),
        height / DenoiseLocalSizeY + (height % DenoiseLocalSizeY != 0),
    };
}
}
