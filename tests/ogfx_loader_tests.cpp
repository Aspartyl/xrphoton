#include "ogfx.hpp"
#include "ogfx_loader.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace
{
int failureCount = 0;

void expect(bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failureCount;
    }
}

xrphoton::ogfx::Model makeQuad()
{
    xrphoton::ogfx::Model model{};
    model.positions = {
        {-0.5f, -0.5f, 0.0f},
        { 0.5f, -0.5f, 0.0f},
        { 0.5f,  0.5f, 0.0f},
        {-0.5f,  0.5f, 0.0f},
    };
    model.attributes = {
        {1.0f, 0.0f, 0.0f, -1.0f, 2.0f},
        {0.0f, 1.0f, 0.0f,  3.0f, 4.0f},
        {0.0f, 0.0f, 1.0f,  5.0f, 6.0f},
        {1.0f, 1.0f, 1.0f,  7.0f, 8.0f},
    };
    model.indices = {0, 1, 2, 0, 2, 3};
    model.geometries.push_back({0, 4, 0, 6, 0, false});
    model.meshes.push_back({0, 1});
    model.materials.emplace_back();
    model.materials[0].baseColorFactor = {0.25f, 0.5f, 0.75f, 0.875f};
    model.materials[0].alphaCutoff = 0.375f;
    return model;
}

bool sceneIsEmpty(const xrphoton::SceneData& scene)
{
    return scene.positions.empty()
        && scene.attributes.empty()
        && scene.indices.empty()
        && scene.geometries.empty()
        && scene.meshes.empty()
        && scene.instances.empty()
        && scene.materials.empty()
        && scene.images.empty();
}

void testSceneConversion()
{
    const xrphoton::ogfx::SerializeResult serialized =
        xrphoton::ogfx::serializeModel(makeQuad(), "loader-source");
    expect(static_cast<bool>(serialized), "runtime-loader source serializes");
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return;
    }

    const xrphoton::OgfxLoadResult loaded =
        xrphoton::decodeOgfxScene(serialized.bytes, "quad.ogfx");
    expect(static_cast<bool>(loaded), "runtime adapter decodes the canonical quad");
    if (!loaded) {
        std::cerr << loaded.error << '\n';
        return;
    }

    constexpr float expectedPositions[]{
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.5f,  0.5f, 0.0f,
        -0.5f,  0.5f, 0.0f,
    };
    expect(
        loaded.scene.positions
            == std::vector<float>(std::begin(expectedPositions), std::end(expectedPositions)),
        "runtime positions are flattened field-by-field");
    constexpr std::array expectedAttributes{
        xrphoton::VertexAttributes{1.0f, 0.0f, 0.0f, -1.0f, 2.0f},
        xrphoton::VertexAttributes{0.0f, 1.0f, 0.0f,  3.0f, 4.0f},
        xrphoton::VertexAttributes{0.0f, 0.0f, 1.0f,  5.0f, 6.0f},
        xrphoton::VertexAttributes{1.0f, 1.0f, 1.0f,  7.0f, 8.0f},
    };
    expect(loaded.scene.attributes.size() == expectedAttributes.size(),
        "runtime owns four attribute records");
    bool attributesMatch = loaded.scene.attributes.size() == expectedAttributes.size();
    for (std::size_t index = 0;
         attributesMatch && index < expectedAttributes.size();
         ++index) {
        const xrphoton::VertexAttributes& actual = loaded.scene.attributes[index];
        const xrphoton::VertexAttributes& expected = expectedAttributes[index];
        attributesMatch = actual.nx == expected.nx
            && actual.ny == expected.ny
            && actual.nz == expected.nz
            && actual.u == expected.u
            && actual.v == expected.v;
    }
    expect(attributesMatch, "runtime attributes retain every normal and UV field");
    expect(loaded.scene.indices == std::vector<std::uint32_t>({0, 1, 2, 0, 2, 3}),
        "runtime owns the six geometry-local indices");
    expect(loaded.scene.geometries.size() == 1
            && loaded.scene.geometries[0].vertexCount == 4
            && loaded.scene.geometries[0].indexCount == 6
            && !loaded.scene.geometries[0].alphaTested,
        "runtime geometry range is reconstructed");
    expect(loaded.scene.meshes.size() == 1
            && loaded.scene.meshes[0].firstGeometry == 0
            && loaded.scene.meshes[0].geometryCount == 1,
        "runtime mesh grouping is reconstructed");
    expect(loaded.scene.materials.size() == 1
            && loaded.scene.materials[0].baseColorFactor[0] == 0.25f
            && loaded.scene.materials[0].baseColorFactor[1] == 0.5f
            && loaded.scene.materials[0].baseColorFactor[2] == 0.75f
            && loaded.scene.materials[0].baseColorFactor[3] == 0.875f
            && loaded.scene.materials[0].baseColorImage == 0
            && loaded.scene.materials[0].alphaCutoff == 0.375f,
        "runtime material fields are reconstructed without a texture reference");
    expect(loaded.scene.instances.empty(), "OGFx decoding creates no world instances");
    expect(loaded.scene.images.empty(), "the texture-free M4 profile creates no images");

    std::vector<std::uint8_t> malformed = serialized.bytes;
    malformed[0] = 'X';
    const xrphoton::OgfxLoadResult rejected =
        xrphoton::decodeOgfxScene(malformed, "bad.ogfx");
    expect(!rejected, "runtime adapter propagates decoder failure");
    expect(sceneIsEmpty(rejected.scene), "runtime adapter exposes no partial SceneData");
    expect(rejected.error.find("bad.ogfx") != std::string::npos,
        "runtime adapter preserves the decoder diagnostic");
}

void testFileBoundary()
{
    const xrphoton::ogfx::SerializeResult serialized =
        xrphoton::ogfx::serializeModel(makeQuad(), "file-source");
    if (!serialized) {
        expect(false, "file-loader source serializes");
        return;
    }

    const std::filesystem::path path =
        std::filesystem::current_path()
        / std::filesystem::path(u8"xrphoton-ogfx-loader-é-test.ogfx");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(
            reinterpret_cast<const char*>(serialized.bytes.data()),
            static_cast<std::streamsize>(serialized.bytes.size()));
        expect(static_cast<bool>(output), "test fixture writes to its temporary file");
    }

    const xrphoton::OgfxLoadResult loaded = xrphoton::loadOgfxModel(path);
    expect(static_cast<bool>(loaded), "filesystem wrapper loads a complete OGFx file");
    expect(loaded.scene.positions.size() == 12, "filesystem wrapper delegates to scene decoding");

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    expect(!removeError, "temporary OGFx fixture is removed");

    const xrphoton::OgfxLoadResult missing = xrphoton::loadOgfxModel(path);
    expect(!missing, "filesystem wrapper rejects a missing file");
    expect(sceneIsEmpty(missing.scene), "missing-file failure has no partial scene");
    expect(missing.error.find("file open") != std::string::npos,
        "missing-file diagnostic identifies the file-open field");
}
}

int main()
{
    testSceneConversion();
    testFileBoundary();

    if (failureCount != 0) {
        std::cerr << failureCount << " OGFx runtime-loader test assertion(s) failed.\n";
        return 1;
    }
    return 0;
}
