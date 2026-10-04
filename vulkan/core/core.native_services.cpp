// Vulkan device services used by the engine through query_extension(vulkan_escape).
// The engine owns its rendering policy and resources; this unit owns only device operations.
module;

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <mutex>
#include <span>
#include <type_traits>
#include <vulkan/vulkan.h>

module deren.vulkan.core;
import deren.promise.rhi;

namespace deren::vulkan {
    namespace {
        namespace rhi = deren::promise::rhi;

        template <class T>
        T* service_payload(void* const bytes, std::uint32_t const size) noexcept {
            if (bytes == nullptr || size != sizeof(T) || reinterpret_cast<std::uintptr_t>(bytes) % alignof(T) != 0) {
                return nullptr;
            }
            return static_cast<T*>(bytes);
        }

        template <class T>
        bool valid_blob(rhi::vulkan_blob const& blob) noexcept {
            return blob.data == nullptr ? blob.size == 0 : blob.size == sizeof(T) && reinterpret_cast<std::uintptr_t>(blob.data) % alignof(T) == 0;
        }

        template <class T>
        T native_handle(std::uint64_t const value) noexcept {
            if constexpr (std::is_pointer_v<T>) {
                return reinterpret_cast<T>(static_cast<std::uintptr_t>(value));
            } else {
                return static_cast<T>(value);
            }
        }

        template <class T>
        std::uint64_t handle_value(T const handle) noexcept {
            if constexpr (std::is_pointer_v<T>) {
                return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
            } else {
                return static_cast<std::uint64_t>(handle);
            }
        }

        rhi::error result_error(VkResult const result) noexcept {
            if (result == VK_SUCCESS) {
                return rhi::error::ok;
            }
            if (result == VK_TIMEOUT || result == VK_NOT_READY) {
                return rhi::error::not_ready;
            }
            return rhi::error::device_lost;
        }
    } // namespace

    deren::promise::rhi::error core::frame_escape::service(
        deren::promise::rhi::vulkan_service const operation, void* const payload,
        std::uint32_t const payload_size) noexcept {
        namespace rhi = deren::promise::rhi;
        using op = rhi::vulkan_service;
        if (this->owner == nullptr) {
            return rhi::error::not_ready;
        }
        core& gpu = *this->owner;

        switch (operation) {
        case op::snapshot: {
            auto* const out = service_payload<rhi::vulkan_snapshot>(payload, payload_size);
            if (out == nullptr || !valid_blob<VkPhysicalDeviceProperties>(out->device_properties) ||
                !valid_blob<VkPhysicalDeviceRayTracingPipelinePropertiesKHR>(out->ray_tracing_properties) ||
                !valid_blob<VkPhysicalDeviceAccelerationStructurePropertiesKHR>(out->acceleration_properties)) {
                return rhi::error::invalid_argument;
            }
            out->instance = reinterpret_cast<void*>(gpu.instance);
            out->physical_device = reinterpret_cast<void*>(gpu.physical_device);
            out->device = reinterpret_cast<void*>(gpu.logical_device);
            out->graphics_queue = reinterpret_cast<void*>(gpu.graphics_queue_handle);
            out->queue_family = gpu.graphics_queue_family_index;
            out->present_queue_family = gpu.present_queue_family_index;
            out->frame_count = static_cast<std::uint32_t>(core::MAX_FRAMES_IN_FLIGHT);
            out->current_frame = static_cast<std::uint32_t>(gpu.current_frame);
            out->image_count = static_cast<std::uint32_t>(gpu.swap_chain_images.size());
            out->sample_count = VK_SAMPLE_COUNT_1_BIT;
            out->format = static_cast<std::uint32_t>(gpu.swap_chain_image_format);
            out->depth_format = static_cast<std::uint32_t>(gpu.depth_attachment_format);
            out->width = gpu.swap_chain_extent.width;
            out->height = gpu.swap_chain_extent.height;
            VkExtent2D const extent = gpu.render_extent();
            out->render_width = extent.width;
            out->render_height = extent.height;
            out->render_scale = gpu.render_scale;
            out->mesh_shader = gpu.mesh_shader_available ? 1u : 0u;
            out->ray_query = gpu.ray_query_available ? 1u : 0u;
            out->ray_tracing = gpu.ray_tracing_pipeline_available ? 1u : 0u;
            out->opacity_micromap = gpu.opacity_micromap_available ? 1u : 0u;
            out->host_image_copy = gpu.host_image_copy_available ? 1u : 0u;
            out->gpu_timing = gpu.gpu_timing_available() ? 1u : 0u;
            out->heap_ready = gpu.descriptor_heaps.ready() ? 1u : 0u;
            out->heap_grid_offset = gpu.heap_grid_offset;
            out->heap_resource_size = gpu.descriptor_heaps.resource_size();
            heap_limits const& limits = gpu.descriptor_heap_limits;
            out->heap = {limits.max_resource_size, limits.max_sampler_size,
                         limits.resource_alignment, limits.sampler_alignment, limits.resource_reserved,
                         limits.sampler_reserved_with_embedded, limits.buffer_descriptor_size,
                         limits.image_descriptor_size, limits.sampler_descriptor_size,
                         limits.max_push_data, limits.max_embedded_samplers};
            if (out->device_properties.data != nullptr) {
                *static_cast<VkPhysicalDeviceProperties*>(out->device_properties.data) = gpu.device_properties;
            }
            if (out->ray_tracing_properties.data != nullptr) {
                auto& properties = *static_cast<VkPhysicalDeviceRayTracingPipelinePropertiesKHR*>(out->ray_tracing_properties.data);
                properties = gpu.ray_tracing_pipeline_properties;
                properties.pNext = nullptr;
            }
            if (out->acceleration_properties.data != nullptr) {
                auto& properties = *static_cast<VkPhysicalDeviceAccelerationStructurePropertiesKHR*>(out->acceleration_properties.data);
                properties = gpu.acceleration_structure_properties;
                properties.pNext = nullptr;
            }
            return rhi::error::ok;
        }
        case op::wait_slot:
        case op::acquire:
        case op::submit:
        case op::present:
        case op::advance:
        case op::recreate:
        case op::frame_command:
        case op::swapchain_image: {
            auto* const frame = service_payload<rhi::vulkan_frame>(payload, payload_size);
            if (frame == nullptr) {
                return rhi::error::invalid_argument;
            }
            frame->result = VK_SUCCESS;
            if (operation == op::wait_slot || operation == op::frame_command) {
                if (frame->slot >= static_cast<std::uint32_t>(core::MAX_FRAMES_IN_FLIGHT)) {
                    return rhi::error::invalid_argument;
                }
            }
            if (operation == op::swapchain_image) {
                if (frame->image_index >= gpu.swap_chain_images.size() ||
                    frame->image_index >= gpu.swap_chain_image_views.size()) {
                    return rhi::error::invalid_argument;
                }
                frame->image = handle_value(gpu.swap_chain_images[frame->image_index]);
                frame->image_view = handle_value(gpu.swap_chain_image_views[frame->image_index]);
            } else if (operation == op::wait_slot) {
                std::uint64_t const value = gpu.frame_done_values[frame->slot];
                if (value != 0) {
                    VkSemaphoreWaitInfo const wait = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, nullptr, 0, 1,
                                                      &gpu.frame_done_semaphores[frame->slot], &value};
                    frame->result = vkWaitSemaphores(gpu.logical_device, &wait, UINT64_MAX);
                }
            } else if (operation == op::frame_command) {
                if (frame->slot >= gpu.frame_command_buffers.size()) {
                    return rhi::error::not_ready;
                }
                frame->command_buffer = reinterpret_cast<void*>(*gpu.frame_command_buffers[frame->slot]);
            } else if (operation == op::acquire) {
                frame->slot = static_cast<std::uint32_t>(gpu.current_frame);
                frame->result = gpu.acquire_next_image(frame->image_index);
            } else if (operation == op::submit) {
                if (frame->command_buffer == nullptr || !gpu.frame_in_flight || frame->image_index != gpu.acquired_image_index) {
                    return rhi::error::invalid_argument;
                }
                std::lock_guard const guard(gpu.vma.queue_sync());
                frame->result = gpu.submit(reinterpret_cast<VkCommandBuffer>(frame->command_buffer), frame->image_index);
            } else if (operation == op::present) {
                if (frame->image_index >= gpu.swap_chain_images.size()) {
                    return rhi::error::invalid_argument;
                }
                std::lock_guard const guard(gpu.vma.queue_sync());
                frame->result = gpu.present(frame->image_index);
            } else if (operation == op::advance) {
                gpu.to_next_frame();
                frame->slot = static_cast<std::uint32_t>(gpu.current_frame);
            } else {
                // Recreation starts by waiting idle. Hold the same queue lock as upload/readback/frame submission.
                std::lock_guard const guard(gpu.vma.queue_sync());
                frame->recreated = gpu.recreate_swap_chain() ? 1u : 0u;
            }
            return rhi::error::ok;
        }
        case op::create_pool:
        case op::allocate_command:
        case op::free_command:
        case op::reset_pool: {
            auto* const command = service_payload<rhi::vulkan_command>(payload, payload_size);
            if (command == nullptr) {
                return rhi::error::invalid_argument;
            }
            command->result = VK_SUCCESS;
            VkCommandPool pool = native_handle<VkCommandPool>(command->pool);
            if (operation == op::create_pool) {
                VkCommandPoolCreateInfo const info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, gpu.graphics_queue_family_index};
                command->result = vkCreateCommandPool(gpu.logical_device, &info, nullptr, &pool);
                command->pool = command->result == VK_SUCCESS ? handle_value(pool) : 0;
                if (command->result == VK_SUCCESS) {
                    // Pools share the backend root's lifetime, just as the original recording pools did.
                    // Engine command owners free their buffers before this registered cleanup runs.
                    gpu.register_cleanup([device = gpu.logical_device, pool] {
                        vkDestroyCommandPool(device, pool, nullptr);
                    });
                }
            } else {
                if (pool == VK_NULL_HANDLE) {
                    pool = gpu.command_pool;
                    command->pool = handle_value(pool);
                }
                if (pool == VK_NULL_HANDLE) {
                    return rhi::error::not_ready;
                }
                if (operation == op::allocate_command) {
                    VkCommandBufferAllocateInfo const info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr,
                                                              pool, command->secondary != 0 ? VK_COMMAND_BUFFER_LEVEL_SECONDARY : VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
                    VkCommandBuffer buffer = VK_NULL_HANDLE;
                    command->result = vkAllocateCommandBuffers(gpu.logical_device, &info, &buffer);
                    command->command_buffer = command->result == VK_SUCCESS ? reinterpret_cast<void*>(buffer) : nullptr;
                } else if (operation == op::free_command) {
                    if (command->command_buffer == nullptr) {
                        return rhi::error::invalid_argument;
                    }
                    VkCommandBuffer const buffer = reinterpret_cast<VkCommandBuffer>(command->command_buffer);
                    vkFreeCommandBuffers(gpu.logical_device, pool, 1, &buffer);
                    command->command_buffer = nullptr;
                } else {
                    command->result = vkResetCommandPool(gpu.logical_device, pool, 0);
                }
            }
            return result_error(static_cast<VkResult>(command->result));
        }
        case op::begin_timing:
        case op::mark_timing:
        case op::read_timing: {
            auto* const timing = service_payload<rhi::vulkan_timing>(payload, payload_size);
            if (timing == nullptr || timing->slot >= static_cast<std::uint32_t>(core::MAX_FRAMES_IN_FLIGHT)) {
                return rhi::error::invalid_argument;
            }
            static_assert(rhi::vulkan_timing_capacity == gpu_timing_mark_capacity);
            if (operation == op::read_timing) {
                auto const result = gpu.read_gpu_timings(timing->slot);
                timing->mark_count = result.mark_count;
                timing->milliseconds = result.milliseconds;
            } else {
                if (timing->command_buffer == nullptr) {
                    return rhi::error::invalid_argument;
                }
                VkCommandBuffer const buffer = reinterpret_cast<VkCommandBuffer>(timing->command_buffer);
                if (operation == op::begin_timing) {
                    gpu.begin_gpu_timing(buffer, timing->slot);
                } else {
                    gpu.mark_gpu_timing(buffer, timing->slot, static_cast<VkPipelineStageFlagBits>(timing->stage));
                }
            }
            timing->written_marks = gpu.gpu_timing_marks[timing->slot];
            return rhi::error::ok;
        }
        case op::heap_write_buffer: {
            auto* const write = service_payload<rhi::vulkan_heap_buffer>(payload, payload_size);
            if (write == nullptr) {
                return rhi::error::invalid_argument;
            }
            write->accepted = gpu.descriptor_heaps.write_buffer(write->offset, write->address, write->size,
                                                                static_cast<VkDescriptorType>(write->type))
                                  ? 1u
                                  : 0u;
            return rhi::error::ok;
        }
        case op::heap_write_image: {
            auto* const write = service_payload<rhi::vulkan_heap_image>(payload, payload_size);
            if (write == nullptr || write->view.data == nullptr || !valid_blob<VkImageViewCreateInfo>(write->view)) {
                return rhi::error::invalid_argument;
            }
            write->accepted = gpu.descriptor_heaps.write_image(write->offset,
                                                               *static_cast<VkImageViewCreateInfo const*>(write->view.data), static_cast<VkImageLayout>(write->layout),
                                                               static_cast<VkDescriptorType>(write->type))
                                  ? 1u
                                  : 0u;
            return rhi::error::ok;
        }
        case op::heap_write_samplers: {
            auto* const write = service_payload<rhi::vulkan_heap_samplers>(payload, payload_size);
            if (write == nullptr || write->count == 0 || write->infos.data == nullptr ||
                static_cast<std::uint64_t>(write->infos.size) != static_cast<std::uint64_t>(write->count) * sizeof(VkSamplerCreateInfo) ||
                reinterpret_cast<std::uintptr_t>(write->infos.data) % alignof(VkSamplerCreateInfo) != 0) {
                return rhi::error::invalid_argument;
            }
            write->accepted = gpu.descriptor_heaps.write_samplers(write->offset,
                                                                  std::span<VkSamplerCreateInfo const>(static_cast<VkSamplerCreateInfo const*>(write->infos.data), write->count))
                                  ? 1u
                                  : 0u;
            return rhi::error::ok;
        }
        case op::heap_bind:
        case op::heap_push:
        case op::heap_bind_infos: {
            auto* const heap = service_payload<rhi::vulkan_heap_commands>(payload, payload_size);
            if (heap == nullptr) {
                return rhi::error::invalid_argument;
            }
            heap->accepted = 0;
            if (operation == op::heap_bind_infos) {
                if (heap->resource_bind.data == nullptr || heap->sampler_bind.data == nullptr ||
                    !valid_blob<VkBindHeapInfoEXT>(heap->resource_bind) || !valid_blob<VkBindHeapInfoEXT>(heap->sampler_bind)) {
                    return rhi::error::invalid_argument;
                }
                gpu.descriptor_heaps.bind_infos(*static_cast<VkBindHeapInfoEXT*>(heap->resource_bind.data),
                                                *static_cast<VkBindHeapInfoEXT*>(heap->sampler_bind.data));
                heap->accepted = gpu.descriptor_heaps.ready() ? 1u : 0u;
            } else {
                if (heap->command_buffer == nullptr) {
                    return rhi::error::invalid_argument;
                }
                VkCommandBuffer const buffer = reinterpret_cast<VkCommandBuffer>(heap->command_buffer);
                if (operation == op::heap_bind) {
                    gpu.descriptor_heaps.record_bind(buffer);
                    heap->accepted = gpu.descriptor_heaps.ready() ? 1u : 0u;
                } else {
                    if (heap->data == nullptr && heap->data_size != 0) {
                        return rhi::error::invalid_argument;
                    }
                    heap->accepted = gpu.descriptor_heaps.push_data(buffer, heap->offset,
                                                                    std::span<std::byte const>(static_cast<std::byte const*>(heap->data), heap->data_size))
                                         ? 1u
                                         : 0u;
                }
            }
            return rhi::error::ok;
        }
        case op::submit_and_wait: {
            auto* const submit = service_payload<rhi::vulkan_submit_wait>(payload, payload_size);
            if (submit == nullptr || submit->command_buffer == nullptr) {
                return rhi::error::invalid_argument;
            }
            VkFence fence = VK_NULL_HANDLE;
            VkFenceCreateInfo const fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0};
            VkResult result = vkCreateFence(gpu.logical_device, &fence_info, nullptr, &fence);
            if (result == VK_SUCCESS) {
                VkCommandBuffer const buffer = reinterpret_cast<VkCommandBuffer>(submit->command_buffer);
                VkSubmitInfo const info = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &buffer, 0, nullptr};
                {
                    // Serialize exactly the queue call with uploads and frame submission. Do not hold a queue
                    // lock across a fence wait: unrelated work remains free to submit while this copy completes.
                    std::lock_guard const guard(gpu.vma.queue_sync());
                    result = vkQueueSubmit(gpu.graphics_queue_handle, 1, &info, fence);
                }
                if (result == VK_SUCCESS) {
                    result = vkWaitForFences(gpu.logical_device, 1, &fence, VK_TRUE, submit->timeout_nanoseconds);
                    if (result == VK_TIMEOUT) {
                        // A pending fence cannot be destroyed. Finish this accepted submission before releasing
                        // its synchronization object, while retaining the timeout result for the caller.
                        VkResult const completed = vkWaitForFences(gpu.logical_device, 1, &fence, VK_TRUE, UINT64_MAX);
                        if (completed != VK_SUCCESS) {
                            result = completed;
                        }
                    }
                }
                vkDestroyFence(gpu.logical_device, fence, nullptr);
            }
            submit->result = result;
            return result_error(result);
        }
        case op::wait_idle: {
            if (payload != nullptr || payload_size != 0) {
                return rhi::error::invalid_argument;
            }
            std::lock_guard const guard(gpu.vma.queue_sync());
            return result_error(vkDeviceWaitIdle(gpu.logical_device));
        }
        }
        return rhi::error::unsupported;
    }
} // namespace deren::vulkan
