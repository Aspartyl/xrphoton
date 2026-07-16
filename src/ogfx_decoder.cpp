#include "ogfx.hpp"

#include "ogfx_detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace xrphoton::ogfx
{
namespace
{
using detail::Bounds;
using detail::checkedAdd;
using detail::checkedAlignUp;
using detail::indexedField;
using detail::positionIsFinite;

constexpr std::array RequiredChunkIds{
    static_cast<std::uint32_t>(ChunkId::Model),
    static_cast<std::uint32_t>(ChunkId::Geometries),
    static_cast<std::uint32_t>(ChunkId::Meshes),
    static_cast<std::uint32_t>(ChunkId::Materials),
    static_cast<std::uint32_t>(ChunkId::Positions),
    static_cast<std::uint32_t>(ChunkId::Attributes),
    static_cast<std::uint32_t>(ChunkId::Indices),
};

struct ModelMetadata
{
    Bounds bounds{};
    Position sphereCenter{};
    float sphereRadius = 0.0f;
};

struct ChunkView
{
    std::uint32_t id = 0;
    std::size_t payloadOffset = 0;
    std::uint64_t payloadSize = 0;
};

bool isKnownRequiredChunk(std::uint32_t id)
{
    return std::find(RequiredChunkIds.begin(), RequiredChunkIds.end(), id)
        != RequiredChunkIds.end();
}

std::string hexadecimal(std::uint64_t value)
{
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t readU64(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint64_t>(readU32(bytes, offset))
        | (static_cast<std::uint64_t>(readU32(bytes, offset + 4)) << 32);
}

float readF32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::bit_cast<float>(readU32(bytes, offset));
}

Position readPosition(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return {
        readF32(bytes, offset),
        readF32(bytes, offset + 4),
        readF32(bytes, offset + 8),
    };
}

Bounds readBounds(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return {
        .minimum = readPosition(bytes, offset),
        .maximum = readPosition(bytes, offset + 12),
    };
}

bool validUtf8(std::span<const std::uint8_t> bytes)
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

class Decoder
{
public:
    Decoder(std::span<const std::uint8_t> bytes, std::string_view diagnosticName)
        : bytes_(bytes)
        , diagnosticName_(diagnosticName)
    {
    }

    DecodeResult run()
    {
        // Index every bounded chunk before decoding payloads. That makes file order
        // irrelevant and lets MODEL reject an unsupported type before any
        // type-specific array is interpreted.
        if (!scanFile()) {
            return failedResult();
        }

        ModelMetadata metadata{};
        std::vector<Bounds> geometryBounds;
        if (!decodeModelMetadata(&metadata)) {
            return failedResult();
        }
        if (!decodeGeometries(&geometryBounds)
            || !decodeMeshes()
            || !decodeMaterials()
            || !decodePositions()
            || !decodeAttributes()
            || !decodeIndices()
            || !validateModel(geometryBounds, metadata)
            || !validateRuntimeProfile()) {
            return failedResult();
        }

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
        error_ = detail::makeChunkDiagnostic(
            diagnosticName_,
            "decoder",
            chunkId,
            field,
            expected,
            found);
        return false;
    }

    bool rejectFile(
        std::string_view field,
        std::string expected,
        std::string found)
    {
        error_ = detail::makeFileDiagnostic(
            diagnosticName_,
            "decoder",
            field,
            expected,
            found);
        return false;
    }

    DecodeResult failedResult()
    {
        return {
            .model = {},
            .error = std::move(error_),
        };
    }

    const ChunkView* findChunk(ChunkId id) const
    {
        const std::uint32_t rawId = static_cast<std::uint32_t>(id);
        const auto found = std::find_if(
            chunks_.begin(),
            chunks_.end(),
            [rawId](const ChunkView& chunk) { return chunk.id == rawId; });
        return found == chunks_.end() ? nullptr : &*found;
    }

    bool scanFile()
    {
        if (bytes_.size() > MaximumFileBytes) {
            return rejectFile(
                "file byte size",
                "at most " + std::to_string(MaximumFileBytes),
                std::to_string(bytes_.size()));
        }
        if (bytes_.size() < FileHeaderSize) {
            return rejectFile(
                "file byte size",
                "at least " + std::to_string(FileHeaderSize),
                std::to_string(bytes_.size()));
        }
        if (!std::equal(FileMagic.begin(), FileMagic.end(), bytes_.begin())) {
            return rejectFile("magic", "OGFX", "different bytes");
        }

        const std::uint32_t containerVersion = readU32(bytes_, 4);
        if (containerVersion != ContainerVersion) {
            return rejectFile(
                "containerVersion",
                std::to_string(ContainerVersion),
                std::to_string(containerVersion));
        }
        const std::uint32_t headerSize = readU32(bytes_, 8);
        if (headerSize != FileHeaderSize) {
            return rejectFile(
                "headerSize",
                std::to_string(FileHeaderSize),
                std::to_string(headerSize));
        }
        const std::uint32_t headerReserved = readU32(bytes_, 12);
        if (headerReserved != 0) {
            return rejectFile(
                "reserved",
                "0",
                std::to_string(headerReserved));
        }

        std::unordered_set<std::uint32_t> seenChunkIds;
        std::uint64_t headerOffset = headerSize;
        while (headerOffset < bytes_.size()) {
            if (chunks_.size() >= MaximumChunkCount) {
                return rejectFile(
                    "chunk count",
                    "at most " + std::to_string(MaximumChunkCount),
                    "more than " + std::to_string(MaximumChunkCount));
            }
            // The version-1 file header is 16 bytes and every later header offset
            // comes from checkedAlignUp below, so misalignment is not input-reachable.
            assert((headerOffset % ChunkAlignment) == 0);

            const std::uint64_t payloadOffset = headerOffset + ChunkHeaderSize;
            if (payloadOffset > bytes_.size()) {
                return rejectFile(
                    "chunk header byte range",
                    "a complete " + std::to_string(ChunkHeaderSize) + "-byte header",
                    std::to_string(bytes_.size() - static_cast<std::size_t>(headerOffset))
                        + " remaining bytes");
            }

            const std::size_t header = static_cast<std::size_t>(headerOffset);
            const std::uint32_t id = readU32(bytes_, header);
            const std::uint32_t flags = readU32(bytes_, header + 4);
            const std::uint32_t version = readU32(bytes_, header + 8);
            const std::uint32_t reserved0 = readU32(bytes_, header + 12);
            const std::uint64_t payloadSize = readU64(bytes_, header + 16);
            const std::uint64_t reserved1 = readU64(bytes_, header + 24);

            if ((flags & ~RequiredChunkFlags) != 0) {
                return reject(id, "flags", "only required bit 0", hexadecimal(flags));
            }
            if (reserved0 != 0) {
                return reject(id, "reserved0", "0", std::to_string(reserved0));
            }
            if (reserved1 != 0) {
                return reject(id, "reserved1", "0", std::to_string(reserved1));
            }

            std::uint64_t payloadEnd = 0;
            if (!checkedAdd(payloadOffset, payloadSize, &payloadEnd)) {
                return reject(id, "payload end", "checked u64 arithmetic", "overflow");
            }
            if (payloadEnd > bytes_.size()) {
                return reject(
                    id,
                    "payload byte range",
                    "end at or before " + std::to_string(bytes_.size()),
                    std::to_string(payloadEnd));
            }

            if (!seenChunkIds.insert(id).second) {
                return reject(id, "occurrence count", "exactly 1", "a duplicate");
            }
            if (isKnownRequiredChunk(id)) {
                if (flags != RequiredChunkFlags) {
                    return reject(
                        id,
                        "flags",
                        std::to_string(RequiredChunkFlags),
                        std::to_string(flags));
                }
                if (version != ChunkVersion) {
                    return reject(
                        id,
                        "version",
                        std::to_string(ChunkVersion),
                        std::to_string(version));
                }
            } else if ((flags & RequiredChunkFlags) != 0) {
                return reject(
                    id,
                    "chunk id",
                    "a supported required chunk or an optional chunk",
                    hexadecimal(id) + " marked required");
            }

            chunks_.push_back({
                .id = id,
                .payloadOffset = static_cast<std::size_t>(payloadOffset),
                .payloadSize = payloadSize,
            });

            if (payloadEnd == bytes_.size()) {
                headerOffset = payloadEnd;
                break;
            }

            std::uint64_t nextHeader = 0;
            const bool alignmentSucceeded =
                checkedAlignUp(payloadEnd, ChunkAlignment, &nextHeader);
            assert(alignmentSucceeded);
            static_cast<void>(alignmentSucceeded);
            if (nextHeader > bytes_.size()) {
                return reject(
                    id,
                    "trailing bytes",
                    "no bytes after the final payload",
                    std::to_string(bytes_.size() - static_cast<std::size_t>(payloadEnd)));
            }
            for (std::uint64_t offset = payloadEnd; offset < nextHeader; ++offset) {
                if (bytes_[static_cast<std::size_t>(offset)] != 0) {
                    return reject(
                        id,
                        "alignment padding",
                        "all zero bytes",
                        "a nonzero byte at file offset " + std::to_string(offset));
                }
            }

            const std::uint64_t nextPayload = nextHeader + ChunkHeaderSize;
            if (nextPayload > bytes_.size()) {
                return rejectFile(
                    "chunk header byte range",
                    "a complete " + std::to_string(ChunkHeaderSize) + "-byte header",
                    std::to_string(bytes_.size() - static_cast<std::size_t>(nextHeader))
                        + " remaining bytes");
            }
            headerOffset = nextHeader;
        }

        for (std::uint32_t id : RequiredChunkIds) {
            if (!seenChunkIds.contains(id)) {
                return reject(id, "occurrence count", "exactly 1", "0");
            }
        }

        return true;
    }

    bool validateBounds(std::uint32_t chunkId, std::string_view field, const Bounds& bounds)
    {
        if (!positionIsFinite(bounds.minimum) || !positionIsFinite(bounds.maximum)) {
            return reject(
                chunkId,
                std::string(field) + ".minimum/maximum",
                "finite f32 values",
                "a non-finite value");
        }
        if (bounds.minimum.x > bounds.maximum.x
            || bounds.minimum.y > bounds.maximum.y
            || bounds.minimum.z > bounds.maximum.z) {
            return reject(
                chunkId,
                field,
                "minimum no greater than maximum on every axis",
                "a reversed axis");
        }
        return true;
    }

    bool decodeModelMetadata(ModelMetadata* metadata)
    {
        const ChunkView& chunk = *findChunk(ChunkId::Model);
        if (chunk.payloadSize != ModelRecordSize) {
            return reject(
                chunk.id,
                "byteSize",
                std::to_string(ModelRecordSize),
                std::to_string(chunk.payloadSize));
        }

        const std::size_t offset = chunk.payloadOffset;
        const std::uint32_t modelType = readU32(bytes_, offset);
        if (modelType != NormalModelType) {
            return reject(
                chunk.id,
                "modelType",
                std::to_string(NormalModelType) + " (normal/static)",
                std::to_string(modelType));
        }
        const std::uint32_t modelFlags = readU32(bytes_, offset + 4);
        if (modelFlags != 0) {
            return reject(chunk.id, "modelFlags", "0", hexadecimal(modelFlags));
        }

        metadata->bounds = readBounds(bytes_, offset + 8);
        metadata->sphereCenter = readPosition(bytes_, offset + 32);
        metadata->sphereRadius = readF32(bytes_, offset + 44);
        if (!validateBounds(chunk.id, "aabb", metadata->bounds)) {
            return false;
        }
        if (!positionIsFinite(metadata->sphereCenter)) {
            return reject(
                chunk.id,
                "sphereCenter",
                "finite f32 values",
                "a non-finite value");
        }
        if (!std::isfinite(metadata->sphereRadius) || metadata->sphereRadius < 0.0f) {
            return reject(
                chunk.id,
                "sphereRadius",
                "a finite nonnegative f32",
                std::to_string(metadata->sphereRadius));
        }
        return true;
    }

    bool arrayCount(
        const ChunkView& chunk,
        std::uint32_t stride,
        std::string_view countField,
        std::uint32_t* count)
    {
        if (chunk.payloadSize == 0) {
            return reject(chunk.id, countField, "at least 1", "0");
        }
        if ((chunk.payloadSize % stride) != 0) {
            return reject(
                chunk.id,
                "byteSize",
                "a multiple of " + std::to_string(stride),
                std::to_string(chunk.payloadSize));
        }
        const std::uint64_t wideCount = chunk.payloadSize / stride;
        // The 1 GiB file cap bounds every array count well below UINT32_MAX.
        assert(wideCount <= std::numeric_limits<std::uint32_t>::max());
        *count = static_cast<std::uint32_t>(wideCount);
        return true;
    }

    bool decodeGeometries(std::vector<Bounds>* geometryBounds)
    {
        const ChunkView& chunk = *findChunk(ChunkId::Geometries);
        std::uint32_t count = 0;
        if (!arrayCount(chunk, GeometryRecordSize, "record count", &count)) {
            return false;
        }

        model_.geometries.reserve(count);
        geometryBounds->reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::size_t offset = chunk.payloadOffset
                + static_cast<std::size_t>(index) * GeometryRecordSize;
            const std::uint32_t geometryFlags = readU32(bytes_, offset + 20);
            if ((geometryFlags & ~GeometryFlagAlphaTested) != 0) {
                return reject(
                    chunk.id,
                    indexedField("geometries", index, "geometryFlags"),
                    "only alpha-tested bit 0",
                    hexadecimal(geometryFlags));
            }

            const Bounds bounds = readBounds(bytes_, offset + 24);
            if (!validateBounds(
                    chunk.id,
                    indexedField("geometries", index, "aabb"),
                    bounds)) {
                return false;
            }
            model_.geometries.push_back({
                .firstVertex = readU32(bytes_, offset),
                .vertexCount = readU32(bytes_, offset + 4),
                .firstIndex = readU32(bytes_, offset + 8),
                .indexCount = readU32(bytes_, offset + 12),
                .materialIndex = readU32(bytes_, offset + 16),
                .alphaTested = (geometryFlags & GeometryFlagAlphaTested) != 0,
            });
            geometryBounds->push_back(bounds);
        }
        return true;
    }

    bool decodeMeshes()
    {
        const ChunkView& chunk = *findChunk(ChunkId::Meshes);
        std::uint32_t count = 0;
        if (!arrayCount(chunk, MeshRecordSize, "record count", &count)) {
            return false;
        }

        model_.meshes.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::size_t offset = chunk.payloadOffset
                + static_cast<std::size_t>(index) * MeshRecordSize;
            model_.meshes.push_back({
                .firstGeometry = readU32(bytes_, offset),
                .geometryCount = readU32(bytes_, offset + 4),
            });
        }
        return true;
    }

    bool decodeStringArena(
        const ChunkView& chunk,
        std::size_t arenaOffset,
        std::uint32_t arenaSize,
        const std::vector<std::uint32_t>& requestedOffsets,
        std::vector<std::uint8_t>* matchedOffsets)
    {
        std::uint32_t offset = 0;
        std::size_t requestedIndex = 0;
        while (offset < arenaSize) {
            const std::uint32_t remaining = arenaSize - offset;
            if (remaining < 2) {
                return reject(
                    chunk.id,
                    "string arena",
                    "a complete u16 length prefix",
                    std::to_string(remaining) + " trailing byte");
            }

            const std::uint16_t length = readU16(bytes_, arenaOffset + offset);
            if (length == 0) {
                return reject(chunk.id, "string length", "1..4096", "0");
            }
            if (length > MaximumStringBytes) {
                return reject(
                    chunk.id,
                    "string length",
                    "at most " + std::to_string(MaximumStringBytes),
                    std::to_string(length));
            }
            if (static_cast<std::uint32_t>(length) > remaining - 2) {
                return reject(
                    chunk.id,
                    "string byte range",
                    "inside the string arena",
                    std::to_string(length) + " bytes with "
                        + std::to_string(remaining - 2) + " available");
            }

            const std::size_t textOffset = arenaOffset + offset + 2;
            const std::span<const std::uint8_t> textBytes =
                bytes_.subspan(textOffset, length);
            if (!validUtf8(textBytes)) {
                return reject(chunk.id, "string UTF-8", "valid UTF-8", "an invalid sequence");
            }

            while (requestedIndex < requestedOffsets.size()
                   && requestedOffsets[requestedIndex] < offset) {
                ++requestedIndex;
            }
            if (requestedIndex < requestedOffsets.size()
                && requestedOffsets[requestedIndex] == offset) {
                (*matchedOffsets)[requestedIndex] = 1;
            }
            offset += 2 + length;
        }
        return true;
    }

    bool decodeMaterials()
    {
        const ChunkView& chunk = *findChunk(ChunkId::Materials);
        if (chunk.payloadSize < MaterialHeaderSize) {
            return reject(
                chunk.id,
                "byteSize",
                "at least " + std::to_string(MaterialHeaderSize),
                std::to_string(chunk.payloadSize));
        }

        const std::size_t offset = chunk.payloadOffset;
        const std::uint32_t materialCount = readU32(bytes_, offset);
        materialStringByteSize_ = readU32(bytes_, offset + 4);
        const std::uint32_t reserved0 = readU32(bytes_, offset + 8);
        const std::uint32_t reserved1 = readU32(bytes_, offset + 12);
        if (materialCount == 0) {
            return reject(chunk.id, "materialCount", "at least 1", "0");
        }
        if (reserved0 != 0) {
            return reject(chunk.id, "payload reserved0", "0", std::to_string(reserved0));
        }
        if (reserved1 != 0) {
            return reject(chunk.id, "payload reserved1", "0", std::to_string(reserved1));
        }

        // These on-disk counts are u32 and are widened before arithmetic, so the
        // complete material payload formula cannot overflow u64.
        const std::uint64_t recordBytes =
            static_cast<std::uint64_t>(materialCount) * MaterialRecordSize;
        const std::uint64_t expectedBytes = MaterialHeaderSize
            + recordBytes
            + materialStringByteSize_;
        if (expectedBytes != chunk.payloadSize) {
            return reject(
                chunk.id,
                "byteSize",
                std::to_string(expectedBytes) + " from its framed fields",
                std::to_string(chunk.payloadSize));
        }

        model_.materials.reserve(materialCount);
        textureReferenceOffsets_.reserve(materialCount);
        for (std::uint32_t index = 0; index < materialCount; ++index) {
            const std::size_t recordOffset = offset + MaterialHeaderSize
                + static_cast<std::size_t>(index) * MaterialRecordSize;
            Material material{};
            for (std::size_t component = 0;
                 component < material.baseColorFactor.size();
                 ++component) {
                material.baseColorFactor[component] =
                    readF32(bytes_, recordOffset + component * 4);
                if (!std::isfinite(material.baseColorFactor[component])) {
                    return reject(
                        chunk.id,
                        indexedField("materials", index, "baseColorFactor")
                            + '[' + std::to_string(component) + ']',
                        "a finite f32",
                        "a non-finite value");
                }
            }
            material.alphaCutoff = readF32(bytes_, recordOffset + 16);
            if (!std::isfinite(material.alphaCutoff)) {
                return reject(
                    chunk.id,
                    indexedField("materials", index, "alphaCutoff"),
                    "a finite f32",
                    "a non-finite value");
            }

            const std::uint32_t textureOffset = readU32(bytes_, recordOffset + 20);
            const std::uint32_t recordReserved0 = readU32(bytes_, recordOffset + 24);
            const std::uint32_t recordReserved1 = readU32(bytes_, recordOffset + 28);
            if (recordReserved0 != 0) {
                return reject(
                    chunk.id,
                    indexedField("materials", index, "reserved0"),
                    "0",
                    std::to_string(recordReserved0));
            }
            if (recordReserved1 != 0) {
                return reject(
                    chunk.id,
                    indexedField("materials", index, "reserved1"),
                    "0",
                    std::to_string(recordReserved1));
            }

            textureReferenceOffsets_.push_back(textureOffset);
            model_.materials.push_back(std::move(material));
        }

        // M4 ultimately rejects texture references, so retaining every arena string
        // would only amplify hostile input. Sort the referenced offsets, validate all
        // UTF-8 once, and mark matching entry starts in one bounded pass instead.
        std::vector<std::uint32_t> requestedOffsets;
        const std::size_t referenceCount = static_cast<std::size_t>(std::count_if(
            textureReferenceOffsets_.begin(),
            textureReferenceOffsets_.end(),
            [](std::uint32_t textureOffset) {
                return textureOffset != NoTextureReference;
            }));
        requestedOffsets.reserve(referenceCount);
        for (std::uint32_t textureOffset : textureReferenceOffsets_) {
            if (textureOffset != NoTextureReference) {
                requestedOffsets.push_back(textureOffset);
            }
        }
        std::sort(requestedOffsets.begin(), requestedOffsets.end());
        requestedOffsets.erase(
            std::unique(requestedOffsets.begin(), requestedOffsets.end()),
            requestedOffsets.end());
        std::vector<std::uint8_t> matchedOffsets(requestedOffsets.size(), 0);

        const std::size_t arenaOffset = offset + MaterialHeaderSize
            + static_cast<std::size_t>(recordBytes);
        if (!decodeStringArena(
                chunk,
                arenaOffset,
                materialStringByteSize_,
                requestedOffsets,
                &matchedOffsets)) {
            return false;
        }

        const auto unmatched = std::find(matchedOffsets.begin(), matchedOffsets.end(), 0);
        if (unmatched != matchedOffsets.end()) {
            const std::size_t unmatchedIndex =
                static_cast<std::size_t>(unmatched - matchedOffsets.begin());
            const std::uint32_t unmatchedOffset = requestedOffsets[unmatchedIndex];
            const auto material = std::find(
                textureReferenceOffsets_.begin(),
                textureReferenceOffsets_.end(),
                unmatchedOffset);
            const std::size_t materialIndex =
                static_cast<std::size_t>(material - textureReferenceOffsets_.begin());
            return reject(
                chunk.id,
                indexedField("materials", materialIndex, "textureRefOffset"),
                "UINT32_MAX or the start of a string entry",
                std::to_string(unmatchedOffset));
        }
        return true;
    }

    bool decodePositions()
    {
        const ChunkView& chunk = *findChunk(ChunkId::Positions);
        std::uint32_t count = 0;
        if (!arrayCount(chunk, PositionRecordSize, "element count", &count)) {
            return false;
        }

        model_.positions.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const Position position = readPosition(
                bytes_,
                chunk.payloadOffset + static_cast<std::size_t>(index) * PositionRecordSize);
            if (!positionIsFinite(position)) {
                return reject(
                    chunk.id,
                    indexedField("positions", index, "x/y/z"),
                    "finite f32 values",
                    "a non-finite value");
            }
            model_.positions.push_back(position);
        }
        return true;
    }

    bool decodeAttributes()
    {
        const ChunkView& chunk = *findChunk(ChunkId::Attributes);
        std::uint32_t count = 0;
        if (!arrayCount(chunk, AttributeRecordSize, "element count", &count)) {
            return false;
        }

        model_.attributes.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::size_t offset = chunk.payloadOffset
                + static_cast<std::size_t>(index) * AttributeRecordSize;
            VertexAttributes attributes{
                readF32(bytes_, offset),
                readF32(bytes_, offset + 4),
                readF32(bytes_, offset + 8),
                readF32(bytes_, offset + 12),
                readF32(bytes_, offset + 16),
            };
            if (!std::isfinite(attributes.nx)
                || !std::isfinite(attributes.ny)
                || !std::isfinite(attributes.nz)
                || !std::isfinite(attributes.u)
                || !std::isfinite(attributes.v)) {
                return reject(
                    chunk.id,
                    indexedField("attributes", index, "nx/ny/nz/u/v"),
                    "finite f32 values",
                    "a non-finite value");
            }

            const double nx = attributes.nx;
            const double ny = attributes.ny;
            const double nz = attributes.nz;
            const double lengthSquared = nx * nx + ny * ny + nz * nz;
            if (!std::isfinite(lengthSquared) || lengthSquared < 1.0e-12) {
                return reject(
                    chunk.id,
                    indexedField("attributes", index, "normal length squared"),
                    "finite and at least 1e-12",
                    std::to_string(lengthSquared));
            }
            model_.attributes.push_back(attributes);
        }
        return true;
    }

    bool decodeIndices()
    {
        const ChunkView& chunk = *findChunk(ChunkId::Indices);
        std::uint32_t count = 0;
        if (!arrayCount(chunk, IndexRecordSize, "element count", &count)) {
            return false;
        }

        model_.indices.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            model_.indices.push_back(readU32(
                bytes_,
                chunk.payloadOffset + static_cast<std::size_t>(index) * IndexRecordSize));
        }
        return true;
    }

    bool boundsEnclosePosition(const Bounds& bounds, const Position& position) const
    {
        return bounds.minimum.x <= position.x && position.x <= bounds.maximum.x
            && bounds.minimum.y <= position.y && position.y <= bounds.maximum.y
            && bounds.minimum.z <= position.z && position.z <= bounds.maximum.z;
    }

    bool validateModel(const std::vector<Bounds>& geometryBounds, const ModelMetadata& metadata)
    {
        if (model_.positions.size() != model_.attributes.size()) {
            return reject(
                static_cast<std::uint32_t>(ChunkId::Attributes),
                "element count",
                std::to_string(model_.positions.size()) + " (position count)",
                std::to_string(model_.attributes.size()));
        }

        std::uint64_t expectedFirstVertex = 0;
        std::uint64_t expectedFirstIndex = 0;
        for (std::size_t geometryIndex = 0;
             geometryIndex < model_.geometries.size();
             ++geometryIndex) {
            const Geometry& geometry = model_.geometries[geometryIndex];
            if (geometry.vertexCount == 0) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Geometries),
                    indexedField("geometries", geometryIndex, "vertexCount"),
                    "at least 1",
                    "0");
            }
            if (geometry.indexCount == 0 || (geometry.indexCount % 3) != 0) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Geometries),
                    indexedField("geometries", geometryIndex, "indexCount"),
                    "a nonzero multiple of 3",
                    std::to_string(geometry.indexCount));
            }
            if (geometry.firstVertex != expectedFirstVertex) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Geometries),
                    indexedField("geometries", geometryIndex, "firstVertex"),
                    std::to_string(expectedFirstVertex) + " (next partition offset)",
                    std::to_string(geometry.firstVertex));
            }
            if (geometry.firstIndex != expectedFirstIndex) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Geometries),
                    indexedField("geometries", geometryIndex, "firstIndex"),
                    std::to_string(expectedFirstIndex) + " (next partition offset)",
                    std::to_string(geometry.firstIndex));
            }

            // Each pair is u32 and is widened before addition, so u64 overflow is
            // impossible; only the resulting range relative to the arrays is hostile.
            const std::uint64_t vertexEnd =
                static_cast<std::uint64_t>(geometry.firstVertex) + geometry.vertexCount;
            const std::uint64_t indexEnd =
                static_cast<std::uint64_t>(geometry.firstIndex) + geometry.indexCount;
            if (vertexEnd > model_.positions.size()) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Geometries),
                    indexedField("geometries", geometryIndex, "vertex range end"),
                    "at most " + std::to_string(model_.positions.size()),
                    std::to_string(vertexEnd));
            }
            if (indexEnd > model_.indices.size()) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Geometries),
                    indexedField("geometries", geometryIndex, "index range end"),
                    "at most " + std::to_string(model_.indices.size()),
                    std::to_string(indexEnd));
            }
            if (geometry.materialIndex >= model_.materials.size()) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Geometries),
                    indexedField("geometries", geometryIndex, "materialIndex"),
                    "less than " + std::to_string(model_.materials.size()),
                    std::to_string(geometry.materialIndex));
            }

            for (std::uint64_t index = geometry.firstIndex; index < indexEnd; ++index) {
                const std::uint32_t localIndex = model_.indices[static_cast<std::size_t>(index)];
                if (localIndex >= geometry.vertexCount) {
                    return reject(
                        static_cast<std::uint32_t>(ChunkId::Indices),
                        indexedField(
                            "indices",
                            static_cast<std::size_t>(index),
                            "geometry-local value"),
                        "less than " + std::to_string(geometry.vertexCount),
                        std::to_string(localIndex));
                }
            }
            for (std::uint64_t index = geometry.firstVertex; index < vertexEnd; ++index) {
                if (!boundsEnclosePosition(
                        geometryBounds[geometryIndex],
                        model_.positions[static_cast<std::size_t>(index)])) {
                    return reject(
                        static_cast<std::uint32_t>(ChunkId::Geometries),
                        indexedField("geometries", geometryIndex, "aabb enclosure"),
                        "every position in its vertex range",
                        "an out-of-bounds position");
                }
            }

            expectedFirstVertex = vertexEnd;
            expectedFirstIndex = indexEnd;
        }

        if (expectedFirstVertex != model_.positions.size()) {
            return reject(
                static_cast<std::uint32_t>(ChunkId::Geometries),
                "final vertex partition end",
                std::to_string(model_.positions.size()),
                std::to_string(expectedFirstVertex));
        }
        if (expectedFirstIndex != model_.indices.size()) {
            return reject(
                static_cast<std::uint32_t>(ChunkId::Geometries),
                "final index partition end",
                std::to_string(model_.indices.size()),
                std::to_string(expectedFirstIndex));
        }

        std::uint64_t expectedFirstGeometry = 0;
        for (std::size_t meshIndex = 0; meshIndex < model_.meshes.size(); ++meshIndex) {
            const Mesh& mesh = model_.meshes[meshIndex];
            if (mesh.geometryCount == 0) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Meshes),
                    indexedField("meshes", meshIndex, "geometryCount"),
                    "at least 1",
                    "0");
            }
            if (mesh.firstGeometry != expectedFirstGeometry) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Meshes),
                    indexedField("meshes", meshIndex, "firstGeometry"),
                    std::to_string(expectedFirstGeometry) + " (next partition offset)",
                    std::to_string(mesh.firstGeometry));
            }

            const std::uint64_t geometryEnd =
                static_cast<std::uint64_t>(mesh.firstGeometry) + mesh.geometryCount;
            if (geometryEnd > model_.geometries.size()) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Meshes),
                    indexedField("meshes", meshIndex, "geometry range end"),
                    "at most " + std::to_string(model_.geometries.size()),
                    std::to_string(geometryEnd));
            }
            expectedFirstGeometry = geometryEnd;
        }

        if (expectedFirstGeometry != model_.geometries.size()) {
            return reject(
                static_cast<std::uint32_t>(ChunkId::Meshes),
                "final geometry partition end",
                std::to_string(model_.geometries.size()),
                std::to_string(expectedFirstGeometry));
        }

        for (const Position& position : model_.positions) {
            if (!boundsEnclosePosition(metadata.bounds, position)) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Model),
                    "aabb enclosure",
                    "every model position",
                    "an out-of-bounds position");
            }

            const double dx = static_cast<double>(position.x) - metadata.sphereCenter.x;
            const double dy = static_cast<double>(position.y) - metadata.sphereCenter.y;
            const double dz = static_cast<double>(position.z) - metadata.sphereCenter.z;
            const double distanceSquared = dx * dx + dy * dy + dz * dz;
            const double radius = metadata.sphereRadius;
            if (radius * radius < distanceSquared) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Model),
                    "sphere enclosure",
                    "every model position",
                    "an out-of-bounds position");
            }
        }

        return true;
    }

    bool validateRuntimeProfile()
    {
        if (model_.meshes.size() != 1) {
            return reject(
                static_cast<std::uint32_t>(ChunkId::Meshes),
                "M4 runtime record count",
                "exactly 1",
                std::to_string(model_.meshes.size()));
        }
        if (model_.geometries.size() != 1) {
            return reject(
                static_cast<std::uint32_t>(ChunkId::Geometries),
                "M4 runtime record count",
                "exactly 1",
                std::to_string(model_.geometries.size()));
        }
        if (model_.materials.size() != 1) {
            return reject(
                static_cast<std::uint32_t>(ChunkId::Materials),
                "M4 runtime record count",
                "exactly 1",
                std::to_string(model_.materials.size()));
        }
        if (model_.geometries[0].alphaTested) {
            return reject(
                static_cast<std::uint32_t>(ChunkId::Geometries),
                "geometries[0].geometryFlags",
                "opaque geometry with bit 0 clear in M4",
                "alpha-tested bit 0 set");
        }
        for (std::size_t index = 0; index < textureReferenceOffsets_.size(); ++index) {
            if (textureReferenceOffsets_[index] != NoTextureReference) {
                return reject(
                    static_cast<std::uint32_t>(ChunkId::Materials),
                    indexedField("materials", index, "textureRefOffset"),
                    "UINT32_MAX in the M4 runtime",
                    std::to_string(textureReferenceOffsets_[index]));
            }
        }
        if (materialStringByteSize_ != 0) {
            return reject(
                static_cast<std::uint32_t>(ChunkId::Materials),
                "stringByteSize",
                "0 in the M4 runtime",
                std::to_string(materialStringByteSize_));
        }
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    std::string_view diagnosticName_;
    std::vector<ChunkView> chunks_;
    Model model_;
    std::vector<std::uint32_t> textureReferenceOffsets_;
    std::uint32_t materialStringByteSize_ = 0;
    std::string error_;
};
}

DecodeResult decodeModel(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName)
{
    try {
        return Decoder(bytes, diagnosticName).run();
    } catch (const std::bad_alloc&) {
        return {
            .model = {},
            .error = detail::makeFileDiagnostic(
                diagnosticName,
                "decoder",
                "resource allocation",
                "enough memory for the bounded model",
                "allocation failure"),
        };
    } catch (const std::length_error&) {
        return {
            .model = {},
            .error = detail::makeFileDiagnostic(
                diagnosticName,
                "decoder",
                "resource allocation",
                "container sizes accepted by the host",
                "a length error"),
        };
    }
}
}
