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

Right now it renders an indexed quad with a UV gradient that you can fly around
(WASD + mouse look).
That said, the whole ray tracing stack is already behind it: every frame traces
a ray per pixel through a real BLAS/TLAS with `vkCmdTraceRaysKHR` from a
perspective camera fed to the shader via push constants, writes a storage image
and blits it to the swapchain, with two frames in flight and proper resize
handling. Shaders are written in [Slang](https://shader-slang.org/) and compiled
into the runtime binary at build time, so shader deployment is self-contained
and needs no runtime shader files.

The first complete OGFx round trip has landed. Every normal engine build generates
`build/<preset>/assets/test_quad.ogfx` through the canonical writer; the runtime
strictly decodes that file into `SceneData` and renders the file-backed quad through
the existing GPU/BLAS/TLAS path. Next up is direct legacy-static OGF conversion,
followed by real geometry and materials, dynamic scenes (TLAS refits, skinning),
actual path tracing with lights, and finally temporal accumulation and denoising.
Details in [ARCHITECTURE.md](ARCHITECTURE.md).

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
