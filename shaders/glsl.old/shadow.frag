#version 450
// the albedo texture is picked by the material record's index (bindless array), like pbr.frag
#extension GL_EXT_nonuniform_qualifier : enable

/**
 * @file shaders/shadow.frag
 * @brief Depth-only shadow fragment shader plus the glTF alphaMode MASK test.
 * @ingroup shaders
 *
 * The pass has no color attachment: rasterization depth is all it writes, and the shader declares no
 * output. What it DOES do is discard below the material's alpha cutoff, so a masked caster (foliage,
 * curtains, grates) casts a cut-out shadow. Before this, the pass skipped masked leaves entirely -
 * the sun poured straight through a Sponza curtain - and casting a SOLID shadow would be just as
 * wrong.
 *
 * The test mirrors pbr.frag's, on the same texture array and the same material record, so the
 * shaded alpha edge and the shadow edge always agree. Opaque materials take the early-out below:
 * their record has flag bit4 clear, so the texture fetch never executes (one uniform branch per
 * draw, decided by the per-draw material_index push constant).
 *
 * The push-constant block must stay byte-identical to shadow.vert's: one pipeline shares a single
 * push-constant range across both stages.
 */

// One entry of the material table; layout matches material_record in vulkan/model.cppm (std430)
struct Material {
    uvec4 tex_indices; // albedo, metallic-roughness, normal, occlusion (indices into textures[])
    uint emissive_index;
    float alpha_cutoff;       // alphaMode MASK threshold
    float occlusion_strength; // mix(1, sampled AO, strength)
    uint _pad;
    vec4 base_color_factor;
    vec4 emissive_factor;
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    uint flags; // bit0: normal map, bit1: occlusion map, bit2: emissive map, bit3: double-sided, bit4: alphaMode MASK, bit5: alphaMode BLEND
};
// HEAP-NATIVE (see unlit.frag for the shape): the bindless array is the shared `heap_textures`, the table is an
// array of blocks whose ARRAY name carries the heap slot, and the extensions are per stage.
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : enable
#include "heap_slots.glsl"
layout(descriptor_heap, descriptor_stride = heap_slot_stride) readonly buffer Materials { Material materials[]; } heap_material_tables[];

layout(push_constant) uniform PushConstants {
    uint material_index; // index into the material table (the mask test's only input)
    uint flags;          // unused here: declared to keep the block layout identical to shadow.vert
    uint skin_base;
    uint morph_base;
    uint morph_targets;
    uint morph_vertices;
    uint instance_base;
    mat4 model;
} push;

layout(location = 0) in vec2 v_uv; // albedo UV from shadow.vert (only the mask test reads it)

/**
 * @brief the alphaMode MASK test: discard below the material's alpha cutoff
 *
 * Cost is one uniform branch per draw plus one texture fetch for the masked materials only: the
 * material record's flag bit4 is a push-constant-decided value, so opaque casters never sample the
 * albedo texture at all.
 */
void main() {
    Material mat = heap_material_tables[heap_slots_materials].materials[push.material_index];
    if ((mat.flags & 16u) == 0u) {
        return; // OPAQUE / BLEND: nothing to test, so the albedo texture is never sampled
    }
    // same expression as pbr.frag: base_color.a is factor.a * albedo.a
    if (mat.base_color_factor.a * heap_texel(heap_textures[heap_slots_textures + mat.tex_indices.x], heap_sampler_texture, v_uv).a < mat.alpha_cutoff) {
        discard;
    }
}
