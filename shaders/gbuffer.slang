// shaders/gbuffer.slang - the SLANG source of the G-buffer write stage.
//
// PRODUCES `gbuffer.frag.spv`, the same name the GLSL version produced, so the host side is untouched.
// Ported from shaders/gbuffer.frag (now in `shaders/glsl.old/`): the body is the GLSL body, and
// what changes is only what a different language requires - the entry point's parameters and the output
// struct carry `[[vk::location(n)]]` instead of `layout(location = n)`, and the heap is reached through
// the shared accessors (see docs/slang_migration.md).
//
// THE INCLUDE ORDER IS PART OF THE CONTRACT: the shim first, then surface.glsl (which declares the push
// constant block and the heap_frame_slot / heap_image_index aliases that the slot macros expand against),
// then the rest of the shared bodies. Compiling a body before surface.glsl reports heap_frame_slot as an
// undefined identifier - measured.

#include "heap_access.slang"
#include "surface.glsl"
#include "shading.glsl"

struct gbuffer_output
{
    [[vk::location(0)]] float4 albedo_metallic;  // rgb albedo, a metallic
    [[vk::location(1)]] float4 normal_roughness; // xyz world normal, w roughness
    [[vk::location(2)]] float4 material;         // r/g material id, b ao, a flags
    [[vk::location(3)]] float2 velocity;         // motion vector in UV space (current - previous)
    [[vk::location(4)]] float4 scene_color;      // the EMISSIVE term, ADDED into the scene color
};

/// @brief motion vector of this fragment in UV space: where it was last frame, relative to here
/// @note both matrices are the JITTER-FREE pair (the TAA jitter lives in `proj`; a jitter that leaked in
///       here would be read as camera motion), and the previous position is projected with
///       `prev_view_proj` alone because the object's own motion is already baked into it.
/// @note this covers camera motion, rigid object motion and a DEFORMING mesh (see
///       docs/deformation_motion_vectors.md); alpha-blended geometry is not covered, because it is
///       composited outside the G-buffer and writes no velocity at all.
float2 motion_vector(float3 world_pos, float3 prev_world_pos)
{
    // `M * v`, NOT `mul(M, v)`: MEASURED, and the opposite of what the HLSL habit suggests. For a
    // `row_major float4x4` (the spelling that emits the ColMajor layout the host's glm::mat4 has),
    // Slang's `mul(M, v)` compiles to OpVectorTimesMatrix - the TRANSPOSED product - while the GLSL
    // operator keeps GLSL's column-vector meaning under `-allow-glsl` and compiles to exactly the
    // OpMatrixTimesVector glslc emits for gbuffer.frag. Getting this backwards is not loud: the
    // matrix still loads from the right bytes (the member offsets and MatrixStride were verified equal)
    // and only the velocity comes out wrong, which showed up as a black motion channel. See
    // docs/slang_migration.md section 9, "the first leaf port: two traps, both measured, both fixed".
    const float4 current_clip = camera_at(heap_camera_slot).view_proj_unjittered * float4(world_pos, 1.0);
    const float4 previous_clip = camera_at(heap_camera_slot).prev_view_proj * float4(prev_world_pos, 1.0);
    const float2 current_uv = (current_clip.xy / current_clip.w) * 0.5 + 0.5;
    const float2 previous_uv = (previous_clip.xy / previous_clip.w) * 0.5 + 0.5;
    return current_uv - previous_uv;
}

/// @brief write the fragment's surface into the G-buffer targets
/// @note no lighting, no tonemapping, no output alpha semantics: what the surface carries and lighting
///       does not is stored as-is, and the lighting stage decides what to do with it.
[shader("fragment")]
gbuffer_output main(
    [[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
    [[vk::location(1)]] float3 v_normal : TEXCOORD1,
    [[vk::location(2)]] float2 v_uv : TEXCOORD2,
    [[vk::location(3)]] float3 v_prev_world_pos : TEXCOORD3)
{
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv);

    gbuffer_output out;
    // the world normal needs no encoding in an RGBA16F target (see the GLSL file's docs)
    out.albedo_metallic = float4(s.albedo, s.metallic);
    out.normal_roughness = float4(s.normal, s.roughness);

    // 16-bit material id split over two UNORM channels: k / 255 is exactly representable, so
    // round(255 * stored) returns the byte that was written even after the format's conversion
    const uint id = push.material_index;
    out.material = float4(
        float(id & 0xFFu) / 255.0,
        float((id >> 8u) & 0xFFu) / 255.0,
        s.ao,
        float(s.flags & 0xFFu) / 255.0);

    out.velocity = motion_vector(v_world_pos, v_prev_world_pos);

    // The emissive term goes STRAIGHT into the scene color (the attachment blends ONE/ONE), because
    // emissive is lighting-independent and needs the material's emissive texture and this fragment's UVs -
    // neither of which the G-buffer stores, so the lighting stage could not reconstruct it afterwards.
    out.scene_color = float4(s.emissive, 1.0);
    return out;
}
