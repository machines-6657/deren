// shaders/character_forward.slang - THE TOON CHARACTER FORWARD STAGE.
//
// WHAT IT IS FOR, and it is the one thing this renderer was missing to draw a toon character
// correctly: a pass that runs AFTER the lighting stage and OVERWRITES the pixels it already lit, so
// the character can have its own hand-authored shading instead of the deferred one - without being lit
// TWICE, which is what happens if a character's finished colour is fed back through the G-buffer.
//
// THE ARCHITECTURE IS TAKEN FROM THE REFERENCE PORT, NOT INVENTED HERE (see _toon_ref/PORT_SPEC.md):
// DanbaidongRP's PBRToon material family declares a pass named `CharacterForward` carrying `ZWrite Off` +
// `ZTest Equal`, invoked after `DeferredLighting`. The `EQUAL` is the whole mechanism: the pass lands on
// exactly the surface the G-buffer pass recorded, so it REPLACES the deferred result rather than
// layering over it, and nothing needs a mask.
//
// WHY THE OUTPUT IS LINEAR AND NOT DISPLAY-REFERRED. The same reference is explicit about this and it is
// why the MME shader pack this port started from did not fit: `PBRToonBase.shader` ends at
// `PostEvaluate(...)` -> `return float4(resultColor, 1)`, and the tonemap is the post chain's alone. A
// stage that encoded sRGB here would be tonemapped a second time by post.slang's ACES composite.
//
// THE INCLUDE ORDER IS PART OF THE CONTRACT (see gbuffer.slang): the shim first, then surface.glsl, which
// declares the push-constant block and the heap_frame_slot / heap_image_index aliases the slot macros
// expand against; then shading.glsl, whose functions address the per-frame arrays through those macros.
// Compiling a body before surface.glsl reports heap_frame_slot as an undefined identifier - measured.
//
// The alphaMode MASK discard is NOT written here because `gather_surface` already performs it (see
// surface.glsl): it is the material's rule, not the pass's, so no pass can forget it. The BLEND alpha rule
// IS written here, copied from unlit.slang, because it is the pipeline's coverage contract.

#include "heap_access.slang"
#include "surface.glsl"
#include "shading.glsl"

#include "toon_params.slang"

/**
 * @brief the toon stage's VIEW-ANGLE rim - the second half of both references' rim light
 *
 * BOTH references carry TWO rim terms, and they are different mechanisms rather than two tunings of one:
 * a view-angle term here, and a SCREEN-SPACE DEPTH term that compares this fragment's depth against the
 * depth at a screen offset (the MME pack's `Draw*ScreenRim` passes sampling the viewport map's depth
 * channel; the reference's `GetCharacterDirectRimLightArea` sampling `_CameraDepthTexture`). This is the
 * first one, and it is here first because it needs NOTHING the stage does not already have - a normal, the
 * view vector and the light vector.
 *
 * THE DEPTH TERM CANNOT FOLLOW WITHOUT A NEW RESOURCE, and the reason is a Vulkan rule rather than an
 * omission: this pass holds the depth image as its DEPTH ATTACHMENT (that is what makes `EQUAL` possible),
 * and an image cannot be a depth attachment and a sampled texture in the same instance. The reference has a
 * `CopyDepthPass` producing `_CameraDepthTexture` for exactly this. That copy is the next stage's work.
 *
 * `rim_power` comes from the family's table now rather than from a constant here, because the references
 * carry it PER FAMILY (`EF_*_RIM_AREA` / `EF_*_RIM_STRENGTH` / `EF_*_RIM_COLOR` exist once per family, and
 * hair's rim colour is its `MATERIALEDGECOLOR`). `rim_light_end` stays a constant and is the reference's
 * light-side mask: the rim is drawn on the LIT side only, because a rim on the shadow side reads as a second
 * light source rather than as a contour.
 */
static const float character_rim_light_end = 0.35;

/**
 * @brief the V band a RAMP ATLAS's diffuse ramp lives at
 *
 * The reference samples ONE ramp texture twice, at two V bands - the diffuse ramp at 0.125 and the specular
 * ramp at 0.375 - which is why a single map serves two lookups. The band is the ASSET's layout rather than this
 * stage's choice, so it is named rather than inlined: a ramp authored with its bands elsewhere would need this
 * number, and finding it here is faster than finding it in a shader body.
 */
static const float character_ramp_band_v = 0.125;

/// @brief the V band the reference's SPECULAR ramp lives at, in the same atlas as the diffuse band above
///
/// The reference samples one atlas twice, and this is the second sample. The two bands are different MECHANISMS
/// rather than the same curve at two offsets - the diffuse band is indexed by the shadow-gated half-Lambert and
/// this one by the half-vector angle - so they get a constant each instead of one constant and an assumption.
static const float character_ramp_spec_band_v = 0.375;

/// @brief the MATCAP's strength - the reference's `EF_EYE_IRIS_MATCAP05_STRENGTH`
///
/// The reference carries it per MATERIAL (`EF_EYE_IRIS_MATCAP05_STRENGTH = 0.5666667`, recorded from the material
/// the pack was extracted from) rather than per family, and the family table here has no matcap strength because
/// until this lane existed nothing read one. It is a named constant rather than an inline literal for the reason
/// the bands above are: a number whose provenance is a specific material should say so where it is written.
static const float character_matcap_strength = 0.5666667;

/// @brief the CORNEA's bump - the article's `_CorneaBumpStrength`, whose per-material value this port cannot carry
/// @note it is the `0.125` in `corneaTS.xy = 0.125 * _CorneaBumpStrength * centredUV`, i.e. how far the hemisphere
///       tilts at the iris' edge. Named rather than inlined for the reason the other recorded constants are: a
///       number whose provenance is one material should say so where it is written.
static const float character_cornea_bump = 0.125;

/// @brief the article's `_ParallaxDepth` for the eye - the distance the iris' albedo lookup slides behind the cornea
/// @note the VALUE is the game's own (`M_actor_chen_iris_01` states `_ParallaxScale = 0.03`, and the article's
///       property of that name is what its `parallaxOffset = offsetDir * _ParallaxDepth * float2(1.0, 0.25)` reads;
///       the same material's brow carries 0.5, so it is per-material in the game).
/// @note IT IS NO LONGER THE VALUE THE STAGE USES ON EVERY MATERIAL - it is the FALLBACK behind the asset's own
///       per-material row, and the note that used to stand here ("this is the one material's, recorded here
///       exactly as `character_matcap_strength` records another's" ... the per-material value cannot be carried)
///       is now WRONG and was replaced by what is measured: `_ParallaxScale` reaches the GPU as
///       `toon_colour_lane::parallax_scale` (the asset's `extras` block, via
///       `deren::gltf::claimed_extras_floats`), and the read in `toon_diffuse` is
///       `lane.x >= 0 ? lane.x : this constant`. On `chars\chen.glb` the asset states the row for exactly three
///       materials - iris 0.03, brow 0.5, `M_actor_chen_cloth_01` 0.5 - and stays silent for the other four, so
///       THIS CONSTANT IS STILL LIVE and still 0.03: it is the answer for every material whose sources say
///       nothing (the body, the face, the hair, cloth_02, and every non-character model), and on the iris the
///       asset's 0.03 and this constant are the same number to the last bit, which is why the iris renders
///       unchanged.
/// @note AND THE OTHER TWO MATERIALS THAT STATE 0.5 DO NOT MOVE EITHER, which is the measured answer rather than
///       the expected one: the parallax block is gated on the material's MATCAP (`toon_indices.w != 0`) and only
///       the iris has one, so wiring this lane changes zero pixels on this character. The probe that separates
///       "read" from "ignored" (the iris re-stated as 0.3) is recorded in `remaining_port_spec.md`'s
///       "extras 数据源" section, and its mask is `deren-ab/parallax_zoom.png`.
///       Named rather than inlined for the reason the other recorded constants are.
static const float character_eye_parallax_depth = 0.03;

/// @brief the article's `_SpecularColor` for the eye's matcap - ITS OWN PROPERTY, and the port has no game row for it
/// @note the article's eye shader declares it `("SpecularColor", Color) = (1,1,1,1)` and its specular is
///       `specularColor * _SpecularStrength + _SpecularColor * specularW`, where `specularColor`/`specularW` are the
///       matcap's RGB and A. No sidecar this repository carries has a `_SpecularColor` row, so the article's own
///       default is what this is - named rather than inlined because it is a value whose provenance is the article,
///       not the game, which is the opposite of the two constants above it.
static const float3 character_eye_specular_colour = float3(1.0, 1.0, 1.0);

/// @brief the article's `* 2.5` on the eye's two trick colours (`_SpecularTrickColor` / `_EyeInTrickColor`)
/// @note WHERE the trick colours go is the article's and is settled (the LIT layer - see `eye_trick`), but HOW FAR
///       they go is not: the article's `* 2.5` is its own compensation for Unity's light units, and this engine's
///       sun carries roughly 7.5x the radiance the article's does (the reason the hair LUT needed a `/PI` bridge),
///       so copying the article's factor AND this engine's magnitudes at once is not a faithful port - it is two
///       unit systems multiplied. It is 1.0 here, which is this port's existing magnitude, and the A/B that decides
///       between 1.0 and 2.5 is this one number.
static const float character_eye_trick_scale = 2.5;

/// @brief the article's `_SpecularMatcap`: the matcap's own tint, which the game's table carries ONLY for chen's
///        iris (`_MatcapColor` = (0.145, 0.161, 0.314, 0.933)) and which this port does NOT read yet
/// @note RECORDED RATHER THAN INVENTED, and the note is why it is written down instead of implemented: every other
///       colour lane in this stage exists because something consumes it (see `deren::vulkan::toon_colour_lane`), and this
///       one has no consumer, so writing it into a lane would be a second source of truth for a value nothing
///       reads. The reference's own name for the property is `EF_EYE_IRIS_MATCAP05_STRENGTH` for the STRENGTH
///       (`character_matcap_strength` below, 0.5666667) and the article's is `_SpecularMatcap`; the TINT is a
///       third thing and it is the one the game states. The A/B that would settle it: apply
///       `_MatcapColor.rgb` with `_MatcapColor.a` (0.933) as its weight to the eye's highlight and compare the
///       iris against the game's own frame.


/**
 * @brief the MATCAP's UV - the sphere map, from the view-space normal
 *
 * THE THIRD VARIABLE, after the shading term (ramps) and the material's own albedo (the shadow LUT): a matcap is
 * indexed by WHERE THE SURFACE POINTS RELATIVE TO THE CAMERA, which is why a matcap image is a picture of a ball
 * and why it is the one toon map that does not depend on the light at all.
 *
 * THE REFERENCE'S OWN REMAP, `normalVS.xy * float2(0.5, -0.5) + 0.5`, INCLUDING THE FLIPPED Y - both
 * `EfEyeIrisApplyMatcap05` and `EfClothSampleMatcap` carry it, and the latter's comment is explicit that it is
 * there because the texture's V axis is opposite the convention the map was authored in.
 *
 * THE VIEW BASIS IS BUILT FROM THE VIEW VECTOR rather than read from a view matrix, and the whole difference is
 * a ROLL about the view axis: `right` and `up_ortho` are any stable pair orthogonal to `v`, so a matcap read
 * this way agrees with the reference's up to a rotation of the ball - which a matcap cannot care about, because
 * a ball has no distinguished up. What it buys is not needing a second per-frame matrix in this stage for one
 * lookup.
 *
 * MEASURED (阶段④, `remaining_port_spec.md`): THE ROLL HERE IS EXACTLY ZERO, so this basis IS the article's
 * `mul((float3x3)UNITY_MATRIX_V, corneaNormalWS)`. The article builds its own `cameraRight` as
 * `cross(worldUp, cameraForward)` with `cameraForward = UNITY_MATRIX_V[2].xyz` (`ZmdEyeToonMain.hlsl:70,73-74`)
 * - the same world-up, the same row 2 of the same matrix this port's `camera_forward` reads - so its `right`
 * and `up` are this function's `right` and `up_ortho` to the bit. Measured as two real builds: the view-space
 * form and this one render the IDENTICAL frame at yaw 0/30/60 and at the user pose (0 px differing).
 * THE ONLY REAL DIFFERENCE FROM THE ARTICLE'S LINE IS THE SIGN OF V, and the sign is REQUIRED: the article's
 * literal `n_vs.xy * 0.5 + 0.5` on this port's glTF/Vulkan texture (V downwards) kills the catchlight -
 * MEASURED 1219 lit px inside the eye at yaw 0 down to 6 px, and 1206 px of the front iris changed - which is
 * the same V-axis inversion `EfClothSampleMatcap`'s comment describes. So this `-dot(n, up_ortho)` is the
 * article's `corneaNormalVS.xy * 0.5 + 0.5` in this port's texture convention rather than a deviation from it.
 *
 * @param n world-space shading normal
 * @param v the unit surface-to-camera vector, which the caller has already computed
 */
float2 toon_matcap_uv(const float3 n, const float3 v) {
    const float3 up = abs(v.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 right = normalize(cross(up, v));
    const float3 up_ortho = cross(v, right);
    return saturate(float2(dot(n, right), -dot(n, up_ortho)) * 0.5 + 0.5);
}

/// @brief relative luminance, with the article's own Rec. 709 weights (`MyCalculateLightStrength`)
///
/// IT IS USED FOR TWO DIFFERENT JOBS below and both are the article's: desaturating the dark side's albedo
/// toward its own brightness, and holding the ramp's colour mapping to a TINT rather than a brightness change.
static float toon_luminance(const float3 c) {
    return dot(c, float3(0.212672904, 0.715152204, 0.0721750036));
}

/// @brief linear -> display-space (sRGB) encode, the article's own `linear2sRGB` from `ZmdToonUtils.hlsl`
///
/// WHY THE SHADING STAGE NEEDS AN ENCODER AT ALL, when the whole point of working in linear is not to: the game's
/// shadow LUT is indexed by the ALBEDO AS THE ARTIST SAW IT - the display-space value - because that is the space
/// the palette was painted in. Reading it with a linear coordinate lands on the wrong texel everywhere except at
/// the two ends, which shows as a dark side that is roughly right in tone and wrong in hue. So the lookup's domain
/// is converted here and nowhere else; the COLOUR that comes back is still linear, because the lane is uploaded
/// sRGB and the sampler decodes it.
///
/// THE BRANCH IS THE sRGB CURVE'S OWN: a straight segment below 0.0031308 and a power above it. Written as the
/// article writes it, including its `abs` before the `log2` (a negative channel would otherwise be NaN rather than
/// a number nobody looks at).
static float3 toon_linear_to_srgb(const float3 colour) {
    const float3 linear_segment = colour * 12.9200001;
    const float3 curve_segment = exp2(log2(abs(colour)) * 0.416666657) * 1.05499995 - 0.0549999997;
    return float3(colour.r <= 0.00313080009 ? linear_segment.r : curve_segment.r,
                  colour.g <= 0.00313080009 ? linear_segment.g : curve_segment.g,
                  colour.b <= 0.00313080009 ? linear_segment.b : curve_segment.b);
}

/// THE HEAD FRAME the face SDF shades against (scene block slot 749). Layout matches `head_ubo` in
/// vulkan/primitive.cppm: four `vec4`s rather than three `vec3`s plus a position, so the host's struct and this one
/// agree without either side needing padding or `alignas`. Declared BY THIS STAGE, exactly as `LightUBO` and
/// `CameraUBO` are declared by the stages that read them - the heap accessor is a macro and expands here.
///
/// `center` IS STEP 7'S MEMBER AND IT IS A POSITION: the reference's `headCenter` attribute, which
/// `Recalculate normal` subtracts from the fragment's world position to build the sphere normal the FACE is shaded
/// by. The three axes above are DIRECTIONS and cannot stand in for it. **`.w` IS A FLAG**: `1.0` means the host
/// knows a centre (a head bone's world translation), `0.0` means it does not - and then the face arm keeps the
/// socket's own `interface[]` default (`sphereNormal_Strength = 0.0`) instead of building a normal about `(0,0,0)`,
/// which would be the world origin rather than a head.
struct HeadUBO {
    float4 front;  // the direction the face looks
    float4 right;  // its right
    float4 up;     // its up
    float4 center; // the head object's world position (the reference's `headCenter`); `.w` = "a centre is known"
};

/// THE TOON LIGHT RIG (scene block slot 751): the character stage's global numbers, declared BY THIS STAGE
/// exactly as `HeadUBO` is. THE LAYOUT MUST MATCH `deren::vulkan::toon_rig` - nine `float4`s, lane by lane named
/// here - and `runtime::set_toon_rig` is the only writer. A rig nobody writes is not a broken frame: the
/// application always publishes one, and the host's defaults are the article's own values.
struct ToonRigUBO {
    // x = `_DayStrength` (0 = the sun's direct term is gone and the head light carries the character, 1 = full
    // sun), y = `_OtherLightOffset`, z = `_OtherLightStrength`, w = `_OtherLightStrength_Offset`
    float4 day;
    // x = the head light's weight in the day-0 state, y = its (much smaller) weight in the day-1 state,
    // z = `_SelfAoShadowStrength`, w = the occlusion exponent the metallic/gloss map's B channel is raised to
    float4 other_light;
    // rgb = the head light's colour on the SHADOW side of a surface
    float4 other_colour;
    // rgb = the sun's colour on a surface's shadow side, a = `_BaseColorPow` (the albedo's own exponent)
    float4 main_dark;
    // x = `_RimLightArea`, y = `_RimLightStrength`, z = `_RimLightNoLxzStrength`,
    // w = `_RimLightDiffuseColorEffect`
    float4 rim;
    // rgb = `_SssColor`, a = `_SssPowStrength`
    float4 sss;
    // x = `_EnvLightStrength`, y = `_EnvRotation` in degrees, z = `_SpecularStrength`,
    // w = `_DiffuseBlendEffect`
    float4 env;
    // x = the backlight compensation's weight, y = `_NoFStrength`, z = `_NoFPowStrength`,
    // w = `_RampColorNoLStrength`
    float4 misc;
    // x = `[render] toon_shadow_softness`: 0 = the shipped single 3x3 PCF (BIT-IDENTICAL - the call site's
    // `calc_shadow_soft` returns `calc_shadow` unchanged at 0), 1..4 = the wider unrotated PCF grids
    // (2,2)/(3,3)/(5,4)/(8,3). y/z/w reserved.
    float4 shadow_softness;
};

/// @brief the rig's lanes as a VALUE, which a heap handle cannot be
///
/// THE HANDLE IS AN ADDRESS RANGE, NOT THE BLOCK, and Slang says so: `heap_at<ConstantBuffer<T>>(slot)` supports
/// MEMBER access (`.day.x`, which is what every other block in this renderer is read through) but not assignment
/// to a `T`. Copying lane by lane is the whole of this function, and it exists so that the shading body can hold
/// the rig once rather than re-reading a handle it does not name - MEASURED: `const ToonRigUBO rig =
/// toon_rig_at(...)` fails to compile with "expected an expression of type 'ToonRigUBO', got 'DescriptorHandle<...>'".
static ToonRigUBO toon_rig_value() {
    ToonRigUBO rig;
    rig.day = toon_rig_at(heap_slots_toon_rig).day;
    rig.other_light = toon_rig_at(heap_slots_toon_rig).other_light;
    rig.other_colour = toon_rig_at(heap_slots_toon_rig).other_colour;
    rig.main_dark = toon_rig_at(heap_slots_toon_rig).main_dark;
    rig.rim = toon_rig_at(heap_slots_toon_rig).rim;
    rig.sss = toon_rig_at(heap_slots_toon_rig).sss;
    rig.env = toon_rig_at(heap_slots_toon_rig).env;
    rig.misc = toon_rig_at(heap_slots_toon_rig).misc;
    rig.shadow_softness = toon_rig_at(heap_slots_toon_rig).shadow_softness;
    return rig;
}

/// @brief the reference's `EF_FACE_SDF_GOO_BASE` twin used to live here, and it is DELETED WITH ITS FORMULA
///
/// THIS FILE USED TO CARRY `character_sdf_sharp = 0.5` / `character_sdf_center_offset = 0.1` /
/// `character_sdf_base = 100000.0` and threshold the SDF with `1 / (1 + base^(-3*sharp*(x - center)))`. Those
/// three numbers are the GAME/RP's (`EF_FACE_SDF_GOO_*` appears nowhere in the article's sources - see
/// `deren-ab/face_shader_compare.md` §5-8), and the article's own face main writes a DIFFERENT formula at
/// `ZmdFaceToonMain.hlsl:168-172`: it normalises the SDF value into the light-angle WINDOW `[sdf_min, sdf_min +
/// sdf_width]`, smoothsteps that, and hands the RESULT to the ramp as its U coordinate. MEASURED, and it is why
/// this was replaced rather than tuned: the sigmoid's flip window is `+-0.06` in SDF units (about +-15 of 255),
/// while the asset's R channel is <=30 over 48% of its texels and the G channel is 255 over 66%, so the term
/// came out near-BINARY - the cheek's 3x3 luminance mottle read **7.18** against the unprobed baseline's
/// **5.37** (`deren-ab/p13_probe_report.md` §3-3, which also ablated the term to 3.49 by forcing it to 1.0).
/// The article's window is the whole SDF range for a light the face looks into, so the same term is a GRADIENT.

/**
 * @brief the FACE's shadow terminator, from its SDF lightmap - the article's `ZmdFaceToonMain.hlsl:168-173`
 *
 * WHY A FACE NEEDS A DIFFERENT MECHANISM FROM EVERY OTHER SURFACE, and it is the whole reason this lane exists:
 * a low-poly anime face is a handful of triangles whose normals are a poor description of its form - the cheeks
 * and the nose are painted into the texture rather than modelled - so shading it by `dot(N, L)` puts the
 * terminator wherever the tessellation happens to put it. The reference therefore shades the face as a 2D SHAPE:
 * a distance field authored per face, thresholded against an angle derived from the light's direction IN THE
 * HEAD'S OWN FRAME. That is why the light is projected onto the head plane below and the surface normal is
 * never used - the only place the reference touches `normalWS` in its whole SDF path is a debug escape hatch.
 *
 * THE HEAD FRAME IS READ, NOT ASSUMED. `head_frame_at(heap_slots_scene_head + heap_frame_slot)` below is written
 * once per frame by the host from the HEAD BONE's world matrix (the `-row1`/`row3` extraction `EfFaceGetHeadBasis`
 * performs, reproduced in gltf_loader's `head_basis_from_axes`), and it falls back to glTF's own basis -
 * `front = (0,0,1)`, `right = (-1,0,0)`, `up = (0,1,0)` - for a model whose skeleton has no usable head bone.
 * `zhuangfy_scalar.glb` is that case: 7 nodes, 0 skins, a static pose split by body part, so the fallback is not
 * an approximation for it - a static model's head does not turn, and a fixed basis and a bone matrix give the
 * same three vectors. A model that DOES have one publishes its own, so a skinned character's terminator follows
 * its head as it turns.
 *
 * THE FRONT AXIS POINTS WHERE THE FACE LOOKS, which is glTF's convention rather than the reference's: it is `+Z`
 * for every model here, measured on the face primitive's mean bind normal - `(+0.10 +0.31 +0.95)` on
 * `lizhiyan_toon.glb`, `(-0.18 +0.41 +0.89)` on `zhuangfy_toon.glb`, `(0 -0.38 +0.93)` on the game's own
 * `actor_zhuangfy.glb`. `EfFaceGetHeadBasis`'s `-row3` pointed the other way for ITS assets, and following it
 * here is not a cosmetic difference: it puts the sun 0.83 of the way round the head for a light the face is
 * looking almost straight into, and 94.1% of the texels the face's own UVs sample then read as shadow - the whole
 * face wears the family's shadow tint and this function shapes nothing. See `deren::gltf::head_basis` for the numbers.
 *
 * THE MIRROR IS THE PART WORTH STATING, because it looks like a trick and is the reference's design: the atlas
 * holds ONE face, and a light from the other side reads it MIRRORED (`1 - uv.x`). Measured on the real asset
 * (`T_actor_common_female_face_02_SDF`): the image correlates -0.89 with its own horizontal flip against +0.62
 * with its vertical flip, i.e. it is strongly antisymmetric about u = 0.5, which is exactly the structure that
 * makes the mirrored read land on a meaningful counterpart.
 *
 * THE LIGHT-ANGLE WINDOW IS THE ARTICLE'S OWN, COPIED LINE FOR LINE from `ZmdFaceToonMain.hlsl:159-173`, and it
 * is what replaced a fixed-slope sigmoid (see the note above the deleted constants). Four of the article's lines
 * collapse here because the port computes their inputs above in the caller: its `mainLightDir_xz_faceDir.z` is
 * this function's `dot(light_dir, head_front)`, and its `backLight * faceNoL_compensation` is the caller's
 * `ramp_curve * back_light` (algebraically identical - `-0.5*z*z + 0.5 == 0.5 - 0.5*z*z`), which is exactly what
 * the caller's `ramp_u` adds to `face_no_l`. Only `sdf_backLight` and the window have to be built here.
 *
 * THE WINDOW IS WHAT MAKES THE TERM CONTINUOUS: `faceNoL` is the light's forward component in the head's frame
 * mapped to [0,1], the window is `[max(0, 2*faceNoL - 1), min(1, 2*faceNoL)]`, and the SDF's R/G average is
 * stretched across it. With the light straight ahead (`faceNoL = 1`) the window is the WHOLE range `[0,1]`, so
 * every level of the distance field is a distinct terminator position instead of a hard flip.
 *
 * WHAT IT DOES NOT DO, said plainly: it never reads the surface normal - that is the point of the lane - so a
 * material with no SDF map, or one whose artist switched `_UseSDFLightmap` off, keeps the geometric terminator
 * instead. The caller's `sdf_lane != 0u` test is the whole of that decision, and both cases collapse to it.
 *
 * ONE DEGENERACY IS THE ARTICLE'S AND IS NOT PAPERED OVER HERE EITHER: at `faceNoL = 0` - a light edge-on to the
 * face's forward axis - its `sdf_width` is `min(1,0) - max(0,-1) = 0` and the window divides by zero. The article
 * does not guard it (`ZmdFaceToonMain.hlsl:169-170`); this port leaves the same division unguarded so the two
 * agree, and the weight is then 0 or 1 from the saturate rather than a value anybody reads.
 *
 * @param uv the surface's texture coordinate, which is the SDF atlas's coordinate too
 * @param lane the material's SDF heap lane; the caller must not call this with 0
 * @param light_ws the unit SURFACE-TO-LIGHT vector
 * @param head_face_dir_z the camera's forward axis measured along the head's own forward axis (the article's
 *        `cameraForward_faceDir.z` / `headFDotCamerF`), which the shadow mask on the caller's side shares too
 * @param sdf_back_light the article's `backLight` - its backlight compensation's weight, which the caller has
 *        already built from the same two directions `ramp_curve` uses
 * @param out_mask_uv the uv the caller must sample the FACE MASK (`_SDFMask`) at - the ORIGINAL coordinate, not
 *        the mirrored one this function samples the SDF at (see the note at the assignment)
 * @return the light/dark weight, 0 = shadow side, 1 = lit side
 */
float toon_sdf_mask(const float2 uv, const uint lane, const float3 light_ws, const float head_face_dir_z, const float sdf_back_light, out float2 out_mask_uv, out float out_sdf_z) {
    // THE HEAD FRAME, READ FROM THE PER-FRAME BLOCK rather than written as constants here. `HeadUBO` is declared
    // by this stage exactly as `LightUBO` and `CameraUBO` are declared by the stages that read them - a macro is
    // text, so the accessor above expands at this point and finds the declaration below.
    //
    // The values are the SAME as the reference's fallback constants for a model with no usable skeleton (the
    // application resolves it, and this repository's model has none), so moving them out of here changed no
    // pixel - which is the capture gate's job to confirm rather than this comment's to assert.
    const float3 head_front = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).front.xyz);
    const float3 head_right = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).right.xyz);
    const float3 head_up = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).up.xyz);
    // THE LIGHT IS PROJECTED ONTO THE HEAD PLANE. A light straight down the head's up axis has no stable
    // left/right side, and the reference bails out of that case (`return float4(1,1,0,1)`); this returns "lit",
    // because a magenta debug frame is not an option in a shipped stage and 1.0 is the value the ramp treats as
    // unshadowed.
    const float3 projected = light_ws - dot(light_ws, head_up) * head_up;
    const float projected_len_sq = dot(projected, projected);
    if (projected_len_sq < 1e-8) {
        out_mask_uv = uv;
        out_sdf_z = 0.0;
        return 1.0;
    }
    const float3 light_dir = projected * rsqrt(projected_len_sq);
    const float side = dot(light_dir, head_right);
    const float mirror = step(0.0, side);
    const float2 sdf_uv = float2(lerp(1.0 - uv.x, uv.x, mirror), uv.y);
    // HANDED BACK AS THE **ORIGINAL** uv, WHICH IS THE ARTICLE'S OWN COORDINATE FOR THIS TEXTURE: its face main
    // samples `_SdfRefineTex` at `i.uv0` (`ZmdFaceToonMain.hlsl:55`) and reserves the mirrored `float2(sdf_u,
    // sdf_v)` for `_SdfTex` alone (`:157`). THE PORT USED TO HAND BACK `sdf_uv` HERE, on the assumption that the
    // face mask is authored in the same mirrored space the SDF is - an assumption the article's source does not
    // support, and one that matters because the mask's `.y` (the neck/jaw ring) and `.w` (the side bands) are
    // HARD-EDGED rings: read at the mirrored coordinate, one side of the face receives the OTHER side's band and
    // the blend weight jumps across the seam.
    out_mask_uv = uv;
    const float4 sdf = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + lane), heap_sampler_texture, sdf_uv);
    // R AND G AVERAGED, which is what the reference's production path does. Its debug branches read the
    // channels separately (front light -> G, back light -> R); the goo path this ports averages them, and B and
    // A are never read outside a raw dump.
    const float average = saturate(0.5 * (sdf.r + sdf.g));
    // THE THIRD CHANNEL IS THE FACE'S OWN NORMAL TERM, mirrored with the same flag the sample was: the article turns
    // it back into a world-space normal in the head's frame (see the caller), which is how a face gets a rim that
    // follows its SHAPE rather than its tessellation.
    out_sdf_z = lerp(-(sdf.b * 2.0 - 1.0), sdf.b * 2.0 - 1.0, mirror);
    // ---- THE ARTICLE'S WINDOW, `ZmdFaceToonMain.hlsl:159-173` ----
    //
    // `light_dir_front_z` IS THE ARTICLE'S `mainLightDir_xz_faceDir.z` and `sdf_back_light` ITS `backLight`: the
    // light's own forward component in the head's frame, plus the compensation that keeps a backlit face from
    // collapsing onto the dark end of its own distance field (`backLight * (-0.5*z*z + 0.5)`) - the same term the
    // caller adds to this weight when it builds the ramp coordinate, which is why this is only the SHADOW side of
    // it: the article's `halfFaceNoL * ceil(halfFaceNoL)` is the backlit half, and `abs(-smooth - back) * 2 - 1`
    // folds to `smooth + back` for `smooth, back` in [0,1] - the caller's `ramp_u` is that sum.
    const float light_dir_front_z = dot(light_dir, head_front);
    const float sdf_face_no_l_compensation = -0.5 * light_dir_front_z * light_dir_front_z + 0.5;
    const float sdf_half_face_no_l = (light_dir_front_z + sdf_back_light * sdf_face_no_l_compensation) * 0.5;
    const float face_no_l_window = saturate(-(light_dir_front_z + sdf_back_light * sdf_face_no_l_compensation) * 0.5 + 0.5);
    const float sdf_min = max(0.0, 2.0 * face_no_l_window - 1.0);
    const float sdf_window = min(1.0, 2.0 * face_no_l_window) - sdf_min;
    // THE STRETCH AND THE SMOOTHSTEP ARE THE ARTICLE'S TWO LINES (`:170`, `:172`), and they are what turns a
    // 8-bit distance field into a gradient: the SDF's whole range is mapped onto the window the light angle opens,
    // so the terminator MOVES CONTINUOUSLY with the light instead of flipping where a fixed threshold crosses it.
    float sdf_smooth = saturate((average - sdf_min) / sdf_window);
    sdf_smooth = sdf_smooth * sdf_smooth * (3.0 - 2.0 * sdf_smooth);
    // AND IT IS HANDED BACK AS THE LIGHT TERM THE RAMP IS INDEXED BY, which is the article's `:173`: the shadow
    // side of the smoothstep is subtracted from the backlit half so the two halves of the face meet at one value.
    return abs(-sdf_smooth - sdf_half_face_no_l * ceil(sdf_half_face_no_l)) * 2.0 - 1.0;
}

/**
 * @brief the neutral ramp's HALF edge width, and the other half of a shared contract
 *
 * The ramp this stage reads when a material HAS one is not the game's and not any family's: it is a NEUTRAL
 * UNIT STEP baked by the host - zero through the shadow side, one through the lit side, a step at x = 0.5 with
 * this half width (see `bake_unit_ramp` in main.cpp). ONE redistributable asset therefore serves every
 * family, and where along it this material's terminator sits comes from the family's own `center` / `softness`
 * rather than from the pixels.
 *
 * THE REMAP BELOW INVERTS THE BAKE EXACTLY WHEN THESE TWO NUMBERS MATCH, and that is the whole point rather
 * than a nicety: with them equal, reading the texture at `0.5 + (gated - center) * (w / softness)` produces the
 * same value `smoothstep(center - softness, center + softness, gated)` would have, so the two branches of
 * `toon_diffuse` disagree only by the sampler's 8-bit quantization. Drift between the numbers breaks that
 * quietly - the ramp still looks like a ramp, it is just read at the wrong width - so
 * `tests/test_toon_material_sidecar.cpp` reads this file and main.cpp and fails when they differ.
 */
static const float character_ramp_half_width = 0.035;

/// @brief the SHADOW LUT's edge: how many 32x32 tiles across its 1024x32 strip
///
/// The reference's skin and cloth LUTs are `1024x32` strips holding 32 horizontal `32x32` tiles - a `32^3`
/// colour cube flattened - and this is that number. It is shared with the host's bake (`baked_lut_tiles` in
/// main.cpp) because the tiling below and the bake are INVERSE operations: a bake laid out for a different
/// tile count reads back as a different colour with no other symptom, so
/// `tests/test_toon_material_sidecar.cpp` compares the two.
static const float character_shadow_lut_tiles = 32.0;

/// @brief how many `vec4` lanes `deren::vulkan::toon_colour_lane` has - the stride of the colour table
///
/// THE SHADER ADDRESSES THE COLOUR TABLE FLAT (`material_index * this + lane`), so this number is HALF OF A
/// CONTRACT and the other half is `deren::vulkan::toon_colour_lane::count` in the host. A mismatch reads another
/// material's colour rather than failing, which is the silent-drift shape `tests/test_toon_material_sidecar.cpp`
/// exists to catch - it compares this against the enum for the same reason it compares the LUT's tile count.
/// LANE 3 IS THE OUTLINE'S (`.rgb` = `_OutlineTintColor`, `.w` = `_OutlineWidth`), and IT IS WHY THIS FILE IS
/// NOT THE ONLY READER: `pbr.slang`'s outline mesh entry reads the same lane for the width of the hull it
/// expands and `outline.slang` for the tint, each with its own copy of the stride for the same reason - a stage
/// cannot include this one without inheriting its entry point.
/// LANE 4 IS THE FIRST LANE FILLED FROM A SOURCE OTHER THAN THE SIDECAR - the asset's own `extras` block
/// (`_Specular`, in `.x`, with `-1` meaning "the asset states nothing"; see the read in `toon_diffuse`), and the
/// first of the two lanes whose neutral is not white for that reason.
/// LANE 5 IS THE SECOND, FROM THE SAME SOURCE AND WITH THE SAME SENTINEL (`_ParallaxScale`, in `.x` - the depth
/// the eye's parallax offset uses, see `character_eye_parallax_depth`): both lanes carry a per-material SCALAR
/// whose "absent" cannot be a number in the value's own range, and the read below resolves them together.
/// LANE 6 IS THE REWRITTEN CHAIN'S AND THIS STAGE DOES NOT READ IT (`_GooEyeBrightness`, the Goo iris group's
/// `Eyes brightness` / `Eyes HightLight brightness`, in `.x` / `.y` - see `shaders/goo_toon.slang` and
/// `deren::vulkan::toon_colour_lane::goo_eye_brightness`). It is counted here because the STRIDE is what this constant
/// is: a table whose stride disagreed with the host's would make every material read its neighbour's lanes, and
/// the stage that reads none of a lane still has to address around it.
/// LANES 7 AND 8 ARE THE SAME STAGE'S OBJECT-SPACE RIM (`_GooRimColour`, `.rgb` = the Goo base/hair group's
/// `Rim_Color`; `_GooRimScalars`, `.x` = `Rim_ColorStrength`, `.y` = `Rim_DirLightAtten`, `.z` =
/// `ToonfresnelPow`, `.w` = the HAIR's `Use Rimlimitation?`), and THIS STAGE DOES NOT READ THEM EITHER: the rim
/// they carry is the rewritten chain's, and the stage that draws it reads them where it draws it. They are
/// counted here for lane 6's reason and no other.
/// LANE 9 IS THAT CHAIN'S SCREEN-SPACE RIM WIDTHS (`_GooRimWidths`, `.x` = `Rim_width_X`, `.y` = `Rim_width_Y`),
/// READ BY THE FULLSCREEN RIM STAGE (`shaders/goo_rim.slang`) - not by this one, for lane 6's reason again.
/// LANES 10..15 ARE STEP 4'S, and only ONE of them is a texture: lane 10 is `_GooBaseRamp`, the `_RD` image the
/// Goo reference's `RampSelect` resolves to for this material, which THIS stage reads (in the guarded arm below)
/// through `toon_lanes2_at(...).z`. Lanes 11..15 are that step's five colour lanes and are read by the same
/// guarded arm; the rest of this file never sees them, and the guard is what keeps the UNGUARDED arm (`outline.slang`
/// and the old pipeline) byte-for-byte what it was.
/// LANES 16..18 ARE STEP 5'S THREE, read by the SAME guarded arm as step 4's five because they are the same three
/// terms' inputs: `_GooSpecularFGD` (`.x` = `specularFGD Strength`), `_GooLightColor` (`.rgb` = the per-material
/// `dirLight_lightColor`, the light colour of BOTH direct terms) and `_GooAmbientTint` (`.rgb` =
/// `AmbientLightColorTint`, what the reference's IBL diffuse multiplies the engine's probe irradiance by).
/// LANE 19 IS STEP 5'S FOURTH, `_GooSpecularColor` (the direct specular's own HDR per-material multiplier).
/// LANES 20..23 ARE STEP 7'S FOUR, read by the FACE arm and by nothing else: `_GooFaceScalarsA` (the chin
/// `SigmoidSharp` pair / `sphereNormal_Strength` / `SmoothnessMax`), `_GooFaceScalarsB` (the two emission
/// brightnesses / `Front R Pow` / `Front R Smo`), `_GooFaceNoseShadow` (a MIX's A side, neutral BLACK) and
/// `_GooFaceFrontR` (`Front transparent red`'s `sideColor`, neutral BLACK). The FACE container's other stated
/// numbers already had carriers by SAME SOCKET NAME - `BaseColor` on 10, the four `SigmoidSharp`/`MetallicMax`
/// scalars on 11/12, the three light colours on 17/18/19 and `Rim_Color` on 7 - which is the audit the step's own
/// spec asked for before minting a lane.
/// LANE 24 IS STEP 8'S, AND IT IS THE FIRST LANE THIS STAGE'S OWN ENTRY POINT USES A NEW WAY RATHER THAN READS: it
/// carries `_GooNormalStrength`, the `DecodeNormal` group's `NormalStrength` - the strength the reference's normal
/// decode applies to the map's `xy` before it becomes a tangent-space normal. The decode itself is
/// `goo_toon_shading_normal` below, which the two entry points call in place of `normalize(s.normal)` (see its note
/// for the formula and for why the G-buffer's own normal is deliberately left alone). The lane's `.x` is the number;
/// its neutral is `-1000` on every component, so a material the port carried no value for keeps the normal it had.
/// LANE 25 IS STEP 10'S, `_GooAnisoGate`: the THREE `PBRToonBase` group inputs that decide the direct specular's
/// arm - `.x = Use anisotropy?` (the factor of `混合.016`, which chooses between `DV_SmithJointGGX_Aniso.original *
/// F_Schlick` and the `混合.017` masked side), `.y = Anisotropic mask` (`混合.017`'s second operand) and
/// `.z = Use Toonaniso?` (it switches `混合.020`, and it is the THIRD switch the port reads: the lobe's own
/// selector, whose lane-26 operand arrived in step 12 - see the block at the arm). `.w` is reserved. The neutral is `(0, 0, 0, 0)`, which is the reference's own `interface[]` default
/// for all three sockets - so an unstated material reads the arm the graph itself would take, and the lane is also
/// what finally replaced the two hardcoded `Use anisotropy?` / `Anisotropic mask` defaults this stage carried
/// until step 10 (both deleted with the switch-over, rather than left as a second source for the same switch).
/// LANE 26 IS STEP 12'S, `_GooAnisoRough`: the two `PBRToonBase` roughnesses the ANISOTROPIC LOBE is built from -
/// `.x = Aniso_SmoothnessMaxT`, `.y = Aniso_SmoothnessMaxB`, from which the stage's `goo_aniso_roughness_t` /
/// `goo_aniso_roughness_b` are `(1 - socket)^2`. `.z` / `.w` are reserved. The neutral is `(0, 0, 0, 0)`, the
/// reference's own `interface[20]` / `[21]` default - so a material that states no row gets `rT = rB = 1`, which is
/// the lobe the graph itself describes for a caller that states nothing, and NOT the mirror an unoverridden
/// `glm::vec4(1.0f)` would give.
/// LANES 27 AND 28 ARE STEP 13'S, `_GooRSScalars` AND `_GooRSTint`, and they carry mechanism table #14 (`RS EFF`) -
/// the reference's `_M ⊙ RS ColorTint` pass. They are the stage's OWN lanes in the sense that matters: neither the
/// OLD chain (`character_forward.slang`'s `main`) nor any other stage reads them, and only
/// `shaders/goo_toon.slang`'s non-eye branch does (see the STEP 13 block there). `.x / .y / .z` of the first are
/// `Use RS_Eff?` / `RS Multiply Value` / `RS Model`, its `.w` is reserved; `.rgb` of the second is `RS ColorTint`
/// and its `.w` is the `_M` smoothstep's `max`, which is NOT part of the tint. Both neutrals are `(0, 0, 0, 0)`,
/// and here the zero is the switch rather than a group default - so a material with no row reads `Use = 0` and the
/// branch is off. `min` of that smoothstep is `0` on every material measured and is not carried at all, and a
/// stated `.w <= 0` is read as `1.0` so that the neutral cannot divide by zero (spec U7/U11).
/// THE STRIDE CHANGED HERE AND IN THREE OTHER FILES, WHICH IS THE PART THAT IS EASY TO MISS: this constant,
/// `goo_rim.slang`'s `goo_rim_colour_lanes`, `pbr.slang`'s `pbr_toon_colour_lanes` and (through the `#include` at
/// the top of it) `outline.slang` all address the SAME table, so a stale copy in any one of them reads another
/// material's lanes - a plausible wrong picture rather than an error. `tests/test_toon_material_sidecar.cpp` and
/// `tests/test_goo_toon_math.cpp` pin all four against `deren::vulkan::toon_colour_lane::count`.
static const uint character_toon_colour_lanes = 30u;

/// @brief how many `uvec4` BLOCKS of sidecar lanes the table holds - the stride of lanes 4 and up
///
/// LANES 4..7 LIVE IN BLOCK 0, 8..11 IN BLOCK 1 AND 12..15 IN BLOCK 2, so the shader addresses block 1 at
/// `material * 3 + 1` and block 2 at `material * 3 + 2` (see `toon_lanes2_at` / `toon_lanes3_at`). This number is
/// half of that contract and `deren::vulkan::toon_lane_blocks` is the other half - WHICH STEP 7 RAISED FROM 2 TO 3,
/// because the FACE's three masks did not fit the second block. The three accessors' literals and this constant
/// have to move together; a stale one reads another material's block, silently.
static const uint character_toon_lane_blocks = 3u;

// ============================================================================================================
// STEP 4: THE REFERENCE'S DIRECT-DIFFUSE TERM, AND THE TWO FACTORS IT MULTIPLIES THE PRE-RIM COLOUR BY
// ============================================================================================================
//
// WHAT THIS BLOCK IS, in the reference's own nodes (`Arknights: Endfield_PBRToonBase`, spec
// `deren-ab/goo_step4_diffuse_spec.md` §2/§3, and §8 for the arithmetic):
//
//     混合.012 = 混合.004 ⊙ 混合.006                                  # the pre-rim colour
//     混合.006 = ToonFresnel colour = lerp(fresnelOutsideColor, fresnelInsideColor,
//                                          smoothstep(SMO_L, SMO_H, pow(clamp(N·V), ToonfresnelPow)))
//     混合.004 = 混合.011 ⊙ clamp(smoothstep(GlobalShadowBrightnessAdjustment, 1.0, RampSelect.RampAlpha))
//     混合.011 = directLighting_diffuse + direct_specular + IBL_diffuse + IBL_specular
//     directLighting_diffuse = (1/π) ⊙ RampColor ⊙ (albedo ⊙ (1−metallic)) ⊙ dirLight_lightColor ⊙ directOcclusion
//     RampColor = RampSelect(RampIndex).sample((min(CastShadowSigmoid, HalfLambertSigmoid), 0.5, 0.0))
//     SigmoidSharp(x, center, sharp) = 1/(1 + 100000^(-3·sharp·(x−center)))          # no clamp anywhere
//
// ---- WHAT THIS STEP REPLACES, AND WHAT IT DELIBERATELY LEAVES ON THE PORT'S OWN PATH ----
//
// `directLighting_diffuse` IS REPLACED. The port's three-layer diffuse (`brdf_low_light` / `brdf_final` /
// `direct`, and the `min_shadow_effect` gate inside them) goes away FOR THE FOUR FAMILIES THIS ARM DRAWS, and
// what takes its place is the expression above. THE OTHER THREE TERMS OF `混合.011` ARE NOT THIS STEP'S:
// `direct_specular`, `IBL_diffuse` and `IBL_specular` STAY THE PORT'S OWN (`main_specular`, `ambient`,
// `indir_light_specular`), which is what the next steps are for. The composition below therefore reads
//
//     new colour = (goo_direct_diffuse + port_direct_specular + port_ambient + port_ibl_specular)
//                  ⊙ gsba_gate ⊙ toon_fresnel
//
// and THAT IS A HYBRID rather than the reference's `混合.012`: four terms of the sum are the reference's
// arrangement (one of them the reference's own expression, three of them the port's standing in for terms the
// reference computes elsewhere), and the two factors are the reference's exactly. It must not be mistaken for
// "the reference's shading model is now implemented" - the spec's §10.2 lists the port terms that would
// DOUBLE-COUNT against the substituted diffuse, and each one's disposition is recorded where it is touched.
//
// ---- WHY `1/π` IS IMPLEMENTED HERE (the spec's §9-U8), AND WHY IT COEXISTS WITH `:1495-1499` ----
//
// The two look like a contradiction and are not, because THEY APPLY TO TWO DIFFERENT LIGHTS. The port's own
// `direct` multiplies `brdf_day` by `main_light_final`, and that light is the ARTICLE's, built the article's way
// (`:1425-1481`): `main_light_intensity` is `sun_intensity` standing in for `_MainLightIntensity`, i.e. a colour
// of ORDER 1, and the file's own note at `:1495-1499` records that the `/π` that used to be there was a bridge
// for feeding the ENGINE's radiance (`7.5 * sun_intensity`) into a chain tuned for that colour. THE REFERENCE'S
// `directLighting_diffuse` MULTIPLIES A DIFFERENT LIGHT: `dirLight_lightColor`, which is a PER-MATERIAL
// CONSTANT (`[1.0, 0.9580, 0.9580]` on `body_01`) and is not a radiance at all. The reference's own graph states
// the `1/π` explicitly (`directLighting_diffuse :: 值(明度).Value = 0.31830987334251404`, read straight out of
// the `.blend` in `deren-ab/goo_screenspace_info.md` §Q5), so the reference's expression is implemented VERBATIM
// and the article's path keeps its own convention untouched. U8 CANNOT BE SETTLED WITHOUT A RENDER - whether the
// two lights are on the same scale is exactly the question - so this step takes the reference's explicit number
// and RECORDS that the hybrid's absolute level is the thing to check against the reference frame.
//
// ---- THE RAMPS'S COLOUR SPACE (the spec's §5.3) ----
//
// The reference's seven `_RD` images are all `colorspace = 'sRGB'`, so Blender LINEARIZES every texel before its
// node graph sees it, and `directLighting_diffuse`'s `RampColor` is a LINEAR colour. The lane this port samples
// is uploaded `VK_FORMAT_R8G8B8A8_SRGB` (`runtime.constructor.cppm`) so the sampler does the same conversion -
// WHICH IS WHY NOTHING HERE CONVERTS IT AGAIN. Had the lane been UNORM, the texel would be ~2.2 gamma too bright.

/// `directLighting_diffuse :: 值(明度).Value` - the reference's own float for `1/π`
/// (`0.31830987334251404`, which is `float32(1/π)` rather than the double `0.31830988618379069`; see
/// `deren-ab/goo_screenspace_info.md` §Q5, where the value was read from the `.blend` itself).
/// THE SENTINEL EVERY ONE OF STEP 4'S LANES USES, and it is NOT `< 0` - which is what this arm was first written
/// with, and what the lane's own note argued for on the grounds that `0.0` is a value the reference really states.
/// THE ARGUMENT WAS RIGHT AND THE NUMBER WAS WRONG: two of the six sockets are LEGITIMATELY NEGATIVE in the
/// reference's own assets - `CastShadow_center` is `-0.10000000149011612` on `M_actor_laevat_body_01/02` and
/// `GlobalShadowBrightnessAdjustment` is `-1.7999999523162842` on the cloth (spec §4.1/§4.3) - so a `< 0` test
/// reads an AUTHORED value as "not stated" and silently substitutes the group's default. That is the same class of
/// failure as a stale lane stride: the frame stays plausible. `-1000` is outside every value any of these sockets
/// states in either dump, so a stated value - negative or not - passes the test and only the lane's own neutral
/// fails it.
static const float goo_lane_absent = -1000.0;
/// the test's other half, spelled once: a component is STATED when it is strictly greater than this.
static const float goo_lane_absent_threshold = -999.0;
/// THE TWO ABSENCE CONTRACTS, NAMED. They are NOT the same comparison and the difference is not cosmetic:
/// `-1.0` means "not stated" for step 5's four lanes (whose sockets are never negative in the reference's
/// assets - see the neutral table in `main.cpp`, where their neutral is `(-1, 1, 1, 1)`) and means "an authored
/// `-1.0`" for the four lanes whose sentinel is `-1000.0`. Debt (u) was this difference going unread: lane 16's
/// contract is `< 0` (`goo_specular_fgd_strength_default` answers `-1.0`), but its one call site tested
/// `> goo_lane_absent_threshold`, so the neutral `-1.0` passed as a STATED strength and the reference's own
/// group default `1.0` was never reached. Both predicates stay named so the next lane has to pick one.
static bool goo_lane_stated(const float value) { return value >= 0.0; }
static bool goo_lane_stated_sentinel(const float value) { return value > goo_lane_absent_threshold; }
static const float goo_inverse_pi = 0.31830987334251404;

/// `SigmoidSharp :: 运算.002.Value` - the base of the power in the reference's own sigmoid.
static const float goo_sigmoid_sharp_base = 100000.0;
/// `SigmoidSharp :: 运算.003.Value` - the multiplier applied to `sharp` before the power (`-3.0 * sharp`).
static const float goo_sigmoid_sharp_gain = -3.0;

/// the reference's three `RampSelect :: 运算.00{4,5,8}.Value_001` boundaries, in the order the selector tests them
static const float goo_ramp_index_body = 0.9900000095367432;
static const float goo_ramp_index_cloth1 = 2.0;
static const float goo_ramp_index_tplk = 2.990000009536743;

/// the `PBRToonBase :: 组输入.*` defaults the per-material sentinels fall back to, read out of
/// `gooblender/nodes.json`'s `interface[]` rather than chosen here (spec §4.1).
static const float goo_remap_center_default = 0.5699999928474426;
static const float goo_remap_sharp_default = 0.1599999964237213;
static const float goo_castshadow_center_default = 0.0;
static const float goo_castshadow_sharp_default = 0.0;
static const float goo_gsba_default = 0.0;
static const float goo_metallic_max_default = 1.0;
/// `ToonfresnelPow`'s group default, for a material that states none (`::- ... :: 组输入.ToonfresnelPow =
/// 2.0`; the same number step 2 recorded as the rim group's interface default for this socket).
static const float goo_rim_fresnel_pow_default = 2.0;
static const float goo_fresnel_smo_l_default = 0.0;
static const float goo_fresnel_smo_h_default = 1.0;

// ============================================================================================================
// STEP 5: THE REFERENCE'S FGD GROUP, ITS DIRECT SPECULAR, AND ITS TWO IBL TERMS
// ============================================================================================================
//
// EVERY NUMBER BELOW IS A SOCKET OR A LITERAL IN `gooblender/nodes.json`, named by the node it belongs to
// (spec `deren-ab/goo_step5_specular_spec.md`, which is the authority for this step). They are spelled here rather
// than inline because `tests/test_goo_toon_math.cpp` pins each spelling against the reference's own value - a
// re-tuning of one of them has to fail a test rather than quietly move a highlight.

/// `Remap01ToHalfTexelCoord :: 值(明度).Value` - the LUT's resolution, and the frame inside the reference's own
/// graph is even NAMED `FGDTEXTURE_RESOLUTION`. It appears in `coordLUT = coord*(1 - 1/N) + (1/N)*1.0`, which is
/// the half-texel remap that keeps a `[0,1]` shading parameter inside the image's texel CENTRES.
static const float goo_fgd_resolution = 64.0;
/// `GetPreIntegratedFGDGGXAndDisneyDiffuse :: 运算.Value_001`, the ADD's second operand: `diffuseFGD = LUT.B + 0.5`.
///
/// **THE FLAGS SAY `+ 0.5`, AND THIS WAS ONCE `0.0`.** The ADD is `Value <- 分离 XYZ.001.Z (enabled = true)`,
/// `Value_001 = 0.5 (enabled = true, unlinked)`, `Value_002 = 0.5 (enabled = FALSE, unlinked)`, and the rule this
/// step's LUT coordinates are built on - an unlinked socket PARTICIPATES when `enabled = true` and does NOT when
/// `enabled = false` (see `goo_fgd_coord_bias` below) - makes the ADD's surviving operands `LUT.B` and one `0.5`.
/// The first version of this step followed a parent ruling of `0.0`; that ruling was withdrawn.
///
/// ⚠ THE ARGUMENT THAT MADE `0.0` LOOK RIGHT, KEPT HERE SO A READER CAN WEIGH IT: the LUT's own measured range is
/// `B ∈ [0,1]` (mean 0.480, monotone corner-to-corner: `B+0.5 ∈ [0.5, 1.5]` is a diffuse term brighter than the
/// albedo it multiplies and one that cannot reach 0 at grazing angles). That is a PRIOR about what a finished
/// Disney diffuse FGD term "should" be - it is not evidence about the author's graph, and this author's style does
/// contain exactly this kind of ambient floor (the same author's `GlobalShadowBrightnessAdjustment` lift is step
/// 4). The two readings differ by a constant 0.5 on `IBL_diffuse` and nothing else.
///
/// IT IS ONE NAMED CONSTANT AND THE EXPERIMENT IS ONE LINE: `0.5` is what the node says, `0.0` is the prior, and
/// flipping this line is the whole of the choice. `tests/test_goo_toon_math.cpp` pins BOTH the value and the fact
/// that the `+ 0.5` is in the expression.
static const float goo_fgd_diffuse_offset = 0.5;
/// `ComputeFresnel0 :: 组输入.dielectricF0` on the Base instance - the dielectric end of
/// `fresnel0 = lerp(this, BaseColor, metallic)`, stored as the float32 `0.07999999821186066`.
static const float goo_fgd_dielectric_f0 = 0.07999999821186066;
/// `运算 :: Value_001` (the `clampedNdotV` MAXIMUM's floor), stored as `9.999999747378752e-05`. NOT `1e-4`: the
/// difference is one ulp and this project records stored values rather than their decimal approximations.
static const float goo_fgd_ndotv_floor = 9.999999747378752e-05;
/// `DV_SmithJointGGX_Aniso :: 运算.013.Value` - the reference's own `1/π` (`0.3183099925518036`), which
/// `运算.013` then multiplies by `运算.001 = 1/2` before `运算.015` multiplies that into `D * Gv`. So the
/// group's `original` carries `1/(2π)`, NOT this file's `goo_inverse_pi` - and the two are different numbers
/// (`0.31830987334251404` against `0.3183099925518036`), so the group's own spelling is what is used.
static const float goo_dv_half_inverse_pi = 0.3183099925518036 * 0.5;
/// @brief the ANISOTROPIC LOBE's own `1/(2π)` - `运算.013` of `DV_SmithJointGGX_Aniso`'s second instance
///
/// IT IS A SECOND CONSTANT AND NOT A REUSE OF THE ONE ABOVE, because the two are DIFFERENT float32s and they come
/// from two different subtrees of the same group: `0.3183099925518036 * 0.5` above is `混合.016`'s A arm (the
/// isotropic `original`), while the lobe multiplies by `0.31830987334251404 * 0.5` = `0.15915493667125702`
/// (bit pattern `0x3E22F983`, against the other's `0x3E22F987`). Step 9's plan note warns about exactly this
/// mix-up; the two values are asserted separately in `tests/test_goo_toon_math.cpp`.
static const float goo_aniso_half_inverse_pi = 0.31830987334251404 * 0.5;
/// @brief `运算.017`'s MAXIMUM literal on the lobe's denominator, VERBATIM from the dump
///
/// `0x3A83126F`, i.e. the float32 nearest `1.0e-3` - the same literal the isotropic `original` uses at the arm
/// above, spelled separately because the two nodes are separate. It is `max(den, this)` and NOT a clamp on `s2`.
static const float goo_aniso_denominator_floor = 0.0010000000474974513;

/// `PBRToonBase :: 组输入.specularFGD Strength`'s `interface[]` default, for a material whose container has no
/// such socket at all (the Face and Hair containers do not - spec §5.1).
static const float goo_specular_fgd_strength_default = 1.0;

// ============================================================================================================
// STEP 6: THE REFERENCE'S DARK-SIDE DESATURATION - `DeSaturation` + `色相/饱和度/明度`
// ============================================================================================================
//
// WHERE IT SITS, WHICH IS THE ONE THING THAT DECIDES THE WHOLE STEP: **AFTER THE RIM, ON THE FINISHED PIXEL.**
// The reference's chain is
//
//     混合.012 = (spec_dir ⊙ (1 + f0·energyComp) + diff_dir + diff_ibl + spec_ibl) ⊙ gate ⊙ toon_fres   # step 5
//     混合.019 = 混合.012 + rim                                        # step 2/3, ITS OWN STAGE here
//     混合.026 = 混合.019 + emission                                   # step 5's `Emission` block
//     混合.029 = LIGHTEN(混合.026, RS EFF) ; 混合.030 = MIX(Use RS_Eff?, 混合.026, 混合.029)
//     色相/饱和度/明度 = HSV(Hue = 0.5, Saturation = clamp(...,0,1), Value = 1.0, Fac = 1.0, Color = 混合.030)
//     群组.020(ShaderOutput).着色结果 <- 色相/饱和度/明度.Color -> 自发光(发射) (Strength = 1.1) -> 混合着色器
//
// Every link of that is `gooblender/nodes.json`: `::- ... :: 色相/饱和度/明度.Color -> 群组.020.Input_3`,
// `::- ShaderOutput :: 组输入.着色结果 -> 自发光(发射).Color`, `::- ShaderOutput :: 混合着色器.Shader ->
// 组输出.Output_1`. There is NO consumer of `色相/饱和度/明度` inside `混合.012`'s own subtree and NO path from it
// back to `混合.011` - so this term is applied AFTER the rim, which is exactly the trap the step's own brief names.
//
// WHY THAT MATTERS RATHER THAN BEING A DETAIL: `DeSaturation` is fed `RampSelect.RampColor` (the DARK ramp), so its
// result is small in the LIT area and large in the SHADOW - and `saturation = clamp(luma + attenuation, 0, 1)` is
// therefore near 1 in shadow and near the material's own attenuation in the light. Applied BEFORE the rim it would
// have desaturated the rim's own colour too; applied after, the rim keeps its own saturation, which is what the
// reference's screenshots show (a saturated contour light on a nearly grey shadow side).
//
// ---- WHAT IT IS ----
//
// `DeSaturation` (11 nodes: two `NodeFrame`, `组输入/组输出`, two `NodeReroute`, one `CombineXYZ` and FOUR
// `ShaderNodeVectorMath`) is a `lerp` TOWARD luma, NOT a Blender HSV node:
//
//     矢量运算.004 = DOT_PRODUCT(转接点.159(组输入.Color), 合并 XYZ.002 = (0.21267299354076385, 0.7151520252227783,
//                                                                        0.07217500358819962))    -> .Value
//     矢量运算.005 = SUBTRACT(组输入.Color, 矢量运算.004.Value)          # the broadcast is Blender's
//     矢量运算.006 = MULTIPLY(转接点.160(组输入.DeSaturation), 矢量运算.005.Vector)
//     矢量运算.007 = ADD(矢量运算.006.Vector, 矢量运算.004.Value)  -> 组输出.Vector
//     ⇒  out = luma + DeSaturation * (Color - luma)   =  lerp(luma, Color, DeSaturation)
//
// `色相/饱和度/明度` (`ShaderNodeHueSaturation`) is the Blender HSV node, and its own renderer GLSL is
// `hsv[1] = clamp(hsv[1] * sat, 0, 1); hsv[2] = hsv[2] * value; out = mix(col, out, fac)`
// (`gpu_shader_material_hue_sat_val.glsl`, cited in `deren-ab/goo_step6_result.md`). With `Hue = 0.5` (`fract(h+0.5)`
// = h), `Value = 1.0` and `Fac = 1.0`, THAT IS `lerp(luma(colour).xxx, colour, sat)` - THE SAME FORM. So the two
// nodes compose to one line of arithmetic, and the file's `goo_hsv_desaturate` below is that line.

/// `DeSaturation :: 合并 XYZ.002`'s three literals, the luma weights, VERBATIM and UNROUNDED:
/// `::- DeSaturation :: 合并 XYZ.002.X/.Y/.Z = 0.21267299354076385 / 0.7151520252227783 / 0.07217500358819962`.
static const float goo_desaturation_luma_r = 0.21267299354076385;
static const float goo_desaturation_luma_g = 0.7151520252227783;
static const float goo_desaturation_luma_b = 0.07217500358819962;

/// `PBRToonBase :: 组输入.Color desaturation in shaded areas attenuation`'s `interface[]` default - **`0.0`**, and
/// it is the reference's own number rather than a neutral chosen here (`::- Arknights: Endfield_PBRToonBase ::
/// interface[]`, `Hair`'s is `0.0` too; `Face`'s is `1.0`). `saturation = clamp(luma + 0.0, 0, 1) = luma`, so a
/// material that states nothing desaturates TO GREY in its shadows, which is the author's default and not this
/// port's invention. It is what a material with a lane but no stated `.y` gets.
static const float goo_desaturation_default = 0.0;

/// THE IDENTITY VALUE OF `goo_hsv_desaturate`, and it is **NOT** the group default above.
///
/// THIS IS THE STEP-6 SCOPE BOUNDARY AND IT IS ONE LINE: `saturation = 1` is `lerp(luma, colour, 1) = colour`, so a
/// material this arm does not take - HAIR, FACE, the eye, the outline stage's call, and EVERY material in a
/// `goo_toon = false` frame - passes through `main`'s desaturation line unchanged. The group's own default for the
/// socket is `0.0` (`goo_desaturation_default`), which is `saturation = luma` and therefore a REAL desaturation:
/// writing that into the neutral would apply the reference's term to the hair and the face, which is the scope
/// this step was given and did not take. The two numbers are consequently different on purpose and each has its
/// own name.
static const float goo_desaturation_neutral = 1.0;

/// @brief `DeSaturation`: `lerp(luma(colour), colour, desaturation)` with the reference's own weights.
///
/// THE FORM IS THE GRAPH'S, AND SO IS THE BROADCAST: `矢量运算.004.Value` is a FLOAT and `矢量运算.005` subtracts
/// it from the RGBA `组输入.Color`, which in Blender broadcasts the scalar to all three channels. The alpha is
/// carried through untouched (`矢量运算.007`'s ADD adds the same scalar to the RGBA's alpha, i.e. adds a 4th
/// channel that no consumer reads - `组输出.Vector`'s only consumer takes `.Color`).
static float3 goo_desaturation_luma_colour(const float3 colour) {
    const float luma = dot(colour, float3(goo_desaturation_luma_r, goo_desaturation_luma_g, goo_desaturation_luma_b));
    return luma.xxx;
}

/// @brief `色相/饱和度/明度` at the reference's own settings (`Hue = 0.5`, `Value = 1.0`, `Fac = 1.0`).
///
/// THE IDENTITY THAT MAKES THIS EXACT RATHER THAN A LOOKALIKE, written out so the next reader does not have to
/// re-derive it: for `sat ∈ [0,1]`, `lerp(luma.xxx, colour, sat)` is per channel `luma + sat*(c - luma)`, and
/// Blender's own GLSL reaches the same numbers through `rgb_to_hsv` / `hsv_to_rgb`. The three anchors:
/// `sat = 0` gives luma (grey), `sat = 1` gives the colour untouched, `sat = 0.5` gives the midpoint - which is
/// the whole of what the HSV round trip does when hue and value are the identity. IT IS `sat`, NOT
/// `1 - sat`: the reference's `Saturation` socket is the HSV node's saturation multiplier, and `钳制.004`'s
/// `[0,1]` range is the node's own documented range (its Saturation input is `min = 0.0`, `max = 2.0`).
static float3 goo_hsv_desaturate(const float3 colour, const float saturation) {
    return lerp(goo_desaturation_luma_colour(colour), colour, saturate(saturation));
}

// ============================================================================================================
// STEP 7: THE FACE CONTAINER, `Arknights: Endfield_PBRToonBaseFace`
// ============================================================================================================
//
// WHAT IT IS, in the reference's own nodes (spec `deren-ab/goo_step7_face_spec.md`, re-read node by node against
// `gooblender/nodes.json` for this step - and the spec's §7.3 and §2.4 readings of two fields were WRONG, see the
// notes on `goo_face_angle_thresholds` and on the brightness switch in `toon_diffuse`'s face arm):
//
//     AngleThreshold, Flip = calculateAngel(LightDirection, headUp, headRight, headForward)   # 值(明度) = -1.0
//     N  = Recalculate normal(posWS, headCenter, NormalWS, sphereNormal_Strength = 1, ChinMask = cm_M.G)
//     cm = 图像纹理.002 (T_actor_..._cm_M.png.001)      # sampled at THIS FRAGMENT's uv, not at texel (0,0)
//     SDF= 图像纹理.001 (T_actor_..._SDF.png)           # sampled at (mirror(uv.x), uv.y), read as (R+G)/2
//     ramp_u = min( sigmoid(lerp(1, CastShadows, cm.G), CastShadow_center, _sharp),
//                   lerp( min(lerp(1, 1.1, 运算.029), min(sigmoid(SDF, AngleThreshold + SDF_center, SDF_sharp),
//                                                          CastShadows)),
//                         sigmoid(0.5*N·L + 0.5, chin_center, chin_sharp), cm.G ) )
//     ramp   = T_actor_common_face_01_RD(ramp_u, 0.5)
//     混合.011 = directLighting_diffuse + directLighting_specular + IBL_diffuse + IBL_specular
//     混合.004 = 混合.011 ⊙ clamp(1, 0, SmoothStep(GSBA, 1, ramp.a))
//     混合.014 = clamp(SmoothStep(0.8,1,dot(headForward,V)) · (cm.A · mirror(uv.x)) · (1 − AngleThreshold),0,1) ⊙ Rim_Color
//     混合.024 = ((混合.014 + 混合.004) ⊙ mix(_D.A, nose_shadow_Color, white)) + (Lips ⊙ hl_M.r) ⊙ max(ramp_u, 0.4)
//     自发光(发射) = (色相/饱和度/明度(混合.024, Saturation = clamp(luma(ramp.rgb) + 0.9, 0, 1)),
//                     Strength = CsumtMask.G > 0.5 ? Eyes white Final brightness : Face Final brightness)
//
// ---- WHY THIS IS ITS OWN ARM AND NOT A BRANCH INSIDE STEP 4's ----
//
// Because THE COMPOSITION IS DIFFERENT, not merely its inputs. Step 4's arm replaces ONE of `混合.011`'s four terms
// and hands two factors back to the composition at `toon_colour`; the FACE's arm owns ALL FOUR terms, its own
// pre-rim term (which is `混合.014`, a MULTIPLICATIVE rim rather than the reference's additive one), two terms
// after the rim, its own desaturation and its own emission strength - i.e. the whole pixel. It therefore RETURNS,
// and the return is the mechanism that guards the article's chain off: `toon_colour`, `eye_highlight`, the old
// `rim`, `main_specular`, `ambient` and `indir_light_specular` are all below it and are never reached for a FACE
// fragment. THE ALTERNATIVE - widening step 4's `if` and branching inside - would have put five conditionals inside
// a block seven materials are already verified against, which is the one thing this step may not do.

/// `混合.022`'s second input when the screen-space "shadow proxy" says there is NO occluder.
///
/// `运算.025 = MAX(运算.024, 0)` and `运算.024 = 运算.023 + 0.1 = (1 − 运算.020) + 0.1`, so with `运算.020 = 0` the
/// proxy's answer is `1.1` and `混合.022 = lerp(1.0, 1.1, f) ≥ 1` - i.e. the MINIMUM that follows it never binds and
/// `运算.022 = 运算.014`. THE OTHER BRANCH (`运算.020 = 1` ⇒ `0.1`) CANNOT BE COMPUTED BY THIS STAGE, and that is a
/// measurement rather than a preference: `运算.019 = Screenspace Info.001.SceneDepth − Screenspace Info.SceneDepth`
/// needs the SCENE DEPTH at a re-projected screen position, and the depth is THIS PASS'S OWN ATTACHMENT
/// (`character_forward_targets = {scene_color, gbuffer_depth}`) - the same wall that made step 3 move the rim into
/// a fullscreen pass of its own. So this stage takes the OPEN branch and the report says so; the zeros in
/// `混合.022`'s numerator are the reference's own `1.0` A side.
static const float goo_face_shadow_proxy_lift = 0.10000000149011612;
/// ... and the resulting value, spelled as the graph's own arithmetic rather than as `1.1`.
static const float goo_face_shadow_proxy_open = 1.0 + goo_face_shadow_proxy_lift;
/// `亮度 / 对比度.Contrast` - the FACE's IBL-diffuse darkening, `-0.4000000059604645`, and its `Bright` is `0.0`.
static const float goo_face_ambient_contrast = -0.4000000059604645;
/// `PBRToonBaseFace :: 组输入.Lips highlight color`'s `interface[]` default. THE MATERIAL THIS STEP PORTS LEAVES
/// IT AT THAT DEFAULT, and the value is why the lips term is not carried as a lane: `混合.023 = this ⊙ hl_M.r` and
/// `混合.025 = 混合.023 ⊙ max(ramp_u, 0.4)`, so a BLACK lips colour makes the term zero whatever the mask says.
/// THE MASK IS THEREFORE NOT READ EITHER - `分离 XYZ.007.X` is written below as the reference's own zero rather
/// than as a texture fetch of a sheet whose consumer is black. See `toon_slot::goo_face_csumt`'s note for the
/// three masks that ARE lanes.
static const float3 goo_face_lips_highlight_default = float3(0.0, 0.0, 0.0);
// ---- `PBRToonBaseFace`'s own `interface[]` defaults, for a material that states no row for a socket ----
//
// Read out of `gooblender/nodes.json`'s `::- Arknights: Endfield_PBRToonBaseFace :: interface[]`, socket by
// socket, EXCEPT where the socket's default already has a name in this file (the SDF pair, the CastShadow pair,
// `GlobalShadowBrightnessAdjustment`, `Color desaturation in shaded areas attenuation` and `MetallicMax` are the
// SAME numbers the Base container states, so step 4's constants are reused rather than duplicated).
static const float goo_face_chin_center_default = 0.0;
static const float goo_face_chin_sharp_default = 0.0;
/// `sphereNormal_Strength`'s default is `0.0`, i.e. "no sphere normal at all" - the behaviour the port had before
/// this step, which is why a material that states nothing is not given a sphere.
static const float goo_face_sphere_strength_default = 0.0;
static const float goo_face_smoothness_max_default = 1.0;
static const float goo_face_brightness_default = 1.149999976158142;
/// THE INTERFACE DEFAULT IS `1.2999999523162842` AND NOT `1.5`: `M_actor_laevat_face_01` OVERRIDES it to `1.5`
/// (its sidecar row carries `1.5`), so the two numbers are different on purpose - this one is what a FACE material
/// that states nothing gets, and it is the number the spec's §1.1 quotes for the socket.
static const float goo_face_eye_brightness_default = 1.2999999523162842;
static const float goo_face_front_r_pow_default = 2.0;
static const float goo_face_front_r_smo_default = 0.0;

/// `calculateAngel :: 值(明度)`'s output default - THE SIGN THE HEAD'S FORWARD AXIS IS MULTIPLIED BY.
///
/// `-1.0`, read out of `gooblender/nodes.json`'s `calculateAngel` FOR THIS STEP, and it is the same number
/// `shaders/goo_toon.slang` carries as `goo_iris_forward_sign` - the reference has ONE `calculateAngel`
/// data-block (`users == 5`) and both the iris group and the Face group instantiate it, so the two constants are
/// two spellings of one socket rather than two choices. The spec's §7.3 reports `1.0` off the same field and calls
/// it U1; the parent re-read the field in both dumps and it is `-1.0`, which is also what the iris' arm was
/// already built on. THE TWO ARE ASSERTED AGAINST EACH OTHER in `tests/test_goo_toon_math.cpp`, because a drift
/// between them would put the iris' highlight and the face's terminator on opposite sides of the head.
static const float goo_face_forward_sign = -1.0;
/// `calculateAngel :: 运算.004.Value_001` - the reference's own float literal for pi, so the two sides agree to
/// the bit. The iris' copy of the same number is `goo_iris_half_turn` in `shaders/goo_toon.slang`.
static const float goo_face_half_turn = 3.141592502593994;

/// @brief the reference's `calculateAngel`, both of its outputs, from the head's three axes and the sun
///
/// `.x` IS `AngleThreshold` = `1 − |atan2(dot(proj,headRight), 值(明度)·dot(proj,headForward))| / π` and `.y` IS
/// `Flip threshold` = the raw `dot(proj, headRight)`. THE SIGN IS THE FIELD READ THAT MATTERS AND IT IS `-1.0`:
/// `::- calculateAngel :: 值(明度).outputs[0].default = -1.0`, read out of `gooblender/nodes.json` for this step
/// and out of `chen_dump/nodes.json` too (the spec's §7.3 reports `1.0` from the same file and the same field, and
/// the iris' arm already used `-1.0` - see `goo_iris_forward_sign`). It is load-bearing: it shifts where
/// `AngleThreshold` crosses zero by a quarter turn, which is the whole of the SDF's terminator position and both
/// of the `Front transparent red` gates.
///
/// @note `normalize` OF A DEGENERATE VECTOR is Blender's `(0,0,0)` rather than a NaN, and `atan2` of two zeroes is
///       written out for the same reason `goo_iris_angle_threshold` writes it out: SPIR-V leaves it undefined.
static float2 goo_face_angle_thresholds(const float3 light_direction, const float3 head_up, const float3 head_right, const float3 head_forward) {
    const float3 projected = light_direction - dot(light_direction, head_up) * head_up;
    const float projected_length = length(projected);
    const float3 axis = projected_length > 1e-6 ? projected / projected_length : float3(0.0, 0.0, 0.0);
    const float right_component = dot(axis, head_right);
    const float forward_component = dot(axis, head_forward) * goo_face_forward_sign;
    const float azimuth_magnitude = abs(right_component) + abs(forward_component);
    const float azimuth_turns = azimuth_magnitude > 1e-12 ? atan2(right_component, forward_component) / goo_face_half_turn : 1.0;
    return float2(1.0 - abs(azimuth_turns), right_component);
}

/// @brief `亮度 / 对比度` at the FACE's own settings: `(colour − 0.5)·(contrast + 1) + 0.5 + brightness`
///
/// Blender's node is that expression with `Bright = 0.0` here, and it is the A side of `混合.019` - the FACE's IBL
/// diffuse reads the probe irradiance TWICE, darkened and raw, and `cm_M.G` picks between them.
static float3 goo_face_bright_contrast(const float3 colour, const float contrast) {
    return (colour - 0.5) * (contrast + 1.0) + 0.5;
}

/// @brief the reference's `SigmoidSharp` sub-group: `1 / (1 + 100000^(-3·sharp·(x−center)))`
///
/// SIX NODES, IN ITS OWN ORDER (`goo_step4_diffuse_spec.md` §2.1): `运算.003 = -3.0 * sharp`;
/// `运算.004 = x - center`; `运算.005 = 运算.004 * 运算.003`; `运算.002 = 100000 ^ 运算.005`;
/// `运算.006 = 1.0 + 运算.002`; `运算.007 = 1.0 / 运算.006`. ALL SIX `use_clamp` FLAGS ARE FALSE, so THE
/// EXPONENT IS NOT CLAMPED AND NEITHER IS `x` - which is why `x` can be an unclamped `N·L` (the half-Lambert
/// below is `0.5*N·L + 0.5` and lands in `[0,1]` naturally, not because anything clamped it).
///
/// @note THE DEGENERATE EXPONENT IS THE ONE THING THAT NEEDS A GUARD, and it is `sharp == 0`: then the exponent
///       is `0` and the answer is exactly `0.5` for every `x` - which is what the reference computes, and what
///       this returns - but a NEGATIVE-sharp material would drive `100000^exponent` to `+inf` and then
///       `1/(1+inf)` to zero, while `100000^-inf` is zero at the other end. Both are finite in IEEE and the
///       expression below is therefore left exactly as the reference writes it; the guard is only on the
///       exponent being a number at all (`isfinite`), because `pow` of a NaN is NaN on this platform and the
///       value would reach the framebuffer. The reference cannot produce it: Blender's `POWER` with a NaN
///       exponent answers a NaN too, and its own materials never state one.
static float goo_sigmoid_sharp(const float x, const float center, const float sharp) {
    const float exponent = goo_sigmoid_sharp_gain * sharp * (x - center);
    if (!isfinite(exponent)) {
        return 0.5;
    }
    return 1.0 / (1.0 + pow(goo_sigmoid_sharp_base, exponent));
}

/// @brief the reference's `SmoothStep` sub-group: `t*t*(3-2t)` with `t = clamp((x-min)/(max-min), 0, 1)`
///
/// FOUR NODES AND A CLAMP (`goo_step4_diffuse_spec.md` §8-A7): `运算.004.Value = 3.0`,
/// `运算.003.Value_001 = 2.0`, `钳制.Min`/`.Max` = `0.0`/`1.0`. `max == min` is not reachable on this asset
/// (the gate's `max` is the literal `1.0` and the fresnel's `SMO_H` is `0.5` or `1.07`), so the division below
/// is left as the reference's and the degenerate case answers the reference's own `clamp` of a division by zero.
static float goo_smoothstep(const float minimum, const float maximum, const float x) {
    const float t = saturate((x - minimum) / (maximum - minimum));
    return t * t * (3.0 - 2.0 * t);
}

/**
 * @brief the material's SHADOW COLOUR for its own albedo - the reference's `_ShadowLutTex` lane
 *
 * THIS IS THE ONE LANE THAT IS NOT A RAMP, and the difference is what it is for. A ramp answers "how deep is
 * the shadow here"; this answers "what is THIS MATERIAL's colour when it is in shadow", indexed by the
 * material's own albedo. `EfFaceSampleSkinLut` / `EfClothSampleLut` in the reference do exactly this, and the
 * reason skin ships one is that a skin tone's shadow is a function of the skin tone - a single per-family
 * constant cannot express it, which is why the family table wants a LUT here and a threshold there.
 *
 * THE TILING IS THE REFERENCE'S, deliberately and to the constant: a `32^3` cube in a `1024x32` strip, the
 * tile chosen by the X channel, the in-tile position by Y and Z, and a bilinear hop between the two adjacent
 * tiles because X is a continuous coordinate rather than an index. Its numbers are `1/32` and `31/32` plus the
 * half-texel offsets, and they are written below as the arithmetic they are so the next reader does not have
 * to recover them from decimals.
 *
 * THE DOMAIN IS THE DISPLAY-SPACE ALBEDO, which is the reference's own and is now this stage's too: the game's
 * palette was painted against the albedo AS THE ARTIST SAW IT, so the lookup converts the linear albedo through
 * `toon_linear_to_srgb` first. THE BAKED FALLBACK WAS CHANGED TO MATCH - it stores the display-space coordinate
 * rather than the linear one - which keeps its identity property ("albedo in, albedo out") true in the new domain
 * instead of turning the fallback into a brighter version of the albedo. See main.cpp's `bake_shadow_lut`.
 *
 * THE CONTENT OF THE BAKED LUT IS THE IDENTITY, which makes this lane READ WITHOUT CHANGING THE LOOK - the
 * same property the ramps have - and it is not a placeholder: what the lane is for is honouring an AUTHORED
 * cube, and an artist's intent is not something a neutral bake can invent. `albedo` in gives `albedo` out,
 * `params.shadow_tint` still rides on top, and the caller then evaluates the reference's own expression
 * `lerp(lutDark, authoredBright, diffuseWeight)` with `diffuseWeight` being this stage's ramp step.
 *
 * @param albedo the surface albedo (LINEAR, matching `surface_sample::albedo`)
 * @param lane the material's shadow-LUT heap lane; 0 means "do not read" and the caller must not call this
 */
float3 toon_shadow_lut(const float3 albedo, const uint lane) {
    // THE DISPLAY-SPACE COORDINATE, because the palette was painted in that space (see the note above).
    const float3 cube = saturate(toon_linear_to_srgb(albedo));
    // ---- THE THREE AXES ARE THE ARTICLE'S, AND THEY ARE THE SWIZZLE'S RATHER THAN AN ARBITRARY ASSIGNMENT ----
    //
    // The article writes BOTH its colour and its coordinate from one line (`ZmdFaceToonMain.hlsl:133`):
    //
    //     float3 albedo_sRGB = linear2sRGB(albedo.brg);
    //
    // so `albedo_sRGB = (f(b), f(r), f(g))`, and every later use of `.x` / `.y` / `.z` is a use of THOSE:
    //     `:134` `lut_uv = albedo_sRGB.xz * float2(31, 0.96875)`        -> the TILE is the BLUE channel
    //     `:136` `lut_uv_yz = albedo_sRGB.yz * float2(...) + ...`       -> in-tile X is RED, in-tile Y is GREEN
    //     `:140` `lutTex_lerp = albedo_sRGB.x * 31 - lut_uv_floorx`     -> and the tile blend weight with it.
    // The port read `tile <- cube.x`, `u <- cube.y`, `v <- cube.z`, i.e. the same three roles permuted, which on
    // the real asset (`T_actor_common_femaleskincolor02_lut_D`, `1024x32` = 32 `32x32` tiles of a `32^3` cube)
    // reads a DIFFERENT cell of the cube: the darkness is roughly right and the HUE is wrong. The axis order is
    // not a preference and not something to tune - the article's swizzle is the statement of it.
    //
    // THE BLEND SAMPLE'S OFFSET IS THE SAME STATEMENT: the article's second fetch is at
    // `lut_uv_final + float2(0.03125, 0.015625)` (`:142`) - `+1/32` in u, which is the TILE PITCH, and `+1/64`
    // in v, which is half of its `31/32` in-tile span - so the pair straddles the tile boundary by one tile in
    // u and half an in-tile span in v, and `lerp(low, high, lutTex_lerp)` walks from one tile to the next.
    // This port's two fetches are `u` and `u + 1/tiles` at the same v, which is that same pair written with the
    // tile pitch as a constant instead of as a decimal - the v offset being zero keeps the flip direction
    // consistent with the author's own upside-down coordinate (`1 - lut_uv_yz.y`).
    const float tile_f = cube.z * (character_shadow_lut_tiles - 1.0);
    const float tile = floor(tile_f);
    const float tile_lerp = tile_f - tile;
    // The half-texel offsets put a sample at a texel CENTRE rather than at its corner, which is what makes the
    // bake and this read invert each other instead of being off by half a texel everywhere.
    const float in_tile_x = cube.x * ((character_shadow_lut_tiles - 1.0) / (character_shadow_lut_tiles * character_shadow_lut_tiles)) + (0.5 / (character_shadow_lut_tiles * character_shadow_lut_tiles));
    const float in_tile_y = 1.0 - (cube.y * ((character_shadow_lut_tiles - 1.0) / character_shadow_lut_tiles) + (0.5 / character_shadow_lut_tiles));
    const float u = tile / character_shadow_lut_tiles + in_tile_x;
    const float3 low = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + lane), heap_sampler_texture, float2(u, in_tile_y)).rgb;
    const float3 high = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + lane), heap_sampler_texture, float2(u + 1.0 / character_shadow_lut_tiles, in_tile_y)).rgb;
    return lerp(low, high, tile_lerp);
}

// ============================================================================================================
// STEP 8: THE REFERENCE'S `DecodeNormal` - THE PORT'S TANGENT-SPACE NORMAL, RECONSTRUCTED FROM X AND Y
// ============================================================================================================
//
// THE DEFECT THIS REPLACES IS MEASURED, NOT SUSPECTED. `surface.glsl`'s decode (which is UNCHANGED by this step)
// samples the normal map as `heap_sample(...).rgb * 2.0 - 1.0`, i.e. it trusts the blue channel to be the tangent
// normal's `z`. Every normal map this asset ships stores `x` and `y` and a CONSTANT blue - measured per channel over
// the shipped PNGs: `T_actor_laevat_body_01_N` R uniq 233 / G uniq 136 / B uniq 1 (constant 0), `cloth_01_N` B uniq
// 1, `cloth_02_N` B uniq 1, `T_wpn_misc_0016_01_N` B uniq 1 (the one exception in the model is the hair's `_HN`,
// which is a live-RGB map; hair is NOT given a lane here, see the note at the end of this block). So `rgb*2-1` yields
// `z = -1` for every texel of every one of them and the shading normal points INTO the surface - a plausible-looking
// frame whose lighting is simply wrong.
//
// THE REFERENCE NEVER READS THE BLUE CHANNEL. Its `DecodeNormal` group (`Arknights: Endfield_PBRToonBase`, one
// instance, 12 nodes, `users = 3`) reconstructs `z` from `x` and `y`; read node by node out of
// `deren-ab/gooblender/nodes.json`:
//
//     Vector Math       (MULTIPLY_ADD)  = (2x-1, 2y-1, 0)            from the group's X / Y inputs
//     Vector Math.001   (DOT_PRODUCT)   = d = (2x-1)^2 + (2y-1)^2
//     钳制              (CLAMP)         = clamp(d, 0.0, 1.0)
//     运算              (SUBTRACT)      = 1.0 - clamp(d, 0, 1)
//     运算.001          (SQRT)          = sqrt(1.0 - clamp(d, 0, 1))
//     运算.002          (MAXIMUM)       = z1 = max(sqrt(...), 1.0000000168623835e-16)   [Value_001 = that literal]
//     运算.003          (MULTIPLY_ADD)  = 0.5*z1 + 0.5                                   [0.5*z1 + 0.5, verbatim]
//     合并 XYZ.001      (COMBINE_XYZ)   = Color = (x_raw, y_raw, 0.5*z1 + 0.5)
//     法线贴图          (ShaderNodeNormalMap, Strength <- 组输入.NormalStrength, Color <- the combine above)
//     组输出            (GROUP_OUTPUT)  -> NormalWS
//
// The `1.0000000168623835e-16` floor is the group's own literal (`~sqrt(float32_min_normal)`, the same shape the
// article's eye hemisphere uses at `1e-16` above); it stops a `1 - d` of exactly zero from producing a normal the
// NormalMap node would then divide by.
//
// THE NORMAL MAP NODE IS A BLACK BOX IN THE DUMP, but the REFERENCE ENGINE'S OWN IMPLEMENTATION of that node is not:
// the strength's form comes from the Goo engine's source rather than from a reading of the graph -
// `goo-engine-v4.2-release`, `source/blender/gpu/shaders/material/gpu_shader_material_normal_map.glsl` (a local copy
// is kept at `deren-ab/goo_engine_node_normal_map.glsl`), whose `node_normal_map` applies the strength IN TANGENT SPACE
// to the `(xy, z1)` that `color_to_normal_new_shading` (`2*color - 1`) has already produced:
//
//     texnormal.xy *= strength;
//     texnormal.z = mix(1.0, texnormal.z, saturate(strength));
//
// so this port ships `n_ts = normalize(float3(strength * xy, mix(1.0, z1, saturate(strength))))`. FOR EVERY STRENGTH
// THIS STEP PORTS (`>= 1`: the body's 1.25, the cloth's 1.4458599090576172) `saturate(strength)` is exactly 1 and the
// z term is `z1` - "scale xy, keep z" - which is also the form the AUTHOR'S OWN Unity port uses
// (`deren-ab/author_src/ZmdToonUtils.hlsl:9-16`, `UnpackNormalFromTex`: `normal.xy *= scale;` after the same z
// reconstruction), so two independent sources agree. AN EARLIER REVISION OF THIS BLOCK SHIPPED
// `1.0 + strength*(z1 - 1.0)`, and the engine's `saturate` is exactly what excludes it: at the body's own `strength`
// that expression answers `z = -0.1961161351` for a texel at the map's extreme - a normal bent THROUGH the surface,
// 11.31 degrees away - and it treats the strength as a WEIGHT for every `strength != 1`, not as a scale. BOTH SIDES
// ARE PINNED IN A UNIT TEST, together with the `saturate`'s own edge cases (a strength below 1, and a negative one).
//
// WHERE THIS IS APPLIED AND WHERE IT IS NOT: only the TWO ENTRY POINTS THAT DRAW THE REWRITTEN CHAIN call this
// (`character_forward.slang`'s own `main` and `shaders/goo_toon.slang`'s `goo_toon_frag_main`), and the body is
// compiled out entirely unless `VR_GOO_TOON_CHAIN` is defined - so `outline.slang` and `pbr.slang`, which include
// this file WITHOUT the define, keep the old argument and the article chain's frames stay byte-identical. THE
// G-BUFFER'S NORMAL IS DELIBERATELY NOT CHANGED (`shaders/gbuffer.slang` still writes the `surface.glsl` decode's
// result, and `surface.glsl` itself is untouched): the toon stage takes the geometric normal and reads the map
// itself, so the two can disagree for a normal-mapped material - a recorded, deliberate asymmetry, because moving
// the G-buffer's normal would move the article chain, every SSAO/shadow consumer of it and four pinned frames.
//
// HAIR: the reference gives `M_actor_laevat_hair_01` a `NormalStrength` of `0.5` (an INSTANCE value - the group
// default is `1.0`) and this step ADDS THAT ROW, so the decode above is now what the hair takes; the number is
// pinned in `tests/test_goo_toon_math.cpp` beside the body's and the cloth's. The premise this paragraph used to
// state is refuted by the step-14 dump: the hair's `_HN` is not a map with a live blue channel but a packed pair of
// two tangent-space normals - `(R, G)` is the first, reconstructed by this same `DecodeNormal`, and `(B, A)` is the
// second, whose `HNormalStrength` of `0.75` has no carrier here: the dump traces it into the group's `H Normal`
// output and, through the Anisotropic-highlight frame, into its `HightLightColor` and the Emission behind the
// group's `Result` - the hair highlight, unported. The old decode read `B` as `z`, i.e. the wrong axis entirely.
static const float goo_normal_z_floor = 1.0000000168623835e-16;
/// the reference's group default for a material that states none - recorded here because `toon_diffuse`'s arm and the
/// sidecar's diagnostics both quote it, NOT because an absent lane falls back to it (see the lane's own note).
static const float goo_normal_strength_default = 1.0;

/// @brief the reference's `DecodeNormal`, applied to the material's normal map; the port's normal when it cannot be
/// @param world_pos world-space position, for the uv-derivative tangent frame
/// @param geo_normal the INTERPOLATED GEOMETRIC normal (the entry point's `v_normal`), which is what the frame's
///        third axis is - `surface_sample` does not carry it any more, and `s.normal` cannot be inverted back to it
/// @param uv the material's uv
/// @param port_normal `normalize(s.normal)`, i.e. what the caller would have passed before this step
///
/// THE THREE WAYS THIS ANSWERS THE PORT'S OWN NORMAL, in the order they are tested: the material has no normal
/// texture (`mat.flags & 1u` - the port's boolean for "this material reads a normal map at all", see the uncertainty
/// section of the result document for the reference's `Use NormalTex?` gate this approximates); lane 24 is absent
/// (the material has no `_GooNormalStrength` row); or the uv-derivative frame is degenerate (`surface_tbn` returns
/// false, the same fallback `surface.glsl` takes on a collapsed triangle).
float3 goo_toon_shading_normal(const float3 world_pos, const float3 geo_normal, const float2 uv, const float3 port_normal) {
#if defined(VR_GOO_TOON_CHAIN)
    const Material mat = material_at(heap_slots_materials, push.material_index);
    if ((mat.flags & 1u) == 0u) {
        return port_normal;
    }
    const float strength = toon_colour_at(heap_slots_toon_colours, (push.material_index * character_toon_colour_lanes) + 24u).x;
    if (!(strength > goo_lane_absent_threshold)) {
        return port_normal;
    }
    const float3 normal = normalize(geo_normal);
    float3 sdir = float3(1.0, 0.0, 0.0);
    float3 tdir = float3(0.0, 1.0, 0.0);
    // THE SAME FRAME `surface.glsl` BUILDS, and the same fallback when the derivatives are degenerate: the article's
    // own note says the uv-derivative frame is what the reference's maths corresponds to, so no vertex tangent and no
    // vertex-format change is needed.
    float3 decoded = normal;
    if (surface_tbn(world_pos, uv, sdir, tdir)) {
        const float2 xy = heap_sample(mat.tex_indices.z, uv).rg * 2.0 - 1.0;
        const float d = clamp(dot(xy, xy), 0.0, 1.0);
        const float z1 = max(sqrt(1.0 - d), goo_normal_z_floor);
        // THE STRENGTH'S FORM COMES FROM THE REFERENCE ENGINE'S OWN GLSL and not from the dump's black-box node: `xy`
        // is scaled by the RAW strength while `z` is mixed towards the flat normal by `saturate(strength)` - see the
        // note above. For a strength at or above 1 that is exactly `float3(strength*xy, z1)`, and the `saturate` is
        // kept rather than folded away because it is the engine's own expression, it is what a strength below 1 needs
        // - the hair's `0.5` is one since this step's row landed - and it is what makes `z` strictly positive.
        const float3 n_ts = normalize(float3(strength * xy, mix(1.0, z1, saturate(strength))));
        decoded = normalize(mat3(sdir, tdir, normal) * n_ts);
    }
    // THE DOUBLE-SIDED FLIP IS APPLIED TO THE RESULT AND NOT TO THE FRAME, which is `surface.glsl`'s own order: the
    // frame's third axis is the RAW geometric normal and the flip is the last thing that happens to the normal.
    if ((mat.flags & 8u) != 0u && !gl_FrontFacing) {
        decoded = -decoded;
    }
    return decoded;
#else
    return port_normal;
#endif
}

/**
 * @brief the toon diffuse: THREE ALBEDO LAYERS chosen by the light, the shadow and the material's own ramp
 *
 * THIS IS THE 终末地 CHAIN, and it differs from the quantizer this stage started with in what the ramp IS. That
 * model read a neutral unit step out of the ramp lane and used it as a THRESHOLD - the shadow picked the side and
 * the half-Lambert positioned the fragment within it - so the ramp carried a shape and the family carried the
 * colour. The game's ramp is the opposite arrangement: its U axis IS the light term, its RGB is a COLOUR mapping
 * along that axis and its ALPHA is a LIGHTNESS, so the texture answers "what colour is this at this light term"
 * and "how lit is this at this light term" at once, and the family's numbers move to the layers instead of the
 * threshold. Both readings are implemented below and the branch between them is the LANE: a material with a ramp
 * takes the texture's answers, and a material without one gets the family's tint and Hermite step, which is what
 * the previous model did for everybody.
 *
 * WHY LAYERS RATHER THAN A MULTIPLIER, and it is the article's whole thesis: a toon character's dark side is a
 * HUE shift as much as a darkening, so the shadow side needs its OWN albedo - and the darkest region inside it
 * needs a third. The light, the shadow and the material's ambient occlusion then CHOOSE among the three, which is
 * why the shadow appears here as a gate on a lerp's weight and not as a factor on the result: applied as a factor
 * as well it would extinguish the layers it was choosing between.
 *
 * THE RAMPS THIS READS ARE THE GAME'S OWN (the asset pipeline's `_DiffRampMap`, 256x1), which is a change from
 * the neutral bake this repository carried: the bake remains as the fallback for a model that has none, but a
 * character that ships its ramp is shaded with it - see `main.cpp`'s `bake_neutral_ramp` and `toon_texture`.
 *
 * @param albedo the surface albedo (linear)
 * @param albedo_alpha the albedo texture's ALPHA, which the article's chain reads as a MATERIAL weight rather
 *        than as coverage: it decides how far the diffuse steps aside for the highlight. The data says this is
 *        per-material and not universal - the face's alpha is a constant 1 while the body's carries a mask - so
 *        the chain must work for both, which is what `_DiffuseBlendEffect` being the rig's buys.
 *        ... AND IT IS A SECOND CONSUMER'S ENTIRE SOURCE: the article's SKIN reads its OCCLUSION out of this
 *        same channel (`ZmdSkinToonMain.hlsl:154`), which is the one family whose `ao` is not the metallic/gloss
 *        map's blue - see `ao_used` below.
 * @param family the material's `deren::gltf::toon_family`, which this function needs for the TWO places the article's
 *        shaders disagree BY FAMILY rather than by material: the occlusion's source (`ao_used`) and nothing else
 *        - every other branch here is selected by a LANE, because a lane is what the asset states.
 * @param n world-space shading normal
 * @param world_pos world-space position, for the shadow lookup
 * @param ao_engine the ENGINE's own occlusion for this fragment (the G-buffer's, from glTF if the material has
 *        one). It multiplies the map's own baked occlusion rather than replacing it, so neither term has to know
 *        about the other - and the product scales the AMBIENT and the layer weights, never the sun's own
 *        radiance, because a baked occlusion term is a statement about the sky's reach.
 * @param params the material FAMILY's numbers (see toon_params.slang): the thresholds and tints the procedural
 *        branch uses, the rim's shape, and the 终末地 fields this chain added
 * @param toon_indices the material's FOUR in-record sidecar LANES (see `deren::vulkan::toon_slot`): x is the diffuse
 *        ramp, y the shadow LUT, z the specular ramp, w the matcap. A lane of 0 is "the artist switched this off,
 *        or the model does not have it", and every read below is gated on that rather than on a separate flag -
 *        which is what makes the `_Use` flag in the sidecar the only switch there is.
 * @param toon_lanes the lanes that do NOT fit the record (see `core::heap_slots::toon_lanes`): x is the face SDF
 *        map and y the metallic/gloss map, each 0 for "do not read"
 * @param out_desaturation STEP 6's ONE OUTPUT, and it is an OUT PARAMETER for a reason rather than for symmetry:
 *        the reference applies this term TO THE FINISHED PIXEL (`色相/饱和度/明度` sits after the rim, after
 *        emission and after RS EFF - see the step-6 block at the top of this file), while this function is the
 *        rim-inclusive PRE-EMISSION colour. The caller adds `s.emissive` to what this returns, so applying the
 *        desaturation here would leave the emission undersaturated; the value therefore travels out and the
 *        caller applies it at the pixel, which is where the reference has it. It is `goo_desaturation_neutral`
 *        unless the step-6 arm runs, so a caller that does not care gets the identity.
 */
float3 toon_diffuse(float3 albedo, const float albedo_alpha, const uint family, float3 n, float3 world_pos, float ao_engine, toon_params params, uint4 toon_indices, const float2 uv, const uint4 toon_lanes, const uint material_index, out float out_desaturation) {
    // ---- STEP 4'S TWO OUTPUTS, WHICH ARE LOCALS RATHER THAN PARAMETERS BECAUSE THE COMPOSITION IS ALSO HERE ----
    //
    // The reference multiplies `混合.011` - `directLighting_diffuse` PLUS direct specular, IBL diffuse and IBL
    // specular - by a shadow-brightness gate and a ToonFresnel colour, and `toon_colour` further down this
    // function is where those four are summed. So the arm below cannot return the composed sum (an earlier
    // revision tried, and referenced two names declared after it); it substitutes ONE term and sets these two.
    //
    // NEUTRAL IS THE NO-OP AND NOT A SPECIAL CASE: `1.0` and white, so the branch at `toon_colour` is an exact
    // identity for every material this arm does not take - which is what keeps a `goo_toon = false` frame
    // byte-identical rather than merely equal.
    bool goo_arm = false;
    float3 goo_factor = float3(1.0, 1.0, 1.0);
    // ---- STEP 6'S ONE OUTPUT, DECLARED BESIDE THE OTHERS AND NEUTRAL BY DEFAULT ----
    //
    // The reference's `色相/饱和度/明度` reads `Saturation = 钳制.004 = clamp(luma(RampColor) + attenuation, 0, 1)`,
    // and BOTH addends are the material's: `luma` is the dark ramp sampled at the same `u` step 4 samples it at, and
    // the attenuation is `_GooDiffuseB.y` - a lane that has been carried SINCE STEP 4 with no consumer (its note
    // says so), so this step gives it the consumer rather than a new lane. A material this arm does not take keeps
    // `goo_desaturation_neutral`, which is the identity of the desaturation - NOT the socket's group default, whose
    // own comment says why. A material the arm DOES take and that states no `.y` gets the group default, `0.0`.
    float goo_desaturation = goo_desaturation_neutral;
    // ---- STEP 5'S THREE SUBSTITUTES, DECLARED BESIDE STEP 4'S TWO AND FOR THE SAME REASON (see the note above) ----
    //
    // Step 5 replaces the OTHER THREE TERMS of `混合.011`: `directLighting_specular` (the port's `main_specular`),
    // `IBL_diffuse` (the port's `ambient`) and `IBL_specular` (the port's `indir_light_specular`). Two of those
    // three are also declared BELOW this arm, so the arm cannot assign them - the same compile error step 4 hit -
    // and the substitutes therefore ride these three locals, which the composition at `toon_colour` selects.
    //
    // ZERO IS THE NEUTRAL HERE, unlike `goo_factor`'s white, because the composition ADDS them: a material this arm
    // does not take contributes nothing rather than becoming the whole lighting. With `goo_arm` false these three
    // are never read at all, so the value is a formality - but a formality that has to be the additive one.
    float3 goo_spec_direct = float3(0.0, 0.0, 0.0);
    float3 goo_spec_ibl = float3(0.0, 0.0, 0.0);
    float3 goo_diff_ibl = float3(0.0, 0.0, 0.0);
    // THE LIGHT VECTOR IS THE SURFACE-TO-LIGHT DIRECTION, which is the engine's existing convention
    // (`evaluate_direct_light` normalizes this same field into its `l`).
    const float3 l = normalize(light_at(heap_light_slot).light_dir.xyz);

    // ---- THE EYE'S OWN NORMAL: A HEMISPHERE THE MATERIAL'S UV DESCRIBES ----
    //
    // THE ARTICLE SHADES AN EYE WITH A MANUFACTURED NORMAL, and this is the half of that mechanism this port was
    // missing: its `NoL = dot(corneaNormalWS, flattenedLightDirWS)` and its `NoF` off the same normal are what put a
    // CURVED terminator ACROSS the iris, so its upper half reads lit and its lower half shadowed. This port built
    // the same hemisphere at the matcap and spent it there only, so the ramp was indexed by the FLAT quad's normal -
    // one value over the whole eye - and an eye whose single value lands under the ramp's threshold is uniformly
    // dark rather than graded. MEASURED on chen's iris: the rendered mean is 0.65x its own texture with the hue
    // pulled cool (render (91.9,75.1,81.2) against the texture's (141.2,102.9,100.7)), i.e. the DARK layer
    // everywhere - `dark_base = albedo * _ShadowColorBrightness` with `_ShadowColorSaturation` 1.4 on top.
    //
    // THE ZONE AND THE BASIS ARE THE HIGHLIGHT'S OWN, and they are computed HERE because the ramp needs them: the
    // uv's offset from the eye's centre (`eye_uv`, used again by the trick colours and the matcap below), and a
    // basis built from the CAMERA - which is `toon_matcap_uv`'s own argument, that a matcap is a picture of a ball
    // so a roll about the view axis cannot show. The article takes the basis from the mesh tangent instead.
    //
    // WHAT IS NOT REPRODUCED IS ITS LIGHT DIRECTION: the article flattens `_MainLightPosition` onto the face's own
    // plane with `_FaceRight` / `_FaceForward`, which are globals the game supplies and no sidecar of this
    // repository carries, so this uses the light as it is. That makes this term the article's SHAPE - a hemisphere
    // lit from one side - rather than its exact angle, and it is written down as a difference rather than hidden.

    const float2 eye_uv = frac(uv) - float2(0.5, 0.5);
    // `step(0.25, |uv|^2)` is the article's own line between the two zones: INSIDE it the hemisphere stands, outside
    // it the quad's flat normal does, which is what keeps the cornea's curvature off the sclera's rim.
    const float eye_centre_area = step(0.25, dot(eye_uv, eye_uv));
    // THE MESH'S OWN TANGENT FRAME, read ONCE here for both of the eye's uses below - the cornea's basis and the
    // parallax's `viewDirTS` - and it is the article's own basis rather than the camera-built one this port used to
    // substitute: the article takes `tangentWS` / `bitangenWS` from the VERTEX TANGENT attribute and builds
    // `corneaNormalWS` from them, and `surface_tbn` derives that same frame from the surface's uv derivatives (the
    // substitute this repository uses wherever a vertex tangent would be - see its note on mirrored-uv handedness).
    // A DEGENERATE FRAME FALLS BACK TO THE FLAT NORMAL, which the article never needs (a vertex tangent always
    // exists) and a uv-derivative frame does on a collapsed triangle.
    vec3 eye_tangent = vec3(1.0, 0.0, 0.0);
    vec3 eye_bitangent = vec3(0.0, 1.0, 0.0);
    const bool eye_has_tbn = surface_tbn(world_pos, uv, eye_tangent, eye_bitangent);
    float3 shading_normal = n;
    if (toon_indices.w != 0u && eye_has_tbn) {
        const float2 centred_uv = eye_uv * 2.0;
        const float z_hemi = max(1e-16, sqrt(max(0.0, 1.0 - min(1.0, dot(centred_uv, centred_uv)))));
        float3 cornea_ts = float3(character_cornea_bump * centred_uv, z_hemi);
        cornea_ts = lerp(cornea_ts, float3(0.0, 0.0, 1.0), eye_centre_area);
        // the article's `corneaNormalTS.x = -x` flip, without which the terminator sits on the wrong side.
        cornea_ts.x = -cornea_ts.x;
        // `corneaNormalTS.x * tangentWS + .y * bitangenWS + .z * normalWS`, the article's own line.
        shading_normal = normalize(cornea_ts.x * eye_tangent + cornea_ts.y * eye_bitangent + cornea_ts.z * n);
    }

    // ---- THE PARALLAX DEPTH: THE ASSET'S OWN NUMBER WHEN IT STATES ONE, THE CONSTANT ABOVE OTHERWISE ----
    //
    // READ HERE RATHER THAN BESIDE THE OTHER LANES, and the placement is forced rather than chosen: this is the
    // first use in the function (`parallax_offset` below), and the material's colours are read further down for
    // the reasons stated there. `material_index` is all this read needs - the lane is a flat
    // `material_index * character_toon_colour_lanes + lane` lookup like every other one.
    //
    // `_ParallaxScale` IS A PER-MATERIAL ROW AND THE STAGE USED ONE MATERIAL'S VALUE ON EVERY MATERIAL. It
    // carried chen's iris number (`character_eye_parallax_depth`, 0.03) with a note saying the per-material row
    // could not be carried. It can: the row is in the asset's own `extras` block
    // (`deren::gltf::claimed_extras_floats`, read into `toon_colour_lane::parallax_scale`), which the host resolves
    // before the sidecar and before the constant. MEASURED on `chars\chen.glb`, the asset states it for three
    // materials - iris 0.03, brow 0.5, cloth_01 0.5 - and holds no such row for the rest, and the `.toon.tsv`
    // sidecar this repository carries has no `_ParallaxScale` row at all, so the asset is the only source that
    // can speak for this property on this character.
    //
    // THE FALLBACK IS THE CONSTANT ITSELF RATHER THAN A FAMILY NUMBER, which is the difference from the specular
    // lane below and it is deliberate: the eye path (`toon_indices.w != 0`, a material with a matcap) is the ONLY
    // consumer of this row in the whole port, so the right answer for a material that states nothing is the
    // offset that material had before this lane existed - a family number would be a value invented here.
    //
    // WHAT THIS READ TURNED OUT TO CHANGE ON chen IS NOTHING, AND THE REASON IS WORTH WRITING DOWN because it is
    // the opposite of what the row's values suggest (three materials state it, and only ONE of them can consume
    // it): the parallax block below is gated on `toon_indices.w != 0` - the material's MATCAP - and the iris is
    // the only chen material that has one. The brow's sidecar carries a `_MatcapColor` and no `_MatcapTex`, and
    // the runtime prints `split normal 0` for it, so its 0.5 reaches the GPU and stops there. The iris' 0.03
    // equals the constant bit for bit, and the other four state no row at all, so wiring this lane changes ZERO
    // pixels on this character - which is why a PROBE is what proves the lane is live rather than ignored: the
    // iris re-stated as 0.3 in the asset's own extras moves 1 315 pixels, 0.0 moves 480 and 1.0 moves 1 485,
    // every one of them inside the iris (`deren-ab/parallax_zoom.png` paints the mask over the eye).
    //
    // `0.0` IS A LEGAL STATEMENT ON THIS ROW (a material that wanted no parallax would state it), so as on the
    // specular lane "absent" is `-1` rather than zero and the test is `>= 0.0` on both.
    const float extras_parallax_scale = toon_colour_at(heap_slots_toon_colours, (material_index * character_toon_colour_lanes) + 5u).x;
    const float parallax_depth = extras_parallax_scale >= 0.0 ? extras_parallax_scale : character_eye_parallax_depth;

    // ---- THE ARTICLE'S PARALLAX, which moves the ALBEDO'S LOOKUP AND NOTHING ELSE ----
    //
    // `offsetDir` is the view direction expressed IN THE MESH'S OWN TANGENT FRAME (the article's `viewDirTS`, whose
    // basis is the mesh TBN - which this stage has through `surface_tbn`, so this read is the article's rather than an
    // approximation of it), the offset is `_ParallaxDepth * float2(1.0, 0.25)` - the article's quarter-strength y,
    // kept - and the mask is a smoothstep of `-5 * (distSq - 0.25)`: full at the iris' centre, zero by the disc's
    // edge, which is what keeps the sclera from sliding. `distSq` is the SAME `dot(eye_uv, eye_uv)` the cornea zone
    // uses, on the RAW uv - the article computes the zone from `frac(i.uv0)` and offsets only the texture read.
    // THE DEPTH IS `parallax_depth` AND NOT A CONSTANT HERE, which is the one line this block gained when
    // `_ParallaxScale` became a per-material row (see its read above and `character_eye_parallax_depth`): the
    // article's `_ParallaxDepth` is per material, and which material this is comes from the record the fragment
    // already carries.
    //
    // ONLY THE TEXTURE READ MOVES, which is the article's own arrangement: its `finalEyeUV` is passed to `_AlbedoTex`
    // and to nothing else, so the cornea normal, the ramp, the two trick colours and the matcap all stay on the raw
    // uv. Re-sampling here rather than inside `gather_surface` is what keeps it to this stage (a G-buffer is shared
    // with the deferred path, and the article's parallax is a character-forward statement about an iris).
    if (toon_indices.w != 0u && eye_has_tbn) {
        const float3 to_camera = normalize(camera_at(heap_camera_slot).camera_pos.xyz - world_pos);
        const float3 view_ts = float3(dot(eye_tangent, to_camera), dot(eye_bitangent, to_camera), dot(n, to_camera));
        const float2 offset_dir = normalize(view_ts).xy;
        const float2 parallax_offset = offset_dir * parallax_depth * float2(1.0, 0.25);
        const float dist_sq = dot(eye_uv, eye_uv);
        const float mask_raw = saturate(-5.0 * (dist_sq - 0.25));
        const float parallax_mask = mask_raw * mask_raw * (3.0 - 2.0 * mask_raw);
        const Material eye_material = material_at(heap_slots_materials, material_index);
        albedo = (eye_material.base_color_factor * heap_sample(eye_material.tex_indices.x, uv - parallax_offset * parallax_mask)).rgb;
    }
    const float no_l = dot(shading_normal, l);

    // DEBT (s): `calc_shadow` returns the RAW per-cascade visibility, UNQUANTIZED - the reference never
    // quantizes `Cast Shadows` and has no band generator on that chain (its `_RD` ramps are continuous
    // remaps, not steps; see `deren-ab/goo_debt_s_armA_verify.md` §2.4). Nor is any shadow/visibility/diffuse
    // term in THIS shader quantized into a cel band: the only quantizers here are the SDF half-face
    // expression, the tile-atlas lookup and the eye/emoji UV selection (`ceil`/`floor`/`frac`), none of
    // them a cel band. The knob's leftover effect (the `ndotl` band plus the highlight block) lives inside
    // `evaluate_direct_light`, whose only callers are the two direct-light sites in `shading.glsl` and the
    // two in `megalights_trace.slang` - never `character_forward.slang` or `goo_toon.slang` - so on this
    // path it lands on the deferred/pbr path only.
    //
    // AN EYE DOES NOT READ IT AT ALL, because the ARTICLE'S EYE SHADER HAS NO CAST-SHADOW TERM - its shading is
    // the toon ramp off the cornea normal, `NoF`, and the two-state light, and that is the whole list. This is not
    // a stylistic choice on this port's part but the fix for a measured dead end: MEASURED with a false-colour
    // probe of `ao` / `shadow` / `ramp_weight` on the face close-up, the iris is at `shadow = 0.00` across 100% of
    // its pixels while the cheek one centimetre away is at `1.00` (forehead 0.09 under the bangs, hair 0.55) - the
    // eyeball is INSIDE the socket, so the shadow map's depth at its own screen position belongs to the face plate
    // in front of it and every eye pixel is "occluded" for good. `min_shadow_effect = min(ao, shadow, ramp_weight)`
    // is therefore 0 over the whole eye, the LIT layer never contributes, and the iris can only ever be the dark
    // layer (`albedo * _ShadowColorBrightness` with `_ShadowColorSaturation` 1.4 on top) plus the ambient and the
    // matcap: a flat, dark, desaturated ball - which is what the eye looked like, and why the cornea normal, the
    // two trick colours and their `* 2.5` all measured as no-ops or near-no-ops before this line existed.
    // THE ONE SHADOW CALL SITE OF THIS BODY, and the one place `[render] toon_shadow_softness` acts: the
    // level rides the rig's ninth lane (read straight off the handle because the `rig` value below is bound
    // AFTER this line) and `calc_shadow_soft` treats 0 as "call `calc_shadow` and nothing else", which is what
    // keeps the default run byte-identical to the shipped one. THIS BODY HAS THREE CALLERS - this file's own
    // entry point, `shaders/outline.slang` and `shaders/goo_toon.slang` (the rewritten chain reuses
    // `toon_diffuse` for every non-eye family) - so the level is compiled into those three stages as well;
    // that is a property of the shared body, not a second switch. `shaders/goo_toon.slang`'s OWN shadow call
    // (its `rs_shadow`, the bare `calc_shadow`) is the one left byte-untouched, and none of the three stages
    // changes a pixel at level 0.
    // THE AREA LIGHT'S SHADOW IS A THIRD ROUTE THROUGH THIS ONE CALL SITE (spec 3.2), and it is switched by
    // the sign of the lane the light UBO already carries rather than by a new uniform: `area_light_axis.w > 0`
    // is the host saying "this light has a penumbra of that many world metres", anything else (`<= 0`, the
    // default with `[lighting] area_light_size = 0` AND with `area_light_shadow = false`) falls through to the
    // EXACT expression that shipped - not a copy of it - so a default run compiles the same two functions it
    // always did. `calc_shadow_area` itself returns `calc_shadow(world_pos, normal)` for `<= 0` as well, which
    // makes the byte-identity guarantee hold at both levels.
    const float shadow = toon_indices.w != 0u
                             ? 1.0
                             : (light_at(heap_light_slot).area_light_axis.w > 0.0
                                    ? calc_shadow_area(world_pos, n)
                                    : calc_shadow_soft(world_pos, n, toon_rig_at(heap_slots_toon_rig).shadow_softness.x));

    // ---- THE CAMERA'S OWN BASIS, which the article's chain needs twice ----
    //
    // `CAMERA FORWARD` IS NOT THE VIEW VECTOR, and the difference is the whole reason it is read here: the view
    // vector points from THIS fragment to the camera, so it fans out across the frame, while the camera's forward
    // axis is ONE direction for the whole frame - and the article uses that one direction to decide whether the
    // character is backlit (a property of the rig) and where its highlights sit (a property of the pose). Its
    // `UNITY_MATRIX_V[2].xyz` is the view matrix's third ROW, which is the camera's world-space BACK axis: the
    // direction a surface faces when it looks at the camera. Read the same way here, from the view matrix the
    // engine already publishes, so no second per-frame block is needed for one direction.
    const float3 camera_forward = normalize(mul(camera_at(heap_camera_slot).view, float4(0.0, 0.0, 1.0, 0.0)).xyz);

    // THE RIG, read once here and used by every stage of the chain below (see `ToonRigUBO`): the sun/head-light
    // split, their shadow-side colours, and the scalars that are the ARTICLE's globals rather than a family's or
    // a material's.
    const ToonRigUBO rig = toon_rig_value();

    // ---- THE MATERIAL'S OWN COLOURS, from the table the host fills per material ----
    //
    // THE SECOND HALF OF THE SIDECAR'S VOCABULARY: the `color` rows are four floats rather than a texture name, so
    // they cannot ride `toon_lanes` (a `uvec4` of texture indices) and they live in a flat `float4` array the
    // shader addresses as `material_index * character_toon_colour_lanes + lane` (see `deren::vulkan::toon_colour_lane`).
    // A MATERIAL WITH NO SUCH ROW READS WHITE, which is the neutral every one of these lanes is defined against -
    // so the shading falls back to doing nothing rather than to doing something black. THE TWO SCALAR LANES (4
    // and 5) ARE THE EXCEPTIONS to that neutral (`-1`, read below), because their values are a strength and a
    // depth rather than tints.
    const uint colour_base = material_index * character_toon_colour_lanes;
    const float3 sdf_rim_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 0u).rgb;
    const float3 eye_highlight_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 1u).rgb;
    const float3 eye_scattering_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 2u).rgb;

    // ---- THE PARALLAX DEPTH IS READ ABOVE, beside the article's parallax block, for the reason stated there ----
    // (it is that block's first use and `parallax_depth` is in scope from its declaration onward).

    // ---- THE SPECULAR STRENGTH: THE ASSET'S OWN NUMBER WHEN IT STATES ONE, THE FAMILY'S OTHERWISE ----
    //
    // THE GAME STATES `_Specular` PER MATERIAL AND THIS PORT USED TO READ IT PER FAMILY. The family table
    // (`toon_params.slang`) is the article's shape - one set of numbers per family - and it carried the game's
    // value as a FAMILY constant, which is right for six of chen's seven materials and wrong for the seventh:
    // `M_actor_chen_brow_01` classifies as `face` (its own eyebrow) and states `_Specular = 0.0` against the
    // face's 1.0, so the brow wore a highlight nobody authored.
    //
    // WHERE THE NUMBER COMES FROM IS A THIRD SOURCE, and this lane is the first property connected from it: the
    // ASSET'S OWN `extras` block, which is the game's exported material table travelling inside the .glb
    // (`deren::gltf::claimed_extras_floats`). The host resolves the order - extras first, then the `.toon.tsv` sidecar
    // (which carries no `_Specular` row at all), then this family table - and the ONLY thing this stage has to
    // know is the sentinel: `.x < 0` means neither source spoke, and the family's number stands. `0.0` is a value
    // the game really states (the iris and the brow), which is why "absent" could not be encoded as zero.
    const float extras_specular_strength = toon_colour_at(heap_slots_toon_colours, colour_base + 4u).x;
    const float specular_strength = extras_specular_strength >= 0.0 ? extras_specular_strength : params.spec_strength;


    // ---- THE HAIR'S SMOOTH NORMAL, packed beside the model's own in ONE texture ----
    //
    // THE GAME'S `_SplitNormalMap` (chen's hair ships `T_actor_chen_hair_01_HN`) holds TWO tangent-space normals in
    // one map: the model's own and a SMOOTHED one, which the reference port confirmed at the instruction level. The
    // smooth one exists for exactly ONE consumer - the highlight - because a strand's silhouette is geometry while
    // its highlight is a line DOWN the strand, and the raw normals of a low-poly hair shell break that line into
    // facets.
    //
    // IT IS READ ONLY WHERE THE LANE IS SET, and that is what keeps the derivative-based frame below off every other
    // material's fragments: a lane of 0 (no map, or the artist's `_UseSpecBumpMap` off) leaves the highlight on the
    // model's own normal, exactly where it was before this lane existed.
    //
    // THE `.zw` PAIR IS THE SMOOTH ONE and it unpacks like any tangent-space normal - two channels plus a
    // reconstructed third - and the frame comes from `surface_tbn`, the SAME function the normal-map path uses
    // rather than a second copy of that arithmetic (see surface.glsl).
    const uint4 toon_lanes2 = toon_lanes2_at(heap_slots_toon_lanes, material_index);
    float3 highlight_normal = n;
    float3 hair_bitangent = float3(0.0, 1.0, 0.0); // the smooth frame's binormal; the hair LUT blends toward it
    if (toon_lanes2.x != 0u) {
        const float4 packed_normal = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_lanes2.x), heap_sampler_texture, uv);
        const float2 smooth_xy = packed_normal.zw * 2.0 - 1.0;
        const float smooth_z = sqrt(max(1.0e-16, 1.0 - min(1.0, dot(smooth_xy, smooth_xy))));
        const float3 tangent_normal = normalize(float3(smooth_xy, smooth_z));
        float3 tangent_dir = float3(1.0, 0.0, 0.0);
        float3 bitangent_dir = float3(0.0, 1.0, 0.0);
        if (surface_tbn(world_pos, uv, tangent_dir, bitangent_dir)) {
            const float3x3 smooth_frame = float3x3(tangent_dir, bitangent_dir, n);
            highlight_normal = normalize(mul(smooth_frame, tangent_normal));
            hair_bitangent = normalize(mul(smooth_frame, float3(0.0, 1.0, 0.0)));
        }
    }

    // THE VIEW VECTOR AND ITS COSINE, computed HERE rather than in the rim block below where they used to live,
    // and the reason is that the chain needs them three times before that point: the SSS term is a function of the
    // angle to the camera, the environment's reflection is built from the mirror direction, and the highlight
    // needs `NoV` for its `V` term.
    const float3 v = normalize(camera_at(heap_camera_slot).camera_pos - world_pos);
    const float no_v = saturate(dot(n, v));

    // ---- THE MATERIAL'S OWN NUMBERS: the metallic/gloss lane, or THE FAMILY'S ----
    //
    // THE FOUR CHANNELS ARE THE ARTICLE'S `ORM`: metallic, reflectivity, ambient occlusion and smoothness. A LANE
    // OF 0 IS "DO NOT READ" (see `deren::vulkan::toon_slot`) and that is the SKIN's normal case: the game ships no
    // metallic/gloss map for skin at all, so the family's own `roughness` / `reflectivity` are what the shading
    // uses - which is the article's "give the default material's numbers to a material that has no map" written as
    // a family default instead of as a second material.
    float3 orm = float3(0.0, params.reflectivity, 1.0); // no metal, the family's reflectivity, unoccluded
    float smoothness = 1.0 - params.roughness;          // the family's roughness, inverted into the map's domain
    if (toon_lanes.y != 0u) {
        const float4 orm_texel = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_lanes.y), heap_sampler_texture, uv);
        orm = orm_texel.rgb;
        smoothness = orm_texel.w;
    }
    const float metallic = (toon_lanes2.x != 0u) ? 0.0 : orm.x;
    // ---- THE HAIR'S `R` CHANNEL IS NOT METALLIC, and this is the article's own shader saying so ----
    //
    // THE MEASUREMENT THAT MADE THIS A BRANCH RATHER THAN A READING: chen's hair metallic/gloss map has R WHITE on
    // 55% of its texels AS AN IMAGE, and `energy_distribution_metallic = 0.96 - 0.96 * metallic` would zero the
    // hair's DIRECT DIFFUSE wherever it is 1. THE PICTURE MEASURED LESS THAN THE IMAGE SUGGESTED, and both numbers
    // belong here: the change moved 2,226 pixels of a 1080x960 frame (0.21%) with a handful of them strongly - so
    // the region the hair mesh actually samples is mostly R=0, and this branch is a correctness fix for the texels
    // that are not, rather than the rescue the image statistic promised. The article's hair shader uses that channel
    // for something else entirely: it is the WEIGHT between two bitangents
    // (`lerp(hairBiNormal_flatten, hariBinormal, orm.x)`) and the offset in its view direction
    // (`_ViewDirYOffset * (1 - orm.x)`) - a mask over the strands, which is what a channel that is half white on a
    // head of hair looks like.
    //
    // SO THE LANE THAT SAYS "THIS MATERIAL IS A HAIR" (the split normal) IS WHAT SELECTS THE INTERPRETATION, and the
    // `reflectivity` / smoothness channels keep their meaning: those two are the ones the hair's specular reads.
    // THE REFLECTIVITY IS THE SECOND CHANNEL and it is what makes F0 a material property rather than a constant
    // (see the highlight below): on this repository's cloth maps it is a CONSTANT 1.0, which the data said when
    // this lane was wired - so the channel is read for the materials that do use it and costs nothing where it is
    // flat.
    const float reflectivity = orm.y;
    // THE ROUGHNESS IS THE SMOOTHNESS INVERTED, which is the article's own line and not a preference: the map is
    // authored as a gloss channel, and every consumer below - the specular lobe and the environment's mip - is
    // written in roughness.
    const float roughness = 1.0 - smoothness;
    // THE OCCLUSION CARRIES BOTH SOURCES: the map's own baked occlusion (which is what the game ships) and the
    // engine's `s.ao` (which is 1 for every one of these materials, because glTF occlusion is not where the game
    // put its own). Multiplying them means neither has to know about the other.
    const float ao = pow(saturate(orm.z), rig.other_light.w) * ao_engine;

    // ---- THE HAIR'S SHADOW IS PULLED OPEN BY THE METALLIC/GLOSS MAP'S RED CHANNEL ----
    //
    // `ZmdHairToonMain.hlsl:144`, one line and the LAST statement of that shader's shadow block (its `:142-143`
    // are the same `SigmoidSharp`/`_ShadowOffset` chain this port's `calc_shadow` stands in for):
    //     `shadowScene = lerp(1,shadowScene,ormTex_var.x);`
    // The article's hair shader reads that channel THREE times and each use is a different mask over the strands:
    // the smooth normal's weight (`:87`), the bitangent's blend (`:111`) and this one - a mask that OPENS the
    // cast shadow, i.e. `orm.x` 1 leaves the shadow alone and `orm.x` 0 removes it entirely.
    //
    // THE PORT HAD THE FIRST TWO AND NOT THIS ONE. It already declines to read the channel as METALLIC for a
    // material with a split normal (`metallic` above) and uses it as `binormal_lut`'s weight in the Kajiya-Kay
    // block, so this is the missing third consumer rather than a new interpretation of the channel.
    //
    // WHAT IT COSTS ON THIS ASSET, stated because the image statistic is easy to misread: chen's hair
    // metallic/gloss map has R WHITE on 59.6% of its texels AS AN IMAGE (`glb_images.py`), so this is a
    // large-amplitude mask - but the same map's `G` is what the smoothness uses and the strand shell's own area
    // is what decides how much of the frame sees it. MEASURED in the report's item 8.
    //
    // WHY IT IS NOT A FAMILY TEST: the article applies it in the HAIR main, and what makes a material a hair for
    // this port is the split-normal lane (`toon_lanes2.x`), which is also what selects the other two reads of
    // this channel - so `sdf_lane`-style lane ownership is what is used here, not the family id.
    //
    // THE EYE'S FORCED 1.0 STAYS FIRST and is unchanged: the iris is inside the socket, so the engine's shadow
    // map reports it occluded by the face plate in front of it (see the note above), and the article's eye main
    // reads no shadow map at all.
    const float shadow_used = (toon_indices.w != 0u)
        ? 1.0
        : ((toon_lanes2.x != 0u) ? lerp(1.0, shadow, orm.x) : shadow);

    // ---- THE SSS USED TO SIT HERE, and it moved BELOW the face mask ----
    //
    // Its weight is limited by the mask's `.x` / `.y` for a face (the article's own arrangement), and the mask is
    // read at the SDF's mirrored coordinate, which is only known further down. Nothing between here and there reads
    // the albedo it changes: the ramp block is a function of the light term and the camera, and the face mask's own
    // inputs are the light and the head frame.

    // ---- THE FACE SDF LANE: the terminator as a SHAPE rather than as geometry (see toon_sdf_mask) ----
    //
    // THE MASK REPLACES THE COORDINATE, NOT THE COLOUR. It answers "which side of the ramp is this fragment on"
    // and the ramp below still decides what the two sides look like - which is the reference's own composition:
    // its SDF mask becomes the U coordinate of an RD ramp lookup and that lookup's alpha becomes the light/dark
    // weight. So this is a SUBSTITUTION, not a second term to blend with the light term, and a material without
    // the lane keeps the coordinate it always had.
    //
    // THE MASK ANSWERS 0..1 AND THE BAND COORDINATE IS SIGNED, so it is mapped back rather than blended: the
    // ramp's U axis runs from the shadow side to the lit one, and a face's SDF is that same axis measured from
    // the light's angle in the HEAD's frame instead of from the surface normal.
    //
    // A LANE OF 0 MEANS "DO NOT READ", as in every other lane, and it is the ONLY test needed: the sidecar's
    // `_UseSDFLightmap` and the model's lack of an SDF both collapse to it at import (see
    // core::heap_slots::toon_lanes), so this stage cannot tell them apart and does not have to.
    const uint sdf_lane = toon_lanes.x;
    // ---- THE HEAD'S FORWARD AXIS AND THE CAMERA'S SHARE OF IT, read ONCE, ABOVE EVERY READER ----
    //
    // THE ORDER HERE IS A DEPENDENCY RATHER THAN A STYLE. The article's face main reads
    // `cameraForward_faceDir.z` (`ZmdFaceToonMain.hlsl:109`) twice - the SDF's backlight compensation at `:162-164`
    // and the projected-shadow mask at `:196` - and the backlight compensation is itself one of the terms the RAMP
    // coordinate below adds. So the ROOT of the chain is the camera's direction in the head's frame, and both the
    // SDF call here and the ramp further down have to take it from this one place or the two would drift.
    const float3 head_front = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).front.xyz);
    const float head_face_dir_z = dot(head_front, camera_forward);
    // THE BACKLIGHT COMPENSATION'S WEIGHT, which is the same value the ramp's `back_light` is - read here rather
    // than beside the ramp because the SDF takes it as an argument (see `toon_sdf_mask`). A character lit from
    // behind can show the camera nothing but surfaces facing away from the sun, so this term lifts the terminator
    // back across the grazing range instead of letting the whole visible face collapse onto the SDF's dark end.
    const float2 camera_forward_xz = normalize(camera_forward.xz + float2(1e-6, 0.0));
    const float2 light_dir_xz = normalize(l.xz + float2(1e-6, 0.0));
    const float back_light_xz = saturate(-dot(camera_forward_xz, light_dir_xz));
    const float back_light_y = smoothstep(0.0, 1.0, saturate(-abs(camera_forward.y) + 0.75));
    const float back_light = back_light_xz * back_light_y * rig.misc.x;
    float2 face_mask_uv = uv;
    float face_sdf_z = 0.0;
    const float sdf_weight = sdf_lane != 0u ? toon_sdf_mask(uv, sdf_lane, l, head_face_dir_z, back_light, face_mask_uv, face_sdf_z) : 0.0;
    // ---- THE FACE MASK (`_SDFMask`), sampled at the ORIGINAL uv ----
    //
    // THE ARTICLE'S SECOND FACE TEXTURE, and it answers what the SDF cannot: WHERE on the face a term belongs. Its
    // four channels are read by different parts of the chain below - `.x` limits the subsurface, `.y` is the
    // NECK SEAM, `.z` is the projected SHADOW RING (read by nothing here yet) and `.w` the rim's region - and a
    // material with no mask says "no limits" through the default below rather than through a branch at each use.
    //
    // THE COORDINATE IS `i.uv0` AND NOT THE SDF's MIRRORED ONE, which is the article's own arrangement rather
    // than a preference: `ZmdFaceToonMain.hlsl:55` samples `_SdfRefineTex` at `i.uv0` and `:157` samples
    // `_SdfTex` at `float2(sdf_u, sdf_v)`, so the two face textures are read in DIFFERENT spaces on purpose - the
    // atlas holds one face which a light from the other side reads mirrored, while the mask's rings are authored
    // on the face's own uv. The port handed the mirrored coordinate back here (`out_face_uv = sdf_uv`) on the
    // assumption that both were the same space; the article's lines are what say otherwise.
    //
    // THE DEFAULT IS (1, 0, ...): `.x = 1` means "the SSS region is the whole face", `.y = 0` means "no seam, use
    // the SDF's own term everywhere" - which is exactly what this stage did before the lane existed, so a model
    // without a mask is shaded as it was.
    float4 face_mask = float4(1.0, 0.0, 0.0, 0.0);
    if (toon_lanes.z != 0u && sdf_lane != 0u) {
        face_mask = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_lanes.z), heap_sampler_texture, face_mask_uv);
    }
    const float face_no_l = sdf_lane != 0u ? (sdf_weight * 2.0 - 1.0) : no_l;
    // ---- THE PROJECTED SHADOW'S OWN MASK, WHICH A FACE GATES ON ITS SDF ----
    //
    // `ZmdFaceToonMain.hlsl:196-199`, copied line for line:
    //
    //     float shadowArea = MySmoothstep(saturate(-2 * cameraForward_faceDir.z)) * sdfRefineTex_var.z;
    //     shadowArea = max(sdfRefineTex_var.y, shadowArea);
    //     float shadowEffect = lerp( 1, shadowScene, shadowArea);
    //
    // WHY A FACE NEEDS IT AND NO OTHER MATERIAL DOES: the engine's cast shadow arrives per-fragment from a shadow
    // map, and on a face it is the WRONG SHAPE - a lock of hair, a collar or the character's own nose casts a
    // hard-edged, map-resolution stamp across a surface whose whole shading model is a painted distance field. The
    // article's answer is not to filter it but to say WHERE a cast shadow is allowed to land at all: `refine.y` is
    // the mask's neck/geometry ring (1 at the jaw and the neck, 0 over the face plate) and `refine.z` is its
    // shadow ring, and the camera term is the second opinion on the same question - a camera looking at the face
    // from BEHIND (`cameraForward_faceDir.z` negative) sees the ring the shadow belongs to. Where the mask says
    // "this is the face's own plate", `shadowArea` is 0 and `shadowEffect` is 1: the fragment is treated as
    // UNOCCLUDED by the cast shadow whatever the map says.
    //
    // `MySmoothstep(x)` IS THE ARTICLE'S UTILITY (`ZmdToonUtils.hlsl:24-30`): `x*x*(-2x + 3)`, written out rather
    // than called, because this file has no include of that header - and it is NOT HLSL's `smoothstep`, which is
    // why it is spelled out here instead of `smoothstep(0, 1, x)` (the two agree only at the endpoints and at 0.5).
    //
    // THE MASK IS `_SDFMask` AND THAT IS THIS PORT'S IDENTIFICATION, NOT THE ARTICLE'S: the sidecar has no
    // `_SdfRefineTex` row (it carries `_SDFMask` for the face's own mask - see `deren-ab/face_shader_compare.md` §1
    // and §5-1 for the channel-role evidence). `.z` was previously described in this file as "the rim's own
    // normal" and never read; the article's refine `.z` is THIS, the shadow ring, and the rim's normal is the
    // SDF's own B channel (`out_sdf_z` above). One channel, one meaning, and the shadow ring is the one the
    // article names.
    //
    // ---- THIS WAS TRIED ONCE, REVERTED, AND IS NOW KEPT: THE JUDGEMENT CHANGED WITH THE CRITERION ----
    //
    // It first landed on the 5.37-mottle baseline, where it read as a noise REGRESSION (mottle 4.78 -> 5.81,
    // ROI std 41.22 -> 54.27, edges 8669/5938/4222 -> 9209/6974/4808) and was reverted. That measurement was
    // taken BEFORE the face corrections of `remaining_port_spec.md`'s "脸部链按参考对齐" (the SDF window, the
    // ambient, `_AlbedoDarkStrength`, the LUT axes, the mask uv, the rim and the SSS tint), i.e. while the
    // cheek's own terminator was still near-binary, and it was scored on the cheek's local RANGE - a NOISE
    // measure - rather than on what the user then asked about, which is the SUN'S CAST SHADOW BEING TOO STRONG.
    //
    // RE-APPLIED on the current baseline (mottle 2.84) and re-scored on the shadow, camera
    // `-30.3668,-14.3239,0.5,0.0,-0.48,0.0`, cheek ROI x0=560 y0=130 s=300:
    //     ROI mean lum                 54.93 -> 77.41   (+41%)
    //     darkest-quartile mean        30.98 -> 40.02   (+29%)
    //     brightest-quartile mean      87.69 -> 138.56  (+58%)
    //     row-mean shadow profile     45..74 -> 54..90  (the whole cheek lifts; `face_ref_shadow2.py`)
    //     strongest horizontal step     0.99 -> 0.92
    //     mottle                        2.84 -> 4.02    (+41%, the KNOWN cost)
    //     ROI lum std                  26.51 -> 44.28
    //     edges >8 / >16 / >32   8080/5591/2984 -> 9306/6286/3778
    //     frame                    144871 px -> 417079 px, mean|d| 10.35 -> 7.37, max|d| 118
    // and at the STANDARD pose - where the user's complaint does not arise - it is nearly free and slightly
    // BETTER: mottle 11.83 -> 11.87, lum std 27.37 -> 25.97, edges 14065/11226/7360 -> 14045/11155/7300.
    // The report's "太阳阴影强度" section carries the false-colour evidence for why the lift is so large: the
    // cheek's cast shadow was the term suppressing the LIT layer over 85% of the ROI.
    const float shadow_camera = saturate(-2.0 * head_face_dir_z);
    const float shadow_area = max(face_mask.y, shadow_camera * shadow_camera * (-2.0 * shadow_camera + 3.0) * face_mask.z);
    const float shadow_effect = lerp(1.0, shadow_used, shadow_area);
    // ---- THE SKIN'S OCCLUSION IS ITS ALBEDO'S ALPHA, AND IT IS THE ARTICLE'S OWN SOURCE ----
    //
    // `ZmdSkinToonMain.hlsl:154` is one line - `float ao = mainTex_var.w;` - and it is NOT the same source the
    // article's other four shaders use: the base, hair, face and eye shaders all read the occlusion out of the
    // metallic/gloss map (`ormTex_var.b`, `_AoStrength`), while SKIN reads the ALBEDO TEXTURE'S ALPHA. That is
    // why the source has to be a per-family branch here rather than a change to `orm.z`'s read above.
    //
    // THE PORT HAD NO EQUIVALENT SOURCE AT ALL, so this is a substitution rather than a tuning: skin ships no
    // ORM map in this repository's assets (`toon_lanes.y == 0`), so the line above leaves `orm = (0, r, 1)` and
    // the engine's own `s.ao` is 1 for every one of these materials (no glTF `occlusionTexture`) - the skin's
    // `ao` was IDENTICALLY 1.0, and `ao` is not a cosmetic term: it is the "which layer shows" gate
    // (`min_shadowEffect = min(ao, shadowScene, rampColor.w)`, `:156-157`), the edge darkening
    // (`ao_shadow = ao * shadowScene`, `:155`) and the two rims' own `min(ao, shadowScene)`.
    //
    // MEASURED ON THIS ASSET, and the probe had to be built before this line could honestly be written - the
    // task's own rule is that a channel may not be nominated as AO from the armchair. THE CRITERION IS THE
    // ASSET-SIDE PROBE ALONE (`deren-ab/skin_ao_probe.py` / `skin_ao_meshuv.py`, re-run unchanged for this
    // record): `T_actor_chen_body_01_D` (512x512, `baseColorFactor.a = 1`) carries a real mask in alpha -
    // min 0 / max 255, mean 79.56 / std 88.19 over the image, 1.20% of its texels at 255 and 9.84% at 0,
    // with the low values spread through the interior (median distance from the image border 75 texels)
    // rather than hugging a silhouette, and a correlation of -0.005 with the RGB luminance - i.e. it is an
    // occlusion-like map and NOT a UV-occupancy cut. Sampled at the body mesh's own TEXCOORD_0 the 8-bit
    // mean is 93.24 bilinear / 93.33 nearest (flip V: 95.87 / 95.57), i.e. 0.366-0.376 linear.
    //
    // TWO CLAIMS AN EARLIER DRAFT OF THIS COMMENT CARRIED ARE WITHDRAWN, and the record says why: `the mean
    // is 0.46 linear` at that same TEXCOORD_0 is reproduced by NO script in this repository (the two above
    // give 0.366/0.376, and `deren-ab/` contains no "0.46" at all), and `the RENDERED read ... agrees with the
    // stored image to within a level` is CONTRADICTED by the surviving arms `deren-ab/skin_ao_probe_pose.png`
    // / `skin_ao_probe_std.png`: over the character's pixels both are a PLATEAU OF 232 (the R==G==B set's
    // p10/p50/p90 = 232/232/232, mean 228.63 pose / 226.06 std), which is the plateau `skin_ao_meshuv.py`'s
    // own docstring names - and is why this line is judged on the ASSET-SIDE probe and not on the rendered
    // one. See `deren-ab/remaining_port_spec.md`: item 5's number table and "本节新发现" item 3.
    //
    // IT IS READ FROM `s.alpha`, which is the SAME product the probe measured: `surface.glsl:193/201` sets it
    // to `base_color_factor.a * albedo_texture.a`. The article's own `mainTex_var.w` has no factor in it, so
    // this is that value AND the material factor the asset states - and on chen's skin the factor is 1.
    //
    // WHY HERE AND NOT AT `:745`: the line above is the article's `shadowEffect` (`ZmdFaceToonMain.hlsl:199`),
    // which is the FACE's gate and exists only for a material with an SDF lane - skin has none, so shadow_effect
    // is exactly `shadow` for it and the substitution below is the article's own chain. Computing it here also
    // keeps `ao`'s existing definition (the ORM channel and the engine's occlusion) intact for the four
    // families that read it, which is what makes this a SKIN-only change.
    const float ao_used = (family == VR_FAMILY_SKIN) ? albedo_alpha : ao;
    // THE NECK SEAM, and it is the article's own line: `.y` is 0 over the face's own region and 1 where the mesh
    // meets the neck, and the light term is lerped from the SDF's SHAPE to the model's own normal across it. Without
    // it the two shading models meet at a line - the face is shaped by a distance field and the neck beside it by
    // its normals - and the seam shows as a crack down the jaw.
    const float no_l_used = lerp(face_no_l, no_l, face_mask.y);

    // ---- THE FACE'S OWN NORMAL, REBUILT FROM THE SDF ----
    //
    // THE SDF'S THIRD CHANNEL IS A DIRECTION, not a weight: the atlas stores, per texel, how far the face's surface
    // leans to one side at that point, and the article turns that into a world-space normal by expressing
    // `(tilt, ~0, forward)` in the HEAD's own basis. WHY IT IS WORTH THE TROUBLE: a low-poly anime face's normals
    // describe its TESSELLATION, so a rim computed from them breaks into facets along the cheeks and vanishes from
    // the nose - the same reason the terminator comes from the SDF rather than from `dot(N, L)`. The rebuilt normal
    // is smooth by construction, and the rim below is what reads it.
    //
    // IT IS BLENDED BACK TO THE MODEL'S NORMAL BY THE MASK'S `.y`, exactly as the light term is: the two channels
    // answer the same question in the same place, which is what keeps the rim off the neck (where the SDF has
    // nothing to say and the model's own normal is the right one).
    float3 face_normal = n;
    if (sdf_lane != 0u) {
        const float3 face_dir = normalize(float3(face_sdf_z, 6.10351562e-05, 1.0 - abs(face_sdf_z)));
        const float3 head_front = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).front.xyz);
        const float3 head_right = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).right.xyz);
        const float3 head_up = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).up.xyz);
        face_normal = normalize(face_dir.x * head_right + face_dir.y * head_up + face_dir.z * head_front);
        face_normal = normalize(lerp(face_normal, n, face_mask.y));
    }

    // ---- THE SSS, WHICH MOVES THE ALBEDO RATHER THAN ADDING A TERM ----
    //
    // THE ARTICLE'S SIMPLEST POSSIBLE SUBSURFACE: a surface seen at a grazing angle is a surface light passes
    // THROUGH, so it takes the material's scattering colour, and the weight is `1.05 - NoV` raised to a power the
    // material chooses. It is applied to the ALBEDO, BEFORE the layers, because what it stands for is a change in
    // what the surface scatters rather than light arriving from somewhere else - which is why every layer, the F0
    // and the rim below inherit it, and why a cheek or a purple skirt reads as translucent instead of as tinted.
    //
    // THE ALBEDO'S ALPHA IS BOTH ITS WEIGHT AND ITS GATE, and that is the article's own line: `step(albedo.w, 0.99)`
    // switches the term OFF wherever that channel says "this texel is opaque material rather than thin material".
    // THE DATA AGREES WITH THAT READING: the face's alpha is a constant 1 in every material this repository loads,
    // so the FACE gets none of this term on its own - which is why the article gives the face a SECOND gate, the
    // mask's `.x` and `.y` below, and why the face's subsurface has to be requested explicitly.
    //
    // IT SITS HERE, AFTER THE FACE MASK, because the mask is one of its inputs - and before the layers, which read
    // the albedo it has just changed.
    // THE FACE'S OWN LIMIT AND THE FACE'S OWN EXPRESSION, and the two are DIFFERENT from the base's rather than the
    // same one tuned - which the article states by writing a second shader for the face. `.x` scales the term and
    // `.y` blends the head's aspect into it, so a face seen from behind keeps a little scattering on the parts that
    // curve toward the camera; and the result TINTS the albedo (`albedo * lerp(1, sss, area)`) instead of replacing
    // it, because a cheek's scattering is a shift in what the skin scatters rather than a different material. The
    // article's `sss_NoV = saturate(NoV) * 0.85 + 0.15` is its face/skin variant of the base's `1.05 - NoV`.
    //
    // MEASURED, AND WHY THE BASE'S GATE CANNOT BE REUSED HERE: the base switches this term off with
    // `step(albedo.w, 0.99)`, and the face's albedo alpha is a constant 1 in every material this repository loads -
    // so keeping that gate made the whole face branch a no-op (25 pixels of a 1080x960 frame, all of them the neck
    // seam). The mask is what the article gives the face INSTEAD of that gate.
    // ---- THE ARTICLE'S "IS THE HEAD TURNED TOWARD THE CAMERA" GATE, read once, used by the SSS and the rim ----
    //
    // `ZmdFaceToonMain.hlsl:109-110`: the camera's forward axis expressed in the head's OWN basis, then its
    // FORWARD component normalised by its horizontal one -
    //     float3 cameraForward_faceDir = normalize(float3(dot(cameraForward, _FaceRight), dot(cameraForward, _FaceUp), dot(cameraForward, _FaceForward)));
    //     float headFDotCamerF = cameraForward_faceDir.z * rsqrt(dot(cameraForward_faceDir.xz, cameraForward_faceDir.xz));
    // - which is "how much of the camera's direction is the direction the face LOOKS, measured against how far
    // off-axis it is". The article reads it twice, for two different purposes (the face SSS at `:110-113` and the
    // rim at `:310-311`), so it is built here once, next to the SDF's `head_face_dir_z` that it is derived from.
    //
    // THE HORIZONTAL LENGTH IS MEASURED IN THE HEAD'S OWN FRAME and NOT in world space, which the port used to get
    // wrong: `-abs(h)` with `h = dot(head_front, camera_forward)` is the horizontal share only when the camera's
    // residual sits entirely in the head's forward axis, and on a real head basis it does not - the two disagree by
    // up to a factor of `sqrt2`. `rsqrt` of a squared length is written out because the article's `rsqrt` is not
    // available on a `float2` here.
    const float3 camera_forward_face_dir = float3(
        dot(camera_forward, head_frame_at(heap_slots_scene_head + heap_frame_slot).right.xyz),
        dot(camera_forward, head_frame_at(heap_slots_scene_head + heap_frame_slot).up.xyz),
        head_face_dir_z);
    const float head_forward_dot_camera_forward = camera_forward_face_dir.z * rsqrt(max(dot(camera_forward_face_dir.xz, camera_forward_face_dir.xz), 1e-8));
    const float head_aspect = saturate(head_forward_dot_camera_forward + 0.5);
    if (sdf_lane != 0u) {
        const float face_no_v = saturate(no_v) * 0.85 + 0.15;
        const float sss_region = lerp(head_aspect, 1.0, face_mask.y) * face_mask.x;
        const float face_area = saturate(params.sss_strength * sss_region * (1.0 - face_no_v));
        // `_SSSColor` IS (1,1,1) ON THE FACE IN THE ARTICLE, so its `lerp(float3(1), _SSSColor, sss_area)` is the
        // IDENTITY and the article's face carries NO subsurface tint at all. Two facts say so and both are file
        // evidence rather than inference: the property's own default is
        //     `_SSSColor ("SSSColor", Color) = (1,1,1,1)`   (`MyZmdFaceShader.shader:35`)
        // and there is no sidecar row and no `efColors` entry that overrides it on `M_actor_chen_face_01`
        // (`deren-ab/face_shader_compare.md` §1 and §6-1). The port's `rig.sss.rgb` is `(1.0, 0.80, 0.78)`
        // (`vulkan/primitive/primitive.cppm:199` - the MME pack's `EF_SKIN_SSS_COLOR`, which this port's own
        // `toon_params.slang:209-210` records as coming from MME and NOT from the article) and `main.cpp` never
        // writes the lane, so the face was being tinted warm where the article tints nothing.
        //
        // THE WEIGHT IS SET TO ZERO RATHER THAN THE EXPRESSION BEING DELETED, which is the article's own value
        // written as the number it is: `lerp(1, colour, 0)` is exactly `lerp(1, (1,1,1), area)`, and leaving the
        // line in place is what makes "the article's face has no SSS colour" a statement in the code instead of a
        // missing term. The tint itself is NOT touched here - the SKIN family reads the same lane and the
        // article's skin shader DOES read `_SSSColor` (`ZmdSkinToonMain.hlsl:103`), so its own correction is a
        // separate change on that family's branch.
        albedo *= lerp(float3(1.0, 1.0, 1.0), rig.sss.rgb, 0.0);
    } else if (params.sss_strength > 0.0) {
        // ---- THE SKIN'S OWN SUBSURFACE, WHICH IS A DIFFERENT FORMULA FROM THE BASE'S ----
        //
        // `ZmdSkinToonMain.hlsl:97-103`, copied line for line:
        //     float NoV = dot(normalWS, viewDir);
        //     float sss_NoV = saturate(NoV);
        //     sss_NoV = sss_NoV * 0.85 + 0.15;
        //     float sss_area = saturate(_SSSArea * (1.0 - sss_NoV));
        //     float3 sssColorEffect = lerp(float3(1.0, 1.0, 1.0), _SSSColor, sss_area);
        //     float3 albedo_sssRefine = mainTex_var.rgb * sssColorEffect;
        //
        // FOUR THINGS THIS CHANGES, and each is a line of the article rather than a taste:
        //   1. THE AREA IS LINEAR, NOT A POWER. The branch used to run the BASE shader's MME formula
        //      (`pow(1.05 - NoV, 1 + alpha*_SssPowStrength)`, `ZmdToonMain.hlsl:103-104`), which the SKIN shader
        //      does not use: skin's is `saturate(_SSSArea * (1 - sss_NoV))`, a straight ramp.
        //   2. NO ALPHA GATE. The base has `sss_area = min(0.9, sss_area) * step(mainTex_var.w, 0.99)`, and the
        //      port had carried that gate here - where it is a no-op for a DIFFERENT reason than the article's:
        //      the face's albedo alpha is a constant 1 in every material this repository loads, so the gate
        //      switched the term off everywhere. The article's skin has no gate and no `min(0.9)`.
        //   3. IT TINTS RATHER THAN REPLACES. `albedo * lerp(1, colour, area)` instead of `lerp(albedo, colour,
        //      area)`: the two agree at area 0 and differ by a factor where the colour is dark, and the article's
        //      form is the one the skin shader writes.
        //   4. `_SSSColor` DOES NOT REACH THIS GPU, AND THAT IS WHY THE FACTOR IS WHITE. The property is declared
        //      `_SSSColor ("SSSColor", Color) = (1,1,1,1)` (`MyZmdSkinToonShader.shader:...`) and there is NO
        //      sidecar row and no `efColors` entry for it on any of chen's seven materials
        //      (`deren-ab/body_shader_compare.md`), so the article's own value IS white and its `lerp` is the
        //      identity. The port's `rig.sss.rgb` is `(1.0, 0.80, 0.78)` - the MME pack's `EF_SKIN_SSS_COLOR`
        //      (`vulkan/primitive/primitive.cppm:199`, and `toon_params.slang` records it as coming from MME and
        //      NOT from the article) - and it is deliberately NOT used here: inventing a colour for a property the
        //      assets never state is exactly what "port the article, do not invent" forbids. The zero weight below
        //      IS the article's default written out, and the line stays so that a future asset that DOES state the
        //      property has somewhere to land.
        const float sss_no_v = saturate(no_v) * 0.85 + 0.15;
        const float sss_area = saturate(params.sss_strength * (1.0 - sss_no_v));
        albedo *= lerp(float3(1.0, 1.0, 1.0), rig.sss.rgb, 0.0 /* the article's `_SSSColor` = white */);
    } else {
        // ---- EVERY OTHER FAMILY'S ARTICLE SHADER HAS NO SUBSURFACE TERM AT ALL ----
        //
        // `ZmdHairToonMain.hlsl` and `ZmdEyeToonMain.hlsl` contain no `sss`/`SSS` read whatsoever (grep: 0 hits
        // in each), and the BASE shader's own term (`ZmdToonMain.hlsl:105`) is gated by `_IsNeedSss` with a
        // different formula. This branch is what a family whose article shader states nothing gets, and what it
        // states is NOTHING: `lerp(1, colour, 0)`. Before this change the port ran the base's `pow(1.05 - NoV, …)`
        // on hair and eye as well, i.e. it gave two materials a subsurface term their own shaders do not have.
        albedo *= 1.0;
    }

    // ---- THE RAMP: the article's own read, which answers TWO questions from one texture ----
    //
    // THE GAME'S RAMP IS NOT A QUANTIZER, which is what this stage assumed before and what a baked unit step can
    // express. Its U AXIS IS THE LIGHT TERM, its RGB is a COLOUR mapping along that axis (dark warm brown to white
    // on the body ramp this repository carries), and its ALPHA is a second mapping of the same axis - a LIGHTNESS,
    // 0 at the dark end and 1 at the lit one. So one fetch gives the shading two of its three variables, and the
    // second fetch - at the surface's angle to the CAMERA rather than to the sun - gives it the third.
    //
    // THE BAND COORDINATE IS THE PHYSICAL LIGHT TERM WITH THE ARTICLE'S BACKLIGHT COMPENSATION ON TOP OF IT, and
    // the compensation is the fix for a specific collapse: a character lit from behind can show the camera nothing
    // but surfaces facing away from the sun, so the ramp reads its dark end everywhere and the silhouette flattens
    // into one shape. The correction depends on the CAMERA's yaw against the light and on how far the camera is
    // from horizontal, so it appears where the problem does - a camera looking along the light at a level pitch -
    // and vanishes for a top-down or a side-on shot, where the dark side is information rather than a loss.
    // ---- THE CAMERA'S FORWARD AXIS IN THE HEAD'S FRAME WAS READ ABOVE, WITH THE SDF ----
    //
    // `head_face_dir_z` IS THE ARTICLE'S `cameraForward_faceDir.z` (`ZmdFaceToonMain.hlsl:109`), and it is read
    // ONCE, above the SDF call, because the article's face main reads the same direction twice for two different
    // jobs: the SDF's backlight compensation (`:162-164`) and the projected-shadow mask (`:196`). `back_light` is
    // the same value the SDF took as an argument. Both are the ROOT of the chain this block finishes, not inputs
    // invented here.
    //
    // THE RAMP IS THE ARTICLE'S OWN READ, WHICH ANSWERS TWO QUESTIONS FROM ONE TEXTURE, and the article's own
    // backlight compensation rides on the light term below - `sdf_backLight` on the SDF side and this same
    // `back_light` here are the two halves of its `faceNoL`.

    // `0.5 - 0.5 * NoL * NoL` is the article's curve rather than `0.5 * NoL + 0.5`, and the difference is what the
    // compensation is multiplied by: this one is ZERO at both ends of the light term, so the correction can only
    // move a surface that is neither fully lit nor fully shadowed - which is exactly the grazing range a backlit
    // character's visible surfaces live in.
    const float ramp_curve = 0.5 - 0.5 * no_l_used * no_l_used;
    const float ramp_u = saturate((ramp_curve * back_light + no_l_used) * 0.5 + 0.5);

    // THE SECOND READ, indexed by the CAMERA angle: the article's `_NoFStrength` and `_NoFPowStrength` shape it,
    // and what it buys is the edge darkening that a cel drawing gets from its linework - a limb darkens against
    // the body it overlaps without a second texture or a depth test. IT READS `shading_normal` rather than `n` for
    // the article's own reason: its eye takes BOTH reads off the cornea's normal, so the eye's edge term rises and
    // falls with the hemisphere as well - a flat quad's `NoF` is one value, which is the same flatness the ramp had.
    const float no_f = saturate(dot(shading_normal, camera_forward) * 0.5 + 0.5);

    float3 ramp_colour;
    float ramp_weight;
    float no_f_weight;
    if (toon_indices.x != 0u) {
        ramp_colour = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_indices.x), heap_sampler_texture, float2(ramp_u, character_ramp_band_v)).rgb;
        ramp_weight = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_indices.x), heap_sampler_texture, float2(ramp_u, character_ramp_band_v)).w;
        no_f_weight = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_indices.x), heap_sampler_texture, float2(no_f, character_ramp_band_v)).w;
    } else {
        // THE PROCEDURAL BRANCH, which is what a material with no ramp - or an artist who switched it off - gets.
        // The family's tint stands in for the ramp's COLOUR and its Hermite step for the ramp's ALPHA, so the two
        // branches differ in WHERE the numbers come from and in nothing else - which is what keeps the family
        // table's thresholds meaningful: they are what a character looks like BEFORE its ramps arrive.
        const float step = smoothstep(params.center - params.softness, params.center + params.softness, ramp_u);
        ramp_colour = lerp(params.shadow_tint, float3(1.0, 1.0, 1.0), step);
        ramp_weight = step;
        no_f_weight = step;
    }
    no_f_weight = pow(saturate(no_f_weight * rig.misc.y), rig.misc.z);
    // `_RampColorNoLStrength` scales the ramp's LIGHTNESS lane, and it is the article's only control over how far
    // the material's own ramp may move the layer weights - the colour lane has its own (`ramp_colour_strength`,
    // which is per family because the references carry it per material). Scaling it HERE rather than at each use
    // is the article's own order: it multiplies the sampled alpha once, before the weights are built from it.
    ramp_weight *= rig.misc.w;

    // ---- THE EMOTION TRICK: AN EXPRESSION PAINTED OVER THE ALBEDO ----
    //
    // THE ARTICLE'S SECOND FACE TEXTURE (`_EmotionMap`) is a 2x2 ATLAS of four eyebrow-and-mouth paintings, and the
    // expression index becomes a CELL rather than a blend: `frac(t)` picks the column, `floor(t) * 0.5` the row
    // (with the index halved first, so 0..3 lands on the four cells), and the mesh's OWN uv at HALF SCALE reads
    // inside that cell - the atlas is laid out over the face's uv space, so `uv * 0.5` is what puts the face into
    // one quadrant. Its RGB REPLACES the albedo where the cell's alpha says so, which is how a face is given an
    // expression without a morph target or a second material.
    //
    // THE INDEX IS NOT IN THE MATERIAL FILE, which is a fact about the game rather than a gap in this port: the
    // expression is chosen by the character's animation/script system, and no `_TrickType` exists in any sidecar.
    // It comes from `[toon] emotion_type`, whose default of 0 leaves every earlier capture where it was.
    if (toon_lanes.w != 0u && rig.other_colour.w > 0.0) {
        const float emoji_type = rig.other_colour.w * 0.5;
        const float2 emoji_uv = float2(frac(abs(emoji_type)), floor(emoji_type) * 0.5);
        const float4 trick = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_lanes.w), heap_sampler_texture, uv * 0.5 + emoji_uv);
        albedo = lerp(albedo, trick.rgb, trick.w);
    }

    // ---- THE EYE'S TWO REGION COLOURS, WHICH THE ARTICLE APPLIES TO THE LIT LAYER ----
    //
    // TWO WEIGHTS, TWO COLOURS, ONE PRODUCT: `eyeCenterArea` (the uv-derived zone the cornea highlight below also
    // uses) carries one colour, and the ALBEDO'S OWN ALPHA carries the other over whatever the material marked -
    // the same channel the base's SSS reads, used here for a different statement about the same surface.
    //
    // THEY MULTIPLY THE **LIT LAYER** AND NOT THE ALBEDO, which is a correction the face close-up forced rather
    // than a tidy-up: the article's own line is `lerp(dark, mainDiffuseColor_Light * trickAlbedo, rampNoL)`, so
    // the tint reaches the surface only where the surface is LIT. Multiplying the albedo instead - what this port
    // did - sends the same tint into the dark layer, the ramp's colour and the ambient, so the eye's SHADOWED side
    // comes out tinted where the reference tints nothing. Measured on chen's iris, whose rendered colour was pulled
    // to `_EyeHighLightColor`'s own hue over the whole eye rather than over its corners: the render's iris reads
    // B/G 1.081 against that colour's 1.080, while the iris TEXTURE is 0.98 (a warm brown) and the eye's ramp
    // (`T_actor_common_body_01_RD`, dark end (183,121,94)) is warm too.
    //
    // THE STRENGTH IS THE ARTICLE'S `* 2.5`, KEPT AS THE ARTICLE WRITES IT. An earlier version of this note held it
    // at 1.0 on the argument that 2.5 compensates for the article's Unity light units while this engine's sun
    // carries ~7.5x the radiance - AND THAT ARGUMENT WAS WRONG, which is worth recording because the distinction is
    // the whole question: this expression scales a COLOUR RATIO (`lerp(1, colour * 2.5, w)`, a number between 1 and
    // the material's own colour), not a light magnitude, so the ratio transfers between engines unchanged. What
    // needed a bridge was the hair LUT's `/PI`, and that reconciled two RADIANCE scales - a different kind of
    // difference. The wrong argument is also why the constant looked inert for two rounds: it was invisible because
    // the LAYER it scales had zero weight, which `shadow` now explains.
    //
    // MEASURED, ONCE THE EYE'S SHADOW GATE WAS FIXED: 1.0 against 2.5 differs by 8530 px / 0.82% of the face
    // close-up with max |d| 33, and by 63 px on the standard frame, 2.5 being the brighter of the two - the tint's
    // colours are all above 1 (chen's iris is (2.40,1.89,2.04) and (2.43,2.05,1.86)), so scaling them up lifts the
    // lower lid's scattering and the corner highlight the article's two weights select.
    //
    // THE ZONE AND THE TWO COLOURS ARE READ ABOVE (`eye_uv` / `eye_centre_area`, which the shading normal needs) and
    // the colours come from the MATERIAL'S OWN `color` ROWS (see the read at the top of this function) - which is
    // what makes them per-material rather than the eye FAMILY's reading of one character's iris. They are GATED ON
    // THE MATCAP LANE, which is the measurement talking rather than a preference: a BROW material carries
    // `_EyeHighLightColor` too (chen's is (2,2,2)) and applying "the eye's highlight tint" to an eyebrow is a
    // 130-pixel error measured the moment the colours moved to their real source. `_UseMatcap` is what separates the
    // two in the game's own data - 1.0 on the iris, 0.0 on the brow - and it is the same lane the eye's highlight
    // below needs, so the two uses agree by construction.
    // THE ZONE IS THE ARTICLE'S, EXACTLY AS IT WRITES IT, AND A ROUND OF THIS PORT DEPARTED FROM IT AND CAME BACK.
    // The departure was mine and it is worth recording as a warning: the article's `eyeCenterArea` covers the region
    // OUTSIDE the inscribed circle (`step(0.25, |uv|^2)`), so the tint lands on the eye's CORNERS, and a measurement
    // showed the iris itself staying dark, which invited flipping the mask so the ~2.4x colour reached the iris
    // (it brightened it by ~46/255). THAT WAS THE PORT INVENTING A MECHANISM: the article's mask is what it is, the
    // materials were authored against it, and a port that "fixes" a formula because a reference image looks brighter
    // has stopped porting. Reverted to `eye_centre_area`; whatever the eye's brightness is, it is now the article's
    // own composition of terms rather than one of them bent to fit a picture.
    float3 eye_trick = float3(1.0, 1.0, 1.0);
    if (toon_indices.w != 0u) {
        eye_trick = lerp(float3(1.0, 1.0, 1.0), eye_highlight_colour * character_eye_trick_scale, eye_centre_area) *
                    lerp(float3(1.0, 1.0, 1.0), eye_scattering_colour * character_eye_trick_scale, albedo_alpha);
    }

    // ---- THE THREE LAYERS, and the article's weights between them ----
    //
    // THE SHADOW AND THE OCCLUSION ARE GATES ON WHICH LAYER SHOWS, not extinction on the result, and the two ways
    // they combine below are different on purpose: a MIN where the terms are alternative statements about whether
    // light arrives at all (a crevice and a cast shadow are not additive - either one makes the fragment dark),
    // and a PRODUCT where both are required (the edge darkening wants the surface in shadow AND turned away).
    const float ao_shadow = ao_used * shadow_effect;
#if defined(VR_GOO_TOON_CHAIN)
    // THE FACE GETS THE ARTICLE'S GSBA GATE ON TOP OF THE THREE-WAY MIN (v2 spec §0.2/§1.4, P2 = the whole gate).
    //
    // The reference records the face's self-shadow as `S = saturate(SmoothStep(GSBA, 1.0, RampAlpha))`, whose floor
    // is `S(0) = 0.6483564136266021` - the shadow may take at most 35% - and that `S` acts on a quantity where 0 is
    // fully dark and 1 is fully lit.  The only such quantity on this path is the gate that feeds the `lerp` below,
    // so the gate itself is remapped instead of multiplying the finished colour: `S <= 1`, so multiplying the
    // result would only ever darken the face, which is the opposite of what the reference's floor states (v1's S1
    // landed that way and measured a net -3.4 level drop).  `S(x) > x` for every `x` in `[0, 1)` and `S(1) = 1`
    // (`|S'| <= 0.5996 < 1`, so `S - x` is strictly decreasing and ends at 0) - therefore this can only hold a
    // fragment at its current brightness or push it toward the lit layer, never darken it.
    //
    // Lane 12 is `_GooDiffuseB`, the same key the Goo face arm reads at `:2258`.  Its absent value is the -1000
    // sentinel a material with no such row carries, which fails `> goo_lane_absent_threshold` and so falls back to
    // `goo_gsba_default` - the article's own group default, exactly as the Goo arm does.  Re-reading it here costs
    // nothing at runtime: it is the same 64-byte colour line the tail of this function already fetches.
    //
    // The `#else` branch is the shipped line, verbatim: the three stages that include this file WITHOUT the Goo
    // define (`character_forward`, `outline`, and the rest) compile the original text and keep their bytes.
    const float4 face_diffuse_b_v2 = toon_colour_at(heap_slots_toon_colours, colour_base + 12u);
    const float face_gsba_v2 = face_diffuse_b_v2.x > goo_lane_absent_threshold ? face_diffuse_b_v2.x : goo_gsba_default;
    const float min_shadow_effect_article = min(min(ao_used, shadow_effect), ramp_weight);
    const float min_shadow_effect = (family == VR_FAMILY_FACE)
        ? saturate(goo_smoothstep(face_gsba_v2, 1.0, min_shadow_effect_article))
        : min_shadow_effect_article;
#else
    const float min_shadow_effect = min(min(ao_used, shadow_effect), ramp_weight);
#endif
    const float ao_shadow_no_f = ao_shadow * no_f_weight;

    // THREE ALBEDOS RATHER THAN THREE MULTIPLIERS, because a toon character's dark side is a HUE shift as much as
    // a darkening: the lit layer is the albedo, the dark one is that albedo scaled and pulled toward its own
    // brightness, and the darkest is the dark layer scaled again.
    const float energy_distribution_metallic = 0.96 - 0.96 * metallic;
    // THE LIT LAYER IS WHERE THE EYE'S TWO TRICK COLOURS GO (see the block that builds `eye_trick`), which is the
    // article's own placement: `mainDiffuseColor_Light * trickAlbedo`. For every other material `eye_trick` is
    // exactly 1.0, so this multiply is bit-exact where there is no eye.
    const float3 light_colour = albedo * energy_distribution_metallic * eye_trick;
    // THE DARK SIDE'S COLOUR COMES FROM THE MATERIAL'S OWN LUT WHEN IT HAS ONE, and NOTHING ELSE IS MULTIPLIED
    // INTO IT - which is a correction rather than a detail, and the frames are what found it. The family's
    // `shadow_tint` used to ride on top (`* params.shadow_tint`), which was right while this lane read the
    // IDENTITY BAKE: with the bake the tint WAS the dark side's colour, and a tint of white left the albedo alone.
    // The game's palette is not an identity - it is a stylised dark-side colour, saturated teal and violet on the
    // skin and cloth palettes this repository loads - so multiplying a cool family tint into it tinted the artist's
    // choice a SECOND time and dragged every cloth's shadow toward the tint's hue. THE ARTICLE'S OWN EXPRESSION is
    // `lerp(lutDark, authoredBright, weight)` with `lutDark` straight out of the LUT, and that is what this is.
    float3 dark_base;
    if (toon_indices.y != 0u && params.lut_strength > 0.0) {
        // `_AlbedoDarkStrength` IS MULTIPLIED IN HERE TOO, and it is the article's own line rather than a
        // symmetry: `ZmdFaceToonMain.hlsl:145` is
        //     `albedo_lutRefine * energyDistribution_metallic * _AlbedoDarkStrength`
        // and its skin twin says the same (`ZmdSkinToonMain.hlsl:126`). The port had the family's
        // `albedo_dark_strength` (0.55 on the face, from the game's `_ShadowColorBrightness`) only on the
        // NON-LUT branch, so a LUT family - the face and skin both - read its dark layer about 1/0.55 = 1.8x
        // brighter than the article's. MEASURED: the cheek's dark side drops by ~8/255 and the face's mean
        // luminance by ~3.9 while the artifact measures improve slightly; see `remaining_port_spec.md`
        // "脸部链按参考对齐".
        dark_base = toon_shadow_lut(albedo, toon_indices.y) * params.albedo_dark_strength;
    } else {
        dark_base = albedo * params.albedo_dark_strength;
        dark_base = lerp(toon_luminance(dark_base).xxx, dark_base, params.albedo_dark_saturation);
    }
    const float3 dark_colour = dark_base * energy_distribution_metallic;
    // THE ARTICLE'S "DARK IN DARK" IS A SATURATION MOVE, NOT A DARKER COLOUR: it takes the dark layer at 65% and
    // then pushes it 20% FURTHER from its own luminance, so the deepest region reads as a more saturated version
    // of the dark side rather than as the same hue turned down. That is what makes the third layer visible at all
    // - a plain multiplier is indistinguishable from a stronger shadow.
    const float3 dark_attention = dark_colour * params.dark_in_dark;
    const float3 dark_in_dark = lerp(toon_luminance(dark_attention).xxx, dark_attention, 1.2);

    // THE ARTICLE'S COMPOSITION, in its own order: the dark side lerps between the DARK layer and the DARKEST one
    // by the edge/ramp weight, that result lerps toward the lit colour by the shadow gate, and the ramp's COLOUR
    // is applied last.
    const float3 dark_lerped = lerp(dark_in_dark, dark_colour, saturate(ao_shadow_no_f + ramp_weight));
    const float3 brdf_base = lerp(dark_lerped, light_colour, min_shadow_effect);
    // THE RAMP'S COLOUR IS A TINT, NOT A BRIGHTNESS, and the article gets that from the ramp's own SATURATION:
    // `max - min` of its channels measures how coloured the entry is, so a grey ramp entry leaves the surface
    // alone while a coloured one tints it. Without this the ramp's white end would brighten every lit surface,
    // which is the difference between a colour mapping and a light.
    const float ramp_max = max(max(ramp_colour.r, ramp_colour.g), ramp_colour.b);
    const float ramp_min = min(min(ramp_colour.r, ramp_colour.g), ramp_colour.b);
    const float ramp_saturation = ramp_max - ramp_min;
    const float3 ramp_effect = ramp_colour * ramp_saturation + 1.0 - ramp_saturation;
    const float3 brdf_ramped = brdf_base * ramp_effect;
    // `rampColor_control` is the article's guard on top of that: the ramp multiplies into a value that already
    // carries the albedo, so a bright entry would brighten as well as tint unless the result is held to the
    // brightness it started with. ITS OWN UPPER BOUND IS 1.5 AND NOT 1.0, which is the part worth keeping: a
    // saturated ramp entry multiplies the surface DARK, and the control is allowed to give up to half of that
    // back, so the ramp tints the shading instead of replacing it. Clamping to 1.0 would turn the ramp's colour
    // into a second shadow term - which is exactly the "ramp as the主体" arrangement this port is replacing.
    const float ramp_control = clamp(toon_luminance(brdf_base) / max(0.01, toon_luminance(brdf_ramped)), 0.0, 1.5);
    const float3 brdf_final = lerp(brdf_base, brdf_ramped * ramp_control, params.ramp_colour_strength);

    // ---- THE LIGHT RIG: TWO STATES OF THE SUN, AND A HEAD LIGHT THAT IS ALWAYS THERE ----
    //
    // THIS IS THE ARTICLE'S CENTRAL MECHANISM, and it is what a single directional light cannot reproduce: the
    // sun's strength follows the time of day and the weather, and simply turning it DOWN loses the DISTRIBUTION
    // that goes with it - a dimmed sun still draws its own terminator across the character, which is wrong in
    // exactly the situation the game cares about (a character in a corner, lit by what is around it). So the
    // strength becomes a BLEND between two whole shading outcomes: at `_DayStrength` 1 the sun carries the
    // character with the head light supplementing it, and at 0 the sun's DIRECT term is gone - no light
    // direction, no terminator - and the head light carries it.
    //
    // THE HEAD LIGHT IS A RIGGED LIGHT RATHER THAN A PHYSICAL ONE: it comes from straight above (`(0,1,0)`) and
    // its term is shaped by hand - an offset before the saturate, a strength and a second offset after - because
    // what it stands for is not a lamp but the fill a character artist expects on a figure with no GI. Its COLOUR
    // is the shadow side's, lerped toward white by the same `min_shadow_effect` the layers used, so it warms the
    // character exactly where nothing else reaches.
    //
    // ---- THE LIGHT, CONSTRUCTED THE AUTHOR'S WAY ----
    //
    // THE PORT USED TO TAKE THE ENGINE'S RADIANCE STRAIGHT (`vec3(7.5 * sun_intensity)`, the same expression
    // `shade_surface` uses) and then patch the consequence with a `/ PI` on the diffuse. Read against the author's
    // own repository (`github.com/qiudashu233/MyZmdShaders`, commit 6507457), his shaders do neither:
    //
    //   ZmdToonMain.hlsl / ZmdEyeToonMain.hlsl, line for line:
    //     float3 mainLightColor = _MainLightColor;
    //     float  mainLightIntensity = max(0.001, (0.299*r + 0.587*g + 0.114*b));
    //     mainLightColor = mainLightColor / mainLightIntensity;                 // unit-LUMINANCE colour
    //     ...
    //     float3 mainLightColor_new = lerp(mainLightColorStrength.xxx, mainLightColor, w) * mainLightIntensity;
    //     float3 mainLightColor_final = lerp(otherLightResult_day0, mainLightColor_new + otherLightResult_day1,
    //                                        _DayStrength);
    //   and `mainDiffuseResult = mainLightColor_final * mainDiffuseBrdf_final` - NO `/ PI` anywhere.
    //
    // SO `mainLightColor_final` IS A COLOUR OF ORDER 1 IN HIS UNITS (a unit-luminance colour times the light's
    // intensity), and the engine's `7.5 * sun_intensity` is not: feeding it in made EVERY term that multiplies it
    // ~7.5x too strong, which the port masked on the diffuse with `/ PI` and not at all on the specular - the
    // measured symptom being `mainSpecularResult` clipping to pure white wherever the eye's matcap W channel is
    // bright (0.4 on the game's ball) instead of reaching the 0.4 the author's light produces.
    //
    // WHAT REPLACES IT IS THEREFORE HIS OWN EXPRESSION, not a new scale factor: this engine's sun is a SCALAR
    // intensity with no colour of its own (`[render] sun_intensity`; there is no `_MainLightColor` to normalize),
    // so the unit-luminance colour IS white and the intensity IS `sun_intensity`. `toon_luminance(main_dark)` is
    // the port's standing-in for his `mainLightColorStrength` (the luminance of the normalized colour times
    // `_MainLightColor_dark`), which collapses to the dark colour's own luminance when that colour is white.
    //
    // `_IsNeedSelfLight` / `_SelfLightIntensity` (his debug white-light toggle) are NOT reproduced: they are a
    // debugging switch, and this renderer has no GUI for them.
    const float main_light_intensity = max(0.001, light_at(heap_light_slot).sun_intensity);
    // THE HEAD LIGHT READS THE SAME NORMAL THE RAMP DOES (`shading_normal`, which is the cornea's on an eye), because
    // the article's eye takes EVERY read off `corneaNormalWS` and this was the last one still on the flat quad's
    // normal - whose `dot(up, n)` is ~0 for an iris facing the camera. MEASURED AND IT MOVES NOTHING: with the eye's
    // `NoL`/`NoF` already on the cornea normal and the two trick colours at full strength, switching this one changes
    // 0 PIXELS at both the standard pose and the face close-up (max |d| 0) - at `day_strength` 1 the head light is a
    // supplement to a sun that already carries the character, so it is not the missing lift. It stays because it is
    // the article's own arrangement and it is provably free; the eye's remaining darkness is in its ALBEDO and in
    // nothing this term reaches (see the note on the eye's net gain: 0.73x of its own albedo, measured).
    // `params.other_light_no_l` IS THE ARTICLE'S ZERO AND IT BELONGS ON THE WHOLE TERM: the author's
    // `otherLightNoL` starts at 0 in every one of his shaders (`ZmdEyeToonMain.hlsl:178`, `ZmdSkinToonMain.hlsl:160`,
    // `ZmdFaceToonMain.hlsl:208`) and the two lines after it only multiply and offset that zero, so his head light
    // is a FLAT fill whose colour the shadow term picks and which no surface normal can modulate. This port drives
    // the same slot from the WORLD UP AXIS - a hemisphere fill the article does not have - and the families whose
    // article shaders this has been read against (the EYE and the SKIN) set this to 0 to get his term back.
    const float head_no_l = (saturate(dot(float3(0.0, 1.0, 0.0), shading_normal) + rig.day.y) * rig.day.z + rig.day.w) * params.other_light_no_l;
    const float3 head_colour = lerp(rig.other_colour.rgb, float3(1.0, 1.0, 1.0), min_shadow_effect) * head_no_l;
    const float3 head_day0 = head_colour * rig.other_light.x;
    const float3 head_day1 = head_colour * rig.other_light.y;
    const float sun_dark = toon_luminance(rig.main_dark.rgb);
    // `params.light_intensity_scale` IS THE ARTICLE'S PER-SHADER SUN FACTOR AND IT GOES HERE, ON THE SUN ONLY.
    // `ZmdSkinToonMain.hlsl:69` multiplies `mainLightIntensity` by `1.3/1.6` and every other face of the article
    // multiplies it by the no-op `1.6/1.6`, so the skin's direct sun - diffuse and specular both, since both read
    // `mainLightColor_final` - is 18.75% dimmer than every other family's. It multiplies `sun_colour` and NOT
    // `main_light_final`, because the article's `mainLightIntensity` reaches the image inside this term and not
    // inside `head_day0`/`head_day1` (the `_OtherLight*` block is built from `_OtherLightStrength`): scaling the
    // shared intensity would dim the head light too, which the article does not do.
    const float3 sun_colour = lerp(sun_dark.xxx, float3(1.0, 1.0, 1.0), min_shadow_effect) * main_light_intensity * params.light_intensity_scale;
    const float3 main_light_final = lerp(head_day0, sun_colour + head_day1, rig.day.x);

    // THE DAY-0 BRDF IS THE ONE WITHOUT A LIGHT DIRECTION, and that is the whole point of the second state: with
    // the sun's direct term gone, the shadow gate that chose between the layers has nothing to say, so the layers
    // are chosen by the EDGE/OCCLUSION weight instead - the surface is shaded by what reaches it from the sky and
    // the fill - while the head light's own NoL does the modelling. At `_DayStrength` 1 the layered brdf stands.
    const float3 brdf_low_light = lerp(dark_in_dark, light_colour, ao_shadow_no_f);
    const float3 brdf_day = lerp(brdf_low_light, brdf_final, rig.day.x);

    // THE SHADOW IS ALREADY INSIDE THESE BRDFS - as the gate that CHOSE the layer rather than as a multiplier on
    // the result (see the weights above). Multiplying here as well is the mistake this comment exists to prevent:
    // it drives every shadowed fragment to zero, leaves only the ambient, and flattens the very split the layers
    // were computing - which reads as "the banding does not work" rather than as a bug.
    //
    // AND NO `/ PI`: the author's line is `mainDiffuseResult = mainLightColor_final * mainDiffuseBrdf_final`, full
    // stop. The `/ PI` that used to be here was this port's bridge for feeding the ENGINE's radiance
    // (`vec3(7.5 * sun_intensity)`) into a chain the author tuned for a colour of order 1; with the light built
    // the author's way above, the bridge has nothing left to bridge and keeping it would make the diffuse
    // `PI` times too dark. It left with `sun_radiance`.
    // NOT `const`, and the reason is step 4: this is the ONE local the reference's diffuse replaces, and the arm
    // that replaces it sits above the composition rather than returning from it (see that arm's note).
    float3 direct = brdf_day * main_light_final;

    // ========================================================================================================
    // ---- STEP 4: THE REFERENCE'S DIRECT-DIFFUSE REPLACEMENT, AND ITS TWO PRE-RIM FACTORS (see the block
    // ---- of constants and the long note at `character_toon_colour_lanes`) ----
    // ========================================================================================================
    //
    // TWO THINGS DECIDE WHETHER THIS ARM RUNS, AND BOTH ARE NEEDED:
    //
    //   * `VR_GOO_TOON_CHAIN` - the define only `shaders/goo_toon.slang` sets (step 2's guard; `outline.slang` and
    //     this file's own entry point leave it undefined). Without it this text is not compiled AT ALL, so the
    //     old pipeline's frame and the outline's hull are the byte-for-byte same code path they were.
    //   * THE LANE (`toon_lanes2_at(...).z != 0`, i.e. `_GooBaseRamp` resolved to a model texture) - which is
    //     the reference's own statement that THIS MATERIAL HAS A RAMP. `RampSelect` is instantiated by ONE group
    //     (`PBRToonBase`, spec §A10: `users == 1`), so a material that did not resolve one - every material of
    //     every other character in this repository, and the whole of chen - keeps the OLD chain's three-layer
    //     diffuse even with `goo_toon = true`. THAT IS DELIBERATE: this step's scope is the seven
    //     `M_actor_laevat_*` base/skin/cloth materials, and `chen` is the A/B's own control - so a control whose
    //     diffuse moved would make the switch untestable rather than merely out of scope.
    //
    // THE FAMILY TEST IS REDUNDANT ON THIS ASSET AND IS WRITTEN ANYWAY, because the lane is an ASSET fact and the
    // scope is a REFERENCE fact: the Goo project gives `PBRToonBase` - the group whose `混合.012` this is - to
    // base, skin and cloth materials, and the OTHER two containers' same-named `混合.012`/`混合.013` are
    // DIFFERENT mechanisms (the spec's §A10: Hair's `混合.013` is an `ADD` where Base's is a `MULTIPLY`). So a
    // hair material that one day resolves a ramp lane must NOT reach this arm - it would be the base group's
    // composition applied to a family whose own is not it.
    //
    // ---- WHAT THE SUM IS, EXACTLY, AND WHY IT IS A HYBRID ----
    //
    // `directLighting_diffuse` IS THE REFERENCE'S. The port's three-layer diffuse (`brdf_day`, above) is what it
    // replaces, and it is multiplied by the port's own `diffuse_specular_blend` so that the port's DIFFUSE-TO-
    // HIGHLIGHT weighting keeps applying exactly as it did - the weight is a ratio, `direct` was already inside
    // it, and dropping it here would make the highlight's share of the sum jump by up to 5x on a material whose
    // albedo alpha is 0. THE OTHER THREE QUARTERS OF `混合.011` ARE THE PORT'S OWN AND ARE NOT TOUCHED:
    // `main_specular` (direct specular), `ambient` (IBL diffuse) and `indir_light_specular` (IBL specular) are
    // added by the caller's sum exactly as they were, so the composed colour is
    //
    //     ( reference diffuse + port specular + port ambient + port IBL specular ) ⊙ gate ⊙ fresnel
    //
    // WHICH IS NOT `混合.012`. It is the reference's REPLACEMENT for the one term this step owns, wearing the
    // reference's own two new factors, on top of a sum that is still three-quarters the port's.
    //
    // ---- `dirLight_lightColor`, AND THE ONE SUBSTITUTION THIS STEP HAD TO MAKE ----
    //
    // The reference's light colour in `directLighting_diffuse` is `组输入.dirLight_lightColor`, a PER-MATERIAL
    // CONSTANT (`[1.0, 0.958005…, 0.958005…]` on `body_01`; spec §4.2). The port has no such row: its light is the
    // rig's (`main_light_final`, `sun_intensity` based - see the note at `:1540-1574` for why that is a colour of
    // order 1 here). THIS ARM USES `main_light_final` AS THE LIGHT AND `dirLight_lightColor` AS ITS TEMPERATURE
    // RATIO, which is the only composition in which the substituted term and the port's own `main_specular` -
    // which the next steps have not replaced yet - live on ONE scale. Putting the reference's absolute constant
    // there instead would leave the diffuse on `dirLight_lightColor * sun_intensity` while the specular sat on
    // `main_light_final ~ O(1) * sun_intensity * 7.5` (this engine's radiance): a hybrid whose two halves differ
    // by 7.5x is not a hybrid, it is a bug with two explanations. THE TEMPERATURE IS KEPT AND IT IS MEASURED, not
    // assumed: on `body_01` the reference's light colour is `(1.0, 0.9580, 0.9580)`, i.e. `dirLight_lightColor /
    // max(dirLight_lightColor)` is a 4.2% blue pull, and THAT RATIO is what multiplies the port's light below.
    // THE SPEC'S §9-U7 (which of the two lights is authoritative) is therefore NOT closed here - it is recorded,
    // and the substitution is stated at the line that makes it.
    const float3 goo_ramp_lane_absent = float3(0.0, 0.0, 0.0);
#if defined(VR_GOO_TOON_CHAIN)
    const uint goo_base_ramp = toon_lanes2_at(heap_slots_toon_lanes, material_index).z;
#else
    const uint goo_base_ramp = 0u;
#endif
#if defined(VR_GOO_TOON_CHAIN)
    // ========================================================================================================
    // ---- STEP 7: THE FACE CONTAINER (see the block of constants and the long note above) ----
    // ========================================================================================================
    //
    // THREE THINGS GATE IT, and each one is a statement the ASSET makes rather than a switch invented here:
    //
    //   * `family == VR_FAMILY_FACE` - the reference gives `PBRToonBaseFace` to exactly one material per character,
    //     which is the same partition the loader's classifier draws (`deren::gltf::toon_family_of`).
    //   * `goo_base_ramp != 0u` - the material states `_GooRampIndex` / `_GooBaseRamp`, i.e. IT IS a material the
    //     FACE container is instantiated for. This is the same lane the base/skin/cloth arm reads and it is why
    //     `face_01` needed a ramp row: without it this arm cannot run at all.
    //   * the SDF lane resolved (`goo_face_block2.w != 0u`) - a FACE material whose sidecar names no SDF, or whose
    //     model does not carry the sheet, keeps the old chain. That is the direction every other lane's absence
    //     falls in, and on this asset the model DOES carry it (three images were injected for this step).
    const uint4 goo_face_block2 = toon_lanes2_at(heap_slots_toon_lanes, material_index);
    const uint4 goo_face_block3 = toon_lanes3_at(heap_slots_toon_lanes, material_index);
    if (family == VR_FAMILY_FACE && goo_base_ramp != 0u && goo_face_block2.w != 0u) {
        // ---- (0) the lanes, each component behind ITS OWN group `interface[]` default ----
        //
        // THE SENTINEL IS `-1000` AND NOT `< 0`, for step 4's measured reason: one of these sockets
        // (`GlobalShadowBrightnessAdjustment`) is `-1.5015480518341064` on THIS material, i.e. an AUTHORED
        // NEGATIVE that a `< 0` test would read as "not stated".
        const float4 face_scalars_a = toon_colour_at(heap_slots_toon_colours, colour_base + 20u);
        const float4 face_scalars_b = toon_colour_at(heap_slots_toon_colours, colour_base + 21u);
        const float3 face_nose_shadow = toon_colour_at(heap_slots_toon_colours, colour_base + 22u).rgb;
        const float3 face_front_r_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 23u).rgb;
        const float4 face_base_ramp_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 10u);
        const float3 face_base_colour = face_base_ramp_lane.rgb;
        const float4 face_diffuse_a = toon_colour_at(heap_slots_toon_colours, colour_base + 11u);
        const float4 face_diffuse_b = toon_colour_at(heap_slots_toon_colours, colour_base + 12u);
        const float4 face_rim_colour_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 7u);
        const float4 face_light_color_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 17u);
        const float4 face_ambient_tint_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 18u);
        const float4 face_specular_color_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 19u);
        // `SDF_RemaphalfLambert_center` / `_sharp` and `CastShadow_center` / `_sharp`: the SAME four sockets step 4
        // reads out of `_GooDiffuseA`, in the same order, from a different container - so they ride the same lane.
        const float face_sdf_center = face_diffuse_a.x > goo_lane_absent_threshold ? face_diffuse_a.x : goo_remap_center_default;
        const float face_sdf_sharp = face_diffuse_a.y > goo_lane_absent_threshold ? face_diffuse_a.y : goo_remap_sharp_default;
        const float face_cast_center = face_diffuse_a.z > goo_lane_absent_threshold ? face_diffuse_a.z : goo_castshadow_center_default;
        const float face_cast_sharp = face_diffuse_a.w > goo_lane_absent_threshold ? face_diffuse_a.w : goo_castshadow_sharp_default;
        const float face_gsba = face_diffuse_b.x > goo_lane_absent_threshold ? face_diffuse_b.x : goo_gsba_default;
        const float face_desaturation_atten = face_diffuse_b.y > goo_lane_absent_threshold ? face_diffuse_b.y : goo_desaturation_default;
        const float face_metallic_max = face_diffuse_b.z > goo_lane_absent_threshold ? face_diffuse_b.z : goo_metallic_max_default;
        const float face_chin_center = face_scalars_a.x > goo_lane_absent_threshold ? face_scalars_a.x : goo_face_chin_center_default;
        const float face_chin_sharp = face_scalars_a.y > goo_lane_absent_threshold ? face_scalars_a.y : goo_face_chin_sharp_default;
        const float face_sphere_strength = (head_frame_at(heap_slots_scene_head + heap_frame_slot).center.w > 0.5 && face_scalars_a.z > goo_lane_absent_threshold) ? face_scalars_a.z : goo_face_sphere_strength_default;
        const float face_smoothness_max = face_scalars_a.w > goo_lane_absent_threshold ? face_scalars_a.w : goo_face_smoothness_max_default;
        const float face_brightness = face_scalars_b.x > goo_lane_absent_threshold ? face_scalars_b.x : goo_face_brightness_default;
        const float face_eye_brightness = face_scalars_b.y > goo_lane_absent_threshold ? face_scalars_b.y : goo_face_eye_brightness_default;
        const float face_front_r_pow = face_scalars_b.z > goo_lane_absent_threshold ? face_scalars_b.z : goo_face_front_r_pow_default;
        const float face_front_r_smo = face_scalars_b.w > goo_lane_absent_threshold ? face_scalars_b.w : goo_face_front_r_smo_default;
        const float3 face_rim_colour = face_rim_colour_lane.rgb;
        const float3 face_light_color = face_light_color_lane.rgb;
        const float3 face_ambient_tint = face_ambient_tint_lane.rgb;
        const float3 face_specular_color = face_specular_color_lane.rgb;

        // ---- (1) `calculateAngel`: the azimuth of the sun around the head's own up axis ----
        //
        // `AngleThreshold ∈ [0,1]` (1 = the sun in front of the face, 0 = directly behind) moves the SDF's
        // threshold left and right; `Flip threshold = sin(azimuth)` decides which way the SDF's u runs. BOTH are
        // read here, and the FLIP has TWO consumers (the SDF's u and `Front transparent red`'s u) which the
        // reference drives from ONE value - two independent switches would be a bug that does not show on this
        // asset and would on the next one.
        const float3 face_head_forward = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).front.xyz);
        const float3 face_head_right = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).right.xyz);
        const float3 face_head_up = normalize(head_frame_at(heap_slots_scene_head + heap_frame_slot).up.xyz);
        const float2 face_angle = goo_face_angle_thresholds(l, face_head_up, face_head_right, face_head_forward);
        const float face_angle_threshold = face_angle.x;
        const float face_flip = face_angle.y > 0.0 ? 1.0 : 0.0; // `运算.004 = GREATER_THAN(Flip threshold, 0.0)`

        // ---- (2) THE TWO MASKS: `cm_M` at THIS fragment's uv, the SDF at the MIRRORED one ----
        //
        // `图像纹理.002`'s `Vector` IS UNCONNECTED IN THE GRAPH, and the parent's ruling for this step is Blender's
        // actual semantics: an unconnected `Vector` on a `ShaderNodeTexImage` samples the DEFAULT UV MAP, i.e. the
        // fragment's own uv - NOT texel (0,0). The spec's §10-A6/A8 read those three sockets as constants and its
        // §11-U8 recorded the doubt; this is the answer, and it is why `cm.G` is a per-pixel LAYER SELECTOR here.
        const float4 face_cm = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + goo_face_block3.x), heap_sampler_texture, uv);
        const float face_cm_r = face_cm.r; // `Front transparent red`'s `D_R`
        const float face_cm_g = face_cm.g; // the layer selector, in three places plus ChinMask
        const float face_cm_a = face_cm.a; // `运算.010`'s gate on the pre-rim
        // `混合.001 = MIX(A = 1 − uv.x, B = uv.x, f = 运算.004)` then `合并 XYZ.002 = (that, uv.y, 0.0)`.
        const float face_sdf_u = lerp(1.0 - uv.x, uv.x, face_flip);
        const float4 face_sdf_texel = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + goo_face_block2.w), heap_sampler_texture, float2(face_sdf_u, uv.y));
        const float face_sdf_average = (face_sdf_texel.r + face_sdf_texel.g) * 0.5; // `(R + G) / 2`, B and A unread

        // ---- (3) `Recalculate normal`: the sphere normal about `headCenter`, blended twice ----
        //
        // `混合.013 = lerp(NormalWS, sphereNormal, sphereNormal_Strength)` and
        // `混合.014 = lerp(混合.013, NormalWS, ChinMask = cm.G)` - so the face plate is shaded by a SPHERE and the
        // chin/neck by the model's own normals. IT IS THE ONE PLACE THE PORT NEEDED NEW DATA (`head_ubo.center`):
        // the sphere normal is `normalize(posWS − headCenter)` and no earlier step had a reason to carry a position.
        const float3 face_to_center = world_pos - head_frame_at(heap_slots_scene_head + heap_frame_slot).center.xyz;
        const float face_center_length = length(face_to_center);
        const float3 face_sphere_normal = face_center_length > 1e-6 ? face_to_center / face_center_length : float3(0.0, 0.0, 0.0);
        const float3 face_recalculated = normalize(lerp(n, face_sphere_normal, saturate(face_sphere_strength)));
        const float3 face_shading_normal = normalize(lerp(face_recalculated, n, saturate(face_cm_g)));
        const float face_ndotl = dot(face_shading_normal, l);
        const float face_ndotv = dot(face_shading_normal, v);
        const float face_half_lambert = 0.5 * face_ndotl + 0.5; // `运算.001 = MULTIPLY_ADD(N·L, 0.5, 0.5)`

        // ---- (4) THE THREE `SigmoidSharp` CALL SITES, and the ramp coordinate they compose ----
        //
        // `混合.005 = lerp(1.0, CastShadows, cm.G)`: THE FACE PLATE WEARS NO CAST SHADOW AT ALL (at `cm.G = 0` the
        // lerp is 1.0, i.e. fully visible) and the chin/neck wears the engine's. That is the reference's own
        // answer for a surface whose shadow is a painted distance field rather than a map, and it is why the
        // article's face chain needed a separate `shadow_area` mask to say the same thing.
        const float face_cast_visibility = lerp(1.0, shadow_used, saturate(face_cm_g));
        const float face_cast_sigmoid = goo_sigmoid_sharp(face_cast_visibility, face_cast_center, face_cast_sharp);
        const float face_sdf_sigmoid = goo_sigmoid_sharp(face_sdf_average, face_angle_threshold + face_sdf_center, face_sdf_sharp);
        const float face_sdf_min = min(face_sdf_sigmoid, shadow_used); // `运算.014 = MINIMUM(群组.011, Cast Shadows)`
        const float face_chin_sigmoid = goo_sigmoid_sharp(face_half_lambert, face_chin_center, face_chin_sharp);
        // ---- the FOURTH `SmoothStep` AND THE SCREEN-SPACE PROXY IT FEEDS ----
        //
        // `运算.029 = MAX(SmoothStep(0.5, 1.0, 映射范围.001(0.5 − 0.5·dot(V, headUp))), 0)` - a "how much am I
        // looking at the face from above/below" ramp - and `混合.022 = MIX(A = 1.0, B = 运算.025, f = 运算.029)`.
        // `运算.025` is the SHADOW PROXY (a scene-depth comparison at a re-projected screen position) and this
        // stage CANNOT take that sample: the depth is its own attachment. It is therefore held at its open value,
        // which makes the MINIMUM that follows inert (`运算.022 = 运算.014`); see `goo_face_shadow_proxy_open`.
        const float face_view_up = dot(v, face_head_up);
        const float face_map_range = saturate((0.5 - 0.5 * face_view_up - 0.5) / (1.0 - 0.5));
        const float face_proxy_factor = max(goo_smoothstep(0.5, 1.0, 1.0 - face_map_range), 0.0);
        const float face_proxy = lerp(1.0, goo_face_shadow_proxy_open, saturate(face_proxy_factor));
        const float face_after_proxy = min(face_proxy, face_sdf_min); // `运算.022` - NOT merged with `混合.002`
        const float face_chin_or_sdf = lerp(face_after_proxy, face_chin_sigmoid, saturate(face_cm_g)); // `混合.002`
        const float face_ramp_u = min(face_cast_sigmoid, face_chin_or_sdf); // `运算.002` -> `合并 XYZ.X`
        const float4 face_ramp_texel = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + goo_base_ramp), heap_sampler_texture, float2(face_ramp_u, 0.5));
        const float3 face_ramp_colour = face_ramp_texel.rgb; // LINEAR: the lane is `_SRGB` for step 4's reason

        // ---- (5) THE ALBEDO, WITH `Front transparent red` FOLDED IN ----
        //
        // `混合 = _D(sRGB)R.G.B ⊙ BaseColor`, `群组.013 = Front transparent red(...)`, and
        // `混合.006 = MIX(A = 混合, B = 混合 ⊙ 群组.013, f = Cast Shadows)` - THE FRONT-RED TINT APPLIES WHERE THE
        // SURFACE IS LIT, which is what "light passes through the cheek" means. `混合.016` is the author's
        // identity multiply and is written as one.
        const float3 face_albedo_tinted = albedo * face_base_colour;
        const float face_front_r_powered = pow(max(face_cm_r, 0.0), max(face_front_r_pow, 0.0));
        const float face_front_r_window = goo_smoothstep(0.0, face_front_r_smo, face_front_r_powered);
        const float3 face_front_r_mixed = lerp(float3(1.0, 1.0, 1.0), face_front_r_colour, face_front_r_window);
        // `Positive attenuation = dot(headForward, V)`, and the MIX's own `clamp_factor` bounds it to [0,1].
        const float face_front_r_atten = saturate(dot(face_head_forward, v));
        const float3 face_front_red = lerp(float3(1.0, 1.0, 1.0), face_front_r_mixed, face_front_r_atten);
        const float3 face_albedo_masked = lerp(face_albedo_tinted, face_albedo_tinted * face_front_red, saturate(shadow_used));
        // `ComputeDiffuseColor(albedo, metallic = MetallicMax)` - the FACE's own metallic input is the SOCKET, not
        // the map (`:: 群组.009.Input_2 <- 组输入.Input_8 = MetallicMax`), and it is `0.0` on this material.
        const float3 face_diffuse_colour = face_albedo_masked * (1.0 - saturate(face_metallic_max));

        // ---- (6) `混合.011`, ALL FOUR TERMS FROM THE REFERENCE ----
        //
        // THE LIGHT IS STILL THE PORT'S, and that is the one substitution this arm makes rather than a port:
        // the reference's `directLighting_diffuse` multiplies `dirLight_lightColor` - a per-material CONSTANT of
        // order 1 - while this engine's light is `main_light_final`. Step 4 made the same substitution for the
        // body and recorded it (its §4 item 2 / the spec's U7), and using the reference's constant here would put
        // the face and the neck beside it on two different scales. THE COLOR IS NOT LOST: it rides
        // `混合.010`/`Vector Math.016` below, which is where the reference puts it for the SPECULAR.
        const float3 face_ndotl_clamped = saturate(face_ndotl);
        const float3 face_direct_diffuse = goo_inverse_pi * face_ramp_colour * face_diffuse_colour * face_light_color * main_light_final;
        // `directLighting_specular`, the FACE's own wiring: `clampedNdotL ⊙ (原 ⊙ F) ⊙ SpecularColor ⊙ 混合.005 ⊙
        // dirLight_lightColor ⊙ directOcclusion(=[1,1,1]) ⊙ (1/reflectivity)`. THREE THINGS DIFFER FROM STEP 5'S
        // BODY EXPRESSION, each the graph's: the shadow factor is `混合.005` (the PRE-sigmoid lerp, NOT
        // `转接点.016`), the face carries the energy compensation `E1 = (1,1,1) + (1/LUT.G - 1) ⊙ F0` where the body
        // omits it (the older shorthand `E1 = 1/reflectivity` holds ONLY at `F0 = 1` - see `goo_debt_a_e1_factor.md`),
        // and `Abs_NdotL` is fed `|L·H|`. On this material all of it is moot: `SmoothnessMax = 1.0` ⇒ `a2 = 0`.
        const float face_clamped_ndotv = max(face_ndotv, goo_fgd_ndotv_floor);
        const float face_perceptual_roughness = 1.0 - saturate(face_smoothness_max);
        const float face_clamped_roughness = face_perceptual_roughness * face_perceptual_roughness;
        const float face_a2 = face_clamped_roughness * face_clamped_roughness;
        const float face_fgd_inv_resolution = 1.0 / 64.0;
        const float2 face_fgd_coord_lut = float2(sqrt(face_clamped_ndotv), face_perceptual_roughness) * (1.0 - face_fgd_inv_resolution) + face_fgd_inv_resolution * 0.5;
        const float4 face_fgd_texel = heap_texel(heap_at<Texture2D<float4>>(heap_slots_goo_fgd_lut), heap_sampler_texture, face_fgd_coord_lut);
        // `ComputeFresnel0(albedo, metallic = MetallicMax)` with the dielectric end step 5 already spelled.
        const float3 face_fresnel0 = lerp(float3(goo_fgd_dielectric_f0), face_albedo_masked, saturate(face_metallic_max));
        const float3 face_specular_fgd = lerp(float3(face_fgd_texel.r), float3(face_fgd_texel.g), face_fresnel0);
        const float face_reflectivity = face_fgd_texel.g;
        const float face_energy_compensation = 1.0 / face_reflectivity - 1.0;
        const float3 face_half_dir = normalize(l + v);
        const float face_ndoth = saturate(dot(face_shading_normal, face_half_dir));
        const float face_ldoth = saturate(dot(l, face_half_dir));
        // ---- `原`, AND THE SECOND COPY OF STEP 11's ARITHMETIC (欠账 (e), 2026-09-30) ----
        //
        // THIS ARM CARRIED THE PRE-STEP-11 EXPRESSION A SECOND TIME, which `deren-ab/goo_step11_armA_result.md` §7
        // item 2 reports and does not fix ("脸 arm 里还有同一段错算术的第二份副本 ... 若脸 arm 将来复活，它的镜面
        // 同样偏暗 1.78 倍且下限仍在 S^2 上"). It is fixed here, and it is now THE SAME ARITHMETIC, TERM FOR TERM,
        // as the body arm's landed block at `goo_dv_s`: `S`, the UN-rooted `|NoL|` half, the ROOTED `NoV` half,
        // their ADD, `MAXIMUM(S^2 * λ, 1.1754899742869237e-35)`, then `1/(2pi) * (a2 / that)`. The four defects the
        // removed copy carried are the body's own pre-step-11 ones: `pow(S, 2)` with a `1e-12` floor on `S^2`
        // alone, a `sqrt` around the half that is NOT rooted, no `λL` half at all, and a `Gv = 1/(1 + λ)` the
        // reference has no node for (its denominator is the ADD of the two halves, with no `+1` in it).
        //
        // ONE TOKEN DIFFERS FROM THE BODY'S LINES, AND IT IS THE REFERENCE'S OWN WIRING: this arm's `Abs_NdotL`
        // slot is fed `|L·H|` (see the note above the `原` consumer), so the `NoL` operand is `face_ldoth` and not
        // `NoL` itself. That substitution is the REMOVED copy's own (`abs(face_ldoth)`) and is not introduced here.
        //
        // IT IS DEAD CODE ON THIS ASSET, TWICE OVER, which is why the step is expected to be pixel-neutral: the
        // gate above needs the SDF lane (`goo_face_block2.w`, lane 11 = `_GooFaceSDF`), the shipped sidecar names no
        // `_GooFaceSDF` row, and the loader prints no resolved `_GooFaceSDF` slot for ANY material while both
        // sibling masks do resolve; and even if that row existed, this material's `_GooFaceScalarsA.w` is `1.0`, so
        // `SmoothnessMax = 1.0`, `a2 = 0`, and BOTH spellings return exactly `+0.0` because the numerator is the zero
        // `a2`. Section `8r4` of `tests/test_goo_toon_math.cpp` pins the text, the same-source property, that neutrality.
        const float face_dv_s = face_ndoth * face_ndoth * (face_a2 - 1.0) + 1.0;
        const float face_dv_lambda_v = abs(face_ldoth) * (face_a2 + (1.0 - face_a2) * face_clamped_ndotv * face_clamped_ndotv);
        const float face_dv_lambda_l = face_clamped_ndotv * sqrt(face_a2 + (1.0 - face_a2) * face_ldoth * face_ldoth);
        const float face_dv_lambda = face_dv_lambda_v + face_dv_lambda_l;   // `运算.012`: an ADD, with NO `+1` in it
        const float face_dv_denominator = max(face_dv_s * face_dv_s * face_dv_lambda, 1.1754899742869237e-35);
        const float face_dv_original = goo_dv_half_inverse_pi * (face_a2 / face_dv_denominator);
        const float face_schlick_x = 1.0 - face_ldoth;
        const float face_schlick_x5 = face_schlick_x * face_schlick_x * face_schlick_x * face_schlick_x * face_schlick_x;
        const float3 face_schlick_f = face_fresnel0 + (1.0 - face_fresnel0) * face_schlick_x5; // f90 = 1.0
        const float3 face_specular_term = face_dv_original * face_schlick_f;
        const float3 face_direct_specular = face_ndotl_clamped * (face_specular_term * face_specular_color) * face_cast_visibility * face_light_color * (face_energy_compensation + 1.0);
        // `IBL_diffuse = 混合.019 ⊙ AmbientLightColorTint ⊙ diffuseFGD ⊙ diffuseColor`, where `混合.019` reads the
        // probe irradiance TWICE - darkened by `亮度 / 对比度(0, −0.4)` and raw - and `cm.G` picks between them.
        const float3 face_probe = irradiance_sample(face_shading_normal);
        const float3 face_ambient = lerp(goo_face_bright_contrast(face_probe, goo_face_ambient_contrast), face_probe, saturate(face_cm_g));
        const float face_diffuse_fgd = face_fgd_texel.b + goo_fgd_diffuse_offset;
        const float3 face_ibl_diffuse = face_ambient * face_ambient_tint * face_diffuse_fgd * face_diffuse_colour;
        // `IBL_specular = specularFGD ⊙ specularFGD Strength ⊙ 能量补偿`. THE FACE CONTAINER HAS NO
        // `specularFGD Strength` SOCKET AT ALL (spec §5.1), so the reference's own `interface[]` default - step 5's
        // `goo_specular_fgd_strength_default`, `1.0` - is the answer and no lane is read.
        const float3 face_ibl_specular = face_specular_fgd * goo_specular_fgd_strength_default * face_energy_compensation;
        const float3 face_lit_sum = face_direct_diffuse + face_direct_specular + face_ibl_diffuse + face_ibl_specular;
        // `混合.004`'s GSBA gate, the SAME node shape step 4 ports for the body and a DIFFERENT material number.
        const float3 face_gated = face_lit_sum * saturate(goo_smoothstep(face_gsba, 1.0, face_ramp_texel.a));

        // ---- (7) THE FACE'S OWN RIM: `混合.014`, WHICH IS MULTIPLICATIVE AND IS THE PRE-RIM ----
        //
        // The three factors are the graph's own, node for node:
        //     `运算.010 = cm.A ⊙ 混合.012`            (the mask's alpha, and the u mirror that follows the light)
        //     `运算.011 = 运算.010 − AngleThreshold`   (the threshold SUBTRACTED, not scaled)
        //     `群组.010 = SmoothStep(0, 0.2, AngleThreshold)`
        //     `群组.014 = SmoothStep(0.8, 1.0, dot(headForward, V))`
        //     `钳制 = clamp(群组.014 · 运算.011 · 群组.010, 0, 1)`, `混合.014 = 钳制 ⊙ Rim_Color`
        // and it is ADDED to the gated sum (the reference's `混合.015`). THAT IS ALSO THE ANSWER TO STEP 2's
        // DEFERRAL: the FACE's rim exists and it is this one; the article's additive rim is NOT added on top of it,
        // because this arm returns before the code that would add it.
        //
        // `mirror(uv.x)` IS THE SAME `face_flip` THE SDF'S u USES (`混合.012 = MIX(A = step(0.5, uv.x),
        // B = 1 − step(0.5, uv.x), f = 运算.004)`), which the spec's §10-A9 calls out as the thing two independent
        // switches would get wrong.
        const float face_mirror_uv = face_flip > 0.0 ? (uv.x > 0.5 ? 0.0 : 1.0) : (uv.x > 0.5 ? 1.0 : 0.0);
        const float face_rim_window = goo_smoothstep(0.0, 0.20000000298023224, face_angle_threshold);
        const float face_rim_threshold = (face_cm_a * face_mirror_uv) - face_angle_threshold;
        const float face_rim_gate = clamp(goo_smoothstep(0.800000011920929, 1.0, dot(face_head_forward, v)) * face_rim_threshold * face_rim_window, 0.0, 1.0);
        const float3 face_pre_rim = face_rim_gate * face_rim_colour;
        // `混合.018 ⊙ 混合.017`: the nose shadow, whose factor is the ALBEDO TEXTURE'S ALPHA (a mix toward white,
        // NOT a multiply - see `toon_colour_lane::goo_face_nose_shadow` for why that distinction is 14% of a face).
        const float3 face_nose_shadow_mix = lerp(face_nose_shadow, float3(1.0, 1.0, 1.0), saturate(albedo_alpha));
        const float3 face_after_rim = (face_pre_rim + face_gated) * face_nose_shadow_mix;
        // `混合.025 = (Lips highlight color ⊙ hl_M.R) ⊙ MAX(ramp_u, 0.4)`. THE MASK FETCH IS NOT MADE because the
        // colour it multiplies is BLACK on the only material this container is given here (see
        // `goo_face_lips_highlight_default`); the arithmetic is written anyway, so a future lane for either half
        // lands on a term that is already in its place.
        const float3 face_lips = (goo_face_lips_highlight_default * 0.0) * max(face_ramp_u, 0.4000000059604645);
        const float3 face_composed = face_after_rim + face_lips;
        // ---- (8) `色相/饱和度/明度` AND THE EMISSION STRENGTH ----
        //
        // `Saturation = clamp(luma(RampColor) + Color desaturation in shaded areas attenuation, 0, 1)`, which is
        // the SAME shape step 6 ports for the body - and this arm is where the FACE gets it, because step 6's arm
        // does not take this family (its gate is base/skin/cloth). THE VALUE THE MATERIAL STATES IS `0.9`, NOT the
        // neutral - so this term was a no-op on the face before this step and is not one now.
        const float face_saturation = saturate(dot(face_ramp_colour, float3(goo_desaturation_luma_r, goo_desaturation_luma_g, goo_desaturation_luma_b)) + face_desaturation_atten);
        const float3 face_desaturated = goo_hsv_desaturate(face_composed, face_saturation);
        // `混合.020 = MIX(Face Final brightness, Eyes white Final brightness, f = CsutmMask.G > 0.5)`, and the
        // factor's source is `图像纹理.004` - NOT `cm_M` (the spec's §2.4 attributes it to `cm_M.G`; the dump's
        // `运算.028 <- 分离 XYZ.006.Y <- 图像纹理.004.Color` says otherwise and the dump wins). It, too, is sampled
        // at this fragment's uv rather than at texel (0,0), per the same ruling as `cm_M`.
        const float face_csumt_g = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + goo_face_block3.y), heap_sampler_texture, uv).g;
        const float face_emission_strength = face_csumt_g > 0.5 ? face_eye_brightness : face_brightness;
        // THE CALLER ADDS `s.emissive` AND APPLIES `out_desaturation`; this pixel has already had BOTH of its own
        // versions applied (the reference's face OUTPUT is an emission node whose colour is the desaturated result
        // and whose strength is the number above), so the out-parameter is the identity and the arm returns.
        out_desaturation = goo_desaturation_neutral;
        return face_desaturated * face_emission_strength;
    }
#endif
    if (goo_base_ramp != 0u && (family == VR_FAMILY_BASE || family == VR_FAMILY_SKIN || family == VR_FAMILY_CLOTH)) {
        // ---- the reference's own lanes, each component behind the group's `interface[]` default ----
        //
        // EVERY `< 0` COMPONENT ANSWERS THE REFERENCE'S OWN DEFAULT rather than a number chosen here, which is the
        // contract `deren::vulkan::toon_colour_lane` states for this lane: the material states nothing for that socket, so
        // what it gets is what a material calling the group without stating it gets in Goo.
        const float4 goo_diffuse_a = toon_colour_at(heap_slots_toon_colours, colour_base + 11u);
        const float4 goo_diffuse_b = toon_colour_at(heap_slots_toon_colours, colour_base + 12u);
        const float4 goo_fresnel_inside = toon_colour_at(heap_slots_toon_colours, colour_base + 13u);
        const float4 goo_fresnel_outside = toon_colour_at(heap_slots_toon_colours, colour_base + 14u);
        const float4 goo_direct_occlusion_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 15u);
        const float3 goo_base_colour = toon_colour_at(heap_slots_toon_colours, colour_base + 10u).rgb;

        const float remap_center = goo_diffuse_a.x > goo_lane_absent_threshold ? goo_diffuse_a.x : goo_remap_center_default;
        const float remap_sharp = goo_diffuse_a.y > goo_lane_absent_threshold ? goo_diffuse_a.y : goo_remap_sharp_default;
        const float cast_center = goo_diffuse_a.z > goo_lane_absent_threshold ? goo_diffuse_a.z : goo_castshadow_center_default;
        const float cast_sharp = goo_diffuse_a.w > goo_lane_absent_threshold ? goo_diffuse_a.w : goo_castshadow_sharp_default;
        const float gsba = goo_diffuse_b.x > goo_lane_absent_threshold ? goo_diffuse_b.x : goo_gsba_default;
        const float metallic_max = goo_diffuse_b.z > goo_lane_absent_threshold ? goo_diffuse_b.z : goo_metallic_max_default;
        // ---- STEP 6 IS THE CONSUMER `_GooDiffuseB.y` HAS BEEN WAITING FOR SINCE STEP 4 ----
        //
        // `toon_colour_lane::goo_diffuse_b`'s own note records the four components as
        // `GlobalShadowBrightnessAdjustment`[x] / `Color desaturation in shaded areas attenuation`[y] /
        // `MetallicMax`[z] / spare[w], and step 4's report (§4 item 6) says `.y` "本步没有消费者，值照记不照用".
        // The reference's value is the material's own: `0.7000000476837158` on `body_01/02`,
        // `0.8999999761581421` on `cloth_01/02/04/05`, `0.8500000238418579` on `cloth_03` (nodes.json). NO NEW LANE
        // AND NO NEW SIDECAR ROW: the number has been in the lane, in the sidecar and in `_GooDiffuseB` since step
        // 4 - what was missing was this read.
        //
        // NEGATIVE IS NOT POSSIBLE ON THIS SOCKET (`interface[]` default `0.0`; all seven materials state
        // `0.7`-`0.9`), so the sentinel test is the same `<= -1000` contract as step 4's rather than a `< 0` test
        // that would read an authored value as "not stated". A material that HAS a base ramp but states no `.y`
        // gets the reference's group default, `0.0` - i.e. `saturation = luma(RampColor)` - and NOT the identity
        // the materials outside this arm keep; that is the difference between "the reference's group was called
        // without this socket" and "the reference's group was not called at all".
        goo_desaturation = goo_diffuse_b.y > goo_lane_absent_threshold ? goo_diffuse_b.y : goo_desaturation_default;
        const float fresnel_smo_l = goo_fresnel_inside.w > goo_lane_absent_threshold ? goo_fresnel_inside.w : goo_fresnel_smo_l_default;
        const float fresnel_smo_h = goo_fresnel_outside.w > goo_lane_absent_threshold ? goo_fresnel_outside.w : goo_fresnel_smo_h_default;
        const float3 fresnel_inside_colour = lerp(float3(1.0, 1.0, 1.0), goo_fresnel_inside.rgb, goo_fresnel_inside.w > goo_lane_absent_threshold ? 1.0 : 0.0);
        const float3 fresnel_outside_colour = lerp(float3(1.0, 1.0, 1.0), goo_fresnel_outside.rgb, goo_fresnel_outside.w > goo_lane_absent_threshold ? 1.0 : 0.0);
        // `directOcclusionColor` NEEDS NO SENTINEL: BLACK IS ITS GROUP DEFAULT AND A REAL VALUE (`[0,0,0,1]` on all seven
        // materials this step covers), so a lane nobody stated and a lane that stated black are the same answer -
        // and that answer is the reference's own (`directOcclusion = lerp(black, white, AO)`).
        const float3 direct_occlusion_colour = goo_direct_occlusion_lane.rgb;

        // ---- `RampUV.x = min(castShadowSigmoid, halfLambertSigmoid)`, with NOTHING clamped (spec §2.1) ----
        //
        // `Cast Shadows` is the ENGINE's own occlusion factor, which is the port's `shadow_used` - the same term
        // the old chain's layer gate reads (`:888`), and on a family with no SDF lane it IS the shadow map's
        // answer. The half-Lambert is `0.5 * N·L + 0.5` off the UNCLAMPED `no_l_used`: the reference's own
        // `运算.001 = MULTIPLY_ADD(N·L, 0.5, 0.5)` takes a raw `dot(L, N)` and `SigmoidSharp` clamps nothing.
        const float cast_shadow_sigmoid = goo_sigmoid_sharp(shadow_used, cast_center, cast_sharp);
        const float remap_half_lambert_sigmoid = goo_sigmoid_sharp(0.5 * no_l_used + 0.5, remap_center, remap_sharp);
        const float ramp_u = min(cast_shadow_sigmoid, remap_half_lambert_sigmoid);
        // `合并 XYZ.Y = 0.5` / `.Z = 0.0` - the reference's two UNCONNECTED constants (spec §2.5). `v` cannot
        // matter on a `256x1` image, which is what the spec's §A9 asserts of all four `_RD` assets; it is written
        // as the reference writes it rather than as this port's `character_ramp_band_v` so that the two readings
        // cannot diverge on the day a ramp that is NOT one pixel tall arrives.
        const float4 ramp_texel = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + goo_base_ramp), heap_sampler_texture, float2(ramp_u, 0.5));
        const float3 ramp_colour_goo = ramp_texel.rgb; // LINEAR: the lane is uploaded `_SRGB` for the reference's reason

        // ---- `directLighting_diffuse` ----
        //
        // `diffuseColor = albedo * BaseColor * (1 - metallic)`: the port's `albedo` carries no `baseColorFactor`
        // (spec §9-U9 - all eleven materials state none), so the reference's `BaseColor` has to arrive from the
        // lane, and `metallic` is the port's own read of the metallic/gloss map - which on this asset is
        // `lerp(0, MetallicMax, _P.R)` with `MetallicMax = 1.0` exactly as the reference's is.
        const float3 goo_diffuse_colour = albedo * goo_base_colour * (1.0 - metallic * metallic_max);
        // `directOcclusion = lerp(directOcclusionColor, [1,1,1], AO)`, and `AO` is the REFERENCE's per-material
        // switch - `Is Skin?` - which is the port's own `ao_used` (`:1056`: the albedo's alpha on skin, the
        // metallic/gloss map's blue elsewhere). THE SAME SWITCH, THE SAME MEANING: spec §4.4 measures
        // `Is Skin? = 1.0` on exactly `body_01`/`body_02` and `0.0` on every cloth, and the port's test is
        // `family == VR_FAMILY_SKIN`, which is the same seven materials.
        const float3 direct_occlusion = lerp(direct_occlusion_colour, float3(1.0, 1.0, 1.0), saturate(ao_used));
        // THE LIGHT IS THE PORT'S, STATED RATHER THAN DRESSED UP: the reference multiplies this term by its own
        // per-material `dirLight_lightColor` (`[1.0, 0.958005…, 0.958005…]` on `body_01`), and NO LANE CARRIES IT -
        // its authority is the spec's U7 (the material's constant against Goo's `Shader Info`) and this step did not
        // settle it. So the term is `main_light_final` and the reference's 4.2% blue pull is NOT applied. An earlier
        // revision of this line read `main_light_final * (cond ? white : white)` under a comment describing a
        // temperature ratio it did not compute; the no-op is gone and the gap is named instead of implied.
        const float3 goo_dir_light = main_light_final;
        const float3 goo_direct_diffuse = goo_inverse_pi * ramp_colour_goo * goo_diffuse_colour * direct_occlusion * goo_dir_light;

        // ---- `混合.004`: the GLOBAL SHADOW BRIGHTNESS gate ----
        //
        // `gate = 钳制.006 = clamp(1.0, 0.0, SmoothStep(GSBA, 1.0, RampAlpha))` (spec §3.1 step 5): the
        // reference's LIFT of the shadow side, per material, driven by the ramp's ALPHA at the SAME `u` the
        // colour was sampled at. A LOWER `GSBA` lifts MORE, which is why the cloth (`-1.8`) starts at `0.708` and
        // the body (`0.0318`) at `0.0`. The outer `clamp` is the reference's own node and the `smoothstep`'s own
        // `[0,1]` makes it a no-op - it is kept as the node writes it.
        const float ramp_alpha = ramp_texel.a;
        const float gsba_gate = saturate(goo_smoothstep(gsba, 1.0, ramp_alpha));

        // ---- `混合.006`: the TOONFRESNEL inner/outer colour ----
        //
        // `lerp(fresnelOutsideColor, fresnelInsideColor, smoothstep(SMO_L, SMO_H, pow(clamp(N·V), ToonfresnelPow)))`.
        // `ToonfresnelPow` IS NOT A LANE OF THIS STEP'S: it already rides `toon_colour_lane::goo_rim_scalars.z` and
        // this reads THAT carrier (`:398`), which is step 2's, so one socket has one carrier. `N·V` is the
        // UNCLAMPED geometry-normal dot the reference's `Vector Math.002` computes (`no_v` here is `saturate`d for
        // the specular lobe above; the reference clamps it itself, so this passes the raw value and clamps once).
        // `ToonfresnelPow` RIDES STEP 2'S LANE (`toon_colour_lane::goo_rim_scalars.z`) and is read from it HERE,
        // in this stage, because step 3 moved the rim - and with it that lane's read - into its own fullscreen pass;
        // an arm of this stage cannot see that pass's locals. One socket still has one carrier; only the reader
        // moved. The lane's other three components belong to the rim and are not read here.
        const float4 goo_rim_scalars = toon_colour_at(heap_slots_toon_colours, colour_base + 8u);
        const float goo_fresnel_pow = goo_rim_scalars.z > goo_lane_absent_threshold ? goo_rim_scalars.z : goo_rim_fresnel_pow_default;
        const float goo_no_v = saturate(dot(n, v));
        const float goo_fresnel_factor = goo_smoothstep(fresnel_smo_l, fresnel_smo_h, pow(goo_no_v, goo_fresnel_pow));
        const float3 toon_fresnel = lerp(fresnel_outside_colour, fresnel_inside_colour, goo_fresnel_factor);

        // ====================================================================================================
        // ---- STEP 5: THE REFERENCE'S DIRECT SPECULAR, IBL DIFFUSE AND IBL SPECULAR ------------------------
        // ====================================================================================================
        //
        // THE REFERENCE'S THREE TERMS (`Arknights: Endfield_PBRToonBase`, spec
        // `deren-ab/goo_step5_specular_spec.md` §2.2/§6/§7.1), each one a read of the SHARED GLOBAL FGD LUT:
        //
        //     specularFGD = lerp(LUT.R, LUT.G, fresnel0)                      # three channels, one per f0 component
        //     diffuseFGD  = LUT.B                                              # §0 item 2 / §9-U1 - see the offset below
        //     reflectivity= LUT.G
        //     energyComp  = 1 / reflectivity - 1
        //     IBL_specular = specularFGD * specularFGD_strength * energyComp
        //     IBL_diffuse  = AmbientLighting * AmbientLightColorTint * diffuseFGD * diffuseColor
        //     direct spec  = clampedNdotL * (D*V) * F_Schlick(f0, f90=1, LdotH) * SpecularColor * CastShadows
        //                    * dirLight_lightColor * directOcclusion
        //
        // ---- THE FGD LUT IS A SHARED GLOBAL TEXTURE AND NOT A LANE (`core::heap_slots::goo_fgd_lut`) ----
        //
        // The reference's `图像纹理` is ONE data-block shared by the Base / Face / Hair containers
        // (`users == 3`, one `ShaderNodeTexImage`), and its sample coordinate is COMPUTED from shading parameters
        // (`sqrt(clampedNdotV)`, `perceptualRoughness`) rather than read from a material's uv, a material's map or
        // a material's parameter. So per-material lanes would be eleven sidecar rows pointing at one file for zero
        // information - the ruling is the spec's §3.4, and it is why this step adds NO lane and NO sidecar row for
        // the LUT itself.
        //
        // ---- THE COORDINATE: `Remap01ToHalfTexelCoord`, AND IT IS THE SPEC'S §A2's FORM AFTER ALL ----
        //
        // `coordLUT = coord * (1 - 1/64) + (1/64) * 0.5`, i.e. `0.0078125 + 0.984375*coord`, the textbook
        // half-texel offset `coord*(1 - 1/N) + 0.5/N` that keeps a `[0,1]` parameter inside the texel CENTRES.
        //
        // THE `* 0.5` IS A FLAG AND NOT A FASHION: `运算.001 = MULTIPLY(运算.Value = 1/64, Value_001 = 0.5
        // (enabled = true), Value_002 = 0.5 (enabled = FALSE))`, and the rule this step's FGD group is read by is
        // that an UNLINKED socket with `enabled = true` PARTICIPATES while one with `enabled = false` does NOT.
        // The sub-group's own name is the proof of the rule rather than the other way round: `Remap01ToHalfTexelCoord`
        // with `运算.001 = 1/64` would be a FULL texel, and this is the HALF-texel remap.
        //
        // AN EARLIER VERSION OF THIS STEP READ `1.0` OFF THAT SOCKET (giving `0.015625 + 0.984375*coord`) and
        // recorded the spec's §A2 table as wrong; the reverse is true - §A2's table and its worked example are both
        // right, and its `1/64` bias is this file's old reading. The two readings agree at NO interior point and
        // differ by half a texel everywhere, which is why the numbers the tests pin had to move with it.
        const float goo_fgd_inv_resolution = 1.0 / 64.0;
        const float goo_fgd_coord_scale = 1.0 - goo_fgd_inv_resolution;
        const float goo_fgd_coord_bias = goo_fgd_inv_resolution * 0.5;
        // `clampedNdotV = max(dot(N, V), 9.999999747378752e-05)` (`运算`), on the SHADING normal, which is what the
        // reference's `转接点.004 <- 运算 <- Vector Math.002 = DOT(转接点.001 = N, 转接点.002 = Incoming)` computes.
        // The shader's own `no_v` is `saturate(dot(n, v))` and REASSIGNED for the cornea above, so this step
        // computes its own from the same normal rather than reading `goo_no_v` (which is the same expression the
        // toon-fresnel factor uses and is `saturate`d, where the reference's floor is `1e-4` and its ceiling is none).
        const float goo_clamped_ndotv = max(dot(n, v), goo_fgd_ndotv_floor);
        // `合并 XYZ = (运算.001 = SQRT(clampedNdotV), 组输入.perceptualRoughness, 0.0)`.
        //
        // `roughness` HERE IS THE REFERENCE'S `perceptualRoughness` AND NOT ITS `roughness`, which is the naming
        // trap the spec's §4.2 warns about twice (its §A4's negative example). The port's `roughness` is
        // `1 - smoothness` (`:1005`), i.e. exactly `PerceptualSmoothnessToPerceptualRoughness`'s output; the
        // reference's `PerceptualRoughnessToRoughness` (`roughness = perceptualRoughness^2`) feeds
        // `clampedRoughness` into the ANISOTROPIC BRDF branch, NOT into the FGD group. `:2013`'s `roughness2` is
        // that square. The two agree on THIS asset only because both are 0.0 or 1.0 on its seven materials - so if
        // this line were ever "simplified" to `roughness2`, no frame of this character would move.
        const float2 goo_fgd_coord = float2(sqrt(goo_clamped_ndotv), roughness);
        const float2 goo_fgd_coord_lut = goo_fgd_coord * goo_fgd_coord_scale + goo_fgd_coord_bias;
        // `合并 XYZ.001 = (分离 XYZ.X, 分离 XYZ.Y, 0.0)` - the LUT is sampled at `(coordLUT.x, coordLUT.y)` and the
        // sampler is `heap_sampler_texture` (LINEAR, REPEAT, which is the reference's own `image_user`:
        // `interpolation = Linear`, `extension = REPEAT`). The LUT has ONE mip level (see `set_goo_fgd_lut`).
        const float4 goo_fgd_texel = heap_texel(heap_at<Texture2D<float4>>(heap_slots_goo_fgd_lut), heap_sampler_texture, goo_fgd_coord_lut);
        // ---- `specularFGD` IS A THREE-CHANNEL MIX OF THE LUT's R AND G, AND `fresnel0` IS ITS FACTOR ----
        //
        // `lerp(float3(LUT.R), float3(LUT.G), fresnel0)`, which is what the reference's three `ShaderNodeMix`
        // nodes plus `合并 XYZ.002` do: EACH output component takes its own `fresnel0` component as the factor
        // (`混合`'s factor is `分离 XYZ.002.X`, `混合.001`'s is `.Y`, `混合.002`'s is `.Z`), so the expression is a
        // component-wise lerp and not a dot product. LUT.B DOES NOT PARTICIPATE.
        //
        // `fresnel0 = ComputeFresnel0 = lerp(0.07999999821186066, BaseColor, metallic)` - the REFERENCE's own
        // function, and BOTH of its arguments are now the two quantities `goo_diffuse_colour` above reads, because
        // the reference feeds both of its consumers from ONE output: `ComputeFresnel0.Input_0` is `转接点.090 =
        // _D(sRGB) ⊙ BaseColor`, the SAME value `转接点.045` hands to `ComputeDiffuseColor.albedo` (spec §1 item 3;
        // the second dump's B2/Input_0) - so the metal end is `albedo * goo_base_colour` and not the lane's tint
        // alone. Its factor is `metallic * metallic_max`, the same scaling `:2493` applies, because the reference's
        // `metallic` is `混合.002 = MetallicMax ⊙ clamp(_P.R, 0, 1)` (spec §1 item 1) while the port's own `metallic`
        // (`:1442`, `orm.x`) does not carry `MetallicMax`. `:2491`'s claim that on this asset it is already
        // `lerp(0, MetallicMax, _P.R)` is the PORT's own reading and this step neither verifies nor changes it.
        // WHAT MOVES WHEN THESE TWO ARGUMENTS MOVE: on this asset, NOTHING - measured. E2 is a no-op because all 23
        // `PBRToonBase` instances carry `MetallicMax = 1.0` (the verifier's B1), and E3 is a no-op because
        // `metallic * (1 - albedo)` stays under the 8-bit floor even amplified 1000x, i.e. `albedo` is 1 (to ~1e-5)
        // wherever `metallic` is not 0. Both arguments are still the reference's own and stay, because the port's
        // `metallic` (`:1442`) and `albedo` (`surface.glsl`'s `base_color_factor * texture`) are ASSET-DERIVED: an
        // asset whose metal texels are not white does move under E3, and one with `MetallicMax != 1` under E2.
        // `goo_step9_result.md` §4 has the probes.
        const float3 goo_fresnel0 = lerp(float3(goo_fgd_dielectric_f0), albedo * goo_base_colour, metallic * metallic_max);
        const float3 goo_specular_fgd = lerp(float3(goo_fgd_texel.r), float3(goo_fgd_texel.g), goo_fresnel0);
        // `reflectivity = 分离 XYZ.001.Y = LUT.G` (`组输出.Output_5`). NOTE THE NAME CLASH WITH THE PORT'S OWN
        // `reflectivity` (`orm.y`, the article's channel): they are DIFFERENT QUANTITIES - the article's is an F0
        // scale, the reference's is the LUT's G channel - which is the spec's G6. This local shadows nothing (the
        // port's read is used only by the branches this arm replaces) and is named apart on purpose.
        const float goo_fgd_reflectivity = goo_fgd_texel.g;
        // ---- `diffuseFGD = LUT.B + 0.5`: THE FLAGS WIN, AND THE MEASUREMENT IS KEPT AS THE DOUBT ----
        //
        // The ADD's `Value_001 = 0.5` is `enabled = true` and unlinked, so it participates; `Value_002 = 0.5` is
        // `enabled = false` and does not. The same rule settles `Remap01ToHalfTexelCoord`'s bias above, and both
        // were once read the other way here. What makes a reader doubt this one is the LUT itself: `B ∈ [0,1]`
        // (mean 0.480) is a finished Disney diffuse FGD, so `B + 0.5 ∈ [0.5, 1.5]` is a diffuse term that is
        // brighter than the albedo it multiplies and cannot reach 0 at grazing angles (`tests/test_goo_toon_math.cpp`
        // pins `b_min == 0 && b_max == 255` on the file's own bytes). That is a prior about the term, not evidence
        // about the graph - and this author does write ambient floors (step 4's `GlobalShadowBrightnessAdjustment`).
        //
        // IT IS ONE NAMED CONSTANT SO THE EXPERIMENT IS ONE LINE: set it to 0.0 for the prior, 0.5 for the node.
        // The measured consequence of the `0.5` this step landed on is in `deren-ab/goo_step5_result.md` §12.
        const float goo_diffuse_fgd = goo_fgd_texel.b + goo_fgd_diffuse_offset;
        const float goo_energy_compensation = 1.0 / goo_fgd_reflectivity - 1.0;
        // `specularFGD Strength`: PER MATERIAL (`body_01/02` `0.7999999523162842`, the cloths the group's `1.0`)
        // and ABSENT for the Face and Hair containers, whose `interface[]` has no such socket at all. `< 0` is the
        // sentinel and the reference's own group default is the fallback - the same contract `goo_rim_scalars` uses,
        // and the reason it is not `>= 0`-with-zero: `0.0` would switch a material's IBL specular off.
        const float4 goo_specular_fgd_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 16u);
        const float goo_specular_fgd_strength = goo_lane_stated(goo_specular_fgd_lane.x) ? goo_specular_fgd_lane.x : goo_specular_fgd_strength_default;
        // ... AND THE TWO COLOUR LANES THE SAME WAY, except that their fallback is per COMPONENT rather than behind
        // a sentinel: a light colour is a colour and there is no in-range value that means "absent". `1.0` is the
        // reference's own `interface[]` default for both (`dirLight_lightColor` and `AmbientLightColorTint`), and
        // the neutral the host table is filled with, so the `lerp` below is an identity for a material that states
        // no row - which is exactly how step 4's `fresnel_inside_colour` reads its own two.
        //
        // THE `goo_light_color` LANE IS THE SPEC'S §5.4 RULING AND IT CLOSES STEP 4'S U7: `dirLight_lightColor` is
        // authoritative for the DIRECT terms (`directLighting_specular` multiplies it through `Vector Math.006`,
        // and `directLighting_diffuse` through `群组.019.Input_1`), while Goo's `Shader Info.Ambient Lighting` is
        // the PROBE DIFFUSE IRRADIANCE the IBL-diffuse term is built from - so `IBL_diffuse` does NOT read this
        // lane (see the note at that term below).
        const float4 goo_light_color_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 17u);
        const float3 goo_light_color = lerp(float3(1.0, 1.0, 1.0), goo_light_color_lane.rgb, goo_light_color_lane.r > goo_lane_absent_threshold ? 1.0 : 0.0);
        const float4 goo_ambient_tint_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 18u);
        const float3 goo_ambient_tint = lerp(float3(1.0, 1.0, 1.0), goo_ambient_tint_lane.rgb, goo_ambient_tint_lane.r > goo_lane_absent_threshold ? 1.0 : 0.0);

        // ---- `directLighting_specular` (§6): SEVEN FACTORS, AND ITS `D*V` IS NOT A STANDARD GGX ----
        //
        // `clampedNdotL * (原 · F) * SpecularColor * CastShadows * dirLight_lightColor * directOcclusion`, where
        // `原` is `DV_SmithJointGGX_Aniso.original` and `F = F_Schlick(f0, f90 = 1.0, u = LdotH)`.
        //
        // `F = f0 + (1 - f0)*(1 - LdotH)^5`: the `F_Schlick` group's `运算.004 = SUBTRACT(1.0, x5)` READS `1.0`
        // FROM AN UNLINKED SLOT AND NOT FROM `f90`, which the spec's §A5 calls out as a naming trap - and the
        // instance's `f90` is `1.0`, so writing `f90 - x5` would agree here and diverge the moment anyone changed
        // it. This is written as the graph writes it.
        //
        // `LdotH` COMES FROM `Get_NoH_LoH_ToH_BoH` AND ITS FORM IS WORTH STATING, because the obvious `dot(L, H)`
        // is NOT what the graph computes: `invLenLV = 1/sqrt(dot(L,V)*2 + 2)`, `钳制.001 = clamp(invLenLV +
        // invLenLV*(invLenLV*LoV), 0, 1)`. The algebra is the standard identity `dot(L, H) = invLenLV*(1 + dot(L,V)*
        // invLenLV)` written that way to avoid building `H` at all; this port builds `H` (the anisotropic branch
        // above already has one) and takes `saturate(dot(L, H))`, which is the same number to float32 rounding.
        //
        // `原 = DV_SmithJointGGX_Aniso.original`, FROM THE MULTI-SCATTER SMITH JOINT GGX (Blender's, which this
        // reference and the chen dump both carry as the LTS-derived `DV_SmithJointGGX_Aniso`):
        //
        //     S  = 1 + NoH^2 * (a2 - 1)                                    # `运算.011` squares it, `运算.016` x λ
        //     λ  = |NoL| * (a2 + (1 - a2)*NoV^2)                           # UN-rooted, `运算.005` / `运算.006`
        //         + NoV * sqrt(a2 + (1 - a2)*NoL^2)                        # rooted, `运算.009` / `运算.010` / `运算.019`
        //     原 = (0.3183099 * 0.5) * a2 / max(S^2 * λ, 1.1754899742869237e-35)   # `运算.013` .. `运算.018`
        //
        // FOUR THINGS IN THAT EXPRESSION ARE NOT THE TEXTBOOK SMITH-JOINT GGX, AND STEP 11 EXISTS BECAUSE THE PORT
        // HAD TRANSCRIBED IT AS THE TEXTBOOK ONE. It pinned nothing either, so 1.78479x of missing highlight was
        // invisible to a 13/13 suite; the node-by-node arbitration is `deren-ab/goo_step11_armA_raw.md` and the
        // measurement is `deren-ab/goo_step11_armA_result.md`.
        //
        //   1. THERE IS NO `+1`. The denominator is `S^2 * λ`, not `S^2 * (1 + λ)`: `运算.012` adds the two halves
        //      of `λ` and `运算.018` divides by the result with no ADD of 1.0 anywhere between them. The port's
        //      `Gv = 1/(1 + lambdaV)` was worth a factor of λ/(1 + λ) = 0.3293069768 at the arbitrated point.
        //   2. THE TWO HALVES OF `λ` ARE ASYMMETRIC. `运算.010` is a SQRT node and it sits on the **NoL** bracket,
        //      multiplied by `NoV`; the `NoV` bracket is multiplied by `|NoL|` with no root, no POWER node and no
        //      second SQRT anywhere in the group's chain. λ therefore went from 0.4909950834 to 0.8353896146 at
        //      the arbitrated point - a factor of 1.7014215476 - while the expression's SHAPE barely moved.
        //   3. THE FLOOR IS ON THE WHOLE DENOMINATOR at the dump's own literal `1.1754899742869237e-35`
        //      (`运算.017 = MAXIMUM(运算.016, ...)`), not a `1e-12` on `S^2`. See the constant's own note below.
        //   4. NO GUARD INSIDE THE SQRT, and step 2 above is why that is safe rather than lucky.
        //
        // UNCERTAIN, AND STATED RATHER THAN HIDDEN: whether the reference's runtime evaluates `运算.010`'s SQRT in
        // float32 or in half precision is NOT recorded anywhere in the dump - it stores nodes, not qualifiers - so
        // this port evaluates it in float32. Even in float32 the arbitrated point moves by 3.0e-7 relative
        // (7.291723011e-02 as a closed form, 7.291720808e-02 when the trace executed the graph in float32), which
        // is the largest model-dependence in this term and is recorded rather than rounded away.
        //
        // and the `(1/pi)*(1/2)` is the group's OWN constant (`运算.013.Value = 0.3183099925518036` then
        // `运算.015 = MULTIPLY(运算.013, 运算.018)`), NOT this port's `goo_inverse_pi` - it is `1/(2pi)`, which is
        // what makes `原 * F` a usable magnitude in the reference's light units. `a2 = clampedRoughness^2`, and
        // `clampedRoughness` is the FGD's `roughness` SQUARED (`群组.003.roughness <- 转接点.010`), i.e. the
        // port's own `roughness2`-and-not-`roughness` - the ONE place the squared value IS the right one, which is
        // why both names are in scope here.
        //
        // ---- ARMS, AND A STUB THAT IS EXACT HERE BECAUSE OF THE CENSUS, NOT BECAUSE OF A GUESS ----
        //
        // `混合.016 = MIX(f = 组输入.Use anisotropy?, A = 转接点.035, B = 混合.017)`, and `转接点.035 =
        // Vector Math.003 = 群组.007.original × 群组.008.输出` - i.e. the ISOTROPIC product `原 * F_Schlick`, which
        // is what this port computes below. `lerp` in the reference's socket order therefore returns the FIRST
        // argument at `Use anisotropy? = 0`, and an earlier version of this block passed the two arms the other way
        // round: it returned the masked side at 0 and `原 * F` at 1, which is `混合.016`'s truth table INVERTED and
        // made the whole of `goo_direct_specular` identically zero (step 9's own note below records the symptom).
        // The order below is the reference's.
        //
        // THE MASKED ARM IS STILL A STUB, AND THAT IS EXACT FOR EVERY INSTANCE THIS REPOSITORY RENDERS. Be precise
        // about what the stub claims: `goo_anisotropic_masked` is "`混合.017` at `Anisotropic mask = 0`", NOT
        // "`混合.017`" - `混合.017 = MULTIPLY(A = 混合.020, B = 组输入.Anisotropic mask)` with the `MULTIPLY`'s
        // unlinked-but-enabled factor `1.0`, so the mask multiplies the WHOLE branch away and a mask of 0 makes the
        // reference's B arm ZERO whatever the anisotropic lobe would have been. REACHABILITY, with its census
        // (`goo_step9_verify.md` §7.6, all 23 `ng[2]` instances): the stub is only reached when
        // `Use anisotropy? = 1` AND `Anisotropic mask ≠ 0` together; among the asset's seven `M_actor_laevat_*`
        // materials (`body_01/02`, `cloth_01..05`) the flag is `1.0` only on `cloth_05`, whose mask is `0.0`, and the
        // mask is `0.0` on all seven - so `cloth_05`'s reference answer is exactly ZERO and the stub reproduces it.
        // The ONLY `ng[2]` instance in the dump with both is `M_actor_chen_cloth_01.001` (`1.0`/`1.0`, its mask
        // LINKED to `分离 XYZ.002.X`), and it is NOT a material of this asset (`laevatain_goo.glb`). The value is also
        // unobservable here: `混合.020 = MIX(f = 组输入.Use Toonaniso?, A = 群组.007.anisotropy × 群组.008.输出,
        // B = 钳制.Result = clamp(dot(cross(N, Tangent), Incoming), 0, 1))` is not implemented, and the probe that
        // swaps the arms (step 9's marker G, 74 640 px = 7.199% of the frame) cannot tell the two apart because on
        // this asset the mask zeroes the arm either way.
        // ---- STEP 10: WHERE THE TWO SWITCHES COME FROM ----
        //
        // THEY ARE PER MATERIAL, AND UNTIL THIS STEP THE PORT HARDCODED THEM. `混合.016`'s factor is
        // `组输入.Use anisotropy?` and `混合.017`'s second operand is `组输入.Anisotropic mask` (its `MULTIPLY`
        // factor is the unlinked-but-`enabled` `1.0`, so `混合.017 = 混合.020 * mask` exactly). Both are plain group
        // inputs of `PBRToonBase` (`ng[2]`), with NO other node on either chain - step 9's `_s9_aniso2.py` /
        // `_s9_aniso3.py` §B checked `混合.016.inputs[0].from` and it is the group input itself, so there is no
        // polarity node to invert - and they ride lane 25 (`_GooAnisoGate`) as `.x` and `.y`. `.z` (`Use Toonaniso?`)
        // is RECORDED THERE AND NOT READ HERE: it switches `混合.020`, which this port does not implement (see the
        // block below). The lane's neutral is `(0, 0, 0, 0)`, which is the reference's own `interface[]` default for
        // all three sockets, so a material that states no row reads exactly what the graph returns for a caller that
        // states nothing.
        const float4 goo_aniso_gate = toon_colour_at(heap_slots_toon_colours, colour_base + 25u);
        const float goo_use_anisotropy = goo_aniso_gate.x;
        const float goo_anisotropic_mask = goo_aniso_gate.y;
        const float3 goo_half_dir = normalize(l + v);
        const float goo_ndoth = saturate(dot(n, goo_half_dir));
        const float goo_ndotl_clamped = saturate(dot(l, n));
        const float goo_ldoth = saturate(dot(l, goo_half_dir));
        // `a2 = clampedRoughness^2` where `clampedRoughness` is the port's `roughness` SQUARED - i.e. `a2` is the
        // perceptual roughness to the FOURTH. IT IS SPELLED HERE RATHER THAN READ FROM `:2013`'s `roughness2`,
        // which is declared several hundred lines BELOW this arm (the anisotropic BRDF is in the port's own
        // specular block, after `main_specular`), and the first build of this step failed with
        // `undefined identifier 'roughness2'` for exactly that reason. `max(roughness*roughness, 0.0078)` is the
        // port's own floor and NOT the reference's: the reference's `clampedRoughness` is a bare product of the
        // node graph with no clamp at all, and `roughness = 0` (a perfectly smooth material) gives `a2 = 0` and
        // therefore `D = 0` - which is the reference's own answer for the body's `perceptualRoughness = 0.0` and
        // one this port must not "fix" with a floor.
        const float goo_clamped_roughness = roughness * roughness;
        const float goo_a2 = goo_clamped_roughness * goo_clamped_roughness;
        // `S = 1 + NoH^2*(a2 - 1)`. `运算.011` multiplies this value by ITSELF and `运算.016` multiplies that
        // product by `λ`; it is spelled out here rather than passed through `pow(..., 2.0)` because the graph's
        // `运算.011` is a MULTIPLY node and the two round differently in the last bit.
        const float goo_dv_s = goo_ndoth * goo_ndoth * (goo_a2 - 1.0) + 1.0;
        // THE TWO HALVES OF `λ`, AND THEY ARE NOT SYMMETRIC - this is the asymmetry step 11's trace arbitrated:
        //   `群组.partLambdaV = a2 + (1 - a2)*NoV^2`, multiplied by `Abs_NdotL` (`运算.005`, `运算.006`):
        //   UN-rooted, and this port's name for that half used to be the single `goo_lambda_v`;
        //   `NoV * SQRT(a2 + (1 - a2)*NoL^2)` (`运算.009`, `运算.010`, `运算.019`): THE ONE ROOT ON THIS PATH,
        //   and it is on the NoL bracket.
        const float goo_dv_lambda_v = abs(goo_ndotl_clamped) * (goo_a2 + (1.0 - goo_a2) * goo_clamped_ndotv * goo_clamped_ndotv);
        const float goo_dv_lambda_l = goo_clamped_ndotv * sqrt(goo_a2 + (1.0 - goo_a2) * goo_ndotl_clamped * goo_ndotl_clamped);
        const float goo_dv_lambda = goo_dv_lambda_v + goo_dv_lambda_l;   // `运算.012`: an ADD, with NO `+1` in it
        // THE FLOOR IS ON THE WHOLE DENOMINATOR AND NOT ON `S^2`: `运算.016 = S^2 * λ`, then `运算.017 =
        // MAXIMUM(运算.016, <the literal below>)`. The literal is the dump's, VERBATIM, and it is `f32(1.17549e-35)`
        // (bit pattern 0x0579FFC3, i.e. 999.9962768554688 * FLT_MIN): NOT FLT_MIN, not `1000 * FLT_MIN` and not
        // `f32(1e-35)`, each of which is a different float32.
        const float goo_dv_denominator = max(goo_dv_s * goo_dv_s * goo_dv_lambda, 1.1754899742869237e-35);
        // NO `max(·, 0)` INSIDE THE SQRT, because the reference has none and the argument cannot go negative:
        // `a2` is in [0, 1] and `NoL^2` is in [0, 1], so `a2 + (1 - a2)*NoL^2` is a convex combination of two
        // non-negative numbers for every input this term can receive. The clamp the port used to carry there was
        // never load-bearing, and removing it keeps the expression the graph's.
        // WHETHER THE REFERENCE'S RUNTIME EVALUATES THAT SQRT IN FLOAT32 OR IN HALF IS UNDETERMINED - the dump
        // records nodes, not precision qualifiers - so this port evaluates it in float32, this shader's default.
        // `运算.014 = a2 * 1.0` is the DIVIDE's numerator and `运算.018 = 运算.014 / 运算.017`; `运算.015` then
        // multiplies THAT by `运算.013`'s `1/(2pi)`, which is the order spelled below.
        const float goo_dv_original = goo_dv_half_inverse_pi * (goo_a2 / goo_dv_denominator);
        const float goo_schlick_x = 1.0 - goo_ldoth;
        const float goo_schlick_x5 = goo_schlick_x * goo_schlick_x * goo_schlick_x * goo_schlick_x * goo_schlick_x;
        const float3 goo_schlick_f = goo_fresnel0 + (1.0 - goo_fresnel0) * goo_schlick_x5;
        // `混合.016 = MIX(f = 组输入.Use anisotropy?, A = 原*F, B = 混合.017)`, and `混合.017 = 混合.020 ⊙
        // Anisotropic mask` with both `Use Toonaniso?` and `Anisotropic mask` read from the material. The B side is
        // therefore `钳制.Result * mask`, and with `mask = 0.0` on all seven materials it is zero whichever
        // `Use Toonaniso?` says - so the `mask` is the factor that decides, and the `Use anisotropy?` flag only
        // chooses between `原*F` and that zero.
        // `混合.016` IN THE REFERENCE'S ARM ORDER: `lerp(A = 原 * F, B = 混合.017, f = Use anisotropy?)` - NOT the
        // masked side first. See the block above for the census that makes the stub exact here, and for the fact
        // that step 9 measured the two swapped arms as a 74 640 px (7.199%) frame difference.
        // ---- STEP 12: THE LOBE ITSELF, IN PLACE OF THE ZERO STUB ----
        //
        // WHAT WAS HERE UNTIL THIS STEP: `float3(0.0, 0.0, 0.0) * goo_anisotropic_mask` - "`混合.017` at
        // `Anisotropic mask = 0`", which was exact for every material of this asset (all seven masks are 0.0) and
        // which the note above still describes correctly. It was exact and it was a STUB: for any material whose
        // mask is non-zero - `M_actor_chen_cloth_01.001`, not in this repository's render set - the port returned
        // zero where the reference returns a lobe, and its self-description said so.
        //
        // THE REFERENCE'S THREE LAYERS, read node by node out of `gooblender/nodes.json` (`ng[17]`
        // `DV_SmithJointGGXAniso` / `ng[25] GetSmithJointGGXAnisoPartLambdaV`, re-derived by
        // `deren-ab/_s12v_trace.py`, spec `goo_step12_aniso_spec.md` and brief `goo_step12_impl_brief.md` §1):
        //
        //     层 1  群组.007.anisotropy = LOBE = 0.15915493667125702 * num / max(den, 0.0010000000474974513)
        //                            num = 1.0 * p^3,  den = ( |W|*NdotV + Lam*NdotL ) * s2^2
        //     层 2  混合.020 = MIX(f = Use Toonaniso?, A = LOBE ⊙ F_Schlick, B = 钳制.Result)
        //             钳制.Result = clamp(dot(cross(N, Tangent), Incoming), 0, 1)
        //     层 3  混合.017 = 混合.020 ⊙ Anisotropic mask
        //
        // ... AND THREE THINGS ABOUT THAT ARE WORTH SPELLING, because each one is a place the port's earlier
        // reading of this group (`goo_step11_anisodv_spec.md`, whose §3 list of the iso group's arithmetic is
        // FALSIFIED - see the brief's §6) got it wrong:
        //
        //   (1) `S`'s THIRD COMPONENT IS `NdotH * p` AND `W` IS THE LIGHT-SIDE VECTOR. `S = (rB*TdotH, rT*BdotH,
        //       NdotH*p)` and `W = (rT*TdotL, rB*BdotL, NdotL)` - note the CROSSED roughnesses, which is the
        //       reference's own and not a typo - so `|W|` is built from the LIGHT vector and `Lam` from the VIEW
        //       vector. Swapping them is invisible in a unit test written from the same misreading, which is why
        //       the test points are pinned against the trace's own numbers rather than against a formula.
        //   (2) `num = 1.0 * p^3`, where the `1.0` is `合并 XYZ.003.X` - an ENABLED, UNLINKED literal that nothing
        //       links (the trace prints it; it is why the numerator is a MULTIPLY node and not a single value).
        //   (3) `Lam = length([rT*TdotV, rB*BdotV, NdotV])` - the WHOLE of `ng[25]`'s body, NOT `λV` and NOT the
        //       `|NoL|*(a2 + (1-a2)*NoV^2)` shape the plan's 层 1 sketch carries. `W` and `Lam` are summed
        //       exactly once, in the denominator, and `W` is NOT re-used as a numerator factor.
        //
        // `roughnessT` / `roughnessB` COME FROM LANE 26 (`_GooAnisoRough`, `(1 - Aniso_SmoothnessMax{x})^2`), and
        // the LAUNDERING IS DELIBERATELY NOT APPLIED: the reference folds `roughness = lerp(0, Aniso_SmoothnessMax,
        // _P.A)` before the square, and `_P.A` (a `非色彩Non_Color` factor) has no lane in this port. `_P.A` is
        // LINKED on all five cloth materials and its source is not exported by the dump, so this is a NAMED
        // APPROXIMATION rather than an oversight: see the result document's uncertainty section. It cannot move a
        // pixel of any frame this repository renders, because `混合.017`'s mask is `0.0` on every material of this
        // asset - which is also why the acceptance for this step is "byte-identical frames", not "a nicer picture".
        // `p = rT*rB` IS THE PORT'S `a2` HERE AND NOT `roughness^4`: the two shares a name because the reference
        // reuses the socket, and the ARM's `goo_a2` above is the ISOTROPIC `clampedRoughness^2` - a different
        // number from a different material input. Do not merge them.
        const float4 goo_aniso_rough_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 26u);
        const float goo_aniso_roughness_t = (1.0 - goo_aniso_rough_lane.x) * (1.0 - goo_aniso_rough_lane.x);
        const float goo_aniso_roughness_b = (1.0 - goo_aniso_rough_lane.y) * (1.0 - goo_aniso_rough_lane.y);
        const float goo_lobe_p = goo_aniso_roughness_t * goo_aniso_roughness_b;
        // THE TANGENT FRAME, FROM `surface_tbn` - THE SAME ONE STEP 8'S NORMAL DECODE USES, and the ONE substitution
        // in this term that is the PORT's rather than the graph's: the reference's Tangent is a `ShaderNodeTangent`
        // whose uv_map / direction / axis are NOT exported by the dump, and this renderer carries no vertex tangent
        // attribute, so the uv-derivative frame is what the article's own note says the reference's maths
        // corresponds to. `surface_tbn`'s OUT parameters are taken as they arrive: the reference recomputes its
        // bitangent as `N × T`, and reconstructing one here too would double-count the flip the function already
        // applied. The FALLBACK is the function's own literal `(1,0,0)` / `(0,1,0)` basis, used WITHOUT
        // normalising - the brief's §3.4 says why: a zero vector there would put a NaN into a lobe that this arm
        // evaluates for EVERY pixel and then multiplies by zero.
        float3 goo_lobe_t = float3(1.0, 0.0, 0.0);
        float3 goo_lobe_b = float3(0.0, 1.0, 0.0);
        surface_tbn(world_pos, uv, goo_lobe_t, goo_lobe_b);
        // `TdotH` / `BdotH` / `TdotL` / `BdotL` / `TdotV` / `BdotV`, against the SAME vectors the block above
        // already uses (`l`, `v`, `goo_half_dir`, `n`). No `saturate`, because the reference has none on them:
        // `dot` is signed here and `s2` / `|W|` are sums of squares, so a negative dot is a real input.
        const float goo_lobe_t_dot_h = dot(goo_lobe_t, goo_half_dir);
        const float goo_lobe_b_dot_h = dot(goo_lobe_b, goo_half_dir);
        const float goo_lobe_t_dot_l = dot(goo_lobe_t, l);
        const float goo_lobe_b_dot_l = dot(goo_lobe_b, l);
        const float goo_lobe_t_dot_v = dot(goo_lobe_t, v);
        const float goo_lobe_b_dot_v = dot(goo_lobe_b, v);
        // `s2 = dot(S, S)` and `|W|`, both SPELLED OUT as the two MULTIPLY nodes plus the ADD the graph uses,
        // rather than as `dot(...)` of a temporary: this is the one place in this block where the graph's own
        // node count is visible in the answer (step 11's trace showed a last-bit difference between the FMA and
        // the split form), and the split form is the graph's.
        const float goo_lobe_s2 = (goo_aniso_roughness_b * goo_lobe_t_dot_h) * (goo_aniso_roughness_b * goo_lobe_t_dot_h) +
                                  (goo_aniso_roughness_t * goo_lobe_b_dot_h) * (goo_aniso_roughness_t * goo_lobe_b_dot_h) +
                                  (goo_ndoth * goo_lobe_p) * (goo_ndoth * goo_lobe_p);
        const float goo_lobe_w_length = sqrt((goo_aniso_roughness_t * goo_lobe_t_dot_l) * (goo_aniso_roughness_t * goo_lobe_t_dot_l) +
                                             (goo_aniso_roughness_b * goo_lobe_b_dot_l) * (goo_aniso_roughness_b * goo_lobe_b_dot_l) +
                                             goo_ndotl_clamped * goo_ndotl_clamped);
        // `Lam` - `ng[25]`'s WHOLE body, on the VIEW side (see (1) and (3) above). `goo_clamped_ndotv` is the
        // arm's own `clamp(dot(N, V), 0, 1)`, the same one `goo_dv_lambda_l` above uses, and it is NOT `abs`ed:
        // `ng[25]` takes the view dot as it arrives.
        const float goo_lobe_lambda = sqrt((goo_aniso_roughness_t * goo_lobe_t_dot_v) * (goo_aniso_roughness_t * goo_lobe_t_dot_v) +
                                           (goo_aniso_roughness_b * goo_lobe_b_dot_v) * (goo_aniso_roughness_b * goo_lobe_b_dot_v) +
                                           goo_clamped_ndotv * goo_clamped_ndotv);
        const float goo_lobe_denominator = (goo_lobe_w_length * goo_clamped_ndotv + goo_lobe_lambda * goo_ndotl_clamped) * goo_lobe_s2 * goo_lobe_s2;
        // THE FLOOR IS `运算.017`'s MAXIMUM ON THE WHOLE DENOMINATOR (the reference's own guard, spelled as its
        // own constant rather than shared with the arm above). It cannot bind for a unit direction frame - `s2` is
        // a convex combination of `{rB^2, rT^2, p^2}` and so is `> 0`, and `roughness -> 0` drives `num` to zero
        // faster than `den` can - but it is what makes the lobe's own `0 * NaN` path impossible, which is the
        // property this arm needs because it evaluates the lobe for mesh-visible pixels AND THEN multiplies by a
        // mask that is zero for every one of them.
        const float goo_lobe = goo_aniso_half_inverse_pi * ((goo_lobe_p * goo_lobe_p * goo_lobe_p) / max(goo_lobe_denominator, goo_aniso_denominator_floor));
        // `钳制.Result`, THE CLAMP ARM - `clamp(dot(cross(N, Tangent), Incoming), 0, 1)` with `Incoming` the view
        // vector. IT IS NOT INVARIANT UNDER `T -> -T`, unlike everything else in this lobe and unlike `Lam` / `s2`
        // / the dots above; the dump does not export the tangent's direction/axis, so whether the reference's
        // `cross(N, T)` handedness matches `surface_tbn`'s is UNVERIFIABLE here (plan U4). On this asset it is
        // unreachable anyway: `混合.017` multiplies the whole of `混合.020` by a mask that is 0.0 everywhere.
        const float goo_lobe_toon_aniso = saturate(dot(cross(n, goo_lobe_t), v));
        // `混合.020` AT THE REFERENCE'S OWN ARM ORDER, `lerp(A = LOBE ⊙ F_Schlick, B = clamp arm, f = Use
        // Toonaniso?)` - the same reading of factor 0 as `混合.016` above takes, and the third switch of lane 25
        // is now genuinely consumed rather than recorded.
        const float3 goo_anisotropic_masked = lerp(goo_lobe * goo_schlick_f, float3(goo_lobe_toon_aniso, goo_lobe_toon_aniso, goo_lobe_toon_aniso), goo_aniso_gate.z) * goo_anisotropic_mask;
        const float3 goo_specular_chosen = lerp(goo_dv_original * goo_schlick_f, goo_anisotropic_masked, goo_use_anisotropy);
        // `转接点.077 <- 组输入.SpecularColor` - PER MATERIAL AND HDR (not a tint): `body_01` is
        // `[4.2092814445495605, 3.7652196884155273, 3.7652196884155273]`, `cloth_02` is
        // `[1.0, 0.8945042490959167, 0.8945042490959167]`. The spec's §7.3 item 5 says why it CANNOT be skipped or
        // substituted (the body's direct specular would be 4.2x too dark, and no other socket of this group
        // carries it - `BaseColor` disagrees by 3.6x and by a different SIGN of the difference on `cloth_02`).
        const float4 goo_specular_color_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 19u);
        const float3 goo_specular_color = lerp(float3(1.0, 1.0, 1.0), goo_specular_color_lane.rgb, goo_specular_color_lane.r > goo_lane_absent_threshold ? 1.0 : 0.0);
        // `Vector Math.016 = 混合.016.Result * SpecularColor`, then `Vector Math.004 = 钳制.002 * that` - the
        // `clampedNdotL` is `钳制.002 = clamp(转接点.038, 0, 1)` where `转接点.038 <- 转接点 <- Vector Math.Value =
        // DOT(L, N)`, i.e. `clamp(dot(L, N), 0, 1)` on the SHADING normal. (The spec's §6.1 says the same and adds
        // why `Abs_NdotL` is a misnomer; the `DV_SmithJointGGX_Aniso` instance in fact receives the UNCLAMPED
        // `dot(N, V)` from `转接点.079 <- 转接点.003 <- Vector Math.002`, whose `lambdaV` takes `abs` of it inside
        // the group - so this step feeds `goo_clamped_ndotv`, which is what that formula wants, and `clamp` on
        // `clampedNdotL` separately.)
        // `转接点.040 = 转接点.039 = Shader Info.Cast Shadows` IS THE ENGINE'S VISIBILITY FACTOR, which is the port's
        // `shadow_used` and is ALREADY the `SigmoidSharp` argument for the ramp above (`:1756`). The spec's §9-U2
        // leaves the two scales unverified against each other; on THIS character `shadow_used` is the engine's own
        // cast shadow and the reference's is its visibility term, and the SIGMOID above shows the port reads it as
        // a shadow amount rather than as a factor. THE STEP 4 RAMP AND THIS TERM THEREFORE SHARE ONE SAMPLE, which
        // is the reference's own arrangement (`Shader Info.Cast Shadows` feeds `转接点.039` and `混合.005` - the
        // ramp's `u` - from one output).
        // `转接点.044 = 混合.005 = MIX(A = directOcclusionColor, B = white, f = 转接点.046)` - the SAME
        // `direct_occlusion` step 4 computes for the diffuse (`:1787`), whose factor is the reference's
        // `Is Skin?`-selected occlusion and the port's `ao_used`. ONE PORT READ, TWO REFERENCE CONSUMERS.
        //
        // ---- `Vector Math.014`, THE DIRECT PATH'S ENERGY COMPENSATION (spec §3.3) ----
        // The reference multiplies the DIRECT specular by `1 + energyCompensation ⊙ fresnel0`, where
        // `energyCompensation = 1/reflectivity - 1` with `reflectivity = LUT.G` (both are `:2639` and `:2625` above).
        // The factor is COMPONENT-WISE because `fresnel0` is a float3. The IBL product below keeps the BARE
        // `energyCompensation` - that is step 5's line and the reference does the same - and since the two products
        // are added ONCE EACH at `toon_colour` (`:3289`), the asymmetry is a difference of FACTOR, not a double
        // count. Before this line the direct path had no factor at all, which is the defect this step closes.
        // WHAT THIS FACTOR MEASURABLY DID WHEN STEP 9 LANDED: NOTHING - all three poses were byte-identical before
        // and after it (three runs each; `goo_step9_result.md` §3) - and the cause was the TERM above the factor,
        // never the factor: `goo_specular_chosen` was `lerp(goo_anisotropic_masked, goo_dv_original *
        // goo_schlick_f, goo_use_anisotropy)` with `goo_use_anisotropy` pinned to a hardcoded 0.0 constant (step 10
        // replaced it with lane 25's `.x`), and `lerp`
        // returns its FIRST argument at 0 - which was `goo_anisotropic_masked = float3(0,0,0) * 0.0`, i.e. the
        // `混合.016` arms swapped. Scaling the whole product by 50x moved no pixel; replacing the lerp's zero arm by
        // `float3(0.25)` moved 74 640 px, so the term was dead, not small. **STEP 10 SWAPPED THOSE ARMS** (the two
        // lines above this block) and the factor is now live on every pixel the direct specular reaches. What
        // remains a property of the FORM rather than a measurement: the body's FGD carries `LUT.G = 1.0`, so
        // `goo_energy_compensation` is 0.0 EXACTLY and this factor is exactly `1 + 0*0.08` on every body pixel -
        // while the cloth's `1.0407` is a PREDICTION from its reported `LUT.G = 0.968313694`
        // (`fresnel0 = 1.2424540519714355` on a metal texel). Step 10 measured the revived term's own frame share as
        // ~+0.02/255 in the frame mean (`goo_step10_result.md` §3).
        //
        // ---- DEBT (q), THE VISIBILITY TOKEN (spec §3.1) ----
        // The reference's direct specular multiplies `Shader Info.Cast Shadows`'s RAW output (`Vector Math.005.in[1]`
        // <- `Shader Info#1 out[1]`), NOT its post-`SigmoidSharp` value: `cast_shadow_sigmoid` (`:2524`) belongs to
        // the RAMP, which is why `:2526 ramp_u = min(cast_shadow_sigmoid, remap_half_lambert_sigmoid)` keeps it and
        // this line does not. The two differ by the cast curve itself (`:924-930`), and the substitution is
        // TWO-SIDED - it darkens where `sig(v) > v` and brightens where `sig(v) < v` (body's fixpoint is
        // `v* = 0.833766073`; cloth/face's is `0.997142116`) - so do not read it as "the highlight only gets darker"; the closed form and the
        // fixpoint are pinned in `tests/test_goo_toon_math.cpp`. Nothing else on this line moved.
        const float3 goo_direct_specular = goo_ndotl_clamped * (goo_specular_chosen * goo_specular_color) * shadow_used * goo_light_color * direct_occlusion * (1.0 + goo_energy_compensation * goo_fresnel0);

        // ---- `IBL_specular` AND `IBL_diffuse` (§7.1) ----
        //
        //     IBL_specular = specularFGD ⊙ specularFGD_Strength ⊙ 能量补偿
        //     IBL_diffuse  = AmbientLighting ⊙ AmbientLightColorTint ⊙ diffuseFGD ⊙ diffuseColor
        //
        // THE IBL SPECULAR LOOKS AT NO ENVIRONMENT MAP AT ALL, and the spec's §7.3 says so in as many words: this
        // is a real DECOUPLING from the port's `indir_light_specular`, which samples `env_cube_sample_lod` and is
        // therefore tied to `EnvRotation` / `env_mip_count` / `rig.env.x`. The reference's version is an
        // "irradiance x FGD" analytic term built from the material's own numbers, so after this step a toon
        // character's specular IBL no longer reflects the sky the rest of the frame reflects. THAT IS THE
        // REFERENCE'S OWN MECHANISM AND IT IS WRITTEN HERE RATHER THAN HIDDEN, because it is the one place this
        // step makes the character LESS consistent with its scene.
        //
        // ITS MAGNITUDE IS NEARLY ZERO ON THIS ASSET, AND THAT IS ALSO THE REFERENCE'S OWN ANSWER (the spec's §A6
        // table, which is why its §10.4 criterion 2 says a frame that barely moves is the CORRECT outcome): on a
        // smooth material `reflectivity = LUT.G = 1.0` exactly, so `1/1 - 1 = 0` and `body_01`/`body_02`'s whole
        // IBL specular is zero; on the cloth it is `0.0326 * 0.1854 = 0.006`. The visible change of this step is
        // the DIRECT specular, and a report that measures only the IBL terms would conclude the step did nothing.
        goo_spec_ibl = goo_specular_fgd * goo_specular_fgd_strength * goo_energy_compensation;
        // `混合.013 = Shader Info.Ambient Lighting ⊙ 组输入.AmbientLightColorTint`: the irradiance is the ENGINE's
        // (the spec's §5.4: `Shader Info.Ambient Lighting` is PROBE DIFFUSE IRRADIANCE and it has exactly ONE
        // consumer in the container - here), which the port already samples as `irradiance_sample(n)`; the TINT is
        // the material's new lane. `G8`/`G9` of the spec's §7.2 are the two things that must NOT come along:
        // `params.ambient_strength` (a family switch the reference has no equivalent of) and `ao_used` (the
        // reference's IBL diffuse reads no occlusion - its `混合.008` has four factors and AO is not one of them).
        // `diffuseColor` IS STEP 4'S `goo_diffuse_colour`, deliberately and not by coincidence: `G10`/`G15` say the
        // reference's IBL diffuse and its direct diffuse must carry the SAME `(1 - metallic)` discount, and step 4
        // already chose the reference's `1 - metallic*MetallicMax` over the article's `0.96 - 0.96*metallic`.
        goo_diff_ibl = irradiance_sample(n) * goo_ambient_tint * goo_diffuse_fgd * goo_diffuse_colour;

        // ---- AND WHAT THIS ARM HANDS BACK: THE ONE TERM IT OWNS, AND THE TWO FACTORS ----
        //
        // IT SUBSTITUTES `direct` AND DOES NOT RETURN. The reference's two factors multiply `混合.011` - four
        // terms - while this function composes those four at `toon_colour`, several hundred lines BELOW and after
        // `ambient` / `indir_light_specular` are declared. An earlier revision of this arm built the sum here and
        // returned it, which does not compile, and the reason is the boundary itself rather than a slip: this step
        // owns ONE of the four, so the only honest place for the product is where the four are added. `direct`
        // keeps its meaning - the port's direct-diffuse term - and the composition below is unchanged except for
        // the two factors.
        direct = goo_direct_diffuse;
        goo_spec_direct = goo_direct_specular;
        goo_arm = true;
        goo_factor = gsba_gate * toon_fresnel;
    }

    // The ambient is the engine's irradiance map and the engine's composition
    // (`ibl_diffuse * albedo * ao`), so a toon character in a scene is lit by the same sky as everything
    // else in it. WITHOUT IT the dark side is the tint times nothing, which reads as a hole rather than as
    // shadow - and a toon look without ambient is the single most common way this style reads as broken.
    // `ao_used` AND NOT `ao`, for the reason the skin branch above states: the article's skin feeds its own
    // albedo-alpha occlusion to EVERY term that reads an occlusion, and this one does too.
    const float3 ambient = irradiance_sample(n) * albedo * ao_used * params.ambient_strength;

    // ---- THE RIM: TWO DIFFERENT MECHANISMS, WHICH IS WHAT THE ARTICLE CARRIES ----
    //
    // RIM ONE IS A RANGE-MAPPED FRESNEL, and the mapping is the part worth stating: rather than raising `1 - NoV` to
    // a power and calling the result a rim, the article maps it through a WINDOW whose start and end both move with
    // ONE material number (`_RimLightArea`) - so an artist widens the contour's inner edge without moving its outer
    // one, which is what lets the same shader give a sleeve a broad rim and a cheek a tight one. The window is
    // smoothstepped, so the band has no hard edge at either end.
    const float rim_start = rig.rim.x * -0.6 + 0.8;
    const float rim_end = rig.rim.x * -0.4 + 0.9;
    const float rim_width = max(rim_end - rim_start, 1e-4);
    // A FACE'S RIM IS MEASURED AGAINST ITS REBUILT NORMAL rather than the model's (`face_normal` above), which is
    // the whole reason that normal exists: a rim from faceted normals breaks along the cheeks.
    const float rim_no_v = sdf_lane != 0u ? saturate(dot(face_normal, v)) : no_v;
    float rim_t = saturate(((1.0 - rim_no_v) - rim_start) / rim_width);
    float rim_area = rim_t * rim_t * (3.0 - 2.0 * rim_t);
    // ---- AND A FACE'S RIM HAS A REGION AND A SIDE, BOTH FROM THE MASK ----
    //
    // `.w` IS THE RIM'S OWN REGION: the article blends its window against that channel -
    // `lerp(rimArea, rimArea_mask, _RimMaskStrength)` (`ZmdFaceToonMain.hlsl:322`) - and `_RimMaskStrength`
    // defaults to 1 (`MyZmdFaceShader.shader:87`) with no sidecar row to override it, so at the article's own
    // defaults the mask DECIDES and the window is discarded. THAT IS NOT A HARDCODED `rim_area = 1.0`, which is
    // what this line was mistaken for: the `1.0` here is the lerp's WEIGHT, and the value it selects is the
    // sampled channel. A material with no mask keeps the window it always had, because the block is skipped.
    //
    // THE SIDE IS THE CAMERA'S, taken as `dot(camera_right, normal)`: a rim down BOTH sides of a face reads as a
    // painted mask rather than as a light, so the article keeps the one the camera's right axis agrees with.
    float3 rim_one_sided = float3(1.0, 1.0, 1.0);
    if (sdf_lane != 0u) {
        const float3 camera_right = normalize(cross(float3(0.0, 1.0, 0.0), camera_forward) + float3(1e-6, 0.0, 0.0));
        // THE MASK IS THE RIM'S REGION, AND IT REPLACES THE WINDOW RATHER THAN MULTIPLYING IT - which is the
        // article's own `lerp(rimArea, rimArea_mask, _RimMaskStrength)`, taken at the weight that lets the mask
        // decide. TWO EARLIER READINGS OF THIS CHANNEL WERE WRONG, and the file itself is what settled it: rendered
        // as an image, `.w` is a BROAD VERTICAL BAND DOWN EACH SIDE OF THE FACE plus a small patch at the NOSE -
        // exactly where a face's contour light belongs, and exactly what the article's own text calls out ("the
        // nose-tip rim, which can be observed in the game"). I first read it as "a region, not a rim", and then as a
        // window to be multiplied by the fresnel band - the product of two masks, which intersected the rim away and
        // is why this stage's face rim has been nearly invisible since.
        //
        // THE COORDINATE QUESTION THIS COMMENT USED TO LEAVE OPEN IS ANSWERED, and the answer was the fault: the
        // mask was being sampled at the SDF's MIRRORED uv, and the article samples it at the ORIGINAL one
        // (`ZmdFaceToonMain.hlsl:55` vs `:157`). A probe of this mask then read ~0.04 at p90 where the channel's own
        // statistics promise bright bands at ~1.0 over 13% of its area - i.e. the read was landing in the wrong
        // place, which is why a five-factor product left this rim a six-pixel effect. See the mask's own block
        // where `face_mask_uv` is built.
        rim_area = lerp(rim_area, face_mask.w, 1.0);
        rim_one_sided = saturate(dot(camera_right, face_normal)).xxx;
    }
    // ---- THE ARTICLE'S "THE HEAD IS TURNED TOWARD THE CAMERA" DOOR, WHICH THIS PORT DID NOT HAVE ----
    //
    // `ZmdFaceToonMain.hlsl:310-311` and `:324`, both of which are the SAME factor:
    //     float headFDotCamerF_rim = saturate(-0.9 + headFDotCamerF * 10);
    //     headFDotCamerF_rim = MySmoothstep(headFDotCamerF_rim);
    //     float3 rimLight = rimArea_final * headFDotCamerF_rim * _RimLightColor * _RimLightStrength;
    // `headFDotCamerF` positive means the camera is looking along the head's own forward axis, i.e. the face is
    // turned TOWARD it; the ramp `-0.9 + 10x` then saturates at `x = 0.09` and is 0 for everything below `-0.9`, so
    // a face seen from behind loses its contour light entirely. That is the article's answer to "a rim is a
    // property of a lit edge, and a face pointing away from the camera has no such edge to show it".
    //
    // `MySmoothstep(x) = x*x*(-2x+3)` (`ZmdToonUtils.hlsl:24-30`) is written OUT because this file has no include
    // of that header, and it is NOT HLSL's `smoothstep` - the two agree only at 0, 0.5 and 1.
    const float head_f_dot_camera_f_rim = saturate(-0.9 + head_forward_dot_camera_forward * 10.0);
    const float rim_head_gate = head_f_dot_camera_f_rim * head_f_dot_camera_f_rim * (-2.0 * head_f_dot_camera_f_rim + 3.0);
    // ---- THE RIM'S COLOUR IS THE ARTICLE'S `_RimLightColor`, WHICH IS WHITE ----
    //
    // `MyZmdFaceShader.shader:84` `_RimLightColor ("RimLightColor", Color) = (1,1,1,1)`, and it has NO sidecar
    // row, so the article's face rim is WHITE. THIS PORT USED `_SDFRimColor` HERE - a name that does not exist
    // anywhere in the article's sources (`deren-ab/face_shader_compare.md` §1's last row) - read from the lane that
    // carries the GAME's `_SDFRimColor` row (chen: 0.761/0.553/0.455), i.e. a warm tint the article never applies.
    // It is gone, and `params.rim_colour` (the family's, white from the family table) is what the rim takes. The
    // `sdf_rim_colour` lane is still READ above and still used - by the EYE's highlight tint, which is a different
    // consumer of the same `color` table - so the lane itself is untouched.
    //
    // AND `params.rim_strength` IS GONE TOO, for the same reason: the article multiplies `_RimLightStrength`, whose
    // default is 1 (`MyZmdFaceShader.shader:85`) and which has no sidecar row, so the article's face rim carries no
    // family scale. The port's face family setting is 0.7, i.e. it was drawing this rim 30% weaker than the
    // article. `rig.rim.y` (the config's own `_RimLightStrength`, default 1.0) STAYS: that is the article's
    // property under the article's own name, and it is the knob an A/B is supposed to be able to move.
    const float3 rim_colour_used = params.rim_colour;
    const float3 rim_brdf = (light_colour - 0.25) * rig.rim.w + 0.25;
    // THE `min(ao, shadow)` GATE IS THE PORT'S AND NOT THE ARTICLE'S, and it is left as it is ON PURPOSE: the
    // article's line is `rimLight * min(ao, shadowScene)` (`:325`) and `shadowScene` does not exist in this stage
    // (its sigmoid remap `SigmoidSharp` is not in `author_src/` - see the "not done" item in the report). Using
    // the raw `shadow` here is the port's existing approximation of the same gate, and changing the RIM to a term
    // the rest of the chain does not have would be a second definition of "the shadow" rather than a port.
    const float3 rim_fresnel = rim_area * rim_head_gate * rim_colour_used * rig.rim.y * rim_brdf * min(ao_used, shadow_used) * rim_one_sided;
    // ---- RIM TWO IS THE LIGHT'S OWN, AND IT IS WHY A CHARACTER READS AS ROUND ----
    //
    // It is built from the light's direction PROJECTED INTO THE XZ PLANE - the article's "treat the character as a
    // cylinder lit from the side" - so it appears where the surface turns away from the light's AZIMUTH rather than
    // from the camera. `0.5 - (0.5 * NoLxz - 1) * NoLxz` reshapes that term so its peak sits at the silhouette, the
    // `NoV` mask keeps it off a surface that faces the camera (a rim there is a glow rather than an edge), and the
    // whole term is scaled by `_DayStrength` because with the sun gone there is no azimuth to take a rim from. Its
    // brightness follows the albedo's lit layer for the same reason rim one's does.
    //
    // THE ARTICLE'S OWN COLOUR TERM IS A CONSTANT HERE, and that is a units decision rather than an omission: its
    // `lerp(1, mainLightColor * mainLightIntensity, _DayStrength)` multiplies a NORMALISED hue by its own intensity
    // and comes out at white, while this engine's radiance is a real number (7.5 * sun_intensity) that would blow
    // the rim out. What sets the rim's brightness here is `_RimLightNoLxzStrength` and the family's strength.
    const float no_lxz = dot(n, float3(light_dir_xz.x, 6.10351562e-05, light_dir_xz.y));
    const float no_lxz_refine = (0.5 - (0.5 * no_lxz - 1.0) * no_lxz) * rig.day.x;
    const float no_v_rim_mask = smoothstep(0.0, 1.0, saturate(5.0 * (0.4 - no_v)));
    // THE AZIMUTH RIM IS SKIPPED FOR A FACE, which is the article's own arrangement: it replaces the base material's
    // rim with its mask-driven one rather than adding to it, because the two together put a band on the jaw and
    // another on the temple.
    const float3 rim_main_light_result = float3(1.0, 1.0, 1.0) * no_lxz_refine * no_v_rim_mask * min(ao_used, shadow_used) * max(float3(0.15, 0.15, 0.15), light_colour) * rig.rim.z * (sdf_lane != 0u ? 0.0 : 1.0);
    // THE SUM IS CLAMPED AT ZERO, which is the article's `max(rimLight_finalResult, 0)`: the two mechanisms can
    // cancel on a fragment that is at the silhouette of one and the inside of the other, and a negative rim would
    // carve a dark line into the character.
    //
    // AND THE EYE IS GATED OUT OF BOTH, which is the ARTICLE'S answer rather than a taste: `ZmdEyeToonMain.hlsl`
    // contains no `_Rim*` read at all (the rim lives in the base and face shaders), so an iris has no rim in the
    // reference. This port's own family table already states that fact - `toon_params.slang:384` writes
    // `p.rim_strength = 0.0` for the eye - but `params.rim_strength` is no longer read by THIS expression (see the
    // note at `:1472`), so that value only reaches the screen-space rim pass today. MEASURED (phase ② probe,
    // `deren-ab/eye_term_probe.py`, target `0,-0.47,0.20` distance 0.45, yaw 0/30/60): this rim is EXACTLY 0 on the
    // iris at yaw 0 and then grows to 4.6 and 165.6 (8-bit display mean over the eye's own lane), i.e. the iris at
    // yaw 60 was lit almost entirely by a rim the reference does not have - the reported "extra highlight at the
    // side". The gate is the same lane idiom as the `sdf_lane` one above, it is 0/1 rather than a strength, and no
    // colour or strength is tuned: the A/B changed 0 px outside the eye at every yaw, 0 px ON the eye at yaw 0, and
    // dropped the eye's mean luminance 171.6 -> 27.0 at yaw 60.
    //
    // ---- AND IT IS SWITCHED OFF FOR THE FAMILIES THE REWRITTEN CHAIN PORTS (step 2) ----
    //
    // WHY, IN ONE SENTENCE: THE REWRITTEN CHAIN HAS ITS OWN RIM, and this one is inside `toon_diffuse`, which that
    // chain calls for every non-eye family - so with both live the character wears TWO rims, which is not a
    // rendering subtlety but the wrong picture.
    //
    // WHY NOT FOR EVERY FAMILY, and this is the half that is easy to get wrong: the Goo reference states a rim for
    // `PBRToonBase` (base / skin / cloth) and `PBRToonBaseHair` (hair) and NOT for `PBRToonBaseFace`, whose
    // `Rim_Color` socket exists but whose rim-width and attenuation ports do not - the spec (§5.10, §9-U7) leaves
    // that consumer chain UNDETERMINED, so this step does not port the face's rim and the face must keep this one.
    // A family this step does not port (`none`, and the eye, which has its own gate already) keeps it too: the
    // rewrite's promise is that a feature is not LOST while it is being replaced.
    //
    // WHY THE `#ifndef` RATHER THAN A SECOND PARAMETER: the value of this switch is that it is legible at the site
    // it changes, and the unguarded arm has to stay a NO-OP rather than a re-derivation. The guarded arm keeps the
    // expression above EXACTLY as it is - its constants, its reads of `rig.rim`, all of it - and only zeroes the
    // RESULT, which is the smallest edit that can satisfy "the old pipeline's output does not move by one pixel"
    // for the three families that keep it. `VR_GOO_TOON_CHAIN` is defined by `shaders/goo_toon.slang` before it
    // includes this file, and `outline.slang` - the other consumer of this body - leaves it undefined, so the
    // outline keeps the old rim on every family, exactly as it has it today.
#ifndef VR_GOO_TOON_CHAIN
    const float3 rim = max(rim_fresnel + rim_main_light_result, float3(0.0)) * (toon_indices.w != 0u ? 0.0 : 1.0);
#else
    // THE FAMILIES THE REWRITTEN CHAIN'S RIM COVERS, spelled as the families rather than as `!= FACE` so a NEW
    // family is excluded by default: an unported family falls back to this rim, which is the safe direction.
    const bool rim_ported_by_goo = family == VR_FAMILY_BASE || family == VR_FAMILY_SKIN || family == VR_FAMILY_CLOTH || family == VR_FAMILY_HAIR;
    const float3 rim = rim_ported_by_goo ? float3(0.0) : max(rim_fresnel + rim_main_light_result, float3(0.0)) * (toon_indices.w != 0u ? 0.0 : 1.0);
#endif

    // ---- THE STYLIZED PBR HIGHLIGHT: A REFLECTION OF A LIGHT THAT IS PARTLY THE CAMERA ----
    //
    // THE ARTICLE'S SECOND BIG DEPARTURE from a stock BRDF, and the reason a toon face carries its highlight where
    // the artist put it rather than where the geometry says: the half-vector is built against a DIRECTION THAT IS
    // PARTLY THE CAMERA'S. A physical half-vector slides the highlight across the surface as either the sun or the
    // eye moves; here the sun's direction is mixed with a "forward light" - the camera's own forward axis, raised
    // toward the sun's pitch - and at `_DayStrength` 0 the sun's share is gone entirely, so the highlight answers
    // to the CAMERA alone and stays put while the character turns. That is the whole style: a highlight that is a
    // property of the SHOT rather than of the sky.
    //
    // THE LOBE IS THE PBR ONE, SIMPLIFIED TO THE PAIR THAT MATTERS: a GGX-like `D` and the cheap `V` it is
    // normally paired with, clamped, times F0 - where F0 COMES FROM THE MATERIAL'S OWN METALLIC/GLOSS MAP rather
    // than from a constant, which is what gives cloth a broad sheen and a metal-trimmed part its response without
    // a second shader.
    const float no_v_spec = saturate(dot(highlight_normal, v));
    const float roughness2 = max(roughness * roughness, 0.0078);
    const float3 forward_light_dir = normalize(float3(camera_forward.x, lerp(0.5, l.y, rig.day.x), camera_forward.z));
    const float3 spec_light_dir = l * rig.day.x + 2.0 * forward_light_dir;
    const float3 spec_half_dir = normalize(v * (2.0 + rig.day.x) + spec_light_dir);
    const float no_h = saturate(dot(highlight_normal, spec_half_dir));
    const float a2 = roughness2 * roughness2;
    float specular_d = (no_h * a2 - no_h) * no_h + 1.0;
    specular_d *= specular_d;
    specular_d = a2 / specular_d;
    float specular_v = no_v_spec * 2.0 + roughness2 + 9.99999975e-05;
    specular_v = 0.5 / specular_v;
    // THE MATERIAL'S OWN F0, the article's expression and the usual metal/dielectric split: a dielectric reflects
    // 4% scaled by its reflectivity channel, and a metal reflects its own albedo.
    const float3 f0 = 0.04 * reflectivity.xxx + metallic * (albedo - 0.04 * reflectivity.xxx);
    float specular_dv = specular_d * specular_v - 6.10351562e-05;
    specular_dv = clamp(specular_dv, 0.0, 20.0);
    // `float3` AND NOT `const`, because the hair branch below REPLACES the lobe with its LUT-driven highlight rather
    // than adding to it - the article's own arrangement for that family (see the Kajiya-Kay block).
    float3 specular_brdf = specular_dv * f0;
    // THE HIGHLIGHT'S OWN SHADOWING RE-USES THE DIFFUSE CHAIN'S WEIGHTS rather than inventing a third: the
    // article's `lerp(ao_shadow_NoFRamp, min_shadowEffect, _DayStrength)` is "how much light reaches here", and
    // its `selfAoShadowEffect` turns that into the self-occlusion factor the highlight is scaled by.
    const float ao_shadow_low_light = lerp(ao_shadow_no_f, min_shadow_effect, rig.day.x);
    const float self_ao_shadow = lerp(rig.other_light.z, 1.0, ao_shadow_low_light);
    const float3 specular_light = main_light_final * self_ao_shadow * (ao_shadow_low_light * 0.5 + 0.5);

    // ---- THE SPECULAR RAMP LANE, KEPT AS A GATE ON THE HIGHLIGHT RATHER THAN AS THE HIGHLIGHT ----
    //
    // THE ARTICLE'S BASE MATERIAL HAS NO HIGHLIGHT RAMP - the lobe above is its whole highlight - but the GAME'S
    // MATERIALS SHIP ONE (`_SpecRampMap`, on 23 of the 45) and it is the map the article's own HAIR section reads
    // as a colour LUT. So the port keeps the read and gives it the job it can do without contradicting the lobe: a
    // material that ships a ramp has its highlight SHAPED by it, and one that does not is gated by the family's own
    // threshold and softness. The remap, the host's unit-step bake and the half width are unchanged, which is why
    // that contract now belongs to THIS lane alone (see tests/test_toon_material_sidecar.cpp).
    //
    // A LANE OF 0 MEANS "DO NOT READ", the same contract every other lane uses.
    float spec_band;
    if (toon_indices.z != 0u) {
        const float spec_x = saturate(0.5 + (no_h - params.spec_center) * (character_ramp_half_width / max(params.spec_softness, 1e-4)));
        spec_band = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_indices.z), heap_sampler_texture, float2(spec_x, character_ramp_spec_band_v)).r;
    } else {
        spec_band = smoothstep(params.spec_center - params.spec_softness, params.spec_center + params.spec_softness, no_h);
    }
    // ---- THE HAIR'S HIGHLIGHT: KAJIYA-KAY, WITH ITS COLOUR OUT OF A LUT ----
    //
    // THE ARTICLE'S HAIR SHADER REPLACES THE LOBE ABOVE rather than adding to it, and both halves are the article's
    // own lines: the SHAPE comes from Kajiya-Kay (a strand is a cylinder, so the highlight is where the half-vector
    // is PERPENDICULAR to the strand's binormal - `1 - ToH^2`), and the COLOUR comes from the material's ramp map
    // read as a precomputed highlight (`_SpecRampMap`, the lane the base material uses as a band gate).
    //
    // THE FLATTENING IS WHAT MAKES IT READ AS HAIR: `highlight_normal - dot(highlight_normal, camera_right) *
    // camera_right` removes the component across the camera's axis, so every strand of the shell ends up with the
    // SAME binormal and the highlights line up instead of scattering - which is the article's own complaint about
    // its first attempt ("把法向上与 cameraRight 混合的部分去除，使法向在任何地方都只朝正前方"). Its weight is
    // `_SpecularTrick_Flatten`, a material value this repository cannot carry, and it is taken at 1 (fully flat),
    // which is the look the article settled on.
    //
    // THE UNRECOVERABLE KNOBS ARE THE ARTICLE'S MATERIAL VALUES AND SIT AT THEIR NEUTRAL NUMBERS, each named where
    // it is used: `_ViewDirYOffset` = 0 (no lift of the view direction), `_BiNormalOffset_specularLut` = 0 (no
    // offset along the normal), `_LutVPowStrength` = 1 (the LUT's V ramp unwarped), `_SpecularBackF0` = 0 (no
    // halo). A value with no source is NOT invented here - the ones that exist in the game's own table are the
    // channels this reads (`reflectivity`, the R mask, smoothness), and the rest would be tuning.
    if (toon_lanes2.x != 0u) {
        const float3 camera_right = normalize(cross(float3(0.0, 1.0, 0.0), camera_forward) + float3(1e-6, 0.0, 0.0));
        const float3 cylinder_normal = normalize(highlight_normal - dot(highlight_normal, camera_right) * camera_right);
        const float3 flat_normal = normalize(lerp(highlight_normal, cylinder_normal, 1.0));
        const float3 fake_tangent = normalize(cross(float3(0.0, 1.0, 0.0), flat_normal));
        const float3 binormal_flat = normalize(cross(flat_normal, fake_tangent));
        // `orm.x` IS THE WEIGHT between the flattened binormal and the smooth frame's own - the same channel the
        // metallic branch above declines to read as metallic, used here for what the article uses it for.
        const float3 binormal_lut = normalize(lerp(binormal_flat, hair_bitangent, orm.x));
        const float to_h = dot(normalize(v + l), binormal_lut);
        const float to_h_shape = max(0.0001, sqrt(max(0.0, 1.0 - to_h * to_h)));
        // THE U COORDINATE IS A SHARPENED CURVE, not the raw `1 - ToH^2`: `exp2(200 * log2(x))` is `x^200`, which
        // is what turns a broad band into the thin line a strand's highlight is, and `reflectivity` scales how far
        // up the LUT it reaches.
        const float lut_u = saturate(exp2(200.0 * log2(to_h_shape)) * reflectivity);
        const float2 view_proj = float2(dot(v, camera_right), dot(v, camera_forward));
        const float2 normal_proj = float2(dot(highlight_normal, camera_right), dot(highlight_normal, camera_forward));
        const float vohn = saturate(dot(view_proj, normal_proj));
        const float lut_v = vohn * vohn * step(0.0, to_h);
        const float3 lut_f0 = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_indices.z), heap_sampler_texture, float2(lut_u, lut_v)).rgb * f0;
        // THE `* 7` IS THE ARTICLE'S, and it is what makes a LUT entry a HIGHLIGHT rather than a tint: the map holds
        // a fraction of F0 and the factor brings it into the range a specular highlight needs.
        //
        // AND THERE IS NO `/ PI` HERE ANY MORE, because the question this comment used to leave open - "whether the
        // whole specular path should divide by PI" - is ANSWERED by the author's own source: nothing in
        // `ZmdToonMain.hlsl` or `ZmdEyeToonMain.hlsl` divides by PI, and the article's own line (p52 of the deck) is
        // `finalF0 = lutF0 * 7 + backF0`. The `/ PI` was this port's bridge for feeding the engine's
        // `vec3(7.5 * sun_intensity)` into a chain tuned for a light colour of order 1, and that bridge now lives
        // where the light is built (`main_light_intensity` above, the author's own normalization). Keeping it here
        // as well would divide ONE family by PI twice and leave the others untouched - which is exactly the
        // asymmetry the old note was apologising for.
        //
        // THE SILVER-HAIR BLOW-OUT THAT PROMPTED IT IS NOW PREVENTED AT THE SOURCE: `main_light_final` is the
        // author's O(1) light, so a LUT whose R channel sits at 0.8..1.0 (`measured` on
        // `T_actor_common_hair_08_RS`) lands at 7 * 0.8..1.0 * O(1) rather than 7 * 0.8..1.0 * 7.5.
        spec_band = 1.0; // the LUT IS the shape here, so the band gate that shapes the lobe has nothing left to do
        specular_brdf = lut_f0 * 7.0;
    }

    // GATED BY `_SpecularStrength` AND NOTHING ELSE, which is the ARTICLE'S OWN composition and not a tuning:
    // every one of its four mains multiplies the specular by that one scalar and stops there -
    //     `ZmdToonMain.hlsl:271-272`:  `mainLightSpeuclarResult = specularLight * specular_brdf;`
    //                                  `mainLightSpeuclarResult *= _SpecularStrength;`
    //     `ZmdSkinToonMain.hlsl:238-239`, `ZmdHairToonMain.hlsl:279-283`, `ZmdEyeToonMain.hlsl:219` - the same
    //     pair. Its specular's SHADOWING is already inside `specularLight`: `selfAoShadowEffect` (from
    //     `ao_shadow_lowLight`) is the article's own self-occlusion factor and is what makes a highlight fade in
    //     a crevice.
    //
    // THE PORT HAD TWO EXTRA GATES HERE - `* shadow` and `* light_side` - and they are this port's, not the
    // article's: `shadow` is the ENGINE's cast-shadow term (the article's `shadowScene`, which never reaches its
    // specular at all) and `light_side` is the port's `smoothstep(0, 0.35, dot(n, l))` mask, which exists for the
    // RIM and has no counterpart in any of the four mains. MEASURED on this character (see the report's item 7):
    // the two gates were suppressing the highlight on the shadowed side and at grazing angles, which is exactly
    // where the article's own `selfAoShadowEffect` already decides how much highlight survives.
    const float3 main_specular = specular_light * specular_brdf * spec_band * specular_strength * rig.env.z;

    // ---- THE EYE'S HIGHLIGHT: A MATCAP ON A CORNEA THE UV ITSELF DESCRIBES ----
    //
    // AUDITED LINE BY LINE AGAINST `ZmdEyeToonMain.hlsl` (the article's eye shader), and the result is recorded here
    // because "it matches the article" is a claim that should be checkable rather than asserted:
    //   * L101-103  `finalEyeUV` read, `albedo = tex * _BaseColor`, `albedo_dark = albedo * _AlbedoDarkStrength`
    //               (with NO saturation step - see toon_params' note on the eye's `albedo_dark_saturation`) - HERE
    //   * L105-119  the cornea hemisphere, its `lerp` to (0,0,1) by `eyeCenterArea`, the `.x = -x` flip, and the
    //               TBN assembly - HERE, on `surface_tbn` rather than the vertex TANGENT attribute
    //   * L121-124  `energyDistribution_metallic = 0.96`, the lit and dark albedos - HERE
    //   * L126-143  the `_FACIAL_ON` branch - NOT TAKEN, and correctly: it is the variant where the FACE material
    //               draws the eye (`corneaNormalWS = i.normalWS`, a LUT dark side); chen's iris is its own material
    //               with `_UseMatcap = 1` and `_UseShadowLutTex = 0`, which is the article's non-facial eye
    //   * L145-166  the light flattening, `NoL`, the ramp's colour and `w`, and `NoF` off the same normal - HERE,
    //               EXCEPT the flattening: `_FaceRight` / `_FaceUp` / `_FaceForward` are globals the game supplies
    //               and no sidecar carries, so the light is used unflattened (the one difference this port cannot
    //               close, stated where it happens)
    //   * L168-169  both trick colours, `lerp(1, colour * 2.5, weight)`, `eyeCenterArea` and the albedo's alpha
    //               as the two weights - HERE, and the polarity is the article's own (a round of this port flipped
    //               it to brighten the iris and was reverted: see the note at `eye_trick`)
    //   * L170-205  `trickAlbedo`, the other-light and day blends, the three albedo layers and the dark-in-dark
    //               saturation move - HERE, in the composition phase 1 ported for every family (the port blends the
    //               layer weights with `min(ao, shadow, ramp_weight)`, which for an eye is `ramp_weight`: it has no
    //               ORM lane, and the article's eye reads no AO and no shadow map at all)
    //   * L206-226  `DayDarkEffect`, the view-space matcap uv, `specularColor * _SpecularStrength + _SpecularColor
    //               * specularW`, and `specularDarkEffect` - HERE (see the block below)
    //   * L229-233  `mainSpecularResult + mainDiffuseResult * lerp(1, mainTex_var.w, _DiffuseBlendEffect)` - HERE,
    //               minus this engine's sky irradiance and the family's rim, NEITHER of which the article's eye
    //               carries (see `ambient_strength` and the eye's `rim_strength`)
    //
    // TWO DIFFERENCES ARE STRUCTURAL AND REMAIN, both written where they occur: the unflattened light above, and the
    // port's phase-1 layer composition (shared by every family rather than special-cased for the eye). Everything the
    // article's eye shader states per material - `_ParallaxDepth`, `_CorneaBumpStrength`, `_SpecularStrength`,
    // `_SpecularColor`, `_AlbedoDarkStrength`, `_NoLOffset`, `_MainDiffuseColorLightStrength`, `_DiffuseDarkStrength`
    // - is either read from chen's own row or is a no-op at the article's declared default.
    //
    // THE ARTICLE'S THIRD USE OF THE MATERIAL'S UV, and the reason this family cannot use the lobe above: an iris is
    // a FLAT quad, so its normals point at the camera everywhere and a spherical lookup reads ONE texel. MEASURED:
    // with the game's own ball (`T_actor_common_matcap_06_D`, whose centre is black and whose catchlight is a rim)
    // the matcap highlight was a 0-PIXEL difference on this character - which is what sent this block to the uv.
    //
    // SO THE NORMAL IS MANUFACTURED, ONCE, ABOVE: the uv's own offset from the eye's centre becomes the tilt of a
    // HEMISPHERE (`z = sqrt(1 - |uv|^2)`), smooth by construction, and both this matcap and the ramp before it read
    // that ONE normal (`shading_normal`) - which is the article's own arrangement: its eye takes `NoL`, `NoF` and
    // the matcap's uv from the same `corneaNormalWS`. `step(0.25, |uv|^2)` is the article's line between the two
    // zones: inside it the hemisphere stands, outside it the quad's flat normal does, which is what keeps the
    // catchlight on the iris rather than on the sclera.
    float3 eye_highlight = float3(0.0, 0.0, 0.0);
    if (toon_indices.w != 0u) {
        const float4 eye_matcap = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_indices.w), heap_sampler_texture, toon_matcap_uv(shading_normal, camera_forward));
        // THE ARTICLE'S OWN TWO TERMS, in its own order: `specularColor * _SpecularStrength + _SpecularColor *
        // specularW`. The first is the BALL's colour gated by the material's `_Specular` - chen's iris states 0.0, so
        // on this character that term is dead and the highlight is the second one alone; the second is the article's
        // `_SpecularColor` (its own property, white by default) times the ball's W, which is the channel its comment
        // treats as the catchlight's own mask.
        //
        // THE `_Specular` HERE IS THE SAME RESOLVED `specular_strength` THE LOBE ABOVE USES - the material's own
        // value where the asset states one, the family's otherwise - because the article's eye means the same
        // property in both places and a second read of the raw family number would let the two disagree.
        //
        // THIS PORT USED TO COMPUTE `matcap.rgb * matcap.w`, gated on `(1 - eyeCenterArea)` and scaled by the rig's
        // environment strength. NONE OF THAT IS THE ARTICLE'S and all of it is gone: the gate was this port reading
        // the cornea zone a second time, and the article's matcap lookup already lands on the sclera's flat normal
        // outside that zone by construction.
        const float3 specular_brdf = eye_matcap.rgb * specular_strength + character_eye_specular_colour * eye_matcap.w;
        // `DayDarkEffect = lerp(rampTex_NoF, rampNoL, _DayStrength)` and the two lines under it: half-and-half with
        // the lit end, and `_SelfAoShadowStrength` (the rig's `other_light.z`) holding the highlight down wherever
        // the surface is dark.
        const float day_dark_effect = lerp(no_f_weight, ramp_weight, rig.day.x);
        const float specular_dark_effect = (day_dark_effect * 0.5 + 0.5) * lerp(rig.other_light.z, 1.0, day_dark_effect);
        eye_highlight = main_light_final * specular_brdf * specular_dark_effect;
    }

    // ---- THE ARTICLE'S OWN BLEND OF THE TWO HALVES ----
    //
    // THE DIFFUSE STEPS ASIDE WHERE THE MATERIAL WANTS THE HIGHLIGHT TO CARRY THE SURFACE, and the weight is the
    // ALBEDO'S OWN ALPHA - the one channel of the game's `_BaseMap` that is not colour. The strength of the step
    // is `_DiffuseBlendEffect`'s, in the rig, so a material whose alpha carries nothing (the face's is a constant
    // 1 in every material this repository loads) comes out exactly where it was.
    //
    // THE HIGHLIGHT IS ADDED, NOT MULTIPLIED BY THE ALBEDO, which is the difference from the band this replaced:
    // F0 already carries the albedo for a metal and 4% for a dielectric, so a second multiply would darken every
    // highlight that is not on a bright surface.
    //
    // THE SKIN IS THE ONE FAMILY THAT DOES NOT GET THIS STEP, and that is the article's own composition rather
    // than a tuning: all four of its other mains write
    //     `float diffuseSpecularBlend = 1 - _DiffuseBlendEffect * (1 - mainTex_var.w);`
    //     `float3 mainLightResult = mainDiffuseResult * diffuseSpecularBlend + mainLightSpecluarResult;`
    // (`ZmdToonMain.hlsl:275-276`, the hair's `:283`, the eye's `:229`), while the SKIN's is ONE line with no
    // blend at all - `ZmdSkinToonMain.hlsl:242`:
    //     `float3 mainLightResult = mainDiffuseResult + mainLightSpecluarResult;`
    // The reason is visible in the other change this family needed: the skin's albedo ALPHA is its OCCLUSION
    // (`:154`, see `ao_used`), so using it as a diffuse/highlight MIX as well would make the same channel do two
    // unrelated jobs - the article gives the alpha to the occlusion and leaves the blend out of this shader.
    const float diffuse_specular_blend = (family == VR_FAMILY_SKIN) ? 1.0 : (1.0 - rig.env.w * (1.0 - albedo_alpha));

    // ---- THE ENVIRONMENT'S SPECULAR HALF: A POLYNOMIAL INSTEAD OF THE ENGINE'S LUT ----
    //
    // THE ENGINE ALREADY HAS A SPLIT-SUM IBL (`shaders/ibl_specular.glsl`, shared with the deferred path) AND THIS
    // STAGE DELIBERATELY DOES NOT USE ITS BRDF HALF, which is the article's choice rather than an oversight: the
    // game's highlight comes from a FIT of the same integral - the pair of polynomials below, the DFG fit NVIDIA's
    // NRD uses - plus a KULLA-CONTY term for the energy a single-scattering fit loses. A toon character is exactly
    // where a LUT's texel grid shows: on a surface as smooth as an eye, a bilinear lookup reads as a facet.
    //
    // WHAT IS SHARED WITH THE ENGINE IS THE ENVIRONMENT ITSELF (`env_cube_sample_lod`), because the sky a toon
    // character reflects has to be the sky the rest of the frame reflects - a second cubemap for the character
    // would put it in a different world from the ground it stands on.
    const float no_v2 = no_v * no_v;
    const float no_v3 = no_v2 * no_v;
    const float roughness4 = roughness2 * roughness2;
    const float roughness6 = roughness4 * roughness2;
    const float ibl_numerator = (3.32707 * no_v + 0.0365463) + (-9.04755 * no_v + 9.0632) * roughness2;
    const float fit_x = 3.59685 * no_v2 - 1.36772 * no_v3 + 1.0;
    const float fit_y = 9.22949 * no_v3 - 16.3174 * no_v2 + 9.04401;
    const float fit_z = -20.2123 * no_v3 + 19.7886 * no_v2 + 5.56589;
    const float ibl_denominator = dot(float3(fit_x, fit_y, fit_z), float3(1.0, roughness2, roughness6));
    const float ibl_brdf = ibl_numerator / max(ibl_denominator, 1e-6);
    const float env_scale = dot(float2(dot(float2(-1.28514, 1.0), float2(no_v, 0.990440011)), dot(float2(1.0, -0.75591), float2(1.29678, no_v))), float2(1.0, roughness2));
    const float bias_x = dot(float3(2.92338, 59.4188, 1.0), float3(no_v, no_v3, 1.0));
    const float bias_y = dot(float3(1.0, -27.0302, 222.592), float3(20.3225, no_v, no_v3));
    const float bias_z = dot(float3(626.130, 316.627, 1.0), float3(no_v, no_v3, 121.563004));
    const float env_bias = env_scale / max(dot(float3(bias_x, bias_y, bias_z), float3(1.0, roughness2, roughness6)), 1e-6);
    const float3 ibl_brdf_final = ibl_brdf * f0 + env_bias;
    // THE KULLA-CONTY COMPENSATION, and the reason the article needs it at all: a single-scattering BRDF loses the
    // light that bounces more than once on a rough surface, which reads as a dark edge on metal. The term adds back
    // `F0 * (1 - albedo) / albedo` of the energy the fit says is missing, which is the standard approximation.
    const float directional_albedo = max(ibl_brdf + env_bias, 1e-3);
    const float3 multi_scatter = f0 * ((1.0 - directional_albedo) / directional_albedo);
    const float3 final_ibl_brdf = ibl_brdf_final * (1.0 + multi_scatter);

    // THE REFLECTION IS MIRRORED IN X AND Z BEFORE THE ROTATION. Those two lines are the article's and they are a
    // convention of the ENVIRONMENT MAP it reads rather than of the maths - the game's cube is stored with those
    // axes flipped - and the Y rotation exists so an artist can turn the sky without turning the sun.
    float3 reflect_dir = reflect(-v, n);
    reflect_dir.x = -reflect_dir.x;
    reflect_dir.z = -reflect_dir.z;
    // `_EnvRotation` is DEGREES in the rig, and this is the one place it becomes radians.
    const float env_angle = rig.env.y * 0.0174532925;
    float env_sin = 0.0;
    float env_cos = 0.0;
    sincos(env_angle, env_sin, env_cos);
    const float3 env_dir = float3(reflect_dir.x * env_cos - reflect_dir.z * env_sin, reflect_dir.y, reflect_dir.x * env_sin + reflect_dir.z * env_cos);
    // THE LEVEL IS THE ARTICLE'S CURVE, CLAMPED TO THIS RENDERER'S CHAIN, and the clamp is a documented difference
    // rather than a silent one: `log2(roughness) * 1.2 + 5` assumes the game's own prefiltered chain, while this
    // engine's is `[lighting] env_mip_count` mips deep (5 by default). The curve's SHAPE is kept - a smooth
    // surface reads a sharper mip than linear-in-roughness would give it - and the range is the asset's.
    const float env_level = clamp(log2(max(0.01, roughness)) * 1.2 + 5.0, 0.0, max(0.0, float(env_cube_levels() - 1)));
    const float3 env_map_colour = env_cube_sample_lod(env_dir, env_level);
    // `selfAoShadowEffect` again, which is the article's: an environment reflection is occluded by the same
    // self-shadowing the highlight is, or a crevice shines.
    const float3 indir_light_specular = env_map_colour * final_ibl_brdf * rig.env.x * self_ao_shadow * params.ibl_specular_strength;

    // ---- STEP 4 APPLIES ITS TWO FACTORS HERE, WHICH IS WHERE THE REFERENCE APPLIES THEM ----
    //
    // TO THE SUM OF THE FOUR TERMS OF `混合.011`, and NOT to `eye_highlight` (the port's own iris overlay) or to
    // the article's `rim` - the reference adds its rim to the product rather than inside it. With `goo_arm` false
    // the else-branch is the expression this line has always been, CHARACTER FOR CHARACTER: the `goo_toon = false`
    // frame has to stay byte-identical, and folding the two no-op factors into it would re-associate the additions
    // (IEEE addition is not associative, so "multiplying by 1.0" is not the identity a re-association is not).
    //
    // ---- STEP 5 REPLACES THE OTHER THREE TERMS IN THE *TRUE* BRANCH, ONE TERNARY EACH ----
    //
    // `goo_spec_direct` / `indir_light_specular` / `ambient` are three of the four summands, and step 5 hands back
    // the reference's `directLighting_specular` / `IBL_specular` / `IBL_diffuse` for them. THE ELSE BRANCH IS
    // UNTOUCHED TEXT, which is the byte-identity criterion - and the ternaries are inside the TRUE branch precisely
    // so that they cannot perturb it. `direct * diffuse_specular_blend` is step 4's and is NOT one of these: its
    // own note (`:2264`) records that the reference's `混合.011` has no blend and that the port keeps it because
    // it was already applied to `direct` before this rewrite began.
    //
    // WHAT THAT MEANS FOR `diffuse_specular_blend` AND THE THREE NEW TERMS (`G16`, the spec's §9-U3): the reference
    // multiplies NOTHING by it - its four terms are added and the sum is then gated by `混合.004` and tinted by
    // `混合.006`, which is `goo_factor` here - so a port that applied it to the four terms would not be the
    // reference either. THIS STEP'S CHOICE IS TO APPLY IT TO NONE OF THE THREE: leaving the article's blend on
    // `direct` alone is the state step 4 landed and verified, and extending it to three terms the reference does
    // not have would be a second departure made in the same breath as the first. The consequence is named rather
    // than hidden: inside the `goo_arm` branch the sum is
    //
    //     direct * blend + (reference direct specular + reference IBL diffuse + reference IBL specular)
    //
    // i.e. ONE term of four still wears an article weight, and the spec's §9-U3 (which asks for a render A/B to
    // settle whether it should come off as well) is LEFT OPEN by this step rather than silently answered.
    const float3 toon_colour = goo_arm
                                   ? (direct * diffuse_specular_blend + goo_spec_direct + goo_spec_ibl + goo_diff_ibl) * goo_factor + eye_highlight + rim
                                   : direct * diffuse_specular_blend + main_specular + eye_highlight + indir_light_specular + ambient + rim;

    // ---- NO MATCAP MULTIPLY HERE, AND THAT IS THE ARTICLE'S OWN ANSWER ----
    //
    // THIS BLOCK USED TO BE `toon_colour * (1 + max(matcap, 0) * character_matcap_strength)`, cited to
    // `EfEyeIrisApplyMatcap05`. That function is the GAME'S, not the article's: read against the author's own
    // repository (`github.com/qiudashu233/MyZmdShaders`, commit 6507457), NEITHER `ZmdToonMain.hlsl` (the base
    // toon) NOR `ZmdEyeToonMain.hlsl` (the eye) multiplies the finished colour by a matcap - the eye's
    // `_SpecularMatcap` is consumed by `specularBrdf = specularColor * _SpecularStrength + _SpecularColor *
    // specularW` and by nothing else, and the base toon has no matcap at all. The port had taken the multiply
    // from the reference port (DanbaidongRP), which follows the game; this stage follows the ARTICLE, so the
    // multiply is gone and the lane is read where the article reads it (`eye_highlight` below).
    //
    // WHAT THE LANE STILL DOES: it gates the eye block (`toon_indices.w != 0`), which is how the article's eye
    // shader is selected, and it is the `_MatcapTex`/`_SpecularMatcap` image the highlight samples. A material
    // with no matcap lane is unaffected either way - the removed factor was exactly 1.0 for it.
    //
    // ---- STEP 6 HANDS ITS SATURATION OUT RATHER THAN APPLYING IT HERE, AND THE REASON IS THE RIM THAT IS IN `toon_colour` ----
    //
    // `toon_colour` above is POST-RIM - the reference's `混合.019` - which is the side of the rim the step-6 block's
    // links put `色相/饱和度/明度` on, and `out_desaturation` is therefore already correct for it. What is NOT here
    // is the reference's last additive term before that node: `混合.026 = 混合.019 + emission`. The caller adds
    // `s.emissive` to what this returns, so the reference's desaturation - which sits AFTER `混合.026` - belongs at
    // the caller, one line down, where the pixel is finished. See `main`'s `lit`.
#if defined(VR_GOO_TOON_CHAIN)
    // ---- STEP 7's LAST NODE REACHES THE FACE ON THE PATH THE FACE ACTUALLY TAKES ----
    //
    // The reference multiplies a face's FINISHED colour by exactly one factor:
    // `混合.020 = MIX(Face Final brightness, Eyes white Final brightness, f = CsutmMask.G > 0.5)`, the `Strength` of
    // `自发光(发射)`. The Goo face arm above implements it at `:2494-2495` and returns at `:2500`, but that arm is
    // gated off on this asset (`:2245`'s `goo_face_block2.w != 0` needs `_GooFaceSDF`, which the user's own step-8
    // decision removed), so on the shipped sidecar the face never sees the node at all. The multiplicand is the same
    // quantity the arm multiplies - the composed colour, post-rim (`:3583-3589`) - so landing here adds no deviation.
    //
    // EVERY NUMBER IS READ THE WAY THE ARM READS IT: lane 21 (`toon_colour_lane::goo_face_scalars_b`) carries
    // `Face Final brightness` in `.x` and `Eyes white Final brightness` in `.y`, and its absent-row value is the
    // `-1000` sentinel (`primitive.cppm`'s step-7 block), so an absent row answers the reference's own group default
    // (`goo_face_brightness_default` = 1.15) instead of multiplying the pixel by zero.
    if (family == VR_FAMILY_FACE) {
        const float4 face_scalars_b_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 21u);
        const float face_brightness = face_scalars_b_lane.x > goo_lane_absent_threshold ? face_scalars_b_lane.x : goo_face_brightness_default;
        const float face_eye_brightness = face_scalars_b_lane.y > goo_lane_absent_threshold ? face_scalars_b_lane.y : goo_face_eye_brightness_default;
        // `CsutmMask` is uploaded UNORM (`runtime.constructor.cppm:1423`), so `.g > 0.5` compares the STORED texel -
        // the reference's own comparison, and the same read the face arm performs at `:2494`.
        const float face_csumt_g = heap_texel(heap_at<Texture2D<float4>>(heap_slots_textures + toon_lanes3_at(heap_slots_toon_lanes, material_index).y), heap_sampler_texture, uv).g;
        const float face_emission_strength = face_csumt_g > 0.5 ? face_eye_brightness : face_brightness;
        out_desaturation = goo_desaturation;
        return toon_colour * face_emission_strength;
    }
#endif
    out_desaturation = goo_desaturation;
    return toon_colour;
}

[shader("fragment")]
float4 main(
    [[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
    [[vk::location(1)]] float3 v_normal : TEXCOORD1,
    [[vk::location(2)]] float2 v_uv : TEXCOORD2,
    bool v_is_front : SV_IsFrontFace) : SV_Target
{
    // The same surface the G-buffer pass wrote, gathered by the same function: a normal-map fetch, the
    // double-sided flip and the material's factors cannot disagree with the G-buffer if there is one
    // implementation of them.
    const surface_sample s = gather_surface(v_world_pos, v_normal, v_uv);

    // THE PER-FAMILY RAMP, selected by the family the LOADER classified this material's name into (see
    // gltf_loader's toon_family_of) and carried through the material record. An unclassified material gets
    // `toon_params_for(0)`, which is the family-independent path.
    const toon_params params = toon_params_for(s.toon_family);
    // THE LANES BESIDE THE RECORD, read once here: `.x` is the face SDF map the SDF lane shades with, and
    // `.y` is the metallic/gloss map, which the shading below reads as its metallic, reflectivity, occlusion
    // and smoothness - the four MATERIAL numbers glTF's own five slots cannot carry (see `deren::vulkan::toon_slot`).
    const uint4 toon_lanes = toon_lanes_at(heap_slots_toon_lanes, push.material_index);
    // STEP 6's SATURATION COMES BACK OUT OF THE SHADING (see the out-parameter's note): it is the reference's
    // `色相/饱和度/明度.Saturation`, and it is applied to the FINISHED PIXEL below rather than to this stage's
    // rim-inclusive colour, because the reference's own chain adds emission between the two.
    float desaturation = 0.0;
    // STEP 8'S DECODE, in place of the argument this call used to pass: `goo_toon_shading_normal` answers
    // `normalize(s.normal)` unchanged unless the Goo chain is the one being compiled, the material reads a normal
    // map and lane 24 states a strength - see its note for the reference's formula, the dump nodes it was read from
    // and the reason the G-buffer's own normal is deliberately left where it is.
    const float3 colour = toon_diffuse(s.albedo, s.alpha, s.toon_family, goo_toon_shading_normal(v_world_pos, v_normal, v_uv, normalize(s.normal)), v_world_pos, s.ao, params, s.toon_indices, v_uv, toon_lanes, push.material_index, desaturation);

    // ---- THE EMISSIVE TERM, ADDED AFTER THE LIGHTING LIKE EVERY OTHER STAGE'S ----
    //
    // IT WAS BEING GATHERED AND THROWN AWAY, and the loss was invisible rather than absent: `gather_surface`
    // already fetches `s.emissive` (the material's emissive factor times its emissive map, `surface.glsl`), this
    // stage simply never read it. What that costs is not "the toon shade is missing a term" but "an emissive
    // toon material lost the emission the G-buffer had already added": the G-buffer pass ADDS emissive into the
    // scene colour (`gbuffer.slang`: `out.scene_color = float4(s.emissive, 1.0)`, blended ONE/ONE), and this
    // pass then OVERWRITES that same pixel with the toon result - so the emission has to be put back, or a
    // character whose material ships one (chen's cloth carries an emissive map and factor 0.57/0.81/0.88) is
    // shaded flat where the engine's own forward path would glow.
    //
    // THE POSITION IS THE ENGINE'S, NOT AN INVENTION: `shading.glsl` composes `ambient + direct + specular_ibl +
    // s.emissive`, i.e. emission is added after everything that is lit, and it is not attenuated by shadow or
    // ao - it is radiance the surface produces, not light it receives. `_EmissionBrightness` (the game's own
    // scalar, 1.0 on every material of every character in this repository) is deliberately NOT applied here:
    // glTF's `emissiveFactor` already carries what the game stated, and multiplying by that scalar would be a
    // second place for one number to live.
    const float3 lit = colour + s.emissive;

    // ---- STEP 6: THE REFERENCE'S DARK-SIDE DESATURATION, ON THE FINISHED PIXEL ----
    //
    // `色相/饱和度/明度` is the LAST node of the reference's graph and it reads `混合.030`, i.e. the sum of the
    // pre-rim colour, the rim, the emission and the RS-EFF branch. THIS LINE IS THAT POSITION: `lit` is
    // `混合.026` (the port adds nothing after its emission), and `desaturation` is
    // `钳制.004 = clamp(luma(RampColor) + Color desaturation in shaded areas attenuation, 0, 1)` - computed in
    // `toon_diffuse`'s step-6 arm, where `luma` is the same ramp sample step 4 shades with. The term is
    // `lerp(luma(lit).xxx, lit, saturation)`, and the two anchors of that family are worth stating because they
    // are what makes it safe for a material this step does not cover: `saturation = 1` is the IDENTITY, and
    // `saturation = 0` is grey. The value is `goo_desaturation_neutral` (the identity) for every material the
    // step-6 arm does not take, so a FACE, a HAIR, an EYE or a `goo_toon = false` frame passes through this line
    // unchanged - which is the byte-identity the `goo_toon = false` anchors check.
    const float3 lit_final = goo_hsv_desaturate(lit, desaturation);

    // the BLEND rule, copied from unlit.slang: an alphaMode BLEND material carries its own coverage, and
    // every other material is fully opaque regardless of what its base colour's alpha says
    //
    // ... AND THE ARTICLE'S OWN TRANSPARENT VARIANT IS THE SECOND TERM HERE (`_TRANSPARENT_ON`, record bit8 -
    // see `toon_inputs::alpha_blend`). It is a DIFFERENT statement from glTF's: that material's glTF
    // `alphaMode` is OPAQUE and its sidecar is what says "blend, SrcAlpha / OneMinusSrcAlpha", so without this
    // bit the port draws a surface the author blends. The article's coverage for that variant is
    // `mainTex_var.w * _CustomAlpha` (`ZmdToonMain.hlsl:386`), i.e. the albedo alpha times a material scalar
    // whose default is 1; the port carries no `_CustomAlpha` row and the game's own value is 1.0, so the
    // albedo alpha IS the coverage.
    const bool toon_transparent = (s.flags & 256u) != 0u;
    const float out_alpha = ((s.flags & 32u) != 0u || toon_transparent) ? s.alpha : 1.0;
    return float4(lit_final, out_alpha);
}