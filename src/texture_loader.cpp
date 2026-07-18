#include "texture_loader.hpp"

#include "texture_loader_detail.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xrphoton
{
namespace
{
constexpr uint32_t DdsHeaderSize = 124;
constexpr uint32_t DdsPixelFormatSize = 32;
constexpr uint32_t MaximumTextureDimension = 16384;
constexpr std::size_t DdsFileHeaderSize = 128;

constexpr uint32_t DdsdCaps = 0x00000001;
constexpr uint32_t DdsdHeight = 0x00000002;
constexpr uint32_t DdsdWidth = 0x00000004;
constexpr uint32_t DdsdPixelFormat = 0x00001000;
constexpr uint32_t DdsdMipMapCount = 0x00020000;
constexpr uint32_t DdsdDepth = 0x00800000;
constexpr uint32_t RequiredDdsFlags =
    DdsdCaps | DdsdHeight | DdsdWidth | DdsdPixelFormat;

constexpr uint32_t DdpfFourCc = 0x00000004;
constexpr uint32_t DdsCapsTexture = 0x00001000;
constexpr uint32_t DdsCapsMipMap = 0x00400000;
constexpr uint32_t DdsCaps2CubeMap = 0x00000200;
constexpr uint32_t DdsCaps2CubeMapFaces = 0x0000FC00;
constexpr uint32_t DdsCaps2Volume = 0x00200000;

constexpr uint32_t makeFourCc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<unsigned char>(a))
        | (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8)
        | (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16)
        | (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

constexpr uint32_t FourCcDxt1 = makeFourCc('D', 'X', 'T', '1');
constexpr uint32_t FourCcDxt5 = makeFourCc('D', 'X', 'T', '5');

uint32_t readU32(const std::array<uint8_t, DdsFileHeaderSize>& bytes, std::size_t offset)
{
    return static_cast<uint32_t>(bytes[offset])
        | (static_cast<uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

bool asciiAlphaNumeric(unsigned char byte)
{
    return (byte >= static_cast<unsigned char>('A')
               && byte <= static_cast<unsigned char>('Z'))
        || (byte >= static_cast<unsigned char>('a')
            && byte <= static_cast<unsigned char>('z'))
        || (byte >= static_cast<unsigned char>('0')
            && byte <= static_cast<unsigned char>('9'));
}

std::string quotedReference(std::string_view reference)
{
    return "texture reference \"" + std::string(reference) + "\"";
}

bool rejectPath(
    std::string_view reference,
    std::string_view rule,
    std::string* error)
{
    if (error != nullptr) {
        *error = quotedReference(reference) + ": " + std::string(rule);
    }
    return false;
}

std::string fourCcName(uint32_t value)
{
    std::string name(4, '?');
    for (std::size_t index = 0; index < name.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(value >> (index * 8));
        name[index] = byte >= 0x20 && byte <= 0x7E ? static_cast<char>(byte) : '?';
    }
    return name;
}

uint32_t fullMipCount(uint32_t width, uint32_t height)
{
    uint32_t dimension = std::max(width, height);
    uint32_t count = 1;
    while (dimension > 1) {
        dimension >>= 1;
        ++count;
    }
    return count;
}

uint64_t levelByteSize(uint32_t width, uint32_t height, uint32_t blockSize)
{
    const uint64_t blockWidth = std::max<uint64_t>(1, (uint64_t{width} + 3) / 4);
    const uint64_t blockHeight = std::max<uint64_t>(1, (uint64_t{height} + 3) / 4);
    return blockWidth * blockHeight * blockSize;
}

TextureLoadResult rejectLoad(std::string error)
{
    TextureLoadResult result{};
    result.error = std::move(error);
    return result;
}

ResolveTexturesResult rejectResolve(
    std::string error,
    std::optional<uint32_t> material = std::nullopt)
{
    ResolveTexturesResult result{};
    result.error = std::move(error);
    result.failedMaterial = material;
    return result;
}

std::string materialPrefix(uint32_t materialIndex, std::string_view reference)
{
    return "texture resolver: material " + std::to_string(materialIndex) + " "
        + quotedReference(reference);
}
}

bool resolveLogicalTexturePath(
    std::string_view logicalReference,
    std::filesystem::path* relativePath,
    std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }
    if (relativePath == nullptr) {
        if (error != nullptr) {
            *error = "texture reference resolution: output path pointer is null";
        }
        return false;
    }

    try {
        if (logicalReference.empty()) {
            return rejectPath(logicalReference, "empty references are not allowed", error);
        }

        for (std::size_t index = 0; index < logicalReference.size(); ++index) {
            const unsigned char byte =
                static_cast<unsigned char>(logicalReference[index]);
            if (byte == 0) {
                return rejectPath(
                    logicalReference,
                    "NUL byte at offset " + std::to_string(index) + " is not allowed",
                    error);
            }
            if (byte < 0x20 || byte == 0x7F) {
                return rejectPath(
                    logicalReference,
                    "control byte at offset " + std::to_string(index) + " is not allowed",
                    error);
            }
            if (byte == static_cast<unsigned char>('/')) {
                return rejectPath(
                    logicalReference,
                    "forward-slash separators are not allowed; use canonical backslashes",
                    error);
            }
            if (byte == static_cast<unsigned char>('.')) {
                return rejectPath(
                    logicalReference,
                    "dot bytes are not allowed in extensionless logical names",
                    error);
            }
            if (byte == static_cast<unsigned char>(':')) {
                return rejectPath(
                    logicalReference,
                    "colon bytes and drive-qualified paths are not allowed",
                    error);
            }
            if (byte != static_cast<unsigned char>('\\')
                && byte != static_cast<unsigned char>('_')
                && byte != static_cast<unsigned char>('-')
                && !asciiAlphaNumeric(byte)) {
                return rejectPath(
                    logicalReference,
                    "byte at offset " + std::to_string(index)
                        + " is outside the ASCII [A-Za-z0-9_-] component grammar",
                    error);
            }
        }

        if (logicalReference.front() == '\\' || logicalReference.back() == '\\') {
            return rejectPath(
                logicalReference,
                "leading or trailing separators (absolute-path shapes) are not allowed",
                error);
        }

        std::string normalized;
        normalized.reserve(logicalReference.size() + 4);
        bool previousWasSeparator = false;
        for (char byte : logicalReference) {
            if (byte == '\\') {
                if (previousWasSeparator) {
                    return rejectPath(
                        logicalReference,
                        "empty path components are not allowed",
                        error);
                }
                normalized.push_back('/');
                previousWasSeparator = true;
            } else {
                normalized.push_back(byte);
                previousWasSeparator = false;
            }
        }
        normalized += ".dds";

        std::filesystem::path resolved(normalized);
        *relativePath = std::move(resolved);
        return true;
    } catch (const std::bad_alloc&) {
        if (error != nullptr) {
            *error = "texture reference resolution: resource allocation failed";
        }
        return false;
    } catch (const std::length_error&) {
        if (error != nullptr) {
            *error = "texture reference resolution: resource allocation failed";
        }
        return false;
    }
}

TextureLoadResult loadTextureFile(const std::filesystem::path& path)
{
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input.is_open()) {
            return rejectLoad("file open failed");
        }

        const std::streampos end = input.tellg();
        if (end < 0) {
            return rejectLoad("file size query failed");
        }
        const uint64_t fileSize = static_cast<uint64_t>(end);
        if (fileSize < DdsFileHeaderSize) {
            return rejectLoad(
                "DDS header is truncated: expected at least 128 bytes, found "
                + std::to_string(fileSize));
        }
        input.seekg(0, std::ios::beg);
        if (!input) {
            return rejectLoad("file seek failed");
        }

        std::array<uint8_t, DdsFileHeaderSize> header{};
        input.read(
            reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
        if (input.gcount() != static_cast<std::streamsize>(header.size())) {
            return rejectLoad("DDS header read failed");
        }

        if (header[0] != 'D' || header[1] != 'D'
            || header[2] != 'S' || header[3] != ' ') {
            return rejectLoad("DDS magic: expected 'DDS '");
        }
        if (readU32(header, 4) != DdsHeaderSize) {
            return rejectLoad(
                "DDS_HEADER.dwSize: expected 124, found "
                + std::to_string(readU32(header, 4)));
        }

        const uint32_t flags = readU32(header, 8);
        if ((flags & RequiredDdsFlags) != RequiredDdsFlags) {
            return rejectLoad(
                "DDS_HEADER.dwFlags: required CAPS|HEIGHT|WIDTH|PIXELFORMAT flags are missing");
        }

        const uint32_t height = readU32(header, 12);
        const uint32_t width = readU32(header, 16);
        if (width == 0 || width > MaximumTextureDimension) {
            return rejectLoad(
                "DDS_HEADER.dwWidth: expected 1..16384, found "
                + std::to_string(width));
        }
        if (height == 0 || height > MaximumTextureDimension) {
            return rejectLoad(
                "DDS_HEADER.dwHeight: expected 1..16384, found "
                + std::to_string(height));
        }

        const uint32_t depth = readU32(header, 24);
        const uint32_t caps2 = readU32(header, 112);
        if ((caps2 & (DdsCaps2CubeMap | DdsCaps2CubeMapFaces)) != 0) {
            return rejectLoad("DDS_HEADER.dwCaps2: cubemap textures are not supported");
        }
        if ((caps2 & DdsCaps2Volume) != 0 || (flags & DdsdDepth) != 0 || depth != 0) {
            return rejectLoad("DDS header: volume textures are not supported");
        }
        if (caps2 != 0) {
            return rejectLoad(
                "DDS_HEADER.dwCaps2: unsupported 2D texture capability bits found");
        }

        if (readU32(header, 76) != DdsPixelFormatSize) {
            return rejectLoad(
                "DDS_PIXELFORMAT.dwSize: expected 32, found "
                + std::to_string(readU32(header, 76)));
        }
        const uint32_t pixelFlags = readU32(header, 80);
        if (pixelFlags != DdpfFourCc) {
            return rejectLoad(
                "DDS_PIXELFORMAT.dwFlags: expected exactly DDPF_FOURCC; "
                "uncompressed masks, palettes, and extra format flags are unsupported");
        }
        if (readU32(header, 88) != 0
            || readU32(header, 92) != 0
            || readU32(header, 96) != 0
            || readU32(header, 100) != 0
            || readU32(header, 104) != 0) {
            return rejectLoad(
                "DDS_PIXELFORMAT RGB bit count/color masks: expected zero for DXT1/DXT5");
        }

        const uint32_t fourCc = readU32(header, 84);
        uint32_t blockSize = 0;
        SceneImageFormat format = SceneImageFormat::Rgba8Srgb;
        if (fourCc == FourCcDxt1) {
            blockSize = 8;
            format = SceneImageFormat::Bc1RgbaSrgb;
        } else if (fourCc == FourCcDxt5) {
            blockSize = 16;
            format = SceneImageFormat::Bc3Srgb;
        } else {
            return rejectLoad(
                "DDS_PIXELFORMAT.dwFourCC: unsupported fourCC '"
                + fourCcName(fourCc) + "' (expected DXT1 or DXT5)");
        }

        const uint32_t caps = readU32(header, 108);
        if ((caps & DdsCapsTexture) == 0) {
            return rejectLoad("DDS_HEADER.dwCaps: DDSCAPS_TEXTURE is required");
        }

        const bool hasMipFlag = (flags & DdsdMipMapCount) != 0;
        const bool hasMipCap = (caps & DdsCapsMipMap) != 0;
        const uint32_t storedMipCount = readU32(header, 28);
        if (hasMipFlag != hasMipCap) {
            return rejectLoad(
                "DDS mip framing: DDSD_MIPMAPCOUNT and DDSCAPS_MIPMAP disagree");
        }

        const uint32_t completeMipCount = fullMipCount(width, height);
        uint32_t declaredMipCount = 1;
        if (hasMipFlag) {
            if (storedMipCount != 1 && storedMipCount != completeMipCount) {
                return rejectLoad(
                    "DDS_HEADER.dwMipMapCount: expected 1 or full chain count "
                    + std::to_string(completeMipCount) + ", found "
                    + std::to_string(storedMipCount));
            }
            declaredMipCount = storedMipCount;
        } else if (storedMipCount > 1) {
            return rejectLoad(
                "DDS mip framing: dwMipMapCount declares multiple levels without mip flags/caps");
        }

        const uint64_t mipZeroSize = levelByteSize(width, height, blockSize);
        uint64_t payloadSize = 0;
        uint32_t mipWidth = width;
        uint32_t mipHeight = height;
        for (uint32_t level = 0; level < declaredMipCount; ++level) {
            payloadSize += levelByteSize(mipWidth, mipHeight, blockSize);
            mipWidth = std::max<uint32_t>(1, mipWidth >> 1);
            mipHeight = std::max<uint32_t>(1, mipHeight >> 1);
        }
        const uint64_t expectedFileSize = DdsFileHeaderSize + payloadSize;
        if (fileSize < expectedFileSize) {
            return rejectLoad(
                "DDS payload is truncated: expected "
                + std::to_string(expectedFileSize) + " bytes, found "
                + std::to_string(fileSize));
        }
        if (fileSize > expectedFileSize) {
            return rejectLoad(
                "DDS file has trailing bytes: expected "
                + std::to_string(expectedFileSize) + " bytes, found "
                + std::to_string(fileSize));
        }
        if (mipZeroSize > std::numeric_limits<std::size_t>::max()
            || mipZeroSize > static_cast<uint64_t>(
                std::numeric_limits<std::streamsize>::max())) {
            return rejectLoad("DDS mip-0 payload exceeds host addressable size");
        }

        TextureLoadResult result{};
        result.image.format = format;
        result.image.width = width;
        result.image.height = height;
        result.image.pixels.resize(static_cast<std::size_t>(mipZeroSize));
        input.read(
            reinterpret_cast<char*>(result.image.pixels.data()),
            static_cast<std::streamsize>(result.image.pixels.size()));
        if (input.gcount() != static_cast<std::streamsize>(result.image.pixels.size())) {
            return rejectLoad("DDS mip-0 payload read failed");
        }
        return result;
    } catch (const std::bad_alloc&) {
        return rejectLoad("texture load: resource allocation failed");
    } catch (const std::length_error&) {
        return rejectLoad("texture load: resource allocation failed");
    }
}

namespace texture_loader_detail
{
ResolveTexturesResult resolveSceneTexturesWithByteLimit(
    SceneData* scene,
    const std::filesystem::path& textureRoot,
    uint64_t byteLimit)
{
    if (scene == nullptr) {
        return rejectResolve("texture resolver: scene pointer is null");
    }

    try {
        std::vector<SceneImage> resolvedImages;
        resolvedImages.reserve(scene->materials.size() + 1);

        SceneImage fallback{};
        fallback.format = SceneImageFormat::Rgba8Srgb;
        fallback.width = 1;
        fallback.height = 1;
        fallback.pixels = {255, 255, 255, 255};
        resolvedImages.push_back(std::move(fallback));

        std::vector<uint32_t> materialImageIndices(scene->materials.size(), 0);
        std::unordered_map<std::string, uint32_t> imageByRelativePath;
        imageByRelativePath.reserve(scene->materials.size());
        uint64_t decodedTextureBytes = 0;

        for (std::size_t materialOffset = 0;
             materialOffset < scene->materials.size();
             ++materialOffset) {
            if (materialOffset > std::numeric_limits<uint32_t>::max()) {
                return rejectResolve(
                    "texture resolver: material index exceeds UINT32_MAX");
            }
            const uint32_t materialIndex = static_cast<uint32_t>(materialOffset);
            const std::string& reference =
                scene->materials[materialOffset].baseColorTexture;
            if (reference.empty()) {
                continue;
            }

            if (textureRoot.empty()) {
                return rejectResolve(
                    materialPrefix(materialIndex, reference)
                        + ": texture root is not configured (expected "
                          "XRPHOTON_GALLERY_TEXTURE_ROOT to be configured)",
                    materialIndex);
            }

            std::filesystem::path relativePath;
            std::string pathError;
            if (!resolveLogicalTexturePath(reference, &relativePath, &pathError)) {
                return rejectResolve(
                    "texture resolver: material " + std::to_string(materialIndex)
                        + ": " + pathError,
                    materialIndex);
            }

            const std::string key = relativePath.generic_string();
            const auto existing = imageByRelativePath.find(key);
            if (existing != imageByRelativePath.end()) {
                materialImageIndices[materialOffset] = existing->second;
                continue;
            }

            const std::filesystem::path resolvedPath = textureRoot / relativePath;
            TextureLoadResult loaded = loadTextureFile(resolvedPath);
            if (!loaded) {
                return rejectResolve(
                    materialPrefix(materialIndex, reference) + ": resolved to "
                        + resolvedPath.string() + ": " + loaded.error,
                    materialIndex);
            }

            const uint64_t imageBytes = loaded.image.pixels.size();
            if (decodedTextureBytes > byteLimit
                || imageBytes > byteLimit - decodedTextureBytes) {
                return rejectResolve(
                    materialPrefix(materialIndex, reference)
                        + ": cumulative decoded mip-0 payload exceeds the "
                          "MaxSceneTextureBytes cap ("
                        + std::to_string(byteLimit) + " bytes)",
                    materialIndex);
            }
            decodedTextureBytes += imageBytes;

            if (resolvedImages.size() > std::numeric_limits<uint32_t>::max()) {
                return rejectResolve(
                    materialPrefix(materialIndex, reference)
                        + ": resolved image index exceeds UINT32_MAX",
                    materialIndex);
            }
            const uint32_t imageIndex =
                static_cast<uint32_t>(resolvedImages.size());
            resolvedImages.push_back(std::move(loaded.image));
            imageByRelativePath.emplace(key, imageIndex);
            materialImageIndices[materialOffset] = imageIndex;
        }

        scene->images = std::move(resolvedImages);
        for (std::size_t index = 0; index < scene->materials.size(); ++index) {
            scene->materials[index].baseColorImage = materialImageIndices[index];
        }
        return {};
    } catch (const std::bad_alloc&) {
        return rejectResolve("texture resolver: resource allocation failed");
    } catch (const std::length_error&) {
        return rejectResolve("texture resolver: resource allocation failed");
    }
}
}

ResolveTexturesResult resolveSceneTextures(
    SceneData* scene,
    const std::filesystem::path& textureRoot)
{
    return texture_loader_detail::resolveSceneTexturesWithByteLimit(
        scene,
        textureRoot,
        MaxSceneTextureBytes);
}
}
