#version 450
#extension GL_EXT_nonuniform_qualifier : enable

/**
 * @file shaders/gbuffer.frag
 * @brief G-buffer write stage: stores the surface of an opaque fragment instead of shading it.
 * @ingroup shaders
 *
 * The deferred path's first half. Instead of evaluating lights (what pbr.frag does), this stage
 * only asks what the fragment is made of and writes that into the G-buffer targets, so the lighting
 * stage can shade every pixel once, from screen space, and screen-space effects (SSAO, SSR, TAA) get
 * the data they need. Both paths gather that surface through the same shaders/surface.glsl, so the
 * deferred and forward views of one object cannot drift apart.
 *
 * Targets (attachment order = core::gbuffer_formats + core::gbuffer_velocity_format, and the location
 * order below):
 * - location 0 (RGBA8_UNORM): albedo.rgb + metallic - base color is linear and stored unencoded, and
 *   8-bit metallic is plenty for a metallic-roughness workflow (glTF allows 8-bit inputs);
 * - location 1 (RGBA16F): world normal.xyz + roughness - the normal is stored as three floats rather
 *   than octahedral-encoded: 4 bytes per pixel buys the absence of a whole class of precision bugs
 *   while the layout is young;
 * - location 2 (RGBA8_UNORM): material_id low byte + high byte + ambient occlusion + material flags.
 *   The id is 16 bits split over two 8-bit channels (each an exact k/255 value, so it round-trips
 *   exactly through the UNORM target and a NEAREST fetch): the lighting stage needs it to look up the
 *   material record (emissive, shading model, flags) without the G-buffer carrying textures of its own;
 * - location 3 (RG16F): the MOTION VECTOR in UV space (current - previous) that the TAA resolve
 *   reprojects the history with;
 * - location 4 (RGBA16F): the EMISSIVE term, ADDED into the scene color (the attachment blends with
 *   ONE/ONE). This is the one lighting-independent part of the shading, and it lives here because it
 *   needs the emissive texture and the UVs, which no G-buffer target stores - see deferred.frag, which
 *   deliberately does not add it a second time.
 *
 * Depth is written by this pass (the pipeline's own single-sampled depth image), so the lighting
 * stage reconstructs the world position from it instead of storing one.
 *
 * @note the five color targets are exactly core::gbuffer_pass_attachment_count (three surface targets +
 *       velocity + scene color); a missing output here leaves its attachment at the value the blend
 *       equation produces from the fragment's default (0,0,0,1), which for an additive blend is a
 *       silent no-op rather than an error.
 * @note alphaMode BLEND materials never reach this pass: the runtime keeps them in the forward
 *       transparent pass (a G-buffer cannot blend a surface into existence). alphaMode MASK
 *       materials DO render here and gather_surface() discards their cut-out texels.
 * @note skybox / background pixels keep the cleared G-buffer values (all zero) - material id 0 is
 *       the reserved default material, and the lighting stage treats a far-plane depth as "no
 *       geometry" (those pixels are lit as sky).
 */

// surface.glsl's declarations are heap-native (see pbr.frag for why this stage has to ask for the extensions).
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : enable

#include "surface.glsl"
// ... and the scene state for the motion vector's two matrices. Only the camera UBO is used here;
// the lighting declarations this include also carries (light UBO, shadow map, the BRDF functions)
// compile away unused, and their descriptors are part of the shared scene set either way.
#include "shading.glsl"

layout(location = 0) in vec3 v_world_pos;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec2 v_uv;
layout(location = 3) in vec3 v_prev_world_pos;

layout(location = 0) out vec4 out_albedo_metallic;  // rgb albedo, a metallic
layout(location = 1) out vec4 out_normal_roughness; // xyz world normal, w roughness
layout(location = 2) out vec4 out_material;         // r/g material id, b ao, a flags
layout(location = 3) out vec2 out_velocity;         // motion vector in UV space (current - previous)
layout(location = 4) out vec4 out_scene_color;      // the EMISSIVE term, ADDED into the scene color

/**
 * @brief motion vector of this fragment in UV space: where it was last frame, relative to here
 * @param world_pos the fragment's world position (this frame)
 * @param prev_world_pos where the same vertex was last frame, i.e. through the PREVIOUS frame's
 *        world matrix (computed by the vertex stage from the scene set's previous-transform block,
 *        binding 13)
 * @return (current_uv - previous_uv), so the TAA resolve samples the history at `uv - velocity`
 *
 * Both matrices come from the camera UBO and are deliberately the JITTER-FREE pair: `proj` carries
 * the TAA jitter, and a jitter that leaked in here would be read as camera motion - the history
 * would be reprojected by up to a pixel every frame, which is exactly the aliasing TAA removes.
 * The previous position is projected with `prev_view_proj` alone, because the object's own motion
 * is already baked into it; the jitter-free pairing still holds on both sides.
 *
 * @note This covers CAMERA motion, RIGID object motion (a moving/rotating node, an instance) and a
 *       DEFORMING mesh: `pbr.vert` computes the previous local position through the joint matrices as
 *       they were one frame ago AND through the morph weights as they were one frame ago, so a vertex
 *       that moved inside its own object space reports that movement instead of zero.
 *       NOT COVERED: alpha-blended geometry, which is composited outside the G-buffer and writes no
 *       velocity at all (see docs/deformation_motion_vectors.md).
 */
vec2 motion_vector(vec3 world_pos, vec3 prev_world_pos) {
    const vec4 current_clip = camera[heap_camera_slot].view_proj_unjittered * vec4(world_pos, 1.0);
    const vec4 previous_clip = camera[heap_camera_slot].prev_view_proj * vec4(prev_world_pos, 1.0);
    const vec2 current_uv = (current_clip.xy / current_clip.w) * 0.5 + 0.5;
    const vec2 previous_uv = (previous_clip.xy / previous_clip.w) * 0.5 + 0.5;
    return current_uv - previous_uv;
}

/**
 * @brief write the fragment's surface into the G-buffer targets
 * @note no lighting, no tonemapping, no output alpha semantics: everything the surface carries that
 *       is not lighting-independent is stored as-is, and the lighting stage decides what to do with it
 */
void main() {
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv);

    // the world normal needs no encoding in an RGBA16F target (see the file docs)
    out_albedo_metallic = vec4(s.albedo, s.metallic);
    out_normal_roughness = vec4(s.normal, s.roughness);

    // 16-bit material id split over two UNORM channels: k / 255 is exactly representable, so
    // round(255 * stored) returns the byte that was written even after the format's conversion
    const uint id = push.material_index;
    out_material = vec4(
        float(id & 0xFFu) / 255.0,
        float((id >> 8u) & 0xFFu) / 255.0,
        s.ao,
        float(s.flags & 0xFFu) / 255.0);

    out_velocity = motion_vector(v_world_pos, v_prev_world_pos);

    // The emissive term goes STRAIGHT into the scene color, which is why this pass owns it: emissive
    // is lighting-independent (no light, shadow or BRDF enters it), and it needs the material's
    // emissive texture and this fragment's UVs - neither of which the G-buffer stores, so the lighting
    // stage could not reconstruct it afterwards. deferred.frag documents the same split from its side
    // and deliberately sets its own shade_input.emissive to zero rather than adding it twice.
    //
    // out_scene_color's attachment blends with ONE/ONE (make_color_blend_attachment_additive), so this
    // is an accumulation on top of the sky the earlier passes left there, not an overwrite. The alpha
    // lane follows the pipeline's srcAlphaBlendFactor = ZERO and contributes nothing.
    out_scene_color = vec4(s.emissive, 1.0);
}
