// The goo rim pass's implementation: the pipeline (one ADDITIVE target, no depth attachment), the fullscreen draw,
// and nothing else.
//
// MODELLED ON toon_screen_rim.cpp, and what is MISSING from it is the point: no barriers, and no clear. No barriers
// because the G-buffer targets and the depth are already in a sampled layout - the renderer's stage preamble
// publishes them (see `goo_rim`'s stage in runtime.frames.cppm), and this pass is never the one that took them out
// of it. No clear because the target is LOADed: the whole point is to ADD to the frame the character-forward stage
// produced, not to replace it.

module;

#include <array>
#include <cstdint>
#include <glm/glm.hpp> // the push block's inverse view-projection (`io.constants.inv_view_proj`)
#include <span>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.goo_rim;

import deren.vulkan.render_resource;
import deren.vulkan.constant_init;
import deren.vulkan.core;          // deren::vulkan::hdr_format: the one target this pass writes
import deren.vulkan.core.pipeline; // deren::vulkan::make_pipeline: the generic builder this pass uses directly
import deren.utility;

namespace deren::vulkan::pass {

    render_resource::pass_io const& goo_rim_pass::io() const noexcept {
        return render_resource::goo_rim_io;
    }

    deren::vulkan::pass::behaviour const& goo_rim_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view goo_rim_pass::feature() const noexcept {
        // A NAME THE CHAIN OWNER COMPOSES, not a `[render]` key (see `toon_screen_rim_pass::feature`, whose
        // arrangement this is): `render_start_demo::feature_active` answers `goo_rim` with
        // `character_forward_pending && the pass is ready && runtime::goo_toon_active()`.
        //
        // THE THIRD TERM IS THE WHOLE REASON THIS PASS IS SAFE TO ADD: with `[render] goo_toon` false the runner
        // asks this before it resolves anything, so the pass records NOTHING - no instance, no push, no draw - and
        // every existing capture scenario is byte-identical rather than "identical because the shader wrote zero".
        // And it cannot disagree with which pipeline the character stage drew through, because it is the runtime's
        // own predicate: `goo_toon_on && the pipeline exists`.
        return "goo_rim";
    }

    void goo_rim_pass::create(pass_context const& context) {
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
            // NO FORMAT ARGUMENT IN THIS TRANSLATION UNIT, and that is a toolchain workaround rather than a style:
            // clang 22.1.8 crashes in `EmitBuiltinNewDeleteCall` while generating a function that returns a
            // `std::string`-carrying value through a format-argument `deren::utility::log` here - the same bug
            // `render_start_demo.cpp` records, met again while this pass was added. The message names both files
            // literally instead.
            deren::utility::log("goo rim disabled: the owner has no post.vert.spv or goo_rim.frag.spv");
            return;
        }
        // ONE colour target and it is the HDR one, because this pass runs INSIDE the HDR chain (after the
        // character stage, before the resolve) - a rim written to the swapchain would be tonemapped twice.
        std::array<VkFormat, 1> const formats = {deren::vulkan::hdr_format};
        // ADDITIVE: the rim is `混合.019`'s ADD - a contribution to the frame, not a replacement for it. The
        // character stage's own pipeline is the opposite (blending OFF, it OVERWRITES), which is exactly why this
        // rim could not be a second draw in that pass.
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
            // The reason is dropped for the same toolchain reason as the pair above; the pipeline builder logs the
            // cause itself (see `deren::vulkan::make_pipeline`).
            deren::utility::log("goo rim disabled: the pipeline builder refused (see the pipeline log above)");
            this->release_owned();
            return;
        }
        built->viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}; // the runner resyncs it from io.extent
        built->scissor = {{0, 0}, {1u, 1u}};
        this->pass_pipeline = std::move(*built);
        deren::utility::log("SUCCESS: goo rim pipeline created (the rewritten toon chain's rim, recomposed from the G-buffer)");
    }

    void goo_rim_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the HDR target's format and on nothing whose size changes,
        // and the viewport/scissor are resynced by the runner (see pass_behaviour::resync_viewport).
    }

    void goo_rim_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    goo_rim_pass::~goo_rim_pass() {
        this->release_owned();
    }

    bool goo_rim_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    VkPipeline goo_rim_pass::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
    }

    void goo_rim_pass::record(resolved_io const& io) {
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
        // NO SET TO BIND: the G-buffer, the material table and the colour lanes are per-image or frame-invariant
        // heap slots the shader indexes with the two lanes the framework appends to the block below. The three
        // constants are the FRAME's - the same inverse view-projection `deferred` unprojects with, and the same
        // depth-linearization pair the article's contour and the debug view linearize with.
        push_constants const push = {
            .inv_view_proj = io.constants.inv_view_proj,
            .proj_22 = io.constants.proj[2][2],
            .proj_32 = io.constants.proj[3][2],
        };
        [[maybe_unused]] bool const pushed = io.push_block(io.cmd, pass::push_bytes(push));
        vkCmdDraw(io.cmd, 3, 1, 0, 0);
        vkCmdEndRendering(io.cmd);
    }

} // namespace deren::vulkan::pass
