#include "capture.hpp"

#include "lighting.hpp"

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

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
constexpr float DenoiseSurfaceNormalThreshold = 0.9f;
constexpr float DenoiseSurfaceDepthRelativeThreshold = 0.02f;
constexpr float DenoiseSurfaceDepthAbsoluteThreshold = 0.05f;
constexpr float DenoiseFireflyMedianMultiple = 8.0f;
constexpr float DenoiseFireflyMinimumAllowance = 0.05f;

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

bool parseTimeOfDayHours(std::string_view text, float* value)
{
    if (value == nullptr || text.empty()) {
        return false;
    }
    float parsed = 0.0f;
    const std::from_chars_result result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        parsed,
        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
        || !std::isfinite(parsed) || parsed < 0.0f || parsed >= 24.0f) {
        return false;
    }
    *value = parsed;
    return true;
}

template<std::size_t RegionCount>
bool accumulateRegions(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint16_t> rgba16,
    const std::array<FurnaceRegion, RegionCount>& regions,
    std::uint32_t denominator,
    RegionAccumulator<RegionCount>* accumulator)
{
    if (accumulator == nullptr || width == 0 || height == 0
        || denominator == 0) {
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

    auto updated = *accumulator;
    if (updated.sampleCount == 0) {
        updated.width = width;
        updated.height = height;
    }
    for (std::size_t regionIndex = 0;
         regionIndex < regions.size();
         ++regionIndex) {
        const FurnaceRegion& region = regions[regionIndex];
        const std::uint32_t x0 = width * region.x0Numerator / denominator;
        const std::uint32_t x1 = width * region.x1Numerator / denominator;
        const std::uint32_t y0 = height * region.y0Numerator / denominator;
        const std::uint32_t y1 = height * region.y1Numerator / denominator;
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

template<std::size_t RegionCount>
bool summarizeRegions(
    const RegionAccumulator<RegionCount>& accumulator,
    std::array<ReferenceRegionSummary, RegionCount>* summaries)
{
    if (summaries == nullptr || accumulator.sampleCount == 0) {
        return false;
    }
    std::array<ReferenceRegionSummary, RegionCount> candidate{};
    const double count = accumulator.sampleCount;
    for (std::size_t region = 0; region < RegionCount; ++region) {
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
            "Usage: xrPhoton [--validation] [--spp <1|2|4|8|16>] "
            "[--denoise <off|spatial|temporal>] "
            "[--capture <count> <output.ppm>] "
            "[--gbuffer-probe|--denoise-probe] [--time <hours>] | "
            "[--validation] --reference <count> --estimator <mis|nee|bsdf> "
            "[--spp <1|2|4|8|16>] [--time <hours>] | [--validation] "
            "--reference <count> --estimator bsdf --furnace";
        bool modeSeen = false;
        bool estimatorSeen = false;
        bool timeSeen = false;
        bool furnaceSeen = false;
        bool validationSeen = false;
        bool samplesPerPixelSeen = false;
        bool denoiseSeen = false;
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
            } else if (option == "--time") {
                if (timeSeen || index + 1 >= argumentCount
                    || arguments[index + 1] == nullptr) {
                    *error = Usage;
                    return false;
                }
                float hours = 0.0f;
                if (!parseTimeOfDayHours(arguments[index + 1], &hours)) {
                    *error = "Time of day must be a finite number in [0, 24).";
                    return false;
                }
                candidate.timeOfDayHours = hours;
                timeSeen = true;
                index += 2;
            } else if (option == "--spp") {
                if (samplesPerPixelSeen || index + 1 >= argumentCount
                    || arguments[index + 1] == nullptr) {
                    *error = Usage;
                    return false;
                }
                std::uint32_t samplesPerPixel = 0;
                if (!parsePositiveU32(arguments[index + 1], &samplesPerPixel)
                    || !isSupportedSamplesPerPixel(samplesPerPixel)) {
                    *error = "Samples per pixel must be 1, 2, 4, 8, or 16.";
                    return false;
                }
                candidate.samplesPerPixel = samplesPerPixel;
                samplesPerPixelSeen = true;
                index += 2;
            } else if (option == "--furnace") {
                if (furnaceSeen) {
                    *error = Usage;
                    return false;
                }
                candidate.furnaceRequested = true;
                furnaceSeen = true;
                ++index;
            } else if (option == "--denoise") {
                if (denoiseSeen || index + 1 >= argumentCount
                    || arguments[index + 1] == nullptr) {
                    *error = Usage;
                    return false;
                }
                const std::string_view value(arguments[index + 1]);
                if (value == "off") {
                    candidate.denoise = DenoiseMode::Off;
                } else if (value == "spatial") {
                    candidate.denoise = DenoiseMode::Spatial;
                } else if (value == "temporal") {
                    *error = "Denoise mode 'temporal' arrives with plan phase D3; "
                             "use 'off' or 'spatial'.";
                    return false;
                } else {
                    *error = "Denoise mode must be 'off', 'spatial', or 'temporal'.";
                    return false;
                }
                denoiseSeen = true;
                index += 2;
            } else if (option == "--gbuffer-probe") {
                if (candidate.gbufferProbeRequested
                    || candidate.denoiseProbeRequested) {
                    *error = Usage;
                    return false;
                }
                candidate.gbufferProbeRequested = true;
                ++index;
            } else if (option == "--denoise-probe") {
                if (candidate.denoiseProbeRequested
                    || candidate.gbufferProbeRequested) {
                    *error = Usage;
                    return false;
                }
                candidate.denoiseProbeRequested = true;
                ++index;
            } else {
                *error = Usage;
                return false;
            }
        }
        if (candidate.mode == CommandLineMode::Reference && !estimatorSeen) {
            *error = "Reference mode requires --estimator.";
            return false;
        }
        if (candidate.mode != CommandLineMode::Reference && estimatorSeen) {
            *error = "--estimator is only valid with --reference.";
            return false;
        }
        if (candidate.furnaceRequested
            && candidate.mode != CommandLineMode::Reference) {
            *error = "--furnace is only valid with --reference.";
            return false;
        }
        if (candidate.furnaceRequested
            && candidate.estimator != EstimatorMode::Bsdf) {
            *error = "--furnace requires --estimator bsdf.";
            return false;
        }
        if (candidate.furnaceRequested && candidate.timeOfDayHours.has_value()) {
            *error = "--time is not valid with the constant furnace environment.";
            return false;
        }
        if (denoiseSeen && candidate.mode == CommandLineMode::Reference) {
            *error = "Reference mode bypasses the denoiser structurally; "
                     "--denoise is not valid with --reference.";
            return false;
        }
        if (candidate.gbufferProbeRequested
            && candidate.mode != CommandLineMode::Capture) {
            *error = "--gbuffer-probe is only valid with --capture.";
            return false;
        }
        if (candidate.denoiseProbeRequested
            && candidate.mode != CommandLineMode::Capture) {
            *error = "--denoise-probe is only valid with --capture.";
            return false;
        }
        if (candidate.gbufferProbeRequested
            && candidate.captureFrameCount < MinimumGBufferProbeFrameCount) {
            *error = "--gbuffer-probe requires at least 128 capture frames "
                     "(the dynamic crate must settle clear of the wall probe's "
                     "sight line).";
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

bool extractGBufferProbeSample(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t x,
    std::uint32_t y,
    std::span<const std::uint16_t> normalDepthRgba16,
    std::span<const std::uint8_t> albedoRgba8,
    std::span<const std::uint32_t> instanceIds,
    GBufferProbeSample* sample)
{
    if (sample == nullptr
        || width == 0
        || height == 0
        || x >= width
        || y >= height) {
        return false;
    }
    const std::size_t sizeWidth = width;
    const std::size_t sizeHeight = height;
    if (sizeWidth > std::numeric_limits<std::size_t>::max() / sizeHeight) {
        return false;
    }
    const std::size_t pixelCount = sizeWidth * sizeHeight;
    if (pixelCount > std::numeric_limits<std::size_t>::max() / 4
        || normalDepthRgba16.size() != pixelCount * 4
        || albedoRgba8.size() != pixelCount * 4
        || instanceIds.size() != pixelCount) {
        return false;
    }

    const std::size_t pixelIndex = static_cast<std::size_t>(y) * sizeWidth + x;
    GBufferProbeSample candidate;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        candidate.normal[channel] =
            binary16ToFloat(normalDepthRgba16[pixelIndex * 4 + channel]);
        candidate.albedo[channel] =
            static_cast<float>(albedoRgba8[pixelIndex * 4 + channel]) / 255.0f;
    }
    candidate.viewDepth = binary16ToFloat(normalDepthRgba16[pixelIndex * 4 + 3]);
    candidate.instanceId = instanceIds[pixelIndex];
    candidate.primaryGlass = albedoRgba8[pixelIndex * 4 + 3] >= 128;
    *sample = candidate;
    return true;
}

bool expectedPlaneViewDepth(
    const CameraPushConstants& camera,
    float jitterX,
    float jitterY,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t planeAxis,
    float planeCoordinate,
    float* viewDepth)
{
    if (viewDepth == nullptr
        || planeAxis > 2
        || width == 0
        || height == 0
        || x >= width
        || y >= height) {
        return false;
    }

    // Mirror rayGenMain: jittered pixel center to [-1, 1] NDC, with the dispatch-space
    // row flip applied as a negated up contribution.
    const float ndcX = (static_cast<float>(x) + 0.5f + jitterX)
        / static_cast<float>(width) * 2.0f - 1.0f;
    const float ndcY = (static_cast<float>(y) + 0.5f + jitterY)
        / static_cast<float>(height) * 2.0f - 1.0f;
    const glm::vec3 direction = glm::normalize(
        camera.forward + ndcX * camera.right - ndcY * camera.up);
    const float axisDirection = direction[static_cast<glm::length_t>(planeAxis)];
    const float axisDistance =
        camera.origin[static_cast<glm::length_t>(planeAxis)] - planeCoordinate;
    if (!std::isfinite(axisDirection) || axisDirection * axisDistance >= 0.0f) {
        return false;
    }
    const float hitDistance = -axisDistance / axisDirection;
    const float depth = hitDistance * glm::dot(direction, camera.forward);
    if (!std::isfinite(depth) || depth <= 0.0f) {
        return false;
    }
    *viewDepth = depth;
    return true;
}

bool projectWorldPointToPixel(
    const CameraPushConstants& camera,
    float jitterX,
    float jitterY,
    std::uint32_t width,
    std::uint32_t height,
    const std::array<float, 3>& worldPoint,
    std::uint32_t* x,
    std::uint32_t* y)
{
    if (x == nullptr || y == nullptr || width == 0 || height == 0) {
        return false;
    }

    // The camera basis is mutually orthogonal by construction (forward unit,
    // right/up pre-scaled), so the NDC coefficients fall out of three dot
    // products: toPoint = s * (forward + ndcX * right - ndcY * up).
    const glm::vec3 toPoint = glm::vec3(
        worldPoint[0],
        worldPoint[1],
        worldPoint[2]) - camera.origin;
    const float forwardDistance = glm::dot(toPoint, camera.forward);
    const float rightScaleSquared = glm::dot(camera.right, camera.right);
    const float upScaleSquared = glm::dot(camera.up, camera.up);
    if (!(forwardDistance > 0.0f)
        || !(rightScaleSquared > 0.0f)
        || !(upScaleSquared > 0.0f)) {
        return false;
    }
    const float ndcX = glm::dot(toPoint, camera.right)
        / (forwardDistance * rightScaleSquared);
    const float ndcY = -glm::dot(toPoint, camera.up)
        / (forwardDistance * upScaleSquared);

    // Invert the jittered pixel-center mapping and round to the nearest pixel.
    const float pixelX = (ndcX + 1.0f) * 0.5f * static_cast<float>(width)
        - 0.5f - jitterX;
    const float pixelY = (ndcY + 1.0f) * 0.5f * static_cast<float>(height)
        - 0.5f - jitterY;
    const float roundedX = std::round(pixelX);
    const float roundedY = std::round(pixelY);
    if (!std::isfinite(roundedX) || !std::isfinite(roundedY)
        || roundedX < 0.0f || roundedY < 0.0f
        || roundedX >= static_cast<float>(width)
        || roundedY >= static_cast<float>(height)) {
        return false;
    }
    *x = static_cast<std::uint32_t>(roundedX);
    *y = static_cast<std::uint32_t>(roundedY);
    return true;
}

bool gbufferSurfaceProbePasses(
    const GBufferProbeSample& sample,
    const std::array<float, 3>& expectedNormal,
    float expectedViewDepth,
    const std::array<float, 3>& expectedAlbedo,
    std::uint32_t expectedInstanceId)
{
    // Probe surfaces are flat box faces whose normals are axis-aligned before the
    // instance transform, so the normal tolerance only absorbs binary16 storage
    // and the shader's rotation arithmetic. Depth compares an fp32 analytic value
    // against binary16 storage of the GPU's fp32 result; albedo absorbs one UNORM8
    // step on top of the quantized expectation.
    constexpr float NormalTolerance = 2.0e-3f;
    constexpr float DepthRelativeTolerance = 5.0e-3f;
    constexpr float AlbedoTolerance = 2.0f / 255.0f;
    if (sample.instanceId != expectedInstanceId || sample.primaryGlass) {
        return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!(std::fabs(sample.normal[axis] - expectedNormal[axis])
                <= NormalTolerance)) {
            return false;
        }
    }
    if (!(expectedViewDepth > 0.0f)
        || !(std::fabs(sample.viewDepth - expectedViewDepth)
            <= expectedViewDepth * DepthRelativeTolerance)) {
        return false;
    }
    for (std::size_t channel = 0; channel < 3; ++channel) {
        if (!(std::fabs(sample.albedo[channel] - expectedAlbedo[channel])
                <= AlbedoTolerance)) {
            return false;
        }
    }
    return true;
}

bool gbufferSkyProbePasses(const GBufferProbeSample& sample)
{
    return sample.instanceId == GBufferMissInstanceId
        && sample.viewDepth == 0.0f
        && sample.normal[0] == 0.0f
        && sample.normal[1] == 0.0f
        && sample.normal[2] == 0.0f
        && sample.albedo[0] == 0.0f
        && sample.albedo[1] == 0.0f
        && sample.albedo[2] == 0.0f
        && !sample.primaryGlass;
}

bool summarizeDenoiseAcceptance(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint16_t> hdrRgba16,
    std::span<const std::uint16_t> normalDepthRgba16,
    std::span<const std::uint8_t> albedoRgba8,
    DenoiseAcceptanceSummary* summary)
{
    if (summary == nullptr || width == 0 || height == 0) {
        return false;
    }
    const std::uint64_t pixelCount64 =
        static_cast<std::uint64_t>(width) * height;
    if (pixelCount64 > std::numeric_limits<std::size_t>::max() / 4
        || pixelCount64 > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const std::size_t pixelCount = static_cast<std::size_t>(pixelCount64);
    if (hdrRgba16.size() != pixelCount * 4
        || normalDepthRgba16.size() != pixelCount * 4
        || albedoRgba8.size() != pixelCount * 4) {
        return false;
    }

    const auto isGlass = [&](std::size_t pixel) {
        return albedoRgba8[pixel * 4 + 3] >= 128;
    };
    const auto depth = [&](std::size_t pixel) {
        return binary16ToFloat(normalDepthRgba16[pixel * 4 + 3]);
    };
    const auto radiance = [&](std::size_t pixel, std::size_t channel) {
        return binary16ToFloat(hdrRgba16[pixel * 4 + channel]);
    };
    const auto luminanceAt = [&](std::size_t pixel) {
        constexpr float DemodulationFloor = 1.0f / 64.0f;
        const auto demodulated = [&](std::size_t channel) {
            const float albedo = static_cast<float>(
                albedoRgba8[pixel * 4 + channel]) / 255.0f;
            return radiance(pixel, channel)
                / std::max(albedo, DemodulationFloor);
        };
        return 0.2126f * demodulated(0)
            + 0.7152f * demodulated(1)
            + 0.0722f * demodulated(2);
    };
    const auto sameSurface = [&](std::size_t center, std::size_t sample) {
        const float centerDepth = depth(center);
        const float sampleDepth = depth(sample);
        if (!(centerDepth > 0.0f) || !(sampleDepth > 0.0f)) {
            return false;
        }
        float normalDot = 0.0f;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            normalDot += binary16ToFloat(normalDepthRgba16[center * 4 + axis])
                * binary16ToFloat(normalDepthRgba16[sample * 4 + axis]);
        }
        const float depthTolerance = std::max(
            DenoiseSurfaceDepthAbsoluteThreshold,
            DenoiseSurfaceDepthRelativeThreshold * centerDepth);
        return normalDot >= DenoiseSurfaceNormalThreshold
            && std::abs(centerDepth - sampleDepth) <= depthTolerance;
    };

    DenoiseAcceptanceSummary candidate;
    candidate.glassRadianceHash = Fnv1aOffsetBasis;
    for (unsigned int byteIndex = 0; byteIndex < 4; ++byteIndex) {
        hashByte(
            static_cast<std::uint8_t>(width >> (byteIndex * 8)),
            &candidate.glassRadianceHash);
        hashByte(
            static_cast<std::uint8_t>(height >> (byteIndex * 8)),
            &candidate.glassRadianceHash);
    }

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const float value = radiance(pixel, channel);
                if (!std::isfinite(value) || value < 0.0f) {
                    return false;
                }
            }
            if (isGlass(pixel)) {
                ++candidate.glassPixelCount;
                const std::uint32_t pixel32 = static_cast<std::uint32_t>(pixel);
                for (unsigned int byteIndex = 0; byteIndex < 4; ++byteIndex) {
                    hashByte(
                        static_cast<std::uint8_t>(pixel32 >> (byteIndex * 8)),
                        &candidate.glassRadianceHash);
                }
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const std::uint16_t half = hdrRgba16[pixel * 4 + channel];
                    hashByte(
                        static_cast<std::uint8_t>(half & 0xffu),
                        &candidate.glassRadianceHash);
                    hashByte(
                        static_cast<std::uint8_t>(half >> 8u),
                        &candidate.glassRadianceHash);
                }
                continue;
            }
            if (!(depth(pixel) > 0.0f)) {
                continue;
            }

            std::array<float, 8> neighborLuminance{};
            std::size_t neighborCount = 0;
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    if (offsetX == 0 && offsetY == 0) {
                        continue;
                    }
                    const int neighborX = static_cast<int>(x) + offsetX;
                    const int neighborY = static_cast<int>(y) + offsetY;
                    if (neighborX < 0 || neighborY < 0
                        || neighborX >= static_cast<int>(width)
                        || neighborY >= static_cast<int>(height)) {
                        continue;
                    }
                    const std::size_t neighbor =
                        static_cast<std::size_t>(neighborY) * width
                        + static_cast<std::uint32_t>(neighborX);
                    if (isGlass(neighbor) || !sameSurface(pixel, neighbor)) {
                        continue;
                    }
                    neighborLuminance[neighborCount++] = luminanceAt(neighbor);
                }
            }
            if (neighborCount < 4) {
                continue;
            }
            for (std::size_t index = 1; index < neighborCount; ++index) {
                const float value = neighborLuminance[index];
                std::size_t insertion = index;
                while (insertion > 0
                    && neighborLuminance[insertion - 1] > value) {
                    neighborLuminance[insertion] =
                        neighborLuminance[insertion - 1];
                    --insertion;
                }
                neighborLuminance[insertion] = value;
            }
            const float median = neighborLuminance[neighborCount / 2];
            const float ceiling = std::max(
                median * DenoiseFireflyMedianMultiple,
                median + DenoiseFireflyMinimumAllowance);
            const float centerLuminance = luminanceAt(pixel);
            if (centerLuminance > ceiling) {
                if (candidate.isolatedHotPixelCount == 0) {
                    candidate.firstHotPixelX = x;
                    candidate.firstHotPixelY = y;
                    candidate.firstHotPixelLuminance = centerLuminance;
                    candidate.firstHotPixelCeiling = ceiling;
                }
                ++candidate.isolatedHotPixelCount;
            }
        }
    }
    *summary = candidate;
    return true;
}

bool accumulateReferenceImage(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint16_t> rgba16,
    ReferenceAccumulator* accumulator)
{
    constexpr std::uint32_t Denominator = 8;
    constexpr std::array<FurnaceRegion, ReferenceRegionCount> Regions{{
        {3, 5, 3, 5},
        {4, 6, 2, 4},
        {2, 6, 4, 6},
    }};
    return accumulateRegions(
        width, height, rgba16, Regions, Denominator, accumulator);
}

bool summarizeReferenceRegions(
    const ReferenceAccumulator& accumulator,
    std::array<ReferenceRegionSummary, ReferenceRegionCount>* summaries)
{
    return summarizeRegions(accumulator, summaries);
}

bool accumulateFurnaceImage(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::uint16_t> rgba16,
    FurnaceAccumulator* accumulator)
{
    return accumulateRegions(
        width,
        height,
        rgba16,
        FurnaceRegions,
        FurnaceRegionDenominator,
        accumulator);
}

bool summarizeFurnaceCases(
    const FurnaceAccumulator& accumulator,
    std::array<ReferenceRegionSummary, FurnaceCaseCount>* summaries)
{
    return summarizeRegions(accumulator, summaries);
}

bool furnaceCasePasses(const ReferenceRegionSummary& summary)
{
    constexpr double Expected = FurnaceEnvironmentRadiance;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const double mean = summary.mean[channel];
        const double standardError = summary.standardError[channel];
        if (!std::isfinite(mean) || !std::isfinite(standardError)
            || mean < 0.0 || standardError < 0.0
            || std::abs(mean / Expected - 1.0)
                > std::max(0.02, 3.0 * standardError / Expected)) {
            return false;
        }
    }
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
