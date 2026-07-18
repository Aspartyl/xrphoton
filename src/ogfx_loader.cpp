#include "ogfx_loader.hpp"

#include "ogfx_detail.hpp"

#include <cstddef>
#include <fstream>
#include <new>
#include <utility>
#include <vector>

namespace xrphoton
{
namespace
{
std::string fileFailure(
    std::string_view diagnosticName,
    std::string_view field,
    std::string_view expected,
    std::string_view found)
{
    return ogfx::detail::makeFileDiagnostic(
        diagnosticName,
        "loader",
        field,
        expected,
        found);
}

OgfxLoadResult allocationFailure(std::string_view diagnosticName)
{
    return {
        .scene = {},
        .error = fileFailure(
            diagnosticName,
            "resource allocation",
            "enough memory for the bounded model",
            "allocation failure"),
    };
}

std::string pathDiagnosticName(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return {
        reinterpret_cast<const char*>(utf8.data()),
        utf8.size(),
    };
}
}

OgfxLoadResult decodeOgfxScene(
    std::span<const std::uint8_t> bytes,
    std::string_view diagnosticName)
{
    ogfx::DecodeResult decoded = ogfx::decodeModel(bytes, diagnosticName);
    if (!decoded) {
        return {
            .scene = {},
            .error = std::move(decoded.error),
        };
    }

    try {
        SceneData scene{};
        scene.positions.reserve(decoded.model.positions.size() * 3);
        for (const ogfx::Position& position : decoded.model.positions) {
            scene.positions.push_back(position.x);
            scene.positions.push_back(position.y);
            scene.positions.push_back(position.z);
        }

        scene.attributes.reserve(decoded.model.attributes.size());
        for (const ogfx::VertexAttributes& attributes : decoded.model.attributes) {
            scene.attributes.push_back({
                attributes.nx,
                attributes.ny,
                attributes.nz,
                attributes.u,
                attributes.v,
            });
        }

        scene.indices = std::move(decoded.model.indices);
        scene.geometries.reserve(decoded.model.geometries.size());
        for (const ogfx::Geometry& geometry : decoded.model.geometries) {
            scene.geometries.push_back({
                .firstVertex = geometry.firstVertex,
                .vertexCount = geometry.vertexCount,
                .firstIndex = geometry.firstIndex,
                .indexCount = geometry.indexCount,
                .materialIndex = geometry.materialIndex,
                .alphaTested = geometry.alphaTested,
            });
        }

        scene.meshes.reserve(decoded.model.meshes.size());
        for (const ogfx::Mesh& mesh : decoded.model.meshes) {
            scene.meshes.push_back({
                .firstGeometry = mesh.firstGeometry,
                .geometryCount = mesh.geometryCount,
            });
        }

        scene.materials.reserve(decoded.model.materials.size());
        for (const ogfx::Material& material : decoded.model.materials) {
            SceneMaterial sceneMaterial{};
            for (std::size_t component = 0;
                 component < material.baseColorFactor.size();
                 ++component) {
                sceneMaterial.baseColorFactor[component] = material.baseColorFactor[component];
            }
            sceneMaterial.baseColorImage = 0;
            sceneMaterial.alphaCutoff = material.alphaCutoff;
            // Runtime acceptance still guarantees this is empty until the texture
            // resolver/upload/sampling consumer lands, but preserve the OGFx carrier
            // verbatim now so opening that gate cannot silently discard identity.
            sceneMaterial.baseColorTexture = material.baseColorTexture;
            scene.materials.push_back(std::move(sceneMaterial));
        }

        return {
            .scene = std::move(scene),
            .error = {},
        };
    } catch (const std::bad_alloc&) {
        return allocationFailure(diagnosticName);
    }
}

OgfxLoadResult loadOgfxModel(const std::filesystem::path& path)
{
    std::string diagnosticName;
    try {
        // File I/O keeps the native path. UTF-8 conversion is diagnostic-only so a
        // Windows path outside the active narrow code page remains loadable.
        diagnosticName = pathDiagnosticName(path);
    } catch (const std::filesystem::filesystem_error&) {
        diagnosticName = "<OGFx path unavailable as UTF-8>";
    } catch (const std::bad_alloc&) {
        return allocationFailure("<OGFx path>");
    }
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {
            .scene = {},
            .error = fileFailure(
                diagnosticName,
                "file open",
                "a readable binary file",
                "open failure"),
        };
    }

    const std::streampos endPosition = input.tellg();
    if (endPosition < std::streampos(0)) {
        return {
            .scene = {},
            .error = fileFailure(
                diagnosticName,
                "file byte size",
                "a measurable file",
                "tell failure"),
        };
    }
    const std::uint64_t fileSize =
        static_cast<std::uint64_t>(static_cast<std::streamoff>(endPosition));
    if (fileSize > ogfx::MaximumFileBytes) {
        return {
            .scene = {},
            .error = fileFailure(
                diagnosticName,
                "file byte size",
                "at most " + std::to_string(ogfx::MaximumFileBytes),
                std::to_string(fileSize)),
        };
    }

    try {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
        input.seekg(0);
        if (!bytes.empty()) {
            input.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        }
        if (!input) {
            return {
                .scene = {},
                .error = fileFailure(
                    diagnosticName,
                    "file read",
                    std::to_string(fileSize) + " bytes",
                    std::to_string(input.gcount()) + " bytes"),
            };
        }
        return decodeOgfxScene(bytes, diagnosticName);
    } catch (const std::bad_alloc&) {
        return allocationFailure(diagnosticName);
    }
}
}
