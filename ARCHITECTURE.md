# xrPhoton Architecture

This document describes how xrPhoton is put together: its modules, the lifetimes
and ownership of its resources, the per-frame flow, and the synchronization model.

## Status

xrPhoton brings up a Vulkan instance and device configured for hardware ray
tracing, creates a swapchain, and runs a render loop that clears each frame to a
solid color and presents it. The present path is fully wired — swapchain creation,
per-frame synchronization, and resize handling all work. There is **no ray tracing
yet**: the ray tracing entry points are loaded but unused, and the frame is produced
by a `vkCmdClearColorImage` rather than by tracing rays. The clear-and-present path
is deliberately the seed of the eventual renderer.

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

The code is three translation units, all in `namespace xrphoton`. The split is along
**resource lifetime** (program-lifetime vs. recreated-on-resize) and **orchestration
vs. mechanism**.

```
                 ┌──────────────────────────────────────────────┐
                 │ main.cpp                                       │
                 │   orchestration + the render loop              │
                 │   drawFrame / recordClearSwapchainImageCommand │
                 └───────────────┬───────────────┬───────────────┘
                                 │ uses          │ uses
                 ┌───────────────▼──────┐ ┌──────▼─────────────────┐
                 │ vulkan_context.{hpp, │ │ swapchain.{hpp,cpp}    │
                 │ cpp}                 │ │   Swapchain owner       │
                 │   VulkanContext owner│ │   create / recreate /   │
                 │   instance, device,  │ │   support query         │
                 │   queues, RT fns     │ │                         │
                 └──────────────────────┘ └─────────────────────────┘
```

| Unit | Owns / provides | Lifetime |
|------|-----------------|----------|
| [src/vulkan_context.hpp](src/vulkan_context.hpp) / [.cpp](src/vulkan_context.cpp) | `VulkanContext` (instance, debug messenger, surface, device, command pool/buffer, frame sync), `QueueFamilyIndices`, `RayTracingFunctions`, and the bring-up helpers | Program lifetime — created once |
| [src/swapchain.hpp](src/swapchain.hpp) / [.cpp](src/swapchain.cpp) | `Swapchain` (swapchain, images, image views, per-image render-finished semaphores) and its create/recreate/query lifecycle | Recreated on resize |
| [src/main.cpp](src/main.cpp) | `main()` orchestration, `drawFrame`, `recordClearSwapchainImageCommandBuffer` | Program lifetime |

### Header dependency rule

Includes are kept acyclic by a deliberate rule:

- `swapchain.hpp` only **forward-declares** `QueueFamilyIndices`.
- `vulkan_context.hpp` never mentions `Swapchain`.

The two genuine cross-links are resolved in the `.cpp`s, not the headers:

1. `isPhysicalDeviceSuitable` (in `vulkan_context.cpp`) calls
   `hasRequiredSwapchainSupport` (declared in `swapchain.hpp`).
2. The swapchain functions need the full definition of `QueueFamilyIndices`, which
   they get by including `vulkan_context.hpp` in `swapchain.cpp`.

File-local helpers live in an anonymous namespace inside each `.cpp`; only the
cross-file surface is declared in the headers.

## Ownership model

Two RAII owners, split at the resize boundary:

- **`VulkanContext`** (program lifetime, created once) owns: the GLFW init flag, the
  window, the instance, the debug messenger, the surface, the device, the command
  pool, the command buffer, the image-available semaphore, and the in-flight fence.
- **`Swapchain`** (recreated on resize) owns: the swapchain, its images, its image
  views, the format/extent, and the **per-image** render-finished semaphores. Its
  `VkDevice` is **non-owning** — borrowed from `VulkanContext` and used only to
  destroy the children above.

Things that are neither created nor destroyed by the program (`physicalDevice`, the
`VkQueue` handles, the resolved `RayTracingFunctions`) stay as plain `main()` locals.

### Destruction order

In `main()` the `VulkanContext` is declared **first** and the `Swapchain` **second**,
so the `Swapchain` destructs first — before the device and surface it borrows from.
This is the single most important ordering invariant in the program, and it is what
lets every failure path in `main()` be a bare `return 1;` with no manual cleanup.

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
   `GLFW_NO_API`, initially **hidden** window (`GLFW_VISIBLE` false); it is shown only
   after the first frame presents, so it never flashes blank during bring-up.
2. **Instance.** Validation-layer and instance-extension availability checks. The
   instance extensions are GLFW's required surface set plus `VK_EXT_debug_utils`. The
   debug-messenger create info is chained via `pNext` on the instance create info, so
   validation also covers instance creation and destruction.
3. **Debug messenger.** Standalone `VK_EXT_debug_utils` messenger, filtered to
   warnings and errors, routing messages to `std::cerr`.
4. **Surface.** `glfwCreateWindowSurface`.
5. **Physical device.** `pickPhysicalDevice` takes the first GPU passing every
   suitability check (see [Device selection](#device-selection)).
6. **Logical device.** One queue per unique {trace, present} family, with the ray
   tracing feature chain enabled.
7. **Ray tracing functions.** `loadRayTracingFunctions` resolves the RT entry points
   via `vkGetDeviceProcAddr`. **Loaded, not yet used.**
8. **Swapchain.** `createSwapchainResources` — swapchain, image views, and per-image
   render-finished semaphores.
9. **Command pool + buffer** (trace family) and **frame sync objects**
   (image-available semaphore + in-flight fence).
10. **Render loop.** `drawFrame` per iteration; recreate on out-of-date/suboptimal.

### Device selection

`isPhysicalDeviceSuitable` is an aggregate test, ordered cheapest-first so the
short-circuiting `&&` skips expensive queries once a device has already failed:

```
queueFamilies.isComplete()           // a compute (trace) and a present family
  && hasRequiredApiVersion            // >= Vulkan 1.3
  && areRequiredDeviceExtensions...   // the RT stack + swapchain
  && hasRequiredSwapchainSupport      // a format, a present mode, required usages
  && areRequiredRayTracingFeatures... // the features actually enabled
```

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

- **`traceFamily`** — the first compute-capable family. Named "trace" because it is
  where ray tracing work will be recorded; today it records the clear.
- **`presentFamily`** — the first family that can present to the surface.

The two may resolve to the same family. The `has*` booleans distinguish "found family
index 0" from "no family found", since 0 is a valid index. When the families differ,
the swapchain images are created `VK_SHARING_MODE_CONCURRENT` across both; when they
coincide, `VK_SHARING_MODE_EXCLUSIVE` (valid and faster).

## The frame

A single frame is in flight at a time. `drawFrame` in [src/main.cpp](src/main.cpp):

```
vkWaitForFences(inFlightFence)         // block until the previous frame completed
vkAcquireNextImageKHR                  // -> imageIndex, signals imageAvailableSemaphore
  ├─ OUT_OF_DATE        -> return (caller recreates the swapchain)
  └─ bounds-check imageIndex against the per-image vectors
vkResetCommandBuffer
recordClearSwapchainImageCommandBuffer // see below
vkResetFences(inFlightFence)           // only now that a submit is guaranteed to follow
vkQueueSubmit(traceQueue)              // waits imageAvailable@TRANSFER, signals renderFinished[i], fence
vkQueuePresentKHR(presentQueue)        // waits renderFinished[i]
  └─ OUT_OF_DATE / SUBOPTIMAL -> return (caller recreates)
return acquireResult                   // surfaces a SUBOPTIMAL acquire to the caller
```

`recordClearSwapchainImageCommandBuffer` records a one-time-submit buffer with three
steps:

1. Barrier the acquired image `UNDEFINED → TRANSFER_DST_OPTIMAL`. `oldLayout`
   `UNDEFINED` discards prior contents (the whole image is overwritten).
2. `vkCmdClearColorImage` to a dark red.
3. Barrier `TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR` for presentation.

This function is the embryonic renderer. When ray tracing lands, the clear is
replaced by a `vkCmdTraceRaysKHR` into a storage image followed by a blit to the
swapchain image, and the natural move is to extract `drawFrame` +
`recordClear...` into a `renderer.{hpp,cpp}` unit, leaving `main.cpp` as
orchestration only.

## Synchronization model

The current model is intentionally minimal — one frame in flight — with three sync
primitives:

| Primitive | Count | Owner | Role |
|-----------|-------|-------|------|
| `imageAvailableSemaphore` | 1 | `VulkanContext` | Signaled by acquire; the submit waits on it at the `TRANSFER` stage. |
| `renderFinishedSemaphores[i]` | one **per swapchain image** | `Swapchain` | Signaled by the submit; the present waits on it. |
| `inFlightFence` | 1 | `VulkanContext` | Signaled when the submit completes; the next frame waits on it. Created **already signaled** so the first frame's wait does not deadlock. |

Two subtleties worth preserving:

- **Stage matching.** The acquire semaphore's wait stage and the first barrier's
  `srcStageMask` are both `TRANSFER`, so the layout transition cannot begin before the
  image is acquired. The present barrier's `dstStageMask` is `BOTTOM_OF_PIPE` because
  no later GPU stage consumes the image — the render-finished semaphore is what the
  present actually waits on.
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

- **Format:** prefer `B8G8R8A8_SRGB` + `SRGB_NONLINEAR`; otherwise the surface's first.
- **Present mode:** prefer `MAILBOX`; fall back to `FIFO` (always supported).
- **Extent:** the surface's `currentExtent` when fixed; otherwise the window
  framebuffer size clamped to the surface min/max.
- **Image count:** `minImageCount + 1` (so the app isn't forced to wait on the driver),
  clamped to `maxImageCount` when that is non-zero.
- **Composite alpha:** prefer `OPAQUE`, else the first supported mode.
- **Usage:** `TRANSFER_DST` (for the clear/blit) + `COLOR_ATTACHMENT` (reserved for
  later attachment-based rendering). A surface lacking either is rejected.

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

1. **Storage image.** A device-local image the ray tracer writes, blitted to the
   swapchain image (replacing the clear). Introduces an image to allocate, size to the
   swapchain extent, and recreate on resize.
2. **Acceleration structures.** BLAS/TLAS build — geometry upload, build sizes,
   scratch buffers, and the device-address plumbing the RT pipeline needs. The
   already-loaded `RayTracingFunctions` exist for exactly this.
3. **Ray tracing pipeline + shader binding table.** Ray generation / miss / hit
   shaders, the pipeline, and the SBT layout `vkCmdTraceRaysKHR` indexes into.
4. **Renderer extraction.** Once the above lands, `drawFrame` and the record function
   move into a `renderer.{hpp,cpp}` unit; `main.cpp` becomes orchestration only.
5. **Frames in flight.** Likely a parallel change — see the note in
   [Synchronization model](#synchronization-model).

As each item is built, update the [Status](#status) section, add a subsystem section,
and revise the ownership/synchronization sections if the new code changes those
invariants.
