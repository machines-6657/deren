// The FXAA pass's implementation: the LDR image's transition to a sampled layout, the clear instance over the
// swapchain, the one-set bind, the 52-byte push block with mode 3 in it, the fullscreen draw and the overlay
// inside the same instance. Moved out of `runtime::record_fxaa` (which was the second half of
// `record_composite`) UNCHANGED in behaviour - the same two barriers in the same order, the same attachment, the
// same push lanes and the same draw - so the capture gate decides the move on `deferred_taa_fxaa`, the one
// scenario that runs with FXAA on.

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.fxaa;

import deren.vulkan.constant_init;
import deren.vulkan.pipelines; // build_fxaa_owned: the pass's own pipeline, built from its two shaders and the surface's format
import deren.utility;

namespace deren::vulkan::pass {

    fxaa_pass::~fxaa_pass() {
        this->release_owned();
    }

    void fxaa_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& fxaa_pass::io() const noexcept {
        return render_resource::fxaa_io;
    }

    deren::vulkan::pass::behaviour const& fxaa_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view fxaa_pass::feature() const noexcept {
        // THE NAME THE RENDERER ALREADY ANSWERS: `f.fxaa` = "the knob is on AND the pipeline exists", which is one
        // definition (`runtime::post_fxaa_active`) shared with the composite's target choice and the overlay's
        // owner - so the runner's gate, the frame's target and the overlay cannot disagree about whether this pass
        // runs.
        return "fxaa";
    }

    void fxaa_pass::create(pass_context const& context) {
        if (context.device == VK_NULL_HANDLE) {
            return;
        }
        if (this->device != VK_NULL_HANDLE && this->device != context.device) {
            this->release_owned();
        }
        this->device = context.device;
        if (this->pass_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<uint8_t const> const vertex_spirv = context.shader != nullptr ? context.shader(context.owner, vertex_shader_name) : std::span<uint8_t const>{};
        std::span<uint8_t const> const fragment_spirv = context.shader != nullptr ? context.shader(context.owner, fragment_shader_name) : std::span<uint8_t const>{};
        if (vertex_spirv.empty() || fragment_spirv.empty()) {
            deren::utility::log("fxaa disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // The surface's format is the pipeline's declared colour format (the filter writes the swapchain).
        auto built = pipelines::build_fxaa_owned(context.device, context.swap_chain_image_format, vertex_spirv, fragment_spirv);
        if (!built) {
            deren::utility::log("fxaa disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->antialias);
        this->swap_chain_format = context.swap_chain_image_format;
        deren::utility::log("SUCCESS: fxaa pipeline created (LDR -> anti-aliased swapchain)");
    }

    void fxaa_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the surface's FORMAT (a session-stable device fact) and not on
        // its size, and the set this pass binds belongs to the post family, whose owner retires it.
    }

    bool fxaa_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    VkPipeline fxaa_pass::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
    }

    void fxaa_pass::set_frame(fxaa_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void fxaa_pass::set_overlay(draw_callback const overlay) noexcept {
        // The host's hook, installed once. This pass needs no fact to decide whether to use it: whenever FXAA
        // resolves, THIS is the frame's last writer (the composite's frame is what needs the answer).
        this->overlay_callback = overlay;
    }

    void fxaa_pass::prepare_frame([[maybe_unused]] frame_facts const& facts) noexcept {
        this->set_frame(fxaa_frame{.after_draw = this->overlay_callback});
    }

    void fxaa_pass::record(resolved_io const& io) {
        if (!this->pipeline_ready() || io.targets.empty() || io.pipelines.empty() || io.pipelines[0] == VK_NULL_HANDLE ||
            io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (see frame_pass::resolve)
        }
        VkImage const target = io.targets[0].image;
        VkImageView const target_view = io.targets[0].view;
        if (target == VK_NULL_HANDLE || target_view == VK_NULL_HANDLE) {
            return;
        }
        // THE INPUT FIRST, and it is THIS pass's transition rather than the frame loop's: the LDR image was written
        // by the composite earlier in this same command buffer, so its old layout is known to be a colour attachment
        // and the src masks have to publish that write. It is the declaration's one barrier image, so the handle is
        // the one the pass named - and on a frame this pass does not run, nothing moves the image at all (the
        // composite writes it as an attachment and the next frame writes it again).
        if (!io.barrier_images.empty() && io.barrier_images[0].image != VK_NULL_HANDLE) {
            VkImageMemoryBarrier2 to_sampling = deren::vulkan::hdr_sampling_transition;
            to_sampling.image = io.barrier_images[0].image;
            VkDependencyInfo const sampling_dependency = make_image_dependency_info(1, &to_sampling);
            vkCmdPipelineBarrier2(io.cmd, &sampling_dependency);
        }
        // ... then the swapchain, which the instance CLEARs: UNDEFINED as the old layout asserts nothing about
        // contents the filter is about to replace entirely.
        VkImageMemoryBarrier2 to_attachment = deren::vulkan::color_attachment_transition;
        to_attachment.image = target;
        VkDependencyInfo const attachment_dependency = make_image_dependency_info(1, &to_attachment);
        vkCmdPipelineBarrier2(io.cmd, &attachment_dependency);
        // The push block is composed HERE (S3): the frame's settings (exposure, the bloom weight/threshold and
        // this pass's two thresholds - all of them lanes the composite pushes too, which is why they are frame
        // settings rather than one pass's parameters), the struct's defaults for the lanes this mode does not
        // read, the `encode_gamma` lane (this pass always writes the swapchain, so the answer is the surface's
        // own format), and the pass's own stage lane - FXAA is mode 3.
        render_settings const& settings = io.constants.settings;
        post_push_constants push = {
            .exposure = settings.exposure,
            .bloom_intensity = settings.bloom_intensity,
            .bloom_threshold = settings.bloom_threshold,
            .mode = 3.0f, // the pass's own stage lane: FXAA
            // Same meaning as in the composite: 0 = the swapchain attachment encodes to display values in
            // hardware, so FXAA must hand it LINEAR values; 1 = the target is a UNORM format and FXAA's own
            // display-encoded result is what should be stored.
            .encode_gamma = deren::vulkan::is_srgb_format(this->swap_chain_format) ? 0.0f : 1.0f,
            .fxaa_subpixel = settings.fxaa_subpixel,
            .fxaa_edge_threshold = settings.fxaa_edge_threshold,
        };
        VkClearValue clear = {};
        VkRenderingAttachmentInfo const attachment = make_color_attachment_info(target_view, clear, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE);
        VkRenderingInfo const rendering_info = make_rendering_info(0, {{0, 0}, io.extent}, true, &attachment, nullptr);
        vkCmdBeginRendering(io.cmd, &rendering_info);
        vkCmdSetCullMode(io.cmd, VK_CULL_MODE_NONE); // the synthetic triangle has no facing to cull
        // The post chain's source is a heap slot now (see shaders/post.slang): the third push lane names it, and
        // the frame bound the heaps for this command buffer, so there is no set to bind here.
        [[maybe_unused]] bool const pushed = io.push_block(io.cmd, pass::push_bytes(push));
        vkCmdDraw(io.cmd, 3, 1, 0, 0);
        // INSIDE the instance, between the draw and its end: this pass is the frame's LAST writer whenever it runs,
        // so the overlay belongs here - drawing it in the composite's instance instead would let the edge filter
        // blur the UI text into mush (see fxaa_frame::after_draw, and the composite's frame for the other case).
        if (this->pass_frame.after_draw.valid()) {
            this->pass_frame.after_draw.record(this->pass_frame.after_draw.owner, io.cmd);
        }
        vkCmdEndRendering(io.cmd);
    }

} // namespace deren::vulkan::pass
