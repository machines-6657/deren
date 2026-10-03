#version 450

/**
 * @file shaders/fxaa.frag
 * @brief FXAA (Lottes' 3.11 "quality" variant) over the display-referred LDR image.
 * @ingroup shaders
 *
 * Position in the chain: forward -> HDR -> bloom -> composite -> [this] -> swapchain. It has to sit
 * AFTER tonemapping (the luma thresholds below are tuned for display-referred, gamma-encoded data)
 * and it needs its own input, because a pass cannot read the image it renders into: that is what
 * core::ldr_images are for. The composite writes them *gamma-encoded* (post_push_constants::
 * encode_gamma - they are R16F, so nothing decodes them again) and this shader therefore works
 * directly in the perceptual space FXAA was designed for. Its own output is @b linear when the
 * swapchain is an sRGB attachment (which encodes to display values in hardware) and stays
 * display-encoded for a UNORM swapchain.
 *
 * What it does: luma of a 3x3 neighbourhood -> if the local contrast is under a relative threshold
 * the pixel is left alone (that is what keeps flat/gradual shading and text from being blurred) ->
 * otherwise the edge direction is estimated from the diagonal luma gradients and the pixel is
 * blended along it, with a two-step (near/far) search that stops as soon as the blended luma leaves
 * the neighbourhood's range. Cost is ~9 texture fetches, no depth/normal/motion input.
 *
 * Because it is a screen-space blur it cannot fix sub-pixel shimmer in motion (that needs TAA), and
 * it softens fine detail; the debug overlay is therefore drawn AFTER this pass, not before.
 *
 * Requires the pipeline built by runtime::make_fxaa_pipeline() (post.vert + this file, the
 * swapchain color format).
 */

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

// binding 5 = the gamma-encoded LDR image, HEAP-NATIVE now: a resource heap image plus a sampler from the sampler
// heap. The sampler matters less here than it looks - FXAA's taps are exact texel centres, and at a centre a LINEAR
// and a NEAREST filter return the same texel - so this uses the post chain's sampler, exactly as before.
#extension GL_EXT_descriptor_heap : require
#extension GL_EXT_nonuniform_qualifier : enable
#include "heap_slots.glsl"
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D display_texture[];

// Same block as post.frag / post.vert (the pipeline layout shares one push-constant range); only the
// FXAA lanes are read here, the rest exist so the offsets stay identical.
layout(push_constant) uniform PostPush {
    float exposure;
    float bloom_intensity;
    float bloom_threshold;
    float mode;
    float encode_gamma;
    float fxaa_subpixel;      // 0 = pure directional blend, up to 1 = also blend away single-pixel aliasing
    float fxaa_edge_threshold; // relative luma contrast below which a pixel counts as flat (0.166 = FXAA default)
    // THE HEAP INDICES (see heap_slots.glsl), at the END so every field above keeps its offset.
    uint frame_slot;
    uint image_index;
} pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

const float FXAA_EDGE_THRESHOLD_MIN = 0.0833; // absolute floor for the contrast test (dark areas)
const float FXAA_DIR_STEP_CAP = 8.0;          // max length of the edge-direction step, in texels
const float FXAA_SUBPIXEL_CAP = 1.0;

/**
 * @brief perceptual luma of a display-referred colour
 * @param color gamma-encoded RGB
 * @return Rec.601 luma, the quantity FXAA's contrast test and direction estimate work on
 */
float luma_of(vec3 color) {
    return dot(color, vec3(0.299, 0.587, 0.114));
}

/**
 * @brief sRGB -> linear, for the final write
 * @param color display-encoded RGB
 * @return linear RGB
 * @note the swapchain attachment encodes linear -> sRGB in hardware, so handing it an already
 *       encoded value would double-encode (see the gamma note in post.frag). A UNORM swapchain does
 *       no such encoding, and then pc.encode_gamma keeps the encoded value instead.
 * @note no local `const` inside the body: GLSL wants `const float x`, and the east-const form
 *       (`float const x`) is rejected by glslc with a bare "unexpected CONST".
 */
vec3 srgb_to_linear(vec3 color) {
    vec3 low = color / 12.92;
    vec3 high = pow(max((color + 0.055) / 1.055, vec3(0.0)), vec3(2.4));
    return mix(low, high, step(vec3(0.04045), color));
}

/**
 * @brief the FXAA pass itself: contrast test, edge direction, two-step blend, sub-pixel term
 *
 * Reads the gamma-encoded LDR image (binding 5) and writes the swapchain. The two controls come
 * from runtime::set_fxaa(): pc.fxaa_edge_threshold decides how much local luma contrast counts as
 * an edge (lower = more pixels treated = softer image) and pc.fxaa_subpixel mixes away the
 * single-pixel aliasing that survives on near-axis-aligned edges.
 */
void main() {
    vec2 texel = 1.0 / vec2(textureSize(sampler2D(display_texture[heap_slots_display_color + heap_image_index], heap_samplers[heap_sampler_post]), 0));

    vec3 rgb_m = heap_texel(display_texture[heap_slots_display_color + heap_image_index], heap_sampler_post, v_uv).rgb;
    vec3 rgb_nw = heap_texel(display_texture[heap_slots_display_color + heap_image_index], heap_sampler_post, v_uv + vec2(-1.0, -1.0) * texel).rgb;
    vec3 rgb_ne = heap_texel(display_texture[heap_slots_display_color + heap_image_index], heap_sampler_post, v_uv + vec2(1.0, -1.0) * texel).rgb;
    vec3 rgb_sw = heap_texel(display_texture[heap_slots_display_color + heap_image_index], heap_sampler_post, v_uv + vec2(-1.0, 1.0) * texel).rgb;
    vec3 rgb_se = heap_texel(display_texture[heap_slots_display_color + heap_image_index], heap_sampler_post, v_uv + vec2(1.0, 1.0) * texel).rgb;

    float luma_m = luma_of(rgb_m);
    float luma_nw = luma_of(rgb_nw);
    float luma_ne = luma_of(rgb_ne);
    float luma_sw = luma_of(rgb_sw);
    float luma_se = luma_of(rgb_se);

    float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));

    // flat area: keep the pixel untouched (this early-out is what leaves text and smooth gradients
    // alone, and it is why most pixels cost only the five taps above)
    if ((luma_max - luma_min) < max(FXAA_EDGE_THRESHOLD_MIN, luma_max * pc.fxaa_edge_threshold)) {
        out_color = vec4(srgb_to_linear(rgb_m), 1.0);
        return;
    }

    // edge direction: perpendicular to the local gradient, from the diagonal luma differences
    vec2 dir = vec2(-((luma_nw + luma_ne) - (luma_sw + luma_se)), ((luma_nw + luma_sw) - (luma_ne + luma_se)));

    // shorten the direction where the gradients nearly cancel (|dir| small), then normalise so the
    // step is measured in texels and clamp its length
    float dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * 0.25 * 0.03125, 1.0 / 128.0);
    float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, vec2(-FXAA_DIR_STEP_CAP), vec2(FXAA_DIR_STEP_CAP)) * texel;

    // two-step search along the edge: a near pair and a far pair; pick the far pair unless its luma
    // already left the neighbourhood's range (that would mean the blend ran into another feature)
    vec3 rgb_a = 0.5 * (heap_texel(display_texture[heap_slots_display_color + heap_image_index], heap_sampler_post, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                        heap_texel(display_texture[heap_slots_display_color + heap_image_index], heap_sampler_post, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 rgb_b = rgb_a * 0.5 + 0.25 * (heap_texel(display_texture[heap_slots_display_color + heap_image_index], heap_sampler_post, v_uv + dir * -0.5).rgb +
                                       heap_texel(display_texture[heap_slots_display_color + heap_image_index], heap_sampler_post, v_uv + dir * 0.5).rgb);
    float luma_b = luma_of(rgb_b);
    vec3 rgb_result = (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;

    // sub-pixel term: on top of the directional blend, mix toward the centre pixel where the pixel
    // sits noticeably above the local average (removes the single-pixel "sparkle" FXAA otherwise
    // leaves on near-horizontal/vertical edges). 0 disables it.
    if (pc.fxaa_subpixel > 0.0) {
        float luma_lowpass = (luma_nw + luma_ne + luma_sw + luma_se + luma_m * 4.0) * 0.125;
        float range = luma_max - luma_min;
        float subpixel = clamp(abs(luma_lowpass - luma_m) / max(range, 1e-5), 0.0, 1.0);
        subpixel = smoothstep(0.0, 1.0, subpixel);
        subpixel = subpixel * subpixel * clamp(pc.fxaa_subpixel, 0.0, FXAA_SUBPIXEL_CAP);
        rgb_result = mix(rgb_result, rgb_m, subpixel);
    }

    // Hand the result back in the encoding the TARGET expects: an sRGB swapchain attachment encodes
    // linear -> sRGB in hardware, so it must receive linear values (encode_gamma == 0); a UNORM
    // swapchain does no encoding, so the display-encoded result is stored as-is (encode_gamma == 1).
    // Ignoring this branch made the UNORM case too dark by a whole gamma.
    if (pc.encode_gamma > 0.5) {
        out_color = vec4(rgb_result, 1.0);
    } else {
        out_color = vec4(srgb_to_linear(rgb_result), 1.0);
    }
}
