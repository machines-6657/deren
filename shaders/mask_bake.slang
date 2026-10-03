// shaders/mask_bake.slang - the SLANG source of the alphaMode MASK bake.
//
// PRODUCES `mask_bake.comp.spv`, the same name the GLSL version produced, so the host side is untouched.
// Ported from shaders/mask_bake.comp.
//
// WHY IT EXISTS, unchanged by the port: an inline ray query has no any-hit stage, so a MASK material's
// `discard` cannot run during traversal and the surface is SOLID to every ray. This pass was the answer
// available while the shadow ran on a ray QUERY: resolve the mask BEFORE the build - evaluate the material's
// alpha over a triangle, write the result into an expanded vertex buffer, and build the structure from that.
// The shadow now runs on a ray-tracing PIPELINE whose any-hit stage decides the same question per hit
// (`shaders/rt_shadow.slang`), which is strictly finer; this pass stays because a switch is what makes the
// two comparable, and it is off by default (`[render] rt_mask_bake`).
//
// Hiding a triangle means writing all three of its vertices at the SAME position - a degenerate triangle,
// which traversal can never report a hit against. That is only safe when the triangle OWNS its vertices, so
// the geometry is EXPANDED to three unique vertices per triangle and the instance table points at that
// buffer; a zero index address marks the geometry as not indexed.
//
// THE RULE IS "THE WHOLE TRIANGLE IS CUT", sampled over the triangle's UV AREA on a grid at TEXTURE
// RESOLUTION - not at its vertices, and not on a fixed grid. Both alternatives were measured as wrong:
// vertex sampling cuts a quad whose opaque shape sits in its middle (the sample assets' two-quad MASK planes
// measure 100% cut by their vertices and 0% over their area), and a fixed 10-sample grid misses the small
// opaque islands a MASK texture is made of (it cut 1.0% of a vase's triangles where the texel walk cut 0.1%,
// and the vase's frame came out 1.42 of mean brightness brighter than the raster shadow it should match).
// A footprint too large to walk is KEPT, so the bake never removes geometry it has not looked at.
//
// WHAT THE SLANG SIDE SPELLS DIFFERENTLY: the three buffers are read through GL_EXT_buffer_reference in GLSL
// and through C-style pointer casts off the same 64-bit device addresses here, and `textureSize` is the
// shim's `heap_size`. The fetch uses the implicit-LOD form, which a 64-wide workgroup may use (TRAP 4: the
// implicit form needs a derivative quad, so a group that is not a multiple of four in x could not).

#include "heap_access.slang"

// THE MATERIAL LAYOUT IS NOT DECLARED HERE ANY MORE: it is `material_record.glsl`, the one place the
// record's std430 layout lives, and this stage includes it. A second copy is a second thing to keep in
// step with the host's `static_assert(sizeof(material_record) == 80)`, and a copy that drifts indexes
// every material after the first by the wrong STRIDE - which shows up as a wrong texture on every
// material but one, not as a build error.
#include "material_record.glsl"

struct MaskBakePush
{
    // Device addresses as two 32-bit halves, the shape every pass in this project pushes them in.
    uint2 source_vertices;
    uint2 source_indices; // zero when the source geometry is not indexed
    uint2 destination;
    uint source_stride;
    uint destination_stride;
    uint index_type; // VkIndexType: 0 = uint16, 1 = uint32 (ignored when source_indices is zero)
    uint triangle_count;
    uint material_index;
};
[[vk::push_constant]] ConstantBuffer<MaskBakePush> pc;

/// @brief the most samples per UV axis one triangle's footprint is walked at (a larger footprint is KEPT)
#define MASK_BAKE_MAX_STEPS 64

/// @brief the 64-bit device address held in two push lanes (little-endian, low half first)
uint64_t mask_bake_address(uint2 halves)
{
    return (uint64_t(halves.y) << 32u) | uint64_t(halves.x);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void comp_main(uint3 invocation : SV_DispatchThreadID)
{
    const uint triangle = invocation.x;
    if (triangle >= pc.triangle_count)
    {
        return; // the dispatch is rounded up to the workgroup size
    }

    // The triangle's three source vertices, by the same arithmetic the hit shader uses: an index (or, for a
    // non-indexed source, the vertex's own position in the list), times the stride, off the base.
    uint* indices = (uint*)mask_bake_address(pc.source_indices);
    float* vertices = (float*)mask_bake_address(pc.source_vertices);
    const uint stride = pc.source_stride / 4u;
    const bool indexed = (pc.source_indices.x != 0u) || (pc.source_indices.y != 0u);
    float3 positions[3];
    float3 normals[3];
    float2 uvs[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        const uint i = triangle * 3u + k;
        uint vertex = i;
        if (indexed)
        {
            // VkIndexType: UINT16 is 0 and UINT32 is 1.
            if (pc.index_type == 0u)
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
        normals[k] = float3(vertices[at + 3u], vertices[at + 4u], vertices[at + 5u]);
        uvs[k] = float2(vertices[at + 6u], vertices[at + 7u]);
    }

    // The area test: the triangle is collapsed only when EVERY texel of its UV footprint is cut - so it
    // survives as soon as ONE texel inside its UV triangle is opaque. The samples walk the footprint AT
    // TEXTURE RESOLUTION (see the file header for what a fixed grid measured), and the cap is spent on the
    // SAFE side: a footprint too large to walk is KEPT.
    const Material material = material_at(heap_slots_materials, pc.material_index);
    const float cutoff = material.alpha_cutoff;
    const float factor_alpha = material.base_color_factor.a;
    const DescriptorHandle<Texture2D<float4>> albedo = heap_at<Texture2D<float4>>(heap_slots_textures + material.tex_indices.x);
    const float2 uv_min = min(min(uvs[0], uvs[1]), uvs[2]);
    const float2 uv_max = max(max(uvs[0], uvs[1]), uvs[2]);
    const float2 uv_span = uv_max - uv_min;
    const int2 footprint = int2(abs(uv_span) * heap_size(albedo, 0u));
    const int steps_u = max(1, min(footprint.x, MASK_BAKE_MAX_STEPS));
    const int steps_v = max(1, min(footprint.y, MASK_BAKE_MAX_STEPS));
    bool any_opaque = footprint.x > MASK_BAKE_MAX_STEPS || footprint.y > MASK_BAKE_MAX_STEPS;
    for (int iu = 0; iu < steps_u && !any_opaque; ++iu)
    {
        for (int iv = 0; iv < steps_v && !any_opaque; ++iv)
        {
            // The sample grid covers the UV BOUNDING BOX, so a sample outside the triangle has to be skipped:
            // the box is not the triangle, and its corners belong to whatever else shares the texture atlas.
            const float2 uv = uv_min + (float2(float(iu), float(iv)) + float2(0.5, 0.5)) / float2(float(steps_u), float(steps_v)) * uv_span;
            const float2 e0 = uvs[1] - uvs[0];
            const float2 e1 = uvs[2] - uvs[0];
            const float determinant = e0.x * e1.y - e1.x * e0.y;
            if (abs(determinant) < 1e-12)
            {
                any_opaque = true; // a degenerate UV triangle: keep it, nothing can be decided about it
                break;
            }
            const float2 rel = uv - uvs[0];
            const float w1 = (rel.x * e1.y - e1.x * rel.y) / determinant;
            const float w2 = (e0.x * rel.y - rel.x * e0.y) / determinant;
            const float w0 = 1.0 - w1 - w2;
            if (min(min(w0, w1), w2) < -0.01)
            {
                continue; // outside the triangle (the tolerance absorbs the grid's own rounding)
            }
            // The same coverage the raster path tests: base colour factor alpha times the texture's alpha,
            // kept when it reaches the cutoff (shadow.frag's and surface.glsl's `alpha < cutoff -> discard`).
            // THE EXPLICIT-LOD FETCH, and here it is not merely a spelling choice: GLSL's implicit fetch is
            // legal here (the group is 64 wide, a multiple of four) but Slang refuses it - `E31210: derivative
            // group quad thread count error` - because the fetch sits inside non-uniform control flow (the
            // nested loops with their break/continue guards), and a derivative has no meaning across a quad
            // whose four invocations are four DIFFERENT triangles. Which is also why the GLSL side's implicit
            // LOD is a latent hazard rather than a resolution-aware fetch: the "texture resolution" walk the
            // header describes depends on the mip the derivatives happen to select. LOD 0 is what the walk
            // wants, and the A/B against the GLSL build is what says whether the two agree.
            const float coverage = factor_alpha * heap_texel_lod0(albedo, heap_sampler_texture, uv).a;
            any_opaque = coverage >= cutoff;
        }
    }

    // Write the three destination vertices: the source's own when the triangle survives, and three copies of
    // its FIRST vertex when it does not. Position, normal AND UV are copied in both cases - the UVs are what
    // the hit shading interpolates, and the normal is the third axis of its tangent frame when the material
    // has a normal map, so a zeroed one would be a degenerate frame rather than an unused field. For a hidden
    // triangle none of the three is ever read: no ray can hit it.
    float* out_values = (float*)mask_bake_address(pc.destination);
    const uint destination_stride = pc.destination_stride / 4u;
    for (uint k = 0u; k < 3u; ++k)
    {
        const uint source = any_opaque ? k : 0u;
        const uint at = (triangle * 3u + k) * destination_stride;
        out_values[at + 0u] = positions[source].x;
        out_values[at + 1u] = positions[source].y;
        out_values[at + 2u] = positions[source].z;
        out_values[at + 3u] = normals[source].x;
        out_values[at + 4u] = normals[source].y;
        out_values[at + 5u] = normals[source].z;
        out_values[at + 6u] = uvs[source].x;
        out_values[at + 7u] = uvs[source].y;
    }
}
