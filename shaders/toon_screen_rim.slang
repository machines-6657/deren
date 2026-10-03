// shaders/toon_screen_rim.slang - THE SCREEN-SPACE DEPTH RIM, added over the toon-shaded frame.
//
// WHAT IT IS, and it is the SECOND of the two rim mechanisms both references carry. The first - the
// view-angle rim - is inside the character-forward stage and needs only a normal and a view vector. This one
// catches what that cannot: a contour where the surface does NOT turn away from the camera but is simply
// occluded by nearer geometry (an arm across a torso, a chin over a collar). Its mechanism is a DEPTH
// COMPARISON at a screen offset, and the references do it in their own geometry passes:
//   * the MME pack's `Draw*ScreenRim` passes sample the viewport map's G channel (linear camera depth)
//   * the reference's `GetCharacterDirectRimLightArea` samples `_CameraDepthTexture`
//
// WHY IT IS A PASS OF ITS OWN RATHER THAN A STEP IN THE CHARACTER-FORWARD STAGE, and the reason is a Vulkan
// rule rather than a layout preference: that stage holds the depth image as its DEPTH ATTACHMENT - which is
// what makes its `ZTest Equal` possible - and an image cannot be a depth attachment and a sampled texture in
// the same instance. Here the depth is only ever SAMPLED (this instance has no depth attachment at all), so
// the comparison is legal. There is no depth COPY: the reference needs one because its depth attachment is
// the only copy it has, and this renderer publishes `gbuffer_depth` as a heap slot a fullscreen stage can
// read (see the stage preamble that publishes it).
//
// WHERE IT SITS: after the character-forward stage and before the resolve, so the anti-aliasing sees the
// contour in the frame it is part of. It ADDS to the scene colour (the pipeline's blend is ONE/ONE) rather
// than reading and rewriting it, so it never needs to sample the image it writes.
//
// WHY THE OFFSET IS SIGN-FREE, and this is a deliberate departure worth stating. The reference picks the
// offset's direction from the sign of the view-space normal's X (`normalExtendLeftOffset = normalVS.x > 0 ?
// 1 : -1`) and always offsets in SCREEN X. Reproducing that needs the view matrix, and the view matrix lives
// in `CameraUBO`, which `shading.glsl` declares - and `shading.glsl` expands the slot macros in its function
// bodies, so a stage that includes it must already have defined `heap_frame_slot`. That is satisfiable, but
// the sign rule buys nothing that this does not: sampling BOTH sides and keeping the larger depth increase
// finds the silhouette on whichever side it lies, and it is strictly more robust where `normalVS.x` is near
// zero - a surface seen edge-on, which is exactly where a silhouette is. What is kept is the reference's
// essential step: `rimArea = saturate((depth_at_offset - depth_here) * gain)`, with the offset scaled by
// `1 / (eye_depth + 3)` so it shrinks with distance the way the reference's does.
//
// WHAT IT CANNOT DO YET, and this is the constraint recorded when the family table was made shared: it
// cannot read the MATERIAL TABLE, because the `Material` struct is declared by `surface.glsl` - along with
// the shared scene push block, which is 144 bytes and over this path's 128-byte limit, so a fullscreen stage
// cannot include it. The rim therefore takes its colour and strength from its own push block: ONE set for
// every character rather than one per family. The per-family refinement needs either a family id in the
// G-buffer or a material struct declared where a fullscreen stage can reach it.

#include "heap_access.slang"
#include "material_record.glsl"
#include "toon_params.slang"

struct ToonRimPush
{
    float proj_22;      // projection[2][2]: the depth-linearization terms (see view_depth)
    float proj_32;      // projection[3][2]
    float rim_width;    // the reference's `rimWidth` scale: how far along the screen the comparison reaches
    float rim_scale;    // the gain on the depth difference (the reference's `* 4`)
    float rim_strength; // 0 disables the whole pass, which is what keeps a frame without a character free
    float pad0;
    float pad1;
    float pad2;
    float4 rim_colour;
    uint frame_slot;   // THE HEAP INDICES, at the END so every field above keeps its offset
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<ToonRimPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

/// @brief view-space distance of a depth-buffer sample
/// @note the same inversion `gbuffer_debug.slang` documents: for the perspective projection the CPU builds,
///       the stored depth is z_ndc = (proj_22 * z_view + proj_32) / -z_view, so z_view = proj_32 / (z_ndc +
///       proj_22). Written once here and once there rather than shared, because the shared body would have to
///       carry the push block that holds the two terms - which is the thing this stage cannot include.
float rim_view_depth(float depth)
{
    return pc.proj_32 / (depth + pc.proj_22);
}

float rim_depth_at(float2 uv)
{
    return heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), heap_sampler_gbuffer, uv).r;
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_uv : TEXCOORD0) : SV_Target
{
    const float stored_depth = rim_depth_at(v_uv);
    // THE CLEARED DEPTH IS THE FAR PLANE: those pixels hold no surface, so there is no contour to draw and
    // returning zero leaves the frame exactly as the character-forward stage left it.
    if (stored_depth >= 1.0 || pc.rim_strength <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // ---- WHICH MATERIAL IS THIS PIXEL, and it has to be asked ----
    // WITHOUT THIS THE PASS OUTLINES EVERY SILHOUETTE IN THE SCENE, which is what the first version of it did:
    // the depth comparison finds an edge wherever geometry meets something nearer, and geometry is not only the
    // character. The material id the G-buffer pass stored (two UNORM bytes, exactly as `gbuffer.slang` wrote
    // them) is what turns "an edge" into "a CHARACTER's edge" - the family is the test, and `none` means the
    // name claimed no family, which is every non-character surface.
    const float4 material_target = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_material + heap_image_index), heap_sampler_gbuffer, v_uv);
    const uint material_id = uint(round(material_target.r * 255.0)) | (uint(round(material_target.g * 255.0)) << 8u);
    const Material mat = material_at(heap_slots_materials, material_id);
    if (mat.toon_family == 0u)
    {
        return float4(0.0, 0.0, 0.0, 0.0); // no family claimed it: not a character, no contour
    }
    const toon_params params = toon_params_for(mat.toon_family);
    if (params.rim_strength <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0); // the family opted out of a rim (the reference's eye does)
    }

    const float depth_here = rim_view_depth(stored_depth);
    // The offset SHRINKS WITH DISTANCE (`/ (eye_depth + 3)`) so a constant rim width in screen terms is a
    // constant width on the SURFACE rather than on the image - the reference's own expression, including its
    // constant, and the 3.0 is what keeps the divisor away from zero for a surface at the near plane.
    const float step = pc.rim_width * 0.0044 / (depth_here + 3.0);
    const float depth_left = rim_view_depth(rim_depth_at(saturate(v_uv + float2(-step, 0.0))));
    const float depth_right = rim_view_depth(rim_depth_at(saturate(v_uv + float2(step, 0.0))));
    // The larger depth ANY side reaches is the silhouette: this fragment is nearer than what is beside it.
    const float rise = max(depth_left, depth_right) - depth_here;
    const float rim_area = saturate(rise * pc.rim_scale);

    // The MME pack lightens the albedo for the rim (`albedoRimColor = saturate(albedo + 0.3)`) rather than
    // adding a flat colour, so a dark material's contour is still recognisably ITS colour. The albedo is the
    // G-buffer's first target, which this stage reaches the same way it reaches the depth.
    //
    // THE COLOUR AND STRENGTH ARE THE FAMILY'S, not this pass's: the references carry `EF_*_RIM_COLOR` /
    // `EF_*_RIM_STRENGTH` once per family, and a contour whose colour came from one place while the shading
    // under it came from another is exactly the mismatch the shared family table exists to prevent. What stays
    // the PASS's is the rim's SHAPE - the offset width and the depth gain, which are properties of the screen
    // and the projection rather than of the material - and `pc.rim_strength` as one global scale over
    // everything, so the stage can be dimmed without touching the table.
    const float3 albedo = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_albedo + heap_image_index), heap_sampler_gbuffer, v_uv).rgb;
    const float3 rim = params.rim_colour * rim_area * params.rim_strength * pc.rim_strength * saturate(albedo + 0.3);

    // The pipeline blends ONE/ONE, so this is an ADDITION to the scene colour and the alpha lane is unused.
    return float4(rim, 0.0);
}
