// shaders/post.slang - the SLANG source of the fullscreen-triangle vertex stage.
//
// PRODUCES `post.vert.spv`, the same name the GLSL version produced, so the host is untouched. Every
// fullscreen pass uses it (the bloom/composite chain, TAA, FXAA, deferred lighting and the G-buffer debug
// view), so this one file is the vertex stage of five pipelines - which is why it is ported early: it is
// also the cheapest place to prove a VERTEX stage in Slang, before pbr.vert and shadow.vert need one.
//
// No vertex buffer: the triangle is synthesized from the vertex index, which covers the whole screen with
// three vertices and avoids the diagonal seam a two-triangle quad can show with some filters. The UV
// follows the framebuffer orientation, so v_uv = (0,0) is the top-left texel of the source image.
//
// THE FILE INCLUDES THE SHIM FOR ITS FRAGMENT ENTRY (the post chain's three modes read heap images). The
// vertex entry needs none of it, and pays nothing for it: an unused declaration compiles away.

#include "heap_access.slang"

struct post_vertex_output
{
    [[vk::location(0)]] float2 v_uv;
    float4 position : SV_Position; // gl_Position on the GLSL side; SV_Position is the same builtin
};

[shader("vertex")]
post_vertex_output main(uint vertex_index : SV_VertexID)
{
    // GLSL's gl_VertexIndex is SPIR-V's VertexIndex; SV_VertexID subtracts BaseVertex, which a
    // fullscreen draw never sets - the two are the same value here (Slang's own docs state the mapping).
    float2 pos = float2(float((vertex_index << 1u) & 2u), float(vertex_index & 2u));

    post_vertex_output out;
    out.v_uv = pos;
    out.position = float4(pos * 2.0 - 1.0, 0.0, 1.0);
    return out;
}

// =============================================================================================================
// THE FRAGMENT STAGE (entry `frag_main`): the post chain's three modes, selected by `pc.mode` - the bright-pass
// prefilter (mode 0), the 13-tap downsample (mode 1) and the composite (mode 2). Every frame of every scenario
// goes through this chain, so this port is the most widely recorded one.
//
// THE ONE PORT SHAPE THAT DIFFERS FROM THE GLSL, and it is a Slang advantage rather than a workaround: the GLSL
// filters take a heap SLOT as a parameter and index the heap array inside, because a `texture2D` parameter does
// not survive a function boundary in glslang (the shim's heap_texel note records the spirv-val failure that
// proved it). Slang has no such limit, so `downsample_13` and `sample_tent` take the texture HANDLE and the
// sampler slot, which is what the shim's `heap_texel` function already wants.
// =============================================================================================================

struct PostPush
{
    float exposure;        // linear exposure scale (runtime::set_exposure)
    float bloom_intensity; // blend weight of the bloom sum (0 = off)
    float bloom_threshold; // linear value subtracted in the bright pass
    float mode;            // 0 prefilter / 1 downsample / 2 composite
    float encode_gamma;    // composite only: 1 = encode to sRGB by hand (non-sRGB swapchain), 0 = the
                           // attachment is an sRGB format and the hardware encodes on write
    // THE FXAA LANES ARE DECLARED EVEN THOUGH THIS STAGE DOES NOT READ THEM: the CPU pushes the WHOLE
    // post_push_constants struct (28 bytes) and appends the three heap indices after it, so a block that
    // stopped at `encode_gamma` (20 bytes) would read its lanes out of these two floats instead - which is
    // what made the composite sample slot 0 instead of the HDR and left the frame black. A stage may ignore a
    // member, but the layout up to the indices has to match the CPU struct.
    float fxaa_subpixel;
    float fxaa_edge_threshold;
    // THE ARTICLE'S LUT WEIGHT (`ZmdLutPost.shader`'s `_LutWeight`). It sits HERE, at the end of the floats and
    // before the three heap indices, because that is where the CPU's `post_push_constants` puts it - see this
    // struct's own note above about a block that stops short reading its lanes out of the wrong members.
    float lut_weight;
    uint frame_slot;
    uint image_index;
    uint post_source_slot; // this chain's own lane: the host names the pass's ABSOLUTE grid slot per pass
};
[[vk::push_constant]] ConstantBuffer<PostPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)
#define heap_post_source_slot (pc.post_source_slot)

/// @brief ACES filmic tonemapping (Narkowicz fit)
/// @param color linear HDR colour, already multiplied by the exposure scale
/// @return display-referred colour in [0,1]
float3 aces_tone_mapping(float3 color)
{
    return clamp((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14), 0.0, 1.0);
}

/// @brief linear -> sRGB, for a swapchain that is NOT an sRGB format
/// @note with an sRGB attachment the hardware encodes on write, and encoding by hand on top of that applies
///       gamma TWICE (measured: a linear 0.5 came out as 188, which is already sRGB(0.5), so the extra
///       pow(1/2.2) turned x^0.45 into x^0.207 - a washed-out image). This path therefore only runs when
///       pc.encode_gamma is set, i.e. for a UNORM swapchain.
float3 linear_to_srgb(float3 color)
{
    const float3 low = color * 12.92;
    const float3 high = 1.055 * pow(max(color, float3(0.0, 0.0, 0.0)), float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    return mix(low, high, step(float3(0.0031308, 0.0031308, 0.0031308), color));
}

/// @brief 13-tap downsample (Karis, "Next Generation Post Processing in Call of Duty"), the filter UE uses for
///        its bloom chain: a 5x5 footprint built from a 3x3 ring plus a 2x2 inner quad, corners weighted
///        lowest
/// @param source the level being read (its size drives the tap spacing, so no resolution constant)
/// @return the filtered RGB (weights sum to 1)
/// @note a plain box downsample moves a single bright pixel into the next level almost unchanged, and after
///       two or three levels that pixel IS a whole texel of a very coarse image, which the composite then
///       magnifies into a visible block; the 13 taps spread it over its neighbourhood on the way down.
float3 downsample_13(DescriptorHandle<Texture2D<float4>> source, uint sampler_slot, float2 uv)
{
    const float2 t = 1.0 / heap_size(source, 0u);
    const float3 a = heap_texel(source, sampler_slot, uv + t * float2(-2.0, -2.0)).rgb;
    const float3 b = heap_texel(source, sampler_slot, uv + t * float2(0.0, -2.0)).rgb;
    const float3 c = heap_texel(source, sampler_slot, uv + t * float2(2.0, -2.0)).rgb;
    const float3 d = heap_texel(source, sampler_slot, uv + t * float2(-2.0, 0.0)).rgb;
    const float3 e = heap_texel(source, sampler_slot, uv).rgb;
    const float3 f = heap_texel(source, sampler_slot, uv + t * float2(2.0, 0.0)).rgb;
    const float3 g = heap_texel(source, sampler_slot, uv + t * float2(-2.0, 2.0)).rgb;
    const float3 h = heap_texel(source, sampler_slot, uv + t * float2(0.0, 2.0)).rgb;
    const float3 i = heap_texel(source, sampler_slot, uv + t * float2(2.0, 2.0)).rgb;
    const float3 j = heap_texel(source, sampler_slot, uv + t * float2(-1.0, -1.0)).rgb;
    const float3 k = heap_texel(source, sampler_slot, uv + t * float2(1.0, -1.0)).rgb;
    const float3 l = heap_texel(source, sampler_slot, uv + t * float2(-1.0, 1.0)).rgb;
    const float3 m = heap_texel(source, sampler_slot, uv + t * float2(1.0, 1.0)).rgb;
    float3 sum = e * 0.125;
    sum += (a + c + g + i) * 0.03125;
    sum += (b + d + f + h) * 0.0625;
    sum += (j + k + l + m) * 0.125;
    return sum;
}

/// @brief 3x3 tent filter (1 2 1 / 2 4 2 / 1 2 1) over one texel of @p level 's own resolution: the footprint
///        an upsample step of the bloom pyramid stands for
/// @return the filtered RGB (weights sum to 1)
/// @note sampling a coarse level with a single bilinear fetch (or a half-texel box) makes one of ITS texels a
///       block on screen - at 1/16 resolution that is 16x16 pixels of flat light with square edges. The tent
///       is what UE's bloom upsample uses (PostProcessBloom.usf), and it is the reason a coarse level reads
///       as a wide glow instead of a tile.
float3 sample_tent(DescriptorHandle<Texture2D<float4>> level, uint sampler_slot, float2 uv)
{
    const float2 t = 1.0 / heap_size(level, 0u);
    float3 sum = heap_texel(level, sampler_slot, uv).rgb * 4.0;
    sum += heap_texel(level, sampler_slot, uv + float2(-t.x, 0.0)).rgb * 2.0;
    sum += heap_texel(level, sampler_slot, uv + float2(t.x, 0.0)).rgb * 2.0;
    sum += heap_texel(level, sampler_slot, uv + float2(0.0, -t.y)).rgb * 2.0;
    sum += heap_texel(level, sampler_slot, uv + float2(0.0, t.y)).rgb * 2.0;
    sum += heap_texel(level, sampler_slot, uv + float2(-t.x, -t.y)).rgb;
    sum += heap_texel(level, sampler_slot, uv + float2(t.x, -t.y)).rgb;
    sum += heap_texel(level, sampler_slot, uv + float2(-t.x, t.y)).rgb;
    sum += heap_texel(level, sampler_slot, uv + float2(t.x, t.y)).rgb;
    return sum / 16.0;
}

/// @brief the three post-process stages, selected by pc.mode
/// @note the bloom level weights are the artistic part: 0.50 / 0.30 / 0.20 / 0.12 over the four levels, so
///       the widest level contributes least.
[shader("fragment")]
float4 frag_main([[vk::location(0)]] float2 v_uv : TEXCOORD0) : SV_Target
{
    const DescriptorHandle<Texture2D<float4>> source = heap_at<Texture2D<float4>>(heap_post_source_slot);

    if (pc.mode < 0.5)
    {
        // bright pass: one fetch into a half-resolution target. Sampling a full-resolution source from a
        // pixel centre of the half-resolution target lands exactly halfway between four source texels, so
        // the LINEAR sampler returns their 2x2 average.
        float3 color = heap_texel(source, heap_sampler_post, v_uv).rgb;
        const float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));

        // THE THRESHOLD IS A RAMP OVER LUMINANCE, not a per-channel subtract-and-clamp: subtracting and
        // clamping makes the bloom switch on and off along the iso-luminance contour of whatever is in the
        // frame, so a whole region pops in at once with geometrically straight edges as the camera moves -
        // the "bright spot suddenly becomes a big square" artifact. UE does exactly this ramp
        // (PostProcessBloom.usf, BloomSetupCommon): BloomAmount = saturate((Luminance - Threshold) * 0.5).
        //
        // FIREFLY CEILING: the ramp above bounds the FRACTION a pixel may contribute, not its magnitude.
        // With a specular highlight three orders of magnitude above the sky, one blown pixel still entered
        // the chain at full value and the whole pyramid carried it - a tiny highlight becoming a large round
        // patch that appears and vanishes as the camera moves. UE clamps the bloom input for exactly this
        // reason. The curve leaves everything at or below the ceiling untouched and folds the rest towards
        // 2x the ceiling (an excess of 97.5 becomes 4.94 at a ceiling of 2.5) - smoothly, so the clamp
        // introduces no plateau and no edge, and since it is a luminance scale the hue survives.
        const float firefly_ceiling = 2.5;
        const float base = min(luminance, firefly_ceiling);
        const float excess = max(luminance - firefly_ceiling, 0.0);
        const float compressed = base + excess / (1.0 + excess / firefly_ceiling);
        color *= compressed / max(luminance, 1e-5);

        const float amount = clamp((compressed - pc.bloom_threshold) * 0.5, 0.0, 1.0);
        return float4(color * amount, 1.0);
    }

    if (pc.mode < 1.5)
    {
        // downsample: 13-tap filter into the next level (see downsample_13)
        return float4(downsample_13(source, heap_sampler_post, v_uv), 1.0);
    }

    // composite: weighted sum of the four levels, each read through the tent filter that the upsample step of
    // a bloom pyramid stands for (see sample_tent)
    float3 bloom = sample_tent(heap_at<Texture2D<float4>>(heap_slots_bloom_l0 + heap_image_index), heap_sampler_post, v_uv) * 0.50;
    bloom += sample_tent(heap_at<Texture2D<float4>>(heap_slots_bloom_l1 + heap_image_index), heap_sampler_post, v_uv) * 0.30;
    bloom += sample_tent(heap_at<Texture2D<float4>>(heap_slots_bloom_l2 + heap_image_index), heap_sampler_post, v_uv) * 0.20;
    bloom += sample_tent(heap_at<Texture2D<float4>>(heap_slots_bloom_l3 + heap_image_index), heap_sampler_post, v_uv) * 0.12;

    float3 color = heap_texel(source, heap_sampler_post, v_uv).rgb;
    // The screen-space GI that used to be added here (a joint-bilateral upsample of the half-resolution
    // indirect) is not added here any more: what the scene target holds is the direct radiance plus the
    // ambient and the IBL.
    color += bloom * pc.bloom_intensity;
    color *= pc.exposure;
    color = aces_tone_mapping(color);
    // Display encoding: normally the sRGB swapchain attachment does it (pc.encode_gamma == 0); this path
    // only exists so a UNORM swapchain still gets correct output.
    if (pc.encode_gamma > 0.5)
    {
        color = linear_to_srgb(color);
    }

    // ---- THE ARTICLE'S LUT POST (`ZmdLutPost.shader`), ON THE DISPLAY-REFERRED COLOUR ----
    //
    // WHY HERE AND NOT EARLIER: the article's domain mapping is
    // `log2(c * 5.55555582 + 0.0479959995) * 0.0734997839 + 0.386036009` - the standard log encoding a 32^3 grading
    // LUT is authored against - and the value it grades is the one the artist saw, i.e. AFTER the tonemap and the
    // display encode. Both have just happened above, which is why this block sits between them and the return.
    //
    // THE ADDRESSING IS THE ARTICLE'S, LINE FOR LINE: blue picks the slice and the two neighbouring slices are
    // lerped, red and green sit inside the tile with the vertical flip its `yOffset` carries, and every index is
    // pulled half a texel in (`halfColX` / `halfColY`) so a linear filter reads texel CENTRES. `_LutWeight` is the
    // article's own blend (`lerp(origin, lut, w)`), not a multiply.
    //
    // THE SLOT HOLDS A NEUTRAL CUBE BY DEFAULT (`runtime::set_post_lut` bakes it), so this block is the identity with
    // no artist's cube present: the weight can be the article's 1.0 and the frame is unchanged, which is what makes
    // "the LUT lane is wired" a measurement rather than an assertion.
    //
    // WHAT IS NOT REPRODUCED IS THE ARTICLE'S DITHER (`ditherNoise * 0.0013725491 * _LutWeight`), a recorded
    // difference rather than an oversight: it needs the pixel coordinate and this stage has no screen-size constant
    // to build one from (the bloom's filters derive their spacing from each level's own size instead). Its amplitude
    // is 1.4e-3 of the graded value, below what an 8-bit attachment can hold, so the grade is applied exactly and the
    // dither is left to a stage that knows the resolution.
    if (pc.lut_weight > 0.0)
    {
        const float3 origin = color;
        const float3 lut_input = saturate(log2(max(float3(0.0, 0.0, 0.0), color * 5.55555582 + 0.0479959995)) * 0.0734997839 + 0.386036009);
        const float colors = 32.0;
        const float max_colour = colors - 1.0;
        const float half_col_x = 0.5 / (colors * colors);
        const float half_col_y = 0.5 / colors;
        const float threshold = max_colour / colors;
        const float slice_offset = lut_input.b * max_colour;
        const float slice_0 = floor(slice_offset);
        const float slice_1 = min(slice_0 + 1.0, max_colour);
        const float slice_weight = frac(slice_offset);
        const float x_offset = half_col_x + lut_input.r * threshold / colors;
        const float y_offset = 1.0 - (half_col_y + lut_input.g * threshold);
        const float3 lut_0 = heap_texel(heap_at<Texture2D<float4>>(heap_slots_post_lut), heap_sampler_post, float2(x_offset + slice_0 / colors, y_offset)).rgb;
        const float3 lut_1 = heap_texel(heap_at<Texture2D<float4>>(heap_slots_post_lut), heap_sampler_post, float2(x_offset + slice_1 / colors, y_offset)).rgb;
        color = lerp(origin, lerp(lut_0, lut_1, slice_weight), pc.lut_weight);
    }
    return float4(color, 1.0);
}
