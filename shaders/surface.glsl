/**
 * @file shaders/surface.glsl
 * @brief Shared material-surface gathering: the material table lookup, the glTF alpha tests, the
 *        tangent-space normal mapping and the texture-derived factors.
 * @ingroup shaders
 *
 * Included by both fragment stages that shade a scene surface - pbr.frag (the forward path) and
 * gbuffer.frag (the deferred path's G-buffer write). Both need exactly the same answers to "what
 * is this surface made of", and it is the part of the shading chain where a divergence would be
 * least visible and most confusing (a deferred frame whose albedo disagreed with the forward
 * frame's). Keeping the fetch in one place also keeps the glTF semantics (MASK discard, the
 * double-sided normal flip, occlusion_strength) in one place.
 *
 * What stays OUT of here: the lighting itself (the forward path evaluates it in pbr.frag; the
 * deferred path evaluates it in the lighting pass from what the G-buffer stored), the camera /
 * light / shadow bindings, and anything that depends on the pass.
 *
 * The descriptor bindings declared here (1 = the texture array, 5 = the material table) and the
 * push constant block are the shared scene set convention - see docs/shaders.md. A shader that
 * includes this file must NOT declare them again, and a host that creates a pipeline from such a
 * shader must use the runtime's shared scene pipeline layout (it does: every scene pipeline shares
 * deren::vulkan::core::scene_pipeline_layout).
 */

#ifndef DEREN_SURFACE_GLSL
#define DEREN_SURFACE_GLSL

// THE GEOMETRY LANES A MESH STAGE OF THIS FILE'S OWNERS NEEDS (docs/mesh_shaders.md step 2). The block below ends
// with them because their offsets are the block's: the material fields (96 B), the two heap index lanes (8 B) and
// then `MeshGeometryLanes` - whose first member is a `uint2`, so it lands on the 8-byte boundary at
// mesh_geometry_offset (112) that the host pushes them at. The VERTEX and FRAGMENT entries of every stage that
// includes this file declare the same lanes and never read them; only a `mesh_main` entry does, and only
// `shaders/mesh_geometry.slang` - included here so the type exists - knows how to fetch through them.
#include "mesh_geometry.slang"

// The runtime's texture array: every material's image lives in one bindless array, indexed by the
// material record. HEAP-NATIVE (see docs/descriptor_heap_migration.md): the array IS the heap - one 64 B slot per
// texture, starting at heap_slots_textures - and the sampler is SEPARATE, because a combined image sampler cannot
// be declared this way at all: every fetch constructs one, `sampler2D(heap_textures[i], heap_samplers[s])`.
#include "heap_slots.glsl"

// One entry of the material table; layout matches material_record in vulkan/primitive.cppm
// (std430, 80 bytes). Field order and the flag bits are a CPU/GPU contract - see register_material.
// THE MATERIAL RECORD'S LAYOUT LIVES IN ITS OWN FILE, because a FULLSCREEN stage cannot include this one
// (the shared scene push block below is 144 bytes, over the 128-byte heap limit, so only a geometry stage
// can declare it) and a fullscreen stage may still need the material table. See material_record.glsl.
#include "material_record.glsl"
// The ARRAY name carries the HEAP slot and the block member carries the record index: two index spaces, which is
// why a lookup is `heap_material_tables[heap_slots_materials].materials[push.material_index]`.
#ifndef VR_SLANG
layout(descriptor_heap, descriptor_stride = heap_slot_stride) readonly buffer Materials { Material materials[]; } heap_material_tables[];
#endif // the Slang side reaches the table through material_at(): a GLSL block member does not exist there

/**
 * @brief one fetch from the bindless array, with the sampler the descriptor-set path used to bind for it
 * @param texture_index the texture's own index (its slot is heap_slots_textures + this)
 * @param uv the coordinate
 * @note THE TWO HALVES OF A FETCH ARE SEPARATE IN A HEAP: the image comes from the resource heap at its slot, the
 *       sampler from the SAMPLER heap at heap_sampler_texture (linear, repeat, mips - exactly what the set path
 *       bound), and a combined image sampler cannot be declared at all, so every fetch says both.
 */
#ifndef VR_SLANG
vec4 heap_sample(uint texture_index, vec2 uv) {
    return texture(sampler2D(heap_textures[heap_slots_textures + texture_index], heap_samplers[heap_sampler_texture]), uv);
}
#endif // the Slang side defines heap_sample in shaders/heap_access.slang, where a combined sampler is a
       // DescriptorHandle<Sampler2D> whose .x names the texture slot and .y the sampler slot

// Push constant block: must mirror the vertex stages and material_push_constants in the runtime
// (eight uint fields first, then the aligned mat4) so member offsets agree across stages and with
// the CPU writes. A fragment stage typically reads material_index and flags only; the other fields
// exist to keep the block layout identical.
layout(push_constant) uniform PushConstants {
    uint material_index; // index into the material table (material data lives on the GPU)
    uint flags;          // bit0: instanced draw -> model from instances[...]; bit3: double-sided
    uint skin_base;      // start of this primitive's joint block in skins.matrices (0 = identity)
    uint morph_base;     // float index of this primitive's morph block in morph_data.morphs (0 = none)
    uint morph_targets;  // number of morph targets (0 = not morphable)
    uint morph_vertices; // vertex count of this primitive (morph block stride)
    uint instance_base;  // mat4 start of this instanced primitive's transforms (unused here)
    // Declared rather than left as implicit padding, because the VERTEX stage reads it: pbr.vert turns
    // push.motion_base into this draw's index into the previous-frame world matrices, which is where the
    // rigid half of a motion vector comes from. It sits in the 4 bytes std430 leaves between instance_base
    // and the 16-aligned `model`, so declaring it moves NO offset in any stage.
    uint motion_base;    // start of this draw's previous-frame world matrices (pbr.vert: binding 13)
    // VR_MAT4, not `mat4`, and that matters now that a VERTEX stage reads this member: Slang's default
    // majorness is ROW-major, so a plain `mat4` here would compile to `RowMajor` and read the host's
    // column-major glm::mat4 transposed. VR_MAT4 is `mat4` in GLSL (unchanged) and `row_major float4x4` in
    // Slang, which is the spelling that emits `ColMajor`. The fragment stages never read `model`, which is
    // why this stayed harmless until the vertex port - the same trap as docs/slang_migration.md section 6.
    VR_MAT4 model;       // per-model world transform (kept out of the shared camera UBO; unused here)
    // THE HEAP INDICES (see heap_slots.glsl), shared by every stage that includes this file: which frame slot and
    // which swapchain image it runs for. LAST on purpose, so every field above keeps its offset, and delivered
    // through vkCmdPushDataEXT because a heap pipeline has no layout to hold push constants.
    uint frame_slot;
    uint image_index;
    // THE ENDPOINT'S THIRD LANE, declared though no scene stage reads it: the renderer's push endpoint sends
    // three lanes (frame slot, swapchain image, and the post chain's source slot - see
    // render_environment::push_block), and declaring all three is what makes the block CONTIGUOUS, which the
    // geometry lanes below need. With a descriptor-heap pipeline every byte of the declared block must have
    // been written by vkCmdPushDataEXT before the draw (VUID-...-11376), so a member that is pushed but not
    // declared, or declared but skipped, is a validation error rather than a wasted word.
    uint spare_lane;
    // ... AND THE LANES' OWN ALIGNMENT WORD, declared so their offset is a CONTRACT rather than a consequence:
    // `MeshGeometryLanes` is a struct, and Slang lays a std140 struct member out on a 16-byte boundary - which
    // put it at 112 (not at the 108 the members before it end at) and left a 4-byte hole that made every fetch
    // read another draw's window. Declaring this word makes 112 the end of a real member, so the layout survives
    // a change of layout rules; the host pushes it as zero (see primitive::mesh_geometry_push_offset_scene).
    uint geometry_pad;
    // ... and the geometry lanes at the offset the host pushes them at (see the file's header and
    // primitive::mesh_geometry_lanes_offset): the window of vertex and index data this draw covers, which a
    // MESH entry has no input assembler to take it from. The VERTEX and FRAGMENT entries declare them too -
    // one source file is one block layout for every entry - and the host fills them for every draw, so the
    // vertex path pays the same bytes and simply never reads them.
    MeshGeometryLanes geometry;
} push;

// ... and the two names the shared slot macros use (heap_slots.glsl says why these are macros and not constants).
#define heap_frame_slot (push.frame_slot)
#define heap_image_index (push.image_index)

/**
 * @brief everything the lighting code needs to know about one surface point
 * @note the names mirror the glTF metallic-roughness material model so the fields map 1:1 onto the
 *       spec (albedo = baseColor, ao = occlusion, ...)
 */
/**
 * @brief the fragment's TANGENT frame, from the screen-space derivatives of `world_pos` and `uv`
 *
 * A FUNCTION RATHER THAN A BLOCK INSIDE `gather_surface`, and the reason is a SECOND CALLER rather than tidiness:
 * the normal-map path needs the frame only when the material HAS a normal map (which no character material in this
 * repository does), while the toon stage needs one for a material whose hair normal is packed beside the model's
 * own in the game's split-normal texture. A second copy of this arithmetic would be the second implementation this
 * file's neighbours keep refusing, and a caller that needs no frame does not pay for one.
 *
 * @param world_pos the surface point whose screen-space derivatives are one half of the frame
 * @param uv the surface point's texture coordinates, the other half of the same derivative pair
 * @param tangent receives the world-space tangent direction (normalised)
 * @param bitangent receives the world-space bitangent direction (normalised)
 * @return FALSE when the UV derivatives are degenerate - a zero-area triangle or a UV seam, where no frame exists.
 *         The caller must then fall back to the geometric normal: a NaN frame would silently poison the normal it
 *         is multiplied into, and the two directions written above are a defined answer rather than a guess.
 * @note the frame's Z axis is the unit geometric normal the caller already has, and that normal is also the
 *       caller's fallback; this function writes the two axes above and nothing else.
 */
bool surface_tbn(vec3 world_pos, vec2 uv, out vec3 tangent, out vec3 bitangent) {
    const vec3 dp1 = dFdx(world_pos);
    const vec3 dp2 = dFdy(world_pos);
    const vec2 duv1 = dFdx(uv);
    const vec2 duv2 = dFdy(uv);
    const float denom = duv1.x * duv2.y - duv2.x * duv1.y;
    if (abs(denom) < 1e-8) {
        tangent = vec3(1.0, 0.0, 0.0);
        bitangent = vec3(0.0, 1.0, 0.0);
        return false;
    }
    tangent = normalize((duv2.y * dp1 - duv1.y * dp2) / denom);
    bitangent = normalize((duv1.x * dp2 - duv2.x * dp1) / denom);
    return true;
}

struct surface_sample {
    vec3 albedo;    // base color: base_color_factor * albedo texture (linear, NOT premultiplied)
    float alpha;    // coverage: base_color_factor.a * albedo.a (only meaningful for MASK/BLEND)
    vec3 normal;    // shading normal in world space: normal-mapped, double-sided flipped
    vec3 emissive;  // emissive_factor * emissive texture (linear HDR-ish radiance)
    float roughness; // roughness_factor * metallic-roughness texture .g
    float metallic;  // metallic_factor * metallic-roughness texture .b
    float ao;        // mix(1, occlusion texture .r, occlusion_strength)
    uint flags;      // the material record's flag bits (see Material)
    uint toon_family; // the toon material family (see Material::toon_family); 0 == none
    // THE TOON TEXTURE LANES (see Material::toon_indices): x = diffuse ramp, y = shadow LUT, z = specular
    // ramp, w = matcap. ELEMENT 0 IS "DO NOT READ" - it is the white fallback, and a stage that read it as a
    // ramp would tint from a constant rather than from a no-op.
    uvec4 toon_indices;
};

/**
 * @brief gather the surface properties of one fragment from the material table and the texture array
 * @param world_pos interpolated world position (needed for the screen-space TBN derivatives)
 * @param geo_normal interpolated geometric normal (used when the material has no normal map)
 * @param uv interpolated texture coordinates
 * @return the surface sample of this fragment (never for a discarded fragment: see below)
 * @note CALLS @c discard FOR alphaMode MASK MATERIALS whose alpha is below the record's
 *       alphaCutoff. That is the glTF rule and it belongs here rather than at the call sites: every
 *       pass that shades a surface (forward, G-buffer) must cut the same fragments out, or the
 *       shadow/deferred/forward views of one object disagree. The caller sees a "clean" surface.
 * @note the TBN frame comes from screen-space derivatives of the world position and the UVs, so
 *       mirrored UV layouts (glTF TANGENT.w = -1) are handled implicitly and the vertex TANGENT
 *       attribute is never consumed; a degenerate UV derivative falls back to the fine normal.
 */
surface_sample gather_surface(vec3 world_pos, vec3 geo_normal, vec2 uv) {
    Material mat = material_at(heap_slots_materials, push.material_index);

    surface_sample s;
    const vec4 base_color = mat.base_color_factor * heap_sample(mat.tex_indices.x, uv);
    // alphaMode MASK (record flag bit4): discard fragments below the cutoff (base_color.a is
    // factor.a * albedo.a) - glTF alphaCutoff semantics
    if ((mat.flags & 16u) != 0u && base_color.a < mat.alpha_cutoff) {
        discard;
    }

    s.albedo = base_color.rgb;
    s.alpha = base_color.a;
    s.metallic = mat.metallic_factor * heap_sample(mat.tex_indices.y, uv).b;
    s.roughness = mat.roughness_factor * heap_sample(mat.tex_indices.y, uv).g;
    // occlusion: sampled AO modulated by occlusion_strength; without an occlusion map the slot is
    // the white fallback (ao = 1) and the strength has no effect
    s.ao = mix(1.0, heap_sample(mat.tex_indices.w, uv).r, mat.occlusion_strength);
    s.emissive = mat.emissive_factor.rgb * heap_sample(mat.emissive_index, uv).rgb;
    s.flags = mat.flags;
    s.toon_family = mat.toon_family;
    s.toon_indices = mat.toon_indices;

    // ---- normal: optional tangent-space normal map, else the interpolated normal ----
    if ((mat.flags & 1u) != 0u) {
        const vec3 normal = normalize(geo_normal);
        vec3 sdir = vec3(1.0, 0.0, 0.0);   // world tangent direction (see surface_tbn)
        vec3 tdir = vec3(0.0, 1.0, 0.0);   // world bitangent direction
        if (!surface_tbn(world_pos, uv, sdir, tdir)) {
            s.normal = normal; // degenerate UV derivatives: fall back to the interpolated normal
        } else {
            vec3 tbn_normal = heap_sample(mat.tex_indices.z, uv).rgb * 2.0 - 1.0;
            tbn_normal.xy *= mat.normal_scale;
            tbn_normal = normalize(tbn_normal);
            s.normal = normalize(mat3(sdir, tdir, normal) * tbn_normal);
        }
    } else {
        s.normal = normalize(geo_normal);
    }
    // double-sided material (record flag bit3): mirror the normal on back faces, as the glTF spec
    // requires, so the inner side of a shell is lit by its inward-facing normal
    if ((mat.flags & 8u) != 0u && !gl_FrontFacing) {
        s.normal = -s.normal;
    }
    return s;
}

#endif // DEREN_SURFACE_GLSL
