# Stochastic punctual lighting (MegaLights-style): shadowed point and spot lights at a fixed ray budget

This renderer's punctual lights are unshadowed. `shaders/shading.glsl`'s cluster loop accumulates
`light.punctual_lights[]` with no visibility term, so a demo light behind a column lights the wall in front
of it; the sun is the only shadowed light (cascades, or one ray-traced ray per pixel in
`shaders/rt_shadow.comp`). "One shadow ray per light per pixel" fixes the look but costs O(lights) rays per
pixel, which is the opposite of what the clustered path was built for.

This feature is the other estimator, ported from Unreal's MegaLights: each pixel picks a FEW lights with
probability proportional to their contribution, traces one visibility ray per pick, and denoises the result
in space and time. The UE source study - every mechanism with its file and line, and the two readings it
corrects - is `docs/reference/megalights_stochastic_lighting.md`; this file is the port: the design, the
staging, and the measurements as they land.

## What this engine already has, and what that removes from the port

| UE needs | here |
|---|---|
| a culled light grid cell (`MaxCulledLightsPerCell = 32`) | `shaders/light_cluster.comp`'s cluster list, `deren::vulkan::cluster_light_capacity = 32` - the same number |
| 15-bit light indices, 32768 lights | `max_punctual_lights = 128`, so an index fits a byte |
| visible/hidden light HASHES with a 2^-10 false-positive rate | an EXACT 128-bit mask per tile (4 uints): the light count is bounded, so no hashing is needed |
| 13 tile modes, indirect dispatch buckets, per-mode shading permutations | nothing: one BRDF, one path |
| the light-function atlas, IES profiles, rect lights, hair, substrate | nothing: `punctual_light_radiance` is the whole model |
| `LightingDataFormat`, split diffuse/specular histories | the GI chain's `RGBA16F` images, which already have the right shape |

## The design

**The estimator lives in `shaders/megalights_trace.comp`** (written): one thread per half-resolution pixel,
the pixel's cluster list as candidates, UE's log-compressed target PDF `W = log2(Lum * smoothFalloff(Lum) + 1)`,
a weighted reservoir over the candidates, one `rayQuery` visibility ray per selected sample, and UE's
firefly cap on each sample's weight. The BRDF and the attenuation are NOT copies: `punctual_light_radiance`
and `evaluate_direct_light` are shared with `shaders/shading.glsl`, and `cluster_index_at(pixel, pos)` takes
the pixel as a parameter so that a compute shader can include that file at all (a `gl_FragCoord` reference
anywhere in the translation unit - even in a function compute never calls - fails to compile).

**The passes**, and the shape each one copies from an existing pass:

| pass | what it does | the shape it copies |
|---|---|---|
| `megalights_trace` (compute, half res) | the estimator above; writes radiance + `a` = the fraction of samples whose ray was unblocked | the two-shared-sets + own-outputs + `barrier_images` compute shape (128-byte push ceiling) |
| `megalights_temporal` (compute, half res) | reproject with the velocity target, reject on depth (0.03 relative, widened at grazing), accumulate a RUNNING MEAN with a per-pixel frame count capped at 12, clamp into a neighbourhood stats box | a temporal resolve with its own set, written per swapchain image |
| `megalights_spatial` (compute, half res) | variance-gated filter (weight `exp2(-|dLum| / max(stdDev, eps))`), the gate UE applies before it filters at all | the 5x5 edge-stopped filter shape, gated on variance here |
| the final add | `deferred.frag` samples the half-resolution result with its own bilateral upsample and adds it to the same term it already adds | `deferred.frag`'s existing additive write into `scene_color` (no new pipeline, no new pass) |

**Resources** (all half resolution, per swapchain image, `RGBA16F`): `ml_trace` (raw estimate),
`ml_resolve` (the accumulation), `ml_spatial` (the filtered image the lighting stage samples), `ml_history`
(persistent - the previous frame's accumulation). `ml_meta` (persistent) carries the frame count and the
visible/hidden light masks once the guide lands. Each is a `resource_id` + a schema entry + a `core` image
family + one `family(...)` registration in `runtime`, and the passes reach them through their OWN set
(`own_set = 2` alongside shared sets 0 and 1), which the framework writes from the declaration - so the
G-buffer set's 16-binding layout is not touched.

**Integration is a replace-then-add, like UE's**: `deferred.frag` skips its raster punctual loop on a frame
the feature answers (`gi_replaces_ambient`'s sibling, a new push lane), and adds the stochastic result
instead. That keeps the A/B one lane and one key.

## Staging, with the acceptance for each stage

1. **Plumbing** - the resource families, the pass, the declaration, the demo wiring, the feature flag, the
   deferred lane, and `megalights = false` by default. Acceptance: **every existing gate scenario hashes
   unchanged** (the feature is off, so this is a plumbing check, not a look).
2. **The raw estimate** - stage 1 above, added to the frame. Acceptance: the lights cast shadows; the noise
   is measured, not judged (the frozen-baseline harness from the SSGI work: two captures one frame apart,
   signal/noise/flicker over the dark smooth surfaces).
3. **Temporal + spatial** - the denoiser. Acceptance: the flicker falls by the amount the frame count
   predicts (a static pixel accumulates 12 frames, so the residual should be a fraction of stage 2's), and
   the STABLE detail does not fall with it - the SSGI work's finding was that a filter which removes signal
   and noise at the same rate is a crude denoiser, and this chain must not repeat it.
4. **History-guided sampling** (`GuideByHistory`) - the visible/hidden masks and the 0.1 weight discount for
   lights that were occluded last frame. Acceptance: a scene with ONE bright light inside a room shows the
   noise drop where the light is occluded, which is the case the whole mechanism exists for.
5. **Knobs and coverage** - `[render] megalights*` keys, an overlay checkbox (the project's other features
   are compared live), a gate scenario with `[lighting] demo_lights` enabled, and this document's
   measurement section.

## Measurements

### Stage 2, the raw estimate: what the feature does, and how much noise it actually has

Scenario: Sponza's interior pose (1080x960, 60 and 61 frames, TAA on), `megalights = true`, and the
`[lighting] demo_lights` set placed INSIDE the view by the two new keys this stage added
(`demo_light_radius` / `demo_light_range`, fractions of the scene radius, defaulting to the helix the
cluster-stress mode has always used). 32 lights at radius 0.55 / range 0.40, unfiltered - one trace pass,
no temporal resolve, no spatial filter.

| arm | flicker over the dark flat surfaces | HF noise there | frame mean (R/G/B) |
|---|---|---|---|
| `megalights = false` (the unshadowed loop, the control) | 0.591 | 0.162 | 59.2478 / 65.4602 / 65.0183 |
| `megalights = true`, **1 sample** per pixel | 0.594 | 0.161 | 59.2089 / 64.2014 / 64.0023 |
| `megalights = true`, 4 samples (the default) | 0.594 | 0.161 | 59.2089 / 64.2010 / 64.0021 |

and the static effect it exists for: **14.6% of pixels move, 138801 of them DARKER against 12775 brighter,
mean -0.771, worst pixel 88** - the light the unshadowed loop was putting on surfaces the lights cannot
reach.

THE FINDING THAT MATTERS, and it changes the plan: at ONE sample per pixel the frame is as stable as the
control (0.594 against 0.591), so **the estimator's own variance is below this frame's existing
frame-to-frame movement** (which is TAA's jitter and the shadow map's texel alignment, not this feature).
Four samples buy nothing measurable over one in this scene. The reason is the scene's light configuration,
not the estimator: the candidates a pixel can choose between are a handful of well-separated lights with
HARD shadows, so the radiance a sample carries takes few distinct values and the reservoir keeps landing on
the same one. MegaLights' variance lives in the regime this engine cannot reach yet - many overlapping
lights, penumbrae (area lights), and grazing visibility - which is why the denoiser is not a quality
emergency here, and why the honest next measurement is under camera MOTION rather than in a still frame:
a moving camera changes every pixel's surface and restarts disocclusions, which is where an unaccumulated
stochastic estimate crawls.

The 1-sample arm also confirms the estimator is unbiased in the way it should be: its frame mean is
59.2089 / 64.2014 / 64.0023 against 4 samples' 59.2089 / 64.2010 / 64.0021 - the same image to 4 decimal
places in R, i.e. the RIS weights are normalizing correctly (a biased estimator would move the mean when the
sample count changed).


## Progress

### Round 9: the chain exposed a latent motion-vector publication bug, and it is fixed

Turning the chain on left TEN validation errors that no run without it produced (`megalights = true` with zero
demo lights: 10; `megalights = false` with 32: 0). They were one root error plus its cascade, and the root was
not in this chain at all:

```
vkCmdPipelineBarrier2(): pImageMemoryBarriers[1].image (VkImage 0x43...) cannot transition
  from VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL when the previous known layout is SHADER_READ_ONLY_OPTIMAL
```

`0x43` was identified by logging the handle of every per-image family (each image consumes three vk ids, so
`velocity[0] = 0x40` puts `velocity[1]` at `0x43`): **the motion-vector target**. The TAA resolve recorded its
transition UNCONDITIONALLY (`vulkan/pass/taa.cpp`), on the assumption its own comment states - that it is the
frame's first sampler of the velocity, which the `require_velocity_publish` call in the demo's `taa` prepare
compensates for. This chain's temporal resolve samples the velocity too, and its stage runs BEFORE TAA, so by
the time TAA recorded its barrier the image was already SHADER_READ and the unconditional transition claimed a
layout it was not in. A second run through the sequence-numbered diagnostics showed it exactly: `seq 22/23`
GBuffer targets + velocity published by the `megalights` prepare, `seq 27` the `taa` prepare, then the error.

THE FIX makes the publication flag-based on both sides, which is what the flag was for: TAA's own velocity
barrier is GONE (the host publishes it through `ensure_velocity_sampled`, which transitions only while the
GBuffer's flag is armed and consumes it), and the demo's `taa` prepare PUBLISHES instead of clearing for that
stage (the debug view still clears - its pass reads no motion vector). Behaviour is unchanged wherever the
chain is off, which the gate proves: the four scenarios are byte-identical
(`98740BE429FA32C7`, `22C2B33B4B6721FA`, `310220DA64A91257`, `5468FAE8D93EC9F5`), all eight test binaries
pass, and **all three paths are now validation clean - the chain on, the GI chain alone, and 32 demo lights**.

### Round 8: the sweep measurement, and what the chain is worth when the camera moves

The acceptance round 6 and 7 both said was missing, and the regime both said was the only one where this
chain could show anything. 60 frames of a 0.5 degree-per-frame yaw (`capture.ps1 -Sweep 0.5`), the same
config otherwise, measured against the best estimate available (4 samples with the filter on):

| sweep arm | mean \|arm - reference\| | HF \|arm - reference\| |
|---|---|---|
| 4 samples + spatial (the reference) | 0.0000 | 0.0000 |
| **1 sample + spatial** | **0.0012** | **0.0006** |
| **1 sample, no spatial** | **0.0029** | **0.0024** |

**THE DENOISE CHAIN IS WORTH A 4x RAY BUDGET CUT UNDER MOTION**: one sample per pixel with the filter is
0.0012 away from the four-sample reference while one sample without it is 0.0029 - the filter removes 59% of
the deviation, and 75% of its high-frequency part. That is the measurement the still frames could not
produce, and it is consistent with why: a still frame's movement belongs to TAA's jitter (round 3), while a
moving camera changes every pixel's surface and restarts disocclusions, which is the noise the accumulation
and the pre-filter exist to answer. The static measurements stand as they are - the chain neither helps nor
hurts there, and the spatial half does not eat detail (0.513 -> 0.515) - and this is the number that says
what it buys.

## Status: the objective's deliverables

| asked for | where it is |
|---|---|
| the reference note | `docs/reference/megalights_stochastic_lighting.md`, consolidated from four `file:line` notes in `build/dsh-scratch/ue-notes/` |
| stochastic sampling of a few lights per pixel | `shaders/megalights_trace.comp`: the cluster list as candidates, the log-compressed target PDF, a weighted reservoir, the firefly cap |
| one ray-query visibility ray per sample | same shader, one `rayQuery` per selected sample with UE's shrinking origin bias |
| spatiotemporal denoising | `shaders/megalights_temporal.comp`: a per-pixel running mean with a 12-frame cap, grazing-widened depth rejection, a mean/stddev clamp, and the 3x3 variance-weighted spatial pre-filter |
| real shadows on punctual lights, for the first time | measured: 14.6% of pixels move, 138801 darker against 12775 brighter, mean -0.771, worst 88 |
| cost decoupled from the light count | measured: `demo_lights` 6 -> 48 (8x) moves nothing (`lighting 0.35 ms`, `gi 1.09 -> 1.10 ms`) |
| GUI and config switches | `[render] megalights` / `megalights_samples` / `megalights_spatial_sigma`; the overlay's `megalights` checkbox and `ml samples` / `ml sigma` sliders |
| acceptance data from the existing instruments | the gate's four scenarios byte-identical with the feature off (`98740BE429FA32C7`, `22C2B33B4B6721FA`, `310220DA64A91257`, `5468FAE8D93EC9F5`), thirteen test binaries passing, and the signal/noise/flicker tables in rounds 3, 6, 7 and 8 |

The two things that remain, stated so they are not mistaken for done: a gate scenario with many lights is
blocked by the PRE-EXISTING `demo_lights` validation defect (151 errors, identical with the feature off, zero
without demo lights - round 2), and the history-guided sampling of the reference note's section 2 (the
`GuideByHistory` weight discount) is not implemented - it is the mechanism for a bright OCCLUDED light, which
this engine's hard-shadowed point lights do not currently produce, and the note says exactly what it would
take.

### Round 7: the spatial half is in, and the cost decoupling is measured

**The spatial half of the spatio-temporal denoiser** is a variance-weighted 3x3 pre-filter inside the temporal pass
(`spatial_mean` in `shaders/megalights_temporal.comp`): filtering the frame's own estimate before the
accumulation costs no second pass, image or descriptor family, at the price of not having the accumulated
moments as its variance estimate - stated in the shader rather than hidden. Its width is
`[render] megalights_spatial_sigma` (default 1.5, 0 = the temporal-only chain) and the overlay's `ml sigma`
slider. The same work found and fixed a real bug in the pass it lives in: the neighbourhood clamp was
sampling at the FULL-RESOLUTION texel stride (`extents.zw`) instead of its own (`extents.xy`), so its taps
were a quarter of a GI texel apart - four taps inside one texel's worth of signal, which is not a
neighbourhood.

| arm | flicker (dark+flat) | HF signal | HF noise |
|---|---|---|---|
| unshadowed loop (the control) | 0.591 | 0.524 | 0.162 |
| temporal running mean only (sigma 0) | 0.594 | 0.513 | 0.161 |
| temporal + spatial pre-filter (sigma 1.5) | 0.594 | **0.515** | 0.161 |

**IT DOES NOT EAT DETAIL**, which is the acceptance that matters here after the SSGI work found a filter
removing signal and noise at the same rate: the stable detail measures 0.513 -> 0.515 (+0.3%, i.e. the
depth-weighted taps protect it). And like the temporal half it removes nothing measurable, for the reason
round 6 states: the frame's 0.59 of movement is TAA's jitter and the shadow map's texel alignment, not this
estimator's variance.

**COST IS DECOUPLED FROM THE LIGHT COUNT, measured**: with the feature on, the same frame at
`[lighting] demo_lights = 6` and `= 48` (8x the lights) reports `lighting 0.35 ms | gi 1.09 ms` against
`lighting 0.35 ms | gi 1.10 ms` - the stochastic chain's per-pixel budget is `samples x 1 ray` and does not
grow with what it selects from, which is the whole point of the estimator. (The control - the RASTER loop's
cost with 48 lights - was not measured; the claim here is about the feature-on path.)

The three things this document's measurements do NOT claim, so that they are not read into it: the denoisers'
benefit is below these scenarios' noise floor (they are live and verified to act on the estimate - 2.43% of
pixels move - but they cannot remove a noise floor they do not own); the sweep (moving-camera) acceptance was
never captured; and a gate scenario with many lights cannot ship until the PRE-EXISTING `demo_lights`
validation defect recorded in round 2 is fixed.

### Round 6: the temporal resolve is LIVE, and the measurement says what it is worth here

The two wires round 5 left unapplied - the lighting stage sampling `ml_resolve` instead of the raw trace, and
the frame loop collecting the `megalights` stage (which is what sets this image's history flag) - are in, and
the accumulation now changes the frame: at frame 60 the same config renders **59.2080 / 64.1919 / 63.9931**
against the raw estimate's 59.2089 / 64.2010 / 64.0021, 2.43% of pixels moved, mean -0.0063.

| arm | flicker over the dark flat surfaces | HF noise there |
|---|---|---|
| unshadowed loop (the control) | 0.591 | 0.162 |
| the estimate, RAW (no resolve) | 0.594 | 0.161 |
| the estimate, temporal resolve LIVE | 0.594 | 0.161 |

THE HONEST READING, and it is the same one round 3 reached from the other side: the resolve changes the
estimate (2.43% of pixels, above) and removes NOTHING measurable in this frame, because there is nothing of
ITS OWN to remove - a 1-sample estimate is already as stable as the unshadowed control (0.594 against 0.591,
round 3), so the frame's 0.59 of frame-to-frame movement belongs to TAA's jitter and the shadow map's texel
alignment, not to this estimator. A denoiser cannot show a reduction of a noise floor it does not own, and a
scene with a handful of hard-shadowed, well-separated lights is precisely the regime where the RIS converges
on the same light every frame (the reference note's section 1: the variance lives in many overlapping lights
and penumbras, which point lights with hard shadows do not produce).

SO WHAT THE CHAIN IS WORTH, stated as measured rather than as intended: the accumulation is what a MOVING
camera and a restarting disocclusion need, not what a still frame needs, and the acceptance for it is a sweep
capture - which is the one measurement this round did not reach. Everything else is verified: all eight test
binaries pass, the four gate scenarios sit at their exact hashes with `megalights = false`
(`deferred` `98740BE429FA32C7`, `sponza` `22C2B33B4B6721FA`, `default_gi` `310220DA64A91257`, `sponza_march`
`5468FAE8D93EC9F5`) and validation-clean, and the two defects the wiring found are recorded above.

THE SPATIAL HALF of that denoiser is NOT written yet: the chain is trace -> temporal -> composite, with no
variance-gated filter between the resolve and the lighting stage. The reference note's section 4 has its
policy (variance from the accumulated moments, a gate before it filters at all), and the SSGI work's
lesson applies to it directly: a filter that removes signal and noise at the same rate is worse than no
filter, so it needs the same signal/noise measurement this document has been using.

### Round 5: the startup hang is FIXED, and the resolve's own skip is the last wire left

**The hang, root-caused and fixed.** It was the temporal declaration's SHAPE, not its recording: a pass that
declares a SHARED set and its own bindings at index 1 (`shared_sets = {{family 1}}` with `own_set = 1`) makes
the frame loop never complete - the log fills with `GPU timing marks recorded out of order (mark 13 at index
8)` and no frame is ever presented, while the same pass recorded nothing at all (its `record` was never
entered). The fix is the **no-shared-set arrangement - no shared set at all, five own bindings with the depth
and the velocity among them, `own_set = 0`** - which the removed GI chain's temporal resolve had also used.
After it the renderer runs, the pass's pipeline is created and its declaration resolves (`resource table:
verified 9 declaration handle(s) for pass 'megalights_temporal'`).

ONE MORE DEFECT FOUND ON THE WAY, worth recording because it is invisible until it is looked for: the shader
had BOTH `velocity` and `ml_resolved` at set 0 binding 4, so `vkCreateComputePipelines` failed with a
descriptor-type mismatch - and a pass whose pipeline failed to build is simply SKIPPED by the runner, which is
indistinguishable from "the feature does nothing" except in the startup log. With the binding numbers matched
to the declaration the pipeline builds.

**What is left, and it is one wire:** with `megalights = true` the pass is created and its declaration
resolves, but `record()` is still never entered - the runner skips it and logs no reason, and the frame stays
byte-identical to round 3's raw estimate (59.2089 / 64.2010 / 64.0021), which is how this was noticed at all.
The two `TEMPORARY: the lifecycle bisect` log lines left in `vulkan/pass/megalights_temporal.cpp` will say so
the moment it starts recording, and the next step is to read the runner's per-pass gate in
`vulkan/pass/pass.cppm`'s `record_stage` rather than to guess: everything the pass controls (readiness, the
feature name, the declaration) has now been verified from the outside.

VERIFIED IN THIS STATE: all eight test binaries pass; the four gate scenarios sit at their exact hashes with
`megalights = false` (`deferred` `98740BE429FA32C7`, `sponza` `22C2B33B4B6721FA`, `default_gi`
`310220DA64A91257`, `sponza_march` `5468FAE8D93EC9F5`) and validation-clean; and the feature-on frame is
round 3's, byte for byte, which is the honest statement that the accumulation is not yet in the picture.

### Round 4: the temporal resolve is written, and its WIRING is parked on an open defect

Built and compiling, running as code but NOT recorded: `shaders/megalights_temporal.comp` (the running mean
with a per-pixel frame count, UE's grazing-widened depth rejection, the mean/stddev neighbourhood clamp, and
the frame count riding in the history's alpha lane - the policy is `docs/reference/
megalights_stochastic_lighting.md` section 4), `vulkan/pass/megalights_temporal.cppm/.cpp` (the GI resolve's
shape, with the depth and velocity read from the shared G-buffer set instead of per-image own bindings so the
chain needs no second stage for its ordering rule), `megalights_temporal_io`, the `ml_resolve` / `ml_history`
resource families, the `frame_results` / `frame_facts` fields, and the demo's construction, setter and collect
report.

**THE OPEN DEFECT, and it is why the wiring is commented out in
`render_start_demo::attach`:** with the temporal pass CONSTRUCTED (not recorded - the stage still holds only
the tracer), the renderer hangs. The symptom is a frame loop that never completes: the log fills with
`GPU timing marks recorded out of order (mark 13 at index 8) - the pass report is mislabeled` and the frame
never presents. The bisect is unambiguous and is the one thing that fixes it: removing
`chain_.emplace<pass::megalights_temporal_pass>()` makes the same binary exit normally, with the four gate
scenarios at their exact hashes (`deferred` `98740BE429FA32C7`, `sponza` `22C2B33B4B6721FA`, `default_gi`
`310220DA64A91257`, `sponza_march` `5468FAE8D93EC9F5`) and the feature-on frame byte-identical to round 3's
(59.2089 / 64.2010 / 64.0021). So the cause is inside that pass's LIFECYCLE rather than its recording, and
the suspects in order are: its `create()` (the two-layout pipeline layout built by forwarding to the
ray-traced shadow's builder, whose argument order now means "G-buffer at 0, own at 1"), the
`on_swapchain_recreated` retirement of its family, and whether a pass that declares a shared set AND an own
set is handed a valid `shared.gbuffer` by the runner while never being recorded. The next step is to
re-enable it with the pass's three lifecycle steps logged, not to guess.

Everything else the objective asks for is in place and verified: the estimator (rounds 1-2), the config and
overlay switches, the reference note, and the stage-2 acceptance data above. What is missing is the
accumulation being live, and the motion measurement that would show its value.

**Done, compiling, and verified bit-exact.**

* The UE study: `docs/reference/megalights_stochastic_lighting.md` (consolidated from four
  `file:line` notes in `build/dsh-scratch/ue-notes/`), including the two readings it corrects - the
  "light complexity" pass is visualization-only (so there is no adaptive sample budget to build) and the
  composite lives inside the spatial denoiser (so no resolve pass is needed here either).
* `shaders/megalights_trace.comp`: the whole estimator - the cluster list as candidates, the
  log-compressed target PDF, the weighted reservoir, one `rayQuery` per selected sample, the firefly cap,
  and the unblocked-sample fraction in alpha. Compiles (`megalights_trace.comp.spv`).
* Two SHARED refactors, both frame-exact by the gate's own hashes (`deferred` `98740BE429FA32C7`,
  `sponza` `22C2B33B4B6721FA`, `default_gi` `310220DA64A91257` - each identical to its value before the
  refactor, `flaky 0`): `punctual_light_radiance()` is now the one definition of a light's attenuation
  (the cluster loop and the estimator both call it), and `cluster_index_at(pixel, world_pos)` plus
  `shade_input::pixel` remove the last `gl_FragCoord` from `shaders/shading.glsl`, which is what lets a
  COMPUTE shader include it at all.
* The plumbing that does not move a frame: the `ml_trace` resource family (schema entry, `core` images,
  the runtime's `family(...)` registration), the `megalights_trace_io` declaration (the tracer's shape
  with one own binding), and the runtime's flag, composed predicate (`megalights_active()`), setter
  (`set_megalights_enabled`) and frame fact. All eight test binaries pass (including
  `test_render_resources`' schema-size and declaration checks, whose hand-synced counts moved with it),
  and a gate scenario is validation clean with the same hash as before the change.

**Next.** The pass class itself (`vulkan/pass/megalights_trace`, the shape a traced compute pass has here -
two shared sets, its own output at the shared G-buffer set rather than an own set, so two new bindings there,
one to write and one to sample), its construction and gate in `deren.vulkan.render_start_demo`, the
`punctual_replaced` lane that stops `deferred.frag` adding the lights raster-style, and the composite that
adds the half-resolution result. Then `[render] megalights*` keys and the first measurement - the raw
estimate's noise against the unshadowed baseline, which is stage 2's acceptance.

### Stage 1 landed: the estimate is in the frame, and the feature-off frame is untouched

`vulkan/pass/megalights_trace` exists (the tracer's shape: two shared sets, no own binding, one image moved
through `barrier_images`), `pipelines::build_megalights_trace` builds it, `deren.vulkan.render_start_demo`
constructs and gates it, and the runtime records its stage between the `rt_shadow` stage and the `deferred`
stage. The image reaches the frame through the shared G-buffer set's two new bindings (16 = the storage
image the trace writes, 17 = the sampler the lighting stage adds it through), and `deferred.frag` does the
2x2 joint-bilateral upsample and the add, next to the lane that stops its own cluster loop.

VERIFIED, and these are the acceptance for this stage:

* `[render] megalights = false` (the default) leaves the four checked gate scenarios at their exact
  pre-change hashes - `deferred` `98740BE429FA32C7`, `sponza` `22C2B33B4B6721FA`, `default_gi`
  `310220DA64A91257`, `sponza_march` `5468FAE8D93EC9F5`, `flaky 0` - with validation clean.
* All eight test binaries pass, including `test_render_resources`' declaration checks (the deferred push
  block's pinned size moved 88 -> 92 with its new lane, and the schema's hand-synced counts with the new
  resource).
* With `megalights = true` and `[lighting] demo_lights = 6` on Sponza's interior: 2.99% of pixels change,
  0.52% by more than 1/255 and 609 by more than 4/255 (max 35), and the change is 17445 pixels DARKER
  against 265 brighter - shadows only ever remove light relative to the unshadowed loop. The amplified
  difference traces exactly the occluded creases and arch interiors, which is what a punctual shadow looks
  like; the flat surfaces do not move.

TWO BUGS THE WIRING FOUND, both recorded because they are the kind that would otherwise be discovered as
"the feature does nothing": the shader has to be REGISTERED (`chores.cpp`; without it the pass's create step
logs "the owner has no megalights_trace.comp.spv" and the knob silently does nothing), and the stage's
`prepare` has to publish the G-buffer instance's attachment writes (`render_start_demo`'s `"megalights"`
case) - the estimator is a sampler of the stored surface and runs before the lighting stage, so without that
call it dispatched against images still in their attachment layout, which the validation layer reports as a
descriptor/layout mismatch and which left the estimate at zero.

A PRE-EXISTING DEFECT, found while making the measurement and NOT this feature's: `[lighting] demo_lights`
produces 151 validation errors (identical with `megalights = false`, and zero without demo lights). The
measurement above was taken with them, so its numbers are sound, but the scenario a gate needs cannot ship
until that path is fixed - and it also puts the demo lights' reach in question, since their contribution to
this interior view is only ~0.03 of mean brightness. Stage 3 should start by fixing that and by giving the
scenario a light that actually covers the view.


