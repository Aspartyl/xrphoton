#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xrphoton::ogfx
{
inline constexpr std::array<std::uint8_t, 4> FileMagic{'O', 'G', 'F', 'X'};
inline constexpr std::uint32_t ContainerVersion = 1;
inline constexpr std::uint32_t FileHeaderSize = 16;
inline constexpr std::uint32_t ChunkHeaderSize = 32;
inline constexpr std::uint32_t RequiredChunkFlags = 1;
inline constexpr std::uint32_t ChunkVersion = 1;
inline constexpr std::uint32_t ChunkAlignment = 16;
inline constexpr std::uint32_t NormalModelType = 0;
inline constexpr std::uint32_t GeometryFlagAlphaTested = 1;
inline constexpr std::uint32_t NoTextureReference =
    std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t MaximumStringBytes = 4096;
inline constexpr std::uint32_t MaximumChunkCount = 4096;
inline constexpr std::uint64_t MaximumFileBytes = 1ull << 30;
// Decoding materializes one owning string per referenced material. Keep duplicate-
// reference expansion well below the independent file-size cap in both profiles.
inline constexpr std::uint64_t MaximumDecodedTextureBytes = 64ull << 20;

inline constexpr std::uint32_t ModelRecordSize = 48;
inline constexpr std::uint32_t GeometryRecordSize = 48;
inline constexpr std::uint32_t MeshRecordSize = 8;
inline constexpr std::uint32_t MaterialHeaderSize = 16;
inline constexpr std::uint32_t MaterialRecordSize = 32;
inline constexpr std::uint32_t PositionRecordSize = 12;
inline constexpr std::uint32_t AttributeRecordSize = 20;
inline constexpr std::uint32_t IndexRecordSize = 4;

enum class ChunkId : std::uint32_t
{
    Model = 0x0001,
    Geometries = 0x0010,
    Meshes = 0x0011,
    Materials = 0x0012,
    Positions = 0x0020,
    Attributes = 0x0021,
    Indices = 0x0022,
    Description = 0x0040,
};

// This compiler-facing model deliberately owns no Vulkan, GLM, instance, or file-I/O
// state. Source adapters populate it; the one canonical writer owns the disk schema.
struct Position
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct VertexAttributes
{
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

struct Geometry
{
    std::uint32_t firstVertex = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t materialIndex = 0;
    bool alphaTested = false;
};

struct Mesh
{
    std::uint32_t firstGeometry = 0;
    std::uint32_t geometryCount = 0;
};

struct Material
{
    std::array<float, 4> baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    float alphaCutoff = 0.5f;

    // OGFx v1 stores logical texture references in its string arena. Both decoder
    // profiles preserve them so the runtime texture resolver can load the image.
    std::string baseColorTexture;
};

struct Model
{
    std::vector<Position> positions;
    std::vector<VertexAttributes> attributes;
    std::vector<std::uint32_t> indices;
    std::vector<Geometry> geometries;
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
};

struct SerializeResult
{
    std::vector<std::uint8_t> bytes;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error.empty();
    }
};

struct DecodeResult
{
    Model model;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return error.empty();
    }
};

// Produces the canonical version-1 byte stream. diagnosticName is carried only into
// errors; it is never serialized, so identical models always produce identical bytes.
[[nodiscard]] SerializeResult serializeModel(
    const Model& model,
    std::string_view diagnosticName = "<memory>");

// Decodes and validates the complete version-1 static schema within its published
// resource caps, including logical texture references and alpha-tested geometry
// beyond the current runtime capability gates. This is the offline round-trip /
// inspection entry point; it does not make the model runtime-ready.
[[nodiscard]] DecodeResult decodeModelSchema(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName = "<memory>");

// Decodes the staged runtime profile transactionally: failure returns one diagnostic
// and no partially populated model. Logical texture references are reconstructed for
// the scene resolver; alpha-tested geometry remains gated until the opaque/alpha
// split. The returned model deliberately has no instance concept because OGFx stores
// reusable model data, not world placement.
[[nodiscard]] DecodeResult decodeModel(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName = "<memory>");
}
