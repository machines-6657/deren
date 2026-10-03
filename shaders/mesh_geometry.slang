// shaders/mesh_geometry.slang - THE GEOMETRY FETCH A MESH STAGE USES INSTEAD OF THE INPUT ASSEMBLER.
//
// WHY THIS FILE EXISTS (docs/mesh_shaders.md step 1): a mesh stage REPLACES the vertex stage and the input
// assembler together. There is no VkVertexInputBindingDescription to read a vertex through and no
// SV_VertexID to index one with, so the mesh stage has to name the buffers itself - which is the whole
// reason a mesh stage can cull before it emits anything, and also the whole cost of porting a vertex
// stage to one.
//
// IT IS SLANG, NOT GLSL, and that is deliberate: the fetch is a device ADDRESS cast to a pointer
// (`(uint*)address`), which GLSL spells GL_EXT_buffer_reference and Slang spells as a C-style cast (see
// shaders/compute_skin.slang, which does exactly this against the same 64-byte interleaved vertex). The
// GLSL sources are archived in shaders/glsl.old/ and no longer built, so there is no second consumer to
// keep a GLSL spelling alive for.
//
// THE LAYOUT IS NOT NEGOTIATED HERE: `mesh_vertex_at` reads the engine's ONE interleaved vertex record,
// byte for byte the one the vertex pipelines declare as locations 0,1,2,4,5 and the one
// shaders/compute_skin.slang skins. Position (float3, byte 0), normal (float3, byte 12), uv (float2, byte
// 24), joints (uint4, byte 32), weights (float4, byte 48), stride 64 - sorted by location, so the input
// assembler's derived offsets and these are the same numbers. A mesh whose layout differed would need
// this file changed with it; the caller refuses such a mesh instead (see the host's mesh draw path).
//
// IT DOES NOT DECIDE WHERE A TRIANGLE GOES: the stage's own transform body is shared with the vertex
// entry of the same file, so the two paths emit the same clip positions by construction rather than by a
// copied expression - which is what makes the capture gate a valid acceptance for a mesh port.

#ifndef MESH_GEOMETRY_SLANG
#define MESH_GEOMETRY_SLANG

/// @brief the 64-bit device address held in two 32-bit push lanes (little-endian, low half first)
/// @note the same shape shaders/compute_skin.slang pushes its two buffer addresses in, so a reader who
///       has seen one pass has seen both
uint64_t mesh_lane_address(uint2 halves)
{
    return (uint64_t(halves.y) << 32u) | uint64_t(halves.x);
}

/// @brief where a draw's geometry is, and which window of it this draw covers
///
/// @note A MESH STAGE HAS NO DRAW COMMAND TO READ THESE FROM. `vkCmdDrawMeshTasksEXT` carries group
///       counts and nothing else: no vertex buffer binding, no index buffer, no first index, no
///       instance count. Everything the input assembler used to consume therefore has to arrive as
///       DATA, and this struct is that data - pushed by the primitive's own draw() right before its
///       mesh dispatch, at the offset its stage block reserves for it (see the host's
///       `mesh_geometry_offset_*`).
///
/// @note THE ADDRESSES ARE FOUR 32-BIT HALVES RATHER THAN TWO `uint2`s, and that is not style: a `uint2`
///       aligns to 8 bytes, which leaves a 4-byte hole between the heap index lanes and this struct - and
///       with a descriptor-heap pipeline EVERY byte of the declared block must have been written by
///       `vkCmdPushDataEXT` before the draw, so a hole is a validation error
///       (VUID-vkCmdDrawMeshTasksEXT-None-11376, measured: "uses push-constant statically at range
///       [0, 144), but vkCmdPushDataEXT was never called for range [108, 112)"). Four-byte fields leave
///       no hole at all.
struct MeshGeometryLanes
{
    uint vertex_address_low; // device address of the vertex buffer's base (byte 0 of vertex 0)
    uint vertex_address_high;
    uint index_address_low;  // device address of the index buffer's base (index 0)
    uint index_address_high;
    uint first_index;        // the draw's first index inside the index buffer
    uint index_count;        // indices this draw covers (a multiple of 3: triangles)
    int base_vertex;         // added to every fetched index (static-draw chunks; 0 otherwise)
    uint index_width;        // bytes per index: 2 or 4
};

/// @brief one record of the engine's interleaved vertex, as a mesh stage needs it
struct MeshVertex
{
    float3 position;
    float3 normal;
    float2 uv;
    uint4 joints;
    float4 weights;
};

/// @brief the engine's interleaved vertex stride, in bytes
/// @note NOT a push lane: a mesh whose vertex layout differed could not be drawn by the raster pipelines
///       at all (their input state is derived from the vertex stage's own declarations), so the stride is
///       a property of the RENDERER rather than of the draw. shaders/compute_skin.slang makes the same
///       assumption and the caller refuses a source whose stride is not this.
static const uint mesh_vertex_stride = 64u;

/// @brief read vertex @p index (base-vertex included by the caller) off the draw's vertex buffer
/// @param geometry the draw's geometry lanes
/// @param index the ABSOLUTE vertex index (the caller adds `base_vertex` to the fetched index)
/// @note untyped words, bit-cast to float, exactly as compute_skin reads them: the bit pattern is the
///       payload, so the type is a view rather than a conversion. THE WORD OFFSETS ARE THE LAYOUT'S, not
///       the stride's: 0 = position, 3 = normal, 6 = uv, 8 = joints, 12 = weights.
MeshVertex mesh_vertex_at(const MeshGeometryLanes geometry, const uint index)
{
    MeshVertex v;
    const uint64_t base = mesh_lane_address(uint2(geometry.vertex_address_low, geometry.vertex_address_high));
    uint* words = (uint*)(base + uint64_t(index) * uint64_t(mesh_vertex_stride));
    v.position = float3(asfloat(words[0]), asfloat(words[1]), asfloat(words[2]));
    v.normal = float3(asfloat(words[3]), asfloat(words[4]), asfloat(words[5]));
    v.uv = float2(asfloat(words[6]), asfloat(words[7]));
    v.joints = uint4(words[8], words[9], words[10], words[11]);
    v.weights = float4(asfloat(words[12]), asfloat(words[13]), asfloat(words[14]), asfloat(words[15]));
    return v;
}

/// @brief read index @p i (absolute, i.e. `first_index` already added) off the draw's index buffer
/// @note THE 16-BIT CASE IS READ AS TWO HALVES OF A 32-BIT WORD rather than as a `uint16_t` load: a
///       16-bit buffer load would pull the `StorageBuffer16BitAccess` capability into every mesh stage
///       for a fetch that a shift and a mask express exactly, and Vulkan's index buffers are
///       little-endian, so the odd index is the word's high half.
uint mesh_index_at(const MeshGeometryLanes geometry, const uint i)
{
    const uint64_t base = mesh_lane_address(uint2(geometry.index_address_low, geometry.index_address_high));
    if (geometry.index_width == 2u)
    {
        uint* words = (uint*)(base + uint64_t(i >> 1u) * 4ull);
        const uint word = words[0];
        return (i & 1u) != 0u ? (word >> 16u) : (word & 0xFFFFu);
    }
    uint* words = (uint*)(base + uint64_t(i) * 4ull);
    return words[0];
}

/// @brief one MESHLET RECORD: exactly the 28 bytes `deren::vulkan::meshlet` holds and the GPU table stores, in byte order
struct MeshletRecord
{
    uint first_index;
    uint index_count;
    int base_vertex;
    float3 center;   // std430 puts a float3 on a 16-byte boundary: the host record carries the padding for it
    float radius;
    float3 axis;     // the normal cone's axis (the average of the window's face normals)
    float cone_cos;  // the smallest dot any face normal has with the axis, i.e. the cone's half-angle is acos(this)
};

/// @brief one record of the meshlet table (grid slot `heap_slots_meshlets`)
/// @note ONE descriptor rather than a per-frame array: the table is written once while the scene imports.
MeshletRecord meshlet_at(const uint index)
{
    return heap_at<StructuredBuffer<MeshletRecord>>(heap_slots_meshlets)[index];
}

/// @brief the same record out of the HOST-CULLED table for this frame (docs/mesh_shaders.md step 3)
/// @param frame_slot the frame in flight (the push block's, which the caller has)
/// @param index the record index INSIDE the primitive's run - the culled table uses the table's own layout, so the
///        host's compaction is what makes `index` (a workgroup id) select the i-th SURVIVING meshlet
/// @note a CULLED session passes its lanes' flag here: the host culled the primitive's run against the camera and
///       wrote only the survivors out, so the workgroups the dispatch asks for are exactly the ones with work to do.
///       The table's own indexing is unchanged, which is what keeps this a table SWAP rather than a second lookup.
MeshletRecord culled_meshlet_at(const uint frame_slot, const uint index)
{
    return heap_at<StructuredBuffer<MeshletRecord>>(heap_slots_meshlet_culled)[frame_slot * meshlet_table_capacity + index];
}

/// @brief how many records one frame's lane of the culled table holds: the same budget as the table itself
/// @note it is the value `core::meshlet_capacity` holds, and the host's `heap_slots::meshlet_culled` binding writes
///       exactly this many records per frame slot - the two have to agree or a lane would read its neighbour's run.
static const uint meshlet_table_capacity = 65536u;

// =============================================================================================================
// WHAT THE CULLING BUYS, COUNTED (docs/mesh_shaders.md step 3). The meshlet entry culls a whole meshlet against
// the camera before it emits anything, and until this existed the LOOK of the frame was the only evidence that it
// happened - a rejected meshlet and a meshlet nobody dispatched are the same pixels. So each entry adds to a
// counter, the host reads the buffer back at shutdown and logs the totals, and the number that matters is
// `mesh_stat_culled`: it is both how many meshlets the frustum test rejected AND how many workgroup launches a
// compute pass that culled BEFORE the dispatch would never have paid for.
//
// IT COSTS ONE ATOMIC PER WORKGROUP PER COUNTER, which is why nothing here counts per triangle. The buffer is one
// descriptor with a per-frame lane per counter (`mesh_stat_stride` uints per frame in flight), for the reason every
// per-frame buffer in this renderer has one: the recording of frame N+1 must not add to totals the GPU is still
// reading for frame N.
// =============================================================================================================
static const uint mesh_stat_stride = 8u;
static const uint mesh_stat_workgroups = 0u;      // meshlet workgroups dispatched = meshlets the stage looked at
static const uint mesh_stat_culled = 1u;         // ... rejected by the frustum test, emitting nothing
static const uint mesh_stat_emitted = 2u;        // ... that emitted their triangles
static const uint mesh_stat_triangles = 3u;      // triangles those meshlets emitted
static const uint mesh_stat_mesh_workgroups = 4u; // workgroups of the NON-meshlet mesh path, for comparison

/// @brief add @p value to one of this frame's mesh counters (see the block comment above)
export void mesh_stat_add(const uint frame_slot, const uint counter, const uint value)
{
    RWStructuredBuffer<uint> stats = heap_at<RWStructuredBuffer<uint>>(heap_slots_meshlet_stats);
    atomicAdd(stats[frame_slot * mesh_stat_stride + counter], value);
}
// =============================================================================================================
// CULLING HELPERS (docs/mesh_shaders.md step 3): what a stage needs to reject a whole meshlet before emitting any
// of it - and the reason they are THIS conservative rather than tight.
//
// The sphere a meshlet record carries is in OBJECT space, so a cull has to bring it to clip space through two
// matrices (the draw's world transform and the camera's or the light's projection) and scale its radius by how
// much those matrices can stretch a direction. THE RADIUS SCALE IS AN UPPER BOUND, deliberately: any accepted
// bound on a matrix's operator norm works (|M v| <= ||M|| |v|), and a row-sum bound is cheap and never
// underestimates. A tight bound would be faster to cull with and is the kind of "optimisation" that produces a
// hole in the shadow map on a few frames of one scene - a meshlet that IS visible must never be rejected, so the
// test errs on the side of drawing.
// =============================================================================================================

/// @brief the largest row sum of a matrix's linear part: an upper bound on how much it scales a direction
/// @note row sums rather than singular values: `|M v|_inf <= max_i sum_j |M_ij| |v|_inf`, and the sphere is a
///       Euclidean ball, so the bound is conservative for the 2-norm too.
float matrix_max_axis_scale(const VR_MAT4 m)
{
    float largest = 0.0;
    for (uint row = 0u; row < 3u; ++row)
    {
        const float sum = abs(m[row][0]) + abs(m[row][1]) + abs(m[row][2]);
        largest = max(largest, sum);
    }
    return largest;
}

/// @brief whether a meshlet faces entirely AWAY from the eye, i.e. every triangle in it is back-facing
/// @param axis_world the record's cone axis in world space (the caller transforms and normalizes it)
/// @param cone_cos the record's cone cosine: the cone's half-angle is `acos(cone_cos)`, so `1` is an EXACT cone (one
///        normal direction) and `0` is the widest useful one - at `0` the threshold below is `-1`, which no dot
///        product can go under, so the test never culls a meshlet whose normals span a hemisphere. (This note said
///        the opposite until the back-face experiment measured it: the value that "states nothing" is 0, not 1.)
/// @param centre_world the meshlet's sphere centre in world space, @p radius_world its radius there
/// @param eye the camera position, which is what "facing" is measured against
/// @note THE TEST IS THE CONSERVATIVE ONE: a triangle is front-facing when its normal has a positive dot with the
///       direction to the eye, and the cone's BEST-CASE normal is `axis` rotated `acos(cone_cos)` towards the eye -
///       so the meshlet can only be rejected when even that one points away: `dot(axis, to_eye) < -sin(cone)`. A
///       tighter test (using the axis alone) would cull more and would sometimes cull a meshlet with one visible
///       triangle, which is a hole rather than a saving.
/// @note AN EYE INSIDE THE SPHERE CANCELS THE TEST: the cone says nothing about which side the eye is on when the
///       eye is surrounded by the geometry, and the standard guard is to draw in that case.
bool meshlet_back_facing(const float3 axis_world, const float cone_cos, const float3 centre_world, const float radius_world, const float3 eye)
{
    const float3 to_eye = eye - centre_world;
    const float distance_sq = dot(to_eye, to_eye);
    if (distance_sq <= radius_world * radius_world)
    {
        return false;
    }
    const float3 direction = to_eye * rsqrt(distance_sq);
    return dot(axis_world, direction) < -sqrt(max(0.0, 1.0 - cone_cos * cone_cos));
}
/// @brief whether a clip-space sphere can intersect the clip volume (x, y in [-w, w], z in [0, w], w > 0)
/// @note the test uses `w + radius` on every plane, which is what makes it correct for a PERSPECTIVE projection
///       (where w varies per vertex) and trivially true for the shadow pass's orthographic one (w = 1).
bool clip_sphere_visible(const float4 clip, const float radius)
{
    return clip.w + radius >= 0.0 && abs(clip.x) <= clip.w + radius && abs(clip.y) <= clip.w + radius &&
           clip.z >= -clip.w - radius && clip.z <= clip.w + radius;
}
/// @brief how many triangles one mesh workgroup emits, and the vertices that costs
///
/// @note BOTH NUMBERS ARE LIMITS OF THE DEVICE, measured on this one and reported at startup (see
///       docs/mesh_shaders.md section 2): `maxMeshOutputVertices` and `maxMeshOutputPrimitives` are 256
///       each, and a triangle whose three vertices are written out separately - which is what a stage
///       without index sharing does - costs three of the former per one of the latter. 85 triangles =
///       255 vertices, i.e. the largest count that fits BOTH: 86 triangles would want 258 vertices.
///
/// @note THE INDEX SHARING A MESHLET WOULD DO IS NOT HERE ON PURPOSE. Emitting each triangle's three
///       vertices independently is the DIRECT port of what the input assembler produced (it is the same
///       triangle with the same three vertices in the same order), which is what lets the capture gate
///       decide the port - the sharing, and the meshlet structure it needs, is step 3.
static const uint mesh_triangles_per_workgroup = 85u;
static const uint mesh_vertices_per_workgroup = 255u;
static const uint mesh_indices_per_triangle = 3u;

#endif // MESH_GEOMETRY_SLANG
