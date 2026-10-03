// The alphaMode MASK bake's implementation: the pipeline it owns and the dispatch per caster. Moved out of
// `runtime::make_mask_bake_pipeline` and the mask branch of
// `runtime::record_acceleration_structures` UNCHANGED in behaviour - the same heap slots (the bindless texture
// array and the material table), the same 44-byte push block, the same
// 64-wide workgroup and the same one dispatch per caster - so the A/B against the parent commit decides it.

module;

#include <cstdint>
#include <glm/glm.hpp>
#include <span>
#include <string>
#include <utility>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.mask_bake;

import deren.vulkan.pipelines; // build_mask_bake: the compute pipeline this job owns
import deren.utility;

namespace deren::vulkan::pass {

    mask_bake_job::~mask_bake_job() {
        this->release_owned();
    }

    void mask_bake_job::release_owned() noexcept {
        this->pass_pipeline.reset();
    }

    bool mask_bake_job::ready() const noexcept {
        return this->pass_pipeline.has_value();
    }

    VkPipeline mask_bake_job::pipeline() const noexcept {
        return this->pass_pipeline.has_value() ? this->pass_pipeline->get_pipeline() : VK_NULL_HANDLE;
    }

    std::expected<void, std::string> mask_bake_job::create(pass_context const& context) {
        if (context.device == VK_NULL_HANDLE) {
            return std::unexpected(std::string("mask bake: no device"));
        }
        if (this->device != VK_NULL_HANDLE && this->device != context.device) {
            this->release_owned();
        }
        this->device = context.device;
        std::span<uint8_t const> const spirv = context.shader != nullptr ? context.shader(context.owner, shader_name) : std::span<uint8_t const>{};
        if (spirv.empty()) {
            return std::unexpected(std::string("mask bake: the owner has no ") + std::string(shader_name));
        }
        // Everything the bake reads is a heap slot the shader names itself: the material table (the alpha
        // texture's index, the base colour factor's alpha, the cutoff) and the bindless texture array.
        auto built = pipelines::build_mask_bake(context.device, spirv);
        if (!built) {
            return std::unexpected(std::move(built.error()));
        }
        this->pass_pipeline = std::move(built->trace);
        deren::utility::log("SUCCESS: alphaMode MASK bake pipeline created (the mask is collapsed into the structures)");
        return {};
    }

    void mask_bake_job::record(VkCommandBuffer const command_buffer, mask_bake_request const& request, void* const push_owner,
                               bool (*push_raw)(void* owner, VkCommandBuffer command_buffer, std::span<std::byte const> bytes)) const noexcept {
        if (!this->ready() || request.triangle_count == 0) {
            return;
        }
        auto const halves = [](VkDeviceAddress const address) {
            return glm::uvec2(static_cast<uint32_t>(address & 0xFFFFFFFFu), static_cast<uint32_t>(address >> 32u));
        };
        mask_bake_push_constants bake = {};
        bake.source_vertices = halves(request.source_vertices);
        bake.source_indices = halves(request.source_indices);
        bake.destination = halves(request.destination);
        bake.source_stride = request.source_stride;
        bake.destination_stride = request.destination_stride;
        bake.index_type = request.index_type;
        bake.triangle_count = request.triangle_count;
        bake.material_index = request.material_index;
        vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, this->pipeline());
        // THE BLOCK GOES AS DATA, not as a push constant: the pipeline has no layout (see the header). This shader
        // declares no heap indices, so nothing is appended - the block is pushed exactly as declared. The job's own
        // set is no longer bound either: the material table and the bindless textures are heap slots the shader
        // names itself, and a set bound to a layout-less pipeline is invalid.
        if (push_raw != nullptr) {
            [[maybe_unused]] bool const pushed = push_raw(push_owner, command_buffer, std::as_bytes(std::span(&bake, 1)));
        }
        vkCmdDispatch(command_buffer, (bake.triangle_count + group_size - 1u) / group_size, 1, 1);
    }

} // namespace deren::vulkan::pass
