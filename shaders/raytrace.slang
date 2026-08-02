// All six ray tracing stages compile into one SPIR-V module. Imported source
// modules keep records, sampling, BSDFs, and lighting independently maintainable.
import records;
import sampling;
import bsdf;
import lighting;

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
// Radiance rays that may reach the environment and infinite visibility rays must
// share one horizon or their MIS techniques estimate different integrals.
static const float InfiniteRayTMax = 1.0e30;
static const float SurfaceRayOffset = 1.0e-3;
static const uint NoEmitterLight = 0xffffffff;
static const uint EstimatorShift = 2;
static const uint EstimatorMask = 3u << EstimatorShift;
static const uint EstimatorMis = 0;
static const uint EstimatorNee = 1;
static const uint EstimatorBsdf = 2;
static const uint GeometryFlagAlphaTested = 1u << 0;

uint shadowTraceFlags()
{
    // Glass candidates use IgnoreHit after tinting the payload, so shadow traversal
    // must remain available for every interface along the light segment.
    return RAY_FLAG_SKIP_CLOSEST_HIT_SHADER;
}

float surfaceCosine(bool twoSided, float3 normal, float3 direction)
{
    float cosine = dot(normal, direction);
    return twoSided ? abs(cosine) : cosine;
}

bool validSurfaceDirection(
    bool twoSided,
    OrientedSurfaceNormals normals,
    float3 direction)
{
    float shadingCosine = dot(normals.shading, direction);
    float geometricCosine = dot(normals.geometric, direction);
    if (twoSided)
        return shadingCosine * geometricCosine > 0.0;
    return shadingCosine > 0.0 && geometricCosine > 0.0;
}

float3 offsetSurfaceRay(
    float3 worldPosition,
    float3 geometricNormal,
    float3 direction,
    bool twoSided)
{
    if (twoSided && dot(geometricNormal, direction) < 0.0)
        return worldPosition - geometricNormal * SurfaceRayOffset;
    return worldPosition + geometricNormal * SurfaceRayOffset;
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

[[vk::binding(2, 0)]]
StructuredBuffer<GeometryRecord> geometryRecords;

[[vk::binding(3, 0)]]
StructuredBuffer<MaterialRecord> materials;

// Must match MaxSceneTextures in gpu_scene.hpp. Every slot is written; unused slots
// repeat the opaque-white fallback, so no partially-bound descriptor feature is used.
[[vk::binding(4, 0)]]
Sampler2D sceneTextures[1024];

[[vk::binding(5, 0)]]
ConstantBuffer<FrameLighting> frameLighting;

// P2b publishes these immutable tables. P2c consumes them for emitter NEE and
// BSDF-hit emission; keeping them live now makes the descriptor ABI land atomically.
[[vk::binding(6, 0)]]
StructuredBuffer<LightRecord> lightRecords;

[[vk::binding(7, 0)]]
StructuredBuffer<float> lightCdf;

[[vk::binding(8, 0)]]
StructuredBuffer<EmitterLookupRecord> emitterLookup;

// Primary-hit G-buffer, written once per pixel from the first sample's camera ray
// (denoising plan D0). A primary miss writes the depth sentinel 0 and
// GBufferMissInstanceId so filtering passes can bypass sky pixels.
[[vk::binding(9, 0)]]
[format("rgba16f")]
uniform RWTexture2D<float4> gbufferNormalDepthImage;

[[vk::binding(10, 0)]]
[format("rgba8")]
uniform RWTexture2D<float4> gbufferAlbedoImage;

[[vk::binding(11, 0)]]
[format("r32ui")]
uniform RWTexture2D<uint> gbufferInstanceIdImage;

static const uint GBufferMissInstanceId = 0xffffffff;

// Primary-surface attributes exported from the first path vertex. Zero-initialized
// fields are exactly the miss sentinel.
struct PrimarySurface
{
    float3 shadingNormal;
    float viewDepth;
    float3 albedo;
    uint instanceIndex;
};

[[vk::push_constant]]
ConstantBuffer<RaygenPushConstants> frameConstants;

uint estimatorMode()
{
    return (frameLighting.flags & EstimatorMask) >> EstimatorShift;
}

// At later vertices, surviving Russian roulette is part of the BSDF technique's
// path density. NEE must use that same density or the two MIS weights do not sum.
float bsdfTechniquePdfAfterRoulette(
    uint pathVertex,
    float3 pathThroughput,
    float3 brdf,
    float cosine,
    float pdf)
{
    if (pdf <= 0.0)
        return 0.0;
    if (pathVertex < 3)
        return pdf;

    float3 candidateThroughput = pathThroughput * brdf * (cosine / pdf);
    float maximumThroughput = max(
        candidateThroughput.x,
        max(candidateThroughput.y, candidateThroughput.z));
    return maximumThroughput > 0.0
        ? pdf * clamp(maximumThroughput, 0.05, 0.95)
        : 0.0;
}

uint selectEmitter(float sample)
{
    uint first = 0;
    uint count = frameLighting.lightCount;
    while (count > 0)
    {
        uint step = count / 2;
        uint middle = first + step;
        if (sample < lightCdf[middle])
            count = step;
        else
        {
            first = middle + 1;
            count -= step + 1;
        }
    }
    return first;
}

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

// Only Glass shadow hits need orientation. Keeping this separate preserves the
// compact alpha-test path used by no-glass scenes.
float3 fetchHitWorldGeometricNormal(GeometryRecord geometry)
{
    uint* indices = (uint*)geometry.indexAddress;
    uint baseIndex = 3 * PrimitiveIndex();
    uint3 triangle = uint3(
        indices[baseIndex],
        indices[baseIndex + 1],
        indices[baseIndex + 2]);
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
    return normalize(cross(worldB - worldA, worldC - worldA));
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

uint cameraJitterCell(uint sequenceIndex)
{
    // Mirrored by CameraJitterCellPermutation in lighting.hpp. Keeping the compact
    // known-answer schedule here lets every in-frame path sample occupy a distinct
    // 4x4 cell while 1 SPP remains byte-for-byte identical.
    switch (sequenceIndex & 15u)
    {
    case 0u: return 3u;
    case 1u: return 9u;
    case 2u: return 8u;
    case 3u: return 0u;
    case 4u: return 7u;
    case 5u: return 2u;
    case 6u: return 4u;
    case 7u: return 12u;
    case 8u: return 15u;
    case 9u: return 5u;
    case 10u: return 1u;
    case 11u: return 10u;
    case 12u: return 13u;
    case 13u: return 14u;
    case 14u: return 11u;
    default: return 6u;
    }
}

float2 cameraJitterForSequence(uint sequenceIndex)
{
    uint cell = cameraJitterCell(sequenceIndex);
    // Keep samples in distinct stratification cells, but do not align every extra
    // primary ray to the same 4x4 center lattice. The separate frame-global scramble
    // is deterministic and does not consume the per-pixel path stream.
    uint jitterState = pcgHash(sequenceIndex ^ 0x4a495454u);
    float2 withinCell = float2(
        rngNextFloat(jitterState),
        rngNextFloat(jitterState));
    return (float2(cell & 3u, cell >> 2u) + withinCell) * 0.25 - 0.5;
}

float3 traceCameraPath(
    uint2 pixel,
    float2 dimensions,
    uint sampleSequence,
    float2 cameraJitter,
    out PrimarySurface primarySurface)
{
    primarySurface.shadingNormal = float3(0.0);
    primarySurface.viewDepth = 0.0;
    primarySurface.albedo = float3(0.0);
    primarySurface.instanceIndex = GBufferMissInstanceId;

    // The CPU sends a camera origin plus a pre-scaled perspective basis: forward is
    // unit length, while right/up already include FOV and aspect scaling.
    float2 ndc = (float2(pixel) + 0.5 + cameraJitter)
        / dimensions * 2.0 - 1.0;
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
    // self-intersect. The infinite horizon matches sky visibility rays.
    ray.TMin = 0.001;
    ray.TMax = InfiniteRayTMax;

    float3 rayDirectionX = normalize(
        frameConstants.camera.forward
        + (ndc.x + ndcStep.x) * frameConstants.camera.right
        - ndc.y * frameConstants.camera.up);
    float3 rayDirectionY = normalize(
        frameConstants.camera.forward
        + ndc.x * frameConstants.camera.right
        - (ndc.y + ndcStep.y) * frameConstants.camera.up);

    // Closed rough Glass may need more than eight interface events before it reaches
    // the furnace environment. Production skies keep the established cap; the
    // full-sphere acceptance flag raises only the offline proof's truncation ceiling.
    uint maxPathVertices = (frameLighting.flags & (1u << 4u)) != 0
        ? 32u
        : 8u;
    float3 radiance = float3(0.0);
    float3 throughput = float3(1.0);
    float previousBsdfPdf = 0.0;
    float previousSkyPdf = 0.0;
    uint shadowRayFlags = shadowTraceFlags();
    uint rngState = pcgHash(
        pixel.x + pcgHash(pixel.y + pcgHash(sampleSequence)));
    for (uint pathVertex = 0; pathVertex < maxPathVertices; ++pathVertex)
    {
        SurfaceHit hit;
        hit.hitT = -1.0;
        hit.materialIndex = 0;
        hit.instanceIndex = GBufferMissInstanceId;
        hit.albedo = float3(0.0);
        hit.shadingNormal = float3(0.0);
        hit.geometricNormal = float3(0.0);
        hit.emission = float3(0.0);
        hit.emitterArea = 0.0;
        hit.emitterPTriangle = 0.0;
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
            uint estimator = estimatorMode();
            float skyMissWeight = pathVertex == 0 || estimator == EstimatorBsdf
                ? 1.0
                : estimator == EstimatorNee
                    ? 0.0
                    : powerHeuristic(previousBsdfPdf, previousSkyPdf);
            radiance += throughput
                * skyRadiance(frameLighting, ray.Direction)
                * skyMissWeight;

            // The finite solar disc is a separate source layered over the analytic
            // sky. A delta sun has zero BSDF-hit measure and therefore contributes
            // only through its legacy direct estimator below.
            float3 missedSunRadiance = sunRadiance(
                frameLighting,
                ray.Direction);
            float maximumMissedSun = max(
                missedSunRadiance.x,
                max(missedSunRadiance.y, missedSunRadiance.z));
            if (maximumMissedSun > 0.0)
            {
                float sunMissWeight = pathVertex == 0
                    || estimator == EstimatorBsdf
                    ? 1.0
                    : estimator == EstimatorNee
                        ? 0.0
                        : powerHeuristic(
                            previousBsdfPdf,
                            sunPdf(frameLighting, ray.Direction));
                radiance += throughput
                    * missedSunRadiance
                    * sunMissWeight;
            }
            break;
        }

        float3 worldPosition = ray.Origin + ray.Direction * hit.hitT;
        OrientedSurfaceNormals normals = orientSurfaceNormals(
            hit.shadingNormal,
            hit.geometricNormal,
            -ray.Direction);
        if (pathVertex == 0)
        {
            // Linear view depth: the hit distance projected onto the unit forward
            // axis, so the value is jitter-stable across a flat surface.
            primarySurface.shadingNormal = normals.shading;
            primarySurface.viewDepth = hit.hitT
                * dot(ray.Direction, frameConstants.camera.forward);
            primarySurface.albedo = hit.albedo;
            primarySurface.instanceIndex = hit.instanceIndex;
        }
        MaterialRecord material = materials[hit.materialIndex];
        float3 wo = -ray.Direction;
        bool isGlass = material.materialClass == MaterialClassGlass;
        float etaI = 1.0;
        float etaT = 1.0;
        if (isGlass)
        {
            bool enteringGlass = dot(hit.geometricNormal, wo) > 0.0;
            etaI = enteringGlass ? 1.0 : GlassIor;
            etaT = enteringGlass ? GlassIor : 1.0;
        }
        uint estimator = estimatorMode();
        float3 hitEmission = hit.emission * frameLighting.emitterScale;
        float maximumEmission = max(
            hitEmission.x,
            max(hitEmission.y, hitEmission.z));
        if (maximumEmission > 0.0)
        {
            float emissionWeight = 1.0;
            if (pathVertex > 0)
            {
                if (estimator == EstimatorNee)
                    emissionWeight = 0.0;
                else if (estimator == EstimatorMis)
                {
                    float lightCosine = dot(
                        hit.geometricNormal,
                        -ray.Direction);
                    float lightPdf = lightCosine > 0.0
                        && hit.emitterArea > 0.0
                        ? frameLighting.pEmitters
                            * hit.emitterPTriangle
                            * (hit.hitT * hit.hitT)
                            / (hit.emitterArea * lightCosine)
                        : 0.0;
                    emissionWeight = powerHeuristic(previousBsdfPdf, lightPdf);
                }
            }
            radiance += throughput * hitEmission * emissionWeight;
        }
        float maximumSunIrradiance = max(
            frameLighting.sunIrradiance.x,
            max(frameLighting.sunIrradiance.y, frameLighting.sunIrradiance.z));
        bool deltaSun = frameLighting.sunSolidAngle <= 0.0;
        bool sampleSunAtVertex = deltaSun
            || (pathVertex + 1 < maxPathVertices
                && estimator != EstimatorBsdf);
        if (maximumSunIrradiance > 0.0 && sampleSunAtVertex)
        {
            // Do not consume path-stream draws on the zero-angle compatibility path.
            float2 sunRandom = deltaSun
                ? float2(0.0)
                : float2(rngNextFloat(rngState), rngNextFloat(rngState));
            SunSample lightSample = sampleSun(frameLighting, sunRandom);
            float shadingCosine = surfaceCosine(
                isGlass,
                normals.shading,
                lightSample.direction);
            if (lightSample.pdf > 0.0
                && validSurfaceDirection(
                    isGlass,
                    normals,
                    lightSample.direction))
            {
                ShadowPayload shadow;
                shadow.visibility = 0.0;
                shadow.attenuation = float3(1.0);

                RayDesc shadowRay;
                shadowRay.Origin = offsetSurfaceRay(
                    worldPosition,
                    normals.geometric,
                    lightSample.direction,
                    isGlass);
                shadowRay.Direction = lightSample.direction;
                shadowRay.TMin = 0.0;
                shadowRay.TMax = InfiniteRayTMax;

                TraceRay(
                    topLevelAS,
                    shadowRayFlags,
                    0xFF,
                    ShadowRayType,
                    XRPHOTON_RAY_TYPE_COUNT,
                    ShadowRayType,
                    shadowRay,
                    shadow);

                float sunBsdfPdf = bsdfPdf(
                    material.materialClass,
                    hit.albedo,
                    material.perceptualRoughness,
                    material.dielectricF0,
                    etaI,
                    etaT,
                    normals.shading,
                    wo,
                    lightSample.direction);
                float3 sunBrdf = evaluateBsdf(
                    material.materialClass,
                    hit.albedo,
                    material.perceptualRoughness,
                    material.dielectricF0,
                    etaI,
                    etaT,
                    normals.shading,
                    wo,
                    lightSample.direction);
                float neeWeight = (lightSample.delta
                    || estimator == EstimatorNee)
                    ? 1.0
                    : powerHeuristic(
                        lightSample.pdf,
                        bsdfTechniquePdfAfterRoulette(
                            pathVertex,
                            throughput,
                            sunBrdf,
                            shadingCosine,
                            sunBsdfPdf));
                radiance += throughput
                    * sunBrdf
                    * lightSample.radiance
                    * shadingCosine
                    * shadow.visibility
                    * shadow.attenuation
                    * (neeWeight / lightSample.pdf);
            }
        }

        // Draw exactly one sky/emitter branch. The separately sampled sun cannot
        // perturb these probabilities or enter their selector MIS balances.
        float selectableProbability = frameLighting.pSky + frameLighting.pEmitters;
        // A light connection adds the source endpoint to the path. Do not add it at
        // the terminal surface where the matching BSDF technique has no remaining
        // segment; all estimator modes must integrate the same finite path set.
        if (pathVertex + 1 < maxPathVertices
            && estimator != EstimatorBsdf
            && selectableProbability > 0.0)
        {
            float selectorSample = rngNextFloat(rngState);
            if (selectorSample < frameLighting.pSky)
            {
                SkySample lightSample = sampleSky(
                    frameLighting,
                    normals.shading,
                    isGlass,
                    float2(rngNextFloat(rngState), rngNextFloat(rngState)));
                float lightPdf = frameLighting.pSky * lightSample.pdf;
                if (validSurfaceDirection(
                        isGlass,
                        normals,
                        lightSample.direction)
                    && lightPdf > 0.0)
                {
                    ShadowPayload shadow;
                    shadow.visibility = 0.0;
                    shadow.attenuation = float3(1.0);

                    RayDesc shadowRay;
                    shadowRay.Origin = offsetSurfaceRay(
                        worldPosition,
                        normals.geometric,
                        lightSample.direction,
                        isGlass);
                    shadowRay.Direction = lightSample.direction;
                    shadowRay.TMin = 0.0;
                    shadowRay.TMax = InfiniteRayTMax;

                    TraceRay(
                        topLevelAS,
                        shadowRayFlags,
                        0xFF,
                        ShadowRayType,
                        XRPHOTON_RAY_TYPE_COUNT,
                        ShadowRayType,
                        shadowRay,
                        shadow);

                    float lightBsdfPdf = bsdfPdf(
                        material.materialClass,
                        hit.albedo,
                        material.perceptualRoughness,
                        material.dielectricF0,
                        etaI,
                        etaT,
                        normals.shading,
                        wo,
                        lightSample.direction);
                    float3 lightBrdf = evaluateBsdf(
                        material.materialClass,
                        hit.albedo,
                        material.perceptualRoughness,
                        material.dielectricF0,
                        etaI,
                        etaT,
                        normals.shading,
                        wo,
                        lightSample.direction);
                    float neeWeight = estimator == EstimatorNee
                        ? 1.0
                        : powerHeuristic(
                            lightPdf,
                            bsdfTechniquePdfAfterRoulette(
                                pathVertex,
                                throughput,
                                lightBrdf,
                                surfaceCosine(
                                    isGlass,
                                    normals.shading,
                                    lightSample.direction),
                                lightBsdfPdf));
                    radiance += throughput
                        * lightBrdf
                        * lightSample.radiance
                        * surfaceCosine(
                            isGlass,
                            normals.shading,
                            lightSample.direction)
                        * shadow.visibility
                        * shadow.attenuation
                        * (neeWeight / lightPdf);
                }
            }
            else if (frameLighting.lightCount > 0
                && selectorSample < selectableProbability)
            {
                uint lightIndex = selectEmitter(rngNextFloat(rngState));
                if (lightIndex < frameLighting.lightCount)
                {
                    LightRecord light = lightRecords[lightIndex];
                    float sqrtSample = sqrt(rngNextFloat(rngState));
                    float triangleSample = rngNextFloat(rngState);
                    float3 lightPosition = light.v0
                        + light.edge1 * (sqrtSample * (1.0 - triangleSample))
                        + light.edge2 * (sqrtSample * triangleSample);
                    float3 lightNormal = normalize(cross(light.edge1, light.edge2));
                    float3 shadowOrigin = worldPosition;
                    float3 toLight = lightPosition - shadowOrigin;
                    float distanceSquared = dot(toLight, toLight);
                    float distance = sqrt(distanceSquared);
                    if (distance > SurfaceRayOffset)
                    {
                        float3 lightDirection = toLight / distance;
                        shadowOrigin = offsetSurfaceRay(
                            worldPosition,
                            normals.geometric,
                            lightDirection,
                            isGlass);
                        toLight = lightPosition - shadowOrigin;
                        distanceSquared = dot(toLight, toLight);
                        distance = sqrt(distanceSquared);
                        lightDirection = toLight / distance;
                        float shadingLightCosine = surfaceCosine(
                            isGlass,
                            normals.shading,
                            lightDirection);
                        float emitterCosine = dot(lightNormal, -lightDirection);
                        float lightPdf = emitterCosine > 0.0 && light.area > 0.0
                            ? frameLighting.pEmitters
                                * light.pTriangle
                                * distanceSquared
                                / (light.area * emitterCosine)
                            : 0.0;
                        float endpointExclusion = max(
                            SurfaceRayOffset,
                            1.0e-4 * distance);
                        float shadowTMax = distance - endpointExclusion;
                        if (validSurfaceDirection(
                                isGlass,
                                normals,
                                lightDirection)
                            && lightPdf > 0.0
                            && shadowTMax > 0.0)
                        {
                            ShadowPayload shadow;
                            shadow.visibility = 0.0;
                            shadow.attenuation = float3(1.0);

                            RayDesc shadowRay;
                            shadowRay.Origin = shadowOrigin;
                            shadowRay.Direction = lightDirection;
                            shadowRay.TMin = 0.0;
                            shadowRay.TMax = shadowTMax;

                            TraceRay(
                                topLevelAS,
                                shadowRayFlags,
                                0xFF,
                                ShadowRayType,
                                XRPHOTON_RAY_TYPE_COUNT,
                                ShadowRayType,
                                shadowRay,
                                shadow);

                            float lightBsdfPdf = bsdfPdf(
                                material.materialClass,
                                hit.albedo,
                                material.perceptualRoughness,
                                material.dielectricF0,
                                etaI,
                                etaT,
                                normals.shading,
                                wo,
                                lightDirection);
                            float3 lightBrdf = evaluateBsdf(
                                material.materialClass,
                                hit.albedo,
                                material.perceptualRoughness,
                                material.dielectricF0,
                                etaI,
                                etaT,
                                normals.shading,
                                wo,
                                lightDirection);
                            float neeWeight = estimator == EstimatorNee
                                ? 1.0
                                : powerHeuristic(
                                    lightPdf,
                                    bsdfTechniquePdfAfterRoulette(
                                        pathVertex,
                                        throughput,
                                        lightBrdf,
                                        shadingLightCosine,
                                        lightBsdfPdf));
                            radiance += throughput
                                * lightBrdf
                                * light.emission
                                * frameLighting.emitterScale
                                * shadingLightCosine
                                * shadow.visibility
                                * shadow.attenuation
                                * (neeWeight / lightPdf);
                        }
                    }
                }
            }
        }

        if (pathVertex + 1 >= maxPathVertices)
            break;

        // Draw through the class-aware sampler, then evaluate the matching complete
        // BSDF and PDF. Directions rejected by the actual triangle are terminated
        // rather than resampled, which would bias the smooth-normal distribution.
        float lobeSample = rngNextFloat(rngState);
        float2 directionSample = float2(
            rngNextFloat(rngState),
            rngNextFloat(rngState));
        float3 bounceDirection = sampleBsdf(
            material.materialClass,
            hit.albedo,
            material.perceptualRoughness,
            material.dielectricF0,
            etaI,
            etaT,
            normals.shading,
            wo,
            lobeSample,
            directionSample);
        if (!validSurfaceDirection(
                isGlass,
                normals,
                bounceDirection))
            break;

        float bounceCosine = surfaceCosine(
            isGlass,
            normals.shading,
            bounceDirection);
        float bouncePdf = bsdfPdf(
            material.materialClass,
            hit.albedo,
            material.perceptualRoughness,
            material.dielectricF0,
            etaI,
            etaT,
            normals.shading,
            wo,
            bounceDirection);
        if (bounceCosine <= 0.0 || bouncePdf <= 0.0)
            break;
        float3 bounceBrdf = evaluateBsdf(
            material.materialClass,
            hit.albedo,
            material.perceptualRoughness,
            material.dielectricF0,
            etaI,
            etaT,
            normals.shading,
            wo,
            bounceDirection);
        throughput *= bounceBrdf * (bounceCosine / bouncePdf);
        previousBsdfPdf = bouncePdf;
        previousSkyPdf = frameLighting.pSky
            * skyPdf(normals.shading, bounceDirection, isGlass);

        float maximumThroughput = max(throughput.x, max(throughput.y, throughput.z));
        if (maximumThroughput <= 0.0)
            break;
        if (pathVertex >= 3)
        {
            float survivalProbability = clamp(maximumThroughput, 0.05, 0.95);
            if (rngNextFloat(rngState) >= survivalProbability)
                break;
            throughput /= survivalProbability;
            previousBsdfPdf *= survivalProbability;
        }

        ray.Origin = offsetSurfaceRay(
            worldPosition,
            normals.geometric,
            bounceDirection,
            isGlass);
        ray.Direction = bounceDirection;
        ray.TMin = 0.001;
        ray.TMax = InfiniteRayTMax;
        // Mip 0 is the only resident mip today. Matching neighboring directions
        // makes the secondary footprint explicitly zero until ray differentials
        // are promoted alongside a real mip chain.
        rayDirectionX = bounceDirection;
        rayDirectionY = bounceDirection;
    }

    return radiance;
}

[shader("raygeneration")]
void rayGenMain()
{
    uint2 pixel = DispatchRaysIndex().xy;
    float2 dimensions = float2(DispatchRaysDimensions().xy);
    uint samplesPerPixel = clamp(frameConstants.samplesPerPixel, 1u, 16u);
    float3 accumulatedRadiance = float3(0.0);
    for (uint sampleIndex = 0; sampleIndex < samplesPerPixel; ++sampleIndex)
    {
        uint sampleSequence =
            frameConstants.frameIndex * samplesPerPixel + sampleIndex;
        float2 cameraJitter = sampleIndex == 0
            ? float2(frameConstants.cameraJitterX, frameConstants.cameraJitterY)
            : cameraJitterForSequence(sampleSequence);
        PrimarySurface primarySurface;
        accumulatedRadiance += traceCameraPath(
            pixel,
            dimensions,
            sampleSequence,
            cameraJitter,
            primarySurface);
        if (sampleIndex == 0)
        {
            // The G-buffer describes the first sample's primary surface, whose
            // jitter is the frame-pinned pair in the push constants.
            gbufferNormalDepthImage[pixel] = float4(
                primarySurface.shadingNormal,
                primarySurface.viewDepth);
            gbufferAlbedoImage[pixel] = float4(
                saturate(primarySurface.albedo),
                1.0);
            gbufferInstanceIdImage[pixel] = primarySurface.instanceIndex;
        }
    }

    hdrRadianceImage[pixel] = float4(
        max(accumulatedRadiance / float(samplesPerPixel), 0.0),
        1.0);
}

[shader("miss")]
void missMain(inout SurfaceHit payload)
{
    payload.hitT = -1.0;
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
    payload.instanceIndex = InstanceIndex();
    payload.albedo = material.baseColorFactor.rgb
        * sceneTextures[NonUniformResourceIndex(material.baseColorTexture)]
              .SampleGrad(hit.uv, hit.uvDx, hit.uvDy).rgb;
    payload.shadingNormal = hit.worldShadingNormal;
    payload.geometricNormal = hit.worldGeometricNormal;
    payload.emission = float3(0.0);
    payload.emitterArea = 0.0;
    payload.emitterPTriangle = 0.0;

    uint instanceIndex = InstanceIndex();
    if (instanceIndex < frameLighting.instanceCount)
    {
        EmitterLookupRecord header = emitterLookup[instanceIndex];
        uint geometryIndex = GeometryIndex();
        if (geometryIndex < header.count)
        {
            EmitterLookupRecord range = emitterLookup[header.first + geometryIndex];
            uint primitiveIndex = PrimitiveIndex();
            if (range.first != NoEmitterLight
                && primitiveIndex < range.count
                && range.first < frameLighting.lightCount
                && primitiveIndex < frameLighting.lightCount - range.first)
            {
                LightRecord light = lightRecords[range.first + primitiveIndex];
                if (dot(hit.worldGeometricNormal, -WorldRayDirection()) > 0.0)
                {
                    payload.emission = light.emission;
                    payload.emitterArea = light.area;
                    payload.emitterPTriangle = light.pTriangle;
                }
            }
        }
    }
}

[shader("anyhit")]
void anyHitMain(
    inout SurfaceHit payload,
    BuiltInTriangleIntersectionAttributes attribs)
{
    GeometryRecord geometry = geometryRecords[InstanceID() + GeometryIndex()];
    if ((geometry.flags & GeometryFlagAlphaTested) == 0)
        return;
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
    GeometryRecord geometry = geometryRecords[InstanceID() + GeometryIndex()];
    HitMaterialCoordinates hit = fetchHitMaterialCoordinates(attribs);
    MaterialRecord material = materials[hit.materialIndex];
    float4 sampledBaseColor =
        sceneTextures[NonUniformResourceIndex(material.baseColorTexture)]
            .SampleLevel(hit.uv, 0);
    if ((geometry.flags & GeometryFlagAlphaTested) != 0
        && rejectsAlpha(material, sampledBaseColor.a))
    {
        IgnoreHit();
        return;
    }
    if (material.materialClass == MaterialClassGlass)
    {
        float3 worldGeometricNormal = fetchHitWorldGeometricNormal(geometry);
        float3 wo = -WorldRayDirection();
        bool entering = dot(worldGeometricNormal, wo) > 0.0;
        float etaI = entering ? 1.0 : GlassIor;
        float etaT = entering ? GlassIor : 1.0;
        float fresnel = fresnelDielectric(
            abs(dot(worldGeometricNormal, wo)),
            etaI,
            etaT);
        float3 tint = saturate(
            material.baseColorFactor.rgb * sampledBaseColor.rgb);
        payload.attenuation *= tint * (1.0 - fresnel);
        IgnoreHit();
    }
}
