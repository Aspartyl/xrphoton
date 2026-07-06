# Plan: Camera + push constants (roadmap step 1)

## What changes conceptually

Today the raygen shader hardcodes an orthographic camera: each pixel's ray starts
at its NDC position behind the triangle plane and travels +Z, and the NDC-square
mapping stretches with the window (the latent aspect-ratio distortion). This step
replaces it with a perspective camera whose parameters are delivered via push
constants, recorded into each frame's command buffer, plus a GLFW fly camera
(WASD + mouse look) so the engine is interactive from the first possible moment.

Push constants over per-frame uniform buffers, deliberately: the payload is 64
bytes (half the spec-guaranteed 128-byte `maxPushConstantsSize`), the data is
recorded into the command buffer so the two frames in flight cannot race on it by
construction, and no descriptor set layout change is needed — only a
`VkPushConstantRange` on the pipeline layout. Per-`FrameResources`-slot uniform
buffers remain the designated promotion path when a payload outgrows the push
range — expected at scene time, not camera time.

Two design decisions resolved up front:

- **Origin + ray basis, not inverse view/projection matrices.** The push payload
  is the camera origin plus three basis vectors (`forward`, and `right`/`up`
  pre-scaled on the CPU by `tan(fov/2)` and aspect), and the raygen shader
  computes `direction = normalize(forward + ndc.x * right - ndc.y * up)` — no
  per-pixel matrix multiply, no w-divide, no un-projection of NDC corners. It is
  also what keeps the CPU side matrix-free: deriving the basis from yaw/pitch
  needs only `sin`/`cos`, cross products, and normalize. Inverse-VP earns its
  keep when a raster path or reprojection consumes the same matrices; the first
  consumer of a real view-projection matrix is temporal reprojection (roadmap
  step 5), which can build one from this same camera state when it lands.
- **No math library (yet).** The project currently links only Vulkan and GLFW,
  and this step needs exactly: vec3 add/scale, cross, normalize. That is a
  ~20-line hand-rolled `Vec3` in the new camera module, not a GLM dependency.
  Matrices enter the engine at geometry/scene time (instance transforms) — that
  step decides GLM vs. growing the in-house math, with real requirements in
  hand. One focus: don't buy the general mechanism before the single sufficient
  path stops sufficing.

## 1. New module: `src/camera.hpp` / `src/camera.cpp`

The seventh translation unit. Owns all camera state, all fly-control policy, and
the push-constant payload type. Pure math + GLFW input — **no Vulkan include**
(`makeCameraPushConstants` takes a plain `float aspect`, not a `VkExtent2D`), so
the header stays acyclic and Vulkan-free.

`camera.hpp`:

```cpp
#pragma once

#include <cstddef>  // offsetof, for the ABI asserts below

struct GLFWwindow;

namespace xrphoton
{
// Minimal vector type: exactly the operations the camera basis math needs live as
// helpers in camera.cpp. Growing this into a real math module (matrices) is a
// geometry/scene-time decision, not a camera-time one.
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 60° in radians. In the header (not camera.cpp's tuning constants) only because
// Camera's default member initializer needs it.
constexpr float DefaultVerticalFov = 1.0471976f;

// Persistent fly-camera state, plus the cursor-tracking fields updateCamera needs
// across frames. A plain value struct owned by main(), not an RAII owner.
struct Camera
{
    Vec3 position{0.0f, 0.0f, -2.0f};
    float yaw = 0.0f;    // radians; yaw 0, pitch 0 looks down +Z (see basis note)
    float pitch = 0.0f;  // radians, clamped inside ±π/2 (see the pitch-clamp trap)
    float verticalFov = DefaultVerticalFov;

    // Mouse-look anchor: the cursor position the next frame's delta is measured
    // from, and whether it is valid (false on the first frame and after every
    // recapture, to swallow the position jump — see the anchor trap).
    double lastCursorX = 0.0;
    double lastCursorY = 0.0;
    bool cursorAnchorValid = false;
};

// The push-constant payload: origin + ray basis, raygen-only. right and up are
// pre-scaled by tan(verticalFov/2) (and aspect, for right) so the shader's ray
// direction is forward + ndc.x*right - ndc.y*up with no per-pixel trig. Explicit
// pads: float3 rounds up to 16-byte alignment in every GPU layout rule set, and
// the static_assert pins the CPU struct to the same 64-byte shape.
struct CameraPushConstants
{
    Vec3 origin;  float pad0 = 0.0f;
    Vec3 forward; float pad1 = 0.0f;
    Vec3 right;   float pad2 = 0.0f;
    Vec3 up;      float pad3 = 0.0f;
};
static_assert(sizeof(CameraPushConstants) == 64,
    "must match the shader's push-constant block and stay within the 128-byte spec minimum");
static_assert(offsetof(CameraPushConstants, forward) == 16
    && offsetof(CameraPushConstants, right) == 32
    && offsetof(CameraPushConstants, up) == 48,
    "field offsets are the shader ABI, not just the total size");

// Poll input and advance the camera by dt seconds: cursor capture/release
// (escape releases, left click recaptures), mouse look while captured, WASD +
// space/ctrl movement, shift sprint. All GLFW input policy lives here so main()
// stays orchestration-only.
void updateCamera(Camera* camera, GLFWwindow* window, float dt);

// Derive the frame's push payload from the camera state and the current
// swapchain aspect ratio (width / height) — aspect lives here, not in Camera,
// because it is the swapchain's property and changes on resize.
CameraPushConstants makeCameraPushConstants(const Camera& camera, float aspect);
}
```

Notes:

- `struct GLFWwindow;` forward-declares the same struct GLFW's own
  `typedef struct GLFWwindow GLFWwindow;` names, so `camera.hpp` needs no GLFW
  include; `camera.cpp` includes `GLFW/glfw3.h` for the real API. The header's
  only include is `<cstddef>`, for the `offsetof` asserts.
- Vec3 helpers (`add`, `scale`, `cross`, `normalize`, …) are file-private free
  functions in `camera.cpp`'s anonymous namespace, per the conventions. So are
  the tuning constants: `MoveSpeed` (world units/s), `SprintMultiplier`,
  `MouseSensitivity` (radians per pixel), `PitchLimit`, `WorldUp = {0, 1, 0}`.
  The one exception is `DefaultVerticalFov`, header-bound as shown above.

## 2. Basis convention — keep the current on-screen orientation

The current shader maps world +X to screen-right and world +Y to screen-up
(via the explicit `-ndc.y` flip). The perspective basis must preserve that so
the first perspective frame reads as "the same triangle, now with depth":

- `forward = (cos(pitch)·sin(yaw), sin(pitch), cos(pitch)·cos(yaw))` — yaw 0,
  pitch 0 looks down **+Z**, matching today's view direction.
- `right = normalize(cross(WorldUp, forward))`, `up = cross(forward, right)` —
  at yaw 0 this yields right = +X, up = +Y, exactly the current mapping.
- Scale for perspective: `right *= tan(verticalFov/2) * aspect`,
  `up *= tan(verticalFov/2)`. Scaling `right` by aspect is the aspect-ratio fix:
  the horizontal footprint now tracks the window shape instead of stretching a
  fixed NDC square.
- The `-ndc.y` flip **stays in the shader**: launch IDs counting rows downward is
  a property of dispatch-index space, not of the camera, and the existing comment
  explaining it remains true. The CPU sends an un-flipped, world-up `up`.

Default pose `position = (0, 0, -2)`, yaw 0, pitch 0, vertical FOV 60°: at the
triangle's plane (z = 0) the view half-height is `tan(30°) · 2 ≈ 1.15`, so the
half-unit triangle is framed comfortably, upright, dead center — visibly similar
to today's startup image.

## 3. Shader: `shaders/triangle.slang`

- Add the push-constant block, mirroring the CPU struct field-for-field (the
  explicit CPU pads make the 0/16/32/48 offsets unconditional):

  ```slang
  struct CameraPushConstants
  {
      float3 origin;
      float3 forward;
      float3 right;
      float3 up;
  };

  [[vk::push_constant]]
  ConstantBuffer<CameraPushConstants> camera;
  ```

- Replace the hardcoded ray in `rayGenMain`:

  ```slang
  ray.Origin = camera.origin;
  ray.Direction = normalize(camera.forward + ndc.x * camera.right - ndc.y * camera.up);
  ```

  `TMin = 0.001` / `TMax = 100.0` stay hardcoded — tMax comfortably covers the
  unit-scale scene and becomes a real decision at scene time, not camera time.
- Rewrite the "orthographic camera hardcoded / no push constants yet" comment to
  document the basis contract instead (pre-scaled right/up, why the y-flip is a
  dispatch-space property).
- `missMain` / `closestHitMain` are untouched.

## 4. Pipeline layout: `src/rt_pipeline.cpp` (`createRtPipeline`)

Attach the push range to the existing layout — the only Vulkan-object change in
the whole step:

```cpp
VkPushConstantRange pushRange{};
pushRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
pushRange.offset = 0;
pushRange.size = sizeof(CameraPushConstants);
```

wired into `layoutCreateInfo.pushConstantRangeCount / pPushConstantRanges`.
`rt_pipeline.cpp` includes `camera.hpp` for the sizeof; the header does not need
it. Raygen-only mirrors the descriptor bindings' rationale: miss and closest-hit
touch only the ray payload. Update the `createRtPipeline` doc comment in
`rt_pipeline.hpp` ("no push constants yet" is no longer true) and the descriptor/
layout comments that enumerate what the layout holds.

## 5. Frame path: `src/renderer.hpp` / `src/renderer.cpp`

- `drawFrame` grows a parameter:
  `VkResult drawFrame(const Renderer& renderer, uint32_t frameIndex, const CameraPushConstants& camera);`
  with `struct CameraPushConstants;` forward-declared in `renderer.hpp` like the
  other borrowed types. The camera payload is per-frame *data*, not a
  program-lifetime handle, so it rides as a parameter — putting it in `Renderer`
  (a const view of program-lifetime objects) would misstate its lifetime.
- `recordTraceCommandBuffer` takes the same `const CameraPushConstants&` and
  records, between `vkCmdBindDescriptorSets` and the trace:

  ```cpp
  vkCmdPushConstants(
      commandBuffer,
      rt.pipelineLayout,
      VK_SHADER_STAGE_RAYGEN_BIT_KHR,
      0,
      sizeof(CameraPushConstants),
      &camera);
  ```

  The `stageFlags` must exactly match the range declared on the layout (the
  VUID-vkCmdPushConstants-offset-01795/01796 pair: every pushed byte+stage must
  fall inside a declared range, and the push must cover every stage of any
  range it overlaps) — RAYGEN only, same as step 4. `renderer.cpp` includes
  `camera.hpp` for the complete type (the header keeps only the forward
  declaration).
- No synchronization change anywhere: the push constants are recorded into the
  frame slot's own command buffer after its fence wait, which is precisely the
  no-race-by-construction argument for choosing them. `prepareRtForSwapchain`
  is untouched — the camera does not participate in the resize contract beyond
  main() reading the new `swap.extent` for aspect (next section).

## 6. Orchestration: `src/main.cpp`

- Create a `Camera camera;` before the render loop (default pose from step 2).
- Capture the cursor before entering the loop:
  `glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED)`, plus
  `GLFW_RAW_MOUSE_MOTION` when `glfwRawMouseMotionSupported()` — raw motion
  skips OS pointer acceleration, and the support check matters on Wayland.
  Escape/click toggling thereafter is `updateCamera`'s job (step 7).
- Frame timing: `double lastTime = glfwGetTime();` before the loop; each
  iteration computes `dt = now - lastTime`, clamps it to `MaxFrameDt` (0.1 s),
  and advances `lastTime`. The clamp is load-bearing: interactive window drags
  and resize stalls can block the loop for seconds on some WMs, and an unclamped
  dt teleports the camera on the next frame.
- Loop body, after `glfwPollEvents()`:

  ```cpp
  updateCamera(&camera, ctx.window, dt);

  const float aspect = static_cast<float>(swap.extent.width)
      / static_cast<float>(swap.extent.height);
  const VkResult frameResult = drawFrame(
      renderer, currentFrame, makeCameraPushConstants(camera, aspect));
  ```

  Aspect is read fresh from `swap.extent` every iteration, so the
  recreate-on-resize branch needs no camera-specific code: the frame after a
  recreate picks up the new extent automatically.

## 7. Fly controls (inside `updateCamera`)

Single deliberate control scheme — game-style always-captured look, matching the
STALKER end goal, not editor-style hold-RMB:

- **Capture toggle.** Escape releases the cursor (`GLFW_CURSOR_NORMAL`); left
  click while released recaptures (`GLFW_CURSOR_DISABLED`) — so Escape is
  *not* "quit"; the window closes via the WM close button as today. While the
  cursor is free the camera is fully frozen (no look, no movement): a released
  cursor means the user's attention left the viewport, and a camera that
  half-responds is worse than one that predictably stops.
- **Mouse look.** Poll `glfwGetCursorPos` and integrate the delta against the
  anchor fields: `yaw += dx * MouseSensitivity`, `pitch -= dy * MouseSensitivity`
  (cursor y grows downward). Polling over a cursor-pos callback deliberately:
  no user-pointer plumbing, and the disabled cursor's virtual position makes
  per-frame deltas exact.
- **Anchor discipline (the jump trap).** On every transition into the captured
  state — including the very first frame — set `cursorAnchorValid = false`; when
  the flag is false, record the current position into the anchor and skip that
  frame's look delta. Otherwise the first captured frame integrates the entire
  distance between wherever the OS cursor last was and the capture point as one
  giant rotation.
- **Pitch clamp.** Clamp pitch to `±PitchLimit` (89° in radians), *strictly*
  inside ±π/2: at exactly ±90° `forward` is parallel to `WorldUp` and the
  `cross(WorldUp, forward)` in step 2 degenerates to zero, producing NaNs
  through `normalize`. The clamp is the single guard for that whole failure
  mode. Optionally wrap yaw into (−π, π] to keep float precision over long
  sessions — cheap, do it.
- **Movement.** WASD along the *horizontal* projection is a game-time decision;
  for a fly camera, move along the look direction: W/S = ±`forward`,
  A/D = ∓/±`right` (unscaled, unit-length — recompute the basis or reuse the
  yaw/pitch math, but do not reuse the FOV-scaled push vectors),
  Space/LeftCtrl = ±`WorldUp`, LeftShift multiplies speed by
  `SprintMultiplier`. Normalize the summed direction before applying
  `MoveSpeed * dt` so diagonals are not faster — but only when its squared
  length exceeds a small epsilon: with no keys down (the common case every
  frame) or opposing keys cancelling, `normalize({0,0,0})` divides by zero and
  a single NaN permanently poisons the position. Skip the move entirely for a
  near-zero sum (or make the `normalize` helper return zero for zero input —
  pick one and document it at the helper).

## 8. Build: `CMakeLists.txt`

Add `src/camera.cpp` to the executable's source list. Nothing else — no new
dependencies, and the shader custom command already rebuilds `triangle_spv.h` on
`.slang` edits.

## 9. Documentation

- **ARCHITECTURE.md** (source of truth): add the camera module to the module
  map and status section; extend the per-frame flow with the update-camera /
  push-constants step; add a short **Camera** subsystem section recording the
  decisions above (origin + basis over inverse-VP and the reprojection caveat,
  no math library yet, push-constant layout contract with the shader, basis/
  handedness convention, control scheme, the pitch-clamp and anchor traps);
  update the RT-pipeline section's "no push constants" statements; mark roadmap
  step 1 landed and promote step 2 (geometry + scene) to "the next step to
  build".
- **CLAUDE.md**: add `camera.{hpp,cpp}` to the layout list, mention the push
  constants in the summary paragraph, and update the "Next step" section — keep
  it a summary; the details live in ARCHITECTURE.md.
- **README.md**: the "Next up:" line currently leads with "camera and input" —
  after this lands it should lead with geometry and materials, or the README is
  stale the moment the step ships.
- Delete this plan file in the landing commit, per the established convention
  (plan docs are working material; git history preserves them).

## 10. Verification

- Build clean (`-Wall -Wextra`), run with validation on: zero validation
  messages, in particular none from push-constant VUIDs.
- Startup image: the triangle upright, centered, barycentric-colored over dark
  red — recognizably the same scene as before the change.
- Aspect fix: resize to an extreme shape (very wide, very tall) — the triangle's
  proportions must not stretch, at most more/less of the scene is visible.
  This is the user-visible proof the step's stated bug is fixed.
- Fly: WASD/Space/Ctrl move, mouse looks, Shift sprints; pitch stops at the
  clamp instead of flipping or NaN-ing (look straight up/down and keep pulling).
- Capture toggle: Escape frees the cursor and freezes the camera; click
  recaptures with **no view jump** (the anchor trap's test).
- Stall robustness: drag-resize continuously for a few seconds, release — no
  camera teleport (the dt-clamp's test).

## Suggested commit title

`Add perspective fly camera via push constants`
