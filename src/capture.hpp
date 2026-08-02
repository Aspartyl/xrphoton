#pragma once

#include "scene_preset.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace xrphoton
{
constexpr std::uint32_t CaptureBenchmarkWarmupFrameCount = 32;
constexpr std::uint32_t CaptureBenchmarkMeasuredFrameCount = 256;
constexpr std::size_t CaptureTraceTimingCapacity =
    CaptureBenchmarkWarmupFrameCount + CaptureBenchmarkMeasuredFrameCount;

enum class CommandLineMode
{
    Interactive,
    Capture,
    Reference,
};

struct CommandLineOptions
{
    CommandLineMode mode = CommandLineMode::Interactive;
    std::uint32_t captureFrameCount = 0;
    std::string captureOutputPath;
    std::uint32_t referenceSampleCount = 0;
    ScenePreset scenePreset = ScenePreset::Yard;
    EstimatorMode estimator = EstimatorMode::Mis;
    bool validationRequested = false;
};

struct CaptureTraceTimingSummary
{
    double medianMilliseconds = 0.0;
    std::uint32_t sampleCount = 0;
    bool comparable = false;
};

constexpr std::size_t ReferenceRegionCount = 3;
struct ReferenceAccumulator
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t sampleCount = 0;
    std::array<std::array<double, 3>, ReferenceRegionCount> sums{};
    std::array<std::array<double, 3>, ReferenceRegionCount> squaredSums{};
};

struct ReferenceRegionSummary
{
    std::array<double, 3> mean{};
    std::array<double, 3> standardError{};
};

// Accept interactive/capture with an optional scene selector, or reference mode:
//   [--validation] [--capture <positive-frame-count> <output.ppm>]
//       [--scene <yard|night>]
//   [--validation] --reference <positive-sample-count> --scene <yard|night>
//       --estimator <mis|nee|bsdf>
// Parsing is deliberately independent of GLFW/Vulkan so malformed capture requests
// fail before any window or GPU state is created.
[[nodiscard]] bool parseCommandLine(
    int argumentCount,
    const char* const* arguments,
    CommandLineOptions* options,
    std::string* error);

[[nodiscard]] constexpr const char* scenePresetName(ScenePreset preset)
{
    switch (preset) {
    case ScenePreset::Night:
        return "night";
    default:
        return "yard";
    }
}

[[nodiscard]] constexpr const char* estimatorModeName(EstimatorMode estimator)
{
    switch (estimator) {
    case EstimatorMode::Nee:
        return "nee";
    case EstimatorMode::Bsdf:
        return "bsdf";
    default:
        return "mis";
    }
}

[[nodiscard]] constexpr const char* referenceRegionName(std::size_t index)
{
    constexpr const char* Names[ReferenceRegionCount] = {
        "center",
        "interior",
        "ground",
    };
    return index < ReferenceRegionCount ? Names[index] : "invalid";
}

[[nodiscard]] float binary16ToFloat(std::uint16_t bits);

// Convert one raw R16G16B16A16_SFLOAT frame and accumulate each pinned region's RGB
// frame mean in double precision. The fixed regions make separate estimator runs
// directly comparable without retaining full-resolution sample histories.
[[nodiscard]] bool accumulateReferenceImage(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint16_t> rgba16,
    ReferenceAccumulator* accumulator);

[[nodiscard]] bool summarizeReferenceRegions(
    const ReferenceAccumulator& accumulator,
    std::array<ReferenceRegionSummary, ReferenceRegionCount>* summaries);

// P2c's pairwise estimator gate: agreement within one percent of the larger mean or
// three combined standard errors, independently for every region and RGB channel.
[[nodiscard]] bool referenceEstimatesAgree(
    const ReferenceRegionSummary& first,
    const ReferenceRegionSummary& second);

// Hash the tightly packed, linear RGBA8 tonemapped output with 64-bit FNV-1a.
// Width and height are fed first as four little-endian bytes each, so equal byte
// strings at different extents remain distinct capture results.
[[nodiscard]] bool hashCaptureImage(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> linearRgba8,
    std::uint64_t* hash);

// Summarize trace-only GPU timings. The fixed comparable protocol discards 32 warm-up
// values and takes the median of the next 256; shorter captures report the median of
// every available value and are explicitly diagnostic. Non-finite/negative samples,
// an empty input, or an invalid output pointer are rejected.
[[nodiscard]] bool summarizeCaptureTraceTimings(
    std::span<const double> milliseconds,
    CaptureTraceTimingSummary* summary);

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
