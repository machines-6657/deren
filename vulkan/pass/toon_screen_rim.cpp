// The screen-space depth rim's implementation: the pipeline (one ADDITIVE target, no depth attachment), the
// fullscreen draw, and nothing else.
//
// MODELLED ON geometry_buffer_debug.cpp, and what is MISSING from it is the point: no barriers, and no
// clear. No barriers because the two images this pass samples are already in a sampled layout - the G-buffer
// targets and the depth are published by the stage PREAMBLE (the renderer's per-image bookkeeping, see
// toon_screen_rim's stage in runtime.frames.cppm), and this pass is never the one that took them out of it.
// No clear because the target is LOADed: the whole point is to ADD to the frame the character-forward stage
// produced, not to replace it.

module;

#include <array>
#include <cstdint>
#include <span>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.toon_screen_rim;

import deren.vulkan.render_resource;
import deren.vulkan.constant_init;
import deren.vulkan.core;          // deren::vulkan::hdr_format: the one target this pass writes
import deren.vulkan.core.pipeline; // deren::vulkan::make_pipeline: the generic builder this pass uses directly
import deren.utility;

namespace deren::vulkan::pass {

    render_resource::pass_io const& toon_screen_rim_pass::io() const noexcept {
        return render_resource::toon_screen_rim_io;
    }

    deren::vulkan::pass::behaviour const& toon_screen_rim_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view toon_screen_rim_pass::feature() const noexcept {
        // THE CHARACTER-FORWARD FEATURE, not one of its own: the contour belongs to the toon character stage
        // and means nothing without it, so one switch turns both on. Gating it here is also what keeps every
        // capture scenario byte-identical while that feature is off - the runner asks this before it resolves
        // the declaration, so a frame with no character never even transitions an image for this pass.
        //
        // ... AND IT IS ITS OWN NAME RATHER THAN `character_forward`, WHICH IT USED TO SHARE, because the
        // REWRITTEN toon chain must not wear TWO rims either: this pass draws the ARTICLE's screen-space contour
        // (`toon_screen_rim.slang`), and the Goo reference the rewrite follows has no such contour - its
        // screen-space piece is `DepthRim`, which the rewrite's second step defers (see
        // `deren-ab/goo_step2_rim_spec.md` §9-U1/U2/U4/U5). Sharing the name made the two impossible to separate:
        // `feature_active` is asked once per NAME, so "the rim stage is off" and "the character stage is off"
        // were one answer and the switch could only turn off both.
        //
        // THE NAME IS NOT A NEW CONFIG KEY and does not become one: `[render]` keys are the runtime's registry
        // (see `runtime::feature_active`), and this is a name that table composes - the owner's decision is in
        // `render_start_demo::feature_active`, where the rewritten chain's own answer (`goo_toon` AND its
        // pipeline) is what turns THIS stage off. The article's rim would otherwise be drawn on top of the
        // reference's.
        return "toon_screen_rim";
    }

    void toon_screen_rim_pass::create(pass_context const& context) {
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
            deren::utility::log("toon screen rim disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // ONE colour target and it is the HDR one, because this pass runs INSIDE the HDR chain (after the
        // lighting stage, before the resolve) - a rim written to the swapchain would be tonemapped twice.
        std::array<VkFormat, 1> const formats = {deren::vulkan::hdr_format};
        // ADDITIVE: the rim is a contribution to the frame, not a replacement for it. This is the opposite of
        // the character-forward stage's overwrite, and the two are deliberately different passes for it.
        std::array<VkPipelineColorBlendAttachmentState, 1> const blends = {make_color_blend_attachment_additive()};
        auto built = deren::vulkan::make_pipeline(context.device,
                                                  std::span<VkFormat const>(formats),
                                                  VK_FORMAT_UNDEFINED, // NO depth attachment: the depth is sampled, not tested
                                                  vertex_spirv,
                                                  fragment_spirv,
                                                  VK_SAMPLE_COUNT_1_BIT,
                                                  /*depth_test_enabled=*/false,
                                                  0.0f,
                                                  0.0f,
                                                  0.0f,
                                                  std::span<VkPipelineColorBlendAttachmentState const>(blends));
        if (!built) {
            deren::utility::log("toon screen rim disabled: {}", built.error());
            this->release_owned();
            return;
        }
        built->viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}; // the runner resyncs it from io.extent
        built->scissor = {{0, 0}, {1u, 1u}};
        this->pass_pipeline = std::move(*built);
        deren::utility::log("SUCCESS: toon screen rim pipeline created (a fullscreen additive contour from the depth)");
    }

    void toon_screen_rim_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the HDR target's format and on nothing whose size changes,
        // and the viewport/scissor are resynced by the runner (see pass_behaviour::resync_viewport).
    }

    void toon_screen_rim_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    toon_screen_rim_pass::~toon_screen_rim_pass() {
        this->release_owned();
    }

    bool toon_screen_rim_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    VkPipeline toon_screen_rim_pass::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
    }

    void toon_screen_rim_pass::set_shape(float const width, float const scale, float const strength) noexcept {
        this->rim_width = width;
        this->rim_scale = scale;
        this->rim_strength = strength;
    }

    void toon_screen_rim_pass::set_colour(float const r, float const g, float const b) noexcept {
        this->rim_colour[0] = r;
        this->rim_colour[1] = g;
        this->rim_colour[2] = b;
    }

    void toon_screen_rim_pass::record(resolved_io const& io) {
        if (!this->pipeline_ready() || io.targets.empty() || io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (see frame_pass::resolve)
        }
        VkImageView const target_view = io.targets[0].view;
        if (target_view == VK_NULL_HANDLE) {
            return;
        }
        // LOAD, not clear: this instance ADDS to the frame the character-forward stage wrote.
        VkRenderingAttachmentInfo const attachment = make_load_color_attachment_info(target_view);
        VkRenderingInfo const rendering_info = make_rendering_info(0, {{0, 0}, io.extent}, true, &attachment, nullptr);
        vkCmdBeginRendering(io.cmd, &rendering_info);
        vkCmdSetCullMode(io.cmd, VK_CULL_MODE_NONE); // the synthetic triangle has no facing to cull
        // NO SET TO BIND: the depth and the albedo are per-swapchain-image heap slots the shader indexes with
        // the image index its push block carries. The two projection terms are the frame's (they are the same
        // pair the debug view's depth channel linearizes with), and the rest is this pass's own parameter.
        push_constants const push = {
            .proj_22 = io.constants.proj[2][2],
            .proj_32 = io.constants.proj[3][2],
            .rim_width = this->rim_width,
            .rim_scale = this->rim_scale,
            .rim_strength = this->rim_strength,
            .pad0 = 0.0f,
            .pad1 = 0.0f,
            .pad2 = 0.0f,
            .rim_colour = {this->rim_colour[0], this->rim_colour[1], this->rim_colour[2], 1.0f},
        };
        [[maybe_unused]] bool const pushed = io.push_block(io.cmd, pass::push_bytes(push));
        vkCmdDraw(io.cmd, 3, 1, 0, 0);
        vkCmdEndRendering(io.cmd);
    }

} // namespace deren::vulkan::pass
