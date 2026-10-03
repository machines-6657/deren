// shaders/toon_params.slang - THE TOON FAMILY TABLE, shared by every stage that shades a character.
//
// WHY IT IS A FILE OF ITS OWN: two stages need it - the character-forward stage, which shades the surface,
// and the screen-space rim, which adds the contour over it - and they must agree about a family's
// parameters, or the two would draw one character with two different looks. A second copy of the table
// inside the rim stage is exactly the kind of duplicate this project refuses elsewhere: the failure is
// silent, and it shows only as a contour that does not match the shading under it.
//
// SLANG SYNTAX, deliberately: it is included by `.slang` stages only, so it uses `float3`/`uint` rather than
// the GLSL spellings. It touches no heap slot and declares nothing, so it may be included at any point in a
// stage's include order.
//
// THE FAMILY IDS MUST MATCH `deren::gltf::toon_family` (see gltf_loader.cppm) - the same enum, written out here
// because a shader cannot include a C++ enum, and the numbers ARE the contract.
/**
 * @brief the toon stage's ramp, PER MATERIAL FAMILY
 *
 * WHY A RAMP AND NOT THE ENGINE'S `toon_band`, which is measured rather than assumed: `toon_band` is a
 * QUANTIZER - it multiplies by the step count, takes `floor`, and steps at the half-way point of each cell,
 * so a value lands on the nearest band and the output stays close to the input on average. A ramp is a
 * DIFFERENT shape: flat through the shadow side, a jump at one threshold, flat through the lit side.
 * Rendering the quantizer and the ramp and diffing them showed the quantizer moving 48624 pixels by a
 * maximum of 7/255 - present, and visually invisible, which is exactly what "the banding does not work"
 * looks like when the term is technically active.
 *
 * WHY PER FAMILY, and where the numbers come from. The references carry this pair PER MATERIAL, and the
 * differences between the families are the visible ones: hair's shadow edge is the hardest and sits highest
 * (a strand's terminator is a line), skin's is the softest and lowest (a soft terminator across a curved
 * cheek), and cloth and the eye sit between. `EF_*_SHADOW_CENTER` / `EF_*_SHADOW_SMOOTHNESS` in the MME
 * pack and `Sigmoid(blend, offset, x)` in MyZmdShaders are the same two parameters, and each family's entry
 * file gives them different values.
 *
 * THE DIFFUSE RAMP'S THREE FIELDS BELOW ARE A **FALLBACK**, AND SAYING SO IS A CORRECTION: `center`, `softness`
 * and `shadow_tint` are read only by the PROCEDURAL branch of character_forward.slang, i.e. by a material whose
 * model ships no `_DiffRampMap` or whose artist switched it off. When the model HAS one the shader samples the
 * texture itself - its RGB as the ramp's colour and its alpha as the step (`rampTex_var.xyz` / `.w`, the article's
 * own two reads) - and these three numbers then do nothing at all. That is the normal case rather than the corner
 * one: every one of the 45 materials across the five characters in `chars\` names a `_DiffRampMap`, and chen.glb
 * carries the four images (`T_actor_common_{body,face,cloth_04,hair_01}_RD`, 256x1). AN EARLIER VERSION OF THIS NOTE
 * CLAIMED THE OPPOSITE - that the reference's ramps are "assets this port cannot carry" - which is how a fabricated
 * per-family tint got blamed for a colour cast that was coming from somewhere else entirely (the eye's `color`
 * rows; see `eye_trick`). What is still ours, and still ordered rather than measured, is everything the ramp does
 * not provide: the specular band, the rim, and the per-family scalars.
 * The values are the references' relative order where the game states nothing - the STRUCTURE the order expresses:
 * which family gets the hard edge, which gets the soft one, and which side of the albedo each one's tint moves
 * toward.
 *
 * THE FAMILY IDS MUST MATCH `deren::gltf::toon_family` (gltf_loader.cppm) - they are the same enum, written out
 * here because a shader cannot include a C++ enum and the numbers ARE the contract.
 */
#define VR_FAMILY_BASE 1u
#define VR_FAMILY_SKIN 2u
#define VR_FAMILY_FACE 3u
#define VR_FAMILY_HAIR 4u
#define VR_FAMILY_EYE 5u
#define VR_FAMILY_CLOTH 6u

/// @brief one family's toon parameters: its diffuse ramp, its specular ramp, and its rim
struct toon_params {
    // ---- the DIFFUSE ramp ----
    float center;       // the threshold: where the dark side ends
    float softness;     // its edge width
    float3 shadow_tint; // the colour the dark side is tinted toward
    // ---- the SPECULAR ramp: the reference's SECOND ramp band ----
    // The reference samples ONE ramp atlas twice, at two different V bands: `SampleDirectShadowRamp(ramp, x,
    // 0.125)` for the diffuse and `SampleDirectSpecularRamp(ramp, x, 0.375)` for the highlight. Two bands,
    // two mechanisms: the diffuse band is indexed by the shadow-gated half-Lambert, the specular band by the
    // half-vector angle. Kept as a separate pair of numbers here for the same reason.
    float spec_center;   // the half-vector angle at which the highlight appears
    float spec_softness; // its edge width - small is the drawn highlight blob, large is a sheen
    float spec_strength; // 0 disables it, which is what the eye family needs (its highlight is a matcap);
                         // THE FAMILY'S FALLBACK for `_Specular`, not its only home - a material whose own asset
                         // states the value overrides this (see the provenance table in `toon_params_for`)
    // ---- the RIM ----
    //
    // `rim_power` USED TO SIT HERE and is GONE rather than left unused: it was the exponent on `1 - NoV` in the MME
    // rim, and the 终末地 chain shapes its contour with a RANGE WINDOW instead (`_RimLightArea`, in the rig) - a
    // different function rather than the same one tuned - so the field would have been a knob that changes nothing.
    // What stays per family is what the article also carries per material: the colour and the strength.
    float rim_strength;
    float3 rim_colour;
    // ---- THE 终末地 FIELDS, from the article's toon base (MyZmdShaders' `ZmdToonMain.hlsl`) ----
    //
    // WHAT IS HERE IS WHAT THE ARTICLE ITSELF VARIES PER MATERIAL - the three albedo layers and how much of the
    // ramp's colour reaches the result. Everything else the article calls a global (the day/night split, the head
    // light, the backlight and edge weights, the occlusion exponent, the rim's and the environment's strengths) is
    // the LIGHT RIG's and lives in `deren::vulkan::toon_rig`, because it is one set of numbers for a whole character
    // rather than one per material family - see that struct, and the stage's `ToonRigUBO`, for the split's
    // reasoning.
    //
    // THE DEFAULTS ARE THE ARTICLE'S VALUES wherever it states one (0.65 for the darkest layer), and its
    // `_AlbedoDarkStrength` is declared IDENTICALLY as 0.8 by all five of its shaders - so the family default
    // below is 0.8 and NO FAMILY OVERRIDES IT. THE GAME STATES A DIFFERENT NUMBER AND IT IS DELIBERATELY NOT
    // USED: `_ShadowColorBrightness` is 0.55 on skin/face/eye and 0.5 on cloth/hair (`chen_params_digest.tsv`),
    // and that property belongs to the GAME's own shader family rather than to the article's. The user's ruling
    // is the article's default, so the per-family overrides are gone and the game's numbers stay recorded in the
    // provenance table above rather than being applied.
    //
    // MEASURED, `deren-ab/face_ref_b1_*`, against the previous arm - and the honest headline is that the ALGEBRA is
    // 1.6x on cloth/hair and 1.45x on face/skin/eye, while the PIXELS barely move, because the dark layer is only
    // reached where `min_shadow_effect` is small and the ramp already sends most of this character's lit area to
    // the light layer:
    //     user pose (cheek ROI x0=560 y0=130 s=300)   mottle 4.02 -> 3.96, lum std 44.28 -> 44.12,
    //                                                 ROI mean lum 77.41 -> 76.33, edges 9306/6286/3778 -> 9233/6155/3800
    //     user pose frame                             418335 px changed, mean|d| 7.39, max|d| 121
    //     standard pose (whole character)             mottle 11.87 -> 11.86, lum std 25.97 -> 25.96,
    //                                                 edges 14045/11155/7300 -> 14045/11155/7298, 86224 px changed
    // and the visible region in `face_ref_b1_std_full.png`'s difference map is the FACE and the HAIR's shadow
    // side, exactly where the dark layer reaches the picture. No criterion regressed, so it stays; the effect is
    // small enough that it should be judged together with the skin/eye items rather than on its own.
    float albedo_dark_strength;   // `_AlbedoDarkStrength`: how much darker the shadow side's albedo is
    float albedo_dark_saturation; // `_AlbedoDarkSaturation`: 1 keeps the dark side's hue, 0 greys it
    float dark_in_dark;           // the article's 0.65: the third layer, inside the dark one
    float ramp_colour_strength;   // how much of the ramp's COLOUR mapping reaches the result (1 = all of it)
    float sss_strength;           // `_IsNeedSss`: the family's gate on the subsurface term (0 = none, 1 = all)
    // ---- WHAT A MATERIAL WITH NO METALLIC/GLOSS MAP IS SHADED BY ----
    //
    // `roughness` AND `reflectivity` ARE NOT OVERRIDES of the map: they are what the family says a surface IS when
    // the model ships no map for it, which is the SKIN's normal case - the game puts skin's numbers in the shader
    // rather than in a texture (the article's `_Roughness` / `_ReflectivityStrength`), so a family default is where
    // they belong here. A material that HAS a map never reads them.
    float roughness;
    float reflectivity;
    // `ibl_specular_strength` is the environment's counterpart to `sss_strength`: the article's skin and face drop
    // the environment's SPECULAR entirely (a face's shine is the direct highlight, and its matcap in phase 2), so a
    // family carrying 0 here reflects nothing while still receiving the sky's DIFFUSE light - which is the
    // difference between a matte face and a varnished one.
    float ibl_specular_strength;
    // `lut_strength` is WHICH FAMILIES TAKE THEIR DARK SIDE FROM THE ARTIST'S PALETTE (`_ShadowLutTex`) rather
    // than from `albedo_dark_strength`. The article documents the palette for SKIN and the FACE, and the port
    // measured what happens when it is taken everywhere: the game's CLOTH palette (`T_actor_common_cloth_lut_01_D`)
    // returns an olive-green for chen's blue cloth, and the cape goes green over its whole mid-tone range, because
    // the cloth ramp's alpha sends most of that range to the dark layer. That may be the artist's palette and it may
    // be the article's reverse-engineered indexing being approximate off the skin; until that is settled the port
    // reads the palette where the article documents it and leaves the other families on their own albedo. THE
    // PALETTE ITSELF IS STILL READ AND ITS LANE IS STILL LIVE - flipping a family's number here is the whole switch.
    float lut_strength;
    // `ambient_strength` IS WHETHER THE FAMILY RECEIVES THIS ENGINE'S SKY IRRADIANCE (`irradiance_sample(n) * albedo
    // * ao` in character_forward.slang). Every family wants it except the EYE, and the article is the reason: its eye
    // shader's whole composite is `mainSpecularResult + mainDiffuseResult * lerp(1, mainTex_var.w, _DiffuseBlendEffect)`
    // - there is no ambient read in it at all - because the game's eye takes its environment from the `_OtherLight*`
    // block, which this port also carries (the rig's `other_colour` / `other_light` lanes). Leaving the engine's
    // irradiance on top of that gave the eye a second environment term the reference does not have. It is a FIELD
    // rather than a hard-coded zero for the eye because the same question applies to any family an article shader
    // states its own environment for, and 1.0 keeps every other family exactly where it was.
    //
    // MEASURED, AND IT IS THE LARGEST OF THE EYE'S RECENT CORRECTIONS: switching it off moves 129 px (max |d| 94) at
    // the standard pose and 27677 px / 2.67% (max |d| 116) on the face close-up, and the affected pixels go
    // (50.6,40.9,45.5) -> (32.2,20.2,15.7) - a darkening whose BLUE falls most (-29.8 against -18.4 R / -20.7 G),
    // i.e. the iris' blue-grey lift was this engine's sky, not the game's eye. That is worth stating plainly rather
    // than filing as a detail: the "iris reads dark" observation this session chased was, in part, a term the
    // reference's eye does not have being removed.
    float ambient_strength;
    // ---- `light_intensity_scale`: THE SUN'S OWN FACTOR, PER FAMILY, WHICH ONLY THE SKIN HAS ----
    //
    // THE ARTICLE'S FIVE MAIN SHADERS ALL CONSTRUCT THE SAME LIGHT and four of them leave its intensity alone:
    //     `ZmdToonMain.hlsl:78` / `ZmdFaceToonMain.hlsl:78` / `ZmdEyeToonMain.hlsl:...` / `ZmdHairToonMain.hlsl:...`
    //         `mainLightIntensity *= 1.6/1.6;`     <- a NO-OP, written out (1.0)
    //     `ZmdSkinToonMain.hlsl:69`
    //         `mainLightIntensity *= 1.3/1.6;`     <- 0.8125, the odd one out
    // and it is the LAST line before the intensity enters the shading (`:174`
    // `mainLightColor_new = lerp(mainLightColorStrength.xxx, mainLightColor, min_shadowEffect) * mainLightIntensity`),
    // so on the skin the whole DIRECT SUN - the diffuse AND the specular, since both read `mainLightColor_final` -
    // is 18.75% dimmer than on every other surface. This port had no skin-specific factor at all, i.e. skin was
    // 23% brighter than the article's.
    //
    // WHY IT IS A FIELD AND NOT AN `if (family == SKIN)` AT THE USE: the article states it per shader, and this
    // table is where a per-shader statement lives. WHY IT SCALES ONLY `sun_colour`: it multiplies the article's
    // `mainLightIntensity`, which reaches the image inside `sun_colour` and NOT inside `head_day0`/`head_day1`
    // (the `_OtherLight*` block, which the article builds from `_OtherLightStrength`, not from the sun). Scaling
    // the port's shared `main_light_intensity` instead would dim the HEAD LIGHT as well, which the article does
    // not do - see `character_forward.slang`'s `sun_colour`.
    float light_intensity_scale;
    // ---- `other_light_no_l`: THE HEAD LIGHT'S NORMAL TERM, WHICH TWO FAMILIES' ARTICLE SHADERS ZERO ----
    //
    // THE ARTICLE'S `_OtherLight*` BLOCK IS THE SAME THREE LINES IN EVERY SHADER, and what differs is only the
    // weight the colour is lerped by (`ZmdEyeToonMain.hlsl:178-182`, `ZmdSkinToonMain.hlsl:160-165`,
    // `ZmdFaceToonMain.hlsl:208-214`):
    //     float otherLightNoL = 0;
    //     otherLightNoL = saturate(otherLightNoL + _OtherLightOffset);
    //     otherLightNoL = otherLightNoL * _OtherLightStrength + _OtherLightStrength_Offset;
    // The FIRST line is the one that matters: the author starts the term at LITERALLY ZERO in all five shaders,
    // and with `_OtherLightOffset` / `_OtherLightStrength` / `_OtherLightStrength_Offset` at their defaults
    // (0 / 1 / 0 - `deren::vulkan::toon_rig`'s `day.y/z/w`) it STAYS zero, so his head light is a FLAT fill whose colour
    // is chosen by the shadow term and which no surface normal can modulate.
    //
    // THIS PORT DRIVES THAT SAME TERM FROM THE WORLD UP AXIS (`head_no_l = saturate(dot(up, shading_normal) +
    // _OtherLightOffset) * _OtherLightStrength + _OtherLightStrength_Offset` in `character_forward.slang`), which
    // is a MECHANISM THE ARTICLE DOES NOT HAVE - a hemisphere fill that brightens every surface facing up. At the
    // defaults it is "on", and the face's DARK SIDE has been living on it. This field is the family's switch back
    // to the article's zero, set for the two families whose article shaders this has been read against line by
    // line - the EYE and the SKIN - and left at 1.0 everywhere else until that family's own shader is read.
    //
    // MEASURED, AND THE HONEST HEADLINE IS THAT IT IS ALMOST A NO-OP AT THIS PORT'S DEFAULTS - which is a fact
    // about the CONFIG rather than about the article. `deren::vulkan::toon_rig`'s `day.y/z/w` are 0/1/0
    // (`_OtherLightOffset` / `_OtherLightStrength` / `_OtherLightStrength_Offset`), so the port's head term is
    // `saturate(dot(up, n) + 0) * 1 + 0` - already the article's value on every surface whose normal does not face
    // up, which on a flat-faced character at these two poses is nearly all of it. The change moves
    //     user pose   473 px, mean|d| 7.43 -> 7.42, max|d| 8
    //     std pose    823 px, mean|d| 0.89 -> 0.90, max|d| 8
    // - a few hundred pixels of <=8/255, with every ROI and whole-frame criterion unchanged (the tables are in
    // `remaining_port_spec.md`'s "其余部位按参考对齐"). IT IS KEPT ANYWAY: it is the article's VALUE in the
    // article's PLACE, and a nonzero `_OtherLightOffset` / `_OtherLightStrength_Offset`, or a fragment whose
    // normal faces up, would make the two implementations differ for real - at which point the port's version
    // would be shading with a mechanism the article does not have.
    float other_light_no_l;
    // ---- A `finish_matcap` FIELD LIVED HERE AND IS DELETED RATHER THAN DEFAULTED ----
    //
    // It let a family decline the game's finished-colour matcap (`EfEyeIrisApplyMatcap05`), and the EYE was set to 0
    // on the argument that the matcap is already its specular, so applying it twice would count one texture twice.
    // THE ARGUMENT WAS WRONG AND THE MEASUREMENT IS WHY: the finished-colour multiply was sampling the ball with the
    // FLAT QUAD's normal, which on an eye reads a single texel (measured: 0 px at the standard pose, max |d| 2 on the
    // face close-up), so switching it off changed nothing and hid the real fault - that term was never reaching the
    // iris. Sampling it through the CORNEA normal (`shading_normal`) is what makes it the eye's brightness, which is
    // the reference's own arrangement, and once it does, switching it off is not a saving but a missing light. So the
    // switch is gone and the normal is fixed instead; see the matcap block at the end of character_forward.slang.
    // ---- THE TWO EYE REGION COLOURS USED TO BE FIELDS HERE, AND THEY ARE NOW A LANE ----
    //
    // They were this family's reading of `M_actor_chen_iris_01`'s `_EyeHighLightColor` / `_EyeScatteringColor`,
    // which is the wrong shape for a value the game states PER MATERIAL: the brow material carries the same row
    // (chen's is (2,2,2)) and would have worn the iris' tint. They now arrive through the material colour table
    // (`deren::vulkan::toon_colour_lane::eye_highlight` / `eye_scattering`, read by the stage from
    // `core::heap_slots::toon_colours`), gated on the material's own `_UseMatcap` - which is the game's own way of
    // saying "this material is an eye". A field left here would be a second source of truth for a value that has
    // exactly one, so it is deleted rather than defaulted.
};

/// @brief the parameters for @p family, falling back to the family-independent ones for `none` and anything
///        this shader does not know - so an unclassified material is shaded exactly as it was before
toon_params toon_params_for(uint family) {
    // THE DEFAULT IS THE FAMILY-INDEPENDENT PATH (family 0 == none), and it is also the fallback for a family
    // this shader has no entry for. Keeping it first and returning it unchanged is what makes "an
    // unclassified material did not move" a property of the code rather than a promise - and it is why
    // EVERY field below has a defined default rather than the table only filling the fields it cares about.
    //
    // `spec_strength = 0` IS PART OF THAT CONTRACT, not a placeholder: the specular band is a term this
    // stage did not have before, so a non-zero default would grow a highlight on every material whose name
    // claimed no family - and the claim "the family table cannot change a frame with no character in it"
    // would become false the moment the table gained a term. The families OPT IN to it; `none` does not.
    // The rim fields, by contrast, default to exactly the constants they replaced (power 3.0, strength 0.9,
    // the same colour), so the rim is unchanged for `none` and only varies where a family overrides it.
    // ---- WHERE THESE NUMBERS COME FROM, AND WHICH OF THEM ARE STILL OURS ----
    //
    // THE TABLE STARTED AS THE REFERENCES' RELATIVE ORDER (see the note above). This pass replaced every number the
    // GAME ALSO STATES with the game's own, read from the per-material parameter table the export tools produce
    // (`_npr_package_20260925/_npr_recon/character/params/material-params.tsv`), restricted to the seven materials
    // of `actor_chen` and kept in `build-release-clang64/deren-ab/chen_params_digest.tsv`:
    //
    //   `spec_strength`          <- `_Specular`                (skin 0.454, cloth/face/hair 1.0, eye 0.0)
    //                               ** NOW A FALLBACK RATHER THAN THE SOURCE **: the value is per MATERIAL in the
    //                               game's own table, and it arrives in the material's `extras` block
    //                               (`deren::gltf::claimed_extras_floats`) as a `toon_colour_lane::specular_strength`
    //                               lane, which the stage prefers whenever the asset states one. This constant
    //                               answers for a material whose asset states nothing - and it was already
    //                               right for six of chen's seven, the exception being `M_actor_chen_brow_01`
    //                               (0.0 against this family's 1.0). See `character_forward.slang`'s read.
    //   `albedo_dark_strength`   <- **THE ARTICLE'S OWN DEFAULT 0.8** - see the note below; the game's
    //                               `_ShadowColorBrightness` (skin/face/eye 0.55, cloth/hair 0.5) is RECORDED
    //                               and NOT used.
    //   `albedo_dark_saturation` <- `_ShadowColorSaturation`   (eye 1.4, others 1.0; cloth_02 is 1.2)
    //   `rim_colour`             <- `_ColorAdjustmentRimColor` (WHITE on all seven, so it is now the
    //                             family-independent default and the per-family overrides are gone)
    //
    // WHAT IS STILL OURS, AND WHY IT HAS TO BE: `rim_strength` has no per-material counterpart to take - the game's
    // `_ColorAdjustmentRimIntensity` (4.0) and `_ColorAdjustmentRimWidth` (0.35) are IDENTICAL on all seven of
    // chen's materials, so the per-family strengths here are the references' ordering and not a measurement - and
    // `roughness` / `reflectivity` have none either, for the reason the digest shows: the game keeps those in the
    // MRO map, which skin does not ship at all.
    //
    // AND WHAT IS PER-MATERIAL BUT UNCLAIMED, recorded here rather than approximated into a family default because
    // this table has no per-material path: `_SkinRimOff` / `_SkinRimOffScale` / `_FaceRimOffScale` (disable scales),
    // `_SDFRimColor` (the face rim's own colour, 0.761/0.553/0.455 on body and face), `_EyeHighLight` (1.0 on the
    // iris, 0.0 on the brow), `_MatcapNormalScale` (0.9), and `_EmotionIndex` / `_EmotionBlend` (both 0 on the
    // face - which is where the config key standing in for the expression index gets its confirmation).
    toon_params p = {0.42, 0.035, float3(0.58, 0.60, 0.72), 0.80f, 0.06f, 0.0f, 0.9f, float3(1.0, 1.0, 1.0), 0.5f, 1.0f, 0.65f, 1.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    if (family == VR_FAMILY_HAIR) {
        // the hardest diffuse edge and the highest threshold: a strand's terminator is a drawn line. The
        // widest, strongest highlight - the reference gives hair THREE specular models and a dedicated LUT
        // because the highlight is what identifies hair, and a broad band is the toon reduction of that.
        p.center = 0.52;
        p.softness = 0.015;
        p.shadow_tint = float3(0.42, 0.42, 0.50);
        p.spec_center = 0.72;
        p.spec_softness = 0.22;
        p.spec_strength = 1.0; // `_Specular` on `M_actor_chen_hair_01`
        p.rim_strength = 1.1;
        p.roughness = 0.35; // a strand's highlight is tight, which is what makes it read as hair
    } else if (family == VR_FAMILY_SKIN) {
        // the softest diffuse edge and a red-shifted dark side: the MME pack's skin ships `EF_SKIN_SSS_COLOR`
        // as a warm colour, and its ramp's dark side is warm for the same reason. Skin's highlight is small
        // and weak - the reference's is a GGX lobe with `EF_SKIN_SPECULAR_STRENGTH 0.65` gated to the lit
        // side, which as a toon band is a tight, low-contrast spot.
        p.center = 0.36;
        p.softness = 0.07;
        p.shadow_tint = float3(0.80, 0.66, 0.63);
        p.spec_center = 0.86;
        p.spec_softness = 0.10;
        p.spec_strength = 0.454; // `_Specular` on `M_actor_chen_body_01` - the lowest of chen's seven
        p.rim_strength = 0.8;
        // NO `albedo_dark_strength` OVERRIDE: the article's own default (0.8) is what every one of its five
        // shaders declares, and the game's `_ShadowColorBrightness` (0.55 here) is a different shader's
        // property. See the field's note at the top of this file.
        // THE ONE FAMILY THE ARTICLE'S `_IsNeedSss` IS ON FOR, and the reason it can be a family default here: the
        // term is gated by the albedo's alpha, which is a per-TEXEL statement about where the thin material is - so
        // a family saying "skin may scatter" cannot turn it on anywhere the map did not put it.
        p.sss_strength = 0.6;
        p.roughness = 0.55;
        // THE ARTICLE'S OWN SKIN FACTOR, AND IT IS THE ONLY FAMILY THAT HAS ONE: `ZmdSkinToonMain.hlsl:69`
        // `mainLightIntensity *= 1.3/1.6;` = 0.8125, where every other shader writes the no-op `1.6/1.6`. It is
        // the last statement before the intensity enters `mainLightColor_new` (`:174`), so the skin's whole direct
        // sun is 18.75% dimmer. Measured on the user's pose: see `remaining_port_spec.md`'s "其余部位按参考对齐".
        p.light_intensity_scale = 1.3f / 1.6f;
        // ... AND ITS HEAD LIGHT HAS NO NORMAL TERM, which is the article's own first line of the block
        // (`ZmdSkinToonMain.hlsl:160` `float otherLightNoL = 0;` and the two lines after it - all multiplying and
        // offsetting zero at the defaults). The port's normal-driven hemisphere fill is a mechanism the article
        // does not have, and skin is one of the two families this has been read against line by line.
        p.other_light_no_l = 0.0f;
        // SKIN REFLECTS NO ENVIRONMENT SPECULAR, which is the article's own line for this family: its shine is the
        // direct highlight and the subsurface term, and an environment reflection on bare skin reads as plastic.
        p.ibl_specular_strength = 0.0;
        // ... AND ITS DARK SIDE IS THE ARTIST'S PALETTE, which is the one place the article documents the LUT.
        p.lut_strength = 1.0;
    } else if (family == VR_FAMILY_FACE) {
        // near skin's: the face is shaded by its SDF in the full reference, and this fallback should not be
        // softer than skin's before that data exists. Almost no specular: the reference's face highlight is a
        // SEPARATE lip-specular mask, not a lobe on the whole surface.
        p.center = 0.40;
        p.softness = 0.06;
        p.shadow_tint = float3(0.82, 0.70, 0.67);
        p.spec_center = 0.88;
        p.spec_softness = 0.08;
        p.spec_strength = 1.0; // `_Specular` on `M_actor_chen_face_01`
        p.rim_strength = 0.7;
        // NO `albedo_dark_strength` OVERRIDE - the article's default (0.8), as above. What the game states for
        // THIS material is `_ShadowColorBrightness` 0.55; `_ShadowColorSaturation` is 1.0, the default.
        p.sss_strength = 0.6; // the face's subsurface does NOT come from here - see the note on sss_strength
        p.roughness = 0.7;
        p.ibl_specular_strength = 0.0;
        p.lut_strength = 1.0; // the face shares the skin palette, which is the article's own arrangement
        // ... AND NO SKY IRRADIANCE, WHICH IS THE ARTICLE'S OWN END OF ITS FACE MAIN: its result is
        //     `float3 resultColor = rimLightResult + mainLightResult;`   (`ZmdFaceToonMain.hlsl:335`)
        // - TWO TERMS, and neither is an ambient or an IBL read. The file DECLARES the SH/lightmap channel
        // (`:24` `DECLARE_LIGHTMAP_OR_SH`, `:42` `OUTPUT_SH`) and never samples it in the fragment - grepping
        // `vertexSH|SampleSH|SAMPLE_GI|ambient` over it hits those two lines and nothing else. So the face's whole
        // environment is the `_OtherLight*` block (`otherLightResult`, `:212-217`), which this port also carries
        // (`head_day0`/`head_day1` in the rig). Leaving this engine's sky irradiance on top of that gave the face a
        // SECOND environment the article does not have - the same fault, in the same place, that the EYE family
        // above was already corrected for, and the face is the only other family whose article shader states its
        // own environment. MEASURED (cheek ROI x0=560 y0=130 s=300 at camera
        // `-30.3668,-14.3239,0.5,0.0,-0.48,0.0`, against the previous arm - full table in
        // `remaining_port_spec.md` "脸部链按参考对齐"):
        //     3x3 mottle        4.78 -> 3.88      (the pre-port baseline was 5.37)
        //     3x3 mottle, ROI   7.12 -> 6.40
        //     lum std          41.22 -> 33.56     (baseline 42.73)
        //     4-neighbour edges >32   4222 -> 4014
        //     ROI mean lum     75.42 -> 66.58     <- the face goes DARKER, which is what removing a light does
        //     frame            88893 px / mean|d| 0.54 -> 115815 px / mean|d| 2.94, max|d| 48
        // So it passes the artifact criteria (calmer, fewer hard edges) and is honest about its cost: the face is
        // dimmer against the neck and the hands, which keep this engine's sky. That cost is the article's own
        // arrangement rather than a tuning choice - its face main has no ambient read to give it. See
        // `deren-ab/face_shader_compare.md` §2.37 for the对照.
        p.ambient_strength = 0.0;
    } else if (family == VR_FAMILY_CLOTH) {
        // a hard diffuse edge and a cool dark side: cloth's shadow reads as a fold, not a gradient. A BROAD,
        // dim highlight - the reference's cloth carries three lobes (direct, broad, anisotropic) and their
        // sum is what a wide low band stands in for.
        p.center = 0.46;
        p.softness = 0.025;
        p.shadow_tint = float3(0.50, 0.52, 0.64);
        p.spec_center = 0.76;
        p.spec_softness = 0.24;
        p.spec_strength = 1.0; // `_Specular`, on both of chen's cloth materials
        p.rim_strength = 1.0;
        p.roughness = 0.6;
        // CLOTH'S DARK SIDE IS PER-MATERIAL IN THE TRUTH AND CANNOT BE HERE: `_ShadowColorBrightness` is 0.5 on
        // cloth_01 and 0.6 on cloth_02, its saturation 1.0 against 1.2. The family takes cloth_01's (the defaults
        // below) and the other is recorded rather than averaged into a number the game never stated.
    } else if (family == VR_FAMILY_EYE) {
        // the softest diffuse of all: an eye is a sphere whose highlight moves, and a hard terminator on it
        // reads as a painted dot rather than as a lit surface. NO specular - the reference's eye highlight is
        // a matcap and a separate overlay material, so a lobe here would be a second, wrong highlight.
        p.center = 0.30;
        p.softness = 0.20;
        p.shadow_tint = float3(0.74, 0.74, 0.78);
        p.spec_strength = 0.0; // `_Specular` is 0.0 on `M_actor_chen_iris_01` - its highlight is the matcap
        // THE EYE HAS NO RIM IN THE ARTICLE, and this is that fact rather than a taste: `ZmdEyeToonMain.hlsl` contains
        // no `_Rim*` read at all (the rim lives in the base and face shaders), while this port's stage adds its rim
        // pass to every family. A rim on an iris is also the one that would be visible for the wrong reason - the
        // contour belongs to the silhouette the eye does not have. Set to 0 rather than deleted so the family table
        // keeps saying what the reference says about it. MEASURED: 55 px (max |d| 5) at the standard pose and 982 px
        // / 0.09% (max |d| 6) on the face close-up, so it was a small term on this character - but it was a term the
        // reference does not have.
        p.rim_strength = 0.0;
        // NO `albedo_dark_strength` OVERRIDE - the article's default (0.8), as above. The eye is the family the
        // change matters for least (its lit-layer weight is ~1.0, so its dark layer barely reaches the picture)
        // and the family whose article shader states the term most plainly: `albedo_dark = albedo *
        // _AlbedoDarkStrength` (`ZmdEyeToonMain.hlsl:103`).
        // THE SATURATION STEP IS OFF FOR THE EYE, AND THE ARTICLE IS WHY: its eye shader's dark albedo is one line -
        // `albedo_dark = albedo * _AlbedoDarkStrength` (`ZmdEyeToonMain.hlsl:103`) - with NO saturation move, and
        // `_AlbedoDarkSaturation` is declared in the eye's input file yet never read by `EyeMain` (it is the BASE and
        // FACE shaders that use it). This port had 1.4 here, from the game's `_ShadowColorSaturation` row on the iris,
        // which is a different shader's property and a mechanism the article's eye does not have; per "port the
        // article, do not invent", the no-op value stands and the game's 1.4 stays recorded as unclaimed for this
        // family. `1.0` is that no-op: `lerp(luminance, albedo, 1.0)` is the albedo itself. MEASURED, and it is
        // nearly free on this character: 12 px (max |d| 1) at the standard pose and 1476 px / 0.14% (max |d| 3) on
        // the face close-up - which follows from the eye's lit-layer weight being ~1.0, so the dark layer the
        // saturation acts on barely reaches the picture.
        p.albedo_dark_saturation = 1.0;
        p.roughness = 0.1; // a cornea is the glossiest surface on the character
        p.ibl_specular_strength = 0.0; // its reflection is the matcap, not the sky (phase 2)
        // ... AND NO SKY IRRADIANCE EITHER, for the same reason the article's eye has none: its environment is the
        // `_OtherLight*` block, which the rig carries. See the field's own note.
        p.ambient_strength = 0.0;
        // ... AND NO NORMAL-DRIVEN HEAD LIGHT EITHER, for the same reason and from the same block: the article's
        // eye starts `otherLightNoL` at ZERO (`ZmdEyeToonMain.hlsl:178-180`) and calls that whole term
        // `otherLightResult`, so the eye's fill is a FLAT colour and not a hemisphere. This is the eye's third
        // "the article states its own environment" correction, after the sky irradiance and the IBL specular.
        p.other_light_no_l = 0.0f;
        // ITS MATCAP IS THE ONE THE FINISHED COLOUR MULTIPLIES, through this family's own cornea normal - see the
        // matcap block at the end of character_forward.slang, and the note where `finish_matcap` used to be.
        // ITS TWO COLOURS ARE NOT HERE - they are the material's own `color` rows now (see the note at the end of
        // the struct): the iris states them, and so does the brow, which is what made a family field wrong.
    }
    return p;
}
