/**
 * @file shaders/shading.glsl
 * @brief Shared lighting: the scene light/IBL/shadow bindings and the BRDF stack both PBR passes use.
 * @ingroup shaders
 *
 * Included by pbr.frag (the forward path) and deferred.frag (the deferred path's lighting stage).
 * The deferred path gets its surface from the G-buffer instead of from a vertex/fragment interface,
 * but from that point on the two paths must agree EXACTLY: same sun, same shadow test, same punctual
 * lights, same split-sum IBL, same selectable BRDF/diffuse presets, same cel-shading bands. Keeping
 * that in one place is not just tidy - it is what makes the forward path a usable A/B reference for
 * the deferred one, and it removes the whole class of "the deferred image looks slightly different"
 * bugs.
 *
 * What lives here: the scene-set bindings a shading stage needs (camera UBO, the three IBL maps, the
 * light UBO, the shadow map), the light/IBL/shadow/BRDF functions, and shade_surface() - the whole
 * per-fragment lighting evaluation.
 *
 * What stays out: the surface itself (shaders/surface.glsl: material table, textures, alpha test,
 * normal mapping) and the sky (shaders/sky.glsl) - the deferred lighting stage calls the sky function
 * for pixels the G-buffer left empty.
 *
 * A shader including this file must NOT declare the bindings it declares (0, 2, 3, 4, 7, 8) and must
 * use the runtime's shared scene pipeline layout for set 0.
 */

#ifndef DEREN_SHADING_GLSL
#define DEREN_SHADING_GLSL

// Camera UBO (scene set binding 0): view/projection and the world-space eye position (the shading
// path needs the eye to build the view vector and, for the sky, the view ray). The last two matrices
// feed the motion vectors: `proj` is the CURRENT projection INCLUDING the TAA jitter (geometry has to
// be sampled at the jittered offsets), while `view_proj_unjittered` / `prev_view_proj` are the
// jitter-free pair - a jitter that leaked into a motion vector would be read as camera motion and
// would reproject the history to the wrong place every frame.
// THE SLOT GRID, which is what every declaration below addresses (see docs/descriptor_heap_migration.md): the
// constants, the two specialization constants that carry the frame and image indices, and the shared heap arrays.
#include "heap_slots.glsl"

// HEAP-NATIVE (see docs/descriptor_heap_migration.md): the array IS the heap and the slot carries the FRAME, so
// every read is `camera_at(heap_camera_slot).field` (the slot constants are declared with the rest of the grid
// below). The block itself is unchanged - it is a CPU/GPU contract.
// THE UBO/RESOURCE BLOCKS ARE WRAPPED, NOT MOVED: the member lists stay here - ONE copy - and only the
// GLSL-only `layout(descriptor_heap, ...) buffer X { ... } name[];` wrapper differs, because the Slang side
// needs the same member list as a plain struct to build its `ConstantBuffer<X>` handles from. VR_MAT4 is
// `mat4` in GLSL and `row_major float4x4` in Slang (see heap_slots.glsl).
//
// WHY THESE BLOCKS ARE `buffer` AND NOT `uniform`, measured: THE STORAGE CLASS A SHADER READS A HEAP
// DESCRIPTOR THROUGH MUST MATCH THE DESCRIPTOR'S TYPE, and a mismatch is SILENT - no validation finding,
// just zeros. The host writes the camera as VK_DESCRIPTOR_TYPE_STORAGE_BUFFER and Slang's
// `DescriptorHandle<ConstantBuffer<T>>` always fetches through a StorageBuffer-class pointer (a Slang
// ConstantBuffer handle does NOT emit Uniform here), so the GLSL side has to read it as a storage buffer
// too. Both wrong pairings were reproduced: a UNIFORM descriptor read through a StorageBuffer pointer
// (Slang's camera read: zeros, the velocity came out NaN and the motion channel went black) and a STORAGE
// descriptor read through a Uniform pointer (the GLSL block as `uniform`: albedo went black, geometry
// gone). The layout does not move: std430 and std140 give this struct the same offsets (0, 64, 128, 144,
// 208) and the same 272-byte size, because the vec3 sits where a mat4 has to be 16-aligned anyway. The
// LIGHT block below follows for the same reason, and its offsets were checked member by member rather than
// assumed: every member is a vec4/mat4 or a scalar in a packed run, and its only array holds a 16-aligned
// 64-byte struct, so std430 lands `punctual_lights` on 352, `cluster_grid` on 8544 and `cluster_depth` on
// 8560 exactly as std140 did (verified against the glslc-built modules).
// See docs/slang_migration.md section 9, "the first leaf port: two traps, both measured, both fixed", and
// docs/descriptor_heap_migration.md.
#ifndef VR_SLANG
layout(descriptor_heap, descriptor_stride = heap_slot_stride) buffer CameraUBO {
#else
struct CameraUBO {
#endif
    VR_MAT4 view;
    VR_MAT4 proj;
    vec3 camera_pos;
    VR_MAT4 view_proj_unjittered;
    VR_MAT4 prev_view_proj;
#ifndef VR_SLANG
} camera[];
#else
};
#endif

// Split-sum IBL: the prefiltered GGX environment (roughness mip chain), the
// irradiance map for the diffuse ambient, and the BRDF integration LUT.
// IMAGES AND SAMPLERS ARE SEPARATE IN A HEAP, so these are textures now and the sampler is the shared one (the
// host picks which at the fetch sites below).
#ifndef VR_SLANG
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform textureCube env_texture[];
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform textureCube irradiance_texture[];
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2D brdf_lut_texture[];
#endif // the Slang side reaches these through env_cube_sample_lod / irradiance_sample / brdf_lut_sample,
       // which shaders/heap_access.slang defines over DescriptorHandle values (see docs/slang_migration.md)

// The SLOTS these declarations resolve to live in heap_slots.glsl with every other stage's, because a stage that
// does not include this file needs them too (see that file's note on which index each kind of array takes).
// ... and the specular half of the split sum, which lives in a file of its own so that the two sides of a
// subtraction compute it from the same expressions (see that file's header).
#include "ibl_specular.glsl"

// Light UBO (scene set binding 7): the orthographic light view-proj (world -> shadow map) and the
// light direction, followed by the active punctual lights. The direction is filled by the CPU
// (make_directional_light_ubo) and matches the sky sun, so the direct light, the visible sun disc
// and the shadows all agree. Layout must match deren::vulkan::light_ubo in primitive.cppm (std140):
// mat4 | vec4 | 4 floats | uint + 3 pad floats | PunctualLight[4] - the CPU mirrors the "uint + pad"
// slot with one glm::vec4, so the array starts at byte 112 and the block is 368 bytes. (A vec3 pad
// would force 16-byte alignment to 128 and shift every light by 16.)
const int MAX_PUNCTUAL_LIGHTS = 128; // deren::vulkan::max_punctual_lights
const int CLUSTER_TILE_SIZE = 64;    // deren::vulkan::cluster_tile_size (pixels per cluster tile)
const int CLUSTER_LIGHT_CAPACITY = 32; // deren::vulkan::cluster_light_capacity (lights stored per cluster)
const int MAX_SHADOW_CASCADES = 4; // deren::vulkan::max_shadow_cascades

struct PunctualLight {
    vec4 position; // xyz: world position (w unused)
    vec4 color;    // xyz: linear color * intensity (w unused)
    vec4 spot_dir; // xyz: spot axis, normalized for spot lights (w unused)
    vec4 params;   // x = range (0 = infinite), y = 0 point / 1 spot, z = cos(outer cone), w = cos(inner cone, spot only)
};

#ifndef VR_SLANG
layout(descriptor_heap, descriptor_stride = heap_slot_stride) buffer LightUBO {
#else
struct LightUBO {
#endif
    // One orthographic world -> light-clip matrix per cascade: entry 0 covers the near range, the
    // rest the ranges given by cascade_splits. With cascade_count == 1 only entry 0 is fitted and
    // used, which is exactly the single-shadow-map behavior.
    VR_MAT4 light_view_proj[MAX_SHADOW_CASCADES];
    vec4 light_dir;           // xyz: normalized light direction, w: 1 / shadow map size (uv texel)
    vec4 cascade_splits;      // view-space FAR distance of each cascade
    vec4 cascade_texel_world; // world size of one shadow-map texel, per cascade (normal-offset bias)
    float shadow_enabled; // 1.0 = sample the shadow map, 0.0 = fully lit (runtime::set_shadow_enabled)
    // selectable BRDF theory models (gui combos -> runtime::set_brdf_model / set_diffuse_model,
    // CPU-side, riding the std140 padding of this block):
    //   brdf_model:    0 = GGX + joint Smith (default), 1 = GGX + height-correlated Smith,
    //                  2 = Beckmann + Smith, 3 = Blinn-Phong + Smith
    //   diffuse_model: 0 = Lambert (default), 1 = Oren-Nayar
    float brdf_model;
    float diffuse_model;
    float cascade_blend; // fraction of a cascade's range blended into the next one (0.1 = last 10%)
    float cascade_count; // active cascades (1 = the single-map path)
    float rt_shadows; // 1.0 = the sun's shadow is the ray-traced visibility image (binding 14),
                      // 0.0 = the cascaded shadow maps. Rides the std140 padding that keeps
                      // light_count on its 16-byte boundary; see the CPU's light_ubo.
    float sun_intensity;
    float furnace_level;
    uint light_count;
    float exposure; // y lane of the CPU's light_count vec4: linear exposure scale (pre-tonemap)
    float toon_steps;   // cel-shading quantization steps (LightUBO.light_count.z; 0 = PBR)
    float toon_softness; // band edge width in normalized [0,1] space (LightUBO.light_count.w)
    PunctualLight punctual_lights[MAX_PUNCTUAL_LIGHTS];
    // Clustered light culling (M5), appended after the light array so its offset is unchanged:
    //   cluster_grid  x = active tile columns, y = active tile rows, z = depth slices,
    //                 w = 1.0 = read the per-cluster light lists, 0.0 = loop every active light
    //   cluster_depth x = near view depth, y = far view depth the slices span (z/w = screen size,
    //                 used by the cluster pass only)
    vec4 cluster_grid;
    vec4 cluster_depth;
    // The reference pack's square area light ([lighting] area_light_*), APPENDED after cluster_depth so every
    // offset above - and the SHORTER LightUBO copies that shaders/light_cluster.slang and shaders/rt_shadow.slang
    // declare as prefixes - stays exactly what it is. `w` carries the switch rather than a separate flag:
    // area_light.w <= 0 (the default, `[lighting] area_light_size = 0`) = no area light, i.e. today's behaviour.
    // v1.1 (spec 7.2): NO shader reads the `area_light` lane any more - the area light now contributes the sun's
    // direction and radiance (host side) and the world-scale PCF in `calc_shadow_area`; the shadow route below
    // reads only `area_light_axis.w`. The lane stays because the wire layout is pinned by tests and a v2 area
    // integral needs centre/half back, and its sign encoding (w > 0 = contributes energy) stays for the same
    // reason - both signs land on the shipped expression while the feature is off.
    vec4 area_light;      // xyz = emitter centre (WORLD, Y-up metres), w = half the side; w <= 0 = no area light
    vec4 area_light_axis; // xyz = emitter normal (centre -> target); w = penumbra world radius (metres); <= 0 = no area shadow
#ifndef VR_SLANG
} light[];
#else
};
#endif

// Per-cluster light lists (scene set bindings 11/12), written by shaders/light_cluster.slang: one
// entry per cluster in cluster_counts (how many lights landed in it) and a fixed-capacity row per
// cluster in cluster_indices holding the indices into light_at(heap_light_slot).punctual_lights. Storage buffers rather
// than more UBO lanes because the grid is thousands of entries - and small enough (16 lights per
// cluster) that no per-cluster linked list / prefix sum is needed.
#ifndef VR_SLANG
layout(descriptor_heap, descriptor_stride = heap_slot_stride) readonly buffer ClusterCounts {
    uint counts[];
} cluster_counts[];
layout(descriptor_heap, descriptor_stride = heap_slot_stride) readonly buffer ClusterIndices {
    uint indices[];
} cluster_indices[];
#else
struct ClusterCounts {
    uint counts[];
};
struct ClusterIndices {
    uint indices[];
};
#endif // the Slang side reaches these through cluster_count_at / cluster_indices_at

// Shadow map (scene set binding 8): a 2D ARRAY of cascades, sampled with a depth-compare sampler
// (sampler2DArrayShadow) whose LINEAR filtering performs HARDWARE percentage-closer filtering - the
// hardware compares the reference depth against the 2x2 texel neighborhood of the addressed layer
// and returns the lit fraction (no manual 3x3 loop needed). An ARRAY texture rather than an array of
// samplers because the layer is chosen per FRAGMENT: dynamic indexing of a sampler array would need
// dynamically uniform indices, while a texture-array layer is just a coordinate.
// ... and it is a heap TEXTURE (the depth-compare sampler comes from the sampler heap at heap_sampler_shadow),
// per swapchain image, which is what heap_image_index selects.
#ifndef VR_SLANG
layout(descriptor_heap, descriptor_stride = heap_slot_stride) uniform texture2DArray shadow_texture[];
#endif // the Slang side reaches it through shadow_sample(), which the caller passes the slot to

// ---- THE TWO REMAINING FETCHES, each behind a NAME ----
//
// Same reason as the IBL helpers: a GLSL fetch builds its combined sampler at the point of use, and Slang
// cannot express that - there the two halves are one DescriptorHandle (a comparison sampler for the shadow
// map). Naming them is what lets the functions below be shared. The Slang definitions are in
// shaders/heap_access.slang; see docs/slang_migration.md.
#ifndef VR_SLANG
/// one comparison fetch from the cascade array: @p slot is the shadow map's heap slot (the CALLER passes
/// `heap_shadow_slot`, because that macro is frame-slot dependent and a shim may be compiled before the
/// push block that defines the frame slot exists), @p tap is in light space, @p cascade picks the layer and
/// @p depth is the fragment's light-space depth the sampler compares against
float shadow_sample(uint slot, vec2 tap, int cascade, float depth) {
    return texture(sampler2DArrayShadow(shadow_texture[slot], heap_samplers[heap_sampler_shadow]), vec4(tap, float(cascade), depth));
}
/// the diffuse ambient: the irradiance map at the world normal
vec3 irradiance_sample(vec3 n) {
    return texture(samplerCube(irradiance_texture[heap_irradiance_slot], heap_samplers[heap_sampler_texture]), n).rgb;
}
#endif // the Slang definitions are in heap_access.slang

const float PI = 3.14159265359;

/**
 * @brief quantize x into @p steps bands with a soft edge of +- @p softness
 * @param x value in [0,1] (a diffuse falloff factor, a shadow factor, ...)
 * @param steps band count; below 1.5 the value is returned unchanged (plain PBR), so the same code
 *              path serves both styles
 * @param softness band edge width in normalized space; smaller = harder cel edges
 * @return the quantized value in [0,1]
 */
float toon_band(float x, float steps, float softness) {
    if (steps < 1.5) {
        return x;
    }
    float scaled = clamp(x, 0.0, 1.0) * steps;
    float base = floor(scaled);
    float frac = scaled - base;
    float edge = smoothstep(0.5 - softness, 0.5 + softness, frac);
    return (base + edge) / steps;
}

/**
 * @brief shadow factor of ONE cascade for a world-space point: normal-offset bias + 3x3 PCF
 * @param world_pos receiver position in world space
 * @param normal receiver world-space normal
 * @param cascade cascade index (the shadow map array layer)
 * @return 1.0 = fully lit, 0.0 = fully shadowed (the raw per-cascade value; never banded - see DEBT (s))
 *
 * - the sample point is pushed along the world normal by a couple of light-space texels, which
 *   removes most quantization acne on flat receivers WITHOUT a large depth bias - and a large depth
 *   bias is exactly what erases the shadow of a thin caster (a sword, a railing). The normal offset
 *   lets the depth bias below drop to a fraction of the old 0.0015. The offset is per cascade
 *   because the texel size is: cascade 0's texels are a fraction of cascade 3's, and a fixed world
 *   offset would over-bias the near range (detaching contact shadows) and under-bias the far one.
 * - the 3x3 grid of hardware 2x2 comparison taps (4x4 texel footprint) smooths the edge; a single
 *   tap flickered badly on thin geometry. The grid is unrotated on purpose: a rotated grid needs TAA
 *   to hide its per-pixel noise (the deferred path has TAA, the forward path does not yet).
 * - outside the light frustum the fragment is reported lit: each cascade's map covers its own fitted
 *   box only, and runtime::update_shadow_frustum() keeps those boxes on the part of the scene the
 *   camera can see.
 */
float calc_shadow_cascade(vec3 world_pos, vec3 normal, int cascade) {
    float texel_uv = light_at(heap_light_slot).light_dir.w;                    // 1 / shadow map size
    float texel_world = light_at(heap_light_slot).cascade_texel_world[cascade]; // world size of one texel of this cascade

    // normal offset: shift the world position before projecting it into light space
    vec3 offset_pos = world_pos + normal * (texel_world * 2.0);
    vec4 light_clip = light_matrix_at(heap_light_slot, cascade) * vec4(offset_pos, 1.0);
    vec3 ndc = light_clip.xyz / light_clip.w; // ortho projection: w == 1
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float current_depth = ndc.z; // [0,1] (RH_ZO ortho)

    // Outside the light frustum: fully lit (the shadow map covers the scene bounds only)
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || current_depth < 0.0 || current_depth > 1.0) {
        return 1.0;
    }

    // The sampler's compareOp is LESS_OR_EQUAL, so lit = (ref - bias) <= stored depth.
    float bias = 0.0004;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 tap = uv + vec2(float(x), float(y)) * texel_uv;
            lit += shadow_sample(heap_shadow_slot, tap, cascade, current_depth - bias);
        }
    }
    float shadow = lit / 9.0;
    // DEBT (s): the raw per-cascade visibility, UNQUANTIZED on purpose. The reference NEVER quantizes it:
    // its `Cast Shadows` -> RampSelect -> `_RD` chain has no band generator and its `_RD` ramps are
    // continuous remaps, not steps (256x1 8-bit distinct texels: cloth_04 191, body_01 162, hair_01 140,
    // TPLK_cloth_03 64; `deren-ab/goo_debt_s_armA_verify.md` §2.4). The engine's `toon_band` cel knob is
    // therefore PORT-SIDE, with no counterpart in the reference (`190e58f` added it, `7d77615` the
    // discrete combo; subjects in the report §11) and OFF by default (`chores.cppm:136` = 0 ->
    // `main.cpp:2377 toon_band_counts[0] = 0.0f` -> `runtime.cpp:1448 toon_steps = 0`), so dropping
    // its premature band here is byte-neutral; with the knob on the leftover `ndotl` band + highlight
    // block still land one stage later in `evaluate_direct_light`. Only this premature band is removed.
    return shadow;
}

/**
 * @brief shadow factor for a world-space point, selecting (and blending between) the cascades
 * @param world_pos receiver position in world space
 * @param normal receiver world-space normal
 * @return 1.0 = fully lit, 0.0 = fully shadowed
 *
 * The cascade is picked by the fragment's VIEW-SPACE DEPTH against the per-cascade split distances
 * the CPU fitted: cascade i covers [splits[i-1], splits[i]], so the near geometry is shadowed by the
 * small, dense cascade 0 and the far range by the coarse last one - the whole point of cascades is
 * that a single map cannot be dense enough for both.
 *
 * Inside `cascade_blend` of a boundary the two neighbouring cascades are both sampled and mixed:
 * a hard switch would show as a line where the resolution (and the offset) step is.
 */
float calc_shadow(vec3 world_pos, vec3 normal) {
    if (light_at(heap_light_slot).cascade_count < 1.5) {
        return calc_shadow_cascade(world_pos, normal, 0); // single map: no selection to do
    }
    const float view_depth = -(camera_at(heap_camera_slot).view * vec4(world_pos, 1.0)).z; // positive distance along the view
    int cascade = int(light_at(heap_light_slot).cascade_count + 0.5) - 1;                 // past the last split: the farthest
    for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
        if (i >= int(light_at(heap_light_slot).cascade_count + 0.5)) {
            break;
        }
        if (view_depth <= light_at(heap_light_slot).cascade_splits[i]) {
            cascade = i;
            break;
        }
    }
    float shadow = calc_shadow_cascade(world_pos, normal, cascade);

    // blend into the next cascade across the boundary band
    const int next = cascade + 1;
    if (next < int(light_at(heap_light_slot).cascade_count + 0.5)) {
        const float boundary = light_at(heap_light_slot).cascade_splits[cascade];
        const float band = max(boundary * light_at(heap_light_slot).cascade_blend, 1e-4);
        if (view_depth > boundary - band) {
            const float t = clamp((view_depth - (boundary - band)) / band, 0.0, 1.0);
            shadow = mix(shadow, calc_shadow_cascade(world_pos, normal, next), t);
        }
    }
    return shadow;
}

/**
 * @brief the same per-cascade visibility as @ref calc_shadow_cascade, on a WIDER unrotated tap grid
 * @param world_pos receiver position in world space
 * @param normal receiver world-space normal
 * @param cascade the cascade to sample
 * @param half_extent taps run from `-half_extent` to `+half_extent` on each axis
 * @param spacing tap spacing in TEXELS (the shipped kernel is `half_extent 1, spacing 1`)
 * @return raw per-cascade visibility, `1.0` outside the light frustum
 *
 * EVERY LINE EXCEPT THE LOOP IS @ref calc_shadow_cascade'S, deliberately: the normal offset, the bias, the
 * frustum test and the sampler's compare semantics are the ones the shipped 3x3 lookup is measured with, so
 * widening the kernel changes the tap footprint and nothing else. The grid stays UNROTATED for the reason
 * the 3x3 one is (a rotated grid needs TAA to hide its per-pixel noise, and the forward path has none).
 *
 * THIS IS A SEPARATE FUNCTION RATHER THAN A PARAMETER ON THE EXISTING ONE because of `test_goo_toon_math`'s
 * structure pins on `calc_shadow_cascade`'s body (it must stay the short raw-sampling path) AND because the
 * OLD chain compiles this file too: `calc_shadow_cascade` has to stay bit-for-bit the shipped lookup for
 * `shaders/goo_toon.slang`, so the wider kernel lives beside it instead of inside it.
 */
float calc_shadow_cascade_proto(vec3 world_pos, vec3 normal, int cascade, int half_extent, int spacing) {
    float texel_uv = light_at(heap_light_slot).light_dir.w;                    // 1 / shadow map size
    float texel_world = light_at(heap_light_slot).cascade_texel_world[cascade]; // world size of one texel of this cascade

    // normal offset: shift the world position before projecting it into light space
    vec3 offset_pos = world_pos + normal * (texel_world * 2.0);
    vec4 light_clip = light_matrix_at(heap_light_slot, cascade) * vec4(offset_pos, 1.0);
    vec3 ndc = light_clip.xyz / light_clip.w; // ortho projection: w == 1
    vec2 uv = ndc.xy * 0.5 + 0.5;
    float current_depth = ndc.z; // [0,1] (RH_ZO ortho)

    // Outside the light frustum: fully lit (the shadow map covers the scene bounds only)
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || current_depth < 0.0 || current_depth > 1.0) {
        return 1.0;
    }

    // The sampler's compareOp is LESS_OR_EQUAL, so lit = (ref - bias) <= stored depth.
    float bias = 0.0004;
    float step_uv = texel_uv * float(spacing);
    float lit = 0.0;
    float taps = 0.0;
    for (int y = -half_extent; y <= half_extent; ++y) {
        for (int x = -half_extent; x <= half_extent; ++x) {
            vec2 tap = uv + vec2(float(x), float(y)) * step_uv;
            lit += shadow_sample(heap_shadow_slot, tap, cascade, current_depth - bias);
            taps += 1.0;
        }
    }
    return lit / taps;
}

/**
 * @brief @ref calc_shadow on the wider kernel of @ref calc_shadow_cascade_proto
 * @param world_pos receiver position in world space
 * @param normal receiver world-space normal
 * @param half_extent taps run from `-half_extent` to `+half_extent` on each axis
 * @param spacing tap spacing in texels
 * @return 1.0 = fully lit, 0.0 = fully shadowed
 *
 * `half_extent <= 0` RETURNS @ref calc_shadow ITSELF, which is the whole guarantee behind the default
 * `[render] toon_shadow_softness = 0`: that path is the shipped function, not a copy of it, so it cannot
 * drift. The cascade selection and the `cascade_blend` mix are the shipped ones lane for lane - only which
 * kernel the two (or one) samples go through differs.
 */
float calc_shadow_proto(vec3 world_pos, vec3 normal, int half_extent, int spacing) {
    if (half_extent <= 0) {
        return calc_shadow(world_pos, normal);
    }
    if (light_at(heap_light_slot).cascade_count < 1.5) {
        return calc_shadow_cascade_proto(world_pos, normal, 0, half_extent, spacing); // single map: no selection to do
    }
    const float view_depth = -(camera_at(heap_camera_slot).view * vec4(world_pos, 1.0)).z; // positive distance along the view
    int cascade = int(light_at(heap_light_slot).cascade_count + 0.5) - 1;                 // past the last split: the farthest
    for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
        if (i >= int(light_at(heap_light_slot).cascade_count + 0.5)) {
            break;
        }
        if (view_depth <= light_at(heap_light_slot).cascade_splits[i]) {
            cascade = i;
            break;
        }
    }
    float shadow = calc_shadow_cascade_proto(world_pos, normal, cascade, half_extent, spacing);

    // blend into the next cascade across the boundary band
    const int next = cascade + 1;
    if (next < int(light_at(heap_light_slot).cascade_count + 0.5)) {
        const float boundary = light_at(heap_light_slot).cascade_splits[cascade];
        const float band = max(boundary * light_at(heap_light_slot).cascade_blend, 1e-4);
        if (view_depth > boundary - band) {
            const float t = clamp((view_depth - (boundary - band)) / band, 0.0, 1.0);
            shadow = mix(shadow, calc_shadow_cascade_proto(world_pos, normal, next, half_extent, spacing), t);
        }
    }
    return shadow;
}

/**
 * @brief the area light's visibility: @ref calc_shadow_cascade_proto's kernel, sized in WORLD metres
 * @param world_pos receiver position in world space
 * @param normal receiver world-space normal
 * @return 1.0 = fully lit, 0.0 = fully shadowed
 *
 * `area_light_axis.w <= 0` RETURNS @ref calc_shadow ITSELF - the shipped function, not a copy of it - which is
 * the byte-identity guarantee for the default `[lighting] area_light_shadow = false` (and for `area_light_size
 * = 0`). The cascade selection and the `cascade_blend` mix below are @ref calc_shadow_proto's, lane for lane;
 * only the tap half-extent differs, and it is derived per cascade from the world penumbra radius.
 *
 * THE RADIUS IS CONVERTED TO A TAP COUNT, NOT SOLVED FOR: `area_light_axis.w` is the penumbra half-width in
 * world metres (the host publishes it), the cascade's own `cascade_texel_world` says how many metres one texel
 * covers there, and `spacing` texels between taps turn that into `half_extent = round(radius_world / (2 *
 * spacing * texel_world))`, clamped to [1, 8] so the kernel is never degenerate and never wider than the
 * ladder @ref calc_shadow_soft already ships. EACH CASCADE SIZES ITS OWN KERNEL: cascade N's texels cover
 * more world metres than cascade 0's, so the same world radius is fewer taps there.
 *
 * THIS IS NOT PCSS AND NOT RAY TRACING, and it cannot become either without a new shadow representation: the
 * shadow map is exposed only through a comparison sampler (shaders/heap_access.slang's `shadow_sample`, which
 * returns the hardware-compared 0/1 for its own 2x2 footprint), so there is no readable blocker depth and the
 * physical penumbra `(d_receiver - d_blocker) / d_blocker * size` cannot be reconstructed. The width is
 * whatever `[lighting] area_light_softness` specifies (0 = the host's automatic "5 cm gap + source side" default),
 * which is why the name here is a world-space PCF radius and nothing more. A real partially-occluded
 * (area-integrated) visibility term is v2.
 */
float calc_shadow_area(vec3 world_pos, vec3 normal) {
    const float radius_world = light_at(heap_light_slot).area_light_axis.w; // penumbra radius, world metres
    if (radius_world <= 0.0) {
        return calc_shadow(world_pos, normal); // THE SHIPPED PATH, not a copy of it
    }
    const int spacing = 16; // texels between taps: the kernel spans `2 * half_extent * spacing` texels
    if (light_at(heap_light_slot).cascade_count < 1.5) {
        const float texel_world = light_at(heap_light_slot).cascade_texel_world[0];
        const int half_extent = clamp(int(round(radius_world / (2.0 * float(spacing) * texel_world))), 1, 8);
        return calc_shadow_cascade_proto(world_pos, normal, 0, half_extent, spacing); // single map: no selection to do
    }
    const float view_depth = -(camera_at(heap_camera_slot).view * vec4(world_pos, 1.0)).z; // positive distance along the view
    int cascade = int(light_at(heap_light_slot).cascade_count + 0.5) - 1;                 // past the last split: the farthest
    for (int i = 0; i < MAX_SHADOW_CASCADES; ++i) {
        if (i >= int(light_at(heap_light_slot).cascade_count + 0.5)) {
            break;
        }
        if (view_depth <= light_at(heap_light_slot).cascade_splits[i]) {
            cascade = i;
            break;
        }
    }
    const int half_extent = clamp(
        int(round(radius_world / (2.0 * float(spacing) * light_at(heap_light_slot).cascade_texel_world[cascade]))), 1, 8);
    float shadow = calc_shadow_cascade_proto(world_pos, normal, cascade, half_extent, spacing);

    // blend into the next cascade across the boundary band (the next cascade sizes its own kernel)
    const int next = cascade + 1;
    if (next < int(light_at(heap_light_slot).cascade_count + 0.5)) {
        const float boundary = light_at(heap_light_slot).cascade_splits[cascade];
        const float band = max(boundary * light_at(heap_light_slot).cascade_blend, 1e-4);
        if (view_depth > boundary - band) {
            const float t = clamp((view_depth - (boundary - band)) / band, 0.0, 1.0);
            const int next_half_extent = clamp(
                int(round(radius_world / (2.0 * float(spacing) * light_at(heap_light_slot).cascade_texel_world[next]))), 1, 8);
            shadow = mix(shadow, calc_shadow_cascade_proto(world_pos, normal, next, next_half_extent, spacing), t);
        }
    }
    return shadow;
}

/**
 * @brief the toon chain's shadow with the `[render] toon_shadow_softness` ladder applied
 * @param world_pos receiver position in world space
 * @param normal receiver world-space normal
 * @param level the rig's softness lane: 0 = shipped, 1..4 = the ladder below (rounded and clamped here)
 * @return 1.0 = fully lit, 0.0 = fully shadowed
 *
 * THE LADDER IS FROZEN and it is `(half_extent, spacing)` in texels: 1 = (2,2), 2 = (3,3), 3 = (5,4),
 * 4 = (8,3), i.e. `(2*half_extent + 1)^2` = 25 / 49 / 121 / 289 taps. The number to quote for the width is
 * the TEXEL SPAN the taps reach, `2*half_extent*spacing + 2` indices first-to-last (each hardware comparison
 * tap carries its own 2x2 texel footprint, which is the same counting as this file's shipped `calc_shadow`
 * note - "the 3x3 grid of hardware 2x2 comparison taps (4x4 texel footprint)" - and level 0 is exactly that
 * lookup), so levels 1..4 span 10 / 20 / 42 / 50 and level 0 spans 4. The host publishes the level already
 * sanitized (see the `[render] toon_shadow_softness` note in `config.example.toml`); the rounding and the
 * clamp below are only a SECOND LINE OF DEFENCE FOR FINITE VALUES - a finite out-of-range level, or one that
 * sits between two steps.
 *
 * NaN IS NOT HANDLED IN THIS SHADER, and no amount of rounding here could handle it: `floor(NaN + 0.5)`,
 * `clamp` and the float-to-int conversion are each undefined on NaN, so a NaN reaching this lane is undefined
 * behaviour. The guard that actually exists is the host's `std::isnan` gate (`application_configuration.cpp`,
 * NaN -> 0) together with this lane's default of 0; the host's `std::clamp` also owns the infinities
 * (+inf -> 4, -inf -> 0). So do NOT read the rule below as "any writer of the lane is safe" - it is false for
 * NaN, and it is only true because the host sanitizes before publishing.
 *
 * LEVEL 0 RETURNS @ref calc_shadow UNCHANGED - no extra tap, no extra fetch, the shipped expression - which
 * is the byte-identity guarantee, and it is why the call site can route the whole chain through here.
 */
float calc_shadow_soft(vec3 world_pos, vec3 normal, float level) {
    int level_step = int(clamp(floor(level + 0.5), 0.0, 4.0));
    if (level_step == 0) {
        return calc_shadow(world_pos, normal); // THE SHIPPED PATH, not a copy of it
    }
    if (level_step == 1) {
        return calc_shadow_proto(world_pos, normal, 2, 2);
    }
    if (level_step == 2) {
        return calc_shadow_proto(world_pos, normal, 3, 3);
    }
    if (level_step == 3) {
        return calc_shadow_proto(world_pos, normal, 5, 4);
    }
    return calc_shadow_proto(world_pos, normal, 8, 3);
}

/**
 * @brief GGX / Trowbridge-Reitz normal distribution (matches UE's D_GGX)
 * @param n world normal
 * @param h half vector
 * @param roughness perceptual roughness
 * @return the NDF value, finite even at a perfectly smooth specular hotspot (the denominator is
 *         clamped: roughness 0 with ndoth == 1 would otherwise be 0/0 = NaN and blacken the fragment)
 */
float distribution_ggx(vec3 n, vec3 h, float roughness) {
    float a2 = roughness * roughness;
    a2 = a2 * a2; // perceptual roughness -> alpha^2 (UE passes Pow4(Roughness))
    float ndoth = max(dot(n, h), 0.0);
    float denom = ndoth * ndoth * (a2 - 1.0) + 1.0;
    // Guard the denominator (same NaN class as the Vis guards below): at the exact specular
    // hotspot center of a perfectly smooth surface (roughness 0 AND ndoth == 1) denom is 0 and
    // a2 / (PI * 0) is 0/0 = NaN, turning the fragment black. Clamping keeps D finite (it is
    // 0 there anyway - an infinitely sharp lobe needs no finite value).
    denom = max(denom, 1e-6);
    return a2 / (PI * denom * denom);
}

/// @brief Beckmann NDF preset (brdf_model 2); same alpha^2 mapping as distribution_ggx
float distribution_beckmann(vec3 n, vec3 h, float roughness) {
    float a2 = roughness * roughness;
    a2 = max(a2 * a2, 1e-6); // roughness 0 would make 0/0 (or inf) below at ndoth == 1
    float ndoth = max(dot(n, h), 1e-4); // guard: tan blows up at grazing, exp() dies first
    float cos2 = ndoth * ndoth;
    float tan2 = (1.0 - cos2) / cos2;
    return exp(-tan2 / a2) / (PI * a2 * cos2 * cos2);
}

/// @brief Blinn-Phong NDF preset (brdf_model 3), exponent capped at 4096
float distribution_blinn_phong(vec3 n, vec3 h, float roughness) {
    float a2 = roughness * roughness;
    a2 = max(a2 * a2, 1e-6);
    const float exponent = min(2.0 / a2 - 2.0, 4096.0); // classic n = 2 / alpha^2 - 2
    const float ndoth = max(dot(n, h), 0.0);
    return (exponent + 2.0) * pow(ndoth, exponent) / (2.0 * PI);
}

/**
 * @brief Geometric shadowing-masking as a visibility term: Vis = G / (4 NoV NoL)
 * @param n world normal
 * @param v view direction
 * @param l light direction
 * @param roughness perceptual roughness
 * @return Heitz's joint Smith approximation for GGX (UE's Vis_SmithJointApprox)
 *
 * One term shadows AND masks in the half-vector sense, so the BRDF is specular = D * Vis * F with no
 * separate 4 NoV NoL denominator (UE's SpecularGGX structure). The denominator is clamped: at an
 * exact grazing silhouette both ndotv and ndotl are 0, and inf * 0 = NaN would turn the whole
 * fragment black (visible as black flashes on thin skinned limbs).
 */
float geometry_vis_smith_joint_approx(vec3 n, vec3 v, vec3 l, float roughness) {
    float a2 = roughness * roughness;
    a2 = a2 * a2; // perceptual roughness -> alpha^2 (UE passes Pow4(Roughness))
    float a = sqrt(a2);
    float ndotv = max(dot(n, v), 0.0);
    float ndotl = max(dot(n, l), 0.0);
    float vis_v = ndotl * (ndotv * (1.0 - a) + a);
    float vis_l = ndotv * (ndotl * (1.0 - a) + a);
    // Guard the denominator: at an exact grazing silhouette both ndotv and ndotl are 0, the sum
    // below is 0 and 0.5/0 would be +inf. The direct term multiplies by radiance (ndotl * ...),
    // so inf * 0 = NaN would turn the whole fragment black (visible as black flashes on thin
    // skinned limbs like RecursiveSkeletons at certain poses). Clamping the sum keeps Vis large
    // but finite - at ndotl == 0 the product is exactly 0 either way.
    return 0.5 / max(vis_v + vis_l, 1e-5);
}

/// @brief Height-correlated Smith visibility (Heitz 2014), the exact form the approximation above
///        simplifies - selected as brdf_model 1; same alpha^2 and the same grazing guard
float geometry_vis_smith_height_correlated(vec3 n, vec3 v, vec3 l, float roughness) {
    float a2 = roughness * roughness;
    a2 = a2 * a2;
    float ndotv = max(dot(n, v), 0.0);
    float ndotl = max(dot(n, l), 0.0);
    const float sqrt_v = sqrt(ndotv * ndotv * (1.0 - a2) + a2);
    const float sqrt_l = sqrt(ndotl * ndotl * (1.0 - a2) + a2);
    // same grazing guard as geometry_vis_smith_joint_approx
    return 0.5 / max(ndotl * sqrt_v + ndotv * sqrt_l, 1e-5);
}

/// @brief Oren-Nayar diffuse (roughness-dependent), selected as diffuse_model 1: the classic A/B
///        approximation of the paper's integral; roughness 0 reduces to Lambert (A = 1, B = 0)
float oren_nayar_diffuse(vec3 n, vec3 v, vec3 l, float roughness, float ndotv, float ndotl) {
    const float alpha2 = roughness * roughness;
    const float A = 1.0 - 0.5 * alpha2 / (alpha2 + 0.33);
    const float B = 0.45 * alpha2 / (alpha2 + 0.09);
    const float sin_v = sqrt(max(1.0 - ndotv * ndotv, 0.0));
    const float sin_l = sqrt(max(1.0 - ndotl * ndotl, 0.0));
    // cos(phi_i - phi_o): angle between the view/light projections onto the tangent plane
    const vec3 vp = v - n * ndotv;
    const vec3 lp = l - n * ndotl;
    const float vp_len = length(vp);
    const float lp_len = length(lp);
    float cos_diff = 0.0;
    if (vp_len > 1e-6 && lp_len > 1e-6) {
        cos_diff = clamp(dot(vp, lp) / (vp_len * lp_len), 0.0, 1.0);
    }
    // sin(alpha) * tan(beta), alpha/beta = the larger/smaller of the two incident angles
    const float sin_max = max(sin_v, sin_l);
    const float tan_min = min(sin_v / max(ndotv, 1e-4), sin_l / max(ndotl, 1e-4));
    return (A + B * cos_diff * sin_max * tan_min) / PI;
}

/// @brief Schlick's Fresnel approximation: @p f0 + (1 - f0) * (1 - cos(theta))^5
vec3 fresnel_schlick(float cos_theta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

/**
 * @brief Cook-Torrance direct light for ONE light, in radiance units
 * @param n world normal
 * @param v view direction
 * @param base_color albedo
 * @param metallic material factor
 * @param roughness perceptual roughness material factor
 * @param f0 the Fresnel reflectance at normal incidence
 * @param light_dir surface-to-light direction
 * @param light_radiance radiance * attenuation
 * @return outgoing radiance (already multiplied by the diffuse ndotl factor)
 *
 * @p light_radiance carries the light's intensity/attenuation (and, for the sun, its shadow factor);
 * ndotl is folded in here, so the directional sun and every punctual light take the exact same code
 * path. The BRDF theory selections (LightUBO.brdf_model / diffuse_model) and the cel-shading bands
 * are applied inside.
 *
 * @note these presets drive the DIRECT lights only. The IBL ambient in shade_surface() always uses
 *       the fixed GGX model (prefiltered GGX environment + GGX BRDF LUT with Fdez-Aguera
 *       multiscatter compensation, Lambert diffuse irradiance) - the GUI preset switch is an honest
 *       direct-light A/B, not a whole-scene model comparison.
 */
vec3 evaluate_direct_light(vec3 n, vec3 v, vec3 base_color, float metallic, float roughness, vec3 f0, vec3 light_dir, vec3 light_radiance) {
    vec3 l = normalize(light_dir);
    // Half vector: normalize(v + l) is NaN when the light sits exactly behind the fragment
    // along the view ray (v + l == 0, e.g. a point light placed at the camera). Fall back to
    // the normal - a degenerate lobe with no real reflection, but a finite one.
    const vec3 sum = v + l;
    vec3 h = length(sum) > 1e-6 ? sum / length(sum) : n;

    // ---- Specular NDF / visibility by the selected BRDF preset. Preset 0 = GGX + joint
    //      Smith, byte-for-byte the historic default; each other preset differs by exactly one
    //      piece so the gui is a live A/B compare.
    float ndf;
    float vis;
    const int brdf_model = int(light_at(heap_light_slot).brdf_model + 0.5);
    if (brdf_model == 1) {
        ndf = distribution_ggx(n, h, roughness);
        vis = geometry_vis_smith_height_correlated(n, v, l, roughness);
    } else if (brdf_model == 2) {
        ndf = distribution_beckmann(n, h, roughness);
        vis = geometry_vis_smith_joint_approx(n, v, l, roughness);
    } else if (brdf_model == 3) {
        ndf = distribution_blinn_phong(n, h, roughness);
        vis = geometry_vis_smith_joint_approx(n, v, l, roughness);
    } else {
        ndf = distribution_ggx(n, h, roughness);
        vis = geometry_vis_smith_joint_approx(n, v, l, roughness);
    }
    vec3 f = fresnel_schlick(max(dot(h, v), 0.0), f0);

    // UE structure: specular = D * Vis * F (Vis already folds in G / (4 NoV NoL))
    vec3 specular = ndf * vis * f;

    vec3 kd = (1.0 - f) * (1.0 - metallic);
    float ndotv = max(dot(n, v), 0.0);
    float ndotl = max(dot(n, l), 0.0);

    // ---- cel/toon shading (no-op when LightUBO.toon_steps < 1): quantize the diffuse falloff
    //      into bands and turn the specular lobe into a single hard highlight block. The
    //      visibility terms keep their unquantized ndotl, so only the shading response bands -
    //      the silhouette stays smooth.
    if (light_at(heap_light_slot).toon_steps > 0.5) {
        ndotl = toon_band(ndotl, light_at(heap_light_slot).toon_steps, light_at(heap_light_slot).toon_softness);
        float ndoth = max(dot(n, h), 0.0);
        float highlight_threshold = 0.5 + 0.5 * (1.0 - roughness); // smooth surfaces -> tighter highlight
        specular *= smoothstep(highlight_threshold - light_at(heap_light_slot).toon_softness, highlight_threshold + light_at(heap_light_slot).toon_softness, ndoth);
    }

    // ---- Diffuse by the selected model (LightUBO.diffuse_model): Lambert (default) or the
    //      roughness-dependent Oren-Nayar approximation (0 -> Lambert).
    vec3 diffuse;
    if (int(light_at(heap_light_slot).diffuse_model + 0.5) == 1) {
        diffuse = kd * base_color * oren_nayar_diffuse(n, v, l, roughness, ndotv, ndotl);
    } else {
        diffuse = kd * base_color / PI;
    }
    return (diffuse + specular) * (light_radiance * ndotl);
}

/// @brief IBL: split-sum approximation (ported from glTF-Sample-Renderer's ibl.glsl); the ambient
///        always uses this fixed GGX model, independent of the BRDF presets above
/// Diffuse ambient: irradiance map lookup by the world normal
vec3 get_diffuse_light(vec3 n) {
    return irradiance_sample(n);
}

// The specular half - get_specular_sample / get_ibl_ggx_fresnel / get_ibl_radiance_ggx - is
// shaders/ibl_specular.glsl's, included above. It moved there so a subtraction would use the same
// expressions rather than a third copy; see that file's header for what was
// verified. The three below are the call sites' names for it.

/**
 * @brief depth slice a view-space depth falls into (exponential slicing between the cluster range)
 * @param view_depth positive distance along the view direction
 * @param slices active slice count
 * @return slice index in [0, slices - 1]
 *
 * MUST stay identical to the same function in shaders/light_cluster.slang: the compute pass assigns
 * lights by unprojecting exactly these slice boundaries, so a different rounding here would put a
 * fragment in a cluster the lights were never assigned to (a light popping out at a slice edge).
 */
int cluster_slice_of(float view_depth, int slices) {
    const float near = max(light_at(heap_light_slot).cluster_depth.x, 1e-4);
    const float far = max(light_at(heap_light_slot).cluster_depth.y, near * 1.0001);
    const float t = clamp(log(max(view_depth, near) / near) / log(far / near), 0.0, 1.0);
    return clamp(int(t * float(slices)), 0, slices - 1);
}

/**
 * @brief cluster index of a pixel, or -1 when clustering is off (the brute-force path)
 * @param pixel the shaded pixel, in PIXELS from the top-left (y down - the same convention the compute
 *        shader's dispatch uses); a fragment passes `ivec2(gl_FragCoord.xy)`
 * @param world_pos the shaded point's world position (its view depth picks the slice)
 * @note THE PIXEL IS A PARAMETER rather than read from gl_FragCoord, and that is what makes this file
 *       usable from a COMPUTE shader: `shaders/megalights_trace.slang` includes it for the same BRDF and
 *       the same cluster lists, and gl_FragCoord does not exist there. The fragment path's wrapper below
 *       is the only place the builtin appears.
 */
int cluster_index_at(ivec2 pixel, vec3 world_pos) {
    if (light_at(heap_light_slot).cluster_grid.w < 0.5) {
        return -1;
    }
    const int tiles_x = int(light_at(heap_light_slot).cluster_grid.x);
    const int tiles_y = int(light_at(heap_light_slot).cluster_grid.y);
    const int slices = int(light_at(heap_light_slot).cluster_grid.z);
    if (tiles_x <= 0 || tiles_y <= 0 || slices <= 0) {
        return -1;
    }
    const ivec2 tile = clamp(pixel / int(CLUSTER_TILE_SIZE), ivec2(0), ivec2(tiles_x - 1, tiles_y - 1));
    const float view_depth = -(camera_at(heap_camera_slot).view * vec4(world_pos, 1.0)).z;
    return (cluster_slice_of(view_depth, slices) * tiles_y + tile.y) * tiles_x + tile.x;
}

/**
 * @brief how many punctual lights the loop must visit for this fragment
 * @param cluster the fragment's cluster index, or -1 for the brute-force path
 */
int cluster_light_count_for(int cluster) {
    if (cluster < 0) {
        return int(light_at(heap_light_slot).light_count);
    }
    return int(min(cluster_count_at(heap_cluster_count_slot, cluster), uint(CLUSTER_LIGHT_CAPACITY)));
}

/**
 * @brief the i-th light index of a cluster (i < cluster_light_count_for(cluster))
 * @param cluster the fragment's cluster index, or -1: then the light index IS i
 */
int cluster_light_index(int cluster, int i) {
    if (cluster < 0) {
        return i;
    }
    return int(cluster_indices_at(heap_cluster_index_slot, cluster * int(CLUSTER_LIGHT_CAPACITY) + i));
}

/**
 * @brief the radiance one punctual light delivers at a surface point, and where it comes from
 * @param pl the light (the light UBO's entry)
 * @param world_pos the receiving point
 * @param out_to_light the unit direction from the point TOWARDS the light
 * @param out_distance the distance to the light, in world units
 * @return linear radiance arriving at the point, BEFORE the BRDF
 *
 * THE ONE DEFINITION OF A PUNCTUAL LIGHT'S ATTENUATION, extracted so that the stochastic lighting pass
 * (`shaders/megalights_trace.slang`) and this shader's own loop cannot disagree about it: the trace pass
 * has to evaluate a light's contribution to build its sampling PDF, and a second copy of inverse-square
 * plus the range fade plus the spot cone is a second chance to pick different thresholds.
 *
 * The falloff is `1 / (1 + d^2)` rather than a physical `1 / d^2`: it is well behaved at zero distance,
 * which is what an artist-facing intensity wants. `range` (0 = infinite) adds a smooth cutoff - the fade
 * is SQUARED so the value and its slope both reach zero at the range boundary.
 */
vec3 punctual_light_radiance(const PunctualLight pl, vec3 world_pos, out vec3 out_to_light, out float out_distance) {
    const vec3 to_light = pl.position.xyz - world_pos;
    out_distance = length(to_light);
    out_to_light = out_distance > 1e-6 ? to_light / out_distance : vec3(0.0, 1.0, 0.0);
    vec3 radiance = pl.color.xyz / (1.0 + out_distance * out_distance);
    const float range = pl.params.x;
    if (range > 0.0) {
        // smooth range cutoff (no hard pop at the boundary)
        const float d = out_distance / range;
        const float fade = clamp(1.0 - d * d, 0.0, 1.0);
        radiance *= fade * fade;
    }
    if (pl.params.y > 0.5) { // spot light: cone around spot_dir
        const float outer = pl.params.z;
        // inner cone: cos of the inner half-angle (glTF KHR innerConeAngle, set by the CPU);
        // w == 0 means the light did not specify one -> legacy soft-inner mix(outer, 1, 0.6)
        const float inner = pl.params.w > 0.0 ? pl.params.w : mix(outer, 1.0, 0.6);
        const float cone = smoothstep(outer, inner, dot(-out_to_light, normalize(pl.spot_dir.xyz)));
        radiance *= cone;
    }
    return radiance;
}

/**
 * @brief everything the shading stage needs to know about one surface point
 * @note the forward path fills this from its interpolated fragment inputs, the deferred path from
 *       the G-buffer texels - which is the whole point: the lighting below cannot tell them apart
 */
struct shade_input {
    ivec2 pixel;    // the pixel being shaded, in PIXELS (y down); the cluster lookup's tile coordinate
    vec3 world_pos; // world-space position (view vector + shadow lookup)
    vec3 normal;    // world-space shading normal (normal-mapped, double-sided flipped)
    vec3 albedo;    // base color, linear
    vec3 emissive;  // emissive radiance, linear (added after the lighting)
    float metallic; // 0 = dielectric, 1 = metal
    float roughness; // perceptual roughness
    float ao;       // ambient occlusion: scales the IBL ambient only, never the direct light
    /**
     * 1 = the PUNCTUAL lights are somebody else's business this frame, so this stage must not add them.
     *
     * The somebody is `shaders/megalights_trace.slang` (docs/megalights.md): it samples a few of the pixel's
     * lights, traces one visibility ray per sample and produces the shadowed estimate that the deferred
     * lighting stage then adds back - so this loop, which knows nothing about occlusion, would double every
     * punctual light in the frame.
     *
     * IT IS A RECORDED FACT RATHER THAN THE KNOB, and the difference matters: the renderer sets it from
     * whether the stochastic pass actually recorded this frame, so a frame whose pass was gated off keeps
     * this loop instead of losing its punctual lights entirely.
     */
    float punctual_replaced;
    // < 0 = no override: the shadow comes from calc_shadow() as it always did. >= 0 = use this factor
    // instead, which is how the deferred path hands in the RAY-TRACED visibility (it is a screen-space
    // lookup, so it cannot be recomputed from world_pos inside the shared lighting code). The forward
    // path and every other caller leave it negative, so their behaviour is unchanged by construction.
    float shadow_override;
};

/**
 * @brief evaluate the full lighting of one surface point
 * @param s the surface properties (see shade_input)
 * @return linear HDR outgoing radiance - no exposure, no tonemapping, no display encoding: the post
 *         pass owns all display-referred work, so a deferred frame and a forward frame of the same
 *         scene go through exactly the same display pipeline
 *
 * The directional sun through evaluate_direct_light() (attenuated by calc_shadow()), the active
 * punctual lights through the same BRDF path, the split-sum IBL ambient (diffuse irradiance +
 * prefiltered specular, always the fixed GGX model), then emissive. The shadow factor attenuates the
 * directional sun only; the IBL ambient stays unshadowed, which is the usual approximation and keeps
 * interiors from going pitch black.
 */
vec3 shade_surface(shade_input s) {
    const vec3 v = normalize(camera_at(heap_camera_slot).camera_pos - s.world_pos);
    const vec3 f0 = mix(vec3(0.04), s.albedo, s.metallic);

    vec3 direct = vec3(0.0);
    // directional sun: shadow factor attenuates only this light; IBL ambient stays unshadowed
    {
        // A ray-traced override wins over both: it IS the shadow, already resolved per pixel.
        float shadow = 1.0;
        if (s.shadow_override >= 0.0) {
            shadow = s.shadow_override;
        } else if (light_at(heap_light_slot).shadow_enabled > 0.5) {
            shadow = calc_shadow(s.world_pos, s.normal);
        }
        direct += evaluate_direct_light(s.normal, v, s.albedo, s.metallic, s.roughness, f0, light_at(heap_light_slot).light_dir.xyz, vec3(7.5 * light_at(heap_light_slot).sun_intensity) * shadow);
    }
    // punctual lights (point/spot, no shadow casting in this version): inverse-square falloff
    // (well-behaved at zero distance) with an optional smooth range cutoff; spots add a soft
    // cone mask between the inner and outer half-angles.
    //
    // Clustered culling (M5): light_cluster.comp sorted the lights into screen-space tiles x
    // exponential depth slices, so this loop only touches the lights that can reach THIS pixel -
    // which is what makes a light count two orders of magnitude past the old four affordable. With
    // clustering off (cluster_grid.w == 0) the loop walks every active light instead, the
    // brute-force reference the clustered path is verified against.
    const int frag_cluster = cluster_index_at(s.pixel, s.world_pos);
    const int punctual_count = s.punctual_replaced != 0.0 ? 0 : cluster_light_count_for(frag_cluster);
    for (int i = 0; i < MAX_PUNCTUAL_LIGHTS; ++i) {
        if (i >= punctual_count) {
            break;
        }
        const int light_index = cluster_light_index(frag_cluster, i);
        const PunctualLight pl = light_at(heap_light_slot).punctual_lights[light_index];
        // The attenuation is `punctual_light_radiance`'s (see its note): the stochastic lighting pass
        // evaluates the same function to build its sampling PDF, so the two cannot drift.
        vec3 dir;
        float dist;
        const vec3 radiance = punctual_light_radiance(pl, s.world_pos, dir, dist);
        if (radiance != vec3(0.0)) {
            direct += evaluate_direct_light(s.normal, v, s.albedo, s.metallic, s.roughness, f0, dir, radiance);
        }
    }

    // ---- IBL (split-sum): diffuse irradiance + prefiltered specular ----
    vec3 ibl_diffuse = get_diffuse_light(s.normal);
    vec3 ibl_specular = ibl_specular_radiance(s.normal, v, s.roughness);
    vec3 fresnel_ibl = ibl_specular_fresnel(s.normal, v, s.roughness, f0, 1.0);

    // Metals have no diffuse term: diffuse ambient is scaled by (1 - metallic),
    // metal color comes entirely from specular environment (matches the official mix(dielectric, metal, metallic))
    vec3 ambient = ibl_diffuse * s.albedo * s.ao * (1.0 - s.metallic);
    // ... and dropped entirely on a frame that does not add the diffuse ambient at all (see
    // shade_input). A BRANCH rather than a fifth factor, and it is not an optimisation: multiplying the
    // finished term by 1.0 is algebraically the identity but NOT bit-identical - folding a fifth operand
    // into the expression changes how the compiler contracts the chain, and it moved one 8-bit texel of
    // 1036800 on one measured frame (blue, one step). Zeroing the finished value leaves every frame that
    // does not use the switch bit for bit what it was, and that is what makes "the frames that do not use
    // it are unchanged" a check rather than a claim.
    vec3 specular_ibl = ibl_specular * fresnel_ibl * s.ao;

    vec3 color = ambient + direct + specular_ibl + s.emissive;

    // The scene target is HDR: this stage writes linear radiance. Exposure, ACES tonemapping and
    // gamma happen once in the post-process pass (post.frag), which also gives the bloom chain a
    // linear image to work on.
    return max(color, vec3(0.0));
}

#endif // DEREN_SHADING_GLSL
