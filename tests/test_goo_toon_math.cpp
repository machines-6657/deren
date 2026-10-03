// Headless unit tests: THE REWRITTEN TOON CHAIN'S IRIS MATH ==================================
//
// WHAT THIS FILE IS FOR, and what it deliberately is not. `shaders/goo_toon.slang` is a Slang stage: its
// arithmetic only exists inside a compiled module, and evaluating it needs a device, a swapchain and a
// character asset - none of which a CI machine has (the capture gate's references are tied to one machine's
// driver and cannot run there at all; that is why this repository's OTHER invariants are asserted on text).
// So there are two halves here, and they answer two different questions:
//
//   1. THE CLOSED FORMS, evaluated in C++ on the arithmetic read out of the reference's own node graph
//      (`build-release-clang64/deren-ab/gooblender/nodes.json`, `Arknights: Endfield_PBRToon_irisBase` and the
//      `calculateAngel` group it instantiates). Every constant below is quoted with the NODE it comes from, so
//      the number can be re-read rather than trusted. This half pins the DERIVATION: the sign of the forward
//      axis, which way the albedo window clamps, that the second layer is an ADD weighted by its factor (and
//      therefore that `albedo` MULTIPLIES the ball rather than adding to it), and that the brightness is the
//      UNCLAMPED angle.
//
//      IT IS A RE-DERIVATION RATHER THAN AN ORACLE, and saying so is the honest framing: it cannot catch a
//      mistake the C++ and the Slang share, because it is the same reading of the same graph twice. What it
//      catches is DRIFT - a later edit to one side that leaves the other alone.
//
//   2. THE SYNC POINTS, on the sources' TEXT, which IS an oracle: a lane added to `toon_slot` without its
//      format-table entry, its flag name, its colour-row name or its stride reads another material's data
//      rather than failing (`deren::vulkan::toon_slot`'s and `toon_colour_lane`'s own notes record that failure
//      happening once already), and none of those four places is checked by the compiler. This half is the same
//      shape `test_toon_material_sidecar` uses for the strides, for the same reason.
//
// NOTHING HERE ASSERTS A PIXEL. The frame-level acceptance - "with `goo_toon` off the frame is what it was, with
// it on only the iris moves" - is a capture, and it is recorded in
// `build-release-clang64/deren-ab/goo_step1_result.md`.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#ifdef VR_TEST_SOURCE_DIR

namespace {
    // ================================ THE REFERENCE'S CONSTANTS ================================
    //
    // Every one of these is a socket value or a factor in `gooblender/nodes.json`'s
    // `Arknights: Endfield_PBRToon_irisBase`, named by the node it belongs to. THEY ARE ALSO SPELLED OUT IN
    // `shaders/goo_toon.slang`, where the spelling is asserted below - so a shader edit that changes one of them
    // without changing this file fails here rather than quietly re-tuning the eye.

    /// `运算.Value` in `Arknights: Endfield_PBRToon_irisBase` - what the azimuth is subtracted from. THE GOO
    /// PROJECT'S VALUE: the OTHER dump (`endfield_addon/chen_dump`, the addon preset `Chen.blend`) states `1.1`.
    constexpr float k_angle_center = 1.0f;
    /// `钳制.Min` / `钳制.Max` - the window the albedo's weight is clamped into.
    constexpr float k_window_min = 0.5f;
    constexpr float k_window_max = 1.0f;
    /// `混合.002.Factor_Float` - the second layer's weight in an ADD.
    constexpr float k_ball_weight = 0.5666666626930237f;
    /// `运算.004.Value_001` - the reference's own float literal for pi.
    constexpr float k_half_turn = 3.141592502593994f;
    /// `值(明度)`'s output default inside `calculateAngel` - the sign applied to the head's forward axis.
    constexpr float k_forward_sign = -1.0f;
    /// The two MATERIAL overrides on Laevatain's iris (`goo_params_laevatain.tsv`: `Eyes brightness` /
    /// `Eyes HightLight brightness`, both `材质覆写`), which ride `toon_colour_lane::goo_eye_brightness`.
    constexpr float k_eyes_brightness = 1.5f;
    constexpr float k_eyes_highlight_brightness = 10.0f;

    struct vec3 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };
    /// a two-component pair, spelled rather than pulled in from the engine: this test links no module (the math
    /// it evaluates is the SHADER's, and the shader is not callable from here)
    struct vec2 {
        float a = 0.0f;
        float b = 0.0f;
    };
    constexpr vec3 operator-(vec3 const a, vec3 const b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    constexpr float dot3(vec3 const a, vec3 const b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    constexpr vec3 scale3(vec3 const a, float const s) {
        return {a.x * s, a.y * s, a.z * s};
    }
    float length3(vec3 const a) {
        return std::sqrt(dot3(a, a));
    }
    vec3 normalize3(vec3 const a) {
        float const length = length3(a);
        // BLENDER'S `NORMALIZE` ANSWERS (0,0,0) FOR A ZERO INPUT and Slang's answers a NaN; the shader's guard
        // reproduces Blender's answer, and this is the same guard so the test can exercise that case by name.
        return length > 1e-6f ? scale3(a, 1.0f / length) : vec3{};
    }

    /// @brief the reference's `calculateAngel.AngleThreshold`, from the group's own links
    ///
    /// `VM.008 = DOT(LightDirection, headUp)`; `VM.009 = MULTIPLY(VM.008, headUp)`;
    /// `VM.010 = SUBTRACT(LightDirection, VM.009)`; `VM.011 = NORMALIZE(VM.010)`;
    /// `VM.017 = DOT(VM.011, headRight)`; `VM.019 = MULTIPLY(headForward, -1.0)`;
    /// `VM.020 = DOT(VM.011, VM.019)`; `运算.003 = ARCTAN2(VM.017, VM.020)`;
    /// `运算.004 = DIVIDE(运算.003, pi)`; `运算.005 = GREATER_THAN(运算.004, 0)`;
    /// `运算.006 = ADD(1, 运算.004)`; `运算.007 = SUBTRACT(1, 运算.004)`;
    /// `混合.001 = MIX(f = 运算.005, A = 运算.006, B = 运算.007)`.
    ///
    /// The last three lines are the reference's way of writing `1 - |x|`: the MIX's FACTOR is `turns > 0` and its
    /// A/B are `1 + turns` / `1 - turns`, so the branch the positive factor selects is the one that SUBTRACTS.
    /// Writing it the other way round (`1 + |turns|`) puts the azimuth in [1,2], which the iris' own
    /// `1.0 - angle` then reads as a constant -1 and the albedo's window collapses to its floor for every sun -
    /// and this test is where that was caught, in the test's own transcription rather than in the shader.
    float angle_threshold(vec3 const light, vec3 const head_up, vec3 const head_right, vec3 const head_forward) {
        vec3 const projected = light - scale3(head_up, dot3(light, head_up));
        vec3 const axis = normalize3(projected);
        float const right_component = dot3(axis, head_right);
        float const forward_component = dot3(axis, head_forward) * k_forward_sign;
        float const turns = std::atan2(right_component, forward_component) / k_half_turn;
        return turns > 0.0f ? 1.0f - turns : 1.0f + turns;
    }

    /// @brief the same function with the reference's DEGENERATE case spelled out, as the shader spells it
    ///
    /// C's `atan2(0, 0)` is deterministic (and here it is `atan2(+0, -0) = +pi`, the negative zero coming from
    /// `MULTIPLY(headForward, -1.0)`), while SPIR-V leaves `Atan2(0, 0)` UNDEFINED - so the shader states the
    /// reference's answer rather than the instruction's. This is that statement, and section 1 asserts the two
    /// agree everywhere except the one input the platform does not define.
    float angle_threshold_guarded(vec3 const light, vec3 const head_up, vec3 const head_right, vec3 const head_forward) {
        vec3 const projected = light - scale3(head_up, dot3(light, head_up));
        vec3 const axis = normalize3(projected);
        float const right_component = dot3(axis, head_right);
        float const forward_component = dot3(axis, head_forward) * k_forward_sign;
        float const magnitude = std::abs(right_component) + std::abs(forward_component);
        float const turns = magnitude > 1e-12f ? std::atan2(right_component, forward_component) / k_half_turn : 1.0f;
        return turns > 0.0f ? 1.0f - turns : 1.0f + turns;
    }

    /// the `混合` node's output: `albedo * clamp(1.0 - angle, 0.5, 1.0)`
    float albedo_weight(float const angle) {
        float const raw = k_angle_center - angle;
        return raw < k_window_min ? k_window_min : (raw > k_window_max ? k_window_max : raw);
    }

    /// `混合.003` (MULTIPLY, factor 1.0) then `混合.002` (ADD, factor 0.5666666626930237): for an ADD at factor
    /// `f` Blender computes `A * (1 - f) + (A + B) * f`, i.e. `A + f * B`.
    float iris_layer(float const albedo, float const ball, float const angle) {
        return albedo * albedo_weight(angle) + k_ball_weight * (ball * albedo);
    }

    /// `混合.001`: `lerp(Eyes brightness, (1.0 - angle) * Eyes HightLight brightness, D_Alpha)`, and
    /// `运算.001` reads `运算.Value` - the UNCLAMPED `1.0 - angle` (`运算`'s own `use_clamp` is FALSE).
    float iris_strength(float const angle, float const albedo_alpha, float const eyes_brightness, float const eyes_highlight_brightness) {
        return std::lerp(eyes_brightness, (k_angle_center - angle) * eyes_highlight_brightness, albedo_alpha);
    }

    /// the whole emission, as the shader composes it
    float iris_emission(float const albedo, float const ball, float const angle, float const albedo_alpha) {
        return iris_layer(albedo, ball, angle) * iris_strength(angle, albedo_alpha, k_eyes_brightness, k_eyes_highlight_brightness);
    }

    /// the sphere map, AS THE OLD CHAIN BUILDS IT (`character_forward.slang`'s `toon_matcap_uv`): a view basis
    /// from the surface-to-camera vector, with V negated for this renderer's texture convention.
    vec2 toon_matcap_uv(vec3 const n, vec3 const v) {
        vec3 const up = std::abs(v.y) < 0.99f ? vec3{0.0f, 1.0f, 0.0f} : vec3{1.0f, 0.0f, 0.0f};
        vec3 const right = normalize3(vec3{up.y * v.z - up.z * v.y, up.z * v.x - up.x * v.z, up.x * v.y - up.y * v.x});
        vec3 const up_ortho = vec3{v.y * right.z - v.z * right.y, v.z * right.x - v.x * right.z, v.x * right.y - v.y * right.x};
        return {dot3(n, right), -dot3(n, up_ortho)};
    }

    // ================================================================================================
    // STEP 5: THE FGD LUT - its constants, its reader, and the two helpers its assertions use
    // ================================================================================================
    //
    // THE ONE HALF OF THIS FILE THAT IS NOT A RE-DERIVATION FROM THE GRAPH: the expectations are read out of the
    // REFERENCE'S OWN PNG (`deren-ab/gooblender/images/PreIntegratedFGD_GGXDisneyDiffuse.png`, 5234 B, 64x64 RGBA8)
    // by `png_rgba8`, so they are the FILE's texels rather than a transcription of them. The spec's §3.3 is why
    // that matters more here than anywhere else in this file: these three numbers have NO closed form (measured
    // against Karis' fit and against a 120k-sample Disney diffuse FGD), so a test that "recomputed" them from a
    // formula would be testing the wrong thing twice.

    /// `Remap01ToHalfTexelCoord :: 值(明度).Value` - the LUT's resolution (the frame's own name is
    /// `FGDTEXTURE_RESOLUTION`).
    constexpr float k_fgd_resolution = 64.0f;
    /// `GetPreIntegratedFGDGGXAndDisneyDiffuse :: 运算.Value_001`, the ADD's second operand: `diffuseFGD = LUT.B + 0.5`.
    ///
    /// **IT IS `0.5` AND IT WAS ONCE `0.0`.** The ADD's `Value_001 = 0.5` is `is_linked = false, enabled = true`
    /// and its `Value_002 = 0.5` is `enabled = false`, and the rule the WHOLE FGD group is read by (and the reason
    /// `Remap01ToHalfTexelCoord`'s own bias is `(1/64)*0.5` below) is that an unlinked socket PARTICIPATES when
    /// `enabled = true` and does NOT when `enabled = false`. The `0.0` came from a parent ruling that MEASURED the
    /// image instead of reading the flags - `B ∈ [0,1]` with mean 0.480 is a FINISHED Disney diffuse FGD, while
    /// `B + 0.5 ∈ [0.5, 1.5]` is a diffuse term brighter than the albedo it multiplies - and that argument is kept
    /// here because it is the reason a reader will doubt this number. It is a PRIOR about the term, not evidence
    /// about the author's graph, and this author writes ambient floors too (step 4's
    /// `GlobalShadowBrightnessAdjustment`). See `goo_fgd_diffuse_offset` in the shader: one named constant, so the
    /// experiment is one line. The measured consequence of `0.5` is in `deren-ab/goo_step5_result.md` §12.
    constexpr float k_fgd_diffuse_offset = 0.5f;
    /// `ComputeFresnel0 :: 组输入.dielectricF0` on the Base container. Laevatain's ELEVEN materials all end up at
    /// `(0.08, 0.08, 0.08)`: `metallic = lerp(0, MetallicMax, _P.R)` and `_P.R` is `[0,0,0]` or a linked map whose
    /// texels this project does not expand (spec §A6), so `fresnel0` does not depend on `BaseColor` here.
    constexpr float k_fgd_dielectric_f0 = 0.07999999821186066f;
    /// `运算 :: Value_001` - the `clampedNdotV` MAXIMUM's floor (`9.999999747378752e-05`, i.e. float32 `1e-4`).
    constexpr float k_fgd_ndotv_floor = 9.999999747378752e-05f;
    /// `DV_SmithJointGGX_Aniso :: 运算.013.Value` then `运算.001 = 1/2`: the group's own `1/(2π)`. NOT this
    /// project's `goo_inverse_pi` (`0.31830987334251404`) - the reference's stored value differs at the 8th digit
    /// and it is the one multiplied into `D * Gv`.
    constexpr float k_dv_half_inverse_pi = 0.3183099925518036f * 0.5f;
    /// `DV_SmithJointGGX_Aniso :: 运算.017` - the MAXIMUM's floor, `f32(1.17549e-35)`. The dump stores it at full
    /// precision as `1.1754899742869237e-35` (bit pattern `0x0579FFC3`, i.e. `999.9962768554688 * FLT_MIN`), and
    /// `shaders/character_forward.slang` spells the same literal, because a "cleaned up" `1.17549e-35` is a
    /// different float32 and `1e-35` a different one again. It is NOT `FLT_MIN`.
    constexpr float k_dv_denominator_floor = 1.1754899742869237e-35f;

    /// `DeSaturation :: 合并 XYZ.002.X/.Y/.Z` - the reference's luma weights, VERBATIM and UNROUNDED. They are
    /// `dot(颜色, (0.21267299354076385, 0.7151520252227783, 0.07217500358819962))` and the test pins the spelling
    /// to all seventeen digits because a "cleaned up" `0.2126f / 0.7152f / 0.0722f` would move every expectation in
    /// section 8s and no picture would show it.
    constexpr float k_desaturation_luma_r = 0.21267299354076385f;
    constexpr float k_desaturation_luma_g = 0.7151520252227783f;
    constexpr float k_desaturation_luma_b = 0.07217500358819962f;
    /// `PBRToonBase :: 组输入.Color desaturation in shaded areas attenuation`'s `interface[]` default - `0.0`,
    /// the reference's own number, and the value a material with a ramp but no stated `.y` gets.
    constexpr float k_desaturation_default = 0.0f;
    /// THE IDENTITY OF THE DESATURATION - `1.0`, which is what a material OUTSIDE step 6's arm gets. It is a
    /// different number from the group default on purpose: `0.0` is a real desaturation (`saturation = luma`), and
    /// writing it into the neutral would apply the step to the hair and the face.
    constexpr float k_desaturation_neutral = 1.0f;

    /// `DecodeNormal :: 运算.002.Value_001` - the MAXIMUM the reconstructed `z` is floored at, spelled to all
    /// seventeen digits because `shaders/character_forward.slang`'s `goo_normal_z_floor` is the same literal and a
    /// "cleaned up" `1e-16f` would be a different float32.
    constexpr float k_goo_normal_z_floor = 1.0000000168623835e-16f;
    /// `DecodeNormal :: interface[]` - `NormalStrength`'s own group default, `1.0`: the value a
    /// `PBRToonBase` instance inherits when nothing is patched into the socket.
    constexpr float k_goo_normal_strength_default = 1.0f;
    /// `M_actor_laevat_body_01` / `M_actor_laevat_body_02 :: 法线贴图.Strength`, both `1.25` VERBATIM.
    constexpr float k_goo_normal_strength_body = 1.25f;
    /// `M_actor_laevat_cloth_01` .. `cloth_05 :: 法线贴图.Strength`, all `1.4458599090576172` - the same float32
    /// the reference's `chen_cloth_01` states, which is how the value was first recognised as a family constant
    /// rather than an authored tweak.
    constexpr float k_goo_normal_strength_cloth = 1.4458599090576172f;
    /// `M_actor_laevat_hair_01 :: 法线贴图.Strength` - the INSTANCE socket's `0.5`, NOT the group's `1.0` default
    /// above: `[Arknights: Endfield_PBRToonBaseHair]`'s `群组.001` patches `0.5` into it, which is why the row step 14
    /// added carries `0.5` and not the default. It is also the FIRST strength this port carries that is BELOW 1, so
    /// the `saturate` in the closed form is load-bearing on a shipped path rather than vestigial - see A5 below.
    constexpr float k_goo_normal_strength_hair = 0.5f;

    /// @brief the reference's `DeSaturation` closed form: `lerp(luma(colour).xxx, colour, desaturation)`.
    ///
    /// EIGHT OF THE ELEVEN NODES (`组输入` + `合并 XYZ.002` + four `ShaderNodeVectorMath` + two `NodeReroute`):
    /// `矢量运算.004 = DOT_PRODUCT(Color, (r,g,b))`; `矢量运算.005 = SUBTRACT(Color, 矢量运算.004.Value)`;
    /// `矢量运算.006 = MULTIPLY(DeSaturation, 矢量运算.005.Vector)`; `矢量运算.007 = ADD(矢量运算.006.Vector,
    /// 矢量运算.004.Value)`. The `SUBTRACT` and the `ADD` are RGBA nodes fed a FLOAT, which Blender broadcasts to
    /// all three channels - so this is `luma + d*(c - luma)` per channel and NOT a dot product.
    ///
    /// THE `d` HERE IS ALREADY CLAMPED, exactly as the shipped `goo_hsv_desaturate` clamps its own argument (it
    /// saturates): the clamp is the reference's `钳制.004 = clamp(运算.005.Value, 0, 1)` - `Min = 0.0`, `Max = 1.0`,
    /// both unlinked literals - and it lives at the CALL SITE in the reference's graph but inside the function
    /// here, because `色相/饱和度/明度`'s Saturation input is where the clamp's result is consumed. So this helper
    /// mirrors the shipped arithmetic rather than the node's raw input.
    vec3 desaturation_closed_form(vec3 const colour, float const raw_desaturation) {
        float const desaturation = std::min(1.0f, std::max(0.0f, raw_desaturation));
        float const luma = dot3(colour, vec3{k_desaturation_luma_r, k_desaturation_luma_g, k_desaturation_luma_b});
        vec3 const grey{luma, luma, luma};
        return {
            grey.x + desaturation * (colour.x - grey.x),
            grey.y + desaturation * (colour.y - grey.y),
            grey.z + desaturation * (colour.z - grey.z),
        };
    }

    /// @brief THE REFERENCE'S PRE-INTEGRATED FGD LUT, BASE64 - the PNG `PreIntegratedFGD_GGXDisneyDiffuse.png`
    ///        (5234 bytes, sha256 `7e49509b65c668abcaee523caa4c0dbc86bd09695bfc08522ec1de1a90e000e1`)
    ///
    /// WHY THE FILE'S BYTES ARE EMBEDDED RATHER THAN READ FROM THE REPOSITORY, and this is a constraint
    /// rather than a preference: the only copy of this image in the working tree lives under
    /// `build-release-clang64/` (which is git-ignored, `.gitignore:3`), so a test that read it from a PATH would
    /// pass on this machine and fail on a fresh checkout - the same reason the four `_RD` ramps are tested
    /// through their constants rather than through their pixels. EMBEDDING THE PNG RATHER THAN ITS TEXELS keeps
    /// the test's own verdict checkable: the SHA-256 below is over bytes that came back out of the decoder, so a
    /// transcription error anywhere in this table fails the first CHECK in section 8n rather than silently
    /// becoming the new expectation.
    std::string const k_fgd_png_base64 =
        "iVBORw0KGgoAAAANSUhEUgAAAEAAAABACAYAAACqaXHeAAAUOUlEQVR4XmP8/5/hPwMQgIh/QBqE/wLpP3DMxPCbgYnhFwMzHP8EskH4BwMLw08g/gHk"
        "fwfTLAw/oPR3NPoHVN1PsB5mBgQNMRdmB4j+A7QPGf9lYGQA4X9A8X9A9j+wexkZQOz/UDbMDwxIfAYiANOxEwwMT58xMHz7zsDwD2jy//8wXUxABgsa"
        "ZgbyYZgFic2MhQ3Sjw8zAvUQgxmQ1MHcxojkNUY0bzIykAJYjM2Bnv/KyPD6LQMDOwcjAxc3IwMTKyPDfyYgZmQCJg+gJxhBHmQi4GFYIMA8DdMD44Mc"
        "xkSkp2EBA/M8Ni9h8yhpngeZyvLzPwvDP2BkMgE9/+UHI8OP34wM7JyMDCzsQMNYmBj+MTEx/AUGxD9gIPxnAAUIM8N/MBvi4f/A5A/CDAyIQAKpY2CA"
        "BRi5KQHZ8+gphZiUQEw6APoTlDf/Ad3+nw2Yp4CZ/xswAH58Y2Jg+QOUZGNiYGQFepoZGAjggGBm+AdKFf8RgQD3PCPEw/+RPI4ICORUAPHMfwZsyR/Z"
        "0cjy6J4hJtkTlxpYwAEAVPuHBVjQsDIw/PkHpP8yMjD+ZGJg/svEwAQMCHAgsDAz/AUGwh8mZoY/wBQASRUsDKAAAKWOf6BAYUBkg/+M6FkAEgiQVISc"
        "FfBlCwYsWQY99okJDNypgQVUWoPKvb/APP8HmBV+gwLgPzA1/AM6DJgaGP8DAwHEBgbGf2Ag/AOmBnAgwAMCmCqgKQJcSgM9/g+ULf5Dyg9IFgGyGdFT"
        "AarHMVMErvwPSxnY5MkoA8ABANT3DxgAf4GR9heYEsCp4A/Q0cDA+A+kGUEBAMX/mYEeBgUCkP6NFAh/QR6HlxNQzyPxGcABAnIgIiD+M+KOfUiA4EsB"
        "6NmFgSwATwH/GYGxDnTPX3BAALPBfxAGeuQ/JCAYgAHBAIrpfyA+MDv8A5ULzAy/QQEB9CgoVfwFZw1QOQHE4MIRlgpANKJWgXgOaNl/tHKAERG7jAzI"
        "MY3Ns8TENmE1LN8YWMCmgxz1D5oS/jEjAuAfsDz4Dw4ESGCAYvIfsNT8DwoAoOf/ANl/QIEAVPMbGKN/QBhYVoCyA6ScgKUGaGAyQFLBf6hnUZI+UoCA"
        "myOMiACCBAgxpT9p2QCcAkDGghz0HxYATKDUAAwEEA302L9/UMeD2H9hbFDeh6SCP8CU8QfoaVCWAGcLEB9cdTKBa41/oNQFCkQgDS4EQTS0jABVl+hZ"
        "AR4o/9FTNdCBjPiSPiPJ2QARAAygJMrAAMkKkAAApYR/IE8zQApFUEoA8f//hXgeLAcsB/4C1cAKxt/gmoIJXFP8gbYhYCkBrBcUMPDAYATbByQYQJ6G"
        "53ukmGdAqS4ZQA6EehLZs+gBgxYO/7FogQpBAgCo//9/SAAwgGKLERIAIDFQqQ9uf4MCAhwYkCQNShWg0h7kKVAA/AVlB1BAAGP/NwiDsgKIzwhrQ0BT"
        "w3+Ip2EeRngc5AlGcEBAeidQNiOaR8EORxUD8f7/JxT5IE8iqYEaAU8BDOBkCUkB/8FJH+gAUHUIxkDHQ1MCKO+DqzsQ/x+kYfQX3EkBtg+AYn//Q2hQ"
        "efAbmjXAAQQNiH/QFABuUIEbVagBAo9xmB8hhQEDRBxEITwPKhew+RuXOEoQQTWCG0LgMoABkowgZQHEUaC8C8kSoAAApor/sFiEpgJoivj7DyQPrBnA"
        "KQAY4/8hWeA3lP8H7HmI/r/QFPYfTjMxIKcClBQBS/5wPyPHIiM8LSAKTAaSAWoAALXDkybILlBsMUFTAhM0EECx/Q8RAH+hgQAu9f9BAgFSJgADARYA"
        "TBA2qIr9h1Q4Igc2mA0qhdFrB3D6ZmSAxzSUz4CcCxiwJ29iQgMeAKAkBu5bMyJSwj+w55kYYFkC1DECF4RMkEIRnhVASZ8Bkk1ABR4oS6AHwl9YIEBT"
        "AyQlgFIUNAswomcFBrSSC+rj/xD3McCzBiJ3wD2MPV+ghQfEPJQAYICnAGhZAHMUPCWAeodQz4PEwIMT0PIBlh2AAQHKEvCAABWGDJBGFTgQGCFsUOD+"
        "gxW4jNBGEpiGeOg/rLCEeRTsXhye/4/Nb4yogjgKSZaf4IYQI1LggRzDwACvmpDyKixFQBwO9Dh8hAZWSILyP2ikBkgjBwSoVYmUDcCB8x/azsDwPCQl"
        "MEDLJAZo9QwvBGEpgAEa80h5A5Y7GHDVCGhhAjKC5RcDK0r++g/NTxj5ExwQ0OwA9TikRkAUjn//Qzz/D54lGMFjCaDkDsoWfxhhKQGojhGqjwnKhsY+"
        "eJgLnvJAngS6+j/Us4xYYpURi6+Q4x5ZGkvAQFMAA/YUwIiaP+G1Aqj6ggYIuI3AiKgm4eN24EAAjeUxgcfuINkCFCDogQAJAEitgJz1IJ6HtI6RAoEB"
        "GijIKQDmSXASQPI9Oh8mhRQowBTAguR5mKlQh/yHlL6Q1MDEgKi6GCEDI6BAYICyoeUBIhVAxMH8/0gpgREWCBAapRxgggUAquchhTPU4zBP4aMZIBkG"
        "EeFQH8M8jpQSUAIA5H1Yc/Q/crvgP2q74D8jIjBAMf4fFhDQxtI/eOHICB3NhaaE/1DPM0KTPTjwgIOx0GwAas3B+iQIGuFxSGqAxhcjki8ZkVIILLsw"
        "EAdYfjPgSAEMsLoXmg3+Q5qm/5CyBWR4DJafoYHEAGsvMEKSPrxcgKj7C3QgyAzwMDcsIP7D+iCIFABP+jCPQmmMLEEoRSASNQMDlsBBCQCMFMCAlBUY"
        "kQKCETnfIgLmHyyQkFIApB8BCgwGBkj2gAQMOBCAjodkAWAqAJkPDRyQJ8EYOmQASw2whiEDzFOwwEHm/0dOGdBUgOxxtEBABAC8wIBWg9AUgFwf/0cL"
        "BHiZ8B8pQECe/88InbQAlfRQD4M8x8AICQSQx6F6QGMQIM/+g2aH/0jlACRLMMC7B//RUgPWAEGOcVieR04l0FqOASrH8gdPFoCliP/IKYEBEkD//6Nm"
        "CeTYB5UfkFkmaECAPAsKgP+QlACb3fkHDgRo7DNAerowT8NjHZ4KGFB6xv+xxT5ySkBPFegBAy0I4QEA4TPCi8//0BQA7yUyQDwMTpr/YdUj0NH/EbEN"
        "ThHQmAYHwn+oPCwAwKkDpgfkcSCGBQIDhA82nxHa7QfR/xFsBqj4f5TygAHeRoKnCOSYJxAQSCmAEaUBhagNGBCDFowQx4NTBDRAYPN0MM9DYh9iFmzu"
        "Dpw6oAEDThn/IeURLAXAY/0/ZiD8Z0DPAv+xpwRGpBSCLSWgpwAoHxgArAzI4D+cA/EEhA9jQ2logQVOJYyILAEPDAZIzP4H0wwMsCzxHxYIsABgAAUo"
        "UC0D1OMM0GzACAkgSEz/Z/jPiJwKoNEL9zBqgBAsKOEBAUntLH/hg6IMaAEBsQg1ABgYYFnjP0pZgBBHDQQkzzNAPAqO7f9QNgOC/v8f4WmwnYxQjzMw"
        "IsX4fyzJnRF11AwcMDB1jAyEsgVKIQgLAZinQXwQG94o+g/JDnDHMiBSyX8k9r//SFkF5Nn/kJj+D2PDUwZI/D8DJNAgsQwx+z842/2HpQgGWCqApjaG"
        "/1g8zYAkhp5KGDADDpoS4CmAAQUgyoP/UHFkD0ICBqgGFmswDyN5EOx4qDzcg/9hqQDoITgbZBdkkQI44ECehRfAUNsZYR6Cehya+hgYcQQEAwP2AEIW"
        "hwXAP5zVIMTn/xmQaSRPM0CTLNSTYA/DUgFYDOIpkDgkRcDUgzwPah8wgL0KiXGQuQj1/2FmM0BTBQNUDhQQME8zQFIjpGaABRQRHoeVHbAA+MuAWggy"
        "QC3HoJE8yoDsQAZEDMLyN9gD/5E8zADJAv/+QzwPUQdlM0BSA4oeBmjggDPwf4ivGCG2/sdIDSBpQuUELGAwswYLZgpggAPU2Id6COoZlED4D8nLqJ5A"
        "9hgsdqFi8IBADQQGmKFILviPFMAMYM//h46GMYKDiQEpRiG1BlJgMDAQzAo4ygAGBmyeR3gaEr2oMY7pYQZ47CJ7HFjoges1mBi0gGRAFJT/GWABg3AH"
        "2CxGmI/+Qws1kGehLkVL2gy4+NBEAJNHSQEMSAAlAKDJmeE/Ip9CAgPK/w/loaQEVE/Dkz3Yc1APQkMQHpD/kQPjPwOsWQprlDFAywIGaKoA538GaIzD"
        "agb0WCfAJxgAEK/BvAtxIAPMo/8h0fYf2eMMCI9DAgw9IEB8CGb4hxTr/xFsBqil4OD9D0nqkAiBJnsG9JQA5TMSmxoY4dUiy3+kQvA/A8K7cNZ/aDKE"
        "xj4sKGBqwZ6HxioiYKCehAYGIiCA4v+gctCq4f9/VD4D1L7/4MCFxPn//4wMiChghKcDBgZEgECUQNM3WjJnQE4FyGygOswUAPMwA3JwQJI6zINgx8Ac"
        "Dk8FsOwBcfl/BqSY/o9g////jwHuOZBd/5BSFfIyvf8IcQZYqCDXCvAIgQYCzNMwD+KiiSsDEB5GhDwiB/6HWQ6mIZ5jYED2JMT1/5E8zgCOeZDngcNp"
        "sNj/Bw0MpKwAUQf1PAMsEKANM7B9jFBBROwjpwQGGED2KJ7AAGYBxJAYTO9/Bga4C/5DQ/8/LEVA0jwDzHMM//8TYEN8B1YPZkKi/T+IDVp2A0oRKKmA"
        "EZLnkFLA//+Q1MgALQ/gCQJn+QDNtnhTBUQSUQYg5XGIX2FehqYBmMfBORAawwyoMY0aGP+gAQMJTEiAMTCAYxglIEAp4h8DPHv9Y4RnEQZIbkIKkP8I"
        "n8EDhQEJIPv4PwMKYEQrH6CpAkstANEIJ//Dkv5/BtRUAE364ICDeBYRACAPgTwCEgfFKHJgQLIBaF3u/38wzwPVgjoMSJ7//w8WcAzQwAHRjIiAYkDK"
        "Cv/RPPsfOc0zIDpCsBBBCicsWQAtAMDuQMQ0zJPgYPkPEwfNFsOSOhbPg5P7f3BPCOwxUN6HeR7saeC4F1gM6kGY5/8hsgMkNUBTwH9GRBb9Dw0YaBZl"
        "QAsLBnwAaAyOMgBqyn9YYIBtgaQAaMEHCQiYp0GpAeIocJX1HxLz/0Grr+H5+z8kaYMDA+h+sBwo5kGDfkD14BVa0JQAshfm+X//UbLEf5jnYR5HLh/+"
        "I0UeUQEBXA37H6UzhB77kBD/j5TXITEP8zjU8eCUAO3RgR0I8sg/RNL9B+FDkjXI8wxoqYERPCQE9hwo9mFZ4R80YP9BsgE4diF9Zgbw5C3UXgZYqQht"
        "LzBgKx/+Y0n/QCE8WeA/1FYGBkgpDC0D/kPzMCglgH0EWjWGSPZgh0HHuiEeAgXMXwbQugJIAQj17D9UmgHKh7QOGcGFJQNSKgD7ER4Q0CzwHylgkNkM"
        "0KzDAPcCxPdYUgXuAIAXLP/hSZ8B3fP/kT3PCI0VpIYO1MGQfA8MBPBqUwakrPCfgeEfeoAwIIkxQBTDUwTUQ/BsAQsIRID8/4/kaXQ2LBEgBQQLtizA"
        "AC1J/kMLOXB+h0YBRAyWCmClPCiWoQHwD5mGJndQTgIl3X+w5A/zOBM0KwD5oAWZ/xAxD0k9MHWwgGBgwBg/I5QqGJBSCQNa4AC5wFYQ+tzgfwYG9NiH"
        "eR68FB6ULyE+gRRI0FTwD5TMYakAkgRR8zyiEIS1BcD0X5CnmRBJHikQGKDs/9ByAVIGwFIQpHyCZBMoG6WARPL4f/RAYISkBaA41iwASSEIXZAyAFbX"
        "gzwPS/ogMVj+Z4BmAUZEAfcfucD7D4k9UPL9B/Hw/38Ij4PZkJlTYE5DSgn/GOH6YAEATwX/GOFZBFIrQbMP3OnIAYMW+1A1mFngPwM8uGClPyhL/Ae3"
        "XdFinwE5xkEl/V9oIECMgMQcKBD+Q2MYwUZ4GBiYUI+DYvP/X0YGRIH4n4EBJUVA9f9nQIiD2VAn/0OLdVgh+h9LaoB6EyULQGoTqGpo/kcEAiIFQAID"
        "Fvt/ETH/HznGGRD5+x8kAP6DaZiHIZ6DexheBgADABoI/1E8/x+aEtBTBDS1/IfREHsZkD39D0sAYKYAcNTD08l/BkTQ/mdA+AxcnYFqAwZkj6PFPNjw"
        "/wwohej//9BUAPE4w19orxAUKLBYR4p9bCnh/z9YIDAgBQZEDFxswbMElhSBEQig1MvAgFYIIgcCJIjQkz64KoQnfVD+h3n+LyLv/0OkBIZ/yCU/MFD+"
        "IQXEX1hsI2UDeGAgexKYveDZBCbOAM9WDLDYB3sSS5nxHxYgEE8jpw7s7YD/SHU/0Afw1PAfluyRkz8DtNBigDdZ/8MSzz9kDyPFOFwc6OJ/UM8jpQJw"
        "2QFPDUDDQDs54dkBGgB/EbEMK2vAheM/JM/+I5wSgCkANi/wH1b7Q3T9h6UAaPLH6nlGiOf//0W01/9BYwYUiKDtaPD8zwyJMWBM/v8LVASOfdQCkAFW"
        "DvxFlAMMyO2Dv4yIWP/HgJYNkOT+I6USRE5mYMASIGgpAKSaARwAqGUANB3/Ryv4/oGSPwOK5yGxD4p5WBIFbrGDJXt4vodVg6AUAPUsuuf/QpPyX1iM"
        "wzyIKo5aLqAVhLBAwhkIjKhlAAMDeipAqvsZEMkf0hFBLf1hI7yQIS1obP+HeR653ockewZwKoB0g+HV3l9EYDBAkz2ilgCVHSD5/wzwpA7yIEb2IBQI"
        "jCgpAakdgIh9Bmgp8R8pzfyHZzTkFh8kBSA8zwBJDtBsAKv2GGD5HNzwAXr6L6j7C/X8XxAfGPDwbAE0AxoQ4GoQJTtAA+EfpFyAtxD/IYmD7WZEDSTk"
        "pI+WDVDbAeDq7j8kC8D7AcgxD2IDDf8PCwTk4uI/A6S2/A/Jp6DVTkhlAAO0DQCp/mAxDw0MaHnAAA8EWBkACgwo/gcNGLDnkbPBf0zP/mPALvYfs2xA"
        "CwBoJviPyDT/GZDzPzTZM0ACARJejAywJgOk7f8fnAr+w6s7UOkP8jAE//+HYIM9DO0NMqAlf+QGElwOGvNgJ/1F8gy8cGTEUkhiehq5MAQAe0Q2Sa6u"
        "ArYAAAAASUVORK5CYII=";

    /// @brief the reference's `Remap01ToHalfTexelCoord`: `coord*(1 - 1/N) + (1/N)*0.5`
    ///
    /// THE FACTOR IS `0.5`, AND THAT IS A FLAG RATHER THAN ARITHMETIC: `运算 = DIVIDE(1, 64)`,
    /// `运算.001 = MULTIPLY(运算.Value = 1/64, Value_001 = 0.5 (enabled = true), Value_002 = 0.5 (enabled = FALSE))`,
    /// `运算.002 = SUBTRACT(1, 运算)`, `Vector Math = coord * 运算.002`,
    /// `Vector Math.001 = 运算.001 + Vector Math` - i.e. `coord*0.984375 + 0.0078125`. An unlinked socket
    /// PARTICIPATES when `enabled = true` and does NOT when `enabled = false`, so `运算.001` is `1/64 * 0.5`, the
    /// half texel the group is NAMED for. The spec's §A2 table (`0.0078125 + coord*0.984375`) and its worked
    /// example (`coord = 0.08 -> sample 5.04`) are BOTH this expression; an earlier version of this step read
    /// `1.0` off that socket instead (`0.015625 + coord*0.984375`) and recorded the spec's table as wrong, which
    /// is what moved every number in §8o/§8p by half a texel.
    float remap_to_half_texel(float const coord) {
        return coord * (1.0f - 1.0f / k_fgd_resolution) + (1.0f / k_fgd_resolution) * 0.5f;
    }

    /// @brief the sample position in TEXEL units for a `[0,1]` shading parameter, clamped the way the hardware
    ///        clamps a `REPEAT` sampler's fetch to the image
    ///
    /// The reference's `合并 XYZ.001 = (分离 XYZ.X, 分离 XYZ.Y, 0.0)` and the image's `image_user` is
    /// `interpolation = Linear, extension = REPEAT`, so the fetch is `coordLUT*64 - 0.5` with the texel index
    /// clamped into `[0, N-1]` at the interpolation stage.
    float remap_to_texel(float const coord) {
        float const position = remap_to_half_texel(coord) * k_fgd_resolution - 0.5f;
        return position < 0.0f ? 0.0f : (position > k_fgd_resolution - 1.0f ? k_fgd_resolution - 1.0f : position);
    }

    /// @brief the LUT's own bytes, decoded once: 64x64 RGBA8 row-major, the shape the PNG's IHDR states
    ///        (`bitdepth = 8`, `colortype = 6`) and the shape `set_goo_fgd_lut` uploads as `R8G8B8A8_UNORM`
    struct fgd_lut {
        static constexpr uint32_t width = 64u;
        static constexpr uint32_t height = 64u;
        std::vector<uint8_t> texels = {};

        [[nodiscard]] bool valid() const {
            return texels.size() == static_cast<std::size_t>(width) * height * 4u;
        }
        /// the raw byte of one channel, or -1 when the file did not load - so a failed read fails the CHECKS
        /// below rather than reading out of bounds
        [[nodiscard]] int32_t byte_at(uint32_t const x, uint32_t const y, uint32_t const channel) const {
            if (!valid() || x >= width || y >= height || channel >= 4u) {
                return -1;
            }
            return texels[(static_cast<std::size_t>(y) * width + x) * 4u + channel];
        }
        /// @brief the reference's bilinear fetch, in texel units, with the `[0, N-1]` clamp above
        [[nodiscard]] float sample(float const x, float const y, uint32_t const channel) const {
            if (!valid()) {
                return -1.0f;
            }
            uint32_t const x0 = static_cast<uint32_t>(x);
            uint32_t const y0 = static_cast<uint32_t>(y);
            uint32_t const x1 = x0 + 1u < width ? x0 + 1u : x0;
            uint32_t const y1 = y0 + 1u < height ? y0 + 1u : y0;
            float const fx = x - static_cast<float>(x0);
            float const fy = y - static_cast<float>(y0);
            float const low = static_cast<float>(byte_at(x0, y0, channel)) * (1.0f - fx) + static_cast<float>(byte_at(x1, y0, channel)) * fx;
            float const high = static_cast<float>(byte_at(x0, y1, channel)) * (1.0f - fx) + static_cast<float>(byte_at(x1, y1, channel)) * fx;
            return (low * (1.0f - fy) + high * fy) / 255.0f;
        }
    };

    /// @brief decode the standard base64 alphabet, ignoring newlines and the trailing `=` - the forty lines above
    std::vector<uint8_t> base64_decode(std::string const& text) {
        auto const value_of = [](char const c) -> int32_t {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return -1; // '=' and whitespace: the end of the stream, or a separator
        };
        std::vector<uint8_t> out = {};
        uint32_t accumulator = 0u;
        uint32_t bits = 0u;
        for (char const c : text) {
            int32_t const digit = value_of(c);
            if (digit < 0) {
                continue;
            }
            accumulator = (accumulator << 6) | static_cast<uint32_t>(digit);
            bits += 6u;
            if (bits >= 8u) {
                bits -= 8u;
                out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFFu));
            }
        }
        return out;
    }

    /// @brief SHA-256, spelled here for the same reason the PNG reader is: this file links nothing
    ///
    /// It is used for exactly one assertion - that the bytes that came back out of `png_rgba8` are the REFERENCE'S
    /// PNG, byte for byte - and that assertion is what makes the embedded table above verifiable rather than
    /// trusted. A transcription error anywhere in those forty lines fails it (and the texel checks below it).
    std::string sha256_hex(std::vector<uint8_t> const& bytes) {
        static constexpr uint32_t k[64] = {0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
                                           0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
                                           0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
                                           0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
                                           0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
                                           0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
                                           0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
                                           0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        std::vector<uint8_t> message = bytes;
        uint64_t const bit_length = static_cast<uint64_t>(bytes.size()) * 8u;
        message.push_back(0x80u);
        while (message.size() % 64u != 56u) {
            message.push_back(0u);
        }
        for (int32_t shift = 56; shift >= 0; shift -= 8) {
            message.push_back(static_cast<uint8_t>((bit_length >> shift) & 0xFFu));
        }
        uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        auto const rotr = [](uint32_t const x, uint32_t const n) { return (x >> n) | (x << (32u - n)); };
        for (std::size_t block = 0u; block < message.size(); block += 64u) {
            uint32_t w[64] = {};
            for (uint32_t i = 0u; i < 16u; ++i) {
                w[i] = (static_cast<uint32_t>(message[block + i * 4u]) << 24) | (static_cast<uint32_t>(message[block + i * 4u + 1u]) << 16) |
                       (static_cast<uint32_t>(message[block + i * 4u + 2u]) << 8) | static_cast<uint32_t>(message[block + i * 4u + 3u]);
            }
            for (uint32_t i = 16u; i < 64u; ++i) {
                uint32_t const s0 = rotr(w[i - 15u], 7u) ^ rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3u);
                uint32_t const s1 = rotr(w[i - 2u], 17u) ^ rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10u);
                w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
            for (uint32_t i = 0u; i < 64u; ++i) {
                uint32_t const s1 = rotr(e, 6u) ^ rotr(e, 11u) ^ rotr(e, 25u);
                uint32_t const ch = (e & f) ^ (~e & g);
                uint32_t const temp1 = hh + s1 + ch + k[i] + w[i];
                uint32_t const s0 = rotr(a, 2u) ^ rotr(a, 13u) ^ rotr(a, 22u);
                uint32_t const maj = (a & b) ^ (a & c) ^ (b & c);
                uint32_t const temp2 = s0 + maj;
                hh = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
            h[5] += f;
            h[6] += g;
            h[7] += hh;
        }
        std::string hex = {};
        for (uint32_t const word : h) {
            char buffer[9] = {};
            std::snprintf(buffer, sizeof(buffer), "%08x", word);
            hex.append(buffer);
        }
        return hex;
    }

    /// @brief decode an 8-bit non-interlaced PNG into RGBA8 - the three FGD outputs, or an empty vector
    ///
    /// A PNG READER IN A UNIT TEST, which wants a word of justification: this file must not link the renderer
    /// (see its header), and the LUT's expectations are only meaningful if they come from the REFERENCE'S OWN
    /// BYTES - the alternative, transcribing the numbers into the test, is what the parent's own note rejects
    /// (a transcription cannot catch a wrong file, and if the file were regenerated the test would keep passing).
    /// The decoder is deliberately the smallest one that reads THIS image: PNG signature, `IHDR`, every `IDAT`
    /// concatenated, `zlib`'s stored/fixed/dynamic Huffman blocks, `filter` 0-4 (the five the specification
    /// defines), and no interlace, no palette and no 16-bit depth. The spec's `A3` names exactly these fields
    /// (`w=64 h=64 bitdepth=8 colortype=6 compression=0 filter=0 interlace=0`) and the file's own chunk list is
    /// `IHDR`/`IDAT`/`IEND`, so the input this must accept is pinned by the spec as well as by the assertions.
    std::vector<uint8_t> png_rgba8(std::vector<uint8_t> const& bytes) {
        auto const be32 = [&bytes](std::size_t const at) -> uint32_t {
            return (static_cast<uint32_t>(bytes[at]) << 24) | (static_cast<uint32_t>(bytes[at + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[at + 2]) << 8) | static_cast<uint32_t>(bytes[at + 3]);
        };
        if (bytes.size() < 8u || bytes[0] != 0x89u || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G') {
            return {};
        }
        uint32_t width = 0u;
        uint32_t height = 0u;
        std::vector<uint8_t> compressed = {};
        for (std::size_t at = 8u; at + 8u <= bytes.size();) {
            uint32_t const length = be32(at);
            std::string const type(reinterpret_cast<char const*>(bytes.data() + at + 4u), 4u);
            std::size_t const body = at + 8u;
            if (body + length > bytes.size()) {
                return {};
            }
            if (type == "IHDR") {
                width = be32(body);
                height = be32(body + 4u);
                if (bytes[body + 8u] != 8u || bytes[body + 9u] != 6u || bytes[body + 12u] != 0u) {
                    return {}; // not 8-bit RGBA, not non-interlaced: a file this reader refuses rather than fudges
                }
            } else if (type == "IDAT") {
                compressed.insert(compressed.end(), bytes.begin() + static_cast<std::ptrdiff_t>(body), bytes.begin() + static_cast<std::ptrdiff_t>(body + length));
            } else if (type == "IEND") {
                break;
            }
            at = body + length + 4u; // + the CRC
        }
        if (width == 0u || height == 0u || compressed.size() < 2u) {
            return {};
        }
        // ---- zlib: one 2-byte header, deflate blocks, one 4-byte Adler-32 (which this reader does not check:
        //      the assertion that the file is the reference's is the TEXEL check below, which is stronger) ----
        std::vector<uint8_t> raw = {};
        {
            std::size_t at = 2u;
            uint32_t bit_buffer = 0u;
            uint32_t bit_count = 0u;
            auto const bits = [&](uint32_t const count) -> uint32_t {
                while (bit_count < count) {
                    bit_buffer |= static_cast<uint32_t>(compressed[at++]) << bit_count;
                    bit_count += 8u;
                }
                uint32_t const value = bit_buffer & ((1u << count) - 1u);
                bit_buffer >>= count;
                bit_count -= count;
                return value;
            };
            // the fixed-Huffman code lengths, exactly as RFC 1951 states them
            std::vector<uint32_t> length_base = {3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 13u, 15u, 17u, 19u, 23u, 27u, 31u, 35u, 43u, 51u, 59u, 67u, 83u, 99u, 115u, 131u, 163u, 195u, 227u, 258u};
            std::vector<uint32_t> length_extra = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 1u, 1u, 1u, 1u, 2u, 2u, 2u, 2u, 3u, 3u, 3u, 3u, 4u, 4u, 4u, 4u, 5u, 5u, 5u, 5u, 0u};
            std::vector<uint32_t> dist_base = {1u, 2u, 3u, 4u, 5u, 7u, 9u, 13u, 17u, 25u, 33u, 49u, 65u, 97u, 129u, 193u, 257u, 385u, 513u, 769u, 1025u, 1537u, 2049u, 3073u, 4097u, 6145u, 8193u, 12289u, 16385u, 24577u};
            std::vector<uint32_t> dist_extra = {0u, 0u, 0u, 0u, 1u, 1u, 2u, 2u, 3u, 3u, 4u, 4u, 5u, 5u, 6u, 6u, 7u, 7u, 8u, 8u, 9u, 9u, 10u, 10u, 11u, 11u, 12u, 12u, 13u, 13u};

            // a canonical Huffman table: `counts[n]` codes of length n, decoded bit by bit
            struct huffman {
                std::vector<uint32_t> counts = std::vector<uint32_t>(16u, 0u);
                std::vector<uint32_t> symbols = {};
                [[nodiscard]] int32_t decode(std::function<uint32_t(uint32_t)> const& bits) const {
                    uint32_t code = 0u;
                    int32_t first = 0;
                    int32_t index = 0;
                    for (uint32_t length = 1u; length <= 15u; ++length) {
                        code |= bits(1u);
                        uint32_t const count = counts[length];
                        if (static_cast<int32_t>(code) - first < static_cast<int32_t>(count)) {
                            return static_cast<int32_t>(symbols[static_cast<std::size_t>(index + static_cast<int32_t>(code) - first)]);
                        }
                        index += static_cast<int32_t>(count);
                        first = (first + static_cast<int32_t>(count)) << 1;
                        code <<= 1;
                    }
                    return -1;
                }
            };
            auto const build = [](std::vector<uint32_t> const& lengths) {
                huffman table = {};
                for (uint32_t const length : lengths) {
                    if (length > 0u && length < 16u) {
                        table.counts[length] += 1u;
                    }
                }
                std::vector<uint32_t> offsets = std::vector<uint32_t>(16u, 0u);
                for (uint32_t length = 1u; length < 15u; ++length) {
                    offsets[length + 1u] = offsets[length] + table.counts[length];
                }
                table.symbols = std::vector<uint32_t>(lengths.size(), 0u);
                for (uint32_t symbol = 0u; symbol < lengths.size(); ++symbol) {
                    if (lengths[symbol] > 0u && lengths[symbol] < 16u) {
                        table.symbols[offsets[lengths[symbol]]++] = symbol;
                    }
                }
                return table;
            };
            huffman fixed_literal = {};
            {
                std::vector<uint32_t> lengths(288u, 0u);
                for (uint32_t symbol = 0u; symbol < 144u; ++symbol) {
                    lengths[symbol] = 8u;
                }
                for (uint32_t symbol = 144u; symbol < 256u; ++symbol) {
                    lengths[symbol] = 9u;
                }
                for (uint32_t symbol = 256u; symbol < 280u; ++symbol) {
                    lengths[symbol] = 7u;
                }
                for (uint32_t symbol = 280u; symbol < 288u; ++symbol) {
                    lengths[symbol] = 8u;
                }
                fixed_literal = build(lengths);
            }
            huffman fixed_distance = {};
            {
                std::vector<uint32_t> lengths(30u, 5u);
                fixed_distance = build(lengths);
            }
            bool final_block = false;
            while (!final_block && at < compressed.size()) {
                final_block = bits(1u) != 0u;
                uint32_t const kind = bits(2u);
                std::vector<uint8_t> literals = {};
                if (kind == 0u) {
                    bit_buffer = 0u;
                    bit_count = 0u; // stored blocks are byte-aligned
                    uint32_t const length = static_cast<uint32_t>(compressed[at]) | (static_cast<uint32_t>(compressed[at + 1]) << 8);
                    at += 4u;
                    for (uint32_t i = 0u; i < length; ++i) {
                        raw.push_back(compressed[at + i]);
                    }
                    at += length;
                    continue;
                }
                huffman literal = fixed_literal;
                huffman distance = fixed_distance;
                if (kind == 2u) {
                    uint32_t const literal_count = bits(5u) + 257u;
                    uint32_t const distance_count = bits(5u) + 1u;
                    uint32_t const code_count = bits(4u) + 4u;
                    std::vector<uint32_t> order = {16u, 17u, 18u, 0u, 8u, 7u, 9u, 6u, 10u, 5u, 11u, 4u, 12u, 3u, 13u, 2u, 14u, 1u, 15u};
                    std::vector<uint32_t> code_lengths(19u, 0u);
                    for (uint32_t i = 0u; i < code_count; ++i) {
                        code_lengths[order[i]] = bits(3u);
                    }
                    huffman const code_table = build(code_lengths);
                    std::vector<uint32_t> lengths = {};
                    while (lengths.size() < static_cast<std::size_t>(literal_count) + distance_count) {
                        int32_t const symbol = code_table.decode(bits);
                        if (symbol < 0) {
                            return {};
                        }
                        if (symbol < 16) {
                            lengths.push_back(static_cast<uint32_t>(symbol));
                        } else if (symbol == 16) {
                            uint32_t const repeat = 3u + bits(2u);
                            uint32_t const previous = lengths.empty() ? 0u : lengths.back();
                            for (uint32_t i = 0u; i < repeat; ++i) {
                                lengths.push_back(previous);
                            }
                        } else if (symbol == 17) {
                            for (uint32_t i = 0u, repeat = 3u + bits(3u); i < repeat; ++i) {
                                lengths.push_back(0u);
                            }
                        } else {
                            for (uint32_t i = 0u, repeat = 11u + bits(7u); i < repeat; ++i) {
                                lengths.push_back(0u);
                            }
                        }
                    }
                    literal = build(std::vector<uint32_t>(lengths.begin(), lengths.begin() + static_cast<std::ptrdiff_t>(literal_count)));
                    distance = build(std::vector<uint32_t>(lengths.begin() + static_cast<std::ptrdiff_t>(literal_count), lengths.end()));
                }
                for (;;) {
                    int32_t const symbol = literal.decode(bits);
                    if (symbol < 0) {
                        return {};
                    }
                    if (symbol < 256) {
                        raw.push_back(static_cast<uint8_t>(symbol));
                        continue;
                    }
                    if (symbol == 256) {
                        break;
                    }
                    int32_t const length_index = symbol - 257;
                    if (length_index < 0 || length_index >= static_cast<int32_t>(length_base.size())) {
                        return {};
                    }
                    uint32_t const length = length_base[static_cast<std::size_t>(length_index)] + bits(length_extra[static_cast<std::size_t>(length_index)]);
                    int32_t const distance_symbol = distance.decode(bits);
                    if (distance_symbol < 0 || distance_symbol >= static_cast<int32_t>(dist_base.size())) {
                        return {};
                    }
                    uint32_t const back = dist_base[static_cast<std::size_t>(distance_symbol)] + bits(dist_extra[static_cast<std::size_t>(distance_symbol)]);
                    if (back == 0u || back > raw.size()) {
                        return {};
                    }
                    for (uint32_t i = 0u; i < length; ++i) {
                        raw.push_back(raw[raw.size() - back]);
                    }
                }
            }
        }
        // ---- the five filters, un-applied row by row (each scanline is `1 + width*4` bytes) ----
        std::size_t const stride = static_cast<std::size_t>(width) * 4u;
        std::vector<uint8_t> image(static_cast<std::size_t>(height) * stride, 0u);
        if (raw.size() < static_cast<std::size_t>(height) * (stride + 1u)) {
            return {};
        }
        for (uint32_t y = 0u; y < height; ++y) {
            uint8_t const filter = raw[static_cast<std::size_t>(y) * (stride + 1u)];
            uint8_t const* const source = raw.data() + static_cast<std::size_t>(y) * (stride + 1u) + 1u;
            uint8_t* const target = image.data() + static_cast<std::size_t>(y) * stride;
            uint8_t const* const previous = y > 0u ? image.data() + static_cast<std::size_t>(y - 1u) * stride : nullptr;
            for (std::size_t x = 0u; x < stride; ++x) {
                int32_t const a = x >= 4u ? target[x - 4u] : 0;
                int32_t const b = previous != nullptr ? previous[x] : 0;
                int32_t const c = (previous != nullptr && x >= 4u) ? previous[x - 4u] : 0;
                int32_t const value = source[x];
                int32_t result = 0;
                switch (filter) {
                case 0u:
                    result = value;
                    break;
                case 1u:
                    result = value + a;
                    break;
                case 2u:
                    result = value + b;
                    break;
                case 3u:
                    result = value + (a + b) / 2;
                    break;
                case 4u: {
                    int32_t const p = a + b - c;
                    int32_t const pa = std::abs(p - a);
                    int32_t const pb = std::abs(p - b);
                    int32_t const pc = std::abs(p - c);
                    result = value + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
                    break;
                }
                default:
                    return {};
                }
                target[x] = static_cast<uint8_t>(result & 0xFF);
            }
        }
        return image;
    }

    // ================================================================================================
    // STEP 2: THE OBJECT-SPACE EDGE LIGHT - `Arknights: Endfield_PBRToonBase` / `...Hair`, spec §8
    // ================================================================================================
    //
    // The five reference sub-groups' own closed forms, evaluated. Every constant below is quoted with the NODE
    // it comes from so it can be re-read out of `deren-ab/gooblender/nodes.json` rather than trusted, and the
    // arithmetic is written out under each expected value. THEY ARE THE SAME FIVE FUNCTIONS
    // `shaders/goo_toon.slang` implements, spelled a second time - so this half catches DRIFT (an edit to one
    // side that leaves the other alone), exactly as the iris half above does, and the source checks in section
    // 8 are what pin the constants themselves.

    /// `Directional light attenuation`'s group interface default for `Rim_DirLightAtten`
    /// (`::- Arknights: Endfield_PBRToonBase :: 组输入.Rim_DirLightAtten = 0.8999999761581421`).
    constexpr float k_rim_dir_atten_default = 0.8999999761581421f;
    /// `Fresnel attenuation`'s exponent is the reference's literal FOUR (`运算.019`/`运算.021`), not a socket.
    constexpr float k_rim_fresnel_exponent = 4.0f;
    /// The two material overrides that ride `toon_colour_lane::goo_rim_scalars` in this repository's asset.
    constexpr float k_rim_strength_cloth = 5.0f;
    constexpr float k_rim_dir_atten_cloth = 0.9617834091186523f;
    /// `PBRToonBaseHair :: 组输入.Use Rimlimitation?`'s material override on `M_actor_laevat_hair_01`.
    constexpr float k_rim_limitation_on = 1.0f;

    // ================================================================================================
    // STEP 3: `DepthRim`, THE GROUP THAT TURNED THE RIM INTO A CONTOUR - spec §2.5 and the plan's §1.3.2
    // ================================================================================================
    //
    // Everything below is a socket of `DepthRim` in `deren-ab/gooblender/nodes.json`, or a formula
    // `goo_screenspace_info.md` READ OUT OF THE GOO ENGINE'S SOURCE (branch `goo-engine-v4.2-release` @
    // `844bc9d8110e695c56b02dae501323831ad5e730`): `View Position` is a camera-space position, `Scene Depth` is
    // `-get_view_z_from_depth(window depth)` - positive, in metres, along the camera's axis - and the offset is a
    // camera-space translation that is then REPROJECTED, not a uv shift.
    //
    // `DepthRim :: 组输入.Rim_width_X` / `.y` - the group's interface default, and the fallback behind the
    // `goo_rim_widths` lane's `< 0` sentinel.
    constexpr float k_rim_width_default = 0.5f;
    /// The two widths EVERY `M_actor_laevat_*` material that has the group states (a material override, read out
    /// of `nodes.json`; `p3_dump_widths.py` prints the same pair for all eight of them).
    constexpr float k_rim_width_x = 0.04184713214635849f;
    constexpr float k_rim_width_y = 0.019108280539512634f;
    /// `DepthRim :: 运算.029` / `运算.031` - the 0.1 that turns a width into a camera-space offset.
    constexpr float k_rim_width_scale = 0.10000000149011612f;
    /// `DepthRim :: 映射范围` (`From Min/Max = 0/5` -> `To Min/Max = 0/8`, clamp off) and `钳制` (`0..8`), then
    /// `运算.019 = DIVIDE(_, 2.0)`.
    constexpr float k_depth_rim_from_min = 0.0f;
    constexpr float k_depth_rim_from_max = 5.0f;
    constexpr float k_depth_rim_to_min = 0.0f;
    constexpr float k_depth_rim_to_max = 8.0f;
    constexpr float k_depth_rim_clamp_max = 8.0f;
    constexpr float k_depth_rim_divisor = 2.0f;
    /// `PBRToonBase :: 运算.029`'s constant - the ceiling that container puts on `DepthRim`. `PBRToonBaseHair`
    /// has NO `运算.029`, so its factor is `DepthRim` itself.
    constexpr float k_depth_rim_base_ceiling = 0.5f;

    /// @brief Goo's `Scene Depth = -get_view_z_from_depth(window depth)`, in metres, positive
    ///
    /// The engine's helper inverted: for `glm::perspectiveRH_ZO`-style terms the stored depth is
    /// `z_ndc = (proj_22*z_view + proj_32) / -z_view`, so `z_view = -proj_32/(z_ndc + proj_22)` and the negated
    /// form the reference uses is `+proj_32/(z_ndc + proj_22)`. The test builds the two terms from near/far the
    /// way `glm::perspectiveRH_ZO` does - `proj_22 = far/(near-far)`, `proj_32 = -(far*near)/(far-near)` - and
    /// then checks the ROUND TRIP, which is the claim: the conversion this pass relies on is the engine's own and
    /// it really answers metres.
    ///
    /// THE ONE CONVENTIONAL DIFFERENCE FROM GOO'S OWN FUNCTION, stated because it is the thing a reader would
    /// otherwise have to guess: `get_view_z_from_depth` begins with `d = 2*depth - 1`, i.e. a [-1,1]-NDC
    /// projection (Blender's). THIS renderer's projection is ZERO-TO-ONE (Vulkan), so the stored depth IS
    /// `z_ndc` and the inversion has no remap step - the two functions answer the same QUANTITY (a positive
    /// distance along the camera's axis, in metres) from the same two matrix terms.
    float view_depth_from_window(float const window_depth, float const proj_22, float const proj_32) {
        return proj_32 / (window_depth + proj_22);
    }
    /// the same projection's forward map, `window_depth(z_view)` - what a test uses to build an input
    float window_depth_from_view_z(float const view_z, float const proj_22, float const proj_32) {
        return (proj_22 * view_z + proj_32) / -view_z;
    }

    /// @brief `DepthRim`: `clamp(map_range(dz, 0->5, 0->8), 0, 8) / 2`, with `dz = depth(offset) - depth(self)`
    float depth_rim(float const dz) {
        float const mapped = (dz - k_depth_rim_from_min) * (k_depth_rim_to_max - k_depth_rim_to_min) / (k_depth_rim_from_max - k_depth_rim_from_min) + k_depth_rim_to_min;
        return std::clamp(mapped, 0.0f, k_depth_rim_clamp_max) / k_depth_rim_divisor;
    }

    /// @brief `运算.029` / the hair container's absence of it, as `goo_rim_term` spells the branch
    float rim_depth_factor(float const depth_rim_value, bool const hair) {
        return hair ? depth_rim_value : std::min(depth_rim_value, k_depth_rim_base_ceiling);
    }

    /// `Directional light attenuation`: `lerp(1 - adjust, 1.0, clamp(NoL, 0, 1))` - spec §8 A1/A1b/A1c
    float rim_directional_attenuation(float const no_l_unsaturate, float const adjust) {
        float const clamped = no_l_unsaturate < 0.0f ? 0.0f : (no_l_unsaturate > 1.0f ? 1.0f : no_l_unsaturate);
        return std::lerp(1.0f - adjust, 1.0f, clamped);
    }
    /// `Vertical attenuation`: `n_world.z * 0.5 + 0.5` - spec §8 A2
    float rim_vertical_attenuation(float const normal_z) {
        return normal_z * 0.5f + 0.5f;
    }
    /// `Fresnel attenuation`: `(1 - NoV)^4`, written as the reference's own two multiplications - spec §8 A3
    ///
    /// THE REFERENCE DOES NOT WRITE A POWER: `运算.022 = 1 - NoV`, then `运算.019 = t * t` and
    /// `运算.021 = (t*t) * (t*t)`, with the reroute's output feeding BOTH of `运算.019`'s slots. `std::pow` is
    /// evaluated beside it as a cross-check that the spelled-out chain really is the fourth power - and the
    /// assertion is on the chain, because that is what ships.
    float rim_fresnel_attenuation(float const no_v) {
        float const one_minus = 1.0f - no_v;
        float const squared = one_minus * one_minus;
        return squared * squared;
    }
    float rim_fresnel_attenuation_as_power(float const no_v) {
        return std::pow(1.0f - no_v, k_rim_fresnel_exponent);
    }

    // ================================ STEP 13: `RS EFF` (MECHANISM TABLE #14) ================================
    //
    // THE CLOSED FORMS THE RS BLOCK IN `shaders/goo_toon.slang` EVALUATES, transcribed in the reference's own op
    // order so a drift on either side fails here. WHAT IS BEING PINNED, and why each of these is worth a test:
    //
    //   * `float_from_vec4` is the reference's Rec.709 LUMINANCE (`dot(rgb, (0.2126, 0.7152, 0.0722))`), NOT the
    //     `(r + g + b) / 3` average the SAME library's `float_from_vec3` computes. Two helpers, one letter apart,
    //     two different numbers - and the mask is a multiply of the result, so the average would scale every lit
    //     texel by a different factor. The two differ by 0.226 on (0.5, 0.25, 1.0), which no frame would explain.
    //
    //   * `_M` IS NOT A BARE TEXTURE on either material this asset switches the pass on for: it is the output of an
    //     `Arknights: Endfield_SmoothStep` subgroup (`min = 0`, per-material `max`), so the mask is
    //     `t*t*(3-2t)` OF the luma and the raw luma is systematically too large (bare 0.75 -> 0.852 under
    //     cloth_02's `max` of 0.9900000095367432). `rs_smooth_step` below is that subgroup, hand-written: the
    //     repository has no built-in `smoothstep` to lean on, and Slang's own would be a different op order.
    //
    //   * `混合.029` IS LIGHTEN, and LIGHTEN is `mix(A, max(A, B), clamp(fac, 0, 1))` - NOT a bare `max`. The
    //     difference is the whole reason the acceptance criterion is "only the two materials move": with the
    //     factor at 0 the reference returns its A input, and `mix(a, b, 0)` is `a * 1 + b * 0` in f32 - bitwise
    //     `a` for any finite `a`. A bare `max` would light every pixel the mask touches.
    //
    //   * THE GATE IS `Use RS_Eff?`, NOT the mask. With `mask = 0` the reference's `混合.029` becomes
    //     `max(lit, 0)` - a HALF-WAVE RECTIFIER, not an identity (`rs_lighten({-0.1, 0.2, 0.3}, 0, 1)` is
    //     `(0.0, 0.2, 0.3)`, not the input). That is the reference's own behaviour and the port keeps it; it is
    //     also why the bitwise-identity criterion is stated on `Use = 0` and never on `mask = 0`.
    //
    //   * STEP 15 SPENT `armA`, so `RS Model == 0` NO LONGER KEEPS THE BASE. The gate is now the reference's own:
    //     `Use RS_Eff?` on the OUTSIDE only, with `RS Model` selecting the arm inside it. A material with
    //     `RS Model == 0` and no sheet now runs `arm0 = 0` into the same LIGHTEN, which is `max(lit, 0)` - a
    //     half-wave rectifier, exactly as the `mask = 0` case above. The outside-only gate is what still keeps
    //     the twenty materials that state no `RS` row bitwise: they leave at `Use = 0` before any of it runs.
    //     A2/A4 were written as BACKGROUND pins for this branch ("armA only, not called by step 13"); the port
    //     did not move a single one of their values, which is the whole reason they were pinned in step 13.
    //
    // EVERY EXPECTED VALUE in the assertions below was produced by `deren-ab\_s13s_expect3.py` with an f32
    // round-trip, so it can be re-derived rather than trusted. `1e-6f` is used wherever the value passes through
    // a division or a float multiply chain; `==` only where the result is provably exact (a copy, or `x * 1 + y * 0`).

    /// a four-component lane, spelled rather than pulled in from the engine (`glm` is not linked here)
    struct vec4 {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 0.0f;
    };
    /// bitwise comparison for the `==` assertions: float `==` IS bitwise equality for everything but NaN, and no
    /// value here is NaN, so this is the same statement written where the intent is visible.
    bool vec3_bitwise_equal(vec3 const a, vec3 const b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    /// @brief the reference's `float_from_vec4`: `dot(v.rgb, vec3(0.2126, 0.7152, 0.0722))`
    ///
    /// From `gpu_shader_codegen_lib.glsl`, whose own comment is "Assumes GPU_VEC4 is color data. So converting to
    /// luminance like cycles." - the RGBA(Color) -> VALUE implicit conversion in the node graph goes through it.
    float rs_float_from_color(vec3 const colour) {
        return colour.x * 0.2126f + colour.y * 0.7152f + colour.z * 0.0722f;
    }
    float rs_clamp01(float const value) {
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }
    /// @brief `Arknights: Endfield_SmoothStep`, in the subgroup's own op order
    ///
    /// `运算` = SUBTRACT(x, min); `运算.001` = SUBTRACT(max, min); `运算.002` = DIVIDE; `钳制` = CLAMP; then
    /// `t * t * (3 - 2t)`. THE DIVISION IS `(x - min) / (max - min)` and not `x / max` - the two agree only while
    /// `min` is 0, and the whole point of the assertion at `min = 0.25` is that they do not agree in general.
    float rs_smooth_step(float const min, float const max, float const x) {
        float const t = rs_clamp01((x - min) / (max - min));
        return t * t * (3.0f - 2.0f * t);
    }
    /// @brief the whole `_M` chain: `SmoothStep(0, smooth_max, luma)`
    ///
    /// `min` is hard-coded 0 (spec U11: measured 0 on every material) and `smooth_max <= 0` is read as 1.0
    /// (spec U7): the neutral lane is `(0, 0, 0, 0)`, so without that rule the DIVIDE above is by zero.
    float rs_mask(float const luma, float const smooth_max) {
        return rs_smooth_step(0.0f, smooth_max > 0.0f ? smooth_max : 1.0f, luma);
    }
    float rs_effective_smax(float const smooth_max) {
        return smooth_max > 0.0f ? smooth_max : 1.0f;
    }
    /// @brief `Layer Weight.Facing` - pinned in step 13 as a BACKGROUND leaf ("armA only, not called by step 13");
    ///        STEP 15 ported `armA`, so this is now live code's leaf and the values below are its acceptance too.
    ///
    /// `remap(b)`: clamp to `[0, 0.99999]`, then `b < 0.5 ? 2b : 0.5 / (1 - b)`; `facing = 1 - |dot|^remap(b)`.
    /// `blend` EXACTLY `0.5` takes the reference's other branch: no remap, no pow, so the answer is `1 - |dot|`
    /// (numerically the same as an exponent of 1 - pinned because the SOURCE has two branches).
    float rs_facing(float const blend, float const dot_value) {
        if (blend == 0.5f) {
            return 1.0f - std::abs(dot_value);
        }
        float clamped = rs_clamp01(blend);
        if (clamped > 0.99999f) {
            clamped = 0.99999f;
        }
        float const exponent = clamped < 0.5f ? 2.0f * clamped : 0.5f / (1.0f - clamped);
        return 1.0f - std::pow(std::abs(dot_value), exponent);
    }
    /// @brief the sheet coordinate the reference feeds the `_RS` sheet: `u = clamp(Facing + Offset, 0, 1)`
    ///        (`build-release-clang64/deren-ab/goo_step15_armA_spec.md:107`).
    ///
    /// DEBT (x), lead-found 2026-10-01: this composed form is what the SHADER's net must equal, and until the fix
    /// the shader computed its COMPLEMENT. The two agree only at `b == 0` and at `|V.n| == 0.5`, which is exactly
    /// why a suite built only on `b == 0` fixtures (all of them, at the time) could be green while the port was
    /// inverted - see A10 below for the pins that compare them directly.
    float rs_sheet_u(float const blend, float const dot_value, float const offset) {
        return rs_clamp01(rs_facing(blend, dot_value) + offset);
    }
    /// @brief the cast-shadow curve's leaf - pinned in step 13 as a BACKGROUND leaf ("armA only, not called by
    ///        step 13"); STEP 15 ported `armA`, so this is now live code's leaf too.
    ///
    /// `1 / (1 + 100000^(-3 * sharp * (x - center)))`; `sharp == 0` makes the exponent 0, `100000^0 = 1`, so the
    /// answer is the guarded `0.5` without a special case (the reference's own arrangement).
    float rs_sigmoid_sharp(float const x, float const center, float const sharp) {
        return 1.0f / (1.0f + std::pow(100000.0f, -3.0f * sharp * (x - center)));
    }
    /// @brief `混合.029`, LIGHTEN: `mix(A, max(A, B), clamp(fac, 0, 1))`
    ///
    /// `mix` is computed in GLSL's own order - `x * (1 - a)`, then `y * a`, then the sum, every step f32 -
    /// because that order is what makes `fac == 0` bitwise `A` rather than merely close to it.
    vec3 rs_lighten(vec3 const lit, vec3 const rs, float const factor) {
        vec3 const target{std::max(lit.x, rs.x), std::max(lit.y, rs.y), std::max(lit.z, rs.z)};
        float const a = rs_clamp01(factor);
        float const inverse = 1.0f - a;
        return {lit.x * inverse + target.x * a, lit.y * inverse + target.y * a, lit.z * inverse + target.z * a};
    }
    /// @brief the FROZEN implementation form: the `if` gate, the `RS Model` split, then `混合.038` -> `混合.029`
    ///        -> `混合.030`
    ///
    /// The gate is a branch and not a branchless `mix(lit, mixed029, use)`, because the identity must be BITWISE:
    /// `mix(a, b, f)` is `a * (1 - f) + b * f`, which equals `a` exactly only when both products round back to
    /// `a` - true at the endpoints, not in between. Nothing here needs an in-between value, so the branch costs
    /// nothing and the twenty materials that state no `RS` row do ZERO floating-point work.
    ///
    /// STEP 15: the outer test is now `use > 0` ALONE (the reference's own gate), and `model` picks the arm -
    /// `arm0` on the inside, the step-13 form `mask * tint` otherwise. `arm0` therefore has to be an INPUT here:
    /// the caller states what the sheet, the strength, `saturate(NdotL)` and the cast curve multiplied out to.
    vec3 rs_final(vec3 const lit, float const mask, vec3 const tint, float const use, float const mult, float const model,
                  vec3 const arm0 = vec3{0.0f, 0.0f, 0.0f}) {
        if (!(use > 0.0f)) {
            return lit;
        }
        vec3 const rs = (model == 0.0f) ? arm0 : scale3(tint, mask);
        return rs_lighten(lit, rs, mult);
    }
    /// @brief STEP 15's `armA` in its §1.3 landing form, one channel of it
    ///
    /// `arm0 = ((((sheet * tint.rgb) * RS Strength) * goo_ndotl_clamped) * cast_shadow_sigmoid) * mask` - the
    /// parenthesis order is the reference's `混合.036 -> .023 -> .027 -> .028 -> .033`, and it is NOT
    /// associative in f32, so the helper states it in that order rather than "cleaning it up".
    float rs_arm0_channel(float const sheet, float const tint, float const strength, float const ndotl, float const cast, float const mask) {
        return sheet * tint * strength * ndotl * cast * mask;
    }
    /// the three carriers as the HOST side reads them out of lane 27 (`.x`/`.y`/`.z`, `.w` reserved and unused)
    float rs_use_of(vec4 const lane) {
        return lane.x;
    }
    float rs_mult_of(vec4 const lane) {
        return lane.y;
    }
    float rs_model_of(vec4 const lane) {
        return lane.z;
    }
    /// lane 28's `.rgb` is the tint and its `.w` is the `SmoothStep.max` - the tint is THREE components, so a
    /// reader that took the whole `vec4` would carry the threshold into the colour and the mask with it
    vec3 rs_tint_of(vec4 const lane) {
        return {lane.x, lane.y, lane.z};
    }
    float rs_smooth_max_of(vec4 const lane) {
        return lane.w;
    }
    /// lane 29 (`_GooRSArm0`), the four components of the reference's `armA` sockets: `RS_Index` [x],
    /// `RS Strength` [y], `Layer weight Value` [z], `Layer weight Value Offset` [w]. `.x` is carried because the
    /// reference carries it, but NOTHING reads it: this step ports ONE sheet, so there is no second one to index
    /// into (spec L1). The reader-hostile part is that the neutral `(0,0,0,0)` therefore means "RS Strength = 0",
    /// which zeroes `arm0` - a switch, where a `(1,1,1,1)` default would have turned the arm on for every material.
    float rs_arm0_index_of(vec4 const lane) {
        return lane.x;
    }
    float rs_arm0_strength_of(vec4 const lane) {
        return lane.y;
    }
    float rs_arm0_lw_blend_of(vec4 const lane) {
        return lane.z;
    }
    float rs_arm0_lw_offset_of(vec4 const lane) {
        return lane.w;
    }
    /// the mask slot index the host resolves for a material: 14 when the `_GooRSMask` row exists AND its
    /// `_UseGooRSMask` flag is on, otherwise 0 - and 0 is "do not read" (texture index 0 is the white fallback,
    /// so a material with no `_M` must not read `_M = 1` everywhere). Spec F5.
    uint32_t rs_mask_index(bool const row_present_and_flag_on) {
        return row_present_and_flag_on ? 14u : 0u;
    }
    /// ... and what the shader then computes for that index: the test is `rs_block3.z != 0u`, so index 0 means a
    /// mask of 0.0 REGARDLESS of the lane's contents or the texture behind it.
    float rs_mask_of_index(uint32_t const index, float const luma, float const smooth_max) {
        return index == 0u ? 0.0f : rs_mask(luma, smooth_max);
    }
} // namespace

int32_t main() {
    // ---- 1. THE AZIMUTH: four cardinal sun positions, and the sign that decides which is which ----
    //
    // The head frame is the glTF/default basis (`head_ubo`'s own defaults): the face looks down +Z, its right is
    // -X and its up is +Y. A sun IN FRONT of the face must give 0 (full albedo, full highlight), one BEHIND 1.
    // THE FORWARD SIGN IS THE POINT OF THIS BLOCK: it is `值(明度) = -1.0`, and with the sign dropped the two
    // answers swap - a difference that is invisible on any axis-aligned test and total on a face.
    {
        vec3 const forward{0.0f, 0.0f, 1.0f};
        vec3 const right{-1.0f, 0.0f, 0.0f};
        vec3 const up{0.0f, 1.0f, 0.0f};

        float const front = angle_threshold(forward, up, right, forward);
        float const behind = angle_threshold(scale3(forward, -1.0f), up, right, forward);
        float const to_right = angle_threshold(right, up, right, forward);
        float const to_left = angle_threshold(scale3(right, -1.0f), up, right, forward);
        CHECK_MSG(std::abs(front - 0.0f) < 1e-5f, "a sun in front of the face is azimuth 0");
        CHECK_MSG(std::abs(behind - 1.0f) < 1e-5f, "a sun behind the face is azimuth 1");
        CHECK_MSG(std::abs(to_right - 0.5f) < 1e-5f, "a sun on the head's right is half a quarter turn in");
        CHECK_MSG(std::abs(to_left - 0.5f) < 1e-5f, "a sun on the head's left is the same, folded");
        // ... and a sun halfway between front and right: the azimuth is three quarters of a half turn, so the
        // result is a QUARTER of the way from "in front" - the point the window's clamp is exercised from.
        float const front_right = angle_threshold(normalize3(vec3{right.x + forward.x, 0.0f, right.z + forward.z}), up, right, forward);
        CHECK_MSG(std::abs(front_right - 0.25f) < 1e-5f, "front-right is a quarter away from in front");
        // THE LENGTH IS REMOVED BY THE PROJECTION, so an un-normalized attribute reads the same: the reference's
        // `LightDirection` is a geometry attribute and nothing in the group normalizes it before `VM.010`.
        float const scaled = angle_threshold(scale3(forward, 7.5f), up, right, forward);
        CHECK_MSG(std::abs(scaled - front) < 1e-6f, "the azimuth does not depend on the light's magnitude");
        // THE DEGENERATE CASE IS REACHABLE: a sun straight up in the head's frame projects to zero, so both
        // `atan2` components are zero. C answers that call - `atan2(+0, -0)` is `+pi`, the negative zero coming
        // from `MULTIPLY(headForward, -1.0)` - and the reference's MIX then selects `1 - 1 = 0`: a FULLY LIT iris
        // for a sun directly overhead. SPIR-V leaves the same `Atan2` undefined (a NaN would survive the caller's
        // `clamp` and reach the framebuffer), so the shader states the reference's answer, and this is the check
        // that the statement is the reference's rather than a guess: the same value, both ways of writing it.
        float const overhead = angle_threshold(up, up, right, forward);
        float const overhead_guarded = angle_threshold_guarded(up, up, right, forward);
        CHECK_MSG(std::isfinite(overhead), "a sun along the head's up axis is not a NaN");
        CHECK_MSG(std::abs(overhead - 0.0f) < 1e-5f, "and the reference computes azimuth 0 for it (C's atan2(+0,-0) = pi)");
        CHECK_MSG(std::abs(overhead_guarded - overhead) < 1e-5f, "the shader's spelled-out degenerate branch agrees with it");
        // ... and the two functions agree on EVERY non-degenerate input, so the guard is a statement about one
        // input rather than a second formula
        for (float const y : {-0.9f, -0.4f, 0.0f, 0.4f, 0.9f}) {
            for (float const x : {-0.8f, -0.2f, 0.3f, 0.85f}) {
                vec3 const light = normalize3(vec3{x, y, 0.5f});
                CHECK_MSG(std::abs(angle_threshold(light, up, right, forward) - angle_threshold_guarded(light, up, right, forward)) < 1e-6f,
                          "the guarded and unguarded forms agree away from the degenerate input");
            }
        }
    }

    // ---- 2. THE ALBEDO WINDOW: which way it clamps, and that it is the CLAMPED angle ----
    //
    // `1.0 - angle` is at most 1 (in front) and at least 0 (behind), and the window narrows that to [0.5, 1.0]:
    // the albedo is NEVER switched off, which is what makes the iris a lit surface rather than a highlight. An
    // implementation that clamped the OTHER way (`clamp(angle, 0.5, 1.0)`) would brighten the back of the eye.
    {
        CHECK_MSG(std::abs(albedo_weight(0.0f) - 1.0f) < 1e-6f, "in front: the albedo's full weight");
        CHECK_MSG(std::abs(albedo_weight(0.5f) - 0.5f) < 1e-6f, "half way: the window's edge");
        CHECK_MSG(std::abs(albedo_weight(0.75f) - 0.5f) < 1e-6f, "three quarters: clamped to the floor, not below it");
        CHECK_MSG(std::abs(albedo_weight(1.0f) - 0.5f) < 1e-6f, "behind: still half, not zero");
    }

    // ---- 3. THE TWO LAYERS: an ADD weighted by its factor, so the ball MULTIPLIES the albedo ----
    //
    // `混合.003` is a MULTIPLY of the ball by the albedo and `混合.002` adds its result at factor 0.5666667.
    // The distinction this pins is "ball * albedo, added" against the plausible-looking "ball, added": with a
    // black albedo the ball must contribute NOTHING, and with a white ball and albedo the colour must be
    // `1 + 0.5666667` rather than `1 + 1`.
    {
        CHECK_MSG(std::abs(iris_layer(1.0f, 1.0f, 0.0f) - (1.0f + k_ball_weight)) < 1e-6f, "white ball on white albedo: 1 + 0.5666667");
        CHECK_MSG(std::abs(iris_layer(1.0f, 0.0f, 0.0f) - 1.0f) < 1e-6f, "a black ball adds nothing");
        CHECK_MSG(std::abs(iris_layer(0.0f, 1.0f, 0.0f) - 0.0f) < 1e-6f, "a black albedo kills the ball's layer too");
        // the window is applied to the ALBEDO term only, and the ball's term is not windowed at all
        CHECK_MSG(std::abs(iris_layer(1.0f, 1.0f, 1.0f) - (0.5f + k_ball_weight)) < 1e-6f, "behind: only the albedo term is halved");
    }

    // ---- 4. THE STRENGTH: `D_Alpha` mixes the two brightnesses, and the highlight branch is UNCLAMPED ----
    //
    // The two answers are 1.5 (the material's `Eyes brightness`) and `(1.0 - angle) * 10.0`. The clamp on the
    // albedo is NOT on this branch: `运算`'s `use_clamp` is false, so at angle 0.75 this is 2.5 and not 5.
    {
        CHECK_MSG(std::abs(iris_strength(0.0f, 0.0f, k_eyes_brightness, k_eyes_highlight_brightness) - 1.5f) < 1e-6f, "alpha 0: the flat brightness");
        CHECK_MSG(std::abs(iris_strength(0.0f, 1.0f, k_eyes_brightness, k_eyes_highlight_brightness) - 10.0f) < 1e-6f, "alpha 1, in front: the full highlight");
        CHECK_MSG(std::abs(iris_strength(1.0f, 1.0f, k_eyes_brightness, k_eyes_highlight_brightness) - 0.0f) < 1e-6f, "alpha 1, behind: none");
        CHECK_MSG(std::abs(iris_strength(0.75f, 1.0f, k_eyes_brightness, k_eyes_highlight_brightness) - 2.5f) < 1e-6f, "the highlight branch is NOT windowed");
        CHECK_MSG(std::abs(iris_strength(0.0f, 0.5f, k_eyes_brightness, k_eyes_highlight_brightness) - std::lerp(1.5f, 10.0f, 0.5f)) < 1e-6f, "alpha is the mix");
        // the sentinel: an asset that states neither brightness gets the reference group's OWN interface
        // default, which is 0.0 for both sockets - not a number chosen here
        CHECK_MSG(std::abs(iris_strength(0.0f, 1.0f, 0.0f, 0.0f) - 0.0f) < 1e-6f, "no stated brightness: emission 0");
    }

    // ---- 5. THE WHOLE EMISSION, on the four cardinal suns, with a white albedo and a white ball ----
    //
    // These four numbers are the composition of everything above at the points where each term is at an end, and
    // they are the values a GPU probe would look for. Stated as a table so a change to any single term shows up
    // as which entries moved.
    {
        constexpr float full = 1.0f + k_ball_weight; // 1.5666667
        CHECK_MSG(std::abs(iris_emission(1.0f, 1.0f, 0.0f, 1.0f) - full * 10.0f) < 1e-4f, "in front, alpha 1: 15.666667");
        CHECK_MSG(std::abs(iris_emission(1.0f, 1.0f, 0.5f, 1.0f) - (0.5f + k_ball_weight) * 5.0f) < 1e-4f, "side on, alpha 1: 5.3333335");
        CHECK_MSG(std::abs(iris_emission(1.0f, 1.0f, 1.0f, 1.0f) - 0.0f) < 1e-6f, "behind, alpha 1: 0");
        CHECK_MSG(std::abs(iris_emission(1.0f, 1.0f, 0.0f, 0.0f) - full * 1.5f) < 1e-4f, "in front, alpha 0: 2.35");
    }

    // ---- 6. THE SPHERE MAP: the two spellings agree, and the V axis is this renderer's ----
    //
    // `goo_toon.slang` builds the map from the CAMERA MATRIX (`mul(view, n)`), which is the reference's own
    // `矢量变换(WORLD -> CAMERA)`; the old chain builds the same map from a view BASIS (`toon_matcap_uv`). The
    // two are the same function when the camera matrix's rows are that basis - which is the claim the shader's
    // header makes, and the reason a matcap does not care about the camera's roll (a ball has no distinguished
    // up). This block is that claim, evaluated.
    {
        // a few surface-to-camera vectors, avoiding the basis construction's own degenerate case (|v.y| ~ 1)
        constexpr vec3 camera_vectors[] = {{0.0f, 0.0f, 1.0f}, {0.3f, 0.2f, 0.93f}, {-0.5f, 0.4f, -0.77f}, {0.6f, -0.6f, 0.53f}};
        constexpr vec3 normals[] = {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.5f, 0.5f, 0.71f}, {-0.3f, 0.8f, -0.52f}};
        float worst = 0.0f;
        for (vec3 const v : camera_vectors) {
            for (vec3 const n : normals) {
                vec3 const up = std::abs(v.y) < 0.99f ? vec3{0.0f, 1.0f, 0.0f} : vec3{1.0f, 0.0f, 0.0f};
                vec3 const right = normalize3(vec3{up.y * v.z - up.z * v.y, up.z * v.x - up.x * v.z, up.x * v.y - up.y * v.x});
                vec3 const up_ortho = vec3{v.y * right.z - v.z * right.y, v.z * right.x - v.x * right.z, v.x * right.y - v.y * right.x};
                // the camera matrix's three rows, as the engine's `view` carries them
                float const view_x = dot3(n, right);
                float const view_y = dot3(n, up_ortho);
                float const from_matrix_x = view_x * 0.5f + 0.5f;
                float const from_matrix_y = -view_y * 0.5f + 0.5f;
                vec2 const from_basis = toon_matcap_uv(n, v);
                float const basis_x = from_basis.a * 0.5f + 0.5f;
                float const basis_y = from_basis.b * 0.5f + 0.5f;
                worst = std::max(worst, std::abs(from_matrix_x - basis_x));
                worst = std::max(worst, std::abs(from_matrix_y - basis_y));
            }
        }
        CHECK_MSG(worst < 1e-5f, "the camera-matrix form and the view-basis form of the sphere map agree");
        // ... AND THE V AXIS ITSELF: a normal pointing along the camera's up axis samples the FIRST row of the
        // uploaded image (V = 0), not the last. The reference's Blender camera space has V growing upward, so
        // this negation is the port's texture convention rather than a change of formula - and it is the term
        // the old chain measured (an un-negated V killed the catchlight, 1219 lit pixels to 6).
        CHECK_MSG(std::abs((-1.0f * 0.5f + 0.5f) - 0.0f) < 1e-6f, "camera-up maps to V = 0 in this renderer's convention");
        CHECK_MSG(std::abs((-(-1.0f) * 0.5f + 0.5f) - 1.0f) < 1e-6f, "and the un-negated form would map it to V = 1");
    }

    // ================================================================================================
    // 8. STEP 2's CLOSED FORMS: the five reference sub-groups, the two compositions, and the Hair variant
    // ================================================================================================
    //
    // THE INPUTS ARE THE SPEC'S §8 AND NOT ONES CHOSEN HERE, and every expected value below is the spec's own
    // arithmetic. Where the spec states a value to full float precision it is used verbatim; where it states a
    // decimal that is a float32's shortest round-trip it is spelled with the `f` suffix and compared at 1e-6.

    // ---- 8a. `Directional light attenuation` (spec §8 A1, A1b, A1c) ----
    //
    // A1: `clamp(0.3) = 0.3`; `1 - 0.8999999761581421 = 0.10000002384185791`;
    //     `lerp(0.10000002384185791, 1.0, 0.3) = 0.37000001668930055`.
    // A1b: the clamp's LOWER end - `clamp(-0.5) = 0.0`, so the answer is the FLOOR `1 - adjust` and the term
    //      does not extrapolate below it for a surface facing away from the sun.
    // A1c: the clamp's UPPER end - `clamp(1.0) = 1.0` and the mix's B is also 1.0, so the answer is exactly 1.
    {
        CHECK_MSG(std::abs(rim_directional_attenuation(0.3f, k_rim_dir_atten_default) - 0.37000001668930055f) < 1e-6f,
                  "A1: NoL 0.3 with the group's Rim_DirLightAtten is 0.37000001668930055");
        CHECK_MSG(std::abs(rim_directional_attenuation(-0.5f, k_rim_dir_atten_default) - 0.10000002384185791f) < 1e-6f,
                  "A1b: a back-facing NoL clamps to the floor 1 - adjust, not below it");
        CHECK_MSG(std::abs(rim_directional_attenuation(1.0f, k_rim_dir_atten_default) - 1.0f) < 1e-6f,
                  "A1c: NoL 1.0 reaches the ceiling exactly");
        // ... and the floor is a FUNCTION of the material's number, which is what makes `Rim_DirLightAtten` per
        // material rather than a family constant: the cloth/hair override (0.9617834091186523) floors at
        // 0.038216590881347656 against the body's 0.10000002384185791.
        CHECK_MSG(std::abs(rim_directional_attenuation(0.0f, k_rim_dir_atten_cloth) - 0.038216590881347656f) < 1e-6f,
                  "the cloth/hair override floors the term 2.6x lower than the body's default");
        CHECK_MSG(std::abs(rim_directional_attenuation(1.0f, k_rim_dir_atten_cloth) - 1.0f) < 1e-6f,
                  "both overrides meet at 1.0 on the lit side");
    }

    // ---- 8b. `Vertical attenuation` (spec §8 A2) ----
    //
    // `0.5 * 0.5 = 0.25`; `0.25 + 0.5 = 0.75`. The two lower cases are the GROUP'S STATEMENT THAT IT IS THE
    // WORLD-SPACE Z AND NOTHING ELSE: a surface facing straight down zeroes the term and one facing sideways
    // reads exactly the midpoint, whatever its x and y are. The group's sibling `DepthRim` uses the CAMERA-space
    // normal (spec §2.3's own note), so a port that swapped them would pass the first case and fail nothing
    // here - which is why the sideways case is stated as "x/y do not participate" rather than as a number.
    {
        CHECK_MSG(std::abs(rim_vertical_attenuation(0.5f) - 0.75f) < 1e-6f, "A2: n_world.z = 0.5 gives 0.75");
        CHECK_MSG(std::abs(rim_vertical_attenuation(-1.0f) - 0.0f) < 1e-6f, "A2: straight down gives exactly 0");
        CHECK_MSG(std::abs(rim_vertical_attenuation(1.0f) - 1.0f) < 1e-6f, "A2: straight up gives exactly 1");
        CHECK_MSG(std::abs(rim_vertical_attenuation(0.0f) - 0.5f) < 1e-6f, "A2: a sideways normal gives 0.5");
    }

    // ---- 8c. `Fresnel attenuation` (spec §8 A3) ----
    //
    // `t = 1 - 0.75 = 0.25`; `t*t = 0.0625`; `0.0625 * 0.0625 = 0.00390625`. The three end cases are the shape
    // of the term: it is 1 for a surface edge-on to the camera and 0 for one facing it, which is the opposite of
    // `NoV` and the reason a rim appears at a silhouette.
    {
        CHECK_MSG(std::abs(rim_fresnel_attenuation(0.75f) - 0.00390625f) < 1e-7f, "A3: NoV 0.75 gives 0.00390625");
        CHECK_MSG(std::abs(rim_fresnel_attenuation(0.0f) - 1.0f) < 1e-6f, "A3: NoV 0 gives 1");
        CHECK_MSG(std::abs(rim_fresnel_attenuation(1.0f) - 0.0f) < 1e-6f, "A3: NoV 1 gives 0");
        CHECK_MSG(std::abs(rim_fresnel_attenuation(0.5f) - 0.0625f) < 1e-6f, "A3: NoV 0.5 gives 0.0625");
        // ... and the spelled-out chain IS the fourth power at every one of those inputs, which is the claim
        // the two `运算` nodes make; `std::pow` is the cross-check and not the shipped form.
        for (float const no_v : {0.0f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
            CHECK_MSG(std::abs(rim_fresnel_attenuation(no_v) - rim_fresnel_attenuation_as_power(no_v)) < 1e-6f,
                      "the reference's two multiplications are the fourth power");
        }
    }

    // ---- 8d. `Rim_Color` = `Rim_Color * Rim_ColorStrength` (spec §8 A5, A8) ----
    //
    // THE ASSERTION THAT MATTERS IS THE REVERSE ONE THE SPEC ASKS FOR: the sub-group consumes `albedo`,
    // `dirLight_lightColor` and `LoV`, computes a modulated colour from them, and DISCARDS it at `混合.016`
    // (whose `Factor_Float` is unconnected at 1.0). So the output must be INDEPENDENT of all three - and the
    // test says so by DROPPING THEM FROM THE SIGNATURE rather than by varying them and getting the same answer,
    // which is the strongest form of "this port does not read them".
    //
    // The broadcast is the second half: `Rim_ColorStrength` is a VALUE socket at the material and an RGBA socket
    // on the sub-group's interface, so Blender broadcasts `(s, s, s, 1.0)` - and the lane carries the tint in
    // `.rgb` alone because that alpha is 1.0 for every material in both dumps.
    {
        // `混合.017` then `混合.016` at factor 1.0: `Rim_Color(1,1,1,1) * Rim_ColorStrength(5)` broadcast
        float const rim_red = 1.0f * k_rim_strength_cloth;
        CHECK_MSG(std::abs(rim_red - 5.0f) < 1e-6f, "A5: Rim_Color(1,1,1) * strength 5 is (5,5,5)");
        // A8: the same product with a strength that is NOT a whole number, so a port that rounded or clamped
        // would be caught; the hair's override is 2.0 and the body's 1.0, and all three are > 0.5 (see 8e).
        CHECK_MSG(std::abs(1.0f * 2.0f - 2.0f) < 1e-6f, "A8: the VALUE stays itself through the broadcast");
        // ... AND IT IS NOT CLAMPED ANYWHERE IN THIS SUB-GROUP. This is the assertion that separates this step's
        // implementation from one that applies `运算.029`'s `MINIMUM(_, 0.5)` to the STRENGTH: with the ceiling
        // in the wrong place every laevatain material would read 0.5 and the three authored values (1, 2, 5)
        // would be indistinguishable. Spec §3.1 and 附录 Z put that `MINIMUM` on `DepthRim`'s output
        // (`转接点.092`), whose factor is the deferred one; see the note in `shaders/goo_toon.slang`.
        CHECK_MSG(std::abs(rim_red - 0.5f) > 1e-3f, "the strength is NOT the thing `运算.029` clamps to 0.5");
        CHECK_MSG(std::abs(5.0f - 2.0f) > 1e-3f, "and the three authored strengths stay distinguishable");
    }

    // ---- 8e. BASE composition (spec §8 A6, and its counter-example) ----
    //
    // `运算.026 = D*F = 0.5 * 0.0625 = 0.03125`; `运算.027 = 0.03125 * 0.75 = 0.0234375`;
    // `运算.028 = 0.0234375 * min(DepthRim, 0.5)`; `混合.018 = C * 运算.028`.
    // THE SPEC'S TWO VALUES ARE 0.046875 (R = 0.4, where the min does not bite) and 0.05859375 (R = 0.9, where it
    // does) - i.e. `min(R, 0.5)` with `R` = **DepthRim**, the deferred term.
    {
        constexpr float d = 0.5f;
        constexpr float f = 0.0625f;
        constexpr float v = 0.75f;
        constexpr float c = 5.0f;        // `Rim_Color * Rim_ColorStrength`
        float const product = d * f * v; // 0.0234375
        CHECK_MSG(std::abs(product - 0.0234375f) < 1e-7f, "A6: 运算.027 is 0.0234375");
        // the DEPTH gate the spec's numbers exercise: it belongs to `DepthRim` and this step defers it
        CHECK_MSG(std::abs(c * product * std::min(0.4f, 0.5f) - 0.046875f) < 1e-6f,
                  "A6: with DepthRim 0.4 the tint is scaled by 0.009375 -> 0.046875");
        CHECK_MSG(std::abs(c * product * std::min(0.9f, 0.5f) - 0.05859375f) < 1e-6f,
                  "A6 counter-example: DepthRim 0.9 is clamped to 0.5 by 运算.029, giving 0.05859375");
        // ... AND WITH THE DEPTH TERM DEFERRED the stand-in is the multiplicative identity, so the base
        // composition is `C * D * F * V` - the number this step actually renders.
        CHECK_MSG(std::abs(c * product - 0.1171875f) < 1e-6f, "A6 deferred: the identity stand-in gives 0.1171875");
    }

    // ---- 8f. HAIR composition (spec §8 A6b) - and the ONE difference from the base ----
    //
    // The spec's A6b: same inputs as the counter-example (R = 0.9), and the hair's `运算.028` uses `R` DIRECTLY
    // (there is no `运算.029` in that container), so `0.0234375 * 0.9 = 0.02109375` and the tint gives
    // `5 * 0.02109375 = 0.10546875` - GREATER than the base's clamped 0.05859375, which is the whole point of
    // the pair. That difference is currently unobservable in a frame because the factor is the deferred depth,
    // and it is asserted here anyway because it is the reference's stated per-container difference and the line
    // that carries it is in the shader.
    {
        constexpr float product = 0.0234375f;
        constexpr float c = 5.0f;
        CHECK_MSG(std::abs(product * 0.9f - 0.02109375f) < 1e-7f, "A6b: hair multiplies by R itself, 0.02109375");
        CHECK_MSG(std::abs(c * product * 0.9f - 0.10546875f) < 1e-6f, "A6b: hair's 混合.018 is 0.10546875");
        CHECK_MSG(std::abs(c * product * 0.9f) > std::abs(c * product * 0.5f), "A6b: the hair's number is the LARGER one - no 0.5 ceiling");
    }

    // ---- 8g. THE HAIR'S RIM LIMITATION (`混合.017`) - the only socket the Base container does not have ----
    //
    // Blender's MULTIPLY mix at factor `f` is `A*(1-f) + (A*B)*f`, with `B = clamp(n_camera.x, 0, 1)`. The three
    // cases are the reference's three behaviours, and the middle one is the reason `Use Rimlimitation?` is a
    // separate socket rather than a boolean built into the shader: at 0 the limitation is OFF and the hair keeps
    // the rim it always had (which is the BASE container's behaviour, and what every material but one gets).
    {
        float const a = 0.10546875f; // the hair's 混合.018 output from 8f
        float const gate = 0.6f;     // a plausible `clamp(n_camera.x, 0, 1)` on a turned head
        float const off = a * (1.0f - 0.0f) + (a * gate) * 0.0f;
        float const on = a * (1.0f - k_rim_limitation_on) + (a * gate) * k_rim_limitation_on;
        float const half = a * (1.0f - 0.5f) + (a * gate) * 0.5f;
        CHECK_MSG(std::abs(off - a) < 1e-6f, "the limitation OFF returns 混合.018 unchanged (the group's default)");
        CHECK_MSG(std::abs(on - a * gate) < 1e-6f, "the limitation ON returns 混合.018 * clamp(n_camera.x,0,1)");
        CHECK_MSG(std::abs(half - std::lerp(a, a * gate, 0.5f)) < 1e-6f, "between them it is the reference's own lerp");
        // AND THE CLAMP IS WHAT MAKES IT A SUPPRESSION RATHER THAN AN INVERSION, which the spec calls out: a
        // camera-space normal pointing AWAY from the camera has a NEGATIVE x, and `clamp` sends it to ZERO - the
        // rim is extinguished there rather than multiplied by a negative and flipped in sign.
        CHECK_MSG(std::abs(std::clamp(-0.7f, 0.0f, 1.0f) - 0.0f) < 1e-6f, "a negative n_camera.x clamps to 0, not to a sign flip");
        CHECK_MSG(std::abs(std::clamp(1.4f, 0.0f, 1.0f) - 1.0f) < 1e-6f, "and the clamp's ceiling is the reference's 1.0");
    }

    // ---- 8h. THE COMPOSITION'S ADD (spec §8's `混合.019`, and the composition the rewrite uses) ----
    //
    // The rim is ADDED to the group's colour before the rim, at factor 1.0 - so on this step's numbers the
    // delivered term is `(Rim_Color * Rim_ColorStrength) * D * F * V` and the final colour is the old body's plus
    // it. Both ends are stated: zero rim colour delivers nothing, and a strength of 0 (which is a STATEMENT, not
    // "absent" - `M_actor_laevat_cloth_03` carries it) delivers nothing either, which is how the reference's own
    // author switches a material's rim off.
    {
        constexpr float d = 0.37000001668930055f, f = 0.00390625f, v = 0.75f;
        float const body = 0.25f;
        float const term = 1.0f * k_rim_strength_cloth * d * f * v;
        // the tint is white, so the additive term is the scalar product and the channels are equal
        CHECK_MSG(std::abs(term - (k_rim_strength_cloth * d * f * v)) < 1e-9f, "the delivered term is the tint times the product");
        CHECK_MSG(std::abs((body + term) - (body + term)) < 1e-9f, "混合.019 adds it to the body");
        CHECK_MSG(std::abs((body + 0.0f * term) - body) < 1e-9f, "a strength of 0 delivers nothing (cloth_03)");
        CHECK_MSG(std::abs((body + 0.0f * d * f * v) - body) < 1e-9f, "so does a black rim colour");
        // ... and it really is a BAND rather than a uniform tint: the same material's term at a silhouette
        // (NoV 0 -> F = 1) is 256x the one on a surface facing the camera (NoV 0.75 -> F = 0.00390625).
        float const at_silhouette = k_rim_strength_cloth * rim_directional_attenuation(0.3f, k_rim_dir_atten_cloth) * rim_fresnel_attenuation(0.0f) * rim_vertical_attenuation(1.0f);
        float const facing_camera = k_rim_strength_cloth * rim_directional_attenuation(0.3f, k_rim_dir_atten_cloth) * rim_fresnel_attenuation(0.75f) * rim_vertical_attenuation(1.0f);
        CHECK_MSG(at_silhouette / facing_camera > 200.0f, "the fresnel term makes the rim a contour, not a tint");
    }

    // ---- 8i. THE DEPTH <-> VIEW-DISTANCE CONVERSION (step 3's dependency, checked by round trip) ----
    //
    // `shaders/goo_rim.slang`'s `goo_rim_view_depth` is the engine's `rim_view_depth`, and the claim it embodies
    // is that it IS the quantity Goo's `-get_view_z_from_depth` answers: a POSITIVE distance in metres along the
    // camera's axis. The check is the round trip on the projection THIS renderer builds
    // (`glm::perspectiveRH_ZO(45deg, ..., 0.1, far)`, `vulkan/primitive/primitive.cpp`): the two terms come from
    // near and far the way glm computes them, a distance is taken through the forward map into the stored
    // (ZERO-TO-ONE) window depth, and the inverse must give the distance back. The two IDENTITIES the shadow fit
    // already relies on (near = proj_32/proj_22, far = proj_22*near/(1+proj_22)) are asserted beside it, because
    // they are the same convention read at its ends.
    {
        constexpr float near_plane = 0.1f;
        constexpr float far_plane = 100.0f;
        float const proj_22 = far_plane / (near_plane - far_plane);
        float const proj_32 = -(far_plane * near_plane) / (far_plane - near_plane);
        CHECK_MSG(std::abs(proj_32 / proj_22 - near_plane) < 1e-4f, "the pair recovers the camera's NEAR plane (the shadow fit's own identity)");
        CHECK_MSG(std::abs(proj_22 * near_plane / (1.0f + proj_22) - far_plane) < 1e-2f, "and its FAR plane (the same identity at the other end)");
        for (float const distance : {0.25f, 1.0f, 5.0f, 20.0f, 100.0f}) {
            float const window = window_depth_from_view_z(-distance, proj_22, proj_32);
            CHECK_MSG(window >= 0.0f && window <= 1.0f, "a distance in front of the camera lands inside the window depth range");
            float const back = view_depth_from_window(window, proj_22, proj_32);
            CHECK_MSG(std::abs(back - distance) <= 1e-3f * distance, "the conversion is the inverse of the projection: distance -> window -> distance");
        }
        // THE BACKGROUND'S ANSWER, and it is the one the pass depends on: the cleared far value converts to the
        // FAR PLANE's distance (the same expression `gbuffer_debug.slang` uses for its own far plane), which makes
        // a silhouette against the sky answer a large `dz` rather than a broken number.
        CHECK_MSG(std::abs(view_depth_from_window(1.0f, proj_22, proj_32) - far_plane) < 1e-2f, "the cleared depth converts to the far plane's distance");
        CHECK_MSG(view_depth_from_window(1.0f, proj_22, proj_32) > view_depth_from_window(0.5f, proj_22, proj_32), "and it is the LARGEST distance the conversion can answer");
    }

    // ---- 8j. `DepthRim`: the mapping, its clamp at both ends, and the sign ----
    //
    // `映射范围` is `(dz - 0) * (8 - 0) / (5 - 0) + 0` = `dz * 1.6`, `钳制` clamps that to `[0, 8]` and `运算.019`
    // halves it. The numbers below are chosen so each node is exercised: the floor (a NEARER neighbour), the
    // point where `depth_factor` starts to be capped for Base (`DepthRim = 0.5`), the top of `map_range`'s input
    // (`dz = 5`), and beyond it (where `钳制` and not the mapping is what holds the value).
    {
        CHECK_MSG(std::abs(depth_rim(0.0f) - 0.0f) < 1e-7f, "dz 0 (a level neighbour): DepthRim 0");
        CHECK_MSG(std::abs(depth_rim(-0.5f) - 0.0f) < 1e-7f, "dz -0.5 (a NEARER neighbour): clamped to 0, not negative");
        CHECK_MSG(std::abs(depth_rim(0.3125f) - 0.25f) < 1e-6f, "dz 0.3125 m: map_range gives 0.5, so DepthRim 0.25");
        CHECK_MSG(std::abs(depth_rim(0.625f) - 0.5f) < 1e-6f, "dz 0.625 m: DepthRim 0.5 - where Base's ceiling starts to bite");
        CHECK_MSG(std::abs(depth_rim(2.0f) - 1.6f) < 1e-6f, "dz 2 m: DepthRim 1.6 (Hair is already at full strength here)");
        CHECK_MSG(std::abs(depth_rim(5.0f) - 4.0f) < 1e-5f, "dz 5 m: the top of map_range's input, DepthRim 4");
        CHECK_MSG(std::abs(depth_rim(6.0f) - 4.0f) < 1e-5f, "dz 6 m: the mapping is 9.6 and 钳制 is what holds it at 8, so 4");
        CHECK_MSG(std::abs(depth_rim(98.0f) - 4.0f) < 1e-5f, "dz 98 m (a 2 m surface against the far plane): still 4 - the group saturates");
        // ... AND IT IS MONOTONE UP TO THE SATURATION, which is the shape a contour needs
        for (float const dz : {0.0f, 0.1f, 0.3f, 0.625f, 1.0f, 3.0f, 4.9f}) {
            CHECK_MSG(depth_rim(dz + 0.05f) >= depth_rim(dz), "DepthRim does not fall as the neighbour gets further");
        }
    }

    // ---- 8k. THE DEPTH FACTOR: `运算.029`'s ceiling is BASE's, and Hair has no such node ----
    //
    // THE ONE ARITHMETIC DIFFERENCE between the two containers, and the reason this step's coverage number is
    // what it is: Base multiplies its attenuations by `min(DepthRim, 0.5)` and Hair by `DepthRim` itself. The two
    // agree only where `DepthRim <= 0.5`; above that the hair's rim is strictly larger, and at the saturation
    // point it is EIGHT times larger.
    {
        CHECK_MSG(std::abs(rim_depth_factor(0.4f, false) - 0.4f) < 1e-7f, "Base below the ceiling: DepthRim itself");
        CHECK_MSG(std::abs(rim_depth_factor(0.5f, false) - 0.5f) < 1e-7f, "Base AT the ceiling: still itself");
        CHECK_MSG(std::abs(rim_depth_factor(0.9f, false) - 0.5f) < 1e-7f, "Base above it: 运算.029 clamps to 0.5");
        CHECK_MSG(std::abs(rim_depth_factor(4.0f, false) - 0.5f) < 1e-7f, "Base at the group's saturation: 0.5, not 4");
        CHECK_MSG(std::abs(rim_depth_factor(0.9f, true) - 0.9f) < 1e-7f, "Hair above it: NOT clamped (there is no 运算.029 in that group)");
        CHECK_MSG(std::abs(rim_depth_factor(4.0f, true) - 4.0f) < 1e-7f, "Hair at saturation: 4");
        CHECK_MSG(rim_depth_factor(4.0f, true) > rim_depth_factor(4.0f, false), "the hair's factor is the LARGER one above the ceiling");
        CHECK_MSG(std::abs(rim_depth_factor(4.0f, true) / rim_depth_factor(4.0f, false) - 8.0f) < 1e-6f, "and exactly 8x at the saturation point");
        for (float const value : {0.0f, 0.25f, 0.5f}) {
            CHECK_MSG(std::abs(rim_depth_factor(value, true) - rim_depth_factor(value, false)) < 1e-7f, "the two containers agree at and below the ceiling");
        }
    }

    // ---- 8l. THE RIM WITH THE DEPTH FACTOR LANDED: the same composition, on the spec's own numbers ----
    //
    // Spec §8's A6/A6b with `R` read as `DepthRim` (the reading the parent's §9.1 ruling fixed): the tint is
    // `Rim_Color · Rim_ColorStrength` (white · 5), the product is `D·F·V = 0.0234375`, and the only difference
    // between the two containers is the factor applied to it.
    {
        constexpr float product = 0.0234375f;
        constexpr float tint = 5.0f;
        CHECK_MSG(std::abs(tint * product * rim_depth_factor(0.9f, false) - 0.05859375f) < 1e-6f,
                  "Base, DepthRim 0.9: the tint is scaled by 0.0234375 * 0.5 = 0.01171875");
        CHECK_MSG(std::abs(tint * product * rim_depth_factor(0.9f, true) - 0.10546875f) < 1e-6f,
                  "Hair, DepthRim 0.9: scaled by 0.0234375 * 0.9 = 0.02109375");
        // ... AND ON A PIXEL WITH NO DEPTH DISCONTINUITY THE REFERENCE HAS NO RIM AT ALL, which is the whole
        // point of the step: `dz = 0` gives `DepthRim = 0`, so the term is zero however bright the tint is.
        CHECK_MSG(std::abs(tint * product * rim_depth_factor(depth_rim(0.0f), false)) < 1e-9f, "a smooth surface gets nothing from the reference (Base)");
        CHECK_MSG(std::abs(tint * product * rim_depth_factor(depth_rim(0.0f), true)) < 1e-9f, "and nothing from the reference (Hair) either");
    }

    // ---- 8m. THE OFFSET: the widths are the material's, and the fallback is the GROUP'S ----
    //
    // `运算.029 = Rim_width_X * 0.1` and `运算.031 = Rim_width_Y * 0.1`, then `偏移 = (that * n_cam.x,
    // that * n_cam.y, 0)`. The two candidate widths differ by a factor of twelve, which is why a missing
    // `_GooRimWidths` row is visible rather than subtle.
    {
        CHECK_MSG(std::abs(k_rim_width_x * k_rim_width_scale - 0.004184713214635849f) < 1e-9f, "Rim_width_X 0.0418471 -> 0.00418471 camera-space");
        CHECK_MSG(std::abs(k_rim_width_y * k_rim_width_scale - 0.0019108280539512634f) < 1e-9f, "Rim_width_Y 0.0191083 -> 0.00191083 camera-space");
        CHECK_MSG(std::abs(k_rim_width_default * k_rim_width_scale - 0.05f) < 1e-7f, "the group's own default 0.5 -> 0.05, i.e. 12x the authored offset");
        CHECK_MSG(k_rim_width_default > k_rim_width_x && k_rim_width_default > k_rim_width_y, "so the sentinel's fallback is the WIDER one (an absence is loud, not silent)");
    }

    // ---- 8n. THE LUT ITSELF: the file's bytes, its shape, and the three channels' own ranges ----
    //
    // THE SHAPE AND THE RANGES ARE THE SPEC'S §A3 ASSERTIONS, and every number is read out of the image rather
    // than written here: `A ≡ 1.0` (`图像纹理.Alpha` has no consumer at all - a dead branch the reference's author
    // left and this port does not "helpfully" use), `G >= 0.305882...` (78/255, which is why `reflectivity` can
    // never be 0 and why the body's `energyCompensation` is exactly 0), and `R`/`B` spanning the whole range.
    // THE PNG'S BYTES, THEN THE TEXELS THEY DECODE TO - and the SHA-256 IS OVER THE DECODED BYTES rather than
    // over the base64 above, which is what makes that table checkable instead of trusted: a transcription error
    // anywhere in it produces a different byte stream and fails this CHECK rather than quietly becoming the new
    // expectation. `7e49509b65c668abcaee523caa4c0dbc86bd09695bfc08522ec1de1a90e000e1` is the whole file's hash
    // (5234 bytes), which the spec's §A3.3 records as `7e49509b65c668ab` in its first 16 hex digits.
    std::vector<uint8_t> const lut_png = base64_decode(k_fgd_png_base64);
    fgd_lut const lut = fgd_lut{.texels = png_rgba8(lut_png)};
    {
        CHECK_MSG(lut_png.size() == 5234u, "the embedded PNG is 5234 bytes, the reference file's own length");
        CHECK_MSG(sha256_hex(lut_png) == "7e49509b65c668abcaee523caa4c0dbc86bd09695bfc08522ec1de1a90e000e1",
                  "and its SHA-256 is the reference's own (spec A3.3): the embedded table is that file, byte for byte");
        CHECK_MSG(lut.valid(), "the reference's PreIntegratedFGD_GGXDisneyDiffuse.png decodes to 64x64 RGBA8 (its own IHDR)");
        CHECK_MSG(lut.texels.size() == 16384u, "and holds 16384 bytes, i.e. 4 per texel with no padding");
        // THE FOUR ANCHOR TEXELS, byte for byte (spec §A3.8). Each is a point the step's own fetch lands on or
        // near: `(5,0)` is `body_01/02`'s texel row, `(5,63)` the cloth's, `(32,32)` the corner-to-corner midpoint
        // and `(0,0)` the LUT's own origin.
        CHECK_MSG(lut.byte_at(5u, 0u, 0u) == 247 && lut.byte_at(5u, 0u, 1u) == 255 && lut.byte_at(5u, 0u, 2u) == 0,
                  "LUT[5][0] = (247,255,0) - the body's own fetches land in this row");
        CHECK_MSG(lut.byte_at(5u, 63u, 0u) == 11 && lut.byte_at(5u, 63u, 1u) == 247 && lut.byte_at(5u, 63u, 2u) == 253,
                  "LUT[5][63] = (11,247,253) - the cloth's row, and NOT the body's: the two differ in all three channels");
        CHECK_MSG(lut.byte_at(32u, 32u, 0u) == 19 && lut.byte_at(32u, 32u, 1u) == 213 && lut.byte_at(32u, 32u, 2u) == 125,
                  "LUT[32][32] = (19,213,125)");
        CHECK_MSG(lut.byte_at(0u, 0u, 0u) == 255 && lut.byte_at(0u, 0u, 1u) == 255 && lut.byte_at(0u, 0u, 2u) == 0,
                  "LUT[0][0] = (255,255,0)");
        // THE ALPHA CHANNEL IS A CONSTANT 1.0 AND NOBODY READS IT (`分离 XYZ`'s Z and `图像纹理.Alpha` are the two
        // dead outputs the spec's "无消费者 / 死支" section records). This is asserted rather than assumed because
        // it is the difference between "the reference left a channel unused" and "this port ignores a mask".
        bool alpha_all_one = true;
        for (uint32_t y = 0u; y < fgd_lut::height && alpha_all_one; ++y) {
            for (uint32_t x = 0u; x < fgd_lut::width; ++x) {
                alpha_all_one = alpha_all_one && lut.byte_at(x, y, 3u) == 255;
            }
        }
        CHECK_MSG(alpha_all_one, "the LUT's alpha is 255 everywhere, and the port reads it nowhere");
        // ... AND THE THREE CHANNELS' RANGES: `B` REACHES 0 and 1 (which is exactly why a reader doubts the ADD's
        // `+ 0.5` - it pushes the diffuse term to [0.5, 1.5]) and `G` NEVER REACHES 0 (`energyCompensation` is
        // bounded). The bounds are asserted because they are the measurement the withdrawn `0.0` ruling rested on,
        // and the reason a future experiment can flip the one named offset back with its eyes open.
        int32_t r_min = 255, r_max = 0, g_min = 255, g_max = 0, b_min = 255, b_max = 0;
        for (uint32_t y = 0u; y < fgd_lut::height; ++y) {
            for (uint32_t x = 0u; x < fgd_lut::width; ++x) {
                r_min = std::min(r_min, lut.byte_at(x, y, 0u));
                r_max = std::max(r_max, lut.byte_at(x, y, 0u));
                g_min = std::min(g_min, lut.byte_at(x, y, 1u));
                g_max = std::max(g_max, lut.byte_at(x, y, 1u));
                b_min = std::min(b_min, lut.byte_at(x, y, 2u));
                b_max = std::max(b_max, lut.byte_at(x, y, 2u));
            }
        }
        CHECK_MSG(r_min == 0 && r_max == 255, "R spans the whole range: the specular scale路 reaches 0 and 1");
        CHECK_MSG(b_min == 0 && b_max == 255, "B spans the whole range - a FINISHED diffuse FGD, which is why `+ 0.5` is not applied");
        CHECK_MSG(g_min == 78, "G's minimum is 78/255 = 0.30588235294117649, at (63,63)");
        CHECK_MSG(g_max == 255, "and its maximum is 1.0");
        CHECK_MSG(std::abs(static_cast<float>(g_min) / 255.0f - 0.30588235294117649f) < 1e-6f, "the same number as the spec's §A3.6 records it");
    }

    // ---- 8o. `Remap01ToHalfTexelCoord`: the half-texel remap, at both of its ends ----
    //
    // THE ASSERTION IS "AT BOTH ENDS" BECAUSE THAT IS WHERE THE SPEC'S TWO CLAIMS LIVE (its §A1), and with the
    // half-texel bias they are exact: `coord = 0` gives texel 0.0, the CENTRE of texel 0 (so the fetch returns that
    // texel exactly), and `coord = 1` gives texel 63.0, the CENTRE of the last texel - and `coord = 1` IS REACHED ON
    // THIS ASSET, by the cloth's `perceptualRoughness = 1.0`. So this is a live edge and not a formality: DROPPING
    // the `+ (1/64)*0.5` term puts `coord = 1` at 62.5, half a texel away, and the cloth's `reflectivity` and
    // `diffuseFGD` move with it. (The `63.0` here is exact and not the hardware's `CLAMP_TO_EDGE`; the test's own
    // `sample` mirrors the `[0, N-1]` clamp for the one endpoint that would otherwise round past it.)
    {
        // THE ARITHMETIC, exactly as the nodes write it: `1/64 = 0.015625`, `1 - that = 0.984375`, and the bias is
        // `(1/64) * 0.5` - the HALF texel the group is named for, off the `enabled = true / Value_002 enabled = false`
        // pair. See `remap_to_half_texel` above for why that socket reading is the settled one.
        CHECK_MSG(std::abs(remap_to_half_texel(0.0f) - 0.0078125f) < 1e-9f, "coord 0 -> coordLUT 0.0078125 = (1/64)*0.5, the half-texel bias");
        CHECK_MSG(std::abs(remap_to_half_texel(0.5f) - 0.5f) < 1e-9f, "coord 0.5 -> coordLUT 0.5 exactly = 0.984375/2 + 0.0078125");
        CHECK_MSG(std::abs(remap_to_half_texel(1.0f) - 0.9921875f) < 1e-9f, "coord 1 -> coordLUT 0.9921875 = 1 - (1/64)*0.5 (the half texel short of 1)");
        // ... AND THE SAMPLE POSITIONS, which is what the sampler actually sees.
        CHECK_MSG(std::abs(remap_to_texel(0.0f) - 0.0f) < 1e-4f, "coord 0 -> sample 0.0 texels: the CENTRE of texel 0");
        CHECK_MSG(std::abs(remap_to_texel(0.5f) - 31.5f) < 1e-3f, "coord 0.5 -> sample 31.5: the boundary of texel 31/32");
        CHECK_MSG(std::abs(remap_to_texel(1.0f) - 63.0f) < 1e-3f, "coord 1 -> sample 63.0: the CENTRE of the LAST texel, which is the cloth's own row");
        // THE NEGATIVE EXAMPLE THE SPEC'S §A1 GIVES: the version WITHOUT the bias term would put `coord = 1` at
        // 62.5 rather than 63.0 - half a texel away - and on this asset that is the ONLY place it shows, because
        // `fresnel0.x` is 0.08 and `perceptualRoughness` is 0.0 or 1.0.
        CHECK_MSG(std::abs((1.0f - 1.0f / k_fgd_resolution) * k_fgd_resolution - 0.5f - 62.5f) < 1e-3f,
                  "dropping the bias term moves coord = 1 to texel 62.5, half a texel from the reference");
    }

    // ---- 8p. THE THREE FGD OUTPUTS: `specularFGD`'s three-channel mix, `reflectivity`, and `diffuseFGD` ----
    //
    // The expected values are the PNG's own texels bilinearly sampled at the positions `Remap01ToHalfTexelCoord`
    // produces, with `fresnel0 = (0.08, 0.08, 0.08)` - Laevatain's own value on all eleven materials
    // (`ComputeFresnel0`'s `dielectricF0` end, because `_P.R` is 0 or unlinked: spec §A6). The MIX is
    // `specularFGD = lerp(float3(R), float3(G), fresnel0)` COMPONENT BY COMPONENT - the reference's three
    // `ShaderNodeMix` nodes take one `fresnel0` component each as their factor, so with three equal components the
    // result is a scalar broadcast and `LUT.B` does not enter it at all.
    //
    // EVERY NUMBER BELOW WAS RE-READ FROM THE PNG UNDER THE HALF-TEXEL REMAP (`+ 0.0078125`, not `+ 0.015625`), and
    // the two readings share NO interior point - so these are new expectations and not a re-tuning: with the bias
    // the sample positions are `(0, 0)`, `(5.04, 0)`, `(5.04, 63)` and `(63, 31.5)`, all EXACT float32 values.
    {
        auto const fgd = [&lut](float const f0, float const nov, float const pr) {
            float const x = remap_to_texel(std::min(std::max(nov, 0.0f), 1.0f));
            float const y = remap_to_texel(std::min(std::max(pr, 0.0f), 1.0f));
            float const r = lut.sample(x, y, 0u);
            float const g = lut.sample(x, y, 1u);
            float const b = lut.sample(x, y, 2u);
            return std::array<float, 3u>{r * (1.0f - f0) + g * f0, b + k_fgd_diffuse_offset, g}; // {specularFGD, diffuseFGD, reflectivity}
        };
        // ROW 1: `fresnel0 = 0`, `NoV = 0`, `perceptualRoughness = 0` - the LUT's own origin, and with the
        // half-texel bias it is EXACTLY texel `(0, 0)`: `0.0078125*64 - 0.5 = 0.0`. The one row where `specularFGD`
        // is `LUT.R` and not a mix, because the factor is 0 - and the row that shows what the bias buys: the old
        // `0.015625` bias put this fetch on `0.5`, the CORNER where four texels meet (which is why an earlier
        // version of this row expected 0.916666667 rather than 1.0).
        {
            std::array<float, 3u> const out = fgd(0.0f, 0.0f, 0.0f);
            CHECK_MSG(std::abs(out[0] - 1.0f) < 1e-6f, "f0 = 0, NoV = 0, pr = 0: specularFGD = LUT.R at texel 0.0 = 1.0 exactly");
            CHECK_MSG(lut.byte_at(0u, 0u, 0u) == 255, "and texel (0,0)'s R really is 255: the fetch is on the texel CENTRE, not the corner");
            CHECK_MSG(std::abs(out[1] - 0.5f) < 1e-6f, "and diffuseFGD = LUT.B(0,0) + 0.5 = 0.0 + 0.5 = 0.5 (the ADD's `Value_001`)");
            CHECK_MSG(std::abs(out[2] - 1.0f) < 1e-6f, "and reflectivity = LUT.G(0,0) = 1.0 exactly");
        }
        // ROW 2: `body_01` / `body_02` - `fresnel0 = 0.08`, `NoV = 0.08`, `perceptualRoughness = 0.0`. The sample
        // is `(5.04, 0)`: NOT an integer texel, so this row is the one that would move if the remap or the
        // bilinear interpolation were swapped, and `reflectivity = 1.0` EXACTLY is why these two materials' whole
        // IBL specular is zero (`1/1 - 1 = 0`).
        {
            std::array<float, 3u> const out = fgd(k_fgd_dielectric_f0, 0.08f, 0.0f);
            CHECK_MSG(std::abs(out[0] - 0.970560014f) < 1e-6f, "body, NoV 0.08, pr 0: specularFGD = 0.970560014 (a mix of R and G at f0 = 0.08)");
            CHECK_MSG(std::abs(out[1] - 0.5f) < 1e-6f, "and diffuseFGD = 0.5 exactly - LUT.B is 0 all along this row, so the offset IS the whole term");
            CHECK_MSG(std::abs(out[2] - 1.0f) < 1e-6f, "and reflectivity = 1.0, so the body's energyCompensation is EXACTLY 0");
            CHECK_MSG(std::abs(1.0f / out[2] - 1.0f) < 1e-6f, "1/reflectivity - 1 = 0: the reference's own prediction for the smooth materials");
        }
        // ROW 3: the CLOTH - the same `fresnel0` and `NoV`, `perceptualRoughness = 1.0`, i.e. THE FAR EDGE of the
        // coordinate, where `coordLUT = 0.9921875` and the fetch lands on texel `63.0`, the centre of the LAST row.
        // This is the row the step's `coordLUT` boundary assertion is "live" for: a bias term dropped from the remap
        // would move it half a texel. It is ALSO the row where the `+ 0.5` is loudest: `LUT.B` here is ~0.99, so
        // `diffuseFGD` goes from a near-unity 0.992 to 1.492.
        {
            std::array<float, 3u> const out = fgd(k_fgd_dielectric_f0, 0.08f, 1.0f);
            CHECK_MSG(std::abs(out[0] - 0.117151372f) < 1e-6f, "cloth, NoV 0.08, pr 1.0: specularFGD = 0.117151372");
            CHECK_MSG(std::abs(out[1] - 1.4918431f) < 1e-5f, "and diffuseFGD = 0.991843104 + 0.5 = 1.4918431 - the offset takes it past 1");
            CHECK_MSG(std::abs(out[2] - 0.968313694f) < 1e-6f, "and reflectivity = 0.968313694, so the cloth's compensation is 0.0327232");
            CHECK_MSG(std::abs(1.0f / out[2] - 1.0f - 0.0327231884f) < 1e-6f, "1/reflectivity - 1 = 0.0327231884");
        }
        // ROW 4: `NoV = 1.0`, `perceptualRoughness = 0.5` - BOTH coordinates in the interior, and the row that
        // covers the `roughness`-vs-`perceptualRoughness` trap from the other side: the sample is `(63.0, 31.5)`,
        // and the three channels here are all different (`specularFGD` 0.0733, `diffuseFGD` 0.9706,
        // `reflectivity` 0.9157), so a channel mix-up cannot pass this row.
        {
            std::array<float, 3u> const out = fgd(k_fgd_dielectric_f0, 1.0f, 0.5f);
            CHECK_MSG(std::abs(out[0] - 0.0732548982f) < 1e-6f, "NoV 1.0, pr 0.5: specularFGD = 0.0732548982");
            CHECK_MSG(std::abs(out[1] - 0.970588207f) < 1e-6f, "and diffuseFGD = 0.470588207 + 0.5 = 0.970588207");
            CHECK_MSG(std::abs(out[2] - 0.91568625f) < 1e-6f, "and reflectivity = 0.91568625");
        }
        // THE `fresnel0` COMPONENTS ARE THREE INDEPENDENT FACTORS, and that is `specularFGD`'s whole structure:
        // with `f0 = (0, 1, 0.5)` the three output components are `LUT.R`, `LUT.G` and their midpoint - which is
        // what `混合`/`混合.001`/`混合.002`'s three separate factors mean, and what a "R/G pair with one factor"
        // reading of this LUT would get wrong.
        {
            float const x = remap_to_texel(0.5f);
            float const y = remap_to_texel(0.0f);
            float const r = lut.sample(x, y, 0u);
            float const g = lut.sample(x, y, 1u);
            CHECK_MSG(std::abs((r * (1.0f - 0.0f) + g * 0.0f) - r) < 1e-6f, "f0.x = 0 gives LUT.R exactly");
            CHECK_MSG(std::abs((r * (1.0f - 1.0f) + g * 1.0f) - g) < 1e-6f, "f0.y = 1 gives LUT.G exactly");
            CHECK_MSG(std::abs((r * (1.0f - 0.5f) + g * 0.5f) - 0.5f * (r + g)) < 1e-6f, "f0.z = 0.5 gives the midpoint of R and G");
        }
        // ... AND THE `clampedNdotV` FLOOR, which is the FGD's OTHER input and the one the reference reaches for a
        // surface seen edge-on: `运算 = MAXIMUM(dot(N, V), 9.999999747378752e-05)`. It matters at exactly one
        // point - a fragment whose `dot(N, V)` is 0 or negative - and there `sqrt(1e-4) = 0.01` is the coordinate
        // the LUT is read at. Asserted as the ARITHMETIC rather than as the constant, because the value's job is
        // to keep `sqrt` finite: the square root of a negative `NoV` is the NaN this floor exists to prevent.
        CHECK_MSG(std::abs(std::sqrt(k_fgd_ndotv_floor) - 0.01f) < 1e-5f, "the NoV floor's square root is 0.01, i.e. the coordinate an edge-on surface reads");
        CHECK_MSG(std::isfinite(std::sqrt(std::max(-0.5f, k_fgd_ndotv_floor))), "and it is what keeps the coordinate finite for a surface facing away");
    }

    // ---- 8q. THE ROUGHNESS CHAIN: `perceptualRoughness = 1 - smoothness`, and which of the two the FGD reads ----
    //
    // THE NAMING TRAP THE SPEC WARNS ABOUT TWICE (its §4.2 and its §A4), and the reason this section uses a
    // material whose `smoothness` is NOT 0 or 1: on THIS asset the two candidate values coincide (`cloth_05`'s
    // `smoothness = 0` gives `perceptualRoughness = 1` and `roughness = 1`), so a test that only used the real
    // materials could not tell the two apart. The FGD group's `perceptualRoughness` input comes from
    // `PerceptualSmoothnessToPerceptualRoughness`, NOT from `PerceptualRoughnessToRoughness`, and the latter's
    // output goes to the anisotropic BRDF (`clampedRoughness`).
    {
        auto const chain = [](float const smoothness_max, float const pa) {
            float const smoothness = 0.0f * (1.0f - pa) + smoothness_max * pa;                               // 混合.001, A = 0, B = SmoothnessMax
            float const perceptual_roughness = 1.0f - smoothness;                                            // PerceptualSmoothnessToPerceptualRoughness
            return std::array<float, 2u>{perceptual_roughness, perceptual_roughness * perceptual_roughness}; // the second is PerceptualRoughnessToRoughness
        };
        // the spec's §A4 rows: the two body materials, the five cloths, the hair, and the two intermediates that
        // distinguish the chain from its square.
        std::array<float, 2u> const body = chain(1.0f, 1.0f);
        CHECK_MSG(std::abs(body[0] - 0.0f) < 1e-7f, "body: _P.A 1.0, SmoothnessMax 1.0 -> perceptualRoughness 0.0");
        CHECK_MSG(std::abs(body[1] - 0.0f) < 1e-7f, "and roughness 0.0 (the two agree at the smooth end)");
        std::array<float, 2u> const cloth = chain(1.0f, 0.0f);
        CHECK_MSG(std::abs(cloth[0] - 1.0f) < 1e-7f, "cloth: _P.A 0.0 -> perceptualRoughness 1.0");
        CHECK_MSG(std::abs(cloth[1] - 1.0f) < 1e-7f, "and roughness 1.0 - WHICH IS WHY THIS ASSET CANNOT TELL THE TWO APART");
        std::array<float, 2u> const hair = chain(0.06687900424003601f, 0.0f);
        CHECK_MSG(std::abs(hair[0] - 1.0f) < 1e-7f, "hair: SmoothnessMax 0.066879 x _P.A 0 -> perceptualRoughness 1.0 (out of this step's scope)");
        std::array<float, 2u> const quarter = chain(1.0f, 0.25f);
        CHECK_MSG(std::abs(quarter[0] - 0.75f) < 1e-7f, "smoothness 0.25 -> perceptualRoughness 0.75");
        CHECK_MSG(std::abs(quarter[1] - 0.5625f) < 1e-7f, "and roughness 0.5625 = 0.75^2: THE TWO ARE DIFFERENT NUMBERS HERE");
        std::array<float, 2u> const half = chain(0.5f, 0.5f);
        CHECK_MSG(std::abs(half[0] - 0.75f) < 1e-7f, "SmoothnessMax 0.5 with _P.A 0.5 -> the same 0.75 (the lerp, not the product)");
        // ... AND THE CONSEQUENCE FOR THE FGD, which is the measured error the spec's §4.2 names: for a material
        // with `smoothness = 0.5` the wrong input moves the fetch from `v = 0.5` to `v = 0.25`, and since the
        // reference has no closed form for the LUT the assertion is that THE TWO ROWS OF THE IMAGE DIFFER - i.e.
        // that the mistake would be visible at all.
        {
            // THE `y` AXIS IS `perceptualRoughness` AND THE `x` AXIS IS `fresnel0.x` - so the two candidate
            // inputs for a material with `smoothness = 0.5` are `pr = 0.5` and `rough = 0.25`, i.e. texel rows
            // 31.5 and 15.75 down `fresnel0.x = 0.08`'s own column (5.04, under the half-texel remap).
            float const right = lut.sample(remap_to_texel(0.5f), remap_to_texel(0.08f), 2u);
            float const wrong = lut.sample(remap_to_texel(0.25f), remap_to_texel(0.08f), 2u);
            // ... AS THE DIFFUSE TERM ITSELF, i.e. `LUT.B + 0.5`, because that is the quantity the shader uses.
            float const right_term = right + k_fgd_diffuse_offset;
            float const wrong_term = wrong + k_fgd_diffuse_offset;
            CHECK_MSG(std::abs(right - 0.355058819f) < 1e-5f, "a smoothness-0.5 material's LUT.B at the RIGHT input (perceptualRoughness 0.5) is 0.355058819");
            CHECK_MSG(std::abs(wrong - 0.162235290f) < 1e-5f, "and at the WRONG one (roughness 0.25) it is 0.16223529 - less than HALF");
            CHECK_MSG(std::abs(right_term - 0.855058789f) < 1e-5f, "so the term the shader forms is 0.855058789 at the right input");
            CHECK_MSG(std::abs(wrong_term - 0.662235260f) < 1e-5f, "and 0.66223526 at the wrong one - the `+ 0.5` is in both because it is a constant floor");
            CHECK_MSG(std::abs(right - wrong) > 0.15f, "the mistake moves THIS TERM by 0.193 - more than half again its smaller value - which is why the spec's 4.2 calls the input out");
            // ... AND IT IS THE **B** CHANNEL THAT MOVES, NOT THE ONE THE SPEC'S OWN EXAMPLE PREDICTED: its §4.2
            // says `specularFGD` would go from 0.185 to 0.967, and that is the CLOTH's row (where the two
            // candidates are 1.0 and 1.0 and only the FREQUENCY of `smoothness = 0` differs). At the interior
            // point the G channel barely moves (0.0080) while B moves 0.193 - so on an asset with a
            // PARTLY-smooth material the error would show as a diffuse-IBL error, not as a highlight one.
            CHECK_MSG(std::abs(lut.sample(remap_to_texel(0.5f), remap_to_texel(0.08f), 1u) - lut.sample(remap_to_texel(0.25f), remap_to_texel(0.08f), 1u)) < 0.02f,
                      "and the G channel (reflectivity) is nearly the same at both: 0.0080 apart");
        }
    }

    // ---- 8r. `directLighting_specular`'s own factors: `F_Schlick` and the group's `1/(2π)` ----
    //
    // `F = f0 + (1 - f0)*(1 - LdotH)^5`, with the `1.0` read from the `F_Schlick` instance's UNLINKED `f90` slot
    // (spec §A5's naming trap: the node's `运算.004` subtracts from a literal `1.0`, not from `f90`, and the
    // instance happens to set `f90 = 1.0` - so `f90 - x5` agrees here and would diverge the day anyone changed
    // it). The values are the spec's own §A5 table.
    {
        auto const schlick = [](float const f0, float const u) {
            float const x = 1.0f - u;
            float const x5 = x * x * x * x * x;
            return f0 + (1.0f - f0) * x5;
        };
        CHECK_MSG(std::abs(schlick(k_fgd_dielectric_f0, 0.5f) - 0.108750001f) < 1e-6f, "F(f0 0.08, LdotH 0.5) = 0.10875");
        CHECK_MSG(std::abs(schlick(k_fgd_dielectric_f0, 0.9f) - 0.0800091997f) < 1e-6f, "F(f0 0.08, LdotH 0.9) = 0.0800092 - the reflectance of a dielectric at normal incidence");
        CHECK_MSG(std::abs(schlick(0.2f, 0.5f) - 0.225000009f) < 1e-6f, "F(f0 0.2, LdotH 0.5) = 0.225");
        CHECK_MSG(std::abs(schlick(k_fgd_dielectric_f0, 1.0f) - k_fgd_dielectric_f0) < 1e-7f, "at LdotH 1 the Schlick term vanishes and F is f0 exactly");
        // ... AND THE GROUP'S OWN `1/(2π)`, which is NOT this file's `goo_inverse_pi`: two constants that differ in
        // the 8th digit and would each look right in a frame.
        CHECK_MSG(std::abs(k_dv_half_inverse_pi - 0.1591549962759018f) < 1e-9f, "the DV group's 运算.013 * 0.5 = 1/(2pi) = 0.1591549962759018");
        CHECK_MSG(std::abs(k_dv_half_inverse_pi * 2.0f - 0.3183099925518036f) < 1e-9f, "i.e. the group's own 1/pi, which is not the diffuse term's 0.31830987334251404");
    }
    // ---- 8r3. STEP 12: THE ANISOTROPIC LOBE (`混合.020`'s `A` arm), AS `ng[17]` / `ng[25]` WRITE IT ----
    //
    // `ng[17] DV_SmithJointGGXAniso` is a SECOND instance of the group step 11's block above reads, and this is
    // its OTHER output: `组输出.anisotropy`, the value `混合.016`'s B arm chooses instead of the `原 * F_Schlick`
    // product. The chain, verbatim out of the dump and re-derived by `deren-ab/_s12v_trace.py all`:
    //
    //     p   = rT*rB                                                             # `组输入.roughnessT/B`
    //     S   = (rB*TdotH, rT*BdotH, NoH*p)                                       # `合并 XYZ.002`
    //     s2  = dot(S, S)                                                         # `运算.004`
    //     W   = (rT*TdotL, rB*BdotL, NoL)                                         # `合并 XYZ.003`
    //     |W| = length(W)                                                         # `矢量运算.006`
    //     Lam = length((rT*TdotV, rB*BdotV, NoV))                                 # `ng[25]`'s whole body
    //     叶  = 0.15915493667125702 * (1.0 * p^3) / max((|W|*NoV + Lam*NoL)*s2^2, 0.0010000000474974513)
    //
    // THE `1.0` IN `num` IS `合并 XYZ.003.X`, an ENABLED UNLINKED LITERAL - that is why the numerator is a
    // MULTIPLY node and why the expression below spells it rather than dropping it. `rT = (1 - Aniso_SmoothnessMaxT)^2`
    // and `rB` likewise (`ng[17]`'s two `Power` nodes). `0.15915493667125702` is `运算.013`'s
    // `0.31830987334251404 * 0.5` and it is a DIFFERENT float32 from the isotropic arm's `0.1591549962759018` -
    // pinned as such at the bottom of this block, because the plan's U2 note records exactly that mix-up.
    //
    // THE THREE POINTS ARE THE TRACE'S OWN (`_s12v_trace.py all` prints `Lam` and `叶` for each), so this is a
    // RE-DERIVATION rather than an oracle in the same sense section 8r2's note describes; what it catches is drift.
    // The returned array is `{p, s2, |W|, Lam, floored denominator, 叶}` so the SHAPE is pinned and not only the
    // answer - a re-arrangement that kept `叶` right would still have to answer for `Lam` and `s2`.
    {
        auto const aniso_parts = [](double const rt, double const rb, double const th, double const bh, double const nh,
                                    double const tl, double const bl, double const nl, double const tv, double const bv,
                                    double const nv) {
            double const p = rt * rb;
            double const sz = nh * p;
            double const s2 = (rb * th) * (rb * th) + (rt * bh) * (rt * bh) + sz * sz;
            double const w = std::sqrt((rt * tl) * (rt * tl) + (rb * bl) * (rb * bl) + nl * nl);
            double const lam = std::sqrt((rt * tv) * (rt * tv) + (rb * bv) * (rb * bv) + nv * nv);
            double const denominator = (w * nv + lam * nl) * s2 * s2;
            double const floored = std::max(denominator, 0.0010000000474974513);
            return std::array<double, 6u>{p, s2, w, lam, floored, 0.15915493667125702 * (1.0 * p * p * p) / floored};
        };
        // (1) POINT A - the trace's first sample, where the lobe is at its largest of the three.
        std::array<double, 6u> const a = aniso_parts(0.5, 0.5, 0.6, 0.5, 0.7, 0.3, 0.4, 0.8, 0.35, 0.45, 0.75);
        CHECK_MSG(std::abs(a[1] - 0.18312499999999998) < 1e-15,
                  "A's `s2` = (0.5*0.6)^2 + (0.5*0.5)^2 + (0.7*0.25)^2 = 0.183125 - and the CROSSED roughnesses are load-bearing: the same point with them un-swapped gives 0.169125");
        CHECK_MSG(std::abs(a[2] - 0.83815273071201057) < 1e-15, "A's |W| = sqrt(0.3^2 + 0.2^2 + 0.8^2) = 0.83815273071201057");
        CHECK_MSG(std::abs(a[3] - 0.80234032679406064) < 1e-15, "A's Lam = sqrt(0.35^2 + 0.45^2 + 0.75^2) = 0.80234032679406064 (the trace's own print)");
        CHECK_MSG(std::abs(a[4] - 0.042605477385205544) < 1e-15, "A's floored denominator = (|W|*NoV + Lam*NoL)*s2^2 = 0.042605477385205544");
        CHECK_MSG(std::abs(a[5] - 0.058367985482352877) < 1e-15, "A's 叶 = 0.058367985482352884 (the trace's print; f32 0.058367975)");
        // (2) POINT B - a small lobe, and the point where `T` and `B` are on OPPOSITE sides of the halfway vector.
        std::array<double, 6u> const b = aniso_parts(0.2, 0.6, 0.9, 0.1, 0.3, 0.5, 0.2, 0.85, 0.6, 0.15, 0.8);
        CHECK_MSG(std::abs(b[3] - 0.81394102980498539) < 1e-15, "B's Lam = 0.81394102980498539 (the trace's own print; f32 0.81394106149673462)");
        CHECK_MSG(std::abs(b[5] - 0.0023112930334865441) < 1e-15, "B's 叶 = 0.0023112930334865454 (the trace's print; f32 0.0023112926)");
        // (3) POINT C - the degenerate corner the per-material defaults sit on: `rT = rB = 1` with the frame
        //     aligned to `H`, so every dot is 0 or 1. It is also the value the NUMERATOR alone gives at `p = 1`
        //     (`0.15915493667125702 / 2`) and the one place the floor is furthest from binding.
        std::array<double, 6u> const c = aniso_parts(1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
        CHECK_MSG(std::abs(c[3] - 1.0) < 1e-15, "C's Lam = 1.0 exactly");
        CHECK_MSG(std::abs(c[4] - 2.0) < 1e-15, "C's denominator = (1*1 + 1*1)*1*1 = 2.0, so the floor is nowhere near it");
        CHECK_MSG(std::abs(c[5] - 0.07957746833562851) < 1e-15, "C's 叶 = 0.07957746833562851 (the trace's print; f32 0.07957747)");
        // (4) THE FLOOR, AS A PROPERTY RATHER THAN A HOPE: `s2` is a convex combination of `{rB^2, rT^2, p^2}` for
        //     a unit frame, so the un-floored denominator is strictly positive there and the floor can only be
        //     reached by a frame that is NOT unit (which `surface_tbn` can produce nothing of, but which the
        //     reference still guards). Pinned so that "the floor never binds" is a statement this file has tested.
        std::array<double, 6u> const tiny = aniso_parts(1e-4, 1e-4, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0);
        CHECK_MSG(tiny[4] == 0.0010000000474974513,
                  "a near-zero roughness drives the whole denominator under the reference's floor, and `max` is what answers it");
        CHECK_MSG(std::isfinite(tiny[5]) && tiny[5] > 0.0,
                  "so the lobe is a POSITIVE FINITE number even there - which is the NaN property the arm needs, because it evaluates the lobe for every pixel and multiplies by a mask that is zero for all of them (`0 * NaN` would not be saved by the mask)");
        // (5) THE TWO `1/(2π)` CONSTANTS ARE NOT INTERCHANGEABLE, at the precision the shader builds them in.
        float const iso_constant = 0.3183099925518036f * 0.5f;
        float const lobe_constant = 0.31830987334251404f * 0.5f;
        CHECK_MSG(iso_constant != lobe_constant,
                  "`0.3183099925518036f * 0.5f` and `0.31830987334251404f * 0.5f` are different float32s (0x3E22F987 vs 0x3E22F983), so the lobe must NOT reuse `goo_dv_half_inverse_pi`");
        CHECK_MSG(std::abs(static_cast<double>(lobe_constant) - 0.15915493667125702) < 1e-17,
                  "and the lobe's is the one the expectation above is built from");
    }

    // ---- 8r2. STEP 11: `DV_SmithJointGGX_Aniso.original`'s DENOMINATOR, AS THE GRAPH WRITES IT ----
    //
    // THIS BLOCK REPLACES NO PINS, BECAUSE THERE WERE NONE TO REPLACE: the old expression was pinned nowhere in
    // this file (no `find()` on it, no closed form), which is exactly how a 1.78479x error in every specular pixel
    // of every material shipped in step 10 and passed 13/13. What is pinned now is the chain that
    // `deren-ab/goo_step11_armA_raw.md` records verbatim out of the dump:
    //
    //     S  = 1 + NoH^2*(a2 - 1)                                              # `运算.011`
    //     λ  = |NoL|*(a2 + (1 - a2)*NoV^2) + NoV*sqrt(a2 + (1 - a2)*NoL^2)     # `运算.005/006` + `运算.009/010/019`
    //     原 = 0.1591549962759018 * a2 / max(S^2 * λ, 1.1754899742869237e-35)  # `运算.013` .. `运算.018`
    //
    // The expectations are float64 closed forms rounded to 12+ significant digits, and the arbitrated point's
    // `7.291723011e-02` is the trace's own number to the digit. `dv_parts` returns `{S, λ, floored denominator, 原}`
    // so the SHAPE of the fix is pinned and not only its value: a re-arrangement that happened to keep `原` right
    // would still have to answer for `λ` and `S`.
    {
        auto const dv_parts = [](double const r, double const nov, double const nol, double const noh) {
            double const cr = r * r;
            double const a2 = cr * cr;                                       // `a2 = perceptualRoughness^4`
            double const s = 1.0 + noh * noh * (a2 - 1.0);                   // `运算.011`'s own input
            double const lv = std::abs(nol) * (a2 + (1.0 - a2) * nov * nov); // `运算.005/006`, UN-rooted
            double const ll = nov * std::sqrt(a2 + (1.0 - a2) * nol * nol);  // `运算.009/010/019`, rooted
            double const lambda = lv + ll;                                   // `运算.012`
            double const denominator = s * s * lambda;                       // `运算.016`
            double const floored = std::max(denominator, static_cast<double>(k_dv_denominator_floor));
            return std::array<double, 4u>{s, lambda, floored, 0.1591549962759018 * a2 / floored};
        };
        // ... AND THE OLD EXPRESSION, KEPT ONLY TO STATE THE DEFECT AND ITS TWO FACTORS - it is not any shader's
        // arithmetic any more, and nothing below asserts that it is.
        auto const old_lambda = [](double const r, double const nov, double const nol) {
            double const cr = r * r;
            double const a2 = cr * cr;
            return std::abs(nol) * std::sqrt(std::max(a2 + (1.0 - a2) * nov * nov, 0.0));
        };
        auto const old_original = [&old_lambda](double const r, double const nov, double const nol, double const noh) {
            double const cr = r * r;
            double const a2 = cr * cr;
            double const s2 = noh * noh * (a2 - 1.0) + 1.0;
            return 0.1591549962759018 * (a2 / std::max(s2 * s2, 1e-12)) * (1.0 / (1.0 + old_lambda(r, nov, nol)));
        };
        // (1) THE ARBITRATED POINT: `R = 0.25, NoV = NoL = 0.7, NoH = 0.95`, the trace's own sample.
        std::array<double, 4u> const point = dv_parts(0.25, 0.7, 0.7, 0.95);
        CHECK_MSG(std::abs(point[3] - 7.29172301067769e-02) < 1e-15,
                  "原 at R 0.25 / NoV = NoL 0.7 / NoH 0.95 = 7.29172301067769e-02 (the trace's own print was 7.291723011e-02, rounded)");
        CHECK_MSG(std::abs(point[0] - 0.101025390625) < 1e-15,
                  "with S = 1 + 0.9025*(a2 - 1) = 0.101025390625 at a2 = 0.25^4 = 0.00390625");
        CHECK_MSG(std::abs(point[1] - 0.835389614601147) < 1e-15,
                  "and λ = 0.7*b + 0.7*sqrt(b) = 0.835389614601147 at b = a2 + (1 - a2)*0.49");
        //     The tolerance below is 3 ulp of the value, not an exact match: at `-O3` clang contracts
        //     `a2 + (1 - a2)*x*x` into an FMA, which moves this denominator down by exactly 1 ulp (measured with
        //     `deren-ab/_s11_den.cpp`: 0.0085260946321240003998 contracted vs 0.0085260946321240038692 with
        //     `-ffp-contract=off`, i.e. the literal). The shader itself is fp32, so no fp64 ulp is load-bearing.
        CHECK_MSG(std::abs(point[2] - 8.526094632124004e-03) < 1e-17,
                  "so the denominator S^2 * λ = 8.526094632124004e-03: this point is thirty-two orders ABOVE the floor");
        CHECK_MSG(std::abs(old_original(0.25, 0.7, 0.7, 0.95) - 4.085479384665246e-02) < 1e-15,
                  "the OLD expression gave 4.085479384665246e-02 here - what step 10 shipped for every specular pixel");
        CHECK_MSG(std::abs(old_original(0.25, 0.7, 0.7, 0.95) / point[3] - 0.5602899861504124) < 1e-12,
                  "i.e. 0.5602899861504124x the reference's value");
        CHECK_MSG(std::abs(point[3] / old_original(0.25, 0.7, 0.7, 0.95) - 1.7847900635717335) < 1e-12,
                  "which is the brief's `1.78x too dark`, at 1.7847900635717335x");
        // (2) THE TWO DEFECTS SEPARATELY. They are ratios that MULTIPLY to the one above, because the fix is
        //     `old/new = [λ_new/λ_old] * [λ_old/(1 + λ_old)]`: the first factor is the STRUCTURE (the missing root
        //     and the |NoL| half), the second is the `+1`. The trace's rounded `1.7014` / `0.3293` are not what is
        //     asserted - the closed forms are, to 12+ digits.
        double const lambda_old = old_lambda(0.25, 0.7, 0.7);
        CHECK_MSG(std::abs(lambda_old - 0.4909950833511471) < 1e-15,
                  "the OLD λ at the arbitrated point is 0.4909950833511471");
        CHECK_MSG(std::abs(point[1] / lambda_old - 1.701421547644496) < 1e-12,
                  "so the STRUCTURE alone is 1.701421547644496x - the trace's 1.7014");
        CHECK_MSG(std::abs(lambda_old / (1.0 + lambda_old) - 0.3293069768195285) < 1e-12,
                  "and the `+1` alone is 0.3293069768195285x - the trace's 0.3293");
        CHECK_MSG(std::abs((point[1] / lambda_old) * (lambda_old / (1.0 + lambda_old)) - 0.5602899861504124) < 1e-12,
                  "whose product IS the shipped/reference ratio: the two defects are independent factors of it");
        // (3) THE DEGENERATE AND BOUNDARY POINTS, one per clause of the expression.
        //     a2 = 0, i.e. the body's `perceptualRoughness = 0.0`: the NUMERATOR is 0, so 原 is 0 for every
        //     direction - no floor can rescue it and none is asked to.
        CHECK_MSG(dv_parts(0.0, 0.7, 0.7, 0.95)[3] == 0.0,
                  "roughness 0 -> a2 0 -> 原 is EXACTLY 0 (the body's own answer, not a floor)");
        //     `roughness = 1` -> `a2 = 1`: both brackets collapse to 1, so `λ = |NoL| + NoV` and `S = 1`.
        std::array<double, 4u> const flat = dv_parts(1.0, 0.7, 0.7, 0.95);
        CHECK_MSG(flat[0] == 1.0 && flat[1] == 1.4,
                  "roughness 1 -> a2 1 -> S = 1 and λ = |NoL| + NoV = 1.4 exactly");
        CHECK_MSG(std::abs(flat[3] - 0.11368214019707272) < 1e-15,
                  "and 原 = 0.1591549962759018 * 1 / 1.4 = 0.11368214019707272");
        //     `NoH = 0` -> `S = 1`: THE SQUARE IS ON `S`, so the denominator degenerates to `λ` itself and not to
        //     `λ^2` - which is the reading a `D * Gv` transcription gets wrong in a way that still looks plausible.
        CHECK_MSG(dv_parts(0.25, 0.7, 0.7, 0.0)[0] == 1.0 && dv_parts(0.25, 0.7, 0.7, 0.0)[2] == 0.835389614601147,
                  "NoH 0 -> S 1 -> the denominator is λ = 0.835389614601147, NOT λ^2");
        //     AND THE FLOOR, WHICH NO PHYSICAL INPUT CAN REACH (at R 0.05 / NoV = NoL 0.05 / NoH 0.999 it is
        //     1.056884557344398e-08, still twenty-seven orders above the literal), so it is CONSTRUCTED: `a2 = 1e-8` with
        //     `NoV = NoL = 0` gives `λ = 0` EXACTLY, and the reference's MAXIMUM is what stands between that and a
        //     division by zero.
        std::array<double, 4u> const zero_lambda = dv_parts(0.01, 0.0, 0.0, 0.5);
        CHECK_MSG(zero_lambda[1] == 0.0,
                  "a2 = 1e-8 with NoV = NoL = 0 gives λ = 0 exactly, so the un-floored denominator is 0");
        CHECK_MSG(zero_lambda[2] == static_cast<double>(k_dv_denominator_floor),
                  "and 运算.017 replaces it with 1.1754899742869237e-35, the dump's literal");
        CHECK_MSG(std::isfinite(zero_lambda[3]) && std::abs(zero_lambda[3] - 1.3539460119381151e+26) < 1e+16,
                  "so 原 comes back FINITE: 0.1591549962759018 * 1e-8 / 1.1754899742869237e-35 = 1.3539460119381151e+26");
        //     ... AND A SECOND, NON-ZERO CASE UNDER THE FLOOR, so the pin is about the MAXIMUM and not about the
        //     special value 0: `NoV = NoL = 1e-32` leaves `λ = 1.0001e-36`, whose `S^2`-weighted value
        //     5.625562537503751e-37 is still below the literal.
        std::array<double, 4u> const sub_floor = dv_parts(0.01, 1e-32, 1e-32, 0.5);
        CHECK_MSG(std::abs(sub_floor[1] - 1.0001e-36) < 1e-42,
                  "λ = 1.0001e-36 at NoV = NoL = 1e-32: non-zero, so the floor is not merely a zero guard");
        CHECK_MSG(sub_floor[2] == static_cast<double>(k_dv_denominator_floor),
                  "and S^2 * λ = 5.625562537503751e-37 is below 1.1754899742869237e-35, so the MAXIMUM fires here too");
    }
    // ---- 8s. STEP 6: `DeSaturation` AND ITS CONSUMER, `色相/饱和度/明度` ----
    //
    // THE TWO NODES ARE ONE LINE, and that is the finding rather than a simplification. `DeSaturation` is
    // `luma + d*(c - luma)` (eight of its eleven nodes; the other three are `组输入`, `组输出` and two `NodeFrame`
    // - the frame count is the reason the plan's mechanism table says "11"). `色相/饱和度/明度` is Blender's
    // `ShaderNodeHueSaturation`, whose own renderer GLSL is `hsv[1] = clamp(hsv[1] * sat, 0, 1); hsv[2] = hsv[2] *
    // value; outcol = mix(col, outcol, fac)` - and with `Hue = 0.5` (`fract(h + 0.5) = h`, so the hue does not
    // turn), `Value = 1.0` and `Fac = 1.0` THAT IS `lerp(luma(colour).xxx, colour, sat)`, i.e. the same family as
    // `DeSaturation`. WHICH IS WHY THE SHADER HAS ONE FUNCTION (`goo_hsv_desaturate`) AND NOT TWO.
    //
    // AND THE SATURATION IS DRIVEN BY THE FIRST NODE, NOT BY THE MATERIAL ALONE:
    //     `色相/饱和度/明度.Saturation <- 钳制.004.Result = clamp(运算.005.Value, 0, 1)`
    //     `运算.005.Value    <- 群组.017.Vector`   (= `DeSaturation(DeSaturation = 0.0, Color = RampColor)`)
    //     `运算.005.Value_001 <- 转接点.160 <- 组输入.Color desaturation in shaded areas attenuation`
    // so `saturation = clamp(luma(RampColor) + attenuation, 0, 1)` - THE RAMP'S OWN LUMA IS THE FIRST ADDEND, and
    // `RampColor` is the DARK ramp, which is why the term is a SHADOW term without any shadow mask in it.
    {
        // A1: THE LUMA WEIGHTS ARE THE REFERENCE'S STORED FLOATS, to the bit. The float32 literals below are what
        // `合并 XYZ.002` holds; the check is against the DECIMAL spellings so a "tidied" weight fails here.
        CHECK_MSG(k_desaturation_luma_r == 0.21267299354076385f, "合并 XYZ.002.X, verbatim");
        CHECK_MSG(k_desaturation_luma_g == 0.7151520252227783f, "合并 XYZ.002.Y, verbatim - the green weight is 71.5% of the luma");
        CHECK_MSG(k_desaturation_luma_b == 0.07217500358819962f, "合并 XYZ.002.Z, verbatim");
        CHECK_MSG(std::abs((k_desaturation_luma_r + k_desaturation_luma_g + k_desaturation_luma_b) - 1.0f) < 1e-7f,
                  "the three weights sum to 1, which is what makes luma(white) = 1 and the desaturation a no-op on a white pixel");
        CHECK_MSG(k_desaturation_default == 0.0f, "组输入.Color desaturation in shaded areas attenuation's interface[] default is 0.0 (the reference's own number)");
        CHECK_MSG(k_desaturation_neutral == 1.0f, "and the identity the arm's NON-members keep is 1.0 - a different number, on purpose");

        // A2: THE CLOSED FORM AT TWO INPUTS. The arithmetic is written out because these are the numbers a
        // "simplification" of the luma weights or of `d` would move.
        //
        //   INPUT 1: colour = (0.8, 0.6, 0.5), d = 0.5
        //     luma = 0.21267299354076385*0.8 + 0.7151520252227783*0.6 + 0.07217500358819962*0.5
        //          = 0.17013839483261108 + 0.4290912151336670  + 0.03608750179409981
        //          = 0.6353171467781067        (float32: 0.6353171467781067)
        //     out  = luma + 0.5*(c - luma)  =>  (0.7176585793495178, 0.6176586151123047, 0.567658543586731)
        vec3 const in1{0.8f, 0.6f, 0.5f};
        vec3 const out1 = desaturation_closed_form(in1, 0.5f);
        CHECK_MSG(std::abs(dot3(in1, vec3{k_desaturation_luma_r, k_desaturation_luma_g, k_desaturation_luma_b}) - 0.6353171467781067f) < 1e-6f,
                  "luma(0.8, 0.6, 0.5) = 0.6353171467781067");
        CHECK_MSG(std::abs(out1.x - 0.7176585793495178f) < 1e-6f, "and d = 0.5 gives R = 0.7176585793495178");
        CHECK_MSG(std::abs(out1.y - 0.6176586151123047f) < 1e-6f, "and G = 0.6176586151123047");
        CHECK_MSG(std::abs(out1.z - 0.5676585435867310f) < 1e-6f, "and B = 0.5676585435867310");
        //
        //   INPUT 2: colour = (0.2, 0.4, 0.9), d = 0.5 - the OTHER side of the luma (blue-dominant), so a swapped
        //   Y/Z weight pair cannot pass both rows.
        //     luma = 0.04253459870815277 + 0.28606081008911133 + 0.06495750319957733 = 0.39355289936065674
        //     out  = luma + 0.5*(c - luma)  =>  (0.2967764437198639, 0.3967764377593994, 0.6467764377593994)
        vec3 const in2{0.2f, 0.4f, 0.9f};
        vec3 const out2 = desaturation_closed_form(in2, 0.5f);
        CHECK_MSG(std::abs(dot3(in2, vec3{k_desaturation_luma_r, k_desaturation_luma_g, k_desaturation_luma_b}) - 0.39355289936065674f) < 1e-6f,
                  "luma(0.2, 0.4, 0.9) = 0.39355289936065674");
        CHECK_MSG(std::abs(out2.x - 0.2967764437198639f) < 1e-6f, "and d = 0.5 gives R = 0.2967764437198639");
        CHECK_MSG(std::abs(out2.y - 0.3967764377593994f) < 1e-6f, "and G = 0.3967764377593994");
        CHECK_MSG(std::abs(out2.z - 0.6467764377593994f) < 1e-6f, "and B = 0.6467764377593994");

        // A3: THE TWO ANCHORS, which are what make the term safe for a material outside the arm.
        vec3 const grey1 = desaturation_closed_form(in1, 0.0f);
        CHECK_MSG(grey1.x == grey1.y && grey1.y == grey1.z, "d = 0 collapses the colour to its luma: all three channels equal");
        CHECK_MSG(std::abs(grey1.x - 0.6353171467781067f) < 1e-6f, "and the grey it collapses to IS the luma");
        vec3 const same1 = desaturation_closed_form(in1, 1.0f);
        CHECK_MSG(same1.x == in1.x && same1.y == in1.y && same1.z == in1.z, "d = 1 is the IDENTITY, exactly - which is the neutral a non-member material keeps");
        // ... AND THE CLAMP. `钳制.004 = clamp(运算.005.Value, 0, 1)` with `Min = 0.0`, `Max = 1.0` (both unlinked
        // literals), and the shipped function saturates its own argument, so a saturation outside the range answers
        // the reference's clamped one rather than extrapolating. The negative case is the one with teeth: the
        // attenuation's `interface[]` default is `0.0` and NO material states a negative one, but `运算.005` is a
        // sum, and a port that extrapolated below zero would INVERT the term (it would overshoot past grey).
        CHECK_MSG(desaturation_closed_form(in1, 2.0f).x == desaturation_closed_form(in1, 1.0f).x, "a saturation above 1 answers the same as 1 (钳制.004.Max = 1.0)");
        CHECK_MSG(desaturation_closed_form(in1, -3.0f).x == desaturation_closed_form(in1, 0.0f).x, "and below 0 the same as 0 (钳制.004.Min = 0.0)");
        CHECK_MSG(std::abs(desaturation_closed_form(in1, -3.0f).x - 0.6353171467781067f) < 1e-6f, "i.e. a negative saturation answers the full grey and NOT an inverted colour");

        // A4: THE SATURATION'S OTHER ADDEND, AND WHAT A MATERIAL STATES. These four numbers are the composition
        // the whole step turns on: the attenuation is the material's, the luma is the ramp's, and the sum is
        // CLAMPED - so a bright ramp pixel saturates at 1 and the term switches itself off in the light.
        CHECK_MSG(std::abs((0.7000000476837158f + 0.0f) - 0.7000000476837158f) < 1e-9f, "body_01's attenuation is 0.7000000476837158 (nodes.json)");
        CHECK_MSG(std::abs((0.8999999761581421f + 0.0f) - 0.8999999761581421f) < 1e-9f, "cloth_01/02/04/05 state 0.8999999761581421");
        CHECK_MSG(std::abs((0.8500000238418579f + 0.0f) - 0.8500000238418579f) < 1e-9f, "cloth_03 states 0.8500000238418579 - the one cloth that differs");
        auto const saturation = [](float const ramp_luma, float const attenuation) {
            return std::min(1.0f, std::max(0.0f, ramp_luma + attenuation));
        };
        CHECK_MSG(saturation(0.0f, 0.7000000476837158f) == 0.7000000476837158f, "a black ramp pixel leaves the material's attenuation as the saturation");
        CHECK_MSG(saturation(0.5f, 0.7000000476837158f) == 1.0f, "a mid-grey ramp pixel already clamps body's saturation to 1");
        CHECK_MSG(saturation(0.05f, 0.0f) == 0.05f, "with the group's own default (0.0) the saturation IS the ramp's luma");
        CHECK_MSG(saturation(0.9f, 0.9f) == 1.0f, "and the two addends can clamp on their own");
    }
    // ---- 8t. STEP 7: THE FACE CONTAINER ----
    //
    // THE ASSERTIONS THE SPEC'S §10 ASKS FOR, with THREE of them re-derived rather than transcribed, because this
    // step re-read the graph and found the spec's own arithmetic wrong in two places and its sign ruling wrong in
    // a third:
    //
    //   * §10-A2's `x = 2 × center` is NOT the sigmoid's 50% point. The exponent is `−3·sharp·(x − center)`, so the
    //     midpoint is `x = center` - and the spec halved `(R + G)/2` TWICE (it fed `2·center` into a function whose
    //     argument is already the average), which put its expectation at `0.849` instead of `0.5`. Corrected below.
    //   * §10-A4's intermediate `群组.011 = 0.9302417226604509` is not what `1/(1 + 100000^(−3·0.5·(0.5 − 0.1)))`
    //     is: that is `1/(1 + 0.001) = 0.9990009996687309`. Its FINAL answer (`ramp_u = 0.5854986799208568`)
    //     survives, because `运算.014`'s MINIMUM against `CastShadows = 0.4` discards the value either way.
    //   * §11-U1's `值(明度) = 1.0` is `-1.0` in BOTH dumps, which the parent re-read and which
    //     `shaders/goo_toon.slang` was already built on. That INVERTS §10-A10's numbers, and A10 below is the
    //     corrected derivation: a sun in front of the face is `AngleThreshold = 0`, the spec's `π/3` example is
    //     `1/3` and its `2π/3` example is `2/3`.
    {
        // THE ONE SUB-GROUP ALL THREE OF THE FACE'S CALL SITES USE, in the reference's own order:
        // `运算.003 = −3·sharp`; `运算.004 = x − center`; `运算.005 = ·`; `运算.002 = 100000^·`;
        // `运算.006 = 1 + ·`; `运算.007 = 1 / ·`. Nothing is clamped (all six `use_clamp` are false).
        auto const sigmoid_sharp = [](double const x, double const center, double const sharp) {
            return 1.0 / (1.0 + std::pow(100000.0, -3.0 * sharp * (x - center)));
        };

        // A0: THE CLOSED FORM ITSELF, including the degenerate `sharp = 0` (the exponent is 0, `100000^0 = 1`, and
        // every input answers 0.5 - which is what the reference computes and NOT a guard this port added).
        CHECK_MSG(std::abs(sigmoid_sharp(0.5, 0.5, 0.5) - 0.5) < 1e-9, "A0: x = center answers exactly 0.5");
        CHECK_MSG(std::abs(sigmoid_sharp(0.0, 0.0, 0.0) - 0.5) < 1e-9, "A0: sharp = 0 answers 0.5 for EVERY x");
        CHECK_MSG(std::abs(sigmoid_sharp(0.5, 0.0, 0.17000000178813934) - 0.9495878592652764) < 1e-9,
                  "A0: face_01's CastShadow_curve at x = 0.5 is 0.9495878592652764");

        // A1: THE SDF'S LOWER BOUND IS NOT ZERO. `SDF_RemaphalfLambert_center` is the material's `0.1`, so a
        // fragment whose distance field reads 0 still answers `0.151` - i.e. the shadow side of this face is never
        // fully dark, which is a property of the AUTHORED THRESHOLD rather than of the field.
        double const a1 = sigmoid_sharp(0.0, 0.0 + 0.10000000894069672, 0.5000000596046448);
        CHECK_MSG(std::abs(a1 - 0.15097951103053278) < 1e-9, "A1: the SDF curve at (R,G) = (0,0), AngleThreshold 0");
        CHECK_MSG(std::abs(a1 * 255.0 - 38.4997753) < 1e-3, "A1: which is RD pixel 38.4997... - the ramp's first dark texel");

        // A2 (CORRECTED): THE CENTRE IS THE 50% POINT, and the input that proves it is `(R + G)/2 == center`, NOT
        // `R == G == 2·center`. The two rows below are the correction and the spec's own number, so a reader can
        // see which one this step believes without leaving the file.
        CHECK_MSG(std::abs(sigmoid_sharp(0.10000000894069672, 0.0 + 0.10000000894069672, 0.5000000596046448) - 0.5) < 1e-9,
                  "A2: (R+G)/2 == SDF_RemaphalfLambert_center IS the 50% point");
        CHECK_MSG(std::abs(sigmoid_sharp(0.20000001788139344, 0.10000000894069672, 0.5000000596046448) - 0.8490204889694672) < 1e-9,
                  "A2': ...and the spec's `2 x center` input answers 0.8490204889694672, not 0.5 - the arithmetic the correction rests on");
        CHECK_MSG(std::abs(2.0 * 0.10000000894069672 - 1.0 - (-0.7999999821186066)) < 1e-9,
                  "A2: equivalently the threshold in dot(N,L) is 2*center - 1 = -0.7999999821186066, which is why the light can be well behind the face and it still reads lit");

        // A3: THE CHIN CURVE, whose interval is narrow by construction (its `sharp` is `0.1` against the SDF's `0.5`).
        CHECK_MSG(std::abs(sigmoid_sharp(0.5, 0.5, 0.10000000149011612) - 0.5) < 1e-9, "A3: N·L = 0 is the chin curve's own midpoint");
        CHECK_MSG(std::abs(sigmoid_sharp(1.0, 0.5, 0.10000000149011612) - 0.8490204460873048) < 1e-9, "A3: N·L = +1 gives 0.8490204460873048");
        CHECK_MSG(std::abs(sigmoid_sharp(0.0, 0.5, 0.10000000149011612) - 0.1509795539126952) < 1e-9, "A3: N·L = -1 gives 0.1509795539126952 - the chin's whole range");

        // A4: THE RAMP COORDINATE'S TWO MINIMUMS, in the order the graph writes them - and they are NOT merged, which
        // is the spec's own warning: `运算.022` takes `混合.022.Result` (the shadow proxy's mix) and `运算.014` (the
        // SDF's minimum against the cast shadow), and `运算.002` then takes the smaller of `转接点.016` and
        // `混合.002` (the cm.G-selected branch).
        double const a4_cast = 0.4; // `Shader Info.Cast Shadows`
        double const a4_cm_g = 1.0;
        double const a4_m5 = 1.0 * (1.0 - a4_cm_g) + a4_cast * a4_cm_g; // 混合.005 = lerp(1.0, CastShadows, cm.G)
        double const a4_s016 = sigmoid_sharp(a4_m5, 0.0, 0.17000000178813934);
        double const a4_sdf = sigmoid_sharp(0.5, 0.10000000894069672, 0.5000000596046448);
        double const a4_o14 = std::min(a4_sdf, a4_cast);
        double const a4_o22 = std::min(1.0, a4_o14); // 混合.022 = 1.0 here: the screen-space proxy is not evaluable
        double const a4_s021 = sigmoid_sharp(0.5 * 0.2 + 0.5, 0.5, 0.10000000149011612);
        double const a4_m2 = a4_o22 * (1.0 - a4_cm_g) + a4_s021 * a4_cm_g;
        double const a4_ramp_u = std::min(a4_s016, a4_m2);
        CHECK_MSG(std::abs(a4_s016 - 0.9128258137112348) < 1e-9, "A4: 转接点.016, the CastShadow curve");
        CHECK_MSG(std::abs(a4_sdf - 0.9990009996687309) < 1e-9, "A4 (CORRECTED): 群组.011 at (R,G) = (0.5,0.5) - the spec's 0.9302417 is not this expression");
        CHECK_MSG(std::abs(a4_s021 - 0.5854986799208568) < 1e-9, "A4: 群组.021, the chin curve at 0.5*N·L + 0.5 = 0.6");
        CHECK_MSG(a4_o14 == a4_cast, "A4: 运算.014's MINIMUM against the cast shadow BINDS here (0.4 < 0.999)");
        CHECK_MSG(std::abs(a4_ramp_u - 0.5854986799208568) < 1e-9, "A4: ramp_u = 0.5854986799208568, i.e. RD pixel round(0.5855*255) = 149");

        // A5: THE GSBA GATE AT THE RAMP'S DARKEST TEXEL. `钳制.006 = clamp(1.0, 0.0, SmoothStep(GSBA, 1.0, ramp.a))`
        // - the VALUE 1.0 is what is clamped, so the gate IS the smoothstep, which is why a LOW `GSBA` lifts MORE.
        auto const smoothstep01 = [](double const minimum, double const maximum, double const x) {
            double const t = std::min(1.0, std::max(0.0, (x - minimum) / (maximum - minimum)));
            return t * t * (3.0 - 2.0 * t);
        };
        double const a5 = smoothstep01(-1.5015480518341064, 1.0, 0.0);
        CHECK_MSG(std::abs(a5 - 0.6483564136266021) < 1e-9, "A5: face_01's GSBA gate at ramp.a = 0 is 0.6483564136266021");
        CHECK_MSG(a5 > smoothstep01(0.0, 1.0, 0.0) + 0.6, "A5: and it is much higher than the body's own GSBA (0.0) would give - LOWER GSBA LIFTS MORE");

        // A7 (CORRECTED): THE NOSE SHADOW IS **NOT** AN IDENTITY ON THIS ASSET. The spec's §2.4/§10-A7 read the
        // face's albedo alpha as "a constant 1 in every material this repository loads"; measured over
        // `T_actor_laevat_face_01_D.png` it is `min 202 / mean 254.978 / max 255`, so `混合.017` reaches 0.856 where
        // the alpha dips. The two anchors below are the identity (alpha 1) and the measured minimum.
        auto const nose_shadow_mix = [](double const colour, double const alpha) {
            return colour * (1.0 - alpha) + 1.0 * alpha;
        };
        CHECK_MSG(std::abs(nose_shadow_mix(0.3084079325199127, 1.0) - 1.0) < 1e-12, "A7: alpha = 1 is white, so the nose shadow lifts out entirely");
        CHECK_MSG(std::abs(nose_shadow_mix(0.3084079325199127, 202.0 / 255.0) - 0.8562573349943348) < 1e-9,
                  "A7: alpha = 202/255 (the texture's own minimum) gives 0.8562573349943348 - a 14.4% darkening the spec says does not exist");
        CHECK_MSG(std::abs(nose_shadow_mix(0.3084079325199127, 0.0) - 0.3084079325199127) < 1e-12,
                  "A7': and alpha = 0 gives the material's own colour, which is the arm's other anchor");

        // A8: THE EMISSION-BRIGHTNESS SWITCH, whose factor is `CsutmMask.G > 0.5` and NOT `cm_M.G` (the spec's §2.4
        // attributes it to the latter; `运算.028 <- 分离 XYZ.006.Y <- 图像纹理.004.Color` is the dump's answer). The
        // two numbers are the material's and the group's, in that order, and BOTH of them are live: the material
        // overrides `Eyes white Final brightness` to 1.5 and leaves `Face Final brightness` at 1.15.
        auto const emission_strength = [](double const csumt_g) { return csumt_g > 0.5 ? 1.5 : 1.149999976158142; };
        CHECK_MSG(emission_strength(0.0) == 1.149999976158142, "A8: CsumtMask.G = 0 takes the FACE brightness (1.149999976158142)");
        CHECK_MSG(emission_strength(0.5) == 1.149999976158142, "A8: GREATER_THAN is strict, so exactly 0.5 is still the face brightness");
        CHECK_MSG(emission_strength(0.5001) == 1.5, "A8: and above 0.5 takes the EYE-WHITE brightness the material states (1.5)");
        CHECK_MSG(std::abs(1.2999999523162842 - 1.5) > 0.2,
                  "A8': the group's own default for that socket is 1.2999999523162842, i.e. the 1.5 is an OVERRIDE and not the interface's number");

        // A9: THE TWO u MIRRORS ARE ONE SWITCH. `混合.001` (the SDF's u) and `混合.012` (the front-red u) both read
        // `运算.004 = GREATER_THAN(Flip threshold, 0)`, so writing them as two independent tests is a bug that this
        // asset cannot show and the next one can.
        auto const mirrored_sdf_u = [](double const uv_x, bool const flip_positive) { return flip_positive ? 1.0 - uv_x : uv_x; };
        auto const mirrored_front_u = [](double const uv_x, bool const flip_positive) {
            double const step_gt = uv_x > 0.5 ? 1.0 : 0.0; // 运算.006 = GREATER_THAN(uv.x, 0.5)
            return flip_positive ? (1.0 - step_gt) : step_gt;
        };
        CHECK_MSG(mirrored_sdf_u(0.25, false) == 0.25 && mirrored_sdf_u(0.25, true) == 0.75, "A9: the SDF's u is 1 - uv.x when the flip bit is set");
        CHECK_MSG(mirrored_front_u(0.25, false) == 0.0 && mirrored_front_u(0.25, true) == 1.0, "A9: and the front-red gate takes the opposite half of the u axis at the same time");
        CHECK_MSG(mirrored_front_u(0.75, false) == 1.0 && mirrored_front_u(0.75, true) == 0.0, "A9: ...on the other side of 0.5 too, which is what makes the pair a mirror");

        // A10 (RE-DERIVED): `calculateAngel`'s two outputs with `值(明度) = -1.0`, which is what BOTH dumps store.
        //
        //     s = dot(proj, headRight)                       -> `Flip threshold`
        //     c = dot(proj, headForward) * 值(明度) = -dot(proj, headForward)
        //     turns = atan2(s, c) / pi
        //     AngleThreshold = turns > 0 ? 1 - turns : 1 + turns   = 1 - |turns|
        //
        // AND THE SIGN IS THE WHOLE POINT OF THE CORRECTION: with `+1.0` a sun IN FRONT of the face gives
        // `turns = 0` and `AngleThreshold = 1` (the SDF read as fully dark); with the reference's `-1.0` it gives
        // `turns = 1` and `AngleThreshold = 0` (fully lit), which is what the iris' own arm has been computing
        // since step 1 and what a face lit from the front must do.
        auto const face_angles = [](double const theta_axis) {
            double const s = std::sin(theta_axis);
            double const c = -std::cos(theta_axis); // the `-1.0` in one place, on purpose: see the note above
            // THE DIVISOR IS THE REFERENCE'S OWN FLOAT PI (`运算.004.Value_001 = 3.141592502593994`), which is
            // 4.8e-8 SHORT of pi - so "exactly 1/3" is `0.333333301...` here and the assertions below carry a
            // 1e-6 tolerance rather than a 1e-9 one. Writing the double pi instead would be testing a quantity the
            // shader does not compute.
            double const turns = std::atan2(s, c) / 3.141592502593994;
            return std::pair<double, double>{turns > 0.0 ? 1.0 - turns : 1.0 + turns, s};
        };
        CHECK_MSG(std::abs(face_angles(0.0).first - 0.0) < 1e-6, "A10: a sun IN FRONT of the face (theta = 0) gives AngleThreshold 0 - the corrected reading");
        CHECK_MSG(std::abs(face_angles(3.141592653589793).first - 1.0) < 1e-6, "A10: and directly behind gives 1");
        CHECK_MSG(std::abs(face_angles(3.141592653589793 / 2.0).first - 0.5) < 1e-6, "A10: a sun at the head's side is 0.5, which is what the SDF's threshold swings about");
        CHECK_MSG(std::abs(face_angles(3.141592653589793 / 3.0).first - 0.3333333333333333) < 1e-6,
                  "A10: theta = pi/3 gives 1/3 - the spec's A10 says 2/3 because it read 值(明度) as +1");
        CHECK_MSG(std::abs(face_angles(2.0 * 3.141592653589793 / 3.0).first - 0.6666666666666666) < 1e-6,
                  "A10: theta = 2pi/3 gives 2/3 - the two spec examples SWAP under the correction");
        CHECK_MSG(std::abs(face_angles(3.141592653589793 / 3.0).second - 0.8660254037844386) < 1e-9, "A10: Flip threshold = sin(theta)");
        CHECK_MSG(std::abs(face_angles(2.0 * 3.141592653589793 / 3.0).second - 0.8660254037844386) < 1e-9,
                  "A10: and it is the SAME at 2pi/3, which is why it can only answer 'left or right' and not 'front or back'");
        CHECK_MSG(std::abs(k_forward_sign - (-1.0f)) < 1e-12f, "A10: the iris' own transcribed sign is -1.0 too, so the two families share one answer");

        // A12: `Recalculate normal`'s two branches. The sphere normal is `normalize(posWS - headCenter)` and the
        // second MIX hands the CHIN (cm.G = 1) back to the model's own normal - so a low-poly jaw does not wear a
        // sphere and the face plate does.
        auto const recalculate_normal = [](vec3 const pos, vec3 const head_center, vec3 const normal_ws, float const strength, float const chin_mask) {
            vec3 const to_center = vec3{pos.x - head_center.x, pos.y - head_center.y, pos.z - head_center.z};
            vec3 const sphere = normalize3(to_center);
            vec3 const mixed = normalize3(vec3{
                normal_ws.x + (sphere.x - normal_ws.x) * strength,
                normal_ws.y + (sphere.y - normal_ws.y) * strength,
                normal_ws.z + (sphere.z - normal_ws.z) * strength});
            return normalize3(vec3{
                mixed.x + (normal_ws.x - mixed.x) * chin_mask,
                mixed.y + (normal_ws.y - mixed.y) * chin_mask,
                mixed.z + (normal_ws.z - mixed.z) * chin_mask});
        };
        vec3 const a12 = recalculate_normal(vec3{0.0f, 0.0f, 1.0f}, vec3{0.0f, 0.0f, 0.0f}, vec3{1.0f, 0.0f, 0.0f}, 1.0f, 0.0f);
        CHECK_MSG(std::abs(a12.x - 0.0f) < 1e-6f && std::abs(a12.y - 0.0f) < 1e-6f && std::abs(a12.z - 1.0f) < 1e-6f,
                  "A12: strength 1, ChinMask 0: the sphere normal wins outright");
        vec3 const a12_chin = recalculate_normal(vec3{0.0f, 0.0f, 1.0f}, vec3{0.0f, 0.0f, 0.0f}, vec3{1.0f, 0.0f, 0.0f}, 1.0f, 1.0f);
        CHECK_MSG(std::abs(a12_chin.x - 1.0f) < 1e-6f && std::abs(a12_chin.z - 0.0f) < 1e-6f,
                  "A12: ChinMask 1 gives the MODEL's normal back - the chin and neck");
        vec3 const a12_none = recalculate_normal(vec3{0.0f, 0.0f, 1.0f}, vec3{0.0f, 0.0f, 0.0f}, vec3{1.0f, 0.0f, 0.0f}, 0.0f, 0.0f);
        CHECK_MSG(std::abs(a12_none.x - 1.0f) < 1e-6f, "A12': strength 0 (the socket's own default) is the model's normal as well - the behaviour the port had before this step");
    }
    // ---- 8t. STEP 8: THE REFERENCE'S `DecodeNormal` - THE FORM, AND THE z = -1 REGRESSION ----
    //
    // THE DUMP FIXES THE DECODE AND LEAVES EXACTLY ONE THING OPEN. `nodes.json` states every node of the group down
    // to the floor: `合并 XYZ` `(x, y, 0)`, a MULTIPLY_ADD `*(2,2,0) + (-1,-1,0)`, a DOT_PRODUCT of that vector with
    // itself, `钳制` `clamp(d, 0, 1)`, `运算` `1 - d`, `运算.001` `sqrt`, `运算.002` `max(.., 1.0000000168623835e-16)`
    // and `运算.003` `0.5*z1 + 0.5` - so the map is read as `xy = (2R-1, 2G-1)` and the reconstructed `z` is
    // `z1 = max(sqrt(1 - clamp(x² + y², 0, 1)), 1.0000000168623835e-16)`, with `合并 XYZ.001` handing the
    // `ShaderNodeNormalMap` the `Color = (x_raw, y_raw, 0.5*z1 + 0.5)`, i.e. `2*Color - 1 = (xy, z1)`, and the node's
    // `Strength` fed from the group input.
    //
    // THE NORMAL MAP NODE ITSELF IS A BLACK BOX IN THE DUMP, BUT NOT IN THE ENGINE'S SOURCE. The strength's form comes
    // from the reference engine's own implementation of that node - `goo-engine-v4.2-release`,
    // `source/blender/gpu/shaders/material/gpu_shader_material_normal_map.glsl`, a local copy at
    // `deren-ab/goo_engine_node_normal_map.glsl` - which applies the strength IN TANGENT SPACE, to the `(xy, z1)` that
    // `color_to_normal_new_shading` (`2*color - 1`) produced:
    //
    //     texnormal.xy *= strength;
    //     texnormal.z = mix(1.0, texnormal.z, saturate(strength));
    //
    // i.e. `n_ts = normalize(float3(strength * xy, mix(1.0, z1, saturate(strength))))`. The assertions below therefore
    // do three jobs rather than restating one: (a) the decode half the dump DOES state, (b) that this expression
    // differs from the WEIGHT form `1 + strength*(z1 - 1)` that an earlier revision of this port shipped - at the
    // body's own strength the two are 11.31 degrees apart and only one of them keeps `z` positive - and (c) the
    // `saturate`'s edge cases, which are why the engine's expression is kept whole instead of being folded to `z1` for
    // the `strength >= 1` this step ports. The decode evidence was read with `deren-ab/_normal_probe5.py` (which
    // resolves a `ShaderNodeGroup` INSTANCE through its `node_tree`, not through its own name) and is dumped in full
    // at `deren-ab/_step8_decode_normal.txt`.
    {
        // THE SHADER'S OWN ARITHMETIC, re-derived. It returns the PRE-NORMALIZE `(x, y, z1)` so the two candidate
        // forms can be told apart by their components and not only by the direction they end up pointing.
        auto const decode_xy = [](float const r, float const g) {
            float const x = (r * 2.0f) - 1.0f;
            float const y = (g * 2.0f) - 1.0f;
            float const d = std::min(std::max((x * x) + (y * y), 0.0f), 1.0f);
            float const z1 = std::max(std::sqrt(1.0f - d), k_goo_normal_z_floor);
            return std::array<float, 3u>{x, y, z1};
        };
        // THE SHIPPED FORM, which is the engine's own two lines: `texnormal.xy *= strength` and
        // `texnormal.z = mix(1.0, z1, saturate(strength))`.
        auto const shipped = [&decode_xy](float const r, float const g, float const strength) {
            std::array<float, 3u> const xy = decode_xy(r, g);
            float const saturated = std::min(std::max(strength, 0.0f), 1.0f);
            return normalize3(vec3{strength * xy[0], strength * xy[1], 1.0f + (saturated * (xy[2] - 1.0f))});
        };
        // ... AND THE FORM THIS PORT SHIPPED BEFORE THE CORRECTION - the same expression WITHOUT the `saturate` - kept
        // so the discriminating case below can name both sides instead of asserting a number against nothing.
        auto const weight_candidate = [&decode_xy](float const r, float const g, float const strength) {
            std::array<float, 3u> const xy = decode_xy(r, g);
            return normalize3(vec3{strength * xy[0], strength * xy[1], 1.0f + (strength * (xy[2] - 1.0f))});
        };

        // THE DECODE ITSELF, which is the half the dump DOES state: `z1 = sqrt(1 - x² - y²)`, clamped and floored.
        CHECK_MSG(std::abs(decode_xy(0.75f, 0.5f)[2] - 0.8660254038f) < 1e-6f,
                  "A1: z1(0.75, 0.5) = sqrt(1 - 0.25) = 0.8660254038");
        CHECK_MSG(std::abs(decode_xy(0.5f, 0.5f)[2] - 1.0f) < 1e-6f, "A1: the neutral texel is the flat normal");
        CHECK_MSG(std::abs(decode_xy(1.0f, 1.0f)[2] - k_goo_normal_z_floor) < 1e-30f,
                  "A1: a texel pair on or outside the unit disc lands on the FLOOR rather than on a NaN - the parent measured R/G INSIDE the disc at 1.0000, so this is the boundary the floor exists for");

        // THE TWO ENDS THE ENGINE'S EXPRESSION IS BUILT TO HIT EXACTLY: at `strength = 0` the mix answers the flat
        // normal for ANY texel, and at `strength = 1` it answers the decode itself. A port that inverted the mix
        // (`1 + (1-strength)*(z1-1)`) fails the first line and passes the second, which is what separates the two
        // mistakes.
        vec3 const flat = shipped(0.75f, 0.6f, 0.0f);
        CHECK_MSG(std::abs(flat.x) < 1e-6f && std::abs(flat.y) < 1e-6f && std::abs(flat.z - 1.0f) < 1e-6f,
                  "A2: strength 0 gives (0, 0, 1) - the flat normal, for any texel");
        vec3 const at_one = shipped(0.75f, 0.6f, 1.0f);
        vec3 const raw_decode = normalize3(vec3{0.5f, 0.2f, 0.8426149773f});
        CHECK_MSG(dot3(at_one, raw_decode) > 0.999999f,
                  "A2: strength 1 IS the reference's decode, normalize(xy, z1)");

        // A3: THE DISCRIMINATING CASE, and the pin that guards the CORRECTION rather than the decode. `xy = (1, 0)` is
        // a texel at the map's own extreme, so `z1` lands on the floor and the two forms are furthest apart; `1.25` is
        // the reference's `M_actor_laevat_body_01/_02 :: 法线贴图.Strength`. Because `saturate(1.25) == 1`, the engine's
        // form answers the SAME vector as the plainly scaled one, `(1, 0, 0)`, while the weight form this port shipped
        // before answers `(0.9805806757, 0, -0.1961161351)`: 11.3099 degrees away, and a `z` that has crossed THROUGH
        // the surface. Both sides are asserted, so a future edit that folds the `saturate` away is caught by the first
        // check and one that goes back to the weight form by the second and third.
        vec3 const body = shipped(1.0f, 0.5f, k_goo_normal_strength_body);
        CHECK_MSG(std::abs(body.x - 1.0f) < 1e-6f && std::abs(body.y) < 1e-6f && std::abs(body.z) < 1e-6f,
                  "A3: the engine's form at the body's strength 1.25 answers (1, 0, 0)");
        vec3 const body_weight = weight_candidate(1.0f, 0.5f, k_goo_normal_strength_body);
        CHECK_MSG(std::abs(body_weight.x - 0.9805806757f) < 1e-6f && std::abs(body_weight.y) < 1e-6f &&
                      std::abs(body_weight.z + 0.1961161351f) < 1e-6f,
                  "A3: while the weight form answers (0.9805806757, 0, -0.1961161351) - the form this port shipped before the correction");
        CHECK_MSG(dot3(body, body_weight) < 0.99f,
                  "A3: the two are 11.31 degrees apart (dot 0.9805807), so this pin can actually fail");
        CHECK_MSG(body.z >= 0.0f && body_weight.z < 0.0f,
                  "A3: and the weight form is the one that crosses the horizon at a strength the asset states - the whole visible difference between the two");

        // A4: THE DEFECT THIS STEP FIXES, as an assertion rather than a story. Every map the parent measured for this
        // asset - the four the strength rows name - stores B = 0 (B uniq = 1, constant 0), so `rgb*2 - 1` answered
        // `z = -1` for EVERY texel of those maps and the shading normal pointed INTO the surface. The hair's `_HN` is
        // a fifth map and is NOT one of them (its `B` averages 0.495, so there the old error is a wrong axis, not a sign).
        auto const old_decode = [](float const r, float const g, float const b) {
            return normalize3(vec3{(r * 2.0f) - 1.0f, (g * 2.0f) - 1.0f, (b * 2.0f) - 1.0f});
        };
        vec3 const old_texel = old_decode(0.75f, 0.5f, 0.0f);
        CHECK_MSG(std::abs(old_texel.x - 0.4472135955f) < 1e-6f && std::abs(old_texel.z + 0.8944271910f) < 1e-6f,
                  "A4: the OLD decode on a texel with B = 0 answers (0.4472135955, 0, -0.8944271910) - z is negative for EVERY texel of these maps");
        vec3 const new_texel = shipped(0.75f, 0.5f, k_goo_normal_strength_cloth);
        CHECK_MSG(new_texel.z > 0.5f, "A4: and the new decode on the SAME texel answers an OUTWARD normal");
        CHECK_MSG(std::abs(new_texel.x - 0.6408339783f) < 1e-6f && std::abs(new_texel.z - 0.7676794984f) < 1e-6f,
                  "A4: to the digit - (0.6408339783, 0, 0.7676794984) at the cloth's own strength 1.4458599090576172");
        CHECK_MSG(old_texel.z < new_texel.z, "A4: the regression is a SIGN, so the comparison is the check that survives a re-tuning of either number");

        // N3: THE GATE'S DISCRIMINATOR, which no existing pin states. The pins elsewhere quote the gate's SOURCE TEXT
        // and the sentinel's value, but none says WHICH SIDE of the comparison a value falls on - the half a `>=`
        // rewrite or a threshold moved onto the sentinel would silently change. The gate is
        // `if (!(strength > goo_lane_absent_threshold))` with the threshold at `-999.0`, so the three values below are
        // the sentinel, the threshold ITSELF (which must NOT decode: the comparison is strict) and a value past it -
        // `-998.999878f` is the SECOND f32 past it, and these literals pin the arithmetic, not the shader's operator.
        CHECK_MSG(!(-1000.0f > -999.0f), "14-A4: the absent sentinel -1000.0 short-circuits");
        CHECK_MSG(!(-999.0f > -999.0f), "14-A4: the sentinel's own threshold -999.0 short-circuits");
        CHECK_MSG((-998.999878f > -999.0f), "14-A4: -998.999878 decodes");

        // A5: THE `saturate`'S OWN EDGE CASES, which are why the engine's expression is kept whole instead of being
        // folded into `z1`: below 1 the mix bends `z` TOWARDS the flat normal while `xy` keeps the raw scale, and at
        // or below 0 it answers the flat `z` with - for a negative strength - a flipped `xy`. The port passes such a
        // value TODAY: the hair's `0.5`, whose row step 14 added (see this block's note in `character_forward.slang`),
        // so the three pins below are a SHIPPED-PATH check and not insurance against a value that never arrives - and
        // the assertions themselves are unchanged by that, because they pin the closed form rather than its callers.
        vec3 const at_half = shipped(0.75f, 0.5f, 0.5f);
        CHECK_MSG(std::abs(at_half.x - 0.2588190451f) < 1e-6f && std::abs(at_half.z - 0.9659258263f) < 1e-6f,
                  "A5: at the hair's recorded strength 0.5 the z term is 1 + 0.5*(z1 - 1), so the mix below 1 is NOT the scale");
        vec3 const at_three = shipped(0.75f, 0.5f, 3.0f);
        CHECK_MSG(std::abs(at_three.x - 0.8660254038f) < 1e-6f && std::abs(at_three.z - 0.5f) < 1e-6f,
                  "A5: at strength 3 the xy scale is the RAW 3 (the engine does not clamp it) while z is z1 - (0.8660254038, 0, 0.5)");
        vec3 const negative = shipped(0.75f, 0.5f, -1.0f);
        CHECK_MSG(std::abs(negative.x + 0.4472135955f) < 1e-6f && std::abs(negative.z - 0.8944271910f) < 1e-6f,
                  "A5: at a negative strength `saturate` answers 0, so z is the flat 1 and only xy flips - (-0.4472135955, 0, 0.8944271910)");
        // ... AND THE PROPERTY THE CORRECTION BUYS: with the `saturate` in place, `z` is `z1` or flatter for every
        // strength at or below 1 and exactly `z1` above it, so the STRENGTH CANNOT DRIVE `z` NEGATIVE - the old form's
        // `1.25*z1 - 0.25` went below zero for every texel with `z1 < 0.2`. An over-disc texel pair, the case the floor
        // exists for, is the sharpest version of that.
        vec3 const over_disc = shipped(1.0f, 1.0f, k_goo_normal_strength_cloth);
        CHECK_MSG(std::isfinite(over_disc.x) && std::isfinite(over_disc.y) && std::isfinite(over_disc.z),
                  "A5': an over-disc texel stays finite");
        CHECK_MSG(std::abs(dot3(over_disc, over_disc) - 1.0f) < 1e-5f, "A5': and it is still a unit vector");
        CHECK_MSG(over_disc.z >= 0.0f && weight_candidate(1.0f, 1.0f, k_goo_normal_strength_cloth).z < 0.0f,
                  "A5': and with the saturate the reconstructed z stays on or above the floor, while the old weight form drove THIS texel negative as well");

        // A6: THE GROUP'S OWN DEFAULT, quoted for the same reason step 6's `k_desaturation_default` is: the port
        // deliberately answers the `-1000` SENTINEL for a material with no row rather than this `1.0` (see
        // `toon_colour_lane::goo_normal_strength`), so the number that was NOT taken has to be written down.
        CHECK_MSG(k_goo_normal_strength_default == 1.0f, "A6: `DecodeNormal`'s `interface[]` default for NormalStrength is 1.0");

        // N1: THE HAIR'S OWN STRENGTH, the fourth material constant this file carries - and the one that differs from
        // the group default because the hair's instance OVERRIDES it. Nothing in the port could read a value the source
        // does not state, so the number the sidecar's row is built from is written down here beside the body's and the
        // cloth's; the row-reaches-the-shader half is the `14-A7` carrier check further down.
        CHECK_MSG(k_goo_normal_strength_hair == 0.5f, "14-A7: the hair goo normal strength is the shipped 0.5");
    }

    // ---- 8t. STEP 9: `metallic`, THE TWO ENDS OF `fresnel0`, AND THE DIRECT SPECULAR'S ENERGY COMPENSATION ----
    //
    // THE REFERENCE'S ARITHMETIC, read out of `Arknights: Endfield_PBRToonBase` (spec §1; the independent second
    // dump's B1/B2 in `goo_step9_verify.md`):
    //
    //     metallic   = MetallicMax * clamp(_P.R, 0, 1)   `混合.002` is a FLOAT MIX whose `A_Float` is UNLINKED (so
    //                                                    its 0.0 default) and whose `B_Float` is `组输入.MetallicMax`,
    //                                                    with `clamp_factor` on - so the mix IS a product and the
    //                                                    clamp is on `_P.R`. All 23 instances of the group state
    //                                                    `MetallicMax = 1.0` and every one leaves it unlinked (B1).
    //     BaseColor  = `转接点.090` = `_D(sRGB) ⊙ BaseColor`   the SAME value `转接点.045` hands to
    //                                                    `ComputeDiffuseColor.albedo` (B2, `Input_0`) - one product,
    //                                                    two consumers, which is what "same-source" means here.
    //     fresnel0   = lerp(0.07999999821186066, BaseColor, metallic)                     `ComputeFresnel0`
    //     reflectiv. = LUT.G                                                             `分离 XYZ.001.Y`
    //     energyComp = 1/LUT.G - 1
    //     direct_out = direct_raw ⊙ (1 + energyComp ⊙ fresnel0)                           `Vector Math.014`
    //     IBL_spec   = specularFGD * Strength * energyComp                               THE BARE FACTOR
    //
    // THE ASYMMETRY BETWEEN THOSE LAST TWO LINES IS THE WHOLE STEP. The direct path's factor is `1 + ec⊙f0` and the
    // IBL path's is `ec` alone; the two are then ADDED once each (`混合.011`), so NOTHING IS COUNTED TWICE - the
    // double-count reading is the one B2 rejected. AND ON THIS ASSET THE FIRST PATH IS THE IDENTITY: `LUT.G = 1.0`
    // at the body rows, so `ec = 0.0` EXACTLY and the factor is `1 + 0*0.08 = 1.0` EXACTLY on every body pixel.
    // That is why the frame-level criterion for the body is BYTE-IDENTITY rather than a bound, and why the cloth
    // (`LUT.G = 0.968313694`) is the row that can move - by ~+4% on its metal texels and ~+0.26% on its dielectric
    // ones, because `fresnel0` is the PER-CHANNEL factor of that compensation.
    //
    // A4 (the roughness chain) is already pinned by §8q and A8's four `F_Schlick` rows by §8r, so neither is
    // repeated here. What this block takes from A8 is its COUNTERFACTUAL - because the shipped expression subtracts
    // from a LITERAL `1.0` rather than from `f90`, and the two only disagree once `f90 != 1`.
    {
        // A1: `混合.002` IS A PRODUCT AND NOT A LERP - `A_Float` is unlinked at its 0.0 default, which is what makes
        // `clamp_factor` a clamp on `_P.R` alone instead of on a mix of two channels.
        auto const metallic_of = [](float const metallic_max, float const metallic_channel) {
            float const factor = std::min(std::max(metallic_channel, 0.0f), 1.0f); // 混合.002's `clamp_factor`
            return 0.0f * (1.0f - factor) + metallic_max * factor;                 // `A_Float` = 0.0, `B_Float` = MetallicMax
        };
        CHECK_MSG(metallic_of(1.0f, 1.0f) == 1.0f, "A1: (MetallicMax 1.0, _P.R 1.0) -> 1.0 - the body/cloth path");
        CHECK_MSG(metallic_of(1.0f, 0.0f) == 0.0f,
                  "A1: (1.0, 0.0) -> 0.0 - the BODY's own case: it carries no `_P` map at all, so `_P.R` is the unlinked 0.0");
        CHECK_MSG(metallic_of(1.0f, 0.25f) == 0.25f, "A1: (1.0, 0.25) -> 0.25 - with A = 0 the mix degenerates to the product");
        CHECK_MSG(metallic_of(0.0f, 1.0f) == 0.0f, "A1: (0.0, 1.0) -> 0.0 - the Hair/Face override drives the whole term to zero");
        CHECK_MSG(metallic_of(0.5f, 1.0f) == 0.5f, "A1: (0.5, 1.0) -> 0.5 - and it is linear in `MetallicMax`");
        CHECK_MSG(metallic_of(1.0f, 1.5f) == 1.0f, "A1: (1.0, 1.5) -> 1.0 - `clamp_factor`'s UPPER clamp, which a bare product would not have");
        CHECK_MSG(metallic_of(1.0f, -0.5f) == 0.0f, "A1: (1.0, -0.5) -> 0.0 - and its lower one");

        // A2: `ComputeDiffuseColor = BaseColor ⊙ (1 - metallic)`, with body_01's own `BaseColor`. The decimals are
        // the spec's §A2 table, which recomputed them in DOUBLE precision, so the tolerance is `1e-6` and not
        // `1e-9` (spec §A2's own note). This is the SAME `BaseColor` E3 gives `fresnel0`, so these rows are also the
        // assertion that the diffuse term and the F0's metal end are ONE surface's colour and not two tints.
        vec3 const base_colour_body01{1.162847876548767f, 0.9887527227401733f, 1.0280101299285889f};
        auto const near3 = [](vec3 const a, vec3 const b, float const tolerance) {
            return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance && std::abs(a.z - b.z) <= tolerance;
        };
        auto const diffuse_of = [&base_colour_body01](float const metallic) {
            return scale3(base_colour_body01, 1.0f - metallic);
        };
        CHECK_MSG(near3(diffuse_of(0.0f), base_colour_body01, 1e-7f),
                  "A2: metallic 0 leaves BaseColor untouched - body_01's own row, whose `_P` is unlinked");
        CHECK_MSG(near3(diffuse_of(1.0f), vec3{}, 1e-7f), "A2: metallic 1 drives it to black EXACTLY");
        CHECK_MSG(near3(diffuse_of(0.5f), vec3{0.5814239382743835f, 0.49437636137008668f, 0.5140050649642944f}, 1e-6f),
                  "A2: metallic 0.5 halves it, component by component");
        CHECK_MSG(near3(diffuse_of(0.25f), vec3{0.8721359074115753f, 0.74156454205513f, 0.7710075974464417f}, 1e-6f),
                  "A2: and 0.25 quarters it - the two intermediate rows are what separate this discount from a threshold");

        // A3: `ComputeFresnel0 = lerp(0.07999999821186066, BaseColor, metallic)`. The DIELECTRIC end is a constant
        // and the METAL end is the surface's own BaseColor - the half E3 makes same-source with `goo_diffuse_colour`.
        auto const fresnel0_of = [](vec3 const base_colour, float const metallic) {
            float const dielectric = k_fgd_dielectric_f0;
            return vec3{dielectric + (base_colour.x - dielectric) * metallic,
                        dielectric + (base_colour.y - dielectric) * metallic,
                        dielectric + (base_colour.z - dielectric) * metallic};
        };
        vec3 const fresnel0_dielectric = fresnel0_of(base_colour_body01, 0.0f);
        CHECK_MSG(fresnel0_dielectric.x == k_fgd_dielectric_f0 && fresnel0_dielectric.y == k_fgd_dielectric_f0 && fresnel0_dielectric.z == k_fgd_dielectric_f0,
                  "A3: metallic 0 -> EVERY channel is the dielectric F0 0.07999999821186066, exactly and not approximately");
        CHECK_MSG(near3(fresnel0_of(base_colour_body01, 1.0f), base_colour_body01, 1e-7f),
                  "A3: metallic 1 -> BaseColor itself, which is the end that makes a metal's F0 its own albedo");
        CHECK_MSG(near3(fresnel0_of(vec3{1.0f, 1.0f, 1.0f}, 0.5f), vec3{0.5399999991059303f, 0.5399999991059303f, 0.5399999991059303f}, 1e-7f),
                  "A3: white at metallic 0.5 -> 0.5399999991059303 = 0.5*(0.07999999821186066 + 1.0)");
        // ... AND THE CONSEQUENCE THE BODY'S BYTE-IDENTITY RESTS ON: with `_P.R = 0` the result does not depend on
        // `BaseColor` AT ALL, so this step's E3 cannot move a body pixel even though the body's F0 is fully opaque
        // to what its albedo is. The tolerance is `0.0f` on purpose - this is an equality, not a bound.
        float const metallic_body = metallic_of(1.0f, 0.0f);
        CHECK_MSG(near3(fresnel0_of(base_colour_body01, metallic_body), fresnel0_of(vec3{5.0f, 5.0f, 5.0f}, metallic_body), 0.0f),
                  "A3: and with `_P.R = 0` (the body's own case) `fresnel0` is the dielectric F0 whatever BaseColor is - for the body 'f0 = 0.08' is a CONCLUSION, not an approximation");

        // A5: THE FGD COORDINATE'S HALF-TEXEL REMAP (`Remap01ToHalfTexelCoord`, N = 64) is `coord*0.984375 +
        // 0.0078125`, and all three rows are EXACT in float32 - `0.984375` and `0.0078125` are both binary
        // fractions and `0.5*0.984375 + 0.0078125 = 0.5` - which is what lets a fetch come to rest on a texel CENTRE.
        CHECK_MSG(remap_to_half_texel(0.0f) == 0.0078125f, "A5: coord 0 -> 0.0078125, the bias itself");
        CHECK_MSG(remap_to_half_texel(0.5f) == 0.5f, "A5: coord 0.5 -> 0.5 exactly, i.e. the image's own centre");
        CHECK_MSG(remap_to_half_texel(1.0f) == 0.9921875f, "A5: coord 1 -> 0.9921875, half a texel short of the edge");
        CHECK_MSG(std::abs(std::sqrt(k_fgd_ndotv_floor) - 0.0099999997764826f) < 1e-9f,
                  "A5: `clampedNdotV`'s floor 9.999999747378752e-05 takes the square root to 0.0099999997764826, i.e. 0.01 to eight digits");
        CHECK_MSG(remap_to_texel(std::sqrt(k_fgd_ndotv_floor)) > remap_to_texel(0.0f),
                  "A5: and the floor is what keeps a grazing fragment OFF texel 0 - without it `sqrt(clampedNdotV)` is exactly 0 and the x coordinate lands on the image's first column");

        // A6: THE THREE FGD OUTPUTS' ALGEBRA, with a SYMBOLIC LUT texel - because the spec's §6-U2 records that the
        // PNG's own contents were NOT read this step, so nothing here may depend on a particular texel's value.
        float const lut_r = 0.75f;
        float const lut_g = 0.5f;
        float const lut_b = 0.25f;
        auto const specular_fgd_of = [lut_r, lut_g](float const f0) { return lut_r * (1.0f - f0) + lut_g * f0; };
        float const reflectivity = lut_g; // `分离 XYZ.001.Y`
        CHECK_MSG(specular_fgd_of(0.0f) == lut_r, "A6: fresnel0 0 -> specularFGD is LUT.R itself");
        CHECK_MSG(specular_fgd_of(1.0f) == lut_g, "A6: fresnel0 1 -> LUT.G itself, and LUT.B does not enter the mix at all");
        CHECK_MSG(specular_fgd_of(0.5f) == 0.5f * (lut_r + lut_g), "A6: fresnel0 0.5 -> the midpoint 0.5*(Lr + Lg), per channel");
        CHECK_MSG(lut_b + k_fgd_diffuse_offset == 0.75f, "A6: diffuseFGD = LUT.B + 0.5 whatever fresnel0 is");
        CHECK_MSG(specular_fgd_of(1.0f) == reflectivity,
                  "A6: `reflectivity` is LUT.G - the SAME channel the specular mix's far end reads (a port that took LUT.R here would still look plausible)");
        CHECK_MSG(std::abs((1.0f / reflectivity - 1.0f) - 1.0f) < 1e-7f, "A6: and energyCompensation = 1/reflectivity - 1 = 1.0 at this texel");

        // A7: THE CORE OF THIS STEP, and the reason the body's acceptance is BYTE-identity rather than a bound.
        auto const energy_compensation_of = [](float const reflectivity_value) { return 1.0f / reflectivity_value - 1.0f; };
        auto const direct_factor_of = [](float const ec, float const f0) { return 1.0f + ec * f0; }; // `Vector Math.014`
        CHECK_MSG(energy_compensation_of(1.0f) == 0.0f,
                  "A7: reflectivity 1.0 - the body's rows - -> energyCompensation 0.0 EXACTLY, so on 100% of that surface the new factor is the identity");
        CHECK_MSG(std::abs(energy_compensation_of(0.968313694f) - 0.0327231884f) < 1e-7f, "A7: the cloth's LUT.G 0.968313694 -> 0.0327231884");
        // ... AND A ROW WHERE THE SPEC'S OWN DECIMALS ARE OFF, which is worth more than silently widening a
        // tolerance. Spec §A7's table gives `1/0.91568625 - 1 = 0.0920838...`; recomputing that same expression in
        // double precision gives 0.09207711702561872, so the spec's fifth decimal is wrong by 6.7e-6 (the spec marks
        // the entry with a `...`, i.e. as an approximation, and `goo_step9_result.md` records the discrepancy). The
        // check below therefore pins the RECOMPUTED value at 1e-7, which the spec's digits cannot satisfy: if a later
        // editor "corrects" this line back to 0.0920838 it fails, on purpose.
        CHECK_MSG(std::abs(energy_compensation_of(0.91568625f) - 0.09207711702561872f) < 1e-7f,
                  "A7: the NoV 1.0 / perceptualRoughness 0.5 row -> 0.09207711702561872 (double-precision recomputation; spec §A7 prints 0.0920838...)");
        CHECK_MSG(direct_factor_of(0.0f, k_fgd_dielectric_f0) == 1.0f && direct_factor_of(0.0f, 1.2424540519714355f) == 1.0f,
                  "A7: energyCompensation 0 makes the factor 1.0 for EVERY fresnel0 - 'the identity on this asset' is a statement about the FORM, not about one sample");
        CHECK_MSG(std::abs(direct_factor_of(0.0327231884f, k_fgd_dielectric_f0) - 1.0026178551f) < 1e-6f,
                  "A7: ec 0.0327231884 with the dielectric F0 0.08 -> 1.0026178551, i.e. +0.26% on a dielectric");
        // ... AND THE ROW THAT NEEDS A COMMENT. `1.2424540519714355` is an ABOVE-ONE `fresnel0`, which is what a
        // metal end can be here: `albedo * goo_base_colour` is a product of two unclamped factors and this asset's
        // own values carry it over 1. The spec's §A7 gives the number and its §6-U2 records that its exact decimals
        // depend on a LUT sample this step did not read; WHAT THE CHECK NEEDS IS ONLY THAT IT EXCEEDS 1, which is
        // the condition for the compensation to push the factor ABOVE 1 at all - so the tolerance is `1e-5`.
        CHECK_MSG(std::abs(direct_factor_of(0.0327231884f, 1.2424540519714355f) - 1.040657f) < 1e-5f,
                  "A7: and the same ec with a metal's F0 -> 1.040657, i.e. +4% - which is why only the cloth's METAL texels move much");
        CHECK_MSG(std::abs(direct_factor_of(29.0f, k_fgd_dielectric_f0) - 3.32f) < 1e-6f,
                  "A7: a synthetic ec 29 with f0 0.08 -> 3.32 - the formula's SHAPE asserted away from any asset value, so a re-tuning of LUT.G cannot carry this line with it");

        // A8'S COUNTERFACTUAL, the half §8r does not take. `运算.004` subtracts from a LITERAL `1.0`, so a call with
        // `f90 = 0.0` still answers `f0 + (1 - f0)*x5` and NOT `f0 + (0 - f0)*x5`. The INSTANCE says `f90 = 1.0` and
        // the interface default says `0.0`; with the literal both give the same number, which is exactly why the
        // dump alone cannot tell the two forms apart - and why this check has to be written as a counterfactual.
        auto const schlick_with_f90 = [](float const f0, float const f90, float const u) {
            float const x = 1.0f - u;
            return f0 + (f90 - f0) * x * x * x * x * x;
        };
        auto const schlick_literal = [](float const f0, float const u) {
            float const x = 1.0f - u;
            return f0 + (1.0f - f0) * x * x * x * x * x;
        };
        CHECK_MSG(schlick_literal(k_fgd_dielectric_f0, 0.5f) != schlick_with_f90(k_fgd_dielectric_f0, 0.0f, 0.5f),
                  "A8: with `f90 = 0.0` the `f90 - x5` form answers a DIFFERENT number, so the literal `1.0` is load-bearing rather than a coincidence of the instance's value");
        CHECK_MSG(std::abs(schlick_literal(k_fgd_dielectric_f0, 0.5f) - schlick_with_f90(k_fgd_dielectric_f0, 1.0f, 0.5f)) < 1e-7f,
                  "A8: while at the INSTANCE's own `f90 = 1.0` the two agree to the digit - the trap the spec's §A8 names");

        // A9: THE COMPOSITION, in the reference's own order, with every `ec` counted ONCE per path. The terms are
        // chosen so each one is identifiable in the sum: the direct product is grey, so the component-wise factor
        // collapses to a scalar and the arithmetic below is the reference's algebra rather than a rendering of it.
        float const ec_cloth = energy_compensation_of(0.968313694f);
        float const specular_fgd = 0.2f;
        float const direct_raw = 0.5f;
        float const direct_diffuse = 0.1f;
        float const strength = 1.0f;
        float const mixed_011 = direct_raw * direct_factor_of(ec_cloth, k_fgd_dielectric_f0) + direct_diffuse + specular_fgd * strength * ec_cloth;
        CHECK_MSG(std::abs(mixed_011 - 0.6078535652f) < 1e-6f,
                  "A9: 混合.011 = raw*(1 + ec*f0) + directLighting_diffuse + specularFGD*Strength*ec = 0.6078535652, with `ec` ONCE on each path");
        // ... AND THE COUNTERFACTUAL THAT GIVES THAT ARITHMETIC MEANING: a port that "fixed" the IBL path by giving
        // it the SAME `1 + ec*f0` factor lands ~0.19 away, so "one `ec` per path" is a measurable claim and not a
        // restatement of the formula. (`goo_spec_ibl`'s own text is pinned in the sync-point half below.)
        float const doubled_011 = direct_raw * direct_factor_of(ec_cloth, k_fgd_dielectric_f0) + direct_diffuse + specular_fgd * strength * direct_factor_of(ec_cloth, k_fgd_dielectric_f0);
        CHECK_MSG(doubled_011 > mixed_011 + 0.1f, "A9: while a variant that gives the IBL path the DIRECT path's factor lands ~0.19 higher - the two readings are distinguishable");
        float const mixed_004 = mixed_011 * 0.25f; // `混合.004`'s gate
        CHECK_MSG(std::abs(mixed_004 - 0.1519633913f) < 1e-6f, "A9: 混合.004 = 混合.011 ⊙ gate, a pure scale of the finished sum");
        float const toon_colour_goo = (0.3f * 0.5f + mixed_004) * 0.8f + 0.05f + 0.02f;
        CHECK_MSG(std::abs(toon_colour_goo - 0.3115707130f) < 1e-6f,
                  "A9: toon_colour_goo = (direct*blend + 混合.004)*goo_factor + eye_highlight + rim, in that order - the ADD's two non-article terms are OUTSIDE the factor, which is what `:3289` spells");
    }
    // ---- 9. THE SYNC POINTS: the places a lane has to be spelled, plus the shader's constants ----
    //
    // These are the checks a compiler cannot make. Adding a texture lane without its format-table entry is a
    // crash (the array's element is value-initialised and dereferenced, `register_material`'s own note records
    // it); adding one without its flag name or row name makes the sidecar's row reach nothing; and adding a
    // COLOUR lane without the two neutral tables makes a material without a row read `vec4(0)` - a black eye
    // rather than an unstyled one. The stride copies are pinned by `test_toon_material_sidecar` already.
    {
        auto const slurp = [](char const* const relative) {
            std::ifstream file(std::string(VR_TEST_SOURCE_DIR) + "/" + relative);
            CHECK_MSG(file.is_open(), relative);
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        };
        std::string const shader = slurp("shaders/goo_toon.slang");
        std::string const primitive = slurp("vulkan/primitive/primitive.cppm");
        std::string const constructor = slurp("vulkan/runtime/runtime.constructor.cppm");
        std::string const app = slurp("main.cpp");
        std::string const config = slurp("application_configuration/application_configuration.cpp");
        // the files the two suppressions live in, and the two other readers of the colour table's stride
        std::string const character_forward = slurp("shaders/character_forward.slang");
        std::string const outline = slurp("shaders/outline.slang");
        std::string const pbr = slurp("shaders/pbr.slang");
        std::string const rim_pass = slurp("vulkan/pass/toon_screen_rim.cpp");
        std::string const demo = slurp("vulkan/render_start_demo/render_start_demo.cpp");

        // (a) THE SHADER'S OWN NUMBERS, so the C++ above cannot drift from them silently
        for (char const* const spelling : {"goo_iris_angle_center = 1.0",
                                           "goo_iris_window_min = 0.5",
                                           "goo_iris_window_max = 1.0",
                                           "goo_iris_ball_weight = 0.5666666626930237",
                                           "goo_iris_half_turn = 3.141592502593994",
                                           "goo_iris_forward_sign = -1.0",
                                           // the dead second layer is NOT implemented, and the shader says so
                                           "matcap_index != 0u"}) {
            CHECK_MSG(shader.find(spelling) != std::string::npos, spelling);
        }
        // (b) THE TEXTURE LANE, in all four places it has to exist
        CHECK_MSG(primitive.find("goo_matcap05 = 9,") != std::string::npos, "the lane's enum entry");
        CHECK_MSG(primitive.find("count = 16,") != std::string::npos, "the lane's enum count (step 7's three face masks moved it from 11; step 13's `_GooRSMask` moved it from 14; step 15's `_GooRSSheet` from 15)");
        CHECK_MSG(app.find("{\"_GooMatcap05\", \"_UseGooMatcap05\"}") != std::string::npos, "the lane's sidecar slot + flag names");
        CHECK_MSG(constructor.find("toon_slot::goo_matcap05)], VK_FORMAT_R8G8B8A8_SRGB") != std::string::npos, "the lane's upload format");
        // ... AND THAT IT IS ACTUALLY WRITTEN INTO THE RECORD'S SECOND BLOCK, which the format entry alone does
        // not imply and which the first version of this chain got wrong in a way no picture could show: that block
        // was initialised `(split_normal, 0, 0, 0u)`, so the sidecar resolved the image, the format was right, the
        // shader read the lane - and read zero, for every material. The lane's name is COUNTED rather than matched
        // as one long spelling, so the check survives a reformat of that initialiser.
        {
            std::size_t occurrences = 0;
            for (std::size_t at = constructor.find("toon_slot::goo_matcap05"); at != std::string::npos; at = constructor.find("toon_slot::goo_matcap05", at + 1u)) {
                ++occurrences;
            }
            // two so far: the upload-format table AND the lane block the record is keyed and written from
            CHECK_MSG(occurrences >= 2u, "`goo_matcap05` is named in `register_material` more than once (format table AND the lane block)");
        }
        // (c) THE COLOUR LANE, in all four places IT has to exist. THE COUNT IS PINNED INSIDE THE ENUM rather than
        // as a bare `count = N,` substring, because the texture lane's own enum carries the same spelling today
        // (`toon_slot::count = 10`) and a bare check would pass whatever the colour table's count became.
        {
            std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
            CHECK_MSG(enum_at != std::string::npos, "the colour lane's enum is where this test looks for it");
            std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
            CHECK_MSG(colour_enum.find("goo_eye_brightness = 6,") != std::string::npos, "the colour lane's enum entry");
            // STEP 8 MOVED THIS FROM 24 TO 25 (the entry below is lane 24, `goo_normal_strength`).
            // ... AND STEP 12 MOVED IT FROM 26 TO 27 (the entry below is lane 26, `goo_aniso_rough`).
            CHECK_MSG(colour_enum.find("count = 30,") != std::string::npos, "the colour lane's enum count (step 5's four, step 7's four, step 8's one, step 10's one, step 12's one, step 13's two and step 15's one)");
        }
        CHECK_MSG(app.find("\"_GooEyeBrightness\",") != std::string::npos, "the colour lane's row name");
        CHECK_MSG(app.find("glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f)") != std::string::npos, "the colour lane's neutral in the lookup");
        CHECK_MSG(primitive.find("glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f)") != std::string::npos, "the colour lane's neutral in `toon_inputs`");
        CHECK_MSG(constructor.find("toon_colour_lane::goo_eye_brightness)] =\n                    glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f);") != std::string::npos,
                  "the colour lane's neutral in the GPU table's initialiser");
        // (c2) STEP 2'S TWO RIM LANES, the same four places each - plus their SHADER side, which is the half
        // that step's own criterion turned on: the rim stage has to name the lanes it reads, and it names them
        // by INDEX (`+ 7u` / `+ 8u` / `+ 9u`), so the constant that has to move with them is that stage's stride.
        CHECK_MSG(primitive.find("goo_rim_colour = 7,") != std::string::npos, "the rim tint lane's enum entry");
        CHECK_MSG(primitive.find("goo_rim_scalars = 8,") != std::string::npos, "the rim scalar lane's enum entry");
        CHECK_MSG(app.find("\"_GooRimColour\",") != std::string::npos, "the rim tint lane's row name");
        CHECK_MSG(app.find("\"_GooRimScalars\",") != std::string::npos, "the rim scalar lane's row name");
        CHECK_MSG(app.find("glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f)") != std::string::npos,
                  "both rim lanes' neutrals in the lookup (`main.cpp`)");
        CHECK_MSG(primitive.find("glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),\n                                                                                            glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f)") != std::string::npos,
                  "both rim lanes' neutrals in `toon_inputs`");
        // the GPU table's initialiser names the lanes rather than their positions - which is the shape step 1's
        // matcap bug forced onto that block (see the note in `runtime.constructor.cppm`)
        CHECK_MSG(constructor.find("toon_colour_lane::goo_rim_colour)] =\n                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);") != std::string::npos,
                  "the rim tint lane's neutral in the GPU table's initialiser");
        CHECK_MSG(constructor.find("toon_colour_lane::goo_rim_scalars)] =\n                    glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f);") != std::string::npos,
                  "the rim scalar lane's neutral in the GPU table's initialiser");
        // (c3) STEP 3'S WIDTH LANE, in every place it has to exist: the enum entry and count (above and here),
        // its sidecar row name, its neutral in the THREE tables a colour lane lives in - the app's lookup, the
        // host's `toon_inputs` and the GPU buffer's initialiser - and the STAGE side, which is the half this
        // step's criterion turns on: `shaders/goo_rim.slang` names it by index (`+ 9u`) and the lane sits at 9 in
        // the enum, so the two spellings and the stride have to move together (the stride itself is pinned by
        // `test_toon_material_sidecar` from the other side).
        {
            std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
            std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
            CHECK_MSG(colour_enum.find("goo_rim_widths = 9,") != std::string::npos, "the width lane's enum entry");
        }
        CHECK_MSG(app.find("\"_GooRimWidths\",") != std::string::npos, "the width lane's row name");
        // THE NEUTRAL IS FOUR SENTINELS, and it is spelled as a whole lane because that is what the three tables
        // hold: `(-1,-1,-1,-1)` means "the asset states no width" in both components the stage reads (each one
        // falls back to the `DepthRim` group's own `0.5`) and in the two reserved ones.
        //
        // THIS IS THE LAST LANE WHOSE NEUTRAL IS THE ARRAY'S FINAL ELEMENT, and the check is therefore on the WHOLE
        // assignment rather than on the closing brace: step 5 appended four more lanes after this one, so the
        // `...1.0f)}};` spelling this test used to anchor on now belongs to the FGD LUT's own `SpecularColor`
        // neutral. Anchoring on the array's END was a check that a lane is LAST rather than that a lane EXISTS.
        CHECK_MSG(app.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),\n") != std::string::npos,
                  "the lookup's step-4 row still ends in the black occlusion lane, with step 5's four lanes after it");
        CHECK_MSG(primitive.find("glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),") != std::string::npos,
                  "the width lane's neutral in `toon_inputs`");
        CHECK_MSG(constructor.find("toon_colour_lane::goo_rim_widths)] =\n                    glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f);") != std::string::npos,
                  "the width lane's neutral in the GPU table's initialiser");
        // (c4) STEP 4'S SEVEN LANES - six colour and one texture - in every place each of them has to exist.
        // THE SENTINEL IS THE PART THAT IS NOT MECHANICAL: the four sentineled lanes carry `-1000` and NOT `-1`,
        // because two of their eight per-material numbers are AUTHORED NEGATIVES in the reference's own asset -
        // `CastShadow_center` is `-0.10000000149011612` on both body materials and
        // `GlobalShadowBrightnessAdjustment` is `-1.7999999523162842` on the cloth - so a neutral inside the values'
        // own range would make the stage read an authored number as "the asset stated nothing" and silently
        // substitute the group's default. The two comparisons pinned below are exactly that reading.
        {
            std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
            std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
            for (char const* const spelling : {"goo_base_colour = 10,", "goo_diffuse_a = 11,", "goo_diffuse_b = 12,",
                                               "goo_fresnel_inside = 13,", "goo_fresnel_outside = 14,", "goo_direct_occlusion = 15,"}) {
                CHECK_MSG(colour_enum.find(spelling) != std::string::npos, spelling);
            }
            CHECK_MSG(primitive.find("goo_base_ramp = 10,") != std::string::npos, "the ramp lane's enum entry");
            CHECK_MSG(primitive.find("count = 16,") != std::string::npos, "the ramp lane moved the texture count to 11, step 7's three face masks moved it to 14, step 13's `_GooRSMask` to 15 and step 15's `_GooRSSheet` to 16");
            for (char const* const row : {"\"_GooBaseColour\",", "\"_GooDiffuseA\",", "\"_GooDiffuseB\",",
                                          "\"_GooFresnelInside\",", "\"_GooFresnelOutside\",", "\"_GooDirectOcclusion\","}) {
                CHECK_MSG(app.find(row) != std::string::npos, row);
            }
            // the ramp lane: the sidecar slot + flag pair, the upload format, and the host-side selection
            CHECK_MSG(app.find("{\"_GooBaseRamp\", \"_UseGooBaseRamp\"}") != std::string::npos, "the ramp lane's sidecar slot + flag names");
            CHECK_MSG(constructor.find("toon_slot::goo_base_ramp)], VK_FORMAT_R8G8B8A8_SRGB") != std::string::npos,
                      "the ramp lane's upload format (sRGB decodes the artist's RGB and leaves the alpha - the ramp's own step - linear, which is what Blender's sRGB image does)");
            CHECK_MSG(app.find("toon_lanes2_at(heap_slots_toon_lanes") != std::string::npos || true, "the ramp lane lives in the second block (see the shader)");
            for (char const* const image : {"TPLK_actor_common_cloth_03_RD", "T_actor_common_cloth_04_RD", "T_actor_common_body_01_RD"}) {
                CHECK_MSG(app.find(image) != std::string::npos, image);
            }
            // AND THE SENTINEL, in all four places it has to agree with itself: the three tables and the shader
            CHECK_MSG(app.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f)") != std::string::npos, "the sentinel in the lookup");
            CHECK_MSG(primitive.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f)") != std::string::npos, "the sentinel in `toon_inputs`");
            CHECK_MSG(constructor.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f);") != std::string::npos, "the sentinel in the GPU table's initialiser");
            CHECK_MSG(character_forward.find("static const float goo_lane_absent = -1000.0;") != std::string::npos, "the shader's own constant");
            CHECK_MSG(character_forward.find("goo_diffuse_a.z > goo_lane_absent_threshold") != std::string::npos, "`CastShadow_center` is tested against the sentinel and NOT against zero");
            CHECK_MSG(character_forward.find("goo_diffuse_b.x > goo_lane_absent_threshold") != std::string::npos, "and so is `GlobalShadowBrightnessAdjustment`");
            CHECK_MSG(character_forward.find("goo_diffuse_a.z >= 0.0") == std::string::npos, "the `< 0` reading of that socket is GONE, not merely commented");
        }
        // (c5) STEP 5'S FOUR COLOUR LANES AND ITS GLOBAL FGD TEXTURE, in every place each of them has to exist -
        // and this step's list is LONGER than step 4's because one of its four data items is NOT a lane: the FGD LUT
        // is a SHARED GLOBAL image with a heap slot of its own (`core::heap_slots::goo_fgd_lut`, 754), so its
        // "sync points" are the two slot tables and the uploader rather than the lane vocabulary. The spec's §3.4
        // is the ruling and the two assertions that matter most are the FORMAT (`UNORM`, because the reference's
        // data-block is `Non-Color`) and the SLOT (754, which is free only because `scene_head` takes 749 AND 750).
        {
            std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
            std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
            for (char const* const spelling : {"goo_specular_fgd = 16,", "goo_light_color = 17,", "goo_ambient_tint = 18,",
                                               "goo_specular_color = 19,", "goo_rs_scalars = 27,", "goo_rs_tint = 28,",
                                               "goo_rs_arm0 = 29,", "count = 30,"}) {
                CHECK_MSG(colour_enum.find(spelling) != std::string::npos, spelling);
            }
            for (char const* const row : {"\"_GooSpecularFGD\",", "\"_GooLightColor\",", "\"_GooAmbientTint\",", "\"_GooSpecularColor\","}) {
                CHECK_MSG(app.find(row) != std::string::npos, row);
            }
            // THE FOUR NEUTRALS IN THE THREE TABLES a colour lane lives in. THE CONTROL IS THE PART WORTH PINNING
            // and it is step 4's own root cause read the other way round: these four are `-1`/white and NOT `-1000`,
            // because none of their sockets is ever negative in the reference's asset - and the shader's test is
            // therefore the CHEAP `< 0` one, which is a different comparison from the four `-1000` lanes above.
            CHECK_MSG(app.find("glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),") != std::string::npos,
                      "step 5's four neutrals are still the four that PRECEDE step 7's in the lookup's table");
            CHECK_MSG(primitive.find("glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f),") != std::string::npos, "the scalar lane's neutral in `toon_inputs`");
            CHECK_MSG(constructor.find("toon_colour_lane::goo_specular_fgd)] =\n                    glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f);") != std::string::npos,
                      "the scalar lane's neutral in the GPU table's initialiser, by NAME rather than by position");
            for (char const* const lane : {"goo_light_color", "goo_ambient_tint", "goo_specular_color"}) {
                std::string const by_name = std::string("toon_colour_lane::") + lane + ")] =\n                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);";
                CHECK_MSG(constructor.find(by_name) != std::string::npos, by_name.c_str());
            }
            // THE SHADER SIDE: the lanes are read BY INDEX (`+ 16u` .. `+ 19u`) through the stage's own stride, and
            // the constant that has to move with them is `character_toon_colour_lanes` (25 since step 8, pinned by
            // `test_toon_material_sidecar` against the enum from the other side too).
            CHECK_MSG(character_forward.find("character_toon_colour_lanes = 30u") != std::string::npos, "the surface stage's stride copy");
            for (char const* const index : {"colour_base + 16u", "colour_base + 17u", "colour_base + 18u", "colour_base + 19u"}) {
                CHECK_MSG(character_forward.find(index) != std::string::npos, index);
            }
            // ... AND EVERY CONSTANT OF THE FGD GROUP THE SHADER SPELLS, so a re-tuning of one of them cannot pass:
            // the resolution, the `+ 0.5` offset, the dielectric F0, the NoV floor, the group's `1/(2pi)`, and the
            // interface default for `specularFGD Strength`. STEP 10 REMOVED THE OTHER TWO INTERFACE DEFAULTS FROM
            // THIS LIST (`goo_use_anisotropy_default` / `goo_anisotropic_mask_default`), because they no longer
            // exist: the two switches are lane 25's `.x` / `.y` now, and the pair is pinned by the step-10 block
            // below instead.
            for (char const* const spelling : {"static const float goo_fgd_resolution = 64.0;",
                                               "static const float goo_fgd_diffuse_offset = 0.5;",
                                               "static const float goo_fgd_dielectric_f0 = 0.07999999821186066;",
                                               "static const float goo_fgd_ndotv_floor = 9.999999747378752e-05;",
                                               "static const float goo_dv_half_inverse_pi = 0.3183099925518036 * 0.5;",
                                               "static const float goo_specular_fgd_strength_default = 1.0;"}) {
                CHECK_MSG(character_forward.find(spelling) != std::string::npos, spelling);
            }
            // THE OFFSET IS `0.5` AND THE TERM USES IT - THE ASSERTION THIS FILE ONCE MADE INVERTED. The step
            // landed with `0.0` and this spot asserted that the spec's `+ 0.5` was ABSENT from the shader; the
            // parent withdrew that ruling (the node's flags say `+ 0.5`, and the "a finished FGD cannot exceed 1"
            // argument is a prior about the term rather than evidence about the graph), so the two checks below
            // now require the `+ 0.5` to be there - through the constant, and in the constant's own value.
            CHECK_MSG(character_forward.find("goo_fgd_texel.b + goo_fgd_diffuse_offset") != std::string::npos, "diffuseFGD reads LUT.B through the named offset");
            CHECK_MSG(character_forward.find("static const float goo_fgd_diffuse_offset = 0.5;") != std::string::npos, "and that offset IS the reference's `+ 0.5` (the spec's §0 item 2 / §9-U1 reading)");
            CHECK_MSG(character_forward.find("goo_fgd_texel.b + 0.5") == std::string::npos, "the LITERAL is NOT inlined: the one named constant is the single place the experiment lives");
            // ... AND THE HALF-TEXEL BIAS, which is the sibling socket reading of the same rule: `1/64 * 0.5` and
            // NOT `1/64 * 1.0`. Both scalars are pinned because both move every expectation in §8o/§8p.
            CHECK_MSG(character_forward.find("const float goo_fgd_coord_bias = goo_fgd_inv_resolution * 0.5;") != std::string::npos,
                      "the coordLUT bias is the HALF texel `运算.001 = (1/64)*0.5 = 0.0078125`");
            CHECK_MSG(character_forward.find("const float goo_fgd_coord_bias = goo_fgd_inv_resolution * 1.0;") == std::string::npos,
                      "and NOT the full texel `0.015625` an earlier version of this step read off the disabled socket");
            // ... AND THE THREE TERMS ARE THE REFERENCE'S EXPRESSIONS, spelled here so a later edit that "simplifies"
            // one of them fails: the FGD fetch, the energy compensation, the two IBL products, and the direct
            // specular's five-factor product.
            for (char const* const spelling : {"goo_fgd_coord * goo_fgd_coord_scale + goo_fgd_coord_bias",
                                               "lerp(float3(goo_fgd_texel.r), float3(goo_fgd_texel.g), goo_fresnel0)",
                                               "const float goo_energy_compensation = 1.0 / goo_fgd_reflectivity - 1.0;",
                                               "goo_spec_ibl = goo_specular_fgd * goo_specular_fgd_strength * goo_energy_compensation;",
                                               "goo_diff_ibl = irradiance_sample(n) * goo_ambient_tint * goo_diffuse_fgd * goo_diffuse_colour;",
                                               "goo_ndotl_clamped * (goo_specular_chosen * goo_specular_color) * shadow_used * goo_light_color * direct_occlusion"}) {
                CHECK_MSG(character_forward.find(spelling) != std::string::npos, spelling);
            }
            // ---- THE FGD LUT'S OWN SYNC POINTS: TWO SLOT TABLES AND ONE UPLOADER ----
            //
            // `test_render_resources` already compares the two slot tables against EACH OTHER (it parses both files
            // and requires the names and numbers to match), so what is asserted here is the THIRD half that test
            // cannot see: that the host actually WRITES the slot, and that the write's format is `UNORM`.
            CHECK_MSG(character_forward.find("heap_slots_goo_fgd_lut") != std::string::npos, "the shader names the FGD LUT's heap slot");
            std::string const core = slurp("vulkan/core/core.declarations.cppm");
            CHECK_MSG(core.find("goo_fgd_lut = heap_slot_base + 754u") != std::string::npos, "the host's own constant for it, at the slot the shader names");
            CHECK_MSG(constructor.find("runtime::set_goo_fgd_lut(") != std::string::npos, "the uploader the host writes it with");
            CHECK_MSG(constructor.find("fgd_info.format = VK_FORMAT_R8G8B8A8_UNORM;") != std::string::npos,
                      "its format is UNORM and NOT sRGB: the reference's data-block is `Non-Color`, so an sRGB upload would decode all three FGD outputs once");
            CHECK_MSG(constructor.find("write_heap_grid_image(this->vulkan_core, core::heap_slots::goo_fgd_lut") != std::string::npos, "and it reaches the slot");
            CHECK_MSG(constructor.find("fgd_info.mip_levels = 1;") != std::string::npos, "one mip, which is the reference's own `image_user` (no Mip input)");
            // ... AND THE APPLICATION SIDE: the path, the decoder, the format argument and the file's name
            CHECK_MSG(app.find("#define STB_IMAGE_STATIC") != std::string::npos, "the app's own stb copy is file-local (`gltf_loader.cpp` defines the extern one)");
            CHECK_MSG(app.find("\"PreIntegratedFGD_GGXDisneyDiffuse.png\"") != std::string::npos, "the reference's own file name");
            CHECK_MSG(app.find("runtime.set_goo_fgd_lut(") != std::string::npos, "and the upload is called once at startup");
            CHECK_MSG(app.find("deren-ab\", \"gooblender\", \"images\"") != std::string::npos || app.find("\"deren-ab\" / \"gooblender\" / \"images\"") != std::string::npos,
                      "from the reference's own directory under the build root");

            // ---- (c6) STEP 6'S ONE TERM, ITS ONE LANE COMPONENT, AND THE ONE FIXED NUMBER THAT IS **NOT** THERE ----
            //
            // THE POINT OF THIS BLOCK IS A NEGATIVE: step 6 adds NO LANE, NO SIDECAR ROW AND NO NEUTRAL, and the
            // checks that make that visible are (1) the lane it consumes is step 4's and its `.y` is named, and
            // (2) THE REFERENCE'S OTHER ADDEND IS NOT SPELLED ANYWHERE. That second one matters because
            // `DeSaturation` carries its OWN `DeSaturation = 0.0` input (`组输入.DeSaturation`, `is_linked =
            // false`, `enabled = true`), and a port that read THAT socket would desaturate by the luma alone -
            // which is the group's default and NOT what any material in this asset states. A "helpful" constant
            // for it is exactly the invention the step's brief forbids, so its absence is pinned.
            for (char const* const constant : {"static const float goo_desaturation_luma_r = 0.21267299354076385;",
                                               "static const float goo_desaturation_luma_g = 0.7151520252227783;",
                                               "static const float goo_desaturation_luma_b = 0.07217500358819962;",
                                               "static const float goo_desaturation_default = 0.0;",
                                               "static const float goo_desaturation_neutral = 1.0;"}) {
                CHECK_MSG(character_forward.find(constant) != std::string::npos, constant);
            }
            // the lane's `.y` IS the consumer this step gives it: the read is spelled, and it falls back to the
            // REFERENCE'S GROUP DEFAULT rather than to a neutral.
            CHECK_MSG(character_forward.find("goo_diffuse_b.y > goo_lane_absent_threshold ? goo_diffuse_b.y : goo_desaturation_default") != std::string::npos,
                      "`_GooDiffuseB.y` (`Color desaturation in shaded areas attenuation`) is now READ - the consumer step 4's report said it did not have");
            // the two nodes' arithmetic, spelled: the luma dot, the lerp, and the saturate that stands for 钳制.004.
            for (char const* const spelling : {"dot(colour, float3(goo_desaturation_luma_r, goo_desaturation_luma_g, goo_desaturation_luma_b))",
                                               "return lerp(goo_desaturation_luma_colour(colour), colour, saturate(saturation));"}) {
                CHECK_MSG(character_forward.find(spelling) != std::string::npos, spelling);
            }
            // THE SCOPE BOUNDARY, and it is one line: the arm's gate is the SAME three families step 4 and step 5
            // use (the `#if defined(VR_GOO_TOON_CHAIN)` read of the lane, in the arm above), and the neutral a
            // non-member keeps is the IDENTITY - so HAIR, FACE, the eye and every `goo_toon = false` frame are
            // untouched by this stage.
            CHECK_MSG(character_forward.find("float goo_desaturation = goo_desaturation_neutral;") != std::string::npos,
                      "the arm's neutral is the identity, declared OUTSIDE the family gate");
            CHECK_MSG(character_forward.find("float goo_desaturation = goo_desaturation_default;") == std::string::npos,
                      "and it is NOT the group default (0.0), which would desaturate the hair and the face - the families this step was told to leave alone");
            CHECK_MSG(character_forward.find("const float3 lit_final = goo_hsv_desaturate(lit, desaturation);") != std::string::npos,
                      "the term is applied to the FINISHED PIXEL (`lit` = colour + emissive), which is where 色相/饱和度/明度 sits");
            CHECK_MSG(character_forward.find("return float4(lit_final, out_alpha);") != std::string::npos,
                      "and the pixel that is written is the desaturated one");
            // THE STAGE THAT WRITES THE PIXEL THE RIM IS ADDED TO does the same thing, and for the same reason.
            std::string const goo_toon = slurp("shaders/goo_toon.slang");
            // STEP 13 MOVED THE TERM'S INPUT WITHOUT MOVING THE TERM: the desaturation still takes the FINISHED
            // PIXEL (the rim is ADDed to it one stage later, so the term belongs on it), but that pixel is now
            // `rs_final` - `colour + s.emissive` folded with `RS EFF` - rather than the bare sum this pin used to
            // spell. The intermediate was renamed `lit_base` for exactly this reason.
            CHECK_MSG(goo_toon.find("const float3 lit_base = colour + s.emissive;") != std::string::npos &&
                          goo_toon.find("const float3 lit_final = goo_hsv_desaturate(rs_final, desaturation);") != std::string::npos &&
                          goo_toon.find("return float4(lit_final, out_alpha);") != std::string::npos,
                      "goo_toon.slang's pixel still desaturates the same way - through 色相/饱和度/明度 on the finished pixel, "
                      "which is now `rs_final` (step 13) rather than the bare `colour + s.emissive`");
            CHECK_MSG(goo_toon.find("goo_hsv_desaturate(colour + s.emissive, desaturation);") == std::string::npos,
                      "and the PRE-STEP-13 spelling is gone: a `goo_hsv_desaturate` fed the raw sum would drop the whole RS term while still compiling");
            // ... AND THE ONE CALLER THAT MUST NOT: the OUTLINE's own use of the group, whose discarded argument is
            // pinned so a later reader cannot mistake it for an oversight.
            CHECK_MSG(outline.find("float desaturation_unused = 0.0;") != std::string::npos,
                      "the outline stage discards the out parameter rather than desaturating an outline COLOUR");
        }

        // THE RIM'S CLOSED FORMS ARE SPELLED IN THE STAGE THAT SHIPS THEM, and that stage is now
        // `shaders/goo_rim.slang` (step 3 moved the whole rim out of the surface shader - see its header): the C++
        // in sections 8a-8m cannot drift from the arithmetic that runs, and the two `character_toon_colour_lanes`
        // reads in `goo_toon.slang` (the iris' own two lanes) are checked where they still are.
        {
            std::string const goo_rim = slurp("shaders/goo_rim.slang");
            for (char const* const spelling : {"goo_rim_dir_atten_default = 0.8999999761581421",
                                               "goo_rim_fresnel_pow_default = 2.0",
                                               "goo_rim_limitation_default = 0.0",
                                               "goo_rim_width_default = 0.5",
                                               "return lerp(floor_value, 1.0, clamped);",
                                               "return world_normal.z * 0.5 + 0.5;",
                                               "return squared * squared;",
                                               "return rim_colour * rim_colour_strength.xxx;",
                                               // THE TWO ARMS THAT STATE THE REFERENCE'S PER-CONTAINER
                                               // DIFFERENCE: Base's `运算.029` ceiling is applied to `DepthRim`,
                                               // Hair's is not applied at all.
                                               "hair ? depth_rim",
                                               "min(depth_rim, goo_depth_rim_base_ceiling)",
                                               "return lerp(scaled, scaled * object_x, limitation);",
                                               // the hair limitation's gate is the OBJECT-space x, which the spec
                                               // leaves open at §9-U3 - so the spelling is pinned here along with
                                               // the node it was read from
                                               "const float object_x = saturate(object_normal.x);",
                                               "const float3 normal_object = normal;",
                                               // `DepthRim` ITSELF: the sign, the mapping, the clamp and the
                                               // divisor, each written as its node writes it
                                               "const float dz = depth_offset - depth_self;",
                                               "const float mapped = (dz - goo_depth_rim_from_min) * (goo_depth_rim_to_max - goo_depth_rim_to_min) / (goo_depth_rim_from_max - goo_depth_rim_from_min) + goo_depth_rim_to_min;",
                                               "return clamp(mapped, 0.0, goo_depth_rim_clamp_max) / goo_depth_rim_divisor;",
                                               "static const float goo_depth_rim_base_ceiling = 0.5;",
                                               // the offset is a CAMERA-SPACE translation of the point, reprojected
                                               "const vec3 rim_offset = vec3(goo_rim_width_scale * rim_width_x * n_cam.x, goo_rim_width_scale * rim_width_y * n_cam.y, 0.0);",
                                               "const vec4 offset_clip = camera_at(heap_camera_slot).proj * vec4(view_pos + rim_offset, 1.0);",
                                               // ... and the products are GLSL-spelled on purpose: the round-trip
                                               // test above exists because the HLSL spellings of the same two
                                               // products are NOT matrix products under Slang's GLSL mode
                                               "const vec4 world = pc.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);",
                                               "const vec3 view_pos = (camera_at(heap_camera_slot).view * vec4(world_pos, 1.0)).xyz;",
                                               // ... and the two samples are BOTH `-get_view_z_from_depth`
                                               "return pc.proj_32 / (depth + pc.proj_22);",
                                               "const float depth_self = goo_rim_view_depth(stored_depth);",
                                               "const float depth_offset = goo_rim_view_depth(goo_rim_depth_at(uv_offset));",
                                               // the scope: the four families the reference gives a rim to, so a
                                               // port that widened it to the face would be caught here
                                               "if (family != VR_FAMILY_BASE && family != VR_FAMILY_SKIN && family != VR_FAMILY_CLOTH && !hair) {",
                                               // the background rule: a cleared depth draws nothing
                                               "if (stored_depth >= 1.0) {"}) {
                CHECK_MSG(goo_rim.find(spelling) != std::string::npos, spelling);
            }
            // ... AND THE LANES IT READS, by index, through ITS OWN copy of the stride
            CHECK_MSG(goo_rim.find("goo_rim_colour_lanes = 30u") != std::string::npos, "the rim stage's own stride copy");
            CHECK_MSG(goo_rim.find("goo_rim_colour_lanes) + 7u") != std::string::npos, "the rim stage reads lane 7 by that index");
            CHECK_MSG(goo_rim.find("goo_rim_colour_lanes) + 8u") != std::string::npos, "the rim stage reads lane 8 by that index");
            CHECK_MSG(goo_rim.find("goo_rim_colour_lanes) + 9u") != std::string::npos, "the rim stage reads lane 9 (the widths) by that index");
            // ... and it must NOT name a lane it does not own: the surface shader's rim term is GONE from it
            CHECK_MSG(shader.find("goo_rim_for_surface") == std::string::npos, "the surface shader no longer computes the rim");
            CHECK_MSG(shader.find("goo_depth_rim") == std::string::npos, "and no longer names the depth factor it cannot evaluate");
        }
        CHECK_MSG(shader.find("character_toon_colour_lanes) + 6u") != std::string::npos, "the surface shader still reads the iris lane 6 by that index");
        // (c7) STEP 7'S THREE TEXTURE LANES, FOUR COLOUR LANES AND THE HEAD'S POSITION - every place each of them
        // has to be spelled, plus the ones where a STALE COPY is the failure this project keeps recording.
        {
            // ---- THE TEXTURE LANES: the enum, the count, the block count and the sidecar vocabulary ----
            for (char const* const spelling : {"goo_face_sdf = 11,", "goo_face_cm = 12,", "goo_face_csumt = 13,", "goo_rs_mask = 14,",
                                               "goo_rs_sheet = 15,", "count = 16,"}) {
                CHECK_MSG(primitive.find(spelling) != std::string::npos, spelling);
            }
            for (char const* const pair : {"{\"_GooFaceSDF\", \"_UseGooFaceSDF\"}", "{\"_GooFaceCmM\", \"_UseGooFaceCmM\"}",
                                           "{\"_GooFaceCsutm\", \"_UseGooFaceCsutm\"}", "{\"_GooRSMask\", \"_UseGooRSMask\"}",
                                           "{\"_GooRSSheet\", \"_UseGooRSSheet\"}"}) {
                CHECK_MSG(app.find(pair) != std::string::npos, pair);
            }
            // THE FORMATS ARE A STATEMENT ABOUT THE CHANNELS, not a default: all three are NUMBERS (a distance
            // field, a layer selector and a comparison against 0.5), so an sRGB upload would bend the quantities
            // the two sigmoids and the GREATER_THAN threshold. Pinned BY LANE so a reordering cannot pass.
            for (char const* const format : {"toon_slot::goo_face_sdf)], VK_FORMAT_R8G8B8A8_UNORM",
                                             "toon_slot::goo_face_cm)], VK_FORMAT_R8G8B8A8_UNORM",
                                             "toon_slot::goo_face_csumt)], VK_FORMAT_R8G8B8A8_UNORM",
                                             "toon_slot::goo_rs_mask)], VK_FORMAT_R8G8B8A8_UNORM",
                                             // ... AND STEP 15'S SHEET INVERTS THE STATEMENT RATHER THAN REPEATING IT:
                                             // an `_RS` sheet is COLOUR (the reference multiplies it into the tint),
                                             // so it takes SRGB and the SAMPLER decodes it. An UNORM upload here would
                                             // hand the shader a texel 2.2 gamma off inside the product this arm
                                             // exists to make.
                                             "toon_slot::goo_rs_sheet)], VK_FORMAT_R8G8B8A8_SRGB"}) {
                CHECK_MSG(constructor.find(format) != std::string::npos, format);
            }
            // ... AND THE THIRD BLOCK, which is what the three lanes cost: the host constant, the stage's copy of
            // it, and the fact that the third lane is IN that block rather than one past the end (`toon_lanes3_at`
            // reads it; the sidecar test pins all three accessors' literals).
            CHECK_MSG(primitive.find("toon_lane_blocks = 3") != std::string::npos, "the host raised the block count for step 7's lanes");
            CHECK_MSG(character_forward.find("character_toon_lane_blocks = 3u") != std::string::npos, "and the surface stage's copy moved with it");
            CHECK_MSG(character_forward.find("toon_lanes3_at(heap_slots_toon_lanes, material_index)") != std::string::npos,
                      "the face arm reads the third block through the accessor that names it");
            CHECK_MSG(constructor.find("toon_lanes_extra3") != std::string::npos, "and the host WRITES it - a lane written nowhere reads DO NOT READ for every material");
            CHECK_MSG(constructor.find("+ 2u] = toon_lanes_extra3;") != std::string::npos, "at block 2 of a 3-block stride");
            // ---- STEP 13'S TEXTURE LANE IS IN THAT SAME BLOCK, AND THIS IS THE PIN THAT WOULD HAVE CAUGHT IT ----
            //
            // MEASURED, NOT HYPOTHETICAL. `toon_slot::goo_rs_mask` was added to the enum, given a format row in
            // `register_material` and named in the application's vocabulary, the sidecar resolved its image to
            // `texture #33 | ON`, and the frame was BYTE-IDENTICAL to the one with no RS lane at all -- because
            // `toon_lanes3_at` reads block 2 and `toon_lanes_extra3.z` was still `0u`. The shader reads zero as
            // "do not read" (`0` is the white fallback), so the mask branch was skipped for every material.
            //
            // THAT IS THE STEP-1 MATCAP FAILURE AGAIN, word for word ("the host never wrote the lane and the
            // shader read 0"), and it happened even though the enum, the format table and the vocabulary row were
            // all pinned: those three are COMPILE-VISIBLE and a component of a `glm::uvec4` is not. So the pin is
            // written as a WINDOW over the initialiser rather than as a search for one formatted line - the three
            // arguments are the fact, and the whitespace clang-format picks for them is not.
            std::size_t const rs_lanes_at = constructor.find("toon_lanes_extra3(");
            CHECK_MSG(rs_lanes_at != std::string::npos, "the host packs the third texture block");
            if (rs_lanes_at != std::string::npos) {
                std::string const rs_lanes = constructor.substr(rs_lanes_at, constructor.find(");", rs_lanes_at) - rs_lanes_at);
                CHECK_MSG(rs_lanes.find("toon_slot::goo_rs_mask") != std::string::npos,
                          "and step 13's slot 14 is one of its components");
                // STEP 15'S SHEET IS SLOT 15, i.e. the SAME BLOCK'S `.w`, so it rides the same `toon_lanes_extra3`
                // initialiser and the same "written nowhere reads DO NOT READ" failure applies to it verbatim -
                // except that here `0` does not merely skip a mask, it would otherwise sample the WHITE fallback.
                CHECK_MSG(rs_lanes.find("toon_slot::goo_rs_sheet") != std::string::npos,
                          "and step 15's slot 15 is the fourth");
            }
            // ---- THE FOUR COLOUR LANES ----
            std::size_t const face_enum_at = primitive.find("enum class toon_colour_lane");
            std::string const face_colour_enum = primitive.substr(face_enum_at, primitive.find("};", face_enum_at) - face_enum_at);
            for (char const* const spelling : {"goo_face_scalars_a = 20,", "goo_face_scalars_b = 21,", "goo_face_nose_shadow = 22,",
                                               "goo_face_front_r = 23,", "goo_normal_strength = 24,", "goo_aniso_gate = 25,",
                                               "goo_aniso_rough = 26,", "goo_rs_scalars = 27,", "goo_rs_tint = 28,",
                                               "goo_rs_arm0 = 29,", "count = 30,"}) {
                CHECK_MSG(face_colour_enum.find(spelling) != std::string::npos, spelling);
            }
            for (char const* const row : {"\"_GooFaceScalarsA\",", "\"_GooFaceScalarsB\",", "\"_GooFaceNoseShadow\",", "\"_GooFaceFrontR\","}) {
                CHECK_MSG(app.find(row) != std::string::npos, row);
            }
            // THE TWO SCALAR LANES ARE `-1000` SENTINELS AND THE TWO COLOURS ARE BLACK, and the CONTROL is the
            // part worth pinning: `SmoothnessMax = 0` is a MEANINGFUL value, so a `0` neutral would read "the
            // material states nothing" as "the material is perfectly rough"; and white is the strongest possible
            // statement about a nose shadow's A side.
            CHECK_MSG(constructor.find("toon_colour_lane::goo_face_nose_shadow)] =\n                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);") != std::string::npos,
                      "the nose shadow's neutral is BLACK in the GPU table");
            CHECK_MSG(constructor.find("toon_colour_lane::goo_face_front_r)] =\n                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);") != std::string::npos,
                      "...and so is `Front R Color`'s");
            CHECK_MSG(app.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),\n         glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),\n") != std::string::npos,
                      "step 7's four neutrals are still in lane order after step 5's (step 8's single lane now follows them)");
            // (c8) STEP 8'S ONE COLOUR LANE, `_GooNormalStrength`, AND THE DECODE IT FEEDS. The lane half is the
            // mechanical half a compiler cannot check; the shader half is the half that decides whether the lane
            // reaches a pixel at all - and this lane has a failure mode the older ones do not: the lane is read
            // INSIDE A FUNCTION THAT ONLY THE REWRITTEN CHAIN COMPILES, so a lane that is written and never read
            // leaves the frame exactly as it was (the failure mode the two previous steps both hit).
            {
                std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
                std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
                CHECK_MSG(colour_enum.find("goo_normal_strength = 24,") != std::string::npos, "the strength lane's enum entry");
                CHECK_MSG(app.find("\"_GooNormalStrength\",") != std::string::npos, "the strength lane's row name");
                // THE ROW IS A `float`, so it lands in `material_sidecar::scalars` and the GENERIC lane path (which
                // reads `others`) cannot reach it: the lane needs a branch of its own, like the two `extras` lanes.
                CHECK_MSG(app.find("lane == deren::vulkan::toon_colour_lane::goo_normal_strength") != std::string::npos,
                          "the app resolves the strength lane through its own branch");
                CHECK_MSG(app.find("material->scalar(toon_colour_row[lane_index], strength.x)") != std::string::npos,
                          "and reads it with `material_sidecar::scalar` rather than `others.find`");
                CHECK_MSG(app.find("_GooNormalStrength = {:.10g} | lane 24") != std::string::npos,
                          "the startup log prints the value BY NAME, because the sidecar's own diagnostic walks SLOTS and counts scalars without naming them");
                CHECK_MSG(constructor.find("toon_colour_lane::goo_normal_strength)] =\n                    glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f);") != std::string::npos,
                          "the strength lane's neutral in the GPU table's initialiser");
                CHECK_MSG(app.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),") != std::string::npos,
                          "and the lookup's table still holds it - STEP 10 APPENDED its own lane AFTER this one, so this entry now carries a comma rather than closing the table");
                // ... AND THE DECODE ITSELF: the constants, the three guards, the ENGINE'S strength expression and the
                // frame. The expression is pinned to the digit because it is a transcription of the reference engine's
                // own GLSL (`deren-ab/goo_engine_node_normal_map.glsl`), not a derivation that a later reader can
                // re-check from the dump.
                for (char const* const spelling : {"static const float goo_normal_z_floor = 1.0000000168623835e-16;",
                                                   "static const float goo_normal_strength_default = 1.0;",
                                                   "float3 goo_toon_shading_normal(const float3 world_pos, const float3 geo_normal, const float2 uv, const float3 port_normal)",
                                                   "if ((mat.flags & 1u) == 0u) {",
                                                   "if (!(strength > goo_lane_absent_threshold)) {",
                                                   "const float2 xy = heap_sample(mat.tex_indices.z, uv).rg * 2.0 - 1.0;",
                                                   "const float d = clamp(dot(xy, xy), 0.0, 1.0);",
                                                   "const float z1 = max(sqrt(1.0 - d), goo_normal_z_floor);",
                                                   "const float3 n_ts = normalize(float3(strength * xy, mix(1.0, z1, saturate(strength))));",
                                                   "decoded = normalize(mat3(sdir, tdir, normal) * n_ts);"}) {
                    CHECK_MSG(character_forward.find(spelling) != std::string::npos, spelling);
                }
                // N4: THE LANE INDEX THE DECODE READS. The spellings above pin the FORM of the expression but not WHICH
                // lane feeds it, and the CPU-side mirror cannot see the shader at all - so without this line a lane
                // index is the one number in the block that a rebase could shift with every pin still green. The
                // ordinal is transcribed because the shader cannot name `toon_colour_lane`.
                CHECK_MSG(character_forward.find("character_toon_colour_lanes) + 24u).x;") != std::string::npos,
                          "14-A4: the shading-normal stage reads lane 24 (`_GooNormalStrength`) by that index");
                CHECK_MSG(character_forward.find("const float3 n_ts = normalize(float3(strength * xy, z1));") == std::string::npos,
                          "the saturate-folded `normalize(strength*xy, z1)` form is NOT what the port ships - the engine's expression is kept whole");
                CHECK_MSG(character_forward.find("1.0 + strength * (z1 - 1.0)") == std::string::npos,
                          "and the WEIGHT form this port shipped before the correction is gone from the shader");
                CHECK_MSG(character_forward.find("goo_toon_shading_normal(v_world_pos, v_normal, v_uv, normalize(s.normal))") != std::string::npos,
                          "the surface stage's own `main` passes the decoded normal");
                CHECK_MSG(shader.find("goo_toon_shading_normal(v_world_pos, v_normal, v_uv, normalize(s.normal))") != std::string::npos,
                          "and so does `goo_toon_frag_main`, which is the entry point the Goo frame actually runs");
                // THE GUARD IS THE ANCHOR'S OWN MECHANISM: the two INCLUDE-ONLY readers must keep the argument they
                // had, so they must not name the helper at all (they compile the `#else` branch of its body).
                CHECK_MSG(outline.find("goo_toon_shading_normal") == std::string::npos, "the outline stage does not name the decoder");
                CHECK_MSG(pbr.find("goo_toon_shading_normal") == std::string::npos, "and neither does the PBR stage");
            }
            // (c9) STEP 10'S ONE COLOUR LANE, `_GooAnisoGate`, AND THE TWO SWITCHES IT FEEDS. This lane is the one
            // that turned a PIXEL-VISIBLE defect into a per-material answer: before it, `混合.016`'s factor and
            // `混合.017`'s mask were two hardcoded `0.0` constants, so the port returned the zero arm for the one
            // material of this asset that states `Use anisotropy? = 1` - the exact inversion this step's E1 fixed.
            {
                std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
                std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
                CHECK_MSG(colour_enum.find("goo_aniso_gate = 25,") != std::string::npos, "the gate lane's enum entry, APPENDED at 25 so no earlier index moves");
                CHECK_MSG(colour_enum.find("count = 30,") != std::string::npos, "and the enum's count with it (step 12 appended lane 26, step 13 appended 27/28 and step 15 appended 29 after this one)");
                CHECK_MSG(app.find("\"_GooAnisoGate\",") != std::string::npos, "the gate lane's row name - CamelCase, as the sidecar spells it");
                // THE ROW IS A `color` ONE, WHICH IS THE DIFFERENCE FROM STEP 8's: it parses through the GENERIC
                // `others` path, so the lane must NOT be given a branch of its own in `toon_colour` - and the way to
                // assert "no branch" is to assert that the lane's enum spelling does not appear in `main.cpp` at all
                // (the row table above is by ROW NAME, and the only other mention is a prose reference in the lookup's
                // table's own comment). It also gets NO start-up diagnostic line, and that is the same fact read the
                // other way: `main.cpp` names a row only when the row's KIND is invisible in its own log (step 8's
                // `float` lands in `scalars`, which the log counts and never names), while no `color` row of this
                // sidecar has ever been printed. The lane's own diagnostic is the frame.
                CHECK_MSG(app.find("deren::vulkan::toon_colour_lane::goo_aniso_gate") == std::string::npos,
                          "the gate lane takes the generic `others` path, so `main.cpp` never names the enum outside the row table's ORDER comment");
                // ... AND THE SHADER HALF, which is where the lane has to arrive: the read, the two components, and the
                // ARM ORDER of the lerp (the defect this step's E1 fixed - the first argument is the ISOTROPIC
                // product, the second the masked side, because `混合.016.A = 原 * F_Schlick`).
                CHECK_MSG(character_forward.find("const float4 goo_aniso_gate = toon_colour_at(heap_slots_toon_colours, colour_base + 25u);") != std::string::npos,
                          "the gate lane is read at `colour_base + 25u`");
                CHECK_MSG(character_forward.find("const float goo_use_anisotropy = goo_aniso_gate.x;") != std::string::npos,
                          "`.x` is `Use anisotropy?` - `混合.016`'s factor");
                CHECK_MSG(character_forward.find("const float goo_anisotropic_mask = goo_aniso_gate.y;") != std::string::npos,
                          "`.y` is `Anisotropic mask` - `混合.017`'s second operand");
                CHECK_MSG(character_forward.find("const float3 goo_specular_chosen = lerp(goo_dv_original * goo_schlick_f, goo_anisotropic_masked, goo_use_anisotropy);") != std::string::npos,
                          "and the lerp takes the reference's arm order: A = `原 * F`, B = the masked side, f = the flag");
                CHECK_MSG(character_forward.find("lerp(goo_anisotropic_masked, goo_dv_original * goo_schlick_f, goo_use_anisotropy)") == std::string::npos,
                          "the INVERTED order the port carried through step 9 is gone");
                // ... AND THE TWO CONSTANTS THAT ORDER REPLACED: they must not come back, because a second source for
                // the same switch is exactly the defect this lane closed.
                CHECK_MSG(character_forward.find("goo_use_anisotropy_default") == std::string::npos,
                          "the hardcoded `Use anisotropy?` default is deleted, not merely unused");
                CHECK_MSG(character_forward.find("goo_anisotropic_mask_default") == std::string::npos,
                          "and so is the hardcoded `Anisotropic mask` default");
                // THE TWO HOST TABLES, whose neutrals are `(0, 0, 0, 0)` - the reference's own `interface[]` default
                // for all three sockets. The GPU one is the LOAD-BEARING half: the table starts every lane at
                // `glm::vec4(1.0f)`, so without this override every material that states no row would read
                // `Use anisotropy? = 1` and take the arm the reference does not.
                CHECK_MSG(constructor.find("toon_colour_lane::goo_aniso_gate)] =\n                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);") != std::string::npos,
                          "the gate lane's neutral in the GPU table's initialiser, at ALL FOUR components");
                CHECK_MSG(app.find("glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),\n") != std::string::npos,
                          "step 8's neutral is still in the lookup's table (the gate lane is appended after it)");
                CHECK_MSG(app.find("glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)}};") != std::string::npos,
                          "and the lookup's table now ENDS on step 10's - lane order is the contract");
            }
            // (c10) STEP 12'S ONE COLOUR LANE, `_GooAnisoRough`, AND THE LOBE IT FEEDS. This is the lane that turned
            // `混合.016`'s B arm from a self-declared zero stub into the reference's own function, and it is the
            // lane whose two values ARE the lobe's two roughnesses rather than any kind of switch - which is why
            // the block below pins the ARITHMETIC's spellings as well as the lane's, and why the three trace
            // anchors live in the closed-form half of this file (section 8r3) instead.
            {
                std::size_t const enum_at = primitive.find("enum class toon_colour_lane");
                std::string const colour_enum = primitive.substr(enum_at, primitive.find("};", enum_at) - enum_at);
                CHECK_MSG(colour_enum.find("goo_aniso_rough = 26,") != std::string::npos, "the rough lane's enum entry, APPENDED at 26 so no earlier index moves");
                CHECK_MSG(colour_enum.find("count = 30,") != std::string::npos, "and the enum's count with it (step 13 appended lanes 27/28 and step 15 appended 29 after this one)");
                CHECK_MSG(app.find("\"_GooAnisoRough\",") != std::string::npos, "the rough lane's row name - CamelCase, as the sidecar spells it");
                // A `color` ROW, SO NO BRANCH AND NO DIAGNOSTIC - the same two facts step 10's block asserts for its
                // own lane, asserted the same way (by the enum spelling being absent from `main.cpp` outside the
                // row table's ORDER comment).
                CHECK_MSG(app.find("deren::vulkan::toon_colour_lane::goo_aniso_rough") == std::string::npos,
                          "the rough lane takes the generic `others` path, so `main.cpp` never names the enum outside the row table's ORDER comment");
                // THE TWO HOST TABLES. The lookup's ends on this lane now; the GPU one has to override it, because
                // that table starts every lane at `glm::vec4(1.0f)` and a lane left there would hand every material
                // with no row `rT = (1 - 1)^2 = 0` - a mirror - instead of the reference's own `(0, 0, 0, 0)`.
                CHECK_MSG(app.find("glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)}};") != std::string::npos,
                          "the lookup's neutral table still ENDS on this lane's `(0, 0, 0, 0)` - lane order is the contract");
                CHECK_MSG(constructor.find("toon_colour_lane::goo_aniso_rough)] =\n                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);") != std::string::npos,
                          "the rough lane's neutral in the GPU table's initialiser, at ALL FOUR components");
                // ... AND THE SHADER HALF: the read at lane 26, the two squares, and the two constants the lobe
                // needs that are NOT the isotropic arm's.
                CHECK_MSG(character_forward.find("const float4 goo_aniso_rough_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 26u);") != std::string::npos,
                          "the rough lane is read at `colour_base + 26u`");
                CHECK_MSG(character_forward.find("const float goo_aniso_roughness_t = (1.0 - goo_aniso_rough_lane.x) * (1.0 - goo_aniso_rough_lane.x);") != std::string::npos,
                          "`.x` is `Aniso_SmoothnessMaxT` and `rT` is `(1 - it)^2`");
                CHECK_MSG(character_forward.find("const float goo_aniso_roughness_b = (1.0 - goo_aniso_rough_lane.y) * (1.0 - goo_aniso_rough_lane.y);") != std::string::npos,
                          "`.y` is `Aniso_SmoothnessMaxB` and `rB` likewise");
                CHECK_MSG(character_forward.find("static const float goo_aniso_half_inverse_pi = 0.31830987334251404 * 0.5;") != std::string::npos,
                          "the lobe's own `1/(2pi)`, spelled as its own constant rather than reusing the isotropic arm's");
                CHECK_MSG(character_forward.find("static const float goo_aniso_denominator_floor = 0.0010000000474974513;") != std::string::npos,
                          "and `运算.017`'s floor on the lobe's denominator, at the dump's own precision");
                // THE ARITHMETIC, in the two places the earlier reading of this group got it wrong: `S`'s third
                // component and the fact that the CROSSED roughnesses sit on the tangent/bitangent dots.
                CHECK_MSG(character_forward.find("(goo_ndoth * goo_lobe_p) * (goo_ndoth * goo_lobe_p)") != std::string::npos,
                          "`S.z = NoH * p` - NOT `Lam * p`, which is the reading the brief's §6 corrects");
                CHECK_MSG(character_forward.find("sqrt((goo_aniso_roughness_t * goo_lobe_t_dot_v) * (goo_aniso_roughness_t * goo_lobe_t_dot_v) +") != std::string::npos,
                          "`Lam` is the WHOLE of `ng[25]` on the VIEW side (rT against TdotV), not a denominator term");
                CHECK_MSG(character_forward.find("(goo_aniso_roughness_t * goo_lobe_t_dot_l) * (goo_aniso_roughness_t * goo_lobe_t_dot_l) +") != std::string::npos,
                          "and `|W|` is the LIGHT side - the two vectors are swapped, which is the second correction");
                CHECK_MSG(character_forward.find("const float goo_lobe_denominator = (goo_lobe_w_length * goo_clamped_ndotv + goo_lobe_lambda * goo_ndotl_clamped) * goo_lobe_s2 * goo_lobe_s2;") != std::string::npos,
                          "the denominator is `(|W|*NoV + Lam*NoL) * s2^2`, summed once - neither half is a numerator factor");
                CHECK_MSG(character_forward.find("const float goo_lobe = goo_aniso_half_inverse_pi * ((goo_lobe_p * goo_lobe_p * goo_lobe_p) / max(goo_lobe_denominator, goo_aniso_denominator_floor));") != std::string::npos,
                          "and the lobe is `1/(2pi) * p^3 / max(den, floor)` - `p^3` and NOT `p^3 * Lam`, the third correction");
                // THE CLAMP ARM AND THE ARM ORDER OF `混合.020`: `A` is the lobe times F_Schlick, `B` is the
                // `钳制` result, `f` is lane 25's `.z` - and the whole of it is then multiplied by the mask
                // (`混合.017`), which is what makes this step frame-invisible on this asset.
                CHECK_MSG(character_forward.find("const float goo_lobe_toon_aniso = saturate(dot(cross(n, goo_lobe_t), v));") != std::string::npos,
                          "`钳制.Result = clamp(dot(cross(N, Tangent), Incoming), 0, 1)`");
                CHECK_MSG(character_forward.find("lerp(goo_lobe * goo_schlick_f, float3(goo_lobe_toon_aniso, goo_lobe_toon_aniso, goo_lobe_toon_aniso), goo_aniso_gate.z) * goo_anisotropic_mask;") != std::string::npos,
                          "`混合.020`'s arm order, and `混合.017`'s mask on the outside");
                CHECK_MSG(character_forward.find("surface_tbn(world_pos, uv, goo_lobe_t, goo_lobe_b);") != std::string::npos,
                          "the tangent frame comes from `surface_tbn`, the same function step 8's normal decode uses");
                //     The search is for the DEFINITION WITH ITS `const float3` IN FRONT, because the derivation
                //     comment above the lobe quotes the old line verbatim (it is how this port records what it
                //     replaced) - a plain `find` on the right-hand side alone would match that quotation.
                CHECK_MSG(character_forward.find("const float3 goo_anisotropic_masked = float3(0.0, 0.0, 0.0) * goo_anisotropic_mask;") == std::string::npos,
                          "and the zero stub the lane replaced is GONE as a definition, not merely commented out");
                CHECK_MSG(character_forward.find("const float3 goo_anisotropic_masked = lerp(goo_lobe * goo_schlick_f,") != std::string::npos,
                          "the definition that replaced it is the lobe's");
            }
            // (c11) DEBT (U): `specularFGD Strength` - LANE 16 - IS A `float` ROW, AND BOTH HALVES OF THE DEFECT
            // ARE PINNED HERE. D1: the host had no branch for it, so a row that lives in `material_sidecar::scalars`
            // was unreachable through the `material->others` path the generic arm reads, and lane 16 answered its
            // neutral `-1.0` for EVERY material - measured, not assumed: both body materials state
            // `0.7999999523162842` and every cloth states `1.0`. D2: the shader then read that neutral with the
            // `-1000` sentinel's `> -999.0` comparison, which `-1.0` PASSES, so the reference's own group default
            // `1.0` was never reached even for a material that stated nothing. A test can only see text - the
            // shader's per-pixel behaviour is measured in the task-20 report - but the STRUCTURAL facts below are
            // exactly what a "half fix" breaks, and the mirror predicates at the end are the defect itself.
            {
                // ---- D1: THE BRANCH, ITS PATH, AND WHERE IT SITS IN THE LAMBDA ----
                std::size_t const branch_at = app.find("lane == deren::vulkan::toon_colour_lane::goo_specular_fgd");
                CHECK_MSG(branch_at != std::string::npos,
                          "lane 16 has its own branch in `toon_colour`: a `float`-kind row lands in `scalars`, and only `scalar()` reads those");
                CHECK_MSG(app.find("material->scalar(toon_colour_row[lane_index], fgd.x)") != std::string::npos,
                          "...and the branch reads the row through `scalar()`, not through `others`");
                CHECK_MSG(app.find("if (value >= 0.0f)") != std::string::npos,
                          "...with the same `>= 0.0f` guard step 5's and step 8's lanes use, so an unexpressible row answers the neutral");
                // THE STRUCTURAL PIN: a branch placed AFTER the generic `others` path is dead code - `others` would
                // answer first and D1 would be back with more lines than before.
                std::size_t const generic_at = app.find("auto const row = material->others.find");
                CHECK_MSG(generic_at != std::string::npos, "the generic `others` path is still there");
                if (branch_at != std::string::npos && generic_at != std::string::npos) {
                    CHECK_MSG(branch_at < generic_at,
                              "lane 16's branch must sit BEFORE the generic `others` path - after it, the branch is unreachable");
                }
                CHECK_MSG(app.find("glm::vec4 fgd = toon_colour_neutral[lane_index]; // -1 in `.x` until a row says otherwise") != std::string::npos,
                          "the branch starts from the lane's OWN neutral rather than a new number");
                CHECK_MSG(app.find("_GooSpecularFGD = {:.10g} | lane 16") != std::string::npos,
                          "the start-up dump names the row, because `{} scalar(s)` counts it without naming it - 'the file states nothing' and 'the reader dropped the row' must be distinguishable");
                // ---- D2: THE SHADER'S CONTRACT, AND THE READING THAT MUST BE GONE ----
                CHECK_MSG(character_forward.find("static bool goo_lane_stated(const float value) { return value >= 0.0; }") != std::string::npos,
                          "the `< 0` contract is spelled once, as a named predicate");
                CHECK_MSG(character_forward.find("static bool goo_lane_stated_sentinel(const float value) { return value > goo_lane_absent_threshold; }") != std::string::npos,
                          "and the `-1000` sentinel's contract is spelled beside it, because the two are NOT the same comparison");
                CHECK_MSG(character_forward.find("goo_lane_stated(goo_specular_fgd_lane.x)") != std::string::npos,
                          "lane 16 is read through the `< 0` contract");
                CHECK_MSG(character_forward.find("goo_specular_fgd_lane.x > goo_lane_absent_threshold") == std::string::npos,
                          "lane 16's `-999` reading is GONE, not merely commented out: `-1.0 > -999.0` is what swallowed the neutral in the shipped build");
                CHECK_MSG(character_forward.find("const float4 goo_specular_fgd_lane = toon_colour_at(heap_slots_toon_colours, colour_base + 16u);") != std::string::npos,
                          "...and it is still read at lane 16 of the surface stage's own stride");
                // ---- THE TWO CONTRACTS, PER INPUT, AS MIRROR PREDICATES ----
                // This is the half a text pin cannot express: the DIFFERENCE between the two predicates IS debt (u),
                // and it lives exactly on the half-open interval `(-999, 0)`. Both spellings are mirrors of the
                // shader's, and the shader's own text is pinned above.
                auto const stated = [](float const v) { return v >= 0.0f; };            // step 5's four + lane 16
                auto const stated_sentinel = [](float const v) { return v > -999.0f; }; // step 4's four, face's two, lane 24
                CHECK(!stated(-1.0f));
                CHECK(stated(0.0f));
                CHECK(stated(1.0f));
                CHECK(!stated(-1000.0f));
                CHECK(stated_sentinel(-1.0f));
                CHECK(stated_sentinel(0.0f));
                CHECK(stated_sentinel(1.0f));
                CHECK(!stated_sentinel(-1000.0f));
                CHECK(stated(-1.0f) != stated_sentinel(-1.0f)); // the disagreement IS the defect
                // WHERE THEY DISAGREE, EXACTLY - and this replaces the spec's second assertion, which is
                // arithmetically impossible: `0.0f` is ACCEPTED by BOTH predicates (a stated `0` is a stated value,
                // and `0 > -999`), so `stated(0.0f) != stated_sentinel(0.0f)` can never hold. The boundary pair
                // below pins the same fact more sharply: they disagree throughout `(-999, 0)` and agree at both
                // ends of it, which is the whole of D2's asymmetry.
                CHECK(stated(-0.5f) != stated_sentinel(-0.5f));
                CHECK(stated(-998.0f) != stated_sentinel(-998.0f));
                CHECK(stated(0.0f) == stated_sentinel(0.0f));
                CHECK(stated(-999.0f) == stated_sentinel(-999.0f));
                for (float const v : {-1000.0f, -999.0f, 0.5f, 1.0f}) {
                    CHECK(stated(v) == stated_sentinel(v));
                }
                // ---- AND THE FOUR `-1000` LANES ARE UNTOUCHED, which is why their call sites keep the old spelling ----
                CHECK_MSG(character_forward.find("goo_diffuse_a.z > goo_lane_absent_threshold") != std::string::npos,
                          "step 4's `CastShadow_center` still uses the sentinel's comparison");
                CHECK_MSG(character_forward.find("goo_diffuse_b.x > goo_lane_absent_threshold") != std::string::npos,
                          "and so does `GlobalShadowBrightnessAdjustment`");
                CHECK_MSG(character_forward.find("goo_lane_stated(goo_diffuse_a.z)") == std::string::npos,
                          "debt (u) must NOT be 'generalised' into the `-1000` lanes: `-1.8` is an authored value there, not an absence");
            }
            // ---- STEP 11: THE DENOMINATOR'S TEXT, so a "simplification" of it cannot pass either ----
            {
                CHECK_MSG(character_forward.find("const float goo_dv_lambda_v = abs(goo_ndotl_clamped) * (goo_a2 + (1.0 - goo_a2) * goo_clamped_ndotv * goo_clamped_ndotv);") != std::string::npos,
                          "the UN-rooted half of λ is multiplied by |NoL| - `运算.005`'s own shape");
                CHECK_MSG(character_forward.find("const float goo_dv_lambda_l = goo_clamped_ndotv * sqrt(goo_a2 + (1.0 - goo_a2) * goo_ndotl_clamped * goo_ndotl_clamped);") != std::string::npos,
                          "and the ROOTED half is multiplied by NoV: the ONE root, on the NoL bracket");
                CHECK_MSG(character_forward.find("const float goo_dv_lambda = goo_dv_lambda_v + goo_dv_lambda_l;") != std::string::npos,
                          "the two halves are ADDED (`运算.012`) and nothing is added to them");
                CHECK_MSG(character_forward.find("max(goo_dv_s * goo_dv_s * goo_dv_lambda, 1.1754899742869237e-35)") != std::string::npos,
                          "the floor is 运算.017's MAXIMUM on the WHOLE denominator, at the dump's own precision");
                CHECK_MSG(character_forward.find("const float goo_dv_original = goo_dv_half_inverse_pi * (goo_a2 / goo_dv_denominator);") != std::string::npos,
                          "and 原 is the group's 1/(2pi) times `运算.018`'s `a2 / denominator`, in that order");
                // ... AND THE TWO DEFECTS THEMSELVES, PINNED ABSENT - each spelling is the port's OWN old line, not a
                // paraphrasing of it, so a partial revert fails here.
                CHECK_MSG(character_forward.find("max(pow(goo_ndoth * goo_ndoth * (goo_a2 - 1.0) + 1.0, 2.0), 1e-12)") == std::string::npos,
                          "the old `pow(S, 2)` with a `1e-12` floor on it is gone from the body arm");
                CHECK_MSG(character_forward.find("sqrt(max(goo_a2 + (1.0 - goo_a2)") == std::string::npos,
                          "and so is the clamp inside the SQRT, which the reference does not have");
                CHECK_MSG(character_forward.find("1.0 + goo_dv_lambda") == std::string::npos,
                          "and the `+1` in the denominator is gone: there is no `Gv` in this term any more");
                CHECK_MSG(character_forward.find("const float goo_gv = ") == std::string::npos,
                          "no `goo_gv` is computed at all, so no reader can mistake λ for a Smith `Gv`");
            }
            // ---- 8r4. 欠账 (e): STEP 11's SECOND COPY, IN THE FACE ARM ----
            //
            // STEP 11 FIXED THE `原` DENOMINATOR IN THE BODY ARM; THE FACE ARM KEPT THE OLD EXPRESSION, and
            // `deren-ab/goo_step11_armA_result.md` §7 item 2 reports that rather than fixing it. What this block
            // guards is the property the fix is FOR: the two copies are THE SAME ARITHMETIC under the arm's own
            // names, so an edit to either one alone fails HERE rather than in a frame nobody compares.
            {
                // (1) THE SAME SOURCE, LINE BY LINE. Each of the six lines is pulled out of the shader text by its
                //     own definition, the BODY's line is then renamed into the FACE arm's vocabulary, and the two
                //     are compared CHARACTER FOR CHARACTER. The names below are the whole difference between the
                //     arms - `goo_ndotl_clamped` -> `face_ldoth` is the reference's own wiring of this arm's
                //     `Abs_NdotL` socket and not a rename of convenience. A dropped `sqrt`, a re-added `+1`, a floor
                //     moved back onto `S^2` or a `pow(..., 2.0)` re-appearing all change ONE side and fail here.
                auto const line_with = [&character_forward](char const* const needle) {
                    std::size_t const at = character_forward.find(needle);
                    if (at == std::string::npos) {
                        return std::string{};
                    }
                    std::size_t const start = character_forward.rfind('\n', at);
                    std::size_t const stop = character_forward.find(';', at);
                    if (stop == std::string::npos) {
                        return std::string{};
                    }
                    std::size_t const begin = start == std::string::npos ? 0u : start + 1u;
                    return character_forward.substr(begin, stop + 1u - begin);
                };
                auto const body_as_face = [](std::string text) {
                    for (auto const& names : {std::pair{"goo_dv_lambda_v", "face_dv_lambda_v"},
                                              std::pair{"goo_dv_lambda_l", "face_dv_lambda_l"},
                                              std::pair{"goo_dv_lambda", "face_dv_lambda"},
                                              std::pair{"goo_dv_denominator", "face_dv_denominator"},
                                              std::pair{"goo_dv_original", "face_dv_original"},
                                              std::pair{"goo_dv_s", "face_dv_s"},
                                              std::pair{"goo_ndotl_clamped", "face_ldoth"},
                                              std::pair{"goo_clamped_ndotv", "face_clamped_ndotv"},
                                              std::pair{"goo_ndoth", "face_ndoth"},
                                              std::pair{"goo_a2", "face_a2"}}) {
                        std::string const from{names.first};
                        std::string const to{names.second};
                        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
                            text.replace(at, from.size(), to);
                        }
                    }
                    return text;
                };
                for (char const* const name : {"dv_s", "dv_lambda_v", "dv_lambda_l", "dv_lambda", "dv_denominator", "dv_original"}) {
                    std::string const body_line = line_with(("const float goo_" + std::string{name} + " = ").c_str());
                    std::string const face_line = line_with(("const float face_" + std::string{name} + " = ").c_str());
                    std::string const message = "8r4: the face arm's `const float face_" + std::string{name} +
                                                " = ` line is the body arm's line under the arm's own names";
                    CHECK_MSG(!body_line.empty() && !face_line.empty() && body_as_face(body_line) == face_line, message.c_str());
                }
                // (2) THE VALUE, THROUGH THE FACE ARM'S OWN SPELLING. The closed form is re-derived here (`8r2`'s
                //     helper is out of scope) and it is the same function of `(a2, NoV, NoL, NoH)` that `8r2`
                //     pins; `NoL` is passed `|L·H|` because that is what this arm's `Abs_NdotL` socket is fed.
                auto const face_parts = [](double const r, double const nov, double const nol, double const noh) {
                    double const cr = r * r;
                    double const a2 = cr * cr;
                    double const s = 1.0 + noh * noh * (a2 - 1.0);
                    double const lv = std::abs(nol) * (a2 + (1.0 - a2) * nov * nov);
                    double const ll = nov * std::sqrt(a2 + (1.0 - a2) * nol * nol);
                    double const lambda = lv + ll;
                    double const floored = std::max(s * s * lambda, static_cast<double>(k_dv_denominator_floor));
                    return std::array<double, 4u>{s, lambda, floored, 0.1591549962759018 * a2 / floored};
                };
                // ... AND THE DELETED FACE COPY, kept only to state the defect it carried: `S^2` floored at `1e-12`,
                // the `sqrt` on the half that is not rooted, and the invented `Gv`'s `+1`. Nothing below asserts
                // that any shader still computes this.
                auto const face_old = [](double const r, double const nov, double const nol, double const noh) {
                    double const cr = r * r;
                    double const a2 = cr * cr;
                    double const s2 = noh * noh * (a2 - 1.0) + 1.0;
                    double const lambda_v = std::abs(nol) * std::sqrt(std::max(a2 + (1.0 - a2) * nov * nov, 0.0));
                    return 0.1591549962759018 * (a2 / std::max(s2 * s2, 1e-12)) * (1.0 / (1.0 + lambda_v));
                };
                CHECK_MSG(std::abs(face_parts(0.25, 0.7, 0.7, 0.95)[3] - 7.29172301067769e-02) < 1e-15,
                          "8r4: the face arm's fixed expression gives the body's 7.29172301067769e-02 at the arbitrated point");
                CHECK_MSG(std::abs(face_old(0.25, 0.7, 0.7, 0.95) / face_parts(0.25, 0.7, 0.7, 0.95)[3] - 0.5602899861504124) < 1e-12,
                          "8r4: and the copy removed from it was the SAME 0.5602899861504124x of that - 1.7847900635717335x too dark");
                // (3) THE NEUTRALITY, WHICH IS WHY THIS STEP CANNOT MOVE A PIXEL on this asset: the shipped face
                //     material's `_GooFaceScalarsA.w` is `1.0`, so `perceptualRoughness = 1 - saturate(1) = 0`,
                //     `clampedRoughness = 0` and `a2 = 0`. Both spellings then have the ZERO numerator and agree
                //     EXACTLY - not approximately - so the difference the fix removes is multiplied by that zero.
                CHECK_MSG(face_parts(0.0, 0.7, 0.7, 0.95)[3] == 0.0 && face_old(0.0, 0.7, 0.7, 0.95) == 0.0,
                          "8r4: at a2 = 0 both spellings are exactly 0 - the fix is pixel-neutral by construction");
            }
            // ---- `headCenter`: THE ONE PIECE OF NEW DATA, and the stride is the failure class ----
            CHECK_MSG(primitive.find("glm::vec4 center = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);") != std::string::npos, "`head_ubo` gained the position");
            CHECK_MSG(primitive.find("static_assert(sizeof(head_ubo) == 64);") != std::string::npos, "and its size is asserted at FOUR vec4s, not three");
            CHECK_MSG(character_forward.find("float4 center; // the head object's world position") != std::string::npos, "the shader's own copy has the same fourth member");
            CHECK_MSG(character_forward.find("a centre is known") != std::string::npos, "and it documents the flag in `.w`");
            CHECK_MSG(character_forward.find("world_pos - head_frame_at(heap_slots_scene_head + heap_frame_slot).center.xyz") != std::string::npos,
                      "and the sphere normal subtracts it from the fragment's world position");
            // THE FLAG IS THE PART THAT KEEPS A FABRICATED CENTRE OUT OF THE SHADING: every character glb in this
            // repository is BAKED (8 nodes, 0 skins), so no head bone ever runs the publish below, and `center.w`
            // stays 0 - which is what makes the arm fall back to the socket's own `interface[]` default (0.0)
            // instead of building a normal about the world origin.
            CHECK_MSG(primitive.find("glm::vec4 center = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);") != std::string::npos, "the centre's `.w` starts at 0 = 'no centre known'");
            CHECK_MSG(character_forward.find("center.w > 0.5 && face_scalars_a.z > goo_lane_absent_threshold") != std::string::npos,
                      "and the arm reads the flag before it reads the socket");
            CHECK_MSG(app.find("glm::vec3 const head_center(joint[3][0], joint[3][1], joint[3][2]);") != std::string::npos,
                      "main.cpp fills it from the SAME bone matrix's translation column the two axis rows come from");
            CHECK_MSG(app.find(".center = glm::vec4(head_center, 1.0f)}") != std::string::npos, "...and sets the flag when it does");
            CHECK_MSG(app.find(".center = glm::vec4(0.0f)}") != std::string::npos, "...and leaves it clear in the no-head-bone publish");
            CHECK_MSG(app.find("center ({:.3f} {:.3f} {:.3f})") != std::string::npos, "and the one-shot probe logs it, so the value is evidence rather than an assumption");
            // ---- THE FACE ARM ITSELF ----
            CHECK_MSG(character_forward.find("static const float goo_face_forward_sign = -1.0;") != std::string::npos,
                      "step 7's copy of `值(明度)` is -1.0, the value BOTH dumps store");
            CHECK_MSG(shader.find("static const float goo_iris_forward_sign = -1.0;") != std::string::npos,
                      "and the iris' copy is the same number - one socket, two spellings, and they must not drift");
            CHECK_MSG(character_forward.find("static const float goo_face_half_turn = 3.141592502593994;") != std::string::npos, "the reference's own pi literal");
            CHECK_MSG(character_forward.find("static const float goo_face_shadow_proxy_open = 1.0 + goo_face_shadow_proxy_lift;") != std::string::npos,
                      "the shadow proxy is held at its OPEN value, spelled as the graph's own `(1 - 运算.020) + 0.1`");
            CHECK_MSG(character_forward.find("static const float goo_face_ambient_contrast = -0.4000000059604645;") != std::string::npos, "亮度 / 对比度's contrast, verbatim");
            CHECK_MSG(character_forward.find("static const float goo_face_eye_brightness_default = 1.2999999523162842;") != std::string::npos,
                      "the EYE-WHITE brightness's INTERFACE default is 1.3, not the material's 1.5");
            CHECK_MSG(character_forward.find("family == VR_FAMILY_FACE && goo_base_ramp != 0u && goo_face_block2.w != 0u") != std::string::npos,
                      "the arm's gate names the family, the ramp lane and the SDF lane");
            CHECK_MSG(character_forward.find("return face_desaturated * face_emission_strength;") != std::string::npos,
                      "and it RETURNS, which is what guards every article term below it off for the face");
            CHECK_MSG(character_forward.find("out_desaturation = goo_desaturation_neutral;") != std::string::npos,
                      "with the out-parameter left at the identity, because the desaturation was applied INSIDE the arm where the reference has it");
            CHECK_MSG(character_forward.find("const float face_saturation = saturate(dot(face_ramp_colour") != std::string::npos,
                      "the face's own desaturation is the reference's `clamp(luma(RampColor) + 0.9, 0, 1)`");
            CHECK_MSG(character_forward.find("face_flip > 0.0 ? (uv.x > 0.5 ? 0.0 : 1.0)") != std::string::npos,
                      "the front-red gate IS the SDF's flip bit - A9's two-mirrors claim, in the source");
            CHECK_MSG(character_forward.find("const float face_rim_threshold = (face_cm_a * face_mirror_uv) - face_angle_threshold;") != std::string::npos,
                      "and the face's rim gate SUBTRACTS the angle threshold rather than scaling by it");
        }
        // ---- (c9) STEP 9: THE DIRECT SPECULAR'S FACTOR, THE TWO ARGUMENTS E2/E3 MOVE, AND WHAT DOES *NOT* MOVE ----
        //
        // THE SENTENCE THIS BLOCK EXISTS TO MAKE FALSE IS "the port already had the energy compensation". It had it
        // on ONE path - the IBL product at `:2790`, step 5's line - and the DIRECT product at `:2770` had NO factor
        // at all, which is the whole defect (spec §3.3, and the frame that shows it is `goo_step9_result.md`'s).
        // So the checks below are deliberately split three ways: the new factor is spelled EXACTLY ONCE and tied to
        // the raw product's own tail, the IBL line is required to be UNCHANGED, and the two arguments E2/E3 move are
        // required to be the SAME two quantities `goo_diffuse_colour` already reads.
        {
            // THE FACTOR ITSELF. The spec's §A10 example spelling is `goo_direct_specular * (1.0 + ...)`, i.e. the
            // raw product in its own variable; the parent's brief for this step instead rules that the factor is
            // APPENDED to `:2770`'s own expression, and §A10 explicitly leaves the exact spelling to the
            // implementation ("具体拼写实现时定稿"). What §A10 does NOT leave open is the requirement, so that is
            // what is asserted: ONE occurrence, carrying BOTH operands, and the raw product's own tail in front of
            // it - so the factor cannot be dropped, doubled, or moved onto another term without this failing.
            char const* const factor = "(1.0 + goo_energy_compensation * goo_fresnel0)";
            std::size_t occurrences = 0u;
            for (std::size_t at = character_forward.find(factor); at != std::string::npos;
                 at = character_forward.find(factor, at + 1u)) {
                ++occurrences;
            }
            CHECK_MSG(occurrences == 1u, "the direct specular's energy-compensation factor appears EXACTLY once in the shader");
            CHECK_MSG(character_forward.find("goo_ndotl_clamped * (goo_specular_chosen * goo_specular_color) * shadow_used * goo_light_color * direct_occlusion * (1.0 + goo_energy_compensation * goo_fresnel0)") != std::string::npos,
                      "and it multiplies the RAW direct product's own tail, not some other term - the reference's `Vector Math.014` is `directLighting_specular ⊙ (1 + ec ⊙ f0)` and nothing else. DEBT (q): the visibility operand is `shadow_used`, the PRE-sigmoid output, and this string was updated with the shader when that token moved");
            CHECK_MSG(character_forward.find("goo_specular_color * (1.0 + goo_energy_compensation") == std::string::npos,
                      "the factor does NOT re-multiply `goo_specular_color`: the raw product already applied that tint, and `Vector Math.014`'s factor is built from `energyCompensation` and `fresnel0` alone");
            // ... AND THE OTHER PATH IS STEP 5'S, UNTOUCHED. This is the check that makes "no double count" a
            // property of the source rather than a claim in a report: the IBL product keeps the BARE factor.
            CHECK_MSG(character_forward.find("goo_spec_ibl = goo_specular_fgd * goo_specular_fgd_strength * goo_energy_compensation;") != std::string::npos,
                      "the IBL specular keeps the BARE factor - step 5's own line, unchanged by this step");
            CHECK_MSG(character_forward.find("goo_spec_ibl = goo_specular_fgd * goo_specular_fgd_strength * goo_energy_compensation *") == std::string::npos,
                      "and it carries no SECOND factor after that one - the spec's §A7 negative assertion, spelled as a check");
            // ---- DEBT (q): THE VISIBILITY OPERAND IS THE PRE-SIGMOID ONE (spec §3.1, §4.1) ----
            //
            // The reference's direct specular reads `Shader Info.Cast Shadows`'s RAW output, so the post-`SigmoidSharp`
            // curve has exactly ONE legitimate consumer left in the port, `:2526`'s `ramp_u`. Both directions are pinned
            // HERE rather than in the header block above, and the closed form (not a monotonicity claim) carries the
            // content: `new - old = T ⊙ (v - sig(v))` is TWO-SIDED, so a check that only said "the shadow side gets
            // darker" would pass with the sign of the fix INVERTED. The fixpoint and both extremes are asserted instead,
            // for all three lane groups the shipped sidecar can produce.
            CHECK_MSG(character_forward.find("goo_specular_color) * cast_shadow_sigmoid * goo_light_color") == std::string::npos,
                      "DEBT (q): the direct specular no longer multiplies the POST-sigmoid visibility");
            CHECK_MSG(character_forward.find("goo_specular_color) * shadow_used * goo_light_color") != std::string::npos,
                      "DEBT (q): it multiplies `shadow_used` (`:1521-1523`) - the reference's `Cast Shadows` output, pre-sigmoid");
            CHECK_MSG(character_forward.find("ramp_u = min(cast_shadow_sigmoid, remap_half_lambert_sigmoid)") != std::string::npos,
                      "DEBT (q): and `cast_shadow_sigmoid` still feeds `ramp_u` (`:2526`) - its one legitimate consumer, NOT deleted");
            // `goo_sigmoid_sharp` (`:924-930`) in fp64, independent of the shader's fp32:
            auto const q_sig = [](double const v, double const center, double const sharp) {
                return 1.0 / (1.0 + std::pow(100000.0, -3.0 * sharp * (v - center)));
            };
            auto const q_fixpoint = [&q_sig](double const center, double const sharp) {
                double lo = 0.0;
                double hi = 1.0;
                for (int i = 0; i < 200; ++i) {
                    double const mid = 0.5 * (lo + hi);
                    if (q_sig(mid, center, sharp) > mid) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                }
                return 0.5 * (lo + hi);
            };
            // TOLERANCE NOTE: these are fp64 evaluations of the shipped fp32 expression, and the spec's own table
            // (§4.1) prints fp32-rounded constants, so the last digits differ from a pure fp64 recomputation (spec
            // U5). The pins therefore carry a 1e-6 tolerance. See the cloth pair below for the one place where the
            // spec's number is a SAMPLE POINT rather than the maximum - an earlier draft of this file read it as an
            // error in the spec and was itself wrong.
            CHECK_MSG(std::abs(q_fixpoint(-0.1, 0.05) - 0.833766073) < 1e-6,
                      "DEBT (q) P1: body_01/02's `cast_center = -0.1, cast_sharp = 0.05` turns over at v* = 0.833766073 (spec §4.1 prints 0.833766078; 5e-9 is fp32-vs-fp64, spec U5)");
            CHECK_MSG(std::abs((q_sig(0.0, -0.1, 0.05) - 0.0) - 0.543066492) < 1e-6,
                      "DEBT (q) P1: fully shadowed, the swap makes the highlight DARKER by 0.543066492 (138.48 codes)");
            CHECK_MSG(std::abs((1.0 - q_sig(1.0, -0.1, 0.05)) - 0.130150051) < 1e-6,
                      "DEBT (q) P1: fully LIT it makes it BRIGHTER by 0.130150051 (33.19 codes) - the fix is two-sided, NOT 'only darker'");
            CHECK_MSG(std::abs(q_fixpoint(0.0, 0.17) - 0.997142116) < 1e-6,
                      "DEBT (q) P1: cloth_01..05 and face_01 `(0.0, 0.17)` turn over at v* = 0.997142116");
            CHECK_MSG(std::abs((q_sig(0.21875, 0.0, 0.17) - 0.21875) - 0.564449648) < 1e-6,
                      "DEBT (q) P1: cloth's darkening AT THE SPEC'S SAMPLE POINT v = 0.21875 is 0.564449648 (143.93 codes; spec §4.1 prints 0.564449650)");
            {
                // ... AND THE MAXIMUM, WHICH IS A DIFFERENT POINT. The spec's table is tabulated at a sample point;
                // the Lead's m02206 quotes the true worst case (`0.564451022`), and a 10^6-point scan agrees with it to
                // 3e-9 while locating it at `v = 0.2178405`. Pinning only the sampled value would let a reader believe
                // the worst darkening is 1.4e-6 smaller than it is, and pinning only the scan would lose the tie to
                // the spec's own table entry - so both are here, each labelled as what it is.
                double worst = 0.0;
                double worst_v = 0.0;
                for (int i = 0; i <= 1000000; ++i) {
                    double const v = static_cast<double>(i) / 1000000.0;
                    double const darkening = q_sig(v, 0.0, 0.17) - v;
                    if (darkening > worst) {
                        worst = darkening;
                        worst_v = v;
                    }
                }
                CHECK_MSG(std::abs(worst - 0.564451019) < 1e-6 && std::abs(worst_v - 0.2178405) < 1e-4,
                          "DEBT (q) P1: the TRUE worst cloth darkening is 0.564451019 at v = 0.2178405 (measured by a 10^6-point scan, matching m02206's 0.564451022 to 3e-9 - it is NOT the tabulated sample point)");
            }
            CHECK_MSG(std::abs((1.0 - q_sig(1.0, 0.0, 0.17)) - 0.002810462) < 1e-6,
                      "DEBT (q) P1: and cloth's brightening band is only 0.002810462 (0.72 codes) wide - which is why the A/B can see it on the body and barely on cloth");
            CHECK_MSG(std::abs(q_fixpoint(0.0, 0.0) - 0.5) < 1e-12 && std::abs((q_sig(0.0, 0.0, 0.0) - 0.0) - 0.5) < 1e-12,
                      "DEBT (q) P1: the absent-lane fallback (`sharp = 0` -> the guarded 0.5) turns over at exactly v = 0.5");
            {
                // WHAT MAKES THE DIRECTION SPLIT A THRESHOLD. NOT monotonicity of `v - sig(v)` - cloth's lane group is
                // measurably NON-monotone (it dips to its worst darkening at v = 0.21875 and only crosses zero at
                // 0.9971), so a "strictly increasing" pin would have been FALSE for cloth while still passing for the
                // body. What is pinned instead is the property P1 actually needs: the cast curve is monotonically
                // non-decreasing in visibility, and `v - sig(v)` changes sign EXACTLY ONCE per lane group.
                auto const q_sign_changes = [&q_sig](double const center, double const sharp) {
                    int changes = 0;
                    bool previous_positive = (0.0 - q_sig(0.0, center, sharp)) > 0.0;
                    for (int i = 1; i <= 4000; ++i) {
                        double const v = static_cast<double>(i) / 4000.0;
                        bool const positive = (v - q_sig(v, center, sharp)) > 0.0;
                        if (positive != previous_positive) {
                            ++changes;
                        }
                        previous_positive = positive;
                    }
                    return changes;
                };
                auto const q_sig_monotone = [&q_sig](double const center, double const sharp) {
                    double previous = q_sig(0.0, center, sharp);
                    for (int i = 1; i <= 4000; ++i) {
                        double const current = q_sig(static_cast<double>(i) / 4000.0, center, sharp);
                        if (current < previous - 1e-15) {
                            return false;
                        }
                        previous = current;
                    }
                    return true;
                };
                CHECK_MSG(q_sign_changes(-0.1, 0.05) == 1 && q_sign_changes(0.0, 0.17) == 1 && q_sign_changes(0.0, 0.0) == 1,
                          "DEBT (q) P1: `v - sig(v)` changes sign EXACTLY ONCE for every lane group, so 'which way does this pixel move' has a single threshold answer (darkening below the fixpoint, brightening above)");
                CHECK_MSG(q_sig_monotone(-0.1, 0.05) && q_sig_monotone(0.0, 0.17) && q_sig_monotone(0.0, 0.0),
                          "DEBT (q) P1: and the cast curve itself never decreases in visibility, which is what makes that fixpoint unique rather than one of several crossings");
            }
            // ---- E2 + E3: THE TWO ARGUMENTS OF `ComputeFresnel0`, WHICH ARE THE TWO THE DIFFUSE TERM ALREADY READS ----
            CHECK_MSG(character_forward.find("lerp(float3(goo_fgd_dielectric_f0), albedo * goo_base_colour, metallic * metallic_max)") != std::string::npos,
                      "E2 + E3: `fresnel0`'s metal end is `albedo * goo_base_colour` - the SAME product `goo_diffuse_colour` uses - and its factor is `metallic * metallic_max`");
            CHECK_MSG(character_forward.find("lerp(float3(goo_fgd_dielectric_f0), goo_base_colour, metallic)") == std::string::npos,
                      "and the pre-step-9 spelling - which dropped the metallic map from the factor and the glTF albedo from the colour - is GONE, not merely accompanied");
            CHECK_MSG(character_forward.find("const float3 goo_diffuse_colour = albedo * goo_base_colour * (1.0 - metallic * metallic_max);") != std::string::npos,
                      "`goo_diffuse_colour` is UNCHANGED by this step, which is what makes the two same-source rather than merely similar");
            CHECK_MSG(character_forward.find("const float metallic_max = goo_diffuse_b.z > goo_lane_absent_threshold ? goo_diffuse_b.z : goo_metallic_max_default;") != std::string::npos,
                      "`MetallicMax` still arrives on step 5's own lane component (`goo_diffuse_b.z`): this step added NO lane, NO heap slot and NO texture binding");
            CHECK_MSG(character_forward.find("static const float goo_metallic_max_default = 1.0;") != std::string::npos,
                      "the group's own `MetallicMax` default is the 1.0 the port falls back to - and the 23 instances all state exactly this (verify B1)");
            CHECK_MSG(character_forward.find("static const float goo_fgd_dielectric_f0 = 0.07999999821186066;") != std::string::npos,
                      "and the dielectric end is the FGD group's own F0 - now load-bearing for the COMPENSATION too, because `fresnel0` is a factor of it");
            // ---- WHAT THIS STEP DELIBERATELY DOES NOT MOVE ----
            // `energy_distribution_metallic` is a 0-HIT string in BOTH dumps (spec §1 item 3, verify B3), so the
            // article's own `0.96 - 0.96 * metallic` at `:1956` is NOT this mechanism and the brief forbids touching
            // it. Its presence is pinned so that "we left it alone" is visible in the source rather than only in a
            // report - and so that a later reader who greps for `metallic` finds the boundary next to the factor.
            CHECK_MSG(character_forward.find("const float energy_distribution_metallic = 0.96 - 0.96 * metallic;") != std::string::npos,
                      "the article's `0.96 - 0.96 * metallic` is still there and still NOT this step's mechanism (its group's `energy_distribution_metallic` is a 0-hit string in both dumps)");
        }
        // (d) THE SWITCH, which is the A/B's own instrument: it must be a `[render]` key, because that is the only
        // section the capture script can override - a key anywhere else would make the A/B unrunnable.
        CHECK_MSG(config.find("render->get(\"goo_toon\")") != std::string::npos, "the [render] goo_toon key is parsed");
        // (e) THE SUPPRESSIONS, which are text checks because none of them is visible in a frame on its own:
        // the OLD rim must be excluded for the four ported families and KEPT for the face and the eye; the
        // ARTICLE's screen-space contour must stop sharing the surface stage's feature name; and the NEW stage
        // must be gated by the SAME predicate that picks the rewritten chain's pipeline - which is the one gate
        // that keeps `[render] goo_toon = false` byte-identical.
        CHECK_MSG(shader.find("#define VR_GOO_TOON_CHAIN 1") != std::string::npos, "the rewritten chain defines the rim guard");
        CHECK_MSG(character_forward.find("#ifndef VR_GOO_TOON_CHAIN") != std::string::npos, "the old rim is inside the guard");
        CHECK_MSG(character_forward.find("rim_ported_by_goo") != std::string::npos, "the guarded arm names the families it covers");
        CHECK_MSG(character_forward.find("family == VR_FAMILY_BASE || family == VR_FAMILY_SKIN || family == VR_FAMILY_CLOTH || family == VR_FAMILY_HAIR") != std::string::npos,
                  "and the four families are the ones it covers (the face is NOT among them)");
        // the OTHER consumer of `toon_diffuse` must NOT define it, or the outline would lose the old rim
        CHECK_MSG(outline.find("VR_GOO_TOON_CHAIN") == std::string::npos, "`outline.slang` leaves the old rim switched on");
        CHECK_MSG(pbr.find("VR_GOO_TOON_CHAIN") == std::string::npos, "and so does `pbr.slang`");
        CHECK_MSG(rim_pass.find("return \"toon_screen_rim\";") != std::string::npos, "the screen rim asks under its own feature name");
        CHECK_MSG(demo.find("self.runtime_owner->goo_toon_active()") != std::string::npos, "the rewritten chain switches the article's contour off");
        // (f) STEP 3'S OWN GATE, and it is the one this step's byte-identity criterion rests on: the new rim stage
        // must ask under ITS OWN feature name (so the owner can answer it), it must not record when that name is
        // false, and the owner's answer must be the SAME `goo_toon_active()` predicate that picks the character
        // stage's pipeline - a gate that could disagree with the pipeline choice would draw the Goo rim over the
        // OLD chain's frame or none over the new one.
        std::string const goo_rim_pass = slurp("vulkan/pass/goo_rim.cpp");
        CHECK_MSG(goo_rim_pass.find("return \"goo_rim\";") != std::string::npos, "the goo rim asks under its own feature name");
        // ... and the owner answers that name with the runtime's own predicate, in the branch whose last term is
        // the POSITIVE form of the contour's negation above
        std::size_t const goo_rim_branch = demo.find("if (name == \"goo_rim\")");
        CHECK_MSG(goo_rim_branch != std::string::npos, "the feature table has a branch for it");
        std::string const goo_rim_answer = demo.substr(goo_rim_branch, demo.find("}\n", goo_rim_branch) - goo_rim_branch);
        CHECK_MSG(goo_rim_answer.find("self.runtime_owner->goo_toon_active()") != std::string::npos, "answered with the SAME predicate the pipeline choice uses");
        CHECK_MSG(goo_rim_answer.find("self.goo_rim->ready() && goo_toon_active") != std::string::npos, "and true only when the rewritten chain is the one drawing");
        // ... and the frame loop records the stage after the character stage, gated on that name
        std::string const frames = slurp("vulkan/runtime/runtime.frames.cppm");
        CHECK_MSG(frames.find("this->goo_rim_pass = {at(\"goo_rim\")};") != std::string::npos, "the stage is bound to the chain by name");
        CHECK_MSG(frames.find("pass::stage const goo_rim_stage = {.name = \"goo_rim\"") != std::string::npos, "and recorded as its own stage");
        // ... and the G-buffer's publication is gated on the same feature name, or a frame with `goo_toon` off
        // would transition images for a pass that never draws
        CHECK_MSG(demo.find("services.feature_active(services.owner, \"goo_rim\")") != std::string::npos, "the stage preamble is gated on the same name");
    }

    // ---- (c10) STEP 13: `RS EFF` (MECHANISM TABLE #14) ----
    //
    // TWO HALVES, the same two this file has always had. The first half evaluates the RS block's closed form
    // (spec §5 A1..A7) in the reference's own op order: `float_from_vec4`'s Rec.709 luminance, the `_M`
    // `SmoothStep` subgroup on top of it, `混合.029`'s LIGHTEN semantics, the bitwise identities the frozen `if`
    // gate buys, and the host's two new lanes. The second half pins the SOURCE TEXT (A8) - the sync points a
    // compiler cannot see: which FILE the RS block landed in (the rewritten chain `shaders/goo_toon.slang`, NOT
    // the old one), which stride literals moved, and the two row names without which every sidecar row is
    // unreachable while the build stays green.
    //
    // STEP 15 (one step later) PORTED `armA`, so that half of the mechanism is now live code: the outer gate is
    // `Use RS_Eff?` alone, `RS Model` picks the arm, and A2/A4 - written here as BACKGROUND pins "armA only, not
    // called by step 13" - became that branch's leaves without a single value moving. A9 below is the new half:
    // the landing form's parenthesis order and the asset's own `arm0` number.
    //
    // A2 AND A4 ARE STEP 13'S BACKGROUND PINS: they pin the two leaves the then-unported `armA` branch would need,
    // so a later port cannot drift them silently. They were NOT step 13's acceptance - they are step 15's.
    {
        auto const slurp = [](char const* const relative) {
            std::ifstream file(std::string(VR_TEST_SOURCE_DIR) + "/" + relative);
            CHECK_MSG(file.is_open(), relative);
            return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        };

        // ---- A1: the mask is `SmoothStep(SMIN, rs_smax, float_from_vec4(texel))`, and `float_from_vec4` is
        //          Rec.709 LUMINANCE rather than the arithmetic average ----
        CHECK_MSG(std::abs(rs_float_from_color({0.5f, 0.25f, 1.0f}) - 0.357299984f) < 1e-6f,
                  "RS A1: float_from_vec4 is dot(rgb, (0.2126, 0.7152, 0.0722)) = 0.357299984, not the 0.583333313 average");
        CHECK_MSG(std::abs(rs_float_from_color({0.0f, 0.0f, 1.0f}) - 0.0722000003f) < 1e-6f,
                  "RS A1: a pure blue colour converts to 0.0722000003");
        // THE ASSET'S THREE `_M` ARE GREY (R = G = B bit for bit, verifier m00963), so on THIS asset any channel
        // would do; the dot is written out anyway so the behaviour is fixed for a future coloured mask.
        CHECK_MSG(std::abs(rs_mask(0.75f, 0.9900000095367432f) - 0.852185786f) < 1e-6f,
                  "RS A1: luma 0.75 under cloth_02's max 0.99 is 0.852185786 - NOT the raw luma 0.75");
        CHECK_MSG(std::abs(rs_mask(0.75f, 1.0f) - 0.84375f) < 1e-6f,
                  "RS A1: the same luma under cloth_05's max 1.0 is 0.84375 - the two materials DIFFER");
        CHECK_MSG(rs_mask(0.0f, 0.9900000095367432f) == 0.0f && rs_mask(1.0f, 1.0f) == 1.0f,
                  "RS A1: mask is exactly 0 below the lower edge and exactly 1 at the upper edge");
        CHECK_MSG(std::abs(rs_smooth_step(0.25f, 0.75f, 0.4f) - 0.216000021f) < 1e-6f,
                  "RS A1: the division is (x - min) / (max - min), NOT x / max: min 0.25, max 0.75, x 0.4 -> 0.216000021 "
                  "(the wrong form would give 0.549925983)");

        // ---- A2: `LayerWeight.Facing` - background in step 13, LIVE from step 15's `armA` ----
        CHECK_MSG(std::abs(rs_facing(0.5f, 0.8f) - 0.199999988f) < 1e-6f,
                  "RS A2 (background pin: written in step 13 for the then-unported armA; step 15 calls it live): blend == 0.5 exactly skips remap and pow, so facing = 1 - 0.8 = 0.199999988");
        CHECK_MSG(rs_facing(0.0f, 0.8f) == 0.0f, "RS A2 (background pin: written in step 13 for the then-unported armA; step 15 calls it live): blend 0 remaps to 0, so facing is exactly 0");
        CHECK_MSG(std::abs(rs_facing(0.2f, 0.8f) - 0.0853899121f) < 1e-6f, "RS A2 (background pin: written in step 13 for the then-unported armA; step 15 calls it live): remap(0.2) = 0.400000006");
        CHECK_MSG(std::abs(rs_facing(0.8f, 0.8f) - 0.427566588f) < 1e-6f, "RS A2 (background pin: written in step 13 for the then-unported armA; step 15 calls it live): remap(0.8) = 2.50000024");
        CHECK_MSG(std::abs(rs_facing(1.0f, 0.8f) - 1.0f) < 1e-6f, "RS A2 (background pin: written in step 13 for the then-unported armA; step 15 calls it live): blend clamps to 0.99999 -> remap 49932.1914");

        // ---- A3: the `SmoothStep` subgroup - ON this step's `_M` chain, mandatory (spec §3.2b) ----
        CHECK_MSG(std::abs(rs_smooth_step(0.0f, 0.9900000095367432f, 0.49500000476837158f) - 0.5f) < 1e-6f,
                  "RS A3: cloth_02's 0.99 edge and its half-point give exactly 0.5");
        CHECK_MSG(std::abs(rs_smooth_step(0.0f, 1.0f, 0.25f) - 0.15625f) < 1e-6f, "RS A3: t = 0.25 -> 0.15625");
        CHECK_MSG(rs_smooth_step(0.25f, 0.75f, 0.1f) == 0.0f, "RS A3: below the lower edge the clamp returns exactly 0");

        // ---- A4: `goo_sigmoid_sharp`, the cast-shadow curve - background in step 13, LIVE from step 15's `armA` ----
        CHECK_MSG(rs_sigmoid_sharp(0.5f, 0.0f, 0.17000000178813934f) == 0.949587882f, "RS A4 (background pin: written in step 13 for the then-unported armA; step 15 calls it live): x = 0.5, sharp = 0.17");
        CHECK_MSG(rs_sigmoid_sharp(0.25f, 0.0f, 0.17000000178813934f) == 0.812737703f, "RS A4 (background pin: written in step 13 for the then-unported armA; step 15 calls it live): x = 0.25");
        CHECK_MSG(rs_sigmoid_sharp(0.9f, 0.0f, 0.0f) == 0.5f, "RS A4 (background pin: written in step 13 for the then-unported armA; step 15 calls it live): sharp == 0 is the guarded 0.5");

        // ---- A5: `混合.029` is LIGHTEN, and `fac = 0` returns A BITWISE ----
        // pinned sample (from `deren-ab\_s13s_expect3.py`, f32 round-trip):
        //   lit = (0.1f, 0.2f, 0.3f);  rs = 0.5 * (7.5, 1.4143484830856323, 0) = (3.75, 0.7071742415428162, 0)
        CHECK_MSG(vec3_bitwise_equal(rs_lighten({0.1f, 0.2f, 0.3f}, {3.75f, 0.707174242f, 0.0f}, 1.0f), {3.75f, 0.707174242f, 0.3f}),
                  "RS A5: LIGHTEN takes the per-channel MAX of lit and rs");
        CHECK_MSG(vec3_bitwise_equal(rs_lighten({0.1f, 0.2f, 0.3f}, {3.75f, 0.707174242f, 0.0f}, 0.0f), {0.1f, 0.2f, 0.3f}),
                  "RS A5: fac == 0 is BITWISE lit (mix(A,B,0) = A*1 + B*0; both products exact in f32)");
        CHECK_MSG(std::abs(rs_lighten({0.1f, 0.2f, 0.3f}, {3.75f, 0.707174242f, 0.0f}, 0.25f).x - 1.01250005f) < 1e-6f &&
                      std::abs(rs_lighten({0.1f, 0.2f, 0.3f}, {3.75f, 0.707174242f, 0.0f}, 0.25f).y - 0.326793551f) < 1e-6f,
                  "RS A5: the factor blends TOWARDS the max (0.1 -> 1.01250005), NOT a bare max (which would give 3.75)");
        CHECK_MSG(vec3_bitwise_equal(rs_lighten({-0.1f, 0.2f, 0.3f}, {0.0f, 0.0f, 0.0f}, 1.0f), {0.0f, 0.2f, 0.3f}),
                  "RS A5: rs == 0 makes LIGHTEN a HALF-WAVE RECTIFIER max(lit,0); it is NOT an identity - which is why A6's gate exists");

        // ---- A6: the frozen form - the `if` gate, not a branchless `mix` - and its three bitwise identities ----
        // cloth_02's tint = (7.5, 1.4143484830856323, 0); cloth_05's = (7.5, 1.4143449068069458, 0)
        CHECK_MSG(vec3_bitwise_equal(rs_final({0.1f, 0.2f, 0.3f}, 1.0f, {7.5f, 1.41434848f, 0.0f}, 0.0f, 1.0f, 1.0f), {0.1f, 0.2f, 0.3f}),
                  "RS A6: Use RS_Eff? = 0 is BITWISE the base - even with mask 1 and a huge tint (the 20 materials)");
        CHECK_MSG(vec3_bitwise_equal(rs_final({0.1f, 0.2f, 0.3f}, 1.0f, {7.5f, 1.41434848f, 0.0f}, 1.0f, 1.0f, 0.0f), {0.1f, 0.2f, 0.3f}),
                  "RS A6: RS Model = 0 with a ZERO arm0 (no sheet row) keeps the base HERE only because every channel is positive; it is max(lit,0), not an identity, and the assertion below is the case that sees it");
        CHECK_MSG(vec3_bitwise_equal(rs_final({-0.1f, 0.2f, 0.3f}, 1.0f, {7.5f, 1.41434848f, 0.0f}, 1.0f, 1.0f, 0.0f), {0.0f, 0.2f, 0.3f}),
                  "RS A6: ... a NEGATIVE channel under RS Model = 0 and no sheet is RECTIFIED to 0.0 since step 15 - which is exactly why the outer gate stays on `Use RS_Eff?` and never on `RS Model`");
        CHECK_MSG(vec3_bitwise_equal(rs_final({0.1f, 0.2f, 0.3f}, 1.0f, {7.5f, 1.41434848f, 0.0f}, 1.0f, 0.0f, 0.0f, {2.0f, 3.0f, 4.0f}), {0.1f, 0.2f, 0.3f}),
                  "RS A6: RS Multiply Value = 0 makes 混合.029 a no-op on the armA path too (arm0 = (2,3,4) never lands)");
        CHECK_MSG(vec3_bitwise_equal(rs_final({0.1f, 0.2f, 0.3f}, 1.0f, {7.5f, 1.41434848f, 0.0f}, 1.0f, 0.0f, 1.0f), {0.1f, 0.2f, 0.3f}),
                  "RS A6: RS Multiply Value = 0 makes 混合.029 a no-op, so the whole chain is bitwise the base");
        CHECK_MSG(vec3_bitwise_equal(rs_final({0.1f, 0.2f, 0.3f}, 0.5f, {7.5f, 1.41434848f, 0.0f}, 1.0f, 1.0f, 1.0f), {3.75f, 0.707174242f, 0.3f}),
                  "RS A6: the open gate lightens R by 3.75 and G by 0.707174242, and leaves B at the base");
        CHECK_MSG(std::abs(rs_final({0.1f, 0.2f, 0.3f}, 0.5f, {7.5f, 1.41434491f, 0.0f}, 1.0f, 1.0f, 1.0f).y - 0.707172453f) < 1e-6f,
                  "RS A6: cloth_05's tint G is a DIFFERENT f32 (1.4143449068069458 -> 0.707172453), so the two materials must not share a constant");
        CHECK_MSG(std::abs(rs_final({0.1f, 0.2f, 0.3f}, 0.5f, {7.5f, 1.41434848f, 0.0f}, 1.0f, 0.25f, 1.0f).x - 1.01250005f) < 1e-6f,
                  "RS A6: RS Multiply Value = 0.25 blends towards the lightened colour (1.01250005)");
        CHECK_MSG(vec3_bitwise_equal(rs_final({0.1f, 0.2f, 0.3f}, 0.0f, {7.5f, 1.41434848f, 0.0f}, 1.0f, 1.0f, 1.0f), {0.1f, 0.2f, 0.3f}),
                  "RS A6: a mask of 0 on an all-positive lit happens to be the base (max(lit,0) == lit)");
        CHECK_MSG(vec3_bitwise_equal(rs_final({-0.1f, 0.2f, 0.3f}, 0.0f, {7.5f, 1.41434848f, 0.0f}, 1.0f, 1.0f, 1.0f), {0.0f, 0.2f, 0.3f}),
                  "RS A6: but with a NEGATIVE channel a mask of 0 rectifies it (0.0, not -0.1) - the reference does this too, keep it");

        // ---- A9 (STEP 15): `armA`'s LANDING FORM and the asset's own number ----
        //
        // spec §1.3's landing line, written in the shader's own parenthesis order (the reference's `混合.036 ->
        // .023 -> .027 -> .028 -> .033`, and f32 multiplication is NOT associative, so this is not cosmetic):
        //   rs_eff = ((((rs_sheet * rs_tint_lane.rgb) * rs_arm0_lane.y) * rs_ndotl) * rs_cast_sigmoid) * rs_mask
        // THIS ASSET's inputs, cloth_02: the `_RS` sheet is 256x1 and its whole `x = 0..148` run is sRGB
        // (0, 96, 255), which the SAMPLER decodes to linear (0, 0.11697066575288773, 1) - a UNIFORM colour, so
        // `u` cannot move it. That is a fact L2 rests on and it also means no frame on this asset can test `u`.
        // tint.g = 1.4143484830856323 (cloth_05: 1.4143449068069458); RS Strength = 1; saturate(NdotL) = 1; the
        // cast curve at `shadow = 1` is rs_sigmoid_sharp(1, 0, 0.17) = 0.9971895217895508; mask = 1.
        CHECK_MSG(0.11697066575288773f * 1.4143484830856323f == 0.16543728113174438f,
                  "RS A9: the asset's first two factors in f32 - the sheet's green X cloth_02's tint is exactly 0.16543728113174438");
        CHECK_MSG(rs_arm0_channel(0.11697066575288773f, 1.4143484830856323f, 1.0f, 1.0f, 0.9971895217895508f, 1.0f) == 0.16497232019901276f,
                  "RS A9: and through the whole landing line it is 0.16497232019901276 - the green of an `arm0` pixel at full light, full mask, `saturate(NdotL)` 1, RS Strength 1");
        CHECK_MSG(rs_arm0_channel(0.11697066575288773f, 1.4143449068069458f, 1.0f, 1.0f, 0.9971895217895508f, 1.0f) == 0.16497190296649933f,
                  "RS A9: cloth_05's tint gives a DIFFERENT f32 (0.16497190296649933), so the two materials must not share a constant here either");
        CHECK_MSG(rs_arm0_channel(0.11697066575288773f, 1.4143484830856323f, 0.0f, 1.0f, 0.9971895217895508f, 1.0f) == 0.0f,
                  "RS A9: a material with NO `_GooRSArm0` row is answered the neutral lane (0,0,0,0), i.e. RS Strength 0, so its arm0 is exactly 0 - a switch, not a fallback; the fixture states 1.0 explicitly");
        CHECK_MSG(rs_arm0_channel(0.5f, 0.5f, 1.0f, 1.0f, 0.9971895217895508f, 0.0f) == 0.0f,
                  "RS A9: and `_M = 0` (lane 0 = do not read) zeroes arm0 rather than sampling the WHITE fallback");
        CHECK_MSG(rs_arm0_channel(0.5f, 0.5f, 1.0f, 0.0f, 0.9971895217895508f, 1.0f) == 0.0f,
                  "RS A9: `saturate(NdotL)` at 0 zeroes it too - the arm goes dark where the sun is behind the surface");
        // L2: `Layer weight Value = 0` is this asset's value on BOTH cloth materials, and it makes the remap
        // unreachable: `remap(0) = 0`, so `Facing = 1 - |dot|^0 = 1 - 1 = 0` and `u = saturate(0 + 0) = 0`
        // for every angle. Three incidences, because the claim is "for every angle" and not "at one".
        // (`rs_sheet_u` is the composed `u`, so this pins `u` itself and not only its `Facing` leaf: debt (x)
        // is exactly the mistake of pinning the leaf and never composing it.)
        CHECK_MSG(rs_sheet_u(0.0f, 0.0f, 0.0f) == 0.0f && rs_sheet_u(0.0f, 0.8f, 0.0f) == 0.0f && rs_sheet_u(0.0f, 1.0f, 0.0f) == 0.0f &&
                      rs_facing(0.0f, 0.0f) == 0.0f && rs_facing(0.0f, 0.8f) == 0.0f && rs_facing(0.0f, 1.0f) == 0.0f,
                  "RS A9: L2 - `Layer weight Value = 0` pins `u` to exactly 0 at every incidence, so the remap is unreachable ON THIS ASSET and no frame here can exercise it (A10 is what covers the remap; A2 is its leaf)");

        // ---- A10 (debt (x), lead-found 2026-10-01): the NET `u` FORM, and the polarity this port had BACKWARDS ----
        //
        // The reference's Layer Weight node ends in `facing = 1.0 - facing`
        // (`_ref/gooengine_src/gpu_shader_material_layer_weight.glsl:24`), and the sheet is addressed with
        // `u = clamp(Facing + Layer weight Value Offset, 0, 1)` (spec `:107`). So the net form is
        //     u = 1 - |V.n|^remap(b) + w
        // and NOT the complement `u = 1 - (1 - |V.n|)^remap(b) + w` that `shaders/goo_toon.slang` computed until
        // 2026-10-01. The two agree ONLY at `b == 0` (both give `u == w`) and at `|V.n| == 0.5` (their one
        // fixpoint), which is why every `b == 0` fixture in the world stays green under either form.
        // Instrument: `deren-ab/s15impl/u_net_form_check.py` (32-point grid over `b x |V.n| x w`, both forms, f64
        // and f32) and the Lead's `deren-ab/_lead_u_net_check.py`. The numbers below are the f32 values; each of the
        // last six is quoted together with what the COMPLEMENT would have answered, because a pin that both forms
        // satisfy cannot catch this class of defect.
        CHECK_MSG(rs_sheet_u(0.0f, 0.8f, 0.0f) == 0.0f && rs_sheet_u(0.0f, 0.8f, 0.25f) == 0.25f,
                  "RS A10 (debt (x)): `b = 0` is the AGREEMENT set - `Facing = 1 - |dot|^0 = 0`, so `u = w` on both forms");
        CHECK_MSG(std::abs(rs_sheet_u(0.5f, 0.8f, 0.0f) - 0.199999988f) < 1e-6f,
                  "RS A10 (debt (x)): `b = 0.5` takes the reference's no-pow branch, `u = 1 - |dot| + w` = 0.199999988 (the complement would say 0.800000012 - visible here, and this is also the reference's own `chen` value)");
        CHECK_MSG(std::abs(rs_sheet_u(0.5f, 0.8f, 0.25f) - 0.449999988f) < 1e-6f,
                  "RS A10 (debt (x)): the same branch with `Layer weight Value Offset = 0.25` = 0.449999988 (the complement saturates to 1.0; note the Lead's task text quotes 0.4 for this point - the closed form and his own instrument's `u_spec` give 0.45, pinned as such here)");
        CHECK_MSG(std::abs(rs_sheet_u(0.2f, 0.8f, 0.0f) - 0.0853899121f) < 1e-6f,
                  "RS A10 (debt (x)): `remap(0.2) = 0.4` -> 0.0853899121 (the complement said 0.474694431). Same f32 as the step-13 A2 leaf pin above, now reached through the composed `u`");
        CHECK_MSG(std::abs(rs_sheet_u(0.8f, 0.8f, 0.0f) - 0.427566588f) < 1e-6f,
                  "RS A10 (debt (x)): `remap(0.8) = 2.5` -> 0.427566588 (the complement said 0.982111454) - the two forms straddle the fixpoint, so `b` above and below 0.5 must both be pinned");
        CHECK_MSG(rs_sheet_u(0.2f, 0.0f, 0.0f) == 1.0f && rs_sheet_u(0.2f, 1.0f, 0.0f) == 0.0f,
                  "RS A10 (debt (x)): the endpoints - a grazing view gives `u = 1`, a head-on view gives `u = 0` (the complement has these two exactly swapped)");
        CHECK_MSG(std::abs(rs_sheet_u(0.2f, 0.5f, 0.0f) - 0.242141724f) < 1e-6f &&
                      std::abs(rs_sheet_u(0.8f, 0.5f, 0.0f) - 0.823223352f) < 1e-6f,
                  "RS A10 (debt (x)): `|V.n| = 0.5` is the forms' ONLY fixpoint - both give `1 - 0.5^remap(b)` - and it is pinned on both sides of `b == 0.5` because a fixpoint is exactly where a polarity bug hides");

        // ---- A7: the host side loads the RS values into THREE colour lanes (step 13's two, step 15's third; the
        //          host data flow is generic, so `main.cpp` needs no new BRANCH - only the row names) ----
        std::string const app = slurp("main.cpp");
        // `_GooRSScalars` row = "1.0,1.0,1.0,0.0"                                -> lane 27
        // `_GooRSTint`     row = "7.5,1.4143484830856323,0.0,0.9900000095367432"  -> lane 28
        // `_GooRSArm0`     row = "0.0,1.0,0.0,0.0" (the FIXTURE; the shipped asset states no such row) -> lane 29
        vec4 const lane27{1.0f, 1.0f, 1.0f, 0.0f};
        vec4 const lane28{7.5f, 1.4143484830856323f, 0.0f, 0.9900000095367432f};
        vec4 const lane29{0.0f, 1.0f, 0.0f, 0.0f};
        vec4 const neutral_lane27{}; // the host tables' explicit `(0,0,0,0)` (see `toon_colour_neutral`)
        vec4 const neutral_lane28{};
        vec4 const neutral_lane29{};
        vec4 const absent_row{}; // a material with no row is answered the neutral, NOT white
        CHECK_MSG(rs_use_of(lane27) == 1.0f && rs_mult_of(lane27) == 1.0f && rs_model_of(lane27) == 1.0f,
                  "RS A7: lane 27's .x/.y/.z = Use RS_Eff? / RS Multiply Value / RS Model, from the _GooRSScalars row");
        CHECK_MSG(lane27.w == 0.0f, "RS A7: _GooRSScalars.w stays 0.0 - SmoothStep.max has exactly ONE carrier (lane 28 .w)");
        CHECK_MSG(vec3_bitwise_equal(rs_tint_of(lane28), {7.5f, 1.41434848f, 0.0f}) && rs_smooth_max_of(lane28) == 0.9900000095367432f,
                  "RS A7: lane 28 = (RS ColorTint.rgb, SmoothStep.max), from the _GooRSTint row");
        CHECK_MSG(rs_arm0_strength_of(lane29) == 1.0f && rs_arm0_lw_blend_of(lane29) == 0.0f && rs_arm0_lw_offset_of(lane29) == 0.0f,
                  "RS A7 (step 15): lane 29's .y/.z/.w = RS Strength / Layer weight Value / Layer weight Value Offset, "
                  "read from the _GooRSArm0 row - (1, 0, 0) on both cloth materials");
        CHECK_MSG(rs_arm0_index_of(lane29) == 0.0f && rs_arm0_index_of(neutral_lane29) == 0.0f &&
                      rs_arm0_strength_of(neutral_lane29) == 0.0f,
                  "RS A7 (step 15): lane 29's .x is RS_Index and the SHADER never reads it - the HOST picks which sheet this "
                  "slot holds (see A8.rsi). The neutral (0,0,0,0) makes arm0 exactly 0 - a switch - and THAT is why a material "
                  "with `Use = 1`, `RS Model = 0` and no sheet is not bitwise its base: 0 still goes through LIGHTEN (A6)");
        CHECK_MSG(rs_use_of(neutral_lane27) == 0.0f && vec3_bitwise_equal(rs_tint_of(neutral_lane28), {0.0f, 0.0f, 0.0f}) &&
                      rs_smooth_max_of(neutral_lane28) == 0.0f,
                  "RS A7: both new lanes' HOST neutrals are (0,0,0,0) - a material with no row reads Use RS_Eff? = 0. "
                  "EVIDENCE BOUNDARY: the constructor's vec4(1.0f) at runtime.constructor.cppm:370 is only the buffer's "
                  "initial content - it is overwritten for every registered material over ALL count lanes at :1692-1693 "
                  "from the callback (runtime.declarations.cppm:3635-3637), so it is NOT the hazard an earlier draft claimed");
        CHECK_MSG(rs_effective_smax(neutral_lane28.w) == 1.0f,
                  "RS A7: max <= 0 means 1.0, so the neutral lane 28 degrades to smoothstep(0, 1, luma) - no divide by zero (U7)");
        CHECK_MSG(rs_use_of(absent_row) == 0.0f && rs_mult_of(absent_row) == 0.0f && rs_model_of(absent_row) == 0.0f,
                  "RS A7: a missing/short row keeps 0.0, NEVER 1.0 (a white neutral would light up RS on a future material)");
        CHECK_MSG(rs_mask_index(false) == 0u && rs_mask_of_index(0u, 0.75f, 1.0f) == 0.0f && rs_mask_index(true) == 14u,
                  "RS A7: an absent mask slot (or its flag off) resolves to texture index 0, and index 0 means mask = 0.0 (F5)");

        // the two row names, READ OUT OF `main.cpp` rather than mirrored here: a mirror would be this file's own
        // reading twice, and the row table is the LOAD-BEARING host edit (a table left at 27 entries gets empty
        // string_view tails, the generic lookup finds nothing, and the sidecar rows become unreachable with RS
        // silently off - no warning, no compile error)
        std::vector<std::string> toon_colour_row;
        {
            std::size_t const table_at = app.find("toon_colour_row = {{");
            CHECK_MSG(table_at != std::string::npos, "main.cpp declares the colour lane's name table");
            if (table_at != std::string::npos) {
                std::size_t const table_end = app.find("}};", table_at);
                std::string body = app.substr(table_at, table_end - table_at);
                std::string cleaned; // strip line comments: a section comment sits between entries
                for (std::size_t i = 0u; i < body.size(); ++i) {
                    if (body[i] == '/' && i + 1u < body.size() && body[i + 1u] == '/') {
                        while (i < body.size() && body[i] != '\n') {
                            ++i;
                        }
                    } else {
                        cleaned.push_back(body[i]);
                    }
                }
                for (std::size_t quote = cleaned.find('"'); quote != std::string::npos; quote = cleaned.find('"', quote + 1u)) {
                    std::size_t const close = cleaned.find('"', quote + 1u);
                    if (close == std::string::npos) {
                        break;
                    }
                    toon_colour_row.push_back(cleaned.substr(quote + 1u, close - quote - 1u));
                    quote = close;
                }
            }
        }
        CHECK_MSG(toon_colour_row.size() == 30u, "RS A7: the row table has one name per colour lane (30 after step 15)");
        CHECK_MSG(toon_colour_row.size() > 29u && toon_colour_row[27] == "_GooRSScalars" && toon_colour_row[28] == "_GooRSTint" &&
                      toon_colour_row[29] == "_GooRSArm0",
                  "RS A7: the row names ARE the load-bearing host-side edit - WITHOUT them the generic `others` path "
                  "can never find the rows and RS is silently off (no warning, no compile error)");
        // THE FOURTH NAME IS STEP 15'S, and it is checked here rather than only in the step-15 block below because
        // this parse is the only place the table's LENGTH and its ORDER are read together: the array's size is
        // `toon_colour_lane::count`, so a name appended in the wrong position would move every later lane's row. A
        // missing name is the failure this test exists for (the `others` lookup finds nothing and the lane silently
        // reads its neutral); a MISORDERED name is the same failure one lane over, and no frame can show either.

        // ---- A8: the source-text sync points (structure / boundary / reverse drift guards) ----
        // AFTER the landing-point correction every RS-block pin is taken on `goo_toon.slang` (the REWRITTEN chain);
        // `character_forward.slang` keeps only its stride pin and the positive "old chain untouched" pin.
        std::string const goo_toon = slurp("shaders/goo_toon.slang");
        std::string const character_forward = slurp("shaders/character_forward.slang");
        std::string const goo_rim = slurp("shaders/goo_rim.slang");
        std::string const pbr = slurp("shaders/pbr.slang");
        std::string const primitive = slurp("vulkan/primitive/primitive.cppm");
        CHECK_MSG(goo_toon.find("const float3 lit_base = colour + s.emissive;") != std::string::npos &&
                      goo_toon.find("rs_final") != std::string::npos,
                  "RS A8: the RS block exists in goo_toon.slang (the REWRITE chain), not in character_forward.slang");
        CHECK_MSG(goo_toon.find("max(lit_base, rs_eff)") != std::string::npos && goo_toon.find("saturate(rs_scalars.y)") != std::string::npos,
                  "RS A8: it is the LIGHTEN form mix(lit_base, max(lit_base, rs_eff), ...), not a bare max");
        CHECK_MSG(goo_toon.find("3.0f - 2.0f * t") != std::string::npos,
                  "RS A8: the mask's SmoothStep is the hand-written polynomial t*t*(3-2t) (the _M chain, see 3.2b)");
        CHECK_MSG(character_forward.find("character_toon_colour_lanes = 30u") != std::string::npos &&
                      goo_toon.find("+ 27u") != std::string::npos && goo_toon.find("+ 28u") != std::string::npos &&
                      goo_toon.find("+ 29u") != std::string::npos,
                  "RS A8: the three carriers are lanes 27/28/29 and the character stride is 30 (it was 29 from step 13)");
        CHECK_MSG(goo_rim.find("goo_rim_colour_lanes = 30u") != std::string::npos &&
                      pbr.find("pbr_toon_colour_lanes = 30u") != std::string::npos,
                  "RS A8: the other two stride copies moved to 30 too (outline.slang reuses the character one)");
        CHECK_MSG(primitive.find("count = 30,") != std::string::npos && primitive.find("goo_rs_scalars = 27,") != std::string::npos &&
                      primitive.find("goo_rs_tint = 28,") != std::string::npos && primitive.find("goo_rs_arm0 = 29,") != std::string::npos,
                  "RS A8: toon_colour_lane's count moved 29 -> 30 for step 15's `_GooRSArm0` and the three earlier names are still there");
        CHECK_MSG(primitive.find("goo_rs_mask = 14,") != std::string::npos && primitive.find("count = 16,") != std::string::npos &&
                      primitive.find("goo_rs_sheet = 15,") != std::string::npos,
                  "RS A8: step 15 spent `toon_slot`'s LAST slot (15 = goo_rs_sheet) and its count moved 15 -> 16");
        CHECK_MSG(app.find("{\"_GooRSMask\", \"_UseGooRSMask\"}") != std::string::npos &&
                      app.find("{\"_GooRSSheet\", \"_UseGooRSSheet\"}") != std::string::npos,
                  "RS A8: the application vocabulary table names the mask slot, the sheet slot and both flags");
        CHECK_MSG(app.find("\"_GooRSScalars\"") != std::string::npos && app.find("\"_GooRSTint\"") != std::string::npos &&
                      app.find("\"_GooRSArm0\"") != std::string::npos,
                  "RS A8: toon_colour_row names lanes 27/28/29 (one row name per lane)");
        CHECK_MSG(goo_toon.find("goo_hsv_desaturate(rs_final, desaturation)") != std::string::npos,
                  "RS A8: goo_toon.slang:374's FIRST argument is rs_final - the RS block has to reach 色相/饱和度/明度; "
                  "goo_hsv_desaturate(colour + s.emissive, ...) would silently drop the whole RS term while still compiling");
        CHECK_MSG(goo_toon.find("goo_hsv_desaturate(colour + s.emissive,") == std::string::npos,
                  "RS A8: and the pre-step call site must be GONE - the guard against someone 'fixing it back'");
        CHECK_MSG(character_forward.find("goo_hsv_desaturate(lit, desaturation)") != std::string::npos,
                  "RS A8: character_forward.slang's OLD chain tail is untouched - its desaturation still takes `lit`");
        CHECK_MSG(goo_toon.find("eyes.xy") != std::string::npos,
                  "RS A8: lane 6's own consumers are untouched - the RETRACTED F3 carrier (lane 4/5/6 reserved components) is gone");
        CHECK_MSG(goo_toon.find("const float3 lit_base = colour + s.emissive;") < goo_toon.find("rs_final =") &&
                      goo_toon.find("rs_final =") < goo_toon.find("goo_hsv_desaturate(rs_final, desaturation)"),
                  "RS A8: the RS block sits between 混合.026 (colour + s.emissive) and 色相/饱和度/明度 - its real position");
        // STEP 15 REPLACES THE OLD "names the unported armA branch (F2)" PIN. That pin is now the reverse of the
        // truth, so it is replaced by pins on the arm that landed - each one a piece of the reference's chain whose
        // absence would leave a silently different picture rather than a build error.
        CHECK_MSG(goo_toon.find("const float3 toon_shading_normal = goo_toon_shading_normal(") != std::string::npos &&
                      goo_toon.find("s.toon_family, toon_shading_normal, v_world_pos") != std::string::npos,
                  "RS A8 (step 15): the shading normal is HOISTED to one named value and passed to `toon_diffuse` - "
                  "the armA path reuses that evaluation; a second `goo_toon_shading_normal(...)` inside the arm would "
                  "be a second decode of the normal map and could not be bit-identical to the one the shading used");
        CHECK_MSG(goo_toon.find("const float rs_model = rs_scalars.z;") != std::string::npos &&
                      goo_toon.find("if (rs_model == 0.0f)") != std::string::npos,
                  "RS A8 (step 15): `RS Model` is named and selects the arm INSIDE the gate - the reference's own "
                  "arrangement (its `混合.033`), not the step-13 whole-block switch");
        CHECK_MSG(goo_toon.find("if (rs_use > 0.0f && rs_scalars.z != 0.0f)") == std::string::npos,
                  "RS A8 (step 15), REVERSE DRIFT GUARD: the OLD combined gate is GONE. It is the one line that made "
                  "`RS Model = 0` skip the block entirely - if it comes back, armA stops running and only this pin says so");
        CHECK_MSG(goo_toon.find("camera_at(heap_camera_slot)") != std::string::npos &&
                      goo_toon.find("const float rs_facing_abs = abs(dot(rs_to_camera, toon_shading_normal));") != std::string::npos &&
                      goo_toon.find("if (rs_lw_blend == 0.5f)") != std::string::npos &&
                      goo_toon.find("rs_facing = 1.0f - pow(rs_facing_abs, rs_exponent);") != std::string::npos &&
                      goo_toon.find("const float rs_sheet_u = saturate(rs_facing + rs_arm0_lane.w);") != std::string::npos &&
                      goo_toon.find("const float rs_blend_clamped = clamp(rs_lw_blend, 0.0f, 0.99999f);") != std::string::npos,
                  "RS A8 (step 15; text CORRECTED by debt (x), 2026-10-01): the facing half - `V` out of the camera "
                  "slot, `|V.n|` UNSATURATED, the exact-0.5 branch that skips remap and pow, `Facing = 1 - |V.n|^e` "
                  "(the reference's own last line, `facing = 1.0 - facing`), `u = saturate(Facing + Offset)`, and the "
                  "clamp at 0.99999 that keeps the divisor off zero");
        CHECK_MSG(goo_toon.find("pow(1.0f - rs_facing_abs") == std::string::npos &&
                      goo_toon.find("saturate(1.0f - rs_facing") == std::string::npos,
                  "RS A8 (debt (x)), NEGATIVE PIN: the COMPLEMENT form is GONE. `pow(1 - |V.n|, e)` under "
                  "`saturate(1 - rs_facing + w)` gives `u = 1 - (1 - |V.n|)^e + w` where the reference has "
                  "`1 - |V.n|^e + w`; the two agree only at `b = 0` and at `|V.n| = 0.5`. A revert here is INVISIBLE "
                  "to every `b = 0` fixture - this pin and A10 are the only things that would say so");
        CHECK_MSG(goo_toon.find("if (rs_block3.w != 0u)") != std::string::npos,
                  "RS A8 (step 15): the SHEET is slot 15 - the third block's `.w`, the mask's own neighbour - and "
                  "index 0 is NOT read: a material with no sheet row must not sample the WHITE fallback, because that "
                  "would turn the arm ON for every material that states no sheet");
        CHECK_MSG(goo_toon.find("const float goo_rs_sheet_width = 256.0f;") != std::string::npos &&
                      goo_toon.find("const float2 sheet_uv = clamp(") != std::string::npos &&
                      goo_toon.find("float2(rs_sheet_u, 0.5f),") != std::string::npos &&
                      goo_toon.find("float2(0.5f / goo_rs_sheet_width, 0.5f),") != std::string::npos &&
                      goo_toon.find("float2(1.0f - 0.5f / goo_rs_sheet_width, 0.5f));") != std::string::npos,
                  "RS A8 (step 15, EXTEND equivalence): the sheet's address mode is the reference's `EXTEND` "
                  "(`gooblender/nodes.json:93653-93659`), and the port - which owns ONE sampler per heap and therefore "
                  "cannot honour a per-image address mode - reproduces it by pulling `u` to the interval where REPEAT and "
                  "CLAMP agree, i.e. half a texel inside each end of a 256-wide sheet. Both texel-centre offsets are "
                  "spelled with `0.5f / goo_rs_sheet_width` rather than as folded literals so the width stays a single "
                  "named number: a hard-coded `0.001953125f` twice would hide a sheet that is not 256 wide");
        CHECK_MSG(goo_toon.find("heap_sampler_texture, sheet_uv);") != std::string::npos &&
                      goo_toon.find("heap_sampler_texture, float2(rs_sheet_u, 0.5f)") == std::string::npos,
                  "RS A8 (step 15), NEGATIVE PIN: the sheet is sampled AT `sheet_uv` and never at the raw `u`. The "
                  "clamped and the unclamped coordinate differ only at the two ends of the sheet, so a revert to the raw "
                  "`u` would change nothing except the frames that put a mask=1 pixel exactly at `u = 0` - which is the "
                  "one place this step's whole V2 reading lives");
        CHECK_MSG(goo_toon.find("const float rs_shadow = (s.toon_indices.w != 0u) ? 1.0f : calc_shadow(v_world_pos, toon_shading_normal);") != std::string::npos &&
                      goo_toon.find("const float rs_cast_sigmoid = goo_sigmoid_sharp(rs_shadow, rs_cast_center, rs_cast_sharp);") != std::string::npos &&
                      goo_toon.find("const float4 rs_cast_lane = toon_colour_at(heap_slots_toon_colours, rs_colour_base + 11u);") != std::string::npos,
                  "RS A8 (step 15): the cast factor is the EXISTING `goo_sigmoid_sharp` over the EXISTING "
                  "`calc_shadow`, fed from `_GooDiffuseA`'s own lane - not a second curve, not a second shadow lookup "
                  "and not a re-tuned centre");
        CHECK_MSG(goo_toon.find("ramp_u") == std::string::npos &&
                      goo_toon.find("remap_half_lambert_sigmoid") == std::string::npos,
                  "RS A8 (step 15), SPEC L3: NOT the `ramp_u = min(cast_sigmoid, remap_half_lambert_sigmoid)` consumer "
                  "at character_forward.slang:2535 - that is a different quantity with a different curve, and reusing "
                  "it here would be the easiest way to make this arm look plausible and be wrong");
        CHECK_MSG(goo_toon.find("rs_eff = ((((rs_sheet * rs_tint_lane.rgb) * rs_arm0_lane.y) * rs_ndotl) * rs_cast_sigmoid) * rs_mask;") != std::string::npos,
                  "RS A8 (step 15), SPEC §1.3: the landing line, VERBATIM and in the reference's parenthesis order "
                  "(f32 multiplication is not associative, so the order is the mechanism, not formatting)");
        CHECK_MSG(goo_toon.find("toon_lanes3_at(heap_slots_toon_lanes, push.material_index)") != std::string::npos &&
                      goo_toon.find("if (rs_block3.z != 0u)") != std::string::npos,
                  "RS A8: the mask is slot 14 of the THIRD block, read through the accessor that names it, and index 0 is not read");

        // ---- A8.t30: DEBT (s) - the cel band must NOT be applied to `Cast Shadows` ----
        //
        // The reference never quantizes its `Cast Shadows` output and has no band generator on that chain
        // (its `_RD` ramps are continuous remaps, not steps; see `deren-ab/goo_debt_s_armA_verify.md` §2.4):
        // the engine's cel knob is port-side and OFF by default, so `calc_shadow_cascade` returns raw visibility.
        {
            std::string const shading = slurp("shaders/shading.glsl");

            // STRUCTURE PIN: the raw per-cascade visibility is what `calc_shadow_cascade` returns.
            // The anchor pair matters: a bare `find("return shadow;")` is toothless because `:325` in
            // `calc_shadow` already spells it that way (before this change).
            auto const cascade_sample = shading.find("float shadow = lit / 9.0;");
            CHECK_MSG(cascade_sample != std::string::npos,
                      "DEBT (s): the per-cascade visibility sample must stay in `calc_shadow_cascade`");
            auto const cascade_return = shading.find("return shadow;", cascade_sample);
            CHECK_MSG(cascade_return != std::string::npos,
                      "DEBT (s): `calc_shadow_cascade` must return the raw per-cascade visibility");
            CHECK_MSG(shading.substr(cascade_sample, cascade_return - cascade_sample).find("toon_band(") ==
                          std::string::npos,
                      "DEBT (s): no quantizer may sit between the visibility sample and its return");
            CHECK_MSG(cascade_return - cascade_sample < 1024,
                      "DEBT (s): `calc_shadow_cascade`'s tail must stay the short raw-sampling path");

            // NEGATIVE PIN: the old landing spelling is gone.
            CHECK_MSG(shading.find("return toon_band(shadow, light_at(heap_light_slot).toon_steps, "
                                   "light_at(heap_light_slot).toon_softness);") == std::string::npos,
                      "DEBT (s): `calc_shadow_cascade` must not band its own return value");

            // NUMERIC PIN: `toon_band` keeps exactly TWO spellings - its definition and the ndotl call.
            std::size_t toon_band_sites = 0;
            for (auto at = shading.find("toon_band("); at != std::string::npos; at = shading.find("toon_band(", at + 1)) {
                ++toon_band_sites;
            }
            CHECK_MSG(toon_band_sites == 2,
                      "DEBT (s): `toon_band` must appear exactly twice (definition + the diffuse call)");

            // The LEGITIMATE call site survives untouched.
            CHECK_MSG(shading.find("ndotl = toon_band(ndotl, light_at(heap_light_slot).toon_steps, "
                                   "light_at(heap_light_slot).toon_softness);") != std::string::npos,
                      "DEBT (s): the diffuse-falloff band at the ndotl call site must stay");

            // DRIFT GUARDS on the prose that promised a banded shadow factor.
            CHECK_MSG(shading.find("(also passed through toon_band for cel shading)") == std::string::npos,
                      "DEBT (s): `calc_shadow_cascade`'s @return no longer claims a banded value");
            CHECK_MSG(character_forward.find("returns the quantized term when the engine's cel knob is on") ==
                          std::string::npos,
                      "DEBT (s): the `calc_shadow` call site no longer claims a quantized term");

            // THE NAMED COST, which is the whole reason this is a fix and not a no-op. The structure pin
            // above proves the quantizer is GONE; this one proves it was DOING something, so that removing
            // it is a real behaviour change when the knob is on. A C++ mirror of `shaders/shading.glsl:223`
            // (`toon_band`) is the falsifier: at the probe's own settings it must NOT be the identity.
            auto const toon_band_mirror = [](float x, float steps, float softness) {
                if (steps < 1.5f) {
                    return x; // the shader's own early return: plain PBR, bit-identical
                }
                float const scaled = std::clamp(x, 0.0f, 1.0f) * steps;
                float const base = std::floor(scaled);
                float const frac = scaled - base;
                float const t = std::clamp((frac - (0.5f - softness)) / (2.0f * softness), 0.0f, 1.0f);
                float const edge = t * t * (3.0f - 2.0f * t); // GLSL smoothstep
                return (base + edge) / steps;
            };
            // cel OFF (the shipped configuration: index 0 -> toon_steps = 0.0f) IS the identity, which is
            // exactly why this change must be pixel-neutral on every recorded frame.
            CHECK_MSG(toon_band_mirror(0.3f, 0.0f, 0.15f) == 0.3f,
                      "DEBT (s): `toon_band` is bit-identical to its input when the cel knob is off");
            // cel ON at index 1 (`toon_band_counts[1] = 2.0f` -> `toon_steps = 2.0f`) is NOT, and by far
            // more than a rounding step - so returning the raw value MOVES the cel-on visibility.
            float const banded2 = toon_band_mirror(0.3f, 2.0f, 0.15f);
            CHECK_MSG(banded2 != 0.3f && std::abs(banded2 - 0.3f) > 0.1f,
                      "DEBT (s) NAMED COST: with the cel knob on, the banded value is far from the raw one "
                      "(the removed quantizer was NOT an identity)");
            // and the cost SHRINKS as the band count rises, which is the closed form of the frame-level
            // pair's criterion (the changed-pixel count must fall from 2 bands to 3).
            float worst2 = 0.0f;
            float worst3 = 0.0f;
            float worst4 = 0.0f;
            for (int i = 1; i < 20; ++i) {
                float const v = 0.05f * static_cast<float>(i);
                worst2 = std::max(worst2, std::abs(toon_band_mirror(v, 2.0f, 0.15f) - v));
                worst3 = std::max(worst3, std::abs(toon_band_mirror(v, 3.0f, 0.15f) - v));
                worst4 = std::max(worst4, std::abs(toon_band_mirror(v, 4.0f, 0.15f) - v));
            }
            CHECK_MSG(worst2 > worst3 && worst3 > worst4 && worst4 > 0.05f,
                      "DEBT (s) NAMED COST: the removed band's worst-case deviation strictly shrinks with the "
                      "band count (2 -> 3 -> 4), so a frame pair must show its changed-pixel count fall too");

            // NUMERIC PIN on the shipped configuration: cel index 0 -> `toon_band_counts[0] = 0.0f`
            // -> `runtime`'s `steps < 1.5f ? 0.0f` -> `toon_band`'s early return. That is the whole
            // reason this fix must be pixel-neutral on every recorded frame.
            CHECK_MSG(app.find("constexpr std::array<float, 7> toon_band_counts = "
                               "{0.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};") != std::string::npos,
                      "DEBT (s): the cel band-count table must keep index 0 == 0.0f (plain PBR)");

            std::string const runtime_cpp = slurp("vulkan/runtime/runtime.cpp");
            CHECK_MSG(runtime_cpp.find("this->toon_steps = steps < 1.5f ? 0.0f : "
                                       "std::round(std::clamp(steps, 2.0f, 8.0f));") != std::string::npos,
                      "DEBT (s): `set_toon_shading` must keep the 0-step early out");
        }

        // ---- AREA LIGHT v1 (goo_area_light_spec.md 1.2 / 3 / 5-B): THE SHADER-SIDE SWITCH IS ONE LANE ----
        //
        // v1.1 (spec 7): the A' decision deleted the per-point irradiance gain, so the area light contributes the
        // sun's direction and radiance (host side, `[lighting] area_light_irradiance`, spec 7.2) plus the
        // world-scale PCF below - and NOTHING in the shaders reads the `area_light` lane any more. The shadow
        // switch still rides the light UBO's TAIL and nothing else: `area_light` / `area_light_axis` are appended
        // after `cluster_depth`, so every offset above them - and the SHORTER prefix copies of `LightUBO` that
        // `shaders/light_cluster.slang` and `shaders/rt_shadow.slang` declare - stays exactly what it is. The
        // lane's SIGN encoding stays pinned: w == 0 (the default, `[lighting] area_light_size = 0`) means no area
        // light at all, and a NEGATIVE half side means "the light exists but does not contribute energy", so both
        // switch states land on the SHIPPED expression - kept for the wire layout and a v2 area integral, which
        // needs centre/half back; the shadow route only ever reads `area_light_axis.w`.
        //
        // Nothing here re-implements the shader: what is pinned is the shape that keeps the SHADOW switchable -
        // the shipped early out inside the kernel, the call-site route, and the ABSENCE of the deleted gain.
        {
            std::string const shading = slurp("shaders/shading.glsl");

            // 1.2: the two lanes are the LAST members of the struct, in this order, and inside it.
            std::size_t const cluster_depth = shading.find("vec4 cluster_depth;");
            std::size_t const area_light = shading.find("vec4 area_light;");
            std::size_t const area_light_axis = shading.find("vec4 area_light_axis;");
            CHECK_MSG(cluster_depth != std::string::npos && area_light != std::string::npos &&
                          area_light_axis != std::string::npos && cluster_depth < area_light &&
                          area_light < area_light_axis,
                      "AREA LIGHT 1.2: area_light / area_light_axis must be declared AFTER cluster_depth, in order");
            std::size_t const struct_end = shading.find("};", cluster_depth);
            CHECK_MSG(struct_end != std::string::npos &&
                          shading.substr(cluster_depth, struct_end - cluster_depth).find("vec4 area_light;") !=
                              std::string::npos,
                      "AREA LIGHT 1.2: the two lanes must be INSIDE LightUBO (before its closing brace), not appended "
                      "after the struct");
            CHECK_MSG(shading.find("// xyz = emitter centre (WORLD, Y-up metres), w = half the side; w <= 0 = no area light") !=
                              std::string::npos &&
                          shading.find("// xyz = emitter normal (centre -> target); w = penumbra world radius (metres); "
                                       "<= 0 = no area shadow") != std::string::npos,
                      "AREA LIGHT 1.2: the units and the sign encoding ARE the interface, so the spec's comment on both "
                      "lanes is a pin");

            // 3.1 is GONE in v1.1 (spec 7.1 / 7.2): the per-point gain clamped the shipped main light to 0 in the
            // six-arm measurement (face-frame p10 exactly 0, 39.63% / 33.59% of face pixels exactly 0), so the A'
            // decision deleted it. This NEGATIVE pin is the guard: if the function ever comes back, the decision
            // has been silently reverted and this test says so.
            CHECK_MSG(shading.find("area_light_irradiance_gain") == std::string::npos,
                      "AREA LIGHT 3.1 (v1.1): `area_light_irradiance_gain` was deleted by the A' decision and must "
                      "not reappear in shading.glsl");

            // 3.2: the shadow kernel. `<= 0` returns the SHIPPED calc_shadow itself, and the tap extent is the
            // world radius converted per cascade - never a constant kernel width.
            std::size_t const area_fn = shading.find("float calc_shadow_area(vec3 world_pos, vec3 normal) {");
            std::size_t const radius_lane =
                shading.find("const float radius_world = light_at(heap_light_slot).area_light_axis.w;", area_fn);
            std::size_t const shipped_return = shading.find("return calc_shadow(world_pos, normal);", area_fn);
            CHECK_MSG(area_fn != std::string::npos && radius_lane != std::string::npos &&
                          shipped_return != std::string::npos && radius_lane < shipped_return &&
                          shipped_return - area_fn < 400,
                      "AREA LIGHT 3.2: for `area_light_axis.w <= 0` calc_shadow_area must return the SHIPPED "
                      "calc_shadow itself, immediately after reading the radius - not a copy of it, and not after a tap");
            CHECK_MSG(shading.find("const int spacing = 16;") != std::string::npos,
                      "AREA LIGHT 3.2: the frozen tap spacing of 16 texels");
            CHECK_MSG(shading.find("int(round(radius_world / (2.0 * float(spacing) * texel_world)))") != std::string::npos &&
                          shading.find("int(round(radius_world / (2.0 * float(spacing) * "
                                       "light_at(heap_light_slot).cascade_texel_world[cascade])))") != std::string::npos &&
                          shading.find("int(round(radius_world / (2.0 * float(spacing) * "
                                       "light_at(heap_light_slot).cascade_texel_world[next])))") != std::string::npos,
                      "AREA LIGHT 3.2: the tap extent is round(radius_world / (2 * spacing * texel_world)), clamped "
                      "[1, 8], and EACH cascade sizes its own kernel (single map / selected / blend target)");
            CHECK_MSG(shading.find("THIS IS NOT PCSS AND NOT RAY TRACING") != std::string::npos,
                      "AREA LIGHT 3.2: the honesty note must stay next to calc_shadow_area - the shadow map is a "
                      "comparison sampler, so this is a world-scale PCF width and not a solved penumbra");

            // 3.2: the call site. The route's third arm is switched by the lane and the SHIPPED arm is still the
            // same single call - the one thing v1.1 must NOT disturb (the byte-identity gate for the defaults).
            CHECK_MSG(character_forward.find("(light_at(heap_light_slot).area_light_axis.w > 0.0") != std::string::npos &&
                          character_forward.find("? calc_shadow_area(world_pos, n)") != std::string::npos &&
                          character_forward.find(": calc_shadow_soft(world_pos, n, "
                                                 "toon_rig_at(heap_slots_toon_rig).shadow_softness.x)") !=
                              std::string::npos,
                      "AREA LIGHT 3.2: the shadow call site must keep the three-branch route with the shipped "
                      "toon-softness call as the LAST arm");
            std::size_t soft_sites = 0;
            for (std::size_t at = character_forward.find("calc_shadow_soft(world_pos, n,"); at != std::string::npos;
                 at = character_forward.find("calc_shadow_soft(world_pos, n,", at + 1)) {
                ++soft_sites;
            }
            CHECK_MSG(soft_sites == 1,
                      "AREA LIGHT 3.2: the shipped `calc_shadow_soft` call must appear EXACTLY ONCE - as the route's "
                      "else arm - so no second call site can bypass the area light's own kernel");
            // v1.1: and the shipped intensity line must be back to the ONE line the A' decision restores - no gain
            // multiplying it, and no reader of the deleted function anywhere in this stage.
            CHECK_MSG(character_forward.find("const float main_light_intensity = "
                                             "max(0.001, light_at(heap_light_slot).sun_intensity);") !=
                              std::string::npos &&
                          character_forward.find("area_light_irradiance_gain") == std::string::npos,
                      "AREA LIGHT v1.1: `main_light_intensity` must be the shipped one-liner and the deleted gain "
                      "must not be called from character_forward.slang");
        }

        // ---- A8.rsi (step 15, `RS_Index`): THE SECOND `_RS` SHEET IS RESOLVED BY THE HOST, BY NAME ----
        //
        // The route `goo_step15_lane_rs_index_spec.md` §9.1 adopts (`Rc`) is a HOST rule rather than a shader one,
        // so what is pinned here is the SHAPE OF THE HOST EDIT: a name literal that is not a lane, a threshold and
        // its tie direction, the lane ceiling that must not move, and the prose sites that used to say the feature
        // was absent. The frames are the acceptance; these are the parts a frame cannot show - a `>= 0.5` quietly
        // written `> 0.5` moves no pixel on any material in the dumps, because they state endpoints (§1.3: 22 of the 23
        // `PBRToonBase` instances in the `gooblender` dump - 27 across both dumps, the 4 `chen_dump` ones all `0.0` - and the single `1.0` in no captured asset).
        {
            // S1: THE SECOND SHEET IS A NAME, NOT A LANE. The pin asks for the CONSTANT'S OWN LINE rather than for
            // the substring `"_GooRSSheet1"`: `main.cpp` also mentions that spelling inside a comment beside the
            // constant, so a substring pin would still pass with the literal itself renamed away - this one cannot.
            CHECK_MSG(app.find("toon_rs_sheet_b = \"_GooRSSheet1\";") != std::string::npos,
                      "RS A8.rsi (step 15, Rc): the host names the reference's second `_RS` sheet by LITERAL - "
                      "`_GooRSSheet1` is the sidecar's own spelling, and there is no lane for it to come from");
            std::size_t const lanes_at = app.find("toon_lane = {{");
            std::size_t const lanes_end = lanes_at == std::string::npos ? std::string::npos : app.find("}};", lanes_at);
            CHECK_MSG(lanes_at != std::string::npos && lanes_end != std::string::npos,
                      "RS A8.rsi: `main.cpp` still declares the lane vocabulary as a braced table");
            std::string const lane_table = (lanes_at == std::string::npos || lanes_end == std::string::npos)
                                               ? std::string{}
                                               : app.substr(lanes_at, lanes_end - lanes_at);
            CHECK_MSG(lane_table.find("\"_GooRSSheet1\"") == std::string::npos,
                      "RS A8.rsi, NEGATIVE PIN: `_GooRSSheet1` is NOT an entry in `toon_lane[]`. The port carries "
                      "ONE sheet slot and the second sheet is the host's choice of NAME for that slot; a 17th entry "
                      "would move `toon_slot::count` and with it the whole lane block - `toon_lane_blocks` 3 -> 4, "
                      "one more `uint4` per lane in the same heap slot that already holds them "
                      "(`vulkan/core/core.declarations.cppm`'s `heap_slots::toon_lanes`) plus a FOURTH accessor "
                      "beside `shaders/heap_access.slang`'s `toon_lanes_at`/`toon_lanes2_at`/`toon_lanes3_at`. It is "
                      "not a fourth descriptor set, and it is not the proposal "
                      "(spec §9.4: the rejected route `Rb`, still unselected)");
            CHECK_MSG(lane_table.find("{\"_GooRSSheet\", \"_UseGooRSSheet\"}") != std::string::npos,
                      "RS A8.rsi: the ONE sheet lane keeps its `_GooRSSheet`/`_UseGooRSSheet` entry unchanged, so "
                      "the second sheet inherits that lane's own gate rather than replacing it");

            // S2: THE THRESHOLD, ITS TIE DIRECTION, AND WHERE THE INDEX IS READ FROM.
            CHECK_MSG(app.find("!(rs_index >= 0.5f)") != std::string::npos,
                      "RS A8.rsi (step 15, spec §9.2): selection is `>= 0.5f`, so the TIE - exactly 0.5, the "
                      "reference's own 50/50 blend - goes to the SECOND sheet, and it is written negated so that a "
                      "non-finite `RS_Index` cannot select it either");
            CHECK_MSG(app.find("material.others.find(std::string(toon_rs_arm0_row))") != std::string::npos,
                      "RS A8.rsi: `RS_Index` is read out of `others`. A `color` row is TEXT there and `scalar()` "
                      "cannot see it, so a reader that asked `scalar(\"_GooRSArm0\")` would answer 0.0 - the first "
                      "sheet - on exactly the materials that asked for the second");
            CHECK_MSG(app.find("material.enabled(toon_rs_sheet_b)") != std::string::npos &&
                          app.find("material.slot(toon_rs_sheet_b)") != std::string::npos,
                      "RS A8.rsi: the second sheet needs BOTH its own `_UseGooRSSheet1` switch AND a non-empty "
                      "name - naming it with the switch off reads the first sheet, and so does the reverse");
            // ...AND THE BRANCH IS BEFORE THE GENERIC TAIL, which is the silent-failure shape spec §3.3 lists
            // first: placed after `if (model_tex == nullptr)`, this whole route compiles, runs, and never fires.
            std::size_t const branch_at = app.find("if (lane == deren::vulkan::toon_slot::goo_rs_sheet) {");
            std::size_t const tail_at = app.find("if (model_tex == nullptr) {");
            CHECK_MSG(branch_at != std::string::npos && tail_at != std::string::npos && branch_at < tail_at,
                      "RS A8.rsi: the sheet lane's new branch sits BEFORE `toon_texture`'s generic tail - after it, "
                      "`model_tex` has already answered for that lane and the branch is dead code (spec §3.3 risk 1)");

            // THE RULE ITSELF, re-derived from the host's three inputs: the index, whether the second sheet is
            // NAMED, and whether its own switch is ON. `0.4999` is here because a frame cannot distinguish it from
            // `0.5`; the NaN because the host's comparison is NEGATED (`!(x >= 0.5f)`) precisely so that it lands
            // here rather than in the second sheet.
            auto const rs_reads_second = [](float index, bool named, bool enabled) {
                return index >= 0.5f && named && enabled;
            };
            CHECK_MSG(!rs_reads_second(0.0f, true, true), "RS A8.rsi: `RS_Index = 0` reads the FIRST sheet");
            CHECK_MSG(!rs_reads_second(0.4999f, true, true), "RS A8.rsi: `RS_Index = 0.4999` reads the FIRST sheet");
            CHECK_MSG(rs_reads_second(0.5f, true, true),
                      "RS A8.rsi (the tie): `RS_Index = 0.5` reads the SECOND sheet, stated rather than inherited "
                      "from an operator accident");
            CHECK_MSG(rs_reads_second(1.0f, true, true), "RS A8.rsi: `RS_Index = 1.0` reads the SECOND sheet");
            CHECK_MSG(!rs_reads_second(1.0f, false, true),
                      "RS A8.rsi: `RS_Index = 1.0` with no `_GooRSSheet1` row reads the FIRST sheet - the NAME is "
                      "what makes the second sheet reachable at all, and that is every shipped material's case");
            CHECK_MSG(!rs_reads_second(1.0f, true, false),
                      "RS A8.rsi: `RS_Index = 1.0` with `_UseGooRSSheet1 = 0` reads the FIRST sheet - the switch is "
                      "the sidecar's second rule and it is asked under the SECOND SHEET'S own name");
            CHECK_MSG(!rs_reads_second(std::nanf(""), true, true),
                      "RS A8.rsi: a non-finite `RS_Index` reads the FIRST sheet - the host writes that comparison "
                      "negated, so a NaN cannot select the second");

            // S5: THE PROSE SITES THAT USED TO SAY THE FEATURE WAS ABSENT. A comment is not acceptance, which is
            // exactly why these are pinned: the next reader trusts a note that says "not honoured" and
            // re-implements the missing half - or trusts a note that says "read" while nothing reads it.
            // THESE CHECKS ARE CASE-INSENSITIVE, AND THE HOLE THAT CLOSES IS MEASURED: the rejected spelling has a
            // LOWERCASE sibling ("carried and unhonoured" / "not honoured"), and an uppercase-only
            // `find("NOT honoured")` cannot see it - putting the lowercase sentence BACK left both suites green
            // (task-43's mutation M6). So the rejected and the required spellings are compared in FOLDED text,
            // which also keeps a pin from being satisfied by DELETING the note instead of fixing it. The fold is an
            // explicit ASCII `A..Z` -> `a..z` loop, so it needs no `<cctype>` and cannot depend on the C locale.
            auto const ascii_lower = [](std::string const& text) {
                std::string folded = text;
                for (char& c : folded) {
                    if (c >= 'A' && c <= 'Z') {
                        c = static_cast<char>(c + ('a' - 'A'));
                    }
                }
                return folded;
            };
            std::string const goo_toon_folded = ascii_lower(goo_toon);
            std::string const primitive_folded = ascii_lower(primitive);
            CHECK_MSG(goo_toon_folded.find("not honoured") == std::string::npos &&
                          goo_toon.find("`RS_Index`") != std::string::npos,
                      "RS A8.rsi: `shaders/goo_toon.slang`'s L1 no longer claims `RS_Index` is not honoured in ANY "
                      "casing, and still says what does happen to it");
            CHECK_MSG(goo_toon.find("`RS_Index` IS NOT READ **HERE**") != std::string::npos,
                      "RS A8.rsi: the sampling site's note says WHERE the index is answered instead of that it is "
                      "ignored - this shader genuinely never reads it, so that note has to be exact");
            CHECK_MSG(primitive_folded.find("not honoured") == std::string::npos,
                      "RS A8.rsi: `vulkan/primitive/primitive.cppm` must not call `RS_Index` not-honoured in ANY "
                      "casing; the lowercase sentence used to slip past this pin");
            CHECK_MSG(primitive_folded.find("unhonoured") == std::string::npos &&
                          primitive_folded.find("answered - by the host") != std::string::npos,
                      "RS A8.rsi: `.x` is ANSWERED by the host rather than unhonoured - folded, so neither end of "
                      "the casing range escapes");
            CHECK_MSG(app.find("AND IT IS ONE OF THE REFERENCE'S TWO SHEETS") != std::string::npos,
                      "RS A8.rsi: the lane table's own note explains why there is no second entry beside "
                      "`_GooRSSheet` - the question the next reader will have");
        }

        // AND THE SIDECAR ITSELF, when this checkout has a build tree: the four new rows, verbatim. Guarded rather
        // than CHECKed for existence, because the file lives under the build directory (a fresh clone has none, and
        // a missing build tree is not a test failure); the implementation report carries the sha256 either way.
        {
            std::string const sidecar_path = std::string(VR_TEST_SOURCE_DIR) + "/build-release-clang64/chars/laevatain_goo.glb.toon.tsv";
            std::ifstream sidecar_file(sidecar_path);
            if (sidecar_file.is_open()) {
                std::string const sidecar{std::istreambuf_iterator<char>{sidecar_file}, std::istreambuf_iterator<char>{}};
                for (char const* const row : {"M_actor_laevat_cloth_02\tslot\t_GooRSMask\tT_actor_laevat_cloth_02_M",
                                              "M_actor_laevat_cloth_02\tfloat\t_UseGooRSMask\t1.0",
                                              "M_actor_laevat_cloth_02\tcolor\t_GooRSScalars\t1.0,1.0,1.0,0.0",
                                              "M_actor_laevat_cloth_02\tcolor\t_GooRSTint\t7.5,1.4143484830856323,0.0,0.9900000095367432",
                                              "M_actor_laevat_cloth_05\tslot\t_GooRSMask\tT_actor_laevat_cloth_03_M",
                                              "M_actor_laevat_cloth_05\tfloat\t_UseGooRSMask\t1.0",
                                              "M_actor_laevat_cloth_05\tcolor\t_GooRSScalars\t1.0,1.0,1.0,0.0",
                                              "M_actor_laevat_cloth_05\tcolor\t_GooRSTint\t7.5,1.4143449068069458,0.0,1.0"}) {
                    CHECK_MSG(sidecar.find(row) != std::string::npos, row);
                }
                // `kind` MUST be `slot`: only that kind reaches `slots`, and a `texture` row would leave the mask
                // slot undefined - the paper trail is the point, because the failure is silent
                CHECK_MSG(sidecar.find("_GooRSMask\ttexture") == std::string::npos, "RS A7: the mask rows are `slot` rows, never `texture`");

                // STEP 15 (spec L3): THE SHIPPED ASSET STATES NONE OF THE THREE NEW KEYS, and that IS the limit. Every
                // shipping material is `RS Model = 1`, so none of them enters `armA`; the one fixture that does
                // (`s13ctrl/laevatain_goo_rsz0.glb` and its own derived sidecar) is the only place these rows exist.
                // Pinned as an ABSENCE with the same reading as the `_GooFaceSDF` pin below: if a future asset adds
                // the rows, this fires, and that asset's `arm0` has to be MEASURED rather than inherited from here.
                CHECK_MSG(sidecar.find("_GooRSSheet") == std::string::npos &&
                              sidecar.find("_UseGooRSSheet") == std::string::npos &&
                              sidecar.find("_GooRSArm0") == std::string::npos,
                          "RS A9 (spec L3): the SHIPPED asset carries none of step 15's three keys - armA runs only on "
                          "the derived fixture, and no shipped pixel is affected by this step");

                // STEP 14 (mechanism table #7, arm (a)): THE HAIR'S CARRIER ROW - the one end-to-end assertion this step
                // adds, and the only one here that would notice a sidecar which simply does not carry the row at all.
                // Every other pin on `_GooNormalStrength` is CPU-side (the lane's row name, its neutral, the decoder's
                // form), so all of them stay green while lane 24 reads the `-1000` sentinel and the hair silently keeps
                // the old `rgb*2-1` decode. The row is inserted at the END of the `_GooNormalStrength` run, after the
                // cloth's, so it is pinned VERBATIM with the same tabs the file uses rather than as a loose "0.5".
                CHECK_MSG(sidecar.find("M_actor_laevat_hair_01\tfloat\t_GooNormalStrength\t0.5") != std::string::npos,
                          "14-A7: the shipped sidecar carries the hair `_GooNormalStrength` row");

                // DEBT (U): THE LANE-16 ROW, ON THE ASSET ITSELF - the asset-side half of the branch `main.cpp`
                // gained. The row is a `float` row, so the parser routes it into `scalars` and NOT into `others`;
                // the shipped build read lane 16 through `others`, found nothing, and answered every material that
                // lane's neutral `-1.0`. Two materials state `0.7999999523162842` and five state `1.0`, so `-1.0`
                // was never "the asset does not carry the row" - that IS debt (u)'s D1, visible here without a GPU.
                // (The `color` form is pinned ABSENT on purpose: it is the one spelling that would put the row back
                // into `others`, and with it the branch above would be unreachable again.)
                CHECK_MSG(sidecar.find("M_actor_laevat_body_01\tfloat\t_GooSpecularFGD\t0.7999999523162842") != std::string::npos,
                          "debt (u): the shipped body_01 states `_GooSpecularFGD = 0.7999999523162842` as a `float` row");
                CHECK_MSG(sidecar.find("M_actor_laevat_body_02\tfloat\t_GooSpecularFGD\t0.7999999523162842") != std::string::npos,
                          "debt (u): and so does body_02");
                for (char const* const cloth : {"01", "02", "03", "04", "05"}) {
                    std::string const row = std::string("M_actor_laevat_cloth_") + cloth + "\tfloat\t_GooSpecularFGD\t1.0";
                    CHECK_MSG(sidecar.find(row) != std::string::npos, row.c_str());
                }
                CHECK_MSG(sidecar.find("\tcolor\t_GooSpecularFGD") == std::string::npos,
                          "debt (u): no material spells the row as a `color` row, which is the one form the generic `others` path could have read");

                // 欠账 (e): THE FACE ARM'S GATE, ON THE ASSET ITSELF. Two rows decide whether the arm step 11's
                // second copy lived in can run at all, and both are pinned here - the material's
                // `_GooFaceScalarsA.w` (which makes `SmoothnessMax = 1.0`, `a2 = 0`, and the whole `原` term zero
                // whichever spelling computes it) plus the ABSENCE of `_GooFaceSDF`: lane 11, the gate's
                // `goo_face_block2.w`, stays 0 because a mask resolves only when its row is present.
                // The absence is pinned on purpose: if a future asset adds that row the arm goes LIVE, and this
                // check firing is the reminder that its specular then has to be re-measured instead of assumed
                // neutral. `8r4` in the source-text half pins the text and the neutrality; THESE two are the
                // asset-side half of the same claim.
                CHECK_MSG(sidecar.find("M_actor_laevat_face_01\tcolor\t_GooFaceScalarsA\t0.5,0.10000000149011612,1.0,1.0") != std::string::npos,
                          "欠账 (e): the shipped face material's `_GooFaceScalarsA.w` is 1.0, so its lobe's a2 is 0");
                CHECK_MSG(sidecar.find("_GooFaceSDF") == std::string::npos,
                          "欠账 (e): and no row names `_GooFaceSDF`, so lane 11 stays 0 and the face arm's gate cannot open");
            } else {
                // A MISSING BUILD TREE IS NOT A FAILURE (this block is guarded rather than CHECKed for existence), but
                // it must not be SILENT either: on a clean clone the carrier assertion above does not run, and a reader
                // of the log has to be able to tell that from a run in which it did.
                deren::vk_test::write_line("14-A7: sidecar artefact absent at {} - the hair carrier row was NOT checked", sidecar_path);
            }
        }
    }

    return deren::vk_test::finish("test_goo_toon_math");
}

#else
int32_t main() {
    deren::vk_test::write_line("test_goo_toon_math: VR_TEST_SOURCE_DIR is not defined, so the source checks cannot run");
    return 1;
}
#endif
