# xrPhoton Asset Formats

This document is the source of truth for xrPhoton's asset-format direction —
the role [ARCHITECTURE.md](ARCHITECTURE.md) plays for engine architecture. It
records the settled decisions about how content reaches the runtime (OGFx,
OMFx, the shared offline asset compiler) and the proposed elaboration of those
decisions into concrete mechanism. OGFx v1's static core, canonical writer,
strict runtime decoder/adapter, offline probe front ends, narrow M4a
legacy-static adapter, narrow SoC v4 rigid-compound adapter, optional
backend-neutral rigid-physics records, narrow headless Blender 5.1.x
static-mesh adapter, generic multi-model gallery consumer, and DDS-backed
opaque base-color path now exist; broader source profiles, live physics, later
format families, and SDK tools remain planned. The landed slices are recorded at the
[revised first OGFx milestone](#the-revised-first-ogfx-milestone-m4).

**How to read the labels.** Three kinds of statement appear below, and the
distinction is load-bearing:

- **DECISION** — settled by the project owner. Not up for re-litigation;
  implementation questions are resolved *within* these, never against them.
- **PROPOSED** — this document's elaboration of a decision into concrete
  mechanism (exact header fields, chunk numbering, hash choice, …). Refine
  freely at implementation time as long as the parent decision holds.
- **IMPLEMENTED** — a concrete mechanism that has landed and is pinned by code
  and tests. Change it deliberately as a versioned contract, not as draft prose.

Legacy-format archaeology below was checked against the
[OpenXRay](https://github.com/OpenXRay/xray-16) engine sources and the
[xray_re tools](https://github.com/abramcumner/xray_re-tools) format headers.
Claims that could not be pinned to a source are marked *(verify against
OpenXRay sources at implementation time)* — the converter asserts nothing
about the legacy formats that its test corpus has not confirmed.

## Overall engine direction

**DECISION.** xrPhoton is a **selective modernization** of X-Ray, not a
from-scratch engine:

- Replace the rendering pipeline with the Vulkan hardware ray tracing renderer
  this repository is building.
- Preserve useful X-Ray systems and behavior.
- Expand systems such as A-Life and AI instead of rewriting them
  unnecessarily.
- Reuse original animations except where new content — new weapons, for
  example — requires additional animations.
- Replace or significantly modify a subsystem only when there is a concrete
  reason.
- Continue development while waiting for GSC's response.

The goal is not to build every part of a game engine from scratch. It is to
preserve what gives X-Ray and S.T.A.L.K.E.R. their identity while modernizing
the parts that limit the project. Every section below is an application of
that rule.

## Asset-format direction

**DECISION.** The runtime uses **modernized X-Ray formats**, not several
unrelated formats loaded simultaneously. Original assets are converted
automatically, offline:

```
original .ogf  ──►  automated converter  ──►  modern .ogfx  ──►  xrPhoton runtime
original .omf  ──►  automated converter  ──►  modern .omfx  ──►  xrPhoton runtime   (if OMFx happens — see below)
```

The original files remain the **source assets**; generated files can always be
rebuilt from them. This is the property that keeps the converter honest — a
converter bug is fixed and the outputs regenerated, never hand-patched — and
it is what lets the compiler skip unchanged assets by hash rather than by
guesswork.

The same offline boundary applies to modern content:

```
Blender  ──►  xrPhoton export front end  ──►  shared asset compiler  ──►  .ogfx / .omfx
```

The runtime loads exactly one model format: OGFx. No interchange format is a
runtime dependency or runtime loading path. Blender's direct xrPhoton export
path is primary; an optional GLB importer remains an allowed offline adapter
if a concrete tooling need later justifies it. Either way, only the shared
asset compiler writes OGFx (see
[Blender and external assets](#blender-and-external-assets)).

## OGFx

**DECISION.** The model format:

- **Name:** OGFx — Object Geometry Format Extended
- **Extension:** `.ogfx`
- **File magic:** `OGFX`

OGFx is an **evolution of OGF**, not an unrelated free-form format. The rule
governing every field-level choice: **preserve an original concept when it
still works; change it only when a demonstrated engine or tooling requirement
demands it.**

### The OGF heritage

OGF version 4 is the model format shipped across the S.T.A.L.K.E.R. releases
(version 3 appears in earlier builds; the first converter profile accepts only
its pinned version 4 input and rejects every other version). It is a
chunk-oriented binary container: a flat sequence of `{u32 id, u32 size}` chunks,
some containing nested sub-chunks. The chunk vocabulary, verified against
xray_re's `xr_ogf_format.h`:

| Id | Chunk | Contents |
|----|-------|----------|
| `0x1` | `OGF_HEADER` | `u8` format version (4), `u8` model type, `u16` shader id, AABB, bounding sphere |
| `0x2` | `OGF_TEXTURE` | texture name followed by engine-shader name, each a NUL-terminated byte string |
| `0x3` | `OGF_VERTICES` | `u32` vertex format, `u32` vertex count, packed vertex array |
| `0x4` | `OGF_INDICES` | `u32` index count, then **16-bit** (`u16`) indices |
| `0x6` | `OGF_SWIDATA` | four reserved `u32` values, a `u32` window count, then slide-window records: `{u32 offset, u16 tris, u16 verts}` |
| `0x9` | `OGF_CHILDREN` | child visuals of hierarchical models, each an embedded OGF |
| `0xD` | `OGF_S_BONE_NAMES` | bone names and hierarchy (per-bone name, parent, bind OBB); the SoC v4 subset used by the landed rigid-barrel profile is pinned |
| `0xE` | `OGF_S_MOTIONS` | embedded skeletal motions — the same layout `.omf` externalizes (see [OMFx](#omfx)) |
| `0xF` | `OGF_S_SMPARAMS` | motion parameters: `u16` version (3 or 4), bone partitions, per-motion definitions |
| `0x10` | `OGF_S_IKDATA` | per-bone physics/IK metadata: shape, joint data, bind transform, mass, center; the SoC v4 rigid-cylinder subset is pinned |
| `0x11` | `OGF_S_USERDATA` | free-form ltx-style text blob |
| `0x12` | `OGF_S_DESC` | provenance: source file, export tool + build time, owner/creation time, modifier/modification time |
| `0x13` | `OGF_S_MOTION_REFS_0` | external motion references (one string; `0x18`/`_1` is the Clear-Sky+ multi-string variant) |
| `0x17` | `OGF_S_LODS` | LOD linkage *(reference string vs. embedded model: verify)* |

Additional ids (`0x5` P_MAP, `0x7`/`0x8` V/ICONTAINER, `0xA` CHILDREN_L,
`0xB` LODDEF2, `0xC` TREEDEF2, `0x14` SWICONTAINER, `0x15` GCONTAINER,
`0x16` FASTPATH) serve compiled-level geometry sharing, billboard LOD
definitions, and tree/fastpath variants. Strings throughout are
null-terminated. The generic X-Ray chunk reader treats the high bit of a chunk
id as a compression mark *(whether any shipping OGF uses it: verify)*.

The `OGF_HEADER` model type is what selects the visual class:

| Value | Type |
|-------|------|
| `0` | normal (static mesh) |
| `1` | hierarchy (container of child visuals) |
| `2` | progressive (slide-window LOD mesh) |
| `3` | skeletal, animated (`SKELETON_ANIM`) |
| `4`/`5` | skeletal geometry parts (`SKELETON_GEOMDEF_PM`/`_ST`) |
| `6` | billboard LOD |
| `7`/`0xB` | tree (static / progressive) |
| `8`/`9` | particle effect / group |
| `0xA` | skeletal, rigid (`SKELETON_RIGID`) |
| `0xC` | 3D fluid volume (`3DFLUIDVOLUME`) |

Vertex data is packed in D3D-FVF-style formats selected by the `u32` at the
head of `OGF_VERTICES`: `0x112` (the D3DFVF `XYZ|NORMAL|TEX1` value) for
static position/normal/uv vertices, and dedicated skinned layouts —
`0x12071980` (1-link: position, normal, tangent, binormal, uv, `u32` bone
index) and `0x240E3300` (2-link: two `u16` bone indices, position, normal,
tangent, binormal, `f32` weight, uv). 3/4-link skinning arrives in the Clear
Sky / Call of Pripyat era as the small-integer `_CS` format values
`0x1`–`0x4` (covering 1–4 links); OpenXRay's `FMesh.hpp` also defines
multiplied constants (`0x36154C80`, labeled N-link, plus `0x481C6600` =
3-link and `0x5A237F80` = 4-link) whose real-asset use is unclear *(verify
against a real asset corpus at implementation time)*.

**IMPLEMENTED — narrow M4a source profile.** The first legacy adapter accepts
exactly one flat, uncompressed instance of `OGF_HEADER`, `OGF_TEXTURE`,
`OGF_VERTICES`, and `OGF_INDICES`, in any order, with no extra chunks. It pins
version `4`, normal/static type `0`, header shader id `0`, engine shader
`default`, FVF `0x112`, 32-byte position/normal/UV records, and `u16` triangle
indices. Both source strings must be nonempty printable ASCII of at most 255
bytes; this deliberately handles the ASCII corpus without pretending arbitrary
legacy code-page bytes are UTF-8. Other versions, types, encodings, shaders,
vertex formats, compression, and chunk families fail with a file/chunk/field
diagnostic rather than losing semantics.

Positions, normals, UVs, and triangle order pass through unchanged; indices are
widened to `u32`. Source AABB extrema must exactly match the vertex stream, and
the stored sphere must enclose it with the one-outward-`f32`-ULP tolerance needed
by the acceptance corpus. Each corner normal of a nondegenerate triangle must
agree with its unchanged right-handed winding. The adapter creates one opaque
geometry/mesh/material and preserves the logical texture name; the canonical
OGFx writer independently regenerates bounds and serializes it. The adapter does
not add a runtime OGF loading path.

**IMPLEMENTED — narrow SoC rigid-compound profile.** The generalized
`convert-ogf` entry point also accepts the exact regular-barrel family proven by
`meshes/physics/balon/bochka_close_1.ogf`: an uncompressed OGF v4 type-`0xA`
root containing exactly `HEADER`, `DESC`, `CHILDREN`, `S_BONE_NAMES`, and
`S_IKDATA`. Its children are contiguous embedded type-`5` visuals containing
the four required render chunks, one-link `0x12071980` 60-byte vertices, and
`u16` triangle indices. The source shader pair is pinned to header shader id
`0` plus `models\model`, mapped explicitly to opaque OGFx material semantics;
the acceptance asset preserves `mtl\mtl_barrel_01`.

The acceptance asset changed from the earlier `bochka_fuel` roadmap candidate
to `bochka_close_1` because this milestone is meant to prove an ordinary closed
barrel, not an explosive gameplay object. This was a content choice rather
than an adapter limitation: `bochka_fuel` also fits the profile, while SoC's
`explosive_barrel` configuration supplies its explosive class, blast, damage,
fuse, and sound behavior outside the OGF visual.

Bone names are nonempty printable ASCII, case-insensitively unique, and form
one rooted acyclic hierarchy. This first profile accepts only active cylinder
shapes, rigid nonbreakable joints, finite positive per-bone masses, and zero
bind rotations. Unsupported chunks, child types, vertex formats, shape flags,
shape types, joint types/flags, rotations, or malformed hierarchy fail with a
source chunk/field diagnostic rather than being discarded.

The render P/N/UV records and triangle order are already in bind/model space,
so they pass through unchanged and the validated one-link bone indices are not
retained as skinning data. The children flatten into an ordinary static OGFx
mesh (`modelType == 0`). Hierarchical bind translations are accumulated only
to place each cylinder center and per-bone center of mass in model space. The
result is one reusable 62-unit compound-body recipe with three named cylinder
colliders; it is metadata, not a live physics object.

What this heritage gets right — and what OGFx therefore **preserves**
(**DECISION**):

- Chunk-oriented binary structure
- Visual/model types
- Mesh and child-visual organization
- Skeleton and bone conventions
- Motion references
- Material and texture references
- Attachments, IK and bone metadata
- X-Ray asset organization

And its concrete limitations — what OGFx **modernizes** (**DECISION**):

- 32-bit indices (OGF's 16-bit indices cap a mesh at 65 536 vertices per
  range — the constraint behind much of the era's mesh splitting)
- Modern typed vertex streams
- Vulkan- and ray-tracing-friendly geometry
- BLAS-oriented geometry grouping
- Explicit versions and fixed-width fields
- Strong bounds and overflow validation
- Documented coordinate/layout conventions
- Optional extensible chunks
- Better error reporting
- Modern material and RT metadata
- Better support for large, detailed models

### Implemented version-1 container layout

**IMPLEMENTED** — this is the concrete version-1 contract used by M4. Field
widths, chunk ids, offsets, and validation rules below are pinned together: a
change updates this document and its test vectors with both sides of the
writer/loader pair.

Every field in the container is **little-endian and fixed-width**. There are
no variable-width integers and no host-dependent types anywhere in the format;
`f32` means IEEE-754 binary32.

**File header** (fixed, before any chunk):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | magic `OGFX` (bytes `4F 47 46 58`) |
| 4 | 4 | `u32` container version (starts at 1) |
| 8 | 4 | `u32` header size — the file offset of the first chunk; version 1 requires 16 |
| 12 | 4 | `u32` reserved, must be zero |

The header is fixed and minimal so the magic/version gate runs before any
chunk parsing: an unknown container version is rejected outright, never
"parsed optimistically".

**Chunk model.** After the header, a flat sequence of chunks — OGF's central
concept, kept. Each chunk header:

| Field | Type | Meaning |
|-------|------|---------|
| `id` | `u32` | chunk identifier |
| `flags` | `u32` | bit 0 = **required** (see below); remaining bits reserved, must be zero |
| `version` | `u32` | per-chunk payload version, gated independently of the container version |
| `reserved0` | `u32` | must be zero |
| `byteSize` | `u64` | exact payload size in bytes |
| `reserved1` | `u64` | must be zero |

The header is exactly 32 bytes. Every chunk header begins at a
**16-byte-aligned file offset** (the writer zero-pads between chunks), so
**every payload also begins 16-byte aligned**. The point of the guarantee: GPU-destined
streams can be handed from an mmapped or block-read file straight to the
staged-upload path without re-packing, and every fixed-stride record array is
naturally aligned for direct tooling access on the engine's supported
little-endian hosts.

**Required vs. optional chunks** — the PNG critical/ancillary idea, which is
what makes "optional extensible chunks" safe: a loader that encounters an
unknown chunk with the required bit set **must reject the file** (it cannot
render this model correctly); an unknown chunk without it is **skipped** using
`byteSize` alone. Old loaders thus survive new optional extensions, and new
required semantics fail loudly instead of half-loading.

**Strings** are length-prefixed UTF-8 — `u16` byte length, no terminator,
at most `4096` bytes, and always validated against the enclosing chunk. This
deliberately replaces OGF's null-terminated strings: a
length-prefixed string can be bounds-checked before it is read; a
null-terminated one can only be scanned and hoped about.

**Validation rules a loader enforces** (the modernized limitation "strong
bounds and overflow validation", made concrete — these are obligations, not
suggestions):

1. Magic and container version gate before anything else. Version 1 requires
   `headerSize == 16`, at least 16 file bytes, and a zero reserved header field.
2. Chunk iteration is exact. Let `payloadEnd = headerOffset + 32 + byteSize`,
   using checked 64-bit arithmetic. The aligned header and full payload must
   fit. If `payloadEnd == fileSize`, iteration ends. Otherwise
   `nextHeader = alignUp(payloadEnd, 16)`; every byte in
   `[payloadEnd, nextHeader)` is zero and a full 32-byte next header fits.
   The canonical writer emits no padding after the final payload, so all
   trailing bytes are rejected.
3. Every file/chunk-header reserved field, and every documented reserved field
   in a supported known payload, is zero. Known-chunk flags match the schema
   exactly: all seven version-1 required chunks use `flags == 1`; known
   optional chunks use `flags == 0`. Version-1 `modelFlags` is zero and
   `geometryFlags & ~1u` is zero. An unsupported required chunk version is
   rejected; an unsupported optional version is skipped, never parsed as an
   older layout. An id merely reserved for a future optional schema remains
   unknown until that schema lands, so the same optional skip rule applies.
4. Chunks required for the selected model type are present exactly once.
   Unknown required chunks are rejected by id; unknown optional chunks are
   skipped. Duplicates are rejected unless a chunk is documented repeatable.
5. Every `count × stride` computation runs in checked 64-bit arithmetic and
   matches the chunk's `byteSize` exactly; framed chunks use their documented
   size formula instead. Before allocation, the M4 loader enforces its initial
   1 GiB per-file and 4096-chunk sanity caps. That file cap plus the minimum
   record strides guarantees decoded element counts fit `u32` and, on supported
   hosts, the full file fits `size_t`; compile-time assertions pin the host
   invariant instead of retaining unreachable runtime branches. These are
   loader resource limits, not a reason to reinterpret a valid larger file.
6. Position, attribute, and material arrays are nonempty; position and
   attribute counts match. Every stored position, attribute, and material
   scalar is finite. Each normal's squared length, computed in `f64`, is
   finite and at least `1.0e-12`.
7. Every geometry range is checked: nonzero counts, `indexCount` divisible
   by 3, `firstVertex + vertexCount` and `firstIndex + indexCount` widened from
   their `u32` fields to `u64` before comparison with their streams, every index
   less than the range's `vertexCount`, and
   every `materialIndex` less than the material count. In geometry-array
   order, vertex ranges partition positions/attributes exactly once and index
   ranges partition indices exactly once: each first offset equals the prior
   end, beginning at zero, and the final end equals the stream count.
8. Every mesh has a nonzero geometry range widened to `u64` for its end check,
   and the mesh ranges partition the geometry array exactly once: no orphaned
   or multiply owned geometry. A loader rejects unsupported model types before
   reading type-specific data.
9. String lengths are bounded, valid UTF-8, and lie wholly within their
   enclosing chunks. A string arena is parsed from offset zero as checked
   `u16 length + length bytes` entries until exactly `stringByteSize`; a
   partial prefix, overrun, empty entry, or trailing byte is invalid.
10. Bounds are finite, each AABB minimum is no greater than its maximum, and
    each sphere radius is nonnegative. Every geometry AABB encloses every
    position in its vertex range; the model AABB and sphere enclose every
    model position. Enclosure tests use `f64`. The canonical writer chooses
    the sphere center component-wise with `std::midpoint` from the stored `f32`
    model-AABB minimum and maximum. It then computes the maximum squared distance
    in `f64` from that stored `f32` center to every stored `f32` position, uses its
    square root as the initial radius candidate, and advances to the next `f32`
    until the stored radius squared encloses that maximum in `f64`. AABBs likewise
    round outward. No tolerance or platform-dependent equality rule is needed.
11. Failures produce one diagnostic naming the file, chunk id, field, and the
    expected-versus-found values. Dependency-free library entry points return
    that diagnostic to their caller; the CLI or runtime boundary reports it to
    `std::cerr` once, per the engine's loud-failure convention.

### Geometry payload

**IMPLEMENTED** for the M4 static profile, designed around the engine that
exists. OGFx is a stable serialized schema decoded field-by-field; it never
dumps native C++ structs or inherits compiler padding. Its raw stream records
are deliberately byte-compatible with the corresponding
[`SceneData`](src/scene.hpp) arrays,
while range and material fields are decoded into engine-owned structures
before the existing [`GpuScene`](src/gpu_scene.hpp) BDA records are built.
The current CPU scene has no bounds/model-metadata owner; M4 validates those
fields and then discards them. A later consumer adds explicit CPU ownership
rather than smuggling serialized records into runtime structs.

| Id | Chunk | Required | Payload |
|----|-------|----------|---------|
| `0x0001` | `OGFX_MODEL` | yes | `u32` model type, `u32` model flags, model bounds (AABB + bounding sphere — the OGF header heritage) |
| `0x0010` | `OGFX_GEOMETRIES` | yes | fixed-width geometry-range records (below) |
| `0x0011` | `OGFX_MESHES` | yes | `{u32 firstGeometry, u32 geometryCount}` records — the BLAS grouping |
| `0x0012` | `OGFX_MATERIALS` | yes | material records: `f32[4]` baseColorFactor, `f32` alphaCutoff, logical texture reference string (carried into `SceneData` and resolved scene-globally as described below) |
| `0x0020` | `OGFX_POSITIONS` | yes | tightly packed `f32×3` positions, 12-byte stride |
| `0x0021` | `OGFX_ATTRIBUTES` | yes | 20-byte all-scalar attribute records: `nx, ny, nz, u, v` |
| `0x0022` | `OGFX_INDICES` | yes | `u32` indices, geometry-local |
| `0x0030` | `OGFX_RIGID_PHYSICS` | optional | backend-neutral compound-body records: contiguous collider ranges, masses/centers of mass, cylinder shapes, source material, and source-node names |
| `0x0040` | `OGFX_DESC` | optional | provenance (the `OGF_S_DESC` heritage: source asset, converting tool + version, stable source-provided timestamps) plus the complete-input hash the compiler used |

All seven required version-1 chunks use chunk version `1`, set the required
flag, and occur exactly once. The canonical writer emits them in ascending-id
order for deterministic output; the loader is order-independent. There is no
implicit record padding. The exact version-1 payloads are:

```text
OGFX_MODEL — exactly one 48-byte record
 0  u32 modelType          (0 = normal/static; all others unsupported in v1)
 4  u32 modelFlags         (must be zero in v1)
 8  f32 aabbMinX
12  f32 aabbMinY
16  f32 aabbMinZ
20  f32 aabbMaxX
24  f32 aabbMaxY
28  f32 aabbMaxZ
32  f32 sphereCenterX
36  f32 sphereCenterY
40  f32 sphereCenterZ
44  f32 sphereRadius

OGFX_GEOMETRIES — nonempty array, 48-byte stride
 0  u32 firstVertex
 4  u32 vertexCount
 8  u32 firstIndex
12  u32 indexCount
16  u32 materialIndex
20  u32 geometryFlags      (bit 0 = alpha-tested; all other bits zero in v1)
24  f32 aabbMinX
28  f32 aabbMinY
32  f32 aabbMinZ
36  f32 aabbMaxX
40  f32 aabbMaxY
44  f32 aabbMaxZ

OGFX_MESHES — nonempty array, 8-byte stride
0  u32 firstGeometry
4  u32 geometryCount

OGFX_POSITIONS — nonempty array, 12-byte stride
0  f32 x
4  f32 y
8  f32 z

OGFX_ATTRIBUTES — same element count as positions, 20-byte stride
 0  f32 nx
 4  f32 ny
 8  f32 nz
12  f32 u
16  f32 v

OGFX_INDICES — nonempty array, 4-byte stride
0  u32 geometryLocalIndex
```

`OGFX_MATERIALS` is the one framed required chunk rather than a pure array:

```text
payload header — 16 bytes
 0  u32 materialCount
 4  u32 stringByteSize
 8  u32 reserved0          (zero)
12  u32 reserved1          (zero)

material record — materialCount records, 32-byte stride
 0  f32 baseColorR
 4  f32 baseColorG
 8  f32 baseColorB
12  f32 baseColorA
16  f32 alphaCutoff
20  u32 textureRefOffset   (from string-arena start; UINT32_MAX = none)
24  u32 reserved0          (zero)
28  u32 reserved1          (zero)

string arena — stringByteSize bytes immediately after the records
```

Its exact size is `16 + materialCount × 32 + stringByteSize`, computed with
checked 64-bit arithmetic. The arena is a sequence of the length-prefixed
UTF-8 strings defined above. A texture offset must point to the `u16` prefix
of a known entry; empty entries and offsets into the middle of entries are
invalid because `UINT32_MAX` is the only no-texture representation. The
canonical writer validates every nonempty logical reference as at most 4096
UTF-8 bytes and interns byte-identical references. Distinct strings enter the
arena in first material-use order, so map/hash iteration order cannot affect
the output. The offline `decodeModelSchema` entry point validates this complete
static schema and reconstructs logical references; reserializing a model it
decoded produces the canonical arena form. To bound expansion from many
materials sharing one arena entry, the writer and both decoder profiles cap the
sum of reconstructed texture-reference byte lengths at 64 MiB, counting each
material reference; every canonical writer output is therefore accepted by
that decoder resource check. Runtime `decodeModel` reconstructs the same logical
references as `decodeModelSchema`; its remaining capability difference is the
opaque-only geometry gate.

`OGFX_RIGID_PHYSICS` is emitted only when both physics arrays are nonempty. It
uses chunk version `1`, `flags == 0` (optional), and this exact framing:

```text
payload header — 32 bytes
 0  u32 bodyCount
 4  u32 colliderCount
 8  u32 stringByteSize
12  u32 reserved[5]         (all zero)

body record — bodyCount records, 32-byte stride
 0  u32 firstCollider
 4  u32 colliderCount
 8  u32 reserved[2]         (both zero)
16  f32 mass                (finite and positive)
20  f32 centerOfMassX
24  f32 centerOfMassY
28  f32 centerOfMassZ

collider record — colliderCount records, 64-byte stride
 0  u32 shapeType           (1 = cylinder)
 4  u32 colliderFlags       (zero; reserved in version 1)
 8  u32 materialRefOffset   (UINT32_MAX = none)
12  u32 sourceNodeRefOffset (UINT32_MAX = none)
16  f32 centerX
20  f32 centerY
24  f32 centerZ
28  f32 axisX
32  f32 axisY
36  f32 axisZ
40  f32 height              (finite and positive)
44  f32 radius              (finite and positive)
48  f32 mass                (finite and positive)
52  f32 centerOfMassX
56  f32 centerOfMassY
60  f32 centerOfMassZ

string arena — stringByteSize bytes immediately after the records
```

The body ranges form one ordered, gap-free partition of every collider exactly
once. Centers, axes, and centers of mass are finite; axes are nonzero. Material
and source-node references use the same interned length-prefixed UTF-8 arena
discipline as material texture references, including the 4096-byte per-string
and 64 MiB reconstructed-string caps. Both physics arrays must be empty or both
nonempty. No physics-engine handles, motion type, world placement, or live
dynamics enter this chunk.

An unsupported future version of this optional chunk is safe to skip for
loading, but its opaque payload is not retained in the reconstructed `Model`.
Consequently, decoding such a file with an older compiler and serializing that
model again intentionally omits the skipped physics chunk: optional-version
forward compatibility is a render/load guarantee, not an archival
decode-and-rewrite guarantee.

The reference remains an extensionless logical name rather than image bytes in
the geometry container. The scene resolver accepts canonical X-Ray names made of
`[A-Za-z0-9_-]+` components separated by `\`, maps them exactly beneath the
configured texture root, appends `.dds`, and rejects traversal, alternate
separators, drive syntax, control bytes, and non-ASCII names. DDS DXT1/DXT5 mip-0
payloads remain BC-compressed and map to sRGB BC1/BC3 GPU images. Scene image 0 is
always a generated opaque-white RGBA8-sRGB fallback; untextured materials select
that real slot, while missing or invalid referenced files are loud startup
failures. This keeps OGFx geometry reusable and image-container policy separate.

The geometry, mesh, position, attribute, and index chunks are **pure arrays**:
their element counts derive from `byteSize / stride`. Animation skeletons, skin
weights, and motion references remain deferred. The landed rigid source profile
instead flattens its render hierarchy and preserves only its reusable compound
physics recipe in the optional chunk above.

`OGFX_DESC` currently reserves an id and purpose only. M4 does not emit or
interpret it; until its version-1 payload is pinned, writers omit it and
loaders handle it by the ordinary unknown-optional skip rule. Provenance and
cache-key policy are still binding compiler requirements even before that
metadata gains an on-disk representation.

The design directly mirrors the engine's data model:

- **Separate position and attribute streams.** `OGFX_POSITIONS` is exactly
  the BLAS build input: `VK_FORMAT_R32G32B32_SFLOAT`, 12-byte stride, no
  shading data interleaved — dense build-input reads, and the stream a future
  skinning pass rewrites without touching attributes. `OGFX_ATTRIBUTES` is
  byte-identical to the 20-byte all-scalar `VertexAttributes` record that
  [src/scene.hpp](src/scene.hpp) pins with a `static_assert` and that the
  `GpuScene` attribute buffer and
  [shaders/raytrace.slang](shaders/raytrace.slang) mirror field-for-field
  (the shader side has no compile-time check — the records are kept
  identical by hand). When the record grows (tangents for normal mapping),
  the chunk `version` field gates it.
- **32-bit indices, geometry-local.** Each range's indices count from its own
  vertex 0 — matching the pre-offset device addresses `GeometryRecord`
  carries, so the loader computes `firstVertex`/`firstIndex` offsets exactly
  the way `createGpuScene` already consumes them.
- **Triangle winding is counter-clockwise from the front.** For each nondegenerate
  stored triangle `(v0, v1, v2)`, the right-handed cross product
  `cross(v1 - v0, v2 - v0)` points toward the geometric front/outward side and
  therefore agrees with outward vertex normals. This is the convention already
  exercised by M3b's `0,1,2 / 0,2,3` +Z-normal quad; offline adapters own any
  source winding conversion. The generic writer preserves this as an input
  convention rather than trying to infer "outward": hard-edge normals, shared
  vertices, and intentional degenerate triangles do not admit one reliable
  format-level agreement test (a degenerate triangle has no orientation). Each
  source adapter validates or tests its known source convention before feeding
  the writer.
- **Geometry ranges map 1:1 to `SceneGeometry`.** Each record carries
  `firstVertex`, `vertexCount`, `firstIndex`, `indexCount`, `materialIndex`
  (all `u32`), a `u32` flags word whose bit 0 is the **alpha-tested class**,
  and a per-geometry AABB (`f32×6`). BLAS-oriented grouping and the
  opaque/alpha-tested split are therefore first-class file concepts: a mesh
  is one BLAS, its geometries are the BLAS's geometry list, and the class
  flag is what drives hit-group selection and the per-geometry
  `VK_GEOMETRY_OPAQUE_BIT_KHR` when the split lands (the roadmap's
  foliage-driven design axis — see ARCHITECTURE.md roadmap step 2).
- **The geometry flag is the one alpha-class authority.** Materials carry
  shading data such as `alphaCutoff`, but do not duplicate the alpha-tested
  classification. The loader copies geometry flag bit 0 into
  `SceneGeometry::alphaTested`; later AS and SBT construction consume that
  same value.
- **Per-geometry bounds** feed culling, streaming, and validation without
  re-deriving them from streams at load time; the compiler generates them
  (see [the compiler](#the-shared-asset-compiler)).
- **Element counts stay `u32`.** Streams are sized in `u64` bytes, but
  element indexing keeps the engine's `uint32_t` element convention — the
  24-bit `instanceCustomIndex` cap and descriptor-range gates already bound
  real content far below 2³².

OGFx contains no world-instance placement. It is a *model* format in the OGF
tradition; placement belongs to the eventual scene/level representation. The
preview gallery assembles decoded models and supplies their `SceneInstance`
placements. The reusable decoder itself returns no instances. This is runtime
bring-up policy, not serialized model data, and it disappears once a real scene
owner supplies instances.

The runtime profile accepts structurally valid multi-record mesh, geometry, and
material arrays plus logical texture references and their string arena. The
N-BLAS/N-instance consumer handles mesh and geometry ranges without a format-version
change, and scene-global texture resolution assigns the material image indices.

The opaque-only runtime profile rejects geometry flag bit 0. The current trace
uses `RAY_FLAG_FORCE_OPAQUE` and has no any-hit/SBT class split, so accepting an
alpha-tested record would silently render the wrong semantics. The later
opaque/alpha milestone removes that capability gate when it adds the actual
consumer.

Both schema and runtime byte decoders validate and reconstruct optional rigid
physics records. [`src/ogfx_loader.cpp`](src/ogfx_loader.cpp) intentionally
copies only render data into `SceneData` today, so the gallery barrel remains a
static placement: no backend is selected, no body is instantiated, and no TLAS
update or collision simulation is implied.

Container version 1 supports only the static `normal` model type. Heritage
model-type values remain documented and reserved, but the loader rejects them
until their required chunks and semantics are designed and implemented.

### Coordinate conventions

**DECISION.** Coordinate conversion happens **offline, in the shared
compiler**. The runtime loads exactly one documented convention — an `.ogfx`
file is already in engine space, and no loader-side axis fixups exist.

The engine convention is already in force and documented in ARCHITECTURE.md's
Camera section: **world Y-up**, with the camera at zero yaw/pitch looking down
**+Z** and world +X mapping screen-right. OGFx stores geometry in exactly this
space; triangle winding and normal orientation are documented next to the
geometry chunks when the writer lands.

The conversions the compiler owns:

- **Legacy X-Ray** is left-handed, Y-up, Z-forward (its Direct3D lineage) —
  the *same* arrangement ARCHITECTURE.md documents for the engine (Y-up, +Z
  forward at zero yaw, +X screen-right), so the expected legacy conversion
  is a coordinate **pass-through**, not a mirror. M4a and the rigid barrel pin
  unchanged render positions, normals, UVs, and triangle order against real SoC
  assets. For the rigid slice, child vertices are already bind/model-space;
  hierarchical bind translations affect only the local collider centers and
  centers of mass that the compiler flattens into model space. Animated
  skinning conventions remain to be pinned when that profile lands.
- **Blender** is right-handed, Z-up. The landed static-mesh adapter maps each
  transformed position and normal from `(x, y, z)` to engine `(x, z, y)`. That
  axis swap has determinant `-1`; the compiler therefore reverses triangle
  winding exactly when the complete object-plus-axis transform reverses
  orientation. Object-space normals use the object's inverse transpose before
  the same axis map and normalization. Scene-unit scaling and the affine object
  transform are baked offline; the runtime performs no Blender fixup.

### What is decided vs. proposed — summary

**DECISION:** name/extension/magic; evolution-of-OGF (the preserve list, the
modernize list, the preserve-unless-demanded rule); offline conversion with
originals as source assets; one runtime convention with offline coordinate
conversion. **PROPOSED:** every concrete number above — header field layout,
chunk ids and header shape, the 16-byte alignment constant, string encoding,
the exact record layouts, and the initial static-model support boundary.

## OMFx

**DECISION.** *If* animation storage needs modernization — this is
conditional, not committed — the format is:

- **Name:** OMFx — Object Motion Format Extended
- **Extension:** `.omfx`
- **File magic:** `OMFX`

OMFx is intended mainly as a **tooling and reliability improvement**, not an
animation overhaul. Original animations are reused (the engine-direction
decision above), so the bar for changing what the data *means* is
deliberately high.

### The OMF heritage

OMF is X-Ray's externalized skeletal-motion container: the same two chunks a
skeletal OGF can embed — motions (`0xE`) and motion parameters (`0xF`) —
written standalone so many models share one animation set. Verified against
the xray_re OGF v4 loader:

- **Motions chunk:** per-motion sub-chunks with null-terminated names. Per
  bone, a `u8` flags byte selects the rotation representation. Animated
  rotation stores a `u32` CRC32 followed by one packed quaternion per frame
  (4 × `s16`, dequantized by 1/32767) at 30 keys per second (xray_re's
  `OGF4_MOTION_FPS` / OpenXRay's `SAMPLE_FPS`, both 30). Rotation-absent
  tracks store one packed quaternion directly, with no preceding CRC. If the
  "translation present" flag is set: a `u32` CRC32, quantized translation
  keys (3 × `s8`, or 3 × `s16` under the Call-of-Pripyat-era high-quality
  flag), then a per-track `f32×3` scale (`t_size`) and offset (`t_init`)
  that dequantize them; otherwise a single constant `f32×3` translation.
- **Params chunk:** `u16` version (3 or 4), bone **partitions** (named bone
  groups for partial-body playback), and per-motion definitions — name,
  `u32` motion flags (fx `0x1`, stop-at-end `0x2`, no-mix `0x4`, sync-part
  `0x8`, use-footsteps `0x10`, root-mover `0x20`, idle `0x40`, weapon-bone
  `0x80`), `u16` bone-or-part, `u16` motion id, and `f32` speed / power /
  accrue / falloff. A converter preserves the complete `u32`, including
  unknown bits, rather than masking it to the currently named set. Version 4
  (Clear Sky / Call of Pripyat) adds named
  **motion marks** — time intervals on a motion *(exact mark layout: verify
  against OpenXRay sources at implementation time)*.

The CRCs exist in the original format but shipping engines treat them
incidentally; whether any runtime path validates them is exactly the kind of
reliability gap OMFx exists to close *(verify)*.

### What OMFx keeps and improves

**DECISION — preserve** (the data's meaning does not change): original
animation behavior; bone hierarchy and names; keyframes and interpolation;
motion names; bone partitions; looping and playback properties; weapon/HUD
conventions; script and configuration references.

**DECISION — improve** (the tooling around the data): Blender import and
export; validation; versioning; documentation; coordinate conversion;
missing-bone and skeleton-compatibility diagnostics.

**PROPOSED.** OMFx rides the same container machinery as OGFx — the same
fixed header shape (magic `OMFX`), chunk model, required/optional flag,
string encoding, and validation obligations. One container discipline, not
two (the "one focus" convention applied to file plumbing). The concrete
improvements this buys with almost no format invention: the heritage CRCs
become validated-and-reported instead of decorative; quantization parameters
become explicit documented fields rather than loader lore; and
missing-bone/skeleton-mismatch diagnostics get the named, specific error
reporting the validation rules already mandate. Whether key data stays
bit-identical to OMF's quantized tracks (likeliest, per the
preserve-behavior decision) or is re-quantized is an implementation-time
question *inside* the preserve list, not a license to redesign animation.

## Blender and external assets

**DECISION.** There is no glTF/GLB runtime loader. The primary modern-content
path is direct Blender export through the shared compiler:

```
Blender ──► xrPhoton export front end ──► shared asset compiler ──► OGFx / OMFx
```

**IMPLEMENTED — first headless Blender profile.** Blender 5.1.x runs
[`tools/blender/export_ogfx.py`](tools/blender/export_ogfx.py) against a `.blend`
file and one required object name. The script is an extractor, not an OGFx
writer: it validates the source, triangulates the evaluated mesh, records its
corner positions/normals and optional UVs, and supplies a bounded private
versioned `XRBM` stream to `xrPhotonAssetCompiler convert-blender` over stdin.
The standard-library-only [`src/blender_mesh.cpp`](src/blender_mesh.cpp) adapter
owns axis/normal/winding conversion and corner deduplication, populates the same
compiler model as the legacy adapter, and invokes the one canonical writer.
`XRBM` is a private producer/compiler exchange contract; it is neither a
persisted asset nor a runtime format.

The accepted profile is intentionally exact: one explicitly named active-scene
mesh object with no material slots or evaluated materials, modifiers, object or
mesh animation data, shape keys, constraints, parenting, or color attributes.
Linked-library data and overrides are also outside this self-contained source
profile. It accepts zero or one UV layer and rejects loose vertices/edges, degenerate
triangles, singular/non-finite transforms, non-finite corner data, and normals
that disagree with the converted winding. A one-million-triangle profile cap
bounds the extractor, stdin payload, deduplication table, and compiler model's
combined working set. It produces one opaque geometry, one mesh, and one default
material with no texture reference. This slice does not
flatten hierarchy, apply modifiers, export materials/textures, or infer which
object the user intended.

**UV convention at this boundary.** XRBM version 1 and the current Blender
adapter preserve authored Blender `(u, v)` values exactly; they do not flip V.
That pins deterministic extraction for this material-free profile, but it does
not decide the eventual textured-Blender sampling convention. Before texture
references enter this source profile, an asymmetric textured oracle must decide
one offline normalization point—either UV conversion or image-row conversion—
and test it against both Blender's viewport and xrPhoton. The choice must be an
explicit compiler-contract change, never an unannounced fix applied in multiple
places.

The ignored root `blender/` directory holds owner-local source files. The first
fixtures are `test_pyramid.blend` / object `test_pyramid` and
`test_sphere.blend` / object `test_sphere`, plus
`test_smooth_sphere.blend` / object `test_smooth_sphere`. All three are optional
gallery probes; the sphere pair pins identical position/UV corner streams with
flat-face normal splits versus shared smooth normals.
Generated outputs live beneath the ignored
`build/<preset>/assets/blender/` directory. The opt-in
`xrPhotonBlenderOfflineProof` target uses the cache settings
`XRPHOTON_BLENDER_EXECUTABLE`, `XRPHOTON_BLENDER_PYRAMID_BLEND`, and
`XRPHOTON_BLENDER_SPHERE_BLEND`, and
`XRPHOTON_BLENDER_SMOOTH_SPHERE_BLEND` to run all three files through Blender and
verify their canonical outputs; it does not make the local `.blend` inputs
normal-build dependencies. All three outputs can be configured as gallery entries.

Third-party assets — free path-tracing test models included — normally enter
the same way: Blender imports them, then an xrPhoton export front end feeds the
shared compiler. A polished add-on UI may later wrap this landed headless path;
it must not fork the compiler or writer. A GLB importer remains an allowed but
deferred **offline adapter**:

```
GLB ──► optional importer ──► shared asset compiler ──► OGFx
```

It never enters the runtime and never serializes OGFx independently. Add it
only when a concrete workflow justifies maintaining that extra input surface;
direct Blender export remains the primary path. The invariant is one runtime
model format and one validated OGFx writer, not one possible source format.
(This supersedes the earlier runtime-interchange geometry plan — see
[the milestone section](#the-revised-first-ogfx-milestone-m4) and the notice
atop [GEOMETRY_PLAN.md](GEOMETRY_PLAN.md).)

## The shared asset compiler

**DECISION.** One shared offline compiler serves every producer of runtime
assets:

- Legacy OGF/OMF conversion
- Blender integration
- The modern SDK
- Command-line builds
- Automated asset rebuilding
- Optional interchange import adapters, if and when a concrete need justifies one

**The single-writer rule.** There are **no separate competing OGFx writers**
in Blender, the SDK, and conversion tools — every path funnels through the
one compiler, so there is exactly one implementation of serialization,
validation, coordinate conversion, and bounds generation. A second writer is
how formats fork in practice: two writers disagree in some corner, both
outputs load, and the disagreement becomes load-bearing. This is the "one
focus, clear vision" convention applied to tooling.

The first landed compiler slice is the standard-library-only model and canonical
writer in [`src/ogfx.hpp`](src/ogfx.hpp) / [`src/ogfx.cpp`](src/ogfx.cpp). The
M4a [`src/legacy_ogf.hpp`](src/legacy_ogf.hpp) /
[`src/legacy_ogf.cpp`](src/legacy_ogf.cpp) static source adapter and
[`src/legacy_ogf_rigid.cpp`](src/legacy_ogf_rigid.cpp) rigid-compound adapter
populate that model. Their `decodeModel` entry point dispatches from the source
header type, and `xrPhotonAssetCompiler convert-ogf <input.ogf> <output.ogfx>`
supplies their common filesystem boundary. The Blender extractor and
`src/blender_mesh.cpp` adapter add
the `convert-blender <diagnostic-name> <output.ogfx>` stdin boundary described
above. Neither source adapter owns serialization: the CLI always calls the same
validated writer. These units own neither Vulkan nor renderer-native state.

**DECISION — what the compiler does**, each with its reason:

- **Detect source versions** — OGF v3 vs. v4, SMPARAMS 3 vs. 4, the CS/CoP
  vertex-format variants: legacy content spans a decade of engine revisions
  and the converter must know which it is reading, not assume.
- **Validate source data** — legacy assets contain era-typical corruption
  and editor quirks; garbage is rejected with a diagnostic at convert time,
  never discovered at runtime.
- **Preserve X-Ray semantics** — conversion changes representation, not
  meaning (the engine-direction rule at file granularity).
- **Convert coordinates** — the offline home of the
  [coordinate-convention decision](#coordinate-conventions).
- **Repack geometry** — legacy 16-bit-index splits and FVF layouts become
  OGFx's 32-bit-indexed, BLAS-grouped streams.
- **Generate bounds** — per-geometry and model bounds are computed here once,
  so loaders validate them instead of deriving them.
- **Resolve material information** — legacy texture/shader references become
  OGFx material records and geometry flags through explicit semantic mappings;
  a legacy selector string is omitted only when its complete relevant meaning
  has a pinned OGFx representation. Unknown or ambiguous shaders are rejected,
  not guessed or silently reduced to a texture name.
- **Produce deterministic output** — identical complete inputs, effective
  options, schema version, and compiler version yield byte-identical output.
  The compiler/output revision is bumped for every byte- or
  semantics-affecting change, including an output-affecting dependency
  upgrade; versioning cannot be left to release marketing.
  `OGFX_DESC` may preserve a timestamp supplied by source provenance, but it
  never records conversion wall-clock time or filesystem mtimes; missing or
  unstable provenance fields are omitted or canonicalized. Determinism makes
  hash-based skipping sound, diffs meaningful, and bugs reproducible.
- **Skip unchanged assets using hashes** — the cache key covers the source,
  all dependencies, effective options, schema version, and compiler version;
  an unchanged complete input costs a hash check, not a rebuild. This is
  the property the SDK's ~50-level scale requirement leans on (see
  [SDK.md](SDK.md)). *(PROPOSED: the specific hash — a fast, well-tested
  content hash chosen at implementation time; it authenticates nothing, so
  cryptographic strength is not a requirement.)*
- **Report unsupported or ambiguous data clearly** — naming the file, chunk,
  and field, per the same loud-failure convention the runtime loader follows.
  A converter that silently drops what it does not understand manufactures
  runtime mysteries.

## Formats not automatically replaced

**DECISION.** There will be no extended version of every X-Ray format merely
for naming consistency. A format is replaced when a real limitation appears,
and not before:

- **Likely to remain unchanged:** `.ltx` (configuration), `.lua` (scripts),
  `.dds` (textures), `.ogg` (audio).
- **Deliberately undecided:** level, collision, AI-map, spawn, and detail
  formats. Each is inspected separately when the project reaches it — none of
  these has a decided `x` variant, and this document must not be read as
  implying one.

In particular: the SDK's *editable* level representation needs modernization
(the old workflow is the fragile part — see [SDK.md](SDK.md)), but that does
not automatically mean the *runtime* `.level` format needs replacement. Editor
representation and runtime representation are separate questions with separate
triggers.

## The revised first OGFx milestone (M4)

**DECISION.** The geometry plan's milestone numbering continues from the
landed work — M1 (VMA), M2 (GLM transforms), M3a (staged uploads), and M3b
(the BDA quad probe) are done; their records live in
[GEOMETRY_PLAN.md](GEOMETRY_PLAN.md). The plan **pauses before the old M4**
— the superseded runtime interchange-format loader. The revised M4 is the
**OGFx round-trip**:

```
procedural quad ──► shared compiler writer ──► test_quad.ogfx ──► OGFx decoder
                ──► model SceneData ──► M4 caller adds identity preview instance
                ──► existing GpuScene / AS / RT pipeline ──► rendered quad
```

The procedural quad builder **moves into an offline quad tool**, but that tool
is only an input/front end: M4's serializer is the first implementation
of the shared compiler writer, not a throwaway second writer. The runtime keeps
exactly one model-loading path, so `createProceduralSceneData` and its
`scene.cpp` implementation leave the runtime. The reusable OGFx decoder
reconstructs only model-owned `SceneData` fields and returns no instances. After
requiring one mesh, one geometry, and one material, the M4 preview caller appends
`SceneInstance{meshIndex = 0, identity}`.
The GPU side — `createGpuScene`, the acceleration-structure build, and the RT
pipeline — renders the assembled scene unchanged. A file-backed red/green UV
gradient on the predicted identity-placed quad is the visual oracle; M3b
already proved the deliberately X-rotated inverse-transpose normal path.

**Landed:** the shared writer and offline quad front end generate
`build/<preset>/assets/probes/test_quad.ogfx`; the runtime opens that generated file through
the strict transactional decoder and field-by-field `SceneData` adapter, then the
caller appends one identity preview instance. The old `scene.cpp` procedural builder
is gone, leaving exactly one runtime model-loading path. Deterministic serialization,
writer/decoder round trips, malformed-input rejection, and the unchanged GPU path are
the milestone's verification boundary.

**M4 proves:**

- OGFx writing
- OGFx loading
- Version and chunk validation
- File-backed geometry
- Reconstruction of model data in `SceneData` plus the caller-owned temporary
  identity-preview policy
- Compatibility with the existing GPU scene and RT renderer

**Explicitly out of scope for M4:** skeletons, OMFx, textures, compression,
physics, and level formats. None of these is forced into the first milestone;
each arrives with its own consumer.

**After M4, in permanent additive order:**

1. **M4a — legacy static OGF → command-line converter → OGFx, offline proof
   first. Landed.** This is a direct binary conversion path; Blender is neither an
   intermediate format nor a required batch-conversion dependency. The landed
   slice pins source-version rejection, real-corpus validation, coordinate
   handling, bounds generation, faithful logical texture references, and
   explicit legacy-shader mapping. Its local-corpus acceptance input is the
   externally supplied SoC asset
   `meshes/objects/dynamics/plitka/plitka1.ogf`: OGF v4 `normal`, direct
   `HEADER`/`TEXTURE`/`VERTICES`/`INDICES` chunks, header shader id `0`, FVF
   `0x112`, `u16` indices, and the source engine-shader name `default`. M4a
   accepts only that pinned id/name pair and maps it to the v1 opaque
   geometry/material semantics; any other header shader id or shader name is
   rejected until its complete mapping is specified. OGFx v1 therefore does
   not need fields that merely carry untranslated legacy shader selectors.
   Repository tests use generated synthetic OGF fixtures; no GSC asset or
   machine-specific absolute path enters the repository. The local corpus pins
   1,802 vertices, 3,300 widened indices, its logical texture name, and a
   deterministic 71,328-byte canonical output. The opt-in
   `xrPhotonM4aOfflineProof` target drives the real CLI, verifies source identity,
   complete schema reconstruction, byte-exact reserialization, and runtime
   reconstruction of the logical reference, then persists the proven output
   beneath
   `build/<preset>/assets/soc/meshes/objects/dynamics/plitka/plitka1.ogfx`.

2. **Additive runtime gallery + textured plitka path. Implementation landed;
   final visual sign-off pending.** Generic scene
   assembly merges independently decoded OGFx models and supplies world placements
   without adding instances to the format. The permanent generated wedge probe
   drove the N-BLAS/N-instance and multi-geometry generalization. The texture
   foundation carries logical names into `SceneData`, resolves strict DDS
   DXT1/DXT5 beneath an owner-configured root, uploads BC1/BC3 directly, and maps
   untextured materials to a white fallback. The configured gallery now carries
   the M4a plitka output and its marble DDS into the render loop beside the generated
   probes, exercising the real BC1 upload and nonzero descriptor path without
   changing the canonical bytes or adding source-specific runtime logic. Its
   on-screen orientation, scale, winding, and texture appearance remain a final
   owner visual check.

3. **Blender opaque probe → headless export → OGFx. Landed and gallery
   validated.** Blender 5.1.x and
   `tools/blender/export_ogfx.py` extract
   the explicitly named, material-free static `test_pyramid` mesh through the
   private stdin-only `XRBM` exchange. The C++ adapter applies scene/object
   transforms, `(x, y, z)` → `(x, z, y)`, inverse-transpose normals, and
   determinant-aware winding, then feeds the shared writer. The flat-shaded
   `test_sphere` exercises dense triangulation, its UV seam, and flat-face corner
   splitting; `test_smooth_sphere` pins shared normals over the same geometry and
   UV corner stream. All three canonical outputs are reproducible beneath
   `build/<preset>/assets/blender/` and are independent optional gallery probes.
   The slice adds no second writer or runtime loader. A direct
   GLB-to-compiler adapter remains an optional later offline tool.

4. **Regular-barrel rigid OGF → flattened render mesh + reusable physics
   recipe. Landed.** The externally supplied SoC source is
   `meshes/physics/balon/bochka_close_1.ogf` (29,710 bytes, SHA-256
   `87be6a577756af252496be56d29d4c50c9ebdba9443e9d57f22af212fa1af33f`).
   The direct legacy adapter validates its type-`0xA` root, type-`5` child,
   three-bone hierarchy, rigid/nonbreakable cylinder data, and bind transforms;
   Blender is not a conversion stage. It emits one ordinary opaque render mesh
   with 436 vertices, 1,158 indices, and logical texture
   `mtl\mtl_barrel_01`, plus one optional 62-unit body containing the named
   `barrel`, `obod_1`, and `obod_2` cylinders.

   `xrPhotonRigidOgfOfflineProof` runs the real compiler twice, verifies exact
   source identity, complete render/physics schema reconstruction, byte-exact
   writer round trips, and the runtime decode profile. Its deterministic output
   is 19,352 bytes with SHA-256
   `eed1c06c5d975199ae96fe49517f8893e164cf5e93ce1a040421c7cb0e115060`
   and is persisted at
   `build/<preset>/assets/soc/meshes/physics/balon/bochka_close_1.ogfx`.
   The gallery consumes that render data and the existing DDS path, but no
   current runtime subsystem consumes the optional physics recipe.

The code-owned gallery table remains temporary placement policy until level/scene
data has its real owner; it contains no geometry and never becomes an OGFx chunk.
The opaque/alpha-tested hit-group and `RayTypeCount` SBT split from
[GEOMETRY_PLAN.md](GEOMETRY_PLAN.md) remains the next structural extension when a
real alpha-tested consumer exists. Its sequence changed—not its design: N-BLAS and
opaque base-color sampling landed first, while the geometry-range flag has kept the
future class split explicit since OGFx v1.

## Guiding principle

xrPhoton preserves X-Ray's meaning and identity while modernizing its
renderer, formats, SDK, and selected systems where the project has a concrete
need. Applied to formats: OGFx and OMFx are X-Ray formats, evolved — the
chunk discipline, the visual types, the bone and motion conventions all
survive — with the 2000s-era limits (16-bit indices, unversioned layouts,
unvalidated bounds, undocumented conventions) replaced by exactly what a
Vulkan ray-tracing runtime and a reliable offline toolchain demand, and
nothing more.
