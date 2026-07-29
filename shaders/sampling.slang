// Random-number, directional-sampling, and MIS helpers shared by the path tracer.

static const float InversePi = 0.31830988618379067154;
static const float TwoPi = 6.28318530717958647692;

uint pcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word =
        ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float rngNextFloat(inout uint state)
{
    state = pcgHash(state);
    return float(state >> 8u) * (1.0 / 16777216.0);
}

struct OrthonormalBasis
{
    float3 tangent;
    float3 bitangent;
};

OrthonormalBasis makeOrthonormalBasis(float3 normal)
{
    // Duff's sign-stable revision of Frisvad's orthonormal basis avoids the
    // near-pole cancellation of a cross-product basis without adding a branch on
    // an arbitrary helper axis.
    float sign = normal.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (sign + normal.z);
    float b = normal.x * normal.y * a;
    OrthonormalBasis result;
    result.tangent = float3(
        1.0 + sign * normal.x * normal.x * a,
        sign * b,
        -sign * normal.x);
    result.bitangent = float3(
        b,
        sign + normal.y * normal.y * a,
        -normal.y);
    return result;
}

float3 cosineSampleHemisphere(float3 normal, float2 sample)
{
    float radius = sqrt(sample.x);
    float angle = TwoPi * sample.y;
    float3 localDirection = float3(
        radius * cos(angle),
        radius * sin(angle),
        sqrt(max(0.0, 1.0 - sample.x)));
    OrthonormalBasis basis = makeOrthonormalBasis(normal);
    return normalize(
        basis.tangent * localDirection.x
        + basis.bitangent * localDirection.y
        + normal * localDirection.z);
}

float powerHeuristic(float firstPdf, float secondPdf)
{
    float firstSquared = firstPdf * firstPdf;
    float secondSquared = secondPdf * secondPdf;
    return firstSquared / max(firstSquared + secondSquared, 1.0e-20);
}
