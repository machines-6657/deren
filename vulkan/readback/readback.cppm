// ============================================================================
// module: deren.vulkan.readback
// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))
//
// GPU -> CPU copies of buffer contents: a reusable staging buffer, a one-shot
// submit and the host read, behind one synchronous call.
//
// Extracted from deren.vulkan.runtime, where "read a buffer back" was spelled out inline
// for the screenshot path only (persistent buffer, size tracking, mapped pointer,
// wait) and could not be reused - dumping a material table, a cluster light list or
// an SSBO needed for a test all had to re-implement it.
//
// Depends only on deren.vulkan.core (device, queue, command buffer, VMA).
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <vulkan/vulkan.h>

export module deren.vulkan.readback;

import deren.promise.rhi; // the contract's staging-buffer handle + object_manager
export import deren.vstd;
export import deren.vulkan.core;

/**
 * @file vulkan/readback/readback.cppm
 * @defgroup vulkan_readback GPU -> CPU Readback
 * @brief Copies a range of a device buffer into host memory, synchronously.
 *
 * One staging buffer sized to the largest request so far, one fence, one command buffer per call:
 * `read()` records the copy, submits it alone on the graphics queue, waits for that submission and
 * hands back the bytes. The wait is the point - the caller gets data, not a promise, which is what
 * makes this usable from a debug dump, a headless check or a screenshot without any of them having
 * to know about fences or frame pacing.
 *
 * @note this is deliberately NOT a way to read a buffer an in-flight frame is still writing: such a
 *       resource belongs to that frame's synchronization. Read after the frame has been submitted
 *       and paced, or for a resource the renderer is not using.
 * @note the module knows nothing about IMAGES. An image read-back is an image -> buffer copy, which
 *       only the caller can lay out (layout transitions, aspect, subresource), followed by a read of
 *       that buffer - so the caller records its copy into a buffer this class stages for it
 *       (stage_for_copy), and the screenshot path is exactly that.
 */
namespace deren::vulkan {
    /**
     * @ingroup vulkan_readback
     * @brief reusable GPU -> CPU readback of buffer ranges
     * @note not thread safe: it owns one staging buffer, one fence and the mapped view of them, so a
     *       second concurrent read would resize or reset them under the first one's copy
     */
    export class readback {
        // non-const: the copies go through the contract's factory and the queue, and neither
        // `create_buffer()` nor the submit path is a const operation
        /// Deliberately NOT called `vk`: stage_for_copy() and read() bind a local `core& vk`, and that
        /// local would hide a member of the same name - MSVC /W4 reports C4458, an error under /WX
        /// (clang does not warn: -Wshadow is not enabled there).
        core* gpu = nullptr;
        /// host-visible + coherent + TRANSFER_DST, grown on demand (see stage_for_copy). The contract's
        /// owner keeps the allocation alive for as long as this member lives, and the native handle and
        /// the mapping are asked OF THE HANDLE where they are needed (`vulkan_escape::native_buffer()` /
        /// `buffer::mapped()`) rather than cached beside it.
        deren::promise::rhi::object_manager<deren::promise::rhi::buffer> staging = {};
        VkDeviceSize staging_size = 0;
        /// the one-shot submit's fence. Created lazily, reset by wait(), and only meaningful while a
        /// copy is in flight - `fence_pending` records whether it was signaled.
        VkFence fence = VK_NULL_HANDLE;
        bool fence_pending = false;
        /// the size the last `read()` produced, for total_size()
        std::size_t last_read_size = 0;

        /// wait for the in-flight copy and reset the fence; safe to call with nothing in flight
        void wait();

    public:
        explicit readback(core& device);
        readback(readback const&) = delete;
        readback& operator=(readback const&) = delete;
        readback(readback&&) = delete;
        readback& operator=(readback&&) = delete;
        ~readback();

        /**
         * @ingroup vulkan_readback
         * @brief the staging buffer a caller can record its OWN copy into (the image -> buffer case)
         */
        struct staged_target {
            VkBuffer buffer = VK_NULL_HANDLE; // the TRANSFER_DST buffer a copy command writes to
            void* mapped = nullptr;           // its host-visible view; valid until the next call
            std::size_t size = 0;             // how many bytes it can hold
        };

        /**
         * @brief make the staging buffer hold at least @p size bytes and hand it out
         * @param size bytes the caller is about to copy into it
         * @return the buffer, its mapped view and its capacity, or std::nullopt when it cannot be sized
         * @note the caller records `vkCmdCopyImageToBuffer` (or any other copy) into ITS OWN command
         *       buffer, submits it and waits for that submission - this class cannot know when it
         *       completes, and reading the bytes before it does would hand back the previous contents.
         *       Then `mapped` is the data.
         */
        [[nodiscard]] std::optional<staged_target> stage_for_copy(VkDeviceSize size);

        /**
         * @ingroup vulkan_readback
         * @brief copy @p size bytes at @p offset from @p source and return them
         * @param source the buffer to read (a buffer this VMA allocator owns)
         * @param size how many bytes to read; clamped to what the source actually holds
         * @param offset first byte to read
         * @return the bytes, or an error message when the copy cannot be performed
         * @note blocks until the GPU has finished the copy
         * @note the result may be shorter than @p size when the source is shorter; it is never padded,
         *       so a caller that needs an exact length checks `result->size()`
         */
        std::expected<std::vector<uint8_t>, std::string> read(VkBuffer source, VkDeviceSize size, VkDeviceSize offset = 0);

        /** @brief the size of the last `read()` (0 before the first one) */
        [[nodiscard]] std::size_t total_size() const noexcept {
            return this->last_read_size;
        }
    };
} // namespace deren::vulkan
