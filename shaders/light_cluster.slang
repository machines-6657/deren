// shaders/light_cluster.slang - the SLANG source of the clustered-light culling compute stage.
//
// PRODUCES `light_cluster.comp.spv`, the same name the GLSL version produced, so the host side is untouched.
// Ported from shaders/light_cluster.comp (now in `shaders/glsl.old/`). One invocation per CLUSTER,
// one dispatch per frame, before the frame's rendering: the grid is the screen cut into 64-pixel tiles and 16
// EXPONENTIAL depth slices, and for its cluster the invocation rebuilds the view-space box (the tile's corner
// rays unprojected at the slice's near and far depth) and appends every light whose bounding sphere intersects
// it to the cluster's fixed-capacity list.
//
// THIS IS THE FIRST COMPUTE STAGE IN THE MIGRATION, so it is also where the compute-specific spellings are
// settled: `[shader("compute")]` + `[numthreads(64, 1, 1)]` in place of `layout(local_size_x = 64) in;`, the
// dispatch index as the `SV_DispatchThreadID` parameter in place of `gl_GlobalInvocationID`, and the two
// buffers the pass WRITES declared as `RWStructuredBuffer` behind the same heap slots the GLSL declares as
// `writeonly buffer` (a heap descriptor is an address range, so the read/write direction is the shader's).
//
// The shading side of the contract is `cluster_slice_of()` in shaders/shading.glsl: the slice boundaries MUST
// be the same function on both sides, or a fragment lands in a cluster whose list was never built for it.
// That is why `inverse(camera.proj)` here has to mean exactly what glslang means by it - see the note on
// cluster_bounds.
//
// The count buffer is host-coherent and memset at frame start, so this pass only appends.

#include "heap_access.slang"

const int MAX_PUNCTUAL_LIGHTS = 128;   // deren::vulkan::max_punctual_lights
const int CLUSTER_LIGHT_CAPACITY = 32; // deren::vulkan::cluster_light_capacity (lights stored per cluster)

struct PunctualLight
{
    float4 position; // xyz: world position (w unused)
    float4 color;    // xyz: linear color * intensity (w unused)
    float4 spot_dir; // xyz: spot axis, normalized for spot lights (w unused)
    float4 params;   // x = range (0 = infinite), y = 0 point / 1 spot, z = cos(outer cone), w = cos(inner cone, spot only)
};

// The fields this stage needs; the members in between are declared so the offsets match the shared layout
// (deren::vulkan::light_ubo - see the static_asserts there). A stage may declare fewer members than the CPU writes,
// but not a different layout up to the ones it uses. The matrix array is declared for that reason alone: this
// stage reads none of it (and a matrix ARRAY in a Slang block would lose its majorness decoration anyway - see
// docs/slang_migration.md section 9).
struct LightUBO
{
    VR_MAT4 light_view_proj[4];
    float4 light_dir;
    float4 cascade_splits;
    float4 cascade_texel_world;
    float shadow_enabled;
    float brdf_model;
    float diffuse_model;
    float cascade_blend;
    float cascade_count;
    float _pad0;
    float _pad1;
    float _pad2;
    float4 light_count; // x = active punctual light count
    PunctualLight punctual_lights[MAX_PUNCTUAL_LIGHTS];
    float4 cluster_grid;  // x = tiles x, y = tiles y, z = slices, w = clustering enabled
    float4 cluster_depth; // x = near, y = far (view depths), z/w = screen size in pixels
};

// Only the first three members are read, and std430 puts them at the same offsets std140 did.
struct CameraUBO
{
    VR_MAT4 view;
    VR_MAT4 proj;
    float4 camera_pos;
};

// This stage takes NO other constants (it is a pure culling pass), but it reads the per-frame camera, light
// and cluster descriptors, so it needs the two HEAP INDICES - and a heap pipeline has no layout to put a push
// constant in, so they arrive through vkCmdPushDataEXT.
struct ClusterPush
{
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<ClusterPush> push;
#define heap_frame_slot (push.frame_slot)
#define heap_image_index (push.image_index)

/// @brief view-space box of one cluster
/// @param tile_x / @p tile_y cluster tile coordinates (x right, y DOWN - the gl_FragCoord convention)
/// @param slice depth slice index
/// @param grid the active grid (tiles x, tiles y, slices)
/// @param out_min / @p out_max the box corners in VIEW space
/// @note the tile's NDC rectangle is unprojected at the slice's near and far depth. Both the y-flip and the
///       [0,1] depth range are already inside the projection matrix, so the unprojected corners are in the
///       same space the rendered fragments land in - which is what makes this box and the shader's cluster
///       index describe the same volume.
/// @note `inverse()` IS A RISK WORTH NAMING: SPIR-V has no inverse instruction, so glslang expands the 4x4
///       inverse inline and Slang expands its own, and two expansions can disagree in the last bit. The gate
///       is what decides whether they do here (a cluster box that moves by one ULP can move a light across a
///       boundary); if it ever fails on the cluster scenarios, this line is the first place to look.
void cluster_bounds(uint tile_x, uint tile_y, uint slice, uint3 grid, out float3 out_min, out float3 out_max)
{
    const float near_depth = max(light_at(heap_light_slot).cluster_depth.x, 1e-4);
    const float far_depth = max(light_at(heap_light_slot).cluster_depth.y, near_depth * 1.0001);
    const float ratio = far_depth / near_depth;
    const float slice_near = near_depth * pow(ratio, float(slice) / float(grid.z));
    const float slice_far = near_depth * pow(ratio, float(slice + 1u) / float(grid.z));

    const float2 screen = max(light_at(heap_light_slot).cluster_depth.zw, float2(1.0, 1.0));
    const float2 px_min = float2(float(tile_x), float(tile_y)) * 64.0; // deren::vulkan::cluster_tile_size
    const float2 px_max = min(px_min + 64.0, screen);
    const float2 ndc_min = px_min / screen * 2.0 - 1.0;
    const float2 ndc_max = px_max / screen * 2.0 - 1.0;

    const mat4 inv_proj = inverse(camera_at(heap_camera_slot).proj);
    out_min = float3(3.4e38, 3.4e38, 3.4e38);
    out_max = float3(-3.4e38, -3.4e38, -3.4e38);
    for (int corner = 0; corner < 4; ++corner)
    {
        const float2 ndc = float2((corner & 1) != 0 ? ndc_max.x : ndc_min.x, (corner & 2) != 0 ? ndc_max.y : ndc_min.y);
        for (int plane = 0; plane < 2; ++plane)
        {
            const float4 clip = float4(ndc, plane == 0 ? 0.0 : 1.0, 1.0);
            const float4 view_point = inv_proj * clip;
            const float3 ray = view_point.xyz / view_point.w; // view-space point on that plane
            // scale the ray so its view depth (positive in front of the camera) is the slice's
            const float depth = plane == 0 ? slice_near : slice_far;
            const float3 point = ray * (depth / max(-ray.z, 1e-6));
            out_min = min(out_min, point);
            out_max = max(out_max, point);
        }
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 cluster_id : SV_DispatchThreadID)
{
    const uint tiles_x = uint(light_at(heap_light_slot).cluster_grid.x);
    const uint tiles_y = uint(light_at(heap_light_slot).cluster_grid.y);
    const uint slices = uint(light_at(heap_light_slot).cluster_grid.z);
    if (tiles_x == 0u || tiles_y == 0u || slices == 0u || light_at(heap_light_slot).cluster_grid.w < 0.5)
    {
        return; // clustering disabled: the shading stage loops every light instead
    }
    const uint cluster = cluster_id.x;
    const uint cluster_count = tiles_x * tiles_y * slices;
    if (cluster >= cluster_count)
    {
        return;
    }
    const uint slice = cluster / (tiles_x * tiles_y);
    const uint tile_index = cluster % (tiles_x * tiles_y);
    const uint tile_x = tile_index % tiles_x;
    const uint tile_y = tile_index / tiles_x;

    float3 box_min;
    float3 box_max;
    cluster_bounds(tile_x, tile_y, slice, uint3(tiles_x, tiles_y, slices), box_min, box_max);

    // THE TWO BUFFERS THIS PASS WRITES: the same heap slots the GLSL declares as `writeonly buffer`, read
    // here as read/write structured buffers (the direction belongs to the shader, not to the descriptor).
    RWStructuredBuffer<uint> counts = heap_at<RWStructuredBuffer<uint>>(heap_cluster_count_slot);
    RWStructuredBuffer<uint> indices = heap_at<RWStructuredBuffer<uint>>(heap_cluster_index_slot);

    const uint light_total = min(uint(light_at(heap_light_slot).light_count.x), uint(MAX_PUNCTUAL_LIGHTS));
    for (uint i = 0u; i < light_total; ++i)
    {
        const float4 position = light_at(heap_light_slot).punctual_lights[i].position;
        const float range = light_at(heap_light_slot).punctual_lights[i].params.x;
        // view-space light sphere vs the cluster box: the closest point of the box to the center
        const float3 center = (camera_at(heap_camera_slot).view * float4(position.xyz, 1.0)).xyz;
        const float3 closest = clamp(center, box_min, box_max);
        const float3 delta = center - closest;
        // range == 0 is the "no cutoff" light (its falloff never reaches zero): always include it
        const bool reaches = range <= 0.0 || dot(delta, delta) <= range * range;
        if (!reaches)
        {
            continue;
        }
        // `atomicAdd` returns the PREVIOUS value, which is the slot this light takes in the cluster's list -
        // the same thing the GLSL reads, and the reason the pass needs no prefix sum: the list is fixed
        // capacity and a cluster that overflows simply drops the extra lights.
        const uint slot = atomicAdd(counts[cluster], 1u);
        if (slot < uint(CLUSTER_LIGHT_CAPACITY))
        {
            indices[cluster * uint(CLUSTER_LIGHT_CAPACITY) + slot] = i;
        }
    }
}
