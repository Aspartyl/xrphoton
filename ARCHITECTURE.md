# xrPhoton Architecture

This document describes how xrPhoton is put together: its modules, the lifetimes
and ownership of its resources, the per-frame flow, and the synchronization model.

## Status

xrPhoton renders its first ray traced pixels, and is interactive. It brings up a
Vulkan instance and device configured for hardware ray tracing, creates a
swapchain, builds the ray tracing **acceleration structures** (a BLAS over a
hardcoded triangle and a single-instance TLAS — see
[Acceleration structures](#acceleration-structures)), creates the **ray tracing
pipeline and shader binding table** (see
[Ray tracing pipeline](#ray-tracing-pipeline)), and runs a render loop in which
`vkCmdTraceRaysKHR` fires one ray per pixel into the TLAS from a **perspective
fly camera** (WASD + mouse look, delivered to the raygen shader via push
constants — see [Camera](#camera)), writing a device-local **storage image** —
the triangle in barycentric colors over a dark red miss background — which is
then blitted to the acquired swapchain image and presented. The present path is
fully wired (swapchain creation, two-frame-in-flight synchronization, resize
handling including the descriptor rewrite the resize obligates), and every piece
of the RT stack is now exercised each frame. The frame path lives in its own
`renderer.{hpp,cpp}` unit; `main.cpp` is orchestration only. The tracked
bring-up roadmap is complete; of the follow-on roadmap, camera + push constants
has landed.

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

The code is seven translation units, all in `namespace xrphoton`. The split is along
**resource lifetime** (program-lifetime vs. recreated-on-resize) and **orchestration
vs. mechanism**.

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
│   instance, device,  │ │   support query    │ │   BLAS/TLAS     │ │   owner; SBT; │
│   queues, RT fns     │ │                    │ │   startup build │ │   descriptors │
└──────────────────────┘ └────────────────────┘ └─────────────────┘ └───────────────┘
```

(`main.cpp` also uses the four resource units directly for bring-up, and drives
`camera.{hpp,cpp}` — the Vulkan-free input/math unit whose `CameraPushConstants`
payload main() builds each frame and passes through `drawFrame` into the raygen
stage; the diagram shows the frame-path layering.)

| Unit | Owns / provides | Lifetime |
|------|-----------------|----------|
| [src/vulkan_context.hpp](src/vulkan_context.hpp) / [.cpp](src/vulkan_context.cpp) | `VulkanContext` (instance, debug messenger, surface, device, command pool, per-frame command buffers and sync), `FrameResources`, `QueueFamilyIndices`, `RayTracingFunctions`, and the bring-up helpers (including the shared `findMemoryType` / `createBuffer`) | Program lifetime — created once |
| [src/swapchain.hpp](src/swapchain.hpp) / [.cpp](src/swapchain.cpp) | `Swapchain` (swapchain, images, image views, per-image render-finished semaphores, and the storage output image + its memory and view) and its create/recreate/query lifecycle | Recreated on resize |
| [src/acceleration_structure.hpp](src/acceleration_structure.hpp) / [.cpp](src/acceleration_structure.cpp) | `AccelerationStructure` (triangle geometry + instance buffers, BLAS/TLAS handles and backing buffers) and `buildAccelerationStructures` | Program lifetime — built once at startup |
| [src/camera.hpp](src/camera.hpp) / [.cpp](src/camera.cpp) | `Vec3`, `Camera` (fly-camera state: position, yaw/pitch, FOV, cursor anchor), `CameraPushConstants` (the raygen push payload + its ABI asserts), `updateCamera` (all GLFW input policy), `makeCameraPushConstants` | Plain value state owned by `main()` — no Vulkan objects |
| [src/rt_pipeline.hpp](src/rt_pipeline.hpp) / [.cpp](src/rt_pipeline.cpp) | `RtPipeline` (descriptor set layout/pool/set, pipeline layout with the camera push-constant range, ray tracing pipeline, SBT buffer + the four trace regions), `createRtDescriptorSet`, `createRtPipeline`, `buildShaderBindingTable`, `writeRtDescriptorSet` | Program lifetime — created once at startup; the descriptor set is *rewritten* on resize |
| [src/renderer.hpp](src/renderer.hpp) / [.cpp](src/renderer.cpp) | `Renderer` (the non-owning view of everything the frame path uses), `drawFrame`, `prepareRtForSwapchain`, and the file-private `recordTraceCommandBuffer` / `recordImageBarrier` / `recordExecutionBarrier` | Owns nothing — a parameter bundle over borrowed handles |
| [src/main.cpp](src/main.cpp) | `main()` orchestration + the render loop | Program lifetime |

### Header dependency rule

Includes are kept acyclic by a deliberate rule:

- `swapchain.hpp` only **forward-declares** `QueueFamilyIndices`.
- `acceleration_structure.hpp` and `rt_pipeline.hpp` only **forward-declare**
  `RayTracingFunctions`.
- `renderer.hpp` only **forward-declares** `CameraPushConstants`,
  `FrameResources`, `RayTracingFunctions`, `RtPipeline`, and `Swapchain`; it
  never mentions `VulkanContext` — the renderer borrows specific handles, not the
  context, so the unit is decoupled from bring-up entirely.
- `vulkan_context.hpp` never mentions `Swapchain`, `AccelerationStructure`,
  `RtPipeline`, or `Renderer`.
- `camera.hpp` includes **no project or Vulkan header at all** (only `<cstddef>`
  for its `offsetof` ABI asserts) and forward-declares `GLFWwindow`;
  `makeCameraPushConstants` takes a plain `float aspect` rather than a
  `VkExtent2D` precisely to keep the unit Vulkan-free.

The genuine cross-links are resolved in the `.cpp`s, not the headers:

1. `isPhysicalDeviceSuitable` (in `vulkan_context.cpp`) calls
   `hasRequiredSwapchainSupport` (declared in `swapchain.hpp`) and
   `hasRequiredAccelerationStructureFormatSupport` (declared in
   `acceleration_structure.hpp`).
2. The swapchain functions need the full definition of `QueueFamilyIndices`, which
   they get by including `vulkan_context.hpp` in `swapchain.cpp`.
3. `buildAccelerationStructures` needs the full `RayTracingFunctions` plus the shared
   `createBuffer` / `findMemoryType` helpers, which it gets by including
   `vulkan_context.hpp` in `acceleration_structure.cpp`.
4. `rt_pipeline.cpp` likewise includes `vulkan_context.hpp` for the full
   `RayTracingFunctions` and `createBuffer`; it additionally includes the
   build-generated `triangle_spv.h` (the embedded shader module — see
   [Ray tracing pipeline](#ray-tracing-pipeline)).
5. `renderer.cpp` includes `camera.hpp`, `rt_pipeline.hpp`, `swapchain.hpp`, and
   `vulkan_context.hpp` to resolve the borrowed structs its header only
   forward-declares.
6. `rt_pipeline.cpp` includes `camera.hpp` for `sizeof(CameraPushConstants)` —
   the pipeline layout's push-constant range; `camera.cpp` includes
   `GLFW/glfw3.h` for the real input API its header only forward-declared.

File-local helpers live in an anonymous namespace inside each `.cpp`; only the
cross-file surface is declared in the headers.

## Ownership model

Four RAII owners — split by resource lifetime:

- **`VulkanContext`** (program lifetime, created once) owns: the GLFW init flag, the
  window, the instance, the debug messenger, the surface, the device, the command
  pool, and the `frames` array. Each `FrameResources` slot owns one command buffer,
  one image-available semaphore, and one in-flight fence.
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

`Renderer` is deliberately **not** a fifth owner: it is a parameter bundle over
borrowed handles (in the spirit of `QueueFamilyIndices`), with no destructor and no
idle wait. Its handle members are copies of program-lifetime objects; `Swapchain` is
held by pointer because its members are replaced on every recreate, and the
`FrameResources` array is borrowed by pointer because `VulkanContext` owns it for the
program lifetime. Keeping the frame slots in `VulkanContext` lets the acceleration
structure build borrow `frames[0]` before the RT pipeline and `Renderer` exist,
without forcing two-phase renderer initialization. `main()` creates the renderer
view after everything it points at, so it cannot outlive what it borrows.

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
9. **Command pool + frame resources** (trace family): one primary command buffer,
   image-available semaphore, and in-flight fence per frame slot.
10. **Acceleration structures.** `buildAccelerationStructures` — see
    [Acceleration structures](#acceleration-structures). Borrows `frames[0]`'s
    command buffer and in-flight fence from step 9 and returns them in the state the
    first `drawFrame` expects; the other frame slots remain signaled and untouched.
11. **Ray tracing pipeline.** `createRtDescriptorSet` → `createRtPipeline` →
    `buildShaderBindingTable` — see [Ray tracing pipeline](#ray-tracing-pipeline).
12. **Renderer view.** The `Renderer` bundle is populated — last, once every handle
    it borrows exists, including `ctx.frames.data()` — then the initial
    `prepareRtForSwapchain` (descriptor write + dispatch-limit gate) runs against it.
13. **Render loop.** `drawFrame(renderer, currentFrame)` per iteration, rotating
    `currentFrame` modulo `MaxFramesInFlight`; recreate on out-of-date/suboptimal,
    followed by `prepareRtForSwapchain` against the fresh storage image.

### Device selection

`isPhysicalDeviceSuitable` is an aggregate test, ordered cheapest-first so the
short-circuiting `&&` skips expensive queries once a device has already failed:

```
queueFamilies.isComplete()           // a compute+graphics (trace) and a present family
  && hasRequiredApiVersion            // >= Vulkan 1.3
  && areRequiredDeviceExtensions...   // the RT stack + swapchain
  && hasRequiredSwapchainSupport      // format, present mode, usages, + storage/blit support
  && hasRequiredAccelerationStruct... // BLAS vertex format backstop (spec-mandated support)
  && areRequiredRayTracingFeatures... // the features actually enabled
```

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
device address** is core in the 1.3 baseline (its feature is enabled through the
core `VkPhysicalDeviceBufferDeviceAddressFeatures` struct, and
`vkGetBufferDeviceAddress` is resolved by its core name — a 1.3 driver need not
still advertise the promoted KHR extension string), and **pipeline library** is
only an optional interaction of the RT pipeline extension, never used here.

`hasRequiredAccelerationStructureFormatSupport` (in `acceleration_structure.cpp`,
next to the format it checks) verifies the BLAS vertex format supports acceleration
structure builds. The spec mandates that support wherever the feature exists, so
this is a conformance backstop in the "check anyway, fail loudly" family (like the
trace dispatch gate), not a real capability query.

The ray tracing feature chain
(`VkPhysicalDeviceBufferDeviceAddressFeatures` → `RayTracingPipelineFeatures` →
`AccelerationStructureFeatures` → `VkPhysicalDeviceFeatures2`) is queried during
selection and re-used, with the same chain shape, to *enable* those features at
device creation.

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
computes a clamped delta time (`MaxFrameDt` = 0.1 s — window drags and resize
stalls can block the loop for seconds, and an unclamped dt would teleport the
camera), calls `updateCamera`, and derives the frame's `CameraPushConstants` from
the camera state and the current `swap.extent` aspect ratio (read fresh every
iteration, so a recreate needs no camera-specific handling). `main()` owns a
`currentFrame` cursor and rotates it after every
`drawFrame(renderer, currentFrame, cameraPush)` call; each slot has its own
command buffer, image-available semaphore, and in-flight fence. `drawFrame` in
[src/renderer.cpp](src/renderer.cpp) reaches everything through the `Renderer`
view (the camera payload rides as a parameter, not a `Renderer` member — it is
per-frame data, not a program-lifetime handle):

```
frame = frames[frameIndex]
vkWaitForFences(frame.inFlightFence)   // block until this slot's prior submit retired
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

`recordTraceCommandBuffer` records a one-time-submit buffer with seven steps — trace
into the storage image, then blit it into the acquired swapchain image:

1. Barrier the storage image `UNDEFINED → GENERAL` (`srcStageMask`
   `RAY_TRACING_SHADER`, destination `RAY_TRACING_SHADER`/`SHADER_WRITE`;
   `oldLayout` `UNDEFINED` discards prior contents — the whole image is
   overwritten). `GENERAL` is the layout storage-image writes require and must match
   what the descriptor set declared. The source stage chains from the previous
   frame's trailing storage-image barrier without involving the acquire wait's
   `TRANSFER` stage.
2. Bind the pipeline and descriptor set at `PIPELINE_BIND_POINT_RAY_TRACING_KHR`,
   push the frame's `CameraPushConstants` (`vkCmdPushConstants`, raygen-only —
   recorded into this slot's own command buffer after its fence wait, which is
   what makes the camera race-free across frames in flight by construction),
   then `vkCmdTraceRaysKHR` with the owner's four SBT regions and the swapchain
   extent — one ray per pixel. **No acceleration-structure barrier here**: the AS
   build's trailing barrier already made the TLAS visible to every future
   `RAY_TRACING_SHADER` read (see
   [Acceleration structures](#acceleration-structures)).
3. Barrier the storage image `GENERAL → TRANSFER_SRC_OPTIMAL` (shader-write →
   transfer-read), to be the blit source.
4. Barrier the acquired image `UNDEFINED → TRANSFER_DST_OPTIMAL`, the blit destination.
5. `vkCmdBlitImage` storage → swapchain (matching extents, `VK_FILTER_NEAREST`). A
   **blit**, not a copy, on purpose: the selected swapchain format is always sRGB, so
   format conversion gamma-encodes the storage `UNORM` value for presentation here.
6. Execution-only barrier `TRANSFER → RAY_TRACING_SHADER`, so a later frame cannot
   overwrite the shared storage image until this frame's blit has finished reading
   it. No memory dependency is needed for this write-after-read hazard; only ordering
   matters.
7. Barrier the acquired image `TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR` for presentation.

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
- **Borrowed sync.** The submit reuses `frames[0]`'s in-flight fence: reset →
  submit → wait leaves the fence signaled, exactly the state the first `drawFrame`'s
  wait depends on, without introducing a temporary fence that could leak on a
  failure path. The other frame slots are untouched (their fences are created
  signaled).
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
  [shaders/triangle.slang](shaders/triangle.slang) module: `rayGenMain`
  (perspective rays from the camera push constants — see [Camera](#camera) for
  the payload contract — storage-image write at binding 1, `[format("rgba8")]`
  because the device's `shaderStorageImageWriteWithoutFormat` is not enabled),
  `missMain` (the dark red the bring-up clear used), `closestHitMain`
  (barycentric-derived color, proving attribute interpolation and not just a
  hit). CMake compiles it with `slangc
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
  descriptor set layout → SBT buffer/memory, all null-guarded.

## Camera

A perspective fly camera, delivered to the raygen shader via push constants.
Owned by `main()` as a plain value struct (`Camera` in
[src/camera.hpp](src/camera.hpp)) — no Vulkan objects, no RAII; the unit is pure
input + math, and its header is Vulkan-free (see the header dependency rule).

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
- **No math library (yet).** `Vec3` plus a handful of file-private helpers in
  `camera.cpp` is everything this step needs; GLM vs. growing the in-house math
  is a geometry/scene-time decision (instance transforms), to be made with real
  requirements in hand. The `normalize` helper returns zero for near-zero input,
  and the movement path additionally skips near-zero sums — `normalize({0,0,0})`
  is the classic NaN that permanently poisons the position.
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
  camera is fully frozen while the cursor is free. WASD moves along the look
  direction (the unscaled basis, never the FOV-scaled push vectors),
  Space/LeftCtrl along ±world-up, LeftShift sprints; the summed direction is
  normalized so diagonals are not faster. Mouse look polls `glfwGetCursorPos`
  deltas against an anchor stored in `Camera`; the anchor is invalidated on
  every capture transition and re-anchored one frame before deltas apply —
  otherwise the first captured frame integrates the whole cursor jump as one
  giant rotation.
- **Frame timing lives in `main()`.** `glfwGetTime()` deltas, clamped to
  `MaxFrameDt` (0.1 s) so window drags and resize stalls cannot teleport the
  camera. A real loop with fixed-timestep simulation is deferred to the dynamic
  scene step.

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
- Errors go to `std::cerr` and surface as `VkResult` / `bool`, never exceptions.
- Cleanup is RAII via the `VulkanContext` / `Swapchain` destructors, not manual
  unwinding in `main()`. Every `main()` failure path is a bare `return 1;`.
- Comments explain *why*, not *what*: decisions, contracts, and non-obvious Vulkan
  reasoning, not restatements of the code.

## Roadmap

1. **Camera + push constants.** **Landed** — a perspective fly camera (origin +
   pre-scaled ray basis) delivered via raygen-only push constants, with GLFW
   fly controls, fixing the bring-up aspect-ratio distortion on resize. See
   [Camera](#camera) for the decisions and contracts.
2. **Geometry + scene representation.** Pending — **the next step to build**.
   Real meshes replacing the
   hardcoded triangle: indexed vertex data with per-vertex attributes (normals,
   UVs) fetched in the closest-hit shader via buffer device addresses, multiple
   BLASes with instance transforms, and material data in storage buffers indexed
   per instance/geometry. Designed from the start around the split between opaque
   and alpha-tested geometry classes (separate hit groups and SBT entries):
   foliage-heavy STALKER scenes make any-hit alpha testing the engine's single
   biggest traversal cost lever, and the current `FORCE_OPAQUE` trace flag is
   temporary. Implies the first asset-loading path (a simple interchange format
   first; the X-Ray-content conversion pipeline is a separate later concern).
3. **Dynamic scene.** Pending — the scene starts moving, in two tiers. First
   rigid dynamics: per-frame TLAS refit/rebuild from CPU-written instance
   buffers, one per `FrameResources` slot (the first genuinely per-frame-written
   GPU buffer — slot rotation is what prevents overwriting instance data a frame
   in flight still reads). Then deformables: compute-pass skinning into per-slot
   vertex buffers followed by per-character BLAS refits, for NPCs and mutants.
   Also the natural point to grow the loop's timing (the fly camera added
   simple clamped delta-time; fixed-timestep simulation can wait for game
   systems).
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

As each item is built, update the [Status](#status) section, add a subsystem section,
and revise the ownership/synchronization sections if the new code changes those
invariants.
