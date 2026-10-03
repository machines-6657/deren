// module version: 0.3.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/geometry_buffer_debug.cppm
 * @brief The fifteenth real pass: the G-buffer debug view, which shows the stored surface one channel at a time.
 * @defgroup vulkan_pass_gbuffer_debug G-buffer Debug View Pass
 *
 * WHY IT IS A PASS AND WHAT IT OWNS: the view is a fullscreen triangle over the HDR target, so it owns its
 * pipeline and its recording - the four images it moves to a sampled layout, the CLEAR instance, the 16-byte push
 * and the draw. IT IS ALSO THE PASS THAT WRITES THE HEAP SLOTS the view reads, and the deferred lighting stage
 * reads its own through the same heap: a shader names the slot and the frame binds the heap once (see
 * shaders/heap_slots.glsl), so there is no set layout to own and no family to write.
 *
 * WHAT IT DOES NOT OWN, and each is a shared thing rather than an omission: the stored surface itself, which is a
 * per-image heap slot `publish_frame_resources` writes every frame; the two samplers, which a declaration picks by
 * hint and the heap carries; and two pieces of per-image bookkeeping the frame carries as a callback - the
 * G-buffer depth's transition (its old layout depends on whether the G-buffer instance rendered this frame) and
 * clearing the flag that says the motion-vector target has been handed to a sampler (which this pass does, because
 * it runs INSTEAD of the lighting stage and is the only stage that samples that image on those frames).
 *
 * THE HDR TARGET'S TRANSITION IS THE FRAME LOOP'S, for the same reason the post chain's is: it has to happen even
 * on a frame where nothing could run, because the post chain samples that image - and it is the frame loop
 * that owns the fallback (a cleared instance, which is what makes such a frame black rather than undefined).
 */

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.geometry_buffer_debug;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the pipeline this pass builds

export namespace deren::vulkan::pass {

    /// @brief what the renderer hands the view: the two per-image pieces of bookkeeping it cannot own
    /**
     * @brief what the renderer hands the G-buffer debug view: NOTHING, and that is the endpoint
     *
     * The two per-image transitions this frame used to carry (`ensure_inputs`: the depth's hand-back and clearing
     * the motion-vector flag) are the frame's ORDERING rules about images the G-buffer pass wrote, so they run in
     * the debug stage's preamble in the renderer - the same move the ray-traced shadow pass's identical pair made.
     * What is left for this pass to know is its own channel (its parameter) and the frame's facts, so the struct
     * is gone rather than emptied.
     */

    /**
     * @brief the G-buffer debug view: one stored channel, displayed in the HDR target
     *
     * It runs INSTEAD of the deferred lighting stage (the two write the HDR target in incompatible ways), which is
     * what makes it the frame's only sampler of the motion-vector target and what keeps it off the TAA path.
     */
    class gbuffer_debug_pass final : public frame_pass {
    public:
        /// @brief how many stored channels the view can show: the SHADER's own switch (`gbuffer_debug.frag`), which
        ///        is why the count lives with the pass rather than with the renderer that offers a slider for it
        static constexpr int32_t channel_count = 9;
        /// @brief the push block, which is also `gbuffer_debug.frag`'s
        struct push_constants {
            float channel = 1.0f; // 0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 id, 6 depth, 7 flags, 8 motion
            float proj_22 = 0.0f; // projection[2][2] / [3][2]: the depth-linearization terms
            float proj_32 = 0.0f;
            // Amplification for the motion channel only. The stored vector is a UV-space delta, so a pixel of motion
            // at 1080 wide is 0.00093 and the raw value would be black everywhere; the renderer sets this to
            // width/4 so four pixels saturate the channel, which scales itself across resolutions instead of being
            // a magic number per window size.
            float motion_gain = 1.0f;
        };

        gbuffer_debug_pass() = default;
        ~gbuffer_debug_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief whether the pass built everything it records with (the renderer gates the feature on this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question, so an owner holding only a chain can ask it
        ///        (a pass with nothing of its own to build keeps the interface's `true`; see `frame_pass::ready`)
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        /// @brief the pipeline the runner binds before this pass records
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

        /**
         * @brief which stored channel the view shows, which is THIS pass's parameter
         *
         * The knob used to live in the renderer, which clamped it and copied it into the push block; the pass owns
         * it now (with the clamp) and `runtime::set_gbuffer_channel` forwards. There is no frame any more: after the
         * push block moved here the pass needs nothing per-frame except the frame's own facts.
         * @param channel 0 albedo, 1 normal, 2 roughness, 3 metallic, 4 ao, 5 id, 6 depth, 7 flags, 8 motion
         */
        void set_channel(int32_t channel) noexcept;
        /// @brief the channel the view shows (0..gbuffer_channel_count-1)
        [[nodiscard]] int32_t channel() const noexcept;

    private:
        static constexpr std::string_view vertex_shader_name = "post.vert.spv"; // the synthetic fullscreen triangle
        static constexpr std::string_view fragment_shader_name = "gbuffer_debug.frag.spv";

        static constexpr std::array<std::string_view, 1> pipeline_names = {"gbuffer-debug"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::fullscreen,
            .extent = extent_rule::full, // the view runs at the frame's resolution
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = true, // the runner sets the viewport and scissor from io.extent
        };
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
        // called debug_channel, not channel: channel() and set_channel()'s channel parameter would hide it,
        // and MSVC /W4 reports C4458 (an error under /WX).
        /// the channel the view shows (see set_channel); the default is the renderer's historical normal channel
        int32_t debug_channel = 1;
    };

    static_assert(sizeof(gbuffer_debug_pass::push_constants) == render_resource::gbuffer_debug_io.push->size,
                  "the debug view's declared push block must be the size of the struct the renderer composes");

} // namespace deren::vulkan::pass
