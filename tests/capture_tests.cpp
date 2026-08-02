#include "capture.hpp"

#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

xrphoton::CommandLineOptions parse(
    std::initializer_list<const char*> arguments,
    bool* succeeded,
    std::string* error)
{
    const std::vector<const char*> argumentVector(arguments);
    xrphoton::CommandLineOptions options;
    *succeeded = xrphoton::parseCommandLine(
        static_cast<int>(argumentVector.size()),
        argumentVector.data(),
        &options,
        error);
    return options;
}

void testCommandLine()
{
    bool succeeded = false;
    std::string error;

    xrphoton::CommandLineOptions options = parse(
        {"xrPhoton"},
        &succeeded,
        &error);
    expect(
        succeeded
            && options.mode == xrphoton::CommandLineMode::Interactive
            && options.captureFrameCount == 0
            && options.captureOutputPath.empty()
            && options.referenceSampleCount == 0
            && options.scenePreset == xrphoton::ScenePreset::Yard
            && options.estimator == xrphoton::EstimatorMode::Mis
            && !options.validationRequested
            && error.empty(),
        "no arguments select interactive mode");

    options = parse(
        {"xrPhoton", "--capture", "8", "result.ppm"},
        &succeeded,
        &error);
    expect(
        succeeded
            && options.mode == xrphoton::CommandLineMode::Capture
            && options.captureFrameCount == 8
            && options.captureOutputPath == "result.ppm"
            && error.empty(),
        "a positive count and output path select capture mode");

    options = parse(
        {"xrPhoton", "--scene", "night", "--capture", "4", "night.ppm"},
        &succeeded,
        &error);
    expect(
        succeeded && options.mode == xrphoton::CommandLineMode::Capture
            && options.captureFrameCount == 4
            && options.scenePreset == xrphoton::ScenePreset::Night,
        "capture accepts the shared night scene selector in either option order");

    options = parse(
        {"xrPhoton", "--scene", "night"},
        &succeeded,
        &error);
    expect(
        succeeded && options.mode == xrphoton::CommandLineMode::Interactive
            && options.scenePreset == xrphoton::ScenePreset::Night,
        "interactive mode accepts the night scene selector");

    options = parse(
        {"xrPhoton", "--validation", "--scene", "night"},
        &succeeded,
        &error);
    expect(
        succeeded && options.mode == xrphoton::CommandLineMode::Interactive
            && options.scenePreset == xrphoton::ScenePreset::Night
            && options.validationRequested,
        "the canonical engine accepts runtime validation in interactive mode");

    options = parse(
        {"xrPhoton", "--reference", "64", "--scene", "night",
         "--estimator", "nee"},
        &succeeded,
        &error);
    expect(
        succeeded && options.mode == xrphoton::CommandLineMode::Reference
            && options.referenceSampleCount == 64
            && options.scenePreset == xrphoton::ScenePreset::Night
            && options.estimator == xrphoton::EstimatorMode::Nee,
        "reference mode requires and retains its scene and estimator controls");

    options = parse(
        {"xrPhoton", "--capture", "4294967295", "result.ppm"},
        &succeeded,
        &error);
    expect(
        succeeded
            && options.captureFrameCount == UINT32_MAX,
        "the complete uint32 count domain is accepted");

    const std::initializer_list<const char*> invalidArguments[] = {
        {"xrPhoton", "--capture"},
        {"xrPhoton", "--capture", "8"},
        {"xrPhoton", "--capture", "8", "result.ppm", "extra"},
        {"xrPhoton", "--unknown", "8", "result.ppm"},
        {"xrPhoton", "--capture", "0", "result.ppm"},
        {"xrPhoton", "--capture", "-1", "result.ppm"},
        {"xrPhoton", "--capture", "+1", "result.ppm"},
        {"xrPhoton", "--capture", "eight", "result.ppm"},
        {"xrPhoton", "--capture", "8x", "result.ppm"},
        {"xrPhoton", "--capture", "4294967296", "result.ppm"},
        {"xrPhoton", "--capture", "8", ""},
        {"xrPhoton", "--scene", "furnace"},
        {"xrPhoton", "--scene", "glass"},
        {"xrPhoton", "--validation", "--validation"},
        {"xrPhoton", "--reference", "8", "--scene", "night"},
        {"xrPhoton", "--reference", "8", "--estimator", "mis"},
        {"xrPhoton", "--reference", "0", "--scene", "night", "--estimator", "mis"},
        {"xrPhoton", "--reference", "8", "--scene", "night", "--estimator", "both"},
        {"xrPhoton", "--estimator", "mis"},
    };

    for (const auto& arguments : invalidArguments) {
        options = parse(arguments, &succeeded, &error);
        expect(
            !succeeded
                && options.mode == xrphoton::CommandLineMode::Interactive
                && !error.empty(),
            "malformed capture arguments are rejected with a diagnostic");
    }

    expect(
        !xrphoton::parseCommandLine(0, nullptr, &options, &error),
        "an invalid process argument list is rejected");
    expect(
        !xrphoton::parseCommandLine(1, nullptr, &options, &error),
        "a null argument vector is rejected");
    const char* validArguments[] = {"xrPhoton"};
    expect(
        !xrphoton::parseCommandLine(1, validArguments, nullptr, &error)
            && !xrphoton::parseCommandLine(1, validArguments, &options, nullptr),
        "null parse outputs are rejected");
}

void testHalfAndReferenceAccumulation()
{
    expect(
        xrphoton::binary16ToFloat(0x0000u) == 0.0f
            && std::signbit(xrphoton::binary16ToFloat(0x8000u))
            && xrphoton::binary16ToFloat(0x3c00u) == 1.0f
            && xrphoton::binary16ToFloat(0xc000u) == -2.0f
            && xrphoton::binary16ToFloat(0x7bffu) == 65504.0f
            && xrphoton::binary16ToFloat(0x0001u)
                == std::ldexp(1.0f, -24)
            && std::isinf(xrphoton::binary16ToFloat(0x7c00u))
            && std::isnan(xrphoton::binary16ToFloat(0x7e00u)),
        "binary16 conversion covers zero, normal, subnormal, and special values");

    std::vector<std::uint16_t> first(8 * 8 * 4);
    std::vector<std::uint16_t> second(8 * 8 * 4);
    for (std::size_t pixel = 0; pixel < 8 * 8; ++pixel) {
        first[pixel * 4 + 0] = 0x3c00u;
        first[pixel * 4 + 1] = 0x4000u;
        first[pixel * 4 + 2] = 0x4200u;
        first[pixel * 4 + 3] = 0x3c00u;
        second[pixel * 4 + 0] = 0x4000u;
        second[pixel * 4 + 1] = 0x4400u;
        second[pixel * 4 + 2] = 0x4600u;
        second[pixel * 4 + 3] = 0x3c00u;
    }
    xrphoton::ReferenceAccumulator accumulator;
    expect(
        xrphoton::accumulateReferenceImage(8, 8, first, &accumulator)
            && xrphoton::accumulateReferenceImage(8, 8, second, &accumulator)
            && accumulator.sampleCount == 2,
        "two uniform HDR frames accumulate into every pinned region");
    std::array<xrphoton::ReferenceRegionSummary,
        xrphoton::ReferenceRegionCount> summaries{};
    expect(
        xrphoton::summarizeReferenceRegions(accumulator, &summaries),
        "reference region statistics summarize");
    for (const auto& summary : summaries) {
        expect(
            summary.mean[0] == 1.5 && summary.mean[1] == 3.0
                && summary.mean[2] == 4.5
                && std::abs(summary.standardError[0] - 0.5) < 1.0e-12
                && std::abs(summary.standardError[1] - 1.0) < 1.0e-12
                && std::abs(summary.standardError[2] - 1.5) < 1.0e-12,
            "reference means and standard errors use frame-level double sums");
    }
    xrphoton::ReferenceRegionSummary baseline = summaries[0];
    baseline.standardError = {};
    xrphoton::ReferenceRegionSummary close = baseline;
    close.mean[0] *= 1.009;
    xrphoton::ReferenceRegionSummary far = baseline;
    far.mean[0] *= 1.02;
    far.standardError = {};
    xrphoton::ReferenceRegionSummary noisy = far;
    noisy.standardError[0] = 0.02;
    expect(
        xrphoton::referenceEstimatesAgree(baseline, close)
            && !xrphoton::referenceEstimatesAgree(baseline, far)
            && xrphoton::referenceEstimatesAgree(baseline, noisy),
        "reference agreement uses the larger of one-percent and three-sigma bounds");

    const xrphoton::ReferenceAccumulator unchanged = accumulator;
    first[(3 * 8 + 3) * 4] = 0x7e00u;
    expect(
        !xrphoton::accumulateReferenceImage(8, 8, first, &accumulator)
            && accumulator.sampleCount == unchanged.sampleCount
            && !xrphoton::accumulateReferenceImage(7, 8, second, &accumulator)
            && !xrphoton::summarizeReferenceRegions(accumulator, nullptr),
        "invalid HDR data and extent changes are rejected transactionally");
}

void testHash()
{
    struct HashCase
    {
        std::uint32_t width;
        std::uint32_t height;
        std::vector<std::uint8_t> rgba;
        std::uint64_t expected;
    };
    const HashCase cases[] = {
        {1, 1, {0, 0, 0, 255}, 0x7e1f2542cd31c948ull},
        {1, 1, {255, 0, 128, 255}, 0x9cb8381cfbea0663ull},
        {2, 1, {0, 1, 2, 3, 4, 5, 6, 7}, 0x22342ea26fe4857eull},
        {1, 2, {0, 1, 2, 3, 4, 5, 6, 7}, 0x1504782b470fe6ceull},
    };

    for (const HashCase& testCase : cases) {
        std::uint64_t hash = 0;
        expect(
            xrphoton::hashCaptureImage(
                testCase.width,
                testCase.height,
                testCase.rgba,
                &hash)
                && hash == testCase.expected,
            "capture hash matches its known-answer vector");
    }

    std::uint64_t hash = 0;
    const std::vector<std::uint8_t> onePixel{0, 0, 0, 255};
    expect(
        !xrphoton::hashCaptureImage(0, 1, onePixel, &hash)
            && !xrphoton::hashCaptureImage(1, 0, onePixel, &hash)
            && !xrphoton::hashCaptureImage(1, 1, {}, &hash)
            && !xrphoton::hashCaptureImage(1, 1, onePixel, nullptr),
        "capture hashing rejects invalid extents, sizes, and outputs");
}

void testTraceTimingSummary()
{
    xrphoton::CaptureTraceTimingSummary summary{
        .medianMilliseconds = 17.0,
        .sampleCount = 19,
        .comparable = true,
    };
    const std::vector<double> diagnostic{4.0, 1.0, 3.0, 2.0};
    expect(
        xrphoton::summarizeCaptureTraceTimings(diagnostic, &summary)
            && summary.medianMilliseconds == 2.5
            && summary.sampleCount == diagnostic.size()
            && !summary.comparable,
        "short capture timings produce a diagnostic median");

    std::vector<double> benchmark(xrphoton::CaptureTraceTimingCapacity, 1000.0);
    for (std::uint32_t index = 0;
         index < xrphoton::CaptureBenchmarkMeasuredFrameCount;
         ++index) {
        benchmark[xrphoton::CaptureBenchmarkWarmupFrameCount + index] = index;
    }
    expect(
        xrphoton::summarizeCaptureTraceTimings(benchmark, &summary)
            && summary.medianMilliseconds == 127.5
            && summary.sampleCount
                == xrphoton::CaptureBenchmarkMeasuredFrameCount
            && summary.comparable,
        "fixed capture timings discard warm-up frames and report 256-sample median");

    const xrphoton::CaptureTraceTimingSummary unchanged = summary;
    const std::vector<double> negative{1.0, -1.0};
    const std::vector<double> nonFinite{
        std::numeric_limits<double>::quiet_NaN(),
    };
    expect(
        !xrphoton::summarizeCaptureTraceTimings({}, &summary)
            && !xrphoton::summarizeCaptureTraceTimings(negative, &summary)
            && !xrphoton::summarizeCaptureTraceTimings(nonFinite, &summary)
            && !xrphoton::summarizeCaptureTraceTimings(diagnostic, nullptr)
            && summary.medianMilliseconds == unchanged.medianMilliseconds
            && summary.sampleCount == unchanged.sampleCount
            && summary.comparable == unchanged.comparable,
        "trace timing summary rejects invalid input without changing its output");
}

void testPpm(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& unwritablePath)
{
    std::error_code removeError;
    std::filesystem::remove(outputPath, removeError);

    // Linear 128 encodes to sRGB 188 with the pinned transfer curve and rounding.
    const std::vector<std::uint8_t> rgba{
        0, 128, 255, 17,
        255, 0, 128, 239,
    };
    std::string error;
    expect(
        xrphoton::writeCapturePpm(
            outputPath.string(),
            2,
            1,
            rgba,
            &error)
            && error.empty(),
        "a valid linear RGBA image is published as a PPM");

    std::ifstream input(outputPath, std::ios::binary);
    const std::vector<std::uint8_t> actual{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
    const std::string header = "P6\n2 1\n255\n";
    std::vector<std::uint8_t> expected(header.begin(), header.end());
    expected.insert(expected.end(), {0, 188, 255, 255, 0, 188});
    expect(
        actual == expected,
        "PPM bytes pin the header, sRGB transfer, row order, and omitted alpha");

    expect(
        !xrphoton::writeCapturePpm(
            unwritablePath.string(),
            2,
            1,
            rgba,
            &error)
            && !error.empty(),
        "an existing directory exercises the unwritable-output failure");
    expect(
        !xrphoton::writeCapturePpm("", 2, 1, rgba, &error)
            && !xrphoton::writeCapturePpm(
                outputPath.string(),
                1,
                1,
                rgba,
                &error),
        "PPM publication rejects empty paths and mismatched pixel sizes");

    std::filesystem::remove(outputPath, removeError);
}
}

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 3) {
        std::cerr << "capture_tests requires an output file and an existing directory.\n";
        return 1;
    }

    testCommandLine();
    testHalfAndReferenceAccumulation();
    testHash();
    testTraceTimingSummary();
    testPpm(arguments[1], arguments[2]);

    if (failures != 0) {
        std::cerr << failures << " capture test(s) failed.\n";
        return 1;
    }

    std::cout << "Capture tests passed.\n";
    return 0;
}
