// Spatial denoise passes (TEMPORAL_DENOISING_PLAN.md D1): an edge-aware firefly
// clamp, albedo demodulation, a 7x7 spatial luminance-variance estimate, and the
// SVGF-style a-trous wavelet iterations. Everything runs on linear HDR radiance
// before tonemapping. Primary misses and primary Glass pixels pass through every
// stage untouched; a future transmission/specular filter may handle Glass.

[[vk::binding(0, 0)]]
[format("rgba16f")]
uniform RWTexture2D<float4> hdrRadianceImage;

[[vk::binding(1, 0)]]
[format("rgba16f")]
uniform RWTexture2D<float4> gbufferNormalDepthImage;

[[vk::binding(2, 0)]]
[format("rgba8")]
uniform RWTexture2D<float4> gbufferAlbedoImage;

// Ping-pong working pair: demodulated radiance in rgb, its variance in alpha, so
// variance travels the chain with the color and no separate ping-pong exists.
[[vk::binding(3, 0)]]
[format("rgba16f")]
uniform RWTexture2D<float4> workingInput;

[[vk::binding(4, 0)]]
[format("rgba16f")]
uniform RWTexture2D<float4> workingOutput;

struct DenoisePush
{
    uint stride;
    uint remodulate;
};

[[vk::push_constant]]
ConstantBuffer<DenoisePush> denoisePush;

// Compile-time filter policy (mirrored only where a C++ test needs it).
// DemodulationFloor keeps the demodulate/remodulate round trip exact for black
// albedo; the sigmas are the standard SVGF edge-stopping scales.
static const float DemodulationFloor = 1.0 / 64.0;
static const float SigmaDepth = 1.0;
static const float SigmaNormal = 128.0;
static const float SigmaLuminance = 4.0;
static const float WeightEpsilon = 1.0e-4;
static const float SurfaceNormalThreshold = 0.9;
static const float SurfaceDepthRelativeThreshold = 0.02;
static const float SurfaceDepthAbsoluteThreshold = 0.05;
static const float FireflyBaselineMultiple = 6.0;
static const float FireflyMinimumAllowance = 0.05;

float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 demodulationAlbedo(uint2 pixel)
{
    return max(gbufferAlbedoImage[pixel].rgb, DemodulationFloor);
}

bool isMissPixel(float4 normalDepth)
{
    return normalDepth.w <= 0.0;
}

bool isGlassPixel(uint2 pixel)
{
    return gbufferAlbedoImage[pixel].a >= 0.5;
}

bool isSameSurface(float4 centerNormalDepth, float4 sampleNormalDepth)
{
    if (isMissPixel(sampleNormalDepth))
        return false;
    float depthTolerance = max(
        SurfaceDepthAbsoluteThreshold,
        SurfaceDepthRelativeThreshold * centerNormalDepth.w);
    return dot(centerNormalDepth.xyz, sampleNormalDepth.xyz)
            >= SurfaceNormalThreshold
        && abs(centerNormalDepth.w - sampleNormalDepth.w) <= depthTolerance;
}

// Prepare the working radiance before variance sees it. A robust 5x5 trimmed
// luminance estimate discards up to the four brightest comparable samples before
// setting a one-sided ceiling, so a small hot cluster cannot raise its own limit.
// Geometry tests exclude unrelated surfaces. Only luminance is limited, preserving
// chroma, and the fixed top-four reduction avoids per-pixel array sorting.
[shader("compute")]
[numthreads(8, 8, 1)]
void clampMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    hdrRadianceImage.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    uint2 pixel = dispatchThreadId.xy;
    float4 centerNormalDepth = gbufferNormalDepthImage[pixel];
    float3 centerRadiance = max(hdrRadianceImage[pixel].rgb, 0.0);
    if (isMissPixel(centerNormalDepth) || isGlassPixel(pixel))
    {
        workingOutput[pixel] = float4(centerRadiance, 0.0);
        return;
    }

    float3 centerDemodulated = centerRadiance / demodulationAlbedo(pixel);
    float luminanceSum = 0.0;
    float brightest[4] = {0.0, 0.0, 0.0, 0.0};
    uint sampleCount = 0;
    for (int offsetY = -2; offsetY <= 2; ++offsetY)
    {
        for (int offsetX = -2; offsetX <= 2; ++offsetX)
        {
            int2 neighbor = int2(pixel) + int2(offsetX, offsetY);
            if (neighbor.x < 0 || neighbor.y < 0
                || neighbor.x >= int(width) || neighbor.y >= int(height))
                continue;
            uint2 neighborPixel = uint2(neighbor);
            if (isGlassPixel(neighborPixel)
                || !isSameSurface(
                    centerNormalDepth,
                    gbufferNormalDepthImage[neighborPixel]))
                continue;
            float sampleLuminance = luminance(
                max(hdrRadianceImage[neighborPixel].rgb, 0.0)
                / demodulationAlbedo(neighborPixel));
            luminanceSum += sampleLuminance;
            ++sampleCount;
            if (sampleLuminance > brightest[0])
            {
                brightest[3] = brightest[2];
                brightest[2] = brightest[1];
                brightest[1] = brightest[0];
                brightest[0] = sampleLuminance;
            }
            else if (sampleLuminance > brightest[1])
            {
                brightest[3] = brightest[2];
                brightest[2] = brightest[1];
                brightest[1] = sampleLuminance;
            }
            else if (sampleLuminance > brightest[2])
            {
                brightest[3] = brightest[2];
                brightest[2] = sampleLuminance;
            }
            else if (sampleLuminance > brightest[3])
            {
                brightest[3] = sampleLuminance;
            }
        }
    }

    if (sampleCount >= 5)
    {
        uint trimCount = min(4u, sampleCount / 5u);
        float trimmedSum = luminanceSum;
        for (uint index = 0; index < trimCount; ++index)
            trimmedSum -= brightest[index];
        float baseline = max(
            trimmedSum / float(sampleCount - trimCount),
            0.0);
        float ceiling = max(
            baseline * FireflyBaselineMultiple,
            baseline + FireflyMinimumAllowance);
        float centerLuminance = luminance(centerDemodulated);
        if (centerLuminance > ceiling)
            centerDemodulated *= ceiling / centerLuminance;
    }
    workingOutput[pixel] = float4(centerDemodulated, 0.0);
}

// 7x7 luminance variance of the prepared, demodulated radiance around the center.
// Neighbors outside the image, on another geometric surface, or marked Glass are
// excluded rather than clamped so boundaries do not manufacture variance.
[shader("compute")]
[numthreads(8, 8, 1)]
void varianceMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    workingInput.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    uint2 pixel = dispatchThreadId.xy;
    float4 centerNormalDepth = gbufferNormalDepthImage[pixel];
    float3 centerRadiance = workingInput[pixel].rgb;
    if (isMissPixel(centerNormalDepth) || isGlassPixel(pixel))
    {
        // Sky and Glass carry raw radiance with zero variance; the a-trous passes
        // and final remodulation leave them untouched.
        workingOutput[pixel] = float4(centerRadiance, 0.0);
        return;
    }

    float sum = 0.0;
    float squaredSum = 0.0;
    float count = 0.0;
    for (int offsetY = -3; offsetY <= 3; ++offsetY)
    {
        for (int offsetX = -3; offsetX <= 3; ++offsetX)
        {
            int2 neighbor = int2(pixel) + int2(offsetX, offsetY);
            if (neighbor.x < 0 || neighbor.y < 0
                || neighbor.x >= int(width) || neighbor.y >= int(height))
                continue;
            uint2 neighborPixel = uint2(neighbor);
            if (isGlassPixel(neighborPixel)
                || !isSameSurface(
                    centerNormalDepth,
                    gbufferNormalDepthImage[neighborPixel]))
                continue;
            float neighborLuminance = luminance(workingInput[neighborPixel].rgb);
            sum += neighborLuminance;
            squaredSum += neighborLuminance * neighborLuminance;
            count += 1.0;
        }
    }
    float mean = sum / count;
    float variance = max(squaredSum / count - mean * mean, 0.0);
    workingOutput[pixel] = float4(centerRadiance, variance);
}

// One edge-aware a-trous wavelet iteration over the demodulated working image.
// Weights follow SVGF: depth scaled by the screen-space depth derivative and the
// tap distance, normal agreement raised to SigmaNormal, and luminance scaled by
// the 3x3-prefiltered variance. Variance propagates through the squared kernel.
[shader("compute")]
[numthreads(8, 8, 1)]
void atrousMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    workingInput.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    uint2 pixel = dispatchThreadId.xy;
    float4 center = workingInput[pixel];
    float4 centerNormalDepth = gbufferNormalDepthImage[pixel];
    if (isMissPixel(centerNormalDepth) || isGlassPixel(pixel))
    {
        if (denoisePush.remodulate != 0)
            hdrRadianceImage[pixel] = float4(center.rgb, 1.0);
        else
            workingOutput[pixel] = center;
        return;
    }

    // Screen-space view-depth derivative from immediate neighbors; miss or
    // off-image neighbors fall back to the center depth so silhouettes do not
    // produce an unbounded gradient.
    float centerDepth = centerNormalDepth.w;
    float depthLeft = centerDepth;
    float depthRight = centerDepth;
    float depthUp = centerDepth;
    float depthDown = centerDepth;
    if (pixel.x > 0)
    {
        float4 sampleValue = gbufferNormalDepthImage[uint2(pixel.x - 1, pixel.y)];
        if (!isMissPixel(sampleValue))
            depthLeft = sampleValue.w;
    }
    if (pixel.x + 1 < width)
    {
        float4 sampleValue = gbufferNormalDepthImage[uint2(pixel.x + 1, pixel.y)];
        if (!isMissPixel(sampleValue))
            depthRight = sampleValue.w;
    }
    if (pixel.y > 0)
    {
        float4 sampleValue = gbufferNormalDepthImage[uint2(pixel.x, pixel.y - 1)];
        if (!isMissPixel(sampleValue))
            depthUp = sampleValue.w;
    }
    if (pixel.y + 1 < height)
    {
        float4 sampleValue = gbufferNormalDepthImage[uint2(pixel.x, pixel.y + 1)];
        if (!isMissPixel(sampleValue))
            depthDown = sampleValue.w;
    }
    float2 depthGradient = 0.5 * float2(
        depthRight - depthLeft,
        depthDown - depthUp);

    // 3x3 Gaussian-prefiltered variance drives the luminance edge weight.
    static const float GaussianKernel[3] = {0.25, 0.5, 0.25};
    float filteredVariance = 0.0;
    float filteredWeight = 0.0;
    for (int offsetY = -1; offsetY <= 1; ++offsetY)
    {
        for (int offsetX = -1; offsetX <= 1; ++offsetX)
        {
            int2 neighbor = int2(pixel) + int2(offsetX, offsetY);
            if (neighbor.x < 0 || neighbor.y < 0
                || neighbor.x >= int(width) || neighbor.y >= int(height))
                continue;
            uint2 neighborPixel = uint2(neighbor);
            if (isGlassPixel(neighborPixel)
                || !isSameSurface(
                    centerNormalDepth,
                    gbufferNormalDepthImage[neighborPixel]))
                continue;
            float kernelWeight =
                GaussianKernel[offsetX + 1] * GaussianKernel[offsetY + 1];
            filteredVariance += kernelWeight * workingInput[neighborPixel].a;
            filteredWeight += kernelWeight;
        }
    }
    filteredVariance = filteredWeight > 0.0
        ? filteredVariance / filteredWeight
        : center.a;
    float luminanceDenominator =
        SigmaLuminance * sqrt(max(filteredVariance, 0.0)) + WeightEpsilon;

    static const float AtrousKernel[5] = {0.0625, 0.25, 0.375, 0.25, 0.0625};
    float3 centerNormal = centerNormalDepth.xyz;
    float centerLuminance = luminance(center.rgb);
    int stride = int(denoisePush.stride);
    float3 colorSum = float3(0.0);
    float varianceSum = 0.0;
    float weightSum = 0.0;
    for (int tapY = -2; tapY <= 2; ++tapY)
    {
        for (int tapX = -2; tapX <= 2; ++tapX)
        {
            int2 neighbor = int2(pixel) + int2(tapX, tapY) * stride;
            if (neighbor.x < 0 || neighbor.y < 0
                || neighbor.x >= int(width) || neighbor.y >= int(height))
                continue;
            uint2 neighborPixel = uint2(neighbor);
            float4 neighborNormalDepth = gbufferNormalDepthImage[neighborPixel];
            if (isGlassPixel(neighborPixel)
                || !isSameSurface(centerNormalDepth, neighborNormalDepth))
                continue;
            float4 neighborValue = workingInput[neighborPixel];

            float kernelWeight = AtrousKernel[tapX + 2] * AtrousKernel[tapY + 2];
            float normalWeight = pow(
                max(dot(centerNormal, neighborNormalDepth.xyz), 0.0),
                SigmaNormal);
            float2 tapOffset = float2(tapX * stride, tapY * stride);
            float depthDenominator =
                SigmaDepth * abs(dot(depthGradient, tapOffset)) + WeightEpsilon;
            float depthWeight = exp(
                -abs(centerDepth - neighborNormalDepth.w) / depthDenominator);
            float luminanceWeight = exp(
                -abs(centerLuminance - luminance(neighborValue.rgb))
                / luminanceDenominator);
            float weight = kernelWeight * normalWeight * depthWeight
                * luminanceWeight;

            colorSum += weight * neighborValue.rgb;
            varianceSum += weight * weight * neighborValue.a;
            weightSum += weight;
        }
    }

    // The center tap always contributes, so weightSum cannot be zero.
    float4 filtered = float4(
        colorSum / weightSum,
        varianceSum / (weightSum * weightSum));
    if (denoisePush.remodulate != 0)
        hdrRadianceImage[pixel] = float4(
            filtered.rgb * demodulationAlbedo(pixel),
            1.0);
    else
        workingOutput[pixel] = filtered;
}
