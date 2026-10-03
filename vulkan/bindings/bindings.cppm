// module version: 0.10.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/bindings/bindings.cppm
 * @defgroup vulkan_bindings Declaration -> Vulkan Type Mapping
 * @brief The half of `deren.vulkan.render_resource` that has to name a Vulkan enum: a declared binding kind, stage set
 *        or image layout, as the `VkDescriptorType` / `VkShaderStageFlags` / `VkImageLayout` it means.
 *
 * WHAT IS LEFT HERE, after the descriptor-set plumbing was deleted: the TYPE MAPPING alone. The generator that
 * turned a declaration into a `VkDescriptorSetLayout` (and its writes) is gone with the layouts, pools, families
 * and per-frame sets it served - every stage is heap-native now and reaches its descriptors through the frame's
 * descriptor heap (see vulkan/core/descriptor_heap), so no pipeline in this renderer has a layout and no set is
 * ever bound. The mapping itself is kept because it is pure CPU vocabulary with a checked contract (its own
 * ctest), and it is the one place where a declaration's enum and a Vulkan enum are stated to be the same fact.
 */

module;

#include <cstdint>
#include <vulkan/vulkan.h>

export module deren.vulkan.bindings;

import deren.vulkan.render_resource;

namespace deren::vulkan::bindings {

    /// @brief the `VkDescriptorType` a declared binding kind means
    /// @ingroup vulkan_bindings
    export [[nodiscard]] constexpr VkDescriptorType descriptor_type_of(render_resource::binding_kind const kind) noexcept {
        switch (kind) {
        case render_resource::binding_kind::sampled_image:
            return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        case render_resource::binding_kind::storage_image:
            return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case render_resource::binding_kind::sampler:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case render_resource::binding_kind::uniform_buffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case render_resource::binding_kind::storage_buffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case render_resource::binding_kind::input_attachment:
            return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
        case render_resource::binding_kind::acceleration_structure:
            return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        }
        return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }

    /// @brief the shader stages a declared stage set means (a binding may serve two, hence the union)
    /// @ingroup vulkan_bindings
    export [[nodiscard]] constexpr VkShaderStageFlags stage_flags_of(render_resource::stage_flag const stages) noexcept {
        VkShaderStageFlags flags = 0u;
        if (render_resource::has_stage(stages, render_resource::stage_flag::vertex)) {
            flags |= VK_SHADER_STAGE_VERTEX_BIT;
        }
        if (render_resource::has_stage(stages, render_resource::stage_flag::fragment)) {
            flags |= VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        if (render_resource::has_stage(stages, render_resource::stage_flag::compute)) {
            flags |= VK_SHADER_STAGE_COMPUTE_BIT;
        }
        return flags;
    }

} // namespace deren::vulkan::bindings
