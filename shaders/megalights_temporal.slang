// shaders/megalights_temporal.slang - the SLANG source of the stochastic punctual lighting's temporal pass.
//
// PRODUCES `megalights_temporal.comp.spv`, the same name the GLSL version produced, so the host is untouched.
// Ported from shaders/megalights_temporal.comp.
//
// WHAT IT DOES, unchanged by the port: `megalights_trace.comp` answers with a few sampled lights per pixel and
// one frame of that is an estimate rather than an image; averaging DIFFERENT frames' samples is what turns it
// into one. The policy is Unreal's (`docs/reference/megalights_stochastic_lighting.md` section 4): a RUNNING
// MEAN whose weight is a per-pixel count `1 / frames` (a fixed 0.9 EMA averages ten frames forever and cannot
// tell a pixel that just appeared from one that has been still for a hundred), a REJECTION before it on the
// history's existence and on the reprojected depth agreeing, and a CLAMP of the history into this frame's
// neighbourhood statistics as `mean +/- 2 * stddev` rather than a min/max box (a mean/stddev box is not dragged
// around by a single outlier). A disagreeing history is KEPT with its count cut to ML_DISAGREE_FRAMES rather
// than dropped - the hard version showed the chain's raw per-frame estimate exactly where TAA's jitter matters
// most, and the measurement is in the GLSL file's comment.
//
// THE FRAME COUNT RIDES IN THE HISTORY'S ALPHA LANE, and the trace's alpha (the fraction of samples that found
// their light unblocked) has no consumer yet.
//
// TWO PORT DELTAS, both forced and both harmless here:
//   - every fetch is the shim's EXPLICIT-LOD form. TRAP 4 is why it has to be one of the two - a compute stage
//     is allowed implicit LOD only where Slang can see a derivative quad, and several of these fetches sit
//     after a `continue` - and the explicit form is EXACTLY the same value here, because every image in this
//     chain is a single-mip intermediate (the G-buffer's depth and velocity, the trace, the history): with one
//     mip there is no other level to select, so LOD 0 is what the implicit fetch resolved to anyway.
//   - the resolve is an RWTexture2D behind the STORAGE slot the grid reserves for that image
//     (`heap_slots_ml_resolved_storage`), because one image has two descriptors and no single heap descriptor
//     is both sampled and storage.
//
// WHAT IT DOES NOT DO: no separate spatial pass (the pre-filter runs BEFORE the accumulation, over taps that
// are already in registers - filtering the accumulated image would need a second pass, image and descriptor
// family), no history rectification beyond the clamp, and no motion-vector dilation.

#include "heap_access.slang"

struct MlTemporalPush
{
    // x = projection[2][2], y = projection[3][2] (the view-depth linearization pair), z = the frame-count cap,
    // w = the history's relative depth tolerance (UE's 0.03).
    float4 params;
    // xy = this pass's extent in texels, z = the spatial pre-filter's sigma (0 = off), w unused.
    float4 extents;
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<MlTemporalPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

/// @brief UE's `r.MegaLights.Temporal.MaxFramesAccumulated`: where a still pixel's running mean stops.
const float ML_MAX_FRAMES = 12.0;
/// @brief how many taps the neighbourhood statistics take: a 3x3, the same window the GI resolve clamps against.
const int ML_CLAMP_RADIUS = 1;
/// @brief the accumulated count a history is cut to when the reprojected depth disagrees (a soft rejection: the
///        pixel keeps a short average instead of restarting from one raw frame).
const float ML_DISAGREE_FRAMES = 2.0;
/// @brief the spatial pre-filter's radius (a 3x3: the same window the clamp above uses).
const int SPATIAL_RADIUS = 1;

/// @brief view-space distance of a depth-buffer value (the inversion every pass in this chain uses)
float view_depth(float z_ndc)
{
    return pc.params.y / (z_ndc + pc.params.x);
}

/// @brief one exact texel of a per-swapchain-image heap image, through the NEAREST sampler
/// @note this pass works per pixel, so every read is an exact texel; all of these images are single-mip
///       intermediates, which is what makes the explicit-LOD form the same value (see the file header).
float4 ml_texel(uint base_slot, float2 uv)
{
    return heap_texel_lod0(heap_at<Texture2D<float4>>(base_slot + heap_image_index), heap_sampler_gbuffer, uv);
}

/// @brief the variance-weighted mean of this frame's estimate over a small neighbourhood
/// @return the filtered estimate, or this pixel's own value when every tap was rejected
/// @note THE SPATIAL HALF of the denoise, and it runs BEFORE the accumulation rather than after it (see the
///       header). The two weights are this chain's usual pair - a relative view-depth test and a spatial
///       Gaussian - and the SIGMA comes from the push block, so 0 makes this function the identity and the
///       whole thing an A/B.
float3 spatial_mean(float2 uv, float view_here, float sigma)
{
    if (sigma <= 0.0)
    {
        return ml_texel(heap_slots_ml_trace, uv).rgb;
    }
    const float2 texel = float2(1.0, 1.0) / pc.extents.xy;
    const float depth_tolerance = max(0.02 * view_here, 1e-5);
    const float inv_sigma2 = 1.0 / (2.0 * sigma * sigma);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight_sum = 0.0;
    for (int y = -SPATIAL_RADIUS; y <= SPATIAL_RADIUS; ++y)
    {
        for (int x = -SPATIAL_RADIUS; x <= SPATIAL_RADIUS; ++x)
        {
            const float2 offset = float2(float(x), float(y));
            const float2 tap_uv = uv + offset * texel;
            const float tap_depth = ml_texel(heap_slots_gbuffer_depth, tap_uv).r;
            if (tap_depth >= 1.0)
            {
                continue; // background: no estimate, and no depth to agree with
            }
            const float depth_weight = exp(-abs(view_depth(tap_depth) - view_here) / depth_tolerance);
            const float spatial_weight = exp(-dot(offset, offset) * inv_sigma2);
            const float weight = depth_weight * spatial_weight;
            sum += ml_texel(heap_slots_ml_trace, tap_uv).rgb * weight;
            weight_sum += weight;
        }
    }
    return weight_sum > 1e-5 ? sum / weight_sum : ml_texel(heap_slots_ml_trace, uv).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void comp_main(uint3 invocation : SV_DispatchThreadID)
{
    const int2 extent = int2(pc.extents.xy);
    const int2 pixel = int2(invocation.xy);
    if (pixel.x >= extent.x || pixel.y >= extent.y)
    {
        return; // the dispatch is rounded up to the workgroup size
    }
    const float2 uv = (float2(pixel) + 0.5) / float2(extent);

    RWTexture2D<float4> resolved_image = heap_at<RWTexture2D<float4>>(heap_slots_ml_resolved_storage + heap_image_index);

    const float depth_here = ml_texel(heap_slots_gbuffer_depth, uv).r;
    // The estimate this frame contributes is the SPATIALLY filtered one: 0 disables it and leaves this line
    // exactly what it was.
    const float3 current = spatial_mean(uv, view_depth(depth_here), pc.extents.z);
    if (depth_here >= 1.0)
    {
        // Background: the trace wrote 0 here, and so does this - with a count of 0, so that a pixel that gets
        // geometry later starts its own accumulation rather than inheriting the sky's.
        resolved_image[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    // ---- the reprojection and its rejection ----
    const float2 motion = ml_texel(heap_slots_gbuffer_velocity, uv).rg;
    const float2 previous_uv = uv - motion;
    const float view_here = view_depth(depth_here);
    float frames = 0.0;
    float3 history = float3(0.0, 0.0, 0.0);
    if (previous_uv.x >= 0.0 && previous_uv.x <= 1.0 && previous_uv.y >= 0.0 && previous_uv.y <= 1.0)
    {
        const float4 fetched = ml_texel(heap_slots_ml_history, previous_uv);
        frames = fetched.a; // the frame count, which is 0 for a pixel with no accumulation
        const float previous_depth = ml_texel(heap_slots_gbuffer_depth, previous_uv).r;
        // The grazing widening of UE's test needs a surface normal, and this pass reads none: a constant 1
        // divides the tolerance by the loose factor, i.e. UE's test with its `lerp` fixed at its loosest - the
        // CONSERVATIVE direction, which costs history rather than granting a stale one.
        const float tolerance = max(previous_depth < 1.0 ? pc.params.w * view_here : -1.0, -1.0);
        const bool depth_agrees = previous_depth < 1.0 && abs(view_depth(previous_depth) - view_here) <= tolerance;
        if (frames > 0.0)
        {
            history = fetched.rgb;
            if (!depth_agrees)
            {
                // A SOFT REJECTION: the pixel keeps a short average (which damps a per-frame flip) while
                // staying responsive, and the clamp below bounds how far a history from the other surface may
                // pull it. The hard version restarted the accumulation every frame at exactly the pixels where
                // TAA's jitter matters most - see the GLSL file for the measurement.
                frames = min(frames, ML_DISAGREE_FRAMES);
            }
        }
        else
        {
            frames = 0.0;
            history = float3(0.0, 0.0, 0.0);
        }
    }

    // ---- the neighbourhood statistics over THIS frame's trace ----
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 sum_squared = float3(0.0, 0.0, 0.0);
    float taps = 0.0;
    // THE TAP STRIDE IS THIS PASS's texel: the full-resolution one was a bug the spatial pre-filter made
    // visible (a 3x3 at that stride took its taps a quarter of a texel apart, which is not a neighbourhood).
    const float2 texel = float2(1.0, 1.0) / pc.extents.xy;
    for (int y = -ML_CLAMP_RADIUS; y <= ML_CLAMP_RADIUS; ++y)
    {
        for (int x = -ML_CLAMP_RADIUS; x <= ML_CLAMP_RADIUS; ++x)
        {
            const float2 tap_uv = uv + float2(float(x), float(y)) * texel;
            const float tap_depth = ml_texel(heap_slots_gbuffer_depth, tap_uv).r;
            if (tap_depth >= 1.0)
            {
                continue; // a background tap holds no estimate
            }
            const float3 tap = ml_texel(heap_slots_ml_trace, tap_uv).rgb;
            sum += tap;
            sum_squared += tap * tap;
            taps += 1.0;
        }
    }
    // A mean/stddev box, expanded to contain the centre (which the taps may have rejected), and then the
    // accumulated history pulled into it - UE's `mean +/- clampScale * stddev` with a clampScale of 2.
    float3 box_min = current;
    float3 box_max = current;
    if (taps > 0.0)
    {
        const float3 mean = sum / taps;
        const float3 variance = max(sum_squared / taps - mean * mean, float3(0.0, 0.0, 0.0));
        box_min = min(box_min, mean - 2.0 * sqrt(variance));
        box_max = max(box_max, mean + 2.0 * sqrt(variance));
    }
    history = clamp(history, box_min, box_max);

    // ---- the running mean ----
    const float cap = max(pc.params.z, 1.0);
    const float next_frames = min(frames + 1.0, cap);
    const float alpha = 1.0 / next_frames;
    float3 resolved = mix(history, current, alpha);
    // A NaN or an infinity anywhere in the chain (a degenerate light, a zero-length ray) must not be
    // accumulated: it would poison the pixel for as long as the history survives.
    if (any(isnan(resolved)) || any(isinf(resolved)))
    {
        resolved = current;
        resolved_image[pixel] = float4(0.0, 0.0, 0.0, 0.0); // ... and the accumulation restarts
        return;
    }
    resolved_image[pixel] = float4(max(resolved, float3(0.0, 0.0, 0.0)), next_frames);
}
