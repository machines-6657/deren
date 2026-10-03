// module version: 0.4.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/fxaa.cppm
 * @brief The fourteenth real pass: the FXAA resolve, the frame's LAST writer whenever it runs.
 * @defgroup vulkan_pass_fxaa FXAA Pass
 *
 * WHY IT IS THE POST CHAIN'S LAST PASS, and why that decides everything about it: the anti-aliasing filter reads
 * the image the composite produced, so the composite cannot write the swapchain when FXAA is on (a pass may not
 * read the image it renders into) - it writes the R16F LDR image instead, gamma-encoded, because FXAA's luma
 * thresholds are defined on display-referred data. This pass then turns that image into the presented one, which
 * makes it the frame's last writer, which in turn makes it the owner of the debug overlay: the overlay composites
 * a UI with no load op of its own, so it has to be drawn INSIDE whichever instance writes the final image - and
 * that is this one whenever it runs (the composite carries it otherwise; see its frame's `after_draw`).
 *
 * WHAT IT OWNS: its pipeline - the frame's LDR image transition, the clear instance over the swapchain, the push
 * block's mode lane and the draw are its recording. WHAT IT DOES NOT OWN: the push block's SHAPE (deren.vulkan.pass.post's
 * `post_push_constants` - FXAA is mode 3 of the same shader's block, so the struct is imported rather than copied: a
 * second copy of those thirteen lanes is a second thing that can drift) and the LDR image it reads, which is a heap
 * slot the shader names itself.
 *
 * IT IS ALSO THE PASS THAT MADE `vk_pipeline::begin_pipeline` unnecessary for the post chain: the runner binds the
 * pipeline from `behaviour::pipelines` and sets the viewport from the declaration's extent, so the pass sets only
 * the cull mode, and the renderer's `record_fullscreen_triangle` helper (whose last caller this was) is gone with
 * it.
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

export module deren.vulkan.pass.fxaa;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.pass.post;    // post_push_constants: the chain's ONE block, which this pass is mode 3 of
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the pipeline this pass builds

export namespace deren::vulkan::pass {

    /// @brief what this pass's frame carries: the overlay's draw, and nothing else
    struct fxaa_frame {
        /**
         * The debug overlay's draw, recorded INSIDE this pass's rendering instance between its draw and
         * `vkCmdEndRendering`.
         *
         * WHY A CALLBACK AND NOT A PASS (the same decision the composite's frame records, from the other side):
         * the overlay has no load op, so a pass of its own would CLEAR the image it is supposed to draw over. It
         * belongs to whichever instance is the frame's LAST writer, and this pass is that instance whenever it
         * runs - so the pass installs the host's hook (see `set_overlay`) and always draws it, while the
         * composite's own frame decides per frame that the overlay is THIS pass's.
         */
        draw_callback after_draw = {};
    };

    /**
     * @brief the FXAA resolve: the gamma-encoded LDR image -> the anti-aliased swapchain image
     *
     * The frame's last writer, its overlay's owner, and the reason the composite has two pipeline variants.
     */
    class fxaa_pass final : public frame_pass {
    public:
        fxaa_pass() = default;
        ~fxaa_pass() override;

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

        /// @brief install the host's overlay hook (this pass is the frame's last writer whenever it runs)
        void set_overlay(draw_callback overlay) noexcept;
        /// @brief build this pass's frame from the published facts (see frame_pass::prepare_frame)
        void prepare_frame(frame_facts const& facts) noexcept override;
        /// @brief the frame for this stage; the pass composes it itself now (see frame_pass::prepare_frame),
        ///        and the setter stays for a test that wants to hand one over directly
        void set_frame(fxaa_frame const& frame) noexcept;

    private:
        static constexpr std::string_view vertex_shader_name = "post.vert.spv"; // the synthetic fullscreen triangle
        static constexpr std::string_view fragment_shader_name = "fxaa.frag.spv";

        static constexpr std::array<std::string_view, 1> pipeline_names = {"fxaa"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::fullscreen,
            .extent = extent_rule::full, // the filter runs at the frame's resolution
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = true, // the runner sets the viewport and scissor from io.extent
        };
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
        /// the surface's format, cached at create: the push block's `encode_gamma` lane follows from it, and a
        /// session-stable device fact is exactly what a create step may keep (see the composite, which does the
        /// same for its own frame's target choice)
        VkFormat swap_chain_format = VK_FORMAT_UNDEFINED;
        // called overlay_callback, not overlay: set_overlay()'s overlay parameter in fxaa.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        /// the host's overlay hook, installed once (see set_overlay): this pass draws it whenever it runs
        draw_callback overlay_callback = {};
        // called pass_frame, not frame: set_frame()'s frame parameter in fxaa.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        fxaa_frame pass_frame = {};
    };

    /// THE DECLARATION'S NUMBER AND THE PASS'S STRUCT CANNOT DRIFT, and here the struct is the post chain's: FXAA
    /// is mode 3 of the same shader's push block, so this asserts the two the host composes into and the pass
    /// pushes are the same size.
    static_assert(sizeof(post_push_constants) == render_resource::fxaa_io.push->size,
                  "the FXAA pass's declared push block must be the size of the post chain's block it pushes");

} // namespace deren::vulkan::pass
