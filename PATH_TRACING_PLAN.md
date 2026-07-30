# xrPhoton Path Tracing Core — Completion Plan

This plan finishes roadmap step 4, **lighting + path tracing**
(`ARCHITECTURE.md:1345-1354`). It succeeds the retired `LIGHTING_PLAN.md`, whose
five-phase milestone landed phases 1–2 in full (sun, hard shadows, procedural sky,
HDR + tonemap, dielectric GGX materials, multi-bounce transport) and left phases 3–4
unbuilt. Phase 5 of that document — temporal accumulation and denoising — is roadmap
step 5 and stays out of scope here; this plan's job is to hand it an honest,
low-variance, material-complete path tracer to filter. The one biased visibility
approximation retained for production usefulness — straight-line shadow attenuation
through intervening glass — is named, bounded, and tested in P3 rather than being
presented as exact refractive transport.

## 1. Where the renderer actually is

The frame already traces a real path: `rayGenMain` runs an eight-vertex loop
with direct delta-sun evaluation and an alpha-aware shadow ray at every vertex,
Lambert + GGX dielectric BRDF evaluation, VNDF lobe sampling with a matching mixture
PDF, and Russian roulette after vertex 3. P1 added one cosine-sampled sky NEE branch
per eligible vertex and power-heuristic MIS against later BSDF sky misses; raygen owns
the procedural gradient evaluation while the miss stage returns only a marker. Linear
radiance lands in an `R16G16B16A16_SFLOAT` image before compute tonemapping into the
presented 8-bit image. Six stages and seven groups are wired, the routing ABI is fixed
at radiance 0 / shadow 1 / `RayTypeCount = 2` (`src/ray_types.hpp`), descriptor
bindings 0–5 are in use, and the pinned frame ABI is an 80-byte view push block plus a
192-byte dynamic `FrameLighting` uniform record.

What is missing is everything that makes the transport *converge* and everything that
makes the material set *complete*:

| Gap | Consequence today |
|---|---|
| No emissive material channel at all | Emitting surfaces cannot be authored, so there is nothing to sample |
| No emitter records or importance distribution | A lamp, a campfire, or an anomaly cannot join the landed sky/emitter selector |
| Dielectric is the only material class | Metals and transmissive glass cannot be represented honestly |
| Sun and gradient sky are static | Time of day, a finite sun disc, penumbrae, and a night gradient are unavailable |
| One material class (opaque dielectric) | `models\mirror`, `models\window`, `models\selflight` classes have no target category (`LIGHTING_PLAN.md` §2.1 audit: 553 window, 74 selflight, 8 mirror references) |
| A static, hard-coded sun and gradient sky | No time of day, no night, no sun disc |
| Pixel-center sampling only | No anti-aliasing, and no jitter for step 5's reprojection to consume |

## 2. Definition of done

The path tracing core is complete when all of the following hold in the shipped
runtime, validated by the acceptance matrix in §9:

1. Every light in the scene — sun, sky, and emissive geometry — is sampled by both NEE
   and BSDF sampling, combined with MIS, with no technique double-counting and no
   technique missing at an ordinary shading vertex. Connections that cross an
   intervening refractive surface use the explicit straight-shadow approximation in §7.1;
   unbiased manifold/specular connections are a tracked non-goal.
2. Emission is a first-class material channel carried by OGFx, decoded, validated,
   assembled, uploaded, and sampled.
3. Many-light selection is a real distribution over emitters, not a loop over all of
   them, and adding emitters changes cost sublinearly.
4. The material classes named by the SoC audit as *core surface transport* — opaque
   dielectric (landed), metal, and transmissive glass — each have a named category, an
   explicit format representation, a tested BSDF, and an acceptance asset. Unknown
   legacy shader classes still fail conversion loudly.
5. The sun and sky come from one time-of-day model with a finite sun disc, so night is
   a state of the same system rather than a different code path.
6. Primary rays sample the full pixel footprint so reference/temporal accumulation
   anti-aliases, the RNG stream stays deterministic and capture-hashable, and a
   white-furnace configuration converges to the environment radiance within tolerance —
   the energy-conservation proof for the whole BSDF set (not for the explicitly
   approximate intervening-glass visibility rule).
7. `maxPipelineRayRecursionDepth` is still 1, `RayTypeCount` is still 2, and no second
   alpha-test, shadow, or material system exists anywhere in the tree.

**Explicit non-goals.** Temporal accumulation, motion vectors, and SVGF (roadmap step
5). ReSTIR DI/GI and a light BVH (tracked follow-ups, §11). Deformable skinning
(remaining step-3 slice). Water, screen-space distortion, fur shells, normal mapping,
and a real mip chain (each a named later material milestone in the §2.1 audit, and
each blocked on inputs this plan does not produce). Dynamic and alpha-tested emitters.
Participating media and volumetrics. Unbiased NEE through intervening refractive
interfaces (manifold NEE or an equivalent specular-connection method).

## 3. Standing decisions this plan inherits and does not revisit

- **Every `TraceRay` originates in raygen.** Recursion depth stays at the
  spec-guaranteed 1 (`src/rt_pipeline.cpp` pipeline create info). Hit shaders return
  data; they never trace.
- **Two ray types, forever, unless a new *traversal semantic* appears.** Emitter NEE,
  sky NEE, and transmissive shadow attenuation all reuse the shadow ray type. Ray
  types index shader records, not light categories.
- **One deterministic RNG stream.** The PCG permutation in `src/lighting.hpp:48-60`
  remains the shared C++/Slang known-answer authority; every new sampling decision
  inside a path draws from the same frame-indexed per-pixel stream so capture hashes
  stay reproducible. P5's frame-global camera jitter is a separately pinned schedule in
  `RaygenPushConstants`; it does not consume or reorder path-stream draws.
- **Closest-hit owns texture sampling and UV gradients; raygen owns shading.** New
  material channels are fetched where the gradients already exist and travel back in
  the payload.
- **Legacy shader classes map to a named category or fail.** No phase may make an
  asset load by silently degrading a transmissive or emissive surface to diffuse.
- **Errors cross boundaries as result objects / `VkResult` / `bool`, cleanup is RAII,
  and comments explain why.** New owners (`GpuLighting`) follow the existing
  destructor-with-idle-wait pattern.

## 4. Phase P0 — Shader module split, dependency trigger, and timing baseline

**Status: complete (2026-07-28).** The monolithic control and imported-module build
produced the same 1920×1080 frame-7 capture hash (`0x9725f7b1e5652acb`) and
byte-identical PPMs. The first fixed-protocol yard baseline, measured on an NVIDIA
GeForce RTX 5070 Ti with driver 595.71.05 in the validation-enabled debug build and
MangoHud disabled, is **0.412 ms median trace time** after 32 warm-up frames over the
next 256 frames. This number is machine/driver/extent specific; later phase comparisons
must use the same conditions. Capture waits for each timestamp pair before advancing,
so this is a serialized trace-dispatch comparison—not interactive frame throughput.

**Why first.** `shaders/raytrace.slang` is 774 lines before this milestone adds three
BSDF classes, a sky model, two NEE estimators, and MIS. One module would become the
largest file in the project and the hardest to review. The split also retires the
architecture trigger that required a Slang depfile as soon as shader imports appeared.

**Work.**

- Split the current implementation into `shaders/sampling.slang` (PCG stream,
  hemisphere sampling, orthonormal basis, MIS weight), `shaders/bsdf.slang` (surface
  orientation and the current dielectric lobes), `shaders/lighting.slang` (current sky
  evaluation), `shaders/records.slang` (the current GPU records and payloads), and
  `shaders/raytrace.slang` (resources, hit-data fetches, entry points, and path loop).
  Later owning phases extend those modules with cone/disk sampling, the material-class
  switch, light records, `FrameLighting`, and the two NEE estimators; P0 does not invent
  their ABI or behavior during an image-preserving refactor.
- CMake: add `-depfile` output to the `slangc` invocation and pass it to
  `add_custom_command(DEPFILE ...)` so edits to any imported module rebuild the
  embedded SPIR-V header. Keep the direct dependency on the root `raytrace.slang`, but
  no longer rely on that dependency alone.
- The ray-type macro definitions (`CMakeLists.txt:512-518, 667-669`) continue to reach
  every module through the same compile-definition list.
- Add the timestamp-query pair described in §9.4 before changing ray cost. A yard
  capture with 32 warm-up and 256 measured frames reports the baseline median used by
  every later phase; shorter captures retain their existing frame-count behavior.

**Acceptance.** Byte-identical rendered output and identical capture hashes before and
after the split (the shader change is a pure refactor); touching any imported module
triggers a rebuild. Timing instrumentation does not change the image, and the fixed
benchmark protocol produces the recorded P0 median.

## 5. Phase P1 — One light authority, sky NEE, and MIS

**Status: complete (2026-07-28).** `SceneLighting` now validates and packs the exact
192-byte block, `GpuLighting` publishes two aligned dynamic-uniform slots after their
fence waits, and the 80-byte push payload is view-only. Raygen performs one sky NEE
sample per eligible surface and power-heuristic MIS against subsequent BSDF sky hits;
the miss stage returns only its marker and the delta sun remains independent. The new
graphics-free `scene_lighting` suite covers selectors, evaluation/sample/PDF round
trips and normalization, MIS edge cases, both ABIs/flags, and alignment/overflow math.
All 21 debug tests and all 7 `ogfx-core` tests pass; emitted SPIR-V validates and pins
the planned member offsets. Plain, GPU-assisted, and synchronization validation are
clean across both frame slots. Radiance and sky-visibility rays share the same infinite
horizon, and the environment's undefined lower hemisphere is explicitly black. The
overlay-free fixed 32+256 capture is 0.582 ms at 1920×1080 versus P0's 0.412 ms
(+0.170 ms, below budget), with final-frame hash `0x5726093472c82064`.

**Goal.** All light state moves to one owner; the sky stops being a BSDF-only lottery;
MIS lands before any content depends on it.

### 5.1 Decisions

**All light state lives in one per-frame lighting block; push constants keep only view
state.** Before P1 the sun rode in the push payload while the sky was hard-coded in
the shader. Once a sky model has coefficients and a light set has
selection weights, keeping the sun in push would split light state across two
mechanisms — precisely the parallel-systems failure the project rejects.
`RaygenPushConstants` therefore shrinks to the 64-byte camera prefix plus
`frameIndex` and reserved view-state scalars: exact offsets 64/68/72/76 are
`uint frameIndex`, `float cameraJitterX`, `float cameraJitterY`, and a zero word, for an
80-byte block. P1 initializes both jitter scalars to zero; P5 gives them meaning. Every
light value moves into a `FrameLighting` uniform block. The camera prefix and its
`offsetof` asserts are preserved byte-for-byte. The
landed directional-light coefficient is renamed from `sunRadiance` to
`sunIrradiance`: the landed delta estimator already treats it as incident irradiance,
and P4 preserves that quantity when the source becomes a finite disc.

**The frame lighting block is one buffer with `MaxFramesInFlight` sub-ranges bound
through a dynamic offset.** This mirrors the landed per-slot mapped TLAS instance
input: the CPU writes only the slot whose fence has retired, so an in-flight trace
never races a host write. A dynamic uniform-buffer descriptor avoids a second
descriptor set and avoids rewriting a set that may be in flight. `GpuLighting` queries
`minUniformBufferOffsetAlignment`, rounds `sizeof(FrameLighting)` up to a per-slot
stride, binds descriptor offset 0/range `sizeof(FrameLighting)`, and passes
`frameSlot * stride` as the one checked 32-bit dynamic offset. The allocation size and
offset multiplication use checked `VkDeviceSize` arithmetic.

**The sun is evaluated at every vertex; exactly one non-delta light sample is drawn per
vertex.** The sun is a delta light — MIS cannot help it and a selection probability
would only add variance to the term that dominates daylight. The non-delta techniques
(sky now, emitters in P2) share one selector sample chosen by CPU-computed branch
probabilities. Worst-case shadow-ray budget per path vertex is therefore exactly 2,
which is the number step 5 must plan around.

**Sky NEE uses cosine-weighted hemisphere sampling, not a tabulated environment
distribution.** The analytic sky is smooth and low-contrast away from the sun disc
(which is a separate explicit light), so a cosine-weighted sample around the shading
normal already has bounded variance, costs no table, and needs no rebuild when time of
day changes. The upgrade trigger is explicit: **when an environment cubemap (the SoC
weather sky set) or a measured-variance need arrives, replace this with a 2D
piecewise-constant marginal/conditional distribution over the sky.** Until then, do
not build the table.

**MIS uses the power heuristic with exponent 2 everywhere.** A technique whose density
is zero or absent gets weight 1. One heuristic, one exponent, one shared function in
`shaders/sampling.slang`.

### 5.2 New units

- `src/scene_lighting.hpp` / `.cpp` — Vulkan-free `SceneLighting`: the mutable
  analytic sun, the sky parameters, and (from P2) the immutable static-light records
  plus their power distribution. Lives in `xrPhotonOgfxRuntime` so it is headless-
  testable. It computes the per-frame branch probabilities and packs the
  `FrameLighting` block; it owns no GPU state.
- `src/gpu_lighting.hpp` / `.cpp` — engine-side RAII `GpuLighting`: the per-slot
  uniform buffer (host-visible, coherent, persistently mapped), and from P2 the
  device-local light-record, CDF, and emitter-lookup buffers. Borrows device and
  allocator, waits idle in its destructor, and reports teardown like the other owners.
- `tests/scene_lighting_tests.cpp` → CTest name `scene_lighting`, registered beside the
  existing `lighting` suite (`CMakeLists.txt:619-627`).

### 5.3 ABI

`FrameLighting` is an exact 192-byte, `alignas(16)`, std140-compatible record, pinned by
exhaustive `sizeof`/`offsetof` assertions in `src/scene_lighting.hpp` exactly as the push
payload is today. Fields reserved for later phases are present and zero-initialized from
P1:

| Offset | Field | Meaning |
|---|---|---|
| 0 | `float3 sunDirection`, `float sunCosineHalfAngle` | Toward the sun; disc half-angle cosine (P4; 1.0 = delta until then) |
| 16 | `float3 sunIrradiance`, `float sunSolidAngle` | Incident irradiance; all-zero RGB disables the sun. Solid angle is 0 for the P1 delta sun |
| 32 | `float3 skyZenith`, `float pSky` | Gradient/Preetham zenith value and sky probability in the shared sky/emitter selector |
| 48 | `float3 skyHorizon`, `float pEmitters` | P1 gradient/P4 night horizon and emitter probability; `pSky + pEmitters == 1` when that selector is enabled |
| 64 | `uint lightCount`, `uint instanceCount`, `float totalLightPower`, `uint flags` | Published light/TLAS-instance counts and mode/debug bits |
| 80 | `float4 skyPerezA` | P4 coefficient RGB in `.xyz`; `.w = 0` |
| 96 | `float4 skyPerezB` | P4 coefficient RGB in `.xyz`; `.w = 0` |
| 112 | `float4 skyPerezC` | P4 coefficient RGB in `.xyz`; `.w = 0` |
| 128 | `float4 skyPerezD` | P4 coefficient RGB in `.xyz`; `.w = 0` |
| 144 | `float4 skyPerezE` | P4 coefficient RGB in `.xyz`; `.w = 0` |
| 160 | `float4 nightZenith` | P4 night-gradient zenith RGB in `.xyz`; `.w = 0` |
| 176 | `float daylightBlend`, `uint reserved0`, `uint reserved1`, `uint reserved2` | P4 daylight weight in [0, 1]; reserved words are zero |

`flags` is also pinned: bit 0 = Perez daylight fields active, bit 1 = scene contains
Glass, and bits 2–3 = estimator mode (`0` MIS, `1` NEE-only, `2` BSDF-only, `3`
invalid). All upper bits must be zero. P1 publishes zero, and every later producer
validates the complete word before the CPU or shader consumes it.

Descriptor bindings grow from 5 to 6: **binding 5** = `FrameLighting`
(`UNIFORM_BUFFER_DYNAMIC`, raygen + closest-hit visible). Bindings 6–8 are reserved in
this phase's layout comment and filled by P2.

### 5.4 Shader work

- `skyRadiance` moves to `shaders/lighting.slang` and gains a matching `skyPdf` plus a
  `sampleSky` that returns direction, radiance, and density — the shared-function
  discipline the retired plan required from the start.
- The radiance miss shader returns only the miss marker. Raygen evaluates the sky from
  the missed ray direction, which keeps all radiance arithmetic in raygen and makes
  binding 5's raygen/closest-hit stage mask complete; no miss shader reads the block.
- The path loop draws one non-delta sample per vertex, traces the finite/infinite
  shadow ray for it, and MIS-weights it against the BSDF density; the subsequent
  BSDF-sampled ray that misses into the sky is MIS-weighted against `skyPdf`. The sun
  term is unchanged and unweighted.

### 5.5 Acceptance

- Headless: sky-selector enabled/disabled and zero-power cases (including proof that sun
  state does not change the separate selector), sky evaluation/PDF/sample round trip
  (density integrates to 1 over the hemisphere within tolerance by fixed-seed
  quadrature), power-heuristic weights (symmetry, zero-density operand, both-zero), the
  exact 80/192-byte ABI layouts and flag validation, and dynamic-stride/offset math at
  representative device alignments and overflow bounds.
- Runtime: an interior/shadowed region of the configured yard shows a visible variance
  reduction at fixed spp versus the P0 build; open sunlit regions are unchanged in
  mean. Capture hashes are re-pinned (they change by construction).
- Validation: plain, GPU-assisted, and synchronization validation clean over the new
  dynamic-offset binding across resize and both frame slots.

## 6. Phase P2 — Emissive geometry, emitter NEE, and the night yard

**P2a status: complete (2026-07-29).** `OGFX_MATERIALS` v3 now carries validated
nonnegative emission RGB in a 48-byte record while v1/v2 retain their 32-byte stride
and decode to zero emission. The canonical writer selects the lowest sufficient
version, both decoder profiles and the runtime loader preserve emission, scene
assembly rejects invalid values transactionally, and the CPU/Slang `MaterialRecord`
ABI is 48 bytes with an offset-44 zero reservation. All 7 graphics-free and 21 debug
tests pass; SPIR-V validation pins offsets 32/44 and `ArrayStride 48`. Because no
shader consumes emission until P2c, the Phase-1 frame-7 hash remains
`0xd6550332dd29cf6c`.

**P2b status: complete (2026-07-29).** The Vulkan-free builder now rejects dynamic or
alpha-tested emitters, expands every supported static placement into exact 64-byte
world-space triangle records, constructs the `area × luminance` float CDF, and emits
the bounded 16-byte instance/geometry reverse lookup. `GpuLighting` stages bindings
6–8 into device-local storage with valid zero-emitter sentinels, while `FrameLighting`
publishes light count, total power, and normalized sky/emitter branch probabilities.
Headless coverage pins record ABIs, equal/unequal distributions, repeated placements,
zero emitters, malformed bounds, rejection policy, and selector modes. P2c remains the
first shader consumer, so P2b deliberately preserves the P1 image. All 7 graphics-free
and 21 debug/release tests pass, SPIR-V validation is clean, and the matching
validation-enabled frame-7 capture retains hash `0xd6550332dd29cf6c`.

**P2c status: complete (2026-07-29).** Raygen now samples power-selected emitting
triangles with finite one-sided visibility segments, closest-hit resolves BSDF-hit
emission through the bounded instance/geometry lookup, and power-heuristic MIS combines
the two techniques. The shared scene selector adds a 21-placement / 42-triangle night
yard. Frozen-scene linear-HDR reference capture and its MIS, NEE-only, and BSDF-only
controls pass the pinned 256-sample pairwise agreement gate in every region/channel.
All 7 graphics-free and 21 debug/release tests pass; SPIR-V and validation-enabled yard
and night captures are clean. At 1920×1080 on the RTX 5070 Ti, fixed 32+256 trace-only
medians are 0.600 ms for the yard and 0.676 ms for the night preset; frame-index-7
hashes are `0xa2418da72c12eb9d` and `0x859a09157a88be57`, respectively.

**Goal.** Surfaces can emit; emitters are importance-sampled from a real distribution;
a BSDF ray that lands on an emitter is MIS-weighted instead of double-counted.

### 6.1 Format: `OGFX_MATERIALS` version 3

The 32-byte v1/v2 record (`FORMATS.md:457-500`) grows to a **48-byte v3 record**:
bytes 0–31 keep their v2 meanings; offsets 32/36/40 are finite, nonnegative emission
RGB; offset 44 is reserved zero (P3 gives it meaning). Versions 1 and 2 decode to zero
emission. The canonical writer emits the **lowest sufficient version** — v1 for legacy
defaults (preserving existing canonical bytes byte-for-byte), v2 for nondefault
roughness/F0 with zero emission, v3 when any emission is nonzero. Compiler model,
writer, schema decoder, runtime decoder, `ogfx_loader`, `scene_assembly` validation,
`FORMATS.md`, and the v1/v2/v3 compatibility tests land in one commit; a partially
migrated format is a decoder that accepts bytes it cannot mean.

`SceneMaterial` (`src/scene.hpp:37-47`) gains `float emission[3]`. `MaterialRecord`
(`src/gpu_scene.hpp:37-50`) grows 32 → 48 bytes with emission at offset 32 and the
class word at 44 reserved zero, keeping its `offsetof` assert block exhaustive.

### 6.2 The light builder is scene *policy*, not scene assembly

A Vulkan-free `buildSceneLighting(SceneData, dynamicInstances)` in
`src/scene_lighting.cpp` extracts emitters. It must receive the dynamic-instance set
explicitly because `SceneData` alone cannot express mobility; generic
`scene_assembly` therefore cannot own this. `main()` calls it after
`loadGalleryScene` and before `createGpuScene`/`createGpuLighting`, and a future level
owner supplies the same two inputs.

Rejected with a loud error rather than silently skipped: emission on a dynamic
placement, and emission on an alpha-tested geometry (a full-triangle area PDF would be
sampling an unknown emitting area). Both are tracked follow-ups (§11).

**Emissiveness is per geometry**, because materials are per geometry
(`src/scene.hpp:25-35`). That makes the reverse lookup exact and cheap:

```
instanceHeader = emitterLookup[InstanceIndex()]
range = emitterLookup[instanceHeader.first + GeometryIndex()]
lightIndex = range.first + PrimitiveIndex()
```

`InstanceID()` deliberately stays `mesh.firstGeometry` for the existing geometry-record
lookup and cannot distinguish repeated placements, so `InstanceIndex()` is the correct
key. Binding 8 is one precisely packed `EmitterLookupRecord[]`; each 16-byte record is
four `uint`s. Records `[0, instanceCount)` are headers whose first/count words are the
first range-record index and geometry count. The remaining records are one range per
instance-geometry pair, with first/count meaning first light and primitive count;
`UINT32_MAX, 0` is the non-emitting sentinel. Header and range reserved words are zero.
The CPU validates every addition, count, and referenced range before upload. Repeated
placements receive distinct range records because their world-space triangles and
light indices differ, while their BLAS remains shared. Closest-hit defensively checks
`InstanceIndex() < instanceCount`, `GeometryIndex() < header.count`, the non-emitting
sentinel, and `PrimitiveIndex() < range.count` before reading a light; failure returns
zero emission rather than indexing an invalid device buffer.

### 6.3 Light records and selection

`LightRecord` — 64 bytes, world-space and immutable (static emitters only):

| Offset | Field |
|---|---|
| 0 | `float3 v0`, `float area` |
| 16 | `float3 edge1`, `float pTriangle` (conditional selection probability) |
| 32 | `float3 edge2`, `uint flags` (zero; P2 emitters are one-sided by winding) |
| 48 | `float3 emission`, `uint materialIndex` |

Selection is a **power-weighted CDF** over emitting triangles with weight
`area × luminance(emission)`, binary-searched in the shader. Not an alias table: the
build is simpler, thousands of entries search in ~11 steps, and the tracked upgrade
(ReSTIR DI plus a light BVH) replaces the whole selector rather than the table type.
Buffers: **binding 6** = `LightRecord[]` (raygen + closest-hit), **binding 7** = the
monotonic `float` CDF (raygen), **binding 8** = `EmitterLookupRecord[]` (closest-hit),
all device-local and uploaded once through the existing staged path. With zero emitters,
`GpuLighting` still binds one valid sentinel allocation per binding while publishing
`lightCount = 0`; the light/CDF sentinels are never indexed. The lookup still contains
valid instance headers/ranges so closest-hit can take the same bounded path in every
scene.

### 6.4 Estimators

**NEE toward an emitter** uses a *finite* visibility segment, never the sun's infinite
range: from the offset surface origin, compute `wi` and distance to the sampled
triangle point, reject a degenerate segment, and trace with
`TMax = distance - max(1e-3, 1e-4 * distance)`. This endpoint exclusion keeps both the
selected emitter and the geometry immediately behind it from being reported as
blockers. Emission is one-sided in P2: a sampled point with
`dot(Ng_light, -wi) <= 0` contributes zero before tracing a shadow ray.

**BSDF ray hitting an emitter**: closest-hit returns evaluated emission plus everything
needed for the competing light density except the mutable branch probability —
`pTriangle`, `area`, and the emitter's geometric normal. Raygen forms

```
cosLight = dot(Ng_light, -wi)
pLight = cosLight > 0
    ? pEmitters * pTriangle * (1 / area) * distance² / cosLight
    : 0
```

and MIS-weights the emission against the BSDF density it actually sampled. Back-facing
or grazing emitters evaluate to zero emission and zero competing light density. The
delta sun never enters any MIS balance.

### 6.5 The night yard

A code-owned night variant of the temporary gallery policy (`src/gallery.cpp`): the
same yard geometry, sun irradiance zero, a dim night sky, and ~20 small emitters —
lamps on the wall and platform, a campfire-scale emitter, and a cluster of
anomaly-scale emitters — selected by a runtime option routed through the existing
capture/gallery seam (§9.3). This is the standing many-light acceptance scene and,
deliberately, the noisiest content in the project: step 5's denoiser must be built
against it, so it exists before the denoiser does.

### 6.6 Acceptance

- Headless: v1/v2 → zero-emission decode; v3 round trip; lowest-sufficient-version
  writer selection; emission validation (finite, nonnegative) rejection cases; builder
  rejection of dynamic and alpha-tested emitters; CDF construction on equal-power
  (uniform expectation) and unequal-power arrays; lookup-table construction over
  repeated placements of a shared mesh, every sentinel/bounds failure, and zero-emitter
  scenes; sky-only/emitter-only/mixed selector normalization; the `pLight` formula as a
  pure function.
- Runtime: night yard renders with all emitters visibly contributing at 1 spp; an
  emitter viewed directly and its illumination of a nearby surface are consistent
  (no double-count, no missing energy). P2c adds the linear-HDR reference-capture path
  specified in §9.2 and its `NEE-only`, `BSDF-only`, and `MIS` controls; double-precision
  CPU accumulations of all three must agree in each predeclared lit region. The gate is
  `abs(meanA - meanB) <= max(0.01 * max(abs(meanA), abs(meanB)), 3 * combinedStandardError)`
  for every pair and RGB channel; sample count and regions are pinned fixtures.

## 7. Phase P3 — Metal and transmissive material classes

**Goal.** Close the material-class gap named at `ARCHITECTURE.md:1350-1351` and give
the audited `models\mirror` / `models\window` / `models\glass` families a real target.

### 7.1 Decisions

**An explicit material class enum, not a metallic-factor blend.** The SoC audit fixed
the import rule: opaque GGX, cutout, thin transmission, water, emission, fur, and
distortion are *distinct categories*, and an unknown legacy class must fail. A scalar
metalness would erase that distinction at the format boundary and invite "load it as
mostly-metal" guesses. `OGFX_MATERIALS` **version 4** gives the v3 reserved word at
offset 44 its meaning: `0 = Dielectric` (landed), `1 = Metal`, `2 = Glass`. Record
stride stays 48 bytes; v1–v3 decode to `Dielectric`.

**Metal reuses base color as spectral F0.** A conductor has no diffuse lobe, so the
existing `baseColorFactor × sampled texture` becomes normal-incidence reflectance and
`dielectricF0` is ignored. No new field, no new texture channel; roughness already
exists. Acceptance: a low-roughness and a high-roughness Blender sphere pair, then the
`wpn_binoculars` mirror-class asset once its source profile is supported.

**Glass is a rough dielectric interface with a fixed IOR of 1.5.** SoC glass is window
glass and bottles; a per-material IOR would add a format field with one value in every
shipped asset. The constant is documented as per-class and becomes a field the day an
asset needs otherwise. The lobe pair is Fresnel-weighted specular reflection plus
specular transmission through the same GGX microfacet distribution, tinted by base
color on the transmitted lobe. The unflipped geometric normal determines whether the
interface is air→glass or glass→air; the BTDF and its PDF include the matching
radiance-mode eta/Jacobian factors and total internal reflection. Nested dielectric
media need a medium stack and stay out of scope.

**Direct-light sampling covers both sides of glass.** At a Glass shading vertex,
`sampleSky` is an equal-probability mixture of cosine-weighted hemispheres on both sides
of the oriented normal, with PDF `0.5 * abs(dot(N, wi)) / pi`; the opaque classes keep
the one-hemisphere distribution. Sun and emitter NEE remove the landed opaque-only
positive-cosine gate, evaluate the selected BSDF hemisphere, and offset the visibility
origin to that side. Thus sky, sun, and emitters remain reachable by NEE as well as by
BSDF sampling after transmission lands.

**Shadow rays use one explicit straight-line approximation through intervening
glass.** Exact NEE through a refractive interface requires solving a specular connection
and is outside this milestone. Blocking the ray would make ordinary window interiors
black, so the accepted approximation keeps the connection direction, multiplies
attenuation by the material tint and the angle-dependent `(1 - Fresnel)` term, and
continues. It is deliberately not used to validate BSDF energy. `ShadowPayload` grows a
`float3 attenuation` initialized to 1 alongside `visibility` initialized to 0; the
glass shadow any-hit updates attenuation and calls `IgnoreHit()`. Opaque ranges commit a
hit, so the miss shader never runs and visibility stays 0; shadow miss sets visibility
to 1 and the estimator multiplies the two. Scenes containing glass drop
`RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`; scenes without glass keep the landed fast
path. This is one shadow system with a scene-wide traversal choice, not a second
estimator.

**Glass is routed to any-hit explicitly.** The padding word at offset 28 of the GPU-only
`GeometryRecord` becomes a flags word whose bit 0 means alpha-tested. BLAS geometry is
opaque only when `!alphaTested && materialClass != Glass`; otherwise it leaves
`VK_GEOMETRY_OPAQUE_BIT_KHR` clear. The existing alpha-capable SBT groups are renamed
any-hit-capable groups and selected by that same predicate, so the stage/group and ray-
type counts do not grow. Radiance any-hit performs alpha rejection only when bit 0 is
set and otherwise accepts the glass candidate; shadow any-hit performs that same alpha
test, then the Glass attenuation rule. The format/assembly boundary rejects the
unsupported `alphaTested && Glass` combination.

### 7.2 Work

- Format/compiler/decoder/loader/assembly/tests for v4, atomically, as in P2.
- `shaders/bsdf.slang` exposes exactly three functions — `evaluateBsdf`, `sampleBsdf`,
  `bsdfPdf` — each switching on the class. No Slang interfaces or generics until a
  second consumer module exists.
- Acceleration-structure opacity, SBT group selection, `GeometryRecord.flags`, and both
  unified any-hit entry points change atomically with the Glass class; no intermediate
  commit can classify glass while still bypassing its any-hit behavior.
- The path loop learns that a sampled transmissive direction may cross the surface:
  the geometric-normal side test that currently terminates a path
  (`shaders/raytrace.slang:672-673`) becomes a signed test against the sampled lobe's
  expected hemisphere, and the ray offset flips sign for transmission.
- Sky/sun/emitter NEE and their competing PDFs gain the two-sided Glass behavior above
  in the same commit as the BTDF.
- Legacy and Blender adapters map their accepted shader classes to the new categories;
  every unmapped class stays a loud conversion failure.

### 7.3 Acceptance

- Headless: v4 round trip and version selection; class/alpha combination validation;
  per-class BSDF reciprocity, positivity, entering/exiting eta cases, total internal
  reflection, and PDF/sample agreement as pure-function tests where the math is shared;
  known-angle tests pin the approximate shadow attenuation separately; traversal-class
  tests prove every Glass geometry clears the BLAS opaque flag and selects the unified
  any-hit SBT record while ordinary opaque geometry retains the fast path.
- Runtime: metal sphere pair shows correct tinted specular with no diffuse floor; a
  glass panel between the sun and a floor produces a tinted lit patch rather than a
  black shadow; the furnace test (§9.2) passes for all three classes.
- Perf: after 32 warm-up frames, the median trace time over the next 256 fixed capture
  frames is measured before/after P3b on the same machine, driver, extent, and glass
  acceptance scene and recorded in `ARCHITECTURE.md`. More than 25% over P3a blocks the
  phase; the no-glass yard must retain the early-exit fast path and remain within 5%.

## 8. Phase P4 — Time-varying sun and sky with a finite sun disc

**Goal.** One model produces the sun direction, sun irradiance, sky radiance, and night
— replacing `DefaultSceneLighting`'s constant analytic values and the fixed gradient.

**Decisions.**

- **Preetham analytic sky, coefficients computed on the CPU per frame.** Five
  coefficients per channel plus zenith terms are a closed form in turbidity and sun
  zenith and occupy the exact `skyPerezA`–`E`/`skyZenith` fields reserved in
  `FrameLighting` (§5.3); `skyHorizon`/`nightZenith` hold the night gradient and
  `daylightBlend` selects the continuous result. Their small CPU evaluation requires no
  GPU table rebuild. Hosek-Wilkie's larger fitted tables are the upgrade trigger if
  the sky's appearance becomes an acceptance blocker; a cubemap environment is the
  other trigger, and it is the one that also promotes sky sampling to a real 2D
  distribution.
- **Preetham owns daylight, not an extrapolation below its domain.** From solar elevation
  0° through -6° civil twilight, `SceneLighting` continuously fades the daylight
  coefficients into an authored dim night gradient; below -6° it evaluates only that
  gradient and disables the sun. Bit 0 of `FrameLighting.flags` selects the Perez-capable
  evaluator, but both states use the same `skyRadiance` entry point and the same NEE/PDF
  contract.
- **One shared evaluation function, mirrored on the CPU.** `SceneLighting` holds the
  C++ reference implementation and `shaders/lighting.slang` the GPU one, pinned
  together by a known-answer test at fixed sun elevations — the same discipline the
  PCG stream already uses (`src/lighting.hpp:48-60`, `tests/lighting_tests.cpp`).
- **Time of day is a plain float driven by `main()`,** with a fixed latitude/date
  constant converting it to a sun direction. A weather/level owner replaces that
  driver later; the ABI does not change when it does.
- **The sun becomes a cone light with a finite half-angle** (~0.27° for the real solar
  disc, authorable). `sunIrradiance` keeps the P1 incident-energy meaning. For nonzero
  solid angle, the uniform-cone estimator evaluates disc radiance as
  `sunIrradiance / sunSolidAngle`, so its integral and apparent brightness remain
  continuous as disc size changes; the analytic sky excludes the separate sun disc.
  A BSDF ray landing inside the disc is MIS-weighted against the cone density. The sun
  remains its own one-sample technique rather than entering the sky/emitter selector,
  so `pSky + pEmitters` retains its stated meaning and the two-shadow-ray budget holds.
  At exactly zero solid angle the code takes the original delta estimator without a
  division and with MIS weight 1.

**Acceptance.** A time-of-day sweep from dawn through noon, the daylight/night blend,
and full night renders without a radiance discontinuity; changing disc size preserves
mean incident sun energy while penumbrae widen; the zero-angle image matches the P1
delta path; at night the sun branch disables cleanly and emitters carry the image;
CPU/GPU sky known-answer tests agree to tolerance.

## 9. Phase P5 — Sampling quality, energy proof, and acceptance instrumentation

### 9.1 Sub-pixel sampling

Raygen currently fires through the pixel center (`shaders/raytrace.slang:531`). Add one
**frame-global** jitter shared by all pixels, selected from a pinned, fixed-seed
permutation of a 4×4 stratification by `frameIndex % 16`. Every consecutive 16-frame
reference accumulation therefore covers the pixel evenly, and step 5 can consume the
same camera jitter without a full-resolution jitter history. The 16 cell centers are
mapped to `[-0.5, 0.5)` pixel units and published in the two reserved P1 view-state
scalars of `RaygenPushConstants`; the future reprojection pass receives the same pair
from the frame owner. The per-pixel PCG path stream stays separate and deterministic.
The neighbor directions
feeding the UV-gradient footprint
(`shaders/raytrace.slang:548-555`) shift with the jittered center so the footprint
stays correct. This is the one place this plan deliberately builds *for* the denoiser.

### 9.2 Linear-HDR reference capture and the furnace proof

P2c adds an acceptance-only reference mode alongside ordinary capture:
`--reference <sample-count> --scene <yard|night|furnace> --estimator
<mis|nee|bsdf>`. After every successful frame it reads the untouched
`R16G16B16A16_SFLOAT` HDR image, converts half values to float, and accumulates each
channel into double-precision CPU sums. It never feeds the sum back to the renderer, so
this is offline measurement rather than roadmap-step-5 temporal accumulation. Ordinary
`--capture` continues to read only the final tonemapped RGBA8 image and retains its
pinned hash contract. Reference mode fixes camera, time of day, and scene transforms
for the complete sample set; it increments only `frameIndex` and does not advance
physics between samples, so all estimators measure the same integrand.

Estimator controls are mode bits in `FrameLighting.flags`: `bsdf` disables non-delta
NEE but keeps BSDF-hit/miss emission; `nee` keeps BSDF continuation for indirect paths
but suppresses non-camera BSDF-hit/miss light contributions; `mis` enables both and
their weights. Delta events that have no competing technique retain weight 1. Fixed
seeds, sample counts, image regions, tolerances, and confidence calculation live in the
capture-runtime proof so a visual judgment cannot move the gate.

A configuration with a constant environment radiance `L`, no sun, no emitters, and
white-albedo materials must converge to `L` on every visible surface, for every
material class and every roughness. This is the single strongest correctness gate for
the BSDF set, and it catches exactly the failures MIS and NEE otherwise hide: a
mismatched PDF, a missing Fresnel/eta term on transmission, or a lobe-probability
normalization error. The furnace preset assigns a fixed image region to each
class/roughness case; reference mode requires every RGB ratio to satisfy
`abs(mean / L - 1) <= max(0.02, 3 * standardError / L)` and reports a hard pass/fail.
Tonemapped LDR values are never used for this proof.

### 9.3 One selector for the code-owned scene presets

P2's night yard and P5's furnace scene are both temporary gallery policy, and they must
not each grow their own entry point. P2 introduces one `--scene <yard|night>` option;
P5 extends its validated enum to `<yard|night|furnace>`. The option is accepted by
interactive, ordinary-capture, and reference modes and consumed by `loadGalleryScene`
and `SceneLighting`; the default stays the current yard. The whole selector retires
with the gallery when level data has a real owner.

### 9.4 Cost instrumentation

The GPU timestamp-query pair lands in P0, around the trace dispatch only. Capture and
reference modes convert ticks with the device timestamp period. A comparable benchmark
run renders 32 warm-up frames followed by 256 measured frames and reports their median
alongside the existing extent/frame-count summary; shorter ordinary captures still
work, but label their timing as diagnostic rather than comparable. Every phase from P1
on records comparable trace cost in `ARCHITECTURE.md`, so the §7.1 shadow-ray trade-off
and step 5's budget have a baseline that predates the work they measure.

### 9.5 Standing validation for every phase

- `ctest --preset ogfx-core` for the graphics-free suites and `ctest --test-dir
  build/debug` for the full set; every new suite is registered in both the list and
  `ARCHITECTURE.md`.
- Plain, GPU-assisted, and synchronization validation clean over live motion, resize,
  camera-mode switching, and teardown — run with `DISABLE_MANGOHUD=1`.
- Capture hashes re-pinned deliberately at each phase that changes the image, with the
  reason recorded in the commit.

## 10. Implementation order

Each step builds, runs, and is independently reviewable. Format changes are atomic
across compiler, writer, both decoders, loader, assembly, documentation, and tests.

1. **P0** timestamp baseline, shader split, and `slangc` depfile — image refactor only,
   hashes unchanged.
2. **P1a** `SceneLighting` / `GpuLighting` / `FrameLighting` + push shrink; sun moves
   off push constants. Image unchanged in mean; hashes re-pinned.
3. **P1b** sky sampling, sky PDF, MIS. First variance win.
4. **P2a — complete.** `OGFX_MATERIALS` v3 + emission through the whole chain,
   GPU record 32 → 48.
   No shading change yet (emission unreferenced).
5. **P2b — complete.** Light builder, records, CDF, emitter lookup, bindings 6–8.
6. **P2c — complete.** Emitter NEE + emitter MIS + the night yard preset + linear-HDR reference
   capture and estimator controls.
7. **P3a** `OGFX_MATERIALS` v4 class word + metal class + sphere acceptance.
8. **P3b** glass class + two-sided direct sampling + unified any-hit routing + explicit
   intervening-glass shadow approximation + perf measurement.
9. **P4** Preetham sky, time of day, sun disc, cone sampling.
10. **P5** frame-global pixel jitter, furnace preset/proof, scene selector, final
    acceptance matrix.

Documentation lands with the code it describes: `ARCHITECTURE.md` gains a **Lighting**
subsystem section (owners, bindings, per-frame lighting write, the estimator set) and
its Status/Roadmap entries move with each phase; `FORMATS.md` gains v3 and v4 in the
same commits as the decoders; `SDK.md` is untouched by this milestone.

## 11. Tracked follow-ups (deliberately not in this plan)

- **ReSTIR DI, then a light BVH / light cuts.** The named upgrade path for many-light
  selection once the flat power CDF is measurably the bottleneck.
- **Dynamic emitters and alpha-tested emitters.** Both need a light set that updates
  per frame and, for cutouts, an emitting-area model the current full-triangle PDF
  cannot express.
- **Two-sided emitters.** The P2 light set is one-sided by winding; authorable
  sidedness needs an OGFx/scene representation before the reserved light flag can be
  enabled.
- **Unbiased light connections through refractive interfaces and nested dielectrics.**
  Manifold NEE (or an equivalent specular-connection method) replaces P3's named
  straight-line shadow approximation; a medium stack handles nested IOR transitions.
- **Environment cubemap + 2D sky importance sampling.** The trigger that also promotes
  the sky from cosine-hemisphere sampling.
- **Water, distortion, fur, normal mapping, a real mip chain.** Named material
  milestones from the SoC audit; each needs inputs this plan does not produce.
- **Physics interpolation and motion vectors.** Fires with roadmap step 5, as already
  recorded at `ARCHITECTURE.md:1367-1370`.

## 12. Risks

- **Shadow-ray cost after P3b.** Losing the first-hit early exit in scenes containing
  glass is the one change in this plan that can cost double-digit percent. Mitigation:
  P0 supplies the baseline, P3b has an explicit 25% gate, and a scene-wide `HasGlass`
  flag keeps `ACCEPT_FIRST_HIT_AND_END_SEARCH` for scenes without transmissive geometry.
- **Intervening-glass visibility is biased.** The straight-line rule cannot reproduce a
  refracted specular connection. Mitigation: it is isolated to shadow visibility,
  tested separately from the BSDF furnace, named in the definition of done, and has a
  tracked replacement rather than silently passing as exact transport.
- **Format churn.** Two material versions land in one milestone. Mitigation: the
  lowest-sufficient-version writer rule keeps every existing canonical asset
  byte-identical, and each version ships with explicit older-version decode tests.
- **Push-constant migration.** Moving the sun off push touches a pinned ABI with
  existing asserts and tests. Mitigation: it happens in one isolated step (P1a) whose
  only expected visual change is none.
- **Night-yard authoring.** The many-light acceptance scene is code-owned placement
  policy, so a bad layout produces a misleading acceptance signal. Mitigation: pin the
  night layout in the `gallery_policy` suite the way the day yard already is.
- **MIS correctness is easy to get subtly wrong.** Mitigation: the NEE-only /
  BSDF-only / MIS three-way mean-agreement check in §6.6 is the gate, and it is cheap
  to run at high spp through linear-HDR reference mode.
