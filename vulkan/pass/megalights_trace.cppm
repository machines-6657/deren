// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/megalights_trace.cppm
 * @brief The stochastic punctual lighting pass: a few sampled lights per pixel, one visibility ray each.
 * @defgroup vulkan_pass_megalights_trace Megalights Trace Pass
 *
 * WHAT IT OWNS: the frame's recording - the two transitions around its output image, the two shared parts of the
 * frame's heap it reads (the scene block, which carries the camera, the light UBO and the cluster lists, and the
 * G-buffer images, which carry the surface it evaluates those lights for), the push block and the half-resolution
 * dispatch - plus its OWN compute pipeline, the one shape every traced pass has, built at create time from its
 * declaration's push-block size and its shader: one compute dispatch over the same heap slots every traced pass
 * reads, with the estimator's parameters in the push block.
 *
 * WHAT IT DOES NOT OWN: any descriptor. Its output image is reached through the frame's heap (the storage slot the
 * runtime publishes for it, and the lighting stage samples the same image through its own slot), so the only
 * handles it needs are the IMAGE it moves - which is what `pass_io::barrier_images` is for - and the two shared
 * parts of the heap above.
 *
 * THE ESTIMATOR ITSELF IS THE SHADER'S (shaders/megalights_trace.slang has the derivation and the UE
 * references); what lives here is the four values the renderer can decide per frame: how many samples, the
 * minimum sample weight below which a light's weight rolls to zero, the ray's self-intersection guard, and
 * the two origin-bias terms.
 */

module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.megalights_trace;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the compute pipeline this pass builds

export namespace deren::vulkan::pass {

    /**
     * @brief stochastic punctual lighting: sample a few of the pixel's lights and trace one shadow ray each
     *
     * The frame position is load-bearing: it runs AFTER the G-buffer pass, whose stored surface is what the
     * estimator evaluates its lights against, and BEFORE the deferred lighting stage, which is what ADDS its
     * result (and which therefore must not add the punctual lights itself - see `frame_facts::megalights`
     * and `deferred_frame`'s punctual lane). The renderer owns that order; this pass owns everything inside
     * it.
     */
    class megalights_trace_pass final : public frame_pass {
    public:
        /// @brief the push block, which is also `shaders/megalights_trace.slang`'s
        struct push_constants {
            glm::mat4 inv_view_proj = glm::mat4(1.0f);
            /// x = samples per pixel, y = the minimum sample weight, z = the ray tmin, w = the frame counter
            /// that makes the sample sequence differ every frame
            glm::vec4 params = glm::vec4(4.0f, 0.001f, 0.01f, 0.0f);
            /// x = the origin bias at normal incidence, y = the bias at grazing incidence, zw unused
            glm::vec4 bias = glm::vec4(0.01f, 0.1f, 0.0f, 0.0f);
        };

        megalights_trace_pass() = default;
        ~megalights_trace_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief the estimator's ray budget and cut-off, with their own clamps
        ///
        /// THE PASS'S OWN PARAMETERS, by the rule the framework settled on: one pass reads them, so the pass
        /// owns them and whoever owns the feature forwards. The sample count is clamped to the shader's
        /// compile-time bound (1..4, which is what UE supports and what its `r.MegaLights.NumSamplesPerPixel`
        /// documents), and the weights are clamped non-negative - a negative minimum weight would make the
        /// smooth cut invert, and a negative bias would push the ray's origin INTO the surface.
        void set_estimator(uint32_t samples, float min_weight, float bias_floor, float bias_grazing) noexcept;
        /// @brief the emitter's angular radius in radians: 0 makes the shadows hard (a point light)
        void set_light_angle(float radians) noexcept;

        /// @brief whether the pass built what it records with (the renderer gates the feature on this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question, so an owner holding only a chain can ask it
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        /// @brief the pipeline the runner binds before this pass records
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

        /// @brief the compile-time bound on samples per pixel, which is the shader's own constant
        static constexpr uint32_t max_samples = 4;

    private:
        static constexpr std::string_view shader_name = "megalights_trace.comp.spv";
        static constexpr uint32_t group_size = 8; // `megalights_trace.comp`'s local_size_x/y
        static constexpr uint32_t barrier_output = 0;
        static_assert(barrier_output + 1 == render_resource::megalights_trace_barriers.size(),
                      "the pass's barrier slots must match the declaration it indexes");
        static constexpr std::array<std::string_view, 1> pipeline_names = {"megalights_trace"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::compute,
            .group_size_x = group_size,
            .group_size_y = group_size,
            .group_size_z = 1,
            .extent = extent_rule::half, // the chain's resolution, like the GI tracer's
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = false,
        };
        void release_owned() noexcept;

        // called sample_count, not samples: set_estimator()'s samples parameter would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        uint32_t sample_count = 4;
        // called weight_floor, not min_weight: set_estimator()'s min_weight parameter would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        float weight_floor = 0.001f;
        float tmin = 0.01f;
        // called floor_bias, not bias_floor: set_estimator()'s bias_floor parameter would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        float floor_bias = 0.01f;
        // called grazing_bias, not bias_grazing: set_estimator()'s bias_grazing parameter would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        float grazing_bias = 0.1f;
        /// the emitter's angular radius in radians; 0 makes the shadows hard (see set_light_angle)
        float light_angle = 0.0f;
        /**
         * The ray sequence's frame counter, and it is the PASS's rather than the frame's because this chain
         * has no other reader: the chain's counter lives in the renderer because TWO of its stages trace
         * (two of its stages share one sequence), while here one pass does. What it is FOR is the
         * same in both: a fixed sequence would feed the temporal resolve the same error in the same place
         * every frame instead of an average of different ones.
         */
        uint32_t frame_index = 0;
        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
    };

    static_assert(sizeof(megalights_trace_pass::push_constants) == render_resource::megalights_trace_io.push->size,
                  "the pass's composed push block must be the size its declaration pins");

} // namespace deren::vulkan::pass
