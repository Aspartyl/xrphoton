# xrPhoton

X-Ray Photon Engine. A rebuild of the X-Ray engine from the S.T.A.L.K.E.R.
series in modern C++ on Vulkan, with a hardware ray-tracing renderer being
built specifically for path tracing and no raster fallback.
The plan is to eventually build a standalone game on it, similar to the old
STALKER games but with overhauled systems.

## One renderer

The engine does each thing one way, chosen deliberately. The original X-Ray
maintained several renderers in parallel (static lighting, dynamic lighting,
one per DirectX generation), while here there is exactly one rendering path.
No lightmaps, no shadow maps, no per-renderer material variants. Everything
from the sun to a flashlight goes through the same light transport, which suits
STALKER's dynamic weather and day/night cycle anyway. The trade-off is steep
hardware requirements: a GPU with hardware ray tracing support is mandatory and
there is no fallback for anything less. The rest of the engine follows the same
idea as it grows.

## Status

Right now it renders a compact test yard you can explore with a collision-aware
capsule character. WASD walks, Left Shift sprints, Space jumps, Left Ctrl
crouches, and F1 toggles a collision-free fly camera that suspends the player
and starts from their current position and view. Escape releases the captured
mouse and left click recaptures it. Every build assembles the yard from
generated ground, wall, and box models into walls, a platform, a staircase and
crates, alongside an indexed quad and a two-geometry wedge kept as regression
probes.

Each frame traces a ray per pixel through one BLAS per mesh and a real
multi-instance TLAS, from a perspective camera fed to the shader through push
constants. Raygen follows paths of up to eight surface vertices, evaluating an
energy-aware Lambert diffuse plus isotropic GGX dielectric, conductor, or rough-glass BSDF under a
directional sun, tracing hard visibility rays for shadows, and sampling
matching lobes for indirect transport. GGX uses visible-normal sampling and
Russian roulette starts after the third vertex, so bounces gather the
procedural sky or sunlit surfaces, fill occluded regions, bleed color and carry
rough or sharp reflections. The result is written as linear radiance to an
`R16G16B16A16_SFLOAT` image, compute-tonemapped with fixed-exposure Reinhard,
and blitted to the swapchain, with two frames in flight and proper resize
handling. A PCG hash seeded by pixel and frame index keeps the one-sample noise
repeatable in capture mode.

Opaque and any-hit-capable geometry get separate hit records for both radiance and
shadow rays. Alpha-tested and Glass BLAS ranges remain non-opaque; the alpha-tested
any-hit variants compare sampled texture alpha against the material cutoff, so
shadow and bounce rays pass through the same cutouts as visible rays. Glass any-hit
accumulates tinted `(1 - Fresnel)` attenuation for direct-light visibility, while
radiance paths use matched rough GGX reflection/refraction sampling and PDFs. Shaders
are written in [Slang](https://shader-slang.org/) and compiled into the binary
at build time, so shader deployment needs no runtime files.

Assets travel through the project's own OGFx container. The offline compiler
writes it, the runtime strictly decodes it, and two source paths converge on
the same writer: a narrow converter for pinned legacy OGF profiles, and a
headless Blender exporter. Textures resolve logical OGFx names to strict DDS
BC1/BC3 or uncompressed RGBA8 images, deduplicated into a fixed sampled-image
array with an opaque-white fallback. The generated probes and the yard need no
external files; converted legacy and Blender models are optional local extras
described below.

Physics is Jolt, driven by a 60 Hz fixed-step accumulator that clamps each
frame's contribution to 0.1 seconds. One generated crate spawns above the yard,
falls, tumbles, settles and sleeps; the configured barrel and pseudodog tail
become dynamic bodies too, and every other instance is static collision
geometry. Physics publishes body transforms back to `SceneData`, and the
renderer rewrites one fence-protected instance-input slot and rebuilds the
shared TLAS in place before every trace while all BLAS geometry stays static.
Plain, GPU-assisted and synchronization validation are clean over live motion,
resize and teardown.

Deformable skinning and BLAS refits, a time-varying
sun/sky, and temporal accumulation and denoising follow later.
[ARCHITECTURE.md](ARCHITECTURE.md) has the module map and the roadmap.

## Building

The project exposes one canonical optimized engine build with debug symbols:

```sh
cmake --preset default
cmake --build --preset default
./build/xrPhoton
```

Vulkan validation is available from that same executable when diagnosing renderer
work: `./build/xrPhoton --validation`. It is best-effort when the Khronos layer is
not installed.

Requirements:

- C++23 and CMake 3.24+
- Vulkan SDK (1.3)
- GLFW 3
- GLM (`libglm-dev` on Debian/Ubuntu)
- `slangc` (use an official release; distro packages are usually too old)
- A GPU/driver with `VK_KHR_acceleration_structure` and
  `VK_KHR_ray_tracing_pipeline`

Two dependencies are vendored rather than installed, so configuration never
downloads anything. Jolt Physics v5.6.0 lives under
[`third_party/jolt`](third_party/jolt) and is built as a static, engine-only
library, with its one local thread-pool exception-safety fix recorded in
[`third_party/jolt/XRPHOTON_PATCHES.md`](third_party/jolt/XRPHOTON_PATCHES.md).
The Vulkan Memory Allocator v3.3.0, which backs every GPU allocation, is the
single header [`third_party/vma`](third_party/vma) compiled in one isolated
translation unit. Both are MIT-licensed and keep their verbatim licenses beside
the sources.

Linux with GCC or Clang is the current development environment. Windows support
is planned but its build and platform integration have not landed yet.

### Deterministic capture

To check the image without watching the window, render a number of frames and
write the final tonemapped result as an sRGB PPM:

```sh
./build/xrPhoton --capture 8 capture.ppm
./build/xrPhoton --capture 8 night.ppm --scene night
./build/xrPhoton --validation --capture 8 checked.ppm
```

Capture mode fixes the camera and extent, advances physics by exactly 1/60
second per successful frame, and reads back the last submitted image. It prints
the extent, the final frame index and a hash of the raw bytes, and two runs of
the same binary on the same machine and driver are expected to match. A resize,
window close, or out-of-date swapchain fails the capture instead of silently
changing the sampled frame.

P2c also provides an acceptance-only linear-HDR reference mode. It freezes the scene,
reads every untouched HDR sample back to the CPU, and exposes MIS, NEE-only, and
BSDF-only controls for estimator checks:

```sh
./build/xrPhoton --reference 256 --scene night --estimator mis
cmake --build --preset default --target xrPhotonReferenceProof
```

The proof target runs all three estimators and requires their means to agree in each
pinned image region; reference mode is offline measurement, not render accumulation.

### Offline tools

The OGFx writer, decoder, scene-lighting policy and their tests need only the
C++ toolchain and header-only GLM, with no Vulkan, GLFW, Slang, Jolt or GPU:

```sh
cmake --preset ogfx-core
cmake --build --preset ogfx-core
ctest --preset ogfx-core
```

That configuration also builds the asset compiler, which converts legacy models
and receives the Blender exporter's stream:

```sh
./build/ogfx-core/xrPhotonAssetCompiler convert-ogf input.ogf output.ogfx
```

The converter deliberately handles two documented profiles, the M4a flat-static
slice and the narrow SoC rigid-compound slice, and is not general skeletal
support. The Blender side runs headless through
[`tools/blender/export_ogfx.py`](tools/blender/export_ogfx.py) with Blender
5.1.x, extracting one explicitly named static mesh and sending a private `XRBM`
stream to `convert-blender` rather than writing OGFx itself:

```sh
/path/to/blender --background --factory-startup --disable-autoexec \
  --python-exit-code 1 blender/test_pyramid.blend \
  --python tools/blender/export_ogfx.py -- \
  --compiler "$PWD/build/ogfx-core/xrPhotonAssetCompiler" \
  --output "$PWD/build/ogfx-core/assets/blender/test_pyramid.ogfx" \
  --object test_pyramid
```

Accepted inputs are narrow on purpose: no modifiers, animation, shape keys,
constraints or parenting, and at most one simple material whose texture is a
lowercase `.dds` under the supplied texture root, or one explicit untextured
Principled Metal material. Anything more elaborate fails loudly.
[FORMATS.md](FORMATS.md) documents the full contract, and each
converted asset has an opt-in CMake proof target there that runs the real tools
twice and pins the output.

### Optional yard entries

The generated probes need no local files. Converted legacy and Blender models
are added to the yard by configuring their paths once, then building normally:

```sh
cmake --preset default \
  -DXRPHOTON_GALLERY_PLITKA_OGFX=".../plitka1.ogfx" \
  -DXRPHOTON_GALLERY_BLENDER_OGFX=".../test_pyramid.ogfx" \
  -DXRPHOTON_GALLERY_BLENDER_SPHERE_OGFX=".../test_sphere.ogfx" \
  -DXRPHOTON_GALLERY_BLENDER_SMOOTH_SPHERE_OGFX=".../test_smooth_sphere.ogfx" \
  -DXRPHOTON_GALLERY_BLENDER_SHINY_SPHERE_OGFX=".../yard_shiny_sphere.ogfx" \
  -DXRPHOTON_GALLERY_BLENDER_LEAF_CARD_OGFX=".../test_leaf_card.ogfx" \
  -DXRPHOTON_GALLERY_BARREL_OGFX=".../bochka_close_1.ogfx" \
  -DXRPHOTON_GALLERY_REMADE_BARREL_OGFX=".../remade_bochka_close_1.ogfx" \
  -DXRPHOTON_GALLERY_CUSTOM_BARREL_OGFX=".../custom_stalker_barrel.ogfx" \
  -DXRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX=".../item_psevdodog_tail.ogfx" \
  -DXRPHOTON_GALLERY_TEXTURE_ROOT="$PWD/original_game_files/soc/textures"
cmake --build --preset default
```

An empty variable skips that entry, and a configured one that fails to load is
a loud startup error rather than a silent fallback. The texture root must keep
its exact-case relative paths, and the yard checks owner-local
`blender/textures` before the legacy root so an authored texture deterministically
shadows a same-named legacy one. CMake remembers these values in the one canonical
build tree. Original game files, Blender
sources and generated proof outputs all stay Git-ignored.

## Docs

[ARCHITECTURE.md](ARCHITECTURE.md) covers the module map, the ownership and
lifetime model, the per-frame flow, synchronization and the roadmap.
[FORMATS.md](FORMATS.md) covers the asset-format plan for OGFx, OMFx and the
shared offline compiler, and [SDK.md](SDK.md) the plan for the modern SDK
successor.
