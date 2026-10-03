module;

// The global-module-fragment include below is load-bearing, not stylistic: with -fno-exceptions
// and the vendored std module, a chores implementation unit that instantiates std::vector (this
// file does, in load_shader) sees TWO 'operator new(size_t, align_val_t)' declarations - module
// std's and the textual libc++ copy baked into utility:data_block.pcm (data_block is the one
// module that never imports std; it includes libc++ headers in its own GMF). The result is
// 'call to operator new is ambiguous' at allocate.h. Textually including glm here (the same
// trick vulkan/animation/controller.cpp uses) makes clang merge the two copies, so
// the allocator instantiations resolve. Do not remove this include to "clean up".
#include <array>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>

module deren.chores;

namespace deren::chores {
    // Resolve the startup config in one step: merge config file + argv into the app settings,
    // then locate the shaders/ dir and pick the model file. Panics when a resource is missing.
    startup_config analyse_config(int32_t argc, char** argv) {
        // 1. Resolve startup settings first: config file (config.toml by default, --config <path>
        //    to override) merged with positional argv overrides. argv[1] = model, argv[2] = grid
        //    side (numeric).
        deren::app_config::app_settings const settings = deren::app_config::resolve_from_argv(argc, argv);
        if (!settings.config_file.empty()) {
            deren::utility::log("app_config: loaded startup settings from '{}'", settings.config_file);
        }

        startup_config config = {settings, {}, {}};

        // 2. Shaders directory (holds GLSL sources and compiled SPIR-V): explicit config path when
        //    given, otherwise walk up from the working directory to find shaders/.
        if (!settings.paths.shaders_dir.empty()) {
            config.shaders_dir = settings.paths.shaders_dir;
            if (!std::filesystem::is_directory(config.shaders_dir)) {
                deren::utility::panic(std::source_location::current(), "cannot find configured shaders_dir '{}'.", settings.paths.shaders_dir);
            }
        } else if (std::optional<std::filesystem::path> const located = locate_shaders_dir()) {
            config.shaders_dir = *located;
        } else {
            deren::utility::panic("cannot find shaders/ directory. run the program from the project root, pass shaders_dir in config.toml, or use a cmake-build-* directory.");
        }

        // 3. Pick the model file: `model = "ask"` opens the platform's own file dialog first (the config's
        //    way of saying "let me choose at startup"); otherwise settings.model when configured/argv-given;
        //    else the default model under settings.paths.model_dir (or the auto-located gltf_model/).
        if (deren::app_config::wants_model_dialog(settings)) {
            // A CANCELLED DIALOG IS NOT AN ERROR, and neither is a build with no way to ask: both fall
            // through to the same default chain an empty model takes, so the app still starts with
            // something on screen. The two are logged apart inside ask_open_file, because "I said no" and
            // "nobody could ask me" are different answers and only one of them is the user's decision.
            if (std::optional<std::filesystem::path> const chosen = deren::utility::ask_open_file("choose a glTF/GLB model to render", "*.glb;*.gltf")) {
                config.model_path = chosen->string();
            } else {
                deren::utility::log("model: 'ask' produced no file, using the default model");
            }
        } else if (!settings.model.empty()) {
            config.model_path = settings.model;
        }
        if (config.model_path.empty() && !settings.paths.model_dir.empty()) {
            config.model_path = (std::filesystem::path(settings.paths.model_dir) / "DamagedHelmet.gltf").string();
            if (!std::filesystem::is_regular_file(config.model_path)) {
                deren::utility::panic(std::source_location::current(), "cannot find model '{}' under configured model_dir '{}'.", "DamagedHelmet.gltf", settings.paths.model_dir);
            }
        } else if (config.model_path.empty()) {
            if (std::optional<std::filesystem::path> const located = locate_model_file()) {
                config.model_path = located->string();
            } else {
                deren::utility::panic("cannot find gltf_model/DamagedHelmet.gltf. run the program from the project root or pass a model path as argv[1]");
            }
        }

        return config;
    }

    // Read a single shader SPIR-V file and print info; panic on failure
    void load_shader(std::filesystem::path const& dir, std::string_view const file_name, std::vector<uint8_t>& out) {
        std::filesystem::path const path = dir / file_name;
        std::optional<std::vector<uint8_t>> const data = deren::utility::read_binary_to_vector(path);
        if (!data) {
            deren::utility::panic(std::source_location::current(), "cannot open shader file '{}'", path.string());
        }
        out = *data;
        deren::utility::log("loaded shader: {} ({} bytes)", path.string(), out.size());
    }

    // Walk up from the working directory to find the shaders/ directory,
    // so it works when run from the project root or a cmake-build-* directory
    std::optional<std::filesystem::path> locate_shaders_dir() {
        // The build's own output first: a shaders/ directory beside the executable, written by the
        // build's shader step. Checking this before the cwd walk is what makes a build-tree run load
        // the SPIR-V its build just compiled instead of resolving `shaders/` upward into the source
        // tree - the stale-binary trap the build no longer commits a fallback for.
        std::filesystem::path const exe_dir = deren::utility::executable_directory();
        if (!exe_dir.empty()) {
            std::filesystem::path const candidate = exe_dir / "shaders";
            if (std::filesystem::is_regular_file(candidate / "pbr.mesh.spv")) {
                return candidate;
            }
        }
        std::filesystem::path current = std::filesystem::current_path();
        for (int32_t depth = 0; depth < 4; ++depth) {
            std::filesystem::path candidate = current / "shaders";
            // The SAME probe as the executable-relative check above, deliberately: a directory named
            // `shaders` that holds no compiled SPIR-V (the source tree's own, since the build writes
            // the .spv into the build tree) is not the answer this function is looking for - returning
            // it only moves the failure into a much less clear "cannot open shader file" panic.
            if (std::filesystem::is_regular_file(candidate / "pbr.mesh.spv")) {
                return candidate;
            }
            std::filesystem::path const parent = current.parent_path();
            if (parent == current) {
                break;
            }
            current = parent;
        }
        return std::nullopt;
    }

    // Walk up from the working directory to find the default model under gltf_model/
    std::optional<std::filesystem::path> locate_model_file() {
        std::filesystem::path current = std::filesystem::current_path();
        for (int32_t depth = 0; depth < 4; ++depth) {
            std::filesystem::path candidate = current / "gltf_model" / "DamagedHelmet.gltf";
            if (std::filesystem::is_regular_file(candidate)) {
                return candidate;
            }
            std::filesystem::path const parent = current.parent_path();
            if (parent == current) {
                break;
            }
            current = parent;
        }
        return std::nullopt;
    }

    // Load a fragment/mesh SPIR-V pair for a named pipeline and create it via runtime; panic on failure. THE VERTEX
    // FILE IS GONE (docs/mesh_shaders.md step 4): a named geometry pipeline is built from a MESH stage and the
    // meshlet entry beside it, and the runtime refuses a name with no mesh module rather than falling back.
    void load_and_create_pipeline(deren::vulkan::runtime& runtime,
                                  std::filesystem::path const& shaders_dir,
                                  std::string_view const pipeline_name,
                                  std::string_view const fragment_file,
                                  std::string_view const mesh_file,
                                  std::string_view const meshlet_file = {}) {
        std::vector<uint8_t> fragment_code;
        std::vector<uint8_t> mesh_code;
        std::vector<uint8_t> meshlet_code;
        load_shader(shaders_dir, fragment_file, fragment_code);
        load_shader(shaders_dir, mesh_file, mesh_code);
        if (!meshlet_file.empty()) {
            load_shader(shaders_dir, meshlet_file, meshlet_code);
        }

        std::expected<void, std::string> const result = runtime.make_pipeline(pipeline_name, fragment_code, mesh_code, meshlet_code);
        if (!result) {
            deren::utility::panic(std::source_location::current(), "failed to create pipeline '{}': {}", pipeline_name, result.error());
        }
        deren::utility::log("SUCCESS: pipeline '{}' created and cached in the runtime", pipeline_name);
    }

    // Create the demo pipelines up front: the standard PBR pipeline (used by the imported scene)
    // and the directional shadow pass (depth-only). The legacy triangle demo pipeline is
    // deliberately not created here - nothing draws it anymore.
    void setup_pipeline(deren::vulkan::runtime& runtime, std::filesystem::path const& shaders_dir) {
        // Standard PBR pipeline: the imported scene's primitives bind to it (the FIRST pipeline
        // created becomes the runtime's implicit default)
        load_and_create_pipeline(runtime, shaders_dir, "pbr", "pbr.frag.spv", "pbr.mesh.spv", "pbr.meshlet.spv");
        // Non-PBR "unlit" pipeline: flat base color, no lighting/shadows/IBL (see unlit.frag).
        // Registered as a SECOND named pipeline - the scene tree's default-semantics leaves draw
        // with whatever the runtime default is, so switching set_default_pipeline() between
        // "pbr" and "unlit" (gui "render mode") re-shades the whole scene without re-baking.
        // It shares pbr's GEOMETRY (pbr.vert.spv or its pbr.mesh.spv form) and differs only in its fragment
        // stage, so it gets a mesh form of its own from the same file.
        load_and_create_pipeline(runtime, shaders_dir, "unlit", "unlit.frag.spv", "pbr.mesh.spv", "pbr.meshlet.spv");

        {
            // THE TOON CHARACTER STAGE'S PIPELINE, and it is registered through a DIFFERENT entry point on
            // purpose: `runtime::make_pipeline` above builds the forward family's pipeline (swapchain format,
            // src-alpha blending, depth LESS_OR_EQUAL), and this stage needs the opposite of all three - one
            // HDR target, no blending, depth compare EQUAL - so it goes through
            // `runtime::make_character_forward_pipeline` (see `core::make_character_forward_pipeline` for why
            // each of the three is forced).
            //
            // THE GEOMETRY IS pbr's, unchanged: it is the same leaves drawn a second time, so the varying
            // block the fragment stage reads (location 0/1/2 = world position, normal, uv) is the one
            // gbuffer.slang already reads, and the two passes cannot disagree about a surface's shape.
            //
            // The name is the one the character-forward pass binds through `render_environment::default_name`;
            // the pass is handed it rather than hardcoding it (see pass::character_forward_frame).
            std::vector<uint8_t> character_fragment_code;
            std::vector<uint8_t> character_mesh_code;
            std::vector<uint8_t> character_meshlet_code;
            load_shader(shaders_dir, "character_forward.frag.spv", character_fragment_code);
            load_shader(shaders_dir, "pbr.mesh.spv", character_mesh_code);
            load_shader(shaders_dir, "pbr.meshlet.spv", character_meshlet_code);
            runtime.register_shader("character_forward.frag.spv", character_fragment_code);
            auto const character_result = runtime.make_character_forward_pipeline("character_forward", character_fragment_code, character_mesh_code, character_meshlet_code);
            if (!character_result) {
                deren::utility::log("WARNING: the character-forward pipeline was not created ({}); the toon character stage will draw nothing", character_result.error());
            }
        }

        {
            // THE REWRITTEN TOON CHAIN'S PIPELINE (`goo_toon.slang`), registered under its own name so that
            // `[render] goo_toon` can select it at a frame's composition and nothing else about the frame changes.
            //
            // IT GOES THROUGH `runtime::make_character_forward_pipeline`, WHICH IS NOT AN OVERSIGHT: that entry
            // point's three forced states (ONE HDR target, blending off, depth compare EQUAL) are this stage's
            // exactly, so the two pipelines differ in ONE thing - which module the fragment stage came from - and
            // a second core-level builder would be a copy of one state block existing only to be called here. The
            // NAME is what the pass and the frame choose between (see `runtime::goo_toon_pipeline_name`).
            //
            // THE GEOMETRY IS pbr's, unchanged, on the character-forward pipeline's own terms: this is the same
            // leaves drawn a second time, so the varying block (location 0/1/2 = world position, normal, uv) is
            // the one both fragment stages read.
            //
            // A FAILURE IS NOT FATAL, and unlike the overlay and outline cases below it is not even a missing
            // feature: `make_character_forward_frame` falls back to the old pipeline name, so a device that
            // refuses this one draws the old chain rather than nothing - and `set_goo_toon` reports the fallback
            // once, when the knob is turned on.
            std::vector<uint8_t> goo_fragment_code;
            std::vector<uint8_t> goo_mesh_code;
            std::vector<uint8_t> goo_meshlet_code;
            load_shader(shaders_dir, "goo_toon.frag.spv", goo_fragment_code);
            load_shader(shaders_dir, "pbr.mesh.spv", goo_mesh_code);
            load_shader(shaders_dir, "pbr.meshlet.spv", goo_meshlet_code);
            runtime.register_shader("goo_toon.frag.spv", goo_fragment_code);
            auto const goo_result = runtime.make_character_forward_pipeline("goo_toon", goo_fragment_code, goo_mesh_code, goo_meshlet_code);
            if (!goo_result) {
                deren::utility::log("WARNING: the rewritten toon pipeline was not created ({}); [render] goo_toon will draw the old chain", goo_result.error());
            }
        }

        {
            // THE OVERLAY CHANNEL'S PIPELINE (`overlay.slang`): the article's two framebuffer multiplies, drawn by
            // the character-forward stage's overlay group. It is registered through a THIRD entry point because
            // its states are a third set again - ONE HDR target (like the toon stage), `dst = src * dst`
            // (like nothing else in the renderer) and depth compare LESS_OR_EQUAL (the toon stage's is EQUAL, and
            // an overlay mesh sits in FRONT of the surface it darkens, so EQUAL would reject it) - see
            // `core::make_overlay_pipeline`, which derives the blend from the article's `BlendOp Multiply` in core
            // Vulkan factors rather than reaching for a blend extension.
            //
            // THE GEOMETRY IS pbr's, unchanged, for the same reason the toon stage's is: an overlay is an ordinary
            // mesh (skinned and morphed like the face it is registered to), so the two passes cannot disagree about
            // a surface's shape.
            //
            // A FAILURE IS NOT FATAL, and that is a deliberate difference from the toon pipeline above: the pass
            // takes an EMPTY overlay name to mean "draw no overlay" (see character_forward_frame), so a device that
            // refuses this pipeline renders the character without its two masks - which is exactly the frame the
            // model had before they were merged into the asset - rather than drawing them as shaded quads.
            std::vector<uint8_t> overlay_fragment_code;
            std::vector<uint8_t> overlay_mesh_code;
            std::vector<uint8_t> overlay_meshlet_code;
            load_shader(shaders_dir, "overlay.frag.spv", overlay_fragment_code);
            load_shader(shaders_dir, "pbr.mesh.spv", overlay_mesh_code);
            load_shader(shaders_dir, "pbr.meshlet.spv", overlay_meshlet_code);
            runtime.register_shader("overlay.frag.spv", overlay_fragment_code);
            auto const overlay_result = runtime.make_overlay_pipeline("overlay", overlay_fragment_code, overlay_mesh_code, overlay_meshlet_code);
            if (!overlay_result) {
                deren::utility::log("WARNING: the overlay pipeline was not created ({}); the character's eye and hair shadows will not be multiplied", overlay_result.error());
            }
        }

        {
            // THE ARTICLE'S ① 描边 (INVERTED HULL) PIPELINE (`outline.slang` + `pbr.slang`'s outline mesh
            // entry), drawn by the character-forward stage's outline group. It goes through a FOURTH entry point
            // because its states are a fourth set: ONE HDR target (like the toon stage), an OPAQUE blend - the
            // article's `MyZmdOutlineShader` states no `Blend` at all - and depth compare LESS_OR_EQUAL (the toon
            // stage's is EQUAL, which would reject the whole outer ring, since a pushed-out hull is not the
            // surface the G-buffer recorded) - see `core::make_outline_pipeline`, which states each of those.
            //
            // THE GEOMETRY IS THE OUTLINE'S OWN MESH STAGE, not pbr's: pushing each vertex outward in clip space
            // is the one thing that turns a second copy of a mesh into an outline, and it lives in the
            // `outline_mesh_main` entry of `pbr.slang` (see `shaders/outline.slang` for why the fragment entry is
            // `frag_main`).
            //
            // THERE IS NO MESHLET FORM, AND THAT IS DELIBERATE RATHER THAN AN OMISSION:
            // `build-release-clang64/shaders/outline.meshlet.spv` is residue from an earlier experiment that no
            // build rule produces (`outline_plan.md` §10), so wiring it would register a stage this build never
            // compiled. A leaf that would otherwise dispatch per meshlet falls back to the mesh form (see the
            // bind in `runtime.frames.cppm`), which costs throughput and not correctness.
            //
            // A FAILURE IS NOT FATAL, on the same terms as the overlay's: the pass takes an EMPTY outline name to
            // mean "draw no outline" (see character_forward_frame), so a device that refuses this pipeline
            // renders exactly the frame this port produced before ① landed - rather than drawing hulls with the
            // toon pipeline, which neither culls front faces nor tests with LESS_OR_EQUAL and would paint whole
            // shaded surfaces over the character.
            std::vector<uint8_t> outline_fragment_code;
            std::vector<uint8_t> outline_mesh_code;
            load_shader(shaders_dir, "outline.frag.spv", outline_fragment_code);
            load_shader(shaders_dir, "outline.mesh.spv", outline_mesh_code);
            runtime.register_shader("outline.frag.spv", outline_fragment_code);
            auto const outline_result = runtime.make_outline_pipeline("outline", outline_fragment_code, outline_mesh_code);
            if (!outline_result) {
                deren::utility::log("WARNING: the outline pipeline was not created ({}); the character will be drawn without its ① outline", outline_result.error());
            }
        }

        {
            // THE SCREEN-SPACE DEPTH RIM'S STAGE. Only the two shaders are registered here: the pass builds its
            // own pipeline in its `create` step (it is a fullscreen stage with one additive target and no depth
            // attachment, which is nothing `runtime::make_pipeline`'s forward family describes), so there is no
            // named pipeline to create and no registration path of its own - the same arrangement the deferred
            // and TAA stages have.
            std::vector<uint8_t> rim_fragment_code;
            load_shader(shaders_dir, "toon_screen_rim.frag.spv", rim_fragment_code);
            runtime.register_shader("toon_screen_rim.frag.spv", rim_fragment_code);
        }

        {
            // THE REWRITTEN CHAIN'S RIM (`goo_rim.slang`), registered exactly as the article's contour above and
            // for the same reason: it is a fullscreen stage with ONE additive target and no depth attachment, so
            // its pass builds its own pipeline in `create` and there is no named pipeline here. The two rims are
            // mutually exclusive at run time (the rewritten chain silences the article's), which is why both are
            // registered unconditionally and neither is a fallback for the other.
            std::vector<uint8_t> goo_rim_fragment_code;
            load_shader(shaders_dir, "goo_rim.frag.spv", goo_rim_fragment_code);
            runtime.register_shader("goo_rim.frag.spv", goo_rim_fragment_code);
        }

        {
            // THE RESOLVE'S STAGE. Only the fragment shader is registered here, for the same reason as the rim's
            // above: the pass builds its own pipeline in its `create` step, from post.vert.spv (the synthetic
            // triangle, registered with the post chain below) and upscale.frag.spv - and that has to have happened
            // before create_passes(), which is where this registration sits.
            std::vector<uint8_t> upscale_fragment_code;
            load_shader(shaders_dir, "upscale.frag.spv", upscale_fragment_code);
            runtime.register_shader("upscale.frag.spv", upscale_fragment_code);
        }

        {
            // The post chain is a PASS PAIR now (deren.vulkan.pass.post): the composite owns the chain's two pipelines,
            // and the four bloom levels record with them. So the app REGISTERS the two shaders the pass builds
            // from (post.vert's synthetic triangle and post.frag, whose `mode` lane selects the stage) and the pass
            // does the rest in create_passes() below.
            //
            // THE SAMPLERS ARE THE DEVICE ROOT'S (`core::create_samplers`), so there is nothing to create here any
            // more - the app registers the two shaders the post chain's passes build from.
            std::vector<uint8_t> vertex_code;
            std::vector<uint8_t> fragment_code;
            load_shader(shaders_dir, "post.vert.spv", vertex_code);
            load_shader(shaders_dir, "post.frag.spv", fragment_code);
            runtime.register_shader("post.vert.spv", vertex_code);
            runtime.register_shader("post.frag.spv", fragment_code);
            // ... and FXAA's, whose pass builds its own pipeline from its fragment shader and post.vert - which is
            // why this registration sits HERE, before create_passes(), and not after it: the pass is created with
            // every other pass now, and the ordering constraint the old make_fxaa_pipeline() call needed is gone
            // with the call.
            load_shader(shaders_dir, "fxaa.frag.spv", fragment_code);
            runtime.register_shader("fxaa.frag.spv", fragment_code);
        }

        {
            // The shadow pass is a PASS (deren.vulkan.pass.shadow): the app REGISTERS its two shaders and the pass builds
            // the depth-only pipeline itself, from them and the context's depth format - which is why there is no
            // make_* here any more. Optional: without the pipeline the scene simply renders without shadows.
            std::vector<uint8_t> fragment_code;
            std::vector<uint8_t> mesh_code;
            load_shader(shaders_dir, "shadow.frag.spv", fragment_code);
            runtime.register_shader("shadow.frag.spv", fragment_code);
            // ... and the MESH stage, which is the pass's ONLY geometry stage since step 4 (docs/mesh_shaders.md):
            // `shadow.vert.spv` is neither compiled nor registered any more, so the two below are what the pass
            // builds from.
            load_shader(shaders_dir, "shadow.mesh.spv", mesh_code);
            runtime.register_shader("shadow.mesh.spv", mesh_code);
            // ... and the MESHLET form of the same pass (docs/mesh_shaders.md step 3): one workgroup per meshlet.
            load_shader(shaders_dir, "shadow.meshlet.spv", mesh_code);
            runtime.register_shader("shadow.meshlet.spv", mesh_code);
        }

        {
            // Clustered light culling compute pass (M5): one dispatch per frame sorts the punctual
            // lights into the screen-tile x depth-slice grid the shading stage then reads. Optional -
            // without it (or with [render] clustered_lights = false) shade_surface() loops every
            // active light, which is the brute-force reference the clustered path is verified on.
            // IT IS A PASS: the app registers the shader and the pass builds its own compute pipeline from it (see
            // deren.vulkan.pass.cluster) - create_passes() below runs that step, and
            // the pass logs its own outcome. It has to be registered HERE, before that call, because a pass
            // created before its shader exists builds nothing and says so.
            std::vector<uint8_t> compute_code;
            load_shader(shaders_dir, "light_cluster.comp.spv", compute_code);
            runtime.register_shader("light_cluster.comp.spv", compute_code);
        }

        {
            // G-buffer pair (the deferred path's first half): the surface-writing pipeline the
            // opaque pass binds when it writes the G-buffer, and the fullscreen debug view that
            // turns one stored channel into a visible image. Both optional - without them
            // runtime::set_gbuffer_debug() has no effect and the opaque pass shades into the HDR target directly.
            std::vector<uint8_t> fragment_code;
            std::vector<uint8_t> mesh_code;
            load_shader(shaders_dir, "pbr.mesh.spv", mesh_code); // the G-buffer pass's geometry stage, and the only one it has
            load_shader(shaders_dir, "gbuffer.frag.spv", fragment_code);
            // ... and the MESHLET form (docs/mesh_shaders.md step 3): one workgroup per meshlet, camera-culled. The
            // runtime prefers it and falls back to the mesh form, which is now the pass's REQUIREMENT - its vertex
            // form went with the rest of the vertex geometry path (step 4).
            std::vector<uint8_t> meshlet_code;
            load_shader(shaders_dir, "pbr.meshlet.spv", meshlet_code);
            auto const gbuffer_result = runtime.make_gbuffer_pipeline(fragment_code, mesh_code, meshlet_code);

            if (!gbuffer_result) {
                deren::utility::log("gbuffer pipeline disabled: {}", gbuffer_result.error());
            } else {
                // The debug view is a PASS (deren.vulkan.pass.geometry_buffer_debug): the app registers its two shaders and the
                // pass builds its pipeline, which is all it owns. The samplers those declarations
                // choose between are the device root's now (`core::create_samplers`).
                // ... AND THIS IS `post.vert.spv`, NOT A GEOMETRY STAGE: a synthetic fullscreen triangle, which is a
                // vertex stage by nature and stays one (docs/mesh_shaders.md step 4 is about the geometry path).
                std::vector<uint8_t> vertex_code;
                load_shader(shaders_dir, "post.vert.spv", vertex_code);
                load_shader(shaders_dir, "gbuffer_debug.frag.spv", fragment_code);
                runtime.register_shader("post.vert.spv", vertex_code);
                runtime.register_shader("gbuffer_debug.frag.spv", fragment_code);
                // The deferred lighting stage is a PASS (deren.vulkan.pass.deferred): the app REGISTERS the two
                    // shaders it builds from and the pass builds its own pipeline from them, which is why one
                    // register call for each replaces the old `make_deferred_pipeline(vertex_code, fragment_code)`
                    // call here.
                    load_shader(shaders_dir, "post.vert.spv", vertex_code);
                    load_shader(shaders_dir, "deferred.frag.spv", fragment_code);
                    runtime.register_shader("post.vert.spv", vertex_code);
                    runtime.register_shader("deferred.frag.spv", fragment_code);
                    // TAA resolve (deferred-only). IT IS A PASS TOO: the app registers the two shaders and the
                    // pass builds its own pipeline from them (see deren.vulkan.pass.taa) - the create_passes() call
                    // below is what runs that step, for every pass at once. The vertex
                    // stage is post.vert's synthetic triangle, the same one the debug view and the post chain use.
                    //
                    // NOTE WHERE THESE TWO REGISTRATIONS SIT: they used to be in the ELSE branch of the deferred
                    // pipeline's creation, so a machine where that failed silently lost TAA as well. They are
                    // gated on the DEBUG pipeline now, which is what they actually need (post.vert), and the
                    // lighting stage's own shaders are registered above it.
                    load_shader(shaders_dir, "taa.frag.spv", fragment_code);
                    runtime.register_shader("taa.frag.spv", fragment_code);
                    deren::utility::log("SUCCESS: gbuffer pipelines created (surface write, debug view)");
            }
        }

        {
            // Stochastic punctual lighting (docs/megalights.md): sample a few of each pixel's clustered lights
            // and trace one visibility ray per sample. OPTIONAL - without it the lighting stage's raster
            // punctual loop stands, which is the unshadowed path every frame before this feature existed had,
            // and `runtime::megalights_active()` then keeps the frame from recording the pass (so the knob-off
            // frame is byte-identical by construction: the gate's scenarios were verified to that).
            //
            // IT IS A PASS: the app registers the shader and the pass builds its own compute pipeline
            // from it (see deren.vulkan.pass.megalights_trace) - `create_passes()` below runs that step.
            std::vector<uint8_t> megalights_code;
            load_shader(shaders_dir, "megalights_trace.comp.spv", megalights_code);
            runtime.register_shader("megalights_trace.comp.spv", megalights_code);
            // ... and the chain's temporal resolve, required for the reason any two-pass chain requires its
            // second: what the lighting stage adds is the ACCUMULATION, so a chain whose resolve is missing has
            // nothing to add and runtime::megalights_active() stays false.
            std::vector<uint8_t> megalights_temporal_code;
            load_shader(shaders_dir, "megalights_temporal.comp.spv", megalights_temporal_code);
            runtime.register_shader("megalights_temporal.comp.spv", megalights_temporal_code);

            // Ray-traced sun shadows: one ray per pixel against the scene's acceleration structures. IT IS A
            // PASS, so its shaders have to be registered BEFORE create_passes() below - the pass builds its own
            // ray-tracing pipeline from them (see deren.vulkan.pass.ray_traced_shadow), and a pass created
            // before its shaders exist builds nothing and says so. Optional, and the builder refuses on a device
            // without a ray-tracing pipeline: without it the cascaded shadow maps keep running.
            std::vector<uint8_t> rt_shadow_raygen_code;
            load_shader(shaders_dir, "rt_shadow.rgen.spv", rt_shadow_raygen_code);
            runtime.register_shader("rt_shadow.rgen.spv", rt_shadow_raygen_code);
            std::vector<uint8_t> rt_shadow_closest_hit_code;
            load_shader(shaders_dir, "rt_shadow.rchit.spv", rt_shadow_closest_hit_code);
            runtime.register_shader("rt_shadow.rchit.spv", rt_shadow_closest_hit_code);
            std::vector<uint8_t> rt_shadow_miss_code;
            load_shader(shaders_dir, "rt_shadow.rmiss.spv", rt_shadow_miss_code);
            runtime.register_shader("rt_shadow.rmiss.spv", rt_shadow_miss_code);
            // The any-hit stage: the second stage of the SAME hit group, and the only place an alphaMode MASK
            // surface can be told apart from its bounding triangles (see shaders/rt_shadow.rahit).
            std::vector<uint8_t> rt_shadow_any_hit_code;
            load_shader(shaders_dir, "rt_shadow.rahit.spv", rt_shadow_any_hit_code);
            runtime.register_shader("rt_shadow.rahit.spv", rt_shadow_any_hit_code);


            // The alphaMode MASK bake (shaders/mask_bake.slang) and the compute skinning job
            // (shaders/compute_skin.slang) are TWO JOBS rather than frame passes - one runs once inside the
            // command buffer that builds the bottom level structures (without it a MASK surface is solid to
            // a ray), the other re-skins the casters and refits their structures per frame (without it a
            // skinned caster's traced shadow is cast by its BIND POSE). Both are built by `create_passes()`
            // below, from the SAME context and the same resource channel every pass gets (see
            // deren.vulkan.pass.mask_bake and deren.vulkan.pass.compute_skin), so their shaders are registered here too.
            std::vector<uint8_t> mask_bake_code;
            load_shader(shaders_dir, "mask_bake.comp.spv", mask_bake_code);
            runtime.register_shader("mask_bake.comp.spv", mask_bake_code);
            std::vector<uint8_t> compute_skin_code;
            load_shader(shaders_dir, "compute_skin.comp.spv", compute_skin_code);
            runtime.register_shader("compute_skin.comp.spv", compute_skin_code);

            // The HEAP-NATIVE PROBE (shaders/heap_probe_comp.slang): registered like the jobs above, and for a purpose
            // of the same kind - it is not part of any frame, it runs once at scene setup in a command buffer of
            // its own (runtime::run_heap_probe) and its answer is a log line. It exists because the migration's
            // four assumptions about the native path (a heap-flagged pipeline with NO layout, `descriptor_heap`
            // declarations, a sampler taken from the sampler heap, parameters through vkCmdPushDataEXT) cannot be
            // tested by a picture until the whole frame is converted.
            std::vector<uint8_t> heap_probe_code;
            load_shader(shaders_dir, "heap_probe.comp.spv", heap_probe_code);
            runtime.register_shader("heap_probe.comp.spv", heap_probe_code);
            // ... and its GRAPHICS half: the same read through a graphics pipeline, which is a different question
            // (a fragment stage reading the heap, and a pipeline created with the flag and no layout).
            std::vector<uint8_t> heap_probe_vertex_code;
            load_shader(shaders_dir, "heap_probe.vert.spv", heap_probe_vertex_code);
            runtime.register_shader("heap_probe.vert.spv", heap_probe_vertex_code);
            std::vector<uint8_t> heap_probe_fragment_code;
            load_shader(shaders_dir, "heap_probe.frag.spv", heap_probe_fragment_code);
            runtime.register_shader("heap_probe.frag.spv", heap_probe_fragment_code);
            // ... and its MESH half (docs/mesh_shaders.md step 0): the same triangle, emitted by a mesh stage
            // through a heap-native pipeline. A mesh stage is a different stage type, so this binary has a name
            // of its own rather than replacing the vertex one.
            std::vector<uint8_t> heap_probe_mesh_code;
            load_shader(shaders_dir, "heap_probe.mesh.spv", heap_probe_mesh_code);
            runtime.register_shader("heap_probe.mesh.spv", heap_probe_mesh_code);

            // ... and now that every pass's and every job's shaders are registered, the pipelines can be built.
            // This block is where the SHADERS come from and nothing else: the ONE create step that builds what the
            // passes and the jobs own (a pipeline each) runs in main(), after the application has handed its chain
            // over (`render_start_demo`) - see `runtime::create_passes`, which logs each object's own outcome.
        }
    }

    // Optional instancing stress: grid_side > 1 (config or argv) draws the first imported
    // primitive as a grid_side x grid_side grid in ONE instanced draw call (an
    // instanced_draw_primitive appended to the scene tree - the frame loop is untouched)
    void add_instancing_grid(deren::vulkan::runtime& runtime, int32_t const grid_side, float const scene_radius) {
        if (grid_side <= 1) {
            return;
        }
        std::vector<deren::vulkan::primitive const*> const pbr_primitives = runtime.get_primitives("pbr");
        if (pbr_primitives.empty()) {
            return;
        }
        deren::vulkan::primitive const& source = *pbr_primitives[0];
        std::vector<glm::mat4> transforms;
        transforms.reserve(static_cast<size_t>(grid_side) * grid_side);
        float const spacing = 2.5f * scene_radius; // keep instances apart: measure draw scaling, not overdraw
        for (int32_t i = 0; i < grid_side; ++i) {
            for (int32_t j = 0; j < grid_side; ++j) {
                float const dx = (static_cast<float>(i) - static_cast<float>(grid_side - 1) * 0.5f) * spacing;
                float const dz = (static_cast<float>(j) - static_cast<float>(grid_side - 1) * 0.5f) * spacing;
                transforms.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(dx, 0.0f, dz)) * source.push.model);
            }
        }
        runtime.make_instanced_primitive(source, transforms);
        deren::utility::log("instancing stress: {} x {} grid ({} instances, 1 draw call)", grid_side, grid_side, transforms.size());
    }

    // Build the demo's Dear ImGui debug overlay (when use_gui): enable it on the runtime and
    // assemble the "deren debug" panel. The widgets bind to @p bindings (fps text,
    // toggles, sliders, animation mirrors, camera selection) - the frame loop keeps the fps
    // and animation mirrors in sync. The overlay's glTF-side content (authored camera names,
    // orbit-camera seeding) arrives as display names + a selection callback, so this helper
    // never touches glTF types.
    void setup_gui(deren::vulkan::runtime& runtime,
                   bool const use_gui,
                   deren::app_config::app_settings const& settings,
                   gui_bindings& bindings,
                   deren::vulkan::animation::controller& animation,
                   std::vector<std::string> const& camera_names,
                   std::function<void(int32_t)> const& on_camera_selected) {
        if (!use_gui) {
            return;
        }
        runtime.enable_debug_gui();
        deren::vulkan::gui::debug_panel& panel = runtime.debug_gui().add_panel("deren debug");
        panel.set_default_size(settings.gui.panel_width, settings.gui.panel_height);
        panel.push_back(std::make_unique<deren::vulkan::gui::label_widget>([&bindings] { return std::format("fps: {:>6.1f}", bindings.fps); })); // fixed-width field: a growing number must not re-wrap the panel
        // per-pass GPU milliseconds (runtime::gpu_timing_summary): the timing that steers the
        // renderer's performance work, so it sits with the fps line at the top of the panel
        panel.push_back(std::make_unique<deren::vulkan::gui::label_widget>([&runtime] { return runtime.gpu_timing_summary(); }));
        // ... and the CPU phases next to it: above a few hundred fps the frame is CPU/pacing-bound,
        // so the GPU line alone no longer explains the frame time (see runtime::cpu_phase).
        panel.push_back(std::make_unique<deren::vulkan::gui::label_widget>([&runtime] { return runtime.cpu_timing_summary(); }));
        panel.push_back(std::make_unique<deren::vulkan::gui::checkbox_widget>(
            "frustum culling",
            &bindings.cull_enabled,
            [&runtime](bool const enabled) { runtime.set_frustum_culling(enabled); }));
        panel.push_back(std::make_unique<deren::vulkan::gui::checkbox_widget>(
            "shadow",
            &bindings.shadow_enabled,
            [&runtime](bool const enabled) { runtime.set_shadow_enabled(enabled); }));
        // clustered light culling (M5): off = every active light is evaluated per pixel (the
        // brute-force reference), on = only the pixel's cluster list. Mirroring it every frame in
        // main() keeps the config and the checkbox in agreement. Offered only when the cluster
        // compute pipeline exists - without it the switch cannot do anything.
        {
            auto clustered = std::make_unique<deren::vulkan::gui::checkbox_widget>("clustered lights", &bindings.clustered_lights);
            clustered->visible_when = [&runtime] { return runtime.feature_available("clustered"); }; // offered whenever the compute pass exists (it works in either path)
            panel.push_back(std::move(clustered));
        }
        // screen-space ambient occlusion (M6): the deferred lighting stage traces the G-buffer, so the
        // whole group (switch + its three knobs) is offered only in a session whose G-buffer pipelines
        // exist - an AVAILABILITY question, which is what visible_when asks (a widget never gates on
        // feature_active: that is the runner's per-frame "this stage ran", so gating on it made the group
        // vanish the moment the user switched to unlit). The knobs then follow the switch's OWN state,
        // which is what they are attached to. The sliders edit the radius (world units), the applied
        // intensity and the sample count.
        {
            auto ssao = std::make_unique<deren::vulkan::gui::checkbox_widget>("ssao", &bindings.ssao_enabled);
            ssao->visible_when = [&runtime] { return runtime.feature_available("deferred"); }; // deferred-only, via the feature registry
            panel.push_back(std::move(ssao));
            auto make_ssao_slider = [&](std::string label, float* value, float lo, float hi) {
                auto slider = std::make_unique<deren::vulkan::gui::slider_widget>(std::move(label), value, lo, hi);
                slider->visible_when = [&bindings] { return bindings.ssao_enabled; };
                panel.push_back(std::move(slider));
            };
            make_ssao_slider("ssao radius", &bindings.ssao_radius, 0.05f, 3.0f);
            make_ssao_slider("ssao intensity", &bindings.ssao_intensity, 0.0f, 1.0f);
            make_ssao_slider("ssao samples", &bindings.ssao_samples, 1.0f, 16.0f);
        }
        // stochastic punctual lighting: the shadows the point and spot lights never had (docs/megalights.md).
        // The SWITCH is offered whenever the chain exists this session (an availability fact, and NOT the
        // switch's own value - see the note below); the knobs follow the switch. Mirrored into the runtime
        // every frame by main like the rest.
        // The SAMPLE COUNT is the estimator's ray budget per half-resolution pixel: cost and noise both scale
        // with it, which is why it sits next to the switch rather than in the config alone.
        {
            auto megalights = std::make_unique<deren::vulkan::gui::checkbox_widget>("megalights", &bindings.megalights_enabled);
            // the SWITCH is gated on AVAILABILITY, never on its own value: the checkbox writes the field its
            // predicate reads, so gating it on bindings.megalights_enabled hides the only way back - the
            // feature defaults off (app_config's render_settings::megalights, and the panel is the only UI
            // that sets it), so the panel would offer megalights exactly never.
            megalights->visible_when = [&runtime] { return runtime.feature_available("megalights"); };
            panel.push_back(std::move(megalights));
            auto samples = std::make_unique<deren::vulkan::gui::slider_widget>("ml samples", &bindings.megalights_samples, 1.0f, 4.0f);
            auto ml_frames = std::make_unique<deren::vulkan::gui::slider_widget>("ml history", &bindings.megalights_frames, 1.0f, 12.0f);
            auto ml_tol = std::make_unique<deren::vulkan::gui::slider_widget>("ml tol", &bindings.megalights_history_tolerance, 0.0f, 0.5f);
            auto ml_bias = std::make_unique<deren::vulkan::gui::slider_widget>("ml bias", &bindings.megalights_bias, 0.0f, 16.0f);
            auto ml_emitter = std::make_unique<deren::vulkan::gui::slider_widget>("ml emitter", &bindings.megalights_light_angle, 0.0f, 0.1f);
            ml_emitter->visible_when = [&bindings] { return bindings.megalights_enabled; };
            panel.push_back(std::move(ml_emitter));
            ml_bias->visible_when = [&bindings] { return bindings.megalights_enabled; };
            panel.push_back(std::move(ml_bias));
            ml_tol->visible_when = [&bindings] { return bindings.megalights_enabled; };
            panel.push_back(std::move(ml_tol));
            ml_frames->visible_when = [&bindings] { return bindings.megalights_enabled; };
            panel.push_back(std::move(ml_frames));
            auto ml_sigma = std::make_unique<deren::vulkan::gui::slider_widget>("ml sigma", &bindings.megalights_spatial_sigma, 0.0f, 4.0f);
            ml_sigma->visible_when = [&bindings] { return bindings.megalights_enabled; };
            panel.push_back(std::move(ml_sigma));
            samples->visible_when = [&bindings] { return bindings.megalights_enabled; };
            panel.push_back(std::move(samples));
        }
        // render mode: pbr (lit) vs unlit (flat base color, no shading). Default-semantics leaves
        // draw with the runtime's default pipeline, so this only records a combo selection here;
        // main() applies it BETWEEN frames via runtime.set_default_pipeline (the registry may
        // not be mutated while a frame records).
        panel.push_back(std::make_unique<deren::vulkan::gui::combo_widget>(
            "render mode",
            std::vector<std::string>{"pbr (lit)", "unlit (flat)"},
            &bindings.render_mode));
        // selectable BRDF theory models (pbr.frag): preset 0 is the default GGX + joint-Smith;
        // each other preset differs by exactly one piece (NDF or visibility), so the gui is a
        // live A/B comparison. CPU-side write-through (safe mid-run, see runtime::set_brdf_model).
        panel.push_back(std::make_unique<deren::vulkan::gui::combo_widget>(
            "brdf model",
            std::vector<std::string>{"GGX + joint Smith", "GGX + height-corr. Smith", "Beckmann + Smith", "Blinn-Phong + Smith"},
            &bindings.brdf_model,
            [&runtime](int32_t const index) { runtime.set_brdf_model(index); }));
        panel.push_back(std::make_unique<deren::vulkan::gui::combo_widget>(
            "diffuse model",
            std::vector<std::string>{"Lambert", "Oren-Nayar"},
            &bindings.diffuse_model,
            [&runtime](int32_t const index) { runtime.set_diffuse_model(index); }));
        // linear exposure applied before tonemapping (pbr.frag + skybox.frag); main pushes it
        // into the runtime every frame like the light slots
        panel.push_back(std::make_unique<deren::vulkan::gui::slider_widget>("exposure", &bindings.exposure, 0.1f, 5.0f));
        // bloom (bright-pass threshold + blend weight); 0 intensity disables it
        // the useful ranges: a threshold above ~0.75 leaves almost no pixel over it (so nothing
        // glows), and the intensity needed for a visible glow grows with the threshold - keeping
        // the threshold low is what makes the whole intensity slider effective
        panel.push_back(std::make_unique<deren::vulkan::gui::checkbox_widget>("bloom", &bindings.bloom_enabled));
        // the two knobs only matter while the chain runs (main pushes a 0 intensity when the box is clear)
        {
            auto bloom_knob = [&](std::string label, float* value, float lo, float hi) {
                auto slider = std::make_unique<deren::vulkan::gui::slider_widget>(std::move(label), value, lo, hi);
                slider->visible_when = [&bindings] { return bindings.bloom_enabled; };
                panel.push_back(std::move(slider));
            };
            bloom_knob("bloom intensity", &bindings.bloom_intensity, 0.0f, 3.0f);
            bloom_knob("bloom threshold", &bindings.bloom_threshold, 0.0f, 0.75f);
        }
        // FXAA: a checkbox plus its two shader knobs (main mirrors all three into the runtime every
        // frame). The knobs are genuine effects, not strength padding - "subpixel" trades edge
        // smoothing for the single-pixel sparkle FXAA leaves on near-axis-aligned edges, and the
        // threshold decides how much contrast counts as an edge (lower = softer whole image).
        panel.push_back(std::make_unique<deren::vulkan::gui::checkbox_widget>("fxaa", &bindings.fxaa_enabled));
        {
            // the knobs only matter while FXAA is on (and while the fxaa pipeline exists at all)
            auto make_fxaa_slider = [&](std::string label, float* value, float lo, float hi) {
                auto slider = std::make_unique<deren::vulkan::gui::slider_widget>(std::move(label), value, lo, hi);
                slider->visible_when = [&bindings] { return bindings.fxaa_enabled; };
                panel.push_back(std::move(slider));
            };
            make_fxaa_slider("fxaa subpixel", &bindings.fxaa_subpixel, 0.0f, 1.0f);
            make_fxaa_slider("fxaa edge threshold", &bindings.fxaa_edge_threshold, 0.05f, 0.5f);
        }
        // G-buffer debug view: what the deferred path stores - the one part of the renderer whose
        // contents cannot be judged from a shaded screenshot, so it gets a channel selector rather
        // than a strength knob. main() mirrors both fields into the runtime every frame.
        {
            auto debug_view = std::make_unique<deren::vulkan::gui::checkbox_widget>("gbuffer debug", &bindings.gbuffer_debug);
            debug_view->visible_when = [&runtime] { return runtime.feature_available("gbuffer-debug"); };
            panel.push_back(std::move(debug_view));
            auto channel = std::make_unique<deren::vulkan::gui::combo_widget>(
                "gbuffer channel",
                std::vector<std::string>{"albedo", "normal", "roughness", "metallic", "ao", "material id", "depth", "flags", "motion"},
                &bindings.gbuffer_channel);
            channel->visible_when = [&bindings] { return bindings.gbuffer_debug; };
            panel.push_back(std::move(channel));
        }
        // TAA: the engine's anti-aliasing (there is no MSAA on a G-buffer), with the two
        // history-weight knobs. The static weight decides how smooth a still image gets (higher =
        // smoother, slower to react to lighting changes); the minimum is what a fast-moving pixel
        // falls back to (lower = trusts the current frame more, which trades smoothing for less
        // ghosting).
        {
            auto taa = std::make_unique<deren::vulkan::gui::checkbox_widget>("taa", &bindings.taa_enabled);
            taa->visible_when = [&runtime] { return runtime.feature_available("taa"); }; // deferred-only, via the feature registry
            panel.push_back(std::move(taa));
            auto make_taa_slider = [&](std::string label, float* value, float lo, float hi) {
                auto slider = std::make_unique<deren::vulkan::gui::slider_widget>(std::move(label), value, lo, hi);
                slider->visible_when = [&bindings] { return bindings.taa_enabled; };
                panel.push_back(std::move(slider));
            };
            make_taa_slider("taa history (static)", &bindings.taa_blend_static, 0.0f, 0.98f);
            make_taa_slider("taa history (min)", &bindings.taa_blend_min, 0.0f, 0.98f);
        }
        // THE TOON CHARACTER STAGE: re-shades the scene's opaque leaves OVER the lit frame, at depth-EQUAL,
        // so a character can carry its own shading instead of the deferred one - and without being lit twice.
        // Offered only when the renderer registered the pipeline the stage binds (it needs the mesh stage), so
        // a switch that would draw nothing is not shown at all - the failure mode feature_available exists for.
        {
            auto character = std::make_unique<deren::vulkan::gui::checkbox_widget>("character forward (toon)", &bindings.character_forward);
            character->visible_when = [&runtime] { return runtime.feature_available("character_forward"); };
            panel.push_back(std::move(character));
            // ... AND WHICH CHAIN IT DRAWS WITH. Offered only while the character stage itself is on, because a
            // switch that selects a shading model for a pass that is not recording is a control with no effect -
            // and offered only when the rewritten pipeline was actually built, on the checkbox above's own terms.
            auto goo = std::make_unique<deren::vulkan::gui::checkbox_widget>("goo toon (rewritten chain)", &bindings.goo_toon);
            goo->visible_when = [&runtime, &bindings] { return bindings.character_forward && runtime.goo_toon_ready(); };
            panel.push_back(std::move(goo));
        }
        // cel/toon shading: quantize the diffuse falloff (and harden shadows/highlights);
        // 0 steps leaves plain PBR, softness shrinks toward hard comic edges
        // cel/toon shading is discrete: every listed band count gives a visibly different look
        // (more bands converge back to smooth PBR, so a continuous slider had dead zones).
        // Softness stays small - a wide band edge erases the steps entirely.
        panel.push_back(std::make_unique<deren::vulkan::gui::combo_widget>(
            "toon shading",
            std::vector<std::string>{"off (plain pbr)", "2 bands (hardest)", "3 bands", "4 bands", "5 bands", "6 bands", "8 bands (softest)"},
            &bindings.toon_bands_index));
        panel.push_back(std::make_unique<deren::vulkan::gui::slider_widget>("toon softness", &bindings.toon_softness, 0.01f, 0.25f));
        panel.push_back(std::make_unique<deren::vulkan::gui::slider_widget>("sun intensity", &bindings.sun_intensity, 0.0f, 3.0f));
        // ---- punctual lights (demo lights; see apply_point_lights): the widgets edit
        //      bindings.point_lights live and main() pushes the enabled set once per frame.
        //      Each slot is a point light or - with `spot` checked - a cone light -------
        // WHICH SLOT THE GROUP BELOW EDITS: the combo doubles as the group's header, so "which light am I
        // looking at" and "how tall is this group" are both answered here instead of by scrolling past
        // thirty-six rows to find the one light that is on.
        {
            std::vector<std::string> light_items;
            light_items.reserve(std::size(bindings.point_lights));
            for (std::size_t i = 0; i < std::size(bindings.point_lights); ++i) {
                light_items.push_back(std::format("punctual light {}", i + 1));
            }
            panel.push_back(std::make_unique<deren::vulkan::gui::combo_widget>("punctual light", std::move(light_items), &bindings.active_light));
        }
        for (std::size_t i = 0; i < std::size(bindings.point_lights); ++i) {
            gui_bindings::light_slot& slot = bindings.point_lights[i];
            // ONE SLOT AT A TIME, chosen by the combo above: every widget of every slot is built once (the
            // panel is a fixed list), but a slot only draws while it is the selected one - four slots of nine
            // controls each was thirty-six rows of panel for a feature most frames leave off, which buried
            // everything below it. The cone knobs go one step further and appear only for a slot that is
            // actually a spot, so an omni slot is seven rows and a cone slot ten.
            auto const selected = [&bindings, i] { return bindings.active_light == static_cast<int32_t>(i); };
            auto const selected_spot = [&bindings, &slot, i] { return bindings.active_light == static_cast<int32_t>(i) && slot.spot; };
            {
                auto w = std::make_unique<deren::vulkan::gui::checkbox_widget>("  enabled", &slot.enabled);
                w->visible_when = selected;
                panel.push_back(std::move(w));
            }
            {
                auto w = std::make_unique<deren::vulkan::gui::vec3_widget>("  position", slot.position, 0.1f);
                w->visible_when = selected;
                panel.push_back(std::move(w));
            }
            {
                auto w = std::make_unique<deren::vulkan::gui::vec3_widget>("  color", slot.color, 0.02f);
                w->visible_when = selected;
                panel.push_back(std::move(w));
            }
            {
                auto w = std::make_unique<deren::vulkan::gui::slider_widget>("  intensity", &slot.intensity, 0.0f, 50.0f);
                w->visible_when = selected;
                panel.push_back(std::move(w));
            }
            {
                auto w = std::make_unique<deren::vulkan::gui::slider_widget>("  range", &slot.range, 0.1f, 100.0f);
                w->visible_when = selected;
                panel.push_back(std::move(w));
            }
            {
                auto w = std::make_unique<deren::vulkan::gui::checkbox_widget>("  spot", &slot.spot);
                w->visible_when = selected;
                panel.push_back(std::move(w));
            }
            {
                auto w = std::make_unique<deren::vulkan::gui::vec3_widget>("  direction", slot.direction, 0.1f);
                w->visible_when = selected_spot;
                panel.push_back(std::move(w));
            }
            {
                auto w = std::make_unique<deren::vulkan::gui::slider_widget>("  inner cone deg", &slot.inner_cone_deg, 0.0f, 89.0f);
                w->visible_when = selected_spot;
                panel.push_back(std::move(w));
            }
            {
                auto w = std::make_unique<deren::vulkan::gui::slider_widget>("  outer cone deg", &slot.outer_cone_deg, 1.0f, 89.0f);
                w->visible_when = selected_spot;
                panel.push_back(std::move(w));
            }
        }
        // camera orbit target: dragging it moves what the camera looks at / orbits around
        // (camera.target is a glm::vec3, i.e. three contiguous floats; the runtime rebuilds the
        // camera UBO from it every frame, so no on_change callback is needed)
        panel.push_back(std::make_unique<deren::vulkan::gui::vec3_widget>("camera target", &runtime.camera.target.x, 0.05f));
        // playback controls (only when the model carries animations): play/pause toggle bound
        // to the playback state, a time scrubber (pauses on drag so the clock cannot fight the
        // scrub; the play checkbox resumes), and - for multi-animation assets - a dropdown to
        // pick which animation plays. All playback state lives in the animation::controller.
        if (animation.has_active()) {
            panel.push_back(std::make_unique<deren::vulkan::gui::label_widget>([&animation] {
                return std::format("animation '{}' ({}s)", animation.active_name(), animation.loop_duration());
            }));
            panel.push_back(std::make_unique<deren::vulkan::gui::checkbox_widget>(
                "play",
                &bindings.anim_playing,
                [&animation](bool const enabled) { animation.set_playing(enabled); }));
            panel.push_back(std::make_unique<deren::vulkan::gui::slider_widget>(
                "time",
                &bindings.anim_time,
                0.0f,
                animation.playable_max_duration(),
                [&animation](float const value) {
                    animation.set_time(value); // scrubbing pauses so the clock does not fight the drag
                }));
            if (animation.playable_count() > 1) {
                std::vector<std::string> names;
                names.reserve(animation.playable_count());
                for (std::size_t i = 0; i < animation.playable_count(); ++i) {
                    names.push_back(std::string(animation.playable_name(i)));
                }
                panel.push_back(std::make_unique<deren::vulkan::gui::combo_widget>(
                    "animation",
                    std::move(names),
                    &bindings.anim_index,
                    [&animation](int32_t const index) { animation.select(static_cast<std::size_t>(index)); }));
            }
            deren::utility::log("gui: playback controls added ({} animation(s))", animation.playable_count());
        }
        // camera selector: "orbit" (free) or any scene camera (its pose seeds the orbit camera,
        // so the mouse keeps working after switching)
        if (!camera_names.empty()) {
            std::vector<std::string> items;
            items.reserve(camera_names.size() + 1);
            items.push_back("orbit");
            items.insert(items.end(), camera_names.begin(), camera_names.end());
            panel.push_back(std::make_unique<deren::vulkan::gui::combo_widget>(
                "camera",
                std::move(items),
                &bindings.current_camera,
                on_camera_selected));
            deren::utility::log("gui: camera selector added ({} camera(s))", camera_names.size());
        }
        // Cascaded shadow maps: how many cascades the sun's shadow pass fills (1 = the historic
        // single map) and how much of a cascade's range fades into the next one. Both are
        // write-through: the runtime refits the cascade boxes on the next frame, and the blend is a
        // shader constant in the light UBO - no pipeline or image rebuild, so they are live.
        panel.push_back(std::make_unique<deren::vulkan::gui::combo_widget>(
            "shadow cascades",
            std::vector<std::string>{"1 (single map)", "2", "3", "4"},
            &bindings.shadow_cascades,
            [&runtime](int32_t const index) { runtime.set_shadow_cascades(static_cast<uint32_t>(index) + 1u); }));
        panel.push_back(std::make_unique<deren::vulkan::gui::slider_widget>(
            "shadow cascade blend",
            &bindings.shadow_cascade_blend,
            0.0f,
            0.5f,
            [&runtime](float const value) { runtime.set_shadow_cascade_blend(value); }));
        // Shadow depth bias (bottom of the panel - a rarely-used tuning aid): the pass's bias
        // is dynamic state applied every frame; the slope factor removes acne on angled
        // surfaces, the constant adds a fixed push. Note it cannot fix geometry that is simply
        // too coarse (e.g. RecursiveSkeletons' sides are large flat triangles - the depth
        // gradient across them is what it is), it only tunes the bias offset.
        panel.push_back(std::make_unique<deren::vulkan::gui::slider_widget>(
            "shadow bias slope",
            &bindings.shadow_bias_slope,
            0.0f,
            10.0f,
            [&runtime, &bindings](float const value) {
                bindings.shadow_bias_slope = value;
                runtime.set_shadow_depth_bias(bindings.shadow_bias_constant, bindings.shadow_bias_slope, 0.0f);
            }));
        panel.push_back(std::make_unique<deren::vulkan::gui::slider_widget>(
            "shadow bias constant",
            &bindings.shadow_bias_constant,
            0.0f,
            10.0f,
            [&runtime, &bindings](float const value) {
                bindings.shadow_bias_constant = value;
                runtime.set_shadow_depth_bias(bindings.shadow_bias_constant, bindings.shadow_bias_slope, 0.0f);
            }));
        deren::utility::log("gui: Dear ImGui debug overlay enabled");
    }

    void apply_point_lights(deren::vulkan::runtime& runtime, gui_bindings const& bindings, std::span<deren::vulkan::punctual_light const> const extra) {
        // build the enabled demo lights into a fixed stack array (limit = the light UBO's
        // array size) and push it; the span form keeps set_point_lights cheap to call per frame.
        // `extra` is the [lighting] demo_lights stress set (M5), appended after the overlay's slots
        // so the overlay keeps working - both share the UBO's light array, so the overlay's slots
        // win when the two together would overflow it.
        std::array<deren::vulkan::punctual_light, deren::vulkan::max_punctual_lights> active = {};
        uint32_t count = 0;
        for (gui_bindings::light_slot const& slot : bindings.point_lights) {
            if (!slot.enabled || count >= deren::vulkan::max_punctual_lights) {
                continue;
            }
            deren::vulkan::punctual_light& light = active[count++];
            light.position = glm::vec3(slot.position[0], slot.position[1], slot.position[2]);
            light.color = glm::vec3(slot.color[0], slot.color[1], slot.color[2]);
            light.intensity = slot.intensity;
            light.range = slot.range;
            light.spot = slot.spot;
            if (slot.spot) {
                light.spot_direction = glm::vec3(slot.direction[0], slot.direction[1], slot.direction[2]);
                // clamp the cone (inner <= outer, both < 90 deg) and hand the shader the cosines
                float const outer_deg = std::clamp(slot.outer_cone_deg, 1.0f, 89.0f);
                float const inner_deg = std::clamp(slot.inner_cone_deg, 0.0f, outer_deg);
                light.spot_outer_cos = std::cos(glm::radians(outer_deg));
                light.spot_inner_cos = std::cos(glm::radians(inner_deg));
            }
        }
        for (deren::vulkan::punctual_light const& light : extra) {
            if (count >= deren::vulkan::max_punctual_lights) {
                break;
            }
            active[count++] = light;
        }
        runtime.set_point_lights(std::span(active.data(), count));
    }

    // Wire an animation backend to the runtime: the scene tree it drives, its per-frame-slot
    // morph/skin buffers (active slot for per-frame writes, explicit slot for setup bakes) and
    // its shared task pool. The controller sees only this surface, never deren::vulkan::runtime.
    deren::vulkan::animation::backend make_animation_backend(deren::vulkan::runtime& runtime) {
        deren::vulkan::animation::backend backend;
        backend.scene = &runtime.get_scene();
        backend.morph_scratch_active = [&runtime]() -> float* {
            return static_cast<float*>(runtime.morph_scratch());
        };
        backend.morph_scratch_slot = [&runtime](uint32_t const slot) -> float* {
            return static_cast<float*>(runtime.morph_scratch(slot));
        };
        backend.set_skin_matrices_active = [&runtime](std::span<glm::mat4 const> matrices) {
            runtime.set_skin_matrices(matrices);
        };
        backend.set_skin_matrices_slot = [&runtime](std::span<glm::mat4 const> matrices, uint32_t const slot) {
            runtime.set_skin_matrices(matrices, slot);
        };
        backend.scene_changed = [&runtime]() {
            runtime.scene_changed();
        };
        backend.run_tasks = [&runtime](std::span<std::function<void()>> tasks) {
            runtime.run_tasks(tasks, deren::vulkan::task_priority::animation);
        };
        backend.task_worker_count = [&runtime]() -> int32_t {
            return runtime.task_pool_threads();
        };
        return backend;
    }
} // namespace deren::chores
