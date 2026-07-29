// Shared GPU records and ray payloads. Keep these layouts synchronized with
// scene.hpp, gpu_scene.hpp, and rt_pipeline.hpp.

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
