// Headless unit tests: app_config (pure CPU) ===================================
// The test executable runs in the build's per-test scratch directory (see the CMakeLists.txt
// VR_BUILD_TESTS block), so fixtures are addressed through the absolute VR_TEST_SOURCE_DIR the
// build injects rather than relative to the working directory.
#include "vk_test.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

import deren.application_configuration;

namespace {
    void test_load_settings_applies_toml() {
        deren::app_config::app_settings const settings = deren::app_config::load_settings(VR_TEST_SOURCE_DIR "/tests/fixtures/config_full.toml");
        CHECK(settings.model == "Models/tri.gltf");
        CHECK(settings.render.vsync);
        CHECK(!settings.render.shadow);
        CHECK(settings.render.shadow_cascades == 2);
        CHECK(settings.render.shadow_cascade_blend > 0.24f && settings.render.shadow_cascade_blend < 0.26f);
        CHECK(settings.render.toon_shadow_softness == 0.0f); // the fixture omits the key: the compiled default
        CHECK(settings.render.shadow_map_size == 1024);
        CHECK(!settings.render.clustered_lights); // fixture turns the M5 cluster pass off
        CHECK(settings.lighting.demo_lights == 3);
        CHECK(settings.lighting.environment_hdr == "hdr/test-environment.hdr"); // the fixture's own path, taken verbatim
        CHECK(settings.lighting.environment_intensity > 0.79f && settings.lighting.environment_intensity < 0.81f);
        CHECK(!settings.render.ssao); // fixture turns the M6 screen-space AO off
        CHECK(settings.render.ssao_radius > 1.49f && settings.render.ssao_radius < 1.51f);
        CHECK(settings.render.ssao_intensity > 0.49f && settings.render.ssao_intensity < 0.51f);
        CHECK(settings.render.ssao_samples == 4);
        CHECK(settings.render.rt_shadows);                                                       // fixture: the ray-traced sun shadows
        CHECK(!settings.render.rt_mask_bake);                                                    // fixture: the bake off (the A/B)
        CHECK(settings.render.rt_skin_bake);                                                     // fixture: the per-frame skin refit on
        CHECK(settings.render.animation_time > 0.74f && settings.render.animation_time < 0.76f); // fixture: a pinned pose
        CHECK(settings.render.furnace);                                                          // fixture: the analytic verification mode
        CHECK(settings.render.unlit);                                                            // fixture: the flat render mode
        CHECK(settings.render.fxaa);
        CHECK(settings.render.render_scale > 0.49f && settings.render.render_scale < 0.51f); // fixture: a half-scale render chain
        CHECK(settings.render.upscale == "linear");                                          // fixture: the bilinear reference the FSR filter is measured against
        CHECK(!settings.render.gpu_timings);
        CHECK(settings.render.gbuffer_debug);
        CHECK(settings.render.gbuffer_channel == 5);
        CHECK(settings.render.taa);
        CHECK(settings.render.taa_blend_static > 0.79f && settings.render.taa_blend_static < 0.81f);
        CHECK(settings.render.taa_blend_min > 0.19f && settings.render.taa_blend_min < 0.21f);
        // the frame's static surround (`[render] background_glb`): a STRING key, so unlike the boolean
        // switches above this CHECK cannot pass by accident - an empty compiled default would read back as
        // "" and the comparison would fail
        CHECK(settings.render.background_glb == "bg/test-background.glb");
        CHECK(!settings.gui.show);
        CHECK(settings.lighting.irr_size == 64);
        // [lighting] area_light_*: the reference package's soft box, with BOTH v1 A/B switches at their
        // NON-default value (false) - so these two CHECKs prove the parser reads the booleans instead of
        // leaving the compiled defaults (true) in place.
        CHECK(settings.lighting.area_light_size > 29.99f && settings.lighting.area_light_size < 30.01f);
        CHECK(settings.lighting.area_light_power > 3999.0f && settings.lighting.area_light_power < 4001.0f);
        CHECK(settings.lighting.area_light_position[0] > -3.01f && settings.lighting.area_light_position[0] < -2.99f);
        CHECK(settings.lighting.area_light_position[1] > 14.99f && settings.lighting.area_light_position[1] < 15.01f);
        CHECK(settings.lighting.area_light_position[2] > 3.99f && settings.lighting.area_light_position[2] < 4.01f);
        CHECK(settings.lighting.area_light_target[0] == 0.0f && settings.lighting.area_light_target[1] == 0.0f && settings.lighting.area_light_target[2] == 0.0f);
        CHECK(settings.lighting.area_light_intensity > 0.99f && settings.lighting.area_light_intensity < 1.01f);
        CHECK(!settings.lighting.area_light_irradiance);
        CHECK(!settings.lighting.area_light_shadow);
        CHECK(settings.lighting.area_light_softness > 0.24f && settings.lighting.area_light_softness < 0.26f);
        CHECK(settings.paths.shaders_dir == "shaders");
        CHECK(settings.paths.screenshot_dir == "captures");
    }

    void test_load_settings_missing_file_keeps_defaults() {
        deren::app_config::app_settings const settings = deren::app_config::load_settings(VR_TEST_SOURCE_DIR "/tests/fixtures/does_not_exist.toml");
        CHECK(settings.model.empty());
        CHECK(settings.render.shadow);
        CHECK(!settings.render.fxaa);
        CHECK(settings.render.gpu_timings); // default: pass timings are collected
        CHECK(!settings.render.gbuffer_debug);
        CHECK(settings.render.gbuffer_channel == 1);
        CHECK(!settings.render.taa);
        // default: NO background model - the frame is the one import it always was (see the key's note)
        CHECK(settings.render.background_glb.empty());
        CHECK(settings.render.shadow_cascades == 3); // default: three cascades
        CHECK(settings.render.shadow_cascade_blend > 0.09f && settings.render.shadow_cascade_blend < 0.11f);
        CHECK(settings.render.clustered_lights); // default: the cluster pass runs
        CHECK(settings.lighting.demo_lights == 0);
        CHECK(settings.render.ssao); // default: screen-space AO runs
        CHECK(settings.render.ssao_samples == 8);
        CHECK(!settings.render.rt_shadows);             // default: the cascaded shadow maps, not traced rays
        CHECK(!settings.render.rt_mask_bake);           // default: OFF - the per-triangle rule measured worse than the raster path
        CHECK(!settings.render.rt_skin_bake);           // default: OFF - the bind-pose shadow is what the L2.2b baseline measures
        CHECK(settings.render.animation_time < 0.0f);   // default: animations play (no pose pinned)
        CHECK(settings.render.shadow_map_size == 2048); // default: 2048^2 per cascade layer
        CHECK(settings.gui.show);
    }

    // The documented example is what users copy: parsing it must succeed and must produce the
    // values its comments claim, or the docs and the parser have drifted apart (M7 collation).
    void test_example_config_matches_documentation() {
        deren::app_config::app_settings const settings = deren::app_config::load_settings(VR_TEST_SOURCE_DIR "/config.example.toml");
        CHECK(!settings.config_file.empty()); // parsed, not rejected
        CHECK(!settings.model.empty());
        CHECK(settings.render.window_width == 1080);
        CHECK(settings.render.shadow);
        CHECK(settings.render.shadow_cascades == 3);
        CHECK(settings.render.shadow_map_size == 2048);
        CHECK(!settings.render.unlit); // default: the lit PBR path
        CHECK(settings.render.clustered_lights);
        CHECK(settings.render.ssao);
        CHECK(settings.render.ssao_samples == 8);
        CHECK(!settings.render.taa);
        CHECK(settings.lighting.demo_lights == 0);
        CHECK(settings.lighting.env_size == 256);
        // environment_hdr empty is the contract that keeps the procedural sky (and therefore every frame
        // taken before the key existed) byte-identical; the intensity default is the reference package's
        // own world_strength, so pointing the key at the converted studio HDR needs no second setting.
        CHECK(settings.lighting.environment_hdr.empty());
        CHECK(settings.lighting.environment_intensity > 0.349f && settings.lighting.environment_intensity < 0.351f);
        // [lighting] area_light_*: the example DOCUMENTS the reference package's soft box but ships it OFF
        // (`area_light_size = 0`), which is the byte-identical contract - a config copied from this file
        // renders what the repository rendered before the keys existed, and the block is one edit away.
        CHECK(settings.lighting.area_light_size == 0.0f);
        CHECK(settings.lighting.area_light_power == 0.0f);
        CHECK(settings.lighting.area_light_position[0] == 0.0f && settings.lighting.area_light_position[1] == 0.0f && settings.lighting.area_light_position[2] == 0.0f);
        CHECK(settings.lighting.area_light_intensity > 0.99f && settings.lighting.area_light_intensity < 1.01f);
        CHECK(settings.lighting.area_light_irradiance);
        CHECK(settings.lighting.area_light_shadow);
        CHECK(settings.lighting.area_light_softness == 0.0f);
        CHECK(settings.gui.show);
    }

    // The [lighting] resolutions feed the CPU IBL precompute, which runs BEFORE the runtime sees the
    // settings - so an unclamped out-of-range value is a crash rather than a bad-looking frame:
    // env_size = 0 reads past an empty pyramid source, a negative size becomes a huge allocation
    // request (length_error -> terminate, exceptions are off), and env_mip_count < 2 wraps the
    // sampler's "pyramid.size() - 1". Every one of these must land on its documented default.
    void test_lighting_sizes_are_clamped() {
        deren::app_config::app_settings const settings = deren::app_config::load_settings(VR_TEST_SOURCE_DIR "/tests/fixtures/config_bad_lighting.toml");
        CHECK(settings.lighting.env_size == 256);
        CHECK(settings.lighting.env_mip_count == 5);
        CHECK(settings.lighting.irr_size == 32);
        CHECK(settings.lighting.lut_size == 256);
        CHECK(settings.lighting.environment_intensity > 0.349f && settings.lighting.environment_intensity < 0.351f); // -1.0 -> the default
        // The area light's own numbers: a negative size/power means "no such emitter" rather than a negative
        // one, `nan` on the multiplier would ride the light UBO into every pixel (clamp cannot catch a NaN),
        // and a negative softness would ask the shading for a negative penumbra radius.
        CHECK(settings.lighting.area_light_size == 0.0f);
        CHECK(settings.lighting.area_light_power == 0.0f);
        CHECK(settings.lighting.area_light_intensity > 0.99f && settings.lighting.area_light_intensity < 1.01f);
        CHECK(settings.lighting.area_light_softness == 0.0f);
    }

    // scripts/make_config.py WRITES config.toml, so every value it can emit has to be a value
    // app_config can read back - including the ones whose bounds the loader clamps. This fixture is
    // that generator's full default output (one entry per key it writes); it exists because the two
    // drifted apart once already: the generator silently omitted eleven [render] keys the parser
    // understood, and its env_mip_count default range (1..10) overlapped a value the loader rejects.
    // A key added to the generator without a parser (or a clamp added without the generator) shows
    // up here as a wrong value rather than as a user's surprise.
    void test_generated_config_parses() {
        deren::app_config::app_settings const settings = deren::app_config::load_settings(VR_TEST_SOURCE_DIR "/tests/fixtures/config_generated_defaults.toml");
        CHECK(!settings.config_file.empty()); // parsed, not rejected
        CHECK(settings.model == "gltf_model/DamagedHelmet.gltf");
        CHECK(settings.grid_side == 0);
        // [render] presentation
        CHECK(settings.render.window_width == 1080);
        CHECK(settings.render.window_height == 960);
        CHECK(settings.render.window_title == "deren");
        CHECK(!settings.render.vsync);       // generator default: Mailbox (uncapped)
        CHECK(settings.render.max_fps == 0); // generator default: 0 = uncapped, and the compiled default too
        CHECK(settings.render.camera_fit == "exterior");
        CHECK(!settings.render.unlit);
        // [render] shadow mapping
        CHECK(settings.render.shadow);
        CHECK(settings.render.shadow_cascades == 3);
        CHECK(settings.render.shadow_map_size == 2048);
        CHECK(settings.render.shadow_cascade_blend > 0.09f && settings.render.shadow_cascade_blend < 0.11f);
        CHECK(settings.render.toon_shadow_softness == 0.0f); // generator default = the shipped lookup
        // [render] shading + post-processing: the eleven keys the generator used to omit. TWO OF THESE
        // ARE BOOLEANS WHOSE GENERATOR DEFAULT IS false, which is the compiled default as well - so
        // `CHECK(!taa)` proves the key is ACCEPTED, not that the parser read it (a parser that ignored
        // `taa` altogether would read false too). `config_full.toml` is the fixture that proves the
        // read-back, with every one of these keys at a non-default value.
        CHECK(!settings.render.taa);
        CHECK(settings.render.taa_blend_static > 0.89f && settings.render.taa_blend_static < 0.91f);
        CHECK(settings.render.taa_blend_min > 0.49f && settings.render.taa_blend_min < 0.51f);
        CHECK(!settings.render.fxaa);
        CHECK(settings.render.render_scale > 0.99f && settings.render.render_scale < 1.01f); // generator default: unscaled
        CHECK(settings.render.upscale == "easu");                                            // generator default: FSR 1's upsampler
        CHECK(!settings.render.gbuffer_debug);
        CHECK(settings.render.gbuffer_channel == 1);
        CHECK(settings.render.gpu_timings);
        // [render] lights + SSAO
        CHECK(settings.render.clustered_lights);
        CHECK(settings.render.ssao);
        CHECK(settings.render.ssao_radius > 0.49f && settings.render.ssao_radius < 0.51f);
        CHECK(settings.render.ssao_intensity > 0.99f && settings.render.ssao_intensity < 1.01f);
        CHECK(settings.render.ssao_samples == 8);
        // [render] ray tracing: written by the generator like every other switch, so it round-trips
        CHECK(!settings.render.rt_shadows);
        CHECK(!settings.render.rt_mask_bake);         // default: the mask bake is off (see the generated-defaults fixture)
        CHECK(!settings.render.rt_skin_bake);         // default: the per-frame skin refit is off (same fixture)
        CHECK(settings.render.animation_time < 0.0f); // default: -1, i.e. play (the generator writes it out)
        CHECK(!settings.render.furnace);              // default: a normal frame, not the verification mode
        // [render] validation
        CHECK(settings.render.validation_layers);
        // [gui]
        CHECK(settings.gui.show);
        CHECK(settings.gui.panel_width > 379.0f && settings.gui.panel_width < 381.0f);
        CHECK(settings.gui.panel_height > 139.0f && settings.gui.panel_height < 141.0f);
        // [lighting]: the loader's clamps must leave every generator default untouched
        CHECK(settings.lighting.env_size == 256);
        CHECK(settings.lighting.env_mip_count == 5);
        CHECK(settings.lighting.irr_size == 32);
        CHECK(settings.lighting.lut_size == 256);
        CHECK(settings.lighting.environment_hdr.empty()); // the generator's own default: the procedural sky
        CHECK(settings.lighting.environment_intensity > 0.349f && settings.lighting.environment_intensity < 0.351f);
        // [lighting] area_light_*: OFF by default, and the generator writes all eight keys out, so this is
        // also the check that the two files (generator + fixture) have not drifted apart.
        CHECK(settings.lighting.area_light_size == 0.0f);
        CHECK(settings.lighting.area_light_power == 0.0f);
        CHECK(settings.lighting.area_light_position[0] == 0.0f && settings.lighting.area_light_position[1] == 0.0f && settings.lighting.area_light_position[2] == 0.0f);
        CHECK(settings.lighting.area_light_target[0] == 0.0f && settings.lighting.area_light_target[1] == 0.0f && settings.lighting.area_light_target[2] == 0.0f);
        CHECK(settings.lighting.area_light_intensity > 0.99f && settings.lighting.area_light_intensity < 1.01f);
        CHECK(settings.lighting.area_light_irradiance);
        CHECK(settings.lighting.area_light_shadow);
        CHECK(settings.lighting.area_light_softness == 0.0f);
    }

    void test_model_ask_sentinel_is_recognized_and_never_a_path() {
        // `model = "ask"` is the config's way of asking for the startup file dialog, and the ONLY thing that
        // may read it that way is deren::app_config::wants_model_dialog - every other caller has to see the string
        // as the path it looks like. The near misses below are why that comparison is exact and
        // case-sensitive: a build that opened a file named "ask" instead of asking is the bug this guards.
        deren::app_config::app_settings const asking = deren::app_config::load_settings(VR_TEST_SOURCE_DIR "/tests/fixtures/config_ask_model.toml");
        CHECK(asking.model == "ask"); // the parser keeps the sentinel verbatim
        CHECK(deren::app_config::wants_model_dialog(asking));

        deren::app_config::app_settings const named = deren::app_config::load_settings(VR_TEST_SOURCE_DIR "/tests/fixtures/config_full.toml");
        CHECK(!deren::app_config::wants_model_dialog(named)); // a real path is a real path

        deren::app_config::app_settings near = {};
        near.model = "ASK"; // a model may legitimately be called this
        CHECK(!deren::app_config::wants_model_dialog(near));
        near.model = "ask.glb";
        CHECK(!deren::app_config::wants_model_dialog(near));
        near.model = " ask";
        CHECK(!deren::app_config::wants_model_dialog(near));
        near.model = {}; // empty means "locate the default", not "ask"
        CHECK(!deren::app_config::wants_model_dialog(near));
    }

    // `[render] toon_shadow_softness` is a five-step ladder (0..4) sanitized in `analyse_config` on the way
    // into `deren::vulkan::toon_rig`'s ninth lane. THE CASE WORTH A TEST OF ITS OWN IS `nan`, because `std::clamp`
    // CANNOT CATCH IT: clamp's comparison form returns its first argument unchanged when both comparisons are
    // false, and for NaN both ARE false - so `clamp(round(nan), 0, 4)` is still NaN. That NaN then rides the
    // lane into the shader's `int(clamp(floor(level + 0.5), 0.0, 4.0))`, and `int(NaN)` is UNDEFINED in
    // SPIR-V: it may land on any level, including one whose dynamic PCF loop is long enough to hang the GPU.
    // TOML can spell `nan`, so a real config file is the honest way to reach the case. The infinities are
    // deliberately NOT folded into it: `round(+inf)` stays `+inf` and clamps to the top of the ladder like a
    // very large finite number would.
    void test_toon_shadow_softness_ladder_is_sanitized() {
        auto const load_one = [](std::string const& body) {
            std::filesystem::path const path{"test_app_config_toon_shadow_softness.toml"};
            {
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file << body;
            }
            deren::app_config::app_settings const settings = deren::app_config::load_settings(path.string());
            std::filesystem::remove(path);
            return settings.render.toon_shadow_softness;
        };

        CHECK(load_one("[render]\ntoon_shadow_softness = nan\n") == 0.0f);  // NaN -> the shipped lookup, not a level
        CHECK(load_one("[render]\ntoon_shadow_softness = inf\n") == 4.0f);  // +inf clamps to the top of the ladder
        CHECK(load_one("[render]\ntoon_shadow_softness = -inf\n") == 0.0f); // -inf clamps to the bottom
        CHECK(load_one("[render]\ntoon_shadow_softness = -1\n") == 0.0f);   // integers are accepted too
        CHECK(load_one("[render]\ntoon_shadow_softness = 5\n") == 4.0f);
        CHECK(load_one("[render]\ntoon_shadow_softness = 2.4\n") == 2.0f); // rounded to the nearest level
        CHECK(load_one("[render]\ntoon_shadow_softness = 3\n") == 3.0f);
        CHECK(load_one("[render]\nshadow = true\n") == 0.0f); // absent key: the compiled default
    }

    // The area light's maths lives in ONE pure function (`deren::app_config::derive_area_light`), so the reference
    // package's own numbers can be pinned without a device: 4000 W over a 30 m side IS the author's soft key,
    // and this derivation is what turns it into the frame's radiance, direction and penumbra. Three cases
    // carry the contract - the reference numbers, size 0 (the byte-identical path: `enabled == false` and
    // every field zero), and the degenerate geometry that would otherwise normalise a zero vector and put a
    // NaN direction through the sun, the cascades and every pixel.
    void test_area_light_derivation_matches_the_reference_package() {
        // The AUTHOR'S ORIGIN - where the manifest measures from, i.e. the ground under the character. At the
        // origin the manifest's numbers are the world numbers, which is why this case reads `scene_origin = 0`.
        std::array<float, 3> const scene_origin = {0.0f, 0.0f, 0.0f};
        deren::app_config::lighting_settings lighting = {};
        lighting.area_light_size = 30.0f;
        lighting.area_light_power = 4000.0f;
        // the manifest's Blender position [-3, -4, 15] turned Y-up: 15.8 m away, ~72 degrees of elevation
        lighting.area_light_position = {-3.0f, 15.0f, 4.0f};
        lighting.area_light_target = {0.0f, 0.0f, 0.0f};

        deren::app_config::area_light_derived const derived = deren::app_config::derive_area_light(lighting, scene_origin);
        CHECK(derived.enabled);
        CHECK(derived.half > 14.99f && derived.half < 15.01f);
        CHECK(derived.world_centre[0] > -3.01f && derived.world_centre[0] < -2.99f);
        CHECK(derived.world_centre[1] > 14.99f && derived.world_centre[1] < 15.01f);
        CHECK(derived.world_centre[2] > 3.99f && derived.world_centre[2] < 4.01f);
        // radiance = intensity * power / (pi * size^2) = 4000 / (pi * 900) = 1.41471: the soft key's own value
        CHECK(derived.radiance > 1.41471f - 1.0e-4f && derived.radiance < 1.41471f + 1.0e-4f);
        // the sun points FROM the scene AT the emitter ([-3, 15, 4], |v| = 15.8114): up, to the left, forward
        CHECK(derived.to_light_dir[0] < 0.0f);
        CHECK(derived.to_light_dir[1] > 0.9f);
        CHECK(derived.to_light_dir[2] > 0.0f);
        float const to_light_length = derived.to_light_dir[0] * derived.to_light_dir[0] +
                                      derived.to_light_dir[1] * derived.to_light_dir[1] +
                                      derived.to_light_dir[2] * derived.to_light_dir[2];
        CHECK(to_light_length > 0.999f && to_light_length < 1.001f); // normalised, not just scaled
        // axis = centre -> target = [3, -15, -4]: the emitting side faces DOWN into the scene
        CHECK(derived.axis[1] < 0.0f);
        // the automatic penumbra: 0.05 * 30 / 15.8114 = 0.09487 m
        CHECK(derived.penumbra > 0.0948f && derived.penumbra < 0.0950f);

        // THE MEASURED AUTHOR ORIGIN OF THIS SCENE: the character's feet land at y = -2.198 (that is
        // `main.cpp`'s `scene_floor_y`), so the same manifest numbers must land at [-3, 12.802, 4] - 15.8114 m
        // from the origin, which is why the radius and the direction the sun gets are both unchanged by the
        // shift. Anchoring on the bounding-box centre instead put the emitter at [-1.422, 15.546, 3.798] and
        // tilted the visible light by ~6 degrees of elevation and ~16 of azimuth; this case is what stops that
        // from coming back.
        std::array<float, 3> const feet_origin = {0.0f, -2.198f, 0.0f};
        deren::app_config::area_light_derived const at_feet = deren::app_config::derive_area_light(lighting, feet_origin);
        CHECK(at_feet.enabled);
        CHECK(at_feet.world_centre[0] > -3.01f && at_feet.world_centre[0] < -2.99f);
        CHECK(at_feet.world_centre[1] > 12.80f && at_feet.world_centre[1] < 12.81f);
        CHECK(at_feet.world_centre[2] > 3.99f && at_feet.world_centre[2] < 4.01f);
        CHECK(at_feet.to_light_dir[1] > 0.9f); // `to_light_dir` is `position` normalised, origin or no origin
        CHECK(at_feet.penumbra > 0.0948f && at_feet.penumbra < 0.0950f);

        // size 0 (the default) is OFF, and OFF means ZERO - the caller writes both lanes as
        // glm::vec4(0) and multiplies the sun by 1.0, so the frame is the one from before the keys existed.
        deren::app_config::lighting_settings const off = {};
        deren::app_config::area_light_derived const disabled = deren::app_config::derive_area_light(off, scene_origin);
        CHECK(!disabled.enabled);
        CHECK(disabled.half == 0.0f && disabled.radiance == 0.0f && disabled.penumbra == 0.0f);
        CHECK(disabled.world_centre[0] == 0.0f && disabled.world_centre[1] == 0.0f && disabled.world_centre[2] == 0.0f);
        CHECK(disabled.axis[0] == 0.0f && disabled.to_light_dir[1] == 0.0f && disabled.to_light_dir[2] == 0.0f);

        // an explicit softness replaces the automatic value, and `shadow = false` removes the penumbra
        // entirely - a hard edge is what that switch asks for, not a smaller soft one.
        lighting.area_light_softness = 0.4f;
        CHECK(deren::app_config::derive_area_light(lighting, scene_origin).penumbra == 0.4f);
        lighting.area_light_shadow = false;
        CHECK(deren::app_config::derive_area_light(lighting, scene_origin).penumbra == 0.0f);

        // An emitter AT the author's origin has no direction to give the sun, and one aimed at itself has no
        // emitting side: both are OFF rather than normalise(0) = NaN.
        deren::app_config::lighting_settings degenerate = {};
        degenerate.area_light_size = 30.0f;
        degenerate.area_light_power = 4000.0f;
        deren::app_config::area_light_derived const no_direction = deren::app_config::derive_area_light(degenerate, scene_origin);
        CHECK(!no_direction.enabled);
        CHECK(no_direction.to_light_dir[0] == 0.0f && no_direction.axis[0] == 0.0f);

        // ... and position/target really are RELATIVE to that origin: with the origin at [10, 0, -2] the same key
        // gives a world centre 10 m to the right, which is what lets the manifest's numbers be copied over.
        std::array<float, 3> const moved_origin = {10.0f, 0.0f, -2.0f};
        deren::app_config::lighting_settings relative = {};
        relative.area_light_size = 4.0f;
        relative.area_light_power = 100.0f;
        relative.area_light_position = {1.0f, 2.0f, 3.0f};
        deren::app_config::area_light_derived const shifted = deren::app_config::derive_area_light(relative, moved_origin);
        CHECK(shifted.enabled);
        CHECK(shifted.world_centre[0] > 10.99f && shifted.world_centre[0] < 11.01f);
        CHECK(shifted.world_centre[1] > 1.99f && shifted.world_centre[1] < 2.01f);
        CHECK(shifted.world_centre[2] > 0.99f && shifted.world_centre[2] < 1.01f);
        // surface -> emitter is [+1, +2, +3] there: still normalised, still pointing at the emitter
        CHECK(shifted.to_light_dir[0] > 0.0f && shifted.to_light_dir[1] > 0.0f && shifted.to_light_dir[2] > 0.0f);
        CHECK(shifted.penumbra > 0.0534f && shifted.penumbra < 0.0535f); // 0.05 * 4 / |[1, 2, 3]|

        // F2 (v1.1): the automatic penumbra's distance is measured to the TARGET, not to the author's origin.
        // This emitter sits 15 m from the origin but 30 m from what it is aimed at, so the two readings differ
        // by 2x - the case pins the one that shipped: 0.05 * 30 / 30 = 0.05, NOT 0.05 * 30 / 15 = 0.1.
        deren::app_config::lighting_settings aimed = {};
        aimed.area_light_size = 30.0f;
        aimed.area_light_power = 4000.0f;
        aimed.area_light_position = {0.0f, 0.0f, 15.0f};
        aimed.area_light_target = {0.0f, 0.0f, -15.0f};
        deren::app_config::area_light_derived const aimed_away = deren::app_config::derive_area_light(aimed, feet_origin);
        CHECK(aimed_away.enabled);
        CHECK(aimed_away.world_centre[2] > 14.99f && aimed_away.world_centre[2] < 15.01f);
        CHECK(aimed_away.penumbra > 0.0499f && aimed_away.penumbra < 0.0501f); // 0.05 * 30 / |[0, 0, -30]|
        // ... and NOT the 0.1 an origin-anchored distance would give:
        CHECK(aimed_away.penumbra < 0.0999f);
    }

    void test_resolve_from_argv_merges_config_and_positional() {
        char const* argv[] = {"vk_test", "Models/tri.gltf", "3"};
        deren::app_config::app_settings const settings =
            deren::app_config::resolve_from_argv(3, argv, VR_TEST_SOURCE_DIR "/tests/fixtures/config_full.toml");
        CHECK(settings.model == "Models/tri.gltf");
        CHECK(settings.grid_side == 3);
    }
} // namespace

int32_t main() {
    test_load_settings_applies_toml();
    test_example_config_matches_documentation();
    test_load_settings_missing_file_keeps_defaults();
    test_lighting_sizes_are_clamped();
    test_generated_config_parses();
    test_model_ask_sentinel_is_recognized_and_never_a_path();
    test_toon_shadow_softness_ladder_is_sanitized();
    test_area_light_derivation_matches_the_reference_package();
    test_resolve_from_argv_merges_config_and_positional();
    return deren::vk_test::finish("test_app_config");
}
