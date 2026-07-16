#include "ogfx.hpp"

#include "ogfx_detail.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <sstream>
#include <utility>

namespace xrphoton::ogfx
{
namespace
{
using detail::Bounds;
using detail::indexedField;
using detail::positionIsFinite;

struct PreparedModel
{
    Bounds bounds{};
    Position sphereCenter{};
    float sphereRadius = 0.0f;
    std::vector<Bounds> geometryBounds;
};

SerializeResult failure(
    std::string_view diagnosticName,
    ChunkId chunk,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    return {
        .bytes = {},
        .error = detail::makeChunkDiagnostic(
            diagnosticName,
            "writer",
            static_cast<std::uint32_t>(chunk),
            field,
            expected,
            found),
    };
}

std::string quoteText(std::string_view value)
{
    std::ostringstream text;
    text << '"' << value << '"';
    return text.str();
}

bool fitsU32(std::size_t value)
{
    return value <= std::numeric_limits<std::uint32_t>::max();
}

Bounds boundsForRange(const Model& model, std::uint32_t firstVertex, std::uint32_t vertexCount)
{
    Bounds bounds{
        .minimum = model.positions[firstVertex],
        .maximum = model.positions[firstVertex],
    };

    const std::uint64_t end = static_cast<std::uint64_t>(firstVertex) + vertexCount;
    for (std::uint64_t index = static_cast<std::uint64_t>(firstVertex) + 1;
         index < end;
         ++index) {
        const Position& position = model.positions[static_cast<std::size_t>(index)];
        bounds.minimum.x = std::min(bounds.minimum.x, position.x);
        bounds.minimum.y = std::min(bounds.minimum.y, position.y);
        bounds.minimum.z = std::min(bounds.minimum.z, position.z);
        bounds.maximum.x = std::max(bounds.maximum.x, position.x);
        bounds.maximum.y = std::max(bounds.maximum.y, position.y);
        bounds.maximum.z = std::max(bounds.maximum.z, position.z);
    }

    return bounds;
}

SerializeResult prepareModel(
    const Model& model,
    std::string_view diagnosticName,
    PreparedModel* prepared)
{
    if (model.positions.empty()) {
        return failure(diagnosticName, ChunkId::Positions, "element count", "at least 1", "0");
    }
    if (model.attributes.empty()) {
        return failure(diagnosticName, ChunkId::Attributes, "element count", "at least 1", "0");
    }
    if (model.indices.empty()) {
        return failure(diagnosticName, ChunkId::Indices, "element count", "at least 1", "0");
    }
    if (model.geometries.empty()) {
        return failure(diagnosticName, ChunkId::Geometries, "record count", "at least 1", "0");
    }
    if (model.meshes.empty()) {
        return failure(diagnosticName, ChunkId::Meshes, "record count", "at least 1", "0");
    }
    if (model.materials.empty()) {
        return failure(diagnosticName, ChunkId::Materials, "materialCount", "at least 1", "0");
    }

    if (!fitsU32(model.positions.size())) {
        return failure(
            diagnosticName,
            ChunkId::Positions,
            "element count",
            "u32",
            std::to_string(model.positions.size()));
    }
    if (!fitsU32(model.attributes.size())) {
        return failure(
            diagnosticName,
            ChunkId::Attributes,
            "element count",
            "u32",
            std::to_string(model.attributes.size()));
    }
    if (!fitsU32(model.indices.size())) {
        return failure(diagnosticName, ChunkId::Indices, "element count", "u32", std::to_string(model.indices.size()));
    }
    if (!fitsU32(model.geometries.size())) {
        return failure(
            diagnosticName,
            ChunkId::Geometries,
            "record count",
            "u32",
            std::to_string(model.geometries.size()));
    }
    if (!fitsU32(model.meshes.size())) {
        return failure(diagnosticName, ChunkId::Meshes, "record count", "u32", std::to_string(model.meshes.size()));
    }
    if (!fitsU32(model.materials.size())) {
        return failure(
            diagnosticName,
            ChunkId::Materials,
            "materialCount",
            "u32",
            std::to_string(model.materials.size()));
    }

    if (model.positions.size() != model.attributes.size()) {
        return failure(
            diagnosticName,
            ChunkId::Attributes,
            "element count",
            std::to_string(model.positions.size()) + " (position count)",
            std::to_string(model.attributes.size()));
    }

    for (std::size_t index = 0; index < model.positions.size(); ++index) {
        if (!positionIsFinite(model.positions[index])) {
            return failure(
                diagnosticName,
                ChunkId::Positions,
                indexedField("positions", index, "x/y/z"),
                "finite f32 values",
                "a non-finite value");
        }

        const VertexAttributes& attributes = model.attributes[index];
        if (!std::isfinite(attributes.nx)
            || !std::isfinite(attributes.ny)
            || !std::isfinite(attributes.nz)
            || !std::isfinite(attributes.u)
            || !std::isfinite(attributes.v)) {
            return failure(
                diagnosticName,
                ChunkId::Attributes,
                indexedField("attributes", index, "nx/ny/nz/u/v"),
                "finite f32 values",
                "a non-finite value");
        }

        const double nx = attributes.nx;
        const double ny = attributes.ny;
        const double nz = attributes.nz;
        const double normalLengthSquared = nx * nx + ny * ny + nz * nz;
        if (!std::isfinite(normalLengthSquared) || normalLengthSquared < 1.0e-12) {
            return failure(
                diagnosticName,
                ChunkId::Attributes,
                indexedField("attributes", index, "normal length squared"),
                "finite and at least 1e-12",
                std::to_string(normalLengthSquared));
        }
    }

    for (std::size_t index = 0; index < model.materials.size(); ++index) {
        const Material& material = model.materials[index];
        for (std::size_t component = 0; component < material.baseColorFactor.size(); ++component) {
            if (!std::isfinite(material.baseColorFactor[component])) {
                return failure(
                    diagnosticName,
                    ChunkId::Materials,
                    indexedField("materials", index, "baseColorFactor")
                        + '[' + std::to_string(component) + ']',
                    "a finite f32",
                    "a non-finite value");
            }
        }
        if (!std::isfinite(material.alphaCutoff)) {
            return failure(
                diagnosticName,
                ChunkId::Materials,
                indexedField("materials", index, "alphaCutoff"),
                "a finite f32",
                "a non-finite value");
        }
        if (!material.baseColorTexture.empty()) {
            return failure(
                diagnosticName,
                ChunkId::Materials,
                indexedField("materials", index, "baseColorTexture"),
                "no texture reference in the M4 writer profile",
                quoteText(material.baseColorTexture));
        }
    }

    prepared->geometryBounds.clear();
    prepared->geometryBounds.reserve(model.geometries.size());

    std::uint64_t expectedFirstVertex = 0;
    std::uint64_t expectedFirstIndex = 0;
    for (std::size_t geometryIndex = 0; geometryIndex < model.geometries.size(); ++geometryIndex) {
        const Geometry& geometry = model.geometries[geometryIndex];
        if (geometry.vertexCount == 0) {
            return failure(
                diagnosticName,
                ChunkId::Geometries,
                indexedField("geometries", geometryIndex, "vertexCount"),
                "at least 1",
                "0");
        }
        if (geometry.indexCount == 0 || (geometry.indexCount % 3) != 0) {
            return failure(
                diagnosticName,
                ChunkId::Geometries,
                indexedField("geometries", geometryIndex, "indexCount"),
                "a nonzero multiple of 3",
                std::to_string(geometry.indexCount));
        }
        if (geometry.firstVertex != expectedFirstVertex) {
            return failure(
                diagnosticName,
                ChunkId::Geometries,
                indexedField("geometries", geometryIndex, "firstVertex"),
                std::to_string(expectedFirstVertex) + " (next partition offset)",
                std::to_string(geometry.firstVertex));
        }
        if (geometry.firstIndex != expectedFirstIndex) {
            return failure(
                diagnosticName,
                ChunkId::Geometries,
                indexedField("geometries", geometryIndex, "firstIndex"),
                std::to_string(expectedFirstIndex) + " (next partition offset)",
                std::to_string(geometry.firstIndex));
        }

        const std::uint64_t vertexEnd =
            static_cast<std::uint64_t>(geometry.firstVertex) + geometry.vertexCount;
        const std::uint64_t indexEnd =
            static_cast<std::uint64_t>(geometry.firstIndex) + geometry.indexCount;
        if (vertexEnd > model.positions.size()) {
            return failure(
                diagnosticName,
                ChunkId::Geometries,
                indexedField("geometries", geometryIndex, "vertex range end"),
                "at most " + std::to_string(model.positions.size()),
                std::to_string(vertexEnd));
        }
        if (indexEnd > model.indices.size()) {
            return failure(
                diagnosticName,
                ChunkId::Geometries,
                indexedField("geometries", geometryIndex, "index range end"),
                "at most " + std::to_string(model.indices.size()),
                std::to_string(indexEnd));
        }
        if (geometry.materialIndex >= model.materials.size()) {
            return failure(
                diagnosticName,
                ChunkId::Geometries,
                indexedField("geometries", geometryIndex, "materialIndex"),
                "less than " + std::to_string(model.materials.size()),
                std::to_string(geometry.materialIndex));
        }

        for (std::uint64_t index = geometry.firstIndex; index < indexEnd; ++index) {
            const std::uint32_t localIndex = model.indices[static_cast<std::size_t>(index)];
            if (localIndex >= geometry.vertexCount) {
                return failure(
                    diagnosticName,
                    ChunkId::Indices,
                    indexedField("indices", static_cast<std::size_t>(index), "geometry-local value"),
                    "less than " + std::to_string(geometry.vertexCount),
                    std::to_string(localIndex));
            }
        }

        prepared->geometryBounds.push_back(
            boundsForRange(model, geometry.firstVertex, geometry.vertexCount));
        expectedFirstVertex = vertexEnd;
        expectedFirstIndex = indexEnd;
    }

    if (expectedFirstVertex != model.positions.size()) {
        return failure(
            diagnosticName,
            ChunkId::Geometries,
            "final vertex partition end",
            std::to_string(model.positions.size()),
            std::to_string(expectedFirstVertex));
    }
    if (expectedFirstIndex != model.indices.size()) {
        return failure(
            diagnosticName,
            ChunkId::Geometries,
            "final index partition end",
            std::to_string(model.indices.size()),
            std::to_string(expectedFirstIndex));
    }

    std::uint64_t expectedFirstGeometry = 0;
    for (std::size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex) {
        const Mesh& mesh = model.meshes[meshIndex];
        if (mesh.geometryCount == 0) {
            return failure(
                diagnosticName,
                ChunkId::Meshes,
                indexedField("meshes", meshIndex, "geometryCount"),
                "at least 1",
                "0");
        }
        if (mesh.firstGeometry != expectedFirstGeometry) {
            return failure(
                diagnosticName,
                ChunkId::Meshes,
                indexedField("meshes", meshIndex, "firstGeometry"),
                std::to_string(expectedFirstGeometry) + " (next partition offset)",
                std::to_string(mesh.firstGeometry));
        }

        const std::uint64_t geometryEnd =
            static_cast<std::uint64_t>(mesh.firstGeometry) + mesh.geometryCount;
        if (geometryEnd > model.geometries.size()) {
            return failure(
                diagnosticName,
                ChunkId::Meshes,
                indexedField("meshes", meshIndex, "geometry range end"),
                "at most " + std::to_string(model.geometries.size()),
                std::to_string(geometryEnd));
        }
        expectedFirstGeometry = geometryEnd;
    }

    if (expectedFirstGeometry != model.geometries.size()) {
        return failure(
            diagnosticName,
            ChunkId::Meshes,
            "final geometry partition end",
            std::to_string(model.geometries.size()),
            std::to_string(expectedFirstGeometry));
    }

    prepared->bounds = boundsForRange(
        model,
        0,
        static_cast<std::uint32_t>(model.positions.size()));
    prepared->sphereCenter = {
        std::midpoint(prepared->bounds.minimum.x, prepared->bounds.maximum.x),
        std::midpoint(prepared->bounds.minimum.y, prepared->bounds.maximum.y),
        std::midpoint(prepared->bounds.minimum.z, prepared->bounds.maximum.z),
    };

    double maximumDistanceSquared = 0.0;
    for (const Position& position : model.positions) {
        const double dx = static_cast<double>(position.x) - prepared->sphereCenter.x;
        const double dy = static_cast<double>(position.y) - prepared->sphereCenter.y;
        const double dz = static_cast<double>(position.z) - prepared->sphereCenter.z;
        maximumDistanceSquared =
            std::max(maximumDistanceSquared, dx * dx + dy * dy + dz * dz);
    }

    constexpr double maximumFiniteF32 = std::numeric_limits<float>::max();
    constexpr double maximumFiniteF32Squared = maximumFiniteF32 * maximumFiniteF32;
    if (maximumDistanceSquared > maximumFiniteF32Squared) {
        return failure(
            diagnosticName,
            ChunkId::Model,
            "sphereRadius",
            "a finite enclosing f32",
            "a value larger than finite f32 can represent");
    }

    const double maximumDistance = std::sqrt(maximumDistanceSquared);
    if (!std::isfinite(maximumDistance) || maximumDistance > maximumFiniteF32) {
        return failure(
            diagnosticName,
            ChunkId::Model,
            "sphereRadius",
            "a finite enclosing f32",
            "a value larger than finite f32 can represent");
    }
    float radius = static_cast<float>(maximumDistance);
    // Comparing squared values preserves contributions that sqrt could round away in
    // f64 (for example, 1 + 2^-52). Advance until the stored f32 itself proves the
    // same f64 enclosure test the decoder applies.
    while (static_cast<double>(radius) * radius < maximumDistanceSquared) {
        radius = std::nextafter(radius, std::numeric_limits<float>::infinity());
        if (!std::isfinite(radius)) {
            return failure(
                diagnosticName,
                ChunkId::Model,
                "sphereRadius",
                "a finite enclosing f32",
                "a value larger than finite f32 can represent");
        }
    }
    prepared->sphereRadius = radius;

    return {};
}

void addChunkToFileSize(std::uint64_t payloadBytes, std::uint64_t* fileBytes)
{
    const std::uint64_t remainder = *fileBytes % ChunkAlignment;
    if (remainder != 0) {
        *fileBytes += ChunkAlignment - remainder;
    }
    *fileBytes += ChunkHeaderSize + payloadBytes;
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

void appendF32(std::vector<std::uint8_t>* bytes, float value)
{
    appendU32(bytes, std::bit_cast<std::uint32_t>(value));
}

void appendPosition(std::vector<std::uint8_t>* bytes, const Position& position)
{
    appendF32(bytes, position.x);
    appendF32(bytes, position.y);
    appendF32(bytes, position.z);
}

void appendBounds(std::vector<std::uint8_t>* bytes, const Bounds& bounds)
{
    appendPosition(bytes, bounds.minimum);
    appendPosition(bytes, bounds.maximum);
}

void alignOutput(std::vector<std::uint8_t>* bytes)
{
    const std::size_t remainder = bytes->size() % ChunkAlignment;
    if (remainder != 0) {
        bytes->insert(bytes->end(), ChunkAlignment - remainder, 0);
    }
}

void appendChunkHeader(
    std::vector<std::uint8_t>* bytes,
    ChunkId id,
    std::uint64_t payloadBytes)
{
    alignOutput(bytes);
    appendU32(bytes, static_cast<std::uint32_t>(id));
    appendU32(bytes, RequiredChunkFlags);
    appendU32(bytes, ChunkVersion);
    appendU32(bytes, 0);
    appendU64(bytes, payloadBytes);
    appendU64(bytes, 0);
}
}

SerializeResult serializeModel(const Model& model, std::string_view diagnosticName)
{
    PreparedModel prepared{};
    SerializeResult validation = prepareModel(model, diagnosticName, &prepared);
    if (!validation) {
        return validation;
    }

    // prepareModel has bounded every collection count to u32. Widening before
    // applying the pinned v1 strides keeps each payload and their sum below 2^40,
    // so these formulas cannot overflow u64.
    const std::uint64_t geometryBytes =
        static_cast<std::uint64_t>(model.geometries.size()) * GeometryRecordSize;
    const std::uint64_t meshBytes =
        static_cast<std::uint64_t>(model.meshes.size()) * MeshRecordSize;
    const std::uint64_t materialRecordBytes =
        static_cast<std::uint64_t>(model.materials.size()) * MaterialRecordSize;
    const std::uint64_t materialBytes = MaterialHeaderSize + materialRecordBytes;
    const std::uint64_t positionBytes =
        static_cast<std::uint64_t>(model.positions.size()) * PositionRecordSize;
    const std::uint64_t attributeBytes =
        static_cast<std::uint64_t>(model.attributes.size()) * AttributeRecordSize;
    const std::uint64_t indexBytes =
        static_cast<std::uint64_t>(model.indices.size()) * IndexRecordSize;

    std::uint64_t fileBytes = FileHeaderSize;
    addChunkToFileSize(ModelRecordSize, &fileBytes);
    addChunkToFileSize(geometryBytes, &fileBytes);
    addChunkToFileSize(meshBytes, &fileBytes);
    addChunkToFileSize(materialBytes, &fileBytes);
    addChunkToFileSize(positionBytes, &fileBytes);
    addChunkToFileSize(attributeBytes, &fileBytes);
    addChunkToFileSize(indexBytes, &fileBytes);
    if (fileBytes > MaximumFileBytes) {
        return failure(
            diagnosticName,
            ChunkId::Model,
            "file byte size",
            "at most " + std::to_string(MaximumFileBytes),
            std::to_string(fileBytes));
    }
    SerializeResult result{};
    result.bytes.reserve(static_cast<std::size_t>(fileBytes));
    result.bytes.insert(result.bytes.end(), FileMagic.begin(), FileMagic.end());
    appendU32(&result.bytes, ContainerVersion);
    appendU32(&result.bytes, FileHeaderSize);
    appendU32(&result.bytes, 0);

    appendChunkHeader(&result.bytes, ChunkId::Model, ModelRecordSize);
    appendU32(&result.bytes, NormalModelType);
    appendU32(&result.bytes, 0);
    appendBounds(&result.bytes, prepared.bounds);
    appendPosition(&result.bytes, prepared.sphereCenter);
    appendF32(&result.bytes, prepared.sphereRadius);

    appendChunkHeader(&result.bytes, ChunkId::Geometries, geometryBytes);
    for (std::size_t index = 0; index < model.geometries.size(); ++index) {
        const Geometry& geometry = model.geometries[index];
        appendU32(&result.bytes, geometry.firstVertex);
        appendU32(&result.bytes, geometry.vertexCount);
        appendU32(&result.bytes, geometry.firstIndex);
        appendU32(&result.bytes, geometry.indexCount);
        appendU32(&result.bytes, geometry.materialIndex);
        appendU32(&result.bytes, geometry.alphaTested ? GeometryFlagAlphaTested : 0);
        appendBounds(&result.bytes, prepared.geometryBounds[index]);
    }

    appendChunkHeader(&result.bytes, ChunkId::Meshes, meshBytes);
    for (const Mesh& mesh : model.meshes) {
        appendU32(&result.bytes, mesh.firstGeometry);
        appendU32(&result.bytes, mesh.geometryCount);
    }

    appendChunkHeader(&result.bytes, ChunkId::Materials, materialBytes);
    appendU32(&result.bytes, static_cast<std::uint32_t>(model.materials.size()));
    appendU32(&result.bytes, 0);
    appendU32(&result.bytes, 0);
    appendU32(&result.bytes, 0);
    for (const Material& material : model.materials) {
        for (float component : material.baseColorFactor) {
            appendF32(&result.bytes, component);
        }
        appendF32(&result.bytes, material.alphaCutoff);
        appendU32(&result.bytes, NoTextureReference);
        appendU32(&result.bytes, 0);
        appendU32(&result.bytes, 0);
    }

    appendChunkHeader(&result.bytes, ChunkId::Positions, positionBytes);
    for (const Position& position : model.positions) {
        appendPosition(&result.bytes, position);
    }

    appendChunkHeader(&result.bytes, ChunkId::Attributes, attributeBytes);
    for (const VertexAttributes& attributes : model.attributes) {
        appendF32(&result.bytes, attributes.nx);
        appendF32(&result.bytes, attributes.ny);
        appendF32(&result.bytes, attributes.nz);
        appendF32(&result.bytes, attributes.u);
        appendF32(&result.bytes, attributes.v);
    }

    appendChunkHeader(&result.bytes, ChunkId::Indices, indexBytes);
    for (std::uint32_t index : model.indices) {
        appendU32(&result.bytes, index);
    }

    if (result.bytes.size() != fileBytes) {
        return failure(
            diagnosticName,
            ChunkId::Model,
            "canonical writer byte count",
            std::to_string(fileBytes),
            std::to_string(result.bytes.size()));
    }

    return result;
}
}
