// shaders/taa.frag -> shaders/taa.slang: the TAA resolve, ported to Slang.
//
// PRODUCES `taa.frag.spv`, the same name the GLSL version produced, so the host is untouched. The resolve
// is the GLSL resolve - reproject the history at `uv - velocity`, clamp it into the current frame's 3x3
// YCoCg neighbourhood, reject what a view-depth difference says is another surface, then blend with a
// weight that falls off as the pixel moves. Only the heap reads go through the shared shim
// (see docs/slang_migration.md).
//
// THE PUSH BLOCK'S TEXEL SIZE STAYS TWO SCALARS (see the GLSL's note): a float2 aligns to 8 bytes in the
// block, which would insert padding the C++ struct does not have.

#include "heap_access.slang"

struct TaaPush
{
    // 1 = trust the history (the normal case), 0 = use the current frame only (the first frame for this
    // image, or a swapchain recreation: the history image holds nothing meaningful then)
    float history_valid;
    float blend_static; // history weight for a static pixel (0.9 = 10% of the current frame each frame)
    float blend_min;    // history weight floor under motion
    float texel_size_x; // 1 / target width  (the 3x3 neighbourhood + the depth guard's radius)
    float texel_size_y; // 1 / target height
    float depth_scale;  // the projection's depth-linearization terms, for the disocclusion guard
    float depth_offset;
    float _pad;
    uint frame_slot;    // THE HEAP INDICES, at the END so every field above keeps its offset
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<TaaPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

/// @brief the target's texel size as a vector
float2 texel_size()
{
    return float2(pc.texel_size_x, pc.texel_size_y);
}

/// @brief linear RGB -> YCoCg (the space the neighbourhood clamp is computed in)
float3 rgb_to_ycocg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

/// @brief YCoCg -> linear RGB
float3 ycocg_to_rgb(float3 c)
{
    const float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

/// @brief view-space distance of a depth-buffer sample (same inversion as gbuffer_debug)
float view_depth(float raw_depth)
{
    return pc.depth_offset / (raw_depth + pc.depth_scale);
}

/// @brief resolve one pixel: clamp the reprojected history into the current neighbourhood and blend
float3 resolve(float2 uv)
{
    const DescriptorHandle<Texture2D<float4>> current = heap_at<Texture2D<float4>>(heap_slots_taa_current + heap_image_index);
    const DescriptorHandle<Texture2D<float4>> motion_image = heap_at<Texture2D<float4>>(heap_slots_gbuffer_velocity + heap_image_index);
    const DescriptorHandle<Texture2D<float4>> history_image = heap_at<Texture2D<float4>>(heap_slots_taa_history + heap_image_index);
    const DescriptorHandle<Texture2D<float4>> depth_image = heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index);

    const float3 center = heap_texel(current, heap_sampler_taa, uv).rgb;
    const float2 motion = heap_texel(motion_image, heap_sampler_taa, uv).rg;

    // ---- the current frame's local range, computed in YCoCg ----
    // A per-channel RGB min/max box lets one channel's outlier widen the other two: on a surface whose luma
    // is aliasing (a specular highlight, a normal-mapped edge) the chroma is dragged along with it, and the
    // history is then clamped in a direction it never moved. YCoCg decorrelates luma from the two chroma
    // axes, so the box is tight where the signal is quiet and wide only where it truly moves.
    float3 neighborhood_min = rgb_to_ycocg(center);
    float3 neighborhood_max = neighborhood_min;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const float3 sample_color = rgb_to_ycocg(heap_texel(current, heap_sampler_taa, uv + float2(float(x), float(y)) * texel_size()).rgb);
            neighborhood_min = min(neighborhood_min, sample_color);
            neighborhood_max = max(neighborhood_max, sample_color);
        }
    }

    // ---- reproject the history ----
    const float2 history_uv = uv - motion;
    // outside the frame there is no history: whatever the clamp below does with the border texel would be a
    // guess, so fall back to the current frame entirely
    if (history_uv.x < 0.0 || history_uv.x > 1.0 || history_uv.y < 0.0 || history_uv.y > 1.0)
    {
        return center;
    }
    float3 history = heap_texel(history_image, heap_sampler_taa, history_uv).rgb;

    // ---- disocclusion guard: is the reprojected texel the same surface? ----
    // The motion vector alone cannot tell: at a silhouette the previous frame's pixel at that location
    // showed the background, and blending it in is the classic TAA ghost trail. Comparing the two view
    // depths (with a relative tolerance that scales with distance) rejects those.
    const float current_depth = view_depth(heap_texel(depth_image, heap_sampler_taa, uv).r);
    const float history_depth = view_depth(heap_texel(depth_image, heap_sampler_taa, history_uv).r);
    const float depth_tolerance = max(0.02 * current_depth, 0.1); // metres-ish, scale-free enough
    if (abs(current_depth - history_depth) > depth_tolerance)
    {
        return center;
    }

    // ---- rejection: pull a stale history value into what this pixel looks like now ----
    history = ycocg_to_rgb(clamp(rgb_to_ycocg(history), neighborhood_min, neighborhood_max));

    // ---- blend, trusting the history less the faster the pixel moves ----
    const float speed = length(motion / texel_size()); // motion in pixels
    const float history_weight = mix(pc.blend_static, pc.blend_min, clamp(speed, 0.0, 1.0)) * pc.history_valid;
    return mix(center, history, history_weight);
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_uv : TEXCOORD0) : SV_Target
{
    return float4(resolve(v_uv), 1.0);
}
