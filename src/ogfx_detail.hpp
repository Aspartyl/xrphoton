#pragma once

#include "ogfx.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace xrphoton::ogfx::detail
{
static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(
    std::numeric_limits<std::size_t>::max() >= MaximumFileBytes,
    "the 1 GiB OGFx cap must fit size_t");

struct Bounds
{
    Position minimum{};
    Position maximum{};
};

constexpr std::string_view chunkName(std::uint32_t id)
{
    switch (static_cast<ChunkId>(id)) {
    case ChunkId::Model:
        return "OGFX_MODEL";
    case ChunkId::Geometries:
        return "OGFX_GEOMETRIES";
    case ChunkId::Meshes:
        return "OGFX_MESHES";
    case ChunkId::Materials:
        return "OGFX_MATERIALS";
    case ChunkId::Positions:
        return "OGFX_POSITIONS";
    case ChunkId::Attributes:
        return "OGFX_ATTRIBUTES";
    case ChunkId::Indices:
        return "OGFX_INDICES";
    case ChunkId::Description:
        return "OGFX_DESC";
    }

    return "unknown";
}

inline std::string indexedField(
    std::string_view collection,
    std::size_t index,
    std::string_view field)
{
    std::ostringstream name;
    name << collection << '[' << index << "]." << field;
    return name.str();
}

inline std::string formatDiagnostic(
    std::string_view diagnosticName,
    std::string_view operation,
    std::string_view chunkLabel,
    std::uint32_t chunkId,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    std::ostringstream message;
    message << diagnosticName << ": OGFx " << operation << ": chunk "
            << chunkLabel << " (0x" << std::hex << std::setw(4)
            << std::setfill('0') << chunkId << std::dec << "), field " << field
            << ": expected " << expected << ", found " << found;
    return message.str();
}

inline std::string makeChunkDiagnostic(
    std::string_view diagnosticName,
    std::string_view operation,
    std::uint32_t chunkId,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    return formatDiagnostic(
        diagnosticName,
        operation,
        chunkName(chunkId),
        chunkId,
        field,
        expected,
        found);
}

inline std::string makeFileDiagnostic(
    std::string_view diagnosticName,
    std::string_view operation,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    // File-level failures have no chunk ID. Zero keeps the established diagnostic
    // shape, while the explicit label avoids colliding with a real unknown chunk 0.
    return formatDiagnostic(
        diagnosticName,
        operation,
        "OGFX_FILE",
        0,
        field,
        expected,
        found);
}

inline bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t* result)
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }

    *result = left + right;
    return true;
}

inline bool checkedAlignUp(
    std::uint64_t value,
    std::uint64_t alignment,
    std::uint64_t* result)
{
    const std::uint64_t remainder = value % alignment;
    if (remainder == 0) {
        *result = value;
        return true;
    }

    return checkedAdd(value, alignment - remainder, result);
}

inline bool positionIsFinite(const Position& position)
{
    return std::isfinite(position.x)
        && std::isfinite(position.y)
        && std::isfinite(position.z);
}
}
