# Descriptor heap: the whole-frame conversion

**STATUS: REACHED.** The target below is the state of the renderer now. The conversion landed as the
heap-native frame, and the deletions it called for are done: `vkCreateDescriptorSetLayout`,
`vkAllocateDescriptorSets`, `vkCreateDescriptorPool`, `vkCreatePipelineLayout`,
`vkCmdBindDescriptorSets`, `vkCmdPushConstants` and every `VkDescriptorSetAndBindingMapping*` type
appear ZERO times in the tree. The sections below that are written in the future tense are kept as the
plan they were - they are what the work followed - and the deletions section is annotated with what
each item became.

The target is a renderer with **no descriptor sets, no set layouts, no pools and no
`VkDescriptorSetAndBindingMappingEXT` shim**: every descriptor lives in the resource/sampler heap,
every shader names the heap natively, and every pipeline is created heap-native.

## Why it is one commit and not one pass at a time (measured)

Binding the heap is command-buffer state that takes over **every** stage recorded after it. With the bind
recorded at the start of the frame and every mapping switched off - so the heap held nothing any shader had
asked for - all nine gate scenarios came back as the **same** frame:

```
hash DC5F6D66428C26D8   mean 0.00   (unlit reference: 88.1)   validation: SILENT
```

A stage whose descriptors came from a set reads the heap instead once one is bound, so a half-migrated frame
renders *nothing*. The unit of this migration is the frame.

Two rules cost one gate run each and are not optional:

1. Without `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` a mapping is **silently ignored** ("the
   VkDescriptorSetLayout will be read instead"). The bit is `0x1000000000`, i.e. past the 32-bit `flags` field,
   so it arrives through `VkPipelineCreateFlags2CreateInfo`.
2. That bit **requires `VkPipelineLayout == VK_NULL_HANDLE`** - the layout is exactly what the heap replaces.
   (This holds for the mapping path; the native path below drops layouts for the same reason.)

## The shader model: heap-native GLSL, proven with this toolchain

`slangc` (the compiler this tree builds every stage with - see `CMakeLists.txt`'s `find_program(VR_SLANGC_EXECUTABLE NAMES slangc ...)`) supports `GL_EXT_descriptor_heap` through `-capability spvDescriptorHeapEXT`, and its output is verified compiling against `-target spirv -profile spirv_1_6`:

| declaration | form |
| --- | --- |
| images | `layout(descriptor_heap) uniform texture2D heap_textures[];` |
| samplers | `layout(descriptor_heap) uniform sampler heap_samplers[];` |
| uniform blocks | `layout(descriptor_heap) uniform Camera { mat4 view; } heap_cameras[];` |
| storage blocks | `layout(descriptor_heap) buffer Cluster { uint counts[]; } heap_clusters[];` |
| storage images | `layout(descriptor_heap, rgba16f) uniform image2D heap_images[];` |
| acceleration structures | `layout(descriptor_heap) uniform accelerationStructureEXT heap_tlas[];` |

Rules that follow from the extension and are already paid for:

- A **combined image sampler cannot be declared**. It is constructed at the use site:
  `texture(sampler2D(heap_textures[i], heap_samplers[s]), uv)`. Every fetch therefore names an image index
  *and* a sampler index - the sampler is no longer implicit in the binding, so the sampler indices become a
  shader-visible convention (the five shared samplers at fixed heap indices, named by `const uint` in a
  shared GLSL header).
- A **variable index** requires `#extension GL_EXT_nonuniform_qualifier : enable` and `nonuniformEXT(i)`.
- Storage images with `descriptor_heap` need a **format layout qualifier** (`rgba16f`, `r16f`, ...), which the
  existing shaders already carry.
- `descriptor_stride` (a power of two) may override the API stride for an array.

### The index space: an index IS a byte offset divided by that kind's descriptor stride

Every `descriptor_heap` array in a shader **aliases the same heap from offset 0**, so
`heap_lights[i]` reads `heapBase + i * stride(lights)`. Two declarations of different names but the same kind
and stride are literally the same memory. The workable convention is therefore:

- the host places a descriptor at an offset that is a multiple of its kind's descriptor stride
  (device properties: buffer 16 B / align 8, image 32 B / align 32, sampler 32 B / align 32);
- a shader indexes with `byteOffset / stride`, i.e. indices are kind-relative **slot numbers**;
- the values that vary per frame or per swapchain image (`frame slot`, `image index`, material id, texture id,
  sampler id, the post chain's per-pass image base) reach the shader through **push constants**.

Push *data* (`vkCmdPushDataEXT`) is how they arrive: the block is the same one the shader's `layout(push_constant)` declares, and the mapping sources this document's first half used are gone.

### The storage class has to match the descriptor's type, and a mismatch is SILENT

A heap descriptor carries its TYPE, and the storage class of the pointer a shader fetches it through has to
agree with it. When it does not, the read does not fail loudly - **there is no validation finding at all**,
the read simply comes back zeroed, and only the picture shows it. All four combinations were reproduced on
this machine (2026-09-23, RTX 4060 Laptop, driver 616.92):

| heap descriptor written as | storage class the shader reads it through | result |
| --- | --- | --- |
| `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | `StorageBuffer` | works |
| `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | `Uniform` | works |
| `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` | `StorageBuffer` | reads as ZEROS |
| `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` | `Uniform` | reads as ZEROS |

THREE THINGS HAVE TO AGREE, and this is why it is written down here rather than discovered per stage: the
descriptor's type, the USAGE BIT on the buffer (`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` for a storage
descriptor - without it the validation layer rejects the heap write itself, with the render still looking
right), and the shader's declaration.

THE CAMERA AND THE LIGHT ARE STORAGE DESCRIPTORS FOR THIS REASON (see `runtime.constructor.cppm`'s two
heap writes and `shading.glsl`'s camera/light blocks): Slang's
`DescriptorHandle<ConstantBuffer<T>>` always fetches through a `StorageBuffer`-class pointer - a Slang
`ConstantBuffer` handle never emits `Uniform` - so the GLSL side had to declare those blocks `buffer` as
well. The layout did not move, and that was CHECKED RATHER THAN ASSUMED: `CameraUBO` keeps its offsets
(0, 64, 128, 144, 208) and the light block keeps every one of its members where std140 had them - including
the 8576-byte size that comes from `punctual_lights[128]` at 352, `cluster_grid` at 8544 and
`cluster_depth` at 8560, which is the member list a std140/std430 mismatch would have moved. The full
finding, including what it cost to find and the two ruled-out suspects, is docs/slang_migration.md
section 9.

## Shader inventory (the resources each stage reads, and the binding each one came from)

There is no `layout(set = ` left to grep: the `.frag` / `.vert` / `.comp` sources this table was built from
are retired to `shaders/glsl.old/` (see `CMakeLists.txt`'s `THE GLSL STAGE LIST USED TO LIVE HERE`), and the
stages this build compiles are Slang leaves - `docs/slang_migration.md` and `docs/shaders.md` record them.
So the table below is a RECORD of what each retired file declared (a reader can still check each row against
the archived source), not a description of any file's current contents: the host keeps the same resources under
the same classic binding numbers in `vulkan/core/core.declarations.cppm`, and every shader now reads them as
`heap_slots_textures`-style array entries instead of declaring a binding at all.

| file (retired) | the bindings it declared | where they live now |
| --- | --- | --- |
| `shading.glsl` (shared) | 0 CameraUBO, 2 env cube, 3 irradiance cube, 4 BRDF LUT, 7 LightUBO, 8 shadow array, 11/12 cluster | - |
| `surface.glsl` (shared) | 1 `textures[]`, 5 materials | - |
| `pbr.vert` | 0 camera, 6 instance transforms, 13 previous transforms, 9 skin matrices, 10 morph | - |
| `shadow.vert` | 6, 9, 10, 7 | - |
| `shadow.frag` | 1, 5 | - |
| `unlit.frag` | 1, 5 | - |
| `light_cluster.comp` | 0, 7, 11 (write), 12 (write) | - |
| `compute_skin.comp` | 9 | - |
| `mask_bake.comp` | 1, 5 | - |
| `deferred.frag` | 14 rt shadow visibility | 0..3 G-buffer, 7 ml lighting |
| `gbuffer_debug.frag` | - | 0..4 (its own set 0) |
| `megalights_trace.comp` | 16 TLAS | 0..3 G-buffer, 6 `image2D` out |
| `megalights_temporal.comp` | 0..4 (its own set 0) | - |
| `post.frag` | 0 source, 1..4 bloom, 7 depth, 8 normal | - |
| `taa.frag` | 0..3 | - |
| `fxaa.frag` | 5 display colour | - |
| `rt_shadow.rgen` | 0, 7, 16 TLAS, 15 `image2D` out | 1, 3 G-buffer |
| `rt_shadow.rahit` | 1, 5, 17 mask instances | - |

## Host-side work

1. **Heap layout, reserved once** in `core`: per-kind index spaces (frame slot x2; swapchain image xN;
   material table; texture array; the five samplers; TLAS array; storage images; the post chain's per-pass
   images), each offset a multiple of that kind's descriptor stride, recorded in `core` beside the existing
   `heap_*` members.
2. **Every descriptor write gains a heap twin**: each `vkUpdateDescriptorSets` site (scene set, G-buffer
   family, post family, IBL, shadow, cluster, AS, storage images) becomes `write_image`/`write_buffer` into the
   reserved offsets, per frame slot or per swapchain image.
3. **Push constants carry the indices**: each shader's push-constant block gains `frame_slot` and
   `image_index` (and a per-pass image base where needed), and each host push site writes them.
4. **Pipelines**: builders stop taking/creating set layouts, create with `VkPipelineCreateFlags2CreateInfo` +
   `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` and a null layout - graphics, compute and ray-tracing alike.
5. **Delete**: `vkCmdBindDescriptorSets` everywhere, set layouts, pools, families, and the whole mapping shim
   (`scene_heap_layout`, `scene_heap_stage_mapping`, `map_from_heap`).
6. **Frame**: bind the heaps once (the existing `record_bind`), with no push.

## Acceptance

`BUILD=0`, 13/13 tests, gate 14/14 `changed: 0` (frame byte-identical, `mean|d| ~0.029`), validation silent, and a
deliberate **wrong-index** push as the negative proof (the picture must break, which is what proves the shaders
read the heap).

## The acceleration structure, and the one thing it still needs

The heap's descriptor payload has no acceleration-structure member - the whole union is

```c
typedef union VkResourceDescriptorDataEXT {
    const VkImageDescriptorInfoEXT*        pImage;
    const VkTexelBufferDescriptorInfoEXT*  pTexelBuffer;
    const VkDeviceAddressRangeEXT*         pAddressRange;
    const VkTensorViewCreateInfoARM*       pTensorARM;
} VkResourceDescriptorDataEXT;
```

so an AS in a heap is an **address range** with `type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`, i.e. the
structure's `vkGetAccelerationStructureDeviceAddressKHR` address - which is what "a heap descriptor for one is its
device address" meant. `descriptor_heap::write_buffer(offset, address, size, type)` already builds exactly that.

WHAT IS STILL OPEN IS THE SIZE, and it is open on purpose rather than guessed: the same call's size ends up in
`VkDeviceAddressRangeEXT::size`, and this renderer already learned (VUID-VkDeviceAddressRangeKHR-address-11365,
on the material table) that a heap range must carry a REAL size rather than `VK_WHOLE_SIZE`. An acceleration
structure is not a buffer, so the candidates are its creation size (`VkAccelerationStructureCreateInfoKHR::size`)
or the size the build sizes query reports - whichever the structures module already knows. The write site is
`runtime::build_rt_structures`'s per-slot block (runtime.cpp, beside `write_rt_structure_binding`), inside the
same `rt_binding_written[frame_slot] != tlas` guard, at grid slot `heap_slots::tlas + frame_slot` - the two slots
the TLAS has BECAUSE it is rebuilt every frame.

THE SIZE IS COMPUTED IN THE RAY-TRACING MODULE, NOT THE ACCELERATION-STRUCTURE ONE - and that correction cost a
build, so it is written down: the TLAS this renderer uses is built by **`ray_tracing::structure_set`**
(`vulkan/ray_tracing/ray_tracing.cppm`, `build()`/`update()` in `ray_tracing.cpp`), whose public surface today is
`handle(frame_slot)`, `instance_table(frame_slot)`, `casters()`, `attempted()`, `ready()`. The
`acceleration_structure` module's `top_level_structure` also creates a top level structure with
`create.size = sizes.accelerationStructureSize`, and that is the module the first version of this note pointed at;
pointing there made the write compile against a class the runtime does not own (the error names the type:
`no member named 'structure_size' in 'deren::vulkan::ray_tracing::structure_set'`).

So the next step is: publish the size from `structure_set` - it is known where the structure is created, next to
`handle()` - and then the write in `runtime::build_rt_structures` is three lines: the address query, that size,
and `write_buffer` with `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`, at grid slot `heap_slots::tlas +
frame_slot`, inside the existing `rt_binding_written[frame_slot] != tlas` guard.

## Where the work stands (measured, not claimed)

POPULATED AND VERIFIED IN THE LOG. The grid is reserved at 1 MiB and every write below is proven by its own log
line, because a heap write has no picture to show for itself until the shaders read the heap:

| what | grid slot(s) | evidence |
| --- | --- | --- |
| 6 shared samplers | sampler grid at 65536 | `6 shared samplers written to the sampler grid at 65536` |
| material table (binding 5) | 512 | `material table written (... offset 1081344)` = (16384 + 512) * 64 |
| texture array (binding 1) | 0 + index | `N texture descriptors written` |
| camera / clusters (0, 11, 12) | 514, 518, 520 (+frame slot) | `2 per-frame descriptor(s) written for grid slots 16898..16899` (and 16902, 16904) |
| motion / skin / morph (13, 9, 10) | 524, 526, 528 (+frame slot) | the same lines for 16908, 16910, 16912 |
| instance table (6) | 522 | silent success, no `did not reach` line |
| env / irradiance / BRDF LUT (2, 3, 4) | 532, 533, 534 | silent success, no `did not reach` line |
| top level structure (16) | 703 + frame slot | silent success, validation SILENT with a real range size |
| light UBO (7) | 516 + frame slot | silent success |

WHAT THE HEAP WRITES ALREADY TAUGHT (each cost a build or a validation cycle):

- A heap-bound buffer needs `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` (VUID-VkBufferDeviceAddressInfo-buffer-02601:
  the material table, the light UBO, then motion/skin/morph and the instance table in one round).
- A heap range needs a REAL size, never `VK_WHOLE_SIZE` (VUID-VkDeviceAddressRangeKHR-address-11365).
- `vkGetAccelerationStructureDeviceAddressKHR` is not exported by the loader: resolve it per device.
- Success must be logged, or "wrote it" and "the vector was empty" look the same.

STILL TO POPULATE: the shadow map (binding 8, base slot 535), the per-swapchain G-buffer and post images (base
slots 551..702), the storage images (binding 15 and the megalights outputs), the mask/instance table at binding 17
(its write site is `runtime::write_rt_structure_binding`, which already handles 16 and 17 together), and any
image whose binding is repointed later (the furnace mode).

STILL TO DO, and it is the larger half: (2) heap-native shaders in place of `layout(set, binding)` - untyped
declarations indexed with `descriptor_stride = 64`, combined image samplers CONSTRUCTED at the use site, and the
`frame_slot` / `image index` carried in PUSH CONSTANTS (the mapping-era reading was that push data only fed
mapping sources, which the proposal text corrects); (3) every
pipeline created with `VkPipelineCreateFlags2CreateInfo` + `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` and a
NULL layout; (4) bind the heaps once per frame and delete descriptor sets, set layouts, pools and the mapping
shim (`scene_heap_layout`, `scene_heap_stage_mapping`, `set_scene_heap_layout`, `build_cluster`'s
`map_from_heap` flag, `push_heap_frame_slot`). Then the flip, and the negative proof: push a WRONG index and show
the picture break.

Items (2), (3) and (4) have since been done as written - every stage is heap-native, every pipeline carries the
flag with a null layout, the frame binds the heaps once and the shim is deleted (docs/descriptor_heap_handover.md).

The migration is still one frame-wide switch, for the reason at the top of this file: a frame whose stages do not
all read the heap renders nothing at all.

## The native path's shape, from the extension proposal (no experiment needed)

The two questions above are ANSWERED by the proposal text
([VK_EXT_descriptor_heap](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_heap.html)),
so they cost a search rather than a build:

- **The flag forces a NULL layout, in so many words**: "This has the same effect as the pipeline flag - the
  pipeline layout must be `NULL` and shader resources will be sourced from a descriptor heap." So every
  converted pipeline is created with `VkPipelineCreateFlags2CreateInfo` +
  `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` and `layout = VK_NULL_HANDLE`, and the set layouts it used to
  name go away with it.
- **PUSH CONSTANTS SURVIVE, through `vkCmdPushDataEXT`**: "Push constants in this data can be accessed in the
  same way as before via the `PushConstant` storage class, it is now simply unnecessary to construct a pipeline
  layout to do that." That is the mechanism this design needs: the shaders keep their `layout(push_constant)`
  blocks (and gain the frame slot / image index in them), and the HOST changes only HOW it sends them - one
  `vkCmdPushDataEXT` per push instead of `vkCmdPushConstants`. The window is `maxPushDataSize` (256 B on this
  device), and the two commands invalidate each other, so a converted frame uses push data and nothing else.
- **Mappings are not needed for the native model at all**: "Applications can fully ignore the mappings; bindless
  interfaces are provided for all resource types." They remain only as the interface for existing set/binding
  declarations, and "VkShaderDescriptorSetAndBindingMappingInfoEXT is ignored if the shader or pipeline is
  created with a pipeline layout or descriptor layouts" - which is why the shim built earlier is inert here.
- The one exception worth knowing: mappings are still required for **embedded samplers and input attachments**.
  The native form sidesteps the first by taking samplers from the SAMPLER heap explicitly (`sampler2D(tex, samp)`
  at the use site), which is why the grid gives the six shared samplers their own slots.

So the conversion is, in order: (1) every push site sends its block through `vkCmdPushDataEXT`; (2) every
pipeline drops its set layouts and takes the flag with a null layout; (3) every shader converts its
`layout(set, binding)` declarations to `descriptor_heap` ones and its fetches to constructed samplers; (4) the
frame binds the heaps once and nothing binds a set; (5) delete the layouts, families, pools and the mapping
shim. Nothing in that list is a question any more, only work - and it is one frame-wide commit, because a frame
whose stages do not all read the heap renders nothing at all.

### ... and there is NO cheaper path, measured

The obvious hope was that the flag is only about MAPPINGS, so a heap-native shader might keep its pipeline
layout and its ordinary push constants, leaving every builder and every push site untouched. The probe answered
it in one run, by being built that way (layout with a push range, no flag): `vkCreateComputePipelines` refused
the pipeline, and validation said exactly why -

```
... is trying to use descriptor heaps ([Resource Heap, variable "resource_heap"]) but is also trying to use a
VkPipelineLayout, either set the layout to NULL or remove the heaps from the shader
   (VUID-VkComputePipelineCreateInfo-layout-07988)
```

- so a `descriptor_heap` declaration and a non-null layout are mutually exclusive. The full list above is the
only route, and the probe (flag + null layout + `vkCmdPushDataEXT`, reading the default material's white base
colour as `0xffff`) is the proof that the route works.

### Ray tracing is covered, and it says so in three VUIDs

The one pipeline kind whose flag EXPRESSIBILITY was in doubt is the ray-tracing one: its `flags` field is 32-bit
and the heap bit lives past bit 31, so the flag can only arrive through `VkPipelineCreateFlags2CreateInfo` in its
`pNext`. It does - measured by chaining exactly that into the ray-traced shadow pipeline and reading what
validation said (three VUIDs, all of which are the *rules*, not a refusal):

- `VUID-VkRayTracingPipelineCreateInfoKHR-flags-11311`: with the flag, `layout` must be `VK_NULL_HANDLE` - the
  same rule compute pipelines have, so a ray-tracing pipeline is converted the same way.
- `VUID-VkRayTracingPipelineCreateInfoKHR-flags-11312`: with the flag, every shader variable carrying a
  `DescriptorSet`/`Binding` decoration must have a mapping in `VkShaderDescriptorSetAndBindingMappingInfoEXT` -
  i.e. an RT shader is either native or mapped, with no third option.
- `VUID-vkCmdTraceRaysKHR-None-11376`: with the flag, a push constant the shader uses must have been set by
  `vkCmdPushDataEXT` - so the push-data rule holds for ray tracing too.

The measurement edit was reverted (it left the pipeline flagged with a non-null layout, which is a validation
error by 11311); what is kept is the knowledge, because it means objective item (3) - "graphics, compute and
ray-tracing" - has no hidden exception to design around.

## The conversion's work list, counted

Everything below is a pointer, not a plan: each line is a site that has to change, and the counts are what make
the size of the remaining commit visible. Line numbers are as of the commit that added this section.

**Push sites that switched to `vkCmdPushDataEXT` (21).** The layout argument disappeared with the layout, and
the bytes are the same ones the shader's `layout(push_constant)` block already declares, so this is a one-line
change per site:

| file | lines |
| --- | --- |
| `vulkan/runtime/runtime.cpp` | 2299 (the shadow cascade index) |
| `vulkan/primitive/primitive.cpp` | 26, 78, 116 |
| `vulkan/pass/mask_bake.cpp` | 128 |
| `vulkan/pass/compute_skin.cpp` | 143 |
| `vulkan/pass/fxaa.cpp` | 175 |
| `vulkan/pass/geometry_buffer_debug.cpp` | 158 |
| `vulkan/pass/deferred.cpp` | 173 |
| `vulkan/pass/ray_traced_shadow.cpp` | 204 |
| `vulkan/pass/megalights_trace.cpp` | 152 |
| `vulkan/pass/megalights_temporal.cpp` | 217 |
| `vulkan/pass/post.cpp` | 244, 356 |
| `vulkan/pass/taa.cpp` | 237 |

**Pipeline layouts that must become null, with the heap flag replacing them (13).**
`vulkan/pipelines/pipelines.cppm` creates one per builder (lines 240, 337, 378, 417, 462, 640, 737, 810, 960,
1001, 1070); `vulkan/core/core.cpp` creates the shared `scene_pipeline_layout` (1542) - which is RETAINED but
never handed to a converted pipeline, because a set layout is still what the not-yet-converted passes name; and
the compute pipelines are created at lines 437, 482, 674, 709 (the probe, already flagged), 757, 980, plus the
ray-tracing one at ~907.

**Graphics pipelines.** `vulkan/core/pipeline/pipeline.cpp:206` is the single `vkCreateGraphicsPipelines` in the
renderer (the `make_pipeline` in `core.pipeline`, which the G-buffer, post and debug passes all go through), and
`vulkan/core/core.cpp`'s G-buffer builder is the other one - so "every graphics pipeline" is really two
functions plus whatever `make_pipeline`'s callers pass.

**Shaders (24 stages).** The inventory table above lists the bindings the retired GLSL sources declared; each
declaration becomes a `descriptor_heap` array, each fetch over a combined image sampler becomes
`sampler2D(heap_textures[slot], heap_samplers[sampler_slot])` at the use site, and each stage that reads a
per-frame or per-generation array needs the index for it - which, with no layout, arrives through push data and
therefore has to be ADDED to that stage's push block (the one piece of the conversion that changes a shader's
interface rather than its declarations).

**Deletions, once the frame is heap-only - ALL DONE.** Every `vkCmdBindDescriptorSets`, the set layouts
and their pools, `scene_sets`/`gbuffer_family`/`post_family`, `write_rt_structure_binding`'s set write,
the mapping shim (`scene_heap_layout`, `scene_heap_stage_mapping`, `set_scene_heap_layout`,
`build_cluster`'s `map_from_heap` flag, `push_heap_frame_slot`) - and `descriptor_heap::make_mapping`,
which nothing else used. They came out in three commits, each proven byte-neutral by the gate
scenarios against a frozen reference set: the mapping shim, then the classic descriptor world (41 files,
-3005 lines: the set layouts, pools, pipeline layouts, every pass's `set_layout_`/`pipeline_layout_`/
family and their per-frame writes), then the last of the mapping machinery that the first commit's
deletion had exposed. The declaration layer lost its set vocabulary with them: a `pass_binding` names a
resource and whether it is the pass's own per-image target, not a set it lives in, and `set_owner` is
`binding_owner` with two values.

**What is already proven, and therefore not in doubt.** The grid is reserved and covered (every slot the header
names is written by host code, checked by a test); the heaps are bound by `record_bind`; the native path works
with validation silent (the probe); the flag is expressible on compute and on ray-tracing pipelines and implies
a null layout and push data (probe + three VUIDs); and the frame-atomicity rule that makes this one commit is
measured at the top of this file.

## Where this stands after the last round of work

Everything below is MEASURED, and the log lines are quoted rather than paraphrased because a heap write has no
picture to show for itself until the shaders read the heap.

```
descriptor heap: slot grid at 1048576 (1024 slots x 64 B; textures 0 materials 512 tlas 703 camera 514 ...)
descriptor heap: 6 shared samplers written to the sampler grid at 65536
descriptor heap: 2 per-frame descriptor(s) written for grid slots 16898..16899     (camera, clusters, motion,
descriptor heap: 2 per-frame descriptor(s) written for grid slots 16902..16903      skin, morph and the
descriptor heap: 2 per-frame descriptor(s) written for grid slots 16904..16905      instance table all report)
descriptor heap: 2 per-frame descriptor(s) written for grid slots 16908..16909
descriptor heap: 2 per-frame descriptor(s) written for grid slots 16910..16911
descriptor heap: 2 per-frame descriptor(s) written for grid slots 16912..16913
descriptor heap: material table written (address 0x..., 16384 records, offset 1081344)
descriptor heap: the heap-native probe sampled grid slot 16384 through sampler slot 2048 ... the material
                 table's DEFAULT record read 0xffff (its white base colour is 0xffff)
descriptor heap: the heap-native GRAPHICS probe rendered grid slot 16896 ... read back rgba 255,255,255,255
descriptor heap: the heap-native GRAPHICS probe rendered grid slot 16897 ... read back rgba 0,0,0,255
```

- Item (1) - every descriptor written - is DONE and guarded: the grid is reserved at a fixed 1 MiB with a 64 B
  stride, the sampler heap has its own 64 KiB base, every array the shader header names has a host write site (a
  test fails if one appears without), and the writes above are the evidence for the categories. The IMAGE side is
  written where each image is created - the G-buffer targets and depth, the motion vectors, the scene colour and
  TAA's history, the ray-traced visibility (as both a sampled and a storage descriptor), the megalights trace,
  resolve and history, the post chain's HDR and display targets, the bloom levels, the shadow map and the three
  IBL images - and the absence of any `did not reach grid slot` line is the evidence that those writes succeeded.
- The MECHANISM of items (2), (3) and (4) is proven end to end: a heap-flagged, layout-less, push-data-fed
  pipeline reads the right value out of the right grid slot, for compute AND for graphics, and a deliberately
  wrong slot reads a different value. Ray tracing takes the flag too (three VUIDs say how).
- What REMAINED was the frame-wide conversion itself - the counted work list above (15 push sites, 13 pipeline
  layouts, 2 graphics creation functions, 17 shaders, then the deletions) - and it has since been done: the
  stages are heap-native, the pipelines carry the flag with a null layout, and the frame binds the heaps once
  (`docs/descriptor_heap_handover.md`). It was one commit because a frame whose
  stages do not all read the heap renders nothing, which is measured at the top of this file.
- Acceptance, against the criteria as written: BUILD=0, 13/13 tests, gate `changed: 0` and validation silent
  all hold at every commit; the wrong-slot negative proof holds AT THE MECHANISM LEVEL (the two probe lines
  above); what could not be claimed when this was written is the same pair of facts for a CONVERTED frame,
  because the frame was not converted yet - the reference frames still matched trivially, since every pass
  still read its descriptor set. The frame-wide conversion that closed that gap is recorded in
  `docs/descriptor_heap_handover.md`.

## The POC that died - and the method lesson it left (the questions above are now answered)

The plan was to prove the heap-native shader path on the MASK BAKE, which looks ideal: it reads exactly two
descriptors (the material table and the bindless texture array, both already on the grid), it is a job rather
than a frame pass, and it only runs under `[render] rt_mask_bake`, which no gate scenario sets - so a mistake
could not reach a reference frame. The conversion was written and COMPILED (glslc accepts it first try):

```glsl
#include "heap_slots.glsl"
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D heap_textures[];
layout(descriptor_heap) uniform sampler heap_samplers[];
layout(descriptor_heap, descriptor_stride = heap_slot_stride) readonly buffer MaskMaterials { MaskMaterial materials[]; } heap_material_tables[];
...
heap_material_tables[heap_slots_materials].materials[pc.material_index]          // ARRAY name = heap index,
texture(sampler2D(heap_textures[heap_slots_textures + material.tex_indices.x],  // block member = record index
                  heap_samplers[heap_sampler_texture]), uv)
```

It was REVERTED, because the probe cannot be isolated: `ray_tracing::structure_set` builds the structures in the
FRAME's command buffer (`runtime::build_rt_structures`, whose gpu marks sit next to the frame's), and
`runtime::structure_record_mask_bake` records the bake into that same command buffer. Binding the heap there is
exactly the measured poison that makes every set-based stage in the frame read the heap - so the bake is only
safe to convert *together with the whole frame*, which is the point this file opens with. A lesson about the
method, not the mechanism: "off the gate's path" is not the same as "isolated".

TWO QUESTIONS THE REAL CONVERSION MUST ANSWER, in this order:

1. **Does a heap-native pipeline need `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` at all?** The flag's
   validation message is about MAPPINGS being read instead of a layout; a native declaration references the heap
   builtin directly. Test it by converting one stage with the flag OFF and a normal layout.
2. **If the flag IS needed, the layout must be NULL** (measured, VUID above) - and then there are no PUSH
   CONSTANTS, because those live in the pipeline layout. The indices this design planned to carry in push
   constants would have to come from the heap instead (a per-frame-slot index block is already on the grid, and
   which slot is read is itself the chicken-and-egg: it must be derivable from something the shader already
   knows, or the slot's DESCRIPTOR must be rewritten per frame, which frames in flight forbid).

That is the decision to make first, and it is a measurement away: one stage, either answer, both cheap.

## What populating the IMAGE half needs (found by trying)

A heap image descriptor carries a **`VkImageViewCreateInfo`, not a view** - the driver creates the view inside
the descriptor (this is why `descriptor_heap::write_image` takes a create info). The renderer builds its views
through `core::make_image_view`, which throws its create info away, so the heap twin needs that info again.

IT DOES NOT NEED NEW PLUMBING: `deren::vulkan::make_image_view_info(image, format, view_type, aspect, mip_levels,
array_layers)` - a `constexpr` function in **`deren.vulkan.constant_init`**, which `runtime.cpp` already imports - is
already the one place that builds it, and `core::make_image_view` is a call to it plus `vkCreateImageView`. So
the rule for every image is: write the heap descriptor **where the image and its view are created**, from the
same `make_image_view_info` arguments - not in `write_*_bindings`, which only carry handles.

Two consequences worth knowing before doing it:

- The site that WRITES the heap descriptor has to be the site that knows the format and range. `set_ibl` knows
  them (it creates the environment, irradiance and BRDF-LUT images), while `write_ibl_bindings` only sees the
  three views - so the IBL writes belong in `set_ibl`.
- A binding that CHANGES VIEW later needs a heap rewrite beside that change: the furnace mode points slots 0 and
  1 at the constant cube instead of the real environment (`write_ibl_bindings`), and a heap descriptor written
  only at creation time would keep reading the old image. The per-frame slot arrays are the same kind of trap
  seen from the other side: which SLOT is read is a shader index, and it is the host's job to keep the contents
  of every slot that index can name current.

## The grid as it stands (measured at startup)

```
descriptor heap: available (VK_EXT_descriptor_heap, revision 1)
SUCCESS: descriptor heap created (resource 1088 KiB at ..., sampler 128 KiB at ...;
         strides buffer 16 B, image 32 B, sampler 32 B)
descriptor heap: the reserved window ends at 96768 B, so the grid's 1 MiB base is 1048576 B away
descriptor heap: slot grid at 1048576 (1024 slots x 64 B; textures 0 materials 512 tlas 513
                 camera 514 light 516 clusters 518/520 gbuffer 551 env 532 lut 534)
```

- The resource grid's base is **1 MiB** and its stride **64 B**; the slot numbers (relative to that base) are
  the constants in `shaders/heap_slots.glsl` and `core::heap_slots`. 703 of the 1024 slots are named.
- The SAMPLER heap is a second grid at a **64 KiB** base with the device's 32 B stride: the API caps that heap
  at 128 KiB, so it cannot use the resource grid's 1 MiB. Refused unless the device's reserved window fits
  below the base and its sampler stride is 32 B.
- **The heap sizes are derived from the layout, not guessed.** They were 256 KiB and 64 KiB, and both were
  wrong in ways only the log showed: 256 KiB could not fit the 1 MiB base ("a reservation of 951808 B does not
  fit the 262144 B resource heap", after which the grid landed on the reserved window at 96768 B and was
  refused), and 64 KiB of sampler heap is *exactly* the reserved window the embedded-sampler path requires,
  i.e. no usable sampler space at all. They are 1088 KiB and 128 KiB now.
- Still inert: nothing reads the grid, so the frame is unchanged (gate 9/9, `changed: 0`).
