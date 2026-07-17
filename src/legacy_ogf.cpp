#include "legacy_ogf.hpp"

#include "ogfx_detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace xrphoton::legacy_ogf
{
namespace
{
constexpr std::uint32_t HeaderChunkId = 0x1;
constexpr std::uint32_t TextureChunkId = 0x2;
constexpr std::uint32_t VerticesChunkId = 0x3;
constexpr std::uint32_t IndicesChunkId = 0x4;
constexpr std::uint32_t CompressionMark = 0x80000000u;
constexpr std::uint32_t LegacyChunkHeaderSize = 8;
constexpr std::uint32_t HeaderPayloadSize = 44;
constexpr std::uint32_t VertexPayloadHeaderSize = 8;
constexpr std::uint32_t IndexPayloadHeaderSize = 4;

using ogfx::detail::positionIsFinite;

constexpr std::array RequiredChunkIds{
    HeaderChunkId,
    TextureChunkId,
    VerticesChunkId,
    IndicesChunkId,
};

struct ChunkView
{
    std::uint32_t id = 0;
    std::size_t payloadOffset = 0;
    std::uint32_t payloadSize = 0;
};

struct SourceBounds
{
    ogfx::Position minimum{};
    ogfx::Position maximum{};
    ogfx::Position sphereCenter{};
    float sphereRadius = 0.0f;
};

std::string chunkName(std::uint32_t id)
{
    switch (id) {
    case HeaderChunkId: return "OGF_HEADER";
    case TextureChunkId: return "OGF_TEXTURE";
    case VerticesChunkId: return "OGF_VERTICES";
    case IndicesChunkId: return "OGF_INDICES";
    default: return "OGF_UNKNOWN";
    }
}

std::string hexadecimal(std::uint64_t value)
{
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

std::string describeText(std::string_view value)
{
    std::ostringstream text;
    text << '"';
    for (unsigned char byte : value) {
        if (byte >= 0x20 && byte <= 0x7e && byte != '\\' && byte != '"') {
            text << static_cast<char>(byte);
        } else if (byte == '\\' || byte == '"') {
            text << '\\' << static_cast<char>(byte);
        } else {
            text << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned int>(byte) << std::dec;
        }
    }
    text << '"';
    return text.str();
}

std::string makeChunkDiagnostic(
    std::string_view diagnosticName,
    std::uint32_t chunkId,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    std::ostringstream message;
    message << diagnosticName << ": legacy OGF decoder: chunk "
            << chunkName(chunkId) << " (" << hexadecimal(chunkId)
            << "), field " << field << ": expected " << expected
            << ", found " << found << '.';
    return message.str();
}

std::string makeFileDiagnostic(
    std::string_view diagnosticName,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    std::ostringstream message;
    message << diagnosticName << ": legacy OGF decoder: OGF_FILE, field "
            << field << ": expected " << expected << ", found " << found << '.';
    return message.str();
}

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

float readF32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::bit_cast<float>(readU32(bytes, offset));
}

ogfx::Position readPosition(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return {
        readF32(bytes, offset),
        readF32(bytes, offset + 4),
        readF32(bytes, offset + 8),
    };
}

class Decoder
{
public:
    Decoder(std::span<const std::uint8_t> bytes, std::string_view diagnosticName)
        : bytes_(bytes)
        , diagnosticName_(diagnosticName)
    {
    }

    ogfx::DecodeResult run()
    {
        SourceBounds sourceBounds{};
        if (!scanChunks()
            || !decodeHeader(&sourceBounds)
            || !decodeTexture()
            || !decodeGeometryHeaders()
            || !decodeVertices()
            || !decodeIndices()
            || !validateSourceBounds(sourceBounds)
            || !validateWinding()) {
            return failedResult();
        }

        model_.geometries.push_back({
            .firstVertex = 0,
            .vertexCount = static_cast<std::uint32_t>(model_.positions.size()),
            .firstIndex = 0,
            .indexCount = static_cast<std::uint32_t>(model_.indices.size()),
            .materialIndex = 0,
            .alphaTested = false,
        });
        model_.meshes.push_back({
            .firstGeometry = 0,
            .geometryCount = 1,
        });
        model_.materials.emplace_back();
        model_.materials[0].baseColorTexture = std::move(textureName_);

        return {
            .model = std::move(model_),
            .error = {},
        };
    }

private:
    bool reject(
        std::uint32_t chunkId,
        std::string_view field,
        std::string expected,
        std::string found)
    {
        error_ = makeChunkDiagnostic(
            diagnosticName_, chunkId, field, expected, found);
        return false;
    }

    bool rejectFile(
        std::string_view field,
        std::string expected,
        std::string found)
    {
        error_ = makeFileDiagnostic(diagnosticName_, field, expected, found);
        return false;
    }

    ogfx::DecodeResult failedResult()
    {
        return {
            .model = {},
            .error = std::move(error_),
        };
    }

    std::optional<ChunkView>& chunkSlot(std::uint32_t id)
    {
        assert(id >= HeaderChunkId && id <= IndicesChunkId);
        return chunks_[id - HeaderChunkId];
    }

    const ChunkView& chunk(std::uint32_t id) const
    {
        assert(id >= HeaderChunkId && id <= IndicesChunkId);
        return *chunks_[id - HeaderChunkId];
    }

    std::span<const std::uint8_t> payload(const ChunkView& view) const
    {
        return bytes_.subspan(view.payloadOffset, view.payloadSize);
    }

    bool scanChunks()
    {
        if (bytes_.size() > ogfx::MaximumFileBytes) {
            return rejectFile(
                "file byte size",
                "at most " + std::to_string(ogfx::MaximumFileBytes),
                std::to_string(bytes_.size()));
        }
        if (bytes_.size() < LegacyChunkHeaderSize) {
            return rejectFile(
                "file byte size",
                "at least " + std::to_string(LegacyChunkHeaderSize),
                std::to_string(bytes_.size()));
        }

        std::size_t offset = 0;
        while (offset < bytes_.size()) {
            const std::size_t remaining = bytes_.size() - offset;
            if (remaining < LegacyChunkHeaderSize) {
                return rejectFile(
                    "trailing bytes",
                    "a complete 8-byte chunk header",
                    std::to_string(remaining) + " bytes");
            }

            const std::uint32_t rawId = readU32(bytes_, offset);
            const std::uint32_t payloadSize = readU32(bytes_, offset + 4);
            const std::uint32_t id = rawId & ~CompressionMark;
            if ((rawId & CompressionMark) != 0) {
                return reject(
                    id,
                    "compression flag",
                    "clear for the M4a direct profile",
                    "set in " + hexadecimal(rawId));
            }
            if (id < HeaderChunkId || id > IndicesChunkId) {
                return reject(
                    id,
                    "chunk id",
                    "one of 0x1, 0x2, 0x3, or 0x4",
                    hexadecimal(id));
            }

            std::optional<ChunkView>& slot = chunkSlot(id);
            if (slot.has_value()) {
                return reject(id, "occurrence count", "exactly 1", "a duplicate");
            }

            const std::size_t payloadOffset = offset + LegacyChunkHeaderSize;
            if (payloadSize > bytes_.size() - payloadOffset) {
                return reject(
                    id,
                    "payload byte range",
                    "inside the file",
                    std::to_string(payloadSize) + " bytes with "
                        + std::to_string(bytes_.size() - payloadOffset)
                        + " available");
            }
            slot = ChunkView{
                .id = id,
                .payloadOffset = payloadOffset,
                .payloadSize = payloadSize,
            };
            offset = payloadOffset + payloadSize;
        }

        for (std::uint32_t id : RequiredChunkIds) {
            if (!chunkSlot(id).has_value()) {
                return reject(id, "presence", "exactly 1 required chunk", "missing");
            }
        }
        return true;
    }

    bool decodeHeader(SourceBounds* bounds)
    {
        const ChunkView& view = chunk(HeaderChunkId);
        if (view.payloadSize != HeaderPayloadSize) {
            return reject(
                view.id,
                "byteSize",
                std::to_string(HeaderPayloadSize),
                std::to_string(view.payloadSize));
        }

        const std::span<const std::uint8_t> bytes = payload(view);
        const std::uint8_t version = bytes[0];
        const std::uint8_t modelType = bytes[1];
        const std::uint16_t shaderId = readU16(bytes, 2);
        if (version != SupportedVersion) {
            return reject(
                view.id,
                "formatVersion",
                std::to_string(SupportedVersion),
                std::to_string(version));
        }
        if (modelType != SupportedModelType) {
            return reject(
                view.id,
                "modelType",
                "0 (normal/static)",
                std::to_string(modelType));
        }
        if (shaderId != SupportedShaderId) {
            return reject(
                view.id,
                "shaderId",
                "0 for the pinned default/opaque mapping",
                std::to_string(shaderId));
        }

        bounds->minimum = readPosition(bytes, 4);
        bounds->maximum = readPosition(bytes, 16);
        bounds->sphereCenter = readPosition(bytes, 28);
        bounds->sphereRadius = readF32(bytes, 40);
        if (!positionIsFinite(bounds->minimum)
            || !positionIsFinite(bounds->maximum)) {
            return reject(
                view.id,
                "AABB min/max",
                "finite f32 values",
                "a non-finite value");
        }
        if (bounds->minimum.x > bounds->maximum.x
            || bounds->minimum.y > bounds->maximum.y
            || bounds->minimum.z > bounds->maximum.z) {
            return reject(
                view.id,
                "AABB ordering",
                "minimum no greater than maximum",
                "an inverted component");
        }
        if (!positionIsFinite(bounds->sphereCenter)
            || !std::isfinite(bounds->sphereRadius)
            || bounds->sphereRadius < 0.0f) {
            return reject(
                view.id,
                "bounding sphere",
                "a finite center and nonnegative finite radius",
                "an invalid scalar");
        }
        return true;
    }

    bool readSourceString(
        std::span<const std::uint8_t> bytes,
        std::size_t* offset,
        std::string_view field,
        std::string* result)
    {
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(*offset);
        const auto terminator = std::find(begin, bytes.end(), 0);
        if (terminator == bytes.end()) {
            return reject(
                TextureChunkId,
                field,
                "a NUL-terminated string",
                "no terminator before the chunk end");
        }

        const std::size_t length =
            static_cast<std::size_t>(terminator - begin);
        if (length == 0) {
            return reject(TextureChunkId, field, "a nonempty string", "empty");
        }
        if (length > MaximumSourceStringBytes) {
            return reject(
                TextureChunkId,
                field,
                "at most " + std::to_string(MaximumSourceStringBytes)
                    + " source bytes",
                std::to_string(length) + " bytes");
        }

        const std::span<const std::uint8_t> textBytes = bytes.subspan(*offset, length);
        if (!std::all_of(textBytes.begin(), textBytes.end(), [](std::uint8_t byte) {
                return byte >= 0x20 && byte <= 0x7e;
            })) {
            return reject(
                TextureChunkId,
                field,
                "printable 7-bit ASCII in the narrow M4a profile",
                "a control or non-ASCII byte");
        }
        result->assign(
            reinterpret_cast<const char*>(textBytes.data()), textBytes.size());
        *offset += length + 1;
        return true;
    }

    bool decodeTexture()
    {
        const ChunkView& view = chunk(TextureChunkId);
        const std::span<const std::uint8_t> bytes = payload(view);
        std::size_t offset = 0;
        std::string shaderName;
        if (!readSourceString(bytes, &offset, "textureName", &textureName_)
            || !readSourceString(bytes, &offset, "engineShaderName", &shaderName)) {
            return false;
        }
        if (offset != bytes.size()) {
            return reject(
                view.id,
                "payload byte range",
                "exactly two NUL-terminated strings",
                std::to_string(bytes.size() - offset) + " trailing bytes");
        }
        if (shaderName != "default") {
            return reject(
                view.id,
                "engineShaderName",
                describeText("default") + " for the pinned opaque mapping",
                describeText(shaderName));
        }
        return true;
    }

    bool decodeGeometryHeaders()
    {
        const ChunkView& vertices = chunk(VerticesChunkId);
        if (vertices.payloadSize < VertexPayloadHeaderSize) {
            return reject(
                vertices.id,
                "byteSize",
                "at least " + std::to_string(VertexPayloadHeaderSize),
                std::to_string(vertices.payloadSize));
        }

        const std::span<const std::uint8_t> vertexBytes = payload(vertices);
        const std::uint32_t vertexFormat = readU32(vertexBytes, 0);
        vertexCount_ = readU32(vertexBytes, 4);
        if (vertexFormat != SupportedVertexFormat) {
            return reject(
                vertices.id,
                "vertexFormat",
                hexadecimal(SupportedVertexFormat) + " (XYZ|NORMAL|TEX1)",
                hexadecimal(vertexFormat));
        }
        if (vertexCount_ == 0) {
            return reject(vertices.id, "vertexCount", "at least 1", "0");
        }

        const std::uint64_t expectedVertexBytes = VertexPayloadHeaderSize
            + static_cast<std::uint64_t>(vertexCount_) * SourceVertexRecordSize;
        if (expectedVertexBytes != vertices.payloadSize) {
            return reject(
                vertices.id,
                "byteSize",
                std::to_string(expectedVertexBytes)
                    + " from vertexCount and stride 32",
                std::to_string(vertices.payloadSize));
        }

        const ChunkView& indices = chunk(IndicesChunkId);
        if (indices.payloadSize < IndexPayloadHeaderSize) {
            return reject(
                indices.id,
                "byteSize",
                "at least " + std::to_string(IndexPayloadHeaderSize),
                std::to_string(indices.payloadSize));
        }

        const std::span<const std::uint8_t> indexBytes = payload(indices);
        indexCount_ = readU32(indexBytes, 0);
        if (indexCount_ == 0 || (indexCount_ % 3) != 0) {
            return reject(
                indices.id,
                "indexCount",
                "a nonzero multiple of 3",
                std::to_string(indexCount_));
        }

        const std::uint64_t expectedIndexBytes = IndexPayloadHeaderSize
            + static_cast<std::uint64_t>(indexCount_) * sizeof(std::uint16_t);
        if (expectedIndexBytes != indices.payloadSize) {
            return reject(
                indices.id,
                "byteSize",
                std::to_string(expectedIndexBytes)
                    + " from indexCount and u16 stride",
                std::to_string(indices.payloadSize));
        }
        return validateCanonicalSize();
    }

    bool decodeVertices()
    {
        const ChunkView& view = chunk(VerticesChunkId);
        const std::span<const std::uint8_t> bytes = payload(view);
        model_.positions.reserve(vertexCount_);
        model_.attributes.reserve(vertexCount_);
        for (std::uint32_t index = 0; index < vertexCount_; ++index) {
            const std::size_t offset = VertexPayloadHeaderSize
                + static_cast<std::size_t>(index) * SourceVertexRecordSize;
            const ogfx::Position position = readPosition(bytes, offset);
            const ogfx::VertexAttributes attributes{
                readF32(bytes, offset + 12),
                readF32(bytes, offset + 16),
                readF32(bytes, offset + 20),
                readF32(bytes, offset + 24),
                readF32(bytes, offset + 28),
            };
            if (!positionIsFinite(position)) {
                return reject(
                    view.id,
                    ogfx::detail::indexedField("vertices", index, "position"),
                    "finite f32 values",
                    "a non-finite value");
            }
            if (!std::isfinite(attributes.nx)
                || !std::isfinite(attributes.ny)
                || !std::isfinite(attributes.nz)
                || !std::isfinite(attributes.u)
                || !std::isfinite(attributes.v)) {
                return reject(
                    view.id,
                    ogfx::detail::indexedField("vertices", index, "normal/uv"),
                    "finite f32 values",
                    "a non-finite value");
            }
            const double nx = attributes.nx;
            const double ny = attributes.ny;
            const double nz = attributes.nz;
            const double normalLengthSquared = nx * nx + ny * ny + nz * nz;
            if (!std::isfinite(normalLengthSquared)
                || normalLengthSquared < 1.0e-12) {
                return reject(
                    view.id,
                    ogfx::detail::indexedField(
                        "vertices", index, "normal length squared"),
                    "finite and at least 1e-12",
                    std::to_string(normalLengthSquared));
            }
            model_.positions.push_back(position);
            model_.attributes.push_back(attributes);
        }
        return true;
    }

    bool decodeIndices()
    {
        const ChunkView& view = chunk(IndicesChunkId);
        const std::span<const std::uint8_t> bytes = payload(view);
        model_.indices.reserve(indexCount_);
        for (std::uint32_t index = 0; index < indexCount_; ++index) {
            const std::uint32_t value = readU16(
                bytes,
                IndexPayloadHeaderSize
                    + static_cast<std::size_t>(index) * sizeof(std::uint16_t));
            if (value >= model_.positions.size()) {
                return reject(
                    view.id,
                    ogfx::detail::indexedField("indices", index, "value"),
                    "less than " + std::to_string(model_.positions.size()),
                    std::to_string(value));
            }
            model_.indices.push_back(value);
        }
        return true;
    }

    bool validateSourceBounds(const SourceBounds& bounds)
    {
        assert(!model_.positions.empty());
        ogfx::Position actualMinimum = model_.positions.front();
        ogfx::Position actualMaximum = model_.positions.front();
        for (const ogfx::Position& position : model_.positions) {
            actualMinimum.x = std::min(actualMinimum.x, position.x);
            actualMinimum.y = std::min(actualMinimum.y, position.y);
            actualMinimum.z = std::min(actualMinimum.z, position.z);
            actualMaximum.x = std::max(actualMaximum.x, position.x);
            actualMaximum.y = std::max(actualMaximum.y, position.y);
            actualMaximum.z = std::max(actualMaximum.z, position.z);
        }

        if (bounds.minimum.x != actualMinimum.x
            || bounds.minimum.y != actualMinimum.y
            || bounds.minimum.z != actualMinimum.z
            || bounds.maximum.x != actualMaximum.x
            || bounds.maximum.y != actualMaximum.y
            || bounds.maximum.z != actualMaximum.z) {
            return reject(
                HeaderChunkId,
                "AABB extrema",
                "the exact decoded vertex extrema",
                "different bounds");
        }

        float acceptedRadius = bounds.sphereRadius;
        if (acceptedRadius < std::numeric_limits<float>::max()) {
            acceptedRadius = std::nextafter(
                acceptedRadius, std::numeric_limits<float>::infinity());
        }
        const double radius = acceptedRadius;
        for (const ogfx::Position& position : model_.positions) {
            const double dx = static_cast<double>(position.x) - bounds.sphereCenter.x;
            const double dy = static_cast<double>(position.y) - bounds.sphereCenter.y;
            const double dz = static_cast<double>(position.z) - bounds.sphereCenter.z;
            const double distanceSquared = dx * dx + dy * dy + dz * dz;
            if (radius * radius < distanceSquared) {
                return reject(
                    HeaderChunkId,
                    "sphere enclosure",
                    "every vertex within the stored radius plus one f32 ULP",
                    "an out-of-bounds vertex");
            }
        }
        return true;
    }

    bool validateWinding()
    {
        for (std::size_t firstIndex = 0;
             firstIndex < model_.indices.size();
             firstIndex += 3) {
            const ogfx::Position& a = model_.positions[model_.indices[firstIndex]];
            const ogfx::Position& b = model_.positions[model_.indices[firstIndex + 1]];
            const ogfx::Position& c = model_.positions[model_.indices[firstIndex + 2]];
            const double edge1X = static_cast<double>(b.x) - a.x;
            const double edge1Y = static_cast<double>(b.y) - a.y;
            const double edge1Z = static_cast<double>(b.z) - a.z;
            const double edge2X = static_cast<double>(c.x) - a.x;
            const double edge2Y = static_cast<double>(c.y) - a.y;
            const double edge2Z = static_cast<double>(c.z) - a.z;
            const double crossX = edge1Y * edge2Z - edge1Z * edge2Y;
            const double crossY = edge1Z * edge2X - edge1X * edge2Z;
            const double crossZ = edge1X * edge2Y - edge1Y * edge2X;
            const double crossLengthSquared =
                crossX * crossX + crossY * crossY + crossZ * crossZ;
            if (crossLengthSquared == 0.0) {
                continue;
            }

            for (std::size_t corner = 0; corner < 3; ++corner) {
                const ogfx::VertexAttributes& normal =
                    model_.attributes[model_.indices[firstIndex + corner]];
                const double agreement = crossX * normal.nx
                    + crossY * normal.ny
                    + crossZ * normal.nz;
                if (!(agreement > 0.0)) {
                    return reject(
                        IndicesChunkId,
                        ogfx::detail::indexedField(
                            "triangles", firstIndex / 3, "winding"),
                        "the unchanged right-handed cross product to agree with all vertex normals",
                        "a nonpositive dot product");
                }
            }
        }
        return true;
    }

    bool validateCanonicalSize()
    {
        const std::uint64_t fileBytes = ogfx::detail::canonicalModelFileBytes({
            .geometryCount = 1,
            .meshCount = 1,
            .materialCount = 1,
            .materialStringBytes =
                static_cast<std::uint32_t>(2 + textureName_.size()),
            .positionCount = vertexCount_,
            .attributeCount = vertexCount_,
            .indexCount = indexCount_,
        });
        if (fileBytes > ogfx::MaximumFileBytes) {
            return rejectFile(
                "canonical OGFx byte size",
                "at most " + std::to_string(ogfx::MaximumFileBytes),
                std::to_string(fileBytes));
        }
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    std::string_view diagnosticName_;
    std::array<std::optional<ChunkView>, RequiredChunkIds.size()> chunks_;
    ogfx::Model model_;
    std::string textureName_;
    std::uint32_t vertexCount_ = 0;
    std::uint32_t indexCount_ = 0;
    std::string error_;
};
}

ogfx::DecodeResult decodeStaticModel(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName)
{
    try {
        return Decoder(bytes, diagnosticName).run();
    } catch (const std::bad_alloc&) {
        return {
            .model = {},
            .error = makeFileDiagnostic(
                diagnosticName,
                "resource allocation",
                "enough memory for the bounded source model",
                "allocation failure"),
        };
    } catch (const std::length_error&) {
        return {
            .model = {},
            .error = makeFileDiagnostic(
                diagnosticName,
                "resource allocation",
                "container sizes accepted by the host",
                "a length error"),
        };
    }
}
}
