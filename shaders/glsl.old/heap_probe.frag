// The FRAGMENT half of the graphics probe: it reads the material table out of the heap and writes the DEFAULT
// material's base colour, so the host can read a pixel back and compare it with a value it knows in advance
// (white, because element 0 is the material this renderer reserves). Everything about the mechanism is the same
// as the compute probe's read - `descriptor_heap` declarations, the grid's 64 B stride, the heap bound in the
// command buffer - and the difference being tested is that a GRAPHICS pipeline can be created that way at all
// (heap flag, null layout) and that a fragment stage reads the heap the same as a compute stage does.
#version 460
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : enable

#include "heap_slots.glsl"

layout(location = 0) in vec2 uv;

layout(location = 0) out vec4 colour;

// The same record heap_probe.comp and mask_bake.comp declare: it must stay byte-identical to material_record in
// vulkan/primitive.cppm, which is a CPU/GPU contract rather than a convenience.
struct ProbeMaterial {
    uvec4 tex_indices;
    uint emissive_index;
    float alpha_cutoff;
    float occlusion_strength;
    uint _pad;
    vec4 base_color_factor;
    vec4 emissive_factor;
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    uint flags;
};
layout(descriptor_heap, descriptor_stride = heap_slot_stride) readonly buffer ProbeMaterials { ProbeMaterial materials[]; } heap_material_tables[];

// The slot is a PARAMETER rather than a constant, and that is the point: the host pushes it, so the probe can be
// run a second time with a deliberately WRONG slot - a probe that can only say "fine" would pass every check.
// With no pipeline layout (the flag's requirement) this block arrives through vkCmdPushDataEXT.
layout(push_constant) uniform HeapProbePush {
    uint material_slot; // ABSOLUTE grid slot of the material table, e.g. heap_slots_materials
} pc;

void main() {
    // The uv is unused on purpose: the value is a constant of the heap's contents, not of the fragment's position,
    // so the whole 4x4 target must come back the same colour - which is what makes a single pixel a valid probe.
    colour = vec4(heap_material_tables[pc.material_slot].materials[0].base_color_factor.rgb, 1.0);
}
