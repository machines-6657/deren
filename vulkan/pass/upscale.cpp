// The upscale pass's implementation: the LDR image's transition to a sampled layout, the clear instance over
// the swapchain, the push block with the display transfer in it, the fullscreen draw and the overlay inside
// the same instance. Moved into a pass from the shape the FXAA pass established, and the difference between
// the two is exactly what the resolve needs: FXAA's filter runs at the render resolution (a `full` extent),
// while this pass's extent comes from its declaration's `resource` rule over the swapchain image, i.e. the
// OUTPUT extent - so the same single tap of the same linear sampler covers the whole presented image.

module;

#include <array>
#include <cstdint>
#include <span>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.upscale;

import deren.vulkan.constant_init;
import deren.vulkan.pipelines; // build_upscale_owned: the pass's own pipeline, from its two shaders and the surface's format
import deren.utility;

namespace deren::vulkan::pass {

    upscale_pass::~upscale_pass() {
        this->release_owned();
    }

    void upscale_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& upscale_pass::io() const noexcept {
        return render_resource::upscale_io;
    }

    deren::vulkan::pass::behaviour const& upscale_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view upscale_pass::feature() const noexcept {
        // THE NAME THE RENDERER ALREADY ANSWERS: `f.upscale` = "the render chain is smaller than the output AND
        // this pass built its pipeline" (`runtime::post_upscale_active`), which is one definition shared with
        // the composite's target choice and the overlay's owner - so the runner's gate, the frame's target and
        // the overlay cannot disagree about whether this pass runs. At render_scale = 1.0 it answers false and
        // the pass is never resolved or recorded, which is what keeps every scale-1.0 frame byte-identical.
        return "upscale";
    }

    void upscale_pass::create(pass_context const& context) {
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
            deren::utility::log("upscale disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // The surface's format is the pipeline's declared colour format (the resolve writes the swapchain).
        auto built = pipelines::build_upscale_owned(context.device, context.swap_chain_image_format, vertex_spirv, fragment_spirv);
        if (!built) {
            deren::utility::log("upscale disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->resolve);
        this->swap_chain_format = context.swap_chain_image_format;
        deren::utility::log("SUCCESS: upscale pipeline created (LDR -> the presented swapchain, a linear filter)");
    }

    void upscale_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the surface's FORMAT (a session-stable device fact) and not
        // on its size - the viewport is resynced by the runner from `io.extent`, which is the swapchain's own
        // extent this time (see pass_behaviour) - and this pass owns no descriptor family.
    }

    bool upscale_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    VkPipeline upscale_pass::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
    }

    void upscale_pass::set_frame(upscale_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void upscale_pass::set_filter(upscale_filter const filter) noexcept {
        this->filter_kind = filter;
    }

    upscale_filter upscale_pass::filter() const noexcept {
        return this->filter_kind;
    }

    easu_constants make_easu_constants(uint32_t const render_width, uint32_t const render_height, uint32_t const output_width, uint32_t const output_height) noexcept {
        // `FsrEasuCon` TRANSCRIBED (AMD's ffx_fsr1.h v1.20210629, see shaders/upscale.slang for the license
        // and the kernel that consumes these): the input viewport and the input size are the same two numbers
        // here, because this renderer has no dynamic-resolution offset region inside a larger input resource.
        //
        // THE ZERO GUARDS ARE NOT DEFENSIVE PADDING: these are divisions by an extent, and a zero would put an
        // infinity into EASU's tap positions, which produces a frame of garbage rather than a small image. The
        // frame cannot reach here with a zero extent (the pass returns early, and `core::render_extent` clamps
        // to 1), so the guard exists to make that a property of THIS function rather than of its callers.
        float const in_w = static_cast<float>(render_width == 0u ? 1u : render_width);
        float const in_h = static_cast<float>(render_height == 0u ? 1u : render_height);
        float const out_w = static_cast<float>(output_width == 0u ? 1u : output_width);
        float const out_h = static_cast<float>(output_height == 0u ? 1u : output_height);
        float const rcp_in_w = 1.0f / in_w;
        float const rcp_in_h = 1.0f / in_h;

        easu_constants con = {};
        // The output integer position -> a pixel position in the input viewport, and the -0.5 that keeps the
        // mapping centred on the input's own pixel centres rather than its corner.
        con.con0[0] = in_w / out_w;
        con.con0[1] = in_h / out_h;
        con.con0[2] = 0.5f * in_w / out_w - 0.5f;
        con.con0[3] = 0.5f * in_h / out_h - 0.5f;
        // Viewport pixel position -> normalized image space, which is the upper-left of the 'F' tap, plus the
        // three further gather positions relative to it (the reference's own arrangement, kept so the shader's
        // p0..p3 stay recognisable against it).
        con.con1[0] = rcp_in_w;
        con.con1[1] = rcp_in_h;
        con.con1[2] = 1.0f * rcp_in_w;
        con.con1[3] = -1.0f * rcp_in_h;
        con.con2[0] = -1.0f * rcp_in_w;
        con.con2[1] = 2.0f * rcp_in_h;
        con.con2[2] = 1.0f * rcp_in_w;
        con.con2[3] = 2.0f * rcp_in_h;
        con.con3[0] = 0.0f * rcp_in_w;
        con.con3[1] = 4.0f * rcp_in_h;
        con.con3[2] = 0.0f;
        con.con3[3] = 0.0f;
        return con;
    }

    void upscale_pass::set_overlay(draw_callback const overlay) noexcept {
        // The host's hook, installed once. This pass needs no fact to decide whether to use it: whenever the
        // chain is resolved, THIS is the frame's last writer (the composite's frame is what needs the answer).
        this->overlay_callback = overlay;
    }

    void upscale_pass::prepare_frame([[maybe_unused]] frame_facts const& facts) noexcept {
        this->set_frame(upscale_frame{.after_draw = this->overlay_callback});
    }

    void upscale_pass::record(resolved_io const& io) {
        if (!this->pipeline_ready() || io.targets.empty() || io.pipelines.empty() || io.pipelines[0] == VK_NULL_HANDLE ||
            io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (see frame_pass::resolve)
        }
        VkImage const target = io.targets[0].image;
        VkImageView const target_view = io.targets[0].view;
        if (target == VK_NULL_HANDLE || target_view == VK_NULL_HANDLE) {
            return;
        }
        // THE INPUT FIRST, and it is THIS pass's transition rather than the frame loop's: the LDR image was
        // written by the composite earlier in this same command buffer, so its old layout is known to be a
        // colour attachment and the src masks have to publish that write. It is the declaration's one barrier
        // image, so the handle is the one the pass named - and on a frame this pass does not run, nothing moves
        // the image at all (the composite writes it as an attachment, or writes the swapchain directly).
        if (!io.barrier_images.empty() && io.barrier_images[0].image != VK_NULL_HANDLE) {
            VkImageMemoryBarrier2 to_sampling = deren::vulkan::hdr_sampling_transition;
            to_sampling.image = io.barrier_images[0].image;
            VkDependencyInfo const sampling_dependency = make_image_dependency_info(1, &to_sampling);
            vkCmdPipelineBarrier2(io.cmd, &sampling_dependency);
        }
        // ... then the swapchain, which the instance CLEARs: UNDEFINED as the old layout asserts nothing about
        // contents the resolve is about to replace entirely (the same claim the composite and FXAA make - a
        // CLEAR instance's old layout is dead by definition).
        VkImageMemoryBarrier2 to_attachment = deren::vulkan::color_attachment_transition;
        to_attachment.image = target;
        VkDependencyInfo const attachment_dependency = make_image_dependency_info(1, &to_attachment);
        vkCmdPipelineBarrier2(io.cmd, &attachment_dependency);
        // The push block is composed HERE (S3), and it is the pass's own. TWO GROUPS OF LANES:
        //
        //   * EASU's four constants, from the two extents the pass can see without being told the scale: the
        //     FRAME's extent is the render chain's (`io.frame.extent` - the same value `pass_frame` handed the
        //     resolver) and `io.extent` is the OUTPUT extent this pass's own `resource` rule resolved to. Their
        //     ratio IS the render scale, which is why nothing here needs to know it.
        //   * the display transfer, which follows from the surface's format - 0 when the swapchain attachment
        //     encodes linear -> sRGB in hardware (so the shader must hand it LINEAR values) and 1 when the
        //     format is UNORM and the display-encoded value is what should be stored. The same lane, the same
        //     convention and the same expression as fxaa_pass::record, because a second spelling of the
        //     transfer is a second chance to double-encode the frame.
        easu_constants const es = make_easu_constants(io.frame.extent.width, io.frame.extent.height, io.extent.width, io.extent.height);
        push_constants const push = {
            .con0 = {es.con0[0], es.con0[1], es.con0[2], es.con0[3]},
            .con1 = {es.con1[0], es.con1[1], es.con1[2], es.con1[3]},
            .con2 = {es.con2[0], es.con2[1], es.con2[2], es.con2[3]},
            .con3 = {es.con3[0], es.con3[1], es.con3[2], es.con3[3]},
            .mode = this->filter_kind == upscale_filter::easu ? 1.0f : 0.0f,
            .encode_gamma = deren::vulkan::is_srgb_format(this->swap_chain_format) ? 0.0f : 1.0f,
        };
        VkClearValue clear = {};
        VkRenderingAttachmentInfo const attachment = make_color_attachment_info(target_view, clear, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE);
        VkRenderingInfo const rendering_info = make_rendering_info(0, {{0, 0}, io.extent}, true, &attachment, nullptr);
        vkCmdBeginRendering(io.cmd, &rendering_info);
        vkCmdSetCullMode(io.cmd, VK_CULL_MODE_NONE); // the synthetic triangle has no facing to cull
        // The source is a heap slot (see upscale.slang): the appended index lane names it, and the frame bound
        // the heaps for this command buffer, so there is no set to bind here.
        [[maybe_unused]] bool const pushed = io.push_block(io.cmd, pass::push_bytes(push));
        vkCmdDraw(io.cmd, 3, 1, 0, 0);
        // INSIDE the instance, between the draw and its end: this pass is the frame's LAST writer whenever it
        // runs, so the overlay belongs here and NOT in the composite's instance - the composite drew into the
        // render-extent LDR image this pass is about to resample, so a UI drawn there would be scaled up with
        // the scene (see upscale_frame::after_draw and the composite's frame for the other case).
        if (this->pass_frame.after_draw.valid()) {
            this->pass_frame.after_draw.record(this->pass_frame.after_draw.owner, io.cmd);
        }
        vkCmdEndRendering(io.cmd);
    }

} // namespace deren::vulkan::pass
