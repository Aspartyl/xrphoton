#include "blender_mesh.hpp"
#include "legacy_ogf.hpp"
#include "ogfx.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
class TemporaryOutputGuard
{
public:
    explicit TemporaryOutputGuard(const std::filesystem::path& path)
        : path_(path)
    {
    }

    ~TemporaryOutputGuard()
    {
        if (armed_) {
            std::error_code removeError;
            std::filesystem::remove(path_, removeError);
        }
    }

    void arm() noexcept { armed_ = true; }
    void release() noexcept { armed_ = false; }

private:
    const std::filesystem::path& path_;
    bool armed_ = false;
};

bool pathsAlias(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    if (left.lexically_normal() == right.lexically_normal()) {
        return true;
    }
    std::error_code equivalentError;
    return std::filesystem::equivalent(left, right, equivalentError)
        && !equivalentError;
}

bool pathAliasesRunningCompiler(
    const std::filesystem::path& invokedPath,
    const std::filesystem::path& outputPath)
{
    if (pathsAlias(invokedPath, outputPath)) {
        return true;
    }

    // argv[0] may be a bare PATH lookup, so it is not enough to compare that
    // spelling from the caller's working directory. Linux is the currently
    // supported host; /proc pins the executable that publication must protect.
    std::error_code executableError;
    const std::filesystem::path runningPath =
        std::filesystem::read_symlink("/proc/self/exe", executableError);
    return !executableError && pathsAlias(runningPath, outputPath);
}

bool readSourceFile(
    const std::filesystem::path& path,
    std::string_view diagnosticName,
    std::vector<std::uint8_t>* bytes,
    std::string* error)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        *error = std::string(diagnosticName) + ": failed to open the source OGF file.";
        return false;
    }

    const std::streampos end = input.tellg();
    if (end < 0) {
        *error = std::string(diagnosticName) + ": failed to determine the source file size.";
        return false;
    }
    const std::uint64_t size =
        static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
    if (size > xrphoton::ogfx::MaximumFileBytes) {
        *error = std::string(diagnosticName)
            + ": source file byte size exceeds the 1 GiB compiler cap.";
        return false;
    }
    if (size > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamsize>::max())) {
        *error = std::string(diagnosticName)
            + ": source file is too large for one complete stream read.";
        return false;
    }

    bytes->resize(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!input) {
        *error = std::string(diagnosticName) + ": failed to seek to the source file start.";
        return false;
    }
    if (!bytes->empty()) {
        input.read(
            reinterpret_cast<char*>(bytes->data()),
            static_cast<std::streamsize>(bytes->size()));
    }
    if (!input) {
        *error = std::string(diagnosticName) + ": failed to read the complete source file.";
        return false;
    }
    if (input.peek() != std::char_traits<char>::eof()) {
        *error = std::string(diagnosticName)
            + ": source file changed size while it was being read.";
        return false;
    }
    return true;
}

bool readSourceStream(
    std::istream& input,
    std::string_view diagnosticName,
    std::vector<std::uint8_t>* bytes,
    std::string* error)
{
    bytes->clear();
    constexpr std::uint64_t MaximumBlenderStreamBytes =
        xrphoton::blender_mesh::StreamHeaderSize
        + static_cast<std::uint64_t>(
            xrphoton::blender_mesh::MaximumTriangleCount)
            * xrphoton::blender_mesh::CornersPerTriangle
            * xrphoton::blender_mesh::CornerRecordSize;
    constexpr std::size_t ReadBlockSize = 64 * 1024;
    std::array<char, ReadBlockSize> block{};
    while (true) {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        const std::streamsize readCount = input.gcount();
        if (readCount < 0) {
            *error = std::string(diagnosticName)
                + ": failed to read the Blender exchange stream.";
            return false;
        }
        if (readCount != 0) {
            const std::uint64_t newSize =
                static_cast<std::uint64_t>(bytes->size())
                + static_cast<std::uint64_t>(readCount);
            if (newSize > MaximumBlenderStreamBytes) {
                *error = std::string(diagnosticName)
                    + ": Blender exchange stream exceeds the "
                    + std::to_string(MaximumBlenderStreamBytes)
                    + "-byte profile cap.";
                return false;
            }
            const auto* first = reinterpret_cast<const std::uint8_t*>(block.data());
            bytes->insert(bytes->end(), first, first + readCount);
        }
        if (input.eof()) {
            break;
        }
        if (!input) {
            *error = std::string(diagnosticName)
                + ": failed to read the complete Blender exchange stream.";
            return false;
        }
    }
    return true;
}

bool publishOutput(
    const std::filesystem::path& outputPath,
    std::string_view diagnosticName,
    const std::vector<std::uint8_t>& bytes,
    std::string* error)
{
    std::error_code directoryError;
    const std::filesystem::path parentPath = outputPath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath, directoryError);
    }
    if (directoryError) {
        *error = std::string(diagnosticName)
            + ": failed to create the output directory: "
            + directoryError.message() + '.';
        return false;
    }
    if (static_cast<std::uint64_t>(bytes.size())
        > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamsize>::max())) {
        *error = std::string(diagnosticName)
            + ": output is too large for one complete stream write.";
        return false;
    }

    // Exclusive adjacent temporaries keep a stale file, symlink, hard link, or
    // concurrent converter from being followed or truncated before publication.
    std::filesystem::path temporaryPath;
    TemporaryOutputGuard temporaryGuard(temporaryPath);
    std::ofstream output;
    constexpr std::uint32_t MaximumTemporaryAttempts = 256;
    for (std::uint32_t attempt = 0; attempt < MaximumTemporaryAttempts; ++attempt) {
        temporaryPath = outputPath;
        temporaryPath += ".tmp." + std::to_string(attempt);
        std::ofstream candidate(
            temporaryPath,
            std::ios::binary | std::ios::out | std::ios::noreplace);
        if (candidate) {
            temporaryGuard.arm();
            output = std::move(candidate);
            break;
        }

        std::error_code existsError;
        const bool exists = std::filesystem::exists(temporaryPath, existsError);
        if (existsError || !exists) {
            *error = std::string(diagnosticName)
                + ": failed to create an exclusive temporary output file.";
            return false;
        }
    }
    if (!output.is_open()) {
        *error = std::string(diagnosticName)
            + ": no unused temporary output name is available.";
        return false;
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        *error = std::string(diagnosticName)
            + ": failed to write the complete temporary output file.";
        return false;
    }

    std::error_code renameError;
    std::filesystem::rename(temporaryPath, outputPath, renameError);
    if (renameError) {
        *error = std::string(diagnosticName)
            + ": failed to publish the output file: "
            + renameError.message() + '.';
        return false;
    }
    temporaryGuard.release();
    return true;
}

void printUsage()
{
    std::cerr
        << "Usage:\n"
        << "  xrPhotonAssetCompiler convert-ogf <input.ogf> <output.ogfx>\n"
        << "  xrPhotonAssetCompiler convert-blender <source-name> <output.ogfx> < XRBM\n";
}

int convertLegacyOgf(char** arguments)
{
    const std::filesystem::path inputPath = arguments[2];
    const std::filesystem::path outputPath = arguments[3];
    if (pathsAlias(inputPath, outputPath)) {
        std::cerr << inputPath.string()
                  << ": source and destination paths must identify different files.\n";
        return 1;
    }

    const std::string inputName = inputPath.string();
    std::string error;
    const std::string outputName = outputPath.string();
    xrphoton::ogfx::SerializeResult serialized;
    {
        xrphoton::ogfx::DecodeResult decoded;
        {
            std::vector<std::uint8_t> sourceBytes;
            if (!readSourceFile(inputPath, inputName, &sourceBytes, &error)) {
                std::cerr << error << '\n';
                return 1;
            }
            decoded = xrphoton::legacy_ogf::decodeModel(sourceBytes, inputName);
        }
        if (!decoded) {
            std::cerr << decoded.error << '\n';
            return 1;
        }
        serialized = xrphoton::ogfx::serializeModel(decoded.model, outputName);
    }
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return 1;
    }
    if (!publishOutput(outputPath, outputName, serialized.bytes, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}

int convertBlenderMesh(char** arguments)
{
    const std::string sourceName = arguments[2];
    const std::filesystem::path outputPath = arguments[3];
    const std::string outputName = outputPath.string();
    std::string error;

    xrphoton::ogfx::SerializeResult serialized;
    {
        xrphoton::ogfx::DecodeResult decoded;
        {
            std::vector<std::uint8_t> sourceBytes;
            if (!readSourceStream(
                    std::cin, sourceName, &sourceBytes, &error)) {
                std::cerr << error << '\n';
                return 1;
            }
            decoded = xrphoton::blender_mesh::decodeStaticMesh(
                sourceBytes, sourceName);
        }
        if (!decoded) {
            std::cerr << decoded.error << '\n';
            return 1;
        }
        serialized = xrphoton::ogfx::serializeModel(decoded.model, outputName);
    }
    if (!serialized) {
        std::cerr << serialized.error << '\n';
        return 1;
    }
    if (!publishOutput(outputPath, outputName, serialized.bytes, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return 0;
}

int run(int argumentCount, char** arguments)
{
    if (argumentCount != 4) {
        printUsage();
        return 1;
    }
    const std::filesystem::path compilerPath = arguments[0];
    const std::filesystem::path outputPath = arguments[3];
    if (pathAliasesRunningCompiler(compilerPath, outputPath)) {
        std::cerr << outputPath.string()
                  << ": destination path must not identify the asset compiler executable.\n";
        return 1;
    }
    const std::string_view command = arguments[1];
    if (command == "convert-ogf") {
        return convertLegacyOgf(arguments);
    }
    if (command == "convert-blender") {
        return convertBlenderMesh(arguments);
    }
    printUsage();
    return 1;
}
}

int main(int argumentCount, char** arguments)
{
    try {
        return run(argumentCount, arguments);
    } catch (const std::bad_alloc&) {
        std::cerr << "xrPhotonAssetCompiler: memory allocation failed.\n";
    } catch (const std::length_error&) {
        std::cerr << "xrPhotonAssetCompiler: a requested container size is unsupported.\n";
    } catch (const std::exception& exception) {
        std::cerr << "xrPhotonAssetCompiler: host error: " << exception.what() << '\n';
    } catch (...) {
        std::cerr << "xrPhotonAssetCompiler: unknown host error.\n";
    }
    return 1;
}
