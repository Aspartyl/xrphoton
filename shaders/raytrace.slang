// All six ray tracing stages live in this one module — Slang compiles every
// [shader(...)] entry point into a single SPIR-V module, so the pipeline creates
// one VkShaderModule and selects stages by entry-point name.

#if XRPHOTON_RAY_TYPE_COUNT != 2
#error "The shader module defines exactly radiance and shadow ray types"
#endif
#if XRPHOTON_RADIANCE_RAY_TYPE != 0
#error "The radiance ray type must remain SBT index 0"
#endif
#if XRPHOTON_SHADOW_RAY_TYPE != 1
#error "The shadow ray type must remain SBT index 1"
#endif

static const uint RadianceRayType = XRPHOTON_RADIANCE_RAY_TYPE;
static const uint ShadowRayType = XRPHOTON_SHADOW_RAY_TYPE;
static const float SceneTMax = 100.0;
static const float ShadowTMax = 1.0e30;
static const float SurfaceRayOffset = 1.0e-3;
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

float3 skyRadiance(float3 direction)
{
    static const float3 HorizonRadiance = float3(0.32, 0.38, 0.46);
    static const float3 ZenithRadiance = float3(0.055, 0.12, 0.28);
    return lerp(HorizonRadiance, ZenithRadiance, saturate(direction.y));
}

// Bindings match the descriptor set layout in rt_pipeline: the TLAS at 0, the
// swapchain-sized HDR radiance image at 1. The [format] attribute stamps the SPIR-V
// image format, required for storage-image writes without the
// shaderStorageImageWriteWithoutFormat feature.
[[vk::binding(0, 0)]]
uniform RaytracingAccelerationStructure topLevelAS;

[[vk::binding(1, 0)]]
[format("rgba16f")]
uniform RWTexture2D<float4> hdrRadianceImage;

// All-scalar or explicitly aligned records mirror scene.hpp/gpu_scene.hpp exactly.
struct VertexAttributes
{
    float nx, ny, nz;
    float u, v;
};

struct GeometryRecord
{
    uint64_t indexAddress;
    uint64_t positionAddress;
    uint64_t attributeAddress;
    uint materialIndex;
    uint pad0;
};

struct MaterialRecord
{
    float4 baseColorFactor;
    uint baseColorTexture;
    float alphaCutoff;
    float perceptualRoughness;
    float dielectricF0;
};

[[vk::binding(2, 0)]]
StructuredBuffer<GeometryRecord> geometryRecords;

[[vk::binding(3, 0)]]
StructuredBuffer<MaterialRecord> materials;

// Must match MaxSceneTextures in gpu_scene.hpp. Every slot is written; unused slots
// repeat the opaque-white fallback, so no partially-bound descriptor feature is used.
[[vk::binding(4, 0)]]
Sampler2D sceneTextures[1024];

struct CameraPushConstants
{
    float3 origin;
    float3 forward;
    float3 right;
    float3 up;
};

struct RaygenPushConstants
{
    CameraPushConstants camera;
    float3 sunDirection;
    float pad0;
    float3 sunRadiance;
    uint frameIndex;
};

[[vk::push_constant]]
ConstantBuffer<RaygenPushConstants> frameConstants;

struct SurfaceHit
{
    float hitT;
    uint materialIndex;
    float3 albedo;
    float3 shadingNormal;
    float3 geometricNormal;
    // Full neighboring primary-ray directions provide explicit screen-space
    // derivatives in hit shaders, where implicit fragment derivatives do not
    // exist. SampleGrad uses the resulting UV footprint for real anisotropy.
    float3 rayDirectionX;
    float3 rayDirectionY;
};

struct ShadowPayload
{
    float visibility;
};

struct HitMaterialCoordinates
{
    float2 uv;
    uint materialIndex;
};

struct HitAttributes
{
    float3 worldShadingNormal;
    float3 worldGeometricNormal;
    float2 uv;
    float2 uvDx;
    float2 uvDy;
    uint materialIndex;
};

HitMaterialCoordinates fetchHitMaterialCoordinates(
    BuiltInTriangleIntersectionAttributes attribs)
{
    GeometryRecord geometry = geometryRecords[InstanceID() + GeometryIndex()];
    uint* indices = (uint*)geometry.indexAddress;
    uint baseIndex = 3 * PrimitiveIndex();
    uint3 triangle = uint3(
        indices[baseIndex],
        indices[baseIndex + 1],
        indices[baseIndex + 2]);
    VertexAttributes* attributes = (VertexAttributes*)geometry.attributeAddress;
    float3 weights = float3(
        1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y);

    HitMaterialCoordinates result;
    result.uv = float2(
        attributes[triangle.x].u,
        attributes[triangle.x].v) * weights.x
        + float2(
            attributes[triangle.y].u,
            attributes[triangle.y].v) * weights.y
        + float2(
            attributes[triangle.z].u,
            attributes[triangle.z].v) * weights.z;
    result.materialIndex = geometry.materialIndex;
    return result;
}

bool rejectsAlpha(MaterialRecord material, float sampledAlpha)
{
    return material.baseColorFactor.a * sampledAlpha < material.alphaCutoff;
}

float2 uvGradient(
    float3 worldDelta,
    float3 positionEdge1,
    float3 positionEdge2,
    float2 uvEdge1,
    float2 uvEdge2)
{
    float d00 = dot(positionEdge1, positionEdge1);
    float d01 = dot(positionEdge1, positionEdge2);
    float d11 = dot(positionEdge2, positionEdge2);
    float determinant = d00 * d11 - d01 * d01;
    if (abs(determinant) < 1.0e-20)
        return float2(0.0);

    float r0 = dot(worldDelta, positionEdge1);
    float r1 = dot(worldDelta, positionEdge2);
    float edgeWeight1 = (d11 * r0 - d01 * r1) / determinant;
    float edgeWeight2 = (d00 * r1 - d01 * r0) / determinant;
    return uvEdge1 * edgeWeight1 + uvEdge2 * edgeWeight2;
}

HitAttributes fetchHitAttributes(
    BuiltInTriangleIntersectionAttributes attribs,
    SurfaceHit payload)
{
    GeometryRecord geometry = geometryRecords[InstanceID() + GeometryIndex()];
    uint* indices = (uint*)geometry.indexAddress;
    uint baseIndex = 3 * PrimitiveIndex();
    uint3 triangle = uint3(
        indices[baseIndex],
        indices[baseIndex + 1],
        indices[baseIndex + 2]);

    VertexAttributes* attributes = (VertexAttributes*)geometry.attributeAddress;
    float3 weights = float3(
        1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
        attribs.barycentrics.x,
        attribs.barycentrics.y);

    VertexAttributes a = attributes[triangle.x];
    VertexAttributes b = attributes[triangle.y];
    VertexAttributes c = attributes[triangle.z];
    float3 objectNormal = float3(a.nx, a.ny, a.nz) * weights.x
        + float3(b.nx, b.ny, b.nz) * weights.y
        + float3(c.nx, c.ny, c.nz) * weights.z;

    HitAttributes result;
    // With HLSL row-vector multiplication, WorldToObject's 3x3 block applies the
    // required object-to-world inverse-transpose normal transform.
    result.worldShadingNormal = normalize(
        mul(objectNormal, (float3x3)WorldToObject3x4()));
    result.uv = float2(a.u, a.v) * weights.x
        + float2(b.u, b.v) * weights.y
        + float2(c.u, c.v) * weights.z;

    // Intersect the two neighboring primary rays with this triangle's world-space
    // plane, then map their hit-point deltas through the triangle's position/UV
    // basis. maxLod remains zero in the sampler, so these gradients drive only
    // anisotropic filtering within mip 0.
    float* positions = (float*)geometry.positionAddress;
    float3 objectA = float3(
        positions[triangle.x * 3],
        positions[triangle.x * 3 + 1],
        positions[triangle.x * 3 + 2]);
    float3 objectB = float3(
        positions[triangle.y * 3],
        positions[triangle.y * 3 + 1],
        positions[triangle.y * 3 + 2]);
    float3 objectC = float3(
        positions[triangle.z * 3],
        positions[triangle.z * 3 + 1],
        positions[triangle.z * 3 + 2]);
    float3 worldA = mul(ObjectToWorld3x4(), float4(objectA, 1.0));
    float3 worldB = mul(ObjectToWorld3x4(), float4(objectB, 1.0));
    float3 worldC = mul(ObjectToWorld3x4(), float4(objectC, 1.0));
    float3 positionEdge1 = worldB - worldA;
    float3 positionEdge2 = worldC - worldA;
    result.worldGeometricNormal = normalize(
        cross(positionEdge1, positionEdge2));
    float2 uvEdge1 = float2(b.u - a.u, b.v - a.v);
    float2 uvEdge2 = float2(c.u - a.u, c.v - a.v);

    float3 origin = WorldRayOrigin();
    float3 hitPosition = origin + WorldRayDirection() * RayTCurrent();
    float3 planeNormal = cross(positionEdge1, positionEdge2);
    float planeNumerator = dot(planeNormal, hitPosition - origin);
    float denominatorX = dot(planeNormal, payload.rayDirectionX);
    float denominatorY = dot(planeNormal, payload.rayDirectionY);
    result.uvDx = float2(0.0);
    result.uvDy = float2(0.0);
    if (abs(denominatorX) >= 1.0e-20)
    {
        float3 neighborHitX = origin + payload.rayDirectionX
            * (planeNumerator / denominatorX);
        result.uvDx = uvGradient(
            neighborHitX - hitPosition,
            positionEdge1,
            positionEdge2,
            uvEdge1,
            uvEdge2);
    }
    if (abs(denominatorY) >= 1.0e-20)
    {
        float3 neighborHitY = origin + payload.rayDirectionY
            * (planeNumerator / denominatorY);
        result.uvDy = uvGradient(
            neighborHitY - hitPosition,
            positionEdge1,
            positionEdge2,
            uvEdge1,
            uvEdge2);
    }
    result.materialIndex = geometry.materialIndex;
    return result;
}

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

float fresnelSchlick(float cosine, float f0)
{
    float oneMinusCosine = 1.0 - saturate(cosine);
    float factor = oneMinusCosine * oneMinusCosine;
    factor *= factor * oneMinusCosine;
    return f0 + (1.0 - f0) * factor;
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

[shader("raygeneration")]
void rayGenMain()
{
    // The CPU sends a camera origin plus a pre-scaled perspective basis: forward is
    // unit length, while right/up already include FOV and aspect scaling.
    uint2 pixel = DispatchRaysIndex().xy;
    float2 dimensions = float2(DispatchRaysDimensions().xy);
    float2 ndc = (float2(pixel) + 0.5) / dimensions * 2.0 - 1.0;
    float2 ndcStep = 2.0 / dimensions;

    // Launch IDs count rows downward from the top of the image, so ndc.y is still
    // negated here to keep world y-up: that flip belongs to dispatch space, not the
    // camera basis the CPU sends.
    RayDesc ray;
    ray.Origin = frameConstants.camera.origin;
    ray.Direction = normalize(
        frameConstants.camera.forward
        + ndc.x * frameConstants.camera.right
        - ndc.y * frameConstants.camera.up);
    // tMin keeps self-intersection conventions even though nothing here can
    // self-intersect; SceneTMax comfortably covers the unit-scale scene.
    ray.TMin = 0.001;
    ray.TMax = SceneTMax;

    float3 rayDirectionX = normalize(
        frameConstants.camera.forward
        + (ndc.x + ndcStep.x) * frameConstants.camera.right
        - ndc.y * frameConstants.camera.up);
    float3 rayDirectionY = normalize(
        frameConstants.camera.forward
        + ndc.x * frameConstants.camera.right
        - (ndc.y + ndcStep.y) * frameConstants.camera.up);

    static const uint MaxPathVertices = 8;
    float3 radiance = float3(0.0);
    float3 throughput = float3(1.0);
    uint rngState = pcgHash(
        pixel.x + pcgHash(pixel.y + pcgHash(frameConstants.frameIndex)));
    for (uint pathVertex = 0; pathVertex < MaxPathVertices; ++pathVertex)
    {
        SurfaceHit hit;
        hit.hitT = -1.0;
        hit.materialIndex = 0;
        hit.albedo = float3(0.0);
        hit.shadingNormal = float3(0.0);
        hit.geometricNormal = float3(0.0);
        hit.rayDirectionX = rayDirectionX;
        hit.rayDirectionY = rayDirectionY;

        // Opaque BLAS ranges bypass any-hit through their geometry flag;
        // alpha-tested ranges remain eligible for their SBT-selected any-hit shader.
        TraceRay(
            topLevelAS,
            RAY_FLAG_NONE,
            0xFF,
            RadianceRayType,
            XRPHOTON_RAY_TYPE_COUNT,
            RadianceRayType,
            ray,
            hit);

        if (hit.hitT < 0.0)
        {
            radiance += throughput * hit.albedo;
            break;
        }

        float3 worldPosition = ray.Origin + ray.Direction * hit.hitT;
        OrientedSurfaceNormals normals = orientSurfaceNormals(
            hit.shadingNormal,
            hit.geometricNormal,
            -ray.Direction);
        MaterialRecord material = materials[hit.materialIndex];
        float3 wo = -ray.Direction;
        float shadingCosine = dot(normals.shading, frameConstants.sunDirection);
        float geometricCosine = dot(normals.geometric, frameConstants.sunDirection);
        if (shadingCosine > 0.0 && geometricCosine > 0.0)
        {
            ShadowPayload shadow;
            shadow.visibility = 0.0;

            RayDesc shadowRay;
            shadowRay.Origin = worldPosition
                + normals.geometric * SurfaceRayOffset;
            shadowRay.Direction = frameConstants.sunDirection;
            shadowRay.TMin = 0.0;
            shadowRay.TMax = ShadowTMax;

            TraceRay(
                topLevelAS,
                RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
                    | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER,
                0xFF,
                ShadowRayType,
                XRPHOTON_RAY_TYPE_COUNT,
                ShadowRayType,
                shadowRay,
                shadow);

            float3 sunBrdf = evaluateDielectricBrdf(
                hit.albedo,
                material.perceptualRoughness,
                material.dielectricF0,
                normals.shading,
                wo,
                frameConstants.sunDirection);
            radiance += throughput
                * sunBrdf
                * frameConstants.sunRadiance
                * shadingCosine
                * shadow.visibility;
        }

        if (pathVertex + 1 >= MaxPathVertices)
            break;

        // Select one lobe by its normalized energy estimate, then evaluate the full
        // diffuse+specular mixture and matching mixture PDF. Directions rejected by
        // the actual triangle are terminated rather than resampled, which would bias
        // the distribution selected around the smooth shading normal.
        float2 lobeProbabilities = brdfLobeProbabilities(
            hit.albedo,
            material.dielectricF0);
        if (lobeProbabilities.x + lobeProbabilities.y <= 0.0)
            break;
        float lobeSample = rngNextFloat(rngState);
        float2 directionSample = float2(
            rngNextFloat(rngState),
            rngNextFloat(rngState));
        float3 bounceDirection;
        if (lobeSample < lobeProbabilities.y)
        {
            float alpha = max(
                material.perceptualRoughness * material.perceptualRoughness,
                1.0e-4);
            float3 halfVector = sampleGgxVisibleNormal(
                normals.shading,
                wo,
                alpha,
                directionSample);
            bounceDirection = reflect(-wo, halfVector);
        }
        else
        {
            bounceDirection = cosineSampleHemisphere(
                normals.shading,
                directionSample);
        }
        if (dot(normals.geometric, bounceDirection) <= 0.0)
            break;

        float bounceCosine = dot(normals.shading, bounceDirection);
        float bouncePdf = dielectricBrdfPdf(
            hit.albedo,
            material.perceptualRoughness,
            material.dielectricF0,
            normals.shading,
            wo,
            bounceDirection);
        if (bounceCosine <= 0.0 || bouncePdf <= 0.0)
            break;
        float3 bounceBrdf = evaluateDielectricBrdf(
            hit.albedo,
            material.perceptualRoughness,
            material.dielectricF0,
            normals.shading,
            wo,
            bounceDirection);
        throughput *= bounceBrdf * (bounceCosine / bouncePdf);

        float maximumThroughput = max(throughput.x, max(throughput.y, throughput.z));
        if (maximumThroughput <= 0.0)
            break;
        if (pathVertex >= 3)
        {
            float survivalProbability = clamp(maximumThroughput, 0.05, 0.95);
            if (rngNextFloat(rngState) >= survivalProbability)
                break;
            throughput /= survivalProbability;
        }

        ray.Origin = worldPosition + normals.geometric * SurfaceRayOffset;
        ray.Direction = bounceDirection;
        ray.TMin = 0.001;
        ray.TMax = SceneTMax;
        // Mip 0 is the only resident mip today. Matching neighboring directions
        // makes the secondary footprint explicitly zero until ray differentials
        // are promoted alongside a real mip chain.
        rayDirectionX = bounceDirection;
        rayDirectionY = bounceDirection;
    }

    hdrRadianceImage[pixel] = float4(max(radiance, 0.0), 1.0);
}

[shader("miss")]
void missMain(inout SurfaceHit payload)
{
    payload.hitT = -1.0;
    payload.albedo = skyRadiance(WorldRayDirection());
}

[shader("closesthit")]
void closestHitMain(
    inout SurfaceHit payload,
    BuiltInTriangleIntersectionAttributes attribs)
{
    HitAttributes hit = fetchHitAttributes(attribs, payload);
    MaterialRecord material = materials[hit.materialIndex];
    payload.hitT = RayTCurrent();
    payload.materialIndex = hit.materialIndex;
    payload.albedo = material.baseColorFactor.rgb
        * sceneTextures[NonUniformResourceIndex(material.baseColorTexture)]
              .SampleGrad(hit.uv, hit.uvDx, hit.uvDy).rgb;
    payload.shadingNormal = hit.worldShadingNormal;
    payload.geometricNormal = hit.worldGeometricNormal;
}

[shader("anyhit")]
void anyHitMain(
    inout SurfaceHit payload,
    BuiltInTriangleIntersectionAttributes attribs)
{
    HitAttributes hit = fetchHitAttributes(attribs, payload);
    MaterialRecord material = materials[hit.materialIndex];
    float sampledAlpha =
        sceneTextures[NonUniformResourceIndex(material.baseColorTexture)]
            .SampleGrad(hit.uv, hit.uvDx, hit.uvDy).a;
    if (rejectsAlpha(material, sampledAlpha))
        IgnoreHit();
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload)
{
    payload.visibility = 1.0;
}

[shader("anyhit")]
void shadowAnyHitMain(
    inout ShadowPayload payload,
    BuiltInTriangleIntersectionAttributes attribs)
{
    HitMaterialCoordinates hit = fetchHitMaterialCoordinates(attribs);
    MaterialRecord material = materials[hit.materialIndex];
    float sampledAlpha =
        sceneTextures[NonUniformResourceIndex(material.baseColorTexture)]
            .SampleLevel(hit.uv, 0).a;
    if (rejectsAlpha(material, sampledAlpha))
        IgnoreHit();
}
