// The two filtered views over a core: the application's (what `runtime::operator->` returns) and a pass's init
// view. Both forward to the core they hold a share of; neither manages a frame, and the pass filter deliberately
// offers no per-generation handle (see the header for the contract).

module;

#include <GLFW/glfw3.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vulkan/vulkan.h>

module deren.vulkan.core.filters;

namespace deren::vulkan {

    // =============================================================================================
    // the application's view
    // =============================================================================================

    user_filter::user_filter(std::shared_ptr<core> owner) noexcept
        : owner_share(std::move(owner))
        , vk_core(this->owner_share.get()) {
    }

    VkDevice user_filter::get_device() const noexcept {
        return this->vk_core->logical_device;
    }

    GLFWwindow* user_filter::get_window() const noexcept {
        return this->vk_core->window;
    }

    void user_filter::wait_idle() const noexcept {
        this->vk_core->wait_idle();
    }

    void user_filter::set_window_title(std::string_view const title) const noexcept {
        this->vk_core->set_window_title(title);
    }

    VkExtent2D user_filter::get_swap_chain_extent() const noexcept {
        return this->vk_core->swap_chain_extent;
    }

    VkFormat user_filter::get_swap_chain_image_format() const noexcept {
        return this->vk_core->swap_chain_image_format;
    }

    uint32_t user_filter::get_current_frame() const noexcept {
        return static_cast<uint32_t>(this->vk_core->current_frame);
    }

    vk_command_buffer user_filter::make_command_buffer() const {
        return this->vk_core->make_command_buffer();
    }

    std::optional<vk_shader_module> user_filter::make_shader_module(std::span<uint8_t> const shader) const noexcept {
        return this->vk_core->make_shader_module(shader);
    }

    vk_image_view user_filter::make_image_view(VkImage const image, VkFormat const format, VkImageViewType const type) const {
        return this->vk_core->make_image_view(image, format, type);
    }

    vk_sampler user_filter::make_sampler(VkSamplerAddressMode const address_mode, float const max_lod) const {
        return this->vk_core->make_sampler(address_mode, max_lod);
    }

    // Deliberately non-const: returning a mutable vma_allocator from a const method would break the const
    // contract (a const runtime must not allocate). Some toolchains still suggest adding const here, hence the
    // suppression.
    vma_allocator& user_filter::get_vma() noexcept { // NOLINT
        return this->vk_core->vma;
    }

    bool user_filter::recreate_swap_chain() const {
        return this->vk_core->recreate_swap_chain();
    }

    // =============================================================================================
    // the pass's view: a device, the surface's format, the owner's pool, the allocator, and the
    // resources the owner published - nothing that manages a frame, and no per-generation handle
    // =============================================================================================

    pass_filter::pass_filter(std::shared_ptr<core> owner) noexcept
        : owner_share(std::move(owner))
        , vk_core(this->owner_share.get()) {
    }

    VkDevice pass_filter::device() const noexcept {
        return this->vk_core->logical_device;
    }

    VkFormat pass_filter::swap_chain_image_format() const noexcept {
        return this->vk_core->swap_chain_image_format;
    }

    VkExtent2D pass_filter::swap_chain_extent() const noexcept {
        return this->vk_core->swap_chain_extent;
    }

    vma_allocator& pass_filter::vma() noexcept { // NOLINT: the same const contract as user_filter::get_vma
        return this->vk_core->vma;
    }

    void pass_filter::register_resource(render_resource::resource_id const id, uint32_t const element, resource_handles const handles) noexcept {
        // The key packs the declaration's identity: which resource, and which element of its family. A SECOND
        // registration of the same key REPLACES the first, because the owner re-publishing a resource after a
        // rebuild is the same statement as publishing it.
        uint32_t const key = (static_cast<uint32_t>(id) << 8u) | (element & 0xFFu);
        for (auto& [existing, existing_handles] : this->registered) {
            if (existing == key) {
                existing_handles = handles;
                return;
            }
        }
        this->registered.emplace_back(key, handles);
    }

    resource_handles pass_filter::resource(render_resource::resource_id const id, uint32_t const element) const noexcept {
        uint32_t const key = (static_cast<uint32_t>(id) << 8u) | (element & 0xFFu);
        for (auto const& [existing, handles] : this->registered) {
            if (existing == key) {
                return handles;
            }
        }
        return {}; // the owner has none: a pass's "cannot build" branch, which every pass already has
    }

} // namespace deren::vulkan
