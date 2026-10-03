// ============================================================================
// module: deren.vulkan.runtime:probes  - the heap probes, which are diagnostics rather than production
//
// run_heap_probe reads back one slot of the bindless texture array to prove the host wrote it, and
// run_heap_graphics_probe renders the bindings of one material through the heap-native shaders and
// reads the result back. Both exist to answer "did the heap get what the shader expects" without a
// validation layer, which is how this migration was debugged - and neither belongs in the production
// runtime, so they live in their own partition.
//
// Imports are NOT transitive: this partition imports what the probe code calls for itself.
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
#include <vector> // the destination of a host image copy, when that read-back path is taken
#include <vulkan/vulkan.h>

module deren.vulkan.runtime:probes;

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

// Route std::pmr allocations through mimalloc for this TU (deren.utility:better_pmr). Idempotent:
// init_pmr() returns the same process-wide singleton no matter which TU calls it first, so
// main.cpp's keep-alive and this one coexist safely. The reference itself is never read; it
// only forces the (dynamic) initialization before any pmr container in this TU is constructed.
[[maybe_unused]] static auto& pmr = deren::utility::init_pmr(); // NOLINT(keep-alive)

namespace deren::vulkan {
    void runtime::run_heap_probe(uint32_t const texture_slot) {
        // ---- THE HEAP-NATIVE PROBE (see shaders/heap_probe_comp.slang and docs/descriptor_heap_migration.md) ----
        //
        // The whole migration assumes four things about the native path, and this is where they stop being
        // assumptions: a pipeline created with VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT and NO layout, a
        // shader that declares its resources with `descriptor_heap` and indexes the grid by slot, a combined image
        // sampler CONSTRUCTED at the use site from a sampler in the sampler heap, and its parameters delivered by
        // vkCmdPushDataEXT. It runs in its own command buffer at scene setup, which is what makes it isolated -
        // the mask bake looked isolated too and turned out to record into the frame's own buffer.
        core& vk = this->vulkan_core;
        // Slot 0 of the sampler heap is the texture sampler: the first of the six core::create_samplers makes, in
        // the order shaders/heap_slots.glsl names (the contract test compares that order, and the host has no
        // per-sampler constant because it keeps them as a list).
        uint32_t const sampler_slot = static_cast<uint32_t>(core::heap_sampler_base);
        std::span<uint8_t const> const spirv = this->registered_shader("heap_probe.comp.spv");
        if (!vk.descriptor_heaps.ready() || vk.heap_grid_offset == VK_WHOLE_SIZE || spirv.empty()) {
            return; // no heap, no grid or no shader: nothing to probe with, and no heap path to protect
        }
        auto const built = pipelines::build_heap_probe(vk.logical_device, spirv);
        if (!built.has_value()) {
            deren::utility::log("descriptor heap: the heap-native probe's pipeline was refused: {}", built.error());
            return;
        }

        // The answer's buffer: 16 bytes, host-visible, and ADDRESSABLE because the push carries its address.
        // THE CONTRACT OWNS IT (object_manager releases the reference on every path out of this function,
        // including the two early returns below), and `mapped()` is the byte view `create_buffer` threw away
        // for everything that is not host-visible.
        rhi::object_manager<rhi::buffer> answer{this->vulkan_core.create_buffer(rhi::buffer_desc{
            .size = 16u,
            .usage = rhi::buffer_usage::storage_coherent,
            .flags = rhi::to_bits(rhi::buffer_flag::device_address),
        })};
        std::span<std::byte> const answer_bytes = answer ? answer->mapped() : std::span<std::byte>{};
        if (answer_bytes.empty()) {
            deren::utility::log("descriptor heap: the heap-native probe could not allocate its answer buffer");
            return;
        }
        uint64_t const answer_address = this->buffer_address(*answer);

        VkCommandPoolCreateInfo const pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .pNext = nullptr, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = vk.graphics_queue_family_index};
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(vk.logical_device, &pool_info, nullptr, &pool);
        VkCommandBufferAllocateInfo const allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .pNext = nullptr, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(vk.logical_device, &allocate, &command_buffer);
        VkCommandBufferBeginInfo const begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr};
        vkBeginCommandBuffer(command_buffer, &begin);
        // The heaps first: they are command-buffer state, and this buffer holds nothing else.
        vk.descriptor_heaps.record_bind(command_buffer);
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, built->trace->get_pipeline());
        // The parameters, THROUGH PUSH DATA: there is no pipeline layout to push constants to, which is the flag's
        // requirement and the reason this function exists.
        std::array<uint32_t, 4> const push = {
            static_cast<uint32_t>(answer_address & 0xFFFFFFFFu),
            static_cast<uint32_t>(answer_address >> 32u),
            texture_slot,
            sampler_slot, // the host's choice of sampler, not the shader's
        };
        [[maybe_unused]] bool const pushed = vk.descriptor_heaps.push_data(command_buffer, 0u, std::as_bytes(std::span(push)));
        vkCmdDispatch(command_buffer, 1u, 1u, 1u);
        vkEndCommandBuffer(command_buffer);

        VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(vk.logical_device, &fence_info, nullptr, &fence);
        VkSubmitInfo const submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                     .pNext = nullptr,
                                     .waitSemaphoreCount = 0,
                                     .pWaitSemaphores = nullptr,
                                     .pWaitDstStageMask = nullptr,
                                     .commandBufferCount = 1,
                                     .pCommandBuffers = &command_buffer,
                                     .signalSemaphoreCount = 0,
                                     .pSignalSemaphores = nullptr};
        vkQueueSubmit(vk.graphics_queue_handle, 1, &submit, fence);
        vkWaitForFences(vk.logical_device, 1, &fence, VK_TRUE, UINT64_MAX);

        uint32_t const readback = *reinterpret_cast<uint32_t const*>(answer_bytes.data());
        uint32_t const material_readback = reinterpret_cast<uint32_t const*>(answer_bytes.data())[1];
        deren::utility::log("descriptor heap: the heap-native probe sampled grid slot {} through sampler slot {} and read back 0x{:08x} (texture red 0x{:04x}, alpha 0x{:04x}); the material table's DEFAULT record read 0x{:04x} (its white base colour is 0xffff)",
                            texture_slot,
                            sampler_slot,
                            readback,
                            readback & 0xFFFFu,
                            readback >> 16u,
                            material_readback & 0xFFFFu);

        vkDestroyFence(vk.logical_device, fence, nullptr);
        vkDestroyCommandPool(vk.logical_device, pool, nullptr);
    }

    void runtime::run_heap_graphics_probe(uint32_t const material_slot, bool const mesh_shader) {
        // ---- THE GRAPHICS HALF OF THE HEAP-NATIVE PROBE (see shaders/heap_probe.slang) ----
        //
        // The compute probe proved the mechanism for a compute pipeline; this is the same question for the kind
        // the frame is mostly made of. It renders into a target CLEARED TO BLACK first, so a white pixel can only
        // have come from the fragment stage's read of the heap rather than from the clear or from a default.
        //
        // AND IT RUNS TWICE (docs/mesh_shaders.md step 0): once with the triangle emitted by a VERTEX stage and
        // once by a MESH stage. Same fragment stage, same target, same readback, one different stage type - so
        // the two log lines compare the two pipeline kinds directly, and the mesh one is what proves a mesh
        // pipeline can be created heap-natively (the heap flag, a NULL layout) and dispatched with
        // vkCmdDrawMeshTasksEXT at all.
        core& vk = this->vulkan_core;
        std::span<uint8_t const> const vertex_code = this->registered_shader(mesh_shader ? "heap_probe.mesh.spv" : "heap_probe.vert.spv");
        std::span<uint8_t const> const fragment_code = this->registered_shader("heap_probe.frag.spv");
        if (!vk.descriptor_heaps.ready() || vk.heap_grid_offset == VK_WHOLE_SIZE || vertex_code.empty() || fragment_code.empty()) {
            return;
        }
        constexpr VkFormat probe_format = VK_FORMAT_R8G8B8A8_UNORM;
        auto const built = pipelines::build_heap_probe_graphics(vk.logical_device, probe_format, vertex_code, fragment_code, mesh_shader ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT);
        // vkCmdDrawMeshTasksEXT IS AN EXTENSION ENTRY POINT and is loaded the way this project loads every other
        // one (see acceleration_structure.cpp): the loader's import library does not export it, so it arrives
        // through vkGetDeviceProcAddr - and a null there is the honest "this device cannot run this probe"
        // instead of a link error. It is fetched only for the mesh arm, so the vertex arm cannot be affected.
        PFN_vkCmdDrawMeshTasksEXT draw_mesh_tasks = nullptr;
        if (mesh_shader) {
            draw_mesh_tasks = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(vkGetDeviceProcAddr(vk.logical_device, "vkCmdDrawMeshTasksEXT"));
            if (draw_mesh_tasks == nullptr) {
                deren::utility::log("descriptor heap: the MESH probe is skipped - vkGetDeviceProcAddr returned null for vkCmdDrawMeshTasksEXT");
                return;
            }
        }
        if (!built.has_value()) {
            deren::utility::log("descriptor heap: the heap-native graphics probe's pipeline was refused: {}", built.error());
            return;
        }

        // ---- THE READ-BACK PATH, NOW SINGLE (docs/host_image_copy.md) ----
        // With VK_EXT_host_image_copy the copy out of the image is performed by the IMPLEMENTATION into a pointer
        // of the app's own memory: the target needs the HOST_TRANSFER usage instead of TRANSFER_SRC, no staging
        // buffer is created and no copy command is recorded. The capability is REQUIRED of the device now, so
        // there is no second path to choose between and no predicate for later branches to disagree about.
        constexpr VkDeviceSize probe_bytes = static_cast<VkDeviceSize>(pipelines::heap_probe_extent) * pipelines::heap_probe_extent * 4u;
        image_create_info target_info = {};
        target_info.width = pipelines::heap_probe_extent;
        target_info.height = pipelines::heap_probe_extent;
        target_info.mip_levels = 1;
        target_info.array_layers = 1;
        target_info.format = probe_format;
        target_info.extra_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
        vk_image target = vk.vma.create_image(nullptr, 0, target_info, image_type::texture_2d);
        auto const* const target_detail = target.valid() ? vk.vma.get_image_detail(target.handle()) : nullptr;
        if (target_detail == nullptr) {
            deren::utility::log("descriptor heap: the heap-native graphics probe could not allocate its target");
            return;
        }
        vk_image_view target_view = vk.make_image_view(target_detail->image, probe_format, VK_IMAGE_VIEW_TYPE_2D);
        if (*target_view == VK_NULL_HANDLE) {
            deren::utility::log("descriptor heap: the heap-native graphics probe could not prepare its target view");
            return;
        }

        VkCommandPoolCreateInfo const pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .pNext = nullptr, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = vk.graphics_queue_family_index};
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(vk.logical_device, &pool_info, nullptr, &pool);
        VkCommandBufferAllocateInfo const allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .pNext = nullptr, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(vk.logical_device, &allocate, &command_buffer);
        VkCommandBufferBeginInfo const begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pNext = nullptr, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = nullptr};
        vkBeginCommandBuffer(command_buffer, &begin);
        vk.descriptor_heaps.record_bind(command_buffer);

        VkImageMemoryBarrier2 to_colour = {};
        to_colour.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_colour.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        to_colour.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_colour.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_colour.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_colour.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_colour.image = target_detail->image;
        to_colour.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo const to_colour_dependency = make_image_dependency_info(1, &to_colour);
        vkCmdPipelineBarrier2(command_buffer, &to_colour_dependency);

        VkRenderingAttachmentInfo const attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                      .pNext = nullptr,
                                                      .imageView = *target_view,
                                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                      .resolveMode = VK_RESOLVE_MODE_NONE,
                                                      .resolveImageView = VK_NULL_HANDLE,
                                                      .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                      .clearValue = {.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}}};
        VkRenderingInfo const rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                           .pNext = nullptr,
                                           .flags = 0,
                                           .renderArea = {{0, 0}, {pipelines::heap_probe_extent, pipelines::heap_probe_extent}},
                                           .layerCount = 1,
                                           .viewMask = 0,
                                           .colorAttachmentCount = 1,
                                           .pColorAttachments = &attachment,
                                           .pDepthAttachment = nullptr,
                                           .pStencilAttachment = nullptr};
        vkCmdBeginRendering(command_buffer, &rendering);
        // The slot, THROUGH PUSH DATA: the pipeline has no layout (the flag requires that), so this is the only
        // way a parameter reaches the fragment stage - and running the probe with a wrong value here is the
        // negative proof (see the caller).
        std::array<uint32_t, 4> const push = {material_slot, 0u, 0u, 0u};
        [[maybe_unused]] bool const pushed = vk.descriptor_heaps.push_data(command_buffer, 0u, std::as_bytes(std::span(push)));
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, built->get_pipeline());
        if (mesh_shader) {
            // ONE workgroup, ONE triangle: the mesh entry emits three vertices and one index triple, and the
            // launch size is the dispatch's business (the shader's own [numthreads(1, 1, 1)] sizes the group).
            // This call is the whole host-side difference between the two probes - there is no vertex buffer,
            // no vertex input and no layout either way.
            draw_mesh_tasks(command_buffer, 1u, 1u, 1u);
        } else {
            vkCmdDraw(command_buffer, 3u, 1u, 0u, 0u); // the fullscreen triangle the vertex entry builds from SV_VertexID
        }
        vkCmdEndRendering(command_buffer);

        VkImageMemoryBarrier2 to_copy = {};
        to_copy.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_copy.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_copy.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        // The consumer of the render's writes is the HOST stage: the implementation performs the copy between
        // image memory and the app's pointer, so this barrier is what makes those writes visible to it.
        to_copy.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        to_copy.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        to_copy.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_copy.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_copy.image = target_detail->image;
        to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo const to_copy_dependency = make_image_dependency_info(1, &to_copy);
        vkCmdPipelineBarrier2(command_buffer, &to_copy_dependency);

        vkEndCommandBuffer(command_buffer);

        VkFenceCreateInfo const fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(vk.logical_device, &fence_info, nullptr, &fence);
        VkSubmitInfo const submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                     .pNext = nullptr,
                                     .waitSemaphoreCount = 0,
                                     .pWaitSemaphores = nullptr,
                                     .pWaitDstStageMask = nullptr,
                                     .commandBufferCount = 1,
                                     .pCommandBuffers = &command_buffer,
                                     .signalSemaphoreCount = 0,
                                     .pSignalSemaphores = nullptr};
        vkQueueSubmit(vk.graphics_queue_handle, 1, &submit, fence);
        vkWaitForFences(vk.logical_device, 1, &fence, VK_TRUE, UINT64_MAX);

        // ---- the host copy itself: no command records it and no queue runs it, and it is legal HERE because the
        //      barrier above has been submitted and waited on (the render's writes are visible to the host stage and
        //      the image is in GENERAL, the layout this renderer keeps every image in). The region is the whole image
        //      tightly packed (memoryRowLength and memoryImageHeight 0 - both a multiple of the texel block extent,
        //      and pHostPointer large enough for it).
        std::vector<uint8_t> host_pixels(static_cast<std::size_t>(probe_bytes));
        VkImageToMemoryCopy const host_region = {.sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY_EXT,
                                                 .pNext = nullptr,
                                                 .pHostPointer = host_pixels.data(),
                                                 .memoryRowLength = 0,
                                                 .memoryImageHeight = 0,
                                                 .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                                 .imageOffset = {0, 0, 0},
                                                 .imageExtent = {pipelines::heap_probe_extent, pipelines::heap_probe_extent, 1}};
        VkCopyImageToMemoryInfo const copy_info = {.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO_EXT,
                                                   .pNext = nullptr,
                                                   .flags = 0,
                                                   .srcImage = target_detail->image,
                                                   .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                   .regionCount = 1,
                                                   .pRegions = &host_region};
        VkResult const copied = vk.copy_image_to_memory(vk.logical_device, &copy_info);
        if (copied != VK_SUCCESS) {
            deren::utility::log("descriptor heap: the heap-native {} probe's HOST image copy failed (VkResult {})", mesh_shader ? "MESH" : "GRAPHICS", static_cast<int>(copied));
            vkDestroyFence(vk.logical_device, fence, nullptr);
            vkDestroyCommandPool(vk.logical_device, pool, nullptr);
            return;
        }
        uint8_t const* const pixel = host_pixels.data();
        deren::utility::log("descriptor heap: the heap-native {} probe rendered grid slot {} into a {}x{} target and read back rgba {},{},{},{} (the default material's white base colour is 255,255,255,255, so the WRONG slot proves the index selects the descriptor)",
                            mesh_shader ? "MESH" : "GRAPHICS",
                            material_slot,
                            pipelines::heap_probe_extent,
                            pipelines::heap_probe_extent,
                            pixel[0],
                            pixel[1],
                            pixel[2],
                            pixel[3]);
        deren::utility::log("descriptor heap: the heap-native {} probe read that pixel back through the HOST IMAGE COPY (no staging buffer, no copy command)",
                            mesh_shader ? "MESH" : "GRAPHICS");

        vkDestroyFence(vk.logical_device, fence, nullptr);
        vkDestroyCommandPool(vk.logical_device, pool, nullptr);
    }

} // namespace deren::vulkan