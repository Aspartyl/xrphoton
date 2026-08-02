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
    uint flags;
};

struct MaterialRecord
{
    float4 baseColorFactor;
    uint baseColorTexture;
    float alphaCutoff;
    float perceptualRoughness;
    float dielectricF0;
    float3 emission;
    uint materialClass;
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
    uint frameIndex;
    float cameraJitterX;
    float cameraJitterY;
    uint samplesPerPixel;
};

// Exact 192-byte std140 record mirrored by scene_lighting.hpp. The explicit scalar
// in every float3 lane pins offsets across the C++ and Slang ABIs.
struct FrameLighting
{
    float3 sunDirection;
    float sunCosineHalfAngle;
    float3 sunIrradiance;
    float sunSolidAngle;
    float3 skyZenith;
    float pSky;
    float3 skyHorizon;
    float pEmitters;
    uint lightCount;
    uint instanceCount;
    float totalLightPower;
    uint flags;
    float4 skyPerezA;
    float4 skyPerezB;
    float4 skyPerezC;
    float4 skyPerezD;
    float4 skyPerezE;
    float4 nightZenith;
    float daylightBlend;
    float emitterScale;
    uint reserved1;
    uint reserved2;
};

struct LightRecord
{
    float3 v0;
    float area;
    float3 edge1;
    float pTriangle;
    float3 edge2;
    uint flags;
    float3 emission;
    uint materialIndex;
};

struct EmitterLookupRecord
{
    uint first;
    uint count;
    uint reserved0;
    uint reserved1;
};

struct SurfaceHit
{
    float hitT;
    uint materialIndex;
    float3 albedo;
    float3 shadingNormal;
    float3 geometricNormal;
    float3 emission;
    float emitterArea;
    float emitterPTriangle;
    // Full neighboring primary-ray directions provide explicit screen-space
    // derivatives in hit shaders, where implicit fragment derivatives do not
    // exist. SampleGrad uses the resulting UV footprint for real anisotropy.
    float3 rayDirectionX;
    float3 rayDirectionY;
};

struct ShadowPayload
{
    float visibility;
    float3 attenuation;
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
