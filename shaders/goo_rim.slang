// shaders/goo_rim.slang - THE REWRITTEN TOON CHAIN'S EDGE LIGHT, AS A FULLSCREEN PASS (`goo_rim.frag.spv`).
//
// ================================ WHAT MOVED HERE, AND WHY IT HAD TO ================================
//
// STEP 2 put the Goo reference's OBJECT-SPACE RIM inside the character stage's fragment shader
// (`shaders/goo_toon.slang`) and left the screen-space half of it - `DepthRim`, the reference group of the same
// name - out, because the two Goo custom nodes it reads were undetermined (`goo_step2_rim_spec.md` §9-U1/U2/U4/U5).
// `goo_screenspace_info.md` settled those from the engine's own source, and what it settled makes the rim
// IMPOSSIBLE to finish where step 2 left it:
//
//   `vulkan/render_resource/render_resource.cppm`'s `character_forward_targets = {scene_color, gbuffer_depth}`
//   makes the depth an ATTACHMENT of the very pass whose fragment shades the surface - and an image cannot be a
//   depth attachment and a sampled texture in one instance. The reference's composition is
//   `rim_term = C ⊙ (D · F · V · depth_factor)`, a PRODUCT that cannot be split across two passes: splitting it
//   gives `C·D·F·V + mask` rather than `·mask`.
//
// So the WHOLE rim moves here: this stage re-computes the same `C`, `D`, `F` and `V` from the G-buffer and
// multiplies them by the depth factor in the same expression, which is the only arrangement in which the product
// is the reference's. The engine already had this shape for the same reason - the ARTICLE's contour is a
// fullscreen pass (`shaders/toon_screen_rim.slang`), whose own header records the identical Vulkan rule.
//
// ================================ THE REFERENCE, NODE BY NODE ================================
//
// Everything below is a socket or a connection in `deren-ab/gooblender/nodes.json`, and the specification is
// `deren-ab/goo_step2_rim_spec.md` §2.5 and §3.1/§3.2 (the container compositions). The four attenuations are
// step 2's functions, MOVED (not rewritten): `D` = `Directional light attenuation`, `F` = `Fresnel attenuation`,
// `V` = `Vertical attenuation`, `C` = the `Rim_Color` sub-group's surviving output `Rim_Color·Rim_ColorStrength`.
//
//   Base (`Arknights: Endfield_PBRToonBase`):
//     `运算.026 = D*F`; `运算.027 = 运算.026*V`; `运算.029 = MINIMUM(DepthRim, 0.5)`; `运算.028 = 运算.027·运算.029`;
//     `混合.018 = C ⊙ 运算.028` (MULTIPLY, factor 1.0); `混合.019 = <the group's own colour> + 混合.018` (ADD).
//   Hair (`Arknights: Endfield_PBRToonBaseHair`):
//     the same, EXCEPT that `运算.029` DOES NOT EXIST in that group - `运算.028 = 运算.027 · DepthRim` directly -
//     and it has one layer Base has not: `混合.017`, a MULTIPLY mix at factor `Use Rimlimitation?` between
//     `混合.018` and `clamp(n_object.x, 0, 1)`.
//
// THE ADD IS THIS PASS'S BLEND STATE rather than a line of arithmetic: `混合.019` adds the rim to the colour the
// container computed, and the pipeline's blend is ONE/ONE (see `vulkan/pass/goo_rim.cpp`), so the fragment returns
// the rim TERM and the frame's own colour is what it is added to. That is the same "additive fullscreen stage"
// the article's contour uses, and it is why this pass never samples the image it writes.
//
// ================================ `DepthRim` (the group this step lands) ================================
//
// The reference's 21-node group, as `goo_screenspace_info.md` reads it out of the engine's source
// (branch `goo-engine-v4.2-release` @ 844bc9d8110e695c56b02dae501323831ad5e730):
//
//   矢量变换 (WORLD -> CAMERA, vector_type = NORMAL)  ->  n_cam        # 3x3 direction transform, no translation
//   分离 XYZ.002 -> nx, ny, nz
//   运算.029 = Rim_width_X * 0.1 ; 运算.031 = Rim_width_Y * 0.1
//   合并 XYZ.002 = (运算.029*nx, 运算.031*ny, 值(明度).004*nz)          # 值(明度).004 == 0.0, so the z term is 0
//   Vector Math.017 = 合并XYZ.002 + 纹理坐标.Camera                     # the offset is IN CAMERA SPACE
//   Screenspace Info     .SceneDepth = -get_view_z_from_depth(depth at proj(view_pos + offset))
//   Screenspace Info.001 .SceneDepth = -get_view_z_from_depth(depth at this pixel)   # View Position unconnected
//   运算.021 = ADD(Screenspace Info.Scene Depth, 0.0) ; 运算.018 = SUBTRACT(运算.021, Screenspace Info.001.Scene
//             Depth)                                                  # = offset - self, in METRES, along the axis
//   映射范围 = map_range(运算.018, 0 -> 5, 0 -> 8)  [clamp off]  ->  钳制 = clamp(_, 0, 8)  ->  运算.019 = _ / 2
//   DepthRim = 钳制 / 2,  in [0, 4]
//
// FOUR THINGS THAT ARE LOAD-BEARING AND EASY TO GET WRONG, each with its evidence:
//
//   * THE OFFSET IS A CAMERA-SPACE TRANSLATION, NOT A UV SHIFT. `screenspace_info` re-projects the moved POINT
//     (`get_uvs_from_view`), and the two `* vec3(1,1,-1)` in the chain (one in `view_position_get`/`坐标.Camera`,
//     one inside `screenspace_info`) cancel - so the net effect is a shift in the ORIGINAL view space by
//     `(0.1·RimX·n_cam.x, 0.1·RimY·n_cam.y, 0)`. That is why the offset is built in view space here and why the
//     projection is applied afterwards.
//   * `z` IS ZERO, so the sample lands on the same depth PLANE and the difference measures only "is what is
//     beside me further away". `值(明度).004` was read directly out of the `.blend` (not the dump): `0.0`.
//   * `Scene Depth` IS A POSITIVE LINEAR DISTANCE IN METRES along the camera's forward axis, i.e. exactly
//     `-get_view_z_from_depth(window depth)` - the engine's own helper `rim_view_depth` below IS that function
//     (see its note), so no unit conversion is invented here.
//   * THE SIGN: `运算.018 = depth(offset) - depth(self)`, so `dz > 0` means the neighbour is FURTHER = this pixel
//     is the silhouette. A neighbour that is NEARER gives a negative `dz`, the clamp sends it to 0, and no rim is
//     drawn - which is why this is a contour and not an edge detector.
//
// ================================ WHAT THE G-BUFFER CAN AND CANNOT GIVE ================================
//
// `n_world` is the G-buffer's stored WORLD normal (RGBA16F at `gbuffer_normal`, the same `gather_surface` normal
// the character stage shades with, quantized). `world_pos` is rebuilt from the stored depth with the frame's
// `inv_view_proj`, the same expression `shaders/deferred.slang` uses. The material id is the G-buffer's two bytes
// (`gbuffer_material`), which is what turns "a pixel" into "a material" and therefore into the lanes that carry
// this material's rim numbers - exactly as `shaders/toon_screen_rim.slang` reads them.
//
// WHAT IT DOES NOT GIVE: the fragment's OBJECT-space normal, which the hair's rim limitation is gated on. The
// reference's `矢量变换` there is WORLD -> OBJECT, and no fragment stage in this renderer is handed the draw's
// inverse world transform. THE WORLD NORMAL IS PASSED AS `n_object`, which is EXACT for a draw whose world
// transform carries no rotation and no scale - and every `*_hair_01` mesh in this repository is a root node with
// no rotation (step 2 measured the same thing and made the same substitution; see its `goo_rim_term` note and
// `goo_step2_result.md`). It is an APPROXIMATION in general, and it is an argument rather than a second read of
// the same value so that a future caller with the transform in hand can be exact.
//
// ================================ THE SCOPE, WHICH IS THE REFERENCE'S PARTITION ================================
//
// `base`, `skin`, `cloth` and `hair` - the families the Goo project gives a rim to through `PBRToonBase` /
// `PBRToonBaseHair`. `face` is NOT among them and keeps the OLD chain's rim (which `character_forward.slang`
// leaves running for it under `VR_GOO_TOON_CHAIN`), because `PBRToonBaseFace` has a `Rim_Color` socket and no
// width or attenuation port at all, leaving its consumer chain undetermined (spec §5.10, §9-U7). `eye`/`none`
// return zero, as in step 2.
//
// ================================ THE BACKGROUND (a stated boundary, not an accident) ================================
//
// THE PIXEL ITSELF: a stored depth of the cleared far value means no surface was written here and the fragment
// returns 0 - the frame is left exactly as the character stage wrote it (the article's contour pass has the same
// rule and for the same reason).
//
// THE OFFSET SAMPLE: when it lands on the background, `rim_view_depth(1.0)` is the FAR PLANE's distance - the same
// expression `shaders/gbuffer_debug.slang` uses for its own far plane - so `dz` is large, `DepthRim` saturates at
// 4, and the pixel gets the full `depth_factor` (`min(4, 0.5) = 0.5` for Base, `4` for Hair). THAT IS THE
// REFERENCE'S BEHAVIOUR, and it is the mechanism: `maxzBuffer` holds the far depth there too, so a silhouette
// against the sky lights up. The offset uv is saturated into [0,1] first, which is what the reference's
// clamp-addressed HiZ buffer does as well.
//
// ONE CONSEQUENCE WORTH NAMING: this stage samples the G-buffer, which the SCENE pass wrote - so the rim lands on
// every pixel that stored a base/skin/cloth/hair surface, including pixels a LATER group has since overdrawn
// (the outline hull's ring where it overlaps another surface of the same character). Step 2's rim could not: it
// was invented inside the surface's own fragment. That difference is measured in the control arm of
// `goo_step3_result.md` rather than argued here.

#include "heap_access.slang"
#include "material_record.glsl"
#include "toon_params.slang"

struct GooRimPush
{
    // clip (NDC xyz, w = 1) -> world position, for the pixel's own depth. THE FRAME'S, like `deferred`'s field of
    // the same name: the world position this stage needs is the one the shading stages reconstructed, so it is
    // the same matrix and not a second reading of the camera.
    VR_MAT4 inv_view_proj;
    // projection[2][2] / [3][2]: THE DEPTH LINEARIZATION, and they are here rather than in the camera UBO because
    // they are what `rim_view_depth` below is made of - the frame's own frame_constants carries exactly this pair
    // for the article's contour and the debug view.
    float proj_22;
    float proj_32;
    // THE HEAP INDICES, at the END so every field above keeps its offset (the host appends them; see the pass).
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<GooRimPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)

// THE INCLUDE ORDER IS THE SAME CONTRACT `deferred.slang` documents: the shim and this stage's own declarations
// FIRST, and only then `shading.glsl`, whose function bodies address the per-frame arrays through the slot macros
// (`heap_camera_slot`, `heap_light_slot`) - so `pc` and the two `#define`s above have to exist before it.
#include "shading.glsl"

/// @brief how many `vec4` lanes `deren::vulkan::toon_colour_lane` has - the stride of the colour table
///
/// THE THIRD COPY OF THIS NUMBER, and the copies are deliberate (`character_forward.slang` and `pbr.slang` carry
/// the other two): a stage cannot include a file that has an entry point, so each reader spells the host's
/// `toon_colour_lane::count` itself and `tests/test_toon_material_sidecar.cpp` pins all three against the enum.
/// A stride left at an older count reads ANOTHER MATERIAL's lanes - and this stage reads five of them, so the
/// failure would be a rim in the wrong colour, with the wrong width, gated by somebody else's hair flag.
/// STEP 7 RAISED IT TO 24 WITH THE OTHER THREE COPIES (the FACE container's four lanes), which is the same edit in
/// four files and the reason the sidecar test compares all of them rather than one.
/// STEP 8 RAISED IT TO 25 WITH THE SAME THREE (`_GooNormalStrength`, the reference's `DecodeNormal.NormalStrength`,
/// which THIS stage does not read: it is the surface stage's, and the lane is counted here for the stride's sake,
/// exactly as lane 6 is counted there).
/// STEP 10 RAISED IT TO 26 FOR `_GooAnisoGate` (`Use anisotropy?` / `Anisotropic mask` / `Use Toonaniso?` of
/// `PBRToonBase`), again counted and not read here.
/// STEP 12 RAISED IT TO 27 FOR `_GooAnisoRough` (the anisotropic lobe's two roughnesses, `Aniso_SmoothnessMaxT` /
/// `Aniso_SmoothnessMaxB`, built into `roughnessT` / `roughnessB` by the surface stage), again counted and not
/// read here.
/// STEP 13 RAISED IT TO 29 FOR `_GooRSScalars` / `_GooRSTint` (mechanism table #14, `RS EFF`: `Use RS_Eff?` /
/// `RS Multiply Value` / `RS Model`, then `RS ColorTint` and the `_M` smoothstep's `max`). THIS PASS READS NEITHER
/// OF THEM and must not: the whole RS branch belongs to the surface stage that owns the emission term, and adding
/// the RS term here as well would apply it twice. The number is here only because this file addresses the same
/// lane table, so a stale count would land this pass's rim reads (lanes 7/8/9) on another material's lanes.
/// STEP 15 RAISED IT TO 30 FOR `_GooRSArm0` (`RS_Index` / `RS Strength` / `Layer weight Value` / `Layer weight
/// Value Offset`), the four sockets the `armA` side of the same mechanism needs. THIS PASS STILL READS NONE OF THE
/// RS LANES - the note above applies unchanged - and the number moves only because the table grew.
static const uint goo_rim_colour_lanes = 30u;

// ============================ THE REFERENCE'S GROUP-INTERNAL CONSTANTS ============================
//
// Every number below is a socket's value in `gooblender/nodes.json`, named by the node it belongs to, and the
// step-2 spellings are kept verbatim so the two files can be diffed against each other where they overlap.

/// `组输入.Rim_ColorStrength` - the CONTAINER group interface's default. IT IS THE "ABSENT" ANSWER AND IT
/// SWITCHES THE RIM OFF, which is the reference's own use of the value rather than a convention invented here
/// (`M_actor_laevat_cloth_03` states exactly this).
static const float goo_rim_strength_default = 0.0;
/// `组输入.Rim_DirLightAtten` - the group interface's default, and the value `M_actor_laevat_body_01/02` carry.
static const float goo_rim_dir_atten_default = 0.8999999761581421;
/// `PBRToonBase` / `...Hair` `组输入.ToonfresnelPow` - the CONTAINER group's interface default. It is the
/// exponent of the container's separate ToonFresnel term and NOT of the rim's fresnel (see the note in
/// `goo_rim_term`), so it is carried here for the same reason its lane component is: the step that ports
/// `混合.012` reads it there.
static const float goo_rim_fresnel_pow_default = 2.0;
/// `PBRToonBaseHair :: 组输入.Use Rimlimitation?` - the group interface's default (0.0 = the limitation is off).
static const float goo_rim_limitation_default = 0.0;
/// The tint's absent answer: the `Rim_Color` SUB-group's interface default, `[1.0, 1.0, 1.0, 1.0]`.
static const float3 goo_rim_colour_default = float3(1.0, 1.0, 1.0);

/// `DepthRim :: 组输入.Rim_width_X` / `.y` - THE GROUP'S OWN INTERFACE DEFAULT, `0.5` in both, and it is the
/// answer for a material that states no `_GooRimWidths` row (`meta.node_groups[DepthRim].interface[]`, read out of
/// the dump rather than chosen here). IT IS A LOUD DEFAULT: `0.5` makes the offset sample 10x further across the
/// screen than the `0.04184713214635849` every laevatain material states, which is why a missing row shows up as
/// a rim of the wrong shape rather than as no rim.
static const float goo_rim_width_default = 0.5;
/// `DepthRim :: 运算.029`'s `Value` and `运算.031`'s - the 0.1 that turns a WIDTH into a camera-space offset.
static const float goo_rim_width_scale = 0.10000000149011612;
/// `DepthRim :: 映射范围`'s four ends, `From Min/Max = 0/5` into `To Min/Max = 0/8` (clamp off), and the `钳制`
/// that follows it: `clamp(map_range(dz, 0, 5, 0, 8), 0, 8)`. Written as the node graph writes it - a scale of
/// `8/5` applied to `dz` - rather than as the folded `1.6`, so the two numbers in the dump can be re-read here.
static const float goo_depth_rim_from_min = 0.0;
static const float goo_depth_rim_from_max = 5.0;
static const float goo_depth_rim_to_min = 0.0;
static const float goo_depth_rim_to_max = 8.0;
static const float goo_depth_rim_clamp_max = 8.0;
/// `DepthRim :: 运算.019`'s divisor (`DIVIDE` by 2.0): the group's output is in [0, 4], not [0, 8].
static const float goo_depth_rim_divisor = 2.0;
/// `PBRToonBase :: 运算.029`'s constant - the ceiling the BASE container puts on `DepthRim`. HAIR HAS NO SUCH
/// NODE (spec §3.2), which is the one arithmetic difference between the two containers' rim composition.
static const float goo_depth_rim_base_ceiling = 0.5;

/// @brief VIEW-SPACE DISTANCE of a depth-buffer sample, in metres, positive, along the camera's forward axis
///
/// THIS IS THE QUANTITY GOO'S `Scene Depth` IS, and the form is the engine's existing helper rather than a new
/// one: for the right-handed ZERO-TO-ONE projection this renderer builds (`glm::perspectiveRH_ZO`, see
/// `vulkan/primitive/primitive.cpp`), the stored depth IS `z_ndc = (proj_22·z_view + proj_32) / -z_view`, which
/// inverts to `z_view = -proj_32 / (z_ndc + proj_22)` - and the reference's `Scene Depth` is
/// `-get_view_z_from_depth(...)`, i.e. `+proj_32 / (z_ndc + proj_22)`, which is what this returns.
///
/// THE ONE CONVENTIONAL DIFFERENCE FROM GOO'S OWN FUNCTION, stated rather than glossed: `get_view_z_from_depth`
/// opens with `d = 2.0*depth - 1.0` because Blender's NDC is [-1,1]; this renderer's is zero-to-one (Vulkan), so
/// the stored depth needs no remap and the inversion is the single expression below. The QUANTITY and its UNIT
/// are the same - a positive distance in metres along the camera's axis - and the terms are the same two
/// projection entries. Two INDEPENDENT confirmations that the convention is this one: the shadow fit recovers
/// the camera's near and far planes in metres from exactly these two entries
/// (`vulkan/shadow_fit/shadow_fit.cpp`: `near = proj[3][2]/proj[2][2]`, `far = proj[2][2]·near/(1+proj[2][2])`),
/// and `tests/test_goo_toon_math.cpp` round-trips a distance through the projection and back.
///
/// `depth = 1.0` (the cleared far value) answers `proj_32 / (1 + proj_22)` = THE FAR PLANE'S DISTANCE, which is
/// what makes a silhouette against the background light up - see the header's background note.
static float goo_rim_view_depth(float depth) {
    return pc.proj_32 / (depth + pc.proj_22);
}

/// @brief the stored depth at @p uv, as the G-buffer holds it
static float goo_rim_depth_at(float2 uv) {
    return heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), heap_sampler_gbuffer, uv).r;
}

/// @brief rebuild the world-space position of a pixel from its stored depth
/// @note the same expression, and the same note, as `shaders/deferred.slang`'s `world_position_from_depth`: the
///       projection is a RH_ZO matrix with its Y row flipped for Vulkan, and post.vert's v_uv already maps (0,0)
///       to the top-left texel, so the NDC of a pixel is `(2*uv - 1)` in both axes and the inverse matrix carries
///       the rest of the conventions.
/// @note `pc.inv_view_proj * vec4(...)` IS `deferred.slang`'s EXACT SPELLING, GLSL TYPES AND ALL, and that is not
///       cosmetic: under `-allow-glsl` Slang resolves a matrix product by the SPELLING of the vector, so
///       `row_major float4x4 * float4` is NOT `row_major float4x4 * vec4` (and neither is `mul(...)` of the
///       former). MEASURED, the hard way: with the HLSL spellings the unprojection and the reprojection below
///       stopped being inverses - re-projecting a pixel's own reconstructed position recovered its uv nowhere in
///       the frame (0 % of 1 036 800 px) - which read as a rim whose every offset sample landed on the
///       background. With these spellings the same round trip recovers the uv over the surface's whole footprint.
static float3 goo_rim_world_position(float2 uv, float depth) {
    const vec4 world = pc.inv_view_proj * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return world.xyz / world.w;
}

/// @brief `Directional light attenuation` - spec §8 A1/A1b/A1c (STEP 2'S FUNCTION, MOVED UNCHANGED)
///
/// `钳制.005 = clamp(NoL_Unsaturate, 0, 1)`; `运算.025 = 1.0 - Directional light attenuation adjust`;
/// `混合.013` is a FLOAT MIX whose factor is the clamped `NoL`, whose A is that subtraction and whose B is the
/// unconnected constant `1.0`. THE CLAMP IS LOAD-BEARING AT BOTH ENDS: a `NoL` of `-0.5` reads the FLOOR
/// `1 - adjust` rather than extrapolating past it, and a `NoL` of `1.0` reads the ceiling `1.0` exactly.
///
/// @param no_l_unsaturate the RAW `dot(N, L)` before any half-Lambert remap (this function's clamp is the only one)
/// @param adjust the material's `Rim_DirLightAtten`
static float goo_rim_directional_attenuation(const float no_l_unsaturate, const float adjust) {
    const float clamped = saturate(no_l_unsaturate);
    const float floor_value = 1.0 - adjust;
    return lerp(floor_value, 1.0, clamped);
}

/// @brief `Vertical attenuation` - spec §8 A2 (STEP 2'S FUNCTION, MOVED UNCHANGED)
///
/// `运算.023 = n_world.z * 0.5`, `运算.024 = that + 0.5`. IT IS THE WORLD-SPACE Z and not the camera-space one,
/// which is the distinction the sibling `DepthRim` group makes the other way: `几何数据.002.Normal` reaches
/// `分离 XYZ.003` with NO `矢量变换` in between, while `DepthRim`'s normal goes through `WORLD -> CAMERA` first.
static float goo_rim_vertical_attenuation(const float3 world_normal) {
    return world_normal.z * 0.5 + 0.5;
}

/// @brief `Fresnel attenuation` - spec §8 A3 (STEP 2'S FUNCTION, MOVED UNCHANGED)
///
/// The reference writes `(1 - NoV)^4` as four nodes: `运算.022 = 1 - NoV`, a reroute feeding BOTH slots of
/// `运算.019 = t*t`, then `运算.021 = (t*t)*(t*t)`. The exponent is the reference's literal FOUR and not
/// `ToonfresnelPow` (that socket belongs to the container's separate ToonFresnel term).
static float goo_rim_fresnel_attenuation(const float no_v) {
    const float one_minus_no_v = 1.0 - no_v;
    const float squared = one_minus_no_v * one_minus_no_v;
    return squared * squared;
}

/// @brief the `Rim_Color` sub-group's SURVIVING output: `Rim_Color * Rim_ColorStrength` - spec §8 A5/A8
///
/// `混合.016`'s `Factor_Float` is unconnected at `1.0`, so the group's albedo/light/LoV-modulated branch
/// (`混合.015` -> `混合.014`) reaches nothing and `混合.017 = Rim_Color · Rim_ColorStrength` is what survives.
/// The `.xxx` is Blender's broadcast of a VALUE socket into an RGBA (`(s,s,s,1.0)`), which is why the output's
/// alpha is always 1.0 and why the lane carries the tint in `.rgb` alone.
static float3 goo_rim_colour_of(const float3 rim_colour, const float rim_colour_strength) {
    return rim_colour * rim_colour_strength.xxx;
}

/// @brief `DepthRim` - the reference group's 21 nodes, as §2.5 reads them (see the header for the full chain)
///
/// @param depth_self the pixel's own `Scene Depth` (`Screenspace Info.001`, whose `View Position` is unconnected,
///        so it samples THIS fragment's depth)
/// @param depth_offset `Scene Depth` at the projected offset point (`Screenspace Info`, whose `View Position` is
///        `纹理坐标.Camera + offset`)
/// @return `DepthRim` in [0, 4]: 0 when the neighbour is nearer or level, rising with the depth difference
/// @note THE SIGN IS THE CONTOUR: `运算.018 = 运算.021 - Screenspace Info.001.Scene Depth`, i.e.
///       `depth(offset) - depth(self)`, and `钳制`'s floor at 0 is what makes a NEARER neighbour produce nothing
///       rather than a negative rim. `map_range`'s `clamp=False` is why the `钳制` node exists at all, and the
///       two are written in that order here because that is the order the value passes through.
static float goo_depth_rim(const float depth_self, const float depth_offset) {
    const float dz = depth_offset - depth_self;
    const float mapped = (dz - goo_depth_rim_from_min) * (goo_depth_rim_to_max - goo_depth_rim_to_min) / (goo_depth_rim_from_max - goo_depth_rim_from_min) + goo_depth_rim_to_min;
    return clamp(mapped, 0.0, goo_depth_rim_clamp_max) / goo_depth_rim_divisor;
}

/// @brief ONE MATERIAL'S rim term, as `PBRToonBase` / `PBRToonBaseHair` compose it - STEP 2'S FUNCTION, with
///        `DepthRim` now the real value instead of the multiplicative identity
///
/// THE COMPOSITION IS THE SAME IN BOTH CONTAINERS AND DIFFERS IN EXACTLY TWO PLACES, which is why this one
/// function takes a `hair` flag:
///
///   1. `运算.028`: BASE multiplies `运算.027` by `min(DepthRim, 0.5)` (`运算.029 = MINIMUM(转接点.092, 0.5)`,
///      where `转接点.092 <- 群组.016.DepthRim`); HAIR multiplies it by `DepthRim` DIRECTLY, because that group
///      has no `运算.029` at all (spec §3.2, and step 2's report §9.1 for the ruling). The two arms are one line
///      each and they are NOT the same line.
///   2. `混合.017`, the rim LIMITATION, which only the hair container has: a MULTIPLY mix at factor
///      `Use Rimlimitation?` between `混合.018` and `clamp(n_object.x, 0, 1)`.
///
/// @param rim_colour the material's rim tint (lane 7's `.rgb`, sentinel already resolved)
/// @param rim_scalars the material's four scalars (lane 8), IN THE LANE'S ORDER: `.x` = `Rim_ColorStrength`,
///        `.y` = `Rim_DirLightAtten`, `.z` = `ToonfresnelPow`, `.w` = `Use Rimlimitation?` - each `< 0` meaning
///        "not stated" and answered with the reference group's own default
/// @param depth_rim `DepthRim` for this pixel, already in [0, 4]
/// @param object_normal the OBJECT-space normal, which only the hair's limitation reads - THE CALLER PASSES THE
///        WORLD NORMAL (see the header; exact for an unrotated, unscaled hair node, an approximation in general)
/// @param hair whether this material is shaded by `PBRToonBaseHair` (see the two differences above)
/// @return the rim term's RGB, which the pipeline ADDS to the frame (`混合.019`, ADD at factor 1.0)
static float3 goo_rim_term(const float3 rim_colour,
                           const float4 rim_scalars,
                           const float no_l_unsaturate,
                           const float no_v,
                           const float3 world_normal,
                           const float3 object_normal,
                           const bool hair,
                           const float depth_rim) {
    // ---- the four numbers, each behind the reference group's OWN interface default ----
    //
    // The tests are per COMPONENT because the reference lets a material state one socket and not another - its
    // own materials do not, but the group's interface does not require them to.
    const float strength = rim_scalars.x >= 0.0 ? rim_scalars.x : goo_rim_strength_default;
    const float dir_atten = rim_scalars.y >= 0.0 ? rim_scalars.y : goo_rim_dir_atten_default;
    const float limitation = rim_scalars.w >= 0.0 ? rim_scalars.w : goo_rim_limitation_default;
    // `ToonfresnelPow` (`.z`) IS READ BY NOTHING HERE, and that is the reference's arrangement rather than a
    // dropped socket: it is the exponent of the container's SEPARATE ToonFresnel term (`运算.014`), whose colour
    // reaches the final sum through `混合.012` - the frame this stage ADDS to, not a term of the rim. It rides the
    // lane because that is where its material states it; the step that ports `混合.012` reads it there.
    const float3 tint = goo_rim_colour_of(rim_colour, strength);

    // ---- `运算.026` / `运算.027`: the three attenuations, in the reference's own order ----
    const float directional = goo_rim_directional_attenuation(no_l_unsaturate, dir_atten);
    const float fresnel = goo_rim_fresnel_attenuation(no_v);
    const float vertical = goo_rim_vertical_attenuation(world_normal);
    const float product = directional * fresnel * vertical;

    // ---- `运算.029` / `运算.028` / `混合.018` (MULTIPLY, factor 1.0): the tint, scaled by the depth factor ----
    //
    // THE ONE LINE WHERE THE TWO CONTAINERS DIFFER, spelled as the reference spells it: BASE has
    // `运算.029 = MINIMUM(DepthRim, 0.5)`, HAIR has no such node and uses `DepthRim` itself.
    const float rim_depth_factor = hair ? depth_rim                       // hair: `运算.028` uses `DepthRim` itself
                                        : min(depth_rim, goo_depth_rim_base_ceiling); // base: `运算.029`'s `MINIMUM`
    const float3 scaled = tint * (product * rim_depth_factor);
    if (!hair) {
        return scaled;
    }
    // ---- `混合.017`: the HAIR's rim limitation, and the only other thing the two containers compute differently ----
    //
    // Blender's MULTIPLY mix at factor `f` is `A*(1-f) + (A*B)*f`, so at the flag's two extremes this is `A` and
    // `A*B`. `钳制`'s window is `[0.0, 1.0]` applied to the OBJECT-space normal's x, so the gate is 1 where the
    // surface turns SIDEWAYS in the mesh's own frame and 0 where it points along that frame's forward or back
    // axis. THE CLAMP IS WHAT MAKES IT A SUPPRESSION RATHER THAN AN INVERSION: `n_object.x` is negative over half
    // the sphere and `clamp` sends that half to ZERO, so the rim is multiplied away rather than flipped in sign.
    const float object_x = saturate(object_normal.x);
    return lerp(scaled, scaled * object_x, limitation);
}

/// @brief the rim term for ONE PIXEL, from the material the G-buffer stored there
///
/// THE SCOPE IS THE REFERENCE'S PARTITION (see the header): this returns zero for `face`, `eye` and `none`, so
/// the family test is the first thing after the material id is read - a pixel whose material claims no family is
/// not a character at all (every non-character surface in the scene), and the face's rim is the OLD chain's.
///
/// @param material_id the G-buffer's two material-id bytes, decoded by the caller
/// @param no_l_unsaturate the RAW `dot(n_world, L)`
/// @param no_v `dot(n_world, V)`, saturated - the same quantity `toon_diffuse` computes for the surface, so the
///        term this pass adds and the term the body used cannot differ by a normalization
/// @param depth_rim `DepthRim` for this pixel
static float3 goo_rim_for_pixel(const uint material_id, const float no_l_unsaturate, const float no_v, const float3 normal, const float depth_rim) {
    const Material mat = material_at(heap_slots_materials, material_id);
    const uint family = mat.toon_family;
    const bool hair = family == VR_FAMILY_HAIR;
    if (family != VR_FAMILY_BASE && family != VR_FAMILY_SKIN && family != VR_FAMILY_CLOTH && !hair) {
        return float3(0.0, 0.0, 0.0); // out of this chain's scope; the old chain's rim is what shades these
    }

    // THE TWO RIM LANES, at the flat stride the whole table is addressed with (`goo_rim_colour_lanes`, which is
    // the host's `toon_colour_lane::count`). Their sentinels are per component and are resolved inside
    // `goo_rim_term`. The material's THIRD rim lane - the widths - is read in `main`, because the offset it
    // builds is what produces `depth_rim`; this function is handed the result.
    const float4 rim_colour_lane = toon_colour_at(heap_slots_toon_colours, (material_id * goo_rim_colour_lanes) + 7u);
    const float4 rim_scalars = toon_colour_at(heap_slots_toon_colours, (material_id * goo_rim_colour_lanes) + 8u);

    // ---- The world normal IS the object normal here: see the header's provenance note. Written as its own
    // binding so the substitution is visible at the call rather than folded into the argument list.
    const float3 normal_object = normal;
    return goo_rim_term(rim_colour_lane.rgb, rim_scalars, no_l_unsaturate, no_v, normal, normal_object, hair, depth_rim);
}

[shader("fragment")]
float4 main([[vk::location(0)]] float2 v_uv : TEXCOORD0) : SV_Target {
    // ---- THE PIXEL MUST HOLD A SURFACE (a cleared depth is the background) ----
    const float stored_depth = goo_rim_depth_at(v_uv);
    if (stored_depth >= 1.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    // ---- WHICH MATERIAL THIS PIXEL IS: the two UNORM bytes `gbuffer.slang` wrote ----
    const float4 material_target = heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_material + heap_image_index), heap_sampler_gbuffer, v_uv);
    const uint material_id = uint(round(material_target.r * 255.0)) | (uint(round(material_target.g * 255.0)) << 8u);

    // ---- THE MATERIAL'S TWO RIM WIDTHS, each behind the `DepthRim` group's own interface default ----
    //
    // THE TEST IS PER COMPONENT and the fallback is the GROUP'S `0.5`, not this renderer's number: see the lane's
    // note. `.z` / `.w` are reserved and are not read.
    const float4 widths_lane = toon_colour_at(heap_slots_toon_colours, (material_id * goo_rim_colour_lanes) + 9u);
    const float rim_width_x = widths_lane.x >= 0.0 ? widths_lane.x : goo_rim_width_default;
    const float rim_width_y = widths_lane.y >= 0.0 ? widths_lane.y : goo_rim_width_default;

    // ---- the surface facts this stage rebuilds instead of interpolating ----
    const float3 n_world = normalize(heap_texel(heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index), heap_sampler_gbuffer, v_uv).xyz);
    const float3 world_pos = goo_rim_world_position(v_uv, stored_depth);

    // ---- `DepthRim`: the offset is a CAMERA-SPACE translation, then a reprojection (see the header) ----
    //
    // `矢量变换(WORLD -> CAMERA, NORMAL)` is a DIRECTION transform, so the translation column takes no part:
    // a `w = 0` product, exactly as `goo_toon.slang` builds its matcap uv. THE VECTORS ARE THE GLSL SPELLINGS and
    // the products are `*` because that is the only form that is a matrix product here (see
    // `goo_rim_world_position`'s note - the HLSL spellings silently made the round trip fail).
    const vec3 view_pos = (camera_at(heap_camera_slot).view * vec4(world_pos, 1.0)).xyz;
    const vec3 n_cam = (camera_at(heap_camera_slot).view * vec4(n_world, 0.0)).xyz;
    const vec3 rim_offset = vec3(goo_rim_width_scale * rim_width_x * n_cam.x, goo_rim_width_scale * rim_width_y * n_cam.y, 0.0);
    const vec4 offset_clip = camera_at(heap_camera_slot).proj * vec4(view_pos + rim_offset, 1.0);
    // The offset uv is SATURATED into the frame, which is what the reference's clamp-addressed HiZ buffer does:
    // a sample outside the image reads the edge texel there and would read the edge texel here (the G-buffer
    // sampler is clamp-addressed too), so the saturation states the same behaviour rather than changing it.
    const float2 uv_offset = saturate((offset_clip.xy / offset_clip.w) * 0.5 + 0.5);

    // ---- `运算.018` -> `映射范围` -> `钳制` -> `运算.019`: DepthRim ----
    //
    // BOTH SAMPLES GO THROUGH THE SAME CONVERSION, `-get_view_z_from_depth`: the pixel's own stored depth and the
    // stored depth at the offset. A background sample answers the FAR PLANE's distance there, which is what makes
    // a silhouette against the sky light up (see the header).
    const float depth_self = goo_rim_view_depth(stored_depth);
    const float depth_offset = goo_rim_view_depth(goo_rim_depth_at(uv_offset));
    const float depth_rim = goo_depth_rim(depth_self, depth_offset);

    // ---- the reference's remaining rim inputs ----
    //
    // `NoL_Unsaturate` is the RAW `dot(N, L)`: `转接点.096` carries the container's own socket, before any
    // half-Lambert remap (`钳制.005` is the group's only clamp). `NoV` is saturated exactly as step 2's caller
    // saturated it, so the term this pass adds is the term step 2 added.
    const float3 light_direction = normalize(light_at(heap_light_slot).light_dir.xyz);
    const float no_l_unsaturate = dot(n_world, light_direction);
    const float3 to_camera = normalize(camera_at(heap_camera_slot).camera_pos.xyz - world_pos);
    const float no_v = saturate(dot(n_world, to_camera));

    const float3 rim = goo_rim_for_pixel(material_id, no_l_unsaturate, no_v, n_world, depth_rim);
    // THE PIPELINE BLENDS ONE/ONE, so this is an ADDITION to the frame the character stage wrote and the alpha
    // lane is unused - `混合.019`'s ADD, expressed as the pass's blend state rather than as an arithmetic step.
    return float4(rim, 0.0);
}
