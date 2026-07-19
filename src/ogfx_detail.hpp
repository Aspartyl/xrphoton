#pragma once

#include "ogfx.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <span>
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
    case ChunkId::RigidPhysics:
        return "OGFX_RIGID_PHYSICS";
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

struct CanonicalModelCounts
{
    std::uint32_t geometryCount = 0;
    std::uint32_t meshCount = 0;
    std::uint32_t materialCount = 0;
    std::uint32_t materialStringBytes = 0;
    std::uint32_t positionCount = 0;
    std::uint32_t attributeCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t physicsBodyCount = 0;
    std::uint32_t physicsColliderCount = 0;
    std::uint32_t physicsStringBytes = 0;
};

// The canonical writer and bounded source adapters share this one size formula.
// Every count is u32, so the pinned v1 record products and their sum fit u64.
inline std::uint64_t canonicalModelFileBytes(const CanonicalModelCounts& counts)
{
    auto addChunk = [](std::uint64_t payloadBytes, std::uint64_t* fileBytes) {
        const std::uint64_t remainder = *fileBytes % ChunkAlignment;
        if (remainder != 0) {
            *fileBytes += ChunkAlignment - remainder;
        }
        *fileBytes += ChunkHeaderSize + payloadBytes;
    };

    const std::uint64_t geometryBytes =
        static_cast<std::uint64_t>(counts.geometryCount) * GeometryRecordSize;
    const std::uint64_t meshBytes =
        static_cast<std::uint64_t>(counts.meshCount) * MeshRecordSize;
    const std::uint64_t materialBytes = MaterialHeaderSize
        + static_cast<std::uint64_t>(counts.materialCount) * MaterialRecordSize
        + counts.materialStringBytes;
    const std::uint64_t positionBytes =
        static_cast<std::uint64_t>(counts.positionCount) * PositionRecordSize;
    const std::uint64_t attributeBytes =
        static_cast<std::uint64_t>(counts.attributeCount) * AttributeRecordSize;
    const std::uint64_t indexBytes =
        static_cast<std::uint64_t>(counts.indexCount) * IndexRecordSize;
    const std::uint64_t rigidPhysicsBytes = RigidPhysicsHeaderSize
        + static_cast<std::uint64_t>(counts.physicsBodyCount) * PhysicsBodyRecordSize
        + static_cast<std::uint64_t>(counts.physicsColliderCount)
            * PhysicsColliderRecordSize
        + counts.physicsStringBytes;

    std::uint64_t fileBytes = FileHeaderSize;
    addChunk(ModelRecordSize, &fileBytes);
    addChunk(geometryBytes, &fileBytes);
    addChunk(meshBytes, &fileBytes);
    addChunk(materialBytes, &fileBytes);
    addChunk(positionBytes, &fileBytes);
    addChunk(attributeBytes, &fileBytes);
    addChunk(indexBytes, &fileBytes);
    if (counts.physicsBodyCount != 0 || counts.physicsColliderCount != 0) {
        addChunk(rigidPhysicsBytes, &fileBytes);
    }
    return fileBytes;
}

inline bool positionIsFinite(const Position& position)
{
    return std::isfinite(position.x)
        && std::isfinite(position.y)
        && std::isfinite(position.z);
}

inline bool validUtf8(std::span<const std::uint8_t> bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::uint8_t first = bytes[offset];
        if (first <= 0x7f) {
            ++offset;
            continue;
        }

        std::size_t length = 0;
        std::uint8_t secondMinimum = 0x80;
        std::uint8_t secondMaximum = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
        } else if (first >= 0xe0 && first <= 0xef) {
            length = 3;
            if (first == 0xe0) {
                secondMinimum = 0xa0;
            } else if (first == 0xed) {
                secondMaximum = 0x9f;
            }
        } else if (first >= 0xf0 && first <= 0xf4) {
            length = 4;
            if (first == 0xf0) {
                secondMinimum = 0x90;
            } else if (first == 0xf4) {
                secondMaximum = 0x8f;
            }
        } else {
            return false;
        }

        if (length > bytes.size() - offset) {
            return false;
        }
        if (bytes[offset + 1] < secondMinimum || bytes[offset + 1] > secondMaximum) {
            return false;
        }
        for (std::size_t index = 2; index < length; ++index) {
            if (bytes[offset + index] < 0x80 || bytes[offset + index] > 0xbf) {
                return false;
            }
        }
        offset += length;
    }

    return true;
}

inline bool validUtf8(std::string_view text)
{
    return validUtf8(std::span{
        reinterpret_cast<const std::uint8_t*>(text.data()),
        text.size(),
    });
}
}
