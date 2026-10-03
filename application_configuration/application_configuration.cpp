module;

#include <cmath>
#include <sstream>
#include <toml++/toml.hpp>

module deren.application_configuration;

import deren.utility;

namespace deren::app_config {
    app_settings load_settings(std::string const& path) {
        app_settings settings = {};

        // toml++ parses to a parse_result when exceptions are disabled; operator bool reports
        // success and error() carries the message
        toml::parse_result const parsed = toml::parse_file(path);
        if (!parsed) {
            deren::utility::log("app_config: cannot load '{}': {}", path, parsed.error().description());
            return settings; // config_file stays empty -> caller falls back to defaults
        }
        toml::table const& table = parsed.table();

        // Dump every key the file actually carried, so "which config was loaded and what did it say" is
        // answerable from the log alone. A key that is missing from this dump fell back to the compiled-in
        // default - which is precisely what made a frame-rate cap look like a no-op: the file that was
        // loaded (the one next to the executable, not the one being edited) simply had no max_fps key.
        {
            std::ostringstream dumped;
            dumped << table;
            deren::utility::log("app_config: '{}' contents:\n{}", path, dumped.str());
        }

        settings.config_file = path;
        // read keys only when present: absent keys keep the struct defaults (an empty model /
        // grid_side = 0 also mean "not specified" to the caller)
        if (toml::node const* node = table.get("model")) {
            if (std::optional<std::string> const value = node->value<std::string>()) {
                settings.model = *value;
            }
        }
        if (toml::node const* node = table.get("grid_side")) {
            if (std::optional<int64_t> const value = node->value<int64_t>()) {
                settings.grid_side = static_cast<int32_t>(*value);
            }
        }

        if (toml::table const* paths = table.get_as<toml::table>("paths")) {
            if (toml::node const* node = paths->get("shaders_dir")) {
                if (std::optional<std::string> const value = node->value<std::string>()) {
                    settings.paths.shaders_dir = *value;
                }
            }
            if (toml::node const* node = paths->get("model_dir")) {
                if (std::optional<std::string> const value = node->value<std::string>()) {
                    settings.paths.model_dir = *value;
                }
            }

            if (toml::node const* node = paths->get("screenshot_dir")) {
                if (std::optional<std::string> const value = node->value<std::string>()) {
                    settings.paths.screenshot_dir = *value;
                }
            }
        }

        if (toml::table const* render = table.get_as<toml::table>("render")) {
            if (toml::node const* node = render->get("window_width")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.render.window_width = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = render->get("window_height")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.render.window_height = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = render->get("window_title")) {
                if (std::optional<std::string> const value = node->value<std::string>()) {
                    settings.render.window_title = *value;
                }
            }
            if (toml::node const* node = render->get("vsync")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.vsync = *value;
                }
            }
            if (toml::node const* node = render->get("max_fps")) {
                // TOML integers are not doubles: max_fps = 60 must work as written, so try both.
                // The second local is `integer_value`, not a second `value`: it would hide the one declared
                // in the first condition and MSVC /W4 reports that as C4456, an error under /WX.
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.max_fps = *value;
                } else if (std::optional<int64_t> const integer_value = node->value<int64_t>()) {
                    settings.render.max_fps = static_cast<double>(*integer_value);
                }
            }
            if (toml::node const* node = render->get("clear_color")) {
                if (toml::array const* color = node->as_array()) {
                    std::size_t i = 0;
                    for (toml::node const& element : *color) {
                        if (i >= settings.render.clear_color.size()) {
                            break;
                        }
                        if (std::optional<double> const channel = element.value<double>()) {
                            settings.render.clear_color[i++] = static_cast<float>(*channel);
                        }
                    }
                }
            }
            if (toml::node const* node = render->get("sun_intensity")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.sun_intensity = static_cast<float>(std::clamp(*value, 0.0, 3.0));
                }
            }
            if (toml::node const* node = render->get("megalights")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.megalights = *value;
                }
            }
            if (toml::node const* node = render->get("megalights_samples")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.render.megalights_samples = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = render->get("megalights_spatial_sigma")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.megalights_spatial_sigma = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("megalights_history_tolerance")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.megalights_history_tolerance = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("megalights_light_angle")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.megalights_light_angle = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("megalights_bias")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.megalights_bias = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("furnace")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.furnace = *value;
                }
            }
            if (toml::node const* node = render->get("rt_shadows")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.rt_shadows = *value;
                }
            }
            if (toml::node const* node = render->get("rt_mask_bake")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.rt_mask_bake = *value;
                }
            }
            if (toml::node const* node = render->get("rt_skin_bake")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.rt_skin_bake = *value;
                }
            }
            if (toml::node const* node = render->get("animation_time")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.animation_time = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("camera_pose")) {
                if (toml::array const* pose = node->as_array()) {
                    std::size_t value_index = 0;
                    for (toml::node const& element : *pose) {
                        if (value_index >= settings.render.camera_pose.size()) {
                            break;
                        }
                        if (std::optional<double> const number = element.value<double>()) {
                            settings.render.camera_pose[value_index++] = static_cast<float>(*number);
                        }
                    }
                    // All six are required: a partial pose is not a pose, and silently keeping half of it
                    // would pin a view nobody asked for.
                    settings.render.camera_pose_set = value_index == settings.render.camera_pose.size();
                }
            }
            if (toml::node const* node = render->get("camera_fit")) {
                if (std::optional<std::string> const value = node->value<std::string>()) {
                    settings.render.camera_fit = *value;
                }
            }
            if (toml::node const* node = render->get("shadow")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.shadow = *value;
                }
            }
            if (toml::node const* node = render->get("shadow_cascades")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.render.shadow_cascades = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = render->get("shadow_cascade_blend")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.shadow_cascade_blend = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("toon_shadow_softness")) {
                // FLOAT OR INTEGER: the five levels are a small ladder and both spellings read naturally in a
                // config file, while toml11 keeps integers and floats in separate types - so both are asked
                // for. Rounding and clamping happen in `analyse_config`, with the log line.
                if (std::optional<int64_t> const integral = node->value<int64_t>()) {
                    settings.render.toon_shadow_softness = static_cast<float>(*integral);
                } else if (std::optional<double> const real = node->value<double>()) {
                    settings.render.toon_shadow_softness = static_cast<float>(*real);
                }
            }
            if (toml::node const* node = render->get("shadow_bias_constant")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.shadow_bias_constant = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("shadow_bias_slope")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.shadow_bias_slope = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("clustered_lights")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.clustered_lights = *value;
                }
            }
            if (toml::node const* node = render->get("shadow_map_size")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.render.shadow_map_size = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = render->get("ssao")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.ssao = *value;
                }
            }
            if (toml::node const* node = render->get("ssao_radius")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.ssao_radius = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("ssao_intensity")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.ssao_intensity = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("ssao_samples")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.render.ssao_samples = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = render->get("unlit")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.unlit = *value;
                }
            }
            if (toml::node const* node = render->get("fxaa")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.fxaa = *value;
                }
            }
            if (toml::node const* node = render->get("render_scale")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.render_scale = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("upscale")) {
                if (std::optional<std::string> const value = node->value<std::string>()) {
                    settings.render.upscale = *value;
                }
            }
            if (toml::node const* node = render->get("taa")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.taa = *value;
                }
            }
            if (toml::node const* node = render->get("taa_blend_static")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.taa_blend_static = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("taa_blend_min")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.render.taa_blend_min = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = render->get("gbuffer_debug")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.gbuffer_debug = *value;
                }
            }
            if (toml::node const* node = render->get("character_forward")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.character_forward = *value;
                }
            }
            // THE REWRITTEN TOON CHAIN'S SELECTOR ([render] goo_toon): which `.spv` the character stage's fragment
            // half comes from - see `deren::vulkan::runtime::goo_toon_pipeline_name`. It rides `[render]` rather than the
            // `[toon]` rig table below because it is a frame's render setting rather than a piece of the
            // character's art direction, and because the capture instrument can only override `[render]` keys
            // (`scripts/windows/capture.ps1`), which is what makes the A/B runnable.
            if (toml::node const* node = render->get("goo_toon")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.goo_toon = *value;
                }
            }
            // THE STATIC SURROUND'S SECOND MODEL ([render] background_glb): an empty string is the default and
            // means "this frame has no background model", which keeps every recorded reference valid. The path
            // is NOT touched here - `main.cpp` resolves a relative one against the executable's directory,
            // because that is where the assets are and the config loader has no idea where the exe sits.
            if (toml::node const* node = render->get("background_glb")) {
                if (std::optional<std::string> const value = node->value<std::string>()) {
                    settings.render.background_glb = *value;
                }
            }
            // ---- THE TOON LIGHT RIG ([toon]) ----
            //
            // A TABLE OF ITS OWN rather than more keys under `[render]`, because these are the CHARACTER stage's art
            // direction rather than the frame's render settings - and because they belong together: they are the
            // lanes of one block (`deren::vulkan::toon_rig`), and a reader who moves one of them wants to see the others.
            if (toml::table const* toon = table.get_as<toml::table>("toon")) {
                for (auto const& [key, target] : {std::pair{"day_strength", &settings.toon.day_strength},
                                                  std::pair{"head_light_day0", &settings.toon.head_light_day0},
                                                  std::pair{"head_light_day1", &settings.toon.head_light_day1},
                                                  std::pair{"env_strength", &settings.toon.env_strength},
                                                  std::pair{"env_rotation", &settings.toon.env_rotation},
                                                  std::pair{"specular_strength", &settings.toon.specular_strength},
                                                  std::pair{"diffuse_blend_effect", &settings.toon.diffuse_blend_effect},
                                                  std::pair{"rim_area", &settings.toon.rim_area},
                                                  std::pair{"rim_strength", &settings.toon.rim_strength},
                                                  std::pair{"rim_nolxz_strength", &settings.toon.rim_nolxz_strength},
                                                  std::pair{"backlight_strength", &settings.toon.backlight_strength},
                                                  std::pair{"emotion_type", &settings.toon.emotion_type}}) {
                    if (toml::node const* node = toon->get(key)) {
                        if (std::optional<double> const value = node->value<double>()) {
                            *target = static_cast<float>(*value);
                        }
                    }
                }
                // THE DAY STRENGTH IS CLAMPED rather than trusted, because it is a blend weight between two whole
                // shading outcomes: a value above 1 would extrapolate past the sun's own state and a negative one
                // past the head light's, and neither is a look anybody asked for. Every other rig number is a scale
                // whose useful range is the artist's.
                settings.toon.day_strength = std::clamp(settings.toon.day_strength, 0.0f, 1.0f);
                // THREE NUMBERS OR NOTHING, the rule the sun's direction already follows: a partly-specified colour
                // would mix this config's components with the default's, which is a colour nobody asked for.
                for (auto const& [key, target] : {std::pair{"head_light_colour", &settings.toon.head_light_colour},
                                                  std::pair{"sun_dark_colour", &settings.toon.sun_dark_colour}}) {
                    if (toml::node const* node = toon->get(key)) {
                        if (toml::array const* values = node->as_array(); values != nullptr && values->size() == target->size()) {
                            for (std::size_t i = 0; i < values->size(); ++i) {
                                if (std::optional<double> const component = (*values)[i].value<double>()) {
                                    (*target)[i] = static_cast<float>(*component);
                                }
                            }
                        }
                    }
                }
            }
            if (toml::node const* node = render->get("gbuffer_channel")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.render.gbuffer_channel = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = render->get("gpu_timings")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.gpu_timings = *value;
                }
            }
            if (toml::node const* node = render->get("validation_layers")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.render.validation_layers = *value;
                }
            }
        }

        if (toml::table const* lighting = table.get_as<toml::table>("lighting")) {
            if (toml::node const* node = lighting->get("env_size")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.lighting.env_size = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = lighting->get("env_mip_count")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.lighting.env_mip_count = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = lighting->get("irr_size")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.lighting.irr_size = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = lighting->get("lut_size")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.lighting.lut_size = static_cast<int32_t>(*value);
                }
            }
            if (toml::node const* node = lighting->get("sun_direction")) {
                if (toml::array const* values = node->as_array()) {
                    // Three numbers or nothing: a partly-specified direction would silently mix this config's
                    // components with the default's, which is a light nobody asked for and no way to see it.
                    if (values->size() == settings.lighting.sun_direction.size()) {
                        for (std::size_t i = 0; i < values->size(); ++i) {
                            if (std::optional<double> const component = (*values)[i].value<double>()) {
                                settings.lighting.sun_direction[i] = static_cast<float>(*component);
                            }
                        }
                    }
                }
            }
            // The environment image and its multiplier (see the struct's own note). The path is taken
            // VERBATIM and left relative here; the host resolves it against the executable's directory,
            // because that is where the assets live and this module has no business guessing.
            if (toml::node const* node = lighting->get("environment_hdr")) {
                if (std::optional<std::string> const value = node->value<std::string>()) {
                    settings.lighting.environment_hdr = *value;
                }
            }
            if (toml::node const* node = lighting->get("environment_intensity")) {
                if (std::optional<double> const value = node->value<double>()) {
                    settings.lighting.environment_intensity = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = lighting->get("demo_lights")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.lighting.demo_lights = static_cast<int32_t>(*value);
                }
            }
            // The generated lights' geometry (see the struct's own note): a radius and a range, both as
            // fractions of the scene radius, whose defaults are the helix this mode has always used.
            for (auto const& [key, target] : {std::pair{"demo_light_radius", &settings.lighting.demo_light_radius},
                                              std::pair{"demo_light_range", &settings.lighting.demo_light_range}}) {
                if (toml::node const* node = lighting->get(key)) {
                    if (std::optional<double> const value = node->value<double>()) {
                        *target = static_cast<float>(*value);
                    }
                }
            }
            // The area light (see the struct's own note for the manifest these keys come from). The two
            // vectors take exactly the shape sun_direction takes, and for the same reason: three numbers or
            // nothing, because a partly-specified vector would mix this config's components with the default's
            // and put a light somewhere nobody asked for.
            auto const read_vec3 = [](toml::table const& source, char const* key, std::array<float, 3>& target) {
                if (toml::node const* node = source.get(key)) {
                    if (toml::array const* values = node->as_array()) {
                        if (values->size() == target.size()) {
                            for (std::size_t i = 0; i < values->size(); ++i) {
                                if (std::optional<double> const component = (*values)[i].value<double>()) {
                                    target[i] = static_cast<float>(*component);
                                }
                            }
                        }
                    }
                }
            };
            read_vec3(*lighting, "area_light_position", settings.lighting.area_light_position);
            read_vec3(*lighting, "area_light_target", settings.lighting.area_light_target);
            for (auto const& [key, target] : {std::pair{"area_light_size", &settings.lighting.area_light_size},
                                              std::pair{"area_light_power", &settings.lighting.area_light_power},
                                              std::pair{"area_light_intensity", &settings.lighting.area_light_intensity},
                                              std::pair{"area_light_softness", &settings.lighting.area_light_softness}}) {
                if (toml::node const* node = lighting->get(key)) {
                    if (std::optional<double> const value = node->value<double>()) {
                        *target = static_cast<float>(*value);
                    }
                }
            }
            for (auto const& [key, target] : {std::pair{"area_light_irradiance", &settings.lighting.area_light_irradiance},
                                              std::pair{"area_light_shadow", &settings.lighting.area_light_shadow}}) {
                if (toml::node const* node = lighting->get(key)) {
                    if (std::optional<bool> const value = node->value<bool>()) {
                        *target = *value;
                    }
                }
            }
        }

        if (toml::table const* gui = table.get_as<toml::table>("gui")) {
            if (toml::node const* node = gui->get("show")) {
                if (std::optional<bool> const value = node->value<bool>()) {
                    settings.gui.show = *value;
                }
            }
            if (toml::node const* node = gui->get("panel_width")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.gui.panel_width = static_cast<float>(*value);
                }
            }
            if (toml::node const* node = gui->get("panel_height")) {
                if (std::optional<int64_t> const value = node->value<int64_t>()) {
                    settings.gui.panel_height = static_cast<float>(*value);
                }
            }
        }

        // Sanity-clamp numeric settings: negative/absurd values would break window/swapchain
        // creation or reserve huge buffers. Non-positive window sizes fall back to the defaults;
        // grid_side is capped (the instancing stress reserves side*side transforms).
        if (settings.render.window_width <= 0) {
            settings.render.window_width = 1080;
        }
        if (settings.render.window_height <= 0) {
            settings.render.window_height = 960;
        }
        settings.gui.panel_width = std::max(settings.gui.panel_width, 0.0f); // 0 = ImGui auto-size
        settings.gui.panel_height = std::max(settings.gui.panel_height, 0.0f);
        settings.grid_side = std::clamp(settings.grid_side, 0, 90);
        if (settings.render.shadow_cascades < 1 || settings.render.shadow_cascades > 4) {
            deren::utility::log("app_config: invalid shadow_cascades {} (use 1..4), falling back to 3", settings.render.shadow_cascades);
            settings.render.shadow_cascades = 3;
        }
        if (settings.render.shadow_map_size < 256 || settings.render.shadow_map_size > 8192) {
            deren::utility::log("app_config: invalid shadow_map_size {} (use 256..8192), falling back to 2048", settings.render.shadow_map_size);
            settings.render.shadow_map_size = 2048;
        }
        // THE TOON SHADOW SOFTNESS LADDER ([render] toon_shadow_softness, 0..4): rounded to the nearest level
        // and clamped into the range, with ONE log line whenever either happened - so a config that asks for
        // 2.4 or 7 renders as a level the user can read back, rather than silently. 0 is the shipped lookup
        // and the default; the value reaches the shader through `deren::vulkan::toon_rig`'s ninth lane, and the one
        // call site that reads it is the shadow lookup inside `toon_diffuse` (`shaders/character_forward.slang`).
        //
        // NAN IS HANDLED HERE BECAUSE `std::clamp` CANNOT HANDLE IT, which is the whole reason this block is not
        // one line: clamp's comparison form returns its first argument unchanged when both comparisons are
        // false, and for NaN both ARE false - so `clamp(round(nan), 0, 4)` is still NaN. That NaN then rides
        // the rig lane into the shader's `int(clamp(floor(level + 0.5), 0.0, 4.0))`, and `int(NaN)` is
        // UNDEFINED in SPIR-V: it may land on any level, including one whose dynamic PCF loop is long enough to
        // hang the GPU. TOML can spell `nan`, so this is reachable rather than theoretical. A NaN is read as
        // "no level stated" (0, the shipped lookup) and gets ITS OWN log line, kept distinct from the
        // round/clamp line so the two cannot be confused after the fact. THE INFINITIES ARE NOT FOLDED INTO
        // THAT CASE: `round(+inf)` stays `+inf` and clamps to the top of the ladder exactly as a very large
        // finite number would, and `-inf` clamps to the bottom.
        {
            float const requested = settings.render.toon_shadow_softness;
            bool const not_a_number = std::isnan(requested);
            float const clamped = std::clamp(not_a_number ? 0.0f : std::round(requested), 0.0f, 4.0f);
            if (not_a_number) {
                deren::utility::log("app_config: toon_shadow_softness is not a number (nan), using 0 (0 = shipped 3x3 PCF)");
            } else if (clamped != requested) {
                deren::utility::log("app_config: toon_shadow_softness {} -> {} (0 = shipped 3x3 PCF, 1..4 = wider soft kernels)",
                                    requested,
                                    clamped);
            }
            settings.render.toon_shadow_softness = clamped;
        }
        if (settings.render.ssao_samples < 1 || settings.render.ssao_samples > 16) {
            deren::utility::log("app_config: invalid ssao_samples {} (use 1..16), falling back to 8", settings.render.ssao_samples);
            settings.render.ssao_samples = 8;
        }
        settings.render.ssao_radius = std::clamp(settings.render.ssao_radius, 0.0f, 100.0f);
        settings.render.ssao_intensity = std::clamp(settings.render.ssao_intensity, 0.0f, 1.0f);
        // IBL resolutions drive the CPU precompute that runs BEFORE the runtime validates anything
        // (main.cpp bakes the cubemap right after analyse_config), so an out-of-range value is not a
        // quality knob but a crash: env_size = 0 indexes an empty source buffer inside
        // build_environment_pyramid (its source_size floors at 1, so every fetch runs off the end),
        // a negative one turns "6 * size * size * 4" into a huge size_t (length_error -> terminate
        // with exceptions disabled), and env_mip_count < 2 makes the sampler's
        // "pyramid.size() - 1" wrap around. Clamp to the range the precompute actually supports.
        if (settings.lighting.env_size < 16 || settings.lighting.env_size > 4096) {
            deren::utility::log("app_config: invalid env_size {} (use 16..4096), falling back to 256", settings.lighting.env_size);
            settings.lighting.env_size = 256;
        }
        if (settings.lighting.env_mip_count < 2 || settings.lighting.env_mip_count > 12) {
            deren::utility::log("app_config: invalid env_mip_count {} (use 2..12), falling back to 5", settings.lighting.env_mip_count);
            settings.lighting.env_mip_count = 5;
        }
        if (settings.lighting.irr_size < 1 || settings.lighting.irr_size > 1024) {
            deren::utility::log("app_config: invalid irr_size {} (use 1..1024), falling back to 32", settings.lighting.irr_size);
            settings.lighting.irr_size = 32;
        }
        if (settings.lighting.lut_size < 1 || settings.lighting.lut_size > 1024) {
            deren::utility::log("app_config: invalid lut_size {} (use 1..1024), falling back to 256", settings.lighting.lut_size);
            settings.lighting.lut_size = 256;
        }
        // The environment multiplier scales radiance: a negative one would mirror the image into nonsense
        // and a NaN would poison every texel of the IBL it feeds. Both fall back to the reference's own
        // number instead of rendering an environment nobody chose. (`!(x >= 0)` is the NaN test.)
        if (!(settings.lighting.environment_intensity >= 0.0f)) {
            deren::utility::log("app_config: invalid environment_intensity {} (use a finite value >= 0), falling back to 0.35", settings.lighting.environment_intensity);
            settings.lighting.environment_intensity = 0.35f;
        }
        // The area light's own numbers. A negative size/power means "no such emitter" rather than a negative
        // one, a negative multiplier would give a negative radiance (a light that sucks photons out of the
        // frame), and a NaN or infinity in ANY of them travels straight into the light UBO and then into every
        // pixel - the same failure mode task-91 found for `toon_shadow_softness`. `!(x >= 0)` is the NaN test.
        if (!(settings.lighting.area_light_size >= 0.0f)) {
            deren::utility::log("app_config: invalid area_light_size {} (use a finite value >= 0), falling back to 0 (no area light)", settings.lighting.area_light_size);
            settings.lighting.area_light_size = 0.0f;
        }
        if (!(settings.lighting.area_light_power >= 0.0f)) {
            deren::utility::log("app_config: invalid area_light_power {} (use a finite value >= 0), falling back to 0", settings.lighting.area_light_power);
            settings.lighting.area_light_power = 0.0f;
        }
        if (!(settings.lighting.area_light_intensity >= 0.0f)) {
            deren::utility::log("app_config: invalid area_light_intensity {} (use a finite value >= 0), falling back to 1", settings.lighting.area_light_intensity);
            settings.lighting.area_light_intensity = 1.0f;
        }
        if (!(settings.lighting.area_light_softness >= 0.0f)) {
            deren::utility::log("app_config: invalid area_light_softness {} (use a finite value >= 0), falling back to 0 (automatic penumbra)", settings.lighting.area_light_softness);
            settings.lighting.area_light_softness = 0.0f;
        }
        for (auto const& [key, target] : {std::pair{"area_light_position", &settings.lighting.area_light_position},
                                          std::pair{"area_light_target", &settings.lighting.area_light_target}}) {
            for (std::size_t i = 0; i < target->size(); ++i) {
                if (!std::isfinite((*target)[i])) {
                    deren::utility::log("app_config: invalid {}[{}] (use finite numbers), falling back to 0", key, i);
                    (*target)[i] = 0.0f;
                }
            }
        }
        if (settings.lighting.area_light_size > 0.0f && !(settings.lighting.area_light_power > 0.0f)) {
            // Not an error (the user may be stripping the key light without deleting its geometry), but it is
            // never what someone means, and the symptom - a frame lit only by the environment - looks like a
            // broken area light rather than a zero power.
            deren::utility::log("app_config: area_light_size {} with area_light_power {}: the main light contributes nothing", settings.lighting.area_light_size, settings.lighting.area_light_power);
        }
        if (settings.lighting.demo_lights < 0 || settings.lighting.demo_lights > static_cast<int32_t>(max_demo_lights)) {
            deren::utility::log("app_config: invalid demo_lights {} (use 0..{}), clamping", settings.lighting.demo_lights, max_demo_lights);
            settings.lighting.demo_lights = std::clamp(settings.lighting.demo_lights, 0, static_cast<int32_t>(max_demo_lights));
        }
        // The stochastic punctual lighting's sample count, clamped to the shader's own compile-time bound (see
        // vulkan.pass.megalights_trace::max_samples): the shader's loop is bounded by that constant, so a larger
        // value here would silently do nothing rather than cost more.
        settings.render.megalights_samples = std::clamp(settings.render.megalights_samples, 1, 4);
        // 0..4: the same bound the pass clamps to, and for the same reason (past a few texels it is wider than
        // the neighbourhood it can read).
        settings.render.megalights_spatial_sigma = std::clamp(settings.render.megalights_spatial_sigma, 0.0f, 4.0f);
        settings.render.megalights_history_tolerance = std::clamp(settings.render.megalights_history_tolerance, 0.0f, 1.0f);
        settings.render.megalights_bias = std::clamp(settings.render.megalights_bias, 0.0f, 32.0f);
        settings.render.megalights_light_angle = std::clamp(settings.render.megalights_light_angle, 0.0f, 0.1f);
        if (settings.render.camera_fit != "exterior" && settings.render.camera_fit != "interior") {
            deren::utility::log("app_config: invalid camera_fit '{}' (use exterior/interior), falling back to exterior", settings.render.camera_fit);
            settings.render.camera_fit = "exterior";
        }
        return settings;
    }

    area_light_derived derive_area_light(lighting_settings const& lighting, std::array<float, 3> const& scene_origin) {
        constexpr float pi = 3.14159265358979323846f;
        area_light_derived derived = {};

        // size <= 0 is the OFF switch AND the byte-identical contract: nothing below runs, `derived` stays
        // zero-filled, the caller writes a zero light_ubo pair and leaves the sun exactly as configured.
        if (!(lighting.area_light_size > 0.0f)) {
            return derived;
        }

        // position/target are relative to the AUTHOR'S ORIGIN (the feet / the ground - see the key notes), so
        // world = scene_origin + key. That is what makes the reference package's numbers copyable verbatim.
        derived.world_centre = {scene_origin[0] + lighting.area_light_position[0],
                                scene_origin[1] + lighting.area_light_position[1],
                                scene_origin[2] + lighting.area_light_position[2]};
        std::array<float, 3> const world_target = {scene_origin[0] + lighting.area_light_target[0],
                                                   scene_origin[1] + lighting.area_light_target[1],
                                                   scene_origin[2] + lighting.area_light_target[2]};

        auto const normalised = [](std::array<float, 3> const& direction) {
            float const length_sq = direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
            if (!(length_sq > 0.0f)) {
                return std::array<float, 3>{0.0f, 0.0f, 0.0f};
            }
            float const inverse_length = 1.0f / std::sqrt(length_sq);
            return std::array<float, 3>{direction[0] * inverse_length, direction[1] * inverse_length, direction[2] * inverse_length};
        };

        // The direction the frame's ONE light gets: from the author's origin towards the emitter. This is what
        // makes the reference package's soft box the sun - main.cpp hands it to runtime.set_sun_direction while
        // the emitter contributes energy (`area_light_irradiance`).
        std::array<float, 3> const origin_to_light = {derived.world_centre[0] - scene_origin[0],
                                                      derived.world_centre[1] - scene_origin[1],
                                                      derived.world_centre[2] - scene_origin[2]};
        // The emitter's own normal, i.e. the side that emits: centre -> target. It is the penumbra's direction
        // (the second UBO lane's xyz) and the axis the shadow is offset along - see `calc_shadow_area`.
        std::array<float, 3> const to_target = {world_target[0] - derived.world_centre[0],
                                                world_target[1] - derived.world_centre[1],
                                                world_target[2] - derived.world_centre[2]};
        std::array<float, 3> const to_light = normalised(origin_to_light);
        std::array<float, 3> const axis = normalised(to_target);
        if ((to_light[0] == 0.0f && to_light[1] == 0.0f && to_light[2] == 0.0f) ||
            (axis[0] == 0.0f && axis[1] == 0.0f && axis[2] == 0.0f)) {
            // An emitter AT the author's origin (no direction to light) or aimed at itself (no emitting side).
            // There is no honest direction to hand the sun, so the area light stays off: a zero light_ubo pair
            // is the only answer that cannot invent a frame.
            deren::utility::log("app_config: area light at [{}, {}, {}] with target [{}, {}, {}] has no usable direction, ignoring it",
                                lighting.area_light_position[0], lighting.area_light_position[1], lighting.area_light_position[2],
                                lighting.area_light_target[0], lighting.area_light_target[1], lighting.area_light_target[2]);
            return derived;
        }

        derived.half = 0.5f * lighting.area_light_size;
        derived.axis = axis;
        derived.to_light_dir = to_light;
        // The emitter's Lambertian radiance: its power over its emitting area, divided by pi because a
        // Lambertian emitter radiates into a hemisphere with a cosine falloff. 4000 W over a 30 m side = 1.41471.
        derived.radiance = lighting.area_light_intensity * lighting.area_light_power /
                           (pi * lighting.area_light_size * lighting.area_light_size);
        // The penumbra is a WORLD radius in metres, not an angle: a 30 m emitter 15.8 m away subtends most of a
        // hemisphere, so its shadow's edge is soft by metres. `0.05 * size / |position - target|` is the rough
        // heuristic v1 freezes (it grows with the source and shrinks with its distance); an explicit softness
        // replaces it, and area_light_shadow = false asks for a hard edge.
        //
        // THE DISTANCE IS MEASURED TO THE TARGET, NOT TO THE AUTHOR'S ORIGIN (spec 2:47, the F2 correction):
        // the heuristic is about how far the light travels to what it is AIMED at. The two agree for the
        // shipping keys, whose target is the origin itself (|[-3, 15, 4]| = 15.811388 => 0.094868 m), and part
        // company as soon as the emitter aims somewhere else.
        float const distance_to_target = std::sqrt(to_target[0] * to_target[0] + to_target[1] * to_target[1] +
                                                   to_target[2] * to_target[2]);
        // An if/else rather than one nested conditional: `shadow ? (softness > 0 ? softness : heuristic) : 0`
        // is the same expression, but clang-tidy's readability-avoid-nested-conditional-operator rejects a
        // conditional used as a sub-expression of another under --warnings-as-errors. Found on the merge
        // commit (2026-10-02, CI run 36953918715): clang-tidy is the one gate the local loop does not run,
        // because it needs compile_commands.json and the MSYS2 clang-tidy. It is reproduced and fixed here.
        derived.penumbra = 0.0f;
        if (lighting.area_light_shadow) {
            derived.penumbra = lighting.area_light_softness > 0.0f
                                   ? lighting.area_light_softness
                                   : 0.05f * lighting.area_light_size / std::max(distance_to_target, 1e-3f);
        }
        derived.enabled = true;
        return derived;
    }

    bool wants_model_dialog(app_settings const& settings) {
        // the sentinel is compared in exactly one function on purpose (see the declaration's note)
        return settings.model == model_ask;
    }

    app_settings resolve_from_argv(int32_t const argc, char const* const* const argv, std::string const& default_config_path) {
        // 1. Collect the non-option positional arguments (--config <path> / --config=<path> is
        //    consumed as an option, not a positional), so model/grid positions stay stable
        //    regardless of where --config appears.
        std::string config_path = default_config_path;
        std::vector<std::string_view> positional;
        for (int32_t i = 1; i < argc; ++i) {
            std::string_view const arg(argv[i]);
            if (arg == "--config") {
                if (i + 1 < argc) {
                    config_path = argv[i + 1];
                }
                ++i; // skip the option's value
                continue;
            }
            if (arg.starts_with("--config=")) {
                config_path = std::string(arg.substr(9));
                continue;
            }
            positional.push_back(arg);
        }

        app_settings settings = {};
        std::error_code ec;
        if (!config_path.empty() && std::filesystem::is_regular_file(config_path, ec)) {
            settings = load_settings(config_path);
        } else if (!config_path.empty()) {
            deren::utility::log("app_config: config file '{}' not found, using defaults", config_path);
        }

        // 2. Positional argv overrides the file: [0] = model, [1] = grid side (numeric).
        if (!positional.empty() && !positional[0].empty()) {
            settings.model = std::string(positional[0]);
        }
        if (positional.size() > 1 && !positional[1].empty()) {
            std::string const arg(positional[1]);
            char* end = nullptr;
            // `strtol` answers in `long`, whose width is the PLATFORM's (32 bits on Windows, 64 on Linux): the
            // conversion is explicit so the fixed-width type this file uses everywhere else is not narrowed by
            // whichever platform happened to compile it. The clamp below is what bounds the value anyway.
            int32_t const side = static_cast<int32_t>(std::strtol(arg.c_str(), &end, 10));
            if (end != arg.c_str() && *end == '\0') {
                // grid side: clamp instead of trusting the raw value - a huge argv number would
                // otherwise make the instancing stress reserve enormous buffers (and strtol
                // overflow saturates to LONG_MAX, which the clamp also absorbs)
                settings.grid_side = std::clamp(side, int32_t{0}, int32_t{90});
            } else {
                deren::utility::log("app_config: ignoring unrecognized positional argument '{}' (expected a numeric grid side)", arg);
            }
        }
        return settings;
    }

    app_settings resolve_from_argv(int32_t const argc, char const* const* const argv) {
        // no explicit default path: fall back to "config.toml" in the working directory
        return resolve_from_argv(argc, argv, "config.toml");
    }
} // namespace deren::app_config