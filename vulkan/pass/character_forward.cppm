// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/character_forward.cppm
 * @brief The TOON CHARACTER stage: the same leaves the scene pass drew, shaded a SECOND time and
 *        written OVER the deferred result.
 * @defgroup vulkan_pass_character_forward Character Forward Pass
 *
 * WHY IT IS ITS OWN PASS, and the reference this follows. The port's reference implementation
 * (DanbaidongRP's PBRToon material family, see _toon_ref/PORT_SPEC.md) draws its character materials in a
 * pass named `CharacterForward` carrying `ZWrite Off` + `ZTest Equal`, invoked after the deferred lighting
 * stage.
 * The two states together are the mechanism: the pass lands on exactly the surface the G-buffer pass
 * recorded, so it REPLACES the lit pixel instead of layering over it - no mask, no stencil, and no
 * double lighting. It cannot be a step inside the lighting instance, because that instance samples the
 * depth this pass needs as an attachment.
 *
 * WHERE IT SITS: after the lighting stage (and after the transparent pass, which composites over the
 * lit frame) and before the resolve. A toon character drawn before the lighting stage would be lit by
 * it, which is the single thing this pass exists to prevent.
 *
 * WHAT IT OWNS: the two targets entered with LOAD, the two barriers that hand the depth between
 * "sampled" and "attachment" layouts (it is what takes it out of SHADER_READ and what puts it back), and
 * the raster state the stage needs - depth compare EQUAL comes from the PIPELINE
 * (`core::make_character_forward_pipeline`); depth WRITE off is a dynamic state this pass records and
 * then LOCKS, because every leaf's draw() otherwise turns it back on (see
 * render_environment::depth_write_locked).
 *
 * IT ALSO DRAWS THE ARTICLE'S TWO OVERLAY MULTIPLIES (the eye shadow and the hair shadow), AFTER the toon
 * leaves and with a SECOND pipeline - see `character_forward_frame::overlay_leaves`. They belong to this
 * stage rather than a pass of their own because they modify exactly what this stage just wrote, in the same
 * instance, on the same depth: the multiply has to see the toon-shaded pixel, and the depth test is what
 * confines the mask to the surface it was authored over.
 *
 * WHAT IT DOES NOT OWN: the geometry. Its leaves are the SCENE's, re-drawn - the renderer hands over the
 * same list the scene pass drew, which is what makes the two agree about which surfaces exist.
 *
 * IT IS BEHIND A FEATURE ("character_forward") THAT DEFAULTS OFF, so a frame from a scene with no toon
 * character is bit-for-bit what it was before this pass existed - the project's acceptance rule for a
 * behaviour-visible addition.
 */

module;

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.character_forward;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.render_resource.shared;
import deren.vulkan.constant_init;
import deren.vulkan.primitive;
import deren.vulkan.render_environment;

export namespace deren::vulkan::pass {

    /// @brief what the renderer hands the character-forward pass: the leaves, the draw state and the formats
    struct character_forward_frame {
        /// the OPAQUE leaves - the same set the scene pass drew, because this pass re-shades those surfaces
        std::span<primitive const* const> leaves = {};
        /**
         * THE OVERLAY LEAVES: the article's two framebuffer multiplies (an eye shadow and a hair shadow), which
         * the renderer has already taken OUT of the opaque/transparent/shadow sets because a mask is not a
         * surface (see `primitive::overlay_kind`).
         *
         * THEY ARE A SECOND LIST AND NOT A PART OF `leaves` because they are drawn differently in three ways at
         * once: a different pipeline (the multiply one, see `overlay_pipeline_name`), AFTER every toon leaf
         * rather than in scene order, and for the hair shadow's mask by geometry alone - this renderer has no
         * stencil attachment for the article's `Stencil { Ref 1 Comp Equal }` to test, which is a recorded
         * difference rather than an oversight (see `core::make_overlay_pipeline`).
         */
        std::span<primitive const* const> overlay_leaves = {};
        /**
         * THE OUTLINE LEAVES: the article's `MyZmdOutlineShader` - the INVERTED HULL, i.e. the character's own
         * leaves drawn a SECOND time with their front faces culled and their vertices pushed outward in clip
         * space, so what survives is the ring just outside each silhouette (the article's ① 描边).
         *
         * THEY ARE A THIRD LIST for the same three reasons the overlay list is a second one, and each is a
         * difference rather than a convenience:
         *   * a different pipeline (`outline_pipeline_name`): CULL FRONT, depth compare LESS_OR_EQUAL, no blend;
         *   * a different ORDER: after every toon leaf, because the hull has to be occluded by the surface the
         *     leaves just re-shaded onto the same depth (the depth test is the only thing that confines a hull
         *     to the outside of its silhouette);
         *   * a per-MATERIAL gate that is not the mesh: a material whose `_OutlineWidth` is 0 (chen's `cloth_02`
         *     is 0.0 in the game's own table) is in `leaves` but NOT here, because the article has no outline for
         *     it - the width rides the material's own colour lane (see `toon_colour_lane::outline_edge`), and the
         *     renderer's filter is what keeps the two lists consistent.
         *
         * DRAWN BEFORE THE OVERLAY GROUP: both belong to the article's character stage and the overlays MULTIPLY
         * what that stage wrote, so the hull is part of what they multiply - which is the order the article's own
         * opaque/multiply split implies.
         */
        std::span<primitive const* const> outline_leaves = {};
        /**
         * Draw state for this session, built by the renderer.
         *
         * A CALLBACK for the same reason the scene and transparent frames' is (the pipeline registry is the
         * renderer's), and the pass then OVERRIDES two things on what it gets back: `default_name` (so a
         * default-semantics leaf binds the character pipeline rather than `pbr`) and the locked depth-write
         * state. Both are fields of the environment rather than callback arguments, which is what lets this
         * pass state its own raster rules without the renderer having to know them.
         */
        render_environment (*make_environment)(void* owner, VkCommandBuffer command_buffer, bool gbuffer) = nullptr;
        void* owner = nullptr;
        /**
         * The pipeline name every leaf of this session binds.
         *
         * THE RENDERER FILLS IT rather than the pass hardcoding it, because the name is the runtime's key
         * into its own registry - the pass would be asserting that some other module registered a pipeline
         * under a string it chose. Empty means the session cannot bind anything and the pass draws nothing.
         */
        std::string_view pipeline_name = {};
        /**
         * The pipeline the OVERLAY group binds, on the same terms (`overlay_pipeline_name` in the runtime).
         *
         * EMPTY MEANS THE OVERLAY GROUP IS NOT DRAWN, and that is the honest answer rather than a fallback: the
         * toon pipeline would not multiply (it OVERWRITES, and its fragment shades a surface), so drawing the
         * masks with it would paint two shaded quads over the face - which is the very defect this pass exists
         * to remove. A device where the overlay pipeline could not be built therefore draws no overlay, and the
         * character looks exactly as it did before the masks were merged into the asset.
         */
        std::string_view overlay_pipeline_name = {};
        /**
         * The pipeline the OUTLINE group binds (`outline_pipeline_name` in the runtime), on the same terms.
         *
         * EMPTY MEANS THE OUTLINE GROUP IS NOT DRAWN, and that is again the honest answer: the toon pipeline
         * neither culls front faces nor tests with LESS_OR_EQUAL, so drawing hulls with it would paint whole
         * shaded surfaces over the character instead of a ring around it. A device where the outline pipeline
         * could not be built therefore draws no outline, which is what the frame looked like before ① landed.
         */
        std::string_view outline_pipeline_name = {};
        /// the two declared targets' formats and the extent (the pass opens its own instance)
        VkFormat color_format = VK_FORMAT_UNDEFINED;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent = {0, 0};
    };

    /**
     * @brief the toon character stage: the scene's own opaque leaves, re-shaded over the lit frame
     *
     * @note the pass draws nothing when its leaf set is empty or the session has no pipeline name. That is
     *       not an optimisation: a frame with no character has no toon surface, and skipping it keeps such
     *       a frame bit-for-bit what it was.
     */
    class character_forward_pass final : public frame_pass {
    public:
        character_forward_pass() = default;
        ~character_forward_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        void set_frame(character_forward_frame const& frame) noexcept;

    private:
        static constexpr std::array<std::string_view, 0> pipeline_names = {}; // the leaves name their pipelines
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::graphics,
            .extent = extent_rule::full,
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = false, // the character pipelines carry their own stored viewport
        };

        // called pass_frame, not frame: set_frame()'s frame parameter in character_forward.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        character_forward_frame pass_frame = {};
    };

} // namespace deren::vulkan::pass
