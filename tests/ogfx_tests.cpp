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
using xrphoton::ogfx::PhysicsBody;
using xrphoton::ogfx::PhysicsCollider;
using xrphoton::ogfx::PhysicsShapeType;
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

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    expect(offset + 2 <= bytes.size(), "u16 read stays inside the serialized file");
    if (offset + 2 > bytes.size()) {
        return 0;
    }

    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
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

Model makeAxisAlignedBox(
    Position minimum,
    Position maximum,
    const std::array<float, 4>& baseColorFactor)
{
    Model model{};
    model.positions = {
        {minimum.x, minimum.y, minimum.z},
        {minimum.x, minimum.y, maximum.z},
        {minimum.x, maximum.y, maximum.z},
        {minimum.x, maximum.y, minimum.z},

        {maximum.x, minimum.y, maximum.z},
        {maximum.x, minimum.y, minimum.z},
        {maximum.x, maximum.y, minimum.z},
        {maximum.x, maximum.y, maximum.z},

        {minimum.x, minimum.y, maximum.z},
        {minimum.x, minimum.y, minimum.z},
        {maximum.x, minimum.y, minimum.z},
        {maximum.x, minimum.y, maximum.z},

        {minimum.x, maximum.y, minimum.z},
        {minimum.x, maximum.y, maximum.z},
        {maximum.x, maximum.y, maximum.z},
        {maximum.x, maximum.y, minimum.z},

        {maximum.x, minimum.y, minimum.z},
        {minimum.x, minimum.y, minimum.z},
        {minimum.x, maximum.y, minimum.z},
        {maximum.x, maximum.y, minimum.z},

        {minimum.x, minimum.y, maximum.z},
        {maximum.x, minimum.y, maximum.z},
        {maximum.x, maximum.y, maximum.z},
        {minimum.x, maximum.y, maximum.z},
    };

    constexpr std::array<std::array<float, 3>, 6> FaceNormals{{
        {-1.0f,  0.0f,  0.0f},
        { 1.0f,  0.0f,  0.0f},
        { 0.0f, -1.0f,  0.0f},
        { 0.0f,  1.0f,  0.0f},
        { 0.0f,  0.0f, -1.0f},
        { 0.0f,  0.0f,  1.0f},
    }};
    constexpr std::array<std::array<float, 2>, 4> FaceUvs{{
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
    }};
    model.attributes.reserve(24);
    model.indices.reserve(36);
    for (std::uint32_t face = 0; face < FaceNormals.size(); ++face) {
        for (const std::array<float, 2>& uv : FaceUvs) {
            model.attributes.push_back({
                FaceNormals[face][0],
                FaceNormals[face][1],
                FaceNormals[face][2],
                uv[0],
                uv[1],
            });
        }
        const std::uint32_t firstVertex = face * 4;
        model.indices.insert(model.indices.end(), {
            firstVertex,
            firstVertex + 1,
            firstVertex + 2,
            firstVertex,
            firstVertex + 2,
            firstVertex + 3,
        });
    }
    model.geometries.push_back(Geometry{0, 24, 0, 36, 0, false});
    model.meshes.push_back(Mesh{0, 1});
    model.materials.emplace_back();
    model.materials[0].baseColorFactor = baseColorFactor;
    return model;
}

Model makeTestYardGround()
{
    return makeAxisAlignedBox(
        {-10.0f, -0.4f, -10.0f},
        { 10.0f,  0.0f,  10.0f},
        {0.42f, 0.42f, 0.45f, 1.0f});
}

Model makeTestYardWall()
{
    return makeAxisAlignedBox(
        {-4.0f, 0.0f, -0.15f},
        { 4.0f, 3.0f,  0.15f},
        {0.55f, 0.24f, 0.18f, 1.0f});
}

Model makeTestYardBox()
{
    return makeAxisAlignedBox(
        {-0.5f, -0.5f, -0.5f},
        { 0.5f,  0.5f,  0.5f},
        {0.80f, 0.62f, 0.22f, 1.0f});
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

Model makeRigidPhysicsQuad()
{
    Model model = makeQuad();
    model.physicsBodies.push_back(PhysicsBody{
        .firstCollider = 0,
        .colliderCount = 3,
        .mass = 62.0f,
        .centerOfMass = {0.0f, 0.125f, 0.0f},
    });
    model.physicsColliders = {
        PhysicsCollider{
            .shapeType = PhysicsShapeType::Cylinder,
            .flags = 0,
            .material = "objects\\barrel",
            .sourceNode = "barrel",
            .center = {0.0f, 0.5f, 0.0f},
            .axis = {0.0f, 1.0f, 0.0f},
            .height = 1.0746f,
            .radius = 0.3521f,
            .mass = 60.0f,
            .centerOfMass = {0.0f, 0.5f, 0.0f},
        },
        PhysicsCollider{
            .shapeType = PhysicsShapeType::Cylinder,
            .flags = 0,
            .material = "objects\\barrel",
            .sourceNode = "obod_1",
            .center = {0.0f, 1.0f, 0.0f},
            .axis = {0.0f, 1.0f, 0.0f},
            .height = 0.0556f,
            .radius = 0.3711f,
            .mass = 1.0f,
            .centerOfMass = {0.0f, 1.0f, 0.0f},
        },
        PhysicsCollider{
            .shapeType = PhysicsShapeType::Cylinder,
            .flags = 0,
            .material = "objects\\barrel",
            .sourceNode = "obod_2",
            .center = {0.0f, 0.0f, 0.0f},
            .axis = {0.0f, -1.0f, 0.0f},
            .height = 0.0551f,
            .radius = 0.3711f,
            .mass = 1.0f,
            .centerOfMass = {0.0f, 0.0f, 0.0f},
        },
    };
    return model;
}

Model makeBoxPhysicsQuad()
{
    Model model = makeQuad();
    model.physicsBodies.push_back(PhysicsBody{
        .firstCollider = 0,
        .colliderCount = 1,
        .mass = 10.0f,
        .centerOfMass = {-0.03f, 0.03f, 0.01f},
    });
    model.physicsColliders.push_back(PhysicsCollider{
        .shapeType = PhysicsShapeType::Box,
        .flags = 0,
        .material = "objects\\dead_body",
        .sourceNode = "link",
        .center = {-0.03f, 0.04f, 0.03f},
        .orientation = {0.5f, 0.5f, 0.5f, 0.5f},
        .halfExtents = {0.04f, 0.03f, 0.23f},
        .mass = 10.0f,
        .centerOfMass = {-0.03f, 0.03f, 0.01f},
    });
    return model;
}

std::size_t chunkHeaderOffset(
    const std::vector<std::uint8_t>& bytes,
    ChunkId id)
{
    std::size_t offset = xrphoton::ogfx::FileHeaderSize;
    while (offset + xrphoton::ogfx::ChunkHeaderSize <= bytes.size()) {
        if (readU32(bytes, offset) == static_cast<std::uint32_t>(id)) {
            return offset;
        }
        offset += xrphoton::ogfx::ChunkHeaderSize
            + static_cast<std::size_t>(readU64(bytes, offset + 16));
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

void testYardAssetSerialization()
{
    struct Fixture
    {
        std::string_view name;
        Model model;
        std::size_t expectedByteSize;
    };
    const std::array fixtures{
        Fixture{"yard ground", makeTestYardGround(), 1312},
        Fixture{"yard wall", makeTestYardWall(), 1312},
        Fixture{"yard box", makeTestYardBox(), 1312},
    };

    for (const Fixture& fixture : fixtures) {
        const SerializeResult first = xrphoton::ogfx::serializeModel(
            fixture.model,
            std::string(fixture.name) + "-first.ogfx");
        const SerializeResult second = xrphoton::ogfx::serializeModel(
            fixture.model,
            std::string(fixture.name) + "-second.ogfx");
        expect(static_cast<bool>(first),
            std::string(fixture.name) + " serializes");
        expect(static_cast<bool>(second),
            std::string(fixture.name) + " serializes under another diagnostic name");
        if (!first || !second) {
            if (!first.error.empty()) {
                std::cerr << first.error << '\n';
            }
            if (!second.error.empty()) {
                std::cerr << second.error << '\n';
            }
            continue;
        }
        expect(first.bytes == second.bytes,
            std::string(fixture.name)
                + " serialization is deterministic and ignores diagnostic names");
        expect(first.bytes.size() == fixture.expectedByteSize,
            std::string(fixture.name) + " has its pinned canonical byte size");
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

void testTextureStringArena()
{
    const std::string firstTexture = "textures/plitka";
    const std::string utf8Texture = "textures/caf\xc3\xa9";
    Model model = makeQuad();
    model.materials[0].baseColorTexture = firstTexture;
    model.materials.emplace_back();
    model.materials.emplace_back();
    model.materials.back().baseColorTexture = firstTexture;
    model.materials.emplace_back();
    model.materials.back().baseColorTexture = utf8Texture;

    const SerializeResult first =
        xrphoton::ogfx::serializeModel(model, "textured-first.ogfx");
    const SerializeResult second =
        xrphoton::ogfx::serializeModel(model, "textured-second.ogfx");
    expect(static_cast<bool>(first), "logical texture references serialize");
    expect(static_cast<bool>(second), "logical texture references serialize again");
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
        "textured serialization is deterministic and ignores diagnostic names");
    const std::vector<std::uint8_t>& bytes = first.bytes;
    const std::uint32_t secondOffset =
        static_cast<std::uint32_t>(2 + firstTexture.size());
    const std::uint32_t arenaBytes = secondOffset
        + static_cast<std::uint32_t>(2 + utf8Texture.size());

    expect(readU64(bytes, 240) == 16 + 4 * 32 + arenaBytes,
        "material payload includes four records and the interned string arena");
    expect(readU32(bytes, 256) == 4, "textured payload records four materials");
    expect(readU32(bytes, 260) == arenaBytes,
        "textured payload records the exact string arena size");
    expect(readU32(bytes, 292) == 0,
        "first texture reference points to the first arena entry");
    expect(readU32(bytes, 324) == xrphoton::ogfx::NoTextureReference,
        "an empty texture between references keeps the no-texture sentinel");
    expect(readU32(bytes, 356) == 0,
        "duplicate texture references reuse the first arena entry");
    expect(readU32(bytes, 388) == secondOffset,
        "second unique texture points past the first arena entry");

    constexpr std::size_t arenaOffset = 400;
    expect(readU16(bytes, arenaOffset) == firstTexture.size(),
        "first arena entry has an explicit little-endian byte length");
    expect(std::equal(
            firstTexture.begin(),
            firstTexture.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(arenaOffset + 2),
            [](char left, std::uint8_t right) {
                return static_cast<std::uint8_t>(left) == right;
            }),
        "first arena entry preserves its UTF-8 bytes");
    const std::size_t secondEntry = arenaOffset + secondOffset;
    expect(readU16(bytes, secondEntry) == utf8Texture.size(),
        "second arena entry counts UTF-8 bytes rather than code points");
    expect(std::equal(
            utf8Texture.begin(),
            utf8Texture.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(secondEntry + 2),
            [](char left, std::uint8_t right) {
                return static_cast<std::uint8_t>(left) == right;
            }),
        "second arena entry preserves multibyte UTF-8");

    Model maximumLength = makeQuad();
    maximumLength.materials[0].baseColorTexture.assign(
        xrphoton::ogfx::MaximumStringBytes,
        'a');
    const SerializeResult maximum =
        xrphoton::ogfx::serializeModel(maximumLength, "maximum-texture.ogfx");
    expect(static_cast<bool>(maximum),
        "a texture reference at the 4096-byte schema limit serializes");
    if (maximum) {
        expect(readU32(maximum.bytes, 260) == 2 + xrphoton::ogfx::MaximumStringBytes,
            "the maximum-length entry contributes its prefix and all text bytes");
        expect(readU32(maximum.bytes, 292) == 0,
            "the maximum-length reference points to the arena start");
        expect(readU16(maximum.bytes, 304) == xrphoton::ogfx::MaximumStringBytes,
            "the maximum length fits the pinned u16 entry prefix");
        const xrphoton::ogfx::DecodeResult decoded =
            xrphoton::ogfx::decodeModelSchema(maximum.bytes, "maximum-schema.ogfx");
        expect(static_cast<bool>(decoded),
            "the schema decoder accepts a texture at the 4096-byte limit");
        if (decoded) {
            expect(
                decoded.model.materials[0].baseColorTexture
                        == maximumLength.materials[0].baseColorTexture,
                "the schema decoder preserves every maximum-length texture byte");
        } else {
            std::cerr << decoded.error << '\n';
        }
    } else {
        std::cerr << maximum.error << '\n';
    }
}

void testRigidPhysicsChunk()
{
    const Model model = makeRigidPhysicsQuad();
    const SerializeResult first =
        xrphoton::ogfx::serializeModel(model, "physics-first.ogfx");
    const SerializeResult second =
        xrphoton::ogfx::serializeModel(model, "physics-second.ogfx");
    expect(static_cast<bool>(first), "a rigid body with three cylinders serializes");
    expect(static_cast<bool>(second), "the rigid-physics model serializes again");
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
        "rigid-physics serialization is deterministic and ignores diagnostic names");

    const std::vector<std::uint8_t>& bytes = first.bytes;
    const std::size_t header = chunkHeaderOffset(bytes, ChunkId::RigidPhysics);
    expect(header == 560,
        "the optional physics header follows alignment after the unchanged static core");
    expect(readU32(bytes, header + 4) == 0,
        "the rigid-physics chunk is optional for older render-only readers");
    expect(readU32(bytes, header + 8) == xrphoton::ogfx::ChunkVersion,
        "the rigid-physics chunk uses version 1");

    constexpr std::uint32_t MaterialEntryBytes = 2 + 14;
    constexpr std::uint32_t BarrelEntryBytes = 2 + 6;
    constexpr std::uint32_t HoopEntryBytes = 2 + 6;
    constexpr std::uint32_t StringBytes =
        MaterialEntryBytes + BarrelEntryBytes + 2 * HoopEntryBytes;
    constexpr std::uint64_t PayloadBytes = xrphoton::ogfx::RigidPhysicsHeaderSize
        + xrphoton::ogfx::PhysicsBodyRecordSize
        + 3 * xrphoton::ogfx::PhysicsColliderRecordSize
        + StringBytes;
    expect(readU64(bytes, header + 16) == PayloadBytes,
        "the physics payload frames its body, colliders, and string arena exactly");

    const std::size_t payload = header + xrphoton::ogfx::ChunkHeaderSize;
    expect(readU32(bytes, payload) == 1, "physics header records one body");
    expect(readU32(bytes, payload + 4) == 3,
        "physics header records three colliders");
    expect(readU32(bytes, payload + 8) == StringBytes,
        "physics header records the exact interned string byte count");
    for (std::size_t offset = payload + 12; offset < payload + 32; offset += 4) {
        expect(readU32(bytes, offset) == 0, "physics-header reserved words are zero");
    }

    const std::size_t body = payload + xrphoton::ogfx::RigidPhysicsHeaderSize;
    expect(readU32(bytes, body) == 0 && readU32(bytes, body + 4) == 3,
        "the body owns the complete contiguous collider range");
    expect(readU32(bytes, body + 8) == 0 && readU32(bytes, body + 12) == 0,
        "physics-body reserved words are zero");
    expect(readF32(bytes, body + 16) == 62.0f,
        "the body preserves its aggregate mass");
    expect(readF32(bytes, body + 24) == 0.125f,
        "the body preserves its center of mass");

    const std::size_t colliders = body + xrphoton::ogfx::PhysicsBodyRecordSize;
    expect(readU32(bytes, colliders) == 1,
        "the first collider records the cylinder shape type");
    expect(readU32(bytes, colliders + 8) == 0,
        "first-use material text starts the physics string arena");
    expect(readU32(bytes, colliders + 12) == MaterialEntryBytes,
        "the first source-node string follows the material entry");
    expect(readU32(bytes, colliders + 64 + 8) == 0,
        "a repeated physics material reuses its first arena entry");
    expect(readU32(bytes, colliders + 64 + 12)
            == MaterialEntryBytes + BarrelEntryBytes,
        "the second source node follows first-use order");
    expect(readU32(bytes, colliders + 128 + 12)
            == MaterialEntryBytes + BarrelEntryBytes + HoopEntryBytes,
        "the third source node follows first-use order");
    expect(readF32(bytes, colliders + 40) == 1.0746f
            && readF32(bytes, colliders + 44) == 0.3521f
            && readF32(bytes, colliders + 48) == 60.0f,
        "the first cylinder preserves height, radius, and source mass");
    expect(bytes.size() == header + xrphoton::ogfx::ChunkHeaderSize + PayloadBytes,
        "the optional final chunk has no trailing padding");

    const xrphoton::ogfx::DecodeResult decoded =
        xrphoton::ogfx::decodeModelSchema(bytes, "physics-schema.ogfx");
    expect(static_cast<bool>(decoded), "the schema decoder accepts writer physics output");
    if (decoded) {
        expect(decoded.model.physicsBodies.size() == 1
                && decoded.model.physicsBodies[0].mass == 62.0f,
            "schema decoding reconstructs the rigid body");
        expect(decoded.model.physicsColliders.size() == 3
                && decoded.model.physicsColliders[0].material == "objects\\barrel"
                && decoded.model.physicsColliders[1].sourceNode == "obod_1"
                && decoded.model.physicsColliders[2].flags == 0,
            "schema decoding reconstructs collider fields and strings");
        const SerializeResult roundTrip = xrphoton::ogfx::serializeModel(
            decoded.model, "physics-round-trip.ogfx");
        expect(static_cast<bool>(roundTrip) && roundTrip.bytes == bytes,
            "writer-decoder-writer preserves every rigid-physics byte");
    } else {
        std::cerr << decoded.error << '\n';
    }
}

void testBoxPhysicsChunkVersion2()
{
    const Model model = makeBoxPhysicsQuad();
    const SerializeResult first =
        xrphoton::ogfx::serializeModel(model, "box-physics-first.ogfx");
    const SerializeResult second =
        xrphoton::ogfx::serializeModel(model, "box-physics-second.ogfx");
    expect(static_cast<bool>(first) && static_cast<bool>(second),
        "an oriented box rigid body serializes");
    if (!first || !second) {
        std::cerr << (!first ? first.error : second.error) << '\n';
        return;
    }
    expect(first.bytes == second.bytes,
        "version-2 box physics serialization is deterministic");

    constexpr std::uint32_t MaterialEntryBytes = 2 + 17;
    constexpr std::uint32_t NodeEntryBytes = 2 + 4;
    constexpr std::uint32_t StringBytes = MaterialEntryBytes + NodeEntryBytes;
    constexpr std::uint64_t PayloadBytes = xrphoton::ogfx::RigidPhysicsHeaderSize
        + xrphoton::ogfx::PhysicsBodyRecordSize
        + xrphoton::ogfx::PhysicsColliderRecordSizeV2
        + StringBytes;
    const std::size_t header = chunkHeaderOffset(first.bytes, ChunkId::RigidPhysics);
    expect(readU32(first.bytes, header + 8)
            == xrphoton::ogfx::RigidPhysicsChunkVersion2,
        "a box selects rigid-physics chunk version 2 without changing core chunks");
    expect(readU64(first.bytes, header + 16) == PayloadBytes,
        "version-2 physics frames the 80-byte box record exactly");

    const std::size_t collider = header + xrphoton::ogfx::ChunkHeaderSize
        + xrphoton::ogfx::RigidPhysicsHeaderSize
        + xrphoton::ogfx::PhysicsBodyRecordSize;
    expect(readU32(first.bytes, collider) == 2,
        "the version-2 collider records the box shape type");
    expect(readU32(first.bytes, collider + 8) == 0
            && readU32(first.bytes, collider + 12) == MaterialEntryBytes,
        "box collider strings use canonical first-use arena offsets");
    expect(readF32(first.bytes, collider + 28) == 0.5f
            && readF32(first.bytes, collider + 40) == 0.5f,
        "the box record preserves its orientation quaternion");
    expect(readF32(first.bytes, collider + 44) == 0.04f
            && readF32(first.bytes, collider + 52) == 0.23f,
        "the box record preserves all half extents");
    expect(readF32(first.bytes, collider + 56) == 10.0f,
        "the version-2 record preserves collider mass at its pinned offset");
    expect(readU32(first.bytes, collider + 72) == 0
            && readU32(first.bytes, collider + 76) == 0,
        "version-2 collider reserved words are zero");

    const xrphoton::ogfx::DecodeResult decoded =
        xrphoton::ogfx::decodeModelSchema(first.bytes, "box-physics-schema.ogfx");
    expect(static_cast<bool>(decoded)
            && decoded.model.physicsColliders.size() == 1
            && decoded.model.physicsColliders[0].shapeType
                == PhysicsShapeType::Box
            && decoded.model.physicsColliders[0].orientation.x == 0.5f
            && decoded.model.physicsColliders[0].halfExtents.z == 0.23f,
        "the schema decoder reconstructs version-2 box physics");
    if (decoded) {
        const SerializeResult roundTrip = xrphoton::ogfx::serializeModel(
            decoded.model, "box-physics-round-trip.ogfx");
        expect(static_cast<bool>(roundTrip) && roundTrip.bytes == first.bytes,
            "writer-decoder-writer preserves every version-2 box byte");
    }
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
        model.materials[0].baseColorTexture.assign(
            xrphoton::ogfx::MaximumStringBytes + 1,
            'a');
        expectRejected(std::move(model), "baseColorTexture");
    }
    {
        Model model = makeQuad();
        model.materials[0].baseColorTexture =
            std::string{"\xc0\xaf", 2};
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
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders.clear();
        expectRejected(std::move(model), "body/collider presence");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsBodies.clear();
        expectRejected(std::move(model), "body/collider presence");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsBodies[0].colliderCount = 0;
        expectRejected(std::move(model), "colliderCount");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsBodies[0].firstCollider = 1;
        expectRejected(std::move(model), "firstCollider");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsBodies[0].colliderCount = 2;
        expectRejected(std::move(model), "final collider partition end");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsBodies[0].mass = 0.0f;
        expectRejected(std::move(model), "bodies[0].mass");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsBodies[0].centerOfMass.x =
            std::numeric_limits<float>::quiet_NaN();
        expectRejected(std::move(model), "bodies[0].centerOfMass");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].shapeType =
            static_cast<PhysicsShapeType>(99);
        expectRejected(std::move(model), "colliders[0].shapeType");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].flags = 1;
        expectRejected(std::move(model), "colliders[0].flags");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].center.z =
            std::numeric_limits<float>::infinity();
        expectRejected(std::move(model), "colliders[0].center");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].axis = {};
        expectRejected(std::move(model), "axis length squared");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].height = 0.0f;
        expectRejected(std::move(model), "colliders[0].height");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].radius =
            std::numeric_limits<float>::quiet_NaN();
        expectRejected(std::move(model), "colliders[0].radius");
    }
    {
        Model model = makeBoxPhysicsQuad();
        model.physicsColliders[0].orientation.w = 0.25f;
        expectRejected(std::move(model), "orientation length squared");
    }
    {
        Model model = makeBoxPhysicsQuad();
        model.physicsColliders[0].orientation.x =
            std::numeric_limits<float>::quiet_NaN();
        expectRejected(std::move(model), "orientation length squared");
    }
    {
        Model model = makeBoxPhysicsQuad();
        model.physicsColliders[0].halfExtents.y = 0.0f;
        expectRejected(std::move(model), "halfExtents.y");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].mass = -1.0f;
        expectRejected(std::move(model), "colliders[0].mass");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].centerOfMass.y =
            std::numeric_limits<float>::quiet_NaN();
        expectRejected(std::move(model), "colliders[0].centerOfMass");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].material.assign(
            xrphoton::ogfx::MaximumStringBytes + 1, 'a');
        expectRejected(std::move(model), "colliders[0].material");
    }
    {
        Model model = makeRigidPhysicsQuad();
        model.physicsColliders[0].sourceNode = std::string{"\xc0\xaf", 2};
        expectRejected(std::move(model), "colliders[0].sourceNode");
    }
}
}

int main()
{
    testGoldenQuad();
    testYardAssetSerialization();
    testSupportedSchemaBreadth();
    testSphereEnclosureRounding();
    testAsymmetricBoundsAndMaterialFraming();
    testTextureStringArena();
    testRigidPhysicsChunk();
    testBoxPhysicsChunkVersion2();
    testValidation();

    if (failureCount != 0) {
        std::cerr << failureCount << " OGFx core test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "OGFx core tests passed.\n";
    return 0;
}
