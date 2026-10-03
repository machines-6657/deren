module;

#include <algorithm> // std::clamp / std::max in orbit_camera_pan_delta
#include <array>
#include <cmath> // std::sin / std::cos in the orbit camera maths
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>
#include <vulkan/vulkan.h>

module deren.vulkan.primitive;

import deren.utility; // DIAGNOSTIC only (see normal_draw_primitive::draw): this app is a GUI-subsystem binary, so
                      // stdout/stderr go nowhere and every traceable line has to come through deren::utility::log.

namespace deren::vulkan {
    namespace {
        /// Send this draw's stage block to the pipeline it is about to draw with.
        ///
        /// A block is DATA now: no pipeline in this renderer has a layout (`vkCmdPushConstants` would have
        /// nothing to push to), and a heap-native shader reads `vkCmdPushDataEXT` exactly as it read push
        /// constants. The environment's endpoint appends the two heap indices - the frame slot and the swapchain
        /// image - as the block's last fields, so what is pushed here is the block alone. See
        /// `render_environment::push_block` and shaders/heap_slots.glsl.
        void push_stage_block(render_environment const& env, auto const& block) {
            if (env.push_block != nullptr) {
                [[maybe_unused]] bool const pushed = env.push_block(env.push_owner, env.command_buffer, std::as_bytes(std::span(&block, 1)), 0u);
            }
        }
    } // namespace
    void primitive::set_world(glm::mat4 const& world) {
        // the accumulated world transform written by a scene tree walk (scene_tree::primitive
        // interface); draw() pushes push.model verbatim, so this is all the leaf needs
        this->push.model = world;
    }

    // `bind_geometry_and_push` IS GONE (docs/mesh_shaders.md step 4): it bound a primitive's vertex and index
    // buffers and pushed its block, and with the vertex path removed nothing calls it - a geometry draw pushes its
    // lanes and dispatches (see push_meshlet_lanes), so there is no binding step left to share.

    // ---- THE HOST'S COPY OF THE SHADER'S CULLING MATHS (docs/mesh_shaders.md step 3) ----
    // These are deliberate duplicates of `matrix_max_axis_scale` and `clip_sphere_visible` in
    // shaders/mesh_geometry.slang, and the duplication IS the safety argument: the host must never reject a meshlet
    // the stage would have kept, and the only way to be sure of that is to run the SAME conservative test - the
    // radius bound is a row-sum bound on both matrices (|M v|_inf <= max_i sum_j |M_ij| |v|_inf), and the clip test
    // uses `w + radius` on every plane so it is correct for a perspective projection too.
    namespace {
        float host_matrix_max_axis_scale(glm::mat4 const& m) {
            float largest = 0.0f;
            for (int32_t row = 0; row < 3; ++row) {
                // glm's operator[] hands back a COLUMN, so the shader's `m[row][0..2]` reads as these three entries
                float const sum = std::abs(m[0][row]) + std::abs(m[1][row]) + std::abs(m[2][row]);
                largest = std::max(largest, sum);
            }
            return largest;
        }

        bool host_clip_sphere_visible(glm::vec4 const& clip, float const radius) {
            return clip.w + radius >= 0.0f && std::abs(clip.x) <= clip.w + radius && std::abs(clip.y) <= clip.w + radius &&
                   clip.z >= -clip.w - radius && clip.z <= clip.w + radius;
        }
    } // namespace

    uint32_t primitive::push_meshlet_lanes(render_environment const& env, primitive const& geometry, uint32_t const first_index, uint32_t const index_count, int32_t const base_vertex, bool const material_two_sided) const {
        // WHETHER A BACK-FACE TEST MAY CULL ANYTHING FOR THIS DRAW (docs/mesh_shaders.md step 3): only when the pass
        // will cull back faces itself. A two-sided material - or a session that forces two-sided rasterization, which
        // is what the shadow pass does so a caster is never dropped for facing away from the light - KEEPS both sides
        // on purpose, so rejecting a meshlet for facing away would remove geometry the pass meant to draw. That is
        // exactly what the first backface experiment did, and why it changed six scenarios.
        bool const backface_legal = !env.two_sided && !material_two_sided;
        // WHETHER THIS DRAW CAN BE CULLED AT ALL, and every clause is one of the ways a host-side cull could remove
        // something visible:
        //  - only a meshlet session, only when the session asked for it, and only with both endpoints present;
        //  - NEVER an instanced draw: its world matrix comes from the instance table per workgroup, so one run's
        //    survivors differ per instance and a single compacted run cannot describe them;
        //  - NEVER a deforming draw: the sphere is built from the BIND POSE, so it bounds geometry this draw is not
        //    about to emit - the same rule the entries state for their own test.
        bool const cullable = env.meshlets && env.meshlet_culled && env.meshlet_view_proj != nullptr && env.meshlet_culled_write != nullptr &&
                              geometry.meshlet_count != 0u && (this->push.flags & 1u) == 0u && this->push.skin_base == 0u && this->push.morph_targets == 0u;
        uint32_t survivors = not_culled;
        std::vector<deren::vulkan::meshlet> kept;
        if (cullable) {
            std::array<float, 16> view_proj = {};
            if (env.meshlet_view_proj(env.push_owner, view_proj.data())) {
                // the matrix arrives as 16 floats (the camera UBO's `proj * view`), copied rather than constructed
                // through glm's type_ptr helpers: same layout, no extra include, and no doubt about transpose
                glm::mat4 vp = {};
                std::memcpy(&vp, view_proj.data(), sizeof(vp));
                glm::mat4 const& world = this->push.model;
                float const vp_scale = host_matrix_max_axis_scale(vp);
                float const world_scale = host_matrix_max_axis_scale(world);
                kept.reserve(geometry.meshlet_count);
                for (deren::vulkan::meshlet const& record : geometry.meshlets) {
                    glm::vec4 const clip = vp * (world * glm::vec4(record.center_x, record.center_y, record.center_z, 1.0f));
                    float const radius = record.radius * world_scale * vp_scale;
                    if (host_clip_sphere_visible(clip, radius)) {
                        kept.push_back(record);
                    }
                }
                // the write has to succeed or the run stays unculled: a half-written culled table would make the
                // entry read stale records, which is a wrong picture rather than a slow one
                if (env.meshlet_culled_write(env.push_owner, geometry.meshlet_base, std::as_bytes(std::span(kept)))) {
                    survivors = static_cast<uint32_t>(kept.size());
                }
            }
        }
        // the lanes carry the flags (see the declaration): bit0 "read this frame's culled table", bit1 "a back-face
        // test may cull this draw" - both only meaningful in a meshlet session, where this field is otherwise unused
        this->push_geometry_lanes_impl(env, geometry, first_index, index_count, base_vertex, survivors != not_culled, backface_legal);
        return survivors;
    }

    void primitive::push_geometry_lanes_impl(render_environment const& env, primitive const& geometry, uint32_t const first_index, uint32_t const index_count, int32_t const base_vertex, bool const host_culled, bool const backface_legal) const {
        if (env.push_at == nullptr || env.buffer_address == nullptr || env.mesh_geometry_push_offset == 0u) {
            return; // a session that does not use the shared scene block (or a test one) has nothing to fill
        }
        // ---- the geometry lanes: what the input assembler used to consume, as data ----
        // A PRIMITIVE WITHOUT GEOMETRY CAN REACH HERE (a leaf whose destroy() has run, or one built without
        // buffers), and the contract's handle makes that state READABLE as an empty manager - the old code
        // read it through a `buffer_detail const*` that was simply dereferenced, so this is the same
        // precondition with a defined answer instead of a null dereference. The answer is the zeroes the
        // block below already documents: "no buffer device address", which a mesh session can see in the
        // lanes it was just pushed.
        mesh_geometry_lanes lanes;
        lanes.vertex_address = geometry.vertex_buffer ? env.buffer_address(env.push_owner, *geometry.vertex_buffer) : 0;
        lanes.index_address = geometry.index_buffer ? env.buffer_address(env.push_owner, *geometry.index_buffer) : 0;
        // A MESHLET SESSION CARRIES THE MESHLET RUN HERE (docs/mesh_shaders.md step 3): the meshlet entry reads each
        // record's own window, so these two fields describe WHICH RECORDS this draw owns; the buffer addresses and
        // the index width are still the draw's, which is why the lanes carry both kinds of fact.
        lanes.first_index = env.meshlets ? geometry.meshlet_base : first_index;
        lanes.index_count = env.meshlets ? geometry.meshlet_count : index_count;
        // ... AND, in a meshlet session, WHETHER THE RUN WAS HOST-CULLED: the field is otherwise unused there (a
        // meshlet record carries its own base vertex), and the entry point reads it as "this frame's culled table,
        // not the shared one" (docs/mesh_shaders.md step 3)
        lanes.base_vertex = env.meshlets ? ((host_culled ? 1 : 0) | (backface_legal ? 2 : 0)) : base_vertex;
        // the buffer's own index type, which the shader needs because a raw load has no format: 4 for UINT32,
        // 2 for UINT16 (the shader reads the 16-bit case as the half of a 32-bit word its index falls in)
        lanes.index_width = geometry.index_type == VK_INDEX_TYPE_UINT16 ? 2u : 4u;
        if (lanes.vertex_address == 0 || lanes.index_address == 0) {
            // A buffer without a device address cannot be fetched by a mesh stage at all, and dispatching anyway
            // would read address 0. The primitive upload asks for SHADER_DEVICE_ADDRESS_BIT whenever the device
            // has buffer device addresses, so this is the "device does not" case: the lanes go out as zeroes
            // (they must be written either way) and a MESH session must skip the draw, which it can see from the
            // zeroes it just pushed - so the log line is the only thing left to add.
            static bool logged = false;
            if (!logged) {
                logged = true;
                deren::utility::log("[mesh] a caster at material slot {} has no buffer device address - a mesh pipeline cannot draw it", this->push.material_index.value);
            }
        }
        // THE PUSH COVERS THE WHOLE DECLARED BLOCK, and it starts at the previous member's END rather than at the
        // lanes - both because of one measured rule: with a descriptor-heap pipeline every byte of the declared
        // block must have been written before the draw, and validation names the range it is missing
        // (VUID-vkCmdDrawMeshTasksEXT-None-11376: "[0, 144), but vkCmdPushDataEXT was never called for range
        // [108, 112)" first, then "[140, 144)" - the scene block's alignment word and the std140 block-size
        // rounding respectively). So: zero-filled payload from `mesh_geometry_push_offset` to the block's end, with
        // the lanes copied in where the SHADER reads them.
        constexpr std::size_t payload_size = mesh_stage_block_size - mesh_geometry_push_offset_scene;
        static_assert(mesh_geometry_lanes_offset >= mesh_geometry_push_offset_scene, "the lanes cannot sit before the scene block's fill starts");
        static_assert(mesh_geometry_lanes_offset + sizeof(mesh_geometry_lanes) <= mesh_stage_block_size, "the lanes must fit inside the block");
        // WHERE THE LANES GO INSIDE THIS PAYLOAD: the shader reads them at the same offset in both blocks, but the
        // two sessions start filling at different ones - so this is a difference of the two, not a constant. (The
        // first version copied at the SCENE's offset for both and put the shadow pass's lanes 4 bytes late, which
        // read as a window belonging to no draw: the shadow map came out empty and the frame lost every shadow.)
        std::size_t const lanes_at = mesh_geometry_lanes_offset - env.mesh_geometry_push_offset;
        std::array<std::byte, payload_size> payload = {};
        std::memcpy(payload.data() + lanes_at, &lanes, sizeof(lanes));
        std::size_t const bytes = static_cast<std::size_t>(mesh_stage_block_size) - env.mesh_geometry_push_offset;
        [[maybe_unused]] bool const pushed = env.push_at(env.push_owner, env.command_buffer, env.mesh_geometry_push_offset, std::span<std::byte const>(payload.data(), bytes));
    }

    void primitive::mesh_dispatch(render_environment const& env, primitive const& geometry, uint32_t const index_count, uint32_t const instance_count, uint32_t const survivors) const {
        // ---- the dispatch: one workgroup per `mesh_triangles_per_workgroup` triangles of THIS draw ----
        // The workgroup budget is the shader's (see shaders/mesh_geometry.slang: 85 triangles, 255 vertices,
        // because a triangle costs three of the device's 256 output vertices), and the group count has to cover
        // the window: the stage itself decides how many of its 85 triangles are real, which is what makes a
        // window that is not a multiple of 85 work without a second command.
        constexpr uint32_t triangles_per_workgroup = 85u;
        uint32_t const triangles = index_count / 3u;
        // A MESHLET SESSION DISPATCHES ONE WORKGROUP PER MESHLET: each one reads its own window out of the table and
        // emits the whole meshlet, so the group count is the primitive's run - not a slice of a triangle count. A
        // HOST-CULLED run dispatches its SURVIVORS instead, which is the point of culling before the dispatch: a
        // meshlet the frustum rejected costs no workgroup at all (docs/mesh_shaders.md step 3).
        uint32_t const groups = survivors != not_culled
                                    ? survivors
                                    : (env.meshlets ? geometry.meshlet_count : (triangles + triangles_per_workgroup - 1u) / triangles_per_workgroup);
        if (groups != 0u) {
            // A MESHLET SESSION DISPATCHES THROUGH THE INDIRECT ENTRY POINT (docs/mesh_shaders.md step 3, second
            // mechanism): the counts travel in the command table at the primitive's OWN slot (`meshlet_base`), so a
            // COMPUTE culling pass can rewrite that record with the counts culling left and the dispatch picks them
            // up with no host change. The other mesh sessions keep the direct call - their counts are the host's,
            // and nothing will ever rewrite them - and the runtime logs which route was taken either way.
            if (env.meshlets && env.draw_mesh_tasks_indirect != nullptr) {
                // THE COMMAND IS THE SLOT'S CLASS, one table capacity apart: a HOST-CULLED run and an unculled one
                // (the shadow pass) dispatch the same primitive with DIFFERENT counts, so they cannot share a
                // command - whichever wrote last would be the count both passes got.
                uint32_t const slot = geometry.meshlet_base + (survivors != not_culled ? deren::vulkan::meshlet_capacity : 0u);
                [[maybe_unused]] bool const dispatched = env.draw_mesh_tasks_indirect(env.push_owner, env.command_buffer, slot, groups, instance_count, 1u);
            } else {
                [[maybe_unused]] bool const dispatched = env.draw_mesh_tasks(env.push_owner, env.command_buffer, groups, instance_count, 1u);
            }
        }
    }

    // A GEOMETRY DRAW IS A DISPATCH, and there is nothing to fall back to any more (docs/mesh_shaders.md step 4):
    // the vertex path is gone, so a session with no mesh pipeline bound (a leaf naming an unknown pipeline) draws
    // NOTHING and says so once - recording the command anyway would be a draw with no pipeline, which is a
    // validation error rather than a missing object.
    namespace {
        void log_meshless_draw_once() {
            static bool logged = false;
            if (!logged) {
                logged = true;
                deren::utility::log("a geometry draw ran in a session with no mesh pipeline bound - the draw is skipped (docs/mesh_shaders.md step 4)");
            }
        }
    } // namespace

    // Default-semantics draws (normal / instanced / static): request the recording session's
    // default pipeline - bind_default() no-ops when it is already bound, so consecutive leaves
    // of the same pass share one bind. Cull mode stays per draw (dynamic state, pipeline
    // independent): double-sided materials keep back faces. Transparent (alphaMode BLEND)
    // leaves disable depth writes so they blend onto whatever is behind them. All commands
    // record onto env.command_buffer.
    void normal_draw_primitive::draw(render_environment& env) const {
        // THE BIND COMES FIRST, and it is what decides `mesh_stage` - the session's bind callback picks the form.
        // Checking the flag before it was a measured mistake: a session built for the transparent pass starts with it
        // FALSE, so every one of that pass's leaves skipped itself and the scenario's frame changed.
        env.bind_default();
        if (!env.mesh_stage) {
            log_meshless_draw_once();
            return;
        }
        env.set_depth_write(!this->transparent);
        env.set_cull_mode(this->double_sided);
        // A MESH SESSION DOES NOT BIND GEOMETRY: the stage fetches it from the lanes pushed below.
        push_stage_block(env, this->push);
        uint32_t const survivors = this->push_meshlet_lanes(env, *this, 0u, this->draw_index_count, 0, this->double_sided);
        this->mesh_dispatch(env, *this, this->draw_index_count, 1u, survivors);
    }

    void normal_draw_primitive::destroy(vma_allocator&) noexcept {
        // geometry is owned by the two contract handles: reset() drops THIS leaf's reference (the GPU
        // memory dies with the last one - release is not destruction, see rhi::buffer::release()), and
        // there is no cached accessor to drop beside them any more. The old code nulled two
        // `buffer_detail const*` fields by hand, which existed only because those were borrows into the
        // allocator's map rather than part of the handle.
        this->vertex_buffer.reset();
        this->index_buffer.reset();
        this->draw_index_count = 0;
        this->vertex_count = 0;
    }

    bool normal_draw_primitive::is_valid() const noexcept {
        return static_cast<bool>(this->vertex_buffer) && static_cast<bool>(this->index_buffer) &&
               this->draw_index_count != 0;
    }

    void instanced_draw_primitive::draw(render_environment& env) const {
        env.bind_default(); // the bind decides `mesh_stage`, so it happens before the flag is read
        if (!env.mesh_stage) {
            log_meshless_draw_once();
            return;
        }
        env.set_depth_write(!this->transparent);
        // geometry belongs to source; push flag bit0 makes the mesh stage pick instances[group_id.y] per instance
        primitive const& geometry_source = *this->source;
        env.set_cull_mode(this->double_sided);
        // THE INSTANCE COUNT BECOMES THE DISPATCH'S Y: vkCmdDrawMeshTasksEXT has no instanceCount and a mesh stage
        // has no SV_InstanceID, so the stage reads its instance index from the workgroup grid's Y - which is exactly
        // what the vertex path's SV_InstanceID meant here (see shadow.slang's mesh entry).
        push_stage_block(env, this->push);
        uint32_t const survivors = this->push_meshlet_lanes(env, geometry_source, 0u, geometry_source.draw_index_count, 0, this->double_sided);
        this->mesh_dispatch(env, geometry_source, geometry_source.draw_index_count, this->instance_count, survivors);
    }

    void instanced_draw_primitive::destroy([[maybe_unused]] vma_allocator& vma) noexcept {
        // owns nothing: the instance transform buffer is runtime-owned, geometry is source's
    }

    bool instanced_draw_primitive::is_valid() const noexcept {
        return this->source != nullptr && this->source->is_valid() && this->instance_count != 0;
    }

    void static_draw_primitive::draw(render_environment& env) const {
        env.bind_default(); // the bind decides `mesh_stage`, so it happens before the flag is read
        if (!env.mesh_stage) {
            log_meshless_draw_once();
            return;
        }
        env.set_depth_write(!this->transparent);
        // Nothing is BOUND any more: one merged geometry, one dispatch per chunk, and the chunk's window travels in
        // the lanes rather than as vkCmdDrawIndexed's arguments (which is what the vertex path used them for).

        // chunked: per chunk set the cull mode + material_index (push.material_index is the
        // first field, so only that slice needs re-pushing; model stays from the base push).
        // The chunk table is validated when a static draw is built (in-range index windows and
        // vertex references), so no draw can go out of bounds.
        for (chunk_record const& chunk : this->chunks) {
            env.set_cull_mode(chunk.double_sided);
            material_push_constants const chunk_push = [&] {
                material_push_constants p = this->push; // model + flags already correct
                p.material_index = chunk.material_index;
                return p;
            }();
            push_stage_block(env, chunk_push);
            // the cull is the SAME for every chunk of one primitive - the run covers the merged buffer, not the
            // chunk - so the survivors come out identical and the culled table is rewritten with the same records.
            // (That the dispatch happens once per chunk at all is a known defect of the meshlet session, recorded in
            // docs/mesh_shaders.md: a chunk's material cannot reach a whole-run dispatch.)
            uint32_t const survivors = this->push_meshlet_lanes(env, *this, chunk.first_index, chunk.index_count, static_cast<int32_t>(chunk.vertex_offset), chunk.double_sided);
            this->mesh_dispatch(env, *this, chunk.index_count, 1u, survivors);
        }
    }

    void static_draw_primitive::destroy(vma_allocator&) noexcept {
        // owns the merged buffers: dropping the two handles' references frees the GPU memory when the
        // last reference goes (see the note on normal_draw_primitive::destroy above)
        this->vertex_buffer.reset();
        this->index_buffer.reset();
        this->draw_index_count = 0;
        this->vertex_count = 0;
        this->chunks.clear();
    }

    bool static_draw_primitive::is_valid() const noexcept {
        if (!static_cast<bool>(this->vertex_buffer) || !static_cast<bool>(this->index_buffer) || this->chunks.empty()) {
            return false; // a validated, non-empty chunk table is required (see the static-draw builder)
        }
        return std::ranges::all_of(this->chunks, [](chunk_record const& c) { return c.index_count != 0; });
    }

    camera_ubo make_orbit_camera_ubo(
        float const yaw,
        float const pitch,
        float const distance,
        glm::vec3 const& target,
        float const scene_radius,
        float const aspect) {
        // Orbit camera: the eye orbits the target point spherically
        float const cp = std::cos(pitch);
        glm::vec3 const eye(target + glm::vec3(distance * cp * std::sin(yaw),
                                               distance * std::sin(pitch),
                                               distance * cp * std::cos(yaw)));

        // RH_ZO: right-handed + depth [0,1] (Vulkan convention). The far plane always covers the
        // whole scene: the farthest visible point sits at target + scene_radius, i.e. at most
        // distance + scene_radius from the eye, so far >= distance + scene_radius (with margin).
        // Zooming in (small distance) must NOT shrink the far plane below that - a distance-follow
        // far (e.g. 8 * distance) clips the scene's far side exactly when the camera gets close,
        // making objects vanish. Zooming out keeps the historic generous far (max with 8*distance).
        float const far_plane = std::max(100.0f, std::max(distance + 2.0f * scene_radius, 8.0f * distance));
        glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(45.0f), aspect, 0.1f, far_plane);
        // glm's projection follows the OpenGL convention (NDC y up), but Vulkan framebuffers are y-down:
        // flip the projection's Y, otherwise glTF's CCW front-face winding becomes CW in the framebuffer
        // and is culled by the pipeline's CULL_BACK, leaving only the object's interior visible.
        proj[1][1] *= -1.0f;

        camera_ubo ubo;
        ubo.view = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
        ubo.proj = proj;
        ubo.camera_pos = eye;
        return ubo;
    }

    glm::vec3 orbit_camera_pan_delta(
        float const yaw,
        float const distance,
        float const strafe,
        float const rise,
        float const dt,
        bool const fast) {
        // The camera's horizontal frame, taken from the very sphere make_orbit_camera_ubo places the eye on:
        // eye sits at target + distance * (cp*sin yaw, sin pitch, cp*cos yaw), so the right vector is
        // cross(horizontal view direction, world up) = (cos yaw, 0, -sin yaw).
        glm::vec3 const right(std::cos(yaw), 0.0f, -std::sin(yaw));
        // UP/DOWN RISE ALONG WORLD UP, NOT ALONG THE VIEW DIRECTION. The first cut walked the eye along
        // (-sin yaw, 0, -cos yaw) and was rejected by the user the moment it shipped: moving the eye toward
        // the subject changes its apparent SIZE, so the arrow keys read as a zoom rather than as an up/down
        // slide. World up keeps the two gestures distinct at every pitch; y is exactly 0 unless UP/DOWN is
        // held, which is also what keeps the horizontal keys' output byte-identical to the first cut's.
        glm::vec3 const up(0.0f, 1.0f, 0.0f);

        // Normalize the two axes TOGETHER: a diagonal press must not move sqrt(2) times faster than a
        // straight one (the two axes are orthogonal, so this is sqrt(strafe^2 + rise^2)). No key held is
        // exactly zero, so an idle frame does not touch the camera at all.
        glm::vec3 const direction = right * strafe + up * rise;
        float const length = glm::length(direction);
        if (length <= 0.0f) {
            return glm::vec3(0.0f);
        }

        // Speed scales with the orbit distance (an orbit twice as far away pans twice as fast, so the
        // on-screen rate is the same at any zoom) with a floor for a fully zoomed-in rig, and SHIFT multiplies
        // it. Artistic units, not physical: 0.75 of the distance per second crosses a framed scene in ~1.3 s.
        constexpr float pan_speed_per_distance = 0.75f;
        constexpr float pan_speed_floor = 0.1f;
        constexpr float fast_multiplier = 4.0f;
        constexpr float max_step_seconds = 0.25f;
        float const speed = std::max(pan_speed_floor, distance) * pan_speed_per_distance * (fast ? fast_multiplier : 1.0f);

        // Clamp the step: a stalled frame (breakpoint, swapchain recreation, the first frame's unset clock)
        // must not teleport the camera across the scene.
        float const step = std::clamp(dt, 0.0f, max_step_seconds) * speed;
        return direction / length * step;
    }

    light_ubo make_directional_light_ubo(glm::vec3 const& sun_direction, glm::vec3 const& scene_center, float const scene_radius, float const shadow_map_size) {
        // THE DIRECTION IS AN INPUT, NOT A CONSTANT, and it has to be ONE input for three consumers: this
        // UBO's matrices, the shader's `light_dir`, and the sun the sky draws - `sky.glsl` reads this block's
        // `light_dir`, so a light that moved here and not there would light a scene from one side while the
        // visible sun sat on the other. It comes from `[lighting] sun_direction`.
        // light_dir points TOWARD the sun in the sky (pbr.frag treats it as the surface-to-light
        // vector), so the sun's rays travel -light_dir and the shadow camera must sit UP-SUN.
        glm::vec3 const light_dir = glm::normalize(sun_direction);

        // Orthographic shadow frustum framing the scene's bounding sphere:
        //  - the light sits up-sun at scene_center + dir * 2r (above the scene for a sky sun),
        //    looking down along -dir at scene_center, matching the rays of the real sun
        //  - the sphere spans [r, 3r] along the light direction (center at 2r), so the near/far
        //    planes with a margin around it cover every caster
        //  - the ortho box half-extent is the sphere radius (plus margin): any point of the
        //    sphere projects within it, so nothing casts outside the shadow map
        float const r = scene_radius;
        glm::vec3 const eye = scene_center + light_dir * (2.0f * r);
        glm::mat4 const view = glm::lookAt(eye, scene_center, glm::vec3(0.0f, 1.0f, 0.0f));

        float const half = r * 1.1f;
        glm::mat4 proj = glm::orthoRH_ZO(-half, half, -half, half, r * 0.5f, r * 3.5f);
        // Same y-flip convention as the camera projection (see make_orbit_camera_ubo): Vulkan
        // framebuffers are y-down, so the light view-proj must flip Y too, otherwise the shadow
        // pass renders the scene mirrored and the sampled shadow UVs would not match it.
        proj[1][1] *= -1.0f;

        light_ubo ubo;
        // Default: ONE cascade covering the whole scene sphere - the fit runtime::update_shadow_frustum
        // replaces on the first frame (and the cascaded version fills the same slots with one fitted
        // matrix per cascade). Every lane gets the same matrix here so a frame that renders before the
        // first fit still samples something sound.
        for (auto& matrix : ubo.light_view_proj) {
            matrix = proj * view;
        }
        ubo.light_dir = glm::vec4(light_dir, 1.0f / shadow_map_size); // w: uv texel size for the pcf taps
        ubo.shadow_enabled = 1.0f;                                    // shadows on by default; runtime::set_shadow_enabled flips it
        ubo.brdf_model = 0.0f;                                        // defaults: GGX + joint Smith, Lambert (see light_ubo docs)
        ubo.diffuse_model = 0.0f;
        float const default_texel_world = (2.0f * half) / shadow_map_size;
        ubo.cascade_texel_world = glm::vec4(default_texel_world); // world size of one shadow texel
        ubo.cascade_count = 1.0f;
        return ubo;
    }
} // namespace deren::vulkan
