#version 450
#extension GL_EXT_nonuniform_qualifier : enable

/**
 * @file shaders/deferred.frag
 * @brief Deferred lighting: shade every pixel once, from the G-buffer, in screen space.
 * @ingroup shaders
 *
 * The second half of the deferred path. It reads the three G-buffer targets plus the pass's 1x depth
 * image, rebuilds the world position from that depth, and evaluates the same lighting the forward
 * path evaluates per fragment - literally the same code: shaders/shading.glsl owns the sun, the
 * shadow test, the punctual lights, the split-sum IBL and the BRDF presets, and this stage only
 * supplies a shade_input assembled from G-buffer texels instead of from interpolated vertex data.
 * That is what makes the forward and the deferred image comparable rather than merely similar.
 *
 * It also owns the screen-space ambient occlusion (M6): the occlusion is computed from the very depth
 * and normals this stage already reads, folded into the shade_input's ao, and therefore scales the
 * IBL ambient exactly like a material's baked AO map does - no extra render target, no extra pass.
 *
 * Inputs (the G-buffer images, each a heap image array indexed by the frame's slot):
 * - binding 0: albedo.rgb + metallic (RGBA8)
 * - binding 1: world normal.xyz + roughness (RGBA16F)
 * - binding 2: material id low/high byte + ambient occlusion + material flags (RGBA8)
 * - binding 3: the G-buffer pass's depth (D32_SFLOAT, sampled)
 *
 * Push constant: the inverse view-projection, which turns (uv, depth) back into a world position, and
 * the SSAO parameters. Reconstructing the position instead of storing it costs one 4x4 multiply per
 * pixel and saves 8-12 bytes per pixel of G-buffer memory; the depth is exact, so the reconstruction
 * is exact too.
 *
 * Output: LINEAR HDR radiance, added to the HDR target (the pipeline blends with ONE/ONE). What is
 * already in that target when this pass runs:
 * - the emissive term, which the G-buffer pass added additively (gbuffer.frag): emissive is
 *   lighting-independent, so it does not belong to this stage's work, and
 * - nothing else - the HDR target is cleared (to zero) before the G-buffer pass, so a pixel with no
 *   geometry holds exactly 0. This stage is therefore free to write the SKY there, which is why the
 *   deferred path draws no background pass at all: a background is not a surface and has no place in
 *   a G-buffer. The sky evaluation is the same function the forward skybox pass uses
 *   (shaders/sky.glsl), so both paths produce the same background from the same code.
 */

// The two names the shared slot macros use. They are textual, so they may be defined here while the block they
// name comes later - but shading.glsl, WHICH USES THEM, must be included after that block (see it below).
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

// shading.glsl's declarations are heap-native: this stage asks for the two extensions they need (see pbr.frag).
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : enable
// THE GRID HEADER, early: this stage's OWN declarations below are heap-native and need its constants, while
// shading.glsl (which also carries it) is included only after the push block. The header has an include guard, so
// the second inclusion costs nothing.
#include "heap_slots.glsl"

#include "sky.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// HEAP-NATIVE (see docs/descriptor_heap_migration.md): a heap image descriptor is an IMAGE, so every read
// constructs its sampler from the sampler heap - and the sampler for all five is the G-buffer's NEAREST one, which
// is what these exact texel fetches want (the declaration this replaces said so in its own comment). The G-buffer,
// the scene colour and the lighting image are per SWAPCHAIN IMAGE; the ray-traced visibility is per FRAME SLOT.
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D gbuffer_albedo_texture[];   // RGBA8: albedo.rgb + metallic
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D gbuffer_normal_texture[];   // RGBA16F: normal.xyz + roughness
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D gbuffer_material_texture[]; // RGBA8: id lo/hi + ao + flags
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D gbuffer_depth_texture[];
/// @brief one G-buffer texel AT @p slot, through the set's NEAREST sampler
/// @param slot the heap slot of the image, already per-image (heap_slots_gbuffer_x + heap_image_index)
/// @note THE PARAMETER IS A SLOT, NOT A texture2D, and that is measured rather than stylistic: a texture2D
///       parameter does not survive a function boundary - glslang emits a call whose argument type does not match
///       the callee's parameter type, and spirv-val then rejects the module at vkCreateShaderModule
///       (VUID-VkShaderModuleCreateInfo-pCode-08737). Indexing ONE of the declared arrays inside is sound
///       because every heap array view is a view of the SAME resource heap: the slot chooses the image and the
///       name does not.
vec4 gbuffer_texel(uint slot, vec2 uv) {
    return texture(sampler2D(gbuffer_albedo_texture[slot], heap_samplers[heap_sampler_gbuffer]), uv);
}
// set 0 (the shared scene set, per frame slot): the ray-traced sun visibility this stage multiplies the
// sun term by when the light UBO says so. Written by the ray-traced shadow pass, which runs between the
// G-buffer pass and this one.
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D rt_visibility_texture[];
// The STOCHASTIC PUNCTUAL LIGHTING image (docs/megalights.md): half resolution, per swapchain image, added by this
// stage instead of by its own composite pass. Read at exact texels too (the 2x2 gather below), so the same NEAREST
// sampler serves it.
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D ml_lighting_texture[];

layout(push_constant) uniform DeferredPush {
    mat4 inv_view_proj; // clip (NDC xyz, w = 1) -> world position
    // Screen-space ambient occlusion (M6): x = world-space radius, y = intensity (0 = off),
    // z = sample count, w = depth bias. The runtime fills it per frame; an intensity <= 0 makes the
    // term return exactly 1.0, which is what keeps an SSAO-off frame bit for bit the pre-M6 frame.
    vec4 ssao;
    // 1.0 = "unlit" render mode: write the stored albedo, unshaded (see runtime::set_unlit)
    float unlit;
    // 1.0 = the stochastic punctual lighting pass ANSWERED this frame, so the cluster loop below must not add
    // the punctual lights a second time (and this stage adds `ml_lighting` instead, see the end of main). It is
    // set from whether that pass actually recorded, not from its knob - a frame whose pass was gated off keeps
    // the raster loop, which is what makes the two paths exclusive rather than complementary.
    float punctual_replaced;
    // THE HEAP INDICES (see heap_slots.glsl): which swapchain image this stage runs for. LAST on purpose, so every
    // field above keeps its offset; the host sends them through vkCmdPushDataEXT.
    uint frame_slot;
    uint image_index;
} pc;

// THE SCENE-SET HEADER, INCLUDED ONLY NOW: its declarations are heap-native and its functions address the per-frame
// arrays through the slot macros above, which expand to `pc.frame_slot` - so `pc` has to exist first. That is the
// one ordering rule this pattern has.
#include "shading.glsl"

/**
 * @brief rebuild the world-space position of the pixel from the stored depth
 * @param uv the pixel's texture coordinate (v_uv)
 * @param depth the G-buffer depth in [0,1]
 * @return world-space position
 * @note the engine's projection is a RH_ZO matrix with its Y row flipped for Vulkan
 *       (make_orbit_camera_ubo), and post.vert's v_uv already maps (0,0) to the top-left texel, so
 *       the NDC of a pixel is (2*uv - 1) in both axes: no extra flip belongs here - the inverse
 *       matrix already carries the projection's own conventions.
 */
vec3 world_position_from_depth(vec2 uv, float depth) {
    const vec4 world = pc.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

/// @brief AO sample budget: the shader loop bound (the runtime's ssao_samples is clamped to it)
const int MAX_SSAO_SAMPLES = 16;

/**
 * @brief screen-space ambient occlusion of the pixel (hemisphere samples against the depth buffer)
 * @param uv the pixel's texture coordinate (v_uv)
 * @param depth the G-buffer depth at @p uv
 * @param world_normal the G-buffer world normal at @p uv
 * @return 1.0 = fully unoccluded (also when SSAO is off), 0.0 = fully occluded
 *
 * The classic screen-space AO: a hemisphere of sample points around the pixel's view-space position,
 * each projected back to screen and compared against the depth buffer. A sample counts as occluded
 * when the surface stored at its screen position is CLOSER to the camera than the sample point and
 * inside the radius - view-space z is negative in front of the camera, so "closer" means a greater z.
 * The kernel is a golden-angle hemisphere SPIRAL (uniform in the projected disc, no literal table)
 * rotated per pixel by a hash of gl_FragCoord: a fixed kernel would show the sample pattern as
 * spiral banding.
 *
 * This is deliberately the affordable end of screen-space AO. It is not a horizon-based GTAO (which
 * estimates the true horizon angle per slice and is the modern engine default) and it carries the
 * usual screen-space limitations: geometry off screen occludes nothing, and the radius is in world
 * units, so its apparent strength depends on the view distance. It scales the IBL ambient only -
 * `shade_surface()` applies `s.ao` to the diffuse and specular ambient and never to the direct sun.
 */
float ssao_occlusion(vec2 uv, float depth, vec3 world_normal) {
    const float radius = pc.ssao.x;
    const float intensity = pc.ssao.y;
    const int samples = int(pc.ssao.z + 0.5);
    if (radius <= 0.0 || intensity <= 0.0 || samples <= 0) {
        return 1.0; // off: an exact 1.0 leaves the shaded result untouched
    }
    const vec3 world_pos = world_position_from_depth(uv, depth);
    const vec3 view_pos = (camera[heap_camera_slot].view * vec4(world_pos, 1.0)).xyz;
    // the G-buffer normal is world space; the hemisphere must be built around its VIEW-space form
    const vec3 view_normal = normalize((camera[heap_camera_slot].view * vec4(world_normal, 0.0)).xyz);

    // per-pixel rotation: hash the pixel coordinate into an angle (the classic fract(sin(dot(..))))
    const float hash = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    const float angle = hash * 6.2831853;
    const vec3 random_vec = vec3(cos(angle), sin(angle), 0.0);
    const vec3 tangent = normalize(random_vec - view_normal * dot(random_vec, view_normal));
    const vec3 bitangent = cross(view_normal, tangent);
    const mat3 tbn = mat3(tangent, bitangent, view_normal);

    float occlusion = 0.0;
    for (int i = 0; i < MAX_SSAO_SAMPLES; ++i) {
        if (i >= samples) {
            break;
        }
        // golden-angle spiral in the hemisphere: cos/sin over the projected disc, z = the height
        const float t = (float(i) + 0.5) / float(samples);
        const float disc_radius = sqrt(t);
        const float phi = float(i) * 2.39996323;
        const vec3 kernel = tbn * vec3(cos(phi) * disc_radius, sin(phi) * disc_radius, sqrt(max(1.0 - t, 0.0)));
        // samples closer to the pixel count more: scale the kernel towards the origin
        const vec3 sample_pos = view_pos + kernel * (radius * (0.2 + 0.8 * t));

        const vec4 clip = camera[heap_camera_slot].proj * vec4(sample_pos, 1.0);
        const vec2 sample_uv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || sample_uv.y < 0.0 || sample_uv.y > 1.0) {
            continue; // outside the frame: no depth to compare against (the screen-space limit)
        }
        const float sample_depth = gbuffer_texel(heap_slots_gbuffer_depth + heap_image_index, sample_uv).r;
        if (sample_depth >= 1.0) {
            continue; // sky there: not an occluder
        }
        const vec3 scene_pos = world_position_from_depth(sample_uv, sample_depth);
        const float scene_z = (camera[heap_camera_slot].view * vec4(scene_pos, 1.0)).z;
        // occluded when the stored surface sits IN FRONT of the sample point (greater z = closer) ...
        // ... and the range check fades the contact out over the radius instead of cutting it hard
        const float range = clamp(radius / max(abs(view_pos.z - scene_z), 1e-5), 0.0, 1.0);
        occlusion += (scene_z >= sample_pos.z + pc.ssao.w) ? range : 0.0;
    }
    occlusion /= float(samples);
    return clamp(1.0 - occlusion * intensity, 0.0, 1.0);
}

/**
 * @brief shade the pixel described by the G-buffer
 */
void main() {
    const float depth = gbuffer_texel(heap_slots_gbuffer_depth + heap_image_index, v_uv).r;
    if (depth >= 1.0) {
        // No geometry here: this pixel shows the sky. The far-plane point along the pixel's ray gives
        // the view direction (the reconstructed position is exact even at depth 1), and the sky
        // function is the forward skybox pass's - so the two paths cannot disagree about the
        // background. The value is ADDED, like the lighting below, onto an HDR target this stage
        // knows to be zero there.
        const vec3 far_point = world_position_from_depth(v_uv, 1.0);
        out_color = vec4(sky_color(normalize(far_point - camera[heap_camera_slot].camera_pos)), 1.0);
        return;
    }

    const vec4 albedo_metallic = gbuffer_texel(heap_slots_gbuffer_albedo + heap_image_index, v_uv);
    // "unlit" render mode: the forward path draws this geometry with the flat unlit pipeline, so the
    // deferred path must produce the same thing - the base color with no lighting, no shadows, no
    // IBL and no AO (the sky above is still the shared sky, exactly as on the forward path).
    if (pc.unlit > 0.5) {
        out_color = vec4(albedo_metallic.rgb, 1.0);
        return;
    }
    const vec4 normal_roughness = gbuffer_texel(heap_slots_gbuffer_normal + heap_image_index, v_uv);
    const vec4 material = gbuffer_texel(heap_slots_gbuffer_material + heap_image_index, v_uv);

    shade_input si;
    si.pixel = ivec2(gl_FragCoord.xy); // the cluster grid's tile coordinate (see shade_input)
    si.world_pos = world_position_from_depth(v_uv, depth);
    si.normal = normal_roughness.xyz;
    si.albedo = albedo_metallic.rgb;
    si.metallic = albedo_metallic.a;
    si.roughness = normal_roughness.w;
    // ambient occlusion: the material's baked AO map times the screen-space term (M6). With SSAO off
    // ssao_occlusion() returns exactly 1.0, so this is the pre-M6 value bit for bit. It scales the IBL
    // ambient (diffuse and specular) and never the direct sun.
    si.ao = material.b * ssao_occlusion(v_uv, depth, si.normal);
    si.emissive = vec3(0.0);
    // Ray-traced sun visibility, or NEGATIVE to keep the cascaded shadow maps: the flag is the light
    // UBO's, and it is only ever set when the pass ran and the device has ray queries - so a frame with
    // rt_shadows off samples nothing that does not exist and shades exactly as it did before.
    si.shadow_override = (light[heap_light_slot].rt_shadows > 0.5) ? texture(sampler2D(rt_visibility_texture[heap_slots_rt_visibility + heap_frame_slot], heap_samplers[heap_sampler_gbuffer]), v_uv).r : -1.0;
    // ... and the punctual lights' own switch: with the stochastic pass having answered this frame, the cluster
    // loop inside shade_surface adds nothing and what is added instead is `ml_lighting`, right below.
    si.punctual_replaced = pc.punctual_replaced;

    vec3 color = shade_surface(si);

    // ---- the stochastic punctual lighting (docs/megalights.md) ----
    // Added AFTER the surface is shaded, because it is a lighting term rather than a property of the surface:
    // the stochastic pass evaluated the same BRDF from the same G-buffer, so this is the same quantity the
    // cluster loop would have produced - with a visibility ray per sample instead of no shadow at all.
    //
    // THE UPSAMPLE IS A 2x2 JOINT-BILATERAL GATHER, the same shape (and the same two criteria) the composite's
    // GI upsample uses: a half-resolution texel's value belongs to the surface it was computed FOR, so a tap
    // only counts if it agrees on view depth and on normal. The relative depth tolerance and the normal
    // exponent are literals here rather than push lanes - unlike the composite's, this gather has no
    // A/B measurement behind it yet, and inventing knobs before the measurement is how a knob ends up with a
    // value nobody can justify.
    if (pc.punctual_replaced > 0.5) {
        const vec2 ml_extent = vec2(textureSize(sampler2D(ml_lighting_texture[heap_slots_ml_resolved + heap_image_index], heap_samplers[heap_sampler_gbuffer]), 0));
        const vec2 ml_texel = 1.0 / ml_extent;
        const vec2 base = floor(v_uv * ml_extent - 0.5);
        const float view_here = -length(si.world_pos - camera[heap_camera_slot].camera_pos.xyz);
        // The tolerance is relative to the pixel's own view distance: the same absolute depth error means far
        // less at 5 units than at 50 (the argument the chain's filters make).
        const float depth_tolerance = max(0.02 * abs(view_here), 1e-5);
        vec3 ml_sum = vec3(0.0);
        float ml_weight_sum = 0.0;
        for (int y = 0; y <= 1; ++y) {
            for (int x = 0; x <= 1; ++x) {
                const vec2 tap_uv = (clamp(base + vec2(float(x), float(y)), vec2(0.0), ml_extent - 1.0) + 0.5) * ml_texel;
                const float tap_depth = gbuffer_texel(heap_slots_gbuffer_depth + heap_image_index, tap_uv).r;
                if (tap_depth >= 1.0) {
                    continue; // a background tap holds no lighting and has no view depth to compare
                }
                const vec3 tap_world = world_position_from_depth(tap_uv, tap_depth);
                const float tap_view = -length(tap_world - camera[heap_camera_slot].camera_pos.xyz);
                const float depth_weight = exp(-abs(tap_view - view_here) / depth_tolerance);
                const vec3 tap_normal = gbuffer_texel(heap_slots_gbuffer_normal + heap_image_index, tap_uv).xyz;
                const float normal_weight = pow(max(dot(si.normal, tap_normal), 0.0), 16.0);
                const float weight = depth_weight * normal_weight;
                ml_sum += texture(sampler2D(ml_lighting_texture[heap_slots_ml_resolved + heap_image_index], heap_samplers[heap_sampler_gbuffer]), tap_uv).rgb * weight;
                ml_weight_sum += weight;
            }
        }
        // The fallback is the filtered fetch at the pixel's own uv, which is what a sliver of geometry whose
        // whole 2x2 block belongs to somebody else gets - the same answer the composite's upsample gives.
        color += ml_weight_sum > 1e-5 ? ml_sum / ml_weight_sum : texture(sampler2D(ml_lighting_texture[heap_slots_ml_resolved + heap_image_index], heap_samplers[heap_sampler_gbuffer]), v_uv).rgb;
    }

    out_color = vec4(color, 1.0);
}
