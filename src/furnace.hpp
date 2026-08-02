#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace xrphoton
{
inline constexpr float FurnaceEnvironmentRadiance = 1.0f;
inline constexpr std::size_t FurnaceRoughnessCount = 3;
inline constexpr std::size_t FurnaceMaterialClassCount = 3;
inline constexpr std::size_t FurnaceCaseCount =
    FurnaceRoughnessCount * FurnaceMaterialClassCount;

enum class FurnaceMaterialKind : std::uint32_t
{
    Dielectric,
    Metal,
    Glass,
};

struct FurnaceRegion
{
    std::uint32_t x0Numerator;
    std::uint32_t x1Numerator;
    std::uint32_t y0Numerator;
    std::uint32_t y1Numerator;
};

struct FurnaceCase
{
    FurnaceMaterialKind material;
    float perceptualRoughness;
    const char* name;
    FurnaceRegion region;
};

inline constexpr std::uint32_t FurnaceRegionDenominator = 100;
inline constexpr std::array FurnaceCases{
    FurnaceCase{FurnaceMaterialKind::Dielectric, 0.1f, "dielectric-low",
        {22, 28, 22, 28}},
    FurnaceCase{FurnaceMaterialKind::Dielectric, 0.5f, "dielectric-medium",
        {47, 53, 22, 28}},
    FurnaceCase{FurnaceMaterialKind::Dielectric, 0.9f, "dielectric-high",
        {72, 78, 22, 28}},
    FurnaceCase{FurnaceMaterialKind::Metal, 0.1f, "metal-low",
        {22, 28, 47, 53}},
    FurnaceCase{FurnaceMaterialKind::Metal, 0.5f, "metal-medium",
        {47, 53, 47, 53}},
    FurnaceCase{FurnaceMaterialKind::Metal, 0.9f, "metal-high",
        {72, 78, 47, 53}},
    FurnaceCase{FurnaceMaterialKind::Glass, 0.1f, "glass-low",
        {22, 28, 72, 78}},
    FurnaceCase{FurnaceMaterialKind::Glass, 0.5f, "glass-medium",
        {47, 53, 72, 78}},
    FurnaceCase{FurnaceMaterialKind::Glass, 0.9f, "glass-high",
        {72, 78, 72, 78}},
};

inline constexpr auto FurnaceRegions = [] {
    std::array<FurnaceRegion, FurnaceCaseCount> regions{};
    for (std::size_t index = 0; index < regions.size(); ++index) {
        regions[index] = FurnaceCases[index].region;
    }
    return regions;
}();

[[nodiscard]] constexpr const char* furnaceCaseName(std::size_t index)
{
    return index < FurnaceCases.size() ? FurnaceCases[index].name : "invalid";
}
}
