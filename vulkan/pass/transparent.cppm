// module version: 0.1.1  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/transparent.cppm
 * @brief The FOURTH real pass: the alpha-blended geometry, composited over the shaded frame.
 * @defgroup vulkan_pass_transparent Transparent Pass
 *
 * WHY IT IS ITS OWN PASS AND NOT PART OF THE SCENE PASS, even though both draw leaves: they are two different
 * INSTANCES with incompatible attachments. The scene pass writes the surface (its depth is an attachment it
 * CLEARs); this one LOADs the shaded colour and the surface depth and blends over them, so it cannot run inside
 * that instance - an image cannot be a sampled texture and a depth attachment at the same time, which is
 * exactly why the lighting stage (which samples the depth) and this pass (which depth-tests against it) are
 * separate instances too.
 *
 * WHAT IT OWNS: its two declared targets entered with LOAD, the per-slot secondary its leaves are recorded
 * into, the four barriers that hand the depth between "sampled" and "attachment" layouts (this pass is what
 * takes it out of SHADER_READ and what puts it back), and the draw order - the leaves arrive already sorted far
 * to near, which is the order alpha blending needs.
 *
 * WHAT IT USES, exactly like the scene pass and for the same reasons: the pipeline registry (through the draw
 * state the renderer builds), the shared scene block, the per-slot secondary command buffer, and the leaves
 * themselves. Its input is the same `scene_frame` shape with three fields different (one segment, the forward
 * default, one colour format).
 */

module;

#include <cstdint>
#include <span>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.transparent;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.render_resource.shared;
import deren.vulkan.constant_init;
import deren.vulkan.primitive;
import deren.vulkan.render_environment;

export namespace deren::vulkan::pass {

    /// @brief what the renderer hands the transparent pass: the sorted leaves, one secondary, and the draw state
    struct transparent_frame {
        /// the blended leaves, sorted FAR to NEAR by the frame loop's culling (the blend order)
        std::span<primitive const* const> leaves = {};
        /// the per-slot secondary command buffer this pass records into (one segment: the leaves are few)
        VkCommandBuffer secondary = VK_NULL_HANDLE;
        /// the draw state builder (see scene_frame::make_environment: the registry is the renderer's)
        render_environment (*make_environment)(void* owner, VkCommandBuffer command_buffer, bool gbuffer) = nullptr;
        void* owner = nullptr;
        /// the heap bind infos this secondary must inherit (see scene_frame::fill_heap_bind: a secondary is
        /// validated on its own, so the primary's heap bind does not reach it)
        void (*fill_heap_bind)(void* owner, VkBindHeapInfoEXT& resource, VkBindHeapInfoEXT& sampler) = nullptr;
        /// the ONE colour attachment the secondary inherits, plus the depth format
        VkFormat color_format = VK_FORMAT_UNDEFINED;
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent = {0, 0};
    };

    /**
     * @brief the frame's alpha-blended geometry, composited over the shaded image
     *
     * Its position in the frame is load-bearing and unchanged: after the lighting stage (a blended surface
     * composites over SHADED pixels), outside that stage's instance (which samples the depth this one needs as
     * an attachment), and before the resolve (which must see the composited frame).
     */
    class transparent_pass final : public frame_pass {
    public:
        transparent_pass() = default;
        ~transparent_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        void set_frame(transparent_frame const& frame) noexcept;

    private:
        static constexpr std::array<std::string_view, 0> pipeline_names = {}; // the leaves name their pipelines
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::graphics,
            .extent = extent_rule::full,
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = false, // the scene pipelines carry their own stored viewport
        };

        // called pass_frame, not frame: set_frame()'s frame parameter in transparent.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        transparent_frame pass_frame = {};
    };

} // namespace deren::vulkan::pass
