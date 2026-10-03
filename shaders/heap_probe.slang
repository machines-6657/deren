// shaders/heap_probe.slang - the SLANG source of the heap-native PROBE pair (a fullscreen triangle and the
// fragment stage that reads the material table out of the heap).
//
// PRODUCES `heap_probe.vert.spv`, `heap_probe.frag.spv` AND (since the mesh-shader work, docs/mesh_shaders.md
// step 0) `heap_probe.mesh.spv`, the same names the GLSL versions produced for the first two - so the host
// side is untouched except for the mesh .spv it learns explicitly, which is unavoidable: a mesh stage is a
// different stage type, not a different source for the same binary.
//
// THIS PAIR IS THE ONE GROUP OF SHADERS THAT VERIFIES ITSELF, and that is why it is worth porting even though
// no capture-gate scenario renders it: the runtime dispatches it during initialization, reads a pixel back, and
// logs the result against a value it knows in advance (element 0 of the material table is the white default
// material this renderer reserves). The verification of a port here is a LOG COMPARISON, not a frame - and the
// MESH entry exists so the same comparison covers a mesh pipeline: same triangle, same fragment stage, same
// 4x4 readback, one different stage type.
//
// There is deliberately NO vertex buffer: what the probe tests is the FRAGMENT stage's access to the heap, and
// a vertex buffer would add a second thing that could be wrong.

#include "heap_access.slang"

// THE MATERIAL LAYOUT IS NOT DECLARED HERE ANY MORE: it is `material_record.glsl`, the one place the
// record's std430 layout lives, and this stage includes it. A second copy is a second thing to keep in
// step with the host's `static_assert(sizeof(material_record) == 80)`, and a copy that drifts indexes
// every material after the first by the wrong STRIDE - which shows up as a wrong texture on every
// material but one, not as a build error.
#include "material_record.glsl"

// The slot is a PARAMETER rather than a constant, and that is the point: the host pushes it, so the probe can
// be run a second time with a deliberately WRONG slot - a probe that could only say "fine" would pass every
// check. With no pipeline layout (what the heap flag requires) this block arrives through vkCmdPushDataEXT.
struct HeapProbePush
{
    uint material_slot; // ABSOLUTE grid slot of the material table, e.g. heap_slots_materials
};
[[vk::push_constant]] ConstantBuffer<HeapProbePush> pc;

struct heap_probe_vertex_output
{
    [[vk::location(0)]] float2 uv : UV_OUT;
    float4 position : SV_Position;
    // A LANE NO FRAGMENT STAGE READS, and it is here because a MESH stage cannot read its own outputs
    // (`E54005: cannot read values from mesh shader outputs`, measured), so the usual "keep the read alive
    // with a never-taken branch" idiom does not work: the value has to be WRITTEN somewhere. A multiply by
    // zero would be folded away with the fetch, so it is written to a location nothing consumes instead -
    // the drawn triangle is unaffected, and the fetch is real.
    [[vk::location(1)]] float heap_probe_lane : PROBE_LANE;
};

/// @brief the fullscreen triangle, synthesized from the vertex index alone
[shader("vertex")]
heap_probe_vertex_output main(uint vertex_index : SV_VertexID)
{
    const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    const float2 p = positions[vertex_index];

    heap_probe_vertex_output out;
    out.uv = (p + float2(1.0, 1.0)) * 0.5;
    out.position = float4(p, 0.0, 1.0);
    out.heap_probe_lane = 0.0;
    return out;
}

/// @brief the SAME fullscreen triangle, emitted by a MESH stage instead of a vertex stage
/// @note STEP 0 OF THE MESH-SHADER WORK (docs/mesh_shaders.md): the cheapest possible mesh stage - one
///       workgroup emits one triangle whose three positions are constants - run through the SAME fragment
///       stage and the same readback as the vertex entry above, so the two paths can be compared directly in
///       the log rather than through a screenshot. It also carries the binding-model question: a mesh stage is
///       a new execution model, and the heap fetch below is what proves the shim's handles, the two BuiltIn
///       heaps and vkCmdPushDataEXT reach it too.
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void mesh_main(out vertices heap_probe_vertex_output verts[3], out indices uint3 tris[1])
{
    SetMeshOutputCounts(3, 1);

    // The same three vertices the vertex entry derives from SV_VertexID, term for term - the constants are
    // repeated rather than recomputed from the loop index, because the first version of this decoded them out
    // of `i` and got (0,0), (2,0), (0,2): a quarter-screen triangle in the WRONG corner, which the readback
    // reported as the clear colour. A mesh stage has no SV_VertexID to share the decode with, so the
    // positional contract is written out literally and the host's readback is what checks it.
    const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    for (uint i = 0u; i < 3u; ++i)
    {
        const float2 p = positions[i];
        verts[i].uv = (p + float2(1.0, 1.0)) * 0.5;
        verts[i].position = float4(p, 0.0, 1.0);
    }

    // AND ONE HEAP READ, through the shim, so "a mesh stage can reach the heap" is measured rather than
    // assumed. `material_at(slot, 0)` is the DEFAULT material's white base colour, which the host knows the
    // value of; it is written to the lane above, which no fragment stage reads.
    const float probe = material_at(pc.material_slot, 0).base_color_factor.r;
    for (uint i = 0u; i < 3u; ++i)
    {
        verts[i].heap_probe_lane = probe;
    }

    tris[0] = uint3(0u, 1u, 2u);
}

/// @brief read element 0 of the material table at @p pc.material_slot and write its base colour
/// @note the uv is unused on purpose: the value is a constant of the heap's contents, not of the fragment's
///       position, so the whole 4x4 target must come back the same colour - which is what makes a single read
///       pixel a valid probe.
[shader("fragment")]
float4 frag_main([[vk::location(0)]] float2 uv : UV_IN) : SV_Target
{
    return float4(material_at(pc.material_slot, 0).base_color_factor.rgb, 1.0);
}
