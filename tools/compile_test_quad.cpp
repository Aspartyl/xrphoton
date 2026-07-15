#include "ogfx.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

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
}

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 2) {
        std::cerr << "Usage: xrPhotonTestQuadCompiler <output.ogfx>\n";
        return 1;
    }

    const std::filesystem::path outputPath = arguments[1];
    const std::string diagnosticName = outputPath.string();
    const xrphoton::ogfx::SerializeResult serialized =
        xrphoton::ogfx::serializeModel(buildTestQuad(), diagnosticName);
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return 1;
    }

    std::error_code directoryError;
    const std::filesystem::path parentPath = outputPath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath, directoryError);
    }
    if (directoryError) {
        std::cerr << diagnosticName << ": failed to create its output directory: "
                  << directoryError.message() << ".\n";
        return 1;
    }

    std::filesystem::path temporaryPath = outputPath;
    temporaryPath += ".tmp";
    std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << diagnosticName << ": failed to open its temporary output file.\n";
        return 1;
    }

    output.write(
        reinterpret_cast<const char*>(serialized.bytes.data()),
        static_cast<std::streamsize>(serialized.bytes.size()));
    output.close();
    if (!output) {
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        std::cerr << diagnosticName << ": failed to write the complete output file.\n";
        return 1;
    }

    // Publish only a complete file. Because the temporary file is beside the final
    // path, rename is an atomic replacement on the current POSIX build platforms;
    // an interrupted write can leave only an ignored .tmp file, never a truncated
    // asset that the build system mistakes for a successful output.
    std::error_code renameError;
    std::filesystem::rename(temporaryPath, outputPath, renameError);
    if (renameError) {
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        std::cerr << diagnosticName << ": failed to publish the output file: "
                  << renameError.message() << ".\n";
        return 1;
    }

    return 0;
}
