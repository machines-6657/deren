#include <charconv>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
// ---- THE GOO REFERENCE'S PRE-INTEGRATED FGD LUT IS A PNG, AND THIS IS THE DECODER ----
//
// `third_party/stb/stb_image.h` is already vendored and already used by `gltf_loader.cpp` for GLB textures, and
// that translation unit defines `STB_IMAGE_IMPLEMENTATION` (whose functions are `STBIDEF`, i.e. `extern`). A
// SECOND implementation in this file is therefore not a double definition but a LINK CLASH, and the fix is the
// documented one: `STB_IMAGE_STATIC` makes this file's copy file-local, so the two never meet at link time. It
// costs a second copy of ~7 KB of decode routines and buys the one thing a global image needs - a decoder at the
// point of use, without making the engine core depend on `gltf_loader` (see `vulkancorekit`'s own note on why
// that dependency is deliberately absent).
//
// The include path resolves because `vulkancorekit` exports `third_party/` as a PUBLIC include directory, and
// `deren` links it.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
import deren.vstd;
import deren.application_configuration;
import deren.chores; // demo bootstrap helpers (shader loading / dir locating / pipelines)
import deren.gltf_loader;
import deren.toon_material_sidecar; // the .toon.tsv a character model carries: its toon maps, and the _Use flags
import deren.utility;               // re-exports deren.utility:frame_clock / frame_stats / bvh / better_pmr / thread_pool / data_block
import deren.vulkan.animation;
import deren.vulkan.animation.mmd_motion; // VMD (MMD motion) parsing, retargeting and clip baking      // animation::controller: glTF playback / skinning / morphs on the runtime tree
import deren.vulkan.math;
import deren.vulkan.scene_tree; // scene storage + GPU primitives (was vulkan.model)
import deren.vulkan.runtime;
import deren.vulkan.render_start_demo; // the example's pass wiring: this app's chain, from outside the renderer

// Route std::pmr allocations through mimalloc (deren.utility:better_pmr) before main(): this
// file-scope reference's dynamic initialization runs at startup, so every runtime/scene
// object built below already allocates its std::pmr vectors from mimalloc. Idempotent —
// other TUs (vulkan/runtime/runtime.cpp) keep their own copy of the same singleton.
[[maybe_unused]] static auto& pmr = deren::utility::init_pmr(); // NOLINT(keep-alive)

namespace {

    // ---- THE ASSET'S OWN `extras` ROWS, WHICH THE LOADER CARRIES AND THE TOON LOOKUP CONSULTS FIRST ----
    //
    // THE THIRD DATA SOURCE, and the ordering between the three is the whole content of this helper. A toon
    // material's per-material facts can come from:
    //   1. the ASSET'S `extras` block (`deren::gltf::material::extras_floats`, read by the loader from the game's own
    //      exported table that ships inside the .glb) - the ORIGINAL;
    //   2. the `.toon.tsv` SIDECAR beside the model - this port's TRANSCRIPTION of a subset of the same tables;
    //   3. the family table in `shaders/toon_params.slang` - the port's own fallback.
    // EXTRAS WINS, and the reason is what the two files ARE rather than which is newer: extras is written by the
    // game's own material pipeline and travels inside the asset, while the sidecar is a copy this repository made
    // from the game's JSON tables - a copy can be incomplete (it carries 109 of the 194 names) and can be stale,
    // and a copy must not outrank the thing it was copied from. The sidecar stays the fallback rather than being
    // dropped because it carries facts the extras do not (the sidecar's own slot vocabulary, and every model
    // whose .glb has no `extras` block at all), and the family table stays the last resort because some of its
    // numbers are the port's own (see its note).
    //
    // MEASURED, AND THE HONEST STATEMENT IS THAT THIS ORDER IS NOT SETTLED BY chen's FRAMES: on `chars\chen.glb`
    // the two sources never disagree - every name they share carries the same value in both - so no frame can
    // tell the two rules apart. What the frames DO settle is that the extras value reaches the GPU at all (see
    // `deren-ab/extras_dump.md` and the probe arms in `remaining_port_spec.md`'s "extras 数据源"), and the order
    // above is therefore a rule stated where it is applied and PROVABLE BY ASSET: a sidecar row that contradicts
    // extras is ignored, and the probe in that section is exactly such a row.
    [[nodiscard]] std::optional<float> extras_float_of(deren::gltf::scenes const* const scenes, std::string_view const material_name, std::string_view const row) {
        if (scenes == nullptr) {
            return std::nullopt;
        }
        deren::gltf::material const* const material = scenes->material_by_name(material_name);
        if (material == nullptr) {
            return std::nullopt;
        }
        auto const found = material->extras_floats.find(std::string(row));
        if (found == material->extras_floats.end()) {
            return std::nullopt; // this asset states no such row: the caller's next source answers
        }
        return found->second;
    }

    // ---- scripted capture (dev tool) ----
    // Verifying anything visual used to need a human at the keyboard (F12). Two flags remove
    // that: `--capture-frames <n>` renders n frames, saves a screenshot through EXACTLY the F12
    // path (same read-back, same PNG writer, same logged line) and quits; `--capture-camera
    // <yaw,pitch,distance>` overrides the orbit camera at startup (degrees / scene units) so a
    // specific view - e.g. looking down-sun, where a shadow leak shows - reproduces on demand.
    // Both are stripped from argv before app_config sees them, so the positional model /
    // grid-side slots keep their meaning.
    struct capture_options {
        int32_t frames = 0;                                        // 0 = normal interactive run
        std::optional<std::array<float, 3>> camera = std::nullopt; // yaw(deg), pitch(deg), distance
        std::optional<glm::vec3> target = std::nullopt;            // orbit target override (optional)
        // Degrees of YAW added per presented frame (`--capture-sweep`). 0 - the default - is a fixed
        // camera, which is what every other capture in this repository is.
        //
        // WHY IT EXISTS: with the camera still, every reprojection path in the renderer is exercised only
        // in its trivial case - a motion vector of zero, a history fetched from the pixel it came from.
        // TAA's resolve, the GI temporal accumulation, the reflection's history and the velocity target
        // itself are all "correct" under a static camera no matter how they are written, so a break in
        // any of them passed the capture harness. This makes a capture move the camera by a fixed amount
        // per FRAME INDEX (not per wall-clock second), so a sweep capture is as reproducible as a still
        // one - the gate compares two runs of it like any other scenario.
        float sweep_yaw_deg_per_frame = 0.0f;
        // Seconds of ANIMATION time added per PRESENTED frame (`--capture-animation-sweep`). 0 - the
        // default - leaves playback on the wall clock, which is what an interactive run wants.
        //
        // WHY IT EXISTS, and it is the camera sweep's argument one member up, for the other half of the
        // frame: a capture of a DEFORMING mesh is only reproducible if the pose is a function of the frame
        // index. [render] animation_time makes such a capture reproducible and USELESS for this purpose - a
        // pinned pose uploads the same skin matrices every frame, so the previous-frame deformation equals
        // the current one and the deformation term of the motion vector is exactly zero, which is why a
        // pinned-pose screenshot cannot tell a deformation-aware renderer from the one that ignores
        // deformation. The wall clock (frame_clock::delta_seconds()) is reproducible in neither direction.
        // So playback gets the frame-indexed clock the camera already has: the pose is a function of the
        // frames PRESENTED, and two runs of one animation sweep are byte-identical.
        float animation_seconds_per_frame = 0.0f;
    };

    // strtof with a full-string check (no exceptions: std::stof would abort under -fno-exceptions)
    std::optional<float> parse_number(std::string_view const text) {
        if (text.empty()) {
            return std::nullopt;
        }
        std::string const copy(text); // strtof needs a null-terminated buffer
        char* end = nullptr;
        float const value = std::strtof(copy.c_str(), &end);
        if (end == copy.c_str() || *end != '\0') {
            return std::nullopt;
        }
        return value;
    }

    capture_options parse_capture_options(int32_t const argc, char** argv, std::vector<char*>& filtered) {
        capture_options options = {};
        filtered.push_back(argv[0]);
        // "--flag value" or "--flag=value"; returns the value and advances i past it
        auto const take_value = [&](int32_t& i, std::string_view const arg, std::string_view const name) -> std::optional<std::string_view> {
            if (arg == name) {
                return i + 1 < argc ? std::optional<std::string_view>(argv[++i]) : std::nullopt;
            }
            if (arg.size() > name.size() && arg[name.size()] == '=' && arg.starts_with(name)) {
                return arg.substr(name.size() + 1);
            }
            return std::nullopt;
        };
        for (int32_t i = 1; i < argc; ++i) {
            std::string_view const arg(argv[i]);
            // --mmd-motion is consumed here even though main scans argv for it itself: the app's
            // parser would otherwise treat the flag as the positional model path and panic with
            // 0xC0000409 (exactly what an unknown first positional does).
            if (take_value(i, arg, "--mmd-motion").has_value()) {
                continue;
            }
            if (std::optional<std::string_view> const value = take_value(i, arg, "--capture-frames")) {
                if (std::optional<float> const frames = parse_number(*value)) {
                    options.frames = static_cast<int32_t>(std::max(0.0f, *frames));
                } else {
                    deren::utility::log("capture: ignoring '--capture-frames {}' (expected a frame count)", *value);
                }
                continue;
            }
            if (std::optional<std::string_view> const value = take_value(i, arg, "--capture-camera")) {
                // yaw,pitch,distance[,target.x,target.y,target.z] - comma-separated numbers
                std::array<float, 6> parsed = {};
                std::size_t cursor = 0;
                std::size_t count = 0;
                bool valid = true;
                while (cursor <= value->size()) {
                    std::size_t const comma = value->find(',', cursor);
                    std::string_view const piece = value->substr(cursor, comma == std::string_view::npos ? std::string_view::npos : comma - cursor);
                    std::optional<float> const number = parse_number(piece);
                    if (!number || count == parsed.size()) {
                        valid = false;
                        break;
                    }
                    parsed[count++] = *number;
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    cursor = comma + 1;
                }
                if (valid && (count == 3 || count == 6)) {
                    options.camera = std::array<float, 3>{parsed[0], parsed[1], parsed[2]};
                    if (count == 6) {
                        options.target = glm::vec3(parsed[3], parsed[4], parsed[5]);
                    }
                } else {
                    deren::utility::log("capture: ignoring '--capture-camera {}' (expected yaw,pitch,distance[,target.x,target.y,target.z])", *value);
                }
                continue;
            }
            if (std::optional<std::string_view> const value = take_value(i, arg, "--capture-sweep")) {
                // degrees of yaw per presented frame - see capture_options::sweep_yaw_deg_per_frame
                if (std::optional<float> const number = parse_number(*value)) {
                    options.sweep_yaw_deg_per_frame = *number;
                } else {
                    deren::utility::log("capture: ignoring '--capture-sweep {}' (expected degrees of yaw per frame)", *value);
                }
                continue;
            }
            if (std::optional<std::string_view> const value = take_value(i, arg, "--capture-animation-sweep")) {
                // seconds of animation time per presented frame - see capture_options::animation_seconds_per_frame
                if (std::optional<float> const number = parse_number(*value)) {
                    options.animation_seconds_per_frame = *number;
                } else {
                    deren::utility::log("capture: ignoring '--capture-animation-sweep {}' (expected seconds per frame)", *value);
                }
                continue;
            }
            filtered.push_back(argv[i]);
        }
        if (options.frames > 0) {
            deren::utility::log("capture mode: {} frames, then screenshot + quit", options.frames);
            if (options.sweep_yaw_deg_per_frame != 0.0f) {
                deren::utility::log("capture camera sweep: {:.3f} deg of yaw per frame, from whatever pose the scene settled on", options.sweep_yaw_deg_per_frame);
            }
            if (options.animation_seconds_per_frame != 0.0f) {
                deren::utility::log("capture animation sweep: {:.4f} s of animation per frame, from the clip's own start", options.animation_seconds_per_frame);
            }
        }
        return options;
    }

    /// @brief the `_RD` image name a material's `RampIndex` selects, i.e. the reference's `RampSelect` resolved
    ///
    /// READ OUT OF `gooblender/nodes.json`, NOT GUESSED (spec §2.3/§2.4): the selector is
    /// `运算.004 = idx < 0.9900000095367432`, `运算.005 = idx < 2.0`, `运算.006 = idx > 0.9900000095367432`,
    /// `运算.007 = 运算.005 < 运算.006`, `运算.008 = idx > 2.990000009536743`, and three MIXes whose `f = 0`
    /// takes their **A** side. Its four outcomes, in the order the cascade is written below (the LAST comparison is
    /// the FIRST test, because it is the outermost MIX):
    ///
    ///     idx >  2.990000009536743  -> `图像纹理.006`  `TPLK_actor_common_cloth_03_RD`
    ///     idx >= 2.0                -> `图像纹理.005`  `T_actor_common_cloth_04_RD`
    ///     idx >= 0.9900000095367432 -> `图像纹理.001`  `T_actor_common_body_01_RD`
    ///     otherwise (negative too)  -> `图像纹理`      `T_actor_common_cloth_04_RD`
    ///
    /// THE THIRD AND FOURTH ARMS NAME THE SAME IMAGE, which is the spec's A8 rather than a shortcut: slots 1 and 3
    /// are byte-identical (`sha256 ff12009a...`) and were exported from two different characters' folders.
    /// THE FIRST TWO A'S ARE WHAT MAKES THE BOUNDARIES WHAT THEY ARE: at `idx` exactly `0.9900000095367432` the
    /// first comparison is FALSE and the MIX takes its A side, which is the BODY ramp - writing "`f` true takes
    /// cloth" flips that boundary, and it is the boundary every laevatain material sits one step away from.
    ///
    /// THE RETURN IS A `string_view` INTO THE FILE, so a material that states no `RampIndex` (every other family,
    /// and every material of every other character in this repository) gets an EMPTY view - which the lane resolver
    /// turns into `invalid`, i.e. "no base ramp", i.e. the old chain's diffuse.
    [[nodiscard]] std::string_view toon_base_ramp_name(deren::toon::material_sidecar const& material) {
        float const index = material.scalar("_GooRampIndex", 0.0f);
        return index > 2.990000009536743f     ? std::string_view{"TPLK_actor_common_cloth_03_RD"}
               : index >= 2.0f                ? std::string_view{"T_actor_common_cloth_04_RD"}
               : index >= 0.9900000095367432f ? std::string_view{"T_actor_common_body_01_RD"}
                                              : std::string_view{"T_actor_common_cloth_04_RD"};
    }

    /// THE `RS_Index` ROW'S NAME, which is the sidecar's contract rather than this table's: it is written here
    /// because `toon_colour_row` (which carries the same name for lane 29) is declared BELOW this point, and the
    /// parse below has to be the same parse that lane gets - see `toon_rs_sheet_of`.
    static constexpr std::string_view toon_rs_arm0_row = "_GooRSArm0";

    /// THE REFERENCE'S SECOND `_RS` SHEET SLOT - the one `RS_Index = 1` selects - AND IT IS NOT A LANE, nor may it
    /// become one. `toon_lane` below is sized by `deren::vulkan::toon_slot::count`, so a second sheet slot AS A LANE would
    /// cost a 17th vocabulary entry, a wider lane block (`toon_lane_blocks` 3 -> 4, in the SAME heap slot:
    /// `vulkan/core/core.declarations.cppm`'s `heap_slots::toon_lanes`) and a FOURTH accessor beside the three that
    /// exist (`shaders/heap_access.slang`'s `toon_lanes_at`/`toon_lanes2_at`/`toon_lanes3_at`, one `uint4` column
    /// each) - a fourth DESCRIPTOR SET is NOT the cost and was never the proposal
    /// (`goo_step15_lane_rs_index_spec.md` §3.5/§9.4: the rejected `Rb`).
    /// The reference needs none of that: `RS_Index` is a per-material CONSTANT (§1.3 - no material link in either
    /// dump), so the choice is resolved here at registration time and the shader keeps reading ONE slot, exactly
    /// like `RampSelect`/`RampIndex` above and for the same reason.
    static constexpr std::string_view toon_rs_sheet_b = "_GooRSSheet1";

    /// @brief what the `armA` sheet lane should resolve: a sidecar slot NAME, and whether it is the second sheet
    struct toon_rs_sheet_choice {
        /// the name to look up in the model; empty means this material names no sheet at all
        std::string_view name = {};
        /// true when `RS_Index` selected `_GooRSSheet1` AND that sheet's own `_UseGooRSSheet1` switch is on
        bool second = false;
    };

    /// @brief which of the reference's two `_RS` sheets @p material reads, given its first sheet's name @p sheet_a
    ///
    /// THE RULE IS A CHOICE AND NOT A REPRODUCTION, and this paragraph is the whole reason the route is named the
    /// way it is: the reference MIXES its two sheets CONTINUOUSLY - `mix(A, B, clamp(RS_Index, 0, 1))` in
    /// `混合.032` (spec §1.1) - while this answers one of two names. An `RS_Index` strictly between the endpoints
    /// is therefore QUANTISED TO AN ENDPOINT and is NOT the reference's blend. That is a named limitation of this
    /// route (§9.3 (z)(1)) rather than an oversight, and it must not be written up as a faithful implementation.
    ///
    /// WHY `0.5`, AND WHY THE TIE GOES TO THE SECOND SHEET: the two dumps carry only the two endpoints (§1.3).
    /// The count is 27 `PBRToonBase`-group instances in total, NOT 23: the `gooblender` dump has 23 (22 at `0.0`
    /// plus the single `1.0`, `M_actor_yvonne_cloth_03`, which is in no captured asset) and `chen_dump` adds 4
    /// more under `PBRToonBase.001`, ALL of them `0.0`. The union of those values is still `{0.0, 1.0}`, so ANY
    /// threshold in `(0, 1)` agrees exactly; `0.5` is the reference's own 50/50 blend, i.e. the one value at which
    /// neither sheet is closer than the other; and `>=` rather than `>` fixes the tie's direction here instead of
    /// leaving it to an operator accident (§9.2).
    ///
    /// THE SECOND SHEET NEEDS ITS OWN SWITCH AS WELL AS THE LANE'S: `_UseGooRSSheet` was already asked by the
    /// caller before this runs (see the flag gate at the top of `toon_texture`), and `_UseGooRSSheet1` is asked
    /// here - absent means off, the sidecar module's contract for every optional slot. So a material that names
    /// `_GooRSSheet1` without switching it on reads the FIRST sheet, and so does one that switches it on without
    /// naming anything: `name` carries the answer and an empty `name` means the first sheet.
    [[nodiscard]] toon_rs_sheet_choice toon_rs_sheet_of(deren::toon::material_sidecar const& material, std::string_view const sheet_a) {
        toon_rs_sheet_choice choice = {};
        choice.name = sheet_a;
        // `RS_Index` IS THE `.x` OF A `color` ROW, so it sits in `others` VERBATIM and `scalar()` cannot see it
        // (the module keeps a `color` row as text on purpose). THE PARSE IS LANE 29's OWN RULE - comma separated
        // components read with `from_chars` - but this function answers only the FIRST component (`.x`, the
        // `substr` below), because the row's other components (`RS ColorTint`) are a different question. A missing
        // or unparseable component keeps the NEUTRAL, which for this lane is 0.0, i.e. the first sheet. Reading the
        // same row with a different rule would let this choice and the lane-29 colour lane disagree about one
        // material.
        // `from_chars` ALSO ACCEPTS `inf` AND `nan`: `nan` fails the `>= 0.5` test and keeps the first sheet, while
        // `inf` WOULD select the second - neither is reachable from the reference data, whose two dumps carry only
        // `0.0` and `1.0` (§1.3), so both are pinned here rather than measured away.
        float rs_index = 0.0f;
        if (auto const row = material.others.find(std::string(toon_rs_arm0_row)); row != material.others.end()) {
            std::string_view const row_text = row->second;
            std::string_view const first_component = row_text.substr(0, row_text.find(','));
            float value = 0.0f;
            if (auto const result = std::from_chars(first_component.data(), first_component.data() + first_component.size(), value);
                result.ec == std::errc{}) {
                rs_index = value;
            }
        }
        // WRITTEN AS A NEGATED COMPARISON because that is also the NaN-safe direction: an `RS_Index` that is
        // somehow not a number (a row this parser cannot produce, but a `nan` row it CAN) is NOT `>= 0.5`, so it
        // answers the first sheet rather than silently selecting the second one.
        if (!(rs_index >= 0.5f)) {
            return choice;
        }
        // THE SECOND SWITCH, asked by the flag's own name: `enabled("_GooRSSheet1")` is `_UseGooRSSheet1`, absent
        // meaning off (see the sidecar module's second rule).
        if (!material.enabled(toon_rs_sheet_b)) {
            return choice;
        }
        std::string_view const sheet_b = material.slot(toon_rs_sheet_b);
        if (sheet_b.empty()) {
            return choice;
        }
        choice.name = sheet_b;
        choice.second = true;
        return choice;
    }

} // namespace

// `int`, NOT `int32_t`: the C++ standard requires main's own signature to use the keyword, and this is the one
// place in the tree where that is a LANGUAGE rule rather than a choice about the project's arithmetic types.
int main(int argc, char** argv) {
    // --version: print the version (single source: project(VERSION) in CMakeLists.txt, injected
    // as DEREN_VERSION_*) and exit before any config / Vulkan init.
    for (int32_t i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            deren::utility::println("deren {}.{}.{}", DEREN_VERSION_MAJOR, DEREN_VERSION_MINOR, DEREN_VERSION_PATCH);
            return 0;
        }
    }

    // 1-3. Resolve the startup config in one step (chores): merge the config file (config.toml
    // by default, --config <path> to override) with positional argv overrides (argv[1] = model,
    // argv[2] = grid side (numeric)), then locate the shaders/ dir and pick the model file.
    // Panics on any missing configured/located resource. The dev-tool capture flags are removed
    // from argv first (see parse_capture_options) so they cannot land in the positional slots.
    std::vector<char*> filtered_argv;
    capture_options const capture = parse_capture_options(argc, argv, filtered_argv);
    deren::chores::startup_config const config = deren::chores::analyse_config(static_cast<int32_t>(filtered_argv.size()), filtered_argv.data());
    deren::app_config::app_settings const& settings = config.settings;
    std::filesystem::path const& shaders_dir = config.shaders_dir;
    std::string const& model_path = config.model_path;

    // startup banner: version (single source: project(VERSION) in CMakeLists.txt)
    deren::utility::log("deren {}.{}.{}", DEREN_VERSION_MAJOR, DEREN_VERSION_MINOR, DEREN_VERSION_PATCH);

    // 4. Kick off the runtime-independent heavy CPU stages BEFORE constructing the (heavy)
    //    Vulkan runtime, so window/instance/device/swapchain init overlaps the model parse +
    //    texture decode and the base environment cubemap generation. IBL resolutions come from
    //    the [lighting] config (smaller = faster startup, larger = higher quality).
    auto const env_size = settings.lighting.env_size;
    auto const env_mip_count = settings.lighting.env_mip_count;
    auto const irr_size = settings.lighting.irr_size;
    auto const lut_size = settings.lighting.lut_size;
    auto const startup_start = std::chrono::steady_clock::now();

    // ---- THE ENVIRONMENT IMAGE, WHEN THE CONFIG NAMES ONE ([lighting] environment_hdr) ----
    //
    // This is the reference package's own route to an environment: its world is an equirectangular HDRI
    // (`lighting/studio_01_1k.exr`) that its manifest gives a `world_strength` of 0.35, not a procedural
    // sky. The engine's decoder is `stb_image` (see the include note at the top of this file), which reads
    // Radiance `.hdr` and NOT OpenEXR, so the package's file is converted once, offline, into exactly that
    // format - `deren-ab/bg/_hdr_convert.py`, which is also where its own statistics were measured. The
    // engine gains no dependency: it gains a file it could already read.
    //
    // The pixels are MOVED into the async task that bakes the cubemap, so no buffer here has to outlive the
    // statement that loaded it. An empty path means the analytic sky, untouched.
    std::vector<float> environment_pixels = {};
    int32_t environment_width = 0;
    int32_t environment_height = 0;
    if (!settings.lighting.environment_hdr.empty()) {
        std::filesystem::path environment_path = settings.lighting.environment_hdr;
        if (environment_path.is_relative()) {
            // relative to the EXECUTABLE's directory, the same rule [render] background_glb follows
            environment_path = deren::utility::executable_directory() / environment_path;
        }
        int32_t channels = 0;
        float* const decoded = stbi_loadf(environment_path.string().c_str(), &environment_width, &environment_height, &channels, 4); // 4 = force RGBA, the layout the bake reads
        if (decoded == nullptr) {
            deren::utility::panic(std::source_location::current(), "failed to load the environment HDRI '{}': {}", environment_path.string(), stbi_failure_reason());
        }
        environment_pixels.assign(decoded, decoded + static_cast<std::size_t>(environment_width) * static_cast<std::size_t>(environment_height) * 4);
        stbi_image_free(decoded);
        // The image's own mean radiance is logged BEFORE the bake, because it turns the multiplier's effect
        // on the ambience into arithmetic instead of a guess - and it is the number the offline conversion
        // script prints for the same file, which is how the two ends are checked against each other.
        double luma_sum = 0.0;
        for (std::size_t i = 0; i + 2 < environment_pixels.size(); i += 4) {
            luma_sum += 0.2126 * environment_pixels[i] + 0.7152 * environment_pixels[i + 1] + 0.0722 * environment_pixels[i + 2];
        }
        double const texel_count = static_cast<double>(environment_width) * static_cast<double>(environment_height);
        double const luma_mean = texel_count > 0.0 ? luma_sum / texel_count : 0.0;
        deren::utility::log("environment: '{}' loaded as {}x{} RGBA float, linear luma mean {:.5f}; intensity {:.2f} (the reference package's own world_strength) scales it to {:.5f}",
                            environment_path.string(),
                            environment_width,
                            environment_height,
                            luma_mean,
                            settings.lighting.environment_intensity,
                            luma_mean * settings.lighting.environment_intensity);
        deren::utility::log("environment: the procedural sky's gradient and its baked sun are off for the IBL; the DIRECT light ([lighting] sun_direction) still drives the shadows, the shading and the visible disc");
    }

    // THE SUN, AS ONE VECTOR FOR EVERYTHING THAT HAS TO AGREE ABOUT IT: the light UBO built from it (the
    // shading's `light_dir` and the shadow cascades), the disc the sky draws - which reads that UBO's
    // `light_dir`, so the sky cannot put its sun where the shadows do not fall - and the env cubemap baked
    // below, whose sun has to sit in the same place for a reflection's glint to match the sky it reflects.
    // (That last consumer is the analytic sky's alone: an environment image carries its own lights, and the
    // direct light is not baked into it a second time.)
    auto const& sun_direction = settings.lighting.sun_direction;
    auto env_future = environment_pixels.empty()
                          ? deren::vulkan::generate_environment_cubemap_async(env_size, sun_direction)
                          : deren::vulkan::generate_environment_cubemap_from_equirect_async(std::move(environment_pixels), environment_width, environment_height, env_size, settings.lighting.environment_intensity);
    auto load_future = deren::gltf::load_model_async(model_path);

    // 5. Construct deren::vulkan::runtime from the startup render settings (window size / title /
    //    vsync; the defaults in render_settings mirror the historic hardcoded values)
    deren::vulkan::core_create_info core_options = {};
    core_options.window_width = settings.render.window_width;
    core_options.window_height = settings.render.window_height;
    core_options.window_title = settings.render.window_title;
    core_options.vsync = settings.render.vsync;
    core_options.validation_layers = settings.render.validation_layers;
    // the render scale is a CREATION option and not a runtime setter, because it decides the extent every
    // render target is created with (see core_create_info::render_scale): it has to be in the options the
    // core is constructed from, and it applies from the first frame.
    core_options.render_scale = settings.render.render_scale;
    // ---- A SCRIPTED CAPTURE GETS NO WINDOW, which is the one place a human would have seen one. The
    //      capture flags exist so a visual check needs nobody at the keyboard (see capture_options above),
    //      and the gate / the A/B batches then run this binary twenty to a hundred times in a row: with a
    //      visible window that is a row of flashes on the screen of whoever is using the machine. The
    //      result is unaffected BY CONSTRUCTION - the screenshot is a read-back of the swapchain image the
    //      frame rendered into, not a capture of the window - and the gate's references plus the four
    //      recorded A/B anchors are byte-identical with this line and without it, which is the measurement
    //      behind the claim. The interactive path (`capture.frames == 0`) is untouched: a normal window.
    core_options.window_visible = capture.frames == 0;
    deren::vulkan::runtime runtime{core_options};
    runtime.background_color = glm::vec3(settings.render.clear_color[0], settings.render.clear_color[1], settings.render.clear_color[2]);
    // shadow is applied after enable_shadows() below (it needs the shadow maps to exist)
    // per-pass GPU timings (timestamp queries): on by default, reported in the log + overlay
    runtime.set_gpu_timings(settings.render.gpu_timings);
    // Shadow cascades: applied here (BEFORE the scene import) because the shadow map's layered image
    // is created when the first scene set binds it - see runtime::set_shadow_cascades.
    runtime.set_shadow_cascades(static_cast<uint32_t>(settings.render.shadow_cascades));
    runtime.set_shadow_cascade_blend(settings.render.shadow_cascade_blend);
    runtime.set_shadow_depth_bias(settings.render.shadow_bias_constant, settings.render.shadow_bias_slope, 0.0f);
    // shadow map edge length: same startup-only rule as the cascade count (the layered image and its
    // views are created when the first scene set binds them, so this must precede the scene import)
    runtime.set_shadow_map_size(static_cast<uint32_t>(settings.render.shadow_map_size));
    auto const runtime_ready = std::chrono::steady_clock::now();
    deren::utility::log("vulkan runtime initialized: {:.1f} ms (async model load + env generation running in background)", std::chrono::duration<double, std::milli>(runtime_ready - startup_start).count());

    // 6. Pipelines up front (deren::chores::setup_pipeline): the standard PBR pipeline (used by the
    //    imported scene) and the directional shadow pass. The legacy
    //    triangle demo pipeline is no longer created - nothing draws it.
    deren::chores::setup_pipeline(runtime, shaders_dir);
    // THE EXAMPLE'S OWN WIRING: this application's passes are fed by `deren.vulkan.render_start_demo`, which finds them
    // in the chain the runtime owns and answers the frame's per-stage questions (see the module's header). The
    // runtime holds none of these references itself any more, which is what lets a second application hand it a
    // different chain - and the demo object outlives the frame loop because it lives here, in the app's own scope.
    deren::vulkan::render_start_demo start_demo;
    static_cast<void>(start_demo.attach(runtime)); // builds this app's chain and hands it over
    // ... and the CREATE step runs over that chain (the shaders above are registered by now): every pass builds what
    // it owns, and the renderer's two jobs - which are not passes - are created with them.
    runtime.create_passes();
    // Stochastic punctual lighting (docs/megalights.md): the switch and the sample count, with the two bias
    // terms left at the pass's own defaults (they are self-intersection guards rather than look knobs, and the
    // pass clamps them). OFF by default, so a stock config is the unshadowed path it always was.
    // The two bias radii are UE's pair scaled by the config's dial (see app_config's note): the floor at
    // normal incidence and the larger offset at grazing incidence, where the ray leaves nearly parallel to the
    // surface and a small offset would let it re-hit the surface it started from.
    float const ml_bias_floor = 0.01f * settings.render.megalights_bias;
    float const ml_bias_grazing = 0.1f * settings.render.megalights_bias;
    start_demo.set_megalights(settings.render.megalights, static_cast<uint32_t>(settings.render.megalights_samples), 0.001f, ml_bias_floor, ml_bias_grazing);
    // ... and the chain's policy: UE's relative depth tolerance (0.03) and frame-count cap (12) for the temporal
    // running mean, plus this chain's own spatial pre-filter width, which is the config's because it is the dial
    // between grain and detail (see app_config's note and docs/megalights.md's measurement).
    start_demo.set_megalights_accumulation(settings.render.megalights_history_tolerance, 12.0f, settings.render.megalights_spatial_sigma);
    // Same bargain as the ray-traced shadows: a request the runtime grants only on a device with ray
    // queries and a built top level structure - otherwise the GI rays keep marching the depth buffer.
    // The furnace verification mode: an analytic reference rather than another estimator of ours.
    runtime.set_furnace(settings.render.furnace);
    runtime.set_rt_shadows(settings.render.rt_shadows);
    // ... and the alphaMode MASK bake, which is what keeps a masked surface from being SOLID to those rays:
    // a compute pass collapses the triangles the material's alpha cuts out, before the structures are built.
    runtime.set_rt_mask_bake(settings.render.rt_mask_bake);
    // ... and the per-frame skinning pass, which is what keeps an ANIMATED caster's traced shadow where the
    // caster actually is: the structures are built from the bind pose, so without it the ray sees the mesh
    // at rest.
    runtime.set_rt_skin_bake(settings.render.rt_skin_bake);

    // 7. Collect the async startup results
    auto scenes = load_future.get();
    if (!scenes) {
        deren::utility::panic(std::source_location::current(), "failed to load model '{}': error code {}", model_path, static_cast<int32_t>(scenes.error()));
    }
    std::vector<float> const env = env_future.get();
    auto const startup_done = std::chrono::steady_clock::now();
    deren::utility::log("model loaded + environment cubemap (startup window incl. runtime init): {:.1f} ms", std::chrono::duration<double, std::milli>(startup_done - startup_start).count());

    // 8. Whole-model world AABB + loader diagnostics (gltf_loader, pure CPU over the retained
    //    scene data): logs the scene summary (contents, hierarchy, animations/skins/morphs/
    //    cameras/lights) and returns the world bounds that frame the orbit camera and center
    //    the scene before import. Panics when the model has no drawable primitives.
    deren::gltf::scene_bounds const bounds = deren::gltf::log_scene_diagnostics(*scenes);
    glm::vec3 const scene_center = bounds.min * 0.5f + bounds.max * 0.5f;
    float const scene_radius = glm::length(bounds.max - bounds.min) * 0.5f;

    // 8a. THE AREA LIGHT, DERIVED ONCE (see `deren::app_config::derive_area_light` and the `[lighting] area_light_*`
    //     notes): the reference package's 30 m soft box, expressed through the frame's ONE directional light.
    //     Its `position`/`target` keys are relative to the AUTHOR'S ORIGIN - the character's feet on the ground -
    //     because that is the frame the manifest was authored in ("the character stands at the origin"), and in
    //     Blender that origin is on the ground under it. This engine does not keep that origin as a quantity: it
    //     sinks the whole model by `scene_radius` (see `scene_sink` below), which lands the feet at
    //     `bounds.min.y + (-scene_center.y - scene_radius)`, with world XZ at 0. That point is what is handed
    //     over here, and it is logged so the number can be audited against the frame. Anchoring on
    //     `scene_center` instead was wrong: the bounding-box centre sits half a body higher, ~0.85 m here, which
    //     moves the emitter's visible elevation by ~6 degrees and its azimuth by ~16 degrees.
    //     Deriving it here rather than in the frame loop keeps the per-frame mirror below free of the maths;
    //     the values themselves are constant for the whole run.
    //
    //     `area_light.enabled == false` (the default: `area_light_size = 0`) means every field is zero, and
    //     the loop then multiplies the sun by 1.0 and writes a zero light_ubo pair - byte for byte the frame
    //     that existed before these keys did.
    //     The `std::array` is not decoration: `application_configuration` deliberately does not depend on glm
    //     (the derivation is unit-tested there), so this is the one place the scene's origin crosses over.
    float const scene_floor_y = bounds.min.y - scene_center.y - scene_radius;
    deren::app_config::area_light_derived const area_light =
        deren::app_config::derive_area_light(settings.lighting, std::array<float, 3>{0.0f, scene_floor_y, 0.0f});
    // `world_centre - scene_origin`, i.e. the emitter's direction FROM the author's origin, normalised by the
    // derivation: this is the sun's direction while the emitter TAKES OVER the main light, and it is why
    // `[lighting] sun_direction` is not the last word on where the light comes from then.
    //
    // THE EMITTER TAKES OVER ONLY WHEN IT CONTRIBUTES ENERGY (v1.1, the A' ruling): `area_light_irradiance`
    // is that switch, and `area_light_size == 0` (the default) is OFF outright. With `irradiance = false` the
    // area light still widens the shadow into a penumbra, but the sun keeps `gui.sun_direction` and the plain
    // `gui.sun_intensity` - the "penumbra only" mode, which is why `area_light_scale` is 1.0f there too.
    bool const area_light_takes_over = area_light.enabled && settings.lighting.area_light_irradiance;
    float const area_light_scale = area_light_takes_over ? area_light.radiance : 1.0f;
    if (area_light.enabled) {
        deren::utility::log("area light: author origin [0.000, {:.3f}, 0.000], centre [{:.3f}, {:.3f}, {:.3f}] half {:.3f} m, "
                            "axis [{:.3f}, {:.3f}, {:.3f}], radiance {:.5f}{}, penumbra {:.3f} m",
                            scene_floor_y, area_light.world_centre[0], area_light.world_centre[1], area_light.world_centre[2],
                            area_light.half, area_light.axis[0], area_light.axis[1], area_light.axis[2], area_light.radiance,
                            area_light_takes_over ? " (TAKES OVER the main light: x sun_intensity, capped at 3.0 by set_sun_intensity)"
                                                  : " (penumbra only: the sun keeps its own direction and intensity)",
                            area_light.penumbra);
    }

    // Sink the model so it sits near the world horizon (y = 0) and move the camera target with it:
    // the camera then orbits/looks at the model's position instead of the scene origin.
    glm::vec3 const scene_sink(0.0f, -scene_radius, 0.0f);
    runtime.camera.target = scene_sink;

    // 8b. THE TOON MATERIAL SIDECAR, RESOLVED AGAINST THIS MODEL'S OWN IMAGES.
    //
    //     A toon character's materials read a SECOND set of texture slots that glTF has no concept of - a
    //     diffuse ramp, a shadow LUT, a specular ramp, a matcap, a face SDF - and the `.toon.tsv` beside the
    //     model names them. Nothing connects that file to the model except IMAGE NAMES, which is why
    //     `gltf_loader` now carries them (see `texture_data::name` and `scenes::texture_index_by_name`).
    //
    //     THIS BLOCK RESOLVES THE JOIN AND REPORTS IT, which is deliberately all it does so far: it is the one
    //     place the resolution can be SEEN on a real character rather than on a test fixture, and what it
    //     prints is exactly the data the shading stage needs next - each material's family, and for every toon
    //     slot the sidecar declares, whether the artist switched it on and which of the model's images it
    //     names. The three outcomes are each reported distinctly because they are three different situations:
    //     no sidecar at all (the normal case for a model that is not a character), a slot whose flag is off,
    //     and a slot that is ON while the model has no such image - which is the case a consumer must handle by
    //     leaving the feature off rather than by substituting something.
    // THE ASSET PIPELINE'S VOCABULARY FOR THE FOUR TOON LANES, and the only place it appears: one entry per
    // `deren::vulkan::toon_slot`, in lane order, carrying BOTH names the pipeline uses for it.
    //
    // TWO NAMES RATHER THAN ONE, and the second is not derivable from the first: the ramp and LUT lanes are
    // switched on by `_Use<Slot>`, but the MATCAP lane `_MatcapTex` is switched on by `_UseMatcap` - the slot's
    // `Tex` suffix is not in the flag, which is a fact about the pipeline rather than a rule. Deriving the flag
    // from the slot answered "off" for every matcap in the file, silently, which is the failure the sidecar
    // module exists to prevent and precisely the one its own convention could not see: it is right for three
    // lanes and wrong for the fourth.
    //
    // IT SITS HERE, ABOVE THE DIAGNOSTIC, because the diagnostic prints every slot in the file and has to answer
    // the same question the lookup does. It did not, once: the lookup was taught the real flag names while the log
    // went on calling the convention, and the result was a log line reading `| off` about a lane the renderer was
    // reading - a diagnostic that contradicts the renderer is worse than no diagnostic, so both now ask
    // `toon_flag_for`.
    struct toon_lane_names {
        std::string_view slot;
        std::string_view flag;
    };
    static constexpr std::array<toon_lane_names, static_cast<std::size_t>(deren::vulkan::toon_slot::count)> toon_lane = {{
        {"_DiffRampMap", "_UseDiffRampMap"},
        {"_ShadowLutTex", "_UseShadowLutTex"},
        {"_SpecRampMap", "_UseSpecRampMap"},
        {"_MatcapTex", "_UseMatcap"},
        {"_SDFLightmap", "_UseSDFLightmap"},
        // THE METALLIC/GLOSS LANE FOLLOWS THE `_Use<Slot>` CONVENTION, unlike the matcap above: `_UseMetallicGlossMap`
        // is what the material files carry (24 of the 45 materials across the five characters export one).
        {"_MetallicGlossMap", "_UseMetallicGlossMap"},
        // THE FACE MASK IS SWITCHED ON BY THE SDF'S OWN FLAG, which is a fact about the material files rather than a
        // convention: there is no `_UseSDFMask` in any of them (the census over the five characters has
        // `_UseSDFLightmap` on 10 materials and `_UseSDFMask` on none), and the mask is meaningless without the SDF
        // it belongs to - the two are authored for one face and read at one coordinate.
        {"_SDFMask", "_UseSDFLightmap"},
        // THE EMOTION ATLAS HAS A FLAG OF ITS OWN (`_UseEmotionMap`, on the face and on nothing else in chen).
        {"_EmotionMap", "_UseEmotionMap"},
        // THE SPLIT NORMAL'S FLAG IS `_UseSpecBumpMap`, not `_UseSplitNormalMap` - the second slot in this table
        // whose flag does not follow the convention (the matcap is the first), and found the same way: by asking
        // the files rather than by deriving the name.
        {"_SplitNormalMap", "_UseSpecBumpMap"},
        // THE GOO IRIS BALL'S FLAG FOLLOWS THE CONVENTION (`_UseGooMatcap05`), which is the only kind of name it
        // could have: the reference has no per-material switch for this texture at all (it is sampled from inside
        // the group), so the flag is the port's own sidecar vocabulary rather than a row copied from an asset.
        // Keeping it in the `_Use<Slot>` shape is what lets the diagnostic above walk it like every other lane.
        {"_GooMatcap05", "_UseGooMatcap05"},
        // THE GOO BASE RAMP'S FLAG FOLLOWS THE CONVENTION (`_UseGooBaseRamp`), for the matcap lane's own reason:
        // the reference resolves this image INSIDE its `RampSelect` group and no material states a per-material
        // switch for it, so the flag is this port's sidecar vocabulary rather than a row copied from an asset -
        // and the `_Use<Slot>` shape is what lets the diagnostic above walk it like every other lane. The
        // reference's own switch is `RampIndex`, which is not a flag but a SELECTOR, and it is resolved here (see
        // `toon_base_ramp_name` below): the four slots hold two distinct images and every material this asset
        // gives the group to states `0.0` or `1.0`, which is the spec's §2.6 + its A8.
        {"_GooBaseRamp", "_UseGooBaseRamp"},
        // ---- STEP 7: THE FACE CONTAINER'S THREE MASKS, ALL `_Use<Slot>` ----
        //
        // THE CONVENTION HOLDS FOR ALL THREE, and that is a fact about this port's vocabulary rather than about an
        // asset: the reference samples every one of them from INSIDE `PBRToonBaseFace` (they are not material
        // inputs at all), so there is no game-side `_Use...` row to copy and the flag is this sidecar's own switch
        // - exactly as `_GooMatcap05` and `_GooBaseRamp` above are. Keeping the shape is what lets the diagnostic
        // walk them like every other lane.
        {"_GooFaceSDF", "_UseGooFaceSDF"},
        {"_GooFaceCmM", "_UseGooFaceCmM"},
        {"_GooFaceCsutm", "_UseGooFaceCsutm"},
        // ---- STEP 13: THE `RS EFF` MASK ----
        //
        // THE CONVENTION HOLDS, for the matcap-lane reason rather than an asset's: the reference samples `_M`
        // INSIDE its `Arknights: Endfield_PBRToonBase` group - it is a `组输入` socket, not a `_`-prefixed game
        // property - so there is no game-side `_Use...` row to copy and `_UseGooRSMask` is this sidecar's own
        // switch. `_M` is the LEFT input of `混合.038 = _M ⊙ RS ColorTint` (mechanism table #14); its texel is a
        // Non-Color mask (uploaded UNORM) and the smoothstep that turns it into a factor is the shader's, because
        // its `max` is per material and rides `_GooRSTint.w`. See `toon_slot::goo_rs_mask`.
        //
        // IT MUST BE IN THIS TABLE RATHER THAN ONLY IN THE SIDECAR: the lookup below resolves a lane's name here
        // first and returns the WHITE fallback before the flag is even asked, so a lane missing from this table
        // reads `slot_name` as empty, resolves no texture, and the branch is silently off - no warning, no
        // compile error, only a frame that does not move.
        {"_GooRSMask", "_UseGooRSMask"},
        // `_GooRSSheet` IS `armA`'s 256x1 `_RS` COLOUR SHEET, THE SIBLING OF THE MASK ABOVE AND THE OTHER HALF OF
        // MECHANISM TABLE #14's FIRST ARM. It is a texture lane rather than a colour lane because it is an image,
        // it is uploaded SRGB because it is a colour (the sampler decodes it, the shader does not), and it is
        // `_GooRSSheet`/`_UseGooRSSheet` rather than a game property name for the mask's own reason: the reference
        // reads it through a `组输入` socket, so there is no `_Use...` row to copy. A material with no row here and
        // `RS Model = 0` gets `arm0 = 0` - the branch must NOT fall back to the white texture at index 0. See
        // `toon_slot::goo_rs_sheet` and the `armA` block in `shaders/goo_toon.slang`.
        //
        // AND IT IS ONE OF THE REFERENCE'S TWO SHEETS, WHICH IS WHY THERE IS NO SECOND ENTRY HERE: `RS_Index`
        // (`_GooRSArm0`'s `.x`, a `color` row) chooses between this sheet and the second one, and the host
        // resolves that choice BY NAME in `toon_texture` (`toon_rs_sheet_of` above) instead of growing this
        // table - a 17th entry would need `toon_slot::count` to move, i.e. a second texture lane, a
        // `toon_lane_blocks` of 4 and a fourth descriptor set the shader would have to choose between. The shader
        // therefore still reads ONE slot and still never looks at `rs_arm0_lane.x`: the second sheet is a
        // MATERIAL's choice, not a frame's, which is the whole reason it can be answered here. See
        // `goo_step15_lane_rs_index_spec.md` §9 and `toon_rs_sheet_b`.
        {"_GooRSSheet", "_UseGooRSSheet"},
    }};
    // THE MATERIAL COLOUR VOCABULARY, one `color` row name per `deren::vulkan::toon_colour_lane`, in lane order - the
    // same arrangement the texture table above uses and for the same reason: the asset pipeline's spelling belongs
    // where the sidecar is read, and the ORDER is the contract with the shader's lane indices.
    static constexpr std::array<std::string_view, static_cast<std::size_t>(deren::vulkan::toon_colour_lane::count)> toon_colour_row = {{
        "_SDFRimColor",
        "_EyeHighLightColor",
        "_EyeScatteringColor",
        // THE OUTLINE LANE IS THE ONE LANE WHOSE ROW CARRIES ONLY PART OF THE VALUE: `.rgb` is this row
        // (`color _OutlineTintColor`, the game's name for the author's `_OutlineColor`), and `.w` is the SEPARATE
        // `float _OutlineWidth`, read as a scalar below - see `toon_colour_lane::outline_edge`.
        "_OutlineTintColor",
        // THE SPECULAR LANE'S ROW IS A `float` AND NOT A `color`, which is why this table's name is used only as
        // the SIDECAR FALLBACK for it: the value the port prefers is the ASSET'S (`extras_float_of`), and this
        // row is consulted second - see `toon_colour_lane::specular_strength` and `extras_float_of`.
        "_Specular",
        // THE PARALLAX DEPTH'S ROW IS A `float` TOO, AND ON THIS ASSET THE SIDECAR CARRIES IT NOWHERE:
        // `chars\chen.glb.toon.tsv` has no `_ParallaxScale` row at all, so the asset's `extras` block is the only
        // source that speaks for it here and the stage's own constant is the answer for every material it does
        // not speak for. The name still belongs in this table for the other direction: a model whose sidecar
        // holds the row is a model this lane can answer for without any `extras` block at all.
        //
        // WHAT "EVERY MATERIAL IT DOES NOT SPEAK FOR" TURNED OUT TO MEAN, MEASURED RATHER THAN EXPECTED: on chen
        // the asset states the row for three materials (iris 0.03, brow 0.5, cloth_01 0.5) and the ONE consumer -
        // the stage's parallax block - is gated on the material's matcap, which only the iris has. So the lane is
        // routed correctly and this character's frame does not move by a pixel; the probe that proves the lane is
        // live (the iris re-stated as 0.3) is in `remaining_port_spec.md`'s "extras 数据源" section.
        "_ParallaxScale",
        // THE GOO IRIS BRIGHTNESS LANE'S ROW IS A `color` AND NOT TWO `float`s, which is the shape the lane
        // forces rather than a choice: one lane is one `vec4` and the reference states its two numbers as two
        // SIBLING sockets of one group (`Eyes brightness` / `Eyes HightLight brightness`), read at the same two
        // lines of it. The row's name is the port's own - the reference's sockets are neither `_`-prefixed game
        // properties nor sidecar rows - and `.x` / `.y` are the two sockets IN THE REFERENCE'S OWN ORDER.
        "_GooEyeBrightness",
        // THE REWRITTEN CHAIN'S RIM LANES, and BOTH rows are `color` - which is the shape the reference forces
        // rather than a choice: `Rim_Color` is an RGBA socket, and `Rim_ColorStrength` / `Rim_DirLightAtten` /
        // `ToonfresnelPow` / `Use Rimlimitation?` are FOUR SIBLING sockets of the same group instance, read at
        // one composition, so one `vec4` row carries them in the reference's own order. `.w` is a BOOLEAN's
        // 0/1, which a `color` row states as a fourth float without loss.
        //
        // THE NAMES ARE THE PORT'S OWN, on `_GooEyeBrightness`'s terms: the reference's sockets are neither
        // `_`-prefixed game properties nor sidecar rows. They are spelled with the reference's own capitalisation
        // of `Rim` so a reader can find the socket they came from.
        "_GooRimColour",
        "_GooRimScalars",
        // THE SCREEN-SPACE RIM'S TWO WIDTHS, and they are THEIR OWN ROW rather than two more floats on
        // `_GooRimScalars` because they belong to the OTHER group the same material instantiates: the container
        // reads `Rim_ColorStrength` / `Rim_DirLightAtten` from its own interface, while `Rim_width_X` /
        // `Rim_width_Y` are `DepthRim`'s two `组输入` sockets, reached through `群组.016`. `.z` / `.w` are
        // reserved. See `toon_colour_lane::goo_rim_widths`.
        "_GooRimWidths",
        // ---- STEP 4: THE BASE / SKIN / CLOTH DIRECT-DIFFUSE REPLACEMENT'S SIX LANES ----
        //
        // ITS OWN `BaseColor` FIRST, and the row name is the REFERENCE'S OWN - `组输入.BaseColor` of
        // `Arknights: Endfield_PBRToonBase`. It is a `color` row in the material tree (`[1.1628, 0.9888, 1.0280]`
        // on `body_01`), so `.x` / `.y` / `.z` are the tint and nothing else is read; the reason the port needs it
        // at all is that its own albedo carries no `baseColorFactor` (see `toon_colour_lane::goo_base_colour`).
        "_GooBaseColour",
        // THE FOUR `SigmoidSharp` ARGUMENTS, in the order the reference's two call sites state them, and the
        // ordering is the lane's contract rather than a preference: `.x` / `.y` are the half-Lambert curve's
        // `center` / `sharp` and `.z` / `.w` the cast-shadow curve's.
        "_GooDiffuseA",
        // THE GATE'S LOWER EDGE AND ITS TWO NEIGHBOURS ON THE SAME MATERIAL ROW. `.y` HAS NO CONSUMER THIS STEP
        // and the row is still named here, because this table's job is to be the ONE place the asset pipeline's
        // spelling lives - see `toon_colour_lane::goo_diffuse_b` for why the value is recorded rather than applied.
        "_GooDiffuseB",
        // THE TOONFRESNEL PAIR: each row is a `color` whose `.rgb` is one side of the reference's `混合.006` MIX and
        // whose `.w` is one end of the window its factor is a `smoothstep` of. `ToonfresnelPow` - the exponent
        // between them - is NOT here: it rides `_GooRimScalars.z` already, and one socket must have one carrier.
        "_GooFresnelInside",
        "_GooFresnelOutside",
        // AND THE DIRECT-OCCLUSION COLOUR, whose neutral is BLACK rather than white because black is what the
        // reference's `lerp(black, white, AO)` starts from (see `toon_colour_lane::goo_direct_occlusion`).
        "_GooDirectOcclusion",
        // ---- STEP 5: THE REFERENCE'S DIRECT-SPECULAR / IBL-DIFFUSE / IBL-SPECULAR TERMS' FOUR LANES ----
        //
        // THE FOUR PER-MATERIAL NUMBERS THOSE TERMS CONSUME AND THE PORT COULD NOT CARRY, each one the
        // REFERENCE'S OWN socket name (spec `goo_step5_specular_spec.md` §5.2/§7.3):
        //
        //   * `specularFGD Strength` - the IBL specular's strength (`0.8` on `body_01`/`body_02`, the group's
        //     `1.0` on the cloth, and NO SOCKET AT ALL on the Face and Hair containers);
        //   * `dirLight_lightColor` - THE LIGHT COLOUR OF THE DIRECT TERMS, which the spec's §5.4 makes
        //     authoritative over Goo's `Shader Info` and which closes step 4's own recorded substitution (its
        //     result document §4 item 2). It is a `color` row with a 4.2% blue/green pull on every material;
        //   * `AmbientLightColorTint` - what the IBL diffuse multiplies the engine's probe irradiance by
        //     (`[1.5121498107910156] x3` on the body, white on the cloth).
        //
        // ... AND A FOURTH, `SpecularColor`, whose absence would leave the direct specular 4.2x too dark on
        // `body_01`/`body_02` (spec §7.3 item 5). It is NOT `BaseColor` reused: the two are independent sockets of
        // the same group instance and disagree by 3.6x on the body, and `cloth_02`'s specular colour is darker than
        // its base colour while the body's is brighter - see `toon_colour_lane::goo_specular_color`.
        //
        // THE ROW NAMES ARE THE REFERENCE'S SOCKET NAMES, which is why they have no leading underscore: every other
        // entry above is a GAME property (`_BaseColor`, `_Specular`) or a name this port minted for a reference
        // socket it had to name itself (`_GooRimColour`). These four are socket names the reference's own group
        // carries verbatim, so a reader can grep `nodes.json` for them - and `_GooBaseColour` above set that
        // precedent for `BaseColor`'s row.
        "_GooSpecularFGD",
        "_GooLightColor",
        "_GooAmbientTint",
        "_GooSpecularColor",
        // ---- STEP 7: THE FACE CONTAINER'S FOUR LANES ----
        //
        // THE ROW NAMES ARE THE PORT'S OWN (the reference's sockets are neither `_`-prefixed game properties nor
        // sidecar rows), on `_GooEyeBrightness`'s terms - and the two scalar rows carry FOUR SIBLING SOCKETS each,
        // packed in the reference's own order, which is the shape one `vec4` forces and the same packing
        // `_GooDiffuseA` / `_GooDiffuseB` already use:
        //
        //   * `_GooFaceScalarsA` = `chin_RemaphalfLambert_center`[x] / `_sharp`[y] / `sphereNormal_Strength`[z] /
        //     `SmoothnessMax`[w] - the chin `SigmoidSharp` pair, the sphere-normal blend and the roughness source;
        //   * `_GooFaceScalarsB` = `Face Final brightness`[x] / `Eyes white Final brightness`[y] / `Front R
        //     Pow`[z] / `Front R Smo`[w] - the emission-brightness switch's two arms and `Front transparent
        //     red`'s shape;
        //   * `_GooFaceNoseShadow` = `nose_shadow_Color` and `_GooFaceFrontR` = `Front R Color`, both `color` rows
        //     whose NEUTRAL IS BLACK because black is the socket's own `interface[]` default (see the lane's note
        //     in `deren::vulkan::toon_colour_lane`).
        //
        // THE AUDIT THAT KEPT THIS AT FOUR ROWS: eight more of the container's numbers already have carriers,
        // filled from the SAME SOCKET NAMES by the same materials - `BaseColor` on `_GooBaseColour`, the two
        // `SigmoidSharp` pairs and `MetallicMax` on `_GooDiffuseA` / `_GooDiffuseB`, `dirLight_lightColor` /
        // `AmbientLightColorTint` / `SpecularColor` on step 5's three, `Rim_Color` on `_GooRimColour`, and
        // `Color desaturation in shaded areas attenuation` on `_GooDiffuseB.y` (which step 6 taught to
        // desaturate). Minting rows for those would give one socket two carriers.
        "_GooFaceScalarsA",
        "_GooFaceScalarsB",
        "_GooFaceNoseShadow",
        "_GooFaceFrontR",
        // ---- STEP 8: THE NORMAL DECODE'S STRENGTH ----
        //
        // ONE ROW, ONE SCALAR, and it is the reference's `DecodeNormal :: 组输入.NormalStrength`: the weight between
        // the FLAT tangent normal and the one the map's `xy` decodes to. It is a `float` row rather than a `color`
        // one (the port's rule for a lane whose payload is `.x` alone), so it does NOT go through the generic
        // `others` path below - its `float` kind lands in `material_sidecar::scalars`, which only `scalar()` reads,
        // and that is why `toon_colour` has a branch of its own for it, the way `_Specular` and `_ParallaxScale` do.
        //
        // THE VALUES ARE THE REFERENCE'S, NOT THE GLTF'S: `normalTexture.scale` is `1.0` on all 11 materials of
        // `chars\laevatain_goo.glb`, so it cannot be the source (the `_Specular` lane's `extras` precedence exists
        // for a property the asset really states; this one it does not).
        "_GooNormalStrength",
        // ---- STEP 10: THE ANISOTROPY GATE ----
        //
        // ONE ROW FOR THREE SIBLING SOCKETS OF `PBRToonBase`, in the reference's own order, which is the shape
        // `_GooRimScalars` and `_GooEyeBrightness` already use and the shape one `vec4` forces: `.x = Use
        // anisotropy?`, `.y = Anisotropic mask`, `.z = Use Toonaniso?`, `.w` reserved. `混合.016` (the direct
        // specular's arm selector) reads the first, `混合.017` multiplies by the second, and `混合.020` - the
        // anisotropic lobe - switches on the third, which step 12 implemented (the lobe's own roughnesses ride
        // the lane below); the mask is `0.0` on every material of this asset, so none of it is reachable in a
        // frame this repository renders.
        //
        // IT IS A `color` ROW AND NOT A `float` ONE, so it needs NO branch in `toon_colour`: unlike step 8's
        // `_GooNormalStrength` (whose `float` kind lands in `material_sidecar::scalars` and is therefore invisible to
        // the generic `others` path below), this row parses through that path like every other colour lane, and its
        // four comma-separated components arrive component by component.
        "_GooAnisoGate",
        // ---- STEP 12: THE ANISOTROPIC LOBE'S TWO ROUGHNESSES ----
        //
        // ONE `color` ROW FOR THE TWO SIBLING SOCKETS `roughnessT` / `roughnessB` ARE BUILT FROM, spelled
        // `Aniso_SmoothnessMaxT` / `Aniso_SmoothnessMaxB` in the reference's own graph (`.x` / `.y`, in that
        // order; `.z` / `.w` reserved). The port's lobe computes `rT = (1 - lane.x)^2`, `rB = (1 - lane.y)^2`,
        // which is the two `Power` nodes the snapshot calls `roughnessT` / `roughnessB`.
        //
        // IT IS THE SAME KIND OF ROW AS `_GooAnisoGate` ABOVE AND TAKES THE SAME PATH, so it needs no branch in
        // `toon_colour`: a `color` row lands in `material_sidecar::others`, which is what the generic tail of that
        // lambda reads, and the four comma-separated components arrive one at a time. Its neutral is `(0,0,0,0)`
        // - the reference's own group default rather than a sentinel - see `toon_colour_lane::goo_aniso_rough`.
        "_GooAnisoRough",
        // ---- STEP 13: THE TWO `RS EFF` LANES ----
        //
        // `_GooRSScalars` is `.x Use RS_Eff?` / `.y RS Multiply Value` / `.z RS Model` and `_GooRSTint` is
        // `.rgb RS ColorTint` / `.w SmoothStep.max` (mechanism table #14). BOTH ARE `color` ROWS AND BOTH TAKE THE
        // GENERIC TAIL OF `toon_colour`, so this table is the whole host-side change: they land in
        // `material_sidecar::others` and their four comma-separated components are parsed one at a time, exactly
        // as `_GooAnisoGate`'s and `_GooAnisoRough`'s are - no branch, no sentinel, no new sidecar grammar.
        //
        // THEY ARE IN THIS TABLE BECAUSE THE TABLE *IS* THE BINDING: the lambda derives the row name from the LANE
        // and the lane from the enum, so an enum lane with no name here makes every `_GooRSScalars` /
        // `_GooRSTint` row in every sidecar unreachable - the lookup finds nothing, falls back to
        // `toon_colour_neutral` (zero), and the RS branch is off with no warning and no compile error.
        //
        // THE NEUTRAL OF BOTH IS `(0,0,0,0)`, which is a value and not a placeholder: `.x` of lane 27 is the
        // branch's switch, and zero there is what makes the stage's identity BITWISE (see the `float3
        // rs_final = lit;` / `if (rs_use > 0.0f && ...)` pair in `shaders/goo_toon.slang`, which does no
        // floating-point work at all when the gate is shut).
        "_GooRSScalars",
        "_GooRSTint",
        // `_GooRSArm0` IS THE SAME MECHANISM'S OTHER ARM, and it is a COLOUR row rather than four `float` rows for
        // the reason `_GooRSScalars` is one: it is four numbers (`RS_Index`, `RS Strength`, `Layer weight Value`,
        // `Layer weight Value Offset`) off four `组输入` sockets of the same group, and the sidecar vocabulary has a
        // four-component form for exactly that. Its neutral is `(0,0,0,0)`, which is ALSO the arm's off switch
        // (`RS Strength = 0` zeroes the product), so a material that states no row gets an `armA` that changes
        // nothing - but note it is not the same as `Use RS_Eff? = 0`: a zeroed `armA` still passes through the
        // LIGHTEN below. See `toon_colour_lane::goo_rs_arm0` and the `armA` block in `shaders/goo_toon.slang`.
        "_GooRSArm0",
    }};
    // The declared flag for a toon lane; the `_Use<Slot>` convention for every OTHER slot, which the diagnostic
    // needs because it walks the whole file (`_BaseMap`, `_BumpMap`, the outline and SDF masks and the rest).
    auto const toon_flag_for = [](std::string_view const slot_name) -> std::string {
        for (toon_lane_names const& lane : toon_lane) {
            if (lane.slot == slot_name) {
                return std::string(lane.flag);
            }
        }
        std::string flag{deren::toon::enable_flag_prefix};
        flag.append(slot_name.starts_with('_') ? slot_name.substr(1) : slot_name);
        return flag;
    };
    // ... AND THE ONE `float` ROW THAT IS NOT A LANE SWITCH AND NOT A COLOUR, named here because the diagnostic
    // below walks the SIDECAR rather than the lane table and will therefore report it: `_GooRampIndex` is the
    // reference's `RampIndex` socket, written beside its resolved name (`_GooBaseRamp`) so that the choice the
    // host made can be re-derived from the file. Its synthetic flag is `_UseGooRampIndex`, which no asset states,
    // so the diagnostic prints it `off` - which is TRUE of the flag and says nothing about the ramp (the ramp's
    // switch is `_UseGooBaseRamp`). It has no colour row either, so `toon_colour` below never parses it.

    std::optional<deren::toon::sidecar> toon_sidecar = {}; // kept in scope: the import below is what consumes it
    {
        auto const sidecar = deren::toon::load_sidecar(model_path);
        if (!sidecar.has_value()) {
            deren::utility::log("toon sidecar: NOT READ - {}", sidecar.error());
        } else if (sidecar->empty()) {
            deren::utility::log("toon sidecar: none beside '{}' (the normal case for a model that is not a character)", model_path);
        } else {
            toon_sidecar = *sidecar;
            deren::utility::log("toon sidecar: {} material(s) described ({} line(s) skipped)", sidecar->materials.size(), sidecar->skipped_lines);
            // A NON-ZERO MERGE COUNT IS A FACT ABOUT THE FILE AND NOT A COMPLAINT: it says some material's rows
            // arrived in more than one block, which the reader merges by name (see `sidecar::merged_rows`). It is
            // logged because the OPPOSITE reading - "this material states nothing" - is what a reader that did not
            // merge would silently report, and the two are indistinguishable in every other line of this output.
            if (sidecar->merged_rows > 0) {
                deren::utility::log("toon sidecar: {} row(s) belong to a material whose rows are NOT CONTIGUOUS - merged into its entry by name", sidecar->merged_rows);
            }
            for (deren::toon::material_sidecar const& material : sidecar->materials) {
                // THE FAMILY COMES FROM THE LOADER'S CLASSIFIER over the SAME name, so the sidecar (which
                // supplies the parameters) and the renderer (which selects them) cannot disagree about which
                // family a material is: there is one classifier and both sides ask it.
                deren::utility::log("  '{}' -> family {} | {} slot(s), {} scalar(s)", material.name, static_cast<uint32_t>(deren::gltf::toon_family_of(material.name)), material.slots.size(), material.scalars.size());
                // STEP 8'S `float` ROW IS PRINTED BY NAME, because the loop below walks `material.slots` ONLY: a
                // `float` property is counted in the `{} scalar(s)` above and never named, so a human reading this
                // log could not tell "the file states nothing" from "the reader dropped the row" - the two look
                // identical in every other line. `ABSENT` means the chain's previous normal stands for this
                // material; a value means lane 24 carries it into the Goo chain's decode (see
                // `toon_colour_lane::goo_normal_strength`).
                if (auto const normal_strength = material.scalars.find("_GooNormalStrength"); normal_strength != material.scalars.end()) {
                    deren::utility::log("      _GooNormalStrength = {:.10g} | lane 24 (the reference's DecodeNormal strength)", static_cast<double>(normal_strength->second));
                } else {
                    deren::utility::log("      _GooNormalStrength = ABSENT | lane 24 keeps the chain's previous normal");
                }
                // LANE 16'S `float` ROW IS PRINTED BY NAME FOR THE SAME REASON, and here the distinction is the
                // whole of debt (u): `ABSENT` makes the stage answer the reference's own group default `1.0`, while
                // a value is what this lane exists to carry. The two body materials state `0.7999999523162842` and
                // every cloth states `1.0`, so a reader that sees `ABSENT` for them is looking at D1.
                if (auto const specular_fgd = material.scalars.find("_GooSpecularFGD"); specular_fgd != material.scalars.end()) {
                    deren::utility::log("      _GooSpecularFGD = {:.10g} | lane 16 (the reference's `specularFGD Strength`)", static_cast<double>(specular_fgd->second));
                } else {
                    deren::utility::log("      _GooSpecularFGD = ABSENT | lane 16 answers the reference's group default 1.0");
                }
                for (auto const& [slot_name, texture_name] : material.slots) {
                    std::optional<uint16_t> const index = scenes->texture_index_by_name(texture_name);
                    deren::utility::log("      {} = '{}' -> {} | {}", slot_name, texture_name, index.has_value() ? std::format("texture #{}", *index) : std::string("ABSENT from this model"), material.enabled_by_flag(toon_flag_for(slot_name)) ? "ON" : "off");
                }
            }
        }
    }

    // 9. IBL stage 2 + material resolve run concurrently via their _async wrappers: the
    //    prefilter (GGX importance sampling), irradiance map, BRDF LUT and the per-material
    //    texture decode + mip chains only depend on what we already have (env, scenes). The
    //    per-stage times are not reported individually: get() orders the waits, so only the
    //    wall-clock of the parallel stage is meaningful (the other tasks hide under the
    //    slowest one).
    deren::utility::log("generating IBL (prefilter/irradiance/BRDF LUT) + resolving materials...");
    auto const stage2_start = std::chrono::steady_clock::now();
    auto prefilter_future = deren::vulkan::prefilter_environment_async(env, env_size, env_mip_count);
    auto irradiance_future = deren::vulkan::generate_irradiance_map_async(env, env_size, irr_size);
    auto lut_future = deren::vulkan::generate_brdf_lut_async(lut_size);
    auto resolve_future = deren::gltf::resolve_materials_async(*scenes);

    std::vector<float> const prefiltered = prefilter_future.get();
    std::vector<float> const irradiance = irradiance_future.get();
    std::vector<float> const brdf_lut = lut_future.get();
    std::vector<deren::gltf::resolved_material> const materials = resolve_future.get();
    auto const stage2_done = std::chrono::steady_clock::now();
    deren::utility::log("  IBL (prefilter/irradiance/BRDF LUT) + material resolve, parallel wall: {:.1f} ms", std::chrono::duration<double, std::milli>(stage2_done - stage2_start).count());

    std::vector<uint8_t> const env_bytes = deren::vulkan::to_half_rgba(prefiltered);
    std::vector<uint8_t> const irr_bytes = deren::vulkan::to_half_rgba(irradiance);
    std::vector<uint8_t> const lut_bytes = deren::vulkan::to_half_rg(brdf_lut);

    // 10. Upload the scene-wide IBL once: shared by every primitive (bindings 2-4 of the scene block)
    runtime.set_ibl(deren::vulkan::ibl_input{.prefiltered_env = env_bytes, .irradiance = irr_bytes, .brdf_lut = lut_bytes, .env_size = static_cast<uint32_t>(env_size), .env_mip_count = static_cast<uint32_t>(env_mip_count), .irr_size = static_cast<uint32_t>(irr_size), .lut_size = static_cast<uint32_t>(lut_size)});

    // ---- THE ARTICLE'S POST LUT (`ZmdLutPost.shader`'s `_LutTex`), BAKED NEUTRAL ----
    //
    // A `1024x32` strip of 32 `32x32` tiles holding a `32^3` cube: the layout the article's own addressing walks
    // (blue picks the slice, red and green sit inside the tile, the tile's rows run bottom-up). THE CONTENT IS THE
    // IDENTITY UNDER THAT ADDRESSING rather than a placeholder, and the identity means INVERTING the article's
    // domain mapping: the shader encodes the frame with
    // `log2(c * 5.55555582 + 0.0479959995) * 0.0734997839 + 0.386036009` and looks THAT up, so the texel at address
    // `a` has to hold the colour `c` that encodes to `a` - `decode(a)`, below.
    //
    // AND IT IS STORED sRGB-ENCODED, because the lane is an `R8G8B8A8_SRGB` image and the sampler DECODES what it
    // reads: storing `srgb_encode(decode(a))` is what makes the shader receive `decode(a)` itself. That is the same
    // convention the shadow LUT's bake states for its own lane, and it is the difference between a neutral cube and
    // a brighter-than-neutral one.
    //
    // WHY IT IS BAKED RATHER THAN SHIPPED: the ramps, the shadow LUT and the matcap are all baked here for the same
    // reason - the lane then reads without changing the frame, so the shader's weight can default to the article's
    // 1.0 with no artist's cube present, and an artist's cube replaces the bake the day one exists.
    {
        constexpr uint32_t post_lut_width = 1024u;
        constexpr uint32_t post_lut_height = 32u;
        constexpr uint32_t post_lut_tiles = 32u;
        auto const srgb_encode = [](float const linear) {
            return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        };
        std::vector<uint8_t> post_lut(static_cast<std::size_t>(post_lut_width) * post_lut_height * 4u, 255u);
        for (uint32_t row = 0; row < post_lut_height; ++row) {
            for (uint32_t column = 0; column < post_lut_width; ++column) {
                float const address[3] = {
                    // THE AXES ARE THE ARTICLE'S, AND THEY ARE NOT THE SHADOW LUT'S - the trap this bake walked into
                    // first: `xOffset = halfColX + lut_input.r * threshold / colors` puts RED in the tile's own X,
                    // `yOffset = 1 - (halfColY + lut_input.g * threshold)` puts GREEN in the tile's Y (flipped, because
                    // v grows upward while the strip's rows grow downward), and the slice term `slice / colors` puts
                    // BLUE in the TILE INDEX. The shadow LUT's bake next door assigns those axes differently (tile =
                    // red, in-tile X = green, row = blue), and copying its layout wrote a cube whose red and blue were
                    // swapped - measured as 99.45% of the frame moving when the lookup was switched on against what
                    // was supposed to be the identity.
                    static_cast<float>(column % post_lut_tiles) / static_cast<float>(post_lut_tiles - 1u),
                    static_cast<float>(post_lut_tiles - 1u - row) / static_cast<float>(post_lut_tiles - 1u),
                    static_cast<float>(column / post_lut_tiles) / static_cast<float>(post_lut_tiles - 1u),
                };
                uint8_t* const texel = post_lut.data() + (static_cast<std::size_t>(row) * post_lut_width + column) * 4u;
                for (int32_t c = 0; c < 3; ++c) {
                    float const decoded = (std::exp2((address[c] - 0.386036009f) / 0.0734997839f) - 0.0479959995f) / 5.55555582f;
                    texel[c] = static_cast<uint8_t>(std::clamp(srgb_encode(std::clamp(decoded, 0.0f, 1.0f)), 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                texel[3] = 255u;
            }
        }
        runtime.set_post_lut(post_lut, post_lut_width, post_lut_height);
    }

    // ---- THE GOO REFERENCE'S PRE-INTEGRATED FGD LUT: A GLOBAL IMAGE, UPLOADED ONCE, FROM A FILE ----
    //
    // WHY THIS ONE IS LOADED FROM DISK WHEN EVERY OTHER TOON LOOKUP IN THIS FILE IS EITHER BAKED OR INJECTED INTO
    // THE GLB, and the reason is the step-5 spec's §3.4 architecture ruling read the other way round: the bake
    // functions above exist for images THIS PORT CAN COMPUTE (`smoothstep`, an identity cube, black), and the spec
    // proves by measurement that these three FGD numbers are NOT a standard analytic form - the R channel is not
    // Karis' `scale` at any axis mapping (ratio 0.00 to 1.01 and non-monotone), the G channel is 5-35x the fit's
    // `bias` with no overlap in range, and the B channel differs from a 120k-sample Disney diffuse FGD by 2.4x
    // (§3.3). So a bake here would be the port substituting its own invented curve for the reference's authored
    // data - the one thing a port may not do - and the reference's own PNG is what travels.
    //
    // WHY IT DOES NOT GO THROUGH THE SIDECAR / `toon_slot` EITHER: it is ONE image shared by three containers with
    // a coordinate computed from shading parameters, not from a material (see `heap_slots_goo_fgd_lut`), so a lane
    // per material would be eleven rows pointing at one file. This is the "自持的全局纹理 + 自持的 heap 槽" the
    // spec's §3.4 names, and the slot is `core::heap_slots::goo_fgd_lut` (754).
    //
    // THE PATH IS RESOLVED FROM THE EXECUTABLE'S OWN DIRECTORY, not from the process's working directory: a
    // capture runs with `WorkingDirectory` set to its `-WorkDir` (see `scripts/windows/capture.ps1`), so a
    // CWD-relative path would miss. Nothing here is a new path *convention*: this file is a data file that ships
    // beside the build's own `shaders/` and `chars/` directories, and `deren-ab/` is where the reference's assets
    // live in this repository.
    //
    // A MISSING FILE IS LOGGED AND NOT FATAL, deliberately: with no LUT uploaded, the slot holds no descriptor and
    // `heap_texel` reads zero, which makes the step-5 arm's FGD terms zero - a frame that is WRONG in a way the log
    // names, rather than a crash in a system whose other 40 features are fine. The three terms only exist inside
    // the `goo_arm` branch, so `goo_toon = false` is unaffected either way.
    {
        std::vector<uint8_t> fgd_file = {};
        // `deren::utility::executable_directory()` AND NOT `current_path()`, which is the fix for a measured failure
        // rather than a preference: a capture runs with its working directory set to the harness's `-WorkDir`
        // (see `scripts/windows/capture.ps1`'s launch), so `current_path()` resolved to
        // `...\deren-ab\laevat\deren-ab\gooblender\images\...` and EVERY step-5 frame was rendered with no LUT
        // uploaded at all - the log said so (`goo FGD LUT: NOT uploaded`) and the three FGD terms read zero.
        // The executable's own directory is `build-release-clang64/`, which is where `deren-ab/` and the
        // reference's assets live, so the path is right whatever the process's working directory is.
        std::filesystem::path const fgd_path = deren::utility::executable_directory() / "deren-ab" / "gooblender" / "images" / "PreIntegratedFGD_GGXDisneyDiffuse.png";
        if (!fgd_path.empty()) {
            std::ifstream file(fgd_path, std::ios::binary);
            if (file) {
                fgd_file.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
        }
        int32_t fgd_width = 0;
        int32_t fgd_height = 0;
        int32_t fgd_channels = 0;
        stbi_uc* const fgd_pixels = fgd_file.empty()
                                        ? nullptr
                                        : stbi_load_from_memory(fgd_file.data(), static_cast<int32_t>(fgd_file.size()), &fgd_width, &fgd_height, &fgd_channels, 4); // 4 = force RGBA8
        if (fgd_pixels == nullptr) {
            deren::utility::log("goo FGD LUT: NOT uploaded - '{}' is missing or not a readable PNG. The step-5 arm's FGD terms "
                                "will read zero (see heap_slots_goo_fgd_lut); everything else is unaffected.",
                                fgd_path.string());
        } else {
            // THE BYTES ARE HANDED OVER VERBATIM AND THAT IS THE WHOLE POINT OF THE UPLOAD'S FORMAT: the
            // reference's image data-block is `colorspace = 'Non-Color'`, so Blender does not linearize it and the
            // node graph reads the raw texels. `set_goo_fgd_lut` therefore creates an `R8G8B8A8_UNORM` image, and
            // NOT the `_SRGB` one `set_post_lut` next door creates - an sRGB upload would decode all three FGD
            // outputs once and move the highlight and the ambient (spec §3.1 item 1).
            runtime.set_goo_fgd_lut(std::span<uint8_t const>(fgd_pixels, static_cast<std::size_t>(fgd_width) * static_cast<std::size_t>(fgd_height) * 4u),
                                    static_cast<uint32_t>(fgd_width),
                                    static_cast<uint32_t>(fgd_height));
            // The decode is `STB_IMAGE_STATIC`'s own allocation and is freed here rather than kept: the upload
            // copies into a device-local image, so nothing downstream reads these bytes.
            stbi_image_free(fgd_pixels);
        }
    }

    // 11. Batch-import: the runtime drives the traversal itself through two aligned loader
    //     streams — the retained node hierarchy (deren::gltf::scene_node_iterator: DFS pre-order,
    //     transform-only nodes included, name + local transform per node) and the drawables
    //     of those nodes (deren::gltf::drawable_iterator: geometry/material getters, node-aligned).
    //     The runtime rebuilds the scene tree (node per loader node) and attaches each
    //     drawable as a leaf primitive under its node, so whole-group transforms work on the
    //     imported hierarchy. The orbit camera looks at the origin, so center the scene and
    //     pull it back to fit its radius (same framing as the old single-model fit).
    //
    //     The scene tree is CALLER-OWNED: main declares it (AFTER the runtime, so C++ reverse
    //     declaration order destroys it BEFORE the runtime — the leaves' GPU buffers release
    //     through the runtime's vma allocator while it is still alive) and binds it with
    //     set_scene() before any import.
    deren::vulkan::scene_tree::scene scene;
    runtime.set_scene(scene);
    // Initial camera framing ([render] camera_fit). "exterior" is the historic fit: 2.75 scene radii
    // frames a compact object, and for that it stays the default. It is also useless for a building -
    // the camera ends up well outside its own walls, so the frame is a facade with no interior to
    // bounce light in, which is why GI work needs the other mode. "interior" stands inside and looks
    // along the LONGEST HORIZONTAL AXIS, because that is the axis a hall, nave or corridor runs down
    // (for Sponza: 29.8 x 18.3 in the ground plane, so the look direction is +X). The distance is a
    // fraction of the SMALLER horizontal half-extent, which is what keeps the eye inside even a
    // narrow hall while still leaving enough parallax for the view to read as a space.
    {
        glm::vec3 const half_extent = (bounds.max - bounds.min) * 0.5f;
        if (settings.render.camera_pose_set) {
            // A pinned pose from the config: the same six numbers `--capture-camera` takes, so a view a user
            // reports (printed by the F12 block below) can be pasted straight back in here.
            runtime.camera.yaw = glm::radians(settings.render.camera_pose[0]);
            runtime.camera.pitch = glm::radians(settings.render.camera_pose[1]);
            runtime.camera.distance = settings.render.camera_pose[2];
            runtime.camera.target = glm::vec3(settings.render.camera_pose[3], settings.render.camera_pose[4], settings.render.camera_pose[5]);
        }
        bool const interior = settings.render.camera_fit == "interior";
        if (settings.render.camera_pose_set) {
            // the pinned pose above already decided everything; the fit's numbers would overwrite it
        } else if (interior) {
            runtime.camera.yaw = half_extent.x >= half_extent.z ? glm::radians(90.0f) : 0.0f;
            runtime.camera.pitch = 0.0f;
            runtime.camera.distance = std::min(half_extent.x, half_extent.z) * 0.7f;
        } else {
            runtime.camera.distance = scene_radius * 2.75f;
        }
        deren::utility::log("initial camera: fit={} yaw {:.1f} deg, pitch {:.1f} deg, distance {:.2f} (scene radius {:.2f}, ground half-extent {:.2f} x {:.2f})",
                            settings.render.camera_fit,
                            glm::degrees(runtime.camera.yaw),
                            glm::degrees(runtime.camera.pitch),
                            runtime.camera.distance,
                            scene_radius,
                            half_extent.x,
                            half_extent.z);
    }
    // ONE place that turns the live pose into the six numbers the config and the command line take: the F12
    // screenshot prints it (so a bug report carries its own camera) and so does the exit below.
    auto const log_camera_pose = [&runtime](char const* const why) {
        deren::utility::log("camera pose ({}): {:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}   -> [render] camera_pose = [...] or --capture-camera",
                            why,
                            glm::degrees(runtime.camera.yaw),
                            glm::degrees(runtime.camera.pitch),
                            runtime.camera.distance,
                            runtime.camera.target.x,
                            runtime.camera.target.y,
                            runtime.camera.target.z);
    };

    deren::gltf::scene_node_iterator const node_first = scenes->nodes_begin();
    deren::gltf::scene_node_iterator const node_last;
    deren::gltf::drawable_iterator const scene_first(*scenes, materials);
    deren::gltf::drawable_iterator const scene_last;
    glm::vec3 const scene_import_shift = -scene_center + scene_sink;

    // ---- THE TOON LOOKUP: the one place the sidecar reader and the renderer meet ----
    // `vulkancorekit` deliberately does not depend on `gltf_loader` (see that target's note: the engine is
    // loader-agnostic), so the runtime cannot hold a sidecar and must not learn what one is. What it CAN ask for
    // is "the texture input for this material and this lane", and this application is the layer that links both
    // - so this is where they are joined.
    struct toon_lookup_state {
        deren::toon::sidecar const* sidecar = nullptr;
        deren::gltf::scenes const* scenes = nullptr;
        /// the PROCEDURALLY BAKED neutral unit ramp (see the baker below), kept alive here because the texture
        /// input the lookup returns is a SPAN INTO IT and `register_material` reads it during the import. ONE
        /// buffer serves BOTH ramp lanes: the diffuse ramp and the specular ramp are the same neutral step, and
        /// what tells them apart is which family numbers the shader moves it with
        std::vector<uint8_t> baked_unit_ramp = {};
        uint32_t baked_ramp_width = 0;
        uint32_t baked_ramp_height = 0;
        /// the DIFFUSE ramp lane's neutral content (see the baker below) - a different SHAPE from the unit step
        /// above, because the two lanes read their ramp differently: one as a threshold, this one as a colour
        /// mapping plus a lightness
        std::vector<uint8_t> baked_neutral_ramp = {};
        uint32_t baked_neutral_ramp_width = 0;
        uint32_t baked_neutral_ramp_height = 0;
        /// the SHADOW LUT cube (see the baker below) - a different shape from the ramp and so a different
        /// buffer, kept alive for the same reason: the lookup hands out a SPAN INTO IT
        std::vector<uint8_t> baked_shadow_lut = {};
        uint32_t baked_lut_width = 0;
        uint32_t baked_lut_height = 0;
        /// the MATCAP ball (see the baker below) - a third shape again, and a black one, because this stage had
        /// no matcap term to reproduce
        std::vector<uint8_t> baked_matcap = {};
        uint32_t baked_matcap_size = 0;
        /// the METALLIC/GLOSS lane has NO neutral here on purpose - see the note where its baker used to be.
        /// A material with no such map is shaded by its family's own `roughness` / `reflectivity`.
    };

    // ---- THE PROCEDURAL RAMP, which is what replaces the game's own ramps on the read path ----
    //
    // WHY IT IS BAKED RATHER THAN SAMPLED FROM THE MODEL: the ramps a character ships with are the GAME's
    // textures, and `ASSET_LICENSE_BOUNDARY_CN.md` excludes those from redistribution - so a read path that
    // depends on them is a read path this repository cannot carry. This bakes an equivalent SHAPE instead: a flat
    // shadow side, a step, a flat lit side, which is what a toon ramp IS (see character_forward.slang's note on
    // why a ramp replaces the procedural threshold rather than layering with it).
    //
    // ONE ASSET SERVES BOTH LANES, the diffuse ramp and the specular ramp. The reference keeps them in two V
    // bands of one atlas for exactly this reason - they are the same step read with two different coordinates,
    // one from the shadow-gated half-Lambert and one from the half-vector angle - and a neutral step is the
    // shape both of them need. Two files would be two things to keep in step for no gain.
    //
    // THE BAKE IS A NEUTRAL UNIT STEP, NOT ANY ONE FAMILY'S RAMP, and that is what lets a SINGLE redistributable
    // asset serve every family without copying the family table into this file. The texture holds the SHAPE -
    // zero through the shadow side, one through the lit side, a step at x = 0.5 - and the SHADER maps the
    // family's own threshold and edge width onto that step when it builds the coordinate it reads at (see
    // `character_ramp_half_width` in character_forward.slang). The family's numbers therefore stay in ONE place,
    // in the shader, where the procedural branch and the rim stage already read them; what this file supplies is
    // the shape, and what the shader supplies is where along it this material's terminator sits.
    //
    // THAT IS NOT A WORKAROUND FOR THE TABLE BEING IN THE SHADER, it is the structure the reference uses: its
    // ramp atlas is shared across materials and each material's own `_ShadowCenter` / `_ShadowSmoothness` decide
    // the UV it is read at. A per-family BAKE would be the copy - and it would be a worse one, because the
    // family's edge width would be frozen into pixels at load time instead of staying a number the shader can
    // evaluate the procedural branch with.
    //
    // `baked_ramp_half_width` IS HALF OF A SHARED CONTRACT; the other half is the shader's
    // `character_ramp_half_width`. The shader's remap inverts this bake exactly when the two agree, which is
    // what makes the texture branch and the procedural branch produce the same tint for the same family rather
    // than merely similar ones. `tests/test_toon_material_sidecar.cpp` reads both files and fails on drift -
    // and it is there rather than in a test of its own because the contract is a sidecar-lane contract.
    constexpr uint32_t baked_ramp_width = 256;
    constexpr uint32_t baked_ramp_height = 8;
    constexpr float baked_ramp_half_width = 0.035f;
    auto const bake_unit_ramp = []() {
        // `smoothstep(0.5 - w, 0.5 + w, x)`: the same Hermite step the shader's procedural branch builds, with
        // the step moved to the ramp's centre and its width normalised so the remap can undo it.
        auto const srgb_encode = [](float const linear) {
            return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        };
        auto const smoothstep = [](float const edge0, float const edge1, float const x) {
            float const t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        std::vector<uint8_t> pixels(static_cast<std::size_t>(baked_ramp_width) * baked_ramp_height * 4u, 255u);
        for (uint32_t v = 0; v < baked_ramp_height; ++v) {
            for (uint32_t u = 0; u < baked_ramp_width; ++u) {
                float const x = static_cast<float>(u) / static_cast<float>(baked_ramp_width - 1u);
                float const step = smoothstep(0.5f - baked_ramp_half_width, 0.5f + baked_ramp_half_width, x);
                uint8_t* const texel = pixels.data() + (static_cast<std::size_t>(v) * baked_ramp_width + u) * 4u;
                // GREY, and the SHADER READS `.r`: the ramp carries the step and nothing else, so the family's
                // own `shadow_tint` is what colours the dark side rather than a bake that could only ever hold
                // one family's tint. Writing the same value into R, G and B keeps the asset readable as a ramp
                // by eye.
                //
                // THE UPLOAD IS SRGB (see register_material's slot table), so the texel holds the ENCODED value:
                // what the sampler hands the shader is then the LINEAR step, which is the number the procedural
                // branch's `smoothstep` produced. Encoding is what makes the two branches agree on the VALUE and
                // not just on the shape.
                uint8_t const encoded = static_cast<uint8_t>(std::clamp(srgb_encode(step), 0.0f, 1.0f) * 255.0f + 0.5f);
                texel[0] = encoded;
                texel[1] = encoded;
                texel[2] = encoded;
                texel[3] = 255u;
            }
        }
        return pixels;
    };

    // ---- THE DIFFUSE RAMP'S NEUTRAL CONTENT, which is a DIFFERENT SHAPE from the unit step above ----
    //
    // THE TWO LANES READ THEIR RAMP DIFFERENTLY, so one bake can no longer serve both. The specular lane reads
    // `.r` as a THRESHOLD, and the unit step above is exactly that. The diffuse lane reads RGB as a COLOUR mapping
    // and A as a LIGHTNESS (see the shading stage's `toon_diffuse`), and a grey step is wrong in BOTH channels for
    // that use: its alpha is a constant 1, which claims every fragment is fully lit, and its RGB would tint the
    // lit side as though the material's ramp had said so.
    //
    // SO THIS BAKE HOLDS THE ONLY CONTENT THAT MEANS "NO COLOUR MAPPING, AND AS LIT AS THE LIGHT TERM SAYS": WHITE
    // in RGB and the identity lightness in A. A material whose model ships no ramp is then shaded by its own light
    // term rather than by an invented band, which is the same neutrality the other bakes keep - and the real map
    // takes precedence whenever the model has one (`_DiffRampMap` is on every character material in this
    // repository, so the fallback is for fixtures and for models exported without it).
    constexpr uint32_t baked_neutral_ramp_width = 256;
    constexpr uint32_t baked_neutral_ramp_height = 8;
    auto const bake_neutral_ramp = []() {
        auto const srgb_encode = [](float const linear) {
            return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        };
        // WHITE RGB, filled in here and never overwritten: the lane is uploaded as sRGB, so 255 is the encoded
        // form of the linear 1.0 the shading stage needs back - a colour mapping that changes nothing.
        std::vector<uint8_t> pixels(static_cast<std::size_t>(baked_neutral_ramp_width) * baked_neutral_ramp_height * 4u, 255u);
        for (uint32_t v = 0; v < baked_neutral_ramp_height; ++v) {
            for (uint32_t u = 0; u < baked_neutral_ramp_width; ++u) {
                float const x = static_cast<float>(u) / static_cast<float>(baked_neutral_ramp_width - 1u);
                uint8_t* const texel = pixels.data() + (static_cast<std::size_t>(v) * baked_neutral_ramp_width + u) * 4u;
                // ENCODED for the same reason the unit step is: the lane is an sRGB texture, so the shader must
                // get the LINEAR lightness back, and writing `x` raw would hand it the encoded one.
                texel[3] = static_cast<uint8_t>(std::clamp(srgb_encode(x), 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
        return pixels;
    };

    // ---- THE SHADOW LUT, WHICH IS A CUBE RATHER THAN A STEP ----
    //
    // IT IS THE ONE LANE THAT CANNOT REUSE THE UNIT RAMP, and the shape is the reason: a ramp answers "how deep
    // is the shadow here", a shadow LUT answers "what is THIS MATERIAL's colour in shadow" and is indexed by the
    // material's own albedo. That is not a variation on the ramp - it is a function of a different variable -
    // and it is why the reference ships skin and cloth a `1024x32` cube at all rather than another band.
    //
    // THE LAYOUT IS THE REFERENCE'S, TO THE TILE COUNT: 32 horizontal `32x32` tiles holding a `32^3` cube, the
    // tile chosen by the X channel and a bilinear hop between adjacent tiles because X is continuous. The
    // shader's `toon_shadow_lut` inverts this exactly, and `baked_lut_tiles` is HALF OF THAT CONTRACT - the other
    // half is `character_shadow_lut_tiles` - because a bake laid out for a different tile count reads back as a
    // different colour with no other symptom. `tests/test_toon_material_sidecar.cpp` compares the two.
    //
    // THE CONTENT IS THE IDENTITY CUBE, which is the same choice the ramps make and for the same reason: a
    // neutral bake reproduces the look the procedural branch already had (`albedo` in, `albedo` out), so the
    // lane is EXERCISED without the frame being changed by a guess at what the artist meant. What the lane buys
    // is that an AUTHORED cube would be honoured - a real skin palette makes the shadow a function of the skin
    // tone, which no per-family constant can be - and that is not something a neutral bake can substitute for.
    //
    // Note the axes, because they are the ones that invert: X picks the TILE, the in-tile X is the GREEN channel,
    // the in-tile Y is the BLUE channel FLIPPED, and each channel's stored value is `index / (tiles - 1)`. The
    // half-texel offsets are in the SHADER rather than here - the bake writes texel centres, and a reader that
    // sampled corners would be off by half a texel in every direction.
    constexpr uint32_t baked_lut_tiles = 32;
    constexpr uint32_t baked_lut_width = baked_lut_tiles * baked_lut_tiles;
    constexpr uint32_t baked_lut_height = baked_lut_tiles;
    auto const bake_shadow_lut = []() {
        std::vector<uint8_t> pixels(static_cast<std::size_t>(baked_lut_width) * baked_lut_height * 4u, 255u);
        for (uint32_t row = 0; row < baked_lut_height; ++row) {
            for (uint32_t column = 0; column < baked_lut_width; ++column) {
                // The identity cube at this texel's coordinate: the X channel from which tile it is in, the Y
                // channel from where it sits inside that tile, the Z channel from the row - flipped, because the
                // shader reads the cube's Z from the BOTTOM of the strip upward.
                float const x = static_cast<float>(column / baked_lut_tiles) / static_cast<float>(baked_lut_tiles - 1u);
                float const y = static_cast<float>(column % baked_lut_tiles) / static_cast<float>(baked_lut_tiles - 1u);
                float const z = static_cast<float>(baked_lut_tiles - 1u - row) / static_cast<float>(baked_lut_tiles - 1u);
                float const cube[3] = {x, y, z};
                uint8_t* const texel = pixels.data() + (static_cast<std::size_t>(row) * baked_lut_width + column) * 4u;
                for (int32_t c = 0; c < 3; ++c) {
                    // STORED AS THE DISPLAY-SPACE COORDINATE, which is what keeps the identity true now that the
                    // shader indexes this lane by the DISPLAY-SPACE albedo (the game's own domain - see
                    // character_forward.slang's toon_shadow_lut). The lane is uploaded as an sRGB texture, so the
                    // sampler DECODES whatever is stored: storing the encoded coordinate makes the shader receive
                    // the LINEAR value that coordinate means, which is exactly the albedo it looked up. Encoding the
                    // linear coordinate instead - what this bake did before the domain changed - would hand back
                    // `srgb(albedo)` decoded, i.e. a brighter albedo, on every material with no LUT.
                    texel[c] = static_cast<uint8_t>(std::clamp(cube[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                texel[3] = 255u;
            }
        }
        return pixels;
    };

    // ---- THE MATCAP, WHICH IS BLACK, AND THAT IS THE WHOLE OF ITS NEUTRALITY ----
    //
    // THE THIRD SHAPE AND THE THIRD VARIABLE: a ramp is indexed by the shading term, the shadow LUT by the
    // material's albedo, and a matcap by the VIEW-SPACE NORMAL - `normalVS.xy * 0.5 + 0.5`, which is a sphere
    // map's own UV and the reason a matcap image is a picture of a ball.
    //
    // BLACK IS NOT A PLACEHOLDER, IT IS THE ONLY CONTENT THAT IS NEUTRAL HERE, and the difference from the other
    // two lanes is worth stating because it looks like a weaker bake and is not. The ramps and the cube have a
    // procedural branch to reproduce, so their neutral bake is a SHAPE the shader already computed (`smoothstep`,
    // the identity). This stage had NO matcap term at all before this lane existed, so "the look without the
    // artist's matcap" is the reference's own expression with the lookup at zero - `1 + 0 * strength` - and any
    // other content would be this repository inventing art direction and calling it a substitute. The lane is
    // real, it is read, and what it is FOR is honouring an authored ball.
    constexpr uint32_t baked_matcap_size = 256;
    auto const bake_matcap = []() {
        // A matcap is an sRGB reference image (the reference's `EfClothSampleMatcap` says so explicitly), so the
        // lane's upload format applies here as it does to the ramps: zero is zero in both encodings, which is the
        // one value where the encode step cannot disagree with itself.
        return std::vector<uint8_t>(static_cast<std::size_t>(baked_matcap_size) * baked_matcap_size * 4u, 0u);
    };

    // ---- THE METALLIC/GLOSS NEUTRAL IS GONE, AND ITS ABSENCE IS THE CONTRACT ----
    //
    // A material with no `_MetallicGlossMap` is shaded by its FAMILY's `roughness` / `reflectivity` (see
    // toon_params.slang), because those four channels are the material's own numbers rather than art this project
    // would be reproducing - and a family default is the article's own answer for skin, which ships no such map.
    // The lane therefore answers "there is none" and the shading stage decides.

    // THE LOOKUP, whose lane vocabulary lives with the diagnostic above so that the two cannot disagree about
    // which flag switches a lane on.
    auto const toon_texture = [](void* const owner, std::string_view const material_name, deren::vulkan::toon_slot const lane) -> deren::vulkan::texture_input {
        toon_lookup_state const& state = *static_cast<toon_lookup_state*>(owner);
        deren::vulkan::texture_input out = {};
        if (state.sidecar == nullptr || state.scenes == nullptr) {
            return out;
        }
        deren::toon::material_sidecar const* const material = state.sidecar->find(material_name);
        if (material == nullptr) {
            return out;
        }
        toon_lane_names const& names = toon_lane[static_cast<std::size_t>(lane)];
        std::string_view const slot_name = names.slot;
        // white fallback - i.e. "do not read" (see material_record::toon_indices). A map that exists while its
        // flag is off must NOT be read: that is the first rule the sidecar module exists to keep. Asked BY NAME
        // rather than by slot, because that is the only spelling that is true for all four lanes.
        if (!material->enabled_by_flag(names.flag)) {
            return out;
        }
        // ---- WHICH LANES READ THE MODEL'S IMAGE AND WHICH READ A NEUTRAL BAKE ----
        //
        // THE SPLIT IS THE ASSET BOUNDARY RATHER THAN A PREFERENCE, and the two sides are different in kind. Three
        // lanes stand in for maps whose content is a SHAPE the shading could describe itself (see bake_unit_ramp /
        // bake_shadow_lut / bake_matcap), so they take the bake whatever the model carries - the artist's switch
        // above still decides WHETHER there is a ramp or a LUT at all. The METALLIC/GLOSS lane is the other case:
        // its four channels are the material's own NUMBERS rather than art this project would be reproducing, so
        // it READS THE MODEL'S MAP and falls back to a neutral texel when the model has none.
        //
        // THE MODEL'S IMAGE FOR THIS SLOT IS RESOLVED ONCE, here, for every lane below: the sidecar names a texture
        // asset and the glTF carries images by name, so the join is one lookup - and a lane that resolved it may
        // still decline it.
        std::string_view const texture_name = material->slot(slot_name);
        deren::gltf::texture_data const* model_tex = nullptr;
        if (!texture_name.empty()) {
            if (std::optional<uint16_t> const index = state.scenes->texture_index_by_name(texture_name); index.has_value()) {
                deren::gltf::texture_data const& candidate = state.scenes->textures[*index];
                if (!candidate.data.empty() && candidate.width != 0 && candidate.height != 0) {
                    model_tex = &candidate; // present and usable; whether it is READ is the lane's decision
                }
            }
        }
        // ---- THE METALLIC/GLOSS LANE READS THE MODEL'S OWN MAP, OR ANSWERS "THERE IS NONE" ----
        //
        // IT DOES NOT GET A NEUTRAL TEXEL, unlike the ramps and the LUT below, and that is a change the SKIN forced:
        // the four channels are the material's own numbers, and a material with no map for them is not a material
        // whose numbers are neutral - it is one whose numbers the FAMILY states (`toon_params`' `roughness` /
        // `reflectivity`, which is exactly where the article keeps its skin's). So a lane of 0 here means "ask the
        // family", and handing it a baked texel instead would override that with one invented surface.
        if (lane == deren::vulkan::toon_slot::metallic_gloss) {
            if (model_tex != nullptr) {
                out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
                out.width = model_tex->width;
                out.height = model_tex->height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // ---- THE DIFFUSE RAMP READS THE MODEL'S OWN MAP, WHICH IS THE CHANGE THIS LANE WAS WAITING FOR ----
        //
        // THE GAME'S RAMP IS THE ONE TOON ASSET WHOSE CONTENT IS NOT A SHAPE this stage could reconstruct: its
        // RGB is the artist's colour mapping along the light axis and its A the artist's lightness along the same
        // axis, so a neutral bake can only ever stand in for a model that has none. Every character in this
        // repository ships one (`_DiffRampMap` is on 45 of the 45 materials across the five characters), so the
        // lane reads it and `bake_neutral_ramp` covers the rest.
        if (lane == deren::vulkan::toon_slot::diffuse_ramp) {
            if (model_tex != nullptr) {
                out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
                out.width = model_tex->width;
                out.height = model_tex->height;
                out.mip_levels = 1;
                out.valid = true;
                return out;
            }
            if (!state.baked_neutral_ramp.empty()) {
                out.data = std::span<uint8_t const>(state.baked_neutral_ramp.data(), state.baked_neutral_ramp.size());
                out.width = state.baked_neutral_ramp_width;
                out.height = state.baked_neutral_ramp_height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // THE SPECULAR RAMP LANE STILL GETS THE BAKED UNIT STEP, NOT THE MODEL'S IMAGE - see bake_unit_ramp for
        // why a path that read the game's own ramps is a path this repository cannot carry, and note that the
        // artist's switch above is still what decides WHETHER there is a ramp at all. Nothing has taught THIS
        // lane to read the game's own atlas yet, and it is the one that still reads `.r` as a threshold.
        if (lane == deren::vulkan::toon_slot::specular_ramp && !state.baked_unit_ramp.empty()) {
            out.data = std::span<uint8_t const>(state.baked_unit_ramp.data(), state.baked_unit_ramp.size());
            out.width = state.baked_ramp_width;
            out.height = state.baked_ramp_height;
            out.mip_levels = 1;
            out.valid = true;
            return out;
        }
        // ---- THE SHADOW LUT READS THE MODEL'S OWN PALETTE, AND THIS IS THE SKIN'S BIGGEST LEVER ----
        //
        // THE ONE TOON LANE WHOSE CONTENT IS A COLOUR THE ARTIST CHOSE per skin tone: the game ships
        // `T_actor_common_femaleskincolorNN_lut_D` - a `1024x32` strip holding a `32^3` cube - and the shading reads
        // it as "what is THIS albedo when it is in shadow". Its earlier treatment here was the identity bake, which
        // is the right FALLBACK for a model that ships none and the wrong answer for one that does: a baked identity
        // reproduces the albedo, so the whole dark side of every skin in the game was being replaced by the lit
        // one. The artist's switch above still decides whether there is a LUT at all, which is why hair - whose
        // `_UseShadowLutTex` is off - is unaffected by any of this.
        if (lane == deren::vulkan::toon_slot::shadow_lut) {
            if (model_tex != nullptr) {
                out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
                out.width = model_tex->width;
                out.height = model_tex->height;
                out.mip_levels = 1;
                out.valid = true;
                return out;
            }
            if (!state.baked_shadow_lut.empty()) {
                out.data = std::span<uint8_t const>(state.baked_shadow_lut.data(), state.baked_shadow_lut.size());
                out.width = state.baked_lut_width;
                out.height = state.baked_lut_height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // ---- THE MATCAP LANE READS THE MODEL'S OWN BALL, WITH THE BLACK BAKE AS ITS FALLBACK ----
        //
        // THE EYE'S HIGHLIGHT IS A MATCAP AND NOT A LOBE, which is the article's own arrangement for that family
        // (`EF_EYE_IRIS_MATCAP05_STRENGTH` and a separate overlay material), and the game ships the ball as
        // `T_actor_common_matcap_06_D`. Reading it is what turns an iris from a flat dark disc into a surface with a
        // catchlight in it - and the bake below still covers a model whose material switches a matcap on without
        // shipping one, where black remains the only neutral content (see bake_matcap).
        //
        // THIS IS ALSO THE LANE THE FLAG TABLE ABOVE WAS FIXED FOR: until the flag was asked by name, `_MatcapTex`
        // resolved to "off" for the very material that ships it, so this branch was unreachable and the game's ball
        // was neither read nor replaced - the lane was simply absent, the quietest possible version of wrong.
        if (lane == deren::vulkan::toon_slot::matcap) {
            if (model_tex != nullptr) {
                out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
                out.width = model_tex->width;
                out.height = model_tex->height;
                out.mip_levels = 1;
                out.valid = true;
                return out;
            }
            if (!state.baked_matcap.empty()) {
                out.data = std::span<uint8_t const>(state.baked_matcap.data(), state.baked_matcap.size());
                out.width = state.baked_matcap_size;
                out.height = state.baked_matcap_size;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // ---- THE GOO BASE RAMP RESOLVES THE REFERENCE'S `RampSelect` ON THE HOST, AND THAT IS THE WHOLE POINT ----
        //
        // THE REFERENCE'S SELECTOR IS NOT A TEXTURE SLOT: `RampSelect` instantiates four `ShaderNodeTexImage`
        // nodes inside itself and picks one with the material's `RampIndex` through five comparators and three
        // MIX nodes (`goo_step4_diffuse_spec.md` §2.3/§2.4). The port may resolve it here for two reasons the spec
        // proves rather than assumes (§2.6 + its A8):
        //
        //   * `RampIndex` HAS NO MATERIAL LINK in either dump - it is a per-material CONSTANT, so the choice cannot
        //     change inside a frame;
        //   * THE FOUR SLOTS HOLD ONLY TWO DISTINCT IMAGES (sha256 `ff12009a...` for slots 1 and 3,
        //     `f92de330...` for slot 2 and the dangling `...001`), and every laevatain material states
        //     `RampIndex` 0.0 or 1.0 - so on this asset the whole 41-node selector answers one of two names.
        //
        // THE SHADER THEREFORE SEES ONE LANE AND ONE FETCH, and this branch is where the arm it lands in stops
        // being the shader's business. THE NAMES ARE THE `_RD` IMAGES' glTF NAMES, appended to the model by
        // `deren-ab/attach_toon_images.py` - without that step the lane resolves to nothing and the material keeps
        // the old chain's diffuse, which is the correct fallback and an invisible one in a log.
        if (lane == deren::vulkan::toon_slot::goo_base_ramp) {
            // TWO SOURCES, AND THE ORDER IS THE STATEMENT RATHER THAN A CONVENIENCE. `_GooBaseRamp` is the
            // sidecar's own slot row and carries the name the reference's selector resolves to, already applied
            // where the file was written; `_GooRampIndex` is the raw socket and is read only when the slot row is
            // absent, which is the case for a material whose sidecar states the index and no name. Both answer the
            // SAME name on every material this asset gives the group to, and `tests/test_goo_toon_math.cpp` asserts
            // that agreement against the reference's four thresholds.
            //
            // THE MODEL IS THE JUDGE OF PRESENCE: a name the model does not carry leaves the lane `invalid`, so a
            // material with no ramp in the file keeps the old chain's diffuse rather than sampling the white
            // fallback as if it were a ramp.
            std::string_view const ramp_name = !texture_name.empty() ? texture_name : toon_base_ramp_name(*material);
            deren::gltf::texture_data const* ramp_tex = nullptr;
            if (!ramp_name.empty()) {
                if (std::optional<uint16_t> const index = state.scenes->texture_index_by_name(ramp_name); index.has_value()) {
                    deren::gltf::texture_data const& candidate = state.scenes->textures[*index];
                    if (!candidate.data.empty() && candidate.width != 0 && candidate.height != 0) {
                        ramp_tex = &candidate;
                    }
                }
            }
            if (ramp_tex != nullptr) {
                out.data = std::span<uint8_t const>(ramp_tex->data.data(), ramp_tex->data.size());
                out.width = ramp_tex->width;
                out.height = ramp_tex->height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }
        // ---- THE `armA` SHEET LANE HONOURS `RS_Index`: TWO NAMED SHEETS, AND THE HOST IS WHAT CHOOSES ----
        //
        // THE REFERENCE MIXES TWO PER-CHARACTER `_RS` SHEETS by `RS_Index` (`混合.032`; spec §1.1) and this port
        // reads ONE sheet per material, so the choice has to be resolved HERE, before the shader sees a lane -
        // which is what `toon_rs_sheet_of` above does. It may be resolved on the host for the same reason
        // `RampSelect` may be (see the branch above): `RS_Index` has NO MATERIAL LINK in either dump, so it is a
        // per-material CONSTANT and the answer cannot change inside a frame (spec §1.3, §9.1).
        //
        // WHY THIS IS A HOST RULE AND NOT A SHADER ONE: `rs_arm0_lane.x` stays UNREAD in `shaders/goo_toon.slang`
        // (see the L1 note there) - the lane carries the reference's socket, the HOST answers it, and the shader's
        // `u` derivation and its single `heap_texel` fetch are untouched by this step. THE `.spv` FILES ARE THE
        // EVIDENCE: this branch must not move a single one of them.
        //
        // THE FALLBACK IS THE FIRST SHEET, IN EVERY DIRECTION. A name that is absent, or whose image this model
        // does not carry, leaves the lane reading `_GooRSSheet` exactly as it read before this branch existed -
        // which is also what keeps every existing fixture and every shipped material (`RS_Index = 0`, no
        // `_GooRSSheet1` row at all) byte-identical. `_GooRSSheet1`'s own `_UseGooRSSheet1` switch is asked
        // inside `toon_rs_sheet_of`, so "named but switched off" is the first sheet too.
        if (lane == deren::vulkan::toon_slot::goo_rs_sheet) {
            toon_rs_sheet_choice const choice = toon_rs_sheet_of(*material, texture_name);
            deren::gltf::texture_data const* sheet_tex = nullptr;
            if (!choice.name.empty()) {
                if (std::optional<uint16_t> const index = state.scenes->texture_index_by_name(choice.name); index.has_value()) {
                    deren::gltf::texture_data const& candidate = state.scenes->textures[*index];
                    if (!candidate.data.empty() && candidate.width != 0 && candidate.height != 0) {
                        sheet_tex = &candidate;
                    }
                }
            }
            // THE SECOND SHEET WAS NAMED, SWITCHED ON, AND STILL DID NOT RESOLVE: this model does not carry that
            // image, so the material reads the first sheet instead - the same answer as every other miss, and the
            // same distinction the tail below draws between "the sidecar names a map" and "the model has it". The
            // alternative (an invalid lane) would silently punch a hole in the shading rather than degrade to the
            // sheet the material would have read without `RS_Index` at all.
            if (sheet_tex == nullptr && choice.second) {
                if (std::optional<uint16_t> const index = state.scenes->texture_index_by_name(texture_name); index.has_value()) {
                    deren::gltf::texture_data const& candidate = state.scenes->textures[*index];
                    if (!candidate.data.empty() && candidate.width != 0 && candidate.height != 0) {
                        sheet_tex = &candidate;
                    }
                }
            }
            if (sheet_tex != nullptr) {
                out.data = std::span<uint8_t const>(sheet_tex->data.data(), sheet_tex->data.size());
                out.width = sheet_tex->width;
                out.height = sheet_tex->height;
                out.mip_levels = 1;
                out.valid = true;
            }
            return out;
        }

        if (model_tex == nullptr) {
            return out; // the sidecar names a map this model does not have, or one that is present but unusable
        }
        out.data = std::span<uint8_t const>(model_tex->data.data(), model_tex->data.size());
        out.width = model_tex->width;
        out.height = model_tex->height;
        out.mip_levels = 1;
        out.valid = true;
        return out;
    };
    // THE COLOUR LOOKUP, whose lanes are the same sidecar rows read as four floats instead of as a texture name.
    //
    // A `color` ROW IS TEXT UNTIL SOMEBODY PARSES IT (`2.3985064,1.885226,2.0379374,1.0`), and the parsing lives
    // here rather than in the sidecar module because the *meaning* of the value is the consumer's: the module
    // keeps the row verbatim so that a consumer which does not know what `_EyeHighLightColor` is cannot damage it.
    // A ROW THAT IS ABSENT, OR UNPARSEABLE, ANSWERS THE LANE'S NEUTRAL - white for the three multiply-tints, and
    // for lane 3 white in `.rgb` with a WIDTH OF 0 in `.w` (see the table below), and for the two scalar lanes
    // (4 and 5) the sentinel `-1` (see them, too) - so a material whose sources say nothing tints nothing, is not
    // outlined, and keeps the fallback its consumer already had.
    //
    // THE NEUTRAL IS PER LANE AND NOT ONE WHITE, AND THAT IS A CORRECTNESS MATTER RATHER THAN A REFINEMENT: three
    // of the outline lane's four floats are a tint, whose neutral is white, and the fourth is `_OutlineWidth`,
    // whose neutral is ZERO. A missing row answered with `vec4(1.0f)` would therefore mean a width of 1.0 - an
    // outline around EVERY material that states none, which is the whole model (`toon_colour_lane`'s own note
    // states the same thing from the other side).
    //
    // THE SPECULAR LANE'S NEUTRAL IS `-1.0` AND IT IS A SENTINEL RATHER THAN A NO-OP, which is the first of the
    // two places this table says "the asset states nothing" instead of "multiply by one": the lane's `.x` is a
    // STRENGTH, so `0` would mean "no highlight at all" - a value the game really states on the iris and the
    // brow - and `1` would mean a full one. The stage tests `>= 0.0` and falls back to the family table (see
    // `toon_colour_lane::specular_strength`).
    //
    // THE PARALLAX LANE'S NEUTRAL IS THE SAME `-1.0` FOR THE SAME KIND OF REASON: its `.x` is a DEPTH rather than a
    // tint, so there is no no-op number - `0` is a legitimate authored "no parallax", and any other in-range value
    // would be a depth the port invented. The stage tests `>= 0.0` and falls back to its own
    // `character_eye_parallax_depth` constant (see `toon_colour_lane::parallax_scale`).
    static constexpr std::array<glm::vec4, static_cast<std::size_t>(deren::vulkan::toon_colour_lane::count)> toon_colour_neutral = {
        {glm::vec4(1.0f), glm::vec4(1.0f), glm::vec4(1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 0.0f), glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f), glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f),
         // STEP 4'S SIX. THE FOUR SENTINELED LANES START AT `-1000.0f` AND NOT AT `-1.0f`, and the reason is
         // the whole point of a sentinel: two of their eight per-material numbers are AUTHORED NEGATIVES -
         // `CastShadow_center` is `-0.10000000149011612` on both body materials and
         // `GlobalShadowBrightnessAdjustment` is `-1.7999999523162842` on the cloth - so a neutral inside the
         // values' own range would make the stage read an authored number as "not stated". See
         // `goo_lane_absent` in `shaders/character_forward.slang` and each lane's note in
         // `deren::vulkan::toon_colour_lane`.
         glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
         // STEP 5'S FOUR, WHOSE NEUTRALS ARE THE REFERENCE'S OWN `interface[]` DEFAULTS rather than the `-1000`
         // sentinel four of step 4's lanes need - and the reason is the SOCKETS' RANGES, which is the check step 4's
         // own root cause ("two of MY sockets are legitimately negative") says to make before reusing a sentinel.
         // None of these four numbers is negative in the reference's asset: `specularFGD Strength` is `0.8` or
         // `1.0`, `dirLight_lightColor` is `(1, 0.958..., 0.958...)`, `AmbientLightColorTint` is white or
         // `(1.512...)`, and `SpecularColor` is a positive HDR multiplier (`[4.2093, 3.7652, 3.7652]`). So `< 0`
         // lies outside every one of their domains and is enough, while step 4's lanes had
         // `CastShadow_center = -0.1` and `GlobalShadowBrightnessAdjustment = -1.8` - authored negatives that a `-1`
         // neutral would have swallowed. The scalar lane carries its sentinel in `.x` alone (the stage resolves
         // that component to the reference's own `1.0`), and the three colour lanes fall back on their own four
         // components, because a `-1` component of a light or a multiplier is not a state the reference has.
         glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f), glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
         // STEP 7'S FOUR. THE TWO SCALAR LANES ARE `-1000` SENTINELS and the two colours are BLACK, each for the
         // reason its own note gives: the scalars' zero is a meaningful value (`SmoothnessMax = 0` is "perfectly
         // rough", and the two brightnesses are multiplied into the pixel), while the two colours' socket defaults
         // in the reference's own interface ARE black - white would be the strongest possible statement about a
         // nose shadow (`混合.017`'s A side) and about `Front transparent red`'s tint.
         glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f), glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
         glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
         // STEP 8'S ONE. A `-1000` SENTINEL RATHER THAN THE REFERENCE'S GROUP DEFAULT `1.0`, and that is the whole
         // of this lane's contract: an absent row leaves the chain's previous normal in place instead of switching
         // the decode on for a material the port carried no measured value for. `NormalStrength = 0` is a value the
         // reference's own materials state (`chen_body_01.001` is `1.3184...` with `Use NormalTex? = 0`), so a `0`
         // neutral would be a statement; `-1000` is outside the socket's range and passes only as "not stated".
         glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f),
         // STEP 10'S ONE IS `(0, 0, 0, 0)` AND IT IS THE REFERENCE'S OWN GROUP DEFAULT, not a sentinel and not a
         // chosen number: `ng[2].interface[3]` (`Use anisotropy?`) is `0.0`, `interface[45]` (`Anisotropic mask`) is
         // `0.0` and `interface[4]` (`Use Toonaniso?`) is `0.0`. So a material that states no `_GooAnisoGate` row
         // gets the answer the graph gives a caller that states nothing - and because `混合.016` returns its A arm
         // at a factor of 0, that answer is the isotropic product (`.y = 0` also makes the masked arm zero, so the
         // two agree here as they do in the dump). The ZERO IS ALSO LOAD-BEARING FOR THE OTHER DIRECTION: the GPU
         // table starts every lane at `glm::vec4(1.0f)`, so without this entry a material with no row would read
         // `Use anisotropy? = 1` and take the arm the reference does not. See `toon_colour_lane::goo_aniso_gate`.
         //
         // IT NEEDS NO BRANCH IN `toon_colour` BELOW AND GETS NO START-UP DIAGNOSTIC, and both follow from the same
         // fact: it is a `color` row with three components the reference states as plain numbers, so it carries no
         // sentinel - step 8's lane needed its own branch only because a `float` row cannot travel the generic
         // `others` path, and its `-1000` needed the `>= 0.0f` test because a sentinel has to be read as "not
         // stated". Here `0.0` IS a stated value (it is the reference's own default), there is nothing to test for,
         // and the generic path at the end of `toon_colour` carries the row as it carries `_GooSpecularColor`.
         glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
         // STEP 12'S ONE IS ALSO `(0, 0, 0, 0)`, AND FOR STEP 10'S REASON RATHER THAN BY IMITATION: `ng[2].interface[20]`
         // (`Aniso_SmoothnessMaxT`) is `0.0` and `interface[21]` (`Aniso_SmoothnessMaxB`) is `0.0`, so a material
         // that states no `_GooAnisoRough` row reads the roughnesses - `rT = rB = 1` - that the graph itself gives
         // a caller that states nothing. It too is a `color` row, so it too needs no branch below, and the value
         // `0.0` is a real one the reference uses, so it is not a sentinel. The GPU-side override is
         // load-bearing in the same way: that table starts every lane at `glm::vec4(1.0f)`, and an unoverridden
         // lane 26 would give every rowless material `rT = (1 - 1)^2 = 0`. See
         // `toon_colour_lane::goo_aniso_rough`.
         glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
         // STEP 13'S TWO ARE `(0, 0, 0, 0)` AS WELL, AND HERE THE ZERO IS NOT A GROUP DEFAULT BUT THE SWITCH
         // ITSELF: `_GooRSScalars.x` is `Use RS_Eff?`, so a material whose sidecar states no such row must read
         // `Use = 0` - the reference's own `interface[]` default for that socket - and the stage's gate then
         // leaves the material's colour UNTOUCHED. Lane 28's `.w` is the mask's `SmoothStep.max` and zero there
         // means "unstated" rather than a literal zero-sized window, which the stage turns into `1.0` precisely so
         // that this neutral cannot divide by zero (see `toon_colour_lane::goo_rs_tint`, spec U7).
         //
         // THESE TWO ARE WRITTEN OUT RATHER THAN LEFT TO THE ARRAY'S TAIL, and that is not style: this array's
         // length is now `toon_colour_lane::count`, and what a VALUE-INITIALISED tail holds depends on GLM's
         // `GLM_FORCE_CTOR_INIT` - so "27 entries plus a tail that happens to be zero" is a claim about a build
         // configuration rather than about this table. Stated zeros cannot drift with a define.
         //
         // AND THEY ARE NOT WHAT KEEPS THE TWENTY `Use RS_Eff? = 0` MATERIALS UNCHANGED. That is a property of
         // the three mirror tables' common stride (`material_index * toon_colour_lane::count + lane`), which
         // `register_material` overwrites lane by lane for every material it registers. These entries are the
         // honest statement of the neutral, and the guard against a future consumer that reads the table before
         // registration - not the mechanism of the identity gate.
         glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
         glm::vec4(0.0f, 0.0f, 0.0f, 0.0f),
         // STEP 15'S ONE IS `(0, 0, 0, 0)`, WHICH IS BOTH THE REFERENCE'S GROUP DEFAULTS AND THE ARM'S OFF
         // SWITCH: all four `armA` sockets default to `0.0` in `ng[2].interface[]`, `RS Strength = 0` zeroes the
         // whole arm's product, and `RS_Index = 0` names the first sheet - so a material that states no
         // `_GooRSArm0` row reads what the graph gives a caller that states nothing. It is a `color` row, so the
         // generic path at the end of `toon_colour` carries it and no branch below is needed.
         //
         // IT IS NOT THE SAME AS `Use RS_Eff? = 0`, and the difference is stated rather than hidden: this lane's
         // zero makes `armA`'s colour zero, and a zero colour still passes through `混合.029` (LIGHTEN), whose
         // `max(base, 0)` is only bitwise `base` where `base >= 0`. The outer gate is what keeps a material with no
         // RS row unchanged. See `toon_colour_lane::goo_rs_arm0`.
         glm::vec4(0.0f, 0.0f, 0.0f, 0.0f)}};
    auto const toon_colour = [](void* const owner, std::string_view const material_name, deren::vulkan::toon_colour_lane const lane) -> glm::vec4 {
        std::size_t const lane_index = static_cast<std::size_t>(lane);
        toon_lookup_state const& state = *static_cast<toon_lookup_state*>(owner);
        // ---- THE SPECULAR STRENGTH IS READ BEFORE THE SIDECAR IS EVEN LOOKED FOR, AND THAT ORDER IS THE RULE ----
        //
        // Its value is the ASSET'S (`extras_float_of`), the sidecar is only the fallback, and the FAMILY TABLE in
        // the shader is the fallback behind that - so this lane must answer even for a model that has extras and
        // NO sidecar at all, which is why it does not sit behind the two early returns below. `-1` is the third
        // answer (neither source speaks), and the stage reads it as "the family's number stands".
        //
        // THE PARALLAX DEPTH TAKES EXACTLY THE SAME PATH IN THE SAME ORDER, which is why the two share one branch:
        // both are per-material SCALARS whose preferred source is the asset (`-1` = nothing stated), and the only
        // thing that differs is what the stage falls back TO - `params.spec_strength` for one,
        // `character_eye_parallax_depth` for the other. On chen the sidecar holds no `_ParallaxScale` row at all,
        // so for that property the asset is the ONLY source that can speak and this lane answers the sentinel for
        // every material but the three that state the row - which is the state the frame had before this lane
        // existed, material by material.
        if (lane == deren::vulkan::toon_colour_lane::specular_strength || lane == deren::vulkan::toon_colour_lane::parallax_scale) {
            glm::vec4 scalar = toon_colour_neutral[lane_index]; // -1 in .x until a source states a value
            if (std::optional<float> const from_asset = extras_float_of(state.scenes, material_name, toon_colour_row[lane_index])) {
                scalar.x = *from_asset;
                return scalar;
            }
            if (state.sidecar != nullptr) {
                if (deren::toon::material_sidecar const* const from_sidecar = state.sidecar->find(material_name)) {
                    // The same "states nothing" contract as the generic path below: `scalar` answers the fallback
                    // when the row is absent, and the fallback here IS the sentinel.
                    float const value = from_sidecar->scalar(toon_colour_row[lane_index], scalar.x);
                    // A SIDECAR ROW OUTSIDE THE SENTINEL'S SIDE OF ZERO IS IGNORED RATHER THAN TRUSTED, because
                    // `-1` is not a strength (nor a depth) and a negative one would shade nothing: a row that
                    // parsed as a negative number is a row this lane cannot express, so the stage's own fallback
                    // answers instead.
                    if (value >= 0.0f) {
                        scalar.x = value;
                    }
                }
            }
            return scalar;
        }
        if (state.sidecar == nullptr) {
            return toon_colour_neutral[lane_index];
        }
        deren::toon::material_sidecar const* const material = state.sidecar->find(material_name);
        if (material == nullptr) {
            return toon_colour_neutral[lane_index];
        }
        // ---- STEP 8'S NORMAL STRENGTH, WHICH IS A `float` ROW AND THEREFORE NEEDS THIS BRANCH ----
        //
        // The generic path at the bottom of this lambda reads `material->others`, and a `float`-kind row does not
        // land there: `toon_material_sidecar.cpp` routes `slot` to `slots`, `float` to `scalars` and every other
        // kind to `others`, and only `scalar()` reads `scalars`. So a `_GooNormalStrength` row written as a `color`
        // row would be parsed by the generic path and a row written as the `float` row it is would be invisible -
        // hence this branch, and hence it sits before the outline branch rather than inside the generic one.
        //
        // THE `>= 0.0f` TEST IS THE SAME ONE THE SPECULAR LANE MAKES, for the same reason: the value is a positive
        // strength on every material either dump states, so a row that parsed negative is a row this lane cannot
        // express and the neutral (the sentinel, i.e. "not stated") answers instead of a strength that would flip
        // the normal's `xy`.
        if (lane == deren::vulkan::toon_colour_lane::goo_normal_strength) {
            glm::vec4 strength = toon_colour_neutral[lane_index]; // -1000 in every component until a row says otherwise
            float const value = material->scalar(toon_colour_row[lane_index], strength.x);
            if (value >= 0.0f) {
                strength.x = value;
            }
            return strength;
        }
        // `specularFGD Strength` IS A `float` ROW TOO, so it needs the same branch step 8's lane needed: a
        // `float`-kind row lands in `material_sidecar::scalars` (see `toon_material_sidecar.cpp`), and the
        // generic path below reads `material->others`, so without this branch the row is unreachable and the
        // lane answers its neutral for every material - which is what shipped: both body materials state
        // `0.7999999523162842` and every cloth states `1.0`, and the shader read `-1.0` for all of them.
        //
        // THE NEUTRAL'S `.x` IS THE SENTINEL (`-1.0`, "this material's container states nothing") and the
        // shader resolves it to the reference's own group default `1.0` - see
        // `deren::vulkan::toon_colour_lane::goo_specular_fgd`, whose contract is `< 0` and NOT the `-1000` sentinel
        // step 4's four lanes use.
        if (lane == deren::vulkan::toon_colour_lane::goo_specular_fgd) {
            glm::vec4 fgd = toon_colour_neutral[lane_index]; // -1 in `.x` until a row says otherwise
            float const value = material->scalar(toon_colour_row[lane_index], fgd.x);
            if (value >= 0.0f) {
                fgd.x = value;
            }
            return fgd;
        }
        // THE OUTLINE LANE IS THE ONE LANE FED BY TWO ROWS OF TWO DIFFERENT KINDS, so it does not go through the
        // single-row path below: `.rgb` is the `color _OutlineTintColor` row (the game's name for the author's
        // `_OutlineColor`) and `.w` is the SEPARATE `float _OutlineWidth` row - see `toon_colour_lane::outline_edge`.
        //
        // THE TWO ROWS ARE INDEPENDENT, WHICH IS MEASURED RATHER THAN ASSUMED: in `chars\chen.glb.toon.tsv` the
        // tint row exists for `M_actor_chen_cloth_01` ALONE (1,1,1,1), while `_OutlineWidth` exists for all five
        // materials (body 0.6 / cloth_01 0.6 / cloth_02 0.0 / face 0.5 / hair 0.5). Reading the width only when
        // the tint row is present would therefore outline one material out of five and silently drop the rest, so
        // the width is read unconditionally and a missing tint answers white - the article's own `_OutlineColor`
        // default is white, and white is what "the game states no tint" has to look like for a multiply.
        if (lane == deren::vulkan::toon_colour_lane::outline_edge) {
            glm::vec4 outline = toon_colour_neutral[lane_index]; // white tint, width 0 until a row says otherwise
            if (auto const tint = material->others.find(std::string(toon_colour_row[lane_index])); tint != material->others.end()) {
                std::string_view rest = tint->second;
                for (int32_t component = 0; component < 4 && !rest.empty(); ++component) {
                    std::size_t const comma = rest.find(',');
                    std::string_view const field = rest.substr(0, comma);
                    float value = 0.0f;
                    if (auto const result = std::from_chars(field.data(), field.data() + field.size(), value); result.ec == std::errc{}) {
                        outline[component] = value;
                    }
                    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
                }
            }
            // ... AND THE WIDTH, which is a `float` row rather than part of the colour row. A material with no
            // `_OutlineWidth` row gets 0.0, i.e. no outline - the only answer that leaves the frame where it was.
            outline.w = material->scalar("_OutlineWidth", 0.0f);
            return outline;
        }
        auto const row = material->others.find(std::string(toon_colour_row[lane_index]));
        if (row == material->others.end()) {
            return toon_colour_neutral[lane_index];
        }
        glm::vec4 parsed = toon_colour_neutral[lane_index];
        std::string_view rest = row->second;
        for (int32_t component = 0; component < 4 && !rest.empty(); ++component) {
            std::size_t const comma = rest.find(',');
            std::string_view const field = rest.substr(0, comma);
            float value = 0.0f;
            if (auto const result = std::from_chars(field.data(), field.data() + field.size(), value); result.ec == std::errc{}) {
                parsed[component] = value;
            }
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
        return parsed;
    };
    // THE SCALAR LOOKUP, for the one toon fact that is neither a map nor a colour: the article's TRANSPARENT
    // variant, which `M_actor_chen_cloth_02` selects with `_SrcBlend 5` / `_DstBlend 10` (Unity's `SrcAlpha` /
    // `OneMinusSrcAlpha`) while its glTF `alphaMode` stays OPAQUE. The two rows are read by NAME through this
    // callback so the runtime never has to know what a sidecar is (see `toon_lookup::scalar`), and the pair is
    // mapped to `toon_inputs::alpha_blend` only when BOTH say the standard alpha equation - 5/10 is the only
    // pair any material in this repository states, and any other pair is left alone rather than guessed at.
    auto const toon_scalar = [](void* const owner, std::string_view const material_name, std::string_view const row, float const fallback) -> float {
        toon_lookup_state const& state = *static_cast<toon_lookup_state*>(owner);
        if (state.sidecar == nullptr) {
            return fallback;
        }
        deren::toon::material_sidecar const* const material = state.sidecar->find(material_name);
        if (material == nullptr) {
            return fallback;
        }
        return material->scalar(row, fallback);
    };
    toon_lookup_state toon_state{.sidecar = toon_sidecar.has_value() ? &*toon_sidecar : nullptr, .scenes = &*scenes};
    toon_state.baked_unit_ramp = bake_unit_ramp();
    toon_state.baked_ramp_width = baked_ramp_width;
    toon_state.baked_ramp_height = baked_ramp_height;
    toon_state.baked_neutral_ramp = bake_neutral_ramp();
    toon_state.baked_neutral_ramp_width = baked_neutral_ramp_width;
    toon_state.baked_neutral_ramp_height = baked_neutral_ramp_height;
    toon_state.baked_shadow_lut = bake_shadow_lut();
    toon_state.baked_lut_width = baked_lut_width;
    toon_state.baked_lut_height = baked_lut_height;
    toon_state.baked_matcap = bake_matcap();
    toon_state.baked_matcap_size = baked_matcap_size;
    runtime.set_toon_lookup(deren::vulkan::runtime::toon_lookup{.owner = &toon_state, .texture = toon_texture, .colour = toon_colour, .scalar = toon_scalar});

    // ---- THE HEAD FRAME the face SDF shades against, resolved once here and published every frame ----
    //
    // THE LOADER IS ASKED FIRST, and the answer is a decision this code makes rather than a constant it never
    // questioned - which matters, because for the models in this repository the two answers happen to coincide.
    //
    // THE FALLBACK IS THE REFERENCE'S OWN FRAME (`EfFaceGetHeadBasis`'s `valid < 0.5` branch), so a model with
    // no usable skeleton is shaded by the answer the reference itself gives for that case rather than by a guess
    // made here. This model is exactly that case: `zhuangfy_scalar.glb` is seven nodes and zero skins, a static
    // pose split by body part, and a model that cannot turn its head has one head frame whether it is read from
    // a bone or from these three constants.
    // WHICH RIG AND WHICH JOINT, kept for the frame loop below: both are needed there to read the bone's world
    // matrix, and the joint index is the SAME number `head_joint_of` returns (an index into `skin::joints`, not
    // an asset node index) because that is what picks a matrix out of the rig's joint block.
    std::size_t head_rig = 0;
    std::optional<std::size_t> head_joint = std::nullopt;
    {
        deren::gltf::head_basis head = deren::gltf::head_basis_fallback();
        for (std::size_t scene_index = 0; scene_index < scenes->scene.size() && !head_joint.has_value(); ++scene_index) {
            for (std::size_t skin_index = 0; skin_index < scenes->skins.size(); ++skin_index) {
                if (std::optional<std::size_t> const joint = deren::gltf::head_joint_of(*scenes, scene_index, skin_index); joint.has_value()) {
                    head_rig = skin_index;
                    head_joint = joint;
                    break;
                }
            }
        }
        if (head_joint.has_value()) {
            deren::utility::log("head frame: '{}' HAS a head bone at rig {} joint {} - the frame is read from it every frame", model_path, head_rig, *head_joint);
        } else {
            deren::utility::log("head frame: no head bone in '{}' ({} skin(s)) - shading from the reference's fallback frame", model_path, scenes->skins.size());
        }
        // THE FOURTH MEMBER IS A POSITION AND A FLAG, and the fallback publish leaves BOTH at zero: this branch is
        // the one a model with NO HEAD BONE takes, and for such a model there is no `HC` object to read a centre
        // from. `center.w = 0.0` is what tells the face arm that, and its answer is the socket's own default
        // (`sphereNormal_Strength = 0.0`) rather than a sphere about the world origin.
        runtime.set_head_basis(deren::vulkan::head_ubo{.front = glm::vec4(head.front, 0.0f), .right = glm::vec4(head.right, 0.0f), .up = glm::vec4(head.up, 0.0f), .center = glm::vec4(0.0f)});
    }

    // ---- THE TOON LIGHT RIG, published once, before the frame loop ----
    //
    // THE ONE CONFIG KNOB THE RIG TAKES SO FAR IS THE DAY STRENGTH, and it is the one the two-state lighting is
    // driven by - so it is the one an A/B capture has to be able to move. Every other rig number keeps
    // `deren::vulkan::toon_rig`'s own default, which is the article's value.
    //
    // PUBLISHING HERE RATHER THAN IN THE FRAME LOOP IS THE BLOCK'S CONTRACT (see core::heap_slots::toon_rig):
    // nothing rewrites it while a frame is in flight, which is what makes a write from outside the frame path
    // safe - and the cost of that is exactly this, that the write has to happen before the loop starts.
    {
        deren::vulkan::toon_rig rig = {};
        rig.day.x = settings.toon.day_strength;
        rig.other_light.x = settings.toon.head_light_day0;
        rig.other_light.y = settings.toon.head_light_day1;
        rig.other_colour = glm::vec4(settings.toon.head_light_colour[0], settings.toon.head_light_colour[1], settings.toon.head_light_colour[2], 0.0f);
        rig.main_dark.x = settings.toon.sun_dark_colour[0];
        rig.main_dark.y = settings.toon.sun_dark_colour[1];
        rig.main_dark.z = settings.toon.sun_dark_colour[2];
        rig.env.x = settings.toon.env_strength;
        rig.env.y = settings.toon.env_rotation;
        rig.env.z = settings.toon.specular_strength;
        rig.env.w = settings.toon.diffuse_blend_effect;
        rig.rim.x = settings.toon.rim_area;
        rig.rim.y = settings.toon.rim_strength;
        rig.rim.z = settings.toon.rim_nolxz_strength;
        rig.misc.x = settings.toon.backlight_strength;
        // THE FACE'S EXPRESSION INDEX rides the head light's colour block's fourth lane, which was unused and is
        // named in `deren::vulkan::toon_rig` - the same zero-cost reuse the material record's `toon_family` makes.
        rig.other_colour.w = settings.toon.emotion_type;
        // THE TOON CHAIN'S SHADOW SOFTNESS LADDER (`[render] toon_shadow_softness`, already rounded and clamped
        // by `analyse_config`): 0 is the shipped 3x3 PCF and the default, so this line changes nothing unless a
        // config asked for a wider kernel. The lane is read by the shared `toon_diffuse` body in
        // `shaders/character_forward.slang`, which is compiled into the `character_forward`, `goo_toon` and
        // `outline` stages (`goo_toon.slang`'s own `rs_shadow` lookup is a different call site and stays 3x3 at
        // every level; `overlay` has no shadow sample at all).
        rig.shadow_softness.x = settings.render.toon_shadow_softness;
        runtime.set_toon_rig(rig);
        deren::utility::log("toon: light rig published - day strength {:.3f}, head light {:.2f} at day 1 of {:.2f} at day 0, env strength {:.2f}, shadow softness {}",
                            rig.day.x,
                            rig.other_light.y,
                            rig.other_light.x,
                            rig.env.x,
                            static_cast<int>(rig.shadow_softness.x));
    }

    deren::vulkan::scene_import_result const imported = runtime.import_scene(node_first, node_last, scene_first, scene_last, scene_import_shift);
    deren::utility::log("imported {} primitives ({} new materials)", imported.primitive_count, imported.material_count);
    runtime.log_scene_tree();

    // 12b. THE FRAME'S STATIC SURROUND, when the config names one ([render] background_glb): the SAME
    //      importer runs a SECOND time into the SAME scene tree, flagged as environment. The flag is the
    //      whole feature: it takes every leaf this import creates out of the toon character stage and out
    //      of the shadow casters (and out of the shadow fit, see runtime::update_shadow_frustum), while the
    //      scene pass draws them through the ordinary PBR/unlit path - which is where the reference's own
    //      matte ground and emissive backdrop live. Nothing about the subject's own import changes.
    //
    //      WHY A SECOND IMPORT AND NOT A MODEL MERGE: the two files are authored separately, the background
    //      is optional and off by default, and the importer already appends to the scene tree (it was built
    //      to). Merging the GLBs offline would bake one machine's choice of background into the character
    //      asset - and the asset is the artist's, not ours.
    if (!settings.render.background_glb.empty()) {
        std::filesystem::path background_path = settings.render.background_glb;
        if (background_path.is_relative()) {
            // relative to the EXECUTABLE's directory, which is where every other asset of this application
            // is found (the shaders dir comes from the same place, see deren::chores::analyse_config)
            background_path = deren::utility::executable_directory() / background_path;
        }
        auto background_load = deren::gltf::load_model_async(background_path.string());
        auto background_scenes = background_load.get();
        if (!background_scenes) {
            deren::utility::panic(std::source_location::current(), "failed to load background model '{}': error code {}", background_path.string(), static_cast<int32_t>(background_scenes.error()));
        }
        deren::gltf::scene_bounds const background_bounds = deren::gltf::log_scene_diagnostics(*background_scenes);
        auto background_resolve = deren::gltf::resolve_materials_async(*background_scenes);
        std::vector<deren::gltf::resolved_material> const background_materials = background_resolve.get();
        // THE OFFSET IS DERIVED FROM THE SUBJECT, NOT CONFIGURED: the background's own y = 0 is its author's
        // GROUND plane and the subject's lowest point after ITS centering shift is `bounds.min.y +
        // scene_import_shift.y`, so shifting the background by the subject's shift plus that minimum puts the
        // ground exactly under the subject's feet. The two XZ centers coincide by construction: the subject's
        // own import sends `scene_center` to `scene_sink`, whose XZ is 0, and the background is authored
        // centered on its own origin. A background whose ground is not at its y = 0 does NOT land correctly -
        // that assumption is stated rather than guessed at (see the config key's note).
        glm::vec3 const background_shift = scene_import_shift + glm::vec3(0.0f, bounds.min.y, 0.0f);
        deren::gltf::scene_node_iterator const background_node_first = background_scenes->nodes_begin();
        deren::gltf::scene_node_iterator const background_node_last;
        deren::gltf::drawable_iterator const background_scene_first(*background_scenes, background_materials);
        deren::gltf::drawable_iterator const background_scene_last;
        deren::vulkan::scene_import_result const background_imported = runtime.import_scene(background_node_first, background_node_last, background_scene_first, background_scene_last, background_shift, true);
        deren::utility::log("background: '{}' imported as the frame's static surround - {} primitives ({} new materials), own bounds y [{:.3f}, {:.3f}], offset ({:.3f}, {:.3f}, {:.3f}); the toon character stage and the shadow casters skip it",
                            background_path.string(),
                            background_imported.primitive_count,
                            background_imported.material_count,
                            background_bounds.min.y,
                            background_bounds.max.y,
                            background_shift.x,
                            background_shift.y,
                            background_shift.z);
    }

    // 12. Enable directional shadow mapping over the imported scene: the shadow frustum frames
    //      the sphere around where the primitives actually sit (they were translated by the import
    //      offset above, so their world-space center is scene_sink) with their original radius
    runtime.enable_shadows(scene_sink, scene_radius);
    // apply the config shadow toggle now that the shadow maps exist (turning it off clears them)
    if (!settings.render.shadow) {
        runtime.set_shadow_enabled(false);
    }

    // 13. Optional instancing stress (deren::chores::add_instancing_grid): grid_side > 1 (config or
    //     argv) draws the first imported primitive as a grid_side x grid_side grid in ONE
    //     instanced draw call (the frame loop is untouched); no-op otherwise.
    deren::chores::add_instancing_grid(runtime, settings.grid_side, scene_radius);

    // 13b. Clustered-light stress ([lighting] demo_lights): spawn N procedural punctual lights on a
    //      helix around the scene bounds, pushed every frame together with the overlay's slots. The
    //      clustered path's whole point is a light count the brute-force loop could not afford, and
    //      the overlay's four slots cannot show that - this is what makes the difference measurable
    //      (and what the [render] clustered_lights A/B is compared against).
    std::vector<deren::vulkan::punctual_light> demo_lights;
    if (settings.lighting.demo_lights > 0) {
        int32_t const total = std::min(settings.lighting.demo_lights, static_cast<int32_t>(deren::app_config::max_demo_lights));
        demo_lights.reserve(static_cast<std::size_t>(total));
        for (int32_t i = 0; i < total; ++i) {
            float const t = static_cast<float>(i) / static_cast<float>(total);
            float const angle = t * 6.2831853f * 3.0f; // three turns around the scene
            float const radius = scene_radius * settings.lighting.demo_light_radius;
            deren::vulkan::punctual_light light = {};
            light.position = scene_sink + glm::vec3(std::cos(angle) * radius, scene_radius * (t - 0.5f), std::sin(angle) * radius);
            // hue cycle: a warm/cool strip of colors makes the per-cluster lists visible as color
            light.color = glm::vec3(0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::cos(angle + 2.094f), 0.5f + 0.5f * std::cos(angle + 4.188f));
            light.intensity = 12.0f;
            light.range = scene_radius * settings.lighting.demo_light_range; // finite range: what the cluster sphere test culls on
            demo_lights.push_back(light);
        }
        deren::utility::log("demo lights: {} procedural punctual lights around the scene (clustered light stress)", total);
    }

    // 14. Main render loop: until the window closes or ESC is pressed.
    //     Every Vulkan frame step (fences, acquire, command buffers, render pass, submit, present)
    //     lives inside runtime::render_frame()
    runtime.log_feature_status(); // one line naming every optional feature that could not be created
    deren::utility::log("rendering '{}' with PBR... left-drag to orbit, wheel to zoom, ESC to exit", model_path);

    // Dear ImGui debug overlay on by default ([gui] show)
    bool const use_gui = settings.gui.show;

    // FPS statistics (deren.utility:frame_stats): a rolling one-second window of frame gaps.
    // tick() once per presented frame, on_skipped() on minimized/recreate iterations, and
    // the once-per-second report (log + the overlay's smoothed value) keys off window_rolled().
    deren::utility::frame_stats frame_stats;

    // ---- keyframe animation playback + skinning + morph targets (deren.vulkan.animation) ----
    // The controller owns playback (sampling + writing node locals), the skin rigs (per-frame
    // joint matrices) and the morph rigs (static deltas + per-frame weights) against the
    // runtime scene tree; initialize it before the first frame (it bakes morph deltas and the
    // identity skin block into every frame slot's buffers). A float mirror of the playback
    // clock feeds the gui time slider (slider_widget binds an external float).
    deren::vulkan::animation::controller animation;
    // the controller drives the runtime through an injected surface (chores wires the scene,
    // per-slot buffers and task pool), so it never depends on deren::vulkan::runtime itself
    // ---- MMD motion (--mmd-motion): parsed, retargeted onto this model's own skeleton, and
    // appended to the loaded file's animations.  Appended rather than handed to the controller
    // because the controller takes its clip list from the scene at init() and never grows one
    // afterwards (select() only swaps the active clip), so a runtime clip has to join the
    // scene's list BEFORE init.  Names come from the pool nodes, and target_node stays the ASSET
    // node index (deren::gltf::node::source_index): animations are file-scoped, the pool index is a
    // different numbering.
    {
        std::string mmd_motion_path;
        for (int32_t i = 1; i + 1 < argc; ++i) {
            if (std::string_view(argv[i]) == "--mmd-motion") {
                mmd_motion_path = argv[i + 1];
            }
        }
        if (!mmd_motion_path.empty()) {
            std::optional<deren::vulkan::animation::mmd_motion> motion =
                deren::vulkan::animation::load_mmd_motion(mmd_motion_path);
            if (!motion.has_value()) {
                deren::utility::panic(std::source_location::current(), "failed to parse MMD motion '{}'", mmd_motion_path);
            }
            // Names come from the asset-level lookup (asset node index -> the loader's node copy),
            // walked over the skin's joints: skin::joints and animation_channel::target_node are
            // BOTH asset node indices, so pairing the two here keeps target_node exact.
            std::vector<std::string> joint_names;
            std::vector<std::size_t> joint_sources;
            for (deren::gltf::skin const& skin : scenes->skins) {
                for (std::size_t const joint_source : skin.joints) {
                    auto const found = scenes->node_by_source.find(joint_source);
                    if (found == scenes->node_by_source.end() || found->second == nullptr) {
                        continue;
                    }
                    joint_names.push_back(found->second->name);
                    joint_sources.push_back(joint_source);
                }
            }
            deren::vulkan::animation::mmd_retarget const retarget =
                deren::vulkan::animation::build_mmd_retarget(*motion, joint_names);
            deren::utility::log("mmd motion '{}': {} bones, {} mapped onto this skeleton, {} unmapped", mmd_motion_path,
                                motion->bones.size(), retarget.mapped, retarget.unmapped);
            for (std::size_t const bone : retarget.unresolved_bones) {
                if (bone < 8) {
                    deren::utility::log("  unmapped: {}", deren::vulkan::animation::escape_mmd_name(motion->bones[bone].name));
                }
            }
            deren::vulkan::animation::clip const baked = deren::vulkan::animation::bake_mmd_clip(*motion, retarget);
            deren::gltf::animation converted;
            converted.name = "mmd";
            converted.samplers.reserve(baked.samplers.size());
            for (deren::vulkan::animation::sampler const& s : baked.samplers) {
                deren::gltf::animation_sampler out;
                out.times = s.times;
                out.values = s.values;
                out.per_key = s.per_key;
                switch (s.interp) {
                case deren::vulkan::animation::interpolation::step:
                    out.interpolation = deren::gltf::animation_interpolation::step;
                    break;
                case deren::vulkan::animation::interpolation::cubic_spline:
                    out.interpolation = deren::gltf::animation_interpolation::cubic_spline;
                    break;
                case deren::vulkan::animation::interpolation::linear:
                default:
                    out.interpolation = deren::gltf::animation_interpolation::linear;
                    break;
                }
                converted.samplers.push_back(std::move(out));
            }
            converted.channels.reserve(baked.channels.size());
            for (deren::vulkan::animation::channel const& c : baked.channels) {
                deren::gltf::animation_channel out;
                out.sampler = c.sampler;
                out.target_node = joint_sources[c.target_node];
                switch (c.path) {
                case deren::vulkan::animation::channel_path::rotation:
                    out.path = deren::gltf::animation_path::rotation;
                    break;
                case deren::vulkan::animation::channel_path::scale:
                    out.path = deren::gltf::animation_path::scale;
                    break;
                case deren::vulkan::animation::channel_path::weights:
                    out.path = deren::gltf::animation_path::weights;
                    break;
                case deren::vulkan::animation::channel_path::translation:
                default:
                    out.path = deren::gltf::animation_path::translation;
                    break;
                }
                converted.channels.push_back(out);
            }
            // The controller's translation channel REPLACES a node's local translation, so the node's
            // own rest offset has to be added back: the bake emits an offset from rest, not a position.
            for (deren::gltf::animation_channel const& c : converted.channels) {
                if (c.path != deren::gltf::animation_path::translation) {
                    continue;
                }
                auto const found = scenes->node_by_source.find(c.target_node);
                if (found == scenes->node_by_source.end() || found->second == nullptr) {
                    continue;
                }
                glm::vec3 const rest = found->second->translation;
                deren::gltf::animation_sampler& s = converted.samplers[c.sampler];

                for (std::size_t i = 0; i + 2 < s.values.size(); i += 3) {
                    s.values[i] += rest.x;
                    s.values[i + 1] += rest.y;
                    s.values[i + 2] += rest.z;
                }
            }
            deren::utility::log("mmd motion: baked clip '{}' - {} samplers, {} channels", converted.name,
                                converted.samplers.size(), converted.channels.size());
            scenes->animations.push_back(std::move(converted));
        }
    }
    animation.init(*scenes, deren::chores::make_animation_backend(runtime), scene_import_shift);
    // [render] animation_time >= 0 PINS the pose: playback is wall-clock driven, so two captures of an
    // animated scene differ unless the time is fixed - and this is also what makes such a scene usable in
    // a measurement or a regression scenario at all. scrub() is the overlay's time slider, so the pose is a
    // function of the value alone; a negative value (the default) plays as always.
    if (settings.render.animation_time >= 0.0f) {
        animation.set_time(settings.render.animation_time);
        deren::utility::log("animation: pinned at {:.2f}s by [render] animation_time (playback is wall-clock driven, so captures of an animated scene are only reproducible this way)", settings.render.animation_time);
        if (capture.animation_seconds_per_frame != 0.0f) {
            deren::utility::log("capture: --capture-animation-sweep is IGNORED - [render] animation_time pins the pose, so the clock never advances (set animation_time = -1 to play)");
        }
    }
    // Live gui widget state (deren::chores::gui_bindings) is declared after the authored-camera
    // seeding below, right before deren::chores::setup_gui() builds the overlay.

    // ---- authored (glTF) camera selection ----
    // A glTF camera is used as a VIEWPOINT SEED for the orbit camera: picking one places the
    // orbit (target = scene center, distance/yaw/pitch derived from the camera node's world),
    // and from there the mouse keeps working normally (drag orbits, wheel zooms). The gui
    // combo below switches among "orbit" and the scene's cameras; the runtime's external-camera
    // override is not used by the demo (it stays available for exact/animated authored cameras).
    // usable cameras: those whose owning node exists in the imported tree, in scenes.cameras order
    struct authored_camera {
        deren::gltf::camera const* camera = nullptr;
        std::size_t source = 0; // owning loader node (asset node index)
    };
    std::vector<authored_camera> authored_cameras;
    for (deren::gltf::camera const& cam : scenes->cameras) {
        // find a node referencing this camera that is present in the imported tree (the loader's
        // asset-level node table + the controller's tree membership test)
        for (auto const& [source, loader_node] : scenes->node_by_source) {
            if (loader_node->camera_index && *loader_node->camera_index == static_cast<std::size_t>(&cam - scenes->cameras.data()) && animation.has_runtime_node(source)) {
                authored_cameras.push_back(authored_camera{&cam, source});
                break;
            }
        }
    }
    // current selection: 0 = orbit, 1..N = authored_cameras[i - 1]; default = the first
    // authored camera when the scene has any (same initial view as before, but now movable)
    int32_t current_camera = authored_cameras.empty() ? 0 : 1;
    // point the orbit camera at the authored pose: target = scene center (frame like the
    // default view), yaw/pitch/distance solved from the camera node's world transform
    auto const seed_orbit_from_camera = [&](int32_t const index) {
        if (index <= 0 || index > static_cast<int32_t>(authored_cameras.size())) {
            return; // "orbit": keep the current free orbit
        }
        authored_camera const& ac = authored_cameras[static_cast<std::size_t>(index - 1)];
        glm::mat4 camera_world = glm::mat4(1.0f);
        bool found = false;
        auto const find_world = [&](auto&& self, deren::vulkan::scene_tree::scene_node& node, glm::mat4 const& parent_world) -> bool {
            glm::mat4 const world = parent_world * node.local;
            if (node.source_index == ac.source) {
                camera_world = world;
                return true;
            }
            for (deren::vulkan::scene_tree::scene_node& child : node.children) {
                if (self(self, child, world)) {
                    return true;
                }
            }
            return false;
        };
        for (deren::vulkan::scene_tree::scene_node& root : runtime.get_scene().roots) {
            if (find_world(find_world, root, glm::mat4(1.0f))) {
                found = true;
                break;
            }
        }
        if (!found) {
            return;
        }
        glm::vec3 const eye = glm::vec3(camera_world[3]);
        glm::vec3 const center = scene_center;
        glm::vec3 const dir = center - eye;
        float const dist = glm::length(dir);
        if (dist < 1e-4f) {
            return;
        }
        glm::vec3 const d = dir / dist;
        runtime.camera.target = center;
        runtime.camera.distance = dist;
        runtime.camera.yaw = std::atan2(d.x, d.z);
        runtime.camera.pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
        std::string_view const cam_name = ac.camera->name.empty() ? std::string_view("<unnamed>") : std::string_view(ac.camera->name);
        deren::utility::log("camera: starting pose from glTF camera '{}' ({}) - you can still orbit/zoom", cam_name,
                            ac.camera->type == deren::gltf::camera_type::perspective ? "perspective" : "orthographic");
    };
    if (!authored_cameras.empty()) {
        seed_orbit_from_camera(current_camera);
    }

    // ---- authored (glTF) punctual lights: KHR_lights_punctual lights up automatically ----
    // They load straight into the editable gui light slots below (see the gui_bindings setup),
    // so imported lights are adjustable in the overlay like the demo ones.

    // Optional Dear ImGui debug overlay: deren::chores::setup_gui enables it on the runtime (when
    // use_gui) and assembles the whole panel - fps label, frustum-culling / shadow
    // toggles, the camera-target drag, animation playback controls, the camera selector and
    // the shadow-bias sliders. The widgets bind to the live gui_bindings below (checkbox and
    // slider mirrors + the animation mirrors, which the frame loop keeps in sync each frame);
    // authored-camera names and the orbit-seeding callback are passed in, so chores never
    // touches glTF types.
    deren::chores::gui_bindings gui;
    gui.shadow_enabled = settings.render.shadow; // checkbox initial states mirror the config
    gui.fxaa_enabled = settings.render.fxaa;
    gui.gbuffer_debug = settings.render.gbuffer_debug; // gbuffer debug view initial state (M1)
    gui.gbuffer_channel = settings.render.gbuffer_channel;
    gui.render_mode = settings.render.unlit ? 1 : 0; // render-mode combo (0 = pbr, 1 = unlit)
    gui.taa_enabled = settings.render.taa;           // temporal anti-aliasing (M3)
    // ... and its TWO BLEND WEIGHTS, which the frame loop mirrors into the runtime every frame
    // (start_demo.set_taa below). Without these two lines the config's values never reached the
    // renderer: chores' gui_bindings defaults (0.9 / 0.5) are what set_taa received on every frame,
    // so editing `[render] taa_blend_static` in the file changed NOTHING - measured, the flicker at a
    // pinned close-up was byte-identical at 0.90, 0.95 and 0.98 (38.22% of pixels changing per frame
    // in all three). `taa_enabled` alone happened to look wired because it IS copied here.
    gui.taa_blend_static = settings.render.taa_blend_static;
    gui.taa_blend_min = settings.render.taa_blend_min;
    // THE TOON CHARACTER STAGE: copied here like every other toggle, and that copy is load-bearing rather than
    // tidiness - the frame loop mirrors `gui.character_forward` into the runtime every frame, so a config value
    // that never reached the gui (the trap the two lines above record) would be overwritten by the binding's
    // default on the very first frame.
    gui.character_forward = settings.render.character_forward;
    // ... AND WHICH CHAIN THAT STAGE DRAWS WITH, initialised from the config on the same line and for the same
    // reason: the frame loop mirrors the binding into the runtime, so a config value that never reached the
    // binding would be overwritten by its default on the first frame.
    gui.goo_toon = settings.render.goo_toon;
    gui.megalights_enabled = settings.render.megalights;
    gui.megalights_samples = static_cast<float>(settings.render.megalights_samples);
    gui.megalights_spatial_sigma = settings.render.megalights_spatial_sigma;
    gui.megalights_history_tolerance = settings.render.megalights_history_tolerance;
    gui.sun_intensity = settings.render.sun_intensity; // a scale on the sun (see [render] sun_intensity)
    gui.megalights_bias = settings.render.megalights_bias;
    gui.megalights_light_angle = settings.render.megalights_light_angle;
    start_demo.set_megalights_light_angle(settings.render.megalights_light_angle);
    // WHICH FILTER RESOLVES A SCALED RENDER CHAIN ([render] upscale), applied ONCE here and deliberately NOT
    // mirrored into `gui` like the toggles above: it selects a shader path rather than a per-frame quantity, so
    // there is nothing to change between frames - and the render scale it works with is a creation option, so
    // the pair is a startup decision by construction.
    start_demo.set_upscale_filter(settings.render.upscale);
    gui.shadow_cascades = settings.render.shadow_cascades - 1;       // cascade combo index (0 = single map)
    gui.shadow_cascade_blend = settings.render.shadow_cascade_blend; // cascaded shadow maps (M4)
    gui.clustered_lights = settings.render.clustered_lights;         // clustered light culling (M5)
    gui.ssao_enabled = settings.render.ssao;                         // screen-space AO (M6)
    gui.ssao_radius = settings.render.ssao_radius;
    gui.ssao_intensity = settings.render.ssao_intensity;
    gui.ssao_samples = static_cast<float>(settings.render.ssao_samples);
    gui.anim_playing = animation.is_playing(); // play checkbox initial state
    gui.current_camera = current_camera;       // combo selection (the pose seeded above)

    // ---- authored (glTF) punctual lights -> the editable gui light slots ----
    // KHR_lights_punctual lights load straight into the gui slots (up to
    // deren::vulkan::max_punctual_lights): main pushes the enabled set through
    // deren::chores::apply_point_lights() every frame, so imported lights are adjustable in the overlay
    // like the demo ones. Position comes from the owning node's loader-space world matrix,
    // shifted by the same import offset the geometry got. KHR directional lights are NOT mapped:
    // the engine sun is the shadow-casting analytic light configured by enable_shadows() above -
    // the log keeps that miss visible instead of silent. The lights are fixed to the base pose
    // (an animated light node would need per-frame resolution - not wired yet).
    std::size_t imported_lights = 0;
    std::size_t imported_directional = 0;
    std::size_t imported_truncated = 0;
    for (auto const& [source, loader_node] : scenes->node_by_source) {
        if (!loader_node->light_index.has_value() || !animation.has_runtime_node(source)) {
            continue; // no light, or the node is absent from the imported tree
        }
        deren::gltf::light const& src = scenes->lights[*loader_node->light_index];
        if (src.type == deren::gltf::light_type::directional) {
            ++imported_directional;
            continue;
        }
        if (imported_lights >= deren::vulkan::max_punctual_lights) {
            ++imported_truncated;
            continue;
        }
        deren::chores::gui_bindings::light_slot& slot = gui.point_lights[imported_lights++];
        glm::vec3 const position = glm::vec3(loader_node->transform_matrix[3]) + scene_import_shift;
        slot.enabled = true;
        slot.position[0] = position.x;
        slot.position[1] = position.y;
        slot.position[2] = position.z;
        slot.color[0] = src.color.x;
        slot.color[1] = src.color.y;
        slot.color[2] = src.color.z;
        slot.intensity = src.intensity;
        slot.range = src.range.value_or(0.0f); // 0 = infinite falloff (UBO semantics)
        if (src.type == deren::gltf::light_type::spot) {
            slot.spot = true;
            glm::vec3 const dir = glm::mat3(loader_node->transform_matrix) * glm::vec3(0.0f, 0.0f, -1.0f); // glTF spot axis
            glm::vec3 const axis = glm::dot(dir, dir) > 1e-8f ? glm::normalize(dir) : glm::vec3(0.0f, -1.0f, 0.0f);
            slot.direction[0] = axis.x;
            slot.direction[1] = axis.y;
            slot.direction[2] = axis.z;
            float const outer = src.spot_outer_cone.value_or(glm::radians(45.0f)); // KHR default cone
            slot.outer_cone_deg = glm::degrees(outer);
            // inner cone: the KHR angle when authored, else the legacy soft-inner derived in
            // cosine space (mix(outerCos, 1, 0.6)) converted back to degrees for the slider
            float const inner_cos = src.spot_inner_cone.has_value()
                                        ? std::cos(*src.spot_inner_cone)
                                        : 0.6f + 0.4f * std::cos(outer);
            slot.inner_cone_deg = glm::degrees(std::acos(std::clamp(inner_cos, -1.0f, 1.0f)));
        }
    }
    if (imported_lights > 0) {
        deren::utility::log("KHR_lights_punctual: {} point/spot light(s) loaded into the editable gui light slots (base pose; adjustable in the overlay)",
                            imported_lights);
    }
    if (imported_directional > 0) {
        deren::utility::log("KHR_lights_punctual: {} directional light(s) ignored - the engine sun is enable_shadows()'s analytic light",
                            imported_directional);
    }
    if (imported_truncated > 0) {
        deren::utility::log("KHR_lights_punctual: {} additional light(s) dropped (GPU punctual-light cap = {})", imported_truncated, deren::vulkan::max_punctual_lights);
    }
    std::vector<std::string> gui_camera_names; // selector items: authored names (orbit added inside)
    gui_camera_names.reserve(authored_cameras.size());
    for (authored_camera const& ac : authored_cameras) {
        gui_camera_names.push_back(ac.camera->name.empty() ? "<unnamed>" : std::string(ac.camera->name));
    }
    deren::chores::setup_gui(runtime, use_gui, settings, gui, animation, gui_camera_names, seed_orbit_from_camera);

    // Per-frame cheap clock: stamp() once per presented frame on this (the frame owner) thread,
    // so any other thread can read the current frame time as a plain atomic load. Animation /
    // future parallel workers should prefer frame_clock.last_ns()/delta_ns() over now().
    deren::utility::frame_clock frame_clock;

    // All per-frame decisions (event polling, ESC/close response, minimize skip, swapchain
    // recreation on restore/resize) live inside the runtime's frame phases, which main calls at
    // fine granularity so it can write per-frame data (scene node locals -> culling, skin
    // matrices, morph weights) between pacing and recording.
    int32_t last_render_mode = 0; // gui render-mode combo (0 = pbr); applied between frames below
    // scripted capture: apply the camera override LAST, so nothing in the setup above (the orbit
    // framing of the imported scene, an authored glTF camera) can win over the requested view
    if (capture.camera) {
        runtime.camera.yaw = glm::radians((*capture.camera)[0]);
        runtime.camera.pitch = glm::radians((*capture.camera)[1]);
        runtime.camera.distance = (*capture.camera)[2];
        if (capture.target) {
            runtime.camera.target = *capture.target;
        }
        deren::utility::log("capture camera: yaw {:.1f} deg, pitch {:.1f} deg, distance {:.2f}, target ({:.2f}, {:.2f}, {:.2f})",
                            (*capture.camera)[0], (*capture.camera)[1], (*capture.camera)[2],
                            runtime.camera.target.x, runtime.camera.target.y, runtime.camera.target.z);
    }
    // The sweep's BASE pose is whatever the camera ended up as - the scene's own `camera_fit`, an authored
    // glTF camera, or the pinned `--capture-camera` above - so a sweep composes with all three instead of
    // demanding a pinned pose it would have to be told twice.
    float const sweep_base_yaw = runtime.camera.yaw;
    int32_t captured_frames = 0; // presented frames so far (scripted capture; see --capture-frames)
    while (true) {
        // Phase 1: poll window events (ESC / native close -> closed, minimized -> skipped)
        deren::vulkan::frame_status const polled = runtime.poll_events();
        if (polled == deren::vulkan::frame_status::closed || deren::vulkan::is_failure(polled)) {
            break;
        }
        if (polled == deren::vulkan::frame_status::skipped) {
            // Minimized: skip this frame's CPU work too; refresh the fps baseline so the pause
            // is not counted as one huge rendered frame.
            frame_stats.on_skipped();
            std::this_thread::yield();
            continue;
        }
        runtime.recreate_if_minimized();

        // Scripted capture: the camera sweep, advanced by the number of PRESENTED frames - a frame index,
        // not a clock reading, so two runs of one sweep are byte-identical (the harness's determinism run
        // is what verifies it). It has to happen before pace_and_acquire(), which is the phase that writes
        // the camera UBO.
        if (capture.sweep_yaw_deg_per_frame != 0.0f) {
            runtime.camera.yaw = sweep_base_yaw + glm::radians(capture.sweep_yaw_deg_per_frame) * static_cast<float>(captured_frames);
        }

        // Phase 2: pace + acquire the next frame slot. After pace_and_acquire() returns
        // proceed, this slot's previous submission has completed, so the per-frame host writes
        // below (scene node locals -> culling, skin matrices, morph weights) cannot race an
        // in-flight frame.
        deren::vulkan::frame_status const paced = runtime.pace_and_acquire();
        if (paced == deren::vulkan::frame_status::closed || deren::vulkan::is_failure(paced)) {
            break;
        }
        if (paced == deren::vulkan::frame_status::skipped) {
            // The swapchain is not usable this iteration - zero-sized (not sized yet / restored
            // minimized) so there are no attachments to render into, or it was recreated during the
            // acquire. Either way: skip this frame's CPU work too, like the minimized case above.
            // (There used to be a SECOND identical check here with a "recreated during acquire"
            // comment: unreachable, since the first one already covered it - both return the same
            // status. The reasons differ, the handling does not.)
            frame_stats.on_skipped();
            std::this_thread::yield();
            continue;
        }

        // drive the animation controller: sample the active animation into node locals (T/R/S +
        // morph weights) and rebuild the skin matrices, into the frame slot pace_and_acquire()
        // just paced. dt is the WALL CLOCK - clamped, so a pause (minimized / swapchain-recreate
        // gaps that never stamped) does not fast-forward the animation by the whole gap, and
        // playback resumes where it paused - UNLESS --capture-animation-sweep asked for a
        // frame-indexed clock instead, which is the only form a capture of a deforming mesh can
        // be gated on (see capture_options::animation_seconds_per_frame).
        float const dt = capture.animation_seconds_per_frame != 0.0f
                             ? capture.animation_seconds_per_frame
                             : static_cast<float>(std::min(frame_clock.delta_seconds(), 0.25));
        animation.update(dt);
        // ---- THE HEAD FRAME FOLLOWS THE BONE, published here because this is where the pose exists ----
        //
        // IT LANDS ONE FRAME LATE, and that is a consequence of the per-frame-slot arrangement rather than an
        // oversight: the runtime copies `head_state` into the paced slot INSIDE pace_and_acquire(), which has
        // already run by the time this frame's pose is produced. So a pose is shaded with the head frame
        // computed from the previous pose. For a pose that is standing still - which is what a capture gates on,
        // and what this model's rest pose is - the difference is exactly zero, and for a turning head it is one
        // frame of lag: the same trade every other per-frame write in this loop makes.
        //
        // THE EXTRACTION IS THE REFERENCE'S (`EfFaceGetHeadBasis`): `-row3` of the bone's world matrix is the
        // head's forward and `-row1` its right, where "row" is HLSL's and therefore a COLUMN of the
        // column-major matrix this engine stores. `head_basis_from_axes` negates, re-orthogonalises and falls
        // back when the bone is degenerate, so a zero matrix here cannot put NaNs in the sigmoid.
        if (head_joint.has_value()) {
            if (std::optional<glm::mat4> const bone = animation.joint_world(head_rig, *head_joint); bone.has_value()) {
                glm::mat4 const& joint = *bone;
                glm::vec3 const forward_row(joint[0][2], joint[1][2], joint[2][2]); // HLSL `_31_32_33`
                glm::vec3 const right_row(joint[0][0], joint[1][0], joint[2][0]);   // HLSL `_11_12_13`
                deren::gltf::head_basis const basis = deren::gltf::head_basis_from_axes(forward_row, right_row);
                // ---- AND THE HEAD'S OWN POSITION, FROM THE SAME MATRIX'S TRANSLATION COLUMN ----
                //
                // `Recalculate normal` needs `normalize(posWS - headCenter)`, and the reference gets `headCenter`
                // from an OBJECT's `Object Info.Location` (spec §5.1: `存储已命名属性.003 <- 物体信息(HC).Location`).
                // The equivalent here is the head BONE's world matrix translation - `joint[3]`, the same matrix the
                // two rows above come from - which `deren::gltf::head_basis` deliberately does not carry because every
                // consumer before this step wanted a direction. It is published UNCONDITIONALLY OF `basis.from_skeleton`
                // on purpose: the axes falling back to glTF's constants says nothing about where the head IS, and a
                // centre of `(0,0,0)` would make the sphere normal `normalize(posWS)`, i.e. a normal pointing away
                // from the world origin - a face shaded as if its head were at the origin of the scene.
                // `center.w = 1.0` IS THE FLAG THAT SAYS THE SPHERE NORMAL HAS A CENTRE (see `head_ubo::center`):
                // the initial publish below leaves it `0.0`, and on this repository's assets - every one of which
                // is a BAKED pose with `skins = 0` - this branch is never taken, so the face arm keeps the socket's
                // own default instead of building a normal about the world origin.
                glm::vec3 const head_center(joint[3][0], joint[3][1], joint[3][2]);
                runtime.set_head_basis(deren::vulkan::head_ubo{.front = glm::vec4(basis.front, 0.0f), .right = glm::vec4(basis.right, 0.0f), .up = glm::vec4(basis.up, 0.0f), .center = glm::vec4(head_center, 1.0f)});
                static bool logged_head_probe = false;
                if (!logged_head_probe) {
                    logged_head_probe = true;
                    deren::utility::log("head probe: bone forward row ({:.3f} {:.3f} {:.3f}) right row ({:.3f} {:.3f} {:.3f}) -> basis front ({:.3f} {:.3f} {:.3f}) from_skeleton {} center ({:.3f} {:.3f} {:.3f})",
                                        forward_row.x, forward_row.y, forward_row.z, right_row.x, right_row.y, right_row.z,
                                        basis.front.x, basis.front.y, basis.front.z, basis.from_skeleton,
                                        head_center.x, head_center.y, head_center.z);
                }
            } else {
                static bool logged_head_miss = false;
                if (!logged_head_miss) {
                    logged_head_miss = true;
                    deren::utility::log("head probe: joint_world({}, {}) returned NOTHING", head_rig, *head_joint);
                }
            }
        }
        gui.anim_time = animation.current_time();  // keep the gui time slider in sync
        gui.anim_playing = animation.is_playing(); // reflect controller-side pauses (scrub / select)
        gui.anim_index = static_cast<int32_t>(animation.current());

        // Phase 3: record + submit + present the paced frame
        deren::vulkan::frame_status const rec = runtime.begin_recording();
        if (rec == deren::vulkan::frame_status::closed || deren::vulkan::is_failure(rec)) {
            break;
        }
        runtime.record_main_drawcalls();
        deren::vulkan::frame_status const ended = runtime.end_recording();
        if (ended == deren::vulkan::frame_status::closed || deren::vulkan::is_failure(ended)) {
            break;
        }
        deren::vulkan::frame_status const result = runtime.submit_and_present();
        if (result == deren::vulkan::frame_status::closed || deren::vulkan::is_failure(result)) {
            break;
        }
        if (result == deren::vulkan::frame_status::skipped) {
            // present reported the swapchain out of date / recreated it: retry next iteration
            frame_stats.on_skipped();
            std::this_thread::yield();
            continue;
        }

        // A frame was presented: publish its stamp for cheap readers (frame_clock)
        frame_clock.stamp();

        // scripted capture: once the requested number of frames has been PRESENTED, request the
        // screenshot from the runtime - the F12 block below consumes the request in this same
        // iteration, so the capture happens on a fully warmed-up frame
        if (capture.frames > 0 && ++captured_frames >= capture.frames) {
            runtime.request_screenshot();
        }

        // gui "render mode": switch the runtime's default pipeline BETWEEN frames (the pipeline
        // registry must not be mutated while a frame records; this point is after submit, before
        // the next frame's recording). Default-semantics leaves re-shade on the next frame.
        if (gui.render_mode != last_render_mode) {
            last_render_mode = gui.render_mode;
            std::string_view const mode_name = gui.render_mode == 0 ? "pbr" : "unlit";
            runtime.set_default_pipeline(mode_name);
            // the lighting stage cannot switch pipelines per fragment, so tell it that the default
            // pipeline is the flat one - it then writes the stored albedo instead of shading, so
            // "unlit" means the same thing for the opaque scene and for the transparent pass
            start_demo.set_unlit(gui.render_mode == 1);
            deren::utility::log("render mode: {} ({})", mode_name, gui.render_mode == 0 ? "lit" : "unlit / flat");
        }

        // fps statistics: accumulate the frame gap into the rolling window
        frame_stats.tick();
        // punctual lights: push the gui slot set every frame, INDEPENDENT of the overlay being
        // visible - imported model lights were loaded into those slots, so they must stay lit in
        // headless-overlay runs too (the demo slots stay off unless the user enabled them)
        deren::chores::apply_point_lights(runtime, gui, demo_lights);
        runtime.set_exposure(gui.exposure);                                                     // gui exposure slider -> linear scale (post-process pass)
        runtime.set_bloom(gui.bloom_enabled ? gui.bloom_intensity : 0.0f, gui.bloom_threshold); // bloom checkbox + knobs -> post pass
        runtime.set_max_fps(config.settings.render.max_fps);                                    // 0 = uncapped (see config.example.toml)
        // FXAA: mirrored every frame like the other post-process values (the runtime clamps them and
        // ignores the flag when no fxaa pipeline was created)
        runtime.set_fxaa(gui.fxaa_enabled, gui.fxaa_subpixel, gui.fxaa_edge_threshold);
        // G-buffer debug view (the G-buffer's stored data): mirrored every frame like the FXAA state,
        // so the config, the overlay checkbox and the channel combo all take effect immediately
        runtime.set_gbuffer_debug(gui.gbuffer_debug);
        start_demo.set_gbuffer_channel(gui.gbuffer_channel);
        // TAA (the engine's anti-aliasing): mirrored like the other render toggles. The jitter
        // follows automatically - it is applied to the projection when TAA is active.
        start_demo.set_taa(gui.taa_enabled, gui.taa_blend_static, gui.taa_blend_min);
        // Stochastic punctual lighting: the overlay's switch and sample count, mirrored like the GI's - the two
        // bias terms are the pass's constants and are passed through at their shipped values.
        start_demo.set_megalights(gui.megalights_enabled, static_cast<uint32_t>(std::max(gui.megalights_samples, 1.0f) + 0.5f), 0.001f, 0.01f * gui.megalights_bias,
                                  0.1f * gui.megalights_bias);
        start_demo.set_megalights_light_angle(gui.megalights_light_angle);
        // ... and the chain's policy, so the overlay's own slider moves the spatial pre-filter live (0 = the
        // temporal-only chain, which is also the A/B the measurement uses).
        start_demo.set_megalights_accumulation(gui.megalights_history_tolerance, gui.megalights_frames, gui.megalights_spatial_sigma);

        // Order matters for the M5/M6 mirrors: their availability checks read the state the lines
        // above just set (the debug view replaces the lighting stage, clustered lighting only exists
        // when the cluster pipeline does), so mirroring them earlier would report a stale answer for
        // the first frame of every run.
        runtime.set_clustered_lights(gui.clustered_lights);
        // The toon character stage: mirrored like the other render toggles. Its own gate composes this knob
        // with the frame's opaque leaf list (feature_facts::character_forward_pending), so turning it on in a
        // frame with no opaque geometry records nothing rather than an empty instance.
        runtime.set_character_forward(gui.character_forward);
        // ... and WHICH CHAIN that stage shades with, mirrored on the line above's own terms - CPU-side, read while
        // the next frame is composed, so a toggle mid-run is seen by the frame after it (`runtime::set_goo_toon`).
        runtime.set_goo_toon(gui.goo_toon);
        start_demo.set_ssao(gui.ssao_enabled, gui.ssao_radius, gui.ssao_intensity, static_cast<uint32_t>(std::max(gui.ssao_samples, 0.0f) + 0.5f));
        // cel shading: the combo picks a discrete band count (index 0 = off); every entry is a
        // visibly different look, unlike a continuous strength that had dead zones between bands
        constexpr std::array<float, 7> toon_band_counts = {0.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};
        auto const toon_index = static_cast<std::size_t>(std::clamp(gui.toon_bands_index, 0, static_cast<int32_t>(toon_band_counts.size()) - 1));
        runtime.set_toon_shading(toon_band_counts[toon_index], gui.toon_softness);
        // The area light's radiance MULTIPLIES the sun, but ONLY while the emitter contributes energy
        // (`area_light_takes_over` = `area_light_size > 0 && area_light_irradiance`), so `area_light_scale` is
        // 1.0f for the default config and for the penumbra-only mode - i.e. exactly the line it always was.
        // IT MUST STAY ON THIS LINE, IN THIS BLOCK: the mirror is replayed EVERY FRAME, so scaling once at
        // startup would be overwritten by `gui.sun_intensity` from frame 2 onwards - and a capture at frame 40
        // would not show it, because every frame but the first would be back to 1.0x. `set_sun_intensity`
        // clamps to 0..3, so a radiance above 3.0 is a value the sun cannot express (see the
        // `area_light_intensity` note).
        runtime.set_sun_intensity(gui.sun_intensity * area_light_scale);
        // The emitter's geometry goes to the two appended light-UBO lanes. Idempotent, and deliberately
        // AFTER the shadow setup above: `enable_shadows` rebuilds the whole light UBO from the sun alone.
        runtime.set_area_light(glm::vec3(area_light.world_centre[0], area_light.world_centre[1], area_light.world_centre[2]),
                               area_light.half,
                               settings.lighting.area_light_irradiance,
                               glm::vec3(area_light.axis[0], area_light.axis[1], area_light.axis[2]),
                               area_light.penumbra);
        // The sun's DIRECTION is mirrored the same way, from the config (`[lighting] sun_direction`). It is
        // safe to set every frame: the runtime compares against the direction it already has and only
        // invalidates the shadow cascade fit when it actually moved.
        //
        // WITH AN AREA LIGHT CONTRIBUTING ENERGY (`area_light_takes_over`), THE EMITTER DECIDES THAT DIRECTION,
        // because "the soft box is the frame's main light" is the whole point: the shading, the cascades and
        // the sky's disc all follow it. With `irradiance = false` (penumbra only) the direction stays
        // `gui.sun_direction`, and with the default `size = 0` this is the line it always was. A consequence
        // worth knowing: this scene also carries a studio HDRI and a background dome, so the analytic sky's own
        // sun ends up behind the dome and is invisible - documented rather than worked around, and why the
        // environment stays the only other light in the frame.
        runtime.set_sun_direction(area_light_takes_over
                                      ? glm::vec3(area_light.to_light_dir[0], area_light.to_light_dir[1], area_light.to_light_dir[2])
                                      : glm::vec3(sun_direction[0], sun_direction[1], sun_direction[2]));
        // F12 screenshot: the runtime reports the request (edge-triggered in poll_events), main
        // captures the presented swapchain image and writes it as a PNG (dependency-free encoder)
        if (runtime.consume_screenshot_request()) {
            auto const image = runtime.acquire_current_frame_image();
            if (!image) {
                deren::utility::log("screenshot failed: {}", image.error());
            } else {
                // base directory from [paths] screenshot_dir (empty = the current working
                // directory); created on demand so a fresh checkout can capture immediately
                std::filesystem::path directory(settings.paths.screenshot_dir);
                if (!directory.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(directory, ec);
                    if (ec) {
                        deren::utility::log("screenshot: cannot create '{}' - saving to the working directory", directory.string());
                        directory.clear();
                    }
                }
                std::filesystem::path const path = directory / std::format("screenshot_{:%Y%m%d_%H%M%S}.png", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
                auto const written = deren::utility::write_png(path, image->width, image->height, image->rgba);
                if (written) {
                    deren::utility::log("screenshot saved: {} ({}x{})", path.string(), image->width, image->height);
                } else {
                    deren::utility::log("screenshot save failed: {}", written.error());
                }
                // ... AND THE POSE THAT FRAME WAS RENDERED WITH, in the form both the config and the
                // command line take. A screenshot without its camera is a picture nobody can reproduce,
                // which is how a user's "the face goes grey at some angle" cost a day of guessing.
                log_camera_pose("F12 screenshot");
            }
            // scripted capture: the frame is captured (saved or not - do not spin forever on a
            // failing writer), so leave the render loop and shut down cleanly
            if (capture.frames > 0) {
                break;
            }
        }
        // overlay fps mirror: updated unconditionally - the overlay can be hidden with F1 and
        // shown again at runtime, so its data must stay fresh even while it is not drawn
        gui.fps = frame_stats.smoothed_fps();
        if (frame_stats.window_rolled()) {
            // once per second: the fps log line stays for headless / non-gui runs; the overlay
            // shows the same number via smoothed_fps()
            deren::utility::log("fps: {:.1f} ({:.2f} ms/frame)", frame_stats.window_fps(), frame_stats.window_frame_ms());
            if (animation.has_active()) {
                // report the playback clock + the first animated node's evaluated translation
                // (proves the keyframes are actually moving the tree)
                std::string_view const node_name = animation.get_debug_node_name().empty()
                                                       ? std::string_view("<no target in scene>")
                                                       : animation.get_debug_node_name();
                deren::utility::log("  anim '{}': t={:.3f}s/{:.2f}s, '{}' at ({:.3f}, {:.3f}, {:.3f})",
                                    animation.active_name(), animation.current_time(), animation.loop_duration(), node_name,
                                    animation.get_debug_translation().x, animation.get_debug_translation().y, animation.get_debug_translation().z);
            }
            if (animation.is_skin_debug_valid()) {
                deren::utility::log("  skin '{}': last joint world x-axis ({:.3f}, {:.3f}, {:.3f})", animation.get_skin_debug_name(),
                                    animation.get_skin_debug_translation().x, animation.get_skin_debug_translation().y, animation.get_skin_debug_translation().z);
            }
        }
    }

    // The pose the session ENDED on, so a view reached by orbiting can be written down without
    // re-deriving it from the fit. Printed whether the loop ended by closing the window or by a
    // scripted capture finishing.
    log_camera_pose("exit");
    // 15. Wait for the GPU to finish; primitives and pipelines are released by the runtime destructor
    runtime->wait_idle();
    deren::utility::log("render loop finished");
    return 0;
}
