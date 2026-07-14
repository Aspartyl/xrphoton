# xrPhoton — Roadmap Step 2 Implementation Plan: Geometry + Scene Representation

> **Historical through M3b; revised from M4 onward.** Milestones M1–M3b
> landed as described and their decision records remain accurate history. The
> old M4 runtime-interchange loader and the numbered milestones that followed
> it are superseded. The runtime now loads **OGFx**, xrPhoton's modernized
> evolution of X-Ray's OGF. [FORMATS.md](FORMATS.md) is the source of truth
> for the revised M4 round-trip and asset-pipeline sequence: OGFx is the only
> runtime model format, while an optional interchange importer may exist only
> as an offline front end to the shared compiler. The format-independent
> designs below — the N-BLAS generalization, opaque/alpha-tested hit-group
> split, `RayTypeCount` SBT scheme, and texture design — remain reference
> designs whose new milestone numbers are deliberately unassigned. Check
> FORMATS.md before implementing any post-M3b milestone.

This is the implementation record for roadmap step 2 in
[ARCHITECTURE.md](ARCHITECTURE.md). M1–M3b describe landed work; later
format-independent sections remain design references, subject to the revised
sequence in FORMATS.md.

## 1. Goal and scope

Replace the hardcoded triangle with real meshes loaded from asset files:
indexed vertex data with per-vertex normals/UVs fetched in the
closest-hit shader via buffer device addresses, multiple BLASes with
per-instance transforms, material data in storage buffers indexed per geometry,
and the opaque/alpha-tested geometry split built into the hit-group/SBT layout
from the start — including real any-hit alpha testing against texture alpha.
The step also lands the infrastructure ARCHITECTURE.md explicitly deferred to
it: VMA as the engine allocator, staging uploads for device-local geometry, and
explicit aligned suballocation as the definitive answer to
acceleration-structure build-input address alignment.

**Delivery discipline (governing constraint of this plan):** every milestone
leaves the app building and rendering something verifiable on screen, with
clean validation. The triangle (later: quad, later: scene) never stops
rendering for longer than the work inside one milestone. Risk retires
front-to-back: allocator first, math second, the Slang/BDA/descriptor unknowns
third, the OGFx round-trip fourth, then real-content conversion and N-BLAS
generalization, SBT restructuring, and textures.

## 2. Non-goals

Each deferred item names what picks it up, per the trigger-based-engineering
convention:

- **X-Ray content conversion** — the offline converter and its OGFx target
  are specified in [FORMATS.md](FORMATS.md); it follows the revised M4 as
  the first follow-on.
- **Dynamic scene** (TLAS refits, skinning) — roadmap step 3. This plan only
  avoids painting it into a corner (see §8).
- **Lighting / path tracing** — roadmap step 4. Closest-hit shading stays a
  debug visualization (baseColor × a world-space normal term).
- **Alpha *blending*** — blend-mode (translucent) materials are compiled as
  opaque with a conversion-time warning; order-independent transparency is a
  path-tracing-step (step 4) concern.
- **Mipmaps / texture LOD** — textures upload a single mip and shaders sample
  `SampleLevel(uv, 0)`. LOD selection needs ray differentials or a cone
  heuristic, which belongs to step 4 where the ray model is decided. No
  mip-generation machinery is built for a consumer that does not exist.
- **Asset caching, hot reload, multiple scenes** — revised M4 loads its one
  OGFx test asset at startup. Arbitrary asset selection and reload trigger on
  real content iteration during the game-systems era, not before.
- **Multi-file Slang modules and the slangc dependency-file CMake wiring** —
  deferred exactly as ARCHITECTURE.md specifies; the trigger (a shader
  `import`) does not fire this step (see D10 and §9).

## 3. Design decisions

One answer per question, per the "one focus, clear vision" convention; the
rejected alternative is recorded so the question stays closed.

### D1. Runtime model format: OGFx, written and converted offline

**Decision (revised — supersedes the original interchange-format choice):**
the runtime loads **OGFx**. The format, its container and validation rules,
and the revised milestone sequence live in [FORMATS.md](FORMATS.md). No
interchange format is part of the runtime. External and test content
primarily enters through Blender; a future optional interchange importer may
feed the shared compiler offline, but can never become a runtime loader or a
second OGFx writer. The original decision recorded here — a third-party
interchange format parsed by a vendored runtime library — is void, along with
its vendored-parser plan.

**Vendoring rule (one answer for the whole project — unchanged, and landed
with VMA in M1):** single-file C/C++ headers are vendored under
`third_party/<lib>/` with their license, each pinned to a named upstream
release (recorded in a comment at the top of the file) and shipped with its
upstream `LICENSE` file, and the directory is added as a `SYSTEM` include so
`-Wall -Wextra` stays clean. Multi-header libraries come from system
`find_package` (the existing Vulkan/glfw convention). This step vendored one
header (vk_mem_alloc) and added one system package (glm). All
`*_IMPLEMENTATION` macros live in one dedicated TU (see D3) so no engine TU
pays their compile cost.

### D2. Math library: adopt GLM (system package), retire the in-house Vec3

**Decision:** GLM via `find_package(glm REQUIRED)` +
`target_link_libraries(xrPhoton PRIVATE glm::glm)`. `camera.hpp`'s `Vec3` is
deleted; `Camera` / `CameraPushConstants` migrate to `glm::vec3`, keeping the
explicit `pad0..pad3` floats and every existing `static_assert` (offsets
0/16/32/48, sizeof 64 — the shader ABI is unchanged, and the asserts prove the
migration changed nothing). `camera.hpp` stays Vulkan-free (GLM is
Vulkan-free).

**Environmental fact, not optional:** GLM is **not currently installed** on the
dev machine (no `/usr/include/glm`, no CMake config). Milestone M2 starts with
`apt install libglm-dev` (or the distro equivalent), and the prerequisite is
added to the Build sections of [CLAUDE.md](CLAUDE.md) and README in the same
milestone — otherwise "clone and build" silently breaks at configure time.
Distro packaging variance in the exported target name (`glm::glm` vs. the
legacy `glm`) is a known risk (§7); the CMake handles it once, in one place,
with a comment stating the requirement.

The one layout hazard gets exactly one owner — a file-private helper in the
anonymous namespace of `acceleration_structure.cpp`, its sole consumer (the
instance-population code; step 3's refit path lives in the same TU):

```cpp
// acceleration_structure.cpp — VkTransformMatrixKHR is the top three rows of a
// ROW-MAJOR 4x4 (i.e. a row-major 3x4); GLM is column-major, so this is a
// transpose, done exactly once, here.
VkTransformMatrixKHR toVkTransformMatrix(const glm::mat4& m);
```

**Rationale:** scene hierarchies require 4×4 multiplies,
quaternion→matrix, and TRS composition during offline compilation and future
scene loading; instance transforms require
the column-major → row-major 3×4 conversion. Growing in-house math to cover
that is scope with no engine value and a classic source of transpose bugs.
`camera.hpp`'s own comment says `Vec3` stays minimal "until a broader math
module earns its keep" — this step is when it is earned. Keeping both Vec3 and
GLM would be exactly the parallel-systems situation the conventions forbid.

**Rejected:** growing in-house Vec3/Mat4 (untestable math scope,
transpose/handedness bugs, zero differentiation value for a renderer project).

### D3. VMA: full adoption as the engine allocator, threaded through VulkanContext

**Decision:** Vendor `third_party/vma/vk_mem_alloc.h` (pinned release + LICENSE
per D1's vendoring rule). `VMA_IMPLEMENTATION` — and any later vendored
implementation macros — are defined in one new dedicated TU,
`src/third_party_impl.cpp` (wrapped in `#pragma GCC diagnostic` suppressions).
Rationale for the dedicated TU: `vulkan_context.cpp` is the most-edited file
this step, and VMA's implementation is a large recompile tax to attach to it;
one small TU that never changes isolates all three heavy implementations.

`VulkanContext` gains `VmaAllocator allocator`, created right after the device
via a new `createAllocator(instance, physicalDevice, device, &allocator)` with
`VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` — the allocator-level
equivalent of the existing usage → `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`
pairing in `createBuffer`, so buffer usage and allocation flags still cannot
diverge — and `VmaAllocatorCreateInfo::vulkanApiVersion = RequiredApiVersion`:
leaving it zero tells VMA to assume Vulkan **1.0**, silently downgrading which
core entry points it uses. `~VulkanContext` destroys the allocator immediately before the device
(every buffer-owning struct is declared after `ctx` in `main()` and
self-idle-waits, so all allocations are already freed).

`createBuffer` is **rewritten in place** (same header, no parallel path) and
`findMemoryType` is **deleted**:

```cpp
// vulkan_context.hpp
VkResult createBuffer(
    VmaAllocator allocator,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VmaAllocationCreateFlags allocationFlags,  // 0 = device-local;
                                               // HOST_ACCESS_SEQUENTIAL_WRITE | MAPPED for CPU-written
    VkBuffer* buffer,
    VmaAllocation* allocation);
```

Memory usage is always `VMA_MEMORY_USAGE_AUTO`. Host-written buffers (SBT,
TLAS instance buffer, staging) pass
`VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
VMA_ALLOCATION_CREATE_MAPPED_BIT` and write through
`VmaAllocationInfo::pMappedData` — no `vkMapMemory`/`vkUnmapMemory` call sites
survive the migration.

**Coherency is pinned, not assumed:** the HOST_ACCESS flags alone do not
guarantee a `HOST_COHERENT` memory type, and the code being replaced explicitly
relies on coherent memory to skip flushes (the "no explicit flush" comments in
`acceleration_structure.cpp` and `rt_pipeline.cpp`). Every HOST_ACCESS
allocation therefore also sets
`VmaAllocationCreateInfo::requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT`
— the spec guarantees a HOST_VISIBLE + HOST_COHERENT type exists, the migration
stays behavior-identical to today, and no `vmaFlushAllocation` call sites are
introduced (one write discipline, not two). Step 3's per-frame instance-buffer
rewrites inherit this policy. *Rejected:* mandated `vmaFlushAllocation` after
every mapped write (a second write discipline whose omission is exactly the
silent-stale-read class validation cannot see).

**Header hygiene:** one tiny forwarding header, `src/vma_fwd.hpp`, holds
everything project headers need to *name* VMA types: the opaque-handle
typedefs (`typedef struct VmaAllocator_T* VmaAllocator;` /
`typedef struct VmaAllocation_T* VmaAllocation;` — identical to VMA's own
`VK_DEFINE_HANDLE`-style definitions, so redeclaration is legal) and the
`typedef VkFlags VmaAllocationCreateFlags;` alias `createBuffer`'s signature
uses. `vulkan_context.hpp` and `gpu_scene.hpp` include it; `vk_mem_alloc.h`
itself appears **only** in the `.cpp`s that call VMA functions, so the large
declaration-only header cost never leaks in through the project's
most-included headers, and the declarations live in exactly one place.

Every `VkDeviceMemory` field in `Swapchain` (the storage
image moves to `vmaCreateImage`), `AccelerationStructure`, and `RtPipeline`
becomes a `VmaAllocation`, and **each of those owners gains a borrowed
`VmaAllocator` member** next to its borrowed `VkDevice` —
`vmaDestroyBuffer`/`vmaDestroyImage` take the allocator, so without it the
destructors cannot honor the null-guarded teardown contract. The failure
contract (partial handles parked in the owner for the destructor) is preserved
verbatim.

**Rationale:** ARCHITECTURE.md names this step as VMA's trigger; per-mesh +
per-texture resources under one-`vkAllocateMemory`-each would collide with
`maxMemoryAllocationCount` (guaranteed minimum only 4096) and forfeit
alignment/pooling control. Migrating *everything* in one early milestone (M1)
keeps a single allocation system.

**Rejected:** keeping raw `vkAllocateMemory` with a hand-rolled arena
(re-implements VMA badly); partial adoption "for new resources only" (two
allocators side by side — precisely the forbidden parallel system).

### D4. GPU geometry: three unified device-local buffers, element-granular suballocation, staged uploads with an explicit trailing barrier

**Decision:** the GPU scene owner (D8) holds three unified device-local
geometry buffers, each uploaded once via staging:

| Buffer | Contents | Usage flags |
|---|---|---|
| `positionBuffer` | tightly packed `float3` positions, all geometries concatenated | `AS_BUILD_INPUT_READ_ONLY \| SHADER_DEVICE_ADDRESS \| TRANSFER_DST` |
| `attributeBuffer` | `VertexAttributes` records (20 B, D8), same order | `SHADER_DEVICE_ADDRESS \| TRANSFER_DST` |
| `indexBuffer` | `uint32` indices, all geometries concatenated (legacy 16-bit sources widened offline by the converter — FORMATS.md) | `AS_BUILD_INPUT_READ_ONLY \| SHADER_DEVICE_ADDRESS \| TRANSFER_DST` |

**Aligned suballocation — the deferred item's definitive answer:** geometry
ranges are packed at **element granularity** — each geometry's range starts at
`firstVertex * 12`, `firstVertex * 20`, or `firstIndex * 4` bytes. Every
element size in play (12-byte position, 20-byte attribute record, 4-byte index)
is a multiple of 4, so every derived device address is 4-aligned **by
construction** — which meets both spec minimums the build commands impose
(vertex component size 4 for `VK_FORMAT_R32G32B32_SFLOAT`, index size 4 for
`VK_INDEX_TYPE_UINT32`) and the scalar-load alignment BDA fetches need. No
inter-range padding constant is introduced: an alignment constant plus padding
machinery would be structure serving no requirement that element granularity
does not already satisfy. The existing runtime guard
`hasRequiredBuildInputAlignment` stays, applied to each unified buffer's
**base** device address only — exactly the backstop role its own comment
(`acceleration_structure.cpp:66-68`) anticipated. `BlasVertexFormat` stays
`VK_FORMAT_R32G32B32_SFLOAT`, so the device-selection format gate
(`hasRequiredAccelerationStructureFormatSupport`) is untouched.

**Addressing:** each geometry gets *pre-offset device addresses*
(`bufferBaseAddress + elementOffset * elementSize`, computed in `VkDeviceSize`
math) stored in its `GeometryRecord` (D5). The same addresses feed
`VkAccelerationStructureGeometryKHR::triangles.vertexData/indexData`, with
`primitiveOffset = 0` and `firstVertex = 0` in the range info — one addressing
mechanism serves both the AS build and the hit shaders. BDA loads require only
`SHADER_DEVICE_ADDRESS` usage, not `STORAGE_BUFFER`.

**Positions de-interleaved from attributes**, deliberately: (a) the BLAS build
reads only positions, and a tight 12-byte stride keeps build-input reads dense;
(b) step 3's compute skinning deforms positions *and normals* (deformation
bends both — see §8 for what that obligates), and keeping shading attributes
out of the position stream narrows what step 3 must duplicate per frame slot
while keeping today's BLAS-input reads dense.

**Upload path and visibility:** a new helper in `vulkan_context.{hpp,cpp}`:

```cpp
// vulkan_context.hpp — creates the device-local buffer, stages data into it
// through a transient host-visible buffer, and submits on the borrowed
// command buffer/queue/fence (returned signaled, per the AS-build contract).
VkResult uploadDeviceLocalBuffer(
    VmaAllocator allocator, VkDevice device,
    VkCommandBuffer commandBuffer, VkQueue queue, VkFence fence,
    const void* data, VkDeviceSize size, VkBufferUsageFlags usage,
    VkBuffer* buffer, VmaAllocation* allocation);
```

Every upload command buffer ends with an **explicit trailing memory barrier**:

```
srcStage TRANSFER → dstStage ACCELERATION_STRUCTURE_BUILD_KHR | RAY_TRACING_SHADER_KHR
srcAccess TRANSFER_WRITE → dstAccess ACCELERATION_STRUCTURE_READ_KHR | SHADER_READ
```

This mirrors the AS build's own trailing-barrier idiom and is **mandatory, not
belt-and-suspenders**: per the codebase's documented reasoning
(ARCHITECTURE.md, "One submission, two barriers"), the fence wait gives only
the *host* visibility of the transfer, and submission order carries no memory
dependency — without this barrier the subsequent AS-build and trace
submissions read the device-local buffers with no device-side dependency on
the copy. The barrier's second scope covers all later commands in submission
order on the queue, so neither the AS build nor the frame path needs an
upload-related barrier of its own. The helper records the barrier
unconditionally — one upload shape, no flags for futures that don't exist.

**Rejected:** *per-mesh buffers* (N× buffers/allocations and staging passes,
and the aligned-suballocation deferred item would remain unanswered; BDA makes
unified buffers exactly as easy to address); *an interleaved single vertex
buffer* (forces the BLAS build to stride over shading data and makes step-3
skinning writes strided); *an alignment constant with inter-range padding*
(rejected above — machinery without a requirement).

### D5. Hit-shader addressing: flat GeometryRecord table indexed by `InstanceID() + GeometryIndex()`

**Decision:** one flat array `GeometryRecord[totalGeometryCount]` in a
device-local storage buffer (descriptor binding 2), one record per
(BLAS, geometry) pair — i.e. per `SceneGeometry` range. Every TLAS instance sets
`instanceCustomIndex = meshes[meshIndex].firstGeometry` (the flat index of its
BLAS's first geometry). Hit shaders compute:

```
recordIndex = InstanceID() + GeometryIndex()   // InstanceID() == instanceCustomIndex in Slang/HLSL
```

Record ABIs (definitions in §4.2). Indices stored in `indexBuffer` are
mesh-local (0-based within the geometry), so `positionAddress` /
`attributeAddress` point at that geometry's vertex 0 and
`indexAddress[3 * PrimitiveIndex() + k]` indexes directly.

`instanceCustomIndex` is 24-bit, and `instanceShaderBindingTableRecordOffset`
(D6) is also 24-bit and scales by the ray-type count — so the load-time
assert covers the larger of the two:
`geometries.size() * RayTypeCount < (1u << 24)`, failing loudly to
`std::cerr`.

**Rationale:** one indexing scheme identifies a geometry for *both* record
lookup and SBT hit-group selection (D6 uses the identical flat index), the
records live in an ordinary storage buffer where step 3 can update them
without touching the SBT, and the scheme is uniform for single- and
multi-geometry BLASes. Records/materials are descriptor-bound (not a BDA in
push constants) because they are scene-static, the descriptor path already
exists, and push-constant space stays budgeted for per-frame data.

**Rejected:** *SBT shader-record data* (embedding per-geometry data after the
handles couples scene data to SBT layout — every step-3 change would force SBT
rewrites, and record-stride math grows `maxShaderGroupStride` constraints for
zero benefit); *`GeometryIndex()` alone* (no cross-BLAS identity);
*`InstanceIndex()` + a per-instance table* (indirects through a second table to
reach per-geometry data that `instanceCustomIndex + GeometryIndex()` reaches in
one step).

### D6. Hit groups and SBT: two hit groups, one hit record per geometry per ray type, everything parameterized by `RayTypeCount`

**Decision — pipeline groups** (order remains the SBT contract):

| Group | Type | Shaders |
|---|---|---|
| 0 | GENERAL | `rayGenMain` |
| 1 | GENERAL | `missMain` |
| 2 | TRIANGLES_HIT_GROUP | closestHit = `closestHitMain` (opaque class) |
| 3 | TRIANGLES_HIT_GROUP | closestHit = `closestHitMain`, anyHit = `anyHitMain` (alpha-tested class) |

`GroupCount = 4`, `stageCount = 4` — the closest-hit stage is shared by both
hit groups, legal and deliberate: one shading path; the two classes differ
only in traversal behavior. The existing `VK_SHADER_UNUSED_KHR` pre-fill loop
already covers the new group.

**The scaling constant — defined once, in the build**, because its consumers
span three domains: `rt_pipeline.cpp` (SBT layout), `acceleration_structure.cpp`
(instance `sbtOffset`), the loader's 24-bit assert, and the shader's `TraceRay`
multiplier. `CMakeLists.txt` sets `XRPHOTON_RAY_TYPE_COUNT` to `1` and feeds
the same value to both sides: `target_compile_definitions` for C++ — surfaced
as the project-facing constant in `rt_pipeline.hpp` — and a `-D` define on the
`slangc` custom command for the shader.

```cpp
// rt_pipeline.hpp — number of ray types traced against the SBT. Step 4 (NEE
// shadow rays) flips the one CMake definition to 2 and adds the new shaders;
// every offset formula, instance sbtOffset, and TraceRay multiplier is written
// against this — nothing else moves.
constexpr uint32_t RayTypeCount = XRPHOTON_RAY_TYPE_COUNT;
```

A comment-pinned CPU/shader pair (the `MaxSceneTextures`/`[1024]` treatment)
was rejected *for this constant specifically*: a descriptor-array-size
mismatch is caught at pipeline creation, but a `RayTypeCount` mismatch
silently corrupts SBT routing — undefined traversal, no error from any layer —
which is exactly the failure class this design spends its machinery
eliminating. Different failure modes, different mechanisms.

**SBT layout** (miss region parameterized from day one, so step 4 changes zero
offset formulas):

```
recordStride   = roundUpToMultiple(handleSize, shaderGroupHandleAlignment)     // division form, unchanged
raygenOffset   = 0
missOffset     = roundUpToMultiple(recordStride, baseAlignment)
hitOffset      = missOffset + roundUpToMultiple(RayTypeCount * recordStride, baseAlignment)
hitRecordCount = geometries.size() * RayTypeCount
tableSize      = hitOffset + hitRecordCount * recordStride
hitRecord[i]   = handle(geometries[i / RayTypeCount].alphaTested ? group 3 : group 2)
                 // records handle-only, per D5. geometry = i / RayTypeCount,
                 // rayType = i % RayTypeCount — records interleave per geometry;
                 // step 4 switches on rayType here to pick the shadow-variant group.
missRegion     = { tableAddress + missOffset, .stride = recordStride, .size = RayTypeCount * recordStride }
hitRegion      = { tableAddress + hitOffset,  .stride = recordStride, .size = hitRecordCount * recordStride }
```

The existing division-form `roundUpToMultiple` (deliberately not the bit-mask
`alignUp`; `shaderGroupBaseAlignment` is not spec-guaranteed power-of-two), the
buffer over-allocation by `baseAlignment - 1`, and the **`alignmentDelta` shift
applied identically to the CPU-side mapped writes** are all preserved by
editing `buildShaderBindingTable` in place; the three memcpys become a loop
over hit records. The function gains `const SceneData&` (D8). The callable
region keeps its valid-address-with-zero-size shape (VUID 03692).

**Selection math** (spec:
`hitGroup = instanceSbtOffset + geometryIndex * multiplier + rayContribution`):

- `instance.instanceShaderBindingTableRecordOffset = firstGeometry * RayTypeCount`
  — with `RayTypeCount == 1`, the *same number* as `instanceCustomIndex`.
- `TraceRay(..., RayContributionToHitGroupIndex = 0,
  MultiplierForGeometryContributionToHitGroupIndex = RayTypeCount,
  MissShaderIndex = 0, ...)`.
- ⇒ selected hit record == flat geometry index == `GeometryRecord` index. One
  number, two tables. Step 4's shadow rays pass ray contribution 1 and miss
  index 1; hit records interleave per geometry
  `[geo0·radiance, geo0·shadow, geo1·radiance, …]`.

**Opacity flags:** each `VkAccelerationStructureGeometryKHR` sets
`VK_GEOMETRY_OPAQUE_BIT_KHR` iff its geometry class is opaque; alpha-tested
geometries set no flags (`NO_DUPLICATE_ANY_HIT_INVOCATION` deliberately
omitted — the alpha test is idempotent and the flag costs traversal
performance). `RAY_FLAG_FORCE_OPAQUE` is **removed** from the raygen
`TraceRay` — with it set, any-hit never runs regardless of geometry flags.
Instance-level `VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR` is **not used**: the
class is a per-geometry property (a tree BLAS mixes an opaque trunk with
alpha-tested leaf cards), and instance-level forcing would silently break
mixed BLASes. Opaque geometry keeps identical traversal cost to today via the
per-geometry OPAQUE bit.

**Rejected:** *per-instance class with two total hit records* (forces a
uniform class per BLAS — splits every mixed foliage model into two
meshes/instances; content-hostile for exactly the STALKER asset shapes this
split exists for); *per-geometry records with embedded data* (rejected in D5);
*"append the shadow miss record later"* (leaves offset math to re-derive in
step 4 — parameterizing by `RayTypeCount` now costs nothing).

### D7. Alpha testing: structural split first (one-milestone procedural alpha), textures as the final milestone

**Decision:** both land this step, sequenced split-before-textures. The
hit-group/SBT split lands in its own later milestone with a file-private
procedural alpha function in the any-hit (a UV checkerboard tested against
`alphaCutoff`); the following texture milestone replaces that function's body
with real texture alpha and deletes the checkerboard.

**The ordering is a conscious resolution of a contested point.** The
checkerboard is scaffolding *inside* the step — alive for exactly one
milestone, deleted by the next, never a parallel system that survives the
step. Its benefit is that the split milestone's failure domain — the step's most
regression-prone surface (SBT restructuring, hit-group selection, geometry
flags) — contains **zero image machinery**: a mis-selected hit group shows as
a cutout on the wrong object, not as "maybe the texture upload is broken".
Textures then land against an already-proven traversal structure. The
alternative (textures before the split, zero throwaway) was rejected because
it stacks the descriptor-indexing/feature/upload unknowns *under* the SBT
restructuring, widening the failure domain of the riskiest milestone.

Real texture-alpha testing is in-scope because the roadmap calls any-hit alpha
testing "the engine's single biggest traversal cost lever" — a lever is not
validated until it pulls real content.

**Texture/descriptor design (later texture milestone):**

- **Fallback texture, no sentinel branch:** texture array index 0 is always a
  generated 1×1 opaque-white `R8G8B8A8_SRGB` image; materials without a
  baseColor texture get `baseColorTexture = 0`; loaded textures start at
  index 1. Hit shaders always sample — no `0xFFFFFFFF` sentinel, no divergent
  "has texture?" branch in the any-hit hot path.
- **Fixed-size array, every slot written:** binding 4 is
  `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, count
  `constexpr uint32_t MaxSceneTextures = 1024` (in `gpu_scene.hpp`), stages
  `CLOSEST_HIT | ANY_HIT`. The loader writes the fallback view into every
  unused slot, so **every** descriptor in the array is always valid — which
  drops the need for `descriptorBindingPartiallyBound`, variable descriptor
  counts, and `runtimeDescriptorArray` entirely. The only descriptor-indexing
  feature required is `shaderSampledImageArrayNonUniformIndexing` (material
  indices diverge within a wave ⇒ shaders index with
  `NonUniformResourceIndex`).
- **Limit gate:** `MaxSceneTextures` exceeds the *spec minimums* of every
  descriptor limit a combined-image-sampler array counts against — and it
  counts against **both** the sampled-image and the sampler limits
  (`maxPerStageDescriptorSamplers`' guaranteed minimum is only 16), per stage
  and per set, plus the aggregate `maxPerStageResources` (spec minimum 128).
  Device selection gains one check requiring each of
  `maxPerStageDescriptorSampledImages`, `maxPerStageDescriptorSamplers`,
  `maxDescriptorSetSampledImages`, and `maxDescriptorSetSamplers` to be
  `>= MaxSceneTextures` (the binding holds *exactly* `MaxSceneTextures`
  descriptors — the fallback lives inside slot 0, not on top), and
  `maxPerStageResources >= MaxSceneTextures + 2` (the two SSBOs share the hit
  stages) — declared in `gpu_scene.hpp` and called from
  `queryPhysicalDeviceSuitability`, mirroring the existing
  `hasRequiredAccelerationStructureFormatSupport` cross-link; each failed
  limit lands in the rejection report like every other category.
  Scene-*dependent* limits cannot gate device selection and are checked at
  startup instead (D8).
- **Sampler:** one shared `VkSampler` (linear min/mag, repeat, no anisotropy —
  the feature is not enabled; maxLod 0), reused in every write.
- **Upload path:** `vmaCreateImage` device-local `VK_FORMAT_R8G8B8A8_SRGB`,
  single mip; staging buffer → `UNDEFINED → TRANSFER_DST` barrier →
  `vkCmdCopyBufferToImage` → `TRANSFER_DST → SHADER_READ_ONLY_OPTIMAL` with
  `dstStageMask = RAY_TRACING_SHADER` (the image-barrier equivalent of D4's
  trailing visibility barrier) — recorded on the borrowed frame-0 command
  buffer like all startup uploads. The texture milestone must first decide how
  an OGFx logical texture reference resolves to decoded RGBA8 bytes (runtime
  DDS decode versus a separately compiled texture payload are still open);
  only then does that data enter `SceneData` (D8). Revised M4 intentionally
  rejects nonempty texture references rather than choosing this early, and
  that gate remains through the N-BLAS and alpha-split probes.

**Rejected:** *`PARTIALLY_BOUND` + sentinel* (an extra device feature plus a
divergent shader branch, to avoid ~1000 startup descriptor writes that cost
nothing); *variable-count descriptors* (more feature bits and plumbing to save
descriptors that cost nothing); *textures-first ordering* (rejected above).

### D8. CPU/GPU scene split: a Vulkan-free `SceneData` value plus a `GpuScene` RAII owner, in two new TUs

**Decision:** the scene lands as **two** units, split along the same line the
codebase already draws for the camera (Vulkan-free data/policy vs. Vulkan
resource ownership) — and along exactly the cut step 3 needs (CPU scene state
stays alive and mutable in `main()`; GPU state is an owner that step 3 can
grow per-frame structures in):

- **[src/scene.hpp](src/scene.hpp) / [src/scene.cpp](src/scene.cpp)** —
  `SceneData`, a plain value struct (no Vulkan, no VMA; GLM only — the header
  includes `<glm/glm.hpp>`, keeping the `camera.hpp` precedent of
  Vulkan-free), plus the scene-loading entry point (the OGFx loader per
  [FORMATS.md](FORMATS.md)). File parsing is confined to `scene.cpp`; no
  parser type leaks out.
- **[src/gpu_scene.hpp](src/gpu_scene.hpp) / [src/gpu_scene.cpp](src/gpu_scene.cpp)**
  — `GpuScene`, the RAII owner of every scene GPU resource, plus the GPU
  record ABI structs (`GeometryRecord`, `MaterialRecord`) and
  `createGpuScene`. The header includes Vulkan + `vma_fwd.hpp` (D3's
  forwarding header — never `vk_mem_alloc.h`) and forward-declares
  `SceneData` and `RayTracingFunctions` (header-acyclicity rule).

Struct definitions in §4.1/§4.2. The original rule list here was written
against the superseded interchange format. The shared compiler validates
source data before writing; the runtime OGFx loader defensively repeats every
structural and semantic check that can be expressed against the serialized
file (magic/version gates, chunks, exact-stride arrays, indices, ranges,
strings, and finite bounds). [FORMATS.md](FORMATS.md) specifies those
obligations. Device-dependent limits remain startup checks after loading.
Every failure is loud and names its input; there is no silent degradation.

- One topology (triangles) and indexed geometry only; hard errors over
  guessed repairs.
- Index values bounds-checked against their range's vertex count; index
  counts nonzero and divisible by 3; empty geometries and meshes are load
  errors — all of which guards `maxVertex = vertexCount - 1` and the
  unchecked BDA reads downstream.
- OGFx stores models, not world-instance placement. When a future scene/level
  producer supplies transforms, it rejects or explicitly skips a
  zero-determinant transform (`WorldToObject` is undefined) with a diagnostic
  naming the instance; a scene left with no renderable instances is an error.
- Geometry counts are element-indexed `uint32_t` (`firstVertex`/`firstIndex`
  etc.), so no byte-offset field exists to silently cap a buffer at 4 GiB;
  byte offsets are derived in `VkDeviceSize` math at upload/build time.
  Element counts overflowing `uint32_t` and the 24-bit SBT/custom-index cap
  (D5) are checked at load.
- **Scene-vs-device checks at startup** (between load and build — the
  scene-dependent siblings of D7's static gate, same loud-failure family):
  the `GeometryRecord`/`MaterialRecord` buffer sizes against
  `limits.maxStorageBufferRange` (the 24-bit geometry cap admits a ~512 MiB
  record buffer; the spec floor for the range limit is 128 MiB),
  `instances.size()` against the acceleration-structure properties'
  `maxInstanceCount`, and each BLAS's summed triangle count against
  `maxPrimitiveCount`.

**Probe model and placement fixture (deferred until multi-mesh content arrives
via the FORMATS.md follow-ons):** one Blender project feeds deterministic,
milestone-specific OGFx probe outputs through the same add-on/compiler writer;
none contains world instances. The N-BLAS generalization adds a small
file-private preview table mapping mesh indices to deterministic transforms.
That table is temporary bring-up scene ownership, not another geometry path or
file format, and is retired when level/scene data gets its real owner.

The first output is opaque-only and carries no texture references: a ground
plane, two box meshes with distinct material factors, one shared mesh, and a
sphere. The placement table references the shared mesh twice (rotated and
translated), and applies non-uniform scale composed with rotation to the sphere
for the inverse-transpose oracle. Every material has a distinct
`baseColorFactor` so identity errors remain visible before textures. One source
mesh is parented under a rotated + translated empty and compiled into
model-local geometry, proving offline hierarchy flattening. The sphere is
load-bearing: axis-aligned normals under a diagonal scale cannot distinguish
the plain inverse from the inverse-transpose after normalization, while its
varying normals plus rotation can.

The split-milestone output adds a mesh containing both opaque and alpha-tested
geometry plus an alpha-cutout card, but still no texture references. The
texture-milestone output adds the checker ground and leaf-style RGBA references
only after their carrier/resolver is decided. Regeneration is automated from
one authoring project; no immutable output is asked to cross a capability gate.

**Rejected:** *one merged `Scene` owner holding CPU vectors + GPU handles*
(puts Vulkan/VMA into the loader's header, blurs the CPU-static vs.
GPU-dynamic boundary step 3 depends on, and breaks the Vulkan-free-data-unit
precedent for no gain); *keeping any hardcoded-geometry path alongside the
loader* (parallel systems — the procedural quad builder used mid-step is
deleted the milestone the loader lands); *putting the preview transform table
inside OGFx* (world placement is scene/level ownership, not model data).

### D9. Ownership, lifetime, and bring-up order

- **`VulkanContext`** gains `VmaAllocator allocator` (created after the
  device, destroyed just before it). Nothing else changes.
- **`SceneData`** is a plain `main()` local (like `Camera`), loaded after
  frame-sync creation. It **stays alive for the program's lifetime** — step 3
  reads its instance transforms every frame.
- **`GpuScene`** (new owner): declared after `ctx`, created between
  `loadSceneData` and the AS build (the build consumes its device addresses).
  Own `vkDeviceWaitIdle` in the destructor (null-guarded, reverse creation
  order, per-resource teardown log lines), so ordering relative to sibling
  owners is immaterial; failure paths stay bare `return 1;`. All startup
  uploads borrow frame 0's command buffer + fence and return the fence
  signaled — the same borrow-and-restore contract the AS build follows; the
  upload submissions and the AS build run sequentially on the same borrowed
  slot.
- **`AccelerationStructure`** generalizes: `std::vector<BlasEntry> blases`
  (`{VkAccelerationStructureKHR blas; VkBuffer buffer; VmaAllocation
  allocation; VkDeviceAddress address;}`), singular TLAS + instance buffer +
  one shared scratch arena (freed post-build; destructor-guarded, as today).
  `buildAccelerationStructures` gains `VmaAllocator`, `const SceneData&`, and
  `const GpuScene&` parameters and drops its file-private triangle data. It
  stays **swapchain-independent** and startup-only; the trailing-barrier
  contract (no per-frame AS barrier) is unchanged. The TLAS instance buffer
  stays **host-visible + persistently mapped, deliberately**: step 3 rewrites
  instance transforms from the CPU every frame, so per-frame-slot instance
  buffers become a duplication of an existing shape rather than a redesign.
  Destructor order: all AS handles → all backing/scratch/instance buffers;
  `device`, the borrowed `VmaAllocator` (the destructor now frees through
  VMA — D3), and `destroyAccelerationStructure` adopted before first
  creation, exactly as today, vectorized.
- **`RtPipeline`**: descriptor set layout/pool grow (bindings 2–4);
  `buildShaderBindingTable` takes `const SceneData&` and the `VmaAllocator` —
  the one `RtPipeline` function that allocates through VMA — and adopts the
  allocator into the owner *before* creating the SBT buffer, so a partial
  failure still tears down through it (D3; the same adopt-before-create
  contract `AccelerationStructure` uses for its destroy function). The SBT remains
  program-lifetime and host-visible (unchanged rationale). New function
  `writeSceneDescriptorSet(VkDevice, VkDescriptorSet, const GpuScene&)` writes
  bindings 2–4 **once at startup**; `writeRtDescriptorSet` keeps exactly its
  current signature and recreate-path contract (bindings 0–1, resize-bound) —
  the recreate path is untouched by this step.
- **`Renderer` view: gains nothing.** Scene resources are reached through
  descriptors and the SBT regions it already passes opaquely; the copied
  `tlas` handle stays legal because the TLAS remains built-once,
  program-lifetime. (Step 3 must convert it to a borrowed pointer — noted at
  the field and in §8, not done now.)
- **`main()` order:** … createLogicalDevice → loadRayTracingFunctions →
  queues → **createAllocator** (before `Swapchain`, whose storage image now
  allocates through VMA) → `Swapchain` → command pool → frame sync →
  `SceneData sceneData; loadSceneData(...)` →
  `GpuScene gpuScene; createGpuScene(...)` →
  `buildAccelerationStructures(..., sceneData, gpuScene, ...)` →
  `createRtDescriptorSet` → `createRtPipeline` →
  `buildShaderBindingTable(..., ctx.allocator, sceneData)` → `writeSceneDescriptorSet` →
  `Renderer` → `prepareRtForSwapchain` → loop.

### D10. Shader module: one Slang file, renamed; BDA via uint64 + typed pointer casts; world-space normals

**Decision:** one Slang module, renamed `shaders/triangle.slang` →
**`shaders/raytrace.slang`** (embed name `raytrace_spv`; the CMake custom
command's OUTPUT/DEPENDS/COMMENT and the `sed` include-prepend post-step move
with it, in one mechanical commit). No Slang `import` this step ⇒ the
slangc-dependency-file CMake wiring stays deferred exactly as ARCHITECTURE.md
specifies. Entry points after this step: `rayGenMain`, `missMain`,
`closestHitMain`, `anyHitMain`.

**BDA mechanics** (slangc has native pointer support): GPU structs mirror the
C++ ABI structs; addresses are carried as `uint64_t` and cast to typed
pointers at use (this is what obligates `shaderInt64` — §4.3), pinning layout
responsibility on the all-scalar/explicit layouts + `static_assert`s rather
than on pointer-member layout rules.
`VertexAttributes` is **all-scalar on both sides** (five floats, 20 bytes) —
std430, scalar, and natural layouts coincide for all-scalar structs, so the
ABI is layout-rule-proof without wasted padding bytes:

```slang
struct VertexAttributes { float nx, ny, nz, u, v; }   // 20 B — matches the C++ struct exactly
struct GeometryRecord   { uint64_t indexAddress; uint64_t positionAddress;
                          uint64_t attributeAddress; uint materialIndex; uint pad0; }   // 32 B
struct MaterialRecord   { float4 baseColorFactor; uint baseColorTexture;
                          float alphaCutoff; uint pad0; uint pad1; }                    // 32 B

[[vk::binding(2, 0)]] StructuredBuffer<GeometryRecord> geometryRecords;
[[vk::binding(3, 0)]] StructuredBuffer<MaterialRecord> materials;
[[vk::binding(4, 0)]] Sampler2D sceneTextures[1024];   // == MaxSceneTextures; the pairing is pinned by a comment on both sides
```

Shared file-local hit-attribute fetch (used by closest-hit and any-hit):

```slang
GeometryRecord g = geometryRecords[InstanceID() + GeometryIndex()];
uint* indices = (uint*)g.indexAddress;                       // 4-byte-aligned loads by construction (D4)
uint  base    = 3 * PrimitiveIndex();
uint3 tri     = uint3(indices[base], indices[base + 1], indices[base + 2]);
VertexAttributes* attrs = (VertexAttributes*)g.attributeAddress;   // 20-byte all-scalar stride
float3 w = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
                  attribs.barycentrics.x, attribs.barycentrics.y);
// ... interpolate normal / uv from attrs[tri.x..z] with weights w
```

**Normals are transformed object → world with the inverse-transpose** —
non-negotiable, because rotated instances arrive in the very milestone whose
visual oracle depends on shading looking right, and content transforms may
carry non-uniform scale. `WorldToObject3x4()`'s 3×3 block is the plain inverse of the
object-to-world 3×3, and HLSL's row-vector `mul(n, M)` applies the matrix's
transpose — so the pair below yields exactly the inverse-transpose. (The
tempting `4x3` variant is the transpose of the `3x4` one, so it would silently
apply the *plain inverse* — rotations applied backwards to normals; this is
the formula NVIDIA's DXR references use.)

```slang
float3 worldNormal = normalize(mul(objectNormal, (float3x3)WorldToObject3x4()));
```

Any-hit (texture-milestone form; the preceding split milestone's checkerboard
placeholder replaces only the alpha source):

```slang
[shader("anyhit")]
void anyHitMain(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    // uv + material via the shared fetch; index 0 is the opaque-white fallback (D7), so no branch
    float alpha = m.baseColorFactor.a
                * sceneTextures[NonUniformResourceIndex(m.baseColorTexture)].SampleLevel(uv, 0).a;
    if (alpha < m.alphaCutoff)
        IgnoreHit();   // reject this hit; traversal continues
}
```

Raygen changes, milestone-pinned: `TMax` rises to `1.0e4` when real converted
scene extents first require it; the revised M4 quad does not need that change.
The `RAY_FLAG_FORCE_OPAQUE` → `RAY_FLAG_NONE` flip and the geometry-multiplier
argument → `RayTypeCount` land atomically in the opaque/alpha split milestone
together with the per-geometry hit records and instance SBT offsets: the three
are one indivisible SBT-selection change. Applying the multiplier against the
generalization milestone's still-one-record hit region would index outside
that region entirely. Closest-hit shading this step:
`payload.color = baseColor.rgb * (0.2 + 0.8 * abs(dot(worldNormal, -WorldRayDirection())))`
— a debug visualization replaced wholesale by step 4. The payload stays
`{ float3 color; }`; the `[format("rgba8")]` storage-image attribute and the
push-constant ABI are untouched.

**Rejected:** splitting into multiple Slang files now (forces the CMake
dep-file work early for a module that fits comfortably in one file; the split
waits for path-tracing shader growth); pointer-typed struct members (moves the
ABI onto Slang pointer-layout rules instead of asserted scalar offsets);
padded 32-byte vertex attributes (12 dead bytes per vertex to dodge layout
rules the all-scalar struct already dodges for free).

## 4. Target architecture (post-step)

### 4.1 CPU scene model — [src/scene.hpp](src/scene.hpp), Vulkan-free

```cpp
// The vertex shading attributes, all-scalar so std430/scalar/natural layouts
// coincide — this struct is the shader ABI (see raytrace.slang twin).
struct VertexAttributes {
    float nx, ny, nz;   // object-space normal
    float u, v;
};
static_assert(sizeof(VertexAttributes) == 20);

struct SceneGeometry {            // one geometry range == one BLAS geometry == RayTypeCount SBT hit records
    uint32_t firstVertex;         // element index into positions/attributes
    uint32_t vertexCount;         // feeds triangles.maxVertex = vertexCount - 1
    uint32_t firstIndex;          // element index into indices (mesh-local values stored)
    uint32_t indexCount;
    uint32_t materialIndex;
    bool alphaTested;             // the geometry class — drives hit-group choice and the OPAQUE geometry bit
};
struct SceneMaterial {
    float baseColorFactor[4];
    uint32_t baseColorImage;      // index into images; 0 == generated 1x1 white fallback
    float alphaCutoff;
};
struct SceneMesh     { uint32_t firstGeometry; uint32_t geometryCount; };  // one BLAS
struct SceneInstance { uint32_t meshIndex; glm::mat4 transform; };         // one TLAS instance, world-space
struct SceneImage    { uint32_t width; uint32_t height; std::vector<uint8_t> pixels; };  // decoded RGBA8

struct SceneData {                // plain value, owned by main() — alive all program (step 3 reads transforms)
    std::vector<float> positions;              // packed float3, all geometries concatenated
    std::vector<VertexAttributes> attributes;  // same order
    std::vector<uint32_t> indices;             // mesh-local, widened to uint32
    std::vector<SceneGeometry> geometries;     // flat — THE index space of D5/D6
    std::vector<SceneMesh> meshes;
    std::vector<SceneInstance> instances;
    std::vector<SceneMaterial> materials;
    std::vector<SceneImage> images;            // [0] is the generated fallback
};

bool loadSceneData(SceneData* scene, const char* path);
```

### 4.2 GPU scene — [src/gpu_scene.hpp](src/gpu_scene.hpp)

```cpp
// GPU record ABIs; the static_asserts are the shader contract (twins in raytrace.slang).
struct GeometryRecord {                 // 32 B — binding 2, indexed by InstanceID() + GeometryIndex()
    VkDeviceAddress indexAddress;       // 0  — uint32 index 0 of this geometry's range (pre-offset, D4)
    VkDeviceAddress positionAddress;    // 8  — float3 position of this geometry's vertex 0
    VkDeviceAddress attributeAddress;   // 16 — VertexAttributes of this geometry's vertex 0
    uint32_t materialIndex;             // 24
    uint32_t pad0;                      // 28
};
static_assert(sizeof(GeometryRecord) == 32);

struct MaterialRecord {                 // 32 B — binding 3, indexed by materialIndex
    float baseColorFactor[4];           // 0
    uint32_t baseColorTexture;          // 16 — texture array index; 0 == fallback white (never a sentinel)
    float alphaCutoff;                  // 20
    uint32_t pad0, pad1;                // 24 — the class needs no GPU flag: the SBT already selected the hit group
};
static_assert(sizeof(MaterialRecord) == 32);

constexpr uint32_t MaxSceneTextures = 1024;   // gated against maxPerStageDescriptorSampledImages at device selection

struct SceneTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
};

// Null-initialization is the teardown contract, not tidiness: the destructor's
// null guards are what let a partial createGpuScene failure bare-return.
struct GpuScene {                                  // RAII owner
    VkDevice device = VK_NULL_HANDLE;              // borrowed
    VmaAllocator allocator = nullptr;              // borrowed
    VkBuffer positionBuffer = VK_NULL_HANDLE;
    VkBuffer attributeBuffer = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkBuffer geometryRecordBuffer = VK_NULL_HANDLE;
    VkBuffer materialBuffer = VK_NULL_HANDLE;
    VmaAllocation positionAllocation = nullptr;
    VmaAllocation attributeAllocation = nullptr;
    VmaAllocation indexAllocation = nullptr;
    VmaAllocation geometryRecordAllocation = nullptr;
    VmaAllocation materialAllocation = nullptr;
    VkDeviceAddress positionBufferAddress = 0;
    VkDeviceAddress attributeBufferAddress = 0;
    VkDeviceAddress indexBufferAddress = 0;
    std::vector<SceneTexture> textures;            // [0] fallback white
    VkSampler sampler = VK_NULL_HANDLE;

    GpuScene() = default;
    GpuScene(const GpuScene&) = delete;            // every owner deletes copies —
    GpuScene& operator=(const GpuScene&) = delete; // the existing convention
    ~GpuScene();                                   // own vkDeviceWaitIdle, null-guarded, reverse order
};

VkResult createGpuScene(GpuScene* gpu, const SceneData& scene,
    VkDevice device, VmaAllocator allocator,
    const RayTracingFunctions& functions,
    VkCommandBuffer commandBuffer, VkQueue traceQueue, VkFence fence);
```

`geometryRecordBuffer` / `materialBuffer` are device-local
`STORAGE_BUFFER | TRANSFER_DST` (descriptor-bound; no BDA needed). All five
buffers and every texture go through the D4 staged-upload path with its
trailing visibility barrier.

### 4.3 Descriptor set and device features

Set 0: **0** TLAS (raygen) · **1** storage image (raygen) · **2**
`GeometryRecord` SSBO (CH|AH) · **3** `MaterialRecord` SSBO (CH|AH) · **4**
combined-image-sampler[`MaxSceneTextures`], every slot written (CH|AH). Pool
sized accordingly, `maxSets = 1`, still no `FREE_DESCRIPTOR_SET_BIT`. Bindings
0–1 rewritten per recreate (unchanged path); 2–4 written once at startup.

Device features land in two installments, each in the milestone whose shaders
first need it:

- **M3b:** `VkPhysicalDeviceFeatures2::features.shaderInt64 = VK_TRUE` enters
  both the suitability query and the enable chain (and the rejection report).
  Carrying buffer device addresses as `uint64_t` in shader code makes the
  SPIR-V module declare the `Int64` capability, and using 64-bit integer types
  in shaders without the feature enabled is invalid API usage — the feature is
  owed to the *first BDA fetch*, not to the later feature-chain consolidation.
- **Texture milestone:** `VkPhysicalDeviceVulkan12Features` enters both chains carrying
  `bufferDeviceAddress = VK_TRUE` and
  `shaderSampledImageArrayNonUniformIndexing = VK_TRUE`. **The standalone
  `VkPhysicalDeviceBufferDeviceAddressFeatures` struct is removed in the same
  commit — a spec obligation, not tidiness: chaining both structs together is
  forbidden once `Vulkan12Features` is present.** The rejection report gains
  the new feature bits and the descriptor-limit gate (D7).

Extension list unchanged; `vkGetBufferDeviceAddress` stays resolved by its
core name.

### 4.4 Frame path

Untouched. `drawFrame`, all barriers, the fence discipline (`vkResetFences`
placement — no new failure path between reset and submit; all new upload work
happens pre-loop on the borrowed frame-0 slot), `prepareRtForSwapchain`, and
the recreate flow are identical. Push constants unchanged — 64 guaranteed
bytes still spare.

## 5. Milestones

Every milestone builds, runs, renders something on-screen-verifiable, and
exits with silent validation layers (debug preset). ARCHITECTURE.md sections
and the CLAUDE.md summary update inside each milestone that changes a
documented contract (see §10).

**M1 — VMA becomes the allocator (triangle unchanged).**
Files: `third_party/vma/vk_mem_alloc.h` + LICENSE (new),
`src/third_party_impl.cpp` (new), [CMakeLists.txt](CMakeLists.txt),
[src/vulkan_context.hpp](src/vulkan_context.hpp)/[.cpp](src/vulkan_context.cpp)
(allocator member + `createAllocator` + `createBuffer` rewrite,
`findMemoryType` deleted), `src/vma_fwd.hpp` (new — the forwarding header
per D3),
[src/swapchain.hpp](src/swapchain.hpp)/[.cpp](src/swapchain.cpp)
(`storageImageMemory` becomes a `VmaAllocation` and the struct borrows the
allocator for its teardown path, so
`createSwapchainResources`/`recreateSwapchain` gain the allocator parameter —
which also touches the render loop's recreate call in `main.cpp`; the storage
image moves to `vmaCreateImage`),
[src/acceleration_structure.hpp](src/acceleration_structure.hpp)/[.cpp](src/acceleration_structure.cpp),
[src/rt_pipeline.hpp](src/rt_pipeline.hpp)/[.cpp](src/rt_pipeline.cpp)
(`VkDeviceMemory` → `VmaAllocation`; SBT/host writes via persistent mapping),
[src/main.cpp](src/main.cpp).
Exit: triangle renders pixel-identical; validation clean; **grep gate:**
`grep -rn vkAllocateMemory src/` returns nothing (VMA's implementation TU
excluded); teardown log lines confirm reverse-order destruction.

**M2 — GLM adopted; the instance-transform path proven.**
Starts with the environment step: install `libglm-dev`; record the
prerequisite in [CLAUDE.md](CLAUDE.md)'s Build section and README.
Files: `CMakeLists.txt` (find_package glm, target-name variance handled once
with a comment), [src/camera.hpp](src/camera.hpp)/[.cpp](src/camera.cpp)
(`Vec3` → `glm::vec3`; every ABI static_assert kept),
`src/acceleration_structure.cpp` (file-private `toVkTransformMatrix` +
hardcoded instance transform: rotate 45° about Z, translate +0.5 X).
Exit: the triangle appears rotated 45° CCW and shifted right, matching a
hand-predicted image — the row-major transpose is proven before N instances
exist. Validation clean.

**M3 — staged uploads, then BDA attribute fetch (two runnable halves).**
M3 carries the step's two independent silent-failure classes — staging-upload
visibility and the Slang BDA/layout ABI — so it lands as two separately
verifiable halves so one oracle never has to disambiguate both. The shared
first commit is the mechanical rename `shaders/triangle.slang` →
`shaders/raytrace.slang` + CMake embed-name changes — separated so the heavy
diffs contain no rename noise.

**M3a — the upload path in isolation (triangle unchanged).**
`uploadDeviceLocalBuffer` lands in `vulkan_context.{hpp,cpp}` **including the
trailing TRANSFER → AS_BUILD | RAY_TRACING_SHADER memory barrier (D4)**, and
the existing triangle's position/index buffers move to staged device-local
memory; shaders and shading untouched.
Files: `src/vulkan_context.{hpp,cpp}`, `src/acceleration_structure.cpp`.
Exit: the M2 triangle renders pixel-identical off device-local memory;
validation clean **including a synchronization-validation run** — this half
exists to prove a submission-crossing transfer barrier, so the layer feature
built to see missing barriers must watch it. A missed transfer barrier or
broken staging path is isolated here, against a known-good shader side.

**M3b — the BDA/ABI unknowns on trivial content (single procedural quad).**
The triangle becomes an indexed **quad** (4 vertices, 6 indices — proves
shared-vertex indexing) with normals/UVs, built by a file-private procedural
`SceneData` builder (one mesh, one instance, one material — scaffolding
migrated into the revised M4 front end to the shared writer). New TUs `src/scene.*` and
`src/gpu_scene.*` per D8
(§4.1/§4.2, no textures yet); unified position/attribute/index buffers +
GeometryRecord/Material buffers; AS build consumes `sceneData`/`gpuScene`
(still 1 BLAS); `shaderInt64` enters the feature query/enable chains and the
rejection report (§4.3); descriptor bindings 2–3 with CH|AH stage flags +
`writeSceneDescriptorSet`; closest-hit does the shared BDA fetch and colors by
interpolated UV, already transforming the normal via the inverse-transpose
(D10). The procedural instance's rotation changes to **45° about the X axis**:
the transpose was proven at M2, and the quad's normal is Z-parallel — a Z
rotation leaves it invariant under both the right and the wrong transform, so
an off-normal axis is what makes normal-transform errors observable at all.
Files: `src/scene.{hpp,cpp}`, `src/gpu_scene.{hpp,cpp}` (new, + CMake source
list), `src/vulkan_context.cpp`, `src/acceleration_structure.{hpp,cpp}`,
`src/rt_pipeline.{hpp,cpp}`, `shaders/raytrace.slang`, `CMakeLists.txt`,
`src/main.cpp`.
Exit: a quad shaded as a smooth red/green UV gradient (a garbage BDA, stride,
or layout bug renders as noise, never subtly); then a one-line debug swap to
`payload.color = 0.5 + 0.5 * worldNormal` shows the hand-predicted tilted
normal of the X-rotated quad — catching inverse-vs-inverse-transpose confusion
before real meshes arrive (the swap is reverted before the milestone closes).
Validation clean **including a GPU-assisted validation run** — plain layers
cannot see BDA out-of-bounds reads, this half's dominant silent-failure class.
This retires the step's biggest unknowns — Slang pointers, layout ABI,
hit-stage descriptors — on trivial content.

**Revised M4 — OGFx round-trip.** [FORMATS.md](FORMATS.md) is authoritative
for its exact scope: move the procedural quad into an offline writer front end, produce
`test_quad.ogfx` through the first shared-compiler serializer, load and
validate that file at runtime, reconstruct the
model portions of `SceneData`, then have the M4 caller append one identity
preview instance (the reusable decoder returns no world placement).
The existing GPU/AS/RT path stays unchanged. Exit: the file-backed quad shows
the predicted red/green UV gradient; plain and GPU-assisted validation are
clean. This milestone proves the format boundary, not multi-object AS logic.

**Post-M4 N-BLAS / N-instance generalization — milestone number deferred.**
This rides the first real converted models. The format-independent
engineering recorded here applies when that generalization lands:

- `AccelerationStructure` vectorizes (`blases`); all BLAS builds batch into
  **one** `cmdBuildAccelerationStructures` call with per-BLAS ranges and one
  shared scratch arena sized
  `max(Σ aligned per-BLAS scratch, aligned TLAS scratch)`: the BLAS regions
  are concurrent (batched builds are unordered) so they *sum*, disjoint and
  each address-`alignUp`ed; the TLAS build then reuses the arena from its
  base — safe because existing barrier #1 orders the reuse. So: batched BLAS
  builds → barrier #1 → TLAS(`primitiveCount = N`) → barrier #2, contracts
  unchanged.
- Instances get `instanceCustomIndex = firstGeometry`, real transforms via
  `toVkTransformMatrix`, mask 0xFF. **Interim SBT contract — load-bearing
  for the generalization milestone's runnability:** instances set
  `instanceShaderBindingTableRecordOffset = 0` and the raygen `TraceRay`
  keeps its geometry multiplier at `0` until the opaque/alpha split milestone
  restructures the hit region — every instance routes to the single existing
  hit record; only `instanceCustomIndex` is populated here, so a mis-render
  is a loader/transform bug, never SBT routing. Closest-hit shading becomes
  baseColor × world-normal term; `TMax` rises to `1.0e4` when real probe
  extents arrive.
- Exit oracles: distinctly colored, distinctly placed objects at predicted
  positions; per-object color proves the
  `instanceCustomIndex + GeometryIndex()` identity; the twice-instanced
  rotated mesh shades correctly (proves the world-space normal transform
  under rotation). A deliberate edit to the source asset (move one box)
  shows up on screen (proves data flows from the file); the empty-parented box
  and the scaled-and-rotated sphere shade at their predicted places (proves
  hierarchy flattening and the inverse-transpose under real transforms).
  Validation clean **including a synchronization-validation run** (batched
  builds + TLAS scratch reuse); release preset built and run for a perf
  sanity check.

**Later — opaque/alpha-tested split: hit groups, per-geometry SBT, any-hit
(procedural alpha); milestone number deferred.**
Pipeline grows to 4 groups; the SBT hit region becomes per-geometry records
selected by class, all math via `RayTypeCount` (D6, including the
parameterized miss region); instances set
`instanceShaderBindingTableRecordOffset = firstGeometry * RayTypeCount`;
TraceRay multiplier = `RayTypeCount`; `RAY_FLAG_FORCE_OPAQUE` removed;
per-geometry `VK_GEOMETRY_OPAQUE_BIT_KHR` only for the opaque class; the
24-bit assert per D5. `anyHitMain` tests a UV-checkerboard alpha against
`alphaCutoff` — the one-milestone placeholder D7 justifies, deleted by the
following texture milestone.
Files: `src/rt_pipeline.{hpp,cpp}`, `src/acceleration_structure.cpp`,
`src/scene.cpp`, `shaders/raytrace.slang`, `CMakeLists.txt` (the
`XRPHOTON_RAY_TYPE_COUNT` definition feeding both compilers — D6).
Exit: checkerboard cutout — the box and miss background visible *through* the
card's rejected texels; the mixed mesh's opaque primitive unaffected (proves
`GeometryIndex()`-based SBT selection — the case per-instance schemes get
wrong); the opaque-only parts of the frame unchanged vs. the preceding
generalization milestone. Validation clean
**including a GPU-assisted validation run** (SBT record contents and
misrouting are invisible to plain layers). **Grep gate:**
`grep -rn FORCE_OPAQUE shaders/ src/` returns nothing.

**Later — textures + descriptor indexing and real alpha testing; milestone
number deferred.**
`VkPhysicalDeviceVulkan12Features` consolidation per §4.3 (standalone BDA
struct removed in the same commit) + rejection-report and
`maxPerStageDescriptorSampledImages` gating; binding 4 fixed-size array with
the 1×1 white fallback in slot 0 and every unused slot (D7); shared sampler;
image staging uploads with layout transitions ending at
`SHADER_READ_ONLY_OPTIMAL` / `RAY_TRACING_SHADER`; closest-hit samples
baseColor, any-hit samples texture alpha (checkerboard deleted).
Files: `src/vulkan_context.cpp`, `src/rt_pipeline.{hpp,cpp}`,
`src/scene.{hpp,cpp}`, `src/gpu_scene.{hpp,cpp}`, `shaders/raytrace.slang`.
Exit: textured scene; the card's cutout silhouette follows its texture's alpha
channel exactly; the two boxes show different textures (proves
`NonUniformResourceIndex` indexing). Validation clean **including GPU-assisted
validation** (descriptor-array misindexing class); release preset run.
ARCHITECTURE.md status/roadmap and the CLAUDE.md summary updated for the
landed step (§10).

## 6. Validation strategy

- **Always-on:** the debug preset (validation layers) must be silent at every
  milestone exit; the release preset is built and run at the N-BLAS
  generalization and texture milestones.
  Destructor logging verifies teardown order for the new vectors and owners.
  **"Silent" only counts if the layer actually loaded:** bring-up deliberately
  warns-and-continues when the layer is missing (`main.cpp`'s best-effort
  validation), so every milestone's validation run must confirm the absence of
  the "validation layer is not available" startup warning before its silence
  means anything.
- **Synchronization validation** (the Khronos layer's sync-validation
  feature) runs at **M3a and the N-BLAS generalization milestone** — the two
  milestones that add submission-crossing transfer/build hazards (staged
  uploads; batched builds + TLAS scratch reuse). It runs as a **separate
  pass** from GPU-assisted
  validation; the two features are not co-enabled.
- **GPU-assisted validation** runs at M3b, revised M4, the opaque/alpha split,
  and the texture milestone (enable the
  `VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT` feature of the Khronos layer
  via vkconfig or layer settings). It is the only tool that catches
  buffer-device-address out-of-bounds reads and descriptor-array
  misindexing — the two silent-garbage classes this step introduces, which the
  standard layers structurally cannot see.
- **Deliberate oracles per risk:** M2's predicted rotate+translate catches the
  `VkTransformMatrixKHR` transpose; M3a's pixel-identical triangle isolates
  staging-upload visibility; M3b's UV-gradient quad catches BDA/layout/stride
  bugs (they render as noise, never subtly) and its X-rotated instance +
  normal-visualization check catches a missing *or inverse-only* normal
  transform; revised M4's file-backed identity quad proves OGFx → `SceneData`
  → screen data flow; the later generalization's per-object colors, positions,
  and twice-instanced rotated mesh catch custom-index, transform, and
  normal-transform plumbing; the opaque/alpha split's cutout-in-front-of-a-box
  plus the mixed opaque+MASK mesh exercise `GeometryIndex()`-based SBT
  selection; the texture milestone's two textures on two boxes exercise
  `NonUniformResourceIndex`.
- **Layout-dispute tiebreaker:** on any suspected Slang pointee-layout
  mismatch, compile with `slangc -target spirv-asm` and diff the member
  `Offset` decorations against the C++ `static_assert`s — decides the dispute
  mechanically instead of by staring at noise.
- **Grep gates:** no `vkAllocateMemory` after M1; no `FORCE_OPAQUE` after the
  opaque/alpha split.
- **Guard rails preserved:** build-input base-address checks, scratch
  `alignUp` + slack, the SBT `alignmentDelta` CPU shift, division-form
  `roundUpToMultiple`, the `VK_SHADER_UNUSED_KHR` pre-fill for the new group,
  the callable region's valid address, the `vkResetFences` placement (all
  new upload work is pre-loop, on the borrowed slot), and the pinned
  `HOST_COHERENT` requirement on every VMA host-access allocation (D3 — no
  flush call sites exist to forget).
- **Loud failures:** compiler input errors and runtime loader errors (D8's
  engine-side list plus the FORMATS.md file-validation rules), the 24-bit cap (including
  the `RayTypeCount` multiplier), the scene-vs-device limit checks, and
  `MaxSceneTextures` overflow all report to `std::cerr` and abort startup —
  never silently degrade. The **documented degradations** are exhaustive:
  the offline compiler warns when mapping blend-mode materials to opaque or
  non-repeat/non-linear sampler states to the shared sampler; a future scene
  loader warns when skipping a zero-determinant instance (and errors if no
  renderable instance remains).

## 7. Risks and open questions

1. **Slang pointer codegen / layout mismatch** — the step's genuine unknown.
   Mitigated by all-scalar/asserted ABI structs, the `uint64_t`-address + cast
   style, 4-byte-aligned loads by construction, retiring the whole class in
   M3b on a quad, GPU-AV, and the spirv-asm tiebreaker.
2. **VMA + BDA interaction** — forgetting
   `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` makes every
   `vkGetBufferDeviceAddress` invalid; set at allocator creation, with the old
   usage→flag pairing comment moved there.
3. **SBT restructuring regressions** — the `alignmentDelta` shift and the
   division-form rounding are the two silent-garbage traps; both preserved by
   editing `buildShaderBindingTable` in place, with the split milestone's
   oracle scene making
   mis-selection visible and GPU-AV covering what it cannot.
4. **Batched BLAS scratch aliasing** — batched builds are unordered; the
   shared arena allocates disjoint per-BLAS regions, each address-`alignUp`ed,
   sized with per-region slack. The TLAS reuses the arena only *after*
   barrier #1 orders it (the N-BLAS generalization's sizing rule:
   `max(Σ aligned per-BLAS scratch, aligned TLAS scratch)`).
5. **GLM packaging variance** — `glm::glm` vs. legacy target names across
   distro versions; handled once in CMake, requirement documented (D2). Open
   question: none once `libglm-dev`'s exported target is confirmed at M2
   configure time.
6. **Asset-pipeline friction** — Blender export quirks (tangent-less normals,
   node scale, texture handling) move offline into the FORMATS.md
   add-on/compiler path, which validates and reports at convert time rather
   than at engine load. Open question: the exact export settings for the
   probe asset — resolved empirically when it is authored and recorded next
   to the asset.
7. **Blend-mode (translucent) content** — deliberately compiled as opaque
   with a conversion-time warning; no silent alpha-testing of blend materials.
8. **Scratch arena sizing — decided** (formerly an open question):
   `max(Σ aligned per-BLAS scratch, aligned TLAS scratch)`, the BLAS regions
   disjoint and aligned, the TLAS reusing the arena after barrier #1 (the
   N-BLAS generalization milestone).
   Driver-friendly chunking of very large batches has no consumer at this
   scene scale; step 3's refit sizing revisits if real content changes that.

## 8. Interaction with roadmap steps 3–5

- **Step 3 (dynamic scene):** the TLAS instance buffer is already
  host-visible + persistently mapped (D9), so per-frame-slot instance buffers
  duplicate an existing shape rather than redesign one. `Renderer.tlas` must
  become a borrowed pointer (documented today at the field); the frame path
  regains an AS barrier for refits (the no-frame-barrier contract is
  explicitly startup-build-only). Compute skinning deforms **positions and
  normals** — not positions alone — and frames in flight need per-slot
  storage for whatever it rewrites, so step 3 owns a real buffer/record
  design decision there: split normals into their own stream then, or give
  skinned meshes per-slot attribute copies — decided with the
  compute-skinning design in hand, per the trigger-based-engineering
  convention. What this step guarantees is the *seam*, not the answer:
  `GeometryRecord.positionAddress`/`attributeAddress` already indirect per
  geometry, so per-slot copies swap in by rewriting records (an ordinary
  storage-buffer update, no SBT change), and the position stream's density
  benefit for BLAS refit reads stands under either outcome. `SceneData` staying alive in `main()` is what gives step 3 its
  per-frame CPU transforms. Static BLASes keep `PREFER_FAST_TRACE`; refittable
  ones add `ALLOW_UPDATE` at their own creation sites.
- **Step 4 (path tracing):** the SBT scales by flipping the one CMake
  `XRPHOTON_RAY_TYPE_COUNT` definition to 2 (C++ and shader move together —
  D6) plus adding the shadow miss/any-hit shaders — every consuming site (offsets,
  miss-region size, instance sbtOffset, TraceRay multiplier, the 24-bit
  assert) already references the constant (D6). The TLAS stays a raygen-only
  binding (iterative bounces + NEE trace from raygen). Payload growth and
  push-constant additions have 64 guaranteed bytes of headroom. Texture LOD
  (mips, ray cones) is decided there, on top of this step's single-mip images.
- **Step 5 (accumulation/denoising):** a storage-image format change touches
  the Slang `[format]` attribute and the swapchain storage image together (the
  documented pairing); new passes reading the storage image must respect the
  trailing execution-barrier invariant — this step adds no reader after the
  blit, keeping that invariant trivially intact.

## 9. Deferred-work ledger (from ARCHITECTURE.md)

**Resolved by this step:** VMA adoption (M1); staging uploads for device-local
geometry (M3a); explicit aligned suballocation for AS build inputs (M3b —
by element-granular construction, D4). The build-input residency question gets
its answer too: positions/indices stay resident for good — hit shaders read
them every frame and step 3's refits will consume them.

**Still deferred, deliberately:** the slangc dependency-file CMake wiring —
the trigger (a Slang `import`) does not fire this step. De-risking note for
when it does: slangc's `-depfile` flag is present in the pinned toolchain
(verify against the installed 2026.x release at that time) and CMake ≥ 3.24
supports `DEPFILE` on `add_custom_command`, so the wiring is mechanical.
Presentation-completion fences and the UI/compositing surface-usage questions
remain untouched by this step.

## 10. Documentation upkeep

Per the existing keep-in-sync convention: as each milestone lands, update
[ARCHITECTURE.md](ARCHITECTURE.md) — the Status section, the module map (two
new TUs + `third_party_impl.cpp`), the ownership model (allocator in
`VulkanContext`, the `GpuScene` owner, `SceneData` as a `main()` value), the
acceleration-structure and RT-pipeline sections (vectorized BLASes, four
groups, the `RayTypeCount` contract, bindings 2–4), and the roadmap/trigger
ledger (§9's resolutions) — and mirror the summary changes into
[CLAUDE.md](CLAUDE.md) (layout list, build prerequisites including
`libglm-dev`, and the Next-step pointer moving to roadmap step 3).
ARCHITECTURE.md remains the source of truth for landed runtime architecture
and status; FORMATS.md owns format contracts and asset-pipeline sequencing.
The texture milestone does the final pass that marks step 2 landed.
