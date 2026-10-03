# What the three migrations bought, and what they cost

The renderer went through three migrations that are usually discussed separately and are in fact ONE
architecture:

| migration | what it provides | its own record |
| --- | --- | --- |
| the descriptor heap | the binding model: no descriptor sets, no layouts, every resource a slot in one flat grid | docs/descriptor_heap_migration.md, docs/descriptor_heap_handover.md |
| Slang | the compiler: one .slang file per family with an entry per stage, and the heap as a first-class capability | docs/slang_migration.md |
| mesh shaders | the first consumer that NEEDS geometry to arrive as data, and the reason the per-meshlet culling exists at all | docs/mesh_shaders.md |

Each one is a precondition for the next: a mesh stage has no input assembler, so its geometry must
arrive as pushed data - which is only possible because no pipeline has a layout to push constants
into; and a stage that reads the whole heap by index is only expressible because the compiler has a
descriptor-heap capability. This document is the cross-cutting account: what the three bought, what
they cost, and - stated separately, because it is the part that is easiest to overclaim - **what has
NOT been measured**.

## 1. The descriptor heap

### What it bought

- **Binding disappeared from the API.** No set layouts, no pipeline layouts, no pools, no
  write/update/derive cycle: a shader names a slot (`heap_slots_gbuffer_depth + heap_image_index`)
  and the resource's identity IS its position in the grid. The migration deleted the classic
  descriptor world's **41 files** (docs/descriptor_heap_migration.md).
- **The heap is what makes a mesh stage feedable.** A heap descriptor for a buffer is an address
  range, so a mesh stage can fetch the vertices the input assembler used to bind - that is the whole
  mechanism behind `mesh_geometry_lanes`, and it is why the geometry window travels in the push block
  instead of in a binding.
- **Resource lifetime became an explicit rule**: what is written ONCE owns one slot (the meshlet
  table), what is rebuilt per frame owns one per frame in flight (the TLAS). "Is there a frame whose
  contents could disagree with this?" is now a question with a written answer.
- **One structure, two consumers**: the TLAS and the meshlets read the SAME vertex and index buffers
  by device address, so ray-traced shadows and the meshlet path cannot drift apart.

### What it costs

- **The push block became a whole-block contract.** With no layout, every declared byte must have been
  written by `vkCmdPushDataEXT` before the draw (VUID-vkCmdDrawMeshTasksEXT-None-11376 and its
  `vkCmdDrawIndexed` twin), so a draw pays for the union of what its stages declare - 32 bytes of
  geometry lanes per draw that a vertex stage never read, when a vertex stage still existed.
- **144 bytes is more than the specification guarantees.** The stage block is 144 B against the 128 B
  every implementation must support, so "how big is one draw's pushed data" became a device
  requirement rather than an implementation detail: this machine reports 256/256, and the same box's
  other GPU reports 128.
- **The contract is the shader's to keep.** A descriptor's storage class must match how the shader
  reads it: a UNIFORM descriptor read through a `StorageBuffer` pointer returns ZEROS with no
  validation finding at all (docs/slang_migration.md, section 9).
- **Slot numbering is positional.** Growing an array in place renumbers every array after it, which is
  why the meshlet table, the counters and the culled table were appended at 745/746/747 rather than
  inserted where they belonged.
- **A wrong slot is not an error, it is other memory.** Reading the wrong slot yields a defined read of
  a different resource: the heap probe exists because the difference between "the right slot" and
  "the wrong slot" is white versus black pixels, with nothing in between to warn you.

## 2. Slang

### What it bought

- **One file, many entries - and one shared body.** `pbr.slang` holds the fragment entry, the mesh
  entry and the meshlet entry, and all three call the same `pbr_shade_vertex`. That is what makes
  "the mesh path produces the same pixels as the vertex path" a property of the SOURCE rather than a
  hope, and it is why the capture gate could accept each stage byte-for-byte.
- **An incremental migration was possible.** `-allow-glsl` plus `#include` let the shared GLSL bodies
  stay where they were while the 24 stages moved one at a time, each verified against the GLSL build.
- **Codegen became checkable.** The migration's acceptance included instruction-level evidence: 3
  `OpImageSampleDref` for the PCF fetch, 7 cube samples, `OpVectorTimesMatrix` count 0, and
  `inverse()` lowering to `OpExtInst MatrixInverse` in BOTH compilers rather than being inlined - so
  the two compilers agreed bit for bit where it mattered.
- **The heap is a capability, not an extension of the language.** `-capability spvDescriptorHeapEXT`
  with the heap strides is what lets a mesh entry read the same grid every other stage reads.

### What it costs

- **Silent semantic differences, several of which were a wrong picture or a wedged GPU**: `mul(M, v)`
  on a row-major matrix emits the TRANSPOSED product (the GLSL spelling `M * v` is the correct one,
  and getting it wrong blacked out the motion channel); Slang's default majorness is row-major, so a
  plain `mat4` member of the push block is read transposed (`VR_MAT4` exists for this); a matrix
  ARRAY inside a block loses its majorness and produced an empty shadow map (TRAP 3).
- **Different surface rules**: `[[vk::location(0)]]` on a return type is `E31002` (which is why the
  entries keep `: SV_Target`), west-const is rejected (`const MeshVertex v`, not `MeshVertex const
  v`), and a shared header's `export const` needs `static const` (`E31223` - measured while adding
  the culling counters).
- **The compiler is a single, pinned point of failure.** CI installs one Slang version and asserts
  `slangc:` in the configure log, with no glslc fallback. On this toolchain a TASK stage cannot be
  given host data at all: slangc v2026.18.2 dies with `0xC0000005` on a six-line entry point that
  declares a push block, and again on one that only includes the heap shim - which is why the
  pre-dispatch culling had to be done on the host instead of in a task stage.
- **Two dialects live in one tree.** The leaves are Slang and the shared bodies are GLSL, kept that
  way deliberately to make the migration incremental; readers pay for it, and so do tools.
- **"Equivalent to glslc" is true per case, not in general.** Rounds needed documented re-baselines:
  `-fp-mode precise` made nine scenarios differ, and one round left eight of ten scenarios differing
  by 1 to 4 pixels. The acceptance rule is therefore byte-identical FRAMES plus a written reason
  whenever a re-baseline is needed - not "the compiler is transparent".

## 3. Mesh shaders

### What it bought

- **Per-meshlet culling with no CPU draw list.** Each meshlet carries an object-space sphere, the
  entry point tests it against the pass's clip volume and emits NOTHING when it fails. Measured on
  `sponza` over 40 frames: 118541 meshlet workgroups dispatched, 75741 emitted, **42800 rejected
  (36%)**; on `deferred` (DamagedHelmet) 8554 dispatched and 710 rejected (8%).
- **The cheapest stage turned out to be the host.** The camera's runs are now culled while they are
  recorded - the host already holds the primitive's meshlets, the draw's model matrix and the camera -
  so the dispatch asks for the survivors only: 118541 workgroups became **84675**, i.e. **33866
  launches never happen**, and the triangles emitted are **identical to the digit** (6325695) with
  every scenario byte-identical. A shadow-off run measures the same 33866 independently.
- **The counts are data, not arguments.** Every meshlet dispatch goes through
  `vkCmdDrawMeshTasksIndirectEXT`, reading its three counts out of a command table at a slot the
  primitive owns, so a GPU-side culling pass can rewrite them with no host change - and the route is
  logged, because the first version of that seam silently fell back to the direct call and looked
  perfect.
- **The draw path got smaller.** No input assembler, no vertex/index binding: `bind_geometry_and_push`
  and both `vkCmdDrawIndexed` paths for scene geometry are gone, and the three draw strategies are a
  guard, a lane push and a dispatch.

### What it costs

- **`VK_EXT_mesh_shader` is a REQUIREMENT.** The vertex geometry path was removed in step 4 of
  docs/mesh_shaders.md, so a device without the extension - or with `maxPushConstantsSize` under the
  144-byte block - panics with the reason before any pass is created. This is a deliberate decision,
  and it is the single largest cost in this document.
- **A layout bug does not look like a bug.** The meshlet record was 28 bytes on the host against the
  shader's std430 stride; the symptom was not a wrong picture but a GPU that stopped signalling
  fences. Every record layout is now `static_assert`ed field by field.
- **Correctness rules that are easy to forget, each one learned by measuring**: a DEFORMING draw must
  never be culled per meshlet (the bounds describe the bind pose), an INSTANCED draw cannot use one
  compacted run (its world matrix varies per workgroup), and a TWO-SIDED draw must not be culled for
  facing away (the rasterizer keeps both sides on purpose).
- **Back-face culling is measured and NOT shipped.** With the two-sided-draw and `cone_cos` mistakes
  of the first attempt fixed, the cone test still removes geometry the rasterizer was drawing, in
  BOTH orientations (3 of the core 5 scenarios change). docs/mesh_shaders.md lists the three
  candidate causes and the experiment that settles each.
- **Two known gaps remain**: the shadow pass's share of the host culling (8934 workgroups over 40
  frames of `sponza`, because its frustum is per cascade) and the fact that a static draw's meshlet
  run is dispatched once per CHUNK (invisible in a depth pass, wrong when two chunks of one primitive
  disagree about a material).
- **The vertex-reuse argument was NOT banked.** The meshlet entry emits three vertices per triangle -
  the direct equivalent of what the input assembler produced - so this migration bought structure and
  culling, not vertex shading bandwidth. A meshlet COULD share vertices inside its workgroup; that is
  a further step, not a property of what shipped.

## 4. The cost they share: the contracts moved out of the API

Every item above is the same trade seen from a different side. The classic API held the contracts -
a binding told you what a stage could read, a layout told you what a push contained, a vertex input
state told you the stride - and the three migrations moved all of it into the SOURCE, the DOCUMENTS
and the TESTS:

- the push contract is a VUID plus a helper that always pushes the whole block;
- the slot grid is a pair of constants files that have to agree (compared by
  `tests/test_render_resources.cpp`);
- the record layouts are `static_assert`ed against what `spirv-dis` shows;
- the shader list lives in four places (compared by `tests/test_shader_sources.cpp`);
- and the documents themselves are now a BUILD INPUT whose invariants are checked by
  `tests/test_docs.cpp` - every `docs/*.md` in Doxyfile's INPUT, ASCII only because pdflatex reads
  them, anchors that resolve, fences that balance, and no document naming a heap slot that no file
  declares.

That is why this project carries a document set and a 600-page manual: with no descriptor sets and
no layouts, the reasoning IS the interface.

## 5. What is NOT claimed

Stated plainly, because the documents above contain a lot of numbers and it is easy to read more into
them than they say:

- **Nothing here has been TIMED.** The culling figures count WORK (workgroups, meshlets, triangles),
  not milliseconds. "36% of the meshlet workgroups are rejected" does not mean 36% less frame time;
  the counters can be extended to say more, and the overlay's GPU marks are the instrument that does.
- **No multi-vendor data.** Every measurement was taken on one machine (RTX 4060 Laptop, driver
  616.92). The extension's support matrix and the 144-byte push requirement are the portability
  questions, and neither has been tested beyond this box's two GPUs.
- **The gate cannot tell which stage drew a frame.** A mesh stage and the vertex stage it replaced
  produce the same pixels by construction, which is exactly why the acceptance for each stage was a
  byte-identical frame PLUS a forced probe (emit nothing, or cull everything, and watch the frame
  change) PLUS a log line saying which route was taken.
- **Two migrations' own records are history, not current state.** `pbr.vert` and `shadow.vert` are
  named throughout docs/slang_migration.md because that document records the Slang port; those
  entries were removed later, and each mention says so where a reader meets it.
