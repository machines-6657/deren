// module version: 0.3.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/shadow.cppm
 * @brief The sixteenth real pass: the directional shadow map, one depth-only cascade per layer.
 * @defgroup vulkan_pass_shadow Shadow Pass
 *
 * WHY IT IS DIFFERENT FROM EVERY OTHER GRAPHICS PASS: the others draw a fullscreen triangle whose shape the pass
 * owns; this one draws THE SCENE, once per cascade, into a layer of a layered depth image - and it records that
 * content into SECONDARY command buffers the frame loop's task pool fills. So its frame carries the scene's content
 * as a CALLBACK (see shadow_frame::record_cascade) plus what is specific to this pass: the secondaries, the map's
 * edge and the task scheduler.
 *
 * WHAT IT OWNS: the depth-only pipeline (built at create time from `pass_context::depth_format` and the three
 * slope-scaled bias factors), and the per-cascade recording: the layer's transition to a
 * depth attachment, the depth-only instance at the map's edge, the pre-recorded secondary's execution and the
 * instance's end.
 *
 * WHAT IT DOES NOT OWN, each a shared thing rather than an omission: the shadow map IMAGES and their pool (the
 * renderer creates them and the scene shaders read them through the heap - a shared resource like the G-buffer
 * family); the HAND-BACK
 * barrier (it covers every ALLOCATED layer, including the spare ones a shrank cascade count left behind, which only
 * the image's creator knows - the composite's HDR transition, one resource class over); the content itself (the
 * live depth-bias state, the two-sided policy, the casters and the secondary's begin info are the
 * renderer's, which is why they arrive as one callback); and the task POOL (the frame loop's scheduler).
 */

module;

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.shadow;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.primitive;    // max_shadow_cascades: the run of layers this pass's declaration claims
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the pipeline this pass builds

export namespace deren::vulkan::pass {

    /**
     * @brief what the renderer hands the shadow pass: the per-cascade content, the scheduler, and the map's edge
     *
     * THE CONTENT IS ONE CALLBACK AND NOT AN ENVIRONMENT FACTORY, which is a correction this module's first draft
     * measured: recording a SECONDARY is not "build a draw state and draw". It is `vkBeginCommandBuffer` with an
     * INHERITANCE struct (whose rendering info names the depth format and no colour attachment), the cascade's push,
     * the content, and `vkEndCommandBuffer` - and the begin info, the inheritance struct, the scene block, the live
     * depth-bias dynamic state, the two-sided policy and the casters are ALL the renderer's facts. Splitting those
     * across a factory would leave this pass holding a draw state it cannot begin a command buffer with.
     */
    struct shadow_frame {
        /**
         * @brief record ONE cascade's content into its secondary, from a fresh begin to its end
         *
         * The implementation begins @p secondary with its own inheritance info and usage flags, sets the map-sized
         * viewport and scissor, pushes @p cascade_index at the offset this pass's declaration names and draws the
         * frame's casters with @p pipeline - then ends the buffer.
         */
        /// @return whether the secondary was recorded: a begin that FAILED must not be executed (that is a VUID
        ///         and can wedge the frame slot), so the answer travels back rather than being assumed
        /// @param mesh_stage whether @p pipeline is a MESH pipeline, i.e. whether the casters must be drawn as
        ///        dispatches (vkCmdDrawMeshTasksEXT) instead of indexed draws - the pipeline and the way to feed
        ///        it are one fact, so they travel together (see docs/mesh_shaders.md step 1)
        bool (*record_cascade)(void* owner, VkCommandBuffer secondary, uint32_t cascade_index, VkPipeline pipeline, bool mesh_stage, bool meshlets) = nullptr;
        /// the frame loop's scheduler: one task per cascade, each recording into its own secondary
        void (*run_tasks)(void* owner, std::span<std::function<void()>> tasks) = nullptr;
        void* owner = nullptr;
        /// the per-cascade SECONDARY command buffers, in cascade order - one per layer this frame renders
        std::span<VkCommandBuffer const> cascades = {};
        /// the shadow map's edge: this pass's rendering-instance size (it does NOT follow the surface)
        uint32_t map_size = 0;
    };

    /**
     * @brief the directional shadow map: the scene's depth from the light, one cascade per layer
     *
     * It runs BEFORE the scene pass, because the surfaces that sample the maps are shaded after it - and it may not
     * run at all: the renderer skips the whole stage when the maps this slot already owns are still valid for it and
     * the caster geometry has not changed, which is what an UNRESOLVED pass means (nothing is recorded, and the
     * host's hand-back keeps the array sampleable).
     */
    class shadow_pass final : public frame_pass {
    public:
        shadow_pass() = default;
        ~shadow_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief whether the pass built the pipeline it records with (the renderer gates its feature on this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question, so an owner holding only a chain can ask it
        ///        (a pass with nothing of its own to build keeps the interface's `true`; see `frame_pass::ready`)
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        /// @brief the pipeline the runner binds before this pass records
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

        void set_frame(shadow_frame const& frame) noexcept;

    private:
        static constexpr std::string_view vertex_shader_name = "shadow.vert.spv";
        static constexpr std::string_view fragment_shader_name = "shadow.frag.spv";
        /// THE MESH STAGE'S OWN MODULE, from the same shader file's `mesh_main` entry (docs/mesh_shaders.md step
        /// 1). It replaces the vertex entry rather than joining it: the pipeline is built with MESH + the same
        /// fragment stage, and the two pipelines are the same depth pass drawn two ways.
        static constexpr std::string_view mesh_shader_name = "shadow.mesh.spv";
        /// THE MESHLET FORM of the same pass (docs/mesh_shaders.md step 3): one workgroup per meshlet, each
        /// reading its window out of the table. Preferred over both others when it exists.
        static constexpr std::string_view meshlet_shader_name = "shadow.meshlet.spv";
        /// the CREATE-time half of the bias state: slope-scaled rasterization bias pushes a caster's depth away from
        /// the light proportionally to its slope, which is what removes acne on angled surfaces. The per-frame half
        /// is dynamic state and belongs to the renderer's content callback.
        static constexpr float create_bias_constant = 0.0f;
        static constexpr float create_bias_slope = 1.5f;
        static constexpr float create_bias_clamp = 0.0f;

        static constexpr std::array<std::string_view, 1> pipeline_names = {"shadow"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::graphics, // a draw per caster, not one fullscreen triangle
            .extent = extent_rule::none,      // the map's edge is the pass's, not the frame's (see the field below)
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            // The viewport is the MAP's edge and it belongs to the secondary the content is recorded into (the
            // renderer's callback sets it), so the runner must not overwrite it with io.extent - which `none` leaves
            // empty anyway.
            .resync_viewport = false,
        };
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        /// THE MESH FORM, and since step 4 (docs/mesh_shaders.md) it is the pass's ONLY form: the vertex pipeline is
        /// gone with the rest of the vertex geometry path, so a device that cannot build this one gets no shadow map
        /// rather than a different rasterizer. It fetches its casters' vertices the way the input assembler used to.
        std::optional<vk_pipeline> mesh_pipeline = std::nullopt;
        /// ... and the MESHLET form, preferred over it when it exists (see meshlet_shader_name)
        std::optional<vk_pipeline> meshlet_pipeline = std::nullopt;
        // called pass_frame, not frame: set_frame()'s frame parameter in shadow.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        shadow_frame pass_frame = {};
    };

    /// THE DECLARATION'S PUSH BLOCK is the four-byte cascade index at the scene block's end, and the renderer's
    /// content callback is what pushes it - so the two facts are asserted together here, where both are visible.
    static_assert(render_resource::shadow_io.push->size == sizeof(uint32_t), "the shadow pass pushes one cascade index");
    static_assert(render_resource::shadow_io.push->stages == (render_resource::stage_flag::vertex | render_resource::stage_flag::fragment),
                  "the cascade index is read by both stages of the depth-only draw");
    /// ... and so is the RUN of layers, because it is the same two-copies-one-fact rule: the declaration claims
    /// every cascade the map can hold, and `deren.vulkan.primitive` is where that count lives (the light UBO's matrix
    /// array is the same number).
    static_assert(static_cast<uint32_t>(render_resource::shadow_io.targets[0].count) == deren::vulkan::max_shadow_cascades, "the shadow pass claims every cascade layer the map can have");

} // namespace deren::vulkan::pass
