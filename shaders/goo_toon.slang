// shaders/goo_toon.slang - THE REWRITTEN TOON CHAIN's fragment stage (`goo_toon.frag.spv`).
//
// STEP 1 OF THE REWRITE: THE IRIS' TWO-LAYER MATCAP, AND NOTHING ELSE.
//
// ================================ WHAT THE AUTHORITY IS, AND WHY IT CHANGED ================================
//
// The toon chain in this port was written against the author's article (`终末地渲染.pdf`) and the MME pack, and it
// lives in `character_forward.slang`. The rewrite takes a DIFFERENT authority, decided by the person this port is
// for: XIYAG's Goo node presets - the `Arknights: Endfield_PBRToon*` shader groups inside
// `陈_佩丽卡_莱万汀_伊冯_1.2.1_GooBlender4.2+(请阅读RM)_ By 新杨XIYAG.blend`, dumped node-for-node to
// `build-release-clang64/deren-ab/gooblender/nodes.json`. Every constant and every connection below is read out of
// that file; the specification is `deren-ab/goo_toon_plan.md` and the per-material values are
// `deren-ab/goo_params_laevatain.tsv`.
//
// WHERE THE TWO SOURCES DISAGREE, THE GOO PROJECT WINS. The port also holds a second dump of the same six groups
// (`deren-ab/endfield_addon/chen_dump/nodes.json`, the kkskaguya addon's `presets/Chen.blend`), and the two differ
// in this very group - `Chen.blend` carries three extra nodes that modulate both emission layers by the Goo
// engine's `Shader Info.Cast Shadows`, and states the albedo window's centre as `1.1` rather than `1.0`. The Goo
// project is the authority because it is the file that HAS this character's materials in it (Laevatain's eleven
// materials exist only there), and because it is the file named. Measured, not assumed: the four materials that
// the Goo project attaches `PBRToon_irisBase` to are exactly the four `*_iris_01` materials of its four
// characters, enumerated in `deren-ab/goo_step1_result.md`.
//
// ================================ WHY A SECOND STAGE RATHER THAN AN EDIT ================================
//
// This file is compiled into its OWN module and registered under its own pipeline name
// (`runtime::goo_toon_pipeline_name`), so one run can draw the same frame, through the same pass, with the same
// descriptors and the same geometry, using either the old fragment or this one - selected by `[render] goo_toon`.
// An edit in place could not be switched back off inside a run, and a two-run comparison of two builds cannot
// separate "the new chain" from everything else that changed between them.
//
// ================================ WHAT THIS STEP DOES NOT PORT, AND WHY THAT IS THE CRITERION ================
//
// EVERY family except the iris is shaded by the OLD body below (`toon_diffuse`, `character_forward.slang`), called
// exactly as the old entry point calls it - verbatim, same arguments, same order. That is not a shortcut: this
// step's acceptance criterion is "with `goo_toon` on, the pixels that change are the iris' and no others", and a
// second implementation of the base toon in this file would make that criterion unmeasurable rather than met.
//
// WHY THE IRIS IS THE FIRST GROUP, three properties and all of them checkable:
//   * it is the SMALLEST reference group (25 nodes, 4 interface sockets in the Goo project) - the smallest state
//     surface a chain can be asked to reproduce exactly;
//   * it is exactly ONE material per character (`M_actor_*_iris_01`), i.e. one primitive, so the screen region a
//     change may land in can be pointed at by the frame itself rather than argued about;
//   * it exercises EVERY layer of the new plumbing at once - a new `.slang` module, a new pipeline name, a new
//     `[render]` switch, a new texture lane, a new colour lane and a new per-material row. Making it draw proves
//     the pipe, not one term in it.
//
// ================================ THE MECHANISM, NODE BY NODE, WITH NOTHING ADDED ==========================
//
// The reference group's whole output is `相加着色器(Emission1, Emission2)`; both halves are emission, and the
// group has no BSDF at all. Emission 1:
//
//   matcap uv   `纹理坐标.Normal -> 矢量变换(WORLD -> CAMERA) -> 矢量运算(MULTIPLY, Vector_001 = (0.5,0.5,0.0))
//               -> 矢量运算.001(ADD, Vector_001 = (0.5,0.5,0.0)) -> 图像纹理.001.Vector`
//               i.e. the SPHERE MAP of the shading normal in camera space: `n_view.xy * 0.5 + 0.5`. The
//               multiply and the add are the reference's own two nodes rather than one `*0.5+0.5`, and the
//               `.z = 0.0` of the multiply's constant is what makes the third component drop out.
//   ball        `图像纹理.001.image = T_actor_common_matcap_05_D.png` (256x256 RGBA, the group's OWN image - it is
//               not a material input, see the "per-family" note below)
//   weight      `群组.AngleThreshold -> 运算(SUBTRACT, Value = 1.0) -> 钳制(Min = 0.5, Max = 1.0)`, and
//               `组输入.D_RGB -> 混合.A_Color`, `钳制.Result -> 混合.B_Color`, `混合` = MULTIPLY at factor 1.0
//               i.e. `albedo * clamp(1.0 - AngleThreshold, 0.5, 1.0)`.
//   layer 2     `图像纹理.001.Color -> 混合.003.A_Color`, `组输入.D_RGB -> 混合.003.B_Color` (MULTIPLY, 1.0)
//               then `混合.Result -> 混合.002.A_Color`, `混合.003.Result -> 混合.002.B_Color`, and `混合.002` is an
//               ADD at factor 0.5666666626930237 - which for an ADD is `A + 0.5666666626930237 * B`.
//               So `layer2 = ball * albedo`, and emission 1's colour is `albedo * w + 0.5666667 * ball * albedo`.
//   strength    `组输入.D_Alpha -> 混合.001.Factor_Float`, `组输入.Eyes brightness -> 混合.001.A_Float`,
//               `运算.001(MULTIPLY) -> 混合.001.B_Float`, and `运算.001` takes `运算.Value` (the SAME `1.0 - angle`
//               the clamp above reads, from BEFORE the clamp - `运算`'s own `use_clamp` is FALSE) times
//               `组输入.Eyes HightLight brightness`. So
//               `strength = lerp(Eyes brightness, (1.0 - angle) * Eyes HightLight brightness, D_Alpha)`,
//               and emission 1 is `layer2_colour * strength`.
//
// Emission 2 is a DEAD BRANCH of the reference, and it is not implemented here:
//   `图像纹理.002.image = T_actor_common_matcap_07_D.png`, its `Vector` input is UNCONNECTED (so it samples its
//   own texel (0,0) for every fragment), `图像纹理.002.Color -> 自发光.Color`, `自发光.Strength` = 0.6999999284744263
//   (an unconnected default), and `自发光.Emission -> 相加着色器.Shader_001`. Measured on the image the group
//   names: `T_actor_common_matcap_07_D.png`'s texel (0,0) is `(0, 0, 0)` - so the branch contributes exactly
//   zero to every pixel of every material, and a port that carried it would carry a texture fetch, a lane and a
//   constant in order to add black. The reference's own authors left the wire off; the evidence is recorded in
//   `deren-ab/goo_step1_result.md` rather than reproduced here.
//
// ---- `AngleThreshold`: the reference's `calculateAngel` GROUP, WHICH THE IRIS DECLARES AS A SUB-GROUP ----
//
// `群组` in the iris group is one instance of `calculateAngel` (30 nodes), and only its `AngleThreshold` output is
// consumed (`Flip threshold` has no consumer anywhere in this group). Its whole body, from its own links:
//
//   `Vector Math.008 = DOT_PRODUCT(LightDirection, headUp)`
//   `Vector Math.009 = MULTIPLY(VM.008, headUp)`
//   `Vector Math.010 = SUBTRACT(LightDirection, VM.009)`      // the light direction, projected off the head's up
//   `Vector Math.011 = NORMALIZE(VM.010)`
//   `Vector Math.017 = DOT_PRODUCT(VM.011, headRight)`        // -> `运算.003`'s A, and `Flip threshold`
//   `Vector Math.019 = MULTIPLY(headForward, 值(明度) = -1.0)`
//   `Vector Math.020 = DOT_PRODUCT(VM.011, VM.019)`           // -> `运算.003`'s B
//   `运算.003 = ARCTAN2(VM.017, VM.020)`
//   `运算.004 = DIVIDE(运算.003, 3.141592502593994)`          // the reference's own pi, to the float
//   `运算.005 = GREATER_THAN(运算.004, 0.0)`
//   `运算.006 = ADD(1.0, 运算.004)`; `运算.007 = SUBTRACT(1.0, 运算.004)`
//   `混合.001 = MIX(factor = 运算.005, A = 运算.006, B = 运算.007)`
//
// so `AngleThreshold = 1 - |atan2(dot(proj, headRight), -dot(proj, headForward))| / pi`: the azimuth of the sun
// around the head's own up axis, as a fraction of a half turn, folded to [0,1]. THE SIGN OF THE FORWARD AXIS IS
// THE REFERENCE'S AND IT IS NOT COSMETIC: `值(明度)` is a `ShaderNodeValue` whose output default is `-1.0` in BOTH
// dumps of this group, and it is the reason a sun IN FRONT of the face gives `angle = 0` (full albedo, full
// highlight brightness) rather than `angle = 1`.
//
// ---- GOO-CONVENTION: THE MATCAP'S V AXIS ----
//
// The reference's `矢量变换` is `WORLD -> CAMERA`, and Blender's camera space has V growing UPWARD through its
// images. This renderer samples its textures with V growing DOWNWARD (the glTF/Vulkan convention), so the SAME uv
// value reads a DIFFERENT row of the same PNG; reproducing the reference's image therefore means negating V, and
// the negation below is that, not a change of formula. IT IS INDEPENDENTLY CONFIRMED BY THE OLD CHAIN: the
// `toon_matcap_uv` helper this file includes carries the same flip and its note records the measurement that
// forced it (the un-negated form killed the catchlight, 1219 lit pixels down to 6 at yaw 0). `toon_matcap_uv(n,v)`
// and the expression below are the same function - one from a view basis, this one from the camera matrix - and
// `tests/test_goo_toon_math.cpp` asserts that equivalence over a sweep of normals.
//
// ---- GOO-CONVENTION: `normalize` OF A DEGENERATE VECTOR ----
//
// Blender's `NORMALIZE` answers `(0,0,0)` for a zero-length input; Slang's answers a NaN. The input here is the
// light direction projected off the head's up axis, which is zero when the sun is exactly overhead in the head's
// frame - so the guard below reproduces Blender's answer rather than this platform's, and the case is reachable
// rather than theoretical (the sun's configured direction is a free parameter).
//
// ================================ WHERE THE PER-MATERIAL NUMBERS COME FROM ==================================
//
// `Eyes brightness` (1.5) and `Eyes HightLight brightness` (10.0) are the two MATERIAL overrides on Laevatain's
// iris in the Goo project (`goo_params_laevatain.tsv`, both `材质覆写`), and they ride
// `deren::vulkan::toon_colour_lane::goo_eye_brightness` - the carrier this renderer already uses for a per-material pair
// of scalars (`_Specular`, `_ParallaxScale`), because the material record is inline in the per-draw push block
// and has no room for them. `< 0` in a component means "this asset states no such row FOR THAT SOCKET", tested
// one component at a time because the reference lets a material state one and not the other; the answer then is
// the REFERENCE'S OWN INTERFACE DEFAULT rather than a number chosen here: both sockets default to `0.0` in the
// group's interface, which is what a material that calls the group without stating them gets in Goo.
//
// THE MATCAP IMAGE IS A PER-FAMILY TEXTURE IN THE REFERENCE - it is inside the group, not an input of it - and it
// rides a per-material lane here anyway (`deren::vulkan::toon_slot::goo_matcap05`). That is the asset pipeline's shape
// rather than a claim about the art: every toon texture in this port arrives as a named glTF image joined through
// the model's sidecar, and a material that states no such row simply reads no ball (see the `!= 0u` guard below,
// whose "no ball" is BLACK rather than this renderer's white fallback - white would be a much stronger statement
// about a missing texture than black is).

#define VR_GOO_TOON_CHAIN 1
#include "character_forward.slang"
// ---- AND THE ONE THING THIS INCLUSION CHANGES IN THE INCLUDED FILE: THE OLD CHAIN'S OWN RIM ----
//
// `character_forward.slang` shades a rim INSIDE `toon_diffuse` - the article's view-angle Fresnel plus its
// light-azimuth term - and this file calls that body for every non-eye family. Left alone, the character would
// wear the article's rim AND the reference's, which is the "no double rim" failure this step has to prevent.
// The define above is how the suppression is expressed and it is WORTH STATING WHY IT IS THE SHAPE AND NOT A
// SECOND PARAMETER ON THE BODY:
//
//   * `toon_diffuse` has THREE callers (`character_forward.slang`'s own entry point, `outline.slang` and this
//     file), and the truth being expressed is a property of the FILE DOING THE CALLING rather than of any one
//     call - "this module draws the reference's shading model, and that model's rim is the reference's". A
//     parameter would make every call site state the same constant and would invite a fourth caller to state it
//     wrong; a preprocessor symbol is set once, by the module that knows.
//   * the OTHER consumer, `outline.slang`, does NOT define it (it includes the same file directly), so the
//     outline keeps the old rim on every family - which is required, because the outline's hull is shaded by the
//     old body and an outline whose lighting disagreed with the surface under it would be its own bug.
//   * it leaves the unguarded arm of that file BYTE-FOR-BYTE the code it has today, including the reads of
//     `rig.rim` - which is what the "the old pipeline's output does not move by one pixel" criterion is verified
//     against, and which a runtime parameter could not promise as cheaply.
//
// WHAT THE GUARDED ARM DOES, EXACTLY: it keeps the whole old rim computation and zeroes its RESULT for the four
// families this step ports (`base`, `skin`, `cloth`, `hair` - the families the Goo project gives a rim to through
// `PBRToonBase` / `PBRToonBaseHair`), and leaves it alone for `face` and for every family this step does not
// port. The FACE is excluded on the reference's own evidence and not for convenience: `PBRToonBaseFace` has a
// `Rim_Color` socket and NO rim-width or attenuation ports, and where that colour goes is the spec's §9-U7
// (undetermined) - so the face's only rim is the old one, and it keeps it.

// ============================ THE REFERENCE'S GROUP-INTERNAL CONSTANTS ============================
//
// Every number below is a socket's value or a factor in `gooblender/nodes.json`'s
// `Arknights: Endfield_PBRToon_irisBase`, and the node's name is given so the value can be re-read rather than
// trusted. THE WRITE-UP'S ROUNDED FORMS ARE NOT USED: `MAT_OVERRIDES.md` renders the layer weight as `0.567`,
// while the .blend stores `0.5666666626930237`, and the stored value is what the reference computes with.

/// `运算.Value` - the constant the azimuth is subtracted FROM. THE GOO PROJECT'S VALUE: `Chen.blend` states `1.1`
/// here, and that difference is one of the two this group has between the two dumps.
static const float goo_iris_angle_center = 1.0;
/// `钳制.Min` / `钳制.Max` - the window the albedo's weight is clamped into. Both dumps agree on these two.
static const float goo_iris_window_min = 0.5;
static const float goo_iris_window_max = 1.0;
/// `混合.002.Factor_Float` - the second layer's weight in an ADD mix.
static const float goo_iris_ball_weight = 0.5666666626930237;
/// `运算.004.Value_001` - the divisor that turns the azimuth into a fraction of a half turn. The reference's own
/// float literal for pi (`3.141592502593994`, pi to float precision) rather than a platform constant, so the two
/// sides agree to the bit.
static const float goo_iris_half_turn = 3.141592502593994;
/// `值(明度)`'s output default inside `calculateAngel` - the sign the forward axis is multiplied by. `-1.0` in
/// both dumps of that group.
static const float goo_iris_forward_sign = -1.0;
/// The unconnected default of `自发光.Strength` on the DEAD second layer (see the header): recorded because the
/// number is part of what makes the branch dead, NOT applied, because the layer it multiplies is black.
/// (no constant: the branch is not implemented - the note above is the record.)

/// @brief the reference's `calculateAngel.AngleThreshold` - the sun's azimuth around the head's up axis
///
/// @param light_direction WORLD-space surface-to-light direction (the reference's `LightDirection` attribute,
///        which its own geometry-node tree `日光方向传递` writes; the port reads the engine's sun)
/// @param head_up / @p head_right / @p head_forward the head's own world-space basis (the reference's `headUp` /
///        `headRight` / `headForward` attributes; the port reads `heap_slots_scene_head`)
/// @return `1 - |azimuth| / pi`, in [0,1]: 0 when the sun is IN FRONT of the face, 1 when it is directly behind
/// @note THE RETURN IS NOT CLAMPED HERE and the reference does not clamp it either: the caller's `钳制` is what
///       bounds the albedo weight, and the brightness branch deliberately reads the UNCLAMPED value.
static float goo_iris_angle_threshold(const float3 light_direction, const float3 head_up, const float3 head_right, const float3 head_forward) {
    // the light direction with the head's up component removed - Blender's DOT_PRODUCT / MULTIPLY / SUBTRACT chain
    const float3 projected = light_direction - dot(light_direction, head_up) * head_up;
    // Blender's NORMALIZE answers (0,0,0) for a degenerate input and Slang's answers a NaN (see the header)
    const float projected_length = length(projected);
    const float3 axis = projected_length > 1e-6 ? projected / projected_length : float3(0.0, 0.0, 0.0);
    const float right_component = dot(axis, head_right);
    const float forward_component = dot(axis, head_forward) * goo_iris_forward_sign;
    // ---- `ATAN2` OF TWO ZEROES, SPELLED OUT RATHER THAN LEFT TO THE DEVICE ----
    //
    // The reference's `运算.003` is Blender's `ARCTAN2`, i.e. C's `atan2`, and this degenerate input is
    // REACHABLE: it is the sun lying exactly along the head's up axis, where the projection above is zero (the
    // guard) and both components are therefore zero. C answers that call DETERMINISTICALLY - `atan2(+0, -0)` is
    // `+pi`, and the negative zero comes from `值(明度)`'s `MULTIPLY(headForward, -1.0)` - so the reference's own
    // answer is a full half turn: `turns = +1`, and with the MIX's branch selection below that is
    // `AngleThreshold = 0`, a fully lit iris.
    //
    // SPIR-V'S `Atan2` DOES NOT ANSWER IT: its result is UNDEFINED when both operands are zero, so the same
    // expression on the device may produce any number - including a NaN, which the caller's `clamp` cannot clean
    // up and which would reach the framebuffer. The reference's answer is therefore written out here. This is not
    // a choice between two plausible values: it is the one the reference computes, stated where the platform
    // declines to.
    const float azimuth_magnitude = abs(right_component) + abs(forward_component);
    const float azimuth_turns = azimuth_magnitude > 1e-12 ? atan2(right_component, forward_component) / goo_iris_half_turn : 1.0;
    return 1.0 - abs(azimuth_turns);
}

/// @brief the reference group's emission 1 for the iris: the albedo layer plus the matcap layer, scaled
///
/// @param albedo the material's base colour - `混合.A_Color` / `混合.003.B_Color`, which the material tree feeds
///        from its albedo texture. The reference applies no `_BaseColor` tint in this group, and the port's glTF
///        base colour factor is what carries that tint here; on every iris in this repository the factor is
///        white, so the two are the same value rather than two readings of it.
/// @param albedo_alpha the same texture's alpha - the reference's `D_Alpha`, the MIX factor between the two
///        brightnesses. NOT the same channel as a coverage alpha: it is the material's own authored number and it
///        is read as a weight, so it is passed explicitly rather than taken from the surface's output coverage.
/// @param shading_normal the WORLD-space shading normal the sphere map is built from
/// @param matcap_index the `toon_slot::goo_matcap05` lane's texture-array index, `0` for "this material states
///        none" (see the header - the answer is black, not this renderer's white fallback)
/// @param eyes the `toon_colour_lane::goo_eye_brightness` lane: `.x` = `Eyes brightness`, `.y` = `Eyes HightLight
///        brightness`, either `< 0` for "the asset states no such row" (see the header for the fallback)
static float3 goo_iris_emission(const float3 albedo,
                                const float albedo_alpha,
                                const float3 shading_normal,
                                const uint matcap_index,
                                const float2 eyes) {
    // ---- the sphere map: `n_view.xy * 0.5 + 0.5`, with V negated for this renderer's texture convention ----
    // `mul(view, float4(n, 0.0))` IS the reference's `矢量变换(WORLD -> CAMERA)`: this renderer's camera matrix
    // has the camera's basis in its rows, which is the same space Blender's CAMERA conversion produces (see the
    // header's GOO-CONVENTION note, and `toon_matcap_uv` for the independent confirmation).
    const float3 normal_view = mul(camera_at(heap_camera_slot).view, float4(shading_normal, 0.0)).xyz;
    const float2 matcap_uv = float2(normal_view.x, -normal_view.y) * 0.5 + 0.5;
    const float3 ball = matcap_index != 0u
                            ? heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + matcap_index), heap_sampler_texture, matcap_uv).rgb
                            : float3(0.0, 0.0, 0.0);

    // ---- the two brightnesses, with the reference's own interface default behind the sentinel ----
    const float2 brightness = float2(eyes.x >= 0.0 ? eyes.x : 0.0, eyes.y >= 0.0 ? eyes.y : 0.0);

    // `混合` : `albedo * clamp(1.0 - angle, 0.5, 1.0)`
    const float3 head_up = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).up.xyz);
    const float3 head_right = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).right.xyz);
    const float3 head_forward = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).front.xyz);
    const float3 light_direction = light_at(heap_light_slot).light_dir.xyz;
    const float angle = goo_iris_angle_threshold(light_direction, head_up, head_right, head_forward);
    const float albedo_weight = clamp(goo_iris_angle_center - angle, goo_iris_window_min, goo_iris_window_max);
    const float3 albedo_layer = albedo * albedo_weight;
    // `混合.003` then `混合.002`: `albedo_layer + 0.5666667 * (ball * albedo)`
    const float3 colour = albedo_layer + goo_iris_ball_weight * (ball * albedo);
    // `混合.001`: `lerp(Eyes brightness, (1.0 - angle) * Eyes HightLight brightness, D_Alpha)`, on the UNCLAMPED
    // angle - `运算`'s `use_clamp` is false, so this is not the same number the albedo weight above was clamped to.
    const float strength = lerp(brightness.x, (goo_iris_angle_center - angle) * brightness.y, albedo_alpha);
    return colour * strength;
}

// ============================================================================
// STEP 2'S EDGE LIGHT HAS MOVED OUT OF THIS FILE (STEP 3)
// ============================================================================
//
// The whole object-space rim - `Rim_Color · Rim_ColorStrength`, `Directional light attenuation`,
// `Fresnel attenuation`, `Vertical attenuation`, the hair's `Use Rimlimitation?` layer AND the screen-space
// `DepthRim` the step-2 header below used to defer - now lives in `shaders/goo_rim.slang`, drawn by the
// fullscreen stage `vulkan/pass/goo_rim.cpp` with an additive blend.
//
// WHY IT COULD NOT STAY HERE, in one sentence: `character_forward_targets = {scene_color, gbuffer_depth}` makes
// the depth an ATTACHMENT of this very pass, so this fragment cannot sample it - and the reference's composition
// is a PRODUCT (`rim_term = C ⊙ (D · F · V · depth_factor)`) that cannot be split across two passes. The long
// argument, the source citations and the per-node derivation are in `shaders/goo_rim.slang`'s header and
// `deren-ab/goo_step3_result.md`; this file keeps only the iris.
//
// WHAT THIS FILE STILL OWNS, and it is unchanged: the iris' two matcap layers, the `VR_GOO_TOON_CHAIN` guard
// that switches the OLD chain's rim off for the four families the Goo reference gives a rim to (a define that
// still has to be set here, because this is the module that knows which shading model it draws), and the
// non-eye branch's call into `toon_diffuse`. The old rim stays ON for `face` and `eye` - the face keeps it
// outright, and the eye never had one.

// ============================ THE ENTRY POINT: ONE BRANCH, AND EVERYTHING ELSE UNCHANGED ============================
//
// THE FAMILY IS THE GATE, and it is the reference's own partition rather than a heuristic: the Goo project attaches
// `Arknights: Endfield_PBRToon_irisBase` to exactly four materials - `M_actor_{chen,laevat,_pelica,yvonne}_iris_01`
// - and `deren::gltf::toon_family_of` classifies exactly the names that contain `iris` (among its eye tokens) as
// `toon_family::eye`. Measured over every character glb in this repository: one eye-classified material per
// character (two on `tangtang`), each an `*_iris_0*`, and no other material of any of them matches. So "family ==
// eye" and "the reference uses the iris group" are the same set on every asset here, and the enumeration is in
// `deren-ab/goo_step1_result.md`.
//
// NOTHING ELSE IN THIS ENTRY POINT DIFFERS FROM THE OLD ONE - same `gather_surface`, same family table, same
// lane reads, same emissive term, same coverage rule, and for a non-eye material the same `toon_diffuse` call with
// the same arguments. The ONE thing the eye branch skips is the old chain's own eye block inside `toon_diffuse`,
// which is the point: the two chains must not both shade the iris.
[shader("fragment")]
float4 goo_toon_frag_main(
    [[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
    [[vk::location(1)]] float3 v_normal : TEXCOORD1,
    [[vk::location(2)]] float2 v_uv : TEXCOORD2,
    bool v_is_front : SV_IsFrontFace) : SV_Target {
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv);
    const toon_params params = toon_params_for(s.toon_family);

    // ---- THE IRIS: THE REFERENCE GROUP, WHOSE WHOLE OUTPUT IS EMISSION ----
    //
    // The two lanes it reads are the ones this step added: the Goo ball
    // (`toon_slot::goo_matcap05`, the second lane of the record's SECOND block) and the material's pair of
    // brightnesses (`toon_colour_lane::goo_eye_brightness`). The albedo and its alpha come from `gather_surface`,
    // i.e. from the same glTF base-colour texture the reference's material tree feeds the group from.
    if (s.toon_family == VR_FAMILY_EYE) {
        const uint4 lane_block_2 = toon_lanes2_at(heap_slots_toon_lanes, push.material_index);
        const float4 eyes = toon_colour_at(heap_slots_toon_colours, (push.material_index * character_toon_colour_lanes) + 6u);
        // THE ENGINE'S OWN EMISSIVE TERM IS STILL ADDED, on the same terms as the old entry point and for the same
        // reason: the G-buffer pass added it into the scene colour and this pass overwrites that pixel, so
        // dropping it would lose a term the frame already had. ON THIS MATERIAL IT IS EXACTLY ZERO - the iris'
        // glTF material states no `emissiveFactor` and no emissive map - which is why it changes nothing here; it
        // is carried because a second eye material in another asset may state one.
        const float3 lit = goo_iris_emission(s.albedo, s.alpha, normalize(s.normal), lane_block_2.y, eyes.xy) + s.emissive;
        const bool toon_transparent = (s.flags & 256u) != 0u;
        const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
        return float4(lit, out_alpha);
    }

    // ---- EVERY OTHER FAMILY: THE OLD BODY, WITH THE REFERENCE'S RIM LEFT TO ITS OWN PASS ----
    //
    // The lane reads and the argument list are copied from `character_forward.slang`'s own entry point, so a
    // non-eye pixel is the same computation in both modules - WITH ONE DELIBERATE DIFFERENCE, which is the
    // rewrite's: the body's own rim term is switched off for the four families the reference gives a rim to (see
    // the `#ifndef VR_GOO_TOON_CHAIN` note near the top of this file), and the reference's rim is NOT added here.
    //
    // WHERE THE RIM IS ADDED INSTEAD, and why it is not a composition choice: `混合.019` adds it to the colour
    // this function computes, but the factor it is added WITH (`运算.028`'s `min(DepthRim, 0.5)`) needs a depth
    // sample this pass cannot take - the depth is this pass's own attachment. So the whole product is composed by
    // `shaders/goo_rim.slang`, which reads the frame this stage wrote and ADDs the term to it with a ONE/ONE
    // blend. ON THE PIXELS THIS STAGE SHADES THE TWO ARE THE SAME SUM: `混合.019`'s ADD at factor 1.0 on the same
    // linear values, one stage later. The one thing this file must therefore NOT do is compute the rim a second
    // time, which is why the step-2 call that stood here is gone rather than multiplied by zero.
    const uint4 toon_lanes = toon_lanes_at(heap_slots_toon_lanes, push.material_index);
    // STEP 6's SATURATION COMES BACK OUT OF THE SHADING AND IS APPLIED TO THE PIXEL, exactly as
    // `character_forward.slang`'s `main` does it and for the same reason (`色相/饱和度/明度` is the reference's LAST
    // node, after its rim AND after its emission). The rim this pass cannot draw is added one stage later by
    // `goo_rim.slang`, which ADDs to what this writes - so this line's output is the pixel the desaturation
    // belongs on, and the rim that lands on top of it keeps its own saturation, which is the reference's own
    // arrangement. The value is the identity for every family this arm does not take.
    float desaturation = 0.0;
    // STEP 8'S DECODE stands here rather than inside `toon_diffuse`, because the shading normal is an ARGUMENT and
    // the reference decodes it where the normal map is sampled. `goo_toon_shading_normal` (defined in
    // `character_forward.slang`, which this file defines `VR_GOO_TOON_CHAIN` for before including it) answers the
    // `normalize(s.normal)` this call used to pass unless the material reads a normal map and its lane 24 row states
    // a strength, so a material with no row shades exactly as before.
    //
    // NAMED RATHER THAN INLINED AS OF STEP 15, and the name is the reason: the `armA` branch of the `RS EFF` block
    // below (mechanism table #14) needs the SAME shading normal, and the reference feeds it the same decoded normal
    // this call takes. Hoisting the call is what keeps a single decode in the stage - a second
    // `goo_toon_shading_normal(...)` would be a second `normalize` of the same three inputs and a second read of the
    // normal map, which is exactly the kind of "second normal" the port must not grow. The value is unchanged: the
    // call is evaluated once, exactly where it was, and passed by value.
    const float3 toon_shading_normal = goo_toon_shading_normal(v_world_pos, v_normal, v_uv, normalize(s.normal));
    const float3 colour = toon_diffuse(s.albedo, s.alpha, s.toon_family, toon_shading_normal, v_world_pos, s.ao, params, s.toon_indices, v_uv, toon_lanes, push.material_index, desaturation);
    // THE EMISSIVE TERM IS ADDED HERE RATHER THAN INSIDE THE `goo_hsv_desaturate` ARGUMENT so that the RS pass
    // below has something to be the LEFT operand of. It is the same sum the old entry point performs at the same
    // place (`character_forward.slang`'s `const float3 lit = colour + s.emissive;`), and for the same reason: the
    // G-buffer pass added `s.emissive` into the scene colour and this pass overwrites that pixel. It is named
    // `lit_base` rather than `lit` because `lit_final` below is NOT this value any more - the RS block sits
    // between them - and a reader who greps for `lit` should not have to decide which of the two he found.
    const float3 lit_base = colour + s.emissive;

    // ==================== STEP 13: `RS EFF` (MECHANISM TABLE #14) ====================
    //
    // WHAT THE REFERENCE DOES HERE, in its own node names: `混合.038 = _M ⊙ RS ColorTint` produces a colour which
    // `混合.029` (LIGHTEN, i.e. `mix(A, max(A, B), clamp(fac, 0, 1))`) combines with the shaded colour at factor
    // `RS Multiply Value`, and `混合.030` (MIX) then blends that result back over the shaded colour at factor
    // `Use RS_Eff?`. It sits AFTER the emission add and BEFORE `色相/饱和度/明度`, which is where the old entry
    // point's `lit` sits - the reference's own order.
    //
    // `_M` IS NOT A BARE TEXTURE and this is the part that is easy to get wrong: on both materials this asset
    // switches the pass on for, `_M` is the output of an `Arknights: Endfield_SmoothStep` subgroup instantiated
    // with `min = 0` and a PER-MATERIAL `max` (`0.9900000095367432` for `M_actor_laevat_cloth_02`, `1.0` for
    // `M_actor_laevat_cloth_05`), whose `x` is `float_from_vec4(图像纹理.Color)`. `float_from_vec4` is
    // `dot(v.rgb, vec3(0.2126, 0.7152, 0.0722))` - Rec.709 luminance, NOT the `(r + g + b) / 3` average the
    // reference's *other* helper (`float_from_vec3`) computes - and `t * t * (3 - 2t)` is the subgroup's own
    // smoothstep, expanded here rather than called as a built-in so the op order is the reference's. The mask lane
    // holds the RAW image (uploaded UNORM: it is a Non-Color mask and the reference decodes no sRGB for it), and
    // the `max` rides `_GooRSTint.w` rather than being baked at upload time.
    //
    // `min` IS HARD-CODED `0.0` (spec U11): it is `0.0` on every material measured, the port carries no component
    // for it, and a material with a non-zero `min` would need a lane of its own. `max <= 0` is read as `1.0`,
    // which is a DELIBERATE deviation (spec U7): the neutral lane is `(0, 0, 0, 0)`, so without it the divide
    // below would be by zero.
    //
    // `armA` IS PORTED AS OF STEP 15, WITH THREE LIMITS THAT ARE STATED HERE RATHER THAN DISCOVERED LATER. The
    // reference's `RS EFF` has two arms - `RS Model = 0` takes the two 256x1 `_RS` sheets, `RS Model = 1` takes
    // `_M ⊙ RS ColorTint` - and this block now takes whichever the component names, so the gate below is
    // `Use RS_Eff?` alone and the `RS Model` test lives INSIDE it:
    //
    //   L1 - ONE SHEET PER DRAW, CHOSEN ON THE HOST, SO `RS_Index` IS QUANTISED TO AN ENDPOINT. The reference
    //        MIXES two per-character `_RS` sheets CONTINUOUSLY - `mix(A, B, clamp(RS_Index, 0, 1))` (`混合.032`) -
    //        and a sheet is a TEXTURE lane, slot 15 being the last one this table has (see
    //        `deren::vulkan::toon_slot::goo_rs_sheet`), so `RS_Index` rides lane 29's `.x` and is answered by the HOST
    //        instead of here: `main.cpp` resolves the second sheet by name when `RS_Index >= 0.5` and this block
    //        still samples exactly ONE sheet (`.x` is never read in this file). A material stating `RS_Index = 1`
    //        therefore reads the second sheet; one stating a FRACTION between the endpoints is read as an endpoint
    //        rather than as the reference's blend - a NAMED LIMITATION of this route (spec §9.3 (z)(1)), not a
    //        reproduction. On the dumps it costs nothing: the `gooblender` dump has 23 `PBRToonBase` instances, 22 of
    //        them `0.0` plus the only `RS_Index = 1` material `M_actor_yvonne_cloth_03` (in no captured asset), and `chen_dump` adds 4 more, all `0.0` - 27 across both dumps.
    //   L2 - THE `u` REMAP IS NOT REACHED BY ANY CAPTURED MATERIAL. `u` comes off `Layer weight Value`, and both
    //        materials state `0.0`/`0.0`, so `u = 0` here by construction and the `remap`/`pow` pair below has only
    //        the CPU test's evidence behind it (see `tests/test_goo_toon_math.cpp`), not a frame.
    //   L3 - NO SHIPPED ASSET TAKES THIS ARM. Both materials on the shipped asset state `RS Model = 1`, so `armB`
    //        is what every shipped frame runs; `armA` is reached only by the `s13ctrl` fixture family. "Ported" here
    //        means "the branch exists and is exercised by a fixture", not "the shipped asset exercises it".
    //
    // WHAT `armA` DOES, in the reference's own node names: `混合.036 = sheet ⊙ RS ColorTint`; `混合.023` scales it
    // by `RS Strength` (with NO clamp, unlike the two later factors); `混合.027` multiplies by
    // `saturate(dot(light_dir, n_rs))`; `混合.028` multiplies by the `Cast Shadows` sigmoid; `混合.033` multiplies
    // by `_M`; and `混合.037` selects between the two arms by `clamp(RS Model, 0, 1)`. The multiplication ORDER is
    // the reference's own nesting and is kept as written - `((((sheet * tint) * strength) * NdotL) * cast) * mask` -
    // because floating-point multiplication is not associative and this port's whole point is bitwise fidelity.
    //
    // THE TWO ARMS SHARE `_M` AND THE `LIGHTEN`: `混合.037` feeds `混合.029` a SINGLE colour, so `_M` is read once
    // (by whichever arm) and the LIGHTEN/MIX pair below is arm-agnostic. That is why the mask is computed before the
    // branch rather than inside it.
    //
    // `RS Model` IS STILL CARRIED ON THE LANE for the reason the lane's own note gives: the branch is the
    // reference's own predicate rather than a count of materials, and on the one material where the two arms differ
    // the difference is now VISIBLE (the fixture frames) rather than suppressed.
    //
    // WHY AN `if` AND NOT A `mix`: the identity is required to be BITWISE. `mix(a, a, f)` expands to
    // `a * (1 - f) + a * f`, which is `a` only if both products round back to `a` exactly - true for `f == 0`
    // and `f == 1`, and NOT true in between. Nothing here needs a value in between: `Use RS_Eff?` is the branch's
    // switch, and with it at zero the reference's `混合.030` returns its A input at a factor of 0. So the gate is
    // taken with a branch and `rs_final` starts as a plain COPY of `lit_base`, which performs no floating-point
    // operation at all on the twenty materials that state no row. This is the same trade the old entry point
    // makes for the same reason.
    const uint rs_colour_base = push.material_index * character_toon_colour_lanes;
    const float4 rs_scalars = toon_colour_at(heap_slots_toon_colours, rs_colour_base + 27u); // `_GooRSScalars`
    const float4 rs_tint_lane = toon_colour_at(heap_slots_toon_colours, rs_colour_base + 28u); // `_GooRSTint`
    const float4 rs_arm0_lane = toon_colour_at(heap_slots_toon_colours, rs_colour_base + 29u); // `_GooRSArm0`
    const float rs_use = rs_scalars.x;
    // `RS Model` IS NAMED HERE AS OF STEP 15 BECAUSE IT IS NO LONGER THE OUTER GATE: it selects the ARM below
    // (0 = `armA`, the `_RS` sheet; 1 = `armB`, `_M ⊙ RS ColorTint`) and the outer gate is `Use RS_Eff?` alone.
    const float rs_model = rs_scalars.z;
    float3 rs_final = lit_base;
    if (rs_use > 0.0f) { // `!= 0.0f`, never `== 1.0f`: a boolean socket travels as a float
        // THE MASK'S LANE IS SLOT 14, i.e. `.z` OF THE THIRD `uvec4` BLOCK, and `0` means "do not read" - the
        // same contract every other lane uses. It is load-bearing HERE for a second reason: texture index 0 is the
        // WHITE fallback, so a material with no `_M` would otherwise get `_M = 1` everywhere, i.e. the strongest
        // possible statement about a pass it never switched on (spec F5). The UV is `v_uv`, UV0, which is the
        // coordinate the reference's own texture node takes, and the sampler is the same LINEAR one every other
        // toon texture read in this stage uses.
        //
        // IT IS READ BEFORE THE ARM BRANCH, not inside it: `混合.037` selects between the two arms and hands
        // `混合.029` ONE colour, so the `_M` product is shared and must not be recomputed per arm.
        float rs_mask = 0.0f;
        const uint4 rs_block3 = toon_lanes3_at(heap_slots_toon_lanes, push.material_index);
        if (rs_block3.z != 0u) {
            const float4 rs_texel = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + rs_block3.z), heap_sampler_texture, v_uv);
            // `float_from_vec4`, in the reference's op order: dot with the Rec.709 weights, subtract the min,
            // divide by `(max - min)`, clamp, then `t * t * (3 - 2t)`.
            const float rs_luma = dot(rs_texel.rgb, float3(0.2126f, 0.7152f, 0.0722f));
            const float rs_smin = 0.0f;
            const float rs_smax = (rs_tint_lane.w > 0.0f) ? rs_tint_lane.w : 1.0f;
            const float t = saturate((rs_luma - rs_smin) / (rs_smax - rs_smin));
            rs_mask = t * t * (3.0f - 2.0f * t);
        }
        float3 rs_eff = float3(0.0f, 0.0f, 0.0f);
        if (rs_model == 0.0f) {
            // ---- `armA` (`RS Model = 0`): the 256x1 `_RS` sheet, and the three limits are stated above ----
            //
            // THE SHEET'S LANE IS SLOT 15, i.e. `.w` OF THE THIRD `uvec4` BLOCK, and it follows `_M`'s `0 = do not
            // read` contract for the same reason `_M` does: texture index 0 is the WHITE fallback, so a material
            // with no `_GooRSSheet` row would otherwise sample white and light the cloth with it. A material that
            // states no sheet gets `arm0 = 0` - the arm's neutral - which is what the reference's own group does
            // for a caller that states nothing (lane 29's neutral is `(0,0,0,0)`, so `RS Strength = 0` zeroes the
            // product even before this test; the test is what keeps the TEXTURE READ away, which is the part that
            // actually matters).
            float3 rs_sheet = float3(0.0f, 0.0f, 0.0f);
            if (rs_block3.w != 0u) {
                // `u` IS BUILT OFF THE LAYER WEIGHT, AND `rs_facing` IS THAT NODE'S `Facing` OUTPUT. The
                // reference fork's Layer Weight node is verbatim
                // (`_ref/gooengine_src/gpu_shader_material_layer_weight.glsl:18-24`):
                //     facing = abs(dot(V, N));
                //     if (blend != 0.5) { blend = clamp(blend, 0.0, 0.99999);
                //                          blend = (blend < 0.5) ? 2.0 * blend : 0.5 / (1.0 - blend);
                //                          facing = pow(facing, blend); }
                //     facing = 1.0 - facing;
                // i.e. `Facing = 1 - |V.n|^remap(b)`, and the reference then feeds `u = clamp(Facing + Offset, 0, 1)`
                // to the sheet. `V` is the INCOMING vector (`coordinate_incoming`, i.e.
                // `normalize(camera_pos - world_pos)`, and NOT saturated); `n` is `n_rs`; `b` is
                // `Layer weight Value` (`rs_arm0_lane.z`); the offset is `Layer weight Value Offset`
                // (`rs_arm0_lane.w`). `v` IS THE CONSTANT `0.5` - the reference's `合并 XYZ.Y` - and it cannot
                // matter on a `256x1` image, which is what every `_RS` sheet in the dumps is; it is written as the
                // reference writes it rather than as this port's `character_ramp_band_v` for the reason
                // `character_forward.slang`'s ramp read states.
                //
                // `remap` IS THAT INTRINSIC STEP OF THE REFERENCE'S OWN GLSL - the `2b` / `0.5/(1-b)` remap
                // bracketing the `pow` - and NOT a claim that the node is "not a pow". It has a HOLE AT EXACTLY
                // `0.5`: `2b` below the midpoint and `0.5 / (1 - b)` above it are both `1.0` at `b == 0.5`, so the
                // reference's own graph collapses to a bare `facing` there and the port must skip the `pow`
                // rather than compute `pow(x, 1.0)`, which is NOT bitwise `x` on this platform. `b` is clamped to
                // `[0, 0.99999]` BEFORE the branch (which is why the test is on the raw `b` and not on the clamped
                // one: clamping cannot produce `0.5` from a non-`0.5`, and the upper guard is what keeps the
                // division finite).
                //
                // DEFECT (lead-found, 2026-10-01): UNTIL THIS FIX THE TWO LINES BELOW COMPUTED THE REFERENCE'S
                // COMPLEMENT. `rs_facing` was assigned `pow(1 - |V.n|, e)` and `u` then subtracted it from 1,
                // i.e. the net `u = 1 - (1 - |V.n|)^e + w` where the reference's `1 - |V.n|^e + w` is required.
                // The two forms agree only at `b = 0` (both give `u = 0`) and at the single fixed point
                // `|V.n| = 0.5`; everywhere else they are opposite (`b = 0.5`, `|V.n| = 0.8`: `0.200000000` correct
                // vs `0.800000000` before; `b = 0.2`: `0.085389896` vs `0.474694439`; `b = 0.8`: `0.427566598`
                // vs `0.982111456`). SHIPPING WAS UNAFFECTED FOR A REASON AND NOT BY LUCK: the shipped asset runs
                // `RS Model = 1`, so armA is unreachable there, AND every fixture that existed at the time had
                // `_GooRSArm0.z = 0`, where both forms give `u = 0` - which is why the suite, the gate and all 34
                // pre/post frames stayed green. The branch is NOT dead in the reference's own files: `chen
                // body/cloth` use `b = 0.5` and `M_actor_yvonne_cloth_03` uses `b = 0.7999999523162842` with
                // offset `0.3999999761581421` (`deren-ab/goo_ref_rs_sheet_sampling.md` §7, task-26). The `b != 0`
                // fixtures are what puts this path under frame-level test from now on.
                const float3 rs_to_camera = normalize(camera_at(heap_camera_slot).camera_pos.xyz - v_world_pos);
                const float rs_facing_abs = abs(dot(rs_to_camera, toon_shading_normal));
                const float rs_lw_blend = rs_arm0_lane.z;
                float rs_facing;
                if (rs_lw_blend == 0.5f) {
                    rs_facing = 1.0f - rs_facing_abs;
                } else {
                    const float rs_blend_clamped = clamp(rs_lw_blend, 0.0f, 0.99999f);
                    const float rs_exponent = (rs_blend_clamped < 0.5f) ? (2.0f * rs_blend_clamped) : (0.5f / (1.0f - rs_blend_clamped));
                    rs_facing = 1.0f - pow(rs_facing_abs, rs_exponent);
                }
                const float rs_sheet_u = saturate(rs_facing + rs_arm0_lane.w);
                // ---- THE SHEET'S ADDRESS MODE IS THE REFERENCE'S `EXTEND`, REPRODUCED HERE AS A UV CLAMP ----
                // The node that feeds this sheet is `图像纹理.005` and it asks for
                // `image_user.extension = "EXTEND"` (`gooblender/nodes.json:93653-93659`, the same six lines that
                // name `T_actor_aurora_cloth_03_RS.png`), i.e. CLAMP, NOT REPEAT. THIS PORT CANNOT HONOUR A
                // PER-IMAGE ADDRESS MODE: the heap hands every texture ONE sampler, `heap_sampler_texture`
                // (`shaders/heap_slot_constants.glsl:166-175`), whose address mode is REPEAT
                // (`vulkan/core/core.cpp:436-437`), and there is no per-image sampler slot. So the reference's
                // SEMANTICS are reproduced instead of its setting: `u` is pulled into the interval where REPEAT
                // and CLAMP return the same texel. They are pointwise equal for this sheet - LINEAR filtering,
                // a `256x1` image, and `u` already inside `[0, 1]` (it is `saturate`d above). At `u == 0` the
                // EXTEND bilinear footprint has BOTH taps clamped to texel 0 and returns exactly `T0`, which is
                // exactly what REPEAT returns at `u = 0.5/256`; `u == 1` and texel 255 are the mirror case; and
                // strictly inside the interval the clamp is inactive and both modes filter the same texels.
                // (The equality was MEASURED off the renderer by `deren-ab/_lead_extend_equiv.py`: two bilinear
                // mip-0 paths over the real sheet, 200001 sample points across `u` in `[0,1]`, max
                // `|EXTEND - this inset|` = `0.000e+00`; `EXTEND @u=0` comes out `(0, 96, 255, 255)` = `T0` while
                // `REPEAT @u=0` comes out `(43, 106, 255, 255)` = the seam average. That instrument reads the
                // ASSET and not this shader - see the (w) limit below.)
                //
                // (w) THE PORT HAS ONE SAMPLER PER HEAP, SO THE REFERENCE'S PER-IMAGE `extension` IS IGNORED.
                // The 162 image nodes in the reference dump are 155 `REPEAT`, 6 `EXTEND` and 1 `CLIP`; the
                // Base/Face/Hair `_RD` ramps and `T_actor_laevat_cloth_02_M.png` are affected in principle and
                // are NOT measured by this step. Only the `_RS` sheet gets the texel-centre clamp here.
                //
                // 256 IS THE SHEET'S OWN WIDTH and is not invented here: every `_RS` sheet in the dumps is
                // `256x1`, and the slot documentation of `deren::vulkan::toon_slot::goo_rs_sheet` states the same.
                const float goo_rs_sheet_width = 256.0f;
                const float2 sheet_uv = clamp(
                    float2(rs_sheet_u, 0.5f),
                    float2(0.5f / goo_rs_sheet_width, 0.5f),
                    float2(1.0f - 0.5f / goo_rs_sheet_width, 0.5f));
                const float4 rs_sheet_texel = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + rs_block3.w), heap_sampler_texture, sheet_uv);
                // LINEAR, AND THE DECODE IS THE SAMPLER'S: the lane is `VK_FORMAT_R8G8B8A8_SRGB` (see
                // `deren::vulkan::toon_slot::goo_rs_sheet`), so the texel arrives already decoded and a second decode
                // here would double-apply the curve. `RS_Index` IS NOT READ **HERE**: it selects WHICH sheet the
                // host bound to this one slot (see L1 above), so by the time this line runs the choice is already
                // made and lane 29's `.x` has nothing left to say.
                rs_sheet = rs_sheet_texel.rgb;
            }
            // THE LIGHT DIRECTION AND THE TWO SIGMOID INPUTS ARE RECOMPUTED HERE rather than borrowed from
            // `toon_diffuse`, and that is a LIMIT OF SCOPE rather than a choice: `goo_ndotl_clamped`
            // (`character_forward.slang:2817`) and `cast_shadow_sigmoid` (`:2533`) are LOCALS of that function, so
            // the RS block cannot see them. The expressions below are that function's own, leaf for leaf:
            // `l = normalize(light_at(heap_light_slot).light_dir.xyz)` (`:1238`),
            // `saturate(dot(l, n))` (`:2817`), `goo_sigmoid_sharp(shadow_used, cast_center, cast_sharp)` (`:2533`)
            // with `cast_center`/`cast_sharp` off `_GooDiffuseA.z/.w` under the same `-1000` sentinel (`:2497-2498`,
            // defaults `0.0`/`0.0`), and `shadow` is `:1369`'s `toon_indices.w != 0u ? 1.0 : calc_shadow(pos, n)`.
            //
            // ONE TERM OF `shadow_used` IS NOT PORTED HERE AND IT IS THE HAIR MODIFIER (`:1530-1532`'s
            // `(toon_lanes2.x != 0u) ? lerp(1.0, shadow, orm.x) : shadow`): `orm` is a texture read INSIDE
            // `toon_diffuse` (`:1475`) and is not in scope either, so this reads the bare `shadow`. On both
            // materials this asset switches the arm on for, `toon_indices.w == 0` and `toon_lanes2.x == 0`, so the
            // two spellings are bitwise the same value there; on a material with a split normal they would not be,
            // and that is a stated gap rather than a silent one.
            const float3 rs_light_dir = normalize(light_at(heap_light_slot).light_dir.xyz);
            const float rs_ndotl = saturate(dot(rs_light_dir, toon_shading_normal));
            const float4 rs_cast_lane = toon_colour_at(heap_slots_toon_colours, rs_colour_base + 11u); // `_GooDiffuseA`
            const float rs_cast_center = rs_cast_lane.z > goo_lane_absent_threshold ? rs_cast_lane.z : goo_castshadow_center_default;
            const float rs_cast_sharp = rs_cast_lane.w > goo_lane_absent_threshold ? rs_cast_lane.w : goo_castshadow_sharp_default;
            const float rs_shadow = (s.toon_indices.w != 0u) ? 1.0f : calc_shadow(v_world_pos, toon_shading_normal);
            const float rs_cast_sigmoid = goo_sigmoid_sharp(rs_shadow, rs_cast_center, rs_cast_sharp);
            // THE BRACKETING IS THE REFERENCE'S OWN (`混合.036` -> `混合.023` -> `混合.027` -> `混合.028` ->
            // `混合.033`) and it is kept as nested parentheses rather than a flat product because
            // floating-point multiplication is not associative. `RS Strength` (`.y`) IS NOT CLAMPED here, which is
            // the reference's own arrangement: only `RS Multiply Value` and `Use RS_Eff?` are clamped, below.
            rs_eff = ((((rs_sheet * rs_tint_lane.rgb) * rs_arm0_lane.y) * rs_ndotl) * rs_cast_sigmoid) * rs_mask;
        } else {
            rs_eff = rs_mask * rs_tint_lane.rgb;                                                                // `混合.038`, armB, UNCHANGED
        }
        const float3 mixed029 = mix(lit_base, max(lit_base, rs_eff), saturate(rs_scalars.y));                   // `混合.029`, LIGHTEN
        rs_final = mix(lit_base, mixed029, saturate(rs_use));                                                   // `混合.030`, MIX
    }

    const float3 lit_final = goo_hsv_desaturate(rs_final, desaturation);
    const bool toon_transparent = (s.flags & 256u) != 0u;
    const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
    return float4(lit_final, out_alpha);
}
