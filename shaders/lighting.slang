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

float3 skyRadiance(FrameLighting frameLighting, float3 direction)
{
    // Phase 1 has no analytic ground-light model. Geometry supplies real ground
    // bounce; an escaping direction below the world horizon sees black rather than
    // receiving fictitious horizon radiance from underneath the scene.
    if (direction.y < 0.0)
        return float3(0.0);
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
