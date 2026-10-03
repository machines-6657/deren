// shaders/shadow.slang - the SLANG source of the depth-only shadow fragment stage.
//
// PRODUCES `shadow.frag.spv`, the same name the GLSL version produced, so the host side is untouched.
// Ported from shaders/shadow.frag (now in `shaders/glsl.old/`).
//
// The stage has no color attachment: rasterization depth is all it writes, which is why the Slang entry
// point returns void and declares only its one varying. What it DOES do is discard below the material's
// alpha cutoff, so a masked caster (foliage, curtains, grates) casts a cut-out shadow.
//
// THIS LEAF SHARES NO BODY and is therefore the shape a stage takes when the shim is the only thing it
// needs: the material table and the albedo fetch go through the shared accessors (`material_at` and
// `heap_sample`), which is exactly the pair that the deferred and forward paths call too, so the shaded
// alpha edge and the shadow edge keep agreeing by construction rather than by a copied expression.
//
// The push-constant block must stay byte-identical to shadow.vert's: one pipeline shares a single
// push-constant range across both stages.

#include "heap_access.slang"
#include "mesh_geometry.slang"

// THE MATERIAL LAYOUT IS NOT DECLARED HERE ANY MORE: it is `material_record.glsl`, the one place the
// record's std430 layout lives, and this stage includes it. A second copy is a second thing to keep in
// step with the host's `static_assert(sizeof(material_record) == 80)`, and a copy that drifts indexes
// every material after the first by the wrong STRIDE - which shows up as a wrong texture on every
// material but one, not as a build error.
#include "material_record.glsl"

// ONE block for both entries, and it is the VERTEX stage's shape that is the complete one: the host pushes
// the material fields (96 bytes) and then appends its lanes, so the fragment entry reads material_index at
// offset 0 and ignores the rest. The trailing words are where the two heap indices and the cascade land, and
// the cascade MUST sit after all three lanes: the per-caster material push covers 96 + 3 lanes = 108 bytes,
// so a cascade word at 96 was overwritten by frame_slot and the pass rendered every draw into the wrong
// layer (the GLSL file records the same finding).
struct ShadowPush
{
    uint material_index; // index into the material table (the mask test's only input)
    uint flags;          // bit0: instanced draw -> model comes from the instance table
    uint skin_base;
    uint morph_base;
    uint morph_targets;
    uint morph_vertices;
    uint instance_base;
    VR_MAT4 model;
    uint frame_slot;   // PER FRAME SLOT arrays: the skin and morph blocks the vertex stage reads
    uint image_index;
    uint spare_lane;   // every stage gets three lanes; the third is the post chain's own source slot
    uint cascade;      // which shadow-map layer this pass renders into (offset 108)
    // THE MESH ENTRY'S GEOMETRY LANES (docs/mesh_shaders.md step 1), at offset 112 - after the three heap
    // index lanes and the cascade, so every field above keeps its offset. The VERTEX and FRAGMENT entries
    // declare the same block and simply never read them: the host pushes the same first 112 bytes for
    // every caster and appends these only when the mesh path is the one drawing.
    MeshGeometryLanes geometry;
};
[[vk::push_constant]] ConstantBuffer<ShadowPush> push;
#define heap_frame_slot (push.frame_slot)
#define heap_image_index (push.image_index)

// The light block is NOT declared here: the vertex stage reads its cascade matrices through
// `light_matrix_at` (see heap_access.slang and docs/slang_migration.md section 9 for why the block member
// cannot be used), so this leaf needs no `LightUBO` struct at all.

/// @brief the alphaMode MASK test: discard below the material's alpha cutoff
/// @note cost is one uniform branch per draw plus one texture fetch for the masked materials only: the
///       material record's flag bit4 is a push-constant-decided value, so opaque casters never sample the
///       albedo texture at all.
[shader("fragment")]
void main([[vk::location(0)]] float2 v_uv : TEXCOORD0)
{
    const Material mat = material_at(heap_slots_materials, push.material_index);
    if ((mat.flags & 16u) == 0u)
    {
        return; // OPAQUE / BLEND: nothing to test, so the albedo texture is never sampled
    }
    // same expression as pbr.frag: base_color.a is factor.a * albedo.a
    if (mat.base_color_factor.a * heap_sample(mat.tex_indices.x, v_uv).a < mat.alpha_cutoff)
    {
        discard;
    }
}

// =============================================================================================================
// THE VERTEX STAGE (entry `vertex_main`), in the same file because it shares the push block above and the
// pass's whole point is that both stages agree on where the geometry is. It transforms the model into the
// LIGHT's orthographic clip space, so rasterization writes the depth seen from the light; skinning and
// morphing are applied exactly as pbr.vert does them, which is what keeps the shadow silhouette on the
// geometry that is actually drawn.
// =============================================================================================================

struct shadow_vertex_output
{
    [[vk::location(0)]] float2 uv : UV_OUT; // consumed by the MASK test above; the depth pass has no other use
    float4 position : SV_Position;
};

/// @brief morph, skin, transform into light clip space, and pass the albedo UV through
/// @param v the vertex record, from the input assembler (the vertex entry) or fetched by address (the
///        mesh entry) - the two paths differ ONLY in how the bytes arrive
/// @param vertex_index SV_VertexID in the vertex entry, the fetched index in the mesh entry
/// @param instance_index SV_InstanceID in the vertex entry, the mesh workgroup's Y in the mesh entry
/// @note the morph/skin order and math are identical to pbr.vert; the only differences are the final
///       projection (the cascade's light view-projection instead of camera.proj * camera.view) and the
///       UV passthrough.
/// @note THE NORMAL IS READ HERE even though a depth pass has no use for it, and that is not dead code:
///       the raster pipelines derive their vertex INPUT STATE from the vertex stage's declarations
///       (deren::vulkan::make_pipeline), so a normal that Slang can prove unused is dropped from the module and
///       the input's stride silently shrinks by 12 bytes - every vertex after the first would then be
///       read from the wrong place. The never-taken branch is what keeps it declared; it is the same
///       guard the pre-mesh version of this file carried, now shared by both entries.
shadow_vertex_output shadow_shade_vertex(const MeshVertex v, const uint vertex_index, const uint instance_index)
{
    // morph blend first (same layout as pbr.vert): only the POSITION deltas matter in a depth pass
    float4 local_pos = float4(v.position, 1.0);
    if (push.morph_targets > 0u)
    {
        const uint vert = vertex_index;
        // SPARSE morph block - the layout and the reasoning are on pbr_shade_vertex (pbr.slang); this pass
        // must walk it identically or the shadow silhouette stops matching the shaded one. Only the
        // POSITION deltas matter in a depth pass, so the entry stride is still 7 but only 3 are read.
        const uint offset_base = push.morph_base + push.morph_targets * 2u;
        const uint entry_base = offset_base + push.morph_vertices + 1u;
        float3 pos_delta = float3(0.0, 0.0, 0.0);
        const uint first = uint(morph_value_at(heap_morph_slot, offset_base + vert));
        const uint last = uint(morph_value_at(heap_morph_slot, offset_base + vert + 1u));
        for (uint k = first; k < last; ++k)
        {
            const uint e = entry_base + k * 7u;
            const uint t = uint(morph_value_at(heap_morph_slot, e));
            const float w = morph_value_at(heap_morph_slot, push.morph_base + t);
            pos_delta += w * float3(morph_value_at(heap_morph_slot, e + 1u), morph_value_at(heap_morph_slot, e + 2u), morph_value_at(heap_morph_slot, e + 3u));
        }
        local_pos = float4(v.position + pos_delta, 1.0);
    }

    // skinning (identical to pbr.vert: weighted joint transforms, identity block at skin_base 0)
    const float wsum = v.weights.x + v.weights.y + v.weights.z + v.weights.w;
    if (wsum > 0.0)
    {
        float4 pos = float4(0.0, 0.0, 0.0, 0.0);
        pos += v.weights.x * (skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.x) * local_pos);
        pos += v.weights.y * (skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.y) * local_pos);
        pos += v.weights.z * (skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.z) * local_pos);
        pos += v.weights.w * (skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.w) * local_pos);
        local_pos = pos / wsum;
    }

    // VR_MAT4 for the local as well, so the ternary is between two values of the same type (a mismatch is a
    // conversion, and that conversion is a transpose - see docs/slang_migration.md section 6).
    const VR_MAT4 world = (push.flags & 1u) != 0u ? instance_transform_at(heap_instance_slot, push.instance_base + instance_index) : push.model;
    const float4 world_pos = world * local_pos;

    shadow_vertex_output o;
    // light_matrix_at, not `light_at(...).light_view_proj[...]`: see heap_access.slang (a matrix ARRAY inside
    // a Slang block loses its majorness decoration, and the block read came back unusable).
    o.position = light_matrix_at(heap_light_slot, push.cascade) * world_pos;
    o.uv = v.uv;

    // Keep the input without a role in the depth pass (the normal) alive so the vertex input layout - and
    // thus the bound buffer stride - stays identical to pbr.vert's (64 bytes, locations 0,1,2,4,5). This
    // branch can never run.
    if (isnan(v.position.x) && isinf(v.normal.x))
    {
        o.position = float4(0.0, 0.0, 0.0, 0.0);
    }
    return o;
}

// =============================================================================================================
// THE VERTEX ENTRY IS GONE (docs/mesh_shaders.md step 4). `shadow.vert.spv` was this file's input-assembler half -
// the same five attributes pbr.vert declared, with the same vertex input layout - and the renderer no longer builds a
// vertex pipeline for any geometry stage, so it was removed rather than left compiled-but-unreachable.
// `shadow_shade_vertex` stays: the mesh stages below are its only callers now.
// =============================================================================================================

// =============================================================================================================
// THE MESH STAGE (entry `mesh_main`), which replaced the input assembler AND the vertex entry above:
// docs/mesh_shaders.md step 1. It fetches the same vertices the input assembler would have bound (through
// the geometry lanes the host pushes), projects them with THE SAME shared body, and emits exactly the
// triangles the draw's index window describes - and it was accepted on the gate being byte-identical BEFORE
// the vertex entry was removed, which is what makes the removal a removal.
//
// IT WAS BUILT TO BE MEASURED, NOT TO BE FASTER: one workgroup emits up to 85 triangles with 1 invocation
// doing the work, which is strictly worse than the vertex path was. What it established is the two facts a
// meshlet path (step 3) is built on: that this renderer can feed geometry to a mesh stage at all, and how
// much of a draw one workgroup's output budget covers.
//
// `group_id.y` IS THE INSTANCE COUNT: vkCmdDrawMeshTasksEXT has no instanceCount and a mesh stage has no
// SV_InstanceID, so the host dispatches (triangle groups, instance_count, 1) and the instanced draw this
// engine used to issue as one vkCmdDrawIndexed is now one dispatch whose Y is the instance. The transform body
// above reads it exactly as the vertex entry read SV_InstanceID, so `push.flags & 1` keeps its meaning.
// =============================================================================================================
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void mesh_main(out vertices shadow_vertex_output verts[mesh_vertices_per_workgroup], out indices uint3 tris[mesh_triangles_per_workgroup], uint3 group_id : SV_GroupID)
{
    const MeshGeometryLanes geometry = push.geometry;
    const uint triangle_count = geometry.index_count / mesh_indices_per_triangle;
    // this workgroup's first triangle and how many of the draw's triangles are left for it
    const uint first_triangle = group_id.x * mesh_triangles_per_workgroup;
    const uint remaining = first_triangle < triangle_count ? triangle_count - first_triangle : 0u;
    const uint emit = min(remaining, mesh_triangles_per_workgroup);

    // ONE SetMeshOutputCounts for the whole workgroup, before any output is written.
    SetMeshOutputCounts(emit * mesh_indices_per_triangle, emit);

    for (uint t = 0u; t < emit; ++t)
    {
        for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner)
        {
            // the triangle's corner vertex, exactly as the input assembler would have assembled it: the
            // index at (first_index + triangle * 3 + corner), plus the draw's base vertex
            const uint raw_index = mesh_index_at(geometry, geometry.first_index + (first_triangle + t) * mesh_indices_per_triangle + corner);
            const uint vertex_index = uint(int(raw_index) + geometry.base_vertex);
            const MeshVertex v = mesh_vertex_at(geometry, vertex_index);
            verts[t * mesh_indices_per_triangle + corner] = shadow_shade_vertex(v, vertex_index, group_id.y);
        }
        tris[t] = uint3(t * mesh_indices_per_triangle + 0u, t * mesh_indices_per_triangle + 1u, t * mesh_indices_per_triangle + 2u);
    }
}


// =============================================================================================================
// THE MESHLET ENTRY (entry `meshlet_main`): ONE workgroup per MESHLET, its window read out of the table.
//
// `group_id.x` is the meshlet's index inside this draw's run (the host dispatches `meshlet_count` groups), and
// `group_id.y` is still the instance. The lanes carry what no record can know - the draw's vertex/index buffer
// addresses and the index width - while the record carries what the lanes no longer describe: the window.
//
// THE EMIT LOOP IS `mesh_main`'s, written out rather than shared: the two differ in where the window comes from
// and nothing else, and threading an `out vertices` array through a Slang function boundary is a construct this
// project has not measured. What must stay IDENTICAL is the projection - both call `shadow_shade_vertex` - because
// that is what makes the capture gate a valid acceptance for either path.
//
// THE CLAMP IS NOT CAUTION, IT IS THE MEASURED ONE: `SetMeshOutputCounts` may never exceed the device's
// 85-triangle / 255-vertex budget, and a record read at the wrong index would otherwise ask for an unbounded
// output - which hangs the GPU instead of drawing something wrong.
// =============================================================================================================
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void meshlet_main(out vertices shadow_vertex_output verts[mesh_vertices_per_workgroup], out indices uint3 tris[mesh_triangles_per_workgroup], uint3 group_id : SV_GroupID)
{
    const MeshGeometryLanes geometry = push.geometry;
    const MeshletRecord meshlet = meshlet_at(geometry.first_index + group_id.x);
    // ONE ATOMIC PER WORKGROUP: the shadow pass and the G-buffer share the counters, so the totals below are the
    // session's and the doc quotes them per pass where it can (docs/mesh_shaders.md step 3's measurement)
    mesh_stat_add(push.frame_slot, mesh_stat_workgroups, 1u);

    // ---- PER-MESHLET CULLING, and it happens HERE rather than in a task stage (docs/mesh_shaders.md step 3) ----
    // A task stage cannot be given push data on this Slang, so the culling lives in the stage that can read the
    // block: the mesh entry rejects a whole meshlet by emitting NOTHING (`SetMeshOutputCounts(0, 0)`), which
    // removes every vertex and triangle it would have contributed. What a task stage would add is skipping the
    // workgroup LAUNCH, not the effect - so the feature is available, one dispatch argument at a time.
    //
    // The sphere is object space: the draw's world transform (the push's model matrix, or the instance table for an
    // instanced caster) brings it to world, the cascade's light matrix to clip space, and the radius is scaled by
    // an upper bound on both matrices' axis scales - so a rejected meshlet cannot contain a vertex inside the
    // frustum, and the depth map it is left out of is the one the kept meshlets would have written anyway.
    // ---- A DEFORMING DRAW IS NEVER CULLED PER MESHLET, and that is a correctness rule rather than caution ----
    // A meshlet's sphere and cone are built from the BIND-POSE vertices, while this draw's vertices are skinned
    // (joint matrices, `skin_base != 0`) or morphed (`morph_targets > 0`) - so the bounds describe geometry the
    // stage is not about to emit, and a cull against them can remove something visible. The gate's fixtures happen
    // to deform little enough that it did not show, which is exactly why the rule is stated here instead of being
    // left to a measurement: what a meshlet path wants for deformable geometry is bounds per FRAME, not per draw.
    const bool deforming = push.morph_targets > 0u || push.skin_base != 0u;
    const VR_MAT4 world = (push.flags & 1u) != 0u ? instance_transform_at(heap_instance_slot, push.instance_base + group_id.y) : push.model;
    const VR_MAT4 light = light_matrix_at(heap_light_slot, push.cascade);
    const float4 clip = light * (world * float4(meshlet.center, 1.0));
    const float radius = meshlet.radius * matrix_max_axis_scale(world) * matrix_max_axis_scale(light);
    if (!deforming && !clip_sphere_visible(clip, radius))
    {
        mesh_stat_add(push.frame_slot, mesh_stat_culled, 1u);
        SetMeshOutputCounts(0u, 0u); // culled: no vertices, no triangles, nothing rasterized
        return;
    }

    const uint triangle_count = min(meshlet.index_count, mesh_triangles_per_workgroup * mesh_indices_per_triangle) / mesh_indices_per_triangle;

    SetMeshOutputCounts(triangle_count * mesh_indices_per_triangle, triangle_count);
    mesh_stat_add(push.frame_slot, mesh_stat_emitted, 1u);
    mesh_stat_add(push.frame_slot, mesh_stat_triangles, triangle_count);

    for (uint t = 0u; t < triangle_count; ++t)
    {
        for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner)
        {
            const uint raw_index = mesh_index_at(geometry, meshlet.first_index + t * mesh_indices_per_triangle + corner);
            const uint vertex_index = uint(int(raw_index) + meshlet.base_vertex);
            const MeshVertex v = mesh_vertex_at(geometry, vertex_index);
            verts[t * mesh_indices_per_triangle + corner] = shadow_shade_vertex(v, vertex_index, group_id.y);
        }
        tris[t] = uint3(t * mesh_indices_per_triangle + 0u, t * mesh_indices_per_triangle + 1u, t * mesh_indices_per_triangle + 2u);
    }
}