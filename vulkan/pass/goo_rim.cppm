// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/goo_rim.cppm
 * @brief THE REWRITTEN TOON CHAIN'S EDGE LIGHT as a fullscreen pass: the Goo reference's whole rim, screen-space
 *        `DepthRim` included, ADDED over the frame the character stage wrote.
 * @defgroup vulkan_pass_goo_rim Goo Rim Pass
 *
 * WHY IT EXISTS AT ALL, and it is a verified constraint rather than a preference:
 * `vulkan/render_resource/render_resource.cppm`'s `character_forward_targets = {scene_color, gbuffer_depth}` makes
 * the depth an ATTACHMENT of the pass whose fragment shades the surface, so `shaders/goo_toon.slang` cannot sample
 * it - and the reference's rim is a PRODUCT (`rim_term = C ⊙ (D · F · V · depth_factor)`) whose factor needs that
 * sample. A product cannot be split across two passes (split, it is `C·D·F·V + mask`), so the WHOLE rim moves out
 * of the surface shader and is recomposed here, where the depth is only ever SAMPLED.
 *
 * THE SHAPE IS `toon_screen_rim`'s, deliberately: one additive target, no depth attachment, no own binding (every
 * input is a per-image heap slot the shader indexes with the image index its push block carries), and a push block
 * of the frame's own constants plus this pass's nothing-at-all - this pass has no parameter of its own, because
 * every number in the rim is either the reference's (a constant in the shader) or the MATERIAL's (a colour lane the
 * shader reads through the G-buffer's material id).
 *
 * WHAT IT OWNS: its pipeline and the record. WHAT IT DOES NOT OWN: the geometry (a fullscreen triangle), the
 * G-buffer and the depth (the frame's per-image heap slots), and their transition to a sampled layout, which is
 * the RENDERER's per-image bookkeeping because it belongs to the pass that wrote them - the stage preamble below
 * is what publishes them, exactly as the article's contour stage does.
 *
 * IT IS GATED BY THE REWRITTEN CHAIN, NOT BY ITS OWN FEATURE NAME, and it is the one pass in this renderer whose
 * `feature()` names a flag the owner composes out of `runtime::goo_toon_active()`: with `[render] goo_toon` false
 * the pass is never resolved and never recorded, which is what keeps every existing capture scenario byte-identical
 * (the runner asks `feature()` before it resolves the declaration, so the skip costs no barrier and no resolve).
 * The owner's table owns that answer because the same predicate picks the character stage's pipeline, and a gate
 * that could disagree with the pipeline choice would either draw a Goo rim over the OLD chain's frame or draw none
 * over the new one.
 */

module;

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.goo_rim;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the pipeline this pass builds

export namespace deren::vulkan::pass {

    /**
     * @brief the rewritten toon chain's rim: a fullscreen additive stage over the shaded frame
     *
     * It has no frame of its own - every value it needs is the frame's (the inverse view-projection and the two
     * depth-linearization terms) and the rest is the material's, read from the G-buffer - so it follows `deferred`
     * and `toon_screen_rim` in taking its parameters from the pass and nothing from the renderer.
     */
    class goo_rim_pass final : public frame_pass {
    public:
        /// @brief the push block, which is also `goo_rim.slang`'s first three fields
        struct push_constants {
            glm::mat4 inv_view_proj = glm::mat4(1.0f); // clip -> world, for the pixel's own depth
            float proj_22 = 0.0f;                      // projection[2][2] / [3][2]: what `-get_view_z_from_depth`
            float proj_32 = 0.0f;                      // ... (the engine's `rim_view_depth`) is built from
            // THE HEAP INDICES ARE APPENDED BY THE FRAMEWORK, not composed here (see the shader's note and
            // gbuffer_debug's identical one) - so this struct is the PASS's block and nothing more:
            // 64 + 4 + 4 = 72 bytes, which is 56 under the 128-byte guaranteed minimum.
        };

        goo_rim_pass() = default;
        ~goo_rim_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief whether the pass built the pipeline it records with (the owner's feature gate asks this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        /// @brief the pipeline the runner binds before this pass records
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

    private:
        static constexpr std::string_view vertex_shader_name = "post.vert.spv"; // the synthetic fullscreen triangle
        static constexpr std::string_view fragment_shader_name = "goo_rim.frag.spv";

        static constexpr std::array<std::string_view, 1> pipeline_names = {"goo-rim"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::fullscreen,
            .extent = extent_rule::full, // the rim is drawn at the frame's resolution
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = true, // the runner sets the viewport and scissor from io.extent
        };
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
    };

    /// the pass's own block: the frame's inverse view-projection and its two depth-linearization terms, and the
    /// framework appends the two heap lanes on top (see the shader).
    static_assert(sizeof(goo_rim_pass::push_constants) == render_resource::goo_rim_io.push->size,
                  "the goo rim's declared push block must be the size of the struct the pass composes");

} // namespace deren::vulkan::pass
