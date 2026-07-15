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
using xrphoton::ogfx::Geometry;
using xrphoton::ogfx::Material;
using xrphoton::ogfx::Mesh;
using xrphoton::ogfx::Model;
using xrphoton::ogfx::Position;
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

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    expect(offset + 4 <= bytes.size(), "u32 read stays inside the serialized file");
    if (offset + 4 > bytes.size()) {
        return 0;
    }

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

float readF32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return std::bit_cast<float>(readU32(bytes, offset));
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
    model.geometries.push_back({
        .firstVertex = 0,
        .vertexCount = 4,
        .firstIndex = 0,
        .indexCount = 6,
        .materialIndex = 0,
        .alphaTested = false,
    });
    model.meshes.push_back({
        .firstGeometry = 0,
        .geometryCount = 1,
    });
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
        Geometry{3, 3, 3, 3, 0, true},
    };
    model.meshes = {
        Mesh{0, 1},
        Mesh{1, 1},
    };
    model.materials.emplace_back();
    return model;
}

void expectRejected(Model model, std::string_view expectedField)
{
    const SerializeResult result =
        xrphoton::ogfx::serializeModel(model, "invalid-fixture.ogfx");
    expect(!result, "invalid model is rejected");
    expect(result.bytes.empty(), "a rejected model returns no partial byte stream");
    expect(
        result.error.find("invalid-fixture.ogfx") != std::string::npos,
        "rejection names its input");
    expect(
        result.error.find(expectedField) != std::string::npos,
        std::string("rejection names field: ") + std::string(expectedField));
    expect(
        result.error.find("chunk OGFX_") != std::string::npos
            && result.error.find(" (0x") != std::string::npos
            && result.error.find(": expected ") != std::string::npos
            && result.error.find(", found ") != std::string::npos,
        "rejection follows the file/chunk/field/expected/found diagnostic contract");
}

void testGoldenQuad()
{
    const Model quad = makeQuad();
    const SerializeResult first = xrphoton::ogfx::serializeModel(quad, "first-name.ogfx");
    const SerializeResult second = xrphoton::ogfx::serializeModel(quad, "other-name.ogfx");
    expect(static_cast<bool>(first), "the M3b reference quad serializes");
    expect(static_cast<bool>(second), "the reference quad serializes a second time");
    if (!first || !second) {
        if (!first.error.empty()) {
            std::cerr << first.error << '\n';
        }
        if (!second.error.empty()) {
            std::cerr << second.error << '\n';
        }
        return;
    }

    const std::vector<std::uint8_t>& bytes = first.bytes;
    expect(bytes == second.bytes, "serialization is deterministic and ignores diagnostic names");
    expect(bytes.size() == 552, "the canonical texture-free quad is exactly 552 bytes");
    expect(
        bytes.size() >= xrphoton::ogfx::FileMagic.size()
            && std::equal(
                xrphoton::ogfx::FileMagic.begin(),
                xrphoton::ogfx::FileMagic.end(),
                bytes.begin()),
        "the file begins with the OGFX magic bytes");
    expect(readU32(bytes, 4) == 1, "container version is little-endian version 1");
    expect(readU32(bytes, 8) == 16, "file header records its 16-byte size");
    expect(readU32(bytes, 12) == 0, "file-header reserved field is zero");

    struct ExpectedChunk
    {
        std::size_t headerOffset;
        ChunkId id;
        std::uint64_t payloadSize;
    };
    constexpr std::array expectedChunks{
        ExpectedChunk{16, ChunkId::Model, 48},
        ExpectedChunk{96, ChunkId::Geometries, 48},
        ExpectedChunk{176, ChunkId::Meshes, 8},
        ExpectedChunk{224, ChunkId::Materials, 48},
        ExpectedChunk{304, ChunkId::Positions, 48},
        ExpectedChunk{384, ChunkId::Attributes, 80},
        ExpectedChunk{496, ChunkId::Indices, 24},
    };

    for (const ExpectedChunk& chunk : expectedChunks) {
        expect((chunk.headerOffset % 16) == 0, "golden chunk header offset is 16-byte aligned");
        expect(
            readU32(bytes, chunk.headerOffset) == static_cast<std::uint32_t>(chunk.id),
            "chunk id matches canonical ascending order");
        expect(readU32(bytes, chunk.headerOffset + 4) == 1, "required chunk flags equal 1");
        expect(readU32(bytes, chunk.headerOffset + 8) == 1, "required chunk version equals 1");
        expect(readU32(bytes, chunk.headerOffset + 12) == 0, "chunk reserved0 is zero");
        expect(readU64(bytes, chunk.headerOffset + 16) == chunk.payloadSize, "chunk byteSize is exact");
        expect(readU64(bytes, chunk.headerOffset + 24) == 0, "chunk reserved1 is zero");
        expect(((chunk.headerOffset + 32) % 16) == 0, "chunk payload is 16-byte aligned");
    }

    expect(readU32(bytes, 48) == 0, "MODEL modelType is static/normal");
    expect(readU32(bytes, 52) == 0, "MODEL modelFlags is zero");
    expect(readU32(bytes, 56) == 0xbf000000u, "MODEL minimum X is exact -0.5f");
    expect(readU32(bytes, 60) == 0xbf000000u, "MODEL minimum Y is exact -0.5f");
    expect(readU32(bytes, 64) == 0, "MODEL minimum Z is exact zero");
    expect(readU32(bytes, 68) == 0x3f000000u, "MODEL maximum X is exact 0.5f");
    expect(readU32(bytes, 72) == 0x3f000000u, "MODEL maximum Y is exact 0.5f");
    expect(readU32(bytes, 76) == 0, "MODEL maximum Z is exact zero");
    expect(readF32(bytes, 80) == 0.0f, "MODEL sphere center X is midpoint zero");
    expect(readF32(bytes, 84) == 0.0f, "MODEL sphere center Y is midpoint zero");
    expect(readF32(bytes, 88) == 0.0f, "MODEL sphere center Z is midpoint zero");
    expect(
        readU32(bytes, 92) == 0x3f3504f4u,
        "MODEL radius is the next enclosing f32 above sqrt(0.5)");

    expect(readU32(bytes, 128) == 0, "geometry firstVertex is zero");
    expect(readU32(bytes, 132) == 4, "geometry vertexCount is four");
    expect(readU32(bytes, 136) == 0, "geometry firstIndex is zero");
    expect(readU32(bytes, 140) == 6, "geometry indexCount is six");
    expect(readU32(bytes, 144) == 0, "geometry materialIndex is zero");
    expect(readU32(bytes, 148) == 0, "opaque geometry has no flags");
    expect(readU32(bytes, 152) == 0xbf000000u, "geometry AABB minimum X is exact");
    expect(readU32(bytes, 164) == 0x3f000000u, "geometry AABB maximum X is exact");

    expect(readU32(bytes, 208) == 0, "mesh firstGeometry is zero");
    expect(readU32(bytes, 212) == 1, "mesh geometryCount is one");
    for (std::size_t offset = 216; offset < 224; ++offset) {
        expect(bytes[offset] == 0, "inter-chunk alignment padding is zero");
    }

    expect(readU32(bytes, 256) == 1, "materialCount is one");
    expect(readU32(bytes, 260) == 0, "texture string arena is empty");
    expect(readU32(bytes, 264) == 0 && readU32(bytes, 268) == 0,
        "material-header reserved fields are zero");
    expect(readU32(bytes, 272) == 0x3f800000u, "base color R is exact 1.0f");
    expect(readU32(bytes, 288) == 0x3f000000u, "alpha cutoff is exact 0.5f");
    expect(
        readU32(bytes, 292) == xrphoton::ogfx::NoTextureReference,
        "texture reference uses the pinned UINT32_MAX sentinel");
    expect(readU32(bytes, 296) == 0 && readU32(bytes, 300) == 0,
        "material-record reserved fields are zero");

    expect(readU32(bytes, 336) == 0xbf000000u, "position stream begins at its pinned offset");
    expect(readU32(bytes, 424) == 0x3f800000u, "first normal Z is exact 1.0f");
    constexpr std::array<std::uint32_t, 6> expectedIndices{0, 1, 2, 0, 2, 3};
    for (std::size_t index = 0; index < expectedIndices.size(); ++index) {
        expect(
            readU32(bytes, 528 + index * 4) == expectedIndices[index],
            "index stream is explicit little-endian u32 data");
    }
    expect(528 + expectedIndices.size() * 4 == bytes.size(), "writer emits no trailing padding");
}

void testSupportedSchemaBreadth()
{
    const Model model = makeTwoGeometryModel();

    const SerializeResult result = xrphoton::ogfx::serializeModel(model, "two-meshes.ogfx");
    expect(static_cast<bool>(result), "the core schema supports multiple meshes and geometries");
    if (result) {
        expect(readU64(result.bytes, 112) == 96, "two geometry records occupy 96 bytes");
        expect(readU32(result.bytes, 196) == 1, "the second geometry emits alpha-tested bit 0");
    } else {
        std::cerr << result.error << '\n';
    }
}

void testSphereEnclosureRounding()
{
    const float tiny = std::ldexp(1.0f, -26);
    Model model{};
    model.positions = {
        { 1.0f,  tiny, 0.0f},
        {-1.0f, -tiny, 0.0f},
        { 0.0f,  0.0f, 0.0f},
    };
    model.attributes.assign(3, VertexAttributes{0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
    model.indices = {0, 1, 2};
    model.geometries.push_back(Geometry{0, 3, 0, 3, 0, false});
    model.meshes.push_back(Mesh{0, 1});
    model.materials.emplace_back();

    const SerializeResult result = xrphoton::ogfx::serializeModel(model, "rounding.ogfx");
    expect(static_cast<bool>(result), "the adversarial sphere-rounding model serializes");
    if (!result) {
        std::cerr << result.error << '\n';
        return;
    }

    // In f64, the farthest squared distance is 1 + 2^-52, but sqrt rounds that
    // value to exactly 1.0. The writer must still advance the stored f32 radius.
    expect(readU32(result.bytes, 92) == 0x3f800001u,
        "sphere radius advances when f64 sqrt hides a nonzero contribution");
    const double radius = readF32(result.bytes, 92);
    const double radiusSquared = radius * radius;
    for (const Position& position : model.positions) {
        const double distanceSquared = static_cast<double>(position.x) * position.x
            + static_cast<double>(position.y) * position.y
            + static_cast<double>(position.z) * position.z;
        expect(radiusSquared >= distanceSquared,
            "serialized sphere passes the decoder's f64 enclosure test");
    }
}

void testAsymmetricBoundsAndMaterialFraming()
{
    Model model{};
    model.positions = {
        {1.0f, -2.0f,  3.0f},
        {5.0f,  6.0f, -1.0f},
        {2.0f,  3.0f,  4.0f},
    };
    model.attributes.assign(3, VertexAttributes{0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
    model.indices = {0, 1, 2};
    model.geometries.push_back(Geometry{0, 3, 0, 3, 1, false});
    model.meshes.push_back(Mesh{0, 1});
    model.materials.emplace_back();
    model.materials.push_back(Material{
        .baseColorFactor = {0.25f, 0.5f, 0.75f, 1.0f},
        .alphaCutoff = 0.25f,
        .baseColorTexture = {},
    });

    const SerializeResult result = xrphoton::ogfx::serializeModel(model, "asymmetric.ogfx");
    expect(static_cast<bool>(result), "asymmetric bounds and two materials serialize");
    if (!result) {
        std::cerr << result.error << '\n';
        return;
    }

    expect(readF32(result.bytes, 56) == 1.0f, "asymmetric model minimum X is exact");
    expect(readF32(result.bytes, 60) == -2.0f, "asymmetric model minimum Y is exact");
    expect(readF32(result.bytes, 64) == -1.0f, "asymmetric model minimum Z is exact");
    expect(readF32(result.bytes, 68) == 5.0f, "asymmetric model maximum X is exact");
    expect(readF32(result.bytes, 72) == 6.0f, "asymmetric model maximum Y is exact");
    expect(readF32(result.bytes, 76) == 4.0f, "asymmetric model maximum Z is exact");
    expect(readF32(result.bytes, 80) == 3.0f, "sphere center X uses the AABB midpoint");
    expect(readF32(result.bytes, 84) == 2.0f, "sphere center Y uses the AABB midpoint");
    expect(readF32(result.bytes, 88) == 1.5f, "sphere center Z uses the AABB midpoint");

    const double centerX = readF32(result.bytes, 80);
    const double centerY = readF32(result.bytes, 84);
    const double centerZ = readF32(result.bytes, 88);
    const double radius = readF32(result.bytes, 92);
    for (const Position& position : model.positions) {
        const double dx = static_cast<double>(position.x) - centerX;
        const double dy = static_cast<double>(position.y) - centerY;
        const double dz = static_cast<double>(position.z) - centerZ;
        expect(radius * radius >= dx * dx + dy * dy + dz * dz,
            "asymmetric model sphere encloses every position in f64");
    }

    expect(readU64(result.bytes, 240) == 80, "two-material framed payload is exactly 80 bytes");
    expect(readU32(result.bytes, 256) == 2, "framed payload records two materials");
    expect(readU32(result.bytes, 260) == 0, "two-material payload still has no string arena");
    expect(readU32(result.bytes, 304) == 0x3e800000u, "second material R is exact 0.25f");
    expect(readU32(result.bytes, 308) == 0x3f000000u, "second material G is exact 0.5f");
    expect(readU32(result.bytes, 312) == 0x3f400000u, "second material B is exact 0.75f");
    expect(readU32(result.bytes, 320) == 0x3e800000u, "second alpha cutoff is exact 0.25f");
    expect(readU32(result.bytes, 324) == xrphoton::ogfx::NoTextureReference,
        "each material gets the no-texture sentinel");
}

void testValidation()
{
    {
        Model model = makeQuad();
        model.positions.clear();
        expectRejected(std::move(model), "element count");
    }
    {
        Model model = makeQuad();
        model.attributes.clear();
        expectRejected(std::move(model), "element count");
    }
    {
        Model model = makeQuad();
        model.indices.clear();
        expectRejected(std::move(model), "element count");
    }
    {
        Model model = makeQuad();
        model.geometries.clear();
        expectRejected(std::move(model), "record count");
    }
    {
        Model model = makeQuad();
        model.meshes.clear();
        expectRejected(std::move(model), "record count");
    }
    {
        Model model = makeQuad();
        model.materials.clear();
        expectRejected(std::move(model), "materialCount");
    }
    {
        Model model = makeQuad();
        model.attributes.pop_back();
        expectRejected(std::move(model), "element count");
    }
    {
        Model model = makeQuad();
        model.positions[0].x = std::numeric_limits<float>::quiet_NaN();
        expectRejected(std::move(model), "positions[0].x/y/z");
    }
    {
        Model model = makeQuad();
        model.attributes[0].u = std::numeric_limits<float>::infinity();
        expectRejected(std::move(model), "attributes[0].nx/ny/nz/u/v");
    }
    {
        Model model = makeQuad();
        model.attributes[0].nx = 0.0f;
        model.attributes[0].ny = 0.0f;
        model.attributes[0].nz = 0.0f;
        expectRejected(std::move(model), "normal length squared");
    }
    {
        Model model = makeQuad();
        model.materials[0].alphaCutoff = std::numeric_limits<float>::quiet_NaN();
        expectRejected(std::move(model), "alphaCutoff");
    }
    {
        Model model = makeQuad();
        model.materials[0].baseColorFactor[2] = std::numeric_limits<float>::infinity();
        expectRejected(std::move(model), "baseColorFactor");
    }
    {
        Model model = makeQuad();
        model.geometries[0].firstVertex = 1;
        expectRejected(std::move(model), "firstVertex");
    }
    {
        Model model = makeQuad();
        model.geometries[0].indexCount = 5;
        expectRejected(std::move(model), "indexCount");
    }
    {
        Model model = makeQuad();
        model.geometries[0].firstIndex = 1;
        expectRejected(std::move(model), "firstIndex");
    }
    {
        Model model = makeQuad();
        model.geometries[0].vertexCount = 5;
        expectRejected(std::move(model), "vertex range end");
    }
    {
        Model model = makeQuad();
        model.indices[5] = 4;
        expectRejected(std::move(model), "geometry-local value");
    }
    {
        Model model = makeQuad();
        model.geometries[0].materialIndex = 1;
        expectRejected(std::move(model), "materialIndex");
    }
    {
        Model model = makeQuad();
        model.meshes[0].firstGeometry = 1;
        expectRejected(std::move(model), "firstGeometry");
    }
    {
        Model model = makeQuad();
        model.meshes[0].geometryCount = 0;
        expectRejected(std::move(model), "geometryCount");
    }
    {
        Model model = makeQuad();
        model.meshes[0].geometryCount = 2;
        expectRejected(std::move(model), "geometry range end");
    }
    {
        Model model = makeTwoGeometryModel();
        model.meshes.pop_back();
        expectRejected(std::move(model), "final geometry partition end");
    }
    {
        Model model = makeQuad();
        model.materials[0].baseColorTexture = "ston/example";
        expectRejected(std::move(model), "baseColorTexture");
    }
    {
        Model model = makeQuad();
        model.positions.push_back({1.0f, 1.0f, 0.0f});
        model.attributes.push_back({0.0f, 0.0f, 1.0f, 0.0f, 0.0f});
        expectRejected(std::move(model), "final vertex partition end");
    }
    {
        Model model = makeQuad();
        model.indices.push_back(0);
        expectRejected(std::move(model), "final index partition end");
    }
    {
        Model model = makeQuad();
        const float maximum = std::numeric_limits<float>::max();
        model.positions = {
            { maximum,  maximum,  maximum},
            {-maximum, -maximum, -maximum},
            {0.0f, 0.0f, 0.0f},
        };
        model.attributes.resize(3);
        model.indices = {0, 1, 2};
        model.geometries[0].vertexCount = 3;
        model.geometries[0].indexCount = 3;
        expectRejected(std::move(model), "sphereRadius");
    }
}
}

int main()
{
    testGoldenQuad();
    testSupportedSchemaBreadth();
    testSphereEnclosureRounding();
    testAsymmetricBoundsAndMaterialFraming();
    testValidation();

    if (failureCount != 0) {
        std::cerr << failureCount << " OGFx core test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "OGFx core tests passed.\n";
    return 0;
}
