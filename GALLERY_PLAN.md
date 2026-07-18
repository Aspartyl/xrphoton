# xrPhoton — OGFx Gallery Milestone Plan: Two Models, N BLASes, First Textures

This is the implementation plan for the next milestone after M4a: render the
build-generated `test_quad.ogfx` and the real legacy-converted `plitka1.ogfx`
**together**, side by side, through one generic runtime path — the beginning of
a persistent **OGFx gallery**. It is the companion to
[GEOMETRY_PLAN.md](GEOMETRY_PLAN.md) (whose format-independent N-BLAS, SBT, and
texture reference designs this plan instantiates) and is subordinate to
[FORMATS.md](FORMATS.md) for format contracts and to
[ARCHITECTURE.md](ARCHITECTURE.md) for landed runtime architecture. Where those
documents already made a decision, this plan applies it; new decisions are
recorded below in the D-record style so each question stays closed.

## 1. Goal and scope

Load **two independent OGFx models** into one preview scene and render them
beside each other with code-owned preview transforms:

1. The build-generated `test_quad.ogfx` (already rendered today).
2. The real converted `plitka1.ogfx` (M4a's offline proof output), including
   its base-color texture `ston\ston_stena_marbl_m_03_back`.

Delivering that visibly requires four capability steps, each already sketched
as a reference design in GEOMETRY_PLAN.md and FORMATS.md:

- expanding the strict M4 runtime decode profile to the textured static schema,
- a generic multi-model CPU scene-assembly path,
- the N-BLAS / N-instance acceleration-structure generalization,
- the minimum opaque base-color texture path (resolve → decode → upload →
  sample).

**Delivery discipline (inherited from GEOMETRY_PLAN §1):** every phase below is
one reviewable commit that builds under all three presets (`debug`, `release`,
`ogfx-core`), passes every CTest suite, and leaves the app rendering something
on-screen-verifiable with clean validation. The owner commits each phase
themselves.

### The core architectural rule

**The runtime must not care where an OGFx file originated.** Generated,
legacy-converted, and (later) Blender-exported files all travel through the
same OGFx runtime decoder, the same `SceneData` representation, the same GPU
upload path, the same BLAS/TLAS construction, the same material/texture system,
and the same shaders. There is no `plitka1`-specific runtime logic anywhere in
this plan; source-specific behavior lives in offline adapters only
([src/legacy_ogf.cpp](src/legacy_ogf.cpp) today, the Blender exporter later).
Every phase below must be reviewable against this rule: if a change would only
make sense for one gallery entry, it is in the wrong layer.

### Why this milestone reorders the roadmap

The roadmap in ARCHITECTURE.md (step 2 narrative and GEOMETRY_PLAN's
"Blender opaque export probe" section) previously put the Blender probe next,
with the N-BLAS generalization riding it. M4a changed the calculus: the
pipeline `plitka1.ogf → legacy adapter → canonical writer → schema decoder` is
already proven offline (the opt-in `xrPhotonM4aOfflineProof` target pins the
1,802-vertex / 3,300-index / 71,328-byte result —
[tests/m4a_offline_proof.cmake](tests/m4a_offline_proof.cmake),
[tests/legacy_ogf_tests.cpp:621-634](tests/legacy_ogf_tests.cpp)), but the
visible end-to-end pipeline — *real OGF → OGFx → runtime loader → GPU →
rendered pixels* — stops at the runtime capability gate. Finishing that visible
pipeline first:

- proves the whole modernization thesis on real S.T.A.L.K.E.R. content with
  zero new authoring work (the Blender probe requires building an export
  path first);
- gives the N-BLAS generalization and the texture path a *real* acceptance
  input instead of a synthetic one;
- leaves the Blender probe as the **third gallery entry**, which it can join
  through exactly the same generic path once its exporter exists.

The permanent gallery ordering (recorded in the docs phase):

1. Generated `test_quad.ogfx` — already rendered.
2. Legacy-static `plitka1.ogfx` — **this plan**.
3. Blender opaque probe — next milestone, third entry.
4. `bochka_fuel.ogfx` — later, after hierarchy/bone/IK OGFx contracts and
   their runtime consumers exist.

The gallery is a preview/integration scene, not a new asset format and not a
source-specific subsystem: entries are additive rows in one code-owned table,
and every row uses the same runtime path.

## 2. Current state (verified against the code)

Phases 0–2 are complete (`f75fe82`, `31e3e6e`), and this section reflects the
verified Phase-3 working tree pending its owner commit. The load-bearing facts,
with citations; where the briefing that motivated this plan disagreed with the
code, the correction is noted:

- **Runtime decoding.** The decoder has exactly two profiles —
  `DecodeProfile::Schema` and `DecodeProfile::Runtime`
  ([src/ogfx_decoder.cpp:109-113](src/ogfx_decoder.cpp)). Both run the complete
  structural/semantic validation; the runtime profile then layers
  `validateRuntimeProfile()`. Phase 3 removed the one-mesh, one-geometry, and
  one-material restrictions and changed opacity validation to inspect every
  geometry. It still requires opaque geometry, every `textureRefOffset ==
  UINT32_MAX`, and `stringByteSize == 0` until the Phase-4 texture consumer
  lands.
  `decodeModelSchema` / `decodeModel` are the two entry points
  (:1158-1170). **Correction to the brief:** the runtime adapter does not
  "discard the decoded logical texture reference" — under the runtime profile
  the decoder never *reconstructs* reference strings at all
  (`matchedTexts` is recorded only for `Schema`,
  [src/ogfx_decoder.cpp:713-717, 727, 749-787](src/ogfx_decoder.cpp)), so the
  expansion starts in the decoder, not the loader. The 64 MiB
  decoded-reference cap (`MaximumDecodedTextureBytes`,
  [src/ogfx.hpp:29](src/ogfx.hpp)) is likewise enforced only on the Schema
  branch today (:750-771).
- **Runtime adaptation.** `decodeOgfxScene`
  ([src/ogfx_loader.cpp:51-124](src/ogfx_loader.cpp)) calls `ogfx::decodeModel`
  (:55), converts field-by-field into one `SceneData`, sets every
  `SceneMaterial::baseColorImage` to zero (:112), carries the decoded logical
  `baseColorTexture` string into `SceneMaterial`, and leaves `instances` and
  `images` empty by contract ([src/ogfx_loader.hpp:24-31](src/ogfx_loader.hpp);
  pinned by [tests/ogfx_loader_tests.cpp:127-128](tests/ogfx_loader_tests.cpp)).
  `SceneData` already carries vectors for positions, attributes, indices,
  geometries, meshes, instances, materials, and images
  ([src/scene.hpp:62-72](src/scene.hpp)).
- **Gallery orchestration.** `main()` calls `loadGalleryScene()` and retains
  the returned ordinary `SceneData`. File-private asset and placement tables
  load the generated quad and wedge through the same OGFx path, merge their
  records through the Vulkan-free scene-assembly API, and place the quad once
  and the wedge twice. The absolute generated-asset paths remain embedded at
  configure time and explicitly temporary.
- **Acceleration structures.** Phase 3 builds one BLAS per mesh and one TLAS
  instance per scene placement. BLAS handles and arena offsets are owned in a
  vector; geometry storage and aligned scratch storage are arena-backed; all
  BLAS builds are batched before the TLAS build. Device limits are checked for
  geometry, primitive, instance, storage-buffer-range, and address-alignment
  requirements. `instanceCustomIndex = mesh.firstGeometry` preserves the
  shader's `InstanceID() + GeometryIndex()` lookup across merged models and
  shared BLAS instances.
- **RT pipeline.** Four descriptor bindings (TLAS, storage image,
  geometry-record SSBO, material SSBO —
  [src/rt_pipeline.cpp:90-113](src/rt_pipeline.cpp)); pool sized to exactly
  those descriptors, `maxSets = 1` (:128-140); three shader groups (:25,
  :277-299); SBT with one record per region (:352-434).
  `writeSceneDescriptorSet` writes bindings 2–3 once at startup (:195-217);
  `writeRtDescriptorSet` rewrites 0–1 per swapchain recreate.
- **Shader.** `fetchHitAttributes` indexes
  `geometryRecords[InstanceID() + GeometryIndex()]`
  ([shaders/raytrace.slang:72](shaders/raytrace.slang)); `TMax = 100.0` (:122);
  `TraceRay(..., RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ...)` — SBT offset and
  geometry multiplier zero (:130); closest-hit shades a red/green UV gradient ×
  `baseColorFactor` × a normal-length liveness term (:145-151).
  `MaterialRecord.baseColorTexture` exists in the ABI
  ([src/gpu_scene.hpp:31-42](src/gpu_scene.hpp), fed from `baseColorImage` at
  [src/gpu_scene.cpp:212](src/gpu_scene.cpp)) but no shader reads it yet.
- **plitka1 artifacts.** The canonical opt-in proof output exists locally at
  `build/ogfx-core/corpus/meshes/objects/dynamics/plitka/plitka1.ogfx`
  (71,328 bytes, SHA-256 `cdeb6203…`, logical texture
  `ston\ston_stena_marbl_m_03_back` embedded); hash-identical manual copies
  exist under `build/debug/converted/` and `build/release/converted/`. Its
  model AABB is min ≈ (0, 0, −0.0104), max ≈ (2.3706, 2.8633, 0) — a wall-tile
  section about 2.4 m × 2.9 m and 1 cm thick with its corner at the origin
  (read from the file; useful for the preview transforms in G10). The proof
  currently *asserts the runtime rejects the output*
  ([tests/legacy_ogf_tests.cpp:718-723](tests/legacy_ogf_tests.cpp), driven by
  [tests/m4a_offline_proof.cmake:41-52](tests/m4a_offline_proof.cmake)) — the
  Phase 4 gate removal must flip that assertion in that same commit. The
  Git-ignored local SoC corpus includes the matching DDS at the Phase-0 path
  recorded below; no GSC texture is tracked by Git.
- **Build layout.** `xrPhotonOgfx` (writer + decoders) and
  `xrPhotonLegacyOgf` build without graphics ([CMakeLists.txt:15-36](CMakeLists.txt));
  `XRPHOTON_BUILD_ENGINE=OFF` returns before any graphics dependency
  (:112-116); `xrPhotonOgfxRuntime` contains the GLM-dependent loader and
  Vulkan-free scene assembly with their own tests. `gallery.cpp` belongs to
  the engine executable. The probe compiler now emits both generated assets.
  The corpus-path cache variable precedent is `XRPHOTON_M4A_CORPUS_OGF`.
- **Remaining documentation debt.** ARCHITECTURE.md reflects the Phase-3
  N-BLAS/gallery state. The older roadmap narrative receives its final
  consolidation in Phase 6.

## 3. Non-goals

Each names what picks it up, per the trigger-based-engineering convention.
This list is binding scope (see also §7):

- **Runtime loading of original `.ogf`** — never; OGFx is the only runtime
  model format (FORMATS.md asset-format decision).
- **Source-specific runtime adapters** — never; offline adapters only.
- **Blender exporter implementation** — the next milestone (gallery entry 3).
- **`bochka_fuel` conversion** — after nested-visual/bone/IK OGFx contracts
  (FORMATS.md milestone section); gallery entry 4.
- **Alpha-tested any-hit shaders and the opaque/alpha SBT split** — the split
  milestone (GEOMETRY_PLAN D6/D7 and its `RayTypeCount` scheme). This plan
  keeps `RAY_FLAG_FORCE_OPAQUE`, the single hit group, geometry multiplier 0,
  and `instanceShaderBindingTableRecordOffset = 0`.
- **General translucency; normal/roughness/metalness textures** — path-tracing
  step and its material model.
- **Mip generation / LOD selection** — step 4 (ray cones); this plan uploads
  mip 0 only, exactly the single-mip non-goal GEOMETRY_PLAN §2 already records
  (DDS files carry mip chains; the extra mips are validated-ignored, not
  generated).
- **Dynamic TLAS refits, skinning** — roadmap step 3.
- **Scene/level file ownership of instances** — the code-owned gallery table
  is explicitly temporary bring-up scene ownership (GEOMETRY_PLAN D8); the
  future scene/level owner retires it.
- **Path-tracing lighting, accumulation, denoising** — roadmap steps 4–5.
- **Asset caching / hot reload / a real asset root** — the SDK/project-model
  era; the embedded-absolute-path policy stays for this milestone (G4).

## 4. Design decisions

Numbered G1… to avoid colliding with GEOMETRY_PLAN's D-records, which several
of these instantiate.

### G1. Runtime-profile expansion: one gate function shrinks in consumer-atomic steps; no second decoder

**Decision:** the runtime profile expands **in place**, in
[src/ogfx_decoder.cpp](src/ogfx_decoder.cpp), by shrinking
`validateRuntimeProfile()` — but each gate falls **in the same commit as the
runtime consumer that makes the newly accepted data renderable**, never
earlier:

- **Phase 3** removes the record-count gates (mesh/geometry/material counts,
  :1070-1090). Their consumer is the N-BLAS/N-instance generalization and
  per-mesh geometry ranges landing in that same commit. The alpha-tested
  check becomes a loop over *all* geometries in the same commit — the
  single-geometry check at :1091 was complete only *because* the count gate
  held.
- **Phase 4** removes the texture-reference and string-arena gates
  (:1098-1113). Their consumer is texture resolution, upload, and the
  guaranteed fallback landing in that same commit. The Schema-only branches
  that reconstruct reference strings (`matchedTexts`, :713-717/:727, and the
  assignment loop :773-787) and the 64 MiB `MaximumDecodedTextureBytes` cap
  (:749-771) become profile-independent here too — profile-independent
  reconstruction has no runtime consumer before this phase.
- The runtime profile continues to reject, through this whole milestone:
  **any geometry with the alpha-tested flag** — the trace still forces opaque
  and has no any-hit consumer, so accepting the flag would silently render
  wrong semantics (the exact reasoning FORMATS.md records for the current
  gate). The split milestone removes it atomically with its any-hit consumer,
  continuing this same rule.

`DecodeProfile::RuntimeM4` is renamed `DecodeProfile::Runtime` in Phase 1 (a
pure internal rename; the enum keeps exactly two values). Gate diagnostic
strings keep their current wording until the phase that removes or reshapes
that gate — the alpha gate's text drops the "M4" wording when it becomes a
loop in Phase 3, reporting `"opaque geometry required by the runtime profile
(alpha-tested consumer not yet implemented)"`-style expected/found text
through the existing `makeChunkDiagnostic` shape.

**The atomicity rule (binding for this and future milestones):** at every
commit boundary, `decodeModel` accepts exactly what the engine in that same
commit can render faithfully. FORMATS.md states the texture gate holds "until
the texture milestone lands" and that a runtime reference is "rejected as
unsupported, never silently discarded" ([FORMATS.md:440-447](FORMATS.md)) —
opening a gate phases before its consumer exists would recreate exactly the
valid-but-not-renderable ambiguity that contract exists to prevent, and "no
caller loads such a file yet" is a gallery-table fact, not an API guarantee.
The cost is that the profile-gate test expectations move in two commits
instead of one; each move is enumerated in its phase.

After Phase 4 the two profiles differ **only** in the alpha capability gate —
`decodeModelSchema` remains the offline superset (it accepts alpha-tested
geometry for compiler round-trips), and no validation logic is duplicated.
Structural validation is untouched throughout: chunk framing, partitions,
index bounds, string-arena rules, bounds enclosure, and the 1 GiB /
4096-chunk / 4096-byte-string resource caps all still run for both profiles
in every phase.

**Rejected:** a third decode profile ("textured-but-single-mesh" or
"multi-mesh-but-untextured") — capability gates would multiply combinatorially
and each intermediate profile would need its own test matrix for one commit of
lifetime; the staged shrink of the one gate function covers the same ground
with no new profile. Also rejected: moving the capability gate into the
loader — the decoder is where FORMATS.md documents runtime acceptance, and
splitting the gate across layers would create two acceptance authorities.
Also rejected: opening every gate in Phase 1 under a "no caller loads textured
files yet" sequencing guarantee (an earlier draft's shape) — it broke
gate/consumer atomicity and the FORMATS.md contract for three commits, and
was flagged in external review.

### G2. Texture references travel inside `SceneMaterial`; resolution happens at scene assembly

**Decision:** `SceneMaterial` ([src/scene.hpp:34-39](src/scene.hpp)) gains one
field:

```cpp
struct SceneMaterial
{
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t baseColorImage = 0;      // resolved index into SceneData::images; 0 == fallback
    float alphaCutoff = 0.5f;
    // The logical texture reference exactly as stored in OGFx (e.g.
    // "ston\ston_stena_marbl_m_03_back"); empty == no reference. Kept after
    // resolution as CPU-side identity/diagnostic data — the GPU boundary
    // uploads only baseColorImage.
    std::string baseColorTexture;
};
```

The runtime adapter ([src/ogfx_loader.cpp](src/ogfx_loader.cpp)) copies
`ogfx::Material::baseColorTexture` into it verbatim and keeps
`baseColorImage = 0`. (Until Phase 4 the runtime gate guarantees the decoded
string is empty — the copy is landed groundwork; its nonempty case is pinned
by hand-built `SceneData` in the merge tests (G3) from Phase 2, and by the
loader's textured fixture the moment the gate opens in Phase 4.) The
**resolver** (G7) runs later, during scene assembly,
over the fully merged `SceneData`: it deduplicates references, loads images,
appends them to `SceneData::images`, and writes each material's final
`baseColorImage`. Invariant after assembly: every material with a nonempty
`baseColorTexture` has a `baseColorImage` that indexes the image resolved from
it, or assembly failed loudly; `baseColorImage == 0` with an empty reference
means the deliberate white fallback.

**Rationale:** the brief's three candidate carriers were (a) extending
`SceneMaterial`, (b) an intermediate loaded-model type, (c) resolving during
assembly. (a) and (c) are not alternatives — the *carrier* is the material
field, the *resolution point* is assembly, and both are needed: resolution
cannot happen per-model because image indices are scene-global and shared
across models (two models referencing the same texture must map to one image).
(b) — a parallel `LoadedModel` struct or an index-aligned side vector of
strings in `OgfxLoadResult` — was rejected as the classic desync-prone parallel
structure; a self-describing field on the material keeps loader, merge, and
resolver honest with no index bookkeeping, and the retained string has real
future consumers (SDK model viewer, hot reload, `bochka_fuel` diagnostics).
Vulkan stays out of the decoder/adapter layer: `std::string` on a Vulkan-free
struct changes nothing about the boundary.

### G3. Multi-model merge: a transactional `appendSceneModel` in a new Vulkan-free assembly unit

**Decision:** new TU `src/scene_assembly.{hpp,cpp}`, added to the
`xrPhotonOgfxRuntime` library (Vulkan-free; GLM only through `scene.hpp`, the
same dependency story as the loader —
[CMakeLists.txt:140-145](CMakeLists.txt)). Exported surface:

```cpp
// scene_assembly.hpp — generic CPU-side scene assembly. OGFx models own no
// world placement, so incoming models must have empty instances and images;
// the caller adds instances afterwards and resolves images after all appends.
bool appendSceneModel(SceneData* scene, SceneData&& model, std::string* error);
bool appendSceneInstance(SceneData* scene, uint32_t meshIndex,
    const glm::mat4& transform, std::string* error);
// Final caps over the assembled scene (24-bit instanceCustomIndex budget,
// element-count sanity) — called once after the last append/instance.
bool validateAssembledScene(const SceneData& scene, std::string* error);
```

Rebase rules in `appendSceneModel` (the complete list — every record type):

| Appended record | Rebase applied |
|---|---|
| `positions` / `attributes` / `indices` | raw concatenation — index *values* stay geometry-local by format contract (FORMATS.md "32-bit indices, geometry-local"), so they never rebase |
| `SceneGeometry.firstVertex` | `+= vertexBase` (`attributes.size()` before append; `positions.size()/3` must agree) |
| `SceneGeometry.firstIndex` | `+= indexBase` (`indices.size()` before append) |
| `SceneGeometry.materialIndex` | `+= materialBase` |
| `SceneMesh.firstGeometry` | `+= geometryBase` |
| `SceneMaterial` | appended as-is (reference string travels; `baseColorImage` still 0) |
| `SceneInstance.meshIndex` | not touched here — models carry none; `appendSceneInstance` takes the merged mesh index and bounds-checks it |
| `SceneData.images` / `baseColorImage` | **not merged** — models must arrive with `images` empty (see below); images exist only scene-globally after resolution (G7) |

Preconditions, each a loud diagnostic naming the violated rule: the incoming
model's `instances` and `images` must be empty (image index 0 is the scene's
fallback slot, so per-model image indices cannot rebase meaningfully — the
rule removes the ambiguity instead of inventing a rebase for data that never
exists on the loader path); position count divisible by 3 and equal to
attribute count. Deep per-geometry range validation is *not* re-implemented
here — the decoder guarantees it per model and `createGpuScene` independently
re-checks the merged geometry streams and material indices
([src/gpu_scene.cpp:96-114](src/gpu_scene.cpp)) — but `createGpuScene` covers
**neither mesh ranges nor instances**, so `validateAssembledScene` owns the
whole-scene invariants the GPU boundary otherwise assumes: every rebased
offset computed in `uint64_t` and checked against `UINT32_MAX`;
`geometries.size() < (1u << 24)` (the `instanceCustomIndex` budget —
GEOMETRY_PLAN D5; the `RayTypeCount` multiplier tightens this bound at the
split milestone and is noted in a comment, not implemented); every mesh's
geometry range nonempty and within the geometry array; every instance's
`meshIndex` valid; **at least one instance** (an instance-less scene has
nothing to trace and fails by name here, not somewhere inside the TLAS
builder); and every instance transform **finite with an invertible 3×3
linear block** (all elements finite, determinant nonzero, the diagnostic
naming the instance). The invertibility rejection is not a style choice:
`VkTransformMatrixKHR` requires an invertible 3×3
(VUID-VkTransformMatrixKHR-matrix-03799 — `WorldToObject` is undefined
otherwise), and GEOMETRY_PLAN's conversion rules already record exactly this
rejection for the future scene producer
([GEOMETRY_PLAN.md:554-557](GEOMETRY_PLAN.md)); the gallery is that producer
for now.

**Transactionality:** compute and validate all new sizes first, `reserve()`
every target vector (the only throwing step, before any mutation — wrapped so
`std::bad_alloc`/`std::length_error` become the named allocation-failure
error result instead of crossing the API, per the
no-exceptions-across-subsystem-boundaries convention), then
append; a failure therefore leaves `*scene` unchanged, mirroring the decoder's
no-partial-result contract. The count/cap checks live in a small pure helper
over plain counts so unit tests can exercise the `UINT32_MAX` and 24-bit
boundaries with arbitrary numbers instead of gigabyte fixtures.

**Unit-test matrix** (new suite `scene_assembly`,
`tests/scene_assembly_tests.cpp`, engine configurations like the loader suite):

- append one model into an empty scene → identity (byte-equal vectors);
- append two hand-built models (second with 2 geometries, 2 materials, 1 mesh)
  → every offset column above verified exactly, arrays concatenated exactly;
- append three models → repeated-rebase correctness;
- append the same model twice → duplicate-data independence (the Phase 3
  on-screen case);
- hand-built models whose materials carry nonempty `baseColorTexture`
  strings → the merge preserves them byte-exact (assembly is pure
  `SceneData`-level code, so reference survival through merging is pinned
  here in Phase 2, before Phase 4 opens the decoder gate);
- one end-to-end case: two models produced via `serializeModel` →
  `decodeOgfxScene`, merged, verifying material rebase through the real
  loader path (untextured models until Phase 4 — the runtime gate still
  rejects references; Phase 4 upgrades this case to a textured file);
- `appendSceneInstance`: valid indices; two instances of one mesh; rejection
  of `meshIndex >= meshes.size()` with the index named in the diagnostic;
- `validateAssembledScene` rejections, each with its named diagnostic: empty
  instance list; a mesh whose geometry range is empty or out of bounds; an
  instance with an invalid `meshIndex`; a non-finite transform element; a
  zero-determinant (singular) transform;
- precondition rejections: incoming model with instances; with images;
  mismatched position/attribute counts — each leaves the target unchanged
  (assert exact equality with a pre-append copy);
- count-helper boundary tests: offsets that would exceed `UINT32_MAX`;
  geometry totals at and over `1 << 24`; each produces the named diagnostic.

**Rejected:** merging inside `loadOgfxModel` (couples the file boundary to
scene policy and makes single-model tests impossible); merging inside
`createGpuScene` (puts CPU scene policy behind the Vulkan boundary); a
"gallery scene" type distinct from `SceneData` (a parallel scene
representation — the gallery is ordinary `SceneData`).

### G4. Gallery tables and CMake/asset policy: assets vs placements, compile-time absolute paths, empty-default cache variables, skip-vs-fail split, explicit log lines

**Decision:** a new engine-side TU `src/gallery.{hpp,cpp}` owns the temporary
code-owned gallery as **two file-private tables** — assets and placements —
because one OGFx file may carry several meshes and one loaded asset may be
placed several times:

```cpp
struct GalleryAsset       // one OGFx file, loaded exactly once
{
    const char* name;         // log/diagnostic label (plain ASCII)
    const char8_t* ogfxPath;  // compile-time absolute path; empty == not configured
    bool optional;            // skip-if-unconfigured vs required
};
struct GalleryPlacement   // one world placement of one loaded asset
{
    uint32_t assetIndex;
    glm::mat4 transform;
};
```

The path fields are `const char8_t*` because the embedded-path macros are
C++ `u8"..."` literals ([CMakeLists.txt:210-212](CMakeLists.txt) —
`XRPHOTON_TEST_QUAD_ASSET_PATH=u8"…"`); a `const char*` table would not
compile against them. Paths convert through `std::filesystem::path` at load
time, exactly as [src/main.cpp:355](src/main.cpp) does today.

**Placement semantics (the multi-mesh rule, decided now so the Blender probe
inherits it):** one placement instantiates **every mesh of its asset** with
that one transform — OGFx meshes are model-space parts of a single object, so
placing "the asset" places all of them (one `appendSceneInstance` per mesh
of the asset's recorded mesh range, same matrix). Placing an asset twice
therefore yields shared BLASes with
distinct TLAS instances — the N-instance sharing case Phase 3 puts on screen.
A future per-mesh placement need (none exists in this milestone's content)
would extend `GalleryPlacement` with an optional mesh selector; recorded as
the trigger, not built.

**Provenance for placements and diagnostics:** `loadGalleryScene` records,
per asset, whether it **loaded** (an unconfigured optional asset records
`loaded = false`, and placements referencing it are skipped alongside the
asset's own "skipped" log line — never an error), plus its name, source
path, and the merged scene's **mesh range**
`[firstMesh, firstMesh + meshCount)` and **material range**
`[firstMaterial, firstMaterial + materialCount)`, both captured at append
time. The mesh range is what placements instantiate (one
`appendSceneInstance` per mesh in the range); the material range is what
diagnostics key on. The texture resolver (G7) stays fully generic — its
structured result carries the generic diagnostic plus the failing scene
material index — and gallery code maps `failedMaterial` into the recorded
material ranges to identify the owning entry, prefixing its name and OGFx
path; no diagnostic text is ever parsed. The same ranges produce each
entry's resolved-texture count for the per-entry log lines. Without this
provenance, multi-mesh placements and the promised entry-level diagnostics
would both be unconstructible from a merged, index-only scene.

One exported function:

```cpp
// gallery.hpp — loads every configured gallery entry through the one generic
// OGFx path, appends preview instances, and resolves textures (Phase 4+).
// Returns model-owned SceneData ready for createGpuScene.
struct GalleryLoadResult { SceneData scene; std::string error; /* bool op */ };
GalleryLoadResult loadGalleryScene();
```

`main()` shrinks to `loadGalleryScene()` + error check — orchestration only.
The tables are bring-up scene ownership per GEOMETRY_PLAN D8, retired when
scene/level data has a real owner; they never become an OGFx chunk.

CMake policy (Phase 5), following the two precedents already in the tree —
the embedded absolute asset path ([CMakeLists.txt:208-212](CMakeLists.txt))
and the corpus cache variable ([CMakeLists.txt:89-92](CMakeLists.txt)):

- `XRPHOTON_GALLERY_PLITKA_OGFX` — `CACHE FILEPATH`, **default empty**. When
  empty, the plitka entry is compiled out of the table (empty path literal)
  and reported as skipped at runtime. When set, the absolute path is embedded
  via `target_compile_definitions`, so a configured gallery works regardless
  of the executable's working directory (same mechanism as the quad).
- `XRPHOTON_GALLERY_TEXTURE_ROOT` — `CACHE PATH`, **default empty**; the local
  texture root the resolver (G7) searches beneath.
- Defaults are deliberately empty rather than pointing at
  `build/ogfx-core/corpus/…`: an empty default makes "skipped: not
  configured" the true out-of-box state, and a maybe-existing default path
  would create a third ambiguous state (default-configured-but-absent).
  Nothing in **engine code** references the `build/ogfx-core` layout; README
  documents the one-time configure line
  `-DXRPHOTON_GALLERY_PLITKA_OGFX=$PWD/build/ogfx-core/corpus/meshes/objects/dynamics/plitka/plitka1.ogfx`
  (cache variables persist per build tree). The M4a proof target itself
  remains opt-in and unchanged.

**The skip-vs-fail distinction, exactly:**

- *Not configured* (cache variable empty) → the entry is skipped; normal
  builds and tests are unaffected; runtime prints
  `Gallery entry 'plitka1': skipped (XRPHOTON_GALLERY_PLITKA_OGFX not configured).`
- *Configured but broken* (file missing/unreadable, decode failure, texture
  root missing while references exist, unresolvable/undecodable texture) →
  **loud startup failure**: the full loader/resolver diagnostic to
  `std::cerr`, `main()` returns 1. A misconfigured gallery is a broken build,
  and skipping it would hide exactly the class of bug this milestone exists
  to surface.
- Every loaded entry logs one line naming the entry, its mesh/geometry/
  material counts, and (Phase 5) its resolved texture count:
  `Gallery entry 'test_quad': loaded (1 mesh, 1 geometry, 1 material).`

The non-optional quad entry keeps the existing always-generated
`XRPHOTON_TEST_QUAD_ASSET_PATH`; normal builds therefore still generate and
render the quad with zero configuration, and repository tests never touch GSC
assets.

**Rejected:** runtime CLI arguments or environment variables for the paths (a
second configuration system next to the existing compile-time policy; the
documented replacement for both is the future asset root, and this milestone
must not design it by accident); defaulting the plitka path to the proof
output (ambiguity argued above); making a broken configured entry a warning
(silent-degradation class); a CMake-generated configuration header instead of
compile definitions (more robust against exotic path characters, but the
compile-definition precedent already exists twice and today's burden is one
quoted `u8` literal — recorded as the fallback if a real local path ever
breaks the definition route).

### G5. N BLASes and N instances: batched builds, one aligned scratch arena, vectorized ownership, and the `InstanceID() + GeometryIndex()` identity

**Decision:** implement GEOMETRY_PLAN's recorded N-BLAS reference design
(GEOMETRY_PLAN "Post-M4 N-BLAS / N-instance generalization" and risks 4/8)
in [src/acceleration_structure.{hpp,cpp}](src/acceleration_structure.hpp),
keeping `buildAccelerationStructures`' signature and borrowed-sync contract.

Ownership changes:

```cpp
struct BlasEntry
{
    VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkDeviceAddress address = 0;    // queried at creation; instances reference it
};
struct AccelerationStructure
{
    // device / allocator / destroyAccelerationStructure unchanged
    VkBuffer instanceBuffer;  VmaAllocation instanceBufferAllocation;   // N instances
    std::vector<BlasEntry> blases;                                      // one per SceneMesh
    VkAccelerationStructureKHR tlas;  VkBuffer tlasBuffer;  VmaAllocation tlasBufferAllocation;
    VkBuffer scratchBuffer;  VmaAllocation scratchBufferAllocation;     // ONE shared arena
    ...
};
```

The destructor loops `blases` (handles first, then backing buffers — they are
placed on them), then TLAS handle/buffer, the one scratch arena, and the
instance buffer, all null-guarded so partial builds still bare-return; entries
are pushed into the vector **as they are created**, preserving the
partial-failure teardown contract verbatim. The container itself gets an
explicit failure contract on this `VkResult` path:
`blases.reserve(meshCount)` runs **before any Vulkan resource is created**,
so the adoption pushes are non-throwing by construction, and every remaining
allocating step is wrapped to convert `std::bad_alloc`/`std::length_error`
into `VK_ERROR_OUT_OF_HOST_MEMORY` — a throw escaping while fresh handles
sit outside the owner would violate the no-exceptions-across-subsystem-APIs
rule. Adoption is immediate: a created handle is in the vector before the
next call that can fail.

Build flow (one submission on the borrowed frame-0 slot, exactly today's
shape):

1. Per mesh *m*: build its `VkAccelerationStructureGeometryKHR[]` from the
   mesh's geometry range — one entry per `SceneGeometry`, each with
   *pre-offset* vertex/index device addresses
   (`base + firstVertex·12` / `base + firstIndex·4`, the same arithmetic
   `createGpuScene` uses for `GeometryRecord`s,
   [src/gpu_scene.cpp:175-187](src/gpu_scene.cpp)), `primitiveOffset = 0`,
   `firstVertex = 0`, `maxVertex = vertexCount − 1`, and
   `VK_GEOMETRY_OPAQUE_BIT_KHR` (every geometry is opaque this milestone — G1
   rejects the alpha flag upstream). Element-granular offsets keep every
   derived address 4-aligned by construction (GEOMETRY_PLAN D4); the base
   address guards stay.
2. Query `getAccelerationStructureBuildSizes` per BLAS (with the per-geometry
   `maxPrimitiveCounts` array) and for the TLAS
   (`primitiveCount = instances.size()`).
3. **Scratch arena:** allocate one buffer of
   `max(Σᵢ alignUp(blasScratchSizeᵢ, scratchAlignment), alignUp(tlasScratchSize, scratchAlignment)) + scratchAlignment − 1`
   bytes; `alignUp` the base device address once; BLAS *i*'s scratch region
   starts at the running aligned sum — disjoint because batched builds are
   unordered and concurrent, so per-BLAS regions **sum**; the TLAS reuses the
   arena from its aligned base because barrier #1 orders that reuse. All size
   sums run in `uint64_t`/`VkDeviceSize` with explicit overflow checks before
   allocation — a pathological sum fails loudly, never by wrap.
4. Record **one** `cmdBuildAccelerationStructures(cmd, blasCount, infos,
   rangePtrs)` for all BLASes, then barrier #1, then the TLAS build, then the
   trailing barrier #2 — same two-barrier structure and the same
   "no per-frame AS barrier" contract as today
   ([src/acceleration_structure.cpp:504-544](src/acceleration_structure.cpp)).
5. **Barrier #1 grows a write-visibility bit:** `srcAccessMask` stays
   `ACCELERATION_STRUCTURE_WRITE`, `dstAccessMask` becomes
   `ACCELERATION_STRUCTURE_READ | ACCELERATION_STRUCTURE_WRITE` (both at the
   BUILD stage). The read bit covers the TLAS reading BLAS contents, as
   today; the added write bit covers the arena reuse — a write-after-write on
   the same memory that synchronization validation reports as a hazard if the
   second scope carries no write access. This is the one deliberate
   synchronization change of the milestone; everything else is untouched.
6. Instance array: `N = scene.instances.size()` records written into the
   host-visible instance buffer (its 16-byte base-address check stays; records
   are 64 bytes, so all elements inherit the alignment). Per instance:
   `transform = toVkTransformMatrix(instance.transform)` (the one
   transform-layout boundary, unchanged), `instanceCustomIndex =
   meshes[instance.meshIndex].firstGeometry`, `mask = 0xFF`,
   `instanceShaderBindingTableRecordOffset = 0`, `flags = 0`,
   `accelerationStructureReference = blases[instance.meshIndex].address`.
7. Loud scene-vs-device gates where the properties are already in hand
   (:296-307): `instances.size() <= maxInstanceCount`, each BLAS's summed
   triangle count `<= maxPrimitiveCount`, per-BLAS geometry count
   `<= maxGeometryCount`. (`maxStorageBufferRange` vs. the record buffers is
   checked in `createGpuScene`, the unit that sizes them — Phase 3 adds it
   there.)

The AS unit consumes an assembled scene that `validateAssembledScene` (G3)
has already vetted — nonempty in-bounds mesh geometry ranges, valid instance
mesh indices, at least one instance, finite invertible transforms — so its
own gates
stay device-property-shaped; the existing base-address alignment guards
remain.

**Why `InstanceID() + GeometryIndex()` stays correct for every instance and
geometry** (documented here and as a comment at the instance-population site):
the merge (G3) makes each mesh's geometries a contiguous flat range
`[firstGeometry, firstGeometry + geometryCount)` — OGFx validation rule 8
guarantees mesh ranges partition the geometry array, and rebasing preserves
contiguity. BLAS *m*'s geometry list is built in exactly that flat order, so
for a hit on geometry *g* of instance *i* referencing mesh *m*:
`GeometryIndex() = g − firstGeometry(m)` (the BLAS-local index) and
`InstanceID() = instanceCustomIndex = firstGeometry(m)`, hence
`InstanceID() + GeometryIndex() = g` — the flat `GeometryRecord` index, for
any number of instances, including multiple instances of one mesh (they share
the same `firstGeometry`, and records are per-geometry, not per-instance, so
sharing is correct). The interim SBT contract (offset 0, multiplier 0, single
hit record) means SBT routing cannot be the source of a mis-render this
milestone — a wrong image is a loader/merge/transform bug by construction
(GEOMETRY_PLAN's load-bearing interim contract, preserved deliberately).

**Rejected:** per-BLAS scratch buffers (N allocations for transient memory the
arena covers; the recorded design already answered this); building BLASes in N
submissions (serializes what the batch API exists to overlap and multiplies
fence round-trips); `VK_GEOMETRY_INSTANCE_FORCE_OPAQUE` instance flags
(redundant with the per-geometry OPAQUE bit and hostile to the later split —
GEOMETRY_PLAN D6 already rejected it).

### G6. First accepted texture source format: DDS with BC1/BC3 payloads, decoded by a strict in-house header parser, uploaded compressed

**Decision:** the resolver accepts **`.dds` files carrying DXT1 or DXT5
payloads**, mapped to `VK_FORMAT_BC1_RGBA_SRGB_BLOCK` and
`VK_FORMAT_BC3_SRGB_BLOCK`, uploaded **compressed** (no CPU decompression).
The parser is a new in-house, standard-library-only strict decoder with a
pinned profile in the M4a style — accept exactly what the corpus needs, reject
everything else with a named diagnostic:

- magic `DDS ` and `dwSize == 124`; required flags CAPS|HEIGHT|WIDTH|PIXELFORMAT;
- `ddspf.dwSize == 32`, `DDPF_FOURCC` set, fourCC ∈ {`DXT1`, `DXT5`}
  (`DXT3`, `DX10`, uncompressed masks, palettes: rejected by name until a real
  asset demands them);
- 2D only — cubemap/volume caps bits rejected; nonzero dims with a 16384
  sanity cap;
- mip/level framing is exact, not "at least": level *k*'s size is
  `max(1, ceil((w>>k)/4)) · max(1, ceil((h>>k)/4)) · blockSize` (8/16 bytes)
  — never trusted from the header's pitch/linear-size fields (legacy writers
  fill them inconsistently). If `DDSD_MIPMAPCOUNT` or the MIPMAP caps bit is
  present, `dwMipMapCount` must be 1 or the full chain count for the stated
  dimensions (`floor(log2(max(w,h))) + 1`); flag/caps/count disagreements are
  rejected by name. The file size must equal `128 + Σ declared level sizes`
  **exactly** — truncation and trailing bytes are both named rejections; a
  strict parser does not shrug at either. Only mip 0 is *read* (single-mip
  policy, GEOMETRY_PLAN §2 non-goal — the mips already exist in the file, so
  nothing is generated *or* consumed), but the whole file must frame
  correctly.
- Base-color textures decode as sRGB block formats: DX9-era DDS carries no
  colorspace tag, X-Ray albedo is sRGB-authored, and base color is this
  milestone's only sampler consumer. When non-color maps arrive (out of
  scope), the material slot they bind to decides the colorspace — recorded so
  the question stays closed until then.

Resource caps, in the decoder-caps spirit: the 16384 dimension cap above,
plus a cumulative scene cap `MaxSceneTextureBytes = 512 MiB` over decoded
mip-0 payloads, enforced by `resolveSceneTextures` with a named diagnostic;
all image-size arithmetic runs in `uint64_t`.

**This profile is provisional until Phase 0.** Phase 0 inspects the real
`ston_stena_marbl_m_03_back.dds` — size, SHA-256, dimensions, fourCC, header
flags, caps bits, mip count and layout — and pins this field list to observed
reality *before any parser code is written in Phase 4*. A non-DXT1/DXT5
variant extends the profile deliberately there, never mid-implementation.

`SceneImage` ([src/scene.hpp:53-58](src/scene.hpp)) gains an explicit format:

```cpp
enum class SceneImageFormat : uint32_t { Rgba8Srgb, Bc1RgbaSrgb, Bc3Srgb };
struct SceneImage
{
    SceneImageFormat format = SceneImageFormat::Rgba8Srgb;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;   // mip 0 exactly; size pinned by format math
};
```

`scene.hpp` stays Vulkan-free; `gpu_scene.cpp` owns the one
`SceneImageFormat → VkFormat` mapping. The generated 1×1 opaque-white fallback
is `Rgba8Srgb` `{255,255,255,255}` at image index 0, always present (G7).

**Rationale, against the alternatives and the conventions:**

- **`.dds` is already the decided answer.** FORMATS.md's "Formats not
  automatically replaced" records `.dds` (textures) as *likely to remain
  unchanged* — the engine's texture format **is** DDS unless a real
  limitation appears. Requiring a hand-converted PNG would make the runtime's
  first texture consumer contradict the format decision on day one.
- **One deliberate answer that also serves entries 3 and 4.** `bochka_fuel`
  and every other legacy gallery entry reference DDS natively — the same
  resolver serves them with zero per-asset work. The Blender probe is
  *texture-free by design* (FORMATS.md milestone list), so it creates no PNG
  pressure; when Blender-authored *textured* content eventually arrives, the
  shared offline compiler decides how modern sources are compiled (DDS/BC
  output being the obvious candidate), on its own trigger. Accepting PNG now
  would guarantee two live source formats later — the parallel-systems
  outcome.
- **Source-asset honesty.** The originals-are-source-assets rule (FORMATS.md)
  means a hand-made PNG derivative of a GSC DDS is exactly the hand-patched
  intermediate the pipeline exists to eliminate; there is no offline texture
  compiler yet to automate it, and building one for this milestone is scope
  with no consumer.
- **No new vendored dependency.** A pinned-profile DDS header parse is a few
  hundred lines of the same bounded, diagnostic-first parsing this codebase
  already does twice ([src/legacy_ogf.cpp](src/legacy_ogf.cpp), the OGFx
  decoder); `stb_image` neither reads DXT DDS (without decompression hacks)
  nor fits the strict-rejection posture.
- **GPU cost.** BC stays compressed in VRAM and is the layout GPUs sample
  natively; decompress-to-RGBA8 would quadruple memory for strictly worse
  sampling.

The cost is one new device feature: `VkPhysicalDeviceFeatures::textureCompressionBC`
enters the suitability query, the enable chain, and the rejection report
(Phase 4). Every desktop GPU exposing `VK_KHR_ray_tracing_pipeline` supports
BC, so this narrows nothing in practice — it is the same
"check anyway, fail loudly" family as the existing format backstops.

**Rejected:** PNG + vendored decoder (all four bullets above); runtime DXT
decompression to RGBA8 (memory, and a second image path the moment BC upload
lands); KTX2/BasisU transcoding (a container and transcoder with no present
consumer — a future *offline compiler* question, not a runtime one).

### G7. Texture resolution: normalize-and-confine path mapping; missing referenced textures fail loudly

**Decision:** new Vulkan-free TU `src/texture_loader.{hpp,cpp}` in
`xrPhotonOgfxRuntime`, exporting:

```cpp
// Pure logical-name mapping — no filesystem access; unit-testable exhaustively.
// Returns the root-relative path ("ston/ston_stena_marbl_m_03_back.dds") or a
// diagnostic naming the offending byte/rule. The stored OGFx value is never
// modified; normalization output is local to resolution.
bool resolveLogicalTexturePath(std::string_view logicalReference,
    std::filesystem::path* relativePath, std::string* error);

// Checked file input + strict DDS decode (G6) into one SceneImage.
struct TextureLoadResult { SceneImage image; std::string error; /* bool op */ };
TextureLoadResult loadTextureFile(const std::filesystem::path& path);

// Walks the merged scene: ensures images[0] is the generated 1x1 white
// fallback, resolves every nonempty material reference beneath root
// (deduplicated by normalized relative path, indices assigned in first-use
// material order — deterministic), and writes baseColorImage. root may be
// empty only if no material carries a reference. On failure, error carries
// the generic core diagnostic and failedMaterial the scene index of the
// material the failure is attributable to — including the missing-root
// case, which reports the first material carrying a reference, so the
// gallery can name the entry that demanded a root. nullopt is reserved for
// genuinely scene-global failures (fallback creation, allocation failure).
// This is the structured hook the gallery's provenance keys on; callers
// never parse diagnostic text.
struct ResolveTexturesResult
{
    std::string error;                       // empty == success
    std::optional<uint32_t> failedMaterial;  // scene material index, if any
    explicit operator bool() const { return error.empty(); }
};
ResolveTexturesResult resolveSceneTextures(SceneData* scene,
    const std::filesystem::path& textureRoot);
```

Normalization and confinement rules — a deliberate **logical-name grammar**,
enforced by the resolver (a resolver contract, not an OGFx schema rule: OGFx
deliberately stores bounded UTF-8, and the offline compiler adopts this same
grammar on the producing side when it becomes the reference producer —
recorded as that milestone's trigger). Each rule rejects with a dedicated
diagnostic quoting the offending reference verbatim:

- Accepted form: one or more components of `[A-Za-z0-9_-]+` separated by
  single `\` — the canonical X-Ray separator. Internally `\` becomes `/` for
  the root-relative path; the stored OGFx string is never modified.
- Rejected by name: empty references or components; leading/trailing
  separators (absolute-path shapes); `/` (accepting both separators would
  give one texture two spellings and quietly break deduplication); any `.`
  byte (one rule that simultaneously kills `..` traversal, `.` segments, and
  extension smuggling — legacy logical names are extensionless and dot-free
  by convention); `:` bytes (Windows drive/alternate-stream defense —
  portable-code rule); NUL and control bytes (`< 0x20`, `0x7F`); and any byte
  outside the accepted set, including non-ASCII (shipping X-Ray names are
  ASCII).
- Append `.dds` (the G6 decision) and join the validated components beneath
  the configured root. Matching is exact-case; legacy shipping names are
  lowercase, and the README documents that the local root must preserve that
  (no case-folding machinery). Symlinks beneath the root resolve normally and
  are deliberately in scope: the root is owner-configured local machine
  state, and the threat model is malformed *asset names*, not a hostile
  filesystem — no lexical escape is possible once absolute prefixes, dots,
  separators-in-components, and drive bytes are banned. Recorded so the
  confinement claim is precise.

**Missing/undecodable referenced textures are a hard startup failure** — never
silently the white fallback. `resolveSceneTextures` produces the generic core
(material index, reference string, resolved path, violated rule/cause — all
it can know from a merged, index-only scene), and gallery code prefixes the
owning entry's name and OGFx path, keyed by the structured `failedMaterial`
index against G4's provenance ranges. The final
wrapped diagnostic names everything needed to act, one line to `std::cerr`,
e.g.:

```
gallery: entry 'plitka1' (<ogfx path>): material 0 texture reference
"ston\ston_stena_marbl_m_03_back": resolved to
<root>/ston/ston_stena_marbl_m_03_back.dds: file open failed
```

with the same shape for: texture root not configured while references exist
(`expected XRPHOTON_GALLERY_TEXTURE_ROOT to be configured`), a
normalization rejection (offending reference quoted verbatim plus the violated
rule), an unsupported DDS variant (found fourCC named), and a size/framing
mismatch (expected vs. found byte counts). Untextured materials (empty
reference) map to fallback index 0 by design and are not diagnostics. If the
owner ever wants a dev-mode "render missing textures as placeholder" policy,
that is an explicit future owner decision (§8), not a default.

The **fallback image** is created unconditionally by `resolveSceneTextures`
(even in a generated-assets-only, root-less gallery), so image index 0, binding 4's slot
0, and the "materials without references sample white" contract hold in every
configuration from Phase 4 on.

`loadTextureFile` and `resolveSceneTextures` wrap their allocating steps the
same way G3's assembly does — allocation failure returns the named
error result; no exception crosses the API.

**Rejected:** case-insensitive or fuzzy resolution (hides content errors and
invites platform divergence); searching multiple roots (a second lookup system
before the real asset root exists); making the fallback substitute for
failures (the silent-equivalence the brief forbids without an explicit owner
choice).

### G8. Descriptor organization: D7's fixed combined-image-sampler array, sized `MaxSceneTextures = 1024`, every slot written

**Decision:** adopt GEOMETRY_PLAN D7's recorded texture/descriptor design
unchanged — it was written for exactly this milestone family:

- Binding **4**: `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, descriptor count
  `MaxSceneTextures` (`constexpr uint32_t MaxSceneTextures = 1024;` in
  [src/gpu_scene.hpp](src/gpu_scene.hpp)), stages
  `CLOSEST_HIT | ANY_HIT` (the any-hit stage flag costs nothing now and is
  what the split milestone needs). The shader declares
  `Sampler2D sceneTextures[1024]` with the CPU/shader pairing pinned by a
  comment on both sides (the D6-argued treatment for a mismatch class that
  pipeline creation catches).
- `writeSceneDescriptorSet` writes **every** slot at startup: resolved scene
  images first (their order = `SceneData::images` order), the white-fallback
  view into all remaining slots. No `PARTIALLY_BOUND`, no variable counts, no
  `runtimeDescriptorArray` — writing ~1 K descriptors once at startup buys
  freedom from three feature bits.
- One shared `VkSampler` (linear min/mag, repeat, no anisotropy, maxLod 0),
  owned by `GpuScene` and reused in every write.
- Shaders index with `NonUniformResourceIndex(material.baseColorTexture)` —
  material indices diverge within a wave the moment two materials are visible.
- **Features** (the §4.3 second installment, verbatim):
  `VkPhysicalDeviceVulkan12Features` enters both the suitability query and the
  enable chain carrying `bufferDeviceAddress = VK_TRUE` and
  `shaderSampledImageArrayNonUniformIndexing = VK_TRUE`, and the standalone
  `VkPhysicalDeviceBufferDeviceAddressFeatures` struct is **removed in the
  same commit** — chaining both is invalid once `Vulkan12Features` is present
  (a spec obligation GEOMETRY_PLAN already flagged). `textureCompressionBC`
  (G6) rides the core-features struct in the same chain. The rejection report
  gains all three.
- **Per-format capability probe:** device selection additionally queries
  `vkGetPhysicalDeviceFormatProperties` for each `SceneImageFormat`'s
  `VkFormat` (requiring the `SAMPLED_IMAGE` and `TRANSFER_DST` format
  features plus linear filtering for the sampled case), **and**
  `vkGetPhysicalDeviceImageFormatProperties` for the exact creation tuple
  the upload path uses — 2D, optimal tiling, `SAMPLED | TRANSFER_DST` usage,
  no flags — treating `VK_ERROR_FORMAT_NOT_SUPPORTED` as a named rejection.
  Format-feature flags alone do not prove a specific image creation is
  legal; the tuple query is the authority, and image creation gates extents
  against the tuple's returned `maxExtent` rather than the generic
  `maxImageDimension2D`. `textureCompressionBC` guarantees the BC family's
  core capabilities, but both queries are their own truth — the same
  belt-and-braces family as `hasRequiredAccelerationStructureFormatSupport`,
  turning an exotic driver gap into a named selection rejection.
- **Limit gate** (declared in `gpu_scene.hpp`, called from
  `queryPhysicalDeviceSuitability` — the existing cross-link pattern of
  `hasRequiredAccelerationStructureFormatSupport`): the combined array counts
  against both the sampled-image and sampler limits, per stage and per set,
  plus the aggregate — require `maxPerStageDescriptorSampledImages`,
  `maxPerStageDescriptorSamplers`, `maxDescriptorSetSampledImages`,
  `maxDescriptorSetSamplers` `>= MaxSceneTextures` and
  `maxPerStageResources >= MaxSceneTextures + 2` (the two hit-stage SSBOs).
  The spec minimums here are tiny (16 per-stage samplers), so this is a real
  gate, reported per-limit in the device rejection log like every other
  category.
- **The fixed pool grows in place**
  ([src/rt_pipeline.cpp:128-140](src/rt_pipeline.cpp)): one more
  `VkDescriptorPoolSize { COMBINED_IMAGE_SAMPLER, MaxSceneTextures }`;
  `maxSets` stays 1; still no `FREE_DESCRIPTOR_SET_BIT`; bindings 0–1 keep
  their resize-rewrite path untouched, bindings 2–4 are startup-written.
- Startup scene-vs-device check: `scene.images.size() <= MaxSceneTextures`,
  loud (`createGpuScene`).

The design does not bake in two textures: the gallery uses 2–3 slots of a
1024-slot array whose scaling story (and its `MaxSceneTextures` constant) is
already the recorded long-term shape. Growing past 1024 later is a one-constant
change gated by the same limit check.

**Rejected** (re-affirming D7 against the new context): scene-sized descriptor
counts (forces `runtimeDescriptorArray`/variable-count features and couples
pipeline layout to scene content for zero benefit at any realistic scene
size); separate sampler + sampled-image bindings (two arrays and two writes
per slot to save nothing — one shared sampler makes combined the natural
type); per-material descriptor sets (descriptor-set multiplication, the
opposite of the bindless direction the records already point at).

### G9. Shader behavior and the quad's UV-gradient diagnostic: retire the probe; generic material shading with a live normal term; the quad becomes the fallback-path oracle

**Decision:** `closestHitMain` becomes the generic opaque material shading the
N-BLAS reference design records (GEOMETRY_PLAN D10 / generalization
milestone), extended by the sampled base color:

```slang
HitAttributes hit = fetchHitAttributes(attribs);
MaterialRecord material = materials[hit.materialIndex];
float3 baseColor = material.baseColorFactor.rgb
    * sceneTextures[NonUniformResourceIndex(material.baseColorTexture)]
          .SampleLevel(hit.uv, 0).rgb;
payload.color = baseColor
    * (0.2 + 0.8 * abs(dot(hit.worldNormal, -WorldRayDirection())));
```

No source-specific branch exists anywhere: the untextured quad samples the
white fallback at slot 0 (index 0 is a *value*, not a sentinel — no "has
texture?" branch), and plitka samples its resolved image. The red/green UV
gradient — M3b/M4's BDA/stride/layout oracle — is **retired as a completed
probe**, the recommended option, because the alternative (a generated
UV-gradient texture for the quad) has no clean carrier: assigning a generated
image to the quad's material at runtime would be exactly the source-specific
runtime logic the core rule forbids, and teaching the build to emit a gradient
DDS plus a logical reference would bolt an image encoder onto the quad tool to
preserve a probe whose milestone already closed.

What replaces each diagnostic property the gradient provided:

- *BDA/stride/layout corruption* — still visible: `hit.uv` feeds the sample
  coordinate (garbage UVs sample garbage texels on plitka) and the normal
  term keeps the attribute fetch and inverse-transpose transform live on
  every hit; additionally the merge unit tests (G3) pin exact attribute
  pass-through CPU-side, and the M3b one-line debug swap
  (`payload.color = 0.5 + 0.5 * hit.worldNormal`) remains the documented
  bring-up tool for any suspected regression (used transiently, reverted
  before commit).
- *Material identity* — the probe front end (`tools/compile_probe_assets.cpp`
  after Phase 3's rename)
  gives the quad's material a distinct non-white `baseColorFactor`
  (`{1.0, 0.45, 0.12, 1.0}` — amber), so the quad renders a recognizable flat
  color through the fallback path instead of white-on-white ambiguity; a
  material-rebase bug in the merge shows as the wrong color on the wrong
  model. (The generated asset is not byte-pinned anywhere — the golden-bytes
  test in [tests/ogfx_tests.cpp](tests/ogfx_tests.cpp) pins the *writer*
  against its own fixture, not the quad tool's model — so this is a safe,
  reviewable one-liner.)
- *UV orientation* — plitka's real marble texture, checked once against a
  Blender import of `plitka1.ogf` (Blender as visual oracle is the
  documented role; it is not part of the conversion path).
- *Winding and stored-normal sign* — **not observable in the standing
  shading**: `abs(dot(N, view))` is sign-blind and ray traversal culls
  nothing, so reversed winding or flipped normal signs render identically.
  The offline adapter's winding-versus-normals validation (M4a) remains the
  standing guarantee; on-screen positive verification happens once, during
  Phase 5 acceptance, via **two documented transient diagnostics** — each a
  one-line closest-hit swap, run hands-off and reverted before commit, the
  same discipline as the M3b normal swap. They are separate because
  `HitKind()` reports *geometric* facing derived from winding only and says
  nothing about the stored normals: (1) winding via
  `HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE` (front green / back red);
  (2) stored-normal sign via coloring by
  `sign(dot(hit.worldNormal, -WorldRayDirection()))` (green when the
  interpolated normal faces the viewer on a front-facing hit). Phase 5's
  standing claim is narrowed accordingly: its permanent oracles are texture
  orientation and the offline validation. The generated quad and wedge pin
  the diagnostic convention: their geometric fronts and stored normals face
  the startup camera, so their reference-pose hits must be green in both
  transient passes. Plitka is judged independently against its Blender
  reference rather than inheriting a generated-probe convention.

`missMain` and `rayGenMain` are unchanged except that raygen's
`RAY_FLAG_FORCE_OPAQUE`, zero SBT offsets, and `TMax` stay exactly as they are
(G10); the flag's removal belongs to the split milestone atomically with the
multiplier flip (GEOMETRY_PLAN D10's indivisible-change note).

### G10. Camera framing and `TMax`: keep `TMax = 100`; frame the gallery with the table's transforms, not camera changes

**Decision:** `TMax` stays `100.0` ([shaders/raytrace.slang:122](shaders/raytrace.slang)).
The recorded trigger ("rises to `1.0e4` when real converted scene extents
first require it") does not fire: the gallery's extent is under ten meters
(plitka is 2.37 × 2.86 m, the quad 1 × 1), and the startup camera at
`(0, 0, −2)` looking +Z ([src/camera.hpp:19-22](src/camera.hpp)) is ~5 m
from the placed models. Flying hundreds of meters away and losing the gallery
past `TMax` is expected fly-camera behavior, not a defect. The camera unit is
untouched.

Provisional gallery placements (code-owned, adjusted on screen during
Phase 3/5 review — they are preview policy, not contract): quad at
`translate(-3.0, 0, 3.0)`; plitka at `translate(-0.8, -1.43, 3.0)` (centering
its corner-origin AABB vertically); the generated wedge probe (Phase 3)
placed twice on the right — `translate(+1.5, 0, 3.0)` and
`translate(+3.6, 0, 3.0) · rotate(30°, Y) · scale(1.5, 1.0, 1.5)`, the second
deliberately rotated **and non-uniformly scaled** — its silhouette verifies
the vertex transform in Phase 3, and the same instance becomes the
normal-transform visual under Phase 4's view-dependent shading. The startup
camera at `(0, 0, −2)` looking +Z frames every Phase-3 silhouette at 16:9;
the fly camera reaches the rest, and the whole row sits comfortably inside
`TMax`. Timing: the quad
keeps the identity transform through Phase 2 (whose commit oracle is a
pixel-identical render); the quad's placement and both wedge placements land
together in Phase 3, whose oracle is placement; plitka's placement lands in
Phase 5.

## 5. Phases

Phase 0 is a no-commit prerequisite; every later phase is one commit. Exit
criteria for **every** committed phase: `debug`,
`release`, and `ogfx-core` presets configure and build; `ctest --preset
ogfx-core` and `ctest --test-dir build/debug` pass completely; the app runs
and renders its stated visual; the debug run is validation-silent (having
confirmed the layer actually loaded — the "silent only counts if the layer
loaded" rule from GEOMETRY_PLAN §6). Phase-specific validation runs are
listed per phase; §6 collects the practicalities.

### Phase 0 — Prerequisite (no commit): obtain and pin the real texture asset

**Status: complete (2026-07-17).** The owner-supplied, Git-ignored corpus file
`build/ogfx-core/original_game_files/soc/textures/ston/ston_stena_marbl_m_03_back.dds`
was inspected read-only and pins G6 to this observed profile:

- file size `174,904` bytes; SHA-256
  `b38cf3b7ee85f0c8bf5f3ace83385c13fc0a5ebb5dfb11e7ea6f31f02cf64fad`;
- `512 × 512`, fourCC `DXT1`, `DDS_HEADER.dwSize = 124`,
  `DDS_PIXELFORMAT.dwSize = 32`, pixel-format flags `0x00000004`
  (`DDPF_FOURCC`), and `dwPitchOrLinearSize = 131,072`;
- header flags `0x000A1007` (`CAPS | HEIGHT | WIDTH | PIXELFORMAT |
  LINEARSIZE | MIPMAPCOUNT`), caps `0x00401008`
  (`TEXTURE | COMPLEX | MIPMAP`), and caps2/caps3/caps4 all zero;
- `10` declared mip levels forming the complete 512→1 chain. Their DXT1
  payload sizes are `131072, 32768, 8192, 2048, 512, 128, 32, 8, 8, 8`
  bytes at file offsets `128, 131200, 163968, 172160, 174208, 174720,
  174848, 174880, 174888, 174896`, totaling `174,776`; with the 128-byte
  DDS framing this exactly matches the file size, with neither truncation nor
  trailing bytes.

Completed prerequisite record:

- The owner supplied `ston_stena_marbl_m_03_back.dds` (§8.1) in the ignored
  local corpus.
- The inspection above records its file size, SHA-256, dimensions, fourCC,
  header flags, caps bits, mip count, and exact layout.
- G6's accepted-field list is pinned to the observed values; a non-DXT1/DXT5
  variant extends the parser profile deliberately *now*, before parser code
  exists — never discovered mid-Phase-4.

Exit: the pinned record exists in this file; no repository code changes.

### Phase 1 — Reference carrier groundwork: `SceneMaterial::baseColorTexture` and the profile rename

**Status: complete (2026-07-17, commit `f75fe82`).**

**Lands:** G2's carrier and the `DecodeProfile::Runtime` rename. **No
acceptance change anywhere** — every gate in `validateRuntimeProfile()` still
holds (G1's atomicity rule: the gates fall in Phases 3 and 4 with their
consumers). The render is pixel-identical.

- **Files/APIs:** [src/ogfx_decoder.cpp](src/ogfx_decoder.cpp)
  (`DecodeProfile::RuntimeM4` → `DecodeProfile::Runtime` — an internal
  rename; every gate and diagnostic string is untouched),
  [src/ogfx.hpp](src/ogfx.hpp) (the `decodeModel` doc comment states the
  staged capability boundary and where each gate falls),
  [src/scene.hpp](src/scene.hpp) (`SceneMaterial::baseColorTexture`),
  [src/ogfx_loader.cpp](src/ogfx_loader.cpp) (copy the decoded reference
  verbatim; comment updated from "discards" semantics, noting the copied
  string is provably empty until the Phase 4 gate opens). No signature
  changes.
- **Ownership/lifetime:** unchanged — strings are owned by
  `ogfx::Model`/`SceneData` values.
- **Validation/errors:** none change.
- **Vulkan:** untouched (`MaterialRecord.baseColorTexture` still receives the
  inert 0 from `baseColorImage`).
- **Tests:** [tests/ogfx_loader_tests.cpp](tests/ogfx_loader_tests.cpp) — an
  untextured file adapts with an **empty** `baseColorTexture` and
  `baseColorImage == 0` (pins the copy path's shape). Every existing decoder
  expectation is unchanged — the rename is invisible to them, which is the
  point: this commit has no acceptance surface. The nonempty-string cases
  arrive with their gates: hand-built merge fixtures in Phase 2, the decoder
  and loader flips in Phases 3–4 (each phase carries its own audit rule:
  every `decodeModel`-based expectation whose schema-valid input only that
  phase's removed gates rejected flips to acceptance in that same commit, or
  the suites fail at the phase boundary).
- **Docs in-phase:** none beyond code comments (no contract moved).
- **Depends on:** nothing.
- **Removes:** nothing. **Keeps:** every runtime gate — deliberately (G1's
  atomicity rule); model-type 0; all decoder resource caps;
  `decodeModelSchema` as the offline superset.

### Phase 2 — Generic multi-model scene assembly; `main()` moves onto the gallery path (still one quad on screen)

**Status: complete (2026-07-17, commit `31e3e6e`).**

**Lands:** G3, plus the gallery skeleton from G4 with a single, non-optional
quad entry carrying the **identity transform** (the G10 gallery placements
land in Phase 3, not here). Visual: the identical quad — this phase's oracle
is *no change*, pixel-identical, while the entire CPU assembly path swaps in
underneath.

- **Files/APIs:** new `src/scene_assembly.{hpp,cpp}` and
  `src/gallery.{hpp,cpp}`; [src/main.cpp](src/main.cpp) replaces the
  load-plus-`emplace_back` block (:353-364) with `loadGalleryScene()`;
  [CMakeLists.txt](CMakeLists.txt) adds `scene_assembly.cpp` to
  `xrPhotonOgfxRuntime`, `gallery.cpp` to the engine target, and the
  `scene_assembly` test executable/suite.
- **Ownership/lifetime:** `SceneData` remains a `main()` plain value with
  program lifetime; the gallery table is file-private static data in
  `gallery.cpp`; `appendSceneModel` is transactional (reserve-then-append).
- **Validation/errors:** the G3 precondition/overflow/24-bit diagnostics;
  gallery load failures reach `main()` as one `GalleryLoadResult.error` and a
  bare `return 1;`; the per-entry "loaded" log line lands now.
- **Vulkan:** untouched; `createGpuScene`'s existing merged-range re-checks
  ([src/gpu_scene.cpp:96-114](src/gpu_scene.cpp)) now guard real merged input.
- **Tests:** the full G3 matrix in new `tests/scene_assembly_tests.cpp`
  (engine configurations, linked against `xrPhotonOgfxRuntime` like the
  loader suite).
- **Docs in-phase:** ARCHITECTURE.md module-map rows for `scene_assembly`
  and `gallery` (new TUs are documented in the commit that creates them —
  source-of-truth documents are never left stale across commits).
- **Depends on:** Phase 1 (merged materials carry reference strings through
  untouched).
- **Removes:** the single-asset orchestration in `main()`. **Keeps:** the AS
  single-mesh/single-instance gate (the gallery table still produces exactly
  one model and one instance until Phase 3 — deliberately, so this commit
  isolates the CPU path swap).

### Phase 3 — N-BLAS / N-instance acceleration structures; the record-count gates fall with their consumer (quad + twice-placed wedge)

**Status: implementation complete and verified (2026-07-18; owner commit
pending).**

**Lands:** G5, the Phase-3 half of G1, and a second **permanent** generated
probe asset. The offline front end
([tools/compile_test_quad.cpp](tools/compile_test_quad.cpp), renamed
`tools/compile_probe_assets.cpp` along with its CMake target) additionally
emits `test_wedge.ogfx`: one mesh, **two geometries** (two sloped rectangular
faces meeting at a ridge), **two materials** with visibly distinct
`baseColorFactor`s (blue and green). All models still shade the UV gradient ×
factor this phase — which is exactly what makes per-geometry factors visible.
The gallery gains the wedge as a required generated asset **placed twice**
(G10), and the quad simultaneously moves from Phase 2's identity to its G10
placement. The screen shows three objects from two assets, and the oracles
are GEOMETRY_PLAN's recorded exit oracles for this generalization
([GEOMETRY_PLAN.md:1086-1097](GEOMETRY_PLAN.md)) — two identical quads would
leave BLAS selection, BLAS sharing, and geometry indexing unobserved:

- two BLASes with **different content** (a swapped BLAS reference is visible);
- one BLAS referenced by **two TLAS instances** (sharing, on screen);
- `GeometryIndex() > 0` on the wedge's second geometry, whose distinct factor
  must land on the correct face (pins `InstanceID() + GeometryIndex()`
  visually);
- a rotated, **non-uniformly scaled** instance whose *placement and
  silhouette* verify the vertex transform. Its world-space **normal**
  transform is deliberately *not* claimed as a Phase 3 visual: the standing
  probe shader normalizes the normal and multiplies by
  `saturate(dot(N, N))` ≈ 1
  ([shaders/raytrace.slang:96, :150](shaders/raytrace.slang)) — a
  view-independent liveness term, not an oracle. The normal-transform visual
  lands with Phase 4's view-dependent `abs(dot(N, −ray))` shading on this
  same instance; if Phase 3 debugging needs it earlier, the documented
  transient normal swap (`payload.color = 0.5 + 0.5 * hit.worldNormal`) is
  the tool.

- **Files/APIs:**
  [src/acceleration_structure.{hpp,cpp}](src/acceleration_structure.hpp)
  (`BlasEntry` vector, arena, batched build, instance loop, destructor
  loops, scene-vs-device gates; `InstanceCount` constant deleted);
  [src/ogfx_decoder.cpp](src/ogfx_decoder.cpp) (**record-count gates
  removed; the alpha check becomes the all-geometries loop and its
  diagnostic drops the "M4" wording** — the Phase-3 half of G1);
  [src/gpu_scene.cpp](src/gpu_scene.cpp) (`maxStorageBufferRange` check on the
  record buffers); `tools/compile_probe_assets.cpp` (wedge model, second
  output; CMake emits both assets and embeds
  `XRPHOTON_TEST_WEDGE_ASSET_PATH`); `src/gallery.cpp` (wedge asset row + the
  three placements). `Renderer` and the frame
  path are untouched — the TLAS is still built once at startup, so the copied
  `Renderer.tlas` handle stays legal (the step-3 borrowed-pointer note at the
  field remains a note).
- **Ownership/lifetime:** per G5 — entries adopted into the vector as
  created; the single scratch arena freed after the fence wait exactly like
  today's scratch pair; on failure the destructor tears down whatever exists.
- **Validation/errors:** loud gates for `maxInstanceCount`,
  `maxPrimitiveCount`, `maxGeometryCount`, `maxStorageBufferRange`; the
  existing base-address alignment guards preserved; runtime decoder
  acceptance now bounded by structural validation plus the alpha gate.
- **Vulkan synchronization:** the batched-build structure and the barrier #1
  `dstAccessMask` extension (`AS_READ | AS_WRITE`) from G5; barrier #2 and
  the no-frame-AS-barrier contract unchanged; all uploads and builds still on
  the borrowed frame-0 slot with the reset→submit→wait fence discipline.
- **Tests:** the Phase-3 gate flips in
  [tests/ogfx_decoder_tests.cpp](tests/ogfx_decoder_tests.cpp), governed by
  the audit rule (every `decodeModel` expectation whose schema-valid input
  only the record-count gates rejected flips here): `multipleMaterials`
  (:417-421) becomes runtime-accepted (all-opaque, untextured). The
  two-geometry fixtures do **not** flip — `makeTwoGeometryModel` deliberately
  marks its second geometry alpha-tested (:111), so
  `twoGeometry`/`oneMeshTwoGeometries` (:744-750) and `broadBytes` (:449,
  built from the same fixture) stay rejected with their expected diagnostic
  **changing from "M4 runtime record count" to the alpha gate's** — which is
  precisely the pin for the new non-first-geometry loop (`broad`
  additionally carries texture references, so it stays rejected through
  Phase 4 and beyond: alpha fires first). Add **all-opaque multi-record
  acceptance fixtures** — an all-opaque two-geometry/two-mesh variant and an
  all-opaque one-mesh-two-geometries variant — asserting runtime acceptance
  now matches schema acceptance. Texture/arena expectations are untouched
  (their gates hold until Phase 4). `scene_assembly` already covers the
  merged-index inputs. GPU acceptance: three objects at predicted
  placements; per-geometry color identity on the wedge; the rotated
  non-uniformly scaled instance lands at its predicted silhouette (its
  normal-transform visual is Phase 4's); resize and shutdown clean
  (teardown log lines show the vector teardown).
  **Synchronization-validation run required** (batched BLAS
  builds + arena reuse are exactly the hazard class it exists for), plus the
  release preset built and run.
- **Docs in-phase:** FORMATS.md's runtime-profile paragraph (record counts
  now runtime-accepted; the texture sentence stays true until Phase 4);
  ARCHITECTURE.md's acceleration-structure section, its ownership model
  (`AccelerationStructure` vectors + arena), its status line
  (N-BLAS/N-instance gallery), and the probe-tool rename.
- **Depends on:** Phase 2 (multi-model `SceneData` input).
- **Removes:** the one-mesh/one-instance/one-geometry AS gate (:275-283), the
  singular BLAS/scratch ownership, and the runtime record-count gates.
  **Keeps (deliberately):** the texture/arena gates (they fall with Phase 4's
  consumer); `instanceShaderBindingTableRecordOffset = 0`, geometry
  multiplier 0, `RAY_FLAG_FORCE_OPAQUE`, the single hit group and
  single-record hit region — the interim SBT contract that makes a Phase 3
  mis-render a loader/transform bug, never SBT routing. Also keeps the
  gradient shading for one more phase (the factor multiply already makes
  per-geometry identity visible on it; generic material shading arrives with
  Phase 4).

### Phase 4 — Texture foundation: the texture/arena gates fall with their consumer — features, resolver, DDS decode, sampled base color (flat-shaded quad + wedges)

**Lands:** G6, G7 (machinery; no root configured yet), G8, G9, and the
Phase-4 half of G1: the texture-reference and string-arena gates fall, and
reference reconstruction plus the 64 MiB cap become profile-independent, in
the same commit as their consumer. Visual: the gradient retires — the quad
shades `amber factor × white fallback × normal term` and the wedge faces go
flat blue/green, proving binding 4, the shared sampler, slot-0 fallback, and
the generic shading path with zero image files involved.

- **Files/APIs:** new `src/texture_loader.{hpp,cpp}` (+
  `tests/texture_loader_tests.cpp`); [src/scene.hpp](src/scene.hpp)
  (`SceneImageFormat`, `SceneImage.format`);
  [src/ogfx_decoder.cpp](src/ogfx_decoder.cpp) (**texture/arena gates
  removed; reference reconstruction and the 64 MiB cap become
  profile-independent** — the Phase-4 half of G1);
  [src/vulkan_context.cpp](src/vulkan_context.cpp) (Vulkan12Features
  consolidation + `textureCompressionBC` + the per-format capability probe +
  rejection report — G8);
  [src/gpu_scene.{hpp,cpp}](src/gpu_scene.hpp) (`MaxSceneTextures`,
  `SceneTexture` vector + shared sampler in `GpuScene`, the
  format→`VkFormat` map, a file-private staged image-upload helper, the
  descriptor-limit gate helper, image-count checks);
  [src/rt_pipeline.cpp](src/rt_pipeline.cpp) (binding 4, pool growth,
  `writeSceneDescriptorSet` writes the full array);
  [shaders/raytrace.slang](shaders/raytrace.slang) (binding 4 declaration +
  G9 shading); `tools/compile_probe_assets.cpp`
  (amber quad `baseColorFactor`); `src/gallery.cpp` calls
  `resolveSceneTextures(&scene, /*root*/ {})` after assembly (fallback
  creation; no references exist yet) and lands the G4 provenance capture +
  resolver-error wrapping; [CMakeLists.txt](CMakeLists.txt) (TU +
  test wiring).
- **Ownership/lifetime:** `GpuScene` owns the texture images/views and the
  sampler; destructor extends in reverse creation order (sampler → views →
  images → existing buffers), null-guarded, self-idle-waiting as today.
  Staging buffers are transient per image, dead after each borrowed-slot
  fence wait. The `SceneTexture` vector follows the G5 container contract:
  `reserve()`d to the scene image count before any image is created, handles
  adopted immediately on creation, and allocating steps on the `VkResult`
  path converting `std::bad_alloc`/`std::length_error` to
  `VK_ERROR_OUT_OF_HOST_MEMORY`.
- **Validation/errors:** the resolver/DDS rejection matrix (G6/G7) with named
  diagnostics; device-selection additions reported per category;
  `createGpuScene` rejects `images.empty()`, `images.size() >
  MaxSceneTextures`, `baseColorImage >= images.size()`, and pixel-size/format
  mismatches, all loud.
- **Vulkan synchronization/layouts:** per image, on the borrowed frame-0
  command buffer: staging copy → `UNDEFINED → TRANSFER_DST_OPTIMAL` barrier
  (`TOP/TRANSFER`, dstAccess `TRANSFER_WRITE`) → `vkCmdCopyBufferToImage`
  (tightly packed, `bufferRowLength = 0`; BC extents handled by block math) →
  `TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL` barrier with
  `dstStageMask = RAY_TRACING_SHADER`, `dstAccess SHADER_READ` — the image
  analogue of the D4 trailing buffer barrier, giving later trace submissions
  visibility without frame-path barriers. Descriptor writes declare
  `SHADER_READ_ONLY_OPTIMAL`. The storage-image and swapchain barrier chains
  are untouched. If a post-upload fence wait fails, the path falls back to
  `vkDeviceWaitIdle` before destroying transient staging resources, then
  propagates the error — staging is never destroyed while possibly in
  flight.
- **Tests:** the Phase-4 gate flips, governed by the audit rule (every
  `decodeModel` expectation whose schema-valid input only the texture/arena
  gates rejected flips here).
  [tests/ogfx_decoder_tests.cpp](tests/ogfx_decoder_tests.cpp) — both
  `texturedBytes` assertions in `testSchemaProfile` (:464-468, the
  `expectRejected` call *and* the error-text check) flip to acceptance with
  the reconstructed reference asserted equal to the schema result; the
  string-arena cases flip: :871-874 (valid unreferenced nonempty arena — its
  schema acceptance is already pinned at :876-880), :883-885 (offset-0 valid
  reference), and the `validLastReference` pair (~:890-901, with `"d"`
  equality); `broadBytes` stays rejected (alpha, per Phase 3).
  [tests/legacy_ogf_tests.cpp](tests/legacy_ogf_tests.cpp) —
  `testAcceptedProfile` (:394-398, always-run in every preset including
  `ogfx-core`) flips to assert `decodeModel` accepts the synthetic textured
  output and reconstructs `ston\synthetic_asymmetric`; `verifyCorpusOutput`
  (:718-723) flips to "accepts the corpus output and reconstructs
  `ston\ston_stena_marbl_m_03_back`" (the opt-in `xrPhotonM4aOfflineProof`
  target stays truthful; its pinned byte hashes are untouched because the
  writer does not change).
  [tests/ogfx_loader_tests.cpp](tests/ogfx_loader_tests.cpp) — a textured
  fixture: the reference survives adaptation with `baseColorImage == 0` and
  `images` still empty; G3's end-to-end merge case upgrades to a textured
  decoded model. New `texture_loader` suite — resolver accepts canonical
  names (`ston\ston_stena_marbl_m_03_back` →
  `ston/ston_stena_marbl_m_03_back.dds`); rejects dots, absolute shapes,
  `/`, drive/colon bytes, NUL/control bytes, out-of-set bytes, empty
  components, empty references — each by name; DDS decode of hand-built
  in-memory BC1/BC3 fixtures (correct level sizing incl. non-multiple-of-4
  dims), rejection of wrong magic/fourCC/cubemap/mip-count-inconsistency/
  truncation/trailing-bytes by named field; `resolveSceneTextures` —
  fallback always at index 0, untextured materials → 0, deduplication and
  stable first-use indices, references-without-root error, missing-file
  error, cumulative-cap rejection. GPU acceptance: three flat-shaded objects
  with visible normal-term falloff on the rotated wedge; **GPU-assisted
  validation run required** (descriptor-array misindexing is its class),
  plus plain validation clean. **Scope honesty:** this phase's GPU runs
  exercise binding 4, the full-array write, slot-0 fallback, and generic
  shading — they do **not** yet exercise BC image upload, descriptor indices
  > 0, divergent `NonUniformResourceIndex`, or the DDS→`VkFormat` mapping;
  those first run on a GPU in Phase 5 with the configured plitka texture
  (Phase 0 guarantees the asset exists by then), and BC decode correctness
  is pinned CPU-side here by the fixture unit tests.
- **Docs in-phase:** FORMATS.md's texture paragraphs — `decodeModel` now
  accepts the textured static opaque schema (alpha still rejected until the
  split); the carrier/resolver question recorded as decided: logical name →
  local DDS beneath a configured root, BC uploaded directly (G6/G7).
  ARCHITECTURE.md — module-map row for `texture_loader`; ownership model
  (`GpuScene` textures/views/sampler); the RT-pipeline section (binding 4,
  the Vulkan12Features consolidation, the descriptor-limit gate); the new
  device-feature requirements in the bring-up notes; the status line
  (generic material shading + fallback sampling).
- **Depends on:** Phases 1–3 (carrier present; merged scene; generalized
  shading on N instances) and Phase 0 (the pinned DDS profile).
- **Removes:** the UV-gradient probe shading, the "inert zero"
  `baseColorImage` state (FORMATS.md:443-447's placeholder wording becomes
  real fallback semantics), and the runtime texture/arena gates. **Keeps:**
  the alpha gate (split milestone); `FORCE_OPAQUE`, single hit group,
  `TMax = 100`, single-mip sampling (`SampleLevel(uv, 0)`).

### Phase 5 — The plitka1 gallery entry: optional configuration, loud failures, end-to-end render

**Lands:** G4's cache variables and the goal itself: the textured plitka
joins the quad and wedges. The wedge stays: it is a **permanent** generated
gallery entry — the repo-only N-BLAS/`GeometryIndex()` regression scene —
per the persistent, additive gallery idea; deleting it would return
unconfigured builds to a single-model scene.

- **Files/APIs:** [CMakeLists.txt](CMakeLists.txt)
  (`XRPHOTON_GALLERY_PLITKA_OGFX`, `XRPHOTON_GALLERY_TEXTURE_ROOT` cache
  variables + compile definitions, documented next to the existing quad-path
  definition as the same temporary policy); `src/gallery.cpp` (plitka entry
  marked optional, skip/fail semantics, texture root passed to
  `resolveSceneTextures`, per-entry log lines incl. resolved-texture counts).
- **Ownership/lifetime:** unchanged — plitka is just another row producing
  ordinary `SceneData`.
- **Validation/errors:** the complete skip-vs-fail behavior of G4 and the G7
  missing-texture diagnostics become reachable end-to-end; an unconfigured
  build renders the generated entries (quad + two wedges) and prints the
  skip line.
- **Vulkan:** no new mechanisms — this phase exists to prove the previous
  four on real content through the unchanged generic path (the core rule's
  acceptance test: the diff contains no renderer change).
- **Tests:** no new unit suites (resolver/assembly behavior already pinned).
  Full GPU/manual acceptance (§6 checklist): all four objects at predicted
  positions through one path; **first GPU exercise of the compressed path**
  — BC image creation/upload, a descriptor index > 0, divergent
  `NonUniformResourceIndex` (fallback slot 0 and plitka's index in one
  frame), and the DDS→`VkFormat` mapping — under a GPU-assisted validation
  pass over the configured gallery; plitka's UV orientation, scale, and
  marble texture verified (Blender import of `plitka1.ogf` as the visual
  oracle); winding and stored-normal sign positively checked once via G9's
  two transient diagnostics (`HitKind()` facing and signed-normal coloring;
  run hands-off, reverted before commit —
  the standing shading is sign-blind); quad and wedges unchanged;
  resize/shutdown clean; plain validation and a synchronization-validation
  pass; debug and release presets run; `ogfx-core` preset re-verified
  untouched.
- **Docs in-phase:** README gains the gallery configuration section (the two
  `-D` lines and the texture-root layout note) — it is the operational knob
  landing in this commit; ARCHITECTURE.md's status line (textured
  multi-model gallery with the optional legacy entry). The broader narrative
  updates wait one phase.
- **Depends on:** Phases 1–4, plus the **owner-supplied texture** (§8); the
  commit is fully reviewable without it (unconfigured = the generated
  entries only), but the
  milestone's acceptance needs the file.
- **Removes:** nothing structural — the phase adds the first optional
  real-content entry. **Keeps:** the code-owned tables (until the
  scene/level owner), the wedge probe (permanent), the
  embedded-absolute-path policy (until the asset root), the opt-in proof
  target exactly as is.

### Phase 6 — Documentation reconciliation and the permanent gallery ordering

**Lands:** the consolidation pass. Contract-level documentation already
moved in-phase (ARCHITECTURE.md module rows, ownership, AS and RT-pipeline
sections, and status across Phases 2–5; FORMATS.md runtime-profile and
texture paragraphs in Phases 3–4; README configuration in Phase 5) — a
source-of-truth document is never left stale across commits;
this phase owns the narrative and roadmap reconciliation:

- **ARCHITECTURE.md:** roadmap step 2 rewritten around the
  **permanent gallery ordering** (quad ✓ → plitka1 ✓ → Blender probe →
  `bochka_fuel`) with the gallery defined as an additive preview/integration
  scene on one runtime path (status, ownership, and pipeline sections were
  already updated in Phases 3–5). The rewrite **explicitly supersedes** the prior
  ordering statements — the Blender probe as the next milestone, and the
  original GEOMETRY_PLAN sequence that put the opaque/alpha SBT split ahead
  of any texture consumer — rather than leaving them merely annotated.
- **GEOMETRY_PLAN.md:** the header notice and the N-BLAS/texture reference
  sections gain "landed via GALLERY_PLAN.md" annotations (history preserved,
  not rewritten); the split milestone remains the next structural item.
- **FORMATS.md:** final coherence pass over the paragraphs already updated
  in Phases 3–4 (runtime profile, texture carrier/resolver decision); M4a's
  "not runtime-ready" wording now points at the landed consumer.
- **README.md:** status paragraph (real legacy content rendered; gallery
  described; next: Blender probe as third entry).
- **CLAUDE.md:** layout/summary/"Next step" rewritten to match (gallery
  landed; Blender probe next). *Note:* CLAUDE.md is gitignored
  ([.gitignore:13](.gitignore)), so this edit is machine-local and will not
  appear in the commit — called out so its absence from the diff is not
  mistaken for an omission.
- `GALLERY_PLAN.md` itself gains a landed-status banner (the GEOMETRY_PLAN
  historical-notice pattern).

**Depends on:** Phase 5. Exit: all presets build, all tests pass, docs
describe the tree as it is.

## 6. Testing and validation

### Offline/unit coverage → phase map (every brief obligation)

| Obligation | Phase | Suite |
|---|---|---|
| Runtime decoding accepts the textured `plitka1` profile | 4 | `ogfx_decoder` texture/arena flips (+ `legacy_ogf_adapter`'s `testAcceptedProfile` :394-398 and `verifyCorpusOutput` :718-723 flips) |
| Runtime decoding accepts multi-record opaque models | 3 | `ogfx_decoder` (all-opaque acceptance fixtures; `multipleMaterials` flip) |
| Runtime decoding still rejects alpha-tested / unsupported capabilities | 3 | `ogfx_decoder` (existing two-geometry fixtures re-pinned to the alpha diagnostic — the non-first-flag loop) |
| Logical texture references survive merging / runtime adaptation | 2 / 4 | `scene_assembly` (hand-built strings) / `ogfx_runtime_loader` (textured fixture) |
| Multi-model merging rebases every record type | 2 | `scene_assembly` (offset-column matrix) |
| Multiple instances of one mesh | 2 (unit), 3 (GPU) | `scene_assembly` (`appendSceneInstance`); the wedge placed twice on screen |
| Overflow and invalid-reference diagnostics | 2 | `scene_assembly` (count-helper boundaries; meshIndex/precondition/`validateAssembledScene` rejections) |
| Resolver accepts canonical logical names | 4 | `texture_loader` |
| Resolver rejects traversal/absolute/malformed/control-byte/unsupported/missing | 4 | `texture_loader` |
| Untextured materials map to the white fallback | 4 | `texture_loader` (`resolveSceneTextures`) |
| Referenced textures map to stable scene image indices | 4 | `texture_loader` (dedup + first-use order) |
| Existing writer/decoder tests unchanged and deterministic | 1–6 | `ogfx_core` untouched; `ogfx_decoder` and `legacy_ogf_adapter` changes confined to the per-phase gate-flip lists (Phases 3–4); pinned corpus hashes unchanged |

### GPU/manual acceptance → phase map

| Obligation | Phase |
|---|---|
| Quad renders unchanged through the assembly path | 2 |
| Two distinct BLASes; shared BLAS with two instances; `GeometryIndex() > 0` color identity; non-uniform-scale silhouette | 3 |
| Sync validation clean over batched builds + scratch-arena reuse | 3 (repeated at 5) |
| Fallback sampling + descriptor array + generic shading (no image files); normal transform visible on the rotated wedge under view-dependent shading | 4 |
| GPU-assisted validation clean | 4 (scoped — see Phase 4's coverage note), full compressed path at 5 |
| First BC upload; descriptor index > 0; divergent `NonUniformResourceIndex`; DDS→`VkFormat` mapping on GPU | 5 |
| Quad + wedges + plitka together; one generic path; plitka UV/scale/texture correct; winding + stored-normal sign via the two transient diagnostics | 5 |
| Camera framing and `TMax` accommodate the gallery (G10) | 3/5 |
| Resize and shutdown clean (teardown log lines) | every phase; explicitly re-checked 3 and 5 |
| Debug and release presets build and run | every phase; release *run* at 3 and 5 |
| `ogfx-core` remains Vulkan-free and passes | every phase |

### Run practicalities (from the project's validation experience)

- Synchronization-validation and GPU-assisted validation are **separate
  passes**, never co-enabled (GEOMETRY_PLAN §6).
- Sync-validation runs need `DISABLE_MANGOHUD=1` (the overlay's own Vulkan
  work otherwise pollutes the report), and logs are captured with line
  buffering so the last messages before any exit survive:
  `DISABLE_MANGOHUD=1 stdbuf -oL -eL ./build/debug/xrPhoton 2>&1 | tee /tmp/xrphoton-sync.log`
  with the layer feature enabled via vkconfig or layer settings.
- Manual visual checks happen hands-off after launch: stray mouse clicks
  recapture the cursor and yaw the camera, which invalidates any
  screenshot-comparison oracle; the startup pose is the reference view.
- "Validation silent" only counts after confirming the startup log does
  **not** contain the "validation layer is not available" warning.

## 7. Scope boundaries (binding)

**In scope:** two OGFx files in one scene; N-BLAS/N-instance static scene
support; opaque base-color texture resolution and sampling; optional local
legacy gallery configuration; tests and roadmap reconciliation.

**Out of scope:** runtime loading of original `.ogf`; source-specific runtime
adapters; Blender exporter implementation; `bochka_fuel` conversion;
alpha-tested any-hit shaders; the opaque/alpha SBT split; general
translucency; normal/roughness/metalness textures; mip generation beyond what
the minimum chosen image path requires (none — mip 0 of existing chains is
read, nothing is generated); dynamic TLAS refits; skinning; scene/level file
ownership of instances; path-tracing lighting, accumulation, or denoising.

## 8. Owner decisions and prerequisites

1. **The texture asset (Phase 0 — needed before Phase 4 is implemented so
   the DDS profile is pinned against the real file, and blocking Phase 5
   acceptance).** Please provide the
   original S.T.A.L.K.E.R. Shadow of Chernobyl texture for the logical
   reference `ston\ston_stena_marbl_m_03_back` — the file
   **`ston_stena_marbl_m_03_back.dds`** from the game's texture data (in the
   unpacked archives it lives at `gamedata/textures/ston/ston_stena_marbl_m_03_back.dds`),
   expected to be a DDS with a DXT1 or DXT5 payload. Place it under a local
   directory of your choosing **preserving the `ston/` subdirectory** (i.e.
   `<your-texture-root>/ston/ston_stena_marbl_m_03_back.dds`) and pass that
   root at configure time via `-DXRPHOTON_GALLERY_TEXTURE_ROOT=…`. Like the
   OGF corpus, it must never be committed. If the file turns out to carry a
   different DDS variant (uncompressed, `DXT3`, `DX10` header), report the
   header's fourCC/flags — the pinned decode profile (G6) is then extended
   deliberately for that variant rather than guessed in advance.
2. **Missing-texture policy confirmation.** This plan implements the loud
   default: a configured gallery entry whose referenced texture cannot be
   resolved or decoded aborts startup with the G7 diagnostic. If you instead
   want a visible dev placeholder policy, say so before Phase 4; silence
   means loud.
3. **plitka source-path choice.** The recommended configure value is the
   opt-in proof output
   `build/ogfx-core/corpus/meshes/objects/dynamics/plitka/plitka1.ogfx`
   (hash-pinned by `xrPhotonM4aOfflineProof`); any locally converted copy
   with the same bytes works, since the cache variable takes an arbitrary
   absolute path.
4. **Roadmap reordering sign-off.** Phase 6 rewrites the step-2 ordering
   (gallery/plitka before the Blender probe; probe becomes gallery entry 3).
   This plan assumes assent; the docs commit is where it becomes recorded
   project direction.
5. **Commit cadence.** Six commits as phased above (Phase 0 makes none; the
   owner commits each); `GALLERY_PLAN.md` itself lands at the repo root
   before Phase 1.

## 9. Risks and open questions

1. **Real-corpus DDS variance** — resolved by construction: Phase 0 inspects
   the actual file and pins G6 before any parser code exists; a surprising
   variant becomes a deliberate profile extension recorded in this document,
   never a mid-implementation discovery.
2. **plitka orientation/winding on screen** — M4a validated winding against
   normals offline, but the first *rendered* legacy mesh is where a
   pass-through assumption would finally show; the Blender visual oracle and
   the both-sides `abs(dot)` shading term bound the blast radius; a real
   discrepancy is an offline-adapter fix, never a runtime one (core rule).
3. **Sync-validation noise on batched builds** — layer versions vary in how
   they model per-region scratch access; the disjoint-aligned-region
   discipline plus barrier #1's added write bit is the spec-clean shape, and
   any residual report is triaged against it before touching barriers.
4. **Descriptor-limit gate on exotic drivers** — 1024 combined samplers is
   far under desktop RT-class limits, but the gate turns an exotic failure
   into a named selection rejection instead of a mystery.
5. **The retired gradient's residual diagnostic value** — accepted and
   mitigated in G9 (live UV/normal paths, CPU pins, the documented debug
   swap); if a future ABI regression proves painful to localize, the answer
   is a *temporary* debug swap, not a permanent branch.
6. **Gate/consumer drift** — eliminated by construction: G1's atomicity rule
   means no commit boundary exists where `decodeModel` accepts data the
   engine of that commit cannot render faithfully. The residual risk is a
   future milestone forgetting the rule — which is why G1 records it as
   binding, not as this milestone's local choice.
