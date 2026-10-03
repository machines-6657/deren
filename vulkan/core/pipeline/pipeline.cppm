module;

#include <vulkan/vulkan.h>

export module deren.vulkan.core.pipeline;
export import deren.vstd;
export import deren.vulkan.core.handles;

/**
 * @file pipeline.cppm
 * @defgroup vulkan_pipeline Vulkan Pipeline
 * @brief graphics pipeline creation from raw SPIR-V binary
 * @note
 *      - make_pipeline() builds a full pipeline from raw SPIR-V binary
 *      - the vertex input layout is parsed from the SPIR-V itself, no user-defined structure needed
 *      - the descriptor set layout and push constant block are NOT parsed: every stage is heap-native and the
 *        pipeline is created with a NULL layout, so a pass reaches its descriptors through the frame's heap bind
 *      - returns std::expected<vk_pipeline, std::string_view>, errors carry a message
 */
namespace deren::vulkan {
    /**
     * @ingroup vulkan_pipeline
     * @brief create a graphics pipeline directly from raw SPIR-V binary
     * @param device the logical device
     * @param color_format swapchain color attachment format (VK_FORMAT_UNDEFINED for depth-only
     *        pipelines with no color attachment)
     * @param depth_format depth attachment format
     * @param vertex_shader_code raw SPIR-V binary of the vertex shader
     * @param fragment_shader_code raw SPIR-V binary of the fragment shader
     * @param msaa_level the render instance's sample count (1 in this engine)
     * @param depth_test_enabled enable depth test + depth write (false e.g. for the skybox pass)
     * @param has_color_attachment whether the pipeline renders color (false for depth-only
     *        passes like the shadow map: no color attachment, no color blending)
     * @param depth_bias_constant_factor constant rasterization depth bias added to depth
     * @param depth_bias_slope_factor slope-scaled depth bias (removes shadow acne on angled surfaces)
     * @param depth_bias_clamp maximum depth bias magnitude (0 = no clamp)
     * @return vk_pipeline on success, error message on failure
     * @note created for DYNAMIC RENDERING (Vulkan 1.3 core, the only path the engine uses): the
     *       attachment formats are declared through VkPipelineRenderingCreateInfo, no render pass
     */
    export std::expected<vk_pipeline, std::string_view> make_pipeline(
        VkDevice device,
        VkFormat color_format,
        VkFormat depth_format,
        std::span<uint8_t const> vertex_shader_code,
        std::span<uint8_t const> fragment_shader_code,
        VkSampleCountFlagBits msaa_level,
        bool depth_test_enabled = true,
        bool has_color_attachment = true,
        // fixed-function rasterization depth bias (only meaningful for depth-writing passes
        // such as the shadow map): slope-scaled bias removes shadow acne on angled surfaces
        float depth_bias_constant_factor = 0.0f,
        float depth_bias_slope_factor = 0.0f,
        float depth_bias_clamp = 0.0f,
        // THE STAGE THAT EMITS THE GEOMETRY, and it is a parameter because a MESH stage replaces the vertex
        // stage rather than sitting beside it (see docs/mesh_shaders.md): the mesh module is passed in
        // `vertex_shader_code` (it is the pipeline's first stage either way), its module is created as
        // VK_SHADER_STAGE_MESH_BIT_EXT, and the VERTEX INPUT STATE is not derived from it - a mesh stage
        // fetches its own vertices, so there is no input interface to parse and no binding to describe.
        VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_VERTEX_BIT);

    /**
     * @ingroup vulkan_pipeline
     * @brief multi-target variant of make_pipeline(): the pipeline writes @p color_formats.size()
     *        color attachments (the G-buffer pass writes three - albedo/metallic,
     *        normal/roughness, material id/AO/flags), one blend attachment per color target
     * @param color_formats attachment formats in attachment order, which the fragment shader's
     *        layout(location = i) outputs must match one for one; empty = no color attachment
     *        (depth-only, the same as has_color_attachment false above)
     * @param depth_format depth attachment format
     * @param msaa_level the render instance's sample count (the G-buffer is 1x)
     * @param depth_test_enabled enable depth test + depth write
     * @param depth_bias_* fixed-function rasterization depth bias
     * @param blend_attachments per-color-attachment blend state, in attachment order; EMPTY (the
     *        default) means every target is overwritten (make_color_blend_attachment_opaque), which
     *        is what a G-buffer surface target needs - alpha there carries data (metallic /
     *        roughness / flags), so src-alpha blending would mix the surface with the cleared target.
     *        A non-empty list must have exactly one entry per color format, and lets a pass mix
     *        states per target: the G-buffer pass overwrites its three surface targets and accumulates
     *        into the HDR target it adds emissive to (make_color_blend_attachment_additive)
     * @return vk_pipeline on success, error message on failure
     */
    export std::expected<vk_pipeline, std::string_view> make_pipeline(
        VkDevice device,
        std::span<VkFormat const> color_formats,
        VkFormat depth_format,
        std::span<uint8_t const> vertex_shader_code,
        std::span<uint8_t const> fragment_shader_code,
        VkSampleCountFlagBits msaa_level,
        bool depth_test_enabled = true,
        float depth_bias_constant_factor = 0.0f,
        float depth_bias_slope_factor = 0.0f,
        float depth_bias_clamp = 0.0f,
        std::span<VkPipelineColorBlendAttachmentState const> blend_attachments = {},
        /// @copydoc make_pipeline(VkDevice, VkFormat, VkFormat, std::span<uint8_t const>, std::span<uint8_t const>, VkSampleCountFlagBits, bool, bool, float, float, float)
        /// (the same `first_stage`: VERTEX derives the vertex input state from the module, MESH does not)
        VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_VERTEX_BIT,
        /**
         * @param depth_compare_op the depth COMPARE OPERATOR, defaulted to what every pipeline in this
         *        renderer used before the parameter existed, so no existing caller's pipeline changes.
         *
         *        The CHARACTER-FORWARD pass is the first caller that needs another value, and `EQUAL` is
         *        the reason that pass is shaped the way it is: it draws the same leaves a second time to
         *        overwrite the pixels the deferred stage already lit, and `EQUAL` is what confines the
         *        overwrite to exactly the surface the G-buffer pass recorded. Depth WRITE is not a
         *        parameter here because it is already a DYNAMIC state (see the dynamic-state list in
         *        pipeline.cpp) - a pass that needs the test on with the write off sets it per draw.
         */
        VkCompareOp depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL);
} // namespace deren::vulkan
