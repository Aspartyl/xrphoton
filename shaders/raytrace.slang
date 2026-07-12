// All three ray tracing stages live in this one module — Slang compiles every
// [shader(...)] entry point into a single SPIR-V module, so the pipeline creates
// one VkShaderModule and selects stages by entry-point name.

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
};

struct HitAttributes
{
    float3 worldNormal;
    float2 uv;
    uint materialIndex;
};

HitAttributes fetchHitAttributes(BuiltInTriangleIntersectionAttributes attribs)
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
    result.materialIndex = geometry.materialIndex;
    return result;
}

[shader("raygeneration")]
void rayGenMain()
{
    // The CPU sends a camera origin plus a pre-scaled perspective basis: forward is
    // unit length, while right/up already include FOV and aspect scaling.
    uint2 pixel = DispatchRaysIndex().xy;
    float2 ndc = (float2(pixel) + 0.5) / float2(DispatchRaysDimensions().xy) * 2.0 - 1.0;

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

    // The geometry was built OPAQUE; forcing opaque lets traversal skip any-hit
    // consideration entirely. Mask 0xFF accepts every instance; the offsets and
    // miss index are all zero with a single hit group and single miss shader.
    TraceRay(topLevelAS, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ray, payload);

    outputImage[pixel] = float4(payload.color, 1.0);
}

[shader("miss")]
void missMain(inout Payload payload)
{
    // Dark red keeps miss pixels visually distinct from the UV-gradient quad.
    payload.color = float3(0.24, 0.02, 0.015);
}

[shader("closesthit")]
void closestHitMain(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    HitAttributes hit = fetchHitAttributes(attribs);
    MaterialRecord material = materials[hit.materialIndex];
    // A smooth red/green UV gradient is an unambiguous BDA/stride/layout oracle.
    // The normalized-normal factor stays 1 for valid data while ensuring the normal
    // fetch and transform remain live in this probe shader.
    float normalLength = saturate(dot(hit.worldNormal, hit.worldNormal));
    payload.color = float3(hit.uv, 0.0) * material.baseColorFactor.rgb * normalLength;
}
