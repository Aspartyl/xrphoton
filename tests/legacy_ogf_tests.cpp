#include "legacy_ogf.hpp"
#include "ogfx.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using xrphoton::legacy_ogf::decodeStaticModel;
using xrphoton::ogfx::DecodeResult;
using xrphoton::ogfx::Model;
using xrphoton::ogfx::SerializeResult;

constexpr std::uint32_t HeaderChunkId = 0x1;
constexpr std::uint32_t TextureChunkId = 0x2;
constexpr std::uint32_t VerticesChunkId = 0x3;
constexpr std::uint32_t IndicesChunkId = 0x4;

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

void appendF32(std::vector<std::uint8_t>* bytes, float value)
{
    appendU32(bytes, std::bit_cast<std::uint32_t>(value));
}

void writeU16(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint16_t value)
{
    (*bytes)[offset] = static_cast<std::uint8_t>(value);
    (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void writeU32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value)
{
    (*bytes)[offset] = static_cast<std::uint8_t>(value);
    (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    (*bytes)[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    (*bytes)[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void writeF32(std::vector<std::uint8_t>* bytes, std::size_t offset, float value)
{
    writeU32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

float readF32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return std::bit_cast<float>(readU32(bytes, offset));
}

struct RawChunk
{
    std::uint32_t id = 0;
    std::vector<std::uint8_t> payload;
};

void appendPosition(
    std::vector<std::uint8_t>* bytes,
    float x,
    float y,
    float z)
{
    appendF32(bytes, x);
    appendF32(bytes, y);
    appendF32(bytes, z);
}

std::vector<RawChunk> makeSyntheticChunks()
{
    RawChunk header{.id = HeaderChunkId, .payload = {}};
    header.payload.push_back(xrphoton::legacy_ogf::SupportedVersion);
    header.payload.push_back(xrphoton::legacy_ogf::SupportedModelType);
    appendU16(&header.payload, xrphoton::legacy_ogf::SupportedShaderId);
    appendPosition(&header.payload, 1.0f, -2.0f, 3.0f);
    appendPosition(&header.payload, 5.0f, 6.0f, 7.0f);
    appendPosition(&header.payload, 3.0f, 2.0f, 5.0f);
    float enclosingRadius = static_cast<float>(std::sqrt(24.0));
    while (static_cast<double>(enclosingRadius) * enclosingRadius < 24.0) {
        enclosingRadius = std::nextafter(
            enclosingRadius, std::numeric_limits<float>::infinity());
    }
    appendF32(
        &header.payload,
        std::nextafter(enclosingRadius, 0.0f));

    RawChunk texture{.id = TextureChunkId, .payload = {}};
    constexpr std::string_view textureName = "ston\\synthetic_asymmetric";
    texture.payload.insert(
        texture.payload.end(), textureName.begin(), textureName.end());
    texture.payload.push_back(0);
    constexpr std::string_view shaderName = "default";
    texture.payload.insert(
        texture.payload.end(), shaderName.begin(), shaderName.end());
    texture.payload.push_back(0);

    // cross(p1 - p0, p2 - p0) = (0, -16, 32). This normalized value pins
    // pass-through axes, normals, and triangle order without a symmetric probe.
    constexpr float normalY = -0.4472135901451111f;
    constexpr float normalZ = 0.8944271802902222f;
    RawChunk vertices{.id = VerticesChunkId, .payload = {}};
    appendU32(&vertices.payload, xrphoton::legacy_ogf::SupportedVertexFormat);
    appendU32(&vertices.payload, 3);
    const std::array<std::array<float, 8>, 3> records{{
        {1.0f, -2.0f, 3.0f, 0.0f, normalY, normalZ, 0.125f, 0.25f},
        {5.0f, -2.0f, 3.0f, 0.0f, normalY, normalZ, 0.75f, 0.5f},
        {1.0f,  6.0f, 7.0f, 0.0f, normalY, normalZ, 0.375f, 0.875f},
    }};
    for (const auto& record : records) {
        for (float value : record) {
            appendF32(&vertices.payload, value);
        }
    }

    RawChunk indices{.id = IndicesChunkId, .payload = {}};
    appendU32(&indices.payload, 3);
    appendU16(&indices.payload, 0);
    appendU16(&indices.payload, 1);
    appendU16(&indices.payload, 2);
    return {header, texture, vertices, indices};
}

std::vector<std::uint8_t> assemble(const std::vector<RawChunk>& chunks)
{
    std::vector<std::uint8_t> bytes;
    for (const RawChunk& chunk : chunks) {
        appendU32(&bytes, chunk.id);
        appendU32(&bytes, static_cast<std::uint32_t>(chunk.payload.size()));
        bytes.insert(bytes.end(), chunk.payload.begin(), chunk.payload.end());
    }
    return bytes;
}

bool writeBytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    std::error_code directoryError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), directoryError);
    }
    if (directoryError) {
        std::cerr << "FAIL: could not create CLI fixture directory: "
                  << directoryError.message() << '\n';
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "FAIL: could not open CLI fixture output.\n";
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::cerr << "FAIL: could not write complete CLI fixture.\n";
        return false;
    }
    return true;
}

bool readBytes(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>* bytes)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        std::cerr << "FAIL: could not open test file " << path << ".\n";
        return false;
    }
    const std::streampos end = input.tellg();
    if (end < 0) {
        std::cerr << "FAIL: could not determine test file size.\n";
        return false;
    }
    bytes->resize(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes->empty()) {
        input.read(
            reinterpret_cast<char*>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
    }
    if (!input) {
        std::cerr << "FAIL: could not read the complete test file.\n";
        return false;
    }
    return true;
}

RawChunk& chunkById(std::vector<RawChunk>* chunks, std::uint32_t id)
{
    const auto found = std::find_if(
        chunks->begin(),
        chunks->end(),
        [id](const RawChunk& chunk) { return chunk.id == id; });
    expect(found != chunks->end(), "requested synthetic OGF chunk exists");
    return *found;
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
        const auto& a = left.positions[index];
        const auto& b = right.positions[index];
        const auto& aa = left.attributes[index];
        const auto& ba = right.attributes[index];
        if (a.x != b.x || a.y != b.y || a.z != b.z
            || aa.nx != ba.nx || aa.ny != ba.ny || aa.nz != ba.nz
            || aa.u != ba.u || aa.v != ba.v) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.geometries.size(); ++index) {
        const auto& a = left.geometries[index];
        const auto& b = right.geometries[index];
        if (a.firstVertex != b.firstVertex || a.vertexCount != b.vertexCount
            || a.firstIndex != b.firstIndex || a.indexCount != b.indexCount
            || a.materialIndex != b.materialIndex || a.alphaTested != b.alphaTested) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.meshes.size(); ++index) {
        const auto& a = left.meshes[index];
        const auto& b = right.meshes[index];
        if (a.firstGeometry != b.firstGeometry || a.geometryCount != b.geometryCount) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.materials.size(); ++index) {
        const auto& a = left.materials[index];
        const auto& b = right.materials[index];
        if (a.baseColorFactor != b.baseColorFactor
            || a.alphaCutoff != b.alphaCutoff
            || a.baseColorTexture != b.baseColorTexture) {
            return false;
        }
    }
    return true;
}

void expectRejected(
    const std::vector<std::uint8_t>& bytes,
    std::string_view expectedChunk,
    std::string_view expectedField)
{
    const DecodeResult result = decodeStaticModel(bytes, "invalid-source.ogf");
    expect(!result, "unsupported or malformed legacy OGF is rejected");
    expect(modelIsEmpty(result.model), "legacy rejection exposes no partial model");
    expect(result.error.find("invalid-source.ogf") != std::string::npos,
        "legacy rejection names its input");
    expect(result.error.find("legacy OGF decoder") != std::string::npos,
        "legacy rejection names its decoder boundary");
    expect(result.error.find(expectedChunk) != std::string::npos,
        std::string("legacy rejection names chunk: ") + std::string(expectedChunk));
    expect(result.error.find(expectedField) != std::string::npos,
        std::string("legacy rejection names field: ") + std::string(expectedField));
    expect(result.error.find(": expected ") != std::string::npos
            && result.error.find(", found ") != std::string::npos,
        "legacy rejection states expected and found values");
}

void testAcceptedProfile()
{
    const std::vector<RawChunk> chunks = makeSyntheticChunks();
    const std::vector<std::uint8_t> source = assemble(chunks);
    const DecodeResult decoded = decodeStaticModel(source, "synthetic.ogf");
    expect(static_cast<bool>(decoded), "the pinned synthetic OGF v4 profile decodes");
    if (!decoded) {
        std::cerr << decoded.error << '\n';
        return;
    }

    const Model& model = decoded.model;
    expect(model.positions.size() == 3 && model.attributes.size() == 3,
        "legacy vertices become matching OGFx position and attribute streams");
    expect(model.positions[0].x == 1.0f && model.positions[0].y == -2.0f
            && model.positions[0].z == 3.0f,
        "legacy position axes pass through unchanged");
    expect(model.attributes[0].ny < 0.0f && model.attributes[0].nz > 0.0f
            && model.attributes[0].u == 0.125f && model.attributes[0].v == 0.25f,
        "legacy normals and UVs pass through unchanged");
    expect(model.indices == std::vector<std::uint32_t>({0, 1, 2}),
        "legacy u16 indices widen to u32 without reordering");
    expect(model.geometries.size() == 1
            && model.geometries[0].firstVertex == 0
            && model.geometries[0].vertexCount == 3
            && model.geometries[0].firstIndex == 0
            && model.geometries[0].indexCount == 3
            && model.geometries[0].materialIndex == 0
            && !model.geometries[0].alphaTested,
        "the adapter creates one complete opaque geometry");
    expect(model.meshes.size() == 1
            && model.meshes[0].firstGeometry == 0
            && model.meshes[0].geometryCount == 1,
        "the adapter creates one mesh owning that geometry");
    expect(model.materials.size() == 1
            && model.materials[0].baseColorTexture == "ston\\synthetic_asymmetric",
        "the default shader mapping preserves the logical texture name");

    const SerializeResult first =
        xrphoton::ogfx::serializeModel(model, "synthetic-first.ogfx");
    const SerializeResult second =
        xrphoton::ogfx::serializeModel(model, "synthetic-second.ogfx");
    expect(static_cast<bool>(first) && static_cast<bool>(second),
        "the canonical writer accepts the adapted compiler model");
    if (!first || !second) {
        if (!first.error.empty()) {
            std::cerr << first.error << '\n';
        }
        if (!second.error.empty()) {
            std::cerr << second.error << '\n';
        }
        return;
    }
    expect(first.bytes == second.bytes,
        "repeated legacy conversion produces deterministic OGFx bytes");
    expect(readF32(first.bytes, 92) < 5.0f
            && static_cast<double>(readF32(first.bytes, 92))
                    * readF32(first.bytes, 92) >= 24.0,
        "the canonical writer regenerates a tight outward-rounded sphere");

    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(first.bytes, "synthetic-schema.ogfx");
    expect(static_cast<bool>(schema), "schema decoding accepts converted OGFx");
    if (schema) {
        expect(modelsEqual(schema.model, model),
            "schema decoding reconstructs the complete adapted model");
        const SerializeResult roundTrip =
            xrphoton::ogfx::serializeModel(schema.model, "synthetic-round-trip.ogfx");
        expect(static_cast<bool>(roundTrip) && roundTrip.bytes == first.bytes,
            "legacy adapter writer-schema-writer round trip is byte exact");
    } else {
        std::cerr << schema.error << '\n';
    }
    const DecodeResult runtime =
        xrphoton::ogfx::decodeModel(first.bytes, "synthetic-runtime.ogfx");
    expect(!runtime
            && runtime.error.find("UINT32_MAX in the M4 runtime") != std::string::npos,
        "converted textured output remains explicitly offline-only");

    std::vector<RawChunk> reordered = chunks;
    std::reverse(reordered.begin(), reordered.end());
    const DecodeResult reorderedResult =
        decodeStaticModel(assemble(reordered), "reordered.ogf");
    expect(static_cast<bool>(reorderedResult)
            && modelsEqual(reorderedResult.model, model),
        "legacy direct chunk order does not affect the decoded model");
}

void testFramingAndProfileRejections()
{
    const std::vector<std::uint8_t> canonical = assemble(makeSyntheticChunks());
    expectRejected({}, "OGF_FILE", "file byte size");

    std::vector<std::uint8_t> bytes = canonical;
    bytes.push_back(0);
    expectRejected(bytes, "OGF_FILE", "trailing bytes");

    bytes = canonical;
    writeU32(&bytes, 4, std::numeric_limits<std::uint32_t>::max());
    expectRejected(bytes, "OGF_HEADER", "payload byte range");

    std::vector<RawChunk> chunks = makeSyntheticChunks();
    chunks.push_back(RawChunk{.id = 5, .payload = {}});
    expectRejected(assemble(chunks), "OGF_UNKNOWN", "chunk id");

    chunks = makeSyntheticChunks();
    chunks[0].id |= 0x80000000u;
    expectRejected(assemble(chunks), "OGF_HEADER", "compression flag");

    chunks = makeSyntheticChunks();
    chunks.push_back(chunks[0]);
    expectRejected(assemble(chunks), "OGF_HEADER", "occurrence count");

    chunks = makeSyntheticChunks();
    chunks.erase(chunks.begin() + 1);
    expectRejected(assemble(chunks), "OGF_TEXTURE", "presence");
}

void testHeaderAndBoundsRejections()
{
    std::vector<RawChunk> chunks = makeSyntheticChunks();
    chunkById(&chunks, HeaderChunkId).payload.pop_back();
    expectRejected(assemble(chunks), "OGF_HEADER", "byteSize");

    chunks = makeSyntheticChunks();
    chunkById(&chunks, HeaderChunkId).payload[0] = 3;
    expectRejected(assemble(chunks), "OGF_HEADER", "formatVersion");

    chunks = makeSyntheticChunks();
    chunkById(&chunks, HeaderChunkId).payload[1] = 1;
    expectRejected(assemble(chunks), "OGF_HEADER", "modelType");

    chunks = makeSyntheticChunks();
    writeU16(&chunkById(&chunks, HeaderChunkId).payload, 2, 1);
    expectRejected(assemble(chunks), "OGF_HEADER", "shaderId");

    chunks = makeSyntheticChunks();
    writeF32(
        &chunkById(&chunks, HeaderChunkId).payload,
        4,
        std::numeric_limits<float>::infinity());
    expectRejected(assemble(chunks), "OGF_HEADER", "AABB min/max");

    chunks = makeSyntheticChunks();
    writeF32(&chunkById(&chunks, HeaderChunkId).payload, 4, 6.0f);
    expectRejected(assemble(chunks), "OGF_HEADER", "AABB ordering");

    chunks = makeSyntheticChunks();
    writeF32(&chunkById(&chunks, HeaderChunkId).payload, 4, 0.0f);
    expectRejected(assemble(chunks), "OGF_HEADER", "AABB extrema");

    chunks = makeSyntheticChunks();
    writeF32(&chunkById(&chunks, HeaderChunkId).payload, 40, -1.0f);
    expectRejected(assemble(chunks), "OGF_HEADER", "bounding sphere");

    chunks = makeSyntheticChunks();
    RawChunk& oneUlpShortHeader = chunkById(&chunks, HeaderChunkId);
    const float oneUlpShortRadius = readF32(oneUlpShortHeader.payload, 40);
    writeF32(
        &oneUlpShortHeader.payload,
        40,
        std::nextafter(oneUlpShortRadius, 0.0f));
    expectRejected(assemble(chunks), "OGF_HEADER", "sphere enclosure");

    chunks = makeSyntheticChunks();
    writeF32(&chunkById(&chunks, HeaderChunkId).payload, 40, 0.0f);
    expectRejected(assemble(chunks), "OGF_HEADER", "sphere enclosure");
}

void setTexturePayload(
    RawChunk* texture,
    std::string_view textureName,
    std::string_view shaderName)
{
    texture->payload.assign(textureName.begin(), textureName.end());
    texture->payload.push_back(0);
    texture->payload.insert(
        texture->payload.end(), shaderName.begin(), shaderName.end());
    texture->payload.push_back(0);
}

void testTextureRejections()
{
    std::vector<RawChunk> chunks = makeSyntheticChunks();
    chunkById(&chunks, TextureChunkId).payload.pop_back();
    expectRejected(assemble(chunks), "OGF_TEXTURE", "engineShaderName");

    chunks = makeSyntheticChunks();
    setTexturePayload(&chunkById(&chunks, TextureChunkId), "", "default");
    expectRejected(assemble(chunks), "OGF_TEXTURE", "textureName");

    chunks = makeSyntheticChunks();
    setTexturePayload(&chunkById(&chunks, TextureChunkId), "ston\\test", "default");
    chunkById(&chunks, TextureChunkId).payload.push_back(0);
    expectRejected(assemble(chunks), "OGF_TEXTURE", "payload byte range");

    chunks = makeSyntheticChunks();
    setTexturePayload(&chunkById(&chunks, TextureChunkId), "ston\\test", "other");
    expectRejected(assemble(chunks), "OGF_TEXTURE", "engineShaderName");

    chunks = makeSyntheticChunks();
    std::string maximumLength(
        xrphoton::legacy_ogf::MaximumSourceStringBytes, 'a');
    setTexturePayload(
        &chunkById(&chunks, TextureChunkId), maximumLength, "default");
    const DecodeResult maximumLengthResult =
        decodeStaticModel(assemble(chunks), "maximum-texture.ogf");
    expect(static_cast<bool>(maximumLengthResult)
            && maximumLengthResult.model.materials[0].baseColorTexture
                == maximumLength,
        "the 255-byte source string boundary is accepted and preserved");

    chunks = makeSyntheticChunks();
    std::string tooLong(
        xrphoton::legacy_ogf::MaximumSourceStringBytes + 1, 'a');
    setTexturePayload(&chunkById(&chunks, TextureChunkId), tooLong, "default");
    expectRejected(assemble(chunks), "OGF_TEXTURE", "textureName");

    chunks = makeSyntheticChunks();
    RawChunk& texture = chunkById(&chunks, TextureChunkId);
    texture.payload[0] = 0x80;
    expectRejected(assemble(chunks), "OGF_TEXTURE", "textureName");
}

void testGeometryRejections()
{
    std::vector<RawChunk> chunks = makeSyntheticChunks();
    chunkById(&chunks, VerticesChunkId).payload.resize(7);
    expectRejected(assemble(chunks), "OGF_VERTICES", "byteSize");

    chunks = makeSyntheticChunks();
    writeU32(&chunkById(&chunks, VerticesChunkId).payload, 0, 0x12071980u);
    expectRejected(assemble(chunks), "OGF_VERTICES", "vertexFormat");

    chunks = makeSyntheticChunks();
    writeU32(&chunkById(&chunks, VerticesChunkId).payload, 4, 0);
    expectRejected(assemble(chunks), "OGF_VERTICES", "vertexCount");

    chunks = makeSyntheticChunks();
    chunkById(&chunks, VerticesChunkId).payload.pop_back();
    expectRejected(assemble(chunks), "OGF_VERTICES", "byteSize");

    chunks = makeSyntheticChunks();
    writeF32(
        &chunkById(&chunks, VerticesChunkId).payload,
        8,
        std::numeric_limits<float>::quiet_NaN());
    expectRejected(assemble(chunks), "OGF_VERTICES", "vertices[0].position");

    chunks = makeSyntheticChunks();
    RawChunk& vertices = chunkById(&chunks, VerticesChunkId);
    writeF32(&vertices.payload, 20, 0.0f);
    writeF32(&vertices.payload, 24, 0.0f);
    writeF32(&vertices.payload, 28, 0.0f);
    expectRejected(assemble(chunks), "OGF_VERTICES", "normal length squared");

    chunks = makeSyntheticChunks();
    chunkById(&chunks, IndicesChunkId).payload.resize(3);
    expectRejected(assemble(chunks), "OGF_INDICES", "byteSize");

    chunks = makeSyntheticChunks();
    writeU32(&chunkById(&chunks, IndicesChunkId).payload, 0, 4);
    expectRejected(assemble(chunks), "OGF_INDICES", "indexCount");

    chunks = makeSyntheticChunks();
    chunkById(&chunks, IndicesChunkId).payload.pop_back();
    expectRejected(assemble(chunks), "OGF_INDICES", "byteSize");

    chunks = makeSyntheticChunks();
    writeU16(&chunkById(&chunks, IndicesChunkId).payload, 4, 3);
    expectRejected(assemble(chunks), "OGF_INDICES", "indices[0].value");

    chunks = makeSyntheticChunks();
    RawChunk& indices = chunkById(&chunks, IndicesChunkId);
    writeU16(&indices.payload, 4, 0);
    writeU16(&indices.payload, 6, 2);
    writeU16(&indices.payload, 8, 1);
    expectRejected(assemble(chunks), "OGF_INDICES", "triangles[0].winding");

    chunks = makeSyntheticChunks();
    RawChunk& oneFlippedNormal = chunkById(&chunks, VerticesChunkId);
    writeF32(
        &oneFlippedNormal.payload,
        24,
        -readF32(oneFlippedNormal.payload, 24));
    writeF32(
        &oneFlippedNormal.payload,
        28,
        -readF32(oneFlippedNormal.payload, 28));
    expectRejected(assemble(chunks), "OGF_INDICES", "triangles[0].winding");
}

void expectPinnedCorpusModel(const Model& model)
{
    expect(model.positions.size() == 1802 && model.attributes.size() == 1802,
        "plitka corpus contains the pinned 1802 complete vertices");
    expect(model.indices.size() == 3300,
        "plitka corpus contains the pinned 3300 indices");
    expect(model.geometries.size() == 1
            && model.geometries[0].firstVertex == 0
            && model.geometries[0].vertexCount == 1802
            && model.geometries[0].firstIndex == 0
            && model.geometries[0].indexCount == 3300
            && model.geometries[0].materialIndex == 0
            && !model.geometries[0].alphaTested,
        "plitka corpus maps to one complete opaque geometry");
    expect(model.meshes.size() == 1
            && model.meshes[0].firstGeometry == 0
            && model.meshes[0].geometryCount == 1,
        "plitka corpus maps to one mesh");
    expect(model.materials.size() == 1
            && model.materials[0].baseColorTexture
                == "ston\\ston_stena_marbl_m_03_back",
        "plitka corpus preserves its pinned logical texture name");
}

void testLocalCorpus(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, &bytes)) {
        ++failureCount;
        return;
    }

    const DecodeResult decoded = decodeStaticModel(bytes, path.string());
    expect(static_cast<bool>(decoded), "the local plitka1.ogf corpus asset decodes");
    if (!decoded) {
        std::cerr << decoded.error << '\n';
        return;
    }
    expectPinnedCorpusModel(decoded.model);

    const SerializeResult serialized =
        xrphoton::ogfx::serializeModel(decoded.model, "plitka1.ogfx");
    expect(static_cast<bool>(serialized), "the corpus model serializes canonically");
    if (serialized) {
        expect(serialized.bytes.size() == 71328,
            "the corpus produces the pinned 71328-byte OGFx output");
        expect(readU32(serialized.bytes, 92) == 0x3fede7e4u,
            "the corpus sphere is regenerated one outward f32 ULP");
        const DecodeResult schema =
            xrphoton::ogfx::decodeModelSchema(serialized.bytes, "plitka1.ogfx");
        expect(static_cast<bool>(schema) && modelsEqual(schema.model, decoded.model),
            "the corpus output passes complete OGFx schema reconstruction");
    } else {
        std::cerr << serialized.error << '\n';
    }
}

void verifyCorpusOutput(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& outputPath)
{
    std::vector<std::uint8_t> sourceBytes;
    std::vector<std::uint8_t> outputBytes;
    if (!readBytes(sourcePath, &sourceBytes)
        || !readBytes(outputPath, &outputBytes)) {
        ++failureCount;
        return;
    }

    const DecodeResult source = decodeStaticModel(sourceBytes, sourcePath.string());
    expect(static_cast<bool>(source),
        "the M4a source corpus decodes through the legacy adapter");
    if (!source) {
        std::cerr << source.error << '\n';
        return;
    }
    expectPinnedCorpusModel(source.model);

    expect(outputBytes.size() == 71328,
        "the persisted corpus output has the pinned canonical byte size");
    if (outputBytes.size() >= 96) {
        expect(readU32(outputBytes, 92) == 0x3fede7e4u,
            "the persisted corpus output has the regenerated outward sphere");
    }

    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(outputBytes, outputPath.string());
    expect(static_cast<bool>(schema),
        "the persisted corpus output passes complete OGFx schema decoding");
    if (!schema) {
        std::cerr << schema.error << '\n';
        return;
    }
    expect(modelsEqual(schema.model, source.model),
        "the persisted OGFx model exactly reconstructs the legacy source model");

    const SerializeResult canonical =
        xrphoton::ogfx::serializeModel(source.model, "corpus-proof.ogfx");
    expect(static_cast<bool>(canonical) && canonical.bytes == outputBytes,
        "the persisted output is byte-exact canonical writer output");
    const SerializeResult roundTrip =
        xrphoton::ogfx::serializeModel(schema.model, "corpus-round-trip.ogfx");
    expect(static_cast<bool>(roundTrip) && roundTrip.bytes == outputBytes,
        "the persisted corpus writer-schema-writer round trip is byte exact");

    const DecodeResult runtime =
        xrphoton::ogfx::decodeModel(outputBytes, outputPath.string());
    expect(!runtime
            && runtime.error.find("UINT32_MAX in the M4 runtime")
                != std::string::npos,
        "the proof preserves the runtime texture capability gate");
}

void verifyCliOutputs(
    const std::filesystem::path& firstPath,
    const std::filesystem::path& secondPath)
{
    std::vector<std::uint8_t> firstBytes;
    std::vector<std::uint8_t> secondBytes;
    if (!readBytes(firstPath, &firstBytes)
        || !readBytes(secondPath, &secondBytes)) {
        ++failureCount;
        return;
    }
    expect(firstBytes == secondBytes,
        "two asset-compiler invocations produce byte-identical output");

    const DecodeResult source =
        decodeStaticModel(assemble(makeSyntheticChunks()), "cli-source.ogf");
    expect(static_cast<bool>(source),
        "the expected CLI source model decodes in the verifier");
    if (!source) {
        std::cerr << source.error << '\n';
        return;
    }
    const SerializeResult expected =
        xrphoton::ogfx::serializeModel(source.model, "cli-expected.ogfx");
    expect(static_cast<bool>(expected) && firstBytes == expected.bytes,
        "the CLI output is the canonical writer output for the source model");

    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(firstBytes, firstPath.string());
    expect(static_cast<bool>(schema) && modelsEqual(schema.model, source.model),
        "the CLI output passes complete OGFx schema reconstruction");
}
}

int main(int argumentCount, char** arguments)
{
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--write-cli-fixture") {
        return writeBytes(arguments[2], assemble(makeSyntheticChunks())) ? 0 : 1;
    }
    if (argumentCount == 4
        && std::string_view(arguments[1]) == "--verify-cli-outputs") {
        verifyCliOutputs(arguments[2], arguments[3]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " asset compiler CLI test assertion(s) failed.\n";
            return 1;
        }
        return 0;
    }
    if (argumentCount == 4
        && std::string_view(arguments[1]) == "--verify-corpus-output") {
        verifyCorpusOutput(arguments[2], arguments[3]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " M4a offline corpus proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "M4a offline corpus proof passed.\n";
        return 0;
    }

    testAcceptedProfile();
    testFramingAndProfileRejections();
    testHeaderAndBoundsRejections();
    testTextureRejections();
    testGeometryRejections();

    if (argumentCount == 2) {
        testLocalCorpus(arguments[1]);
    } else if (argumentCount != 1) {
        std::cerr << "Usage: xrPhotonLegacyOgfTests [optional-corpus.ogf]\n";
        return 1;
    }

    if (failureCount != 0) {
        std::cerr << failureCount
                  << " legacy OGF adapter test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "Legacy OGF adapter tests passed.\n";
    return 0;
}
