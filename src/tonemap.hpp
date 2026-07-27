#pragma once

#include <cstdint>

namespace xrphoton
{
// CPU/shader ABI for the fixed tonemap pass. Exposure remains explicit even while it
// is fixed so later camera adaptation can change policy without changing the pipeline
// layout.
struct TonemapState
{
    float exposure = 1.0f;
};

inline constexpr TonemapState DefaultTonemapState{};
inline constexpr std::uint32_t TonemapLocalSizeX = 8;
inline constexpr std::uint32_t TonemapLocalSizeY = 8;

struct TonemapDispatch
{
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};

// Match the shader's [numthreads(8, 8, 1)] and round partial edge tiles up. The shader
// performs the corresponding bounds check before touching either storage image.
constexpr TonemapDispatch makeTonemapDispatch(std::uint32_t width, std::uint32_t height)
{
    return {
        width / TonemapLocalSizeX + (width % TonemapLocalSizeX != 0),
        height / TonemapLocalSizeY + (height % TonemapLocalSizeY != 0),
    };
}

static_assert(sizeof(TonemapState) == 4);
}
