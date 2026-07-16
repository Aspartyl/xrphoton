#include "ogfx.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using xrphoton::ogfx::ChunkId;
using xrphoton::ogfx::DecodeResult;
using xrphoton::ogfx::Geometry;
using xrphoton::ogfx::Mesh;
using xrphoton::ogfx::Model;
using xrphoton::ogfx::SerializeResult;
using xrphoton::ogfx::VertexAttributes;

int failureCount = 0;

void expect(bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failureCount;
    }
}

void appendU16(std::vector<std::uint8_t>* bytes, std::uint16_t value)
{
    bytes->push_back(static_cast<std::uint8_t>(value));
    bytes->push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(std::vector<std::uint8_t>* bytes, std::uint32_t value)
{
    bytes->push_back(static_cast<std::uint8_t>(value));
    bytes->push_back(static_cast<std::uint8_t>(value >> 8));
    bytes->push_back(static_cast<std::uint8_t>(value >> 16));
    bytes->push_back(static_cast<std::uint8_t>(value >> 24));
}

void appendU64(std::vector<std::uint8_t>* bytes, std::uint64_t value)
{
    appendU32(bytes, static_cast<std::uint32_t>(value));
    appendU32(bytes, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::uint64_t readU64(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint64_t>(readU32(bytes, offset))
        | (static_cast<std::uint64_t>(readU32(bytes, offset + 4)) << 32);
}

void writeU32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value)
{
    (*bytes)[offset] = static_cast<std::uint8_t>(value);
    (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    (*bytes)[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    (*bytes)[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void writeU64(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint64_t value)
{
    writeU32(bytes, offset, static_cast<std::uint32_t>(value));
    writeU32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32));
}

void writeF32(std::vector<std::uint8_t>* bytes, std::size_t offset, float value)
{
    writeU32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

Model makeQuad()
{
    Model model{};
    model.positions = {
        {-0.5f, -0.5f, 0.0f},
        { 0.5f, -0.5f, 0.0f},
        { 0.5f,  0.5f, 0.0f},
        {-0.5f,  0.5f, 0.0f},
    };
    model.attributes = {
        {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
    };
    model.indices = {0, 1, 2, 0, 2, 3};
    model.geometries.push_back(Geometry{0, 4, 0, 6, 0, false});
    model.meshes.push_back(Mesh{0, 1});
    model.materials.emplace_back();
    return model;
}

Model makeTwoGeometryModel()
{
    Model model{};
    model.positions = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {2.0f, 0.0f, 0.0f},
        {3.0f, 0.0f, 0.0f},
        {2.0f, 1.0f, 0.0f},
    };
    model.attributes.assign(6, VertexAttributes{0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
    model.indices = {0, 1, 2, 0, 1, 2};
    model.geometries = {
        Geometry{0, 3, 0, 3, 0, false},
        Geometry{3, 3, 3, 3, 0, false},
    };
    model.meshes = {Mesh{0, 1}, Mesh{1, 1}};
    model.materials.emplace_back();
    return model;
}

std::vector<std::uint8_t> serialize(const Model& model)
{
    const SerializeResult result = xrphoton::ogfx::serializeModel(model, "test-source");
    expect(static_cast<bool>(result), "test source model serializes");
    if (!result) {
        std::cerr << result.error << '\n';
    }
    return result.bytes;
}

struct RawChunk
{
    std::uint32_t id = 0;
    std::uint32_t flags = 0;
    std::uint32_t version = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
    std::vector<std::uint8_t> payload;
};

std::vector<RawChunk> splitChunks(const std::vector<std::uint8_t>& bytes)
{
    std::vector<RawChunk> chunks;
    std::size_t offset = xrphoton::ogfx::FileHeaderSize;
    while (offset < bytes.size()) {
        RawChunk chunk{};
        chunk.id = readU32(bytes, offset);
        chunk.flags = readU32(bytes, offset + 4);
        chunk.version = readU32(bytes, offset + 8);
        chunk.reserved0 = readU32(bytes, offset + 12);
        const std::uint64_t payloadSize = readU64(bytes, offset + 16);
        chunk.reserved1 = readU64(bytes, offset + 24);
        const std::size_t payloadOffset = offset + xrphoton::ogfx::ChunkHeaderSize;
        chunk.payload.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
            bytes.begin() + static_cast<std::ptrdiff_t>(payloadOffset + payloadSize));
        chunks.push_back(std::move(chunk));
        offset = payloadOffset + static_cast<std::size_t>(payloadSize);
        if (offset != bytes.size()) {
            const std::size_t remainder = offset % xrphoton::ogfx::ChunkAlignment;
            if (remainder != 0) {
                offset += xrphoton::ogfx::ChunkAlignment - remainder;
            }
        }
    }
    return chunks;
}

void alignOutput(std::vector<std::uint8_t>* bytes)
{
    const std::size_t remainder = bytes->size() % xrphoton::ogfx::ChunkAlignment;
    if (remainder != 0) {
        bytes->insert(
            bytes->end(),
            xrphoton::ogfx::ChunkAlignment - remainder,
            0);
    }
}

std::vector<std::uint8_t> assembleFile(const std::vector<RawChunk>& chunks)
{
    std::vector<std::uint8_t> bytes;
    bytes.insert(
        bytes.end(),
        xrphoton::ogfx::FileMagic.begin(),
        xrphoton::ogfx::FileMagic.end());
    appendU32(&bytes, xrphoton::ogfx::ContainerVersion);
    appendU32(&bytes, xrphoton::ogfx::FileHeaderSize);
    appendU32(&bytes, 0);

    for (const RawChunk& chunk : chunks) {
        alignOutput(&bytes);
        appendU32(&bytes, chunk.id);
        appendU32(&bytes, chunk.flags);
        appendU32(&bytes, chunk.version);
        appendU32(&bytes, chunk.reserved0);
        appendU64(&bytes, chunk.payload.size());
        appendU64(&bytes, chunk.reserved1);
        bytes.insert(bytes.end(), chunk.payload.begin(), chunk.payload.end());
    }
    return bytes;
}

RawChunk& chunkById(std::vector<RawChunk>* chunks, ChunkId id)
{
    const std::uint32_t rawId = static_cast<std::uint32_t>(id);
    const auto found = std::find_if(
        chunks->begin(),
        chunks->end(),
        [rawId](const RawChunk& chunk) { return chunk.id == rawId; });
    expect(found != chunks->end(), "requested raw chunk exists");
    return *found;
}

std::size_t chunkHeaderOffset(const std::vector<std::uint8_t>& bytes, ChunkId id)
{
    const std::uint32_t rawId = static_cast<std::uint32_t>(id);
    std::size_t offset = xrphoton::ogfx::FileHeaderSize;
    while (offset + xrphoton::ogfx::ChunkHeaderSize <= bytes.size()) {
        if (readU32(bytes, offset) == rawId) {
            return offset;
        }
        const std::uint64_t size = readU64(bytes, offset + 16);
        offset += xrphoton::ogfx::ChunkHeaderSize + static_cast<std::size_t>(size);
        if (offset != bytes.size()) {
            const std::size_t remainder = offset % xrphoton::ogfx::ChunkAlignment;
            if (remainder != 0) {
                offset += xrphoton::ogfx::ChunkAlignment - remainder;
            }
        }
    }
    expect(false, "requested serialized chunk exists");
    return 0;
}

constexpr std::string_view expectedChunkName(ChunkId id)
{
    switch (id) {
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

bool modelIsEmpty(const Model& model)
{
    return model.positions.empty()
        && model.attributes.empty()
        && model.indices.empty()
        && model.geometries.empty()
        && model.meshes.empty()
        && model.materials.empty();
}

void expectRejected(
    const std::vector<std::uint8_t>& bytes,
    std::string_view expectedChunk,
    std::string_view expectedField)
{
    const DecodeResult result = xrphoton::ogfx::decodeModel(bytes, "invalid.ogfx");
    expect(!result, "malformed OGFx is rejected");
    expect(modelIsEmpty(result.model), "rejection exposes no partial decoded model");
    expect(result.error.find("invalid.ogfx") != std::string::npos,
        "decoder diagnostic names its input");
    expect(result.error.find(expectedChunk) != std::string::npos,
        std::string("decoder diagnostic names chunk: ") + std::string(expectedChunk));
    expect(result.error.find(expectedField) != std::string::npos,
        std::string("decoder diagnostic names field: ") + std::string(expectedField));
    expect(result.error.find(": expected ") != std::string::npos
            && result.error.find(", found ") != std::string::npos,
        "decoder diagnostic states expected and found values");
}

bool modelsEqual(const Model& left, const Model& right)
{
    if (left.positions.size() != right.positions.size()
        || left.attributes.size() != right.attributes.size()
        || left.indices != right.indices
        || left.geometries.size() != right.geometries.size()
        || left.meshes.size() != right.meshes.size()
        || left.materials.size() != right.materials.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.positions.size(); ++index) {
        if (left.positions[index].x != right.positions[index].x
            || left.positions[index].y != right.positions[index].y
            || left.positions[index].z != right.positions[index].z) {
            return false;
        }
        const VertexAttributes& a = left.attributes[index];
        const VertexAttributes& b = right.attributes[index];
        if (a.nx != b.nx || a.ny != b.ny || a.nz != b.nz || a.u != b.u || a.v != b.v) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.geometries.size(); ++index) {
        const Geometry& a = left.geometries[index];
        const Geometry& b = right.geometries[index];
        if (a.firstVertex != b.firstVertex || a.vertexCount != b.vertexCount
            || a.firstIndex != b.firstIndex || a.indexCount != b.indexCount
            || a.materialIndex != b.materialIndex || a.alphaTested != b.alphaTested) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.meshes.size(); ++index) {
        if (left.meshes[index].firstGeometry != right.meshes[index].firstGeometry
            || left.meshes[index].geometryCount != right.meshes[index].geometryCount) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.materials.size(); ++index) {
        if (left.materials[index].baseColorFactor != right.materials[index].baseColorFactor
            || left.materials[index].alphaCutoff != right.materials[index].alphaCutoff
            || left.materials[index].baseColorTexture != right.materials[index].baseColorTexture) {
            return false;
        }
    }
    return true;
}

void testRoundTripAndExtensions()
{
    const Model quad = makeQuad();
    const std::vector<std::uint8_t> canonical = serialize(quad);
    const DecodeResult decoded = xrphoton::ogfx::decodeModel(canonical, "quad.ogfx");
    expect(static_cast<bool>(decoded), "the canonical quad decodes");
    if (!decoded) {
        std::cerr << decoded.error << '\n';
        return;
    }
    expect(modelsEqual(decoded.model, quad), "writer-to-decoder round trip preserves the model");
    const SerializeResult reserialized =
        xrphoton::ogfx::serializeModel(decoded.model, "round-trip.ogfx");
    expect(static_cast<bool>(reserialized), "the decoded quad serializes again");
    expect(reserialized.bytes == canonical, "writer-decoder-writer reproduces every byte");

    std::vector<RawChunk> reordered = splitChunks(canonical);
    std::reverse(reordered.begin(), reordered.end());
    expect(
        static_cast<bool>(xrphoton::ogfx::decodeModel(assembleFile(reordered), "reordered.ogfx")),
        "required chunks decode independently of file order");

    std::vector<RawChunk> extended = splitChunks(canonical);
    extended.push_back(RawChunk{0x8000, 0, 99, 0, 0, {1, 2, 3}});
    extended.push_back(RawChunk{
        static_cast<std::uint32_t>(ChunkId::Description), 0, 77, 0, 0, {4, 5}});
    expect(
        static_cast<bool>(xrphoton::ogfx::decodeModel(assembleFile(extended), "extended.ogfx")),
        "unknown optional chunks and the unpinned DESC chunk are skipped");

    std::vector<RawChunk> expandedBounds = splitChunks(canonical);
    RawChunk& model = chunkById(&expandedBounds, ChunkId::Model);
    writeF32(&model.payload, 8, -2.0f);
    writeF32(&model.payload, 12, -2.0f);
    writeF32(&model.payload, 20, 2.0f);
    writeF32(&model.payload, 24, 2.0f);
    writeF32(&model.payload, 44, 3.0f);
    RawChunk& geometry = chunkById(&expandedBounds, ChunkId::Geometries);
    writeF32(&geometry.payload, 24, -2.0f);
    writeF32(&geometry.payload, 28, -2.0f);
    writeF32(&geometry.payload, 36, 2.0f);
    writeF32(&geometry.payload, 40, 2.0f);
    RawChunk& attributes = chunkById(&expandedBounds, ChunkId::Attributes);
    writeF32(&attributes.payload, 12, 4.0f);
    expect(
        static_cast<bool>(xrphoton::ogfx::decodeModel(
            assembleFile(expandedBounds), "expanded-bounds.ogfx")),
        "enclosing noncanonical bounds and finite out-of-range UVs are valid");

    Model multipleMaterials = makeQuad();
    multipleMaterials.materials.emplace_back();
    const DecodeResult materialResult = xrphoton::ogfx::decodeModel(
        serialize(multipleMaterials), "multiple-materials.ogfx");
    expect(static_cast<bool>(materialResult),
        "the M4 runtime accepts multiple texture-free materials");
    expect(materialResult && materialResult.model.materials.size() == 2,
        "all decoded materials remain model-owned");
}

void testFileAndChunkFraming()
{
    const std::vector<std::uint8_t> canonical = serialize(makeQuad());
    expectRejected({}, "OGFX_FILE", "file byte size");
    expectRejected(
        std::vector<std::uint8_t>(canonical.begin(), canonical.begin() + 15),
        "OGFX_FILE",
        "file byte size");

    std::vector<std::uint8_t> bytes = canonical;
    bytes[0] = 'X';
    expectRejected(bytes, "OGFX_FILE", "magic");
    bytes = canonical;
    writeU32(&bytes, 4, 2);
    expectRejected(bytes, "OGFX_FILE", "containerVersion");
    bytes = canonical;
    writeU32(&bytes, 8, 32);
    expectRejected(bytes, "OGFX_FILE", "headerSize");
    bytes = canonical;
    writeU32(&bytes, 12, 1);
    expectRejected(bytes, "OGFX_FILE", "reserved");

    bytes = canonical;
    bytes.resize(506);
    expectRejected(bytes, "OGFX_FILE", "chunk header byte range");
    bytes = canonical;
    writeU64(&bytes, 32, std::numeric_limits<std::uint64_t>::max());
    expectRejected(bytes, "OGFX_MODEL", "payload end");
    bytes = canonical;
    const std::size_t indexHeader = chunkHeaderOffset(bytes, ChunkId::Indices);
    writeU64(&bytes, indexHeader + 16, 28);
    expectRejected(bytes, "OGFX_INDICES", "payload byte range");
    bytes = canonical;
    bytes[216] = 1;
    expectRejected(bytes, "OGFX_MESHES", "alignment padding");
    bytes = canonical;
    bytes.push_back(0);
    expectRejected(bytes, "OGFX_INDICES", "trailing bytes");

    bytes = canonical;
    writeU32(&bytes, 20, 2);
    expectRejected(bytes, "OGFX_MODEL", "flags");
    bytes = canonical;
    writeU32(&bytes, 28, 1);
    expectRejected(bytes, "OGFX_MODEL", "reserved0");
    bytes = canonical;
    writeU64(&bytes, 40, 1);
    expectRejected(bytes, "OGFX_MODEL", "reserved1");

    std::vector<RawChunk> chunks = splitChunks(canonical);
    chunks.push_back(RawChunk{0x8001, 1, 1, 0, 0, {}});
    expectRejected(assembleFile(chunks), "unknown", "chunk id");
    chunks = splitChunks(canonical);
    chunks.push_back(RawChunk{0, 1, 1, 0, 0, {}});
    expectRejected(assembleFile(chunks), "unknown", "chunk id");
    chunks = splitChunks(canonical);
    chunks.push_back(RawChunk{0x8001, 0, 1, 0, 0, {}});
    chunks.push_back(RawChunk{0x8001, 0, 2, 0, 0, {}});
    expectRejected(assembleFile(chunks), "unknown", "occurrence count");

    chunks = splitChunks(canonical);
    for (std::uint32_t index = 0;
         index < xrphoton::ogfx::MaximumChunkCount;
         ++index) {
        chunks.push_back(RawChunk{0x10000u + index, 0, 1, 0, 0, {}});
    }
    expectRejected(assembleFile(chunks), "OGFX_FILE", "chunk count");
}

void testRequiredChunkRules()
{
    const std::vector<std::uint8_t> canonical = serialize(makeQuad());
    const std::vector<RawChunk> source = splitChunks(canonical);
    constexpr std::array requiredIds{
        ChunkId::Model,
        ChunkId::Geometries,
        ChunkId::Meshes,
        ChunkId::Materials,
        ChunkId::Positions,
        ChunkId::Attributes,
        ChunkId::Indices,
    };

    for (ChunkId id : requiredIds) {
        const std::uint32_t rawId = static_cast<std::uint32_t>(id);
        std::vector<RawChunk> chunks = source;
        chunks.erase(
            std::remove_if(
                chunks.begin(),
                chunks.end(),
                [rawId](const RawChunk& chunk) { return chunk.id == rawId; }),
            chunks.end());
        expectRejected(assembleFile(chunks), expectedChunkName(id), "occurrence count");

        chunks = source;
        chunkById(&chunks, id).flags = 0;
        expectRejected(assembleFile(chunks), expectedChunkName(id), "flags");

        chunks = source;
        chunkById(&chunks, id).version = 2;
        expectRejected(assembleFile(chunks), expectedChunkName(id), "version");

        chunks = source;
        chunks.push_back(chunkById(&chunks, id));
        expectRejected(assembleFile(chunks), expectedChunkName(id), "occurrence count");
    }
}

void testPayloadFramingAndScalars()
{
    const std::vector<std::uint8_t> canonical = serialize(makeQuad());

    std::vector<RawChunk> chunks = splitChunks(canonical);
    chunkById(&chunks, ChunkId::Model).payload.pop_back();
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "byteSize");

    constexpr std::array pureArrays{
        std::pair{ChunkId::Geometries, "record count"},
        std::pair{ChunkId::Meshes, "record count"},
        std::pair{ChunkId::Positions, "element count"},
        std::pair{ChunkId::Attributes, "element count"},
        std::pair{ChunkId::Indices, "element count"},
    };
    for (const auto& [id, field] : pureArrays) {
        chunks = splitChunks(canonical);
        chunkById(&chunks, id).payload.clear();
        expectRejected(assembleFile(chunks), "OGFX_", field);
        chunks = splitChunks(canonical);
        chunkById(&chunks, id).payload.pop_back();
        expectRejected(assembleFile(chunks), "OGFX_", "byteSize");
    }

    chunks = splitChunks(canonical);
    chunkById(&chunks, ChunkId::Materials).payload.resize(12);
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "byteSize");
    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Materials).payload, 0, 2);
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "byteSize");
    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Materials).payload, 0, 0);
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "materialCount");
    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Materials).payload, 8, 1);
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "payload reserved0");
    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Materials).payload, 12, 1);
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "payload reserved1");
    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Materials).payload, 40, 1);
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "materials[0].reserved0");
    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Materials).payload, 44, 1);
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "materials[0].reserved1");

    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Model).payload, 0, 1);
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "modelType");
    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Model).payload, 4, 1);
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "modelFlags");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Model).payload, 8,
        std::numeric_limits<float>::quiet_NaN());
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "aabb.minimum/maximum");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Model).payload, 8, 1.0f);
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "field aabb:");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Model).payload, 44, -1.0f);
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "sphereRadius");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Model).payload, 44,
        std::numeric_limits<float>::infinity());
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "sphereRadius");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Model).payload, 32,
        std::numeric_limits<float>::infinity());
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "sphereCenter");

    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Positions).payload, 0,
        std::numeric_limits<float>::infinity());
    expectRejected(assembleFile(chunks), "OGFX_POSITIONS", "positions[0].x/y/z");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Attributes).payload, 12,
        std::numeric_limits<float>::quiet_NaN());
    expectRejected(assembleFile(chunks), "OGFX_ATTRIBUTES", "attributes[0].nx/ny/nz/u/v");
    chunks = splitChunks(canonical);
    RawChunk& attributes = chunkById(&chunks, ChunkId::Attributes);
    writeF32(&attributes.payload, 0, 0.0f);
    writeF32(&attributes.payload, 4, 0.0f);
    writeF32(&attributes.payload, 8, 0.0f);
    expectRejected(assembleFile(chunks), "OGFX_ATTRIBUTES", "normal length squared");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Materials).payload, 16,
        std::numeric_limits<float>::quiet_NaN());
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "baseColorFactor");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Materials).payload, 32,
        std::numeric_limits<float>::infinity());
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "alphaCutoff");

    chunks = splitChunks(canonical);
    chunkById(&chunks, ChunkId::Attributes).payload.resize(3 * xrphoton::ogfx::AttributeRecordSize);
    expectRejected(assembleFile(chunks), "OGFX_ATTRIBUTES", "element count");
}

void testRangesBoundsAndRuntimeGates()
{
    const std::vector<std::uint8_t> canonical = serialize(makeQuad());
    std::vector<RawChunk> chunks = splitChunks(canonical);
    RawChunk* geometry = &chunkById(&chunks, ChunkId::Geometries);
    writeU32(&geometry->payload, 4, 0);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "vertexCount");
    chunks = splitChunks(canonical);
    geometry = &chunkById(&chunks, ChunkId::Geometries);
    writeU32(&geometry->payload, 12, 5);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "indexCount");
    chunks = splitChunks(canonical);
    geometry = &chunkById(&chunks, ChunkId::Geometries);
    writeU32(&geometry->payload, 0, 1);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "firstVertex");
    chunks = splitChunks(canonical);
    geometry = &chunkById(&chunks, ChunkId::Geometries);
    writeU32(&geometry->payload, 8, 1);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "firstIndex");
    chunks = splitChunks(canonical);
    geometry = &chunkById(&chunks, ChunkId::Geometries);
    writeU32(&geometry->payload, 4, 5);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "vertex range end");
    chunks = splitChunks(canonical);
    geometry = &chunkById(&chunks, ChunkId::Geometries);
    writeU32(&geometry->payload, 12, 9);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "index range end");
    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Indices).payload, 20, 4);
    expectRejected(assembleFile(chunks), "OGFX_INDICES", "geometry-local value");
    chunks = splitChunks(canonical);
    geometry = &chunkById(&chunks, ChunkId::Geometries);
    writeU32(&geometry->payload, 16, 1);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "materialIndex");
    chunks = splitChunks(canonical);
    geometry = &chunkById(&chunks, ChunkId::Geometries);
    writeU32(&geometry->payload, 20, 2);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "geometryFlags");

    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Geometries).payload, 24,
        std::numeric_limits<float>::quiet_NaN());
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "aabb.minimum/maximum");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Geometries).payload, 24, 1.0f);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "field geometries[0].aabb:");

    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Geometries).payload, 24, 0.0f);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "aabb enclosure");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Model).payload, 8, 0.0f);
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "aabb enclosure");
    chunks = splitChunks(canonical);
    writeF32(&chunkById(&chunks, ChunkId::Model).payload, 44, 0.1f);
    expectRejected(assembleFile(chunks), "OGFX_MODEL", "sphere enclosure");

    chunks = splitChunks(canonical);
    writeU32(&chunkById(&chunks, ChunkId::Meshes).payload, 4, 2);
    expectRejected(assembleFile(chunks), "OGFX_MESHES", "geometry range end");

    Model alpha = makeQuad();
    alpha.geometries[0].alphaTested = true;
    expectRejected(serialize(alpha), "OGFX_GEOMETRIES", "geometryFlags");

    const std::vector<std::uint8_t> twoGeometry = serialize(makeTwoGeometryModel());
    expectRejected(twoGeometry, "OGFX_MESHES", "M4 runtime record count");
    Model oneMeshTwoGeometries = makeTwoGeometryModel();
    oneMeshTwoGeometries.meshes = {Mesh{0, 2}};
    expectRejected(
        serialize(oneMeshTwoGeometries),
        "OGFX_GEOMETRIES",
        "M4 runtime record count");
    chunks = splitChunks(twoGeometry);
    RawChunk& meshes = chunkById(&chunks, ChunkId::Meshes);
    writeU32(&meshes.payload, 4, 0);
    expectRejected(assembleFile(chunks), "OGFX_MESHES", "geometryCount");
    chunks = splitChunks(twoGeometry);
    writeU32(&chunkById(&chunks, ChunkId::Meshes).payload, 8, 0);
    expectRejected(assembleFile(chunks), "OGFX_MESHES", "firstGeometry");
    chunks = splitChunks(twoGeometry);
    writeU32(&chunkById(&chunks, ChunkId::Meshes).payload, 12, 2);
    expectRejected(assembleFile(chunks), "OGFX_MESHES", "geometry range end");
    chunks = splitChunks(twoGeometry);
    chunkById(&chunks, ChunkId::Meshes).payload.resize(8);
    writeU32(&chunkById(&chunks, ChunkId::Meshes).payload, 4, 1);
    expectRejected(assembleFile(chunks), "OGFX_MESHES", "final geometry partition end");

    chunks = splitChunks(canonical);
    RawChunk& positions = chunkById(&chunks, ChunkId::Positions);
    const std::vector<std::uint8_t> extraPosition(
        positions.payload.end() - 12,
        positions.payload.end());
    positions.payload.insert(
        positions.payload.end(), extraPosition.begin(), extraPosition.end());
    RawChunk& extraAttributes = chunkById(&chunks, ChunkId::Attributes);
    const std::vector<std::uint8_t> extraAttribute(
        extraAttributes.payload.end() - 20,
        extraAttributes.payload.end());
    extraAttributes.payload.insert(
        extraAttributes.payload.end(),
        extraAttribute.begin(),
        extraAttribute.end());
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "final vertex partition end");

    chunks = splitChunks(canonical);
    appendU32(&chunkById(&chunks, ChunkId::Indices).payload, 0);
    appendU32(&chunkById(&chunks, ChunkId::Indices).payload, 0);
    appendU32(&chunkById(&chunks, ChunkId::Indices).payload, 0);
    expectRejected(assembleFile(chunks), "OGFX_GEOMETRIES", "final index partition end");
}

void setMaterialArena(
    RawChunk* materials,
    std::vector<std::uint8_t> arena,
    std::uint32_t textureOffset = xrphoton::ogfx::NoTextureReference)
{
    materials->payload.resize(xrphoton::ogfx::MaterialHeaderSize
        + xrphoton::ogfx::MaterialRecordSize);
    writeU32(&materials->payload, 4, static_cast<std::uint32_t>(arena.size()));
    writeU32(&materials->payload, 36, textureOffset);
    materials->payload.insert(materials->payload.end(), arena.begin(), arena.end());
}

void testStringValidationAndTextureGate()
{
    const std::vector<std::uint8_t> canonical = serialize(makeQuad());
    std::vector<RawChunk> chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), {1});
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "string arena");

    chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), {0, 0});
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "string length");

    std::vector<std::uint8_t> tooLong;
    appendU16(&tooLong, 4097);
    tooLong.resize(4099, 'a');
    chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), std::move(tooLong));
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "string length");

    chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), {3, 0, 'a'});
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "string byte range");
    chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), {1, 0, 'a', 0});
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "string arena");
    chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), {2, 0, 0xc0, 0xaf});
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "string UTF-8");
    chunks = splitChunks(canonical);
    setMaterialArena(
        &chunkById(&chunks, ChunkId::Materials),
        {3, 0, 0xed, 0xa0, 0x80});
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "string UTF-8");
    chunks = splitChunks(canonical);
    setMaterialArena(
        &chunkById(&chunks, ChunkId::Materials),
        {4, 0, 0xf4, 0x90, 0x80, 0x80});
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "string UTF-8");
    chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), {2, 0, 0xe2, 0x82});
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "string UTF-8");

    chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), {1, 0, 'a'}, 1);
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "textureRefOffset");
    chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), {1, 0, 'a'});
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "stringByteSize");
    chunks = splitChunks(canonical);
    setMaterialArena(&chunkById(&chunks, ChunkId::Materials), {1, 0, 'a'}, 0);
    expectRejected(assembleFile(chunks), "OGFX_MATERIALS", "textureRefOffset");

    const std::vector<std::uint8_t> severalStrings{
        1, 0, 'a',
        2, 0, 'b', 'c',
        1, 0, 'd',
    };
    chunks = splitChunks(canonical);
    setMaterialArena(
        &chunkById(&chunks, ChunkId::Materials),
        severalStrings,
        7);
    const std::vector<std::uint8_t> validLastReference = assembleFile(chunks);
    expectRejected(validLastReference, "OGFX_MATERIALS", "textureRefOffset");
    expect(
        xrphoton::ogfx::decodeModel(validLastReference, "strings.ogfx")
                .error.find("UINT32_MAX in the M4 runtime") != std::string::npos,
        "a valid last-entry offset reaches the runtime texture capability gate");
    chunks = splitChunks(canonical);
    setMaterialArena(
        &chunkById(&chunks, ChunkId::Materials),
        severalStrings,
        4);
    const std::vector<std::uint8_t> middleReference = assembleFile(chunks);
    expectRejected(middleReference, "OGFX_MATERIALS", "textureRefOffset");
    expect(
        xrphoton::ogfx::decodeModel(middleReference, "strings.ogfx")
                .error.find("start of a string entry") != std::string::npos,
        "an offset into a string body fails schema validation before the runtime gate");
}
}

int main()
{
    testRoundTripAndExtensions();
    testFileAndChunkFraming();
    testRequiredChunkRules();
    testPayloadFramingAndScalars();
    testRangesBoundsAndRuntimeGates();
    testStringValidationAndTextureGate();

    if (failureCount != 0) {
        std::cerr << failureCount << " OGFx decoder test assertion(s) failed.\n";
        return 1;
    }
    return 0;
}
