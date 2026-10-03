// module version: 0.4.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/cluster.cppm
 * @brief The tenth real pass: the clustered-light sort, which bins the frame's punctual lights into the tile x
 *        slice grid the shading stages then read.
 * @defgroup vulkan_pass_cluster Clustered-Light Sort Pass
 *
 * WHAT IT OWNS: its pipeline layout and its compute pipeline (built at create time from the shared scene block
 * layout and its own shader - the first compute pipeline in this tree that came out of `deren.vulkan.core`, where
 * `core::make_cluster_pipeline` built it against the core's own scene pipeline layout); the bind of the shared
 * scene set; the one-dimensional dispatch over the cluster grid; and the TWO BUFFER BARRIERS that make its
 * writes visible to the fragment stages reading them later in the same submission. That last part is why the
 * framework grew `pass_io::barrier_buffers` in the same step - see the header of `deren.vulkan.pass`'s
 * `resolved_io` for the argument.
 *
 * WHY `extent_rule::none`: this dispatch is `tiles_x * tiles_y * slices` workgroups, a count derived from the
 * frame's extent but equal to neither it nor half of it. Declaring `full` would have been a claim the host
 * cannot honour (and one the pass would have to ignore), so the pass declares that it sizes its own work and
 * reads the count from its own frame - the same split as the scene pass's leaves.
 *
 * WHAT IT DOES NOT OWN: the two cluster buffers themselves (the renderer allocates them per frame slot, and
 * they live in the shared scene block as bindings 11 and 12) and the grid's dimensions, which the renderer derives
 * from the swapchain extent once per frame.
 */

module;

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.cluster;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the compute pipeline this pass builds

export namespace deren::vulkan::pass {

    /**
     * @brief what the renderer hands the sort: how many clusters this frame has
     *
     * One number, and it is the frame's: the grid comes from the extent, the slice count is the shader's own,
     * and the two buffers arrive through `resolved_io::barrier_buffers` because the pass orders them without
     * binding them (they are the shared scene block's bindings 11 and 12).
     */
    struct cluster_frame {
        /// `tiles_x * tiles_y * cluster_slice_count`, i.e. the number of work items (one cluster each)
        uint32_t cluster_count = 0;
    };

    /**
     * @brief the clustered-light sort: a 1D dispatch that bins the punctual lights into clusters
     *
     * It runs before the scene pass (the shading stages read the bins it writes) and is skipped entirely when
     * no punctual light is active or the frame is flat-shaded: `feature()` is the renderer's own predicate, so a
     * frame whose light list is empty records nothing - which is what keeps every pre-M5 reference byte-identical.
     */
    class cluster_pass final : public frame_pass {
    public:
        cluster_pass() = default;
        ~cluster_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief whether the pass built what it records with (the renderer gates its feature on this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question, so an owner holding only a chain can ask it
        ///        (a pass with nothing of its own to build keeps the interface's `true`; see `frame_pass::ready`)
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

        /// @brief build this pass's frame from the published facts (see frame_pass::prepare_frame)
        void prepare_frame(frame_facts const& facts) noexcept override;
        /// @brief the frame for this stage; the pass composes it itself now (see frame_pass::prepare_frame),
        ///        and the setter stays for a test that wants to hand one over directly
        void set_frame(cluster_frame const& frame) noexcept;

    private:
        static constexpr std::string_view shader_name = "light_cluster.comp.spv";
        /// the workgroup size, which must be `light_cluster.slang`'s `local_size_x`
        static constexpr uint32_t group_size = 64;
        /// the declared barrier buffers, by the position the declaration gives them
        static constexpr uint32_t barrier_counts = 0;
        static constexpr uint32_t barrier_indices = 1;
        static_assert(barrier_indices + 1 == render_resource::cluster_barriers.size(),
                      "the sort's barrier slots must match the declaration it indexes");

        static constexpr std::array<std::string_view, 1> pipeline_names = {"cluster"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::compute,
            .group_size_x = group_size,
            .group_size_y = 1,
            .group_size_z = 1,
            .extent = extent_rule::none, // the count is the frame's, not the extent's - see the file's header
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = false,
        };
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
        // called pass_frame, not frame: set_frame()'s frame parameter in cluster.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        cluster_frame pass_frame = {};
    };

} // namespace deren::vulkan::pass
