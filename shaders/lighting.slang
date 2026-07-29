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

float skyPdf(float3 surfaceNormal, float3 direction)
{
    return max(dot(surfaceNormal, direction), 0.0) * InversePi;
}

SkySample sampleSky(
    FrameLighting frameLighting,
    float3 surfaceNormal,
    float2 sample)
{
    SkySample result;
    result.direction = cosineSampleHemisphere(surfaceNormal, sample);
    result.radiance = skyRadiance(frameLighting, result.direction);
    result.pdf = skyPdf(surfaceNormal, result.direction);
    return result;
}
