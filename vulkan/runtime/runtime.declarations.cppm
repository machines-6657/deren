// ============================================================================
// module: deren.vulkan.runtime:declarations  - the INTERFACE PARTITION of deren.vulkan.runtime
//
// Same shape as vulkan.core: the declarations live in a partition, the primary re-exports them, and
// the implementation partitions import THIS one (a partition does not see the primary interface, and
// imports are not transitive). `import deren.vulkan.runtime;` is unchanged for main, chores and
// render_start_demo.
// ============================================================================
// ============================================================================
// module: deren.vulkan.runtime
// module version: 0.72.0  (independent of the app version in CMakeLists project(VERSION))
//
// The renderer core: per-frame-slot frame facade (pace/record/submit phases,
// scene resources, parallel secondary-CB recording). It re-exports its peer
// modules deren.vulkan.scene_tree (scene storage + GPU primitives) and
// deren.vulkan.render_environment (per-worker draw state) - the frame draws through
// both, so they are versioned as ONE unit because they share the scene / draw
// interface and evolve together.
// Depends on deren.vulkan.core (GPU), deren.vulkan.math (IBL), deren.vulkan.shadow_fit (the
// cascade fit it gathers for and caches), the promise contract (the recording surface the read-back
// copy goes through) and utility, with the frame struct
// fills coming from vulkan.constant_init.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan.h>

export module deren.vulkan.runtime:declarations;

import deren.vulkan.profiling;
import deren.vulkan.pass;                       // the pass framework: the host the runner talks to, and the stage runner
import deren.vulkan.pass.taa;                   // the second, and the first GRAPHICS one
import deren.vulkan.pass.scene;                 // the third: the scene itself, whose work is DATA rather than a declaration
import deren.vulkan.pass.transparent;           // the fourth: the blended geometry, over the shaded frame
import deren.vulkan.pass.character_forward;     // ... and the toon character stage, which re-shades the OPAQUE leaves over it
import deren.vulkan.pass.toon_screen_rim;       // ... and its second rim, a fullscreen additive contour from the depth
import deren.vulkan.pass.goo_rim;               // ... and the REWRITTEN chain's rim, a fullscreen additive stage of its own
import deren.vulkan.pass.upscale;               // the resolve: the render chain's LDR image -> the presented swapchain
import deren.vulkan.pass.ray_traced_shadow;     // the ninth, and the only pass that traces outside the chain: the ray-traced shadow
import deren.vulkan.pass.mask_bake;             // ... and the one-shot MASK bake, which is a JOB rather than a frame pass
import deren.vulkan.pass.compute_skin;          // ... and the compute-skinning job, which is a job for the same reason
import deren.vulkan.pass.geometry_buffer_debug; // the fifteenth: the G-buffer debug view
import deren.vulkan.pass.shadow;                // the sixteenth: the directional shadow map, one depth-only cascade per layer
import deren.vulkan.pass.chain;                 // the chain container: what holds a run of passes and its ORDER
import deren.vulkan.render_resource.shared;     // the five samplers a pass's declaration chooses between
import deren.vulkan.frame_constants;            // one frame's shared constants, filled by the frame loop and read by passes
import deren.vulkan.shadow_fit;                 // the cascade fit itself (pure CPU; the runtime gathers and caches)
// S2 BATCH 2: THIS RUNTIME NO LONGER IMPORTS deren.vulkan.readback. The screenshot's copy is recorded
// through the contract's recording surface and the bytes come out of the BACKEND's read-back slot, so
// the staging class that used to be constructed here (and the two record-time fields it fed) are gone.
// The module itself stays in the tree; it simply has no consumer left in the engine.
import deren.vulkan.acceleration_structure; // build_input_usage: the usage bits a structure build reads a buffer through
import deren.vulkan.ray_tracing;            // THE STRUCTURE PHASE: the structures, the caster map and the copies (a value this class owns)
import deren.vulkan.init_utils;             // the resource-creation patterns the init functions below repeat
import deren.promise.rhi;                   // the creation contract (create_info): the type the constructor below takes
export import deren.vstd;
export import deren.vulkan.core;
export import deren.vulkan.core.filters;
export import deren.vulkan.scene_tree;         // scene storage + the abstract leaf interface (pure CPU)
export import deren.vulkan.primitive;          // the GPU primitives + material/UBO records (peer module)
export import deren.vulkan.render_environment; // per-worker draw state (peer module)
import deren.utility;
export import deren.vulkan.graphical_user_interface; // optional debug overlay (gui_content): exported so callers can manage panels/widgets via debug_gui()

// THE CONTRACT, UNDER THE SHORT NAME EVERY SITE BELOW USES. The module `deren.promise.rhi` declares
// `deren::promise::rhi`, and every member this file converted to a contract handle (the buffers held in
// `object_manager`) names it - so the alias is here rather than repeated per member.
namespace rhi = deren::promise::rhi;

/**
 * @file runtime.cppm
 * @defgroup vulkan_runtime Vulkan Runtime Facade
 * @brief runtime facade: a thin wrapper exposing all functionality of deren::vulkan::core
 * @note
 *      - use operator-> to access the filtered core view (core_filter, e.g. runtime->get_device())
 *      - the inner core's lifetime is tied to the runtime
 */
namespace deren::vulkan {
    /**
     * @ingroup vulkan_runtime
     * @brief orbit camera state, updated by the mouse callbacks registered in the runtime constructor
     *        (the arrow keys pan it too - see runtime::poll_events and orbit_camera_pan_delta)
     */
    export struct orbit_camera {
        double last_x = 0.0;
        double last_y = 0.0;
        bool dragging = false;
        float yaw = 0.0f;
        // level view: the skybox horizon (the direction parallel to the ground plane) then sits
        // exactly at the screen center, where the primitive is framed against it
        float pitch = 0.0f;
        float distance = 2.2f;
        // point the camera looks at and orbits around (default origin; main may sink it
        // together with the scene so the camera follows the primitive)
        glm::vec3 target = glm::vec3(0.0f);
        // the clock reading of the last arrow-key pan (glfwGetTime seconds; 0 = never sampled). The pan
        // is speed x elapsed time, so it needs a previous reading; keeping it HERE rather than in a local
        // is what makes the step independent of the frame rate.
        double last_pan_time = 0.0;
    };

    /**
     * @ingroup vulkan_runtime
     * @brief outcome of one frame step or of a whole frame (the frame phases); the caller reacts to it
     * @note shared by the split frame steps: each step returns proceed when it succeeded and the
     *       caller may continue to the next step (for the frame path that means a frame was
     *       recorded, submitted and presented). Failures are granular per stage so a caller can
     *       tell WHERE the frame broke (acquire / command-buffer recording / submit / present).
     */
    export enum class frame_status {
        proceed, // step succeeded / a frame was presented; caller continues
        skipped, // not renderable this iteration (window minimized / swapchain recreated); caller yields and retries
        closed,  // the window was closed (ESC or the native close button); caller exits the loop
        // stage-specific failures: a fatal Vulkan error at that point; caller exits the loop
        acquire_failed,         // vkAcquireNextImageKHR failed (other than out-of-date)
        begin_recording_failed, // vkBeginCommandBuffer failed
        end_recording_failed,   // vkEndCommandBuffer failed
        submit_failed,          // vkQueueSubmit failed
        present_failed,         // vkQueuePresentKHR failed (other than out-of-date / suboptimal)
    };

    /**
     * @ingroup vulkan_runtime
     * @brief true for any stage-specific failure (not proceed / skipped / closed)
     */
    export [[nodiscard]] constexpr bool is_failure(frame_status const status) noexcept {
        return status != frame_status::proceed && status != frame_status::skipped && status != frame_status::closed;
    }

    /**
     * @ingroup vulkan_runtime
     * @brief named tiers for tasks submitted to the runtime's shared task pool (run_tasks).
     *
     * Each enumerator names one frame-time parallel stage of the runtime; it maps onto the
     * underlying pool's integer priority (higher numeric values run earlier) and is ALSO that
     * stage's wait group, so distinct stages never block on each other's tasks. Values follow
     * frame causality: the stage that PRODUCES data (animation sampling) carries the higher
     * value so it runs first, the stage that CONSUMES it (command recording / rendering) the
     * lower one - "prepare the data, then render". Add an enumerator per future stage instead
     * of passing magic ints at call sites (slot new stages in by execution order; renumbering
     * is free, these values are compile-time only).
     */
    export enum class task_priority : int32_t {
        animation = 1, // runs first: per-source animation sampling fan-out (animation::controller::update)
        recording = 0, // runs after the data is ready: pass/secondary command-buffer recording (record_main_drawcalls)
        // future frame-time stages (culling, skin upload, ...) slot in between by execution order
    };

    /**
     * @ingroup vulkan_runtime
     * @brief vulkan runtime facade class
     * @note
     *      - use operator-> to access the filtered core view (core_filter, e.g. runtime->get_device())
     *      - default construction performs the whole core initialization (window/instance/device/swap chain etc.)
     *        and registers the orbit camera mouse callbacks on the window
     */
    export class runtime {
        /**
         * THE DEVICE ROOT, and the reason it is a `shared_ptr`: the core is the one object every other device
         * resource depends on, so its lifetime is the frame's OUTERMOST fact. A caller may hand in a finished one
         * (see the constructor that takes a `shared_ptr<core>`), which is what lets two owners - a second
         * viewport, an editor, a test host - share one device without an artificial "who owns whom" order.
         *
         * DECLARED FIRST so it is released LAST: every member below (the pipelines a pass has not taken over,
         * the descriptor families, the readback staging buffer, the filter view) is destroyed while this
         * reference still holds the device alive. The destructor used to spell that ordering out by hand; a
         * reference-counted root states it instead, and the device may now outlive this runtime when the caller
         * kept its own reference - which is the point of sharing it.
         */
        std::shared_ptr<core> core_owner;
        /// the alias every method and member keeps using: it IS `*core_owner`, and owns nothing of its own
        core& vulkan_core;

        /**
         * @ingroup vulkan_runtime
         * @brief the CPU frame phases that are measured per frame (see cpu_timing_summary)
         *
         * Above a few hundred fps the frame stops being GPU-bound: measured on the RTX 4060 at
         * 960x720, cutting the GPU frame from 0.52 to 0.41 ms raised fps by 74% while cutting it
         * further to 0.38 ms changed nothing, and halving the shadow map (which halves the shadow
         * pass rasterization) changed neither - so what is left is CPU-side recording,
         * synchronization and presentation. These phases say which: `pace` is the wait for the frame
         * slot, i.e. where GPU/present backpressure surfaces.
         */
        // `cluster` and `shadow` are SUB-measurements of `scene` (which spans the whole
        // record_main_drawcalls() call). The shadow pass records one secondary + one rendering
        // instance + one barrier PER CASCADE on the primary thread, which makes it the first suspect
        // for the scene phase's ~1.3 ms, so it is reported separately - and, being inside `scene`, it
        // is NOT added to the total again.

        // set while the window is iconified; the restore transition recreates the swapchain
        bool was_minimized = false;

        // ---- shared scene resources (the heap's per-frame-slot slots; see core::heap_slots) ----
        //
        // EVERY BUFFER BELOW IS A CONTRACT `object_manager` NOW, AND ITS RELEASE ORDER IS THE ONE FACT
        // THIS SECTION RELIES ON: a manager's destructor calls `buffer::release()`, which runs INSIDE the
        // backend and may decrement the allocator's reference count - so the core that owns that allocator
        // has to outlive every one of them. It does, and the guarantee is the DECLARATION ORDER that was
        // already here: `core_owner` is declared first (see its own note), members destruct in reverse, and
        // every manager below is therefore released while the device root still holds the device. Nothing in
        // the destructor body frees a buffer, so there is no second, hand-written order to keep in step.
        //
        // camera UBO: one buffer per frame slot, updated once per frame, shared by every primitive
        std::vector<rhi::object_manager<rhi::buffer>> camera_buffers = {};
        std::vector<void*> camera_mapped = {};
        // texture registry: flat entries of the set 0 binding 1 array (raw handles); the owning
        // views / vma images live in the vectors below (vk_image RAII frees the GPU image when
        // the runtime goes away). texture_slot_cache deduplicates uploads by CONTENT (xxh3 of
        // the decoded bytes + format + dimensions): several materials sharing one glTF image
        // (same decoded pixels, different byte copies) all point at the same array slot instead
        // of uploading a copy per material - the loader hands each material its own byte copy,
        // so a pointer key would never match.
        std::vector<VkImageView> texture_array_views = {};
        std::vector<vk_image_view> owned_texture_views = {};
        std::vector<vk_image> owned_textures = {};
        uint32_t white_texture_index = 0;
        std::map<std::tuple<deren::utility::xxh3_digest, VkFormat, std::uint32_t, std::uint32_t, std::uint32_t>, uint32_t> texture_slot_cache = {}; // digest (data_block<16>), format, width, height, mip_levels
        // scene-wide IBL (bindings 2-4): prefiltered env / irradiance / BRDF LUT, uploaded once
        std::vector<vk_image_view> ibl_views = {};
        std::vector<vk_image> ibl_images = {};
        // THE ARTICLE'S POST LUT (`ZmdLutPost.shader`'s `_LutTex`) IS ITS OWN IMAGE, kept alive here for the same
        // reason the IBL's three are: the heap holds a DESCRIPTOR, and an image the host lets go of leaves that
        // descriptor pointing at nothing. Uploaded once by `set_post_lut`, which the application calls with a baked
        // neutral cube.
        vk_image post_lut_image = {};
        vk_image_view post_lut_view = {};
        // THE GOO REFERENCE'S PRE-INTEGRATED FGD LUT (`PreIntegratedFGD_GGXDisneyDiffuse.png`) IS THE SAME KIND OF
        // RESOURCE FOR THE SAME REASON: the heap holds a DESCRIPTOR, so an image the host lets go of leaves that
        // descriptor pointing at nothing. Uploaded ONCE by `set_goo_fgd_lut`, by the application, from the
        // reference's own 64x64 PNG - and it is the STEP-5 spec's architecture ruling that it is a shared global
        // image rather than a per-material lane (§3.4).
        //
        // ITS FORMAT IS `VK_FORMAT_R8G8B8A8_UNORM` AND THAT IS A CORRECTNESS REQUIREMENT RATHER THAN A DEFAULT:
        // the reference's image data-block is `colorspace = 'Non-Color'`, i.e. Blender does NOT linearize it, so
        // the node graph reads the texel's BYTES as the value. An `_SRGB` upload would decode every channel once
        // and move all three outputs of the group (spec §3.1 item 1).
        vk_image goo_fgd_image = {};
        vk_image_view goo_fgd_view = {};
        vk_sampler env_sampler = {};
        // GPU material table (set 0 binding 5): one material_record per entry (texture indices +
        // factors + flags); primitives only push their material_index. Host-visible, written at
        // registration, read-only for the GPU.
        rhi::object_manager<rhi::buffer> material_buffer = {};
        void* material_mapped = nullptr;
        // THE TOON LANES BESIDE THE RECORD: ONE `uvec4` PER MATERIAL (x = `_SDFLightmap`, y =
        // `_MetallicGlossMap`, z and w reserved), and a buffer of its own because the material record cannot
        // carry a fifth lane - it is inline in the per-draw push block (see core::heap_slots::toon_lanes).
        // Host-visible and written once at import, exactly like the table above and for the same reason.
        rhi::object_manager<rhi::buffer> toon_lane_buffer = {};
        void* toon_lane_mapped = nullptr;
        // THE TOON LIGHT RIG: one block for the whole run (see core::heap_slots::toon_rig), written by
        // `set_toon_rig` - which the application calls from its config, before the first frame. Host-visible and
        // never rewritten by the frame path, exactly like the lane table above.
        rhi::object_manager<rhi::buffer> toon_rig_buffer = {};
        void* toon_rig_mapped = nullptr;
        // THE MATERIAL COLOURS (see `core::heap_slots::toon_colours`): one `vec4` per lane per material, filled
        // from the sidecar at registration and never rewritten afterwards - the same once-written contract the two
        // tables above follow.
        rhi::object_manager<rhi::buffer> toon_colour_buffer = {};
        void* toon_colour_mapped = nullptr;
        uint32_t material_count = 0;
        // content-addressed material dedup + overflow fallback (see register_material):
        // material_slot_cache keys the full material_record bytes (texture indices + factors +
        // flags) as a byte-exact data_block<sizeof(material_record)>, so N primitives sharing
        // one glTF material register ONE record instead of N identical appends (an unordered
        // key: data_block's own FNV-1a hasher + byte-equality); when the table really fills up,
        // later registrations degrade to the reserved default material at index 0 (registered
        // in init_scene_resources) with a one-time log instead of a hard panic.
        //
        // THE KEY CARRIES THE LANE BLOCK PAST THE RECORD, and it has to: those lanes live OUTSIDE the record
        // (see core::heap_slots::toon_lanes for why), so a key built from the record alone cannot tell two
        // materials apart when they differ ONLY in their SDF or metallic/gloss map - the second would take the
        // dedup early return, never write its lanes, and that material would silently lose the feature. See
        // register_material.
        //
        // ... AND IT CARRIES THE COLOUR LANES FOR EXACTLY THE SAME REASON, one table further along: those six
        // `vec4`s live outside the record too (see core::heap_slots::toon_colours), and the write that fills
        // them sits AFTER this map's early return. A key without them made the write unreachable for the second
        // of two record-identical materials, so that material read the FIRST registrant's six lanes - which is
        // not a missing feature but a WRONG VALUE, and it now governs `_Specular` (lane 4), `_ParallaxScale`
        // (lane 5) and `_OutlineWidth` (lane 3's `.w`). MEASURED WITH AN ASSET PROBE rather than argued: a
        // mesh-less copy of `M_actor_chen_hair_01` stating `_Specular = 0.0` was registered one node earlier
        // than the hair itself, the hair took this early return, and the render came out BYTE-FOR-BYTE identical
        // to an asset whose hair states 0.0 - see `remaining_port_spec.md`'s "材质去重键补上 colour lanes" section.
        //
        // THE THIRD TERM IS SPELLED IN TERMS OF THE ENUM, so adding a colour lane grows the key by itself (a
        // hand-written 4 or 5 here is the drift this note exists to prevent), and it is in LANE ORDER, the same
        // order register_material writes the table in and the shader addresses it in.
        std::unordered_map<deren::utility::data_block<sizeof(deren::vulkan::material_record) + deren::vulkan::toon_lane_blocks * sizeof(glm::uvec4) + static_cast<std::size_t>(deren::vulkan::toon_colour_lane::count) * sizeof(glm::vec4)>, material_id,
                           deren::utility::data_block<sizeof(deren::vulkan::material_record) + deren::vulkan::toon_lane_blocks * sizeof(glm::uvec4) + static_cast<std::size_t>(deren::vulkan::toon_colour_lane::count) * sizeof(glm::vec4)>::hasher>
            material_slot_cache = {};
        bool material_overflow_logged = false;
        // same degradation policy for the texture array: when scene_texture_capacity distinct
        // images are in use, later slots fall back to the white element (0) with a one-time log
        bool texture_overflow_logged = false;
        // per-instance transforms for instanced primitives (scene block slot 6): one mat4 per
        // instance, host-visible. The buffer is ONE shared region split into per-instanced-
        // primitive slices: make_instanced_primitive() appends its transforms at instance_cursor
        // (mat4 units), hands the slice start to the new primitive via push.instance_base, and
        // advances the cursor, so several instanced primitives coexist without overwriting each
        // other. instance_cursor resets to 0 whenever instanced primitives are cleared (they are
        // the only writers).
        rhi::object_manager<rhi::buffer> instance_buffer = {};
        /// Buffers a PASS asked for through `pass_context::create_upload_buffer` (the ray-traced shadow's shader
        /// binding table is the first): the owner keeps them for the generation, because the pass gets a handle
        /// and a device address, not an allocation it could free.
        std::vector<rhi::object_manager<rhi::buffer>> pass_upload_buffers = {};
        void* instance_mapped = nullptr;
        uint32_t instance_cursor = 0;
        // previous-frame world matrices (scene block slot 13): ONE buffer per frame slot, host
        // visible, scene_motion_capacity mat4s each - the same per-slot rule as the skin and morph
        // buffers, so the frame being rendered never shares the buffer the next frame rewrites.
        // Every leaf owns a slice of it (an instanced draw owns one slot per instance and fills them
        // at setup); motion_previous keeps the CPU-side "world matrix as of one frame ago" that
        // advance_motion_transforms() writes out and then refreshes.
        std::vector<rhi::object_manager<rhi::buffer>> motion_buffers = {};
        std::vector<void*> motion_mapped = {};
        std::vector<glm::mat4> motion_previous = {};
        uint32_t motion_cursor = 0;
        // per-joint skin matrices (scene block slot 9): ONE buffer per frame slot, like the
        // camera UBO — each slot's scene set always points at its own buffer, so a frame being
        // rendered never shares the buffer the next frame rewrites. scene_skin_capacity mat4s
        // each, host-visible; indices 0-3 are the identity block (unskinned fallback),
        // per-skin joint blocks follow. Filled per frame by set_skin_matrices(); primitives
        // reference their block via material_push_constants::skin_base
        std::vector<rhi::object_manager<rhi::buffer>> skin_buffers = {};
        std::vector<void*> skin_mapped = {};
        // the SAME joint blocks at the time the PREVIOUS frame drew them (heap: skin_matrices_previous):
        // one buffer per frame slot like the current ones, plus the CPU-side copy of "the matrices one
        // frame ago" that advance_motion_deformations() publishes into the CURRENT slot's buffer and then
        // refreshes - the exact arrangement motion_buffers/motion_previous uses for world matrices, and for
        // the same reason: a deforming vertex's motion vector is the difference between the two. indices
        // 0-3 are the identity block in both buffers, so an unskinned read is unchanged either way
        std::vector<rhi::object_manager<rhi::buffer>> skin_buffers_previous = {};
        std::vector<void*> skin_previous_mapped = {};
        std::vector<glm::mat4> skin_previous = {};
        // morph data (scene block slot 10): ONE buffer per frame slot, like the skin matrices.
        // scene_morph_capacity floats each, host-visible. The caller writes per-primitive blocks
        // (morph deltas + weights + the PREVIOUS frame's weights, see morph_scratch()'s note) through
        // morph_scratch() and points primitives at them via material_push_constants::morph_* fields
        std::vector<rhi::object_manager<rhi::buffer>> morph_buffers = {};
        std::vector<void*> morph_mapped = {};
        // per-slot scene resources: ONE buffer per frame slot, like the camera UBO - each slot's shader reads
        // its own, so a frame being rendered never shares the buffer the next frame rewrites. The shaders reach
        // them through the descriptor heap's per-slot grid slots (see core::heap_slots).
        // the frame slot paced by the last successful pace_and_acquire();
        // per-frame host writes (set_skin_matrices / morph_scratch) target this slot's buffers
        uint32_t active_slot = 0;
        bool ibl_ready = false;

        // ---- GPU pass timing (see gpu_mark / gpu_timing_summary) ----
        // Which pass boundaries a frame marks. The sequence is FIXED: a pass that does not record
        // this frame (shadow off, bloom intensity 0, FXAA off) still writes its mark immediately
        // after the previous one, so the measured interval is 0 and the label-to-interval mapping
        // never shifts. Mark i is written at the END of the pass named by gpu_timing_labels[i],
        // which is why the label table is one entry shorter than the mark list.
        enum class gpu_mark_id : uint32_t {
            frame_begin = 0, // first command of the frame (TOP_OF_PIPE)
            rt_build_end,    // after the acceleration-structure builds (the bottom levels on the frame
                             // that creates them, the top level every frame after it)
            shadow_end,      // after the shadow pass + its sampling barrier
            scene_end,       // after the geometry instance: forward main (opaque + transparent), or
                             // the background + G-buffer pass in the deferred path
            rt_shadow_end,   // after the ray-traced sun shadow pass (~0 when it does not run). Its own
                             // mark because it sits BETWEEN the G-buffer pass and the lighting stage:
                             // without it the traversals were reported as lighting time, which made the
                             // lighting interval look four times more expensive with rays on
            lighting_end,    // after the deferred path's shading work: the lighting stage and, in the
                             // same interval, the transparent pass that composites over it (~0 in the
                             // old forward path, where both of those happened inside the scene instance)
            taa_end,         // after the TAA resolve + its history copy (~0 when TAA is off)
            main_end,        // after the last scene-side work of the frame (the debug view, when it runs)
                             // those passes are the only compute work in the post chain: without it their
                             // cost was reported as bloom time, which is where a traced GI's price was
                             // invisible in the one report a user reads
                             // own mark for the same reason gi_end has one: it is compute work in the
                             // same stretch of the frame, and folding it into "gi" would hide which of
                             // the two the GI budget is going to
            bloom_end,       // after the bloom prefilter/downsample chain
            composite_end,   // after the composite (exposure + ACES + display encode)
            fxaa_end,        // after the FXAA pass (and the overlay, when FXAA draws it)
            frame_end,       // last command of the frame (the screenshot copy + present barrier)
            count,           // not a mark: the number of marks a frame writes
        };
        static constexpr uint32_t gpu_mark_count = static_cast<uint32_t>(gpu_mark_id::count);
        static_assert(gpu_mark_count <= deren::vulkan::gpu_timing_mark_capacity, "the core's timestamp pool must hold one frame's marks");
        // labels of the intervals between consecutive marks (interval i = mark i -> mark i + 1).
        // new_line starts a new line in the overlay's report, which has to fit one narrow panel
        // row; the log line ignores it and prints everything on one line.
        // "scene" is the geometry instance of whichever path is active, "lighting" is the deferred
        // path's shading work (the lighting stage plus the transparent pass that composites over it;
        // 0 ms in the old forward path, where the shading happened inside the scene instance - as does the
        // forward transparent pass, whose cost therefore shows up in "scene" as well), and "debug" is
        // the G-buffer debug view when it runs.
        struct gpu_timing_label {
            std::string_view name;
            bool new_line; // begin a new line in the overlay report
        };
        static constexpr std::array<gpu_timing_label, gpu_mark_count - 1> gpu_timing_labels = {{
            {"rt", false},
            {"shadow", false},
            {"scene", false},
            {"rt shadow", false},
            {"lighting", false},
            {"taa", false},
            {"debug", true},
            {"bloom", false},
            {"composite", false},
            {"fxaa", false},
            {"tail", false},
        }};
        // whether to collect pass timings at all ([render] gpu_timings); the device must be able
        // to timestamp as well, which core::gpu_timing_available() reports
        bool gpu_timings_enabled = true;
        // rolling window of measured intervals: every GPU_TIMING_WINDOW frames the collected
        // samples are averaged, logged, and the window starts over (the GUI label reads the
        // window's running mean, so it stays live instead of dropping to 0 on the reset)
        static constexpr uint32_t GPU_TIMING_WINDOW = 60;
        std::array<double, gpu_mark_count - 1> gpu_timing_sum = {}; // current window's summed ms
        uint32_t gpu_timing_window_frames = 0;                      // frames sampled in the current window
        // The overlay's copy of the last COMPLETED timing window (gpu_timing_summary): a label whose
        // text changes width every frame re-wraps against the panel edge and makes the whole overlay
        // twitch, so this is refreshed once per window and every number is a fixed-width field.
        std::string gpu_timing_report_label = {};
        uint32_t gpu_timing_marks_measured = 0; // intervals the last measured frame had
        void gpu_mark(VkCommandBuffer command_buffer, gpu_mark_id mark, VkPipelineStageFlagBits stage) noexcept;

        // ---- CPU frame phase timing (same 60-frame window as the GPU marks) ----
        // A scope timer rather than manual marks: every phase function has early returns
        // (skipped/minimized/closed) that must still be measured, and an RAII object cannot miss one.
        deren::vulkan::profiling::cpu_phases cpu_timings;
        void collect_gpu_timings(uint32_t slot);

        // ---- G-buffer / deferred path ----
        // M1: the opaque pass writes the G-buffer (three surface targets + the HDR target it adds
        // emissive into) instead of shading; M2 adds the deferred lighting stage that reads it back.
        // The G-buffer pipeline shades nothing: albedo/metallic, normal/roughness and material
        // id/AO/flags go into core::gbuffer_* (1x targets + the pass's own 1x depth), so the opaque
        // pass runs at 1x.
        std::optional<vk_pipeline> gbuffer_pipeline = std::nullopt;
        /// ... and the MESH form of the same pass (docs/mesh_shaders.md step 2), when the device can run one and the
        /// app handed the mesh stage's SPIR-V over: the scene session prefers it the way the shadow pass prefers
        /// its own, and a device without it keeps `gbuffer_pipeline` above.
        std::optional<vk_pipeline> gbuffer_pipeline_mesh = std::nullopt;
        /// ... and its MESHLET form (docs/mesh_shaders.md step 3): one workgroup per meshlet, its window read out of the
        /// table, each meshlet culled against the camera before it emits anything. Preferred over both others when built.
        std::optional<vk_pipeline> gbuffer_pipeline_meshlet = std::nullopt;
        /**
         * @brief THE MESH FORM OF EACH NAMED PIPELINE, under the same name (docs/mesh_shaders.md step 2)
         *
         * A second map rather than a second field on `vk_pipeline`, because the two are alternatives rather than a
         * pair: a leaf asks for a NAME, and the session binds the mesh form when the device built one - so anything
         * that reads the registry has to be able to ask "is there a mesh form of this name" as one question.
         * Filled by `make_pipeline` when the app hands the mesh stage's SPIR-V over, so a device without mesh
         * shaders simply has an empty map and every lookup falls through to the vertex pipeline.
         */
        // `std::map`, not `std::unordered_map`: cl 19.44 ICEs (C1001, msc1.cpp:1589) when the primary
        // loads this partition through `export import` and an exported class inside an interface
        // partition holds a `std::unordered_map` data member (lead_lab/rr_map_lab/FINDINGS.md: 7/7
        // `std::map` variants compile clean, 11/11 `unordered_map` ones ICE).
        // `std::less<>` matches the `std::map<..., vk_pipeline, std::less<>> pipelines` member below.
        std::map<std::string_view, vk_pipeline, std::less<>> mesh_pipelines = {};
        /**
         * @brief THE MESHLET FORM OF EACH NAMED PIPELINE, under the same name (docs/mesh_shaders.md step 3)
         *
         * A THIRD map rather than a flag, for the reason the mesh one is a second map: the three forms differ in how
         * their geometry ARRIVES, and a session's flag has to follow the bind that chose one. The meshlet form draws
         * one workgroup per meshlet - its window read out of the heap table, culled against the camera - which is
         * what a forward/unlit/transparent leaf wants exactly as much as a G-buffer one does. Filled by
         * `make_pipeline` when the app hands that entry's SPIR-V over; empty on a device without mesh shaders, or
         * when the app passed no meshlet file, and every lookup then falls through to the mesh form, then the vertex
         * one - so the three are tried in the order of how much they save, and the last is always a complete answer.
         */
        // the same cl 19.44 C1001 reason as `mesh_pipelines` above (see lead_lab/rr_map_lab/FINDINGS.md)
        std::map<std::string_view, vk_pipeline, std::less<>> meshlet_pipelines = {};
        // whether the opaque pass writes the G-buffer this frame (see set_gbuffer_debug). Only
        // takes effect once the needed pipelines exist, so the flags can be set before setup ends.
        bool gbuffer_debug = false;
        // which channel the debug view shows (see gbuffer_debug.frag / set_gbuffer_channel)
        // The G-buffer images are heap slots now: publish_frame_resources writes each image's grid slot once
        // per frame, and there is no family, pool or set left to keep here.

        struct gbuffer_debug_push_constants {
            float channel = 1.0f; // 0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 id, 6 depth, 7 flags, 8 motion
            float proj_22 = 0.0f; // projection[2][2] / [3][2]: the depth-linearization terms
            float proj_32 = 0.0f;
            // Amplification for the motion channel only. The stored vector is a UV-space delta, so a
            // pixel of motion at 1080 wide is 0.00093 and the raw value would be black everywhere;
            // the runtime sets this to width/4 so four pixels saturate the channel, which scales
            // itself across resolutions instead of being a magic number per window size.
            float motion_gain = 1.0f;
        };
        // ---- THE PASSES: OWNED BY THE CHAIN, HELD HERE AS VIEWS --------------------------------------
        //
        // THE OWNERSHIP RULE, and it is the reason this class holds no pass at all: a renderer that declares one
        // member per pass decides when every pass is built and destroyed. That question - who owns whom - is
        // answered in exactly one place in this branch: the device root is the `shared_ptr<core>`, and every other
        // holder holds a VIEW (`pass_filter`, `core::user_filter`, a chain's pass list). The passes were the last
        // exception, and the chain the APPLICATION builds is what removes it: that chain OWNS them (`emplace`), it
        // decides the order they are built and destroyed in, and this class keeps no reference to any of them - its
        // twelve stage arrays hold `frame_pass*` found BY DECLARATION NAME, which is the whole tie.
        //
        // WHAT EACH PASS OWNS is its own GPU objects - its pipeline, its barrier batches, its per-generation
        // state - so what is left in this class is the RENDERER's:
        // the knobs, the frame counters, the push block's VALUES, the two jobs, and the frame state a pass cannot
        // know.
        /**
         * The chain the frame CREATES and RECORDS, handed over by `set_pass_chain` and owned by whoever built it
         * (in this application, `deren.vulkan.render_start_demo`). Every stage array and both GI halves are filled from it
         * BY DECLARATION NAME, which is what lets the passes live outside this class. It is REQUIRED before the
         * frame work: `create_passes` refuses to run without one and says so, and every other question this class
         * asks a chain asks it through a null check rather than falling back to a chain of its own - there is no
         * longer such a thing.
         */
        /// named `frame_chain` rather than `chain`: `bind_frame_chain`/`set_pass_chain` take a
        /// `pass::pass_chain& chain`, and a member of that name would be hidden by the parameter - MSVC /W4 reports C4458, which /WX makes an error
        pass::pass_chain* frame_chain = nullptr;

        // THE PASSES ARE NOT CONSTRUCTED HERE ANY MORE, and the stage arrays below are what is left of this class's
        // knowledge of them: the APPLICATION builds its chain (deren.vulkan.render_start_demo) and hands it over through
        // `set_pass_chain`, which fills these arrays and the two GI halves BY DECLARATION NAME - the stage sequence,
        // its order and the marks are the frame loop's and stay. The two JOBS are not
        // passes and are still created here, from the same context the passes are.
        /// the bloom chain's stage, in level order (the runner walks the array; a stage IS the order, which is why
        /// it is an array of pointers and never a container whose iteration order is an accident)
        // called bloom_pass, not bloom_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 4> bloom_pass = {};
        /// the composite's own stage: one pass, the frame's display work (and the frame's LAST writer whenever
        /// FXAA is off, which is why its frame carries the overlay)
        std::array<pass::frame_pass*, 1> post_composite_stage = {};
        /// the shadow pass's stage: it runs BEFORE the scene pass (the maps have to exist before the surfaces that
        /// sample them are shaded), and the frame loop records it only on a frame the maps are not reused.
        // called shadow_pass, not shadow_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> shadow_pass = {};
        std::array<pass::frame_pass*, 1> gbuffer_debug_stage = {};
        // called fxaa_pass, not fxaa_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> fxaa_pass = {};
        /**
         * THE UPSCALE PASS (deren.vulkan.pass.upscale): the resolve that scales the render chain back up to the
         * output, and the frame's LAST writer on the frames it runs. It is the only pass in the frame whose
         * extent is the SWAPCHAIN's rather than the render extent's (its declaration says so - see
         * `upscale_io`), and it is mutually exclusive with the FXAA pass by `post_fxaa_active()`: both read the
         * composite's LDR image and both want to write the presented one.
         */
        // called upscale_pass, not upscale_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> upscale_pass = {};

        // called rt_shadow_pass, not rt_shadow_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> rt_shadow_pass = {};
        /// the deferred lighting stage's own stage: it sits between the ray-traced shadow (whose output its
        /// descriptor samples) and the transparent pass (which composites over the image it shades), which is
        /// where the frame loop records it and the only fact about it the renderer still spells out.
        // called deferred_pass, not deferred_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> deferred_pass = {};
        // The stochastic PUNCTUAL LIGHTING chain ([render] megalights, docs/megalights.md): the pass's own
        // stage, which runs between the G-buffer and the deferred lighting stage - the lighting stage is what
        // ADDS its result, so it has to have it, and the G-buffer is what it evaluates its lights against.
        /// TWO passes, a tracer then its temporal resolve - a two-half split without a second
        /// second STAGE, because the ordering rule the resolve needs (the G-buffer's depth and the motion-vector
        /// target have to be published for it) runs in this stage's own prepare before either pass records.
        // called megalights_pass, not megalights_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 2> megalights_pass = {};
        // Whether the punctual lights are the stochastic pass's business this frame. When it is, the deferred
        // lighting stage skips its own raster punctual loop (the `punctual_replaced` lane) rather than adding
        // the same lights twice - a REPLACE rather than an addition.
        bool megalights_on = false;
        /// Whether the stochastic pass RECORDED this frame (see `frame_facts::megalights_resolved`): cleared
        /// immediately before the stage is recorded and set from its report, so the lighting stage's lane can
        /// never disagree with what actually ran.
        bool megalights_resolved = false;
        /// Per swapchain image: whether that image's stochastic accumulation holds a frame yet. The counterpart
        /// feature's off -> on edge).
        std::vector<bool> megalights_history_valid = {};
        // The furnace verification mode ([render] furnace, not wired to the config yet): the sun is turned
        // off and the environment becomes a constant level, so the correct frame is computable by hand.
        bool furnace = false;
        // Whether this target generation's furnace cube has had its level written yet. It is cleared ONCE
        // per generation (see begin_recording): the level never changes, so re-clearing it every frame would
        // be a barrier pair bought for nothing, and the flag is reset with the images it describes.
        bool furnace_cube_ready = false;

        // THE DEFERRED LIGHTING STAGE'S SSAO PARAMETERS, ITS FLAT-RENDER FLAG AND ITS PUSH BLOCK ARE THE PASS'S
        // NOW (see vulkan.pass.deferred::set_ssao / set_unlit / push_constants): the shape belongs to the pass that
        // pushes it, the values belong to the pass that reads them, and `set_ssao` / `set_unlit` forward. What is
        // left here is the frame's own half - the inverse view-projection below, refreshed with the camera UBO.
        // the inverse of this frame's view-projection, refreshed with the camera UBO in
        // pace_and_acquire() (the deferred lighting stage reconstructs world positions from depth)
        glm::mat4 current_inv_view_proj = glm::mat4(1.0f);
        // This frame's projection WITHOUT the TAA jitter: the shadow fit extracts near/far from
        // the projection's z-row (a product with the view matrix does not carry those terms) and
        // rebuilds the sub-frustum corners from it.
        glm::mat4 current_proj_unjittered = glm::mat4(1.0f);
        /// @brief the two per-image pieces of bookkeeping the debug view's frame carries (the depth's hand-back and
        ///        the motion-vector flag's clearing): the pass's header says why they are not the pass's
        /// @brief the frame's answer when the debug view did NOT record: clear the HDR target, so the frame the post
        ///        chain samples is defined (a black frame) instead of half-written
        void clear_hdr_for_missing_gbuffer_set(VkCommandBuffer command_buffer);
        /// @brief resolve the deferred lighting pass's frame: the two shared blocks, the frame's scene target,
        ///        the pass's own 88-byte push block and the extent its declaration's rule produces
        /// @return false when this frame cannot run it (no target generation, no pipeline)
        /// @brief the two per-image input transitions the lighting stage's push block names (the three
        ///        stored targets and the G-buffer depth): their "was it written this frame" flags belong to the
        ///        pass that WROTE those images, so the pass cannot own them and the frame carries the callback
        /// @brief the frame's answer when the lighting pass did NOT record: clear the scene colour target, so
        ///        the frame the post chain samples is defined instead of half-written
        /// @note this is the renderer's and not the pass's because its cause - no lighting pipeline this frame -
        ///       is a startup failure of a pipeline this class created; the pass never sees it
        void clear_scene_color_for_missing_gbuffer(VkCommandBuffer command_buffer);

        // ---- temporal anti-aliasing (M3, deferred path only) ----
        // TAA is the engine's anti-aliasing: the projection is jittered per frame (a Halton
        // sequence), the G-buffer writes motion vectors, and a resolve pass blends the current frame
        // with a reprojected, neighborhood-clamped history. The deferred scene writes
        // core::scene_color and the resolve writes the HDR target, so the whole post chain keeps
        // reading exactly what it read before TAA existed. A copy of the resolved frame becomes the
        // next frame's history (no ping-pong, hence no per-frame descriptor rewrites).
        // The TAA resolve is a PASS (deren.vulkan.pass.taa): it owns its pipeline and its per-image history flags, so
        // all that is left here is the pass member and the stage the runner is handed.
        // Per swapchain image: whether that image has a GI history yet. First frame after startup or
        // after a resize there is none, and the resolve then uses the current trace alone.
        // The shaders the app has loaded, by file name, for the passes that build their own pipelines. The APP
        // is the loader (it knows the shader directory); the runtime is only the place a pass asks. A copy
        // rather than a view, because the caller's buffer is a local in a startup scope.
        std::vector<std::pair<std::string, std::vector<uint8_t>>> registered_shaders = {};
        // pass when the generation changed or when the chain was switched on (see on_swapchain_recreated and
        // The alphaMode MASK bake's push block is NOT here any more: its shape is the JOB's
        // (pass::mask_bake_push_constants in deren.vulkan.pass.mask_bake), because only that job composes it.
        // THE SCENE PASS (deren.vulkan.pass.scene): it owns the surface instance, the segment strategy and the draw
        // loop; the renderer hands it the leaves through a typed frame (see make_scene_frame) and keeps the
        // pipeline registry, the secondary buffers and the scheduler.
        // called scene_pass, not scene_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> scene_pass = {};
        /// THE TRANSPARENT PASS (deren.vulkan.pass.transparent): the same scene, its own LOAD instance, after lighting
        // called transparent_pass, not transparent_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> transparent_pass = {};
        /**
         * THE CHARACTER-FORWARD PASS (deren.vulkan.pass.character_forward): the same OPAQUE leaves a second time,
         * shaded by the toon pipeline and written OVER the deferred result at depth-EQUAL. It sits after the
         * transparent pass (a toon body drawn under a blended surface would be composited over, which is the
         * right order: the blend belongs on top) and before the resolve.
         */
        // called character_forward_pass, not character_forward_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> character_forward_pass = {};
        /**
         * THE SCREEN-SPACE DEPTH RIM (deren.vulkan.pass.toon_screen_rim): a fullscreen additive contour, right after
         * the surface it outlines and before the resolve. It has no frame of its own - its parameters are the
         * pass's and its inputs are heap slots - so this array is only what the chain lookup fills.
         */
        // called toon_screen_rim_pass, not toon_screen_rim_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> toon_screen_rim_pass = {};
        /**
         * THE REWRITTEN CHAIN'S RIM (deren.vulkan.pass.goo_rim): the SAME screen-space shape as the contour above -
         * a fullscreen additive stage, no frame of its own - placed right after it, and the two are mutually
         * exclusive by construction rather than by order: the owner answers this pass's feature with
         * `goo_toon_active()`, which is the negation of the predicate that answers the contour's. On a frame the
         * rewritten chain draws, this stage records and that one does not; on every other frame neither does.
         */
        // called goo_rim_pass, not goo_rim_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> goo_rim_pass = {};
        /// the scene frame's view of the per-slot segments (a member, so the span it hands the pass outlives it)
        std::vector<pass::segment_buffer> scene_segment_view = {};
        /// the colour formats the scene pass's secondaries inherit, in attachment order
        std::array<VkFormat, deren::vulkan::gbuffer_pass_attachment_count> scene_color_formats = {};
        // called taa_pass, not taa_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> taa_pass = {};
        bool taa_on = false; // [render] taa
        /**
         * WHETHER THE TOON CHARACTER STAGE IS SWITCHED ON. Default FALSE, and that default is the whole
         * contract of this feature's landing: with it off the pass's `feature()` answers inactive, the runner
         * never resolves or records it, and every frame is bit-for-bit what it was before the stage existed -
         * which is what the capture gate checks and what this project requires of a behaviour-visible
         * addition. `set_character_forward` is the only writer.
         */
        bool character_forward_on = false;
        /**
         * WHICH TOON CHAIN THE CHARACTER STAGE DRAWS WITH (see @ref goo_toon_pipeline_name and `set_goo_toon`):
         * false = the old `character_forward.slang`, true = the rewritten `goo_toon.slang`.
         *
         * SEPARATE FROM `character_forward_on` ABOVE AND NOT FOLDED INTO IT, because they answer two different
         * questions: whether the stage runs, and which shading model it runs. The rewrite's A/B holds the first
         * fixed and moves the second, which is a comparison that cannot be expressed with one flag.
         */
        bool goo_toon_on = false;
        // The two blend weights are NOT here any more: they are the TAA pass's own parameters now, set through
        // `set_taa` (which forwards them) and clamped by the pass - see vulkan.pass.taa::set_blend. What stays
        // here is the SWITCH and the jitter phase, because both are frame-loop state: the switch decides whether
        // the projection is jittered at all and which target the scene side writes, and the jitter index is the
        // Halton position the frame loop advances.
        uint32_t taa_jitter_index = 0; // position in the Halton sequence
        // The view-projection each swapchain image's history was rendered with. Remembered PER IMAGE on
        // purpose: with several swapchain images in rotation, "the previous frame's camera" is not what that
        // image's history was rendered with, and reprojecting against the wrong matrix is exactly what makes a
        // TAA history smear. It stays HERE rather than in the pass because the camera UBO is what reads it -
        // as `prev_view_proj`, which is where the motion vectors come from - while the pass is what writes it
        // (through `taa_pass::wrote_history`, the renderer updates it only for a frame that really resolved).
        std::vector<glm::mat4> image_view_proj = {};
        /**
         * @brief the host the pass runner talks to: the callbacks a stage needs, and the create-time facts
         * @note rebuilt per call (it is a struct of function pointers), so it holds no state of its own; the
         *       context is this runtime, which is what the callbacks cast back to
         */
        [[nodiscard]] pass::pass_host make_pass_host() noexcept;
        /**
         * @brief the frame a pass is being recorded in: both counters, the generation's image count, the extent
         * @note `image_count` is the swapchain generation's count and NOT the image index - a pass that owns a
         *       per-image descriptor family sizes it from the first and indexes with the second
         */
        [[nodiscard]] pass::frame_identity pass_frame() const noexcept;
        /// the bytes of a shader the app registered, by file name (empty when it did not)
        [[nodiscard]] std::span<uint8_t const> registered_shader(std::string_view name) const noexcept;
        /**
         * @brief run the HEAP-NATIVE probe once (see shaders/heap_probe_comp.slang): the first pipeline in this renderer
         *        with NO layout at all, its parameters through vkCmdPushDataEXT, reading a texture from the resource
         *        heap through a sampler from the sampler heap
         * @param texture_slot the ABSOLUTE grid slot of the texture to sample (heap_slots::textures + an index)
         * @note it is a member rather than a free helper for the ordinary reason - it needs the device, the
         *       registered shaders and the heap - and it is called ONCE, at scene setup, in a command buffer of its
         *       own: that is what makes it isolated. The answer is a line in the log, because the question it exists
         *       to answer ("does the native path work here?") cannot be answered by a picture until the whole frame
         *       is converted.
         */
        void run_heap_probe(uint32_t texture_slot);
        /**
         * @brief run the GRAPHICS half of the heap-native probe once (see shaders/heap_probe.slang)
         * @param material_slot the ABSOLUTE grid slot of the material table the fragment stage reads; the caller
         *        runs it once with the right slot and once with a deliberately WRONG one, because a probe that can
         *        only say "fine" would pass every check
         * @note the same question as run_heap_probe for the pipeline kind the frame is mostly made of: a
         *       heap-flagged, layout-less GRAPHICS pipeline whose fragment stage reads the heap. It renders the
         *       default material's base colour into a 4x4 target cleared to black first - so a white pixel can only
         *       have come from the shader - and reads it back.
         */
        /// @param mesh_shader when true the SAME probe runs through a MESH pipeline instead of a vertex one
        ///        (docs/mesh_shaders.md step 0): the same triangle, the same fragment stage, the same readback,
        ///        which is what makes the mechanism comparable in the log rather than through a screenshot
        void run_heap_graphics_probe(uint32_t material_slot, bool mesh_shader = false);
        /**
         * @brief the ONE create-time context every pass is built with
         *
         * It was a block inside `create_passes()` plus a copy per job (the MASK bake, the compute-skinning job) -
         * and a second copy of this struct is exactly how a per-pass entry point per job appears. One builder,
         * used by everything that constructs a pass, is what keeps that from growing back.
         */
        [[nodiscard]] pass::pass_context make_pass_context() noexcept;
        /**
         * @brief publish the resources a PASS may name, in the declaration layer's own vocabulary
         *
         * Called once, before the passes are created: it is the renderer's half of the pass filter's registry
         * (`register_resource`), and it is what lets a job name a resource the renderer holds (the material
         * table, the bindless texture array, the per-slot skin matrices) instead of the renderer doing it through
         * an entry point per pass. Everything published here is
         * SESSION-STABLE by the filter's contract - created once, contents rewritten.
         */
        void publish_pass_resources();
        /// the samplers a declaration chooses between, in one place (see pass_context::samplers)
        [[nodiscard]] render_resource::shared::sampler_set shared_samplers() const noexcept;
        /// @brief whether the LIGHTING STAGE is in the flat render mode this frame, published by the chain's owner
        bool scene_unlit = false;
        /**
         * @brief fill this frame's shared constants (`pass::resolved_io::constants`) from the camera/light state
         *
         * Called once per frame, after `pace_and_acquire` has computed the camera record, the light UBO and the
         * fitted scene bounds - so every pass of that frame reads the same numbers the shaders see. It exists as
         * a function because it is the ONE place where `deren.vulkan.frame_constants` and `camera_ubo`/`light_ubo` have
         * to agree (the facts type deliberately does not include those, so that the pass framework keeps its
         * distance from `deren.vulkan.core`).
         */
        void update_frame_constants() noexcept;
        /**
         * @brief publish every resource that exists right now into `frame_resources`, in the declaration's
         *        vocabulary (`resource_id` + element + instance)
         *
         * Called once per frame before the first stage records: a per-swapchain-image view is a generation
         * object, an alias like `scene_color` is decided per frame (TAA or HDR), and the lazily created shadow
         * map only exists once the scene's resources do - so "this frame's resources" is the only honest publication
         * time. See `pass::resource_table` for why the table exists at all.
         */
        void publish_frame_resources();
        /**
         * @brief the differential check of the resource table against the resolver that just ran
         *
         * THE MIGRATION'S ORACLE: while a pass still has a hand-written resolver in this class, that resolver's
         * answer - the handles it put into `own`, `targets`, `barrier_images` and `barrier_buffers` - is compared
         * against what the table answers for the SAME declaration entries. A mismatch is a table entry that is
         * wrong or missing, which is exactly the fact that has to be right before the resolver can be deleted;
         * it is logged (once per pass) rather than fatal, so a wrong entry can never change a frame while the
         * layering is being moved. The check becomes vacuous for a pass whose resolver is gone, which is the
         * point at which it has nothing left to disagree with.
         */
        void verify_resource_table(pass::frame_pass const& pass, pass::resolved_io const& io);
        /// create the TAA resolve's shared sampler if it does not exist (see create_passes for why it is
        /// made here rather than in a pipeline builder)
        /**
         * @brief resolve a pass's declaration into this frame's handles (the runner's `resolve` callback)
         * @return false when this frame cannot run the pass, which skips it WITHOUT recording anything
         * @note THE PER-PASS SWITCH IS GONE: this function fills the frame's shared constants and asks the pass
         *       to resolve its own declaration (see `frame_pass::resolve`), so the table's contents and the
         *       frame's constants are all the renderer contributes. A pass cannot reach a resource its
         *       declaration does not name, and the renderer no longer knows which pass wants which image.
         */
        [[nodiscard]] bool resolve_pass(pass::frame_pass const& pass, pass::resolved_io& out);
        /** @brief the behaviour's mechanical part, before the pass records: bind the pipeline(s), resync the
         *         viewport. A pass cannot forget these because it does not do them */
        void apply_pass_behaviour(pass::frame_pass const& pass, pass::resolved_io const& io);
        /**
         * @brief whether the pass whose DECLARATION is named @p name is ready to record (see `frame_pass::ready`)
         *
         * THE FIRST QUESTION THIS RENDERER ASKS IN THE CHAIN'S OWN VOCABULARY instead of through a typed member.
         * Every feature answer used to read `deferred.pipeline_ready()`, `taa_resolve.pipeline_ready()` and ten more
         * like them; they now ask the chain by the name the declaration carries, which is the only key a renderer
         * handed a chain from OUTSIDE has - and "handed from outside" is where this layer is going (see
         * `deren.vulkan.render_start_demo`).
         * @note the frame's own pipelines that no pass owns (the G-buffer's, the shadow fit's) are still the
         *       renderer's members, so a feature that needs one of those AND a pass's still reads both
         */
        [[nodiscard]] bool pass_ready(std::string_view name) const noexcept;
        /**
         * @ingroup vulkan_runtime
         * @brief whether the TAA resolve runs this frame (enabled + deferred lighting + pipeline)
         */
        [[nodiscard]] bool taa_active() const noexcept;
        /** @brief the image the scene-side passes write into (the TAA input, or the HDR target) */
        [[nodiscard]] VkImage scene_target_image(uint32_t image_index) const noexcept;
        [[nodiscard]] VkImageView scene_target_view(uint32_t image_index) const noexcept;
        /** @brief the sub-pixel jitter for a position in the Halton(2,3) sequence, in PIXELS */
        [[nodiscard]] static glm::vec2 taa_jitter_offset(uint32_t index) noexcept;
        /** @brief whether the opaque pass writes the G-buffer this frame (pipelines present + enabled) */
        // The opaque pass writes the G-buffer and the lighting stage shades it - the engine's only
        // scene path, so there is no flag for it. gbuffer_pass_active() reports whether the G-buffer
        // can run at all (its pipelines exist, and the debug view has its own).
        [[nodiscard]] bool gbuffer_pass_active() const noexcept;
        /** @brief whether the lighting stage shades this frame (the G-buffer pass runs and the debug view is off) */
        [[nodiscard]] bool deferred_lit_active() const noexcept;

        /**
         * @brief record the barrier that hands the G-buffer depth to the stage that SAMPLES it, and
         *        return true when a barrier was written
         * @param command_buffer the frame's command buffer
         * @param image_index the swapchain image whose G-buffer depth is being sampled
         *
         * Three stages read that depth - the deferred lighting stage (binding 3, world-position
         * reconstruction and SSAO), the TAA resolve (its disocclusion guard) and the G-buffer debug
         * view (channel 6) - and a frame runs the lighting stage OR the debug view, with TAA layered
         * on the lighting stage. The layout to transition FROM therefore depends on whether the
         * G-buffer pass rendered this frame, which is what gbuffer_depth_written tracks; that is why
         * this is one accessor instead of a `shadow_map_sampling_transition` copy at each of the
         * three call sites (two of which would then declare an old layout the image is not in).
         *
         * The transition itself is shadow_map_sampling_transition: GENERAL as the old layout (as it is
         * for every image now) with the fragment-test src masks, so the attachment write is published
         * AND the contents survive - unlike undefined_to_depth_sampling_transition, whose UNDEFINED
         * old layout is only honest for a depth nothing has written (see that constant's warning).
         * @note must be called outside a rendering instance (it records a pipeline barrier)
         */
        bool ensure_gbuffer_depth_sampled(VkCommandBuffer command_buffer, uint32_t image_index);
        // Per-swapchain-image flag: set by the G-buffer instance, cleared by
        // ensure_gbuffer_depth_sampled(). Per IMAGE rather than per frame because each image owns
        // its own depth image, and that depth keeps whatever layout its last recorded frame left it
        // in until that image comes around again.
        std::vector<bool> gbuffer_depth_written = {};

        /**
         * @ingroup vulkan_runtime
         * @brief publish the stored surface targets' attachment writes and make them samples
         * @return whether a transition was recorded (false = they were already readable)
         * @note the SAME "whose write is it" question ensure_gbuffer_depth_sampled() answers, for the
         *       three stored surface targets: the G-buffer instance writes them as COLOR attachments and
         *       TWO stages sample them - the ray-traced sun shadow (which runs first when it runs at all)
         *       and the deferred lighting stage. Whoever gets here first does the transition and the other
         *       finds the flag clear; without it the second one claims a COLOR_ATTACHMENT old layout the
         *       image is not in, which is a validation error and, on a driver that believes it, a
         *       discarded surface.
         * @note must be called outside a rendering instance (it records a pipeline barrier)
         */
        bool ensure_gbuffer_targets_sampled(VkCommandBuffer command_buffer, uint32_t image_index);
        // Per-swapchain-image flag: set by the G-buffer instance, cleared by the accessor above.
        std::vector<bool> gbuffer_targets_written = {};

        /**
         * @ingroup vulkan_runtime
         * @brief publish the G-buffer motion-vector target's attachment write and make it a sample
         * @return whether a transition was recorded (false = the image was already readable)
         * @note the same "whose write is it" question ensure_gbuffer_depth_sampled() answers, for the
         *       motion-vector target: the G-buffer instance writes it as a COLOR attachment, and the
         *       two stages that sample it are the TAA resolve (a FRAGMENT stage, which transitions it
         *       itself as part of its own barrier batch) and the GI denoiser's resolve (a COMPUTE
         *       stage, which is why this exists - a second unconditional COLOR_ATTACHMENT old layout
         *       would be a lie on every frame where TAA already moved the image).
         * @note must be called outside a rendering instance (it records a pipeline barrier)
         */
        bool ensure_velocity_sampled(VkCommandBuffer command_buffer, uint32_t image_index);
        // Per-swapchain-image flag: set by the G-buffer instance, cleared by whichever stage first
        // hands the motion-vector target to a sampler (see ensure_velocity_sampled).
        std::vector<bool> velocity_written = {};
        /**
         * Per frame slot: the acceleration structure handle whose heap slot was last WRITTEN.
         *
         * WHY IT EXISTS, and it is a validation error rather than tidiness: the top level structure is rebuilt
         * every frame, and its heap descriptor is rewritten here - so a frame still in flight may be reading the
         * slot the write lands in. With two frames in flight the other slot's frame is routinely still running,
         * which is what made the write invalidate it: measured, 151 validation errors in a frame - the first one
         * naming that command buffer, the rest the cascade of calls on a buffer
         * the layer had already invalidated - and they appeared only when the frame rate was low enough for a
         * frame to still be in flight (which is why turning the demo lights on was what surfaced them).
         *
         * The guard is exact rather than a heuristic: the handle is what the heap slot HAS to carry, so
         * rewriting it with the same value is a no-op that costs a needless write, and rewriting it with a
         * different one is required. A structure REBUILT into a different buffer still writes here, which is
         * the rare case the project's other per-generation writes also accept.
         */
        std::vector<VkAccelerationStructureKHR> rt_binding_written = {};

        /** @brief the swapchain was rebuilt: drop everything that pointed at the old generation
         *         (the debug overlay's backend, whose views are gone) */
        void on_swapchain_recreated();

        // ---- post-processing: HDR scene target -> exposure + ACES + gamma -> swapchain ----
        // THE CHAIN'S GPU MATERIAL, THE PUSH BLOCK'S SHAPE AND THE FXAA PIPELINE ARE ALL PASSES' NOW
        // (deren.vulkan.pass.post, deren.vulkan.pass.fxaa): the composite owns the chain's two pipelines, the push block's
        // type lives with the passes that push it (this class fills a VALUE of it per frame, in the resolvers),
        // and FXAA's pipeline is its own pass's. What is left here is the two samplers the pass context hands over
        // and the values themselves.
        // FXAA state: enabled + the two knobs the shader takes (see pass::post_push_constants)
        bool fxaa_on = false;
        float fxaa_subpixel = 0.75f;
        float fxaa_edge_threshold = 0.166f;
        // A second sampler for the composite's GI upsample: the GI image, the G-buffer depth and the
        // G-buffer normal are all read AT exact texel centres and must not be interpolated (averaging
        // two depths invents a surface between them, which is exactly what an edge-aware test must not
        // see). Everything else in the post chain wants the linear one above.
        // Whether the composite upsamples the GI bilaterally or with the plain bilinear fetch (see
        // pass::post_push_constants::gi_upsample). On by default; false exists for measurement.
        // The post chain's sets are gone with every other set in this renderer: the composite, the bloom levels
        // and FXAA read their images through the frame's heap.
        // per-stage render toggle: whether the shadow pass actually records this frame. Shadow off
        // skips the depth pass (the shadow map is cleared to fully-lit so the main pass samples "no
        // shadow"). Defaults on.
        bool shadow_enabled = true;

        // ---- directional shadow mapping (scene block slot 7 light UBO + binding 8 shadow map) ----
        // Shadow map edge length in texels ([render] shadow_map_size). A MEMBER, not a constant,
        // because M7 surfaced it as a config knob - every user of it (the layered image, the depth
        // pass's rendering instance + pipeline viewport, the light UBO's texel size and the fit)
        // reads this value at creation time. Startup-only: set_shadow_map_size() before the scene
        // import (like set_shadow_cascades, the resources are created when the first scene set binds
        // them); changing it afterwards would need the image, the views and the descriptor rewritten.
        uint32_t shadow_map_size = 2048;
        // Layers currently owned by shadow_images: one per ACTIVE cascade (see ensure_shadow_resources).
        // Tracked separately from shadow_cascades because shrinking the count keeps the layers that are
        // already allocated - only growing it costs a rebuild.
        uint32_t shadow_allocated_layers = 0;
        // Frame limiter (see set_max_fps): the instant the next frame may start, advanced by exactly one
        // period per frame so a slow frame resyncs instead of banking debt (which would show up as a
        // burst of catch-up frames) and a fast one does not drift.
        double max_fps = 0.0;
        std::chrono::steady_clock::time_point next_frame_deadline = {};
        // Cascaded shadow maps: ONE 2D-array depth image per frame slot (while slot A is in flight,
        // slot B already rewrites its own map, so the two never race on the same image), with
        // shadow_cascades layers - each layer fitted to its own sub-range of the camera view.
        // Rendering goes through the per-layer views (one cascade = one dynamic rendering instance),
        // sampling through the array view (the fragment shader picks its cascade per pixel).
        std::vector<vk_image> shadow_images = {};                        // layered depth images
        std::vector<vk_image_view> shadow_array_views = {};              // 2D ARRAY views (sampled)
        std::vector<std::vector<vk_image_view>> shadow_layer_views = {}; // per slot: one 2D view per cascade
        // Active cascades (1 = exactly the single-shadow-map behavior; [render] shadow_cascades) and
        // the fraction of a cascade's range over which the shader blends into the next one.
        uint32_t shadow_cascades = 1;
        float shadow_cascade_blend = 0.1f;
        bool shadow_cascade_logged = false; // one-time per-cascade texel-density log
        // one-time log for the shadow-caster switch (see begin_recording): scenes over
        // full_scene_shadow_leaf_limit draw a culled subset instead of every leaf - say so once
        // instead of silently changing behavior
        bool shadow_heuristic_logged = false;
        // Light UBO (scene block slot 7): ONE host-visible buffer per frame slot, like the
        // camera UBO - each slot's scene set points at its own buffer, so the per-frame host
        // write into the paced slot's copy can never race a frame still in flight on the other
        // slot. CPU-side light_state mirrors the content: enable_shadows() fills it once,
        // set_shadow_enabled() flips its flag, and pace_and_acquire() copies it into the paced
        // slot's buffer next to the camera UBO - arbitrary-time calls (GUI callbacks included)
        // never touch mapped memory directly.
        std::vector<rhi::object_manager<rhi::buffer>> light_buffers = {};
        std::vector<void*> light_mapped = {};
        light_ubo light_state = {};
        // THE HEAD FRAME (scene block slot 749), the same per-frame-slot arrangement as the light UBO above and
        // for the same reason: on a model whose head TURNS these three vectors change every frame, so a frame in
        // flight must not read a buffer the next frame is rewriting. `head_state` is the CPU-side value and
        // `set_head_basis` is the only thing that writes it; pace_and_acquire copies it into the paced slot.
        //
        // IT IS INITIALISED TO THE REFERENCE'S FALLBACK FRAME rather than to zeros, which is what a scene that
        // never calls set_head_basis gets: zero vectors would reach the sigmoid's `atan2` and produce NaNs, i.e.
        // a face that is black or flickering instead of one shaded by the fallback.
        std::vector<rhi::object_manager<rhi::buffer>> head_buffers = {};
        std::vector<void*> head_mapped = {};
        head_ubo head_state = {};
        // linear exposure scale applied before tonemapping; copied into light_state.light_count.y
        // (the light UBO's first unused lane) right before the per-frame UBO upload, and pushed to
        // the shading stages apply through the scene layout's push-constant range (see
        // record_main_segment)
        float exposure_scale = 1.0f;
        float bloom_intensity = 0.0f;
        float bloom_threshold = 0.6f;
        // cel/toon shading: quantization steps (0 = plain PBR) and the band edge softness;
        // copied into light_state.light_count.z/w every frame like the exposure lane
        float toon_steps = 0.0f;
        float toon_softness = 0.15f;
        // a scale on the sun (runtime::set_sun_intensity); the furnace mode forces the lane to 0 regardless
        float sun_intensity = 1.0f;
        // THE SUN'S DIRECTION (runtime::set_sun_direction), pointing FROM the surface TOWARD the sun and
        // unnormalized. It is the one source the light UBO is built from (`make_directional_light_ubo`), so
        // changing it moves the shading, the shadow cascades and - through the UBO's `light_dir`, which
        // `sky.glsl` reads - the disc in the sky together. The default is the historic hard-coded vector.
        glm::vec3 sun_direction = glm::vec3(0.3f, 1.0f, 0.5f);
        // bloom parameters (see set_bloom): blend weight into the HDR image and the bright-pass
        // threshold subtracted in linear space (0 intensity disables the effect)

        // F12 screenshot request: set edge-triggered by poll_events, consumed by the caller
        // (see consume_screenshot_request / acquire_current_frame_image)
        bool screenshot_requested = false;
        bool screenshot_key_down = false;
        // Read-back copy of the presented image, recorded INSIDE the frame's own command buffer
        // (end_recording, right before the present transition) while the swapchain image is still
        // owned by the app. The old path transitioned the image after vkQueuePresentKHR, which the
        // spec forbids - the presentation engine owns it by then (validation: "performs a layout
        // transition on presentable VkImage ... but the image has not been acquired").
        // screenshot_pending = a copy was recorded and is ready to be read once the submit lands.
        bool screenshot_pending = false;
        // one-time log for "this surface cannot do screenshots" (see end_recording's copy_image_to_buffer
        // result)
        bool screenshot_unsupported_logged = false;
        // S2 BATCH 2: WHAT IS LEFT HERE IS ABOUT THE FRAME, NOTHING ABOUT THE BUFFER. The staging pointer
        // and the record-time extent used to live here because the recorded copy wrote into a buffer this
        // runtime owned; the copy is now recorded through the contract's recording surface and the bytes
        // are read from the BACKEND's slot (core::frame_readback_buffer()), so the frame's own state is
        // the one-shot flags above and nothing else.
        bool shadows_enabled = false; // true after enable_shadows() (light UBO filled + pipeline ready)
        // live-tunable depth bias of the shadow pass (dynamic state, set per frame before the
        // depth-only draw): slope-scaled bias removes acne on angled surfaces, the constant
        // factor adds a fixed push; tune from the debug gui when a model shows acne/peter-panning
        float shadow_depth_bias_constant = 0.0f;
        float shadow_depth_bias_slope = 1.5f;
        float shadow_depth_bias_clamp = 0.0f;

        // ---- clustered light culling (M5) ----
        // THE PASS (deren.vulkan.pass.cluster) owns the pipeline AND the two
        // buffer barriers its own writes need; the renderer keeps the grid's dimensions (they come from the
        // swapchain extent) and hands the cluster count over in the pass's frame. Optional: without the shader
        // (or with clustering off) shade_surface() falls back to the brute-force loop, which is exactly what the
        // clustered path is verified against.
        // called cluster_pass, not cluster_stage: the local of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::array<pass::frame_pass*, 1> cluster_pass = {};
        // Per-cascade shadow recording pairs (one {pool, buffer} per cascade per frame slot). The
        // cascade tasks run CONCURRENTLY on the task pool, and a VkCommandPool is not thread safe, so
        // they may not share one - the same rule the main-pass workers already follow. The shared
        // secondary_command_buffers pool stays for the slot-scoped passes (gui, transparent) that the
        // primary thread records alone.
        std::vector<std::vector<std::pair<VkCommandPool, vk_command_buffer>>> shadow_recording = {};
        // Reused task scratch: the frame builds its task batches into these vectors every frame, so
        // they are members with clear() (capacity kept) instead of a fresh heap allocation per frame
        // (M9 overhead trim: a per-frame allocation plus one std::function per task is a few
        // microseconds of a ~0.35 ms frame on a light scene).
        std::vector<std::function<void()>> shadow_task_scratch = {};
        // Shadow-map reuse (M9): the maps depend on the fitted cascade matrices AND on where the
        // casters actually are, so a slot may skip the pass only when BOTH are unchanged since that
        // slot last rendered. The fit half is a counter bumped by every real refit and by the shadow
        // toggles; the geometry half is a hash of the casters' world matrices, computed per frame -
        // an animated/deformed/programmatic scene changes it (the fit cache does NOT, which is exactly
        // the trap the animated-Fox test caught), while a static scene repeats it.
        uint32_t shadow_content_version = 1;
        std::array<uint32_t, deren::vulkan::core::MAX_FRAMES_IN_FLIGHT> shadow_rendered_version = {};
        std::array<uint64_t, deren::vulkan::core::MAX_FRAMES_IN_FLIGHT> shadow_rendered_models = {};
        // The other half of geometry changed: a SKINNED caster keeps a constant push.model, its
        // pose lives entirely in the joint matrices the vertex shader reads, so those are hashed at
        // upload instead (set_skin_matrices). Morph targets have no upload hook at all - the caller
        // writes into morph_scratch() directly - so handing that scratch out bumps a revision and
        // conservatively blocks reuse. The animated Fox is the regression test for the first half:
        // hashing push.model alone left it with a frozen shadow map.
        uint64_t skin_matrix_hash = 1469598103934665603ull;
        // ATOMIC because morph_scratch() is reached from the animation controller's sampling
        // fan-out, i.e. from several task-pool workers at once (animation::controller::update ->
        // backend::morph_scratch_active -> runtime::morph_scratch). A plain counter there is an
        // unsynchronized read-modify-write (UB, and a lost increment on top), and this value feeds
        // shadow_geometry_signature() - so a lost bump keeps a stale shadow map for a frame, which
        // is exactly the failure the revision exists to prevent. Relaxed is enough: it is only ever
        // compared for inequality, never used to publish other data.
        std::atomic<uint32_t> morph_revision = 0;
        std::vector<rhi::object_manager<rhi::buffer>> cluster_count_buffers = {}; // per slot: one uint per cluster
        std::vector<void*> cluster_count_mapped = {};                             // their persistent mappings (memset per frame)
        std::vector<rhi::object_manager<rhi::buffer>> cluster_index_buffers = {}; // per slot: cluster_light_capacity uints per cluster
        bool clustered_lights = true;                                             // set_clustered_lights()
        uint32_t cluster_tiles_x = 0;                                             // active grid this frame (from the extent)
        uint32_t cluster_tiles_y = 0;

        // shared CPU worker pool for frame-time parallel stages (run_tasks). Sized to the
        // machine (hardware_concurrency()/4, floor 1) instead of per-consumer pools; tasks
        // are grouped by priority so each consumer waits only for its own group. Declared
        // before the scene/pipeline state so the pool outlives what tasks may touch (destructor
        // order is reverse declaration: pipelines etc. go first, the pool joins last).
        // The width is init_utils::default_task_pool_threads(), which carries the measurement
        // that decided the quarter (see vulkan/init_utils/init_utils.cppm).
        deren::utility::thread_pool task_pool = deren::utility::thread_pool{init_utils::default_task_pool_threads()};

        // Guards the pipeline registry below (pipelines / default_pipeline_name):
        // parallel recording workers read it through render_environment's binder (shared locks,
        // concurrent), while make_pipeline / set_default_pipeline / resize resync write it
        // (unique lock). A reader/writer lock because reads vastly outnumber writes. Mutable so
        // const accessors (get_pipeline, the env construction inside the const record steps)
        // can take shared locks. The scene tree, per-frame vectors and camera state are NOT
        // guarded: they are only touched on the frame thread or inside run_tasks' synchronous
        // windows, so locking them would only add hot-path cost.
        mutable std::shared_mutex access_mutex;
        // string keys (not string_view): the runtime owns the pipeline names, so lookups
        // stay valid regardless of the caller's storage lifetime. std::less<> enables heterogeneous
        // lookup, so the string_view-based API (get_pipeline / ...) still works without
        // constructing a std::string per call.
        std::map<std::string, vk_pipeline, std::less<>> pipelines;
        // NOTE: there used to be a `std::deque<std::string> pipeline_names` here, "mirroring the
        // pipelines map" so a stable pointer could be handed to render_environment::available. That
        // API does not exist (the recording workers ask the binder by name), so the deque was written
        // by every make_pipeline() and never read. Removed rather than left as a promise.
        // name of the runtime's default pipeline: primitives with DEFAULT semantics (empty
        // pipeline_name) draw with it. Set implicitly to the FIRST created pipeline, or
        // explicitly via set_default_pipeline(); the shadow pass never consults it (its env
        // default is the shadow pipeline).
        std::string default_pipeline_name = {};
        // Scene storage: a scene tree of nodes with local transforms + children; every primitive
        // (normal_draw_primitive / instanced_draw_primitive) lives in a node's primitive leaf. The frame
        // record phase walks the tree once per frame: update_world() accumulates world matrices into each
        // leaf (primitive::set_world -> push.model), then each pipeline draws the leaves bound to
        // it (primitive::draw stays polymorphic). This replaces the old flat per-pipeline primitive list.
        //
        // NON-OWNING: the tree belongs to the caller (set_scene() binds it). The caller must keep the
        // tree alive while the runtime is in use and destroy it BEFORE the runtime (the leaves release
        // their GPU resources through this runtime's vma allocator on destruction). Declaration order in
        // the caller (scene after runtime) gives that order automatically.
        scene_tree::scene* bound_scene = nullptr; // user-owned scene the runtime renders
        // conservative radius of the bound scene around the camera target / scene center
        // (set by enable_shadows, which receives it). The camera projection far plane uses it
        // (make_orbit_camera_ubo) so zooming in never clips the scene's far side.
        // The `_extent_` infix is here because enable_shadows()'s parameter is called
        // scene_radius: a member of the same name would be hidden by it there (MSVC /W4
        // C4458, an error under /WX).
        float scene_extent_radius = 100.0f;
        // optional whole-scene transform applied on top of every root before local transforms
        // (programmatic grouping / demo rotation; identity by default = no visual change)
        glm::mat4 scene_transform = glm::mat4(1.0f);
        // frustum culling of the main pass (BVH over per-leaf world AABBs vs the camera frustum);
        // enabled by default, disable for verification / debugging
        bool frustum_culling = true;
        // culling caches: the BVH is rebuilt only when the scene changed (bvh_dirty), and the
        // culled result is reused while neither the scene nor the camera moved. cull_bvh holds
        // the last built tree (world AABBs are captured at build time and stay valid as long as
        // the scene is unchanged: update_world rewrites the same matrices each frame).
        //
        // Liveness contract (why the raw primitive const* inside the BVH / cull_visible can
        // never dangle): every scene mutation that removes or adds leaves -
        // make_primitive / make_instanced_primitive, import, set_scene,
        // and scene_changed() for callers editing get_scene() directly - sets bvh_dirty. The
        // next begin_recording() then (1) recollects frame_leaves_buffer from the CURRENT tree, (2)
        // destroys the old cull_bvh (its stale leaf pointers die with it) and rebuilds from
        // those fresh leaves, and (3) re-runs the cull so cull_visible also drops dead leaves.
        // Between frames the caches are never touched, so a leaf removed mid-frame is safe as
        // long as the removal went through an API that sets bvh_dirty (or the caller invoked
        // scene_changed()). Callers editing the tree behind the runtime's back must call
        // scene_changed() after every structural change or the BVH/cull_visible can outlive a
        // destroyed leaf.
        //
        // Known limitation (documented, accepted): the BVH bounds are the STATIC local AABBs
        // captured at upload/import. Skinned/morphed vertices can move far outside that box
        // (see set_skin_matrices / morph_scratch), so such a primitive may be frustum-culled
        // even while its deformed geometry is on screen. Conservative animation rigs stay
        // inside the authored bounds; culling can also be disabled (frustum_culling = false).
        std::optional<deren::utility::bvh<primitive>> cull_bvh = std::nullopt;
        bool bvh_dirty = true;                                // scene structure/transforms changed -> rebuild
        std::pmr::vector<primitive const*> cull_visible = {}; // last culled result (main-pass set)
        // camera identity for result reuse: yaw, pitch, distance, target.xyz (7 floats)
        std::array<float, 7> camera_key = {};
        bool camera_moved = true; // camera key differs from the last cull frame
        // THE FRAME'S PRIMARY COMMAND BUFFERS, BORROWED FROM THE BACKEND (S2 batch 2). The RAII objects
        // moved into `core`, because the contract's begin_commands() has to hand out the frame's list and
        // the type that owns the device should own them; this runtime still decides the SHAPE (one per
        // frame slot, allocated once at construction) and still begins, ends and submits them. A SPAN and
        // not a vector: keeping the container here would be a second owner of resources this class does
        // not own.
        std::span<vk_command_buffer> command_buffers;
        // per-slot secondary command buffers for pass recording (stage 2/3 of parallel
        // recording):
        //   - shadow: one shadow-pass CB per frame slot (single segment; the depth-only pass
        //     shares one pipeline, so further splitting buys little - stage 2)
        //   - gui: one overlay CB per frame slot (stage 2)
        //   - transparent: one CB per frame slot for the alpha-blended leaves - recorded on the
        //     PRIMARY thread after the opaque segments (transparent leaves are usually few and
        //     order-sensitive, so they do not join the parallel fan-out), executed last before
        //     the gui overlay so blends compose over the opaque depth
        //   - main: SEGMENT secondaries per frame slot (stage 3): the main pass splits its
        //     visible leaves into up-to-SEGMENT contiguous ranges, each recorded on a pool
        //     worker and executed in order. SEGMENT is sized to the task pool (see
        //     record_main_drawcalls), so the GPU can read all secondaries while the slot's
        //     primary executes - they share the primary's lifetime (reused after the slot's
        //     timeline wait, no per-frame allocation, no pool lock).
        // The slot-scoped secondary the primary thread records alone (the alpha-blended pass), plus
        // the per-cascade shadow pairs and the main-pass segments that live elsewhere:
        //  - shadow: one {pool, buffer} PER CASCADE PER SLOT (see shadow_recording), because the
        //    cascades record concurrently on the task pool and a VkCommandPool is not thread safe;
        //  - main: SEGMENT secondaries per frame slot (stage 3), one per task-pool worker, in
        //    main_segments.
        // This enum used to reserve shadow_0..shadow_3 and gui as well; all five had stopped being
        // recorded by the time the shadow pass moved to per-cascade pairs and the overlay started
        // recording inline, so they were five command buffers allocated and freed per slot for
        // nothing. It is one entry now - the secondaries that are really used.
        enum class secondary_pass : std::size_t { transparent = 0,
                                                  count = 1 }; // fixed non-segment slots
        std::vector<std::array<vk_command_buffer, static_cast<std::size_t>(secondary_pass::count)>> secondary_command_buffers;
        // per-slot main-pass parallel segments (stage 3): one {command pool, secondary buffer}
        // PAIR per task-pool worker, in the same style as the vma allocator's command_cache -
        // a VkCommandPool is not thread safe, so every parallel recording thread owns its own
        // pool and the buffer allocated from it, kept together so they can never drift apart.
        // The pair's pool is registered on the core (destroyed by its cleanup AFTER this
        // runtime's RAII vk_command_buffer members free their buffers into those pools). One
        // inner vector per frame slot (the GPU reads the secondaries while the slot's primary
        // executes, so they share the primary's lifetime), index = segment.
        std::vector<std::vector<std::pair<VkCommandPool, vk_command_buffer>>> main_segments;
        // per-frame state shared by the split frame steps (the frame steps call them in order,
        // so an external caller can interleave its own work between the same steps)
        uint32_t current_image_index = 0; // swapchain image acquired by pace_and_acquire()
        float current_aspect = 1.0f;      // swapchain aspect for the frame's UBO + culling
        camera_ubo current_ubo = {};      // camera UBO snapshot written in pace_and_acquire()
        // called frame_leaves_buffer, not frame_leaves: the local view of that name in
        // runtime.frames.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        std::pmr::vector<primitive const*> frame_leaves_buffer = {}; // every scene leaf this frame (shadow + cull input)
        // reused scratch for the shadow-frustum caster fit (see update_shadow_frustum): every
        // scene leaf is tested here, not just the visible ones, so off-screen casters count
        std::pmr::vector<primitive const*> shadow_caster_scratch = {};
        std::pmr::vector<primitive const*> frame_visible = {}; // opaque frustum-visible subset (main pass)
        // transparent (alphaMode BLEND) frustum-visible leaves, sorted FAR -> NEAR from the
        // camera each time the cull re-runs: drawn AFTER the opaque pass (depth-write off), so
        // the blend order is back-to-front. Rebuilt in begin_recording together with the cull.
        std::pmr::vector<primitive const*> frame_transparent = {};
        /**
         * THE OVERLAY LEAVES (`primitive::overlay_kind != 0`), which are the article's two framebuffer
         * multiplies - the eye shadow and the hair shadow.
         *
         * THEY ARE REMOVED FROM `frame_leaves_buffer` AS THEY ARE COLLECTED, so they reach NONE of the lists built
         * from it: not the cull, not `frame_visible`, not `frame_transparent`, and not `shadow_casters`. That is
         * the whole point of the list rather than a convenience - a mask drawn by a shading pass is a shaded
         * quad (see `primitive::overlay_kind` for the measurement), and one drawn by the shadow pass casts a
         * shadow of its own. They are drawn by the character-forward stage's overlay group instead, with the
         * multiply pipeline and depth test, after the toon character they modify.
         */
        std::pmr::vector<primitive const*> frame_overlay = {};
        /**
         * THE OUTLINE LEAVES (`primitive::outline_width > 0`), which are the leaves the article's ① 描边
         * (inverted hull) is drawn from.
         *
         * UNLIKE `frame_overlay` THESE ARE NOT TAKEN OUT OF ANY LIST: an outline is the SAME surface drawn a
         * second time, so its leaf must stay in `frame_visible` (the toon stage re-shades it) AND in
         * `shadow_casters` (its surface still casts the shadow it always did) - only the hull is extra. What
         * this list adds is the per-MATERIAL gate the article has and the mesh does not: a material whose
         * `_OutlineWidth` is 0 gets no hull, and it is a material fact rather than a mesh one (chen's
         * `cloth_02` is 0.0 in the game's own table while its neighbours are 0.6).
         *
         * IT IS BUILT FROM `frame_visible` rather than from `frame_leaves_buffer`, and that is a correctness
         * requirement rather than an optimisation: a hull is confined to the outside of its silhouette BY THE
         * DEPTH TEST against the surface the pass just re-shaded (see `character_forward_frame::outline_leaves`),
         * so a leaf the cull dropped would leave its hull with nothing to be occluded by - a whole unlit shell
         * painted over the frame instead of a ring. Culling a leaf must therefore cull its outline with it.
         */
        std::pmr::vector<primitive const*> frame_outline = {};
        /**
         * THE TOON CHARACTER STAGE'S LEAVES: `frame_visible` MINUS THE FRAME'S STATIC SURROUND
         * (`primitive::environment`, set by `import_scene(..., environment = true)` for the application's
         * second import of `[render] background_glb`).
         *
         * WHY IT IS A LIST OF ITS OWN RATHER THAN A FILTER AT THE CALL SITE: the stage's frame hands the pass
         * a `std::span` over a container that outlives the call (`character_forward_frame::leaves`), so the
         * filtered set has to live somewhere for the frame being recorded - this member is that somewhere,
         * rebuilt with the other per-frame lists.
         *
         * WHY THE SURROUND IS TAKEN OUT AT ALL: the toon chain is the CARTOON AUTHOR'S chain, written against
         * one character's own material data (a family, a ramp, a sidecar) that the surround does not have -
         * and the reference never gave it one: its ground and backdrop are ordinary lit/emissive surfaces that
         * the reference's own deferred pass draws. This is also why the flag is NOT an "unlit" switch: the
         * scene pass is free to draw the ground with the very same PBR path it draws everything else with.
         */
        std::pmr::vector<primitive const*> frame_character = {};
        // shadow-pass subset (rebuilt each frame before the shadow recording). SMALL scenes
        // (<= full_scene_shadow_leaf_limit leaves, begin_recording): EVERY leaf - exact and cheap
        // at that size. HEAVY scenes: the camera-visible leaves plus the leaves the BVH reports
        // inside the SHADOW frustum itself. That frustum follows the camera and already contains
        // every caster whose shadow can land in view (update_shadow_frustum merges the per-leaf
        // boxes whose light-space xy overlaps the camera frustum's, which is exact under the
        // orthographic light), so - unlike the old camera-frustum-shifted-by-a-margin heuristic -
        // an off-screen caster such as the wall behind the camera is still included. Instanced /
        // bound-less leaves are always included.
        // ... AND THE STATIC SURROUND IS NEVER ONE OF THEM (`primitive::environment`): a 34 m backdrop dome
        // drawn as a caster would put the character inside a shadow it casts on itself, and the ground disc
        // has nothing to cast onto that the frame can see. It is filtered where the list is built.
        std::pmr::vector<primitive const*> shadow_casters = {};
        // how far up-light of the camera frustum a caster still matters (its shadow can still
        // reach the view). Scene-scale heuristic: max(1, scene_radius / 8); see enable_shadows.
        float shadow_caster_extent = 1.0f;

        // ---- ray-traced shadows (see [render] rt_shadows / set_rt_shadows) ----
        // The flag is the CONFIG's wish; whether anything can be built from it also depends on the
        // device (core::ray_query_available), and the two are kept apart on purpose: a device without
        // ray queries must run the cascaded maps exactly as before, silently, rather than fail or log
        // once per frame.
        bool rt_shadows = false;

        // ---- mesh shaders (see docs/mesh_shaders.md) ----
        /**
         * @brief whether a pass may build and use a MESH pipeline on this device
         *
         * THE THREE CONDITIONS, all measured, and they are deliberately kept in one place because each one is a
         * different kind of fact:
         *  1. VK_EXT_mesh_shader AND its `meshShader` feature are enabled (`core.mesh_shader_available`) - without
         *     them `vkCreateShaderModule` refuses the module, and validation says so as an ERROR.
         *  2. the device's push-constant budget covers `mesh_stage_block_size`: a mesh stage must be handed the
         *     draw's whole geometry window as data (there is no input assembler to take it), and that block is
         *     larger than the 128 bytes the spec guarantees - so the path is available where the device reports
         *     room for it and the VERTEX path remains where it does not. The number is logged either way.
         *  3. the heap's push-data window covers it too, which is the limit `vkCmdPushDataEXT` actually obeys.
         */
        bool mesh_shaders = false;
        /// the reason `mesh_shaders` is false, for the one startup log line (empty when it is true)
        std::string mesh_shaders_unavailable_reason = {};
        /**
         * @brief THE GATE ITSELF: whether a mesh pipeline may be built on this device, and why not when it may not
         * @param reason filled with the failing condition when the answer is false and a string was passed (null to
         *        skip it - the question is asked twice, and only one caller logs)
         * @note a pure question about the device, so it can be asked before `create_passes` has run: the G-buffer
         *       pipeline is built by the APP's setup path, which is earlier than the pass context.
         */
        [[nodiscard]] bool evaluate_mesh_shaders(std::string* reason) const noexcept;
        /// `vkCmdDrawMeshTasksEXT`, resolved once from the device because the loader's import library does not
        /// export an extension command (null on a device without the mesh shader extension)
        PFN_vkCmdDrawMeshTasksEXT mesh_dispatch = nullptr;
        /**
         * @brief the meshlet split's totals, for the one startup log line (docs/mesh_shaders.md step 3)
         *
         * The split itself is `deren.vulkan.meshlet`'s and is asserted by tests/test_meshlet.cpp; what these two count is
         * how much of THIS SCENE it produced, which is the number that says whether a task stage reading a meshlet
         * table is worth the table (a scene of one meshlet per primitive would not be).
         */
        std::size_t meshlet_total = 0;
        std::size_t meshlet_primitives = 0;
        /// THE TABLE ITSELF: `meshlet_capacity` records of `sizeof(deren::vulkan::meshlet)`, host-visible, appended to
        /// while the scene imports and read by a task stage through the heap (one slot, written once - see
        /// core::heap_slots::meshlets). `meshlet_total` IS the append cursor.
        rhi::object_manager<rhi::buffer> meshlet_buffer = {};
        void* meshlet_mapped = nullptr;
        /// one log line for the overflow path, so a scene past the capacity says so once rather than per primitive
        bool meshlet_overflow_logged = false;
        /// one log line for a malformed record (see create_primitive): the second line of defence behind the splitter's own tests
        bool meshlet_records_unsound_logged = false;
        /**
         * @brief THE INDIRECT MESH COMMANDS (docs/mesh_shaders.md step 3, the second mechanism), and why the slot is
         *        the primitive's `meshlet_base` rather than a per-frame cursor
         *
         * `vkCmdDrawMeshTasksIndirectEXT` reads `{groupCountX, groupCountY, groupCountZ}` out of a buffer, which is
         * the seam a COMPUTE culling pass needs: culling decides how many of a primitive's meshlets survive, and
         * that number is only known on the GPU. This commit builds the SEAM - the counts written here are the ones
         * the direct call would have carried, so the frame must not move - and the pass that writes them comes next.
         *
         * THE SLOT IS THE PRIMITIVE'S OWN, and that is the whole design. The first attempt handed slots out from an
         * atomic cursor reset per frame in flight, with a fixed capacity and a fall-back to the direct call on
         * overflow; every scenario stayed byte-identical, but `sponza` came out FLAKY (two runs, two hashes), i.e.
         * a command the GPU read was not the command the host meant - a cursor makes WHEN a slot is rewritten a
         * property of the frame's dispatch COUNT, and that count is not the same on every run. Here a slot belongs
         * to one primitive forever (records are appended once, at import, so `meshlet_base` is stable and unique per
         * primitive), so a dispatch rewrites the same slot with the same counts - and a frame's region is a frame
         * in flight's, which the frame-slot wait already guarantees the GPU has finished with. There is no cursor,
         * no capacity left to exhaust, and nothing to fall back for.
         *
         * IT IS COMMAND DATA, NOT A RESOURCE: it is not on the heap, no shader reads it, and the buffer exists only
         * because the command's three arguments have to live somewhere the GPU can read. Host-visible and mapped,
         * with INDIRECT usage, `MAX_FRAMES_IN_FLIGHT` regions of `meshlet_capacity` records.
         */
        rhi::object_manager<rhi::buffer> mesh_indirect_buffer = {};
        void* mesh_indirect_mapped = nullptr;
        /// the raw `VkBuffer` the command takes: `vk_buffer::handle()` is the allocator's id, not a `VkBuffer`, and
        /// resolving the entry point without binding the buffer is what silently sent every dispatch down the
        /// DIRECT path in the first version of this seam - which is why the route is logged (see the counts below)
        VkBuffer mesh_indirect_table = VK_NULL_HANDLE;
        /// one line for the route actually taken, and one for the first slot that two draws disagree about: a
        /// conflict is a DESIGN violation (the same primitive dispatched with two different counts), and it falls
        /// back to the direct call for that dispatch rather than risking a command the GPU reads half-written
        bool mesh_indirect_route_logged = false;
        bool mesh_indirect_conflict_logged = false;
        /// how many dispatches went each way, for the one line the first frame prints (and for the acceptance:
        /// "0 direct" is what says the seam is the one in use rather than a silent fall-back)
        std::atomic<uint64_t> mesh_indirect_dispatches = 0;
        std::atomic<uint64_t> mesh_indirect_direct_fallbacks = 0;
        /// HOW MANY COMMANDS one frame's lane holds (docs/mesh_shaders.md step 3): TWO runs of `meshlet_capacity`, one
        /// per COMMAND CLASS, because a HOST-CULLED run and an unculled one (the shadow pass) dispatch the same
        /// primitive with DIFFERENT counts - and a shared slot would hand both passes whichever wrote last.
        static constexpr uint32_t mesh_command_capacity = 2u * deren::vulkan::meshlet_capacity;
        /**
         * @brief THE MESH CULLING COUNTERS (docs/mesh_shaders.md step 3, "what the culling buys"): a mesh entry adds
         *        to one per workgroup, the host reads the buffer back once at shutdown and logs the totals
         *
         * `MAX_FRAMES_IN_FLIGHT` lanes of `mesh_stat_stride` (8) uints, on the heap at
         * `core::heap_slots::meshlet_stats` - the only way a mesh stage can reach memory. Host-visible and coherent
         * because the read-back is a plain mapped read after `wait_idle`, and the counters are cumulative for the
         * whole session: the numbers the doc quotes are totals over a fixed scenario, which is what makes them
         * comparable between runs (the gate's scenarios are 40 frames each).
         *
         * WHAT IT ANSWERS: how many meshlets the frustum test rejected. That number is BOTH the culling's effect on
         * the mesh entry AND - because a rejected meshlet still costs a workgroup launch - the number of dispatches a
         * compute pass that culled before the dispatch would not have recorded at all.
         */
        rhi::object_manager<rhi::buffer> meshlet_stats_buffer = {};
        void* meshlet_stats_mapped = nullptr;
        /// the one line per session, printed from the destructor (after `wait_idle`, so the counters are final)
        void log_meshlet_stats() const;
        /**
         * @brief THE HOST-CULLED MESHLET TABLE (docs/mesh_shaders.md step 3, the culling's cheapest stage): a per-frame
         *        lane of `meshlet_capacity` records, written while a CULLED session's draws are recorded and read by
         *        that session's mesh entry
         *
         * The host already holds the three things the stage's frustum test needs - the primitive's meshlets, the
         * draw's model matrix and the camera - so the cull costs no pass, no command buffer and no draw list, which is
         * why it is the stage that shipped. What it buys is measured: the same pixels with the culled workgroups never
         * launched (see the counters above, and the table in the doc).
         */
        rhi::object_manager<rhi::buffer> meshlet_culled_buffer = {};
        void* meshlet_culled_mapped = nullptr;
        /// the two endpoints the recording path calls (see render_environment::meshlet_view_proj / _write)
        static bool meshlet_view_proj(void* owner, float* out16);
        static bool meshlet_culled_write(void* owner, uint32_t base, std::span<std::byte const> records);
        /**
         * THE STRUCTURE PHASE ITSELF, which is one value now (see `deren.vulkan.ray_tracing`): the bottom and top level
         * structures, the map from their indices back to the casters they were built from, and the MASK/skin
         * copies a hit's shading reads that geometry through. Built once (lazily, on the first frame the flag is
         * on, because the caster set is only known once the scene has been culled), refitted and RE-INSTANCED
         * every frame, from the casters this frame's culling produced and the two jobs below.
         *
         * WHY IT IS THIS RENDERER'S AND NOT A PASS'S: the ray-traced shadow pass's binding is what reads it, and
         * the structures are one build per frame feeding every ray-traced effect, so by the
         * ownership rule it belongs to the shared owner - and the phase records BEFORE any rendering instance
         * opens. What stays here beside it is the POLICY: the three knobs, the two predicates
         * (`rt_structures_wanted` / `rt_shadows_active`), the caster set, the ORDER of the phase in the frame, and
         * the scene-set binding the handle is published through.
         *
         * INITIALIZED HERE from the device root this class already holds, and that is not a style choice: the
         * constructor's init list has to follow DECLARATION order, and this member is declared before
         * `pass_resources` - so an entry there would be a reorder warning (`-Werror`) for a dependency that is
         * real but does not need the list to express it (`vulkan_core` is declared above everything).
         */
        ray_tracing::structure_set structures{this->vulkan_core};
        // The alphaMode MASK bake (see shaders/mask_bake.slang): ONE JOB OBJECT owns its pipeline
        // (vulkan.pass.mask_bake_job), because it is not a frame pass at all -
        // it runs once, inside the same command buffer as the bottom level builds it feeds, and its input is
        // the caster list this renderer is walking at that moment. It is OFF by default and the shipped shadow
        // does not need it: the ray-tracing pipeline's any-hit stage cuts a MASK surface per hit
        // (shaders/rt_shadow.rahit). Absent on a device without ray queries, where masked geometry is solid to a
        // ray - but so is everything else, since nothing traces there at all.
        //
        // The bake reads the material table and the bindless texture array from the HEAP (the slots the shader
        // names itself), which is why it owns no set of its own: written where the material table and the texture
        // array are, and read by the dispatch without a bind.
        // THE TWO JOBS ARE THE EXCEPTION THIS FILE STILL HAS, and the reason is a TYPE, not a policy: neither is
        // a `frame_pass` (see their headers - one runs once inside the structure-build command buffer, the other
        // per frame from a caster list), so a chain cannot hold them. Everything else follows the same rule they
        // do: they are built from the pass context, they own their pipelines and sets, and they release them in
        // their own destructors. THEY ARE ORDINARY MEMBERS rather than objects a chain keeps alive, and what
        // decided that is the reference count rather than a preference: with `pass_chain::keep` gone these two are
        // the only non-pass GPU-owning objects in the renderer, so one member each is less machinery than a
        // container that exists to hold exactly two objects of two known types. The DECLARATION ORDER is what keeps
        // the device alive underneath them: `core_owner` is declared at the top of this class and is therefore
        // destroyed LAST, so both jobs release their pipelines while the device still exists - the same order the
        // chain's `kept_alive_` used to give them. A `job_chain` is the shape to reach for if a THIRD job appears.
        pass::mask_bake_job mask_bake = {};
        // Whether that bake runs at all ([render] rt_mask_bake). Off by default: the per-triangle rule
        bool rt_mask_bake = false;

        // ---- skinned meshes (see shaders/compute_skin.slang) ----
        // This engine skins in the VERTEX shader, so the deformed positions never reach memory a build can
        // read, and a skinned mesh's traced shadow is its BIND POSE (measured: the traced shadow's pose
        // section). The pass below writes the same vertices the vertex shader computes into the buffer the
        // structure is built from, once per frame, and the structure is REFITTED rather than rebuilt
        // because only the bytes change.
        // The job owns its pipeline (vulkan.pass.compute_skin_job);
        // what stays here is the POLICY - the knob, the skinned caster list, the buffers each caster is skinned
        // into, and the refit bookkeeping - plus the request list it hands over, kept as a member so a frame
        // does not allocate while recording. A member for the same measured reason as the MASK bake above.
        pass::compute_skin_job compute_skin = {};
        std::vector<pass::compute_skin_request> compute_skin_requests = {};
        // Whether the skinning pass runs ([render] rt_skin_bake): a knob because it is the A/B that measures
        // whether a traced shadow now follows the pose, and because a device without ray queries has no
        // structures for it to feed.
        bool rt_skin_bake = false;
        // The ray-traced sun shadow pass (see the shaders/rt_shadow.* pipeline stages): its pipeline,
        // push block's shape and its one-shot log line are the PASS's now (deren.vulkan.pass.ray_traced_shadow), and its
        // member and stage are declared next to the other passes above. The renderer keeps two facts about it:
        // WHERE it sits (after the G-buffer pass, before the lighting stage - see the frame loop) and the
        // transition the lighting stage's heap slot needs on a frame where the pass does not run.
        /**
         * @brief WHAT THE STRUCTURE PHASE NEEDS FROM THIS RENDERER, as one value (see ray_tracing::build_inputs)
         *
         * The casters the culling produced, the material table the alphaMode MASK rule reads, the two knobs, and
         * the two JOBS - which stay this class's members (they are GPU-owning objects built from a pass context)
         * and are driven through `ray_tracing::bake_hooks`: the phase's module drives them without depending on
         * the passes that implement them, and the slice that moves the jobs in changes one side only.
         */
        [[nodiscard]] ray_tracing::build_inputs make_structure_inputs() const noexcept;
        /// @brief the four hooks above, as the function pointers the phase takes (each one casts `owner` back)
        static bool structure_mask_ready(void* owner) noexcept;
        static void structure_record_mask_bake(void* owner, VkCommandBuffer command_buffer, pass::mask_bake_request const& request);

        /**
         * @brief the host's push endpoint for a converted stage (see pass::resolved_io::push_block)
         * @return whether the block was sent
         * @note it is a static member rather than a free helper for the ordinary reason: it needs the device, and
         *       the heap, and the runtime owns both. The bytes are the stage's own push block, whose last fields
         *       are the two heap indices the renderer fills in before calling this.
         */
        static bool push_stage_block(void* owner, VkCommandBuffer command_buffer, std::span<std::byte const> bytes, uint32_t extra_lane);
        /**
         * @brief the same append, WITHOUT the post chain's third lane
         * @note the compute skin stage declares the two heap indices and nothing else, and pushing lanes a shader
         *       has not declared is not something to guess at - so each stage's endpoint appends what it declares.
         */
        static bool push_index_block(void* owner, VkCommandBuffer command_buffer, std::span<std::byte const> bytes, uint32_t extra_lane);
        /// @brief a block pushed VERBATIM, with no index lanes: the mask bake declares none (it addresses its
        ///        sources and its destination through device addresses and reads one material table)
        static bool push_raw_block(void* owner, VkCommandBuffer command_buffer, std::span<std::byte const> bytes);
        /**
         * @brief the MESH session's three endpoints (docs/mesh_shaders.md step 1), each the runtime's because it
         *        owns the device: the device address of a bound buffer, a push at a RAW offset (the geometry lanes
         *        sit past the block `push_stage_block` sends, which appends the heap indices at its own end), and
         *        the dispatch itself.
         * @note `draw_mesh_tasks` answers false when the device has no `vkCmdDrawMeshTasksEXT` at all: the command
         *       is an EXTENSION command that the loader's import library does not export, so it is resolved
         *       through `vkGetDeviceProcAddr` once and the answer is cached (see `mesh_dispatch`).
         *
         * IT TAKES A CONTRACT BUFFER because that is what the caller's lanes hold (the primitive's geometry is
         * `object_manager` members now), which is why the hook in `render_environment` narrowed to this shape.
         */
        static VkDeviceAddress mesh_buffer_address(void* owner, rhi::buffer const& buffer);
        static bool push_geometry_block(void* owner, VkCommandBuffer command_buffer, uint32_t offset, std::span<std::byte const> bytes);
        static bool draw_mesh_tasks(void* owner, VkCommandBuffer command_buffer, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);
        /// the same dispatch through the INDIRECT entry point: `command_slot` names the primitive's own record (its
        /// `meshlet_base`), and the counts are written there before the call - so a compute pass can later rewrite
        /// that record with the counts culling left, without the recording path changing at all
        static bool draw_mesh_tasks_indirect(void* owner, VkCommandBuffer command_buffer, uint32_t command_slot, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);
        /**
         * @brief hand a SECONDARY the two heap bind infos it must inherit (see scene_frame::fill_heap_bind)
         * @note the caller owns the storage, because VkCommandBufferInheritanceDescriptorHeapInfoEXT points at the
         *       infos rather than copying them, and they have to outlive vkBeginCommandBuffer.
         */
        static void fill_heap_bind(void* owner, VkBindHeapInfoEXT& resource, VkBindHeapInfoEXT& sampler);
        static bool structure_skin_ready(void* owner) noexcept;
        static bool structure_record_skin(void* owner, VkCommandBuffer command_buffer, std::span<ray_tracing::caster_level const> casters);
        /** @brief write the structure's and its instance table's heap slots for @p frame_slot */
        void write_rt_structure_binding(VkAccelerationStructureKHR tlas, uint32_t frame_slot);
        /**
         * @brief THE NATIVE HANDLE BEHIND A CONTRACT BUFFER, for the places a raw `VkBuffer` is still what
         *        the call takes (a heap descriptor written by hand, `vkCmdBindVertexBuffers`, the indirect
         *        dispatch's own lookup).
         *
         * THE ESCAPE IS THE CONTRACT'S OWN ANSWER TO THIS QUESTION (`vulkan_escape::native_buffer`, and its
         * note says why it exists), so the temporary `VkBuffer` this hands back is BORROWED exactly as that
         * call states: valid while `buffer` holds its reference, which every caller here does - a member or
         * a vector element of this class, alive for the session.
         */
        [[nodiscard]] VkBuffer buffer_of(rhi::buffer const& buffer) noexcept;
        /**
         * @brief the DEVICE ADDRESS of a contract buffer, through the `device_address` ability.
         * @return 0 when the backend did not announce the ability, or when the buffer was created without
         *         `buffer_flag::device_address` - the ability's own two answers, which every call site here
         *         asks for a buffer it created WITH that flag.
         */
        [[nodiscard]] VkDeviceAddress buffer_address(rhi::buffer const& buffer) const noexcept;
        /**
         * @brief write ONE converted buffer's address range into the descriptor heap's grid slot @p slot.
         * @return whether the heap took the write; a false is the caller's to log or to panic on, exactly as
         *         the per-site `descriptor_heaps.write_buffer` call it replaces was.
         */
        [[nodiscard]] bool write_heap_buffer(rhi::buffer const& buffer, uint32_t slot, VkDeviceSize size, VkDescriptorType type) const;
        // scene center handed to enable_shadows. The fit falls back to center +- scene_radius when a
        // shadow caster has no world AABB of its own AND is not an instanced draw whose instance
        // matrices we can read (see instanced_world_aabb).
        glm::vec3 shadow_scene_center = glm::vec3(0.0f);
        // Fit cache: rebuilding the light frustum walks every leaf and transforms 8 corners each,
        // which is O(scene) work for a result that only changes when the camera or the scene moves.
        // shadow_fit_view_proj is the camera matrix the current frustum was fitted for;
        // shadow_frustum_valid is cleared by enable_shadows() (new light setup) and bvh_dirty marks
        // a changed scene.
        //
        // Only the CACHE stays here. The fit itself (splits, per-cascade boxes, texel snapping, the
        // orthographic matrices) is deren.vulkan.shadow_fit, which needs nothing but glm - and the
        // gathering below stays here because it needs scene_tree and primitive.
        glm::mat4 shadow_fit_view_proj = glm::mat4(1.0f); // unjittered view * proj the last fit used
        // per-frame scratch for the gather: each caster's light-space AABB, then the world boxes it
        // came from. Reused (capacity kept) so a refit allocates nothing on a steady scene.
        std::vector<std::pair<glm::vec3, glm::vec3>> shadow_caster_boxes = {};
        std::vector<std::pair<glm::vec3, glm::vec3>> shadow_caster_world_boxes = {};

        bool shadow_frustum_valid = false;
        // optional Dear ImGui debug overlay; inactive until enable_debug_gui() succeeds. The
        // runtime drives it inside the frame steps (new_frame before recording, record after the
        // runtime's own draw calls) so callers only manage its content via debug_gui().
        gui::gui_content debug_overlay;
        // whether the active overlay is drawn (the built-in F1 toggle flips it);
        // hiding keeps the overlay initialized and its panels intact, so showing is instant
        bool debug_gui_shown = true;
        // F1 edge detection for the overlay toggle (true while the key is held, so one press
        // toggles exactly once - see poll_events)
        bool gui_toggle_down = false;
        // filtered view over vulkan_core, exposed via operator-> (external code never sees the raw core)
        user_filter filtered_core;
        /**
         * THE PASS FILTER (deren.vulkan.core.filters): the view a pass's create step is handed, which is how the
         * renderer stops knowing what each pass needs. It shares the device (`core_owner`) and carries the
         * resources this runtime publishes for its passes to name - the material table, the bindless texture
         * array, the per-slot skin matrices - instead of the renderer doing it through a bespoke entry point per
         * pass. Declared here,
         * after `core_owner`, so it is released before the device.
         */
        pass_filter pass_resources;

        /**
         * THIS FRAME's shared constants, and THIS FRAME's resources in the declaration's own vocabulary.
         *
         * The first is handed to every pass through `resolved_io::constants`; the second is what the framework
         * will resolve a declaration against once the per-pass resolvers in this class are gone (see
         * `publish_frame_resources`). Both are per-FRAME state - the table is refreshed every frame rather than
         * every generation, because one of its families is an alias decided per frame - which is why they sit
         * next to the frame's other state rather than next to the create-time publication above.
         */
        frame_constants frame_facts = {};
        pass::resource_table frame_resources = {};
        /**
         * The differential check's running totals, and the passes it has already named.
         *
         * THE WINDOW IS THE FIRST FEW FRAMES, not just the first: a pass can legitimately resolve later than
         * frame 0 (TAA needs a history, a feature can be gated on the previous frame's result), and a check that
         * only watched the first frame would silently report "not covered" for exactly those. The pass list is
         * what keeps the coverage lines to one per pass rather than one per pass per frame.
         */
        uint32_t resource_check_checked = 0;
        uint32_t resource_check_mismatched = 0;
        uint32_t resource_check_frames = 0;
        bool resource_check_reported = false;
        std::vector<pass::frame_pass const*> resource_check_passes = {};

        /**
         * @ingroup vulkan_runtime
         * @brief collect every leaf primitive under @p node (DFS pre-order) into @p out
         * @note leaves are stored as scene_tree::primitive; every leaf this runtime creates is a
         *       deren::vulkan::primitive (the GPU primitive implements scene_tree::primitive), so the cast is safe
         */
        static void collect_leaf_primitives(scene_tree::scene_node const& node, std::pmr::vector<primitive const*>& out);

        /**
         * @ingroup vulkan_runtime
         * @brief write every tracked leaf's PREVIOUS world matrix into this frame slot's
         *        previous-transform buffer (scene block slot 13), then refresh the stored copies to
         *        the matrices the frame about to be recorded will draw with
         *
         * Runs once per frame from begin_recording(), straight after the scene tree's world matrices
         * are recomputed - and that position is the whole reason it exists. The GPU needs the two
         * matrices side by side to build a motion vector, and the only per-draw channel that could
         * carry the previous one is a push constant, which is full (a second mat4 would need 64 of
         * the 128 guaranteed bytes, and the block is already 96). A buffer written at DRAW time is
         * not an option either: the main pass records segments on pool workers in parallel, and a
         * shared mapped buffer written from several threads is a race. Writing it here is a single
         * thread, before any recording, exactly once per leaf per frame.
         * @note Frame slot, not frame: each slot has its own buffer and its own scene set, so a
         *       frame in flight never shares the buffer the next frame rewrites (the same rule the
         *       skin and morph buffers follow).
         * @note A leaf that was culled last frame keeps whatever matrix it had then, so the first
         *       frame it comes back its motion vector is stale. TAA's neighborhood clamp rejects
         *       that, which is why this is not worth a second walk over the whole tree.
         */
        void advance_motion_transforms();
        /**
         * @ingroup vulkan_runtime
         * @brief publish the joint matrices ONE FRAME AGO into the current frame slot's previous-skin buffer,
         *        then remember this frame's - the deformation half of every skinned vertex's motion vector
         *
         * WHY IT IS SEPARATE FROM advance_motion_transforms(). That function walks the scene tree, because a
         * world matrix is a per-LEAF fact; a joint matrix is not - the animation controller uploads the whole
         * block through set_skin_matrices() before this runs, so all this has to do is the same
         * publish-then-refresh the motion transforms do, over the whole block instead of per leaf. Keeping
         * them apart is also what keeps the controller out of it: no controller path can desync the two
         * halves, because the ONLY writer here is this function.
         *
         * @note Called from begin_recording(), immediately after advance_motion_transforms() and therefore
         *       after the animation controller's own write window (see animation::controller::update: after
         *       pace_and_acquire(), before begin_recording()) - so what it reads has been written.
         * @note A frame on which the controller does not run leaves the two copies equal, so the deformation
         *       term of the motion vector is exactly zero, which is the correct answer for a pose that did
         *       not change rather than a special case. The same holds after a PINNED animation_time.
         * @note The identity block the controller bakes into every slot at setup (indices 0-3) is also this
         *       buffer's initial content, so a vertex with no history reads the bind pose; the one frame that
         *       could be wrong is a frame TAA gives no history to anyway (history_valid = 0).
         */
        void advance_motion_deformations();
        /**
         * @ingroup vulkan_runtime
         * @brief build a normal_draw_primitive from @p info WITHOUT attaching it to the scene tree:
         *        uploads geometry buffers and registers the material (textures + material_record).
         * @param pipeline_name the pipeline the primitive draws with (must already exist)
         * @return the new primitive (caller attaches it into a scene node), or nullptr if the
         *         pipeline does not exist
         * @note make_primitive() is create_primitive() + attach-as-root-leaf; the hierarchy import
         *       (import_scene) attaches leaves to their node instead
         * @note material registration writes every scene set (binding 1), which is only valid
         *       before the first frame or while the runtime is idle - see make_primitive()'s
         *       timing note
         */
        std::unique_ptr<primitive> create_primitive(std::string_view pipeline_name, primitive_create_info const& info);

        /**
         * @ingroup vulkan_runtime
         * @brief begin the frame's rendering on the given command buffer: clears the color
         *        attachment with a dark background and the depth attachment
         * @param command_buffer the command buffer being recorded
         * @param image_index the acquired swapchain image index (selects the attachment views)
         * @note uses vkCmdBeginRendering (dynamic rendering, Vulkan 1.3 core - the only path
         *       the engine supports; device selection requires an apiVersion >= 1.3 device)
         */
        void begin_rendering(VkCommandBuffer command_buffer, uint32_t image_index, VkRenderingFlags flags = 0) const;

        // ---- scene resource management (see the members above) ----
        void init_scene_resources();     // camera UBO buffers + white fallback texture + texture sampler + material table
        void init_recording_resources(); // primary + secondary command buffers and their pools
        /**
         * @brief reset every per-image flag that describes a swapchain GENERATION
         *
         * A freshly created target image starts in UNDEFINED, holds nothing, and no pass has written
         * it yet - which is the state generation 0 (the constructor, after the core built this
         * generation's targets) and every later generation (on_swapchain_recreated) both need. One
         * function, so the two call sites cannot drift apart.
         *
         * @note deliberately NOT the TAA history matrices (image_view_proj): those may only be written
         *       from a real camera snapshot (current_ubo), which the constructor does not have yet, so
         *       they stay with the two call sites that do
         */
        void reset_image_generation_state();
        void ensure_shadow_resources(); // (lazily) layered shadow map + light UBO buffers
        void ensure_cluster_buffers();  // (lazily) per-slot cluster count/index buffers (M5)
        // Diagnostics for the optional features (see log_feature_status / warn_missing_feature): a
        // toggle whose prerequisites are missing would otherwise do nothing at all - the user clicks,
        // nothing changes, and the log has no trace to explain it. Each message is emitted at most
        // once per session, keyed by the feature name.
        std::vector<std::string> warned_features = {};
        void ensure_scene_heap_slots();                                   // (lazily) create the shadow map and the clustered-light buffers, and write their heap slots
        void write_light_and_shadow_bindings();                           // write the light UBO and the shadow map into each frame slot's heap block
        material_id register_material(primitive_create_info const& info); // upload textures into the array, append a material_record, return its index

    public:
        /**
         * @ingroup vulkan_runtime
         * @brief what the runtime reports back after a stage, for the passes' owner to fill
         *
         * The frame loop's own decisions, read from the passes that answer them: whether the stochastic
         * lighting chain's resolve wrote an accumulation this frame (the next frame's history flag is set from
         * it), and whether the TAA resolve wrote a history (the camera UBO's `prev_view_proj` is only advanced
         * when it did).
         */
        struct frame_results {
            /// the stochastic punctual lighting chain's resolve wrote its accumulation this frame: the next
            /// frame's history flag for THIS image is set from it
            bool megalights_temporal_resolved = false;
            bool taa_wrote_history = false;
        };

        /**
         * @ingroup vulkan_runtime
         * @brief the FACTS a feature answer is composed from: the renderer's half of the registry
         *
         * WHY A VALUE RATHER THAN ACCESSORS: the feature table is a POLICY (what runs this frame) and the facts it is
         * made of are the renderer's (its pipelines, its frame's content, the device, its knobs). So the renderer
         * reports the facts and the CHAIN'S OWNER decides - which is exactly the split every feature answer in this
         * renderer has been expressing by hand ("the knob AND the pass built its pipeline AND the frame has a
         * surface"), now with the two halves on their own sides of the seam.
         *
         * WHAT IS NOT HERE: anything a pass answers about itself (`unlit`, `ssao_enabled`, whether this frame's
         * temporal resolve recorded) - the owner holds those passes and asks them - and the *composed* predicates the
         * built; `taa_active` picks the scene target and jitters the projection), which appear here as atoms so that
         * the two sides cannot implement the same composition twice.
         */
        struct feature_facts {
            /// the runtime's own predicates, each of which the renderer also acts on (see the list above)
            bool gbuffer_pass = false; // gbuffer_pass_active(): the surface pipeline exists and this frame shades
            bool deferred_lit = false; // deferred_lit_active(): the lighting stage is this frame's shading path
            bool megalights = false;   // megalights_active(): the knob, the pass, and the deferred shading path
            bool rt_shadow = false;    // rt_shadows_active(): the knob and the device
            bool fxaa = false;         // post_fxaa_active(): the knob and the FXAA pass
            /**
             * post_upscale_active(): the render chain running below the output size AND the upscale pass having
             * built its pipeline. Reported as an atom for the same reason `fxaa` is: the runtime acts on it (it
             * picks the composite's target and pipeline variant), the frame reports it in `frame_facts` and the
             * chain owner's table relays it, so all three read ONE composition rather than three.
             */
            bool upscale = false;
            /// the knobs the feature table composes with, and the frame's own content
            bool gbuffer_debug = false;       // the debug view's knob
            bool shadow = false;              // the checkbox AND enable_shadows() having succeeded
            bool clustered = false;           // the clustered-lights knob
            bool taa = false;                 // the TAA knob
            bool bloom = false;               // the bloom knob, resolved (intensity > 0)
            bool transparent_pending = false; // this frame has alpha-blended geometry to composite
            /**
             * this frame has OPAQUE geometry for the character-forward stage to re-shade, AND the stage is
             * switched on. The two are one flag rather than two because the stage has nothing to say about a
             * frame with no opaque geometry - and because the pass's own gate must be able to answer "skip"
             * without the runner ever resolving its declaration.
             */
            bool character_forward_pending = false;
            bool gbuffer_pipeline = false; // the surface pipeline exists (the scene pass records with it)
            bool structures_ready = false; // this frame's top level structure is built for the slot
            bool furnace = false;          // the analytic verification mode
            float punctual_lights = 0.0f;  // live punctual lights (the light UBO's own lane)
        };

        /**
         * @ingroup vulkan_runtime
         * @brief THE RUNTIME'S PER-FRAME SERVICES for whoever owns the chain of passes
         *
         * WHY THIS EXISTS: the runtime kept one TYPED member per pass and therefore knew, in its frame loop, which
         * pass wanted which frame - thirteen `set_frame` calls, five stage preambles and nine result reads, every
         * one of them naming a concrete pass. Handing the chain over means the runtime must stop naming passes,
         * and this is the boundary that replaces it: the DATA is the runtime's (the leaves the culling produced,
         * the per-cascade secondaries, the per-image publish flags) and the PASS is the chain owner's, so the
         * runtime builds the frame and the owner hands it to the pass it belongs to.
         *
         * A VALUE, rebuilt per stage call: every field is a function pointer plus the one `owner` they all take,
         * so building it costs a handful of stores. It is passed BY VALUE to `chain_wiring::prepare`, which means a
         * mis-wired stage is a null pointer the owner can see rather than a member it should not have touched.
         */
        struct frame_services {
            void* owner = nullptr;                // this runtime: what every function pointer below casts back to
            VkCommandBuffer cmd = VK_NULL_HANDLE; // the command buffer this frame records into
            /// which swapchain image this frame is recording (the per-image rules below take it, and a stage
            /// preamble that is gated on an image's own state needs it)
            uint32_t image_index = 0;
            // ---- the frame's TOOLKIT: the device-level facts a family, a push block or a barrier needs, so that
            //      an owner can do what the renderer's own resolvers used to do without reaching into it ----
            VkDevice device = VK_NULL_HANDLE;
            /// the six shared samplers a declaration chooses between by hint (see render_resource::shared)
            render_resource::shared::sampler_set samplers = {};
            /// THIS FRAME'S RESOURCES, in the declaration's own vocabulary: what a pass's declaration names, and
            /// the handles behind it - the channel that lets an owner read any PUBLISHED family's views (its own
            /// sets, or a second family over the same layout) without the renderer handing over its image arrays
            pass::resource_table const* table = nullptr;
            /// the frame's identity (image index, slot, count, extent), for a recording that sizes its own work
            pass::frame_identity frame = {};
            /// the frame's constants, for a push block an owner composes itself
            frame_constants const* constants = nullptr;
            // ---- the frames ONLY the runtime can build, because they carry its own recording machinery (the
            //      per-slot secondary buffers, the per-cascade secondaries, the draw-state factory and the task
            //      scheduling those run through). Every other frame is the PASS's own now: see
            //      `frame_pass::prepare_frame`, which the host calls for every pass in a stage before this ----
            pass::shadow_frame (*make_shadow_frame)(void* owner) = nullptr;
            pass::scene_frame (*make_scene_frame)(void* owner) = nullptr;
            pass::transparent_frame (*make_transparent_frame)(void* owner) = nullptr;
            /// the character-forward frame: the OPAQUE leaves again, plus the pipeline name the session binds.
            /// Unlike the two above it carries no recording machinery of its own - the pass draws directly into
            /// the primary - so it is here because the LEAF LIST is the runtime's, not because of a secondary.
            pass::character_forward_frame (*make_character_forward_frame)(void* owner) = nullptr;
            // ---- the frame's ORDERING RULES a stage preamble runs: the runtime's per-image bookkeeping, whose
            //      flags belong to the passes that WROTE those images (see each accessor) ----
            bool (*ensure_gbuffer_targets_sampled)(void* owner, VkCommandBuffer cmd, uint32_t image_index) = nullptr;
            bool (*ensure_gbuffer_depth_sampled)(void* owner, VkCommandBuffer cmd, uint32_t image_index) = nullptr;
            bool (*ensure_velocity_sampled)(void* owner, VkCommandBuffer cmd, uint32_t image_index) = nullptr;
            /// clear that flag WITHOUT recording a transition: what a stage whose own pass samples the image
            /// does when the resolve that would have published it runs LATER in the frame (the TAA resolve and
            /// the debug view both leave a later pass to publish the motion-vector target)
            void (*require_velocity_publish)(void* owner, uint32_t image_index) = nullptr;
            /// the renderer's feature registry, for a stage preamble gated on the same predicate the runner gates
            /// the stage on (the ray-traced shadow's and the TAA resolve's preambles are the two that ask)
            bool (*feature_active)(void* owner, std::string_view name) = nullptr;
        };

        /**
         * @ingroup vulkan_runtime
         * @brief what the runtime needs from whoever owns this frame's concrete passes
         *
         * `prepare` runs immediately before a stage records, and it is where the owner gives its passes their
         * frames and runs the frame's ordering rules for that stage (see `frame_services`). `collect` runs after
         * a stage, and it is where the owner reports the results the frame loop decides on (see `frame_results`).
         * The NAME is the stage's, which is the frame's own structure - the owner switches on it and the runtime
         * never learns which pass is behind it.
         */
        struct chain_wiring {
            void* owner = nullptr;
            void (*prepare)(void* owner, frame_services const& services, std::string_view stage) = nullptr;
            void (*collect)(void* owner, std::string_view stage, frame_results& out) = nullptr;
            /**
             * The FEATURE TABLE, in the declaration's vocabulary: what the runner gates each pass on, what the
             * overlay offers and what the startup log reports (see `feature_facts` for the renderer's half).
             */
            bool (*feature_active)(void* owner, feature_facts const& facts, std::string_view name) = nullptr;
            /// ... and the different question "could this feature ever run this SESSION", which the overlay's menu
            /// and `log_feature_status()` ask (see the note on the runtime's forwarding answer)
            bool (*feature_available)(void* owner, feature_facts const& facts, std::string_view name) = nullptr;
            /**
             * The swapchain was rebuilt, so every per-generation object the owner holds is stale - the same call the
             * runner makes for a pass, for the GPU-owning things the owner keeps OUTSIDE the chain (this
             * application's reflection descriptor family is the one today).
             */
            void (*recreated)(void* owner) = nullptr;
        };

        /**
         * @ingroup vulkan_runtime
         * @brief hand this runtime the owner of its passes' frames
         * @param wiring the two callbacks and their context; an empty one gives every pass an empty frame, which
         *        is a frame that records nothing (see `chain_wiring`)
         * @note the owner must outlive this runtime's recording, which is the same contract the chain itself has
         */
        void set_chain_wiring(chain_wiring wiring) noexcept;
        /// fill the frame loop's stage arrays and its two GI halves from a chain, BY DECLARATION NAME (see
        /// `set_pass_chain`): the frame structure is this renderer's, the passes are the owner's
        void bind_frame_chain(pass::pass_chain& chain) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief publish the ONE frame constant a chain owner produces mid-chain
         *
         * INSIDE the temporal pass's recording - and read by the spatial filter later in the same chain, so it
         * cannot travel through `collect` (that runs after the whole chain). The owner is the only one that knows the
         * answer, so this is the narrow channel it writes it through rather than handing over the whole constants
         * value to scribble on.
         */

        /**
         * @ingroup vulkan_runtime
         * @brief publish the LIGHTING STAGE's flat-render state, which the renderer's own policy reads
         *
         * The flag itself is the lighting pass's parameter (the shader returns the stored albedo), and the renderer
         * needs it for one thing it decides on its own: whether a screen-space effect that reads the G-buffer is
         * worth running at all (the flat mode's image is stored albedos rather than a transport term). Since the
         * pass left the renderer, the owner that holds it publishes
         * a policy predicate without keeping a pass.
         */
        void set_scene_unlit(bool unlit) noexcept {
            this->scene_unlit = unlit;
        }
        /**
         * @ingroup vulkan_runtime
         * @brief report a feature that cannot do what it was asked, at most once per session
         *
         * PUBLIC because the chain owner is the one that now knows both halves of the question: the renderer
         * answers "this feature cannot run" (`feature_active` / `feature_available`) and the owner is what asked
         * the feature to do something. The deduplication and the "not before the startup is complete" rule are
         * the runtime's, so the log stays one line per feature whatever calls it.
         */
        void warn_missing_feature(std::string_view key, std::string const& message);

        /**
         * @ingroup vulkan_runtime
         * @brief the host hook the frame's LAST WRITER draws the debug overlay with (see `pass::draw_callback`)
         *
         * WHY IT IS PUBLIC, and it is the same shape as `warn_missing_feature`: the overlay is this renderer's
         * (it owns the ImGui renderer and the flag that turns it off), but WHICH pass records it is the chain
         * owner's to know - the overlay has no load op, so it has to be recorded inside whichever pass writes the
         * frame last, and that is a property of the chain. The owner installs this hook on the passes that may be
         * last (`deren.vulkan.render_start_demo` does it in `attach`), and each pass decides for itself whether it is.
         */
        [[nodiscard]] pass::draw_callback overlay_draw() const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief HAND THIS RENDERER THE CHAIN OF PASSES IT RECORDS
         *
         * THE HANDOVER ITSELF, and it is one call because everything else moved first: the per-pass frames, the stage
         * preambles, the result collection and the feature table are the owner's since the slices this document
         * records. What the runtime keeps is the RECORD LOOP - the stage sequence, the marks, the renderer's own work
         * between the stages - so this call is where the two meet:
         *
         *  * the renderer's STAGE ARRAYS are filled BY DECLARATION NAME out of @p chain (that is what still ties the
         *    record loop to this application: which stage holds which pass, which IS the frame's structure);
         *  * the two GI HALVES are assembled the same way, because their order (the trace half, the frame's rule
         *    between them, the denoise half) is the frame loop's;
         *  * @p chain must OUTLIVE the recording, exactly as the wiring's owner must;
         *  * @p wiring is the per-frame knowledge the renderer does not have (see `chain_wiring`).
         * @note a name the chain does not declare leaves that stage empty, which the runner treats as "no pass here" -
         *       a frame that draws less rather than a crash
         */
        void set_pass_chain(pass::pass_chain& chain, chain_wiring wiring) noexcept;

        // A non-const runtime exposes a mutable filter (e.g. runtime->get_vma()); a const runtime
        // gets a read-only filter, so mutating operations are impossible through const access.
        user_filter* operator->() noexcept {
            return &this->filtered_core;
        }
        user_filter const* operator->() const noexcept {
            return &this->filtered_core;
        }

        /**
         * @ingroup vulkan_runtime
         * @brief orbit camera state; left-drag rotates, wheel zooms
         */
        orbit_camera camera;

        /**
         * @ingroup vulkan_runtime
         * @brief background clear color applied every frame (the skybox is drawn over it, so it
         *        shows only where the environment pass leaves the background uncovered)
         */
        // background_color, not clear_color: render()'s own VkClearValue local is called
        // clear_color and a member of that name would be hidden by it there (MSVC /W4
        // C4458, an error under /WX).
        glm::vec3 background_color = glm::vec3(0.02f, 0.02f, 0.03f);

        /**
         * @ingroup vulkan_runtime
         * @brief run a batch of tasks on the runtime's shared worker pool and block until that
         *        priority group finished. Frame-time parallel stages (the animation controller's
         *        per-source sampling fan-out today, more later) submit their tasks here instead
         *        of owning private pools, so all CPU parallelism shares one pool sized to the
         *        machine (hardware_concurrency()/4 threads, floor 1).
         * @param tasks the batch; each task runs exactly once on a pool worker
         * @param priority the stage this batch belongs to (see task_priority); run_tasks waits
         *        only for this stage's tasks, so parallel stages can share the pool safely
         * @note synchronous: returns only after every task in the batch finished, which is what
         *       the frame phases need (the paced slot is read right after animation sampling)
         */
        void run_tasks(std::span<std::function<void()>> tasks, task_priority priority = task_priority::animation);

        /**
         * @ingroup vulkan_runtime
         * @brief worker count of the shared task pool (what run_tasks() fans out over); callers
         *        that need to slice their work across workers (e.g. the animation controller's
         *        per-source sampling) size their slices to this
         */
        [[nodiscard]] int32_t task_pool_threads() const noexcept {
            return this->task_pool.thread_count();
        }

        /**
         * @ingroup vulkan_runtime
         * @brief construct the runtime over a FINISHED core the caller already owns a reference to
         * @param shared_core the device root; must not be null (nothing can be created without a device, and
         *        with exceptions off there is no way to report it afterwards). The runtime takes a reference and
         *        KEEPS the device alive for as long as it lives; the caller's own reference is what lets the
         *        device outlive this runtime, which is the whole reason to share one.
         * @note the runtime's own objects (the pipelines a pass has not taken over, the descriptor families, the
         *       readback staging buffer) are still created and destroyed by this runtime, in that order - the
         *       shared root only changes WHO owns the device, not who owns those
         */
        explicit runtime(std::shared_ptr<core> shared_core);

        /**
         * @ingroup vulkan_runtime
         * @brief construct the runtime: performs the full core initialization (window / instance /
         *        device / swapchain / resources) from @p options, and registers the orbit camera
         *        mouse callbacks on the window
         * @param options the contract's creation structure, `deren::promise::rhi::create_info`
         *        (promise/rhi/rhi.core_desc.cppm): title, size, render scale, vsync, validation layers,
         *        window visibility, and the optional native_window the CALLER owns
         *
         * THE CONTRACT'S STRUCTURE IS THE ONLY ONE it takes: it goes straight to the core's own
         * constructor (`core(deren::promise::rhi::create_info const&)`), so there is no second
         * spelling of the creation parameters and no translation between them - the same structure
         * the program fills at startup is the one `deren_make_api_core()` receives. Nothing about
         * window / instance / device / swapchain creation is repeated here.
         *
         * `options.native_window` non-null means the CALLER owns that window: the core binds to it and
         * neither creates nor destroys it, while this runtime still registers its orbit-camera callbacks
         * on it (see the constructor above). A null `native_window` keeps today's behaviour: the backend
         * creates the window from the fields.
         * @note it creates its OWN core (see the constructor above for the sharing variant)
         */
        explicit runtime(deren::promise::rhi::create_info const& options);

        /**
         * @ingroup vulkan_runtime
         * @brief construct the runtime with default core options (1080x960 window,
         *        mailbox present mode)
         */
        runtime();

        /**
         * @ingroup vulkan_runtime
         * @brief destroy the runtime's own device objects while the device is still alive
         * @note explicit destructor: the members that hold device objects (pipelines a pass has not taken over,
         *      descriptor families, the readback staging buffer) are released here, and `core_owner` is declared
         *      ABOVE them so the device is still held while they go. The DEVICE itself may outlive this runtime
         *      when the caller kept a reference to the shared core - that is what sharing it means - so nothing
         *      here may assume it is the last user.
         */
        ~runtime();

        /**
         * @ingroup vulkan_runtime
         * @brief initialize the Dear ImGui debug overlay on top of this runtime's window
         * @return true when the overlay is active afterwards (initialized, or already active)
         * @note the overlay is drawn inside the runtime frame phases: its
         *       per-frame new_frame/record calls are driven by the runtime once enabled. Call
         *       after the runtime is fully set up (window/device ready). Safe to call again to
         *       re-enable after shutdown; no-op when already active.
         * @note F1 toggles the overlay at runtime: hide/show the whole
         *       overlay without losing its panels, and press it once more to bring it back
         */
        bool enable_debug_gui();

        /**
         * @brief true while the debug overlay owns the mouse: the camera orbit/zoom callbacks are
         *        suppressed then, so dragging an overlay slider cannot rotate or zoom the view
         */
        [[nodiscard]] bool debug_gui_wants_mouse() const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief access the debug overlay to manage its content from outside (register panels,
         *        push widgets, show/hide windows)
         * @return the runtime's gui_content (non-const: adding/removing panels mutates it)
         * @note panels added here are drawn every rendered frame by the runtime; add them after
         *       enable_debug_gui() (or any time — they are only drawn while the overlay is active)
         */
        [[nodiscard]] gui::gui_content& debug_gui() noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief drive one whole application frame in a single call by running the frame phases
         *        directly (see below): poll/skip/close, recreate-if-minimized, pace + acquire,
         *        record (world accumulation / culling / shadow / main pass / overlay) and
         *        submit + present, with no opportunity to interleave per-frame host writes.
         * @return frame_status: proceed when a frame was presented; skipped when not renderable
         *         (minimized or swapchain recreated - caller yields and calls again); closed on
         *         window close; one of the stage-specific *_failed values on a fatal Vulkan
         *         error (is_failure() tests for any of them)
         * @note callers that must write per-frame data (animation poses, skin matrices, morph
         *       weights) between pacing and recording call the granular phases themselves in the
         *       same order: poll_events -> recreate_if_minimized -> pace_and_acquire, then the
         *       writes (they may only touch per-slot buffers after pace_and_acquire has waited
         *       the slot's timeline), then begin_recording -> record_main_drawcalls ->
         *       end_recording -> submit_and_present.
         */
        frame_status render_frame();

        // ---- frame phases: one frame is these calls in order. render_frame() runs them all
        //      back to back; callers with per-frame host writes run them at fine granularity
        //      (writes go between pace_and_acquire() and begin_recording()) ----
        /** @brief poll window events; returns closed on ESC/native close, skipped while minimized.
         *         F1 toggles the debug overlay (initializing it on demand when it was disabled at
         *         startup), edge-triggered so one press toggles exactly once */
        frame_status poll_events();
        /** @brief recreate the swapchain if the window was minimized (extent 0) since the last frame */
        void recreate_if_minimized();
        /** @brief wait the frame slot's timeline, acquire the next image, write the camera UBO,
         *         and remember the paced slot; skipped when the swapchain
         *         was recreated, acquire_failed on a fatal acquire error */
        frame_status pace_and_acquire();
        /** @brief begin the slot's command buffer and run the CPU scene prep (world-matrix
         *         accumulation + frustum culling); begin_recording_failed when vkBeginCommandBuffer fails */
        frame_status begin_recording();
        /** @brief record the shadow pass, the attachment transitions and the main scene pass */
        void record_main_drawcalls();

        /**
         * @ingroup vulkan_runtime
         * @brief record the depth-only shadow-pass drawing content into @p command_buffer:
         *        bind the shared scene block + shadow pipeline, set the live depth bias, draw
         *        every scene leaf. The caller frames it (already inside the shadow rendering
         *        instance, depth-only).
         * @note extracted from record_main_drawcalls() so the same content can be recorded
         *       inline (stage 1) or into a per-slot secondary command buffer (stage 2,
         *       parallel recording) - only bind/push/draw commands, no barriers / begin-end.
         */
        /** @brief refit the directional shadow frustum to the current camera view (called once
         *         per frame from pace_and_acquire; skips the work unless the camera or scene moved,
         *         see shadow_frustum_valid) */
        void update_shadow_frustum();
        /**
         * @brief world-space AABB of an INSTANCED leaf (one draw covering many transforms)
         * @param leaf the leaf to bound
         * @param[out] wmin/wmax the union of the source geometry's LOCAL AABB transformed by each of
         *             the leaf's instance matrices (the vertex shader uses the instance matrix as the
         *             whole world matrix, so this is exact)
         * @return false when the leaf is not an instanced draw, its source/bounds are missing, or its
         *         instance slice cannot be read - the caller then has to fall back to a coarser bound
         */
        [[nodiscard]] bool instanced_world_aabb(primitive const& leaf, glm::vec3& wmin, glm::vec3& wmax) const;
        /// @brief record ONE cascade's content into its secondary: the begin (with the depth-only inheritance), the
        ///        cascade index's push, the scene block, the live bias state and every caster - the frame's callback
        /// @return whether the secondary was recorded (a failed begin must not be executed)
        static bool record_shadow_cascade(void* owner, VkCommandBuffer secondary, uint32_t cascade_index, VkPipeline pipeline, bool mesh_stage, bool meshlets);
        /// @brief the frame loop's scheduler, handed to the pass so one task per cascade records a secondary
        static void run_shadow_tasks(void* owner, std::span<std::function<void()>> tasks);
        /// @brief record the casters of one cascade, with either pipeline: @p mesh_stage says whether they are
        ///        DISPATCHED (the mesh form, see docs/mesh_shaders.md step 1) or drawn with the input assembler
        void record_shadow_content(VkCommandBuffer command_buffer, VkPipeline pipeline, bool mesh_stage, bool meshlets) const;

        /**
         * @ingroup vulkan_runtime
         * @brief fingerprint of everything the shadow pass reads as input: the caster count, every
         *        caster's world matrix, the uploaded skin matrices and the morph-scratch revision
         * @return one 64-bit fingerprint, equal across frames exactly when the shadow maps a slot
         *         already holds are still the maps the frame would render
         *
         * The shadow-map reuse compares this against the fingerprint of the frame that last rendered
         * a slot (see record_main_drawcalls). A skinned caster keeps a CONSTANT world matrix - its
         * pose lives entirely in the joint matrices - so the skin upload has to be part of the
         * fingerprint or an animated model keeps a frozen map; morph targets are written through
         * morph_scratch(), which has no upload hook, so it bumps a revision instead. XXH3 per matrix,
         * not a byte loop over the concatenation: this runs on every frame, the reused ones included.
         */
        [[nodiscard]] uint64_t shadow_geometry_signature() const;

        /**
         * @ingroup vulkan_runtime
         * @brief record the scene pass into @p command_buffer: move the single-sampled G-buffer
         *        targets (surface + velocity + the pass's own depth) into their render layouts, resync
         *        the pass geometry, then record the opaque leaves into them - no shading at all
         * @param command_buffer the frame's primary command buffer
         *
         * The engine's only scene path. It draws no background: the lighting stage writes the sky
         * into the pixels no geometry covered (shaders/sky.glsl, the same function the removed
         * forward skybox pass used). Alpha-blended geometry is recorded by
         * record_transparent_pass() after the lighting stage, because a blended surface has to
         * compose over the SHADED image - a G-buffer cannot hold a surface that does not exist yet.
         * record_scene_tail() turns the G-buffer into the frame afterwards through the deferred
         * lighting PASS (deren.vulkan.pass.deferred, resolved by resolve_deferred_pass()).
         */
        void record_scene(VkCommandBuffer command_buffer);

        /**
         * @ingroup vulkan_runtime
         * @brief record the transparent pass into @p command_buffer: the alpha-blended leaves, shaded
         *        while they draw and blended over the image the lighting stage just wrote,
         *        depth-testing against the G-buffer depth
         * @param command_buffer the frame's primary command buffer
         *
         * Runs AFTER the lighting stage (deren.vulkan.pass.deferred, the stage recorded just before it), in an
         * instance of its own, and that order is the whole
         * reason it is separate. Blending needs a shaded image underneath, and the lighting stage
         * needs the G-buffer depth as a SAMPLED texture - an image cannot be sampled and used as a
         * depth attachment in the same instance, so the depth is handed back to attachment layout in
         * between (sampling_to_depth_attachment_transition). It draws no sky (the lighting stage
         * already put the sky in the pixels no geometry covered) and does not write depth (every
         * transparent leaf draws with depth writes off, see primitive::draw).
         * @note these leaves carry no motion vectors, so TAA reprojects them with whatever the opaque
         *       surface behind them reported - good enough while the camera is the only thing moving,
         *       and the thing to revisit when object motion vectors land.
         */
        void record_transparent_pass(VkCommandBuffer command_buffer);

        /**
         * @ingroup vulkan_runtime
         * @brief move the attachments of one scene path into the layouts its rendering instance
         *        declares, before vkCmdBeginRendering - dynamic rendering has no automatic
         *        transitions the way a render pass does
         * @param command_buffer the frame's primary command buffer
         * @note the set is the G-buffer mode's three single-sampled surface targets, the motion-vector
         *       target and the scene color the emissive goes into, plus the pass's own 1x depth image;
         *       the main HDR target is not touched here
         */
        void record_scene_attachments(VkCommandBuffer command_buffer);

        /**
         * @ingroup vulkan_runtime
         * @brief resync every pipeline's cached fullscreen viewport/scissor from the current
         *        swapchain extent, on the primary thread and before the scene content is recorded
         *
         * A resize changes the extent the cached values were built from, and begin_pipeline() pushes
         * the cached values - so they are refreshed once per frame here, for every pipeline that can
         * draw (the scene pipelines, the skybox/post chain, the G-buffer, deferred lighting and TAA).
         */
        void update_pass_geometry();

        /**
         * @ingroup vulkan_runtime
         * @brief record the opaque scene into @p command_buffer: one secondary per task-pool worker
         *        segment (or a single one for a small frame), each inheriting the instance's color +
         *        depth attachments, plus the transparent secondary where the frame records one - the
         *        primary executes them in order inside the rendering instance
         * @param command_buffer the frame's primary command buffer
         * @note neither the G-buffer choice nor the transparent half is a parameter of this entry
         *       point: the leaves' pipelines and the optional extras are chosen inside the frame's own
         *       recording path (see record_main_segment() and record_transparent_pass())
         *
         * Shared by the opaque and transparent halves on purpose - the segmentation, the per-segment secondary
         * lifetime and the execute order are the same work in either; only the pipelines the leaves
         * bind (chosen in record_main_segment()) and the two optional extras
         * differ.
         */
        void record_opaque_scene(VkCommandBuffer command_buffer);

        /**
         * @ingroup vulkan_runtime
         * @brief record one contiguous slice of the main-pass leaves into @p command_buffer:
         *        bind the shared scene block, then draw the leaves of @p leaves (a sub-range of
         *        frame_visible). When @p draw_skybox the skybox background is drawn first so
         *        the background stays ordered before the scene (segment 0 only); later
         *        segments are pure scene.
         * @note stage 3 of parallel recording: each task-pool worker records one segment into
         *       its own secondary command buffer (see sub_render_task), the primary executes
         *       them in order. Only bind/push/draw commands - caller owns barriers + the
         *       rendering instance.
         */
        /** @brief close the scene rendering instance and record the post-process pass (HDR ->
         *         exposure/tonemap -> swapchain) plus the debug overlay on the final image
         *  @return true when a fullscreen pass actually wrote the SWAPCHAIN image (so it is in
         *          GENERAL and its contents are this frame's post-processed
         *          result); false when the pass was skipped (no pipeline/descriptors), in which case
         *          the swapchain image was never transitioned and the caller must not pretend
         *          otherwise to the present barrier or the screenshot copy
         *  @note this function only SEQUENCES the frame's back half: the scene-side tail is recorded by
         *        record_scene_tail, and the bloom chain and the composite are two STAGES of passes
         *        (deren.vulkan.pass.post) whose frames this function sets and resolves - the renderer no longer records
         *        them. It used to be all of that in one 209-line body, whose real problem was not its length but
         *        that a change to any one of them had to be located inside the other two. */
        [[nodiscard]] bool record_post_process(VkCommandBuffer command_buffer);
        /**
         * @brief close the geometry instance and record the scene-side stages that follow it
         *        (deferred lighting, the TAA resolve, the G-buffer debug view), each with its own
         *        GPU timing mark
         * @note the marks are unconditional even when a stage does not run this frame: the
         *       label-to-interval mapping is positional, so a skipped stage writes its mark
         *       immediately after the previous one and its interval reads 0
         */
        void record_scene_tail(VkCommandBuffer command_buffer);
        /**
         * @brief whether the FXAA pass is this frame's LAST writer (the frame's own question: the composite's
         *        target and pipeline variant, the overlay's owner, set_fxaa() and the feature registry all ask it)
         */
        [[nodiscard]] bool post_fxaa_active() const noexcept;
        /**
         * @brief whether the UPSCALE pass is this frame's LAST writer
         *
         * The frame's own question, exactly as `post_fxaa_active` is: the composite's target and pipeline
         * variant, the overlay's owner, the feature registry and `frame_facts::upscale_resolves` all ask it.
         * It is true only BELOW render_scale 1.0 - at 1.0 the render chain IS the output, so a resolve would be
         * a same-size resample that costs a pass and changes nothing, and the frame stays exactly the frame it
         * has always been.
         */
        [[nodiscard]] bool post_upscale_active() const noexcept;
        /**
         * @brief the debug overlay, drawn INSIDE the instance of whichever pass is the frame's last writer
         *
         * A static thunk because a pass's frame takes a plain function pointer and a context: what it calls is this
         * class's `record_overlay_if_enabled`, which is a member. The FXAA pass gets it on the frames it runs and
         * the composite on the frames it does not - one callback, handed to exactly one of them per frame.
         */
        static void draw_overlay_after(void* owner, VkCommandBuffer command_buffer);
        /** @brief the overlay, when it should draw into @p command_buffer (inside the caller's instance) */
        void record_overlay_if_enabled(VkCommandBuffer command_buffer);
        /** @brief transition @p image to SHADER_READ_ONLY (a barrier must not be recorded inside a
         *         rendering instance, so this is always called before vkCmdBeginRendering)
         *  @note the only caller left is the HDR target's transition before the post chain - the one transition the
         *        chain's passes deliberately do NOT own (see render_resource::post_composite_io). The
         *        colour-attachment twin this pair used to have is gone with the recorders that used it. */
        void barrier_image_to_sampling(VkCommandBuffer command_buffer, VkImage image);
        /**
         * @brief record one segment of the main pass (see the doc block above record_opaque_scene)
         * @param gbuffer_pass true = the leaves bind the G-buffer pipeline (the opaque instance of
         *        the deferred path); false = they bind their own forward pipelines. Passed in rather
         *        than re-derived from gbuffer_pass_active(), because the deferred path also has
         *        forward-style segments: its transparent pass runs while the G-buffer pass is the
         *        active mode, and still shades while it draws.
         */

        /**
         * @ingroup vulkan_runtime
         * @brief one recording job of the parallel main pass (stage 3): records @p leaves (a
         *        contiguous slice of the frame's visible leaves) into @p command_buffer, a
         *        per-slot SECONDARY command buffer. operator() begins the secondary (inheriting
         *        the main instance's color+depth attachments via dynamic rendering 1.3
         *        inheritance info), records the slice and ends it, so a batch of these can be
         *        posted straight to the shared task pool and the recording group waited on.
         * @note value type (span + handle + formats; no owning pointers), safe to copy into
         *       std::function for the pool; the begin-info is assembled fresh inside operator()
         *       so copies never share dangling pNext chains.
         */
        struct sub_render_task {
            VkCommandBuffer command_buffer = VK_NULL_HANDLE;
            std::span<primitive const* const> leaves = {};
            // Color attachment formats of the instance this secondary is recorded into, in
            // attachment order: one entry (the HDR target) when the leaves shade into it, the four G-buffer
            // attachments when the opaque pass writes the G-buffer. Held by value because the task outlives the
            // call that builds it (it is moved into the task pool).
            std::array<VkFormat, deren::vulkan::gbuffer_pass_attachment_count> color_formats = {};
            uint32_t color_count = 0;                    // formats in use (1 when only the HDR target, 4 for surface targets + HDR)
            VkFormat depth_format = VK_FORMAT_UNDEFINED; // main depth attachment format
            VkSampleCountFlagBits rasterization_samples = VK_SAMPLE_COUNT_1_BIT;
            bool gbuffer_pass = false;      // leaves bind the G-buffer pipeline (not the HDR-shading ones)
            runtime const* owner = nullptr; // recording context (scene set / pipeline caches)
            // set to true by operator() when the secondary was actually recorded (begin + end
            // succeeded). Points into a per-frame array owned by the caller of the task batch;
            // the caller waits the recording group before reading it, so no extra sync is
            // needed. The primary must NOT execute a segment whose begin failed.
            std::atomic<bool>* recorded = nullptr;

            void operator()() const; // defined in runtime.cpp (module-private)
        };

        /** @brief end the rendering instance / render pass and finish recording;
         *         end_recording_failed when vkEndCommandBuffer fails */
        frame_status end_recording();
        /** @brief submit the recorded command buffer and present the image (recreating the
         *         swapchain when presentation reports out of date); submit_failed /
         *         present_failed on fatal errors */
        frame_status submit_and_present();

        /**
         * @ingroup vulkan_runtime
         * @brief create a named pipeline from raw SPIR-V and cache it in the runtime. The first
         *        pipeline created becomes the runtime's DEFAULT pipeline (implicitly); primitives
         *        with default semantics draw with it. Every pipeline is heap-native and layout-less,
         *        so any number of them can coexist in one scene (leaves choose by name).
         * @param pipeline_name the pipeline's name (used by primitives to request it, and by
         *        render_environment to bind it); must be unique
         * @param fragment_shader_code raw SPIR-V binary of the fragment shader
         * @param mesh_vertex_shader_code raw SPIR-V binary of the MESH stage, which IS the vertex stage
         *        since step 4 (docs/mesh_shaders.md): a name without a mesh module is an ERROR rather
         *        than a fallback
         * @param meshlet_shader_code the meshlet form of that stage - one workgroup per meshlet, culled
         *        against the camera; optional, and preferred over the mesh form where it exists
         * @return success, or an error message on failure
         * @note thread-safe (registry guarded), but call OUTSIDE the frame loop: recording
         *       workers read the registry lock-free during a frame (see record_main_segment's
         *       concurrency note), so mid-frame registration would race them. Register at setup
         *       or between frames (while idle).
         */
        std::expected<void, std::string> make_pipeline(
            std::string_view pipeline_name,
            std::span<uint8_t const> fragment_shader_code,
            // THE MESH FORM, which is the pipeline itself since step 4 (docs/mesh_shaders.md): the vertex stage is
            // gone, so a name without a mesh module is an ERROR rather than a fallback. Stored under @p pipeline_name
            // in `mesh_pipelines`, which is what a session that binds by name looks in first.
            std::span<uint8_t const> mesh_vertex_shader_code,
            // ... AND THE MESHLET FORM OF IT (docs/mesh_shaders.md step 3), the same shape again one level in: one
            // workgroup per meshlet, culled against the camera, stored in `meshlet_pipelines` under the same name
            // and preferred by the named sessions over the mesh form.
            std::span<uint8_t const> meshlet_shader_code = {});

        /**
         * @ingroup vulkan_runtime
         * @brief register the CHARACTER-FORWARD pipeline under @p pipeline_name
         *
         * A SEPARATE ENTRY POINT FROM make_pipeline rather than an overload of it, because that one builds
         * the FORWARD FAMILY's pipeline - the swapchain format, src-alpha blending, depth LESS_OR_EQUAL -
         * which is right for a leaf that shades straight into the display target and wrong for this stage in
         * all three. This one goes through `core::make_character_forward_pipeline`: ONE HDR target, blending
         * OFF, depth compare EQUAL (see that builder for why each of the three is forced rather than chosen).
         *
         * The result lands in the SAME two registries (`mesh_pipelines` / `meshlet_pipelines`) under the same
         * name, so a session that binds by name finds it through the one lookup every forward session already
         * performs. That is what lets the character-forward pass get its pipeline by setting
         * `render_environment::default_name` and nothing else - no binder change, no redirect flag.
         *
         * It does NOT become the implicit default: `default_pipeline_name` is left alone, because a frame
         * whose leaves are drawn before any character pipeline is registered must not silently acquire one.
         */
        std::expected<void, std::string> make_character_forward_pipeline(
            std::string_view pipeline_name,
            std::span<uint8_t const> fragment_shader_code,
            std::span<uint8_t const> mesh_vertex_shader_code,
            std::span<uint8_t const> meshlet_shader_code = {});

        /**
         * @ingroup vulkan_runtime
         * @brief register the OVERLAY (multiply) pipeline under @p pipeline_name
         *
         * THE THIRD ENTRY POINT OF THE SAME SHAPE, and the third set of states: `make_pipeline` builds the
         * forward family's (swapchain format, src-alpha blending, depth LESS_OR_EQUAL),
         * `make_character_forward_pipeline` the toon stage's (HDR target, blending off, depth EQUAL), and this
         * one the article's two masks' (HDR target, `dst = src * dst`, depth LESS_OR_EQUAL). It goes through
         * `core::make_overlay_pipeline`, which states why each of those is forced - including why the depth
         * compare cannot be the toon stage's `EQUAL`.
         *
         * It lands in the same two registries and, like the toon pipeline, never becomes the implicit default.
         * A device that refuses it is not a failure of the run: the pass takes an EMPTY name to mean "draw no
         * overlay" (`character_forward_frame::overlay_pipeline_name`), because the only alternative - drawing
         * the masks with the toon pipeline - would paint two shaded quads over the face.
         */
        std::expected<void, std::string> make_overlay_pipeline(
            std::string_view pipeline_name,
            std::span<uint8_t const> fragment_shader_code,
            std::span<uint8_t const> mesh_vertex_shader_code,
            std::span<uint8_t const> meshlet_shader_code = {});

        /**
         * @ingroup vulkan_runtime
         * @brief create the ARTICLE'S ① 描边 (inverted hull) pipeline under @p pipeline_name
         * @param pipeline_name the name the pipeline is registered under (the runtime's own
         *        `outline_pipeline_name = "outline"`)
         * @param fragment_shader_code raw SPIR-V of outline.frag (the article's inverted-hull fragment stage)
         * @param mesh_vertex_shader_code raw SPIR-V of pbr.mesh - the geometry is the outline's own mesh stage,
         *        because a hull has to push its vertices outward and only that stage does (see `pbr.slang`)
         * @param meshlet_shader_code raw SPIR-V of the meshlet form, or empty when there is none
         * @return success, or an error message on failure
         *
         * THE FOURTH ENTRY POINT OF THE SAME SHAPE, and the fourth set of states: `make_pipeline` builds the
         * forward family's (swapchain format, src-alpha blending, depth LESS_OR_EQUAL), `make_character_forward_pipeline`
         * the toon stage's (HDR target, blending off, depth EQUAL), `make_overlay_pipeline` the article's two
         * masks' (HDR target, `dst = src * dst`, depth LESS_OR_EQUAL), and this one the ① 描边 hull's (HDR
         * target, opaque blend, depth LESS_OR_EQUAL). It goes through `core::make_outline_pipeline`, which
         * states why each of those is what it is.
         *
         * IT LANDS IN THE SAME TWO REGISTRIES AND, LIKE THE OTHER TWO FAMILY PIPELINES, NEVER BECOMES THE
         * IMPLICIT DEFAULT: it declares ONE colour attachment, so a default-semantics leaf drawn by it would
         * paint an unlit hull where a shaded surface belongs. A device that refuses it is not a failure of the
         * run: the pass takes an EMPTY name to mean "draw no outline"
         * (`character_forward_frame::outline_pipeline_name`), which is the frame this renderer produced before ①
         * landed.
         */
        std::expected<void, std::string> make_outline_pipeline(
            std::string_view pipeline_name,
            std::span<uint8_t const> fragment_shader_code,
            std::span<uint8_t const> mesh_vertex_shader_code,
            std::span<uint8_t const> meshlet_shader_code = {});

        /**
         * @ingroup vulkan_runtime
         * @brief make @p pipeline_name the runtime's default pipeline (the one default-semantics
         *        primitives draw with; see make_pipeline for the implicit first-pipeline default)
         * @param pipeline_name a pipeline previously created via make_pipeline()
         */
        void set_default_pipeline(std::string_view pipeline_name);

        /**
         * @ingroup vulkan_runtime
         * @brief turn clustered light culling on/off (no-op without the cluster PASS's pipeline)
         * @param enabled true = the shading stage loops only its own cluster's light list
         * @note CPU-side only (the flag rides the light UBO's cluster_grid.w lane): the next frame's
         *       cluster pass and shading both read it, so it is safe to toggle mid-run.
         */
        void set_clustered_lights(bool enabled) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief turn the TOON CHARACTER STAGE on/off
         *
         * OFF BY DEFAULT, and that is the feature's contract rather than a conservative choice: the stage
         * re-shades the scene's opaque leaves over the lit frame, so with it on every opaque surface in the
         * scene is drawn by the character pipeline - which is what a character viewer wants and what a
         * scenario comparing against the pre-existing references must not have.
         *
         * @param enabled true = the character-forward pass records this frame (when there is opaque geometry
         *        to re-shade and the pipeline was created); false = it is skipped without even resolving
         * @note CPU-side only: the flag rides `feature_facts::character_forward_pending`, which the pass's own
         *       gate reads, so it is safe to toggle mid-run - the next frame's stage sees it.
         */
        void set_character_forward(bool enabled) noexcept;

        /**
         * @brief whether the character-forward pipeline exists, i.e. whether the toon stage CAN run
         *
         * The pipeline needs the mesh stage, so a device without `VK_EXT_mesh_shader` registers none - and a
         * switch offered for a stage that cannot draw is the bug `feature_available` exists to prevent. This is
         * that question asked of the registry rather than of the knob.
         */
        [[nodiscard]] bool character_forward_ready() const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief draw the character stage with the REWRITTEN toon chain instead of the old one
         *
         * WHAT IT SELECTS, EXACTLY: the pipeline name `make_character_forward_frame` hands the pass - see
         * @ref goo_toon_pipeline_name. It does NOT turn the stage on: a frame with `character_forward` off draws
         * no character stage at all, whatever this says, and that separation is deliberate - "the new shading
         * model" and "the character stage exists" are two facts, and a single knob for both would make the A/B
         * that verifies the rewrite unable to say which one moved the picture.
         *
         * OFF BY DEFAULT, on `set_character_forward`'s terms: it changes what every eye-family material in the
         * scene is shaded by, and this step of the rewrite implements ONE reference group - so it is a switch a
         * run asks for rather than a state a frame drifts into.
         *
         * @param enabled true = the stage draws with `shaders/goo_toon.slang` (when that pipeline was created);
         *        false = it draws with the old `character_forward.slang`
         * @note CPU-side only, like `set_character_forward`: it is read while the frame is COMPOSED, so a toggle
         *       mid-run is seen by the next frame and disturbs no in-flight recording.
         */
        void set_goo_toon(bool enabled) noexcept;

        /**
         * @brief whether the REWRITTEN chain's pipeline exists, i.e. whether `goo_toon` can select anything
         *
         * Asked of the registry rather than of the knob, exactly as `character_forward_ready` is: the pipeline
         * needs the mesh stage, so a device without `VK_EXT_mesh_shader` registers none.
         */
        [[nodiscard]] bool goo_toon_ready() const noexcept;

        /**
         * @brief whether THE REWRITTEN CHAIN IS THE ONE DRAWING this frame: `goo_toon` on AND its pipeline there
         *
         * A DIFFERENT QUESTION FROM `goo_toon_ready` ABOVE, and the difference is the knob: `ready` answers "can
         * this chain be selected at all", this answers "is it selected". It is the SAME PREDICATE
         * `make_character_forward_frame` uses to choose the pipeline name, exposed because a second stage has to
         * ask it: the screen-space rim must not draw the article's contour over the reference's rim, and the
         * question it needs answered is this one rather than either half of it. Two predicates for one decision
         * would let a knob turned on without a pipeline silence the article's rim and draw nothing - so the
         * frame's own answer is published here rather than recomposed by the asker.
         */
        [[nodiscard]] bool goo_toon_active() const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief HOW THE RUNTIME REACHES A MATERIAL'S TOON MAPS, without knowing what a sidecar is
         *
         * WHY A CALLBACK RATHER THAN THE DATA: the toon maps are named in a `.toon.tsv` beside the model, and
         * the module that reads it lives in `gltf_loader` - which `vulkancorekit` deliberately does not depend
         * on (see that target's note: the engine is loader-agnostic). So the runtime cannot hold a sidecar, and
         * it must not learn what one is. What it CAN say is what it needs: given a material's NAME and one of
         * the four `toon_slot` lanes, the texture input for it, and the artist's `_Use` bits for that material.
         * The APPLICATION is the layer that links both, so the application installs this.
         *
         * A NULL `texture` MEANS "THIS RENDERER HAS NO TOON SOURCE", and then every primitive's toon block stays
         * empty - which is every model that is not a character, and every build that never installs one. A null
         * `flags` with a live `texture` is allowed and means "every lane that resolved is wanted"; the two are
         * separable because a caller may have the maps without the sidecar's switches.
         */
        struct toon_lookup {
            void* owner = nullptr;
            /// the texture input for @p lane of @p material_name, or an INVALID one when the material has no such
            /// map OR the artist's `_Use` flag is off - the two collapse HERE on purpose, because to a shader
            /// they are one instruction ("do not read a ramp") and the record carries no enable word for it (see
            /// `material_record::toon_indices`). The sidecar keeps them apart for diagnosis, and the application
            /// is where the distinction is consumed.
            texture_input (*texture)(void* owner, std::string_view material_name, toon_slot lane) = nullptr;
            /// the COLOUR for @p lane of @p material_name, or WHITE when the material has no such row.
            ///
            /// SEPARATE FROM `texture` BECAUSE IT CAN BE: a caller may have the maps and not the colours (an older
            /// sidecar), and a null callback here leaves every lane at the neutral white `toon_inputs` starts with -
            /// which is the same state a material with no `color` row reaches, and for the same reason.
            glm::vec4 (*colour)(void* owner, std::string_view material_name, toon_colour_lane lane) = nullptr;
            /// ONE SIDECAR SCALAR of @p material_name, or @p fallback when the material states none.
            ///
            /// THE THIRD CALLBACK EXISTS FOR A BLEND STATE, which is the one toon fact that is not a map, not a
            /// colour and not a family: the author's transparent variant is selected by a MATERIAL PROPERTY
            /// (`_TransParentToon`) and its blending by a PAIR OF NUMBERS (`_SrcBlend 5` / `_DstBlend 10` on
            /// chen's `cloth_02`), and in this port neither glTF nor the family can express it - glTF's
            /// `alphaMode` on that material is OPAQUE. So the runtime asks the sidecar for the two numbers by
            /// NAME, exactly as it asks for the two maps and the colours, and the honesty of that is the point:
            /// "this surface is alpha-blended" gets to be a statement the ASSET makes rather than a material name
            /// or a family the port hard-codes. See `toon_inputs::alpha_blend`.
            float (*scalar)(void* owner, std::string_view material_name, std::string_view row, float fallback) = nullptr;
        };

        /**
         * @ingroup vulkan_runtime
         * @brief install the toon map source used by the NEXT import (see toon_lookup)
         * @note the owner must outlive the import. The lookup is read while `import_scene` builds each
         *       primitive's `primitive_create_info`, so a lookup installed after the import has no effect on
         *       what was imported - which is the honest behaviour for a source rather than a mystery.
         */
        void set_toon_lookup(toon_lookup const& lookup) noexcept;

        /**
         * WHERE THIS RENDERER GETS A MATERIAL'S TOON MAPS (see `toon_lookup`). Empty by default, and then every
         * primitive's toon block stays empty - which is what makes "no source installed" and "no sidecar beside
         * this model" the same state rather than two to handle.
         */
        /// named `toon_lookup_source` rather than `toon_lookup`: the struct above is `toon_lookup`, and a
        /// member of that name would hide the type inside this class's scope
        toon_lookup toon_lookup_source = {};

        /**
         * @ingroup vulkan_runtime
         * @brief enable directional shadow mapping: fills the light UBO with an orthographic
         *        view-proj framing the given scene bounds (plus the light direction, matching
         *        the sky sun). Must be called after the models exist (the shadow pass draws them).
         * @param scene_center world-space center of the shadow frustum (usually the scene bounds
         *        center after the scene offset is applied, i.e. where the models actually sit)
         * @param scene_radius conservative radius covering all shadow casters
         * @note requires the shadow PASS's pipeline (deren.vulkan.pass.shadow, created by create_passes()); no-op otherwise
         */
        void enable_shadows(glm::vec3 const& scene_center, float scene_radius);

        /**
         * @ingroup vulkan_runtime
         * @brief enable or disable main-pass frustum culling (BVH vs camera frustum)
         * @param enabled true (default) culls leaves outside the view frustum before drawing
         */
        void set_frustum_culling(bool enabled) noexcept {
            this->frustum_culling = enabled;
        }

        /**
         * @ingroup vulkan_runtime
         * @brief enable or disable the directional shadow each frame
         * @param enabled true (default) records the shadow pass and samples the map; false skips
         *        the depth pass - pbr.frag then skips calc_shadow entirely (fully lit)
         * @note requires enable_shadows() to have succeeded. Flag-only toggle: nothing is cleared
         *       and no command is recorded; the shader simply stops sampling the stale map
         */
        void set_shadow_enabled(bool enabled);

        /**
         * @ingroup vulkan_runtime
         * @brief ask for ray-traced sun shadows ([render] rt_shadows)
         * @param enabled true = shadows are traced against the scene's acceleration structures
         * @note the request is granted only on a device with ray queries (core::ray_query_available);
         *       everywhere else, and while the flag is false, the cascaded shadow maps are what the
         *       shading stages sample and nothing about the frame changes
         * @note the structures themselves are built by the first frame that records with the flag on,
         *       because the caster set they are built from is only complete once the scene is loaded
         */
        void set_rt_shadows(bool enabled) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief whether the alphaMode MASK bake runs when the structures are built ([render] rt_mask_bake)
         * @param enabled false = masked geometry is built OPAQUE, i.e. solid to every ray
         * @note a no-op without ray-traced shadows (no structures, no bake), so a frame
         *       with them off is byte-identical either way. It exists as a knob because the bake is an
         *       approximation - a triangle is either in the structure or not, while the raster path discards
         *       per fragment - and the size of that difference is what its measurement compares (see
         */
        void set_rt_mask_bake(bool enabled) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief whether skinned casters are re-skinned and their structures refitted every frame
         *        ([render] rt_skin_bake)
         * @param enabled false = the structures keep whatever pose they were last built or refitted in
         * @note the structures are built from the model-space bind pose (see acceleration_structure::add), so
         *       without this pass a traced shadow of an animated mesh is cast by the mesh where it is NOT -
         *       default, and unlike set_rt_mask_bake it is read every frame: the pass runs per frame, so the
         *       flag can be flipped at any time and the next frame's traced shadows follow.
         */
        void set_rt_skin_bake(bool enabled) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief whether ANY ray-traced feature is asking for the acceleration structures
         * @note the structures serve both ray-traced features, so they are built when either wants them -
         *       and they must be, because both pipelines declare the top level structure as a descriptor:
         *       a shader that statically uses a binding needs it to have been written, whether or not the
         *       branch that reads it runs.
         */
        [[nodiscard]] bool rt_structures_wanted() const noexcept;

        /** @brief whether ray-traced shadows are actually on (asked for AND the device can do it) */
        [[nodiscard]] bool rt_shadows_active() const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief select the specular BRDF model preset (pbr.frag, gui "brdf model" combo):
         *        0 = GGX + joint Smith (default), 1 = GGX + height-correlated Smith,
         *        2 = Beckmann + Smith, 3 = Blinn-Phong + Smith
         * @param model preset id (clamped to the valid range)
         * @note CPU-side only: the value rides the light UBO's std140 padding and is copied into
         *       the paced slot's buffer every frame, so this is safe at any time (GUI included)
         */
        void set_brdf_model(int32_t model) noexcept;
        /**
         * @ingroup vulkan_runtime
         * @brief select the diffuse BRDF model (pbr.frag, gui "diffuse model" combo):
         *        0 = Lambert (default), 1 = Oren-Nayar
         * @param model model id (clamped)
         * @note same timing rule as set_brdf_model
         */
        void set_diffuse_model(int32_t model) noexcept;
        /**
         * @ingroup vulkan_runtime
         * @brief linear exposure scale applied to the rendered image before tonemapping
         *        (pbr.frag / skybox.frag read it from the light UBO / skybox push constant)
         * @param exposure multiplier (1 = unchanged; clamped to a sane 0.05 .. 20 range)
         * @note same timing rule as set_brdf_model: CPU-side only, copied into the paced slot
         *       every frame, so this is safe at any time (GUI slider included)
         */
        void set_exposure(float exposure) noexcept;
        /**
         * @ingroup vulkan_runtime
         * @brief publish the FACE SDF's head frame (scene block slot 749) for the next frame
         *
         * WHY THE APPLICATION OWNS THIS RATHER THAN THE RUNTIME: the head frame comes from a BONE, and bones live
         * in the loader's skeleton - which `vulkancorekit` deliberately does not depend on (the engine is
         * loader-agnostic, the same reason `set_toon_lookup` exists). So the layer that links both resolves the
         * head joint and hands the three axes over, and the runtime only stores and uploads them.
         *
         * ARBITRARY-TIME, like `set_exposure`: it writes `head_state` and the next `pace_and_acquire` copies that
         * into the paced slot, so a frame already in flight keeps the frame it started with. Calling it every
         * frame is how a model whose head TURNS stays correct; calling it once is correct for a model that cannot
         * turn its head, which is what this repository's only SDF-bearing model is.
         */
        /**
         * @brief publish the toon stage's LIGHT RIG (the sun/head-light split and the chain's global scalars)
         *
         * The character stage's global numbers are one block rather than fields of every material record - see
         * `deren::vulkan::toon_rig` for what belongs there and `core::heap_slots::toon_rig` for why it is safe to write
         * outside the frame path: nothing rewrites it while a frame is in flight, so a call from the import path
         * or from a settings change cannot be read half-written. It MEANS the call must be made BEFORE the frame
         * loop starts (or between frames), which is what the application does.
         */
        void set_toon_rig(toon_rig const& rig) noexcept;
        void set_head_basis(head_ubo const& basis) noexcept;
        /**
         * @ingroup vulkan_runtime
         * @brief the current exposure scale (see set_exposure)
         */
        [[nodiscard]] float exposure() const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief cel/toon shading: quantize the direct-light diffuse falloff (and harden the
         *        shadow edge / the specular lobe) into @p steps bands
         * @param steps 0 = plain PBR (default); 2..8 = band count (rounded, clamped)
         * @param softness band edge width in normalized [0,1] space (0.01..0.5); smaller = harder
         *        cel edges
         * @note same timing rule as set_exposure: CPU-side, copied into the light UBO every frame
         */
        void set_toon_shading(float steps, float softness) noexcept;
        /**
         * @brief a scale on the sun's radiance (0..3; 1.0 = the shading path's constant unchanged).
         * @note the furnace mode forces the lane to 0 whatever this says - it turns the sun OFF, and a
         *       slider must not be able to argue with that.
         */
        void set_sun_intensity(float scale) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief move the sun: `[lighting] sun_direction`, pointing FROM the surface TOWARD the sun
         *
         * ONE CALL MOVES EVERYTHING THAT MUST AGREE. The light UBO is rebuilt with this direction (the
         * shading's `light_dir` and the shadow cascades' matrices), the sky reads the same field out of that
         * UBO for its disc, and the env bake took it at startup - so a moved sun is a moved sun everywhere
         * rather than a new shadow direction under an old sky.
         *
         * A CHANGE INVALIDATES THE CACHED SHADOW FIT: the cascades are fitted in light space, so they are only
         * valid for one direction - the same invalidation a new light setup does. Setting the SAME direction
         * again is free, which is what lets a caller mirror it every frame.
         *
         * @param direction the sun's direction in world space, unnormalized; a zero vector is ignored because
         *        there is no direction in it and `normalize` would put NaNs through the whole lighting stage
         */
        void set_sun_direction(glm::vec3 direction) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief describe the frame's rectangular AREA LIGHT (the reference package's 30 m soft box)
         *
         * A SECOND LIGHT IS NOT A SECOND LIGHT IN THIS ENGINE. The shading, the shadows, the sky's disc and
         * the environment bake all read ONE directional light out of the light UBO, so an emitter is expressed
         * through it: `centre`/`axis` land in the two `area_light`/`area_light_axis` lanes appended after
         * `cluster_depth`, and the CALLER separately points the sun at the emitter (`set_sun_direction`) and
         * scales its radiance (`set_sun_intensity`). See `deren::app_config::derive_area_light`.
         *
         * @param centre    emitter centre in world space (metres)
         * @param half      HALF the emitter's side (metres); <= 0 means NO area light and writes zero to both
         *                  lanes, which is the default and the byte-identical contract
         * @param irradiance whether the emitter CONTRIBUTES ENERGY, i.e. whether the caller lets it take over
         *                  the main light's direction and radiance (v1.1). The lane's `w` component carries the
         *                  sign, because the block has no spare boolean: `half` when true, `-half` when false.
         *                  A zero `half` still writes a literal `0`, never `-0`. Note that as of v1.1 no shader
         *                  reads this lane any more (the polygon-Lambert size correction it used to gate was
         *                  deleted); the sign is kept because the wire layout is pinned and the value is what a
         *                  capture can be audited against.
         * @param axis      the emitter's normal (centre -> target), unnormalized; only read while `half > 0`
         * @param penumbra  the shadow's penumbra radius in WORLD metres (0 = the shadow keeps its hard edge)
         * @note CPU-side and idempotent, copied into the light UBO every frame; unlike `set_sun_direction` it
         *       does NOT invalidate the cached shadow fit - the cascades are fitted to the sun's direction, and
         *       this only widens the lookup inside them. `enable_shadows` rebuilds `light_state` from scratch,
         *       so a caller that enables shadows must call this AFTER that (the frame loop does).
         */
        void set_area_light(glm::vec3 const& centre, float half, bool irradiance, glm::vec3 const& axis, float penumbra) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief bloom amount for the post-process pass (bright-pass threshold + blend weight)
         * @param intensity how much of the blurred bright pass is added back (0 disables bloom)
         * @param threshold linear luminance subtracted in the bright pass (visible range 0..0.75:
         *        brightest highlights glow); both are clamped to sane ranges
         * @note same timing rule as set_exposure: CPU-side, copied into the post push constants
         */
        void set_bloom(float intensity, float threshold) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief enable/disable FXAA and set its two knobs
         * @param enabled when true the composite renders into a display-referred (gamma-encoded)
         *        LDR image and an extra fullscreen pass anti-aliases it into the swapchain; when
         *        false the composite writes the swapchain directly (no extra pass, no LDR read)
         * @param subpixel sub-pixel term strength, 0..1 (0 = pure directional blend; higher also
         *        blends away the single-pixel aliasing FXAA leaves on near-axis-aligned edges)
         * @param edge_threshold relative luma contrast below which a pixel counts as flat and is
         *        left untouched (FXAA default 0.166; lower = more edges treated, softer image)
         * @note requires the FXAA PASS's pipeline (deren.vulkan.pass.fxaa, created by create_passes()); without it the
         *       flag has no effect. CPU-side, copied
         *       into the post push constants every frame (same timing rule as set_exposure)
         */
        void set_fxaa(bool enabled, float subpixel = 0.75f, float edge_threshold = 0.166f) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief cap the render loop at @p fps frames per second (0 or less = uncapped)
         *
         * A frame limiter, not a present-mode choice: with Mailbox - and, measured, even with
         * FIFO_LATEST_READY - the loop free-runs as fast as the CPU can record, which burns most of a
         * core producing frames nobody sees. The wait lives in pace_and_acquire() so it is attributed
         * to the pace phase and never happens while a swapchain image is held. 0 is the default because
         * that is the mode a throughput measurement needs.
         */
        void set_max_fps(double fps) noexcept;

        /** @brief whether the FXAA pass is currently enabled (see set_fxaa) */
        [[nodiscard]] bool fxaa() const noexcept {
            return this->fxaa_on;
        }

        /**
         * @ingroup vulkan_runtime
         * @brief enable/disable the GPU pass timing read-back
         * @param enabled when true (the default) every frame records one timestamp per pass
         *        boundary and the completed measurements are averaged over a 60-frame window,
         *        logged and exposed through gpu_timing_summary(); when false no timestamp is
         *        written and nothing is read back
         * @note the switch only matters on devices that can timestamp at all
         *       (core::gpu_timing_available()); the marks themselves are a handful of
         *       vkCmdWriteTimestamp calls per frame, so the cost of leaving it on is negligible -
         *       this exists for a measurement-free profile run
         */
        void set_gpu_timings(bool enabled) noexcept {
            this->gpu_timings_enabled = enabled;
            // one switch for measure this frame: the CPU phases follow the GPU marks
            this->cpu_timings.set_enabled(enabled);
        }

        /** @brief whether GPU pass timings are being collected (see set_gpu_timings) */
        [[nodiscard]] bool gpu_timings() const noexcept {
            return this->gpu_timings_enabled;
        }

        /**
         * @ingroup vulkan_runtime
         * @brief report of the current timing window: the mean GPU milliseconds per pass
         * @return e.g. "gpu:  shadow 0.11  main 0.24  bloom 0.01\n     composite 0.08  fxaa 0.00
         *         tail 0.00\n     total 0.43 ms" (broken over lines to fit the narrow overlay
         *         panel), or a short "off"/"unavailable"/"collecting" note
         * @note safe to call every frame (it formats a string from the running window mean) - the
         *       debug overlay binds a label to it
         */
        [[nodiscard]] std::string gpu_timing_summary() const;

        /**
         * @ingroup vulkan_runtime
         * @brief stochastic punctual lighting: sample a few lights per pixel and trace one shadow ray each
         * @param enabled true = the punctual lights are the stochastic pass's business (and the deferred
         *        stage stops adding them raster-style), false = the engine's historic unshadowed loop
         * @return whether this call turned the feature ON (the off -> on edge)
         * @note the SAMPLE COUNT, the minimum sample weight and the origin bias are the PASS's parameters and
         *       are set by whoever owns that pass (`vulkan.render_start_demo::set_megalights`), by the same
         */
        [[nodiscard]] bool set_megalights_enabled(bool enabled) noexcept;

        /// @brief whether the stochastic punctual lighting chain runs this frame (its own composed predicate)
        [[nodiscard]] bool megalights_active() const noexcept;

        /**
         * @ingroup vulkan_runtime
         *       nothing to the marched path, whose hits are the depth buffer's own surface.
         */

        /**
         * @ingroup vulkan_runtime
         * @brief trace a glossy reflection ray per pixel, so a reflection shows the scene and not the sky
         * @note a REPLACEMENT for the specular ambient the lighting stage adds, not an addition: the
         *       estimate falls back to exactly `ibl_specular * F * ao` (the lighting stage's own term, from
         *       the same expressions) wherever a ray finds no geometry, and the spatial filter removes that
         *       term again. So the frame is unchanged wherever the reflection has nothing local to show, and
         *       what changes is the light the reflection actually carries.
         * @note it needs the TRACED path, hit shading and a built instance table - a marched hit is the
         *       depth buffer's own surface and cannot be shaded from its geometry - so it is granted only
         *       where all three hold, and the pass is not even recorded otherwise (which is what makes the
         *       knob-off frame byte-identical by construction rather than by arithmetic).
         * @note how far the rays reach as a fraction of the scene radius
         *        coarse to find anything between them. Measured on Sponza, the lobe's effect is -0.907 at
         *        0.12 (the marched default, i.e. 39% of what is available), -2.126 at 0.5 and -2.350 at
         *        2.0, the cost rising +0.94 ms from the first to the second and not at all after it.
         */

        /**
         * @ingroup vulkan_runtime
         * @note the predicate the frame's record order and the spatial filter's second subtraction are BOTH
         *       subtraction that disagree about whether the pass ran leave the frame wrong by the whole
         *       term, and the two are evaluated in different functions.
         */

        /**
         * @ingroup vulkan_runtime
         * @brief the furnace verification mode ([render] furnace)
         * @param enabled true = the sun is off; the constant-environment half is not implemented yet
         * @note the intent is an analytic reference: with the sun off and the environment a constant level L,
         *       a diffuse surface's outgoing radiance is exactly albedo * L, so a frame with the mode on is
         *       computable by hand and an independent estimate can be checked against it rather than agreed with.
         *       Both lanes are wired: the sun is off, and the environment is the constant cube the frame clears
         *       (see `furnace_cube_ready`).
         */
        void set_furnace(bool enabled) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief hand the runtime a loaded shader, by file name, for the passes that build their own pipelines
         * @param name the file name a pass asks for at create time (a pass's own constant)
         * @param bytecode raw SPIR-V; copied, because the caller's buffer is a local in a startup scope
         * @note THE APP IS THE SHADER LOADER and this is the hand-over: the runtime knows a shader DIRECTORY
         *       and a file format nowhere, and a pass knows neither - it asks for its own by name. Register
         *       before calling create_passes(); a pass whose shader is missing says so and does not run.
         */
        void register_shader(std::string_view name, std::span<uint8_t const> bytecode);

        /**
         * @ingroup vulkan_runtime
         * @brief run every wired pass's CREATE step: what a pass owns, built from its own declaration
         * @note A pass that cannot build what it records with logs why and stays inactive. Idempotent (a pass is
         *       created once per device generation), and it must run after the samplers
         *       the pass context hands over exist. Until it runs, the
         *       passes that own a pipeline are inactive and the frame records exactly what it did before they
         *       existed - which is why a startup failure here is a log line and not a broken frame.
         */
        void create_passes();

        /**
         * @brief re-skin every skinned caster and REFIT its structure, for this frame
         * @param command_buffer where to record (the frame's structure phase, before the top level build)
         * @param casters the structure set's own map, so the request list is built from the same walk that built
         *        the structures (a caster that was skipped there has no entry here and is not skinned)
         * @return whether anything was recorded
         * @note the job writes the vertices the VERTEX shader would compute, into the buffer the structure
         *       was built from, and the refit then makes traversal see them. It has to run AFTER the
         *       animation uploaded this slot's per-joint matrices and BEFORE the frame writes the scene
         *       set's binding 16 - the ordering the mask bake's own-set comment explains.
         */
        bool record_compute_skin_pass(VkCommandBuffer command_buffer, std::span<ray_tracing::caster_level const> casters);
        /// @brief refill the job's request list from the skinned casters the structure set built
        void fill_compute_skin_requests(std::span<ray_tracing::caster_level const> casters);

        /**
         * @ingroup vulkan_runtime
         * @brief name the G-buffer pass's default pipeline is bound under while it records
         * @note the G-buffer pipeline is NOT registered in the runtime's named pipeline cache: it
         *       declares three color attachments, so it can only be used inside the G-buffer
         *       rendering instance (the named cache's pipelines all declare the single HDR target
         *       and are usable in the ordinary forward instance). The G-buffer pass therefore hands
         *       its own pipeline to default-semantics leaves under this name - a leaf with explicit
         *       pipeline semantics still requests its own name and is skipped with a log, because
         *       drawing it here would violate the instance's attachment formats.
         */
        static constexpr std::string_view gbuffer_pipeline_name = "gbuffer";

        /**
         * @brief the name the character-forward pipeline is registered under, which is what its pass binds
         *
         * THE ONE PLACE THE STRING LIVES, and it has to be reachable from three: `chores.cpp` registers the
         * pipeline under it, `make_character_forward_frame` hands it to the pass, and a log or a test that
         * wants to ask "is the toon stage's pipeline present" uses it rather than a literal of its own.
         *
         * UNLIKE `gbuffer_pipeline_name`, this name IS in the runtime's registry (`mesh_pipelines` /
         * `meshlet_pipelines`): the character-forward pass binds it by setting `render_environment::default_name`,
         * and a session that binds by name looks those maps up. The G-buffer pipeline is kept OUT of them
         * because it is only ever the pass's own default, whereas this one has to be findable by the one
         * lookup every forward session already performs.
         */
        static constexpr std::string_view character_forward_pipeline_name = "character_forward";

        /**
         * @brief the name the OVERLAY (multiply) pipeline is registered under, which the character-forward
         *        pass binds for its overlay group
         *
         * THE ARTICLE'S TWO FRAMEBUFFER MULTIPLIES - the eye shadow and the hair shadow - draw with this one
         * pipeline, and the state that makes them overlays rather than surfaces is the pipeline's:
         * `dst = src * dst` (see `core::make_overlay_pipeline`), with the depth test on so each mask lands only
         * on the geometry it was authored over. Registering it by NAME rather than keeping it as the pass's own
         * object follows `character_forward_pipeline_name` exactly, and for the same reason: the pass is handed
         * the name (it does not own a registry) and the session's bind callback resolves it.
         */
        static constexpr std::string_view overlay_pipeline_name = "overlay";

        /**
         * @brief the name the OUTLINE (inverted hull) pipeline is registered under, which the
         *        character-forward pass binds for its outline group
         *
         * THE ARTICLE'S ① 描边 - `MyZmdOutlineShader` - draws with this one pipeline, and the state that makes a
         * hull an outline rather than a surface is the pipeline's: FRONT faces culled (so only the pushed-out
         * back shell survives), depth compare LESS_OR_EQUAL and no blend (see `core::make_outline_pipeline`).
         * Registering it by NAME rather than keeping it as the pass's own object follows
         * `overlay_pipeline_name` exactly, and for the same reason: the pass is handed the name (it does not own
         * a registry) and the session's bind callback resolves it. An empty name means "draw no outline" - the
         * frame this renderer produced before ① landed - which is what the pass reads it as.
         */
        static constexpr std::string_view outline_pipeline_name = "outline";

        /**
         * @brief the name the REWRITTEN TOON CHAIN's fragment stage is registered under (`shaders/goo_toon.slang`)
         *
         * THE SAME PASS, THE SAME FRAME AND THE SAME STATE AS `character_forward_pipeline_name` ABOVE, and that
         * is the whole design: this name differs from it in ONE thing - which `.spv` the fragment stage came from
         * - so switching `[render] goo_toon` swaps the shading model and nothing else. The alternative (a second
         * pass, a second stage, a second target) would have made "the new chain" and "a different frame" the same
         * experiment, and the rewrite's own acceptance is a single-variable A/B.
         *
         * IT IS REGISTERED THROUGH `make_character_forward_pipeline`, WHICH IS WHY NO `core::make_goo_toon_*`
         * EXISTS: that builder's three forced states (ONE HDR target, blending off, depth compare EQUAL, plus the
         * render-extent viewport) are exactly this stage's, so a second core entry point would be a byte-for-byte
         * copy of one state block whose only purpose was to be called from here. The two names are then chosen
         * between in `make_character_forward_frame`, which is also the one place that knows whether this pipeline
         * was built at all.
         *
         * ⚠ DIFFERENT FROM `character_forward_pipeline_name` IN ONE MORE WAY, AND IT IS THE PASS'S: the pass
         * keeps its `feature()` name (`"character_forward"`) either way, so the OUTLINE and OVERLAY stages that
         * hang off that feature keep running unchanged. `goo_toon` selects a SHADING MODEL, not a pass, and this
         * step of the rewrite ports one group of that model; a switch that also turned the pass on would make
         * "the new iris" and "the character stage exists at all" the same variable.
         *
         * ... AND THE SCREEN-SPACE RIM IS THE ONE TOON STAGE THAT DOES *NOT* KEEP RUNNING UNDER IT, which is why
         * it no longer shares this feature name: that pass draws the ARTICLE's contour and the reference the
         * rewrite follows has no such thing, so the two rims would both be on the character. It asks under
         * `"toon_screen_rim"` and is switched off by `goo_toon_active()` (`toon_screen_rim_pass::feature` has the
         * argument, and it is a name this renderer's feature table composes rather than a new `[render]` key).
         */
        static constexpr std::string_view goo_toon_pipeline_name = "goo_toon";

        /**
         * @ingroup vulkan_runtime
         * @brief create the G-buffer pipeline: the deferred path's surface-only fragment stage
         * @param fragment_shader_code raw SPIR-V of gbuffer.frag
         * @param mesh_vertex_shader_code raw SPIR-V of the MESH stage that feeds the surface write: the
         *        G-buffer reuses the forward vertex stage's work (instancing / skinning / morphing /
         *        tangents are identical, only the shading half differs), and it is required because the
         *        vertex form is gone (docs/mesh_shaders.md step 4)
         * @param meshlet_vertex_shader_code the MESHLET form of that stage - one workgroup per meshlet,
         *        culled against the camera before it emits anything; optional, and the pass keeps the
         *        mesh form when it is empty
         * @return success, or an error message on failure
         * @note the pipeline declares the three core::gbuffer_formats targets plus the depth format,
         *       so it is only valid inside a rendering instance with exactly those attachments - the
         *       one set_gbuffer_debug() builds. Register it like any other pipeline (it is NOT the
         *       default: the G-buffer pass binds it explicitly).
         */
        std::expected<void, std::string> make_gbuffer_pipeline(std::span<uint8_t const> fragment_shader_code,
                                                               std::span<uint8_t const> mesh_vertex_shader_code = {}, std::span<uint8_t const> meshlet_vertex_shader_code = {});

        /**
         * @ingroup vulkan_runtime
         * @brief create the samplers the declarations choose between (the G-buffer's NEAREST one and the post
         *        chain's LINEAR one are two of the five the heap carries)
         * @return success, or an error message on failure
         * @note the samplers must exist before create_passes(): the pass context hands every pass the five a
         *       declaration may pick, and the sampler heap is where a shader finds them.
         */
        /** @brief how many jitter positions the Halton(2,3) TAA sequence cycles through */
        static constexpr uint32_t taa_jitter_count = 8;

        /**
         * @brief this frame's SCENE, as the scene pass needs it (see vulkan.pass.scene::scene_frame)
         * @note the leaves, the per-slot secondary buffers and the three callbacks the pass cannot answer for
         *       itself: a fresh draw state per segment, the scheduler, and the attachment formats
         */
        [[nodiscard]] pass::scene_frame make_scene_frame() noexcept;
        /** @brief build ONE segment's draw state (the renderer owns the pipeline registry and the mode) */
        static render_environment make_scene_environment(void* owner, VkCommandBuffer command_buffer, bool gbuffer);
        /** @brief the renderer's scheduler, handed to the pass so the segment fan-out stays the frame loop's */
        static void run_scene_tasks(void* owner, std::span<std::function<void()>> tasks);
        /**
         * @brief what a RESOLVER is given: this frame's resources and this runtime's three lookups
         *
         * The framework's `frame_pass::resolve` default resolves a declaration against `resolve_context`
         * (see deren.vulkan.pass): the resource table published for the frame, the frame identity and command buffer,
         * and the owner's answers for the DECLARATION's own keys - the set index, the extent of a resource
         * element, a pipeline name. One function, because every pass is handed the same thing.
         */
        [[nodiscard]] pass::resolve_context make_resolve_context() noexcept;
        /** @brief the extent of a declared resource element (the bloom levels, the probe grid) - the rule that
         *         used to live in `pass_extent`, now shared with the framework's resolver */
        [[nodiscard]] VkExtent2D resolve_resource_extent(render_resource::resource_id id, uint32_t element) const noexcept;
        /**
         * @brief the pipeline a `behaviour::pipelines` name refers to
         *
         * Two places are asked, in this order: this renderer's own registry (the pipelines the APP registered), and
         * then the CHAIN'S passes, each by name - because a chain's stages may share one pipeline (the post
         * chain's four bloom levels record with the composite's R16F variant). Asking the passes is what keeps this
         * chain-agnostic: no branch here names a pass.
         */
        [[nodiscard]] pass::owned_pipeline resolve_pipeline(std::string_view name) const noexcept;
        /** @brief this frame's blended geometry, as the transparent pass needs it */
        [[nodiscard]] pass::transparent_frame make_transparent_frame() noexcept;
        /**
         * @brief this frame's OPAQUE geometry again, as the character-forward pass needs it
         *
         * The SAME leaf list the scene pass drew (`frame_visible`), because the pass re-shades those surfaces
         * rather than a set of its own: a leaf the scene pass culled has no lit pixel to overwrite, and a leaf
         * it drew but this list omitted would keep the deferred shading while its neighbours did not.
         *
         * WITH ONE EXCEPTION, AND IT IS A CONFIGURED ONE: the frame's STATIC SURROUND (`frame_character`, i.e.
         * `frame_visible` minus `primitive::environment`) is left to the scene pass's own shading, because the
         * toon chain is a character's chain and the `[render] background_glb` ground and backdrop have no
         * material data for it - and no counterpart in the reference, whose ground and dome are ordinary lit
         * and emissive surfaces. See `frame_character`.
         */
        [[nodiscard]] pass::character_forward_frame make_character_forward_frame() noexcept;

        // =============================================================================================
        // THE CHAIN OWNER'S SEAM: the frames ONLY this renderer can build (they
        // carry its own recording machinery), the per-image ordering rules its stages run, and the two callbacks
        // whoever owns the passes implements. Nothing here names a pass's TYPE as a member - the point is that
        // this renderer stops holding one reference per pass and is handed a chain instead - and since the frame
        // composition moved into the passes (`frame_pass::prepare_frame`) the builders below are down to the
        // three whose frames carry the parallel-recording policy.
        // =============================================================================================
        /// the shadow pass's frame: the per-cascade secondaries the structure phase recorded, the map's edge and
        /// the two callbacks. The secondaries are copied into a member so the span the frame carries stays valid
        /// for the whole recording (a local would dangle the moment the builder returned).
        [[nodiscard]] pass::shadow_frame make_shadow_frame() noexcept;
        /// every builder and ordering rule above, bound to this runtime: what `chain_wiring::prepare` is handed
        [[nodiscard]] frame_services make_frame_services(VkCommandBuffer command_buffer) noexcept;
        /// this frame's half of the feature table (see `feature_facts`), which is what the owner's answers are
        /// composed from
        [[nodiscard]] feature_facts make_feature_facts() const noexcept;
        /**
         * @brief the per-frame values only this renderer can compute, for the passes that build their own frames
         *
         * EVERY FIELD IS FILLED WITH THE EXPRESSION THE BUILDER IT REPLACED USED, which is a correctness rule
         * rather than tidiness: `gi_specular` and `debug_view` are COMPOSED answers (the knob AND this frame's
         * structures, the chain owner's own feature table), and the feature facts carry knobs of the same names.
         *       top level structure, which the structure phase REBUILDS during the frame - a value computed at
         *       `pace_and_acquire` would be the previous frame's answer. This is the same timing trap
         *       `frame_constants::gi_instance_table` is filled late for.
         */
        [[nodiscard]] pass::frame_facts make_frame_facts() const noexcept;
        /// run the owner's `prepare` for one stage, or do nothing when no owner is wired (see `chain_wiring`)
        /// @note this is also where every pass in @p stage builds ITS OWN frame (see frame_pass::prepare_frame),
        ///       BEFORE the owner's `prepare` - so an owner that still wants to override a frame can
        void prepare_stage(pass::stage const& stage, VkCommandBuffer command_buffer);
        /// run the owner's `collect` for one stage and apply what it reports (see `frame_results`)
        void collect_stage(std::string_view stage);
        /// clear one image's "the G-buffer instance wrote the motion-vector target" flag, so the next sampler of
        /// that image publishes it (what a stage whose pass samples it after TAA already moved it must do)
        void require_velocity_publish(uint32_t image_index);
        /// the per-cascade secondaries a shadow frame's span points at (see make_shadow_frame)
        std::array<VkCommandBuffer, deren::vulkan::max_shadow_cascades> shadow_secondaries_scratch = {};
        /// whoever owns this frame's passes; empty until `set_chain_wiring` is called, and a frame with no wiring
        /// gives its passes no frames - which is what makes the seam's absence visible rather than silent
        /// named `frame_wiring` rather than `wiring`: `set_chain_wiring`/`set_pass_chain` take a
        /// `chain_wiring wiring`, and a member of that name would be hidden by the parameter - MSVC /W4 reports C4458, which /WX makes an error
        chain_wiring frame_wiring = {};
        /**
         * THE FACTS PUBLISHED TO THE PASSES FOR THE STAGE ABOUT TO RECORD (see make_frame_facts and
         * `frame_pass::prepare_frame`).
         *
         * NOT named `frame_facts`, although that is its type: this class already has a `frame_facts` member and
         * it is the frame CONSTANTS (see `frame_constants frame_facts` above) - one underscore apart from the
         * published facts would be a trap for whoever reads either name next.
         */
        pass::frame_facts stage_facts = {};

        /**
         * @ingroup vulkan_runtime
         * @brief enable/disable temporal anti-aliasing
         * @param enabled when true (and the deferred path is the active render mode) the projection is
         *        jittered every frame, the G-buffer's motion vectors are resolved against a reprojected
         *        history, and the result is what the post chain processes. This is the deferred path's
         *        anti-aliasing: it resolves sub-pixel detail no edge filter can, AND the
         *        shimmer in motion that no edge filter can remove. The pass builds its own pipeline
         *        (deren.vulkan.pass.taa); without it the flag has no effect.
         * @note the pass's two history weights are its own, set through `taa_pass::set_blend()`: a pixel
         *       that did not move blends 0.9 of the history (10% of the current frame per frame; higher
         *       converges smoother but reacts slower to lighting changes), and one that moves a pixel or
         *       more per frame falls to the floor - lower trusts the current frame more under motion,
         *       which trades smoothing for less ghosting
         * @note there is no PER-OBJECT motion vector to clamp against yet, only the camera:
         *       fix for it. The G-buffer motion vectors are camera-only at this milestone, so a
         *       deformed (skinned/morphed) object can ghost slightly - see gbuffer.frag.
         */
        bool set_taa_enabled(bool enabled) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief set the shadow map's edge length in texels ([render] shadow_map_size)
         * @param size requested edge length; clamped to 256..8192 and rounded to a power of two
         * @note STARTUP-ONLY, like set_shadow_cascades: the layered image, its per-layer views, its
         *       heap slot, the depth pass's rendering instance and the shadow pipeline's viewport
         *       are all created from it, so it must be called before the scene import (the resources
         *       are created lazily by the first primitive that asks for the scene). A later call is ignored with a log line
         *       rather than silently taking effect on the next resize.
         */
        void set_shadow_map_size(uint32_t size) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief set how many shadow cascades to fit, render and sample
         * @param cascades 1 (one box over the whole visible range, the historic behavior) up to
         *        deren::vulkan::max_shadow_cascades; clamped
         * @note call it BEFORE the scene is imported (the resources exist by then and the cascade
         *       count only changes which layers are used). The split scheme is a practical
         *       logarithmic/uniform blend, so cascade 0 gets a fraction of the far cascade's texel
         *       size - the whole reason a single map cannot serve near and far at once.
         */
        void set_shadow_cascades(uint32_t cascades) noexcept;

        /** @brief set the cascade blend band (0 disables blending; 0.1 = the last 10% of a cascade) */
        void set_shadow_cascade_blend(float blend) noexcept;

        /** @brief whether TAA is enabled (see set_taa) */
        [[nodiscard]] bool taa() const noexcept {
            return this->taa_on;
        }

        /**
         * @ingroup vulkan_runtime
         * @brief draw the scene's opaque geometry into the G-buffer and show a debug view of it
         *        instead of the shaded image
         * @param enabled when true the opaque pass records into the three G-buffer targets (1x) and
         *        a fullscreen pass visualizes one channel into the HDR target, which the post chain
         *        then processes as usual. Without a pipeline from make_gbuffer_pipeline() +
         *        make_gbuffer_debug_pipeline() the flag has no effect (gbuffer_pass_active() is false
         *        and no scene is drawn at all). The debug view wins over the lighting stage:
         *        inspecting the stored data is not a render mode.
         * @note alphaMode BLEND geometry is skipped in this mode: it is composited over a SHADED
         *       image, and the debug view is not one
         */
        void set_gbuffer_debug(bool enabled) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief log which optional features are actually available this session
         *
         * One line naming every optional pipeline that could NOT be created (deferred lighting,
         * G-buffer debug view, TAA, FXAA, shadow pass, skybox, clustered light culling). Those
         * features are toggled from the debug overlay and from the config, and a toggle whose
         * pipeline is missing does nothing at all - silently, which reads as "this switch is
         * broken". Call it once after the pipelines are created (main does) so the log answers
         * that question up front; `warn_missing_feature` additionally reports a toggle that
         * cannot take effect at the moment it is switched on.
         */
        void log_feature_status() const;

        /**
         * @ingroup vulkan_runtime
         * @brief whether an optional feature is usable this session (its pipeline was created)
         * @param name one of "deferred", "gbuffer-debug", "taa", "fxaa", "shadow", "skybox",
         *        "clustered" - unknown names return false
         *
         * The debug overlay asks this to decide what to offer (see deren::vulkan::gui::widget::visible_when):
         * a control whose pipeline does not exist can never do anything, so it is hidden instead of
         * being shown inert. log_feature_status() prints the same information once at startup.
         */
        [[nodiscard]] bool feature_available(std::string_view name) const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief which render features actually RUN this frame
         *
         * One declared place for "what exists, what it needs, and is it on" - every field is derived
         * from the config/GUI state plus the pipelines that were created, so nothing can disagree.
         * It is the input to pass recording (the shadow pass and the cluster dispatch are skipped
         * when no shading stage would read their output) and to `feature_active()`, which the debug
         * overlay uses to decide what to offer and the log uses to report what ran.
         *
         * The gates that are NOT just "the switch is on" are the interesting ones, and each carries
         * its reason:
         *  - shadow: skipped in the flat (unlit) render mode - nothing samples the map there, so the
         *    pass is pure waste (measured: 0.22 ms, ~45% of a 0.5 ms frame).
         *  - clustered: skipped when no punctual light is active (nothing to sort) and in unlit mode.
         *  - taa/ssao: they consume the G-buffer, so they need the G-buffer pass to have run.
         *  - bloom: off with an intensity of 0 or while the G-buffer debug view is up.
         *  - transparent: needs alpha-blended leaves to have been culled, and an instance to composit
         *    them into (so not the debug view, which shows the G-buffer and not a frame).
         */
        struct render_features {
            bool unlit = false;         // the flat render mode (no lighting anywhere)
            bool gbuffer_debug = false; // the opaque pass stores the G-buffer for the debug view
            bool shadow = false;        // record the directional shadow pass
            bool rt_shadow = false;     // record the ray-traced shadow pass (the knob, ray queries, its pipeline)
            bool clustered = false;     // record the cluster compute pass
            bool taa = false;           // resolve TAA
            bool ssao = false;          // the lighting stage applies screen-space AO (shader-side gate)
            bool bloom = false;         // run the bloom chain
            bool fxaa = false;          // run the final FXAA pass
            bool upscale = false;       // resolve the render chain up to the output (post_upscale_active)
            bool transparent = false;   // the transparent pass has work to record
        };
        [[nodiscard]] render_features active_features() const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief the last completed CPU phase window, as the overlay label shows it
         * @return one line per phase (pace / begin / scene / post / submit) with fixed-width
         *         millisecond fields, updated once per 60-frame window - the CPU counterpart of
         *         gpu_timing_summary(), and the instrument for the range where the frame is no
         *         longer GPU-bound (see cpu_phase)
         */
        [[nodiscard]] std::string cpu_timing_summary() const;

        /**
         * @ingroup vulkan_runtime
         * @brief whether a feature is available AND switched on right now (name-keyed)
         * @param name "gbuffer-debug", "taa", "fxaa", "shadow", "clustered",
         *        feature_available()
         *
         * This is what the overlay gates its controls on (`deren::vulkan::gui::widget::visible_when`): a
         * control is offered exactly when switching it could change the frame. feature_available()
         * answers the different question "could this ever run this session", which is what the
         * startup log reports.
         */
        [[nodiscard]] bool feature_active(std::string_view name) const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief one captured frame image: tightly packed 8-bit RGBA (top-left origin, row-major)
         */
        struct frame_image {
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<uint8_t> rgba = {}; // width * height * 4, R G B A
        };

        /**
         * @ingroup vulkan_runtime
         * @brief read back the frame captured by the last screenshot request
         * @return the captured image (8-bit RGBA, swizzled from the swapchain format), or an error
         *         string when nothing was captured / the swapchain format is unsupported
         * @note the pixels were already copied GPU-side while the frame was recorded - since S2 batch 2 by
         *       the contract's copy_image_to_buffer() into the BACKEND's read-back slot (see
         *       end_recording in runtime.frames.cppm) - so this only waits for the device to idle, reads
         *       the slot's mapping and swizzles: no transition of a presentable image, because the
         *       presentation engine owns it by then. Still a low-frequency
         *       debug feature: expect a visible hitch, do not call per frame. Requires a 4-channel
         *       8-bit swapchain format (BGRA/RGBA, sRGB or UNORM); the sRGB encoding is preserved,
         *       so the PNG matches what was on screen.
         */
        std::expected<frame_image, std::string> acquire_current_frame_image();

        /**
         * @ingroup vulkan_runtime
         * @brief ask whether a frame was captured for the pending screenshot request
         * @return true when the read-back copy has been recorded and is ready to be read by
         *         acquire_current_frame_image() (which consumes it) - so the caller's pattern stays
         *         `if (consume_screenshot_request()) { acquire_current_frame_image(); ... }`
         * @note the request itself (F12 edge in poll_events, or request_screenshot()) is consumed
         *       during recording: end_recording() records the copy into that frame's own command
         *       buffer while the swapchain image still belongs to it. A request whose frame could
         *       not be recorded (zero-sized swapchain) stays pending for a later frame.
         */
        [[nodiscard]] bool consume_screenshot_request() noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief request a screenshot without the F12 key (scripted / automatic captures)
         * @note sets exactly the request that the F12 edge trigger sets, so the caller still
         *       consumes it through consume_screenshot_request() and owns the read-back + save;
         *       lets a headless-ish run (e.g. `--capture-frames`) capture a frame with no human
         *       at the keyboard
         */
        void request_screenshot() noexcept {
            this->screenshot_requested = true;
        }

        /**
         * @ingroup vulkan_runtime
         * @brief set the active punctual lights (point/spot, pbr.frag's direct-light loop).
         *        Each light is evaluated through the same BRDF path as the directional sun
         *        (inverse-square falloff, optional smooth range cutoff, spot cone mask) and
         *        never casts a shadow in this version.
         * @param lights the lights to enable. Every `deren::vulkan::punctual_light` field is honored:
         *        `position`, linear `color` (radiance = color * intensity), `range` (0 =
         *        infinite falloff, otherwise a smooth cutoff at this distance), and for spot
         *        lights `spot = true` + `spot_direction` + `spot_outer_cos` (cos of the outer
         *        half-angle; the shader derives the soft inner cone as mix(outer, 1, 0.6)).
         *        Entries beyond `deren::vulkan::max_punctual_lights` (128) are dropped.
         * @note same timing rule as set_brdf_model: CPU-side only, copied into the paced
         *       slot's buffer every frame, so safe at any time (GUI included). The demo GUI edits
         *       four slots ONE AT A TIME - a `punctual light` combo picks the slot and the group
         *       below draws only that one (enable + position / color / intensity / range, plus
         *       direction / inner and outer cone when the slot is a spot) - so a spot light is
         *       settable from the panel as well as by a programmatic caller.
         */
        void set_point_lights(std::span<punctual_light const> lights) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief set the live depth bias of the directional shadow pass (applied every frame via
         *        vkCmdSetDepthBias before the depth-only draw)
         * @param constant_factor fixed depth bias added to every fragment's depth
         * @param slope_factor slope-scaled bias (depth units per depth-unit of surface slope) -
         *        the main acne control for angled surfaces
         * @param clamp maximum depth bias magnitude (0 = no clamp)
         * @note cheap: only changes the value recorded into the command buffer, no pipeline or
         *       resource rebuild; useful to chase shadow acne / peter-panning per model
         */
        void set_shadow_depth_bias(float constant_factor, float slope_factor, float clamp) noexcept {
            this->shadow_depth_bias_constant = constant_factor;
            this->shadow_depth_bias_slope = slope_factor;
            this->shadow_depth_bias_clamp = clamp;
        }

        /**
         * @ingroup vulkan_runtime
         * @brief mark the scene tree as changed (structure or per-node local transforms edited
         *        through get_scene(), e.g. programmatic animation): the culling BVH is rebuilt on
         *        the next frame. Internal scene mutations (import / make / clear)
         *        invalidate automatically.
         */
        void scene_changed() noexcept {
            this->bvh_dirty = true;
        }

        /**
         * @ingroup vulkan_runtime
         * @brief print the scene tree (names + local-transform marker + leaf primitive pipeline)
         *        to the log, one indented line per node, plus a shape summary
         * @note diagnostic helper: shows whether an import rebuilt the real hierarchy (gltf
         *       node names and nesting) or a flat list of root leaves (pipeline names)
         */
        void log_scene_tree() const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief bind the scene tree this runtime renders. The tree is owned by the caller
         *        (never by the runtime): it must stay alive while the runtime is in use and be
         *        destroyed before the runtime goes away, because the leaves' GPU buffers are
         *        released through the runtime's vma allocator when the tree is destroyed.
         * @param scene the caller-owned scene tree to render
         * @note bind before creating/importing primitives and before the first frame; the
         *       culling BVH is invalidated so the next frame rebuilds it
         */
        void set_scene(scene_tree::scene& scene) noexcept {
            this->bound_scene = &scene;
            this->bvh_dirty = true; // a new tree's world AABBs must be (re)collected
        }

        /**
         * @ingroup vulkan_runtime
         * @brief access the scene tree (roots + children + per-node local transforms) for
         *        programmatic whole-group / subtree transforms
         * @note the tree structure is fixed after import (no reallocation of scene or the
         *       children vectors while nodes are only edited in place), so pointers/references
         *       into the tree stay valid until the next make_primitive / import / clear call
         */
        [[nodiscard]] scene_tree::scene& get_scene() noexcept {
            if (this->bound_scene == nullptr) {
                deren::utility::panic("runtime::get_scene() called before set_scene() bound a scene");
            }
            return *this->bound_scene;
        }
        [[nodiscard]] scene_tree::scene const& get_scene() const noexcept {
            if (this->bound_scene == nullptr) {
                deren::utility::panic("runtime::get_scene() called before set_scene() bound a scene");
            }
            return *this->bound_scene;
        }

        /**
         * @ingroup vulkan_runtime
         * @brief get a cached pipeline by its name
         * @param pipeline_name the name passed to make_pipeline()
         * @return pointer to the cached pipeline, or nullptr if no pipeline with that name exists
         */
        [[nodiscard]] vk_pipeline const* get_pipeline(std::string_view pipeline_name) const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief upload the scene-wide IBL resources (prefiltered env / irradiance / BRDF LUT)
         *        into the frame's heap; call it before creating models that use IBL
         * @param info precomputed split-sum IBL bytes (see deren::vulkan::generate_* helpers)
         * @note the images are uploaded once and shared by every primitive (they used to be
         *       duplicated per primitive)
         * @note call before the first frame, or only while the runtime is idle (no frame in
         *       flight): the heap slots for the three cubes are rewritten, and a frame in flight may be
         *       reading them - a heap write has no update-after-bind notion to make that legal. Mid-loop
         *       IBL swaps need wait_idle() first.
         */
        void set_ibl(ibl_input const& info);

        /**
         * @brief upload the ARTICLE'S POST LUT (`ZmdLutPost.shader`'s `_LutTex`) into its heap slot
         *
         * A `width x height` R8G8B8A8_SRGB image, uploaded once, exactly as `set_ibl` uploads the BRDF LUT - and for
         * the same reason the two are separate entry points rather than fields of one info struct: the IBL arrives
         * from the async generators while this arrives from the application's own bake (`bake_post_lut` in main.cpp),
         * which is a different producer with a different lifetime.
         *
         * THE CONTENT IS THE IDENTITY, and that is what makes the port's LUT weight default safe: the shader's
         * addressing is the article's log-domain encoding, so a cube holding `decode(address)` at every texel reads
         * back as the colour it was looked up with - a frame with the weight at 1 and no artist's cube is the frame
         * without any LUT at all, which is a measurement the capture gate can check.
         */
        void set_post_lut(std::span<uint8_t const> pixels, uint32_t width, uint32_t height);

        /**
         * @ingroup vulkan_runtime
         * @brief upload the GOO REFERENCE'S PRE-INTEGRATED FGD LUT into its own heap slot
         *
         * A `width x height` R8G8B8A8_**UNORM** image, uploaded once from the reference's own PNG
         * (`deren-ab/gooblender/images/PreIntegratedFGD_GGXDisneyDiffuse.png`, 64x64, 5234 B). The step-5 spec's
         * §3.4 ruling is why it is a SHARED GLOBAL image rather than a `toon_slot` lane: the reference's FGD group
         * holds one `ShaderNodeTexImage`, three containers share that data-block, and the sample coordinate is
         * computed from shading parameters - so per-material lanes would be eleven rows of sidecar pointing at one
         * file, i.e. the full lane cost for zero information. See `core::heap_slots::goo_fgd_lut`.
         *
         * UNORM AND NOT `_SRGB`, which is the one thing about this upload that is easy to get wrong and changes the
         * picture rather than the format field: the data-block's `colorspace` is `'Non-Color'` (every other
         * `_RD`/`_D` image in this project is `'sRGB'`), so Blender hands the node graph the texel's RAW bytes and
         * an sRGB upload would decode all three of `specularFGD` / `diffuseFGD` / `reflectivity` once (spec §3.1).
         */
        void set_goo_fgd_lut(std::span<uint8_t const> pixels, uint32_t width, uint32_t height);

        /**
         * @ingroup vulkan_runtime
         * @brief upload the scene-wide skin matrices (scene block slot 9) into the frame slot
         *        paced by the last pace_and_acquire(): the caller fills the buffer layout
         *        [identity block (4 mat4s) | per-skin joint blocks] and calls this once per frame
         *        AFTER pace_and_acquire(); primitives reference their block start via
         *        material_push_constants::skin_base (0 = the identity block: unskinned draws)
         * @param matrices at most scene_skin_capacity mat4s; anything beyond the capacity is dropped
         * @note host-visible copy, no Vulkan objects involved; the per-slot buffers guarantee an
         *       in-flight frame never shares the buffer being rewritten
         */
        void set_skin_matrices(std::span<glm::mat4 const> matrices);

        /**
         * @ingroup vulkan_runtime
         * @brief like set_skin_matrices() but into an explicit slot buffer (used for setup-time
         *        uploads that must be visible to every slot, e.g. the identity block before the
         *        render loop starts)
         */
        void set_skin_matrices(std::span<glm::mat4 const> matrices, uint32_t slot);

        /**
         * @ingroup vulkan_runtime
         * @brief host-visible scratch memory of the ACTIVE frame slot's morph buffer (scene set
         *        binding 10, scene_morph_capacity floats each). The caller lays out per-primitive
         *        morph blocks (per vertex per target pos-delta/nrm-delta floats, then the
         *        per-target weights, then the per-target PREVIOUS weights) and points primitives at
         *        their block through material_push_constants::morph_base / morph_targets /
         *        morph_vertices (see those fields for the layout convention).
         * @note THE TWO WEIGHT REGIONS ARE A CONTRACT, and the second one is why a morphing mesh has a
         *       motion vector at all: the vertex stage reads the block's FIRST weight region as this
         *       frame's weights and the SECOND as the weights one frame ago, so a caller that rewrites
         *       weights per frame must copy the first region into the second BEFORE overwriting it -
         *       otherwise its mesh reports a morph deformation that is not happening. A caller that sets
         *       weights once (at setup) has nothing to do: animation::controller::update, which is the
         *       only per-frame writer this renderer has, bakes both regions equal and then keeps them in
         *       step. See docs/deformation_motion_vectors.md.
         * @return the mapped base of the slot paced by the last pace_and_acquire(), or nullptr when
         *         the morph buffer is unavailable
         */
        [[nodiscard]] void* morph_scratch() noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief like morph_scratch() but for an explicit slot buffer (used for setup-time bakes
         *        that must be duplicated into every slot's buffer before the render loop starts)
         */
        [[nodiscard]] void* morph_scratch(uint32_t slot) noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief create a primitive and attach it to the scene tree as a new leaf (root node).
         * @param pipeline_name the pipeline the primitive draws with (must already exist)
         * @param info geometry and material textures
         * @return pointer to the created primitive (owned by the scene tree), or nullptr if the
         *         pipeline does not exist
         * @note the leaf's local transform is @p info.model_matrix and its world is identity
         *       (the frame record phase runs update_world before drawing, so primitive::set_world writes
         *       the same matrix into push.model as before)
         * @note call before the first frame, or only while the runtime is idle (no frame in
         *       flight): registering a material appends texture entries to the heap's bindless array, and a
         *       write landing where an in-flight frame may be reading is a hazard the heap has no
         *       update-after-bind rule to make legal. Mid-loop creation needs wait_idle()
         *       first.
         */
        primitive* make_primitive(std::string_view pipeline_name, primitive_create_info const& info);

        /**
         * @ingroup vulkan_runtime
         * @brief collect every leaf primitive of the given pipeline (DFS over the scene tree)
         * @param pipeline_name the pipeline name passed to make_primitive()
         * @return models whose leaf node name matches @p pipeline_name, in scene-tree order
         */
        [[nodiscard]] std::vector<primitive const*> get_primitives(std::string_view pipeline_name) const noexcept;

        /**
         * @ingroup vulkan_runtime
         * @brief append an instanced_draw_primitive: draws @p source's geometry once per transform
         *        in ONE draw call per frame (per-instance matrices in scene block slot 6)
         * @param source any primitive of this runtime (its geometry is drawn transforms.size() times;
         *        it must stay in the runtime's primitive list while the instanced primitive is drawn)
         * @param transforms one world matrix per instance (fully places the source geometry)
         * @return pointer to the appended instanced primitive, or nullptr if nothing was appended
         */
        primitive* make_instanced_primitive(primitive const& source, std::span<glm::mat4 const> transforms);

        /**
         * @ingroup vulkan_runtime
         * @brief batch-import a scene by traversing the retained node hierarchy (structural
         *        node stream) and its drawables (geometry stream) together.
         *        @p nfirst must model deren::vulkan::scene_node_iterator: DFS pre-order over every
         *        scene node INCLUDING transform-only nodes, exposing get_name() /
         *        get_local_transform() / get_depth() / get_drawable_count(). @p dfirst must
         *        model deren::vulkan::scene_drawable_iterator (++ plus geometry/material getters);
         *        the loader keeps both streams over the same pool in the same order, so each
         *        node's get_drawable_count() drawables are the next entries of the drawable
         *        stream. The runtime drives the traversal: it rebuilds the node tree into
         *        scene_tree::scene (one scene_node per loader node, named, with its local
         *        transform; a node's drawable becomes a primitive leaf attached to that node —
         *        extra primitives of one node become identity-local child leaves), uploads
         *        buffers and registers materials. No glTF (or any scene format) knowledge
         *        lives in the runtime.
         * @param nfirst,nlast iterator pair over the scene's node hierarchy
         * @param dfirst,dlast iterator pair over the scene's drawables
         * @param offset translation applied to every scene ROOT node's local transform
         *        (e.g. -scene_center + sink); children inherit it through update_world
         * @param environment true when this import IS the frame's static surround rather than a subject
         *        (the application's second import of `[render] background_glb`): every leaf created here
         *        is flagged `primitive::environment` and is then skipped by the toon character stage and
         *        left out of the shadow casters, while the scene pass draws it as usual. Default false,
         *        i.e. the subject's own import is untouched. See `primitive_create_info::environment`.
         * @return counts of imported primitives and materials
         * @note call before the first frame, or only while the runtime is idle (no frame in
         *       flight): importing registers materials, which appends texture entries to the heap's
         *       bindless array, and a write landing where an in-flight frame may be reading is a
         *       hazard the heap has no update-after-bind rule to make legal. Mid-loop imports
         *       need wait_idle() first.
         */
        template <class NI, class DI>
            requires scene_node_iterator<NI> && scene_drawable_iterator<DI>
        scene_import_result import_scene(NI nfirst, NI nlast, DI dfirst, DI dlast, glm::vec3 const& offset, bool const environment = false) {
            if (this->bound_scene == nullptr) {
                deren::utility::panic("runtime::import_scene() called before set_scene() bound a scene");
            }
            scene_import_result result = {};
            uint32_t const materials_before = this->material_count;
            // converts a pure image_source (e.g. the glTF loader's image_view) into the
            // internal texture_input with the slot's upload format; invalid images -> white
            auto const to_texture = [](auto const& image, VkFormat const format) {
                texture_input out = {};
                if (image.valid) {
                    out.data = image.data;
                    out.width = image.width;
                    out.height = image.height;
                    out.mip_levels = image.mip_levels;
                    out.format = format;
                    out.valid = true;
                }
                return out;
            };
            // per-node drawable -> primitive_create_info (reads the next drawable of the stream)
            auto const fill_info = [&](DI& drawable, primitive_create_info& info) {
                auto const vertex = drawable.get_vertex();
                auto const index = drawable.get_index();
                info.vertex_data = vertex.data;
                info.vertex_stride = vertex.stride;
                info.vertex_count = vertex.count;
                info.index_data = index.data;
                info.index_type = index.width == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
                info.index_count = index.count;
                info.albedo = to_texture(drawable.get_albedo(), VK_FORMAT_R8G8B8A8_SRGB);
                info.metallic_roughness = to_texture(drawable.get_metallic_roughness(), VK_FORMAT_R8G8B8A8_UNORM);
                info.normal = to_texture(drawable.get_normal(), VK_FORMAT_R8G8B8A8_UNORM);
                info.occlusion = to_texture(drawable.get_occlusion(), VK_FORMAT_R8G8B8A8_UNORM);
                info.emissive = to_texture(drawable.get_emissive(), VK_FORMAT_R8G8B8A8_SRGB); // glTF emissive textures are sRGB
                auto const factors = drawable.get_factors();
                info.factors.base_color_factor = factors.base_color_factor;
                info.factors.emissive_factor = factors.emissive_factor;
                info.factors.metallic_factor = factors.metallic_factor;
                info.factors.roughness_factor = factors.roughness_factor;
                info.factors.normal_scale = factors.normal_scale;
                info.factors.occlusion_strength = factors.occlusion_strength;
                info.factors.alpha_cutoff = factors.alpha_cutoff;
                info.factors.alpha_mask = factors.alpha_mask;
                info.factors.alpha_blend = factors.alpha_blend;
                info.double_sided = drawable.get_double_sided();
                // THE TOON FAMILY, resolved by the loader from the material's NAME (the last place the name
                // exists). Copied here like every other per-material fact so the runtime never sees a string.
                info.toon_family = drawable.get_toon_family();
                // ... AND THE OVERLAY CHANNEL, resolved by the loader from the same name and at the same
                // moment, so the runtime never sees a string here either. A non-zero value takes this leaf out
                // of every shading pass and into the overlay one (see `primitive::overlay_kind`).
                info.overlay_kind = drawable.get_overlay_kind();
                // ---- THE TOON MAPS, from the installed lookup (see set_toon_lookup) ----
                // The runtime asks by NAME because the sidecar is keyed by name; it never sees the sidecar.
                // A null lookup leaves `info.toon` empty, which is what a model with no sidecar gets: an empty
                // block rather than a block of white, so a shader finds nothing switched on.
                if (this->toon_lookup_source.texture != nullptr || this->toon_lookup_source.colour != nullptr || this->toon_lookup_source.scalar != nullptr) {
                    std::string_view const material_name = drawable.get_material_name();
                    for (uint32_t lane = 0; lane < static_cast<uint32_t>(toon_slot::count); ++lane) {
                        if (this->toon_lookup_source.texture != nullptr) {
                            info.toon.slots[lane] = this->toon_lookup_source.texture(this->toon_lookup_source.owner, material_name, static_cast<toon_slot>(lane));
                        }
                    }
                    // THE COLOURS, from the same name and the same sidecar: a material with no such row keeps the
                    // neutral white `toon_inputs` gave it, which is what "the game states no colour here" has to
                    // look like for a multiply-tint.
                    for (uint32_t lane = 0; this->toon_lookup_source.colour != nullptr && lane < static_cast<uint32_t>(toon_colour_lane::count); ++lane) {
                        info.toon.colours[lane] = this->toon_lookup_source.colour(this->toon_lookup_source.owner, material_name, static_cast<toon_colour_lane>(lane));
                    }
                    // THE ARTICLE'S TRANSPARENT VARIANT, from the sidecar's own blend pair - see
                    // `toon_inputs::alpha_blend` and `toon_lookup::scalar`. `5` / `10` are Unity's `SrcAlpha` /
                    // `OneMinusSrcAlpha`, which is the pair every material in this repository states when it
                    // states one; anything else is left as the opaque overwrite rather than approximated,
                    // because a blend state this port cannot evaluate is not a blend state it should guess.
                    if (this->toon_lookup_source.scalar != nullptr) {
                        float const src = this->toon_lookup_source.scalar(this->toon_lookup_source.owner, material_name, "_SrcBlend", 1.0f);
                        float const dst = this->toon_lookup_source.scalar(this->toon_lookup_source.owner, material_name, "_DstBlend", 0.0f);
                        info.toon.alpha_blend = src == 5.0f && dst == 10.0f;
                    }
                }
            };
            // attach one leaf primitive to @p node (geometry from the next drawable of the stream);
            // returns the created primitive or nullptr if the pipeline is missing
            auto const attach_leaf = [&](scene_tree::scene_node& node, DI& drawable) -> primitive* {
                if (!(drawable != dlast)) {
                    deren::utility::panic(std::source_location::current(), "drawable stream ended before the node tree did");
                }
                primitive_create_info info = {};
                fill_info(drawable, info);
                // WHETHER THIS IMPORT IS THE FRAME'S SURROUND: one flag for the whole import, stamped on every
                // leaf it creates (see the argument's note). It is a property of the CALL and not of the
                // drawable, which is why the loader has nothing to say about it.
                info.environment = environment;
                ++drawable;
                ++result.primitive_count;
                std::unique_ptr<primitive> created = this->create_primitive("pbr", info);
                if (created == nullptr) {
                    return nullptr;
                }
                if (node.primitive_leaf == nullptr) {
                    node.primitive_leaf = std::move(created);
                } else {
                    // a glTF node can carry several primitives; scene_node has one leaf slot, so
                    // extra primitives become identity-local child leaves (world unchanged)
                    scene_tree::scene_node extra;
                    extra.node_name = node.node_name + "/prim";
                    extra.primitive_leaf = std::move(created);
                    node.children.push_back(std::move(extra));
                }
                return static_cast<primitive*>(node.primitive_leaf ? node.primitive_leaf.get() : node.children.back().primitive_leaf.get());
            };

            // DFS over the loader's node stream, rebuilding parent/child edges with an explicit
            // stack of ancestors: ancestors[d] holds the scene_node at depth d on the path to
            // the current node. The loader emits nodes in DFS pre-order, so when a node arrives
            // at depth d its parent is the ancestor at depth d-1 (pop everything deeper first).
            std::vector<scene_tree::scene_node*> ancestors = {}; // ancestors[d] = node at depth d
            for (; nfirst != nlast; ++nfirst) {
                std::size_t const depth = nfirst.get_depth();
                // pop ancestors deeper than the arriving node's parent level (their subtrees are done)
                while (ancestors.size() > depth) {
                    ancestors.pop_back();
                }
                scene_tree::scene_node node;
                node.node_name = std::string(nfirst.get_name());
                node.source_index = nfirst.get_source_index(); // asset node index (e.g. glTF): animation targets map onto the tree through it
                node.local = nfirst.get_local_transform();
                if (depth == 0) {
                    node.local = glm::translate(glm::mat4(1.0f), offset) * node.local; // scene root gets the offset
                }
                // attach under the parent (depth-1) or as a new scene root
                if (depth == 0) {
                    this->bound_scene->roots.push_back(std::move(node));
                    ancestors.assign(1, &this->bound_scene->roots.back());
                } else {
                    if (ancestors.size() != depth) {
                        deren::utility::panic(std::source_location::current(), "node tree stream: broken ancestor stack");
                    }
                    scene_tree::scene_node* const parent = ancestors[depth - 1];
                    parent->children.push_back(std::move(node));
                    ancestors.resize(depth + 1);
                    ancestors[depth] = &parent->children.back();
                }
                scene_tree::scene_node* const current = ancestors[depth];
                // consume this node's drawables (the stream is aligned node-for-node)
                std::size_t const drawable_count = nfirst.get_drawable_count();
                for (std::size_t i = 0; i < drawable_count; ++i) {
                    if (attach_leaf(*current, dfirst) == nullptr) {
                        deren::utility::panic(std::source_location::current(), "failed to import drawable (pipeline 'pbr' missing)");
                    }
                }
            }
            this->bvh_dirty = true; // new leaves attached -> culling BVH must be rebuilt
            result.material_count = this->material_count - materials_before;
            // WHAT THE SCENE CUT INTO (docs/mesh_shaders.md step 3): the meshlets a task stage would cull, and how
            // many primitives contributed them. Logged once per import, where the whole scene's numbers exist.
            if (this->meshlet_total != 0u) {
                deren::utility::log("meshlets: {} over {} primitives ({} triangles each at most, object-space spheres)",
                                    this->meshlet_total,
                                    this->meshlet_primitives,
                                    deren::vulkan::meshlet_max_triangles);
            }
            return result;
        }
    };

    /**
     * @brief THE ONE SPELLING OF A CONVERTED CREATION SITE'S DESCRIPTOR: the size, the memory intent, the
     *        capabilities, and the bytes that ARE the upload.
     *
     * `initial_bytes` non-empty means "upload these at creation" and empty means "allocate only" - the
     * contract's own rule - and it is why the sites that keep a host mapping pass an EMPTY span: the
     * content is what the CALLER then writes through `mapped()`, which is not something the descriptor can
     * know. @p size smaller than @p initial_bytes is refused (a panic) rather than truncated into a buffer
     * nobody asked for.
     */
    [[nodiscard]] rhi::buffer_desc make_buffer_desc(std::uint64_t size, rhi::buffer_usage usage, rhi::buffer_flags flags, std::span<std::byte const> initial_bytes) noexcept;

    /**
     * @brief create ONE contract buffer, panic on refusal, and hand back its mapping.
     * @param device the core whose CONTRACT FACE creates it (see the note on the definition: the factory is
     *        called through `rhi::api_core&`, never through the concrete class, or the call would emit a
     *        backend symbol into this half)
     * @param what what the resource is called in the failure log
     * @param output receives the OWNER of the reference the factory hands over (the manager releases it on
     *        every path out, which is the whole reason it is the output type rather than a bare handle)
     * @param mapped receives `mapped().data()`, or nothing when the pointer is null (the cluster index rows,
     *        which only the GPU ever fills)
     * @note panics when the factory refuses - the same failure mode `init_utils::create_host_buffer` had
     */
    void create_buffer(core& device, rhi::buffer_usage usage, rhi::buffer_flags flags, std::span<std::byte const> initial_bytes, std::string_view what, rhi::object_manager<rhi::buffer>& output, void** mapped);

    /**
     * @brief the per-frame-slot form: create ONE buffer per frame slot of a shape the whole family shares,
     *        appending them to @p outputs and collecting their mappings in slot order.
     * @param outputs the family; APPENDED to (one handle per slot), so its size is the slot count on return
     * @param mapped the mappings to appends to; pass nullptr for a family the host never writes through
     */
    void create_buffers(core& device, std::vector<rhi::object_manager<rhi::buffer>>& outputs, rhi::buffer_usage usage, rhi::buffer_flags flags, std::span<std::byte const> initial_bytes, std::string_view what, std::vector<void*>* mapped = nullptr);

    /**
     * @brief THE DEVICE ADDRESS OF A CONTRACT BUFFER, through the `device_address` ability.
     * @return 0 when the backend did not announce the ability, or when the buffer was created without
     *         `buffer_flag::device_address` - the ability's own two answers, and every call site here asks
     *         for a buffer it created WITH that flag.
     */
    [[nodiscard]] VkDeviceAddress buffer_address(core& device, rhi::buffer const& buffer) noexcept;
} // namespace deren::vulkan
