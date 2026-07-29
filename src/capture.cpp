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

    constexpr const char* Usage =
        "Usage: xrPhoton [--capture <positive-frame-count> <output.ppm>]";

    if (argumentCount != 4
        || arguments[1] == nullptr
        || arguments[2] == nullptr
        || arguments[3] == nullptr
        || std::string_view(arguments[1]) != "--capture") {
        *error = Usage;
        return false;
    }

    const std::string_view countText(arguments[2]);
    std::uint32_t frameCount = 0;
    const std::from_chars_result parseResult = std::from_chars(
        countText.data(),
        countText.data() + countText.size(),
        frameCount,
        10);

    if (countText.empty()
        || parseResult.ec != std::errc{}
        || parseResult.ptr != countText.data() + countText.size()
        || frameCount == 0) {
        *error = "Capture frame count must be a positive 32-bit integer.";
        return false;
    }

    if (arguments[3][0] == '\0') {
        *error = "Capture output path must not be empty.";
        return false;
    }

    try {
        options->mode = CommandLineMode::Capture;
        options->captureFrameCount = frameCount;
        options->captureOutputPath = arguments[3];
    } catch (const std::bad_alloc&) {
        *options = {};
        *error = "Not enough host memory to store capture arguments.";
        return false;
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
