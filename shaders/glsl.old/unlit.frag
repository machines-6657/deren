#version 450
#extension GL_EXT_nonuniform_qualifier : enable

/**
 * @file shaders/unlit.frag
 * @brief Non-PBR "unlit" fragment stage: the material's base color, flat, no lighting.
 * @ingroup shaders
 *
 * Draws the material's base color (factor x albedo texture) with no direct light, shadows, IBL or
 * emissive - a shading-free view of the geometry. Useful for assets whose authored data is minimal
 * (e.g. normal-less skinned stress models) and as a reference against the lit pass. The runtime
 * switches to it through the render mode combo (runtime::set_default_pipeline), which re-shades every
 * default-semantics leaf without re-baking anything - the vertex stage is still pbr.vert, so the
 * geometry path is identical.
 *
 * Alpha semantics match pbr.frag: MASK fragments discard below alpha_cutoff, BLEND materials output
 * their real coverage so the always-on blending composites them correctly, OPAQUE writes alpha 1.
 */

layout(location = 2) in vec2 v_uv; // pbr.vert also outputs v_world_pos / v_normal; not consumed here

layout(location = 0) out vec4 out_color;

// Single flat scene set (same as pbr.frag): this pass only needs the runtime texture array
// (binding 1) and the GPU material table (binding 5); the other bindings are unused.
// HEAP-NATIVE (see docs/descriptor_heap_migration.md): the bindless array is the shared `heap_textures` (declared
// by the grid header) and the table is an array of blocks whose ARRAY name carries the heap slot, while the block
// MEMBER carries the record index. The extensions are required in every stage that declares such a resource.
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : enable
#include "heap_slots.glsl"

// One entry of the material table; layout matches material_record (std430, 80 bytes) - must
// stay identical to pbr.frag's Material so both stages read the same record.
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
layout(descriptor_heap, descriptor_stride = heap_slot_stride) readonly buffer Materials { Material materials[]; } heap_material_tables[];

// Push constant block: identical layout to pbr.vert / pbr.frag (only material_index is read).
layout(push_constant) uniform PushConstants {
    uint material_index;
    uint flags;
    uint skin_base;
    uint morph_base;
    uint morph_targets;
    uint morph_vertices;
    uint instance_base;
    mat4 model;
} push;

// Non-PBR "unlit" pass: draws the material's base color (factor x albedo texture) flat, with
// no direct light, shadows, IBL or emissive - a shading-free view of the geometry (useful for
// assets whose authored data is minimal, e.g. normal-less skinned stress models, and as a
// reference against the lit pass). Alpha semantics match pbr.frag: MASK fragments discard
// below alpha_cutoff, BLEND materials output their real coverage so the always-on blending
// composites them correctly, OPAQUE writes alpha 1.
/**
 * @brief write the base color (factor x albedo) flat, with glTF alpha semantics
 *
 * The push-constant block and the Material struct are mirrored from pbr.frag, so both fragment
 * stages read the same GPU records; only material_index is used here.
 */
void main() {
    Material mat = heap_material_tables[heap_slots_materials].materials[push.material_index];
    vec4 base_color = mat.base_color_factor * heap_texel(heap_textures[heap_slots_textures + mat.tex_indices.x], heap_sampler_texture, v_uv);
    if ((mat.flags & 16u) != 0u && base_color.a < mat.alpha_cutoff) {
        discard;
    }
    float out_alpha = ((mat.flags & 32u) != 0u) ? base_color.a : 1.0;
    out_color = vec4(base_color.rgb, out_alpha);
}
