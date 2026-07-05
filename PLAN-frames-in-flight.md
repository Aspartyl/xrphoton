# Plan: Frames in flight (roadmap step 5)

## What changes conceptually

Today the in-flight fence wait at the top of `drawFrame` fully serializes frames:
the GPU finishes frame N before the CPU even records frame N+1. Moving to
`MaxFramesInFlight = 2` lets the CPU record frame N+1 while the GPU still executes
frame N. Per the "When this grows" note in
[ARCHITECTURE.md](ARCHITECTURE.md#synchronization-model): the command buffer,
image-available semaphore, and in-flight fence become **per-frame-in-flight**; the
render-finished semaphores **stay per-image** in `Swapchain`.

This is not just "duplicate the sync objects" — going from 1 to N frames in flight
introduces a real GPU-side hazard on the shared storage image that the current
barriers don't cover (step 4 below is the correctness core of the change).

## 1. Per-frame resources in `VulkanContext`

- Add `constexpr uint32_t MaxFramesInFlight = 2;` to
  [src/vulkan_context.hpp](src/vulkan_context.hpp) (next to `RequiredApiVersion`).
- Group the trio into a small struct so it rotates as a unit:

  ```cpp
  struct FrameResources
  {
      VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
      VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
      VkFence inFlightFence = VK_NULL_HANDLE;
  };
  ```

- `VulkanContext` replaces its three scalar members with
  `std::array<FrameResources, MaxFramesInFlight> frames;`. The destructor tears
  down each element with the existing null-guard pattern (partial bring-up still
  cleans up).
- `allocateCommandBuffer` becomes an N-buffer allocation: one
  `vkAllocateCommandBuffers` call with `commandBufferCount = MaxFramesInFlight`.
  **Trap:** the call writes N handles contiguously, and `frames` is an array of
  structs — `&frames[0].commandBuffer` strides through the semaphore/fence
  members and would corrupt them. Allocate into a temporary
  `std::array<VkCommandBuffer, MaxFramesInFlight>`, then scatter
  `commandBuffers[i]` into `frames[i].commandBuffer`. (Same in the destructor:
  either free one buffer per frame in the teardown loop, or gather the handles
  into a temporary array before a batched `vkFreeCommandBuffers`.)
- `createFrameSyncObjects` loops, creating each fence **signaled** as today. It
  should write into the `frames` array in place (take
  `std::array<FrameResources, MaxFramesInFlight>*` instead of today's two
  out-pointers), so a mid-loop failure leaves the created handles where the
  `VulkanContext` destructor already cleans them up — replacing the function's
  current destroy-on-failure unwinding (which only worked because the caller's
  handles stayed null) with the `createBuffer` convention.

## 2. Ownership decision deferred in ARCHITECTURE.md ("Ownership and lifetime")

Resolve it by **keeping the per-frame objects in `VulkanContext`**, not moving
them into `Renderer`. Rationale: the AS build still borrows a command buffer +
fence before the RT pipeline exists, so renderer ownership would force the
two-phase initialization the doc warns about. `Renderer` stays a non-owning
parameter bundle — replace its three scalar members with
`const FrameResources* frames = nullptr;`, with `FrameResources`
forward-declared in `renderer.hpp` like the other borrowed structs (a by-value
`std::array` would need the complete type and `MaxFramesInFlight`, forcing
`renderer.hpp` to include `vulkan_context.hpp` — against the header rule).
`main()` passes `ctx.frames.data()`; `renderer.cpp` already includes
`vulkan_context.hpp` for the complete type. The frame cursor lives in `main()` (a
`uint32_t currentFrame`), passed to `drawFrame(renderer, currentFrame)`; main
advances it `(currentFrame + 1) % MaxFramesInFlight` after each loop iteration.
Advancing even on a no-submit iteration (OUT_OF_DATE at acquire) is safe because
a frame that didn't submit leaves its fence signaled.

## 3. `drawFrame` rotation

- Signature: `VkResult drawFrame(const Renderer& renderer, uint32_t frameIndex);`
- Waits `frames[frameIndex].inFlightFence`, acquires with
  `frames[frameIndex].imageAvailableSemaphore`, records/submits
  `frames[frameIndex].commandBuffer`, signals that fence. The
  wait-fence-then-reuse-semaphore ordering makes semaphore reuse safe (the
  submission that waited on it has provably retired). Everything else — the
  deferred fence reset, per-image `renderFinishedSemaphores[imageIndex]`, the
  SUBOPTIMAL propagation — is unchanged.

## 4. The parallel-change hazard: storage-image WAR across frames

Today the storage image's first barrier in
[src/renderer.cpp](src/renderer.cpp) (`recordTraceCommandBuffer`, step 1) is
`UNDEFINED → GENERAL` with `srcStageMask = TOP_OF_PIPE`, `srcAccessMask = 0` —
correct only because the fence guarantees the previous frame fully retired
before the next one is submitted. With two frames in flight, frame N+1's layout
transition (a write) can begin while frame N's **blit is still reading** the
same storage image.

The naive fix — `srcStageMask = TRANSFER` on that leading barrier — is correct
but has a hidden cost: the submit waits the image-available semaphore at
`TRANSFER`, and a semaphore wait chains with any subsequent barrier whose
`srcStageMask` intersects the wait stages. Acquire → storage transition → trace
would become a dependency chain, serializing the trace behind its own frame's
acquire and destroying the pre-acquire trace overlap the sync model was built
around.

Fix that preserves the overlap — put the cross-frame dependency at the **end**
of the frame, outside the acquire wait's stage, and chain the next frame off a
non-`TRANSFER` stage:

- **Trailing barrier** (new, after the blit in `recordTraceCommandBuffer`): a
  pure execution dependency `srcStageMask = TRANSFER` →
  `dstStageMask = RAY_TRACING_SHADER`, no memory barriers (a write-after-read
  hazard needs only execution ordering). Within frame N this is ordered after
  the blit anyway, so chaining behind frame N's acquire costs nothing; its
  second scope covers all later `RAY_TRACING_SHADER` work in queue order —
  including frame N+1's trace.
- **Leading barrier** (changed): `UNDEFINED → GENERAL` with
  `srcStageMask = RAY_TRACING_SHADER` instead of `TOP_OF_PIPE`. This chains off
  the previous frame's trailing barrier (its `dstStageMask`), completing the
  chain blit N → trailing barrier → leading transition N+1 → trace N+1, while
  staying disjoint from the `TRANSFER` acquire-wait stage — so trace N+1 still
  runs pre-acquire. `UNDEFINED` still discards contents; `srcAccessMask` stays 0.

Alternatives, noted in the doc but not taken: `srcStageMask = TRANSFER` on the
leading barrier alone (simpler, but serializes trace behind acquire); per-frame
storage images plus per-frame descriptor sets (full cross-frame trace overlap,
not warranted for one triangle); split trace/blit submits.

## 5. AS build borrow

`buildAccelerationStructures` borrows `frames[0].commandBuffer` /
`frames[0].inFlightFence` and returns them signaled/resettable as today; frames
1..N-1 were created signaled and untouched, so the first N `drawFrame` calls all
find their fences in the expected state. No change inside
`acceleration_structure.cpp` — only the call site in `main.cpp`.

## 6. Unchanged, verify only

- `recreateSwapchain`'s `vkDeviceWaitIdle` still makes the
  `prepareRtForSwapchain` descriptor rewrite race-free with N frames in flight —
  no change.
- Per-image render-finished semaphores: already correct for this model.
- Command pool already has per-buffer reset.

## 7. Documentation

- ARCHITECTURE.md: rewrite the sync-primitive table (counts become "one per
  frame in flight"), replace the "When this grows" callout with the realized
  model + the new trailing/leading barrier pair and why its stages were chosen
  (the acquire-wait chaining subtlety above), resolve the ownership question in
  the Ownership model section **and** update its `VulkanContext` bullet (the
  scalar command buffer / semaphore / fence become the `frames` array), update
  the Status section ("the next structural move is frames in flight"), the
  frame-flow diagram, and bring-up steps 9/10/12/13 (step 10's "borrows the
  frame command buffer and in-flight fence" becomes `frames[0]`'s), and mark
  roadmap step 5 done.
- **The roadmap ends at step 5** — there is no step 6. Landing this leaves the
  tracked roadmap complete, so either add the next intended item to the roadmap
  (user's call — e.g. camera/uniforms, more geometry) or explicitly note the
  roadmap as complete; CLAUDE.md's "Next step" section mirrors whichever is
  chosen.
- CLAUDE.md: update the summary paragraph (it repeats the Status text) and the
  module-map blurbs that mention the per-frame objects.
- Comment updates in `renderer.hpp` ("a single frame in flight" doc comment) and
  `vulkan_context.hpp`.

## 8. Verification

Build, run, confirm the triangle renders; resize/minimize to exercise the
recreate path. The default validation layer catches semaphore/fence misuse but
**not** execution/memory hazards — synchronization validation is a separate
layer setting. Run at least once with it enabled — simplest:
`VK_VALIDATION_VALIDATE_SYNC=1 ./build/xrPhoton` (see the layer's
[syncval usage doc](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/syncval_usage.md));
`vkconfig` or a `vk_layer_settings.txt` with
`khronos_validation.validate_sync = true` pointed at by
`VK_LAYER_SETTINGS_PATH` work too — to check the barrier changes; expect it to
flag the storage-image WAR if the trailing barrier were omitted, and silence
with it in place. A quick sanity check that quitting mid-flight tears down
cleanly (the destructors' `vkDeviceWaitIdle` covers both in-flight frames).

The riskiest piece is step 4 — everything else is mechanical array-ification.
