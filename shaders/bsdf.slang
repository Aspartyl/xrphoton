import sampling;

static const uint MaterialClassDielectric = 0;
static const uint MaterialClassMetal = 1;
static const uint MaterialClassGlass = 2;
static const float GlassIor = 1.5;

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

// Exact dielectric Fresnel is shared by the rough Glass BSDF and the named
// straight-line shadow approximation. etaI/etaT describe the side selected by the
// unflipped geometric normal; total internal reflection therefore falls out here.
float fresnelDielectric(float cosineI, float etaI, float etaT)
{
    cosineI = saturate(abs(cosineI));
    float eta = etaI / etaT;
    float sineSquaredT = eta * eta * max(0.0, 1.0 - cosineI * cosineI);
    if (sineSquaredT >= 1.0)
        return 1.0;
    float cosineT = sqrt(max(0.0, 1.0 - sineSquaredT));
    float parallel = (etaT * cosineI - etaI * cosineT)
        / max(etaT * cosineI + etaI * cosineT, 1.0e-8);
    float perpendicular = (etaI * cosineI - etaT * cosineT)
        / max(etaI * cosineI + etaT * cosineT, 1.0e-8);
    return 0.5 * (parallel * parallel + perpendicular * perpendicular);
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

float3 evaluateGlassBsdf(
    float3 transmissionTint,
    float perceptualRoughness,
    float etaI,
    float etaT,
    float3 normal,
    float3 wo,
    float3 wi)
{
    float normalDotOut = dot(normal, wo);
    float normalDotIn = dot(normal, wi);
    if (normalDotOut <= 0.0 || abs(normalDotIn) <= 1.0e-8)
        return float3(0.0);

    float alpha = max(perceptualRoughness * perceptualRoughness, 1.0e-4);
    if (normalDotIn > 0.0)
    {
        float3 halfVectorSum = wo + wi;
        float halfLengthSquared = dot(halfVectorSum, halfVectorSum);
        if (halfLengthSquared <= 1.0e-12)
            return float3(0.0);
        float3 halfVector = halfVectorSum * rsqrt(halfLengthSquared);
        float outDotHalf = dot(wo, halfVector);
        if (outDotHalf <= 0.0)
            return float3(0.0);
        float fresnel = fresnelDielectric(outDotHalf, etaI, etaT);
        float distribution = ggxDistribution(saturate(dot(normal, halfVector)), alpha);
        float masking = ggxSmithG1(normalDotOut, alpha)
            * ggxSmithG1(normalDotIn, alpha);
        return float3(fresnel * distribution * masking
            / max(4.0 * normalDotOut * normalDotIn, 1.0e-8));
    }

    // Walter's rough dielectric transmission half-vector. eta is etaT/etaI;
    // the final 1/eta^2 converts importance transport to radiance transport.
    float eta = etaT / etaI;
    float3 halfVectorSum = wo + eta * wi;
    float halfLengthSquared = dot(halfVectorSum, halfVectorSum);
    if (halfLengthSquared <= 1.0e-12)
        return float3(0.0);
    float3 halfVector = halfVectorSum * rsqrt(halfLengthSquared);
    if (dot(normal, halfVector) < 0.0)
        halfVector = -halfVector;
    float outDotHalf = dot(wo, halfVector);
    float inDotHalf = dot(wi, halfVector);
    if (outDotHalf <= 0.0 || inDotHalf >= 0.0)
        return float3(0.0);
    float fresnel = fresnelDielectric(outDotHalf, etaI, etaT);
    float distribution = ggxDistribution(saturate(dot(normal, halfVector)), alpha);
    float masking = ggxSmithG1(normalDotOut, alpha)
        * ggxSmithG1(-normalDotIn, alpha);
    float denominator = inDotHalf + outDotHalf / eta;
    float scale = (1.0 - fresnel) * distribution * masking
        * abs(inDotHalf * outDotHalf)
        / max(
            abs(normalDotIn) * normalDotOut * denominator * denominator,
            1.0e-8);
    scale /= eta * eta;
    return transmissionTint * scale;
}

float glassReflectionTargetEnergy(float cosine, float etaI, float etaT)
{
    return fresnelDielectric(cosine, etaI, etaT);
}

float glassTransmissionTargetEnergy(float cosine, float etaI, float etaT)
{
    float eta = etaI / etaT;
    return (1.0 - fresnelDielectric(cosine, etaI, etaT)) * eta * eta;
}

float2 glassMissingEnergyComponents(
    float perceptualRoughness,
    float etaI,
    float etaT,
    float cosine)
{
    float roughness = saturate(perceptualRoughness);
    float roughnessSquared = roughness * roughness;
    float roughnessFourth = roughnessSquared * roughnessSquared;
    float reflectionMissing = 0.0;
    float transmissionMissing = 0.0;
    if (etaI < etaT)
    {
        reflectionMissing = 0.0367 * roughnessFourth;
        transmissionMissing = 0.0280 * roughnessFourth;
        reflectionMissing *= glassReflectionTargetEnergy(
                cosine, etaI, etaT)
            / 0.04;
        transmissionMissing *= glassTransmissionTargetEnergy(
                cosine, etaI, etaT)
            / 0.42666667;
    }
    else
    {
        transmissionMissing = max(
            0.0,
            0.00894375 - 0.198625 * roughness
                + 1.321875 * roughnessSquared);
        transmissionMissing *= glassTransmissionTargetEnergy(
                cosine, etaI, etaT)
            / 2.16;
    }
    return min(
        float2(reflectionMissing, transmissionMissing),
        float2(
            glassReflectionTargetEnergy(cosine, etaI, etaT),
            glassTransmissionTargetEnergy(cosine, etaI, etaT)));
}

float glassMultipleScatterClosure(float perceptualRoughness)
{
    // The separable Kulla-Conty return lobe overestimates a closed dielectric
    // because successive air/glass interfaces are correlated. This pinned closure
    // curve comes from a fixed-IOR VNDF slab integration and restores the
    // paired-interface equilibrium without changing the exact low-roughness lobe.
    float roughness = saturate(perceptualRoughness);
    if (roughness <= 0.1)
        return roughness * 0.7598;
    if (roughness <= 0.5)
        return lerp(0.07598, 0.3445, (roughness - 0.1) / 0.4);
    if (roughness <= 0.9)
        return lerp(0.3445, 0.93, (roughness - 0.5) / 0.4);
    return lerp(0.93, 1.0, (roughness - 0.9) / 0.1);
}

float3 opaqueMultipleScatterTint(
    uint materialClass,
    float3 baseColor,
    float averageMissing)
{
    if (materialClass != MaterialClassMetal)
        return baseColor;

    // Kulla-Conty's infinite-bounce Fresnel series. It becomes exactly one for a
    // white conductor, while colored conductors retain their spectral response.
    float3 averageFresnel = baseColor
        + (float3(1.0) - baseColor) * (1.0 / 21.0);
    float averageSingleScatter = 1.0 - averageMissing;
    return averageFresnel * averageFresnel * averageSingleScatter
        / max(
            float3(1.0)
                - averageFresnel * averageMissing,
            float3(1.0e-6));
}

float opaqueMissingEnergy(
    uint materialClass,
    float perceptualRoughness)
{
    float roughness = saturate(perceptualRoughness);
    float normalMissing = 0.0;
    if (materialClass == MaterialClassMetal)
    {
        normalMissing = roughness < 0.1
            ? roughness * 0.00582
            : max(
                0.0,
                0.04174966 - 0.53601188 * roughness
                    + 1.24335313 * roughness * roughness);
    }
    else
    {
        normalMissing = 0.08430553 - 0.02165688 * roughness
            + 0.04921563 * roughness * roughness;
    }
    // This compact fit is intentionally angle-independent: it preserves the fixed
    // roughness energy closure without putting a dynamic table lookup into every
    // ordinary yard BSDF evaluation.
    return saturate(normalMissing);
}

float3 evaluateOpaqueMultipleScatter(
    uint materialClass,
    float3 baseColor,
    float perceptualRoughness,
    float3 normal,
    float3 wo,
    float3 wi)
{
    float normalDotOut = dot(normal, wo);
    float normalDotIn = dot(normal, wi);
    if (normalDotOut <= 0.0 || normalDotIn <= 0.0)
        return float3(0.0);
    float missingOut = opaqueMissingEnergy(
        materialClass,
        perceptualRoughness);
    float averageMissing = materialClass == MaterialClassMetal
        ? opaqueMissingEnergy(materialClass, perceptualRoughness)
        : 0.0;
    // A cosine return lobe integrates directly to the fitted missing energy.
    // Keeping the fit angle-independent avoids adding a lookup to every ordinary
    // NEE/BSDF evaluation.
    return opaqueMultipleScatterTint(
            materialClass,
            baseColor,
            averageMissing)
        * (missingOut * InversePi);
}

float3 evaluateGlassMultipleScatter(
    float3 transmissionTint,
    float perceptualRoughness,
    float etaI,
    float etaT,
    float3 normal,
    float3 wo,
    float3 wi)
{
    float normalDotOut = dot(normal, wo);
    float normalDotIn = dot(normal, wi);
    if (normalDotOut <= 0.0 || abs(normalDotIn) <= 1.0e-8)
        return float3(0.0);

    bool transmission = normalDotIn < 0.0;
    float2 missing = glassMissingEnergyComponents(
        perceptualRoughness,
        etaI,
        etaT,
        normalDotOut);
    float missingOut = transmission ? missing.y : missing.x;
    return transmissionTint * glassMultipleScatterClosure(perceptualRoughness)
        * (missingOut * InversePi);
}

float2 glassMultipleScatterMissingEnergy(
    float perceptualRoughness,
    float etaI,
    float etaT,
    float3 normal,
    float3 wo)
{
    return glassMissingEnergyComponents(
        perceptualRoughness,
        etaI,
        etaT,
        max(dot(normal, wo), 0.0));
}

float glassMultipleScatterReflectionProbability(
    float perceptualRoughness,
    float etaI,
    float etaT,
    float3 normal,
    float3 wo)
{
    float2 missing = glassMultipleScatterMissingEnergy(
        perceptualRoughness,
        etaI,
        etaT,
        normal,
        wo);
    return missing.x / max(missing.x + missing.y, 1.0e-6);
}

float multipleScatterProbability(
    uint materialClass,
    float3 baseColor,
    float perceptualRoughness,
    float etaI,
    float etaT,
    float3 normal,
    float3 wo)
{
    float normalDotOut = max(dot(normal, wo), 0.0);
    if (materialClass == MaterialClassGlass)
    {
        float2 missingComponents = glassMultipleScatterMissingEnergy(
            perceptualRoughness,
            etaI,
            etaT,
            normal,
            wo);
        float missing = (missingComponents.x + missingComponents.y)
            * glassMultipleScatterClosure(perceptualRoughness);
        float target = glassReflectionTargetEnergy(
                normalDotOut, etaI, etaT)
            + glassTransmissionTargetEnergy(
                normalDotOut, etaI, etaT);
        return clamp(
            missing / max(target, 1.0e-6),
            0.0,
            0.95);
    }

    float missing = opaqueMissingEnergy(
        materialClass,
        perceptualRoughness);
    float3 tint = opaqueMultipleScatterTint(
        materialClass,
        baseColor,
        materialClass == MaterialClassMetal
            ? opaqueMissingEnergy(
                materialClass, perceptualRoughness)
            : 0.0);
    return clamp(
        missing * dot(tint, float3(0.2126, 0.7152, 0.0722)),
        0.0,
        0.95);
}

float3 evaluateBsdf(
    uint materialClass,
    float3 baseColor,
    float perceptualRoughness,
    float dielectricF0,
    float etaI,
    float etaT,
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
            wi)
            + evaluateOpaqueMultipleScatter(
                materialClass,
                baseColor,
                perceptualRoughness,
                normal,
                wo,
                wi);
    if (materialClass == MaterialClassGlass)
        return evaluateGlassBsdf(
            baseColor,
            perceptualRoughness,
            etaI,
            etaT,
            normal,
            wo,
            wi)
            + evaluateGlassMultipleScatter(
                baseColor,
                perceptualRoughness,
                etaI,
                etaT,
                normal,
                wo,
                wi);
    return evaluateDielectricBrdf(
        baseColor,
        perceptualRoughness,
        dielectricF0,
        normal,
        wo,
        wi)
        + evaluateOpaqueMultipleScatter(
            materialClass,
            baseColor,
            perceptualRoughness,
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
    float etaI,
    float etaT,
    float3 normal,
    float3 wo,
    float lobeSample,
    float2 directionSample)
{
    float multipleScatter = multipleScatterProbability(
        materialClass,
        baseColor,
        perceptualRoughness,
        etaI,
        etaT,
        normal,
        wo);
    if (lobeSample < multipleScatter)
    {
        if (materialClass == MaterialClassGlass)
        {
            float reflectionProbability =
                glassMultipleScatterReflectionProbability(
                    perceptualRoughness,
                    etaI,
                    etaT,
                    normal,
                    wo);
            bool opposite = directionSample.x >= reflectionProbability;
            float2 remappedSample = float2(
                opposite
                    ? (directionSample.x - reflectionProbability)
                        / max(1.0 - reflectionProbability, 1.0e-6)
                    : directionSample.x
                        / max(reflectionProbability, 1.0e-6),
                directionSample.y);
            return cosineSampleHemisphere(
                opposite ? -normal : normal,
                remappedSample);
        }
        return cosineSampleHemisphere(normal, directionSample);
    }
    float baseLobeSample = (lobeSample - multipleScatter)
        / max(1.0 - multipleScatter, 1.0e-6);

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

    if (materialClass == MaterialClassGlass)
    {
        float3 halfVector = sampleGgxVisibleNormal(
            normal,
            wo,
            alpha,
            directionSample);
        float fresnel = fresnelDielectric(dot(wo, halfVector), etaI, etaT);
        if (baseLobeSample < fresnel)
            return reflect(-wo, halfVector);
        return refract(-wo, halfVector, etaI / etaT);
    }

    float2 probabilities = brdfLobeProbabilities(baseColor, dielectricF0);
    if (probabilities.x + probabilities.y <= 0.0)
        return float3(0.0);
    if (baseLobeSample < probabilities.y)
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

float glassBsdfPdf(
    float perceptualRoughness,
    float etaI,
    float etaT,
    float3 normal,
    float3 wo,
    float3 wi)
{
    float normalDotOut = dot(normal, wo);
    float normalDotIn = dot(normal, wi);
    if (normalDotOut <= 0.0 || abs(normalDotIn) <= 1.0e-8)
        return 0.0;

    float eta = etaT / etaI;
    float3 halfVectorSum = normalDotIn > 0.0
        ? wo + wi
        : wo + eta * wi;
    float halfLengthSquared = dot(halfVectorSum, halfVectorSum);
    if (halfLengthSquared <= 1.0e-12)
        return 0.0;
    float3 halfVector = halfVectorSum * rsqrt(halfLengthSquared);
    if (dot(normal, halfVector) < 0.0)
        halfVector = -halfVector;
    float outDotHalf = dot(wo, halfVector);
    float inDotHalf = dot(wi, halfVector);
    if (outDotHalf <= 0.0)
        return 0.0;

    float alpha = max(perceptualRoughness * perceptualRoughness, 1.0e-4);
    float visibleNormalPdf = ggxDistribution(
        saturate(dot(normal, halfVector)), alpha)
        * ggxSmithG1(normalDotOut, alpha)
        * abs(outDotHalf)
        / max(normalDotOut, 1.0e-8);
    float fresnel = fresnelDielectric(outDotHalf, etaI, etaT);
    if (normalDotIn > 0.0)
        return fresnel * visibleNormalPdf
            / max(4.0 * abs(outDotHalf), 1.0e-8);
    if (inDotHalf >= 0.0)
        return 0.0;
    float denominator = inDotHalf + outDotHalf / eta;
    float halfVectorJacobian = abs(inDotHalf)
        / max(denominator * denominator, 1.0e-8);
    return (1.0 - fresnel) * visibleNormalPdf * halfVectorJacobian;
}

float baseBsdfPdf(
    uint materialClass,
    float3 baseColor,
    float perceptualRoughness,
    float dielectricF0,
    float etaI,
    float etaT,
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
    if (materialClass == MaterialClassGlass)
        return glassBsdfPdf(
            perceptualRoughness,
            etaI,
            etaT,
            normal,
            wo,
            wi);
    return dielectricBrdfPdf(
        baseColor,
        perceptualRoughness,
        dielectricF0,
        normal,
        wo,
        wi);
}

float bsdfPdf(
    uint materialClass,
    float3 baseColor,
    float perceptualRoughness,
    float dielectricF0,
    float etaI,
    float etaT,
    float3 normal,
    float3 wo,
    float3 wi)
{
    float multipleScatter = multipleScatterProbability(
        materialClass,
        baseColor,
        perceptualRoughness,
        etaI,
        etaT,
        normal,
        wo);
    float normalDotIn = dot(normal, wi);
    float multipleScatterPdf = max(normalDotIn, 0.0) * InversePi;
    if (materialClass == MaterialClassGlass)
    {
        float reflectionProbability =
            glassMultipleScatterReflectionProbability(
                perceptualRoughness,
                etaI,
                etaT,
                normal,
                wo);
        multipleScatterPdf = abs(normalDotIn) * InversePi
            * (normalDotIn > 0.0
                ? reflectionProbability
                : 1.0 - reflectionProbability);
    }
    return (1.0 - multipleScatter) * baseBsdfPdf(
            materialClass,
            baseColor,
            perceptualRoughness,
            dielectricF0,
            etaI,
            etaT,
            normal,
            wo,
            wi)
        + multipleScatter * multipleScatterPdf;
}
