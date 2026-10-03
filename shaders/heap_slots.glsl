// THE SLOT GRID: where every descriptor of this renderer lives, as a HEAP-NATIVE shader sees it.
//
// A shader that declares `layout(descriptor_heap) uniform texture2D t[];` can only say `t[i]`, and
// GL_EXT_descriptor_heap resolves that to `heapBase + i * stride` - every such array aliases the heap from
// offset 0, so an INDEX IS A BYTE OFFSET DIVIDED BY THAT KIND'S STRIDE, and the only way for a shader to find
// its data at a compile-time-known index is for the data to sit on a grid with a FIXED base. Both heaps start
// that grid at 1 MiB (past any reserved window a driver reports here: 94 KiB of resource heap, 64 KiB of
// sampler heap), the stride is fixed at 64 B - the smallest power of two covering the device's largest
// descriptor, 32 B - and each array below is a CONTIGUOUS RUN OF SLOTS.
//
// THESE NUMBERS MUST EQUAL vulkan/core/core.cppm's `core::heap_slots`, which is where the host reserves them;
// the host logs its own table at startup (`descriptor heap: slot grid ...`), which is how a drift is seen.
// See docs/descriptor_heap_migration.md.
//
// The stride override is what makes the grid possible: `layout(descriptor_heap, descriptor_stride = 64)`.
// GL_EXT_descriptor_heap requires that value to be a power of two, and the host writes its descriptors 64 B
// apart, so both sides agree on where slot N is.

#ifndef HEAP_SLOTS_GLSL
#define HEAP_SLOTS_GLSL

// THE TWO INDICES A HEAP-NATIVE STAGE NEEDS ARE PUSH CONSTANTS, and they live INSIDE each stage's own block: a
// heap pipeline has no pipeline layout, so vkCmdPushConstants has nothing to push to, but vkCmdPushDataEXT supplies
// exactly the same bytes and the shader reads them exactly as it always read push constants (the extension proposal
// says so in those words, and the heap-native probe proves it). A push_constant BLOCK may not carry an `offset`
// qualifier - measured, glslang says "only applies to block members" - so there is no shared block to put them in:
// each stage appends `uint frame_slot; uint image_index;` to ITS OWN block (at the end, so every existing field
// keeps its offset) and then aliases them to the two names the slot macros in heap_slot_constants.glsl use:
//
//     #define heap_frame_slot (push.frame_slot)
//     #define heap_image_index (push.image_index)
//
// (with `push` the stage's own block instance name). A stage that uses neither pays 8 bytes of push block.

// THE CONSTANTS AND THE SLOT MACROS LIVE IN heap_slot_constants.glsl, which BOTH shading languages can
// read: the migration to Slang needs the same numbers without a second copy, and that file is what
// tests/test_render_resources.cpp parses against core::heap_slots. Only the shared heap arrays and the
// fetch helper below are GLSL-specific, so only they stay here.
#include "heap_slot_constants.glsl"

// THE SHARED HEAP ARRAYS, declared once here rather than per header: more than one converted file needs them
// (surface.glsl the textures, shading.glsl the samplers) and GLSL has no way to declare the same array twice in
// one stage. A stage that uses neither pays nothing for them - an unused declaration compiles away - and a stage
// that uses both gets ONE declaration, which is the whole reason they live in the shared file. They come LAST
// because `descriptor_stride` has to be a constant expression, and heap_slot_stride is declared above.
#ifndef VR_SLANG
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D heap_textures[];
layout(descriptor_heap) uniform sampler heap_samplers[];

// ---- THE ACCESSORS: the names a SHARED body uses to reach the heap ----
//
// One per resource family, and this is what makes the migration safe: the GLSL definition of each accessor
// expands to EXACTLY the expression it replaced, so rewriting a shared body to call it is verifiable -
// the capture gate must not move by a pixel, and a moved frame means an accessor is wrong. The Slang side
// defines the same names over `DescriptorHandle<>` accessors in shaders/heap_access.slang, which is the
// only place the two languages differ. See docs/slang_migration.md.
#define material_at(slot, index) heap_material_tables[slot].materials[index]
#define camera_at(slot) camera[slot]
#define light_at(slot) light[slot]
#define cluster_count_at(slot, cluster) cluster_counts[slot].counts[cluster]
#define cluster_indices_at(slot, i) cluster_indices[slot].indices[i]
// THE LIGHT'S CASCADE MATRICES GO THROUGH AN ACCESSOR, not through `light_at(...).light_view_proj[...]`, and
// that is a Slang requirement rather than tidiness: Slang lowers a matrix ARRAY member of a block into an
// inner struct decorated with `ArrayStride` but WITHOUT `ColMajor`/`MatrixStride`, and a stage that read the
// cascade that way projected every draw out of the light's frustum - an empty shadow map, which read as
// `deferred` and `shadow_single` producing IDENTICAL frames. The GLSL definition below expands to exactly the
// expression it replaces, so the GLSL side does not move by a pixel, and the Slang definition in
// heap_access.slang reads the same slot as a structured buffer of matrices, whose decorations ARE correct.
// See docs/slang_migration.md section 9.
#define light_matrix_at(slot, index) light[slot].light_view_proj[index]
// A MATRIX MEMBER'S TYPE, per language: GLSL's `mat4` is column-major, and on the Slang side it is the
// HLSL keyword `row_major` that emits SPIR-V ColMajor (the keyword reads BACKWARDS - `column_major` emits
// RowMajor). ColMajor is what matches glslc's `mat4` and the host's column-major glm::mat4. See
// docs/slang_migration.md, and heap_access.slang for the Slang definition.
#define VR_MAT4 mat4

/**
 * @brief one texel from a heap image, through the sampler at @p sampler_slot
 * @param tex the image, already indexed by its slot (heap_slots_x + heap_frame_slot or + heap_image_index)
 * @param sampler_slot which sampler heap entry to use (heap_sampler_gbuffer for exact fetches, and so on)
 * @note THE TWO HALVES OF A FETCH ARE SEPARATE IN A HEAP - the image is a resource heap descriptor and the sampler
 *       a sampler heap one - and a combined image sampler cannot be declared at all, so every fetch has to name
 *       both. This exists so that naming both happens in one place instead of at sixty-one sites.
 *
 * A MACRO RATHER THAN A FUNCTION, and that is measured rather than a preference. As a function it produced a
 * module that glslang accepted and spirv-val rejected - "OpFunctionCall Argument <id>'s type does not match
 * Function <id>'s parameter type", every failing call a call to this helper, at vkCreateShaderModule
 * (VUID-VkShaderModuleCreateInfo-pCode-08737): a `texture2D` parameter does not survive a function boundary.
 * The extension's own rule points the same way - a sampler constructor must appear at the point of USE - and a
 * macro expands there by definition, so the fetch below is built exactly where it is written.
 *
 * The three parameters are fixed, and that is enough: a comma inside a call's argument list does not separate
 * arguments when it is inside parentheses, and every uv expression here keeps its commas there
 * (`uv + t * vec2(2.0, 0.0)`). A VARIADIC macro would have been the obvious way to be safe and it is not
 * available - glslang's preprocessor refuses `__VA_ARGS__` outright ("'#define' : bad argument"), which cost one
 * build to learn.
 */
#define heap_texel(tex, sampler_slot, uv) texture(sampler2D((tex), heap_samplers[sampler_slot]), (uv))
#else
// SLANG SEES NONE OF THE ABOVE, and the reason is measured: `layout(descriptor_heap, ...)` is the ONE
// construct Slang's GLSL mode rejects (E31217 unrecognized GLSL layout qualifier - eleven of them, one per
// declaration, and nothing else in the shared code). A combined image sampler cannot be built from its two
// halves there either, so the whole heap surface - the arrays, `heap_texel`, `heap_sample` and the
// accessors - is provided by shaders/heap_access.slang instead, over DescriptorHandle<> values fetched
// from literal slots. A SHARED BODY MUST THEREFORE TOUCH THE HEAP ONLY THROUGH THOSE NAMES.
#endif

#endif // HEAP_SLOTS_GLSL