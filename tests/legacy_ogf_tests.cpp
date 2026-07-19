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
using xrphoton::legacy_ogf::decodeModel;
using xrphoton::ogfx::DecodeResult;
using xrphoton::ogfx::Model;
using xrphoton::ogfx::SerializeResult;

constexpr std::uint32_t HeaderChunkId = 0x1;
constexpr std::uint32_t TextureChunkId = 0x2;
constexpr std::uint32_t VerticesChunkId = 0x3;
constexpr std::uint32_t IndicesChunkId = 0x4;
constexpr std::uint32_t ChildrenChunkId = 0x9;
constexpr std::uint32_t BoneNamesChunkId = 0xD;
constexpr std::uint32_t IkDataChunkId = 0x10;
constexpr std::uint32_t DescriptionChunkId = 0x12;
constexpr std::uint32_t BoneShapeBytes = 112;
constexpr std::uint32_t JointDataBytes = 76;
constexpr std::string_view RigidTextureName = "mtl\\mtl_barrel_01";
constexpr std::string_view RigidPhysicsMaterial = "objects\\barrel";

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

void appendString(std::vector<std::uint8_t>* bytes, std::string_view value)
{
    bytes->insert(bytes->end(), value.begin(), value.end());
    bytes->push_back(0);
}

std::size_t directChunkPayloadOffset(
    const std::vector<std::uint8_t>& bytes,
    std::uint32_t id)
{
    std::size_t offset = 0;
    while (offset + 8 <= bytes.size()) {
        const std::uint32_t chunkId = readU32(bytes, offset);
        const std::uint32_t size = readU32(bytes, offset + 4);
        if (chunkId == id) {
            return offset + 8;
        }
        offset += 8 + size;
    }
    return bytes.size();
}

RawChunk makeRigidHeader(std::uint8_t modelType)
{
    RawChunk header{.id = HeaderChunkId, .payload = {}};
    header.payload.push_back(xrphoton::legacy_ogf::SupportedVersion);
    header.payload.push_back(modelType);
    appendU16(&header.payload, xrphoton::legacy_ogf::SupportedShaderId);
    appendPosition(&header.payload, -2.0f, -1.0f, 0.0f);
    appendPosition(&header.payload, 3.0f, 4.0f, 2.0f);
    appendPosition(&header.payload, 0.5f, 1.5f, 1.0f);
    appendF32(&header.payload, 4.0f);
    return header;
}

struct SyntheticRigidFixture
{
    std::vector<RawChunk> chunks;
    std::array<std::size_t, 2> shapeOffsets{};
    std::array<std::size_t, 2> jointOffsets{};
    std::array<std::size_t, 2> bindRotationOffsets{};
    std::size_t childIdOffset = 0;
    std::size_t childSizeOffset = 4;
    std::size_t vertexBoneOffset = 0;
    std::size_t indexValueOffset = 0;
};

SyntheticRigidFixture makeSyntheticRigidFixture(
    std::string_view rootParent = {},
    std::string_view childParent = "root")
{
    SyntheticRigidFixture fixture{};

    RawChunk description{.id = DescriptionChunkId, .payload = {}};
    appendString(&description.payload, "synthetic\\rigid_barrel.object");
    appendString(&description.payload, "unit-test-builder");
    appendU32(&description.payload, 0x10203040u);
    appendString(&description.payload, "fixture-author");
    appendU32(&description.payload, 0x50607080u);
    appendString(&description.payload, "fixture-modifier");
    appendU32(&description.payload, 0x90a0b0c0u);

    RawChunk boneNames{.id = BoneNamesChunkId, .payload = {}};
    appendU32(&boneNames.payload, 2);
    appendString(&boneNames.payload, "root");
    appendString(&boneNames.payload, rootParent);
    for (std::size_t component = 0; component < 15; ++component) {
        appendF32(&boneNames.payload, 0.0f);
    }
    appendString(&boneNames.payload, "rim0");
    appendString(&boneNames.payload, childParent);
    for (std::size_t component = 0; component < 15; ++component) {
        appendF32(&boneNames.payload, component == 12 ? 0.25f : 0.0f);
    }

    RawChunk ikData{.id = IkDataChunkId, .payload = {}};
    struct BoneProbe
    {
        std::array<float, 3> cylinderCenter;
        std::array<float, 3> cylinderAxis;
        float height;
        float radius;
        std::array<float, 3> bindTranslation;
        float mass;
        std::array<float, 3> centerOfMass;
    };
    constexpr std::array probes{
        BoneProbe{
            .cylinderCenter = {0.5f, 0.25f, -0.5f},
            .cylinderAxis = {0.0f, 1.0f, 0.0f},
            .height = 2.0f,
            .radius = 0.4f,
            .bindTranslation = {1.0f, 2.0f, 3.0f},
            .mass = 3.0f,
            .centerOfMass = {0.25f, 0.5f, 0.75f},
        },
        BoneProbe{
            .cylinderCenter = {-0.5f, 1.0f, 0.5f},
            .cylinderAxis = {0.0f, 0.6f, 0.8f},
            .height = 0.5f,
            .radius = 0.75f,
            .bindTranslation = {-1.0f, 4.0f, 2.0f},
            .mass = 1.0f,
            .centerOfMass = {1.0f, -2.0f, 3.0f},
        },
    };
    for (std::size_t boneIndex = 0; boneIndex < probes.size(); ++boneIndex) {
        const BoneProbe& probe = probes[boneIndex];
        appendU32(&ikData.payload, 1);
        appendString(&ikData.payload, RigidPhysicsMaterial);

        fixture.shapeOffsets[boneIndex] = ikData.payload.size();
        std::vector<std::uint8_t> shape(BoneShapeBytes, 0);
        writeU16(&shape, 0, 3);
        writeU16(&shape, 2, 0);
        writeF32(&shape, 80, probe.cylinderCenter[0]);
        writeF32(&shape, 84, probe.cylinderCenter[1]);
        writeF32(&shape, 88, probe.cylinderCenter[2]);
        writeF32(&shape, 92, probe.cylinderAxis[0]);
        writeF32(&shape, 96, probe.cylinderAxis[1]);
        writeF32(&shape, 100, probe.cylinderAxis[2]);
        writeF32(&shape, 104, probe.height);
        writeF32(&shape, 108, probe.radius);
        ikData.payload.insert(ikData.payload.end(), shape.begin(), shape.end());

        fixture.jointOffsets[boneIndex] = ikData.payload.size();
        std::vector<std::uint8_t> joint(JointDataBytes, 0);
        writeU32(&joint, 0, 0);
        writeU32(&joint, 60, 0);
        writeF32(&joint, 72, 0.125f);
        ikData.payload.insert(ikData.payload.end(), joint.begin(), joint.end());

        fixture.bindRotationOffsets[boneIndex] = ikData.payload.size();
        appendPosition(&ikData.payload, 0.0f, 0.0f, 0.0f);
        appendPosition(
            &ikData.payload,
            probe.bindTranslation[0],
            probe.bindTranslation[1],
            probe.bindTranslation[2]);
        appendF32(&ikData.payload, probe.mass);
        appendPosition(
            &ikData.payload,
            probe.centerOfMass[0],
            probe.centerOfMass[1],
            probe.centerOfMass[2]);
    }

    RawChunk childTexture{.id = TextureChunkId, .payload = {}};
    appendString(&childTexture.payload, RigidTextureName);
    appendString(&childTexture.payload, "models\\model");

    RawChunk childVertices{.id = VerticesChunkId, .payload = {}};
    appendU32(
        &childVertices.payload,
        xrphoton::legacy_ogf::SupportedRigidVertexFormat);
    appendU32(&childVertices.payload, 3);
    constexpr float normalY = -0.3713906705379486f;
    constexpr float normalZ = 0.9284766912460327f;
    const std::array<std::array<float, 14>, 3> vertices{{
        {-2.0f, -1.0f, 0.0f, 0.0f, normalY, normalZ,
            1.0f, 0.0f, 0.0f, 0.0f, normalZ, -normalY, 0.125f, 0.25f},
        {3.0f, -1.0f, 0.0f, 0.0f, normalY, normalZ,
            1.0f, 0.0f, 0.0f, 0.0f, normalZ, -normalY, 0.75f, 0.5f},
        {-2.0f, 4.0f, 2.0f, 0.0f, normalY, normalZ,
            1.0f, 0.0f, 0.0f, 0.0f, normalZ, -normalY, 0.375f, 0.875f},
    }};
    constexpr std::array<std::uint32_t, 3> boneIndices{0, 1, 1};
    for (std::size_t vertexIndex = 0; vertexIndex < vertices.size(); ++vertexIndex) {
        for (float value : vertices[vertexIndex]) {
            appendF32(&childVertices.payload, value);
        }
        appendU32(&childVertices.payload, boneIndices[vertexIndex]);
    }

    RawChunk childIndices{.id = IndicesChunkId, .payload = {}};
    appendU32(&childIndices.payload, 3);
    appendU16(&childIndices.payload, 0);
    appendU16(&childIndices.payload, 1);
    appendU16(&childIndices.payload, 2);

    const std::vector<RawChunk> childChunks{
        makeRigidHeader(xrphoton::legacy_ogf::SupportedRigidChildModelType),
        std::move(childTexture),
        std::move(childVertices),
        std::move(childIndices),
    };
    const std::vector<std::uint8_t> childStream = assemble(childChunks);
    const std::size_t vertexPayloadOffset =
        directChunkPayloadOffset(childStream, VerticesChunkId);
    const std::size_t indexPayloadOffset =
        directChunkPayloadOffset(childStream, IndicesChunkId);

    RawChunk children{.id = ChildrenChunkId, .payload = {}};
    appendU32(&children.payload, 0);
    appendU32(
        &children.payload,
        static_cast<std::uint32_t>(childStream.size()));
    children.payload.insert(
        children.payload.end(), childStream.begin(), childStream.end());
    fixture.vertexBoneOffset = 8 + vertexPayloadOffset + 8 + 56;
    fixture.indexValueOffset = 8 + indexPayloadOffset + 4;

    fixture.chunks = {
        makeRigidHeader(xrphoton::legacy_ogf::SupportedRigidModelType),
        std::move(description),
        std::move(children),
        std::move(boneNames),
        std::move(ikData),
    };
    return fixture;
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
        && model.materials.empty()
        && model.physicsBodies.empty()
        && model.physicsColliders.empty();
}

bool positionsEqual(
    const xrphoton::ogfx::Position& left,
    const xrphoton::ogfx::Position& right)
{
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

bool modelsEqual(const Model& left, const Model& right)
{
    if (left.positions.size() != right.positions.size()
        || left.attributes.size() != right.attributes.size()
        || left.indices != right.indices
        || left.geometries.size() != right.geometries.size()
        || left.meshes.size() != right.meshes.size()
        || left.materials.size() != right.materials.size()
        || left.physicsBodies.size() != right.physicsBodies.size()
        || left.physicsColliders.size() != right.physicsColliders.size()) {
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
    for (std::size_t index = 0; index < left.physicsBodies.size(); ++index) {
        const auto& a = left.physicsBodies[index];
        const auto& b = right.physicsBodies[index];
        if (a.firstCollider != b.firstCollider
            || a.colliderCount != b.colliderCount
            || a.mass != b.mass
            || !positionsEqual(a.centerOfMass, b.centerOfMass)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.physicsColliders.size(); ++index) {
        const auto& a = left.physicsColliders[index];
        const auto& b = right.physicsColliders[index];
        if (a.shapeType != b.shapeType
            || a.flags != b.flags
            || a.material != b.material
            || a.sourceNode != b.sourceNode
            || !positionsEqual(a.center, b.center)
            || !positionsEqual(a.axis, b.axis)
            || a.height != b.height
            || a.radius != b.radius
            || a.mass != b.mass
            || !positionsEqual(a.centerOfMass, b.centerOfMass)) {
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

void expectRigidRejected(
    const std::vector<std::uint8_t>& bytes,
    std::string_view expectedChunk,
    std::string_view expectedField)
{
    const DecodeResult result = decodeModel(bytes, "invalid-rigid.ogf");
    expect(!result, "unsupported or malformed rigid legacy OGF is rejected");
    expect(modelIsEmpty(result.model),
        "rigid legacy rejection exposes no partially decoded model or physics");
    expect(result.error.find("invalid-rigid.ogf") != std::string::npos,
        "rigid rejection names its input");
    expect(result.error.find("legacy OGF rigid decoder") != std::string::npos,
        "rigid rejection names its adapter boundary");
    expect(result.error.find(expectedChunk) != std::string::npos,
        std::string("rigid rejection names chunk: ") + std::string(expectedChunk));
    expect(result.error.find(expectedField) != std::string::npos,
        std::string("rigid rejection names field: ") + std::string(expectedField));
    expect(result.error.find(": expected ") != std::string::npos
            && result.error.find(", found ") != std::string::npos,
        "rigid rejection states expected and found values");
}

void testAcceptedRigidProfile()
{
    const SyntheticRigidFixture fixture = makeSyntheticRigidFixture();
    const DecodeResult decoded =
        decodeModel(assemble(fixture.chunks), "synthetic-rigid.ogf");
    expect(static_cast<bool>(decoded),
        "the pinned synthetic v4 rigid-compound profile decodes");
    if (!decoded) {
        std::cerr << decoded.error << '\n';
        return;
    }

    const Model& model = decoded.model;
    expect(model.positions.size() == 3 && model.attributes.size() == 3,
        "rigid child vertices flatten into ordinary OGFx render streams");
    expect(positionsEqual(model.positions[0], {-2.0f, -1.0f, 0.0f})
            && positionsEqual(model.positions[1], {3.0f, -1.0f, 0.0f})
            && positionsEqual(model.positions[2], {-2.0f, 4.0f, 2.0f}),
        "bind-model-space rigid positions pass through without bone translation");
    expect(model.attributes[0].ny < 0.0f
            && model.attributes[0].nz > 0.0f
            && model.attributes[0].u == 0.125f
            && model.attributes[0].v == 0.25f,
        "rigid normals and UVs pass through unchanged");
    expect(model.indices == std::vector<std::uint32_t>({0, 1, 2}),
        "rigid child indices widen without reordering");
    expect(model.geometries.size() == 1
            && model.geometries[0].firstVertex == 0
            && model.geometries[0].vertexCount == 3
            && model.geometries[0].firstIndex == 0
            && model.geometries[0].indexCount == 3
            && model.geometries[0].materialIndex == 0
            && !model.geometries[0].alphaTested,
        "the rigid child becomes one ordinary opaque geometry");
    expect(model.meshes.size() == 1
            && model.meshes[0].firstGeometry == 0
            && model.meshes[0].geometryCount == 1,
        "the flattened rigid model owns its child geometry through one mesh");
    expect(model.materials.size() == 1
            && model.materials[0].baseColorTexture == RigidTextureName,
        "the explicit models/model mapping preserves the barrel texture reference");

    expect(model.physicsBodies.size() == 1
            && model.physicsBodies[0].firstCollider == 0
            && model.physicsBodies[0].colliderCount == 2
            && model.physicsBodies[0].mass == 4.0f
            && positionsEqual(
                model.physicsBodies[0].centerOfMass,
                {1.1875f, 2.875f, 4.8125f}),
        "bone masses and global centers produce the pinned compound body mass/COM");
    expect(model.physicsColliders.size() == 2,
        "each rigid source bone becomes one engine-neutral collider record");
    if (model.physicsColliders.size() == 2) {
        const auto& root = model.physicsColliders[0];
        expect(root.shapeType == xrphoton::ogfx::PhysicsShapeType::Cylinder
                && root.flags == 0
                && root.material == RigidPhysicsMaterial
                && root.sourceNode == "root"
                && positionsEqual(root.center, {1.5f, 2.25f, 2.5f})
                && positionsEqual(root.axis, {0.0f, 1.0f, 0.0f})
                && root.height == 2.0f
                && root.radius == 0.4f
                && root.mass == 3.0f
                && positionsEqual(root.centerOfMass, {1.25f, 2.5f, 3.75f}),
            "the root cylinder and mass center receive the root bind translation");

        const auto& rim = model.physicsColliders[1];
        expect(rim.shapeType == xrphoton::ogfx::PhysicsShapeType::Cylinder
                && rim.flags == 0
                && rim.material == RigidPhysicsMaterial
                && rim.sourceNode == "rim0"
                && positionsEqual(rim.center, {-0.5f, 7.0f, 5.5f})
                && positionsEqual(rim.axis, {0.0f, 0.6f, 0.8f})
                && rim.height == 0.5f
                && rim.radius == 0.75f
                && rim.mass == 1.0f
                && positionsEqual(rim.centerOfMass, {1.0f, 4.0f, 8.0f}),
            "the child cylinder uses its accumulated parent/child bind translation");
    }

    const SerializeResult first =
        xrphoton::ogfx::serializeModel(model, "synthetic-rigid-first.ogfx");
    const SerializeResult second =
        xrphoton::ogfx::serializeModel(model, "synthetic-rigid-second.ogfx");
    expect(static_cast<bool>(first) && static_cast<bool>(second),
        "the canonical writer accepts flattened render data plus compound metadata");
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
        "repeated rigid adaptation produces deterministic OGFx bytes");

    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(first.bytes, "synthetic-rigid-schema.ogfx");
    expect(static_cast<bool>(schema) && modelsEqual(schema.model, model),
        "schema decoding reconstructs the complete render and rigid-physics model");
    if (schema) {
        const SerializeResult roundTrip = xrphoton::ogfx::serializeModel(
            schema.model, "synthetic-rigid-round-trip.ogfx");
        expect(static_cast<bool>(roundTrip) && roundTrip.bytes == first.bytes,
            "rigid writer-schema-writer round trip is byte exact");
    } else {
        std::cerr << schema.error << '\n';
    }
    const DecodeResult runtime =
        xrphoton::ogfx::decodeModel(first.bytes, "synthetic-rigid-runtime.ogfx");
    expect(static_cast<bool>(runtime) && modelsEqual(runtime.model, model),
        "runtime decoding retains render data and engine-neutral physics metadata");

    const std::vector<std::uint8_t> staticSource = assemble(makeSyntheticChunks());
    const DecodeResult directStatic =
        decodeStaticModel(staticSource, "dispatch-static-direct.ogf");
    const DecodeResult dispatchedStatic =
        decodeModel(staticSource, "dispatch-static.ogf");
    expect(static_cast<bool>(directStatic)
            && static_cast<bool>(dispatchedStatic)
            && modelsEqual(directStatic.model, dispatchedStatic.model),
        "the generalized OGF entry point preserves the existing static profile");
}

void testRigidProfileRejections()
{
    SyntheticRigidFixture fixture = makeSyntheticRigidFixture({}, "missing");
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0xd", "bones[1].parentName");

    fixture = makeSyntheticRigidFixture({}, {});
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0xd", "root bone count");

    fixture = makeSyntheticRigidFixture();
    writeU16(
        &chunkById(&fixture.chunks, IkDataChunkId).payload,
        fixture.shapeOffsets[0],
        2);
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0x10", "bones[0].shapeType");

    fixture = makeSyntheticRigidFixture();
    writeU32(
        &chunkById(&fixture.chunks, IkDataChunkId).payload,
        fixture.jointOffsets[1],
        1);
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0x10", "bones[1].jointType");

    fixture = makeSyntheticRigidFixture();
    writeU32(
        &chunkById(&fixture.chunks, IkDataChunkId).payload,
        fixture.jointOffsets[0] + 60,
        1);
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0x10", "bones[0].jointFlags");

    fixture = makeSyntheticRigidFixture();
    writeF32(
        &chunkById(&fixture.chunks, IkDataChunkId).payload,
        fixture.bindRotationOffsets[1],
        0.25f);
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0x10", "bones[1].bindRotation");

    fixture = makeSyntheticRigidFixture();
    writeF32(
        &chunkById(&fixture.chunks, IkDataChunkId).payload,
        fixture.shapeOffsets[0] + 80,
        std::numeric_limits<float>::max());
    writeF32(
        &chunkById(&fixture.chunks, IkDataChunkId).payload,
        fixture.bindRotationOffsets[0] + 12,
        std::numeric_limits<float>::max());
    expectRigidRejected(
        assemble(fixture.chunks),
        "chunk 0x10",
        "bones[0].model-space collider/centerOfMass");

    fixture = makeSyntheticRigidFixture();
    writeU32(
        &chunkById(&fixture.chunks, ChildrenChunkId).payload,
        fixture.childIdOffset,
        1);
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0x9", "child id");

    fixture = makeSyntheticRigidFixture();
    writeU32(
        &chunkById(&fixture.chunks, ChildrenChunkId).payload,
        fixture.childSizeOffset,
        std::numeric_limits<std::uint32_t>::max());
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0x9", "child payload byte range");

    fixture = makeSyntheticRigidFixture();
    writeU32(
        &chunkById(&fixture.chunks, ChildrenChunkId).payload,
        fixture.vertexBoneOffset,
        2);
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0x3", "vertices[0].boneIndex");

    fixture = makeSyntheticRigidFixture();
    writeU16(
        &chunkById(&fixture.chunks, ChildrenChunkId).payload,
        fixture.indexValueOffset,
        3);
    expectRigidRejected(
        assemble(fixture.chunks), "chunk 0x4", "indices[0].value");
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
    expect(static_cast<bool>(runtime)
            && runtime.model.materials[0].baseColorTexture
                == "ston\\synthetic_asymmetric",
        "runtime decoding accepts and reconstructs converted texture references");

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

bool positionsNear(
    const xrphoton::ogfx::Position& left,
    const xrphoton::ogfx::Position& right,
    float tolerance = 1.0e-6f)
{
    return std::fabs(left.x - right.x) <= tolerance
        && std::fabs(left.y - right.y) <= tolerance
        && std::fabs(left.z - right.z) <= tolerance;
}

void expectPinnedRigidBarrelModel(const Model& model)
{
    expect(model.positions.size() == 436 && model.attributes.size() == 436,
        "the regular barrel contains the pinned 436 complete render vertices");
    expect(model.indices.size() == 1158,
        "the regular barrel contains the pinned 1158 widened indices");
    expect(model.geometries.size() == 1
            && model.geometries[0].firstVertex == 0
            && model.geometries[0].vertexCount == 436
            && model.geometries[0].firstIndex == 0
            && model.geometries[0].indexCount == 1158
            && model.geometries[0].materialIndex == 0
            && !model.geometries[0].alphaTested,
        "the regular barrel flattens to one complete opaque geometry");
    expect(model.meshes.size() == 1
            && model.meshes[0].firstGeometry == 0
            && model.meshes[0].geometryCount == 1,
        "the regular barrel flattens to one render mesh");
    expect(model.materials.size() == 1
            && model.materials[0].baseColorTexture == RigidTextureName,
        "the regular barrel preserves the mtl_barrel_01 logical texture");

    if (model.positions.size() == 436) {
        xrphoton::ogfx::Position minimum = model.positions.front();
        xrphoton::ogfx::Position maximum = minimum;
        for (const auto& position : model.positions) {
            minimum.x = std::min(minimum.x, position.x);
            minimum.y = std::min(minimum.y, position.y);
            minimum.z = std::min(minimum.z, position.z);
            maximum.x = std::max(maximum.x, position.x);
            maximum.y = std::max(maximum.y, position.y);
            maximum.z = std::max(maximum.z, position.z);
        }
        expect(positionsNear(minimum, {-0.370839f, 0.001489f, -0.372322f})
                && positionsNear(maximum, {0.370551f, 1.090266f, 0.369067f}),
            "the flattened barrel render vertices retain the pinned model-space bounds");
    }

    expect(model.physicsBodies.size() == 1
            && model.physicsBodies[0].firstCollider == 0
            && model.physicsBodies[0].colliderCount == 3
            && model.physicsBodies[0].mass == 62.0f
            && positionsNear(
                model.physicsBodies[0].centerOfMass,
                {1.35486705e-5f, 0.539355881f, -0.00148954768f}),
        "the regular barrel preserves one 62-unit compound body and aggregate COM");
    expect(model.physicsColliders.size() == 3,
        "the regular barrel preserves its three authored cylinder colliders");
    if (model.physicsColliders.size() != 3) {
        return;
    }

    const auto& barrel = model.physicsColliders[0];
    expect(barrel.shapeType == xrphoton::ogfx::PhysicsShapeType::Cylinder
            && barrel.flags == 0
            && barrel.material == RigidPhysicsMaterial
            && barrel.sourceNode == "barrel"
            && positionsNear(
                barrel.center,
                {0.000362378545f, 0.538193166f, -0.00171245355f})
            && positionsEqual(barrel.axis, {0.0f, 1.0f, 0.0f})
            && std::fabs(barrel.height - 1.074635386f) <= 1.0e-6f
            && std::fabs(barrel.radius - 0.352066427f) <= 1.0e-6f
            && barrel.mass == 60.0f
            && positionsNear(
                barrel.centerOfMass,
                {0.0000187968835f, 0.538891077f, -0.00148494681f}),
        "the main barrel cylinder is flattened into model space exactly once");

    const auto& lowerRim = model.physicsColliders[1];
    expect(lowerRim.shapeType == xrphoton::ogfx::PhysicsShapeType::Cylinder
            && lowerRim.flags == 0
            && lowerRim.material == RigidPhysicsMaterial
            && lowerRim.sourceNode == "obod_1"
            && positionsNear(
                lowerRim.center,
                {-0.000143597252f, 0.343390855f, -0.00162860146f})
            && positionsEqual(lowerRim.axis, {0.0f, 1.0f, 0.0f})
            && std::fabs(lowerRim.height - 0.0556095019f) <= 1.0e-6f
            && std::fabs(lowerRim.radius - 0.371128023f) <= 1.0e-6f
            && lowerRim.mass == 1.0f
            && positionsNear(
                lowerRim.centerOfMass,
                {-0.000144170597f, 0.342964927f, -0.00162728876f}),
        "the lower rim collider receives its hierarchical bind translation");

    const auto& upperRim = model.physicsColliders[2];
    expect(upperRim.shapeType == xrphoton::ogfx::PhysicsShapeType::Cylinder
            && upperRim.flags == 0
            && upperRim.material == RigidPhysicsMaterial
            && upperRim.sourceNode == "obod_2"
            && positionsNear(
                upperRim.center,
                {-0.000143688521f, 0.763634990f, -0.00162752182f})
            && positionsNear(
                upperRim.axis,
                {-0.000211764025f, 0.999999702f, 0.00087506551f})
            && std::fabs(upperRim.height - 0.0551285669f) <= 1.0e-6f
            && std::fabs(upperRim.radius - 0.371128947f) <= 1.0e-6f
            && upperRim.mass == 1.0f
            && positionsNear(
                upperRim.centerOfMass,
                {-0.000143624842f, 0.763635085f, -0.00162785873f}),
        "the upper rim collider receives both parent bind translations");
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
    expect(static_cast<bool>(runtime)
            && runtime.model.materials[0].baseColorTexture
                == "ston\\ston_stena_marbl_m_03_back",
        "the proof output is accepted by the runtime texture profile");
}

void verifyRigidCorpusOutput(
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

    const DecodeResult source = decodeModel(sourceBytes, sourcePath.string());
    expect(static_cast<bool>(source),
        "the pinned regular-barrel source decodes through rigid OGF dispatch");
    if (!source) {
        std::cerr << source.error << '\n';
        return;
    }
    expectPinnedRigidBarrelModel(source.model);
    expect(outputBytes.size() == 19352,
        "the persisted regular-barrel output has the pinned canonical byte size");

    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(outputBytes, outputPath.string());
    expect(static_cast<bool>(schema),
        "the persisted regular-barrel output passes complete OGFx schema decoding");
    if (!schema) {
        std::cerr << schema.error << '\n';
        return;
    }
    expect(modelsEqual(schema.model, source.model),
        "persisted OGFx reconstructs the barrel render and physics metadata exactly");

    const SerializeResult canonical =
        xrphoton::ogfx::serializeModel(source.model, "rigid-corpus-proof.ogfx");
    expect(static_cast<bool>(canonical) && canonical.bytes == outputBytes,
        "the regular-barrel output is byte-exact canonical writer output");
    const SerializeResult roundTrip = xrphoton::ogfx::serializeModel(
        schema.model, "rigid-corpus-round-trip.ogfx");
    expect(static_cast<bool>(roundTrip) && roundTrip.bytes == outputBytes,
        "the regular-barrel writer-schema-writer round trip is byte exact");

    const DecodeResult runtime =
        xrphoton::ogfx::decodeModel(outputBytes, outputPath.string());
    expect(static_cast<bool>(runtime) && modelsEqual(runtime.model, source.model),
        "runtime decoding accepts the barrel texture and optional physics profile");
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
    if (argumentCount == 4
        && std::string_view(arguments[1]) == "--verify-rigid-corpus-output") {
        verifyRigidCorpusOutput(arguments[2], arguments[3]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " rigid OGF offline corpus proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "Rigid OGF offline corpus proof passed.\n";
        return 0;
    }

    testAcceptedProfile();
    testFramingAndProfileRejections();
    testHeaderAndBoundsRejections();
    testTextureRejections();
    testGeometryRejections();
    testAcceptedRigidProfile();
    testRigidProfileRejections();

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
