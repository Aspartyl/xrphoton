#pragma once

#include "ogfx.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace xrphoton::blender_mesh
{
// XRBM is a private, versioned Blender-to-compiler exchange stream. It is not a
// runtime asset format: Blender extracts source semantics into it and the shared
// C++ compiler remains the only owner of coordinate conversion and OGFx writing.
inline constexpr std::array<std::uint8_t, 4> StreamMagic{'X', 'R', 'B', 'M'};
inline constexpr std::uint32_t StreamVersion = 1;
inline constexpr std::uint32_t StreamHeaderSize = 96;
inline constexpr std::uint32_t CornerRecordSize = 32;
inline constexpr std::uint32_t CornersPerTriangle = 3;
inline constexpr std::uint32_t MaximumTriangleCount = 1'000'000;
inline constexpr std::uint32_t StreamFlagHasUvs = 1;
inline constexpr std::uint32_t SupportedStreamFlags = StreamFlagHasUvs;

// Decodes one static, material-free Blender mesh extraction. The result is an
// ordinary compiler-facing model and must still pass through serializeModel().
[[nodiscard]] ogfx::DecodeResult decodeStaticMesh(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName = "<Blender stream>");
}
