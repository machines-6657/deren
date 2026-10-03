# Descriptor heap migration: handover

**STATUS: this document is the HISTORICAL RECORD of a finished migration.** Everything below was
written while the work was in flight, so where it uses the present tense, read the past: the frame
that was black renders, and the descriptor-set world it describes as still present - the set layouts,
the pools, the per-pass descriptor families, the pipeline layouts and the whole mapping shim - has
been DELETED, commit by commit, with the proof each one carried (see
`docs/descriptor_heap_migration.md`, "Deletions ... ALL DONE"). What is still worth reading here is
the reasoning and the measured bugs: this is where the design's shape comes from, and where the bugs
that made the frame black and the fallback texture nobody wrote are recorded with their numbers.

The work rode on the `descriptor-heap-migration` branch while it was in flight, because the migration's unit is the whole frame and a half-migrated frame renders nothing (hash `DC5F6D66428C26D8`, mean `0.00` against the reference's `88.1`, *explained* rather than merely observed: see (e) below). It is FINISHED - the branch carries the conversions and the deletions that followed them, and the commits that end the story, with the byte-identical gate proof each one carried, are recorded in `docs/descriptor_heap_migration.md`.

## What is done, and what verifies it

1. **Every descriptor is in the heaps.** A fixed slot grid at a 1 MiB base with a 64 B stride
   (`core::heap_slots`, mirrored in `shaders/heap_slots.glsl`), the sampler grid at 64 KiB with a 32 B
   stride, and a host write site for every slot the shader header names - `tests/test_render_resources.cpp`
   fails if a name appears in one and not the other, and it also pins the values, the scalars and the
   sampler order.
2. **Every shader is heap-native.** `layout(set = ` appears **zero** times in `shaders/`; the shader
   build is green. Nineteen files were converted, including the ray-tracing pair and the three compute
   stages.
3. **Every pipeline is created against `layout = VK_NULL_HANDLE`** - graphics through
   `core::pipeline`, compute and ray tracing through the builders in `pipelines.cppm`.
4. **Every push travels as data.** `vkCmdPushConstants` appears **zero** times outside comments in
   `vulkan/`: all 21 sites now go through `vkCmdPushDataEXT`. The frame's passes use
   `resolved_io::push_block`; the primitive draws use `render_environment::push_block`; the mask bake,
   the skinning dispatch and the shadow cascade use endpoints of their own shape.
5. **The frame binds the heaps once**, in `runtime::begin_recording`, and the per-frame and
   per-swapchain-image indices ride in each stage's own push block, appended by the endpoints
   (`push_stage_block` = 3 lanes, `push_index_block` = 2, `push_raw_block` = 0).
6. **The mechanism was proven before it was applied**: a compute probe and a graphics probe read a
   written slot and returned the written value, and a deliberately wrong slot returned a different
   one. That negative proof is the reason the rest was worth doing at all.

## What remains

### (a) Descriptor binds - DONE

`vkCmdBindDescriptorSets` now appears **zero** times in `vulkan/`. Every bind is gone: the scene pass's
per-segment bind and the transparent pass's per-secondary one (each of which existed because a secondary
inherits no state from its primary - the heap bind has the same property, and the runtime makes it when
it records the buffer), plus the sets in `fxaa`, `gbuffer_debug`, `cluster`, `deferred`, `post` (both
record paths), `taa`, `megalights_trace`, `megalights_temporal`, `rt_shadow` and the shadow content in
`runtime.cpp`. `scene_pass::record_segment` lost its `scene_set` parameter with them, which is why its
call sites and its declaration in `scene.cppm` changed too, and `runtime::record_shadow_content` lost
its only use of `pipeline_layout` (now `[[maybe_unused]]`, because the callback signature it must match
belongs to the shadow pass).

Note that the guards which gate on these binds (`io.pipeline_layout == VK_NULL_HANDLE`,
`env.layout != VK_NULL_HANDLE`) still *pass*, because the runtime still hands out
`scene_pipeline_layout` - so none of this was caught by an early return, and none of it was dead code.

### (b) The post chain's source slot - DONE

The third push lane now carries it, so the host hands the absolute slot over and the shader indexes with it
(`docs/descriptor_heap_migration.md` records the fix); the note below is the reasoning that made it a
separate piece of work rather than a two-line change.

`shaders/post.frag` reads `post_source_texture[pc.post_source_slot]` - and that index is an
**absolute** heap slot, not a base plus `heap_image_index` (contrast the four bloom levels two lines
below it, which are `bloom_lN_texture[heap_slots_bloom_lN + heap_image_index]`). So the *host* has to
resolve the source, per frame, per swapchain image:

| which stage | its source slot |
| --- | --- |
| prefilter (`post_pass`, `level_ == 0`, mode 0) | `post_color + io.frame.image_index` |
| downsample (mode 1, `level_` > 0) | `bloom_l0 + (level_ - 1) * 8 + io.frame.image_index` |
| composite (mode 2) | `post_color + io.frame.image_index` (it takes the bloom levels itself, by constant) |

`post_color` is 639 and the bloom chain is 647/655/663/671 - stride 8, those are the
`core::heap_slots` / `heap_slots.glsl` constants.

**Why this is not a two-line fix.** A pass may not import `deren.vulkan.core`: no pass does today (they import
`deren.vulkan.core.handles` and `deren.vulkan.render_resource`, and the split is deliberate - the grid is the
renderer's, and a pass that knows it could take over an image family). So the pass cannot name 639. The
host must hand the slot over, exactly as it hands over `shared_set_layout`, `shared_pipeline_layout` and
`push_block`: a callback on `pass_context`/`pass_host` that answers "what is the post chain's source
slot for this image at this level", filled in `runtime::make_pass_host` and in the pass context the
runtime builds - the same shape as the three callbacks already there. Until then `post.cpp` pushes the
default lane of `0`, and the shader would read slot `0 + 0` - the bindless texture array's first
entry - rather than its source.

### (c) The objects nothing points at any more - DONE

They were created, written and read by no shader, and they have since been deleted: `core::scene_pipeline_layout` and
`scene_descriptor_set_layout` (`vulkan/core/core.cpp:1536`, `:1412`), `core::create_descriptor_pool`
(`:1353`), `runtime::scene_sets`, `runtime::gbuffer_family`, `runtime::post_family`,
`pipelines::make_post_set_layout` / `make_gbuffer_set_layout`, `bindings::make_set_layout` /
`write_set` / `image_set_family`, the passes' `pipeline_layout_` / `set_layout_` members and their
`pipeline_layout()` / `set_layout()` accessors, and the `shared_set_layout` / `shared_pipeline_layout` /
`descriptor_set` context plumbing. Deleting them is the whole point of the migration's title, and it is
purely subtractive work that did not affect what the frame does.

### (d) The mapping shim - DONE

`pipelines::scene_heap_layout`, `scene_heap_stage_mapping`, `set_scene_heap_layout`,
`map_from_heap` in `build_cluster`, `runtime::push_heap_frame_slot` and
`descriptor_heap::make_mapping` are now unreachable - `map_from_heap` is `static constexpr bool = false`
and the mapping pointer it guards is therefore never built. They went with the rest of the shim.

### (e) What was measured on the first runs

Two runs of `--config build-release-clang64\render-check\default.toml --capture-frames 1` with
validation on. **The first** found two blockers; both were fixed and the second run passed them:

1. **The heap flag is not optional when the layout is null.** Measured:
   `vkCreateComputePipelines(): pCreateInfos[0].flags (VkPipelineCreateFlags2(0)) does not include
   VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT while layout is VK_NULL_HANDLE`
   (`VUID-VkComputePipelineCreateInfo-None-11367`). The rule is *both or neither*. Four compute
   builders had no `flags2` struct at all, `build_cluster` had one but chained it only on the mapping
   path, and the ray-tracing builder chained none - all now set the bit. **Fixed.**
2. **`spirv-val` rejected the SPIR-V the shared `heap_texel` helper generated**:
   `OpFunctionCall Argument <id>'s type does not match Function <id>'s parameter type`, every failing
   call a call to that helper (`VUID-VkShaderModuleCreateInfo-pCode-08737`). A `texture2D` parameter
   does not survive a function boundary. `heap_texel` is a **macro** now, which expands the fetch at the
   point of use. **Fixed** - and note for the next reader: glslang's preprocessor rejects
   `__VA_ARGS__` outright ("'#define' : bad argument"), so it is a fixed three-parameter macro, which is
   enough because every uv argument keeps its commas inside parentheses.

**The second run** then created every pipeline - pbr, unlit, gbuffer, gbuffer-debug, shadow, deferred,
megalights trace and temporal, TAA, ray-traced shadow (raygen + miss + hit + any-hit), the post chain,
fxaa, the mask bake and the compute skinning - ran the frame loop and saved
`screenshot_20260918_133008.png`. The heap probes still pass (slot 16896 reads `255,255,255,255`,
16897 reads `0,0,0,255`). Three things remain, all precisely identified:

**1. The helper functions - FIXED.** `gbuffer_texel` (deferred.frag) and `downsample_13` / `sample_tent`
(post.frag) took a `texture2D` **parameter**, the same thing that broke `heap_texel`. They take the image's
heap **slot** (a `uint`) now and index the array inside, so the fetch is built where it is written and no
image crosses a function boundary. Indexing one declared array for every caller is sound because every
heap array view is a view of the *same* resource heap - the slot chooses the image, the name does not. After
this change the run reports **no `OpFunctionCall` error at all**: every shader module validates.

**2. The inherited heap bind - FIXED, and it removed every validation error.** A secondary is validated on
its own, so the primary's bind never reached it (`VUID-vkCmdDrawIndexed-None-11308`). The two bind infos
`record_bind` builds are now a separate `descriptor_heap::bind_infos` (one definition, two destinations),
and all three secondaries chain a `VkCommandBufferInheritanceDescriptorHeapInfoEXT` whose `pNext` is the
rendering info they already passed: `scene.cpp`'s segment begin, `transparent.cpp`, and the shadow
cascade in `runtime.cpp`. The passes receive the infos through a `fill_heap_bind` callback on their frame
structs - the heap itself stays the renderer's, exactly as with `make_environment` and `push_block`.

**3. The post chain's source slot - FIXED.** The third push lane now crosses the line it needed to: a post
stage passes WHICH source it reads (0 = the HDR target the chain starts from, N > 0 = bloom level N - 1)
and `runtime::push_stage_block` turns that into the absolute slot the shader indexes, image index
included. Before this every level read the HDR slot, so the whole bloom chain and the composite were
sampling the wrong image.

**After all of that the run is CLEAN: zero `[ERROR]` lines, everything created, frames recorded,
screenshot saved.** And the frame is still a uniform black image whose bytes are identical across
scenarios and across every one of the fixes (`DC5F6D66428C26D8`). That is the state to pick up, and it
is a *data* problem now, not a binding one: the draws are legal, the heaps are bound and inherited, the
probes prove that a heap read and a heap draw both work, and no push is refused.

**What is measured away, all of it by experiment.** Take these as settled - do not re-derive them:

* **The scene pass draws.** A one-shot diagnostic in `record_segment` printed `1 leaf/leaves, heap push
  endpoint SET`: the pass iterates its leaves and the environment carries the endpoint.
* **The pushed frame slot.** Forcing `heap_frame_slot` to a literal `0u` in `pbr.vert` changed nothing.
* **The slot arithmetic.** Both sides agree: a per-image array steps by the RAW image index
  (`gbuffer_albedo + image_index`; `core.cpp` writes image `i` at `heap_slot_base + i`), while different
  arrays are `heap_image_capacity` (8) apart (`bloom_l0 + level * heap_image_capacity`).
* **The push-lane length.** Two lanes instead of three produced six validation errors *and* the same hash.
* **The push block layout.** 8 uints + `alignas(16) glm::mat4` = 96 = `scene_push_constant_size`, and the
  shader's block is the same 8 uints then `mat4`, so the lanes land at 96/100 exactly as declared.
* **The vertex stage, entirely.** With `gl_Position` computed by hand - no camera UBO at all - and the model
  matrix replaced by the identity, the frame is *still* black.
* **The image index, and the capture path.** Painting the post chain flat red changed the captured hash
  (`90BC07F22EE6BBD2` against the black `DC5F6D66428C26D8`), and that red reached the capture *through*
  `fxaa.frag`'s read of `display_texture[heap_slots_display_color + heap_image_index]`. So the index the
  shaders are given is the image being captured, the capture reads what the passes write, and the post
  chain - which runs in the PRIMARY - renders.
* **The G-buffer fragment stage.** Forcing a flat green albedo in `gbuffer.frag` left the G-buffer debug
  view black, so the scene's fragments never execute at all - it is not that they write black.
* **The inheritance chain order.** Chaining the heap info onto the rendering info instead of the other way
  round produces two validation errors and the same black hash: the original order is the validated one.
* **The viewport.** Dynamic viewport/scissor are set per pipeline bind (`vk_pipeline::begin_pipeline`), so
  a secondary gets them; the scene path is not missing them.

**What that leaves.** Every link in the chain has now been printed and verified correct, including the ones
that were only assumed before:

| link | measured |
| --- | --- |
| the draw is issued | `[diag] normal draw: index_count=46356 vertex_count=14556 vertex_detail=true index_detail=true` |
| the instance | `5 color attachment(s), depth YES; renderArea 1080x960` |
| viewport AND scissor | `viewport 1080x960 at (0, 0) depth [0, 1], SCISSOR 1080x960 at (0, 0)` |
| depth state | `make_depth_stencil_state`: `depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL`, cull BACK, front face CCW |
| blend state | `make_color_blend_attachment_opaque()` - `blendEnable = VK_FALSE` |
| the G-buffer descriptors | written to `heap_slots::gbuffer_albedo + i` (etc.) at creation - the same slots the shaders name |
| **the image itself** | `[diag] heap gbuffer albedo for image 0 is image 0x250000000025 view 0x270000000027` and `[diag] scene target 0 image 0x250000000025 view 0x270000000027` - **the scene renders into exactly the image and view the heap descriptor names** |
| the heap bind | validation is silent, which is itself the proof: an unbound heap is `VUID-vkCmdDrawIndexed-None-11308` |
| the unlit path | also black, so it is not the lighting |
| culling, discard, vertex fetch, camera, model matrix, the whole fragment stage | each neutralised in turn - no change |

And the two facts that bound it from the other side: the **post chain renders** (flat red changed the
captured hash to `90BC07F22EE6BBD2`), and that red reached the capture *through* `fxaa.frag` reading
`display_texture[heap_slots_display_color + heap_image_index]`, so the per-image index and the capture
path are correct.

**The last question, and it needs an instrument outside the code.** Writes to per-image target slots work
(the post chain), a material-table read works (the graphics probe, slot 512), and a per-image target
*read* works too - `fxaa.frag`'s read of `display_texture[display_slot + image_index]` returned the red the
composite had just written. And the scene renders into exactly the image the heap descriptor names
(`image 0x25 view 0x27` on both sides). So the fault is in none of those places, and what remains is the
one thing no log inside the renderer can answer: **does a fragment from the scene's indexed draws reach
the image at all?** The draw is issued (46356 indices, valid geometry), and the instance is open in the
submitted command buffer with the right attachments, render area, viewport, scissor, depth state, blend
state and pipeline. Read the G-buffer albedo image back with a copy right after the frame's submit, or
take a GPU capture (RenderDoc) of the scene pass. Everything else in this document can wait for that
answer.

**The measurement worth carrying forward:** the black frame's hash is `dc5f6d66428c26d8...` - byte for byte
the hash this migration recorded earlier as "a half-migrated frame renders nothing". It is a *uniform*
image, which is why it survived every fix that changed what the frame does.

## How to finish and verify

```powershell
cmake --build build-release-clang64            # must be exit 0
ctest --test-dir build-release-clang64         # 13 binaries
pwsh scripts\windows\check_render.ps1          # the scenario set, changed must be 0
```

Then re-do the negative proof as a *frame-level* check (push a wrong slot on purpose and confirm the
frame changes), and merge to `pass-chain` only when the gate reports its scenarios unchanged with
validation silent.

The rules the extension actually enforces - the five VUIDs, the null-layout requirement, the Slang and
glslang restrictions that shaped the shader side, and the counted work list this document is the tail of - are
in `docs/descriptor_heap_migration.md`.


## The root cause, found and fixed (measured, with RenderDoc unavailable)

RenderDoc 1.46 cannot capture this renderer at all, and the reason is on RenderDoc's side: its capture layer
HIDES `VK_EXT_descriptor_heap` from the application. Measured with `vulkaninfo` - without the layer
`VK_EXT_descriptor_heap : extension revision 1` is listed (498 `VK_` lines); with
`ENABLE_VULKAN_RENDERDOC_CAPTURE=1` it is gone (403 lines). Under the layer the app therefore takes its
no-heap fallback (that log line then read `descriptor sets stay the binding model`; it now says the heap is the only binding model this renderer has, because there are no descriptor sets left to stay) and dies when
the heap-native shaders are compiled. GFXReconstruct fails the same way. So the diagnosis was done with
instruments inside the renderer instead, and they found the bug.

**A heap IMAGE descriptor written while its image is still in `VK_IMAGE_LAYOUT_UNDEFINED` NEVER RESOLVES.**
That is the whole black frame. The creation loops write every target's descriptor in the same breath as
`vkCreateImage` - i.e. while the image is UNDEFINED - and **every shader that sampled one of them read
zero**, while the image itself was fine:

* the scene's 46356-index draw reaches the G-buffer: copying `gbuffer_images[0][i]` to a buffer and reading
  it back gives `1f 32 36 fe` at the centre, while the deferred's *sample* of the same slot gives 0;
* the BRDF LUT - uploaded and transitioned BEFORE its descriptor was written - samples correctly
  (`0xffffb3ff` through the heap sampler, `0xffff0000` through `texelFetch`), and the material table (a
  BUFFER descriptor, which has no layout) always read `0xffff`. So image sampling, the sampler heap and the
  slot arithmetic were all fine; only the *target* descriptors were dead;
* rewriting the G-buffer descriptor again once the image was defined made the deferred's sample return the
  real albedo immediately (`0x3BC7` = 0.1216 x 8 in the HDR), and forcing the deferred's output to a
  constant reached the HDR too - so the draws, the targets and the descriptors were never the problem.

**The fix** is in `runtime::resolve_pass`: every per-image target descriptor (G-buffer x3, depth, velocity,
taa_current, taa_history, post_color, display_color, the megalights chain and its storage twins, and the four
bloom levels) is rewritten while the frame is being resolved, i.e. once the images are certainly in a defined
layout. `BUFFERS ARE UNAFFECTED`, which is why the material table, the camera/light UBOs and the clusters
worked all along - that asymmetry is what made this so hard to see.

**Still to do after this fix:** the frame does not come out yet - the lighting stage now reads its G-buffer
correctly, but the content still does not survive to the swapchain, so at least one more link is broken
(TAA resolve, the megalights chain, the composite's source lane, or FXAA). The next measurement is the
multi-image read-back that located this one: copy the swapchain, `hdr`, `scene_color`, `ldr` and the
G-buffer albedo into one staging buffer per capture and print each centre pixel - that shows exactly which
stage drops it. The read-back code is in this branch's history (it was reverted; re-add it in
`record_screenshot_copy`).

## The bug the write path hid: a fallback texture nobody gave a heap slot

**Symptom, measured at the gate's own camera.** Sponza rendered every indirectly-lit surface solid
black: mean luma 33.67 with 72.24% of pixels below luma 12, against the official reference's 61.17 /
13.66%. It was NOT the shadows - `shadow = false` produced a BYTE-IDENTICAL frame - and not SSAO,
SSGI or the stochastic chain either (`ssao = false`, `ssgi = false`, `megalights = false` and even
`irr_size = 1` / `env_size = 16` were all byte-identical, with the config dump proving those keys were
loaded). `unlit = true` was bright and complete (mean 120.04) and every G-buffer channel was fully
populated (albedo 118, normal 196), so geometry, textures and materials were fine and the fault was
entirely in the shading.

**The renderer had already said so.** Its own startup probe reads grid slot 16384 - the first slot of
the bindless texture array - and the log had read for weeks:

    the heap-native probe sampled grid slot 16384 ... read back 0x00000000 (texture red 0x0000, alpha 0x0000)
    the material table's DEFAULT record read 0xffff (its white base colour is 0xffff)

**Cause.** The 1x1 white fallback texture is created in the runtime's constructor and never passes
through `register_material`'s heap-write loop, so its slot stayed EMPTY. Every material slot with no
texture points at that index - Sponza's stone carries no occlusion map - so `s.ao` sampled as 0, and
`shaders/shading.glsl` multiplies BOTH the diffuse ambient and the specular IBL by `s.ao`: those
surfaces were left with the sun's direct light and nothing else. That is also why it looked
Sponza-only: the assets whose materials DO carry an occlusion map (DamagedHelmet, and the five
scenarios built on it) were untouched, and the metal sweep was the opposite of insensitive - `s.ao`
scales the specular IBL too, which for a metal ball is nearly the whole image.

**Fix and proof** (commit `f714580`): one `write_image` at creation. The probe now reads
`0xffffffff`; sponza went 33.67 -> 61.24 mean and 72.24% -> 13.51% dark against the official 61.17 /
13.66%; MetalRoughSpheres' two scenarios went 117.79 and 118.46 -> 123.02 and 123.23 against 123.00
and 123.19; the blend asset landed exactly on 122.24. The five helmet scenarios did not move at all,
which is the prediction the diagnosis made before the fix was written, and the gate reported exactly
those four changed and no others.