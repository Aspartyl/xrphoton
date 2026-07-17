#pragma once

#include "ogfx.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace xrphoton::legacy_ogf
{
inline constexpr std::uint8_t SupportedVersion = 4;
inline constexpr std::uint8_t SupportedModelType = 0;
inline constexpr std::uint16_t SupportedShaderId = 0;
inline constexpr std::uint32_t SupportedVertexFormat = 0x112;
inline constexpr std::uint32_t SourceVertexRecordSize = 32;
inline constexpr std::uint32_t MaximumSourceStringBytes = 255;

// Decodes only the pinned M4a legacy-static profile: one flat OGF v4 normal
// visual with direct HEADER/TEXTURE/VERTICES/INDICES chunks, FVF 0x112, and the
// (shader id 0, engine shader "default") opaque mapping. Unsupported source
// semantics are rejected rather than discarded. This first corpus slice accepts
// printable ASCII source names only; wider legacy code-page conversion is deferred.
// Coordinates and winding pass through unchanged; the canonical OGFx writer
// remains the sole output owner.
[[nodiscard]] ogfx::DecodeResult decodeStaticModel(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName = "<memory>");
}
