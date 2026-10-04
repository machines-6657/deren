// 引擎拥有渲染目标和配置；设备/提交/分配器只经已加载的后端契约访问。
module;
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>
export module deren.vulkan.engine_device;
export import deren.vulkan.engine_gpu;
import deren.utility;
export namespace deren::vulkan {
    inline constexpr uint32_t scene_texture_capacity = 128;
    inline constexpr uint32_t scene_push_constant_size = 96;
    inline constexpr uint32_t scene_cascade_push_offset = scene_push_constant_size;
    inline constexpr uint32_t scene_cascade_push_size = sizeof(uint32_t);
    inline constexpr VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    inline constexpr uint32_t gbuffer_target_count = 3;
    inline constexpr std::array<VkFormat, gbuffer_target_count> gbuffer_formats = {
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM,
    };
    inline constexpr VkFormat gbuffer_velocity_format = VK_FORMAT_R16G16_SFLOAT;
    inline constexpr uint32_t gbuffer_pass_attachment_count = gbuffer_target_count + 2;
    inline constexpr uint32_t gpu_timing_mark_capacity = 16;

    struct gpu_timing_result {
        std::array<double, 16> milliseconds{};
        uint32_t mark_count = 0;
    };
    class engine_device {
        std::vector<vk_image> target_owners;
        std::vector<vk_image_view> target_view_owners;
        std::map<VkImage, rhi::image*> target_resources;
        rhi::vulkan_snapshot snapshot() {
            rhi::vulkan_snapshot out{};
            out.device_properties = {&device_properties, sizeof(device_properties)};
            out.ray_tracing_properties = {&ray_tracing_pipeline_properties, sizeof(ray_tracing_pipeline_properties)};
            out.acceleration_properties = {&acceleration_structure_properties, sizeof(acceleration_structure_properties)};
            auto result = gpu.escape->service(rhi::vulkan_service::snapshot, &out, sizeof(out));
            if (result != rhi::error::ok)
                deren::utility::panic("backend device snapshot unavailable");
            return out;
        }
        VkResult frame_call(rhi::vulkan_service op, uint32_t image = 0, VkCommandBuffer command = VK_NULL_HANDLE, uint32_t slot = 0) const {
            rhi::vulkan_frame call{};
            call.image_index = image;
            call.command_buffer = command;
            call.slot = slot;
            auto error = gpu.escape->service(op, &call, sizeof(call));
            return error == rhi::error::ok ? static_cast<VkResult>(call.result) : VK_ERROR_INITIALIZATION_FAILED;
        }

    public:
        gpu_context gpu;
        resource_catalog vma;
        heap_access descriptor_heaps;
        GLFWwindow* window = nullptr;
        VkInstance instance{};
        VkPhysicalDevice physical_device{};
        VkDevice logical_device{};
        VkQueue graphics_queue_handle{};
        uint32_t graphics_queue_family_index = 0;
        VkPhysicalDeviceProperties device_properties{};
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline_properties{};
        VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties{};
        rhi::vulkan_heap_limits descriptor_heap_limits{};
        bool mesh_shader_available = false, ray_query_available = false, ray_tracing_pipeline_available = false, opacity_micromap_available = false, host_image_copy_available = false;
        PFN_vkCmdDrawMeshTasksEXT mesh_dispatch = nullptr;
        PFN_vkCmdDrawMeshTasksIndirectEXT mesh_dispatch_indirect = nullptr;
        PFN_vkCopyImageToMemoryEXT copy_image_to_memory = nullptr;
        float render_scale = 1.0f;
        VkExtent2D swap_chain_extent{};
        VkFormat swap_chain_image_format{}, depth_attachment_format{};
        int32_t current_frame = 0;
        static constexpr int32_t MAX_FRAMES_IN_FLIGHT = 2;
        static constexpr uint32_t bloom_level_count = 4;
        static constexpr VkDeviceSize heap_slot_stride = 64, heap_sampler_stride = 32;
        static constexpr uint32_t heap_slot_base = 16384, heap_slot_count = 1024, heap_image_capacity = 8, heap_sampler_base = 2048;
        static constexpr VkDeviceSize heap_slot_offset(uint32_t slot) {
            return VkDeviceSize(slot) * heap_slot_stride;
        }
        VkDeviceSize heap_grid_offset = VK_WHOLE_SIZE;
        struct heap_slots {
            static constexpr uint32_t textures = heap_slot_base + 0u;
            static constexpr uint32_t materials = heap_slot_base + 512u;
            /**
             * @brief the top level structure, binding 16, as a TWO-SLOT array - and the reason it is not beside the
             *        per-frame buffers above
             *
             * @note THE TLAS IS REBUILT EVERY FRAME (see runtime::write_rt_structure_binding, which is called per
             *       frame slot), so a single slot would hold one frame's structure while the other frame is still
             *       in flight - the same hazard the camera and light UBOs have, and they are two slots each for it.
             *       This was first laid out as ONE slot beside the per-frame buffers, which is the kind of mistake
             *       a picture cannot show until the shaders read the heap: the fix is cheap now and would have been
             *       a silent wrong image later. It lives at the END of the used region because growing it in place
             *       would renumber every array after it.
             */
            static constexpr uint32_t tlas = heap_slot_base + 703u;
            /**
             * @brief the MESHLET TABLE (docs/mesh_shaders.md step 3): one 48-byte record per meshlet
             *
             * @note ONE SLOT, not a per-frame pair, and that is a property of the data rather than a shortcut: the
             *       table is written ONCE, while the scene is imported and before any frame is recorded, and never
             *       touched again - so there is no frame in flight whose contents could disagree with it. (The TLAS
             *       above is the counter-example: it is rebuilt every frame, which is why it owns two slots.)
             * @note it lives at the END of the used region for the reason stated there: the arrays above are
             *       numbered by their position, so growing one in place would renumber everything after it.
             */
            static constexpr uint32_t meshlets = heap_slot_base + 745u;
            /**
             * @brief the MESH CULLING COUNTERS (docs/mesh_shaders.md step 3, "what the culling buys"): one per-frame
             *        lane of 8 uints, added to by the mesh entries and read back by the host at shutdown
             *
             * @note ON THE HEAP rather than in a host-only buffer, because a mesh stage's only way to reach memory
             *       is a heap descriptor - there is no binding model left to hang a counter off. The read-back is a
             *       plain mapped read after `wait_idle`, which is why the buffer is host-visible and coherent.
             */
            static constexpr uint32_t meshlet_stats = heap_slot_base + 746u;
            /**
             * @brief THE HOST-CULLED MESHLET TABLE (docs/mesh_shaders.md step 3, the culling's cheapest stage): a
             *        per-frame lane of `meshlet_capacity` records, written by the host while it records a CULLED
             *        session's draws and read by that session's mesh entry
             *
             * @note per frame rather than one slot, unlike the table itself: this one is rewritten every frame from
             *       the camera, so the frame in flight that is being recorded must not overwrite the one the GPU is
             *       still reading - the rule every per-frame buffer in this renderer follows.
             * @note the compaction is the whole point: the host writes SURVIVORS contiguously, so the dispatch's
             *       group count is the survivor count and no workgroup is launched for a culled meshlet.
             */
            static constexpr uint32_t meshlet_culled = heap_slot_base + 747u;

            static constexpr uint32_t toon_lanes = heap_slot_base + 748u;

            static constexpr uint32_t scene_head = heap_slot_base + 749u;

            static constexpr uint32_t toon_rig = heap_slot_base + 751u;

            static constexpr uint32_t toon_colours = heap_slot_base + 752u;

            static constexpr uint32_t post_lut = heap_slot_base + 753u;
            /**
             * THE GOO REFERENCE'S PRE-INTEGRATED FGD LOOKUP TABLE (see `runtime::set_goo_fgd_lut` and
             * `heap_slots_goo_fgd_lut` in the shader's slot file, which `test_render_resources` holds against this
             * spelling).
             *
             * 754 AND NOT 750: `scene_head` is a PER-FRAME-SLOT array and occupies 749 AND 750 - the note on
             * `toon_rig` above records what happened to the last constant that forgot this - so the next free slot
             * above the post LUT is 754.
             *
             * IT IS A GLOBAL IMAGE RATHER THAN A MATERIAL LANE, which is the architecture ruling the step-5 spec
             * makes from the reference's own graph (§3.4): the Goo `GetPreIntegratedFGDGGXAndDisneyDiffuse` group
             * has ONE `ShaderNodeTexImage`, `users == 3` containers share it, and its coordinate is computed from
             * `sqrt(NoV)` / `perceptualRoughness` / `fresnel0` - never from a material's uv. Routing it through
             * `toon_slot` would need eleven sidecar rows pointing at one file, and zero new information.
             */
            static constexpr uint32_t goo_fgd_lut = heap_slot_base + 754u;
            static constexpr uint32_t scene_camera = heap_slot_base + 514u;
            static constexpr uint32_t scene_light = heap_slot_base + 516u;
            static constexpr uint32_t cluster_counts = heap_slot_base + 518u;
            static constexpr uint32_t cluster_indices = heap_slot_base + 520u;
            static constexpr uint32_t instance_transforms = heap_slot_base + 522u;
            /** @brief the transforms of the previous frame, kept for motion vectors and TAA reprojection */
            static constexpr uint32_t previous_transforms = heap_slot_base + 524u;
            /** @brief the skinned joint matrices for this frame, written by the skinning pass */
            static constexpr uint32_t skin_matrices = heap_slot_base + 526u;
            static constexpr uint32_t morph_data = heap_slot_base + 528u;
            static constexpr uint32_t mask_instances = heap_slot_base + 530u;
            static constexpr uint32_t env_cube = heap_slot_base + 532u;
            static constexpr uint32_t irradiance_cube = heap_slot_base + 533u;
            static constexpr uint32_t brdf_lut = heap_slot_base + 534u;
            static constexpr uint32_t shadow_map = heap_slot_base + 535u;
            /** @brief the ray-traced visibility image, sampled by the lighting stage */
            static constexpr uint32_t rt_visibility = heap_slot_base + 543u;
            /**
             * @brief the SAME image as @ref rt_visibility, as a STORAGE descriptor instead of a sampled one
             *
             * @note TWO DESCRIPTORS FOR ONE IMAGE, and not redundancy: SAMPLED_IMAGE and STORAGE_IMAGE are different
             *       descriptor kinds and no single heap descriptor is both, while this image is WRITTEN by the
             *       ray-traced visibility pass and SAMPLED by the lighting stage. It lives at the end of the used
             *       region for the same reason the TLAS does - growing an array in place would renumber every array
             *       after it.
             */
            static constexpr uint32_t rt_visibility_storage = heap_slot_base + 711u;
            static constexpr uint32_t gbuffer_albedo = heap_slot_base + 551u;
            static constexpr uint32_t gbuffer_normal = heap_slot_base + 559u;
            static constexpr uint32_t gbuffer_material = heap_slot_base + 567u;
            static constexpr uint32_t gbuffer_depth = heap_slot_base + 575u;
            static constexpr uint32_t gbuffer_velocity = heap_slot_base + 583u;
            static constexpr uint32_t ml_trace = heap_slot_base + 591u;

            static constexpr uint32_t ml_trace_storage = heap_slot_base + 719u;
            static constexpr uint32_t ml_resolved_storage = heap_slot_base + 735u;
            /**
             * @brief the joint blocks as they were ONE FRAME AGO, per frame slot: the deformation half of a
             *        motion vector
             *
             * @note A SECOND per-frame family rather than more slots inside @ref skin_matrices, because a
             *       vertex's motion vector needs the matrices the PREVIOUS frame drew with and the current
             *       buffer has already been overwritten with this frame's by the time the frame records.
             *       The layout, the indices and the frame-slot rule are identical to the current family's -
             *       that is what lets the shader read the same `skin_base` from this slot and lets the
             *       runtime publish into the CURRENT frame slot's buffer (see
             *       runtime::advance_motion_deformations), exactly as @ref previous_transforms does for the
             *       world matrices. It lives at the END of the used region for the same reason the TLAS and
             *       the two storage twins do: growing an array in place would renumber every array after it.
             */
            static constexpr uint32_t skin_matrices_previous = heap_slot_base + 743u;
            static constexpr uint32_t ml_history = heap_slot_base + 599u;
            static constexpr uint32_t ml_resolved = heap_slot_base + 607u;
            static constexpr uint32_t taa_current = heap_slot_base + 623u;
            static constexpr uint32_t taa_history = heap_slot_base + 631u;
            static constexpr uint32_t post_color = heap_slot_base + 639u;
            static constexpr uint32_t bloom_l0 = heap_slot_base + 647u;
            static constexpr uint32_t bloom_l1 = heap_slot_base + 655u;
            static constexpr uint32_t bloom_l2 = heap_slot_base + 663u;
            static constexpr uint32_t bloom_l3 = heap_slot_base + 671u;
            static constexpr uint32_t display_color = heap_slot_base + 695u;
        };
        std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> gpu_timing_marks{};
        std::vector<vk_command_buffer> frame_command_buffers;
        std::vector<VkImage> swap_chain_images;
        std::vector<VkImageView> swap_chain_image_views;
        std::vector<VkImage> depth_images, hdr_images, ldr_images, gbuffer_depth_images, velocity_images, scene_color_images, taa_history_images, ml_images, ml_resolve_images, ml_history_images, rt_shadow_images, furnace_cube_images;
        std::vector<VkImageView> depth_image_views, hdr_image_views, ldr_image_views, gbuffer_depth_image_views, velocity_image_views, scene_color_image_views, taa_history_image_views, ml_image_views, ml_resolve_image_views, ml_history_image_views, rt_shadow_image_views, furnace_cube_views;
        std::array<std::vector<VkImage>, 3> gbuffer_images;
        std::array<std::vector<VkImageView>, 3> gbuffer_image_views;
        std::array<std::vector<VkImage>, 4> bloom_images;
        std::array<std::vector<VkImageView>, 4> bloom_image_views;
        vk_sampler texture_sampler, post_sampler, gbuffer_sampler, post_nearest_sampler, taa_sampler, shadow_sampler;
        explicit engine_device(deren::utility::backend_token token, GLFWwindow* host_window)
            : gpu(std::move(token))
            , vma(gpu)
            , descriptor_heaps(gpu)
            , window(host_window) {
            refresh();
            for (uint32_t slot = 0; slot < MAX_FRAMES_IN_FLIGHT; ++slot) {
                rhi::vulkan_frame call{};
                call.slot = slot;
                if (gpu.escape->service(rhi::vulkan_service::frame_command, &call, sizeof(call)) != rhi::error::ok)
                    deren::utility::panic("backend primary command unavailable");
                frame_command_buffers.emplace_back(static_cast<VkCommandBuffer>(call.command_buffer), gpu);
            }
            create_targets();
            create_samplers();
        }
        ~engine_device() {
            wait_idle();
        }
        void refresh() {
            auto s = snapshot();
            instance = static_cast<VkInstance>(s.instance);
            physical_device = static_cast<VkPhysicalDevice>(s.physical_device);
            logical_device = static_cast<VkDevice>(s.device);
            graphics_queue_handle = static_cast<VkQueue>(s.graphics_queue);
            graphics_queue_family_index = s.queue_family;
            current_frame = static_cast<int32_t>(s.current_frame);
            swap_chain_extent = {s.width, s.height};
            swap_chain_image_format = static_cast<VkFormat>(s.format);
            depth_attachment_format = static_cast<VkFormat>(s.depth_format);
            render_scale = s.render_scale;
            mesh_shader_available = s.mesh_shader;
            ray_query_available = s.ray_query;
            ray_tracing_pipeline_available = s.ray_tracing;
            opacity_micromap_available = s.opacity_micromap;
            host_image_copy_available = s.host_image_copy;
            descriptor_heap_limits = s.heap;
            heap_grid_offset = s.heap_grid_offset;
            mesh_dispatch = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(logical_device, "vkCmdDrawMeshTasksEXT"));
            mesh_dispatch_indirect = reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>(vkGetDeviceProcAddr(logical_device, "vkCmdDrawMeshTasksIndirectEXT"));
            copy_image_to_memory = reinterpret_cast<PFN_vkCopyImageToMemoryEXT>(vkGetDeviceProcAddr(logical_device, "vkCopyImageToMemoryEXT"));
            swap_chain_images.resize(s.image_count);
            swap_chain_image_views.resize(s.image_count);
            for (uint32_t i = 0; i < s.image_count; ++i) {
                rhi::vulkan_frame call{};
                call.image_index = i;
                if (gpu.escape->service(rhi::vulkan_service::swapchain_image, &call, sizeof(call)) != rhi::error::ok)
                    deren::utility::panic("backend swapchain image unavailable");
                swap_chain_images[i] = reinterpret_cast<VkImage>(call.image);
                swap_chain_image_views[i] = reinterpret_cast<VkImageView>(call.image_view);
            }
        }
        VkExtent2D render_extent() const noexcept {
            return {std::max(1u, static_cast<uint32_t>(static_cast<float>(swap_chain_extent.width) * render_scale)), std::max(1u, static_cast<uint32_t>(static_cast<float>(swap_chain_extent.height) * render_scale))};
        }
        void wait_idle() const noexcept {
            static_cast<void>(gpu.escape->service(rhi::vulkan_service::wait_idle, nullptr, 0));
        }
        void set_window_title(std::string_view title) const noexcept {
            std::string text(title);
            glfwSetWindowTitle(window, text.c_str());
        }
        rhi::extension* query_extension(rhi::extension_kind kind) {
            return gpu.api().query_extension(kind);
        }
        rhi::buffer* create_buffer(rhi::buffer_desc const& desc) {
            return gpu.api().create_buffer(desc);
        }
        rhi::command_list* begin_commands() {
            return gpu.api().begin_commands();
        }
        rhi::image* frame_image() {
            return gpu.api().frame_image();
        }
        rhi::buffer* frame_readback_buffer() {
            return gpu.api().frame_readback_buffer();
        }
        rhi::ability_bits abilities() const {
            return gpu.api().abilities();
        }
        void wait_frame_slot(uint32_t slot) const {
            static_cast<void>(frame_call(rhi::vulkan_service::wait_slot, 0, VK_NULL_HANDLE, slot));
        }
        VkResult acquire_next_image(uint32_t& index) {
            rhi::vulkan_frame call{};
            auto err = gpu.escape->service(rhi::vulkan_service::acquire, &call, sizeof(call));
            index = call.image_index;
            return err == rhi::error::ok ? static_cast<VkResult>(call.result) : VK_ERROR_INITIALIZATION_FAILED;
        }
        VkResult submit(VkCommandBuffer command, uint32_t image) {
            return frame_call(rhi::vulkan_service::submit, image, command);
        }
        VkResult present(uint32_t image) const {
            return frame_call(rhi::vulkan_service::present, image);
        }
        void to_next_frame() {
            static_cast<void>(frame_call(rhi::vulkan_service::advance));
            current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
        }
        bool recreate_swap_chain() {
            rhi::vulkan_frame call{};
            wait_idle();
            auto err = gpu.escape->service(rhi::vulkan_service::recreate, &call, sizeof(call));
            if (err != rhi::error::ok || !call.recreated)
                return false;
            refresh();
            create_targets();
            return true;
        }
        bool gpu_timing_available() const noexcept {
            rhi::vulkan_snapshot s{};
            return gpu.escape->service(rhi::vulkan_service::snapshot, &s, sizeof(s)) == rhi::error::ok && s.gpu_timing;
        }
        void begin_gpu_timing(VkCommandBuffer command, uint32_t slot) {
            rhi::vulkan_timing call{};
            call.command_buffer = command;
            call.slot = slot;
            static_cast<void>(gpu.escape->service(rhi::vulkan_service::begin_timing, &call, sizeof(call)));
            gpu_timing_marks[slot] = call.written_marks;
        }
        void mark_gpu_timing(VkCommandBuffer command, uint32_t slot, VkPipelineStageFlagBits stage) {
            rhi::vulkan_timing call{};
            call.command_buffer = command;
            call.slot = slot;
            call.stage = stage;
            static_cast<void>(gpu.escape->service(rhi::vulkan_service::mark_timing, &call, sizeof(call)));
            gpu_timing_marks[slot] = call.written_marks;
        }
        gpu_timing_result read_gpu_timings(uint32_t slot) {
            rhi::vulkan_timing call{};
            call.slot = slot;
            static_cast<void>(gpu.escape->service(rhi::vulkan_service::read_timing, &call, sizeof(call)));
            return {call.milliseconds, call.mark_count};
        }
        VkCommandPool make_command_pool() {
            rhi::vulkan_command call{};
            auto err = gpu.escape->service(rhi::vulkan_service::create_pool, &call, sizeof(call));
            if (err != rhi::error::ok || call.result != VK_SUCCESS)
                deren::utility::panic("recording pool creation failed");
            return reinterpret_cast<VkCommandPool>(call.pool);
        }
        vk_command_buffer make_command_buffer() const {
            return allocate_commands(false, VK_NULL_HANDLE);
        }
        vk_command_buffer make_secondary_command_buffer() const {
            return allocate_commands(true, VK_NULL_HANDLE);
        }
        vk_command_buffer make_secondary_command_buffer(VkCommandPool pool) const {
            return allocate_commands(true, pool);
        }
        vk_command_buffer allocate_commands(bool secondary, VkCommandPool pool) const {
            rhi::vulkan_command call{};
            call.pool = reinterpret_cast<uint64_t>(pool);
            call.secondary = secondary;
            auto err = gpu.escape->service(rhi::vulkan_service::allocate_command, &call, sizeof(call));
            if (err != rhi::error::ok || call.result != VK_SUCCESS)
                deren::utility::panic("command buffer creation failed");
            return {static_cast<VkCommandBuffer>(call.command_buffer), gpu, reinterpret_cast<VkCommandPool>(call.pool)};
        }
        VkResult submit_and_wait(VkCommandBuffer command) const {
            rhi::vulkan_submit_wait call{};
            call.command_buffer = command;
            auto err = gpu.escape->service(rhi::vulkan_service::submit_and_wait, &call, sizeof(call));
            return err == rhi::error::ok ? static_cast<VkResult>(call.result) : VK_ERROR_INITIALIZATION_FAILED;
        }
        std::optional<vk_shader_module> make_shader_module(std::span<uint8_t> code) const {
            return deren::vulkan::make_shader_module(code, gpu);
        }
        vk_image_view make_image_view(VkImage image, VkFormat format, VkImageViewType type, uint32_t layer = 0, bool single_layer = false) const {
            rhi::image* resource = vma.image_resource(image);
            auto found = target_resources.find(image);
            if (found != target_resources.end())
                resource = found->second;
            if (!resource)
                deren::utility::panic("view requested for an unowned image");
            rhi::image_view_desc desc{};
            desc.resource = resource;
            desc.format = contract_format(format);
            desc.dimension = type == VK_IMAGE_VIEW_TYPE_CUBE ? rhi::image_view_dimension::cube : type == VK_IMAGE_VIEW_TYPE_3D     ? rhi::image_view_dimension::texture_3d
                                                                                             : type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? rhi::image_view_dimension::texture_2d_array
                                                                                                                                   : rhi::image_view_dimension::texture_2d;
            if (format == depth_attachment_format)
                desc.aspects = static_cast<uint32_t>(rhi::image_aspect::depth);
            desc.base_array_layer = layer;
            if (single_layer)
                desc.array_layer_count = 1;
            auto* view = gpu.api().create_image_view(desc);
            if (!view)
                deren::utility::panic("image view factory failed");
            return {view, gpu, reinterpret_cast<VkImageView>(gpu.escape->native_image_view(*view))};
        }
        vk_image_view make_depth_array_view(VkImage image, VkFormat format) const {
            return make_image_view(image, format, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
        }
        vk_image_view make_depth_layer_view(VkImage image, VkFormat format, uint32_t layer) const {
            return make_image_view(image, format, VK_IMAGE_VIEW_TYPE_2D, layer, true);
        }
        vk_sampler make_sampler(VkSamplerAddressMode address, float max_lod) const {
            rhi::sampler_desc desc{};
            desc.address_u = desc.address_v = desc.address_w = static_cast<rhi::address_mode>(address);
            desc.max_lod = max_lod;
            return make_sampler(desc);
        }
        vk_sampler make_sampler(rhi::sampler_desc const& desc) const {
            auto* object = gpu.api().create_sampler(desc);
            if (!object)
                deren::utility::panic("sampler factory failed");
            return {object, gpu, reinterpret_cast<VkSampler>(gpu.escape->native_sampler(*object))};
        }
        void create_samplers() {
            texture_sampler = make_sampler(VK_SAMPLER_ADDRESS_MODE_REPEAT, 12);
            post_sampler = make_sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1);
            rhi::sampler_desc nearest{};
            nearest.address_u = nearest.address_v = nearest.address_w = rhi::address_mode::clamp_to_edge;
            nearest.max_lod = 0;
            nearest.min_filter = nearest.mag_filter = rhi::filter::nearest;
            gbuffer_sampler = make_sampler(nearest);
            post_nearest_sampler = make_sampler(nearest);
            nearest.mag_filter = rhi::filter::linear;
            taa_sampler = make_sampler(nearest);
            rhi::sampler_desc shadow{};
            shadow.address_u = shadow.address_v = shadow.address_w = rhi::address_mode::clamp_to_border;
            shadow.max_lod = 0;
            shadow.compare_enabled = 1;
            shadow.comparison = rhi::compare_op::less_or_equal;
            shadow_sampler = make_sampler(shadow);
        }
        void create_targets() {
            target_view_owners.clear();
            target_owners.clear();
            target_resources.clear();
            auto extent = render_extent();
            size_t count = swap_chain_images.size();
            auto family = [&](std::vector<VkImage>& images, std::vector<VkImageView>& views, VkFormat format, uint32_t width, uint32_t height, uint32_t usage, size_t family_count, uint32_t slot, uint32_t storage_slot = 0, bool cube = false) {
                images.clear();
                views.clear();
                for (size_t i = 0; i < family_count; ++i) {
                    rhi::image_desc desc{};
                    desc.extent = {width, height, 1};
                    desc.format = contract_format(format);
                    desc.flags = usage;
                    desc.array_layers = cube ? 6u : 1u;
                    desc.dimension = cube ? rhi::image_dimension::cube : rhi::image_dimension::texture_2d;
                    auto* object = gpu.api().create_image(desc);
                    if (!object)
                        deren::utility::panic("render target factory failed");
                    target_owners.emplace_back(object, gpu, reinterpret_cast<VkImage>(gpu.escape->native_image(*object)));
                    VkImage image = *target_owners.back();
                    target_resources[image] = object;
                    auto view = make_image_view(image, format, cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D);
                    images.push_back(image);
                    views.push_back(*view);
                    target_view_owners.push_back(std::move(view));
                    if (slot) {
                        auto info = make_image_view_info(image, format, cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, format == depth_attachment_format ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 1, cube ? 6 : 1);
                        if (!descriptor_heaps.write_image(heap_slot_offset(slot + static_cast<uint32_t>(i)), info, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE))
                            deren::utility::panic("render target sampled descriptor failed");
                        if (storage_slot && !descriptor_heaps.write_image(heap_slot_offset(storage_slot + static_cast<uint32_t>(i)), info, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE))
                            deren::utility::panic("render target storage descriptor failed");
                    }
                }
            };
            auto sampled = rhi::to_bits(rhi::image_flag::sampled), color = sampled | rhi::to_bits(rhi::image_flag::color_attachment), storage = sampled | rhi::to_bits(rhi::image_flag::storage), history = sampled | rhi::to_bits(rhi::image_flag::transfer_destination);
            family(depth_images, depth_image_views, depth_attachment_format, extent.width, extent.height, rhi::to_bits(rhi::image_flag::depth_attachment), count, 0);
            family(hdr_images, hdr_image_views, hdr_format, extent.width, extent.height, color | rhi::to_bits(rhi::image_flag::transfer_source), count, heap_slots::post_color);
            family(ldr_images, ldr_image_views, hdr_format, extent.width, extent.height, color, count, heap_slots::display_color);
            uint32_t bases[3] = {heap_slots::gbuffer_albedo, heap_slots::gbuffer_normal, heap_slots::gbuffer_material};
            for (uint32_t i = 0; i < 3; ++i)
                family(gbuffer_images[i], gbuffer_image_views[i], gbuffer_formats[i], extent.width, extent.height, color, count, bases[i]);
            family(gbuffer_depth_images, gbuffer_depth_image_views, depth_attachment_format, extent.width, extent.height, sampled | rhi::to_bits(rhi::image_flag::depth_attachment), count, heap_slots::gbuffer_depth);
            family(velocity_images, velocity_image_views, gbuffer_velocity_format, extent.width, extent.height, color, count, heap_slots::gbuffer_velocity);
            family(scene_color_images, scene_color_image_views, hdr_format, extent.width, extent.height, color, count, heap_slots::taa_current);
            family(taa_history_images, taa_history_image_views, hdr_format, extent.width, extent.height, history, count, heap_slots::taa_history);
            auto halfw = std::max(1u, extent.width / 2), halfh = std::max(1u, extent.height / 2);
            family(ml_images, ml_image_views, hdr_format, halfw, halfh, storage, count, heap_slots::ml_trace, heap_slots::ml_trace_storage);
            family(ml_resolve_images, ml_resolve_image_views, hdr_format, halfw, halfh, storage | rhi::to_bits(rhi::image_flag::transfer_source), count, heap_slots::ml_resolved, heap_slots::ml_resolved_storage);
            family(ml_history_images, ml_history_image_views, hdr_format, halfw, halfh, history, count, heap_slots::ml_history);
            family(rt_shadow_images, rt_shadow_image_views, VK_FORMAT_R16_SFLOAT, extent.width, extent.height, storage, MAX_FRAMES_IN_FLIGHT, heap_slots::rt_visibility, heap_slots::rt_visibility_storage);
            family(furnace_cube_images, furnace_cube_views, hdr_format, 1, 1, history, 1, 0, 0, true);
            for (uint32_t i = 0; i < 4; ++i)
                family(bloom_images[i], bloom_image_views[i], hdr_format, std::max(1u, extent.width >> (i + 1)), std::max(1u, extent.height >> (i + 1)), color, count, heap_slots::bloom_l0 + i * heap_image_capacity);
        }
        std::expected<vk_pipeline, std::string_view> make_gbuffer_pipeline(
            std::span<uint8_t const> const vertex_shader_code,
            std::span<uint8_t const> const fragment_shader_code,
            VkShaderStageFlagBits const first_stage = VK_SHADER_STAGE_VERTEX_BIT) const;
        std::expected<vk_pipeline, std::string_view> make_character_forward_pipeline(
            std::span<uint8_t const> const vertex_shader_code,
            std::span<uint8_t const> const fragment_shader_code,
            VkShaderStageFlagBits const first_stage = VK_SHADER_STAGE_VERTEX_BIT) const;
        std::expected<vk_pipeline, std::string_view> make_overlay_pipeline(
            std::span<uint8_t const> const vertex_shader_code,
            std::span<uint8_t const> const fragment_shader_code,
            VkShaderStageFlagBits const first_stage = VK_SHADER_STAGE_VERTEX_BIT) const;
        std::expected<vk_pipeline, std::string_view> make_outline_pipeline(
            std::span<uint8_t const> const vertex_shader_code,
            std::span<uint8_t const> const fragment_shader_code,
            VkShaderStageFlagBits const first_stage = VK_SHADER_STAGE_VERTEX_BIT) const;
        std::expected<vk_pipeline, std::string_view> make_depth_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> const fragment_shader_code,
            VkFormat const depth_format,
            float const depth_bias_constant_factor,
            float const depth_bias_slope_factor,
            float const depth_bias_clamp) const;
    };
    std::expected<vk_pipeline, std::string_view> engine_device::make_gbuffer_pipeline(
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkShaderStageFlagBits const first_stage) const {
        // Five color targets: the three surface targets, the motion vectors, and the scene color the
        // pass ADDS the emissive term into (lighting-independent, and it needs the emissive texture and
        // the UVs the G-buffer does not store - see engine_device::gbuffer_pass_attachment_count). The first
        // four are overwritten, the scene color accumulates, so the blend states differ per attachment.
        std::array<VkFormat, gbuffer_pass_attachment_count> const formats = {
            gbuffer_formats[0],
            gbuffer_formats[1],
            gbuffer_formats[2],
            gbuffer_velocity_format,
            hdr_format,
        };
        std::array<VkPipelineColorBlendAttachmentState, gbuffer_pass_attachment_count> const blends = {
            make_color_blend_attachment_opaque(),
            make_color_blend_attachment_opaque(),
            make_color_blend_attachment_opaque(),
            make_color_blend_attachment_opaque(), // motion vectors are data, not coverage
            make_color_blend_attachment_additive(),
        };
        auto result = deren::vulkan::make_pipeline(
            this->gpu,
            std::span<VkFormat const>(formats),
            this->depth_attachment_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // a G-buffer is never multisampled (see gbuffer_formats)
            true,                  // depth test + write: opaque geometry, and the lighting pass needs depth
            0.0f,
            0.0f,
            0.0f,
            std::span<VkPipelineColorBlendAttachmentState const>(blends),
            // ... and the stage that emits the geometry: MESH when the caller passes a mesh entry (then the vertex
            // input state is not derived from it at all - a mesh stage declares no Input variables).
            first_stage);
        if (result) {
            // same fullscreen viewport/scissor default as the forward pipelines (the frame path
            // re-syncs it on every swapchain recreation). The RENDER extent, because this pipeline draws
            // into a render target from the scene chain - see `update_pass_geometry`, which resyncs exactly
            // the pipelines that carry these stored values.
            result->viewport = {
                0.0f,
                0.0f,
                static_cast<float>(this->render_extent().width),
                static_cast<float>(this->render_extent().height),
                0.0f,
                1.0f,
            };
            result->scissor = {{0, 0}, this->render_extent()};
        }
        return result;
    }
    std::expected<vk_pipeline, std::string_view> engine_device::make_character_forward_pipeline(
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkShaderStageFlagBits const first_stage) const {
        // ONE colour target, and it is the HDR one: this pass runs inside the HDR chain (after the lighting
        // stage, before the resolve), so the tonemap stays the post chain's - see the declaration's note.
        std::array<VkFormat, 1> const formats = {hdr_format};
        // STANDARD ALPHA BLENDING, AND FOR AN OPAQUE MATERIAL IT IS BIT-IDENTICAL TO THE OVERWRITE THIS
        // REPLACES: `src * srcAlpha + dst * (1 - srcAlpha)` at `srcAlpha == 1` is `src`, and the character stage
        // writes an alpha of exactly 1 for every material that is not the article's transparent variant (see
        // `toon_inputs::alpha_blend`). That is why one pipeline can serve both: the blend state is per-PASS in
        // this renderer, and the PER-MATERIAL half of the author's `Blend [_SrcBlend] [_DstBlend]` - chen's
        // `cloth_02` is `SrcAlpha` / `OneMinusSrcAlpha` - travels as the fragment stage's coverage instead.
        //
        // THE COST OF SHARING THE PIPELINE, stated rather than hidden: the HDR target's ALPHA channel is now
        // read-modify-written by this pass instead of overwritten. Nothing consumes it - `post.slang` returns
        // `float4(color, 1.0)` and samples `.rgb` - and the A/B that proves the rest of the character is
        // unaffected (0 px) is in `remaining_port_spec.md`'s "其余部位按参考对齐（续）" item 10.
        std::array<VkPipelineColorBlendAttachmentState, 1> const blends = {make_color_blend_attachment()};
        auto result = deren::vulkan::make_pipeline(
            this->gpu,
            std::span<VkFormat const>(formats),
            this->depth_attachment_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // the HDR chain is single-sampled, like the G-buffer it re-shades
            true,                  // the depth TEST is on; the WRITE is turned off per draw (dynamic state)
            0.0f,
            0.0f,
            0.0f,
            std::span<VkPipelineColorBlendAttachmentState const>(blends),
            first_stage,
            // THE ONE OPERATOR IN THE RENDERER THAT IS NOT LESS_OR_EQUAL: it confines the overwrite to the
            // surface the G-buffer pass recorded, which is what makes this pass a replacement rather than a
            // second layer over geometry that is merely in front.
            VK_COMPARE_OP_EQUAL);
        if (result) {
            result->viewport = {
                0.0f,
                0.0f,
                static_cast<float>(this->render_extent().width),
                static_cast<float>(this->render_extent().height),
                0.0f,
                1.0f,
            };
            result->scissor = {{0, 0}, this->render_extent()};
        }
        return result;
    }
    std::expected<vk_pipeline, std::string_view> engine_device::make_overlay_pipeline(
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkShaderStageFlagBits const first_stage) const {
        // ONE colour target, and it is the HDR one - the same target the character-forward stage just wrote, so
        // that the multiply lands on the TOON result rather than on a resolved copy of it. The grade and the
        // tonemap stay the post chain's, exactly as they are for the toon stage itself.
        std::array<VkFormat, 1> const formats = {hdr_format};
        // THE MULTIPLY (see make_color_blend_attachment_multiply): this is the state that makes the article's
        // two `Trick` shaders overlays rather than surfaces, and the reason the port needs no blend extension.
        std::array<VkPipelineColorBlendAttachmentState, 1> const blends = {make_color_blend_attachment_multiply()};
        auto result = deren::vulkan::make_pipeline(
            this->gpu,
            std::span<VkFormat const>(formats),
            this->depth_attachment_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // single-sampled, like the toon stage and the G-buffer it re-shades
            true,                  // the depth TEST is on; the WRITE is turned off per draw (dynamic state)
            0.0f,
            0.0f,
            0.0f,
            std::span<VkPipelineColorBlendAttachmentState const>(blends),
            first_stage,
            // LESS_OR_EQUAL, AND NOT THE TOON STAGE'S EQUAL - the one place the two pipelines differ beyond the
            // blend, and the difference is a property of the geometry rather than a preference. The toon stage
            // draws the SAME triangles the G-buffer recorded, so `EQUAL` is what confines its overwrite to that
            // surface. An overlay mask is a DIFFERENT mesh (36 and 246 vertices on `chars\chen_full2.glb`) whose
            // quads sit a little IN FRONT of the surface they darken - an `EQUAL` test would reject almost every
            // fragment of it and the masks would draw nothing at all. `LESS_OR_EQUAL` is also the article's own
            // state (`ZTest` default, `ZWrite Off`), so the port is reproducing it rather than working around it.
            VK_COMPARE_OP_LESS_OR_EQUAL);
        if (result) {
            result->viewport = {
                0.0f,
                0.0f,
                static_cast<float>(this->render_extent().width),
                static_cast<float>(this->render_extent().height),
                0.0f,
                1.0f,
            };
            result->scissor = {{0, 0}, this->render_extent()};
        }
        return result;
    }
    std::expected<vk_pipeline, std::string_view> engine_device::make_outline_pipeline(
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkShaderStageFlagBits const first_stage) const {
        // ONE colour target, and it is the HDR one - the same target the character-forward stage has just
        // written, so the hull lands in the TOON result rather than on a resolved copy of it. The grade and the
        // tonemap stay the post chain's, exactly as they are for the toon stage and for the overlay group.
        std::array<VkFormat, 1> const formats = {hdr_format};
        // OVERWRITE, NOT MULTIPLY: the article's outline is an OPAQUE surface - `MyZmdOutlineShader`'s SubShader
        // states no `Blend` at all and its fragment returns alpha 1 - so this is the state the TOON stage uses
        // (`make_color_blend_attachment_opaque`), not the overlay group's multiply.
        std::array<VkPipelineColorBlendAttachmentState, 1> const blends = {make_color_blend_attachment_opaque()};
        auto result = deren::vulkan::make_pipeline(
            this->gpu,
            std::span<VkFormat const>(formats),
            this->depth_attachment_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // the HDR chain is single-sampled, like the G-buffer it re-shades
            true,                  // the depth TEST is on; the WRITE is turned off per draw (dynamic state)
            0.0f,
            0.0f,
            0.0f,
            std::span<VkPipelineColorBlendAttachmentState const>(blends),
            first_stage,
            // LESS_OR_EQUAL, AND THE ARTICLE'S OWN `ZTest` DEFAULT rather than the toon stage's `EQUAL`. The
            // difference is the geometry: a hull is the same mesh pushed OUTWARD, so the fragments that survive
            // front-face culling are the ring just OUTSIDE the silhouette - where the depth buffer holds whatever
            // is behind the character (nothing, or a farther surface). `EQUAL` would reject exactly those
            // fragments (they are not the surface the G-buffer recorded) and the outline would draw nothing at
            // all; `LESS_OR_EQUAL` keeps the ring and is what confines the hull's interior to the surface that
            // already covers it. See `character_forward.cpp`, which states the depth-write half of this.
            VK_COMPARE_OP_LESS_OR_EQUAL);
        if (result) {
            result->viewport = {
                0.0f,
                0.0f,
                static_cast<float>(this->render_extent().width),
                static_cast<float>(this->render_extent().height),
                0.0f,
                1.0f,
            };
            result->scissor = {{0, 0}, this->render_extent()};
        }
        // CULL FRONT IS NOT HERE, AND THAT IS THE POINT RATHER THAN AN OMISSION: the rasterization state is
        // DYNAMIC in this renderer (every leaf's draw() calls `set_cull_mode` with its own `doubleSided` flag), so
        // a Cull Front stated in the pipeline would be overwritten by the first hull drawn with it. The front-face
        // culling that makes an inverted hull an outline is `render_environment::forced_cull_front`, which the
        // character-forward pass sets around this group alone (see character_forward.cpp).
        return result;
    }
    std::expected<vk_pipeline, std::string_view> engine_device::make_depth_pipeline(
        std::span<uint8_t const> vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkFormat const depth_format,
        float const depth_bias_constant_factor,
        float const depth_bias_slope_factor,
        float const depth_bias_clamp) const {
        auto result = deren::vulkan::make_pipeline(
            this->gpu,
            VK_FORMAT_UNDEFINED, // no color attachment
            depth_format,
            vertex_shader_code,
            fragment_shader_code,
            VK_SAMPLE_COUNT_1_BIT, // the shadow map is single-sampled
            true,                  // depth test + write
            false,                 // no color attachment
            depth_bias_constant_factor,
            depth_bias_slope_factor,
            depth_bias_clamp);
        // viewport/scissor are dynamic states set by the caller before drawing (the shadow map
        // is a fixed-size target, so engine_device::make_pipeline's swapchain-size defaults do not apply)
        return result;
    }
} // namespace deren::vulkan
