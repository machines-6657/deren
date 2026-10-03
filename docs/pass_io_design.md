# `vulkan.pass_io`: describing a pass's inputs and outputs the way Vulkan already does

The layer every pass in this renderer describes its inputs and outputs with, and the design the code
implements: the types below live in `vulkan/render_resource`, and the per-pass declarations are its consumers.

**STATUS NOTE, because this document's premise is now half history.** It is built around descriptor
sets - "a pass's inputs and outputs are ALREADY descriptor sets", below - and those are gone: the
renderer is heap-native, and `vkCreateDescriptorSetLayout` / `vkCreateDescriptorPool` /
`vkAllocateDescriptorSets` / `vkCreatePipelineLayout` / `vkCmdBindDescriptorSets` appear zero times in
the tree. What this document argues FOR is what survived: ONE declaration per pass, from which the
renderer derives what it needs, instead of the same interface written twice by hand. What it argues
AGAINST - the parallel array of views, the parallel array of `VkDescriptorImageInfo`, the ternaries
deciding storage-vs-sampler, the hand-written set layouts in `pipelines.cppm` and the `ensure_*` writes
in `runtime.cpp` - is exactly what the deletion removed. So read the sections below as the reasoning
that produced the declaration layer, not as a description of how a pass's resources reach the GPU
today: they reach it through the frame's bound heap, addressed by the slot the stage pushes.

## 1. Why this shape, and what it is not

A pass's inputs and outputs in this renderer are ALREADY descriptor sets - the G-buffer set is the interface
between the G-buffer pass, the lighting stage, the stochastic punctual lighting chain and post; the scene set
is the substrate; each pass's private family (TAA 4 bindings, post 9, the lighting chain's temporal resolve 5)
is its own I/O. What exists today is that interface written TWICE BY HAND and kept in agreement by discipline:

* the layout, in `vulkan/pipelines/pipelines.cppm` (`build_resolve_pipeline` builds a 5-binding layout with a
  loop whose storage case is `b == 4u`, `build_gbuffer_debug` a 16-binding one whose storage cases are enumerated);
* the descriptor WRITES, in `vulkan/runtime/runtime.cpp`'s `ensure_*_descriptors()`, as a parallel array of views, a
  parallel array of `VkDescriptorImageInfo`, and ternaries that decide storage-vs-sampler and which sampler.

Both drifts this pair can have are already in this project's history, and both were found by the validation
layer rather than by review: a pool sized for four descriptors per set while the layout asked for five, and a
binding whose type changed without its writer noticing. This design makes the two come from ONE declaration.

IT IS NOT a render graph and not a mini-RHI. It does not allocate images (`core` keeps doing that - it is the
only place the complete image list exists), it does not order passes or derive barriers in its first version,
and it does not renumber a single shader binding. What it does is: one declaration, from which the layout, the
writes and the per-type descriptor counts are generated, checked against the shader that must consume them.

## 2. The model

Three ideas, and everything else follows from them.

1. **A binding is a use.** One record says "at (set, binding) this pass touches THIS resource, this way". The
   `VkDescriptorType` follows from the kind, the image layout follows from the kind, the resource's identity
   follows from the resource - so the three things that are hand-maintained in parallel today are one line.
2. **A resource has an identity, a SCOPE and a LIFETIME, and the scope is the trap made explicit.** This
   project's documented per-image-lifetime trap is the difference between a per-frame-slot resource (shadow
   maps, light/camera/skin buffers, scene sets) and a per-swapchain-image one (TAA history, every GI image,
   the probe's ping-pong), and it has already produced a real bug (one first-use flag guarding a per-image
   resource). In this model a resource cannot be referenced without saying which it is.
3. **The declaration is data, and it lives beside the pass.** Constexpr tables, no allocation, no registration
   order, no discovery: the same style as `gbuffer_formats`, `gpu_timing_labels` and the `static_assert`s that
   pin `light_ubo`'s and `instance_record`'s layouts.

## 3. Naming

The repository's conventions, which this follows: module names are flat peers under `vulkan.`, the namespace
mirrors the module's suffix (`namespace deren::vulkan::bindings`), types are `snake_case`, a builder's result carries
`_owned`, and capacities/constants are lower_snake_case.

    module            deren.vulkan.render_resource               (doxygen: @defgroup vulkan_render_resource)
    namespace         deren::vulkan::render_resource
    files             vulkan/render_resource/render_resource.cppm  (CMake: FILE_SET, as every module)

| name | kind | what it is | why this name |
| --- | --- | --- | --- |
| `resource_id` | `enum class : uint32_t` | one enumerator per resource FAMILY that `core` already owns (`gbuffer_albedo`, `gi_trace`, `gi_spec_reproject`, `shadow_map`, ...) | the family is the unit `core` creates, destroys and recreates; UNLIKE `gpu_mark_id`, **the order carries no meaning** - only identity |
| `resource_scope` | `enum class : uint8_t` | `per_frame_slot` / `per_swapchain_image` / `device_wide` | the project's own vocabulary for the trap (`docs/runtime_split.md` C6) |
| `resource_lifetime` | `enum class : uint8_t` | `per_frame` / `persistent` / `imported` (`imported` = the swapchain image, owned outside) | says whether a first-use transition or a clear is needed at all |
| `resource_info` | `struct` | `{ resource_id id; std::string_view name; resource_kind kind; resource_scope scope; resource_lifetime lifetime; }` | mirrors `VkDescriptorSetLayoutBinding`'s role for resources: what it is, not where it lives |
| `resource_kind` | `enum class : uint8_t` | `image2d` / `image3d` / `image_cube` / `buffer` / `accel_struct` | decides the descriptor type together with `binding_kind` |
| `binding_kind` | `enum class : uint8_t` | `sampled_image` / `storage_image` / `sampler` / `uniform_buffer` / `storage_buffer` / `input_attachment` | 1:1 with `VkDescriptorType`; named after the Vulkan concept rather than "read/write" because the DESCRIPTOR is what the layout is built from |
| `binding_access` | `enum class : uint8_t` | `read` / `write` / `read_write` | the access is NOT derivable from the descriptor type (the spatial filter only READS its `gi_input` storage image), and it is what a later barrier stage keys on |
| `sampler_hint` | `enum class : uint8_t` | `gbuffer` / `taa` / `post` / `nearest` / `shadow` | this renderer creates FIVE samplers today (`gbuffer_sampler`, `taa_sampler`, `post_sampler`, `post_nearest_sampler`, `shadow_sampler`) and which one a binding gets is currently a ternary; naming the choices makes it a field |
| `pass_binding` | `struct` | `{ uint32_t set; uint32_t binding; binding_kind kind; resource_id resource; uint16_t element; uint16_t descriptor_count; binding_access access; sampler_hint sampler; VkShaderStageFlags stages; }` | **the heart**: one binding, one use. `binding` alone would collide with the `deren.vulkan.bindings` module, hence the `pass_` prefix |
| `image_layout` | **REMOVED** | was `sampled` (SHADER_READ_ONLY_OPTIMAL) / `general` / `color_attachment` (COLOR_ATTACHMENT_OPTIMAL) | REMOVED WITH `VK_KHR_unified_image_layouts`: every image this renderer owns is in GENERAL now, so a declaration has no layout left to state and the validator's "a storage image must declare GENERAL" rule has nothing to check. It had been ADDED after the first conversion because the layout was NOT derivable from the kind (the probe cache kept all nine of its own bindings in GENERAL, so a descriptor claiming SHADER_READ for one of them was a lie validation rejects). The history is kept here for one reason: it is why step 4 below no longer takes a layout as input at all |
| `render_target` | `struct` | `{ resource_id resource; uint16_t element; }` | an image a pass RENDERS INTO. Not a binding, and the distinction is not cosmetic: a binding is a descriptor, the layout generator walks that list, and a colour attachment has no `VkDescriptorType` at all - it is bound by `vkCmdBeginRendering`. The load op and clear value are deliberately NOT declared: the pass that renders into the image is the one that opens the rendering instance, so it is the one that says whether the old contents matter |
| `push_block` | `struct` | `{ uint32_t offset; uint32_t size; VkShaderStageFlags stages; }` | the second push range already exists in this codebase (`scene_cascade_push_offset/size`), so the shape is not hypothetical |
| `pass_io` | `struct` | `{ std::string_view name; uint32_t own_set; std::span<pass_binding const> bindings; std::span<render_target const> targets; std::optional<push_block> push; }` | the declaration. `name` is for the error messages the validator produces, not for dispatch |
| `make_set_layout` | function | `expected<VkDescriptorSetLayout, std::string> make_set_layout(VkDevice, pass_io const&, uint32_t set)` | a straight replacement for the hand-written binding loops in `pipelines.cppm`. It takes the DEVICE rather than the core, as the generators and the descriptor families now all do: they need nothing else, and that is what lets a PASS build its own layout and write its own sets out of what its host hands it |
| `set_pool_requirements` | function | per-`VkDescriptorType` counts for one set | the number that had to equal the layout by hand and did not once |
| `write_set` | function | `expected<void, std::string> write_set(VkDevice, pass_io const&, uint32_t set, VkDescriptorSet, span<VkImageView const>, span<VkBuffer const>, sampler_set const&)` | replaces the parallel arrays and ternaries in `ensure_*_descriptors` |
| `resource_views` | `struct` | the owner hands in the actual `VkImageView`/`VkBuffer` per `resource_id` for one (image_index, slot) | the one thing that must stay with the resource's owner; it is why this layer needs no `runtime&` |
| `validate` | function | `expected<void, std::string> validate(pass_io const&, spirv_reflection const&)` | checks the declaration against what the SHADER actually declares, using the existing `deren.vulkan.core.pipeline:spirv_parser` |

Names considered and rejected: `vulkan.graph` (implies order derivation, which is a later stage and not what
this module is), `vulkan.descriptors` / `vulkan.sets` (the repository already has `deren.vulkan.bindings`, which owns
set OWNERSHIP and pool lifetime; this module owns DECLARATION), `vulkan.resources` (it owns no resources),
`vulkan.passes` (reads like pass implementations, which is what it is meant to keep out).

## 4. Implementation overview, in the order it gets built

**Step 1 - the declaration type and the layout generator.** `pass_io.cppm` exports the types above plus
`make_set_layout`. The generator is a loop: `VkDescriptorSetLayoutBinding{ binding = b.binding, descriptorType =
to_vk(b.kind), descriptorCount = 1, stageFlags = b.stages }`, then `vkCreateDescriptorSetLayout`. Converted
first: the passes whose layouts are built in `pipelines.cppm` with a hand-written loop - `build_resolve_pipeline`
(5 bindings) and `build_gbuffer_debug` (16). Acceptance: the generated layout is indistinguishable, which the
byte-exact capture gate proves; plus a startup log of the descriptor counts per set, which step 2 consumes.

**Step 2 - the write generator and the pool counts.** `set_pool_requirements` and `write_set`. The writes become
mechanical: for each binding, an image or buffer info whose layout is `GENERAL` - the only layout any image in
this renderer is in, since `VK_KHR_unified_image_layouts` is required - and a sampler chosen by `sampler_hint`. This is the step that removes the
`b == 6u || b == 8u || b >= 13u` predicate and its sampler ternary - the two places where adding one binding
this session required hand-editing three parallel decisions. Acceptance: same gate; and `image_set_family`'s
pool sizing is fed the DERIVED counts rather than the fingerprint count.

**Step 3 - the shader check.** `validate(pass_io, spirv_reflection)` inside each `build_*`, before pipeline
creation: every declared (set, binding, kind) must exist in the SPIR-V with a compatible type, and every
statically-used binding in the SPIR-V must be declared. This is where the layer starts paying for itself at
runtime rather than at review time: the failure becomes a startup message naming the pass and the binding
instead of a validation-layer line at submit, or a silently wrong image. The validator's own acceptance test is
that a DELIBERATELY wrong declaration fails - that test is part of the step, not an afterthought.

**Step 4 (a later stage, once 1-3 are green) - barrier and order derivation.** `binding_access` plus
`resource_scope` are what make it possible, and the LAYOUT half of this step is now settled rather than derived:
with `VK_KHR_unified_image_layouts` every image is in `GENERAL`, so both sides of every barrier are the same and
what is left to derive is the stage/access pair plus the FIRST use of each resource in a generation, which is
the one place a layout other than GENERAL is still named - the `UNDEFINED ->` form that discards the old
contents. (Before that extension this step had to read an explicit `image_layout` per binding, because the
layout was NOT derivable from the kind: the probe cache - removed since, with the traced-GI chain - kept all
nine of its own bindings in GENERAL because its ping-pong sides stayed there for the whole update. That is
history now, and it is also why the declaration needs no layout field.) The cases that are NOT mechanical must
be expressible as explicit overrides, because they are deliberate and documented: the tracer skips its hand-off
barrier when the glossy lobe will write the same image, and the lobe owes it back; and the resolve's first-use
transition exists because the multi-bounce feedback samples last frame's resolve before this frame writes it.

## 5. Where the declarations live

Beside the pass they describe, not in a central table: `pass_io` is a description OF a pass. While a pass still
lives in `runtime.cpp`, its declaration lives in a small unit of its own next to it (for the first conversion,
`vulkan/pass_io/probe_io.cppm` exporting `gi_probe_io`, the probe cache's 9 bindings and its push block); when
that pass is later extracted into `vulkan.gi_probe` (the first extraction in `docs/runtime_split.md`), the
declaration moves with it and the runtime only passes the `resource_views` in. The resource list itself
(`resource_id` + `resource_info`) belongs to `deren.vulkan.core`, because `core` is the only place the complete image
list exists - and that is the same reason its create/destroy loops can later be driven from it.

## 6. Migration order and the cases that shape the design

    1. probe cache          9 bindings, own family, own push block, no shader change      (the first conversion)
    2. TAA                  4 bindings, own family, per-image history
    3. post                 9 bindings, 5 sets per image, ONE push struct shared by 3 pipelines
    4. the two temporal resolves  SAME layout, TWO resource bundles
    5. the G-buffer set     last: 16 bindings, shared by six passes and seven shaders

Case 4 is the one that proves the model needs a notion this document must state: **the layout is shared while
the resource bundle is not**. `ssgi_spec_temporal_family` was created with `ssgi_temporal_set_layout` and its
own images; the declaration therefore describes the LAYOUT (one `pass_io`), while `resource_views` selects the
bundle per dispatch. Case 3 proves the same for push constants: one `push_block`, three pipelines.

Case 5 is deliberately last and belongs to a different project: the G-buffer set is shared across six passes and
its bindings appear in six shaders, so converting it is a shader change - a behaviour-visible change with its
own gate and its own measurement, not a step of this layer.

## 7. Acceptance, and what would make it fail

Every step: the capture gate 14 scenarios x 2 with 0 changed, Release/Debug/ASan+UBSan clean, ctest 13/13,
doxygen exit 0 with an empty warning stream, every run validation clean. The layer's own success metric, which
is testable and whose baseline is measured: **adding a pass becomes declare-I/O plus implement-record** - no
separate layout loop, no separate write loop, no separate pool count, no parallel storage/sampler ternaries.
Today that list has thirteen entries.

Failure modes to refuse in advance:

* **the declaration drifts from the shader** - the reason step 3 exists, and the reason the validator itself
  needs a test that it FAILS on a wrong declaration;
* **the layer becomes a mini-RHI** - it must never allocate an image, own a queue, or hide a Vulkan type it is
  not generating;
* **`resource_id` becomes a god-enum** - it stays owned by `core` and grows only when a family is added there;
  its order is never a contract (that is the difference between it and `gpu_mark_id`);
* **push-constant size**: declared plus `static_assert`-pinned (as `light_ubo` and `instance_record` already
  are), never computed from the declaration against a shader block whose layout this layer cannot see;
* **over-reach into ordering**: steps 1-3 must not derive a single barrier. The byte-exact gate is what proves
  a step changed nothing, and a step that reorders synchronization can pass it while changing behaviour on
  another driver - so barriers are a separate stage with their own justification, not a side effect.

## 8. What a pass needs from Vulkan, and the three decisions that settled the interface

AT CREATE TIME, once per device generation: the device; the shaders' SPIR-V; for each declared set, a
`VkDescriptorSetLayout` (the pass's own generated from its declaration, the shared ones borrowed from their
owners); a `VkPipelineLayout` from those plus the declared push range; the pipelines themselves; for a graphics
pass the colour/depth formats and blend state; a descriptor family (the pool lives in `deren.vulkan.bindings`, not in
a pass); and one of the renderer's six samplers per sampled binding.

EVERY FRAME: the command buffer, the pass's own `VkDescriptorSet`, the shared sets it declared usage of, one
`VkImageView`/`VkBuffer` per own binding, the resolved pipelines, and the extent this pass works at.

AND DELIBERATELY NOT: instance, physical device, surface, swapchain, queue, fence, semaphore, command pool,
`VkRenderPass`/`VkFramebuffer` (this renderer uses dynamic rendering), and `VmaAllocator` - a pass allocates
nothing, because `deren.vulkan.core` is the single allocator of images. A pass that wanted a `VmaAllocator` would be
taking over an image family, which is a resource-layer change and must be argued separately.

THREE DECISIONS, taken while the interface was being written and recorded because each one closes a gap that
was open in the first draft of this document:

* **shared sets are simply given.** `resolved_io` carries them as they are (one slot per owner: scene, G-buffer,
  post) and a pass that declared usage of one may bind it. No ownership mechanism, no per-set abstraction: the
  runtime already binds the scene set before a draw, and the G-buffer set is shared by six passes by
  construction.
* **the command buffer is handed out per frame, at recording time, and never stored.** That is what lets a pass
  hold no device state between frames - and it closes the first draft's hard gap, in which a pass had nothing to
  record into at all.
* **pipelines are referenced by NAME for now**, and this costs nothing new: `deren.vulkan.runtime` already keys its
  pipelines by name (`make_pipeline` / `set_default_pipeline` / `get_pipeline`), so a pass names what it records
  with and the host resolves those names into `resolved_io::pipelines`, in order. The 840 lines of
  `deren.vulkan.pipelines` stay where they are, with the pipeline layouts still built there.
* **handles stay with their owner, and the SHARED ones get a nested module.** The rule is the schema's own
  scopes applied to ownership: a pass's private family belongs to that pass, anything the frame loop alone
  touches stays in the frame loop, and a handle more than one consumer needs and none owns goes to
  `deren.vulkan.render_resource.shared` - nested, because this layer must stay pure CPU (section 1) while
  `VkSampler` is not. The six samplers are its first tenant: a declaration names a `sampler_hint` and never a
  `VkSampler`, so a pass cannot pick the wrong filter, and `bindings::write_set` resolves the hint against a
  `sampler_set` the runtime fills once. Shared IMAGE and BUFFER handles are deliberately absent until a
  consumer writes a shared set through a declaration, because that consumer is what says which keys the table
  needs.
* **EXCLUSIVE means exclusive, the pipeline included.** The probe cache's pipeline and pipeline layout stayed
  in the renderer through the first pass of this work, on the argument that a pass only names what it records
  with. That was the rule applied to the convenient half of its range: a handle ONLY that pass names and ONLY
  that pass needs is that pass's to build and to destroy, and leaving it behind meant the pass owned its
  family but not the pipeline that family's sets are bound through. So a pass now builds its own pipeline from
  its own declaration, which needed exactly two create-time facts added: its shader's SPIR-V (the app loads
  shaders and registers them; a pass asks for its own by file name) and the layout of the shared set its
  pipeline layout must be built against (asked by SET INDEX, the same vocabulary the declaration already uses).
  The runner still binds the pipeline before the pass records - ownership and binding are different questions -
  and `resolved_io::pipelines`/`pipeline_layout` stay the channel it does it through, filled from the pass's own
  handles when the pass owns them.
* **CREATE AND RECORD ARE TWO STRUCTS, because they have two owners.** Once a pass builds its own objects, what
  it needs at create time (a device, the six samplers, a shared-set-layout lookup, its shader bytes) and what a
  frame loop needs to run one (the frame, the feature registry, the resolver, the behaviour step, the marks)
  stopped being the same list. Merging them had produced one struct that grew with every pass - the context
  object this layer exists to avoid - and it conflated "who may create a pass" with "who may run a frame",
  which is false: an editor, a tool or a test can fill a `pass_context` and own a pass, and a runtime is only
  one such owner. So `frame_pass::create(pass_context const&)` and `create_stage(stage, context)` take the
  context ALONE, `pass_host` stays the runner's, and the framework's create path still needs no device of its
  own (the ctest fills a fake context, which the design's `core_filter` could not have allowed: that class
  holds a `core*` and cannot be faked).

STILL OPEN, stated rather than implied: parallel recording (`main_segments` + the task pool) is not expressible
in `stage` yet.

THE PUSH CONSTANTS ARE SETTLED, and the first consumer is what settled them. They were the one input that is
neither a resource nor a behaviour: the values in them (scene bounds, an instance table's device address, the
global light direction) are the RENDERER's, while the block's shape and its per-dispatch lanes belong to the
PASS that declared the shader. So the split follows the values: the HOST composes the block and `resolved_io`
carries it as raw bytes (`std::span<std::byte const> push`, whose size the declaration's `push` block
contracts), and the PASS reads it as the struct it declared, changes the lanes it owns and pushes it through
`resolved_io::pipeline_layout` - a layout, one per pass, which is what this renderer's passes have (post's five
pipelines and TAA's two are each built from a single layout). The alternative - the host pushing on the pass's
behalf - would have meant the host knowing every pass's push block: the same fact in two places, which is the
one thing this layer exists to prevent.

## 9. Status

    description module (deren.vulkan.render_resource)   DONE: the schema, the usage records, the validators,
                                                        the pool counts, and one real declaration (the probe
                                                        cache); pure CPU, so ctest covers its invariants
    framework module (deren.vulkan.pass)                DONE: behaviour vocabulary, resolved_io, the base class,
                                                        the two interfaces (above), stage, and the three
                                                        runner functions. What only a real pass could
                                                        discover, added in two rounds: the generation's
                                                        image count in frame_identity, the pipeline layout
                                                        and the host-composed push bytes in resolved_io, a
                                                        non-const record (a pass that owns a descriptor
                                                        family has to ensure it), an image handle next to
                                                        the view, and finally the CREATE/RECORD split - so
                                                        what a pass builds itself from is fillable by any
                                                        owner and not only by a frame loop
    the graphics half: render targets +            DONE as a framework step: a pass declares the images it renders
    the fullscreen behaviour                            into (`render_target`, apart from the bindings because an
                                                        attachment is not a descriptor), `resolved_io::targets`
                                                        carries their views and images, and the runner binds a
                                                        graphics pipeline and sets the viewport/scissor for a pass
                                                        that asked (which is what replaces the hand-kept pipeline
                                                        list in `update_pass_geometry`). The rendering INSTANCE
                                                        stays the pass's: it knows the load op. No consumer yet, so
                                                        the frame is unchanged by construction - the first one is
                                                        TAA, which is also why nothing declares a depth target yet.
                                                        `behaviour_kind::graphics`/`instanced` (a draw into a
                                                        stage-opened instance) still say out loud that they are not
                                                        driven
    shared handles (deren.vulkan.render_resource.shared) DONE for the samplers, which is the first tenant: the probe
                                                        cache's writes now CHOOSE one through its declaration
                                                        (`sampler_hint`) instead of naming a `VkSampler`, and
                                                        `bindings` no longer defines a second `sampler_set` of
                                                        its own; the shared image/buffer handles follow a
                                                        consumer, not this table
    the second consumer: deren.vulkan.pass.taa           DONE, and it is the temporal resolve - the first GRAPHICS
                                                        pass, and the one that proves the other half of the
                                                        shape: a declared render target, its own rendering
                                                        instance (the load op is the pass's), the runner's
                                                        graphics bind point and viewport resync, and a
                                                        per-image family whose fingerprint is the GENERATION
                                                        rather than the current image. `runtime` lost
                                                        `record_taa_pass`, `ensure_taa_descriptors`,
                                                        `make_taa_pipeline`, the family and both history
                                                        flags; `image_view_proj` stayed because the camera UBO
                                                        reads it, and the two lines that belong to the
                                                        G-buffer pass (the depth transition, the motion-vector
                                                        flag) stayed as the barrier stage's entry point.
                                                        Byte-identical: `deferred_taa_fxaa` back to
                                                        6999D01E5FBAB508, 12 x 2 with 0 changed and 0 flaky.
                                                        THREE THINGS IT FOUND: the sampler was created inside
                                                        the factory being moved (a null sampler in a descriptor
                                                        write is a crash, and validation named the VUID); the
                                                        per-image family cannot fingerprint the current image;
                                                        and a pass's own sequence may contain another pass's
                                                        bookkeeping
    step 1: the layout generator                  DONE for the probe cache, and PROVEN BY THE GATE rather
                                                        than by a comparison test: `bindings::make_set_layout`
                                                        generates the pass's layout from the declaration, and
                                                        `sponza_gi` - the scenario that runs with the probe
                                                        cache on - is byte-identical across it
    step 2: the write generator + pool counts     DONE for this pass: `bindings::write_set` writes its nine
                                                        own bindings and the PASS, not `runtime`, is now the
                                                        caller; the family's per-set budget is derived
                                                        (`descriptor_counts_for(...).total()`) instead of
                                                        restated as a literal, which is the drift class the
                                                        project has already been bitten by (a pool sized for
                                                        four descriptors per set while the layout asked for
                                                        five). The post chain keeps its literals until it has
                                                        a declaration of its own
    step 3: the SPIR-V check                      NOT STARTED
    step 4: barrier and order derivation          NOT STARTED, and deliberately last (see section 7)
    the first consumer: vulkan.pass.gi_probe      DONE, and it is the probe cache: the pass owns its set
                                                        layout, its two-set ping-pong family, its pipeline
                                                        layout, its pipeline, its barriers, its clear and
                                                        its push, while the renderer keeps the switch and
                                                        the values the push block is composed from.
                                                        `runtime` lost `record_gi_probe_pass`,
                                                        `ensure_gi_probe_descriptors` and
                                                        `make_gi_probe_pipeline` entirely, and the frame is
                                                        byte-identical across both halves of the move -
                                                        `sponza_gi` back to 58EC848DFABE654A, 12 x 2 with 0
                                                        changed and 0 flaky. The pass's create step logs its
                                                        own outcome, and `runtime::register_shader` +
                                                        `runtime::create_passes` are the app's two calls:
                                                        the app owns the FILE, the pass owns the PIPELINE.
                                                        WHAT IT DISCOVERED is in `deren.vulkan.pass` itself (the
                                                        create-time host, the generation's image count, the
                                                        layout and the host-composed push, non-const record,
                                                        and an image handle next to the view) and in section
                                                        8 above
                                                        NOTE: that pass and the probe cache have since been
                                                        removed with the traced-GI chain (`1162d88`)

THE CHOICE THAT MADE STEP 1 PROVABLE is worth keeping: the generator emits bindings IN DECLARATION ORDER, and
`validate` requires a pass's own bindings to be contiguous from zero - so the layout the shader sees and the
declaration cannot drift, and the equality of generated-with-hand-written became a property the existing capture
gate could decide instead of a claim needing a new test. What a comparison test could not have done is prove the
layout is *used* the same way; `sponza_gi` can, because the probe pass runs in it. Step 2 got the same treatment:
the generated writes reproduce the hand-written ones exactly (that comparison is what the `image_layout` field
was introduced for, before unified layouts removed the field altogether) - again decided by the gate, and again on
the pass that actually runs.

A BOUNDARY OF THE INSTRUMENT, measured while moving the pass and recorded because a claim of "verified" is only
worth what the instrument can decide: the capture gate is a RELEASE instrument. Pointed at this project's Debug
or ASan+UBSan build, two runs of the SAME binary differ - measured on `sponza_gi` and on `deferred_ssao_off`,
which runs none of the GI chain at all - so those builds are not deterministic here and cannot be gated against
the Release references. That is why every acceptance number in this sequence comes from the Release build, and
why Debug/ASan+UBSan are held to "clean build, ctest 13/13, and a run that reports no validation or sanitizer
finding" instead.

