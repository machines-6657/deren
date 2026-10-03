// shaders/compute_skin.slang - the SLANG source of the GPU skinning pass that feeds the acceleration structures.
//
// PRODUCES `compute_skin.comp.spv`, the same name the GLSL version produced, so the host side is untouched.
// Ported from shaders/compute_skin.comp.
//
// WHY THIS PASS EXISTS, unchanged by the port: this engine skins in the VERTEX shader, which is where a
// deformation belongs for rasterisation - and which means the deformed positions never exist in memory an
// acceleration structure build can read. So a skinned mesh's ray-traced shadow would be its BIND POSE. This
// pass writes the SAME vertex positions the vertex shader computes, so the structure is built from (and
// refitted against) the deformed geometry. THE MATHS IS COPIED FROM THE VERTEX SHADER TERM FOR TERM: two
// implementations of one deformation that disagree show up as a shadow that does not match its object, which
// is exactly what the pass exists to fix. It does NOT do morph targets (they are the step before skinning,
// and this pass reads only position/normal/uv and the joints/weights).
//
// THE OUTPUT KEEPS THE SOURCE'S VERTEX ORDER, one 32-byte record per vertex (position, normal, uv), so the
// primitive's own index buffer stays valid and the structure keeps its triangle count - which is what makes a
// REFIT possible rather than a rebuild.
//
// WHAT THE SLANG SIDE SPELLS DIFFERENTLY: the GLSL reads both buffers through GL_EXT_buffer_reference, i.e. a
// 64-bit device address per buffer; Slang spells the same thing as a C-style pointer cast from the address
// (`(uint*)address`, `(float*)address`), emitting OpConvertUToPtr into a PhysicalStorageBuffer pointer. The
// words are read as UINT and bit-cast to float (`asfloat`), exactly as the GLSL does with uintBitsToFloat -
// the bit pattern is the payload, so the type is a view rather than a conversion.

#include "heap_access.slang"

struct SkinPush
{
    // Device addresses as two 32-bit halves, the shape every pass in this project pushes them in.
    uint2 source_vertices;
    uint2 destination;
    uint source_stride;      // the source's byte stride (64: the engine's interleaved vertex)
    uint destination_stride; // 32: position(3) + normal(3) + uv(2), what hit shading reads
    uint vertex_count;
    uint skin_base; // this primitive's joint block in the skin matrices (0 = the identity block)
    // THE HEAP INDICES, at the END so every field above keeps its offset.
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<SkinPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

/// @brief the 64-bit device address held in two push lanes (little-endian, low half first)
uint64_t skin_lane_address(uint2 halves)
{
    return (uint64_t(halves.y) << 32u) | uint64_t(halves.x);
}

/// @brief one record of the source vertex's position/normal/uv, off a base address
/// @note the source is the engine's interleaved vertex, read as untyped words: floats for the first eight (the
///       offsets 0, 3 and 6 are position, normal and UV) and uints for the joints, which sit at byte 32 of a
///       64-byte record (the vertex stage declares them as location 4, a uint4; the weights follow at byte
///       48). THOSE TWO OFFSETS ARE THE LAYOUT'S, NOT THE STRIDE'S: `source_stride` scales which vertex is
///       read, and the caller only ever hands this pass a caster whose stride IS 64 - a mesh whose vertex
///       layout changed would need this shader changed with it, and the caller refuses it instead, so a wrong
///       deformation cannot be written silently.
[shader("compute")]
[numthreads(64, 1, 1)]
void comp_main(uint3 invocation : SV_DispatchThreadID)
{
    const uint vertex = invocation.x;
    if (vertex >= pc.vertex_count)
    {
        return; // the dispatch is rounded up to the workgroup size
    }
    const uint64_t source_base = skin_lane_address(pc.source_vertices);
    uint* source = (uint*)source_base;
    const uint at = vertex * (pc.source_stride / 4u);
    const float3 position = float3(asfloat(source[at + 0u]), asfloat(source[at + 1u]), asfloat(source[at + 2u]));
    const float3 normal = float3(asfloat(source[at + 3u]), asfloat(source[at + 4u]), asfloat(source[at + 5u]));
    const float2 uv = float2(asfloat(source[at + 6u]), asfloat(source[at + 7u]));

    // The joints are read through their OWN reference at the vertex's byte 32: a word view rather than a
    // float one, because a joint index is an integer and reinterpreting it as a float and back is the kind of
    // round trip that works until an index lands in a signaling NaN bit pattern.
    uint* joint_words = (uint*)(source_base + 32ull + uint64_t(at) * 4ull);
    const uint4 joints = uint4(joint_words[0], joint_words[1], joint_words[2], joint_words[3]);
    const float4 weights = float4(asfloat(source[at + 12u]), asfloat(source[at + 13u]), asfloat(source[at + 14u]), asfloat(source[at + 15u]));

    const float4 local = float4(position, 1.0);
    float3 skinned_position = position;
    float3 skinned_normal = normal;
    const float weight_sum = weights.x + weights.y + weights.z + weights.w;
    if (weight_sum > 0.0)
    {
        float4 blended = float4(0.0, 0.0, 0.0, 0.0);
        float3 blended_normal = float3(0.0, 0.0, 0.0);
        blended += weights.x * (skin_matrix_at(heap_skin_slot, pc.skin_base + joints.x) * local);
        blended += weights.y * (skin_matrix_at(heap_skin_slot, pc.skin_base + joints.y) * local);
        blended += weights.z * (skin_matrix_at(heap_skin_slot, pc.skin_base + joints.z) * local);
        blended += weights.w * (skin_matrix_at(heap_skin_slot, pc.skin_base + joints.w) * local);
        blended_normal += weights.x * mat3(skin_matrix_at(heap_skin_slot, pc.skin_base + joints.x)) * normal;
        blended_normal += weights.y * mat3(skin_matrix_at(heap_skin_slot, pc.skin_base + joints.y)) * normal;
        blended_normal += weights.z * mat3(skin_matrix_at(heap_skin_slot, pc.skin_base + joints.z)) * normal;
        blended_normal += weights.w * mat3(skin_matrix_at(heap_skin_slot, pc.skin_base + joints.w)) * normal;
        skinned_position = (blended / weight_sum).xyz;
        skinned_normal = blended_normal / weight_sum;
    }

    // OBJECT space, not world: the structures are built from object-space vertices and the instance carries
    // the object-to-world matrix (the same division of labour the unskinned path has, where the build reads
    // the primitive's own vertex buffer).
    float* out_values = (float*)skin_lane_address(pc.destination);
    const uint out_at = vertex * (pc.destination_stride / 4u);
    out_values[out_at + 0u] = skinned_position.x;
    out_values[out_at + 1u] = skinned_position.y;
    out_values[out_at + 2u] = skinned_position.z;
    out_values[out_at + 3u] = skinned_normal.x;
    out_values[out_at + 4u] = skinned_normal.y;
    out_values[out_at + 5u] = skinned_normal.z;
    out_values[out_at + 6u] = uv.x;
    out_values[out_at + 7u] = uv.y;
}
