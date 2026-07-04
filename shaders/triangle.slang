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

struct Payload
{
    float3 color;
};

[shader("raygeneration")]
void rayGenMain()
{
    // Orthographic camera hardcoded for the bring-up triangle: each pixel's ray
    // starts at its NDC position behind the triangle plane (z = 0) and travels +Z.
    // No camera abstraction or push constants yet — that comes with a real scene.
    uint2 pixel = DispatchRaysIndex().xy;
    float2 ndc = (float2(pixel) + 0.5) / float2(DispatchRaysDimensions().xy) * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin = float3(ndc, -1.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
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
    // The same dark red the placeholder clear used, so the first traced frame
    // reads as "the triangle appeared", not "the background changed".
    payload.color = float3(0.24, 0.02, 0.015);
}

[shader("closesthit")]
void closestHitMain(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    // Barycentric-derived color: a gradient across the triangle proves attribute
    // interpolation works, where a flat color would only prove the hit fired.
    // The attributes are the weights of vertices 1 and 2; vertex 0's weight is
    // the remainder.
    float2 bary = attribs.barycentrics;
    payload.color = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
}
