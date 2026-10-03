// The clustered-light sort's implementation: the shared scene block, the 1D dispatch over the cluster grid and
// the two buffer barriers that publish its writes. Moved out of `runtime::record_cluster_pass` and
// `core::make_cluster_pipeline` UNCHANGED in behaviour - the same single set bound at index 0, the same 64-wide
// workgroup, the same `(count + 63) / 64` group count and the same two COMPUTE_SHADER -> FRAGMENT_SHADER buffer
// barriers over the same two buffers - so the A/B against the parent commit decides it.
//
// It is the first compute pipeline this branch has taken out of `deren.vulkan.core`: `core::make_cluster_pipeline`
// built it against the core's own scene pipeline layout, which a pass cannot own. The replacement builds the
// same shape in `deren.vulkan.pipelines` (one set layout, no push range - `light_cluster.comp` takes no constants),
// so what changes is WHO owns the layout, not what the driver is asked for.

module;

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.cluster;

import deren.vulkan.pipelines; // build_cluster: the compute pipeline this pass owns
import deren.utility;

namespace deren::vulkan::pass {

    cluster_pass::~cluster_pass() {
        this->release_owned();
    }

    void cluster_pass::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    render_resource::pass_io const& cluster_pass::io() const noexcept {
        return render_resource::cluster_io;
    }

    deren::vulkan::pass::behaviour const& cluster_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view cluster_pass::feature() const noexcept {
        // The renderer's registry answers whether there is anything to sort (the knob, a punctual light, a
        // shading stage that reads a light list) and whether this pass built its pipeline.
        return "clustered";
    }

    bool cluster_pass::pipeline_ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    VkPipeline cluster_pass::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
    }

    void cluster_pass::set_frame(cluster_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void cluster_pass::prepare_frame(frame_facts const& facts) noexcept {
        // ONE number, and the host is the only one that can compute it: the grid the light culling produced,
        // which is the same `cluster_grid` the light UBO carries (see `runtime::make_frame_facts`).
        this->set_frame(cluster_frame{.cluster_count = facts.cluster_count});
    }

    void cluster_pass::create(pass_context const& context) {
        if (context.device == VK_NULL_HANDLE) {
            return;
        }
        if (this->device != VK_NULL_HANDLE && this->device != context.device) {
            this->release_owned();
        }
        this->device = context.device;
        if (this->pass_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<uint8_t const>{};
        if (spirv.empty()) {
            deren::utility::log("clustered lights disabled: the owner has no {}", shader_name);
            return;
        }
        // NO SET LAYOUT IS ASKED FOR: the pipeline is heap-native (a null layout plus the heap flag), so the
        // pass's descriptors come from the frame's bound heap and not from a set handed over by the owner.
        auto built = pipelines::build_cluster(context.device, spirv);
        if (!built) {
            deren::utility::log("clustered lights disabled: {}", built.error());
            this->release_owned();
            return;
        }
        this->pass_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: clustered light pipeline created (the frame's punctual lights are binned per cluster)");
    }

    void cluster_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the grid's DIMENSIONS are the renderer's frame data (it recomputes them from the new
        // extent and hands them over every frame), and this pass owns no per-generation handle.
    }

    void cluster_pass::record(resolved_io const& io) {
        if (!this->pass_pipeline.has_value() || io.pipelines.empty() || io.pipelines[0] == VK_NULL_HANDLE ||
            io.barrier_buffers.size() < render_resource::cluster_barriers.size() || this->pass_frame.cluster_count == 0) {
            return; // the runner resolves all of this or skips the pass (see runtime::resolve_cluster_pass)
        }
        VkBuffer const counts = io.barrier_buffers[barrier_counts].buffer;
        VkBuffer const indices = io.barrier_buffers[barrier_indices].buffer;
        if (counts == VK_NULL_HANDLE || indices == VK_NULL_HANDLE) {
            return;
        }

        // The dispatch reads the frame's OWN camera and light slots and writes the same generation's cluster
        // buffers - and it needs no bind to do it: the frame bound the heaps once, and the two indices its push
        // block carries pick the slot. A compute stage is not part of a rendering instance, so this still records
        // before vkCmdBeginRendering.
        vkCmdBindPipeline(io.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, io.pipelines[0]);
        vkCmdDispatch(io.cmd, (this->pass_frame.cluster_count + group_size - 1u) / group_size, 1, 1);

        // Hand the two buffers to the fragment stages that read them later in this submission (forward shading
        // inside the main instance, and the deferred lighting pass): a compute SHADER_WRITE is not visible to a
        // later SHADER_READ without this barrier. One barrier per buffer, because VkBufferMemoryBarrier2 covers a
        // single buffer - and the two handles come from the DECLARATION, which is what makes this the writer's
        // own ordering rather than something the host has to remember on the pass's behalf.
        std::array<VkBufferMemoryBarrier2, 2> barriers = {};
        barriers[0].buffer = counts;
        barriers[1].buffer = indices;
        for (VkBufferMemoryBarrier2& barrier : barriers) {
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            // the shader reads counts/indices as storage buffers, not as sampled images
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.offset = 0;
            barrier.size = VK_WHOLE_SIZE;
        }
        VkDependencyInfo dependency = {};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pBufferMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(io.cmd, &dependency);
    }

} // namespace deren::vulkan::pass
