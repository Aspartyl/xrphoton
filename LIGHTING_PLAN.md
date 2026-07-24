# xrPhoton Lighting & Path Tracing Milestone — Implementation Plan

## 1. Why lighting now, skinning later

The engine is currently a ray-tracing *geometry viewer* with a game-shaped foundation
underneath it: OGFx ingestion, a materials/texture table, per-mesh BLAS + per-frame
TLAS rebuild, Jolt rigid dynamics, and a collision-aware player. What the frame
actually computes is one camera ray per pixel with view-dependent normal shading
(`shaders/raytrace.slang:259-260`) over a dark red miss constant — there is no light
in the world. Every other subsystem is ahead of the image. Converting the trace into
real light transport (sun, shadows, a diffuse bounce) is the change that turns the
renderer into the first genuine version of the intended engine: a path-tracing-only
selective modernization of the X-Ray/STALKER renderer, where the ARCHITECTURE.md
roadmap already names this as step 4 (`ARCHITECTURE.md:1280-1286`).

Deformable skinning (the remaining step-3 slice, `ARCHITECTURE.md:1277-1279`) is
deliberately deferred: it needs a real animated character asset to have a consumer,
its compute-skinning/BLAS-refit machinery touches nothing that lighting needs, and
building it before there is a lit image would optimize an invisible feature. The
codebase has also visibly pre-planned this milestone — hit records are already
interleaved by `RayTypeCount`, and `rt_pipeline.cpp:33-34` says outright: "Raising
the build-owned constant must add the shadow variants atomically."

## 2. The five-phase milestone roadmap

**Phase 1 — Sun + simple procedural sky/environment.** One engine-supplied
directional sun (direction, radiance) delivered to raygen, and the radiance miss
shader becomes a simple procedural sky (horizon/zenith gradient) instead of the dark
red debug constant. Interfaces established: the raygen "frame constants" push payload
(camera + sun + frame index) and the convention that the miss shader *is* the
environment light. Constraint on phase 3: a miss-shader-only sky is reachable by
BSDF-sampled rays but not by NEE; when the sky must be importance-sampled as a light,
its evaluation has to be a shared function callable from raygen, so keep sky
evaluation in one Slang function from the start.

**Phase 2 — HDR target, BRDF materials + general multi-bounce loop in raygen.** The
phase opens by moving radiance off the 8-bit image: raygen writes an
`R16G16B16A16_SFLOAT` storage target, and a small tonemap compute pass
(fixed-exposure Reinhard-style curve — one deliberate curve, tuned once) writes the
existing 8-bit presentable image the blit already handles. Sharp specular highlights
cannot be validated honestly through a saturating 8-bit target, so HDR precedes the
BRDF rather than waiting for emissive content.

This is a real frame-graph change, not another stage in the ray-tracing pipeline.
`Swapchain` owns and recreates both resize-bound images: HDR radiance and the existing
LDR output. A separate RAII `TonemapPipeline` owns its compute pipeline, layout,
descriptor pool/set, and fixed `TonemapState`; resize rewrites its HDR/LDR views while
the RT descriptor's output binding moves to HDR. Device suitability and resource
creation both require `R16G16B16A16_SFLOAT` storage-image support. The frame records
RT shader-write → compute shader-read visibility for HDR, compute shader-write →
transfer-read visibility for LDR, and separate trailing execution dependencies from
HDR's compute read to the next frame's RT write and from LDR's transfer read to the
next frame's compute write. LDR deliberately keeps the existing stable end-of-frame
contract: `TRANSFER_SRC_OPTIMAL`, which capture reads directly. Before every tonemap,
the full-overwrite path discards it through `UNDEFINED → GENERAL`; the prior trailing
execution barrier, rather than preserved contents, closes the cross-frame
write-after-read hazard. The post-tonemap barrier transitions it back to
`TRANSFER_SRC_OPTIMAL`. The compute dispatch uses ceil-division, bounds-checks pixels
in the shader, and is gated against compute dispatch limits. Only current-frame HDR
and tonemap land here; histories remain phase 5. The capture/readback seam continues
to read this post-tonemap LDR image, not the HDR target, so its byte/hash contract and
final-layout assumption survive the resource split.

The slice already establishes an iterative throughput loop with
`MaxPathVertices = 2`; after HDR is live, this phase raises that cap, adds Russian
roulette, and replaces Lambert with the first rough/specular BRDF. `MaterialRecord`
gains scalar perceptual roughness and dielectric `F0`. The BRDF-data route is decided
now, not then: texture-sampled values stay owned by closest-hit (it owns the
UV-gradient machinery), which returns sampled results plus `materialIndex` in
`SurfaceHit`; binding 3's material records — visible only to hit stages today
(`rt_pipeline.cpp:108-121`) — gain raygen visibility so the loop fetches the scalar
BRDF parameters by that index.

The BRDF answer is one isotropic dielectric model: Lambert diffuse plus GGX
Trowbridge-Reitz specular, Schlick Fresnel from scalar `F0`, and Smith masking/
shadowing. Perceptual roughness maps to
`alpha = max(roughness², 1e-4)`; diffuse is Fresnel-attenuated so the two lobes do not
simply add energy. Sampling chooses between the diffuse and GGX lobes with their
normalized energy weights, uses visible-normal (VNDF) sampling for GGX, and evaluates
the matching mixture PDF. The phase raises `MaxPathVertices` to 8 and starts Russian
roulette after vertex 3, using clamped
maximum-throughput survival probability `[0.05, 0.95]` (zero throughput terminates
first) and dividing surviving throughput by that probability. These choices consume
the same deterministic RNG stream; no recursive shader calls appear.

This is an explicit **OGFX_MATERIALS v2**, not a reuse of v1 reserved words by
stealth. Version 1 continues to require zero at record offsets 24 and 28 exactly as
today (`FORMATS.md:466-474`) and maps to deliberate runtime defaults
(`roughness = 1.0`, `dielectricF0 = 0.04`). Version 2 gives those two words their new
finite `[0, 1]` scalar meanings. The new decoder accepts both versions, the writer
keeps emitting v1 when every material has the v1 defaults (preserving existing
canonical bytes), and emits v2 only when explicit values require it. Compiler,
writer, schema/runtime decoder, loader, Blender/legacy mapping, and v1→runtime-default
tests land atomically. Every accepted legacy shader profile gets an explicit mapping;
an unknown source shader still fails rather than inheriting guessed BRDF values.

All `TraceRay` calls stay in raygen, so `maxPipelineRayRecursionDepth` stays at the
spec-guaranteed 1 forever. Constraint on phase 5: first-hit diffuse albedo, diffuse
radiance, and specular radiance remain separable; base-color demodulation applies to
the diffuse lobe, not indiscriminately to specular transport.

**Phase 3 — Static-light ABI + light-selection groundwork.** The shadow ray type and
per-geometry shadow SBT records land in the slice (sun-only NEE). This phase adds a
Vulkan-free `SceneLighting` authority and a `GpuLighting` owner. `SceneLighting`
contains the mutable analytic sun plus immutable static-light records; `main()` owns
it until a level/time-of-day owner exists. The sun remains in per-frame push state
and is **not duplicated** in the static storage buffer. A later sun update therefore
changes one CPU value and the push payload only. The static buffer and its
power-weighted distribution are uploaded once by `GpuLighting`. With no static
lights, `GpuLighting` still binds one valid sentinel record/CDF allocation while
publishing logical count and total sampling weight zero; the sentinel is never
indexed. The hierarchy chooses the sun with probability 1 when its weight is positive
and static weight is zero, and skips NEE cleanly when total light weight is zero.
For phase 3's sun-only scene, `main()` constructs `SceneLighting{sun, {}}` and the
sentinel-backed `GpuLighting`; phase 4 populates that same seam before GPU creation.

Selection is hierarchical: first choose the analytic sun versus the static-light set
from nonnegative sampling weights, then sample the static distribution. Static
triangle weight is `area × luminance(emission)`; the analytic sun uses
`luminance(sunRadiance)` as a documented 1 m² reference heuristic. The units are a
variance choice, not a physical-flux claim, and do not affect unbiasedness.
`GpuLighting` exposes the immutable static total; each frame CPU code computes
`pStaticSet` from that total and current sun radiance, and phase 3 promotes the push
payload's offset-76 `pad0` to that float without growing the 96-byte ABI. Raygen uses
`1 - pStaticSet` for the sun branch and divides either direct-light estimator by its
chosen branch probability.

The procedural sky stays BSDF-sampled through this milestone; it is not a third entry
in the NEE hierarchy. Environment importance sampling waits for an environment map
or measured variance need, while the shared `skyRadiance` function keeps that later
promotion local. Phase 3 pins the `LightRecord`/distribution ABI and tests the one
power-weighted selector with equal-power (uniform expected) and unequal-power
synthetic arrays; the rendered scene still has only the sun, so full GPU many-light
acceptance is explicitly deferred to phase 4. MIS also moves to phase 4: a delta sun
and BSDF-only sky have no overlapping non-delta technique to test. Per
`ARCHITECTURE.md:1284-1286`, many-light sampling is a first-class requirement, with
ReSTIR as the tracked follow-up.

**Phase 4 — Emissive geometry + nighttime/anomaly test scene.** With phase 2's HDR
target and tonemap already live, `MaterialRecord`/`SceneMaterial` gain constant RGB
emission through an explicit **OGFX_MATERIALS v3**. Its record is exactly 48 bytes:
the first 32 bytes retain v2 meanings, offsets 32/36/40 are finite, nonnegative
emission RGB, and offset 44 is reserved zero. Versions 1 and 2 decode to zero
emission. The canonical writer emits the lowest sufficient version: v1 for the
legacy defaults, v2 for nondefault roughness/`F0` with zero emission, and v3 when any
emission is nonzero. Compiler, writer, schema/runtime decoder, loader, format
documentation, and v1/v2 compatibility tests land atomically with the GPU-record
growth.

A Vulkan-free scene-lighting builder takes both
`SceneData` and the scene producer's explicit dynamic-instance set, rejects emission
on dynamic placements and alpha-tested geometry for this phase, and extracts static
world-space triangles. The latter avoids pretending a full-triangle area PDF samples
a cutout's unknown emitting area. This policy does not live in generic scene
assembly, which cannot infer mobility from `SceneData` alone. The gallery continues
to return `SceneData` plus
`GalleryLoadResult::dynamicInstances`; after gallery load, `main()` invokes the
builder with both, installs its records in the `SceneLighting` it owns, and then
constructs `GpuLighting`. A future level owner supplies the equivalent
classification. Dynamic and alpha-tested emitters are tracked follow-ups alongside
ReSTIR.

The builder also creates the reverse mapping needed when a BSDF ray hits an emitter:
the hit-stage lookup is keyed by
`(InstanceIndex(), GeometryIndex(), PrimitiveIndex())`. `InstanceID()` deliberately
remains `mesh.firstGeometry` for the existing geometry-record lookup and cannot
distinguish repeated placements. The reverse record supplies the unique light ID,
two-sided emission, conditional triangle-selection probability, and area; closest-hit
returns evaluated emission plus the conditional solid-angle density. For both NEE
and a BSDF hit, the comparable non-delta light PDF is
`pStaticSet × pTriangle × (1 / area) × distance² /
abs(dot(NgLight, -wi))`, with zero density at a grazing emitter. Closest-hit can form
everything except the mutable `pStaticSet`; raygen multiplies that current
hierarchical probability into the returned conditional density. The delta sun never
enters this MIS balance. `materialIndex` alone would be ambiguous for repeated
instances and triangles.

Emissive NEE uses a finite visibility segment, never the sun's infinite shadow range:
from the numerically offset surface origin, compute normalized `wi` and distance to
the sampled triangle point, reject a degenerate segment, and trace with
`TMax = distance - max(1e-3, 1e-4 × distance)`. A nonpositive resulting range
contributes zero. This endpoint exclusion prevents both the selected emitter and
geometry behind it from being reported as blockers.

MIS between NEE and BSDF sampling lands here, now that non-delta lights exist to
test it. Both paths use the power heuristic with exponent 2; a missing/zero-density
competing technique gets weight 1. A nighttime yard variant (gallery-policy-style,
code-owned placements) with several small emitters becomes the standing many-light
acceptance scene. Constraint on phase 5: this scene is the noisiest content in the
project and is the denoiser's acceptance test — build it before the denoiser so
phase 5 has an honest target.

**Phase 5 — Temporal accumulation, motion vectors, SVGF denoising.** One coupled
system: separate diffuse/specular history and moments targets beside phase 2's HDR
radiance images (the 8-bit image remains the post-tonemap output), per-instance
previous-frame transforms and a real previous-frame view-projection (the first
consumer of camera matrices, anticipated at `ARCHITECTURE.md:1120-1122`),
first-hit albedo/normal/depth/roughness outputs from raygen, diffuse-albedo
demodulation for the diffuse lobe, and specular-aware filtering guided by roughness
for the specular lobe. Disocclusion-aware temporal reprojection feeds the SVGF-class
filter. A dedicated `TemporalHistory` owner — never `FrameResources` — owns the
ping-pong images, the previous successful frame's view-projection and instance
transforms, prebuilt ping-pong descriptor variants, a history index, and a validity
generation. Frame N reads the committed state from successful frame N−1 even when
those submissions use different frame slots; explicit image dependencies preserve
that queue-ordered chain, and an in-flight descriptor set is never rewritten. Commit
and rotate only after a `VK_SUCCESS` draw. A device-idle resize recreates every
extent-bound history/AOV image and rewrites both descriptor variants. Scene-topology
replacement uses the same device-idle handoff to rebuild previous-transform storage
to the new instance count and rewrite affected descriptors. Both operations
invalidate the generation, as do out-of-date/suboptimal and camera-mode teleport; a
frame skipped before submission neither commits nor rotates.

This is also when the physics-interpolation trigger fires
(`ARCHITECTURE.md:1299-1302`). Everything phases 1–4 must hand it: frame-indexed
deterministic RNG (slice), separable albedo/normal (slice), HDR radiance (phase 2),
motion-vector inputs (this phase).

## 3. The immediate slice, in full

Scope: one engine-supplied directional sun; Lambertian direct lighting from the
sampled base color; hard shadow rays as a second ray type; one cosine-sampled
indirect diffuse bounce; a simple gradient sky in the radiance miss shader;
deterministic hash-based RNG plus a deterministic capture mode that makes image
oracles a hash comparison; validated in the existing yard. This completes phase 1
and takes a deliberate vertical cut through phases 2–3.

### 3.1 The deliberate design answers

Each fork gets one answer, per the project convention.

**Shadow rays are a second SBT ray type, not ray queries and not a miss-shader
trick.** The alternative — `VK_KHR_ray_query` inline in raygen — would avoid touching
the SBT, but any-hit shaders do not run in ray queries, so leaf-card alpha shadows
would require duplicating the alpha fetch/cutoff logic as manual candidate processing
in raygen, creating a second alpha-test system, and would add a new hard device
requirement. The SBT second-ray-type is also exactly the mechanism phase 3's
many-light NEE needs, and the codebase already carries the interleaved-record
machinery for it: `buildShaderBindingTable` already sizes the miss region as
`RayTypeCount * recordStride` and the hit region as `geometryCount * RayTypeCount`
(`rt_pipeline.cpp:414-419`), and instance SBT offsets are already
`mesh.firstGeometry * RayTypeCount` (`acceleration_structure.cpp:615-616`). We are
filling in a slot the ABI reserved.

**Shading happens in raygen; closest-hit returns hit information.** The roadmap pins
phase 2 as "an iterative bounce loop in raygen (keeping pipeline recursion depth at
1)" (`ARCHITECTURE.md:1281-1283`). Only raygen-side shading satisfies that: if
closest-hit traced shadow or bounce rays, recursion depth would be 2 and
`maxPipelineRayRecursionDepth = 1` (`rt_pipeline.cpp:344`) — the spec-guaranteed
minimum requiring no limit query — would need raising. Shading in raygen also means
sun parameters are consumed only by raygen, so the push-constant range stays
raygen-only, unchanged in stage flags.

**Sun and frame state ride the existing push constants, not a uniform buffer.** The
payload grows from 64 to 96 bytes — still under the spec-guaranteed 128-byte
`maxPushConstantsSize` (the existing struct's own comment budget,
`camera.hpp:93-94`). Push constants are recorded into each frame slot's own command
buffer after its fence wait, so per-frame sun/frame-index values are race-free across
frames in flight *by construction*, with no descriptor churn
(`ARCHITECTURE.md:1106-1113`). Per-slot uniform buffers remain the documented
promotion path for per-frame data; phase 3's immutable static-light buffer is
scene-owned while the analytic sun stays in this per-frame payload.

**Cosine-weighted hemisphere sampling for the single indirect bounce.** For a
Lambertian BRDF, the cosine pdf cancels the `cos θ / π` term exactly, so the bounce
estimator degenerates to `albedo × incomingRadiance` — the lowest-variance choice, no
divisions, no zero-pdf edge cases. Uniform hemisphere sampling would only add
variance for no generality we need. Sun-only-direct (no bounce) was rejected because
a delta directional light needs no random numbers at all, which would leave the
slice's required deterministic-RNG machinery without a consumer and shadowed areas
pitch black.

**Deterministic RNG: PCG-style hash seeded by (pixel, frameIndex).** Detailed in
§3.6.

**No accumulation buffer in this slice.** Single-sample-per-pixel output into the
existing shared `R8G8B8A8_UNORM` storage image, saturated on write. Consequence
accepted: 1 spp of stochastic bounce produces visible per-frame noise shimmer.
Sun/sky radiance constants are chosen so the lit yard stays inside the 8-bit range
(fixed implicit exposure of 1.0). The current-frame HDR target and tonemap arrive at
the head of phase 2, before rough/specular transport; temporal history, moments, and
reprojection remain phase 5.

### 3.2 Routing-ABI changes: `RayTypeCount` 1 → 2

Three build-owned values live together in `CMakeLists.txt`:
`XRPHOTON_RADIANCE_RAY_TYPE = 0`, `XRPHOTON_SHADOW_RAY_TYPE = 1`, and
`XRPHOTON_RAY_TYPE_COUNT = 2`. All three flow to C++ through the `PUBLIC` compile
definitions on `xrPhotonOgfxRuntime` and to Slang through matching `-D` arguments.
The semantic indices therefore cannot drift silently between languages; this is one
generated build ABI, not two hand-maintained copies. Everything below changes
atomically with those definitions, exactly as the guard comments demand.

**`src/ray_types.hpp`** — add named ray-type indices to the shared ABI so C++ SBT
construction and documentation share one vocabulary:

```cpp
inline constexpr uint32_t RadianceRayType = XRPHOTON_RADIANCE_RAY_TYPE;
inline constexpr uint32_t ShadowRayType = XRPHOTON_SHADOW_RAY_TYPE;
static_assert(RadianceRayType == 0);
static_assert(ShadowRayType == 1);
static_assert(ShadowRayType < RayTypeCount && RayTypeCount == 2);
```

The shader derives the same two names from
`XRPHOTON_RADIANCE_RAY_TYPE` / `XRPHOTON_SHADOW_RAY_TYPE`, asserts their expected
order beside its count guard, and uses them for every `TraceRay` offset/miss
argument. C++ SBT writes likewise index records through the named constants; no
literal semantic index appears at a trace or record-selection site.

**`src/rt_pipeline.cpp`** — the concentrated change site:

- Anonymous-namespace group table (`rt_pipeline.cpp:26-34`) becomes 7 groups over 6
  stages, and `static_assert(RayTypeCount == 1)` becomes `== 2` with the comment
  updated to say the shadow variants now exist:

  | Group | Type | Shaders |
  |---|---|---|
  | 0 `RaygenGroup` | GENERAL | stage 0 `rayGenMain` |
  | 1 `RadianceMissGroup` | GENERAL | stage 1 `missMain` |
  | 2 `ShadowMissGroup` | GENERAL | stage 2 `shadowMissMain` |
  | 3 `OpaqueRadianceHitGroup` | TRIANGLES | closest-hit = stage 3 `closestHitMain` |
  | 4 `AlphaTestedRadianceHitGroup` | TRIANGLES | closest-hit = stage 3, any-hit = stage 4 `anyHitMain` |
  | 5 `OpaqueShadowHitGroup` | TRIANGLES | **all shaders `VK_SHADER_UNUSED_KHR`** |
  | 6 `AlphaTestedShadowHitGroup` | TRIANGLES | any-hit = stage 5 `shadowAnyHitMain` only |

  The empty group 5 is deliberate and spec-legal (a triangles hit group may leave
  both closest-hit and any-hit unused): a shadow ray that accepts an opaque hit must
  simply *not* run the miss shader, leaving the payload's occluded initialization
  intact. The existing explicit-`VK_SHADER_UNUSED_KHR` initialization loop
  (`rt_pipeline.cpp:315-321`) already produces this group for free.
- `createRtPipeline` stage array (`rt_pipeline.cpp:292-308`) grows to 6 entries
  (`shadowMissMain`, `shadowAnyHitMain` added); `stageCount = 6`,
  `groupCount = GroupCount = 7`. `maxPipelineRayRecursionDepth` stays 1 — all
  `TraceRay` calls remain in raygen (§3.1).
- Push range (`rt_pipeline.cpp:256-259`): `pushRange.size =
  sizeof(RaygenPushConstants)` (96), stage flags unchanged
  (`VK_SHADER_STAGE_RAYGEN_BIT_KHR` only). The `#include "camera.hpp"` at
  `rt_pipeline.cpp:3` becomes `#include "lighting.hpp"`.
- `buildShaderBindingTable`: region math (`rt_pipeline.cpp:412-420`) is already
  `RayTypeCount`-parameterized and needs no arithmetic change. Two write loops
  change:
  - Miss records (`rt_pipeline.cpp:475-480`): stop copying the single `MissGroup`
    handle to every slot; copy `RadianceMissGroup` at `RadianceRayType` and
    `ShadowMissGroup` at `ShadowRayType`.
  - Hit records (`rt_pipeline.cpp:481-491`): the loop already derives
    `geometryIndex = recordIndex / RayTypeCount`; add
    `rayType = recordIndex % RayTypeCount` and select from the 2×2 table —
    radiance→{3,4}, shadow→{5,6}, opaque/alpha-tested choosing within each pair via
    the existing `scene.geometries[geometryIndex].alphaTested`.

**`src/acceleration_structure.cpp` — no code change.** Instance
`sbtOffset = mesh.firstGeometry * RayTypeCount` (`:615-616`), its 24-bit gate
(`:617-622`), `instanceCustomIndex = mesh.firstGeometry` (`:627`), and per-geometry
opacity flags (`:499-501`) all pick up the doubled stride from the shared constant
automatically. Hit-record identity is unchanged: `InstanceID() + GeometryIndex()`
still recovers the flat `GeometryRecord`, independent of ray type.

**`src/scene_assembly.cpp` — no code change.**
`MaximumGeometryCount = (SbtOffsetLimit - 1) / RayTypeCount + 1` (`:25-27`) halves
automatically (to 8,388,608 — still vastly above any scene). The matching test at
`tests/scene_assembly_tests.cpp:1272-1295` computes the same formula from
`xrphoton::RayTypeCount`, so it stays green without edits — verify, don't assume.

**Documentation and comment sweep.** The change is semantic, not merely numeric.
Search all source and documentation for stage/group counts, `RayTypeCount`,
`CameraPushConstants`, "64 bytes", "dark red", "view-dependent", and "primary rays
only". Update the complete RT-pipeline description, module/ownership table, header
dependency rule, frame flow, push-payload contract, renderer comments, and roadmap
status. Add `lighting.{hpp,cpp}` and the public capture/readback seam to the module
map and synchronization narrative. README's status and command documentation must
describe the landed sun/shadow/bounce and `--capture`; FORMATS' runtime routing
paragraph must say `RayTypeCount = 2`. The grep results plus a semantic read of the
camera, renderer, and RT-pipeline sections are the deliverable; a hand-picked list of
line numbers is not.

**Miss-record indexing at trace time:** radiance `TraceRay` uses
`RadianceRayType` for `MissShaderIndex` and
`RayContributionToHitGroupIndex`; shadow uses `ShadowRayType` for both; both keep
`MultiplierForGeometryContribution = XRPHOTON_RAY_TYPE_COUNT` (now 2), matching the
interleaved-by-ray-type hit layout the SBT writes.

### 3.3 Shader changes (`shaders/raytrace.slang`)

- Guard at `:5-7` becomes `#if XRPHOTON_RAY_TYPE_COUNT != 2 #error "..."`.
- **Push block** (`:56-65`) becomes the frame-constants block mirroring the CPU
  struct field-for-field:

  ```slang
  struct RaygenPushConstants
  {
      CameraPushConstants camera;   // unchanged 64-byte prefix
      float3 sunDirection;          // unit, surface -> sun
      float pad0;
      float3 sunRadiance;
      uint frameIndex;
  };
  [[vk::push_constant]] ConstantBuffer<RaygenPushConstants> frameConstants;
  ```

- **Radiance payload** (`:67-75`) restructures from "final color" to a hit-info
  carrier. `rayDirectionX/Y` stay (they feed `fetchHitAttributes`' UV gradients); the
  output side becomes `float hitT` (−1.0 on miss), `float3 albedo`,
  `float3 shadingNormal`, and `float3 geometricNormal` — the triangle facet normal,
  `normalize(cross(positionEdge1, positionEdge2))` from the world-space edges
  `fetchHitAttributes` already computes for its UV gradients. Closest-hit returns both
  normals un-oriented; one raygen helper owns the surface convention at every path
  vertex. Given `wo = -ray.Direction`, it face-forwards the geometric normal `Ng`
  toward `wo`, face-forwards the interpolated shading normal `Ns` toward `wo`, and
  falls back to `Ns = Ng` if `dot(Ns, Ng) <= 0`. Shading uses `Ns`; secondary-ray
  origins offset along `Ng`, and every reflected direction additionally requires
  `dot(Ng, wi) > 0`. This prevents a smooth normal from launching a ray below the
  actual triangle. On miss, `hitT = -1` and `albedo` carries sky radiance — a
  documented dual use that keeps the immediate payload at 16 scalars; raygen is the
  only reader and branches on `hitT` first. Phase 2 adds `materialIndex`; phase 4
  adds evaluated emission/conditional-light-PDF fields explicitly rather than
  pretending the payload never grows. `closestHitMain` (`:251-261`) becomes: fetch attributes,
  sample base color (`baseColorFactor.rgb × SampleGrad(...).rgb` exactly as today),
  and write `albedo`/raw normals/`hitT = RayTCurrent()`.
  `missMain` (`:244-249`) writes `hitT = -1` and a simple two-tone sky, e.g.
  `lerp(HorizonRadiance, ZenithRadiance, saturate(direction.y))` with shader-owned
  constants — the phase-1 "simple procedural sky", kept as one function
  (`skyRadiance(float3 direction)`) per the phase-3 constraint in §2.
- **New `ShadowPayload`**: one `float visibility`, initialized to 0.0 by raygen;
  **`shadowMissMain`** sets it to 1.0. An accepted hit runs nothing (empty group /
  skip-closest-hit flag), leaving 0.0 = occluded — the initialization *is* the
  occlusion result, so no closest-hit stage is ever needed on the shadow type.
- **New `shadowAnyHitMain(inout ShadowPayload,
  BuiltInTriangleIntersectionAttributes)`**: lean alpha test with **identical
  accept/reject semantics to the radiance any-hit** (`raytrace.slang:263-273`) —
  index fetch, barycentric UV interpolation, `materials[...]` lookup, then
  `alpha = material.baseColorFactor.a *
  sceneTextures[NonUniformResourceIndex(material.baseColorTexture)]
  .SampleLevel(uv, 0).a` and `IgnoreHit()` below `material.alphaCutoff`. Both the
  `baseColorFactor.a` factor and the non-uniform index qualifier must match the
  radiance path, or shadows and visible cutouts can disagree; only the gradient
  machinery differs (`SampleLevel(uv, 0)` is exact since the sampler's maxLod is
  already 0 — no gradients needed, unlike the radiance path's anisotropy). It cannot share
  `fetchHitAttributes` (typed on the radiance payload and computing normals/gradients
  the shadow test doesn't need); factor the index/UV interpolation into a small
  shared helper so the cutoff comparison logic exists once.
- **`rayGenMain`** (`:200-242`) becomes a real iterative shading kernel with
  `MaxPathVertices = 2`, rather than separate first/second-hit code that phase 2
  immediately has to replace:
  1. Build the primary ray and its neighboring directions exactly as today.
     Initialize `radiance = 0`, `throughput = 1`, and the per-pixel RNG state.
  2. For each vertex, initialize a fresh hit payload and trace radiance using
     `RadianceRayType`, multiplier `XRPHOTON_RAY_TYPE_COUNT`,
     miss `RadianceRayType`, and `RAY_FLAG_NONE`.
  3. On miss, add `throughput × payload.albedo` (sky) and break. On hit, reconstruct
     `worldPos = ray.Origin + ray.Direction * hitT`, run the shared orientation
     helper above to obtain `Ns`/`Ng`, and preserve the first hit's albedo/normals
     separately for phase-5 AOVs.
  4. **Direct sun at every hit:** proceed only when both
     `dot(Ns, sunDirection) > 0` and `dot(Ng, sunDirection) > 0`. Trace from
     `worldPos + Ng * 1e-3` toward the sun with `TMin = 0`,
     `TMax = ShadowTMax = 1.0e30` — deliberately not the camera's 100-unit
     `SceneTMax`. Use `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
     RAY_FLAG_SKIP_CLOSEST_HIT_SHADER`, `ShadowRayType` for hit/miss selection, and
     never force opaque. Add
     `throughput × albedo / π × sunRadiance × dot(Ns, sunDirection) × visibility`.
  5. If this is the final vertex, break. Otherwise multiply throughput by albedo,
     draw two RNG floats, and cosine-sample about `Ns` through the robust sign-based
     Frisvad/Duff ONB. If `dot(Ng, bounceDirection) <= 0`, this sample contributes
     zero and the path terminates without tracing; do not resample and silently
     change the PDF. For an accepted sample, cosine weighting cancels Lambert's
     `cos/π`, so the throughput update above is complete. The next ray is fully
     specified: origin `worldPos + Ng * 1e-3`, normalized sampled direction,
     `TMin = 0.001`, `TMax = SceneTMax`, and radiance routing. Secondary
     `rayDirectionX/Y` equal the bounce direction, making the current mip-0-only
     gradient footprint zero. The same loop reconstructs and orients the second
     hit; no camera origin or prior payload field is reused.
  6. `outputImage[pixel] = float4(saturate(radiance), 1.0)` — `saturate`, not
     `min(color, 1.0)`, so numeric-noise negatives clamp too; the explicit clamp
     documents the 8-bit budget.

Lighting stays in linear space end-to-end with no new work: sRGB texture formats
decode on sample, and the existing UNORM→sRGB blit already gamma-encodes for
presentation (`ARCHITECTURE.md:620-622`).

### 3.4 CPU-side changes

**New unit `src/lighting.hpp` / `src/lighting.cpp`** (GLFW-free and Vulkan-free:
standard library + GLM + `camera.hpp`). Owns:

- `RaygenPushConstants` — the CPU mirror, embedding
  `CameraPushConstants` with its 64-byte field layout unchanged. The stale
  `camera.hpp:86-99` comment that calls it the complete shader push block and owns
  the whole 128-byte budget changes during the semantic documentation sweep:

  ```cpp
  struct RaygenPushConstants
  {
      CameraPushConstants camera;
      glm::vec3 sunDirection; float pad0 = 0.0f;
      glm::vec3 sunRadiance;  uint32_t frameIndex = 0;
  };
  static_assert(sizeof(RaygenPushConstants) == 96);
  static_assert(offsetof(RaygenPushConstants, camera) == 0
      && offsetof(RaygenPushConstants, sunDirection) == 64
      && offsetof(RaygenPushConstants, sunRadiance) == 80
      && offsetof(RaygenPushConstants, frameIndex) == 92);
  ```

  (A vec3-then-scalar tail packs at offset 92 under every relevant GPU layout rule,
  same reasoning as the camera struct's comment.)
- `DirectionalSun` — the Vulkan-free owner seam for sun state:
  `struct DirectionalSun { glm::vec3 direction; glm::vec3 radiance; };` plus a
  `DefaultSun` constant (an angled morning/afternoon direction with positive y, e.g.
  `{-0.35f, 0.9f, -0.25f}` pre-normalization, chosen so yard walls and crates throw
  long visible shadows; warm radiance, ~2.5-ish per channel, tuned at the visual
  check against the 8-bit clamp). `main()` owns a `DirectionalSun` value beside the
  camera — the input seam a later time-of-day or scene-lighting producer replaces
  without an API rewrite, and the reason degenerate-sun tests are writable at all.
- `makeRaygenPushConstants(const CameraPushConstants&, const DirectionalSun&,
  uint32_t frameIndex)` — takes
  the **already-built** camera payload, copies it in verbatim, normalizes the given
  sun direction (guarded, in the spirit of `normalizeOrZero`, `camera.cpp:29`), and
  fills radiance and frame index. It deliberately does *not* call
  `makeCameraPushConstants`: that function lives in `camera.cpp`, a GLFW-dependent
  translation unit (`camera.cpp:8` includes GLFW for `updateCamera`'s input policy),
  and taking the finished payload keeps `lighting.cpp` linkable with GLM alone — the
  property the test target in §3.8 depends on. `main()` composes the two calls.
- The **C++ RNG reference**: `constexpr uint32_t pcgHash(uint32_t)` and
  `constexpr float rngNextFloat(uint32_t& state)` mirroring the Slang implementation
  bit-for-bit. Known-answer vectors pin the intended algorithm on the CPU; the
  deterministic capture pins GPU repeatability. This is not mislabeled as an
  automated cross-language equivalence test: a direct shader known-answer probe is
  deferred until a reusable GPU test/readback harness exists, and review must compare
  the compact Slang function against the pinned vectors when it changes.

**`src/renderer.hpp`** — forward-declare `RaygenPushConstants` instead of
`CameraPushConstants` (`renderer.hpp:10`) and change `drawFrame`'s third parameter
(`renderer.hpp:54-57`). **`src/renderer.cpp`** — include `lighting.hpp`;
`recordTraceCommandBuffer`'s parameter and the `vkCmdPushConstants` call
(`renderer.cpp:140-146`) push `sizeof(RaygenPushConstants)` with unchanged
raygen-only stage flags.

The renderer also exposes one explicit one-shot readback seam:
`VkResult readbackStorageImage(const Renderer&, uint32_t finalSubmittedFrameSlot,
StorageImageReadback*)`. `Renderer` gains a borrowed `VmaAllocator` alongside its
other borrowed handles, initialized from `ctx.allocator`; it remains a non-owner.
`StorageImageReadback` owns width, height, and tightly packed RGBA8 bytes; it owns no
Vulkan handles. The function's contract requires rendering to have stopped after
the named latest submission — no later frame may be queued against the shared
storage image. Capture mode satisfies that by leaving its loop immediately after the
final successful draw. The function:

1. validates the slot and checked `width × height × 4` byte count, then waits that
   final slot's render fence (trace-queue order thereby retires every earlier frame);
2. allocates a temporary `TRANSFER_DST` buffer through `createBuffer` with
   `HOST_ACCESS_RANDOM | MAPPED` (therefore host-coherent under the helper's
   contract);
3. resets/reuses the completed slot's command buffer, records
   `vkCmdCopyImageToBuffer` from the storage image's final
   `TRANSFER_SRC_OPTIMAL` layout with tight `VkBufferImageCopy` packing, and records a
   `TRANSFER_WRITE → HOST_READ` buffer barrier (`TRANSFER → HOST`);
4. resets the slot fence only once a submit is guaranteed, submits the copy without
   swapchain semaphores, waits the copy fence, copies mapped bytes into the output,
   and destroys the temporary VMA allocation on every path.

This public function owns GPU readback and synchronization only. It does not parse
CLI arguments, hash pixels, or write files. Host-allocation failure is caught at this
boundary and returned as `VK_ERROR_OUT_OF_HOST_MEMORY`; exceptions do not cross the
renderer API.

**`src/main.cpp`** — add `uint32_t frameCounter = 0;` and
`DirectionalSun sun = DefaultSun;` beside `currentFrame`
(`main.cpp:539`); the draw call (`main.cpp:629-633`) becomes
`drawFrame(renderer, currentFrame,
makeRaygenPushConstants(makeCameraPushConstants(renderCamera, aspect), sun,
frameCounter))`. Interactive mode preserves the current slot-rotation behavior,
including advancing both counters after an attempted draw that returns out-of-date
or suboptimal; skipped interactive seeds are harmless.

**Deterministic capture mode (`--capture <successfulFrameCount> <outPath>`), owned by
`main.cpp`.** The positive frame count means successful `VK_SUCCESS` draws, not loop
iterations or attempted acquires. Capture snapshots a dedicated immutable
`captureCamera` from the gallery spawn, disables the physics character, skips all
input and player-camera attachment/query work, and advances rigid physics with
exactly `PhysicsFixedDt`. It records the slot used by each successful submit before
rotating `currentFrame`; `frameCounter` advances only with those successful capture
submissions. A resize-dirty callback, `OUT_OF_DATE`, or `SUBOPTIMAL` is a loud capture
failure rather than a nondeterministic recreate. The initial `swap.extent` is printed
and remains fixed for the run; closing the window before the requested count is also
a nonzero incomplete-capture failure.

After the requested final submit, `main()` passes the retained
`lastSubmittedSlot` to `readbackStorageImage`. It hashes the extent followed by raw
linear RGBA bytes with 64-bit FNV-1a: width then height as four little-endian bytes
each, followed by tightly packed rows. It converts RGB from linear UNORM to sRGB for
a binary PPM that visually matches presentation, writes the artifact with checked
I/O, prints the extent, successful-frame count, rendered `frameIndex`, and hash, and
exits 0; parse, readback, or publication failure is nonzero. Thus
`--capture N ...` renders indices `0..N-1` and reads index `N-1`.
Fixed stepping makes Jolt deterministic for a given binary on a given machine, so
successful frame N is reproducible without waiting for bodies to settle. The hash
turns "the image is unchanged" into a concrete local comparison.

**`CMakeLists.txt`** — add `src/lighting.cpp` to the `xrPhoton` executable sources
(`:652-665`); add the new test target (§3.8); define and export the count plus both
semantic ray-type indices to C++ and Slang (§3.2).

### 3.5 `MaterialRecord` / `GeometryRecord`: no ABI additions

None are needed and none should be made. The Lambertian albedo is exactly
`baseColorFactor.rgb × sampled base color`, both already present
(`gpu_scene.hpp:37-48`, `scene.hpp:37-45`); `alphaCutoff` already serves the shadow
any-hit. First real `MaterialRecord` growth (roughness/specular, then emission)
belongs to phases 2 and 4. Keeping the record ABIs untouched also keeps `GpuScene`
upload, the gallery, and every Vulkan-free suite bit-identical this slice.

### 3.6 Deterministic RNG — the concrete choice

- **Generator:** the standard single-word PCG hash
  (`state = v * 747796405u + 2891336453u;
  word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;`) as `pcgHash`, plus an advancing sampler:
  `state = pcgHash(state)` per draw, mapped to `[0,1)` via
  `(state >> 8) * (1.0f / 16777216.0f)`, an exact power-of-two scale accepted by
  both C++ and Slang. Integer arithmetic and the conversion are bit-exact across the
  CPU reference and GPU.
- **Seed:** `seed = pcgHash(pixel.x + pcgHash(pixel.y +
  pcgHash(frameConstants.frameIndex)))`, computed in raygen; the state lives in a
  raygen local for the pixel's lifetime. At most two draws are consumed in this slice
  (the cosine sample; miss paths consume none); the advancing-state interface absorbs
  phase 2's extra dimensions without redesign.
- **Where the frame index lives:** in the push payload, owned by `main()`'s
  `frameCounter` (§3.4). No GPU-persistent sampling state exists.
- **Determinism contract:** the traced image is a pure function of (scene, extent,
  camera payload, sun state, frameIndex) — no wall-clock, no uninitialized state.
  Interactive runs still feed clamped wall-clock `dt` into the physics accumulator
  (`main.cpp:544-548`), so they are not reproducible and are not asked to be;
  reproducibility is delivered by capture mode (§3.4): immutable camera snapshot,
  disabled character, fixed `PhysicsFixedDt`, successful-submit counting, fixed
  extent, storage-image readback, and an extent-qualified pixel hash at the chosen
  frame. Any resize/out-of-date/suboptimal event aborts instead of perturbing the
  sequence. The frame index is *in* the seed now — even though nothing accumulates
  yet — because phase 5 requires per-frame decorrelation; the cost is visible
  per-frame shimmer, accepted in §3.1. Oracles are device/driver/binary-pinned (float
  shading math varies across GPUs; the integer RNG itself does not).

### 3.7 Accumulation decision

**None this slice** — stated explicitly: single-sample output into the existing
shared storage image, `prepareRtForSwapchain` and the swapchain/resize machinery
untouched. Current-frame HDR + tonemap arrive in phase 2; temporal history, moments,
reprojection, and denoiser validation arrive together in phase 5.

### 3.8 Validation

**Builds/presets:** `debug` (validation ON), `release`, and `ogfx-core`
(`CMakePresets.json`) must all configure and build. `ogfx-core` is unaffected by
construction — `lighting.cpp` and every `RayTypeCount` consumer sit behind the
`XRPHOTON_BUILD_ENGINE` gate (`CMakeLists.txt:347-351`); keeping it that way is the
Vulkan-free-layer guarantee.

**Existing ctest suites that must stay green** (all graphics-free): `ogfx_core`,
`ogfx_decoder`, `legacy_ogf_adapter`, `blender_mesh_adapter`, the two CLI tests,
`ogfx_runtime_loader`, `scene_assembly` (its `RayTypeCount`-scaled SBT-limit cases at
`tests/scene_assembly_tests.cpp:1272-1295` recompute from the constant — confirm they
pass at 2), `texture_loader`, all five `gallery_policy*` variants, `physics`,
`player`.

**New tests — `lighting` suite** (`tests/lighting_tests.cpp`, target
`xrPhotonLightingTests` linking `src/lighting.cpp` + GLM only, modeled on the
`xrPhotonPlayerTests` pattern at `CMakeLists.txt:605-613`; inside the engine gate,
graphics-free — possible precisely because `makeRaygenPushConstants` takes a
finished `CameraPushConstants` instead of calling into the GLFW-dependent
`camera.cpp`, §3.4): pin `RaygenPushConstants` size/offsets at runtime in addition to
the compile-time asserts; pin `makeRaygenPushConstants` (a passed-in
  `DirectionalSun`'s direction normalized, a degenerate direction guarded — testable
  precisely because the sun is a parameter, not a baked default — frame index passed
  through, and the camera prefix
  copied verbatim from a `CameraPushConstants` value the test constructs directly); pin
  `pcgHash` known-answer vectors and a short `rngNextFloat` sequence for a fixed seed
  with all outputs in `[0,1)`. These vectors are the normative algorithm reference
  that shader review checks; capture separately proves GPU repeatability, not direct
  CPU/GPU vector equality.

**Sync / GPU-assisted runs:** repeat the three-pass practice recorded for the
previous milestone (`ARCHITECTURE.md:1212-1213`): plain validation (debug build),
synchronization validation, and GPU-assisted validation enabled through the Khronos
layer's settings. Each pass keeps the exact layer-settings file and captured log as
an acceptance artifact; the log must contain the engine's
`Using Vulkan validation layer: VK_LAYER_KHRONOS_validation` startup line and the
requested validation feature's activation message. Because the engine messenger
currently filters out INFO/GENERAL (`vulkan_context.cpp:599-607`), the checked
settings route those severities to a layer-owned log; acceptance does not infer
activation from the engine's warning/error stream. Best-effort fallback without the
layer is not an accepted validation run. Plain/GPU-AV can catch invalid accesses, but
a valid-yet-wrong SBT handle mapping is a semantic error caught by capture/visual
behavior, not promised validation magic. Run each pass through startup, several
seconds of yard simulation, a resize, and an F1 switch. Step 6 and final GPU-AV use
the configured leaf build so `shadowAnyHitMain`'s BDA, texture, and index accesses
are instrumented; generated-only cannot exercise that record.

**Visual check in the yard.** Generated-only build (no alpha-tested geometry exists
here — leaf/tail are configured exhibits): sun-consistent bright/dark facing on
ground, wall corner, steps; hard-edged shadows from the wall and both crates falling
in the direction implied by `DefaultSun.direction`; the dynamic crate's shadow
tracking it as it tumbles and settles (proving shadow rays traverse the per-frame
rebuilt TLAS); shadowed regions dimly filled by sky + one bounce (not black), with
expected 1-spp shimmer; sky gradient at the horizon on miss. The configured yard is
an **explicit acceptance prerequisite** for this slice, not an optional extra:
`shadowAnyHitMain` never executes in a generated-only build (no alpha-tested
geometry exists there — the same is already true of the radiance any-hit today, per
the repo's standing policy that alpha proofs are configured exhibits), so only the
configured build proves the shadow any-hit path. There, the leaf card must cast a
**cutout-shaped** shadow
(transparent DXT1 texels pass sun light through `shadowAnyHitMain`'s `IgnoreHit`),
while the pseudodog tail — alpha-routed but fully opaque at mip 0 — casts a solid
shadow, exercising the shadow any-hit sample-and-compare without the visible
rejection branch, mirroring the radiance-path split already documented at
`ARCHITECTURE.md:60-66`. The generated-only build remains the smoke check.

### 3.9 Implementation order (each step builds and runs)

Image oracles below are capture-mode hash comparisons (§3.4, §3.6) at exactly eight
successful frames (`frameIndex = 7`), deliberately beyond
`MaxFramesInFlight = 2`; visual checks use the interactive yard.

1. **Lighting unit + tests.** Add `src/lighting.hpp/.cpp` (payload struct,
   `DirectionalSun` + `DefaultSun`, RNG reference, `makeRaygenPushConstants`) and
   the `lighting` ctest target. Nothing consumes it yet; all suites green.
2. **Deterministic capture mode.** Add positive-count CLI parsing, the immutable
   capture camera/disabled character, successful-submit and last-slot tracking,
   fixed-extent failure policy, the public renderer readback submission, and checked
   sRGB PPM + raw-extent hash output (§3.4). Exercise zero/invalid count and unwritable
   output failures. Then launch two independent
   `--capture 8 <different-output-path>` runs from fresh processes; require identical
   printed extents, `frameIndex = 7`, and hashes before recording the current
   renderer's baseline. Every neutral step below repeats that exact count.
3. **Payload plumbing, behavior-neutral.** Switch
   `drawFrame`/`recordTraceCommandBuffer`/push range to `RaygenPushConstants`; extend
   the shader push block; add `frameCounter` and the `DirectionalSun` value in
   `main.cpp`. The shader ignores the new fields — hash identical to step 2's
   baseline (first oracle).
4. **Routing ABI 1 → 2, behavior-neutral.** Export the count and both semantic
   indices from CMake to C++/Slang; update guards; add `shadowMissMain` /
   `shadowAnyHitMain` entry points (uncalled); expand stages/groups/SBT writes and
   assertions; use the named indices at every trace/record site; and update newly
   false in-code comments. Hash identical again (second oracle), isolating surviving
   radiance routing and pipeline/SBT construction. While the new entry points remain
   uncalled, neither the hash nor GPU-AV validates miss slot 1 or hit groups 5/6;
   shadow routing is first exercised, and GPU-AV re-run, at step 6.
5. **Shading moves to raygen, look-preserving.** Restructure `SurfaceHit` (including
   raw geometric/shading normals); establish the orientation helper and the iterative
   kernel with `MaxPathVertices = 1`; `closestHitMain`/`missMain` return hit info /
   dark-red-plus-flag; raygen reproduces today's
   `baseColor × (0.2 + 0.8·|N·V|)`. Require the same RGBA8 hash as step 2; a changed
   hash means at least one quantized output byte changed and is a regression to
   diagnose, not an automatically accepted "last ulp" rebaseline.
6. **Sun + hard shadows + sky.** Direct Lambertian sun term with the shadow
   `TraceRay`, sky in the radiance miss. Image changes deliberately; check shadow
   direction, crate tracking, and the configured leaf cutouts; tune `DefaultSun`
   against the 8-bit clamp. Re-run plain + GPU-AV validation in the configured leaf
   build — these are the first frames that actually fetch miss record 1 and hit
   groups 5/6, including the alpha-tested shadow path.
7. **Deterministic indirect bounce.** Raise `MaxPathVertices` to 2, seed the RNG,
   cosine-sample through the ONB, reject directions below `Ng`, and let the same loop
   handle second-hit sun NEE/sky. Two capture runs at the same successful frame must
   print the same hash.
8. **Acceptance.** Full ctest across `debug` and `ogfx-core`; `release` build + run;
   the three validation passes of §3.8; the yard visual checklist **in the
   configured build — the leaf-cutout shadow is a stated prerequisite for calling
   the slice done** (§3.8), with generated-only as the smoke check. Finish the full
   semantic documentation/comment sweep in §3.2, including README status/CLI,
   ARCHITECTURE ownership/frame/synchronization/pipeline/roadmap sections, FORMATS
   routing, and every affected source comment.

## 4. Risks and open questions

- **8-bit output budget.** Radiance clips at 1.0; badly tuned sun energy flattens lit
  faces. Mitigation: tune constants at step 6; real fix is the HDR target at the
  head of phase 2. Watch for shadow-region banding at 8 bits.
- **1-spp shimmer without accumulation.** Expected and accepted; if it obscures the
  visual check, the oracle path (fixed frameIndex) provides a still image without
  changing the design.
- **Empty shadow-opaque hit group portability.** Spec-legal but less-traveled; if a
  driver misbehaves, the one-line fallback is pointing opaque shadow records at
  `AlphaTestedShadowHitGroup` (its any-hit is suppressed by the opaque BLAS flag
  anyway). Construction is checked under GPU-AV at step 4; the group is first
  actually fetched at step 6.
- **Shadow acne / terminator artifacts.** The fixed 1e-3 geometric-normal offset may
  show acne at grazing sun angles on the 20 m ground plane. The smooth-normal sphere
  — a configured-only exhibit (`XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX`,
  `CMakeLists.txt:562`) — will show the classic shadow-terminator seam in configured
  builds only. The `Ns`/`Ng` hemisphere gates prevent below-surface rays but do not
  solve the visual terminator. Accept for the slice; replace the fixed epsilon with a
  scale-aware robust offset when scene scale or artifact evidence requires it.
- **Capture-hash stability.** The hash pins device + driver + binary — GPU float
  math and Jolt determinism are same-machine guarantees, not portable ones. It is a
  local regression oracle, never a golden file to commit.
- **Payload growth.** The immediate `SurfaceHit` is 16 scalars. Phase 2 explicitly
  adds `materialIndex`; phase 4 explicitly adds evaluated
  emission/conditional-light-PDF data. Those bounded additions are part of the
  pipeline contract. Scalar BRDF records remain raygen-visible so every later
  parameter does not become another payload field.
- **Open (later):** phase 2's fixed `TonemapState` is the initial exposure owner;
  whether exposure becomes a camera property or automatic adaptation is decided
  after HDR output exists, without coupling that decision to temporal history.
