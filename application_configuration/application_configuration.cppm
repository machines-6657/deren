// ============================================================================
// module: app_config
// module version: 0.30.0  (independent of the app version in CMakeLists project(VERSION))
//
// Startup configuration: TOML file (config.toml / --config) merged with argv.
// Pure CPU, no Vulkan dependency.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

// toml++ is header-only and picks its API from the exception mode it detects in the compiler flags
// (TOML_EXCEPTIONS): with exceptions OFF, parse_result is a type with operator bool and error(); with
// them ON it is an alias of toml::table and the parse functions return something else. This project
// compiles with -fno-exceptions, so the header would detect the mode by itself - but the MSVC dialect
// deliberately uses /EHsc (there is no supported "no exceptions" spelling beside the STL), where the
// detection would pick the throwing API and the two translation units of this target would disagree.
// CMakeLists.txt therefore defines TOML_EXCEPTIONS=0 for app_config on every toolchain, which is the
// mode the code below is written against - do not remove it as "redundant" on clang.
#include <toml++/toml.hpp>

export module deren.application_configuration;
export import deren.vstd;
import deren.utility;

/**
 * @file application_configuration.cppm
 * @defgroup app_config Application Startup Config
 * @brief load deren startup settings from a TOML file, merged with command-line
 *        arguments (`--config <path>` overrides the default file; explicit argv values for the
 *        model / grid override the file). Pure CPU, no Vulkan dependency.
 *
 * Example config.toml:
 * @code
 * # top level: model to load
 * model = "gltf_model/DamagedHelmet.gltf"
 * grid_side = 0     # > 1 enables the instancing stress grid (0 = off)
 *
 * [paths]
 * shaders_dir = ""  # shader SPIR-V dir (empty = auto-locate "shaders/" upward)
 * model_dir   = ""  # default model dir used when model is empty (auto-locate gltf_model/)
 * screenshot_dir = ""  # base directory for F12 screenshots (empty = current working directory)
 *
 * [render]
 * window_width  = 1080
 * window_height = 960
 * window_title  = "deren"
 * vsync = true     # true = FIFO_LATEST_READY (vsync), false = mailbox (uncapped)
 * max_fps = 0      # 0 = uncapped (what a throughput measurement needs), else a frame rate cap
 * clear_color = [0.02, 0.02, 0.03]  # background clear color, RGB in 0..1
 * camera_fit = "exterior"  # "exterior" fits the whole model from outside; "interior" stands inside
 *                          # and looks down the longest horizontal axis (a hall / nave / corridor)
 * shadow = true    # record the directional shadow pass each frame
 * fxaa   = false   # anti-alias the final image (adds one fullscreen pass; needs fxaa.frag.spv)
 * gpu_timings = true  # measure + report per-pass GPU milliseconds (timestamp queries)
 * gbuffer_debug = false  # draw the G-buffer + one of its channels instead of the shaded scene
 * taa = false            # temporal anti-aliasing (jitter + resolved history)
 * rt_shadows = false    # ray-traced sun shadows (needs a device with ray queries; else ignored)
 * rt_mask_bake = false  # bake alphaMode MASK into the acceleration structures (off: the rule measured worse)
 * furnace = false       # verification mode: sun off, environment a constant, so the answer is analytic
 * animation_time = -1.0 # pin a keyframe animation at N seconds (-1 = play it; playback is wall-clock)
 * gbuffer_channel = 1    # which channel: 0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 id,
 *                        # 6 depth, 7 flags, 8 motion (the motion vector, amplified - see the shader)
 * validation_layers = true  # Vulkan validation layers + debug messenger (Debug builds default on, Release off)
 *
 * [gui]
 * show         = true    # show the Dear ImGui debug overlay by default
 * panel_width  = 380     # default debug-panel width (0 = auto-size)
 * panel_height = 140     # default debug-panel height (0 = auto-size)
 *
 * [lighting]
 * env_size     = 256   # environment cubemap size
 * env_mip_count = 5    # prefiltered env mip chain length
 * irr_size     = 32    # irradiance cubemap size
 * lut_size     = 256   # BRDF LUT size
 * environment_hdr = "" # equirect HDR image used as the IBL instead of the procedural sky ("" = the sky)
 * environment_intensity = 0.35 # linear multiplier on that image (the reference's own world_strength)
 * @endcode
 */
namespace deren::app_config {
#ifdef NDEBUG
    // Release builds default validation layers OFF (historic behavior); enable them when needed
    // via [render] validation_layers = true (e.g. debugging in a Release build).
    inline constexpr bool default_validation_layers = false;
#else
    // Debug builds keep the validation layers + debug messenger ON by default (historic
    // behavior); the config can turn them off for raw performance.
    inline constexpr bool default_validation_layers = true;
#endif

    export struct path_settings {
        std::string shaders_dir = {};    // shader SPIR-V dir (empty = auto-locate "shaders/" upward)
        std::string model_dir = {};      // default model dir used when model is empty (auto-locate gltf_model/ if empty)
        std::string screenshot_dir = {}; // F12 screenshot base directory (empty = current working directory)
    };

    /**
     * @ingroup app_config
     * @brief Vulkan/render preferences consumed by the runtime/core (applied via a create_info
     *        that the runtime/core layers add); parsed here but not interpreted by app_config.
     */
    export struct render_settings {
        int32_t window_width = 1080;
        int32_t window_height = 960;
        std::string window_title = "deren";                       // GLFW window title
        bool vsync = true;                                        // true = FIFO_LATEST_READY (FIFO fallback), false = mailbox (uncapped)
        double max_fps = 0.0;                                     // 0 = uncapped; a positive value caps the render loop
        std::array<float, 3> clear_color = {0.02f, 0.02f, 0.03f}; // background clear color (RGB, 0..1)
        // Initial camera framing ([render] camera_fit): how main() aims the orbit camera at the
        // imported scene. "exterior" (default) fits the WHOLE model from outside - the right answer
        // for a compact object, and what every earlier version did. "interior" stands inside the
        // scene and looks along its longest horizontal axis, because that is the axis a hall, a nave
        // or a corridor runs down; a building framed from outside is a facade and nothing else, which
        // makes it useless as a global-illumination reference (there is no interior to bounce in).
        // See the log's "initial camera" line for the numbers this resolves to, and use
        // --capture-camera to pin an exact pose.
        std::string camera_fit = "exterior";
        // A PINNED initial camera pose: yaw (deg), pitch (deg), distance, target x, y, z - the same six
        // numbers `--capture-camera` takes and the same six the log prints (`camera pose: ...`), so a view
        // can be reproduced from a config, pasted between the two, or reported by a user. ABSENT by
        // default, which leaves `camera_fit` in charge; when present it wins over the fit and is itself
        // overridden by `--capture-camera`.
        std::array<float, 6> camera_pose = {};
        bool camera_pose_set = false;
        bool shadow = true; // record the directional shadow pass each frame
        // Cascaded shadow maps ([render] shadow_cascades / shadow_cascade_blend): how many cascades the
        // shadow pass fits, renders and samples (1 = one box over the whole visible range, the historic
        // single-map behavior) and the fraction of a cascade's range over which the shader blends into
        // the next one. 3 by default: the cheapest point where the near range stops paying for the far
        // range's texel size. Applied BEFORE the scene import - see runtime::set_shadow_cascades.
        int32_t shadow_cascades = 3;
        float shadow_cascade_blend = 0.1f;
        // THE TOON CHAIN'S GLOBAL SHADOW SOFTNESS ([render] toon_shadow_softness), a five-step ladder whose
        // MEMBERS ARE `(half_extent, spacing)` IN TEXELS: 0 = (1,1) = the shipped lookup, 1 = (2,2), 2 = (3,3),
        // 3 = (5,4), 4 = (8,3), so the tap count is `(2*half_extent + 1)^2` = 9 / 25 / 49 / 121 / 289. THE
        // WIDTH TO QUOTE IS THE TEXEL SPAN THE TAPS REACH, `2*half_extent*spacing + 2` indices first-to-last -
        // the counting the shipped `calc_shadow` note calls a "4x4 texel footprint" at level 0 - i.e. 4 / 10 /
        // 20 / 42 / 50 for levels 0..4 (the continuous covered width is one texel less). 0 = OFF and
        // BIT-IDENTICAL to the shipped run: the character chain's one shadow lookup is the same single 3x3
        // hardware PCF it always had. The value rides `deren::vulkan::toon_rig`'s ninth lane and
        // is read by the ONE shadow call site inside `toon_diffuse` in `shaders/character_forward.slang` -
        // a body that `shaders/goo_toon.slang` and `shaders/outline.slang` call as well, so those stages
        // carry the same code; `goo_toon.slang`'s OWN `calc_shadow` call (its `rs_shadow`) is untouched, and
        // at 0 the call site is the shipped `calc_shadow` itself. Float or integer; a non-integer is rounded,
        // anything outside 0..4 is clamped, a NaN is read as 0, and each case is logged separately (see
        // `analyse_config`). THE PER-LEVEL COST IS NOT REPEATED HERE ON PURPOSE: it has been re-measured twice
        // and this comment went stale both times. The authority is the `toon_shadow_softness` block in
        // `config.example.toml`, with the exact ROI and mask it used, measured on the welded asset
        // `chars/laevatain_goo.glb` = 63,835,728 B /
        // CE313E4D9515BC1887B4E0C783ABEA66A70FBFFF69ABD18992BDF33C8F5606F9.
        float toon_shadow_softness = 0.0f;
        // Shadow depth bias ([render] shadow_bias_constant / shadow_bias_slope): the rasterization bias the
        // shadow pass pushes a caster's depth by, which is what keeps a lit surface from shadowing itself
        // (acne). The defaults are the runtime's own (runtime::shadow_depth_bias_constant/_slope), so a config
        // that omits them behaves exactly as before they existed. `slope` scales with the surface's depth
        // gradient and is the one that usually matters; `constant` is a flat offset. The GUI panel exposes the
        // same two values as sliders.
        float shadow_bias_constant = 0.0f;
        float shadow_bias_slope = 1.5f;
        // A SCALE ON THE SUN'S RADIANCE ([render] sun_intensity). The shading already multiplies the sun by
        // this lane; without the key the only value reachable was the furnace mode's 0 or 1. 1.0 is the
        // shading path's own constant 7.5 unchanged; the gui exposes it as "sun intensity".
        float sun_intensity = 1.0f;
        // Clustered light culling ([render] clustered_lights, M5): the punctual lights are sorted
        // into a screen-tile x depth-slice grid once per frame and the shading stage loops only its
        // own cluster's list. false = the brute-force loop over every active light - the reference
        // path the clustered one is verified against (and what every pre-M5 frame did).
        bool clustered_lights = true;
        // Screen-space ambient occlusion (M6): the lighting stage traces a hemisphere of samples
        // against the G-buffer depth and scales the IBL ambient by the result. `ssao_radius` is in
        // world units (a fraction of the scene scale), `ssao_samples` is clamped to the shader
        // maximum of 16.
        // Shadow map edge length in texels ([render] shadow_map_size): 1024/2048/4096 are the usual
        // choices - resolution against the pass cost and memory (the layered map is
        // shadow_map_size^2 x 4 layers x 4 bytes per cascade set, per frame slot). Applied before the
        // scene import; the runtime clamps it to 256..8192 and rounds to a power of two.
        int32_t shadow_map_size = 2048;
        // Stochastic PUNCTUAL lighting (docs/megalights.md): sample a few of each pixel's clustered lights, trace
        // one visibility ray per sample and add the shadowed estimate where the raster loop would have added an
        // unshadowed one. OFF by default, and deliberately: the estimator is the first stage of a chain whose
        // denoiser is not built yet, so a stock config must not inherit its raw noise.
        bool megalights = false;
        // How many samples per HALF-RESOLUTION pixel (1..4, the shader's compile-time bound). This is the knob the
        // cost and the noise both scale with, and it is the one the overlay exposes next to the switch.
        int32_t megalights_samples = 4;
        // The chain's SPATIAL pre-filter width in half-resolution texels (0 = off, which is the temporal-only
        // chain). It is the "detail versus grain" dial: a filter that removes signal and noise at the same rate is
        // worse than none, so this one is configurable and its own measurement is in docs/megalights.md.
        float megalights_spatial_sigma = 1.5f;
        // The temporal resolve's history depth tolerance, RELATIVE to the pixel's view depth: the reject
        // threshold for "the history I reprojected belongs to this surface". UE's value is 0.03, widened at
        // grazing angles by 1/lerp(0.1, 1, N dot V) - see docs/reference/megalights_stochastic_lighting.md.
        // It is a knob because it is the balance between a stale history (ghosting) and a lost one (flicker at
        // every depth discontinuity TAA's jitter lands on the wrong side of), and the flicker measurement needs
        // to move it to attribute one to the other.
        float megalights_history_tolerance = 0.03f;
        // A scale on the ray origin's NORMAL OFFSET, which is the self-intersection guard for the visibility
        // rays (UE's mix(0.1, 0.01, N dot L) shape, in world units). 1 is the shipped pair; raising it is the
        // experiment that says whether a flickering terminator is the ray re-hitting its own surface.
        float megalights_bias = 1.0f;
        // The emitter's ANGULAR radius in radians: the soft-shadow knob, and this feature's cure for a flickering
        // hard shadow edge (see shaders/megalights_trace.slang's soft-shadow block). A point light's visibility is
        // binary, so a shadow boundary crossing the pixel flips it; an emitter with size makes the answer the
        // fraction of the emitter the pixel sees, which moves gradually. DEFAULT 0 - hard shadows, the behaviour
        // this feature shipped with - because the soft look is a choice, not a fix: measured, it does NOT reduce
        // the edge flicker this session chased (that flicker is the renderer's, and it survives every shadow
        // algorithm being off), so it stays an opt-in quality dial rather than a new default.
        float megalights_light_angle = 0.0f;
        // Ray-traced sun shadows ([render] rt_shadows): one ray per pixel against the scene's
        // acceleration structures instead of a sample of the cascaded shadow maps. Off by default, and
        // granted only on a device with ray queries - a device without them keeps running the cascaded
        // maps, which is what makes the key safe to leave in a shared config file.
        bool rt_shadows = false;
        // Bake alphaMode MASK into the acceleration structures ([render] rt_mask_bake): a compute pass
        // collapses the triangles a material's alpha covers nowhere. This is the SUBSTITUTE for an any-hit
        // stage and the shipped path no longer needs it: the shadow runs on a ray-tracing pipeline whose
        // any-hit shader cuts the mask per hit (shaders/rt_shadow.rahit). It stays because a knob is the only
        // way to measure the arms against each other. OFF BY DEFAULT, because the measurement says the
        // per-triangle rule is not good enough to be on: on a MASK-heavy sample asset it removes triangles
        // the raster path's own filtered sampling keeps, and the frame comes out 1.29 of mean brightness
        // BRIGHTER than the raster shadow it should match.
        bool rt_mask_bake = false;
        // Re-skin animated casters and refit their acceleration structures every frame ([render]
        // rt_skin_bake). The structures are built from the bind pose, so without this a ray-traced shadow of
        // an animated mesh is cast by the mesh where it is not. OFF BY DEFAULT: it is the A/B whose effect
        // the L2.2b measurement is about, and leaving it off keeps the traced shadow path byte-identical to
        // the frames every earlier measurement was taken with.
        bool rt_skin_bake = false;
        // The furnace verification mode ([render] furnace): the sun is turned off and the environment becomes
        // a constant level, so the correct frame is computable by hand - a diffuse surface's outgoing
        // radiance is exactly albedo * L, so anything added on top of it is double counting.
        // It is the one reference in this project that no estimator of its own can flatter, because it is not
        // an estimator. Off by default, and off is byte-exact.
        bool furnace = false;
        // Pin the keyframe animation at a time in seconds ([render] animation_time), or -1 to play it: a
        // NEGATIVE value is the default and plays as always. Playback is driven by the wall clock
        // (frame_clock::delta_seconds), so a capture of an animated scene is NOT reproducible - two runs of
        // one config differ, measured - and anything that has to compare two captures of one pose (the
        // skinned-mesh ray-tracing work is the first) needs this. It scrubs and pauses, which is what the
        // debug overlay's time slider does, so the pose is a function of the value alone.
        float animation_time = -1.0f;
        bool ssao = true;
        float ssao_radius = 0.5f;
        float ssao_intensity = 1.0f;
        int32_t ssao_samples = 8;
        // Render mode ([render] unlit): the "pbr (lit)" / "unlit (flat)" combo of the debug overlay
        // as a startup setting - the flat base-color reference, useful as a shading-free view. It
        // selects the runtime's default pipeline for the transparent pass, and the lighting stage is
        // told explicitly (set_unlit) because it binds its own pipeline and cannot follow a per-leaf
        // switch: it then outputs the stored albedo instead of shading it.
        bool unlit = false;
        bool fxaa = false; // FXAA the final image (one extra fullscreen pass)
        /**
         * THE RENDER SCALE ([render] render_scale): the fraction of the swapchain's extent the RENDER chain
         * runs at, so the scene is shaded at fewer pixels than are presented.
         *
         * 1.0 - the default - is the historic frame: every render target is the output's own size. Below
         * 1.0 the whole render chain shrinks together (`core::render_extent` is the one definition of the
         * frame's resolution, so a `full` pass, a half-size stochastic target and the bloom chain all follow
         * it), and the frame's last pass resolves those pixels back up to the output extent. That resolve is
         * what makes a scale below 1.0 an image rather than a smaller picture in the corner.
         *
         * A STARTUP value: it is read when the render targets are created, so it takes effect at the next
         * swapchain recreation rather than this frame.
         */
        float render_scale = 1.0f;
        /**
         * WHICH FILTER RESOLVES THE RENDER CHAIN ONTO THE OUTPUT ([render] upscale): "easu" (the default) or
         * "linear".
         *
         * It only means anything below `render_scale = 1.0`, because at 1.0 the render chain IS the output and
         * nothing resolves anything. `easu` is FSR 1's edge-adaptive spatial upsampling (AMD's, MIT - see
         * shaders/upscale.slang, which carries the license), and `linear` is the single bilinear tap that was
         * the resolve's first version. Linear is KEPT rather than replaced because it is the reference the
         * FSR filter is measured against: the claim "EASU is closer to a native-resolution frame" is a number
         * between these two modes, and a mode deleted after the fact takes its own baseline with it.
         */
        std::string upscale = "easu";
        // measure per-pass GPU time with timestamp queries: one vkCmdWriteTimestamp per pass
        // boundary, read back after the frame slot completed, averaged over a 60-frame window
        // (logged + shown in the debug overlay). A no-op on devices that cannot timestamp.
        bool gpu_timings = true;
        // G-buffer debug view ([render] gbuffer_debug / gbuffer_channel): draws the opaque scene
        // into the three G-buffer targets and shows the selected channel through the ordinary post
        // chain. A development view of the deferred path's data - the deferred lighting pass (M2)
        // takes over the display role and this stays as the inspection tool.
        bool gbuffer_debug = false;
        int32_t gbuffer_channel = 1; // 0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 material id, 6 depth, 7 flags, 8 motion
        // Temporal anti-aliasing ([render] taa / taa_blend_static / taa_blend_min): the deferred path's
        // anti-aliasing (a G-buffer cannot be multisampled, so there is no MSAA to fall back on). The projection is jittered every frame and a resolve pass blends the
        // reprojected, neighborhood-clamped history in - see runtime::set_taa. There is no per-object
        // motion yet: the G-buffer motion vectors are camera-only at this milestone.
        bool taa = false;
        float taa_blend_static = 0.9f; // history weight for a pixel that did not move
        float taa_blend_min = 0.5f;    // history weight floor under motion (lower = less ghosting)
        // The TOON CHARACTER STAGE ([render] character_forward): a pass that re-shades the scene's OPAQUE
        // leaves OVER the lit frame, at depth-EQUAL, so a character can have its own hand-authored shading
        // instead of the deferred one - and without being lit twice, which is what happens if a finished
        // colour is fed back through the G-buffer. See vulkan.pass.character_forward.
        //
        // OFF BY DEFAULT, and that default is the feature's contract rather than caution: with it ON every
        // opaque surface in the scene is drawn by the character pipeline, which is what a character viewer
        // wants and what a scenario comparing against the pre-existing references must not have.
        bool character_forward = false;
        // WHICH TOON CHAIN THAT STAGE DRAWS WITH ([render] goo_toon): false = the chain written against the
        // author's article (`character_forward.slang`), true = the rewritten one whose authority is XIYAG's Goo
        // node presets (`goo_toon.slang`, see deren::vulkan::runtime::goo_toon_pipeline_name). NOT a second way to turn
        // the stage on: with `character_forward` off this selects nothing, which is what keeps "the new shading
        // model" and "the character stage exists" separable - the rewrite is verified by holding one and moving
        // the other.
        //
        // OFF BY DEFAULT on the same terms as the flag above, and for a sharper reason: the rewrite lands one
        // reference group at a time, so a run that does not ask for it must get the frame the old chain produces.
        bool goo_toon = false;
        /**
         * THE FRAME'S STATIC SURROUND AS A SECOND MODEL ([render] background_glb): the path of a glTF/GLB
         * whose geometry is the environment rather than the subject. Empty by default and that default is the
         * feature's contract: with no path the frame is exactly the frame this renderer produced before the
         * key existed (one import, one leaf set), which is what the render gate's recorded references are.
         *
         * THE PATH IS RESOLVED LIKE THE MODEL'S: a relative path is taken relative to the executable's own
         * directory (see `deren::utility::executable_directory()`), which is where the assets live, so the shipped
         * config can name `chars/...` or a path under the build directory without being absolute.
         *
         * WHAT IT DOES, IN ONE SENTENCE: the same importer (`runtime::import_scene`) runs a second time into
         * the same scene tree with `environment = true`, which flags every leaf it creates
         * (`primitive::environment`) so the toon character stage skips it and it never casts a shadow - while
         * the scene pass draws it through the ordinary PBR/unlit path, where a matte ground and a glTF
         * emissive backdrop belong. Nothing else about the frame changes.
         *
         * THE OFFSET IS DERIVED, NOT CONFIGURED: the imported roots are translated so the background's own
         * y = 0 plane (the author's ground) lands on the SUBJECT's lowest point after the subject's own
         * centering shift - see the import in `main.cpp`. A background whose ground is not at y = 0 would need
         * its own shift, which is why this is stated here rather than hidden in the loader.
         */
        std::string background_glb = {};
        bool validation_layers = default_validation_layers; // Vulkan validation layers + debug messenger ([render])
    };

    /**
     * @ingroup app_config
     * @brief image-based-lighting precompute resolutions ([lighting] in the config)
     */
    export struct lighting_settings {
        int32_t env_size = 256;    // base environment cubemap size
        int32_t env_mip_count = 5; // prefiltered-environment mip chain length
        int32_t irr_size = 32;     // irradiance cubemap size
        int32_t lut_size = 256;    // BRDF LUT size
        // sun_direction ([lighting] sun_direction): the sun's direction in world space, pointing FROM the
        // surface TOWARD the sun, unnormalized (it is normalized where it is used). ONE setting feeds three
        // consumers that must agree or the frame contradicts itself - the shadow cascades and the shading's
        // `light_dir`, the visible disc the sky draws, and the environment cubemap's baked sun.
        // THE DEFAULT REPRODUCES THE HISTORIC HARD-CODED VECTOR exactly, so a config without this key renders
        // the frame it always did; it is a high sun (59 degrees of elevation, 31 of azimuth) and lowering the
        // elevation is what moves the shadows off a face and onto the ground behind it.
        std::array<float, 3> sun_direction = {0.3f, 1.0f, 0.5f};
        /**
         * THE ENVIRONMENT IMAGE, i.e. the reference package's own way of describing its world.
         *
         * `environment_hdr`: path to an EQUIRECTANGULAR (lat-long) HDR image - in practice the reference
         * package's `lighting/studio_01_1k.exr`, converted once, offline, into the one float format the
         * engine's own decoder reads (Radiance `.hdr`, which is what `stb_image` handles; the conversion
         * and its measurements are in `deren-ab/bg/_hdr_convert.py`). It REPLACES the procedural sky above as
         * the environment cubemap.
         *
         * EMPTY (THE DEFAULT) IS THE CONTRACT: with no image every path renders exactly what it rendered
         * before this key existed, byte for byte. Relative paths resolve against the executable's
         * directory, the same rule [render] background_glb follows.
         *
         * WHEN IT IS SET the procedural gradient AND its baked sun disc are both gone from the IBL - the
         * image is the whole environment. The DIRECT light is untouched: `sun_direction` still drives the
         * shadow cascades, the shading and the visible disc the sky draws, so the image is never asked to
         * double as a sun. (The visible sky itself is still analytic; with the reference's backdrop dome
         * present - see [render] background_glb - it is occluded anyway.)
         */
        std::string environment_hdr = {};
        /**
         * `environment_intensity`: a linear multiplier applied to every texel of that image - the same
         * degree of freedom the reference package calls its world's `world_strength`.
         *
         * THE DEFAULT IS THE REFERENCE'S OWN NUMBER (0.35, from that package's manifest.json): pointing
         * `environment_hdr` at its image and changing nothing else is meant to reproduce the reference's
         * ambience. 1.0 is the file's raw radiance, which for a studio HDRI is a good deal brighter.
         */
        float environment_intensity = 0.35f;
        /**
         * THE REFERENCE PACKAGE'S AREA LIGHT - a 30 m x 30 m square soft box, 4000 W, which that package's
         * `manifest.json` (`deren-ab/bg/endfield-background/manifest.json`) names as the main light of the shot:
         *
         *     UsdLuxRectLight, size_m 30, power_w 4000, position_blender_m [-3, -4, 15], aim_at [0, 0, 0]
         *
         * Blender is Z-up and this engine is Y-up, so that position is `[-3, 15, 4]` here: 15.8 m away at ~72
         * degrees of elevation. A source that big and that close to overhead is what makes the shadows it
         * casts soft, and it is what fills the skirt's shadow in the author's preview - the thing OUR frame
         * has been missing (see PROGRESS.md 3.5's honest boundary).
         *
         * IT IS THE ENGINE'S MAIN LIGHT, NOT A SECOND LIGHT. The engine has exactly one directional light -
         * `sun_direction` drives the shading's `light_dir`, the shadow cascades, the visible disc and the
         * environment bake's sun - and the area light is expressed THROUGH it: the emitter's direction
         * becomes that light's direction and its radiance multiplies `[render] sun_intensity`. The shadow
         * pass itself is untouched. The two appended `light_ubo` lanes carry the emitter's geometry so the
         * shading can widen the shadow lookup into a penumbra.
         *
         * V1.1 (2026-10-02, the A' ruling): the polygon-Lambert "size correction" that used to scale the direct
         * term by the emitter's SIZE is DELETED - measured, it drove the frame's main light to zero. What the
         * keys mean now is "the emitter IS the main light": while `area_light_irradiance` is true the host aims
         * `set_sun_direction` at the emitter and multiplies `set_sun_intensity` by its radiance; with it false
         * the emitter contributes ONLY the penumbra and the sun keeps its own direction and intensity.
         * Consequently the `area_light` lane is UNREAD by the v1.1 shaders (only `area_light_axis.w` still is -
         * see `calc_shadow_area`); it is kept because the pinned wire layout must not move and because a v2
         * area integral needs centre/half.
         *
         * POSITION AND TARGET ARE RELATIVE TO THE AUTHOR'S ORIGIN - THE CHARACTER'S FEET ON THE GROUND - AND NOT
         * TO THIS ENGINE'S `scene_center`. The manifest's numbers are the reference package's "the character
         * stands at the origin" frame, and in Blender that origin is on the ground under the character, so
         * `[-3, -4, 15]` means 3 m to one side, 4 m behind and 15 m up. This engine imports the model with
         * `scene_import_shift`, which puts the character's feet at `y = bounds.min.y - scene_center.y -
         * scene_radius` (world XZ = 0) - measured as -2.198 m for this scene, and cross-checked against the
         * background package's own import offset, whose ground lands on the same number. THAT point is the
         * `scene_origin` the caller passes in, so `world = scene_origin + position` is what lets the manifest's
         * numbers be copied verbatim.
         *
         * ANCHORING ON `scene_center` INSTEAD IS WRONG, and was the first version of this code: the two points
         * are 0.85 m of height apart here, which moves the visible emitter's elevation by ~6 degrees and its
         * azimuth by ~16 degrees (and the soft box's distance from 15.811388 m to 17.37 m). The direction the sun
         * comes FROM is `position` either way; what moves is WHERE the 30 m emitter sits in the world, and the
         * penumbra's heuristic is built from that position (measured on to the target - see
         * `derive_area_light`).
         *
         * SIZE 0 (THE DEFAULT) IS THE CONTRACT: no emitter, both lanes written as a literal zero vec4, and a
         * frame renders exactly what it rendered before these keys existed - byte for byte.
         *
         * `area_light_intensity` is a multiplier on the emitter's radiance, so the effective radiance is
         * `intensity * power / (pi * size^2)`; 4000 W over a 30 m side reads 1.41471. IT CANNOT EXCEED 3.0:
         * the value this multiplies is `[render] sun_intensity`, and `runtime::set_sun_intensity` clamps it
         * to 0..3 (`vulkan/runtime/runtime.cpp`) - so an `area_light_intensity` above `3 / 1.41471` = ~2.12
         * is clipped by that clamp. That is arithmetic in the runtime, not something this module can warn
         * about; it is written down here so the number is never a surprise. As of v1.1 this product reaches the
         * sun only while `area_light_irradiance` is true - with the emitter in penumbra-only mode it is still
         * derived and logged, but the sun keeps its own intensity.
         *
         * `derive_area_light` below is the ONE place these keys become the frame's values.
         */
        float area_light_size = 0.0f;                                  // side of the square emitter, metres (0 = NO area light)
        float area_light_power = 0.0f;                                 // watts (the manifest's Blender unit)
        std::array<float, 3> area_light_position = {0.0f, 0.0f, 0.0f}; // emitter centre, RELATIVE TO THE AUTHOR ORIGIN (the feet)
        std::array<float, 3> area_light_target = {0.0f, 0.0f, 0.0f};   // aim point, same frame as position
        float area_light_intensity = 1.0f;                             // multiplier on the emitter's radiance (>= 0)
        bool area_light_irradiance = true;                             // true = the emitter TAKES OVER the main light (its direction, its radiance)
        bool area_light_shadow = true;                                 // the area light's own (soft) visibility
        float area_light_softness = 0.0f;                              // penumbra world radius, metres; 0 = automatic
        // demo_lights ([lighting] demo_lights): spawn this many procedural punctual lights around
        // the scene (a helix at the scene bounds, cycling colors). This is the clustered-light stress
        // mode: with the debug overlay's four light slots the cluster lists and the brute-force loop
        // visit the same handful of lights, so the win is invisible. 0 (default) = overlay lights
        // only; the generated ones are pushed every frame together with the overlay's slots.
        // Capped at max_demo_lights: the light UBO holds max_punctual_lights lights in total, and the
        // overlay's own slots share that array.
        int32_t demo_lights = 0;
        /**
         * WHERE the generated lights sit and how far they reach, both as FRACTIONS of the scene radius.
         *
         * They default to the helix the clustered-light stress mode has always used (0.85 of the scene radius
         * out, 0.55 of it as the range), so a config that does not set them renders exactly what it did
         * before. They are configurable because the two USES of this set pull in opposite directions: the
         * cluster stress wants lights AROUND the scene, where a light count the brute-force loop could not
         * afford is what shows, while measuring what a punctual light does to a frame (its shadows, and the
         * noise of a stochastic estimate of it - see docs/megalights.md) needs lights INSIDE the view, close
         * enough to matter. A radius near 0 puts them at the scene centre with a range that reaches the
         * camera's neighbourhood.
         */
        float demo_light_radius = 0.85f; // helix radius, as a fraction of the scene radius
        float demo_light_range = 0.55f;  // the lights' range, as the same fraction (0 = no cutoff)
    };

    /** @brief upper bound for [lighting] demo_lights (the UBO's light array is deren::vulkan::max_punctual_lights) */
    export constexpr uint32_t max_demo_lights = 64;

    /**
     * @ingroup app_config
     * @brief the area light's per-frame values, derived once per frame from `[lighting] area_light_*`
     *
     * Everything here is in the ENGINE's frame (Y-up, metres, world space). `enabled == false` means "no
     * emitter at all": every field is zero, the caller writes a literal zero `light_ubo` pair and the frame
     * keeps exactly the sun `[render] sun_intensity` and `[lighting] sun_direction` describe.
     *
     * `std::array<float, 3>` rather than `glm::vec3` on purpose: this module does not depend on glm (see the
     * header block at the top of this file), and the one consumer, `main.cpp`, converts at its call site.
     */
    export struct area_light_derived {
        bool enabled = false;                                   // false = no emitter: every field below is zero
        std::array<float, 3> world_centre = {0.0f, 0.0f, 0.0f}; // the emitter's centre, world space
        float half = 0.0f;                                      // half the emitter's side, metres
        std::array<float, 3> axis = {0.0f, 0.0f, 0.0f};         // emitter normal (centre -> target), unit
        std::array<float, 3> to_light_dir = {0.0f, 0.0f, 0.0f}; // author origin -> emitter, unit (the new sun direction)
        float radiance = 0.0f;                                  // power / (pi * size^2): the emitter's Lambertian radiance
        float penumbra = 0.0f;                                  // shadow penumbra radius, world metres (0 = no area-light shadow)
    };

    /**
     * @ingroup app_config
     * @brief turn `[lighting] area_light_*` into the frame's area light - PURE: no device, no globals, no I/O
     *
     * @param lighting      the parsed `[lighting]` table
     * @param scene_origin  the AUTHOR'S origin: the character's feet on the ground (world XZ = 0), i.e. where the
     *                      reference package's `manifest.json` measures its light from. `area_light_position` and
     *                      `area_light_target` are relative to it, so the manifest's "character at the origin"
     *                      numbers can be copied into a config verbatim. This is NOT `scene_center`: the
     *                      bounding-box centre sits half a body above the feet, and this engine's own
     *                      `scene_import_shift` puts the feet at `bounds.min.y - scene_center.y - scene_radius`
     *                      (see the key documentation above for the measured value and the cross-check)
     *
     * The frozen derivation - `shaders/character_forward.slang` implements the other half of this contract, so
     * changing a formula here without changing it there (or the other way around) is a bug, not a tuning:
     *   half         = 0.5 * size                                (size <= 0 -> disabled)
     *   world_centre = scene_origin + position
     *   world_target = scene_origin + target
     *   axis         = normalize(world_target - world_centre) (degenerate -> disabled)
     *   to_light_dir = normalize(world_centre - scene_origin) (degenerate -> disabled)
     *   radiance     = intensity * power / (pi * size^2)
     *   penumbra     = shadow ? (softness > 0 ? softness : 0.05 * size / max(|position|, 1e-3)) : 0
     *
     * 4000 W over a 30 m side reads radiance == 1.41471, i.e. the reference package's soft key.
     */
    export area_light_derived derive_area_light(lighting_settings const& lighting, std::array<float, 3> const& scene_origin);

    /**
     * @ingroup app_config
     * @brief debug-overlay panel settings ([gui] in the config)
     */
    export struct gui_settings {
        bool show = true;            // show the Dear ImGui debug overlay by default
        float panel_width = 380.0f;  // default debug-panel width (0 = ImGui auto-size)
        float panel_height = 140.0f; // default debug-panel height (0 = ImGui auto-size)
    };

    /**
     * @ingroup app_config
     * @brief the TOON stage's LIGHT RIG ([toon] in the config), which is `deren::vulkan::toon_rig`'s content
     *
     * THESE ARE THE ARTICLE'S GLOBAL NUMBERS - the ones an artist tunes once for a whole character rather than per
     * material: the split between the sun and the head light the game rigs over every figure, what each of them
     * looks like on a surface's shadow side, and the strengths of the two environment terms. A FAMILY's numbers are
     * NOT here (they are `shaders/toon_params.slang`, because two stages must agree about them) and a MATERIAL's
     * come from the sidecar beside the model.
     *
     * THE DEFAULTS ARE THE ARTICLE'S OWN VALUES, so a config that says nothing renders what this port was measured
     * at - and `day_strength` is the one that moves a frame most: 1 is the sun leading with the head light
     * supplementing it, 0 removes the sun's DIRECT term entirely (no terminator, no light direction) and lets the
     * head light and the environment carry the character. Intermediate values blend two whole shading outcomes
     * rather than dimming one, because a dimmed sun still draws its own terminator.
     */
    export struct toon_settings {
        float day_strength = 1.0f;                                      // `_DayStrength` (0..1)
        float head_light_day0 = 0.7f;                                   // `_OtherLightResultStrength_day0` - the author's default
        float head_light_day1 = 0.3f;                                   // `_OtherLightResultStrength_day1` - the author's default
        std::array<float, 3> head_light_colour = {0.60f, 0.65f, 0.80f}; // `_OtherLightColor`
        std::array<float, 3> sun_dark_colour = {0.60f, 0.65f, 0.80f};   // `_MainLightColor_dark`
        float env_strength = 1.0f;                                      // `_EnvLightStrength`
        float env_rotation = 0.0f;                                      // `_EnvRotation`, in DEGREES
        float specular_strength = 1.0f;                                 // `_SpecularStrength`
        float diffuse_blend_effect = 1.0f;                              // `_DiffuseBlendEffect`
        float rim_area = 1.0f;                                          // `_RimLightArea` - the author's default
        float rim_strength = 1.0f;                                      // `_RimLightStrength`
        float rim_nolxz_strength = 1.0f;                                // `_RimLightNoLxzStrength`
        float backlight_strength = 1.0f;                                // the backlight compensation's weight
        // WHICH EXPRESSION THE FACE IS WEARING (`_EmotionMap`'s cell): 0 = none, so the face keeps its own albedo,
        // which is what every capture before this key existed was taken at; 1..3 = the atlas' other three cells.
        // IT IS A CONFIG KEY RATHER THAN A MATERIAL VALUE because the material files have none - the game picks the
        // expression from the character's animation/script system, and nothing in the sidecar describes it.
        float emotion_type = 0.0f;
    };

    /**
     * @ingroup app_config
     * @brief the resolved startup settings after config-file + argv merging.
     * @note empty string / zero fields mean "not specified": the caller falls back to its
     *       built-in defaults, mirroring the pre-config argv behavior.
     */
    export struct app_settings {
        // model file (empty = locate default via paths.model_dir; "ask" opens the platform's own file
        // dialog at startup - see wants_model_dialog, which is the only thing that may interpret it)
        std::string model = {};
        int32_t grid_side = 0; // > 1 enables the instancing stress grid
        path_settings paths = {};
        render_settings render = {};
        lighting_settings lighting = {};
        toon_settings toon = {};
        gui_settings gui = {};
        std::string config_file = {}; // path actually read (empty = no config file found / used)
    };

    /**
     * @ingroup app_config
     * @brief the `model` value that means "ask me with the platform's file dialog at startup"
     */
    export inline constexpr std::string_view model_ask = "ask";

    /**
     * @ingroup app_config
     * @brief whether @p settings asks for the model dialog instead of naming a file
     * @return true when `model` is exactly the @ref model_ask sentinel
     *
     * A function rather than a field, so that exactly ONE place decides what the sentinel is: a caller
     * must never write `settings.model == "ask"` itself, because the day the sentinel changes, that copy
     * opens a file called "ask" instead of asking. The comparison is exact and case-sensitive on purpose -
     * a model may legitimately be named ASK, and a near miss should be treated as the path it looks like
     * rather than as an instruction.
     */
    export bool wants_model_dialog(app_settings const& settings);

    /**
     * @ingroup app_config
     * @brief parse @p path as TOML into the settings it specifies; missing keys keep defaults.
     * @return app_settings with config_file = @p path on success; a partially filled structure
     *         with empty config_file on a read/parse error (the error is logged)
     */
    export app_settings load_settings(std::string const& path);

    /**
     * @ingroup app_config
     * @brief resolve the effective startup settings from argv: a `--config <path>` argument picks
     *        the config file (default: "config.toml" in the working directory if present), then
     *        positional argv values (with `--config <path>` consumed as an option) override the
     *        file: positional[0] = model path, positional[1] = grid side (numeric)
     * @return the merged settings (see app_settings notes for the "not specified" semantics)
     */
    export app_settings resolve_from_argv(int32_t argc, char const* const* argv);

    /**
     * @ingroup app_config
     * @brief like resolve_from_argv() but with an explicit default config path when no --config
     *        argument is present (used when the caller does not want the cwd-relative default)
     */
    export app_settings resolve_from_argv(int32_t argc, char const* const* argv, std::string const& default_config_path);
} // namespace deren::app_config