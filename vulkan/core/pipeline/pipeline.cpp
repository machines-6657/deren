module;

#include <vulkan/vulkan.h>

module deren.vulkan.core.pipeline;
import :spirv_parser;
import deren.vulkan.constant_init;

namespace {
    // Collects the Vulkan objects created during pipeline creation; the destructor frees the
    // pipeline if any later step fails; on full success, release() surrenders ownership to vk_pipeline.
    struct resource_guard {
        VkDevice device = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;

        void release() noexcept {
            this->device = VK_NULL_HANDLE;
            this->pipeline = VK_NULL_HANDLE;
        }

        ~resource_guard() {
            if (this->device == VK_NULL_HANDLE) {
                return;
            }
            if (this->pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(this->device, this->pipeline, nullptr);
            }
        }
    };
} // namespace

namespace deren::vulkan {
    std::expected<vk_pipeline, std::string_view> make_pipeline(
        VkDevice device,
        VkFormat const color_format,
        VkFormat const depth_format,
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkSampleCountFlagBits const msaa_level,
        bool const depth_test_enabled,
        bool const has_color_attachment,
        float const depth_bias_constant_factor,
        float const depth_bias_slope_factor,
        float const depth_bias_clamp,
        VkShaderStageFlagBits const first_stage) {
        // Single-target convenience form: forward the (0 or 1)-element format list to the
        // multi-target implementation below, with the engine's standard src-alpha blending (the
        // forward pipelines' convention: alpha is coverage, and an opaque draw's alpha 1 reduces the
        // blend math to the source color).
        std::array<VkFormat, 1> const single_format = {color_format};
        std::array<VkPipelineColorBlendAttachmentState, 1> const single_blend = {make_color_blend_attachment()};
        std::span<VkFormat const> const color_formats = has_color_attachment ? std::span<VkFormat const>(single_format) : std::span<VkFormat const>{};
        std::span<VkPipelineColorBlendAttachmentState const> const blend_attachments = has_color_attachment ? std::span<VkPipelineColorBlendAttachmentState const>(single_blend) : std::span<VkPipelineColorBlendAttachmentState const>{};
        return make_pipeline(device,
                             color_formats,
                             depth_format,
                             vertex_shader_code,
                             fragment_shader_code,
                             msaa_level,
                             depth_test_enabled,
                             depth_bias_constant_factor,
                             depth_bias_slope_factor,
                             depth_bias_clamp,
                             blend_attachments,
                             first_stage);
    }

    std::expected<vk_pipeline, std::string_view> make_pipeline( // NOLINT(*-function-cognitive-complexity)
        VkDevice device,
        std::span<VkFormat const> const color_formats,
        VkFormat const depth_format,
        std::span<uint8_t const> const vertex_shader_code,
        std::span<uint8_t const> const fragment_shader_code,
        VkSampleCountFlagBits const msaa_level,
        bool const depth_test_enabled,
        float const depth_bias_constant_factor,
        float const depth_bias_slope_factor,
        float const depth_bias_clamp,
        std::span<VkPipelineColorBlendAttachmentState const> const blend_attachments,
        VkShaderStageFlagBits const first_stage,
        VkCompareOp const depth_compare_op) {
        using fail = std::unexpected<std::string_view>;
        if (!blend_attachments.empty() && blend_attachments.size() != color_formats.size()) {
            return fail("make_pipeline: a blend attachment per color format is required");
        }

        // ---- 1. Parse the first stage's interface, filter builtins, build vertex input ----
        // A MESH FIRST STAGE HAS NO VERTEX INPUT, and this branch is not a shortcut: the mesh stage fetches
        // its own vertices, the vertex-input state is ignored for such a pipeline, and parsing a mesh module
        // as a VERTEX interface would derive nothing (it declares no Input variables) - an empty attribute
        // list that silently reads no geometry. See docs/mesh_shaders.md.
        std::vector<VkVertexInputAttributeDescription> attribute_descriptions;
        uint32_t stride = 0;
        if (first_stage == VK_SHADER_STAGE_VERTEX_BIT) {
            auto vertex_interface_expected = pipeline::parse_shader_stage_interface(vertex_shader_code, VK_SHADER_STAGE_VERTEX_BIT);
            if (!vertex_interface_expected) {
                return fail(vertex_interface_expected.error());
            }
            auto const vertex_interface = std::move(vertex_interface_expected).value();

            attribute_descriptions.reserve(vertex_interface.inputs.size());
            for (auto const& variable : vertex_interface.inputs) {
                if (variable.is_builtin || variable.format == VK_FORMAT_UNDEFINED) {
                    continue;
                }
                attribute_descriptions.push_back(VkVertexInputAttributeDescription{
                    .location = variable.location,
                    .binding = 0,
                    .format = variable.format,
                    .offset = stride,
                });
                stride += pipeline::format_size(variable.format);
            }
        }

        VkVertexInputBindingDescription vertex_input_binding = {};
        vertex_input_binding.binding = 0;
        vertex_input_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        vertex_input_binding.stride = stride;

        VkPipelineVertexInputStateCreateInfo vertex_input_state_create_info = {};
        vertex_input_state_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input_state_create_info.vertexBindingDescriptionCount = attribute_descriptions.empty() ? 0u : 1u;
        vertex_input_state_create_info.pVertexBindingDescriptions = attribute_descriptions.empty() ? nullptr : &vertex_input_binding;
        vertex_input_state_create_info.vertexAttributeDescriptionCount = static_cast<uint32_t>(attribute_descriptions.size());
        vertex_input_state_create_info.pVertexAttributeDescriptions = attribute_descriptions.empty() ? nullptr : attribute_descriptions.data();

        // ---- 2. Create shader modules (errors only on failure) ----
        auto vertex_shader_module = make_shader_module(vertex_shader_code, device);
        if (!vertex_shader_module) {
            return fail("failed to create vertex shader module");
        }

        auto fragment_shader_module = make_shader_module(fragment_shader_code, device);
        if (!fragment_shader_module) {
            return fail("failed to create fragment shader module");
        }

        // ---- 3. Fixed-function pipeline state (constexpr factories, see above) ----
        std::array<VkPipelineShaderStageCreateInfo, 2> shader_stage_create_infos = {
            make_shader_stage(**vertex_shader_module, first_stage),
            make_shader_stage(**fragment_shader_module, VK_SHADER_STAGE_FRAGMENT_BIT),
        };

        // double-sided materials need per-draw cull control (core dynamic state since Vulkan 1.3);
        // transparent (alphaMode BLEND) leaves disable depth writes per draw, also a 1.3 core
        // dynamic state, so one pipeline serves both opaque and blended draws
        std::array<VkDynamicState, 6> dynamic_states = {};
        uint32_t dynamic_state_count = 4;
        dynamic_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
        dynamic_states[1] = VK_DYNAMIC_STATE_SCISSOR;
        dynamic_states[2] = VK_DYNAMIC_STATE_CULL_MODE;
        dynamic_states[3] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE;
        // depth-bias pipelines (the shadow pass) take the three bias factors as dynamic state,
        // so the values can be tuned live (e.g. from the debug gui) without recreating the
        // pipeline; depthBiasEnable itself stays static below
        bool const depth_bias_enabled = depth_bias_constant_factor != 0.0f || depth_bias_slope_factor != 0.0f || depth_bias_clamp != 0.0f;
        if (depth_bias_enabled) {
            dynamic_states[dynamic_state_count++] = VK_DYNAMIC_STATE_DEPTH_BIAS;
        }

        VkPipelineInputAssemblyStateCreateInfo const input_assembly_state_create_info = make_input_assembly_state();
        VkPipelineViewportStateCreateInfo const viewport_state_create_info = make_viewport_state();
        VkPipelineDynamicStateCreateInfo const dynamic_state_create_info = make_dynamic_state(dynamic_states.data(), dynamic_state_count);
        VkPipelineRasterizationStateCreateInfo const rasterization_state_create_info = make_rasterization_state(depth_bias_enabled, depth_bias_constant_factor, depth_bias_slope_factor, depth_bias_clamp);
        VkPipelineDepthStencilStateCreateInfo const depth_stencil_state_create_info = make_depth_stencil_state(depth_test_enabled, depth_compare_op);
        // one blend attachment per color target, either the caller's list (a pass that mixes states
        // per target: the G-buffer overwrites its three surface targets and accumulates into the HDR
        // target) or the opaque overwrite state (a G-buffer must not blend: there alpha is
        // metallic/roughness/flags data - see make_color_blend_attachment_opaque)
        VkPipelineColorBlendAttachmentState const default_blend = make_color_blend_attachment_opaque();
        std::vector<VkPipelineColorBlendAttachmentState> const opaque_blend_attachments(color_formats.size(), default_blend);
        std::span<VkPipelineColorBlendAttachmentState const> const blends = blend_attachments.empty() ? std::span<VkPipelineColorBlendAttachmentState const>(opaque_blend_attachments) : blend_attachments;
        VkPipelineColorBlendStateCreateInfo const color_blend_state_create_info = make_color_blend_state(blends.data(), static_cast<uint32_t>(blends.size()));
        VkPipelineMultisampleStateCreateInfo const multisample_state_create_info = make_multisample_state(msaa_level);

        // ---- 4. Pipeline layout: the shared scene layout (passed in) already carries the
        //         agreed flat descriptor set 0 and the fixed push constant block; nothing to
        //         parse from SPIR-V for the indexed layout (see core::init_scene_layouts) ----
        resource_guard guard;
        guard.device = device;

        // ---- 5. graphics pipeline ----
        // Dynamic rendering (Vulkan 1.3 core, the only path the engine uses): attachments are
        // declared through VkPipelineRenderingCreateInfo in the pNext chain instead of a render
        // pass + subpass. Depth-only pipelines (has_color_attachment == false, e.g. the shadow
        // pass) declare no color attachment format.
        VkPipelineRenderingCreateInfo const rendering_create_info = make_rendering_create_info(color_formats.data(), static_cast<uint32_t>(color_formats.size()), depth_format);
        // THE HEAP FLAG AND THE NULL LAYOUT (see docs/descriptor_heap_migration.md): every stage is heap-native now,
        // and validation's rule is explicit - a shader that declares heap resources cannot also be given a pipeline
        // layout ("either set the layout to NULL or remove the heaps from the shader"). The flag is a flags2 bit, so
        // it arrives through VkPipelineCreateFlags2CreateInfo, which chains ahead of the rendering info this
        // pipeline already carries.
        VkPipelineCreateFlags2CreateInfo heap_flags = {};
        heap_flags.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
        heap_flags.pNext = &rendering_create_info;
        heap_flags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

        VkGraphicsPipelineCreateInfo pipeline_create_info = {};
        pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_create_info.pNext = &heap_flags;
        pipeline_create_info.renderPass = VK_NULL_HANDLE; // dynamic rendering: no render pass
        pipeline_create_info.pInputAssemblyState = &input_assembly_state_create_info;
        pipeline_create_info.pViewportState = &viewport_state_create_info;
        pipeline_create_info.pDepthStencilState = &depth_stencil_state_create_info;
        pipeline_create_info.pColorBlendState = &color_blend_state_create_info;
        pipeline_create_info.pVertexInputState = &vertex_input_state_create_info;
        pipeline_create_info.layout = VK_NULL_HANDLE; // the heap flag REQUIRES it (see the note above)
        pipeline_create_info.pRasterizationState = &rasterization_state_create_info;
        pipeline_create_info.pMultisampleState = &multisample_state_create_info;
        pipeline_create_info.pDynamicState = &dynamic_state_create_info;
        pipeline_create_info.stageCount = 2;
        pipeline_create_info.pStages = shader_stage_create_infos.data();
        pipeline_create_info.subpass = 0;
        pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;

        if (vkCreateGraphicsPipelines(device, nullptr, 1, &pipeline_create_info, nullptr, &guard.pipeline) != VK_SUCCESS) {
            return fail("failed to create graphics pipeline");
        }

        // ---- 8. Success: transfer ownership to vk_pipeline; guard no longer cleans up ----
        vk_pipeline result(guard.pipeline, device);
        guard.release();
        return result;
    }
} // namespace deren::vulkan
