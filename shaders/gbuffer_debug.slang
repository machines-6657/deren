// shaders/gbuffer_debug.slang - the SLANG source of the G-buffer debug view.
//
// PRODUCES `gbuffer_debug.frag.spv`, the same name the GLSL version produced, so the host is untouched.
// Ported from shaders/gbuffer_debug.frag: the channel selection, the depth linearization and the motion
// amplification are the GLSL code; the heap reads go through the shared shim (see docs/slang_migration.md).
//
// The view runs through the ordinary post chain (exposure -> ACES -> display encode), so the displayed
// brightness is not the stored value - the point is structure (where a channel is right, wrong or
// missing), not reading numbers back off the screen. The motion channel is what the deformation tests use:
// `motion * gain + 0.5` with the CPU's gain, so "did not move" is one flat value and any movement shifts
// toward red or green by direction (see scripts/measure/motion_channel.py).

#include "heap_access.slang"

struct GbufferDebugPush
{
    float channel;     // 0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 id, 6 depth, 7 flags, 8 motion
    float proj_22;     // projection[2][2]: the depth-linearization terms (see view_depth)
    float proj_32;     // projection[3][2]
    float motion_gain; // channel 8's amplification (the CPU passes width/4: four pixels saturate)
    uint frame_slot;   // THE HEAP INDICES, at the END so every field above keeps its offset
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<GbufferDebugPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

/// @brief view-space distance of a depth-buffer sample
/// @note for the standard perspective projection the CPU builds, the stored depth is
///       z_ndc = (proj_22 * z_view + proj_32) / -z_view, which inverts to z_view = proj_32 / (z_ndc + proj_22).
float view_depth(float depth)
{
    return pc.proj_32 / (depth + pc.proj_22);
}

/// @brief a stable pseudo-random color per material id (neighbouring ids must not look alike)
float3 material_id_color(uint id)
{
    return float3(float((id * 37u) % 251u), float((id * 91u) % 251u), float((id * 53u) % 251u)) / 250.0;
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_uv : TEXCOORD0) : SV_Target
{
    // the four stored targets plus the velocity, each reached from its own heap slot: this pass VIEWS the
    // stored surface, so it samples through the G-buffer's NEAREST sampler (filtering it would show a
    // surface that was never stored)
    const float4 albedo_metallic = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_albedo + heap_image_index), heap_sampler_gbuffer, v_uv);
    const float4 normal_roughness = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index), heap_sampler_gbuffer, v_uv);
    const float4 material = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_material + heap_image_index), heap_sampler_gbuffer, v_uv);
    // The cleared depth is the far plane: those pixels hold no geometry, so every channel shows them
    // black. Without this the normal channel would paint them mid-grey, which reads like a strangely lit
    // surface rather than like empty space.
    const float stored_depth = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), heap_sampler_gbuffer, v_uv).r;
    const bool background = stored_depth >= 1.0;

    const uint channel = uint(pc.channel + 0.5);
    float3 color;
    if (channel == 6u)
    {
        // near = white, far = black; the cleared background is the far plane
        const float far_plane = pc.proj_32 / (1.0 + pc.proj_22);
        color = float3(1.0 - clamp(view_depth(stored_depth) / far_plane, 0.0, 1.0));
    }
    else if (background)
    {
        color = float3(0.0);
    }
    else if (channel == 0u)
    {
        color = albedo_metallic.rgb;
    }
    else if (channel == 1u)
    {
        color = normal_roughness.xyz * 0.5 + 0.5; // world normal -> visible color
    }
    else if (channel == 2u)
    {
        color = float3(normal_roughness.w);
    }
    else if (channel == 3u)
    {
        color = float3(albedo_metallic.a);
    }
    else if (channel == 4u)
    {
        color = float3(material.b);
    }
    else if (channel == 5u)
    {
        const uint id = uint(round(material.r * 255.0)) | (uint(round(material.g * 255.0)) << 8u);
        color = material_id_color(id);
    }
    else if (channel == 8u)
    {
        // Motion vector, amplified: one pixel of motion at 1080 wide is 0.00093, so the raw value is black
        // everywhere and tells you nothing. The +0.5 bias makes "did not move" flat olive while movement
        // shifts toward red or green by direction.
        const float2 motion = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_velocity + heap_image_index), heap_sampler_gbuffer, v_uv).rg;
        color = float3(clamp(motion * pc.motion_gain + 0.5, 0.0, 1.0), 0.0);
    }
    else
    {
        color = float3(material.a); // raw material flag byte
    }
    return float4(color, 1.0);
}
