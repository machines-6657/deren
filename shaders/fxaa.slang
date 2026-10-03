// shaders/fxaa.slang - the SLANG source of the FXAA pass (Lottes' FXAA 3.11 quality variant).
//
// PRODUCES `fxaa.frag.spv`, the same name the GLSL version produced, so the host is untouched. Ported from
// shaders/fxaa.frag: the algorithm is the GLSL algorithm - what changes is only what a different language
// requires (the entry's `[[vk::location]]`, and `GetDimensions` instead of `textureSize`), plus the heap
// fetches, which go through the shared shim (see docs/slang_migration.md).
//
// The pass runs on the TONEMAPPED, gamma-encoded image, because FXAA's relative luma thresholds are
// defined on display-referred data, and it needs its own input (a pass cannot read the image it renders
// into) - which is why the composite writes a per-swapchain-image LDR target when FXAA is on.

#include "heap_access.slang"

// Same block as post.frag / post.vert (the pipeline layout shares one push-constant range); only the FXAA
// lanes are read here, the rest exist so the offsets stay identical.
struct PostPush
{
    float exposure;
    float bloom_intensity;
    float bloom_threshold;
    float mode;
    float encode_gamma;
    float fxaa_subpixel;       // 0 = pure directional blend, up to 1 = also blend away single-pixel aliasing
    float fxaa_edge_threshold; // relative luma contrast below which a pixel counts as flat (0.166 = FXAA default)
    // THE ARTICLE'S LUT WEIGHT, WHICH THIS STAGE NEVER READS - and it is declared here for exactly the reason the
    // comment above gives: the pipeline layout shares ONE push-constant range, so a field missing on this side shifts
    // the heap indices below it by four bytes and this pass then samples whatever slot the wrong number names. That
    // is not hypothetical: adding the field to `post.slang` alone left `deferred_taa_fxaa` as the one reference
    // scenario that changed, because FXAA read its `display` image out of the LUT's slot.
    float lut_weight;
    uint frame_slot;           // THE HEAP INDICES, at the END so every field above keeps its offset
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<PostPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

const float FXAA_EDGE_THRESHOLD_MIN = 0.0833; // absolute floor for the contrast test (dark areas)
const float FXAA_DIR_STEP_CAP = 8.0;          // max length of the edge-direction step, in texels
const float FXAA_SUBPIXEL_CAP = 1.0;

/// @brief perceptual luma of a display-referred colour (Rec.601, the quantity FXAA works on)
float luma_of(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

/// @brief sRGB -> linear, for the final write
/// @note the swapchain attachment encodes linear -> sRGB in hardware, so handing it an already encoded
///       value would double-encode (see the gamma note in post.frag); a UNORM swapchain does no such
///       encoding, and then pc.encode_gamma keeps the encoded value instead.
float3 srgb_to_linear(float3 color)
{
    float3 low = color / 12.92;
    float3 high = pow(max((color + 0.055) / 1.055, float3(0.0)), float3(2.4));
    return mix(low, high, step(float3(0.04045), color));
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_uv : TEXCOORD0) : SV_Target
{
    // one handle for the LDR image this pass reads, built from its heap slot (the GLSL side indexes the
    // `display_texture[]` array instead; both resolve to the same descriptor)
    const DescriptorHandle<Texture2D<float4>> display = heap_at<Texture2D<float4>>(heap_slots_display_color + heap_image_index);

    uint width = 0u;
    uint height = 0u;
    display.GetDimensions(width, height);
    float2 texel = 1.0 / float2(width, height);

    float3 rgb_m = heap_texel(display, heap_sampler_post, v_uv).rgb;
    float3 rgb_nw = heap_texel(display, heap_sampler_post, v_uv + float2(-1.0, -1.0) * texel).rgb;
    float3 rgb_ne = heap_texel(display, heap_sampler_post, v_uv + float2(1.0, -1.0) * texel).rgb;
    float3 rgb_sw = heap_texel(display, heap_sampler_post, v_uv + float2(-1.0, 1.0) * texel).rgb;
    float3 rgb_se = heap_texel(display, heap_sampler_post, v_uv + float2(1.0, 1.0) * texel).rgb;

    float luma_m = luma_of(rgb_m);
    float luma_nw = luma_of(rgb_nw);
    float luma_ne = luma_of(rgb_ne);
    float luma_sw = luma_of(rgb_sw);
    float luma_se = luma_of(rgb_se);

    float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));

    // flat area: keep the pixel untouched (this early-out is what leaves text and smooth gradients alone)
    if ((luma_max - luma_min) < max(FXAA_EDGE_THRESHOLD_MIN, luma_max * pc.fxaa_edge_threshold))
    {
        return float4(srgb_to_linear(rgb_m), 1.0);
    }

    // edge direction: perpendicular to the local gradient, from the diagonal luma differences
    float2 dir = float2(-((luma_nw + luma_ne) - (luma_sw + luma_se)), ((luma_nw + luma_sw) - (luma_ne + luma_se)));

    // shorten the direction where the gradients nearly cancel (|dir| small), then normalise so the step is
    // measured in texels and clamp its length
    float dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * 0.25 * 0.03125, 1.0 / 128.0);
    float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, float2(-FXAA_DIR_STEP_CAP), float2(FXAA_DIR_STEP_CAP)) * texel;

    // two-step search along the edge: a near pair and a far pair; pick the far pair unless its luma already
    // left the neighbourhood's range (that would mean the blend ran into another feature)
    float3 rgb_a = 0.5 * (heap_texel(display, heap_sampler_post, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                          heap_texel(display, heap_sampler_post, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    float3 rgb_b = rgb_a * 0.5 + 0.25 * (heap_texel(display, heap_sampler_post, v_uv + dir * -0.5).rgb +
                                         heap_texel(display, heap_sampler_post, v_uv + dir * 0.5).rgb);
    float luma_b = luma_of(rgb_b);
    float3 rgb_result = (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;

    // sub-pixel term: on top of the directional blend, mix toward the centre pixel where the pixel sits
    // noticeably above the local average (removes the single-pixel "sparkle" FXAA otherwise leaves on
    // near-horizontal/vertical edges). 0 disables it.
    if (pc.fxaa_subpixel > 0.0)
    {
        float luma_lowpass = (luma_nw + luma_ne + luma_sw + luma_se + luma_m * 4.0) * 0.125;
        float range = luma_max - luma_min;
        float subpixel = clamp(abs(luma_lowpass - luma_m) / max(range, 1e-5), 0.0, 1.0);
        subpixel = smoothstep(0.0, 1.0, subpixel);
        subpixel = subpixel * subpixel * clamp(pc.fxaa_subpixel, 0.0, FXAA_SUBPIXEL_CAP);
        rgb_result = mix(rgb_result, rgb_m, subpixel);
    }

    // Hand the result back in the encoding the TARGET expects: an sRGB swapchain attachment encodes
    // linear -> sRGB in hardware, so it must receive linear values (encode_gamma == 0); a UNORM swapchain
    // does no encoding, so the display-encoded result is stored as-is (encode_gamma == 1).
    if (pc.encode_gamma > 0.5)
    {
        return float4(rgb_result, 1.0);
    }
    return float4(srgb_to_linear(rgb_result), 1.0);
}
