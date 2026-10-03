// The stochastic punctual lighting pass's implementation: the first-use transition around its output image, the
// half-resolution dispatch, the hand-off that makes the image readable by the lighting stage that adds it, and the
// one thing it owns outside a frame - its compute pipeline, built from `megalights_trace.comp`: every resource it
// reads is a heap slot the frame binds once (see `megalights_trace.cppm`).
//
// THE ESTIMATOR'S PARAMETERS ARE THE PUSH BLOCK: a sample count, a minimum sample weight, a tmin and the two
// bias terms the shader's `lerp` takes. The barrier reasoning is the two transitions around its output image
// that the hazard rules require, and `megalights_trace.cppm` describes what else the pass owns.

module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.megalights_trace;

import deren.vulkan.render_resource;
import deren.vulkan.constant_init;
import deren.vulkan.pipelines; // build_megalights_trace: the compute pipeline this pass owns
import deren.utility;

namespace deren::vulkan::pass {

    megalights_trace_pass::~megalights_trace_pass() {
        this->release_owned();
    }

    void megalights_trace_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& megalights_trace_pass::io() const noexcept {
        return render_resource::megalights_trace_io;
    }

    deren::vulkan::pass::behaviour const& megalights_trace_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view megalights_trace_pass::feature() const noexcept {
        // The pass's own feature name: "the knob and this pass having built its pipeline". The renderer's
        // `megalights_active()` composes the same answer for the lighting stage, which is what keeps the two
        // from disagreeing about whether the punctual lights were already handled this frame.
        return "megalights";
    }

    bool megalights_trace_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    VkPipeline megalights_trace_pass::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
    }

    void megalights_trace_pass::set_light_angle(float const radians) noexcept {
        // The angle is the emitter's half-size: past a tenth of a radian the "small emitter" the BRDF's
        // representative-point approximation assumes stops being small, so the clamp is where that approximation is
        // still honest rather than where the math breaks.
        this->light_angle = std::clamp(radians, 0.0f, 0.1f);
    }

    void megalights_trace_pass::set_estimator(uint32_t const samples, float const min_weight, float const bias_floor, float const bias_grazing) noexcept {
        // The clamps are this pass's, with the values: the sample count is bounded by the shader's own
        // compile-time array (see the header), and the three floats are bounded by what they MEAN - a negative
        // minimum weight inverts the smooth cut, and a negative bias would start the ray inside the surface it
        // is leaving.
        this->sample_count = std::clamp(samples, 1u, max_samples);
        this->weight_floor = std::max(min_weight, 0.0f);
        this->floor_bias = std::max(bias_floor, 0.0f);
        this->grazing_bias = std::max(bias_grazing, this->floor_bias);
    }

    void megalights_trace_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing per-generation is kept here yet: the chain is one pass until the temporal resolve lands, and
        // that one is where a history and its reset will live (see docs/megalights.md's staging).
        this->frame_index = 0;
    }

    void megalights_trace_pass::create(pass_context const& context) {
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
        std::span<uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("stochastic punctual lighting disabled: the owner has no {}", shader_name);
            return;
        }
        auto built = pipelines::build_megalights_trace(context.device, spirv);
        if (!built) {
            deren::utility::log("stochastic punctual lighting disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: stochastic punctual lighting pipeline created (sampled lights with ray-traced visibility)");
    }

    void megalights_trace_pass::record(resolved_io const& io) {
        if (!this->pass_pipeline.has_value() || io.barrier_images.size() < render_resource::megalights_trace_barriers.size() || io.extent.width == 0 || io.extent.height == 0) {
            return; // the runner resolves all of this or skips the pass (the declaration's own gates are the table's)
        }
        VkImage const output = io.barrier_images[barrier_output].image;

        // The image is this pass's to place, and it needs TWO transitions per frame:
        //  * UNDEFINED -> GENERAL here, because the pass writes it as a STORAGE image and its heap descriptor
        //    (the storage slot `publish_frame_resources` writes for this image) declares GENERAL - the same
        //    statement the tracer makes about `gi_trace`;
        //  * GENERAL -> SHADER_READ at the end, because the deferred lighting stage samples the SAME image
        //    through its own heap slot and that descriptor declares SHADER_READ - one image, two heap descriptors,
        //    and each is only accessed while the image is in the layout it names.
        VkImageMemoryBarrier2 to_general = deren::vulkan::undefined_to_general_transition;
        to_general.image = output;
        VkDependencyInfo const first_use = make_image_dependency_info(1, &to_general);
        vkCmdPipelineBarrier2(io.cmd, &first_use);

        // The pipeline, and no sets: the tracer's inputs are heap slots (the scene's buffers, the G-buffer images
        // per swapchain image, its own storage images), which the frame bound on this command buffer.

        push_constants push = {};
        push.inv_view_proj = io.constants.inv_view_proj;
        push.params = glm::vec4(static_cast<float>(this->sample_count), this->weight_floor, this->tmin, static_cast<float>(this->frame_index));
        push.bias = glm::vec4(this->floor_bias, this->grazing_bias, this->light_angle, 0.0f);
        static_assert(sizeof(push) <= pass::max_push_bytes, "the estimator's push block must fit the guaranteed minimum");
        [[maybe_unused]] bool const pushed = io.push_block(io.cmd, pass::push_bytes(push));
        vkCmdDispatch(io.cmd, (io.extent.width + group_size - 1u) / group_size, (io.extent.height + group_size - 1u) / group_size, 1);

        // ... and the hand-off: a compute SHADER_WRITE is not visible to the lighting stage's texture fetch
        // without this, and the layout it leaves the image in is the one the lighting stage's descriptor
        // declares.
        VkImageMemoryBarrier2 to_sampling = deren::vulkan::general_to_sampling_transition;
        to_sampling.image = output;
        VkDependencyInfo const hand_off = make_image_dependency_info(1, &to_sampling);
        vkCmdPipelineBarrier2(io.cmd, &hand_off);

        ++this->frame_index; // the next frame's ray sequence must differ (see the header)
    }

} // namespace deren::vulkan::pass
