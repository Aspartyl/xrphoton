#include "legacy_ogf.hpp"

#include "ogfx_detail.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xrphoton::legacy_ogf
{
namespace
{
constexpr std::uint32_t HeaderChunkId = 0x1;
constexpr std::uint32_t TextureChunkId = 0x2;
constexpr std::uint32_t VerticesChunkId = 0x3;
constexpr std::uint32_t IndicesChunkId = 0x4;
constexpr std::uint32_t ChildrenChunkId = 0x9;
constexpr std::uint32_t BoneNamesChunkId = 0xD;
constexpr std::uint32_t IkDataChunkId = 0x10;
constexpr std::uint32_t DescriptionChunkId = 0x12;
constexpr std::uint32_t CompressionMark = 0x80000000u;
constexpr std::uint32_t LegacyChunkHeaderSize = 8;
constexpr std::uint32_t HeaderPayloadSize = 44;
constexpr std::uint32_t BoneObbBytes = 60;
constexpr std::uint32_t BoneShapeBytes = 112;
constexpr std::uint32_t JointDataBytes = 76;
constexpr std::uint32_t IkFixedBytes = 4 + BoneShapeBytes + JointDataBytes + 24 + 4 + 12;
constexpr std::uint16_t CylinderShapeType = 3;
constexpr std::uint32_t RigidJointType = 0;
constexpr std::uint32_t BreakableJointFlag = 1;

struct ChunkView
{
    std::uint32_t id = 0;
    std::span<const std::uint8_t> payload;
};

struct SourceBounds
{
    ogfx::Position minimum{};
    ogfx::Position maximum{};
    ogfx::Position sphereCenter{};
    float sphereRadius = 0.0f;
};

struct SourceCylinder
{
    ogfx::Position center{};
    ogfx::Position direction{};
    float height = 0.0f;
    float radius = 0.0f;
};

struct Bone
{
    std::string name;
    std::string parentName;
    std::optional<std::uint32_t> parent;
    std::string material;
    std::uint32_t shapeFlags = 0;
    SourceCylinder cylinder{};
    ogfx::Position bindRotation{};
    ogfx::Position bindTranslation{};
    ogfx::Position globalTranslation{};
    float mass = 0.0f;
    ogfx::Position centerOfMass{};
};

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

float readF32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return std::bit_cast<float>(readU32(bytes, offset));
}

ogfx::Position readPosition(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return {
        readF32(bytes, offset),
        readF32(bytes, offset + 4),
        readF32(bytes, offset + 8),
    };
}

std::string hexadecimal(std::uint64_t value)
{
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}

std::string lowerAscii(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char byte) {
        return byte >= 'A' && byte <= 'Z'
            ? static_cast<char>(byte - 'A' + 'a')
            : static_cast<char>(byte);
    });
    return result;
}

std::string makeRigidDiagnostic(
    std::string_view diagnosticName,
    std::string_view scope,
    std::uint32_t chunkId,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    std::ostringstream message;
    message << diagnosticName << ": legacy OGF rigid decoder: " << scope
            << " chunk " << hexadecimal(chunkId) << ", field " << field
            << ": expected " << expected << ", found " << found << '.';
    return message.str();
}

std::string makeDispatchDiagnostic(
    std::string_view diagnosticName,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    return std::string(diagnosticName)
        + ": legacy OGF decoder dispatch: OGF_FILE, field "
        + std::string(field) + ": expected " + std::string(expected)
        + ", found " + std::string(found) + '.';
}

class RigidDecoder
{
public:
    RigidDecoder(std::span<const std::uint8_t> bytes, std::string_view diagnosticName)
        : bytes_(bytes)
        , diagnosticName_(diagnosticName)
    {
    }

    ogfx::DecodeResult run()
    {
        SourceBounds rootBounds{};
        if (!scanTopLevel()
            || !decodeHeader(
                *find(topChunks_, HeaderChunkId),
                SupportedRigidModelType,
                "root",
                &rootBounds)
            || !decodeDescription()
            || !decodeBones()
            || !decodeIkData()
            || !decodeChildren()
            || !validateBounds(rootBounds, 0, model_.positions.size(), "root")
            || !buildRigidPhysics()) {
            return failedResult();
        }

        model_.meshes.push_back({
            .firstGeometry = 0,
            .geometryCount = static_cast<std::uint32_t>(model_.geometries.size()),
        });
        return {
            .model = std::move(model_),
            .error = {},
        };
    }

private:
    bool reject(
        std::string_view scope,
        std::uint32_t chunkId,
        std::string field,
        std::string expected,
        std::string found)
    {
        error_ = makeRigidDiagnostic(
            diagnosticName_, scope, chunkId, field, expected, found);
        return false;
    }

    ogfx::DecodeResult failedResult()
    {
        return {
            .model = {},
            .error = std::move(error_),
        };
    }

    static const ChunkView* find(
        const std::vector<ChunkView>& chunks,
        std::uint32_t id)
    {
        const auto found = std::find_if(
            chunks.begin(), chunks.end(),
            [id](const ChunkView& chunk) { return chunk.id == id; });
        return found == chunks.end() ? nullptr : &*found;
    }

    bool scanChunks(
        std::span<const std::uint8_t> bytes,
        std::span<const std::uint32_t> allowedIds,
        std::string_view scope,
        std::vector<ChunkView>* chunks)
    {
        if (bytes.size() < LegacyChunkHeaderSize) {
            return reject(
                scope,
                0,
                "byteSize",
                "at least one complete 8-byte chunk header",
                std::to_string(bytes.size()));
        }

        std::unordered_set<std::uint32_t> seen;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t remaining = bytes.size() - offset;
            if (remaining < LegacyChunkHeaderSize) {
                return reject(
                    scope,
                    0,
                    "trailing bytes",
                    "a complete 8-byte chunk header",
                    std::to_string(remaining));
            }
            const std::uint32_t rawId = readU32(bytes, offset);
            const std::uint32_t id = rawId & ~CompressionMark;
            const std::uint32_t payloadSize = readU32(bytes, offset + 4);
            if ((rawId & CompressionMark) != 0) {
                return reject(
                    scope,
                    id,
                    "compression flag",
                    "clear in the pinned rigid profile",
                    "set in " + hexadecimal(rawId));
            }
            if (std::find(allowedIds.begin(), allowedIds.end(), id)
                == allowedIds.end()) {
                return reject(
                    scope,
                    id,
                    "chunk id",
                    "one of the pinned profile chunk ids",
                    hexadecimal(id));
            }
            if (!seen.insert(id).second) {
                return reject(scope, id, "occurrence count", "exactly 1", "a duplicate");
            }

            const std::size_t payloadOffset = offset + LegacyChunkHeaderSize;
            if (payloadSize > bytes.size() - payloadOffset) {
                return reject(
                    scope,
                    id,
                    "payload byte range",
                    "inside its enclosing stream",
                    std::to_string(payloadSize) + " bytes with "
                        + std::to_string(bytes.size() - payloadOffset)
                        + " available");
            }
            chunks->push_back({
                .id = id,
                .payload = bytes.subspan(payloadOffset, payloadSize),
            });
            offset = payloadOffset + payloadSize;
        }
        return true;
    }

    bool scanTopLevel()
    {
        if (bytes_.size() > ogfx::MaximumFileBytes) {
            return reject(
                "root",
                0,
                "file byte size",
                "at most " + std::to_string(ogfx::MaximumFileBytes),
                std::to_string(bytes_.size()));
        }
        constexpr std::array allowed{
            HeaderChunkId,
            DescriptionChunkId,
            ChildrenChunkId,
            BoneNamesChunkId,
            IkDataChunkId,
        };
        if (!scanChunks(bytes_, allowed, "root", &topChunks_)) {
            return false;
        }
        for (std::uint32_t id : allowed) {
            if (find(topChunks_, id) == nullptr) {
                return reject("root", id, "presence", "exactly 1 required chunk", "missing");
            }
        }
        return true;
    }

    bool decodeHeader(
        const ChunkView& chunk,
        std::uint8_t expectedType,
        std::string_view scope,
        SourceBounds* bounds)
    {
        if (chunk.payload.size() != HeaderPayloadSize) {
            return reject(
                scope,
                chunk.id,
                "byteSize",
                std::to_string(HeaderPayloadSize),
                std::to_string(chunk.payload.size()));
        }
        const std::uint8_t version = chunk.payload[0];
        const std::uint8_t type = chunk.payload[1];
        const std::uint16_t shaderId = readU16(chunk.payload, 2);
        if (version != SupportedVersion) {
            return reject(scope, chunk.id, "formatVersion", "4", std::to_string(version));
        }
        if (type != expectedType) {
            return reject(
                scope,
                chunk.id,
                "modelType",
                std::to_string(expectedType),
                std::to_string(type));
        }
        if (shaderId != SupportedShaderId) {
            return reject(scope, chunk.id, "shaderId", "0", std::to_string(shaderId));
        }

        bounds->minimum = readPosition(chunk.payload, 4);
        bounds->maximum = readPosition(chunk.payload, 16);
        bounds->sphereCenter = readPosition(chunk.payload, 28);
        bounds->sphereRadius = readF32(chunk.payload, 40);
        if (!ogfx::detail::positionIsFinite(bounds->minimum)
            || !ogfx::detail::positionIsFinite(bounds->maximum)) {
            return reject(scope, chunk.id, "AABB min/max", "finite f32 values", "non-finite data");
        }
        if (bounds->minimum.x > bounds->maximum.x
            || bounds->minimum.y > bounds->maximum.y
            || bounds->minimum.z > bounds->maximum.z) {
            return reject(scope, chunk.id, "AABB ordering", "ordered axes", "a reversed axis");
        }
        if (!ogfx::detail::positionIsFinite(bounds->sphereCenter)
            || !std::isfinite(bounds->sphereRadius)
            || bounds->sphereRadius < 0.0f) {
            return reject(scope, chunk.id, "bounding sphere", "finite and nonnegative", "invalid data");
        }
        return true;
    }

    bool readString(
        std::span<const std::uint8_t> bytes,
        std::size_t* offset,
        std::string_view scope,
        std::uint32_t chunkId,
        std::string_view field,
        bool allowEmpty,
        std::string* result)
    {
        if (*offset > bytes.size()) {
            return reject(scope, chunkId, std::string(field), "an in-range offset", "past the chunk end");
        }
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(*offset);
        const auto end = std::find(begin, bytes.end(), 0);
        if (end == bytes.end()) {
            return reject(scope, chunkId, std::string(field), "a NUL-terminated string", "no terminator");
        }
        const std::size_t size = static_cast<std::size_t>(end - begin);
        if (!allowEmpty && size == 0) {
            return reject(scope, chunkId, std::string(field), "a nonempty string", "empty");
        }
        if (size > MaximumSourceStringBytes) {
            return reject(
                scope,
                chunkId,
                std::string(field),
                "at most " + std::to_string(MaximumSourceStringBytes) + " bytes",
                std::to_string(size));
        }
        if (!std::all_of(begin, end, [](std::uint8_t byte) {
                return byte >= 0x20 && byte <= 0x7e;
            })) {
            return reject(scope, chunkId, std::string(field), "printable ASCII", "a non-ASCII/control byte");
        }
        result->assign(reinterpret_cast<const char*>(&*begin), size);
        *offset += size + 1;
        return true;
    }

    bool decodeDescription()
    {
        const ChunkView& chunk = *find(topChunks_, DescriptionChunkId);
        std::size_t offset = 0;
        std::string ignored;
        constexpr std::array fields{"sourceFile", "buildName", "createName", "modifyName"};
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (!readString(
                    chunk.payload,
                    &offset,
                    "root",
                    chunk.id,
                    fields[index],
                    false,
                    &ignored)) {
                return false;
            }
            if (index != 0) {
                if (chunk.payload.size() - offset < 4) {
                    return reject(
                        "root",
                        chunk.id,
                        std::string(fields[index]) + " timestamp",
                        "4 bytes",
                        std::to_string(chunk.payload.size() - offset));
                }
                offset += 4;
            }
        }
        if (offset != chunk.payload.size()) {
            return reject(
                "root",
                chunk.id,
                "payload byte range",
                "exactly the validated provenance fields",
                std::to_string(chunk.payload.size() - offset) + " trailing bytes");
        }
        return true;
    }

    bool decodeBones()
    {
        const ChunkView& chunk = *find(topChunks_, BoneNamesChunkId);
        if (chunk.payload.size() < 4) {
            return reject("root", chunk.id, "byteSize", "at least 4", std::to_string(chunk.payload.size()));
        }
        const std::uint32_t count = readU32(chunk.payload, 0);
        if (count == 0 || count > MaximumRigidBoneCount) {
            return reject(
                "root",
                chunk.id,
                "boneCount",
                "1.." + std::to_string(MaximumRigidBoneCount),
                std::to_string(count));
        }

        bones_.reserve(count);
        std::unordered_map<std::string, std::uint32_t> indexByName;
        std::size_t offset = 4;
        for (std::uint32_t index = 0; index < count; ++index) {
            Bone bone{};
            if (!readString(
                    chunk.payload,
                    &offset,
                    "root",
                    chunk.id,
                    ogfx::detail::indexedField("bones", index, "name"),
                    false,
                    &bone.name)
                || !readString(
                    chunk.payload,
                    &offset,
                    "root",
                    chunk.id,
                    ogfx::detail::indexedField("bones", index, "parentName"),
                    true,
                    &bone.parentName)) {
                return false;
            }
            if (chunk.payload.size() - offset < BoneObbBytes) {
                return reject(
                    "root",
                    chunk.id,
                    ogfx::detail::indexedField("bones", index, "obb"),
                    std::to_string(BoneObbBytes) + " bytes",
                    std::to_string(chunk.payload.size() - offset));
            }
            for (std::size_t component = 0; component < 15; ++component) {
                if (!std::isfinite(readF32(chunk.payload, offset + component * 4))) {
                    return reject(
                        "root",
                        chunk.id,
                        ogfx::detail::indexedField("bones", index, "obb"),
                        "15 finite f32 values (zero extents allowed)",
                        "a non-finite component");
                }
            }
            offset += BoneObbBytes;

            const std::string key = lowerAscii(bone.name);
            if (!indexByName.emplace(key, index).second) {
                return reject(
                    "root",
                    chunk.id,
                    ogfx::detail::indexedField("bones", index, "name"),
                    "case-insensitively unique names",
                    '"' + bone.name + '"');
            }
            bones_.push_back(std::move(bone));
        }
        if (offset != chunk.payload.size()) {
            return reject(
                "root", chunk.id, "payload byte range", "exactly boneCount records",
                std::to_string(chunk.payload.size() - offset) + " trailing bytes");
        }

        std::uint32_t rootCount = 0;
        for (std::uint32_t index = 0; index < bones_.size(); ++index) {
            Bone& bone = bones_[index];
            if (bone.parentName.empty()) {
                ++rootCount;
                continue;
            }
            const auto parent = indexByName.find(lowerAscii(bone.parentName));
            if (parent == indexByName.end()) {
                return reject(
                    "root",
                    chunk.id,
                    ogfx::detail::indexedField("bones", index, "parentName"),
                    "the name of another bone",
                    '"' + bone.parentName + '"');
            }
            if (parent->second == index) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "parentName"),
                    "a different bone", "itself");
            }
            bone.parent = parent->second;
        }
        if (rootCount != 1) {
            return reject("root", chunk.id, "root bone count", "exactly 1", std::to_string(rootCount));
        }

        std::vector<std::uint8_t> state(bones_.size(), 0);
        std::function<bool(std::uint32_t)> visit = [&](std::uint32_t index) {
            if (state[index] == 1) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "parentName"),
                    "an acyclic hierarchy", "a cycle");
            }
            if (state[index] == 2) {
                return true;
            }
            state[index] = 1;
            if (bones_[index].parent.has_value() && !visit(*bones_[index].parent)) {
                return false;
            }
            state[index] = 2;
            return true;
        };
        for (std::uint32_t index = 0; index < bones_.size(); ++index) {
            if (!visit(index)) {
                return false;
            }
        }
        return true;
    }

    bool decodeIkData()
    {
        const ChunkView& chunk = *find(topChunks_, IkDataChunkId);
        std::size_t offset = 0;
        for (std::uint32_t index = 0; index < bones_.size(); ++index) {
            if (chunk.payload.size() - offset < IkFixedBytes + 1) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "IK record"),
                    "a complete variable record",
                    std::to_string(chunk.payload.size() - offset) + " bytes");
            }
            const std::uint32_t version = readU32(chunk.payload, offset);
            offset += 4;
            if (version != 1) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "ikVersion"),
                    "1", std::to_string(version));
            }
            Bone& bone = bones_[index];
            if (!readString(
                    chunk.payload,
                    &offset,
                    "root",
                    chunk.id,
                    ogfx::detail::indexedField("bones", index, "physicsMaterial"),
                    false,
                    &bone.material)) {
                return false;
            }
            if (chunk.payload.size() - offset < IkFixedBytes - 4) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "IK fixed fields"),
                    std::to_string(IkFixedBytes - 4) + " bytes",
                    std::to_string(chunk.payload.size() - offset));
            }

            const std::uint16_t shapeType = readU16(chunk.payload, offset);
            bone.shapeFlags = readU16(chunk.payload, offset + 2);
            if (shapeType != CylinderShapeType) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "shapeType"),
                    "3 (cylinder) in the pinned rigid-compound profile",
                    std::to_string(shapeType));
            }
            if (bone.shapeFlags != 0) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "shapeFlags"),
                    "0 for an active nonbreakable collider",
                    hexadecimal(bone.shapeFlags));
            }
            bone.cylinder.center = readPosition(chunk.payload, offset + 80);
            bone.cylinder.direction = readPosition(chunk.payload, offset + 92);
            bone.cylinder.height = readF32(chunk.payload, offset + 104);
            bone.cylinder.radius = readF32(chunk.payload, offset + 108);
            if (!ogfx::detail::positionIsFinite(bone.cylinder.center)
                || !ogfx::detail::positionIsFinite(bone.cylinder.direction)
                || !std::isfinite(bone.cylinder.height)
                || !std::isfinite(bone.cylinder.radius)
                || bone.cylinder.height <= 0.0f
                || bone.cylinder.radius <= 0.0f) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "cylinder"),
                    "finite center/direction and positive finite height/radius",
                    "invalid data");
            }
            const double axisLengthSquared =
                static_cast<double>(bone.cylinder.direction.x) * bone.cylinder.direction.x
                + static_cast<double>(bone.cylinder.direction.y) * bone.cylinder.direction.y
                + static_cast<double>(bone.cylinder.direction.z) * bone.cylinder.direction.z;
            if (!std::isfinite(axisLengthSquared) || axisLengthSquared < 1.0e-12) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "cylinder.direction"),
                    "a nonzero finite vector", std::to_string(axisLengthSquared));
            }
            offset += BoneShapeBytes;

            const std::uint32_t jointType = readU32(chunk.payload, offset);
            if (jointType != RigidJointType) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "jointType"),
                    "0 (rigid) for safe flattening", std::to_string(jointType));
            }
            for (std::size_t floatIndex = 0; floatIndex < 14; ++floatIndex) {
                if (!std::isfinite(readF32(chunk.payload, offset + 4 + floatIndex * 4))) {
                    return reject(
                        "root", chunk.id,
                        ogfx::detail::indexedField("bones", index, "joint data"),
                        "finite f32 fields", "a non-finite field");
                }
            }
            const std::uint32_t jointFlags = readU32(chunk.payload, offset + 60);
            if ((jointFlags & BreakableJointFlag) != 0 || jointFlags != 0) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "jointFlags"),
                    "0 (rigid and nonbreakable)", hexadecimal(jointFlags));
            }
            if (!std::isfinite(readF32(chunk.payload, offset + 64))
                || !std::isfinite(readF32(chunk.payload, offset + 68))) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "break force/torque"),
                    "finite legacy scalars", "non-finite data");
            }
            // The final friction scalar is finite but intentionally not mapped. The
            // SoC reset path did not initialize it and rigid joints never consume it.
            if (!std::isfinite(readF32(chunk.payload, offset + 72))) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "jointFriction"),
                    "a finite legacy scalar", "non-finite data");
            }
            offset += JointDataBytes;

            bone.bindRotation = readPosition(chunk.payload, offset);
            bone.bindTranslation = readPosition(chunk.payload, offset + 12);
            offset += 24;
            if (!ogfx::detail::positionIsFinite(bone.bindRotation)
                || !ogfx::detail::positionIsFinite(bone.bindTranslation)) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "bind transform"),
                    "finite f32 values", "non-finite data");
            }
            if (bone.bindRotation.x != 0.0f
                || bone.bindRotation.y != 0.0f
                || bone.bindRotation.z != 0.0f) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "bindRotation"),
                    "zero rotation in the first rigid-compound profile",
                    "a nonzero component");
            }
            bone.mass = readF32(chunk.payload, offset);
            offset += 4;
            bone.centerOfMass = readPosition(chunk.payload, offset);
            offset += 12;
            if (!std::isfinite(bone.mass) || bone.mass <= 0.0f
                || !ogfx::detail::positionIsFinite(bone.centerOfMass)) {
                return reject(
                    "root", chunk.id,
                    ogfx::detail::indexedField("bones", index, "mass/centerOfMass"),
                    "positive finite mass and finite center",
                    "invalid data");
            }
        }
        if (offset != chunk.payload.size()) {
            return reject(
                "root", chunk.id, "payload byte range",
                "exactly one IK record per bone",
                std::to_string(chunk.payload.size() - offset) + " trailing bytes");
        }

        std::vector<std::uint8_t> state(bones_.size(), 0);
        std::function<void(std::uint32_t)> calculate = [&](std::uint32_t index) {
            if (state[index] == 2) {
                return;
            }
            if (bones_[index].parent.has_value()) {
                calculate(*bones_[index].parent);
                const ogfx::Position& parent = bones_[*bones_[index].parent].globalTranslation;
                bones_[index].globalTranslation = {
                    parent.x + bones_[index].bindTranslation.x,
                    parent.y + bones_[index].bindTranslation.y,
                    parent.z + bones_[index].bindTranslation.z,
                };
            } else {
                bones_[index].globalTranslation = bones_[index].bindTranslation;
            }
            state[index] = 2;
        };
        for (std::uint32_t index = 0; index < bones_.size(); ++index) {
            calculate(index);
        }
        return true;
    }

    bool decodeTexture(
        const ChunkView& chunk,
        std::string_view scope,
        std::string* textureName)
    {
        std::size_t offset = 0;
        std::string shader;
        if (!readString(chunk.payload, &offset, scope, chunk.id, "textureName", false, textureName)
            || !readString(chunk.payload, &offset, scope, chunk.id, "engineShaderName", false, &shader)) {
            return false;
        }
        if (offset != chunk.payload.size()) {
            return reject(
                scope, chunk.id, "payload byte range",
                "exactly two NUL-terminated strings",
                std::to_string(chunk.payload.size() - offset) + " trailing bytes");
        }
        if (shader != "models\\model") {
            return reject(
                scope, chunk.id, "engineShaderName",
                "\"models\\\\model\" for the pinned opaque mapping",
                '"' + shader + '"');
        }
        return true;
    }

    bool validateWinding(
        std::uint32_t chunkId,
        std::string_view scope,
        std::uint32_t firstVertex,
        std::uint32_t firstIndex,
        std::uint32_t indexCount)
    {
        for (std::uint32_t index = 0; index < indexCount; index += 3) {
            const ogfx::Position& a = model_.positions[
                firstVertex + model_.indices[firstIndex + index]];
            const ogfx::Position& b = model_.positions[
                firstVertex + model_.indices[firstIndex + index + 1]];
            const ogfx::Position& c = model_.positions[
                firstVertex + model_.indices[firstIndex + index + 2]];
            const double e1x = static_cast<double>(b.x) - a.x;
            const double e1y = static_cast<double>(b.y) - a.y;
            const double e1z = static_cast<double>(b.z) - a.z;
            const double e2x = static_cast<double>(c.x) - a.x;
            const double e2y = static_cast<double>(c.y) - a.y;
            const double e2z = static_cast<double>(c.z) - a.z;
            const double cx = e1y * e2z - e1z * e2y;
            const double cy = e1z * e2x - e1x * e2z;
            const double cz = e1x * e2y - e1y * e2x;
            if (cx * cx + cy * cy + cz * cz == 0.0) {
                continue;
            }
            for (std::uint32_t corner = 0; corner < 3; ++corner) {
                const ogfx::VertexAttributes& normal = model_.attributes[
                    firstVertex + model_.indices[firstIndex + index + corner]];
                if (!(cx * normal.nx + cy * normal.ny + cz * normal.nz > 0.0)) {
                    return reject(
                        scope, chunkId,
                        ogfx::detail::indexedField("triangles", index / 3, "winding"),
                        "unchanged winding to agree with every corner normal",
                        "a nonpositive dot product");
                }
            }
        }
        return true;
    }

    bool decodeChild(
        std::span<const std::uint8_t> bytes,
        std::uint32_t childIndex)
    {
        const std::string scope = "child[" + std::to_string(childIndex) + ']';
        constexpr std::array allowed{
            HeaderChunkId,
            TextureChunkId,
            VerticesChunkId,
            IndicesChunkId,
        };
        std::vector<ChunkView> chunks;
        if (!scanChunks(bytes, allowed, scope, &chunks)) {
            return false;
        }
        for (std::uint32_t id : allowed) {
            if (find(chunks, id) == nullptr) {
                return reject(scope, id, "presence", "exactly 1 required chunk", "missing");
            }
        }

        SourceBounds bounds{};
        if (!decodeHeader(
                *find(chunks, HeaderChunkId),
                SupportedRigidChildModelType,
                scope,
                &bounds)) {
            return false;
        }

        std::string textureName;
        if (!decodeTexture(*find(chunks, TextureChunkId), scope, &textureName)) {
            return false;
        }
        std::uint32_t materialIndex = 0;
        const auto existingMaterial = materialByTexture_.find(textureName);
        if (existingMaterial == materialByTexture_.end()) {
            if (model_.materials.size() >= std::numeric_limits<std::uint32_t>::max()) {
                return reject(scope, TextureChunkId, "material count", "u32", "too many materials");
            }
            materialIndex = static_cast<std::uint32_t>(model_.materials.size());
            ogfx::Material material{};
            material.baseColorTexture = textureName;
            model_.materials.push_back(std::move(material));
            materialByTexture_.emplace(std::move(textureName), materialIndex);
        } else {
            materialIndex = existingMaterial->second;
        }

        const ChunkView& vertices = *find(chunks, VerticesChunkId);
        if (vertices.payload.size() < 8) {
            return reject(scope, vertices.id, "byteSize", "at least 8", std::to_string(vertices.payload.size()));
        }
        const std::uint32_t vertexFormat = readU32(vertices.payload, 0);
        const std::uint32_t vertexCount = readU32(vertices.payload, 4);
        if (vertexFormat != SupportedRigidVertexFormat) {
            return reject(
                scope, vertices.id, "vertexFormat",
                hexadecimal(SupportedRigidVertexFormat) + " (one-link)",
                hexadecimal(vertexFormat));
        }
        if (vertexCount == 0) {
            return reject(scope, vertices.id, "vertexCount", "at least 1", "0");
        }
        const std::uint64_t vertexBytes = 8
            + static_cast<std::uint64_t>(vertexCount) * SourceRigidVertexRecordSize;
        if (vertexBytes != vertices.payload.size()) {
            return reject(
                scope, vertices.id, "byteSize",
                std::to_string(vertexBytes) + " from vertexCount and stride 60",
                std::to_string(vertices.payload.size()));
        }
        if (vertexCount > std::numeric_limits<std::uint32_t>::max() - model_.positions.size()) {
            return reject(scope, vertices.id, "aggregate vertex count", "u32", "overflow");
        }
        const std::uint32_t firstVertex = static_cast<std::uint32_t>(model_.positions.size());
        model_.positions.reserve(model_.positions.size() + vertexCount);
        model_.attributes.reserve(model_.attributes.size() + vertexCount);
        for (std::uint32_t index = 0; index < vertexCount; ++index) {
            const std::size_t offset = 8
                + static_cast<std::size_t>(index) * SourceRigidVertexRecordSize;
            const ogfx::Position position = readPosition(vertices.payload, offset);
            const ogfx::Position normal = readPosition(vertices.payload, offset + 12);
            const ogfx::Position tangent = readPosition(vertices.payload, offset + 24);
            const ogfx::Position binormal = readPosition(vertices.payload, offset + 36);
            const float u = readF32(vertices.payload, offset + 48);
            const float v = readF32(vertices.payload, offset + 52);
            const std::uint32_t boneIndex = readU32(vertices.payload, offset + 56);
            if (!ogfx::detail::positionIsFinite(position)
                || !ogfx::detail::positionIsFinite(normal)
                || !ogfx::detail::positionIsFinite(tangent)
                || !ogfx::detail::positionIsFinite(binormal)
                || !std::isfinite(u) || !std::isfinite(v)) {
                return reject(
                    scope, vertices.id,
                    ogfx::detail::indexedField("vertices", index, "P/N/T/B/UV"),
                    "finite f32 values", "non-finite data");
            }
            const double normalLengthSquared =
                static_cast<double>(normal.x) * normal.x
                + static_cast<double>(normal.y) * normal.y
                + static_cast<double>(normal.z) * normal.z;
            const double tangentLengthSquared =
                static_cast<double>(tangent.x) * tangent.x
                + static_cast<double>(tangent.y) * tangent.y
                + static_cast<double>(tangent.z) * tangent.z;
            const double binormalLengthSquared =
                static_cast<double>(binormal.x) * binormal.x
                + static_cast<double>(binormal.y) * binormal.y
                + static_cast<double>(binormal.z) * binormal.z;
            if (normalLengthSquared < 1.0e-12
                || tangentLengthSquared < 1.0e-12
                || binormalLengthSquared < 1.0e-12) {
                return reject(
                    scope, vertices.id,
                    ogfx::detail::indexedField("vertices", index, "N/T/B length"),
                    "nonzero finite vectors", "a zero-length vector");
            }
            if (boneIndex >= bones_.size()) {
                return reject(
                    scope, vertices.id,
                    ogfx::detail::indexedField("vertices", index, "boneIndex"),
                    "less than " + std::to_string(bones_.size()),
                    std::to_string(boneIndex));
            }
            model_.positions.push_back(position);
            model_.attributes.push_back({normal.x, normal.y, normal.z, u, v});
        }

        const ChunkView& indices = *find(chunks, IndicesChunkId);
        if (indices.payload.size() < 4) {
            return reject(scope, indices.id, "byteSize", "at least 4", std::to_string(indices.payload.size()));
        }
        const std::uint32_t indexCount = readU32(indices.payload, 0);
        if (indexCount == 0 || (indexCount % 3) != 0) {
            return reject(scope, indices.id, "indexCount", "a nonzero multiple of 3", std::to_string(indexCount));
        }
        const std::uint64_t indexBytes = 4
            + static_cast<std::uint64_t>(indexCount) * sizeof(std::uint16_t);
        if (indexBytes != indices.payload.size()) {
            return reject(
                scope, indices.id, "byteSize",
                std::to_string(indexBytes) + " from indexCount and u16 stride",
                std::to_string(indices.payload.size()));
        }
        if (indexCount > std::numeric_limits<std::uint32_t>::max() - model_.indices.size()) {
            return reject(scope, indices.id, "aggregate index count", "u32", "overflow");
        }
        const std::uint32_t firstIndex = static_cast<std::uint32_t>(model_.indices.size());
        model_.indices.reserve(model_.indices.size() + indexCount);
        for (std::uint32_t index = 0; index < indexCount; ++index) {
            const std::uint32_t value = readU16(
                indices.payload,
                4 + static_cast<std::size_t>(index) * sizeof(std::uint16_t));
            if (value >= vertexCount) {
                return reject(
                    scope, indices.id,
                    ogfx::detail::indexedField("indices", index, "value"),
                    "less than " + std::to_string(vertexCount), std::to_string(value));
            }
            model_.indices.push_back(value);
        }
        if (!validateWinding(indices.id, scope, firstVertex, firstIndex, indexCount)
            || !validateBounds(bounds, firstVertex, vertexCount, scope)) {
            return false;
        }

        model_.geometries.push_back({
            .firstVertex = firstVertex,
            .vertexCount = vertexCount,
            .firstIndex = firstIndex,
            .indexCount = indexCount,
            .materialIndex = materialIndex,
            .alphaTested = false,
        });
        return true;
    }

    bool decodeChildren()
    {
        const ChunkView& chunk = *find(topChunks_, ChildrenChunkId);
        if (chunk.payload.size() < LegacyChunkHeaderSize) {
            return reject("root", chunk.id, "byteSize", "at least one child chunk", std::to_string(chunk.payload.size()));
        }
        std::size_t offset = 0;
        std::uint32_t expectedId = 0;
        while (offset < chunk.payload.size()) {
            if (chunk.payload.size() - offset < LegacyChunkHeaderSize) {
                return reject(
                    "root", chunk.id, "child framing",
                    "a complete 8-byte child header",
                    std::to_string(chunk.payload.size() - offset));
            }
            const std::uint32_t rawId = readU32(chunk.payload, offset);
            const std::uint32_t id = rawId & ~CompressionMark;
            const std::uint32_t size = readU32(chunk.payload, offset + 4);
            if ((rawId & CompressionMark) != 0) {
                return reject("root", chunk.id, "child compression flag", "clear", "set");
            }
            if (id != expectedId) {
                return reject(
                    "root", chunk.id, "child id",
                    std::to_string(expectedId) + " (contiguous source order)",
                    std::to_string(id));
            }
            const std::size_t payloadOffset = offset + LegacyChunkHeaderSize;
            if (size > chunk.payload.size() - payloadOffset) {
                return reject(
                    "root", chunk.id, "child payload byte range",
                    "inside OGF_CHILDREN", std::to_string(size));
            }
            if (!decodeChild(chunk.payload.subspan(payloadOffset, size), id)) {
                return false;
            }
            ++expectedId;
            offset = payloadOffset + size;
        }
        if (expectedId == 0) {
            return reject("root", chunk.id, "child count", "at least 1", "0");
        }
        return true;
    }

    bool validateBounds(
        const SourceBounds& bounds,
        std::size_t firstVertex,
        std::size_t vertexCount,
        std::string_view scope)
    {
        if (vertexCount == 0 || firstVertex > model_.positions.size()
            || vertexCount > model_.positions.size() - firstVertex) {
            return reject(scope, HeaderChunkId, "vertex range", "a nonempty in-range partition", "invalid");
        }
        ogfx::Position minimum = model_.positions[firstVertex];
        ogfx::Position maximum = minimum;
        for (std::size_t index = firstVertex + 1; index < firstVertex + vertexCount; ++index) {
            const ogfx::Position& p = model_.positions[index];
            minimum.x = std::min(minimum.x, p.x);
            minimum.y = std::min(minimum.y, p.y);
            minimum.z = std::min(minimum.z, p.z);
            maximum.x = std::max(maximum.x, p.x);
            maximum.y = std::max(maximum.y, p.y);
            maximum.z = std::max(maximum.z, p.z);
        }
        if (minimum.x != bounds.minimum.x || minimum.y != bounds.minimum.y
            || minimum.z != bounds.minimum.z || maximum.x != bounds.maximum.x
            || maximum.y != bounds.maximum.y || maximum.z != bounds.maximum.z) {
            return reject(scope, HeaderChunkId, "AABB extrema", "the exact decoded vertex extrema", "different bounds");
        }
        float acceptedRadius = bounds.sphereRadius;
        if (acceptedRadius < std::numeric_limits<float>::max()) {
            acceptedRadius = std::nextafter(acceptedRadius, std::numeric_limits<float>::infinity());
        }
        const double radius = acceptedRadius;
        for (std::size_t index = firstVertex; index < firstVertex + vertexCount; ++index) {
            const ogfx::Position& p = model_.positions[index];
            const double x = static_cast<double>(p.x) - bounds.sphereCenter.x;
            const double y = static_cast<double>(p.y) - bounds.sphereCenter.y;
            const double z = static_cast<double>(p.z) - bounds.sphereCenter.z;
            if (radius * radius < x * x + y * y + z * z) {
                return reject(scope, HeaderChunkId, "sphere enclosure", "every vertex within one outward f32 ULP", "an outlier");
            }
        }
        return true;
    }

    bool buildRigidPhysics()
    {
        if (bones_.empty()) {
            return reject("root", IkDataChunkId, "collider count", "at least 1", "0");
        }
        double totalMass = 0.0;
        double weightedX = 0.0;
        double weightedY = 0.0;
        double weightedZ = 0.0;
        model_.physicsColliders.reserve(bones_.size());
        for (std::size_t index = 0; index < bones_.size(); ++index) {
            const Bone& bone = bones_[index];
            const ogfx::Position globalCenter{
                bone.globalTranslation.x + bone.cylinder.center.x,
                bone.globalTranslation.y + bone.cylinder.center.y,
                bone.globalTranslation.z + bone.cylinder.center.z,
            };
            const ogfx::Position globalMassCenter{
                bone.globalTranslation.x + bone.centerOfMass.x,
                bone.globalTranslation.y + bone.centerOfMass.y,
                bone.globalTranslation.z + bone.centerOfMass.z,
            };
            if (!ogfx::detail::positionIsFinite(globalCenter)
                || !ogfx::detail::positionIsFinite(globalMassCenter)) {
                return reject(
                    "root",
                    IkDataChunkId,
                    ogfx::detail::indexedField(
                        "bones", index, "model-space collider/centerOfMass"),
                    "finite values after bind-translation accumulation",
                    "floating-point overflow");
            }
            model_.physicsColliders.push_back({
                .shapeType = ogfx::PhysicsShapeType::Cylinder,
                .flags = bone.shapeFlags,
                .material = bone.material,
                .sourceNode = bone.name,
                .center = globalCenter,
                .axis = bone.cylinder.direction,
                .height = bone.cylinder.height,
                .radius = bone.cylinder.radius,
                .mass = bone.mass,
                .centerOfMass = globalMassCenter,
            });
            totalMass += bone.mass;
            weightedX += static_cast<double>(bone.mass) * globalMassCenter.x;
            weightedY += static_cast<double>(bone.mass) * globalMassCenter.y;
            weightedZ += static_cast<double>(bone.mass) * globalMassCenter.z;
        }
        if (!std::isfinite(totalMass) || totalMass <= 0.0
            || totalMass > std::numeric_limits<float>::max()) {
            return reject("root", IkDataChunkId, "aggregate mass", "positive finite f32", std::to_string(totalMass));
        }
        const ogfx::Position aggregateCenter{
            static_cast<float>(weightedX / totalMass),
            static_cast<float>(weightedY / totalMass),
            static_cast<float>(weightedZ / totalMass),
        };
        if (!ogfx::detail::positionIsFinite(aggregateCenter)) {
            return reject("root", IkDataChunkId, "aggregate center of mass", "finite f32", "overflow");
        }
        model_.physicsBodies.push_back({
            .firstCollider = 0,
            .colliderCount = static_cast<std::uint32_t>(model_.physicsColliders.size()),
            .mass = static_cast<float>(totalMass),
            .centerOfMass = aggregateCenter,
        });
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    std::string_view diagnosticName_;
    std::vector<ChunkView> topChunks_;
    std::vector<Bone> bones_;
    ogfx::Model model_;
    std::unordered_map<std::string, std::uint32_t> materialByTexture_;
    std::string error_;
};

bool detectModelType(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName,
    std::uint8_t* modelType,
    std::string* error)
{
    if (bytes.size() > ogfx::MaximumFileBytes) {
        *error = makeDispatchDiagnostic(
            diagnosticName,
            "file byte size",
            "at most " + std::to_string(ogfx::MaximumFileBytes),
            std::to_string(bytes.size()));
        return false;
    }
    if (bytes.size() < LegacyChunkHeaderSize) {
        *error = makeDispatchDiagnostic(
            diagnosticName, "file byte size", "at least 8", std::to_string(bytes.size()));
        return false;
    }
    std::size_t offset = 0;
    std::optional<std::uint8_t> foundType;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < LegacyChunkHeaderSize) {
            *error = makeDispatchDiagnostic(
                diagnosticName,
                "trailing bytes",
                "a complete 8-byte chunk header",
                std::to_string(bytes.size() - offset));
            return false;
        }
        const std::uint32_t rawId = readU32(bytes, offset);
        const std::uint32_t size = readU32(bytes, offset + 4);
        const std::size_t payloadOffset = offset + LegacyChunkHeaderSize;
        if (size > bytes.size() - payloadOffset) {
            *error = makeDispatchDiagnostic(
                diagnosticName, "payload byte range", "inside the file", std::to_string(size));
            return false;
        }
        if ((rawId & ~CompressionMark) == HeaderChunkId) {
            if ((rawId & CompressionMark) != 0) {
                *error = makeDispatchDiagnostic(
                    diagnosticName, "header compression flag", "clear", "set");
                return false;
            }
            if (foundType.has_value()) {
                *error = makeDispatchDiagnostic(
                    diagnosticName, "header occurrence count", "exactly 1", "a duplicate");
                return false;
            }
            if (size < 2) {
                *error = makeDispatchDiagnostic(
                    diagnosticName, "header byte size", "at least 2", std::to_string(size));
                return false;
            }
            foundType = bytes[payloadOffset + 1];
        }
        offset = payloadOffset + size;
    }
    if (!foundType.has_value()) {
        *error = makeDispatchDiagnostic(
            diagnosticName, "OGF_HEADER presence", "exactly 1", "missing");
        return false;
    }
    *modelType = *foundType;
    return true;
}
}

ogfx::DecodeResult decodeModel(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName)
{
    try {
        std::uint8_t modelType = 0;
        std::string error;
        if (!detectModelType(bytes, diagnosticName, &modelType, &error)) {
            return {.model = {}, .error = std::move(error)};
        }
        if (modelType == SupportedModelType) {
            return decodeStaticModel(bytes, diagnosticName);
        }
        if (modelType == SupportedRigidModelType) {
            return RigidDecoder(bytes, diagnosticName).run();
        }
        return {
            .model = {},
            .error = makeDispatchDiagnostic(
                diagnosticName,
                "modelType",
                "0 (normal/static) or 10 (supported rigid compound)",
                std::to_string(modelType)),
        };
    } catch (const std::bad_alloc&) {
        return {
            .model = {},
            .error = makeDispatchDiagnostic(
                diagnosticName,
                "resource allocation",
                "enough memory for the bounded rigid model",
                "allocation failure"),
        };
    } catch (const std::length_error&) {
        return {
            .model = {},
            .error = makeDispatchDiagnostic(
                diagnosticName,
                "resource allocation",
                "host-supported bounded containers",
                "a length error"),
        };
    }
}
}
