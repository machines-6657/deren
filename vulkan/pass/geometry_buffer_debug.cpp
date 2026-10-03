// The G-buffer debug view's implementation: the four images the pass moves to a sampled layout, the frame's two
// bookkeeping callbacks, the CLEAR instance over the HDR target, the one-set bind, the 16-byte push and the
// fullscreen draw. Moved out of `runtime::record_gbuffer_debug_pass` UNCHANGED in behaviour except for the two
// things the pass does not own - the HDR target's own transition and the missing-set fallback, both of which are
// the frame loop's (see the pass's header) - so the knob-on A/B against the parent commit is what decides the move.

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.geometry_buffer_debug;

import deren.vulkan.constant_init;
import deren.vulkan.pipelines; // build_gbuffer_debug: the view pipeline, and that is all the pass takes from the builder
import deren.utility;

namespace deren::vulkan::pass {

    gbuffer_debug_pass::~gbuffer_debug_pass() {
        this->release_owned();
    }

    void gbuffer_debug_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& gbuffer_debug_pass::io() const noexcept {
        return render_resource::gbuffer_debug_io;
    }

    deren::vulkan::pass::behaviour const& gbuffer_debug_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view gbuffer_debug_pass::feature() const noexcept {
        // THE NAME THE RENDERER ALREADY ANSWERS: `f.gbuffer_debug` is "the knob is on, the G-buffer pipeline and this
        // pass's pipeline exist", which is also what the overlay asks when it offers the view and what the composite
        // asks when it zeroes the bloom weight for it - one answer, not three spellings of the same question.
        return "gbuffer-debug";
    }

    void gbuffer_debug_pass::create(pass_context const& context) {
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
            deren::utility::log("gbuffer debug view disabled: the owner has no {} or {}", vertex_shader_name, fragment_shader_name);
            return;
        }
        // The debug view reads the stored surface through the frame's heap, so the pipeline is all it builds.
        auto built = pipelines::build_gbuffer_debug(context.device, vertex_spirv, fragment_spirv);
        if (!built) {
            deren::utility::log("gbuffer debug view disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->debug);
        deren::utility::log("SUCCESS: gbuffer debug pipeline created (the stored surface, one channel at a time)");
    }

    void gbuffer_debug_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the surface's FORMAT and on nothing whose size changes, and the
        // set this pass binds belongs to the G-buffer family, whose owner retires it.
    }

    bool gbuffer_debug_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    VkPipeline gbuffer_debug_pass::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
    }

    void gbuffer_debug_pass::set_channel(int32_t const channel) noexcept {
        // The clamp came with the parameter: the count is the declaration's own `gbuffer_channel_count`, and a
        // channel outside it would index the shader's switch by a value it does not know.
        this->debug_channel = std::clamp(channel, 0, channel_count - 1);
    }

    int32_t gbuffer_debug_pass::channel() const noexcept {
        return this->debug_channel;
    }

    void gbuffer_debug_pass::record(resolved_io const& io) {
        if (!this->pipeline_ready() || io.targets.empty() || io.pipelines.empty() || io.pipelines[0] == VK_NULL_HANDLE ||
            io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (see frame_pass::resolve)
        }
        VkImageView const target_view = io.targets[0].view;
        if (target_view == VK_NULL_HANDLE) {
            return;
        }
        // THE FOUR IMAGES THIS PASS READS BECOME SAMPLES, in ONE dependency info and in the declaration's order -
        // the three stored targets, then the motion-vector target. A pipeline barrier may not be recorded inside a
        // rendering instance, which is why this is here and not after vkCmdBeginRendering below.
        std::array<VkImageMemoryBarrier2, render_resource::gbuffer_debug_barriers.size()> barriers = {};
        for (std::size_t b = 0; b < barriers.size(); ++b) {
            barriers[b] = deren::vulkan::hdr_sampling_transition; // COLOR_ATTACHMENT -> SHADER_READ
            barriers[b].image = io.barrier_images[b].image;
        }
        VkDependencyInfo const dependency = make_image_dependency_info(static_cast<uint32_t>(barriers.size()), barriers.data());
        vkCmdPipelineBarrier2(io.cmd, &dependency);
        // ... and the two pieces of per-image bookkeeping the frame USED to carry are now the renderer's stage
        // preamble (see gbuffer_debug_frame's replacement note): the depth's hand-back and the motion-vector flag's
        // clearing are the frame's ordering rules about images the G-buffer pass wrote.
        VkClearValue clear = {};
        VkRenderingAttachmentInfo const attachment = make_color_attachment_info(target_view, clear, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE);
        VkRenderingInfo const rendering_info = make_rendering_info(0, {{0, 0}, io.extent}, true, &attachment, nullptr);
        vkCmdBeginRendering(io.cmd, &rendering_info);
        vkCmdSetCullMode(io.cmd, VK_CULL_MODE_NONE); // the synthetic triangle has no facing to cull
        // No set to bind: the G-buffer images are per-swapchain-image heap slots the shader indexes itself with
        // the image index its push block carries (see shaders/gbuffer_debug.slang and heap_slots.glsl).
        // The push block is the pass's own now: the channel it owns, the frame's two projection terms (from
        // `resolved_io::constants`) and the motion gain, which scales itself across resolutions by using the frame's
        // own width (four pixels saturate the motion channel).
        push_constants const push = {
            .channel = static_cast<float>(this->debug_channel),
            .proj_22 = io.constants.proj[2][2],
            .proj_32 = io.constants.proj[3][2],
            .motion_gain = static_cast<float>(io.frame.extent.width) * 0.25f,
        };
        [[maybe_unused]] bool const pushed = io.push_block(io.cmd, pass::push_bytes(push));
        vkCmdDraw(io.cmd, 3, 1, 0, 0);
        vkCmdEndRendering(io.cmd);
    }

} // namespace deren::vulkan::pass
