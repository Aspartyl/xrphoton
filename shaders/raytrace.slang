// All four ray tracing stages live in this one module — Slang compiles every
// [shader(...)] entry point into a single SPIR-V module, so the pipeline creates
// one VkShaderModule and selects stages by entry-point name.

#if XRPHOTON_RAY_TYPE_COUNT != 1
#error "The current shader module defines exactly one radiance ray type"
#endif

// Bindings match the descriptor set layout in rt_pipeline: the TLAS at 0, the
// swapchain-sized storage image at 1. The [format] attribute stamps the SPIR-V
// image format, required for storage-image writes without the
// shaderStorageImageWriteWithoutFormat feature.
[[vk::binding(0, 0)]]
uniform RaytracingAccelerationStructure topLevelAS;

[[vk::binding(1, 0)]]
[format("rgba8")]
uniform RWTexture2D<float4> outputImage;

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
    uint pad0;
    uint pad1;
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

[[vk::push_constant]]
ConstantBuffer<CameraPushConstants> camera;

struct Payload
{
    float3 color;
    // Full neighboring primary-ray directions provide explicit screen-space
    // derivatives in hit shaders, where implicit fragment derivatives do not
    // exist. SampleGrad uses the resulting UV footprint for real anisotropy.
    float3 rayDirectionX;
    float3 rayDirectionY;
};

struct HitAttributes
{
    float3 worldNormal;
    float2 uv;
    float2 uvDx;
    float2 uvDy;
    uint materialIndex;
};

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
    Payload payload)
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
    result.worldNormal = normalize(
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
    float2 ndc = (float2(pixel) + 0.5) / dimensions * 2.0 - 1.0;
    float2 ndcStep = 2.0 / dimensions;

    // Launch IDs count rows downward from the top of the image, so ndc.y is still
    // negated here to keep world y-up: that flip belongs to dispatch space, not the
    // camera basis the CPU sends.
    RayDesc ray;
    ray.Origin = camera.origin;
    ray.Direction = normalize(camera.forward + ndc.x * camera.right - ndc.y * camera.up);
    // tMin keeps self-intersection conventions even though nothing here can
    // self-intersect; tMax comfortably covers the unit-scale scene.
    ray.TMin = 0.001;
    ray.TMax = 100.0;

    Payload payload;
    payload.color = float3(0.0);
    payload.rayDirectionX = normalize(
        camera.forward + (ndc.x + ndcStep.x) * camera.right - ndc.y * camera.up);
    payload.rayDirectionY = normalize(
        camera.forward + ndc.x * camera.right - (ndc.y + ndcStep.y) * camera.up);

    // Opaque BLAS ranges bypass any-hit through their geometry flag; alpha-tested
    // ranges must remain eligible for their SBT-selected any-hit shader. Hit records
    // are interleaved by ray type per geometry; this module currently traces type 0.
    TraceRay(
        topLevelAS,
        RAY_FLAG_NONE,
        0xFF,
        0,
        XRPHOTON_RAY_TYPE_COUNT,
        0,
        ray,
        payload);

    outputImage[pixel] = float4(payload.color, 1.0);
}

[shader("miss")]
void missMain(inout Payload payload)
{
    // Dark red keeps miss pixels visually distinct from gallery materials.
    payload.color = float3(0.24, 0.02, 0.015);
}

[shader("closesthit")]
void closestHitMain(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    HitAttributes hit = fetchHitAttributes(attribs, payload);
    MaterialRecord material = materials[hit.materialIndex];
    float3 baseColor = material.baseColorFactor.rgb
        * sceneTextures[NonUniformResourceIndex(material.baseColorTexture)]
              .SampleGrad(hit.uv, hit.uvDx, hit.uvDy).rgb;
    payload.color = baseColor
        * (0.2 + 0.8 * abs(dot(hit.worldNormal, -WorldRayDirection())));
}

[shader("anyhit")]
void anyHitMain(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    HitAttributes hit = fetchHitAttributes(attribs, payload);
    MaterialRecord material = materials[hit.materialIndex];
    float alpha = material.baseColorFactor.a
        * sceneTextures[NonUniformResourceIndex(material.baseColorTexture)]
              .SampleGrad(hit.uv, hit.uvDx, hit.uvDy).a;
    if (alpha < material.alphaCutoff)
        IgnoreHit();
}
