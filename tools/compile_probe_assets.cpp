#include "ogfx.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace
{
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
    // The startup camera approaches along +Z, so the preview-facing side has a
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
    if (argumentCount != 3) {
        std::cerr
            << "Usage: xrPhotonProbeAssetCompiler <test-quad.ogfx> <test-wedge.ogfx>\n";
        return 1;
    }

    const std::filesystem::path quadOutputPath = arguments[1];
    const std::filesystem::path wedgeOutputPath = arguments[2];
    if (quadOutputPath == wedgeOutputPath) {
        std::cerr << "Probe asset output paths must be distinct.\n";
        return 1;
    }

    const xrphoton::ogfx::SerializeResult quad =
        xrphoton::ogfx::serializeModel(buildTestQuad(), quadOutputPath.string());
    if (!quad) {
        std::cerr << quad.error << '\n';
        return 1;
    }

    const xrphoton::ogfx::SerializeResult wedge =
        xrphoton::ogfx::serializeModel(buildTestWedge(), wedgeOutputPath.string());
    if (!wedge) {
        std::cerr << wedge.error << '\n';
        return 1;
    }

    if (!publishOutput(quadOutputPath, quad.bytes)
        || !publishOutput(wedgeOutputPath, wedge.bytes)) {
        return 1;
    }
    return 0;
}
