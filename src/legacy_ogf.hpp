#pragma once

#include "ogfx.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace xrphoton::legacy_ogf
{
inline constexpr std::uint8_t SupportedVersion = 4;
inline constexpr std::uint8_t SupportedModelType = 0;
inline constexpr std::uint8_t SupportedRigidModelType = 10;
inline constexpr std::uint8_t SupportedRigidChildModelType = 5;
inline constexpr std::uint16_t SupportedShaderId = 0;
inline constexpr std::uint32_t SupportedVertexFormat = 0x112;
inline constexpr std::uint32_t SupportedRigidVertexFormat = 0x12071980;
inline constexpr std::uint32_t SourceVertexRecordSize = 32;
inline constexpr std::uint32_t SourceRigidVertexRecordSize = 60;
inline constexpr std::uint32_t MaximumSourceStringBytes = 255;
inline constexpr std::uint32_t MaximumRigidBoneCount = 63;

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

// Dispatches between the pinned flat-static profile above and the first rigid-
// compound profile. The latter accepts an OGF v4 MT_SKELETON_RIGID root with
// embedded MT_SKELETON_GEOMDEF_ST children, one-link vertices, rigid/nonbreakable
// cylinder bones, and models\\model opaque materials. Its bind-pose render data is
// flattened into the ordinary compiler mesh while the authored compound body is
// retained through OGFx's engine-neutral rigid-physics records.
[[nodiscard]] ogfx::DecodeResult decodeModel(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName = "<memory>");
}
