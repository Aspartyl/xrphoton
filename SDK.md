# xrPhoton SDK Plan

This document is the plan for the modern successor to the X-Ray SDK. It is the
SDK counterpart to [FORMATS.md](FORMATS.md) — read that first for the
asset-format direction and the shared asset compiler the SDK is built around.
The SDK described here remains a plan; its shared compiler foundation, including
the flat-static and narrow rigid-compound legacy adapters, the mixed
opaque/alpha-tested pseudodog-tail conversion, and the first headless Blender
source adapter, already exists. No SDK GUI
or project model exists yet, and neither is scheduled before the runtime
milestones that need it
(see the [FORMATS.md milestone section](FORMATS.md#the-revised-first-ogfx-milestone-m4)).

Labels follow FORMATS.md: **DECISION** is settled by the project owner;
**PROPOSED** is this document's elaboration, refined freely at implementation
time within the decisions.

## Goal

**DECISION.** Build a modern successor to the X-Ray SDK: preserve its useful
editor concepts — the LevelEditor/ActorEditor lineage of level composition,
model and animation inspection, spawn and AI-map authoring — while replacing
the old, fragile workflow around them. The X-Ray SDK's concepts earned their
keep across every S.T.A.L.K.E.R. release; its workflow (manual copying,
whole-level rebuilds, crash-prone editors, undocumented interdependencies) is
what limits the project. This is the engine-direction rule
([FORMATS.md](FORMATS.md#overall-engine-direction)) applied to tooling:
preserve what works, modernize what demonstrably does not.

## CLI first

**DECISION.** The **command-line compiler and the project model come before a
large GUI**. The GUI, when it comes, is a front end to the same reliable build
pipeline — never a second pipeline.

The reasoning: everything the SDK ultimately promises — incremental builds,
validation, packaging, automated rebuilds — is pipeline behavior, and pipeline
behavior is only trustworthy when it is scriptable, testable, and identical
with or without a window open. A GUI layered over a solid CLI inherits its
reliability; a pipeline grown inside a GUI has to be excavated later. This
also means the SDK is usable (for conversion, validation, and batch builds)
long before any editor window exists.

## Relationship to the shared asset compiler

**DECISION.** The asset compiler is specified in
[FORMATS.md](FORMATS.md#the-shared-asset-compiler) and shared by legacy
conversion, Blender integration, command-line builds, and the SDK. The SDK is
a **consumer and front end** of that compiler — it invokes, schedules, and
reports on it. It is **never a second writer**: the single-writer rule means
no SDK code path serializes OGFx/OMFx on its own, however convenient a
"quick save" path might look. No interchange format enters the runtime;
external assets primarily enter through Blender and the one export path. Its
first landed front end is the headless Blender 5.1.x script
[`tools/blender/export_ogfx.py`](tools/blender/export_ogfx.py): it extracts one
explicitly named static mesh into the private stdin-only `XRBM` exchange.
Material-free inputs use byte-compatible v1; strict v2 adds one opaque or
alpha-tested DDS material, logical texture reference, classification/cutoff,
and the one-time textured V normalization. The C++ adapter performs coordinate/normal/winding conversion
and the shared compiler alone writes OGFx. A later add-on UI can wrap that path
but cannot fork it. The SDK may later expose an optional GLB importer, but only
as a front end to the shared compiler, never as another OGFx writer. That adapter stays
deferred until a concrete workflow justifies its additional input surface
([FORMATS.md](FORMATS.md#blender-and-external-assets)).

Legacy batch conversion is a separate direct front end to the same writer. In
addition to the M4a flat-static slice, it now accepts the pinned SoC regular
barrel and pseudodog tail. It flattens their narrowly validated rigid bind-pose
render hierarchies, preserves the tail's `models\model_aref` versus
`models\model` geometry classification and exact 128/255 material cutoff, and
retains backend-neutral body metadata: three cylinders for the barrel and one
box for the tail. Blender is neither required nor invoked by that path. The
engine's separate `PhysicsWorld` now consumes those recipes for configured
dynamic yard placements, but Jolt handles and live simulation state remain
outside both OGFx and the shared compiler.

## Capabilities

**DECISION** — the capability list; grouping and intent notes are
**PROPOSED**. Each exists to replace a specific fragility of the old
workflow, not to accumulate features.

### Project and content management

- **Project and level management** — one project model that knows every
  level, asset, and their relationships; opening, creating, and organizing
  levels is a project operation, not a directory-copying ritual.
- **Asset browser** — browse, search, and preview the project's assets with
  their conversion state visible, so "is this asset built and current?" is a
  glance, not an investigation.
- **Packaging** — producing shippable/testable content sets from the project
  is a pipeline invocation with the same determinism guarantees as any other
  build.

### Authoring and inspection

- **Model and animation viewer** — inspect OGFx models and their motions
  (the ActorEditor lineage) without launching the game; the first natural GUI
  consumer of the runtime's own loading and rendering code.
- **Material editor** — author and edit the material information OGFx
  records carry, with validation rather than free-text conventions.
- **Scene hierarchy and property inspector** — the LevelEditor lineage:
  structured editing of what a level contains, with properties surfaced and
  validated instead of hand-edited.
- **Entity and spawn tools** — placing and configuring game objects and
  spawns as first-class editor operations.
- **A-Life and AI-map tools** — authoring the simulation and AI-navigation
  data that make a level live; preserved concepts from the original SDK,
  since A-Life is expanded rather than rewritten.
- **Cross-level connections** — level transitions and cross-level
  relationships edited and validated project-wide, which is precisely where
  a ~50-level project punishes manual bookkeeping.

### Reliability and iteration

- **Validation and diagnostics** — the compiler's loud, named error
  reporting surfaced in the editor; problems point at the asset and field,
  not at a crashed tool.
- **Incremental and background builds** — editing continues while the
  pipeline rebuilds only what changed.
- **Dependency tracking** — the project model knows what depends on what, so
  a changed texture rebuilds its dependents and nothing else.
- **Autosave and crash recovery** — work is not lost to tool crashes; the
  old SDK's reputation here is exactly what "replacing the fragile workflow"
  means.
- **Undo/redo** — universal, not per-tool.
- **Search and bulk editing** — project-wide queries and edits (rename a
  material everywhere, retarget a texture path) as supported operations
  rather than folk scripts.

## The scale requirement

**DECISION.** The SDK must support a project of roughly **50 levels** without
relying on manual copying or rebuilding everything after every change.

What that concretely implies (**PROPOSED** consequences, all following from
the decision):

- **Incremental, background builds are structural**, not conveniences: at 50
  levels, any "rebuild the world" step becomes the workflow's bottleneck and
  eventually its abandonment reason.
- **Dependency tracking and content hashing** carry the increment: the
  compiler's deterministic output and hash-based skip
  ([FORMATS.md](FORMATS.md#the-shared-asset-compiler)) are what make "only
  what changed" a provable statement instead of a cache heuristic.
- **No manual copying** — shared assets are referenced through the project
  model, never duplicated per level; the original SDK's copy-into-place
  convention is one of the fragilities being replaced.
- **No full rebuilds after every change** — the default cost of any edit is
  proportional to the edit, not to the project.

## The editable level representation

**DECISION.** The SDK's **editable** level representation needs modernization
— the old SDK's level source formats and workflow are part of the fragility
this plan replaces. That does **not** automatically mean the runtime `.level`
format is replaced: editor representation and runtime representation are
separate questions, and the runtime level/collision/AI/spawn/detail formats
remain deliberately undecided, each to be inspected separately
([FORMATS.md](FORMATS.md#formats-not-automatically-replaced)). The SDK plan
must stay compatible with either outcome: a modern editable representation
compiling to the original runtime formats, or to modernized ones if and when
a real limitation forces that decision.

## Sequencing

**PROPOSED.** Tooling grows only around a concrete compiler or runtime
validation need, and no GUI precedes the CLI foundation it fronts. The
compiler grows with the [FORMATS.md milestones](FORMATS.md#the-revised-first-ogfx-milestone-m4)
(M4's shared OGFx writer is its seed), then M4a makes the first legacy path a
direct CLI conversion of an externally supplied static OGF corpus asset. That
path does not use Blender: Blender import remains a visual oracle or an
intentional editing workflow for legacy content, while batch migration parses
OGF directly. The separate narrow headless Blender-to-OGFx path has now landed
for the `test_pyramid` gallery probe, flat-shaded `test_sphere`
dense-triangulation/UV-seam/corner-splitting fixture, matching
`test_smooth_sphere` normal-sharing comparison, and alpha-tested
`test_leaf_card`. The leaf is the first v2 material consumer and visibly proves
any-hit rejection with `trees\trees_new_vetka_green`; the scale-faithful
`remade_bochka_close_1` extends v2 to one opaque DDS material and supplies a
newly authored high-detail comparison beside the converted SoC barrel;
`custom_stalker_barrel` uses the same format for an original 19,128-triangle
Stalker-style production prop with one owner-local native-4K uncompressed RGBA8
DDS. All six are
independent optional gallery entries. The project model and CLI harden
around that legacy conversion, Blender export,
and any later optional import adapters, and GUI tools follow the CLI
they front — model/animation viewing first, since it reuses the runtime's own
loader and renderer, with level tooling following the level-representation
decisions. The hierarchical/skeletal-rigid source adapter is now landed for SoC
`bochka_close_1` and the narrowly pinned `item_psevdodog_tail`. Barrel flattening
remains restricted to its validated rigid/nonbreakable zero-rotation bind
profile. The tail adds validated progressive/static child forms and produces one
mesh with two geometries sharing `act\act_pseudodog_fur`: alpha-tested first,
opaque second. Collider, mass, material, center-of-mass, and source-node
semantics remain in backend-neutral OGFx metadata; the tail contributes one box
recipe, not serialized simulation state. The runtime now copies the usable recipe
fields into `SceneData` and instantiates the configured tail (and regular barrel)
through its engine-side Jolt backend. Its opt-in compiler proof is
`xrPhotonAlphaOgfOfflineProof`, with the source selected by
`XRPHOTON_ALPHA_TAIL_CORPUS_OGF` and the pinned texture selected by
`XRPHOTON_ALPHA_TAIL_TEXTURE_DDS`; the separate runtime gallery cache is
`XRPHOTON_GALLERY_PSEVDODOG_TAIL_OGFX`. A future SDK inspector may visualize or
edit these render/physics records, but no such SDK UI or backend-specific
authoring surface has landed. The shipped tail DDS has no transparent mip-0
texels, so the tail itself proves mixed routing and sampled-alpha any-hit
execution rather than a visible cutout; the Blender leaf card supplies the
separate visible-`IgnoreHit` acceptance. Broader source profiles must continue to fail
until all of their semantics have mappings rather than being silently flattened.
Committing to a finer schedule for the remaining formats would invent decisions
the owner has deferred.
