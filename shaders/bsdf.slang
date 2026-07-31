import sampling;

static const uint MaterialClassDielectric = 0;
static const uint MaterialClassMetal = 1;

// Material-independent surface orientation plus the landed opaque BSDF classes.

struct OrientedSurfaceNormals
{
    float3 shading;
    float3 geometric;
};

OrientedSurfaceNormals orientSurfaceNormals(
    float3 shadingNormal,
    float3 geometricNormal,
    float3 wo)
{
    OrientedSurfaceNormals result;
    result.geometric = dot(geometricNormal, wo) < 0.0
        ? -geometricNormal
        : geometricNormal;
    result.shading = dot(shadingNormal, wo) < 0.0
        ? -shadingNormal
        : shadingNormal;
    if (dot(result.shading, result.geometric) <= 0.0)
        result.shading = result.geometric;
    return result;
}


float fresnelSchlick(float cosine, float f0)
{
    float oneMinusCosine = 1.0 - saturate(cosine);
    float factor = oneMinusCosine * oneMinusCosine;
    factor *= factor * oneMinusCosine;
    return f0 + (1.0 - f0) * factor;
}

float3 fresnelSchlick(float cosine, float3 f0)
{
    float oneMinusCosine = 1.0 - saturate(cosine);
    float factor = oneMinusCosine * oneMinusCosine;
    factor *= factor * oneMinusCosine;
    return f0 + (float3(1.0) - f0) * factor;
}

float ggxDistribution(float normalDotHalf, float alpha)
{
    float alphaSquared = alpha * alpha;
    float denominator = normalDotHalf * normalDotHalf * (alphaSquared - 1.0) + 1.0;
    // At the minimum alpha, the exact peak denominator is below one float ULP from
    // one and can cancel to zero. alphaSquared is its analytic lower bound.
    denominator = max(denominator, alphaSquared);
    return alphaSquared * InversePi / (denominator * denominator);
}

float ggxSmithG1(float normalDotDirection, float alpha)
{
    if (normalDotDirection <= 0.0)
        return 0.0;
    float alphaSquared = alpha * alpha;
    float cosineSquared = normalDotDirection * normalDotDirection;
    return 2.0 * normalDotDirection
        / (normalDotDirection
            + sqrt(alphaSquared + (1.0 - alphaSquared) * cosineSquared));
}

float3 evaluateDielectricBrdf(
    float3 albedo,
    float perceptualRoughness,
    float dielectricF0,
    float3 normal,
    float3 wo,
    float3 wi)
{
    float normalDotOut = dot(normal, wo);
    float normalDotIn = dot(normal, wi);
    if (normalDotOut <= 0.0 || normalDotIn <= 0.0)
        return float3(0.0);

    float3 halfVectorSum = wo + wi;
    float halfLengthSquared = dot(halfVectorSum, halfVectorSum);
    if (halfLengthSquared <= 1.0e-12)
        return float3(0.0);
    float3 halfVector = halfVectorSum * rsqrt(halfLengthSquared);
    float normalDotHalf = saturate(dot(normal, halfVector));
    float outDotHalf = saturate(dot(wo, halfVector));
    float alpha = max(perceptualRoughness * perceptualRoughness, 1.0e-4);

    float fresnel = fresnelSchlick(outDotHalf, dielectricF0);
    float distribution = ggxDistribution(normalDotHalf, alpha);
    float masking = ggxSmithG1(normalDotOut, alpha)
        * ggxSmithG1(normalDotIn, alpha);
    float3 specular = float3(
        fresnel * distribution * masking
            / max(4.0 * normalDotOut * normalDotIn, 1.0e-8));

    // Diffuse light crosses the dielectric boundary on entry and exit. Applying
    // Schlick at both sides prevents the diffuse and specular lobes from simply
    // adding energy at grazing angles.
    float diffuseTransmission =
        (1.0 - fresnelSchlick(normalDotIn, dielectricF0))
        * (1.0 - fresnelSchlick(normalDotOut, dielectricF0));
    float3 diffuse = albedo * InversePi * diffuseTransmission;
    return diffuse + specular;
}

float3 evaluateMetalBrdf(
    float3 spectralF0,
    float perceptualRoughness,
    float3 normal,
    float3 wo,
    float3 wi)
{
    float normalDotOut = dot(normal, wo);
    float normalDotIn = dot(normal, wi);
    if (normalDotOut <= 0.0 || normalDotIn <= 0.0)
        return float3(0.0);

    float3 halfVectorSum = wo + wi;
    float halfLengthSquared = dot(halfVectorSum, halfVectorSum);
    if (halfLengthSquared <= 1.0e-12)
        return float3(0.0);
    float3 halfVector = halfVectorSum * rsqrt(halfLengthSquared);
    float normalDotHalf = saturate(dot(normal, halfVector));
    float outDotHalf = saturate(dot(wo, halfVector));
    float alpha = max(perceptualRoughness * perceptualRoughness, 1.0e-4);

    float3 fresnel = fresnelSchlick(outDotHalf, spectralF0);
    float distribution = ggxDistribution(normalDotHalf, alpha);
    float masking = ggxSmithG1(normalDotOut, alpha)
        * ggxSmithG1(normalDotIn, alpha);
    return fresnel * distribution * masking
        / max(4.0 * normalDotOut * normalDotIn, 1.0e-8);
}

float3 evaluateBsdf(
    uint materialClass,
    float3 baseColor,
    float perceptualRoughness,
    float dielectricF0,
    float3 normal,
    float3 wo,
    float3 wi)
{
    if (materialClass == MaterialClassMetal)
        return evaluateMetalBrdf(
            baseColor,
            perceptualRoughness,
            normal,
            wo,
            wi);
    return evaluateDielectricBrdf(
        baseColor,
        perceptualRoughness,
        dielectricF0,
        normal,
        wo,
        wi);
}

float2 brdfLobeProbabilities(float3 albedo, float dielectricF0)
{
    float averageFresnel = dielectricF0 + (1.0 - dielectricF0) / 21.0;
    float albedoLuminance = dot(albedo, float3(0.2126, 0.7152, 0.0722));
    float diffuseEnergy = max(0.0, albedoLuminance * (1.0 - averageFresnel));
    float specularEnergy = max(0.0, averageFresnel);
    float totalEnergy = diffuseEnergy + specularEnergy;
    if (totalEnergy <= 0.0)
        return float2(0.0);
    return float2(diffuseEnergy, specularEnergy) / totalEnergy;
}

float3 sampleGgxVisibleNormal(
    float3 normal,
    float3 wo,
    float alpha,
    float2 sample)
{
    OrthonormalBasis basis = makeOrthonormalBasis(normal);
    float3 localView = float3(
        dot(wo, basis.tangent),
        dot(wo, basis.bitangent),
        dot(wo, normal));
    float3 stretchedView = normalize(float3(
        alpha * localView.x,
        alpha * localView.y,
        localView.z));

    float projectedLengthSquared = stretchedView.x * stretchedView.x
        + stretchedView.y * stretchedView.y;
    float3 tangent1 = projectedLengthSquared > 0.0
        ? float3(-stretchedView.y, stretchedView.x, 0.0)
            * rsqrt(projectedLengthSquared)
        : float3(1.0, 0.0, 0.0);
    float3 tangent2 = cross(stretchedView, tangent1);

    float radius = sqrt(sample.x);
    float angle = TwoPi * sample.y;
    float diskX = radius * cos(angle);
    float diskY = radius * sin(angle);
    float interpolation = 0.5 * (1.0 + stretchedView.z);
    diskY = (1.0 - interpolation) * sqrt(max(0.0, 1.0 - diskX * diskX))
        + interpolation * diskY;

    float3 stretchedNormal = diskX * tangent1
        + diskY * tangent2
        + sqrt(max(0.0, 1.0 - diskX * diskX - diskY * diskY)) * stretchedView;
    float3 localHalf = normalize(float3(
        alpha * stretchedNormal.x,
        alpha * stretchedNormal.y,
        max(0.0, stretchedNormal.z)));
    return normalize(
        basis.tangent * localHalf.x
        + basis.bitangent * localHalf.y
        + normal * localHalf.z);
}

float3 sampleBsdf(
    uint materialClass,
    float3 baseColor,
    float perceptualRoughness,
    float dielectricF0,
    float3 normal,
    float3 wo,
    float lobeSample,
    float2 directionSample)
{
    float alpha = max(perceptualRoughness * perceptualRoughness, 1.0e-4);
    if (materialClass == MaterialClassMetal)
    {
        float3 halfVector = sampleGgxVisibleNormal(
            normal,
            wo,
            alpha,
            directionSample);
        return reflect(-wo, halfVector);
    }

    float2 probabilities = brdfLobeProbabilities(baseColor, dielectricF0);
    if (probabilities.x + probabilities.y <= 0.0)
        return float3(0.0);
    if (lobeSample < probabilities.y)
    {
        float3 halfVector = sampleGgxVisibleNormal(
            normal,
            wo,
            alpha,
            directionSample);
        return reflect(-wo, halfVector);
    }
    return cosineSampleHemisphere(normal, directionSample);
}

float ggxReflectionPdf(
    float3 normal,
    float3 wo,
    float3 wi,
    float alpha)
{
    float normalDotOut = dot(normal, wo);
    float3 halfVectorSum = wo + wi;
    float halfLengthSquared = dot(halfVectorSum, halfVectorSum);
    if (normalDotOut <= 0.0 || halfLengthSquared <= 1.0e-12)
        return 0.0;
    float3 halfVector = halfVectorSum * rsqrt(halfLengthSquared);
    if (dot(wo, halfVector) <= 0.0)
        return 0.0;
    return ggxDistribution(saturate(dot(normal, halfVector)), alpha)
        * ggxSmithG1(normalDotOut, alpha)
        / max(4.0 * normalDotOut, 1.0e-8);
}

float dielectricBrdfPdf(
    float3 albedo,
    float perceptualRoughness,
    float dielectricF0,
    float3 normal,
    float3 wo,
    float3 wi)
{
    float normalDotIn = dot(normal, wi);
    if (normalDotIn <= 0.0)
        return 0.0;
    float2 probabilities = brdfLobeProbabilities(albedo, dielectricF0);
    float diffusePdf = normalDotIn * InversePi;
    float alpha = max(perceptualRoughness * perceptualRoughness, 1.0e-4);
    float specularPdf = ggxReflectionPdf(normal, wo, wi, alpha);
    return probabilities.x * diffusePdf + probabilities.y * specularPdf;
}

float bsdfPdf(
    uint materialClass,
    float3 baseColor,
    float perceptualRoughness,
    float dielectricF0,
    float3 normal,
    float3 wo,
    float3 wi)
{
    if (materialClass == MaterialClassMetal)
    {
        if (dot(normal, wi) <= 0.0)
            return 0.0;
        float alpha = max(perceptualRoughness * perceptualRoughness, 1.0e-4);
        return ggxReflectionPdf(normal, wo, wi, alpha);
    }
    return dielectricBrdfPdf(
        baseColor,
        perceptualRoughness,
        dielectricF0,
        normal,
        wo,
        wi);
}
