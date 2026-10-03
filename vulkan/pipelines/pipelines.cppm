// module version: 0.20.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pipelines/pipelines.cppm
 * @defgroup vulkan_pipelines Per-Pass Pipeline Builders
 * @brief The engine's per-pass pipelines: one builder per pass, next to the generic builder in
 *        deren.vulkan.core.pipeline (that one knows HOW to build a pipeline, this one knows what each pass's
 *        pipeline looks like - formats, sample counts, blend state, push-constant ranges).
 *
 * Extracted from deren.vulkan.runtime, whose implementation had grown past 4900 lines. The builders are
 * stateless, and EVERY ONE OF THEM NOW TAKES A `VkDevice` rather than the whole core: a device is what a
 * caller that owns one has (a pass's create step gets exactly that, see vulkan.pass::pass_context), and the
 * two builders that also need the surface's format - the composite and FXAA, which write the swapchain image -
 * are handed it as a parameter. That is the whole of what they used to reach into the core for. Ownership stays
 * with whoever asked for the build (the runtime today, a pass once its three-piece has moved).
 *
 * NO BUILDER TAKES OR MAKES A PIPELINE LAYOUT: every stage is heap-native, so a pipeline is created with
 * `layout = VK_NULL_HANDLE` and the descriptor-heap flag (see deren.vulkan.core.pipeline), and the descriptors a stage
 * reads come from the frame's bound heap rather than from a set. The set-layout plumbing the builders used to
 * thread through is gone with it.
 *
 * The pipeline handles are std::optional because vk_pipeline is an RAII owner with no default
 * constructor, which is also how the runtime holds them.
 */

module;

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vulkan/vulkan.h>

export module deren.vulkan.pipelines;

import deren.vulkan.core;
import deren.vulkan.core.pipeline; // vk_pipeline
import deren.vulkan.render_resource;

namespace deren::vulkan::pipelines {
    /// what build_post() creates: the chain's two composites
    export struct post_owned {
        std::optional<vk_pipeline> composite; // tonemap + bloom sum, writes the swapchain
        std::optional<vk_pipeline> hdr;       // the same pass writing an HDR target instead (FXAA on)
    };

    /// @brief what build_gbuffer_debug() creates: the debug view's pipeline
    export struct gbuffer_owned {
        std::optional<vk_pipeline> debug;
    };

    /// what build_taa() creates: the resolve pipeline
    export struct taa_owned {
        std::optional<vk_pipeline> resolve;
    };

    /// what a compute builder returns: the COMPUTE pipeline
    export struct compute_pipeline_owned {
        std::optional<vk_pipeline> trace;
    };

    export std::expected<post_owned, std::string> build_post(VkDevice device, VkFormat swap_chain_format,
                                                             std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export std::expected<gbuffer_owned, std::string> build_gbuffer_debug(VkDevice device, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export std::expected<taa_owned, std::string> build_taa(VkDevice device, std::span<uint8_t const> vertex_shader_code,
                                                           std::span<uint8_t const> fragment_shader_code);

    /// the ray-traced sun shadow: a compute pipeline over the descriptors the frame's heap carries
    export std::expected<compute_pipeline_owned, std::string> build_two_set_compute(VkDevice device, std::span<uint8_t const> compute_shader_code);
    /// the stochastic punctual lighting trace (shaders/megalights_trace.slang)
    export std::expected<compute_pipeline_owned, std::string> build_megalights_trace(VkDevice device,
                                                                                     std::span<uint8_t const> compute_shader_code);
    /// the stochastic chain's temporal resolve (shaders/megalights_temporal.slang)
    export std::expected<compute_pipeline_owned, std::string> build_megalights_temporal(VkDevice device,
                                                                                        std::span<uint8_t const> compute_shader_code);
    /// the mask bake: a compute pass over the material table and the texture array
    /// array), which collapses the triangles a material's alphaMode MASK cuts out and writes the expanded
    /// vertices a bottom level structure is then built from - see shaders/mask_bake.slang
    export std::expected<compute_pipeline_owned, std::string> build_mask_bake(VkDevice device, std::span<uint8_t const> compute_shader_code);
    /// the compute skinning pass: the scene block's per-joint matrices - see
    /// shaders/compute_skin.slang
    export std::expected<compute_pipeline_owned, std::string> build_compute_skin(VkDevice device, std::span<uint8_t const> compute_shader_code);
    /// the clustered-light sort (shaders/light_cluster.slang): heap-native, and NO push constants
    /// at all - the shader reads the light UBO and writes the two cluster buffers through heap slots, which is
    /// why this builder takes no push size. It is the first compute pipeline in this module
    /// that came out of `deren.vulkan.core`.
    export std::expected<compute_pipeline_owned, std::string> build_cluster(VkDevice device, std::span<uint8_t const> compute_shader_code);

    /**
     * @brief the HEAP-NATIVE probe's pipeline: the first one in this renderer created the heap way
     * @param device the logical device
     * @param compute_shader_code the probe's SPIR-V (see shaders/heap_probe_comp.slang)
     * @return the pipeline, or the reason it could not be created
     * @note NO SET LAYOUT AND NO PIPELINE LAYOUT, which is not a simplification but the flag's requirement:
     *       "the pipeline layout must be NULL and shader resources will be sourced from a descriptor heap". The
     *       probe's parameters therefore reach the shader through vkCmdPushDataEXT (see descriptor_heap::push_data)
     *       and not through vkCmdPushConstants, which needs a layout to push to.
     */
    export std::expected<compute_pipeline_owned, std::string> build_heap_probe(VkDevice device, std::span<uint8_t const> compute_shader_code);

    /// the probe's target: one size for the image, the viewport, the scissor and the readback, so a mismatch
    /// between them is impossible rather than merely unlikely
    export inline constexpr uint32_t heap_probe_extent = 4u;

    /**
     * @brief the GRAPHICS half of the heap-native probe: a heap-flagged, layout-less pipeline over two stages
     * @param device the logical device
     * @param colour_format the format the probe renders into (dynamic rendering, like every pass here)
     * @param vertex_code / @param fragment_code the probe's SPIR-V (see shaders/heap_probe.slang)
     * @return the pipeline, or the reason it could not be created
     * @note no vertex input, no blend and a static viewport: the probe's subject is the FRAGMENT stage reading the
     *       heap through a graphics pipeline at all, and every one of those would be a second thing that could be
     *       wrong. The flag and the null layout are the rule the compute probe established.
     */
    /// @param first_stage the stage that emits the geometry: VERTEX for the original probe, MESH for the
    ///        mesh-shader mechanism proof (docs/mesh_shaders.md step 0). Everything else - the empty vertex
    ///        input, the heap flag, the NULL layout, the fragment stage - is identical between the two.
    export std::expected<vk_pipeline, std::string> build_heap_probe_graphics(VkDevice device, VkFormat colour_format, std::span<uint8_t const> vertex_code, std::span<uint8_t const> fragment_code, VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_VERTEX_BIT);

    /// what build_resolve_pipeline() creates: the resolve pipeline
    export struct resolve_pipeline_owned {
        std::optional<vk_pipeline> resolve;
    };

    export std::expected<resolve_pipeline_owned, std::string> build_resolve_pipeline(VkDevice device,
                                                                                     std::span<uint8_t const> compute_shader_code);

    export std::expected<vk_pipeline, std::string> build_fxaa(VkDevice device, VkFormat swap_chain_format, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    /**
     * @brief the SHADOW pass's depth-only pipeline
     *
     * WHY IT TAKES A DEVICE rather than being `core::make_depth_pipeline`: that method is a member of
     * the `core` object (it reads `this->device`, and the caller's depth format), and
     * a PASS reaches neither - the device and the format arrive through `pass_context`. The three
     * BIAS factors are parameters for the same reason the depth format is: they are the pipeline's, not the
     * device's, and the depth pass is the one pipeline in this renderer created with slope-scaled bias.
     * @param device the logical device
     * @param depth_format the shadow map's depth attachment format
     * @param depth_bias_constant_factor constant depth bias
     * @param depth_bias_slope_factor slope-scaled depth bias
     * @param depth_bias_clamp depth bias clamp, 0 disables clamping
     * @param vertex_shader_code the geometry stage's SPIR-V (the mesh path's own module when it fetches its
     *        own vertices)
     * @param fragment_shader_code the fragment stage's SPIR-V, the same shader either way
     * @param first_stage the stage that emits the geometry: VERTEX for the input-assembler path, MESH for the
     *        one that fetches its own vertices - the FRAGMENT stage is the same shader either way, which is
     *        what makes the two paths comparable (see docs/mesh_shaders.md step 1)
     */
    export std::expected<vk_pipeline, std::string> build_shadow(VkDevice device, VkFormat depth_format, float depth_bias_constant_factor, float depth_bias_slope_factor,
                                                                float depth_bias_clamp, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code,
                                                                VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_VERTEX_BIT);
    /// @brief what the FXAA pass's own create step needs: the anti-aliasing pipeline
    export struct fxaa_owned {
        std::optional<vk_pipeline> antialias;
    };
    export std::expected<fxaa_owned, std::string> build_fxaa_owned(VkDevice device, VkFormat swap_chain_format,
                                                                   std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    /**
     * @brief what the UPSCALE pass's own create step needs: the resolve pipeline
     *
     * FXAA's sibling, and its body is the same pipeline recipe on purpose: both passes read the composite's
     * display-referred LDR image and write the swapchain with `post.vert.spv`'s synthetic triangle and no depth
     * attachment, so the only things that differ are the fragment shader and the viewport the RUNNER sets from
     * each pass's own declaration (FXAA's is the frame's extent, this one's is the swapchain's).
     */
    export struct upscale_owned {
        std::optional<vk_pipeline> resolve;
    };
    export std::expected<upscale_owned, std::string> build_upscale_owned(VkDevice device, VkFormat swap_chain_format,
                                                                         std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export struct deferred_owned {
        std::optional<vk_pipeline> lighting;
    };

    /// the additive blend state comes in as a parameter: the helper that builds it is a local of the
    /// runtime, next to the passes whose blend modes it describes
    export std::expected<deferred_owned, std::string> build_deferred(VkDevice device, std::span<VkPipelineColorBlendAttachmentState const> color_blend, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    // post: the composite chain's owner. The two fullscreen pipelines (one per color format the chain renders
    // into) are created here.
    std::expected<post_owned, std::string> build_post(VkDevice const device, VkFormat const swap_chain_format,
                                                      std::span<uint8_t const> const vertex_shader_code,
                                                      std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        post_owned out;

        // TWO variants, one per color format the chain renders into: the composite writes the
        // swapchain, the bright-pass prefilter and the downsample passes write the R16F bloom levels. A
        // pipeline's rendering color format must match its attachment, so one swapchain-format pipeline
        // was a validation error for the HDR passes.
        auto const make_post_variant = [&](VkFormat const color_format) -> std::expected<vk_pipeline, std::string> {
            auto pipeline_result = deren::vulkan::make_pipeline(
                device, color_format, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
            if (!pipeline_result) {
                return std::unexpected(std::string(pipeline_result.error()));
            }
            return std::move(pipeline_result).value();
        };

        auto composite_pipeline = make_post_variant(swap_chain_format);
        if (!composite_pipeline) {
            return fail(std::move(composite_pipeline.error()));
        }
        out.composite = std::move(composite_pipeline).value();

        auto hdr_pipeline = make_post_variant(deren::vulkan::hdr_format);
        if (!hdr_pipeline) {
            return fail(std::move(hdr_pipeline.error()));
        }
        out.hdr = std::move(hdr_pipeline).value();
        return out;
    }

    std::expected<gbuffer_owned, std::string> build_gbuffer_debug(VkDevice const device,
                                                                  std::span<uint8_t const> const vertex_shader_code,
                                                                  std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        gbuffer_owned out;

        VkFormat const hdr_format_only = deren::vulkan::hdr_format;
        auto pipeline_result = deren::vulkan::make_pipeline(
            device, hdr_format_only, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.debug = std::move(pipeline_result).value();
        return out;
    }
    // taa: the resolve pass' owner. It writes the HDR target, so its rendering color format is hdr_format
    // (a span of one), and its sampler is the odd one out - linear magnification, nearest minification,
    // because the resolve upsamples the scene color but must not average neighbouring history texels.
    std::expected<taa_owned, std::string> build_taa(VkDevice const device, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        taa_owned out;

        std::array<VkFormat, 1> const color_formats = {deren::vulkan::hdr_format};
        auto pipeline_result = deren::vulkan::make_pipeline(
            device, std::span<VkFormat const>(color_formats), VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, 0.0f, 0.0f, 0.0f);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.resolve = std::move(pipeline_result).value();
        return out;
    }

    // The mask bake (see shaders/mask_bake.slang): a compute pipeline over the material heap slots ALONE, because
    // everything it needs is there - the material table for the alpha texture's index and the cutoff, and the
    // bindless texture array to sample it. It owns no set layout, like every traced compute pass, and it is the only compute
    // pass here whose output is not an image: it writes vertices into a buffer the acceleration structure is
    // then built from.
    std::expected<compute_pipeline_owned, std::string> build_mask_bake(VkDevice device, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;

        std::optional<vk_shader_module> const module = make_shader_module(compute_shader_code, device);
        if (!module.has_value()) {
            return fail("mask bake: compute shader module creation failed");
        }
        VkPipelineShaderStageCreateInfo stage_info = {};
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = **module;
        stage_info.pName = "main";

        // THE HEAP FLAG IS NOT OPTIONAL WHEN THE LAYOUT IS NULL: validation's rule is "both or neither", and it
        // says so exactly - "pCreateInfos[0].flags (VkPipelineCreateFlags2(0)) does not include
        // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT while layout is VK_NULL_HANDLE"
        // (VUID-VkComputePipelineCreateInfo-None-11367), measured on the first run of the migrated renderer. The
        // flag is a flags2 bit, past the 32-bit `flags` field, so it reaches a classic create call through
        // VkPipelineCreateFlags2CreateInfo - the shape the graphics path and the two probes already use.
        VkPipelineCreateFlags2CreateInfo const heap_flags = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        VkComputePipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &heap_flags;
        pipeline_info.stage = stage_info;
        pipeline_info.layout = VK_NULL_HANDLE; // heap-native stages: a layout would contradict them (see docs)

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            return fail("mask bake: vkCreateComputePipelines failed");
        }
        out.trace = vk_pipeline(pipeline, device);
        return out;
    }

    // The compute skinning pass (see shaders/compute_skin.slang): the same shape as the mask bake above and
    // for the same reason - it reads only the per-joint matrices heap slot.
    std::expected<compute_pipeline_owned, std::string> build_compute_skin(VkDevice device, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;

        std::optional<vk_shader_module> const module = make_shader_module(compute_shader_code, device);
        if (!module.has_value()) {
            return fail("compute skin: compute shader module creation failed");
        }
        VkPipelineShaderStageCreateInfo stage_info = {};
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = **module;
        stage_info.pName = "main";

        // THE HEAP FLAG IS NOT OPTIONAL WHEN THE LAYOUT IS NULL: validation's rule is "both or neither", and it
        // says so exactly - "pCreateInfos[0].flags (VkPipelineCreateFlags2(0)) does not include
        // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT while layout is VK_NULL_HANDLE"
        // (VUID-VkComputePipelineCreateInfo-None-11367), measured on the first run of the migrated renderer. The
        // flag is a flags2 bit, past the 32-bit `flags` field, so it reaches a classic create call through
        // VkPipelineCreateFlags2CreateInfo - the shape the graphics path and the two probes already use.
        VkPipelineCreateFlags2CreateInfo const heap_flags = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        VkComputePipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &heap_flags;
        pipeline_info.stage = stage_info;
        pipeline_info.layout = VK_NULL_HANDLE; // heap-native stages: a layout would contradict them (see docs)

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            return fail("compute skin: vkCreateComputePipelines failed");
        }
        out.trace = vk_pipeline(pipeline, device);
        return out;
    }

    // The clustered-light sort: NO set, NO layout and NO push range. Every stage of the frame is heap-native
    // (see docs/descriptor_heap_handover.md), so this pipeline is created with VK_NULL_HANDLE and the heap flag;
    // the shader reads the light UBO and the cluster buffers out of the scene block by slot, and the slot itself
    // travels in the stage push block (shaders/heap_slots.glsl). Owning the pipeline is all that is left to own.
    std::expected<compute_pipeline_owned, std::string> build_cluster(VkDevice const device, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;

        std::optional<vk_shader_module> const module = make_shader_module(compute_shader_code, device);
        if (!module.has_value()) {
            return fail("cluster: compute shader module creation failed");
        }
        VkPipelineShaderStageCreateInfo stage_info = {};
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = **module;
        stage_info.pName = "main";

        // THE HEAP FLAG IS WHAT MAKES A NULL LAYOUT LEGAL, and it is set unconditionally: this stage is
        // heap-native, so its layout is VK_NULL_HANDLE and the flag is what validation demands for that.
        VkPipelineCreateFlags2CreateInfo pipeline_flags = {};
        pipeline_flags.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
        pipeline_flags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

        VkComputePipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &pipeline_flags; // the heap flag rides in flags2 (its bit is past the 32-bit `flags` field)
        pipeline_info.stage = stage_info;
        pipeline_info.layout = VK_NULL_HANDLE; // heap-native: a layout would contradict the flag (VUID ...-11367)

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            return fail("cluster: vkCreateComputePipelines failed");
        }
        out.trace = vk_pipeline(pipeline, device);
        return out;
    }

    std::expected<compute_pipeline_owned, std::string> build_heap_probe(VkDevice const device, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;

        std::optional<vk_shader_module> const module = make_shader_module(compute_shader_code, device);
        if (!module.has_value()) {
            return fail("heap probe: compute shader module creation failed");
        }
        VkPipelineShaderStageCreateInfo stage_info = {};
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = **module;
        stage_info.pName = "main";

        // The heap flag is a flags2 bit (0x1000000000, past the 32-bit `flags` field), so it arrives through
        // VkPipelineCreateFlags2CreateInfo - and it REQUIRES layout = VK_NULL_HANDLE, which is the whole point:
        // with the flag set the pipeline layout is not read at all, and the shader's resources come from the heap.
        VkPipelineCreateFlags2CreateInfo pipeline_flags = {};
        pipeline_flags.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
        pipeline_flags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

        VkComputePipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &pipeline_flags;
        pipeline_info.stage = stage_info;
        pipeline_info.layout = VK_NULL_HANDLE;

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            return fail("heap probe: vkCreateComputePipelines failed");
        }
        out.trace = vk_pipeline(pipeline, device);
        return out;
    }

    // The SHARED builder every traced pass uses (its name still says `two_set`, from the two sets it used to
    // declare): the shape several traced passes have in common - the camera block and the light UBO in the scene
    // block (plus the top level structure at binding 16 when the device has ray tracing), the stored surface in
    // the G-buffer images - so the caller's only variable is the push block size.
    // (Its old name, build_rt_shadow, is gone with the ray-query shadow pass: the shadow traces through a real
    // ray-tracing PIPELINE now, which is a different builder below.)
    std::expected<vk_pipeline, std::string> build_heap_probe_graphics(VkDevice const device, VkFormat const colour_format, std::span<uint8_t const> const vertex_code, std::span<uint8_t const> const fragment_code, VkShaderStageFlagBits const first_stage) {
        using fail = std::unexpected<std::string>;
        auto const vertex_module = make_shader_module(vertex_code, device);
        if (!vertex_module.has_value()) {
            return fail("heap probe (graphics): the first shader module's creation failed");
        }
        auto const fragment_module = make_shader_module(fragment_code, device);
        if (!fragment_module.has_value()) {
            return fail("heap probe (graphics): fragment shader module creation failed");
        }
        std::array<VkPipelineShaderStageCreateInfo, 2> stages = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        // THE FIRST STAGE IS A PARAMETER, and that is the whole difference between the vertex probe and the
        // MESH probe (docs/mesh_shaders.md step 0): a mesh pipeline substitutes VK_SHADER_STAGE_MESH_BIT_EXT
        // here, keeps the same fragment stage, and ignores the (empty) vertex input state - so the same 4x4
        // target and the same readback compare the two paths directly.
        stages[0].stage = first_stage;
        stages[0].module = **vertex_module;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = **fragment_module;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo const vertex_input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                                   .pNext = nullptr,
                                                                   .flags = 0,
                                                                   .vertexBindingDescriptionCount = 0,
                                                                   .pVertexBindingDescriptions = nullptr,
                                                                   .vertexAttributeDescriptionCount = 0,
                                                                   .pVertexAttributeDescriptions = nullptr};
        VkPipelineInputAssemblyStateCreateInfo const input_assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                                       .pNext = nullptr,
                                                                       .flags = 0,
                                                                       .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                                       .primitiveRestartEnable = VK_FALSE};
        VkViewport const viewport = {.x = 0.0f, .y = 0.0f, .width = static_cast<float>(heap_probe_extent), .height = static_cast<float>(heap_probe_extent), .minDepth = 0.0f, .maxDepth = 1.0f};
        VkRect2D const scissor = {.offset = {0, 0}, .extent = {heap_probe_extent, heap_probe_extent}};
        VkPipelineViewportStateCreateInfo const viewport_state = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                                  .pNext = nullptr,
                                                                  .flags = 0,
                                                                  .viewportCount = 1,
                                                                  .pViewports = &viewport,
                                                                  .scissorCount = 1,
                                                                  .pScissors = &scissor};
        VkPipelineRasterizationStateCreateInfo const rasterization = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                                      .pNext = nullptr,
                                                                      .flags = 0,
                                                                      .depthClampEnable = VK_FALSE,
                                                                      .rasterizerDiscardEnable = VK_FALSE,
                                                                      .polygonMode = VK_POLYGON_MODE_FILL,
                                                                      .cullMode = VK_CULL_MODE_NONE,
                                                                      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                                      .depthBiasEnable = VK_FALSE,
                                                                      .depthBiasConstantFactor = 0.0f,
                                                                      .depthBiasClamp = 0.0f,
                                                                      .depthBiasSlopeFactor = 0.0f,
                                                                      .lineWidth = 1.0f};
        VkPipelineMultisampleStateCreateInfo const multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                                  .pNext = nullptr,
                                                                  .flags = 0,
                                                                  .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                                                                  .sampleShadingEnable = VK_FALSE,
                                                                  .minSampleShading = 1.0f,
                                                                  .pSampleMask = nullptr,
                                                                  .alphaToCoverageEnable = VK_FALSE,
                                                                  .alphaToOneEnable = VK_FALSE};
        VkPipelineColorBlendAttachmentState const blend_attachment = {.blendEnable = VK_FALSE,
                                                                      .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
                                                                      .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                                      .colorBlendOp = VK_BLEND_OP_ADD,
                                                                      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                                                      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
                                                                      .alphaBlendOp = VK_BLEND_OP_ADD,
                                                                      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
        VkPipelineColorBlendStateCreateInfo const colour_blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                                  .pNext = nullptr,
                                                                  .flags = 0,
                                                                  .logicOpEnable = VK_FALSE,
                                                                  .logicOp = VK_LOGIC_OP_COPY,
                                                                  .attachmentCount = 1,
                                                                  .pAttachments = &blend_attachment,
                                                                  .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f}};
        VkPipelineRenderingCreateInfo rendering = {};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colour_format;
        // The heap flag is a flags2 bit and the rendering struct hangs off it, so both travel in one pNext chain.
        VkPipelineCreateFlags2CreateInfo flags = {};
        flags.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO;
        flags.pNext = &rendering;
        flags.flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

        VkGraphicsPipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &flags;
        pipeline_info.stageCount = static_cast<uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pColorBlendState = &colour_blend;
        pipeline_info.layout = VK_NULL_HANDLE; // required by the flag, exactly as for the compute probe

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            return fail("heap probe (graphics): vkCreateGraphicsPipelines failed");
        }
        return vk_pipeline(pipeline, device);
    }

    std::expected<compute_pipeline_owned, std::string> build_two_set_compute(VkDevice device, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        compute_pipeline_owned out;

        std::optional<vk_shader_module> const module = make_shader_module(compute_shader_code, device);
        if (!module.has_value()) {
            return fail("rt shadow: compute shader module creation failed");
        }
        VkPipelineShaderStageCreateInfo stage_info = {};
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = **module;
        stage_info.pName = "main";

        // THE HEAP FLAG IS NOT OPTIONAL WHEN THE LAYOUT IS NULL: validation's rule is "both or neither", and it
        // says so exactly - "pCreateInfos[0].flags (VkPipelineCreateFlags2(0)) does not include
        // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT while layout is VK_NULL_HANDLE"
        // (VUID-VkComputePipelineCreateInfo-None-11367), measured on the first run of the migrated renderer. The
        // flag is a flags2 bit, past the 32-bit `flags` field, so it reaches a classic create call through
        // VkPipelineCreateFlags2CreateInfo - the shape the graphics path and the two probes already use.
        VkPipelineCreateFlags2CreateInfo const heap_flags = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        VkComputePipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &heap_flags;
        pipeline_info.stage = stage_info;
        pipeline_info.layout = VK_NULL_HANDLE; // heap-native stages: a layout would contradict them (see docs)

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            return fail("rt shadow: vkCreateComputePipelines failed");
        }
        out.trace = vk_pipeline(pipeline, device);
        return out;
    }
    /**
     * @brief what the ray-tracing shadow pipeline builder returns
     *
     * `group_count` is the number of shader groups the pipeline created, in the order the SBT must follow
     * (raygen, miss, hit): the shader binding table itself is the CALLER's, because the group handles it is
     * filled with are per-pipeline data and its regions have to outlive this call.
     */
    export struct ray_tracing_pipeline_owned {
        std::optional<vk_pipeline> pipeline;
        uint32_t group_count = 0;
    };

    /**
     * @brief the ray-traced sun shadow's PIPELINE: one raygen, one miss and one triangles hit group
     *
     * WHY A PIPELINE AND NOT AN INLINE RAY QUERY, which is what this pass used to run: a ray query has no
     * any-hit stage, so an alphaMode MASK surface is SOLID to the ray. A traced ray can run an any-hit shader
     * for exactly that test, and it can consult an opacity micromap, which is the hardware form of the same
     * question (per-microtriangle opacity, with the any-hit shader as the fallback for its 'unknown' states).
     * Both of those live in the hit group this creates.
     *
     * Recursion depth is 1: the shadow ray answers a yes/no question and the traversal terminates on the first
     * hit (`gl_RayFlagsTerminateOnFirstHitEXT` in the raygen), so there is nothing for a second level to do.
     */
    export std::expected<ray_tracing_pipeline_owned, std::string> build_rt_shadow_ray_tracing(VkDevice device,
                                                                                              std::span<uint8_t const> raygen_code,
                                                                                              std::span<uint8_t const> closest_hit_code, std::span<uint8_t const> miss_code,
                                                                                              std::span<uint8_t const> any_hit_code) {
        using fail = std::unexpected<std::string>;
        ray_tracing_pipeline_owned out;

        std::optional<vk_shader_module> const raygen = make_shader_module(raygen_code, device);
        std::optional<vk_shader_module> const closest_hit = make_shader_module(closest_hit_code, device);
        std::optional<vk_shader_module> const miss = make_shader_module(miss_code, device);
        std::optional<vk_shader_module> const any_hit = make_shader_module(any_hit_code, device);
        if (!raygen.has_value() || !closest_hit.has_value() || !miss.has_value() || !any_hit.has_value()) {
            return fail("rt shadow: shader module creation failed");
        }

        VkPipelineShaderStageCreateInfo const raygen_stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                              .pNext = nullptr,
                                                              .flags = 0,
                                                              .stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR,
                                                              .module = **raygen,
                                                              .pName = "main",
                                                              .pSpecializationInfo = nullptr};
        VkPipelineShaderStageCreateInfo const closest_hit_stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                                   .pNext = nullptr,
                                                                   .flags = 0,
                                                                   .stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
                                                                   .module = **closest_hit,
                                                                   .pName = "main",
                                                                   .pSpecializationInfo = nullptr};
        VkPipelineShaderStageCreateInfo const miss_stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                            .pNext = nullptr,
                                                            .flags = 0,
                                                            .stage = VK_SHADER_STAGE_MISS_BIT_KHR,
                                                            .module = **miss,
                                                            .pName = "main",
                                                            .pSpecializationInfo = nullptr};
        VkPipelineShaderStageCreateInfo const any_hit_stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                               .pNext = nullptr,
                                                               .flags = 0,
                                                               .stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
                                                               .module = **any_hit,
                                                               .pName = "main",
                                                               .pSpecializationInfo = nullptr};
        // FOUR STAGES, THREE GROUPS: the any-hit shader sits at index 3 and is named by the hit group below rather
        // than becoming a group of its own, which is what keeps the caller's shader binding table regions and
        // their addressing unchanged by this step.
        std::array<VkPipelineShaderStageCreateInfo, 4> const stages = {raygen_stage, miss_stage, closest_hit_stage, any_hit_stage};

        // THE GROUP ORDER IS THE SBT'S ORDER: group 0 is the raygen, group 1 the miss shader, group 2 the hit
        // group. The caller's regions follow exactly this order, which is why the count is returned with them.
        // The hit group names BOTH of its stages: the closest-hit shader answers the ray and the any-hit shader
        // is the one that may refuse the intersection first, which is the whole reason this pipeline exists.
        VkRayTracingShaderGroupCreateInfoKHR const raygen_group = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                                                   .pNext = nullptr,
                                                                   .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                                                                   .generalShader = 0,
                                                                   .closestHitShader = VK_SHADER_UNUSED_KHR,
                                                                   .anyHitShader = VK_SHADER_UNUSED_KHR,
                                                                   .intersectionShader = VK_SHADER_UNUSED_KHR,
                                                                   .pShaderGroupCaptureReplayHandle = nullptr};
        VkRayTracingShaderGroupCreateInfoKHR const miss_group = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                                                 .pNext = nullptr,
                                                                 .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR,
                                                                 .generalShader = 1,
                                                                 .closestHitShader = VK_SHADER_UNUSED_KHR,
                                                                 .anyHitShader = VK_SHADER_UNUSED_KHR,
                                                                 .intersectionShader = VK_SHADER_UNUSED_KHR,
                                                                 .pShaderGroupCaptureReplayHandle = nullptr};
        VkRayTracingShaderGroupCreateInfoKHR const hit_group = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                                                .pNext = nullptr,
                                                                .type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR,
                                                                .generalShader = VK_SHADER_UNUSED_KHR,
                                                                .closestHitShader = 2,
                                                                .anyHitShader = 3, // the ANY-HIT stage of this same group: see shaders/rt_shadow.rahit
                                                                .intersectionShader = VK_SHADER_UNUSED_KHR,
                                                                .pShaderGroupCaptureReplayHandle = nullptr};
        std::array<VkRayTracingShaderGroupCreateInfoKHR, 3> const groups = {raygen_group, miss_group, hit_group};

        VkPipelineCreateFlags2CreateInfo const rt_heap_flags = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        VkRayTracingPipelineCreateInfoKHR const pipeline_info = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
                                                                 .pNext = &rt_heap_flags,
                                                                 .flags = 0,
                                                                 .stageCount = static_cast<uint32_t>(stages.size()),
                                                                 .pStages = stages.data(),
                                                                 .groupCount = static_cast<uint32_t>(groups.size()),
                                                                 .pGroups = groups.data(),
                                                                 .maxPipelineRayRecursionDepth = 1,
                                                                 .pLibraryInfo = nullptr,
                                                                 .pLibraryInterface = nullptr,
                                                                 .pDynamicState = nullptr,
                                                                 .layout = VK_NULL_HANDLE, // heap-native stages: a layout would contradict them
                                                                 .basePipelineHandle = VK_NULL_HANDLE,
                                                                 .basePipelineIndex = -1};
        // THE EXTENSION ENTRY POINT COMES FROM THE DEVICE, not from the link line: `vulkan-1`'s import library
        // does not export an extension command (the acceleration-structure module loads its five the same way),
        // so a direct call is an undefined symbol at link time rather than a missing feature at runtime.
        auto const create_ray_tracing = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkGetDeviceProcAddr(device, "vkCreateRayTracingPipelinesKHR"));
        if (create_ray_tracing == nullptr) {
            return fail("rt shadow: the device did not publish vkCreateRayTracingPipelinesKHR");
        }
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (create_ray_tracing(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            return fail("rt shadow: vkCreateRayTracingPipelinesKHR failed");
        }
        out.pipeline = vk_pipeline(pipeline, device);
        out.group_count = static_cast<uint32_t>(groups.size());
        return out;
    }

    // The stochastic punctual lighting trace (shaders/megalights_trace.slang): the same compute-pipeline shape as
    // the passes above, with a push block of its own. It FORWARDS to the builder above rather than repeating
    // twenty lines of Vulkan, and it exists as its own name because a caller reading `build_two_set_compute`
    // inside this pass's create() would have to check that the two are still the same shape - which is exactly
    // the kind of coupling a name is for.
    std::expected<compute_pipeline_owned, std::string> build_megalights_trace(VkDevice device,
                                                                              std::span<uint8_t const> const compute_shader_code) {
        return build_two_set_compute(device, compute_shader_code);
    }
    // ... and the chain's temporal resolve: the same shape, with the accumulation's own push block.
    std::expected<compute_pipeline_owned, std::string> build_megalights_temporal(VkDevice device,
                                                                                 std::span<uint8_t const> const compute_shader_code) {
        return build_two_set_compute(device, compute_shader_code);
    }

    // The temporal resolve: its own pipeline, over the images the frame's heap carries. It reads no scene
    // buffer: the push block carries the two projection terms its depth guard needs.
    std::expected<resolve_pipeline_owned, std::string> build_resolve_pipeline(VkDevice const device, std::span<uint8_t const> const compute_shader_code) {
        using fail = std::unexpected<std::string>;
        resolve_pipeline_owned out;

        std::optional<vk_shader_module> const module = make_shader_module(compute_shader_code, device);
        if (!module.has_value()) {
            return fail("temporal resolve: compute shader module creation failed");
        }
        VkPipelineShaderStageCreateInfo stage_info = {};
        stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = **module;
        stage_info.pName = "main";

        // THE HEAP FLAG IS NOT OPTIONAL WHEN THE LAYOUT IS NULL: validation's rule is "both or neither", and it
        // says so exactly - "pCreateInfos[0].flags (VkPipelineCreateFlags2(0)) does not include
        // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT while layout is VK_NULL_HANDLE"
        // (VUID-VkComputePipelineCreateInfo-None-11367), measured on the first run of the migrated renderer. The
        // flag is a flags2 bit, past the 32-bit `flags` field, so it reaches a classic create call through
        // VkPipelineCreateFlags2CreateInfo - the shape the graphics path and the two probes already use.
        VkPipelineCreateFlags2CreateInfo const heap_flags = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        VkComputePipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.pNext = &heap_flags;
        pipeline_info.stage = stage_info;
        pipeline_info.layout = VK_NULL_HANDLE; // heap-native stages: a layout would contradict them (see docs)

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) != VK_SUCCESS) {
            return fail("temporal resolve: vkCreateComputePipelines failed");
        }
        out.resolve = vk_pipeline(pipeline, device);
        return out;
    }

    std::expected<deferred_owned, std::string> build_deferred(VkDevice device, std::span<VkPipelineColorBlendAttachmentState const> const color_blend, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        deferred_owned out;
        std::array<VkFormat, 1> const color_formats = {deren::vulkan::hdr_format};
        auto pipeline_result = deren::vulkan::make_pipeline(
            device, std::span<VkFormat const>(color_formats), VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, 0.0f, 0.0f, 0.0f, color_blend);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.lighting = std::move(pipeline_result).value();
        return out;
    }

    std::expected<vk_pipeline, std::string> build_fxaa(VkDevice device, VkFormat const swap_chain_format, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        auto pipeline_result = deren::vulkan::make_pipeline(
            device, swap_chain_format, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        return std::move(pipeline_result).value();
    }

    std::expected<vk_pipeline, std::string> build_shadow(VkDevice const device, VkFormat const depth_format, float const depth_bias_constant_factor,
                                                         float const depth_bias_slope_factor, float const depth_bias_clamp, std::span<uint8_t const> const vertex_shader_code,
                                                         std::span<uint8_t const> const fragment_shader_code,
                                                         // THE STAGE THAT EMITS THE GEOMETRY: VERTEX for the input-assembler path, MESH
                                                         // for the one that fetches its own vertices (docs/mesh_shaders.md step 1 - the
                                                         // fragment stage is the SAME shader either way, which is what makes the two
                                                         // paths comparable at all).
                                                         VkShaderStageFlagBits first_stage) {
        using fail = std::unexpected<std::string>;
        // No color attachment, depth test + write, single-sampled, and the slope-scaled bias the shadow pass needs
        // (it removes acne on surfaces angled away from the light, in units of depth per depth-unit of slope - the
        // numbers are the pass's and the caller's, not this builder's).
        auto result = deren::vulkan::make_pipeline(device,
                                                   VK_FORMAT_UNDEFINED,
                                                   depth_format,
                                                   vertex_shader_code,
                                                   fragment_shader_code,
                                                   VK_SAMPLE_COUNT_1_BIT,
                                                   true,  // depth test + write
                                                   false, // no color attachment
                                                   depth_bias_constant_factor,
                                                   depth_bias_slope_factor,
                                                   depth_bias_clamp,
                                                   first_stage);
        if (!result) {
            return fail(std::string(result.error()));
        }
        return std::move(result).value();
    }
    std::expected<fxaa_owned, std::string> build_fxaa_owned(VkDevice const device, VkFormat const swap_chain_format,
                                                            std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        fxaa_owned out;

        // The anti-aliasing pipeline renders into the SWAPCHAIN, so its declared colour format is the surface's.
        auto pipeline_result = deren::vulkan::make_pipeline(
            device, swap_chain_format, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.antialias = std::move(pipeline_result).value();
        return out;
    }

    std::expected<upscale_owned, std::string> build_upscale_owned(VkDevice const device, VkFormat const swap_chain_format,
                                                                  std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        upscale_owned out;

        // The resolve pipeline renders into the SWAPCHAIN, so its declared colour format is the surface's - and
        // that format is also what decides the shader's `encode_gamma` lane (see upscale_pass::record). The
        // single-target convenience form's blend state is the engine's standard src-alpha one, which the
        // fragment shader's alpha of 1.0 reduces to a copy: the same arrangement the composite's and FXAA's
        // swapchain writes already have, and the reason no blend state is spelled out here.
        auto pipeline_result = deren::vulkan::make_pipeline(
            device, swap_chain_format, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.resolve = std::move(pipeline_result).value();
        return out;
    }
} // namespace deren::vulkan::pipelines