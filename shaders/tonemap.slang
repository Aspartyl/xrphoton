// Fixed-exposure HDR-to-LDR output transform. The two resources remain storage images
// so this pass needs neither a sampler nor any filtering policy.
[[vk::binding(0, 0)]]
[format("rgba16f")]
uniform RWTexture2D<float4> hdrRadianceImage;

[[vk::binding(1, 0)]]
[format("rgba8")]
uniform RWTexture2D<float4> ldrOutputImage;

struct TonemapState
{
    float exposure;
};

[[vk::push_constant]]
ConstantBuffer<TonemapState> tonemapState;

[shader("compute")]
[numthreads(8, 8, 1)]
void tonemapMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    hdrRadianceImage.GetDimensions(width, height);
    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height)
        return;

    float3 radiance = max(hdrRadianceImage[dispatchThreadId.xy].rgb, 0.0);
    float3 exposed = radiance * tonemapState.exposure;
    float3 mapped = exposed / (1.0 + exposed);
    ldrOutputImage[dispatchThreadId.xy] = float4(mapped, 1.0);
}
