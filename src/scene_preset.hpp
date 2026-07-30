#pragma once

#include <cstdint>

namespace xrphoton
{
enum class ScenePreset : std::uint32_t
{
    Yard,
    Night,
};

enum class EstimatorMode : std::uint32_t
{
    Mis = 0,
    Nee = 1,
    Bsdf = 2,
};
}
