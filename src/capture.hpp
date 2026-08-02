#pragma once

#include "camera.hpp"
#include "estimator_mode.hpp"
#include "furnace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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
    std::uint32_t samplesPerPixel = 1;
    EstimatorMode estimator = EstimatorMode::Mis;
    std::optional<float> timeOfDayHours;
    bool furnaceRequested = false;
    bool validationRequested = false;
    bool gbufferProbeRequested = false;
};

struct CaptureTraceTimingSummary
{
    double medianMilliseconds = 0.0;
    std::uint32_t sampleCount = 0;
    bool comparable = false;
};

constexpr std::size_t ReferenceRegionCount = 3;
template<std::size_t RegionCount>
struct RegionAccumulator
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t sampleCount = 0;
    std::array<std::array<double, 3>, RegionCount> sums{};
    std::array<std::array<double, 3>, RegionCount> squaredSums{};
};
using ReferenceAccumulator = RegionAccumulator<ReferenceRegionCount>;
using FurnaceAccumulator = RegionAccumulator<FurnaceCaseCount>;

struct ReferenceRegionSummary
{
    std::array<double, 3> mean{};
    std::array<double, 3> standardError{};
};

// Accept interactive/capture or reference mode for the single test yard:
//   [--validation] [--spp <1|2|4|8|16>]
//       [--capture <positive-frame-count> <output.ppm>] [--time <hours>]
//   [--validation] --reference <positive-sample-count>
//       --estimator <mis|nee|bsdf> [--spp <1|2|4|8|16>] [--time <hours>]
//   [--validation] --reference <positive-sample-count>
//       --estimator bsdf --furnace
// Parsing is deliberately independent of GLFW/Vulkan so malformed capture requests
// fail before any window or GPU state is created.
[[nodiscard]] bool parseCommandLine(
    int argumentCount,
    const char* const* arguments,
    CommandLineOptions* options,
    std::string* error);

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

// The furnace uses the same frame-level statistics over nine independent fixed
// regions. BSDF-only reference mode is intentional: it excludes P3's approximate
// straight shadow transmission from the whole-transport energy proof.
[[nodiscard]] bool accumulateFurnaceImage(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint16_t> rgba16,
    FurnaceAccumulator* accumulator);

[[nodiscard]] bool summarizeFurnaceCases(
    const FurnaceAccumulator& accumulator,
    std::array<ReferenceRegionSummary, FurnaceCaseCount>* summaries);

// Each RGB channel must agree with the constant environment within either two
// percent or three measured standard errors, whichever is wider.
[[nodiscard]] bool furnaceCasePasses(const ReferenceRegionSummary& summary);

// P2c's pairwise estimator gate: agreement within one percent of the larger mean or
// three combined standard errors, independently for every region and RGB channel.
[[nodiscard]] bool referenceEstimatesAgree(
    const ReferenceRegionSummary& first,
    const ReferenceRegionSummary& second);

// The raygen shader's primary-miss sentinel in the instance-ID G-buffer; mirrored
// by GBufferMissInstanceId in raytrace.slang.
constexpr std::uint32_t GBufferMissInstanceId = 0xffffffffu;

// The wall probe's sight line crosses the dynamic crate's spawn position; the
// crate settles well below it within roughly two simulated seconds, so probed
// captures must run long enough for the final frame to be past that.
constexpr std::uint32_t MinimumGBufferProbeFrameCount = 128;

// One decoded G-buffer pixel: world shading normal + linear view depth from the
// binary16 image, linear albedo from the UNORM8 image, and the raw instance ID.
struct GBufferProbeSample
{
    std::array<float, 3> normal{};
    float viewDepth = 0.0f;
    std::array<float, 3> albedo{};
    std::uint32_t instanceId = 0;
};

// Decode pixel (x, y) from tightly packed G-buffer readbacks. Rejects null output,
// out-of-range coordinates, and spans inconsistent with width x height.
[[nodiscard]] bool extractGBufferProbeSample(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t x,
    std::uint32_t y,
    std::span<const std::uint16_t> normalDepthRgba16,
    std::span<const std::uint8_t> albedoRgba8,
    std::span<const std::uint32_t> instanceIds,
    GBufferProbeSample* sample);

// Analytic expected linear view depth for the primary ray through pixel (x, y)
// hitting the axis-aligned plane world[planeAxis] == planeCoordinate (axis 0/1/2
// for x/y/z). Mirrors the raygen construction exactly: jittered pixel center to
// NDC, the dispatch-space y flip, and the pre-scaled camera basis. Fails when the
// ray cannot reach the plane in front of the camera.
[[nodiscard]] bool expectedPlaneViewDepth(
    const CameraPushConstants& camera,
    float jitterX,
    float jitterY,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t planeAxis,
    float planeCoordinate,
    float* viewDepth);

// Invert the raygen ray construction: the integer pixel whose jittered primary
// ray passes closest to worldPoint. Fails for points behind the camera or
// projecting outside the image. Probe sites use this so the pinned target is a
// world position, not an extent-dependent pixel guess.
[[nodiscard]] bool projectWorldPointToPixel(
    const CameraPushConstants& camera,
    float jitterX,
    float jitterY,
    std::uint32_t width,
    std::uint32_t height,
    const std::array<float, 3>& worldPoint,
    std::uint32_t* x,
    std::uint32_t* y);

// D0 acceptance predicates. A surface probe pins the expected world-space normal,
// the analytic plane depth within a relative tolerance covering binary16 storage,
// the expected quantized albedo, and the exact instance index; the sky probe
// requires the exact miss sentinel.
[[nodiscard]] bool gbufferSurfaceProbePasses(
    const GBufferProbeSample& sample,
    const std::array<float, 3>& expectedNormal,
    float expectedViewDepth,
    const std::array<float, 3>& expectedAlbedo,
    std::uint32_t expectedInstanceId);

[[nodiscard]] bool gbufferSkyProbePasses(const GBufferProbeSample& sample);

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
