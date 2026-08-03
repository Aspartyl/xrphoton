# Temporal Denoising Plan

Roadmap step 5: motion vectors, temporal reprojection with disocclusion rejection,
and an SVGF-class spatiotemporal filter. This plan pins the history semantics,
ownership, and acceptance gates before any code moves, because temporal state cuts
across frame ownership, synchronization, image lifetimes, resize behavior, and the
capture oracles at the same time.

Everything here operates on linear HDR radiance before tonemapping. Reference and
furnace modes never see the denoiser; their statistical proofs stay proofs of the
raw estimator.

## Denoiser modes

One runtime-switchable mode enum with three values, one filter core:

- **Off**: raw gameplay rendering at the existing 1 to 16 SPP. The frame path
  records no denoise passes; tonemap reads the traced radiance untouched.
- **Spatial**: current-frame filtering only. Demodulate albedo, estimate variance
  spatially, run the a-trous chain, remodulate. No history images are read or
  written; every frame stands alone.
- **Spatiotemporal**: motion-vector reprojection and temporal accumulation feeding
  the same a-trous chain, with moment-based variance once history is deep enough.

Mode is per-frame state carried in the frame record: interactively a key cycles
Off, Spatial, Spatiotemporal (following the SPP-toggle pattern), and capture takes
`--denoise <off|spatial|temporal>` defaulting to off so every existing noisy-frame
hash oracle stays valid. Passes are recorded conditionally each frame, so switching
costs nothing and rebuilds nothing. The final filter iteration remodulates and
writes back into the HDR radiance image itself, so the tonemap descriptor reads the
same image in all three modes and Off needs no descriptor churn.

Mode transitions have one rule: entering Spatiotemporal from any other mode sets
the history-reset flag (history is stale or was never written). Leaving it needs
nothing; Off and Spatial never touch history.

## D0. G-buffer outputs

Raygen writes primary-hit auxiliary outputs alongside the noisy HDR radiance image.
These serve both filtering modes; motion vectors come later with the temporal
phase. New resize-bound storage images:

- **Normal + depth** `R16G16B16A16_SFLOAT`: world-space shading normal in xyz,
  linear view depth in w. Drives the edge-stopping weights.
- **Primary albedo** `R8G8B8A8_UNORM`: the sampled base color at the primary hit,
  used to demodulate radiance before filtering and remodulate after, so texture
  detail is not blurred.
- **Instance ID** `R32_UINT`: for disocclusion rejection in Spatiotemporal mode.

A primary-ray miss writes a depth sentinel; sentinel pixels bypass filtering
entirely in both modes (sky radiance is already low-variance analytic evaluation).

Acceptance: a fixed capture with a G-buffer debug readback pins normal, depth, and
albedo values at probe pixels against analytically known yard geometry.

## D1. Spatial mode: variance estimation and a-trous filtering

Entry requirement: a permanent textured probe. D0's probes pin the world-space
normal transform, depth, and albedo channel order, but every permanent yard
surface samples the white fallback texture, so the G-buffer albedo would read the
same even if the shader dropped the texture multiply. Since D1's demodulation is
the first consumer that must preserve texture detail, D1 cannot start until the
probe compiler emits a small procedural DXT1 texture with distinct texel colors,
a permanent generated surface samples it in the acceptance-only gallery profile
that `--gbuffer-probe` selects (the ordinary yard stays untouched, so Off-mode
hashes remain byte-identical across phases), and `--gbuffer-probe` gains a pixel
pinning the sampled (factor times texel) albedo at a known UV.

New `denoise.slang` compute module, compiled like `tonemap.slang`, descriptor sets
rewritten after every recreate following the tonemap model.

- Demodulation divides radiance by `max(albedo, DemodulationFloor)` per channel,
  with the floor a compile-time constant (1/64). Remodulation multiplies by the
  same clamped value, so the round trip is exact and black albedo can neither
  blow up the division nor drop radiance.
- Before variance, a 5x5 edge-aware robust luminance clamp limits only isolated
  bright outliers using a trimmed local estimate that discards the four brightest
  comparable samples before applying a broad 6x ceiling. The clamped demodulated
  radiance is the input to variance and every wavelet pass, preventing one extreme
  path from inflating and spreading through the chain.
- Variance: 7x7 spatial luminance-variance estimate over the prepared,
  demodulated color.
  This estimator is shared code; Spatiotemporal later reuses it as its
  young-history fallback.
- Five a-trous wavelet iterations (strides 1, 2, 4, 8, 16) over the demodulated
  color, with the standard edge-stopping weights on depth (scaled by screen-space
  depth derivative), normal, and luminance (scaled by filtered variance). Variance
  propagates through the same kernel weights squared.
- Ping-pong between two dedicated `R16G16B16A16_SFLOAT` working images owned by
  `Swapchain`: demodulated color in rgb, its variance in alpha. Each iteration
  reads and writes both through the same image pair, so variance travels the
  chain with the color and no separate variance ping-pong exists. The last
  iteration remodulates albedo and writes the HDR radiance image. Iteration count
  and weights are compile-time constants. No runtime quality knobs beyond the
  mode itself.
- Albedo G-buffer alpha marks primary Glass. Spatial passes copy those pixels
  byte-exactly and exclude them from neighboring filter footprints; noisy raw
  transmission is preferable to structurally incorrect diffuse smoothing until
  a specialized specular/transmission filter exists.

Frame order in Spatial mode: TLAS rebuild, trace, robust clamp, variance, a-trous
chain, tonemap, blit, present. This phase also lands the mode enum, the
interactive cycle key, and the capture flag, with Spatiotemporal still rejected
at parse time until D3.

Acceptance: pinned LDR hash of a fixed Spatial-mode capture frame; Off-mode
captures rerun byte-identical to today's oracles. A generated fixed Glass-sphere
profile additionally requires identical per-Glass-pixel HDR hashes between Off
and Spatial. A 16-SPP Spatial capture quantitatively gates isolated hot pixels
with an edge-aware neighborhood metric rather than relying on a whole-frame hash.

## D2. Motion vectors and previous-frame plumbing

- **Motion vectors** `R16G16_SFLOAT`, resize-bound: screen-space pixel delta from
  the current primary hit to its previous-frame position. Computed in raygen from
  the hit's object-space position, the instance's previous-frame transform, and
  the previous unjittered view-projection matrix. Jitter is excluded on both ends
  so a static scene yields exactly zero motion. Miss pixels reuse the depth
  sentinel convention.
- The previous frame's unjittered view-projection and its inverse: the 80-byte
  `RaygenPushConstants` block cannot absorb two matrices inside the 128-byte
  minimum, so current/previous camera matrices travel in a new per-slot
  `ReprojectionFrame` record through `GpuLighting`'s existing dynamic-uniform
  ring. This is the engine's first real view-projection matrix; it is built on
  the CPU from the same camera state that produces the push-constant basis.
- Previous-frame instance transforms: one storage buffer per frame slot owned by
  `AccelerationStructure` next to the mapped instance inputs it already rewrites
  post-fence, indexed by `InstanceIndex`. The CPU retains the transform array from
  the last successfully submitted frame and publishes that array into the retired
  slot before recording the next submission. This authority advances only after a
  successful `vkQueueSubmit`; an out-of-date/suboptimal acquire or any other
  no-submit path must not rotate previous transforms or temporal submission parity.

This phase also lands the **scripted rigid-motion probe**: in capture mode, one
crate instance is driven along a pinned analytic path instead of live Jolt
stepping, giving deterministic per-instance motion for this phase's oracle and
D4's disocclusion case. Interactive play keeps Jolt.

Acceptance: with physics stepping frozen and the camera fixed, motion vectors hash
as all-zero everywhere; a scripted one-frame camera translation produces the
analytically expected pixel delta at a pinned probe pixel; the rigid-motion probe
produces the expected per-instance delta.

## D3. Spatiotemporal mode: history, reprojection, disocclusion

History images are resize-bound and live in `Swapchain` beside the HDR/LDR targets,
so recreate tears them down and rebuilds them with everything else. Ping-pong pairs
indexed by submission parity (not frame slot):

- **Color history** `R16G16B16A16_SFLOAT` x2: demodulated accumulated radiance,
  history length (frame count, clamped) in alpha.
- **Moments history** `R16G16_SFLOAT` x2: first and second luminance moments.
- **Previous surface attributes**: the D0 normal + depth and instance-ID images
  become parity pairs (x2 each). Raygen writes the current-parity image; the
  temporal pass reads the other parity as the previous frame's normal, depth, and
  instance ID. The rejection tests below need these; color and moments history
  alone cannot support them.

Invalidation rules, in one place:

- Swapchain recreate invalidates all history. `main()` sets the history-reset flag
  in the frame record for the first frame after `prepareRtForSwapchain`; the
  temporal pass then writes history length zero everywhere. No image clears, no
  extra submissions.
- Entering Spatiotemporal mode sets the same flag (see the modes section).
- Camera cuts (spawn, F1 player/free toggle, any future teleport) set the flag.
  Geometric rejection alone is unsafe across a view discontinuity: a stale
  bilinear tap that happens to pass the thresholds smears the old view into the
  new one, so cuts reset history explicitly.
- SPP changes set the flag. The accumulated moments describe the previous sample
  count's variance and would misdrive the luminance edge-stopping weights.
- Lighting changes (time of day) get no explicit invalidation; the neighborhood
  clamp below bounds the resulting lag.

The temporal pass runs between trace and the a-trous chain:

1. Fetch the motion vector, back-project to the previous frame, sample the history
   pair with bilinear taps.
2. Reject each tap that fails any of, evaluated against the previous-parity
   normal/depth and instance-ID images: instance ID mismatch, normal agreement
   below a fixed cosine threshold, relative depth difference above a fixed
   threshold, or reprojected coordinate off-screen. Renormalize surviving tap
   weights; all taps rejected means disocclusion.
3. Accumulate demodulated color and moments with the standard exponential blend
   (alpha floor around 0.2), history length + 1 on success, reset to 1 on
   disocclusion.
4. Neighborhood clamp: clip the history color to the mean/variance bounds of the
   current frame's 3x3 neighborhood before blending, bounding lag from lighting
   and shading changes that motion vectors cannot express.

Variance comes from the integrated moments when history length is at least 4;
younger pixels fall back to D1's spatial estimate. The first a-trous iteration's
output is what next frame's temporal pass consumes as color history (the SVGF
feedback choice); the final iteration feeds remodulation as in Spatial mode. The
working images use alpha for variance, while color-history alpha is history length,
so those channels never alias: after the temporal pass establishes the current
history length, the first a-trous iteration writes its filtered rgb to both its
ordinary rgb+variance working output and the current-parity color-history rgb while
preserving that history-length alpha. Spatial mode writes no color history.

Two frames in flight need no new synchronization primitives: all passes run on the
trace queue, so frame N's temporal pass is ordered after frame N-1's filter output
by pipeline barriers within and between submissions. The parity descriptor sets
(two per compute pipeline) are rewritten after every recreate.

Thresholds and blend constants are compile-time constants in the shared shader
module, mirrored in one C++ header if any test needs them.

## D4. Acceptance and performance gates

Deterministic cases, all through the existing capture/hash machinery
(`hashCaptureImage`, fixed seeds), with scene motion supplied by frozen physics or
the scripted rigid-motion probe, never by live Jolt stepping:

1. **Off is untouched.** Every existing capture, reference, and furnace oracle
   reruns byte-identical. Reference and furnace structurally bypass the denoiser
   regardless of mode (the passes are not recorded).
2. **Spatial determinism.** Pinned LDR hash for a fixed Spatial-mode frame; two
   consecutive runs hash identically since no state crosses frames.
3. **Static convergence.** Spatiotemporal, fixed camera, a pinned `--time` (which
   fixes lighting only), and physics stepping disabled through an acceptance-only
   capture profile that reuses the reference profile's gallery and fixed-transform
   setup, 256 frames: the three reference regions' means must agree with the stored
   raw-reference means within 3 percent. This still records the ordinary MIS frame
   path and denoiser; it does not enter `CommandLineMode::Reference`, which
   structurally bypasses denoising. The
   stability gate compares matching phases of the 16-frame jitter cycle, not
   consecutive frames (exponential blending over periodic jitter never makes
   adjacent frames equal): per-region mean drift between frames 224 and 240 must
   fall under a pinned tolerance, with bit-equal hashes recorded if the float16
   state reaches its periodic orbit.
4. **Reprojection correctness.** Scripted deterministic camera pan over N frames:
   pinned LDR hash, plus a pinned history-length readback showing interior pixels
   retain history while screen-edge inflow pixels reset.
5. **Disocclusion.** The scripted rigid-motion probe's trailing edge: pinned hash
   of a selected frame, plus a probe asserting history length resets in the newly
   revealed region and nowhere else on the ground plane.
6. **Invalidation.** Resize, mode re-entry, an F1 camera cut, and an SPP change:
   the first Spatiotemporal frame after each event must behave exactly as frame
   zero (equal hash for equal frame index and time).

Performance, using the established 32-warmup/256-median timestamp protocol at
1920x1080 on the dev RTX 5070 Ti:

- New timestamp bracket around the denoise chain. Gates: Spatial median at or
  under 1.5 ms; Spatiotemporal median at or under 2.0 ms. Record the measured
  baselines in ARCHITECTURE.md status when D4 lands, then tighten each gate to
  baseline + 10 percent.
- Trace median regression from the G-buffer writes: at or under 15 percent over
  the current recorded trace baseline, measured in Off mode.

## Exclusions

Deliberately out of scope; do not let them shape this design:

- **Deformable motion.** Motion vectors cover rigid instance motion plus camera
  motion only. Skinned/deformed geometry is roadmap step 3 (BLAS refit) and adds
  per-vertex previous positions when it lands.
- **Advanced upscaling.** No DLSS/FSR/XeSS-class reconstruction and no
  render-scale decoupling; the denoiser runs at native swapchain extent.
- **Adaptive sampling.** Samples per pixel stay the uniform runtime-selected
  1/2/4/8/16 in every mode; no variance-driven per-pixel sample allocation.
- **Neural denoising.** Permanent: no trained models, no inference runtimes, no
  vendor neural denoisers (OptiX denoiser, DLSS Ray Reconstruction class). The
  filter stays analytic and fully deterministic.
- Secondary-bounce or per-technique demodulation splits (direct/indirect
  separation) are a later refinement if the single-channel filter proves limiting.

## Execution order

D0 through D4 land as separate reviewable commits, each with its acceptance checks
green before the next begins. D0 gives both filter modes their inputs; D1 ships a
complete, useful Off/Spatial renderer; D2 adds motion data with no behavior change;
D3 completes Spatiotemporal on that contract; D4 pins everything. ARCHITECTURE.md's
status, per-frame flow, and synchronization sections are updated as each phase
lands.
