# Plan: Ray tracing pipeline + shader binding table

Implementation plan for roadmap item 3 in [ARCHITECTURE.md](ARCHITECTURE.md): the
raygen/miss/closest-hit shaders, the ray tracing pipeline, the shader binding table,
and the descriptor set binding the TLAS and the storage image — ending with
`vkCmdTraceRaysKHR` replacing the placeholder clear. This is the first step with a
visual result: the triangle appears, and the frame becomes "miss shader color"
around "hit shader color".

Prerequisites already in place: the TLAS is built and already visible to
`RAY_TRACING_SHADER` reads (the build's trailing barrier — the frame path must NOT
add its own acceleration-structure barrier), the storage image has `STORAGE` usage
and a ready image view, `rayTracingPipeline` is enabled, and the pipeline/trace entry
points are loaded in `RayTracingFunctions`.

**Scope decisions up front:**

- **Shaders are written in Slang, embedded, not loaded at runtime.** `slangc
  -source-embed-style u32` compiles the Slang module to SPIR-V emitted as a C header
  (a `uint32_t` array plus a `_sizeInBytes` constant), generated at build time into
  the build tree and `#include`d. No runtime file paths, keeping the
  single-executable ethos. All three stages live in **one** Slang file / one SPIR-V
  module (Slang compiles every `[shader(...)]` entry point together;
  `-fvk-use-entrypoint-name` preserves the entry-point names), so the pipeline
  creates one `VkShaderModule` and selects stages by `pName`. slangc is the official
  2026.12.2 release installed user-locally (`~/.local/opt/slang`, symlinked as
  `~/.local/bin/slangc`) — newer than the distro package; it is not a FindVulkan
  component, so CMake locates it with `find_program`.
- **Camera is hardcoded in the raygen shader.** Rays span NDC from a fixed origin;
  no camera abstraction, no push constants yet.
- **The miss shader returns the current dark red** (`0.24, 0.02, 0.015`), so the
  change reads as "the triangle appeared", not "the background changed". The
  closest-hit shader returns barycentric-derived color (visually confirms
  interpolation, not just a hit).
- **SBT lives in host-visible + coherent memory** (via the existing
  `createHostVisibleBuffer` pattern, so coherence comes with it) — same no-staging
  rationale as the geometry buffers; it is written once at startup.
- **Recursion depth 1.** Primary rays only; `maxPipelineRayRecursionDepth = 1` is
  spec-guaranteed supported.

## Step 1 — Shader source and build-time compilation ✅ (landed)

- New `shaders/triangle.slang` holding all three entry points:
  - `rayGenMain` (`[shader("raygeneration")]`): compute the pixel's NDC from
    `DispatchRaysIndex()` / `DispatchRaysDimensions()`, fire one ray (origin behind
    the triangle plane, direction +Z, `RAY_FLAG_FORCE_OPAQUE`, tMin small / tMax
    large, cull mask `0xFF`), then write the payload color into the storage image
    (binding 1, layout `GENERAL`, `[format("rgba8")]` — required for storage writes
    without `shaderStorageImageWriteWithoutFormat`). TLAS at binding 0.
  - `missMain` (`[shader("miss")]`): payload = the dark red above.
  - `closestHitMain` (`[shader("closesthit")]`): payload from
    `BuiltInTriangleIntersectionAttributes` barycentrics.
- CMake: `find_program(XRPHOTON_SLANGC slangc REQUIRED)` and one
  `add_custom_command`:
  `slangc triangle.slang -target spirv -fvk-use-entrypoint-name
  -source-embed-style u32 -source-embed-name triangle_spv -o <build>/shaders/triangle_spv.h`
  (direct SPIR-V emission targets 1.5; ray tracing needs ≥ 1.4). A custom target the
  executable depends on; the build tree's `shaders/` dir goes on the include path.
- slangc's embed output uses `uint32_t` / `size_t` without including their headers,
  so the custom command prepends `#include <cstdint>` / `<cstddef>` via `sed` —
  the generated header is self-contained (IDE-parseable standalone, no include-order
  obligation on consumers).

## Step 2 — New translation unit: `rt_pipeline.{hpp,cpp}` ✅ (landed)

The usual RAII-owner pattern, program-lifetime, borrows `VkDevice`:

- `RtPipeline` owns: descriptor set layout, pipeline layout, descriptor pool, the
  descriptor set handle (freed implicitly with the pool, but held as a member —
  both the render path and the rewrite-on-recreate path need it), the pipeline,
  and the SBT buffer + memory. It stores the four
  `VkStridedDeviceAddressRegionKHR`s (raygen/miss/hit/callable) that
  `vkCmdTraceRaysKHR` consumes — computed once at SBT build.
- Self-contained destructor: idle wait, then pipeline → pipeline layout →
  descriptor pool → descriptor set layout → SBT buffer/memory, null-guarded, with
  the per-resource log lines.
- Header forward-declares `RayTracingFunctions` (same acyclicity rule);
  `rt_pipeline.cpp` includes `vulkan_context.hpp` for the full definition plus
  `createBuffer`.

## Step 3 — Descriptor set layout, pool, set

- Layout: binding 0 `ACCELERATION_STRUCTURE_KHR` (raygen stage), binding 1
  `STORAGE_IMAGE` (raygen stage).
- Pool sized for exactly one set; allocate the one set.
- A small exported helper `writeRtDescriptorSet(device, set, tlas, storageImageView)`
  doing both writes (`VkWriteDescriptorSetAccelerationStructureKHR` chained via
  `pNext` for binding 0; `VkDescriptorImageInfo` with `IMAGE_LAYOUT_GENERAL` for
  binding 1). Called once at startup, and **again after every swapchain recreate**
  (the storage image view is recreated with the swapchain; `recreateSwapchain`
  already device-idles, so rewriting the set is race-free). This is the one new
  resize obligation — the render loop's recreate branch gains one call.

## Step 4 — Pipeline

- One `VkShaderModule` from the embedded SPIR-V (`vkCreateShaderModule` over
  `triangle_spv` / `triangle_spv_sizeInBytes`), and three
  `VkPipelineShaderStageCreateInfo`s sharing it with `pName` = `rayGenMain` /
  `missMain` / `closestHitMain` (modules are only linkage inputs; Vulkan allows one
  module across stages). Park the module in the `RtPipeline` owner — the
  scratch-buffer pattern from the AS build, not local RAII — so a failure between
  module and pipeline creation bare-returns and the destructor cleans up; on
  success it is destroyed immediately after pipeline creation and nulled.
- Three shader groups: `GENERAL` (raygen), `GENERAL` (miss),
  `TRIANGLES_HIT_GROUP` (closest hit only — no any-hit, geometry is OPAQUE).
- `vkCreateRayTracingPipelinesKHR` (no deferred operation, no pipeline cache),
  `maxPipelineRayRecursionDepth = 1`, layout = the pipeline layout from step 3.

## Step 5 — Shader binding table

- Query `VkPhysicalDeviceRayTracingPipelinePropertiesKHR` (chained through
  `vkGetPhysicalDeviceProperties2`) for `shaderGroupHandleSize`,
  `shaderGroupHandleAlignment`, `shaderGroupBaseAlignment`.
- Fetch the 3 group handles with `vkGetRayTracingShaderGroupHandlesKHR`.
- One host-visible + coherent buffer, usage
  `SHADER_BINDING_TABLE_KHR | SHADER_DEVICE_ADDRESS`, with `baseAlignment - 1`
  slack; round the base device address up (the VUIDs constrain the *device
  address* of each region, as with the build scratch). **Alignment-helper caveat:**
  the AS build's bit-mask `alignUp` is only correct for powers of two, and the spec
  guarantees that for `shaderGroupHandleAlignment` (and the scratch alignment) but
  *not* for `shaderGroupBaseAlignment` — its description says only "required
  alignment". Use a general round-up-to-multiple helper
  (`(value + a - 1) / a * a`) for the SBT base and region addresses; keep the
  bit-mask form only where power-of-two is spec-stated.
- **The CPU write pointer must be offset by the same rounding delta as the device
  address** (`alignedAddress - baseAddress` applied to the mapped pointer) —
  otherwise the handles land at unaligned offsets while the regions point at the
  aligned ones, and the GPU reads garbage records with no validation error.
- Region layout: handle stride = `alignUp(handleSize, handleAlignment)`; each
  region starts at a multiple of `shaderGroupBaseAlignment` from the aligned base.
  The raygen region's `size` must equal its `stride` (spec requirement); miss and
  hit regions hold one record each. The callable region gets `size = 0`,
  `stride = 0`, but `deviceAddress` pointing at the aligned SBT base: the current
  VUID (03692) unconditionally requires a valid SBT-buffer address with no
  zero-region exception, and pointing into the existing buffer satisfies the strict
  reading for free (the common `{0,0,0}` idiom relies on validation-layer leniency).
- Copy each handle to its aligned record via the offset mapped pointer, then store
  the four `VkStridedDeviceAddressRegionKHR`s in the owner.

## Step 6 — Replace the clear with the trace in `main.cpp`

`recordClearSwapchainImageCommandBuffer` becomes `recordTraceCommandBuffer` (rename;
renderer extraction is the *next* roadmap item, so it stays in `main.cpp`):

1. Barrier storage image `UNDEFINED → GENERAL` (`dstStage RAY_TRACING_SHADER`,
   `dstAccess SHADER_WRITE`) — `GENERAL` is the layout `imageStore` requires.
2. Bind pipeline + descriptor set at `PIPELINE_BIND_POINT_RAY_TRACING_KHR`.
3. `vkCmdTraceRaysKHR` with the owner's four regions and the swapchain extent.
   Gate the dispatch dimensions against the `width/height/depth` VUIDs (compute
   work-group limits × `maxRayDispatchInvocationCount`; spec minimum 2^30, so any
   realistic swapchain passes — check anyway and fail loudly rather than UB on an
   exotic driver). The dimensions come from `swap.extent`, which changes on resize,
   so the check runs **at startup and after every successful `recreateSwapchain`**
   — same place as the descriptor rewrite, which makes the two post-recreate
   obligations one code path — not just once at pipeline creation.
4. Barrier storage `GENERAL → TRANSFER_SRC_OPTIMAL`
   (`RAY_TRACING_SHADER`/`SHADER_WRITE` → `TRANSFER`/`TRANSFER_READ`).
5. The swapchain-image transition, blit, and present barrier stay exactly as they
   are.

The submit continues to wait on acquire at `TRANSFER` — the trace is now *outside*
that wait stage, so the GPU may trace before the image is acquired. This is the
pre-acquire overlap [ARCHITECTURE.md](ARCHITECTURE.md)'s synchronization section has
been predicting; no submit-side change needed, but the comments there and in
`drawFrame` should be updated to say the overlap is now real.

## Step 7 — Wire into `main.cpp`

`RtPipeline` declared after `ctx` (self-contained idle wait, order vs. the other
borrowing owners immaterial). Create after the acceleration structures; initial
descriptor write; `return 1` + `std::cerr` on failure; success log lines. The
recreate branch in the render loop rewrites the descriptor set after a successful
`recreateSwapchain`. `drawFrame` grows parameters for the pipeline/descriptor
handles — acceptable plumbing debt, deleted by the renderer extraction next.

## Step 8 — Verify

- Build with validation; run: the triangle renders over the dark red background —
  the first frame where pixels prove the TLAS, pipeline, and SBT all work.
- Zero validation messages across startup, steady state, resize, and teardown
  (temporary frame-limit patch again for the full-lifecycle log, then reverted).
- Resize by hand: triangle re-renders at the new extent (descriptor rewrite path).

## Step 9 — Documentation

- ARCHITECTURE.md: Status (first traced pixels), module map (five units), ownership
  (fourth owner), bring-up sequence, "The frame" + synchronization sections (trace
  replaces clear; pre-acquire overlap now real), a new "Ray tracing pipeline"
  section (embedding decision, SBT layout, descriptor-rewrite-on-resize contract),
  strike roadmap item 3.
- CLAUDE.md: summary (renders the triangle via ray tracing), layout, next step
  (renderer extraction).
- Delete this plan file once the work has landed.
