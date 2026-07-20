#include "blender_mesh.hpp"
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
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using xrphoton::blender_mesh::decodeStaticMesh;
using xrphoton::ogfx::DecodeResult;
using xrphoton::ogfx::Model;
using xrphoton::ogfx::SerializeResult;

constexpr std::size_t VersionOffset = 4;
constexpr std::size_t HeaderSizeOffset = 8;
constexpr std::size_t FlagsOffset = 12;
constexpr std::size_t TriangleCountOffset = 16;
constexpr std::size_t BlenderMajorOffset = 20;
constexpr std::size_t BlenderMinorOffset = 24;
constexpr std::size_t UnitScaleOffset = 32;
constexpr std::size_t FirstReservedOffset = 36;
constexpr std::size_t MatrixOffset = 40;
constexpr std::size_t SecondReservedOffset = 88;
constexpr std::size_t ThirdReservedOffset = 92;
constexpr std::size_t MaterialFlagsOffset = 96;
constexpr std::size_t MaterialAlphaCutoffOffset = 100;
constexpr std::size_t TextureReferenceByteCountOffset = 104;
constexpr std::size_t MaterialReservedOffset = 108;
constexpr std::size_t V2TextureReferenceOffset = 112;
constexpr std::size_t FirstCornerOffset = 96;
constexpr std::size_t PositionOffsetInCorner = 0;
constexpr std::size_t NormalOffsetInCorner = 12;
constexpr std::size_t UvOffsetInCorner = 24;

struct Corner
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    float u = 0.0f;
    float v = 0.0f;
};

using Triangle = std::array<Corner, 3>;

constexpr std::array<float, 12> IdentityTransform{
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
};

int failureCount = 0;

void expect(bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failureCount;
    }
}

bool near(float actual, float expected, float tolerance = 1.0e-6f)
{
    return std::abs(actual - expected) <= tolerance;
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

void writeU32(
    std::vector<std::uint8_t>* bytes,
    std::size_t offset,
    std::uint32_t value)
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

std::vector<std::uint8_t> makeStream(
    const std::vector<Triangle>& triangles,
    std::uint32_t flags = xrphoton::blender_mesh::StreamFlagHasUvs,
    const std::array<float, 12>& transform = IdentityTransform,
    float unitScale = 1.0f)
{
    std::vector<std::uint8_t> bytes;
    const std::size_t expectedSize = xrphoton::blender_mesh::StreamHeaderSizeV1
        + triangles.size()
            * xrphoton::blender_mesh::CornersPerTriangle
            * xrphoton::blender_mesh::CornerRecordSize;
    bytes.reserve(expectedSize);
    bytes.insert(
        bytes.end(),
        xrphoton::blender_mesh::StreamMagic.begin(),
        xrphoton::blender_mesh::StreamMagic.end());
    appendU32(&bytes, xrphoton::blender_mesh::StreamVersion1);
    appendU32(&bytes, xrphoton::blender_mesh::StreamHeaderSizeV1);
    appendU32(&bytes, flags);
    appendU32(&bytes, static_cast<std::uint32_t>(triangles.size()));
    appendU32(&bytes, 5);
    appendU32(&bytes, 1);
    appendU32(&bytes, 2);
    appendF32(&bytes, unitScale);
    appendU32(&bytes, 0);
    for (float value : transform) {
        appendF32(&bytes, value);
    }
    appendU32(&bytes, 0);
    appendU32(&bytes, 0);

    for (const Triangle& triangle : triangles) {
        for (const Corner& corner : triangle) {
            for (float value : corner.position) {
                appendF32(&bytes, value);
            }
            for (float value : corner.normal) {
                appendF32(&bytes, value);
            }
            appendF32(&bytes, corner.u);
            appendF32(&bytes, corner.v);
        }
    }
    expect(bytes.size() == expectedSize,
        "synthetic XRBM builder emits its declared byte size");
    return bytes;
}

std::vector<std::uint8_t> makeTexturedStream(
    const std::vector<Triangle>& triangles,
    std::string_view textureReference = "trees\\trees_new_vetka_green",
    float alphaCutoff = 0.5f,
    std::uint32_t materialFlags =
        xrphoton::blender_mesh::MaterialFlagAlphaTested,
    std::uint32_t streamFlags = xrphoton::blender_mesh::StreamFlagHasUvs)
{
    std::vector<std::uint8_t> bytes;
    const std::size_t expectedSize =
        xrphoton::blender_mesh::StreamHeaderSizeV2
        + textureReference.size()
        + triangles.size()
            * xrphoton::blender_mesh::CornersPerTriangle
            * xrphoton::blender_mesh::CornerRecordSize;
    bytes.reserve(expectedSize);
    bytes.insert(
        bytes.end(),
        xrphoton::blender_mesh::StreamMagic.begin(),
        xrphoton::blender_mesh::StreamMagic.end());
    appendU32(&bytes, xrphoton::blender_mesh::StreamVersion2);
    appendU32(&bytes, xrphoton::blender_mesh::StreamHeaderSizeV2);
    appendU32(&bytes, streamFlags);
    appendU32(&bytes, static_cast<std::uint32_t>(triangles.size()));
    appendU32(&bytes, 5);
    appendU32(&bytes, 1);
    appendU32(&bytes, 2);
    appendF32(&bytes, 1.0f);
    appendU32(&bytes, 0);
    for (float value : IdentityTransform) {
        appendF32(&bytes, value);
    }
    appendU32(&bytes, 0);
    appendU32(&bytes, 0);
    appendU32(&bytes, materialFlags);
    appendF32(&bytes, alphaCutoff);
    appendU32(&bytes, static_cast<std::uint32_t>(textureReference.size()));
    appendU32(&bytes, 0);
    bytes.insert(bytes.end(), textureReference.begin(), textureReference.end());

    for (const Triangle& triangle : triangles) {
        for (const Corner& corner : triangle) {
            for (float value : corner.position) {
                appendF32(&bytes, value);
            }
            for (float value : corner.normal) {
                appendF32(&bytes, value);
            }
            appendF32(&bytes, corner.u);
            appendF32(&bytes, corner.v);
        }
    }
    expect(bytes.size() == expectedSize,
        "synthetic textured XRBM builder emits its declared byte size");
    return bytes;
}

Triangle makeAsymmetricTriangle()
{
    // cross(p1 - p0, p2 - p0) = (0, -16, 32). Both Y and Z are
    // deliberately nonzero so an axis swap cannot pass by symmetry.
    constexpr float inverseSqrt5 = 0.4472135901451111f;
    return {{
        Corner{{1.0f, -2.0f, 3.0f}, {0.0f, -inverseSqrt5, 2.0f * inverseSqrt5},
            0.125f, 0.25f},
        Corner{{5.0f, -2.0f, 3.0f}, {0.0f, -inverseSqrt5, 2.0f * inverseSqrt5},
            0.75f, 0.5f},
        Corner{{1.0f, 6.0f, 7.0f}, {0.0f, -inverseSqrt5, 2.0f * inverseSqrt5},
            0.375f, 0.875f},
    }};
}

std::vector<std::uint8_t> makeAsymmetricStream()
{
    return makeStream({makeAsymmetricTriangle()});
}

std::vector<Triangle> makeQuadTriangles()
{
    constexpr std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
    const Corner p0{{0.0f, 0.0f, 0.0f}, normal, 0.0f, 0.0f};
    const Corner p1{{1.0f, 0.0f, 0.0f}, normal, 1.0f, 0.0f};
    const Corner p2{{1.0f, 1.0f, 0.0f}, normal, 1.0f, 1.0f};
    const Corner p3{{0.0f, 1.0f, 0.0f}, normal, 0.0f, 1.0f};
    return {
        Triangle{p0, p1, p2},
        Triangle{p0, p2, p3},
    };
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
            || a.materialIndex != b.materialIndex
            || a.alphaTested != b.alphaTested) {
            return false;
        }
    }
    for (std::size_t index = 0; index < left.meshes.size(); ++index) {
        if (left.meshes[index].firstGeometry != right.meshes[index].firstGeometry
            || left.meshes[index].geometryCount
                != right.meshes[index].geometryCount) {
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

void expectOneOpaqueGeometry(const Model& model, std::size_t indexCount)
{
    expect(model.positions.size() == model.attributes.size(),
        "Blender positions and attributes remain parallel streams");
    expect(model.indices.size() == indexCount,
        "Blender model contains the expected number of triangle indices");
    expect(model.geometries.size() == 1
            && model.geometries[0].firstVertex == 0
            && model.geometries[0].vertexCount == model.positions.size()
            && model.geometries[0].firstIndex == 0
            && model.geometries[0].indexCount == indexCount
            && model.geometries[0].materialIndex == 0
            && !model.geometries[0].alphaTested,
        "Blender mesh maps to one complete opaque geometry");
    expect(model.meshes.size() == 1
            && model.meshes[0].firstGeometry == 0
            && model.meshes[0].geometryCount == 1,
        "Blender extraction maps to one reusable mesh");
    expect(model.materials.size() == 1
            && model.materials[0].baseColorFactor
                == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
            && model.materials[0].alphaCutoff == 0.5f
            && model.materials[0].baseColorTexture.empty(),
        "material-free Blender input receives one canonical opaque material");
}

void expectOneAlphaTestedGeometry(
    const Model& model,
    std::size_t indexCount,
    std::string_view textureReference,
    float alphaCutoff)
{
    expect(model.positions.size() == model.attributes.size(),
        "textured Blender positions and attributes remain parallel streams");
    expect(model.indices.size() == indexCount,
        "textured Blender model contains the expected triangle indices");
    expect(model.geometries.size() == 1
            && model.geometries[0].firstVertex == 0
            && model.geometries[0].vertexCount == model.positions.size()
            && model.geometries[0].firstIndex == 0
            && model.geometries[0].indexCount == indexCount
            && model.geometries[0].materialIndex == 0
            && model.geometries[0].alphaTested,
        "textured Blender mesh maps to one complete alpha-tested geometry");
    expect(model.meshes.size() == 1
            && model.meshes[0].firstGeometry == 0
            && model.meshes[0].geometryCount == 1,
        "textured Blender extraction maps to one reusable mesh");
    expect(model.materials.size() == 1
            && model.materials[0].baseColorFactor
                == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
            && model.materials[0].alphaCutoff == alphaCutoff
            && model.materials[0].baseColorTexture == textureReference,
        "textured Blender input retains its logical DDS reference and cutoff");
}

void expectOneOpaqueTexturedGeometry(
    const Model& model,
    std::size_t indexCount,
    std::string_view textureReference)
{
    expect(model.positions.size() == model.attributes.size(),
        "opaque-textured Blender positions and attributes remain parallel streams");
    expect(model.indices.size() == indexCount,
        "opaque-textured Blender model contains the expected triangle indices");
    expect(model.geometries.size() == 1
            && model.geometries[0].firstVertex == 0
            && model.geometries[0].vertexCount == model.positions.size()
            && model.geometries[0].firstIndex == 0
            && model.geometries[0].indexCount == indexCount
            && model.geometries[0].materialIndex == 0
            && !model.geometries[0].alphaTested,
        "textured Blender mesh maps to one complete opaque geometry");
    expect(model.meshes.size() == 1
            && model.meshes[0].firstGeometry == 0
            && model.meshes[0].geometryCount == 1,
        "opaque-textured Blender extraction maps to one reusable mesh");
    expect(model.materials.size() == 1
            && model.materials[0].baseColorFactor
                == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
            && model.materials[0].alphaCutoff == 0.5f
            && model.materials[0].baseColorTexture == textureReference,
        "opaque-textured Blender input retains its logical DDS reference");
}

void expectRejected(
    const std::vector<std::uint8_t>& bytes,
    std::string_view expectedField)
{
    constexpr std::string_view diagnosticName =
        "bad-source.blend::object bad_mesh";
    const DecodeResult result = decodeStaticMesh(bytes, diagnosticName);
    expect(!result, "invalid XRBM stream is rejected");
    expect(modelIsEmpty(result.model),
        "XRBM rejection returns no partially populated model");
    expect(result.error.find(diagnosticName) != std::string::npos,
        "XRBM rejection preserves the source diagnostic name");
    expect(result.error.find("Blender mesh adapter: XRBM field")
            != std::string::npos,
        "XRBM rejection identifies the offline Blender adapter and field");
    expect(result.error.find(expectedField) != std::string::npos,
        std::string("XRBM rejection names field: ") + std::string(expectedField));
    expect(result.error.find(": expected ") != std::string::npos
            && result.error.find(", found ") != std::string::npos,
        "XRBM rejection follows the field/expected/found diagnostic contract");
}

void testAsymmetricAxisMapAndCcwWinding()
{
    const DecodeResult decoded =
        decodeStaticMesh(makeAsymmetricStream(), "asymmetric.blend::probe");
    expect(static_cast<bool>(decoded),
        "asymmetric Blender-space triangle decodes");
    if (!decoded) {
        std::cerr << decoded.error << '\n';
        return;
    }

    const Model& model = decoded.model;
    expectOneOpaqueGeometry(model, 3);
    expect(model.positions.size() == 3, "one triangle has three unique corners");
    if (model.positions.size() != 3 || model.attributes.size() != 3) {
        return;
    }

    // Identity object transform still reverses source corners 1 and 2 because
    // the engine's (x,z,y) conversion has negative determinant.
    expect(model.positions[0].x == 1.0f
            && model.positions[0].y == 3.0f
            && model.positions[0].z == -2.0f,
        "first position maps exactly from Blender (x,y,z) to engine (x,z,y)");
    expect(model.positions[1].x == 1.0f
            && model.positions[1].y == 7.0f
            && model.positions[1].z == 6.0f,
        "second emitted position is source corner 2 after CCW reversal");
    expect(model.positions[2].x == 5.0f
            && model.positions[2].y == 3.0f
            && model.positions[2].z == -2.0f,
        "third emitted position is source corner 1 after CCW reversal");
    expect(model.indices == std::vector<std::uint32_t>{0, 1, 2},
        "reversed corners produce a canonical local CCW index stream");

    constexpr float inverseSqrt5 = 0.4472135901451111f;
    for (const auto& attributes : model.attributes) {
        expect(near(attributes.nx, 0.0f)
                && near(attributes.ny, 2.0f * inverseSqrt5)
                && near(attributes.nz, -inverseSqrt5),
            "normal axes map and renormalize consistently with positions");
    }
    expect(model.attributes[0].u == 0.125f
            && model.attributes[0].v == 0.25f
            && model.attributes[1].u == 0.375f
            && model.attributes[1].v == 0.875f
            && model.attributes[2].u == 0.75f
            && model.attributes[2].v == 0.5f,
        "UV values remain unflipped while corner order reverses");

    const auto& p0 = model.positions[0];
    const auto& p1 = model.positions[1];
    const auto& p2 = model.positions[2];
    const float crossX = (p1.y - p0.y) * (p2.z - p0.z)
        - (p1.z - p0.z) * (p2.y - p0.y);
    const float crossY = (p1.z - p0.z) * (p2.x - p0.x)
        - (p1.x - p0.x) * (p2.z - p0.z);
    const float crossZ = (p1.x - p0.x) * (p2.y - p0.y)
        - (p1.y - p0.y) * (p2.x - p0.x);
    const auto& normal = model.attributes[0];
    expect(crossX * normal.nx + crossY * normal.ny + crossZ * normal.nz > 0.0f,
        "emitted engine-space winding is counter-clockwise relative to its normal");
}

void testUvPreservationAndVertexDeduplication()
{
    const std::vector<Triangle> quad = makeQuadTriangles();
    const DecodeResult joined = decodeStaticMesh(makeStream(quad), "quad.blend");
    expect(static_cast<bool>(joined), "two-triangle Blender quad decodes");
    if (!joined) {
        std::cerr << joined.error << '\n';
        return;
    }
    expectOneOpaqueGeometry(joined.model, 6);
    expect(joined.model.positions.size() == 4,
        "identical position/normal/UV corner tuples deduplicate across triangles");
    expect(joined.model.indices
            == std::vector<std::uint32_t>{0, 1, 2, 0, 3, 1},
        "deduplication preserves deterministic first-use vertex order");
    if (joined.model.attributes.size() == 4) {
        expect(joined.model.attributes[0].u == 0.0f
                && joined.model.attributes[0].v == 0.0f
                && joined.model.attributes[1].u == 1.0f
                && joined.model.attributes[1].v == 1.0f
                && joined.model.attributes[2].u == 1.0f
                && joined.model.attributes[2].v == 0.0f
                && joined.model.attributes[3].u == 0.0f
                && joined.model.attributes[3].v == 1.0f,
            "all quad UVs survive exact and without V-axis flipping");
    }

    std::vector<Triangle> seamedQuad = quad;
    // The second triangle's source corner 1 is the shared p2. A distinct UV
    // must split it even though its transformed position and normal are equal.
    seamedQuad[1][1].u = 0.25f;
    seamedQuad[1][1].v = 0.75f;
    const DecodeResult seamed =
        decodeStaticMesh(makeStream(seamedQuad), "seamed-quad.blend");
    expect(static_cast<bool>(seamed), "quad with a UV seam decodes");
    if (!seamed) {
        std::cerr << seamed.error << '\n';
        return;
    }
    expect(seamed.model.positions.size() == 5,
        "a UV seam creates exactly one additional unique vertex");
    expect(seamed.model.indices
            == std::vector<std::uint32_t>{0, 1, 2, 0, 3, 4},
        "UV seam receives a deterministic new local index");
    if (seamed.model.attributes.size() == 5) {
        expect(seamed.model.attributes[4].u == 0.25f
                && seamed.model.attributes[4].v == 0.75f,
            "the split vertex preserves its distinct UV exactly");
    }
}

void testAlphaTestedMaterialAndTextureVNormalization()
{
    constexpr std::string_view textureReference =
        "trees\\trees_new_vetka_green";
    const std::vector<std::uint8_t> source = makeTexturedStream(
        makeQuadTriangles(), textureReference, 128.0f / 255.0f);
    const DecodeResult decoded = decodeStaticMesh(source, "leaf-card.blend");
    expect(static_cast<bool>(decoded),
        "alpha-tested Blender leaf-card stream decodes");
    if (!decoded) {
        std::cerr << decoded.error << '\n';
        return;
    }

    const Model& model = decoded.model;
    expectOneAlphaTestedGeometry(
        model, 6, textureReference, 128.0f / 255.0f);
    expect(model.positions.size() == 4,
        "alpha-tested quad deduplicates to four vertices");
    if (model.attributes.size() == 4) {
        expect(model.attributes[0].u == 0.0f
                && model.attributes[0].v == 1.0f
                && model.attributes[1].u == 1.0f
                && model.attributes[1].v == 0.0f
                && model.attributes[2].u == 1.0f
                && model.attributes[2].v == 1.0f
                && model.attributes[3].u == 0.0f
                && model.attributes[3].v == 0.0f,
            "textured v2 flips Blender V for DDS/Vulkan top-row sampling");
    }

    const SerializeResult serialized =
        xrphoton::ogfx::serializeModel(model, "leaf-card.ogfx");
    expect(static_cast<bool>(serialized),
        "alpha-tested Blender model passes through the canonical writer");
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return;
    }
    const DecodeResult schema = xrphoton::ogfx::decodeModelSchema(
        serialized.bytes, "leaf-card-schema.ogfx");
    const DecodeResult runtime = xrphoton::ogfx::decodeModel(
        serialized.bytes, "leaf-card-runtime.ogfx");
    expect(static_cast<bool>(schema) && modelsEqual(schema.model, model),
        "schema decoder exactly reconstructs textured Blender output");
    expect(static_cast<bool>(runtime) && modelsEqual(runtime.model, model),
        "runtime decoder exactly reconstructs textured Blender output");
    if (!schema) {
        std::cerr << schema.error << '\n';
        return;
    }
    const SerializeResult roundTrip =
        xrphoton::ogfx::serializeModel(schema.model, "leaf-card-round-trip.ogfx");
    expect(static_cast<bool>(roundTrip)
            && roundTrip.bytes == serialized.bytes,
        "textured Blender writer/schema/writer round trip is byte exact");
}

void testOpaqueTexturedMaterialAndTextureVNormalization()
{
    constexpr std::string_view textureReference =
        "xrphoton\\remade_bochka_close_1_basecolor";
    const std::vector<std::uint8_t> source = makeTexturedStream(
        makeQuadTriangles(), textureReference, 0.5f, 0);
    const DecodeResult decoded = decodeStaticMesh(source, "remade-barrel.blend");
    expect(static_cast<bool>(decoded),
        "opaque-textured Blender barrel stream decodes");
    if (!decoded) {
        std::cerr << decoded.error << '\n';
        return;
    }

    const Model& model = decoded.model;
    expectOneOpaqueTexturedGeometry(model, 6, textureReference);
    if (model.attributes.size() == 4) {
        expect(model.attributes[0].u == 0.0f
                && model.attributes[0].v == 1.0f
                && model.attributes[1].u == 1.0f
                && model.attributes[1].v == 0.0f
                && model.attributes[2].u == 1.0f
                && model.attributes[2].v == 1.0f
                && model.attributes[3].u == 0.0f
                && model.attributes[3].v == 0.0f,
            "opaque-textured v2 uses the same one-time DDS V normalization");
    }

    const SerializeResult serialized =
        xrphoton::ogfx::serializeModel(model, "remade-barrel.ogfx");
    expect(static_cast<bool>(serialized),
        "opaque-textured Blender model passes through the canonical writer");
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return;
    }
    const DecodeResult runtime = xrphoton::ogfx::decodeModel(
        serialized.bytes, "remade-barrel-runtime.ogfx");
    expect(static_cast<bool>(runtime) && modelsEqual(runtime.model, model),
        "runtime decoder exactly reconstructs opaque-textured Blender output");
}

void testNonuniformTransformAndUnitScale()
{
    constexpr float inverseSqrt2 = 0.7071067690849304f;
    const Triangle triangle{{
        Corner{{0.0f, 0.0f, 0.0f}, {0.0f, -inverseSqrt2, inverseSqrt2}},
        Corner{{1.0f, 0.0f, 0.0f}, {0.0f, -inverseSqrt2, inverseSqrt2}},
        Corner{{0.0f, 1.0f, 1.0f}, {0.0f, -inverseSqrt2, inverseSqrt2}},
    }};
    constexpr std::array<float, 12> transform{
        2.0f, 0.0f, 0.0f, 10.0f,
        0.0f, 3.0f, 0.0f, 20.0f,
        0.0f, 0.0f, 4.0f, 30.0f,
    };
    const DecodeResult decoded = decodeStaticMesh(
        makeStream({triangle}, 0, transform, 0.5f),
        "nonuniform.blend");
    expect(static_cast<bool>(decoded),
        "nonuniformly transformed Blender triangle decodes");
    if (!decoded) {
        std::cerr << decoded.error << '\n';
        return;
    }
    expect(decoded.model.positions.size() == 3,
        "nonuniform transform retains three unique corners");
    if (decoded.model.positions.size() != 3
        || decoded.model.attributes.size() != 3) {
        return;
    }

    expect(decoded.model.positions[0].x == 5.0f
            && decoded.model.positions[0].y == 15.0f
            && decoded.model.positions[0].z == 10.0f,
        "scene unit scale applies to baked object translation");
    expect(decoded.model.positions[1].x == 5.0f
            && decoded.model.positions[1].y == 17.0f
            && decoded.model.positions[1].z == 11.5f,
        "scene unit scale applies after the full nonuniform affine transform");
    expect(decoded.model.positions[2].x == 6.0f
            && decoded.model.positions[2].y == 15.0f
            && decoded.model.positions[2].z == 10.0f,
        "CCW reversal remains correct after nonuniform scaling");
    for (const auto& attributes : decoded.model.attributes) {
        expect(near(attributes.nx, 0.0f)
                && near(attributes.ny, 0.6f)
                && near(attributes.nz, -0.8f),
            "normal uses normalized inverse-transpose, then the engine axis map");
    }
}

void testNegativeDeterminantKeepsSourceOrder()
{
    constexpr std::array<float, 12> reflectedTransform{
        -1.0f, 0.0f, 0.0f, 0.0f,
         0.0f, 1.0f, 0.0f, 0.0f,
         0.0f, 0.0f, 1.0f, 0.0f,
    };
    const Triangle triangle{{
        Corner{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        Corner{{2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        Corner{{0.0f, 3.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    }};
    const DecodeResult decoded = decodeStaticMesh(
        makeStream({triangle}, 0, reflectedTransform),
        "reflected.blend");
    expect(static_cast<bool>(decoded),
        "negative-determinant Blender object transform decodes");
    if (!decoded || decoded.model.positions.size() != 3) {
        if (!decoded) {
            std::cerr << decoded.error << '\n';
        }
        return;
    }

    // The object reflection and the Blender-to-engine axis reflection cancel,
    // so this case deliberately keeps the source corner order.
    expect(decoded.model.positions[0].x == 0.0f
            && decoded.model.positions[1].x == -2.0f
            && decoded.model.positions[1].z == 0.0f
            && decoded.model.positions[2].x == 0.0f
            && decoded.model.positions[2].z == 3.0f,
        "negative object determinant cancels the axis-map winding reversal");
    expect(decoded.model.indices == std::vector<std::uint32_t>{0, 1, 2},
        "reflected transform keeps deterministic source index order");
    for (const auto& attributes : decoded.model.attributes) {
        expect(near(attributes.nx, 0.0f)
                && near(attributes.ny, 1.0f)
                && near(attributes.nz, 0.0f),
            "reflected transform produces the matching engine-space normal");
    }
}

void testShearedTransformUsesFullInverseTranspose()
{
    const Triangle triangle{{
        Corner{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        Corner{{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        Corner{{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    }};
    // z' = x + z is deliberately non-symmetric. A diagonal-only or transposed
    // implementation cannot produce the expected (-X,+Y) engine-space normal.
    constexpr std::array<float, 12> shearedTransform{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 1.0f, 0.0f,
    };
    const DecodeResult decoded = decodeStaticMesh(
        makeStream({triangle}, 0, shearedTransform),
        "sheared.blend");
    expect(static_cast<bool>(decoded),
        "off-diagonal sheared Blender triangle decodes");
    if (!decoded || decoded.model.positions.size() != 3
        || decoded.model.attributes.size() != 3) {
        if (!decoded) {
            std::cerr << decoded.error << '\n';
        }
        return;
    }

    expect(decoded.model.positions[0].x == 0.0f
            && decoded.model.positions[0].y == 0.0f
            && decoded.model.positions[0].z == 0.0f
            && decoded.model.positions[1].x == 0.0f
            && decoded.model.positions[1].y == 0.0f
            && decoded.model.positions[1].z == 1.0f
            && decoded.model.positions[2].x == 1.0f
            && decoded.model.positions[2].y == 1.0f
            && decoded.model.positions[2].z == 0.0f,
        "sheared positions use the complete affine matrix and reversed winding");
    constexpr float inverseSqrt2 = 0.7071067690849304f;
    for (const auto& attributes : decoded.model.attributes) {
        expect(near(attributes.nx, -inverseSqrt2)
                && near(attributes.ny, inverseSqrt2)
                && near(attributes.nz, 0.0f),
            "off-diagonal normal uses A inverse-transpose before the axis map");
    }
}

void testFramingAndHeaderRejections()
{
    const std::vector<std::uint8_t> valid = makeAsymmetricStream();

    expectRejected({}, "byte size");
    expectRejected(
        std::vector<std::uint8_t>(
            valid.begin(),
            valid.begin() + xrphoton::blender_mesh::StreamHeaderSizeV1 - 1),
        "byte size");

    std::vector<std::uint8_t> bytes = valid;
    bytes[0] = 'N';
    expectRejected(bytes, "magic");

    bytes = valid;
    writeU32(&bytes, VersionOffset, xrphoton::blender_mesh::StreamVersion2 + 1);
    expectRejected(bytes, "version");

    bytes = valid;
    writeU32(
        &bytes,
        HeaderSizeOffset,
        xrphoton::blender_mesh::StreamHeaderSizeV1 + 4);
    expectRejected(bytes, "header byte size");

    bytes = valid;
    writeU32(&bytes, FlagsOffset, 2);
    expectRejected(bytes, "flags");

    bytes = valid;
    writeU32(&bytes, TriangleCountOffset, 0);
    expectRejected(bytes, "triangle count");

    bytes = valid;
    writeU32(&bytes, TriangleCountOffset, std::numeric_limits<std::uint32_t>::max());
    expectRejected(bytes, "triangle count");

    bytes = valid;
    writeU32(
        &bytes,
        TriangleCountOffset,
        xrphoton::blender_mesh::MaximumTriangleCount + 1);
    expectRejected(bytes, "triangle count");

    bytes = valid;
    writeU32(&bytes, TriangleCountOffset, 2);
    expectRejected(bytes, "byte size");

    bytes = valid;
    bytes.pop_back();
    expectRejected(bytes, "byte size");

    bytes = valid;
    bytes.push_back(0);
    expectRejected(bytes, "byte size");

    bytes = valid;
    writeU32(&bytes, BlenderMajorOffset, 4);
    expectRejected(bytes, "Blender version");

    bytes = valid;
    writeU32(&bytes, BlenderMinorOffset, 0);
    expectRejected(bytes, "Blender version");

    bytes = valid;
    writeF32(&bytes, UnitScaleOffset, 0.0f);
    expectRejected(bytes, "scene unit scale");

    bytes = valid;
    writeF32(&bytes, UnitScaleOffset, std::numeric_limits<float>::quiet_NaN());
    expectRejected(bytes, "scene unit scale");

    for (const std::size_t reservedOffset : {
             FirstReservedOffset, SecondReservedOffset, ThirdReservedOffset}) {
        bytes = valid;
        writeU32(&bytes, reservedOffset, 1);
        expectRejected(bytes, "reserved header words");
    }

    bytes = valid;
    writeF32(&bytes, MatrixOffset, std::numeric_limits<float>::infinity());
    expectRejected(bytes, "object transform");
}

void testTexturedHeaderRejections()
{
    const std::vector<std::uint8_t> valid =
        makeTexturedStream(makeQuadTriangles());
    std::vector<std::uint8_t> bytes(
        valid.begin(),
        valid.begin() + xrphoton::blender_mesh::StreamHeaderSizeV2 - 1);
    expectRejected(bytes, "byte size");

    bytes = valid;
    writeU32(
        &bytes,
        HeaderSizeOffset,
        xrphoton::blender_mesh::StreamHeaderSizeV1);
    expectRejected(bytes, "header byte size");

    bytes = valid;
    writeU32(&bytes, FlagsOffset, 0);
    expectRejected(bytes, "flags");

    for (const std::uint32_t materialFlags : {2u, 3u}) {
        bytes = valid;
        writeU32(&bytes, MaterialFlagsOffset, materialFlags);
        expectRejected(bytes, "material flags");
    }

    for (const float cutoff : {
             -0.01f,
             1.01f,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity()}) {
        bytes = valid;
        writeF32(&bytes, MaterialAlphaCutoffOffset, cutoff);
        expectRejected(bytes, "material alpha cutoff");
    }

    bytes = valid;
    writeU32(&bytes, TextureReferenceByteCountOffset, 0);
    expectRejected(bytes, "material texture-reference byte count");

    bytes = valid;
    writeU32(
        &bytes,
        TextureReferenceByteCountOffset,
        xrphoton::ogfx::MaximumStringBytes + 1);
    expectRejected(bytes, "material texture-reference byte count");

    bytes = valid;
    writeU32(&bytes, MaterialReservedOffset, 1);
    expectRejected(bytes, "material reserved word");

    bytes = valid;
    bytes[V2TextureReferenceOffset] = '.';
    expectRejected(bytes, "material texture reference");

    bytes = valid;
    const std::uint32_t referenceSize = static_cast<std::uint32_t>(
        std::string_view("trees\\trees_new_vetka_green").size());
    writeU32(&bytes, TextureReferenceByteCountOffset, referenceSize - 1);
    expectRejected(bytes, "byte size");

    bytes = valid;
    writeU32(&bytes, TextureReferenceByteCountOffset, referenceSize + 1);
    expectRejected(bytes, "byte size");

    bytes = valid;
    bytes.pop_back();
    expectRejected(bytes, "byte size");

    bytes = valid;
    bytes.push_back(0);
    expectRejected(bytes, "byte size");
}

void testPayloadAndGeometryRejections()
{
    const std::vector<std::uint8_t> valid = makeAsymmetricStream();
    std::vector<std::uint8_t> bytes = valid;

    writeU32(&bytes, FlagsOffset, 0);
    expectRejected(bytes, "triangles[0].corners[0].UV");

    bytes = valid;
    writeF32(
        &bytes,
        FirstCornerOffset + PositionOffsetInCorner,
        std::numeric_limits<float>::quiet_NaN());
    expectRejected(bytes, "triangles[0].corners[0].position");

    bytes = valid;
    writeF32(
        &bytes,
        FirstCornerOffset + NormalOffsetInCorner,
        std::numeric_limits<float>::infinity());
    expectRejected(bytes, "triangles[0].corners[0].normal");

    bytes = valid;
    writeF32(
        &bytes,
        FirstCornerOffset + UvOffsetInCorner,
        std::numeric_limits<float>::quiet_NaN());
    expectRejected(bytes, "triangles[0].corners[0].UV");

    bytes = valid;
    writeF32(&bytes, MatrixOffset + 10 * sizeof(float), 0.0f);
    expectRejected(bytes, "object transform determinant");

    bytes = valid;
    writeF32(&bytes, MatrixOffset + 10 * sizeof(float), 1.0e-13f);
    expectRejected(bytes, "object transform determinant");

    bytes = valid;
    for (std::size_t component = 0; component < 3; ++component) {
        writeF32(
            &bytes,
            FirstCornerOffset + NormalOffsetInCorner
                + component * sizeof(float),
            0.0f);
    }
    expectRejected(bytes, "triangles[0].corners[0].transformed data");

    bytes = valid;
    writeF32(&bytes, MatrixOffset, std::numeric_limits<float>::max());
    expectRejected(bytes, "transformed data");

    Triangle degenerate = makeAsymmetricTriangle();
    degenerate[2].position = {9.0f, -2.0f, 3.0f};
    expectRejected(makeStream({degenerate}), "triangles[0].area");

    Triangle opposed = makeAsymmetricTriangle();
    for (float& component : opposed[0].normal) {
        component = -component;
    }
    expectRejected(
        makeStream({opposed}),
        "triangles[0].corners[0].normal orientation");
}

void testCanonicalWriterSchemaRuntimeAndDeterminism()
{
    const std::vector<std::uint8_t> source = makeStream(makeQuadTriangles());
    const DecodeResult first = decodeStaticMesh(source, "first-source.blend");
    const DecodeResult second = decodeStaticMesh(source, "second-source.blend");
    expect(static_cast<bool>(first) && static_cast<bool>(second),
        "the same Blender stream decodes repeatedly");
    if (!first || !second) {
        if (!first.error.empty()) {
            std::cerr << first.error << '\n';
        }
        if (!second.error.empty()) {
            std::cerr << second.error << '\n';
        }
        return;
    }
    expect(modelsEqual(first.model, second.model),
        "adapter output is deterministic and ignores diagnostic names");

    const SerializeResult firstBytes =
        xrphoton::ogfx::serializeModel(first.model, "first-output.ogfx");
    const SerializeResult secondBytes =
        xrphoton::ogfx::serializeModel(second.model, "second-output.ogfx");
    expect(static_cast<bool>(firstBytes) && static_cast<bool>(secondBytes),
        "Blender adapter model passes through the canonical writer");
    if (!firstBytes || !secondBytes) {
        if (!firstBytes.error.empty()) {
            std::cerr << firstBytes.error << '\n';
        }
        if (!secondBytes.error.empty()) {
            std::cerr << secondBytes.error << '\n';
        }
        return;
    }
    expect(firstBytes.bytes == secondBytes.bytes,
        "two adapter/writer runs are byte-identical");
    expect(firstBytes.bytes.size() >= xrphoton::ogfx::FileMagic.size()
            && std::equal(
                xrphoton::ogfx::FileMagic.begin(),
                xrphoton::ogfx::FileMagic.end(),
                firstBytes.bytes.begin()),
        "Blender conversion emits canonical OGFx rather than its private XRBM stream");

    const DecodeResult schema = xrphoton::ogfx::decodeModelSchema(
        firstBytes.bytes, "blender-schema.ogfx");
    const DecodeResult runtime = xrphoton::ogfx::decodeModel(
        firstBytes.bytes, "blender-runtime.ogfx");
    expect(static_cast<bool>(schema) && modelsEqual(schema.model, first.model),
        "complete schema decoder exactly reconstructs the Blender adapter model");
    expect(static_cast<bool>(runtime) && modelsEqual(runtime.model, first.model),
        "runtime decoder accepts and exactly reconstructs opaque Blender output");
    if (!schema) {
        std::cerr << schema.error << '\n';
        return;
    }
    const SerializeResult roundTrip =
        xrphoton::ogfx::serializeModel(schema.model, "round-trip.ogfx");
    expect(static_cast<bool>(roundTrip) && roundTrip.bytes == firstBytes.bytes,
        "Blender writer/schema/writer round trip is byte exact");
}

bool writeBytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    if (path == "-") {
        std::cout.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        std::cout.flush();
        if (!std::cout) {
            std::cerr << "FAIL: could not write the XRBM fixture to stdout.\n";
            return false;
        }
        return true;
    }

    std::error_code directoryError;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), directoryError);
    }
    if (directoryError) {
        std::cerr << "FAIL: could not create Blender CLI fixture directory: "
                  << directoryError.message() << '\n';
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "FAIL: could not open Blender CLI fixture output.\n";
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::cerr << "FAIL: could not write complete Blender CLI fixture.\n";
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
        std::cerr << "FAIL: could not open Blender CLI test file " << path << ".\n";
        return false;
    }
    const std::streampos end = input.tellg();
    if (end < 0) {
        std::cerr << "FAIL: could not determine Blender CLI test file size.\n";
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
        std::cerr << "FAIL: could not read complete Blender CLI test file.\n";
        return false;
    }
    return true;
}

std::uint16_t readU16At(
    std::span<const std::uint8_t> bytes,
    std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t readU32At(
    std::span<const std::uint8_t> bytes,
    std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void verifyLeafTexture(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, &bytes)) {
        ++failureCount;
        return;
    }
    constexpr std::size_t DdsFileHeaderSize = 128;
    expect(bytes.size() >= DdsFileHeaderSize,
        "leaf texture contains a complete DDS header");
    if (bytes.size() < DdsFileHeaderSize) {
        return;
    }
    expect(bytes[0] == 'D' && bytes[1] == 'D'
            && bytes[2] == 'S' && bytes[3] == ' ',
        "leaf texture has DDS magic");
    const std::uint32_t height = readU32At(bytes, 12);
    const std::uint32_t width = readU32At(bytes, 16);
    expect(width == 512 && height == 512,
        "leaf texture has the pinned 512x512 dimensions");
    constexpr std::uint32_t Dxt1 =
        static_cast<std::uint32_t>('D')
        | (static_cast<std::uint32_t>('X') << 8)
        | (static_cast<std::uint32_t>('T') << 16)
        | (static_cast<std::uint32_t>('1') << 24);
    expect(readU32At(bytes, 84) == Dxt1,
        "leaf texture uses renderer-supported DXT1/BC1");
    if (width != 512 || height != 512 || readU32At(bytes, 84) != Dxt1) {
        return;
    }

    const std::uint64_t blockWidth = (static_cast<std::uint64_t>(width) + 3) / 4;
    const std::uint64_t blockHeight = (static_cast<std::uint64_t>(height) + 3) / 4;
    const std::uint64_t mipZeroBytes = blockWidth * blockHeight * 8;
    expect(mipZeroBytes <= bytes.size() - DdsFileHeaderSize,
        "leaf texture contains its complete BC1 mip 0");
    if (mipZeroBytes > bytes.size() - DdsFileHeaderSize) {
        return;
    }

    std::uint64_t transparentTexels = 0;
    for (std::uint64_t blockY = 0; blockY < blockHeight; ++blockY) {
        for (std::uint64_t blockX = 0; blockX < blockWidth; ++blockX) {
            const std::size_t offset = DdsFileHeaderSize
                + static_cast<std::size_t>((blockY * blockWidth + blockX) * 8);
            const std::uint16_t color0 = readU16At(bytes, offset);
            const std::uint16_t color1 = readU16At(bytes, offset + 2);
            const std::uint32_t selectors = readU32At(bytes, offset + 4);
            if (color0 > color1) {
                continue;
            }
            for (std::uint32_t texel = 0; texel < 16; ++texel) {
                const std::uint64_t x = blockX * 4 + (texel % 4);
                const std::uint64_t y = blockY * 4 + (texel / 4);
                if (x < width && y < height
                    && ((selectors >> (texel * 2)) & 3u) == 3u) {
                    ++transparentTexels;
                }
            }
        }
    }
    expect(transparentTexels == 153'894,
        "leaf texture has exactly 153894 fully transparent mip-0 texels");
    expect(transparentTexels > 0
            && transparentTexels < static_cast<std::uint64_t>(width) * height,
        "leaf texture contains both visible and rejected alpha-test samples");
}

void verifyRemadeBarrelTexture(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, &bytes)) {
        ++failureCount;
        return;
    }
    constexpr std::size_t DdsFileHeaderSize = 128;
    expect(bytes.size() == 1'398'248,
        "remade barrel texture has the pinned complete DDS byte size");
    if (bytes.size() < DdsFileHeaderSize) {
        return;
    }
    expect(bytes[0] == 'D' && bytes[1] == 'D'
            && bytes[2] == 'S' && bytes[3] == ' ',
        "remade barrel texture has DDS magic");
    const std::uint32_t height = readU32At(bytes, 12);
    const std::uint32_t width = readU32At(bytes, 16);
    expect(width == 1024 && height == 2048,
        "remade barrel texture has the pinned 1024x2048 atlas dimensions");
    constexpr std::uint32_t Dxt1 =
        static_cast<std::uint32_t>('D')
        | (static_cast<std::uint32_t>('X') << 8)
        | (static_cast<std::uint32_t>('T') << 16)
        | (static_cast<std::uint32_t>('1') << 24);
    expect(readU32At(bytes, 84) == Dxt1,
        "remade barrel texture uses renderer-supported DXT1/BC1");
    expect(readU32At(bytes, 8) == 0x000A1007
            && readU32At(bytes, 20) == 1'048'576
            && readU32At(bytes, 28) == 12
            && readU32At(bytes, 108) == 0x00401008,
        "remade barrel DDS declares one canonical complete 12-level mip chain");
    expect(readU32At(bytes, 88) == 0
            && readU32At(bytes, 92) == 0
            && readU32At(bytes, 96) == 0
            && readU32At(bytes, 100) == 0
            && readU32At(bytes, 104) == 0,
        "remade barrel compressed DDS leaves RGB bit count and masks zero");
    if (width != 1024 || height != 2048 || readU32At(bytes, 84) != Dxt1) {
        return;
    }

    const std::uint64_t blockWidth = (static_cast<std::uint64_t>(width) + 3) / 4;
    const std::uint64_t blockHeight = (static_cast<std::uint64_t>(height) + 3) / 4;
    const std::uint64_t mipZeroBytes = blockWidth * blockHeight * 8;
    std::uint64_t transparentSelectors = 0;
    for (std::uint64_t block = 0; block < blockWidth * blockHeight; ++block) {
        const std::size_t offset = DdsFileHeaderSize
            + static_cast<std::size_t>(block * 8);
        const std::uint16_t color0 = readU16At(bytes, offset);
        const std::uint16_t color1 = readU16At(bytes, offset + 2);
        const std::uint32_t selectors = readU32At(bytes, offset + 4);
        if (color0 <= color1) {
            for (std::uint32_t texel = 0; texel < 16; ++texel) {
                transparentSelectors +=
                    ((selectors >> (texel * 2)) & 3u) == 3u;
            }
        }
    }
    expect(mipZeroBytes == 1'048'576 && transparentSelectors == 0,
        "remade barrel BC1 mip 0 contains no transparent selector use");
}

void verifyCustomBarrelTexture(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, &bytes)) {
        ++failureCount;
        return;
    }
    constexpr std::size_t DdsFileHeaderSize = 128;
    expect(bytes.size() == 67'108'992,
        "custom barrel texture has the pinned mip-0-only RGBA8 DDS byte size");
    if (bytes.size() < DdsFileHeaderSize) {
        return;
    }
    expect(bytes[0] == 'D' && bytes[1] == 'D'
            && bytes[2] == 'S' && bytes[3] == ' ',
        "custom barrel texture has DDS magic");
    const std::uint32_t height = readU32At(bytes, 12);
    const std::uint32_t width = readU32At(bytes, 16);
    expect(width == 4096 && height == 4096,
        "custom barrel texture has the pinned native 4096x4096 atlas dimensions");
    expect(readU32At(bytes, 80) == 0x00000041
            && readU32At(bytes, 84) == 0,
        "custom barrel texture uses canonical uncompressed RGB-with-alpha framing");
    expect(readU32At(bytes, 8) == 0x0000100F
            && readU32At(bytes, 20) == 16'384
            && readU32At(bytes, 28) == 0
            && readU32At(bytes, 108) == 0x00001000,
        "custom barrel DDS declares exactly one pitch-framed mip-0 level");
    expect(readU32At(bytes, 88) == 32
            && readU32At(bytes, 92) == 0x000000FF
            && readU32At(bytes, 96) == 0x0000FF00
            && readU32At(bytes, 100) == 0x00FF0000
            && readU32At(bytes, 104) == 0xFF000000,
        "custom barrel DDS pins canonical little-endian RGBA8 masks");
    if (width != 4096 || height != 4096 || bytes.size() != 67'108'992) {
        return;
    }

    bool allAlphaOpaque = true;
    bool hasColorVariation = false;
    const std::array firstColor{bytes[128], bytes[129], bytes[130]};
    for (std::size_t offset = DdsFileHeaderSize;
         offset < bytes.size();
         offset += 4) {
        allAlphaOpaque &= bytes[offset + 3] == 255;
        hasColorVariation |= bytes[offset] != firstColor[0]
            || bytes[offset + 1] != firstColor[1]
            || bytes[offset + 2] != firstColor[2];
    }
    expect(allAlphaOpaque,
        "custom barrel RGBA8 mip 0 is fully opaque");
    expect(hasColorVariation,
        "custom barrel RGBA8 mip 0 contains nonuniform authored color data");
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
        "piped and INPUT_FILE Blender CLI conversions are byte-identical");

    const DecodeResult source =
        decodeStaticMesh(makeAsymmetricStream(), "expected-cli-source.blend");
    expect(static_cast<bool>(source),
        "expected Blender CLI fixture decodes in the verifier");
    if (!source) {
        std::cerr << source.error << '\n';
        return;
    }
    const SerializeResult expected =
        xrphoton::ogfx::serializeModel(source.model, "expected-cli-output.ogfx");
    expect(static_cast<bool>(expected) && firstBytes == expected.bytes,
        "Blender CLI output is exactly the canonical writer output");

    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(firstBytes, firstPath.string());
    const DecodeResult runtime =
        xrphoton::ogfx::decodeModel(firstBytes, firstPath.string());
    expect(static_cast<bool>(schema) && modelsEqual(schema.model, source.model),
        "Blender CLI output passes complete schema reconstruction");
    expect(static_cast<bool>(runtime) && modelsEqual(runtime.model, source.model),
        "Blender CLI output passes the staged runtime profile");
}

struct Bounds
{
    xrphoton::ogfx::Position minimum{
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
    };
    xrphoton::ogfx::Position maximum{
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
};

Bounds calculateBounds(const Model& model)
{
    Bounds bounds{};
    for (const auto& position : model.positions) {
        bounds.minimum.x = std::min(bounds.minimum.x, position.x);
        bounds.minimum.y = std::min(bounds.minimum.y, position.y);
        bounds.minimum.z = std::min(bounds.minimum.z, position.z);
        bounds.maximum.x = std::max(bounds.maximum.x, position.x);
        bounds.maximum.y = std::max(bounds.maximum.y, position.y);
        bounds.maximum.z = std::max(bounds.maximum.z, position.z);
    }
    return bounds;
}

bool everyTriangleIsCcwAndAgreesWithNormals(const Model& model)
{
    if (model.geometries.size() != 1) {
        return false;
    }
    const auto& geometry = model.geometries[0];
    if ((geometry.indexCount % 3) != 0
        || static_cast<std::uint64_t>(geometry.firstIndex)
                + geometry.indexCount
            > model.indices.size()) {
        return false;
    }

    for (std::uint32_t indexOffset = 0;
         indexOffset < geometry.indexCount;
         indexOffset += 3) {
        std::array<std::size_t, 3> unifiedIndices{};
        for (std::size_t corner = 0; corner < unifiedIndices.size(); ++corner) {
            const std::uint32_t localIndex =
                model.indices[geometry.firstIndex + indexOffset + corner];
            if (localIndex >= geometry.vertexCount) {
                return false;
            }
            unifiedIndices[corner] =
                static_cast<std::size_t>(geometry.firstVertex) + localIndex;
            if (unifiedIndices[corner] >= model.positions.size()
                || unifiedIndices[corner] >= model.attributes.size()) {
                return false;
            }
        }

        const auto& p0 = model.positions[unifiedIndices[0]];
        const auto& p1 = model.positions[unifiedIndices[1]];
        const auto& p2 = model.positions[unifiedIndices[2]];
        const double edge1X = static_cast<double>(p1.x) - p0.x;
        const double edge1Y = static_cast<double>(p1.y) - p0.y;
        const double edge1Z = static_cast<double>(p1.z) - p0.z;
        const double edge2X = static_cast<double>(p2.x) - p0.x;
        const double edge2Y = static_cast<double>(p2.y) - p0.y;
        const double edge2Z = static_cast<double>(p2.z) - p0.z;
        const double crossX = edge1Y * edge2Z - edge1Z * edge2Y;
        const double crossY = edge1Z * edge2X - edge1X * edge2Z;
        const double crossZ = edge1X * edge2Y - edge1Y * edge2X;
        const double areaSquared =
            crossX * crossX + crossY * crossY + crossZ * crossZ;
        if (!std::isfinite(areaSquared) || areaSquared <= 1.0e-24) {
            return false;
        }

        for (const std::size_t unifiedIndex : unifiedIndices) {
            const auto& normal = model.attributes[unifiedIndex];
            const double agreement = crossX * normal.nx
                + crossY * normal.ny
                + crossZ * normal.nz;
            if (!std::isfinite(agreement) || agreement <= 0.0) {
                return false;
            }
        }
    }
    return true;
}

bool verifyCommonBlenderProofOutput(
    const std::filesystem::path& path,
    std::string_view assetLabel,
    std::size_t expectedVertexCount,
    std::size_t expectedIndexCount,
    Model* model)
{
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, &bytes)) {
        ++failureCount;
        return false;
    }

    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(bytes, path.string());
    const DecodeResult runtime =
        xrphoton::ogfx::decodeModel(bytes, path.string());
    expect(static_cast<bool>(schema),
        std::string(assetLabel) + " passes complete OGFx schema decoding");
    expect(static_cast<bool>(runtime),
        std::string(assetLabel) + " passes the staged OGFx runtime profile");
    if (!schema) {
        std::cerr << schema.error << '\n';
        if (!runtime.error.empty()) {
            std::cerr << runtime.error << '\n';
        }
        return false;
    }
    if (!runtime) {
        std::cerr << runtime.error << '\n';
    } else {
        expect(modelsEqual(runtime.model, schema.model),
            std::string(assetLabel)
                + " schema and runtime decoders reconstruct the same model");
    }

    const SerializeResult canonical =
        xrphoton::ogfx::serializeModel(schema.model, "proof-round-trip.ogfx");
    expect(static_cast<bool>(canonical),
        std::string(assetLabel) + " reconstructed model serializes canonically");
    if (!canonical) {
        std::cerr << canonical.error << '\n';
    } else {
        expect(canonical.bytes == bytes,
            std::string(assetLabel)
                + " schema/writer round trip is byte exact");
    }

    const Model& decoded = schema.model;
    expect(decoded.positions.size() == expectedVertexCount
            && decoded.attributes.size() == expectedVertexCount,
        std::string(assetLabel)
            + " has the pinned complete unified vertex stream count");
    expect(decoded.indices.size() == expectedIndexCount,
        std::string(assetLabel) + " has the pinned complete index count");
    expect(decoded.geometries.size() == 1
            && decoded.geometries[0].firstVertex == 0
            && decoded.geometries[0].vertexCount == expectedVertexCount
            && decoded.geometries[0].firstIndex == 0
            && decoded.geometries[0].indexCount == expectedIndexCount
            && decoded.geometries[0].materialIndex == 0
            && !decoded.geometries[0].alphaTested,
        std::string(assetLabel) + " contains exactly one complete opaque geometry");
    expect(decoded.meshes.size() == 1
            && decoded.meshes[0].firstGeometry == 0
            && decoded.meshes[0].geometryCount == 1,
        std::string(assetLabel) + " contains exactly one mesh");
    expect(decoded.materials.size() == 1
            && decoded.materials[0].baseColorFactor
                == std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}
            && decoded.materials[0].alphaCutoff == 0.5f
            && decoded.materials[0].baseColorTexture.empty(),
        std::string(assetLabel)
            + " contains exactly one opaque white material with no texture");
    expect(everyTriangleIsCcwAndAgreesWithNormals(decoded),
        std::string(assetLabel)
            + " has nondegenerate CCW triangles agreeing with every corner normal");

    *model = decoded;
    return true;
}

void verifyPyramidOutput(const std::filesystem::path& path)
{
    Model model{};
    if (!verifyCommonBlenderProofOutput(
            path, "test_pyramid", 16, 18, &model)) {
        return;
    }

    const Bounds bounds = calculateBounds(model);
    expect(bounds.minimum.x == -1.0f && bounds.maximum.x == 1.0f
            && bounds.minimum.y == 0.0f && bounds.maximum.y == 2.0f
            && bounds.minimum.z == -1.0f && bounds.maximum.z == 1.0f,
        "test_pyramid has exact engine-space X[-1,1], Y[0,2], Z[-1,1] bounds");

    bool uvVaries = false;
    if (!model.attributes.empty()) {
        const float firstU = model.attributes[0].u;
        const float firstV = model.attributes[0].v;
        uvVaries = std::any_of(
            model.attributes.begin() + 1,
            model.attributes.end(),
            [firstU, firstV](const auto& attributes) {
                return attributes.u != firstU || attributes.v != firstV;
            });
    }
    expect(uvVaries,
        "test_pyramid preserves a nonconstant Blender UV layer");
}

void verifyLeafCardOutput(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, &bytes)) {
        ++failureCount;
        return;
    }
    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(bytes, path.string());
    const DecodeResult runtime =
        xrphoton::ogfx::decodeModel(bytes, path.string());
    expect(static_cast<bool>(schema) && static_cast<bool>(runtime),
        "test_leaf_card passes schema and runtime OGFx decoding");
    if (!schema || !runtime) {
        if (!schema) {
            std::cerr << schema.error << '\n';
        }
        if (!runtime) {
            std::cerr << runtime.error << '\n';
        }
        return;
    }
    expect(modelsEqual(schema.model, runtime.model),
        "test_leaf_card schema and runtime models are identical");
    const SerializeResult roundTrip =
        xrphoton::ogfx::serializeModel(schema.model, "leaf-card-round-trip.ogfx");
    expect(static_cast<bool>(roundTrip) && roundTrip.bytes == bytes,
        "test_leaf_card schema/writer round trip is byte exact");

    const Model& model = schema.model;
    expectOneAlphaTestedGeometry(
        model,
        6,
        "trees\\trees_new_vetka_green",
        128.0f / 255.0f);
    expect(model.positions.size() == 4,
        "test_leaf_card has four unified vertices");
    expect(model.indices == std::vector<std::uint32_t>{0, 1, 2, 0, 3, 1},
        "test_leaf_card has the pinned deterministic quad index stream");
    expect(everyTriangleIsCcwAndAgreesWithNormals(model),
        "test_leaf_card triangles are nondegenerate, CCW, and normal-aligned");

    const Bounds bounds = calculateBounds(model);
    expect(bounds.minimum.x == -1.0f && bounds.maximum.x == 1.0f
            && bounds.minimum.y == 0.0f && bounds.maximum.y == 2.0f
            && bounds.minimum.z == 0.0f && bounds.maximum.z == 0.0f,
        "test_leaf_card has exact engine-space X[-1,1], Y[0,2], Z[0,0] bounds");
    if (model.positions.size() == 4 && model.attributes.size() == 4) {
        constexpr std::array<xrphoton::ogfx::Position, 4> ExpectedPositions{{
            {-1.0f, 0.0f, 0.0f},
            {1.0f, 2.0f, 0.0f},
            {1.0f, 0.0f, 0.0f},
            {-1.0f, 2.0f, 0.0f},
        }};
        constexpr std::array<std::array<float, 2>, 4> ExpectedUvs{{
            {0.0f, 1.0f},
            {1.0f, 0.0f},
            {1.0f, 1.0f},
            {0.0f, 0.0f},
        }};
        bool exactPositionUvPairs = true;
        for (std::size_t index = 0; index < ExpectedPositions.size(); ++index) {
            const auto& position = model.positions[index];
            const auto& attributes = model.attributes[index];
            exactPositionUvPairs = exactPositionUvPairs
                && position.x == ExpectedPositions[index].x
                && position.y == ExpectedPositions[index].y
                && position.z == ExpectedPositions[index].z
                && attributes.u == ExpectedUvs[index][0]
                && attributes.v == ExpectedUvs[index][1];
        }
        expect(exactPositionUvPairs,
            "test_leaf_card pins exact upright position/UV pairs and the one-time V flip");
        expect(std::all_of(
                   model.attributes.begin(),
                   model.attributes.end(),
                   [](const auto& attributes) {
                       return attributes.nx == 0.0f
                           && attributes.ny == 0.0f
                           && attributes.nz == -1.0f;
                   }),
            "test_leaf_card faces the startup camera with exact -Z normals");
    }
}

void verifyRemadeBarrelOutput(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, &bytes)) {
        ++failureCount;
        return;
    }
    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(bytes, path.string());
    const DecodeResult runtime =
        xrphoton::ogfx::decodeModel(bytes, path.string());
    expect(static_cast<bool>(schema) && static_cast<bool>(runtime),
        "remade barrel passes schema and runtime OGFx decoding");
    if (!schema || !runtime) {
        if (!schema) {
            std::cerr << schema.error << '\n';
        }
        if (!runtime) {
            std::cerr << runtime.error << '\n';
        }
        return;
    }
    expect(modelsEqual(schema.model, runtime.model),
        "remade barrel schema and runtime models are identical");
    const SerializeResult roundTrip = xrphoton::ogfx::serializeModel(
        schema.model, "remade-barrel-round-trip.ogfx");
    expect(static_cast<bool>(roundTrip) && roundTrip.bytes == bytes,
        "remade barrel schema/writer round trip is byte exact");

    const Model& model = schema.model;
    expectOneOpaqueTexturedGeometry(
        model,
        47'832,
        "xrphoton\\remade_bochka_close_1_basecolor");
    expect(model.positions.size() == 8'381,
        "remade barrel has the pinned high-fidelity unified vertex count");
    expect(everyTriangleIsCcwAndAgreesWithNormals(model),
        "remade barrel triangles are nondegenerate, CCW, and normal-aligned");
    expect(model.physicsBodies.empty() && model.physicsColliders.empty(),
        "remade visual barrel deliberately adds no physics metadata yet");

    const Bounds bounds = calculateBounds(model);
    expect(near(bounds.minimum.x, -0.3727f)
            && near(bounds.maximum.x, 0.3727f)
            && near(bounds.minimum.y, 0.001489f)
            && near(bounds.maximum.y, 1.0885f)
            && near(bounds.minimum.z, -0.3727f)
            && near(bounds.maximum.z, 0.3727f),
        "remade barrel preserves the SoC barrel's meter-scale bounds");
    expect(std::all_of(
               model.attributes.begin(),
               model.attributes.end(),
               [](const auto& attributes) {
                   return attributes.u >= 0.0f && attributes.u <= 1.063f
                       && attributes.v >= 0.0f && attributes.v <= 1.0f;
               }),
        "remade barrel UVs stay bounded, with only the octagonal bung seam "
        "using the texture's declared repeat mode");
}

void verifyCustomBarrelOutput(const std::filesystem::path& path)
{
    std::vector<std::uint8_t> bytes;
    if (!readBytes(path, &bytes)) {
        ++failureCount;
        return;
    }
    const DecodeResult schema =
        xrphoton::ogfx::decodeModelSchema(bytes, path.string());
    const DecodeResult runtime =
        xrphoton::ogfx::decodeModel(bytes, path.string());
    expect(static_cast<bool>(schema) && static_cast<bool>(runtime),
        "custom barrel passes schema and runtime OGFx decoding");
    if (!schema || !runtime) {
        if (!schema) {
            std::cerr << schema.error << '\n';
        }
        if (!runtime) {
            std::cerr << runtime.error << '\n';
        }
        return;
    }
    expect(modelsEqual(schema.model, runtime.model),
        "custom barrel schema and runtime models are identical");
    const SerializeResult roundTrip = xrphoton::ogfx::serializeModel(
        schema.model, "custom-barrel-round-trip.ogfx");
    expect(static_cast<bool>(roundTrip) && roundTrip.bytes == bytes,
        "custom barrel schema/writer round trip is byte exact");

    const Model& model = schema.model;
    expectOneOpaqueTexturedGeometry(
        model,
        57'384,
        "xrphoton\\custom_stalker_barrel_basecolor");
    expect(model.positions.size() == 11'296,
        "custom barrel has the pinned production-detail unified vertex count");
    expect(everyTriangleIsCcwAndAgreesWithNormals(model),
        "custom barrel triangles are nondegenerate, CCW, and normal-aligned");
    expect(model.physicsBodies.empty() && model.physicsColliders.empty(),
        "custom visual barrel deliberately adds no physics metadata yet");

    const Bounds bounds = calculateBounds(model);
    expect(near(bounds.minimum.x, -0.37147823f)
            && near(bounds.maximum.x, 0.37099651f)
            && near(bounds.minimum.y, 0.0f)
            && near(bounds.maximum.y, 1.09803355f)
            && near(bounds.minimum.z, -0.372f)
            && near(bounds.maximum.z, 0.37123993f),
        "custom barrel keeps a believable one-metre industrial-drum scale");
    expect(std::all_of(
               model.attributes.begin(),
               model.attributes.end(),
               [](const auto& attributes) {
                   return attributes.u >= 0.0f && attributes.u <= 1.0f
                       && attributes.v >= 0.0f && attributes.v <= 1.0f;
               }),
        "custom barrel UVs remain entirely inside the deliberate material atlas");
}

using PositionKey = std::array<std::uint32_t, 3>;
using NormalKey = std::array<std::uint32_t, 3>;
using UvKey = std::array<std::uint32_t, 2>;

PositionKey positionKey(const xrphoton::ogfx::Position& position)
{
    return {
        std::bit_cast<std::uint32_t>(position.x),
        std::bit_cast<std::uint32_t>(position.y),
        std::bit_cast<std::uint32_t>(position.z),
    };
}

NormalKey normalKey(const xrphoton::ogfx::VertexAttributes& attributes)
{
    return {
        std::bit_cast<std::uint32_t>(attributes.nx),
        std::bit_cast<std::uint32_t>(attributes.ny),
        std::bit_cast<std::uint32_t>(attributes.nz),
    };
}

UvKey uvKey(const xrphoton::ogfx::VertexAttributes& attributes)
{
    return {
        std::bit_cast<std::uint32_t>(attributes.u),
        std::bit_cast<std::uint32_t>(attributes.v),
    };
}

bool hasApproximateUnitSphereBounds(const Model& model)
{
    const Bounds bounds = calculateBounds(model);
    return near(bounds.minimum.x, -1.0f, 1.0e-5f)
        && near(bounds.maximum.x, 1.0f, 1.0e-5f)
        && near(bounds.minimum.y, -1.0f, 1.0e-5f)
        && near(bounds.maximum.y, 1.0f, 1.0e-5f)
        && near(bounds.minimum.z, -1.0f, 1.0e-5f)
        && near(bounds.maximum.z, 1.0f, 1.0e-5f);
}

void verifySphereOutput(const std::filesystem::path& path)
{
    Model model{};
    if (!verifyCommonBlenderProofOutput(
            path, "test_sphere", 1984, 2880, &model)) {
        return;
    }

    expect(hasApproximateUnitSphereBounds(model),
        "test_sphere retains approximate unit bounds after the engine axis map");

    std::map<PositionKey, std::pair<NormalKey, UvKey>> firstAttributesByPosition;
    bool hasNormalSplit = false;
    bool hasUvSplit = false;
    for (std::size_t index = 0; index < model.positions.size(); ++index) {
        const auto& attributes = model.attributes[index];
        const std::pair attributeTuple{normalKey(attributes), uvKey(attributes)};
        const auto [found, inserted] = firstAttributesByPosition.emplace(
            positionKey(model.positions[index]), attributeTuple);
        if (!inserted) {
            hasNormalSplit |= found->second.first != attributeTuple.first;
            hasUvSplit |= found->second.second != attributeTuple.second;
        }
    }
    expect(firstAttributesByPosition.size() == 482,
        "test_sphere retains the source UV sphere's 482 geometric positions");
    expect(hasNormalSplit,
        "test_sphere splits equal positions across flat face normals");
    expect(hasUvSplit,
        "test_sphere also preserves its UV seam splits");
}

void verifySmoothSphereOutput(const std::filesystem::path& path)
{
    Model model{};
    if (!verifyCommonBlenderProofOutput(
            path, "test_smooth_sphere", 559, 2880, &model)) {
        return;
    }

    expect(hasApproximateUnitSphereBounds(model),
        "test_smooth_sphere retains approximate unit bounds after the engine axis map");

    std::map<PositionKey, std::pair<NormalKey, UvKey>> firstAttributesByPosition;
    bool hasNormalSplit = false;
    bool hasUvSplit = false;
    for (std::size_t index = 0; index < model.positions.size(); ++index) {
        const auto& attributes = model.attributes[index];
        const std::pair attributeTuple{normalKey(attributes), uvKey(attributes)};
        const auto [found, inserted] = firstAttributesByPosition.emplace(
            positionKey(model.positions[index]), attributeTuple);
        if (!inserted) {
            hasNormalSplit |= found->second.first != attributeTuple.first;
            hasUvSplit |= found->second.second != attributeTuple.second;
        }
    }
    expect(firstAttributesByPosition.size() == 482,
        "test_smooth_sphere retains the source UV sphere's 482 geometric positions");
    expect(!hasNormalSplit,
        "test_smooth_sphere shares one smooth normal across every equal position");
    expect(hasUvSplit,
        "test_smooth_sphere preserves UV seam splits without splitting its normals");
}

void verifySpherePairOutputs(
    const std::filesystem::path& flatPath,
    const std::filesystem::path& smoothPath)
{
    std::vector<std::uint8_t> flatBytes;
    std::vector<std::uint8_t> smoothBytes;
    if (!readBytes(flatPath, &flatBytes) || !readBytes(smoothPath, &smoothBytes)) {
        ++failureCount;
        return;
    }
    const DecodeResult flat =
        xrphoton::ogfx::decodeModelSchema(flatBytes, flatPath.string());
    const DecodeResult smooth =
        xrphoton::ogfx::decodeModelSchema(smoothBytes, smoothPath.string());
    expect(static_cast<bool>(flat) && static_cast<bool>(smooth),
        "flat/smooth sphere comparison inputs pass schema decoding");
    if (!flat || !smooth) {
        if (!flat) {
            std::cerr << flat.error << '\n';
        }
        if (!smooth) {
            std::cerr << smooth.error << '\n';
        }
        return;
    }

    const bool matchingCornerCounts =
        flat.model.indices.size() == smooth.model.indices.size();
    expect(matchingCornerCounts,
        "flat and smooth spheres have the same triangle-corner count");
    if (!matchingCornerCounts) {
        return;
    }

    bool positionsMatch = true;
    bool uvsMatch = true;
    bool normalDiffers = false;
    for (std::size_t corner = 0; corner < flat.model.indices.size(); ++corner) {
        const std::size_t flatIndex = flat.model.indices[corner];
        const std::size_t smoothIndex = smooth.model.indices[corner];
        if (flatIndex >= flat.model.positions.size()
            || flatIndex >= flat.model.attributes.size()
            || smoothIndex >= smooth.model.positions.size()
            || smoothIndex >= smooth.model.attributes.size()) {
            positionsMatch = false;
            uvsMatch = false;
            break;
        }
        positionsMatch &= positionKey(flat.model.positions[flatIndex])
            == positionKey(smooth.model.positions[smoothIndex]);
        uvsMatch &= uvKey(flat.model.attributes[flatIndex])
            == uvKey(smooth.model.attributes[smoothIndex]);
        normalDiffers |= normalKey(flat.model.attributes[flatIndex])
            != normalKey(smooth.model.attributes[smoothIndex]);
    }
    expect(positionsMatch,
        "flat and smooth spheres have the same indexed geometric corner stream");
    expect(uvsMatch,
        "flat and smooth spheres have the same indexed UV corner stream");
    expect(normalDiffers,
        "flat and smooth spheres differ in their exported corner normals");
}
}

int main(int argumentCount, char** arguments)
{
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--write-cli-fixture") {
        return writeBytes(arguments[2], makeAsymmetricStream()) ? 0 : 1;
    }
    if (argumentCount == 4
        && std::string_view(arguments[1]) == "--verify-cli-outputs") {
        verifyCliOutputs(arguments[2], arguments[3]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " Blender asset compiler CLI assertion(s) failed.\n";
            return 1;
        }
        return 0;
    }
    if (argumentCount == 4
        && std::string_view(arguments[1]) == "--verify-sphere-pair") {
        verifySpherePairOutputs(arguments[2], arguments[3]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " flat/smooth sphere comparison assertion(s) failed.\n";
            return 1;
        }
        std::cout << "Flat/smooth sphere OGFx comparison passed.\n";
        return 0;
    }
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--verify-pyramid-output") {
        verifyPyramidOutput(arguments[2]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " test_pyramid offline proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "test_pyramid OGFx output verification passed.\n";
        return 0;
    }
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--verify-leaf-card-output") {
        verifyLeafCardOutput(arguments[2]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " test_leaf_card offline proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "test_leaf_card OGFx output verification passed.\n";
        return 0;
    }
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--verify-leaf-texture") {
        verifyLeafTexture(arguments[2]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " leaf texture proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "Leaf DDS alpha verification passed.\n";
        return 0;
    }
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--verify-remade-barrel-output") {
        verifyRemadeBarrelOutput(arguments[2]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " remade barrel offline proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "Remade barrel OGFx output verification passed.\n";
        return 0;
    }
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--verify-remade-barrel-texture") {
        verifyRemadeBarrelTexture(arguments[2]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " remade barrel texture proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "Remade barrel DDS verification passed.\n";
        return 0;
    }
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--verify-custom-barrel-output") {
        verifyCustomBarrelOutput(arguments[2]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " custom barrel offline proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "Custom barrel OGFx output verification passed.\n";
        return 0;
    }
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--verify-custom-barrel-texture") {
        verifyCustomBarrelTexture(arguments[2]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " custom barrel texture proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "Custom barrel DDS verification passed.\n";
        return 0;
    }
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--verify-sphere-output") {
        verifySphereOutput(arguments[2]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " test_sphere offline proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "test_sphere OGFx output verification passed.\n";
        return 0;
    }
    if (argumentCount == 3
        && std::string_view(arguments[1]) == "--verify-smooth-sphere-output") {
        verifySmoothSphereOutput(arguments[2]);
        if (failureCount != 0) {
            std::cerr << failureCount
                      << " test_smooth_sphere offline proof assertion(s) failed.\n";
            return 1;
        }
        std::cout << "test_smooth_sphere OGFx output verification passed.\n";
        return 0;
    }
    if (argumentCount != 1) {
        std::cerr
            << "Usage: xrPhotonBlenderMeshTests\n"
            << "       xrPhotonBlenderMeshTests --write-cli-fixture <path|->\n"
            << "       xrPhotonBlenderMeshTests --verify-cli-outputs <first> <second>\n"
            << "       xrPhotonBlenderMeshTests --verify-pyramid-output <ogfx>\n"
            << "       xrPhotonBlenderMeshTests --verify-leaf-card-output <ogfx>\n"
            << "       xrPhotonBlenderMeshTests --verify-leaf-texture <dds>\n"
            << "       xrPhotonBlenderMeshTests --verify-remade-barrel-output <ogfx>\n"
            << "       xrPhotonBlenderMeshTests --verify-remade-barrel-texture <dds>\n"
            << "       xrPhotonBlenderMeshTests --verify-custom-barrel-output <ogfx>\n"
            << "       xrPhotonBlenderMeshTests --verify-custom-barrel-texture <dds>\n"
            << "       xrPhotonBlenderMeshTests --verify-sphere-output <ogfx>\n"
            << "       xrPhotonBlenderMeshTests --verify-smooth-sphere-output <ogfx>\n"
            << "       xrPhotonBlenderMeshTests --verify-sphere-pair <flat> <smooth>\n";
        return 1;
    }

    testAsymmetricAxisMapAndCcwWinding();
    testUvPreservationAndVertexDeduplication();
    testAlphaTestedMaterialAndTextureVNormalization();
    testOpaqueTexturedMaterialAndTextureVNormalization();
    testNonuniformTransformAndUnitScale();
    testNegativeDeterminantKeepsSourceOrder();
    testShearedTransformUsesFullInverseTranspose();
    testFramingAndHeaderRejections();
    testTexturedHeaderRejections();
    testPayloadAndGeometryRejections();
    testCanonicalWriterSchemaRuntimeAndDeterminism();

    if (failureCount != 0) {
        std::cerr << failureCount
                  << " Blender mesh adapter test assertion(s) failed.\n";
        return 1;
    }
    std::cout << "Blender mesh adapter tests passed.\n";
    return 0;
}
