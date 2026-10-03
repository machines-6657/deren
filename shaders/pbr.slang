// shaders/pbr.slang - the SLANG source of the FORWARD PBR shader PAIR (fragment + vertex).
//
// PRODUCES `pbr.frag.spv` AND `pbr.vert.spv`, the same names the GLSL versions produced, so the host side is
// untouched. Ported from shaders/pbr.frag and shaders/pbr.vert (now in `shaders/glsl.old/`): the
// bodies are the GLSL bodies - gather the surface, fill a shade_input, call shade_surface; and morph blend ->
// skin -> world transform - and what changes is only what a different language requires. In THESE stages
// that is exactly three things:
//
//   - the entry point's varyings and its output carry `[[vk::location(n)]]` / `SV_Target` instead of
//     `layout(location = n)`;
//   - `gl_FragCoord.xy` becomes the `SV_Position` parameter's `.xy`, which is the same value (pixel
//     centres, y down) - and it is why shading.glsl takes the pixel as a PARAMETER rather than reading
//     gl_FragCoord itself: the same shade path runs in compute stages on the deferred side, where that
//     builtin does not exist;
//   - the heap is reached through the shared accessors, which is the shim's whole job.
//
// THIS IS THE STAGE THAT PROVES THE WHOLE SHADING PATH COMPILES UNDER SLANG. gbuffer.slang (the previous
// port) exercises gather_surface and the material table; this one calls shade_surface, i.e. the sun and the
// cascaded shadow maps, the punctual and clustered light loops, the split-sum IBL with env_cube /
// irradiance / BRDF LUT, and the selectable BRDF and cel-shading branches - every fetch helper in
// heap_access.slang, in the order a lit pixel reaches them.
//
// THE INCLUDE ORDER IS PART OF THE CONTRACT (see docs/slang_migration.md section 8): the shim first, then
// surface.glsl, which declares the push-constant block and the heap_frame_slot / heap_image_index aliases
// the slot macros in heap_slot_constants.glsl expand against.

#include "heap_access.slang"
#include "surface.glsl"
#include "shading.glsl"

// -------------------------------------------------------------------------------------------------------------
// A DOXYGEN GUARD, and it is for the documentation tool rather than for any compiler.
//
// Doxygen parses this file as C++ (see Doxyfile's EXTENSION_MAPPING) and reads the fragment entry's trailing
// `) : SV_Target` as the beginning of a CONSTRUCTOR-INITIALIZER LIST - so it warns about the next `;` it meets
// ("Found ';' while parsing initializer list") and loses the rest of the file's documentation. Slang will not take
// the equivalent attribute spelling either (`[[vk::location(0)]]` on the return type is `E31002: invalid attribute
// placement`, measured), so the only way to keep both tools happy is to let each of them skip what it cannot read:
// `DOXYGEN` is DEFINED for Doxygen alone (Doxyfile's PREDEFINED), so the compiler preprocesses this region away and
// sees the stage exactly as it was - the module it produces is byte-identical (verified by hash).
//
// The entry's own documentation is inside the guard with it, so Doxygen has no dangling comment block to attach to
// the wrong symbol; the file's header describes the pair.
#ifndef DOXYGEN

/// @brief shade one fragment: gather the surface, light it, apply the glTF output alpha
/// @note glTF alpha semantics: only alphaMode BLEND materials carry real coverage in the output alpha.
///       OPAQUE and MASK outputs must write alpha = 1 (their base color alpha is ignored by the spec),
///       otherwise the always-on blending below would make e.g. an albedo texture with an alpha channel
///       unexpectedly translucent. BLEND keeps base_color.a.
[shader("fragment")]
float4 main(
    [[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
    [[vk::location(1)]] float3 v_normal : TEXCOORD1,
    [[vk::location(2)]] float2 v_uv : TEXCOORD2,
    float4 sv_position : SV_Position) : SV_Target
{
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv);

    shade_input si;
    si.pixel = int2(sv_position.xy); // gl_FragCoord.xy: the cluster grid's tile coordinate (see shade_input)
    si.world_pos = v_world_pos;
    si.normal = s.normal;
    si.albedo = s.albedo;
    si.emissive = s.emissive;
    si.metallic = s.metallic;
    si.roughness = s.roughness;
    si.ao = s.ao;
    // The forward path adds the diffuse ambient itself; the deferred path leaves it to the resolve.

    const float out_alpha = ((s.flags & 32u) != 0u) ? s.alpha : 1.0;
    return float4(shade_surface(si), out_alpha);
}

#endif // DOXYGEN

// =============================================================================================================
// THE VERTEX STAGE, in the same file because it is the same CPU/GPU contract: the push block carries the
// per-draw state (it is declared by surface.glsl above, which is why `motion_base` lives in it) and the heap
// arrays carry the per-frame one. THIS IS THE STAGE THE G-BUFFER PASS LOADS FOR EVERY OPAQUE DRAW, so unlike
// `pbr.frag` - which only the transparent pass's forward pipeline records - this entry point is on the gate's
// path in all fourteen scenarios.
//
// The body is the GLSL body: morph blend (local deltas, current and previous weights), then skinning (the
// four joint matrices selected by the vertex's joints, weighted, through BOTH the current and the previous
// frame's blocks), then the world transform, with the previous world position projected by the previous
// frame's world matrix. The two "previous" reads are what give a DEFORMING vertex a motion vector instead of
// only its node's rigid motion (see docs/deformation_motion_vectors.md).
// =============================================================================================================

struct pbr_vertex_output
{
    [[vk::location(0)]] float3 world_pos : WORLD_POS;
    [[vk::location(1)]] float3 normal : WORLD_NORMAL;
    [[vk::location(2)]] float2 uv : UV_OUT;
    [[vk::location(3)]] float3 prev_world_pos : PREV_WORLD_POS;
    float4 position : SV_Position;
};

/// @brief morph blend -> skin -> world transform; the body BOTH geometry entries share
/// @param v the vertex record, from the input assembler (the vertex entry) or fetched by address (the mesh entry,
///        docs/mesh_shaders.md step 2) - the two paths differ ONLY in how the bytes arrive
/// @param vertex_index SV_VertexID in the vertex entry, the fetched index in the mesh entry (the morph block is
///        indexed by it)
/// @param instance_index SV_InstanceID in the vertex entry, the mesh workgroup's Y in the mesh entry
/// @note each stage is skipped by a uniform branch when the primitive does not need it
///       (push.morph_targets == 0, weight sum == 0, flag bit0 clear), so one pipeline serves static,
///       skinned, morphed and instanced geometry without variants.
pbr_vertex_output pbr_shade_vertex(const MeshVertex v, const uint vertex_index, const uint instance_index)
{
    // morph blend first (local deltas + active target weights; skipped when not morphable)
    float4 local_pos = float4(v.position, 1.0);
    // ... and where this vertex was in object space BEFORE the morph blend, which is the same position
    // whenever the primitive is not morphable (the copy is why the branch below can be a uniform one).
    float4 prev_local_pos = local_pos;
    float3 morph_normal = v.normal;
    if (push.morph_targets > 0u)
    {
        const uint vert = vertex_index;
        // SPARSE morph block, layout written by animation::controller: the weights come FIRST so that
        // morph_base / morph_targets / morph_vertices alone locate every other part - adding a field to
        // the shared push block would have meant touching every pass for one number.
        //   [weights: targets][previous weights: targets][offsets: verts + 1][entries: 7 floats each]
        // An entry is (target, dpos.xyz, dnrm.xyz) and EXISTS ONLY WHERE THE DELTA IS NON-ZERO, which is
        // what makes the block proportional to the deformation instead of to verts * targets: a character
        // with 43 facial morphs over a 70k-vertex shared buffer needs ~1 MB here, not the ~70 MB the old
        // dense stride asked for.
        const uint weight_base = push.morph_base;
        const uint offset_base = push.morph_base + push.morph_targets * 2u;
        const uint entry_base = offset_base + push.morph_vertices + 1u;
        float3 pos_delta = float3(0.0, 0.0, 0.0);
        float3 prev_pos_delta = float3(0.0, 0.0, 0.0);
        float3 nrm_delta = float3(0.0, 0.0, 0.0);
        // this vertex's slice of the entry list: consecutive, so the walk is O(moved), not O(targets)
        const uint first = uint(morph_value_at(heap_morph_slot, offset_base + vert));
        const uint last = uint(morph_value_at(heap_morph_slot, offset_base + vert + 1u));
        for (uint k = first; k < last; ++k)
        {
            const uint e = entry_base + k * 7u;
            const uint t = uint(morph_value_at(heap_morph_slot, e));
            const float w = morph_value_at(heap_morph_slot, weight_base + t);
            const float w_prev = morph_value_at(heap_morph_slot, weight_base + push.morph_targets + t);
            const float3 dpos = float3(morph_value_at(heap_morph_slot, e + 1u), morph_value_at(heap_morph_slot, e + 2u), morph_value_at(heap_morph_slot, e + 3u));
            const float3 dnrm = float3(morph_value_at(heap_morph_slot, e + 4u), morph_value_at(heap_morph_slot, e + 5u), morph_value_at(heap_morph_slot, e + 6u));
            pos_delta += w * dpos;
            prev_pos_delta += w_prev * dpos;
            nrm_delta += w * dnrm;
        }
        local_pos = float4(v.position + pos_delta, 1.0);
        // the SAME blend with the previous weights: the morph half of the deformation term, and the deltas
        // are read once because they are static - only the weights moved between the two frames
        prev_local_pos = float4(v.position + prev_pos_delta, 1.0);
        morph_normal = v.normal + nrm_delta;
    }

    // skinning after morph: blend the four joint transforms with the vertex weights (the identity block at
    // skin_base 0 leaves unskinned vertices unchanged). Normals use the weighted rotation part.
    float3 skinned_normal = morph_normal;
    const float wsum = v.weights.x + v.weights.y + v.weights.z + v.weights.w;
    if (wsum > 0.0)
    {
        float4 pos = float4(0.0, 0.0, 0.0, 0.0);
        float4 prev_pos = float4(0.0, 0.0, 0.0, 0.0);
        float3 nrm = float3(0.0, 0.0, 0.0);
        pos += v.weights.x * (skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.x) * local_pos);
        pos += v.weights.y * (skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.y) * local_pos);
        pos += v.weights.z * (skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.z) * local_pos);
        pos += v.weights.w * (skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.w) * local_pos);
        // ... and the SAME blend through the matrices the previous frame drew with, applied to the position
        // the previous frame morphed to: the two halves compose as skin_previous(morph_previous(v)), which is
        // the deformation term for a mesh that both morphs and skins.
        prev_pos += v.weights.x * (skin_matrix_previous_at(heap_skin_previous_slot, push.skin_base + v.joints.x) * prev_local_pos);
        prev_pos += v.weights.y * (skin_matrix_previous_at(heap_skin_previous_slot, push.skin_base + v.joints.y) * prev_local_pos);
        prev_pos += v.weights.z * (skin_matrix_previous_at(heap_skin_previous_slot, push.skin_base + v.joints.z) * prev_local_pos);
        prev_pos += v.weights.w * (skin_matrix_previous_at(heap_skin_previous_slot, push.skin_base + v.joints.w) * prev_local_pos);
        nrm += v.weights.x * mat3(skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.x)) * morph_normal;
        nrm += v.weights.y * mat3(skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.y)) * morph_normal;
        nrm += v.weights.z * mat3(skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.z)) * morph_normal;
        nrm += v.weights.w * mat3(skin_matrix_at(heap_skin_slot, push.skin_base + v.joints.w)) * morph_normal;
        local_pos = pos / wsum;
        prev_local_pos = prev_pos / wsum;
        skinned_normal = nrm / wsum;
    }

    // VR_MAT4 for the local as well, so the ternary is between two values of the SAME type: assigning a
    // row-major-declared matrix into a differently-declared local is a conversion, and that conversion is a
    // transpose (the same trap as the push block's `model` above).
    const VR_MAT4 world = (push.flags & 1u) != 0u ? instance_transform_at(heap_instance_slot, push.instance_base + instance_index) : push.model;
    const float4 world_pos = world * local_pos;

    pbr_vertex_output o;
    o.world_pos = world_pos.xyz;
    o.normal = normalize(mat3(world) * skinned_normal);
    o.uv = v.uv;

    // The previous LOCAL position through the previous frame's world matrix. The node's rigid motion comes
    // from that matrix, and a vertex's own movement inside its object space comes from prev_local_pos above -
    // the previous MORPH weights and the previous SKIN matrices, composed in that order. Which is why this is
    // no longer "the current local position through last frame's model": that version reported a motion
    // vector of exactly zero for a mesh whose node never moved, no matter how it deformed.
    // NOT COVERED HERE, and not by anything in this stage: alpha-blended geometry, which is composited
    // outside the G-buffer and so writes no motion vector at all.
    const uint motion_index = push.motion_base + (((push.flags & 1u) != 0u) ? instance_index : 0u);
    o.prev_world_pos = (previous_transform_at(heap_previous_slot, motion_index) * prev_local_pos).xyz;

    o.position = camera_at(heap_camera_slot).proj * camera_at(heap_camera_slot).view * world_pos;
    return o;
}

// =============================================================================================================
// THE VERTEX ENTRY IS GONE (docs/mesh_shaders.md step 4). `pbr.vert.spv` was the input assembler's half of this
// file: five vertex attributes, SV_VertexID and SV_InstanceID, handed to the shared body above as one record. The
// renderer no longer builds a vertex pipeline for any geometry stage, so the entry was removed rather than left
// compiled-but-unreachable - and `pbr_shade_vertex` stays, because every mesh stage below still calls it.
// =============================================================================================================

// =============================================================================================================
// THE MESH STAGE (entry `mesh_main`), which replaced the input assembler AND the vertex entry above:
// docs/mesh_shaders.md step 2. It fetches the very vertices the input assembler would have bound - through the
// geometry lanes the host pushes into this stage's block - projects them with THE SAME body, and emits exactly the
// triangles the draw's index window describes. The capture gate is what decides that it comes out identical, and it
// was accepted on that basis BEFORE the vertex entry was removed, which is what makes the removal a removal.
//
// IT IS THE SHADOW PASS'S STAGE ONE FAMILY LARGER: the same loop, the same 85-triangle workgroup budget (see
// shaders/mesh_geometry.slang), the same `SV_GroupID.y` as the instance index - and the difference is what the
// shared body does with the vertex, which here is the whole shading-relevant transform: morph, skin, the
// previous-frame position that becomes TAA's motion vector, and the camera projection.
// =============================================================================================================
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void mesh_main(out vertices pbr_vertex_output verts[mesh_vertices_per_workgroup], out indices uint3 tris[mesh_triangles_per_workgroup], uint3 group_id : SV_GroupID)
{
    const MeshGeometryLanes geometry = push.geometry;
    const uint triangle_count = geometry.index_count / mesh_indices_per_triangle;
    // this workgroup's first triangle, and how many of the draw's triangles are left for it
    const uint first_triangle = group_id.x * mesh_triangles_per_workgroup;
    const uint remaining = first_triangle < triangle_count ? triangle_count - first_triangle : 0u;
    const uint emit = min(remaining, mesh_triangles_per_workgroup);

    // ONE SetMeshOutputCounts for the whole workgroup, before any output is written.
    SetMeshOutputCounts(emit * mesh_indices_per_triangle, emit);
    // ... and the same counter the meshLET entry adds to, so the two forms' workgroup counts can be compared on one
    // run (docs/mesh_shaders.md step 3's measurement)
    mesh_stat_add(push.frame_slot, mesh_stat_mesh_workgroups, 1u);

    for (uint t = 0u; t < emit; ++t)
    {
        for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner)
        {
            const uint raw_index = mesh_index_at(geometry, geometry.first_index + (first_triangle + t) * mesh_indices_per_triangle + corner);
            const uint vertex_index = uint(int(raw_index) + geometry.base_vertex);
            const MeshVertex v = mesh_vertex_at(geometry, vertex_index);
            verts[t * mesh_indices_per_triangle + corner] = pbr_shade_vertex(v, vertex_index, group_id.y);
        }
        tris[t] = uint3(t * mesh_indices_per_triangle + 0u, t * mesh_indices_per_triangle + 1u, t * mesh_indices_per_triangle + 2u);
    }
}

// =============================================================================================================
// THE OUTLINE MESH ENTRY (entry `outline_mesh_main`): THE ARTICLE'S OUTLINE, whose geometry half is the author's
// `MyZmdOutlineShader.shader:134-170` and whose colour half is `shaders/outline.slang`.
//
// IT IS `mesh_main` WITH ONE STEP ADDED, and that is the design rather than an economy: the hull must be skinned,
// morphed and placed EXACTLY like the surface it outlines, or the outline detaches from the character the moment
// either deforms. So the loop below is `mesh_main`'s loop, the projection is `pbr_shade_vertex`, and the addition
// is the author's expansion applied to the clip position before the vertex is written out. A mesh stage (rather
// than a vertex stage) is the only shape available: the vertex geometry path was removed (see the note above the
// mesh stage), and the author's expansion is per-vertex behaviour that fits inside the same triangle loop.
//
// THE AUTHOR'S ARITHMETIC, in its own order (`:134-170`), with the two substitutions this port has to make:
//
//   float2 scaledScreenParams = GetScaledScreenParams(); float ScaleX = abs(x/y);        <- the viewport aspect
//   float weightN = sqrt(saturate(1.0 - dot(v.uv1.xy, v.uv1.xy)));                       <- uv1 == 0 for chen
//   float3 smoothNormalOS = v.uv1.x * tangent + v.uv1.y * biTangent + weightN * normal;  <- hence == v.normal
//   float3 smoothNormalVS = mul((float3x3)UNITY_MATRIX_IT_MV, smoothNormalOS);
//   float2 clipNormal2D = mul((float2x3)UNITY_MATRIX_P, smoothNormalVS);
//   float2 normal2D = normalize(clipNormal2D); normal2D.x /= ScaleX;
//   float depthScale = o.positionHCS.w; float outlineDepthClamp = min(depthScale, 20.0);
//   float depthRefine = lerp(1, _ZMinRefine, smoothstep(1, 12, depthScale));
//   float2 offset = normal2D * (_OutlineWidth * outlineDepthClamp * 0.01 * depthRefine);
//   o.positionHCS.xy += offset;
//
//  1. THE SMOOTH NORMAL IS `o.normal`. The author reads a SECOND uv set in which an artist baked a smoothed
//     normal, and computes `weightN = sqrt(saturate(1 - |uv1|^2))`; with `uv1 = (0,0)` that is `weightN = 1` and
//     the sum ADDS UP TO `v.normal` - an algebraic identity, not an approximation. Every mesh this port loads
//     has no uv1 (`hasUV1 = False` on chen's meshes), so the identity is the whole path, and `o.normal` is the
//     mesh's world-space normal (already the world-transformed, skinned, morphed one - `pbr_shade_vertex`).
//  2. THE VIEW SPACE NORMAL IS `mat3(view) * o.normal`. The author's `UNITY_MATRIX_IT_MV` is the inverse
//     transpose of the model-view matrix applied to an OBJECT-space normal; this port's normal is already world
//     space, and for the rigid view transform the inverse transpose of its 3x3 IS its 3x3. THE SPELLING MATTERS
//     AND WAS MEASURED: `mat3(M) * v` is the GLSL product and `mul(M, v)` on this port's `row_major float4x4`
//     emits the TRANSPOSED one (see the note beside VR_SLANG_SOURCES' flags) - the same reason
//     `pbr_shade_vertex`'s projection reads `proj * view * world_pos` and not `mul`.
//
// `ScaleX`: the author's `GetScaledScreenParams()` is the RENDER-TARGET size, which this renderer does not
// publish to a shader; what it does publish is the projection the frame is built with, and for a perspective
// projection the ratio `proj[1][1] / proj[0][0]` IS the viewport's aspect ratio. THAT IS AN EQUIVALENT QUANTITY
// AND NOT `_ScreenParams` ITSELF: a render scale below 1.0 scales BOTH screen dimensions, so the aspect - the
// only thing the author divides by - is the same number. The `abs` is not decoration: this engine's projection
// carries Vulkan's Y flip, so `proj[1][1]` is NEGATIVE, and an unsigned aspect would flip the hull inside out.
//
// `0.01` IS THE AUTHOR'S OWN CONSTANT (`:168`, verbatim in the source - it was the PDF's one truncated scalar
// and the source settled it), NOT a port fudge.
//
// `_ZMinRefine` IS THE ARTICLE'S DEFAULT (0.4, `:17`): the game's parameter table carries no such row (measured
// with `deren-ab/outline_rows.py`), so the constant stands as the article states it.
//
// `_ZBias` IS NOT IMPLEMENTED, and that is a value rather than an omission: `:8` declares its default 0.0, and
// `:172-178` adds `positionHCS.z -= _ZBias` (REVERSED_Z) or `+= _ZBias` - an exact zero for every material at
// the default. The game's `_OutlineOffsetZ` (0.05 on chen's cloth, 0.22 on the face, 1.0 on cloth_02) has NO
// confirmed mapping to it in this port, so nothing is guessed into the term.
//
// THE GATE IS THE WIDTH ITSELF (`> 0`), read from the material's outline lane, and it is per DRAW rather than
// per material: `push.material_index` is this draw's record, so the branch is UNIFORM across the workgroup. A
// material whose width is 0 still emits its vertices and simply is not expanded - which is what lets the whole
// outline pass be recorded in ONE batch: the game's own `chen` cloth_02 states `_OutlineWidth = 0.0` (measured),
// so "this material has no outline" is data rather than a host-side list.
// =============================================================================================================
/// `deren::vulkan::toon_colour_lane::outline_edge`: the material's outline lane in `heap_slots::toon_colours`, whose
/// `.rgb` is the game's `_OutlineTintColor` and whose `.w` is its `_OutlineWidth`. Spelled out here (and again in
/// `outline.slang`) because a shader cannot include a C++ enum; the STRIDE below is the same quantity as
/// `character_forward.slang`'s `character_toon_colour_lanes`, which is the spelling the host's
/// `toon_colour_lane::count` is checked against by `test_toon_material_sidecar`.
static const uint pbr_outline_edge_lane = 3u;
// THE STRIDE IS THE HOST'S LANE COUNT, so it is 29 (`deren::vulkan::toon_colour_lane::count`) and NOT the number of lanes
// THIS STAGE reads - which is one. A stride left at an older count would address a NEIGHBOURING MATERIAL's lane:
// the hull of material 4 would be expanded by material 1's width, silently, with the frame merely looking wrong.
// `test_toon_material_sidecar` pins this spelling against `character_toon_colour_lanes`. STEP 7 MOVED BOTH, and
// `outline.slang` inherits this one through its `#include` of `pbr.slang`; STEP 8 MOVED THEM AGAIN for
// `_GooNormalStrength`, the surface stage's twenty-fifth lane, which this mesh stage does not read; STEP 10
// MOVED THEM ONCE MORE for `_GooAnisoGate`, the surface stage's twenty-sixth, which this one does not read either;
// and STEP 12 MOVED THEM ONCE MORE for `_GooAnisoRough`, the surface stage's twenty-seventh (the anisotropic
// lobe's two roughnesses), which this one does not read either.
// STEP 13 MOVED THEM ONCE MORE for `_GooRSScalars` / `_GooRSTint`, the surface stage's twenty-eighth and
// twenty-ninth lanes (mechanism table #14, `RS EFF`), which this pass must NOT read: the RS term belongs to the
// stage that owns the emission term and adding it to the outline hull would make the outline carry a colour the
// pixel behind it does not.
// STEP 15 MOVED THEM ONCE MORE for `_GooRSArm0`, the surface stage's thirtieth lane (the same mechanism's `armA`
// sockets: `RS_Index` / `RS Strength` / `Layer weight Value` / `Layer weight Value Offset`), which this pass must
// NOT read either, for the reason immediately above.
static const uint pbr_toon_colour_lanes = 30u;

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void outline_mesh_main(out vertices pbr_vertex_output verts[mesh_vertices_per_workgroup], out indices uint3 tris[mesh_triangles_per_workgroup], uint3 group_id : SV_GroupID)
{
    const MeshGeometryLanes geometry = push.geometry;
    const uint triangle_count = geometry.index_count / mesh_indices_per_triangle;
    // this workgroup's first triangle, and how many of the draw's triangles are left for it - `mesh_main`'s own
    // window arithmetic, kept identical on purpose: the hull's triangles must be the surface's triangles.
    const uint first_triangle = group_id.x * mesh_triangles_per_workgroup;
    const uint remaining = first_triangle < triangle_count ? triangle_count - first_triangle : 0u;
    const uint emit = min(remaining, mesh_triangles_per_workgroup);

    SetMeshOutputCounts(emit * mesh_indices_per_triangle, emit);
    // ... the same counter `mesh_main` adds to, so the two passes' workgroup counts are readable on one run.
    mesh_stat_add(push.frame_slot, mesh_stat_mesh_workgroups, 1u);

    // ---- THIS MATERIAL'S OUTLINE WIDTH, and the `> 0` gate (see the block above) ----
    const uint colour_base = push.material_index * pbr_toon_colour_lanes;
    const float outline_width = heap_at<StructuredBuffer<float4>>(heap_slots_toon_colours)[colour_base + pbr_outline_edge_lane].w;
    // `ScaleX`, hoisted out of the loop because it is a property of the frame's projection and not of a vertex.
    const float scale_x = abs(camera_at(heap_camera_slot).proj[1][1] / camera_at(heap_camera_slot).proj[0][0]);
    // The view matrix once, for the same reason: `mat3(view) * normal` is the world -> view rotation.
    const VR_MAT4 outline_view = camera_at(heap_camera_slot).view;

    for (uint t = 0u; t < emit; ++t)
    {
        for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner)
        {
            const uint raw_index = mesh_index_at(geometry, geometry.first_index + (first_triangle + t) * mesh_indices_per_triangle + corner);
            const uint vertex_index = uint(int(raw_index) + geometry.base_vertex);
            const MeshVertex v = mesh_vertex_at(geometry, vertex_index);
            // THE SURFACE'S OWN VERTEX FIRST, through the shared body: everything the hull inherits - morph, skin,
            // the world transform, the previous-frame position - is this call.
            pbr_vertex_output o = pbr_shade_vertex(v, vertex_index, group_id.y);
            if (outline_width > 0.0)
            {
                // ---- THE AUTHOR'S EXPANSION, in clip space (`:152-170`) ----
                const float3 n_view = mat3(outline_view) * o.normal;
                // `(float2x3)UNITY_MATRIX_P * n`: the projection's first two rows against the view normal. This
                // engine's projection has no shear - its upper-left 3x3 is diagonal - so this is the same pair of
                // products the author's cast produces.
                const float2 clip_normal_2d = float2(dot(camera_at(heap_camera_slot).proj[0].xyz, n_view), dot(camera_at(heap_camera_slot).proj[1].xyz, n_view));
                // THE DEGENERATE CASE IS A MEASURED CORRECTION AND NOT A STYLE CHOICE: at a vertex whose normal
                // points straight along the view axis the projected normal is (0,0), `normalize` returns NaN, and
                // ONE NaN vertex drops every triangle that touches it - the first attempt at this outline in this
                // repository moved zero pixels for exactly that reason. A degenerate normal expands by nothing,
                // which is the article's own zero-offset case.
                const float clip_normal_length = length(clip_normal_2d);
                const float2 normal_2d_raw = clip_normal_length > 1e-5 ? clip_normal_2d / clip_normal_length : float2(0.0, 0.0);
                // ... and `normal2D.x /= ScaleX` is the author's correction for the projection NOT being
                // aspect-aware in clip space.
                const float2 normal_2d = float2(normal_2d_raw.x / scale_x, normal_2d_raw.y);

                const float depth_scale = o.position.w;
                const float outline_depth_clamp = min(depth_scale, 20.0);
                const float depth_refine = lerp(1.0, 0.4, smoothstep(1.0, 12.0, depth_scale));
                o.position.xy += normal_2d * (outline_width * outline_depth_clamp * 0.01 * depth_refine);
            }
            verts[t * mesh_indices_per_triangle + corner] = o;
        }
        tris[t] = uint3(t * mesh_indices_per_triangle + 0u, t * mesh_indices_per_triangle + 1u, t * mesh_indices_per_triangle + 2u);
    }
}

// =============================================================================================================
// THE MESHLET ENTRY (entry `meshlet_main`): ONE workgroup per meshlet, its window read out of the table, and the
// same per-meshlet frustum culling the shadow pass's entry does - against the CAMERA's clip volume here, since
// this is the pass the camera sees (docs/mesh_shaders.md step 3).
//
// The projection body is `pbr_shade_vertex`, shared with the vertex and mesh entries, so a kept meshlet lands
// where the other two forms would have put it - which is what lets the capture gate accept this path.
// =============================================================================================================
[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void meshlet_main(out vertices pbr_vertex_output verts[mesh_vertices_per_workgroup], out indices uint3 tris[mesh_triangles_per_workgroup], uint3 group_id : SV_GroupID)
{
    const MeshGeometryLanes geometry = push.geometry;
    // THE LANE'S base_vertex IS THE "THIS RUN WAS HOST-CULLED" FLAG in a meshlet session (the field is otherwise
    // unused there, and the host zeroes it): when it is set, this workgroup's meshlet is the group-th SURVIVOR of the
    // primitive's run rather than the group-th record, and it comes out of this frame's culled table
    // (docs/mesh_shaders.md step 3 - the host culls what the dispatch would otherwise launch a workgroup for).
    const bool host_culled = (geometry.base_vertex & 1u) != 0u;
    // ... AND bit1 IS "A BACK-FACE TEST MAY CULL THIS DRAW": the pass culls back faces itself only for a
    // single-sided material and only when the session does not force two-sided rasterization (the shadow pass does).
    // A two-sided draw KEEPS both sides on purpose, so a meshlet rejected for facing away would remove geometry the
    // pass meant to draw - the mistake the first backface experiment made across six scenarios.
    const bool backface_legal = (geometry.base_vertex & 2u) != 0u;
    const MeshletRecord meshlet = host_culled ? culled_meshlet_at(push.frame_slot, geometry.first_index + group_id.x) : meshlet_at(geometry.first_index + group_id.x);
    // ONE ATOMIC PER WORKGROUP, counting what the culling below decided (docs/mesh_shaders.md step 3's measurement:
    // a rejected meshlet and a meshlet nobody dispatched are the same pixels, so the numbers are the only witness)
    mesh_stat_add(push.frame_slot, mesh_stat_workgroups, 1u);

    // PER-MESHLET CULLING against the camera's clip volume: object -> world (the push's model matrix, or the
    // instance table for an instanced draw) -> clip, with the radius scaled by an upper bound on both matrices'
    // axis scales. A rejected meshlet emits NOTHING, which is the whole effect a task stage would have had on the
    // frame (its advantage is skipping the launch, not the pixels).
    // ---- A DEFORMING DRAW IS NEVER CULLED PER MESHLET, and that is a correctness rule rather than caution ----
    // A meshlet's sphere and cone are built from the BIND-POSE vertices, while this draw's vertices are skinned
    // (joint matrices, `skin_base != 0`) or morphed (`morph_targets > 0`) - so the bounds describe geometry the
    // stage is not about to emit, and a cull against them can remove something visible. The gate's fixtures happen
    // to deform little enough that it did not show, which is exactly why the rule is stated here instead of being
    // left to a measurement: what a meshlet path wants for deformable geometry is bounds per FRAME, not per draw.
    const bool deforming = push.morph_targets > 0u || push.skin_base != 0u;
    const VR_MAT4 world = (push.flags & 1u) != 0u ? instance_transform_at(heap_instance_slot, push.instance_base + group_id.y) : push.model;
    const VR_MAT4 view_proj = camera_at(heap_camera_slot).proj * camera_at(heap_camera_slot).view;
    const float4 clip = view_proj * (world * float4(meshlet.center, 1.0));
    const float radius = meshlet.radius * matrix_max_axis_scale(world) * matrix_max_axis_scale(view_proj);
    if (!deforming && !clip_sphere_visible(clip, radius))
    {
        mesh_stat_add(push.frame_slot, mesh_stat_culled, 1u);
        SetMeshOutputCounts(0u, 0u);
        return;
    }

    // ---- THE BACK-FACE TEST IS NOT SHIPPED, AND THE MEASUREMENT SAYS WHY (docs/mesh_shaders.md step 3) ----
    // `meshlet_back_facing` is the conservative cone test, and the lane flag above is what says whether a draw may be
    // culled by facing at all - but applying it (in EITHER orientation, and restricted to the single-sided draws
    // where the pass really does drop back faces) removes geometry the rasterizer was drawing: `deferred`, `unlit`
    // and `sponza` all change, 3 of the core 5. So the record's cone does not predict the rasterizer's front/back
    // decision for real scene geometry here, and per-meshlet back-face culling is a hole rather than a saving until
    // that is settled. The flag stays because it is what any future attempt has to respect.
    [[maybe_unused]] const bool backface_legal_measured_not_shipped = backface_legal;

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
            verts[t * mesh_indices_per_triangle + corner] = pbr_shade_vertex(v, vertex_index, group_id.y);
        }
        tris[t] = uint3(t * mesh_indices_per_triangle + 0u, t * mesh_indices_per_triangle + 1u, t * mesh_indices_per_triangle + 2u);
    }
}