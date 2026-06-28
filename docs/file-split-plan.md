# main.cpp Split — Plan

Split the single 1550-line [src/main.cpp](../src/main.cpp) into three units, extracting
the swapchain into its own owning type before ray-tracing code piles on. Plan only — no
implementation yet.

## Target shape

**Two owning RAII types** (each mirrors the existing `VulkanContext` pattern: plain fields,
deleted copy, a destructor that tears down in reverse order, manipulated by free
functions):

- **`VulkanContext`** — program-lifetime bring-up, created once:
  glfw/window, instance, debug messenger, surface, device, command pool, command buffer,
  `imageAvailableSemaphore`, `inFlightFence`.
- **`Swapchain`** — the recreate-on-resize cluster:
  a **non-owning** `VkDevice`, the swapchain, images, image views, format, extent, and
  `renderFinishedSemaphores` (per-image). Its destructor is what the teardown-dedup helpers
  (`destroySwapchainResources` / `destroyRenderFinishedSemaphores`) become.

The boundary is not arbitrary — it is exactly what `recreateSwapchain` already touches:

| Field | Recreated on resize? | Owner |
|---|---|---|
| swapchain, images, image views, format, extent | yes | `Swapchain` |
| `renderFinishedSemaphores` (per image) | yes | `Swapchain` |
| instance, surface, device, queues, command pool/buffer | no | `VulkanContext` |
| `imageAvailableSemaphore`, `inFlightFence` | no | `VulkanContext` |

**Three files:**

- `vulkan_context.{hpp,cpp}` — `VulkanContext`, `QueueFamilyIndices`, `RayTracingFunctions`,
  and bring-up helpers: layer/extension checks, debug messenger, `findQueueFamilies`,
  device-suitability + `pickPhysicalDevice`, `createLogicalDevice`, `createCommandPool`,
  `allocateCommandBuffer`, image-available/fence sync creation, `loadRayTracingFunctions`,
  `printVulkanVersion`.
- `swapchain.{hpp,cpp}` — the `Swapchain` type and its resource lifecycle.
  - **Public (`swapchain.hpp`):** `Swapchain`, `hasRequiredSwapchainSupport`,
    `createSwapchainResources`, `recreateSwapchain`.
  - **File-local (`swapchain.cpp` anon namespace):** `SwapchainSupportDetails`,
    `querySwapchainSupport`, the `choose*` helpers, `waitForDrawableFramebuffer` (only
    `recreateSwapchain` calls it), and the `RequiredSwapchainImageUsage` constant — none are
    referenced outside `swapchain.cpp`.
- `main.cpp` — orchestration + the event loop. Keeps `WindowWidth/Height/Title`, and — for
  now — `drawFrame` + `recordClearSwapchainImageCommandBuffer`. These two are the embryonic
  *renderer* (the code RT will rewrite — storage image, trace rays, blit), so they stay in
  the volatile orchestration layer rather than inside the stable swapchain module. They move
  to a dedicated `renderer.{hpp,cpp}` when ray-tracing resources land.

## Include graph (acyclic — verify before moving)

```
vulkan_context.hpp   includes <vulkan/vulkan.h> <GLFW/glfw3.h> <vector>
                     defines VulkanContext, QueueFamilyIndices, RayTracingFunctions
                     (does NOT reference Swapchain)

swapchain.hpp        includes <vulkan/vulkan.h> <GLFW/glfw3.h> <vector>
                     forward-declares  struct QueueFamilyIndices;   // create/recreate take it by const&
                     defines Swapchain

vulkan_context.cpp   includes vulkan_context.hpp + swapchain.hpp   // isPhysicalDeviceSuitable
                                                                   // calls hasRequiredSwapchainSupport
swapchain.cpp        includes swapchain.hpp + vulkan_context.hpp   // needs full QueueFamilyIndices
main.cpp             includes both                                 // drawFrame touches both types
```

No header includes another project header, so there is no cycle. The two cross-links both
resolve at the `.cpp` level:
- `isPhysicalDeviceSuitable` (context) calls `hasRequiredSwapchainSupport` (swapchain) →
  `vulkan_context.cpp` includes `swapchain.hpp`.
- swapchain create/recreate take `const QueueFamilyIndices&` (defined in
  `vulkan_context.hpp`) → `swapchain.hpp` only *forward-declares* the struct (a const-ref
  parameter needs no definition in a declaration); `swapchain.cpp` includes
  `vulkan_context.hpp` for the full type. `vulkan_context.hpp` never mentions `Swapchain`.

(With `drawFrame` staying in `main.cpp`, `swapchain.hpp` no longer needs to forward-declare
`VulkanContext` at all — `QueueFamilyIndices` is the only shared type left.)

## Phase 1 — Decompose ownership in place (still one file)

The only non-mechanical part. Do it inside `main.cpp` so it is easy to see and revert, and
confirm the red window still works before moving any files.

1. Add a `Swapchain` struct next to `VulkanContext`: the fields from the table above, a
   non-owning `VkDevice device`, deleted copy, and a destructor that — guarded on
   `device != VK_NULL_HANDLE` — **first calls `vkDeviceWaitIdle(device)`**, then destroys
   `renderFinishedSemaphores` → image views → swapchain. (See gotcha #1 — this wait is not
   optional once the swapchain owns its own teardown.)
2. Fold `destroySwapchainResources` + `destroyRenderFinishedSemaphores` into a single
   `destroySwapchainResources(Swapchain*)` (device is now a member). The `Swapchain`
   destructor and `recreateSwapchain` both call it — one teardown path for the whole type.
3. Remove the swapchain/views/format/extent/`renderFinishedSemaphores` fields and their
   teardown blocks from `VulkanContext`. Keep `imageAvailableSemaphore` + `inFlightFence`
   and `VulkanContext`'s existing `vkDeviceWaitIdle` (now redundant-but-harmless).
4. Replace the three granular create functions with a single `createSwapchainResources(Swapchain*)`
   (open decision resolved: fold) that, **in this exact order**, (a) sets `swap->device`
   *first*, before any child object, then (b) creates the swapchain, (c) the image views,
   (d) the render-finished semaphores. Setting `device` first is load-bearing: the destructor
   guards every teardown on `device`, so a partial failure after `vkCreateSwapchainKHR` only
   cleans up if `device` is already set. `recreateSwapchain` keeps its current step order:
   `waitForDrawableFramebuffer` → `vkDeviceWaitIdle` → `destroySwapchainResources` →
   `createSwapchainResources`. The framebuffer wait stays *first* so a closing/minimized
   window can early-return `VK_SUCCESS` before the device is idled.
5. Move `renderFinishedSemaphores` creation into `createSwapchainResources` (step 4d) so
   `Swapchain` fully owns its sync. Shrink `createFrameSyncObjects` to create only the
   image-available semaphore + fence into `VulkanContext`. (This also removes the current
   duplication where render-finished semaphores are created in two places — initial create
   vs. recreate.)
6. `drawFrame` (staying in `main.cpp`) takes `(VulkanContext&, Swapchain&, VkQueue trace,
   VkQueue present)` and indexes `swap.images[i]` / `swap.renderFinishedSemaphores[i]` /
   `swap.swapchain`. It is *not* part of the swapchain module — it is the seed of the future
   renderer.
7. In `main`: declare `VulkanContext ctx;` first, then `Swapchain swap;` **after** the device
   exists. (See destruction-order gotcha.)

Verify: builds clean, red window renders, resize still recreates, clean validation on exit.

## Phase 2 — Mechanical file moves

Ownership is now clean, so this is cut-paste + includes + a CMake edit. Behavior-preserving.

1. Create the four headers/sources per the include graph; move each cluster's declarations
   to the header and definitions to the `.cpp`.
2. `CMakeLists.txt`: add `src/vulkan_context.cpp` and `src/swapchain.cpp` to the
   `add_executable(xrPhoton ...)` list.
3. Anonymous-namespace handling (see gotcha #2).

Verify again: identical behavior.

## Gotchas

1. **`vkDeviceWaitIdle` moves into `Swapchain`'s destructor.** Today the wait is the first
   line of `VulkanContext::~VulkanContext()`. After the split, `swap` is declared second and
   so **destructs first** — its destructor would tear down the swapchain / image views /
   render-finished semaphores *before* the context's device-idle wait runs. At normal loop
   exit a frame can still be in flight (last submit/present), so that destroys in-use objects
   → validation error / UB. Fix: `Swapchain::~Swapchain()` calls `vkDeviceWaitIdle(device)`
   first (guarded). Keep `VulkanContext`'s wait too; the second wait is a free no-op and makes
   correctness independent of declaration order.
2. **Destruction order.** `Swapchain` must be destroyed before `VulkanContext`'s device *and*
   surface (the swapchain is a child of both). Locals destruct in reverse declaration order,
   so declaring `ctx` first and `swap` second makes `swap` die first — same reasoning as the
   current single destructor listing swapchain before device/surface. A default-constructed
   `Swapchain` (device == `VK_NULL_HANDLE`) destructs to a no-op, so early returns before
   swapchain creation stay safe.
3. **Headers can't use the anonymous namespace.** Everything currently lives in one anon
   namespace. Functions/types that cross files get real external linkage (declared in the
   header). Keep genuinely file-local helpers in an anon namespace *inside each `.cpp`*.
   Rule of thumb: if `main.cpp` or the other `.cpp` calls it → header; otherwise → file-local.
   Resolved open decision: wrap the project types/functions in `namespace xrphoton { }`.
4. **Non-owning `VkDevice` in `Swapchain`.** It borrows the device from `VulkanContext`; it
   must never destroy it. Only the swapchain-owned children are tied to that handle.

## Sequencing

Do Phase 1 as its own commit (the real change), then Phase 2 as one or more
behavior-preserving move commits. Land this before starting ray-tracing resources
(storage image, BLAS/TLAS, RT pipeline), which will otherwise grow `main.cpp` further.

## Resolved decisions

1. **`namespace xrphoton`** — yes, applied in Phase 2 (cheap, done once). See gotcha #3.
2. **Fold the swapchain create calls** — yes, into one `createSwapchainResources(Swapchain*)`.
   See Phase 1 step 4.
3. **`drawFrame` location** — stays in `main.cpp` for now (the embryonic renderer), not in
   the swapchain module; extracted to `renderer.{hpp,cpp}` when ray-tracing resources land.
