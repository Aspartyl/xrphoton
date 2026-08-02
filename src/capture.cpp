#include "capture.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <limits>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace xrphoton
{
namespace
{
constexpr std::uint64_t Fnv1aOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t Fnv1aPrime = 1099511628211ull;

bool checkedRgba8Size(
    std::uint32_t width,
    std::uint32_t height,
    std::size_t* byteCount)
{
    if (byteCount == nullptr || width == 0 || height == 0) {
        return false;
    }

    constexpr std::size_t ChannelCount = 4;
    const std::size_t sizeWidth = width;
    const std::size_t sizeHeight = height;

    if (sizeWidth > std::numeric_limits<std::size_t>::max() / sizeHeight) {
        return false;
    }
    const std::size_t pixelCount = sizeWidth * sizeHeight;

    if (pixelCount > std::numeric_limits<std::size_t>::max() / ChannelCount) {
        return false;
    }

    *byteCount = pixelCount * ChannelCount;
    return true;
}

void hashByte(std::uint8_t byte, std::uint64_t* hash)
{
    *hash ^= byte;
    *hash *= Fnv1aPrime;
}

std::uint8_t linearUnormToSrgb(std::uint8_t linearByte)
{
    const double linear = static_cast<double>(linearByte) / 255.0;
    const double srgb = linear <= 0.0031308
        ? 12.92 * linear
        : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
    const double encoded = std::clamp(srgb, 0.0, 1.0) * 255.0;
    return static_cast<std::uint8_t>(std::lround(encoded));
}

void setError(std::string* error, const char* message)
{
    if (error != nullptr) {
        *error = message;
    }
}

bool parsePositiveU32(std::string_view text, std::uint32_t* value)
{
    if (value == nullptr || text.empty()) {
        return false;
    }
    std::uint32_t parsed = 0;
    const std::from_chars_result result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        parsed,
        10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
        || parsed == 0) {
        return false;
    }
    *value = parsed;
    return true;
}
}

bool parseCommandLine(
    int argumentCount,
    const char* const* arguments,
    CommandLineOptions* options,
    std::string* error)
{
    if (options == nullptr || error == nullptr) {
        return false;
    }

    *options = {};
    error->clear();

    if (argumentCount < 1 || arguments == nullptr || arguments[0] == nullptr) {
        *error = "Invalid process argument list.";
        return false;
    }

    if (argumentCount == 1) {
        return true;
    }

    try {
        constexpr const char* Usage =
            "Usage: xrPhoton [--validation] [--capture <count> <output.ppm>] "
            "[--scene <yard|night>] | [--validation] --reference <count> "
            "--scene <yard|night> --estimator <mis|nee|bsdf>";
        bool modeSeen = false;
        bool sceneSeen = false;
        bool estimatorSeen = false;
        bool validationSeen = false;
        CommandLineOptions candidate;
        for (int index = 1; index < argumentCount;) {
            if (arguments[index] == nullptr) {
                *error = Usage;
                return false;
            }
            const std::string_view option(arguments[index]);
            if (option == "--validation") {
                if (validationSeen) {
                    *error = Usage;
                    return false;
                }
                candidate.validationRequested = true;
                validationSeen = true;
                ++index;
            } else if (option == "--capture") {
                if (modeSeen || index + 2 >= argumentCount
                    || arguments[index + 1] == nullptr
                    || arguments[index + 2] == nullptr) {
                    *error = Usage;
                    return false;
                }
                std::uint32_t count = 0;
                if (!parsePositiveU32(arguments[index + 1], &count)) {
                    *error = "Capture frame count must be a positive 32-bit integer.";
                    return false;
                }
                if (arguments[index + 2][0] == '\0') {
                    *error = "Capture output path must not be empty.";
                    return false;
                }
                candidate.mode = CommandLineMode::Capture;
                candidate.captureFrameCount = count;
                candidate.captureOutputPath = arguments[index + 2];
                modeSeen = true;
                index += 3;
            } else if (option == "--reference") {
                if (modeSeen || index + 1 >= argumentCount
                    || arguments[index + 1] == nullptr) {
                    *error = Usage;
                    return false;
                }
                std::uint32_t count = 0;
                if (!parsePositiveU32(arguments[index + 1], &count)) {
                    *error = "Reference sample count must be a positive 32-bit integer.";
                    return false;
                }
                candidate.mode = CommandLineMode::Reference;
                candidate.referenceSampleCount = count;
                modeSeen = true;
                index += 2;
            } else if (option == "--scene") {
                if (sceneSeen || index + 1 >= argumentCount
                    || arguments[index + 1] == nullptr) {
                    *error = Usage;
                    return false;
                }
                const std::string_view value(arguments[index + 1]);
                if (value == "yard") {
                    candidate.scenePreset = ScenePreset::Yard;
                } else if (value == "night") {
                    candidate.scenePreset = ScenePreset::Night;
                } else {
                    *error = "Scene must be 'yard' or 'night'.";
                    return false;
                }
                sceneSeen = true;
                index += 2;
            } else if (option == "--estimator") {
                if (estimatorSeen || index + 1 >= argumentCount
                    || arguments[index + 1] == nullptr) {
                    *error = Usage;
                    return false;
                }
                const std::string_view value(arguments[index + 1]);
                if (value == "mis") {
                    candidate.estimator = EstimatorMode::Mis;
                } else if (value == "nee") {
                    candidate.estimator = EstimatorMode::Nee;
                } else if (value == "bsdf") {
                    candidate.estimator = EstimatorMode::Bsdf;
                } else {
                    *error = "Estimator must be 'mis', 'nee', or 'bsdf'.";
                    return false;
                }
                estimatorSeen = true;
                index += 2;
            } else {
                *error = Usage;
                return false;
            }
        }
        if (candidate.mode == CommandLineMode::Reference
            && (!sceneSeen || !estimatorSeen)) {
            *error = "Reference mode requires --scene and --estimator.";
            return false;
        }
        if (candidate.mode != CommandLineMode::Reference && estimatorSeen) {
            *error = "--estimator is only valid with --reference.";
            return false;
        }
        *options = std::move(candidate);
    } catch (const std::bad_alloc&) {
        *options = {};
        *error = "Not enough host memory to store capture arguments.";
        return false;
    }

    return true;
}

float binary16ToFloat(std::uint16_t bits)
{
    const bool negative = (bits & 0x8000u) != 0;
    const std::uint32_t exponent = (bits >> 10u) & 0x1fu;
    const std::uint32_t fraction = bits & 0x03ffu;
    double magnitude = 0.0;
    if (exponent == 0) {
        magnitude = std::ldexp(static_cast<double>(fraction), -24);
    } else if (exponent == 31) {
        magnitude = fraction == 0
            ? std::numeric_limits<double>::infinity()
            : std::numeric_limits<double>::quiet_NaN();
    } else {
        magnitude = std::ldexp(
            1.0 + static_cast<double>(fraction) / 1024.0,
            static_cast<int>(exponent) - 15);
    }
    return static_cast<float>(negative ? -magnitude : magnitude);
}

bool accumulateReferenceImage(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint16_t> rgba16,
    ReferenceAccumulator* accumulator)
{
    if (accumulator == nullptr || width == 0 || height == 0) {
        return false;
    }
    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(width) * height;
    if (pixelCount > std::numeric_limits<std::size_t>::max() / 4
        || rgba16.size() != static_cast<std::size_t>(pixelCount * 4)
        || accumulator->sampleCount == std::numeric_limits<std::uint32_t>::max()
        || (accumulator->sampleCount != 0
            && (accumulator->width != width
                || accumulator->height != height))) {
        return false;
    }

    struct Region
    {
        std::uint32_t x0Numerator;
        std::uint32_t x1Numerator;
        std::uint32_t y0Numerator;
        std::uint32_t y1Numerator;
    };
    constexpr std::uint32_t Denominator = 8;
    constexpr Region Regions[ReferenceRegionCount] = {
        {3, 5, 3, 5},
        {4, 6, 2, 4},
        {2, 6, 4, 6},
    };

    auto updated = *accumulator;
    if (updated.sampleCount == 0) {
        updated.width = width;
        updated.height = height;
    }
    for (std::size_t regionIndex = 0;
         regionIndex < ReferenceRegionCount;
         ++regionIndex) {
        const Region& region = Regions[regionIndex];
        const std::uint32_t x0 = width * region.x0Numerator / Denominator;
        const std::uint32_t x1 = width * region.x1Numerator / Denominator;
        const std::uint32_t y0 = height * region.y0Numerator / Denominator;
        const std::uint32_t y1 = height * region.y1Numerator / Denominator;
        if (x0 >= x1 || y0 >= y1) {
            return false;
        }
        const double regionPixelCount =
            static_cast<double>(x1 - x0) * (y1 - y0);
        std::array<double, 3> frameMean{};
        for (std::uint32_t y = y0; y < y1; ++y) {
            for (std::uint32_t x = x0; x < x1; ++x) {
                const std::size_t pixel =
                    (static_cast<std::size_t>(y) * width + x) * 4;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float value = binary16ToFloat(rgba16[pixel + channel]);
                    if (!std::isfinite(value) || value < 0.0f) {
                        return false;
                    }
                    frameMean[channel] += value;
                }
            }
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            frameMean[channel] /= regionPixelCount;
            updated.sums[regionIndex][channel] += frameMean[channel];
            updated.squaredSums[regionIndex][channel] +=
                frameMean[channel] * frameMean[channel];
        }
    }
    ++updated.sampleCount;
    *accumulator = updated;
    return true;
}

bool summarizeReferenceRegions(
    const ReferenceAccumulator& accumulator,
    std::array<ReferenceRegionSummary, ReferenceRegionCount>* summaries)
{
    if (summaries == nullptr || accumulator.sampleCount == 0) {
        return false;
    }
    std::array<ReferenceRegionSummary, ReferenceRegionCount> candidate{};
    const double count = accumulator.sampleCount;
    for (std::size_t region = 0; region < ReferenceRegionCount; ++region) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const double mean = accumulator.sums[region][channel] / count;
            double standardError = 0.0;
            if (accumulator.sampleCount > 1) {
                const double variance = std::max(
                    0.0,
                    (accumulator.squaredSums[region][channel]
                        - count * mean * mean)
                        / (count - 1.0));
                standardError = std::sqrt(variance / count);
            }
            if (!std::isfinite(mean) || !std::isfinite(standardError)) {
                return false;
            }
            candidate[region].mean[channel] = mean;
            candidate[region].standardError[channel] = standardError;
        }
    }
    *summaries = candidate;
    return true;
}

bool referenceEstimatesAgree(
    const ReferenceRegionSummary& first,
    const ReferenceRegionSummary& second)
{
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const double firstMean = first.mean[channel];
        const double secondMean = second.mean[channel];
        const double firstError = first.standardError[channel];
        const double secondError = second.standardError[channel];
        if (!std::isfinite(firstMean) || !std::isfinite(secondMean)
            || !std::isfinite(firstError) || !std::isfinite(secondError)
            || firstError < 0.0 || secondError < 0.0) {
            return false;
        }
        const double difference = std::abs(firstMean - secondMean);
        const double relativeTolerance =
            0.01 * std::max(std::abs(firstMean), std::abs(secondMean));
        const double statisticalTolerance = 3.0 * std::hypot(
            firstError,
            secondError);
        if (difference > std::max(relativeTolerance, statisticalTolerance)) {
            return false;
        }
    }
    return true;
}

bool hashCaptureImage(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> linearRgba8,
    std::uint64_t* hash)
{
    std::size_t byteCount = 0;
    if (hash == nullptr
        || !checkedRgba8Size(width, height, &byteCount)
        || linearRgba8.size() != byteCount) {
        return false;
    }

    std::uint64_t value = Fnv1aOffsetBasis;
    for (unsigned int byteIndex = 0; byteIndex < 4; ++byteIndex) {
        hashByte(
            static_cast<std::uint8_t>(width >> (byteIndex * 8)),
            &value);
    }
    for (unsigned int byteIndex = 0; byteIndex < 4; ++byteIndex) {
        hashByte(
            static_cast<std::uint8_t>(height >> (byteIndex * 8)),
            &value);
    }
    for (const std::uint8_t byte : linearRgba8) {
        hashByte(byte, &value);
    }

    *hash = value;
    return true;
}

bool summarizeCaptureTraceTimings(
    std::span<const double> milliseconds,
    CaptureTraceTimingSummary* summary)
{
    if (summary == nullptr || milliseconds.empty()) {
        return false;
    }

    const bool comparable = milliseconds.size() >= CaptureTraceTimingCapacity;
    const std::span<const double> measured = comparable
        ? milliseconds.subspan(
              CaptureBenchmarkWarmupFrameCount,
              CaptureBenchmarkMeasuredFrameCount)
        : milliseconds;

    for (const double value : measured) {
        if (!std::isfinite(value) || value < 0.0) {
            return false;
        }
    }

    try {
        std::vector<double> ordered(measured.begin(), measured.end());
        std::sort(ordered.begin(), ordered.end());
        const std::size_t midpoint = ordered.size() / 2;
        const double median = ordered.size() % 2 == 0
            ? (ordered[midpoint - 1] + ordered[midpoint]) * 0.5
            : ordered[midpoint];

        *summary = {
            .medianMilliseconds = median,
            .sampleCount = static_cast<std::uint32_t>(ordered.size()),
            .comparable = comparable,
        };
    } catch (const std::bad_alloc&) {
        return false;
    }

    return true;
}

bool writeCapturePpm(
    const std::string& outputPath,
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint8_t> linearRgba8,
    std::string* error)
{
    if (error != nullptr) {
        error->clear();
    }

    std::size_t byteCount = 0;
    if (outputPath.empty()) {
        setError(error, "Capture output path must not be empty.");
        return false;
    }
    if (!checkedRgba8Size(width, height, &byteCount)
        || linearRgba8.size() != byteCount) {
        setError(error, "Capture pixels do not match the requested nonzero extent.");
        return false;
    }

    constexpr std::size_t PpmChannelCount = 3;
    const std::size_t sizeWidth = width;
    if (sizeWidth > std::numeric_limits<std::size_t>::max() / PpmChannelCount) {
        setError(error, "Capture PPM row size overflows host limits.");
        return false;
    }
    const std::size_t rowByteCount = sizeWidth * PpmChannelCount;
    if (rowByteCount > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max())) {
        setError(error, "Capture PPM row exceeds stream limits.");
        return false;
    }

    try {
        std::ofstream output(
            outputPath,
            std::ios::binary | std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            setError(error, "Could not open the capture output path for writing.");
            return false;
        }

        output << "P6\n" << width << ' ' << height << "\n255\n";
        if (!output) {
            setError(error, "Failed while writing the capture PPM header.");
            return false;
        }

        std::vector<std::uint8_t> srgbRow(rowByteCount);
        for (std::uint32_t y = 0; y < height; ++y) {
            const std::size_t sourceRow =
                static_cast<std::size_t>(y) * sizeWidth * 4;
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t sourcePixel =
                    sourceRow + static_cast<std::size_t>(x) * 4;
                const std::size_t destinationPixel =
                    static_cast<std::size_t>(x) * PpmChannelCount;
                srgbRow[destinationPixel] =
                    linearUnormToSrgb(linearRgba8[sourcePixel]);
                srgbRow[destinationPixel + 1] =
                    linearUnormToSrgb(linearRgba8[sourcePixel + 1]);
                srgbRow[destinationPixel + 2] =
                    linearUnormToSrgb(linearRgba8[sourcePixel + 2]);
            }

            output.write(
                reinterpret_cast<const char*>(srgbRow.data()),
                static_cast<std::streamsize>(srgbRow.size()));
            if (!output) {
                setError(error, "Failed while writing capture PPM pixels.");
                return false;
            }
        }

        output.close();
        if (output.fail()) {
            setError(error, "Failed to finish writing the capture PPM.");
            return false;
        }
    } catch (const std::bad_alloc&) {
        setError(error, "Not enough host memory to encode the capture PPM.");
        return false;
    } catch (...) {
        setError(error, "Unexpected failure while publishing the capture PPM.");
        return false;
    }

    return true;
}
}
