# Plan: Acceleration structures (BLAS/TLAS)

Implementation plan for roadmap item 2 in [ARCHITECTURE.md](ARCHITECTURE.md): build a
bottom-level acceleration structure for a hardcoded triangle and a top-level
acceleration structure with one identity-transform instance referencing it. Once this
lands, the ray tracing pipeline + shader binding table (roadmap item 3) has everything
it needs to trace.

All prerequisites are already in place: the device extensions and the feature chain
(`bufferDeviceAddress`, `accelerationStructure`, `rayTracingPipeline`) are enabled in
`createLogicalDevice`, and every required entry point is loaded in
`RayTracingFunctions`.

**Scope decisions up front:**

- Vertex/index/instance buffers live in host-visible + coherent memory — no staging
  pass. Slower than optimal, but it removes an entire copy/submit path, and for one
  triangle it doesn't matter. Staging lands later alongside real geometry loading.
- BLAS compaction is deliberately skipped.
- Single geometry, single instance. The point is the plumbing, not the scene.

## Step 1 — Promote `findMemoryType` and add a buffer helper ✅ landed

`findMemoryType` currently lives in `swapchain.cpp`'s anonymous namespace. Move it to
`vulkan_context.{hpp,cpp}` (it is device-level, not swapchain-level) and update
`swapchain.cpp` to use the shared declaration.

Add a `createBuffer` helper (create + allocate + bind) taking usage/property flags. It
must set `VK_MEMORY_ALLOCATE_FLAG_DEVICE_ADDRESS_BIT` on the allocation whenever the
usage includes `SHADER_DEVICE_ADDRESS`. Do not add that usage blindly: the
vertex/index/instance input buffers and scratch buffers need buffer device addresses;
the BLAS backing buffer also needs it because we query the BLAS acceleration-structure
device address for the TLAS instance. The TLAS backing buffer does not need it for the
future descriptor path, because `VkWriteDescriptorSetAccelerationStructureKHR` binds the
TLAS handle rather than a TLAS device address.

## Step 2 — New translation unit: `acceleration_structure.{hpp,cpp}` ✅ landed

Add both files to [CMakeLists.txt](CMakeLists.txt). Follows the existing RAII-owner
pattern:

- An `AccelerationStructure` struct owning: vertex/index buffers + memory, instance
  buffer + memory, BLAS buffer/memory/handle, TLAS buffer/memory/handle, and scratch
  buffers + memory until they are explicitly released after a successful build.
- Store the TLAS handle as the future RT descriptor binding target. The BLAS device
  address is needed to write the TLAS instance; a TLAS device address is not needed for
  normal descriptor-set binding.
- Like `Swapchain`, it borrows `VkDevice` non-owning. It also stores its own copy of
  `RayTracingFunctions` (or at minimum the `destroyAccelerationStructure` pointer) so
  its destructor can tear down the two `VkAccelerationStructureKHR` handles without
  caller involvement — this keeps the "destructor tears everything down, failure paths
  are bare returns" invariant intact.
- Destructor with null guards, destroying TLAS/BLAS handles before their backing
  buffers. It should either follow the existing owner pattern and wait for the device to
  go idle itself, or be declared before `swap` in `main` with that teardown-order
  invariant documented so `~Swapchain`'s idle wait runs first. Prefer the self-contained
  destructor unless the extra idle wait becomes measurable.

## Step 3 — Geometry upload

Hardcoded triangle: three `float3` vertices, three `uint32_t` indices, written
directly into mapped host-visible buffers with usage
`ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR | SHADER_DEVICE_ADDRESS`.

## Step 4 — BLAS build setup

- Fill `VkAccelerationStructureGeometryKHR` (triangles, `VK_FORMAT_R32G32B32_SFLOAT`,
  `VK_INDEX_TYPE_UINT32`, buffer device addresses from `getBufferDeviceAddress`).
- Query sizes with `getAccelerationStructureBuildSizes`
  (`PREFER_FAST_TRACE` build flag) using `pMaxPrimitiveCounts = {1}`. The command build
  path also needs a matching `VkAccelerationStructureBuildRangeInfoKHR` with
  `primitiveCount = 1` for this triangle.
- Create the backing buffer (`ACCELERATION_STRUCTURE_STORAGE | SHADER_DEVICE_ADDRESS`,
  device-local) and the `VkAccelerationStructureKHR` via
  `createAccelerationStructure`.
- Create a device-local scratch buffer (`STORAGE_BUFFER | SHADER_DEVICE_ADDRESS`),
  respecting `minAccelerationStructureScratchOffsetAlignment` from
  `VkPhysicalDeviceAccelerationStructurePropertiesKHR`. The actual
  `scratchData.deviceAddress` must be aligned, not merely the allocation size: allocate
  `buildScratchSize + alignment - 1`, round the base device address up, and keep the
  original buffer/memory handles for cleanup.

## Step 5 — TLAS build setup

- Get the BLAS device address via `getAccelerationStructureDeviceAddress`.
- One `VkAccelerationStructureInstanceKHR` with identity transform, mask `0xFF`,
  referencing that address, in a host-visible instance buffer (usage additionally
  `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR`).
- Same query-sizes / create-buffer / create-AS / scratch sequence with geometry type
  `INSTANCES`, again with `pMaxPrimitiveCounts = {1}` and a matching build range whose
  `primitiveCount = 1`.

## Step 6 — Record and submit the build

A `buildAccelerationStructures(...)` entry point called from `main` after
`allocateCommandBuffer`, after `createFrameSyncObjects`, and before the render loop:

- Record both builds into the existing `ctx.commandBuffer` as one-time-submit:
  `cmdBuildAccelerationStructures` for the BLAS, then a memory barrier
  (`ACCELERATION_STRUCTURE_BUILD` → `ACCELERATION_STRUCTURE_BUILD`, AS-write →
  AS-read) so the TLAS build sees the finished BLAS, then the TLAS build.
- Submit on the trace queue using `ctx.inFlightFence`: wait for its initial signaled
  state, reset it, submit, and wait again. This leaves the fence signaled before the
  first `drawFrame`, preserving the current first-frame invariant without introducing a
  temporary fence that can leak.
- Release the scratch buffers after the waited build succeeds. Until that point they
  remain owned by `AccelerationStructure`, so any early return still cleans them up.

## Step 7 — Wire into `main.cpp`

Declare the `AccelerationStructure` owner after `ctx` (before `swap` if relying on
`~Swapchain`'s idle wait; otherwise order relative to `swap` is not semantically
important), call the build after sync-object creation, `return 1` on failure with a
`std::cerr` line, and log success — matching the existing bring-up logging style. The
acceleration structures are swapchain-independent, so the resize/recreate paths do not
change.

## Step 8 — Verify

Build with `-DXRPHOTON_ENABLE_VALIDATION=ON`, run, and confirm:

- the new "built acceleration structures" log line appears;
- zero validation messages — the AS build paths are heavily validated, so this is the
  real test, since there is no visual change yet;
- clean teardown on exit;
- resize still works.

## Step 9 — Documentation

- ARCHITECTURE.md: strike through roadmap item 2, add an "Acceleration structures"
  subsystem section (ownership, build-time flow, why host-visible-no-staging), update
  the Status and ownership sections.
- CLAUDE.md: update the layout list and the "Next step" paragraph.
- Delete this plan file once the work has landed.
