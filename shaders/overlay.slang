// shaders/overlay.slang - the SLANG source of the OVERLAY fragment stage (`overlay.frag.spv`).
//
// WHAT IT IS: the fragment half of the article's two `Trick` shaders, `MyZmdEyeDarkShader` and
// `MyZmdHairShadowShader`. Both are FRAMEBUFFER MULTIPLIES: their pipeline blends `dst = src * dst` and their
// fragment stage outputs the MULTIPLIER, not a colour - so what this file returns is `lerp(1, _Color, w)`,
// which is 1 (a no-op) wherever the mask says nothing and `_Color` where it says everything. The blend state
// that consumes it is `make_color_blend_attachment_multiply`, and the pipeline is built by
// `core::make_overlay_pipeline`; this file's whole job is the article's two expressions for `w`.
//
// THE GEOMETRY STAGE IS `pbr.mesh.spv`, UNCHANGED (see chores.cpp): an overlay mesh is a normal mesh with a
// normal world transform - it is skinned, morphed and placed exactly like the face it darkens, which is what
// keeps a mask registered to the surface as the character animates. So the varyings below are pbr's, and the
// push block is `surface.glsl`'s, declared once for every entry of that pipeline.
//
// THE TWO CHANNELS SHARE THIS STAGE AND DIFFER BY THE RECORD'S FLAGS (bits 6 and 7, see `deren::gltf::overlay_kind`
// and `material_record::flags`), which is a decision rather than an economy: the two masks read DIFFERENT
// inputs - the eye shadow is shaped by its mask texture and the hair shadow is a flat multiply gated by
// `_DayStrength` that never samples its mask - so the branch is the port, and folding them into one expression
// would silently replace one of the article's two mechanisms with the other.
//
// WHAT IS NOT REPRODUCED, stated here because it is the kind of difference that is invisible in a frame:
//  - the article's output ALPHA. Its stages compute one (`lerp(0, _Alpha * mask, _DayStrength)` for the eye
//    shadow, 1 for the hair shadow) and under Unity's advanced `BlendOp Multiply` it would multiply the
//    target's alpha as well. This renderer's multiply leaves the alpha channel alone (see
//    `make_color_blend_attachment_multiply`): the HDR target's alpha carries no coverage and no pass reads it,
//    so computing a value nobody consumes would be a claim rather than a port.
//  - the hair shadow's `Stencil { Ref 1 Comp Equal Pass Keep }`. There is no stencil attachment anywhere in this
//    renderer's dynamic rendering info (`stencilAttachmentFormat` is VK_FORMAT_UNDEFINED), so the test cannot be
//    expressed; the geometry's own coverage stands in for it (see `core::make_overlay_pipeline`).

#include "heap_access.slang"
#include "surface.glsl"

/// THE TOON LIGHT RIG, declared here exactly as `character_forward.slang` declares it - the layout must match
/// `deren::vulkan::toon_rig` (nine `float4`s) and `runtime::set_toon_rig` is the only writer.
///
/// THE OVERLAY STAGE NEEDS THE RIG FOR ONE LANE, and it is worth saying why that is not a layering mistake:
/// `_DayStrength` is a MATERIAL property in the article's two overlay shaders, but this port already models
/// `_DayStrength` as the RIG's two-state lever (day = the sun's direct term is on, night = the head light
/// carries the character), applied to the character chain at every step. The hair shadow is part of that same
/// two-state look - it is the shadow the character's own bangs cast, which is a day-time phenomenon - so reading
/// the rig's lane keeps the overlay and the character it multiplies in ONE state instead of two that a config
/// file could put out of step.
struct ToonRigUBO {
    // x = `_DayStrength`, y = `_OtherLightOffset`, z = `_OtherLightStrength`, w = `_OtherLightStrength_Offset`
    float4 day;
    float4 other_light;
    float4 other_colour;
    float4 main_dark;
    float4 rim;
    float4 sss;
    float4 env;
    float4 misc;
    // x = `[render] toon_shadow_softness` (0 = the shipped 3x3 PCF). UNUSED BY THIS STAGE, and present only
    // because the struct's size is the shader's view of `deren::vulkan::toon_rig`'s layout: the two mirrors and the
    // C++ struct move together or the lanes after them read the wrong floats.
    float4 shadow_softness;
};

/// THE OVERLAY RECORD'S FLAG BITS, named so the branch below reads as the article's two shaders rather than as
/// two constants: `register_material` sets exactly one of them from the loader's classification, and a material
/// that carries neither is not an overlay at all (it can only reach this stage if a host bound the overlay
/// pipeline to something that is not one, and the answer below is then "multiply by 1", i.e. draw nothing).
static const uint kOverlayEyeDark = 64u;    // bit6: `MyZmdEyeDarkShader`
static const uint kOverlayHairShadow = 128u; // bit7: `MyZmdHairShadowShader`

#ifndef DOXYGEN
/// @brief the article's multiplier for one overlay fragment
/// @param v_uv the mesh's own UV, which is what its mask texture is addressed by
/// @param flags the material record's flags: bit6 selects the eye shadow, bit7 the hair shadow
/// @param mat the material record: `base_color_factor.rgb` is `_Color` and `.a` is `_Alpha`
/// @return the multiplier the blend applies, i.e. `lerp(1, _Color, w)` - 1 where the overlay says nothing
/// @note THE MASK IS THE MATERIAL'S OWN baseColorTexture, and that is how the asset states it rather than a
///       convention invented here: `chars\chen_overlay_tex.glb` hangs `T_actor_common_eyeshadow_01_M` and
///       `T_actor_common_hairshadow_01_M` on the two materials' `baseColorTexture` under glTF's own meaning of
///       that slot, and `chars\chen_full2.glb` is the character with those two materials merged in. The
///       article's `_MainTex` and glTF's `baseColorTexture` are the same slot, so `tex_indices.x` IS `_MainTex`.
float3 overlay_multiplier(float2 v_uv, uint flags, Material mat)
{
    const float3 colour = mat.base_color_factor.rgb; // the article's `_Color`, white by default (=> `lerp` to 1)
    const float alpha = mat.base_color_factor.a;     // the article's `_Alpha`
    if ((flags & kOverlayHairShadow) != 0u) {
        // `MyZmdHairShadowShader`: `color = lerp(1, _Color.rgb, _DayStrength)`.
        //
        // THE MASK IS DELIBERATELY NOT SAMPLED HERE, which is the article's own code rather than an omission:
        // the hair shadow's shape comes from its GEOMETRY (a mesh shaped to the bang region), and the shader's
        // only input beyond `_Color` is the state lever. Reading the mask anyway would be the eye shadow's
        // mechanism wearing the hair shadow's name.
        const float day_strength = toon_rig_at(heap_slots_toon_rig).day.x;
        return lerp(float3(1.0, 1.0, 1.0), colour, day_strength);
    }
    if ((flags & kOverlayEyeDark) != 0u) {
        // `MyZmdEyeDarkShader`: `color = lerp(1, _Color.rgb, alphaTex * _Alpha)`, where `alphaTex` is the
        // mask's own channel. The article reads `.x` (and drives `_UseGrayAsAlpha` in the game's material,
        // i.e. "the gray channel is the alpha"), so `.r` is the channel.
        const float mask = heap_sample(mat.tex_indices.x, v_uv).r;
        return lerp(float3(1.0, 1.0, 1.0), colour, saturate(mask * alpha));
    }
    // NOT AN OVERLAY. `1` is the identity of a multiply, so a mis-bound pipeline leaves the frame exactly as it
    // found it - the failure mode is a missing shadow, not a black quad.
    return float3(1.0, 1.0, 1.0);
}

[shader("fragment")]
float4 main(
    [[vk::location(0)]] float3 v_world_pos : TEXCOORD0,
    [[vk::location(1)]] float3 v_normal : TEXCOORD1,
    [[vk::location(2)]] float2 v_uv : TEXCOORD2) : SV_Target
{
    // The two position/normal varyings are declared because the MESH stage emits them and the block has to
    // match its interface one for one; this stage reads only the UV. The world position is unused on purpose:
    // the article's two overlay shaders are screen-space multiplies with no lighting term at all.
    Material mat = material_at(heap_slots_materials, push.material_index);
    return float4(overlay_multiplier(v_uv, mat.flags, mat), 1.0);
}
#endif // DOXYGEN
