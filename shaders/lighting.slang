// Analytic light evaluation lives here; later phases add light sampling and PDFs
// without expanding the ray-tracing entry-point module.

float3 skyRadiance(float3 direction)
{
    static const float3 HorizonRadiance = float3(0.32, 0.38, 0.46);
    static const float3 ZenithRadiance = float3(0.055, 0.12, 0.28);
    return lerp(HorizonRadiance, ZenithRadiance, saturate(direction.y));
}
