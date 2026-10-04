// module version: 0.20.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pipelines/pipelines.cppm
 * @defgroup vulkan_pipelines Per-Pass Pipeline Builders
 * @brief The engine's per-pass pipelines: one builder per pass, next to the generic builder in
 *        deren.vulkan.engine_gpu (that one knows HOW to build a pipeline, this one knows what each pass's
 *        pipeline looks like - formats, sample counts, blend state, push-constant ranges).
 *
 * Extracted from deren.vulkan.runtime, whose implementation had grown past 4900 lines. The builders are
 * stateless and take the host gpu_context: the context token keeps the backend alive and factories create
 * resources inside the DLL. A pass receives this context through vulkan.pass::pass_context, and the
 * two builders that also need the surface's format - the composite and FXAA, which write the swapchain image -
 * are handed it as a parameter. That is the whole of what they used to reach into the core for. Ownership stays
 * with whoever asked for the build (the runtime today, a pass once its three-piece has moved).
 *
 * NO BUILDER TAKES OR MAKES A PIPELINE LAYOUT: every stage is heap-native, so a pipeline is created with
 * `layout = VK_NULL_HANDLE` and the descriptor-heap flag (see deren.vulkan.engine_gpu), and the descriptors a stage
 * reads come from the frame's bound heap rather than from a set. The set-layout plumbing the builders used to
 * thread through is gone with it.
 *
 * Optional pipeline owners describe an unbuilt pass; their resource token keeps the core alive until release.
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

import deren.vulkan.engine_gpu; // vk_pipeline
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

    export std::expected<post_owned, std::string> build_post(gpu_context const& gpu, VkFormat swap_chain_format,
                                                             std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export std::expected<gbuffer_owned, std::string> build_gbuffer_debug(gpu_context const& gpu, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export std::expected<taa_owned, std::string> build_taa(gpu_context const& gpu, std::span<uint8_t const> vertex_shader_code,
                                                           std::span<uint8_t const> fragment_shader_code);

    /// the ray-traced sun shadow: a compute pipeline over the descriptors the frame's heap carries
    export std::expected<compute_pipeline_owned, std::string> build_two_set_compute(gpu_context const& gpu, std::span<uint8_t const> compute_shader_code);
    /// the stochastic punctual lighting trace (shaders/megalights_trace.slang)
    export std::expected<compute_pipeline_owned, std::string> build_megalights_trace(gpu_context const& gpu,
                                                                                     std::span<uint8_t const> compute_shader_code);
    /// the stochastic chain's temporal resolve (shaders/megalights_temporal.slang)
    export std::expected<compute_pipeline_owned, std::string> build_megalights_temporal(gpu_context const& gpu,
                                                                                        std::span<uint8_t const> compute_shader_code);
    /// the mask bake: a compute pass over the material table and the texture array
    /// array), which collapses the triangles a material's alphaMode MASK cuts out and writes the expanded
    /// vertices a bottom level structure is then built from - see shaders/mask_bake.slang
    export std::expected<compute_pipeline_owned, std::string> build_mask_bake(gpu_context const& gpu, std::span<uint8_t const> compute_shader_code);
    /// the compute skinning pass: the scene block's per-joint matrices - see
    /// shaders/compute_skin.slang
    export std::expected<compute_pipeline_owned, std::string> build_compute_skin(gpu_context const& gpu, std::span<uint8_t const> compute_shader_code);
    /// the clustered-light sort (shaders/light_cluster.slang): heap-native, and NO push constants
    /// at all - the shader reads the light UBO and writes the two cluster buffers through heap slots, which is
    /// why this builder takes no push size. It is the first compute pipeline in this module
    /// that came out of `deren.vulkan.core`.
    export std::expected<compute_pipeline_owned, std::string> build_cluster(gpu_context const& gpu, std::span<uint8_t const> compute_shader_code);

    /**
     * @brief the HEAP-NATIVE probe's pipeline: the first one in this renderer created the heap way
     * @param gpu the logical gpu
     * @param compute_shader_code the probe's SPIR-V (see shaders/heap_probe_comp.slang)
     * @return the pipeline, or the reason it could not be created
     * @note NO SET LAYOUT AND NO PIPELINE LAYOUT, which is not a simplification but the flag's requirement:
     *       "the pipeline layout must be NULL and shader resources will be sourced from a descriptor heap". The
     *       probe's parameters therefore reach the shader through vkCmdPushDataEXT (see descriptor_heap::push_data)
     *       and not through vkCmdPushConstants, which needs a layout to push to.
     */
    export std::expected<compute_pipeline_owned, std::string> build_heap_probe(gpu_context const& gpu, std::span<uint8_t const> compute_shader_code);

    /// the probe's target: one size for the image, the viewport, the scissor and the readback, so a mismatch
    /// between them is impossible rather than merely unlikely
    export inline constexpr uint32_t heap_probe_extent = 4u;

    /**
     * @brief the GRAPHICS half of the heap-native probe: a heap-flagged, layout-less pipeline over two stages
     * @param gpu the logical gpu
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
    export std::expected<vk_pipeline, std::string> build_heap_probe_graphics(gpu_context const& gpu, VkFormat colour_format, std::span<uint8_t const> vertex_code, std::span<uint8_t const> fragment_code, VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_VERTEX_BIT);

    /// what build_resolve_pipeline() creates: the resolve pipeline
    export struct resolve_pipeline_owned {
        std::optional<vk_pipeline> resolve;
    };

    export std::expected<resolve_pipeline_owned, std::string> build_resolve_pipeline(gpu_context const& gpu,
                                                                                     std::span<uint8_t const> compute_shader_code);

    export std::expected<vk_pipeline, std::string> build_fxaa(gpu_context const& gpu, VkFormat swap_chain_format, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    /**
     * @brief the SHADOW pass's depth-only pipeline
     *
     * WHY IT TAKES A DEVICE rather than being `engine_device::make_depth_pipeline`: that method is a member of
     * the `core` object (it reads `this->gpu`, and the caller's depth format), and
     * a PASS reaches neither - the gpu and the format arrive through `pass_context`. The three
     * BIAS factors are parameters for the same reason the depth format is: they are the pipeline's, not the
     * gpu's, and the depth pass is the one pipeline in this renderer created with slope-scaled bias.
     * @param gpu the logical gpu
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
    export std::expected<vk_pipeline, std::string> build_shadow(gpu_context const& gpu, VkFormat depth_format, float depth_bias_constant_factor, float depth_bias_slope_factor,
                                                                float depth_bias_clamp, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code,
                                                                VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_VERTEX_BIT);
    /// @brief what the FXAA pass's own create step needs: the anti-aliasing pipeline
    export struct fxaa_owned {
        std::optional<vk_pipeline> antialias;
    };
    export std::expected<fxaa_owned, std::string> build_fxaa_owned(gpu_context const& gpu, VkFormat swap_chain_format,
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
    export std::expected<upscale_owned, std::string> build_upscale_owned(gpu_context const& gpu, VkFormat swap_chain_format,
                                                                         std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    export struct deferred_owned {
        std::optional<vk_pipeline> lighting;
    };

    /// the additive blend state comes in as a parameter: the helper that builds it is a local of the
    /// runtime, next to the passes whose blend modes it describes
    export std::expected<deferred_owned, std::string> build_deferred(gpu_context const& gpu, std::span<VkPipelineColorBlendAttachmentState const> color_blend, std::span<uint8_t const> vertex_shader_code, std::span<uint8_t const> fragment_shader_code);
    // post: the composite chain's owner. The two fullscreen pipelines (one per color format the chain renders
    // into) are created here.
    std::expected<post_owned, std::string> build_post(gpu_context const& gpu, VkFormat const swap_chain_format,
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
                gpu, color_format, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
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

    std::expected<gbuffer_owned, std::string> build_gbuffer_debug(gpu_context const& gpu,
                                                                  std::span<uint8_t const> const vertex_shader_code,
                                                                  std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        gbuffer_owned out;

        VkFormat const hdr_format_only = deren::vulkan::hdr_format;
        auto pipeline_result = deren::vulkan::make_pipeline(
            gpu, hdr_format_only, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.debug = std::move(pipeline_result).value();
        return out;
    }
    // taa: the resolve pass' owner. It writes the HDR target, so its rendering color format is hdr_format
    // (a span of one), and its sampler is the odd one out - linear magnification, nearest minification,
    // because the resolve upsamples the scene color but must not average neighbouring history texels.
    std::expected<taa_owned, std::string> build_taa(gpu_context const& gpu, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        taa_owned out;

        std::array<VkFormat, 1> const color_formats = {deren::vulkan::hdr_format};
        auto pipeline_result = deren::vulkan::make_pipeline(
            gpu, std::span<VkFormat const>(color_formats), VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, 0.0f, 0.0f, 0.0f);
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
    std::expected<compute_pipeline_owned, std::string> build_mask_bake(gpu_context const& gpu, std::span<uint8_t const> const compute_shader_code) {
        compute_pipeline_owned out;
        out.trace = make_compute_pipeline(gpu, compute_shader_code);
        if (!out.trace)
            return std::unexpected(std::string("mask bake: backend compute pipeline factory refused descriptor"));
        return out;
    }

    // The compute skinning pass (see shaders/compute_skin.slang): the same shape as the mask bake above and
    // for the same reason - it reads only the per-joint matrices heap slot.
    std::expected<compute_pipeline_owned, std::string> build_compute_skin(gpu_context const& gpu, std::span<uint8_t const> const compute_shader_code) {
        compute_pipeline_owned out;
        out.trace = make_compute_pipeline(gpu, compute_shader_code);
        if (!out.trace)
            return std::unexpected(std::string("compute skin: backend compute pipeline factory refused descriptor"));
        return out;
    }

    // The clustered-light sort: NO set, NO layout and NO push range. Every stage of the frame is heap-native
    // (see docs/descriptor_heap_handover.md), so this pipeline is created with VK_NULL_HANDLE and the heap flag;
    // the shader reads the light UBO and the cluster buffers out of the scene block by slot, and the slot itself
    // travels in the stage push block (shaders/heap_slots.glsl). Owning the pipeline is all that is left to own.
    std::expected<compute_pipeline_owned, std::string> build_cluster(gpu_context const& gpu, std::span<uint8_t const> const compute_shader_code) {
        compute_pipeline_owned out;
        out.trace = make_compute_pipeline(gpu, compute_shader_code);
        if (!out.trace)
            return std::unexpected(std::string("cluster: backend compute pipeline factory refused descriptor"));
        return out;
    }

    std::expected<compute_pipeline_owned, std::string> build_heap_probe(gpu_context const& gpu, std::span<uint8_t const> const compute_shader_code) {
        compute_pipeline_owned out;
        out.trace = make_compute_pipeline(gpu, compute_shader_code);
        if (!out.trace)
            return std::unexpected(std::string("heap probe: backend compute pipeline factory refused descriptor"));
        return out;
    }

    // The SHARED builder every traced pass uses (its name still says `two_set`, from the two sets it used to
    // declare): the shape several traced passes have in common - the camera block and the light UBO in the scene
    // block (plus the top level structure at binding 16 when the gpu has ray tracing), the stored surface in
    // the G-buffer images - so the caller's only variable is the push block size.
    // (Its old name, build_rt_shadow, is gone with the ray-query shadow pass: the shadow traces through a real
    // ray-tracing PIPELINE now, which is a different builder below.)
    std::expected<vk_pipeline, std::string> build_heap_probe_graphics(gpu_context const& gpu, VkFormat const colour_format, std::span<uint8_t const> const vertex_code, std::span<uint8_t const> const fragment_code, VkShaderStageFlagBits const first_stage) {
        // 资源创建/释放走后端工厂；4x4 viewport/scissor 仍由此配方提供。
        auto result = make_pipeline(gpu, colour_format, VK_FORMAT_UNDEFINED, vertex_code,
                                    fragment_code, VK_SAMPLE_COUNT_1_BIT, false, true,
                                    0.0f, 0.0f, 0.0f, first_stage);
        if (!result)
            return std::unexpected(std::string(result.error()));
        result->viewport = {.x = 0.0f, .y = 0.0f, .width = static_cast<float>(heap_probe_extent), .height = static_cast<float>(heap_probe_extent), .minDepth = 0.0f, .maxDepth = 1.0f};
        result->scissor = {.offset = {0, 0}, .extent = {heap_probe_extent, heap_probe_extent}};
        return std::move(*result);
    }

    std::expected<compute_pipeline_owned, std::string> build_two_set_compute(gpu_context const& gpu, std::span<uint8_t const> const compute_shader_code) {
        compute_pipeline_owned out;
        out.trace = make_compute_pipeline(gpu, compute_shader_code);
        if (!out.trace)
            return std::unexpected(std::string("traced compute: backend compute pipeline factory refused descriptor"));
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
    export std::expected<ray_tracing_pipeline_owned, std::string> build_rt_shadow_ray_tracing(gpu_context const& gpu,
                                                                                              std::span<uint8_t const> raygen_code,
                                                                                              std::span<uint8_t const> closest_hit_code, std::span<uint8_t const> miss_code,
                                                                                              std::span<uint8_t const> any_hit_code) {
        // stage 顺序与 SBT group 顺序保持原配方：raygen/miss/closest-hit/any-hit。
        std::array<rhi::pipeline_stage, 4> const stages{{{rhi::shader_stage::ray_generation, nullptr, raygen_code, "main"},
                                                         {rhi::shader_stage::ray_miss, nullptr, miss_code, "main"},
                                                         {rhi::shader_stage::ray_closest_hit, nullptr, closest_hit_code, "main"},
                                                         {rhi::shader_stage::ray_any_hit, nullptr, any_hit_code, "main"}}};
        std::array<rhi::ray_shader_group, 3> const groups{{{rhi::ray_group_kind::general, 0, rhi::unused_shader, rhi::unused_shader, rhi::unused_shader},
                                                           {rhi::ray_group_kind::general, 1, rhi::unused_shader, rhi::unused_shader, rhi::unused_shader},
                                                           {rhi::ray_group_kind::triangles, rhi::unused_shader, 2, 3, rhi::unused_shader}}};
        rhi::pipeline_desc desc{};
        desc.kind = rhi::pipeline_kind::ray_tracing;
        desc.stages = stages;
        desc.ray_groups = groups;
        desc.max_ray_recursion_depth = 1;
        auto* object = gpu.api().create_pipeline(desc);
        if (!object)
            return std::unexpected(std::string("rt shadow: backend ray-tracing pipeline factory refused descriptor"));
        ray_tracing_pipeline_owned out;
        out.pipeline = vk_pipeline(object, gpu);
        out.group_count = static_cast<uint32_t>(groups.size());
        return out;
    }

    // The stochastic punctual lighting trace (shaders/megalights_trace.slang): the same compute-pipeline shape as
    // the passes above, with a push block of its own. It FORWARDS to the builder above rather than repeating
    // twenty lines of Vulkan, and it exists as its own name because a caller reading `build_two_set_compute`
    // inside this pass's create() would have to check that the two are still the same shape - which is exactly
    // the kind of coupling a name is for.
    std::expected<compute_pipeline_owned, std::string> build_megalights_trace(gpu_context const& gpu,
                                                                              std::span<uint8_t const> const compute_shader_code) {
        return build_two_set_compute(gpu, compute_shader_code);
    }
    // ... and the chain's temporal resolve: the same shape, with the accumulation's own push block.
    std::expected<compute_pipeline_owned, std::string> build_megalights_temporal(gpu_context const& gpu,
                                                                                 std::span<uint8_t const> const compute_shader_code) {
        return build_two_set_compute(gpu, compute_shader_code);
    }

    // The temporal resolve: its own pipeline, over the images the frame's heap carries. It reads no scene
    // buffer: the push block carries the two projection terms its depth guard needs.
    std::expected<resolve_pipeline_owned, std::string> build_resolve_pipeline(gpu_context const& gpu, std::span<uint8_t const> const compute_shader_code) {
        resolve_pipeline_owned out;
        out.resolve = make_compute_pipeline(gpu, compute_shader_code);
        if (!out.resolve)
            return std::unexpected(std::string("temporal resolve: backend compute pipeline factory refused descriptor"));
        return out;
    }

    std::expected<deferred_owned, std::string> build_deferred(gpu_context const& gpu, std::span<VkPipelineColorBlendAttachmentState const> const color_blend, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        deferred_owned out;
        std::array<VkFormat, 1> const color_formats = {deren::vulkan::hdr_format};
        auto pipeline_result = deren::vulkan::make_pipeline(
            gpu, std::span<VkFormat const>(color_formats), VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, 0.0f, 0.0f, 0.0f, color_blend);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.lighting = std::move(pipeline_result).value();
        return out;
    }

    std::expected<vk_pipeline, std::string> build_fxaa(gpu_context const& gpu, VkFormat const swap_chain_format, std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        auto pipeline_result = deren::vulkan::make_pipeline(
            gpu, swap_chain_format, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        return std::move(pipeline_result).value();
    }

    std::expected<vk_pipeline, std::string> build_shadow(gpu_context const& gpu, VkFormat const depth_format, float const depth_bias_constant_factor,
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
        auto result = deren::vulkan::make_pipeline(gpu,
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
    std::expected<fxaa_owned, std::string> build_fxaa_owned(gpu_context const& gpu, VkFormat const swap_chain_format,
                                                            std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        fxaa_owned out;

        // The anti-aliasing pipeline renders into the SWAPCHAIN, so its declared colour format is the surface's.
        auto pipeline_result = deren::vulkan::make_pipeline(
            gpu, swap_chain_format, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.antialias = std::move(pipeline_result).value();
        return out;
    }

    std::expected<upscale_owned, std::string> build_upscale_owned(gpu_context const& gpu, VkFormat const swap_chain_format,
                                                                  std::span<uint8_t const> const vertex_shader_code, std::span<uint8_t const> const fragment_shader_code) {
        using fail = std::unexpected<std::string>;
        upscale_owned out;

        // The resolve pipeline renders into the SWAPCHAIN, so its declared colour format is the surface's - and
        // that format is also what decides the shader's `encode_gamma` lane (see upscale_pass::record). The
        // single-target convenience form's blend state is the engine's standard src-alpha one, which the
        // fragment shader's alpha of 1.0 reduces to a copy: the same arrangement the composite's and FXAA's
        // swapchain writes already have, and the reason no blend state is spelled out here.
        auto pipeline_result = deren::vulkan::make_pipeline(
            gpu, swap_chain_format, VK_FORMAT_UNDEFINED, vertex_shader_code, fragment_shader_code, VK_SAMPLE_COUNT_1_BIT, false, true, 0.0f, 0.0f, 0.0f);
        if (!pipeline_result) {
            return fail(std::string(pipeline_result.error()));
        }
        out.resolve = std::move(pipeline_result).value();
        return out;
    }
} // namespace deren::vulkan::pipelines
