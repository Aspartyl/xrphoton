#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace xrphoton
{
enum class CommandLineMode
{
    Interactive,
    Capture,
};

struct CommandLineOptions
{
    CommandLineMode mode = CommandLineMode::Interactive;
    std::uint32_t captureFrameCount = 0;
    std::string captureOutputPath;
};

// Accept either no arguments (interactive mode) or exactly:
//   --capture <positive-successful-frame-count> <output.ppm>
// Parsing is deliberately independent of GLFW/Vulkan so malformed capture requests
// fail before any window or GPU state is created.
[[nodiscard]] bool parseCommandLine(
    int argumentCount,
    const char* const* arguments,
    CommandLineOptions* options,
    std::string* error);

// Hash the tightly packed, linear RGBA8 tonemapped output with 64-bit FNV-1a.
// Width and height are fed first as four little-endian bytes each, so equal byte
// strings at different extents remain distinct capture results.
[[nodiscard]] bool hashCaptureImage(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> linearRgba8,
    std::uint64_t* hash);

// Publish a binary P6 PPM. RGB is converted from linear UNORM to sRGB for visual
// parity with presentation; alpha is intentionally omitted. All size and stream
// failures are reported through the return value and optional diagnostic.
[[nodiscard]] bool writeCapturePpm(
    const std::string& outputPath,
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> linearRgba8,
    std::string* error);
}
