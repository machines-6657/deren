// shaders/heap_access.slang - THE SLANG HALF OF THE BINDING MODEL.
//
// A shared body (surface.glsl, shading.glsl, sky.glsl, ibl_specular.glsl) is included by both languages and
// touches the heap ONLY through the names defined here or in heap_slots.glsl's GLSL branch. This file is
// that GLSL branch's counterpart, and the two are the only place the languages differ.
//
// WHY IT IS NEEDED AT ALL, measured: Slang's GLSL mode parses every function, type, macro and constant in
// the shared code, and rejects exactly one construct - `layout(descriptor_heap, ...)` (E31217,
// "unrecognized GLSL layout qualifier"). So the DECLARATIONS cannot cross, and this side reaches the same
// descriptors through Slang's `DescriptorHandle<T>` instead.
//
// HOW THE TWO HEAPS MAP, and this is the part that makes the port small: a `DescriptorHandle<T>` is a
// uint2 whose `.x` is an index into the RESOURCE heap and, for a CombinedTextureSampler, whose `.y` is an
// index into the SAMPLER heap - Slang then emits the OpSampledImage at the point of use. That is exactly
// this renderer's model (one flat 64-byte resource grid, one 32-byte sampler grid, the two halves bound
// separately), so every fetch helper the GLSL files already have keeps its name and its signature and only
// its BODY differs.
//
// THE ACCESSOR NAMES MUST MATCH heap_slots.glsl's GLSL definitions, because both sides have to satisfy the
// same shared bodies. A drift is not silent: the capture gate compares every stage's frame against the
// same references, and the two sides render the same scenes.

// THE GRID'S NUMBERS come from the same file the GLSL side includes, so the two cannot drift (and a stage
// that also includes heap_slots.glsl does not define them twice - that file has its own guard).
#include "heap_slot_constants.glsl"

// A MATRIX MEMBER'S TYPE on this side: `row_major` is what emits SPIR-V ColMajor, which is what the GLSL
// `mat4` members and the host's column-major glm::mat4 are (the keyword reads backwards - `column_major`
// emits RowMajor). heap_slots.glsl defines the GLSL half.
#define VR_MAT4 row_major float4x4

// A handle for an absolute slot: `.x` is the resource heap index, or - for a SamplerState - the sampler
// heap index (Slang uses the same component for both; only a combined sampler uses `.y`).
DescriptorHandle<T> heap_at<T>(uint slot) where T : IOpaqueDescriptor
{
    return (DescriptorHandle<T>)uint2(slot, 0u);
}

// A combined image sampler from the two heap slots, which is what a GLSL `sampler2D(tex, samp)` is here.
DescriptorHandle<Sampler2D> combined_at(uint texture_slot, uint sampler_slot)
{
    return (DescriptorHandle<Sampler2D>)uint2(texture_slot, sampler_slot);
}

// ---- THE ACCESSORS (the GLSL definitions live in heap_slots.glsl) ----
//
// The struct types in these macros (`Material`, `CameraUBO`, `LightUBO`) are declared by the shared bodies
// themselves, which is fine because a macro is text: it is expanded at the USE site, by which time the
// bodies have declared them. That is also why the two cluster accessors name `StructuredBuffer<uint>`
// rather than the GLSL blocks - those blocks' `uint counts[];` / `uint indices[];` members are just uint
// arrays, and Slang has no block-member indirection over a structured buffer.
#define material_at(slot, index) heap_at<StructuredBuffer<Material>>(slot)[index]
#define camera_at(slot) heap_at<ConstantBuffer<CameraUBO>>(slot)
#define light_at(slot) heap_at<ConstantBuffer<LightUBO>>(slot)
// THE FACE SDF'S HEAD FRAME (scene block slot 749): three axes per frame slot, resolved by the application from
// the character's head bone or from the reference's fallback and uploaded every frame - see head_ubo in
// vulkan/primitive.cppm. The struct is declared by the stage that reads it, like every other block here.
#define head_frame_at(slot) heap_at<ConstantBuffer<HeadUBO>>(slot)
/// the TOON LIGHT RIG, one block for the run - see `deren::vulkan::toon_rig` and the stage's own `ToonRigUBO`.
#define toon_rig_at(slot) heap_at<ConstantBuffer<ToonRigUBO>>(slot)
/// THE MATERIAL COLOURS, one flat `float4` array: read as `toon_colour_at(slot, material * kLanes + lane)`, because
/// the lanes are per material and a flat array keeps the stride question away (see `deren::vulkan::toon_colour_lane`).
#define toon_colour_at(slot, index) heap_at<StructuredBuffer<float4>>(slot)[index]
/// THE SECOND BLOCK of the toon lane table (lanes 8..11, see `deren::vulkan::toon_slot`): the same buffer, read at the
/// middle `uvec4` - `material_index * 3 + 1`, a stride the stage names as `character_toon_lane_blocks`.
#define toon_lanes2_at(slot, index) heap_at<StructuredBuffer<uint4>>(slot)[(index) * 3u + 1u]
/// THE THIRD BLOCK (lanes 12..15), which STEP 7 raised `deren::vulkan::toon_lane_blocks` TO 3 FOR: the FACE container's
/// three masks (`_GooFaceSDF` / `_GooFaceCmM` / `_GooFaceCsutm`) are lanes 11..13, and lane 11 closed the second
/// block. The three accessors must move TOGETHER - a stale stride here reads another material's block, which is
/// the silent-drift failure the note on `toon_lanes_at` below records.
#define toon_lanes3_at(slot, index) heap_at<StructuredBuffer<uint4>>(slot)[(index) * 3u + 2u]
#define cluster_count_at(slot, cluster) heap_at<StructuredBuffer<uint>>(slot)[cluster]
#define cluster_indices_at(slot, i) heap_at<StructuredBuffer<uint>>(slot)[i]

// ---- THE VERTEX-SIDE MATRIX ARRAYS, and why they are structured buffers here ----
//
// Each GLSL declaration in the vertex leaves is `readonly buffer X { mat4 arr[]; } x[];`, i.e. a heap slot
// holding a runtime array of `mat4`. The Slang counterpart of "a runtime array of mat4" is a structured
// buffer whose ELEMENT is the matrix, and the element type is `VR_MAT4` (`row_major float4x4`) for the same
// measured reason a block member is: that spelling is what emits SPIR-V `ColMajor` with `MatrixStride 16`,
// which is the layout the host's column-major glm::mat4 arrays have. The shared `= 0u`-style slot argument
// is the caller's, so these macros only name the SHAPE.
//
// THEY ARE DEFINED HERE RATHER THAN IN A SHARED BODY because the families are read by the vertex LEAVES: a
// leaf may touch the heap directly, and the GLSL side of these five spells the same reads inline in its own
// file (there is no shared body to keep in step). They exist so that pbr.slang and the shadow.vert port
// that follows it read the same way in both languages.
#define instance_transform_at(slot, index) heap_at<StructuredBuffer<VR_MAT4>>(slot)[index]
#define previous_transform_at(slot, index) heap_at<StructuredBuffer<VR_MAT4>>(slot)[index]
#define skin_matrix_at(slot, index) heap_at<StructuredBuffer<VR_MAT4>>(slot)[index]
#define skin_matrix_previous_at(slot, index) heap_at<StructuredBuffer<VR_MAT4>>(slot)[index]
#define morph_value_at(slot, index) heap_at<StructuredBuffer<float>>(slot)[index]
// THE TOON LANES BESIDE THE MATERIAL RECORD: one `uvec4` per material - x is its `_SDFLightmap` texture-array
// index, y its `_MetallicGlossMap`'s, z and w reserved - with 0 in a component meaning "do not read", the same
// contract the record's four lanes use. It is a structured buffer rather than members of the material record
// because the record is INLINE in the per-draw push block and a word added to it moves every offset in
// `surface.glsl` and `shadow.slang` - see core::heap_slots::toon_lanes.
/// THE FIRST BLOCK of the toon lane table (lanes 4..7), read at `material * blocks + 0`.
///
/// THE STRIDE IS THE WHOLE POINT OF THIS MACRO AND IT WAS WRONG FOR THREE ROUNDS: the table holds
/// `deren::vulkan::toon_lane_blocks` `uvec4`s per material, so block 0 sits at `material * blocks`, block 1 at
/// `material * blocks + 1` and block 2 at `material * blocks + 2` - while this accessor read `[material]` and the
/// host wrote `[material * 2]`. The two sides
/// therefore disagreed about EVERY material: element `m` held the host's material `m / 2`, block `m % 2`. What that
/// looked like is the reason the codebase documents silent drift rather than assuming it: the FACE (material 5) read
/// element 5 = material 2's block 1 = all zeroes, so it silently lost its SDF, its mask and its expression lane at
/// once; the cloth read another material's lanes; and the FRAME still looked like a character - which is how it went
/// unnoticed until a probe on the face rim came back with a 0-pixel difference.
///
/// STEP 7 CHANGED THE `2u` TO `3u` AND THAT IS THE ONE THING ABOUT IT THAT CANNOT BE VERIFIED BY READING: the
/// literal below, `toon_lanes2_at`'s, `toon_lanes3_at`'s, the host's `toon_lane_blocks`, the bytes
/// `material_capacity * toon_lane_blocks * sizeof(uvec4)` allocates and the copy the runtime makes into it are SIX
/// spellings of one number, and `tests/test_toon_material_sidecar.cpp` pins the first three against the fourth.
#define toon_lanes_at(slot, index) heap_at<StructuredBuffer<uint4>>(slot)[(index) * 3u]

/// one cascade's world -> light-clip matrix, read as the FIRST matrix array of the light buffer.
/// @note WHY NOT `light_at(slot).light_view_proj[index]`, measured: Slang lowers a matrix ARRAY member of a
///       block into an extra inner struct and decorates it with `ArrayStride 64` but NO `ColMajor` /
///       `MatrixStride` (a single matrix member, like the camera's, is decorated correctly). A vertex stage
///       that read the cascade through the block produced an empty shadow map - every draw projected out of
///       the light's frustum, so `deferred` and `shadow_single` came out identical, i.e. uniformly lit. A
///       heap descriptor IS an address range, so reading the same slot as a structured buffer of `VR_MAT4`
///       gives element i = the i-th matrix of that array, with the `ArrayStride 64` + `ColMajor` +
///       `MatrixStride 16` decorations verified in the built module. See docs/slang_migration.md.
#define light_matrix_at(slot, index) heap_at<StructuredBuffer<VR_MAT4>>(slot)[index]

// ---- THE FETCH HELPERS the GLSL side defines as macros or functions ----

/// one texel from a heap image, through the sampler at @p sampler_slot (the GLSL side is the
/// `heap_texel` macro: a `texture2D` parameter does not survive a function boundary in GLSL, while Slang
/// has no such restriction - so this side can be, and is, a function).
float4 heap_texel(DescriptorHandle<Texture2D<float4>> texture, uint sampler_slot, float2 uv)
{
    return texture.Sample(heap_at<SamplerState>(sampler_slot), uv);
}

/// one texel at an EXPLICIT mip 0, for the stages that are not allowed an implicit-LOD fetch.
/// @note WHY THIS EXISTS, MEASURED THE HARD WAY: `Sample` lowers to `OpImageSampleImplicitLod`, which the
///       SPIR-V environment only allows in the Fragment, GLCompute, MeshEXT and TaskEXT execution models -
///       implicit LOD needs derivatives, and a RAYTRACING stage has none. The GLSL spelling of the same fetch
///       (`texture(sampler2D(tex, samp), uv)`) is lowered by glslc to an explicit-Lod fetch for exactly this
///       reason, so an RT or compute leaf that reaches for `heap_texel` produces a module that spirv-val
///       REJECTS (`ImplicitLod instructions require Fragment, GLCompute, MeshEXT or TaskEXT execution model:
///       ImageSampleImplicitLod`, caught by the validation layer at vkCreateShaderModule) while the driver
///       still runs it and renders the RIGHT frame - which is why this was found by the gate's LOG check and
///       not by its frame comparison. Any non-fragment stage that samples a heap image wants this function.
float4 heap_texel_lod0(DescriptorHandle<Texture2D<float4>> texture, uint sampler_slot, float2 uv)
{
    return texture.SampleLevel(heap_at<SamplerState>(sampler_slot), uv, 0.0);
}

/// the size of a heap image at a mip, as a float2 - GLSL's `vec2(textureSize(sampler2D(tex, samp), lod))`
/// @note the post chain's filters derive their tap spacing from the level they are reading, so they need the
///       SIZE of a slot rather than a resolution constant; the GLSL side spells `textureSize` inline.
float2 heap_size(DescriptorHandle<Texture2D<float4>> texture, uint lod)
{
    uint width, height, levels;
    texture.GetDimensions(lod, width, height, levels);
    return float2(float(width), float(height));
}

/// the bindless-array fetch the shared bodies call by name (the GLSL definition is in surface.glsl): the
/// material's own image through the shared texture sampler. The GLSL side builds `sampler2D(tex, samp)` at
/// the point of use; here the two slots travel in ONE combined handle instead.
float4 heap_sample(uint texture_index, float2 uv)
{
    return combined_at(heap_slots_textures + texture_index, heap_sampler_texture).Sample(uv);
}

// ---- THE IBL FETCHES (the GLSL definitions live in ibl_specular.glsl) ----

/// the prefiltered chain's mip count, QUERIED from the texture rather than hardcoded
int env_cube_levels()
{
    uint width, height, levels;
    heap_at<TextureCube<float4>>(heap_slots_env_cube).GetDimensions(0u, width, height, levels);
    return int(levels);
}

/// one sample of the prefiltered environment at an explicit mip (GLSL's `textureLod`)
float3 env_cube_sample_lod(float3 dir, float lod)
{
    return heap_at<TextureCube<float4>>(heap_slots_env_cube).SampleLevel(heap_at<SamplerState>(heap_sampler_texture), dir, lod).rgb;
}

/// the BRDF integration LUT's `.rg` at a (ndotv, roughness) point
float2 brdf_lut_sample(float2 uv)
{
    return heap_at<Texture2D<float4>>(heap_slots_brdf_lut).Sample(heap_at<SamplerState>(heap_sampler_texture), uv).rg;
}

// ---- THE SHADOW AND IRRADIANCE FETCHES (the GLSL definitions live in shading.glsl) ----

/// one comparison fetch from the cascade array (GLSL's `sampler2DArrayShadow` + `texture(...)`): Slang
/// names the comparison sampler as a type instead, and `SampleCmpLevelZero` is the comparison fetch.
/// @param slot the shadow map's heap slot, passed in by the caller: a shim is compiled before the push
///        block that `heap_shadow_slot` needs, so the macro cannot be expanded here
float shadow_sample(uint slot, float2 tap, int cascade, float depth)
{
    return heap_at<Texture2DArray<float>>(slot).SampleCmpLevelZero(heap_at<SamplerComparisonState>(heap_sampler_shadow), float3(tap, float(cascade)), depth);
}

/// the diffuse ambient: the irradiance map at the world normal
float3 irradiance_sample(float3 n)
{
    return heap_at<TextureCube<float4>>(heap_slots_irradiance_cube).Sample(heap_at<SamplerState>(heap_sampler_texture), n).rgb;
}
