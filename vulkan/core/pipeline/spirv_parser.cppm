module;

#include "../../../third_party/spirv-reflect/spirv_reflect.h"

#include <algorithm>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

export module deren.vulkan.core.pipeline:spirv_parser;

export import deren.vstd;
import deren.utility;
/**
 * @file spirv_parser.cppm
 * @defgroup vulkan_spirv_parser SPIR-V Reflection Parser
 * @brief parse shader stage interfaces, descriptor set layouts and push constant layouts from SPIR-V binary
 * @note
 *      - built on SPIRV-Reflect
 *      - all functions return std::expected, errors are reported as std::string_view
 */
namespace deren::vulkan::pipeline {
    /**
     * @ingroup vulkan_spirv_parser
     * @brief byte size of the given VkFormat
     * @param format the format to query
     * @return byte size of a single element of the format
     */
    export uint32_t format_size(VkFormat format);

    // Holds the complete info for a single descriptor set layout
    /**
     * @ingroup vulkan_spirv_parser
     * @brief full information of a single descriptor set layout
     */
    export struct descriptor_set_layout_data {
        uint32_t set_number;                                // descriptor set index
        VkDescriptorSetLayoutCreateInfo create_info;        // info for creating the VkDescriptorSetLayout
        std::vector<VkDescriptorSetLayoutBinding> bindings; // concrete binding info
    };

    /**
     * @ingroup vulkan_spirv_parser
     * @brief a single input/output interface variable of a shader stage
     */
    export struct interface_variable_info {
        uint32_t location;
        uint32_t component; // component index, default 0
        VkFormat format;
        std::string name; // variable name, for debugging and error messages
        bool is_builtin;  // whether this is a builtin
    };

    /**
     * @ingroup vulkan_spirv_parser
     * @brief shader stage interface: stage plus its input/output variables
     */
    export struct shader_stage_interface {
        VkShaderStageFlagBits stage;
        std::vector<interface_variable_info> inputs;
        std::vector<interface_variable_info> outputs;
    };

    /**
     * @ingroup vulkan_spirv_parser
     * @brief parse the input/output interface of a shader stage from SPIR-V binary
     * @param spirv_code the SPIR-V binary code
     * @param stage the shader stage to parse
     * @return shader_stage_interface on success, error message on failure
     */
    export std::expected<shader_stage_interface, std::string_view> parse_shader_stage_interface(
        std::span<uint8_t const> spirv_code,
        VkShaderStageFlagBits stage);

    bool validate_interface_match(
        shader_stage_interface const& producer,
        shader_stage_interface const& consumer);

    /**
     * @ingroup vulkan_spirv_parser
     * @brief parse all descriptor set layouts from SPIR-V binary
     * @param spirv_code the SPIR-V binary code
     * @param shader_stage the shader stage to parse
     * @return vector of descriptor set layout data on success, error message on failure
     */
    export std::expected<std::vector<descriptor_set_layout_data>, std::string_view>
    parse_descriptor_set_layouts(
        std::span<uint8_t const> spirv_code,
        VkShaderStageFlagBits shader_stage);

    /**
     * @ingroup vulkan_spirv_parser
     * @brief a single push constant range
     */
    export struct push_constant_info {
        uint32_t offset;
        uint32_t size;
        VkShaderStageFlags stage_flags;
        std::string name;
    };

    // Stores the shader's whole Push Constant layout
    /**
     * @ingroup vulkan_spirv_parser
     * @brief push constant layout of the whole shader
     */
    export struct push_constant_layout {
        std::vector<push_constant_info> constants;
        uint32_t total_size; // total size of the whole Push Constant block
    };

    /**
     * @ingroup vulkan_spirv_parser
     * @brief parse the push constant layout from SPIR-V binary
     * @param spirv_code the SPIR-V binary code
     * @return push_constant_layout on success, error message on failure
     */
    export std::expected<deren::vulkan::pipeline::push_constant_layout, std::string_view> parse_push_constant_layout(
        std::span<uint8_t const> spirv_code);
} // namespace deren::vulkan::pipeline

namespace {
    deren::vulkan::pipeline::interface_variable_info reflect_var_to_info(SpvReflectInterfaceVariable const* p_var) {
        deren::vulkan::pipeline::interface_variable_info info = {};
        info.location = p_var->location;
        info.component = p_var->component;
        info.format = static_cast<VkFormat>(p_var->format);
        info.name = p_var->name ? p_var->name : "unnamed";
        info.is_builtin = (p_var->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) != 0;
        return info;
    }

    void sort_interface_variables(std::vector<deren::vulkan::pipeline::interface_variable_info>& vars) {
        std::ranges::sort(vars,
                          [](deren::vulkan::pipeline::interface_variable_info const& a, deren::vulkan::pipeline::interface_variable_info const& b) {
                              if (a.location != b.location) {
                                  return a.location < b.location;
                              }
                              return a.component < b.component;
                          });
    }
} // namespace

uint32_t deren::vulkan::pipeline::format_size(VkFormat const format) {
    uint32_t result = 0;
    switch (format) {
    case VK_FORMAT_UNDEFINED:
        result = 0;
        break;
    case VK_FORMAT_R4G4_UNORM_PACK8:
        result = 1;
        break;
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
    case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        result = 2;
        break;
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_USCALED:
    case VK_FORMAT_R8_SSCALED:
    case VK_FORMAT_R8_UINT:
    case VK_FORMAT_R8_SINT:
    case VK_FORMAT_R8_SRGB:
        result = 1;
        break;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8_USCALED:
    case VK_FORMAT_R8G8_SSCALED:
    case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8_SRGB:
        result = 2;
        break;
    case VK_FORMAT_R8G8B8_UNORM:
    case VK_FORMAT_R8G8B8_SNORM:
    case VK_FORMAT_R8G8B8_USCALED:
    case VK_FORMAT_R8G8B8_SSCALED:
    case VK_FORMAT_R8G8B8_UINT:
    case VK_FORMAT_R8G8B8_SINT:
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_B8G8R8_UNORM:
    case VK_FORMAT_B8G8R8_SNORM:
    case VK_FORMAT_B8G8R8_USCALED:
    case VK_FORMAT_B8G8R8_SSCALED:
    case VK_FORMAT_B8G8R8_UINT:
    case VK_FORMAT_B8G8R8_SINT:
    case VK_FORMAT_B8G8R8_SRGB:
        result = 3;
        break;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SNORM:
    case VK_FORMAT_R8G8B8A8_USCALED:
    case VK_FORMAT_R8G8B8A8_SSCALED:
    case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SNORM:
    case VK_FORMAT_B8G8R8A8_USCALED:
    case VK_FORMAT_B8G8R8A8_SSCALED:
    case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_USCALED_PACK32:
    case VK_FORMAT_A8B8G8R8_SSCALED_PACK32:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
    case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
    case VK_FORMAT_A2R10G10B10_UINT_PACK32:
    case VK_FORMAT_A2R10G10B10_SINT_PACK32:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
    case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        result = 4;
        break;
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_SNORM:
    case VK_FORMAT_R16_USCALED:
    case VK_FORMAT_R16_SSCALED:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT:
        result = 2;
        break;
    case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM:
    case VK_FORMAT_R16G16_USCALED:
    case VK_FORMAT_R16G16_SSCALED:
    case VK_FORMAT_R16G16_UINT:
    case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16_SFLOAT:
        result = 4;
        break;
    case VK_FORMAT_R16G16B16_UNORM:
    case VK_FORMAT_R16G16B16_SNORM:
    case VK_FORMAT_R16G16B16_USCALED:
    case VK_FORMAT_R16G16B16_SSCALED:
    case VK_FORMAT_R16G16B16_UINT:
    case VK_FORMAT_R16G16B16_SINT:
    case VK_FORMAT_R16G16B16_SFLOAT:
        result = 6;
        break;
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_USCALED:
    case VK_FORMAT_R16G16B16A16_SSCALED:
    case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        result = 8;
        break;
    case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32_SFLOAT:
        result = 4;
        break;
    case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32_SFLOAT:
        result = 8;
        break;
    case VK_FORMAT_R32G32B32_UINT:
    case VK_FORMAT_R32G32B32_SINT:
    case VK_FORMAT_R32G32B32_SFLOAT:
        result = 12;
        break;
    case VK_FORMAT_R32G32B32A32_UINT:
    case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        result = 16;
        break;
    case VK_FORMAT_R64_UINT:
        result = 8;
        break;
    case VK_FORMAT_R64_SINT:
    case VK_FORMAT_R64_SFLOAT:
    case VK_FORMAT_R64G64_UINT:
    case VK_FORMAT_R64G64_SINT:
    case VK_FORMAT_R64G64_SFLOAT:
        result = 16;
        break;
    case VK_FORMAT_R64G64B64_UINT:
    case VK_FORMAT_R64G64B64_SINT:
    case VK_FORMAT_R64G64B64_SFLOAT:
        result = 24;
        break;
    case VK_FORMAT_R64G64B64A64_UINT:
    case VK_FORMAT_R64G64B64A64_SINT:
    case VK_FORMAT_R64G64B64A64_SFLOAT:
        result = 32;
        break;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        result = 4;
        break;

    default:
        break;
    }
    return result;
}

std::expected<deren::vulkan::pipeline::shader_stage_interface, std::string_view>
deren::vulkan::pipeline::parse_shader_stage_interface(
    std::span<uint8_t const> const spirv_code,
    VkShaderStageFlagBits const stage) {
    using fail = std::unexpected<std::string_view>;

    shader_stage_interface out_interface;
    out_interface.stage = stage;
    out_interface.inputs.clear();
    out_interface.outputs.clear();

    SpvReflectShaderModule module = {};
    SpvReflectResult result = spvReflectCreateShaderModule(
        spirv_code.size(),
        spirv_code.data(),
        &module);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        return fail("spv reflect failed");
    }

    // 2. Parse input variables
    uint32_t input_count = 0;
    result = spvReflectEnumerateInputVariables(&module, &input_count, nullptr);
    if (result == SPV_REFLECT_RESULT_SUCCESS && input_count > 0) {
        std::vector<SpvReflectInterfaceVariable*> input_vars(input_count);
        result = spvReflectEnumerateInputVariables(&module, &input_count, input_vars.data());
        if (result == SPV_REFLECT_RESULT_SUCCESS) {
            out_interface.inputs.reserve(input_count);
            for (uint32_t i = 0; i < input_count; ++i) {
                out_interface.inputs.push_back(reflect_var_to_info(input_vars[i]));
            }
        }
    }

    // 3. Parse output variables
    uint32_t output_count = 0;
    result = spvReflectEnumerateOutputVariables(&module, &output_count, nullptr);
    if (result == SPV_REFLECT_RESULT_SUCCESS && output_count > 0) {
        std::vector<SpvReflectInterfaceVariable*> output_vars(output_count);
        result = spvReflectEnumerateOutputVariables(&module, &output_count, output_vars.data());
        if (result == SPV_REFLECT_RESULT_SUCCESS) {
            out_interface.outputs.reserve(output_count);
            for (uint32_t i = 0; i < output_count; ++i) {
                out_interface.outputs.push_back(reflect_var_to_info(output_vars[i]));
            }
        }
    }

    // 4. Sort
    sort_interface_variables(out_interface.inputs);
    sort_interface_variables(out_interface.outputs);

    // 5. Cleanup
    spvReflectDestroyShaderModule(&module);
    return out_interface;
}

bool deren::vulkan::pipeline::validate_interface_match(
    shader_stage_interface const& producer,
    shader_stage_interface const& consumer) {
    // Get the non-builtin outputs and inputs
    std::vector<interface_variable_info const*> producer_outputs;
    std::vector<interface_variable_info const*> consumer_inputs;

    for (auto const& out : producer.outputs) {
        if (!out.is_builtin) {
            producer_outputs.push_back(&out);
        }
    }
    for (auto const& in : consumer.inputs) {
        if (!in.is_builtin) {
            consumer_inputs.push_back(&in);
        }
    }

    // The counts must match
    if (producer_outputs.size() != consumer_inputs.size()) {
        // error: output/input variable count mismatch
        return false;
    }

    // Compare one by one
    for (size_t i = 0; i < producer_outputs.size(); ++i) {
        deren::vulkan::pipeline::interface_variable_info const* p_out = producer_outputs[i];
        deren::vulkan::pipeline::interface_variable_info const* p_in = consumer_inputs[i];

        if (p_out->location != p_in->location) {
            // error: location mismatch
            return false;
        }
        if (p_out->component != p_in->component) {
            // error: component mismatch
            return false;
        }
        if (p_out->format != p_in->format) {
            // error: format mismatch
            return false;
        }
    }

    return true;
}

std::expected<std::vector<deren::vulkan::pipeline::descriptor_set_layout_data>, std::string_view>
deren::vulkan::pipeline::parse_descriptor_set_layouts(
    std::span<uint8_t const> const spirv_code,
    VkShaderStageFlagBits const shader_stage) {

    using fail = std::unexpected<std::string_view>;

    std::vector<descriptor_set_layout_data> set_layouts;

    // 1. Create and load the shader module
    SpvReflectShaderModule module = {};
    SpvReflectResult result = spvReflectCreateShaderModule(
        spirv_code.size(),
        spirv_code.data(),
        &module);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        return fail("spv reflect failed"); // load failed, return empty
    }

    // 2. Query the number of descriptor sets
    uint32_t set_count = 0;
    result = spvReflectEnumerateDescriptorSets(&module, &set_count, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS || set_count == 0) {
        spvReflectDestroyShaderModule(&module);
        return set_layouts;
    }

    // 3. Get pointers to all descriptor sets
    std::vector<SpvReflectDescriptorSet*> sets(set_count);
    result = spvReflectEnumerateDescriptorSets(&module, &set_count, sets.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        spvReflectDestroyShaderModule(&module);
        return set_layouts;
    }

    // 4. Iterate and convert the data
    set_layouts.resize(sets.size());
    for (size_t i = 0; i < sets.size(); ++i) {
        SpvReflectDescriptorSet const& refl_set = *sets[i];
        deren::vulkan::pipeline::descriptor_set_layout_data& layout_data = set_layouts[i];

        layout_data.set_number = refl_set.set;
        layout_data.bindings.resize(refl_set.binding_count);

        for (uint32_t j = 0; j < refl_set.binding_count; ++j) {
            SpvReflectDescriptorBinding const& refl_binding = *(refl_set.bindings[j]);
            VkDescriptorSetLayoutBinding& vk_binding = layout_data.bindings[j];

            vk_binding.binding = refl_binding.binding;
            vk_binding.descriptorType = static_cast<VkDescriptorType>(refl_binding.descriptor_type);

            // Compute the descriptor count, handling arrays
            vk_binding.descriptorCount = 1;
            for (uint32_t dim = 0; dim < refl_binding.array.dims_count; ++dim) {
                vk_binding.descriptorCount *= refl_binding.array.dims[dim];
            }

            // Set the shader stage
            vk_binding.stageFlags = shader_stage;
        }

        // Fill the final VkDescriptorSetLayoutCreateInfo
        layout_data.create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_data.create_info.bindingCount = static_cast<uint32_t>(layout_data.bindings.size());
        layout_data.create_info.pBindings = layout_data.bindings.data();
    }

    // 5. Clean up and return
    spvReflectDestroyShaderModule(&module);
    return set_layouts;
}

std::expected<deren::vulkan::pipeline::push_constant_layout, std::string_view> deren::vulkan::pipeline::parse_push_constant_layout(
    std::span<uint8_t const> spirv_code) {

    using fail = std::unexpected<std::string_view>;
    push_constant_layout out_layout;
    out_layout.constants.clear();
    out_layout.total_size = 0;

    SpvReflectShaderModule module = {};
    SpvReflectResult result = spvReflectCreateShaderModule(
        spirv_code.size(),
        spirv_code.data(),
        &module);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        return fail("spv reflect failed");
    }

    uint32_t pc_count = 0;
    result = spvReflectEnumeratePushConstantBlocks(&module, &pc_count, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS || pc_count == 0) {
        spvReflectDestroyShaderModule(&module);
        return {}; // no Push Constants is a valid state
    }

    std::vector<SpvReflectBlockVariable*> pc_blocks(pc_count);
    result = spvReflectEnumeratePushConstantBlocks(&module, &pc_count, pc_blocks.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        spvReflectDestroyShaderModule(&module);
        return fail("enumerate push constants failed");
    }

    out_layout.constants.reserve(pc_count);
    uint32_t max_offset = 0;

    for (uint32_t i = 0; i < pc_count; ++i) {
        SpvReflectBlockVariable const* p_block = pc_blocks[i];

        for (uint32_t j = 0; j < p_block->member_count; ++j) {
            SpvReflectBlockVariable const& member = p_block->members[j];

            push_constant_info info = {};
            info.offset = member.offset;
            info.size = member.size;
            info.stage_flags = static_cast<VkShaderStageFlags>(module.shader_stage);
            info.name = member.name ? member.name : "unnamed";

            out_layout.constants.push_back(info);

            uint32_t const end = member.offset + member.size;
            max_offset = std::max(end, max_offset);
        }
    }

    std::ranges::sort(out_layout.constants,
                      [](push_constant_info const& a, push_constant_info const& b) {
                          return a.offset < b.offset;
                      });

    out_layout.total_size = max_offset;

    // 6. Cleanup
    spvReflectDestroyShaderModule(&module);
    return out_layout;
}