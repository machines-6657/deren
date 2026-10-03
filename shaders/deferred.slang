// shaders/deferred.slang - the SLANG source of the DEFERRED lighting stage.
//
// PRODUCES `deferred.frag.spv`, the same name the GLSL version produced, so the host side is untouched.
// Ported from shaders/deferred.frag (now in `shaders/glsl.old/`). It reads the G-buffer, rebuilds
// the world position from the stored depth, and evaluates the SAME lighting the forward path evaluates per
// fragment - literally the same code, because shaders/shading.glsl owns the sun, the shadow test, the
// punctual lights, the split-sum IBL and the BRDF presets, and this stage only supplies a shade_input
// assembled from G-buffer texels instead of from interpolated vertex data. It also owns the screen-space
// ambient occlusion, folded into that shade_input's `ao`.
//
// WHAT CHANGED IN THE PORT is only what the language requires:
//   - `[[vk::location(n)]]` / `SV_Target` in place of the layout qualifiers;
//   - `gl_FragCoord.xy` becomes the `SV_Position` parameter, and it is passed into the SSAO helper rather
//     than read there (the same value, and the helper stays a pure function of its arguments);
//   - the G-buffer fetch helper takes the texture HANDLE instead of a heap slot: the GLSL version has to
//     pass a slot because a `texture2D` parameter does not survive a function boundary in glslang, while
//     Slang can hand the handle to the shim's `heap_texel`;
//   - the heap is reached through the shared accessors.
//
// THE GLSL TYPE NAMES ARE KEPT ON PURPOSE (`vec3`, `mat3`, `ivec2`): under `-allow-glsl` Slang maps them
// onto its own types AND keeps GLSL's constructor meaning, which matters for `mat3(tangent, bitangent,
// view_normal)` - GLSL builds COLUMNS from those three vectors where HLSL's `float3x3(a, b, c)` builds
// ROWS, so a transposed TBN is what a "modernized" spelling would have risked. Verified rather than
// assumed: the shared surface.glsl builds its normal-map TBN exactly this way and the Slang-built,
// gate-green gbuffer.frag and pbr.frag both emit `OpCompositeConstruct %mat3v3float` followed by
// `OpMatrixTimesVector`.
//
// THE INCLUDE ORDER IS PART OF THE CONTRACT: the shim, then sky.glsl (pure math), then this stage's own
// declarations, and ONLY THEN shading.glsl - whose functions address the per-frame arrays through the slot
// macros, which expand to `pc.frame_slot`, so `pc` has to exist first. That is the one ordering rule this
// pattern has.

#include "heap_access.slang"
#include "sky.glsl"

struct DeferredPush
{
    VR_MAT4 inv_view_proj; // clip (NDC xyz, w = 1) -> world position
    // Screen-space ambient occlusion: x = world-space radius, y = intensity (0 = off), z = sample count,
    // w = depth bias. An intensity <= 0 makes the term return exactly 1.0, which is what keeps an SSAO-off
    // frame bit for bit the pre-SSAO frame.
    float4 ssao;
    // 1.0 = "unlit" render mode: write the stored albedo, unshaded (see runtime::set_unlit)
    float unlit;
    // 1.0 = the stochastic punctual lighting pass ANSWERED this frame, so the cluster loop inside
    // shade_surface must not add the punctual lights a second time (this stage adds `ml_lighting` instead).
    // It is set from whether that pass actually recorded, not from its knob - a frame whose pass was gated
    // off keeps the raster loop, which is what makes the two paths exclusive rather than complementary.
    float punctual_replaced;
    // THE HEAP INDICES, LAST on purpose so every field above keeps its offset.
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<DeferredPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

#include "shading.glsl"

/// @brief one G-buffer texel through the set's NEAREST sampler
/// @param image the heap image, indexed by its slot (heap_slots_gbuffer_x + heap_image_index)
/// @note the GLSL version takes a SLOT instead, and that is measured rather than stylistic: a `texture2D`
///       parameter does not survive a function boundary in glslang. Slang has no such limit.
float4 gbuffer_texel(DescriptorHandle<Texture2D<float4>> image, float2 uv)
{
    return heap_texel(image, heap_sampler_gbuffer, uv);
}

/// @brief rebuild the world-space position of the pixel from the stored depth
/// @note the engine's projection is a RH_ZO matrix with its Y row flipped for Vulkan, and post.vert's v_uv
///       already maps (0,0) to the top-left texel, so the NDC of a pixel is (2*uv - 1) in both axes: no
///       extra flip belongs here - the inverse matrix already carries the projection's own conventions.
float3 world_position_from_depth(float2 uv, float depth)
{
    const vec4 world = pc.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

/// @brief AO sample budget: the shader loop bound (the runtime's ssao_samples is clamped to it)
const int MAX_SSAO_SAMPLES = 16;

/// @brief screen-space ambient occlusion of the pixel (hemisphere samples against the depth buffer)
/// @param uv the pixel's texture coordinate (v_uv)
/// @param depth the G-buffer depth at @p uv
/// @param world_normal the G-buffer world normal at @p uv
/// @param frag_coord the pixel coordinate (SV_Position.xy, GLSL's gl_FragCoord.xy) for the per-pixel rotation
/// @return 1.0 = fully unoccluded (also when SSAO is off), 0.0 = fully occluded
/// @note the classic screen-space AO: a hemisphere of sample points around the pixel's view-space position,
///       each projected back to screen and compared against the depth buffer. A sample counts as occluded
///       when the surface stored at its screen position is CLOSER to the camera and inside the radius -
///       view-space z is negative in front of the camera, so "closer" means a greater z. The kernel is a
///       golden-angle hemisphere spiral rotated per pixel by a hash of the pixel coordinate: a fixed kernel
///       would show the sample pattern as spiral banding.
/// @note this is deliberately the affordable end of screen-space AO - not a horizon-based GTAO - and it
///       scales the IBL ambient only: `shade_surface()` applies `s.ao` to the diffuse and specular ambient
///       and never to the direct sun.
float ssao_occlusion(float2 uv, float depth, vec3 world_normal, float2 frag_coord)
{
    const float radius = pc.ssao.x;
    const float intensity = pc.ssao.y;
    const int samples = int(pc.ssao.z + 0.5);
    if (radius <= 0.0 || intensity <= 0.0 || samples <= 0)
    {
        return 1.0; // off: an exact 1.0 leaves the shaded result untouched
    }
    const vec3 world_pos = world_position_from_depth(uv, depth);
    const vec3 view_pos = (camera_at(heap_camera_slot).view * vec4(world_pos, 1.0)).xyz;
    // the G-buffer normal is world space; the hemisphere must be built around its VIEW-space form
    const vec3 view_normal = normalize((camera_at(heap_camera_slot).view * vec4(world_normal, 0.0)).xyz);

    // per-pixel rotation: hash the pixel coordinate into an angle (the classic fract(sin(dot(..))))
    const float hash = fract(sin(dot(frag_coord, vec2(12.9898, 78.233))) * 43758.5453);
    const float angle = hash * 6.2831853;
    const vec3 random_vec = vec3(cos(angle), sin(angle), 0.0);
    const vec3 tangent = normalize(random_vec - view_normal * dot(random_vec, view_normal));
    const vec3 bitangent = cross(view_normal, tangent);
    const mat3 tbn = mat3(tangent, bitangent, view_normal);

    float occlusion = 0.0;
    for (int i = 0; i < MAX_SSAO_SAMPLES; ++i)
    {
        if (i >= samples)
        {
            break;
        }
        // golden-angle spiral in the hemisphere: cos/sin over the projected disc, z = the height
        const float t = (float(i) + 0.5) / float(samples);
        const float disc_radius = sqrt(t);
        const float phi = float(i) * 2.39996323;
        const vec3 kernel = tbn * vec3(cos(phi) * disc_radius, sin(phi) * disc_radius, sqrt(max(1.0 - t, 0.0)));
        // samples closer to the pixel count more: scale the kernel towards the origin
        const vec3 sample_pos = view_pos + kernel * (radius * (0.2 + 0.8 * t));

        const vec4 clip = camera_at(heap_camera_slot).proj * vec4(sample_pos, 1.0);
        const vec2 sample_uv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (sample_uv.x < 0.0 || sample_uv.x > 1.0 || sample_uv.y < 0.0 || sample_uv.y > 1.0)
        {
            continue; // outside the frame: no depth to compare against (the screen-space limit)
        }
        const float sample_depth = gbuffer_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), sample_uv).r;
        if (sample_depth >= 1.0)
        {
            continue; // sky there: not an occluder
        }
        const vec3 scene_pos = world_position_from_depth(sample_uv, sample_depth);
        const float scene_z = (camera_at(heap_camera_slot).view * vec4(scene_pos, 1.0)).z;
        // occluded when the stored surface sits IN FRONT of the sample point (greater z = closer) ...
        // ... and the range check fades the contact out over the radius instead of cutting it hard
        const float range = clamp(radius / max(abs(view_pos.z - scene_z), 1e-5), 0.0, 1.0);
        occlusion += (scene_z >= sample_pos.z + pc.ssao.w) ? range : 0.0;
    }
    occlusion /= float(samples);
    return clamp(1.0 - occlusion * intensity, 0.0, 1.0);
}

/// @brief shade the pixel described by the G-buffer, writing LINEAR HDR radiance into the HDR target
/// @note the target already holds the emissive term (added by the G-buffer pass) and is zero where no
///       geometry was drawn - which is why this stage is free to write the SKY there and the deferred path
///       needs no background pass at all.
[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_uv : TEXCOORD0, float4 sv_position : SV_Position) : SV_Target
{
    const float depth = gbuffer_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), v_uv).r;
    if (depth >= 1.0)
    {
        // No geometry here: this pixel shows the sky. The far-plane point along the pixel's ray gives the
        // view direction (the reconstructed position is exact even at depth 1), and the sky function is the
        // forward skybox pass's - so the two paths cannot disagree about the background.
        const vec3 far_point = world_position_from_depth(v_uv, 1.0);
        // THE SUN COMES FROM THE LIGHT UBO, not from a constant in the sky: `sky.glsl` used to carry its own
        // hard-coded `(0.3, 1.0, 0.5)`, which is a second place for one sun to live and would put the visible
        // disc somewhere other than the direction the shadows are cast from the moment `[lighting]
        // sun_direction` moved.
        return float4(sky_color(normalize(far_point - camera_at(heap_camera_slot).camera_pos),
                                normalize(light_at(heap_light_slot).light_dir.xyz)), 1.0);
    }

    const vec4 albedo_metallic = gbuffer_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_albedo + heap_image_index), v_uv);
    // "unlit" render mode: the forward path draws this geometry with the flat unlit pipeline, so the
    // deferred path must produce the same thing - the base color with no lighting, no shadows, no IBL and
    // no AO (the sky above is still the shared sky, exactly as on the forward path).
    if (pc.unlit > 0.5)
    {
        return float4(albedo_metallic.rgb, 1.0);
    }
    const vec4 normal_roughness = gbuffer_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index), v_uv);
    const vec4 material = gbuffer_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_material + heap_image_index), v_uv);

    shade_input si;
    si.pixel = ivec2(sv_position.xy); // the cluster grid's tile coordinate (see shade_input)
    si.world_pos = world_position_from_depth(v_uv, depth);
    si.normal = normal_roughness.xyz;
    si.albedo = albedo_metallic.rgb;
    si.metallic = albedo_metallic.a;
    si.roughness = normal_roughness.w;
    // ambient occlusion: the material's baked AO map times the screen-space term. With SSAO off
    // ssao_occlusion() returns exactly 1.0, so this is the pre-SSAO value bit for bit. It scales the IBL
    // ambient (diffuse and specular) and never the direct sun.
    si.ao = material.b * ssao_occlusion(v_uv, depth, si.normal, sv_position.xy);
    si.emissive = vec3(0.0);
    // Ray-traced sun visibility, or NEGATIVE to keep the cascaded shadow maps: the flag is the light UBO's,
    // and it is only ever set when that pass ran and the device has ray queries - so a frame with rt_shadows
    // off samples nothing that does not exist and shades exactly as it did before.
    si.shadow_override = (light_at(heap_light_slot).rt_shadows > 0.5) ? heap_texel(heap_at<Texture2D<float4>>(heap_slots_rt_visibility + heap_frame_slot), heap_sampler_gbuffer, v_uv).r : -1.0;
    // ... and the punctual lights' own switch: with the stochastic pass having answered this frame, the
    // cluster loop inside shade_surface adds nothing and `ml_lighting` is added instead, right below.
    si.punctual_replaced = pc.punctual_replaced;

    vec3 color = shade_surface(si);

    // ---- the stochastic punctual lighting ----
    // Added AFTER the surface is shaded, because it is a lighting term rather than a property of the
    // surface: the stochastic pass evaluated the same BRDF from the same G-buffer, so this is the same
    // quantity the cluster loop would have produced - with a visibility ray per sample instead of no shadow
    // at all. THE UPSAMPLE IS A 2x2 JOINT-BILATERAL GATHER: a half-resolution texel's value belongs to the
    // surface it was computed FOR, so a tap only counts if it agrees on view depth and on normal. The
    // tolerance and the normal exponent are literals here rather than push lanes - unlike the composite's,
    // this gather has no A/B measurement behind it yet, and inventing knobs before the measurement is how a
    // knob ends up with a value nobody can justify.
    if (pc.punctual_replaced > 0.5)
    {
        const DescriptorHandle<Texture2D<float4>> ml_lighting = heap_at<Texture2D<float4>>(heap_slots_ml_resolved + heap_image_index);
        const vec2 ml_extent = heap_size(ml_lighting, 0u);
        const vec2 ml_texel = 1.0 / ml_extent;
        const vec2 base = floor(v_uv * ml_extent - 0.5);
        const float view_here = -length(si.world_pos - camera_at(heap_camera_slot).camera_pos.xyz);
        // The tolerance is relative to the pixel's own view distance: the same absolute depth error means
        // far less at 5 units than at 50 (the argument the chain's filters make).
        const float depth_tolerance = max(0.02 * abs(view_here), 1e-5);
        vec3 ml_sum = vec3(0.0);
        float ml_weight_sum = 0.0;
        for (int y = 0; y <= 1; ++y)
        {
            for (int x = 0; x <= 1; ++x)
            {
                const vec2 tap_uv = (clamp(base + vec2(float(x), float(y)), vec2(0.0, 0.0), ml_extent - 1.0) + 0.5) * ml_texel;
                const float tap_depth = gbuffer_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), tap_uv).r;
                if (tap_depth >= 1.0)
                {
                    continue; // a background tap holds no lighting and has no view depth to compare
                }
                const vec3 tap_world = world_position_from_depth(tap_uv, tap_depth);
                const float tap_view = -length(tap_world - camera_at(heap_camera_slot).camera_pos.xyz);
                const float depth_weight = exp(-abs(tap_view - view_here) / depth_tolerance);
                const vec3 tap_normal = gbuffer_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index), tap_uv).xyz;
                const float normal_weight = pow(max(dot(si.normal, tap_normal), 0.0), 16.0);
                const float weight = depth_weight * normal_weight;
                ml_sum += heap_texel(ml_lighting, heap_sampler_gbuffer, tap_uv).rgb * weight;
                ml_weight_sum += weight;
            }
        }
        // The fallback is the filtered fetch at the pixel's own uv, which is what a sliver of geometry whose
        // whole 2x2 block belongs to somebody else gets - the same answer the composite's upsample gives.
        color += ml_weight_sum > 1e-5 ? ml_sum / ml_weight_sum : heap_texel(ml_lighting, heap_sampler_gbuffer, v_uv).rgb;
    }

    return float4(color, 1.0);
}
