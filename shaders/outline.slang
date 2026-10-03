// shaders/outline.slang - the SLANG source of the TOON OUTLINE fragment stage (`outline.frag.spv`).
//
// WHAT IT IS: the fragment half of the article's `MyZmdOutlineShader` (the verbatim copy this port works from is
// `build-release-clang64/deren-ab/author_src/MyZmdOutlineShader.shader`; the numbers below are ITS line numbers).
// Its whole mechanism is a BACK-FACE HULL drawn a second time: the geometry stage pushes every vertex out along
// its normal (the author's `:134-170`, ported into `pbr.slang`'s `outline_mesh_main`), the pass rasterizes that
// hull with its FRONT faces culled (the author's `:47` `Cull Front`), and what survives the depth test is a ring
// around the silhouette. THIS file shades the ring.
//
// THE NORMAL IS THE WHOLE IDEA, and it comes from BEHIND the fragment rather than from the hull. The ring's own
// normal points AWAY from the camera - it IS the far side of the hull - so shading with it would give an outline
// that is dark on the lit side of the character, which is the opposite of the reference. The article reads the
// normal of the surface BEHIND the ring out of the G-buffer (`:196-198`: `LOAD_TEXTURE2D_X(_GBuffer2, ...)` /
// `UnpackNormal` / `normalWS = lerp(i.normalWS, normalWS, _IsHair)`) and shades with THAT, so the outline takes
// the colour the surface under it would have had.
//   * the port reads the same thing, from the slot the deferred path also lights by:
//     `heap_slots_gbuffer_normal + heap_image_index`, addressed by `sv_position.xy / heap_size(image, 0u)` -
//     the extent-based pixel coordinate `deferred.slang` uses for its own G-buffer reads. Its `.xyz` is WORLD
//     SPACE (`deferred.slang`'s own note on `si.normal`), which is why nothing here transforms it.
//   * HAIR IS THE ARTICLE'S OWN EXCEPTION (`:198`'s `_IsHair = 1`) and the port keeps it: a strand's silhouette
//     belongs to the strand rather than to the surface behind it, so the hair family is shaded with the MESH's
//     own normal - the same branch the clearing case below takes.
//
// ---- THE BACKGROUND CASE IS THE PORT'S DECISION, NOT THE ARTICLE'S, AND IT IS THE DECIDING CASE ----
//
// The article calls `UnpackNormal` on Unity's G-buffer, whose cleared content is UNITY'S business - so what it
// computes over the sky is NOT something this port can faithfully reproduce, and it is not presented here as if
// it were. What the port CAN see is that the visible ring lies ON BACKGROUND PIXELS: the hull only survives where
// it sticks out past the real silhouette, so most of the outline's own pixels are pixels no character wrote. This
// renderer clears the normal target to (0,0,0,0) (`scene.cpp`), where `normalize` is a NaN - and a NaN normal
// would poison the ramp lookup and take the whole ring with it. So a fragment whose stored normal is degenerate
// falls back to the MESH'S OWN normal, which is exactly the branch the article already keeps for hair: the
// outline over a background pixel is shaded like the strand case rather than like a surface there is none of.
// THIS IS MEASURABLE AND MUST BE MEASURED (the acceptance in `outline_plan.md` asks for the background pixels'
// count and max |d|), because it decides what the outline looks like where it is most visible.
//
// WHY THE DEPTH IMAGE IS NOT CONSULTED FOR THAT TEST, and it is a Vulkan rule rather than a preference: this
// pass's group holds `gbuffer_depth` as its DEPTH ATTACHMENT (`render_resource.cppm`'s
// `character_forward_targets`), and an image cannot be a depth attachment and a sampled texture in the same
// instance (the rule `toon_screen_rim.slang`'s header states for the same reason). The normal target is NOT an
// attachment of this group, so IT can be sampled - which is what lets this stage exist at all - and the cleared
// normal says "no surface was written here" as plainly as a cleared depth would.
//
// WHAT IS NOT REPRODUCED, stated here because each one is the kind of difference that is invisible in a frame:
//  - `_IsNeedSelfLight` / `_SelfLightIntensity` (`:21-22`, `:206-207`). A DEBUG switch whose declared default is
//    0, at which the article's own `lerp`s are the identity - so not carrying it is behaviour-equivalent rather
//    than a missing feature.
//  - `_ZBias` (`:8`, `:172-178`). Its default is 0.0 and the game's `_OutlineOffsetZ` has NO confirmed mapping
//    in this port, so the term is exactly zero and is not implemented (see the note in `pbr.slang`'s mesh entry,
//    where the author's z offset would have lived).
//  - THE ARTICLE'S OWN FRAGMENT HAS NO SPECULAR, MATCAP, RIM, AMBIENT OR EMISSIVE TERM - read `:186-313`: it is
//    the toon DIFFUSE chain and nothing else. This stage therefore calls the port's own implementation of that
//    chain (`toon_diffuse`, `character_forward.slang`) and adds nothing to it, which is the same shape the
//    article has: an outline is a flat contour, not a second shading model.
//
// ---- WHERE THE COLOUR AND THE WIDTH COME FROM, AND WHICH NUMBERS ARE THE ARTICLE'S ----
//
// BOTH ARE PER MATERIAL, and they ride the ONE per-material carrier this renderer has left: a lane of the
// `heap_slots::toon_colours` table (`deren::vulkan::toon_colour_lane::outline_edge`), whose `.rgb` is the game's
// `_OutlineTintColor` row and whose `.w` is the game's `_OutlineWidth` row. The mesh entry reads the same lane
// for the width (its `> 0` gate is there), so the hull and the colour cannot disagree about whether a material
// is outlined at all.
//  - `_OutLineStrength` IS THE ARTICLE'S DEFAULT (1.0, `:16`): the game's parameter table carries no such row
//    (measured with `deren-ab/outline_rows.py`), so there is nothing to take from it.
//  - AND THE ARTICLE'S `_OutlineColor` DEFAULT (white, `:15`) IS ONLY THE FALLBACK the lane answers with when a
//    material states no tint. The author is explicit that the colour is a taste rather than a reconstruction -
//    his own words in the article (PDF p-53) are that the outline's配色 is "很暴力、你可以自己设计" - which is
//    why the port carries the game's per-material row where there is one and does not treat white as a finding.
//
// ---- THE PASS STATE THIS STAGE EXPECTS (the host side that wires it owns all three) ----
//
//  * `Cull Front` IS PART OF THE MECHANISM, not a preference: with the front faces kept, the hull would simply
//    cover the character. THE AUTHOR'S `:47` SAYS `Cull Front` AND THIS RENDERER CANNOT EXPRESS IT YET -
//    `render_environment.cppm`'s cull-mode selection carries NONE and BACK only - so that is HOST-SIDE WORK AND
//    IS NOT DONE. Recorded here so the pass is not wired without it: without it this stage draws a
//    character-shaped blob rather than an outline.
//  * `ZWrite`: the author says On (`:48`) and this port's character-forward group LOCKS depth writes OFF
//    (`vulkan/pass/character_forward.cpp` sets `set_depth_write(false)` and the environment overrides it). That
//    is a RECORDED DEVIATION. With back faces only and `LESS_OR_EQUAL`, the result FOR ONE HULL is the same -
//    the hull's fragments compare against the same depth they would have written - and the cost is that the hull
//    does not occlude what is drawn after it (the overlay group and the post chain).
//  * `ZTest LESS_OR_EQUAL` (the overlay group's compare, `vulkan/core/core.cpp`), i.e. the author's default
//    `LEqual` and NOT the character-forward pass's `Equal`.
//  * THE G-BUFFER NORMAL IS SAMPLED, NEVER AN ATTACHMENT, for the Vulkan rule quoted above.
//
// THE INCLUDE IS A STAGE FILE, deliberately: `toon_diffuse` - the chain this stage's whole body is - lives in
// `character_forward.slang`, and a second copy of it here is the duplicate this repository refuses everywhere (a
// per-family ramp read in two places drifts silently, and the drift shows only as an outline whose colour does
// not match the surface under it). `character_forward.slang` pulls in the shim, `surface.glsl` and
// `shading.glsl` in the order they must be included in, so including it alone is also the correct include order.
//
// ITS `main` COMES WITH IT, and that is why this file's entry is named `frag_main` rather than `main`: two
// `[shader("fragment")]` functions cannot share a name in one translation unit, and the same shape already
// exists twice in this repository (`post.slang`, `heap_probe.slang` - one file, several entries, distinct
// names). The .spv name is what the host loads and it is unchanged.

#include "character_forward.slang"

/// `deren::vulkan::toon_colour_lane::outline_edge` - the material's outline lane in `heap_slots::toon_colours`, whose
/// `.rgb` is `_OutlineTintColor` and whose `.w` is `_OutlineWidth`. Spelled out here (and again in `pbr.slang`)
/// because a shader cannot include a C++ enum; the STRIDE of the table is the stage's own existing
/// `character_toon_colour_lanes`, which is the constant the host's `toon_colour_lane::count` is checked against.
static const uint outline_edge_lane = 3u;

/// `_OutLineStrength` (`:16`): the article's default, and there is nothing to take instead - the game's
/// parameter table carries no strength row (see the header).
static const float outline_strength = 1.0;

#ifndef DOXYGEN
/// @brief shade one outline fragment: the toon diffuse of the surface BEHIND it, tinted by the material's own
///        outline colour
/// @param v_world_pos/v_normal/v_uv the varyings `pbr.slang`'s `outline_mesh_main` emits - the SAME interface
///        `character_forward.slang`'s entry declares, because the hull is the same mesh through the same body
/// @param v_is_front the rasterizer's facing flag, declared for interface parity and read by nothing: the
///        article's fragment does not branch on it either, and the double-sided flip a normal needs is
///        `gather_surface`'s (see surface.glsl) rather than a stage's
/// @param sv_position the pixel coordinate, used to address the G-buffer at the fragment's own pixel
/// @return the outline's LINEAR colour with alpha 1.0 - the article's own return (`:312`) is opaque, because an
///         outline is geometry and not a decal over the surface
[shader("fragment")]
float4 frag_main(
    [[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
    [[vk::location(1)]] float3 v_normal : TEXCOORD1,
    [[vk::location(2)]] float2 v_uv : TEXCOORD2,
    bool v_is_front : SV_IsFrontFace,
    float4 sv_position : SV_Position) : SV_Target
{
    // The same surface the G-buffer pass wrote and the character stage shades, gathered by the same function:
    // the albedo, its alpha (which the article's chain reads as a MATERIAL weight and not as coverage), the
    // engine's occlusion, the material's family and its lane words.
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv);
    const toon_params params = toon_params_for(s.toon_family);
    const uint4 toon_lanes = toon_lanes_at(heap_slots_toon_lanes, push.material_index);

    // ---- THE NORMAL OF THE SURFACE BEHIND THIS FRAGMENT (the article's `:196-198`) ----
    //
    // A `gbuffer_texel` equivalent, written inline rather than shared: `deferred.slang`'s helper is a private
    // function OF THAT STAGE'S FILE (its push block is declared there), and a shared body would have to carry
    // that block. The two reads are the same instruction - `heap_texel` through the G-buffer's NEAREST sampler.
    const DescriptorHandle<Texture2D<float4>> gbuffer_normal = heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index);
    const float2 gbuffer_uv = sv_position.xy / heap_size(gbuffer_normal, 0u);
    const float3 behind_normal = heap_texel(gbuffer_normal, heap_sampler_gbuffer, gbuffer_uv).xyz;
    // DEGENERATE = THE BACKGROUND (see the header): the target is cleared to (0,0,0,0), so a zero-length stored
    // normal is "no surface was written here" and the mesh's own normal is the fallback. `1e-8` rather than an
    // exact zero because the target holds whatever the clear and the G-buffer pass left, not a literal.
    const bool behind_usable = dot(behind_normal, behind_normal) > 1e-8;
    // ... and the article's hair exception, in the same branch.
    const float3 n = (behind_usable && s.toon_family != VR_FAMILY_HAIR) ? normalize(behind_normal) : normalize(s.normal);

    // The article's `:310-312`: `mainDiffuseResult * _OutlineColor * _OutLineStrength`. `toon_diffuse` IS
    // `mainDiffuseResult` here - the same call `character_forward.slang` makes, with the normal substituted -
    // and the tint and the strength are the material's lane and the article's default above.
    //
    // STEP 6'S DESATURATION IS DELIBERATELY NOT APPLIED TO THIS CALL, and the extra argument below is discarded
    // for that reason: the reference's `色相/饱和度/明度` is the LAST node of `PBRToonBase`'s own output chain, it
    // is not part of the group at all, and this stage multiplies the group's result by the outline's own tint. A
    // desaturated `mainDiffuseResult` would darken and grey the OUTLINE, which is the colour of a line rather
    // than a shading term. The out parameter exists because the group's caller needs it, and this is the one
    // caller that does not.
    float desaturation_unused = 0.0;
    const float3 diffuse = toon_diffuse(s.albedo, s.alpha, s.toon_family, n, v_world_pos, s.ao, params, s.toon_indices, v_uv, toon_lanes, push.material_index, desaturation_unused);
    const float3 tint = toon_colour_at(heap_slots_toon_colours, push.material_index * character_toon_colour_lanes + outline_edge_lane).rgb;

    return float4(diffuse * tint * outline_strength, 1.0);
}
#endif // DOXYGEN
