#include "ogfx.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
xrphoton::ogfx::Model buildAxisAlignedBox(
    xrphoton::ogfx::Position minimum,
    xrphoton::ogfx::Position maximum,
    const std::array<float, 4>& baseColorFactor)
{
    using namespace xrphoton::ogfx;

    Model model{};
    // Each face owns four vertices so its normal and complete 0..1 UV square are
    // unambiguous. Vertices and triangles are ordered counter-clockwise when
    // viewed from outside the box.
    model.positions = {
        // -X
        {minimum.x, minimum.y, minimum.z},
        {minimum.x, minimum.y, maximum.z},
        {minimum.x, maximum.y, maximum.z},
        {minimum.x, maximum.y, minimum.z},
        // +X
        {maximum.x, minimum.y, maximum.z},
        {maximum.x, minimum.y, minimum.z},
        {maximum.x, maximum.y, minimum.z},
        {maximum.x, maximum.y, maximum.z},
        // -Y
        {minimum.x, minimum.y, maximum.z},
        {minimum.x, minimum.y, minimum.z},
        {maximum.x, minimum.y, minimum.z},
        {maximum.x, minimum.y, maximum.z},
        // +Y
        {minimum.x, maximum.y, minimum.z},
        {minimum.x, maximum.y, maximum.z},
        {maximum.x, maximum.y, maximum.z},
        {maximum.x, maximum.y, minimum.z},
        // -Z
        {maximum.x, minimum.y, minimum.z},
        {minimum.x, minimum.y, minimum.z},
        {minimum.x, maximum.y, minimum.z},
        {maximum.x, maximum.y, minimum.z},
        // +Z
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

    model.geometries.push_back({
        .firstVertex = 0,
        .vertexCount = 24,
        .firstIndex = 0,
        .indexCount = 36,
        .materialIndex = 0,
        .alphaTested = false,
    });
    model.meshes.push_back({
        .firstGeometry = 0,
        .geometryCount = 1,
    });
    model.materials.emplace_back();
    model.materials[0].baseColorFactor = baseColorFactor;
    return model;
}

xrphoton::ogfx::Model buildTestYardGround()
{
    return buildAxisAlignedBox(
        {-10.0f, -0.4f, -10.0f},
        { 10.0f,  0.0f,  10.0f},
        {0.42f, 0.42f, 0.45f, 1.0f});
}

xrphoton::ogfx::Model buildTestYardWall()
{
    return buildAxisAlignedBox(
        {-4.0f, 0.0f, -0.15f},
        { 4.0f, 3.0f,  0.15f},
        {0.55f, 0.24f, 0.18f, 1.0f});
}

xrphoton::ogfx::Model buildTestYardBox()
{
    using namespace xrphoton::ogfx;

    Model model = buildAxisAlignedBox(
        {-0.5f, -0.5f, -0.5f},
        { 0.5f,  0.5f,  0.5f},
        {0.80f, 0.62f, 0.22f, 1.0f});
    model.physicsBodies.push_back({
        .firstCollider = 0,
        .colliderCount = 1,
        .mass = 30.0f,
        .centerOfMass = {0.0f, 0.0f, 0.0f},
    });
    model.physicsColliders.push_back({
        .shapeType = PhysicsShapeType::Box,
        .flags = 0,
        .material = {},
        .sourceNode = {},
        .center = {0.0f, 0.0f, 0.0f},
        .orientation = {0.0f, 0.0f, 0.0f, 1.0f},
        .halfExtents = {0.5f, 0.5f, 0.5f},
        .mass = 30.0f,
        .centerOfMass = {0.0f, 0.0f, 0.0f},
    });
    return model;
}

xrphoton::ogfx::Model buildTestQuad()
{
    using namespace xrphoton::ogfx;

    Model model{};
    // Keep this byte-for-byte equivalent to M3b's proven quad input. Instance
    // placement is deliberately absent because OGFx stores models, not world state.
    model.positions = {
        {-0.5f, -0.5f, 0.0f},
        { 0.5f, -0.5f, 0.0f},
        { 0.5f,  0.5f, 0.0f},
        {-0.5f,  0.5f, 0.0f},
    };
    // The startup camera approaches along +Z, so the yard-facing side has a
    // -Z normal and front-face winding from that reference pose.
    model.attributes = {
        {0.0f, 0.0f, -1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, -1.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, -1.0f, 0.0f, 1.0f},
    };
    model.indices = {0, 2, 1, 0, 3, 2};
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
    model.materials[0].baseColorFactor = {1.0f, 0.45f, 0.12f, 1.0f};
    return model;
}

xrphoton::ogfx::Model buildTestWedge()
{
    using namespace xrphoton::ogfx;

    Model model{};
    // Two independent rectangular geometry ranges meet at a ridge. The entire wedge
    // is tilted 20 degrees around Z so its normals have both X and Y components; that
    // makes the gallery's later non-uniform-scale normal oracle meaningful. Duplicating
    // the ridge vertices is intentional: OGFx indices are local to each geometry, and
    // the distinct normals/materials make GeometryIndex() == 1 observable on the GPU.
    model.positions = {
        {-0.3586035f, -0.7690277f,  0.25f},
        { 0.2052121f, -0.5638156f, -0.25f},
        {-0.2052121f,  0.5638156f, -0.25f},
        {-0.7690277f,  0.3586035f,  0.25f},

        { 0.2052121f, -0.5638156f, -0.25f},
        { 0.7690277f, -0.3586035f,  0.25f},
        { 0.3586035f,  0.7690277f,  0.25f},
        {-0.2052121f,  0.5638156f, -0.25f},
    };

    constexpr float NormalX = 0.6015766f;
    constexpr float NormalY = 0.2189560f;
    constexpr float NormalZ = 0.7682213f;
    model.attributes = {
        {-NormalX, -NormalY, -NormalZ, 0.0f, 0.0f},
        {-NormalX, -NormalY, -NormalZ, 1.0f, 0.0f},
        {-NormalX, -NormalY, -NormalZ, 1.0f, 1.0f},
        {-NormalX, -NormalY, -NormalZ, 0.0f, 1.0f},

        { NormalX,  NormalY, -NormalZ, 0.0f, 0.0f},
        { NormalX,  NormalY, -NormalZ, 1.0f, 0.0f},
        { NormalX,  NormalY, -NormalZ, 1.0f, 1.0f},
        { NormalX,  NormalY, -NormalZ, 0.0f, 1.0f},
    };

    model.indices = {
        0, 2, 1, 0, 3, 2,
        0, 2, 1, 0, 3, 2,
    };
    model.geometries = {
        Geometry{
            .firstVertex = 0,
            .vertexCount = 4,
            .firstIndex = 0,
            .indexCount = 6,
            .materialIndex = 0,
            .alphaTested = false,
        },
        Geometry{
            .firstVertex = 4,
            .vertexCount = 4,
            .firstIndex = 6,
            .indexCount = 6,
            .materialIndex = 1,
            .alphaTested = false,
        },
    };
    model.meshes.push_back({
        .firstGeometry = 0,
        .geometryCount = 2,
    });

    // The standing material shader multiplies these factors by the white fallback
    // texture and a normal term, preserving the faces' blue/green identities.
    model.materials.resize(2);
    model.materials[0].baseColorFactor = {0.25f, 0.45f, 1.0f, 1.0f};
    model.materials[1].baseColorFactor = {0.15f, 1.0f, 0.25f, 1.0f};
    return model;
}

bool publishOutput(
    const std::filesystem::path& outputPath,
    const std::vector<std::uint8_t>& bytes)
{
    const std::string diagnosticName = outputPath.string();
    std::error_code directoryError;
    const std::filesystem::path parentPath = outputPath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath, directoryError);
    }
    if (directoryError) {
        std::cerr << diagnosticName << ": failed to create its output directory: "
                  << directoryError.message() << ".\n";
        return false;
    }

    std::filesystem::path temporaryPath = outputPath;
    temporaryPath += ".tmp";
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << diagnosticName << ": failed to open its temporary output file.\n";
        return false;
    }

    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        std::cerr << diagnosticName << ": failed to write the complete output file.\n";
        return false;
    }

    // Publish only a complete file. Keeping the temporary file beside the
    // destination makes the replacement atomic on supported filesystems.
    std::error_code renameError;
    std::filesystem::rename(temporaryPath, outputPath, renameError);
    if (renameError) {
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        std::cerr << diagnosticName << ": failed to publish the output file: "
                  << renameError.message() << ".\n";
        return false;
    }
    return true;
}
}

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 6) {
        std::cerr
            << "Usage: xrPhotonProbeAssetCompiler <test-quad.ogfx> "
               "<test-wedge.ogfx> <test-yard-ground.ogfx> "
               "<test-yard-wall.ogfx> <test-yard-box.ogfx>\n";
        return 1;
    }

    const std::array<std::filesystem::path, 5> outputPaths{
        arguments[1],
        arguments[2],
        arguments[3],
        arguments[4],
        arguments[5],
    };
    for (std::size_t left = 0; left < outputPaths.size(); ++left) {
        for (std::size_t right = left + 1; right < outputPaths.size(); ++right) {
            if (outputPaths[left] == outputPaths[right]) {
                std::cerr << "Probe asset output paths must be pairwise distinct.\n";
                return 1;
            }
        }
    }

    const std::array<xrphoton::ogfx::Model, 5> models{
        buildTestQuad(),
        buildTestWedge(),
        buildTestYardGround(),
        buildTestYardWall(),
        buildTestYardBox(),
    };
    std::array<xrphoton::ogfx::SerializeResult, 5> serialized;
    for (std::size_t index = 0; index < models.size(); ++index) {
        serialized[index] = xrphoton::ogfx::serializeModel(
            models[index],
            outputPaths[index].string());
        if (!serialized[index]) {
            std::cerr << serialized[index].error << '\n';
            return 1;
        }
    }

    for (std::size_t index = 0; index < serialized.size(); ++index) {
        if (!publishOutput(outputPaths[index], serialized[index].bytes)) {
            return 1;
        }
    }
    return 0;
}
