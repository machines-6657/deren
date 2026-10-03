module;

#include <GLFW/glfw3.h>
#include <algorithm> // std::min in the resource publication
#include <bit>       // std::bit_cast for the caster world-matrix hash
#include <chrono>
#include <cstring> // std::memcpy, for composing a pass's push block
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>   // the byte spans the contract's buffer descriptors and the push endpoints take
#include <thread> // std::this_thread::yield in the frame limiter
#include <vulkan/vulkan.h>

module deren.vulkan.runtime;

import deren.vulkan.profiling;
import deren.vulkan.pipelines;
import deren.vulkan.bindings;
import deren.vulkan.render_resource;
import deren.vulkan.render_resource.shared;

import deren.utility;
import deren.vulkan.constant_init;
import deren.vulkan.frame_constants; // one frame's shared constants (see update_frame_constants)
import deren.vulkan.core.pipeline;   // deren::vulkan::make_pipeline for the post-process pipeline
import deren.vulkan.meshlet;         // the meshlet split (docs/mesh_shaders.md step 3): pure CPU, built at upload

// Route std::pmr allocations through mimalloc for this TU (deren.utility:better_pmr). Idempotent:
// init_pmr() returns the same process-wide singleton no matter which TU calls it first, so
// main.cpp's keep-alive and this one coexist safely. The reference itself is never read; it
// only forces the (dynamic) initialization before any pmr container in this TU is constructed.
[[maybe_unused]] static auto& pmr = deren::utility::init_pmr(); // NOLINT(keep-alive)

namespace deren::vulkan {
    void runtime::write_rt_structure_binding(VkAccelerationStructureKHR const tlas, uint32_t const frame_slot) {
        // The top level structure is a HEAP slot now, and this is the one thing this function still does: a heap
        // descriptor for an acceleration structure is an ADDRESS RANGE carrying the structure's device address
        // (the heap's payload union has no AS member - see docs/descriptor_heap_migration.md), and it is
        // per FRAME SLOT because the structure is rebuilt every frame - which is why heap_slots::tlas is a
        // two-slot array. The size is the one the structure was created with (published through
        // ray_tracing::structure_set): a heap range must carry a real size, a lesson this renderer already paid
        // for on the material table.
        if (!this->vulkan_core.descriptor_heaps.ready() || this->vulkan_core.heap_grid_offset == VK_WHOLE_SIZE || tlas == VK_NULL_HANDLE) {
            return;
        }
        // RESOLVED PER DEVICE, not linked: the loader exports the core entry points and not this
        // extension one (the link failed with `undefined symbol:
        // vkGetAccelerationStructureDeviceAddressKHR`, which is the same reason the acceleration
        // structure module loads its own entry points through vkGetDeviceProcAddr).
        static PFN_vkGetAccelerationStructureDeviceAddressKHR const get_structure_address =
            reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(this->vulkan_core.logical_device, "vkGetAccelerationStructureDeviceAddressKHR"));
        VkAccelerationStructureDeviceAddressInfoKHR const tlas_address_info = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .pNext = nullptr,
            .accelerationStructure = tlas,
        };
        VkDeviceAddress const tlas_address = get_structure_address != nullptr ? get_structure_address(this->vulkan_core.logical_device, &tlas_address_info) : 0;
        if (!this->vulkan_core.descriptor_heaps.write_buffer(core::heap_slot_offset(core::heap_slots::tlas + frame_slot), tlas_address, this->structures.structure_size(frame_slot), VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)) {
            deren::utility::log("descriptor heap: the top level structure did not reach grid slot {}", core::heap_slots::tlas + frame_slot);
        }

        // ... AND THE INSTANCE TABLE, which is rebuilt WITH the structures and whose slot is the same event's: the
        // descriptor is an address range at heap_slots::mask_instances + frame slot, and the capacity is the
        // structures module's to know (instance_table_size).
        //
        // IT ARRIVES AS A CONTRACT HANDLE (`instance_table_buffer()`), which is what lets the address come from
        // the `device_address` ability instead of `vkGetBufferDeviceAddress` on a handle this side had to narrow
        // out of the escape - the same path every other converted site takes. The pointer is BORROWED from the
        // structure's own slot (see that accessor's note): it is read here and nothing keeps it.
        if (rhi::buffer const* const instance_table = this->structures.instance_table_buffer(frame_slot); instance_table != nullptr) {
            if (!this->write_heap_buffer(*instance_table, core::heap_slots::mask_instances + frame_slot, this->structures.instance_table_size(frame_slot), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) {
                deren::utility::log("descriptor heap: the instance table did not reach grid slot {}", core::heap_slots::mask_instances + frame_slot);
            }
        }
    }

    void runtime::begin_rendering(VkCommandBuffer const command_buffer, uint32_t const image_index, VkRenderingFlags const flags) const {
        core const& vk = this->vulkan_core;

        // Dynamic rendering (Vulkan 1.3 core, the only path the engine supports): attachments are
        // described inline, no render pass / framebuffer objects exist. The scene instance is always
        // the G-buffer pass: the opaque pass writes the surface instead of shading it, into three
        // single-sampled targets + the motion-vector target + the G-buffer's own 1x depth image, and
        // adds the emissive into the scene color target. The lighting stage then shades it into the
        // image the post chain reads (scene_target_view).
        if (this->gbuffer_pass_active()) {
            std::array<VkRenderingAttachmentInfo, deren::vulkan::gbuffer_pass_attachment_count> gbuffer_attachments = {};
            VkClearValue clear = {}; // the surface + motion targets clear to zero: no geometry, no motion
            for (uint32_t target = 0; target < deren::vulkan::gbuffer_target_count; ++target) {
                gbuffer_attachments[target] = make_color_attachment_info(vk.gbuffer_image_views[target][image_index], clear, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE);
            }
            gbuffer_attachments[deren::vulkan::gbuffer_target_count] = make_color_attachment_info(vk.velocity_image_views[image_index], clear, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE);
            // the last attachment is the scene color, CLEARed to zero: it accumulates only the
            // emissive here. The lighting stage then adds the lighting (and the sky, for pixels no
            // geometry wrote) on top, so a lit pixel is emissive + lighting and a background pixel is
            // sky - with no background pass anywhere. Under TAA the scene color is the resolve's input
            // image, and the resolve writes the HDR target the post chain reads (see
            // runtime::scene_target_view).
            gbuffer_attachments[deren::vulkan::gbuffer_target_count + 1] = make_color_attachment_info(this->scene_target_view(image_index), clear, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE);
            // the G-buffer depth clears to the far plane (1.0) - but unlike the old forward path's
            // main depth, its contents must SURVIVE the instance: the lighting stage, the transparent
            // pass, the TAA resolve and the debug view all read this image later in the same
            // submission (the G-buffer depth heap slot, which all four of them read). STORE_OP_DONT_CARE would
            // leave the contents undefined, which is exactly what those four read.
            VkRenderingAttachmentInfo const depth_attachment = make_depth_attachment_info(vk.gbuffer_depth_image_views[image_index], VK_ATTACHMENT_STORE_OP_STORE);
            VkRenderingInfo const rendering_info = make_rendering_info(flags, {{0, 0}, vk.render_extent()}, gbuffer_attachments.data(), static_cast<uint32_t>(gbuffer_attachments.size()), &depth_attachment);
            vkCmdBeginRendering(command_buffer, &rendering_info);
            return;
        }

        // No G-buffer pipeline: no scene can be drawn. Open an empty instance anyway, so every frame
        // still has a matching vkCmdEndRendering and the post chain samples a defined target.
        VkClearValue clear_color = {};
        clear_color.color = {{this->background_color.r, this->background_color.g, this->background_color.b, 1.0f}};
        VkRenderingAttachmentInfo const color_attachment = make_color_attachment_info(
            vk.hdr_image_views[image_index],
            clear_color,
            VK_RESOLVE_MODE_NONE,
            VK_NULL_HANDLE);

        // depth clears to the far plane (1.0): make_depth_attachment_info. DONT_CARE is correct here -
        // nothing samples this depth (the G-buffer depth above is the one that is read back).
        VkRenderingAttachmentInfo const depth_attachment = make_depth_attachment_info(vk.depth_image_views[image_index], VK_ATTACHMENT_STORE_OP_DONT_CARE);

        VkRenderingInfo const rendering_info = make_rendering_info(flags, {{0, 0}, vk.render_extent()}, true, &color_attachment, &depth_attachment);
        vkCmdBeginRendering(command_buffer, &rendering_info);
    }

    frame_status runtime::poll_events() {
        core& vk = this->vulkan_core;
        GLFWwindow* window = vk.window;

        // Window events first: respond to ESC / native close before any GPU work
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        if (glfwWindowShouldClose(window)) {
            return frame_status::closed;
        }

        // F1 toggles the debug overlay (edge-triggered: holding the key toggles once). When the
        // overlay was never initialized ([gui] show = false at startup) the first press enables
        // it on demand, so the key also brings the ui back after a disable.
        bool const f1_down = glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS;
        if (f1_down && !this->gui_toggle_down) {
            if (!this->debug_overlay.is_active()) {
                this->debug_gui_shown = this->enable_debug_gui();
            } else {
                this->debug_gui_shown = !this->debug_gui_shown;
            }
            deren::utility::log("gui overlay: {}", this->debug_gui_shown ? "shown (F1 to hide)" : "hidden (F1 to show)");
        }
        this->gui_toggle_down = f1_down;

        // F12 requests a screenshot (edge-triggered); the caller consumes the request and saves
        // the capture (runtime::consume_screenshot_request + acquire_current_frame_image)
        bool const f12_down = glfwGetKey(window, GLFW_KEY_F12) == GLFW_PRESS;
        if (f12_down && !this->screenshot_key_down) {
            this->screenshot_requested = true;
            deren::utility::log("screenshot requested (F12)");
        }
        this->screenshot_key_down = f12_down;

        // ARROW KEYS PAN THE CAMERA. LEFT/RIGHT strafe along the camera's right vector, UP/DOWN rise and
        // fall along WORLD up, and both translate `camera.target` - the point the eye orbits - so the rig
        // slides without the view rotating (the arithmetic lives in orbit_camera_pan_delta,
        // vulkan/primitive, next to the orbit sphere it is derived from). UP/DOWN is world up rather than
        // the view direction on purpose: the view-direction version pushed the eye toward the subject and
        // read as a zoom, not as the up/down slide the keys promise.
        // Unlike F1/F12 this is a CONTINUOUS input: the step is speed x elapsed time sampled off
        // glfwGetTime, so the pan speed does not depend on the frame rate. There is deliberately no gui
        // guard here - the debug overlay captures the MOUSE only (gui_content exposes wants_mouse), so
        // nothing in it competes for the arrows; if a keyboard-capturing widget ever appears, the guard
        // belongs here, next to the "gui priority over the camera" rule the mouse callbacks follow.
        double const now = glfwGetTime();
        double const elapsed = this->camera.last_pan_time > 0.0 ? now - this->camera.last_pan_time : 0.0;
        this->camera.last_pan_time = now; // sampled every frame, so a key released for a while does not bank a jump
        float const strafe = (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS ? 1.0f : 0.0f) -
                             (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS ? 1.0f : 0.0f);
        float const rise = (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS ? 1.0f : 0.0f) -
                           (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS ? 1.0f : 0.0f);
        bool const pan_fast = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        glm::vec3 const pan = orbit_camera_pan_delta(this->camera.yaw, this->camera.distance, strafe, rise, static_cast<float>(elapsed), pan_fast);
        if (pan != glm::vec3(0.0f)) { // no arrow held (or a zero step) leaves the target UNTOUCHED, byte for byte
            this->camera.target += pan;
        }

        // Minimized: skip this frame (acquiring from an invalidated / 0-sized swapchain would
        //    fail); the restore transition is handled by recreate_if_minimized()
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE) {
            this->was_minimized = true;
            return frame_status::skipped;
        }
        return frame_status::proceed;
    }

    void runtime::recreate_if_minimized() {
        core& vk = this->vulkan_core;
        if (this->was_minimized) {
            this->was_minimized = false;
            deren::utility::log("window restored, recreating swapchain");
            // Only a generation that was ACTUALLY rebuilt invalidates the per-image state. A deferred
            // recreate (the window came back with a 0x0 drawable size) keeps every image the frame loop
            // is holding, so resetting here would throw the temporal histories away for nothing - and a
            // minimize/restore would then pay for two re-convergences instead of one.
            if (vk.recreate_swap_chain()) {
                this->on_swapchain_recreated();
            }
        }
    }

    void runtime::on_swapchain_recreated() {
        // The swapchain generation changed: every per-image target was destroyed and rebuilt, so
        // every descriptor set that pointed at the old views must be replaced before it is used
        // again. Both per-image set families (the post chain's and the G-buffer path's) are dropped
        // with their pools, and the next recorded frame allocates FRESH sets from a fresh pool.
        // That is the only safe way to rebind them here:
        //  - a pool must not be destroyed while any recorded command buffer names its sets
        //    (VUID-vkDestroyDescriptorPool-descriptorPool-00303), and the per-slot frame command
        //    buffers stay recorded between frames - so the pool is RETIRED (destroyed with the
        //    runtime) instead, and
        //  - a set must not be updated while a frame that uses it is pending
        //    (VUID-vkUpdateDescriptorSets-None-03047), and with two frames in flight the other slot's
        //    frame can still be running - a freshly allocated set is referenced by nothing, so
        //    allocating instead of updating sidesteps that entirely.
        this->debug_overlay.on_swapchain_recreated();
        // NOTHING OF THIS CLASS IS RETIRED HERE ANY MORE: the G-buffer and post descriptor families it used to
        // retire are gone with the sets (every stage reaches its images through the heap, whose slots name images
        // rather than sets), so the only per-generation state left is the PASSES' - and they are told below.
        if (this->frame_wiring.recreated != nullptr) {
            this->frame_wiring.recreated(this->frame_wiring.owner);
        }
        // AND THE PASSES ARE TOLD, by the runner rather than by a hand-kept list. That is the hazard this layer was
        // built to remove: a pass that keeps per-generation state cannot be missed, because it is not this function
        // that remembers - `recreate_stage` calls every pass in the stage, and a pass added to one is covered.
        {
            pass::stage const taa_stage = {.name = "taa", .passes = this->taa_pass, .marks = false};
            [[maybe_unused]] pass::run_report const taa_recreated = pass::recreate_stage(taa_stage, this->make_pass_host());
            // THE STAGES ADDED SINCE THIS LIST WAS WRITTEN, and now one call per chain: a pass whose per-image
            // first-use state describes a GENERATION is owed another first-use batch by a swapchain recreation.
            // Leaving a pass out of this list is invisible until someone resizes the window - exactly the hazard
            // `recreate_stage` exists to remove, which is why the fix is this call and not a second hand-kept
            // flag in the host. The chains make "all of them" the default instead of a list someone has to
            // remember to extend.
        }
        // Every swapchain image's history died with the old generation (and its size may have
        // changed): forget the matrices, so the next frame for each image starts a new accumulation
        // instead of blending in a misaligned one. (WHETHER a history holds anything is the TAA pass's own
        // state, and the call above is what cleared it.)
        std::size_t const image_count = this->vulkan_core.taa_history_images.size();
        this->image_view_proj.assign(image_count, this->current_ubo.view_proj_unjittered);
        // The per-image first-use state of the passes in a chain is the PASS's, and the recreate_stage call
        // above is what told each of them its first-use batch - which is why there is no host flag for it any
        // more (the renderer asks the pass).
        //
        // Every OTHER per-image flag is the generation reset, and it is the very function the constructor
        // calls for generation 0: this list used to be hand-kept in two places (the G-buffer depth flag, the
        // motion-vector flag, the G-buffer target flags, the GI accumulation and the furnace cube), and the
        // two copies had already drifted. `image_view_proj` above stays out of it, because it needs a camera
        // snapshot (current_ubo) - which only this call site and set_taa's off -> on edge have.
        this->reset_image_generation_state();
    }

    void runtime::gpu_mark(VkCommandBuffer const command_buffer, gpu_mark_id const mark, VkPipelineStageFlagBits const stage) noexcept {
        if (!this->gpu_timings_enabled) {
            return;
        }
        uint32_t const slot = static_cast<uint32_t>(this->vulkan_core.current_frame);
        // The mark's identity is positional - the interval it closes is the one opened by the mark
        // before it - so every label in gpu_timing_labels is only correct while the calls happen in
        // gpu_mark_id order. The core hands out the next index, which makes the violation visible
        // here instead of only as a mislabeled report.
        if (this->vulkan_core.gpu_timing_marks[slot] != static_cast<uint32_t>(mark)) {
            deren::utility::log("runtime: GPU timing marks recorded out of order (mark {} at index {}) - the pass report is mislabeled",
                                static_cast<uint32_t>(mark),
                                this->vulkan_core.gpu_timing_marks[slot]);
            return;
        }
        this->vulkan_core.mark_gpu_timing(command_buffer, slot, stage);
    }

    void runtime::collect_gpu_timings(uint32_t const slot) {
        gpu_timing_result const result = this->vulkan_core.read_gpu_timings(slot);
        if (result.mark_count < 2) {
            return; // nothing measured in that submission (the first frames, or a failed recording)
        }
        // result.milliseconds[i] is the interval between mark i and mark i + 1, which is the pass
        // gpu_timing_labels[i] names: the marks are written in gpu_mark_id order on every frame, and
        // a pass that did not record left two adjacent marks behind, so its interval reads ~0.
        uint32_t const intervals = std::min(result.mark_count - 1, static_cast<uint32_t>(gpu_timing_labels.size()));
        for (uint32_t interval = 0; interval < intervals; ++interval) {
            this->gpu_timing_sum[interval] += result.milliseconds[interval];
        }
        this->gpu_timing_marks_measured = intervals;
        if (++this->gpu_timing_window_frames < GPU_TIMING_WINDOW) {
            return;
        }

        // window complete: report the means (the label keeps reading the running mean, so this only
        // snapshots and resets) and start over
        double total = 0.0;
        std::string report = std::format("gpu pass timings (avg of {} frames):", GPU_TIMING_WINDOW);
        // The OVERLAY line is built here as well, and deliberately not every frame: every value is a
        // fixed-width field, because a label whose text width changes re-wraps against the panel edge
        // and the whole overlay appears to twitch (the numbers of a live running mean change in width
        // frame by frame: 0.25 -> 10.25). Fixed fields + a report that changes once per window make
        // the panel layout stable between updates.
        std::string label = std::format("gpu ({}f):", GPU_TIMING_WINDOW);
        for (uint32_t interval = 0; interval < intervals; ++interval) {
            double const mean = this->gpu_timing_sum[interval] / static_cast<double>(GPU_TIMING_WINDOW);
            report += std::format(" {} {:.2f} ms |", gpu_timing_labels[interval].name, mean);
            label += std::format("{} {:>5.2f}", gpu_timing_labels[interval].new_line ? "\n     " : " |", mean);
            total += mean; // GPU intervals have no sub-phases: every mark interval counts once
            this->gpu_timing_sum[interval] = 0.0;
        }
        this->gpu_timing_window_frames = 0;
        report += std::format(" total {:.2f} ms", total);
        this->gpu_timing_report_label = label + std::format("\n     total {:>5.2f} ms", total);
        deren::utility::log("{}", report);
    }

    std::string runtime::gpu_timing_summary() const {
        if (!this->gpu_timings_enabled) {
            return "gpu timings: off";
        }
        if (!this->vulkan_core.gpu_timing_available()) {
            return "gpu timings: unavailable on this device";
        }
        if (this->gpu_timing_report_label.empty()) {
            return "gpu timings: collecting...";
        }
        // The last COMPLETED window's report, not a per-frame running mean: the overlay line is a
        // text layout, and a text layout that changes every frame is a UI that twitches (see the
        // report construction above). It refreshes once per GPU_TIMING_WINDOW frames, which is also
        // what the log line reports - so the overlay and the log always agree.
        return this->gpu_timing_report_label;
    }

    bool runtime::enable_debug_gui() {
        if (this->debug_overlay.is_active()) {
            return true;
        }
        // The overlay draws into the runtime's OPEN main rendering instance via dynamic
        // rendering (the backend is initialized with UseDynamicRendering=true).
        deren::vulkan::core const& vk = this->vulkan_core;
        gui::gui_create_info info = {};
        info.window = vk.window;
        info.instance = vk.instance;
        info.physical_device = vk.physical_device;
        info.device = vk.logical_device;
        info.graphics_queue_family = vk.graphics_queue_family_index;
        info.graphics_queue = vk.graphics_queue_handle;
        info.color_format = vk.swap_chain_image_format;
        info.depth_format = VK_FORMAT_UNDEFINED; // the post/gui pass has no depth attachment
        info.frames_in_flight = static_cast<uint32_t>(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT);
        return this->debug_overlay.init(info);
    }

    bool runtime::debug_gui_wants_mouse() const noexcept {
        return this->debug_gui_shown && this->debug_overlay.is_active() && this->debug_overlay.wants_mouse();
    }

    gui::gui_content& runtime::debug_gui() noexcept {
        return this->debug_overlay;
    }

    std::expected<void, std::string> runtime::make_pipeline(std::string_view pipeline_name, std::span<uint8_t const> fragment_shader_code,
                                                            std::span<uint8_t const> const mesh_vertex_shader_code, std::span<uint8_t const> const meshlet_shader_code) {
        using fail = std::unexpected<std::string>;
        // ---- THE VERTEX FORM IS GONE (docs/mesh_shaders.md step 4): a named geometry pipeline is built from a MESH
        // stage, so the mesh module is REQUIRED and its absence or refusal is an error - there is no vertex pipeline
        // left to be a complete answer. The meshlet form is optional and preferred when it exists.
        bool const want_mesh = !mesh_vertex_shader_code.empty() && this->evaluate_mesh_shaders(nullptr);
        {
            // unique lock around the duplicate check + registry append: a concurrent reader
            // (recording worker) must never observe a half-inserted map / name table
            std::unique_lock const lock(this->access_mutex);
            if (this->mesh_pipelines.contains(pipeline_name)) {
                return fail(std::string("pipeline '") + std::string(pipeline_name) + "' already exists");
            }
        }
        if (!want_mesh) {
            return fail(std::string("pipeline '") + std::string(pipeline_name) + "' has no mesh stage to build from, and its vertex form is gone (docs/mesh_shaders.md step 4)");
        }
        // GPU pipeline creation is expensive and touches no shared registry state: build it
        // OUTSIDE the lock so a reader is never blocked by shader compilation.
        //
        // BUILT FROM THE DEVICE, not through `core::make_pipeline`: that entry point is the old style - it
        // reached into the core for the flat scene layout, the surface format and the depth format, which is
        // exactly what a caller that is not the runtime (a pass, whose create step has a device and its own
        // layout) cannot do. The four facts are read here instead, and they are the ones that entry point used,
        // so this is a re-expression: same layout, same formats, same single-sampled pipeline.
        std::array<VkFormat, 1> const color_formats = {this->vulkan_core.swap_chain_image_format};
        // ... AND THE BLEND STATE, which the old entry point got from the convenience overload: the FORWARD
        // pipelines' convention is src-alpha blending (alpha is coverage, and an opaque draw's alpha of one
        // reduces the blend math to the source colour), while the span-based form's default is "overwrite".
        // Omitting it is not a no-op - the gate caught it as a changed scenario, and this is why the conversion
        // is a re-expression rather than a rewrite.
        std::array<VkPipelineColorBlendAttachmentState, 1> const blend_attachments = {make_color_blend_attachment()};
        // ---- THE MESH FORM, which IS the pipeline now (docs/mesh_shaders.md step 4): built before the registry
        //      insert, cached with the same two viewport values its draws need (begin_pipeline re-emits them and
        //      update_pass_geometry resyncs every registered pipeline once per frame), and a failure is the CALLER's
        //      - the vertex pipeline that used to absorb a refusal does not exist any more.
        std::optional<vk_pipeline> mesh_result = std::nullopt;
        {
            auto built = deren::vulkan::make_pipeline(this->vulkan_core.logical_device,
                                                      std::span<VkFormat const>(color_formats),
                                                      this->vulkan_core.depth_attachment_format,
                                                      mesh_vertex_shader_code,
                                                      fragment_shader_code,
                                                      VK_SAMPLE_COUNT_1_BIT,
                                                      /*depth_test_enabled=*/true,
                                                      0.0f,
                                                      0.0f,
                                                      0.0f,
                                                      std::span<VkPipelineColorBlendAttachmentState const>(blend_attachments),
                                                      VK_SHADER_STAGE_MESH_BIT_EXT);
            if (built) {
                // the two cached values every named pipeline needs (begin_pipeline re-emits them, and
                // update_pass_geometry resyncs every registered pipeline once per frame)
                built->viewport = {0.0f, 0.0f, static_cast<float>(this->vulkan_core.render_extent().width), static_cast<float>(this->vulkan_core.render_extent().height), 0.0f, 1.0f};
                built->scissor = {{0, 0}, this->vulkan_core.render_extent()};
                mesh_result = std::move(*built);
            } else {
                // NO FALLBACK LEFT (docs/mesh_shaders.md step 4): the caller sees this as the pipeline's failure
                return fail("pipeline '" + std::string(pipeline_name) + "': the mesh stage was refused (" + std::string(built.error()) + ")");
            }
        }
        std::optional<vk_pipeline> meshlet_result = std::nullopt;
        if (want_mesh && !meshlet_shader_code.empty()) {
            // THE MESHLET FORM (docs/mesh_shaders.md step 3): the same fragment stage again, with the entry that
            // reads one meshlet per workgroup out of the heap table and culls it against the camera. Built exactly
            // like the mesh form - it IS a mesh stage - and a refusal leaves the mesh form as the answer, which is
            // why this is a third entry rather than a replacement.
            auto built = deren::vulkan::make_pipeline(this->vulkan_core.logical_device,
                                                      std::span<VkFormat const>(color_formats),
                                                      this->vulkan_core.depth_attachment_format,
                                                      meshlet_shader_code,
                                                      fragment_shader_code,
                                                      VK_SAMPLE_COUNT_1_BIT,
                                                      /*depth_test_enabled=*/true,
                                                      0.0f,
                                                      0.0f,
                                                      0.0f,
                                                      std::span<VkPipelineColorBlendAttachmentState const>(blend_attachments),
                                                      VK_SHADER_STAGE_MESH_BIT_EXT);
            if (built) {
                built->viewport = {0.0f, 0.0f, static_cast<float>(this->vulkan_core.render_extent().width), static_cast<float>(this->vulkan_core.render_extent().height), 0.0f, 1.0f};
                built->scissor = {{0, 0}, this->vulkan_core.render_extent()};
                meshlet_result = std::move(*built);
            } else {
                deren::utility::log("pipeline '{}': no meshlet form ({}), so its leaves stay on the mesh pipeline", pipeline_name, built.error());
            }
        }
        {
            std::unique_lock const lock(this->access_mutex);
            // THE MESH FORM IS THE PIPELINE (docs/mesh_shaders.md step 4): there is no vertex entry in the registry
            // for a geometry name any more, so a session that binds by name finds this one, or the meshlet one, or
            // nothing at all - and `set_default_pipeline` answers the same question the same way.
            if (mesh_result.has_value()) {
                this->mesh_pipelines.emplace(pipeline_name, std::move(*mesh_result));
                deren::utility::log("SUCCESS: pipeline '{}' created with a MESH form (its leaves are dispatched)", pipeline_name);
            }
            if (meshlet_result.has_value()) {
                this->meshlet_pipelines.emplace(pipeline_name, std::move(*meshlet_result));
                deren::utility::log("SUCCESS: pipeline '{}' created with a MESHLET form (one workgroup per meshlet, camera-culled)", pipeline_name);
            }
            if (this->default_pipeline_name.empty()) {
                this->default_pipeline_name = pipeline_name; // first pipeline is the implicit default
            }
        }
        return {};
    }

    std::expected<void, std::string> runtime::make_character_forward_pipeline(std::string_view const pipeline_name,
                                                                              std::span<uint8_t const> const fragment_shader_code,
                                                                              std::span<uint8_t const> const mesh_vertex_shader_code,
                                                                              std::span<uint8_t const> const meshlet_shader_code) {
        using fail = std::unexpected<std::string>;
        if (fragment_shader_code.empty()) {
            return fail(std::string("character-forward pipeline '") + std::string(pipeline_name) + "': no fragment stage was given");
        }
        // The mesh stage is REQUIRED, exactly as it is for every other geometry pipeline (docs/mesh_shaders.md
        // step 4): the vertex path is gone, so a name without a mesh module has no complete answer.
        bool const want_mesh = !mesh_vertex_shader_code.empty() && this->evaluate_mesh_shaders(nullptr);
        {
            std::unique_lock const lock(this->access_mutex);
            if (this->mesh_pipelines.contains(pipeline_name) || this->meshlet_pipelines.contains(pipeline_name)) {
                return fail(std::string("pipeline '") + std::string(pipeline_name) + "' already exists");
            }
        }
        if (!want_mesh) {
            return fail(std::string("character-forward pipeline '") + std::string(pipeline_name) + "' has no mesh stage to build from, and its vertex form is gone (docs/mesh_shaders.md step 4)");
        }
        // GPU pipeline creation outside the lock, for the reason make_pipeline gives: a recording worker must
        // never be blocked by shader compilation.
        std::optional<vk_pipeline> mesh_result = std::nullopt;
        {
            auto built = this->vulkan_core.make_character_forward_pipeline(mesh_vertex_shader_code, fragment_shader_code, VK_SHADER_STAGE_MESH_BIT_EXT);
            if (!built) {
                return fail("character-forward pipeline '" + std::string(pipeline_name) + "': the mesh stage was refused (" + std::string(built.error()) + ")");
            }
            mesh_result = std::move(*built);
        }
        std::optional<vk_pipeline> meshlet_result = std::nullopt;
        if (!meshlet_shader_code.empty()) {
            // A refusal here leaves the mesh form as the answer rather than failing the call - the same
            // relationship the named forward pipelines have between their two forms.
            auto built = this->vulkan_core.make_character_forward_pipeline(meshlet_shader_code, fragment_shader_code, VK_SHADER_STAGE_MESH_BIT_EXT);
            if (built) {
                meshlet_result = std::move(*built);
            } else {
                deren::utility::log("character-forward pipeline '{}': no meshlet form ({}), so its leaves stay on the mesh pipeline", pipeline_name, built.error());
            }
        }
        {
            std::unique_lock const lock(this->access_mutex);
            this->mesh_pipelines.emplace(pipeline_name, std::move(*mesh_result));
            deren::utility::log("SUCCESS: character-forward pipeline '{}' created with a MESH form (one HDR target, blending off, depth compare EQUAL, depth write held off)", pipeline_name);
            if (meshlet_result.has_value()) {
                this->meshlet_pipelines.emplace(pipeline_name, std::move(*meshlet_result));
                deren::utility::log("SUCCESS: character-forward pipeline '{}' created with a MESHLET form (one workgroup per meshlet)", pipeline_name);
            }
            // NOTE: `default_pipeline_name` is deliberately NOT set here, even when it is empty. This pipeline
            // is only valid inside the character-forward pass's instance (it declares ONE colour attachment),
            // so making it the runtime default would let a default-semantics leaf elsewhere be drawn by it -
            // the same hazard the G-buffer pipeline is kept out of the registry for.
        }
        return {};
    }

    std::expected<void, std::string> runtime::make_overlay_pipeline(std::string_view const pipeline_name,
                                                                    std::span<uint8_t const> const fragment_shader_code,
                                                                    std::span<uint8_t const> const mesh_vertex_shader_code,
                                                                    std::span<uint8_t const> const meshlet_shader_code) {
        using fail = std::unexpected<std::string>;
        if (fragment_shader_code.empty()) {
            return fail(std::string("overlay pipeline '") + std::string(pipeline_name) + "': no fragment stage was given");
        }
        // The mesh stage is REQUIRED, exactly as it is for the toon and forward families (docs/mesh_shaders.md
        // step 4): the vertex geometry path is gone, so a name without a mesh module has no complete answer.
        bool const want_mesh = !mesh_vertex_shader_code.empty() && this->evaluate_mesh_shaders(nullptr);
        {
            std::unique_lock const lock(this->access_mutex);
            if (this->mesh_pipelines.contains(pipeline_name) || this->meshlet_pipelines.contains(pipeline_name)) {
                return fail(std::string("pipeline '") + std::string(pipeline_name) + "' already exists");
            }
        }
        if (!want_mesh) {
            return fail(std::string("overlay pipeline '") + std::string(pipeline_name) + "' has no mesh stage to build from, and its vertex form is gone (docs/mesh_shaders.md step 4)");
        }
        // GPU pipeline creation outside the lock, for the reason make_pipeline gives: a recording worker must
        // never be blocked by shader compilation.
        std::optional<vk_pipeline> mesh_result = std::nullopt;
        {
            auto built = this->vulkan_core.make_overlay_pipeline(mesh_vertex_shader_code, fragment_shader_code, VK_SHADER_STAGE_MESH_BIT_EXT);
            if (!built) {
                return fail("overlay pipeline '" + std::string(pipeline_name) + "': the mesh stage was refused (" + std::string(built.error()) + ")");
            }
            mesh_result = std::move(*built);
        }
        std::optional<vk_pipeline> meshlet_result = std::nullopt;
        if (!meshlet_shader_code.empty()) {
            // A refusal here leaves the mesh form as the answer rather than failing the call - the same
            // relationship both other families have between their two forms. The overlay meshes are two to nine
            // meshlets apiece, so the meshlet form is a small win here and its absence costs nothing.
            auto built = this->vulkan_core.make_overlay_pipeline(meshlet_shader_code, fragment_shader_code, VK_SHADER_STAGE_MESH_BIT_EXT);
            if (built) {
                meshlet_result = std::move(*built);
            } else {
                deren::utility::log("overlay pipeline '{}': no meshlet form ({}), so its leaves stay on the mesh pipeline", pipeline_name, built.error());
            }
        }
        {
            std::unique_lock const lock(this->access_mutex);
            this->mesh_pipelines.emplace(pipeline_name, std::move(*mesh_result));
            deren::utility::log("SUCCESS: overlay pipeline '{}' created with a MESH form (one HDR target, dst = src * dst, depth compare LESS_OR_EQUAL, depth write held off)", pipeline_name);
            if (meshlet_result.has_value()) {
                this->meshlet_pipelines.emplace(pipeline_name, std::move(*meshlet_result));
                deren::utility::log("SUCCESS: overlay pipeline '{}' created with a MESHLET form (one workgroup per meshlet)", pipeline_name);
            }
            // NOTE: `default_pipeline_name` is deliberately NOT set here, for the reason
            // make_character_forward_pipeline gives: this pipeline declares ONE colour attachment and the
            // multiply blend, so a default-semantics leaf anywhere else drawn by it would be multiplied into the
            // frame by a mask texture that has nothing to do with it.
        }
        return {};
    }

    std::expected<void, std::string> runtime::make_outline_pipeline(std::string_view const pipeline_name,
                                                                    std::span<uint8_t const> const fragment_shader_code,
                                                                    std::span<uint8_t const> const mesh_vertex_shader_code,
                                                                    std::span<uint8_t const> const meshlet_shader_code) {
        using fail = std::unexpected<std::string>;
        if (fragment_shader_code.empty()) {
            return fail(std::string("outline pipeline '") + std::string(pipeline_name) + "': no fragment stage was given");
        }
        // The mesh stage is REQUIRED, exactly as it is for the toon, forward and overlay families
        // (docs/mesh_shaders.md step 4): the vertex geometry path is gone, so a name without a mesh module has no
        // complete answer. It is doubly required here: pushing a hull's vertices outward in clip space is the
        // MESH stage's job (`pbr.slang`'s outline entry), so an outline without it would be the surface drawn a
        // second time.
        bool const want_mesh = !mesh_vertex_shader_code.empty() && this->evaluate_mesh_shaders(nullptr);
        {
            std::unique_lock const lock(this->access_mutex);
            if (this->mesh_pipelines.contains(pipeline_name) || this->meshlet_pipelines.contains(pipeline_name)) {
                return fail(std::string("pipeline '") + std::string(pipeline_name) + "' already exists");
            }
        }
        if (!want_mesh) {
            return fail(std::string("outline pipeline '") + std::string(pipeline_name) + "' has no mesh stage to build from, and its vertex form is gone (docs/mesh_shaders.md step 4)");
        }
        // GPU pipeline creation outside the lock, for the reason make_pipeline gives: a recording worker must
        // never be blocked by shader compilation.
        std::optional<vk_pipeline> mesh_result = std::nullopt;
        {
            auto built = this->vulkan_core.make_outline_pipeline(mesh_vertex_shader_code, fragment_shader_code, VK_SHADER_STAGE_MESH_BIT_EXT);
            if (!built) {
                return fail("outline pipeline '" + std::string(pipeline_name) + "': the mesh stage was refused (" + std::string(built.error()) + ")");
            }
            mesh_result = std::move(*built);
        }
        std::optional<vk_pipeline> meshlet_result = std::nullopt;
        if (!meshlet_shader_code.empty()) {
            // A refusal here leaves the mesh form as the answer rather than failing the call - the same
            // relationship the other families have between their two forms. Every leaf that carries an outline
            // width is still drawn by the mesh form, so the absence of a meshlet form costs throughput, not
            // correctness.
            auto built = this->vulkan_core.make_outline_pipeline(meshlet_shader_code, fragment_shader_code, VK_SHADER_STAGE_MESH_BIT_EXT);
            if (built) {
                meshlet_result = std::move(*built);
            } else {
                deren::utility::log("outline pipeline '{}': no meshlet form ({}), so its leaves stay on the mesh pipeline", pipeline_name, built.error());
            }
        }
        {
            std::unique_lock const lock(this->access_mutex);
            this->mesh_pipelines.emplace(pipeline_name, std::move(*mesh_result));
            deren::utility::log("SUCCESS: outline pipeline '{}' created with a MESH form (one HDR target, opaque blend, depth compare LESS_OR_EQUAL, depth write held off, front faces culled per draw)", pipeline_name);
            if (meshlet_result.has_value()) {
                this->meshlet_pipelines.emplace(pipeline_name, std::move(*meshlet_result));
                deren::utility::log("SUCCESS: outline pipeline '{}' created with a MESHLET form (one workgroup per meshlet)", pipeline_name);
            }
            // NOTE: `default_pipeline_name` is deliberately NOT set here, for the reason
            // make_overlay_pipeline gives: this pipeline declares ONE colour attachment and an opaque blend, so a
            // default-semantics leaf anywhere else drawn by it would be painted as an unlit hull in place of its
            // own shaded surface.
        }
        return {};
    }

    void runtime::set_default_pipeline(std::string_view const pipeline_name) {
        std::unique_lock const lock(this->access_mutex);
        if (this->mesh_pipelines.contains(pipeline_name) || this->meshlet_pipelines.contains(pipeline_name)) {
            this->default_pipeline_name = pipeline_name;
        }
    }

    void runtime::set_shadow_map_size(uint32_t const size) noexcept {
        // Startup-only: everything that consumes the size (the layered image + its views + the
        // descriptor, the depth pass rendering instance, the pipeline viewport, the light UBO texel
        // size and the fit) is built from it when the scene block is first created, so a change after
        // that cannot take effect - say so instead of pretending otherwise.
        if (this->pass_ready("shadow") || !this->shadow_images.empty()) {
            deren::utility::log("runtime: set_shadow_map_size({}) ignored - the shadow resources already exist (set it before the scene import)", size);
            return;
        }
        uint32_t clamped = std::clamp(size, 256u, 8192u);
        uint32_t rounded = 256u;
        while (rounded * 2u <= clamped) {
            rounded *= 2u;
        }
        if (rounded != size) {
            deren::utility::log("runtime: shadow map size {} -> {} (clamped to 256..8192 and rounded to a power of two)", size, rounded);
        }
        this->shadow_map_size = rounded;
        ++this->shadow_content_version; // a new map size reallocates the images: every slot must render again
    }

    runtime::feature_facts runtime::make_feature_facts() const noexcept {
        // EVERY FIELD IS THE RENDERER'S OWN (see the struct's docs): its pipelines, its frame's content, the device,
        // the knobs that its own policy also reads. What a PASS answers about itself is deliberately absent - the
        // owner of the passes asks them.
        core const& vk = this->vulkan_core;
        return feature_facts{
            .gbuffer_pass = this->gbuffer_pass_active(),
            .deferred_lit = this->deferred_lit_active(),
            .megalights = this->megalights_active(),
            .rt_shadow = this->rt_shadows_active(),
            .fxaa = this->post_fxaa_active(),
            // THE RESOLVE'S OWN ATOM (see `post_upscale_active`): below render_scale 1.0 and with its pipeline
            // built, this pass is the frame's last writer - and that is also what makes `post_fxaa_active` false
            // on those frames, so the two atoms are the two halves of one exclusion rather than two switches.
            .upscale = this->post_upscale_active(),
            .gbuffer_debug = this->gbuffer_debug,
            .shadow = this->shadow_enabled && this->shadows_enabled,
            .clustered = this->clustered_lights,
            .taa = this->taa_on,
            .bloom = this->bloom_intensity > 0.0f,
            .transparent_pending = !this->frame_transparent.empty(),
            // THE TOON STAGE'S GATE, and it is the knob AND the frame's content for the same reason `.shadow`
            // is the checkbox and `enable_shadows()`: a frame with no opaque geometry has no surface for the
            // pass to re-shade, and a scene with no toon character must not pay for the stage at all. When it
            // is false the pass's `feature()` answers inactive and the runner never resolves its declaration -
            // which is exactly what keeps the fourteen capture-gate scenarios byte-identical while it is off.
            //
            // EITHER LEAF LIST KEEPS IT ALIVE, and the overlay half is not a formality: a character whose only
            // remaining geometry this frame is the article's two masks (an extreme close-up can cull everything
            // else) still has something for the pass to multiply, and a gate that only looked at `frame_visible`
            // would drop it. See `character_forward_pass::record`, which makes the same test from its side.
            .character_forward_pending = this->character_forward_on && (!this->frame_visible.empty() || !this->frame_overlay.empty()),
            .gbuffer_pipeline = this->gbuffer_pipeline_mesh.has_value() || this->gbuffer_pipeline_meshlet.has_value(),
            .structures_ready = this->structures.ready() && this->structures.handle(static_cast<uint32_t>(vk.current_frame)) != VK_NULL_HANDLE,
            .furnace = this->furnace,
            .punctual_lights = this->light_state.light_count.x,
        };
    }

    runtime::render_features runtime::active_features() const noexcept {
        // THE TABLE IS THE CHAIN OWNER'S NOW (see chain_wiring::feature_active): this renderer reports the facts and
        // asks. With no owner wired every feature is off, which is the same "no owner, no frames" answer the stage
        // hooks give - a frame that records nothing.
        render_features f;
        if (this->frame_wiring.feature_active == nullptr) {
            return f;
        }
        feature_facts const facts = this->make_feature_facts();
        auto const ask = [this, &facts](std::string_view const name) { return this->frame_wiring.feature_active(this->frame_wiring.owner, facts, name); };
        f.unlit = ask("unlit");
        f.gbuffer_debug = ask("gbuffer-debug");
        f.shadow = ask("shadow");
        f.rt_shadow = ask("rt_shadow");
        f.clustered = ask("clustered");
        f.taa = ask("taa");
        f.ssao = ask("ssao");
        f.bloom = ask("bloom");
        f.fxaa = ask("fxaa");
        // ... and the resolve, asked the same way: `post_upscale_active` is an ATOM of the facts above and the
        // owner's table relays it (see render_start_demo::feature_active), so the composed answer and the one the
        // frame's target choice reads are the same composition rather than two.
        f.upscale = ask("upscale");
        f.transparent = ask("transparent");
        return f;
    }

    bool runtime::feature_active(std::string_view const name) const noexcept {
        if (this->frame_wiring.feature_active == nullptr) {
            return false;
        }
        return this->frame_wiring.feature_active(this->frame_wiring.owner, this->make_feature_facts(), name);
    }

    void runtime::warn_missing_feature(std::string_view const key, std::string const& message) {
        // Only meaningful once the startup is complete: the app applies the config to the runtime
        // BEFORE the pipelines exist (main sets the toggles, chores then creates the pipelines), so
        // warning there would claim "TAA has no effect" one line above "TAA pipeline
        // created". The scene's resources exist once `ensure_scene_heap_slots` has run, i.e. once the first primitive
        // (or the first recorded frame) has asked for them - which is the moment every optional pipeline exists.
        if (this->cluster_count_buffers.empty()) {
            return;
        }
        // At most once per feature per session: main() mirrors the overlay's state into the runtime
        // every frame, so an unconditional log here would print once per FRAME - which is how a
        // diagnostics feature turns into log spam.
        for (std::string const& seen : this->warned_features) {
            if (seen == key) {
                return;
            }
        }
        this->warned_features.emplace_back(key);
        deren::utility::log("gui: {}", message);
    }

    void runtime::set_gbuffer_debug(bool const enabled) noexcept {
        this->gbuffer_debug = enabled;
        if (enabled && ((!this->gbuffer_pipeline_mesh.has_value() && !this->gbuffer_pipeline_meshlet.has_value()) || !this->pass_ready("gbuffer-debug"))) {
            this->warn_missing_feature("gbuffer-debug", "the G-buffer debug view has no effect: its pipelines were not created (see the startup log)");
        }
    }

    bool runtime::feature_available(std::string_view const name) const noexcept {
        // THE CHAIN OWNER'S ANSWER, for the same reason the table above is: "can this feature run at all this
        // session" is composed from the passes that exist and the renderer's facts, and the owner of the passes is
        // the one that knows both. The overlay asks it to decide what to offer and `log_feature_status()` prints it.
        if (this->frame_wiring.feature_available == nullptr) {
            return false;
        }
        return this->frame_wiring.feature_available(this->frame_wiring.owner, this->make_feature_facts(), name);
    }

    void runtime::log_feature_status() const {
        // One line naming every optional feature, so "why does this switch do nothing?" is answerable
        // from the log alone. `on` means the pipeline exists and the feature CAN run; whether it is
        // currently switched on is the overlay's and the config's business.
        deren::utility::log("features: gbuffer-debug={} megalights={} taa={} fxaa={} shadow={} clustered-lights={}",
                            this->feature_available("gbuffer-debug") ? "on" : "UNAVAILABLE",
                            this->feature_available("megalights") ? "on" : "UNAVAILABLE",
                            this->feature_available("taa") ? "on" : "UNAVAILABLE",
                            this->feature_available("fxaa") ? "on" : "UNAVAILABLE",
                            this->feature_available("shadow") ? "on" : "UNAVAILABLE",
                            this->feature_available("clustered") ? "on" : "UNAVAILABLE");
        if ((!this->gbuffer_pipeline_mesh.has_value() && !this->gbuffer_pipeline_meshlet.has_value()) || !this->pass_ready("deferred")) {
            deren::utility::log("features: the G-buffer pass or its lighting stage was not created, so NO SCENE IS DRAWN this session (see the startup log's 'deferred lighting disabled' line)");
        }
    }

    void runtime::set_max_fps(double const fps) noexcept {
        double const clamped = fps > 0.0 ? fps : 0.0;
        // The demo calls this every frame to mirror the GUI, so an unchanged value must be a no-op:
        // re-arming the deadline here would push it back to the epoch on every frame and the limiter
        // would never wait for anything (which is exactly what the first version did).
        if (clamped == this->max_fps) {
            return;
        }
        this->max_fps = clamped;
        this->next_frame_deadline = {}; // re-arm: the first frame after a change never waits
        if (this->max_fps > 0.0) {
            deren::utility::log("runtime: frame rate limited to {:.1f} fps", this->max_fps);
        } else {
            deren::utility::log("runtime: frame rate limit removed (uncapped)");
        }
    }

    std::string runtime::cpu_timing_summary() const {
        return this->cpu_timings.summary();
    }

    // `set_unlit` IS GONE (the demo's now): it was a pure forwarder to the lighting pass's own flag, and the
    // renderer's features ask the PASS for it through `active_features` - so there was never a second copy here
    // to keep, and nothing in this file read it.

    bool runtime::character_forward_ready() const noexcept {
        std::shared_lock const lock(this->access_mutex);
        return this->mesh_pipelines.contains(character_forward_pipeline_name) || this->meshlet_pipelines.contains(character_forward_pipeline_name);
    }

    bool runtime::goo_toon_ready() const noexcept {
        // The same question asked of the OTHER name, and the same answer's shape: this pipeline goes through the
        // same builder, so it exists exactly when the mesh stage does (see `make_character_forward_frame`, which
        // is where the two names are chosen between and which asks this itself rather than trusting the knob).
        std::shared_lock const lock(this->access_mutex);
        return this->mesh_pipelines.contains(goo_toon_pipeline_name) || this->meshlet_pipelines.contains(goo_toon_pipeline_name);
    }

    bool runtime::goo_toon_active() const noexcept {
        // THE FRAME'S OWN PREDICATE, and the comment below is why it is here rather than recomposed by the asker:
        // `make_character_forward_frame` picks the pipeline with exactly this expression, so the stage that has to
        // stay silent while the rewritten chain draws asks the same question the choice was made with.
        std::shared_lock const lock(this->access_mutex);
        return this->goo_toon_on && (this->mesh_pipelines.contains(goo_toon_pipeline_name) || this->meshlet_pipelines.contains(goo_toon_pipeline_name));
    }

    void runtime::set_toon_lookup(toon_lookup const& lookup) noexcept {
        // A SOURCE, not per-frame state: it is read while `import_scene` builds each primitive's create info, so
        // installing one AFTER an import changes nothing about what was already imported - which is what the
        // declaration says, rather than something a caller has to discover.
        this->toon_lookup_source = lookup;
    }

    void runtime::set_character_forward(bool const enabled) noexcept {
        // CPU-side only, like set_clustered_lights: the flag rides `feature_facts::character_forward_pending`,
        // which `make_feature_facts` composes per frame and the pass's own `feature()` reads - so the next
        // frame's stage sees it and no in-flight recording is disturbed.
        this->character_forward_on = enabled;
        if (enabled && !(this->mesh_pipelines.contains(character_forward_pipeline_name) || this->meshlet_pipelines.contains(character_forward_pipeline_name))) {
            // The knob is on but there is no pipeline to draw with: say why rather than leaving a frame that
            // silently keeps the deferred shading (the same answer `warn_missing_feature` gives elsewhere).
            this->warn_missing_feature("character_forward", "the toon character stage has no effect: the character-forward pipeline was not created (see the startup log - it needs the mesh stage and the device's VK_EXT_mesh_shader)");
        }
    }

    void runtime::set_goo_toon(bool const enabled) noexcept {
        // CPU-side only, on `set_character_forward`'s terms: the flag is read while the next frame's
        // `character_forward_frame` is COMPOSED (see `make_character_forward_frame`, which is where the pipeline
        // name is chosen), so no in-flight recording is touched.
        this->goo_toon_on = enabled;
        if (enabled && !(this->mesh_pipelines.contains(goo_toon_pipeline_name) || this->meshlet_pipelines.contains(goo_toon_pipeline_name))) {
            // The knob is on but there is no pipeline to draw with: say so rather than leaving a frame that looks
            // like the old chain and no line explaining why. NOTE WHAT THIS IS NOT: it is not a warning that the
            // character stage is missing. With `character_forward` also on, that stage still runs - it simply
            // draws with the OLD pipeline, which is a valid frame and a wrong experiment.
            this->warn_missing_feature("goo_toon", "the rewritten toon chain has no effect: the goo_toon pipeline was not created (see the startup log - it needs the mesh stage and the device's VK_EXT_mesh_shader), so the character stage draws with character_forward.slang");
        }
    }

    void runtime::set_clustered_lights(bool const enabled) noexcept {
        // CPU-side only, like set_brdf_model: the flag rides light_state's cluster_grid.w lane and
        // pace_and_acquire() copies light_state into the paced slot's buffer, so the next frame's
        // cluster dispatch and shading both see it (no in-flight buffer is touched).
        this->clustered_lights = enabled;
        if (enabled && !this->pass_ready("cluster")) {
            this->warn_missing_feature("clustered", "clustered light culling has no effect: the cluster compute pipeline was not created, so the shading stage loops EVERY active light instead (see the startup log)");
        }
    }

    // THE CLUSTERED-LIGHT SORT'S RESOLVER IS GONE (S3), and it needed nothing but the mechanism: its two barrier
    // BUFFERS come from the frame's resource table (per-frame-slot instance), its shared scene block from the owner,
    // its pipeline from the PASS (which owns it), its extent from the declaration's `none` rule and its push block
    // from nowhere - it has none. Its old gates are answered by the two mechanisms that own those questions: "the
    // pipeline exists" is `pass.pipeline()` (a null pipeline is what the generic resolution fails on), "the
    // buffers are there" is the resource table, "the scene block is there" is the shared-set rule, and "the grid was
    // computed this frame" is the pass's own `frame_.cluster_count == 0` guard, which its `record` already had.

    // Tighten the directional shadow frustum to the camera's own view frustum every frame. One
    // 2048^2 map cannot cover a whole scene and still resolve a thin caster: the orthographic box
    // therefore follows the camera - its xy footprint is the camera frustum's, and the depth range
    // covers the frustum corners plus every caster whose shadow column can reach the view (a wall
    // behind the camera included). The box center is snapped to the texel grid, which is what keeps
    // the shadow edges from crawling while the camera moves.
    bool runtime::instanced_world_aabb(primitive const& leaf, glm::vec3& wmin, glm::vec3& wmax) const {
        // Instanced draws have no single world AABB (primitive::has_bounds is false for them), but
        // their bounds are perfectly computable: pbr.vert uses instances[instance_base + i] as the
        // whole world matrix (push flag bit0), so the union over the instance slice of the SOURCE
        // geometry's local AABB is exact. Without this the fit had to assume "anywhere in the scene"
        // and a single instanced draw (the grid stress mode places instances outside the scene
        // bounds) both lost its own shadows and coarsened everyone else's.
        if ((leaf.push.flags & 1u) == 0u || this->instance_mapped == nullptr) {
            return false;
        }
        auto const& instanced = static_cast<instanced_draw_primitive const&>(leaf);
        primitive const* const source = instanced.source;
        if (source == nullptr || !source->has_bounds || instanced.instance_count == 0) {
            return false;
        }
        std::size_t const base = leaf.push.instance_base;
        if (base + instanced.instance_count > deren::vulkan::instance_capacity) {
            return false; // slice outside the shared buffer: cannot read it
        }
        auto const* matrices = static_cast<glm::mat4 const*>(this->instance_mapped);
        glm::vec3 const lo = source->local_aabb_min;
        glm::vec3 const hi = source->local_aabb_max;
        wmin = glm::vec3(std::numeric_limits<float>::max());
        wmax = glm::vec3(std::numeric_limits<float>::lowest());
        for (uint32_t i = 0; i < instanced.instance_count; ++i) {
            glm::mat4 const& model = matrices[base + i];
            for (int32_t corner = 0; corner < 8; ++corner) {
                glm::vec3 const p((corner & 1) != 0 ? hi.x : lo.x,
                                  (corner & 2) != 0 ? hi.y : lo.y,
                                  (corner & 4) != 0 ? hi.z : lo.z);
                glm::vec3 const world = glm::vec3(model * glm::vec4(p, 1.0f));
                wmin = glm::min(wmin, world);
                wmax = glm::max(wmax, world);
            }
        }
        return true;
    }

    void runtime::update_shadow_frustum() {
        if (!this->shadows_enabled) {
            return;
        }
        // Refit only when the result can change: this walks every leaf and transforms 8 corners per
        // caster (tens of thousands of transforms on a heavy scene), which is exactly the cost the
        // BVH caster cull exists to avoid. The camera matrices are the fit's only inputs besides the
        // scene, so comparing them (plus the scene-changed flag) is the complete condition.
        //
        // The UNJITTERED view-projection is what this must key on and fit to: the TAA jitter is a
        // sub-pixel rendering offset, and letting it into the fit (a) misses the cache on every single
        // frame and (b) - the real bug - re-quantizes the light-space box to whole texels every frame,
        // so the shadow map's texel grid alternates between two alignments. A surface at a grazing
        // light angle then reads as shadowed in one alignment and lit in the other, and the TAA
        // history averages the flicker into a dark band across it.
        if (this->shadow_frustum_valid && !this->bvh_dirty &&
            this->shadow_fit_view_proj == this->current_ubo.view_proj_unjittered) {
            return;
        }
        this->shadow_fit_view_proj = this->current_ubo.view_proj_unjittered;
        ++this->shadow_content_version; // a refit changes the maps: every slot must render again
        this->shadow_frustum_valid = true;
        if (!(this->current_aspect > 0.0f) || this->current_ubo.proj[2][2] == 0.0f) {
            // Degenerate camera (the very first frames, before the swapchain has an extent): the
            // frustum corners would come out of an inverse of a singular matrix and the fit would be
            // garbage (measured: a light-space z span of 1631 for two unrelated scenes). Keep the
            // enable_shadows() default this frame and try again next frame.
            this->shadow_frustum_valid = false;
            return;
        }

        glm::vec3 const light_dir = glm::normalize(glm::vec3(this->light_state.light_dir));
        // ---- gather: every caster's light-space AABB, computed ONCE for all cascades ----
        // Casters outside the view still cast into it (a wall behind the camera): fitting only the
        // frustum corners clipped them and sunlight leaked through. Every scene leaf's world AABB is
        // collected here and handed to the fit, which decides per cascade which of them can reach it.
        // Geometry whose shadow cannot reach the view (e.g. the floor slab behind the camera) is left
        // out there.
        this->shadow_caster_world_boxes.clear();
        bool unbounded_caster = false;
        if (this->bound_scene != nullptr) {
            this->shadow_caster_scratch.clear();
            for (scene_tree::scene_node const& root : this->bound_scene->roots) {
                collect_leaf_primitives(root, this->shadow_caster_scratch);
            }
            this->shadow_caster_world_boxes.reserve(this->shadow_caster_scratch.size());
            for (primitive const* const leaf : this->shadow_caster_scratch) {
                if (leaf == nullptr) {
                    continue;
                }
                // THE FRAME'S STATIC SURROUND IS NOT FIT FOR (`primitive::environment`, the `[render]
                // background_glb` import): a 34 m ground disc and a backdrop dome tens of metres across would
                // drag the caster fit out to their own size, and the shadow map would spend its texels on
                // geometry that cannot cast anything the camera sees. Leaving it out keeps the fit on the
                // subject - the ground still RECEIVES the subject's shadow wherever that (unchanged) fit
                // covers it, which is the only thing the surround is here for.
                if (leaf->environment) {
                    continue;
                }
                glm::vec3 wmin = {};
                glm::vec3 wmax = {};
                if (leaf->has_bounds) {
                    std::tie(wmin, wmax) = leaf->world_aabb();
                } else if (instanced_world_aabb(*leaf, wmin, wmax)) {
                    // exact bounds, computed from this leaf's own instance matrices
                } else {
                    // a caster whose geometry we genuinely cannot bound: the fit falls back to the
                    // scene sphere below, which is sound but costs resolution
                    unbounded_caster = true;
                    continue;
                }
                this->shadow_caster_world_boxes.emplace_back(wmin, wmax);
            }
        }
        this->shadow_caster_boxes = shadow_fit::fit_casters(this->shadow_caster_world_boxes, light_dir, unbounded_caster);

        // ---- fit: the pure part (deren.vulkan.shadow_fit) ----
        shadow_fit::fit_params params = {};
        params.proj = this->current_proj_unjittered;
        params.view = this->current_ubo.view;
        params.camera_pos = glm::vec3(this->current_ubo.camera_pos);
        params.light_dir = this->light_state.light_dir;
        params.scene_center = this->shadow_scene_center;
        params.scene_radius = this->scene_extent_radius;
        params.caster_extent = this->shadow_caster_extent;
        params.cascades = this->shadow_cascades;
        params.map_size = this->shadow_map_size;
        params.unbounded_caster = unbounded_caster;
        params.log_summary = !this->shadow_cascade_logged;
        params.caster_boxes = this->shadow_caster_boxes;
        shadow_fit::fit_result const fitted = shadow_fit::fit(params);
        if (!fitted.valid) {
            this->shadow_frustum_valid = false;
            return;
        }
        this->shadow_cascade_logged = true;

        // the fit produces exactly the lanes the shader reads; the rest of the UBO is untouched
        this->light_state.light_view_proj = fitted.light_view_proj;
        this->light_state.cascade_splits = glm::vec4(fitted.cascade_splits[0], fitted.cascade_splits[1], fitted.cascade_splits[2], fitted.cascade_splits[3]);
        this->light_state.cascade_texel_world = glm::vec4(fitted.cascade_texel_world[0], fitted.cascade_texel_world[1], fitted.cascade_texel_world[2], fitted.cascade_texel_world[3]);
        this->light_state.cascade_count = static_cast<float>(fitted.cascade_count);
        this->light_state.light_dir = glm::vec4(fitted.light_dir, 1.0f / static_cast<float>(this->shadow_map_size));
    }
    void runtime::set_shadow_cascades(uint32_t const cascades) noexcept {
        uint32_t const clamped = std::clamp(cascades, 1u, deren::vulkan::max_shadow_cascades);
        if (clamped == this->shadow_cascades) {
            return;
        }
        this->shadow_cascades = clamped;
        ++this->shadow_content_version; // a different cascade count refits the splits
        // Growing past the layers we own has to rebuild the images; the heap slot is rewritten
        // afterwards because it holds their array view. Shrinking keeps the layers (no second
        // rebuild when the user cycles the combo, and the spare ones simply go unused).
        if (!this->shadow_images.empty() && clamped > this->shadow_allocated_layers) {
            vkDeviceWaitIdle(this->vulkan_core.logical_device);
            this->shadow_images.clear();
            this->shadow_array_views.clear();
            this->shadow_layer_views.clear();
            this->ensure_shadow_resources();
            this->write_light_and_shadow_bindings();
        }
        // a different cascade layout invalidates the cached fit (and the one-time density log)
        this->shadow_frustum_valid = false;
        this->shadow_cascade_logged = false;
        deren::utility::log("shadow cascades set to {}", clamped);
    }

    void runtime::set_shadow_cascade_blend(float const blend) noexcept {
        // a band wider than half a cascade would reach back into the previous one
        this->shadow_cascade_blend = std::clamp(blend, 0.0f, 0.5f);
        this->light_state.cascade_blend = this->shadow_cascade_blend;
    }
    void runtime::enable_shadows(glm::vec3 const& scene_center, float const scene_radius) {
        // Remember the scene extent even if shadow setup below fails: the camera far plane
        // (make_orbit_camera_ubo) needs it to keep the whole scene visible when zooming in.
        this->scene_extent_radius = scene_radius;
        // shadow caster culling (begin_recording): casters up to ~1/8 of the scene radius
        // up-light of the camera frustum can still throw a shadow into the view
        this->shadow_caster_extent = std::max(1.0f, scene_radius * 0.125f);
        // remembered for update_shadow_frustum's fallback fit (a caster without its own world AABB)
        this->shadow_scene_center = scene_center;
        // a new light setup invalidates the cached fit (see update_shadow_frustum)
        this->shadow_frustum_valid = false;
        if (!this->pass_ready("shadow") || this->light_mapped.empty()) {
            deren::utility::log("shadow mapping not enabled (no shadow pipeline / light buffer)");
            return;
        }
        // light UBO: orthographic light view-proj framing the scene + the light direction.
        // Fill the CPU-side mirror only - pace_and_acquire copies it into every slot's own
        // light buffer as each slot is paced (nothing here touches mapped memory directly).
        this->light_state = make_directional_light_ubo(this->sun_direction, scene_center, scene_radius, static_cast<float>(this->shadow_map_size));
        // the cascade settings are the runtime's, not the UBO builder's: re-apply them over the defaults
        this->light_state.cascade_count = static_cast<float>(std::clamp(this->shadow_cascades, 1u, deren::vulkan::max_shadow_cascades));
        this->light_state.cascade_blend = this->shadow_cascade_blend;
        // respect the current GUI toggle: the flag in the slot's buffer tells pbr.frag whether
        // the depth map was rendered this frame
        this->light_state.shadow_enabled = this->shadow_enabled ? 1.0f : 0.0f;
        this->shadows_enabled = true;
        deren::utility::log("shadow mapping enabled: light frustum center ({:.2f}, {:.2f}, {:.2f}), radius {:.2f}",
                            scene_center.x, scene_center.y, scene_center.z, scene_radius);
    }

    void runtime::set_rt_shadows(bool const enabled) noexcept {
        this->rt_shadows = enabled;
        // The lighting stage reads this from the light UBO, so the flag has to be settled before the
        // next frame's paced write - which is why it is set here rather than recomputed per frame. The
        // device check and the pipeline check are folded in: a request that cannot be honoured leaves the
        // cascaded shadow maps running, and the shader never even looks at the visibility image.
        this->light_state.rt_shadows = (enabled && this->pass_ready("rt_shadow") && this->vulkan_core.ray_query_available) ? 1.0f : 0.0f;
    }

    void runtime::set_rt_mask_bake(bool const enabled) noexcept {
        // Read once, when the structures are built (see ray_tracing::structure_set::build): the bake is startup
        // work and the structures are built once, so this can only be settled before the first traced frame.
        this->rt_mask_bake = enabled;
    }

    void runtime::set_rt_skin_bake(bool const enabled) noexcept {
        // Unlike the mask bake this is read EVERY frame (the pass runs per frame), so it can be toggled at
        // any time: turning it off leaves the structures holding the last pose the pass wrote, which is the
        // A/B's whole point - the traced shadow either follows the animation or it does not.
        this->rt_skin_bake = enabled;
    }

    bool runtime::rt_structures_wanted() const noexcept {
        // The ray-traced sun is the only reason a frame needs a top level structure: the binding is written
        // whenever it is wanted, whether or not the traced branch runs.
        return this->vulkan_core.ray_query_available && this->rt_shadows;
    }

    bool runtime::rt_shadows_active() const noexcept {
        return this->rt_shadows && this->vulkan_core.ray_query_available;
    }

    // =============================================================================================
    // THE STRUCTURE PHASE'S SEAM (see deren.vulkan.ray_tracing): what this renderer hands the phase, and the four
    // hooks that let the phase drive the two jobs this class still owns.
    // =============================================================================================

    ray_tracing::build_inputs runtime::make_structure_inputs() const noexcept {
        // THE FIVE THINGS THE PHASE CANNOT GET ITSELF, and nothing else: the casters this frame's culling
        // produced (the SHADOW caster set, because a caster can sit off screen and still throw a shadow into the
        // view), the material table the alphaMode MASK rule reads - as a span, so an out-of-range index is a size
        // check rather than arithmetic on a mapped pointer - the two knobs, and the two jobs.
        auto const* const materials = static_cast<material_record const*>(this->material_mapped);
        return ray_tracing::build_inputs{
            .casters = this->shadow_casters,
            .materials = materials != nullptr ? std::span<material_record const>(materials, this->material_count) : std::span<material_record const>{},
            .mask_bake = this->rt_mask_bake,
            .skin_bake = this->rt_skin_bake,
            .hooks = ray_tracing::bake_hooks{.owner = const_cast<runtime*>(this),
                                             .mask_ready = &runtime::structure_mask_ready,
                                             .record_mask_bake = &runtime::structure_record_mask_bake,
                                             .skin_ready = &runtime::structure_skin_ready,
                                             .record_skin = &runtime::structure_record_skin},
        };
    }

    bool runtime::structure_mask_ready(void* const owner) noexcept {
        return static_cast<runtime*>(owner)->mask_bake.ready();
    }

    namespace {
        /// Push @p bytes and then @p lanes index lanes (see runtime::push_stage_block). The three endpoints differ
        /// only in that count, because a stage's shader declares exactly as many lanes as it reads: the post chain
        /// three (its source slot included), everything else two, the mask bake none.
        bool push_with_lanes(core const& vk, uint32_t const frame_slot, uint32_t const image_index, VkCommandBuffer const command_buffer,
                             std::span<std::byte const> const bytes, uint32_t const extra_lane, std::size_t const lanes) {
            constexpr std::size_t window = 256; // maxPushDataSize on this device (see heap_limits)
            std::array<std::byte, window> staging = {};
            std::size_t const lane_bytes = lanes * sizeof(uint32_t);
            if (bytes.size() + lane_bytes > staging.size()) {
                deren::utility::log("heap push: a block of {} B plus {} index lanes does not fit the push-data window", bytes.size(), lanes);
                return false;
            }
            std::memcpy(staging.data(), bytes.data(), bytes.size());
            std::array<uint32_t, 3> const indices = {frame_slot, image_index, extra_lane};
            std::memcpy(staging.data() + bytes.size(), indices.data(), lane_bytes);
            return vk.descriptor_heaps.push_data(command_buffer, 0u, std::span<std::byte const>(staging.data(), bytes.size() + lane_bytes));
        }
    } // namespace

    bool runtime::push_stage_block(void* const owner, VkCommandBuffer const command_buffer, std::span<std::byte const> const bytes, uint32_t const extra_lane) {
        // THE INDICES ARE APPENDED HERE, and that placement is the whole trick: the renderer knows the frame slot and
        // the swapchain image, a pass knows neither, and a converted stage's shader declares them as its block's
        // LAST two fields (see shaders/heap_slots.glsl). Doing it here is what keeps every pass's push struct - and
        // its static_assert on the size - untouched. The optional third lane is the post chain's own source slot,
        // which only that chain's passes can name.
        runtime* const self = static_cast<runtime*>(owner);
        // THE THIRD LANE IS RESOLVED INTO A SLOT HERE, because it is the one fact that needs both sides: a post
        // stage knows WHICH source it reads (0 = the HDR target every post chain starts from, N > 0 = bloom level
        // N - 1, which is what a downsample at level N reads), and the renderer knows WHERE the grid puts those
        // images. Neither half is useful to the other, which is why the lane crosses here. The shader indexes
        // `post_source_texture[pc.post_source_slot]` with an ABSOLUTE slot, so the image index is added here too -
        // and the per-level stride is heap_image_capacity, the same 8 slots every per-swapchain-image array is
        // spaced by.
        uint32_t const source_slot = extra_lane == 0u
                                         ? core::heap_slots::post_color + self->current_image_index
                                         : core::heap_slots::bloom_l0 + (extra_lane - 1u) * core::heap_image_capacity + self->current_image_index;
        return push_with_lanes(self->vulkan_core, static_cast<uint32_t>(self->vulkan_core.current_frame), self->current_image_index, command_buffer, bytes, source_slot, 3u);
    }

    bool runtime::push_index_block(void* const owner, VkCommandBuffer const command_buffer, std::span<std::byte const> const bytes, uint32_t const extra_lane) {
        runtime* const self = static_cast<runtime*>(owner);
        return push_with_lanes(self->vulkan_core, static_cast<uint32_t>(self->vulkan_core.current_frame), self->current_image_index, command_buffer, bytes, extra_lane, 2u);
    }

    bool runtime::push_raw_block(void* const owner, VkCommandBuffer const command_buffer, std::span<std::byte const> const bytes) {
        runtime* const self = static_cast<runtime*>(owner);
        return push_with_lanes(self->vulkan_core, 0u, 0u, command_buffer, bytes, 0u, 0u);
    }

    // ---- the MESH session's endpoints (docs/mesh_shaders.md step 1): what a draw without an input assembler
    //      needs and only the device's owner can answer. ----

    VkDeviceAddress runtime::mesh_buffer_address(void* const owner, rhi::buffer const& buffer) {
        runtime* const self = static_cast<runtime*>(owner);
        // THE HOOK TAKES A CONTRACT BUFFER (render_environment's own note says why: the primitive's geometry is
        // `object_manager` members now, and a hook that demanded a raw handle would force every primitive to keep
        // one - i.e. to keep reaching into the backend for exactly what this migration removes).
        //
        // A buffer carries an address only when it was created with `buffer_flag::device_address`, which the
        // primitive upload asks for unconditionally - so a zero here is "this backend cannot serve addresses",
        // and the caller reports the caster rather than drawing from address 0.
        return self->buffer_address(buffer);
    }

    bool runtime::push_geometry_block(void* const owner, VkCommandBuffer const command_buffer, uint32_t const offset, std::span<std::byte const> const bytes) {
        runtime* const self = static_cast<runtime*>(owner);
        // A raw push at an offset the STAGE declares (see mesh_geometry_offset): the block `push_stage_block` sends
        // already ends with the three heap index lanes, so the geometry lanes of a mesh stage's block cannot ride
        // along with it - they are appended after them, which is a second push rather than a second block.
        return self->vulkan_core.descriptor_heaps.push_data(command_buffer, offset, bytes);
    }

    bool runtime::draw_mesh_tasks(void* const owner, VkCommandBuffer const command_buffer, uint32_t const groups_x, uint32_t const groups_y, uint32_t const groups_z) {
        runtime* const self = static_cast<runtime*>(owner);
        core const& vk = self->vulkan_core;
        if (vk.mesh_dispatch == nullptr) {
            // Unreachable while the mesh path is gated on the capability (see runtime::create_passes), and answered
            // rather than asserted: a dispatch that cannot be recorded draws NOTHING, which is the same picture a
            // caster with no geometry produces - and it is logged, so it cannot pass unnoticed.
            deren::utility::log("mesh dispatch: the device has no vkCmdDrawMeshTasksEXT, so the dispatch was skipped");
            return false;
        }
        vk.mesh_dispatch(command_buffer, groups_x, groups_y, groups_z);
        return true;
    }

    // THE INDIRECT DISPATCH (docs/mesh_shaders.md step 3, second mechanism): the three counts travel through a
    // buffer, at the primitive's OWN slot, so a COMPUTE culling pass can rewrite them later without the recording
    // path changing. The counts written here are exactly the ones the direct call would have carried, which is why
    // the frame must not move - and why the acceptance is the byte-identical gate PLUS a forced probe.
    //
    // IT ANSWERS RATHER THAN ASSERTS when the route is unavailable, and the reasons are all logged once: a silent
    // fall-back is a seam nobody tests, which is exactly how the first version of this (entry point resolved,
    // buffer never bound) went unnoticed for a whole round.
    bool runtime::draw_mesh_tasks_indirect(void* const owner, VkCommandBuffer const command_buffer, uint32_t const command_slot, uint32_t const groups_x, uint32_t const groups_y, uint32_t const groups_z) {
        runtime* const self = static_cast<runtime*>(owner);
        core const& vk = self->vulkan_core;
        auto const direct = [&]() {
            self->mesh_indirect_direct_fallbacks.fetch_add(1u, std::memory_order_relaxed);
            return runtime::draw_mesh_tasks(owner, command_buffer, groups_x, groups_y, groups_z);
        };
        if (vk.mesh_dispatch_indirect == nullptr || self->mesh_indirect_mapped == nullptr || self->mesh_indirect_table == VK_NULL_HANDLE) {
            if (!self->mesh_indirect_route_logged) {
                self->mesh_indirect_route_logged = true;
                deren::utility::log("mesh indirect: entry point {}, table {}, mapped {} - the meshlet dispatches go through the DIRECT call",
                                    vk.mesh_dispatch_indirect != nullptr ? "resolved" : "MISSING",
                                    self->mesh_indirect_table != VK_NULL_HANDLE ? "bound" : "missing",
                                    self->mesh_indirect_mapped != nullptr ? "yes" : "no");
            }
            return direct();
        }
        // A slot IS a meshlet record index plus its command class, so one past the table's two classes is impossible -
        // and answered rather than trusted, because the command would make the GPU read whatever follows the table.
        if (command_slot >= runtime::mesh_command_capacity) {
            if (!self->mesh_indirect_conflict_logged) {
                self->mesh_indirect_conflict_logged = true;
                deren::utility::log("mesh indirect: slot {} is past the command table ({} records) - the dispatch goes through the DIRECT call", command_slot, runtime::mesh_command_capacity);
            }
            return direct();
        }
        auto* const commands = static_cast<VkDrawMeshTasksIndirectCommandEXT*>(self->mesh_indirect_mapped);
        // THE WRITE IS UNCONDITIONAL, and that is a measured decision rather than a shortcut. A slot belongs to one
        // primitive, and every writer of it writes the SAME bytes: the group count is the primitive's meshlet run
        // (cut once, at import) and the instance count is set when the draw primitive is created - so the shadow
        // pass's parallel cascades and the G-buffer pass all write one command, and identical concurrent stores
        // converge on it whatever the interleaving. The GPU reads the record long after recording ends (and the
        // frame-slot wait is what says the previous frame's read is over), so a torn read is not a case that exists.
        //
        // A COMPARE-AND-FALL-BACK GUARD WAS TRIED AND REMOVED: it read the slot first, and with the shadow pass's
        // cascades recording in parallel it saw another thread's half-finished store, reported "slot 0 already holds
        // 4x1x1 and this dispatch wants 4x1x1" (the two counts are re-read for the log, which is why they printed
        // equal) and sent one dispatch per frame down the DIRECT call. The invariant above makes the check
        // impossible to write without that race, and it would hide the seam rather than protect it.
        //
        // IF THE COUNTS EVER BECOME PER-FRAME (an animated instance count, say), this slot is the wrong key: it
        // needs one command per (frame, draw) instead of per primitive, which is exactly what the COMPUTE culling
        // pass this seam exists for will write.
        commands[static_cast<std::size_t>(vk.current_frame) * runtime::mesh_command_capacity + command_slot] =
            VkDrawMeshTasksIndirectCommandEXT{.groupCountX = groups_x, .groupCountY = groups_y, .groupCountZ = groups_z};
        VkDeviceSize const offset = static_cast<VkDeviceSize>(vk.current_frame * runtime::mesh_command_capacity + command_slot) * sizeof(VkDrawMeshTasksIndirectCommandEXT);
        vk.mesh_dispatch_indirect(command_buffer, self->mesh_indirect_table, offset, 1u, sizeof(VkDrawMeshTasksIndirectCommandEXT));
        self->mesh_indirect_dispatches.fetch_add(1u, std::memory_order_relaxed);
        if (!self->mesh_indirect_route_logged) {
            self->mesh_indirect_route_logged = true;
            deren::utility::log("mesh indirect: the meshlet dispatches go through vkCmdDrawMeshTasksIndirectEXT (table bound, {} records per frame in flight - two command classes - a slot is the primitive's meshlet_base)", runtime::mesh_command_capacity);
        }
        return true;
    }

    // ---- THE HOST'S SIDE OF MESHLET CULLING (docs/mesh_shaders.md step 3, the culling's cheapest stage) ----
    // The camera's `proj * view`, for the recording path's frustum test: the same matrix the stage computes from the
    // camera UBO, taken from the frame's own snapshot so the cull and the draw agree about where the camera is.
    bool runtime::meshlet_view_proj(void* const owner, float* const out16) {
        runtime const* const self = static_cast<runtime const*>(owner);
        if (out16 == nullptr) {
            return false;
        }
        std::memcpy(out16, &self->current_ubo.view_proj_unjittered, sizeof(glm::mat4));
        return true;
    }

    // THIS FRAME'S COMPACTED RUN of a primitive's surviving meshlets. The layout is the shared table's, one frame
    // lane apart, so a workgroup id selects the i-th SURVIVOR directly - and the entry point needs no second lookup.
    bool runtime::meshlet_culled_write(void* const owner, uint32_t const base, std::span<std::byte const> const records) {
        runtime* const self = static_cast<runtime*>(owner);
        if (self->meshlet_culled_mapped == nullptr) {
            return false;
        }
        uint32_t const count = static_cast<uint32_t>(records.size_bytes() / sizeof(deren::vulkan::meshlet));
        if (base > deren::vulkan::meshlet_capacity || count > deren::vulkan::meshlet_capacity - base) {
            // a run past the table cannot be compacted into it: the caller answers by NOT culling (the whole run is
            // dispatched and the entry culls itself), which is always correct - the table is the meshlet budget
            static bool logged = false;
            if (!logged) {
                logged = true;
                deren::utility::log("mesh culling: a run of {} meshlets at {} does not fit the culled table ({} records) - that draw stays unculled", count, base, deren::vulkan::meshlet_capacity);
            }
            return false;
        }
        auto* const table = static_cast<uint8_t*>(self->meshlet_culled_mapped);
        std::memcpy(table + (static_cast<std::size_t>(self->vulkan_core.current_frame) * deren::vulkan::meshlet_capacity + base) * sizeof(deren::vulkan::meshlet), records.data(), records.size_bytes());
        return true;
    }

    void runtime::fill_heap_bind(void* const owner, VkBindHeapInfoEXT& resource, VkBindHeapInfoEXT& sampler) {
        static_cast<runtime*>(owner)->vulkan_core.descriptor_heaps.bind_infos(resource, sampler);
    }

    void runtime::structure_record_mask_bake(void* const owner, VkCommandBuffer const command_buffer, pass::mask_bake_request const& request) {
        static_cast<runtime*>(owner)->mask_bake.record(command_buffer, request, owner, &runtime::push_raw_block);
    }

    bool runtime::structure_skin_ready(void* const owner) noexcept {
        return static_cast<runtime*>(owner)->compute_skin.ready();
    }

    bool runtime::structure_record_skin(void* const owner, VkCommandBuffer const command_buffer, std::span<ray_tracing::caster_level const> const casters) {
        return static_cast<runtime*>(owner)->record_compute_skin_pass(command_buffer, casters);
    }

    void runtime::set_shadow_enabled(bool const enabled) {
        if (this->shadow_enabled == enabled) {
            return;
        }
        this->shadow_enabled = enabled;
        ++this->shadow_content_version;
        // Only the CPU-side flag changes here: the frame loop copies light_state into the paced
        // slot's OWN light buffer (pace_and_acquire), so this call is safe at ANY time (GUI
        // callbacks included) - it never touches memory a frame in flight may be reading. The
        // light UBO's shadow_enabled flag drives pbr.frag: when shadows are off the shader
        // skips calc_shadow entirely (fully lit), so no shadow-map clearing is needed - this
        // avoids the per-frame-slot double-buffer race that clearing once could not fix.
        this->light_state.shadow_enabled = (enabled && this->shadows_enabled) ? 1.0f : 0.0f;
        if (enabled && !this->shadows_enabled) {
            this->warn_missing_feature("shadow-on", "the shadow pass has no effect: enable_shadows() did not succeed (the startup log says why)");
        }
        deren::utility::log("shadow pass {}", enabled ? "enabled" : "disabled");
    }

    void runtime::set_brdf_model(int32_t const model) noexcept {
        // CPU-side only, like set_shadow_enabled: pace_and_acquire() copies light_state (which
        // carries the selected models in the UBO's std140 padding) into the paced slot's light
        // buffer every frame, so flipping the model mid-run never races an in-flight frame.
        this->light_state.brdf_model = static_cast<float>(std::clamp(model, 0, 3));
    }

    void runtime::set_diffuse_model(int32_t const model) noexcept {
        this->light_state.diffuse_model = static_cast<float>(std::clamp(model, 0, 1));
    }

    void runtime::set_toon_rig(toon_rig const& rig) noexcept {
        // A PLAIN COPY INTO THE MAPPED BLOCK, and it is safe for the reason every other once-written table here
        // is (see core::heap_slots::toon_rig): the frame path never rewrites this block, so there is no frame in
        // flight that could read it half-written. The contract that follows from that is the caller's - write it
        // BEFORE the frame loop, not from inside a frame.
        if (this->toon_rig_mapped != nullptr) {
            std::memcpy(this->toon_rig_mapped, &rig, sizeof(rig));
        }
    }

    void runtime::set_head_basis(head_ubo const& basis) noexcept {
        // CPU-side only, the same rule set_exposure and set_brdf_model follow: remember it here and let
        // pace_and_acquire copy it into the paced slot's buffer, so a call from a GUI callback or from the import
        // path can never write memory a frame in flight is reading.
        //
        // THE THREE AXES ARE REJECTED IF THEY ARE DEGENERATE rather than stored: a zero or NaN frame reaching the
        // shader's `atan2` produces a face that is black or flickering with nothing in the log to say why, and the
        // fallback frame is a usable answer where a broken one is not. The reference guards the same case in
        // `EfFaceGetHeadBasis` (`valid < 0.5`).
        glm::vec3 const front = glm::vec3(basis.front);
        glm::vec3 const right = glm::vec3(basis.right);
        glm::vec3 const up = glm::vec3(basis.up);
        float const front_len_sq = glm::dot(front, front);
        float const right_len_sq = glm::dot(right, right);
        float const up_len_sq = glm::dot(up, up);
        // THE FOURTH MEMBER IS A POSITION AND IT IS STORED BEFORE THE GUARD, because the guard is about the AXES:
        // `headCenter` is what `Recalculate normal` subtracts from the fragment's world position, and a frame whose
        // axes were refused still has a centre - keeping the previous one while a new one is known would make the
        // sphere normal jump between two objects' heads.
        this->head_state.center = basis.center;
        if (!std::isfinite(front_len_sq) || !std::isfinite(right_len_sq) || !std::isfinite(up_len_sq) || front_len_sq <= 1e-8f || right_len_sq <= 1e-8f || up_len_sq <= 1e-8f) {
            deren::utility::log("head frame: refusing a degenerate basis (lengths {} {} {}), keeping the fallback", front_len_sq, right_len_sq, up_len_sq);
            return;
        }
        this->head_state.front = glm::vec4(glm::normalize(front), 0.0f);
        this->head_state.right = glm::vec4(glm::normalize(right), 0.0f);
        this->head_state.up = glm::vec4(glm::normalize(up), 0.0f);
    }

    void runtime::set_exposure(float const exposure) noexcept {
        // CPU-side only (same rule as set_brdf_model): remembered here, written into the light
        // UBO's light_count.y lane by pace_and_acquire() and pushed to the skybox pass
        this->exposure_scale = std::clamp(exposure, 0.05f, 20.0f);
    }

    float runtime::exposure() const noexcept {
        return this->exposure_scale;
    }

    void runtime::set_sun_intensity(float const scale) noexcept {
        this->sun_intensity = std::clamp(scale, 0.0f, 3.0f);
    }

    void runtime::set_sun_direction(glm::vec3 const direction) noexcept {
        float const length_sq = glm::dot(direction, direction);
        if (!(length_sq > 1e-12f)) {
            // NO DIRECTION IN A ZERO VECTOR, and normalising one would put NaNs through the shading's light
            // term and the shadow cascade fit. Ignored rather than stored, the same way the furnace mode
            // ignores the intensity slider instead of arguing with it.
            return;
        }
        float const stored_length_sq = std::max(glm::dot(this->sun_direction, this->sun_direction), 1e-12f);
        glm::vec3 const normalized = direction / std::sqrt(length_sq);
        glm::vec3 const previous = this->sun_direction / std::sqrt(stored_length_sq);
        if (glm::length(normalized - previous) < 1e-6f) {
            // THE SAME SUN AGAIN, which is the common case for a caller that mirrors this every frame - and
            // the invalidation below is not free (every shadow map re-renders), so an unchanged direction
            // stops here rather than at the comparison.
            return;
        }
        this->sun_direction = normalized;
        // THE CASCADES ARE FITTED IN LIGHT SPACE, so a cached fit belongs to ONE direction: the same reason a
        // new light setup invalidates it (see update_shadow_frustum).
        this->shadow_frustum_valid = false;
    }

    void runtime::set_area_light(glm::vec3 const& centre, float const half, bool const irradiance, glm::vec3 const& axis, float const penumbra) noexcept {
        if (!(half > 0.0f)) {
            // NO AREA LIGHT: both lanes are written as a LITERAL zero. `glm::vec4(0.0f)` rather than a
            // computed expression because the shader's "is there an emitter" test is a comparison against
            // zero, and a `-0.0f` from a negated zero would still compare equal - but nothing downstream
            // should ever have to know that. This is also the byte-identical path: a frame that never asks
            // for an area light carries the zeros it carried before this call existed.
            this->light_state.area_light = glm::vec4(0.0f);
            this->light_state.area_light_axis = glm::vec4(0.0f);
            return;
        }
        // The sign of area_light.w is the energy switch, because the two appended lanes have no spare boolean
        // and the shader can read a sign for free (see light_ubo's note).
        float const signed_half = irradiance ? half : -half;
        // A zero penumbra is "no penumbra" for the shader; `penumbra` arrives from derive_area_light as
        // max(0, ...) already, and the clamp here is the last line of defence against a caller handing the
        // shading a negative radius.
        this->light_state.area_light = glm::vec4(centre, signed_half);
        this->light_state.area_light_axis = glm::vec4(axis, std::max(penumbra, 0.0f));
    }

    void runtime::set_toon_shading(float const steps, float const softness) noexcept {
        // 0 disables the cel path (plain PBR); the shader rounds to whole bands
        this->toon_steps = steps < 1.5f ? 0.0f : std::round(std::clamp(steps, 2.0f, 8.0f));
        this->toon_softness = std::clamp(softness, 0.01f, 0.5f);
    }

    void runtime::set_bloom(float const intensity, float const threshold) noexcept {
        this->bloom_intensity = std::clamp(intensity, 0.0f, 4.0f);
        // above ~0.75 the scene has almost no pixel brighter than the threshold, so nothing
        // would glow; the gui slider is limited to the same visible range
        this->bloom_threshold = std::clamp(threshold, 0.0f, 0.75f);
    }

    void runtime::set_fxaa(bool const enabled, float const subpixel, float const edge_threshold) noexcept {
        // no pipeline = the shader was never loaded: keep the flag off rather than silently
        // rendering the composite into an LDR image nothing will ever read back
        this->fxaa_on = enabled && this->pass_ready("fxaa");
        if (enabled && !this->fxaa_on) {
            this->warn_missing_feature("fxaa", "FXAA has no effect: the fxaa pipeline was not created (is fxaa.frag.spv present?)");
        }
        // ... AND A CONFIG THAT ASKS FOR FXAA AND A SCALED RENDER CHAIN GETS ONE OF THE TWO, because the two are
        // mutually exclusive rather than stacked (see post_fxaa_active): the upscale resolve is the frame's last
        // writer whenever the chain runs below the output size, so FXAA's render-resolution filter would be
        // resampled away. The flag above stays SET - at render_scale 1.0 (and on a frame whose upscale pipeline
        // is missing) FXAA is the one that runs - and this line is the one place that says which of the two the
        // frame is actually getting. `warn_missing_feature`'s once-per-key rule is what keeps it to one line.
        if (enabled && this->post_upscale_active()) {
            this->warn_missing_feature("upscale", "FXAA is disabled because the render chain is scaled: the upscale resolve is this frame's last writer and would resample an FXAA result away (set render_scale = 1.0 to use FXAA)");
        }
        this->fxaa_subpixel = std::clamp(subpixel, 0.0f, 1.0f);
        // below ~0.05 every shaded gradient counts as an edge (the whole image gets softened),
        // above ~0.5 almost nothing does; the gui slider uses the same range
        this->fxaa_edge_threshold = std::clamp(edge_threshold, 0.05f, 0.5f);
    }

    void runtime::set_point_lights(std::span<punctual_light const> lights) noexcept {
        // CPU-side only (same rule as set_brdf_model): encode into light_state's GPU-layout
        // array; pace_and_acquire() copies the whole light UBO into the paced slot's buffer.
        uint32_t count = 0;
        for (punctual_light const& light : lights) {
            if (count >= max_punctual_lights) {
                break;
            }
            point_light& slot = this->light_state.punctual_lights[count];
            slot.position = glm::vec4(light.position, 0.0f);
            // color already scaled by intensity (radiance units, as the shader expects)
            slot.color = glm::vec4(light.color * light.intensity, 0.0f);
            // a zero-length spot axis would encode NaNs (normalize(0) divides by 0): fall back
            // to the default downward axis so the shader's normalize() stays finite
            float const axis_length = glm::length(light.spot_direction);
            glm::vec3 const axis = axis_length > 1e-6f ? light.spot_direction / axis_length : glm::vec3(0.0f, -1.0f, 0.0f);
            slot.spot_dir = glm::vec4(axis, 0.0f);
            // inner cone: cos of the inner half-angle (glTF KHR innerConeAngle when set), else the
            // legacy soft-inner derivation mix(outer, 1, 0.6) == 0.6 + 0.4 * outer
            float const inner_cos = light.spot_inner_cos.value_or(0.6f + 0.4f * light.spot_outer_cos);
            slot.params = glm::vec4(light.range, light.spot ? 1.0f : 0.0f, light.spot_outer_cos, light.spot ? inner_cos : 0.0f);
            ++count;
        }
        this->light_state.light_count.x = static_cast<float>(count);
    }

    void runtime::log_scene_tree() const noexcept {
        size_t total_nodes = 0;
        size_t leaf_count = 0;
        size_t max_depth = 0;
        std::vector<std::string> lines;
        auto const walk = [&](auto&& self, scene_tree::scene_node const& node, size_t const depth) -> void {
            ++total_nodes;
            max_depth = std::max(max_depth, depth);
            if (node.primitive_leaf != nullptr) {
                ++leaf_count;
            }
            std::string marker = node.primitive_leaf != nullptr ? " [primitive]" : "";
            lines.push_back(std::format("{}{}{}", std::string(depth * 2, ' '),
                                        node.node_name.empty() ? std::string("<unnamed>") : node.node_name, marker));
            for (scene_tree::scene_node const& child : node.children) {
                self(self, child, depth + 1);
            }
        };
        for (scene_tree::scene_node const& root : this->get_scene().roots) {
            walk(walk, root, 0);
        }
        deren::utility::log("runtime scene tree: {} roots, {} nodes ({} leaf primitives), max depth {}", this->get_scene().roots.size(), total_nodes, leaf_count, max_depth);
        for (std::string const& line : lines) {
            deren::utility::log("  {}", line);
        }
    }

    vk_pipeline const* runtime::get_pipeline(std::string_view const pipeline_name) const noexcept {
        std::shared_lock const lock(this->access_mutex);
        // A GEOMETRY NAME LIVES IN THE MESH MAPS NOW (docs/mesh_shaders.md step 4), so the lookup asks all three: the
        // vertex registry is what is left of the non-geometry pipelines that still have one.
        if (auto const it = this->pipelines.find(pipeline_name); it != this->pipelines.end()) {
            return &it->second;
        }
        if (auto const it = this->mesh_pipelines.find(pipeline_name); it != this->mesh_pipelines.end()) {
            return &it->second;
        }
        if (auto const it = this->meshlet_pipelines.find(pipeline_name); it != this->meshlet_pipelines.end()) {
            return &it->second;
        }
        return nullptr;
    }

    std::unique_ptr<primitive> runtime::create_primitive(std::string_view const pipeline_name, primitive_create_info const& info) {
        // The pipeline must exist (the caller names the pipeline this geometry is for), but a
        // normal_draw_primitive has DEFAULT semantics: it does not store the name, it draws with
        // whatever pipeline the recording pass binds as default (render_environment). A custom
        // draw strategy that needs a specific pipeline stores its own name and requests it.
        {
            std::shared_lock const lock(this->access_mutex);
            if (!this->pipelines.contains(pipeline_name) && !this->mesh_pipelines.contains(pipeline_name) && !this->meshlet_pipelines.contains(pipeline_name)) {
                return nullptr;
            }
        }
        this->ensure_scene_heap_slots();

        auto result = std::make_unique<normal_draw_primitive>();

        // ---- geometry buffers ----
        // The vertex and index buffers carry the acceleration-structure build-input usage when the
        // device has ray tracing, so a later build can read them through their device addresses
        // instead of a second copy of the geometry. The flag is the DEVICE's, not the config's: the
        // usage bit needs the extension enabled, and a buffer uploaded without it can never be built
        // from - so it is decided where the upload happens, once, and not per frame by whoever wants
        // to trace. THE BYTES THEMSELVES TRAVEL IN THE DESCRIPTOR (`initial_bytes` IS the upload),
        // which is also what lets the backend's content-keyed registry see them at creation.
        rhi::buffer_flags const geometry_flags = rhi::to_bits(rhi::buffer_flag::device_address) |
                                                 (this->vulkan_core.ray_query_available ? rhi::to_bits(rhi::buffer_flag::acceleration_structure_input) : rhi::no_buffer_flags);
        create_buffer(this->vulkan_core,
                      rhi::buffer_usage::vertex,
                      geometry_flags,
                      std::as_bytes(info.vertex_data),
                      "vertex buffer",
                      result->vertex_buffer,
                      nullptr);
        create_buffer(this->vulkan_core,
                      rhi::buffer_usage::index,
                      geometry_flags,
                      std::as_bytes(info.index_data),
                      "index buffer",
                      result->index_buffer,
                      nullptr);

        result->index_type = info.index_type;
        result->draw_index_count = info.index_count;
        result->vertex_count = info.vertex_count;
        // Kept for the acceleration-structure build, which reads the vertex buffer directly and has to
        // be told the stride the interleaved layout uses (nothing else needs it after the upload: the
        // raster pipelines take it from the vertex input state).
        result->vertex_stride = info.vertex_stride;

        // ---- local-space AABB for frustum culling: the interleaved vertex layout starts every
        //      vertex with a vec3 position (see the loader's vertex struct / pbr.vert), so scan
        //      the CPU copy before it is released by the upload
        // ---- MESHLETS (docs/mesh_shaders.md step 3): the same CPU copy the AABB scan below reads, cut into runs
        //      of at most `meshlet_max_triangles` triangles with an object-space bounding sphere each - the data a
        //      task stage culls with. Built HERE because this is where the geometry bytes are still in hand and
        //      the layout (stride, index width) is known. The GPU TABLE is filled right below, so a task stage can
        //      read the same records the CPU just computed.
        result->meshlets = deren::vulkan::build_meshlets(deren::vulkan::meshlet_build_input{
            .vertex_data = info.vertex_data,
            .vertex_stride = info.vertex_stride,
            .vertex_count = info.vertex_count,
            .index_data = info.index_data,
            .index_width = info.index_type == VK_INDEX_TYPE_UINT16 ? 2u : 4u,
            .first_index = 0u,
            .index_count = info.index_count,
            .base_vertex = 0,
        });
        // ... AND INTO THE TABLE a task stage reads, where this primitive owns a CONTIGUOUS run: it remembers the
        // run's first record (`meshlet_base`) and the geometry lanes carry it, so one draw's meshlets are one
        // window. A scene past `meshlet_capacity` keeps the geometry it uploaded and loses only the CULLING for the
        // overflow - logged once, because a budget that silently drops geometry is a hole rather than a limit.
        std::size_t const room = this->meshlet_total < deren::vulkan::meshlet_capacity ? static_cast<std::size_t>(deren::vulkan::meshlet_capacity) - this->meshlet_total : 0u;
        if (result->meshlets.size() > room) {
            if (!this->meshlet_overflow_logged) {
                this->meshlet_overflow_logged = true;
                deren::utility::log("meshlet table capacity ({}) exceeded - the extra meshlets are not culled (the geometry is unaffected)", deren::vulkan::meshlet_capacity);
            }
            result->meshlets.resize(room);
        }
        std::memcpy(static_cast<uint8_t*>(this->meshlet_mapped) + this->meshlet_total * sizeof(deren::vulkan::meshlet), result->meshlets.data(), result->meshlets.size() * sizeof(deren::vulkan::meshlet));
        result->meshlet_base = static_cast<uint32_t>(this->meshlet_total);
        this->meshlet_total += result->meshlets.size();
        result->meshlet_count = static_cast<uint32_t>(result->meshlets.size());
        if (!result->meshlets.empty()) {
            ++this->meshlet_primitives;
        }
        // ---- AND THE RECORDS ARE CHECKED AS THEY GO OUT, because everything downstream of them trusts them ----
        // A meshlet's window is what a MESH stage passes to `SetMeshOutputCounts` and to its index fetch, so a
        // record that is malformed is not a wrong picture: it is a dispatch asking for more output than the device
        // has (the hang the first consumer attempt measured) or a fetch outside the buffer. THE RULE ITSELF IS
        // `deren::vulkan::meshlet_record_sound` - one definition, asserted by tests/test_meshlet.cpp - and this is the
        // boundary where the records leave the host.
        bool records_sound = true;
        for (deren::vulkan::meshlet const& meshlet : result->meshlets) {
            records_sound = records_sound && deren::vulkan::meshlet_record_sound(meshlet, info.index_count);
        }
        if (!records_sound && !this->meshlet_records_unsound_logged) {
            this->meshlet_records_unsound_logged = true;
            deren::utility::log("meshlet records: a primitive with {} indices produced a meshlet outside its window or over the "
                                "{} index budget - a mesh stage would ask the device for invalid output (the splitter's invariants are tested in tests/test_meshlet.cpp)",
                                info.index_count,
                                deren::vulkan::meshlet_max_indices);
        }

        if (info.vertex_count > 0 && info.vertex_stride >= sizeof(glm::vec3) && !info.vertex_data.empty()) {
            glm::vec3 aabb_min = glm::vec3(std::numeric_limits<float>::infinity());
            glm::vec3 aabb_max = glm::vec3(-std::numeric_limits<float>::infinity());
            auto const* cursor = info.vertex_data.data();
            for (uint32_t v = 0; v < info.vertex_count; ++v) {
                glm::vec3 position;
                std::memcpy(&position, cursor, sizeof(position));
                aabb_min = glm::min(aabb_min, position);
                aabb_max = glm::max(aabb_max, position);
                cursor += info.vertex_stride;
            }
            result->local_aabb_min = aabb_min;
            result->local_aabb_max = aabb_max;
            result->has_bounds = true;
        }

        // ---- material: register textures + append a material record; the primitive only carries
        //         the material index (texture indices / factors / flags live in the GPU table) ----
        result->push.material_index = this->register_material(info);
        result->push.model = info.model_matrix;
        // Its own motion slot, where advance_motion_transforms() will keep the world matrix this
        // leaf had one frame ago - and which pbr.vert reads to build the object half of TAA's motion
        // vector. Allocated here rather than per draw because the shader needs a STABLE index.
        result->motion_slot_index = this->motion_cursor++;
        result->push.motion_base = result->motion_slot_index;
        result->double_sided = info.double_sided;
        result->transparent = info.factors.alpha_blend;
        // ... AND WHICH OVERLAY CHANNEL THE LEAF BELONGS TO (0 == none). Carried onto the primitive because the
        // LEAF LISTS are built from it every frame (see `frame_overlay`): an overlay surface is drawn by the
        // overlay pass and by nothing else, so this has to be known while the lists are being split rather than
        // when a draw is recorded.
        result->overlay_kind = info.overlay_kind;
        // ... AND THIS LEAF'S OUTLINE WIDTH, for the same list-building reason: the frame has to know which
        // leaves the ① outline group draws BEFORE any draw is recorded, and the material's `_OutlineWidth` is
        // otherwise only in the GPU colour-lane table (`core::heap_slots::toon_colours`) where the frame cannot
        // read it. Read out of the SAME `toon_inputs` the shader's lane is built from, so the gate the frame
        // applies and the gate the outline mesh stage applies (`w > 0`) cannot disagree.
        result->outline_width = info.toon.colours[static_cast<std::size_t>(toon_colour_lane::outline_edge)].w;
        // ... AND WHETHER THIS LEAF IS THE FRAME'S STATIC SURROUND (`[render] background_glb`), which is the
        // third list-building fact carried onto the primitive and the only one that is not derived from the
        // material at all: it comes from WHICH IMPORT created the leaf (see `runtime::import_scene`'s
        // `environment` argument). Two lists read it - the toon character stage and the shadow casters -
        // and both are built here, in the host, every frame.
        result->environment = info.environment;
        return result;
    }

    primitive* runtime::make_primitive(std::string_view const pipeline_name, primitive_create_info const& info) {
        std::unique_ptr<primitive> created = this->create_primitive(pipeline_name, info);
        if (created == nullptr) {
            return nullptr;
        }
        primitive* const result = created.get();

        // attach the primitive as a new root leaf of the scene tree; the node's name records the
        // pipeline it draws with (the record path groups leaves by node name / pipeline)
        scene_tree::scene_node& leaf = this->get_scene().add_root();
        leaf.node_name = std::string(pipeline_name);
        leaf.local = info.model_matrix;  // world = identity * local (root)
        leaf.attach(std::move(created)); // a deren::vulkan::primitive is a scene_tree::primitive
        this->bvh_dirty = true;          // new leaf -> culling BVH must be rebuilt
        return result;
    }

    primitive* runtime::make_instanced_primitive(primitive const& source, std::span<glm::mat4 const> const transforms) {
        uint32_t const count = std::min<uint32_t>(static_cast<uint32_t>(transforms.size()), deren::vulkan::instance_capacity - this->instance_cursor);
        if (count == 0 || this->instance_mapped == nullptr || !source.is_valid()) {
            return nullptr;
        }
        this->ensure_scene_heap_slots();

        // The instance buffer is one shared region; THIS primitive gets the slice starting at
        // instance_cursor (mat4 units). Writing only its own slice keeps several instanced
        // primitives from overwriting each other - each one addresses its transforms through
        // push.instance_base in the vertex shaders.
        uint32_t const base = this->instance_cursor;
        this->instance_cursor += count;
        std::memcpy(static_cast<uint8_t*>(this->instance_mapped) + static_cast<size_t>(base) * sizeof(glm::mat4),
                    transforms.data(),
                    static_cast<size_t>(count) * sizeof(glm::mat4));

        // The instanced draw owns `count` motion slots, and gets them filled with the SAME matrices
        // right away: an instance's previous transform is its current one, so its object motion reads
        // as zero. That is correct for a static grid, and it is where a moving instanced draw would
        // have to be handled (advance_motion_transforms skips instanced leaves on purpose: their
        // per-instance matrices are a setup-time quantity, not a per-frame one).
        uint32_t const motion_base = this->motion_cursor;
        uint32_t const motion_count = std::min<uint32_t>(count, deren::vulkan::scene_motion_capacity - this->motion_cursor);
        this->motion_cursor += motion_count;
        for (int32_t slot = 0; slot < deren::vulkan::core::MAX_FRAMES_IN_FLIGHT; ++slot) {
            auto* const published = static_cast<glm::mat4*>(this->motion_mapped[static_cast<std::size_t>(slot)]);
            if (published != nullptr && motion_count > 0) {
                std::memcpy(published + motion_base, transforms.data(), static_cast<std::size_t>(motion_count) * sizeof(glm::mat4));
            }
        }
        std::copy_n(transforms.data(), motion_count, this->motion_previous.begin() + static_cast<std::ptrdiff_t>(motion_base));

        auto result = std::make_unique<instanced_draw_primitive>();
        // same pipeline semantics as the source geometry it draws (empty = default semantics)
        result->pipeline_name = source.pipeline_name;
        result->source = &source; // geometry owner; must stay in this runtime's scene tree
        result->instance_count = count;
        result->push.material_index = source.push.material_index;
        result->push.flags = 1u; // bit0: pbr.vert picks instances[instance_base + gl_InstanceIndex]
        result->push.instance_base = base;
        result->push.motion_base = motion_base; // slots run parallel to the instance block
        result->push.model = glm::mat4(1.0f);
        result->double_sided = source.double_sided;
        result->transparent = source.transparent; // same material semantics as the source geometry
        // ... and the same material's outline width, so a hull of an instanced draw is gated by the material the
        // instances share rather than by the default (see `primitive::outline_width`).
        result->outline_width = source.outline_width;

        scene_tree::scene_node& leaf = this->get_scene().add_root();
        leaf.node_name = "pbr";
        primitive* const created = static_cast<primitive*>(leaf.attach(std::move(result)));
        this->bvh_dirty = true; // new leaf -> culling BVH must be rebuilt
        return created;
    }

    std::vector<primitive const*> runtime::get_primitives(std::string_view const pipeline_name) const noexcept {
        // match by the pipeline a leaf effectively draws with: an explicit pipeline_name, or the
        // runtime default for default-semantics leaves (empty pipeline_name). Snapshot the
        // default under a shared lock (it can change via set_default_pipeline on another thread).
        std::string_view default_name;
        {
            std::shared_lock const lock(this->access_mutex);
            default_name = this->default_pipeline_name;
        }
        auto const effective = [default_name](primitive const& m) -> std::string_view {
            return m.pipeline_name.empty() ? default_name : m.pipeline_name;
        };
        std::vector<primitive const*> result;
        for (scene_tree::scene_node const& root : this->get_scene().roots) {
            // collect every leaf whose primitive draws with the requested pipeline (leaves name
            // their pipeline, or fall back to the runtime default; the tree just organizes them)
            scene_tree::visit_primitives(root, glm::mat4(1.0f), [&](scene_tree::scene_node const& n, glm::mat4 const&) {
                auto const* m = static_cast<primitive const*>(n.primitive_leaf.get());
                if (effective(*m) == pipeline_name) {
                    result.push_back(m);
                }
            });
        }
        return result;
    }

    void runtime::collect_leaf_primitives(scene_tree::scene_node const& node, std::pmr::vector<primitive const*>& out) {
        scene_tree::visit_primitives(node, glm::mat4(1.0f), [&out](scene_tree::scene_node const& n, glm::mat4 const&) {
            out.push_back(static_cast<primitive const*>(n.primitive_leaf.get()));
        });
    }

    void runtime::set_skin_matrices(std::span<glm::mat4 const> const matrices) {
        this->set_skin_matrices(matrices, this->active_slot);
    }

    void runtime::set_skin_matrices(std::span<glm::mat4 const> const matrices, uint32_t const slot) {
        if (slot >= this->skin_mapped.size() || this->skin_mapped[slot] == nullptr) {
            return;
        }
        std::size_t const bytes = std::min(matrices.size_bytes(), static_cast<std::size_t>(deren::vulkan::scene_skin_capacity) * sizeof(glm::mat4));
        std::memcpy(this->skin_mapped[slot], matrices.data(), bytes);
        // Content fingerprint of this upload: the only per-frame signal that a skinned caster moved
        // (its push.model is constant, the pose lives in these matrices). XXH3 rather than a byte
        // loop - 1.3 us for an 840-joint rig against 43.7 us, and this runs on every frame.
        this->skin_matrix_hash = deren::utility::xxh3_64bits({static_cast<uint8_t const*>(this->skin_mapped[slot]), bytes});
    }

    void* runtime::morph_scratch() noexcept {
        return this->morph_scratch(this->active_slot);
    }

    void* runtime::morph_scratch(uint32_t const slot) noexcept {
        if (slot >= this->morph_mapped.size()) {
            return nullptr;
        }
        // relaxed: the fetch_add is only there to make concurrent bumps from the animation
        // controller's worker threads well defined and non-lossy (see the member docs) - the value
        // is read back through a plain load in shadow_geometry_signature(), on the frame thread.
        this->morph_revision.fetch_add(1, std::memory_order_relaxed); // no upload hook: assume the caller is about to deform the mesh
        return this->morph_mapped[slot];
    }
} // namespace deren::vulkan