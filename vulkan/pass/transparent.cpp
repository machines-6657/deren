// The transparent pass's implementation: the two hand-off barriers, the LOAD instance, one secondary and the
// depth hand-back. Moved unchanged from `runtime::record_transparent_pass` - same barrier constants, same
// attachment helpers, same order - so the gate decides it on `transparent_blend`, the scenario with an
// alphaMode BLEND material, and on every other scenario that has none (where the pass does not run at all).

module;

#include <array>
#include <cstdint>
#include <span>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.transparent;

import deren.vulkan.render_resource;
import deren.vulkan.constant_init;
import deren.utility;

namespace deren::vulkan::pass {

    render_resource::pass_io const& transparent_pass::io() const noexcept {
        return render_resource::transparent_io;
    }

    deren::vulkan::pass::behaviour const& transparent_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view transparent_pass::feature() const noexcept {
        // THE RENDERER'S GATE, as a feature name (see the scene pass): a frame whose culling left nothing blended
        // is a frame this pass is INACTIVE on - which is what the runner checks before it resolves anything, so
        // the pass neither records nor resolves and costs nothing on those frames.
        return "transparent";
    }

    void transparent_pass::create(pass_context const&) {
        // Nothing to build: no own set (everything is in the shared scene block) and no pipeline (the leaves name
        // theirs). See the scene pass.
    }

    void transparent_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing is per-image here; the secondary is the frame loop's and the leaves own their buffers.
    }

    void transparent_pass::set_frame(transparent_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void transparent_pass::record(resolved_io const& io) {
        if (this->pass_frame.make_environment == nullptr || this->pass_frame.secondary == VK_NULL_HANDLE || this->pass_frame.leaves.empty() || io.targets.size() < 2) {
            return; // the runner resolves all of this or skips the pass (see runtime::resolve_transparent_pass)
        }
        VkImageView const target_view = io.targets[0].view; // the scene colour target (declaration order)
        VkImage const target_image = io.targets[0].image;
        VkImageView const depth_view = io.targets[1].view; // the surface depth
        VkImage const depth_image = io.targets[1].image;

        // The lighting stage sampled the surface depth, so it is in SHADER_READ_ONLY: hand it back to the
        // attachment layout for the depth test. The scene target is already in COLOR_ATTACHMENT (the lighting
        // instance ended as an attachment write), but dynamic rendering inserts no dependency between two
        // instances, so that store still has to be published before this instance LOADs the same image.
        std::array<VkImageMemoryBarrier2, 2> barriers = {};
        barriers[0] = deren::vulkan::sampling_to_depth_attachment_transition;
        barriers[0].image = depth_image;
        barriers[1] = deren::vulkan::color_attachment_dependency;
        barriers[1].image = target_image;
        VkDependencyInfo const dependency = make_image_dependency_info(static_cast<uint32_t>(barriers.size()), barriers.data());
        vkCmdPipelineBarrier2(io.cmd, &dependency);

        // The leaves go into the frame slot's transparent secondary, with inheritance matching the instance
        // below: ONE colour attachment at 1x. A secondary does not inherit state from its primary, so it binds
        // the shared scene block for itself - the same bind the scene pass's segments make.
        std::array<VkFormat, 1> const color_formats = {this->pass_frame.color_format};
        VkCommandBufferInheritanceRenderingInfo const inheritance =
            make_inheritance_rendering_info(color_formats.data(), 1, this->pass_frame.depth_format, VK_SAMPLE_COUNT_1_BIT);
        // The heaps are inherited (see scene.cpp's segment begin): a secondary is validated on its own, so the
        // primary's heap bind does not reach it.
        VkBindHeapInfoEXT resource_bind = {};
        VkBindHeapInfoEXT sampler_bind = {};
        bool const inherit_heaps = this->pass_frame.fill_heap_bind != nullptr;
        if (inherit_heaps) {
            this->pass_frame.fill_heap_bind(this->pass_frame.owner, resource_bind, sampler_bind);
        }
        VkCommandBufferInheritanceDescriptorHeapInfoEXT const heap_inheritance = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT,
            .pNext = &inheritance,
            .pSamplerHeapBindInfo = inherit_heaps ? &sampler_bind : nullptr,
            .pResourceHeapBindInfo = inherit_heaps ? &resource_bind : nullptr,
        };
        VkCommandBufferInheritanceInfo const secondary_inherit = make_inheritance_info(&heap_inheritance);
        VkCommandBufferBeginInfo const secondary_begin = make_command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT, &secondary_inherit);
        bool recorded = false;
        if (vkBeginCommandBuffer(this->pass_frame.secondary, &secondary_begin) == VK_SUCCESS) {
            render_environment env = this->pass_frame.make_environment(this->pass_frame.owner, this->pass_frame.secondary, /*gbuffer=*/false);
            // No set to bind (see scene_pass::record_segment): every slot these leaves read comes from the heaps,
            // which the runtime binds on this same secondary before executing it.
            for (primitive const* const leaf : this->pass_frame.leaves) {
                leaf->draw(env);
            }
            vkEndCommandBuffer(this->pass_frame.secondary);
            recorded = true;
        } else {
            deren::utility::log("transparent pass: secondary begin failed - transparent leaves skipped this frame");
        }

        // loadOp LOAD on both attachments: the scene target holds the shaded frame and the depth holds the
        // opaque surface, and neither may be cleared.
        VkRenderingAttachmentInfo const color_attachment = make_load_color_attachment_info(target_view);
        VkRenderingAttachmentInfo const depth_attachment = make_load_depth_attachment_info(depth_view);
        VkRenderingInfo const rendering_info =
            make_rendering_info(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, {{0, 0}, this->pass_frame.extent}, &color_attachment, 1, &depth_attachment);
        vkCmdBeginRendering(io.cmd, &rendering_info);
        if (recorded) {
            vkCmdExecuteCommands(io.cmd, 1, &this->pass_frame.secondary);
        }
        vkCmdEndRendering(io.cmd);

        // Hand the depth back to the layout everything downstream samples it in: this pass took it out of
        // SHADER_READ to depth-test against it, and TWO later stages read the same image (the resolve's
        // disocclusion guard and the composite's edge test). Nothing else would move it - the flag-driven
        // transition was already consumed by the lighting stage - so a frame with blended geometry would leave
        // the image as an attachment and every read after it would be a layout error.
        VkImageMemoryBarrier2 to_sampling = deren::vulkan::shadow_map_sampling_transition; // attachment -> SHADER_READ
        to_sampling.image = depth_image;
        VkDependencyInfo const sampling_dependency = make_image_dependency_info(1, &to_sampling);
        vkCmdPipelineBarrier2(io.cmd, &sampling_dependency);
    }

} // namespace deren::vulkan::pass
