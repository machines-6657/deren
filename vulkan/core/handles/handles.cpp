module;

#include <vulkan/vulkan.h>

module deren.vulkan.core.handles;

import deren.utility;
import deren.vulkan.constant_init;

// vk_command_buffer
namespace deren::vulkan {
    vk_command_buffer::vk_command_buffer(VkCommandBuffer const command_buffer, VkDevice const device, VkCommandPool const pool) noexcept { // NOLINT(*-misplaced-const)
        this->command_buffer = command_buffer;
        this->device = device;
        this->command_pool = pool;
    }

    VkCommandBuffer const& vk_command_buffer::get() const noexcept {
        return this->command_buffer;
    }

    VkCommandBuffer const& vk_command_buffer::operator*() const noexcept {
        return this->get();
    }

    void vk_command_buffer::release() noexcept {
        if (this->command_buffer != VK_NULL_HANDLE && this->device != VK_NULL_HANDLE && this->command_pool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(this->device, this->command_pool, 1, &this->command_buffer);
        }

        this->device = VK_NULL_HANDLE;
        this->command_pool = VK_NULL_HANDLE;
        this->command_buffer = VK_NULL_HANDLE;
    }

    vk_command_buffer::~vk_command_buffer() noexcept {
        this->release();
    }

    vk_command_buffer::vk_command_buffer(vk_command_buffer&& other) noexcept {
        this->command_buffer = other.command_buffer;
        this->device = other.device;
        this->command_pool = other.command_pool;
        other.device = VK_NULL_HANDLE;
        other.command_pool = VK_NULL_HANDLE;
        other.command_buffer = VK_NULL_HANDLE;
    }

    vk_command_buffer& vk_command_buffer::operator=(vk_command_buffer&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        this->release();
        this->command_buffer = other.command_buffer;
        this->device = other.device;
        this->command_pool = other.command_pool;
        other.device = VK_NULL_HANDLE;
        other.command_pool = VK_NULL_HANDLE;
        other.command_buffer = VK_NULL_HANDLE;
        return *this;
    }

    vk_command_buffer make_command_buffer(VkDevice const device, VkCommandPool const command_pool) noexcept {
        VkCommandBuffer buffer = VK_NULL_HANDLE;

        VkCommandBufferAllocateInfo allocate_info = make_command_buffer_allocate_info(command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);

        if (vkAllocateCommandBuffers(device, &allocate_info, &buffer) != VK_SUCCESS) {
            deren::utility::panic("failed to allocate command buffer");
        }

        return vk_command_buffer(buffer, device, command_pool);
    }

    vk_command_buffer make_secondary_command_buffer(VkDevice const device, VkCommandPool const command_pool) noexcept {
        VkCommandBuffer buffer = VK_NULL_HANDLE;

        VkCommandBufferAllocateInfo allocate_info = make_command_buffer_allocate_info(command_pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY);

        if (vkAllocateCommandBuffers(device, &allocate_info, &buffer) != VK_SUCCESS) {
            deren::utility::panic("failed to allocate secondary command buffer");
        }

        return vk_command_buffer(buffer, device, command_pool);
    }
} // namespace deren::vulkan

// vk_shader_module
namespace deren::vulkan {
    vk_shader_module::vk_shader_module(VkShaderModule shader_module, VkDevice const device) noexcept {
        this->shader_module = shader_module;
        this->device = device;
    }

    vk_shader_module::~vk_shader_module() noexcept {
        this->release();
    }

    VkShaderModule const& vk_shader_module::get() const noexcept {
        return this->shader_module;
    }

    VkShaderModule const& vk_shader_module::operator*() const noexcept {
        return this->shader_module;
    }

    vk_shader_module::vk_shader_module(vk_shader_module&& other) noexcept {
        this->shader_module = other.shader_module;
        this->device = other.device;
        other.shader_module = VK_NULL_HANDLE;
        other.device = VK_NULL_HANDLE;
    }

    vk_shader_module& vk_shader_module::operator=(vk_shader_module&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->release();
        this->shader_module = other.shader_module;
        this->device = other.device;
        other.shader_module = VK_NULL_HANDLE;
        other.device = VK_NULL_HANDLE;
        return *this;
    }

    void vk_shader_module::release() {
        if (this->shader_module != VK_NULL_HANDLE && this->device != VK_NULL_HANDLE) {
            vkDestroyShaderModule(this->device, this->shader_module, nullptr);
        }
        this->shader_module = VK_NULL_HANDLE;
        this->device = VK_NULL_HANDLE;
    }

    std::optional<vk_shader_module> make_shader_module(std::span<uint8_t const> const shader, VkDevice const device) noexcept {
        VkShaderModule shader_module = {};

        VkShaderModuleCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .codeSize = shader.size_bytes(),
            .pCode = reinterpret_cast<uint32_t const*>(shader.data()),
        };

        if (vkCreateShaderModule(device, &create_info, nullptr, &shader_module) != VK_SUCCESS) {
            return std::nullopt;
        }

        return vk_shader_module(shader_module, device);
    }
} // namespace deren::vulkan

// vk_pipeline
namespace deren::vulkan {
    vk_pipeline::vk_pipeline(VkPipeline const pipeline, VkDevice const device) noexcept { // NOLINT(*-misplaced-const)
        this->pipeline = pipeline;
        this->device = device;
    }

    vk_pipeline::vk_pipeline(vk_pipeline&& other) noexcept {
        this->device = other.device;
        this->pipeline = other.pipeline;
        this->viewport = other.viewport;
        this->scissor = other.scissor;
        other.device = VK_NULL_HANDLE;
        other.pipeline = VK_NULL_HANDLE;
    }

    vk_pipeline& vk_pipeline::operator=(vk_pipeline&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->release();
        this->device = other.device;
        this->pipeline = other.pipeline;
        this->viewport = other.viewport;
        this->scissor = other.scissor;
        other.device = VK_NULL_HANDLE;
        other.pipeline = VK_NULL_HANDLE;
        return *this;
    }

    vk_pipeline::~vk_pipeline() {
        this->release();
    }

    VkPipeline vk_pipeline::get_pipeline() const noexcept {
        return this->pipeline;
    }

    void vk_pipeline::begin_pipeline(VkCommandBuffer const command_buffer) const {
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipeline);
        vkCmdSetViewport(command_buffer, 0, 1, &this->viewport);
        vkCmdSetScissor(command_buffer, 0, 1, &this->scissor);
    }

    void vk_pipeline::release() noexcept {
        if (this->device != VK_NULL_HANDLE) {
            if (this->pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(this->device, this->pipeline, nullptr);
            }
        }
        this->device = VK_NULL_HANDLE;
        this->pipeline = VK_NULL_HANDLE;
    }

    // vk_image_view
    vk_image_view::vk_image_view(VkImageView const image_view, VkDevice const device) noexcept {
        this->image_view = image_view;
        this->device = device;
    }

    vk_image_view::~vk_image_view() noexcept {
        this->release();
    }

    VkImageView const& vk_image_view::get() const noexcept {
        return this->image_view;
    }

    VkImageView const& vk_image_view::operator*() const noexcept {
        return this->image_view;
    }

    void vk_image_view::release() noexcept {
        if (this->device != VK_NULL_HANDLE && this->image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(this->device, this->image_view, nullptr);
        }
        this->image_view = VK_NULL_HANDLE;
        this->device = VK_NULL_HANDLE;
    }

    vk_image_view::vk_image_view(vk_image_view&& other) noexcept {
        this->image_view = other.image_view;
        this->device = other.device;
        other.image_view = VK_NULL_HANDLE;
        other.device = VK_NULL_HANDLE;
    }

    vk_image_view& vk_image_view::operator=(vk_image_view&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->release();
        this->image_view = other.image_view;
        this->device = other.device;
        other.image_view = VK_NULL_HANDLE;
        other.device = VK_NULL_HANDLE;
        return *this;
    }

    // vk_sampler
    vk_sampler::vk_sampler(VkSampler const sampler, VkDevice const device) noexcept {
        this->sampler = sampler;
        this->device = device;
    }

    vk_sampler::~vk_sampler() noexcept {
        this->release();
    }

    VkSampler const& vk_sampler::get() const noexcept {
        return this->sampler;
    }

    VkSampler const& vk_sampler::operator*() const noexcept {
        return this->sampler;
    }

    void vk_sampler::release() noexcept {
        if (this->device != VK_NULL_HANDLE && this->sampler != VK_NULL_HANDLE) {
            vkDestroySampler(this->device, this->sampler, nullptr);
        }
        this->sampler = VK_NULL_HANDLE;
        this->device = VK_NULL_HANDLE;
    }

    vk_sampler::vk_sampler(vk_sampler&& other) noexcept {
        this->sampler = other.sampler;
        this->device = other.device;
        other.sampler = VK_NULL_HANDLE;
        other.device = VK_NULL_HANDLE;
    }

    vk_sampler& vk_sampler::operator=(vk_sampler&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->release();
        this->sampler = other.sampler;
        this->device = other.device;
        other.sampler = VK_NULL_HANDLE;
        other.device = VK_NULL_HANDLE;
        return *this;
    }
} // namespace deren::vulkan
