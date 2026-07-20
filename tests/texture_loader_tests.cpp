#include "texture_loader.hpp"
#include "texture_loader_detail.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
int failureCount = 0;

constexpr uint32_t makeFourCc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<unsigned char>(a))
        | (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8)
        | (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16)
        | (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

constexpr uint32_t FourCcDxt1 = makeFourCc('D', 'X', 'T', '1');
constexpr uint32_t FourCcDxt3 = makeFourCc('D', 'X', 'T', '3');
constexpr uint32_t FourCcDxt5 = makeFourCc('D', 'X', 'T', '5');

void expect(bool condition, std::string_view description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failureCount;
    }
}

bool contains(std::string_view text, std::string_view fragment)
{
    return text.find(fragment) != std::string_view::npos;
}

void writeU32(std::vector<uint8_t>* bytes, std::size_t offset, uint32_t value)
{
    (*bytes)[offset] = static_cast<uint8_t>(value);
    (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8);
    (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16);
    (*bytes)[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint64_t levelSize(uint32_t width, uint32_t height, uint32_t blockSize)
{
    const uint64_t blocksWide = std::max<uint64_t>(1, (uint64_t{width} + 3) / 4);
    const uint64_t blocksHigh = std::max<uint64_t>(1, (uint64_t{height} + 3) / 4);
    return blocksWide * blocksHigh * blockSize;
}

std::vector<uint8_t> makeRgba8Dds(
    uint32_t width,
    uint32_t height,
    uint32_t mipCount = 1,
    bool declareMipChain = false)
{
    uint64_t payloadSize = 0;
    uint32_t mipWidth = width;
    uint32_t mipHeight = height;
    for (uint32_t level = 0; level < mipCount; ++level) {
        payloadSize += uint64_t{mipWidth} * mipHeight * 4;
        mipWidth = std::max<uint32_t>(1, mipWidth >> 1);
        mipHeight = std::max<uint32_t>(1, mipHeight >> 1);
    }

    std::vector<uint8_t> bytes(static_cast<std::size_t>(128 + payloadSize), 0);
    bytes[0] = 'D';
    bytes[1] = 'D';
    bytes[2] = 'S';
    bytes[3] = ' ';
    writeU32(&bytes, 4, 124);
    uint32_t flags = 0x0000100F;
    uint32_t caps = 0x00001000;
    if (declareMipChain) {
        flags |= 0x00020000;
        caps |= 0x00400000;
    }
    writeU32(&bytes, 8, flags);
    writeU32(&bytes, 12, height);
    writeU32(&bytes, 16, width);
    writeU32(&bytes, 20, width * 4);
    writeU32(&bytes, 28, declareMipChain ? mipCount : 0);
    writeU32(&bytes, 76, 32);
    writeU32(&bytes, 80, 0x00000041);
    writeU32(&bytes, 88, 32);
    writeU32(&bytes, 92, 0x000000FF);
    writeU32(&bytes, 96, 0x0000FF00);
    writeU32(&bytes, 100, 0x00FF0000);
    writeU32(&bytes, 104, 0xFF000000);
    writeU32(&bytes, 108, caps);

    for (std::size_t index = 128; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>((index - 128) & 0xFF);
    }
    return bytes;
}

std::vector<uint8_t> makeDds(
    uint32_t width,
    uint32_t height,
    uint32_t fourCc,
    uint32_t mipCount = 1,
    bool declareMipChain = false)
{
    const uint32_t blockSize = fourCc == FourCcDxt1 ? 8 : 16;
    uint64_t payloadSize = 0;
    uint32_t mipWidth = width;
    uint32_t mipHeight = height;
    for (uint32_t level = 0; level < mipCount; ++level) {
        payloadSize += levelSize(mipWidth, mipHeight, blockSize);
        mipWidth = std::max<uint32_t>(1, mipWidth >> 1);
        mipHeight = std::max<uint32_t>(1, mipHeight >> 1);
    }

    std::vector<uint8_t> bytes(static_cast<std::size_t>(128 + payloadSize), 0);
    bytes[0] = 'D';
    bytes[1] = 'D';
    bytes[2] = 'S';
    bytes[3] = ' ';
    writeU32(&bytes, 4, 124);
    uint32_t flags = 0x00001007;
    uint32_t caps = 0x00001000;
    if (declareMipChain) {
        flags |= 0x00020000;
        caps |= 0x00400000;
    }
    writeU32(&bytes, 8, flags);
    writeU32(&bytes, 12, height);
    writeU32(&bytes, 16, width);
    writeU32(&bytes, 28, declareMipChain ? mipCount : 0);
    writeU32(&bytes, 76, 32);
    writeU32(&bytes, 80, 0x00000004);
    writeU32(&bytes, 84, fourCc);
    writeU32(&bytes, 108, caps);

    for (std::size_t index = 128; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>((index - 128) & 0xFF);
    }
    return bytes;
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto nonce = std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
        path = std::filesystem::temp_directory_path()
            / ("xrphoton-texture-loader-" + std::to_string(nonce));
        std::filesystem::create_directories(path);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

bool writeFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

void expectPathRejected(
    std::string reference,
    std::string_view diagnostic,
    std::string_view description)
{
    std::filesystem::path relative = "unchanged";
    std::string error;
    const bool resolved = xrphoton::resolveLogicalTexturePath(
        reference,
        &relative,
        &error);
    expect(!resolved, description);
    expect(contains(error, diagnostic),
        std::string(description) + " names its violated rule");
    expect(relative == "unchanged",
        std::string(description) + " does not expose a partial path");
}

void testLogicalPathResolution()
{
    std::filesystem::path relative;
    std::string error = "stale";
    expect(
        xrphoton::resolveLogicalTexturePath(
            "ston\\ston_stena_marbl_m_03_back",
            &relative,
            &error),
        "canonical X-Ray logical reference resolves");
    expect(
        relative.generic_string() == "ston/ston_stena_marbl_m_03_back.dds",
        "canonical separators normalize and DDS extension is appended");
    expect(error.empty(), "successful logical resolution clears stale error text");

    expect(
        xrphoton::resolveLogicalTexturePath("A_1-b\\C-2_d", &relative, &error),
        "all explicitly accepted component characters resolve");

    expectPathRejected("", "empty references", "empty logical reference is rejected");
    expectPathRejected("ston.name", "dot bytes", "dot byte is rejected");
    expectPathRejected("\\ston", "leading or trailing", "leading separator is rejected");
    expectPathRejected("ston\\", "leading or trailing", "trailing separator is rejected");
    expectPathRejected("ston\\\\name", "empty path components",
        "empty middle component is rejected");
    expectPathRejected("ston/name", "forward-slash", "forward slash is rejected");
    expectPathRejected("C:\\ston", "colon bytes", "drive-qualified name is rejected");
    expectPathRejected("ston:name", "colon bytes", "alternate-stream shape is rejected");
    expectPathRejected(
        std::string({'s', 't', 'o', 'n', '\0', 'x'}),
        "NUL byte",
        "NUL byte is rejected");
    expectPathRejected(
        std::string({'s', 't', 'o', 'n', '\x1f', 'x'}),
        "control byte",
        "control byte is rejected");
    expectPathRejected(
        std::string({'s', 't', 'o', 'n', static_cast<char>(0x80)}),
        "outside the ASCII",
        "non-ASCII byte is rejected");
    expectPathRejected("ston@name", "component grammar",
        "unsupported ASCII punctuation is rejected");

    expect(
        !xrphoton::resolveLogicalTexturePath("ston", nullptr, &error),
        "null output path is rejected");
    expect(contains(error, "output path pointer is null"),
        "null output path has a named diagnostic");
}

void expectDdsRejected(
    const std::filesystem::path& path,
    const std::vector<uint8_t>& bytes,
    std::string_view diagnostic,
    std::string_view description)
{
    expect(writeFile(path, bytes), std::string(description) + " fixture writes");
    const xrphoton::TextureLoadResult loaded = xrphoton::loadTextureFile(path);
    expect(!loaded, description);
    expect(contains(loaded.error, diagnostic),
        std::string(description) + " names the rejected field/rule");
}

void testDdsDecode(const std::filesystem::path& directory)
{
    const std::filesystem::path fixture = directory / "fixture.dds";

    const std::vector<uint8_t> bc1 = makeDds(5, 7, FourCcDxt1);
    expect(writeFile(fixture, bc1), "BC1 fixture writes");
    xrphoton::TextureLoadResult loaded = xrphoton::loadTextureFile(fixture);
    expect(static_cast<bool>(loaded), "non-block-multiple BC1 DDS decodes");
    expect(
        loaded.image.format == xrphoton::SceneImageFormat::Bc1RgbaSrgb,
        "DXT1 maps to the CPU BC1 sRGB format");
    expect(loaded.image.width == 5 && loaded.image.height == 7,
        "BC1 dimensions survive decoding");
    expect(loaded.image.pixels.size() == 32,
        "BC1 mip-0 size rounds each dimension to whole blocks");
    expect(
        loaded.image.pixels
            == std::vector<uint8_t>(bc1.begin() + 128, bc1.end()),
        "BC1 compressed mip-0 bytes survive unchanged");

    const std::vector<uint8_t> bc3 = makeDds(3, 5, FourCcDxt5);
    expect(writeFile(fixture, bc3), "BC3 fixture writes");
    loaded = xrphoton::loadTextureFile(fixture);
    expect(static_cast<bool>(loaded), "non-block-multiple BC3 DDS decodes");
    expect(
        loaded.image.format == xrphoton::SceneImageFormat::Bc3Srgb,
        "DXT5 maps to the CPU BC3 sRGB format");
    expect(loaded.image.pixels.size() == 32,
        "BC3 mip-0 size uses 16-byte blocks");

    const std::vector<uint8_t> rgba8 = makeRgba8Dds(3, 2);
    expect(writeFile(fixture, rgba8), "RGBA8 fixture writes");
    loaded = xrphoton::loadTextureFile(fixture);
    expect(static_cast<bool>(loaded), "canonical uncompressed RGBA8 DDS decodes");
    expect(
        loaded.image.format == xrphoton::SceneImageFormat::Rgba8Srgb,
        "uncompressed RGBA8 maps to the CPU RGBA8 sRGB format");
    expect(loaded.image.width == 3 && loaded.image.height == 2,
        "RGBA8 dimensions survive decoding");
    expect(loaded.image.pixels.size() == 24,
        "RGBA8 mip-0 size is four bytes per texel");
    expect(
        loaded.image.pixels
            == std::vector<uint8_t>(rgba8.begin() + 128, rgba8.end()),
        "RGBA8 mip-0 channel bytes survive unchanged");

    const std::vector<uint8_t> rgbaMipChain = makeRgba8Dds(4, 2, 3, true);
    expect(writeFile(fixture, rgbaMipChain), "RGBA8 full mip-chain fixture writes");
    loaded = xrphoton::loadTextureFile(fixture);
    expect(static_cast<bool>(loaded), "declared complete RGBA8 mip chain frames exactly");
    expect(loaded.image.pixels.size() == 32,
        "only RGBA8 mip zero is retained from a validated complete chain");

    const std::vector<uint8_t> mipChain = makeDds(5, 3, FourCcDxt1, 3, true);
    expect(writeFile(fixture, mipChain), "full mip-chain fixture writes");
    loaded = xrphoton::loadTextureFile(fixture);
    expect(static_cast<bool>(loaded), "declared complete DDS mip chain frames exactly");
    expect(loaded.image.pixels.size() == 16,
        "only mip zero is retained from a validated complete chain");
    expect(
        std::equal(
            loaded.image.pixels.begin(),
            loaded.image.pixels.end(),
            mipChain.begin() + 128),
        "mip-zero bytes are read from the front of the complete chain");

    std::vector<uint8_t> malformed = bc1;
    malformed[0] = 'X';
    expectDdsRejected(fixture, malformed, "DDS magic", "wrong DDS magic is rejected");

    malformed = bc1;
    writeU32(&malformed, 4, 123);
    expectDdsRejected(fixture, malformed, "dwSize", "wrong DDS header size is rejected");

    malformed = bc1;
    writeU32(&malformed, 8, 0);
    expectDdsRejected(fixture, malformed, "dwFlags", "missing required flags are rejected");

    malformed = bc1;
    writeU32(&malformed, 16, 0);
    expectDdsRejected(fixture, malformed, "dwWidth", "zero width is rejected");

    malformed = bc1;
    writeU32(&malformed, 76, 31);
    expectDdsRejected(fixture, malformed, "DDS_PIXELFORMAT.dwSize",
        "wrong pixel-format size is rejected");

    malformed = bc1;
    writeU32(&malformed, 80, 0);
    expectDdsRejected(fixture, malformed, "DDS_PIXELFORMAT.dwFlags",
        "unknown pixel-format shape is rejected");

    malformed = bc1;
    writeU32(&malformed, 80, 0x00000044);
    writeU32(&malformed, 88, 32);
    writeU32(&malformed, 92, 0x00FF0000);
    expectDdsRejected(fixture, malformed, "expected exactly",
        "hybrid FOURCC and RGB-mask flags are rejected");

    malformed = bc1;
    writeU32(&malformed, 92, 0x00FF0000);
    expectDdsRejected(fixture, malformed, "color masks",
        "nonzero color masks are rejected even with a FOURCC-only flag word");

    malformed = rgba8;
    writeU32(&malformed, 92, 0x00FF0000);
    expectDdsRejected(fixture, malformed, "RGBA8 layout",
        "noncanonical RGBA8 channel masks are rejected");

    malformed = rgba8;
    writeU32(&malformed, 8, 0x00001007);
    expectDdsRejected(fixture, malformed, "DDSD_PITCH",
        "RGBA8 without a pitch flag is rejected");

    malformed = rgba8;
    writeU32(&malformed, 20, 16);
    expectDdsRejected(fixture, malformed, "row pitch",
        "RGBA8 with a noncanonical row pitch is rejected");

    malformed = rgba8;
    writeU32(&malformed, 84, FourCcDxt1);
    expectDdsRejected(fixture, malformed, "expected zero",
        "RGBA8 with a nonzero FOURCC is rejected");

    malformed = bc1;
    writeU32(&malformed, 84, FourCcDxt3);
    expectDdsRejected(fixture, malformed, "DXT3", "unsupported DXT3 is named");

    malformed = bc1;
    writeU32(&malformed, 112, 0x00000200);
    expectDdsRejected(fixture, malformed, "cubemap", "cubemap DDS is rejected");

    malformed = bc1;
    writeU32(&malformed, 112, 0x00200000);
    expectDdsRejected(fixture, malformed, "volume", "volume DDS is rejected");

    malformed = mipChain;
    writeU32(&malformed, 108, 0x00001000);
    expectDdsRejected(fixture, malformed, "disagree",
        "mipmap flag/caps disagreement is rejected");

    malformed = mipChain;
    writeU32(&malformed, 28, 2);
    expectDdsRejected(fixture, malformed, "dwMipMapCount",
        "partial mip chain count is rejected");

    malformed = bc1;
    malformed.pop_back();
    expectDdsRejected(fixture, malformed, "truncated",
        "truncated block payload is rejected by exact framing");

    malformed = bc1;
    malformed.push_back(0);
    expectDdsRejected(fixture, malformed, "trailing bytes",
        "trailing block payload bytes are rejected by exact framing");

    const xrphoton::TextureLoadResult missing =
        xrphoton::loadTextureFile(directory / "missing.dds");
    expect(!missing && contains(missing.error, "file open failed"),
        "missing DDS reports file-open failure");
}

xrphoton::SceneMaterial material(std::string reference, uint32_t staleImage = 0)
{
    xrphoton::SceneMaterial result{};
    result.baseColorTexture = std::move(reference);
    result.baseColorImage = staleImage;
    return result;
}

void testSceneResolution(const std::filesystem::path& directory)
{
    xrphoton::SceneData fallbackScene{};
    fallbackScene.materials.push_back(material("", 99));
    xrphoton::SceneImage stale{};
    stale.width = 7;
    fallbackScene.images.push_back(std::move(stale));
    xrphoton::ResolveTexturesResult resolved =
        xrphoton::resolveSceneTextures(&fallbackScene, {});
    expect(static_cast<bool>(resolved), "root-less untextured scene resolves");
    expect(fallbackScene.images.size() == 1,
        "resolver replaces prior images with exactly the fallback when untextured");
    expect(
        fallbackScene.images[0].format == xrphoton::SceneImageFormat::Rgba8Srgb
            && fallbackScene.images[0].width == 1
            && fallbackScene.images[0].height == 1
            && fallbackScene.images[0].pixels
                == std::vector<uint8_t>({255, 255, 255, 255}),
        "image zero is the generated opaque-white RGBA8 sRGB fallback");
    expect(fallbackScene.materials[0].baseColorImage == 0,
        "untextured material is reset to fallback image zero");

    xrphoton::SceneData missingRoot{};
    missingRoot.materials.push_back(material("stone\\needed", 17));
    missingRoot.images.push_back(fallbackScene.images[0]);
    resolved = xrphoton::resolveSceneTextures(&missingRoot, {});
    expect(!resolved, "referenced texture without configured root fails");
    expect(resolved.failedMaterial == 0,
        "missing-root failure identifies the first referenced material");
    expect(contains(resolved.error, "XRPHOTON_GALLERY_TEXTURE_ROOT"),
        "missing-root diagnostic names the required configuration");
    expect(missingRoot.materials[0].baseColorImage == 17
            && missingRoot.images.size() == 1,
        "missing-root failure is transactional");

    xrphoton::SceneData missingFile{};
    missingFile.materials.push_back(material("stone\\absent"));
    resolved = xrphoton::resolveSceneTextures(&missingFile, directory);
    expect(!resolved && resolved.failedMaterial == 0,
        "missing referenced file identifies its material");
    expect(contains(resolved.error, "stone/absent.dds")
            && contains(resolved.error, "file open failed"),
        "missing-file diagnostic names normalized path and cause");

    xrphoton::SceneData malformedReference{};
    malformedReference.materials.push_back(material("stone/invalid"));
    resolved = xrphoton::resolveSceneTextures(&malformedReference, directory);
    expect(!resolved && resolved.failedMaterial == 0,
        "malformed scene reference identifies its material");
    expect(contains(resolved.error, "forward-slash"),
        "malformed scene reference preserves normalization diagnostic");

    std::vector<uint8_t> secondBytes = makeDds(1, 1, FourCcDxt1);
    secondBytes[128] = 0x22;
    std::vector<uint8_t> firstBytes = makeDds(1, 1, FourCcDxt5);
    firstBytes[128] = 0x11;
    expect(writeFile(directory / "stone" / "second.dds", secondBytes),
        "second first-use texture writes");
    expect(writeFile(directory / "stone" / "first.dds", firstBytes),
        "first texture writes");

    xrphoton::SceneData scene{};
    scene.materials = {
        material("stone\\second"),
        material(""),
        material("stone\\first"),
        material("stone\\second"),
    };
    resolved = xrphoton::resolveSceneTextures(&scene, directory);
    expect(static_cast<bool>(resolved), "mixed referenced/unreferenced scene resolves");
    expect(scene.images.size() == 3,
        "duplicate normalized texture path creates only one scene image");
    expect(scene.materials[0].baseColorImage == 1
            && scene.materials[1].baseColorImage == 0
            && scene.materials[2].baseColorImage == 2
            && scene.materials[3].baseColorImage == 1,
        "image indices follow stable first-use order with fallback at zero");
    expect(scene.images[1].format == xrphoton::SceneImageFormat::Bc1RgbaSrgb
            && scene.images[1].pixels[0] == 0x22,
        "first-used BC1 image occupies scene image one");
    expect(scene.images[2].format == xrphoton::SceneImageFormat::Bc3Srgb
            && scene.images[2].pixels[0] == 0x11,
        "second distinct BC3 image occupies scene image two");

    const std::filesystem::path authoredRoot = directory / "authored-root";
    const std::filesystem::path legacyRoot = directory / "legacy-root";
    std::vector<uint8_t> legacyOverlay = makeDds(1, 1, FourCcDxt1);
    legacyOverlay[128] = 0x55;
    expect(writeFile(
               legacyRoot / "overlay" / "shared.dds",
               legacyOverlay),
        "fallback overlay texture writes");
    const std::array overlayRoots{authoredRoot, legacyRoot};
    xrphoton::SceneData overlayScene{};
    overlayScene.materials = {material("overlay\\shared")};
    resolved = xrphoton::resolveSceneTexturesFromRoots(
        &overlayScene, overlayRoots);
    expect(static_cast<bool>(resolved)
            && overlayScene.images.size() == 2
            && overlayScene.images[1].pixels[0] == 0x55,
        "ordered texture roots fall back to the first root containing the path");

    std::vector<uint8_t> authoredOverlay = makeDds(1, 1, FourCcDxt1);
    authoredOverlay[128] = 0x44;
    expect(writeFile(
               authoredRoot / "overlay" / "shared.dds",
               authoredOverlay),
        "primary overlay texture writes");
    overlayScene = {};
    overlayScene.materials = {material("overlay\\shared")};
    resolved = xrphoton::resolveSceneTexturesFromRoots(
        &overlayScene, overlayRoots);
    expect(static_cast<bool>(resolved)
            && overlayScene.images.size() == 2
            && overlayScene.images[1].pixels[0] == 0x44,
        "the earliest configured texture root deterministically shadows later roots");

    xrphoton::SceneData capped{};
    capped.materials = {
        material("stone\\second"),
        material("stone\\third"),
    };
    expect(writeFile(
               directory / "stone" / "third.dds",
               makeDds(1, 1, FourCcDxt1)),
        "cap-boundary texture writes");
    resolved = xrphoton::texture_loader_detail::resolveSceneTexturesWithByteLimit(
        &capped,
        directory,
        8);
    expect(!resolved && resolved.failedMaterial == 1,
        "cumulative mip-zero byte cap rejects the first overflowing material");
    expect(contains(resolved.error, "MaxSceneTextureBytes cap"),
        "cumulative-cap diagnostic names MaxSceneTextureBytes");
    expect(capped.images.empty()
            && capped.materials[0].baseColorImage == 0
            && capped.materials[1].baseColorImage == 0,
        "cumulative-cap failure exposes no partial resolution");

    resolved = xrphoton::resolveSceneTextures(nullptr, directory);
    expect(!resolved && !resolved.failedMaterial.has_value()
            && contains(resolved.error, "scene pointer is null"),
        "null scene is a named scene-global failure");
}
}

int main()
{
    try {
        TemporaryDirectory temporary;
        testLogicalPathResolution();
        testDdsDecode(temporary.path);
        testSceneResolution(temporary.path);
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: unexpected exception: " << exception.what() << '\n';
        ++failureCount;
    }

    if (failureCount != 0) {
        std::cerr << failureCount << " texture-loader test(s) failed\n";
        return 1;
    }
    std::cout << "texture-loader tests passed\n";
    return 0;
}
