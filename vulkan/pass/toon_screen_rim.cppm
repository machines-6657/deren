// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/toon_screen_rim.cppm
 * @brief The SCREEN-SPACE DEPTH RIM: a contour drawn where the shaded character meets something nearer.
 * @defgroup vulkan_pass_toon_screen_rim Toon Screen-Space Rim Pass
 *
 * WHY IT EXISTS: the character-forward stage's rim is a VIEW-ANGLE term, so it appears where the surface
 * turns away from the camera. It cannot draw a contour where the surface faces the camera and is simply
 * OCCLUDED by nearer geometry - an arm across a torso, a chin over a collar. That is what this pass adds, and
 * both references have the same second term: the MME pack's `Draw*ScreenRim` passes and the reference's
 * `GetCharacterDirectRimLightArea`, both comparing the depth here against the depth at a screen offset.
 *
 * WHY IT IS A PASS AND NOT A STEP IN THE CHARACTER-FORWARD STAGE - the Vulkan rule, not a preference: that
 * stage holds the depth image as its DEPTH ATTACHMENT, which is what makes its `ZTest Equal` possible, and an
 * image cannot be a depth attachment and a sampled texture in the same instance. Here the depth is only
 * SAMPLED: this pass declares NO depth attachment, so the comparison is legal. It needs no depth COPY, which
 * the reference has only because its depth attachment is the sole copy it owns - this renderer publishes
 * `gbuffer_depth` as a heap slot a fullscreen stage can read, and the stage preamble below is what puts it in
 * a sampled layout.
 *
 * WHAT IT OWNS: its pipeline (the ONE target is the scene colour with ADDITIVE blending, so the pass adds to
 * the frame instead of reading and rewriting it), the 48-byte push block, and the draw.
 *
 * WHAT IT DOES NOT OWN: the geometry (there is none - it is a fullscreen triangle), the G-buffer and the
 * depth (per-image heap slots `publish_frame_resources` writes every frame), and their transition to a
 * sampled layout, which is the RENDERER's per-image bookkeeping because it belongs to the pass that WROTE
 * them - the same rule the ray-traced shadow and the stochastic lighting stages follow.
 *
 * IT IS GATED BY THE `character_forward` FEATURE, not by one of its own: the contour belongs to the toon
 * character stage and means nothing without it, so the two are one feature with one switch. That is also what
 * makes every capture scenario byte-identical while the feature is off - the runner asks this pass's
 * `feature()` before it resolves anything.
 */

module;

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.toon_screen_rim;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the pipeline this pass builds

export namespace deren::vulkan::pass {

    /**
     * @brief the screen-space depth rim: a fullscreen additive contour over the shaded frame
     *
     * It has no frame of its own - every value it needs is either the frame's (the two projection terms) or
     * its own parameter (the rim's width, gain, strength and colour) - so it follows `deferred` and
     * `gbuffer-debug` in taking its parameters from the pass and nothing from the renderer.
     */
    class toon_screen_rim_pass final : public frame_pass {
    public:
        /// @brief the push block, which is also `toon_screen_rim.slang`'s
        struct push_constants {
            float proj_22 = 0.0f; // the depth-linearization terms, from the frame's projection
            float proj_32 = 0.0f;
            float rim_width = 0.0f;    // how far along the screen the depth comparison reaches
            float rim_scale = 0.0f;    // the gain on the depth difference
            float rim_strength = 0.0f; // 0 draws nothing at all
            float pad0 = 0.0f;
            float pad1 = 0.0f;
            float pad2 = 0.0f;
            float rim_colour[4] = {1.0f, 0.94f, 0.90f, 1.0f};
            // THE HEAP INDICES ARE APPENDED BY THE FRAMEWORK, not composed here (see the shader's note and
            // gbuffer_debug's identical one) - so this struct is the PASS's block and nothing more.
        };

        toon_screen_rim_pass() = default;
        ~toon_screen_rim_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief whether the pass built the pipeline it records with (the renderer's feature gate asks this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        /// @brief the pipeline the runner binds before this pass records
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

        /**
         * @brief the rim's shape, which is THIS pass's parameter
         * @param width the screen-offset scale (the reference's `rimWidth`, in its own units)
         * @param scale the gain on the depth difference (the reference's `* 4`)
         * @param strength 0 draws nothing, which is how the feature's off state costs nothing
         */
        void set_shape(float width, float scale, float strength) noexcept;
        /// @brief the contour's colour (the reference's `EF_*_RIM_COLOR`)
        void set_colour(float r, float g, float b) noexcept;

    private:
        static constexpr std::string_view vertex_shader_name = "post.vert.spv"; // the synthetic fullscreen triangle
        static constexpr std::string_view fragment_shader_name = "toon_screen_rim.frag.spv";

        static constexpr std::array<std::string_view, 1> pipeline_names = {"toon-screen-rim"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::fullscreen,
            .extent = extent_rule::full, // the contour is drawn at the frame's resolution
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = true, // the runner sets the viewport and scissor from io.extent
        };
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
        float rim_width = 1.0f;
        float rim_scale = 4.0f;
        float rim_strength = 0.55f;
        float rim_colour[3] = {1.0f, 0.94f, 0.90f};
    };

    /// the pass's own block: the eight composed values plus the colour, and the framework appends the two heap
    /// lanes on top (see the shader).
    static_assert(sizeof(toon_screen_rim_pass::push_constants) == render_resource::toon_screen_rim_io.push->size,
                  "the rim's declared push block must be the size of the struct the pass composes");

} // namespace deren::vulkan::pass
