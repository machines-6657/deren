#version 450
#extension GL_EXT_nonuniform_qualifier : enable

/**
 * @file shaders/pbr.frag
 * @brief Forward PBR fragment stage: metallic-roughness direct lighting, IBL, shadows, cel shading.
 * @ingroup shaders
 *
 * The forward path's shading half is thin on purpose: the surface comes from shaders/surface.glsl
 * (material table, alphaMode tests, normal mapping, the texture factors) and the lighting from
 * shaders/shading.glsl (sun + shadows, punctual lights, split-sum IBL, the selectable BRDF/diffuse
 * presets, cel-shading bands). This stage only wires the two together and applies the glTF output
 * alpha rule - which is what makes the deferred path (gbuffer.frag + deferred.frag) an exact A/B
 * reference rather than a lookalike: both paths call the same gather and the same shade function.
 *
 * Output is linear HDR radiance into the frame's HDR target; exposure, ACES tonemapping and display
 * encoding all belong to post.frag, so this stage never applies gamma.
 *
 * The per-object state (which pipeline, which material) arrives through the shared scene descriptor
 * set and the material push block - see docs/shaders.md.
 */

// surface.glsl's declarations are heap-native: a `descriptor_heap` declaration compiles to an untyped pointer, so
// this stage has to ask for that extension, and for the nonuniform qualifier a variable index into one needs.
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : enable

#include "surface.glsl"
#include "shading.glsl"

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

/**
 * @brief shade one fragment: gather the surface, light it, apply the glTF output alpha
 *
 * glTF alpha semantics: only alphaMode BLEND materials carry real coverage in the output alpha.
 * OPAQUE and MASK outputs must write alpha = 1 (their base color alpha is ignored by the spec),
 * otherwise the always-on blending below would make e.g. an albedo texture with an alpha channel
 * unexpectedly translucent. BLEND keeps base_color.a.
 */
void main() {
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv);

    shade_input si;
    si.pixel = ivec2(gl_FragCoord.xy); // the cluster grid's tile coordinate (see shade_input)
    si.world_pos = v_world_pos;
    si.normal = s.normal;
    si.albedo = s.albedo;
    si.emissive = s.emissive;
    si.metallic = s.metallic;
    si.roughness = s.roughness;
    si.ao = s.ao;
    // The forward path adds the diffuse ambient itself.

    const float out_alpha = ((s.flags & 32u) != 0u) ? s.alpha : 1.0;
    out_color = vec4(shade_surface(si), out_alpha);
}
