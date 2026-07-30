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

[shader("raygeneration")]
void rayGenMain()
{
    // The CPU sends a camera origin plus a pre-scaled perspective basis: forward is
    // unit length, while right/up already include FOV and aspect scaling.
    uint2 pixel = DispatchRaysIndex().xy;
    float2 dimensions = float2(DispatchRaysDimensions().xy);
    float2 cameraJitter = float2(
        frameConstants.cameraJitterX,
        frameConstants.cameraJitterY);
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

    static const uint MaxPathVertices = 8;
    float3 radiance = float3(0.0);
    float3 throughput = float3(1.0);
    float previousBsdfPdf = 0.0;
    float previousSkyPdf = 0.0;
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
            float missWeight = pathVertex == 0 || estimator == EstimatorBsdf
                ? 1.0
                : estimator == EstimatorNee
                    ? 0.0
                    : powerHeuristic(previousBsdfPdf, previousSkyPdf);
            radiance += throughput
                * skyRadiance(frameLighting, ray.Direction)
                * missWeight;
            break;
        }

        float3 worldPosition = ray.Origin + ray.Direction * hit.hitT;
        OrientedSurfaceNormals normals = orientSurfaceNormals(
            hit.shadingNormal,
            hit.geometricNormal,
            -ray.Direction);
        MaterialRecord material = materials[hit.materialIndex];
        float3 wo = -ray.Direction;
        uint estimator = estimatorMode();
        float maximumEmission = max(
            hit.emission.x,
            max(hit.emission.y, hit.emission.z));
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
            radiance += throughput * hit.emission * emissionWeight;
        }
        float shadingCosine = dot(normals.shading, frameLighting.sunDirection);
        float geometricCosine = dot(normals.geometric, frameLighting.sunDirection);
        float maximumSunIrradiance = max(
            frameLighting.sunIrradiance.x,
            max(frameLighting.sunIrradiance.y, frameLighting.sunIrradiance.z));
        if (maximumSunIrradiance > 0.0
            && shadingCosine > 0.0
            && geometricCosine > 0.0)
        {
            ShadowPayload shadow;
            shadow.visibility = 0.0;

            RayDesc shadowRay;
            shadowRay.Origin = worldPosition
                + normals.geometric * SurfaceRayOffset;
            shadowRay.Direction = frameLighting.sunDirection;
            shadowRay.TMin = 0.0;
            shadowRay.TMax = InfiniteRayTMax;

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
                frameLighting.sunDirection);
            radiance += throughput
                * sunBrdf
                * frameLighting.sunIrradiance
                * shadingCosine
                * shadow.visibility;
        }

        // Draw exactly one non-delta branch. The delta sun stays separate and cannot
        // perturb the sky/emitter probabilities or enter their MIS balances.
        float selectableProbability = frameLighting.pSky + frameLighting.pEmitters;
        // A light connection adds the source endpoint to the path. Do not add it at
        // the terminal surface where the matching BSDF technique has no remaining
        // segment; all estimator modes must integrate the same finite path set.
        if (pathVertex + 1 < MaxPathVertices
            && estimator != EstimatorBsdf
            && selectableProbability > 0.0)
        {
            float selectorSample = rngNextFloat(rngState);
            if (selectorSample < frameLighting.pSky)
            {
                SkySample lightSample = sampleSky(
                    frameLighting,
                    normals.shading,
                    float2(rngNextFloat(rngState), rngNextFloat(rngState)));
                float lightGeometricCosine = dot(
                    normals.geometric,
                    lightSample.direction);
                float lightPdf = frameLighting.pSky * lightSample.pdf;
                if (lightGeometricCosine > 0.0 && lightPdf > 0.0)
                {
                    ShadowPayload shadow;
                    shadow.visibility = 0.0;

                    RayDesc shadowRay;
                    shadowRay.Origin = worldPosition
                        + normals.geometric * SurfaceRayOffset;
                    shadowRay.Direction = lightSample.direction;
                    shadowRay.TMin = 0.0;
                    shadowRay.TMax = InfiniteRayTMax;

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

                    float lightBsdfPdf = dielectricBrdfPdf(
                        hit.albedo,
                        material.perceptualRoughness,
                        material.dielectricF0,
                        normals.shading,
                        wo,
                        lightSample.direction);
                    float3 lightBrdf = evaluateDielectricBrdf(
                        hit.albedo,
                        material.perceptualRoughness,
                        material.dielectricF0,
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
                                dot(normals.shading, lightSample.direction),
                                lightBsdfPdf));
                    radiance += throughput
                        * lightBrdf
                        * lightSample.radiance
                        * dot(normals.shading, lightSample.direction)
                        * shadow.visibility
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
                    float3 shadowOrigin = worldPosition
                        + normals.geometric * SurfaceRayOffset;
                    float3 toLight = lightPosition - shadowOrigin;
                    float distanceSquared = dot(toLight, toLight);
                    float distance = sqrt(distanceSquared);
                    if (distance > SurfaceRayOffset)
                    {
                        float3 lightDirection = toLight / distance;
                        float shadingLightCosine = dot(
                            normals.shading,
                            lightDirection);
                        float geometricLightCosine = dot(
                            normals.geometric,
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
                        if (shadingLightCosine > 0.0
                            && geometricLightCosine > 0.0
                            && lightPdf > 0.0
                            && shadowTMax > 0.0)
                        {
                            ShadowPayload shadow;
                            shadow.visibility = 0.0;

                            RayDesc shadowRay;
                            shadowRay.Origin = shadowOrigin;
                            shadowRay.Direction = lightDirection;
                            shadowRay.TMin = 0.0;
                            shadowRay.TMax = shadowTMax;

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

                            float lightBsdfPdf = dielectricBrdfPdf(
                                hit.albedo,
                                material.perceptualRoughness,
                                material.dielectricF0,
                                normals.shading,
                                wo,
                                lightDirection);
                            float3 lightBrdf = evaluateDielectricBrdf(
                                hit.albedo,
                                material.perceptualRoughness,
                                material.dielectricF0,
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
                                * shadingLightCosine
                                * shadow.visibility
                                * (neeWeight / lightPdf);
                        }
                    }
                }
            }
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
        previousBsdfPdf = bouncePdf;
        previousSkyPdf = frameLighting.pSky
            * skyPdf(normals.shading, bounceDirection);

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

        ray.Origin = worldPosition + normals.geometric * SurfaceRayOffset;
        ray.Direction = bounceDirection;
        ray.TMin = 0.001;
        ray.TMax = InfiniteRayTMax;
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
