#include "blender_mesh.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace xrphoton::blender_mesh
{
namespace
{
static_assert(
    static_cast<std::uint64_t>(MaximumTriangleCount) * CornersPerTriangle
    <= std::numeric_limits<std::uint32_t>::max());

struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct RawCorner
{
    Vec3 position;
    Vec3 normal;
    float u = 0.0f;
    float v = 0.0f;
};

struct ConvertedCorner
{
    ogfx::Position position;
    ogfx::VertexAttributes attributes;
};

struct VertexKey
{
    std::array<std::uint32_t, 8> words{};
    auto operator<=>(const VertexKey&) const = default;
};

struct Matrix3x4
{
    std::array<double, 12> values{};
};

class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> bytes)
        : bytes_(bytes)
    {
    }

    [[nodiscard]] std::uint32_t readU32()
    {
        const std::uint32_t value =
            static_cast<std::uint32_t>(bytes_[offset_])
            | (static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes_[offset_ + 3]) << 24);
        offset_ += 4;
        return value;
    }

    [[nodiscard]] float readF32()
    {
        return std::bit_cast<float>(readU32());
    }

    [[nodiscard]] Vec3 readVec3()
    {
        return {
            .x = readF32(),
            .y = readF32(),
            .z = readF32(),
        };
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

ogfx::DecodeResult failure(
    std::string_view diagnosticName,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    return {
        .model = {},
        .error = std::string(diagnosticName)
            + ": Blender mesh adapter: XRBM field " + std::string(field)
            + ": expected " + std::string(expected)
            + ", found " + std::string(found) + '.',
    };
}

bool finite(const Vec3& value)
{
    return std::isfinite(value.x)
        && std::isfinite(value.y)
        && std::isfinite(value.z);
}

double determinant(const Matrix3x4& matrix)
{
    const auto& m = matrix.values;
    return m[0] * (m[5] * m[10] - m[6] * m[9])
        - m[1] * (m[4] * m[10] - m[6] * m[8])
        + m[2] * (m[4] * m[9] - m[5] * m[8]);
}

Vec3 transformPosition(
    const Matrix3x4& matrix,
    double unitScale,
    const Vec3& position)
{
    const auto& m = matrix.values;
    const Vec3 blenderWorld{
        .x = unitScale * (m[0] * position.x + m[1] * position.y
            + m[2] * position.z + m[3]),
        .y = unitScale * (m[4] * position.x + m[5] * position.y
            + m[6] * position.z + m[7]),
        .z = unitScale * (m[8] * position.x + m[9] * position.y
            + m[10] * position.z + m[11]),
    };
    // Blender is X-right/Y-forward/Z-up and right-handed. Engine space is
    // X-right/Y-up/Z-forward and left-handed, so swapping Y and Z is the one
    // offline axis conversion. Its negative determinant is handled in winding.
    return {blenderWorld.x, blenderWorld.z, blenderWorld.y};
}

Vec3 transformNormal(
    const Matrix3x4& matrix,
    double inverseDeterminant,
    const Vec3& normal)
{
    const auto& m = matrix.values;
    // Build A^-1 explicitly, then multiply by its transpose. Translation and
    // the positive scene unit scale do not affect normal direction.
    const std::array<double, 9> inverse{
        (m[5] * m[10] - m[6] * m[9]) * inverseDeterminant,
        (m[2] * m[9] - m[1] * m[10]) * inverseDeterminant,
        (m[1] * m[6] - m[2] * m[5]) * inverseDeterminant,
        (m[6] * m[8] - m[4] * m[10]) * inverseDeterminant,
        (m[0] * m[10] - m[2] * m[8]) * inverseDeterminant,
        (m[2] * m[4] - m[0] * m[6]) * inverseDeterminant,
        (m[4] * m[9] - m[5] * m[8]) * inverseDeterminant,
        (m[1] * m[8] - m[0] * m[9]) * inverseDeterminant,
        (m[0] * m[5] - m[1] * m[4]) * inverseDeterminant,
    };
    const Vec3 blenderWorld{
        .x = inverse[0] * normal.x + inverse[3] * normal.y
            + inverse[6] * normal.z,
        .y = inverse[1] * normal.x + inverse[4] * normal.y
            + inverse[7] * normal.z,
        .z = inverse[2] * normal.x + inverse[5] * normal.y
            + inverse[8] * normal.z,
    };
    Vec3 engine{blenderWorld.x, blenderWorld.z, blenderWorld.y};
    const double lengthSquared =
        engine.x * engine.x + engine.y * engine.y + engine.z * engine.z;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-24) {
        return {
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN(),
        };
    }
    const double inverseLength = 1.0 / std::sqrt(lengthSquared);
    engine.x *= inverseLength;
    engine.y *= inverseLength;
    engine.z *= inverseLength;
    return engine;
}

float toCanonicalF32(double value)
{
    const float converted = static_cast<float>(value);
    return converted == 0.0f ? 0.0f : converted;
}

bool convertCorner(
    const RawCorner& source,
    const Matrix3x4& matrix,
    double unitScale,
    double inverseDeterminant,
    ConvertedCorner* converted)
{
    const Vec3 position = transformPosition(matrix, unitScale, source.position);
    const Vec3 normal = transformNormal(matrix, inverseDeterminant, source.normal);
    if (!finite(position) || !finite(normal)) {
        return false;
    }

    converted->position = {
        toCanonicalF32(position.x),
        toCanonicalF32(position.y),
        toCanonicalF32(position.z),
    };
    converted->attributes = {
        toCanonicalF32(normal.x),
        toCanonicalF32(normal.y),
        toCanonicalF32(normal.z),
        source.u == 0.0f ? 0.0f : source.u,
        source.v == 0.0f ? 0.0f : source.v,
    };
    return std::isfinite(converted->position.x)
        && std::isfinite(converted->position.y)
        && std::isfinite(converted->position.z)
        && std::isfinite(converted->attributes.nx)
        && std::isfinite(converted->attributes.ny)
        && std::isfinite(converted->attributes.nz);
}

Vec3 subtract(const ogfx::Position& left, const ogfx::Position& right)
{
    return {
        static_cast<double>(left.x) - right.x,
        static_cast<double>(left.y) - right.y,
        static_cast<double>(left.z) - right.z,
    };
}

Vec3 cross(const Vec3& left, const Vec3& right)
{
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

double dot(const Vec3& left, const ogfx::VertexAttributes& right)
{
    return left.x * right.nx + left.y * right.ny + left.z * right.nz;
}

VertexKey keyFor(const ConvertedCorner& corner)
{
    return {{
        std::bit_cast<std::uint32_t>(corner.position.x),
        std::bit_cast<std::uint32_t>(corner.position.y),
        std::bit_cast<std::uint32_t>(corner.position.z),
        std::bit_cast<std::uint32_t>(corner.attributes.nx),
        std::bit_cast<std::uint32_t>(corner.attributes.ny),
        std::bit_cast<std::uint32_t>(corner.attributes.nz),
        std::bit_cast<std::uint32_t>(corner.attributes.u),
        std::bit_cast<std::uint32_t>(corner.attributes.v),
    }};
}

std::string indexedField(
    std::uint32_t triangle,
    std::uint32_t corner,
    std::string_view field)
{
    return "triangles[" + std::to_string(triangle) + "].corners["
        + std::to_string(corner) + "]." + std::string(field);
}
}

ogfx::DecodeResult decodeStaticMesh(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName)
{
    if (bytes.size() < StreamHeaderSize) {
        return failure(
            diagnosticName,
            "byte size",
            "at least " + std::to_string(StreamHeaderSize),
            std::to_string(bytes.size()));
    }
    if (!std::equal(StreamMagic.begin(), StreamMagic.end(), bytes.begin())) {
        return failure(diagnosticName, "magic", "XRBM", "another value");
    }

    Reader reader(bytes.subspan(4));
    const std::uint32_t version = reader.readU32();
    if (version != StreamVersion) {
        return failure(
            diagnosticName,
            "version",
            std::to_string(StreamVersion),
            std::to_string(version));
    }
    const std::uint32_t headerSize = reader.readU32();
    if (headerSize != StreamHeaderSize) {
        return failure(
            diagnosticName,
            "header byte size",
            std::to_string(StreamHeaderSize),
            std::to_string(headerSize));
    }
    const std::uint32_t flags = reader.readU32();
    if ((flags & ~SupportedStreamFlags) != 0) {
        return failure(
            diagnosticName,
            "flags",
            "only bit 0 (UVs) may be set",
            std::to_string(flags));
    }
    const std::uint32_t triangleCount = reader.readU32();
    if (triangleCount == 0) {
        return failure(diagnosticName, "triangle count", "at least 1", "0");
    }

    const std::uint32_t blenderMajor = reader.readU32();
    const std::uint32_t blenderMinor = reader.readU32();
    const std::uint32_t blenderPatch = reader.readU32();
    if (blenderMajor != 5 || blenderMinor < 1) {
        return failure(
            diagnosticName,
            "Blender version",
            "5.1.x or newer within major version 5",
            std::to_string(blenderMajor) + '.' + std::to_string(blenderMinor)
                + '.' + std::to_string(blenderPatch));
    }

    const float unitScale = reader.readF32();
    if (!std::isfinite(unitScale) || unitScale <= 0.0f) {
        return failure(
            diagnosticName,
            "scene unit scale",
            "a positive finite f32",
            std::to_string(unitScale));
    }
    const std::uint32_t firstReserved = reader.readU32();
    Matrix3x4 matrix{};
    for (double& value : matrix.values) {
        value = reader.readF32();
        if (!std::isfinite(value)) {
            return failure(
                diagnosticName,
                "object transform",
                "12 finite affine f32 values",
                "a non-finite value");
        }
    }
    const std::uint32_t secondReserved = reader.readU32();
    const std::uint32_t thirdReserved = reader.readU32();
    if (firstReserved != 0 || secondReserved != 0 || thirdReserved != 0) {
        return failure(
            diagnosticName,
            "reserved header words",
            "all zero",
            "a nonzero value");
    }

    constexpr std::uint64_t TriangleRecordSize =
        static_cast<std::uint64_t>(CornersPerTriangle) * CornerRecordSize;
    const std::uint64_t sizeLimitedTriangleCount =
        (ogfx::MaximumFileBytes - StreamHeaderSize) / TriangleRecordSize;
    const std::uint64_t maximumTriangleCount = std::min<std::uint64_t>(
        MaximumTriangleCount, sizeLimitedTriangleCount);
    if (triangleCount > maximumTriangleCount) {
        return failure(
            diagnosticName,
            "triangle count",
            "at most " + std::to_string(maximumTriangleCount),
            std::to_string(triangleCount));
    }
    const std::uint64_t expectedSize = StreamHeaderSize
        + static_cast<std::uint64_t>(triangleCount) * TriangleRecordSize;
    if (bytes.size() != expectedSize) {
        return failure(
            diagnosticName,
            "byte size",
            std::to_string(expectedSize) + " from the triangle count",
            std::to_string(bytes.size()));
    }

    const double transformDeterminant = determinant(matrix);
    if (!std::isfinite(transformDeterminant)
        || std::abs(transformDeterminant) <= 1.0e-12) {
        return failure(
            diagnosticName,
            "object transform determinant",
            "finite with absolute value greater than 1e-12",
            std::to_string(transformDeterminant));
    }
    const double inverseDeterminant = 1.0 / transformDeterminant;
    // The Blender-to-engine axis swap has determinant -1. Reverse exactly when
    // the complete transform reflects orientation.
    const bool reverseWinding = transformDeterminant > 0.0;

    ogfx::Model model{};
    const std::uint64_t cornerCount =
        static_cast<std::uint64_t>(triangleCount) * CornersPerTriangle;
    model.positions.reserve(static_cast<std::size_t>(cornerCount));
    model.attributes.reserve(static_cast<std::size_t>(cornerCount));
    model.indices.reserve(static_cast<std::size_t>(cornerCount));
    std::map<VertexKey, std::uint32_t> vertexIndices;

    for (std::uint32_t triangleIndex = 0;
         triangleIndex < triangleCount;
         ++triangleIndex) {
        std::array<RawCorner, CornersPerTriangle> source{};
        for (std::uint32_t cornerIndex = 0;
             cornerIndex < CornersPerTriangle;
             ++cornerIndex) {
            RawCorner& corner = source[cornerIndex];
            corner.position = reader.readVec3();
            corner.normal = reader.readVec3();
            corner.u = reader.readF32();
            corner.v = reader.readF32();
            if (!finite(corner.position)) {
                return failure(
                    diagnosticName,
                    indexedField(triangleIndex, cornerIndex, "position"),
                    "three finite f32 values",
                    "a non-finite value");
            }
            if (!finite(corner.normal)) {
                return failure(
                    diagnosticName,
                    indexedField(triangleIndex, cornerIndex, "normal"),
                    "three finite f32 values",
                    "a non-finite value");
            }
            if (!std::isfinite(corner.u) || !std::isfinite(corner.v)) {
                return failure(
                    diagnosticName,
                    indexedField(triangleIndex, cornerIndex, "UV"),
                    "two finite f32 values",
                    "a non-finite value");
            }
            if ((flags & StreamFlagHasUvs) == 0
                && (corner.u != 0.0f || corner.v != 0.0f)) {
                return failure(
                    diagnosticName,
                    indexedField(triangleIndex, cornerIndex, "UV"),
                    "(0, 0) when the UV flag is clear",
                    "a nonzero value");
            }
        }

        std::array<ConvertedCorner, CornersPerTriangle> converted{};
        const std::array<std::uint32_t, CornersPerTriangle> order =
            reverseWinding
            ? std::array<std::uint32_t, CornersPerTriangle>{0, 2, 1}
            : std::array<std::uint32_t, CornersPerTriangle>{0, 1, 2};
        for (std::uint32_t outputCorner = 0;
             outputCorner < CornersPerTriangle;
             ++outputCorner) {
            const std::uint32_t sourceCorner = order[outputCorner];
            if (!convertCorner(
                    source[sourceCorner],
                    matrix,
                    unitScale,
                    inverseDeterminant,
                    &converted[outputCorner])) {
                return failure(
                    diagnosticName,
                    indexedField(triangleIndex, sourceCorner, "transformed data"),
                    "finite f32 position and normalized normal values",
                    "an unrepresentable value");
            }
        }

        const Vec3 edge1 = subtract(
            converted[1].position, converted[0].position);
        const Vec3 edge2 = subtract(
            converted[2].position, converted[0].position);
        const Vec3 geometricNormal = cross(edge1, edge2);
        const double areaSquared = geometricNormal.x * geometricNormal.x
            + geometricNormal.y * geometricNormal.y
            + geometricNormal.z * geometricNormal.z;
        if (!std::isfinite(areaSquared) || areaSquared <= 1.0e-24) {
            return failure(
                diagnosticName,
                "triangles[" + std::to_string(triangleIndex) + "].area",
                "a nondegenerate triangle after f32 conversion",
                std::to_string(areaSquared));
        }
        for (std::uint32_t cornerIndex = 0;
             cornerIndex < CornersPerTriangle;
             ++cornerIndex) {
            const double agreement =
                dot(geometricNormal, converted[cornerIndex].attributes);
            if (!std::isfinite(agreement) || agreement <= 0.0) {
                return failure(
                    diagnosticName,
                    indexedField(triangleIndex, cornerIndex, "normal orientation"),
                    "positive agreement with counter-clockwise triangle winding",
                    std::to_string(agreement));
            }
        }

        for (const ConvertedCorner& corner : converted) {
            const VertexKey key = keyFor(corner);
            const auto existing = vertexIndices.find(key);
            if (existing != vertexIndices.end()) {
                model.indices.push_back(existing->second);
                continue;
            }
            const std::uint32_t vertexIndex =
                static_cast<std::uint32_t>(model.positions.size());
            vertexIndices.emplace(key, vertexIndex);
            model.positions.push_back(corner.position);
            model.attributes.push_back(corner.attributes);
            model.indices.push_back(vertexIndex);
        }
    }

    model.geometries.push_back({
        .firstVertex = 0,
        .vertexCount = static_cast<std::uint32_t>(model.positions.size()),
        .firstIndex = 0,
        .indexCount = static_cast<std::uint32_t>(model.indices.size()),
        .materialIndex = 0,
        .alphaTested = false,
    });
    model.meshes.push_back({.firstGeometry = 0, .geometryCount = 1});
    model.materials.push_back({});
    return {.model = std::move(model), .error = {}};
}
}
