module;

#include <glm/glm.hpp>

export module deren.chores;

export import deren.vstd;
import deren.application_configuration;
import deren.utility;
import deren.vulkan.animation; // setup_gui builds the animation playback controls
import deren.vulkan.runtime;

/**
 * @file chores.cppm
 * @defgroup chores Demo Bootstrap Chores
 * @brief main()'s startup helper functions, kept out of main.cpp so the entry point reads
 *        first: resolving the startup config (config file + argv merge, shaders/ and
 *        default-model dirs), creating the demo pipelines, loading shader SPIR-V files, the
 *        optional instancing stress grid, and assembling the demo's Dear ImGui debug overlay
 *        (setup_gui). Demo/application layer only - these helpers know about app_config
 *        (settings), deren.vulkan.runtime and the utility log/panic, never about glTF (the
 *        loader's own diagnostics live in gltf_loader).
 * @note interface only - the implementations live in chores.cpp (the module follows the
 *       export import std pattern of the other split modules)
 */
namespace deren::chores {
    /**
     * @ingroup chores
     * @brief the resolved startup environment: merged settings + located shaders dir + model path
     * @note everything main needs before it touches the runtime; produced by analyse_config()
     */
    export struct startup_config {
        deren::app_config::app_settings settings = {}; // merged config-file + argv settings
        std::filesystem::path shaders_dir = {}; // shader SPIR-V directory (configured or located)
        std::string model_path = {};            // glTF file to load (configured, model_dir, or located)
    };

    /**
     * @ingroup chores
     * @brief resolve the startup config in one step: merge the config file + argv into the
     *        app settings, then locate the shaders/ directory and pick the model file
     *        (configured path, model_dir default, or auto-located gltf_model/). Panics when a
     *        configured or located resource is missing.
     * @param argc argv argument count (as received by main)
     * @param argv argv argument vector (as received by main)
     */
    export startup_config analyse_config(int32_t argc, char** argv);

    /**
     * @ingroup chores
     * @brief read one shader SPIR-V file into @p out; panic on failure (prints the path)
     */
    export void load_shader(std::filesystem::path const& dir, std::string_view const file_name, std::vector<uint8_t>& out);

    /**
     * @ingroup chores
     * @brief walk up from the working directory to find the shaders/ directory (works from the
     *        project root or a cmake-build-* directory); nullopt when not found within 4 levels
     */
    export std::optional<std::filesystem::path> locate_shaders_dir();

    /**
     * @ingroup chores
     * @brief walk up from the working directory to find the default model under gltf_model/
     *        (gltf_model/DamagedHelmet.gltf); nullopt when not found within 4 levels
     */
    export std::optional<std::filesystem::path> locate_model_file();

    /**
     * @ingroup chores
     * @brief load a vertex/fragment SPIR-V pair and create the pipeline via the runtime; panic
     *        on load or creation failure
     */
    export void load_and_create_pipeline(deren::vulkan::runtime& runtime,
                                         std::filesystem::path const& shaders_dir,
                                         std::string_view const pipeline_name,
                                         std::string_view const vertex_file,
                                         std::string_view const fragment_file);

    /**
     * @ingroup chores
     * @brief create the demo pipelines up front: the standard PBR pipeline plus the skybox
     *        background (fullscreen environment pass) and the directional shadow-map pass
     *        (depth-only). Panics when PBR/skybox creation fails; a shadow failure only logs
     *        "shadow pipeline disabled" (the scene still renders without shadows).
     * @note the old triangle demo pipeline is deliberately NOT created here: nothing draws it
     *       anymore (the skybox background and the imported scene cover the screen)
     */
    export void setup_pipeline(deren::vulkan::runtime& runtime, std::filesystem::path const& shaders_dir);

    /**
     * @ingroup chores
     * @brief optional instancing stress: when @p grid_side > 1 (config or argv), draw the first
     *        imported "pbr" primitive as a grid_side x grid_side grid in ONE instanced draw call
     *        (an instanced_draw_primitive appended to the scene tree — the frame loop is
     *        untouched). No-op when grid_side <= 1 or the scene has no pbr primitive.
     * @param runtime the initialized runtime holding the imported scene
     * @param grid_side grid side length from settings.grid_side (> 1 enables the grid)
     * @param scene_radius radius of the imported scene (grid spacing = 2.5 x radius, so
     *        instances stay apart: the demo measures draw scaling, not overdraw)
     */
    export void add_instancing_grid(deren::vulkan::runtime& runtime, int32_t grid_side, float scene_radius);

    /**
     * @ingroup chores
     * @brief live state the debug-gui widgets bind to. Owned by main (the frame loop keeps the
     *        fps text and the animation mirrors in sync each frame); setup_gui() wires the
     *        widgets to these fields.
     * @note defaults mirror the historic demo values; main overrides the ones that come from
     *       config ([render] shadow toggles) or runtime state (animation playing)
     */
    export struct gui_bindings {
        double fps = 0.0;                  // fps text (updated once per second when use_gui)
        bool cull_enabled = true;          // frustum-culling checkbox (write-through to the runtime)
        bool shadow_enabled = true;        // shadow checkbox (initial: settings.render.shadow)
        float shadow_bias_constant = 0.0f; // shadow depth-bias sliders (constant factor)
        float shadow_bias_slope = 1.5f;    // shadow depth-bias sliders (slope factor)
        int32_t shadow_cascades = 3;           // cascade-count combo (1..4; index 0 = single map)
        float shadow_cascade_blend = 0.1f; // cascade blend-band slider (fraction of the range)
        float anim_time = 0.0f;            // animation time slider (mirror of the controller clock)
        bool anim_playing = true;          // play/pause checkbox (mirror of controller state)
        int32_t anim_index = 0;                // animation combo selection (0 = the auto-played one)
        int32_t current_camera = 0;            // camera combo selection (0 = orbit, 1..N = authored)
        int32_t render_mode = 0;               // render-mode combo (0 = pbr, 1 = unlit); main applies it
                                           // between frames via set_default_pipeline
        int32_t brdf_model = 0;                // brdf-model combo (0 = GGX+joint, 1 = GGX+height-corr,
                                           // 2 = Beckmann, 3 = Blinn-Phong); write-through to runtime
        int32_t diffuse_model = 0;             // diffuse combo (0 = Lambert, 1 = Oren-Nayar)
        float exposure = 1.0f;             // linear exposure slider (runtime::set_exposure)
        // bloom on/off (M9): the checkbox gates the whole chain; the intensity slider keeps its value
        // while it is off, so toggling back restores the previous look. main() mirrors it by pushing an
        // intensity of 0 when the box is clear, which is what runtime::active_features() gates the
        // bloom passes on.
        bool bloom_enabled = true;
        float bloom_intensity = 0.8f;      // bloom blend weight slider (runtime::set_bloom; 0 = off)
        int32_t toon_bands_index = 0;          // cel-shading combo: index into toon_band_counts (0 = plain PBR)
        float sun_intensity = 1.0f;        // a scale on the sun's radiance (0..3; 1.0 = unchanged)
        float toon_softness = 0.15f;       // cel-shading band edge softness slider (smaller = harder edges)
        float bloom_threshold = 0.35f;     // bloom bright-pass threshold (visible range 0..0.75)
        // FXAA (runtime::set_fxaa): checkbox + the two shader knobs. The checkbox is mirrored by
        // main into the runtime every frame like the other post-process values.
        bool fxaa_enabled = false;         // FXAA on/off (initial: settings.render.fxaa)
        float fxaa_subpixel = 0.75f;       // sub-pixel term strength (0 = pure directional blend)
        float fxaa_edge_threshold = 0.166f; // relative luma contrast below which a pixel is "flat"
        // G-buffer debug view (runtime::set_gbuffer_debug / set_gbuffer_channel): the deferred
        // path's stored surface, one channel at a time. Mirrored into the runtime every frame like
        // the FXAA state, so these fields carry the config's initial values.
        bool gbuffer_debug = false; // draw the G-buffer + its debug view instead of the shaded scene
        int32_t gbuffer_channel = 1;    // 0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 id, 6 depth, 7 flags, 8 motion
        // TAA (runtime::set_taa): the engine's anti-aliasing. Mirrored into the runtime every
        // frame like the other render toggles; the blend weights are the two shader knobs.
        bool taa_enabled = false;
        float taa_blend_static = 0.9f; // history weight for a static pixel (0.9 = 10% of the new frame)
        float taa_blend_min = 0.5f;    // history weight floor under motion (lower = less ghosting)
        // The TOON CHARACTER STAGE (runtime::set_character_forward): re-shades the scene's opaque leaves OVER
        // the lit frame so a character can carry its own shading. Mirrored into the runtime every frame like
        // the other render toggles, and the overlay only offers it when the renderer registered the pipeline
        // (see render_start_demo::feature_available).
        bool character_forward = false;
        // WHICH SHADING MODEL THAT STAGE DRAWS WITH (runtime::set_goo_toon): false = the old chain
        // (`character_forward.slang`), true = the rewritten one (`goo_toon.slang`, see
        // `runtime::goo_toon_pipeline_name`). A SECOND SWITCH RATHER THAN ONE, because the two answer different
        // questions - whether the stage runs, and which model it runs - and the rewrite is verified by holding the
        // first fixed and moving the second. Mirrored into the runtime every frame like the other render toggles.
        bool goo_toon = false;
        // Stochastic PUNCTUAL lighting (docs/megalights.md): the switch and the estimator's sample count. The
        // switch is the A/B a user actually wants - the shadows the punctual lights never had, against the
        // unshadowed path - and the sample count is the one knob cost and noise both scale with.
        bool megalights_enabled = false;
        float megalights_samples = 4.0f;
        // The chain's spatial pre-filter width in half-resolution texels: the dial between grain and detail,
        // and 0 makes the chain temporal-only. A slider rather than a constant, because a filter that removes
        // signal and noise at the same rate is worse than none - so it is moved and measured.
        float megalights_spatial_sigma = 1.5f;
        // How many frames the running mean may average: 1 turns the ACCUMULATION off (the resolve writes this
        // frame's estimate straight through), 12 is the shipped policy. It exists because the comparison a user
        // actually wants is "what does each half of the denoise do", and both halves are now one slider each.
        float megalights_frames = 12.0f;
        // The history's relative depth tolerance: the dial between ghosting (too loose) and losing the
        // accumulation at every depth edge TAA's jitter reprojects onto the wrong side of (too tight).
        float megalights_history_tolerance = 0.03f;
        // The visibility ray's normal-offset scale: the self-intersection guard, and the dial between
        // "shadow acne / a flickering terminator" (too small) and "a shadow detached from its caster" (too big).
        float megalights_bias = 1.0f;
        // The emitter's angular radius (radians): the soft-shadow dial, 0 = hard. It is what moves a flickering
        // terminator, so it is a slider rather than a constant.
        float megalights_light_angle = 0.0f;
        // demo punctual lights (count matches deren::vulkan::max_punctual_lights): the gui rows below
        // edit these fields live (no per-widget callbacks), and main() pushes the enabled set to
        // the runtime once per frame via deren::chores::apply_point_lights(). Each slot is a point light
        // or - with `spot` set - a cone light (direction + inner/outer half-angles in degrees;
        // apply_point_lights clamps inner <= outer < 90 and converts to the shader's cosines).
        // Plain C arrays keep this interface glm-free.
        struct light_slot {
            bool enabled = false;
            bool spot = false;                        // false = point light (omni), true = spot cone
            float position[3] = {0.0f, 0.0f, 0.0f};   // world position
            float direction[3] = {0.0f, -1.0f, 0.0f}; // spot axis (spot only; normalized when pushed)
            float color[3] = {1.0f, 1.0f, 1.0f};      // linear color
            float intensity = 1.0f;
            float range = 10.0f;          // 0 = infinite falloff
            float inner_cone_deg = 20.0f; // spot: soft inner half-angle (degrees)
            float outer_cone_deg = 30.0f; // spot: hard cutoff half-angle (degrees)
        };
        light_slot point_lights[4] = {};
        // Which slot the panel's punctual-light group is editing (0-based, clamped by the panel). The group
        // draws ONE slot at a time - four slots of nine controls each was thirty-six rows of panel for a
        // feature most frames leave off - so this index is what the "punctual light" combo writes and every
        // widget of the group tests.
        int32_t active_light = 0;
        // clustered light culling (M5): checkbox mirrored into the runtime every frame
        // (runtime::set_clustered_lights); false = the brute-force loop over every light
        bool clustered_lights = true;
        // screen-space ambient occlusion (M6, deferred path): mirrored into the runtime every frame
        bool ssao_enabled = true;
        float ssao_radius = 0.5f;
        float ssao_intensity = 1.0f;
        float ssao_samples = 8.0f; // slider value; main rounds it into the runtime call
    };

    /**
     * @ingroup chores
     * @brief build the demo's Dear ImGui debug overlay when @p use_gui: enable it on the
     *        runtime and assemble the "deren debug" panel — fps label, frustum-culling
     *        / skybox / shadow toggles, the camera-target drag, animation playback controls
     *        (label + play + time scrubber + animation dropdown when the controller has an
     *        active animation), the camera selector (when @p camera_names is non-empty), the
     *        shadow depth-bias sliders and the punctual light rows (per-slot enable / position /
     *        color / intensity / range plus spot cone editing: spot toggle, direction, inner and
     *        outer half-angles).
     * @param runtime the initialized runtime (enable_debug_gui() is called here)
     * @param use_gui whether the overlay is wanted ([gui] show); no-op when false
     * @param settings startup settings: [gui] panel size + [render] initial skybox/shadow states
     * @param bindings live widget state (see gui_bindings); the frame loop updates fps and the
     *        animation mirrors each frame
     * @param animation the animation controller the playback widgets drive (may be idle)
     * @param camera_names display names of the scene's authored cameras (no "orbit" entry is
     *        added here — setup_gui prepends it); empty disables the camera selector
     * @param on_camera_selected called with the combo index (0 = orbit, i = camera_names[i-1])
     *        when the user picks a camera; empty when the selector is not shown
     * @note the overlay's glTF-side data (authored camera list, orbit seeding) stays in main:
     *       chores only ever sees display names and a callback, never glTF types
     */
    export void setup_gui(deren::vulkan::runtime& runtime,
                          bool use_gui,
                          deren::app_config::app_settings const& settings,
                          gui_bindings& bindings,
                          deren::vulkan::animation::controller& animation,
                          std::vector<std::string> const& camera_names,
                          std::function<void(int32_t)> const& on_camera_selected);

    /**
     * @ingroup chores
     * @brief push the enabled punctual-light slots of @p bindings into the runtime's light UBO.
     *        Called once per frame from main (while the gui is active): the gui widgets edit
     *        bindings.point_lights live, so a drag/toggle becomes visible next frame without
     *        per-widget callbacks. Each slot is pushed as a point light, or as a spot light when
     *        its `spot` flag is set (direction + clamped inner/outer cone angles). Cheap no-op
     *        when nothing is enabled.
     * @param runtime the runtime whose light UBO receives the slots
     * @param bindings the overlay's live light widgets (see gui_bindings)
     * @param extra additional lights appended after the overlay's slots - the [lighting] demo_lights
     *        stress set (M5). Both share the UBO's light array, so the overlay's slots win when the
     *        two together would overflow deren::vulkan::max_punctual_lights.
     */
    export void apply_point_lights(deren::vulkan::runtime& runtime,
                                   gui_bindings const& bindings,
                                   std::span<deren::vulkan::punctual_light const> extra = {});

    /**
     * @ingroup chores
     * @brief assemble an animation::backend for @p runtime: the host surface an
     *        animation::controller drives (scene + per-slot buffer callbacks + the task pool),
     *        wired to the runtime's own scene, frame-slot buffers and run_tasks. Pass it to
     *        controller::init(); the controller itself never depends on deren::vulkan::runtime.
     */
    export deren::vulkan::animation::backend make_animation_backend(deren::vulkan::runtime& runtime);
} // namespace deren::chores
