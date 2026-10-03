// ============================================================================
// module: deren.vulkan.runtime:frames  - the frame pipeline
//
// THE FRAME: pace_and_acquire through render_frame, uninterrupted - the per-frame-slot phases, the
// recording of every stage, the pass plumbing that resolves and verifies them, the resource publication
// and the submit/present handshake. It was the largest single run left in runtime.cpp, and the reason
// that file was worth splitting at all.
//
// Imports are NOT transitive: this partition imports what its own code calls, and repeats the pmr
// keep-alive that must run before any pmr container in this TU.
// ============================================================================
module;

#include <GLFW/glfw3.h>
#include <algorithm> // std::min in the resource publication
#include <bit>       // std::bit_cast for the caster world-matrix hash
#include <chrono>
#include <cstring> // std::memcpy, for composing a pass's push block
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>   // std::as_bytes for the init_utils calls (the bytes behind a UBO or a zeroed table)
#include <thread> // std::this_thread::yield in the frame limiter
#include <vulkan/vulkan.h>

module deren.vulkan.runtime:frames;

import :declarations;

import deren.vulkan.profiling;
import deren.vulkan.pipelines;
import deren.vulkan.bindings;
import deren.vulkan.render_resource;
import deren.vulkan.render_resource.shared;

import deren.utility;
import deren.vulkan.constant_init;
import deren.vulkan.init_utils;      // the resource-creation patterns the init/ensure functions below repeat
import deren.vulkan.frame_constants; // one frame's shared constants (see update_frame_constants)
import deren.vulkan.core.pipeline;   // deren::vulkan::make_pipeline for the post-process pipeline
import deren.promise.rhi;            // the contract's recording surface: the frame's list, image and read-back slot

// Route std::pmr allocations through mimalloc for this TU (deren.utility:better_pmr). Idempotent:
// init_pmr() returns the same process-wide singleton no matter which TU calls it first, so
// main.cpp's keep-alive and this one coexist safely. The reference itself is never read; it
// only forces the (dynamic) initialization before any pmr container in this TU is constructed.

namespace deren::vulkan {

    // ---- THE BUFFER-CREATION HELPERS THE CONVERTED SITES SHARE (see their notes in :declarations) ----
    //
    // THE INITIAL BYTES ARE THE UPLOAD, and the contract's `initial_bytes` is where they travel: a
    // host-visible buffer the caller then writes through `mapped()` keeps an EMPTY span, which is the
    // descriptor's own "allocate only" (the backend keys its content registry on those bytes, so handing it
    // a span it never wrote would be a lie about the content). Only a buffer the caller already has the
    // bytes for - the vertex and index geometry - passes a non-empty one.

    // ... AND A SIZE SMALLER THAN THE UPLOAD IS NOT A BUFFER AT ALL, so it is a PANIC rather than a
    // truncation: every call site here passes either the upload's own size or a buffer it fills later
    // through `mapped()`, and a backend handed the first without the bytes would allocate a buffer nobody
    // described. The panic is loud on purpose - the alternative, a silently empty allocation, is the class
    // of failure this migration is removing.
    [[nodiscard]] rhi::buffer_desc make_buffer_desc(std::uint64_t const size, rhi::buffer_usage const usage, rhi::buffer_flags const flags, std::span<std::byte const> const initial_bytes) noexcept {
        if (initial_bytes.size() > size) {
            deren::utility::panic(std::source_location::current(), "buffer descriptor: {} initial bytes for a {} B buffer", initial_bytes.size(), size);
        }
        return rhi::buffer_desc{
            .size = size,
            .usage = usage,
            .flags = flags,
            .initial_bytes = initial_bytes,
        };
    }

    void create_buffer(core& device, rhi::buffer_usage const usage, rhi::buffer_flags const flags, std::span<std::byte const> const initial_bytes, std::string_view const what, rhi::object_manager<rhi::buffer>& output, void** const mapped) {
        // THE FACTORY IS CALLED THROUGH THE CONTRACT INTERFACE, not through `core`: a call on the concrete
        // class emits an undefined symbol in this half that the backend defines (it JOINS the boundary
        // worklist), while a virtual call through `rhi::api_core&` loads the vptr and emits none. `core`
        // derives from `api_core`, so the cast is the upcast the interface was built for.
        //
        // THE OUTPUT IS THE OWNER, not the handle: an `object_manager` is move-only and null-on-move, so the
        // reference the factory hands over is taken by the manager and released by ITS destructor - on every
        // path out, which is the whole reason the type exists.
        output = rhi::object_manager<rhi::buffer>{static_cast<rhi::api_core&>(device).create_buffer(make_buffer_desc(initial_bytes.size(), usage, flags, initial_bytes))};
        if (!output) {
            deren::utility::panic(std::source_location::current(), "failed to create {}", what);
        }
        if (mapped != nullptr) {
            *mapped = output->mapped().data();
        }
    }

    void create_buffers(core& device, std::vector<rhi::object_manager<rhi::buffer>>& outputs, rhi::buffer_usage const usage, rhi::buffer_flags const flags, std::span<std::byte const> const initial_bytes, std::string_view const what, std::vector<void*>* const mapped) {
        for (int32_t slot = 0; slot < core::MAX_FRAMES_IN_FLIGHT; ++slot) {
            outputs.emplace_back();
            void* slot_mapped = nullptr;
            create_buffer(device, usage, flags, initial_bytes, what, outputs.back(), mapped != nullptr ? &slot_mapped : nullptr);
            if (mapped != nullptr) {
                mapped->push_back(slot_mapped);
            }
        }
    }

    VkDeviceAddress buffer_address(core& device, rhi::buffer const& buffer) noexcept {
        // THROUGH THE ABILITY POINTER, never through a concrete member: `query_extension` is a virtual on
        // `rhi::api_core`, and `buffer_address` is called on the `device_address` interface it answers with -
        // a call on the backend's own `buffer_address_view` member would emit a symbol into this half.
        auto* const addresses = static_cast<rhi::device_address*>(device.query_extension(rhi::extension_kind::device_address));
        // The ability answers 0 itself for a buffer that was not created with `buffer_flag::device_address`,
        // so a null ability is the only additional case here - and it is the contract's "this backend cannot
        // serve addresses", which every call site rules out by creating the buffer WITH that flag.
        return addresses == nullptr ? 0u : addresses->buffer_address(buffer, 0u);
    }

    VkBuffer runtime::buffer_of(rhi::buffer const& buffer) noexcept {
        // `query_extension` is a NON-const virtual on `api_core` (the abilities it answers with are the
        // backend's own objects), so this reads the core through a non-const reference - the member is one.
        core& device = this->vulkan_core;
        auto* const escape = static_cast<rhi::vulkan_escape*>(device.query_extension(rhi::extension_kind::vulkan_escape));
        return escape == nullptr ? VK_NULL_HANDLE : reinterpret_cast<VkBuffer>(escape->native_buffer(buffer));
    }

    VkDeviceAddress runtime::buffer_address(rhi::buffer const& buffer) const noexcept {
        return deren::vulkan::buffer_address(this->vulkan_core, buffer);
    }

    bool runtime::write_heap_buffer(rhi::buffer const& buffer, uint32_t const slot, VkDeviceSize const size, VkDescriptorType const type) const {
        return this->vulkan_core.descriptor_heaps.write_buffer(core::heap_slot_offset(slot), this->buffer_address(buffer), size, type);
    }

    frame_status runtime::pace_and_acquire() {
        deren::vulkan::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::vulkan::profiling::cpu_phase::pace};
        core& vk = this->vulkan_core;

        // A zero-sized swapchain (a window that has not been sized yet, or was restored from
        // minimized into a 0-sized client area) has no valid attachments: recording would set a
        // 0-wide viewport - a VUID - and produce nothing. Skip the frame exactly like the
        // minimized case; nothing was acquired, so no semaphore is left pending.
        //
        // THE SWAPCHAIN'S OWN EXTENT, not the render extent, and the two are not interchangeable here:
        // this is the question "is there an image to record into at all", which the OUTPUT answers. A
        // render extent that rounded up to at least 1x1 (`render_extent` clamps there) would report a
        // usable frame for a 0-sized window, which is the image the acquire just failed on.
        if (vk.swap_chain_extent.width == 0 || vk.swap_chain_extent.height == 0) {
            return frame_status::skipped;
        }

        // Frame limit (set_max_fps): hold the frame until its deadline before touching the swapchain, so
        // the wait can never happen with an image acquired. yield() and not a timer: on a fast scene the
        // remaining slack is a few milliseconds at most, and an OS sleep at Windows' default 15.6 ms
        // granularity overshoots by more than the target period (a 144 Hz target would sag towards 60).
        // The deadline moves by exactly one period, so overrunning a frame resyncs to now instead of
        // accumulating debt that would be paid back as a burst.
        if (this->max_fps > 0.0) {
            auto const period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / this->max_fps));
            auto const now = std::chrono::steady_clock::now();
            this->next_frame_deadline += period;
            if (this->next_frame_deadline > now) {
                // Sleep to a millisecond before the deadline, then spin that last millisecond: sleeping
                // alone cannot hit a sub-millisecond target, and spinning alone reaches it but burns a
                // whole core doing nothing. Splitting it costs about a millisecond of spin per frame
                // (6% of a core at 60 fps) and still lands on the deadline to within microseconds.
                deren::utility::sleep_for_nanoseconds(std::chrono::duration_cast<std::chrono::nanoseconds>(this->next_frame_deadline - std::chrono::milliseconds(1) - std::chrono::steady_clock::now()).count()); // portable: Win32 waitable timer / POSIX nanosleep
                while (std::chrono::steady_clock::now() < this->next_frame_deadline) {
                    std::this_thread::yield();
                }
            } else {
                this->next_frame_deadline = now; // fell behind: resync, do not bank debt
            }
        }

        // Pace the frame slot: wait until the previous submission on this slot has completed
        //    (host-side timeline wait on the slot's last signaled value). This guards both the
        //    command buffer and the acquire semaphore — acquiring first could reuse a binary
        //    acquire semaphore with pending operations (VUID-vkAcquireNextImageKHR-semaphore-01779)
        uint32_t const frame_slot = static_cast<uint32_t>(vk.current_frame);
        vk.wait_frame_slot(frame_slot);
        // The slot's previous submission is complete, so its timestamps are readable: collect them
        // here, where the wait already guarantees it, and before the slot is recorded again.
        this->collect_gpu_timings(frame_slot);
        // ONE LINE, ONCE, SAYING HOW THE MESHLET DISPATCHES REACHED THE GPU (docs/mesh_shaders.md step 3): a frame
        // has been recorded by now, so the two counters answer the question the frame itself cannot - "is the
        // indirect seam the one in use, or did something fall back to the direct call". That is the exact hole a
        // whole round fell through once (entry point resolved, buffer never bound), so it is printed rather than
        // inferred. Both zero means no meshlet dispatch has been recorded at all.
        {
            static bool logged_mesh_indirect = false;
            uint64_t const indirect_dispatches = this->mesh_indirect_dispatches.load(std::memory_order_relaxed);
            uint64_t const direct_fallbacks = this->mesh_indirect_direct_fallbacks.load(std::memory_order_relaxed);
            if (!logged_mesh_indirect && (indirect_dispatches != 0u || direct_fallbacks != 0u)) {
                logged_mesh_indirect = true;
                deren::utility::log("mesh indirect: {} meshlet dispatch(es) went through the INDIRECT entry point, {} through the direct call", indirect_dispatches, direct_fallbacks);
            }
        }

        // Acquire the next swapchain image; on out-of-date (e.g. the window was resized)
        //    rebuild the swapchain and let the caller retry on the next iteration.
        // THE ACQUIRE ITSELF IS THE CORE'S (receiver of this step): the device and the swapchain are
        //    its objects, and the contract's frame_begin() goes through the same primitive. What stays
        //    here is the POLICY for the result, because rebuilding a swapchain means recompiling the
        //    renderer's targets, which is this object's business and not the backend's.
        VkResult const acquire_result = vk.acquire_next_image(this->current_image_index);
        if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
            deren::utility::log("swapchain out of date, recreating");
            if (vk.recreate_swap_chain()) {
                this->on_swapchain_recreated();
            }
            return frame_status::skipped;
        }
        if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
            return frame_status::acquire_failed;
        }

        // Write this frame's camera UBO into the paced slot's per-slot buffer. The heap's
        //    per-slot camera slot points at that slot's own buffer, so one memcpy is the whole
        //    camera update - there is no per-frame descriptor write to make.
        // The DISPLAY aspect, from the swapchain's extent: the projection is not a per-pixel quantity, so
        // the render scale cancels out of it - but the output's own extent is the honest source (a scale
        // that rounds the two axes differently would otherwise tilt the projection by a fraction of a
        // pixel's worth of aspect).
        this->current_aspect = static_cast<float>(vk.swap_chain_extent.width) / static_cast<float>(vk.swap_chain_extent.height);
        this->current_ubo = make_orbit_camera_ubo(this->camera.yaw, this->camera.pitch, this->camera.distance, this->camera.target, this->scene_extent_radius, this->current_aspect);
        // Motion-vector support: the G-buffer computes its vectors from the UNJITTERED pair, and the
        // previous matrix is the one THIS swapchain image's history was rendered with (not simply the
        // last frame's: several images are in rotation, so the last frame's camera is not what that
        // history shows).
        this->current_ubo.view_proj_unjittered = this->current_ubo.proj * this->current_ubo.view;
        this->current_proj_unjittered = this->current_ubo.proj; // before the jitter below
        this->current_ubo.prev_view_proj = this->current_image_index < this->image_view_proj.size() ? this->image_view_proj[this->current_image_index] : this->current_ubo.view_proj_unjittered;
        // TAA jitter: offset the projection by a sub-pixel amount so consecutive frames sample the
        // image at different positions. The offset lands in the projection's z-row (the only place a
        // perspective matrix carries an NDC translation), in NDC units derived from pixels, and it is
        // part of `proj` - so geometry AND the lighting stage's depth reconstruction agree about where
        // each sample is. Both unjittered matrices above are already taken, so the jitter cannot leak
        // into a motion vector.
        //
        // ONE PIXEL HERE IS A RENDER PIXEL, which is why this divides by the render extent and not by the
        // swapchain's: the halton sequence is a sub-pixel pattern in the raster the samples land in, so at
        // half scale the same NDC offset would cover two output pixels and the resolve would be handed a
        // jitter twice the size of the pixel it is cancelling.
        if (this->taa_active()) {
            glm::vec2 const jitter_pixels = taa_jitter_offset(this->taa_jitter_index);
            this->current_ubo.proj[2][0] += jitter_pixels.x * 2.0f / static_cast<float>(vk.render_extent().width);
            this->current_ubo.proj[2][1] += jitter_pixels.y * 2.0f / static_cast<float>(vk.render_extent().height);
            this->taa_jitter_index = (this->taa_jitter_index + 1) % taa_jitter_count;
        }
        // The deferred lighting stage reconstructs world positions from the G-buffer depth, so its
        // inverse must be the projection actually used to render that depth (jitter included).
        this->current_inv_view_proj = glm::inverse(this->current_ubo.proj * this->current_ubo.view);
        // ... and only now is the frame's camera UBO complete: upload it.
        if (this->camera_mapped[frame_slot] != nullptr) {
            std::memcpy(this->camera_mapped[frame_slot], &this->current_ubo, sizeof(camera_ubo));
        }
        // Same for the light UBO: copy the CPU-side light_state into THIS slot's own light
        // buffer. The slot was just paced (its previous submission completed) and the other
        // in-flight slot's set points at its own buffer, so this host write can never race a GPU
        // read - which is why set_shadow_enabled() / enable_shadows() only touch light_state and
        // never mapped memory directly.
        if (this->light_mapped.size() > static_cast<std::size_t>(frame_slot) && this->light_mapped[frame_slot] != nullptr) {
            // the light_count lane's y carries the exposure scale (pbr.frag / skybox.frag apply it
            // in linear space right before the tonemapper)
            // shadows follow the camera: refit the light frustum to this frame's camera before
            // the UBO upload (16 points + a few matrix multiplies per frame)
            this->update_shadow_frustum();
            this->light_state.light_count.y = this->exposure_scale;
            this->light_state.light_count.z = this->toon_steps;
            this->light_state.light_count.w = this->toon_softness;
            // Ray-traced sun shadows: composed HERE rather than in set_rt_shadows, because the light UBO
            // is rebuilt from light_state every frame and a later enable_shadows() (main.cpp calls it
            // after the settings are applied, which is where this flag was first lost) resets fields of
            // this struct. Recomposing it makes the flag authoritative - the lighting stage reads exactly
            // what the passes below will do this frame.
            this->light_state.rt_shadows = (this->rt_shadows && this->pass_ready("rt_shadow") && this->vulkan_core.ray_query_available) ? 1.0f : 0.0f;
            // the furnace mode still wins: it is a verification mode that turns the sun off entirely
            this->light_state.sun_intensity = (this->furnace ? 0.0f : 1.0f) * this->sun_intensity;
            this->light_state.furnace_level = this->furnace ? 1.0f : 0.0f;

            // ---- clustered light culling (M5): this frame's grid + the view-depth range its
            //      exponential slices span, and the host-side clear of the per-cluster counters.
            //      The grid is derived from the swapchain extent (capped to the allocated tiles) and
            //      the depth range from the same scene bound the shadow fit uses, so every visible
            //      fragment lands in a real cluster. A degenerate projection (the first frames, before
            //      the swapchain has an extent) disables the pass for that frame: shade_surface() then
            //      loop every light, which is always correct - just slower.
            {
                this->cluster_tiles_x = std::min((vk.render_extent().width + deren::vulkan::cluster_tile_size - 1) / deren::vulkan::cluster_tile_size, deren::vulkan::max_cluster_tiles_x);
                this->cluster_tiles_y = std::min((vk.render_extent().height + deren::vulkan::cluster_tile_size - 1) / deren::vulkan::cluster_tile_size, deren::vulkan::max_cluster_tiles_y);
                glm::mat4 const base_proj = this->current_proj_unjittered;
                bool const degenerate = !(this->current_aspect > 0.0f) || std::abs(base_proj[2][2]) < 1e-6f;
                float cluster_near = 0.1f;
                float cluster_far = 1.0f;
                if (!degenerate) {
                    float const camera_near = base_proj[3][2] / base_proj[2][2];
                    float const camera_far = base_proj[2][2] * camera_near / (1.0f + base_proj[2][2]);
                    float const scene_far = glm::distance(glm::vec3(this->current_ubo.camera_pos), this->shadow_scene_center) + this->scene_extent_radius;
                    cluster_near = std::max(camera_near, 0.05f);
                    cluster_far = std::max(std::min(camera_far, scene_far), cluster_near * 2.0f);
                }
                bool const clustered = this->clustered_lights && this->pass_ready("cluster") && !degenerate && this->cluster_tiles_x > 0 && this->cluster_tiles_y > 0;
                this->light_state.cluster_grid = glm::vec4(static_cast<float>(this->cluster_tiles_x),
                                                           static_cast<float>(this->cluster_tiles_y),
                                                           static_cast<float>(deren::vulkan::cluster_slice_count),
                                                           clustered ? 1.0f : 0.0f);
                this->light_state.cluster_depth = glm::vec4(cluster_near,
                                                            cluster_far,
                                                            static_cast<float>(vk.render_extent().width),
                                                            static_cast<float>(vk.render_extent().height));
                // the pass only APPENDS, so the counts are cleared here - the buffer is host-coherent
                // (no flush) and this slot was just paced, so its previous GPU reads are done
                // Gated on the SAME feature flag the dispatch uses: without an active punctual light,
                // or in the flat render mode, nothing will ever read the counts (49 KB per frame).
                bool const cluster_counts_used = this->active_features().clustered;
                if (cluster_counts_used && this->cluster_count_mapped.size() > static_cast<std::size_t>(frame_slot) && this->cluster_count_mapped[frame_slot] != nullptr) {
                    std::memset(this->cluster_count_mapped[frame_slot], 0, static_cast<std::size_t>(deren::vulkan::max_cluster_count) * sizeof(uint32_t));
                }
            }
            std::memcpy(this->light_mapped[frame_slot], &this->light_state, sizeof(light_ubo));
        }
        // THE HEAD FRAME RIDES THE SAME SLOT RULE, and for the same reason: it can change every frame on a model
        // whose head turns, so the paced slot gets its own copy and a frame in flight keeps reading the one it
        // started with. `head_state` is only ever written by set_head_basis, which is an arbitrary-time call -
        // this is where that value reaches the GPU, exactly as the light's does.
        if (this->head_mapped.size() > static_cast<std::size_t>(frame_slot) && this->head_mapped[frame_slot] != nullptr) {
            std::memcpy(this->head_mapped[frame_slot], &this->head_state, sizeof(head_ubo));
        }
        // Remember the paced slot: the caller's per-frame host writes (set_skin_matrices /
        // morph_scratch) land in this slot's buffers and are safe to make now that the slot's
        // previous submission has completed.
        this->active_slot = frame_slot;
        // ... and the frame's shared constants are THIS frame's from here on: the camera record, the light
        // UBO and the fitted scene bounds above are all final at this point, and every pass of the frame reads
        // the same numbers the shaders see (see deren.vulkan.frame_constants).
        this->update_frame_constants();
        return frame_status::proceed;
    }

    void runtime::advance_motion_transforms() {
        if (this->bound_scene == nullptr || this->motion_mapped.empty()) {
            return;
        }
        uint32_t const slot = static_cast<uint32_t>(this->vulkan_core.current_frame);
        if (slot >= this->motion_mapped.size()) {
            return; // no buffer for this slot: nothing to publish, and the shader reads slot 0's set
        }
        auto* const published = static_cast<glm::mat4*>(this->motion_mapped[slot]);
        if (published == nullptr) {
            return;
        }
        for (scene_tree::scene_node const& root : this->bound_scene->roots) {
            scene_tree::visit_primitives(root, this->scene_transform, [this, published](scene_tree::scene_node const& node, glm::mat4 const& world) {
                uint32_t const index = node.primitive_leaf->motion_slot();
                if (index == scene_tree::no_motion_slot || index >= deren::vulkan::scene_motion_capacity) {
                    return; // instanced (filled at setup) or out of range: nothing to advance
                }
                // Publish what this leaf looked like one frame ago, THEN remember this frame's world
                // matrix - so the next frame publishes the matrix this frame is about to draw with.
                published[index] = this->motion_previous[index];
                this->motion_previous[index] = world;
            });
        }
    }

    void runtime::advance_motion_deformations() {
        if (this->skin_previous_mapped.empty() || this->skin_mapped.empty()) {
            return;
        }
        uint32_t const slot = static_cast<uint32_t>(this->vulkan_core.current_frame);
        if (slot >= this->skin_previous_mapped.size() || slot >= this->skin_mapped.size()) {
            return; // no buffer for this slot: nothing to publish, and the shader reads slot 0's set
        }
        auto* const published = static_cast<glm::mat4*>(this->skin_previous_mapped[slot]);
        auto const* const current = static_cast<glm::mat4 const*>(this->skin_mapped[slot]);
        if (published == nullptr || current == nullptr || this->skin_previous.empty()) {
            return;
        }
        // Publish what the PREVIOUS frame drew with, THEN remember this frame's - the same read-then-refresh
        // advance_motion_transforms() does per leaf, here over the whole joint block in one memcpy each
        // because the controller has already uploaded this frame's block through set_skin_matrices() (see the
        // declaration for why the animation controller is deliberately not the writer).
        size_t const bytes = this->skin_previous.size() * sizeof(glm::mat4);
        std::memcpy(published, this->skin_previous.data(), bytes);
        std::memcpy(this->skin_previous.data(), current, bytes);
    }

    frame_status runtime::begin_recording() {
        deren::vulkan::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::vulkan::profiling::cpu_phase::begin};
        core& vk = this->vulkan_core;
        if (this->bound_scene == nullptr) {
            deren::utility::panic("runtime::begin_recording() called before set_scene() bound a scene");
        }
        // THIS FRAME's resources, in the declaration's vocabulary: published before anything resolves, because
        // the per-swapchain-image views are this generation's, an alias (`scene_color`) is decided per frame, and
        // the lazily created shadow map exists only after a scene set does - so this is the first point where
        // "what exists right now" has an answer (see pass::resource_table and runtime::publish_frame_resources).
        this->publish_frame_resources();
        // Record the frame into this slot's command buffer (inline recording: no inheritance)
        vk_command_buffer& command_buffer = this->command_buffers[static_cast<uint32_t>(vk.current_frame)];
        VkCommandBufferBeginInfo const begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = 0,
            .pInheritanceInfo = nullptr,
        };
        if (vkBeginCommandBuffer(*command_buffer, &begin_info) != VK_SUCCESS) {
            return frame_status::begin_recording_failed;
        }
        // THE HEAP IS NOT BOUND HERE, AND THE MEASUREMENT IS WHY: binding it takes the WHOLE command buffer, not
        // only the stages that map from it. With every heap mapping switched OFF - so nothing but these two calls
        // was heap state - all nine gate scenarios came back as the SAME frame (hash DC5F6D66428C26D8, mean 0.00
        // against the 88.1 of the unlit reference), validation SILENT: a set-based stage under a bound heap does
        // not read the set it was handed, it reads the heap, where its bindings were never mapped. So the heap
        // turns on for a frame whose EVERY stage is heap-based, and not one stage earlier - a pass cannot be
        // migrated on its own. EVERY STAGE IS HEAP-BASED NOW, which is why the bind below is unconditional and why
        // the mapping shim it was waiting for is gone rather than switched on.
        // THE HEAPS, BOUND ONCE FOR THE WHOLE FRAME (see descriptor_heap::record_bind): every stage is heap-native
        // now, so this is command-buffer state taken at the earliest point the frame has commands. THERE IS NO
        // PUSH HERE, and its absence is the design: the frame slot and the swapchain image travel INSIDE each
        // stage's own push block (see shaders/heap_slots.glsl), which is what replaced the mapping shim's pushed
        // index - the binding is per frame, the indices are per stage.
        if (vk.descriptor_heaps.ready()) {
            vk.descriptor_heaps.record_bind(*command_buffer);
        }
        // GPU pass timing: open this frame's timestamp range and take the first mark. Marks are
        // written in gpu_mark_id order from here on (see gpu_mark); opening the range outside any
        // rendering instance is required, and this is the first point of the frame where the
        // command buffer exists.
        vk.begin_gpu_timing(*command_buffer, static_cast<uint32_t>(vk.current_frame));
        this->gpu_mark(*command_buffer, gpu_mark_id::frame_begin, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

        // The constant 1x1x6 environment cube, written here and ONCE per target generation: this is the
        // frame's first command buffer, and the cube is sampled by the SKYBOX - which runs long before
        // the lighting stage - so any later point would leave the background of every
        // frame reading the previous contents. Two features share it, which is why the clear is NOT
        // gated on `furnace` any more: the furnace verification mode points the IBL bindings at it as
        // the constant environment (level 1.0, the same value the light UBO's furnace lane carries, so
        // the analytic answer and the environment agree by construction), and the unloaded-IBL path
        // uses it as the type-correct CUBE placeholder (see write_ibl_bindings) - a descriptor pointing
        // at an image nothing ever initialized is worse than one pointing at a neutral value.
        if (!this->furnace_cube_ready && !vk.furnace_cube_images.empty() && vk.furnace_cube_images[0] != VK_NULL_HANDLE) {
            VkImageMemoryBarrier2 to_transfer = deren::vulkan::undefined_to_transfer_dst_transition;
            to_transfer.image = vk.furnace_cube_images[0];
            to_transfer.subresourceRange.layerCount = 6; // all six faces, not the one the constant defaults to
            VkDependencyInfo const to_transfer_dependency = make_image_dependency_info(1, &to_transfer);
            vkCmdPipelineBarrier2(*command_buffer, &to_transfer_dependency);

            VkClearColorValue const level = {{1.0f, 1.0f, 1.0f, 1.0f}};
            VkImageSubresourceRange const faces = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
            vkCmdClearColorImage(*command_buffer, vk.furnace_cube_images[0], VK_IMAGE_LAYOUT_GENERAL, &level, 1, &faces);

            VkImageMemoryBarrier2 to_sampling = deren::vulkan::transfer_dst_to_sampling_transition;
            to_sampling.image = vk.furnace_cube_images[0];
            to_sampling.subresourceRange.layerCount = 6;
            VkDependencyInfo const to_sampling_dependency = make_image_dependency_info(1, &to_sampling);
            vkCmdPipelineBarrier2(*command_buffer, &to_sampling_dependency);

            this->furnace_cube_ready = true;
        }
        // Debug overlay: begin a fresh ImGui frame once per rendered frame (after the acquire,
        // before any UI content is built; the actual draw is recorded at the end of
        // record_main_drawcalls() while the main rendering instance is still open).
        if (this->debug_gui_shown && this->debug_overlay.is_active()) {
            this->debug_overlay.new_frame();
        }

        // Accumulate scene-tree world transforms: every leaf's push.model = scene_transform *
        //     identity * local. With the default scene_transform (identity) this reproduces the
        //     old flat-list world matrices exactly; the scene transform adds programmatic
        //     whole-scene grouping on top.
        for (scene_tree::scene_node& root : this->bound_scene->roots) {
            scene_tree::update_world(root, this->scene_transform);
        }
        // ... and, in the same walk's shadow, the previous-frame world matrices that give TAA its
        // object motion: see the declaration for why this has to happen here and not at draw time.
        this->advance_motion_transforms();
        // ... and the joint matrices ONE FRAME AGO, which are the other half of a motion vector for a
        // DEFORMING mesh: the rigid walk above cannot see that a skinned vertex moved inside its own object
        // space, because that movement is not in any node's matrix. Runs here, immediately after the rigid
        // publication and therefore after the animation controller's own write window.
        this->advance_motion_deformations();
        // Collect the primitive leaves once (DFS over the whole scene): the shadow pass draws all
        // of them, the main pass draws the subset bound to each pipeline
        this->frame_leaves_buffer.clear();
        for (scene_tree::scene_node const& root : this->bound_scene->roots) {
            collect_leaf_primitives(root, this->frame_leaves_buffer);
        }
        // ---- AND THE OVERLAY LEAVES COME STRAIGHT BACK OUT OF THAT SET ----
        //
        // This is the ONE place the article's two framebuffer multiplies are separated from every ordinary
        // surface, and it is here rather than at draw time because of what the separation MEANS: an overlay
        // surface must be absent from the opaque list, the transparent list AND the shadow caster list, not
        // merely ignored by them. A mask left in the opaque set is shaded as a surface (it is drawn with
        // whatever pipeline the pass binds and occludes what is behind it), and one left in the caster set
        // throws a shadow of its own onto the face it exists to darken. Both are the same mistake - a mask
        // treated as geometry.
        //
        // HOW MUCH THAT COSTS, MEASURED WITH A CONTROL rather than quoted: `chars\chen_full2_control.glb` is
        // the same asset with the two overlay materials renamed so neither classifier claims them
        // (`make_overlay_control.py`), i.e. exactly the frame this port replaces. Against the port's own frame
        // it differs by 591 px / 0.06% at the standard pose, 846 / 0.08% at 35 degrees of yaw and 8444 / 0.81%
        // at the 0.45 close-up - small, because these two quads are shaped to the features they darken and
        // almost all of each one lands on the face, and the largest part of what differs is where a quad
        // protrudes past the silhouette onto the background. The magnitude is therefore NOT the argument for
        // this split; the three states above are, and the magnitude is what says the split is not a rewrite of
        // the frame either.
        //
        // THE ORDER INSIDE EACH GROUP IS PRESERVED, which is why this is a stable removal and not a sort: the
        // leaves' order is the scene tree's, and two overlay quads that overlap must keep composing the way the
        // asset authored them.
        this->frame_overlay.clear();
        for (primitive const* const leaf : this->frame_leaves_buffer) {
            if (leaf->overlay_kind != 0u) {
                this->frame_overlay.push_back(leaf);
            }
        }
        std::erase_if(this->frame_leaves_buffer, [](primitive const* const leaf) { return leaf->overlay_kind != 0u; });

        // Frustum culling for the main pass: build a BVH over every leaf that has a single
        //     world AABB (normal draw primitives, whose bounds follow push.model), then keep only
        //     the leaves inside the camera frustum. Instanced primitives spread over many
        //     transforms (no single AABB) and primitives without bounds are never culled. The
        //     shadow pass below still draws the full frame_leaves_buffer set so no caster is lost.
        //
        //     Two-level reuse: the BVH is rebuilt only when the scene changed (bvh_dirty), and
        //     when the camera also did not move the culled result is reused as-is (no rebuild, no
        //     frustum_cull). update_world() above rewrites the same world matrices each frame, so
        //     a non-dirty scene keeps identical world AABBs and the cached BVH stays valid.
        // local aliases into the per-frame state filled above (keeps the cull math unchanged)
        std::pmr::vector<primitive const*> const& frame_leaves = this->frame_leaves_buffer;
        float const aspect = this->current_aspect;
        camera_ubo const& ubo = this->current_ubo;
        std::pmr::vector<primitive const*> visible_leaves = this->frame_leaves_buffer; // fallback: no culling
        // THE CASTER SET, MINUS THE FRAME'S STATIC SURROUND (`primitive::environment`, i.e. the
        // `[render] background_glb` import - see `shadow_casters` in the class docs for why it must never
        // cast). Written once here and used at every place the list is filled, because the caster set is
        // built in four different branches below and a filter applied at three of them is a bug waiting for
        // the fourth.
        auto const fill_shadow_casters = [this](std::pmr::vector<primitive const*> const& source) {
            this->shadow_casters.clear();
            this->shadow_casters.reserve(source.size());
            for (primitive const* const leaf : source) {
                if (!leaf->environment) {
                    this->shadow_casters.push_back(leaf);
                }
            }
        };
        if (this->frustum_culling) {
            // camera identity: the orbit state that shapes the frustum
            bool const scene_changed_this_frame = this->bvh_dirty;
            std::array<float, 7> const key = {
                this->camera.yaw,
                this->camera.pitch,
                this->camera.distance,
                this->camera.target.x,
                this->camera.target.y,
                this->camera.target.z,
                aspect,
            };
            this->camera_moved = key != this->camera_key;

            if (scene_changed_this_frame || !this->cull_bvh.has_value()) {
                // scene changed: rebuild the BVH from current world AABBs (and drop stale leaves)
                std::vector<deren::utility::aabb_box<primitive>> boxes;
                boxes.reserve(frame_leaves.size());
                for (primitive const* leaf : frame_leaves) {
                    if (leaf->has_bounds) {
                        auto const [wmin, wmax] = leaf->world_aabb();
                        boxes.push_back(deren::utility::aabb_box<primitive>{.min = wmin, .max = wmax, .extra_data = const_cast<primitive*>(leaf)});
                    }
                }
                if (!boxes.empty()) {
                    this->cull_bvh.reset(); // destroy the old tree first (its leaves reference this scene)
                    auto make_result = deren::utility::bvh<primitive>::make(boxes);
                    if (make_result) {
                        this->cull_bvh = std::move(make_result).value();
                    }
                } else {
                    this->cull_bvh = std::nullopt;
                }
                this->bvh_dirty = false;
            }

            if (this->camera_moved || scene_changed_this_frame || this->cull_visible.empty()) {
                // camera moved or the scene changed: re-run frustum cull against the current BVH
                std::pmr::vector<primitive const*> visible;
                visible.reserve(frame_leaves.size());
                // leaves without bounds (instanced etc.) are always drawn
                for (primitive const* leaf : frame_leaves) {
                    if (!leaf->has_bounds) {
                        visible.push_back(leaf);
                    }
                }
                if (this->cull_bvh.has_value()) {
                    // the cull frustum is built from the UNJITTERED view-projection: a jittered one
                    // would re-cull (and occasionally flicker a leaf in or out) on every TAA frame
                    deren::utility::frustum const view_frustum = deren::utility::make_frustum(ubo.view_proj_unjittered);
                    auto const inside = this->cull_bvh->frustum_cull(view_frustum);
                    for (auto const* node : inside) {
                        visible.push_back(node->extra_data);
                    }
                }
                this->cull_visible = std::move(visible);
                this->camera_key = key;

                // ---- Shadow caster set (see shadow_casters in the class docs) ----
                // Built only when the camera moved or the scene changed (reusing the cached
                // cull_visible otherwise, like the main pass does). Two regimes split on scene
                // size:
                //  - SMALL scenes: every leaf goes into the shadow map. Caster culling is only an
                //    approximation (a caster can sit arbitrarily far up-light and its PARALLEL
                //    shadow column still lands in the view) and with few leaves the full depth
                //    render is cheap - take the exact path.
                //  - HEAVY scenes (tens of thousands of leaves): the camera-visible set plus the
                //    BVH culled against the SHADOW frustum itself. That frustum is refitted every
                //    frame to the camera view AND already merges every leaf whose shadow column can
                //    reach the view (update_shadow_frustum), so it contains the off-screen casters -
                //    like the wall behind the camera - that the previous "camera frustum shifted
                //    up-light by shadow_caster_extent" heuristic dropped, while staying far smaller
                //    than the whole scene.
                constexpr std::size_t full_scene_shadow_leaf_limit = 1500;
                if (frame_leaves.size() <= full_scene_shadow_leaf_limit) {
                    fill_shadow_casters(this->frame_leaves_buffer);
                } else if (!this->shadows_enabled) {
                    // no shadow frustum yet (enable_shadows() not called): the set is unused until
                    // the shadow pass records, so take the exact path instead of culling against a
                    // zeroed light matrix. enable_shadows() runs before the first frame in
                    // practice; if it does not, the camera moving refreshes this set.
                    fill_shadow_casters(this->frame_leaves_buffer);
                } else {
                    if (!this->shadow_heuristic_logged) {
                        this->shadow_heuristic_logged = true;
                        deren::utility::log("shadow caster culling: scene exceeds {} leaves - shadow casters are the camera-visible plus shadow-frustum sets",
                                            full_scene_shadow_leaf_limit);
                    }
                    fill_shadow_casters(this->cull_visible);
                    if (this->cull_bvh.has_value()) {
                        // every cascade's frustum: a caster that only shadows the far range must still
                        // be drawn, so the union over the cascades is the exact caster set (a cull per
                        // cascade is cheap against the BVH)
                        for (uint32_t cascade = 0; cascade < std::clamp(this->shadow_cascades, 1u, deren::vulkan::max_shadow_cascades); ++cascade) {
                            deren::utility::frustum const light_frustum = deren::utility::make_frustum(this->light_state.light_view_proj[cascade]);
                            auto const in_light = this->cull_bvh->frustum_cull(light_frustum);
                            this->shadow_casters.reserve(this->shadow_casters.size() + in_light.size());
                            for (auto const* node : in_light) {
                                primitive const* const leaf = node->extra_data;
                                if (!leaf->environment) {
                                    this->shadow_casters.push_back(leaf);
                                }
                            }
                        }
                        std::ranges::sort(this->shadow_casters);
                        this->shadow_casters.erase(std::ranges::unique(this->shadow_casters).begin(), this->shadow_casters.end());
                    }
                }
            }
            visible_leaves = this->cull_visible;
        } else {
            // culling disabled: the shadow pass draws every scene leaf (see shadow_casters)
            fill_shadow_casters(this->frame_leaves_buffer);
        }

        // Split the visible set into OPAQUE leaves (frame_visible: drawn first, depth write on,
        // in the parallel segments) and TRANSPARENT leaves (frame_transparent: alpha-blended,
        // depth write off, drawn last). Transparent leaves are sorted FAR -> NEAR from the
        // camera so overlapping blends compose back-to-front. The split re-runs every frame
        // (cheap: one pass over the visible set + sort of the usually-few transparent leaves);
        // with a static camera the input and thus the result are identical, so no extra cache.
        this->frame_visible.clear();
        this->frame_transparent.clear();
        this->frame_character.clear();
        {
            glm::vec3 const eye = ubo.camera_pos;
            auto const distance_to = [&eye](primitive const* const m) -> float {
                // squared world-space distance of the leaf to the eye. Use the center of the
                // world AABB (transform of the local bounds by push.model) so large or
                // skinned/instanced-with-world leaves sort by where they actually occupy
                // space, not by an arbitrary origin point.
                glm::vec3 center;
                if (m->has_bounds) {
                    auto const [wmin, wmax] = m->world_aabb();
                    center = (wmin + wmax) * 0.5f;
                } else {
                    // no single world AABB (e.g. instanced primitive: one leaf, many world
                    // transforms, push.model is identity): fall back to the model origin.
                    // The resulting order is a no-op for instanced leaves, which is fine -
                    // instances spread over space have no meaningful per-leaf depth anyway.
                    center = glm::vec3(m->push.model[3]);
                }
                return glm::dot(center - eye, center - eye);
            };
            for (primitive const* const m : visible_leaves) {
                (m->transparent ? this->frame_transparent : this->frame_visible).push_back(m);
                // ... AND THE TOON CHARACTER STAGE'S SUBSET, which is the opaque leaves minus the frame's
                // static surround: the character chain re-shades the same surfaces the scene pass drew, and
                // the background is the one thing in the frame that is NOT a subject (see `frame_character`).
                if (!m->transparent && !m->environment) {
                    this->frame_character.push_back(m);
                }
            }
            std::ranges::sort(this->frame_transparent,
                              [&distance_to](primitive const* const a, primitive const* const b) {
                                  return distance_to(a) > distance_to(b); // far first
                              });
        }
        return frame_status::proceed;
    }

    // Everything that decides whether a slot's shadow maps are still valid, folded into one 64-bit
    // fingerprint: the caster count (the set itself can change - heavy scenes BVH-cull the casters
    // per frame), each caster's world matrix, the uploaded skin matrices (a skinned mesh keeps a
    // constant push.model; its pose lives only in those) and the morph-scratch revision. It is XXH3
    // rather than a byte loop because it runs on every frame, the reused ones included: 0.99 us
    // against 15.5 us for Sponza's 300 casters. The fold is boost::hash_combine's mix - the values
    // are hashed rather than compared, so the mix carries the collision resistance.
    uint64_t runtime::shadow_geometry_signature() const {
        auto const fold = [](uint64_t const accumulator, uint64_t const value) {
            return accumulator ^ (value + 0x9e3779b97f4a7c15ull + (accumulator << 6) + (accumulator >> 2));
        };
        uint64_t signature = static_cast<uint64_t>(this->shadow_casters.size());
        for (primitive const* caster : this->shadow_casters) {
            uint64_t const matrix = deren::utility::xxh3_64bits({reinterpret_cast<uint8_t const*>(&caster->push.model), sizeof(glm::mat4)});
            signature = fold(signature, matrix);
        }
        return fold(fold(signature, this->skin_matrix_hash), this->morph_revision.load(std::memory_order_relaxed));
    }

    void runtime::record_main_drawcalls() {
        deren::vulkan::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::vulkan::profiling::cpu_phase::scene};
        core& vk = this->vulkan_core;
        vk_command_buffer& command_buffer = this->command_buffers[static_cast<uint32_t>(vk.current_frame)];
        uint32_t const frame_slot = static_cast<uint32_t>(vk.current_frame);

        // ---- Clustered light culling (M5): one compute dispatch before anything renders. It sorts
        //      the punctual lights into the frame's cluster grid, and the shading stages (forward
        //      inside the main instance, deferred later in the frame) then loop only their own
        //      cluster's list. Runs before the shadow pass so the barrier that publishes its buffers
        //      is as early as possible; nothing before it reads the lists.
        {
            deren::vulkan::profiling::cpu_phase_timer const cluster_timer{this->cpu_timings, deren::vulkan::profiling::cpu_phase::cluster};
            // THE CLUSTER SORT records its own dispatch and its own two buffer barriers now
            // (deren.vulkan.pass.cluster); what is this loop's is WHERE it runs - before the passes that read the
            // bins - and the frame data it is handed, which the chain's owner supplies (see prepare_stage).
            pass::stage const cluster_stage = {.name = "cluster", .passes = this->cluster_pass, .marks = false};
            this->prepare_stage(cluster_stage, *command_buffer);
            [[maybe_unused]] pass::run_report const cluster_report = pass::record_stage(cluster_stage, this->make_pass_host());
        }

        // ---- Ray-traced shadows: build the acceleration structures once, before the passes that will
        //      trace against them. It happens HERE (inside the frame's command buffer, before any
        //      rendering instance opens) because a build is a transfer/compute-class command that must
        //      not be recorded inside vkCmdBeginRendering, and because the caster set is only complete
        //      now that the scene is loaded and culled. Nothing reads the structures yet, so a frame
        //      with the flag on renders exactly like one with it off - what this records is the input
        //      the ray-traced pass will need, not a change to the image.
        //
        // THE WORK IS THE STRUCTURE SET'S (deren.vulkan.ray_tracing) and the POLICY is this loop's: WHETHER the
        // structures are wanted at all (`rt_structures_wanted`), and that they are built and re-instanced HERE -
        // before any rendering instance opens. What the set returns is why it gave up; what that means for a knob
        // (the refit's own failure turns `rt_skin_bake` off) is decided here.
        if (this->rt_structures_wanted()) {
            ray_tracing::build_inputs const inputs = this->make_structure_inputs();
            // structures_frame_slot, not frame_slot: the outer frame_slot is in scope; MSVC /W4 C4456, an error under /WX.
            uint32_t const structures_frame_slot = static_cast<uint32_t>(this->vulkan_core.current_frame);
            if (auto const built = this->structures.build(*command_buffer, inputs); !built) {
                deren::utility::log("ray-traced shadows disabled: {}", built.error().message);
            }
            // ... and the top level structure, which is rebuilt EVERY frame: the instance set is culled per
            // frame and a caster's world matrix can change (animation, a moved node), so the instance list
            // is frame data like any other. On the frame that builds the bottom levels it runs right after them;
            // from the next frame on it is the structure a shadow ray will traverse.
            if (auto const updated = this->structures.update(*command_buffer, structures_frame_slot, inputs); !updated) {
                deren::utility::log("runtime: {}", updated.error().message);
                if (updated.error().disable_skin_bake) {
                    this->rt_skin_bake = false;
                }
            }
            // THE TOP LEVEL STRUCTURE'S HEAP SLOT follows the slot's structure, which is why this write is here and
            // not in the pass that reads it: a heap range must carry a real size and the structure's device address,
            // and both exist only once the structure for that slot does.
            // ONLY WHEN THE HANDLE CHANGES (see rt_binding_written): the unconditional write invalidated a frame that
            // was still in flight, which the validation layer reports as "VkDescriptorSet ... was destroyed or
            // updated without UPDATE_AFTER_BIND" followed by every later call on that command buffer failing.
            VkAccelerationStructureKHR const tlas = this->structures.handle(structures_frame_slot);
            if (structures_frame_slot < this->rt_binding_written.size() && this->rt_binding_written[structures_frame_slot] != tlas) {
                this->write_rt_structure_binding(tlas, structures_frame_slot);
                this->rt_binding_written[structures_frame_slot] = tlas;
            }
        }
        // GPU timing: the structures' builds end here. Written UNCONDITIONALLY, like every other mark -
        // a frame that skips a pass still writes its mark next to the previous one (0 ms interval), and
        // the report's labels are positional: leaving a gap here relabeled the whole frame ("marks
        // recorded out of order", which the harness caught on all seven scenarios at once).
        this->gpu_mark(*command_buffer, gpu_mark_id::rt_build_end, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);

        // ---- Shadow pass: render the scene's depth from the light into this slot's shadow map.
        //      Drawn before the main pass; the depth-only pipeline shares the flat scene layout
        //      and the primitive draw() path (same vertex buffers / push constants), so the shadow
        //      pass is just "bind the shadow pipeline, then draw the same models".
        //      Depth-only rendering (no color attachment) needs dynamic rendering, which is core
        //      1.3 - the only path the engine supports.
        //
        //      Stage 2 of parallel recording: the shadow content is recorded into this slot's
        //      shadow SECONDARY command buffer first, then executed from the primary inside the
        //      shadow rendering instance (VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT).
        //      The recording is still sequential on the primary thread - rendering is identical
        //      to inline; stage 3 fans the recording out over the task pool.
        // Feature registry (runtime::active_features): the pass runs only when a shading stage will
        // actually read the map - the flat render mode skips it entirely, which is worth ~45% of the
        // frame (see the measured numbers in the struct's documentation).
        render_features const features = this->active_features();
        // Geometry half of the reuse test: one fingerprint of every input the pass reads - see
        // shadow_geometry_signature().
        uint64_t const geometry_signature = this->shadow_geometry_signature();
        // Skip the whole pass when this slot's maps are still valid: unchanged fit (content version)
        // AND unchanged caster geometry (hash). The maps stay in SHADER_READ_ONLY and the descriptor
        // set still points at them, so there is nothing to record - not even the read barriers.
        bool const shadow_reuse = this->shadow_rendered_version[frame_slot] == this->shadow_content_version && this->shadow_rendered_models[frame_slot] == geometry_signature;
        if (features.shadow && !shadow_reuse) {
            deren::vulkan::profiling::cpu_phase_timer const shadow_timer{this->cpu_timings, deren::vulkan::profiling::cpu_phase::shadow}; // the sub-phase of scene that records every cascade
            auto const* shadow_detail = vk.vma.get_image_detail(this->shadow_images[frame_slot].handle());
            if (shadow_detail != nullptr) {
                // Secondary: inherit only the depth attachment (dynamic rendering 1.3). The
                // shadow map is single-sampled; viewMask 0 = no multiview. The rendering
                // inheritance struct hangs off VkCommandBufferInheritanceInfo::pNext (NOT the
                // begin-info pNext), and a secondary buffer must always provide inheritance info.
                // One secondary PER CASCADE: the caster content is identical, but each cascade pushes
                // its own index (a secondary records that itself - state is not inherited from the
                // primary), and a buffer without SIMULTANEOUS_USE may not be executed twice in one
                // primary anyway. The content is only the leaves/pipelines; the per-cascade difference
                // is that single push.
                // ---- THE SHADOW PASS RECORDS IT (deren.vulkan.pass.shadow) ----
                // What stays HERE is what the pass cannot know: the reuse test above (which is why this whole block
                // is inside it), the map's OWN image and its layer count (the hand-back below covers every
                // allocated layer, including the spare ones), and the bookkeeping that makes the next frame's reuse
                // test true. The frame itself - the per-cascade secondaries, the map's edge and the two callbacks -
                // is built HERE and handed to the pass by whoever owns it (see make_shadow_frame/prepare_stage).
                pass::stage const shadow_stage = {.name = "shadow", .passes = this->shadow_pass, .marks = false};
                {
                    this->prepare_stage(shadow_stage, *command_buffer);
                    [[maybe_unused]] pass::run_report const shadow_report = pass::record_stage(shadow_stage, this->make_pass_host());
                }
                // Hand the cascades back to the shading stages as a sampled array texture: the layers just rendered go
                // depth-attachment -> shader-read (the src masks publish the attachment write), and the SPARE layers a
                // shrank cascade count left behind are covered by the same range - the descriptor's array view spans
                // every allocated layer, so "one barrier, whole array" is the invariant. That count is the IMAGE's,
                // which is why this is the host's and not the pass's.
                std::array<VkImageMemoryBarrier2, 1> shadow_read_barrier = {shadow_map_sampling_transition};
                shadow_read_barrier[0].image = shadow_detail->image;
                shadow_read_barrier[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, this->shadow_allocated_layers};
                VkDependencyInfo const shadow_read_dependency = make_image_dependency_info(1, shadow_read_barrier.data());
                vkCmdPipelineBarrier2(*command_buffer, &shadow_read_dependency);
                this->shadow_rendered_version[frame_slot] = this->shadow_content_version;
                this->shadow_rendered_models[frame_slot] = geometry_signature;
            }
        } else if (features.shadow) {
            // Reuse: the maps were rendered into this slot by an earlier frame and nothing that feeds
            // them has changed since, so the sampling layout they are already in is the one the
            // lighting pass needs. Deliberately does nothing.
        } else if (this->pass_ready("shadow") && this->shadow_images.size() > static_cast<std::size_t>(frame_slot)) {
            // The pass does not run this frame (shadows toggled off, or no light setup yet), but the
            // scene set still binds the shadow map to binding 8 - pbr.frag uses it statically and only
            // decides at runtime whether to sample it - and a sampled descriptor must point at an
            // image that is in the layout the descriptor declares. Leaving the map in UNDEFINED made
            // every shadow-off frame a VUID ("expects ... SHADER_READ_ONLY_OPTIMAL ... current layout
            // is UNDEFINED" - the message is quoted from a run made before VK_KHR_unified_image_layouts
            // collapsed every layout to GENERAL, which is what the driver expects now). Contents do not
            // matter (the shader returns "fully lit"), hence UNDEFINED as the old layout.
            auto const* shadow_detail = vk.vma.get_image_detail(this->shadow_images[frame_slot].handle());
            if (shadow_detail != nullptr) {
                VkImageMemoryBarrier2 shadow_read_barrier = deren::vulkan::undefined_to_depth_sampling_transition;
                shadow_read_barrier.image = shadow_detail->image;
                // Every layer the image OWNS, not just layer 0: the constant's range is single-layer
                // and the whole array view the descriptor covers must be sampleable. The count is
                // shadow_allocated_layers - the layer count the image was actually created with - and
                // NOT max_shadow_cascades: the two differ whenever fewer cascades are active than
                // were ever allocated (the default is three), and a subresource range reaching past
                // the image's own layer count is a VUID on every shadow-off frame.
                shadow_read_barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, this->shadow_allocated_layers};
                VkDependencyInfo const shadow_read_dependency = make_image_dependency_info(1, &shadow_read_barrier);
                vkCmdPipelineBarrier2(*command_buffer, &shadow_read_dependency);
            }
        }

        // GPU timing: the shadow pass (and its hand-back barrier) ends here. The pass is optional,
        // so a frame without shadows just writes this mark next to frame_begin and reports ~0 ms.
        this->gpu_mark(*command_buffer, gpu_mark_id::shadow_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // The scene pass: the opaque leaves write the G-buffer, and everything else follows from it
        // in record_scene_tail (lighting, the transparent pass over the shaded image, TAA).
        this->record_scene(*command_buffer);
    }

    // =============================================================================================
    // THE SHADOW PASS's content callback and its scheduler (deren.vulkan.pass.shadow)
    // =============================================================================================
    //
    // THE RESOLVER THAT USED TO BE HERE IS GONE (S3.8): the layers this frame renders were a hand-written
    // `resolve_shadow_pass` because the declaration could name only ONE layer of the map, and the vocabulary that
    // replaced it is a RUN of elements (`render_target::count`) - the declaration now claims every cascade layer
    // the map can hold and the resource table answers how many it HAS this frame, which is the cascade knob's
    // (`ensure_shadow_resources`). What is left here is what the pass genuinely cannot know: which secondaries to
    // record into, and what a caster's draw state is (the scene block, the live depth-bias state, the two-sided
    // policy, the secondary's own begin info) - so that arrives as a callback, and the map's edge with it.
    bool runtime::record_shadow_cascade(void* const owner, VkCommandBuffer const secondary, uint32_t const cascade_index, VkPipeline const pipeline, bool const mesh_stage, bool const meshlets) {
        runtime* const self = static_cast<runtime*>(owner);
        core const& vk = self->vulkan_core;
        // The secondary inherits ONLY the depth attachment (no colour one): dynamic rendering 1.3, single-sampled,
        // viewMask 0. The inheritance struct hangs off VkCommandBufferInheritanceInfo::pNext (NOT the begin info's),
        // and a secondary buffer must always provide inheritance info.
        VkCommandBufferInheritanceRenderingInfo const inheritance = make_inheritance_rendering_info(false, nullptr, vk.depth_attachment_format, VK_SAMPLE_COUNT_1_BIT);
        // An inherited HEAP bind, for the same reason the scene pass's segments carry one (see
        // scene_frame::fill_heap_bind): this secondary is validated on its own, and the shadow shaders read the
        // light matrices and the shadow map straight out of the heaps.
        VkBindHeapInfoEXT resource_bind = {};
        VkBindHeapInfoEXT sampler_bind = {};
        vk.descriptor_heaps.bind_infos(resource_bind, sampler_bind);
        VkCommandBufferInheritanceDescriptorHeapInfoEXT const heap_inheritance = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT,
            .pNext = &inheritance,
            .pSamplerHeapBindInfo = &sampler_bind,
            .pResourceHeapBindInfo = &resource_bind,
        };
        VkCommandBufferInheritanceInfo const inherit = make_inheritance_info(&heap_inheritance);
        VkCommandBufferBeginInfo const begin = make_command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &inherit);
        if (vkBeginCommandBuffer(secondary, &begin) != VK_SUCCESS) {
            deren::utility::log("runtime: shadow secondary command buffer begin failed - cascade {} skipped this frame", cascade_index);
            return false;
        }
        // which cascade these casters are projected into: the vertex stage indexes the light UBO's matrix array with
        // it, and a secondary records that itself (state is not inherited from the primary). The offset is the
        // declaration's own - where the scene's push block ends.
        uint32_t const index = cascade_index;
        // PUSHED AS DATA, not as a push constant: the pipeline has no layout any more (see
        // render_environment::push_block). This is ONE 4-byte slice of the shadow stage's block - the cascade
        // index's own offset - while the rest of the block is the material's, pushed per caster below. A
        // secondary records its own state (nothing is inherited from the primary), which is why it is set here.
        [[maybe_unused]] bool const pushed = vk.descriptor_heaps.push_data(secondary, render_resource::shadow_io.push->offset, std::as_bytes(std::span(&index, 1)));
        self->record_shadow_content(secondary, pipeline, mesh_stage, meshlets);
        vkEndCommandBuffer(secondary);
        return true;
    }

    void runtime::run_shadow_tasks(void* const owner, std::span<std::function<void()>> const tasks) {
        static_cast<runtime*>(owner)->run_tasks(tasks, deren::vulkan::task_priority::recording);
    }
    void runtime::record_scene(VkCommandBuffer const command_buffer) {
        this->record_scene_attachments(command_buffer);
        this->update_pass_geometry();
        // THE SCENE PASS records the surface write: the instance over its six declared targets, the segmented
        // draw of the visible leaves, and closing the instance - all inside one function now (see
        // deren.vulkan.pass.scene for why that is the point of this extraction).
        if (this->gbuffer_pass_active()) {
            pass::stage const scene_stage = {.name = "scene", .passes = this->scene_pass, .marks = false};
            this->prepare_stage(scene_stage, command_buffer);
            [[maybe_unused]] pass::run_report const scene_report = pass::record_stage(scene_stage, this->make_pass_host());
            return;
        }
        // THE DEGENERATE CASE, which is all the old begin_rendering() is still needed for: no surface pipeline,
        // so there is no pass to run. An EMPTY instance is opened and closed anyway, so the frame keeps a
        // matching pair and the scene target ends in a layout the post chain can sample. (The old code also
        // recorded the scene segments here - with a pipeline that does not exist, which validation would have
        // refused; drawing nothing is both shorter and true.)
        this->begin_rendering(command_buffer, this->current_image_index, 0);
        vkCmdEndRendering(command_buffer);
    }

    // Move the scene pass's attachments into their render layouts; see the declaration for why this
    // cannot be left to a render pass.
    void runtime::record_scene_attachments(VkCommandBuffer const command_buffer) {
        core& vk = this->vulkan_core;
        // Dynamic rendering has no automatic attachment transitions (a render pass would do them
        // implicitly): move every attachment into its render layout before vkCmdBeginRendering. The
        // set is the G-buffer mode's three single-sampled surface targets, the motion-vector target
        // and the scene color the emissive goes into, plus the pass's own 1x depth image. The main
        // HDR target is not touched by the opaque G-buffer pass at all (the debug view writes it
        // afterwards), so it must not be transitioned here.
        //
        // room for every pass attachment plus the depth. Sizing this for the old three-target
        // G-buffer was a stack overflow the moment the emissive attachment arrived - validation
        // reported the fifth barrier as garbage.
        std::array<VkImageMemoryBarrier2, deren::vulkan::gbuffer_pass_attachment_count + 1> attachment_barriers = {};
        uint32_t barrier_count = 0;
        auto const add_render_barrier = [&attachment_barriers, &barrier_count](VkImageMemoryBarrier2 const& transition, VkImage const image) {
            VkImageMemoryBarrier2& barrier = attachment_barriers[barrier_count++];
            barrier = transition; // copy the role default, then retarget it
            barrier.image = image;
        };

        {
            for (uint32_t target = 0; target < gbuffer_target_count; ++target) {
                add_render_barrier(color_attachment_transition, vk.gbuffer_images[target][this->current_image_index]);
            }
            add_render_barrier(color_attachment_transition, vk.velocity_images[this->current_image_index]);
            add_render_barrier(depth_attachment_transition, vk.gbuffer_depth_images[this->current_image_index]);
            // The scene-color target enters the pass as an attachment too (the emissive accumulation
            // target) - the HDR image normally, scene_color when the TAA resolve owns the HDR target
            // this frame (see scene_target_image). It is cleared by the instance below, so UNDEFINED as
            // the old layout is correct whatever it held before.
            add_render_barrier(color_attachment_transition, this->scene_target_image(this->current_image_index));
            // this instance writes the G-buffer depth, so it is in the attachment layout from here on:
            // the stage that samples it later calls ensure_gbuffer_depth_sampled() (see the accessor).
            // The flag vector is per swapchain image and sized with the generation; the guard is a
            // no-op outside that range, so a stale size can never set a flag for an image that does
            // not exist (and on_swapchain_recreated re-sizes it on every generation change).
            if (this->current_image_index < this->gbuffer_depth_written.size()) {
                this->gbuffer_depth_written[this->current_image_index] = true;
            }
            // ... and the motion-vector target it just wrote as a color attachment. Two stages sample
            // it (TAA's resolve and, since the GI denoiser, a compute resolve), so the "has this been
            // handed to a sampler yet" question is asked through a flag rather than assumed - see
            // ensure_velocity_sampled.
            if (this->current_image_index < this->velocity_written.size()) {
                this->velocity_written[this->current_image_index] = true;
            }
            // ... and the three stored surface targets, written as color attachments by the same
            // instance: the ray-traced shadow pass samples them before the lighting stage does, so which
            // stage publishes them is a question for a flag too (see ensure_gbuffer_targets_sampled).
            if (this->current_image_index < this->gbuffer_targets_written.size()) {
                this->gbuffer_targets_written[this->current_image_index] = true;
            }
        }

        VkDependencyInfo const dependency_info = make_image_dependency_info(barrier_count, attachment_barriers.data());
        vkCmdPipelineBarrier2(command_buffer, &dependency_info);
    }

    // Resync the cached viewport/scissor of every pipeline that draws this frame.
    void runtime::update_pass_geometry() {
        core& vk = this->vulkan_core;
        // Pipelines cache a fullscreen viewport/scissor at creation; after a resize the swapchain
        // extent changed, so resync them from the current extent before drawing (begin_pipeline
        // applies the stored values). Done here on the primary thread (it mutates the cached
        // pipeline state), before the scene content below is recorded - inline or in secondaries.
        //
        // THE RENDER EXTENT, and this is complete rather than approximate: the pipelines that read these
        // stored values are the ones that declared `resync_viewport = false` (the scene, transparent,
        // character, shadow and cluster stages), and every one of them draws into a target that
        // `create_render_targets` created at the render extent. A pass that DID declare the resync - the
        // deferred lighting, TAA, the post composite, FXAA and anything that resolves to the output - gets
        // its viewport from its own declaration's extent at record time, so a render scale below 1.0 cannot
        // leave a scene pipeline and a fullscreen pipeline disagreeing about how big a pixel is.
        VkViewport const full_viewport = {
            0.0f,
            0.0f,
            static_cast<float>(vk.render_extent().width),
            static_cast<float>(vk.render_extent().height),
            0.0f,
            1.0f,
        };
        VkRect2D const full_scissor = {{0, 0}, vk.render_extent()};
        {
            // unique lock: mutating every cached pipeline's viewport/scissor while parallel
            // recording workers may read them through their environments
            std::unique_lock const lock(this->access_mutex);
            for (auto& pipeline : this->pipelines | std::views::values) {
                pipeline.viewport = full_viewport;
                pipeline.scissor = full_scissor;
            }
            // ... and every MESH form of them, for the same reason one at a time: a pipeline object stores what
            // begin_pipeline() applies.
            for (auto& pipeline : this->mesh_pipelines | std::views::values) {
                pipeline.viewport = full_viewport;
                pipeline.scissor = full_scissor;
            }
            // ... and every MESHLET form of them, which needs it for the same reason and is the form the named
            // sessions actually bind (docs/mesh_shaders.md step 3)
            for (auto& pipeline : this->meshlet_pipelines | std::views::values) {
                pipeline.viewport = full_viewport;
                pipeline.scissor = full_scissor;
            }
        }
        // the post-process pipelines are NOT in the named cache above, and begin_pipeline() always
        // re-emits the stored viewport/scissor: leaving them at the creation-time default made every
        // post pass set a 0-wide viewport (validation: "pViewports[0].width (0.000000) is not
        // greater than zero"). The per-pass viewport is set explicitly right after the bind anyway -
        // this keeps the stored values valid. The shadow pipeline is deliberately excluded: its
        // viewport is the fixed shadow-map size, which the shadow pass's content callback sets (see the pass).
        // ... and the post chain's pipelines are NOT resynced here any more: they belong to the post composite
        // PASS (deren.vulkan.pass.post), which declares `resync_viewport` for the composite and derives each bloom
        // level's extent from its own declaration - so the viewport comes from the pass's declaration instead of
        // from a list this class maintains. The two blocks that used to be here are what that field replaced.
        // ... and FXAA's pipeline is NOT resynced here either: it is the FXAA PASS's now (deren.vulkan.pass.fxaa),
        // which declares `resync_viewport` like the rest of the post chain.
        // ... and the same for the G-buffer pair: gbuffer_pipeline is the opaque pass's default
        // pipeline (begin_pipeline applies the stored viewport) and gbuffer_debug_pipeline is a
        // fullscreen pass. Neither lives in the named cache above.
        if (this->gbuffer_pipeline) {
            this->gbuffer_pipeline->viewport = full_viewport;
            this->gbuffer_pipeline->scissor = full_scissor;
        }
        // ... and the MESH form of the same pipeline carries its OWN cached viewport, because a pipeline object
        // stores what begin_pipeline() applies: leaving this one at its creation-time zero is the zero-width
        // viewport this project has already paid for once (the same reason the line above exists).
        if (this->gbuffer_pipeline_mesh) {
            this->gbuffer_pipeline_mesh->viewport = full_viewport;
            this->gbuffer_pipeline_mesh->scissor = full_scissor;
        }
        // ... and the MESHLET form, for the same reason: a pipeline object stores what begin_pipeline() applies.
        if (this->gbuffer_pipeline_meshlet) {
            this->gbuffer_pipeline_meshlet->viewport = full_viewport;
            this->gbuffer_pipeline_meshlet->scissor = full_scissor;
        }
        // ... and the debug view's pipeline is not resynced here either: it is its PASS's now (vulkan.pass.
        // gbuffer_debug), which declares resync_viewport like every other fullscreen pass in this chain.
        // ... and the deferred lighting stage's is not here either, for a stronger reason than the TAA
        // resolve's below: it is a PASS (deren.vulkan.pass.deferred), it declares `resync_viewport = true`, so the
        // runner sets the viewport and scissor from the extent its own declaration produced. The old
        // `deferred_pipeline->viewport = ...` line existed because a pipeline object caches what
        // begin_pipeline() applies; the pass never calls begin_pipeline, the runner binds the pipeline.
        // The TAA resolve's pipeline and viewport are NOT resynced here: the runner sets a fullscreen pass's
        // viewport and scissor from the extent its declaration produced, which is what `resync_viewport`
        // means once a pass states it (see deren.vulkan.pass's behaviour). The hazard this list used to guard - a
        // fullscreen pass setting a zero-width viewport because it was left out - is gone by construction.
    }

    // Depth-only shadow-pass content: bind the shared scene block (the light UBO binding 7) +
    // the shadow pipeline, apply the live depth bias and draw every scene-tree leaf (the whole
    // scene casts shadows). Pure bind/push/draw commands - the caller owns the barriers and
    // the depth-only rendering instance around it. Recorded inline today; stage 2 records the
    // same content into a per-slot secondary command buffer for parallel pass recording.
    void runtime::record_shadow_content(VkCommandBuffer const command_buffer, VkPipeline const pipeline, bool const mesh_stage, bool const meshlets) const {
        core const& vk = this->vulkan_core;
        // NO SET IS BOUND (see the heap bind in begin_recording): the light matrices, the camera and the shadow map
        // are heap slots, and the shadow stage's push block carries the two indices that pick this frame's
        // generation. A secondary records its own state, and the heap bind is made on the buffer it records into.
        [[maybe_unused]] uint32_t const frame_slot = static_cast<uint32_t>(vk.current_frame);
        // depth bias is dynamic state on the shadow pipeline: record the live-tunable values
        // (gui-adjustable) before the depth-only draw
        vkCmdSetDepthBias(command_buffer, this->shadow_depth_bias_constant, this->shadow_depth_bias_clamp, this->shadow_depth_bias_slope);
        // Shadow pass render_environment: every leaf draws into the DEPTH-ONLY shadow map, so
        // the binder ignores the requested pipeline name and always binds the shadow pipeline -
        // whatever a custom leaf would draw in the main pass, its geometry still casts the same
        // shadow. Default-semantics leaves hit bind_default() and land here too.
        render_environment env;
        env.command_buffer = command_buffer;
        env.default_name = "shadow"; // binder ignores the name; kept for in_default_pipeline()
        env.bind = [this, pipeline](VkCommandBuffer const cb, std::string_view const /*name*/) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            VkViewport const shadow_viewport = {0.0f, 0.0f, static_cast<float>(this->shadow_map_size), static_cast<float>(this->shadow_map_size), 0.0f, 1.0f};
            VkRect2D const shadow_scissor = {{0, 0}, {this->shadow_map_size, this->shadow_map_size}};
            vkCmdSetViewport(cb, 0, 1, &shadow_viewport);
            vkCmdSetScissor(cb, 0, 1, &shadow_scissor);
        };
        // The shadow pass MUST write depth for every caster: its env always records depth-write
        // ENABLED (VK_TRUE) regardless of what a leaf requests - the first leaf's
        // set_depth_write() emits the one required vkCmdSetDepthWriteEnable and later leaves
        // dedupe against it. (The pipeline declares depth-write as dynamic state, so it must be
        // set at least once even though the value matches the default.)
        env.set_depth_write_fn = [](VkCommandBuffer const cb, VkBool32 const) {
            vkCmdSetDepthWriteEnable(cb, VK_TRUE);
        };
        // ... and it draws every caster TWO-SIDED: a caster is never culled for facing away from
        // the light. A single-sided wall plane whose only face points into the room is back-facing
        // as seen from the sun, so a back-face-culled depth pass never records it and the sunlight
        // pours straight through a wall the camera sees as solid - the classic "the wall behind the
        // camera is transparent" leak. See render_environment::two_sided.
        env.two_sided = true;
        env.set_cull_mode_fn = [](VkCommandBuffer const cb, VkCullModeFlags const mode) {
            vkCmdSetCullMode(cb, mode);
        };
        // The shadow session's endpoint (see render_environment::push_block). `this` is const here because the
        // method is; the endpoint only records into the command buffer, so the cast is a formality.
        env.push_owner = const_cast<runtime*>(this);
        env.push_block = &runtime::push_stage_block;
        // ---- ... AND HOW THIS SESSION FEEDS ITS GEOMETRY: a mesh pipeline has no input assembler, so a caster is
        //      DISPATCHED and its buffers travel as addresses in the stage block instead of being bound (see
        //      primitive::mesh_dispatch). `mesh_stage` is the pass's answer - it is the side that chose which
        //      pipeline to bind - and the three endpoints are this class's because the device is. ----
        env.mesh_stage = mesh_stage;
        // ... AND WHETHER THIS SESSION DRAWS MESHLETS (docs/mesh_shaders.md step 3).
        env.meshlets = meshlets;
        // THE SHADOW BLOCK'S OWN END: its cascade lane is declared after the three heap index lanes, so the geometry
        // lanes start one word later than the scene block's (see mesh_geometry_offset_shadow). Set for BOTH forms
        // of the pass, because both stage blocks declare the lanes - the vertex entry simply never reads them, and
        // a descriptor-heap pipeline requires every declared byte to have been pushed before the draw.
        env.mesh_geometry_push_offset = deren::vulkan::mesh_geometry_push_offset_shadow;
        env.buffer_address = &runtime::mesh_buffer_address;
        env.push_at = &runtime::push_geometry_block;
        if (mesh_stage) {
            env.draw_mesh_tasks = &runtime::draw_mesh_tasks;
            // ... and the INDIRECT twin the meshlet session dispatches through (docs/mesh_shaders.md step 3): the
            // counts live in a command table at the primitive's own slot, which is what a compute culling pass
            // will rewrite. The casters' form decides whether it is used, which is why it is set for both.
            env.draw_mesh_tasks_indirect = &runtime::draw_mesh_tasks_indirect;
            // ONE LINE, ONCE, SAYING HOW THE CASTERS ARE FED - because a mesh stage produces the SAME picture as
            // the vertex stage by construction, so "which one drew this frame" is deliberately invisible in the
            // output and must not be inferred from it. (Measured both ways: the capture gate is byte-identical
            // with either pipeline, and forcing the mesh entry to emit nothing changes it - see
            // docs/mesh_shaders.md step 1.)
            static bool logged = false;
            if (!logged) {
                logged = true;
                deren::utility::log("shadow: the casters are DISPATCHED (mesh stage) - {} casters, {} triangles per workgroup",
                                    this->shadow_casters.size(),
                                    85u);
            }
        }
        // draw only the casters that can throw a shadow into the camera frustum (see
        // shadow_casters in begin_recording); the whole scene only when culling is disabled.
        // BLEND (transparent) leaves are skipped: a depth-only pass has no sensible way to blend
        // an alpha-blended shadow, so "no shadow" stays the correct fallback. MASK leaves ARE
        // drawn - shadow.frag runs the same alpha-cutoff discard as pbr.frag, on the same texture
        // array and material record, so a cut-out caster (foliage, a curtain) casts a cut-out
        // shadow instead of a solid one or, as before, none at all.
        for (primitive const* m : this->shadow_casters) {
            if (m->transparent) {
                continue;
            }
            m->draw(env); // depth-only: shadow.vert transforms into light space
        }
    }

    // ---- post-processing: HDR scene target -> exposure + ACES + gamma -> swapchain ----
    // The post chain's PIPELINES are the composite PASS's (deren.vulkan.pass.post, built by `pipelines::build_post`
    // inside its create step), and its DESCRIPTOR SETS are gone with every other set in this renderer: the
    // composite, the four bloom levels and FXAA read their images through the frame's heap (the HDR target, the
    // bloom levels, the LDR image, the G-buffer depth and normal are all grid slots the shaders name themselves).
    // The samplers stay here only because the pass context hands every pass the five a declaration may choose
    // between (see core::create_samplers).
    // THE FXAA PASS'S RESOLVER IS GONE (S3): its target (the swapchain), the one image it transitions (the LDR
    // image the composite wrote) and its extent all come from its own declaration against the frame's resource
    // table, and its pipeline from the pass. Its push block it composes itself out of the frame's settings and the
    // surface's format, which it cached at create.
    // ---- G-buffer / deferred path (M1: the write pass + its debug view) ----
    // The G-buffer pass is the forward opaque pass with a different fragment stage: same vertex
    // stage, same primitives, same scene set, same instancing/skinning/morphing. What changes is
    // where the fragments go (three 1x targets + a 1x depth image instead of the scene color)
    // and that nothing is lit - see shaders/gbuffer.slang.
    std::expected<void, std::string> runtime::make_gbuffer_pipeline(std::span<uint8_t const> const fragment_shader_code,
                                                                    std::span<uint8_t const> const mesh_vertex_shader_code, std::span<uint8_t const> const meshlet_vertex_shader_code) {
        // ---- THE VERTEX FORM IS GONE (docs/mesh_shaders.md step 4): the G-buffer pass's surface write is fed by a
        // MESH stage, which is why the mesh module is required here and its refusal is an ERROR rather than a log
        // line - with no vertex stage there is nothing to fall back to. (The caller disables the deferred path and
        // says so; a device without the extension never gets this far, the runtime refuses to start.)
        if (mesh_vertex_shader_code.empty()) {
            return std::unexpected(std::string("no mesh stage for the G-buffer pass, and its vertex form is gone (docs/mesh_shaders.md step 4)"));
        }
        if (!this->evaluate_mesh_shaders(nullptr)) {
            return std::unexpected(std::string("the device cannot run the mesh stage the G-buffer pass now requires"));
        }
        auto mesh_result = this->vulkan_core.make_gbuffer_pipeline(mesh_vertex_shader_code, fragment_shader_code, VK_SHADER_STAGE_MESH_BIT_EXT);
        if (!mesh_result) {
            return std::unexpected(std::string(mesh_result.error()));
        }
        this->gbuffer_pipeline_mesh = std::move(mesh_result).value();
        deren::utility::log("SUCCESS: G-buffer MESH pipeline created (the surface write, fed by mesh dispatches)");
        // ---- ... and the MESHLET form (docs/mesh_shaders.md step 3): one workgroup per meshlet, its window read
        //      out of the table and each meshlet culled against the camera before it emits anything. Preferred over
        //      the mesh form when it exists, and a refusal is again a log line rather than a failure.
        if (!meshlet_vertex_shader_code.empty()) {
            auto meshlet_result = this->vulkan_core.make_gbuffer_pipeline(meshlet_vertex_shader_code, fragment_shader_code, VK_SHADER_STAGE_MESH_BIT_EXT);
            if (meshlet_result) {
                this->gbuffer_pipeline_meshlet = std::move(meshlet_result).value();
                deren::utility::log("SUCCESS: G-buffer MESHLET pipeline created (one workgroup per meshlet, camera-culled)");
            } else {
                deren::utility::log("G-buffer MESHLET pipeline refused ({}), so the pass keeps the mesh form", meshlet_result.error());
            }
        }
        return {};
    }

    // THE DEFERRED LIGHTING PASS'S FRAME (deren.vulkan.pass.deferred). The pass owns the barrier, the two per-image
    // input transitions, the LOAD instance, the push and the draw; what the host resolves is the
    // FRAME - which image is lit and the values the push carries - plus deciding that this frame cannot run at all.
    //
    // It was `record_lighting_pass`'s prologue, unchanged, including the order of the two checks: the pipeline
    // first, then the frame's target. The second one is why this returns false instead of recording something, and
    // the caller is what clears the target in that case.
    // THE DEFERRED LIGHTING STAGE'S RESOLVER AND ITS `ensure_inputs` CALLBACK ARE GONE (S3). Its declaration
    // resolves generically - its own pipeline and a
    // full-frame extent - and its TARGET is the frame's alias `scene_color`, which the per-frame resource table
    // answers (the TAA input while the resolve runs, the HDR image otherwise). Its push block it composes itself
    // out of its own SSAO parameters, the frame's inverse view-projection and the frame's answer to what the traced
    // chain is doing. The two per-image transitions the frame used to carry are the frame's ORDERING rule about the
    // images the G-buffer pass wrote, so they now run in the deferred STAGE's preamble in `record_main_drawcalls` -
    // the same move the ray-traced shadow stage's identical pair made, and the same command-stream position.

    void runtime::clear_scene_color_for_missing_gbuffer(VkCommandBuffer const command_buffer) {
        // The deferred lighting pass did not record - its pipeline is missing (a startup failure), or its target
        // is not in this frame's resource table. The pass cannot do this itself, because a pass records nothing
        // when its declaration does not resolve - so the frame's answer lives here: clear
        // the target so the frame is DEFINED (the post chain samples it) instead of leaving whatever the
        // background/emissive wrote mixed with garbage, and say so once per frame, because a silent black frame
        // is worse than a log line. (The log line's wording is historical: the missing thing used to be a
        // descriptor set, and the frame's answer to a missing one was this same clear.)
        core const& vk = this->vulkan_core;
        uint32_t const index = this->current_image_index;
        if (index >= vk.scene_color_images.size()) {
            return;
        }
        deren::utility::log("runtime: deferred lighting has no descriptor set - clearing the scene target");
        std::array<VkImageMemoryBarrier2, 1> clear_barrier = {deren::vulkan::color_attachment_transition};
        clear_barrier[0].image = vk.scene_color_images[index];
        VkDependencyInfo const clear_dependency = make_image_dependency_info(1, clear_barrier.data());
        vkCmdPipelineBarrier2(command_buffer, &clear_dependency);
        VkClearValue clear = {};
        VkRenderingAttachmentInfo const attachment = make_color_attachment_info(vk.scene_color_image_views[index], clear, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE);
        VkRenderingInfo const rendering_info = make_rendering_info(0, {{0, 0}, vk.render_extent()}, true, &attachment, nullptr);
        vkCmdBeginRendering(command_buffer, &rendering_info);
        vkCmdEndRendering(command_buffer);
    }

    // The G-buffer declarations' SAMPLERS: what is left of make_gbuffer_debug_pipeline in the renderer, because the
    // view pipeline is the debug view's PASS's now. They have to exist
    // before create_passes(), since the pass context hands every pass the five a declaration may choose between.
    // The deferred path's transparent pass. Everything about its position is load-bearing:
    //  - after the lighting stage, because a blended surface composites over SHADED pixels, and the
    //    G-buffer instance has no shaded image to composite over;
    //  - not INSIDE the lighting instance, because that one samples the G-buffer depth as a texture
    //    while this one needs the same image as its depth ATTACHMENT, and an image cannot be both in
    //    one instance;
    //  - before the TAA resolve, so the resolve sees the composited frame.
    void runtime::record_transparent_pass(VkCommandBuffer const command_buffer) {
        static_cast<void>(command_buffer); // the pass records into the frame's command buffer it is resolved with
        // THE TRANSPARENT PASS records the blended geometry: the two hand-off barriers, the LOAD instance over
        // its two declared targets, one secondary and the depth hand-back - all in one function now (see
        // deren.vulkan.pass.transparent). Like the scene pass it is SKIPPED without resolving anything on a frame
        // whose culling left nothing blended, which is what keeps a frame with no blended leaves byte-exact.
        pass::stage const transparent_stage = {.name = "transparent", .passes = this->transparent_pass, .marks = false};
        this->prepare_stage(transparent_stage, command_buffer);
        [[maybe_unused]] pass::run_report const transparent_report = pass::record_stage(transparent_stage, this->make_pass_host());
    }
    // ---- temporal anti-aliasing (M3) ----
    glm::vec2 runtime::taa_jitter_offset(uint32_t const index) noexcept {
        // Halton(2,3): the two-radical sequence has the low-discrepancy property that matters here -
        // consecutive samples fill the pixel evenly instead of clustering, so 8 frames of a static
        // scene already resolve close to a 4x4 grid. The offsets are centred on the pixel and given in
        // PIXELS; the caller converts them to a projection offset.
        auto const halton = [](uint32_t position, uint32_t base) {
            float result = 0.0f;
            float fraction = 1.0f;
            while (position > 0) {
                fraction /= static_cast<float>(base);
                result += fraction * static_cast<float>(position % base);
                position /= base;
            }
            return result;
        };
        uint32_t const i = index + 1; // Halton starts at 1 (position 0 gives 0 for every base)
        return {halton(i, 2) - 0.5f, halton(i, 3) - 0.5f};
    }

    bool runtime::taa_active() const noexcept {
        // TAA is
        // the engine's answer to aliasing: a 1x G-buffer cannot be multisampled, so there is no MSAA to fall back on.
        return this->taa_on && this->pass_ready("taa") && this->deferred_lit_active();
    }

    VkImage runtime::scene_target_image(uint32_t const image_index) const noexcept {
        core const& vk = this->vulkan_core;
        return this->taa_active() ? vk.scene_color_images[image_index] : vk.hdr_images[image_index];
    }

    VkImageView runtime::scene_target_view(uint32_t const image_index) const noexcept {
        core const& vk = this->vulkan_core;
        return this->taa_active() ? vk.scene_color_image_views[image_index] : vk.hdr_image_views[image_index];
    }

    bool runtime::set_taa_enabled(bool const enabled) noexcept {
        // THE FLAG IS THE RENDERER'S and the two WEIGHTS are the pass's (the demo sets them): TAA is not only a
        // pass here - the projection is jittered from `taa_active()` and the scene target is chosen with it - so the
        // renderer has to know whether it is on, while the values only the pass reads.
        bool const was_on = this->taa_on;
        this->taa_on = enabled;
        if (enabled) {
            if (!this->pass_ready("taa")) {
                this->warn_missing_feature("taa", "TAA has no effect: the taa pipeline was not created (see the startup log)");
            } else if (!this->deferred_lit_active()) {
                this->warn_missing_feature("taa", "TAA has no effect: the G-buffer pass or its lighting stage was not created (see the startup log)");
            }
        }
        bool const turned_on = enabled && !was_on;
        if (turned_on) {
            // A fresh history - but only on the off -> on EDGE. The caller mirrors the GUI/config state
            // into the runtime every frame (see main.cpp), so resetting unconditionally here would
            // invalidate the history on every frame: the resolve would fall back to the current
            // (jittered, aliased) frame forever, which looks like TAA running while doing nothing.
            std::size_t const image_count = this->vulkan_core.taa_history_images.size();
            this->image_view_proj.assign(image_count, this->current_ubo.view_proj_unjittered);
            this->taa_jitter_index = 0;
        }
        // ... and the PASS's half of that edge (whether each image's history holds anything) is the caller's to
        // apply, which is what the return value says: it is the one thing the flag's owner cannot do for the pass.
        return turned_on;
    }

    // The TAA resolve's factory is gone: `deren.vulkan.pass.taa` builds its own pipeline and owns its per-image
    // history flags in its create step, from its own declaration and its own shaders (the app registers those).

    bool runtime::ensure_gbuffer_depth_sampled(VkCommandBuffer const command_buffer, uint32_t const image_index) {
        // Nothing to do when no G-buffer instance ran for this image: the depth is already in the
        // layout the sampling descriptors declare (it keeps whatever the last frame for this image
        // left it in), so re-transitioning would only claim a layout the image is not in.
        if (image_index >= this->gbuffer_depth_written.size() || !this->gbuffer_depth_written[image_index]) {
            return false;
        }
        // The G-buffer pass left it in GENERAL: publish the attachment
        // write and flip it to the layout the sampling descriptors declare. One barrier per frame,
        // whichever of the three sampling stages gets here first.
        VkImageMemoryBarrier2 barrier = deren::vulkan::shadow_map_sampling_transition;
        barrier.image = this->vulkan_core.gbuffer_depth_images[image_index];
        VkDependencyInfo const dependency = make_image_dependency_info(1, &barrier);
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        this->gbuffer_depth_written[image_index] = false;
        return true;
    }

    bool runtime::ensure_gbuffer_targets_sampled(VkCommandBuffer const command_buffer, uint32_t const image_index) {
        // Nothing to do when no G-buffer instance ran for this image, or when an earlier stage already
        // took the transition (the ray-traced shadow pass runs first when it runs): re-transitioning
        // would claim a COLOR_ATTACHMENT old layout the image is not in.
        if (image_index >= this->gbuffer_targets_written.size() || !this->gbuffer_targets_written[image_index]) {
            return false;
        }
        std::array<VkImageMemoryBarrier2, deren::vulkan::gbuffer_target_count> barriers = {};
        for (uint32_t target = 0; target < deren::vulkan::gbuffer_target_count; ++target) {
            barriers[target] = deren::vulkan::hdr_sampling_transition; // COLOR_ATTACHMENT -> SHADER_READ
            barriers[target].image = this->vulkan_core.gbuffer_images[target][image_index];
        }
        VkDependencyInfo const dependency = make_image_dependency_info(static_cast<uint32_t>(barriers.size()), barriers.data());
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        this->gbuffer_targets_written[image_index] = false;
        return true;
    }
    bool runtime::ensure_velocity_sampled(VkCommandBuffer const command_buffer, uint32_t const image_index) {
        // Nothing to do when no G-buffer instance ran for this image, or when an earlier sampler
        // already took the transition (the TAA resolve, which runs before the GI denoiser in the same
        // frame): re-transitioning would claim a COLOR_ATTACHMENT old layout the image is not in.
        if (image_index >= this->velocity_written.size() || !this->velocity_written[image_index]) {
            return false;
        }
        VkImageMemoryBarrier2 barrier = deren::vulkan::hdr_sampling_transition; // COLOR_ATTACHMENT -> SHADER_READ
        barrier.image = this->vulkan_core.velocity_images[image_index];
        VkDependencyInfo const dependency = make_image_dependency_info(1, &barrier);
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        this->velocity_written[image_index] = false;
        return true;
    }

    bool runtime::gbuffer_pass_active() const noexcept {
        // THE MESH FORMS ARE THE PIPELINE NOW (docs/mesh_shaders.md step 4): `gbuffer_pipeline` is the vertex form,
        // which no longer exists, so the question is whether EITHER mesh form was built.
        if (!this->gbuffer_pipeline_mesh.has_value() && !this->gbuffer_pipeline_meshlet.has_value()) {
            return false;
        }
        if (this->gbuffer_debug) {
            return this->pass_ready("gbuffer-debug");
        }
        return this->pass_ready("deferred");
    }

    bool runtime::deferred_lit_active() const noexcept {
        // the debug view wins when both are available: looking at the stored data is an inspection,
        // not a render mode (and the two write the HDR target in incompatible ways)
        return this->gbuffer_pass_active() && !this->gbuffer_debug;
    }

    // `set_gbuffer_channel` IS GONE (the demo's now): it was a pure forwarder to the debug view's own parameter
    // (which pass owns the channel and its clamp), and nothing in this file read it - the demo sets it on the pass
    // it found by declaration name.

    // WHAT IS NOT HERE ANY MORE: no per-pass resolver, and no G-buffer descriptor family either. The G-buffer
    // images are HEAP slots now (publish_frame_resources writes each image's grid slot once per frame), and the
    // rule every pass follows is the one this space records: a pass resolves its OWN declaration (its own bindings,
    // its pipeline, the extent rule), and its push block is composed by the PASS from `io.constants` plus its own
    // parameters. A frame fact is likewise the frame's (`make_frame_facts`) rather than the renderer's copy of a
    // pass's state.

    bool runtime::megalights_active() const noexcept {
        // deferred lighting stage has to know whether to skip its raster punctual loop, and it has to give the
        // same answer the runner gives when it decides whether to record the pass - one predicate, one answer.
        // The flat render mode is excluded because this pass EVALUATES THE BRDF from the G-buffer and the flat
        return this->megalights_on && this->pass_ready("megalights_trace") && this->deferred_lit_active() && !this->scene_unlit;
    }

    bool runtime::set_megalights_enabled(bool const enabled) noexcept {
        // adds the punctual lights itself, so it is a frame fact this renderer publishes), the sample count and
        // the bias are the PASS's and the demo sets them. There is no history to reset on the off -> on edge
        // yet - the chain is one pass until the temporal resolve lands (see docs/megalights.md's staging).
        bool const was_on = this->megalights_on;
        this->megalights_on = enabled;
        if (enabled && !this->pass_ready("megalights_trace")) {
            this->warn_missing_feature("megalights", "stochastic punctual lighting has no effect: its compute pipeline was not created (see the startup log)");
        } else if (enabled && !this->clustered_lights) {
            // Not a failure: the shader falls back to the brute-force list (cluster_index_at returns -1 and the
            // loop walks every active light), which is the same reference path the raster shading has. Worth
            // saying once because it is the difference between "a few samples over the pixel's lights" and "a
            // few samples over ALL of them" in cost, not in correctness.
            deren::utility::log("stochastic punctual lighting: clustered culling is off, so every light is a candidate for every pixel");
        }
        return enabled && !was_on;
    }

    void runtime::set_furnace(bool const enabled) noexcept {
        this->furnace = enabled;
    }

    void runtime::register_shader(std::string_view const name, std::span<uint8_t const> const bytecode) {
        // The app loads shaders (it knows the directory and the file names) and hands them over here; a pass
        // asks for its own by name at create time. A COPY, because the caller's buffer is a startup local.
        for (auto& [registered_name, registered_bytes] : this->registered_shaders) {
            if (registered_name == name) {
                registered_bytes.assign(bytecode.begin(), bytecode.end());
                return;
            }
        }
        this->registered_shaders.emplace_back(std::string(name), std::vector<uint8_t>(bytecode.begin(), bytecode.end()));
    }

    void runtime::create_passes() {
        // The runner's create step for every stage this runtime wires. A pass builds what it owns from its own
        // declaration and its own shader, so this is also where a pass that could not build itself says so -
        // and a pass that says so stays INACTIVE (its feature predicate is false), which is what makes a
        // startup failure here a log line rather than a broken frame.
        //
        // IT IS HANDED A CONTEXT, NOT A HOST: building a pass needs a device and the shared lookups, and it
        // needs no frame. ANY owner can fill this struct - that is what makes a pass usable outside this
        // renderer - and this runtime is one such owner, filling the device from the core it owns.
        //
        // The SHARED samplers must exist before the context is filled, because a pass caches the five it may
        // choose between at create time (a declaration picks one by hint, and a null sampler in a set is a
        // validation error rather than a skipped fetch). They are the device root's (`core::create_samplers`),
        // which is what `shared_samplers` below reads; two of them were once made inside the pipeline builders
        // that first needed them, which is the naming accident that comment records.
        // THE CHAIN IS AN INPUT, and there is deliberately no fallback: with the passes constructed outside this
        // class, "no chain was handed over" means there is nothing to create or record, so the one honest answer is
        // to say so and return rather than to record a frame of this class's own empty stage sequence.
        if (this->frame_chain == nullptr) {
            deren::utility::log("no pass chain was handed over (see set_pass_chain): nothing to create or record");
            return;
        }
        // ... and what the passes may NAME, before any of them is created: the registry the pass filter answers
        // `resource()` from (see publish_pass_resources). It is the renderer's half of the channel; the passes'
        // half is that they ask for what their own declaration lists instead of being handed it.
        this->publish_pass_resources();
        // THE FRAME'S STRUCTURE, FROM THE CHAIN BY DECLARATION NAME: the stage arrays and the two GI halves (see
        // bind_frame_chain). The chain is the application's (see set_pass_chain) - this class owns no passes, so
        // there is no chain of its own for this to bind.
        this->bind_frame_chain(*this->frame_chain);
        // ---- MESH SHADERS: MAY A PASS BUILD ONE ON THIS DEVICE? (docs/mesh_shaders.md) ----
        // THE THREE CONDITIONS ARE MEASURED HERE, once, before any pass is created - because a pass that already
        // built a mesh pipeline on a device that cannot run one is a validation ERROR at vkCreateShaderModule, and
        // "the pass asked and was refused" is too late. The answer is logged either way: whether the shadow pass
        // drew its casters as dispatches must not be something a reader has to infer from a picture.
        //  1. the extension and its `meshShader` feature (core::mesh_shader_available), and
        //  2. the device's push-constant budget, and the heap's push-data window, both >= the stage block a mesh
        //     stage needs (`mesh_stage_block_size`): a mesh stage is handed the draw's whole geometry window as
        //     data because it has no input assembler to take it from, and that block is larger than the 128 bytes
        //     the spec guarantees - so the path is available where the device reports room and the VERTEX path
        //     stays where it does not.
        {
            uint32_t const push_constants = this->vulkan_core.device_properties.limits.maxPushConstantsSize;
            uint32_t const push_data = this->vulkan_core.descriptor_heap_limits.max_push_data;
            this->mesh_shaders = this->evaluate_mesh_shaders(&this->mesh_shaders_unavailable_reason);
            if (!this->mesh_shaders) {
                // ---- THE EXTENSION IS A REQUIREMENT NOW (docs/mesh_shaders.md step 4): the vertex geometry path is
                // gone, so there is no second rasterizer to fall back to and a device that cannot run a mesh stage
                // cannot draw the scene at all. Saying so here, once, with the reason, is the whole of the fallback
                // that is left - and it happens BEFORE any pass is created.
                deren::utility::panic(std::source_location::current(),
                                      "mesh shaders are required and this device does not offer them: {} (docs/mesh_shaders.md step 4)",
                                      this->mesh_shaders_unavailable_reason);
            }
            deren::utility::log("mesh shaders: REQUIRED and available - every geometry stage is dispatched (push block {} B, within {} B push constants / {} B push data)",
                                deren::vulkan::mesh_stage_block_size,
                                push_constants,
                                push_data);
        }
        pass::pass_context const build = this->make_pass_context();
        // ONE CREATE STEP OVER EVERY PASS, in the order the OWNING chain holds them (see the member block in the
        // header): the chain owns the passes and its `init` IS the whole create step, in the order the application
        // emplaced them. A pass that could not build itself reports its own name in `rejected` and stays INACTIVE
        // (its feature predicate is false), which is what makes a startup failure a log line rather than a broken
        // frame.
        pass::pass_chain& recorded = *this->frame_chain; // the chain the application handed over (see set_pass_chain)
        pass::run_report const created = recorded.init(build);
        if (!created.rejected.empty()) {
            deren::utility::log("pass '{}': its declaration was refused by the validator, so it does not run", created.rejected);
        }
        // THE TWO JOBS, and they are created HERE rather than by the application: neither is a frame pass (see
        // their headers - one runs once inside the structure-build command buffer, the other per frame from a
        // caster list), but both are GPU-owning objects constructed from the same context, so they belong in the
        // same step. Before the pass filter existed each had its own entry point in this class, because a pass
        // could not name a resource the renderer owns; now it asks (see publish_pass_resources).
        // mask_bake_result, not created: `created` above is in scope and MSVC /W4 reports C4456 (an error under /WX).
        if (auto const mask_bake_result = this->mask_bake.create(build); !mask_bake_result) {
            deren::utility::log("alphaMode MASK bake unavailable: {} (the any-hit stage still cuts masked geometry per hit)", mask_bake_result.error());
        }
        // compute_skin_result, not created: same C4456 rule as the mask-bake job above - the outer `created` is in scope.
        if (auto const compute_skin_result = this->compute_skin.create(build); !compute_skin_result) {
            deren::utility::log("skinned shadow refit unavailable: {} (a traced shadow keeps the bind pose)", compute_skin_result.error());
        }
    }

    render_resource::shared::sampler_set runtime::shared_samplers() const noexcept {
        // The five samplers a declaration chooses between, as handles. One place, so that two passes cannot end
        // up with two different ideas of "the post sampler".
        // THE SAMPLERS ARE THE DEVICE ROOT'S (core::create_samplers): a sampler has no per-frame state and no owner
        // among the passes, so this function is now a READ of the handles rather than the place that made them - and
        // it stays the single place the renderer maps them onto the declaration layer's hints.
        core const& vk = this->vulkan_core;
        return {.gbuffer = *vk.gbuffer_sampler,
                .taa = *vk.taa_sampler,
                .post = *vk.post_sampler,
                .nearest = *vk.post_nearest_sampler,
                .shadow = *vk.shadow_sampler,
                // The bindless texture array's sampler, which only hand-written set code used until the MASK
                // bake had to write the scene layout's binding 1 itself (see sampler_set's doc).
                .textures = *vk.texture_sampler};
    }

    std::span<uint8_t const> runtime::registered_shader(std::string_view const name) const noexcept {
        for (auto const& [registered_name, registered_bytes] : this->registered_shaders) {
            if (registered_name == name) {
                return registered_bytes;
            }
        }
        return {}; // a pass whose shader was never registered builds nothing and says so
    }

    bool runtime::evaluate_mesh_shaders(std::string* const reason) const noexcept {
        // THE GATE, as one question asked in two places (create_passes, for the log and the pass context; and the
        // G-buffer pipeline's builder, which runs earlier). In both cases the answer is the same three facts, and
        // the reason string is only filled in for the one caller that logs it.
        uint32_t const push_constants = this->vulkan_core.device_properties.limits.maxPushConstantsSize;
        uint32_t const push_data = this->vulkan_core.descriptor_heap_limits.max_push_data;
        auto const unavailable = [reason](std::string text) {
            if (reason != nullptr) {
                *reason = std::move(text);
            }
            return false;
        };
        if (!this->vulkan_core.mesh_shader_available) {
            return unavailable("the device has no VK_EXT_mesh_shader meshShader feature");
        }
        if (push_constants < deren::vulkan::mesh_stage_block_size) {
            return unavailable(std::format("maxPushConstantsSize is {} B, under the {} B block a mesh stage needs", push_constants, deren::vulkan::mesh_stage_block_size));
        }
        if (push_data < deren::vulkan::mesh_stage_block_size) {
            return unavailable(std::format("the heap's maxPushDataSize is {} B, under the {} B block a mesh stage needs", push_data, deren::vulkan::mesh_stage_block_size));
        }
        if (reason != nullptr) {
            reason->clear();
        }
        return true;
    }

    pass::pass_context runtime::make_pass_context() noexcept { // THE ONE CONSTRUCTION SITE for a pass's create-time context, and it is a method rather than a block
        // because there were two of them: `create_passes()` built one for the stages, and the two jobs that are
        // not frame passes (the MASK bake, the compute-skinning job) each built their own copy. A second copy of
        // this struct is how a per-pass entry point per job appears, which is what the pass filter exists to
        // remove - so there is one builder now, and everything that constructs a pass uses it.
        return pass::pass_context{
            .device = this->vulkan_core.logical_device,
            .samplers = this->shared_samplers(),
            .shader = [](void* owner, std::string_view const name) { return static_cast<runtime*>(owner)->registered_shader(name); },
            // The SBT numbers a tracing pass builds its table against, straight from the capability query (see
            // device_capabilities): zeroed here on a device that has no ray-tracing pipeline.
            .ray_tracing_properties = this->vulkan_core.ray_tracing_pipeline_properties,
            // ... and the owner's buffer factory: the pass gets a handle and a device address, the runtime keeps
            // the allocation for the generation (see `pass_upload_buffers`).
            .create_upload_buffer =
                [](void* owner, void const* data, uint64_t const bytes, VkBufferUsageFlags const usage, VkDeviceAddress* const out_address) -> VkBuffer {
                runtime* const self = static_cast<runtime*>(owner);
                // THE CALLER'S USAGE BITS BECOME CONTRACT FLAGS HERE, and a bit this mapping does not name is
                // a NAMED FAILURE rather than a silent drop - the pass framework's own rule ("a missing
                // capability is a named failure, never a silent skip"), and the reason it matters here is that
                // a buffer created WITHOUT the bit its descriptor needs reads as zeros, with no validation
                // finding at all. The census today is one caller: the ray-traced shadow's shader binding table.
                rhi::buffer_flags flags = rhi::to_bits(rhi::buffer_flag::device_address);
                switch (usage) {
                case 0u:
                    break;
                case VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR:
                    flags |= rhi::to_bits(rhi::buffer_flag::shader_binding_table);
                    break;
                default:
                    deren::utility::panic(std::source_location::current(),
                                          "pass upload buffer: the caller asked for Vulkan usage bits {:#x}, which the contract has no flag for", usage);
                }
                // The mapping is APPENDED BEFORE the address is asked for, so the reference the address call
                // reads is the one this class owns - and the raw handle handed back is borrowed from it, exactly
                // as `vulkan_escape::native_buffer` documents: valid while `pass_upload_buffers` holds it.
                self->pass_upload_buffers.emplace_back();
                create_buffer(self->vulkan_core, rhi::buffer_usage::storage_coherent, flags,
                              std::span<std::byte const>(static_cast<std::byte const*>(data), static_cast<std::size_t>(bytes)),
                              "pass upload buffer", self->pass_upload_buffers.back(), nullptr);
                if (out_address != nullptr) {
                    *out_address = self->buffer_address(*self->pass_upload_buffers.back());
                }
                return self->buffer_of(*self->pass_upload_buffers.back());
            },
            // The surface's format: a SESSION-STABLE device fact a pipeline that renders into the swapchain must
            // be created with (see pass_context). The post chain needs it today; the graphics passes being
            // extracted need it tomorrow.
            .swap_chain_image_format = this->vulkan_core.swap_chain_image_format,
            // ... and the DEPTH format, which the shadow pass`s pipeline needs (it has a depth attachment and no
            // colour one): the same kind of session-stable device fact, and the second one a context carries.
            .depth_format = this->vulkan_core.depth_attachment_format,
            // ... and whether the device can run a MESH pipeline at all, which a pass must not try to find out by
            // attempting it (see the metric above and pass_context::mesh_shaders).
            .mesh_shaders = this->mesh_shaders,
            // The channel a pass uses to build what it owns over resources the RENDERER holds: the handles of the
            // resources this runtime published (`pass_resources`). It forwards to the filter, which is the object
            // that knows what a pass may reach - the context itself stays a plain struct of callbacks, so the
            // framework still does not depend on `deren.vulkan.core`.
            .resource =
                [](void* owner, render_resource::resource_id const id, uint32_t const element) {
                    runtime* const self = static_cast<runtime*>(owner);
                    resource_handles const handles = self->pass_resources.resource(id, element);
                    return pass::resolved_binding{.view = handles.view, .buffer = handles.buffer, .image = handles.image};
                },
            .frames_in_flight = deren::vulkan::core::MAX_FRAMES_IN_FLIGHT,
            .owner = this,
        };
    }

    void runtime::publish_pass_resources() {
        // WHAT THE RENDERER PUBLISHES FOR ITS PASSES, in the declaration layer's own vocabulary: a pass asks for
        // `resource_id::material_table` or `resource_id::skin_matrices` + a slot, not for a member of this class.
        // Everything here is SESSION-STABLE by the filter's contract (the material table and the texture array
        // are created once and only rewritten; the skin matrix buffers are created once and rewritten per slot),
        // which is what makes them safe for a pass to name at create time.
        if (this->material_mapped != nullptr) {
            this->pass_resources.register_resource(render_resource::resource_id::material_table, 0, resource_handles{.buffer = this->buffer_of(*this->material_buffer)});
        }
        if (!this->owned_texture_views.empty()) {
            this->pass_resources.register_resource(render_resource::resource_id::scene_textures, 0, resource_handles{.view = *this->owned_texture_views[0]});
        }
        for (uint32_t slot = 0; slot < this->skin_buffers.size(); ++slot) {
            this->pass_resources.register_resource(render_resource::resource_id::skin_matrices, slot, resource_handles{.buffer = this->buffer_of(*this->skin_buffers[slot])});
        }
    }

    void runtime::update_frame_constants() noexcept {
        // THE ONE PLACE THE FACTS TYPE AND THE UBOs HAVE TO AGREE. `deren.vulkan.frame_constants` deliberately does not
        // include `camera_ubo`/`light_ubo` (the pass framework must not depend on deren.vulkan.primitive, which imports
        // core), so the fields are copied here - one site to check, instead of a type relationship spread across
        // two modules that only a reader of both would notice breaking.
        this->frame_facts.view = this->current_ubo.view;
        this->frame_facts.proj = this->current_ubo.proj; // JITTERED: the geometry is sampled at these offsets
        this->frame_facts.view_proj_unjittered = this->current_ubo.view_proj_unjittered;
        this->frame_facts.inv_view_proj = this->current_inv_view_proj;
        this->frame_facts.camera_pos = glm::vec3(this->current_ubo.camera_pos);
        this->frame_facts.scene_center = this->shadow_scene_center;
        this->frame_facts.scene_radius = this->scene_extent_radius;
        // The sun, normalized here because the shader wants a
        // direction, the UBO's own lane stays as the app set it). A zero direction - nothing has set a light yet
        // - is kept as zero rather than turned into a NaN by normalize().
        glm::vec3 const sun = glm::vec3(this->light_state.light_dir);
        this->frame_facts.light_dir = glm::dot(sun, sun) > 0.0f ? glm::vec4(glm::normalize(sun), 0.0f) : glm::vec4(0.0f);
        // ... and the renderer's settings that MORE THAN ONE pass reads (see render_settings for the rule): the
        // post chain's exposure and bloom weights, FXAA's two thresholds, and the GI upsample's silhouette test.
        // The members stay the app-facing ones (`set_exposure` and friends) - this is the frame's copy of them.
        this->frame_facts.settings.exposure = this->exposure_scale;
        this->frame_facts.settings.bloom_intensity = this->bloom_intensity;
        this->frame_facts.settings.bloom_threshold = this->bloom_threshold;
        this->frame_facts.settings.fxaa_subpixel = this->fxaa_subpixel;
        this->frame_facts.settings.fxaa_edge_threshold = this->fxaa_edge_threshold;
    }

    void runtime::publish_frame_resources() {
        core const& vk = this->vulkan_core;
        pass::resource_table& table = this->frame_resources;
        table.clear();

        // One per-swapchain-image family: published as the run of views and images the core already holds (see
        // resource_table::publish_family), so the per-image channel is that same run rather than a second copy of
        // it. `element` is the family's own index (the G-buffer's third target, the bloom chain's level 2).
        auto const family = [&table](render_resource::resource_id const id, uint32_t const element, std::span<VkImageView const> views, std::span<VkImage const> images) {
            table.publish_family(id, element, views, images);
        };
        // One resource that exists once (a device-wide image): the instance is still the schema's (0 for
        // device-wide, the slot for a per-frame-slot buffer that happens to exist once).
        auto const single = [&table](render_resource::resource_id const id, uint32_t const element, uint32_t const instance, pass::resolved_binding const binding) {
            table.publish(id, element, instance, binding);
        };
        // One buffer: the RAII wrapper holds a handle and the descriptor needs the VkBuffer behind it, so the
        // lookup goes through vma exactly where the renderer's own binding writes do.
        auto const buffer = [this, &table](render_resource::resource_id const id, uint32_t const instance, rhi::object_manager<rhi::buffer> const& owned) {
            table.publish(id, 0, instance, pass::resolved_binding{.buffer = this->buffer_of(*owned)});
        };
        auto const image = [this](vk_image const& owned) -> pass::resolved_binding {
            auto const* const detail = this->vulkan_core.vma.get_image_detail(owned.handle());
            return detail == nullptr ? pass::resolved_binding{} : pass::resolved_binding{.image = detail->image};
        };

        // ---- the render-target chain: the image families core owns ----
        family(render_resource::resource_id::swapchain_image, 0, vk.swap_chain_image_views, vk.swap_chain_images);
        family(render_resource::resource_id::hdr, 0, vk.hdr_image_views, vk.hdr_images);
        family(render_resource::resource_id::ldr, 0, vk.ldr_image_views, vk.ldr_images);
        family(render_resource::resource_id::gbuffer_depth, 0, vk.gbuffer_depth_image_views, vk.gbuffer_depth_images);
        family(render_resource::resource_id::velocity, 0, vk.velocity_image_views, vk.velocity_images);
        family(render_resource::resource_id::taa_history, 0, vk.taa_history_image_views, vk.taa_history_images);
        family(render_resource::resource_id::ml_trace, 0, vk.ml_image_views, vk.ml_images);
        family(render_resource::resource_id::ml_resolve, 0, vk.ml_resolve_image_views, vk.ml_resolve_images);
        family(render_resource::resource_id::ml_history, 0, vk.ml_history_image_views, vk.ml_history_images);
        for (std::size_t target = 0; target < vk.gbuffer_image_views.size() && target < vk.gbuffer_images.size(); ++target) {
            family(render_resource::resource_id::gbuffer_targets, static_cast<uint32_t>(target), vk.gbuffer_image_views[target], vk.gbuffer_images[target]);
        }
        for (std::size_t level = 0; level < vk.bloom_image_views.size() && level < vk.bloom_images.size(); ++level) {
            family(render_resource::resource_id::bloom, static_cast<uint32_t>(level), vk.bloom_image_views[level], vk.bloom_images[level]);
        }
        // `scene_color` is an ALIAS rather than a family of its own: the scene-side passes write the TAA input
        // while the resolve runs and the HDR target otherwise (see scene_target_view), so WHAT THE ID MEANS is
        // decided here, once per frame, in the same place that answers it for the resolvers - which is also why
        // the table is refreshed per frame rather than per generation.
        std::size_t const scene_images = std::min({vk.scene_color_image_views.size(), vk.scene_color_images.size(), vk.hdr_image_views.size(), vk.hdr_images.size()});
        for (std::size_t i = 0; i < scene_images; ++i) {
            single(render_resource::resource_id::scene_color, 0, static_cast<uint32_t>(i),
                   pass::resolved_binding{.view = this->scene_target_view(static_cast<uint32_t>(i)), .buffer = VK_NULL_HANDLE, .image = this->scene_target_image(static_cast<uint32_t>(i))});
        }

        // ---- per FRAME SLOT: the ray-traced visibility image, the shadow map's cascades, the buffers ----
        for (std::size_t slot = 0; slot < vk.rt_shadow_image_views.size() && slot < vk.rt_shadow_images.size(); ++slot) {
            single(render_resource::resource_id::rt_shadow_visibility, 0, static_cast<uint32_t>(slot),
                   pass::resolved_binding{.view = vk.rt_shadow_image_views[slot], .buffer = VK_NULL_HANDLE, .image = vk.rt_shadow_images[slot]});
        }
        // The shadow map's family elements are the CASCADES (see render_resource::shadow_io): the image is one
        // layered depth array per slot and the pass renders one layer at a time, so every layer the image
        // currently HAS is published, by cascade index, with the image behind it for the layer's own barrier.
        for (std::size_t slot = 0; slot < this->shadow_images.size() && slot < this->shadow_layer_views.size(); ++slot) {
            auto const* const detail = this->vulkan_core.vma.get_image_detail(this->shadow_images[slot].handle());
            if (detail == nullptr) {
                continue;
            }
            for (std::size_t layer = 0; layer < this->shadow_layer_views[slot].size(); ++layer) {
                single(render_resource::resource_id::shadow_map, static_cast<uint32_t>(layer), static_cast<uint32_t>(slot),
                       pass::resolved_binding{.view = *this->shadow_layer_views[slot][layer], .buffer = VK_NULL_HANDLE, .image = detail->image});
            }
        }
        // The per-slot buffers, each into its own instance: a frame in flight reads its own copy, which is the
        // whole reason those resources exist per slot (see the member docs).
        uint32_t const slots = static_cast<uint32_t>(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT);
        for (uint32_t slot = 0; slot < slots; ++slot) {
            if (slot < this->camera_buffers.size()) {
                buffer(render_resource::resource_id::camera_ubo, slot, this->camera_buffers[slot]);
            }
            if (slot < this->light_buffers.size()) {
                buffer(render_resource::resource_id::light_ubo, slot, this->light_buffers[slot]);
            }
            if (slot < this->motion_buffers.size()) {
                buffer(render_resource::resource_id::motion_vectors, slot, this->motion_buffers[slot]);
            }
            if (slot < this->skin_buffers.size()) {
                buffer(render_resource::resource_id::skin_matrices, slot, this->skin_buffers[slot]);
            }
            if (slot < this->morph_buffers.size()) {
                buffer(render_resource::resource_id::morph_targets, slot, this->morph_buffers[slot]);
            }
            if (slot < this->cluster_count_buffers.size()) {
                buffer(render_resource::resource_id::cluster_counts, slot, this->cluster_count_buffers[slot]);
            }
            if (slot < this->cluster_index_buffers.size()) {
                buffer(render_resource::resource_id::cluster_indices, slot, this->cluster_index_buffers[slot]);
            }
            // ... and the two that exist ONCE while their schema scope still says per-frame-slot: the material
            // table and the instance transforms are rewritten in place rather than per slot, which is why the
            // same handle is published for every instance.
            buffer(render_resource::resource_id::material_table, slot, this->material_buffer);
            buffer(render_resource::resource_id::instance_table, slot, this->instance_buffer);
        }

        // ---- device-wide: the probe grid's eight elements, its geometry, the cubes and the textures ----
        if (!vk.furnace_cube_views.empty() && !vk.furnace_cube_images.empty()) {
            single(render_resource::resource_id::furnace_cube, 0, 0,
                   pass::resolved_binding{.view = vk.furnace_cube_views[0], .buffer = VK_NULL_HANDLE, .image = vk.furnace_cube_images[0]});
        }
        // The white fallback and the array it is element 0 of. The array is BINDLESS (one binding, N descriptors),
        // which the declaration vocabulary cannot index element by element yet - so what is published is the one
        // element that always exists, which is also the one every declaration can name (element 0).
        if (this->white_texture_index < this->owned_textures.size() && this->white_texture_index < this->owned_texture_views.size()) {
            pass::resolved_binding white = image(this->owned_textures[this->white_texture_index]);
            white.view = *this->owned_texture_views[this->white_texture_index];
            single(render_resource::resource_id::white_texture, 0, 0, white);
            single(render_resource::resource_id::scene_textures, 0, 0, white);
        }
        // The IBL triple, in the order set_ibl uploads it: prefiltered environment, irradiance, BRDF LUT.
        if (this->ibl_views.size() >= 3 && this->ibl_images.size() >= 3) {
            pass::resolved_binding env = image(this->ibl_images[0]);
            env.view = *this->ibl_views[0];
            pass::resolved_binding irradiance = image(this->ibl_images[1]);
            irradiance.view = *this->ibl_views[1];
            pass::resolved_binding lut = image(this->ibl_images[2]);
            lut.view = *this->ibl_views[2];
            single(render_resource::resource_id::ibl_env, 0, 0, env);
            single(render_resource::resource_id::ibl_irradiance, 0, 0, irradiance);
            single(render_resource::resource_id::brdf_lut, 0, 0, lut);
        }
        // NOT published: `top_level_structure`. It is an acceleration structure, and `resolved_binding` carries
        // the three handles a set write and a barrier take - a device address is neither. Whoever needs it asks
        // the runtime for it today (see the scene block's binding 16), and a table entry that could not carry the
        // handle would be a claim this type cannot make.
    }

    void runtime::verify_resource_table(pass::frame_pass const& pass, pass::resolved_io const& io) {
        render_resource::pass_io const& decl = pass.io();
        pass::resource_table const& table = this->frame_resources;
        uint32_t checked = 0;
        uint32_t mismatched = 0;
        auto const as_pointer = [](auto const handle) { return static_cast<void const*>(handle); };
        auto const check = [&](render_resource::resource_id const id, uint32_t const element, pass::resolved_binding const& resolved, std::string_view const channel) {
            render_resource::resource_info const* const info = render_resource::find(id);
            if (info == nullptr) {
                return;
            }
            ++checked;
            pass::resolved_binding const published = table.find(id, element, pass::instance_for(info->scope, io.frame));
            if (published.view == resolved.view && published.buffer == resolved.buffer && published.image == resolved.image) {
                return;
            }
            ++mismatched;
            // Bounded, and only while the check's window is open: a wrong entry is a finding to fix, not a reason
            // to fill the log of every frame.
            if (this->resource_check_frames < 3 && this->resource_check_mismatched + mismatched <= 10) {
                deren::utility::log("resource table: pass '{}' {} (resource {}, element {}) resolved as [view {}, buffer {}, image {}] but published as [view {}, buffer {}, image {}]",
                                    decl.name, channel, static_cast<int32_t>(id), element,
                                    as_pointer(resolved.view), as_pointer(resolved.buffer), as_pointer(resolved.image),
                                    as_pointer(published.view), as_pointer(published.buffer), as_pointer(published.image));
            }
        };
        // The own bindings, indexed by their own binding number: the validator requires those to be contiguous
        // from zero, so `own[binding]` IS this binding's handle (see resolved_io).
        for (render_resource::pass_binding const& binding : decl.bindings) {
            if (binding.owner != render_resource::binding_owner::own || binding.binding >= io.own.size()) {
                continue;
            }
            check(binding.resource, binding.element, io.own[binding.binding], "own binding");
        }
        // The targets, walked the way `resolve_declaration` walks them: a target claiming a RUN of elements
        // (`render_target::count`) expands to one slot per element, so the declaration's t-th entry is NOT always
        // the t-th slot in `io.targets` - the shadow pass's four cascade layers are one entry there and four here.
        uint32_t target_slot = 0;
        for (render_resource::render_target const& target : decl.targets) {
            for (uint16_t i = 0; i < target.count && target_slot < io.targets.size(); ++i, ++target_slot) {
                check(target.resource, static_cast<uint32_t>(target.element) + i, io.targets[target_slot], "render target");
            }
        }
        for (std::size_t i = 0; i < decl.barrier_images.size() && i < io.barrier_images.size(); ++i) {
            check(decl.barrier_images[i].resource, decl.barrier_images[i].element, io.barrier_images[i], "barrier image");
        }
        for (std::size_t i = 0; i < decl.barrier_buffers.size() && i < io.barrier_buffers.size(); ++i) {
            check(decl.barrier_buffers[i].resource, decl.barrier_buffers[i].element, io.barrier_buffers[i], "barrier buffer");
        }
        this->resource_check_checked += checked;
        this->resource_check_mismatched += mismatched;
        // One line per pass, inside the same window: it says WHICH passes the running scenario put under the
        // check, which is what tells a reader which resolvers the table has been proven against and which ones a
        // different scenario has to exercise.
        if (this->resource_check_frames < 3 && checked > 0 &&
            std::find(this->resource_check_passes.begin(), this->resource_check_passes.end(), &pass) == this->resource_check_passes.end()) {
            this->resource_check_passes.push_back(&pass);
            deren::utility::log("resource table: verified {} declaration handle(s) for pass '{}'", checked, decl.name);
        }
    }

    /// the scene pass's per-frame input: the leaves, the segments, and the three things only the renderer can
    /// answer (see scene_frame). Built here rather than stored, because every field is this frame's.
    pass::scene_frame runtime::make_scene_frame() noexcept {
        core const& vk = this->vulkan_core;
        uint32_t const frame_slot = static_cast<uint32_t>(vk.current_frame);
        // the pass's view of the per-slot secondary buffers (members, so the span it holds outlives the stage)
        auto const& segments = this->main_segments[static_cast<std::size_t>(frame_slot)];
        this->scene_segment_view.clear();
        this->scene_segment_view.reserve(segments.size());
        for (auto const& [pool, buffer] : segments) {
            this->scene_segment_view.push_back(pass::segment_buffer{.pool = pool, .buffer = *buffer});
        }
        // the secondaries inherit the instance's attachments: the three surface targets in order, the velocity
        // target, and the scene colour - the same order the pass's declaration lists them in
        this->scene_color_formats = {deren::vulkan::gbuffer_formats[0], deren::vulkan::gbuffer_formats[1], deren::vulkan::gbuffer_formats[2], deren::vulkan::gbuffer_velocity_format, deren::vulkan::hdr_format};
        return pass::scene_frame{
            .leaves = this->frame_visible,
            .segments = this->scene_segment_view,
            .make_environment = &runtime::make_scene_environment,
            .run_tasks = &runtime::run_scene_tasks,
            .owner = this,
            .fill_heap_bind = vk.descriptor_heaps.ready() ? &runtime::fill_heap_bind : nullptr,
            .color_formats = this->scene_color_formats,
            .depth_format = vk.depth_attachment_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .gbuffer = true,
            .extent = vk.render_extent(),
        };
    }

    /// build ONE segment's draw state; a fresh one per segment, because the pipeline-dedup state is per
    /// recording session (see scene_frame::make_environment)
    render_environment runtime::make_scene_environment(void* owner, VkCommandBuffer const command_buffer, bool const gbuffer) {
        runtime& self = *static_cast<runtime*>(owner);
        render_environment env;
        env.command_buffer = command_buffer;
        // The G-buffer pass binds its own pipeline as the pass default (see gbuffer_pipeline_name): same
        // leaves, same draw path, but the fragment stage writes the surface instead of shading.
        // ... AND WHICH FORM OF IT: the MESH pipeline when the device built one (docs/mesh_shaders.md step 2). The
        // decision is the SESSION's rather than the draw's, because the G-buffer pass draws every leaf with this
        // one pipeline - and a mesh session feeds its leaves differently (see the endpoints below), which is why
        // the two facts travel together here.
        bool gbuffer_mesh = false;
        bool gbuffer_meshlet = false;
        {
            std::shared_lock const lock(self.access_mutex);
            env.default_name = gbuffer ? gbuffer_pipeline_name : self.default_pipeline_name;
            gbuffer_mesh = gbuffer && self.gbuffer_pipeline_mesh.has_value();
            gbuffer_meshlet = gbuffer && self.gbuffer_pipeline_meshlet.has_value();
        }
        env.bind = [&self, &env, gbuffer, gbuffer_mesh, gbuffer_meshlet](VkCommandBuffer const cb, std::string_view const name) {
            if (gbuffer) {
                if (name == gbuffer_pipeline_name) {
                    if (gbuffer_meshlet) {
                        // ONE WORKGROUP PER MESHLET, each culled against the camera before it emits anything
                        // (docs/mesh_shaders.md step 3): the lanes carry the primitive's meshlet run, which is
                        // why the session flag travels with the bind rather than being decided up front.
                        self.gbuffer_pipeline_meshlet->begin_pipeline(cb);
                        env.mesh_stage = true;
                        env.meshlets = true;
                    } else if (gbuffer_mesh) {
                        self.gbuffer_pipeline_mesh->begin_pipeline(cb);
                        env.mesh_stage = true;
                        env.meshlets = false;
                    } else {
                        self.gbuffer_pipeline->begin_pipeline(cb);
                        env.mesh_stage = false;
                        env.meshlets = false;
                    }
                    return;
                }
                // a leaf with explicit pipeline semantics cannot draw in the G-buffer instance (the named
                // pipelines declare the single HDR attachment): say so once per leaf instead of issuing a draw
                // that would be a validation error
                deren::utility::log("runtime: leaf requests pipeline '{}' during the G-buffer pass - draw skipped (only default-semantics leaves write the G-buffer)", name);
                return;
            }
            // THE FORWARD/TRANSPARENT SESSIONS BIND BY NAME, so WHICH FORM was bound is decided HERE rather than per
            // session: a leaf that names a pipeline with a MESHLET form gets one workgroup per meshlet (culled
            // against the camera), failing that a MESH form, and failing that the vertex path - and the session's
            // flags follow the bind, which is what makes a session that mixes the forms correct rather than merely
            // likely.
            if (auto const meshlet = self.meshlet_pipelines.find(name); meshlet != self.meshlet_pipelines.end()) {
                meshlet->second.begin_pipeline(cb);
                env.mesh_stage = true;
                env.meshlets = true;
                static std::array<std::string_view, 8> logged_meshlet = {};
                static std::size_t logged_meshlet_count = 0;
                bool seen_meshlet = false;
                for (std::size_t i = 0; i < logged_meshlet_count; ++i) {
                    seen_meshlet = seen_meshlet || logged_meshlet[i] == name;
                }
                if (!seen_meshlet && logged_meshlet_count < logged_meshlet.size()) {
                    logged_meshlet[logged_meshlet_count++] = name;
                    deren::utility::log("scene: the leaves of '{}' are DISPATCHED per meshlet (meshlet stage)", name);
                }
                return;
            }
            if (auto const mesh = self.mesh_pipelines.find(name); mesh != self.mesh_pipelines.end()) {
                mesh->second.begin_pipeline(cb);
                env.mesh_stage = true;
                // the meshlet form is the answer above; a session that lands here draws one workgroup per 85
                // triangles, so the flag that says "the lanes carry a meshlet run" has to be cleared, not left over
                // from a previous bind in the same session
                env.meshlets = false;
                // ONE LINE PER NAME, SAYING THE LEAVES ARE DISPATCHED - the same evidence rule the shadow pass's
                // session follows: a mesh stage produces the same picture as the vertex one by construction, so
                // which path drew a frame is deliberately invisible in it (see docs/mesh_shaders.md).
                static std::array<std::string_view, 8> logged = {};
                static std::size_t logged_count = 0;
                bool seen = false;
                for (std::size_t i = 0; i < logged_count; ++i) {
                    seen = seen || logged[i] == name;
                }
                if (!seen && logged_count < logged.size()) {
                    logged[logged_count++] = name;
                    deren::utility::log("scene: the leaves of '{}' are DISPATCHED (mesh stage)", name);
                }
                return;
            }
            // A NAME THAT IS NEITHER A MESHLET NOR A MESH FORM IS NOT A PIPELINE ANY MORE (docs/mesh_shaders.md
            // step 4): every geometry name is dispatched, so there is no vertex form left to bind, and the only
            // thing to do with an unknown name is to say so once rather than record a draw with no pipeline.
            env.mesh_stage = false;
            env.meshlets = false;
            if (auto const it = self.pipelines.find(name); it != self.pipelines.end()) {
                it->second.begin_pipeline(cb); // a non-geometry named pipeline (none today: kept for the registry's sake)
            } else {
                deren::utility::log("runtime: main pass references unknown pipeline '{}' - draw skipped", name);
            }
        };

        // transparent leaves toggle depth writes off via this (core dynamic state, 1.3)
        env.set_depth_write_fn = [](VkCommandBuffer const cb, VkBool32 const enabled) { vkCmdSetDepthWriteEnable(cb, enabled); };
        // single-sided materials keep back-face culling here (the shadow pass overrides it with env.two_sided;
        // the main pass must not, or double-sided handling would cost fill rate)
        env.set_cull_mode_fn = [](VkCommandBuffer const cb, VkCullModeFlags const mode) { vkCmdSetCullMode(cb, mode); };
        // THE HEAP PUSH (see render_environment::push_block): every draw in this session sends its block through
        // the same endpoint a converted pass uses; that endpoint is what appends the two heap indices, which is
        // why a primitive's own push struct never had to grow a field.
        env.push_owner = &self;
        env.push_block = &runtime::push_stage_block;
        // ... and how a session feeds its leaves: a mesh pipeline has no input assembler, so every leaf is dispatched
        // with its geometry window pushed instead of bound (see primitive::mesh_dispatch). The WINDOW itself is
        // pushed by every session - a vertex pipeline's stages declare the same lanes and a heap pipeline requires
        // them written - so `mesh_stage` is the only per-session flag, and the bind callback above sets it per draw.
        env.mesh_geometry_push_offset = deren::vulkan::mesh_geometry_push_offset_scene;
        env.buffer_address = &runtime::mesh_buffer_address;
        env.push_at = &runtime::push_geometry_block;
        env.draw_mesh_tasks = &runtime::draw_mesh_tasks;                   // only ever called when mesh_stage is true
        env.draw_mesh_tasks_indirect = &runtime::draw_mesh_tasks_indirect; // a meshlet session's dispatch (see above)
        // ---- ... AND WHERE THE CULLING HAPPENS (docs/mesh_shaders.md step 3, the culling's cheapest stage): the
        //      CAMERA's sessions are culled by the HOST while they are recorded - it already holds the primitive's
        //      meshlets, the draw's model matrix and the camera - so the dispatch asks for the survivors only and a
        //      rejected meshlet costs no workgroup launch at all. The shadow pass deliberately does NOT get this: its
        //      frustum is per cascade, and it dispatches its casters once per cascade.
        env.meshlet_culled = true;
        env.meshlet_view_proj = &runtime::meshlet_view_proj;
        env.meshlet_culled_write = &runtime::meshlet_culled_write;
        env.mesh_stage = gbuffer_mesh;
        return env;
    }

    /// the renderer's scheduler, handed to the pass so the segment fan-out stays the frame loop's policy
    void runtime::run_scene_tasks(void* owner, std::span<std::function<void()>> const tasks) {
        static_cast<runtime*>(owner)->run_tasks(tasks, deren::vulkan::task_priority::recording);
    }

    // THE SCENE PASS'S RESOLVER IS GONE (S3): its six targets, its shared set and its extent are resolved from
    // its own declaration against the frame's resource table (see pass::resolve_declaration), and the gate that
    // used to be the first line here - "there is no surface pipeline, or no target generation to draw into" - is
    // now the pass's FEATURE ("scene", answered by `gbuffer_pass_active()` in `feature_active`). The generation
    // half of it is what the table already says: a frame whose targets do not exist has no entry to resolve, so
    // the pass does not run. What is NOT resolved here anymore is `out.own = {}` and `out.push = {}` either -
    // this pass declares neither, and the generic resolver leaves both empty for exactly that case.

    pass::transparent_frame runtime::make_transparent_frame() noexcept {
        core const& vk = this->vulkan_core;
        auto const& secondaries = this->secondary_command_buffers[static_cast<std::size_t>(vk.current_frame)];
        return pass::transparent_frame{
            .leaves = this->frame_transparent,
            .secondary = *secondaries[static_cast<std::size_t>(secondary_pass::transparent)],
            .make_environment = &runtime::make_scene_environment,
            .owner = this,
            .fill_heap_bind = vk.descriptor_heaps.ready() ? &runtime::fill_heap_bind : nullptr,
            .color_format = deren::vulkan::hdr_format,
            .depth_format = vk.depth_attachment_format,
            .extent = vk.render_extent(),
        };
    }

    // THE TRANSPARENT PASS'S RESOLVER IS GONE TOO (S3), for the same reason as the scene pass's: two declared
    // targets, the shared scene block and a full-frame extent, all of them resolved from the declaration. Its gate
    // - "nothing blended this frame" - is the pass's FEATURE ("transparent", answered by `frame_transparent`
    // being empty in `feature_active`), which is the frame's content rather than the declaration's shape.

    pass::character_forward_frame runtime::make_character_forward_frame() noexcept {
        core const& vk = this->vulkan_core;
        // DOES THE OVERLAY PIPELINE EXIST? Asked HERE rather than left to the session's bind callback, and the
        // difference is a log line per frame rather than a nicety: a name that resolves to nothing is reported by
        // that callback as "unknown pipeline", which is right for a leaf that asked for one and wrong for a pass
        // that is about to discover a build failure it should simply not draw through. So the frame hands over an
        // EMPTY name when the pipeline is absent, and the pass reads that as "draw no overlay" - the documented
        // contract on `character_forward_frame::overlay_pipeline_name`.
        bool overlay_ready = false;
        {
            std::shared_lock const lock(this->access_mutex);
            overlay_ready = this->mesh_pipelines.contains(overlay_pipeline_name) || this->meshlet_pipelines.contains(overlay_pipeline_name);
        }
        // ---- WHICH SHADING MODEL THIS FRAME DRAWS WITH (see runtime::set_goo_toon) ----
        //
        // THE ONE PLACE THE TWO NAMES ARE CHOSEN BETWEEN, and it is here rather than in the pass for the reason
        // the name itself is handed over at all: the registry is the runtime's (see the note below). The choice
        // is made on TWO facts rather than one - the knob AND whether that pipeline exists - because a name that
        // was never registered would be reported by the session's bind callback as an unknown pipeline, once per
        // frame; falling back to the old name instead draws the old chain, which is a valid frame whose only
        // defect is that it is not the experiment that was asked for (and `set_goo_toon` says so, once, when the
        // knob is turned on).
        bool goo_toon_ready = false;
        {
            std::shared_lock const lock(this->access_mutex);
            goo_toon_ready = this->mesh_pipelines.contains(goo_toon_pipeline_name) || this->meshlet_pipelines.contains(goo_toon_pipeline_name);
        }
        std::string_view const toon_pipeline_name = (this->goo_toon_on && goo_toon_ready) ? goo_toon_pipeline_name : character_forward_pipeline_name;
        // DOES THE OUTLINE PIPELINE EXIST? Asked on exactly the same terms as the overlay one above, and for the
        // same reason: an empty name is the pass's documented "draw no outline", whereas a name that resolves to
        // nothing would be reported by the session's bind callback as "unknown pipeline" - a log line per frame
        // for a build failure the pass should simply not draw through.
        bool outline_ready = false;
        {
            std::shared_lock const lock(this->access_mutex);
            outline_ready = this->mesh_pipelines.contains(outline_pipeline_name) || this->meshlet_pipelines.contains(outline_pipeline_name);
        }
        // ---- AND THE OUTLINE LEAVES: THE PER-MATERIAL GATE, WHICH IS NOT THE MESH ----
        //
        // The article's ① 描边 is an inverted hull of a surface THIS PASS ALREADY DRAWS, so the list is a
        // SUBSET OF `frame_visible` and not a parallel list: a leaf whose material states an `_OutlineWidth`
        // is drawn once as the surface (the toon stage) and once as its hull (the outline group), and one that
        // states 0 is drawn only as the surface. `frame_visible` rather than `frame_leaves_buffer` because the hull's
        // outside-the-silhouette ring exists only where the surface it hugs was drawn and wrote depth - a hull
        // for a culled leaf would be an unoccluded shell over the whole frame (see `frame_outline`).
        //
        // THE WIDTH IS READ OFF THE PRIMITIVE (`primitive::outline_width`) rather than out of the material's
        // colour lane, because the lane lives in the GPU table (`core::heap_slots::toon_colours`) and the frame
        // cannot read it without a readback. That mirror is filled from the same `toon_inputs` the shader's lane
        // is built from, so this gate and the mesh stage's own `w > 0` cannot drift apart.
        //
        // THE ORDER IS `frame_visible`'S, for the same reason `frame_overlay` keeps the scene tree's: two hulls
        // that overlap must compose in the order the asset authored them.
        this->frame_outline.clear();
        for (primitive const* const leaf : this->frame_visible) {
            // ... AND NEVER THE FRAME'S STATIC SURROUND: an environment leaf is not drawn by this stage at
            // all (see `frame_character`), and a hull is confined to the outside of the surface this pass
            // just re-shaded - with no re-shaded surface under it a hull would be an unlit shell over the
            // background. The material gate already excludes it (`outline_width` is 0 for the ground and the
            // backdrop), and stating it here keeps the rule in one place rather than in the asset.
            if (!leaf->environment && leaf->outline_width > 0.0f) {
                this->frame_outline.push_back(leaf);
            }
        }
        // THE OPAQUE LEAVES, which are the SCENE pass's - this stage re-shades the surfaces the scene pass
        // already drew, so it must see exactly that list. A leaf the culling dropped has no lit pixel to
        // overwrite, and one it kept but this frame omitted would keep the deferred shading while its
        // neighbours were re-shaded.
        //
        // ... AND IT IS `frame_character` RATHER THAN `frame_visible`, WHICH IS THE ONE DIFFERENCE: the
        // frame's static surround (`[render] background_glb`) is an opaque scene leaf drawn by the scene pass
        // and is deliberately NOT re-shaded here - see `frame_character` for why the reference has no toon
        // material for a ground and a backdrop. Everything else in the list is literally `frame_visible`.
        return pass::character_forward_frame{
            .leaves = this->frame_character,
            // ... AND THE OVERLAY LEAVES, which the collection step has already taken out of `frame_visible`
            // (see `frame_overlay`): they are not surfaces to re-shade, they are the article's two masks to
            // multiply over what this pass just wrote.
            .overlay_leaves = this->frame_overlay,
            // ... AND THE OUTLINE LEAVES (`primitive::outline_width > 0`), which are the SAME leaves this pass
            // re-shades, drawn a second time as their inverted hulls. A subset of `leaves` rather than a list of
            // its own, because an outline is not a different surface - see `frame_outline` for why it follows the
            // CULL as well as the material.
            .outline_leaves = this->frame_outline,
            .make_environment = &runtime::make_scene_environment,
            .owner = this,
            // The name the pipeline was registered under (chores.cpp). Handed over rather than hardcoded in
            // the pass, because the registry is the runtime's: a pass asserting a string another module chose
            // would be a name with no owner.
            //
            // ... AND IT IS THE OLD NAME OR THE REWRITTEN CHAIN'S, decided above from `set_goo_toon` and the
            // registry: the pass draws whatever it is handed, so this one expression IS the whole of the
            // rewrite's opt-in on the host side.
            .pipeline_name = toon_pipeline_name,
            // ... and the overlay group's, on the same terms - empty when it was never built, which the pass
            // reads as "there is no overlay to draw" (see `overlay_ready` above).
            .overlay_pipeline_name = overlay_ready ? overlay_pipeline_name : std::string_view{},
            // ... and the outline group's pipeline name, on the overlay's terms exactly: empty when it was never
            // built, which the pass reads as "there is no outline to draw" (see `outline_ready` above).
            .outline_pipeline_name = outline_ready ? outline_pipeline_name : std::string_view{},
            .color_format = deren::vulkan::hdr_format, // one HDR target, and NOT the swapchain format - see the pass
            .depth_format = vk.depth_attachment_format,
            .extent = vk.render_extent(),
        };
    }

    // =============================================================================================
    // THE CHAIN OWNER'S SEAM (see runtime::frame_services)
    // =============================================================================================
    //
    // WHAT IS LEFT HERE IS THE THREE FRAMES THAT CARRY THIS RENDERER'S RECORDING MACHINERY, and that is the
    // whole reason to keep any of them: the scene's and the transparent's carry the per-slot secondary buffers,
    // the draw-state factory and the task-pool scheduling those record through, and the shadow's carry the
    // per-cascade secondaries the structure phase recorded. Every OTHER frame is the PASS's own now - the host
    // publishes the few values a pass cannot derive (see `make_frame_facts`) and the pass composes its frame
    // (see `frame_pass::prepare_frame`), which is why this file no longer has a builder per pass and no longer
    // imports the modules of the passes whose frames it stopped naming.

    pass::shadow_frame runtime::make_shadow_frame() noexcept {
        // The per-cascade secondaries live in per-slot pairs (a command pool is not thread safe), so they are NOT
        // contiguous: they are gathered into the scratch array the returned span points at, and that array is a
        // member because a span over a local would dangle the moment this function returned.
        uint32_t const slot = static_cast<uint32_t>(this->vulkan_core.current_frame);
        uint32_t const cascades = std::clamp(this->shadow_cascades, 1u, deren::vulkan::max_shadow_cascades);
        for (uint32_t cascade = 0; cascade < cascades; ++cascade) {
            this->shadow_secondaries_scratch[cascade] = *this->shadow_recording[slot][cascade].second;
        }
        return pass::shadow_frame{.record_cascade = &runtime::record_shadow_cascade,
                                  .run_tasks = &runtime::run_shadow_tasks,
                                  .owner = this,
                                  .cascades = std::span<VkCommandBuffer const>(this->shadow_secondaries_scratch.data(), cascades),
                                  .map_size = this->shadow_map_size};
    }

    pass::draw_callback runtime::overlay_draw() const noexcept {
        // The trampoline takes the owner as an ARGUMENT rather than using `this`, so the address is a plain
        // function pointer and the hook is one value (see pass::draw_callback).
        return pass::draw_callback{.record = &runtime::draw_overlay_after, .owner = const_cast<runtime*>(this)};
    }

    pass::frame_facts runtime::make_frame_facts() const noexcept {
        // EVERY FIELD IS THE EXPRESSION THE BUILDER IT REPLACED USED - see frame_facts' own note, which names the
        // two where a similarly named feature fact would be a DIFFERENT value (`gi_specular` and `debug_view` are
        // composed, the feature facts of those names are the raw knobs).
        std::size_t const index = this->current_image_index;
        return pass::frame_facts{
            .megalights = this->megalights_active(),
            .megalights_resolved = this->megalights_resolved,
            .megalights_history_valid = index < this->megalights_history_valid.size() && this->megalights_history_valid[index],
            .fxaa_resolves = this->post_fxaa_active(),
            .upscale_resolves = this->post_upscale_active(),
            .debug_view = this->active_features().gbuffer_debug,
            .cluster_count = this->cluster_tiles_x * this->cluster_tiles_y * deren::vulkan::cluster_slice_count,
        };
    }

    runtime::frame_services runtime::make_frame_services(VkCommandBuffer const command_buffer) noexcept {
        // Capture-less lambdas: each one casts the owner back and calls the builder it names, so the owner never
        // sees a member function of this class - it sees the frame. THREE builders, not eleven: the rest of the
        // frames are composed by their own passes from the published facts (see frame_pass::prepare_frame).
        return frame_services{
            .owner = this,
            .cmd = command_buffer,
            .image_index = static_cast<uint32_t>(this->current_image_index),
            .device = this->vulkan_core.logical_device,
            .samplers = this->shared_samplers(),
            .table = &this->frame_resources,
            .frame = this->pass_frame(),
            .constants = &this->frame_facts,
            .make_shadow_frame = [](void* owner) { return static_cast<runtime*>(owner)->make_shadow_frame(); },
            .make_scene_frame = [](void* owner) { return static_cast<runtime*>(owner)->make_scene_frame(); },
            .make_transparent_frame = [](void* owner) { return static_cast<runtime*>(owner)->make_transparent_frame(); },
            .make_character_forward_frame = [](void* owner) { return static_cast<runtime*>(owner)->make_character_forward_frame(); },
            .ensure_gbuffer_targets_sampled =
                [](void* owner, VkCommandBuffer cmd, uint32_t image) { return static_cast<runtime*>(owner)->ensure_gbuffer_targets_sampled(cmd, image); },
            .ensure_gbuffer_depth_sampled =
                [](void* owner, VkCommandBuffer cmd, uint32_t image) { return static_cast<runtime*>(owner)->ensure_gbuffer_depth_sampled(cmd, image); },
            .ensure_velocity_sampled = [](void* owner, VkCommandBuffer cmd, uint32_t image) { return static_cast<runtime*>(owner)->ensure_velocity_sampled(cmd, image); },
            .require_velocity_publish = [](void* owner, uint32_t image) { static_cast<runtime*>(owner)->require_velocity_publish(image); },
            .feature_active = [](void* owner, std::string_view name) { return static_cast<runtime const*>(owner)->feature_active(name); },
        };
    }

    void runtime::bind_frame_chain(pass::pass_chain& chain) noexcept {
        // THE FRAME'S STRUCTURE, and the only thing in this renderer that still names individual passes: which stage
        // holds which pass IS the frame loop's order (it is what the marks, the per-stage preambles and the
        // renderer's own work between the stages are written against), so it is filled here BY DECLARATION NAME out
        // of whichever chain the application handed over. A name the chain does not declare leaves that stage empty,
        // which the runner treats as "no pass here" rather than as an error.
        this->frame_chain = &chain;
        auto const at = [&chain](std::string_view const name) -> pass::frame_pass* { return chain.find(name); };
        this->cluster_pass = {at("cluster")};
        this->shadow_pass = {at("shadow")};
        this->scene_pass = {at("scene")};
        this->transparent_pass = {at("transparent")};
        this->character_forward_pass = {at("character_forward")};
        this->toon_screen_rim_pass = {at("toon_screen_rim")};
        this->goo_rim_pass = {at("goo_rim")};
        this->gbuffer_debug_stage = {at("gbuffer-debug")};
        this->rt_shadow_pass = {at("rt_shadow")};
        this->megalights_pass = {at("megalights_trace"), at("megalights_temporal")};
        this->deferred_pass = {at("deferred")};
        this->taa_pass = {at("taa")};
        this->post_composite_stage = {at("post_composite")};
        this->bloom_pass = {at("post_bloom_0"), at("post_bloom_1"), at("post_bloom_2"), at("post_bloom_3")};
        this->fxaa_pass = {at("fxaa")};
        // ... and the resolve that scales the render chain back up to the output (deren.vulkan.pass.upscale): the
        // frame's last writer whenever `render_scale < 1.0` and its pipeline exists. It is looked up like every
        // other stage here; the frame loop records it after the FXAA stage, with which it is mutually exclusive.
        this->upscale_pass = {at("upscale")};
        // ... and the stochastic lighting chain's two passes, in the order the FRAME records them: the tracer
        // and its temporal resolve. Two passes rather than one because the frame has an ordering rule to
        // run BETWEEN them - it publishes the G-buffer depth and the motion-vector target that the resolve is the
        // first sampler of - and a frame rule cannot run from inside a chain.
    }

    void runtime::set_pass_chain(pass::pass_chain& chain, chain_wiring const wiring) noexcept {
        // THE HANDOVER: the owner's chain, bound into the frame loop's own structure, plus the wiring that supplies
        // everything the renderer does not know about those passes (see chain_wiring).
        this->bind_frame_chain(chain);
        this->set_chain_wiring(wiring);
    }

    void runtime::set_chain_wiring(chain_wiring const wiring) noexcept {
        this->frame_wiring = wiring;
    }

    void runtime::prepare_stage(pass::stage const& stage, VkCommandBuffer const command_buffer) {
        // THE PASSES BUILD THEIR OWN FRAMES FIRST, from the facts this renderer publishes for THIS STAGE:
        //     structure phase rebuilds DURING this frame (see make_frame_facts);
        //   * before the owner's `prepare`, so an owner that still wants to add to a frame (or override one)
        //     has the last word - the ordering the seam has always had.
        this->stage_facts = this->make_frame_facts();
        for (pass::frame_pass* const pass : stage.passes) {
            if (pass != nullptr) {
                pass->prepare_frame(this->stage_facts);
            }
        }
        if (this->frame_wiring.prepare == nullptr) {
            return; // no owner: the passes keep the frames they just built and record with them
        }
        this->frame_wiring.prepare(this->frame_wiring.owner, this->make_frame_services(command_buffer), stage.name);
    }

    void runtime::collect_stage(std::string_view const stage) {
        if (this->frame_wiring.collect == nullptr) {
            return;
        }
        frame_results results = {};
        this->frame_wiring.collect(this->frame_wiring.owner, stage, results);
        // WHAT THE FRAME LOOP DECIDES ON, once per stage that reports: the composite's GI weight is a frame
        // CONSTANT (the composite reads it while recording), and the two flags are the renderer's per-image
        // bookkeeping.
        if (results.taa_wrote_history && this->current_image_index < this->image_view_proj.size()) {
            this->image_view_proj[this->current_image_index] = this->current_ubo.view_proj_unjittered;
        }
    }

    void runtime::require_velocity_publish(uint32_t const image_index) {
        if (image_index < this->velocity_written.size()) {
            this->velocity_written[image_index] = false;
        }
    }

    // `runtime::pass_extent` IS GONE (S3.12), and it is the payoff of the last resolver: it was the bridge from a
    // declaration to a size the renderer owns, and it existed because a hand-written resolver had to apply the
    // declaration's own `extent_rule` itself. The framework applies it now (`pass::resolve_extent`, over the
    // `extent_of` callback below), so the bridge had no callers left - the rule is in ONE place, which is what the
    // function's own comment argued for while it was the second copy.

    VkExtent2D runtime::resolve_resource_extent(render_resource::resource_id const id, uint32_t const element) const noexcept {
        core const& vk = this->vulkan_core;
        switch (id) {
        case pass::resource_id::bloom: {
            // A bloom level is HALF the previous one - max(1, render >> (level + 1)) - which is the SAME
            // formula `core::create_render_targets` created the images with, over the same base: the RENDER
            // extent, so the bloom chain follows a render scale down with the rest of the chain instead of
            // staying output-sized and sampling a target that no longer exists at that size. The clamp is
            // belt-and-braces rather than the contract: the schema's `count` (4) plus the validator's element
            // check is what limits the level, and a shift of 32 or more would be undefined behaviour if one
            // ever got through.
            uint32_t const shift = std::min<uint32_t>(element + 1u, 31u);
            return VkExtent2D{std::max(1u, vk.render_extent().width >> shift), std::max(1u, vk.render_extent().height >> shift)};
        }
        case pass::resource_id::swapchain_image:
            // THE ONE RESOURCE WHOSE EXTENT IS NOT THE FRAME'S. Every other entry here answers a size that is
            // part of the render chain, and the frame's own extent is what a `full` pass gets; this one is the
            // OUTPUT. A pass that declares `extent_rule::resource` over it is asking to cover the presented
            // image while the frame runs at the render extent - which is exactly what the resolve pass that
            // scales the render chain up to the swapchain has to do, and the reason its viewport has to come
            // from its own declaration rather than from the frame.
            return vk.swap_chain_extent;
        default:
            return VkExtent2D{}; // an element of a resource whose extent IS the frame's: nothing to answer
        }
    }

    pass::owned_pipeline runtime::resolve_pipeline(std::string_view const name) const noexcept {
        if (vk_pipeline const* const pipeline = this->get_pipeline(name); pipeline != nullptr) {
            return pass::owned_pipeline{.pipeline = pipeline->get_pipeline()};
        }
        // ... AND THEN THE CHAIN'S OWN PASSES, because a chain's stages may SHARE one pipeline: the post chain's
        // four bloom levels record with the composite's R16F variant, and a copy per level would be five identical
        // pipelines. Asking every pass by NAME is what keeps this chain-agnostic - the renderer does not know, and
        // does not need to know, which pass owns what; a pass answers for the names it publishes and nothing else.
        return {};
    }

    pass::resolve_context runtime::make_resolve_context() noexcept {
        return pass::resolve_context{
            .resources = &this->frame_resources,
            .frame = this->pass_frame(),
            .cmd = *this->command_buffers[static_cast<uint32_t>(this->vulkan_core.current_frame)],
            .extent_of = [](void* owner, render_resource::resource_id const id, uint32_t const element) { return static_cast<runtime*>(owner)->resolve_resource_extent(id, element); },
            .pipeline = [](void* owner, std::string_view const name) { return static_cast<runtime*>(owner)->resolve_pipeline(name); },
            .owner = this,
        };
    }

    pass::frame_identity runtime::pass_frame() const noexcept {
        core const& vk = this->vulkan_core;
        return pass::frame_identity{
            .image_index = this->current_image_index,
            .slot = static_cast<uint32_t>(vk.current_frame),
            // the generation's image count, which is what a pass that owns a per-image family sizes it from -
            // and NOT the same number as the image index above
            .image_count = static_cast<uint32_t>(vk.ml_images.size()),
            // THE FRAME'S EXTENT IS THE RENDER EXTENT - this one line is where "the frame's resolution" is
            // defined for every pass (`pass::resolve_extent` reads it for the `full` and `half` rules). It
            // equals the swapchain extent at the default render scale, and below it every `full` pass follows
            // the render chain down together, which is what keeps a lower internal resolution one decision
            // rather than a per-pass one. A pass that has to write the OUTPUT - the resolve that scales the
            // chain back up to what is presented - declares `extent_rule::resource` over
            // `resource_id::swapchain_image` instead, and `resolve_resource_extent` answers with the
            // swapchain's extent.
            .extent = vk.render_extent(),
        };
    }

    // ---- the pass host: the runner's callbacks, answered by the renderer ---------------------------------
    //
    // It is rebuilt per call because it is a struct of function pointers (it holds no state of its own); the
    // context is this runtime, which is what each callback casts back to. This is the RUNNER's half of the
    // interface and a pass never sees it: at create time a pass is given a `pass_context`, and while recording
    // it is handed `resolved_io`, so it cannot reach a resource its declaration did not name.
    pass::pass_host runtime::make_pass_host() noexcept {
        return pass::pass_host{
            // THE HEAP PUSH EVERY CONVERTED STAGE NEEDS (see resolved_io::push_block): a block sent through
            // vkCmdPushDataEXT, because no heap pipeline has a layout to hold push constants.
            .push_block = {.owner = this, .push = &runtime::push_stage_block},
            .context = this,
            .frame = [](void* context) { return static_cast<runtime*>(context)->pass_frame(); },
            .feature_active = [](void* context, std::string_view const feature) { return static_cast<runtime*>(context)->feature_active(feature); },
            .resolve = [](void* context, pass::frame_pass const& pass, pass::resolved_io& out) { return static_cast<runtime*>(context)->resolve_pass(pass, out); },
            .apply_behaviour = [](void* context, pass::frame_pass const& pass, pass::resolved_io const& io) { static_cast<runtime*>(context)->apply_pass_behaviour(pass, io); },
            // No mark pair for this stage, and deliberately: its cost is already accounted for by the marks around
            // the passes it records, so adding a query pair here would only change the report. The runtime keeps writing
            // the end mark itself, right after the stage.
            .mark_begin = nullptr,
            .mark_end = nullptr,
        };
    }

    bool runtime::resolve_pass(pass::frame_pass const& pass, pass::resolved_io& out) {
        // TWO THINGS WRAP THE FRAMEWORK'S RESOLUTION, and neither belongs there: this frame's shared constants,
        // which every pass is handed whether or not it reads them yet, and the differential check that kept the
        // resource table honest while the resolvers were still here (see verify_resource_table).
        //
        // THE PER-PASS SWITCH IS GONE (S3.12): every pass in this renderer is resolved by its own DECLARATION now
        // (`frame_pass::resolve`), from the resource table the frame publishes - own bindings, targets, barrier
        // entries, the shared sets it names, its own pipeline and the extent from its behaviour's rule. What is
        // left of the renderer's knowledge is the table's CONTENTS (`publish_frame_resources`) and the frame's
        // constants, which is exactly the split this migration was for: a pass cannot reach a resource its
        // declaration does not name, and the renderer no longer knows which pass wants which image.
        out.constants = this->frame_facts; // THE PER-IMAGE TARGET DESCRIPTORS ARE (RE)WRITTEN EVERY FRAME, and that is a MEASURED requirement
        // rather than belt-and-braces: a heap IMAGE descriptor written while its image is still in
        // VK_IMAGE_LAYOUT_UNDEFINED - which is exactly what the creation loops do, in the same breath as
        // vkCreateImage - NEVER RESOLVES. Re-writing the SAME descriptor once the image has been transitioned
        // into a sampled layout samples correctly (measured: the lighting pass read zero from every G-buffer
        // slot until this rewrite existed, and read the real albedo the moment it was added). BUFFERS ARE
        // UNAFFECTED, which is why the material table and the camera/light UBOs worked all along, and why the
        // IBL images worked too - they are uploaded and transitioned before their descriptors are written.
        auto const write_sampled_target = [this](uint32_t const slot, VkImage const image, VkFormat const format, VkImageAspectFlags const aspect) {
            if (image == VK_NULL_HANDLE) {
                return;
            }
            VkImageViewCreateInfo const view = make_image_view_info(image, format, VK_IMAGE_VIEW_TYPE_2D, aspect, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
            [[maybe_unused]] bool const written = this->vulkan_core.descriptor_heaps.write_image(core::heap_slot_offset(slot), view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        };
        std::size_t const heap_image = static_cast<std::size_t>(this->current_image_index);
        if (heap_image < this->vulkan_core.gbuffer_images[0].size()) {
            uint32_t const image_slot = static_cast<uint32_t>(heap_image);
            write_sampled_target(core::heap_slots::gbuffer_albedo + image_slot, this->vulkan_core.gbuffer_images[0][heap_image], deren::vulkan::gbuffer_formats[0], VK_IMAGE_ASPECT_COLOR_BIT);
            write_sampled_target(core::heap_slots::gbuffer_normal + image_slot, this->vulkan_core.gbuffer_images[1][heap_image], deren::vulkan::gbuffer_formats[1], VK_IMAGE_ASPECT_COLOR_BIT);
            write_sampled_target(core::heap_slots::gbuffer_material + image_slot, this->vulkan_core.gbuffer_images[2][heap_image], deren::vulkan::gbuffer_formats[2], VK_IMAGE_ASPECT_COLOR_BIT);
            write_sampled_target(core::heap_slots::gbuffer_depth + image_slot, this->vulkan_core.gbuffer_depth_images[heap_image], this->vulkan_core.depth_attachment_format, VK_IMAGE_ASPECT_DEPTH_BIT);
            write_sampled_target(core::heap_slots::gbuffer_velocity + image_slot, this->vulkan_core.velocity_images[heap_image], deren::vulkan::gbuffer_velocity_format, VK_IMAGE_ASPECT_COLOR_BIT);
            write_sampled_target(core::heap_slots::taa_current + image_slot, this->vulkan_core.scene_color_images[heap_image], deren::vulkan::hdr_format, VK_IMAGE_ASPECT_COLOR_BIT);
            write_sampled_target(core::heap_slots::post_color + image_slot, this->vulkan_core.hdr_images[heap_image], deren::vulkan::hdr_format, VK_IMAGE_ASPECT_COLOR_BIT);
            write_sampled_target(core::heap_slots::display_color + image_slot, this->vulkan_core.ldr_images[heap_image], deren::vulkan::hdr_format, VK_IMAGE_ASPECT_COLOR_BIT);
            // ... and the same for every OTHER per-image target a shader samples or writes: the temporal
            // history, the megalights chain (sampled AND its storage twin), and the four bloom levels, which
            // the grid packs `heap_image_capacity` apart.
            auto const write_storage_target = [this](uint32_t const slot, VkImage const image, VkFormat const format) {
                if (image == VK_NULL_HANDLE) {
                    return;
                }
                VkImageViewCreateInfo const view = make_image_view_info(image, format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
                [[maybe_unused]] bool const written = this->vulkan_core.descriptor_heaps.write_image(core::heap_slot_offset(slot), view, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            };
            if (heap_image < this->vulkan_core.taa_history_images.size()) {
                write_sampled_target(core::heap_slots::taa_history + image_slot, this->vulkan_core.taa_history_images[heap_image], deren::vulkan::hdr_format, VK_IMAGE_ASPECT_COLOR_BIT);
            }
            if (heap_image < this->vulkan_core.ml_images.size()) {
                write_sampled_target(core::heap_slots::ml_trace + image_slot, this->vulkan_core.ml_images[heap_image], deren::vulkan::hdr_format, VK_IMAGE_ASPECT_COLOR_BIT);
                write_storage_target(core::heap_slots::ml_trace_storage + image_slot, this->vulkan_core.ml_images[heap_image], deren::vulkan::hdr_format);
            }
            if (heap_image < this->vulkan_core.ml_resolve_images.size()) {
                write_sampled_target(core::heap_slots::ml_resolved + image_slot, this->vulkan_core.ml_resolve_images[heap_image], deren::vulkan::hdr_format, VK_IMAGE_ASPECT_COLOR_BIT);
                write_storage_target(core::heap_slots::ml_resolved_storage + image_slot, this->vulkan_core.ml_resolve_images[heap_image], deren::vulkan::hdr_format);
            }
            if (heap_image < this->vulkan_core.ml_history_images.size()) {
                write_sampled_target(core::heap_slots::ml_history + image_slot, this->vulkan_core.ml_history_images[heap_image], deren::vulkan::hdr_format, VK_IMAGE_ASPECT_COLOR_BIT);
            }
            for (uint32_t level = 0; level < this->vulkan_core.bloom_images.size(); ++level) {
                if (heap_image < this->vulkan_core.bloom_images[level].size()) {
                    write_sampled_target(core::heap_slots::bloom_l0 + level * core::heap_image_capacity + image_slot, this->vulkan_core.bloom_images[level][heap_image], deren::vulkan::hdr_format, VK_IMAGE_ASPECT_COLOR_BIT);
                }
            }
        }

        bool const resolved = pass.resolve(this->make_resolve_context(), out);

        if (resolved) {
            this->verify_resource_table(pass, out);
        }
        return resolved;
    }

    // THE TAA RESOLVE'S RESOLVER IS GONE (S3), and it is the first pass whose PARAMETERS moved with it: its
    // four own bindings and its HDR target come from the frame's resource table, its pipeline from the pass (it
    // builds its own), its extent from the declaration's `full` rule, and its push block it composes itself out
    // of `resolved_io::constants` (the projection's two depth terms), `io.extent` (the texel size) and its own
    // two blend weights - which `set_taa` now forwards instead of caching them here. Its old gate is answered by
    // the table (the images), the pass's own pipeline (a null one fails the resolution) and the feature "taa".

    void runtime::apply_pass_behaviour(pass::frame_pass const& pass, pass::resolved_io const& io) {
        // The mechanical part of "how this pass is called", done by the runner so that a pass cannot forget
        // it: the pipeline is bound HERE, and the viewport/scissor are set HERE for a pass that asked for them
        // (which is what replaces the hand-kept pipeline list in update_pass_geometry - a pass cannot drop
        // itself from a list it does not maintain).
        //
        // What is deliberately NOT here: the rendering instance. EVERY graphics pass opens its own, over the
        // targets it declared, because the load op and the clear value are the PASS's knowledge. That is why
        // this function no longer has a "not a compute pass" branch: the three graphics kinds differ in how
        // many draws they issue, and only the pass knows that.
        pass::behaviour const& behaviour = pass.behaviour();
        if (behaviour.resync_viewport) {
            // io.extent is the extent the declaration's rule produced (the frame's, half of it, or a
            // resource's), so a fullscreen pass gets a viewport that matches the target it declared.
            VkViewport const viewport = {0.0f, 0.0f, static_cast<float>(io.extent.width), static_cast<float>(io.extent.height), 0.0f, 1.0f};
            VkRect2D const scissor = {{0, 0}, io.extent};
            vkCmdSetViewport(io.cmd, 0, 1, &viewport);
            vkCmdSetScissor(io.cmd, 0, 1, &scissor);
        }
        // THREE bind points, one per way a pass's work reaches the command buffer: a compute dispatch, a
        // traceRays launch (a ray-tracing pipeline bound to the compute point is invalid, so the pass's own
        // kind is what says which), and the graphics kinds, which all mean the same thing here.
        VkPipelineBindPoint const bind_point = behaviour.kind == pass::behaviour_kind::compute       ? VK_PIPELINE_BIND_POINT_COMPUTE
                                               : behaviour.kind == pass::behaviour_kind::ray_tracing ? VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR
                                                                                                     : VK_PIPELINE_BIND_POINT_GRAPHICS;
        for (VkPipeline const pipeline : io.pipelines) {
            if (pipeline != VK_NULL_HANDLE) {
                vkCmdBindPipeline(io.cmd, bind_point, pipeline);
            }
        }
    }

    bool runtime::pass_ready(std::string_view const name) const noexcept {
        // The chain's own answer, and it is deliberately the CHAIN's: the pass whose declaration carries that name,
        // asked whether it built what it records with (see frame_pass::ready). This is the first of the renderer's
        // questions moved into the declaration vocabulary - the direction the handover needs, because a renderer
        // handed a chain from outside holds no typed member to ask.
        return this->frame_chain != nullptr && this->frame_chain->ready(name);
    }

    void runtime::fill_compute_skin_requests(std::span<ray_tracing::caster_level const> const casters) {
        // The job's input, built from the map the STRUCTURE SET built: one request per SKINNED caster (a zero
        // destination means "not skinned: its geometry is what the build read"). The list is a member so a frame
        // does not allocate while recording, and the JOB decides what to do with it.
        this->compute_skin_requests.clear();
        this->compute_skin_requests.reserve(casters.size());
        for (auto const& built : casters) {
            if (built.skin_destination_address == 0) {
                continue;
            }
            this->compute_skin_requests.push_back(pass::compute_skin_request{
                .source_vertices = built.skin_source_address,
                .destination = built.skin_destination_address,
                .source_stride = built.skin_source_stride,
                .destination_stride = built.skin_destination_stride,
                .vertex_count = built.skin_vertex_count,
                .skin_base = built.skin_base,
            });
        }
    }

    bool runtime::record_compute_skin_pass(VkCommandBuffer const command_buffer, std::span<ray_tracing::caster_level const> const casters) {
        // The knob and the frame slot are the RENDERER's; the dispatches and the barrier the acceleration
        // structure build needs after them are the JOB's (vulkan.pass.compute_skin_job). "Is there anything
        // skinned at all" is NOT asked here any more: the structure set owns that answer (its map), and it only
        // calls this when it has skinned levels to refit.
        if (!this->rt_skin_bake || !this->compute_skin.ready()) {
            return false;
        }
        this->fill_compute_skin_requests(casters);
        return this->compute_skin.record(command_buffer, this->compute_skin_requests, this, &runtime::push_index_block);
    }

    // THE RAY-TRACED SHADOW PASS'S RESOLVER IS GONE (S3): its one barrier image comes from the frame's resource
    // table (per-frame-slot instance), its two shared sets from the owner, its pipeline from the PASS (which owns
    // it) and its push block is composed by the pass itself out of `resolved_io::constants` - the first pass for
    // which a push block crossed that line. The two halves of its old gate went where the scene pass's did:
    //
    //  * "this slot's visibility image exists" is the RESOURCE TABLE (no entry, no resolution);
    //  * "this slot's top level structure is built, and the pipeline exists" is the pass's FEATURE
    //    (`feature_active("rt_shadow")`), because an acceleration structure is not a `resolved_binding` - it has
    //    no view, no buffer and no image, only a device address - so it cannot be a table entry at all.
    //
    // THE ONE THING THAT DID NOT MOVE INTO THE PASS is the pair of `ensure_gbuffer_*_sampled` calls it used to
    // make: they are the frame's *ordering* rule ("whoever samples the G-buffer first publishes the G-buffer
    // instance's attachment writes"), and the flags they read are the renderer's. They now run in the rt_shadow
    // STAGE's preamble in `begin_recording`, immediately before the stage records - which is the same position in
    // the command stream, because the stages carry no marks of their own (`make_pass_host` leaves the mark pair
    // null), so nothing is emitted between them.

    void runtime::record_scene_tail(VkCommandBuffer const command_buffer) {
        // NO vkCmdEndRendering HERE ANY MORE: the scene PASS owns its instance and closes it at the end of its
        // own record (see deren.vulkan.pass.scene). That line used to be the far half of a pair whose near half was
        // three functions away - the coupling this extraction removed.
        // GPU timing: the geometry instance ended where the scene pass closed it (the surface write).
        this->gpu_mark(command_buffer, gpu_mark_id::scene_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        core const& vk = this->vulkan_core;

        // Deferred mode: the surface is in the G-buffer and the sky + emissive are in the scene color
        // target; this stage shades every pixel from the G-buffer and adds the result on top, and the
        // alpha-blended leaves then composite over the shaded image (their own instance - see
        // record_transparent_pass).
        if (this->deferred_lit_active()) {
            // Ray-traced sun shadows run HERE: after the G-buffer pass (whose depth and normal the rays
            // start from) and before the lighting stage (which multiplies the sun term by the result).
            // Running it before the G-buffer pass would mean starting rays from the PREVIOUS frame's
            // surface, so the position is not a detail - it is the ordering constraint. The PASS owns the
            // recording (deren.vulkan.pass.ray_traced_shadow); what is this loop's is the position and the off path below.
            pass::stage const rt_shadow_stage = {.name = "rt_shadow", .passes = this->rt_shadow_pass, .marks = false};
            // THIS STAGE'S ONE FRAME-ORDER DUTY, done by the chain's OWNER now that the passes are its: this stage
            // may be the first sampler of the stored surface this frame, and whoever samples it FIRST publishes the
            // G-buffer instance's attachment writes (the flags are the renderer's, and the idempotent `ensure_*`
            // pair is what makes "first" a fact rather than a promise) - so the owner's `prepare` asks for exactly
            // that, gated on the same feature the runner gates the stage on. Nothing is emitted between here and
            // `record_stage` (the stages carry no marks), so the command stream is unchanged.
            this->prepare_stage(rt_shadow_stage, command_buffer);
            pass::run_report const rt_shadow_report = pass::record_stage(rt_shadow_stage, this->make_pass_host());
            if (rt_shadow_report.recorded == 0 && static_cast<std::size_t>(vk.current_frame) < vk.rt_shadow_images.size() &&
                vk.rt_shadow_images[vk.current_frame] != VK_NULL_HANDLE) {
                // The pass did not run, but the lighting stage's descriptor still declares the image as
                // a shader input: its shader samples the binding only under a flag, and Vulkan requires
                // a statically-used binding's image to be in the layout the descriptor declares whether
                // or not the value is used. UNDEFINED as the old layout asserts nothing - the same
                // answer the GI image's off path gives.
                VkImageMemoryBarrier2 to_sampling = deren::vulkan::undefined_to_sampling_transition;
                to_sampling.image = vk.rt_shadow_images[vk.current_frame];
                VkDependencyInfo const sampling_dependency = make_image_dependency_info(1, &to_sampling);
                vkCmdPipelineBarrier2(command_buffer, &sampling_dependency);
            }
            // GPU timing: the ray-traced shadow pass ends here (before the lighting stage reads its
            // output). It gets its own interval because it sits between the G-buffer pass and the
            // lighting stage - without it the traversals were reported as lighting time, which made the
            // lighting interval look four times more expensive with rays on (measured 0.32 -> 1.18 ms
            // while the rays themselves were ~0.85 of that).
            this->gpu_mark(command_buffer, gpu_mark_id::rt_shadow_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            // ---- the stochastic punctual lighting stage ----
            // ITS POSITION IS THE WHOLE OF ITS CONTRACT, and it is the same constraint the ray-traced shadow
            // stage above states: AFTER the G-buffer pass, whose stored surface is what the estimator evaluates
            // its sampled lights against and which it traces its rays from, and BEFORE the lighting stage, which
            // is what ADDS the result to the frame (the stochastic estimate is a lighting term, not a screen
            // effect, so it belongs in the lit scene target with the rest of the lighting).
            //
            // THE LIGHTING STAGE ACTS ON WHETHER THIS RECORDED, not on whether the feature is on: the run
            // report is the answer pushed into the frame facts the lighting stage then reads, so a frame whose
            // pass was gated off keeps the raster punctual loop instead of losing its lights. That is the same
            // "one predicate, two readers" arrangement, with the difference that here the predicate is a
            // RECORDED FACT rather than a knob.
            this->megalights_resolved = false;
            if (this->megalights_active()) {
                pass::stage const megalights_stage = {.name = "megalights", .passes = this->megalights_pass, .marks = false};
                this->prepare_stage(megalights_stage, command_buffer);
                pass::run_report const megalights_report = pass::record_stage(megalights_stage, this->make_pass_host());
                this->megalights_resolved = megalights_report.recorded > 0;

                // ... and the stage's own answer for the NEXT frame (this image's history flag), which is what

                // makes the accumulation grow: without this call the resolve would restart at one frame forever.

                this->collect_stage("megalights");
            }
            if (!this->megalights_resolved && static_cast<std::size_t>(this->current_image_index) < vk.ml_images.size() && vk.ml_images[this->current_image_index] != VK_NULL_HANDLE) {
                // Nothing wrote the stochastic lighting image this frame, but the lighting stage's descriptor set
                // still declares it as a shader input (binding 17) and its shader uses that binding - under a flag,
                // but Vulkan requires a statically-used binding's descriptor to be in the layout the write declared
                // whether or not the value ends up mattering. Nothing else touches the image here, so it would sit
                // in UNDEFINED and every frame would be a layout error (measured: the gate's FIRST run of this
                // change failed exactly so). UNDEFINED as the old layout asserts nothing - it discards contents
                // rather than claiming a layout - so the transition is valid whether the image is untouched or
                // already readable. The same answer the GI image's and the shadow map's spare layers' off paths give.
                VkImageMemoryBarrier2 to_sampling = deren::vulkan::undefined_to_sampling_transition;
                to_sampling.image = vk.ml_resolve_images[this->current_image_index];
                VkDependencyInfo const sampling_dependency = make_image_dependency_info(1, &to_sampling);
                vkCmdPipelineBarrier2(command_buffer, &sampling_dependency);
            }
            // THE LIGHTING STAGE IS A PASS (deren.vulkan.pass.deferred), and what the frame still owes it is one answer
            // plus the stage's own frame-order duty: it is a
            // sampler of the stored surface, and whoever samples it FIRST publishes the G-buffer instance's
            // attachment writes. Both are the chain OWNER's now (see prepare_stage) - the runtime's `prepare` call
            // sits exactly where the pass's frame used to be set, and nothing is emitted between it and
            // `record_stage` (the stages carry no marks), so the command stream is unchanged.
            pass::stage const deferred_stage = {.name = "deferred", .passes = this->deferred_pass, .marks = false};
            this->prepare_stage(deferred_stage, command_buffer);
            pass::run_report const deferred_report = pass::record_stage(deferred_stage, this->make_pass_host());
            if (deferred_report.recorded == 0) {
                // The pass could not resolve a frame. Inside this branch the only remaining cause is the G-buffer
                // family having no set for this image (the pipeline and the target generation are what
                // deferred_lit_active() just checked), and the frame then has to be cleared rather than left
                // half-written - see the fallback's own comment.
                this->clear_scene_color_for_missing_gbuffer(command_buffer);
            }
            this->record_transparent_pass(command_buffer);

            // THE TOON CHARACTER STAGE, and its position here is the whole point of it: AFTER the lighting
            // stage (so there is a lit pixel to overwrite) and after the transparent pass (so a blended
            // surface in front of the character has already been composited and the depth test can reject the
            // character fragment behind it - the reference draws its `CharacterForward` pass before the
            // transparents, which is the same thing seen from the other side, and depth-EQUAL is what makes
            // both orders correct). Before the resolve, so the anti-aliasing sees the re-shaded frame.
            //
            // NO MARK OF ITS OWN: the frame's mark sequence is a fixed positional contract with a 16-mark
            // capacity, and a stage that carries none is what the other chain stages already do.
            //
            // IT IS SKIPPED WITHOUT RESOLVING ANYTHING when the feature table says the frame has no toon
            // character (`character_forward_pending`), which is what keeps every frame of every capture-gate
            // scenario byte-identical while the feature is off - the runner asks the pass's `feature()` before
            // it resolves the declaration, so the skip costs no barrier and no resolve.
            pass::stage const character_forward_stage = {.name = "character_forward", .passes = this->character_forward_pass, .marks = false};
            this->prepare_stage(character_forward_stage, command_buffer);
            [[maybe_unused]] pass::run_report const character_forward_report = pass::record_stage(character_forward_stage, this->make_pass_host());

            // THE SCREEN-SPACE DEPTH RIM, immediately after the stage it outlines: it samples the depth (which
            // the character-forward pass has just handed back to a sampled layout) and ADDS a contour to the
            // scene colour that stage wrote. Still before the resolve, so the anti-aliasing sees the two
            // together rather than a contour drawn over an already-resolved frame.
            //
            // IT IS GATED BY THE SAME FEATURE as the stage above (see its `feature()`), so a frame with no toon
            // character records neither and the two cannot come apart.
            pass::stage const toon_screen_rim_stage = {.name = "toon_screen_rim", .passes = this->toon_screen_rim_pass, .marks = false};
            this->prepare_stage(toon_screen_rim_stage, command_buffer);
            [[maybe_unused]] pass::run_report const toon_screen_rim_report = pass::record_stage(toon_screen_rim_stage, this->make_pass_host());

            // THE REWRITTEN CHAIN'S RIM, in the SAME position and for the same reasons: it samples the depth the
            // character stage has just handed back to a sampled layout and ADDs the Goo reference's rim - the
            // object-space part AND the screen-space `DepthRim` the surface shader cannot compute - to the scene
            // colour that stage wrote, before the resolve sees it.
            //
            // THE TWO RIM STAGES CANNOT BOTH RECORD, and that is one predicate rather than an ordering rule: the
            // owner answers "toon_screen_rim" with `!goo_toon_active()` and "goo_rim" with `goo_toon_active()`, so
            // the article's contour and the Goo rim are alternatives - which is what "the rewritten chain must not
            // wear two rims" means here. It also means a frame with `[render] goo_toon = false` records neither
            // this stage's pass nor its preamble, which is what keeps every capture scenario byte-identical.
            pass::stage const goo_rim_stage = {.name = "goo_rim", .passes = this->goo_rim_pass, .marks = false};
            this->prepare_stage(goo_rim_stage, command_buffer);
            [[maybe_unused]] pass::run_report const goo_rim_report = pass::record_stage(goo_rim_stage, this->make_pass_host());
        } else {
            // The pass does not run (the debug view replaces the lighting stage, and a skipped lighting stage has
            // no G-buffer to start rays from), but every mark is written in order on every frame - the
            // report's labels are positional. Written next to scene_end, so the interval is 0 ms.
            this->gpu_mark(command_buffer, gpu_mark_id::rt_shadow_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        this->gpu_mark(command_buffer, gpu_mark_id::lighting_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // TAA resolve: blend the scene color with the reprojected history into the HDR target the post
        // chain reads, then copy the result into the history image for the next frame that renders this
        // swapchain image. The PASS owns all of that now (deren.vulkan.pass.taa), and the two things below are the
        // part it cannot own yet:
        //
        //  * the G-buffer depth's transition to a sampled layout, whose "was it written this frame" flag
        //    belongs to the G-buffer pass, and
        //  * clearing the motion-vector flag, which is what stops a later pass in the same frame from
        //    transitioning the velocity image a second time.
        //
        // BOTH ARE THE CHAIN OWNER'S NOW, run from its `prepare` for this stage and gated on the same predicate the
        // runner gates the stage on - so the frame never touches them on a frame the pass does not run (clearing the
        // velocity flag for a frame with no resolve would make the GI tracer sample an image still in ATTACHMENT
        // layout). The runtime's call sits exactly where those two lines were.
        pass::stage const taa_stage = {.name = "taa", .passes = this->taa_pass, .marks = false};
        {
            this->prepare_stage(taa_stage, command_buffer);
            [[maybe_unused]] pass::run_report const taa_report = pass::record_stage(taa_stage, this->make_pass_host());
        }
        // The matrix the NEXT frame's motion vectors are computed against is this frame's, and it is only
        // recorded when the resolve actually wrote a history: a resolve that bailed out (no descriptor set) must
        // not claim one. The pass answers that (`wrote_history`) and the owner reports it through `collect`; the
        // array stays the renderer's because the camera UBO - not TAA - reads it as `prev_view_proj`.
        this->collect_stage("taa");
        this->gpu_mark(command_buffer, gpu_mark_id::taa_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // G-buffer debug mode (an inspection of the stored data, never combined with the lighting
        // stage or TAA): turn one channel into a visible image in the HDR target. THE PASS owns the recording
        // (deren.vulkan.pass.geometry_buffer_debug); what stays here is the frame - the target's own transition (which has to
        // happen even when the pass cannot draw, because the post chain samples that image), the two per-image
        // hand-backs its frame carries, and the fallback that clears the target when there is no set to draw with.
        if (this->gbuffer_pass_active() && !this->deferred_lit_active()) {
            // The HDR target becomes a colour attachment BEFORE the stage: the G-buffer pass wrote its own targets,
            // so this image was never an attachment this frame, and the pass's instance CLEARs it.
            std::array<VkImageMemoryBarrier2, 1> hdr_barrier = {deren::vulkan::color_attachment_transition};
            hdr_barrier[0].image = vk.hdr_images[this->current_image_index];
            VkDependencyInfo const hdr_dependency = make_image_dependency_info(1, hdr_barrier.data());
            vkCmdPipelineBarrier2(command_buffer, &hdr_dependency);
            // THIS STAGE'S FRAME-ORDER DUTY, the same shape as the lighting stage's above and for the same reason:
            // the debug view runs INSTEAD of the lighting stage, so it is the stage that hands the stored surface
            // and the motion vectors to samplers this frame - and both halves are the chain OWNER's now (the
            // depth's flag-based accessor and the velocity flag's clear, see prepare_stage).
            pass::stage const debug_stage = {.name = "gbuffer_debug", .passes = this->gbuffer_debug_stage, .marks = false};
            this->prepare_stage(debug_stage, command_buffer);
            pass::run_report const debug_report = pass::record_stage(debug_stage, this->make_pass_host());
            if (debug_report.recorded == 0) {
                // Inside this branch the only remaining cause is the G-buffer family having no set for this image
                // (the pipeline is what gbuffer_pass_active() just checked), and the frame then has to be cleared -
                // see the fallback's own comment. Its own transition is part of it.
                this->clear_hdr_for_missing_gbuffer_set(command_buffer);
            }
        }
        this->gpu_mark(command_buffer, gpu_mark_id::main_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }

    void runtime::barrier_image_to_sampling(VkCommandBuffer const command_buffer, VkImage const image) {
        // A pipeline barrier may not be recorded inside a dynamic rendering instance
        // (VUID-vkCmdPipelineBarrier2-None-09553), which is why every caller of this runs BEFORE its
        // vkCmdBeginRendering.
        std::array<VkImageMemoryBarrier2, 1> barriers = {deren::vulkan::hdr_sampling_transition};
        barriers[0].image = image;
        VkDependencyInfo const dependency_info = make_image_dependency_info(1, barriers.data());
        vkCmdPipelineBarrier2(command_buffer, &dependency_info);
    }

    void runtime::record_overlay_if_enabled(VkCommandBuffer const command_buffer) {
        if (this->debug_gui_shown && this->debug_overlay.is_active()) {
            this->debug_overlay.record(command_buffer);
        }
    }

    // =============================================================================================
    // THE POST CHAIN (deren.vulkan.pass.post)
    // =============================================================================================
    //
    // NO RESOLVER IS LEFT FOR IT (S3). The composite's declaration resolves generically except for the one
    // frame-decided choice (which target and pipeline variant, and the push block that follows from it), which
    // its own `resolve` override makes; its four bloom levels and FXAA resolve ENTIRELY from their declarations -
    // the level they write (a `bloom` element), the level they read (the element before it, or nothing at level
    // 0), their extent (the bloom element's size) and
    // the pipeline they record with, which is the COMPOSITE's R16F variant published by name (`post_hdr`) and
    // resolved by asking the chain's passes - see `resolve_pipeline`.
    //
    // WHAT IS NOT RESOLVED, and it is a decision rather than an omission: the HDR target's transition to a sampled
    // layout. The composite reads that image whether or not bloom runs and the bright-pass prefilter reads it too,
    // and on a frame the whole chain is skipped nobody else would move it - so it has exactly one owner and that
    // owner is the frame loop (see render_resource::post_composite_io).

    bool runtime::post_fxaa_active() const noexcept {
        // ONE definition of "the FXAA pass is this frame's last writer": the resolver picks the composite's target
        // and pipeline variant with it, the frame loop decides the overlay's owner with it, the feature registry
        // reports it, and set_fxaa() already folds the pipeline's existence into `fxaa_on` for the same reason. The
        // pipeline is the FXAA PASS's now (deren.vulkan.pass.fxaa), which is why this asks the pass rather than a member.
        //
        // ... AND IT CANNOT BE TRUE ON A FRAME THE UPSCALE RUNS, which is a CORRECTNESS rule rather than tidiness:
        // the two passes read the SAME LDR image and both want to be the frame's last writer, and FXAA's edge
        // filter is defined on the display-referred image AT THE RESOLUTION IT FILTERS - the render extent. On a
        // scaled frame the upscale resamples that image up to the output, so an FXAA result written at the render
        // extent would be thrown away by the resolve (or, if FXAA won, the resolve would magnify an already
        // filtered image). Both cannot run, and the upscale is the one that must: it is what makes a scaled frame
        // fill the presented image at all.
        return this->fxaa_on && this->pass_ready("fxaa") && !this->post_upscale_active();
    }

    bool runtime::post_upscale_active() const noexcept {
        // ONE definition of "the upscale pass is this frame's last writer", composed from the two facts that
        // decide it: the render chain is running BELOW the output size, and the pass built the pipeline it
        // records with (deren.vulkan.pass.upscale, created by create_passes()).
        //
        // AT render_scale == 1.0 THE ANSWER IS FALSE BY CONSTRUCTION, and that is the whole reason this
        // predicate is not just `pass_ready`: `core::render_extent()` returns the swapchain's extent at 1.0, so
        // the resolve would read an image at the output size and write an image at the output size - a
        // same-size resample that costs a pass and changes no pixel. This is also what keeps every scale-1.0
        // capture byte-identical: the runner never resolves or records the stage, the composite never writes
        // the LDR image for it, and the frame is the frame it always was.
        return this->vulkan_core.render_scale < 1.0f && this->pass_ready("upscale");
    }

    void runtime::draw_overlay_after(void* const owner, VkCommandBuffer const command_buffer) {
        // The composite's frame carries this as a plain function pointer (a pass records into what it is given and
        // knows nothing about this class), so the member is reached through the context that frame also holds.
        static_cast<runtime*>(owner)->record_overlay_if_enabled(command_buffer);
    }

    // =============================================================================================
    // THE G-BUFFER DEBUG VIEW (deren.vulkan.pass.geometry_buffer_debug)
    // =============================================================================================
    //
    // THE G-BUFFER DEBUG VIEW'S RESOLVER AND ITS `ensure_inputs` CALLBACK ARE GONE (S3). Its declaration resolves
    // generically - the HDR display target, the FOUR images it moves to a sampled layout (three stored surface
    // targets and the motion vectors, all per-image families the table publishes),
    // its own pipeline and a full-frame extent - and its push block is the pass's own (its channel, the frame's two
    // projection terms and a motion gain derived from the frame's width). The images themselves are heap slots the
    // shader names. Nothing here is left but the frame's own
    // transitions, which stay in the frame loop as they were: the HDR target's move to a colour attachment has to
    // happen even on a frame the pass cannot draw, because the post chain samples that image.

    void runtime::clear_hdr_for_missing_gbuffer_set(VkCommandBuffer const command_buffer) {
        // The debug view did not record - its pipeline is missing (a startup failure), or the frame has no target
        // for it. The pass cannot do this itself, because a pass records nothing when its declaration does not
        // resolve - so the frame's
        // answer lives here: the target is CLEARED, which is what makes such a frame black rather than undefined
        // (the post chain samples that image), and the log line says so once per frame. (The log line's wording is
        // historical: the missing thing used to be a descriptor set, and the frame's answer to a missing one was
        // this same clear.)
        core const& vk = this->vulkan_core;
        uint32_t const index = this->current_image_index;
        if (index >= vk.hdr_images.size()) {
            return;
        }
        deren::utility::log("runtime: gbuffer debug pass has no descriptor set - showing a cleared frame");
        std::array<VkImageMemoryBarrier2, 1> clear_barrier = {deren::vulkan::color_attachment_transition};
        clear_barrier[0].image = vk.hdr_images[index];
        VkDependencyInfo const clear_dependency = make_image_dependency_info(1, clear_barrier.data());
        vkCmdPipelineBarrier2(command_buffer, &clear_dependency);
        VkClearValue clear = {};
        VkRenderingAttachmentInfo const attachment = make_color_attachment_info(vk.hdr_image_views[index], clear, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE);
        VkRenderingInfo const rendering_info = make_rendering_info(0, {{0, 0}, vk.render_extent()}, true, &attachment, nullptr);
        vkCmdBeginRendering(command_buffer, &rendering_info);
        vkCmdEndRendering(command_buffer);
    }
    bool runtime::record_post_process(VkCommandBuffer const command_buffer) {
        core const& vk = this->vulkan_core;

        // ---- the scene side: close the geometry instance, then the stages that consume the G-buffer
        this->record_scene_tail(command_buffer);

        if (!this->pass_ready("post_composite")) {
            return false; // no post pipeline (creation failed): the HDR frame cannot be presented correctly
        }

        std::size_t const index = this->current_image_index;

        // HDR scene target -> fragment-shader read (the prefilter and the composite both read it)
        this->barrier_image_to_sampling(command_buffer, vk.hdr_images[index]);

        // The composite's GI upsample also samples the G-buffer depth and normal, and the stage that
        // normally publishes those two is the deferred lighting stage (or the debug view) - both part
        // of the G-buffer path. A frame that never ran it (the deferred pipeline missing, so
        // gbuffer_pass_active() is false and nothing wrote the targets) still reaches the composite,
        // whose descriptor declares SHADER_READ for both bindings either way. UNDEFINED as the old
        // layout is honest here - there is no content to preserve - and the GI weight is 0 on such a
        // frame, so the taps' values cannot influence the image.
        if (!this->gbuffer_pass_active()) {
            std::array<VkImageMemoryBarrier2, 2> gbuffer_barriers = {};
            gbuffer_barriers[0] = deren::vulkan::undefined_to_depth_sampling_transition; // DEPTH aspect
            gbuffer_barriers[0].image = vk.gbuffer_depth_images[index];
            gbuffer_barriers[1] = deren::vulkan::undefined_to_sampling_transition;
            gbuffer_barriers[1].image = vk.gbuffer_images[1][index]; // the world normal
            VkDependencyInfo const gbuffer_dependency = make_image_dependency_info(static_cast<uint32_t>(gbuffer_barriers.size()), gbuffer_barriers.data());
            vkCmdPipelineBarrier2(command_buffer, &gbuffer_dependency);
        }

        // Screen-space GI runs HERE, and the position is the whole reason it cannot feed back: `hdr`
        // was rewritten earlier in this frame (by the TAA resolve, or by the G-buffer pass's clear
        // when TAA is off) and the composite that ADDS this pass's output is downstream, so what the
        // tracer samples at a hit is direct radiance and never its own previous result. Sampling an
        // image that already contained GI would make the loop gain > 1 and accumulate energy.
        //
        // pass of the chain - the spatial filter - that sets it: the composite samples that filter's
        // output, so a frame whose filter did not run (no descriptor set, no images) has nothing to add
        // and must weigh 0 rather than show whatever that image happens to hold. The filter in turn
        // only runs when the temporal resolve ran, because filtering a stale accumulation would just
        // make the staleness smoother.
        //
        // through the temporal pass's callback, and a frame whose reflection did not run must not leave the
        // previous frame's answer for the filter's `spec_weight` lane to read.

        // GPU timing: the lighting chain ends here (its tracer and its temporal resolve). Written
        // unconditionally like every mark, so a frame that skips it reports 0 ms and the labels stay aligned.

        {
        }

        // ---- THE BLOOM CHAIN, as a stage of FOUR passes (deren.vulkan.pass.post) ----
        // The chain is four instances of one class; the level IS the pass boundary (each binds a different one of
        // the post family's five sets), and what the host resolves for each is in resolve_post_bloom. The RUNNER
        // gates them: the pass declares the feature `bloom`, which is `bloom_intensity > 0` and the chain's
        // pipeline being there - the same predicate that decides whether the composite adds a bloom sum (see
        // resolve_post_composite, where the weight is zeroed for the debug view - which is also what
        // `active_features().gbuffer_debug` means).
        pass::stage const bloom_stage = {.name = "bloom", .passes = this->bloom_pass, .marks = false};
        pass::run_report const bloom_report = pass::record_stage(bloom_stage, this->make_pass_host());
        if (bloom_report.recorded == 0) {
            // THE OFF PATH, and it is the frame loop's because it is about the COMPOSITE's descriptor: its set
            // declares all four bloom levels as inputs and multiplies them by the weight it pushed, and a
            // statically-used binding's image has to be in the layout that descriptor declares whether or not the
            // value is ever read. UNDEFINED as the old layout asserts nothing (their contents are dead on a frame
            // nothing wrote), and this is the same answer the shadow map's spare layers and the GI image's off path
            // give. It hangs on "the stage recorded nothing" rather than on the knob, so a frame whose levels had no
            // descriptor set takes it too.
            for (uint32_t level = 0; level < deren::vulkan::core::bloom_level_count; ++level) {
                std::array<VkImageMemoryBarrier2, 1> barriers = {deren::vulkan::undefined_to_sampling_transition};
                barriers[0].image = vk.bloom_images[level][index];
                VkDependencyInfo const dependency_info = make_image_dependency_info(1, barriers.data());
                vkCmdPipelineBarrier2(command_buffer, &dependency_info);
            }
        }
        // GPU timing: the bloom chain ends here (a disabled chain is just the layout fixups above, so its interval
        // reads ~0).
        this->gpu_mark(command_buffer, gpu_mark_id::bloom_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // ---- THE COMPOSITE, as a stage of one pass ----
        // ITS FRAME CARRIES THE OVERLAY only when NEITHER resolve runs: the overlay has no load op of its own, so
        // it has to be drawn inside whichever instance is the frame's LAST writer - and that is the FXAA pass on
        // the frames it resolves and the upscale pass on the frames IT does. Which of the three that is, and
        // whether this frame's bloom sum exists, are published as facts
        // (`frame_facts::fxaa_resolves` / `frame_facts::upscale_resolves` / `debug_view`) and the PASS composes
        // its frame from them, including the overlay hook it was handed once in the application's `attach` (see
        // frame_pass::prepare_frame).
        pass::stage const composite_stage = {.name = "post_composite", .passes = this->post_composite_stage, .marks = false};
        this->prepare_stage(composite_stage, command_buffer);
        [[maybe_unused]] pass::run_report const composite_report = pass::record_stage(composite_stage, this->make_pass_host());
        // GPU timing: the composite (and the debug overlay, when it draws here) is done.
        this->gpu_mark(command_buffer, gpu_mark_id::composite_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // ---- THE FXAA RESOLVE, as a stage of one pass (deren.vulkan.pass.fxaa) ----
        // It is the frame's LAST writer whenever it runs, so it is the pass that carries the overlay - the other
        // half of the split the composite's frame above states. The runner gates it on the feature `fxaa`, which is
        // `post_fxaa_active()`: the same predicate that decided this frame's composite TARGET, so the pass runs
        // exactly when the LDR image is what the composite wrote.
        pass::stage const fxaa_stage = {.name = "fxaa", .passes = this->fxaa_pass, .marks = false};
        this->prepare_stage(fxaa_stage, command_buffer);
        [[maybe_unused]] pass::run_report const fxaa_report = pass::record_stage(fxaa_stage, this->make_pass_host());
        // GPU timing: the FXAA pass (and the overlay it carries when it is the last writer) is done. Without FXAA
        // the composite already ended the frame's display work, so this interval is ~0.
        this->gpu_mark(command_buffer, gpu_mark_id::fxaa_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        // ---- THE UPSCALE RESOLVE, as a stage of one pass (deren.vulkan.pass.upscale) ----
        // It is the frame's LAST writer on the frames it runs (the render chain is smaller than the output), so
        // it is the pass that carries the overlay there - the third and last case of the split the composite's
        // frame states. The runner gates it on the feature `upscale`, which is `post_upscale_active()`: the same
        // predicate that decided this frame's composite TARGET, so the pass runs exactly when the LDR image is
        // what the composite wrote. It sits AFTER the FXAA stage because it is the later half of the exclusive
        // pair (`post_fxaa_active` is false whenever this one is true), and it has no timing mark of its own: on
        // the frames it runs its cost is reported in the interval after `fxaa_end`, which is where the frame's
        // remaining commands already are.
        pass::stage const upscale_stage = {.name = "upscale", .passes = this->upscale_pass, .marks = false};
        this->prepare_stage(upscale_stage, command_buffer);
        [[maybe_unused]] pass::run_report const upscale_report = pass::record_stage(upscale_stage, this->make_pass_host());

        // true in all three paths: the composite (when neither resolve runs), the FXAA pass or the upscale pass
        // wrote the swapchain
        return true;
    }
    frame_status runtime::end_recording() {
        deren::vulkan::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::vulkan::profiling::cpu_phase::post};
        core& vk = this->vulkan_core;
        vk_command_buffer& command_buffer = this->command_buffers[static_cast<uint32_t>(vk.current_frame)];

        // close the scene rendering instance and run the post-process pass (exposure/tonemap).
        // The return value says whether a fullscreen pass actually wrote the swapchain image: only
        // then is it in GENERAL and only then does it hold this frame's result.
        bool const post_wrote_swapchain = this->record_post_process(*command_buffer);
        // Screenshot: while the post pass wrote the swapchain image it is still in GENERAL and still owned
        // by this frame - the only point where a read-back copy is legal. Doing it here (rather than after
        // the present) also means the capture needs no extra submit, no re-acquire and no layout hand-back
        // to the WSI.
        //
        // S2 BATCH 2: THE COPY IS THE FIRST FULLY CONTRACT-RECORDED OPERATION OF THIS FRAME. The list is
        // the frame's own primary command buffer (borrowed from the backend), the image is the frame image,
        // the destination is the backend's read-back slot, and the two use() calls are the layout pair the
        // hand-written version carried - the shadow gate (A5) proves they land on the same barriers. THE
        // SUBMITTER DOES NOT CHANGE: this runtime submits the same command buffer below.
        if (post_wrote_swapchain && this->screenshot_requested) {
            deren::promise::rhi::command_list* const commands = vk.begin_commands();
            deren::promise::rhi::image* const frame_image = vk.frame_image();
            deren::promise::rhi::buffer* const slot = vk.frame_readback_buffer();
            if (commands != nullptr && frame_image != nullptr && slot != nullptr) {
                deren::promise::rhi::image_copy_region const region{.extent = frame_image->extent()};
                // BOTH BARRIERS ARE CHECKED (the recording surface answers with an error instead of
                // dropping one silently - a dropped barrier leaves the image in a state nobody declared,
                // which is the failure class task-148 measured). A refused first barrier skips the copy:
                // there is no point recording a copy whose entry barrier never happened.
                deren::promise::rhi::error const to_source =
                    commands->use(*frame_image, deren::promise::rhi::image_use::color_attachment, deren::promise::rhi::image_use::transfer_source);
                deren::promise::rhi::error copied = to_source;
                deren::promise::rhi::error hand_back = deren::promise::rhi::error::ok;
                if (to_source == deren::promise::rhi::error::ok) {
                    copied = commands->copy_image_to_buffer(*slot, *frame_image, region);
                    // The image gets its render-target role back even when the copy was refused: the
                    // present transition below assumes the frame's own state, and the refusal is reported
                    // rather than swallowed.
                    hand_back = commands->use(*frame_image, deren::promise::rhi::image_use::transfer_source, deren::promise::rhi::image_use::color_attachment);
                }

                // ---- AND THE ESCAPE, MEASURED WHERE IT HAS TO AGREE WITH THE CONTRACT -----------------
                // `vulkan_escape` is the one ability this backend announces, and its
                // `native_command_buffer()` has no other caller yet: the engine records through its own
                // `core&` until S3 moves the passes, so this line is the measurement that the escape
                // answers with the VERY buffer this frame is being recorded into - and that the two
                // extension lists the guard rail checks are the ones the context enabled. It sits on the
                // screenshot path on purpose: rare, and next to the operation it is about.
                auto* const escape = static_cast<deren::promise::rhi::vulkan_escape*>(
                    vk.query_extension(deren::promise::rhi::extension_kind::vulkan_escape));
                if (escape != nullptr) {
                    void* const native = escape->native_command_buffer(*commands);
                    deren::utility::log("rhi: vulkan_escape names this frame's command buffer {} (device {:#x}, {} instance extension(s), {} device extension(s))",
                                        native == reinterpret_cast<void const*>(*command_buffer) ? "identically" : "DIFFERENTLY",
                                        reinterpret_cast<uintptr_t>(escape->native_device()),
                                        escape->enabled_instance_extensions().size(),
                                        escape->enabled_device_extensions().size());
                }
                if (to_source != deren::promise::rhi::error::ok) {
                    // The capture is dropped for this frame; the request stays pending, so the next frame
                    // retries - and the refusal is on the record instead of being silently ignored.
                    deren::utility::log("screenshot: the read-back's first barrier was refused ({}); this frame's capture is dropped",
                                        static_cast<std::uint32_t>(to_source));
                } else if (hand_back != deren::promise::rhi::error::ok) {
                    deren::utility::log("screenshot: the read-back's hand-back barrier was refused ({}); this frame's capture is dropped",
                                        static_cast<std::uint32_t>(hand_back));
                } else if (copied == deren::promise::rhi::error::ok) {
                    this->screenshot_pending = true;
                    this->screenshot_requested = false; // served
                } else if (copied == deren::promise::rhi::error::unsupported) {
                    // This surface cannot be a copy source at all (the swapchain images lack TRANSFER_SRC),
                    // so the copy would violate VUID-vkCmdCopyImageToBuffer-srcImage-00186. The JUDGEMENT
                    // is the backend's - the swapchain is its object - and the one-time log and the F12
                    // shutdown that go with it stay here, where they always were. A permanent condition:
                    // retrying every frame would only spam.
                    if (!this->screenshot_unsupported_logged) {
                        this->screenshot_unsupported_logged = true;
                        deren::utility::log("screenshot: unsupported (swapchain has no TRANSFER_SRC usage) - F12 disabled");
                    }
                    this->screenshot_requested = false;
                }
                // not_ready / invalid_argument: the request stays pending and the next frame retries, which
                // is the posture the hand-written version had ("a failed copy stays pending").
            }
        }
        // Dynamic rendering has no render pass finalLayout to hand the image back to the
        // presentation engine: transition the swapchain image to PRESENT_SRC_KHR explicitly. The
        // post pass leaves the image in GENERAL, so the barrier is needed whenever
        // it ran. When it was skipped the image never entered GENERAL, and claiming
        // that old layout would be a lie (validation: "oldLayout is not matching with the current
        // layout"): transition from UNDEFINED instead - the frame has no content to preserve anyway.
        VkImageMemoryBarrier2 present_barrier = post_wrote_swapchain ? present_transition : deren::vulkan::undefined_to_present_transition;
        present_barrier.image = vk.swap_chain_images[this->current_image_index];

        VkDependencyInfo const dependency_info = make_image_dependency_info(1, &present_barrier);
        vkCmdPipelineBarrier2(*command_buffer, &dependency_info);
        // GPU timing: last mark of the frame. The interval it closes is everything after the FXAA
        // (or composite) pass - the screenshot read-back copy and the present barrier.
        this->gpu_mark(*command_buffer, gpu_mark_id::frame_end, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        if (vkEndCommandBuffer(*command_buffer) != VK_SUCCESS) {
            return frame_status::end_recording_failed;
        }
        return frame_status::proceed;
    }

    frame_status runtime::submit_and_present() {
        // The resource table's differential check reports itself HERE, once, at the end of the frame that closes
        // its window - the last point of a frame, so the counters cover EVERY stage (the post and lighting stages record
        // inside `end_recording`, which is why this cannot sit there). THREE frames rather than one, because a
        // pass may legitimately resolve only from the second frame on (TAA needs a history), and a check that
        // watched one frame would report those as uncovered. See verify_resource_table: a mismatch is a table
        // entry to fix before that pass's resolver can be deleted.
        ++this->resource_check_frames;
        if (!this->resource_check_reported && this->resource_check_frames >= 3) {
            this->resource_check_reported = true;
            deren::utility::log("resource table: {} entries published, {} declaration handle checks against the resolvers, {} mismatch(es) over {} frame(s)",
                                this->frame_resources.size(),
                                this->resource_check_checked,
                                this->resource_check_mismatched,
                                this->resource_check_frames);
        }
        deren::vulkan::profiling::cpu_phase_timer const phase_timer{this->cpu_timings, deren::vulkan::profiling::cpu_phase::submit};
        core& vk = this->vulkan_core;
        vk_command_buffer& command_buffer = this->command_buffers[static_cast<uint32_t>(vk.current_frame)];

        // Submit + present; recreate the swapchain when presentation reports out of date
        auto const submit_started = std::chrono::steady_clock::now(); // sub-phase marks: what of submit is the
        // queue submission (with its timeline signal) and what is the present call below
        if (vk.submit(*command_buffer, this->current_image_index) != VK_SUCCESS) {
            return frame_status::submit_failed;
        }
        this->cpu_timings.add(deren::vulkan::profiling::cpu_phase::submit_queue, std::chrono::steady_clock::now() - submit_started);
        auto const present_started = std::chrono::steady_clock::now();
        VkResult const present_result = vk.present(this->current_image_index);
        this->cpu_timings.add(deren::vulkan::profiling::cpu_phase::present, std::chrono::steady_clock::now() - present_started);
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
            deren::utility::log("present out of date, recreating swapchain");
            if (vk.recreate_swap_chain()) {
                this->on_swapchain_recreated();
            }
        } else if (present_result != VK_SUCCESS) {
            return frame_status::present_failed;
        }
        vk.to_next_frame();
        return frame_status::proceed;
    }

    frame_status runtime::render_frame() {
        // Whole frame in one call: run the frame phases directly, in order, with no caller
        // writes interleaved (callers that need per-frame host updates run the phases at fine
        // granularity themselves, writing between pace_and_acquire() and begin_recording()).
        frame_status const skip = this->poll_events();
        if (skip != frame_status::proceed) {
            return skip;
        }
        this->recreate_if_minimized();
        frame_status const paced = this->pace_and_acquire();
        if (paced != frame_status::proceed) {
            return paced;
        }
        frame_status const begin = this->begin_recording();
        if (begin != frame_status::proceed) {
            return begin;
        }
        this->record_main_drawcalls();
        frame_status const end = this->end_recording();
        if (end != frame_status::proceed) {
            return end;
        }
        return this->submit_and_present();
    }
} // namespace deren::vulkan