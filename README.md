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

Right now it renders an additive OGFx preview gallery that you can fly around
(WASD + mouse look). Every build includes one indexed quad and a permanent
two-geometry wedge regression probe placed twice, including one rotated and
non-uniformly scaled instance. A configured reference build adds the converted,
textured legacy `plitka1.ogfx` as a fourth placement through the same runtime path.
That configured path reaches the interactive render loop and passes plain,
GPU-assisted, and synchronization validation; plitka's final on-screen orientation,
scale, winding, and texture appearance still require owner visual sign-off.
That said, the whole ray tracing stack is already behind it: every frame traces
a ray per pixel through one BLAS per mesh and a real multi-instance TLAS with
`vkCmdTraceRaysKHR` from a
perspective camera fed to the shader via push constants, writes a storage image
and blits it to the swapchain, with two frames in flight and proper resize
handling. Shaders are written in [Slang](https://shader-slang.org/) and compiled
into the runtime binary at build time, so shader deployment is self-contained
and needs no runtime shader files.

The first complete OGFx round trip and the narrow M4a legacy-static converter
have landed. Every normal engine build generates
`build/<preset>/assets/test_quad.ogfx` and `test_wedge.ogfx` through the canonical
writer; the runtime strictly decodes both, assembles their model records and world
placements into `SceneData`, batches two different BLAS builds, and shares the
wedge BLAS across two TLAS instances. The texture foundation now reconstructs
logical OGFx references, resolves strict DDS DXT1/DXT5 images, uploads BC1/BC3
payloads directly, and exposes a fixed sampled-image array with an opaque-white
fallback. The generated probes exercise the fallback with an amber quad and
blue/green wedge faces; configured plitka exercises the real BC1 upload and a
nonzero texture-table index. The separate converter accepts the pinned OGF v4
static profile and feeds that same writer for offline validation. The converted
plitka is now the first original X-Ray model carried through xrPhoton's configured
runtime path, with final visual sign-off pending. The Blender opaque export probe
is the next primary content-source entry; `bochka_fuel` follows later
once its hierarchy, bone, and IK/physics contracts exist. Dynamic scenes
(TLAS refits, skinning), actual path tracing with lights, and temporal accumulation
and denoising follow later. Details in [ARCHITECTURE.md](ARCHITECTURE.md).

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

It also builds the narrow legacy OGF converter:

```sh
./build/ogfx-core/xrPhotonAssetCompiler convert-ogf input.ogf output.ogfx
```

This first adapter intentionally accepts only the documented M4a static profile.
Its output preserves a logical texture name that the runtime reconstructs for
scene-global DDS resolution and can be supplied directly to the optional gallery
configuration below.

With the local `plitka1.ogf` source file in
`build/ogfx-core/legacy-ogf-corpus/meshes/objects/dynamics/plitka/`, run the complete,
opt-in offline proof with:

```sh
cmake --build --preset ogfx-core --target xrPhotonM4aOfflineProof
```

It verifies the pinned source and output identities, conversion semantics,
canonical schema round trip, and exact runtime texture-reference reconstruction.
The persistent result is written to
`build/ogfx-core/corpus/meshes/objects/dynamics/plitka/plitka1.ogfx`. A different
local source location can be selected at configure time with
`-DXRPHOTON_M4A_CORPUS_OGF=/path/to/plitka1.ogf`; the corpus remains outside the
repository and normal builds do not depend on it.

### Configuring the legacy gallery entry

The generated quad and wedge need no local game files. To add the verified plitka
output and its original texture to the debug gallery, configure both owner-local
paths once, then build normally:

```sh
cmake --preset debug \
  -DXRPHOTON_GALLERY_PLITKA_OGFX="$PWD/build/ogfx-core/corpus/meshes/objects/dynamics/plitka/plitka1.ogfx" \
  -DXRPHOTON_GALLERY_TEXTURE_ROOT="$PWD/build/ogfx-core/original_game_files/soc/textures"
cmake --build --preset debug
./build/debug/xrPhoton
```

The texture root must preserve the exact-case relative path
`ston/ston_stena_marbl_m_03_back.dds`. CMake remembers both values separately in
each build tree, so configure the `release` preset the same way when needed. With
an empty `XRPHOTON_GALLERY_PLITKA_OGFX`, xrPhoton prints a skip line and renders
only the generated entries. Once plitka is configured, a missing/broken OGFx,
texture root, or DDS is a loud startup failure rather than a silent fallback. The
original game files and generated proof output remain Git-ignored local inputs.

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
