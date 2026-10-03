# Deformation-aware motion vectors: the previous-frame skin and morph data TAA is missing

The design for the renderer's one remaining systematic error in its anti-aliasing path, and the
instrument that has to exist before the fix can be accepted at all.

> **A NOTE ON THE FILE REFERENCES, since two migrations have moved under them.** Every `shaders/pbr.vert` /
> `shaders/shadow.vert` reference below is to the source as it stood when this feature was implemented: the Slang
> migration moved those bodies into `shaders/pbr.slang` / `shaders/shadow.slang` (the GLSL originals are in
> `shaders/glsl.old/`), and the mesh-shader migration then removed those files' vertex ENTRIES altogether
> (docs/mesh_shaders.md step 4) - the morph/skin/motion code they argue about now lives in the shared
> `pbr_shade_vertex` body that the mesh entries call. The measurements, the fixtures and the acceptance are
> unaffected; only the paths changed.

## 1. The gap, stated by the code

`shaders/pbr.vert:149-154` builds the previous world position like this:

```glsl
const uint motion_index = push.motion_base + ((push.flags & 1u) != 0u ? gl_InstanceIndex : 0u);
v_prev_world_pos = (previous_transforms[heap_previous_slot].matrices[motion_index] * local_pos).xyz;
```

`local_pos` is this frame's position - morphed at `:119`, skinned at `:139`. So the previous world
position is the **deformed** vertex pushed through the **previous node matrix**: the node's rigid motion
is exact and the deformation is not represented at all. A vertex that moved 20 cm inside its own object
space contributes zero to the motion vector.

The code says so out loud. `shaders/gbuffer.frag:83-88`: "A deforming mesh is still approximate: a
skinned or morphed vertex moves inside its own object space as well, and the previous-frame skin matrices
/ morph weights that would describe that are not stored yet." `docs/shaders.md:79-82` repeats it, and
`docs/mainpage.md:84-89` lists it as what is still ahead on this path.

Why this is the next thing rather than a polish item: TAA is the engine's **only** anti-aliasing (a 1x
G-buffer cannot be multisampled, `docs/mainpage.md:65`), and every deforming mesh in every scene violates
its reprojection assumption at every frame. TAA's neighbourhood clamp (`shaders/taa.frag:112-125,148`)
absorbs part of the residual; what is left is the ghost.

Nothing stores the missing half today. `motion_previous` (`vulkan/runtime/runtime.declarations.cppm:243`)
holds world matrices only, and the only deformation state that exists is the **current** frame's, one
buffer per frame slot (`skin_buffers` / `morph_buffers`, `MAX_FRAMES_IN_FLIGHT = 2`,
`vulkan/core/core.declarations.cppm:607`). A grep for previous-frame state across `vulkan/runtime/`
returns `prev_view_proj` and nothing else.

## 2. The model

Three decisions, and everything else follows.

**1. The previous-frame deformation lives in a buffer with the SAME layout, the SAME indices and the SAME
frame slot as the current one.** The shader reads the same `push.skin_base` from a different heap slot, so
no addressing change, no per-draw bookkeeping and no new push lane is needed for the skin half. The
addressing is worth stating plainly, because it is the first thing a reviewer will challenge:
`heap_frame_slot` is the **current** frame slot (aliased per stage at `shaders/pbr.vert:87-88`), so a
buffer holding the block that was current one frame ago is reachable as
`heap_slots_skin_matrices_previous + heap_frame_slot` - and that is exactly how the rigid half already
reaches its own past. `advance_motion_transforms()` publishes the one-frame-ago state **into the current
frame slot's** buffer (`vulkan/runtime/runtime.frames.cppm:224,240`) rather than leaving it in the other
slot, which is why `heap_previous_slot` needs no lane (`shaders/heap_slots.glsl:122`), and
`advance_motion_deformations()` does the same by construction (decision 2). The alternative - leave the
previous data sitting in the other frame slot and address it with a host-resolved `1 - frame_slot` through
the existing `extra_lane` mechanism (`vulkan/runtime/runtime.cpp:869-871`) - is refused: it would make the
deformation half address its own past differently from the rigid half, for no gain.

The push budget is why this matters, and it was measured rather than assumed: the scene / G-buffer leaf
push occupies **108 of `pass::max_push_bytes = 128`** - the 96-byte `material_push_constants`
(`scene_push_constant_size`, `vulkan/core/core.declarations.cppm:87`, held to it by the `static_assert` at
`vulkan/primitive/primitive.cppm:488`) plus three 4-byte lanes `{frame_slot, image_index, extra_lane}`
(`runtime.cpp:849`); the limit is at `vulkan/pass/pass.cppm:184` and the validator refuses anything past it
(`vulkan/render_resource/render_resource.cppm:719-721`). **20 bytes stay free**, and only the morph half
needs one of them.

**2. The publication is the runtime's, once per frame, in the same place as the rigid publication.**
`runtime::advance_motion_transforms()` (`vulkan/runtime/runtime.frames.cppm:220-244`, called at `:335`)
already does exactly this for world matrices: it copies the CPU-side one-frame-ago state into the current
frame slot's GPU buffer (`:240`), then refreshes the CPU copy from the frame's live state (`:241`). The
deformation version is `advance_motion_deformations()`, called immediately after it - same slot
(`vulkan_core.current_frame`, `:224`), same read-then-refresh order. The animation controller keeps no
role in it, so no controller path can desync the two halves.

**3. Only the WEIGHTS of a morph block are previous-frame state; the deltas are static, and the previous
weights live INSIDE the block.** The morph scratch is `[per vertex per target: dpos(3) dnrm(3)][weights]`
(the MorphData note in `shaders/pbr.vert`, baked at `vulkan/animation/controller.cppm:564-593`) and only
the trailing weight region is rewritten per frame (`controller.cpp:245-256`). The previous-frame morph
data is therefore **weights only**, stored as a SECOND weight region of the same block:
`[deltas][weights: targets][previous weights: targets]` - 2 * target_count floats (292 for
AnimatedMorphCube, 40 for SimpleMorph), not the 32 MiB a mirror of the scratch would cost.

**THE DEVIATION FROM THIS DOCUMENT'S FIRST DRAFT IS THE INTERESTING PART**, and it is recorded because the
draft planned a push-block lane plus a compact buffer in an index space of its own: that lane is not
available. `material_push_constants` is 96 bytes with **all eight of its uints used**, and its
`alignas(16) glm::mat4 model` cannot move - a ninth field pushes the matrix from offset 32 to 48 and the
block to 112 bytes, which drags every GLSL block's field offsets, the scene pipeline's push range, the
shadow stage's cascade lane (108 -> 124) and the test that asserts that offset. Measured against the
alternative: the in-scratch region touches nothing shared, and the only thing it costs is that a
per-frame weight writer owns one invariant of its own block.

That contract is stated where a writer reads it (`runtime::morph_scratch()`'s note): the vertex stage
reads the FIRST weight region as this frame's weights and the SECOND as the weights one frame ago, so a
caller that rewrites weights **per frame** must copy the first region into the second before overwriting
it - otherwise its mesh reports a morph deformation that is not happening. A caller that sets weights
once (at setup) has nothing to do, and `animation::controller::update`, the only per-frame writer in this
tree, bakes both regions equal and keeps them in step. The shared kernel of the two designs is decision
2: the previous state is published ONCE per frame, next to the rigid publication, and never computed at
draw time.

Both halves inherit the rigid path's own approximation, deliberately: "previous frame" means the frame
before this one, not "the last frame that rendered this swapchain image" - which is what the rigid half
already means. Making the deformation half per-image correct while the rigid half is not would be a new
inconsistency, and it needs the `image_view_proj` treatment (`runtime.declarations.cppm:516`). Out of
scope, stated.

## 3. What this design deliberately does NOT do

* **Transparent geometry.** The other half of `docs/mainpage.md:88`'s sentence: the transparent pass
  composites after the lighting stage and writes no velocity, so TAA cannot reproject a blended surface
  at all. Separate work, separate gate.
* **`shadow.vert`.** It duplicates the skin and morph paths (`:104-123`) and needs no change: a shadow
  needs a pose, not a motion.
* **`compute_skin.comp`** (the `rt_skin_bake` re-skin, `:44,53,103-110`). Same reason, and the new
  previous-frame buffers must not be touched by it.
* **`shadow_geometry_signature`** (`vulkan/runtime/runtime.frames.cppm:516-526`). It folds
  `skin_matrix_hash` and `morph_revision`, and it must keep folding exactly those: the new buffers never
  affect a shadow, so they must not enter the fingerprint.
* **Per-swapchain-image deformation history** (section 2, decision 3).

## 4. Implementation, in the order it gets built

### Step 0 - the instrument and the asset (BLOCKING, and it is two items)

Nothing in the tree can currently show this bug or prove the fix, for two independent reasons, both
verified:

**(a) There is no reproducible capture of a DEFORMING mesh.** `[render] animation_time`
(`config.example.toml:82-90`, applied at `main.cpp:421-427`) pins the pose, and a pinned pose uploads the
same skin matrices every frame - so the previous-frame deformation equals the current one and the
deformation term of the motion vector is **exactly zero**. The pinned-pose capture cannot distinguish the
fixed renderer from the broken one. With the pose unpinned, `main.cpp:699-700` drives the controller from
`frame_clock.delta_seconds()` - the wall clock - so the pose at a given frame count differs run to run
and no gate can exist.

This project already made exactly this argument once, for the camera, and built the answer:
`--capture-sweep` advances the camera by a fixed number of degrees **per frame index** precisely so that
"a capture move[s] ... not per wall-clock second, so a sweep capture is as reproducible as a still one"
(`main.cpp:34-44`), and the gate already uses it (`glossy_motion`, `scripts/windows/check_render.ps1`).
The animation clock now needs its sibling: a frame-indexed advance (`--capture-animation-sweep
<seconds per frame>`, or equivalently making the controller's time a function of the frame index under
capture). **Without it this feature cannot be accepted at all**, which is why it is step 0 and not a
convenience.

Acceptance for (a): two runs of one binary with a nonzero animation sweep produce one PNG hash
(`Get-FileHash -Algorithm SHA256`), and the sweep actually deforms the mesh (the last two frames differ).

**(b) There is no animated skinned or morphed asset.** Verified: the repository ships
`gltf_model/DamagedHelmet.gltf` and nothing else. `build-release-clang64/gilberta.glb` is a static model -
its GLB JSON chunk declares 1 mesh, 2 nodes, no `skins`, no `animations` and no extensions. The only
skinned asset in the tree is the hand-written fixture `tests/fixtures/shared_skin_two_nodes.gltf`
(1 skin, 2 nodes, `JOINTS_0` / `WEIGHTS_0` / `inverseBindMatrices`) and it has **no animation channel**,
so it cannot deform.

Two options, both in the grain of this repository: extend the fixture family with a hand-written animated
skin (and a morph sibling) - small, committable, and sufficient for a numeric acceptance; and/or fetch a
glTF-Sample-Assets model (Fox or CesiumMan for skinning, AnimatedMorphCube for morph) as a **dev-only**
asset for the large-scale visual measurement, never committed.

Acceptance for (b): the model loads, plays, and the **pre-change** binary renders the bug into the
baseline - a static camera, a deforming mesh, `[render] gbuffer_debug = true` +
`gbuffer_channel = 8`, and the deforming region reads flat olive. The channel is
`motion * motion_gain + 0.5` with `motion_gain = width * 0.25` (`shaders/gbuffer_debug.frag:108-115`,
`vulkan/pass/geometry_buffer_debug.cpp:134`), i.e. **"+0.5 bias means 'did not move' reads as flat
olive"** - and today that is what a deforming mesh reports.

### Step 1 - the skins

The half that needs no push-block change, and it covers the assets that skin rather than morph.

* **the heap family**: `heap_slots_skin_matrices_previous`, 2 slots (per-frame families span 2 slots,
  `MAX_FRAMES_IN_FLIGHT = 2` at `vulkan/core/core.declarations.cppm:607`), declared in **both**
  `shaders/heap_slots.glsl` (beside `:56`) and the `struct heap_slots` in
  `vulkan/core/core.declarations.cppm` (beside `:543`), plus a `heap_skin_previous_slot` macro beside
  `shaders/heap_slots.glsl:123`. The grid is `heap_slot_base = 16384`, `heap_slot_stride = 64`,
  `heap_slot_count = 1024` (`core.declarations.cppm:496,513-514`; `core.constructor.cppm:97-110` reserves
  it), the used span is 743 slots, so the free tail is 743..1023 - the two new families belong there
  (743 and 745), not in the middle of a family that would then have to be renumbered. Two ctest rules
  decide this rather than review: `tests/test_render_resources.cpp:659,661` parses **both** files and
  asserts that the grid's scalars and every array's slot agree (a one-sided edit fails), and `:692-702`
  requires every named shader slot to be referenced as `heap_slots::<name>` by some host unit - which the
  `write_heap_scene_buffer` call below is, and would not be if the family were addressed any other way.
  One recorded follow-up: the "703 slots are in use" comment in `shaders/heap_slots.glsl:89` and
  `core.declarations.cppm:514` is stale - it stops at `display_color` + 8 and ignores `tlas` (703,704),
  `rt_visibility_storage` (711..718), `ml_trace_storage` (719..726) and `ml_resolved_storage` (735..742).
* **the buffers**: `skin_buffers_previous` + its mapped pointer, created beside
  `runtime.constructor.cppm:340-351` and published through `write_heap_scene_buffer` beside `:622`; plus a
  host `std::vector<glm::mat4> skin_previous` holding one frame of history, exactly as `motion_previous`
  does (`runtime.declarations.cppm:243`).
* **the publication**: `advance_motion_deformations()` next to `runtime.frames.cppm:335` - inside
  `begin_recording()`, which is after the controller's documented write window (`controller.cppm:640-650`:
  after `pace_and_acquire()`, before `begin_recording()`; the call site is `main.cpp:700`), so what it
  reads has already been written. It reads the slot `vulkan_core.current_frame` and refreshes the CPU copy
  from the just-uploaded `skin_mapped[slot]`. It is the only writer. A frame on which the controller does
  not run leaves the two copies equal, so the deformation term is exactly zero - the desired answer rather
  than a special case.
* **the initial state**: `skin_previous` starts as the 4-matrix identity block that the controller already
  writes into every slot at setup (`controller.cppm:413,492-495`), so a vertex with no history reads the
  bind pose. The one frame that could be wrong is covered by TAA's own first-frame rule: `history_valid = 0`
  makes the resolve return the current frame exactly (`shaders/taa.frag:152-153`,
  `vulkan/pass/taa.cpp:71,175`).
* **the shader**: in the skin branch (`shaders/pbr.vert:128-141`), run the same joint sum a second time
  against the previous-slot buffer to get `prev_local_pos`, and use it at `:154`. `push.skin_base` (`:75`)
  is unchanged, and so is the frame-slot lane the block already carries - the base index and the slot are
  both identical in the two buffers, which is decision 1's whole point.
* **untouched**: the push block, `shadow.vert`, `compute_skin.comp`, `shadow_geometry_signature`.

Acceptance:
* `check_render.ps1` byte-identical - the gate's models are all rigid, so any movement there is a bug in the
  shared path;
* a **pinned-pose** capture of the skinned model byte-identical before and after the change: a pinned pose
  uploads the same matrices every frame, so the deformation term is provably zero. That is the null test
  which says the new path *can* be zero, and it is the only before/after equality a deforming asset can
  offer;
* the motion channel non-flat over the deforming region at a **swept** pose (a pinned pose cannot show it);
* the TAA-on-vs-TAA-off difference at that swept pose **drops** against the step-0 baseline;
* ctest 13/13, doxygen exit 0, validation clean.

### Step 2 - the morph weights

* **the block layout**: a second weight region per morph block,
  `[deltas][weights: targets][previous weights: targets]`. No push lane, no heap family and no buffer -
  see decision 3 for why the draft's lane does not exist.
* **the bake** (`controller.cppm`): writes the default weights TWICE, so both regions start equal and an
  un-animated morphable primitive reports no deformation instead of its distance from zero weights; the
  capacity check reserves `2 * target_count` floats per primitive.
* **per frame** (`controller.cpp:245-256`): copies the current weight region forward into the previous one
  BEFORE overwriting it - one memcpy per rig per frame.
* **the shader**: `shaders/pbr.vert` blends the static deltas a second time with the previous weights and
  uses that as the morph half of `prev_local_pos`, which the skin blend then runs on, so the two compose
  as `skin_previous(morph_previous(v))`.
* **the documented contract**: `runtime::morph_scratch()`, `material_push_constants::morph_base` and the
  shader's MorphData note all state the layout and the writer's obligation.

Acceptance: the same list as step 1, on morph assets. Measured on AnimatedMorphCube (its weights animate
and its node does not move, so the rigid half is exactly zero): **100.00% still -> 99.99% moved** (303,749
of 303,791 px, mean deviation 123.03, 14,144 distinct values), its pinned-pose null test **100.00% still**,
and SimpleMorph **100.00% still -> 99.48% moved**. The flag's step-1 numbers are unchanged to the digit
(65.59% moved, mean 17.144), so the two halves do not disturb each other.

### Step 3 - the documents

`docs/shaders.md:79-82`, `shaders/gbuffer.frag:83-88`, `docs/mainpage.md:84-89` and the README's TAA
feature line all state the gap as current. Each becomes false and is rewritten to the **new** residual:
what stays approximate is transparent geometry, per-swapchain-image deformation history, and a mesh whose
deformation changed while its swapchain image was recreated.

## 5. Acceptance

Every step: Release gate 14 scenarios x 2 with 0 changed and 0 flaky, Release/Debug build clean, ctest 13/13,
doxygen exit 0 with an empty warning stream, every run validation clean. The boundary is the project's
own and is recorded in `docs/pass_io_design.md:351-357`: **the capture gate is a RELEASE instrument** -
Debug and ASan+UBSan builds are measured non-deterministic here, so they are held to "clean build, ctest,
and a run that reports no finding" instead.

Two ctest checks carry steps 1 and 2 specifically, and neither can be satisfied by review:
`tests/test_render_resources.cpp:659,661` (the grid's two sources of truth agree) and `:692-702` (every
named shader slot is written by a host unit); if the morph lane ends up sharing the lane group, `:503`
(the shadow cascade's offset) joins them.

The specific instruments, all of which exist:

```powershell
# the regression that keeps this fixed - seed the references BEFORE the change
pwsh -File scripts/windows/check_render.ps1 -List
pwsh -File scripts/windows/check_render.ps1 -Update
pwsh -File scripts/windows/check_render.ps1            # each scenario runs twice and must hash-equal itself first
# $env:DEREN_BASELINE_DIR overrides %LOCALAPPDATA%\deren\baseline

# the baseline of the BUG (step 0): static camera, swept animation, motion channel
./build-release-clang64/deren.exe --config my.toml --capture-frames 40 `
  --capture-camera=35,20,7,0,-1.6,0 --capture-animation-sweep=0.02   # gbuffer_debug=true, gbuffer_channel=8
```

and, for the A/B at one swept pose, `python scripts/measure/diff.py`, `tiletab.py` and `mean.py` on the
TAA-on and TAA-off captures, with `spec_material.py` for a per-material reading from channels 2/3/1.
`scripts/measure/README.md` fixes the unit: every recorded number is **mean green**.

Once the fixture is committed, the deformation scenario joins `check_render.ps1`'s list (`:87-134`) with
its own config (`taa = true`, `gui.show = false`, a pinned frame-indexed animation sweep) - that scenario
is the deliverable's regression, and it is the reason step 0(a) is not optional.

## 6. What would make this fail, refused in advance

* **Previous-frame deformation drifting out of sync with the current frame.** One writer
  (`advance_motion_deformations()`), one call site, the CPU copy refreshed only there. A second writer -
  the controller writing the previous buffer directly - is the bug this design exists to prevent.
* **The 32 MiB morph scratch double-buffered.** Refused: the deltas are static and the weights are the
  only per-frame state. A step that copies the deltas is rejected even if it measures faster, because it
  buys 32 MiB per frame slot for nothing.
* **Deformation per-swapchain-image correct while the rigid half is not.** Refused as scope, section 2.
* **A change that moves a rigid frame by one bit.** The gate decides: if `deferred`, `sponza`, `unlit`,
  `shadow_single` or `metal_rough_glossy` change, the edit reached the shared path. Deforming draws only.
* **A performance claim.** The deliverable is correctness. The GPU frame here is ~0.5 ms (the README's
  measurement at 960x720), the doubled vertex math is paid only by deforming draws, and the bar is "no
  regression beyond the instrument's noise floor" - not a speedup.
* **A pinned-pose screenshot offered as evidence.** By section 4 step 0(a) it cannot distinguish the two
  renderers, so it is not evidence; the submission must state the swept pose and the frame count.
* **A claim the gate cannot decide.** The gate's scenarios are rigid; they prove the shared path did not
  move, and they prove nothing about deformation. The deformation claim is carried by the pinned-sweep
  capture and the motion channel, or it is not carried.

## 7. Cost

| item | cost |
| --- | --- |
| host `skin_previous` | 128 KiB (`scene_skin_capacity = 2048` x 64 B, `primitive.cppm:426`) |
| GPU skin mirror | +128 KiB x 2 frame slots = 256 KiB |
| GPU previous morph weights | `2 * targets` floats per morphable primitive INSIDE the existing morph scratch (292 floats for AnimatedMorphCube, 40 for SimpleMorph) - no new buffer and no new heap family |
| heap grid | 2 new slots (ONE family x 2 frame slots: the previous skin matrices), placed in the free tail (743); both files, ctest-checked |
| push block | NONE - and that is why step 2 deviates from this document's draft: `material_push_constants` is 96 bytes with all eight of its uints used, so a ninth field moves the `alignas(16)` mat4 from offset 32 to 48 and grows the block to 112 (decision 3) |
| vertex shader | the skin/morph sum runs twice, on deforming draws only |

For scale, what is NOT paid: an extra copy of the morph scratch (32 MiB per slot) and any new pass,
image, descriptor family or render target.

## 8. Status

| item | state |
| --- | --- |
| the gap, documented by the code | DONE - `shaders/gbuffer.frag:83-88`, `docs/shaders.md:79-82` |
| step 0(a) frame-indexed animation sweep | DONE - `--capture-animation-sweep`; byte-neutral without the flag (the rigid reference capture stayed `77EFE3C2D80A3C34F3B3229CC91AC27A9A47440A2393B8C0677795E0AB8E1C53` before and after it), two runs of one sweep hash-equal, and two different rates produce two different poses |
| step 0(b) animated skinned fixture + the instrument | DONE - `scripts/make_animated_skin_fixture.py` -> `tests/fixtures/animated_skin_plane.gltf` (169 vertices / 288 triangles / one pinned joint + one animated about Z / 5 keyframes over 2 s), and `scripts/measure/motion_channel.py`. The bug's baseline: 229,267 foreground px, **100.00% still**, **one** distinct value |
| step 1 the skins (heap family, buffers, publication, vertex stage) | DONE - `skin_matrices_previous` in both grid files, the per-slot previous buffer + its CPU copy, `runtime::advance_motion_deformations()`, and the second skin blend in `shaders/pbr.vert`. Measured on the swept fixture: **0.00% -> 65.59% moved** (150,371 px, mean deviation 17.14, worst 104, 597 distinct values) with the pinned left half correctly still (34.41%); the PINNED-pose null test **100.00% still**; the rigid reference capture byte-identical; ctest 8/8; zero validation findings. On a REAL character from the sample assets (Fox, 24 joints): swept **100.00% moved** (266,727 px, 218 distinct values) against **100.00% still** when the pose is pinned - every skinned pixel reports the motion it has, and none reports any when nothing changed |
| step 2 the morph weights (a second weight region per morph block) | DONE - the block is `[deltas][weights][previous weights]`, the bake writes both regions equal, `animation::controller::update` copies the current region forward before overwriting it, and `shaders/pbr.vert` blends the static deltas a second time with the previous weights and composes it with the previous skin matrices (`skin_previous(morph_previous(v))`). Measured on AnimatedMorphCube: **100.00% still -> 99.99% moved** (303,749 px, mean deviation 123.03, 14,144 distinct values); SimpleMorph **100.00% -> 99.48% moved**; both pinned-pose null tests **100.00% still**; the flag's step-1 numbers identical to the digit; ctest 8/8; gate 10/10 with 0 changed and 0 flaky; the rigid reference capture byte-identical |
| step 3 the documents that call the gap current | DONE - `docs/shaders.md`, `docs/mainpage.md`, the `shaders/gbuffer.frag` note and the layout notes in `pbr.vert`, `runtime::morph_scratch()` and `material_push_constants::morph_base` describe the finished state, and the one case that is still not carried is stated wherever the covered ones are: alpha-blended geometry |
| the deformation scenario in the capture gate | DONE - `check_render.ps1`'s `deformation` scenario captures gbuffer_debug's MOTION CHANNEL (a shaded frame cannot show a wrong motion vector with TAA off), pinned to the repo's own fixture with `--capture-animation-sweep`; the gate is 14 scenarios x 2, 0 changed, 0 flaky |
| transparent geometry's motion vectors | NOT STARTED, deliberately separate (section 3) |
