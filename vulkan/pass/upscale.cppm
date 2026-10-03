// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/upscale.cppm
 * @brief The resolve: the render chain's display-referred LDR image onto the swapchain, by a linear filter.
 * @defgroup vulkan_pass_upscale Upscale Pass
 *
 * WHY IT EXISTS: `core::render_extent()` is the render chain's extent, and below `[render] render_scale = 1.0`
 * the whole chain is created smaller while the swapchain stays at the output size. Nothing resolved that
 * difference, so a scaled frame rendered into the TOP-LEFT QUADRANT of the presented image - the scene was
 * fully shaded, at a lower resolution, and the other three quarters held whatever the allocation had. This
 * pass is that resolve: it reads the LDR image the composite wrote and writes the swapchain, which makes it
 * the frame's LAST writer whenever it runs - and therefore the owner of the debug overlay, exactly as the
 * FXAA pass is when IT is the last writer (the overlay composites a UI with no load op of its own, so it has
 * to be drawn INSIDE whichever instance writes the final image).
 *
 * WHY THE EXTENT IS THE ONE THING THAT MAKES IT WORK, and it is the reason this pass exists as a pass rather
 * than as a line in the composite: its `behaviour` declares `extent_rule::resource` over
 * `resource_id::swapchain_image`, and `runtime::resolve_resource_extent` answers that resource with the
 * OUTPUT extent - the one entry in the table that is not the frame's. So the runner sets this pass's viewport
 * and scissor from the presented image while every other pass in the frame works at the render extent, and
 * the ratio between the two IS the render scale (nothing in this pass has to be told what it is).
 *
 * WHY IT IS NOT FXAA, AND WHY THE TWO CANNOT BOTH RUN: FXAA is an edge filter whose relative luma thresholds
 * are defined on display-referred data AT THE RESOLUTION IT FILTERS, and its input is the same LDR image this
 * pass reads - but it writes the swapchain directly, at the render extent, so on a scaled frame its output
 * would be resolved away by this pass (or, if it ran first, this pass would resample an already-filtered
 * image at the wrong size). Both want the same two things, so `runtime::post_fxaa_active` excludes the frames
 * this pass runs on, and the feature registry, the composite's target choice and the overlay's owner all read
 * that one answer (see `render_features::upscale`).
 *
 * WHAT IT OWNS: its pipeline, the LDR image's transition to a sampled layout, the clear instance over the
 * swapchain, the push block's `encode_gamma` lane and the draw. WHAT IT DOES NOT OWN: the LDR image it reads
 * (a per-swapchain-image heap slot the shader names itself, from the lane the framework appends) and the
 * composite that wrote it - the composite decides to write the LDR image instead of the swapchain from the
 * same `frame_facts` predicate that gates this pass (see post_composite_pass::prepare_frame).
 *
 * THE SHAPE IS FXAA'S (the last-writer template) WITH `toon_screen_rim`'s ONE DIFFERENCE: this pass declares
 * a barrier image, because it is the pass that hands the LDR image to a sampler - the same argument FXAA's
 * declaration makes, and for the same reason (the composite cannot read the image it renders into).
 */

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.upscale;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the pipeline this pass builds

export namespace deren::vulkan::pass {

    /// @brief what this pass's frame carries: the overlay's draw, and nothing else
    struct upscale_frame {
        /**
         * The debug overlay's draw, recorded INSIDE this pass's rendering instance between its draw and
         * `vkCmdEndRendering`.
         *
         * WHY A CALLBACK AND NOT A PASS (the same decision fxaa_frame records, from the other side): the
         * overlay has no load op, so a pass of its own would CLEAR the image it is supposed to draw over. It
         * belongs to whichever instance is the frame's LAST writer, and this pass is that instance whenever
         * it runs - so the pass installs the host's hook (see `set_overlay`) and always draws it, while the
         * composite's own frame decides per frame that the overlay is NEITHER this pass's nor the FXAA pass's
         * business (see post_composite_pass::prepare_frame).
         */
        draw_callback after_draw = {};
    };

    /**
     * @brief WHICH FILTER resolves the render chain onto the output
     *
     * Two, and the naive one is kept on purpose rather than deleted once FSR landed: an upscaler's whole claim
     * is "closer to the native-resolution frame than the cheap answer", and that claim is a MEASUREMENT between
     * these two modes (`build-release-clang64/upscale_quality.py`: mean absolute difference against a
     * `render_scale = 1.0` capture of the same scene, and the mean absolute Laplacian of luma as sharpness).
     * A mode deleted after the fact would take its own baseline with it.
     */
    enum class upscale_filter : uint8_t {
        linear, // one tap of the shared linear sampler per output pixel: a bilinear resample of the whole chain
        easu,   // FSR 1's edge-adaptive spatial upsampling (see shaders/upscale.slang's port and its license note)
    };

    /// @brief EASU's four constants, as the shader's push block carries them
    /// @note a free function rather than a lambda inside `record`: it is `FsrEasuCon` ported (see
    ///       shaders/upscale.slang), it is pure arithmetic on two extents, and that makes it pinnable by a test
    ///       instead of only observable through a rendered frame - a transposed or half-pixel-shifted constant
    ///       still produces a plausible image.
    struct easu_constants {
        float con0[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float con1[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float con2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float con3[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    /**
     * @brief `FsrEasuCon` (AMD's, v1.20210629): the constants EASU's kernel needs, from two extents
     * @param render_width/height the RENDER chain's extent - AMD's `inputViewportInPixels` AND
     *        `inputSizeInPixels`, equal here because this renderer has no dynamic-resolution offset region
     * @param output_width/height the swapchain's extent, i.e. `outputSizeInPixels`
     */
    [[nodiscard]] easu_constants make_easu_constants(uint32_t render_width, uint32_t render_height, uint32_t output_width, uint32_t output_height) noexcept;

    /**
     * @brief the resolve: the display-referred LDR image -> the swapchain, by the configured filter
     *
     * The frame's last writer and its overlay's owner whenever the render chain runs below the output size.
     */
    class upscale_pass final : public frame_pass {
    public:
        /// @brief the push block, which is also `upscale.slang`'s - EASU's four constants, the filter and the transfer
        /// @note THE TWO HEAP LANES ARE APPENDED BY THE FRAMEWORK and are therefore NOT fields here (see the
        ///       shader's note and `runtime::push_stage_block`): the shader's block is this struct plus
        ///       `uint frame_slot; uint image_index;`, and a field added here for either would make the host
        ///       append its own values PAST them.
        struct push_constants {
            /// EASU's four constants - `FsrEasuCon`'s con0..con3, computed by `make_easu_constants` below from
            /// the RENDER extent (input viewport and input size) and the OUTPUT extent. `float[4]` rather than a
            /// graphics-math vector so that sixteen floats do not put a glm dependency into a pass module; the
            /// shader reads them as a `float4` each, and the layout is four tightly packed vectors either way.
            float con0[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float con1[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float con2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float con3[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float mode = 0.0f;         // the filter: 0 = linear, 1 = EASU (see upscale_filter)
            float encode_gamma = 0.0f; // 0 = the swapchain is an sRGB format and encodes on write, 1 = it does not
        };

        upscale_pass() = default;
        ~upscale_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief whether the pass built the pipeline it records with (the renderer gates the feature on this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question, so an owner holding only a chain can ask it
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        /// @brief the pipeline the runner binds before this pass records
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

        /// @brief install the host's overlay hook (this pass is the frame's last writer whenever it runs)
        void set_overlay(draw_callback overlay) noexcept;
        /// @brief which filter to resolve with; a STARTUP value the owner sets from the config, because it
        ///        selects a shader path rather than a per-frame quantity
        void set_filter(upscale_filter filter) noexcept;
        /// @brief the filter in force (a test asks; the shader is told through the push block)
        [[nodiscard]] upscale_filter filter() const noexcept;
        /// @brief build this pass's frame from the published facts (see frame_pass::prepare_frame)
        void prepare_frame(frame_facts const& facts) noexcept override;
        /// @brief the frame for this stage; the pass composes it itself now (see frame_pass::prepare_frame),
        ///        and the setter stays for a test that wants to hand one over directly
        void set_frame(upscale_frame const& frame) noexcept;

    private:
        static constexpr std::string_view vertex_shader_name = "post.vert.spv"; // the synthetic fullscreen triangle
        static constexpr std::string_view fragment_shader_name = "upscale.frag.spv";

        static constexpr std::array<std::string_view, 1> pipeline_names = {"upscale"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::fullscreen,
            // THE ONE PASS IN THE FRAME WHOSE EXTENT IS NOT THE FRAME'S: the resource rule over the swapchain
            // image resolves to the OUTPUT extent (see runtime::resolve_resource_extent), which is what makes
            // the viewport cover the whole presented image while the chain being sampled stays smaller.
            .extent = extent_rule::resource,
            .extent_of = resource_id::swapchain_image,
            .pipelines = pipeline_names,
            .resync_viewport = true, // the runner sets the viewport and scissor from io.extent
        };
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
        /// the surface's format, cached at create: the push block's `encode_gamma` lane follows from it, and a
        /// session-stable device fact is exactly what a create step may keep (see the FXAA pass, which does the
        /// same for the same lane)
        VkFormat swap_chain_format = VK_FORMAT_UNDEFINED;
        // called overlay_callback, not overlay: set_overlay()'s overlay parameter in upscale.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        /// the host's overlay hook, installed once (see set_overlay): this pass draws it whenever it runs
        draw_callback overlay_callback = {};
        // called pass_frame, not frame: set_frame()'s frame parameter in upscale.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        upscale_frame pass_frame = {};
        // called filter_kind, not filter: filter() and set_filter()'s filter parameter would hide it, and
        // MSVC /W4 reports C4458 (an error under /WX).
        /// EASU by DEFAULT, which is also `[render] upscale`'s default: a frame that renders below the output
        /// size is asking for a resolve, and the whole reason this pass exists is FSR's upscaler. The linear
        /// mode is the reference it is measured against, not the thing to fall back to.
        upscale_filter filter_kind = upscale_filter::easu;
    };

    /// THE DECLARATION'S NUMBER AND THE PASS'S STRUCT CANNOT DRIFT: the declaration's `push` size is this
    /// struct, and the framework appends the two heap index lanes on top of it - which is the size the shader
    /// declares as its own block.
    static_assert(sizeof(upscale_pass::push_constants) == render_resource::upscale_io.push->size,
                  "the upscale pass's declared push block must be the size of the struct the pass composes");

} // namespace deren::vulkan::pass
