// ============================================================================
// module: deren.vulkan.constant_init
// module version: 0.9.1  (independent of the app version in CMakeLists project(VERSION))
//
// Compile-time Vulkan info-struct conventions: constexpr factories + constinit
// "transition" defaults for the structs the engine fills identically everywhere
// (object create infos, command-buffer / secondary inheritance, fixed-function
// pipeline state, per-frame layout transitions). Top-level module: depends on
// nothing but the Vulkan headers, so any Vulkan module can embed it.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <vulkan/vulkan.h>

export module deren.vulkan.constant_init;

/**
 * @defgroup vulkan_constant_init Vulkan Info-Struct Builders (fixed conventions)
 * @file constant_init.cppm
 *
 * @brief constexpr constructors and constinit defaults for the Vulkan info structs the engine
 *        fills the same way everywhere.
 *
 * Top-level module (sibling of deren.vulkan.core): it depends on nothing but the Vulkan headers, so
 * any Vulkan module can use it. The name sets it apart from deren.vulkan.core:init_utils - that
 * module performs the initialization PROCEDURES (instance/device/swapchain), while this one
 * holds the compile-time CONSTANTS of those calls: the fixed field values ("constant init").
 *
 * The engine never hand-fills these structs at call sites: every fill is either
 *  - a constexpr factory returning the struct by value (each factory lists EVERY member with
 *    designated initializers, so the zero/unused fields are explicit and identical to a
 *    `= {}` zero-init), or
 *  - a constinit "transition" default the caller copies and then overrides only the fields
 *    that actually differ (e.g. the target image of a per-frame layout transition).
 *
 * Pointer members always point at caller-owned data (never at locals of the factory itself).
 * This module is header-only in effect: all definitions live in the interface, so callers can
 * constant-fold the factories.
 */
export namespace deren::vulkan {
    // ---- Object create infos (one line per object; fields fixed by engine convention) ----

    /**
     * @brief whether a colour format's attachment write path encodes linear -> sRGB in HARDWARE
     *
     * WHY IT IS HERE rather than in the renderer that used to own it: two PASSES need the answer (the composite
     * and FXAA, which push the same block's `encode_gamma` lane) and a third thing needs it for the same reason
     * (the swapchain's own format decides whether the display transfer function is hardware's or the shader's).
     * Writing a gamma-encoded value into one of these formats double-encodes it, and writing it into a UNORM
     * target under-encodes it - so the question is asked on every post frame, and a second copy of the switch is a
     * second place for the list of formats to fall behind.
     */
    [[nodiscard]] constexpr bool is_srgb_format(VkFormat const format) noexcept {
        switch (format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_SRGB:
            return true;
        default:
            return false;
        }
    }

    /**
     * @brief command pool that allows per-buffer reset (the engine resets/re-records buffers)
     */
    constexpr VkCommandPoolCreateInfo make_command_pool_info(uint32_t const queue_family) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = queue_family};
    }
    /** @brief plain binary semaphore (vkAcquireNextImageKHR / vkQueuePresentKHR need binary) */
    constexpr VkSemaphoreCreateInfo make_binary_semaphore_info() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0};
    }
    /** @brief timeline semaphore starting at 0 (frame-slot submission counting + host pacing) */
    constexpr VkSemaphoreTypeCreateInfo make_timeline_semaphore_type_info() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                .pNext = nullptr,
                .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                .initialValue = 0};
    }
    /**
     * @brief linear/mipmap sampler with repeat-style addressing
     * @param address_mode applied to U/V/W
     * @param max_lod upper clamp (e.g. the texture's mip count - 1)
     */
    constexpr VkSamplerCreateInfo make_texture_sampler_info(VkSamplerAddressMode const address_mode, float const max_lod) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .magFilter = VK_FILTER_LINEAR,
                .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                .addressModeU = address_mode,
                .addressModeV = address_mode,
                .addressModeW = address_mode,
                .mipLodBias = 0.0f,
                .anisotropyEnable = VK_FALSE,
                .maxAnisotropy = 1.0f,
                .compareEnable = VK_FALSE,
                .compareOp = VK_COMPARE_OP_NEVER,
                .minLod = 0.0f,
                .maxLod = max_lod,
                .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                .unnormalizedCoordinates = VK_FALSE};
    }
    /**
     * @brief shadow-map sampler: LINEAR min/mag gives HARDWARE percentage-closer filtering on a
     *        sampler2DShadow (compareOp matches pbr.frag's "not deeper than stored depth")
     */
    constexpr VkSamplerCreateInfo make_shadow_sampler_info() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .magFilter = VK_FILTER_LINEAR,
                .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                .mipLodBias = 0.0f,
                .anisotropyEnable = VK_FALSE,
                .maxAnisotropy = 1.0f,
                .compareEnable = VK_TRUE,
                .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
                .minLod = 0.0f,
                .maxLod = 0.0f,
                .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                .unnormalizedCoordinates = VK_FALSE};
    }
    /** @brief unsignaled fence (host waits after one-shot upload submits) */
    constexpr VkFenceCreateInfo make_fence_info() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0};
    }
    /**
     * @brief query pool of @p count queries, all of type @p query_type
     * @param query_type TIMESTAMP for the GPU pass timings, OCCLUSION for visibility queries
     * @param count number of queries in the pool (a timestamp pool also fixes how many marks a
     *        frame may write: see core::gpu_timing_mark_capacity)
     */
    constexpr VkQueryPoolCreateInfo make_query_pool_info(VkQueryType const query_type, uint32_t const count) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = query_type,
                .queryCount = count,
                .pipelineStatistics = 0};
    }
    /** @brief host-visible staging buffer: TRANSFER_SRC only, exclusive sharing */
    constexpr VkBufferCreateInfo make_staging_buffer_info(VkDeviceSize const size) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = size,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr};
    }
    /**
     * @brief one-queue device queue create info (one queue of @p queue_family)
     * @param queue_priorities caller-owned array of @p queue_family's queue priorities
     */
    constexpr VkDeviceQueueCreateInfo make_device_queue_info(uint32_t const queue_family, float const* queue_priorities) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queueFamilyIndex = queue_family,
                .queueCount = 1,
                .pQueuePriorities = queue_priorities};
    }
    /**
     * @brief 2D image view with identity component swizzle, from mip 0 / layer 0
     * @param image the image to view
     * @param format the image's format
     * @param view_type usually 2D
     * @param aspect_mask color or depth(-stencil)
     * @param level_count mip levels in the view (VK_REMAINING_MIP_LEVELS for the whole image)
     * @param layer_count array layers in the view (VK_REMAINING_ARRAY_LAYERS for the whole image)
     */
    constexpr VkImageViewCreateInfo make_image_view_info(VkImage const image, VkFormat const format, VkImageViewType const view_type, VkImageAspectFlags const aspect_mask, uint32_t const level_count, uint32_t const layer_count) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = image,
                .viewType = view_type,
                .format = format,
                .components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {aspect_mask, 0, level_count, 0, layer_count}};
    }

    // ---- Command buffer / submit infos ----

    /**
     * @brief command buffer allocation: @p level buffers from @p pool
     * @param pool command pool to allocate from
     * @param level primary or secondary
     */
    constexpr VkCommandBufferAllocateInfo make_command_buffer_allocate_info(VkCommandPool const pool, VkCommandBufferLevel const level) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = pool,
                .level = level,
                .commandBufferCount = 1};
    }
    /**
     * @brief command buffer begin info
     * @param flags usage flags (0 for plain inline recording, ONE_TIME_SUBMIT for one-shot
     *        uploads, RENDER_PASS_CONTINUE for secondaries)
     * @param inheritance secondary-buffer inheritance info (nullptr for primary buffers)
     */
    constexpr VkCommandBufferBeginInfo make_command_buffer_begin_info(VkCommandBufferUsageFlags const flags, VkCommandBufferInheritanceInfo const* inheritance) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = flags,
                .pInheritanceInfo = inheritance};
    }
    /**
     * @brief one-shot submit of a single command buffer with no semaphores
     * @param command_buffers pointer to the caller's VkCommandBuffer
     */
    constexpr VkSubmitInfo make_submit_info(VkCommandBuffer const* command_buffers) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = nullptr,
                .waitSemaphoreCount = 0,
                .pWaitSemaphores = nullptr,
                .pWaitDstStageMask = nullptr,
                .commandBufferCount = 1,
                .pCommandBuffers = command_buffers,
                .signalSemaphoreCount = 0,
                .pSignalSemaphores = nullptr};
    }

    // ---- Secondary-command-buffer inheritance (dynamic rendering 1.3) ----

    /**
     * @brief VkCommandBufferInheritanceInfo shell; only the pNext chain (the rendering info)
     *        differs per buffer
     * @param p_next points at the VkCommandBufferInheritanceRenderingInfo (caller-owned)
     */
    constexpr VkCommandBufferInheritanceInfo make_inheritance_info(void const* p_next) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
                .pNext = p_next,
                .renderPass = VK_NULL_HANDLE,
                .subpass = 0,
                .framebuffer = VK_NULL_HANDLE,
                .occlusionQueryEnable = VK_FALSE,
                .queryFlags = 0,
                .pipelineStatistics = 0};
    }
    /**
     * @brief dynamic-rendering inheritance: which attachments a secondary may assume
     * @param has_color_attachment depth-only secondaries (shadow pass) pass false
     * @param color_format_ptr caller-owned color format (ignored when has_color_attachment
     *        is false)
     * @param depth_format the depth attachment's format (VK_FORMAT_UNDEFINED if none)
     * @param rasterization_samples the instance's sample count (always 1 in this engine)
     */
    constexpr VkCommandBufferInheritanceRenderingInfo make_inheritance_rendering_info(bool const has_color_attachment, VkFormat const* color_format_ptr, VkFormat const depth_format, VkSampleCountFlagBits const rasterization_samples) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
                .pNext = nullptr,
                .flags = 0,
                .viewMask = 0,
                .colorAttachmentCount = has_color_attachment ? 1u : 0u,
                .pColorAttachmentFormats = has_color_attachment ? color_format_ptr : nullptr,
                .depthAttachmentFormat = depth_format,
                .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
                .rasterizationSamples = rasterization_samples};
    }
    /**
     * @brief dynamic-rendering inheritance with N color attachments: a secondary recorded for a
     *        multi-target instance (the G-buffer) must declare every format it may write
     * @param color_formats caller-owned array of @p color_count formats, in attachment order
     * @param color_count number of color attachments declared
     * @param depth_format the depth attachment's format (VK_FORMAT_UNDEFINED if none)
     * @param rasterization_samples the instance's sample count (always 1 in this engine)
     */
    constexpr VkCommandBufferInheritanceRenderingInfo make_inheritance_rendering_info(VkFormat const* color_formats, uint32_t const color_count, VkFormat const depth_format, VkSampleCountFlagBits const rasterization_samples) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
                .pNext = nullptr,
                .flags = 0,
                .viewMask = 0,
                .colorAttachmentCount = color_count,
                .pColorAttachmentFormats = color_count > 0 ? color_formats : nullptr,
                .depthAttachmentFormat = depth_format,
                .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
                .rasterizationSamples = rasterization_samples};
    }
    /**
     * @brief one barrier pass: VkDependencyInfo with only image memory barriers
     * @param image_barrier_count number of barriers
     * @param barriers caller-owned barrier array
     */
    constexpr VkDependencyInfo make_image_dependency_info(uint32_t const image_barrier_count, VkImageMemoryBarrier2 const* barriers) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .pNext = nullptr,
                .dependencyFlags = 0,
                .memoryBarrierCount = 0,
                .pMemoryBarriers = nullptr,
                .bufferMemoryBarrierCount = 0,
                .pBufferMemoryBarriers = nullptr,
                .imageMemoryBarrierCount = image_barrier_count,
                .pImageMemoryBarriers = barriers};
    }

    // ---- Dynamic rendering attachment infos ----

    /**
     * @brief depth attachment of a rendering instance: loadOp CLEAR with the far-plane value
     *        (1.0, stencil 0 - the engine clears every attachment on load) and no resolve
     * @param image_view the depth image view
     * @param store_op DONT_CARE only for a depth buffer nothing ever reads back (the forward
     *        path's main depth); STORE for the shadow map and the G-buffer depth, whose contents
     *        a later pass in the same submission samples
     */
    constexpr VkRenderingAttachmentInfo make_depth_attachment_info(VkImageView const image_view, VkAttachmentStoreOp const store_op) noexcept {
        VkClearValue clear_value = {};
        clear_value.depthStencil = {1.0f, 0};
        return {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = image_view,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .resolveImageView = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = store_op,
                .clearValue = clear_value};
    }
    /**
     * @brief depth attachment of an instance that CONTINUES an existing depth buffer: loadOp LOAD,
     *        storeOp STORE, so the depth an earlier instance wrote stays intact
     * @param image_view the depth image view, already in GENERAL
     * @note the deferred path's transparent pass: it depth-tests alpha-blended geometry against the
     *       opaque surface the G-buffer pass wrote, and must not clear it (that depth is the only
     *       record of where the opaque geometry is)
     */
    constexpr VkRenderingAttachmentInfo make_load_depth_attachment_info(VkImageView const image_view) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = image_view,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .resolveImageView = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = {}};
    }
    /**
     * @brief color attachment of a rendering instance: GENERAL layout,
     *        loadOp CLEAR + storeOp STORE (the swapchain image is presented afterwards)
     * @param image_view the color image view
     * @param clear_value the runtime clear color
     * @param resolve_mode the resolve mode (the engine always renders at 1x, so NONE)
     * @param resolve_image_view the resolve target when resolve_mode is not NONE, else VK_NULL_HANDLE.
     *        NOTE: the resolve layout must not be PRESENT_SRC_KHR
     *        (VUID-VkRenderingAttachmentInfo-imageView-06146) - the swapchain image moves to
     *        PRESENT_SRC_KHR only after vkCmdEndRendering
     */
    constexpr VkRenderingAttachmentInfo make_color_attachment_info(VkImageView const image_view, VkClearValue const clear_value, VkResolveModeFlagBits const resolve_mode, VkImageView const resolve_image_view) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = image_view,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                .resolveMode = resolve_mode,
                .resolveImageView = resolve_image_view,
                .resolveImageLayout = resolve_mode == VK_RESOLVE_MODE_NONE ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = clear_value};
    }
    /**
     * @brief color attachment of a rendering instance that LOADS the image's existing contents
     *        (no clear) and stores the result
     * @param image_view the color image view to render into
     * @note the deferred path's HDR target: the background pass and the G-buffer pass's emissive
     *       already wrote it, and the lighting stage adds on top - clearing it would throw both away
     */
    constexpr VkRenderingAttachmentInfo make_load_color_attachment_info(VkImageView const image_view) noexcept {
        VkClearValue clear_value = {};
        return {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = image_view,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .resolveImageView = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = clear_value};
    }
    /**
     * @brief dynamic-rendering instance: one layer, at most one color attachment
     * @param flags e.g. VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
     * @param render_area the area to render into (the pipelines apply their own viewport/scissor)
     * @param has_color_attachment depth-only passes (the shadow map) pass false
     * @param color_attachments pointer to the single color attachment when present, else nullptr
     * @param depth_attachment the depth attachment, or nullptr
     */
    constexpr VkRenderingInfo make_rendering_info(VkRenderingFlags const flags, VkRect2D const render_area, bool const has_color_attachment, VkRenderingAttachmentInfo const* color_attachments, VkRenderingAttachmentInfo const* depth_attachment) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext = nullptr,
                .flags = flags,
                .renderArea = render_area,
                .layerCount = 1,
                .viewMask = 0,
                .colorAttachmentCount = has_color_attachment ? 1u : 0u,
                .pColorAttachments = has_color_attachment ? color_attachments : nullptr,
                .pDepthAttachment = depth_attachment,
                .pStencilAttachment = nullptr};
    }
    /**
     * @brief dynamic-rendering instance with N color attachments (the G-buffer pass writes three:
     *        albedo/metallic, normal/roughness, material id/AO/flags)
     * @param flags e.g. VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
     * @param render_area the area to render into (the pipelines apply their own viewport/scissor)
     * @param color_attachments caller-owned array of @p color_count attachments (in the order the
     *        fragment shader's layout(location = i) outputs address)
     * @param color_count number of color attachments (0 for a depth-only pass, same as passing
     *        false to the single-attachment overload above)
     * @param depth_attachment the depth attachment, or nullptr
     */
    constexpr VkRenderingInfo make_rendering_info(VkRenderingFlags const flags, VkRect2D const render_area, VkRenderingAttachmentInfo const* color_attachments, uint32_t const color_count, VkRenderingAttachmentInfo const* depth_attachment) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext = nullptr,
                .flags = flags,
                .renderArea = render_area,
                .layerCount = 1,
                .viewMask = 0,
                .colorAttachmentCount = color_count,
                .pColorAttachments = color_count > 0 ? color_attachments : nullptr,
                .pDepthAttachment = depth_attachment,
                .pStencilAttachment = nullptr};
    }

    // ---- Fixed-function pipeline state (engine-wide conventions) ----

    /** @brief one shader stage of a graphics pipeline (entry point "main", no specialization) */
    constexpr VkPipelineShaderStageCreateInfo make_shader_stage(VkShaderModule const module, VkShaderStageFlagBits const stage) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = stage,
                .module = module,
                .pName = "main",
                .pSpecializationInfo = nullptr};
    }
    /** @brief triangle-list input assembly */
    constexpr VkPipelineInputAssemblyStateCreateInfo make_input_assembly_state() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                .primitiveRestartEnable = VK_FALSE};
    }
    /** @brief viewport/scissor are dynamic state: only the counts are static */
    constexpr VkPipelineViewportStateCreateInfo make_viewport_state() noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .viewportCount = 1,
                .pViewports = nullptr,
                .scissorCount = 1,
                .pScissors = nullptr};
    }
    /** @brief dynamic state list (caller-owned array) */
    constexpr VkPipelineDynamicStateCreateInfo make_dynamic_state(VkDynamicState const* states, uint32_t const count) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .dynamicStateCount = count,
                .pDynamicStates = states};
    }
    /**
     * @brief fill rasterization, back-face cull, CCW front face, optional slope-scaled depth bias
     * @param depth_bias_enabled the bias is dynamic state on shadow pipelines; the static
     *        factors only matter while it is never set dynamically
     * @param depth_bias_constant_factor constant depth bias
     * @param depth_bias_slope_factor slope-scaled depth bias
     * @param depth_bias_clamp depth bias clamp, 0 disables clamping
     */
    constexpr VkPipelineRasterizationStateCreateInfo make_rasterization_state(bool const depth_bias_enabled, float const depth_bias_constant_factor, float const depth_bias_slope_factor, float const depth_bias_clamp) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .depthClampEnable = VK_FALSE,
                .rasterizerDiscardEnable = VK_FALSE,
                .polygonMode = VK_POLYGON_MODE_FILL,
                .cullMode = VK_CULL_MODE_BACK_BIT,
                .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                .depthBiasEnable = depth_bias_enabled ? VK_TRUE : VK_FALSE,
                .depthBiasConstantFactor = depth_bias_constant_factor,
                .depthBiasClamp = depth_bias_clamp,
                .depthBiasSlopeFactor = depth_bias_slope_factor,
                .lineWidth = 1.0f};
    }
    /**
     * @brief depth test + write, with the COMPARE OPERATOR chosen by the caller
     * @param depth_test_enabled enable the test; DEPTH WRITE FOLLOWS IT, which is this family's rule and
     *        is why a pass that needs the test ON with the write OFF must turn the write off through the
     *        DYNAMIC setter instead: depth write is a dynamic state here (see the dynamic-state list in
     *        deren.vulkan.core.pipeline), the compare operator is not.
     * @param depth_compare_op the operator. LESS_OR_EQUAL is what every pipeline in the renderer used
     *        before this parameter existed; the CHARACTER-FORWARD pass is the first caller that needs a
     *        different one, and the reason is the whole point of that pass: it OVERWRITES the pixels the
     *        deferred stage already lit, and `EQUAL` is what restricts that overwrite to exactly the
     *        surface the G-buffer pass recorded. With LESS_OR_EQUAL it would also re-shade fragments that
     *        happen to be in front of the recorded depth, which is not a surface it was asked to draw.
     */
    constexpr VkPipelineDepthStencilStateCreateInfo make_depth_stencil_state(bool const depth_test_enabled, VkCompareOp const depth_compare_op) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .depthTestEnable = depth_test_enabled ? VK_TRUE : VK_FALSE,
                .depthWriteEnable = depth_test_enabled ? VK_TRUE : VK_FALSE,
                .depthCompareOp = depth_compare_op,
                .depthBoundsTestEnable = VK_FALSE,
                .stencilTestEnable = VK_FALSE,
                .front = {},
                .back = {},
                .minDepthBounds = 0.0f,
                .maxDepthBounds = 0.0f};
    }
    /**
     * @brief depth test + write (disabled for background passes such as the skybox, which draw
     *        first and must not occlude later geometry); compare LESS_OR_EQUAL
     * @note DELEGATES to the two-argument form rather than repeating its ten initializers: the operator is
     *       the ONLY thing this overload fixes, and a second copy of the state would be a second place to
     *       keep in step (the failure mode this file's header warns about).
     */
    constexpr VkPipelineDepthStencilStateCreateInfo make_depth_stencil_state(bool const depth_test_enabled) noexcept {
        return make_depth_stencil_state(depth_test_enabled, VK_COMPARE_OP_LESS_OR_EQUAL);
    }
    /**
     * @brief standard alpha blending, ALWAYS enabled: with src alpha == 1 (an opaque material)
     *        the blend math reduces to the source color exactly, so opaque draws are
     *        pixel-identical whether or not blending is on. Blended (transparent) materials
     *        carry alpha < 1 and are drawn depth-write-off in the transparent pass - no second
     *        pipeline needed.
     */
    constexpr VkPipelineColorBlendAttachmentState make_color_blend_attachment() noexcept {
        return {.blendEnable = VK_TRUE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    }
    /**
     * @brief blend state for a target that is OVERWRITTEN instead of blended (a G-buffer
     *        attachment): blending disabled, all four channels written
     * @note a G-buffer target must use this one. On the forward pipelines alpha is coverage, so
     *       "src alpha == 1" makes make_color_blend_attachment() reduce to an overwrite - but in a
     *       G-buffer alpha carries DATA (metallic, roughness, flags), so the same state scales and
     *       then MIXES the stored surface with the cleared target: an untextured/fully-rough
     *       fragment (alpha 0) would erase its own albedo and material id. Measured symptom before
     *       the fix: the albedo target was ~0 everywhere and the material-id target entirely black,
     *       while the normal target (alpha = roughness, rarely 0) survived but was dimmed by it.
     */
    constexpr VkPipelineColorBlendAttachmentState make_color_blend_attachment_opaque() noexcept {
        return {.blendEnable = VK_FALSE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    }
    /**
     * @brief blend state for a target that is ACCUMULATED INTO (src + dst): the G-buffer pass's
     *        emissive attachment, and the deferred lighting pass
     * @note the destination must be load-preserved by the rendering instance (VK_ATTACHMENT_LOAD_OP_LOAD),
     *       which is the point: the emissive and the lighting are added on top of the sky the
     *       background pass already wrote, and a pixel with no geometry contributes exactly 0
     */
    constexpr VkPipelineColorBlendAttachmentState make_color_blend_attachment_additive() noexcept {
        return {.blendEnable = VK_TRUE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    }
    /**
     * @brief blend state for a target that is MULTIPLIED INTO (`dst = src * dst`): the article's two overlay
     *        masks, and nothing else in this renderer
     *
     * WHY IT IS A FACTOR PAIR AND NOT AN EXTENSION, which is the whole reason this late addition to the
     * renderer costs no device feature: the article's shaders are authored in Unity, where `BlendOp Multiply`
     * and `Blend [_BlendSrc=5][_BlendDst=1]` compile to the `KHR_blend_operation_advanced` equation
     * `MULTIPLY` - and an advanced equation IGNORES the source and destination factors. The same product is
     * reachable in core Vulkan by naming the FACTORS instead of the equation: with `blendOp = ADD`,
     * `srcColorBlendFactor = DST_COLOR` and `dstColorBlendFactor = ZERO` the blend reduces to
     * `src * dst + dst * 0`, i.e. exactly `src * dst`. So the port needs no extension, no capability query and
     * no fallback path - and the chapter's `MyZmdEyeDarkShader` / `MyZmdHairShadowShader` are reproduced by
     * the blend state rather than approximated by it.
     *
     * THE ALPHA CHANNEL IS DELIBERATELY UNTOUCHED (`ZERO`/`ONE`, an ADD of nothing): the article's fragment
     * stages also compute an output alpha, and under an advanced blend their alpha would multiply the target's
     * too - but the target here is the HDR colour image, whose alpha carries no coverage and is read by nobody.
     * Writing `dst` back is the choice that keeps this state from silently attenuating a channel that no
     * consumer of this pass ever asked about, and it is stated here rather than left to a default because it is
     * the one channel where "the article does something and we do not" is a deliberate difference.
     *
     * @note the destination must be LOAD-preserved by the rendering instance, which is the point: the multiply
     *       reads what the pass before it wrote, and a cleared target would turn the overlay into a black
     *       rectangle instead of a shadow
     */
    constexpr VkPipelineColorBlendAttachmentState make_color_blend_attachment_multiply() noexcept {
        return {.blendEnable = VK_TRUE,
                .srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR,
                .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                .colorBlendOp = VK_BLEND_OP_ADD,
                .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                .alphaBlendOp = VK_BLEND_OP_ADD,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    }
    /**
     * @brief blend state for one attachment; depth-only pipelines have no color attachment
     * @param attachment caller-owned blend attachment (ignored when has_color_attachment false)
     */
    constexpr VkPipelineColorBlendStateCreateInfo make_color_blend_state(bool const has_color_attachment, VkPipelineColorBlendAttachmentState const* attachment) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .logicOpEnable = VK_FALSE,
                .logicOp = VK_LOGIC_OP_COPY,
                .attachmentCount = has_color_attachment ? 1u : 0u,
                .pAttachments = has_color_attachment ? attachment : nullptr,
                .blendConstants = {1.0f, 1.0f, 1.0f, 1.0f}};
    }
    /**
     * @brief blend state with N attachments, same blend constants
     * @param attachments caller-owned array of @p count blend attachments, in attachment order
     * @param count number of attachments (0 for a depth-only pipeline)
     */
    constexpr VkPipelineColorBlendStateCreateInfo make_color_blend_state(VkPipelineColorBlendAttachmentState const* attachments, uint32_t const count) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .logicOpEnable = VK_FALSE,
                .logicOp = VK_LOGIC_OP_COPY,
                .attachmentCount = count,
                .pAttachments = count > 0 ? attachments : nullptr,
                .blendConstants = {1.0f, 1.0f, 1.0f, 1.0f}};
    }
    /** @brief multisample state (rasterizationSamples is the instance's sample count) */
    constexpr VkPipelineMultisampleStateCreateInfo make_multisample_state(VkSampleCountFlagBits const samples) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .rasterizationSamples = samples,
                .sampleShadingEnable = VK_FALSE,
                .minSampleShading = 0.0f,
                .pSampleMask = nullptr,
                .alphaToCoverageEnable = VK_FALSE,
                .alphaToOneEnable = VK_FALSE};
    }
    /**
     * @brief dynamic-rendering attachment declaration (replaces render pass + subpass)
     * @param color_format_ptr pointer to the caller's color format: the returned info holds
     *        that pointer, so it must outlive the struct
     * @param depth_format the depth attachment format (VK_FORMAT_UNDEFINED for depth-only)
     */
    constexpr VkPipelineRenderingCreateInfo make_rendering_create_info(bool const has_color_attachment, VkFormat const* color_format_ptr, VkFormat const depth_format) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .pNext = nullptr,
                .viewMask = 0,
                .colorAttachmentCount = has_color_attachment ? 1u : 0u,
                .pColorAttachmentFormats = has_color_attachment ? color_format_ptr : nullptr,
                .depthAttachmentFormat = depth_format,
                .stencilAttachmentFormat = VK_FORMAT_UNDEFINED};
    }
    /**
     * @brief dynamic-rendering attachment declaration for an N-target pipeline (the G-buffer
     *        pipeline declares its three targets, in the order its fragment outputs address them)
     * @param color_formats pointer to the caller's format array (retained by the struct, so it
     *        must outlive it - pass the core's static gbuffer_formats)
     * @param color_count number of color attachment formats declared
     * @param depth_format the depth attachment format (VK_FORMAT_UNDEFINED if none)
     */
    constexpr VkPipelineRenderingCreateInfo make_rendering_create_info(VkFormat const* color_formats, uint32_t const color_count, VkFormat const depth_format) noexcept {
        return {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                .pNext = nullptr,
                .viewMask = 0,
                .colorAttachmentCount = color_count,
                .pColorAttachmentFormats = color_count > 0 ? color_formats : nullptr,
                .depthAttachmentFormat = depth_format,
                .stencilAttachmentFormat = VK_FORMAT_UNDEFINED};
    }

    // ---- Per-frame image layout transitions (constinit defaults) ----
    // VK_KHR_unified_image_layouts (REQUIRED, see init_utils) promises that VK_IMAGE_LAYOUT_GENERAL is
    // as efficient as the purpose-built layouts, so this renderer keeps EVERY image in GENERAL and
    // oldLayout == newLayout == GENERAL below. What is left of these barriers is the job they always
    // really had: ordering one stage's write before another stage's read. Only two other layouts
    // survive, and the extension does not replace either:
    //   * oldLayout UNDEFINED - the contents are discarded or were never defined (loadOp CLEAR, a
    //     transient target, a freshly created image), which is how an image's layout metadata is
    //     initialised and the reason no per-frame layout tracking is needed;
    //   * newLayout PRESENT_SRC_KHR - the presentation engine lives outside Vulkan, so present is the
    //     one consumer the extension cannot fold into GENERAL.
    // Every frame runs the same roles; only the target image differs per barrier, so each role below is
    // a default that call sites copy and then override .image on (immutable by design - the invariants
    // must not be retargeted in place). The NAMES keep the role each barrier used to play while the
    // layouts still differed (the far side was COLOR_ATTACHMENT_OPTIMAL / SHADER_READ_ONLY_OPTIMAL /
    // TRANSFER_*_OPTIMAL / ...), because that role is what decides which stages and access masks the
    // barrier has to name.
    /** @brief UNDEFINED -> GENERAL, color-attachment write (a scene target being rendered into) */
    inline constexpr VkImageMemoryBarrier2 color_attachment_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief same-layout dependency (GENERAL -> GENERAL): order one rendering instance's
     *         color-attachment WRITE before the next instance's LOAD of the same image (the scene
     *         color the G-buffer pass fills with the emissive and the deferred lighting stage then
     *         loads to add the lighting on top).
     * @note dynamic rendering inserts no dependency of its own between two instances, and no layout
     *       changes here (none ever does any more), so this barrier exists purely for the write -> read
     *       visibility: without it the second instance's loadOp is not ordered after the first
     *       instance's storeOp - LOAD is a color-attachment access, not a fragment-shader read,
     *       which is why the sampling transitions of the same image cannot stand in for it. */
    inline constexpr VkImageMemoryBarrier2 color_attachment_dependency = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> GENERAL, depth write (main depth buffer + the shadow map) */
    inline constexpr VkImageMemoryBarrier2 depth_attachment_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    /** @brief depth attachment -> GENERAL, sampled read (the shadow map back to the
     *         main pass, and the G-buffer depth to everything that reconstructs from it).
     * @note BOTH consumer stages are named: the G-buffer images have had a COMPUTE consumer since the
     *       screen-space GI passes started reading the stored surface directly (shaders/megalights_trace.slang
     *       and megalights_temporal.comp name the gbuffer slots from a compute stage), and a layout transition
     *       has to name every stage that reads the image afterwards. */
    inline constexpr VkImageMemoryBarrier2 shadow_map_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    /** @brief same-layout dependency (GENERAL -> GENERAL), depth test without depth
     *         write (the G-buffer depth handed back to an attachment for the deferred path's
     *         transparent pass, which depth-tests against the surface the lighting stage just
     *         sampled it for) */
    inline constexpr VkImageMemoryBarrier2 sampling_to_depth_attachment_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        // READ and not WRITE: every transparent leaf draws with depth writes disabled (see
        // primitive::draw), so nothing in that instance writes the depth it tests against
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> GENERAL, compute storage-image write (the half-res GI image, which is
     *         written as a storage image rather than rendered into, so it lives in GENERAL) */
    inline constexpr VkImageMemoryBarrier2 undefined_to_general_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief compute write -> sampled read (GENERAL -> GENERAL): the GI images handed to samplers by a compute
     *         SHADER_WRITE is not visible to a later read without this).
     * @note BOTH consumer stages are named, because the two GI images are handed to different ones:
     *       the raw trace goes to the denoiser's resolve, which is another COMPUTE dispatch, while the
     *       resolved image goes to the FRAGMENT composite. Naming only FRAGMENT would leave the
     *       trace -> resolve hand-off unordered - the resolve could sample a half-written trace. */
    inline constexpr VkImageMemoryBarrier2 general_to_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief sampled read -> compute write (GENERAL -> GENERAL, contents kept): an image read as a sample going back to being
     *         written as a compute storage image, KEEPING its contents.
     * @note the opposite of general_to_sampling_transition, and the reason it exists rather than the
     *       write simply claiming UNDEFINED (which is legal and cheaper): a history image is READ across
     *       frames, so a write that discarded its contents would throw away exactly what the next frame's
     *       reader needs.
     * @note the reading stages are named on the src side and COMPUTE on the dst: the previous frame's
     *       resolve is sampled by the tracer and by the spatial filter (COMPUTE), and by the composite
     *       (FRAGMENT). */
    inline constexpr VkImageMemoryBarrier2 sampling_to_general_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief compute write -> compute read+write (GENERAL -> GENERAL): one compute storage-image write
     *         followed by another dispatch that reads it and writes again - a ping-pong's two images.
     * @note a SAME-layout barrier, which is not a no-op: it is the memory dependency between two
     *       dispatches that touch the same image, and consecutive vkCmdDispatch calls in one command
     *       buffer have none. The layout is named anyway, as in every other barrier here.
     * @note COMPUTE on both sides, with SHADER_WRITE on the src and SHADER_READ | SHADER_WRITE on the
     *       dst: the next dispatch both samples the previous one's cells and overwrites them. */
    inline constexpr VkImageMemoryBarrier2 compute_storage_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief compute storage write -> transfer read (GENERAL -> GENERAL): an image written as a storage
     *         image is copied out of - the GI resolve becoming the next frame's history. The
     *         compute write has to be published to the transfer too, which is what the src masks say. */
    inline constexpr VkImageMemoryBarrier2 general_to_transfer_src_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief transfer read -> sampled read (GENERAL -> GENERAL): the same image, handed on to whatever
     *         samples it after the copy (the composite, for the GI resolve).
     * @note BOTH consumer stages are named, like every other transition that hands an image to a
     *       sampler (see shadow_map_sampling_transition). */
    inline constexpr VkImageMemoryBarrier2 transfer_src_to_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief attachment write -> sampled read (GENERAL -> GENERAL): the HDR scene target and the
     *         motion-vector / stored-surface targets into the passes that sample them)
     * @note BOTH consumer stages are named, as in shadow_map_sampling_transition: the motion-vector
     *       target is sampled by the GI denoiser's COMPUTE resolve as well as by the TAA fragment
     *       resolve, and a layout transition has to name every stage that reads the image afterwards. */
    inline constexpr VkImageMemoryBarrier2 hdr_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> GENERAL for a DEPTH image: keep the shadow map's sampled
     *         descriptor valid on frames where the shadow pass does not run (shadows toggled off).
     *         pbr.frag always binds binding 8 and decides at runtime whether to sample it, and a
     *         descriptor must point at an image in the layout it declares - leaving the map in
     *         UNDEFINED made every such frame a VUID. Contents are irrelevant (the shader returns
     *         "fully lit"), so UNDEFINED as the old layout is correct.
     * @warning ONLY valid when the contents really are irrelevant. A depth image this command
     *          buffer just RENDERED and a later pass samples - the G-buffer depth, read by the
     *          deferred lighting stage, the TAA guard and the debug view - must go through
     *          shadow_map_sampling_transition instead: there the old layout is known to be
     *          GENERAL and the src masks publish the attachment write,
     *          so the sampled contents survive. UNDEFINED discards them. */
    inline constexpr VkImageMemoryBarrier2 undefined_to_depth_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> GENERAL: make a transient target readable without
     *         claiming a layout it may not be in (the bloom chain when the passes are skipped - the
     *         composite still samples those bindings statically, so the layout must be valid, but the
     *         contents are multiplied by zero; and a history image's very first use)
     * @note BOTH consumer stages are named, as everywhere else a transition hands an image to a
     *       sampler: the first frame of the GI history is read by a COMPUTE resolve. */
    inline constexpr VkImageMemoryBarrier2 undefined_to_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief sampled read -> transfer write (GENERAL -> GENERAL): a history image is overwritten with
     *         the newly resolved frame after the resolve sampled it
     * @note BOTH reading stages are named on the src side, for the same reason the sampling
     *       transitions name both on the dst side: the last reader of a history image before the copy
     *       is TAA's fragment resolve or the GI denoiser's compute one. */
    inline constexpr VkImageMemoryBarrier2 sampling_to_transfer_dst_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    }; /** @brief UNDEFINED -> GENERAL: the TAA history image receives the resolved frame
        *         through vkCmdCopyImage; its previous contents are irrelevant (the resolve only trusts a
        *         history it marked valid, and this transition is what starts a new one) */
    inline constexpr VkImageMemoryBarrier2 undefined_to_transfer_dst_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief transfer write -> sampled read (GENERAL -> GENERAL): a history image, written by the
     *         history copy at the end of a frame and sampled by the next frame's resolve
     * @note BOTH consumer stages are named (TAA's resolve is a fragment stage, the GI denoiser's is a
     *       compute one), because a layout transition has to name every stage that reads the image
     *       afterwards - see shadow_map_sampling_transition for the same note. */
    inline constexpr VkImageMemoryBarrier2 transfer_dst_to_sampling_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief attachment write -> transfer read (GENERAL -> GENERAL), screenshot read-back copy
     *         (vkCmdCopyImageToBuffer). Recorded INSIDE the frame's own command buffer, while the
     *         swapchain image is still owned by the app: after vkQueuePresentKHR the presentation
     *         engine owns it and transitioning it again violates the WSI rules. */
    inline constexpr VkImageMemoryBarrier2 color_attachment_to_transfer_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief transfer read -> attachment write (GENERAL -> GENERAL), hand the screenshotted image back
     *         to the frame so present_transition (GENERAL -> PRESENT_SRC) still applies */
    inline constexpr VkImageMemoryBarrier2 transfer_to_color_attachment_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief UNDEFINED -> PRESENT_SRC_KHR: present a frame whose post-process pass was skipped, so
     *         the swapchain image is still UNDEFINED (contents are undefined -
     *         this only exists to hand the WSI a validly-laid-out image instead of lying about the
     *         old layout, which is what present_transition assumes) */
    inline constexpr VkImageMemoryBarrier2 undefined_to_present_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    /** @brief GENERAL -> PRESENT_SRC_KHR (dynamic rendering has no finalLayout) */
    inline constexpr VkImageMemoryBarrier2 present_transition = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = VK_NULL_HANDLE,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
} // namespace deren::vulkan
