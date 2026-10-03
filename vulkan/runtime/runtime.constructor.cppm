// ============================================================================
// module: deren.vulkan.runtime:constructor  - construction, initialisation and resource creation
//
// The two contiguous runs of runtime.cpp that BUILD a runtime rather than drive one: the constructors
// and the whole init/ensure run (init_scene_resources through ensure_scene_heap_slots), then
// write_light_and_shadow_bindings, set_ibl and register_material. What stayed behind runs per frame or
// is a diagnostic (the two heap probes, which get their own partition).
//
// THE GLFW CALLBACKS CAME WITH THE CONSTRUCTOR because the constructor is what registers them; the
// window lookup they use is the same three-line helper runtime.cpp keeps. The heap-write helpers that
// sit inside the init run (heap_slot_offset, write_heap_grid_image, write_heap_scene_buffer) came too,
// and runtime.cpp keeps a copy of the ones its own frame path still calls. Duplication of small
// file-local helpers across two anonymous namespaces is legal but it IS duplication: the follow-up is to
// publish the heap three from deren.vulkan.core:descriptor_heap, where heap plumbing belongs.
//
// Imports are NOT transitive: this partition imports what the moved code calls, and repeats the pmr
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

module deren.vulkan.runtime:constructor;

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
import deren.vulkan.meshlet;         // the meshlet table's record layout and capacity (docs/mesh_shaders.md step 3)
import deren.promise.rhi;            // the RHI creation contract: the type the new constructor below takes

// Route std::pmr allocations through mimalloc for this TU (deren.utility:better_pmr). Idempotent:
// init_pmr() returns the same process-wide singleton no matter which TU calls it first, so
// main.cpp's keep-alive and this one coexist safely. The reference itself is never read; it
// only forces the (dynamic) initialization before any pmr container in this TU is constructed.
[[maybe_unused]] static auto& pmr = deren::utility::init_pmr(); // NOLINT(keep-alive)

namespace {
    deren::vulkan::runtime* runtime_from_window(GLFWwindow* window) {
        return static_cast<deren::vulkan::runtime*>(glfwGetWindowUserPointer(window));
    }

    void mouse_button_callback(GLFWwindow* window, int32_t const button, int32_t const action, [[maybe_unused]] int32_t const mods) {
        auto* runtime = runtime_from_window(window);
        if (button != GLFW_MOUSE_BUTTON_LEFT) {
            return;
        }
        // the overlay owns the mouse while the cursor is over a panel / a widget is being
        // dragged: starting an orbit there would fight the ui (see gui_content::wants_mouse)
        if (runtime->debug_gui_wants_mouse()) {
            return;
        }
        if (action == GLFW_PRESS) {
            runtime->camera.dragging = true;
            glfwGetCursorPos(window, &runtime->camera.last_x, &runtime->camera.last_y);
        } else if (action == GLFW_RELEASE) {
            runtime->camera.dragging = false;
        }
    }

    void cursor_pos_callback(GLFWwindow* window, double const x, double const y) {
        auto& camera = runtime_from_window(window)->camera;
        if (!camera.dragging) {
            return;
        }
        if (runtime_from_window(window)->debug_gui_wants_mouse()) {
            // the drag left the scene and landed on the overlay: keep the camera still but keep
            // tracking the cursor so leaving the panel does not jump the view
            glfwGetCursorPos(window, &camera.last_x, &camera.last_y);
            return;
        }
        constexpr float sensitivity = 0.005f;
        float const dx = static_cast<float>(x - camera.last_x);
        float const dy = static_cast<float>(y - camera.last_y);
        camera.last_x = x;
        camera.last_y = y;
        camera.yaw += dx * sensitivity; // drag direction matches the primitive rotation
        camera.pitch -= dy * sensitivity;
        camera.pitch = std::clamp(camera.pitch, -1.5f, 1.5f); // avoid flipping
    }

    void scroll_callback(GLFWwindow* window, [[maybe_unused]] double const xoffset, double const yoffset) {
        if (runtime_from_window(window)->debug_gui_wants_mouse()) {
            return; // scrolling inside an overlay panel must not zoom the camera
        }
        auto& camera = runtime_from_window(window)->camera;
        // zoom: wheel up pulls in, wheel down pulls out. The upper bound is generous (scene
        // sizes vary from the tiny default model to e.g. the Fox rig, whose framing distance
        // is ~240). The projection far plane always covers the scene (see make_orbit_camera_ubo),
        // so zooming in never clips the far side.
        camera.distance *= std::pow(0.9f, static_cast<float>(yoffset));
        camera.distance = std::clamp(camera.distance, 0.5f, 5000.0f);
    }
} // namespace

namespace deren::vulkan {
    // Run a batch of tasks on the shared pool and wait for exactly this stage's group: the
    // frame phases are synchronous (the paced slot is read right after animation sampling),
    // so run_tasks blocks until every task in the batch finished. The enum tier is mapped
    // onto the pool's integer priority (see task_priority in runtime.cppm).
    void runtime::run_tasks(std::span<std::function<void()>> const tasks, task_priority const priority) {
        int32_t const pool_priority = static_cast<int32_t>(priority);
        if (tasks.empty() || !this->task_pool.post_batch(tasks, pool_priority)) {
            return; // empty batch, or the pool is shut down (never in the running demo)
        }
        this->task_pool.wait_until_priority_done(pool_priority);
    }

    runtime::runtime()
        : runtime(deren::promise::rhi::create_info{}) {
    }

    // THE ONE CREATION CONSTRUCTOR: the contract's structure goes straight to `core`, so the runtime
    // cannot tell where its creation parameters came from - the program's startup config, a test, or
    // (once the flip lands) the same structure handed to `deren_make_api_core()`.
    runtime::runtime(deren::promise::rhi::create_info const& options)
        : runtime(std::make_shared<core>(options)) {
        // The device root is built here and owned by this runtime; the constructor below is the one that does the
        // work, so a caller that ALREADY has a core takes the same path (see its doc note).
    }

    runtime::runtime(std::shared_ptr<core> shared_core)
        : core_owner{std::move(shared_core)}
        , vulkan_core{*this->core_owner}
        , filtered_core{core_owner}
        , pass_resources{core_owner} {
        // ---- THE ONE PLACE GLFW CALLBACKS ARE INSTALLED, and it stays here now that the WINDOW belongs to
        //      the APPLICATION: the callbacks dereference THIS runtime through the window's user pointer, so
        //      they can only be installed once both exist - which is exactly this constructor, running after
        //      the caller created the window and handed it over. The application installs no GLFW callback
        //      and must not touch the user pointer (it is this object's slot). The debug overlay's ImGui
        //      backend chains onto whatever is installed here (ImGui_ImplGlfw_InitForVulkan with
        //      install_callbacks = true, later), so the ORDER - orbit camera first, ImGui after - must not
        //      move.
        glfwSetWindowUserPointer(this->vulkan_core.window, this);
        glfwSetMouseButtonCallback(this->vulkan_core.window, mouse_button_callback);
        glfwSetCursorPosCallback(this->vulkan_core.window, cursor_pos_callback);
        glfwSetScrollCallback(this->vulkan_core.window, scroll_callback);

        // The recording resources - one primary command buffer per frame slot, plus the secondary
        // buffers (and the per-consumer pools that must own them) the parallel recording stages hand
        // out - are built by init_recording_resources(): they depend on nothing else in this
        // constructor but the two capacities, and nothing else here reads them.
        this->init_recording_resources();

        // Shared scene resources: camera UBO buffers, white fallback texture, texture sampler
        this->init_scene_resources();
        // Every per-image flag that describes this generation starts where the generation's images do.
        // The core has already built this generation's targets (its constructor ran
        // create_hdr_resolve_resources), so the flags can be sized HERE, before any frame records; every
        // later generation gets the very same reset from on_swapchain_recreated - one function, so the
        // two lists cannot drift (which they already had).
        this->reset_image_generation_state();
        // (The furnace cube is a new image too - its level is part of the generation reset above.)
        // NOTE: the shadow resources (map layers + light UBO buffers) are created LAZILY, by
        // ensure_shadow_resources() from ensure_scene_heap_slots(). The shadow map is a layered 2D array
        // whose layer count is [render] shadow_cascades, and the app config that carries it is applied
        // after this constructor returns - creating them here would freeze the count at its default.
        // Everything between here and the first scene set works with them empty (the light-buffer
        // writes are guarded, and nothing samples the shadow map before a scene set exists).
    }

    // The destructor body runs before member destruction, so vulkan_core (and the VkDevice it
    // holds) is still alive here: destroying cached pipelines in this order is guaranteed safe,
    // independent of future member reordering. Members then destruct in reverse declaration
    // order with pipelines already empty. The SCENE TREE is caller-owned (set_scene): the caller
    // destroys it before this runtime goes away (its leaves release GPU buffers through the vma
    // allocator while it is still alive), so no tree teardown happens here.
    runtime::~runtime() {
        this->vulkan_core.vma.log_statistics();
        // Wait for the GPU to finish BEFORE releasing anything below: the last submitted frame
        // may still be executing and destroying in-use resources would violate VUIDs (~core()
        // also waits, but that runs after this body — too late for the VMA frees here).
        this->vulkan_core.wait_idle();

        this->pipelines.clear();
        // THE SESSION'S CULLING TOTALS, printed HERE rather than per frame: the counters are cumulative, and this is
        // the one point where the GPU has finished (wait_idle above) and the numbers are final - so a run of any
        // length reports exactly what the meshlet path did, without a read-back inside the frame loop.
        this->log_meshlet_stats();

        // post-process objects: the FXAA pipeline and the two samplers are RAII members, and the post chain's
        // two pipelines belong to the post composite PASS (vulkan.pass.post::release_owned), the same rule
        // every extracted pass follows.
        //
        // NO SET LAYOUT, PIPELINE LAYOUT OR DESCRIPTOR POOL IS TORN DOWN HERE ANY MORE, and that is the whole
        // point of the deletion this destructor records: every stage is heap-native, so there is nothing of that
        // kind left in this class to destroy - no scene set layout (it was `core`'s), no post or G-buffer set
        // layout (this class created them for the families it wrote), no pool and no per-image family. What
        // remains of each extracted pass's GPU material is the PASS's, released by its own destructor.

        // Shared scene resources: views/samplers/buffers/images are RAII and free
        // themselves as this runtime's members destruct (after this body; vulkan_core, which
        // owns the vma allocator, is declared first and destructs last, so every vk_buffer /
        // vk_image still has a live allocator when it releases).

        // Shut the debug overlay down explicitly while the VkDevice is still alive (its ImGui
        // Vulkan backend owns device resources); member destruction would also run it before
        // vulkan_core, but doing it here keeps the order obvious.
        this->debug_overlay.shutdown();
    }

    // THE SESSION'S MESH CULLING TOTALS (docs/mesh_shaders.md step 3, "what the culling buys"). Read from the
    // mapped counter buffer after `wait_idle`: the sums are per session, and the two DERIVED numbers are the point
    // of the line - the share of workgroups the frustum test rejected, and what that share means for a compute pass
    // (a rejected meshlet costs a workgroup launch today, and would cost nothing at all if the culling happened
    // before the dispatch).
    void runtime::log_meshlet_stats() const {
        if (this->meshlet_stats_mapped == nullptr) {
            return;
        }
        auto const* const counters = static_cast<uint32_t const*>(this->meshlet_stats_mapped);
        uint64_t total[8] = {};
        for (int32_t slot = 0; slot < deren::vulkan::core::MAX_FRAMES_IN_FLIGHT; ++slot) {
            for (uint32_t counter = 0; counter < 8u; ++counter) {
                total[counter] += counters[static_cast<std::size_t>(slot) * 8u + counter];
            }
        }
        uint64_t const workgroups = total[0];
        uint64_t const culled = total[1];
        uint64_t const emitted = total[2];
        uint64_t const triangles = total[3];
        uint64_t const mesh_workgroups = total[4];
        if (workgroups == 0u && mesh_workgroups == 0u) {
            return; // no mesh stage ran: a vertex-path session says nothing rather than reporting zeroes
        }
        deren::utility::log("mesh culling: {} meshlet workgroups, {} emitted their triangles, {} were culled ({}% of the workgroups emitted nothing), {} triangles from meshlets",
                            workgroups,
                            emitted,
                            culled,
                            workgroups == 0u ? 0u : (culled * 100u) / workgroups,
                            triangles);
        deren::utility::log("mesh culling: {} workgroups of the NON-meshlet mesh path ran too, so this session's geometry cost {} workgroup launches either way - a compute pass that culled first would record {} fewer",
                            mesh_workgroups,
                            workgroups + mesh_workgroups,
                            culled);
    }

    void runtime::init_scene_resources() {
        // Camera UBO: one buffer per frame slot, mapped for direct writes; all models reference
        // these buffers through the shared scene block, so one memcpy per frame replaces the old
        // per-primitive per-frame UBO updates
        //
        // THE BUFFER IS USABLE AS A STORAGE BUFFER TOO, because the heap descriptor written for it is a
        // VK_DESCRIPTOR_TYPE_STORAGE_BUFFER (see write_heap_scene_buffer's note below): a descriptor's type has
        // to be backed by the matching USAGE bit on the buffer, and the validation layer says so out loud -
        // "vkWriteResourceDescriptorsEXT(): ... has no buffer(s) associated that are valid" - while the render
        // itself still produced the right picture, which is exactly the kind of finding this project treats as
        // a failure. UNIFORM_BUFFER stays set: the buffer's allocation policy (host-visible, coherent) is what
        // the buffer_type names, and both bits are legal together.
        camera_ubo initial = {};
        create_buffers(this->vulkan_core,
                       this->camera_buffers,
                       rhi::buffer_usage::uniform_coherent,
                       rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::storage), // heap-bound: see the scene-set block
                       std::as_bytes(std::span(&initial, 1)),
                       "camera ubo buffer",
                       &this->camera_mapped);

        // 1x1 white fallback texture, always the first entry of the scene texture array; missing
        // material textures point at it
        constexpr std::array<uint8_t, 4> white_pixels = {255, 255, 255, 255};
        deren::vulkan::image_create_info white_info = {};
        white_info.width = 1;
        white_info.height = 1;
        white_info.mip_levels = 1;
        white_info.array_layers = 1;
        white_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        init_utils::texture_2d white = init_utils::create_texture_2d(this->vulkan_core, std::as_bytes(std::span(white_pixels)), white_info, "white fallback texture");
        this->owned_textures.push_back(std::move(white.image));
        this->owned_texture_views.push_back(std::move(white.view));
        this->white_texture_index = static_cast<uint32_t>(this->texture_array_views.size());
        this->texture_array_views.push_back(*this->owned_texture_views.back());

        // THE WHITE ELEMENT NEEDS ITS OWN HEAP DESCRIPTOR HERE, and its absence was a class of black frames.
        // Every texture that reaches the bindless array through register_material has its heap slot written
        // there, but this one is created above that loop and never passes through it - so slot
        // heap_slots::textures + 0 stayed EMPTY and sampled as zero. A material slot with no texture arrives
        // here (Sponza's stone carries no occlusion map), so its ao read 0 - and shading.glsl multiplies BOTH
        // the diffuse ambient and the specular IBL by s.ao, which left those surfaces lit by the sun alone:
        // a black interior, while the metal test assets (whose materials do carry an occlusion map, and whose
        // diffuse term is multiplied away by (1 - metallic) anyway) looked untouched. The startup probe had
        // it in the log the whole time: "the heap-native probe sampled grid slot 16384 ... read back
        // 0x00000000", on the very slot this write fills, while the material table's white record read 0xffff.
        // @note core::heap_slot_offset() is defined below this constructor, so the arithmetic is spelled out: a slot
        //       number is already absolute and the stride is the one every heap array agrees on.
        if (this->vulkan_core.descriptor_heaps.ready()) {
            auto const* const white_detail = this->vulkan_core.vma.get_image_detail(this->owned_textures.back().handle());
            if (white_detail != nullptr) {
                VkImageViewCreateInfo const heap_view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                                         .pNext = nullptr,
                                                         .flags = 0,
                                                         .image = white_detail->image,
                                                         .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                                         .format = VK_FORMAT_R8G8B8A8_UNORM,
                                                         .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
                VkDeviceSize const white_offset = static_cast<VkDeviceSize>(core::heap_slots::textures + this->white_texture_index) * core::heap_slot_stride;
                if (!this->vulkan_core.descriptor_heaps.write_image(white_offset, heap_view, VK_IMAGE_LAYOUT_GENERAL)) {
                    deren::utility::log("descriptor heap: the white fallback texture did not reach grid slot {}", core::heap_slots::textures + this->white_texture_index);
                }
            }
        }

        // (The sampler the array entries are read through is NOT created here: the maxLod-12 REPEAT
        // sampler the comment that used to sit here described moved into the core, next to the other six
        // - see core::create_samplers / shared_samplers.) The white element above is the one entry this
        // function has to place, because every later texture index is assigned around it.

        // GPU material table: fixed capacity, host-visible (direct mapping); records are appended
        // at registration and read-only for the GPU (set 0 binding 5)
        std::vector<uint8_t> const zeroed_materials(static_cast<size_t>(deren::vulkan::material_capacity) * sizeof(material_record), 0);
        create_buffer(this->vulkan_core,
                      rhi::buffer_usage::storage_coherent,
                      // it goes on the descriptor heap, and a heap descriptor for a buffer is an
                      // ADDRESS RANGE - so this buffer needs a device address
                      rhi::to_bits(rhi::buffer_flag::device_address),
                      std::as_bytes(std::span(zeroed_materials)),
                      "material table buffer",
                      this->material_buffer,
                      &this->material_mapped);

        // ---- THE MATERIAL TABLE'S HEAP DESCRIPTOR, written ONCE here ----
        //
        // A storage buffer descriptor is just an address range, so this is the simplest descriptor in the
        // renderer: no image view to create, no sampler, no embedded sampler. The buffer has a FIXED capacity and
        // is created above, so its address is stable and one write covers it - which is why this is not a
        // per-frame write. It goes at its OWN GRID SLOT (core::heap_slots::materials), which is the same number a
        // heap-native shader bakes as `heap_slots_materials` (shaders/heap_slots.glsl): the write and the read are
        // the same number by construction rather than by review.
        if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
            VkDeviceAddress const address = this->buffer_address(*this->material_buffer);
            bool const written = this->write_heap_buffer(*this->material_buffer,
                                                         core::heap_slots::materials,
                                                         static_cast<VkDeviceSize>(deren::vulkan::material_capacity) * sizeof(material_record),
                                                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            deren::utility::log("descriptor heap: material table {} (address 0x{:x}, {} records, offset {})",
                                written ? "written" : "NOT written",
                                address,
                                deren::vulkan::material_capacity,
                                static_cast<VkDeviceSize>(core::heap_slots::materials) * core::heap_slot_stride);
        }

        // ---- THE MATERIAL COLOURS: ONE `vec4` PER LANE PER MATERIAL, WHITE to begin with ----
        //
        // THE NEUTRAL IS THE INITIAL CONTENT and not a placeholder (see `toon_colour_lane`): every lane multiplies
        // or tints something, so a material whose sidecar states no colour must leave that thing alone - and
        // `register_material` overwrites the lanes it was given, per material, at the index the shader addresses.
        // THREE LANES ARE NOT WHITE, and all three are stated below: the outline lane's width (0, not 1) and the
        // TWO scalar lanes' sentinel (-1, not 1) - the specular strength and the parallax depth.
        //
        // A BUFFER OF FLOATS, so the fill below writes 1.0f as a FLOAT - not 0xFF, which is one in a UNORM texture
        // and 0.0 in this. That distinction is the whole reason a colour lane could not reuse the texture lanes.
        {
            std::vector<glm::vec4> neutral_colours(static_cast<size_t>(deren::vulkan::material_capacity) * static_cast<size_t>(deren::vulkan::toon_colour_lane::count), glm::vec4(1.0f));
            // ... EXCEPT THE OUTLINE LANE'S WIDTH, WHICH IS 0.0 AND NOT 1.0 (`toon_inputs::colours` states the
            // whole argument): three of that lane's four floats are the `_OutlineTintColor` tint, whose neutral is
            // white, and the fourth is `_OutlineWidth`, whose neutral is ZERO - a width of 1.0 would draw a hull
            // around every material that states no outline, i.e. the entire character. The layout is
            // MATERIAL-major (`material_index * toon_colour_lane::count + lane`, see `register_material`), so the
            // lane to reach is every `count`-th element.
            for (size_t material = 0; material < static_cast<size_t>(deren::vulkan::material_capacity); ++material) {
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::outline_edge)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);
                // ... AND THE TWO SCALAR LANES' SENTINEL IS `-1.0` RATHER THAN 1.0, for the same kind of reason:
                // neither `.x` is a tint but a STRENGTH (`_Specular`) or a DEPTH (`_ParallaxScale`), whose "no
                // statement" cannot be a number in the value's own range (see `toon_colour_lane::specular_strength`
                // and `toon_colour_lane::parallax_scale`). A table left at 1.0 would shade every material that
                // states no `_Specular` with a full-strength highlight, and would push every material that states
                // no `_ParallaxScale` to a parallax offset thirty-three times the one the stage's own constant
                // gives - which is the same failure from the other side.
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::specular_strength)] =
                    glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::parallax_scale)] =
                    glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
                // ... AND THE GOO IRIS BRIGHTNESS LANE'S TWO COMPONENTS ARE BOTH SENTINELS, because both are
                // BRIGHTNESSES (`Eyes brightness` / `Eyes HightLight brightness`): `0.0` is a value the reference
                // really uses - it is what the group's own interface defaults the two sockets to - so it cannot
                // mean "absent", and anything else in range would be a brightness this port made up. The stage
                // tests each component separately and answers with that interface default (see
                // `toon_colour_lane::goo_eye_brightness`).
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_eye_brightness)] =
                    glm::vec4(-1.0f, -1.0f, 0.0f, 0.0f);
                // ... AND THE TWO REWRITTEN CHAIN'S RIM LANES, whose neutrals are the REFERENCE'S OWN interface
                // defaults rather than one convention (see `toon_colour_lane::goo_rim_colour` /
                // `goo_rim_scalars`): the tint's neutral is the `Rim_Color` sub-group's own `[1,1,1,1]`, and the
                // four scalars are ALL sentinels because every one of them is a value the reference really uses
                // at zero - `Rim_ColorStrength = 0.0` is how its author switches a rim off
                // (`M_actor_laevat_cloth_03`), and `Use Rimlimitation?` is a BOOLEAN whose default is 0.0. The
                // stage resolves each of the four separately to that socket's group default (see the shader).
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_rim_colour)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_rim_scalars)] =
                    glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f);
                // ... AND THE SCREEN-SPACE RIM'S TWO WIDTHS ARE BOTH SENTINELS TOO, because a width has no no-op
                // number either: `0.0` is a width the reference's author really states (it collapses the offset
                // sample onto the pixel and the depth difference is then exactly 0 - a rim that is off ON PURPOSE,
                // material by material), so it cannot mean "absent". The stage answers each component with the
                // `DepthRim` group's OWN interface default, `0.5` (see `toon_colour_lane::goo_rim_widths`, and
                // `gooblender/nodes.json`'s `meta.node_groups[DepthRim].interface[]`).
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_rim_widths)] =
                    glm::vec4(-1.0f, -1.0f, -1.0f, -1.0f);
                // ... AND STEP 4'S SIX LANES, whose NEUTRALS ARE TWO DIFFERENT SHAPES FOR THE SAME REASON THE RIM
                // LANES ABOVE ALREADY GAVE: two of them ARE the reference's own interface defaults
                // (`BaseColor` = white, `directOcclusionColor` = black) and the other four are sentinels. THEIR
                // SENTINEL IS `-1000` AND NOT `-1`, because two of the eight per-material numbers behind them are
                // AUTHORED NEGATIVES - `CastShadow_center` is `-0.1` on both body materials and
                // `GlobalShadowBrightnessAdjustment` is `-1.8` on the cloth - and a `-1` neutral would make the
                // stage read those authored values as "not stated" (see `goo_lane_absent` in the shader).
                // The stage resolves each sentineled component to its group's default - see
                // `toon_colour_lane::goo_base_colour` .. `goo_direct_occlusion` and `shaders/goo_toon.slang`.
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_base_colour)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_direct_occlusion)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                for (uint32_t lane : {static_cast<uint32_t>(deren::vulkan::toon_colour_lane::goo_diffuse_a),
                                      static_cast<uint32_t>(deren::vulkan::toon_colour_lane::goo_diffuse_b),
                                      static_cast<uint32_t>(deren::vulkan::toon_colour_lane::goo_fresnel_inside),
                                      static_cast<uint32_t>(deren::vulkan::toon_colour_lane::goo_fresnel_outside)}) {
                    neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + lane] =
                        glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f);
                }
                // ... AND STEP 5'S THREE LANES, whose neutrals are the reference's own `interface[]` defaults and
                // NOT the `-1000` sentinel the four above use, because none of these three numbers is ever negative
                // in the reference's asset: `specularFGD Strength` is `0.8` or `1.0`, `dirLight_lightColor` is
                // `(1, 0.958..., 0.958...)` and `AmbientLightColorTint` is white or `(1.512...)`. So the CHEAPER
                // `< 0` contract is enough here - and the reason step 4's four needed `-1000` is exactly that two
                // of THEIR eight numbers are authored negatives (`CastShadow_center` = `-0.1`,
                // `GlobalShadowBrightnessAdjustment` = `-1.8`). The scalar lane keeps its sentinel in `.x` only
                // (the stage resolves it to the reference's `1.0`); the two colour lanes are their own fallback.
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_specular_fgd)] =
                    glm::vec4(-1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_light_color)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_ambient_tint)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_specular_color)] =
                    glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
                // ... AND STEP 7'S FOUR, WHOSE NEUTRALS ARE THREE DIFFERENT SHAPES AGAIN, each one the socket's own
                // `interface[]` default rather than a convention:
                //
                //   * THE TWO SCALAR LANES are `-1000` sentinels, like step 4's four and for the same reason: the
                //     chin pair, `sphereNormal_Strength` and the two brightnesses are all positive on every
                //     material this container is given, but `SmoothnessMax` is a value whose ZERO is meaningful
                //     (perfectly rough) - so a neutral of `0` would read "the material states no smoothness" as "the
                //     material is rough", and the stage would then have to invent one. `-1000` is outside every one
                //     of their domains, so a stated value - any sign - passes and only the lane itself fails.
                //   * `nose_shadow_Color` AND `Front R Color` ARE BLACK, because that is what the FACE container's
                //     own interface says (`::- ... :: 组输入.nose_shadow_Color = [0.0, 0.0, 0.0, 1.0]`,
                //     `Front R Color = [0.0, 0.0, 0.0, 1.0]`) and because WHITE WOULD BE A STATEMENT: the nose
                //     shadow is a MIX's A side (white = no shadow at all, the strongest possible statement) and
                //     `Front R`'s colour multiplies a term that is otherwise the albedo itself. A material that
                //     states neither row therefore gets the reference's own defaults, which is what a material
                //     calling the group without stating them gets in Goo.
                for (uint32_t lane : {static_cast<uint32_t>(deren::vulkan::toon_colour_lane::goo_face_scalars_a),
                                      static_cast<uint32_t>(deren::vulkan::toon_colour_lane::goo_face_scalars_b)}) {
                    neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + lane] =
                        glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f);
                }
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_face_nose_shadow)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_face_front_r)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
                // STEP 8'S ONE LANE IS A `-1000` SENTINEL AND NOT THE REFERENCE'S GROUP DEFAULT `1.0`, because the
                // lane does not carry a tint or a factor that has a neutral - it carries the SWITCH that decides
                // whether the Goo chain's normal decode runs at all. A material with no row therefore keeps the
                // normal the chain had before this step, which is what `laevatain_no_sidecar` and the old chain's
                // frames are pinned on; `NormalStrength = 0` is a value the reference states on some materials, so
                // `0` could not be the "not stated" answer (see `goo_normal_strength`).
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_normal_strength)] =
                    glm::vec4(-1000.0f, -1000.0f, -1000.0f, -1000.0f);
                // STEP 10'S ONE IS `(0, 0, 0, 0)`, AND HERE - UNLIKE EVERY OTHER LANE ABOVE - THE NEUTRAL IS
                // DOING TWO JOBS AT ONCE. It IS the reference's own group default, so a material that states no
                // `_GooAnisoGate` row gets the answer the graph gives a caller that states nothing
                // (`ng[2].interface[3]` `Use anisotropy?` = `0.0`, `interface[45]` `Anisotropic mask` = `0.0`,
                // `interface[4]` `Use Toonaniso?` = `0.0`); AND it is the OVERRIDE that keeps that true, because
                // this table starts EVERY lane at `glm::vec4(1.0f)` (the constructor's first line), so an
                // unoverridden lane 25 would hand every such material `Use anisotropy? = 1.0` - the arm the
                // reference does not take - instead of the `0.0` it does. `.z` is recorded and unused, `.w`
                // reserved; keep the spelling of this initialiser and the two hosts' tables in step, because
                // `tests/test_goo_toon_math.cpp` pins all three.
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_aniso_gate)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                // STEP 12'S ONE IS `(0, 0, 0, 0)` TOO, AND THE TWO JOBS IT DOES ARE STEP 10'S OWN: it IS the
                // reference's group default (`ng[2].interface[20]` `Aniso_SmoothnessMaxT` = `0.0`,
                // `interface[21]` `Aniso_SmoothnessMaxB` = `0.0`, so a material with no `_GooAnisoRough` row gets
                // `rT = rB = 1` - the anisotropic lobe at full roughness, which is what the graph answers a
                // caller that states nothing); AND it is the override that keeps that true, because this table
                // starts EVERY lane at `glm::vec4(1.0f)`, so an unoverridden lane 26 would hand every rowless
                // material `rT = (1 - 1)^2 = 0` - a mirror - instead of the `0.0` the graph means. `.z` / `.w`
                // are reserved. Keep the spelling of this initialiser and the two hosts' tables in step, because
                // `tests/test_goo_toon_math.cpp` pins all three. See `toon_colour_lane::goo_aniso_rough`.
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_aniso_rough)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                // STEP 13'S TWO ARE `(0, 0, 0, 0)` AS WELL, and unlike the two above them the value is not the
                // reference's group default but the BRANCH'S OWN SWITCH: `_GooRSScalars.x` is `Use RS_Eff?`, so a
                // material whose sidecar states no `_GooRSScalars` row has to read `Use = 0` and the stage's gate
                // then leaves its colour untouched. Lane 28's `.w` is the mask's `SmoothStep.max`, where zero
                // means "unstated" rather than a zero-width window - the stage maps it to `1.0` so that this
                // neutral cannot divide by zero (see `toon_colour_lane::goo_rs_tint`, spec U7). Keep the spelling
                // of this initialiser and the two hosts' tables in step, because `tests/test_goo_toon_math.cpp`
                // pins all three.
                //
                // THEY ARE DEFENSIVE RATHER THAN LOAD-BEARING, AND THE COMMENT USED TO SAY OTHERWISE ELSEWHERE:
                // these are the BUFFER'S INITIAL CONTENT. `register_material` writes every one of the
                // `toon_colour_lane::count` lanes of every material it registers from `info.toon.colours` (the
                // loop that ends this table's use of `neutral_colours` - see the `colours[lane] =
                // info.toon.colours[lane]` loop in that function), and the host's `toon_colour_neutral` in
                // `main.cpp` is what answers a row that is absent. So this entry is not what makes the twenty
                // `Use RS_Eff? = 0` materials unchanged; it is the honest statement of the neutral plus a guard
                // against a future consumer that reads the buffer before any material is registered.
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_rs_scalars)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_rs_tint)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
                // STEP 15'S ONE IS `(0, 0, 0, 0)` FOR BOTH OF STEP 13'S REASONS, and both apply here verbatim: all
                // four `armA` sockets default to `0.0` in the reference's `interface[]` (so a material stating no
                // `_GooRSArm0` row reads what the graph gives a caller that states nothing), AND the value is the
                // arm's own off switch, because `.y` is `RS Strength` and `s023 = s036 * RS Strength` zeroes the
                // arm. It is ALSO the override that keeps the first statement true: this table starts every lane at
                // `glm::vec4(1.0f)`, so an unoverridden lane 29 would hand every rowless material `RS Strength = 1`
                // with `Layer weight Value = 1` - a `u` of `0.5` on sheet 0 for every material in the scene. Keep
                // the spelling of this initialiser and the two hosts' tables in step, because
                // `tests/test_goo_toon_math.cpp` pins all three. See `toon_colour_lane::goo_rs_arm0`.
                neutral_colours[material * static_cast<size_t>(deren::vulkan::toon_colour_lane::count) + static_cast<size_t>(deren::vulkan::toon_colour_lane::goo_rs_arm0)] =
                    glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
            }
            create_buffer(this->vulkan_core,
                          rhi::buffer_usage::storage_coherent,
                          rhi::to_bits(rhi::buffer_flag::device_address),
                          std::as_bytes(std::span(neutral_colours)),
                          "toon colour table buffer",
                          this->toon_colour_buffer,
                          &this->toon_colour_mapped);
            if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
                VkDeviceAddress const address = this->buffer_address(*this->toon_colour_buffer);
                bool const written = this->write_heap_buffer(*this->toon_colour_buffer,
                                                             core::heap_slots::toon_colours,
                                                             std::as_bytes(std::span(neutral_colours)).size(),
                                                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                deren::utility::log("descriptor heap: toon colour table {} (address 0x{:x}, {} lanes x {} materials, offset {})",
                                    written ? "written" : "NOT written",
                                    address,
                                    static_cast<uint32_t>(deren::vulkan::toon_colour_lane::count),
                                    deren::vulkan::material_capacity,
                                    static_cast<VkDeviceSize>(core::heap_slots::toon_colours) * core::heap_slot_stride);
            }
        }

        // ---- THE TOON LANES BESIDE THE RECORD: ONE `uvec4` PER MATERIAL, and a buffer of its own because the
        //      material record cannot hold them - see core::heap_slots::toon_lanes for the measurement that
        //      settled that (the record is INLINE in the per-draw push block, and adding a word to it crashed the
        //      renderer).
        //
        //      x IS THE FACE SDF LANE (`_SDFLightmap`) and y IS THE METALLIC/GLOSS LANE (`_MetallicGlossMap`); z
        //      and w are reserved for the lanes the rest of the character work adds, which is why the block is a
        //      `uvec4` rather than the single uint the SDF lane needed on its own - one descriptor, four lanes.
        //
        //      IT IS MATERIAL-INDEXED, not lane-indexed like the record's `toon_indices`: the shader reaches it
        //      with the material index it already has, so nothing new has to be threaded through the push path.
        //      Zero means "do not read", which is the contract the record's four lanes use too.
        //
        //      WRITTEN ONCE, here, like the material table beside it: the values are fixed at import and never
        //      rewritten, which is why it is one descriptor and not a per-frame pair.
        std::vector<uint8_t> const zeroed_toon_lanes(static_cast<size_t>(deren::vulkan::material_capacity) * deren::vulkan::toon_lane_blocks * sizeof(glm::uvec4), 0);
        create_buffer(this->vulkan_core,
                      rhi::buffer_usage::storage_coherent,
                      rhi::to_bits(rhi::buffer_flag::device_address),
                      std::as_bytes(std::span(zeroed_toon_lanes)),
                      "toon lane table buffer",
                      this->toon_lane_buffer,
                      &this->toon_lane_mapped);
        if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
            VkDeviceAddress const address = this->buffer_address(*this->toon_lane_buffer);
            bool const written = this->write_heap_buffer(*this->toon_lane_buffer,
                                                         core::heap_slots::toon_lanes,
                                                         static_cast<VkDeviceSize>(deren::vulkan::material_capacity) * deren::vulkan::toon_lane_blocks * sizeof(glm::uvec4),
                                                         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            deren::utility::log("descriptor heap: toon lane table {} (address 0x{:x}, {} lanes, offset {})",
                                written ? "written" : "NOT written",
                                address,
                                deren::vulkan::material_capacity,
                                static_cast<VkDeviceSize>(core::heap_slots::toon_lanes) * core::heap_slot_stride);
        }

        // ---- THE TOON LIGHT RIG: ONE BLOCK FOR THE RUN, and it is not a per-frame pair for the reason the two
        //      tables above are not: nothing rewrites it while a frame is in flight (see
        //      core::heap_slots::toon_rig). The application fills it from its config through
        //      `runtime::set_toon_rig`, and the zeros it is created with are replaced before the first frame.
        {
            std::vector<uint8_t> const zeroed_toon_rig(sizeof(deren::vulkan::toon_rig), 0);
            create_buffer(this->vulkan_core,
                          rhi::buffer_usage::storage_coherent,
                          rhi::to_bits(rhi::buffer_flag::device_address),
                          std::as_bytes(std::span(zeroed_toon_rig)),
                          "toon light rig buffer",
                          this->toon_rig_buffer,
                          &this->toon_rig_mapped);
            if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
                VkDeviceAddress const address = this->buffer_address(*this->toon_rig_buffer);
                bool const written = this->write_heap_buffer(*this->toon_rig_buffer,
                                                             core::heap_slots::toon_rig,
                                                             static_cast<VkDeviceSize>(sizeof(deren::vulkan::toon_rig)),
                                                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                deren::utility::log("descriptor heap: toon light rig {} (address 0x{:x}, {} B, offset {})",
                                    written ? "written" : "NOT written",
                                    address,
                                    sizeof(deren::vulkan::toon_rig),
                                    static_cast<VkDeviceSize>(core::heap_slots::toon_rig) * core::heap_slot_stride);
            }
        }

        // ---- THE MESHLET TABLE (docs/mesh_shaders.md step 3): one `deren::vulkan::meshlet` record per meshlet - 48 bytes,
        //      the layout `static_assert`s in vulkan/meshlet/meshlet.cppm pin field by field, because a 28-byte
        //      host record against the shader's std430 stride WEDGED the GPU before it was found - appended by the
        //      primitive upload while the scene imports and read by a task stage through the heap. Created here,
        //      with the material table's shape and for its reasons: fixed capacity, host-visible (direct mapping),
        //      and a device address because a heap descriptor for a buffer IS an address range.
        {
            std::vector<uint8_t> const zeroed_meshlets(static_cast<size_t>(deren::vulkan::meshlet_capacity) * sizeof(deren::vulkan::meshlet), 0);
            create_buffer(this->vulkan_core,
                          rhi::buffer_usage::storage_coherent,
                          rhi::to_bits(rhi::buffer_flag::device_address),
                          std::as_bytes(std::span(zeroed_meshlets)),
                          "meshlet table buffer",
                          this->meshlet_buffer,
                          &this->meshlet_mapped);
            if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
                VkDeviceAddress const address = this->buffer_address(*this->meshlet_buffer);
                bool const written = this->write_heap_buffer(*this->meshlet_buffer,
                                                             core::heap_slots::meshlets,
                                                             static_cast<VkDeviceSize>(deren::vulkan::meshlet_capacity) * sizeof(deren::vulkan::meshlet),
                                                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                deren::utility::log("descriptor heap: meshlet table {} (address 0x{:x}, {} records of {} B, offset {})",
                                    written ? "written" : "NOT written",
                                    address,
                                    deren::vulkan::meshlet_capacity,
                                    sizeof(deren::vulkan::meshlet),
                                    static_cast<VkDeviceSize>(core::heap_slots::meshlets) * core::heap_slot_stride);
            }
        }

        // ---- THE INDIRECT MESH COMMANDS (docs/mesh_shaders.md step 3, second mechanism): one
        //      VkDrawMeshTasksIndirectCommandEXT per (frame in flight, primitive), written by whichever thread
        //      records that primitive's meshlet dispatch and read by the GPU. THE SLOT IS THE PRIMITIVE'S OWN
        //      `meshlet_base`, which is what removes the cursor the first attempt used - and with it the flakiness
        //      that cursor caused (see the member's note in runtime.declarations.cppm). Not on the heap: it is
        //      command data, not a resource any shader reads.
        {
            constexpr std::size_t commands_per_frame = runtime::mesh_command_capacity;
            std::vector<uint8_t> const zeroed_commands(static_cast<size_t>(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT) * commands_per_frame * sizeof(VkDrawMeshTasksIndirectCommandEXT), 0);
            create_buffer(this->vulkan_core,
                          rhi::buffer_usage::storage_coherent,
                          rhi::to_bits(rhi::buffer_flag::indirect),
                          std::as_bytes(std::span(zeroed_commands)),
                          "mesh indirect command table",
                          this->mesh_indirect_buffer,
                          &this->mesh_indirect_mapped);
            // the raw handle the command takes: `vk_buffer::handle()` was the allocator's id rather than a
            // `VkBuffer`, and a null here is what silently sent every dispatch down the DIRECT path in the
            // first version of this seam. It comes from the escape now, off the handle this class owns.
            this->mesh_indirect_table = this->buffer_of(*this->mesh_indirect_buffer);
        }

        // ---- THE MESH CULLING COUNTERS (docs/mesh_shaders.md step 3, "what the culling buys"): one lane of eight
        //      uints per frame in flight, on the heap because a mesh stage has no other way to reach memory, and
        //      host-visible because the host reads it back once, at shutdown. Zeroed here; the entries only add.
        {
            std::vector<uint8_t> const zeroed_stats(static_cast<size_t>(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT) * 8u * sizeof(uint32_t), 0);
            create_buffer(this->vulkan_core,
                          rhi::buffer_usage::storage_coherent,
                          rhi::to_bits(rhi::buffer_flag::device_address),
                          std::as_bytes(std::span(zeroed_stats)),
                          "mesh culling counter buffer",
                          this->meshlet_stats_buffer,
                          &this->meshlet_stats_mapped);
            if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
                bool const written = this->write_heap_buffer(*this->meshlet_stats_buffer,
                                                             core::heap_slots::meshlet_stats,
                                                             static_cast<VkDeviceSize>(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT) * 8u * sizeof(uint32_t),
                                                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                if (!written) {
                    deren::utility::log("descriptor heap: the mesh culling counters were NOT written - the counters stay zero");
                }
            }
        }

        // ---- THE HOST-CULLED MESHLET TABLE (docs/mesh_shaders.md step 3, the culling's cheapest stage): one lane of
        //      `meshlet_capacity` records per frame in flight, host-visible because the HOST writes it while it
        //      records and the heap-bound because the mesh entry reads it. Per frame rather than one slot, unlike the
        //      table it shadows: this one is rewritten from the camera every frame.
        {
            std::vector<uint8_t> const zeroed_culled(static_cast<size_t>(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT) * deren::vulkan::meshlet_capacity * sizeof(deren::vulkan::meshlet), 0);
            create_buffer(this->vulkan_core,
                          rhi::buffer_usage::storage_coherent,
                          rhi::to_bits(rhi::buffer_flag::device_address),
                          std::as_bytes(std::span(zeroed_culled)),
                          "culled meshlet table buffer",
                          this->meshlet_culled_buffer,
                          &this->meshlet_culled_mapped);
            if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
                bool const written = this->write_heap_buffer(*this->meshlet_culled_buffer,
                                                             core::heap_slots::meshlet_culled,
                                                             static_cast<VkDeviceSize>(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT) * deren::vulkan::meshlet_capacity * sizeof(deren::vulkan::meshlet),
                                                             VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
                if (!written) {
                    deren::utility::log("descriptor heap: the culled meshlet table was NOT written - the draws stay unculled");
                }
            }
        }

        // Per-instance transform buffer (set 0 binding 6): one mat4 per instance, host-visible;
        // filled by set_instanced_draw() for instanced stress draws (see pbr.vert)
        std::vector<uint8_t> const zeroed_instances(static_cast<size_t>(deren::vulkan::instance_capacity) * sizeof(glm::mat4), 0);
        create_buffer(this->vulkan_core,
                      rhi::buffer_usage::storage_coherent,
                      rhi::to_bits(rhi::buffer_flag::device_address), // the heap writes this one's address
                      std::as_bytes(std::span(zeroed_instances)),
                      "instance transform buffer",
                      this->instance_buffer,
                      &this->instance_mapped);

        // Per-motion-slot previous world matrices (set 0 binding 13): ONE buffer per frame slot, like
        // the skin and morph buffers below, so a frame in flight never shares the buffer the next
        // frame rewrites. Zero-filled: a leaf's first frame reports "no motion", which is right -
        // nothing was there to move from. motion_previous is the CPU-side copy of what is currently
        // in it, advanced by advance_motion_transforms().
        std::vector<uint8_t> const zeroed_motion(static_cast<size_t>(deren::vulkan::scene_motion_capacity) * sizeof(glm::mat4), 0);
        create_buffers(this->vulkan_core,
                       this->motion_buffers,
                       rhi::buffer_usage::storage_coherent,
                       // THE HEAP'S REQUIREMENT: a descriptor written as a device ADDRESS range
                       // needs the buffer to be addressable (VUID-VkBufferDeviceAddressInfo-
                       // buffer-02601 says so, and validation did, the moment this write went in).
                       rhi::to_bits(rhi::buffer_flag::device_address),
                       std::as_bytes(std::span(zeroed_motion)),
                       "motion transform buffer",
                       &this->motion_mapped);
        this->motion_previous.assign(deren::vulkan::scene_motion_capacity, glm::mat4(1.0f));

        // Per-joint skin matrices (set 0 binding 9): one buffer PER FRAME SLOT (scene_skin_capacity
        // mat4s each, host-visible) so an in-flight frame never shares the buffer the next frame
        // rewrites. Zero-filled initially (the identity block is written by the setup upload).
        std::vector<uint8_t> const zeroed_skins(static_cast<size_t>(deren::vulkan::scene_skin_capacity) * sizeof(glm::mat4), 0);
        create_buffers(this->vulkan_core,
                       this->skin_buffers,
                       rhi::buffer_usage::storage_coherent,
                       rhi::to_bits(rhi::buffer_flag::device_address), // the heap writes this one's address
                       std::as_bytes(std::span(zeroed_skins)),
                       "skin matrix buffer",
                       &this->skin_mapped);

        // The SAME joint blocks as of ONE FRAME AGO (heap: skin_matrices_previous), one buffer per frame
        // slot: a DEFORMING vertex's motion vector is the difference between the two frames' skinning, so
        // these are as load-bearing as the current ones, and a frame in flight must not share the buffer the
        // next frame rewrites. Zero-filled on the GPU like its sibling; skin_previous is the CPU-side copy
        // ("the matrices one frame ago") that advance_motion_deformations() publishes from, initialised to
        // IDENTITY exactly as motion_previous is - an unskinned or not-yet-animated vertex then reports no
        // deformation, and the one frame that could read it is a frame TAA gives no history to.
        std::vector<uint8_t> const zeroed_previous_skins(static_cast<size_t>(deren::vulkan::scene_skin_capacity) * sizeof(glm::mat4), 0);
        create_buffers(this->vulkan_core,
                       this->skin_buffers_previous,
                       rhi::buffer_usage::storage_coherent,
                       rhi::to_bits(rhi::buffer_flag::device_address), // the heap writes this one's address
                       std::as_bytes(std::span(zeroed_previous_skins)),
                       "previous skin matrix buffer",
                       &this->skin_previous_mapped);
        this->skin_previous.assign(deren::vulkan::scene_skin_capacity, glm::mat4(1.0f));

        // Morph data (set 0 binding 10): one buffer PER FRAME SLOT (scene_morph_capacity floats
        // each, host-visible); the caller bakes per-primitive morph blocks (deltas + weights)
        // into every slot's buffer at setup, then rewrites only the active slot's weights per frame.
        // Zero-filled from one shared host vector (each create_buffer copies its own GPU buffer).
        std::vector<uint8_t> const zeroed_morphs(static_cast<size_t>(deren::vulkan::scene_morph_capacity) * sizeof(float), 0);
        create_buffers(this->vulkan_core,
                       this->morph_buffers,
                       rhi::buffer_usage::storage_coherent,
                       rhi::to_bits(rhi::buffer_flag::device_address), // the heap writes this one's address
                       std::as_bytes(std::span(zeroed_morphs)),
                       "morph data buffer",
                       &this->morph_mapped);

        // Reserve table index 0 as the DEFAULT material (white textures + identity factors):
        // registrations that overflow the table degrade to it (see register_material). Done
        // FIRST so it always lands at index 0 - the raw zeroed record at 0 would render black
        // (all factors zero), not white. Safe here: the texture it uploads is registered on the
        // heap as it arrives, exactly as a later material's is.
        {
            primitive_create_info const default_material = {};
            material_id const default_index = this->register_material(default_material);
            if (default_index.value != 0) {
                deren::utility::panic("default material must occupy table index 0");
            }
        }
    }

    // The recording resources: one primary command buffer per frame slot, plus the secondary buffers
    // of the two parallel recording stages and the pools that have to own them. They are pre-allocated
    // so the GPU can read a secondary while this slot's primary executes, and reused every frame, so
    // they must exist before the first recorded frame - but nothing else in the constructor depends on
    // them, and they depend on nothing else but the two capacities below (hence a function of their
    // own). This is also where the command pools would move to the core (the "command pools and
    // secondaries" item of the trim list): the SHAPE is policy and stays
    // here, the objects are device resources.
    void runtime::init_recording_resources() {
        // THE FRAME'S PRIMARY COMMAND BUFFERS ARE THE BACKEND'S NOW (S2 batch 2): core allocates one per
        // frame slot in its own constructor and releases them through its cleanup, and this runtime
        // BORROWS the container for the frame it begins, ends and submits. The shape is unchanged - the
        // count is still MAX_FRAMES_IN_FLIGHT, allocated once and reused - only the owner moved, which is
        // what lets the contract's begin_commands() hand out the frame's list (see
        // core.declarations.cppm's frame_command_buffers).
        this->command_buffers = this->vulkan_core.frame_command_buffers;
        // One shadow-pass + one gui-overlay secondary command buffer per frame slot (stage 2/3
        // of parallel recording): pre-allocated with the primaries so the GPU can read them
        // while this slot's primary executes. Stage 3 additionally gives the main pass one
        // parallel segment per task-pool worker, each as a {pool, secondary} PAIR (vma-style):
        // a VkCommandPool is not thread safe, so the workers must never begin buffers of a
        // shared pool concurrently - every worker owns its own pool + its buffer (recorded in
        // parallel; see sub_render_task).
        this->secondary_command_buffers.reserve(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT);
        this->main_segments.reserve(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT);
        uint32_t const record_workers = static_cast<uint32_t>(std::max(1, this->task_pool_threads()));
        for (int32_t slot = 0; slot < deren::vulkan::core::MAX_FRAMES_IN_FLIGHT; ++slot) {
            // one entry: the alpha-blended pass's secondary (see secondary_pass). The shadow cascades
            // and the main-pass segments own their buffers elsewhere, because they record concurrently.
            std::array<vk_command_buffer, static_cast<std::size_t>(secondary_pass::count)> pair = {
                this->vulkan_core.make_secondary_command_buffer(), // transparent
            };
            this->secondary_command_buffers.push_back(std::move(pair));

            // Shadow cascades record on the task pool, so each cascade gets its OWN {pool, buffer}: a
            // VkCommandPool is not thread safe and concurrent recording must not share one (M9).
            std::vector<std::pair<VkCommandPool, vk_command_buffer>> cascade_recording;
            cascade_recording.reserve(deren::vulkan::max_shadow_cascades);
            for (uint32_t cascade = 0; cascade < deren::vulkan::max_shadow_cascades; ++cascade) {
                cascade_recording.push_back(init_utils::create_recording_pool(this->vulkan_core));
            }
            this->shadow_recording.push_back(std::move(cascade_recording));
            std::vector<std::pair<VkCommandPool, vk_command_buffer>> segments;
            segments.reserve(record_workers);
            for (uint32_t s = 0; s < record_workers; ++s) {
                segments.push_back(init_utils::create_recording_pool(this->vulkan_core)); // one per worker
            }
            this->main_segments.push_back(std::move(segments));
        }
    }

    // The per-image state a swapchain GENERATION starts from. A freshly created target image is in
    // UNDEFINED, holds nothing, and no pass has written it: that is equally true of generation 0 (this
    // constructor, after the core built the generation's targets) and of every later generation
    // (on_swapchain_recreated), so both call THIS and the two can no longer drift.
    //
    // NOT here, deliberately: the TAA history matrices (image_view_proj). They may only be written from
    // a real camera snapshot (current_ubo), which does not exist yet in the constructor - they stay with
    // the two call sites that have one (on_swapchain_recreated, and set_taa's off -> on edge).
    void runtime::reset_image_generation_state() {
        // The G-buffer depth layout flags are one per swapchain image, and a freshly created depth image
        // is in UNDEFINED (which is what a clear flag says); see ensure_gbuffer_depth_sampled.
        this->gbuffer_depth_written.assign(this->vulkan_core.gbuffer_depth_images.size(), false);
        // The motion-vector images died with the generation as well: clear the layout flag so the first
        // frame of the new generation takes the attachment -> sampled transition (see
        // ensure_velocity_sampled).
        this->velocity_written.assign(this->vulkan_core.velocity_images.size(), false);
        this->rt_binding_written.assign(this->vulkan_core.velocity_images.size(), VK_NULL_HANDLE);
        this->gbuffer_targets_written.assign(this->vulkan_core.gbuffer_images[0].size(), false);
        // a generation has nothing to blend with, and it is reset HERE rather than only on the off -> on
        // vector reads as "no history" for every frame, which silently turns the temporal resolve into a
        // pass-through of the raw trace).
        // ... and its FRAME COUNT restarts with it: the cold-start widening is measured in frames since the
        // accumulation restarted, and a new generation IS that restart (see frame_facts::gi_cold_start).
        // ... and the furnace cube is a new image too, so its level has to be written again.
        this->furnace_cube_ready = false;
    }

    void runtime::ensure_shadow_resources() {
        if (!this->shadow_images.empty() && this->shadow_allocated_layers == this->shadow_cascades) {
            return; // already created for this cascade count
        }
        // Shadow map: one layered depth image per frame slot (see the member docs), with one layer
        // per cascade. Depth-only images carry no uploaded content (vma::create_image with data ==
        // nullptr skips the digest / upload path), so each frame can render the scene's depth from
        // the light's view into every layer.
        this->shadow_cascades = std::clamp(this->shadow_cascades, 1u, deren::vulkan::max_shadow_cascades);
        this->shadow_images.reserve(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT);
        this->shadow_array_views.reserve(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT);
        this->shadow_layer_views.reserve(deren::vulkan::core::MAX_FRAMES_IN_FLIGHT);
        for (int32_t slot = 0; slot < deren::vulkan::core::MAX_FRAMES_IN_FLIGHT; ++slot) {
            deren::vulkan::image_create_info shadow_info = {};
            shadow_info.width = this->shadow_map_size;
            shadow_info.height = this->shadow_map_size;
            shadow_info.mip_levels = 1;
            // One layer per ACTIVE cascade, NOT max_shadow_cascades: the spare layers the old code
            // always allocated were 2048x2048x4 B each per frame slot (33.5 MB with the default three
            // cascades) that nothing ever fitted, rendered or sampled. Growing the count rebuilds
            // these images (see set_shadow_cascades) and SHRINKING keeps the layers already owned,
            // which is why shadow_allocated_layers - not shadow_cascades - is the image's real layer
            // count and the value every subresource range over the whole array has to use.
            shadow_info.array_layers = this->shadow_cascades;
            shadow_info.format = this->vulkan_core.depth_attachment_format;
            shadow_info.extra_usage = VK_IMAGE_USAGE_SAMPLED_BIT; // sampled by shading.glsl
            vk_image shadow_image = this->vulkan_core.vma.create_image(nullptr, 0, shadow_info, deren::vulkan::image_type::texture_2d_depth);
            if (!shadow_image.valid()) {
                deren::utility::panic("failed to create shadow map image");
            }
            auto const* detail = this->vulkan_core.vma.get_image_detail(shadow_image.handle());
            if (detail == nullptr) {
                deren::utility::panic("failed to get shadow map image detail");
            }
            this->shadow_images.push_back(std::move(shadow_image));
            this->shadow_array_views.push_back(this->vulkan_core.make_depth_array_view(detail->image, this->vulkan_core.depth_attachment_format));
            std::vector<vk_image_view> layers;
            layers.reserve(this->shadow_cascades);
            for (uint32_t cascade = 0; cascade < this->shadow_cascades; ++cascade) {
                layers.push_back(this->vulkan_core.make_depth_layer_view(detail->image, this->vulkan_core.depth_attachment_format, cascade));
            }
            this->shadow_layer_views.push_back(std::move(layers));
        }
        this->shadow_allocated_layers = this->shadow_cascades;

        // Light UBO (scene block slot 7): one buffer PER FRAME SLOT (host-visible, mapped), so
        // a frame being rendered never shares the buffer the next frame rewrites. CPU-side
        // content lives in light_state; the frame loop memcpys it into the paced slot's buffer
        // (pace_and_acquire) - see the member docs for the concurrency rationale.
        light_ubo initial = {};
        create_buffers(this->vulkan_core,
                       this->light_buffers,
                       rhi::buffer_usage::uniform_coherent,
                       // it goes on the descriptor heap (a heap descriptor for a buffer is its
                       // device address), so the address has to exist - validation states it as
                       // VUID-VkBufferDeviceAddressInfo-buffer-02601 the moment it is queried -
                       // and it is written as a STORAGE descriptor, so the buffer needs the
                       // matching usage bit (see the camera UBO above and the note on
                       // write_heap_scene_buffer)
                       rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::storage),
                       std::as_bytes(std::span(&initial, 1)),
                       "light ubo buffer",
                       &this->light_mapped);

        // THE HEAD FRAME (scene block slot 749): the same per-frame-slot arrangement as the light UBO above, and
        // `head_state` starts at the reference's fallback frame - see that member for why it is not zeros.
        head_ubo initial_head = {};
        create_buffers(this->vulkan_core,
                       this->head_buffers,
                       rhi::buffer_usage::uniform_coherent,
                       rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::storage),
                       std::as_bytes(std::span(&initial_head, 1)),
                       "head frame buffer",
                       &this->head_mapped);
    }

    namespace {

        /// THE HEAP'S COPY OF ONE IMAGE, built from the SAME arguments `core::make_image_view` uses (see
        /// deren::vulkan::make_image_view_info in deren.vulkan.constant_init): a heap image descriptor carries a CREATE INFO
        /// rather than a view, and the driver makes the view inside it. That is why this is called where the image
        /// and its view are created - only that site knows the format, the view type and the range.
        bool write_heap_grid_image(core& vk, uint32_t const slot, VkImage const image, VkFormat const format, VkImageViewType const type, VkImageAspectFlags const aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
            if (!vk.descriptor_heaps.ready() || vk.heap_grid_offset == VK_WHOLE_SIZE || image == VK_NULL_HANDLE) {
                return false;
            }
            VkImageViewCreateInfo const view_info = make_image_view_info(image, format, type, aspect, VK_REMAINING_MIP_LEVELS, VK_REMAINING_ARRAY_LAYERS);
            return vk.descriptor_heaps.write_image(core::heap_slot_offset(slot), view_info, VK_IMAGE_LAYOUT_GENERAL);
        }

        /**
         * @brief write ONE per-slot binding of the scene block into the GRID - one slot per frame in flight
         *
         * THE HEAP'S BUFFER PATTERN, in one place: a heap descriptor for a buffer IS its address range, and a
         * per-slot binding's entry must name THAT slot's buffer - so this walks the per-slot vector, takes each
         * buffer's device address and writes it at `core::heap_slot_offset(slot_base + slot)`: one GRID slot per frame in
         * flight, which is exactly where the shaders read it (`heap_slots_scene_camera + heap_frame_slot` and its
         * neighbours). The heap is the only path now, so "the heap is not in use" is a startup
         * failure rather than a quiet fall back to a descriptor set.
         */
        void write_heap_scene_buffer(core& vk, std::vector<rhi::object_manager<rhi::buffer>> const& buffers, uint32_t const slot_base, VkDeviceSize const size, VkDescriptorType const type) {
            if (!vk.descriptor_heaps.ready() || vk.heap_grid_offset == VK_WHOLE_SIZE) {
                return;
            }
            uint32_t written = 0;
            for (uint32_t slot = 0; slot < buffers.size(); ++slot) {
                VkDeviceSize const offset = core::heap_slot_offset(slot_base + slot);
                if (vk.descriptor_heaps.write_buffer(offset, buffer_address(vk, *buffers[slot]), size, type)) {
                    ++written;
                } else {
                    deren::utility::log("descriptor heap: the per-frame buffer for grid slot {} (frame slot {}) did not fit at offset {}", slot_base, slot, offset);
                }
            }
            // SUCCESS IS LOGGED TOO, and that is not noise: a heap write has no picture to show for itself until
            // the shaders read the heap, so "no failure line" and "the buffers were empty, so nothing was written"
            // look exactly alike. This line is what tells them apart (it is the same reason the texture array and
            // the material table each log their count).
            deren::utility::log("descriptor heap: {} per-frame descriptor(s) written for grid slots {}..{}", written, slot_base, slot_base + (buffers.empty() ? 0u : static_cast<uint32_t>(buffers.size()) - 1u));
        }
    } // namespace

    void runtime::ensure_cluster_buffers() {
        if (!this->cluster_count_buffers.empty()) {
            return;
        }
        // Clustered light culling (M5), scene set bindings 11/12: one count per cluster and one
        // fixed-capacity index row per cluster, per frame slot (the compute pass writes them, the
        // fragment stage reads them, so a slot in flight must not be overwritten).
        //
        // Allocated for the MAXIMUM grid (max_cluster_count) once: the active grid is capped to it
        // every frame, so a resize only changes the grid dims in the light UBO - no reallocation, no
        // in-flight buffer to retire. Both are host-visible + coherent: the counts are zeroed by the
        // host each frame (that IS the pass's clear, see pace_and_acquire), and the indices only need
        // to live on the GPU between the dispatch and the shading.
        std::vector<uint8_t> const zero_counts(static_cast<std::size_t>(deren::vulkan::max_cluster_count) * sizeof(uint32_t), 0);
        std::vector<uint8_t> const zero_indices(static_cast<std::size_t>(deren::vulkan::max_cluster_count) * deren::vulkan::cluster_light_capacity * sizeof(uint32_t), 0);
        create_buffers(this->vulkan_core,
                       this->cluster_count_buffers,
                       rhi::buffer_usage::storage_coherent,
                       rhi::to_bits(rhi::buffer_flag::device_address), // heap-bound
                       std::as_bytes(std::span(zero_counts)),
                       "cluster count buffer",
                       &this->cluster_count_mapped);
        // The index rows are host-visible for the same reason, but nothing on the CPU ever writes
        // through the mapping: the dispatch fills them, so the mapped list stays a nullptr.
        create_buffers(this->vulkan_core,
                       this->cluster_index_buffers,
                       rhi::buffer_usage::storage_coherent,
                       rhi::to_bits(rhi::buffer_flag::device_address), // heap-bound
                       std::as_bytes(std::span(zero_indices)),
                       "cluster index buffer");

        // The clustered-light pass's TWO buffers, into the heap's per-slot blocks (bindings 11 and 12): this stage
        // is the first whose WHOLE set-0 interface is buffers (camera 0, light 7, counts 11, indices 12), which is
        // what makes it the one to migrate first - no image descriptors, no embedded samplers, nothing but address
        // ranges - and therefore the one that can be verified byte for byte before the image side is attempted.
        // The sizes are the buffers' REAL sizes, not VK_WHOLE_SIZE: a heap buffer descriptor is an
        // address RANGE, and validation states the rule as VUID-VkDeviceAddressRangeKHR-address-11365 - address plus
        // size must stay inside the buffer, which VK_WHOLE_SIZE cannot satisfy.
        write_heap_scene_buffer(this->vulkan_core, this->cluster_count_buffers, core::heap_slots::cluster_counts, static_cast<VkDeviceSize>(deren::vulkan::max_cluster_count) * sizeof(uint32_t), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        write_heap_scene_buffer(this->vulkan_core, this->cluster_index_buffers, core::heap_slots::cluster_indices, static_cast<VkDeviceSize>(deren::vulkan::max_cluster_count) * deren::vulkan::cluster_light_capacity * sizeof(uint32_t), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        // A STORAGE DESCRIPTOR, NOT A UNIFORM ONE, and the shaders declare it as a `buffer` block: the storage
        // class a shader reads a heap descriptor through must match the descriptor's type, and a mismatch reads
        // as zeros with NO validation finding. Slang's `DescriptorHandle<ConstantBuffer<T>>` always fetches
        // through a StorageBuffer-class pointer, so a VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER here made the Slang
        // G-buffer read a zero camera matrix (velocity NaN, motion channel black) while glslc's Uniform read of
        // the same slot worked. See docs/slang_migration.md, "the storage class that has to match".
        write_heap_scene_buffer(this->vulkan_core, this->camera_buffers, core::heap_slots::scene_camera, sizeof(camera_ubo), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        // ... and the rest of the per-frame arrays, from the same place and with the SAME sizes the scene block
        // writes them with (see ensure_scene_heap_slots's write_buffer_binding calls): motion (binding 13), skin (9) and
        // morph (10), each a two-slot array whose slot is the frame's. Bound here rather than in the per-slot loop
        // because a heap descriptor is an ADDRESS: the buffers are allocated once, so their addresses do not
        // change per frame, and only the CONTENTS are rewritten (see the per-frame slot rule in runtime.cppm).
        write_heap_scene_buffer(this->vulkan_core, this->motion_buffers, core::heap_slots::previous_transforms, static_cast<VkDeviceSize>(deren::vulkan::scene_motion_capacity) * sizeof(glm::mat4), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        write_heap_scene_buffer(this->vulkan_core, this->skin_buffers, core::heap_slots::skin_matrices, static_cast<VkDeviceSize>(deren::vulkan::scene_skin_capacity) * sizeof(glm::mat4), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        // ... and the previous-frame twin of the skin block, whose CONTENTS advance_motion_deformations()
        // rewrites per frame slot - so it is registered here, from the same size, for the same reason.
        write_heap_scene_buffer(this->vulkan_core, this->skin_buffers_previous, core::heap_slots::skin_matrices_previous, static_cast<VkDeviceSize>(deren::vulkan::scene_skin_capacity) * sizeof(glm::mat4), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        write_heap_scene_buffer(this->vulkan_core, this->morph_buffers, core::heap_slots::morph_data, static_cast<VkDeviceSize>(deren::vulkan::scene_morph_capacity) * sizeof(float), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        // THE INSTANCE TRANSFORM TABLE (set 0 binding 6) is the odd one: a SINGLE buffer rather than one per frame
        // slot (see runtime.cppm's member), so it takes ONE grid slot instead of a two-slot array - which is what
        // heap_slots::instance_transforms reserved. Written from the same size the scene block gives it.
        if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
            if (!this->write_heap_buffer(*this->instance_buffer, core::heap_slots::instance_transforms, static_cast<VkDeviceSize>(deren::vulkan::instance_capacity) * sizeof(glm::mat4), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) {
                deren::utility::log("descriptor heap: the instance transform table did not reach grid slot {}", core::heap_slots::instance_transforms);
            }
        }
    }

    void runtime::ensure_scene_heap_slots() {
        if (!this->cluster_count_buffers.empty()) {
            return; // the scene's heap slots are already written (the first primitive asked for them)
        }
        // THERE IS NO SCENE SET TO ENSURE ANY MORE - what this function still does is what the heap needs: the
        // shadow map and the clustered-light buffers must exist before the heap slots that name them are written,
        // and their creation is deferred to here so the app config that sets the cascade count has already run
        // (see the constructor note).
        this->ensure_shadow_resources();
        this->ensure_cluster_buffers();
        // binding 7 (light UBO, per-slot) is bound into each slot's heap block here, together with the shadow map's
        // grid slot.
        this->write_light_and_shadow_bindings();
    }

    void runtime::write_light_and_shadow_bindings() {
        // binding 7 (light UBO) + binding 8 (shadow map): BOTH point at THIS slot's own
        // resources (per-slot light buffers like the camera UBO, per-slot shadow images), so no
        // per-frame re-pointing is needed and an in-flight frame never shares a buffer the next
        // frame rewrites. Only the HEAP half is left: the two bindings are written into this slot's heap block
        // (and the shadow map into its grid slot) rather than into a descriptor set.
        for (int32_t slot = 0; slot < deren::vulkan::core::MAX_FRAMES_IN_FLIGHT; ++slot) {
            auto const* shadow_detail = this->vulkan_core.vma.get_image_detail(this->shadow_images[static_cast<std::size_t>(slot)].handle());
            if (shadow_detail == nullptr) {
                deren::utility::panic("failed to get shadow map image detail");
            }

            // THE SHADOW MAP GOES ONTO THE GRID HERE, because an image binding cannot be written the way a buffer
            // binding is: its heap descriptor is a CREATE INFO, rebuilt from the same arguments
            // core::make_depth_array_view uses - this slot's image, the depth format, a 2D-array view and the DEPTH
            // aspect (a colour aspect here would be a validation error, not a wrong picture). Which slot it
            // occupies is the frame's, matching shadow_images[slot].
            if (!write_heap_grid_image(this->vulkan_core, core::heap_slots::shadow_map + static_cast<uint32_t>(slot), shadow_detail->image, this->vulkan_core.depth_attachment_format, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_DEPTH_BIT)) {
                deren::utility::log("descriptor heap: the shadow map for frame slot {} did not reach grid slot {}", slot, core::heap_slots::shadow_map + static_cast<uint32_t>(slot));
            }

            // ---- AND THE LIGHT UBO, INTO THE HEAP'S BLOCK FOR THIS SLOT ----
            //
            // A heap descriptor for a buffer IS an address range: take the buffer's device address, write it at
            // the binding's offset inside this slot's block (core reserved the block and computed the offsets), and
            // nothing else changes. What does NOT work this way is an IMAGE binding: a heap image descriptor
            // carries a VkImageViewCreateInfo while a VkDescriptorImageInfo carries a view, not the image and range
            // that create info is made of - which is why the shadow map above is written where its image is known.
            if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
                VkDeviceSize const heap_offset = core::heap_slot_offset(core::heap_slots::scene_light + slot);
                if (!this->write_heap_buffer(*this->light_buffers[static_cast<std::size_t>(slot)], core::heap_slots::scene_light + slot, sizeof(light_ubo), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) {
                    deren::utility::log("descriptor heap: the light UBO did not fit slot {}'s block at offset {}", slot, heap_offset);
                }
            }

            // ---- AND THE HEAD FRAME, into its own block for this slot ----
            //
            // Its own block rather than a field of the light's, because it is a property of the CHARACTER rather
            // than of the scene's lighting - see `core::heap_slots::scene_head`. Written here beside the light
            // because it is per-frame-slot for the same reason: on a model whose head turns it changes every
            // frame, so each slot needs its own copy.
            if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
                VkDeviceSize const head_offset = core::heap_slot_offset(core::heap_slots::scene_head + slot);
                if (!this->write_heap_buffer(*this->head_buffers[static_cast<std::size_t>(slot)], core::heap_slots::scene_head + slot, sizeof(head_ubo), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)) {
                    deren::utility::log("descriptor heap: the head frame did not fit slot {}'s block at offset {}", slot, head_offset);
                }
            }
        }
    }

    void runtime::set_ibl(ibl_input const& info) {
        if (info.env_size == 0) {
            return;
        }
        auto const upload = [this](std::span<uint8_t const> const data, image_create_info const& create_info, image_type const type) -> vk_image {
            vk_image image = this->vulkan_core.vma.create_image(data.data(), data.size_bytes(), create_info, type);
            if (!image.valid()) {
                deren::utility::panic("failed to create IBL image");
            }
            return image;
        };

        // prefiltered environment cubemap (mip chain)
        deren::vulkan::image_create_info env_info = {};
        env_info.width = info.env_size;
        env_info.height = info.env_size;
        env_info.mip_levels = info.env_mip_count;
        env_info.array_layers = 6;
        env_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vk_image env_image = upload(info.prefiltered_env, env_info, deren::vulkan::image_type::texture_cubemap);
        auto const* env_detail = this->vulkan_core.vma.get_image_detail(env_image.handle());
        if (env_detail == nullptr) {
            deren::utility::panic("failed to get environment image detail");
        }
        this->ibl_images.push_back(std::move(env_image));
        this->ibl_views.push_back(this->vulkan_core.make_image_view(env_detail->image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_VIEW_TYPE_CUBE));
        // ... and the heap's copy of it, at its own grid slot (see docs/descriptor_heap_migration.md): written
        // HERE because this is the site that knows the format and the view type, which is what a heap image
        // descriptor is made of. An image whose BINDING is later repointed (the furnace mode) needs a rewrite
        // beside that change - the heap does not follow a view.
        if (!write_heap_grid_image(this->vulkan_core, core::heap_slots::env_cube, env_detail->image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_VIEW_TYPE_CUBE)) {
            deren::utility::log("descriptor heap: the environment cube did not reach grid slot {}", core::heap_slots::env_cube);
        }

        // irradiance cubemap
        deren::vulkan::image_create_info irr_info = {};
        irr_info.width = info.irr_size;
        irr_info.height = info.irr_size;
        irr_info.mip_levels = 1;
        irr_info.array_layers = 6;
        irr_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vk_image irr_image = upload(info.irradiance, irr_info, deren::vulkan::image_type::texture_cubemap);
        auto const* irr_detail = this->vulkan_core.vma.get_image_detail(irr_image.handle());
        if (irr_detail == nullptr) {
            deren::utility::panic("failed to get irradiance image detail");
        }
        this->ibl_images.push_back(std::move(irr_image));
        this->ibl_views.push_back(this->vulkan_core.make_image_view(irr_detail->image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_VIEW_TYPE_CUBE));
        if (!write_heap_grid_image(this->vulkan_core, core::heap_slots::irradiance_cube, irr_detail->image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_VIEW_TYPE_CUBE)) {
            deren::utility::log("descriptor heap: the irradiance cube did not reach grid slot {}", core::heap_slots::irradiance_cube);
        }

        // BRDF integration LUT
        deren::vulkan::image_create_info lut_info = {};
        lut_info.width = info.lut_size;
        lut_info.height = info.lut_size;
        lut_info.mip_levels = 1;
        lut_info.array_layers = 1;
        lut_info.format = VK_FORMAT_R16G16_SFLOAT;
        vk_image lut_image = upload(info.brdf_lut, lut_info, deren::vulkan::image_type::texture_2d);
        auto const* lut_detail = this->vulkan_core.vma.get_image_detail(lut_image.handle());
        if (lut_detail == nullptr) {
            deren::utility::panic("failed to get BRDF LUT image detail");
        }
        this->ibl_images.push_back(std::move(lut_image));
        this->ibl_views.push_back(this->vulkan_core.make_image_view(lut_detail->image, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D));
        if (!write_heap_grid_image(this->vulkan_core, core::heap_slots::brdf_lut, lut_detail->image, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D)) {
            deren::utility::log("descriptor heap: the BRDF LUT did not reach grid slot {}", core::heap_slots::brdf_lut);
        }

        this->env_sampler = this->vulkan_core.make_sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, static_cast<float>(info.env_mip_count - 1));
        this->ibl_ready = true;
        // The three images above are already on the heap, written where their images are (see the
        // write_heap_grid_image calls): a heap image descriptor is a CREATE INFO, so there is no separate
        // "point the binding at it" step and nothing to rewrite here.
    }

    // THE ARTICLE'S POST LUT (`ZmdLutPost.shader`'s `_LutTex`), uploaded exactly as the BRDF LUT above is - the same
    // `create_image(data, ...)` -> `make_image_view` -> `write_heap_grid_image` triple, because a baked cube and a
    // generated one differ only in who filled the bytes.
    void runtime::set_post_lut(std::span<uint8_t const> const pixels, uint32_t const width, uint32_t const height) {
        if (pixels.empty() || width == 0u || height == 0u) {
            deren::utility::log("post LUT: nothing to upload ({} bytes, {}x{})", pixels.size_bytes(), width, height);
            return;
        }
        deren::vulkan::image_create_info lut_info = {};
        lut_info.width = width;
        lut_info.height = height;
        lut_info.mip_levels = 1;
        lut_info.array_layers = 1;
        lut_info.format = VK_FORMAT_R8G8B8A8_SRGB;
        this->post_lut_image = this->vulkan_core.vma.create_image(pixels.data(), pixels.size_bytes(), lut_info, deren::vulkan::image_type::texture_2d);
        if (!this->post_lut_image.valid()) {
            deren::utility::panic("failed to create the post LUT image");
        }
        auto const* const detail = this->vulkan_core.vma.get_image_detail(this->post_lut_image.handle());
        if (detail == nullptr) {
            deren::utility::panic("failed to get the post LUT image detail");
        }
        this->post_lut_view = this->vulkan_core.make_image_view(detail->image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_VIEW_TYPE_2D);
        if (!write_heap_grid_image(this->vulkan_core, core::heap_slots::post_lut, detail->image, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_VIEW_TYPE_2D)) {
            deren::utility::log("descriptor heap: the post LUT did not reach grid slot {}", core::heap_slots::post_lut);
        }
        deren::utility::log("post LUT uploaded: {}x{}, {} bytes -> grid slot {}", width, height, pixels.size_bytes(), core::heap_slots::post_lut);
    }

    // THE GOO REFERENCE'S PRE-INTEGRATED FGD LUT: the same `create_image` -> `make_image_view` ->
    // `write_heap_grid_image` triple as the two LUTs above, from the reference's own PNG bytes. WHAT DIFFERS FROM
    // BOTH OF THEM IS ONE FIELD, AND IT IS THE WHOLE POINT OF THIS FUNCTION:
    //
    //     `VK_FORMAT_R8G8B8A8_UNORM`, NOT `_SRGB`.
    //
    // The reference's image data-block is `colorspace = 'Non-Color'` - the ONLY such image in this project, every
    // `_RD` / `_D` map being `'sRGB'` - so Blender's texture node hands the graph the texel's raw bytes and the
    // screenshot's three FGD outputs are reads of those bytes. An sRGB upload would linearize each channel once and
    // every one of `specularFGD` / `reflectivity` / `diffuseFGD` would be wrong in the same direction (spec §3.1
    // item 1: the texel at `(0,32)` has `R = 47/255 = 0.184314`, which sRGB-decodes to `0.028`). It is also why
    // `tests/test_goo_toon_math.cpp` pins this format string and asserts the file's own SHA-256.
    //
    // IT IS A GLOBAL IMAGE AND NOT A LANE (step-5 spec §3.4): the Goo `GetPreIntegratedFGDGGXAndDisneyDiffuse`
    // group has ONE `ShaderNodeTexImage`, its `users == 3` containers share that data-block, and its coordinate is
    // computed from `sqrt(NoV)` / `perceptualRoughness` / `fresnel0` rather than from a material's UV or a material
    // map - so a lane per material would be eleven sidecar rows pointing at one file for no information at all.
    void runtime::set_goo_fgd_lut(std::span<uint8_t const> const pixels, uint32_t const width, uint32_t const height) {
        if (pixels.empty() || width == 0u || height == 0u) {
            deren::utility::log("goo FGD LUT: nothing to upload ({} bytes, {}x{})", pixels.size_bytes(), width, height);
            return;
        }
        deren::vulkan::image_create_info fgd_info = {};
        fgd_info.width = width;
        fgd_info.height = height;
        // ONE MIP, WHICH IS THE REFERENCE'S OWN ANSWER rather than a saving: Blender's Texture node has no `Mip`
        // input here (`image_user` states interpolation / extension / projection only), so it samples lod 0 - and a
        // generated chain read by a rough surface would give a different number from the reference's (spec §9-U6).
        fgd_info.mip_levels = 1;
        fgd_info.array_layers = 1;
        fgd_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        this->goo_fgd_image = this->vulkan_core.vma.create_image(pixels.data(), pixels.size_bytes(), fgd_info, deren::vulkan::image_type::texture_2d);
        if (!this->goo_fgd_image.valid()) {
            deren::utility::panic("failed to create the goo FGD LUT image");
        }
        auto const* const detail = this->vulkan_core.vma.get_image_detail(this->goo_fgd_image.handle());
        if (detail == nullptr) {
            deren::utility::panic("failed to get the goo FGD LUT image detail");
        }
        this->goo_fgd_view = this->vulkan_core.make_image_view(detail->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D);
        if (!write_heap_grid_image(this->vulkan_core, core::heap_slots::goo_fgd_lut, detail->image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D)) {
            deren::utility::log("descriptor heap: the goo FGD LUT did not reach grid slot {}", core::heap_slots::goo_fgd_lut);
        }
        deren::utility::log("goo FGD LUT uploaded: {}x{}, {} bytes, R8G8B8A8_UNORM -> grid slot {}", width, height, pixels.size_bytes(), core::heap_slots::goo_fgd_lut);
    }

    material_id runtime::register_material(primitive_create_info const& info) {
        // ---- 1. Resolve the 5 texture slots against the shared array: identical texture bytes
        //         upload once, keyed by a CONTENT hash of the decoded bytes (xxh3 digest +
        //         format/dimensions - the loader hands every material its own copy of a shared
        //         glTF image, so a raw data pointer is NOT a stable identity; the digest lookup
        //         below is what actually dedups); missing slots point at the white fallback
        //         (element 0).
        std::array<std::pair<texture_input const*, VkFormat>, 5 + static_cast<std::size_t>(toon_slot::count)> const slots = {
            std::pair{&info.albedo, VK_FORMAT_R8G8B8A8_SRGB},
            std::pair{&info.metallic_roughness, VK_FORMAT_R8G8B8A8_UNORM},
            std::pair{&info.normal, VK_FORMAT_R8G8B8A8_UNORM},
            std::pair{&info.occlusion, VK_FORMAT_R8G8B8A8_UNORM},
            std::pair{&info.emissive, VK_FORMAT_R8G8B8A8_SRGB}, // glTF emissive textures are sRGB
            // ---- THE TOON SLOTS, in `toon_slot` order ----
            // THE FOUR COLOUR LANES ARE sRGB. They are colour data the reference decodes before using (its ramp
            // lookup is followed by `srgbToLinear(rd.rgb)`), so uploading them as linear would double-decode
            // them. The specular ramp is in there for the same reason: it is a COLOUR ramp rather than a scalar
            // curve.
            //
            // THE SDF IS NOT, AND THAT IS NOT AN OVERSIGHT: the reference declares its SDF sampler with LINEAR
            // filtering and CLAMP addressing and NO sRGB read flag, because the lightmap holds a DISTANCE FIELD
            // - a number per texel, not a colour - and decoding it as sRGB would bend the very quantity the
            // sigmoid thresholds. So this is the one toon lane uploaded UNORM.
            //
            // AND THIS ARRAY IS WHY THE ENUM AND THE INITIALISERS MUST AGREE: its size is `5 + toon_slot::count`,
            // so adding a lane to the enum without adding it here makes the last element value-initialised -
            // `{nullptr, VK_FORMAT_UNDEFINED}` - and the loop below dereferences `slots[i].first`. Measured: the
            // renderer died with an access violation inside `register_material` and the log stopped one stage
            // earlier, which is what sent the search to the descriptor writes instead of here.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::diffuse_ramp)], VK_FORMAT_R8G8B8A8_SRGB},
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::shadow_lut)], VK_FORMAT_R8G8B8A8_SRGB},
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::specular_ramp)], VK_FORMAT_R8G8B8A8_SRGB},
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::matcap)], VK_FORMAT_R8G8B8A8_SRGB},
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::sdf_lightmap)], VK_FORMAT_R8G8B8A8_UNORM},
            // THE METALLIC/GLOSS LANE IS UNORM FOR THE SDF'S REASON RATHER THAN THE RAMPS': its four channels are
            // metallic, reflectivity, occlusion and smoothness - NUMBERS, not colour - so an sRGB decode would
            // bend every one of them, and roughness read out of a decoded smoothness would be wrong everywhere on
            // the surface rather than at one band edge.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::metallic_gloss)], VK_FORMAT_R8G8B8A8_UNORM},
            // THE FACE MASK IS UNORM FOR THE SDF'S REASON RATHER THAN THE RAMPS': its four channels are REGION
            // WEIGHTS and a normal term - numbers, not colour - so an sRGB decode would bend every one of them.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::sdf_mask)], VK_FORMAT_R8G8B8A8_UNORM},
            // THE EMOTION ATLAS IS COLOUR - it is a painting of eyebrows and a mouth that REPLACES the albedo where
            // its alpha says so - so it takes the sRGB treatment the ramps do, not the mask's.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::emotion)], VK_FORMAT_R8G8B8A8_SRGB},
            // THE SPLIT NORMAL IS UNORM: its two packed tangent-space normals are DATA (`* 2 - 1` on the way in),
            // so an sRGB decode would bend both of them.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::split_normal)], VK_FORMAT_R8G8B8A8_UNORM},
            // THE GOO IRIS BALL IS COLOUR, so it takes the ramps' treatment rather than the masks': the reference
            // samples it and adds it to the albedo (`shaders/goo_toon.slang`), i.e. it is light, not a number.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_matcap05)], VK_FORMAT_R8G8B8A8_SRGB},
            // THE GOO BASE RAMP IS COLOUR, so it takes the treatment the reference gives it rather than the
            // masks': `images[...].colorspace = 'sRGB'` on all seven `_RD` images means Blender LINEARIZES each
            // texel before its node graph sees it (spec §5.3), so uploading this lane as `_SRGB` is what makes the
            // sampler do the same thing - and a lane uploaded UNORM would hand the shader a texel 2.2 gamma too
            // bright, which on a mid-grey ramp entry is the difference between a shadow and a highlight. ITS
            // ALPHA IS UNAFFECTED BY EITHER (Blender's colour management does not touch it), which is why the same
            // upload serves `RampAlpha` and the spec's §8-A6 asset-bound assertion.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_base_ramp)], VK_FORMAT_R8G8B8A8_SRGB},
            // ---- STEP 7'S THREE FACE LANES, AND THE FORMAT OF EACH ONE IS A STATEMENT ABOUT ITS CHANNELS ----
            //
            // THE FACE'S SDF IS UNORM, for the article's `sdf_lightmap` above and NOT for the `_RD` ramps': the
            // reference reads it as `(R + G) / 2` and feeds that NUMBER to a `SigmoidSharp` whose `center` is
            // `0.10000000894069672` - a threshold on a distance field, not a colour - so an sRGB decode would bend
            // the very quantity the sigmoid thresholds, by 2.2 gamma, in the region where its slope is steepest.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_face_sdf)], VK_FORMAT_R8G8B8A8_UNORM},
            // `cm_M` IS UNORM TOO, and it is the same kind of statement: its three channels are a LAYER SELECTOR
            // (`G`, compared against 0 and 1 exactly - `混合.002`'s factor), a WEIGHT (`R`, raised to `Front R Pow`
            // and smoothstepped) and a GATE (`A`, a multiply). None of them is light, so none of them may be
            // gamma-decoded; a `_SRGB` upload would turn a mask's "1.0" into "1.0" but its 0.2 into 0.033.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_face_cm)], VK_FORMAT_R8G8B8A8_UNORM},
            // `CsutmMask` IS UNORM FOR THE SAME REASON ONE STEP FURTHER: the only channel the Face container reads
            // is `G`, and it is read through a `GREATER_THAN(·, 0.5)` - a comparison whose whole answer is the
            // comparison, so the upload's transfer function decides which side of 0.5 a texel lands on.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_face_csumt)], VK_FORMAT_R8G8B8A8_UNORM},
            // `_M` IS UNORM AS WELL, and for the same statement once more: the `RS EFF` mask of mechanism table #14
            // is a MASK (`_M（非色彩）` in the reference's own node name), read through a luminance dot and then a
            // smoothstep, and its image's colorspace is Non-Color. A `_SRGB` upload would gamma-decode the very
            // quantity the smoothstep thresholds. See `toon_slot::goo_rs_mask`.
            //
            // THIS ENTRY IS NOT OPTIONAL AND NOT A TIDINESS: the array's size is `5 + toon_slot::count`, so adding
            // the enum lane above without this line value-initialises the LAST element to `{nullptr,
            // VK_FORMAT_UNDEFINED}` and the loop below dereferences `slots[i].first`. Measured: the renderer died
            // with an access violation inside `register_material` (see the block comment above this array).
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_rs_mask)], VK_FORMAT_R8G8B8A8_UNORM},
            // ---- STEP 15'S ONE, AND IT IS THE LANE THAT SPENDS THE TABLE'S LAST SLOT ----
            //
            // THE SHEET IS SRGB, AND THE STATEMENT IS THE OPPOSITE OF THE MASK LINE ABOVE RATHER THAN A COPY OF
            // IT: an `_RS` sheet is COLOUR - the reference multiplies it into `RS ColorTint` and then into the
            // shaded colour - so it takes the ramps' treatment, and uploading it UNORM would hand the shader a
            // texel 2.2 gamma off in the very product this branch exists to make. The decode belongs to the
            // SAMPLER: the shader must not decode it a second time (see `toon_slot::goo_rs_sheet`).
            //
            // AND THIS LINE IS NOT OPTIONAL FOR THE REASON THE COMMENT ABOVE GIVES, WHICH IS NOW A MEASURED ONE: the
            // array's size is `5 + toon_slot::count`, and leaving this entry out value-initialises the last element
            // to `{nullptr, VK_FORMAT_UNDEFINED}` - which the loop below dereferences. Step 15 raises `count` from 15
            // to 16, so omitting this line is the same access violation inside `register_material` the RS mask's own
            // note records.
            std::pair{&info.toon.slots[static_cast<std::size_t>(toon_slot::goo_rs_sheet)], VK_FORMAT_R8G8B8A8_SRGB},
        };

        std::array<uint32_t, 5 + static_cast<std::size_t>(toon_slot::count)> texture_indices = {};
        uint32_t heap_texture_descriptors = 0; // how many went into the descriptor heap (see the log below)
        for (std::size_t i = 0; i < slots.size(); ++i) {
            texture_input const& tex = *slots[i].first;
            if (!tex.valid || tex.data.empty()) {
                texture_indices[i] = this->white_texture_index; // white fallback
                continue;
            }
            // Content-addressed dedup: the loader hands every material its OWN byte copy of a
            // shared glTF image, so identical pixels arrive under different pointers. Hash the
            // decoded bytes (xxh3-128, the same digest vma uses for GPU-image dedup) and key the
            // slot cache on (digest, format, dimensions): N materials over one image upload
            // once and share the array element. The image itself is also vma-deduped below.
            deren::utility::xxh3_digest const digest = deren::utility::xxh3_128bits(std::span<uint8_t const>(tex.data.data(), tex.data.size_bytes()));
            // key on the digest data_block itself (not a raw byte array): data_block carries the
            // equality/ordering the std::map key needs
            auto const key = std::tuple<deren::utility::xxh3_digest, VkFormat, std::uint32_t, std::uint32_t, std::uint32_t>{
                digest, slots[i].second, tex.width, tex.height, tex.mip_levels};
            auto const cached = this->texture_slot_cache.find(key);
            if (cached != this->texture_slot_cache.end()) {
                texture_indices[i] = cached->second; // shared texture: reuse its slot
                continue;
            }
            if (this->texture_array_views.size() >= deren::vulkan::scene_texture_capacity) {
                // Array full (pathological scene with > scene_texture_capacity distinct images):
                // degrade this texture slot to the white element instead of crashing - the
                // material still renders untextured. Same policy as the material-table overflow
                // below (one-time log, then keep going); content dedup means real scenes rarely
                // get close to the cap.
                if (!this->texture_overflow_logged) {
                    this->texture_overflow_logged = true;
                    deren::utility::log("scene texture array capacity ({}) exceeded - extra textures render as white (element {})",
                                        deren::vulkan::scene_texture_capacity, this->white_texture_index);
                }
                texture_indices[i] = this->white_texture_index; // white fallback, like an invalid texture
                continue;
            }
            deren::vulkan::image_create_info image_info = {};
            image_info.width = tex.width;
            image_info.height = tex.height;
            image_info.mip_levels = tex.mip_levels; // the caller uploads a full mip-major chain
            image_info.array_layers = 1;
            image_info.format = slots[i].second;
            init_utils::texture_2d material_texture = init_utils::create_texture_2d(this->vulkan_core, std::as_bytes(tex.data), image_info, "material texture");
            this->owned_textures.push_back(std::move(material_texture.image));
            this->owned_texture_views.push_back(std::move(material_texture.view));
            uint32_t const index = static_cast<uint32_t>(this->texture_array_views.size());
            this->texture_array_views.push_back(*this->owned_texture_views.back());
            this->texture_slot_cache.emplace(key, index);
            texture_indices[i] = index;

            // A texture is registered ONCE, at scene load, and never rewritten - which is why the texture array is
            // the first binding this renderer puts on the heap: there is no per-frame rewrite and therefore no
            // frame-in-flight hazard to design around. The array starts at its own grid slot
            // (core::heap_slots::textures) and advances one SLOT per texture, i.e. 64 B - NOT the device's
            // imageDescriptorSize: the grid's single stride is what lets a shader index it with
            // `descriptor_stride = 64` (see docs/descriptor_heap_migration.md). The descriptor is a VIEW TO CREATE
            // rather than the view above: VkImageDescriptorInfoEXT carries a VkImageViewCreateInfo and the driver
            // makes the view itself. The values below are the ones core::make_image_view uses, on purpose - a view
            // that differs in mip range would sample a different image than the descriptor-set path.
            //
            // Nothing READS the heap yet, so a failure here is a log line and not a wrong frame - but it is the
            // write path that has to work first.
            if (this->vulkan_core.descriptor_heaps.ready()) {
                auto const* const texture_detail = this->vulkan_core.vma.get_image_detail(this->owned_textures.back().handle());
                if (texture_detail != nullptr) {
                    VkImageViewCreateInfo const heap_view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                                             .pNext = nullptr,
                                                             .flags = 0,
                                                             .image = texture_detail->image,
                                                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                                             .format = slots[i].second,
                                                             .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                                                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
                    VkDeviceSize const heap_offset = core::heap_slot_offset(core::heap_slots::textures + index);
                    if (this->vulkan_core.descriptor_heaps.write_image(heap_offset, heap_view, VK_IMAGE_LAYOUT_GENERAL)) {
                        ++heap_texture_descriptors;
                    } else {
                        deren::utility::log("descriptor heap: texture {} did not fit the resource heap at offset {}", index, heap_offset);
                    }
                }
            }
        }

        // THE HEAP-NATIVE PROBE, ONCE, now that the bindless array is actually in the heap: it is the first thing
        // in this renderer to read the heap instead of a descriptor set, and its answer goes to the log (see
        // run_heap_probe). Texture slot 0 is the white placeholder, which is registered first and always exists.
        if (heap_texture_descriptors != 0u) {
            this->run_heap_probe(static_cast<uint32_t>(core::heap_slots::textures));
        }
        // ... and its GRAPHICS half, twice: once with the material table's REAL grid slot (which must come back
        // white, the default material's base colour) and once with a deliberately WRONG one (which must not). The
        // pair is the negative proof the mechanism needs - the same draw, the same shader, one different number.
        if (this->vulkan_core.descriptor_heaps.ready() && this->vulkan_core.heap_grid_offset != VK_WHOLE_SIZE) {
            this->run_heap_graphics_probe(static_cast<uint32_t>(core::heap_slots::materials));
            this->run_heap_graphics_probe(static_cast<uint32_t>(core::heap_slots::materials) + 1u);
            // ... AND THE MESH HALF OF THE SAME QUESTION (docs/mesh_shaders.md step 0): a MESH pipeline created
            // with the heap flag and no layout, dispatched with vkCmdDrawMeshTasksEXT, must read the same slot
            // and come back the same white value. That is the mechanism a mesh-shader geometry path would stand
            // on, and the log line is the proof - the same comparison the other two lines make.
            this->run_heap_graphics_probe(static_cast<uint32_t>(core::heap_slots::materials), true);
        }

        // SAY WHAT WENT INTO THE HEAP, because the success path of a heap write is silent by nature (it returns
        // true and writes memory) and "no failure line" is not evidence that anything happened. This is the line a
        // reader checks to know the texture array really is on the heap; the mapping that points a shader stage at
        // it is the step after this one.
        if (heap_texture_descriptors > 0) {
            deren::utility::log("descriptor heap: {} texture descriptors written ({} B each, {} KiB resource heap)",
                                heap_texture_descriptors,
                                this->vulkan_core.descriptor_heaps.limits().image_descriptor_size,
                                this->vulkan_core.descriptor_heaps.resource_size() / 1024);
        }

        // ---- 2. Append one material record: texture indices + presence flags; factors keep
        //         their identity defaults (extend primitive_create_info to pass custom factors) ----
        material_record record = {};
        record.tex_indices = glm::uvec4(texture_indices[0], texture_indices[1], texture_indices[2], texture_indices[3]);
        record.emissive_index = texture_indices[4];
        // ---- THE TOON SLOTS, from the same upload loop and therefore through the same content-addressed dedup:
        //      a ramp shared by six materials of one character is uploaded ONCE. The FLAGS come from the sidecar
        //      rather than from the indices, because a lane can hold a real texture whose `_Use` flag is off -
        //      and a shader must not read it (see material_record::toon_flags).
        constexpr std::size_t toon_base = 5;
        record.toon_indices = glm::uvec4(texture_indices[toon_base + 0], texture_indices[toon_base + 1], texture_indices[toon_base + 2], texture_indices[toon_base + 3]);
        record.base_color_factor = info.factors.base_color_factor;
        record.emissive_factor = info.factors.emissive_factor;
        record.metallic_factor = info.factors.metallic_factor;
        record.roughness_factor = info.factors.roughness_factor;
        record.normal_scale = info.factors.normal_scale;
        record.alpha_cutoff = info.factors.alpha_cutoff;
        record.occlusion_strength = info.factors.occlusion_strength;
        // The toon family the loader classified this material's name into (0 == none). It rides the record's
        // fourth uint lane, which the std430 layout had already reserved as padding - see material_record.
        record.toon_family = info.toon_family;
        record.flags = 0;
        if (info.normal.valid) {
            record.flags |= 1u;
        }
        if (info.occlusion.valid) {
            record.flags |= 2u;
        }
        if (info.emissive.valid) {
            record.flags |= 4u;
        }
        if (info.double_sided) {
            record.flags |= 8u; // bit3: back faces are rendered, fragment shader flips normals
        }
        if (info.factors.alpha_mask) {
            record.flags |= 16u; // bit4: alphaMode MASK - fragment shader discards below alpha_cutoff
        }
        if (info.factors.alpha_blend) {
            record.flags |= 32u; // bit5: alphaMode BLEND - alpha-blended / transparent material
        }
        // ---- THE OVERLAY CHANNEL, on the record's two free bits ----
        //
        // THE FRAGMENT STAGE HAS TO BRANCH ON THIS, and that is the only reason it is in the record as well as
        // on the primitive: the article's two masks compute DIFFERENT multipliers from the same inputs -
        // `MyZmdEyeDarkShader` multiplies by `mask * _Alpha` while `MyZmdHairShadowShader` multiplies by the
        // scalar `_DayStrength` and never reads its mask - so one overlay pipeline draws both only because the
        // record tells it which. The primitive's copy is what the HOST reads to build the frame's leaf lists
        // (`primitive::overlay_kind`); this one is what the SHADER reads. It is the same deliberate
        // two-copies-of-one-import-fact arrangement `alpha_blend`/bit5 above already has, and it is two bits
        // rather than a lane because the record is at its 80-byte `static_assert` and `flags` had room.
        //
        // THE NUMBERS ARE `deren::gltf::overlay_kind`'S (1 = eye_dark, 2 = hair_shadow) and they are spelled as
        // literals here for the same reason every other flavour of this value is a number on this side of the
        // boundary: `deren.vulkan.runtime` does not import the loader's types - the classification is done where the
        // NAME exists and travels onwards as a value (see `toon_family` above, which does exactly this).
        // tests/test_gltf_loader.cpp asserts the enum's numbering so this pair cannot drift silently.
        if (info.overlay_kind == 1u) {
            record.flags |= 64u; // bit6: the EYE-DARK overlay (mask-driven)
        } else if (info.overlay_kind == 2u) {
            record.flags |= 128u; // bit7: the HAIR-SHADOW overlay (day-strength-driven)
        }
        // ---- THE AUTHOR'S TOON TRANSPARENT VARIANT (`_TRANSPARENT_ON`), ON ITS OWN BIT ----
        //
        // SEPARATE FROM bit5 (`alphaMode BLEND`) ON PURPOSE, and the two are not redundant: bit5 is glTF's
        // statement about coverage and is what the FRAGMENT stage writes a real `out_alpha` for; this one is the
        // sidecar's `_SrcBlend 5 / _DstBlend 10` pair, which the runtime evaluates at import (see
        // `toon_lookup::scalar`) because the blend state itself is per-PASS here. A material with this bit gets
        // the alpha-blended pipeline; every other material keeps the overwrite, and the two are the same
        // arithmetic at alpha 1.
        if (info.toon.alpha_blend) {
            record.flags |= 256u; // bit8: `_TRANSPARENT_ON` - the toon stage's own transparent variant
        }

        // ---- 3. Content-address the record, then append (or degrade on overflow) ----
        // Identical materials (same texture slots, factors and flags) share ONE table entry:
        // registration happens per primitive, so a scene with N primitives over M shared glTF
        // materials would otherwise append N records and burn the table needlessly. The key is
        // the byte-exact 80-byte record carried in a data_block - no hash collisions, because
        // the unordered lookup hashes the block only for bucketing while equality stays
        // byte-exact.
        // ---- THE LANES BESIDE THE RECORD ARE PART OF THE DEDUP KEY, and that is a fix rather than tidiness ----
        //
        // The record does NOT carry these lanes (see core::heap_slots::toon_lanes), so two primitives whose
        // records are byte-identical but whose materials differ in their SDF or metallic/gloss map would COLLIDE
        // on the key below: the second would take the early return, never write its lanes, and that material would
        // silently lose the feature - with the frame showing nothing but a slightly wrong face. MEASURED BEFORE
        // THIS LINE EXISTED: an instrumented probe showed the SDF texture arriving at this function VALID
        // (1024x1024, 4 MB) and every material's written lane was nevertheless 0, which is exactly the signature
        // of the material that owns the map losing the race to one that does not.
        glm::uvec4 const toon_lanes_extra(texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::sdf_lightmap)],
                                          texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::metallic_gloss)],
                                          texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::sdf_mask)],
                                          texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::emotion)]);
        // THE SECOND BLOCK, whose remaining lane (`w`) is reserved: it exists because a fifth lane does not fit a
        // `uvec4`, and it is zeroed rather than left out so that a shader reading a lane nobody set reads "do not
        // read" - the same contract the first block's lanes follow.
        //
        // EACH COMPONENT IS NAMED BY THE LANE IT CARRIES RATHER THAN BY ITS POSITION, which is a correction the
        // rewritten chain's iris forced. This initialiser was `(split_normal, 0, 0, 0u)` under a comment saying the
        // rest were reserved, and a lane written NOWHERE is not a reserved lane: it is a lane that reads "do not
        // read" for every material that states it. MEASURED: with `goo_matcap05` at lane 9 the rewrite's iris
        // produced a frame BYTE-IDENTICAL to the one with no matcap lane at all (`CB321ADE1673AC9C` both ways)
        // while the sidecar reported `_GooMatcap05 -> texture #27 | ON` - the host never wrote the lane and the
        // shader read 0 (see `deren-ab/goo_step1_result.md`). `test_goo_toon_math` now names this line among the
        // lane's sync points, so the next lane cannot be added to the enum and forgotten here.
        glm::uvec4 const toon_lanes_extra2(texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::split_normal)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::goo_matcap05)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::goo_base_ramp)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::goo_face_sdf)]);
        // THE THIRD BLOCK, WHICH STEP 7 ADDED AND WHICH IS WHY `toon_lane_blocks` IS 3: the FACE container's three
        // masks are lanes 11..13, and lane 11 was the last free component of the block above. `w` is reserved and
        // zeroed, the same contract every other block follows ("a lane nobody set reads DO NOT READ").
        //
        // STEP 13'S `goo_rs_mask` TAKES `.z`, AND THE LANE WAS DEAD UNTIL IT DID. `toon_slot::goo_rs_mask` was
        // added to the enum, given a format in `register_material` and named in the application's vocabulary, and
        // the sidecar resolved the image -- and the frame did not move by one pixel, because THIS is where a slot
        // becomes a number the shader can see and this line still packed `0u`. The shader tests `block3.z != 0u`
        // before it samples (slot 0 is the white fallback, so zero means "do not read"), so the mask branch was
        // skipped for every material and `rs_eff` was zero. That is EXACTLY the failure this block's comment above
        // records for the matcap lane at step 1 ("the host never wrote the lane and the shader read 0"), and it is
        // the second time the enum-to-lane-table join has been the last edit missing rather than the first: an
        // enum value, a format table entry and a vocabulary row are all compile-visible, and a component of a
        // `glm::uvec4` is not.
        //
        // `test_goo_toon_math` pins this line by name, next to the matcap one that was pinned after step 1, so a
        // further lane cannot be added to the enum and forgotten here either.
        glm::uvec4 const toon_lanes_extra3(texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::goo_face_cm)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::goo_face_csumt)],
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::goo_rs_mask)],
                                           // STEP 15'S SHEET IS THIS BLOCK'S `.w`, and it is written HERE rather than
                                           // left at the `0u` it used to be for the reason the block comment below
                                           // gives at length: `0` is the shader's "do not read", so a slot packed as
                                           // `0u` reads as a material that states no sheet. The mask one component to
                                           // the left was left at `0u` for a whole step with the enum, the format row
                                           // and the vocabulary row all present, and the frame did not move.
                                           texture_indices[toon_base + static_cast<std::size_t>(deren::vulkan::toon_slot::goo_rs_sheet)]);
        // ---- AND THE COLOUR LANES, THE SAME FIX ONE TABLE FURTHER ALONG ----
        //
        // They are written BELOW, after the early return, which is the whole reason they have to be in the key:
        // the early return is "this material's row is already in the tables", and without these bytes that claim
        // was false for the six `vec4`s - the second of two record-identical materials read the FIRST one's
        // colours. MEASURED, not argued (see `remaining_port_spec.md`'s "材质去重键补上 colour lanes" section): a copy of
        // `M_actor_chen_hair_01` stating `_Specular = 0.0`, attached to a mesh whose node comes earlier in the
        // scene, took the earlier index; the hair itself then took this early return and rendered BYTE-FOR-BYTE
        // like an asset whose hair states 0.0 - its own 1.0 discarded.
        //
        // `info.toon.colours` IS the array the table is filled from, lane for lane and in the same order, so
        // keying exactly these bytes dedups precisely the materials whose six rows would come out identical -
        // a pair stating the same values still shares one entry (the control arm of that probe: 0 px).
        deren::utility::data_block<sizeof(deren::vulkan::material_record) + deren::vulkan::toon_lane_blocks * sizeof(glm::uvec4) + static_cast<std::size_t>(deren::vulkan::toon_colour_lane::count) * sizeof(glm::vec4)> material_key = {};
        std::memcpy(material_key.data.data(), &record, sizeof(record));
        std::memcpy(material_key.data.data() + sizeof(record), &toon_lanes_extra, sizeof(toon_lanes_extra));
        std::memcpy(material_key.data.data() + sizeof(record) + sizeof(toon_lanes_extra), &toon_lanes_extra2, sizeof(toon_lanes_extra2));
        std::memcpy(material_key.data.data() + sizeof(record) + sizeof(toon_lanes_extra) + sizeof(toon_lanes_extra2), &toon_lanes_extra3, sizeof(toon_lanes_extra3));
        std::memcpy(material_key.data.data() + sizeof(record) + sizeof(toon_lanes_extra) + sizeof(toon_lanes_extra2) + sizeof(toon_lanes_extra3),
                    info.toon.colours.data(),
                    static_cast<std::size_t>(deren::vulkan::toon_colour_lane::count) * sizeof(glm::vec4));
        if (auto const cached = this->material_slot_cache.find(material_key); cached != this->material_slot_cache.end()) {
            return cached->second; // already registered: share the existing record
        }
        if (this->material_count >= deren::vulkan::material_capacity) {
            // Table full (pathological - 16384 unique materials): degrade to the reserved
            // default material (index 0, white + identity factors, registered at setup) instead
            // of crashing; the mesh still draws. Logged once, not per registration.
            if (!this->material_overflow_logged) {
                this->material_overflow_logged = true;
                deren::utility::log("material table capacity ({}) exceeded - extra materials render with the default (index 0)", deren::vulkan::material_capacity);
            }
            return {};
        }
        uint32_t const material_index = this->material_count++;
        std::memcpy(static_cast<uint8_t*>(this->material_mapped) + static_cast<size_t>(material_index) * sizeof(material_record), &record, sizeof(record));
        // THE LANES BESIDE THE RECORD, written here rather than into it, and this is the one place that knows both
        // the material's index and its lanes (see core::heap_slots::toon_lanes for why the record cannot carry
        // them). They are MATERIAL-indexed, so the shader reaches them with the index it already uses for the
        // record.
        //
        // IT SITS AFTER THE DEDUP'S EARLY RETURN, deliberately and safely: that return is for a record ALREADY in
        // the table, whose lanes were written when it was appended. A lane of 0 - no map, or the artist's
        // `_UseSDFLightmap` / `_UseMetallicGlossMap` off, which collapse to the same value here exactly as they do
        // for the record's four - is the "do not read" the shader tests.
        static_cast<glm::uvec4*>(this->toon_lane_mapped)[static_cast<std::size_t>(material_index) * deren::vulkan::toon_lane_blocks] = toon_lanes_extra;
        static_cast<glm::uvec4*>(this->toon_lane_mapped)[static_cast<std::size_t>(material_index) * deren::vulkan::toon_lane_blocks + 1u] = toon_lanes_extra2;
        // ... AND THE THIRD BLOCK, for the reason the second one's own note gives: a lane the host never writes
        // reads "do not read" for every material that states it, which on the FACE would silently drop its SDF, its
        // `cm_M` and its brightness switch at once while the log still reported all three as ON (lane 11 is
        // `goo_face_sdf`, i.e. the component that closed the second block - see `toon_slot`).
        static_cast<glm::uvec4*>(this->toon_lane_mapped)[static_cast<std::size_t>(material_index) * deren::vulkan::toon_lane_blocks + 2u] = toon_lanes_extra3;
        // AND THE MATERIAL'S COLOURS, at the same index and in the same once-written spirit: the shader addresses
        // them with `material_index * toon_colour_lane::count + lane`, so the two sides' stride has to agree - see
        // `character_toon_colour_lanes` in the stage and the drift check in the sidecar test.
        //
        // AND THEY *ARE* PART OF THE DEDUP KEY ABOVE, which is a fix and not tidiness: they are written at this
        // index, once, and this paragraph used to say the opposite - that the key was the record plus the two
        // `uvec4` blocks alone, so two materials whose records AND texture lanes were byte-identical shared ONE
        // index here and therefore ONE set of colours, whichever registered first. That was latent on this
        // repository's assets (chen registers all seven of its materials separately - see the `toon: material N`
        // lines - because their records differ), but it governed all three per-material things that live ONLY in
        // these lanes: `toon_colour_lane::specular_strength`, `toon_colour_lane::parallax_scale`, and the outline
        // width in `toon_colour_lane::outline_edge`'s `.w` (which nothing else carries either). Fixed by keying
        // the six lanes' bytes beside the two blocks - see the `material_key` construction above and the note on
        // `material_slot_cache` in `runtime.declarations.cppm`.
        if (this->toon_colour_mapped != nullptr) {
            glm::vec4* const colours = static_cast<glm::vec4*>(this->toon_colour_mapped) + static_cast<std::size_t>(material_index) * static_cast<std::size_t>(deren::vulkan::toon_colour_lane::count);
            for (uint32_t lane = 0; lane < static_cast<uint32_t>(deren::vulkan::toon_colour_lane::count); ++lane) {
                colours[lane] = info.toon.colours[lane];
            }
        }
        // LOGGED WHILE THESE LANES ARE BEING WIRED, and the reason is that a lane which silently stays zero is
        // this table's only failure mode and it is invisible in the frame: the shader's test is `lane != 0`, so a
        // table that was never filled renders exactly like a model with no SDF and no metallic/gloss map at all -
        // the feature does not happen and nothing says why. THE SECOND BLOCK'S THREE NAMED LANES ARE PRINTED TOO,
        // for the same reason one table along: `_GooMatcap05` and `_GooBaseRamp` are lanes the REWRITTEN chain
        // gates on, and a zero here is the difference between "the asset states no ramp" and "the host resolved one
        // and never wrote it" - which is exactly the failure step 1 recorded (the iris rendered a black ball while
        // the sidecar line said `ON`).
        deren::utility::log("toon: material {} (family {}) -> lanes: sdf {}, metallic/gloss {}, face mask {}, split normal {}, goo matcap {}, goo base ramp {} of {} texture(s)",
                            material_index,
                            record.toon_family,
                            toon_lanes_extra.x,
                            toon_lanes_extra.y,
                            toon_lanes_extra.z,
                            toon_lanes_extra2.x,
                            toon_lanes_extra2.y,
                            toon_lanes_extra2.z,
                            this->texture_array_views.size());
        // ... AND THE SPECULAR STRENGTH, WHICH IS THE ONE LANE WHOSE "NOTHING STATED" IS A SENTINEL RATHER THAN
        // THE TABLE'S NEUTRAL (`-1`, see `toon_colour_lane::specular_strength`), so a log line is the only place
        // the difference between "the asset said 0.0" and "no source spoke" is visible at all: both reach the
        // shader as a number, and one of them means the family table answers. Printed for every registered
        // material, because the value is per material and the interesting case is the one that DISAGREES with its
        // family - which is exactly the case a family table cannot show.
        deren::utility::log("toon: material {} -> specular strength {:.4f}{}",
                            material_index,
                            static_cast<double>(info.toon.colours[static_cast<std::size_t>(deren::vulkan::toon_colour_lane::specular_strength)].x),
                            info.toon.colours[static_cast<std::size_t>(deren::vulkan::toon_colour_lane::specular_strength)].x < 0.0f ? "  <- no source stated `_Specular`: the family table's number stands" : "");
        // ... AND THE PARALLAX DEPTH BESIDE IT, for the same reason one lane up and with one difference that makes
        // the line more useful rather than less: the stage's fallback for it is not a family number but its own
        // constant (`character_eye_parallax_depth`, 0.03), so `-1` here reads as "this material keeps the offset
        // the port had before the lane existed". That is the whole expected output for chen: a value on the three
        // materials whose `extras` state the row (0.03 iris / 0.5 brow / 0.5 cloth_01) and the sentinel on the rest.
        deren::utility::log("toon: material {} -> parallax depth {:.4f}{}",
                            material_index,
                            static_cast<double>(info.toon.colours[static_cast<std::size_t>(deren::vulkan::toon_colour_lane::parallax_scale)].x),
                            info.toon.colours[static_cast<std::size_t>(deren::vulkan::toon_colour_lane::parallax_scale)].x < 0.0f ? "  <- no source stated `_ParallaxScale`: the stage's own constant stands" : "");
        // ---- AND THE EMISSIVE LANE, WHICH THE TOON STAGE ADDS AND THEREFORE HAS TO BE ABLE TO TRUST ----
        //
        // `s.emissive` is `emissive_factor * the texture at emissive_index`, and index 0 is the WHITE FALLBACK
        // rather than "no emission" - so a material whose emissive map did not reach the record does not lose a
        // term, it gains `factor * 1`, i.e. the emission its artist stated applied at FULL STRENGTH over the
        // whole surface. That is the opposite of a missing feature and it is invisible in the log without this
        // line: measured on `chars\chen_full2.glb`, where the cloth's near-black emissive map (mean 0.3/255)
        // resolved in one file and not in the other, the difference is 30% of the frame.
        deren::utility::log("toon: material {} -> emissive: index {} factor ({:.3f}, {:.3f}, {:.3f}){}",
                            material_index,
                            record.emissive_index,
                            static_cast<double>(record.emissive_factor.x),
                            static_cast<double>(record.emissive_factor.y),
                            static_cast<double>(record.emissive_factor.z),
                            record.emissive_index == 0u && (record.emissive_factor.x != 0.0f || record.emissive_factor.y != 0.0f || record.emissive_factor.z != 0.0f)
                                ? "  <- FACTOR AGAINST THE WHITE FALLBACK: this material states emission but has no emissive map in the record"
                                : "");
        this->material_slot_cache.emplace(material_key, material_id{material_index});
        // THE TOON FAMILY, LOGGED WHEN IT IS NOT `none`, and this is the one place it can be logged once per
        // MATERIAL rather than once per primitive (the dedup above returns early for a shared record). A
        // classification is a DECISION about a string, and a wrong one is invisible in the frame: a face
        // material shaded with hair's ramp still looks like a toon character, just not like the reference's.
        // Printing the assignment at import is what makes a mis-classification a line to read instead of a
        // look to argue about. Silent for `none`, which is every non-character model and would be log spam.
        if (record.toon_family != 0u) {
            deren::utility::log("toon: material {} -> family {} (1=base 2=skin 3=face 4=hair 5=eye 6=cloth)", material_index, record.toon_family);
        }
        return material_id{material_index};
    }

} // namespace deren::vulkan