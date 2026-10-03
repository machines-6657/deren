// shaders/rt_shadow.slang - the SLANG source of the ray-traced shadow's stages.
//
// PRODUCES `rt_shadow.rchit.spv` (and will carry the raygen, miss and any-hit entries as they are ported),
// the same names the GLSL versions produced, so the host side is untouched. This file starts with the
// CLOSEST-HIT stage, which is the smallest one and the one that carries the payload contract.
//
// THE HIT GROUP REPORTS THAT THE RAY HIT SOMETHING, and nothing else: the sun's visibility is a binary
// question and `gl_RayFlagsTerminateOnFirstHitEXT` in the raygen stops traversal here, so this stage has no
// attribute to read, no material to fetch and no shading to do. It exists because a traced ray needs a hit
// group in its shader binding table, and it is the stage a LATER step grows: an any-hit shader for alphaMode
// MASK, and an opacity micromap in front of it, belong exactly here.
//
// THE PAYLOAD IS WRITTEN HERE AND NOWHERE ELSE ON THIS PATH, and that is the contract to preserve rather than a
// detail: the raygen deliberately does NOT initialise it (an initialisation storing the same value the miss
// shader stores is what the pass header records as a bug - the compiler dropped the redundant store and every
// escaped ray came back occluded). So this 1.0 and the miss shader's 0.0 are the two halves of it, and the
// payload is one float.

// THE SHIM, because the RAYGEN entry reads the heap (the G-buffer images, the light block, the TLAS and the
// visibility storage image). The closest-hit and miss entries need none of it and pay nothing for it.
#include "heap_access.slang"

struct shadow_payload
{
    float occluded;
};

// The camera and light blocks and the push block the RAYGEN reads. The blocks are declared here rather than
// taken from shading.glsl because this file shares no body (the same reason shadow.slang declares its own
// material record); they are named what the shim's `camera_at` / `light_at` accessors expect, and the member
// lists are the PREFIXES this stage reads - a stage may declare fewer members than the CPU writes as long as
// the offsets agree (0/64/128 for the camera; the light's matrices exist to keep light_dir at 256).
struct CameraUBO
{
    VR_MAT4 view;
    VR_MAT4 proj;
    float4 camera_pos;
};

struct LightUBO
{
    VR_MAT4 light_view_proj[4]; // MAX_SHADOW_CASCADES: not read here, only here to keep light_dir's offset
    float4 light_dir;           // xyz = surface-to-light direction (see shading.glsl)
};

// THE MATERIAL LAYOUT IS NOT DECLARED HERE ANY MORE: it is `material_record.glsl`, the one place the
// record's std430 layout lives, and this stage includes it. A second copy is a second thing to keep in
// step with the host's `static_assert(sizeof(material_record) == 80)`, and a copy that drifts indexes
// every material after the first by the wrong STRIDE - which shows up as a wrong texture on every
// material but one, not as a build error.
#include "material_record.glsl"

// The instance table: one record per instance of the top level structure, filled by EVERY structure build
// (acceleration_structure::instance_record, whose 96-byte std430 layout this mirrors). The two addresses are
// held as 32-bit HALVES, the shape this project carries an address in.
struct MaskInstance
{
    uint2 vertex_address;  // the vertex buffer's device address
    uint2 index_address;   // the index buffer's, or zero when the geometry is not indexed (the bake's output)
    VR_MAT4 model;         // object -> world (not read here; the traversal's transform is)
    uint vertex_stride;    // bytes per vertex of the interleaved layout
    uint index_type;       // VkIndexType: 0 = uint16, 1 = uint32
    uint material_index;   // into the material table above
    uint primitive_index;
};

struct RtShadowPush
{
    VR_MAT4 inv_view_proj; // clip -> world, the same reconstruction the lighting stage does
    float4 params;         // x = ray tmin, y = absolute normal-offset floor, z = relative offset scale, w = unused
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<RtShadowPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

// =============================================================================================================
// THE RAYGEN (entry `rgen_main`): ray-traced sun visibility, one traced ray per pixel.
//
// It is the estimator the inline ray-query form used, moved onto a ray-tracing PIPELINE: a pixel with geometry
// reconstructs its world position from the G-buffer depth, casts one ray towards the sun, and writes 1.0 (lit)
// or 0.0 (occluded). Nothing is shaded here and no hit information is used - the ray terminates on the first
// hit and the hit group's only job is to say "something was there" through the payload. What the ray buys over
// a cascaded shadow map is not sharpness but CORRECTNESS of the occluder set: a caster outside every cascade's
// fitted box, a caster between two cascades, a contact shadow at the base of an object, and the bias a depth
// map needs to avoid self-shadowing all stop being approximations.
//
// THE PAYLOAD IS NOT INITIALISED HERE, and that is a measured requirement rather than a style choice: the
// payload must be written by the stage that ENDS the ray, on EVERY path, and a raygen storing the same 0.0 the
// miss shader stores makes the miss shader's store look redundant - the device's compiler then drops it and
// reads the payload back uninitialised (the shadow bug the GLSL pass header records: 61.22 mean over the model
// against 76.48 for the raster reference, 100% of differing pixels darker). One store per path, no
// pre-initialisation. THE BUILT MODULE IS CHECKED FOR THIS rather than assumed: a store to the payload before
// the trace is the failure, and Slang zero-initialising a local is exactly the kind of thing that would
// reintroduce it silently.
// =============================================================================================================

/// @brief world position of a depth-buffer sample (the lighting stage's reconstruction)
float3 rt_world_position_from_depth(float2 uv, float depth)
{
    const float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    const float4 world = pc.inv_view_proj * clip;
    return world.xyz / world.w;
}

[shader("raygeneration")]
void rgen_main()
{
    RWTexture2D<float> visibility = heap_at<RWTexture2D<float>>(heap_slots_rt_visibility_storage + heap_frame_slot);
    uint extent_x, extent_y;
    visibility.GetDimensions(extent_x, extent_y);

    const int2 pixel = int2(DispatchRaysIndex().xy);
    if (pixel.x >= int(extent_x) || pixel.y >= int(extent_y))
    {
        return; // the launch dims are the extent, but a shader that assumes that is one rounding away from writing out of bounds
    }
    const float2 uv = (float2(pixel) + 0.5) / float2(float(extent_x), float(extent_y));

    const float depth = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), heap_sampler_gbuffer, uv).r;
    if (depth >= 1.0)
    {
        // No geometry: nothing to shade, so nothing to shadow. 1.0 rather than 0.0 because the lighting stage
        // uses this as a multiplier on a sun term that does not exist here anyway, and "lit" is the value that
        // leaves a debug read-back readable.
        visibility[pixel] = 1.0;
        return;
    }

    const float3 normal = normalize(heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index), heap_sampler_gbuffer, uv).xyz);
    const float3 world_pos = rt_world_position_from_depth(uv, depth);
    const float3 to_light = normalize(light_at(heap_light_slot).light_dir.xyz);

    // The bias: offset along the normal by an amount that grows with the distance from the camera (a surface 30
    // units away has coarser depth precision than one at 2), with an absolute floor so a pixel right at the near
    // plane still clears its own triangle. The ray also starts at tmin, which handles the case where the offset
    // direction itself is grazing.
    const float distance = length(world_pos - camera_at(heap_camera_slot).camera_pos.xyz);
    const float3 origin = world_pos + normal * max(pc.params.y, pc.params.z * distance);

    RayDesc ray;
    ray.Origin = origin;
    ray.TMin = pc.params.x;
    ray.Direction = to_light;
    ray.TMax = 1e30; // a directional light: the ray is a half-line, not a segment

    // NO RAY_FLAG_OPAQUE: it declares "no any-hit shader may be invoked for this ray", which is the opposite of
    // what this ray is for. Measured, the flag does NOT actually suppress the hit group's any-hit stage on this
    // device (see the any-hit entry for the three-arm test); it is left off so that the alpha test does not
    // rest on a driver over-invoking a stage a conforming one would skip.
    shadow_payload payload;
    TraceRay(heap_at<RaytracingAccelerationStructure>(heap_slots_tlas + heap_frame_slot),
             // GLSL's `gl_RayFlagsTerminateOnFirstHitEXT` is Slang's RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
             // (the DXR name for the same flag - the GLSL spelling does not exist here, which cost one build).
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
             0xFF, // every instance can occlude
             0,    // sbtRecordOffset: one hit group, so every instance maps to it
             0,    // sbtRecordStride
             0,    // missIndex
             ray,
             payload);

    visibility[pixel] = (payload.occluded > 0.5) ? 0.0 : 1.0;
}

// =============================================================================================================
// THE ANY-HIT STAGE (entry `ahit_main`): the alphaMode MASK cut, which a ray query cannot run.
//
// THIS STAGE IS THE REASON THE SHADOW MOVED ONTO A RAY-TRACING PIPELINE. A ray query has no any-hit stage, so
// every triangle it meets is solid: a MASK surface - a leaf card, a chain-link fence, a cut-out sign - casts
// the shadow of its BOUNDING TRIANGLES. A traced ray runs this stage instead, and it cuts the transparent part
// of a triangle with IgnoreHit (GLSL's ignoreIntersectionEXT).
//
// WHAT IT DOES, in one line: it interpolates the material's base colour ALPHA at the hit and refuses the
// intersection when that alpha is below the material's cutoff, with the SAME predicate the raster paths use
// (`base_color_factor.a * albedo.a < alpha_cutoff`). TWO DETAILS ARE FORCED BY WHERE IT RUNS and both are
// traps this migration has already met once:
//  - the fetch is an EXPLICIT-LOD fetch (`heap_texel_lod0`): a hit shader has no derivatives, so an
//    implicit-LOD sample would make the module invalid (docs/slang_migration.md TRAP 4) - and LOD 0 is what
//    the raster test samples anyway;
//  - the UV is NOT an interpolated varying, because there is none. It is FETCHED: the primitive index names
//    the triangle, its three vertex indices come from the geometry's index buffer and their UVs from the
//    vertex buffer, both through the DEVICE ADDRESSES in the instance record. That is GLSL's
//    GL_EXT_buffer_reference, and Slang spells the same thing as a C-style pointer cast from the 64-bit
//    address - `(const float*)address` - which emits OpConvertUToPtr into a PhysicalStorageBuffer pointer
//    (verified in a probe before this port: OpCapability PhysicalStorageBufferAddresses + OpConvertUToPtr).
//
// WHY gl_HitAttributeEXT IS NOT USED: it is written by an INTERSECTION shader, and this pipeline has none -
// triangle geometry is intersected by the fixed-function unit, and VkAccelerationStructureGeometryTrianglesDataKHR
// has no attribute field to declare one in. The fetch above is therefore not a workaround; it is the only
// channel a triangle hit has to its own vertices.
//
// THE TWO "OPAQUE" DECLARATIONS WERE LIFTED FOR THIS STAGE (no RAY_FLAG_OPAQUE in the raygen, no
// VK_GEOMETRY_OPAQUE_BIT_KHR on the BLAS), because both DECLARE "no any-hit shader may be invoked": measured
// on an RTX 4060 with the closest-hit stage silenced, neither flag actually suppressed this stage on that
// driver, but a conforming driver would skip it and the cut would silently do nothing - the worst kind of
// failure, because the frame would still look like a shadow.
// =============================================================================================================

/// @brief material_record::flags bit4 - "alphaMode MASK", the same bit shaders/surface.glsl tests
#define MASK_ALPHA_FLAG 16u
/// @brief the uv's offset in the interleaved vertex, in floats: position(0-2) normal(3-5) uv(6-7)
#define MASK_UV_FLOAT_OFFSET 6u

/// @brief the 64-bit device address held in two lanes (little-endian, low half first)
uint64_t mask_lane_address(uint2 halves)
{
    return (uint64_t(halves.y) << 32u) | uint64_t(halves.x);
}

[shader("anyhit")]
void ahit_main(inout shadow_payload payload)
{
    // gl_InstanceCustomIndexEXT is DXR's InstanceID (the user-provided index), NOT InstanceIndex (the slot in
    // the structure) - the distinction matters because the instance table is indexed by the former.
    const MaskInstance instance = heap_at<StructuredBuffer<MaskInstance>>(heap_mask_instance_slot)[InstanceID()];
    const Material material = material_at(heap_slots_materials, instance.material_index);
    if ((material.flags & MASK_ALPHA_FLAG) == 0u)
    {
        // OPAQUE or BLEND: the raster shadow pass does not cut those either, so the hit stands as it is.
        return;
    }

    // The three vertices of the triangle the ray hit, by the geometry's own addressing: an index (or, for a
    // non-indexed source - which is what the mask bake's expanded geometry is - the vertex's own position in
    // the list), times the stride, off the base.
    float* vertices = (float*)mask_lane_address(instance.vertex_address);
    const uint stride = instance.vertex_stride / 4u;
    const bool indexed = (instance.index_address.x != 0u) || (instance.index_address.y != 0u);
    float3 positions[3];
    float2 uvs[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        const uint i = uint(PrimitiveIndex()) * 3u + k;
        uint vertex = i;
        if (indexed)
        {
            uint* indices = (uint*)mask_lane_address(instance.index_address);
            if (instance.index_type == 0u)
            {
                const uint word = indices[i >> 1u];
                vertex = (i & 1u) == 0u ? (word & 0xFFFFu) : (word >> 16u);
            }
            else
            {
                vertex = indices[i];
            }
        }
        const uint at = vertex * stride;
        positions[k] = float3(vertices[at + 0u], vertices[at + 1u], vertices[at + 2u]);
        uvs[k] = float2(vertices[at + MASK_UV_FLOAT_OFFSET + 0u], vertices[at + MASK_UV_FLOAT_OFFSET + 1u]);
    }

    // The hit's barycentric coordinates, computed in OBJECT space because that is the space the vertices are
    // in: the traversal transforms the ray there. WorldToObject3x4() is the traversal's inverse transform - an
    // affine transform stored as three rows of four - so it multiplies a float4 and yields a float3, which is
    // also what makes the direction's translation drop out (w = 0) and leaves the parametric hit distance
    // unchanged. Moller-Trumbore's u and v are the weights of vertices 1 and 2, which is what the
    // interpolation below uses.
    const float3 origin_object = mul(WorldToObject3x4(), float4(WorldRayOrigin(), 1.0));
    const float3 direction_object = mul(WorldToObject3x4(), float4(WorldRayDirection(), 0.0));
    const float3 hit_point = origin_object + direction_object * RayTCurrent();
    const float3 edge_1 = positions[1] - positions[0];
    const float3 edge_2 = positions[2] - positions[0];
    const float3 pv = cross(direction_object, edge_2);
    const float det = dot(edge_1, pv);
    if (abs(det) < 1e-12)
    {
        // A degenerate triangle has no UV frame to interpolate in. Committing the hit is the conservative
        // answer: an unrefused hit keeps a shadow, while a wrong refusal deletes one.
        return;
    }
    const float inv_det = 1.0 / det;
    const float3 tv = hit_point - positions[0];
    const float u = dot(tv, pv) * inv_det;
    const float v = dot(cross(tv, edge_1), direction_object) * inv_det;
    const float2 uv = uvs[0] * (1.0 - u - v) + uvs[1] * u + uvs[2] * v;

    // THE CUT: the mirror of shaders/shadow.slang's discard and of shaders/surface.glsl's MASK test.
    const float alpha = material.base_color_factor.a * heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_textures + material.tex_indices.x), heap_sampler_texture, uv).a;
    if (alpha < material.alpha_cutoff)
    {
        IgnoreHit();
    }
}

/// @brief a hit along the shadow ray: the surface occludes the sun
/// @note the payload travels as an `inout` parameter, which is how Slang binds a ray payload; the GLSL side
///       declares it as `layout(location = 0) rayPayloadInEXT float`, i.e. the same single float at location 0.
[shader("closesthit")]
void chit_main(inout shadow_payload payload)
{
    payload.occluded = 1.0;
}

/// @brief the ray escaped the scene: nothing blocked the light
/// @note THIS STORE IS LOAD-BEARING AND MUST SURVIVE CODEGEN, which is the opposite of what an "empty miss
///       shader" suggests. The payload is the only channel a traversal's outcome comes back through, and
///       leaving the miss case to a value the raygen initialised is what this pass did first: measured on an
///       NVIDIA RTX 4060, a raygen storing 0.0 before the trace plus a miss shader that does not write the
///       payload made EVERY escaped ray come back occluded (a DamagedHelmet capture measured 61.22 mean over
///       the model against 76.48 for the raster shadow map, with 100% of the differing pixels DARKER - the
///       whole sunlit ground had its sun killed). Storing the SAME 0.0 the raygen stored is not enough
///       either, because then the store is redundant and the compiler drops it. What works is exactly one
///       store per path with the raygen initialising NOTHING: the raygen writes nothing, the hit group
///       writes 1.0 and this writes 0.0 - which is why the built module is checked for this store rather
///       than assumed (see docs/slang_migration.md).
[shader("miss")]
void miss_main(inout shadow_payload payload)
{
    payload.occluded = 0.0; // "nothing blocked the light": the ray escaped the scene unoccluded
}
