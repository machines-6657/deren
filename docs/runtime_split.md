# Splitting `deren.vulkan.runtime`: the map, the boundaries, and the acceptance

This is the plan for cutting the renderer's monolith into submodules so that the dependencies between its parts
become declared rather than positional. It is written before any code moves, because the object being cut is
3187 lines of interface and 6523 lines of implementation, and the interesting part of that is not its size.

## 0. Status: this map has been walked

The split this document maps has since happened, so the plan below reads as the record of what was measured
before any code moved. `deren.vulkan.runtime` is now an umbrella (`runtime.cppm`, 26 lines) over
`runtime.declarations.cppm` (3855), `runtime.frames.cppm` (3144), `runtime.constructor.cppm` (1839),
`runtime.cpp` (1890), `runtime.probes.cppm` (365) and `runtime.readback.cppm` (153), and the pass extraction
the acceptance section asks for has landed.

Two parts of the map describe code that no longer exists, and are kept only as that record: C1's G-buffer
descriptor set (its generator and its writes went with the descriptor-family plumbing - the header of
`vulkan/bindings/bindings.cppm` is the surviving account) and C8's `image_set_family` / `scene_bindings`,
`set_ssgi` and the SSGI / GI-probe families (the traced-GI subsystem left in `1162d88`). The line counts in
section 1 are the pre-split measurements they were written as.

LINE COUNTS IN THIS DOCUMENT are `Get-Content .Count`. PowerShell's `Measure-Object -Line` undercounts a file
whose last line has no trailing newline - the same file reads 3021 and 3187 by the two methods - and this project
quotes numbers so they can be re-derived, so the method is part of the number.

## 1. Why, in the repository's own terms, not mine

The architecture this refactor moves TOWARD is already written down, so this is not a new design:

* `docs/mainpage.md` (87-88): "Most modules are independent building blocks that meet only through narrow
  interfaces, so you are free to recombine or rewire them";
* the same page (96-99) defines `deren.vulkan.core` / `deren.vulkan.runtime` as "a configurable facade ... the demo entry
  point (`main.cpp` + `deren.chores`) is a thin glue layer on top and can be replaced wholesale".

The measured shape of what exists instead:

    vulkan/runtime.cppm          3187 lines   (of the 3063-line class body only 779 are code:
                                              2126 comment-only, 158 blank; the file's code is ~826 lines)
    vulkan/runtime.cpp           6523 lines   164 `runtime::` definitions, 28 `record_*`, 11 `ensure_*`
    next largest module .cppm    1031 lines   (constant_init); everything else is <= 1018

So the interface's line count is mostly DOCUMENTATION and is not the problem. The problem is that one translation
unit and one class own the state and the behaviour of ten subsystems, and that the dependencies between those
subsystems are expressed as shared members, shared descriptor sets and a positional call order rather than as
interfaces. The operational test of a good boundary is:

    Can ONE new render pass be added by editing ONE module plus ONE line in `runtime`?

Every change this project made to the chain recently failed that test: adding two image families, three
descriptor bindings, one push-constant lane and a second dispatch of an existing pipeline each required edits in
`pipelines.cppm`, `core.cppm`, `core.cpp`, `runtime.cppm` and three separate places in `runtime.cpp`.

## 2. What is already extracted (do not re-propose these)

`deren.vulkan.pipelines` (all stateless pass-pipeline builders), `deren.vulkan.bindings` (`scene_bindings`,
`image_set_family` with its pool lifetime), `deren.vulkan.shadow_fit` (the cascade FIT math only - the gather and its
cache stay in runtime), `deren.vulkan.readback` (screenshot staging), `deren.vulkan.acceleration_structure` (the BLAS/TLAS
classes), `deren.vulkan.profiling` (CPU phases), `deren.vulkan.graphical_user_interface`, `deren.vulkan.core` with its nested parts (`filter`,
`handles`, `init_utils`, `deren.vulkan.core.pipeline`, `spirv_parser`, `vma`, `vma.handles`), plus `deren.vulkan.scene_tree`
and `deren.vulkan.render_environment`.

THE PRECEDENT THAT MATTERS: three prior extractions say "Extracted from deren.vulkan.runtime" in their headers
(`vulkan/pipelines/pipelines.cppm:10`, `vulkan/bindings/bindings.cppm:9`, `vulkan/shadow_fit/shadow_fit.cppm:9`) and all three are of the STATELESS/PURE kind - they
take handles and return builders. **No extraction in this repository has ever moved state.** Everything below is
therefore the first of its kind, and the acceptance criteria have to be stronger than "it compiles".

## 3. Who owns what today, which is not what the plan assumed

`core` - not `runtime` - owns every per-image target family: `vulkan/core/core.constructor.cppm:777-1283` creates HDR, bloom, LDR, the
G-buffer, velocity, the GI chain's images (`gi`, `gi_resolve`, `gi_history`, `gi_spatial`), the glossy lobe's
(`gi_spec`, `gi_spec_reproject`, `gi_spec_resolve`, `gi_spec_history`), the probe cache's eight coefficient images
plus its surface image, the furnace cube and the per-SLOT ray-traced shadow maps; the matching cleanup is one
lambda at `vulkan/core/core.constructor.cppm:1182-1282` and the swapchain-recreate path at `vulkan/core/core.cpp:207-380` (teardown `228-358`, rebuild
`360-378`). The only per-slot family `runtime` creates itself is the shadow map (`vulkan/runtime/runtime.cpp:659-678`).

CONSEQUENCE FOR EVERY BOUNDARY BELOW: "a submodule owns its image families" is not available as a move. The
honest target is that a submodule owns its **behaviour, descriptor sets, pipelines, push-constant structs,
per-frame flags and teardown**, while `core` remains the single allocator of images. Taking an image family over
is a resource-layer change and must be argued separately.

## 4. The couplings, ranked by what it costs to cut them

* **C1 - the G-buffer descriptor set (16 bindings).** Declared at `runtime.cppm:352`, written by one function
  (`runtime.cpp:3044-3140`), and read by the deferred lighting stage, the G-buffer debug view, the GI tracer, the
  GI spatial filter, the glossy lobe, the ray-traced shadows and the post set's depth/normal bindings
  (`2420-2501`). It also CARRIES the probe cache's four SH-2 coefficients (9-12) and the whole GI chain (5-8,
  13-15). Splitting this set per subsystem would change what the shaders declare, i.e. it is **not a move**.
  Every boundary that touches GI, post or shadows is bounded by this.
* **C2 - the positional `gpu_mark` sequence.** `gpu_mark_id` is a private nested enum (`vulkan/runtime/runtime.declarations.cppm:345-372`)
  whose ORDER is the contract (guarded at `vulkan/runtime/runtime.cpp:273-278`), with 15 call sites across the frame
  (`346, 710, 803, 2607, 2646, 2750, 2752, 2778, 2806, 2994, 3008, 3020, 3071, 3111, 3120`). A private enum
  cannot even be NAMED by an extracted module, so a pass that moves out cannot mark itself.
* **C3 - `light_state` as a god struct.** `vulkan/runtime/runtime.declarations.cppm:984`, 28 references; written by the shadow cascades
  (`vulkan/runtime/runtime.cpp:1046-1050, 1079`), by the clustered lights, the exposure path and the debug controls
  (`vulkan/runtime/runtime.frames.cppm:218-225, 1644-1669`), and READ by the GI probe cache's reset test (`3850`). A GI module would
  need the light-change signal without the light state.
* **C4 - one scene rendering instance for the whole frame.** Opened in `record_opaque_scene` (`vulkan/runtime/runtime.declarations.cppm:2247`) and
  closed in `record_scene_tail` (`vulkan/runtime/runtime.frames.cppm:2602`). Any pass that wants to record into the scene pass has to be called
  inside that window, which is a call-order coupling rather than an interface.
* **C5 - three per-image "written" flags** (`vulkan/runtime/runtime.declarations.cppm:861, 878, 895`) set in `record_scene_attachments`
  (`886-942`) and cleared in five other places (`2607-2608, 2885-2887, 3564-3565, 4132-4133, 4397`).
* **C6 - per-frame-slot vs per-swapchain-image identity** (`core::current_frame` vs `current_image_index`).
  Mixing the two is this project's documented per-image-lifetime trap (`vulkan/runtime/runtime.declarations.cppm:857-860`), and it has already
  produced one real bug (a single first-use flag guarding a per-image resource).
* **C7 - `current_ubo`** (camera and scene constants) is read 32+ times, including by the GI pushes
  (`3295-3347`), the TAA jitter (`vulkan/runtime/runtime.frames.cppm:159-164`), the cascade fit and the cluster grid.
* **C8 - the manual teardown and reset lists.** 11 raw pipeline layouts are named by hand
  (`vulkan/runtime/runtime.cpp:212-262`), and `on_swapchain_recreated` (`vulkan/runtime/runtime.cpp:212-262`) retires only FOUR of the SIX
  `image_set_family` instances - `ssgi_spec_temporal_family` (`runtime.cppm:516`) and `gi_probe_family`
  (`runtime.cppm:586`) are not in that list and survive only because `ensure()` re-detects changed views
  (`bindings.cppm:283-294`). That is a latent hazard rather than a bug today, and it is exactly the class of
  thing a boundary that owns its own family would make impossible.

## 5. The order, narrowest seam first

    1. probe cache        -> vulkan.gi_probe    218 cpp + 70 cppm, 8 members, ONE call per frame (`vulkan/runtime/runtime.frames.cppm:2602`)
    2. TAA                229 + 42              owns scene_target_image/view, which the scene path calls 4x
    3. screenshot         105 + 26              already half-extracted into deren.vulkan.readback
    4. shadow cascades    534 + 148             the only subsystem that owns an image family outright
    5. rt structures      656 + 111             its instance table is consumed by the GI chain
    6. post               338 + 71              post_push_constants is shared by bloom/composite/FXAA/GI-upsample
    7. GI chain           796 + 191             blocked on C1
    8. G-buffer/deferred  674 + 129
    9. scene resources    485 + 149
   10. scene graph        884 + 26
       frame loop         never - it is the facade the others are called from

**WHY THE PROBE CACHE GOES FIRST**, and why this supersedes an earlier draft of this plan that started with post:
it is the smallest COMPLETE state-owning unit in the runtime, its interface is exactly "`core&`, the scene set
handle, the G-buffer set handle, the resolved-GI views, a params struct, a command buffer", it is called once per
frame, and it exercises the whole pattern end-to-end WITHOUT touching a single shader - so the first state-owning
extraction is also the smallest risk that can still fail in every way the later ones can. Its one oddity: its
sampler is created inside `make_gbuffer_debug_pipeline` (`vulkan/runtime/runtime.frames.cppm:1220-1222`), which is a naming accident to fix while
passing through.

**Extraction 1 is DONE, and it landed as a PASS rather than as the `vulkan.gi_probe` module this map proposed.**
The boundary the project chose for a pass is `deren.vulkan.pass` (the framework) plus one module per pass
(`vulkan.pass.gi_probe`), because the thing being extracted is not "some code in a file" but a unit with a
DECLARED interface - and the declaration is what made the move provable: the frame is byte-identical
(`sponza_gi` back to 58EC848DFABE654A). What moved is narrower than the `218 cpp + 70 cppm, 8 members` above and
what stayed is on purpose: the pass owns its set layout, its two-set ping-pong family, its pipeline layout, its
pipeline, its barriers, its clear and its push; `deren.vulkan.runtime` keeps the switch, and the values the push block
is composed from (they are the renderer's). `record_gi_probe_pass`, `ensure_gi_probe_descriptors` and
`make_gi_probe_pipeline` are gone, and `gi_probe_valid`/`gi_probe_light_dir_valid` are the pass's state now. The
app's two calls are `register_shader` (it owns the file) and `create_passes` (the pass owns the pipeline). The
remaining entries in the order above are unchanged, and TAA is next - but it needed one thing this map did not
anticipate, because the map counted lines rather than shape: TAA is a FULLSCREEN GRAPHICS pass, and the framework
only drove compute. Its half of the framework (declared render targets, a graphics bind point, the
viewport/scissor a pass cannot forget) landed first and separately; the extraction order below is unchanged, only
the cost of entry 2 is now two steps instead of one.

**Extraction 2 is DONE too (`deren.vulkan.pass.taa`, version 0.1.0)**, and it cost the three things the map could not
see either:

* the SAMPLER was created inside the factory it was extracted from, so moving the factory deleted the sampler and
  the first frame under TAA crashed with a null sampler in a descriptor write (validation named it exactly). It
  is now created by `runtime::ensure_taa_sampler`, before `create_passes` fills the context - and the two
  samplers still made inside `make_gbuffer_debug_pipeline` are the same accident, still to be collected;
* the per-image descriptor family needed a GENERATION fingerprint, and the current image's views are exactly the
  wrong one (they differ every frame, which would rewrite sets a pending frame names). The pass caches the first
  frame's views and drops them in `on_swapchain_recreated` - the framework's recreate contract, used for
  something real for the first time;
* two lines of the resolve's own sequence belong to OTHER passes: the G-buffer depth's transition (its flag is
  the G-buffer pass's) and clearing the motion-vector flag. Both stay with the host, gated on the same predicate
  the runner gates the stage on, and they are the barrier/order stage's entry point - the first concrete one this
  sequence produced.

## 6. Step 1 - the file-level split of `runtime.cpp`, and its honest value

The build already lists a module's implementation units under `target_sources(... PRIVATE ...)`
(`CMakeLists.txt:340-377`, e.g. `core.cpp`, `shadow_fit.cpp`) while the `.cppm` sits in
`FILE_SET CXX_MODULES` (`279-339`), so the arrangement is expressible. Proposed 7 units: frame + scaffolding +
profiling + screenshot; scene resources/sets/materials/primitives; shadow; G-buffer/deferred/draw; GI + probe +
TAA; post; traced structures.

WHAT IT BUYS: navigability and per-TU build parallelism. WHAT IT DOES NOT BUY: the interface stays one file, no
boundary is enforced, and a new pass can still reach every member. So it is a real but PARTIAL step, and it must
not be mistaken for the refactor.

TWO THINGS TO SETTLE BEFORE COMMITTING TO IT, both cheap:

* **No module in this repository has two implementation units yet.** Whether the CMake/clang-scan recipe tolerates
  it could not be settled by reading the code. So the first move is a bounded experiment: move ONE function into
  a second TU, build all three configurations, and run the capture gate - 0 changed expected. If that fails, the
  Step 1 shape changes and this document gets a correction rather than a workaround.
* `vulkan/runtime/runtime.cpp:33` has a file-static `deren::utility::init_pmr()` that each new TU would duplicate; whether that is
  harmless or needs a per-TU guard is part of the same experiment.

## 7. Naming, placement and the build list

* Module names are FLAT peers (`deren.vulkan.graphical_user_interface`, `deren.vulkan.profiling`, `deren.vulkan.scene_tree`, `deren.vulkan.render_environment`)
  and only a region's INTERNAL parts nest (`vulkan.core.filter`, `vulkan.core.vma.handles`). The deciding
  question is whether `main.cpp`/`deren.chores` may touch it directly: if yes, a peer module (`vulkan.gi`,
  `vulkan.post`, `vulkan.shadow`, `vulkan.rt_scene`); if only `runtime` may, nest it.
* Documentation belonging is expressed with `@defgroup` + `@ingroup vulkan_runtime`, as
  `deren.vulkan.render_environment` already does.
* `CMakeLists.txt` lists every `.cppm` explicitly, so each extraction adds lines there. The self-check for that
  is part of every step: introduce a deliberate error in the new module and confirm the build FAILS - a file in
  the tree that no target compiles is a silent failure this project has no other guard against.
* Version banners: a new module starts at 0.1.0 and `deren.vulkan.runtime` bumps with each extraction (it is 0.55.0
  today).

## 8. Acceptance for every step, and the hazards that are NOT behaviour changes

Every step is a MOVE, so its acceptance is the strongest control this project has:

* `scripts/windows/check_render.ps1`: the core set, 5 scenarios x 2, **0 changed**, 0 flaky; Release, Debug and ASan+UBSan
  clean; `ctest` 13/13; `doxygen Doxyfile` exit 0 with an empty warning stream; every run validation clean.
* IF A MOVE CHANGES A FRAME, the reason is found and stated before anything is re-seeded. Re-seeding a reference
  to make a refactor pass is how a refactor hides a behaviour change.

Non-behavioural hazards the gate will catch but that deserve to be known in advance:

* `update_pass_geometry`'s hand-maintained pipeline list (`vulkan/runtime/runtime.frames.cppm:945-1031`): dropping a pipeline from it makes a post
  pass set a 0-wide viewport;
* moving a "first-use" flag without its reset sites (constructor `vulkan/runtime/runtime.constructor.cppm:154-163`, `on_swapchain_recreated`
  `vulkan/runtime/runtime.cpp:212-262`, `set_ssgi` `3194-3195`) changes FRAME ONE, which is exactly what a byte-exact gate notices and
  what a "looks fine" eyeball does not;
* `scripts/windows/capture.ps1:194` parses the exact log line "shadow mapping enabled: light frustum center ...
  radius" produced by `enable_shadows` (`vulkan/runtime/runtime.cpp:1107-1108`): moving or rewording that line breaks the harness's
  scene-radius report, i.e. the measurement instrument depends on a string.

## 9. What would make this fail, stated so it can be refused in advance

* A module that still needs `runtime&`. There are two today: `sub_render_task::owner` (`vulkan/runtime/runtime.declarations.cppm:2358` ->
  `vulkan/runtime/runtime.declarations.cppm:2358`) and the `render_environment` binders that capture `this` (`vulkan/runtime/runtime.cpp:1860-1874`). If an
  extraction's interface is "a reference to the thing we are splitting", the boundary is wrong and the honest
  answer is not to extract it yet.
* C1 (the shared G-buffer set) blocking GI, post and shadows at once. Until it is addressed - and addressing it
  means changing shader binding declarations, i.e. a behaviour-visible change with its own gate - those three
  cannot be cleanly separated.
* `light_state`, the shared rendering instance (C4), the per-image flags (C5) and `gpu_mark` (C2), each of which
  turns a would-be interface into a negotiation.
* `active_features()` read from five places (`vulkan/runtime/runtime.frames.cppm:230, 2217`) - a query that several subsystems
  answer for each other.
* Teardown order: `vulkan/runtime/runtime.constructor.cppm:179-211` must `wait_idle` before any module's layouts die, which is safe only
  while the modules are members declared AFTER `vulkan_core` (`vulkan/runtime/runtime.declarations.cppm:168`). An extraction that changes
  declaration order changes destruction order.

## 10. What this buys the work that comes next

The extension this is meant to enable is real and already identified: the glossy reflection is accumulated at
the chain's half resolution with a single ray per pixel, and its one-ray estimate differs from an eight-ray
convergence on 7.37% of pixels (2.80% by more than 4/255), which TAA cannot reduce because it is a bias rather
than flicker. Adding a spatial pass for that signal should be: edit `vulkan.gi`, add one line to `runtime`.
Whether that sentence becomes true is the only measure of whether this refactor worked.
