# xrPhoton Architecture

This document describes how xrPhoton is put together: its modules, the lifetimes
and ownership of its resources, the per-frame flow, and the synchronization model.

## Status

xrPhoton renders an interactive ray-traced OGFx test yard. It
brings up Vulkan hardware ray tracing, a swapchain, one BLAS per model mesh and a
TLAS over every yard placement, then fires one ray per pixel from either a basic
collision-aware player view or the original perspective fly camera. The ray-tracing
shader samples scene materials and
writes a device-local storage image that is blitted to the swapchain. The present
path has two frames in flight, resize handling, and the required descriptor rewrite;
the frame path lives in `renderer.{hpp,cpp}`, while `main.cpp` remains orchestration.
The repository-owned crate is now a live Jolt body: it spawns above the yard,
falls, tumbles, settles, and sleeps. `PhysicsWorld` writes its body-origin
transform into `SceneData`; the renderer remains physics-agnostic, writing one
mapped instance-input slot and fully rebuilding the existing TLAS in place before
every trace.

Generated-only builds load `test_yard_ground.ogfx`, `test_yard_wall.ogfx`, and
`test_yard_box.ogfx` beside the permanent quad and two-geometry wedge probes. The
generated-only scene has **5 models / 13 placements / 5 BLASes / 6 geometries**:
a 20-by-20-metre ground, an L-shaped wall corner, a stepped platform, one static
crate, the dynamic crate, a face-on quad, and two transformed wedges sharing one
BLAS. A fully configured build adds the nine legacy/Blender exhibits below for a
total of **14 models / 22 placements / 14 BLASes / 16 geometries**. Every entry
uses the same OGFx decoder, `SceneData`, GPU upload,
acceleration-structure, material/texture, and shader path.

The converted legacy `plitka1.ogfx` resolves its
`ston\ston_stena_marbl_m_03_back` DDS beneath an owner-supplied texture root and
exercises the real BC1/nonzero-descriptor path. The converted
`test_pyramid.ogfx`, flat-shaded `test_sphere.ogfx`,
`test_smooth_sphere.ogfx`, and alpha-tested `test_leaf_card.ogfx` are independent
optional yard exhibits. The converted regular `bochka_close_1.ogfx` adds its
existing `mtl\mtl_barrel_01.dds`; the adjacent scale-faithful Blender remake adds
one opaque geometry using the owner-local
`xrphoton\remade_bochka_close_1_basecolor.dds` and intentionally no physics
metadata. The third adjacent drum, `custom_stalker_barrel.ogfx`, is an original
192-segment dented design with modeled ribs, bungs, weld seam, riveted plate and
warning bars, using Poly Haven's untouched 4K CC0 `rusty_metal_04` diffuse map
in one owner-local uncompressed RGBA8 DDS and no physics metadata.
The mixed `item_psevdodog_tail.ogfx` follows that three-barrel comparison: its single
mesh contains one `models\model_aref` alpha-tested geometry and one
`models\model` opaque geometry, sharing `act\act_pseudodog_fur` and one material
whose cutoff is exactly 128/255. The wedge remains the shared-BLAS probe. The sphere
pair has identical indexed
position/UV corner streams but deliberately different normals: flat-face splits
versus shared smooth normals that remain split only at the UV seam.
The three SoC yard exhibits retain authored scale: plitka keeps its static
rotated/translated placement, the barrel's rigid spawn is raised to y = 0.6 and
rolled 20 degrees about +Z, and the tail keeps its platform pose.

The landed texture foundation validates strict DDS DXT1/DXT5 and canonical
uncompressed RGBA8 input, uploads mip-0 BC1/BC3/RGBA8 payloads directly, always
supplies an opaque-white fallback at image zero, and exposes one fixed sampled-image descriptor array. The generated probes select the
fallback; configured plitka selects a real nonzero image index. The tail adds
per-geometry opaque/alpha-tested SBT selection, conditional BLAS opacity flags,
and texture-alpha any-hit evaluation. Its shipped DDS has no transparent texels
in mip 0, so it proves mixed routing plus actual texture sampling/any-hit
execution. The Blender leaf card supplies the complementary visible acceptance:
its pinned `trees\trees_new_vetka_green` DXT1 mip contains 153,894 transparent
texels, and rejected hits reveal the miss background. M1 through M4a and the yard
work that instantiated their consumers are recorded in the roadmap below.

The regular barrel's offline source adapter preserves one compound-body
recipe—three named cylinders, their masses and centers of mass, and the source
physics material—in optional OGFx records. Both byte decoders validate those
records; `ogfx_loader` now copies their runtime fields into `SceneData`, scene
assembly rebases and validates them, and a configured barrel placement becomes a
live compound body. The adjacent remade and custom barrels have no recipes and
remain static. The tail follows the same path: its preserved oriented-box recipe
becomes a live body when that optional placement is configured and settles onto
the platform. The required generated crate carries its own one-box recipe, so a
generated-only build still exercises recipe-to-body construction. The opt-in
`xrPhotonAlphaOgfOfflineProof` target takes its OGF
from `XRPHOTON_ALPHA_TAIL_CORPUS_OGF`, pins the companion DDS selected by
`XRPHOTON_ALPHA_TAIL_TEXTURE_DDS`, and writes
`build/<preset>/assets/soc/meshes/equipments/item_psevdodog_tail.ogfx`; the
runtime yard entry is selected independently with
`XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX`.

The engine-side backend is the vendored MIT-licensed Jolt Physics **v5.6.0**,
built as a static library only when `XRPHOTON_BUILD_ENGINE` is enabled. Every
non-dynamic yard instance becomes a static triangle-mesh body; the generated
crate and the configured barrel/tail placements become dynamic primitive or
compound bodies with linear-cast motion quality. The generated-only world is
therefore 1 dynamic and 12 static bodies; the fully configured world is 3 dynamic
and 19 static. `PhysicsWorld` advances Jolt
through a 60 Hz fixed-step accumulator, clamps a frame's contribution to 0.1 s,
and atomically publishes validated transforms to the bound scene. No
interpolation is applied; high-refresh presentation therefore repeats the most
recent simulated pose between fixed steps. The Vulkan-free headless suite pins
body construction, settling/sleep, lifecycle, contracts, determinism, update
failure, static-mesh rules, math bridging, and CCD.

The first direct modern-content adapter is also landed as a narrow headless
Blender 5.1.x path. [`tools/blender/export_ogfx.py`](tools/blender/export_ogfx.py)
validates and extracts one explicitly named static mesh, then sends a private
versioned `XRBM` stream over stdin to `xrPhotonAssetCompiler
convert-blender`. [`src/blender_mesh.cpp`](src/blender_mesh.cpp) bakes scene units
and the object transform, maps Blender `(x, y, z)` to engine `(x, z, y)`, applies
inverse-transpose normal transformation, and reverses winding according to the
combined object/axis-transform determinant before populating the ordinary compiler
model. Only the shared canonical writer serializes OGFx. `test_pyramid`, the
flat-shaded `test_sphere`, and `test_smooth_sphere` use the byte-compatible
material-free XRBM v1 profile. XRBM v2 adds exactly one strict opaque or
alpha-tested DDS material; `test_leaf_card` exercises the alpha-tested branch and
the remade barrel exercises opaque texturing. It carries the logical texture
reference and classification/cutoff and normalizes textured V once at the adapter boundary.
The sphere pair exercises dense triangulation, UV seams, corner splitting, and
flat-versus-smooth normal preservation; the leaf card visibly proves any-hit
rejection through the same runtime path as legacy content.

## Goals and constraints

- **Single runtime executable.** No runtime engine/game split and no plugin system;
  one process opens a window and draws. Offline asset compiler, converter, and SDK
  tools are separate build products and never become alternate runtime paths.
- **Hardware ray tracing as a hard requirement.** Device selection rejects any GPU
  that does not expose `VK_KHR_acceleration_structure` and
  `VK_KHR_ray_tracing_pipeline` (and their prerequisites). There is no raster
  fallback path.
- **Vulkan 1.3 baseline.** `RequiredApiVersion = VK_API_VERSION_1_3`; both the
  instance and the selected physical device must meet it.
- **RAII over manual cleanup.** Resource teardown lives in destructors, never in
  hand-unwound failure paths. See [Ownership model](#ownership-model).
- **Explicit error boundaries.** Errors reach subsystem boundaries as explicit
  result objects, `VkResult`, or `bool`; standard-library exceptions are caught
  internally and do not cross subsystem APIs. Failures are reported to `std::cerr`;
  Vulkan failures include the symbolic result name and numeric value, with a
  numeric fallback for results unknown to the current formatter.
- **C++23, no compiler extensions** (`CMAKE_CXX_EXTENSIONS OFF`).
- **Linux development now, Windows target later.** Linux is the current build,
  development, and validation environment; it is not a permanent platform
  restriction. Portable subsystems must avoid unnecessary Linux assumptions so
  Windows support can be added and validated when the project reaches that work.

## Module map

The engine executable and its supporting runtime/offline libraries are split along
**resource lifetime**
(program-lifetime vs. recreated-on-resize), **orchestration vs. mechanism**, and
the offline-compiler/runtime boundary.

```
        ┌───────────────────────────────────────────────────────────────────┐
        │ main.cpp                                                          │
        │   orchestration + the render loop                                 │
        └───────────────────────────────┬───────────────────────────────────┘
                                        │ calls drawFrame / prepareRtForSwapchain
        ┌───────────────────────────────▼───────────────────────────────────┐
        │ renderer.{hpp,cpp}                                                │
        │   Renderer (non-owning view) + the frame path                     │
        │   drawFrame / recordTraceCommandBuffer / prepareRtForSwapchain    │
        └──────┬──────────────┬──────────────────┬──────────────────┬───────┘
               │ uses         │ uses             │ uses             │ uses
┌──────────────▼───────┐ ┌────▼───────────────┐ ┌▼────────────────┐ ┌▼──────────────┐
│ vulkan_context.{hpp, │ │ swapchain.{hpp,cpp}│ │ acceleration_   │ │ rt_pipeline.  │
│ cpp}                 │ │   Swapchain owner  │ │ structure.      │ │ {hpp,cpp}     │
│   VulkanContext owner│ │   create/recreate/ │ │ {hpp,cpp}       │ │   RtPipeline  │
│ instance/device/VMA  │ │   support query    │ │   BLAS/TLAS     │ │   owner; SBT; │
│   queues, RT fns     │ │                    │ │   build/rebuild │ │   descriptors │
└──────────────────────┘ └────────────────────┘ └─────────────────┘ └───────────────┘
```

(`main.cpp` also uses the resource units directly for bring-up, drives
`camera.{hpp,cpp}`, and creates/steps the renderer-independent
`physics.{hpp,cpp}` world before entering the frame path. The diagram focuses on
the renderer layering.)

| Unit | Owns / provides | Lifetime |
|------|-----------------|----------|
| [src/vulkan_context.hpp](src/vulkan_context.hpp) / [.cpp](src/vulkan_context.cpp) | `VulkanContext` (instance, debug messenger, surface, device, VMA allocator, command pool, per-frame command buffers and sync), `FrameResources`, `QueueFamilyIndices`, `RayTracingFunctions`, and the bring-up helpers (including `createAllocator`, shared VMA-backed `createBuffer`, and `uploadDeviceLocalBuffer`) | Program lifetime — created once |
| [src/third_party_impl.cpp](src/third_party_impl.cpp) / [src/vma_fwd.hpp](src/vma_fwd.hpp) | The one `VMA_IMPLEMENTATION` translation unit and the lightweight VMA handle declarations project headers use | Program lifetime infrastructure |
| [src/swapchain.hpp](src/swapchain.hpp) / [.cpp](src/swapchain.cpp) | `Swapchain` (swapchain, images, image views, per-image render-finished semaphores, and the VMA-backed storage output image + its view) and its create/recreate/query lifecycle | Recreated on resize |
| [src/scene.hpp](src/scene.hpp) | Vulkan-free `SceneData` and its CPU render records plus backend-neutral `ScenePhysicsBody` / `ScenePhysicsCollider` recipes | Plain value state loaded from OGFx and owned by `main()`; instance transforms are mutable physics output |
| [src/ogfx.hpp](src/ogfx.hpp), [src/ogfx_detail.hpp](src/ogfx_detail.hpp), [src/ogfx.cpp](src/ogfx.cpp), and [src/ogfx_decoder.cpp](src/ogfx_decoder.cpp) | Standard-library-only render/physics model and schema constants, private shared format invariants and diagnostics, checked compiler validation/bounds generation, the canonical explicit-little-endian writer, and the transactional strict OGFx schema/runtime byte decoder | Shared offline/runtime core; backend-neutral physics records contain no live backend state |
| [src/legacy_ogf.hpp](src/legacy_ogf.hpp), [src/legacy_ogf.cpp](src/legacy_ogf.cpp), and [src/legacy_ogf_rigid.cpp](src/legacy_ogf_rigid.cpp) | Transactional source dispatch for the pinned M4a flat-static and narrow SoC rigid-compound OGF v4 profiles; the rigid branch flattens validated bind-pose render data, maps `models\model`/`models\model_aref` to opaque/alpha-tested geometry, and retains box-or-cylinder body metadata without owning OGFx serialization | Offline-only source adapters in the graphics-free build |
| [src/blender_mesh.hpp](src/blender_mesh.hpp) / [.cpp](src/blender_mesh.cpp) | Transactional decoder for the private versioned `XRBM` extraction stream; validates the narrow static profile, bakes units/transforms, converts axes, normals, and winding, deduplicates corners, and populates the compiler model without owning OGFx serialization | Offline-only source adapter in the graphics-free build; no Blender or renderer dependency |
| [tools/blender/export_ogfx.py](tools/blender/export_ogfx.py) | Blender 5.1.x source validation and evaluated triangle/corner extraction for one explicitly named static mesh; emits material-free XRBM v1 or the strict one-opaque-or-alpha-tested-DDS-material v2 profile, invokes `convert-blender`, and supplies the exchange on stdin | Headless Blender-side front end; never writes OGFx and is not a runtime dependency |
| [tools/asset_compiler.cpp](tools/asset_compiler.cpp) | `xrPhotonAssetCompiler convert-ogf` / `convert-blender` dispatch, bounded source input, canonical-writer invocation, and exclusive adjacent-temp publication | Offline CLI; no runtime or renderer dependency |
| [src/ogfx_loader.hpp](src/ogfx_loader.hpp) / [.cpp](src/ogfx_loader.cpp) | Checked filesystem input and field-by-field conversion of decoded OGFx render and rigid-recipe data into owned `SceneData`; quaternion component order is crossed explicitly, while source-node names and reserved flags remain format provenance and are dropped | Vulkan-free runtime adapter used by scene producers such as the yard policy; returns no instances or images |
| [src/scene_assembly.hpp](src/scene_assembly.hpp) / [.cpp](src/scene_assembly.cpp) and [src/scene_assembly_detail.hpp](src/scene_assembly_detail.hpp) | Transactional model concatenation and offset rebasing (including body mesh and collider ranges), bounded instance insertion, and complete whole-scene render/physics validation; the detail header exposes only the pure count-check seam | Vulkan-free runtime mechanism; mutates caller-owned `SceneData` and owns no long-lived state |
| [src/texture_loader.hpp](src/texture_loader.hpp) / [.cpp](src/texture_loader.cpp) and [src/texture_loader_detail.hpp](src/texture_loader_detail.hpp) | Canonical logical-name mapping, strict DDS DXT1/DXT5/canonical-RGBA8 framing and mip-0 decode, ordered texture-root overlays, deterministic scene-image deduplication, slot-0 fallback creation, and cumulative texture-byte gating | Vulkan-free runtime mechanism; resolves caller-owned `SceneData` after model assembly |
| [src/ray_types.hpp](src/ray_types.hpp) | Build-owned `RayTypeCount` constant and compile-time C++ side of the C++/Slang SBT-routing ABI | Shared by scene assembly, acceleration-structure construction, and pipeline/SBT construction; currently fixed at one radiance ray type |
| [src/gallery.hpp](src/gallery.hpp) / [.cpp](src/gallery.cpp) | File-private yard asset/placement tables and `loadGalleryScene`, which loads each required or configured OGFx model once, merges it, instantiates every mesh in each placement, resolves fallback/DDS images from Blender-authored then legacy owner-local roots, and returns `GalleryLoadResult` with validated `SceneData`, the accepted spawn, and flat `dynamicInstances` in placement order | Temporary engine-side scene policy called by `main()`; requires the generated crate dynamic, includes configured recipe-bearing barrel/tail placements, and retires when level/scene data has a real owner |
| [src/physics.hpp](src/physics.hpp) / [.cpp](src/physics.cpp) | Jolt-free public `PhysicsWorld` owner and create/step/control/query seam; the implementation alone owns Jolt initialization, shapes/bodies, the invisible `CharacterVirtual`, layer filters, job/temp systems, fixed-step accumulator, topology guards, and GLM ↔ Jolt conversion | Program lifetime after scene load; borrows one stable `SceneData`, writes only dynamic instance transforms, and tears down before that scene |
| [third_party/jolt](third_party/jolt) | Trimmed Jolt Physics v5.6.0 library source, CMake support, MIT license, and one documented thread-pool exception-safety patch | Vendored static engine dependency; never configured by the graphics-free `ogfx-core` build |
| [tools/compile_probe_assets.cpp](tools/compile_probe_assets.cpp) | Offline quad, multi-geometry wedge, and ground/wall/box test-yard front end plus command-line file output; the generated box includes the canonical one-box rigid recipe, while all validation and encoding remain in `xrPhotonOgfx` | Build-time tool — generates the five uncommitted `assets/probes/test_*.ogfx` files in each binary directory |
| [src/gpu_scene.hpp](src/gpu_scene.hpp) / [.cpp](src/gpu_scene.cpp) | `GpuScene` owner, the `GeometryRecord` / `MaterialRecord` shader ABIs, staged upload of unified geometry/record buffers and sampled scene images, shared texture sampler, and storage/descriptor/format gates | Program lifetime — created once at startup |
| [src/acceleration_structure.hpp](src/acceleration_structure.hpp) / [.cpp](src/acceleration_structure.cpp) | `AccelerationStructure` (one mapped TLAS-instance input per frame slot, stable-fields instance template, vector of BLAS handles/backings, TLAS, transient BLAS scratch, and persistent TLAS scratch); startup construction plus checked `writeTlasInstances` and `recordTlasRebuild`, including per-range opacity flags and per-instance first-geometry SBT offsets | Program lifetime — BLASes built once; TLAS rebuilt in place per frame |
| [src/camera.hpp](src/camera.hpp) / [.cpp](src/camera.cpp) | GLM-backed player/free `Camera` view states, `CameraControls` edge state, `CameraPushConstants` (the raygen push payload + its ABI asserts), `updateCamera` (all GLFW input policy), and `makeCameraPushConstants` | Plain value state owned by `main()` — no Vulkan objects |
| [src/player.hpp](src/player.hpp) / [.cpp](src/player.cpp) | Vulkan/Jolt/GLFW-free player constants and pure yaw-relative run/sprint/crouch velocity calculation | Shared by camera input and headless player-control tests |
| [src/rt_pipeline.hpp](src/rt_pipeline.hpp) / [.cpp](src/rt_pipeline.cpp) | `RtPipeline` (descriptor set layout/pool/set, pipeline layout with the camera push-constant range, four-stage/four-group ray tracing pipeline, per-geometry SBT buffer + the four trace regions), `createRtDescriptorSet`, `createRtPipeline`, `buildShaderBindingTable`, `writeRtDescriptorSet`, `writeSceneDescriptorSet` | Program lifetime — created once at startup; bindings 0–1 are *rewritten* on resize |
| [src/renderer.hpp](src/renderer.hpp) / [.cpp](src/renderer.cpp) | `Renderer` (the non-owning view of everything the frame path uses, including CPU scene and acceleration-structure owner), `drawFrame` with its post-fence per-slot instance write, `prepareRtForSwapchain`, and the file-private `recordTraceCommandBuffer` / `recordImageBarrier` / `recordExecutionBarrier` | Owns nothing — a parameter bundle over borrowed handles |
| [src/main.cpp](src/main.cpp) | `main()` orchestration, player/free-camera switching, physics stepping, and the render loop | Program lifetime |

### Header dependency rule

Includes are kept acyclic by a deliberate rule:

- `swapchain.hpp` only **forward-declares** `QueueFamilyIndices`.
- `acceleration_structure.hpp` and `rt_pipeline.hpp` only **forward-declare**
  the scene/RT types they borrow; `gpu_scene.hpp` forward-declares `SceneData`.
- `renderer.hpp` only **forward-declares** `AccelerationStructure`,
  `CameraPushConstants`, `FrameResources`, `RayTracingFunctions`, `RtPipeline`,
  `SceneData`, and `Swapchain`; it
  never mentions `VulkanContext` — the renderer borrows specific handles, not the
  context, so the unit is decoupled from bring-up entirely.
- `vulkan_context.hpp` never mentions `Swapchain`, `AccelerationStructure`,
  `RtPipeline`, or `Renderer`.
- Project headers include only `vma_fwd.hpp`; the full vendored
  `vk_mem_alloc.h` is confined to implementation files that call VMA.
- `camera.hpp` includes **no project or Vulkan header at all** (only GLM's
  Vulkan-free vector header and `<cstddef>` for its `offsetof` ABI asserts) and
  forward-declares `GLFWwindow`;
  `makeCameraPushConstants` takes a plain `float aspect` rather than a
  `VkExtent2D` precisely to keep the unit Vulkan-free.
- `scene.hpp` is likewise Vulkan-free: CPU scene data depends only on the standard
  library and GLM, while `gpu_scene.hpp` owns the Vulkan/VMA boundary.
- `physics.hpp` includes only the standard library and forward-declares
  `SceneData`; the opaque `PhysicsWorld::State` keeps every Jolt type out of the
  public header. `physics.cpp` is the only engine translation unit that includes
  Jolt headers, so the `JPH_*` compile-definition contract propagated by the
  static `Jolt` target has one consumer surface.
- `ogfx.hpp` is a stricter offline boundary: it depends only on the standard
  library and shares no renderer- or physics-backend-native structs. Source
  adapters populate its compiler model, including optional backend-neutral
  body/collider values, and only the canonical writer owns the serialized schema.
  Its offline schema decoder supports compiler round trips, including logical
  texture references; the separate runtime entry point applies the same
  structural and render-semantic validation before adaptation.
- `legacy_ogf.hpp` depends only on that compiler-facing OGFx model and the
  standard library. Its implementation may share private core invariants such
  as canonical-size preflight, but it cannot serialize OGFx or reach renderer
  state.

The genuine cross-links are resolved in the `.cpp`s, not the headers:

1. `queryPhysicalDeviceSuitability` (in `vulkan_context.cpp`) calls
   `hasRequiredSwapchainSupport` (declared in `swapchain.hpp`) and
   `hasRequiredAccelerationStructureFormatSupport` (declared in
   `acceleration_structure.hpp`), plus the scene-texture descriptor-limit and
   format-support helpers declared in `gpu_scene.hpp`.
2. The swapchain functions need the full definition of `QueueFamilyIndices`, which
   they get by including `vulkan_context.hpp` in `swapchain.cpp`.
3. `gpu_scene.cpp` includes `scene.hpp` and `vulkan_context.hpp` to turn CPU arrays
   into device-local buffers; `buildAccelerationStructures` includes both scene
   definitions and borrows their ranges/device addresses.
4. `rt_pipeline.cpp` likewise includes `vulkan_context.hpp` for the full
   `RayTracingFunctions` and `createBuffer`; it additionally includes the
   build-generated `raytrace_spv.h` (the embedded shader module — see
   [Ray tracing pipeline](#ray-tracing-pipeline)).
5. `renderer.cpp` includes `acceleration_structure.hpp`, `camera.hpp`,
   `rt_pipeline.hpp`, `swapchain.hpp`, and `vulkan_context.hpp` to resolve the
   borrowed structs its header only forward-declares and to invoke the per-frame
   TLAS write/rebuild seam.
6. `ogfx_loader.cpp` includes `scene.hpp` through its public header and adapts the
   standard-library-only decoded model into runtime `SceneData`, including the
   backend-neutral body/collider recipe fields.
7. `scene_assembly.cpp` depends only on `scene.hpp` and standard-library helpers;
   it remains in the same Vulkan-free runtime library as the OGFx adapter.
8. `gallery.cpp` includes the loader, assembly, and texture-resolution APIs to own
   temporary startup policy; its public header exposes `SceneData`, the spawn value,
   and the flat dynamic-instance indices selected by placement policy.
9. `physics.cpp` includes `scene.hpp` and all Jolt headers needed to validate and
   instantiate that data plus the virtual player character. No renderer or Vulkan
   type crosses this boundary.
10. `rt_pipeline.cpp` includes `camera.hpp` for `sizeof(CameraPushConstants)` —
   the pipeline layout's push-constant range; `camera.cpp` includes
   `GLFW/glfw3.h` for the real input API its header only forward-declared and
   `player.hpp` for the pure yaw-relative velocity calculation.

File-local helpers live in an anonymous namespace inside each `.cpp`; only the
cross-file surface is declared in the headers.

## Ownership model

Six RAII owners — split by resource lifetime:

- **`VulkanContext`** (program lifetime, created once) owns: the GLFW init flag, the
  window, the instance, the debug messenger, the surface, the device, the one
  `VmaAllocator`, the command pool, and the `frames` array. Each `FrameResources`
  slot owns one command buffer, one image-available semaphore, and one in-flight fence.
- **`Swapchain`** (recreated on resize) owns: the swapchain, its images, its image
  views, the format/extent, the **per-image** render-finished semaphores, and the
  **storage image** (the trace output target — its `VkImage`, `VmaAllocation`, and
  `VkImageView`), which is sized to the swapchain extent and so rides the same
  recreate path. Its `VkDevice` and `VmaAllocator` are **non-owning** — borrowed
  from `VulkanContext` and used only to destroy the children above.
- **`GpuScene`** (program lifetime, created once at startup) owns the device-local
  position, attribute, index, geometry-record, and material buffers; every sampled
  scene image and view; and one shared linear-repeat sampler. It borrows the
  device/allocator and self-idle-waits before destroying sampler → views → images →
  buffers. `SceneData` is the separate plain CPU value owned by `main()` and remains
  alive through the render loop because physics writes dynamic instance transforms.
- **`PhysicsWorld`** (program lifetime after scene load) owns one opaque
  implementation state: the Jolt registration lease, 10 MiB temporary allocator,
  default-initialized, two-worker-capped thread-pool job system, layer interfaces,
  `PhysicsSystem`, every body ID, the fixed-step accumulator, dynamic-body lookup,
  topology snapshot, and preallocated transform scratch. It borrows the exact
  `SceneData` passed to
  `createPhysicsWorld`; that scene's relevant arrays and instance-to-mesh mapping
  stay stable until the world dies. It owns no Vulkan object and performs no device
  wait.
- **`AccelerationStructure`** (program lifetime) owns: one persistently mapped
  TLAS-instance input buffer per `FrameResources` slot, a stable-fields instance
  template, one `BlasEntry` handle/backing/address per `SceneMesh`, and the TLAS
  handle and backing buffer. During startup it also owns a scratch arena whose
  disjoint regions serve the batched BLAS builds; that arena is released after the
  startup fence wait. A separate aligned TLAS scratch allocation remains alive for
  the per-frame in-place rebuilds. Like `Swapchain` it borrows its `VkDevice` and
  `VmaAllocator`; it additionally keeps the
  `vkDestroyAccelerationStructureKHR` pointer (a runtime-resolved extension entry
  point) so its destructor can tear down every `VkAccelerationStructureKHR` handle
  without caller involvement.
- **`RtPipeline`** (program lifetime, created once at startup) owns: the descriptor
  set layout, the pipeline layout, the descriptor pool and the one descriptor set
  allocated from it (freed implicitly with the pool, but held as a member — both the
  render path and the rewrite-on-recreate path need the handle), the ray tracing
  pipeline, the SBT buffer + VMA allocation, the four `VkStridedDeviceAddressRegionKHR`s the
  trace consumes, and — transiently, during creation only — the shader module. Like
  the others it borrows its `VkDevice` and `VmaAllocator`. Its destructor needs no extension entry
  points (`vkDestroyPipeline` etc. are core), unlike `AccelerationStructure`.

Things that are neither created nor destroyed by the program (`physicalDevice`, the
`VkQueue` handles, the resolved `RayTracingFunctions`) stay as plain `main()` locals.

`Renderer` is deliberately **not** another owner: it is a parameter bundle over
borrowed handles (in the spirit of `QueueFamilyIndices`), with no destructor and no
idle wait. Its handle members are copies of program-lifetime objects; `Swapchain` is
held by pointer because its members are replaced on every recreate;
`AccelerationStructure` and the mutable CPU `SceneData` are pointers because
`drawFrame` writes one instance-input slot from the current transforms; and the
`FrameResources` array is borrowed by pointer because `VulkanContext` owns it for
the program lifetime. Keeping the frame slots in `VulkanContext` lets the
acceleration-structure build borrow `frames[0]` and size its instance inputs to the
same slot count before the RT pipeline and `Renderer` exist, without forcing
two-phase renderer initialization. `main()` creates the renderer view after
everything it points at, so it cannot outlive what it borrows.

### Destruction order

In `main()` the `VulkanContext` is declared **first**, so every Vulkan owner destructs
before the allocator, device, and surface it borrows from. `VulkanContext` destroys
the allocator immediately before the device. This is the single most important
ordering invariant in the program, and it is what lets every failure path in `main()`
be a bare `return 1;` with no manual cleanup. Beyond "after `ctx`", the borrowing
owners (`Swapchain`, `GpuScene`, `AccelerationStructure`, `RtPipeline`) need no ordering
*relative to each other*: each waits for device idle in its own destructor rather
than relying on a sibling having done so.

The CPU-side lifetime has its own simple invariant: the loaded `SceneData` is
declared before `PhysicsWorld`, so reverse destruction removes and destroys every
Jolt body and releases the registration lease before the borrowed scene dies.
Process-global allocator/trace hooks install once; a mutex-protected active-world
count creates the Jolt factory and registers types on each 0→1 world epoch, then
unregisters types and deletes the factory on 1→0. This permits overlapping
headless-test worlds without tearing global state down under a survivor.

Each Vulkan-owning destructor:

1. Calls `vkDeviceWaitIdle` first, so no submitted device work still references the
   resources about to be freed. The narrower presentation-engine exception for
   `Swapchain` is documented under [Presentation teardown](#presentation-teardown).
2. Tears down its handles in reverse creation order, each guarded by a
   `VK_NULL_HANDLE` / null check so partial bring-up (an early `return 1;`) still
   cleans up correctly.
3. Emits a per-resource log line on teardown.

`recreateSwapchain` and `~Swapchain` share one teardown path
(`destroySwapchainResources`). `createSwapchainResources` sets the non-owning device
handle **before** creating any child, so a partial-failure cleanup still has a valid
device to destroy through.

## Bring-up sequence

`main()` runs roughly in dependency order. Each step reports to `std::cerr` and
returns `1` on failure (RAII handles the unwind):

1. **GLFW + Vulkan gate.** `glfwInit`, then `glfwVulkanSupported`. Create a
   visible `GLFW_NO_API` window up front, so Wayland compositors can configure the
   drawable surface before swapchain setup and first presentation.
2. **Instance.** Validation is requested at build time (the
   `XRPHOTON_ENABLE_VALIDATION` CMake option, default ON) but is best-effort at
   runtime: if the Khronos layer or `VK_EXT_debug_utils` is missing (machines without
   the Vulkan SDK), bring-up warns and continues without validation rather than
   failing. The instance extensions are GLFW's required surface set, plus
   `VK_EXT_debug_utils` when validation is enabled; with validation on, the
   debug-messenger create info is chained via `pNext` on the instance create info, so
   validation also covers instance creation and destruction.
3. **Debug messenger** (validation builds only). Standalone `VK_EXT_debug_utils`
   messenger, filtered to warnings and errors, routing messages to `std::cerr`.
4. **Surface.** `glfwCreateWindowSurface`.
5. **Physical device.** `pickPhysicalDevice` takes the first GPU passing every
   suitability check (see [Device selection](#device-selection)).
6. **Logical device + allocator.** One queue per unique {trace, present} family, with
   the ray tracing feature chain and `shaderInt64` enabled; then create VMA.
7. **Ray tracing functions.** `loadRayTracingFunctions` resolves the RT entry points
   via `vkGetDeviceProcAddr`. The acceleration-structure subset is used by step 13
   and the per-frame TLAS rebuild, the pipeline subset by step 14, and
   `vkCmdTraceRaysKHR` by every frame.
8. **Swapchain.** `createSwapchainResources` — swapchain, image views, per-image
   render-finished semaphores, and the storage output image (created last, so it is
   torn down first).
9. **Command pool + frame resources** (trace family): one primary command buffer,
   image-available semaphore, and in-flight fence per frame slot.
10. **CPU scene.** `loadGalleryScene` loads the required generated yard assets and
    probes plus every configured optional exhibit, including the mixed tail;
    transactionally merges and rebases their render/physics arrays; applies every
    yard placement; validates the assembled `SceneData`; resolves fallback/DDS
    scene images; and returns the accepted spawn plus flat `dynamicInstances`.
    The generated crate is always dynamic. Unconfigured optional dynamics skip;
    configured ones must be single-mesh and carry exactly one body recipe.
11. **Physics world.** Immediately after scene load and before any GPU scene
    creation, `createPhysicsWorld` binds that stable `SceneData`, creates one static
    triangle-mesh body for every non-dynamic instance, creates recipe-driven dynamic
    bodies for the selected indices, and optimizes Jolt's broad phase. Construction
    is transactional: an empty owner stays empty on failure.
12. **GPU scene.** `createGpuScene` gates both shader-record buffers against
    `maxStorageBufferRange`, then uploads its five geometry/record buffers and all
    sampled scene images through the borrowed frame-0 slot.
13. **Acceleration structures.** `buildAccelerationStructures` — see
    [Acceleration structures](#acceleration-structures). It takes the frame-slot
    count, builds the stable fields of the TLAS instance template, allocates one
    mapped instance input per slot plus persistent TLAS rebuild scratch, and performs
    the initial BLAS/TLAS build. It borrows `frames[0]`'s command buffer and in-flight
    fence from step 9 and returns them in the state the first `drawFrame` expects;
    the other frame slots remain signaled and untouched.
14. **Ray tracing pipeline.** `createRtDescriptorSet` → `createRtPipeline` →
    `buildShaderBindingTable` → `writeSceneDescriptorSet` — see
    [Ray tracing pipeline](#ray-tracing-pipeline).
15. **Renderer view.** The `Renderer` bundle is populated — last, once every handle
    and object it borrows exists, including `ctx.frames.data()`, the acceleration
    structures, and the CPU scene — then the initial
    `prepareRtForSwapchain` (descriptor write + dispatch-limit gate) runs against it.
16. **Render loop.** Update the camera, call `stepPhysics` (also on resize-dirty
    iterations), derive the camera push payload, then call
    `drawFrame(renderer, currentFrame, cameraPush)`;
    `drawFrame` writes that slot's complete TLAS instance array and records an
    in-place TLAS rebuild before tracing. Rotate `currentFrame` modulo
    `MaxFramesInFlight`; recreate when GLFW reports a framebuffer resize or
    acquire/present returns out-of-date/suboptimal, followed by
    `prepareRtForSwapchain` against the fresh storage image.

### Device selection

`pickPhysicalDevice` builds a complete suitability report for each candidate and takes
the first one that passes every category:

```
queueFamilies.isComplete()           // a compute+graphics (trace) and a present family
  && apiVersion >= 1.3                // the core API baseline
  && requiredExtensions.isComplete()  // the RT stack + swapchain
  && hasRequiredSwapchainSupport      // format, present mode, usages, + storage/blit support
  && hasRequiredAccelerationStruct... // BLAS vertex format backstop (spec-mandated support)
  && requiredRayTracingFeatures...    // the features actually enabled
```

Rejected candidates are named and every failed independent category is written to
`std::cerr`; extension and feature failures name each missing item. Ray tracing feature
structs are queried only when the candidate exposes the core version that defines the
buffer-device-address struct and advertises the extensions that define the other two,
because putting an unsupported feature struct in the `pNext` chain is invalid. This
diagnostic pass does a little more startup querying than the former short-circuiting
aggregate, but physical-device selection still uses the same requirements and still
chooses the first suitable GPU.

`hasRequiredSwapchainSupport` now covers the render path's format prerequisites, not
just raw swapchain support: beyond a usable format, present mode, and the required
image usages, it requires that the storage format
(`R8G8B8A8_UNORM`) supports the storage/transfer/blit-source features and that at
least one available surface format is an 8-bit **sRGB** format paired with
`SRGB_NONLINEAR` and usable as a blit destination. Device selection and format choice
use the same predicate, so multi-GPU selection cannot pick a device that passes the
gate and then fails later in swapchain creation.

`RequiredDeviceExtensions` (in `vulkan_context.cpp`) is the hardware ray tracing
stack — acceleration structure, ray tracing pipeline, deferred host operations
(required *enabled* by `VK_KHR_acceleration_structure` even though nothing here
defers) — plus `VK_KHR_swapchain` for presentation. Deliberately absent: **buffer
device address** is core in the 1.3 baseline (its feature is enabled through
`VkPhysicalDeviceVulkan12Features`, and
`vkGetBufferDeviceAddress` is resolved by its core name — a 1.3 driver need not
still advertise the promoted KHR extension string), and **pipeline library** is
only an optional interaction of the RT pipeline extension, never used here.

`hasRequiredAccelerationStructureFormatSupport` (in `acceleration_structure.cpp`,
next to the format it checks) verifies the BLAS vertex format supports acceleration
structure builds. The spec mandates that support wherever the feature exists, so
this is a conformance backstop in the "check anyway, fail loudly" family (like the
trace dispatch gate), not a real capability query.

The feature chain uses `VkPhysicalDeviceVulkan12Features` for both
`bufferDeviceAddress` and `shaderSampledImageArrayNonUniformIndexing`, alongside
the ray-tracing-pipeline and acceleration-structure extension structs. Core
`VkPhysicalDeviceFeatures` carries `shaderInt64`, `textureCompressionBC`, and
`samplerAnisotropy`.
Selection queries this exact chain and device creation reuses it with the required
bits enabled; the standalone buffer-device-address feature struct is deliberately
absent because chaining both promoted forms is invalid.

Scene textures add two suitability backstops. The fixed 1,024-entry combined
image-sampler array must fit the per-stage and per-set sampled-image/sampler limits,
and `maxPerStageResources` must cover the array plus the two hit-stage SSBOs. For
RGBA8-sRGB, BC1-sRGB, and BC3-sRGB, selection requires optimal-tiling sampled,
linear-filter, and transfer-destination format features and confirms the exact 2D
optimal `SAMPLED | TRANSFER_DST` image-creation tuple. Actual image extents are
later gated against that tuple's returned `maxExtent` at `GpuScene` creation. The
device must expose the feature and limit required by the shared 16× anisotropic
scene sampler.

### Queue families

`QueueFamilyIndices` tracks two roles:

- **`traceFamily`** — a family supporting **both compute and graphics**. Named
  "trace" because it is where the ray tracing work is recorded; each frame records
  the trace and the blit. The single-command-buffer renderer needs one family for both
  `vkCmdTraceRaysKHR` (compute) and `vkCmdBlitImage` (graphics-only), so a device that
  exposes graphics and compute *only* on disjoint families is deliberately rejected
  (a split graphics/blit queue — with its ownership transfers and extra semaphores —
  is an async-compute optimization with no current payoff).
- **`presentFamily`** — a family that can present to the surface.

The scan prefers a **single family covering both roles**, so trace and present coincide
whenever the hardware allows it; only if no combined family exists does it fall back to
the first match for each role independently. The `has*` booleans distinguish "found
family index 0" from "no family found", since 0 is a valid index. When the families
differ, the swapchain images are created `VK_SHARING_MODE_CONCURRENT` across both; when
they coincide, `VK_SHARING_MODE_EXCLUSIVE` (valid and faster). The scan lives with the
other suitability checks (file-private in `vulkan_context.cpp`); callers receive the
indices from `pickPhysicalDevice`.

## The frame

Up to `MaxFramesInFlight` frames can be queued. Each loop iteration `main()` first
computes the elapsed time, clamps it to the shared `PhysicsMaxFrameDt` = 0.1 s,
passes that value to `updateCamera`, then either submits player movement or suspends
the character while the free camera is active before calling `stepPhysics` with the
same value. Physics independently
validates and clamps its public input, performs zero or more 60 Hz character/rigid
updates, and atomically writes every dynamic body-origin transform into the bound
`SceneData`; when less than one fixed step is accumulated, the latest poses simply
repeat. The player camera is then attached to the queried feet position plus its eye
offset. This happens even on an iteration that will skip drawing for resize, so
simulation advances at clamped real time rather than presentation count. A physics
failure is loud and exits the loop.

`main()` then derives the frame's `CameraPushConstants` from the selected camera and
the current `swap.extent` aspect ratio (read fresh every iteration, so a recreate
needs no camera-specific handling). A GLFW framebuffer-size callback sets a resize-dirty flag;
a dirty iteration goes straight to recreation because some Wayland compositor/driver
pairs continue scaling an old swapchain without returning `OUT_OF_DATE` or
`SUBOPTIMAL`. The flag is cleared only after the legal Vulkan extent has been selected
and recreation succeeds, so a surface-fixed or min/max-clamped extent cannot cause a
comparison loop. `main()` owns a `currentFrame` cursor and rotates it after every
`drawFrame(renderer, currentFrame, cameraPush)` call; a resize-dirty iteration skips the
draw and leaves the cursor unchanged. Each slot has its own command buffer,
image-available semaphore, in-flight fence, and mapped TLAS-instance input.
`drawFrame` in [src/renderer.cpp](src/renderer.cpp) reaches everything through the
`Renderer` view (the camera payload rides as a parameter, not a `Renderer` member — it
is per-frame data, not a program-lifetime handle):

```
frame = frames[frameIndex]
vkWaitForFences(frame.inFlightFence)   // block until this slot's prior submit retired
writeTlasInstances(frameIndex)         // validate transforms; rewrite only this slot
  └─ failure             -> return before acquiring a swapchain image
vkAcquireNextImageKHR                  // -> imageIndex, signals frame.imageAvailableSemaphore
  ├─ OUT_OF_DATE        -> return (caller recreates the swapchain)
  └─ bounds-check imageIndex against the per-image vectors
vkResetCommandBuffer
recordTraceCommandBuffer               // see below
vkResetFences(frame.inFlightFence)     // only now that a submit is guaranteed to follow
vkQueueSubmit(traceQueue)              // waits frame.imageAvailable@TRANSFER,
                                      // signals renderFinished[i] + frame.inFlightFence
vkQueuePresentKHR(presentQueue)        // waits renderFinished[i]
  └─ OUT_OF_DATE / SUBOPTIMAL -> return (caller recreates)
return acquireResult                   // surfaces a SUBOPTIMAL acquire to the caller
```

Advancing the cursor even after an acquire returns out-of-date is safe: that frame
slot did not submit work, so its fence remains signaled and its image-available
semaphore was not consumed by a queue submission.

`recordTraceCommandBuffer` records a one-time-submit buffer with eight steps —
rebuild the TLAS, trace into the storage image, then blit it into the acquired
swapchain image:

1. `recordTlasRebuild` records a pre-build memory barrier from
   `RAY_TRACING_SHADER | ACCELERATION_STRUCTURE_BUILD` to
   `ACCELERATION_STRUCTURE_BUILD`, with prior acceleration-structure writes made
   available to build reads/writes. It then performs a full in-place TLAS `BUILD`
   from this frame slot's instance buffer and the persistent scratch region, followed
   by an `ACCELERATION_STRUCTURE_BUILD → RAY_TRACING_SHADER` barrier from AS write
   to AS read. The first dependency orders the shared TLAS and scratch after older
   rebuilds and traversal; the second makes this rebuild visible to this frame's
   trace.
2. Barrier the storage image `UNDEFINED → GENERAL` (`srcStageMask`
   `RAY_TRACING_SHADER`, destination `RAY_TRACING_SHADER`/`SHADER_WRITE`;
   `oldLayout` `UNDEFINED` discards prior contents — the whole image is
   overwritten). `GENERAL` is the layout storage-image writes require and must match
   what the descriptor set declared. The source stage chains from the previous
   frame's trailing storage-image barrier without involving the acquire wait's
   `TRANSFER` stage.
3. Bind the pipeline and descriptor set at `PIPELINE_BIND_POINT_RAY_TRACING_KHR`,
   push the frame's `CameraPushConstants` (`vkCmdPushConstants`, raygen-only —
   recorded into this slot's own command buffer after its fence wait, which is
   what makes the camera race-free across frames in flight by construction),
   then `vkCmdTraceRaysKHR` with the owner's four SBT regions and the swapchain
   extent — one ray per pixel. The immediately preceding rebuild's post-build
   barrier makes the fresh TLAS visible to this traversal.
4. Barrier the storage image `GENERAL → TRANSFER_SRC_OPTIMAL` (shader-write →
   transfer-read), to be the blit source.
5. Barrier the acquired image `UNDEFINED → TRANSFER_DST_OPTIMAL`, the blit destination.
6. `vkCmdBlitImage` storage → swapchain (matching extents, `VK_FILTER_NEAREST`). A
   **blit**, not a copy, on purpose: the selected swapchain format is always sRGB, so
   format conversion gamma-encodes the storage `UNORM` value for presentation here.
7. Execution-only barrier `TRANSFER → RAY_TRACING_SHADER`, so a later frame cannot
   overwrite the shared storage image until this frame's blit has finished reading
   it. No memory dependency is needed for this write-after-read hazard; only ordering
   matters.
8. Barrier the acquired image `TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR` for presentation.

`recordTraceCommandBuffer` (and its `recordImageBarrier` / `recordExecutionBarrier`
helpers) are file-private in
`renderer.cpp`; the unit's exported surface is `drawFrame` and
`prepareRtForSwapchain`, each taking only the `Renderer` view.

## Synchronization model

The frame model has two rotating frame slots:

| Primitive | Count | Owner | Role |
|-----------|-------|-------|------|
| `frames[i].imageAvailableSemaphore` | one **per frame in flight** | `VulkanContext` | Signaled by acquire; that frame slot's submit waits on it at the `TRANSFER` stage. |
| `renderFinishedSemaphores[i]` | one **per swapchain image** | `Swapchain` | Signaled by the submit; the present waits on it. |
| `frames[i].inFlightFence` | one **per frame in flight** | `VulkanContext` | Signaled when that slot's submit completes; the next reuse of the slot waits on it. Created **already signaled** so the first wait for each slot does not deadlock. |
| `frames[i].commandBuffer` | one **per frame in flight** | `VulkanContext` | Reset and rerecorded only after the matching in-flight fence proves the slot's previous submission has retired. |
| `instanceSlots[i]` | one **per frame in flight** | `AccelerationStructure` | Persistently mapped, host-coherent TLAS build input. The CPU rewrites only the slot whose matching fence has completed, so an in-flight build never races a host write. |

Synchronization details worth preserving:

- **Stage matching and pre-acquire overlap.** The acquire semaphore waits at
  `TRANSFER`, matching the first thing the submit does to the *swapchain* image: the
  blit-destination transition (step 5) and the blit itself (step 6), both `TRANSFER`
  work. So the swapchain transition cannot begin before the image is acquired. The
  TLAS rebuild and trace (steps 1–3) run outside that wait stage, so the GPU is free
  to rebuild and trace before — or overlapping — the acquire; only the blit onto the
  swapchain image is serialized behind it. The present barrier's `dstStageMask` is
  `BOTTOM_OF_PIPE` because no later GPU stage consumes the image — the render-finished
  semaphore is what the present actually waits on.
- **Shared storage image reuse.** The storage image is still one image shared by all
  frame slots. To keep frame N+1 from discarding it while frame N's blit is still
  reading it, the frame records a trailing execution-only barrier after the blit:
  `TRANSFER → RAY_TRACING_SHADER`. The next frame's leading `UNDEFINED → GENERAL`
  transition uses `srcStageMask = RAY_TRACING_SHADER`, chaining onto that trailing
  barrier. This orders "old blit read" before "new trace write" without making ray
  tracing wait on the acquire semaphore's `TRANSFER` stage. `oldLayout = UNDEFINED`
  still discards prior contents, so `srcAccessMask` stays zero.
- **Per-image, not per-frame, render-finished semaphores.** A present is signaled
  against the specific acquired image, so these are sized to the image count and
  indexed by `imageIndex`, even though command buffers, image-available semaphores,
  and fences rotate by frame slot.
- **Fence before semaphore reuse.** `drawFrame` waits the frame slot's fence before
  acquiring with that slot's image-available semaphore again, proving any prior
  submission that consumed the semaphore has retired before the semaphore is reused.
  That same wait precedes `writeTlasInstances`, proving the prior build has stopped
  reading this slot's mapped buffer; the fallible validation/write remains before
  acquire so failure cannot strand an acquired image.
- **Shared TLAS and scratch reuse.** All frame slots rebuild one TLAS with one
  persistent scratch region. The rebuild's pre-barrier has source stages
  `RAY_TRACING_SHADER | ACCELERATION_STRUCTURE_BUILD` and destination stage
  `ACCELERATION_STRUCTURE_BUILD`: its execution dependency orders the next write
  after older traversal reads, while AS-write → AS-read/write access scopes make
  older TLAS and scratch writes visible before reuse. Its post-barrier is
  AS-build-write → ray-tracing-AS-read, so the same command buffer's trace sees the
  completed rebuild.

### Presentation teardown

Steady-state render-finished semaphore reuse is spec-grounded: each semaphore is
indexed by acquired swapchain image, and the next submission waits on that image's
acquisition before it can signal the same semaphore again, proving the previous present
has finished consuming it. Recreate and shutdown are narrower: without a maintenance
extension, `vkQueuePresentKHR` exposes no completion fence, so `vkDeviceWaitIdle`
formally retires submitted device work but does not prove that the presentation engine
has released its wait semaphores. [Khronos documents this gap and the same pragmatic
idle-then-destroy practice](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
The engine deliberately accepts it: per-image semaphores make the normal frame path
correct, unextended Vulkan offers no stronger direct teardown signal, and an optional
extension path would create a second teardown system for no observed failure here. If
`VK_KHR_swapchain_maintenance1` becomes part of the required baseline, per-present
fences are the upgrade path and must be waited before presentation resources are
destroyed; until then, do not add a parallel present-fence path.

## Swapchain and resize handling

`Swapchain` is the unit that gets rebuilt when the surface changes. The trigger is a
GLFW framebuffer-size notification, or
`VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` from acquire or present. The explicit
dirty flag covers Wayland implementations that keep accepting and scaling the old
swapchain; either trigger reaches the same `recreateSwapchain` path. It is cleared after
successful recreation rather than by comparing raw GLFW dimensions with the legal
Vulkan extent, which may be surface-fixed or capability-clamped.

`recreateSwapchain`:

1. `waitForDrawableFramebuffer` — block (pumping events) while the window has a
   zero-area framebuffer, e.g. while minimized. Returns false if the window is closing,
   in which case recreation is skipped.
2. `vkDeviceWaitIdle` — retire submitted device work before destroying the old
   resources, subject to the documented [presentation teardown](#presentation-teardown)
   gap.
3. `destroySwapchainResources` then `createSwapchainResources` — the same teardown
   path the destructor uses, then a fresh build.

Selection policy inside `createSwapchain`:

- **Format:** restricted to `B8G8R8A8_SRGB` or `R8G8B8A8_SRGB`, paired with
  `SRGB_NONLINEAR` and usable as a blit destination. Prefer BGRA, then RGBA; UNORM and
  10-bit formats are rejected until the renderer has an explicit output-encoding pass.
  The suitability gate uses the same predicate and guarantees one candidate exists.
- **Present mode:** prefer `MAILBOX`; fall back to `FIFO` (always supported).
- **Extent:** the surface's `currentExtent` when fixed; otherwise the window
  framebuffer size clamped to the surface min/max.
- **Image count:** `minImageCount + 1` (so the app isn't forced to wait on the driver),
  clamped to `maxImageCount` when that is non-zero.
- **Composite alpha:** prefer `OPAQUE`, else the first supported mode.
- **Usage:** `TRANSFER_DST` (for the blit destination) + `COLOR_ATTACHMENT` (reserved
  for later attachment-based rendering). A surface lacking either is rejected.

## Storage image

The **storage image** is the trace output target: a device-local image
`vkCmdTraceRaysKHR` writes and the frame then blits to the acquired swapchain
image. It is owned by `Swapchain` because it tracks the swapchain extent and
must be rebuilt on resize, so it rides the existing
`createSwapchainResources` / `destroySwapchainResources` / `recreateSwapchain`
machinery rather than introducing a new lifetime path. A **single** image suffices
for now even with two frames in flight: the frame path adds explicit barriers so a
later frame cannot overwrite it until the previous frame's blit has finished reading
from it. Per-frame storage images remain a possible future simplification if the app
needs deeper overlap.

- **Format:** `R8G8B8A8_UNORM` — a member of Vulkan's guaranteed storage-image set,
  defined once as `StorageImageFormat` in `swapchain.cpp` so the suitability gate and
  the allocation use the exact same value.
- **Usage:** `STORAGE` (the traceRays write) `| TRANSFER_SRC` (blit source), with
  `TILING_OPTIMAL`, 1 mip / 1 layer, and `EXCLUSIVE` sharing. Unlike the swapchain
  images, it is only ever touched on the trace queue, so it needs no cross-family
  sharing.
- **Memory:** `vmaCreateImage` uses `VMA_MEMORY_USAGE_AUTO` with `DEVICE_LOCAL`
  required; VMA selects, allocates, and binds a compatible memory type.
- **Capability gating:** see [Device selection](#device-selection). Selection
  guarantees a supported device; `createStorageImage` keeps a cheap backstop that
  re-asserts the format helpers and fails `VK_ERROR_FEATURE_NOT_PRESENT` on a logic
  error rather than a supported device.
- **Image view:** what the RT descriptor set binds (storage image, binding 1). Because
  the view is recreated with the swapchain, every recreate obligates a descriptor
  rewrite — see [Ray tracing pipeline](#ray-tracing-pipeline).
- **Lifetime:** created **last** in `createSwapchainResources` (after the
  render-finished semaphores, when `swap->extent` is populated) and destroyed **first**
  in `destroySwapchainResources` (reverse creation order). Teardown is null-guarded, so
  a partial create and the recreate error path both clean up through the same path.

## Physics

Rigid dynamics are engine-side and renderer-independent. Jolt Physics v5.6.0 is
vendored under [`third_party/jolt`](third_party/jolt) with its MIT license and
built as a static target only below the `XRPHOTON_BUILD_ENGINE` gate. Its one
local thread-pool cleanup patch is recorded alongside the dependency. The
graphics-free OGFx core continues to own and test backend-neutral recipes without
configuring Jolt. The exact live integration contracts are recorded below.

[`PhysicsWorld`](src/physics.hpp) is a move-forbidden RAII owner with a Jolt-free
header and one opaque state pointer. `createPhysicsWorld` binds one caller-owned
`SceneData` and a list of distinct flat dynamic-instance indices. Creation is
transactional and records the sizes and instance-to-mesh references that must
remain stable for the world's lifetime; topology drift later poisons the world
terminally. The scene must outlive the world, and physics exclusively owns writes
to its dynamic instance transforms. Backend-neutral velocity and active-state
functions provide the current control/inspection seam without leaking a `BodyID`;
finite setter inputs are robustly clamped to the pinned 500 m/s body limit.

Body construction has one deliberate split:

- Every non-dynamic instance becomes a static Jolt triangle mesh. Instance
  transforms are baked into vertices so non-uniform static scale is valid;
  geometry-local OGFx indices are explicitly rebased, reflected transforms rewind
  triangles, and open meshes remain single-sided.
- Each dynamic instance must have a proper rigid transform and exactly one
  `ScenePhysicsBody`. Box and cylinder recipe colliders become one transformed
  primitive or a static compound; authored total mass and aggregate center of mass
  are applied to the final shape. Dynamic bodies use linear-cast motion quality.
  Before Jolt sees a shape, finite adversarial inputs are checked against collider
  count, representable mass/inertia, compound-COM, and backend world-bound limits.
  All body poses cross the GLM ↔ Jolt boundary component-by-component in
  `physics.cpp`, and write-back uses `GetWorldTransform` so render/model-origin
  transforms stay correct even with an off-center COM.
- Two object/broad-phase layers let moving bodies collide with everything and
  statics collide only with moving bodies. Every body currently uses friction 0.5
  and restitution 0.0; preserved material strings remain data for a later material
  table. The system is capped at 1,024 bodies, pairs, and contact constraints, then
  optimized once after startup creation.
- One invisible Jolt `CharacterVirtual` supplies the first-person player without a
  `SceneInstance` or TLAS entry. Its 1.8-m standing and 1.2-m crouching capsules
  (both 0.35-m radius) are translated so the public position is the feet point;
  releasing crouch switches back only when Jolt confirms the standing shape fits.
  A lower-sphere supporting plane, enhanced
  internal-edge removal, the moving-layer filters, and `ExtendedUpdate` provide
  wall collision, slope handling, floor sticking, and 0.4-m stair stepping against
  both static yard meshes and dynamic props. Input supplies normalized world X/Z
  run/sprint/crouch velocity (3/12/1.5 m/s); gravity and the optional 5 m/s jump are integrated
  inside the fixed step. The 0.4-m upward stair step and 0.5-m downward floor-stick
  distance are set explicitly instead of inherited from Jolt defaults. Airborne input
  deliberately replaces horizontal momentum, giving this test-focused controller
  full air control for now. Its explicitly configured 70-kg mass supplies downward
  weight on supporting bodies, while a separate 500-N strength limit makes ordinary
  yard props pushable. Free-camera mode suspends its position and clears pending
  input; resuming refreshes contacts before movement continues. It deliberately has
  no inner proxy body yet, so it can push
  bodies it contacts but is not itself present to general ray/shape queries.

`PhysicsWorld::State` owns a double-precision accumulator. `stepPhysics` rejects
non-finite or negative input, adds at most `PhysicsMaxFrameDt` (0.1 s), and calls
`CharacterVirtual::ExtendedUpdate` and `PhysicsSystem::Update` in exact
`PhysicsFixedDt` (1/60 s) increments, with six as the hard catch-up bound. The
character runs first so impulses it applies reach rigid bodies in that same fixed
step. After all updates succeed it gathers every dynamic
body-origin transform into preallocated scratch, validates the full set, and only
then commits it to `SceneData`; update errors or invalid output publish nothing and
make the world terminal. There is deliberately no interpolation, so the renderer
always consumes the latest pose that actually existed in simulation.

## Acceleration structures

The ray tracing scene contains one **BLAS per `SceneMesh`** and one **TLAS entry
per `SceneInstance`**. The generated-only yard builds 5 BLASes and 13 TLAS
instances over 6 geometries; the fully configured yard builds 14 BLASes and 22
TLAS instances over 16 geometries. In both cases the
two wedge placements reference the same BLAS address. The barrel's cylinder
recipe and the tail's box recipe create live bodies when those optional placements
are configured; together with the required dynamic crate, their transforms are
the CPU-side inputs consumed by the same TLAS rebuild path.

`buildAccelerationStructures` builds every BLAS once and creates/builds the TLAS
after `GpuScene` upload. The frame path then performs a full in-place TLAS `BUILD`
before every trace, changing its contents without replacing its handle or backing.
That stable TLAS handle is what the RT descriptor set binds
(`VkWriteDescriptorSetAccelerationStructureKHR` takes the handle — an
acceleration-structure *device address* is needed only where a TLAS instance
references a BLAS). BLASes, TLAS, per-slot inputs, and scratch are all
**swapchain-independent**: resize/recreate waits for idle but never replaces them.

Decisions and contracts worth preserving:

- **One transform-layout boundary.** Scene transforms use GLM's column-major
  `glm::mat4`; `toVkTransformMatrix` in `acceleration_structure.cpp` alone copies
  them into Vulkan's row-major 3x4 `VkTransformMatrixKHR`. The yard's translated
  quad, translated/rotated/non-uniformly-scaled wedge, and physics-driven crate
  make this boundary visible without putting world placement into OGFx.
- **Stable-fields template, per-slot transforms.** Startup creates one
  `VkAccelerationStructureInstanceKHR` template containing the stable mesh address,
  mask, custom index, SBT offset, and flags for each world instance. One mapped,
  host-coherent copy exists per frame slot. After that slot's fence wait,
  `writeTlasInstances` rejects instance-count drift, invalid slot/mapping state, and
  any non-finite or singular transform; only then does it refresh the template's
  transform fields and copy the complete array to that slot. Runtime physics
  therefore cannot silently invalidate the startup-only scene checks or race a
  build that is still in flight.
- **Mesh ranges become BLAS geometry lists.** Each mesh's contiguous
  `[firstGeometry, firstGeometry + geometryCount)` range is emitted in order as
  one `VkAccelerationStructureGeometryKHR` per `SceneGeometry`. Vertex and index
  addresses are pre-offset to that geometry's range; OGFx indices stay local, so
  the build range uses `primitiveOffset = 0`, `firstVertex = 0`, and
  `maxVertex = vertexCount - 1`. Opaque ranges set
  `VK_GEOMETRY_OPAQUE_BIT_KHR` and bypass any-hit in hardware; alpha-tested
  ranges deliberately leave the flag clear so their SBT-selected any-hit shader
  can reject a texel. A mesh such as the tail can contain both kinds in one BLAS.
- **Hit-record identity is flat and stable.** Each TLAS instance stores its
  mesh's `firstGeometry` as `instanceCustomIndex`. Vulkan's `GeometryIndex()` is
  the BLAS-local geometry index, so `InstanceID() + GeometryIndex()` recovers the
  corresponding flat `GeometryRecord` index for every mesh and for any number of
  placements sharing one BLAS. Its SBT record offset is
  `mesh.firstGeometry * RayTypeCount`; `TraceRay` uses the same `RayTypeCount` as
  its geometry-record multiplier, so each local geometry reaches the opaque or
  alpha-tested record selected for the matching flat geometry. The shared
  C++/Slang ABI currently fixes `RayTypeCount` at 1. Instance flags remain zero:
  opacity belongs to each BLAS geometry, not to an instance that may contain
  both classes.
- **Staged device-local geometry.** `GpuScene` uploads vertex, attribute, index, and
  record data with `uploadDeviceLocalBuffer`: a transient mapped, host-coherent transfer-source buffer
  feeds a `TRANSFER_DST` device-local buffer, then dies after the submission fence.
  The destination is parked directly in `GpuScene`, preserving the
  null-guarded partial-failure teardown contract. The per-slot instance buffers
  remain mapped host-visible memory because every frame rewrites one from the CPU.
  Before upload, geometry and material record byte sizes are checked against
  `maxStorageBufferRange` on the selected physical device.
- **Upload visibility crosses submissions deliberately.** Every upload ends with a
  `VkMemoryBarrier` from `TRANSFER` writes to acceleration-structure-build reads and
  ray-tracing-shader reads. The fence wait only synchronizes the device with the host;
  the barrier's second scope is what makes the copy visible to later queue submissions,
  so neither the AS build nor frame path needs an upload-specific barrier.
- **Sampled scene textures.** The Vulkan-free resolver always creates image 0 as a
  1×1 opaque-white RGBA8-sRGB fallback, then appends deduplicated
  BC1/BC3/uncompressed-RGBA8 images in
  first-material-use order. `GpuScene` reserves ownership before creation, uploads
  each exact mip-0 payload through a transient staging buffer, and records
  `UNDEFINED → TRANSFER_DST_OPTIMAL`, the tightly packed buffer-to-image copy, then
  `TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL` with ray-tracing-shader read
  visibility. A failed post-submit fence wait device-idles before transient staging
  memory can die. Views are created after all images and the shared linear-repeat,
  16×-anisotropic, max-Lod-0 sampler last, giving strict reverse teardown.
  Compressed payloads stay compressed in device memory; no runtime DXT
  decompression or mip generation exists. Ray generation carries neighboring
  primary-ray directions to hit stages; they reconstruct explicit UV gradients so
  `SampleGrad` can apply anisotropy despite ray tracing having no implicit fragment
  derivatives. The gradients still clamp to the sole uploaded mip 0. Both
  closest-hit and alpha-tested any-hit sample this same table. The tail's
  shipped DDS is fully opaque at mip 0, so it exercises the any-hit sample and
  cutoff comparison without taking the visible `IgnoreHit` branch. The leaf
  card resolves a DXT1 mip with both opaque and transparent samples and visibly
  exercises the rejection branch.
- **Device-address rules.** The build consumes buffer *device addresses*, not
  descriptors, so the input and scratch buffers carry
  `SHADER_DEVICE_ADDRESS` usage. The program-lifetime VMA allocator is created with
  `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`, so VMA derives the matching
  Vulkan memory-allocation flag from that usage and the two cannot diverge. The
  **BLAS backing buffers** also need the usage — querying
  a BLAS's acceleration-structure address for the TLAS instance requires it of the
  buffer underneath (VUID 09542). The **TLAS backing buffer** deliberately does not:
  nothing queries a TLAS address.
- **Build-input alignment by construction and guard.** Position and `uint32` index
  buffer base addresses are checked for 4-byte alignment, and every non-pointer
  instance-array base for 16-byte alignment. Geometry range offsets are element-granular
  (`firstVertex × 12` and `firstIndex × 4`), so every derived build-input address keeps
  the required 4-byte alignment without byte padding. Startup still fails with a named
  diagnostic if a base-address premise is ever violated.
- **Scene counts are gated against the device.** Instance count must fit both
  `maxInstanceCount` and the 32-bit TLAS build-range field; each mesh's geometry
  count must fit `maxGeometryCount`; and its summed triangle count must fit
  `maxPrimitiveCount`. A zero frame-slot count is rejected before any Vulkan
  resource exists, because the startup build itself requires slot 0. These checks
  happen before Vulkan consumes narrowed counts or slot indices.
- **Checked transient and persistent scratch.** Batched BLAS builds may execute concurrently, so
  every BLAS receives a disjoint region sized and aligned to
  `minAccelerationStructureScratchOffsetAlignment`. The arena's BLAS requirement
  is the checked sum of those aligned sizes. Its allocation adds `alignment - 1`
  bytes of slack and rounds the returned *device address* up; it is released after
  startup completes. The TLAS gets a separate program-lifetime scratch allocation,
  likewise slackened and aligned once, whose cached address every frame reuses.
  Checked `VkDeviceSize` arithmetic turns pathological overflow into a loud error.
- **Startup submission, then per-frame dependencies.** All BLASes are recorded in one batched
  `vkCmdBuildAccelerationStructuresKHR` call, followed by a build-stage barrier
  from AS write to **AS read and AS write**. The read scope makes BLAS contents
  visible to the initial TLAS build; its AS-write destination matches the later
  rebuild scope even though TLAS scratch is separate from the BLAS arena. The TLAS
  build follows, then a barrier from AS-build write to ray-tracing-shader AS read.
  An acceleration structure's address is fixed at creation, so the TLAS can be
  recorded before the BLAS batch executes; the barriers order the contents. Every
  frame records its own pre-build barrier, full TLAS build, and post-build barrier as
  described under [Synchronization model](#synchronization-model).
- **Borrowed sync.** All buffer and texture uploads plus the AS build reuse
  `frames[0]`'s command buffer
  and in-flight fence. Each reset → submit → wait cycle leaves the fence signaled,
  exactly the state the next startup submission and first `drawFrame` wait depend on,
  without introducing temporary sync objects that could leak on a failure path. The
  command buffer is reset between submissions; the other frame slots are untouched.
- **Scratch release.** Both scratch allocations live in the owner (not as locals) so
  a failed build bare-returns and the destructor cleans up. On success the transient
  BLAS arena is destroyed immediately after the startup fence wait; TLAS scratch
  remains until owner teardown for the per-frame rebuilds.
- **Transactional ownership and teardown.** The BLAS vector reserves before any
  Vulkan creation, and each created handle/backing is adopted immediately. Standard
  allocation failures are translated to `VK_ERROR_OUT_OF_HOST_MEMORY`. The
  destructor waits for device idle, destroys every BLAS and the TLAS handle before
  their backing buffers, then releases transient/persistent scratch and every
  per-slot instance buffer.
  `GpuScene` independently owns the geometry buffers.

## Ray tracing pipeline

The machinery that turns the TLAS into pixels: four shader stages, the pipeline
over them, the shader binding table `vkCmdTraceRaysKHR` indexes into, and the
descriptor set binding the TLAS, storage image, geometry records, materials, and
sampled scene textures. Owned by `RtPipeline`
([src/rt_pipeline.hpp](src/rt_pipeline.hpp)), created once at startup in three
steps (`createRtDescriptorSet` → `createRtPipeline` → `buildShaderBindingTable`),
program-lifetime except for one resize obligation described below.

Decisions and contracts worth preserving:

- **Shaders are Slang, embedded at build time.** All four stages live in one
  [shaders/raytrace.slang](shaders/raytrace.slang) module: `rayGenMain`
  (perspective rays from the camera push constants — see [Camera](#camera) for
  the payload contract — storage-image write at binding 1, `[format("rgba8")]`
  because the device's `shaderStorageImageWriteWithoutFormat` is not enabled),
  `missMain` (the dark red background), `closestHitMain` (indexed BDA fetch of
  normals/UVs followed by material-factor × sampled-base-color × view-dependent
  normal shading), and `anyHitMain` (the same indexed UV/material fetch followed
  by sampled-alpha × material-alpha comparison against `alphaCutoff`, calling
  `IgnoreHit` below the cutoff). Raygen uses `RAY_FLAG_NONE`: per-geometry BLAS
  flags and SBT selection, not `RAY_FLAG_FORCE_OPAQUE`, decide whether any-hit
  runs. CMake compiles the module with `slangc
  -target spirv -fvk-use-entrypoint-name -source-embed-style u32` into a
  self-contained C header (`raytrace_spv.h`, includes prepended by the build) that
  `rt_pipeline.cpp` `#include`s — no runtime shader file paths, keeping the
  single-executable ethos. slangc is located via `find_program` (it is not a
  FindVulkan component).
- **One module, four stages.** Slang compiles every `[shader(...)]` entry point
  into a single SPIR-V module, so the pipeline creates one `VkShaderModule` and
  selects stages by `pName`. The module is parked in the owner during creation (the
  scratch-buffer pattern: a failure bare-returns and the destructor cleans up) and
  destroyed as soon as the pipeline exists.
- **Groups in SBT order.** Group 0 `GENERAL` (raygen), 1 `GENERAL` (miss), 2
  `TRIANGLES_HIT_GROUP` (opaque, closest hit only), and 3
  `TRIANGLES_HIT_GROUP` (alpha-tested, shared closest hit plus any-hit). Triangle
  intersection remains fixed-function. Unused shader indices are set to
  `VK_SHADER_UNUSED_KHR` *explicitly* — zero-init would leave 0, a valid stage
  index, producing a silently wrong pipeline rather than a validation error.
  `maxPipelineRayRecursionDepth = 1` (primary rays only; 1 is the spec-guaranteed
  minimum, so no limit query).
- **SBT layout.** One host-visible + coherent buffer
  (`SHADER_BINDING_TABLE | SHADER_DEVICE_ADDRESS`), written once at startup. It stays
  mapped because it is a tiny specialized table, independent of the reusable
  device-local geometry-upload policy. Record stride =
  `shaderGroupHandleSize` rounded to `shaderGroupHandleAlignment`; each region
  starts at a `shaderGroupBaseAlignment` multiple from the table base. The
  alignment math uses a **general** round-up-to-multiple (not the AS build's
  bit-mask `alignUp`): the spec guarantees power-of-two for the handle alignment
  but describes `shaderGroupBaseAlignment` only as "required alignment". The buffer
  carries `baseAlignment − 1` bytes of slack, the base *device address* is rounded
  up (the VUIDs constrain region device addresses, not buffer offsets), and the CPU
  write pointer is shifted by the **same** delta — otherwise handles land at
  unaligned offsets while the regions point at aligned ones and the GPU reads
  garbage with no validation error. The raygen region contains exactly one record
  and its `size` equals its `stride` (spec requirement). The miss region contains
  `RayTypeCount` records, and the hit region contains
  `geometryCount * RayTypeCount` records interleaved by ray type within each flat
  geometry. Startup copies group 2's handle into opaque geometry records and group
  3's handle into alpha-tested records. CMake supplies the same
  `XRPHOTON_RAY_TYPE_COUNT=1` definition to C++ and Slang; the C++
  `RayTypeCount`, TLAS instance offsets, SBT layout, and `TraceRay` multiplier are
  one routing ABI rather than independently tunable values. The callable region
  is empty (`size = stride = 0`) but its
  `deviceAddress` points at the table base: the current VUID (03692)
  unconditionally requires a valid SBT-buffer address, and reusing the existing
  buffer satisfies the strict reading for free (the common `{0,0,0}` idiom relies
  on validation-layer leniency).
- **BDA/ABI probe.** `GeometryRecord` carries 64-bit device addresses for pre-offset
  index, position, and all-scalar 20-byte attribute streams. C++ `static_assert`s pin
  the 32-byte record layouts; emitted SPIR-V confirms identical offsets/strides.
  Both hit stages index the record with `InstanceID() + GeometryIndex()` and
  interpolate UV/material data; closest-hit additionally fetches normals and
  transforms them with the inverse-transpose implied by row-vector multiplication
  with `WorldToObject3x4()`.
- **Descriptor set:** binding 0 TLAS and binding 1 storage image are raygen-only;
  bindings 2–3 are geometry/material storage buffers visible to hit stages. Binding
  4 is a fixed 1,024-entry combined-image-sampler array visible to closest-hit and
  any-hit. Startup writes every slot: real scene images first, then the
  white fallback view in every unused slot, all sharing one sampler. The shader uses
  `NonUniformResourceIndex`; no partially-bound, variable-count, or runtime-array
  feature is enabled. The pool holds exactly the one
  set, without `FREE_DESCRIPTOR_SET_BIT` (the set is only released with the pool).
  The TLAS write chains `VkWriteDescriptorSetAccelerationStructureKHR` via `pNext`;
  the image write declares `IMAGE_LAYOUT_GENERAL`, which the frame's first barrier
  makes true before every trace.
- **Pipeline layout: the one set layout plus a raygen-only push-constant range**
  (`sizeof(CameraPushConstants)`, 64 bytes at offset 0). The frame path's
  `vkCmdPushConstants` uses the identical stage flags — the
  `vkCmdPushConstants` VUIDs require every pushed byte+stage to fall inside a
  declared range and the push to cover every stage of any range it overlaps.
  The payload contract itself lives in [Camera](#camera).
- **The resize contract.** The storage image view is recreated with the swapchain,
  so after every successful `recreateSwapchain` the render loop calls
  `prepareRtForSwapchain`, which (a) rewrites the descriptor set to the fresh view
  — race-free because the recreate device-idles first — and (b) re-gates the trace
  dispatch dimensions (`swap.extent`) against the `vkCmdTraceRaysKHR` VUIDs
  (compute work-group limits × `maxRayDispatchInvocationCount`; spec minimum 2^30,
  so any realistic swapchain passes — checked anyway to fail loudly rather than hit
  undefined behavior on an exotic driver). The same call runs once at startup, so
  the two post-recreate obligations are one code path.
- **Teardown.** `~RtPipeline` waits for device idle, then destroys pipeline →
  (parked shader module, failure paths only) → pipeline layout → descriptor pool →
  descriptor set layout → SBT buffer/allocation, all null-guarded.

## Camera

Perspective player and collision-free views, delivered to the raygen shader through
the same push constants. `main()` owns two plain `Camera` values plus shared input
edge state — no Vulkan objects and no RAII. The player view's position is overwritten
from the Jolt character's feet plus the actual stance's 1.7-m/1.1-m eye offset; the
free view retains the original direct fly controls. F1 swaps the active value, so
entering free mode copies the player's current eye position, yaw, pitch, and field
of view into the free camera every time. Free-camera movement then changes only that
copied value while the renderer continues to consume one ordinary `Camera`; the
character is suspended for the entire time the free view is active.

Decisions and contracts worth preserving:

- **Push constants over per-frame uniform buffers, deliberately.** The payload
  is 64 bytes — half the spec-guaranteed 128-byte `maxPushConstantsSize` — it is
  recorded into each frame slot's own command buffer after that slot's fence
  wait, so frames in flight cannot race on it *by construction*, and no
  descriptor layout change was needed (only the push range on the pipeline
  layout). Per-`FrameResources`-slot uniform buffers are the designated
  promotion path when a payload outgrows the push range — expected at scene
  time, not camera time.
- **Origin + pre-scaled ray basis, not inverse view/projection matrices.**
  `CameraPushConstants` carries the camera origin plus three basis vectors:
  `forward` unit-length, `right`/`up` pre-scaled on the CPU by
  `tan(verticalFov/2)` (and aspect, for `right`). The raygen shader computes
  `normalize(forward + ndc.x·right − ndc.y·up)` — no per-pixel matrix multiply,
  no w-divide — and the CPU side stays matrix-free (yaw/pitch → basis needs only
  `sin`/`cos`, cross, normalize). The first consumer of a real view-projection
  matrix is temporal reprojection (roadmap step 5), which can build one from
  this same camera state when it lands. Scaling `right` by the swapchain aspect
  each frame is what fixed the bring-up NDC-square stretch on resize.
- **GLM is the single math system.** Geometry M2 retired the in-house `Vec3` when
  instance transforms created the first matrix requirement. Camera vectors are
  now `glm::vec3`; the guarded normalization helper still returns zero for
  near-zero input, and the movement path additionally skips near-zero sums —
  normalizing a zero vector is the classic NaN that permanently poisons the
  position. The GLM column-major to Vulkan row-major 3x4 conversion has one
  owner in `acceleration_structure.cpp`.
- **Payload ABI.** The shader sees four `float3` fields at 16-byte offsets
  (0/16/32/48); the CPU struct pins the same shape with explicit pad floats and
  `static_assert`s on both `sizeof` (64) and the `offsetof` of every field —
  `float3` rounds up to 16-byte alignment under every GPU layout rule set, so
  the offsets are unconditional. Keep the shader struct and the CPU struct
  field-for-field identical.
- **Basis convention.** World y-up; yaw 0 / pitch 0 looks down **+Z**, and
  `right = normalize(cross(WorldUp, forward))`, `up = cross(forward, right)` —
  chosen so world +X maps screen-right and +Y screen-up, exactly the bring-up
  shader's screen mapping (the first perspective frame reads as "the same
  triangle, now with depth"). The `−ndc.y` flip **stays in the shader**: launch
  IDs counting rows downward is a property of dispatch-index space, not of the
  camera, so the CPU sends an un-flipped, world-up `up`. Pitch is clamped
  strictly inside ±90° (`PitchLimit` = 89°): at exactly ±90° `forward` is
  parallel to `WorldUp` and the `right` cross product degenerates to NaNs.
- **Controls (all GLFW input policy lives in `updateCamera`).** Game-style
  always-captured mouse look: the cursor is captured at startup
  (`GLFW_CURSOR_DISABLED`, plus raw mouse motion where supported — the support
  check matters on Wayland); Escape releases it, left click recaptures, and the
  camera input is frozen and player horizontal input is zero while the cursor is
  free. In player mode, WASD is projected from yaw alone onto world X/Z, Space
  requests an edge-triggered jump, and Left Shift changes 3 m/s running to 12 m/s
  sprinting; Left Ctrl holds a 1.2-m crouched capsule and lowers the eye from 1.7 m
  to 1.1 m. In free mode, WASD retains the full look-relative movement and
  Space/LeftCtrl move along ±world-up. Both paths normalize summed directions so
  diagonals are not faster. F1 is edge-detected so holding it cannot oscillate modes.
  Mouse look polls `glfwGetCursorPos`
  deltas against an anchor stored in `Camera`; the anchor is invalidated on
  every capture transition and re-anchored one frame before deltas apply —
  otherwise the first captured frame integrates the whole cursor jump as one
  giant rotation.
- **Frame timing lives in `main()`.** `glfwGetTime()` deltas, clamped to
  the physics module's exported `PhysicsMaxFrameDt` (0.1 s), keep window drags
  and resize stalls from teleporting the free camera. Player movement is submitted
  as desired velocity before the same delta enters `stepPhysics`, or the character
  is suspended when the free camera is active; its independent
  validation/clamp and `PhysicsWorld` accumulator advance Jolt and the character at
  the exported fixed 60 Hz timestep. Mouse look and free-camera timing remain
  frame-relative.

## Conventions

- **One focus, clear vision.** This is a one-man project, and what makes that
  workable is a single deliberate answer to each design question — not parallel
  systems maintained side by side. Leanness is the side effect, not the goal.
  The path-tracing-only rendering pipeline is the flagship example (no raster
  path, no lightmaps, no renderer variants); the same principle applies to every
  subsystem — when a design fork offers a general mechanism next to a single
  sufficient path, take the single path.
- Everything in `namespace xrphoton`; `main.cpp` pulls it in with `using namespace`.
- Free functions, `camelCase` names; `PascalCase` for constants and types.
- Cross-file functions are declared in the headers; file-private helpers live in an
  anonymous namespace inside each `.cpp`.
- Errors reach boundaries as explicit result objects, `VkResult`, or `bool` and
  are reported to `std::cerr`; exceptions do not cross subsystem APIs.
- Cleanup is RAII via the resource owners, including `PhysicsWorld`, not manual
  unwinding in `main()`. Every `main()` failure path is a bare `return 1;`.
- Comments explain *why*, not *what*: decisions, contracts, and non-obvious Vulkan
  reasoning, not restatements of the code.

## Roadmap

1. **Camera + push constants.** **Landed** — a perspective fly camera (origin +
   pre-scaled ray basis) delivered via raygen-only push constants, with GLFW
   fly controls, fixing the bring-up aspect-ratio distortion on resize. See
   [Camera](#camera) for the decisions and contracts.
2. **Geometry + scene representation.** **Landed** — VMA, GLM transforms,
   staged device-local uploads, indexed BDA-fetched geometry, the complete OGFx
   round trip, generic multi-model scene assembly, N-BLAS/N-instance acceleration
   structures, base-color DDS sampling, mixed opaque/alpha-tested per-geometry
   routing, and the narrow headless Blender 5.1.x static-mesh adapter have landed.
   The narrow SoC rigid-compound barrel and pseudodog-tail adapters plus optional
   engine-neutral physics records have also landed. CMake generates the three
   repository-owned ground/wall/box yard models plus the quad and permanent
   two-geometry wedge probe through the shared writer. The configured yard
   additionally loads the verified legacy-converted
   `plitka1.ogfx`, resolves its BC1 texture, and carries it into the render loop
   beside those probes. The alpha-tested Blender leaf card closes the visible
   any-hit acceptance gap. The opaque-textured, scale-faithful Blender barrel
   remake adds the first newly authored comparison asset beside the converted
   SoC barrel; the original custom barrel then exercises full art-direction
   freedom without changing the format. Plain, GPU-assisted, and synchronization validation
   are clean, and the complete configured yard has been visually checked.
   The wedge remains the repository-owned multi-geometry/shared-BLAS regression
   asset; it is not displaced by later content entries.

   The permanent test yard succeeds the earlier preview row: its generated ground,
   wall corner, steps/platform, static crate, and recipe-driven dynamic crate establish
   the level-like structure, while the additive exhibit ordering is **quad →
   plitka1 → Blender pyramid/spheres → Blender leaf card → wedge probes →
   `bochka_close_1` → remade `bochka_close_1` → custom Stalker barrel →
   `item_psevdodog_tail`**. The
   yard is an integration scene, not another format or runtime path:
   every entry travels through the same OGFx
   decoder, `SceneData`, GPU upload, BLAS/TLAS construction, material/texture
   system, and shaders. Source-specific work stays offline. The narrow M4a legacy
   adapter and `xrPhotonAssetCompiler convert-ogf` produced plitka; Blender is not
   part of that conversion path. The separate landed `convert-blender` path produces
   `test_pyramid.ogfx`, the flat-shaded dense-triangulation/UV-seam/corner-
   splitting `test_sphere.ogfx`, and its smooth-normal comparison
   `test_smooth_sphere.ogfx`, plus the alpha-tested
   `test_leaf_card.ogfx` and opaque-textured
   `remade_bochka_close_1.ogfx` / `custom_stalker_barrel.ogfx`, for the optional
   yard beneath
   `build/<preset>/assets/blender/`. The leaf uses strict XRBM v2 material
   metadata and `trees\trees_new_vetka_green`; its transparent DXT1 samples
   visibly execute `IgnoreHit`. The direct legacy path now also produces
   `bochka_close_1.ogfx`: its type-`0xA` hierarchy is validated, its already
   bind/model-space child mesh is flattened, and its body/cylinder data is
   retained in optional OGFx metadata. The regular barrel resolves
   `mtl\mtl_barrel_01` through the existing DDS path and occupies the same
   yard exhibit without any source-specific runtime branch. The tail conversion
   emits one mesh with two geometries—`models\model_aref` alpha-tested and
   `models\model` opaque—sharing `act\act_pseudodog_fur`, a material cutoff of
   128/255, and one preserved box-body recipe. It follows the barrel comparison
   trio; the complete configured scene has 14 BLASes, 22 instances, and 16
   geometries. The tail's
   mip-0 texels are all opaque, so the milestone proves mixed SBT routing and
   sampled-alpha any-hit execution; the leaf provides the visible cutout proof.
   The temporary code-owned tables
   supply placements until scene/level data has a real owner; world instances
   never become OGFx chunks.

   This ordering explicitly supersedes the earlier plan in which the Blender probe
   drove N-BLAS generalization and the older GEOMETRY_PLAN sequence that placed the
   opaque/alpha SBT split before any texture consumer. N-BLAS, the base-color
   texture consumer, and the mixed opaque/alpha-tested split are now landed. The
   runtime has four shader stages/groups, selects a hit record per flat geometry,
   sets BLAS opacity per range, evaluates texture alpha in any-hit, shares the
   build-owned `RayTypeCount = 1` routing ABI between C++ and Slang, and does not
   force rays opaque. Broader skeletal and physics source profiles still require
   explicit contracts; unsupported source semantics are rejected rather than
   hidden by a geometry-only conversion.
3. **Dynamic scene.** **Rigid dynamics landed; deformables pending** — the
   renderer foundation still uses one mapped instance input per `FrameResources`
   slot and a full in-place TLAS rebuild before every trace, with slot rotation and
   explicit pre/post-build barriers protecting the shared TLAS and scratch. Its
   transform producer is now a real engine-side physics system: vendored Jolt
   Physics v5.6.0, backend-neutral rigid recipes carried through `SceneData`, one
   RAII `PhysicsWorld`, static collision for every non-dynamic yard placement, and
   fixed-60-Hz live bodies for the generated crate plus configured regular barrel
   and pseudodog tail. Validated body-origin transforms are published atomically
   through the existing renderer seam; headless tests pin construction, lifecycle,
   settling/sleep, contracts, determinism, failure handling, and CCD. The
   orbit/spin policy has been removed.

   The remaining step-3 slice is deformable geometry: compute-pass skinning into
   per-slot vertex buffers followed by per-character BLAS refits for NPCs and
   mutants. It is separate from the completed rigid-body path.
4. **Lighting + path tracing.** Pending — the renderer becomes an actual path
   tracer: BRDF-based materials, an iterative bounce loop in raygen (keeping
   pipeline recursion depth at 1), next-event estimation with shadow rays,
   emissive geometry, and a sun/sky model for time-of-day. Many-light sampling
   is a first-class requirement, not a stress case — a campsite ringed by
   anomalies at night is the ordinary frame — so NEE lands with light-selection
   sampling from the start and a ReSTIR-class upgrade as the tracked follow-up.
5. **Temporal accumulation + denoising.** Pending — one coupled system, and the
   critical path for a playable image: at real-time budgets every visible pixel
   is denoiser output over ~1 sample per pixel. Motion vectors, temporal
   reprojection with disocclusion rejection, and an SVGF-class spatiotemporal
   filter — not offline-style progressive accumulation, which a moving camera
   and living scene rule out.

### Trigger-based engineering work

These changes are deliberately deferred until the design input that determines their
final shape exists:

- **Physics interpolation.** Revisit rendering between fixed poses when roadmap
  step 5 introduces previous-frame transforms and motion vectors. Until then the
  renderer consumes only poses that existed in the 60 Hz simulation and may repeat
  them on higher-refresh displays.
- **Presentation completion.** Do not add swapchain present fences unless
  `VK_KHR_swapchain_maintenance1` becomes part of the required baseline. At that point,
  replace the [documented teardown assumption](#presentation-teardown) with
  per-present fences.
- **UI/compositing surface usage.** Once that path is defined, reconsider the currently
  unused swapchain image views and `COLOR_ATTACHMENT` usage and capability gate
  together.
- **Slang import dependencies.** When shaders begin importing other source files, make
  Slang emit a dependency file and connect it to CMake's custom command. Depending
  directly on `raytrace.slang` is sufficient while it has no imports.

As roadmap work lands, update the [Status](#status) section, add a subsystem section,
and revise the ownership/synchronization sections if the new code changes those
invariants.
