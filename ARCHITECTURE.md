# xrPhoton Architecture

This document describes how xrPhoton is put together: its modules, the lifetimes
and ownership of its resources, the per-frame flow, and the synchronization model.

## Status

xrPhoton renders its first ray traced pixels. It brings up a Vulkan instance and
device configured for hardware ray tracing, creates a swapchain, builds the ray
tracing **acceleration structures** (a BLAS over a hardcoded triangle and a
single-instance TLAS — see
[Acceleration structures](#acceleration-structures)), creates the **ray tracing
pipeline and shader binding table** (see
[Ray tracing pipeline](#ray-tracing-pipeline)), and runs a render loop in which
`vkCmdTraceRaysKHR` fires one ray per pixel into the TLAS, writing a device-local
**storage image** — the triangle in barycentric colors over a dark red miss
background — which is then blitted to the acquired swapchain image and presented.
The present path is fully wired (swapchain creation, per-frame synchronization,
resize handling including the descriptor rewrite the resize obligates), and every
piece of the RT stack is now exercised each frame. The next structural move is
extracting `drawFrame` and the record function into a `renderer.{hpp,cpp}` unit.

## Goals and constraints

- **Single executable.** No engine/runtime split, no plugin system. One process that
  opens a window and draws.
- **Hardware ray tracing as a hard requirement.** Device selection rejects any GPU
  that does not expose `VK_KHR_acceleration_structure` and
  `VK_KHR_ray_tracing_pipeline` (and their prerequisites). There is no raster
  fallback path.
- **Vulkan 1.3 baseline.** `RequiredApiVersion = VK_API_VERSION_1_3`; both the
  instance and the selected physical device must meet it.
- **RAII over manual cleanup.** Resource teardown lives in destructors, never in
  hand-unwound failure paths. See [Ownership model](#ownership-model).
- **No exceptions.** Errors propagate as `VkResult` / `bool` return values and are
  reported to `std::cerr`.
- **C++23, no compiler extensions** (`CMAKE_CXX_EXTENSIONS OFF`).

## Module map

The code is five translation units, all in `namespace xrphoton`. The split is along
**resource lifetime** (program-lifetime vs. recreated-on-resize) and **orchestration
vs. mechanism**.

```
        ┌───────────────────────────────────────────────────────────────────┐
        │ main.cpp                                                          │
        │   orchestration + the render loop                                 │
        │   drawFrame / recordTraceCommandBuffer / prepareRtForSwapchain    │
        └──────┬──────────────┬──────────────────┬──────────────────┬───────┘
               │ uses         │ uses             │ uses             │ uses
┌──────────────▼───────┐ ┌────▼───────────────┐ ┌▼────────────────┐ ┌▼──────────────┐
│ vulkan_context.{hpp, │ │ swapchain.{hpp,cpp}│ │ acceleration_   │ │ rt_pipeline.  │
│ cpp}                 │ │   Swapchain owner  │ │ structure.      │ │ {hpp,cpp}     │
│   VulkanContext owner│ │   create/recreate/ │ │ {hpp,cpp}       │ │   RtPipeline  │
│   instance, device,  │ │   support query    │ │   BLAS/TLAS     │ │   owner; SBT; │
│   queues, RT fns     │ │                    │ │   startup build │ │   descriptors │
└──────────────────────┘ └────────────────────┘ └─────────────────┘ └───────────────┘
```

| Unit | Owns / provides | Lifetime |
|------|-----------------|----------|
| [src/vulkan_context.hpp](src/vulkan_context.hpp) / [.cpp](src/vulkan_context.cpp) | `VulkanContext` (instance, debug messenger, surface, device, command pool/buffer, frame sync), `QueueFamilyIndices`, `RayTracingFunctions`, and the bring-up helpers (including the shared `findMemoryType` / `createBuffer`) | Program lifetime — created once |
| [src/swapchain.hpp](src/swapchain.hpp) / [.cpp](src/swapchain.cpp) | `Swapchain` (swapchain, images, image views, per-image render-finished semaphores, and the storage output image + its memory and view) and its create/recreate/query lifecycle | Recreated on resize |
| [src/acceleration_structure.hpp](src/acceleration_structure.hpp) / [.cpp](src/acceleration_structure.cpp) | `AccelerationStructure` (triangle geometry + instance buffers, BLAS/TLAS handles and backing buffers) and `buildAccelerationStructures` | Program lifetime — built once at startup |
| [src/rt_pipeline.hpp](src/rt_pipeline.hpp) / [.cpp](src/rt_pipeline.cpp) | `RtPipeline` (descriptor set layout/pool/set, pipeline layout, ray tracing pipeline, SBT buffer + the four trace regions), `createRtDescriptorSet`, `createRtPipeline`, `buildShaderBindingTable`, `writeRtDescriptorSet` | Program lifetime — created once at startup; the descriptor set is *rewritten* on resize |
| [src/main.cpp](src/main.cpp) | `main()` orchestration, `drawFrame`, `recordTraceCommandBuffer`, `prepareRtForSwapchain` | Program lifetime |

### Header dependency rule

Includes are kept acyclic by a deliberate rule:

- `swapchain.hpp` only **forward-declares** `QueueFamilyIndices`.
- `acceleration_structure.hpp` and `rt_pipeline.hpp` only **forward-declare**
  `RayTracingFunctions`.
- `vulkan_context.hpp` never mentions `Swapchain`, `AccelerationStructure`, or
  `RtPipeline`.

The genuine cross-links are resolved in the `.cpp`s, not the headers:

1. `isPhysicalDeviceSuitable` (in `vulkan_context.cpp`) calls
   `hasRequiredSwapchainSupport` (declared in `swapchain.hpp`).
2. The swapchain functions need the full definition of `QueueFamilyIndices`, which
   they get by including `vulkan_context.hpp` in `swapchain.cpp`.
3. `buildAccelerationStructures` needs the full `RayTracingFunctions` plus the shared
   `createBuffer` / `findMemoryType` helpers, which it gets by including
   `vulkan_context.hpp` in `acceleration_structure.cpp`.
4. `rt_pipeline.cpp` likewise includes `vulkan_context.hpp` for the full
   `RayTracingFunctions` and `createBuffer`; it additionally includes the
   build-generated `triangle_spv.h` (the embedded shader module — see
   [Ray tracing pipeline](#ray-tracing-pipeline)).

File-local helpers live in an anonymous namespace inside each `.cpp`; only the
cross-file surface is declared in the headers.

## Ownership model

Four RAII owners — split by resource lifetime:

- **`VulkanContext`** (program lifetime, created once) owns: the GLFW init flag, the
  window, the instance, the debug messenger, the surface, the device, the command
  pool, the command buffer, the image-available semaphore, and the in-flight fence.
- **`Swapchain`** (recreated on resize) owns: the swapchain, its images, its image
  views, the format/extent, the **per-image** render-finished semaphores, and the
  **storage image** (the trace output target — its `VkImage`, backing
  `VkDeviceMemory`, and `VkImageView`), which is sized to the swapchain extent and so
  rides the same recreate path. Its `VkDevice` is **non-owning** — borrowed from
  `VulkanContext` and used only to destroy the children above.
- **`AccelerationStructure`** (program lifetime, built once at startup) owns: the
  triangle vertex/index buffers, the instance buffer, the BLAS and TLAS handles with
  their backing buffers, and — transiently, during the build only — the scratch
  buffers. Like `Swapchain` it borrows its `VkDevice`; it additionally keeps the
  `vkDestroyAccelerationStructureKHR` pointer (a runtime-resolved extension entry
  point) so its destructor can tear down the two `VkAccelerationStructureKHR` handles
  without caller involvement.
- **`RtPipeline`** (program lifetime, created once at startup) owns: the descriptor
  set layout, the pipeline layout, the descriptor pool and the one descriptor set
  allocated from it (freed implicitly with the pool, but held as a member — both the
  render path and the rewrite-on-recreate path need the handle), the ray tracing
  pipeline, the SBT buffer + memory, the four `VkStridedDeviceAddressRegionKHR`s the
  trace consumes, and — transiently, during creation only — the shader module. Like
  the others it borrows its `VkDevice`. Its destructor needs no extension entry
  points (`vkDestroyPipeline` etc. are core), unlike `AccelerationStructure`.

Things that are neither created nor destroyed by the program (`physicalDevice`, the
`VkQueue` handles, the resolved `RayTracingFunctions`) stay as plain `main()` locals.

### Destruction order

In `main()` the `VulkanContext` is declared **first**, so every other owner destructs
before the device and surface it borrows from. This is the single most important
ordering invariant in the program, and it is what lets every failure path in `main()`
be a bare `return 1;` with no manual cleanup. Beyond "after `ctx`", the borrowing
owners (`Swapchain`, `AccelerationStructure`, `RtPipeline`) need no ordering
*relative to each other*: each waits for device idle in its own destructor rather
than relying on a sibling having done so.

Each destructor:

1. Calls `vkDeviceWaitIdle` first, so no in-flight work still references the
   resources about to be freed.
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
6. **Logical device.** One queue per unique {trace, present} family, with the ray
   tracing feature chain enabled.
7. **Ray tracing functions.** `loadRayTracingFunctions` resolves the RT entry points
   via `vkGetDeviceProcAddr`. The acceleration-structure subset is used by step 10,
   the pipeline subset by step 11, and `vkCmdTraceRaysKHR` by every frame.
8. **Swapchain.** `createSwapchainResources` — swapchain, image views, per-image
   render-finished semaphores, and the storage output image (created last, so it is
   torn down first).
9. **Command pool + buffer** (trace family) and **frame sync objects**
   (image-available semaphore + in-flight fence).
10. **Acceleration structures.** `buildAccelerationStructures` — see
    [Acceleration structures](#acceleration-structures). Borrows the frame command
    buffer and in-flight fence from step 9 and returns them in the state the first
    `drawFrame` expects.
11. **Ray tracing pipeline.** `createRtDescriptorSet` → `createRtPipeline` →
    `buildShaderBindingTable`, then the initial `prepareRtForSwapchain` (descriptor
    write + dispatch-limit gate) — see [Ray tracing pipeline](#ray-tracing-pipeline).
12. **Render loop.** `drawFrame` per iteration; recreate on out-of-date/suboptimal,
    followed by `prepareRtForSwapchain` against the fresh storage image.

### Device selection

`isPhysicalDeviceSuitable` is an aggregate test, ordered cheapest-first so the
short-circuiting `&&` skips expensive queries once a device has already failed:

```
queueFamilies.isComplete()           // a compute+graphics (trace) and a present family
  && hasRequiredApiVersion            // >= Vulkan 1.3
  && areRequiredDeviceExtensions...   // the RT stack + swapchain
  && hasRequiredSwapchainSupport      // format, present mode, usages, + storage/blit support
  && areRequiredRayTracingFeatures... // the features actually enabled
```

`hasRequiredSwapchainSupport` now covers the render path's format prerequisites, not
just raw swapchain support: beyond a usable format, present mode, and the required
image usages, it requires that the storage format
(`R8G8B8A8_UNORM`) supports the storage/transfer/blit-source features and that at
least one available surface format is **blit-compatible** as a blit destination.
This gates the storage→blit path at *device selection* so multi-GPU selection cannot
pick a device that passes the old checks and then fails later in `createStorageImage`.

`RequiredDeviceExtensions` (in `vulkan_context.cpp`) is the hardware ray tracing
stack — acceleration structure, ray tracing pipeline, buffer device address, deferred
host operations, pipeline library — plus `VK_KHR_swapchain` for presentation.

The ray tracing feature chain
(`VkPhysicalDeviceBufferDeviceAddressFeatures` → `RayTracingPipelineFeatures` →
`AccelerationStructureFeatures` → `VkPhysicalDeviceFeatures2`) is queried during
selection and re-used, with the same chain shape, to *enable* those features at
device creation.

### Queue families

`QueueFamilyIndices` tracks two roles:

- **`traceFamily`** — a family supporting **both compute and graphics**. Named
  "trace" because it is where ray tracing work will be recorded; today it records the
  clear and the blit. The single-command-buffer renderer needs one family for both
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

A single frame is in flight at a time. `drawFrame` in [src/main.cpp](src/main.cpp):

```
vkWaitForFences(inFlightFence)         // block until the previous frame completed
vkAcquireNextImageKHR                  // -> imageIndex, signals imageAvailableSemaphore
  ├─ OUT_OF_DATE        -> return (caller recreates the swapchain)
  └─ bounds-check imageIndex against the per-image vectors
vkResetCommandBuffer
recordTraceCommandBuffer               // see below
vkResetFences(inFlightFence)           // only now that a submit is guaranteed to follow
vkQueueSubmit(traceQueue)              // waits imageAvailable@TRANSFER, signals renderFinished[i], fence
vkQueuePresentKHR(presentQueue)        // waits renderFinished[i]
  └─ OUT_OF_DATE / SUBOPTIMAL -> return (caller recreates)
return acquireResult                   // surfaces a SUBOPTIMAL acquire to the caller
```

`recordTraceCommandBuffer` records a one-time-submit buffer with six steps — trace
into the storage image, then blit it into the acquired swapchain image:

1. Barrier the storage image `UNDEFINED → GENERAL` (`srcStageMask` `TOP_OF_PIPE`,
   destination `RAY_TRACING_SHADER`/`SHADER_WRITE`; `oldLayout` `UNDEFINED` discards
   prior contents — the whole image is overwritten). `GENERAL` is the layout
   storage-image writes require and must match what the descriptor set declared.
2. Bind the pipeline and descriptor set at `PIPELINE_BIND_POINT_RAY_TRACING_KHR`,
   then `vkCmdTraceRaysKHR` with the owner's four SBT regions and the swapchain
   extent — one ray per pixel. **No acceleration-structure barrier here**: the AS
   build's trailing barrier already made the TLAS visible to every future
   `RAY_TRACING_SHADER` read (see
   [Acceleration structures](#acceleration-structures)).
3. Barrier the storage image `GENERAL → TRANSFER_SRC_OPTIMAL` (shader-write →
   transfer-read), to be the blit source.
4. Barrier the acquired image `UNDEFINED → TRANSFER_DST_OPTIMAL`, the blit destination.
5. `vkCmdBlitImage` storage → swapchain (matching extents, `VK_FILTER_NEAREST`). A
   **blit**, not a copy, on purpose: blit does format conversion, so if the swapchain
   format is sRGB the storage `UNORM` value is gamma-encoded for presentation here.
6. Barrier the acquired image `TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR` for presentation.

This function is still the embryonic renderer: the next roadmap item extracts
`drawFrame` + `recordTraceCommandBuffer` into a `renderer.{hpp,cpp}` unit, leaving
`main.cpp` as orchestration only, and deletes the pipeline/descriptor plumbing
parameters `drawFrame` grew in the meantime.

## Synchronization model

The current model is intentionally minimal — one frame in flight — with three sync
primitives:

| Primitive | Count | Owner | Role |
|-----------|-------|-------|------|
| `imageAvailableSemaphore` | 1 | `VulkanContext` | Signaled by acquire; the submit waits on it at the `TRANSFER` stage. |
| `renderFinishedSemaphores[i]` | one **per swapchain image** | `Swapchain` | Signaled by the submit; the present waits on it. |
| `inFlightFence` | 1 | `VulkanContext` | Signaled when the submit completes; the next frame waits on it. Created **already signaled** so the first frame's wait does not deadlock. |

Two subtleties worth preserving:

- **Stage matching and pre-acquire overlap.** The acquire semaphore waits at
  `TRANSFER`, matching the first thing the submit does to the *swapchain* image: the
  blit-destination transition (step 4) and the blit itself (step 5), both `TRANSFER`
  work. So the swapchain transition cannot begin before the image is acquired. The
  trace (steps 1–2) runs at `RAY_TRACING_SHADER`, **outside** that wait stage, so the
  GPU is free to trace before — or overlapping — the acquire; only the blit onto the
  swapchain image is serialized behind it. This is the pre-acquire overlap this
  section predicted while the storage write was still a transfer clear; it is now
  real, with no submit-side change — the wait stage was chosen for this from the
  start. The present barrier's `dstStageMask` is `BOTTOM_OF_PIPE` because no later
  GPU stage consumes the image — the render-finished semaphore is what the present
  actually waits on.
- **Per-image, not per-frame, render-finished semaphores.** A present is signaled
  against the specific acquired image, so these are sized to the image count and
  indexed by `imageIndex`. The image-available semaphore and the fence are single
  because only one frame is in flight.

> **When this grows:** moving to N frames in flight means per-frame-in-flight
> image-available semaphores, command buffers, and fences (and the bookkeeping to
> rotate them). The render-finished semaphores stay per-image. This is the most likely
> first change to this section.

## Swapchain and resize handling

`Swapchain` is the unit that gets rebuilt when the surface changes. The trigger is a
`VK_ERROR_OUT_OF_DATE_KHR` or `VK_SUBOPTIMAL_KHR` from acquire or present, which
`drawFrame` returns and the render loop turns into a `recreateSwapchain` call.

`recreateSwapchain`:

1. `waitForDrawableFramebuffer` — block (pumping events) while the window has a
   zero-area framebuffer, e.g. while minimized. Returns false if the window is closing,
   in which case recreation is skipped.
2. `vkDeviceWaitIdle` — no in-flight frame may reference the old resources.
3. `destroySwapchainResources` then `createSwapchainResources` — the same teardown
   path the destructor uses, then a fresh build.

Selection policy inside `createSwapchain`:

- **Format:** restricted to formats the storage→swapchain blit can target; among those,
  prefer `B8G8R8A8_SRGB` + `SRGB_NONLINEAR`, otherwise the first blit-compatible one.
  An incompatible `formats[0]` is **never** used as a fallback — the suitability gate
  guarantees a compatible format exists.
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
because only one frame is in flight.

- **Format:** `R8G8B8A8_UNORM` — a member of Vulkan's guaranteed storage-image set,
  defined once as `StorageImageFormat` in `swapchain.cpp` so the suitability gate and
  the allocation use the exact same value.
- **Usage:** `STORAGE` (the traceRays write) `| TRANSFER_SRC` (blit source)
  `| TRANSFER_DST` (kept from the bring-up clear era; harmless and cheap).
  `TILING_OPTIMAL`, 1 mip / 1 layer, `EXCLUSIVE` sharing — unlike the swapchain
  images, it is only ever touched on the trace queue, so it needs no cross-family
  sharing.
- **Memory:** `findMemoryType` picks the first type matching the image's type bits and
  `DEVICE_LOCAL` (GPU-only); creation fails if none.
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

## Acceleration structures

The ray tracing scene: a **BLAS** built over one hardcoded triangle and a **TLAS**
whose single identity-transform instance references it. Built once by
`buildAccelerationStructures` after bring-up, before the render loop; the TLAS handle
is what the future RT descriptor set binds
(`VkWriteDescriptorSetAccelerationStructureKHR` takes the handle — an
acceleration-structure *device address* is only needed where an instance references a
BLAS). Everything is **swapchain-independent**: resize/recreate never touches it.

Decisions and contracts worth preserving:

- **Host-visible inputs, no staging.** The vertex/index/instance buffers are
  host-visible + coherent and written by `memcpy`. Deliberate: a staging pass would
  add an entire copy/submit path to move 60-odd bytes. Staging lands later, alongside
  real geometry loading. The backing and scratch buffers are device-local.
- **Device-address rules.** The build consumes buffer *device addresses*, not
  descriptors, so the input and scratch buffers carry
  `SHADER_DEVICE_ADDRESS` usage (and `createBuffer` derives the matching
  `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` on the allocation from that usage, so the
  two can never diverge). The **BLAS backing buffer** also needs the usage — querying
  a BLAS's acceleration-structure address for the TLAS instance requires it of the
  buffer underneath (VUID 09542). The **TLAS backing buffer** deliberately does not:
  nothing queries a TLAS address.
- **Scratch alignment.** The spec requires the scratch **device address** — not the
  buffer size or offset — to be a multiple of
  `minAccelerationStructureScratchOffsetAlignment`, and a buffer's base address
  carries no such guarantee. So the scratch is allocated with `alignment − 1` bytes
  of slack and the address is rounded up; the unaligned handles are kept for cleanup.
- **One submission, two barriers.** Both builds are recorded back-to-back into the
  frame command buffer (one-time-submit): BLAS build → `VkMemoryBarrier`
  (`ACCELERATION_STRUCTURE_BUILD` stage, AS-write → AS-read) → TLAS build → a final
  `VkMemoryBarrier` (AS-build write → `RAY_TRACING_SHADER` AS-read). The TLAS can be
  *recorded* against the BLAS before anything executes because an acceleration
  structure's device address is fixed at creation; the first barrier orders the
  *contents*. The trailing barrier exists because the fence only gives the **host**
  visibility of the build and submission order carries no memory dependency; a
  pipeline barrier's second scope covers all later commands in submission order on
  the queue, so it makes the TLAS visible to every future `vkCmdTraceRaysKHR`
  without the frame path needing its own barrier.
- **Borrowed sync.** The submit reuses `ctx.inFlightFence`: reset → submit → wait
  leaves the fence signaled, exactly the state the first `drawFrame`'s wait depends
  on, without introducing a temporary fence that could leak on a failure path.
- **Scratch release.** The scratch buffers live in the owner (not as locals) so a
  failed build bare-returns and the destructor cleans up; on success they are
  destroyed immediately after the fence wait rather than held for the program's
  lifetime.
- **Teardown.** `~AccelerationStructure` waits for device idle, destroys the TLAS and
  BLAS handles first (they are *placed on* their backing buffers), then the buffers
  and memory, all null-guarded.

## Ray tracing pipeline

The machinery that turns the TLAS into pixels: three shaders, the pipeline over
them, the shader binding table `vkCmdTraceRaysKHR` indexes into, and the descriptor
set binding the TLAS and the storage image. Owned by `RtPipeline`
([src/rt_pipeline.hpp](src/rt_pipeline.hpp)), created once at startup in three
steps (`createRtDescriptorSet` → `createRtPipeline` → `buildShaderBindingTable`),
program-lifetime except for one resize obligation described below.

Decisions and contracts worth preserving:

- **Shaders are Slang, embedded at build time.** All three stages live in one
  [shaders/triangle.slang](shaders/triangle.slang) module: `rayGenMain` (hardcoded
  orthographic camera — per-pixel NDC origin behind the triangle plane, direction
  +Z — storage-image write at binding 1, `[format("rgba8")]` because the device's
  `shaderStorageImageWriteWithoutFormat` is not enabled), `missMain` (the dark red
  the bring-up clear used), `closestHitMain` (barycentric-derived color, proving
  attribute interpolation and not just a hit). CMake compiles it with `slangc
  -target spirv -fvk-use-entrypoint-name -source-embed-style u32` into a
  self-contained C header (`triangle_spv.h`, includes prepended by the build) that
  `rt_pipeline.cpp` `#include`s — no runtime file paths, keeping the
  single-executable ethos. slangc is located via `find_program` (it is not a
  FindVulkan component).
- **One module, three stages.** Slang compiles every `[shader(...)]` entry point
  into a single SPIR-V module, so the pipeline creates one `VkShaderModule` and
  selects stages by `pName`. The module is parked in the owner during creation (the
  scratch-buffer pattern: a failure bare-returns and the destructor cleans up) and
  destroyed as soon as the pipeline exists.
- **Groups in SBT order.** Group 0 `GENERAL` (raygen), 1 `GENERAL` (miss), 2
  `TRIANGLES_HIT_GROUP` (closest hit only — the geometry is OPAQUE, so no any-hit;
  triangle intersection is fixed-function). Unused shader indices are set to
  `VK_SHADER_UNUSED_KHR` *explicitly* — zero-init would leave 0, a valid stage
  index, producing a silently wrong pipeline rather than a validation error.
  `maxPipelineRayRecursionDepth = 1` (primary rays only; 1 is the spec-guaranteed
  minimum, so no limit query).
- **SBT layout.** One host-visible + coherent buffer
  (`SHADER_BINDING_TABLE | SHADER_DEVICE_ADDRESS`), written once at startup — same
  no-staging rationale as the geometry buffers. Record stride =
  `shaderGroupHandleSize` rounded to `shaderGroupHandleAlignment`; each region
  starts at a `shaderGroupBaseAlignment` multiple from the table base. The
  alignment math uses a **general** round-up-to-multiple (not the AS build's
  bit-mask `alignUp`): the spec guarantees power-of-two for the handle alignment
  but describes `shaderGroupBaseAlignment` only as "required alignment". The buffer
  carries `baseAlignment − 1` bytes of slack, the base *device address* is rounded
  up (the VUIDs constrain region device addresses, not buffer offsets), and the CPU
  write pointer is shifted by the **same** delta — otherwise handles land at
  unaligned offsets while the regions point at aligned ones and the GPU reads
  garbage with no validation error. The raygen region's `size` equals its `stride`
  (spec requirement). The callable region is empty (`size = stride = 0`) but its
  `deviceAddress` points at the table base: the current VUID (03692)
  unconditionally requires a valid SBT-buffer address, and reusing the existing
  buffer satisfies the strict reading for free (the common `{0,0,0}` idiom relies
  on validation-layer leniency).
- **Descriptor set: binding 0 TLAS, binding 1 storage image, both raygen-only**
  (miss/closest-hit touch only the ray payload). The pool holds exactly the one
  set, without `FREE_DESCRIPTOR_SET_BIT` (the set is only released with the pool).
  The TLAS write chains `VkWriteDescriptorSetAccelerationStructureKHR` via `pNext`;
  the image write declares `IMAGE_LAYOUT_GENERAL`, which the frame's first barrier
  makes true before every trace.
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
  descriptor set layout → SBT buffer/memory, all null-guarded.

## Conventions

- Everything in `namespace xrphoton`; `main.cpp` pulls it in with `using namespace`.
- Free functions, `camelCase` names; `PascalCase` for constants and types.
- Cross-file functions are declared in the headers; file-private helpers live in an
  anonymous namespace inside each `.cpp`.
- Errors go to `std::cerr` and surface as `VkResult` / `bool`, never exceptions.
- Cleanup is RAII via the `VulkanContext` / `Swapchain` destructors, not manual
  unwinding in `main()`. Every `main()` failure path is a bare `return 1;`.
- Comments explain *why*, not *what*: decisions, contracts, and non-obvious Vulkan
  reasoning, not restatements of the code.

## Roadmap

The intended trajectory, in rough order. Each item is a section this document should
gain as it lands:

1. ~~**Storage image.**~~ ✅ **Landed** — see [Storage image](#storage-image). A
   device-local image the ray tracer writes, blitted to the swapchain image (replacing
   the direct clear), sized to the swapchain extent and recreated on resize.
2. ~~**Acceleration structures.**~~ ✅ **Landed** — see
   [Acceleration structures](#acceleration-structures). BLAS/TLAS build — geometry
   upload, build sizes, scratch buffers, and the device-address plumbing the RT
   pipeline needs.
3. ~~**Ray tracing pipeline + shader binding table.**~~ ✅ **Landed** — see
   [Ray tracing pipeline](#ray-tracing-pipeline). Ray generation / miss / hit
   shaders, the pipeline, and the SBT layout `vkCmdTraceRaysKHR` indexes into.
4. **Renderer extraction.** Now that the above has landed, `drawFrame` and
   `recordTraceCommandBuffer` move into a `renderer.{hpp,cpp}` unit; `main.cpp`
   becomes orchestration only and `drawFrame`'s plumbing parameters go away.
5. **Frames in flight.** Likely a parallel change — see the note in
   [Synchronization model](#synchronization-model).

As each item is built, update the [Status](#status) section, add a subsystem section,
and revise the ownership/synchronization sections if the new code changes those
invariants.
