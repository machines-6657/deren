// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/megalights_temporal.cppm
 * @brief The stochastic lighting chain's temporal resolve: a running mean with a per-pixel frame count.
 * @defgroup vulkan_pass_megalights_temporal Megalights Temporal Pass
 *
 * WHAT IT OWNS: the frame's recording (the barriers around its accumulation and the copy that becomes the
 * next frame's history) and the accumulation POLICY, which is the point of the pass: the frame count
 * and the depth tolerance are its own parameters, with the argument for each of them in
 * `shaders/megalights_temporal.slang` and in `docs/reference/megalights_stochastic_lighting.md` (section 4 is
 * Unreal's policy, which this is ported from).
 *
 * THE IMAGES IT READS AND WRITES ARE HEAP SLOTS: the three images of the chain, the G-buffer velocity it
 * reprojects with and the depth it rejects the history against are all named by the shader itself, with the
 * swapchain image index its push block carries - so the pass binds no set and owns no family. Nothing
 * about the frame's ordering is delegated either: the two G-buffer transitions are its own barriers, which is
 * why this chain needs no second stage.
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

export module deren.vulkan.pass.megalights_temporal;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the compute pipeline this pass builds

export namespace deren::vulkan::pass {

    /// @brief what the renderer tells the resolve: whether this image's accumulation exists yet
    struct megalights_temporal_frame {
        /**
         * Whether the previous frame's accumulation for THIS image may be read at all. False on the first frame
         * of a generation (a resize destroys the histories) and false for a frame the resolve itself did not
         * record - and the difference matters: with it false the pass writes this frame's estimate with a frame
         * count of 1, which is what makes a restarted accumulation converge from one sample rather than from
         * whatever the allocation held.
         */
        bool history_valid = false;
    };

    /**
     * @brief the temporal resolve: average this frame's samples with the frames before them
     *
     * It runs immediately after the tracer, inside the same stage, and its OUTPUT is what the lighting stage
     * samples - so `feature()` is the chain's feature and the renderer's predicate for the whole chain
     * includes this pass having built its pipeline.
     */
    class megalights_temporal_pass final : public frame_pass {
    public:
        /// @brief the push block, which is also `shaders/megalights_temporal.slang`'s
        struct push_constants {
            /// x = proj[2][2], y = proj[3][2], z = the frame-count cap, w = the relative depth tolerance
            glm::vec4 params = glm::vec4(0.0f, 0.0f, 12.0f, 0.03f);
            /// xy = the half-resolution extent, zw = the full-resolution extent (the clamp's tap stride)
            glm::vec4 extents = glm::vec4(0.0f);
        };

        megalights_temporal_pass() = default;
        ~megalights_temporal_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief the accumulation's policy: the frame-count cap and the history's relative depth tolerance
        ///
        /// THE PASS'S OWN PARAMETERS, by the rule the framework settled on. Both are clamped where they are set
        /// because both have a meaning that a wrong value destroys rather than merely changes: a cap below 1
        /// would make the running mean divide by zero, and a tolerance of 0 would reject every history (the
        /// accumulation would never grow past one frame).
        void set_accumulation(float depth_tolerance, float max_frames) noexcept;

        /// @brief the spatial pre-filter's width in GI texels (0 = off, which makes the pass temporal-only)
        ///
        /// THE PASS'S OWN PARAMETER, like the two above, and clamped for the same kind of reason: a negative
        /// width would invert the Gaussian's falloff, and past a few texels the filter is wider than the
        /// neighbourhood it can read (the same argument the GI spatial filter's own clamp makes).
        void set_spatial(float sigma) noexcept;

        /// @brief the two lanes a caller that moves one value has to restate (see the demo's setter)
        [[nodiscard]] float max_frames() const noexcept {
            return this->accumulation_frames;
        }
        [[nodiscard]] float spatial_sigma() const noexcept {
            return this->denoise_sigma;
        }

        /// @brief this frame's answer about the history (see megalights_temporal_frame)
        void set_frame(megalights_temporal_frame const& frame) noexcept;
        /// @brief build this pass's frame from the published facts (see frame_pass::prepare_frame)
        void prepare_frame(frame_facts const& facts) noexcept override;

        /// @brief whether the resolve wrote its image this frame, i.e. whether the lighting stage may add it
        [[nodiscard]] bool resolved() const noexcept;

        /// @brief the framework's generic form of the same question
        [[nodiscard]] bool ready() const noexcept override;
        /// @brief the pipeline the runner binds before this pass records
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

        /// @brief the compile-time bound on how many frames the running mean may average
        static constexpr float max_frames_limit = 64.0f;

    private:
        static constexpr std::string_view shader_name = "megalights_temporal.comp.spv";
        static constexpr uint32_t group_size = 8; // `megalights_temporal.comp`'s local_size_x/y
        static constexpr uint32_t barrier_resolve = 0;
        static constexpr uint32_t barrier_history = 1;
        static_assert(barrier_history + 1 + 2 == render_resource::megalights_temporal_barriers.size(),
                      "the resolve's barrier slots must match the declaration it indexes");
        static constexpr std::array<std::string_view, 1> pipeline_names = {"megalights_temporal"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::compute,
            .group_size_x = group_size,
            .group_size_y = group_size,
            .group_size_z = 1,
            .extent = extent_rule::half, // the chain's resolution, like the tracer's
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = false,
        };
        void release_owned() noexcept;

        // called accumulation_resolved, not resolved: the class declares resolved() and a member of that
        // name would duplicate it and hide the override.
        bool accumulation_resolved = false;
        // called temporal_depth_tolerance, not depth_tolerance: set_accumulation()'s depth_tolerance parameter would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        float temporal_depth_tolerance = 0.03f;
        // called accumulation_frames, not max_frames: max_frames() and set_accumulation()'s max_frames
        // parameter would hide it, and MSVC /W4 reports C4458 (an error under /WX).
        float accumulation_frames = 12.0f;
        // called denoise_sigma, not spatial_sigma: the class declares spatial_sigma() and a member of that
        // name would duplicate it and hide the override.
        float denoise_sigma = 1.5f;
        // called pass_frame, not frame: set_frame()'s frame parameter in megalights_temporal.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        megalights_temporal_frame pass_frame = {};
        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
    };

    static_assert(sizeof(megalights_temporal_pass::push_constants) == render_resource::megalights_temporal_io.push->size,
                  "the resolve's composed push block must be the size its declaration pins");

} // namespace deren::vulkan::pass
