module;

#include <array>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.readback;

import deren.utility;

namespace deren::vulkan {
    namespace {
        // 工厂通过引擎上下文取得 RHI；engine_device 本身不是后端 api_core 的子类。
        deren::promise::rhi::api_core& contract_of(engine_device& gpu) {
            return gpu.gpu.api();
        }

        /// The escape, obtained from the contract once and then used through ITS pointer.
        deren::promise::rhi::vulkan_escape* escape_of(engine_device& gpu) {
            return static_cast<deren::promise::rhi::vulkan_escape*>(
                contract_of(gpu).query_extension(deren::promise::rhi::extension_kind::vulkan_escape));
        }

        /// The VkBuffer a contract buffer carries, through the escape - the contract's own rule for a
        /// native handle, and the reason the allocator's detail map is consulted nowhere in this class.
        /// Null when the backend announced no escape (it does, and the startup gate refuses a backend
        /// that does not) or when the buffer carries no handle at all.
        VkBuffer native_buffer_of(engine_device& gpu, deren::promise::rhi::buffer const& buffer) {
            auto* const escape = escape_of(gpu);
            if (escape == nullptr) {
                return VK_NULL_HANDLE;
            }
            return reinterpret_cast<VkBuffer>(escape->native_buffer(buffer));
        }
    } // namespace

    readback::readback(engine_device& device)
        : gpu(&device) {
    }

    // read() 的同步提交在返回前完成，staging 可以直接由契约拥有类型释放。
    readback::~readback() = default;

    std::optional<readback::staged_target> readback::stage_for_copy(VkDeviceSize const size) {
        engine_device& vk = *this->gpu;
        if (size == 0) {
            return std::nullopt;
        }
        if (this->staging && this->staging_size >= size) {
            std::span<std::byte> const current_mapping = this->staging->mapped();
            if (current_mapping.data() != nullptr) {
                return staged_target{.buffer = native_buffer_of(vk, *this->staging), .mapped = current_mapping.data(), .size = static_cast<std::size_t>(size)};
            }
        }
        // 本类的 read() 同步完成；外部 stage_for_copy() 使用者负责等待自己提交的工作。
        this->staging = deren::promise::rhi::object_manager<deren::promise::rhi::buffer>{
            contract_of(vk).create_buffer(deren::promise::rhi::buffer_desc{.size = size, .usage = deren::promise::rhi::buffer_usage::readback_coherent})};
        this->staging_size = 0;
        if (!this->staging) {
            deren::utility::log("readback: staging buffer creation failed ({} bytes)", size);
            return std::nullopt;
        }
        std::span<std::byte> const mapped = this->staging->mapped();
        if (mapped.data() == nullptr) {
            // A read-back buffer whose bytes cannot be mapped has nothing to hand the caller: drop the
            // reference rather than keep a staging buffer no read could ever answer from. EMPTY is the
            // contract's spelling of "this buffer is not host-visible" (buffer::mapped()).
            this->staging.reset();
            return std::nullopt;
        }
        this->staging_size = size;
        return staged_target{.buffer = native_buffer_of(vk, *this->staging), .mapped = mapped.data(), .size = static_cast<std::size_t>(size)};
    }

    std::expected<std::vector<uint8_t>, std::string> readback::read(VkBuffer const source, VkDeviceSize const size, VkDeviceSize const offset) {
        engine_device& vk = *this->gpu;
        this->last_read_size = 0;
        if (source == VK_NULL_HANDLE || size == 0) {
            return std::unexpected(std::string("readback: nothing to read (null buffer or zero size)"));
        }
        auto const target = this->stage_for_copy(size);
        if (!target) {
            return std::unexpected(std::string("readback: staging buffer unavailable"));
        }

        // one command buffer per call: the core's pool hands them out and this wrapper frees the
        // buffer back to it on destruction, so nothing accumulates across calls
        vk_command_buffer const commands = vk.make_command_buffer();
        VkCommandBufferBeginInfo const begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                     .pNext = nullptr,
                                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                                                     .pInheritanceInfo = nullptr};
        if (vkBeginCommandBuffer(*commands, &begin_info) != VK_SUCCESS) {
            return std::unexpected(std::string("readback: vkBeginCommandBuffer failed"));
        }

        // The transfer needs the source's writes visible and finished, and it must not start while an
        // earlier transfer/stage is still writing. A buffer barrier (not an image one) is what carries
        // srcAccess/dstAccess for buffers; ALL_COMMANDS as the source is deliberate: the caller may
        // have written the buffer from any stage, and this is a one-shot path where a conservative
        // source costs nothing measurable.
        std::array<VkBufferMemoryBarrier2, 1> barriers = {};
        barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        barriers[0].pNext = nullptr;
        barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].buffer = source;
        barriers[0].offset = offset;
        barriers[0].size = size;
        VkDependencyInfo const dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                             .pNext = nullptr,
                                             .dependencyFlags = 0,
                                             .memoryBarrierCount = 0,
                                             .pMemoryBarriers = nullptr,
                                             .bufferMemoryBarrierCount = static_cast<uint32_t>(barriers.size()),
                                             .pBufferMemoryBarriers = barriers.data(),
                                             .imageMemoryBarrierCount = 0,
                                             .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(*commands, &dependency);

        VkBufferCopy const region = {.srcOffset = offset, .dstOffset = 0, .size = size};
        vkCmdCopyBuffer(*commands, source, target->buffer, 1, &region);
        if (vkEndCommandBuffer(*commands) != VK_SUCCESS) {
            return std::unexpected(std::string("readback: vkEndCommandBuffer failed"));
        }

        // 后端统一持有提交锁和同步对象；失败时不能把未完成的映射当作读回结果。
        VkResult const submitted = vk.submit_and_wait(*commands);
        if (submitted != VK_SUCCESS) {
            return std::unexpected(std::string("readback: synchronous submission failed (VkResult ") +
                                   std::to_string(static_cast<std::int32_t>(submitted)) + ")");
        }

        // NO INVALIDATE HERE, and it is a deliberate removal rather than an omission. The dropped call
        // asked the allocation's MEMORY TYPE and answered a no-op for this buffer: `readback_coherent`
        // is documented host-visible and HOST_COHERENT (vulkan/core/vma/vma.cppm's buffer_type table) and
        // is allocated through VMA_MEMORY_USAGE_CPU_ONLY, whose memory properties are that same pair
        // (vma.cppm's allocation table), so `invalidate_if_not_coherent` never invalidated anything for
        // it. The reliance is not new: the backend's own frame read-back slot hands out its mapping with
        // no invalidate at all (core.api_core.cpp's frame_readback_slot::mapped), and the contract has
        // no invalidate to carry. A device that served a non-coherent type here would be a violation of
        // the allocator's type table - the table is what the host read below now rests on.
        std::vector<uint8_t> out(static_cast<std::size_t>(size));
        std::memcpy(out.data(), target->mapped, static_cast<std::size_t>(size));
        this->last_read_size = out.size();
        return out;
    }
} // namespace deren::vulkan
