// shaders/upscale.slang - THE RESOLVE: the render chain's display-referred LDR image onto the swapchain.
//
// WHY IT EXISTS: `core::render_extent()` is the RENDER chain's extent, so at `[render] render_scale < 1.0` the
// whole chain (G-buffer, HDR, `ldr`, TAA history, bloom) is created smaller while the swapchain keeps the
// output size. Nothing resolved that difference, so a scaled frame landed in the TOP-LEFT QUADRANT of the
// presented image. This pass is that resolve, and it is the frame's LAST writer whenever it runs.
//
// IT IS THE LINEAR (BILINEAR) FILTER AND NOTHING ELSE. The FSR `easu`/`easu_rcas` modes are a separate,
// later step with their own config keys: the whole point of this one is the cheapest correct answer - one
// tap of the shared linear sampler at each OUTPUT pixel centre, which is a bilinear resample of the whole
// chain. `heap_sampler_post` is the sampler that makes that true by construction (linear, clamp, one mip).
//
// THE TRANSFER IS THE ONE THING THAT CAN BE GOT WRONG HERE, so it is COPIED from fxaa.slang and from
// post.frag's composite path rather than invented: the LDR image holds DISPLAY-ENCODED values (the composite
// sets `encode_gamma = 1` when it writes that target, which applies `linear_to_srgb` by hand), and both of
// this pass's destinations are the same two FXAA already handles - an sRGB swapchain attachment, which
// encodes linear -> sRGB in HARDWARE (so it must be handed LINEAR values, or the frame is double-encoded and
// washed out: measured in post.frag as a linear 0.5 coming out at 188), and a UNORM one, which does no
// encoding at all (so the display-encoded value is what should be stored). `pc.encode_gamma` is that pair.

#include "heap_access.slang"

// THIS PASS'S OWN BLOCK, and it is deliberately NOT the post chain's (post.frag's `PostPush`): the resolve
// samples one image and re-encodes nothing else, so the exposure/bloom/edge lanes would all be dead fields.
// The four `con` vectors are EASU's (`FsrEasuCon`, computed on the C++ side - see upscale_pass::record) and
// the mode says which filter runs; the two lanes at the end are the ones the FRAMEWORK appends.
struct UpscalePush
{
    // EASU's constants, computed from the RENDER extent (input viewport AND input size: this renderer has no
    // dynamic-resolution offset region) and the swapchain's extent (the output size). All four are read - by
    // mode 1 - so none of them is a spare lane kept for symmetry.
    float4 con0; // the input -> output scale, and the -0.5 centering
    float4 con1; // 1 / input size, then the first tap offset
    float4 con2; // the second and third tap offsets
    float4 con3; // the fourth tap offset
    // WHICH FILTER: 0 = the linear (bilinear) resolve, 1 = EASU. Linear is kept as a selectable mode rather
    // than deleted, because it is the reference the FSR filter is MEASURED against (see
    // build-release-clang64/upscale_quality.py: distance to a native-resolution frame and sharpness).
    float mode;
    // 1 = the values this pass reads are already the display-encoded ones and the target stores them as-is
    // (a UNORM swapchain); 0 = the target is an sRGB attachment and encodes on write, so this pass must hand
    // it LINEAR values. Composed on the C++ side from the surface's format, exactly as fxaa_pass::record does.
    float encode_gamma;
    // THE HEAP INDICES AT THE END, appended by the host (`runtime::push_stage_block`) so a pass never has to
    // know the frame slot or the swapchain image (see shaders/heap_slots.glsl).
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<UpscalePush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

/// @brief sRGB -> linear, for the final write
/// @note the swapchain attachment encodes linear -> sRGB in hardware, so handing it an already encoded value
///       would double-encode (see the gamma note in post.frag); a UNORM swapchain does no such encoding, and
///       then pc.encode_gamma keeps the encoded value instead.
float3 srgb_to_linear(float3 color)
{
    float3 low = color / 12.92;
    float3 high = pow(max((color + 0.055) / 1.055, float3(0.0)), float3(2.4));
    return mix(low, high, step(float3(0.04045), color));
}

// =====================================================================================================
// EASU - FSR 1's edge-adaptive spatial upsampling, ported from `ffx_fsr1.h` v1.20210629 (AMD, MIT).
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. Permission is hereby granted, free of
// charge, to any person obtaining a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions: the above
// copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// EASU is a directionally and anisotropically adaptive radial lanczos filter: it estimates an edge direction
// and length from the 12-tap neighbourhood, stretches the kernel along that edge, and DE-RINGS the result by
// clamping it to the min/max of the four nearest taps. AMD's stated input requirements are 3-channel RGB in
// [0,1], display-encoded - which is exactly what `heap_slots_display_color` holds after the composite's
// tonemap and gamma encode, and why this pass (after tonemapping, before the UI is composited) is where AMD
// says EASU belongs.
// =====================================================================================================

// ---- the twelve taps, and why this port does NOT use the reference's gathers -------------------------
//
// The reference reaches its 12 taps with FOUR `textureGather` calls and unpacks the components into the
// letters b/c/e/f/g/h/i/j/k/l/n/o. This port samples each tap BY NAME instead, and that is a MEASURED
// decision rather than a simplification:
//
//   * THE GATHER FORM COSTS A DEVICE FEATURE. Slang lowers `GatherRed/Green/Blue` to `OpImageGather`, which
//     needs the `ImageGatherExtended` capability: building the gather version of this file with the project's
//     own slangc reports "entry point 'main' uses additional capabilities that are not part of the specified
//     profile 'spirv_1_6'" and upgrades the profile to include `spvImageGatherExtended`. In Vulkan that
//     capability is gated on `VkPhysicalDeviceFeatures::shaderImageGatherExtended`, so the gather form would
//     make the upscaler unavailable on a device that lacks it - and this renderer QUERIES its optional
//     features (ray queries, mesh shaders) rather than assuming them.
//   * IT IS NOT FEWER INSTRUCTIONS. A gather returns the four texels of ONE channel, so RGB needs three
//     gathers per position: 4 positions x 3 channels = 12 gathers. Twelve point samples of a
//     `Texture2D<float4>` return all three channels each - 12 instructions, the same count, and only the
//     texels that are used (each gather fetches four and the reference discards one per position).
//   * AND IT IS THE SAME ARITHMETIC. A gather returns its texels UNFILTERED - a point fetch of the 2x2 around
//     the coordinate, not a bilinear result - so sampling each tap at its own texel centre with a
//     nearest/clamp sampler is the same value, with the reference's own clamp behaviour at the borders.
//
// The offsets are the reference's own tap offsets (the `off` of its twelve `FsrEasuTapF` calls, relative to
// `f` at (0,0)), and its de-ring block confirms the letters: it takes the four nearest taps as f, g, j, k,
// i.e. the 2x2 at (0,0), (1,0), (0,1), (1,1).
//
//    b c
//  e f g h
//  i j k l
//    n o
float4 easu_tap_texel(DescriptorHandle<Texture2D<float4>> t, uint sampler, float2 fp, float2 off, float2 texel_size)
{
    // (fp + off) is the tap's integer texel index, + 0.5 is its centre, and * texel_size normalizes it.
    return t.SampleLevel(heap_at<SamplerState>(sampler), (fp + off + 0.5) * texel_size, 0.0);
}

/// the reference's approximate luma, times two (its `bL`, `cL`, ... lanes)
float easu_luma(float4 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

// ---- FsrEasuTapF: one tap's contribution ------------------------------------------------------------
void easu_tap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c)
{
    float2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * (-dir.y)) + (off.y * dir.x);
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    // The reference's approximation of lanczos2 without sin/rcp/sqrt, kept as written: the two lobes are what
    // give EASU its edge-directed negative lobe, and the window is what keeps the ringing out.
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

// ---- FsrEasuSetF: accumulate the gradient direction and length --------------------------------------
// The reference's four boolean lanes are compile-time immediates there and an index here; the weight is the
// tap's own bilinear weight in the 2x2 the fractional position sits in.
void easu_set(inout float2 dir, inout float len, float2 pp, int which, float lA, float lB, float lC, float lD, float lE)
{
    float w = 0.0;
    if (which == 0) { w = (1.0 - pp.x) * (1.0 - pp.y); }
    else if (which == 1) { w = pp.x * (1.0 - pp.y); }
    else if (which == 2) { w = (1.0 - pp.x) * pp.y; }
    else { w = pp.x * pp.y; }
    // Direction is the '+' difference, and the length turns a gradient reversal into zero.
    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = 1.0 / lenX;
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;
    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = 1.0 / lenY;
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

// ---- FsrEasuF: the 12-tap kernel --------------------------------------------------------------------
// @param ip the OUTPUT pixel's integer position, which is what the reference's thread coordinate is.
float3 easu(float2 ip)
{
    const DescriptorHandle<Texture2D<float4>> source = heap_at<Texture2D<float4>>(heap_slots_display_color + heap_image_index);
    const uint sampler = heap_sampler_post_nearest; // EASU's taps are point fetches; see the tap note above

    // Position of 'f' (the tap the kernel centres on), and the fractional part the taps are offset by.
    float2 pp = ip * pc.con0.xy + pc.con0.zw;
    float2 fp = floor(pp);
    pp -= fp;
    const float2 texel_size = pc.con1.xy; // 1 / input size, which con1 already carries

    // THE TWELVE TAPS, by name.
    float4 t_b = easu_tap_texel(source, sampler, fp, float2( 0.0, -1.0), texel_size);
    float4 t_c = easu_tap_texel(source, sampler, fp, float2( 1.0, -1.0), texel_size);
    float4 t_e = easu_tap_texel(source, sampler, fp, float2(-1.0,  0.0), texel_size);
    float4 t_f = easu_tap_texel(source, sampler, fp, float2( 0.0,  0.0), texel_size);
    float4 t_g = easu_tap_texel(source, sampler, fp, float2( 1.0,  0.0), texel_size);
    float4 t_h = easu_tap_texel(source, sampler, fp, float2( 2.0,  0.0), texel_size);
    float4 t_i = easu_tap_texel(source, sampler, fp, float2(-1.0,  1.0), texel_size);
    float4 t_j = easu_tap_texel(source, sampler, fp, float2( 0.0,  1.0), texel_size);
    float4 t_k = easu_tap_texel(source, sampler, fp, float2( 1.0,  1.0), texel_size);
    float4 t_l = easu_tap_texel(source, sampler, fp, float2( 2.0,  1.0), texel_size);
    float4 t_n = easu_tap_texel(source, sampler, fp, float2( 0.0,  2.0), texel_size);
    float4 t_o = easu_tap_texel(source, sampler, fp, float2( 1.0,  2.0), texel_size);

    const float bL = easu_luma(t_b);
    const float cL = easu_luma(t_c);
    const float eL = easu_luma(t_e);
    const float fL = easu_luma(t_f);
    const float gL = easu_luma(t_g);
    const float hL = easu_luma(t_h);
    const float iL = easu_luma(t_i);
    const float jL = easu_luma(t_j);
    const float kL = easu_luma(t_k);
    const float lL = easu_luma(t_l);
    const float nL = easu_luma(t_n);
    const float oL = easu_luma(t_o);

    float2 dir = float2(0.0, 0.0);
    float len = 0.0;
    easu_set(dir, len, pp, 0, bL, eL, fL, gL, jL);
    easu_set(dir, len, pp, 1, cL, fL, gL, hL, kL);
    easu_set(dir, len, pp, 2, fL, iL, jL, kL, nL);
    easu_set(dir, len, pp, 3, gL, jL, kL, lL, oL);

    // Normalize with an approximation and clean up close to zero. The 1/32768 guard is the algorithm's, not a
    // defensive extra: a zero-length gradient is the flat case and gets the identity direction.
    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = rsqrt(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;
    // From {0 to 2} to {0 to 1}, shaped with a square.
    len = len * 0.5;
    len *= len;
    // The kernel stretches to sqrt(2) on a diagonal.
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    // The min/max of the four nearest taps (f, g, j, k), which is what de-rings the negative lobe.
    float3 min4 = min(min(min(t_f.rgb, t_g.rgb), t_j.rgb), t_k.rgb);
    float3 max4 = max(max(max(t_f.rgb, t_g.rgb), t_j.rgb), t_k.rgb);

    float3 aC = float3(0.0, 0.0, 0.0);
    float aW = 0.0;
    easu_tap(aC, aW, float2( 0.0, -1.0) - pp, dir, len2, lob, clp, t_b.rgb);
    easu_tap(aC, aW, float2( 1.0, -1.0) - pp, dir, len2, lob, clp, t_c.rgb);
    easu_tap(aC, aW, float2(-1.0,  1.0) - pp, dir, len2, lob, clp, t_i.rgb);
    easu_tap(aC, aW, float2( 0.0,  1.0) - pp, dir, len2, lob, clp, t_j.rgb);
    easu_tap(aC, aW, float2( 0.0,  0.0) - pp, dir, len2, lob, clp, t_f.rgb);
    easu_tap(aC, aW, float2(-1.0,  0.0) - pp, dir, len2, lob, clp, t_e.rgb);
    easu_tap(aC, aW, float2( 1.0,  1.0) - pp, dir, len2, lob, clp, t_k.rgb);
    easu_tap(aC, aW, float2( 2.0,  1.0) - pp, dir, len2, lob, clp, t_l.rgb);
    easu_tap(aC, aW, float2( 2.0,  0.0) - pp, dir, len2, lob, clp, t_h.rgb);
    easu_tap(aC, aW, float2( 1.0,  0.0) - pp, dir, len2, lob, clp, t_g.rgb);
    easu_tap(aC, aW, float2( 1.0,  2.0) - pp, dir, len2, lob, clp, t_o.rgb);
    easu_tap(aC, aW, float2( 0.0,  2.0) - pp, dir, len2, lob, clp, t_n.rgb);

    // Normalize and de-ring: the result cannot leave the neighbourhood of the four nearest taps.
    return min(max4, max(min4, aC * (1.0 / aW)));
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_uv : TEXCOORD0, float4 sv_position : SV_Position) : SV_Target
{
    // ONE handle for the LDR image, built from its per-swapchain-image heap slot: the composite wrote it
    // earlier in this command buffer and upscale_pass::record is what moved it to a sampled layout. There is
    // no set to bind and no sampler to choose - the frame bound the heaps for this command buffer.
    const DescriptorHandle<Texture2D<float4>> display = heap_at<Texture2D<float4>>(heap_slots_display_color + heap_image_index);

    // MODE 0 - THE LINEAR RESAMPLE: v_uv is the output pixel centre in the render image's [0,1] square, so one
    // tap of the shared linear sampler reads the four render texels that surround it and interpolates - which
    // is the whole "linear filter", and the reference EASU is measured against (it loses about half the
    // high-frequency energy of the chain it resamples; see upscale_quality.py).
    float3 color;
    if (pc.mode < 0.5)
    {
        color = heap_texel(display, heap_sampler_post, v_uv).rgb;
    }
    else
    {
        // MODE 1 - EASU. Its input position is the OUTPUT pixel's integer coordinate, which SV_Position
        // carries: the reference's `ip` is its thread coordinate, and the floor of a pixel centre is that pixel.
        color = easu(floor(sv_position.xy));
    }

    // Hand the result back in the encoding the TARGET expects (the same branch fxaa.slang ends with): an sRGB
    // swapchain attachment encodes linear -> sRGB in hardware, so it must receive linear values
    // (encode_gamma == 0); a UNORM swapchain does no encoding, so the display-encoded value is stored as-is.
    if (pc.encode_gamma > 0.5)
    {
        return float4(color, 1.0);
    }
    return float4(srgb_to_linear(color), 1.0);
}
