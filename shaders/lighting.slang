import records;
import sampling;

// Analytic environment evaluation and its surface-conditioned direct-light proposal.
// The cosine proposal is normalized over the shading hemisphere.
struct SkySample
{
    float3 direction;
    float3 radiance;
    float pdf;
};

struct SunSample
{
    float3 direction;
    float3 radiance;
    float pdf;
    bool delta;
};

static const uint PerezSkyBit = 1u << 0;
static const uint FullSphereSkyBit = 1u << 4;

float3 perezDistribution(
    float3 coefficientA,
    float3 coefficientB,
    float3 coefficientC,
    float3 coefficientD,
    float3 coefficientE,
    float cosineTheta,
    float gamma,
    float cosineGamma)
{
    cosineTheta = max(cosineTheta, 0.01);
    return (1.0 + coefficientA * exp(coefficientB / cosineTheta))
        * (1.0 + coefficientC * exp(coefficientD * gamma)
            + coefficientE * cosineGamma * cosineGamma);
}

float3 xyYToLinearRgb(float3 xyY)
{
    float chromaticityY = max(xyY.y, 1.0e-4);
    float luminanceY = max(xyY.z, 0.0);
    float tristimulusX = xyY.x * luminanceY / chromaticityY;
    float tristimulusZ = max(
        0.0,
        (1.0 - xyY.x - xyY.y) * luminanceY / chromaticityY);
    return max(float3(
        3.2406 * tristimulusX - 1.5372 * luminanceY
            - 0.4986 * tristimulusZ,
        -0.9689 * tristimulusX + 1.8758 * luminanceY
            + 0.0415 * tristimulusZ,
        0.0557 * tristimulusX - 0.2040 * luminanceY
            + 1.0570 * tristimulusZ), 0.0);
}

float3 skyRadiance(FrameLighting frameLighting, float3 direction)
{
    // There is no analytic ground-light model. Geometry supplies real ground
    // bounce; an escaping direction below the world horizon sees black rather than
    // receiving fictitious horizon radiance from underneath the scene.
    if (direction.y < 0.0
        && (frameLighting.flags & FullSphereSkyBit) == 0)
        return float3(0.0);
    if ((frameLighting.flags & PerezSkyBit) != 0)
    {
        float upperY = saturate(direction.y);
        float3 nightRadiance = lerp(
            frameLighting.skyHorizon,
            frameLighting.nightZenith.xyz,
            upperY);
        if (frameLighting.daylightBlend <= 0.0)
            return nightRadiance;

        float cosineGamma = clamp(
            dot(direction, frameLighting.sunDirection),
            -1.0,
            1.0);
        float gamma = acos(cosineGamma);
        float3 numerator = perezDistribution(
            frameLighting.skyPerezA.xyz,
            frameLighting.skyPerezB.xyz,
            frameLighting.skyPerezC.xyz,
            frameLighting.skyPerezD.xyz,
            frameLighting.skyPerezE.xyz,
            upperY,
            gamma,
            cosineGamma);
        float cosineSunZenith = saturate(frameLighting.sunDirection.y);
        float sunZenith = acos(cosineSunZenith);
        float3 denominator = perezDistribution(
            frameLighting.skyPerezA.xyz,
            frameLighting.skyPerezB.xyz,
            frameLighting.skyPerezC.xyz,
            frameLighting.skyPerezD.xyz,
            frameLighting.skyPerezE.xyz,
            1.0,
            sunZenith,
            cosineSunZenith);
        float3 xyY = frameLighting.skyZenith * numerator
            / max(denominator, 1.0e-6);
        return lerp(
            nightRadiance,
            xyYToLinearRgb(xyY),
            frameLighting.daylightBlend);
    }
    return lerp(
        frameLighting.skyHorizon,
        frameLighting.skyZenith,
        saturate(direction.y));
}

float skyPdf(float3 surfaceNormal, float3 direction, bool twoSided)
{
    float cosine = dot(surfaceNormal, direction);
    return twoSided
        ? 0.5 * abs(cosine) * InversePi
        : max(cosine, 0.0) * InversePi;
}

SkySample sampleSky(
    FrameLighting frameLighting,
    float3 surfaceNormal,
    bool twoSided,
    float2 sample)
{
    SkySample result;
    float3 hemisphereNormal = surfaceNormal;
    float2 hemisphereSample = sample;
    if (twoSided)
    {
        bool opposite = sample.x >= 0.5;
        hemisphereNormal = opposite ? -surfaceNormal : surfaceNormal;
        hemisphereSample.x = frac(sample.x * 2.0);
    }
    result.direction = cosineSampleHemisphere(hemisphereNormal, hemisphereSample);
    result.radiance = skyRadiance(frameLighting, result.direction);
    result.pdf = skyPdf(surfaceNormal, result.direction, twoSided);
    return result;
}

float sunPdf(FrameLighting frameLighting, float3 direction)
{
    return frameLighting.sunSolidAngle > 0.0
        && dot(frameLighting.sunDirection, direction)
            >= frameLighting.sunCosineHalfAngle
        ? 1.0 / frameLighting.sunSolidAngle
        : 0.0;
}

float3 sunRadiance(FrameLighting frameLighting, float3 direction)
{
    return sunPdf(frameLighting, direction) > 0.0
        ? frameLighting.sunIrradiance / frameLighting.sunSolidAngle
        : float3(0.0);
}

SunSample sampleSun(FrameLighting frameLighting, float2 sample)
{
    SunSample result;
    if (frameLighting.sunSolidAngle <= 0.0)
    {
        result.direction = frameLighting.sunDirection;
        result.radiance = frameLighting.sunIrradiance;
        result.pdf = 1.0;
        result.delta = true;
        return result;
    }

    float cosineTheta = lerp(
        1.0,
        frameLighting.sunCosineHalfAngle,
        sample.x);
    float sineTheta = sqrt(max(0.0, 1.0 - cosineTheta * cosineTheta));
    float phi = TwoPi * sample.y;
    OrthonormalBasis basis = makeOrthonormalBasis(frameLighting.sunDirection);
    result.direction = normalize(
        basis.tangent * (sineTheta * cos(phi))
        + basis.bitangent * (sineTheta * sin(phi))
        + frameLighting.sunDirection * cosineTheta);
    result.radiance = frameLighting.sunIrradiance
        / frameLighting.sunSolidAngle;
    result.pdf = 1.0 / frameLighting.sunSolidAngle;
    result.delta = false;
    return result;
}
