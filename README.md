# xrPhoton

X-Ray Photon Engine. A rebuild of the X-Ray engine from the S.T.A.L.K.E.R.
series in modern C++ on Vulkan, with a hardware ray-tracing renderer being
built specifically for path tracing and no raster fallback.
The plan is to eventually build a standalone game on it, similar to the old
STALKER games but with overhauled systems.

## One renderer

This is a one-man project, and what makes that workable is a single focus and
a clear vision: the engine does each thing one way, chosen deliberately. It
keeps things lean as a side effect. The original X-Ray maintained several renderers in parallel
(static lighting, dynamic lighting, one per DirectX generation). Here there is
exactly one rendering path. No lightmaps, no shadow maps, no per-renderer
material variants. Everything from the sun to a flashlight goes through the
same light transport, which suits STALKER's dynamic weather and day/night
cycle anyway.

The trade-off is steep hardware requirements: a GPU with hardware ray tracing
support is mandatory, and there is no fallback path for anything less.

The rest of the engine will follow the same idea as it grows: one focus, one
way of doing each thing.

## Status

Right now it renders a compact OGFx test yard that you can explore with a basic
collision-aware capsule character. WASD runs, Left Shift sprints, Space jumps,
Left Ctrl crouches, and F1 switches to and from the original collision-free fly
camera; the player
is fully suspended while that free camera is active. Each switch into free-camera
mode starts it at the player's current position and view. Escape
releases the captured mouse and left click recaptures it. Every build includes
generated ground, wall, and unit-box
models assembled into a walled yard, platform, staircase, and crates, plus an
indexed quad and a permanent two-geometry wedge regression probe placed twice.
One ochre crate now spawns above the yard, falls, tumbles, settles, and sleeps
under Jolt Physics control. Physics writes its rigid transform back to
`SceneData`; the existing renderer then rewrites one fence-protected
instance-input slot and rebuilds the shared TLAS in place before every trace
while all BLAS geometry stays static. A configured reference build
adds the converted, textured legacy `plitka1.ogfx` as another yard placement
through the same runtime path.
That plitka configuration reaches the interactive render loop, passes plain,
GPU-assisted, and synchronization validation, and has been visually checked in
the complete yard.
The converted `test_pyramid.ogfx`, flat-shaded `test_sphere.ogfx`,
`test_smooth_sphere.ogfx`, and alpha-tested `test_leaf_card.ogfx` can be
configured independently as Blender yard placements. The converted regular
`bochka_close_1.ogfx` is another yard model: its `mtl\mtl_barrel_01` reference
uses the same DDS path, while its preserved three-cylinder recipe now creates a
dynamic compound body that tips and rolls from its configured spawn.
The adjacent `remade_bochka_close_1.ogfx` is a Blender-authored, scale-faithful
visual remake with 8,381 unified vertices, 15,944 triangles, and the owner-local
opaque BC1 texture `xrphoton\remade_bochka_close_1_basecolor`; it has no physics
recipe and remains static. The third adjacent drum,
`custom_stalker_barrel.ogfx`, is an original Stalker-style design: a dented
192-segment shell, three rolled ribs, recessed dark-steel lid, two modeled bungs,
weld seam, riveted ochre inspection plate, and modeled warning bars. It exports
11,296 unified vertices / 19,128 triangles through the same single opaque DDS
profile, now backed by the untouched 4096×4096 `rusty_metal_04` Poly Haven CC0
diffuse map in an uncompressed RGBA8 DDS, and likewise remains static without a
physics recipe. The converted
`item_psevdodog_tail.ogfx` follows that three-barrel comparison. Its one mesh keeps
two geometries in source order: a `models\model_aref` alpha-tested range and a
`models\model` opaque range, both using `act\act_pseudodog_fur` through one
shared material with the source cutoff of 128/255. Its single rigid box-body
recipe creates the third dynamic body and settles from its unchanged platform
pose. All three configured SoC assets retain their authored scale: plitka stays
at its static rotated/translated placement, the barrel spawns at y = 0.6 with a
20-degree roll, and the tail keeps its platform transform.
The generated-only yard contains 5 models/BLASes, 13 TLAS instances, and 6
geometries. With every optional asset enabled, it contains 14 models/BLASes,
22 TLAS instances, and 16 geometries; the wedge remains the shared-BLAS probe.
The two spheres have identical geometry and UV corner streams but deliberately
different normal sharing, making flat-versus-smooth shading directly observable. The
shipped tail DDS has no transparent texels in mip 0, so that asset proves mixed
opaque/alpha-tested routing and real texture sampling through the any-hit stage.
The Blender leaf card closes the visual acceptance gap: its pinned
`trees\trees_new_vetka_green` DXT1 mip contains 153,894 transparent texels, and
the running yard visibly reveals the miss background through those samples.
That said, the whole ray tracing stack is already behind it: every frame traces
a ray per pixel through one BLAS per mesh and a real multi-instance TLAS with
`vkCmdTraceRaysKHR` from a
perspective camera fed to the shader via push constants, writes a storage image
and blits it to the swapchain, with two frames in flight and proper resize
handling. The four-stage/four-group pipeline selects an opaque or alpha-tested
hit record per geometry, marks only opaque BLAS ranges opaque, and lets the
alpha-tested any-hit shader compare sampled texture alpha against the material
cutoff. The shared C++/Slang routing ABI currently has `RayTypeCount = 1`, and
rays are no longer forced opaque. Shaders are written in
[Slang](https://shader-slang.org/) and compiled into the runtime binary at build
time, so shader deployment is self-contained and needs no runtime shader files.

The first complete OGFx round trip, the narrow M4a legacy-static converter, and
the first headless Blender-to-OGFx path have landed. Every normal engine build
generates `build/<preset>/assets/probes/test_quad.ogfx`, `test_wedge.ogfx`, and
the three `test_yard_*.ogfx` models through the canonical writer; the runtime
strictly decodes all five, assembles their model records and world placements
into `SceneData`, batches five BLAS builds, and shares both the wedge and unit-box
BLASes across multiple TLAS instances. The texture foundation now reconstructs
logical OGFx references, resolves strict DDS DXT1/DXT5 or canonical uncompressed
RGBA8 images, uploads their mip-0 payloads directly, and exposes a fixed
sampled-image array with an opaque-white fallback. The generated probes exercise the fallback with an amber quad and
blue/green wedge faces; configured plitka exercises the real BC1 upload and a
nonzero texture-table index. The separate converter accepts pinned OGF v4
flat-static and rigid-compound profiles and feeds that same writer for offline validation. The converted
plitka is now the first original X-Ray model carried through xrPhoton's configured
runtime path. The Blender slice uses Blender
5.1.x and [`tools/blender/export_ogfx.py`](tools/blender/export_ogfx.py) to extract
one explicitly named static mesh. A private `XRBM` stream crosses
stdin to the C++ adapter, which performs coordinate/normal/winding conversion and
feeds the same canonical writer used by every other source. `test_pyramid` is the
first yard probe; the flat-shaded `test_sphere` exercises dense triangulation,
its UV seam, and corner splitting; and `test_smooth_sphere` proves that equal
positions share smooth normals while UV seams remain split. Material-free inputs
remain byte-compatible XRBM v1. The strict v2 profile adds exactly one opaque or
alpha-tested Blender material, derives one logical DDS reference from its direct
Image Texture node, carries the classification/cutoff, and flips textured V once
for DDS/Vulkan top-row sampling. `test_leaf_card` is its alpha-tested consumer
and visible `IgnoreHit` proof; `remade_bochka_close_1` proves the opaque-textured
branch with a newly authored asset, and `custom_stalker_barrel` proves a fully
original production-detail design within that same contract. All six use the
optional yard path. The regular barrel adds the first narrow type-`0xA`
rigid-compound legacy profile: its bind/model-space child mesh is flattened to
ordinary render geometry and its three cylinder records, masses, centers of
mass, source material, and source-node names enter optional OGFx metadata. The
pseudodog tail extends that same narrow adapter with its validated progressive
and static child forms, mixed opaque/alpha-tested render semantics, shared
material cutoff, and one box collider.
The runtime loader now carries those backend-neutral recipes into `SceneData`,
and the engine-side `PhysicsWorld` turns the generated crate plus configured
barrel and tail placements into live Jolt bodies. Every other yard instance is
static collision geometry. Simulation uses a 60 Hz fixed-step accumulator,
clamps each frame contribution to 0.1 seconds, enables linear-cast motion for
dynamic bodies, and publishes body-origin transforms atomically back to the
scene. Headless tests cover settling and sleep, lifecycle, input contracts,
shape/math bridging, static mesh construction, determinism, capacity failure,
adversarial numeric boundaries, the robust 500 m/s velocity clamp, and CCD
without creating Vulkan objects.

Deformable skinning and BLAS refits, actual path tracing with lights, and
temporal accumulation and denoising follow later. Details in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Building

Development build with Vulkan validation requested:

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/xrPhoton
```

Optimized build with debug information and Vulkan validation disabled:

```sh
cmake --preset release
cmake --build --preset release
./build/release/xrPhoton
```

Jolt Physics **v5.6.0** is pinned and vendored under
[`third_party/jolt`](third_party/jolt). CMake builds it as a static,
engine-only dependency; configuration never downloads it and no system Jolt
package is required. Jolt is MIT-licensed, with its verbatim license at
[`third_party/jolt/LICENSE`](third_party/jolt/LICENSE). The one local
thread-pool exception-safety fix is recorded in
[`third_party/jolt/XRPHOTON_PATCHES.md`](third_party/jolt/XRPHOTON_PATCHES.md).

The offline OGFx writer, decoder, and their tests can be configured without
Vulkan, GLFW, GLM, Slang, or a GPU SDK:

```sh
cmake --preset ogfx-core
cmake --build --preset ogfx-core
ctest --preset ogfx-core
```

That graphics-free configuration can also generate the probe asset explicitly:

```sh
cmake --build --preset ogfx-core --target xrPhotonAssets
```

The `ogfx-core` preset returns before the engine dependency section, so it does
not configure, build, or test Jolt.

It also builds the narrow legacy OGF converter:

```sh
./build/ogfx-core/xrPhotonAssetCompiler convert-ogf input.ogf output.ogfx
```

The command intentionally dispatches between only two documented profiles: the
M4a flat-static slice and the narrowly validated SoC rigid-compound slices used
by `bochka_close_1` and `item_psevdodog_tail`. It is not general skeletal
support. Their outputs preserve a logical texture name that the runtime
reconstructs for scene-global DDS resolution and can be supplied directly to
the optional yard configuration below.

### Converting the Blender probes

The first Blender exporter is deliberately headless rather than a polished add-on
UI. Build the graphics-free compiler, then run the script with Blender 5.1.x (not
the system Python interpreter):

```sh
cmake --build --preset ogfx-core --target xrPhotonAssetCompiler
/path/to/blender --background --factory-startup --disable-autoexec \
  --python-exit-code 1 blender/test_pyramid.blend \
  --python tools/blender/export_ogfx.py -- \
  --compiler "$PWD/build/ogfx-core/xrPhotonAssetCompiler" \
  --output "$PWD/build/ogfx-core/assets/blender/test_pyramid.ogfx" \
  --object test_pyramid
```

The checked input is exactly one explicitly named mesh object: no modifiers,
animation, shape keys, constraints, parenting, or color attributes; linked-library
data and overrides are also rejected. Material-free XRBM v1 accepts zero or one
UV layer. XRBM v2 accepts exactly one local material used by every polygon, one
UV layer, one direct Image Texture → Principled Base Color graph, the Boolean
`xrphoton_alpha_tested` custom property, and a lowercase `.dds` using
exact sRGB/Straight-alpha interpretation beneath the supplied `--texture-root`;
the image Alpha output must also feed Principled Alpha exactly when that property
is true. Opaque inputs leave Alpha unlinked and use the canonical unused cutoff
of 0.5. The graph must be unmuted, unanimated, and use identity texture/color
mapping. Blended or more elaborate materials fail loudly. Scene units and the object's affine
transform are baked by the compiler; a one-million-triangle cap bounds peak
working memory. Blender source files live in the Git-ignored
root `blender/` directory; reproducible outputs live under the Git-ignored
`build/<preset>/assets/blender/` directory. The exporter does not write OGFx: it
sends a bounded private `XRBM` extraction stream to
`xrPhotonAssetCompiler convert-blender`, and the shared C++ writer publishes the
canonical file.

For the four local regression sources, configure their ignored paths once and run
the opt-in end-to-end proof:

```sh
cmake --preset ogfx-core \
  -DXRPHOTON_BLENDER_EXECUTABLE=/path/to/blender \
  -DXRPHOTON_BLENDER_PYRAMID_BLEND="$PWD/blender/test_pyramid.blend" \
  -DXRPHOTON_BLENDER_SPHERE_BLEND="$PWD/blender/test_sphere.blend" \
  -DXRPHOTON_BLENDER_SMOOTH_SPHERE_BLEND="$PWD/blender/test_smooth_sphere.blend" \
  -DXRPHOTON_BLENDER_LEAF_CARD_BLEND="$PWD/blender/test_leaf_card.blend" \
  -DXRPHOTON_BLENDER_LEAF_TEXTURE_ROOT="$PWD/original_game_files/soc/textures"
cmake --build --preset ogfx-core --target xrPhotonBlenderOfflineProof
```

It runs the real Blender extractor and compiler twice for each fixture, verifies
deterministic canonical schema reconstruction, and persists
`test_pyramid.ogfx`, `test_sphere.ogfx`, `test_smooth_sphere.ogfx`, and
`test_leaf_card.ogfx` under `build/ogfx-core/assets/blender/`. It also pins the
leaf DDS identity and proves that 153,894 mip-0 texels select BC1 transparency.
Normal builds do not depend on Blender or the ignored `.blend` files.

The scale-faithful barrel remake has a separate opt-in proof because its source
`.blend`, authored PNG, and runtime DDS are owner-local:

```sh
cmake --preset ogfx-core \
  -DXRPHOTON_BLENDER_EXECUTABLE=/path/to/blender \
  -DXRPHOTON_BLENDER_REMADE_BARREL_BLEND="$PWD/blender/remade_bochka_close_1.blend"
cmake --build --preset ogfx-core --target xrPhotonRemadeBarrelOfflineProof
```

It runs the real Blender export twice, checks byte identity and canonical OGFx
round-trip reconstruction, verifies the 8,381-vertex/15,944-triangle opaque
material result and SoC-scale bounds, pins the complete 12-mip 1024×2048 DXT1
texture, and publishes
`build/ogfx-core/assets/blender/remade_bochka_close_1.ogfx`.

The original custom barrel uses the parallel owner-local proof:

```sh
cmake --build --preset ogfx-core --target xrPhotonCustomBarrelOfflineProof
```

That proof pins one opaque 11,296-vertex / 19,128-triangle model, its one-metre
drum bounds, bounded 0..1 UVs, and the native 4096×4096, mip-0-only uncompressed
RGBA8 material sheet. It publishes
`build/ogfx-core/assets/blender/custom_stalker_barrel.ogfx`.

### Proving the legacy plitka conversion

With the local `plitka1.ogf` source file in
`original_game_files/soc/meshes/objects/dynamics/plitka/`, run the complete,
opt-in offline proof with:

```sh
cmake --build --preset ogfx-core --target xrPhotonM4aOfflineProof
```

It verifies the pinned source and output identities, conversion semantics,
canonical schema round trip, and exact runtime texture-reference reconstruction.
The persistent result is written to
`build/ogfx-core/assets/soc/meshes/objects/dynamics/plitka/plitka1.ogfx`. A different
local source location can be selected at configure time with
`-DXRPHOTON_M4A_CORPUS_OGF=/path/to/plitka1.ogf`; the source tree remains
untracked and normal builds do not depend on it.

### Proving the regular barrel conversion

With the local source at
`original_game_files/soc/meshes/physics/balon/bochka_close_1.ogf`, run:

```sh
cmake --build --preset ogfx-core --target xrPhotonRigidOgfOfflineProof
```

The opt-in proof checks the exact source identity, runs the real converter
twice, compares the outputs byte-for-byte, verifies bind-pose render flattening
and all body/cylinder fields, reconstructs the complete OGFx schema, and pins the
19,352-byte output identity. It persists
`build/ogfx-core/assets/soc/meshes/physics/balon/bochka_close_1.ogfx`. Override
the ignored local source path with
`-DXRPHOTON_RIGID_BARREL_CORPUS_OGF=/path/to/bochka_close_1.ogf` if needed.
No physics backend is selected or run by this proof.

### Proving the mixed pseudodog-tail conversion

With the local source at
`original_game_files/soc/meshes/equipments/item_psevdodog_tail.ogf`, run:

```sh
cmake --build --preset ogfx-core --target xrPhotonAlphaOgfOfflineProof
```

This opt-in proof pins the source identity, runs the real conversion twice,
checks deterministic canonical output, and pins the 34,921-byte result: 930
vertices and 1,116 retained indices across one mesh and two geometries. The
alpha-tested range has 856 vertices/864 indices; the opaque range has 74
vertices and the 252 max-detail indices selected from its 1,209-index progressive
source. The proof also verifies the shared `act\act_pseudodog_fur` material with
its exact 128/255 cutoff and single rigid box-body metadata, pins the 21,992-byte
`act/act_pseudodog_fur.dds`, and thereby fixes the exact shipped texture whose
mip 0 has no transparent texels. It persists
`build/ogfx-core/assets/soc/meshes/equipments/item_psevdodog_tail.ogfx`.
Override the ignored local inputs with
`-DXRPHOTON_ALPHA_TAIL_CORPUS_OGF=/path/to/item_psevdodog_tail.ogf` and
`-DXRPHOTON_ALPHA_TAIL_TEXTURE_DDS=/path/to/act_pseudodog_fur.dds` if needed.
The proof selects no physics backend and performs no simulation.

### Configuring the optional yard entries

The generated quad and wedge need no local source files. To add the verified
plitka, regular-barrel, and pseudodog-tail outputs, their original textures, and
all six converted Blender assets to the debug yard, configure the
owner-local paths once, then build normally:

```sh
cmake --preset debug \
  -DXRPHOTON_GALLERY_PLITKA_OGFX="$PWD/build/ogfx-core/assets/soc/meshes/objects/dynamics/plitka/plitka1.ogfx" \
  -DXRPHOTON_GALLERY_BLENDER_OGFX="$PWD/build/ogfx-core/assets/blender/test_pyramid.ogfx" \
  -DXRPHOTON_GALLERY_BLENDER_SPHERE_OGFX="$PWD/build/ogfx-core/assets/blender/test_sphere.ogfx" \
  -DXRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX="$PWD/build/ogfx-core/assets/blender/test_smooth_sphere.ogfx" \
  -DXRPHOTON_GALLERY_BLENDER_LEAF_CARD_OGFX="$PWD/build/ogfx-core/assets/blender/test_leaf_card.ogfx" \
  -DXRPHOTON_GALLERY_BARREL_OGFX="$PWD/build/ogfx-core/assets/soc/meshes/physics/balon/bochka_close_1.ogfx" \
  -DXRPHOTON_GALLERY_REMADE_BARREL_OGFX="$PWD/build/ogfx-core/assets/blender/remade_bochka_close_1.ogfx" \
  -DXRPHOTON_GALLERY_CUSTOM_BARREL_OGFX="$PWD/build/ogfx-core/assets/blender/custom_stalker_barrel.ogfx" \
  -DXRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX="$PWD/build/ogfx-core/assets/soc/meshes/equipments/item_psevdodog_tail.ogfx" \
  -DXRPHOTON_GALLERY_TEXTURE_ROOT="$PWD/original_game_files/soc/textures"
cmake --build --preset debug
./build/debug/xrPhoton
```

The texture root must preserve the exact-case relative paths
`ston/ston_stena_marbl_m_03_back.dds`, `mtl/mtl_barrel_01.dds`,
`act/act_pseudodog_fur.dds`, and `trees/trees_new_vetka_green.dds`. The shipped
tail texture's mip 0 is fully opaque; the leaf texture supplies the complementary
visible cutout acceptance and shows the miss background through rejected texels.
The yard resolves owner-local `blender/textures` first and the configured
legacy root second, so the remade barrel needs no copied SoC file and an authored
path deterministically shadows a same-named legacy path.
CMake remembers these values
separately in each build tree, so configure the `release` preset the same way
when needed. With
an empty `XRPHOTON_GALLERY_PLITKA_OGFX`,
`XRPHOTON_GALLERY_BLENDER_OGFX`, or
`XRPHOTON_GALLERY_BLENDER_SPHERE_OGFX`, or
`XRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX`, or
`XRPHOTON_GALLERY_BLENDER_LEAF_CARD_OGFX`, or
`XRPHOTON_GALLERY_BARREL_OGFX`, or
`XRPHOTON_GALLERY_REMADE_BARREL_OGFX`, or
`XRPHOTON_GALLERY_CUSTOM_BARREL_OGFX`, or
`XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX`, xrPhoton skips that optional entry. Once
an entry is configured, a missing/broken OGFx—or referenced texture root/DDS—is a
loud startup failure rather than a silent fallback. The original game files,
Blender sources, Blender textures, and generated proof outputs remain Git-ignored
local inputs.

The generated crate is always present in `dynamicInstances`. The regular barrel
and pseudodog tail join that set only when their optional entries are configured;
an unconfigured optional dynamic is skipped, while a configured one must load as
exactly one mesh with exactly one rigid-body recipe. The plitka and every Blender
entry remain static regardless of placement.

The current build and development environment is Linux with GCC or Clang.
Windows support is planned, but its build and platform integration have not landed yet.

Needs currently:

- C++23 and CMake 3.24+
- Vulkan SDK (1.3)
- GLFW 3
- GLM (`libglm-dev` on Debian/Ubuntu)
- `slangc` (use an official release; distro packages are usually too old)
- A GPU/driver with `VK_KHR_acceleration_structure` and
  `VK_KHR_ray_tracing_pipeline`

## Docs

[ARCHITECTURE.md](ARCHITECTURE.md) — module map, ownership and lifetime model,
per-frame flow, synchronization, roadmap.

[FORMATS.md](FORMATS.md) — asset-format plan: OGFx, OMFx, and the shared
offline asset compiler.

[SDK.md](SDK.md) — plan for the modern SDK successor.
