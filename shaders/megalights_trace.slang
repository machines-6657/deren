// shaders/megalights_trace.slang - the SLANG source of the stochastic punctual lighting tracer.
//
// PRODUCES `megalights_trace.comp.spv`, the same name the GLSL version produced, so the host is untouched.
// Ported from shaders/megalights_trace.comp.
//
// WHAT IT EXISTS FOR, unchanged by the port: every punctual light in this engine is unshadowed - the cluster
// loop in shading.glsl accumulates a light's radiance with no visibility term at all, so a demo light behind a
// column lights the wall in front of it. The fix is not one shadow ray per light per pixel (that cost grows
// with the light count) but the estimator Unreal's MegaLights uses: each pixel picks a few lights
// STOCHASTICALLY, with a probability proportional to how much each light contributes, and traces one ray per
// pick. The estimator, in order: the pixel's clustered light list as the candidate set; a target PDF that is a
// LOG-compressed shaded luminance (a light a hundred times brighter buys about seven times the samples rather
// than a hundred) times UE's `SmoothFalloffMask`; a weighted reservoir with one reservoir per sample,
// stratified by a single per-pixel uniform; one ray per selected sample with the engine's self-intersection
// bias, shrinking as the light approaches the normal; a FIREFLY CLAMP on the estimator weight; and an optional
// emitter ANGULAR radius that turns the binary visibility answer into a soft shadow edge (0 keeps the old
// hard-shadow behaviour exactly - see the GLSL file for the flicker measurement that motivated it).
//
// WHAT IT DOES NOT DO YET, stated rather than discovered later: it writes the RAW estimate - no temporal
// accumulation (that is the NEXT pass, `megalights_temporal.slang`), no spatial filter, no history-guided
// sampling, and the sample's own surface is the half-resolution texel's. The alpha lane carries the FRACTION OF
// SAMPLES THAT FOUND THE LIGHT UNBLOCKED, not a radiance: the cheapest confidence signal the resolve can have.
//
// PORT DELTAS, all forced and all value-neutral here:
//   - every fetch is the shim's EXPLICIT-LOD form: this is a compute stage whose fetches sit after an early
//     return (the background case), which Slang will not accept for an implicit fetch (TRAP 4). Every image
//     read here is a single-mip G-buffer target, so LOD 0 IS the mip the implicit fetch resolved to;
//   - `textureSize` is the shim's `heap_size`, and the output is an RWTexture2D behind the STORAGE slot the
//     grid reserves for that image;
//   - the ray query is Slang's `RayQuery<flags>`: `TraceRayInline` + `Proceed` + `CommittedStatus` replace
//     `rayQueryInitializeEXT` / `rayQueryProceedEXT` / `rayQueryGetIntersectionTypeEXT`, and
//     `RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH` is what GLSL calls `gl_RayFlagsTerminateOnFirstHitEXT`.

#include "heap_access.slang"

struct MlPush
{
    VR_MAT4 inv_view_proj; // clip -> world, the same reconstruction the lighting stage and rt_shadow use
    // x = samples per pixel (1..ML_MAX_SAMPLES), y = the minimum sample weight (UE's r.MegaLights.MinSampleWeight),
    // z = the ray tmin (the engine's fixed self-intersection guard), w = a per-frame counter so the sample
    // sequence differs every frame.
    float4 params;
    // x = the normal-offset floor, y = the offset at grazing incidence, z = the emitter's ANGULAR radius
    // (0 = a point light and hard shadows, and 0 is the DEFAULT), w unused.
    float4 bias;
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<MlPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

// ... and the scene-set header, INCLUDED ONLY NOW: its functions read the per-frame arrays through the slot
// macros, which expand to `pc.frame_slot`, so `pc` has to exist first.
#include "shading.glsl"

/// @brief the compile-time bound on samples per pixel: UE supports 1, 2 and 4, and the loop is bounded by the
///        constant so the reservoir arrays have a size (the ACTIVE count is a push lane).
const int ML_MAX_SAMPLES = 4;
/// @brief UE's `r.MegaLights.MaxShadingWeight`: how much a single sample may contribute on its own, which is
///        what keeps one unblocked sample of an occluded-but-bright light from writing a firefly.
const float ML_MAX_SHADING_WEIGHT = 20.0;

/// @brief a uniform in [0,1) from an integer state (the same PCG-style hash the GI tracer uses)
uint hash_u32(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// @brief the next uniform in [0,1) from @p state
float next_uniform(inout uint state)
{
    state = hash_u32(state);
    return float(state) * (1.0 / 4294967296.0);
}

/// @brief world position of a depth-buffer sample (the lighting stage's own reconstruction)
float3 world_position_from_depth(float2 uv, float depth)
{
    const float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    const float4 world = pc.inv_view_proj * clip;
    return world.xyz / world.w;
}

/// @brief one exact texel of a per-swapchain-image G-buffer target, through the NEAREST sampler
/// @note explicit LOD: this is a compute stage with fetches after an early return (TRAP 4), and every one of
///       these targets is single-mip, so LOD 0 is the mip an implicit fetch would have selected.
float4 gbuffer_texel(uint base_slot, float2 uv)
{
    return heap_texel_lod0(heap_at<Texture2D<float4>>(base_slot + heap_image_index), heap_sampler_gbuffer, uv);
}

/// @brief UE's `SmoothFalloffMask`: a smooth cut to zero below @p threshold
/// @note `pow2(saturate(1 - pow4(T/X)))`, exactly as `MegaLights.ush:409-420` writes it, and the SAME value is
///       used to build the sampling weight and to scale the final estimate - which is what keeps the estimator
///       consistent when the mask is far from 1.
float smooth_falloff_mask(float x, float threshold)
{
    if (x <= 0.0)
    {
        return 0.0;
    }
    const float t = clamp(1.0 - pow(threshold / x, 4.0), 0.0, 1.0);
    return t * t;
}

/// @brief was the light at @p dir reachable from @p origin within @p distance
/// @return 1.0 when nothing blocked the ray, 0.0 when something did
/// @note a ray SHORTER than tmin has no interval to search - a ray query initialized with tmax <= tmin is
///       forbidden by the specification - and a light inside the surface it lights is the honest reading of
///       that, so the query is skipped and the light counts as unblocked.
float visibility(float3 origin, float3 dir, float distance, float tmin)
{
    if (distance <= tmin)
    {
        return 1.0;
    }
    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = tmin;
    ray.Direction = dir;
    ray.TMax = distance;

    // GLSL's `gl_RayFlagsOpaqueEXT` is Slang's RAY_FLAG_FORCE_OPAQUE (the DXR name for the same flag - there
    // is no plain RAY_FLAG_OPAQUE, which cost one build), and `gl_RayFlagsTerminateOnFirstHitEXT` is
    // RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH.
    const uint flags = RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(heap_at<RaytracingAccelerationStructure>(heap_slots_tlas + heap_frame_slot), flags, 0xFF, ray);
    while (query.Proceed())
    {
    }
    return query.CommittedStatus() == COMMITTED_NOTHING ? 1.0 : 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void comp_main(uint3 invocation : SV_DispatchThreadID)
{
    RWTexture2D<float4> output_image = heap_at<RWTexture2D<float4>>(heap_slots_ml_trace_storage + heap_image_index);
    uint ml_width, ml_height;
    output_image.GetDimensions(ml_width, ml_height);
    const int2 ml_extent = int2(int(ml_width), int(ml_height));
    const int2 pixel = int2(invocation.xy);
    if (pixel.x >= ml_extent.x || pixel.y >= ml_extent.y)
    {
        return; // the dispatch is rounded up to the workgroup size
    }
    const float2 uv = (float2(pixel) + 0.5) / float2(ml_extent);

    const float depth = gbuffer_texel(heap_slots_gbuffer_depth, uv).r;
    if (depth >= 1.0)
    {
        output_image[pixel] = float4(0.0, 0.0, 0.0, 0.0); // background: no surface, so no punctual light on one
        return;
    }

    // This texel's surface, at HALF resolution - the pass's own resolution, like the GI tracer's.
    const float4 albedo_metallic = gbuffer_texel(heap_slots_gbuffer_albedo, uv);
    const float3 albedo = albedo_metallic.rgb;
    const float metallic = albedo_metallic.a;
    const float4 normal_roughness = gbuffer_texel(heap_slots_gbuffer_normal, uv);
    const float3 normal = normalize(normal_roughness.xyz);
    const float roughness = normal_roughness.w;
    const float3 world_pos = world_position_from_depth(uv, depth);
    const float3 v = normalize(camera_at(heap_camera_slot).camera_pos.xyz - world_pos);
    const float3 f0 = mix(float3(0.04, 0.04, 0.04), albedo, metallic);

    // The cluster lookup happens at the FULL-resolution pixel this half-resolution texel stands for: the light
    // grid is built in full-resolution tiles, so a half-resolution texel that used its own coordinates would
    // read the neighbouring tile's list at every tile edge.
    const float2 full_extent = heap_size(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), 0u);
    const int2 full_pixel = int2((float2(pixel) + 0.5) * full_extent / float2(ml_extent));
    const int cluster = cluster_index_at(full_pixel, world_pos);
    const int light_count = cluster_light_count_for(cluster);

    const int samples = clamp(int(pc.params.x), 1, ML_MAX_SAMPLES);
    const float min_weight = pc.params.y;

    // A per-pixel, per-frame state: the ray sequence and the light sequence both have to change between
    // frames, or a temporal filter would be fed the same mistake instead of an average of different ones.
    uint state = hash_u32(uint(pixel.x) + uint(pixel.y) * 4096u + uint(pc.params.w) * 2654435761u);
    const float pick_random = next_uniform(state);

    // ---- the reservoir, one per sample, initialized to the stratifying sequence ----
    float reservoir_random[ML_MAX_SAMPLES];
    int reservoir_light[ML_MAX_SAMPLES];
    float reservoir_weight[ML_MAX_SAMPLES];
    for (int k = 0; k < samples; ++k)
    {
        reservoir_random[k] = (pick_random + float(k)) / float(samples);
        reservoir_light[k] = -1;
        reservoir_weight[k] = 0.0;
    }

    // ---- one pass over the candidates, building the target PDF and updating every reservoir ----
    float weight_sum = 0.0;
    for (int i = 0; i < light_count; ++i)
    {
        const int light_index = cluster_light_index(cluster, i);
        const PunctualLight pl = light_at(heap_light_slot).punctual_lights[light_index];
        float3 dir;
        float distance;
        const float3 radiance = punctual_light_radiance(pl, world_pos, dir, distance);
        if (radiance == float3(0.0, 0.0, 0.0))
        {
            continue; // out of range, or outside the spot's cone: not a candidate at all
        }
        const float3 shaded = evaluate_direct_light(normal, v, albedo, metallic, roughness, f0, dir, radiance);
        const float lum = dot(shaded, float3(0.2126, 0.7152, 0.0722));
        // The log compression and the smooth cut, in that order (UE: the cut is inside the log's argument). A
        // luminance at or below the threshold lands on a weight of 0 and is dropped.
        const float weight = log2(lum * smooth_falloff_mask(lum, min_weight) + 1.0);
        if (!(weight > 0.0))
        {
            continue; // zero, or a NaN out of a degenerate light
        }
        const float tau = weight_sum / (weight_sum + weight);
        weight_sum += weight;
        for (int k = 0; k < samples; ++k)
        {
            if (reservoir_random[k] < tau)
            {
                reservoir_random[k] /= max(tau, 1e-6); // keep the light already held, rescale its weight
            }
            else
            {
                reservoir_random[k] = (reservoir_random[k] - tau) / max(1.0 - tau, 1e-6);
                reservoir_light[k] = light_index;
                reservoir_weight[k] = weight;
            }
            reservoir_random[k] = clamp(reservoir_random[k], 0.0, 0.9999);
        }
    }

    // ---- trace and shade the selected samples ----
    float3 lighting = float3(0.0, 0.0, 0.0);
    float unblocked = 0.0;
    if (weight_sum > 0.0)
    {
        for (int k = 0; k < samples; ++k)
        {
            if (reservoir_light[k] < 0 || !(reservoir_weight[k] > 0.0))
            {
                continue; // this reservoir never found a candidate
            }
            const PunctualLight pl = light_at(heap_light_slot).punctual_lights[reservoir_light[k]];
            float3 dir;
            float distance;
            const float3 radiance = punctual_light_radiance(pl, world_pos, dir, distance);
            // The origin bias shrinks as the light approaches the normal: at grazing incidence the surface and
            // the ray are nearly parallel, and a bias small enough there would let the ray re-hit the triangle
            // it started on. UE's pair.
            const float offset = mix(pc.bias.y, pc.bias.x, clamp(dot(normal, dir), 0.0, 1.0));
            const float3 origin = world_pos + normal * offset;

            // ---- SOFT SHADOWS: the emitter has a size, so the ray aims at a DISC around its centre ----
            // A point light's visibility is a binary answer, so a pixel whose shadow boundary falls inside it
            // answers 0 or 1 depending on where the boundary sits THIS frame; TAA's sub-pixel jitter moves it
            // every frame and the pixel snaps between lit and unlit. Aiming the SAME ray (no extra cost) at a
            // uniformly distributed point of a disc of radius `distance * bias.z` turns that binary answer into
            // the fraction of the emitter the pixel can see. The parameter is an ANGLE so a distant light casts
            // the same softness as a near one of the same angular size. 0 keeps the old hard-shadow behaviour
            // exactly (no jitter, no extra random draws), and the BRDF is evaluated at the CENTRE direction -
            // the representative-point approximation, which keeps the estimator's weights untouched.
            float3 ray_dir = dir;
            float ray_distance = distance;
            if (pc.bias.z > 0.0)
            {
                const float3 up = abs(dir.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
                const float3 tangent = normalize(cross(up, dir));
                const float3 bitangent = cross(dir, tangent);
                // sqrt keeps the disc's area density uniform (the same trick the light samplers use)
                const float radius = distance * pc.bias.z * sqrt(next_uniform(state));
                const float phi = 6.2831853071795864 * next_uniform(state);
                const float3 target = world_pos + dir * distance + (tangent * cos(phi) + bitangent * sin(phi)) * radius;
                ray_dir = normalize(target - origin);
                // a margin, because the lateral target sits a hair off the sphere the point light lies on
                ray_distance = distance * 1.01;
            }
            const float visible = visibility(origin, ray_dir, ray_distance, pc.params.z);
            const float3 shaded = evaluate_direct_light(normal, v, albedo, metallic, roughness, f0, dir, radiance);
            // `Sum / W_k` is 1/p for the reservoir's selection probability; the firefly cap bounds what a
            // single sample may carry; `1/samples` is the estimator's own normalization, and the smooth cut is
            // applied HERE as well as in the PDF (from the same luminance), which is what keeps the estimate
            // consistent when the cut is not 1.
            const float lum = dot(shaded, float3(0.2126, 0.7152, 0.0722));
            const float weight = min(weight_sum / reservoir_weight[k], ML_MAX_SHADING_WEIGHT) * smooth_falloff_mask(lum, min_weight) / float(samples);
            lighting += shaded * visible * weight;
            unblocked += visible;
        }
    }
    output_image[pixel] = float4(lighting, unblocked / float(samples));
}
