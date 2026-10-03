module;

#include <vulkan/vulkan.h>
#define VMA_IMPLEMENTATION
#include <vma/vk_mem_alloc.h>

export module deren.vulkan.core:vma;

export import deren.vstd;
import deren.utility;
import :vma_handles;
import deren.vulkan.constant_init;

/**
 * @file vma.cppm
 * @defgroup vulkan_vma Vulkan VMA Allocator
 * @brief handle-based GPU buffer/image allocator built on VMA
 * @note
 *      - buffers/images are referenced by uint64_t handles
 *      - thread-safe for creation and lookup
 */
namespace deren::vulkan {

    /**
     * @ingroup vulkan_vma
     * @brief the memory properties the *_coherent buffer types REQUIRE, spelled out instead of
     *        implied by VMA_MEMORY_USAGE_CPU_TO_GPU.
     *
     * VMA's CPU_TO_GPU usage only guarantees HOST_VISIBLE (it *prefers* DEVICE_LOCAL and says
     * nothing about coherence), so an allocation made through it may land in a non-coherent
     * host-visible type. That is fine for the create-time upload path, which checks
     * is_host_coherent() and calls vmaFlushAllocation() - but the per-frame path writes straight
     * into the persistent mapping and never flushes, because these types are documented as
     * coherent. Requiring the flag is what makes that documentation true; without it a device that
     * hands out a non-coherent host-visible type would have the GPU read stale camera / light /
     * skin / morph data, silently and with no validation error.
     * @note DEVICE_LOCAL is left to preferredFlags, so a device that can serve coherent
     *       host-visible DEVICE_LOCAL memory still gets it.
     */
    export constexpr VkMemoryPropertyFlags coherent_host_visible_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    /**
     * @ingroup vulkan_vma
     * @brief detail of a created buffer: the buffer handle, its allocation and allocation info
     */
    export struct buffer_detail {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo allocation_info = {};
        // reference count for shared resources; buffers have no digest today, so this stays 1
        std::atomic<uint32_t> use_count = 1;

        buffer_detail() = default;
        // std::atomic is neither copyable nor movable: a custom move constructor carries the
        // reference count by loading it (the source is discarded right after)
        buffer_detail(buffer_detail&& other) noexcept
            : buffer(other.buffer)
            , allocation(other.allocation)
            , allocation_info(other.allocation_info)
            , use_count(other.use_count.load()) {
            other.buffer = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
        }
        buffer_detail(buffer_detail const&) = delete;
        buffer_detail& operator=(buffer_detail const&) = delete;
        buffer_detail& operator=(buffer_detail&&) = delete;
    };

    /**
     * @ingroup vulkan_vma
     * @brief buffer usage type, decides memory properties and upload strategy
     */
    export enum class buffer_type {
        vertex,                         // GPU_ONLY, requires a staging buffer
        index,                          // GPU_ONLY, requires a staging buffer
        uniform_gpu_only,               // GPU_ONLY, suited for uniforms updated infrequently
        uniform_coherent,               // HOST_VISIBLE | HOST_COHERENT, suited for per-frame uniforms
        uniform_cached,                 // HOST_VISIBLE | HOST_CACHED, suited for read-back
        storage_coherent,               // HOST_VISIBLE | HOST_COHERENT storage buffer (e.g. GPU-visible material table)
        readback_coherent,              // HOST_VISIBLE | HOST_COHERENT with TRANSFER_DST: GPU -> CPU read-back
                                        // (e.g. a screenshot copy). Created without initial contents: pass a
                                        // null data pointer + the byte size to create_buffer(), which then only
                                        // allocates (see direct_upload).
        acceleration_structure_storage, // GPU_ONLY, ACCELERATION_STRUCTURE_STORAGE: the memory a
                                        // bottom/top level structure lives in. Allocate-only for the
                                        // same reason as the read-back type - a build command fills it,
                                        // the host never uploads into it.
        acceleration_structure_scratch, // GPU_ONLY, STORAGE + SHADER_DEVICE_ADDRESS: the build's
                                        // scratch space. Allocate-only, and its DEVICE ADDRESS (not its
                                        // offset) is what has to be aligned - see the AS module.
        storage_gpu_only,               // GPU_ONLY, STORAGE + SHADER_DEVICE_ADDRESS, allocate-only: a
                                        // buffer a COMPUTE pass fills and the host never reads or writes -
                                        // the mask bake's expanded vertices are the first user (see
                                        // shaders/mask_bake.slang). The same flags as the scratch type,
                                        // because both need exactly that; they are separate variants
                                        // because the INTENT is what a reader is looking for.
    };

    /**
     * @ingroup vulkan_vma
     * @brief image usage type, decides memory properties and upload strategy
     */
    export enum class image_type {
        texture_2d,         // GPU_ONLY, regular texture, needs staging
        texture_2d_color,   // GPU_ONLY, texture with a color format
        texture_2d_depth,   // GPU_ONLY, depth texture
        texture_2d_staging, // HOST_VISIBLE + HOST_COHERENT, LINEAR tiling: for dynamically updated
                            // textures. LINEAR is part of the contract, not an implementation
                            // detail - the type is uploaded by writing pixels through the host
                            // mapping, and an OPTIMAL-tiled image's layout is implementation-defined
                            // (see direct_image_upload). The caller owns the layout transitions.
        texture_cubemap,    // GPU_ONLY, cubemap
        render_target,      // GPU_ONLY, render target (readable/writable)
    };

    /**
     * @ingroup vulkan_vma
     * @brief auxiliary data for image creation
     */
    // Helper structure for Image
    export struct image_create_info {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mip_levels = 1;
        uint32_t array_layers = 1;
        VkFormat format = {};
        VkImageUsageFlags extra_usage = 0;
        bool operator==(image_create_info const&) const = default;
    };

    /**
     * @ingroup vulkan_vma
     * @brief detail of a created image: the image handle, its allocation and allocation info
     * @note immutable, data-uploaded textures (texture_2d / texture_2d_color / texture_cubemap)
     *       are deduplicated by content: identical bytes + identical create parameters share one
     *       GPU image and one handle, tracked by use_count
     */
    export struct image_detail {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo allocation_info = {};
        // XXH3-128 content digest (data_block<16>, hex-formatable); all-zero = no content digest
        // (never deduplicated)
        deren::utility::xxh3_digest digest = {};
        // creation parameters, kept so a digest hit only reuses an identical image
        image_create_info create_info = {};
        image_type type = image_type::texture_2d;
        // reference count for shared images; free_image() only really destroys at zero
        std::atomic<uint32_t> use_count = 1;

        image_detail() = default;
        // std::atomic is neither copyable nor movable: a custom move constructor carries the
        // reference count by loading it (the source is discarded right after)
        image_detail(image_detail&& other) noexcept
            : image(other.image)
            , allocation(other.allocation)
            , allocation_info(other.allocation_info)
            , digest(other.digest)
            , create_info(other.create_info)
            , type(other.type)
            , use_count(other.use_count.load()) {
            other.image = VK_NULL_HANDLE;
            other.allocation = VK_NULL_HANDLE;
        }
        image_detail(image_detail const&) = delete;
        image_detail& operator=(image_detail const&) = delete;
        image_detail& operator=(image_detail&&) = delete;
    };

    /**
     * @ingroup vulkan_vma
     * @brief handle-based GPU buffer/image allocator built on VMA
     * @note
     *      - buffers/images are referenced by uint64_t handles
     *      - thread-safe: creation and lookup are protected by an internal mutex
     *      - call destroy() to release the underlying VmaAllocator
     */
    export class vma_allocator : deren::utility::enable_handle_distribute {
        VmaAllocator allocator = {};
        // These two are named after the parameter that carries them, not after what they hold: init()`s device
        // and queue parameters would hide members of those names and MSVC /W4 reports C4458, an error under
        // /WX (clang does not warn: -Wshadow is not in the project flags).
        VkDevice logical_device = VK_NULL_HANDLE;
        VkQueue upload_queue = VK_NULL_HANDLE;
        std::vector<std::pair<VkCommandPool, VkCommandBuffer>> command_cache;
        std::vector<VkFence> fence_cache = {};
        std::map<uint64_t, buffer_detail> buffers = {};
        std::map<uint64_t, image_detail> images = {};
        std::mutex access_mutex = {};
        // Guards command_cache / fence_cache so uploads can safely reuse them outside access_mutex
        std::mutex cache_mutex = {};
        // VkQueue is an externally synchronized object; vkQueueSubmit must be serialized
        std::mutex queue_mutex = {};
        // Staging buffer cache: avoids creating/destroying large staging memory on every upload
        struct staging_buffer_cache {
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VmaAllocationInfo allocation_info = {};
        };
        staging_buffer_cache staging_cache = {};
        // Capacity (bytes) of the currently cached staging buffer
        VkDeviceSize buffer_size = 0;
        // The staging buffer is shared; writes and GPU copies must hold it exclusively
        std::mutex staging_mutex = {};
        // see the note on logical_device above: the same C4458 rule for init()`s queue_family_index parameter
        uint32_t upload_queue_family_index = 0;
        /// The host image copy's UPLOAD entry point (`vkCopyMemoryToImageEXT`), handed down by `core`,
        /// which resolves and CHECKS it at startup - the extension is required, so a running process has
        /// it. It is a device-level call rather than a queue one, which is exactly why the image upload
        /// lives on the allocator's side of the fence and needs this pointer.
        PFN_vkCopyMemoryToImageEXT copy_memory_to_image = nullptr;

        // ---- ownership: release/retain are private - the only public way to release a
        // buffer/image is to destroy (or reset) the vk_buffer / vk_image RAII owner that
        // vma_allocator handed out. create_* injects lambdas (created inside this class,
        // so they may call these) into those owners; raw-handle free calls are impossible
        // from outside, which removes the leak/over-release paths of the old uint64 API.
        // Shared (deduplicated) resources are reference-counted: free decrements and really
        // destroys only the last reference; retain bumps the count for a shared owner.
        void free_buffer(uint64_t handle);
        void free_image(uint64_t handle);
        void retain_buffer(uint64_t handle);
        void retain_image(uint64_t handle);

        [[nodiscard]] VkFence create_fence() const;
        // true when the memory type @p memory_type_index (a memory TYPE INDEX, resolved through
        // VMA's memory table) is HOST_COHERENT, i.e. host writes need no vmaFlushAllocation.
        // NOTE: VmaAllocationInfo::memoryType is an index, never a property bit mask - comparing
        // it against VK_MEMORY_PROPERTY_* bits directly is a bug.
        [[nodiscard]] bool is_host_coherent(uint32_t memory_type_index) const noexcept;
        // Called while holding staging_mutex: reuse the cached buffer if large enough, else destroy and rebuild
        bool ensure_staging_buffer(VkDeviceSize size, VkBuffer& buffer, VmaAllocation& allocation, VmaAllocationInfo& info);
        bool direct_upload(VmaAllocation const& allocation, VmaAllocationInfo& allocation_info, void const* data, uint64_t size) const;
        bool staging_upload(VkBuffer dst_buffer, void const* data, VkDeviceSize size);
        bool direct_image_upload(VmaAllocation allocation, void const* data, VkDeviceSize size) const;
        /// THE image upload: one mechanism, `vkCopyMemoryToImageEXT` (see its definition). The name says
        /// so - nothing is staged any more.
        bool host_image_upload(VkImage dst_image, void const* data, VkDeviceSize size, image_create_info const& info);
        [[nodiscard]] std::pair<VkCommandPool, VkCommandBuffer> create_command_pair() const;

    public:
        /**
         * @ingroup vulkan_vma
         * @brief initialize the VMA allocator with the given vulkan objects
         * @param instance the vulkan instance
         * @param device the logical device
         * @param physical_device the physical device
         * @param queue a queue used for staging uploads
         * @param queue_family_index the queue family of the given queue
         */
        void init(VkInstance instance, VkDevice device, VkPhysicalDevice physical_device, VkQueue queue, uint32_t queue_family_index);

        /**
         * @ingroup vulkan_vma
         * @brief hand this allocator the `vkCopyMemoryToImageEXT` entry point the image upload uses
         * @param to_image the resolved entry point; `core` resolves it through `vkGetDeviceProcAddr` at
         *        startup and REFUSES to run without it (VK_EXT_host_image_copy is a required capability)
         * @note stored rather than resolved here: the capability check belongs to the device-creation
         *       path, which is the one place that can fail by name before anything is uploaded
         */
        void set_host_image_copy(PFN_vkCopyMemoryToImageEXT to_image) noexcept {
            this->copy_memory_to_image = to_image;
        }

        /**
         * @ingroup vulkan_vma
         * @brief make host writes visible to the GPU, for the memory type that needs it
         * @param allocation the allocation that was written through its mapping
         * @param memory_type_index its memory type index (VmaAllocationInfo::memoryType)
         * @param offset first byte written
         * @param size bytes written
         * @note a no-op for HOST_COHERENT memory, which is why the coherent buffer types need no call:
         *       this exists for the paths that write through a mapping VMA did not promise coherency
         *       for (or chose not to provide), where skipping it is a silent stale-data bug
         */
        void flush_if_not_coherent(VmaAllocation allocation, uint32_t memory_type_index, VkDeviceSize offset, VkDeviceSize size) const;

        /**
         * @ingroup vulkan_vma
         * @brief make GPU writes visible to the host, for the memory type that needs it
         * @param allocation the allocation the GPU wrote
         * @param memory_type_index its memory type index
         * @param offset first byte to invalidate
         * @param size bytes to invalidate
         * @note the read-back counterpart of flush_if_not_coherent: the HOST_COHERENT types need no
         *       call, but a device that served the allocation from a non-coherent host-visible type
         *       would otherwise hand the CPU a stale cache line
         */
        void invalidate_if_not_coherent(VmaAllocation allocation, uint32_t memory_type_index, VkDeviceSize offset, VkDeviceSize size) const;

        /**
         * @ingroup vulkan_vma
         * @brief release the underlying VmaAllocator and cached upload resources
         */
        void destroy();

        /**
         * @ingroup vulkan_vma
         * @brief create a GPU buffer of the given type and upload the data
         * @param data source bytes to upload
         * @param size_byte byte size of the data
         * @param type buffer usage type
         * @param extra_usage additional VkBufferUsageFlags to OR in
         * @return an owning vk_buffer (copying shares the buffer, destroying releases one
         *         reference); empty when creation failed
         * @note @p extra_usage exists for the bits that are only legal when an OPTIONAL extension is
         *       enabled - the acceleration-structure build input bit on a vertex/index buffer, which
         *       requires VK_KHR_acceleration_structure - so the caller decides, and a device without
         *       the extension never sees the bit. It is OR'd into the type's own usage, never a
         *       replacement for it (a usage bit is not a feature; enabling one the device lacks is a
         *       validation error, not a fallback).
         */
        vk_buffer create_buffer(uint8_t const* data, uint64_t size_byte, buffer_type type, VkBufferUsageFlags extra_usage = 0);

        /**
         * @ingroup vulkan_vma
         * @brief create a GPU buffer from a typed span
         * @tparam T element type of the span
         * @param data source data to upload
         * @param type buffer usage type
         * @param extra_usage additional VkBufferUsageFlags to OR in (see the byte-span overload)
         * @return an owning vk_buffer (see create_buffer)
         */
        template <typename T>
        vk_buffer create_buffer(std::span<T> data, buffer_type const type, VkBufferUsageFlags const extra_usage = 0) {
            return this->create_buffer(reinterpret_cast<uint8_t*>(data.data()), data.size_bytes(), type, extra_usage);
        }

        /**
         * @ingroup vulkan_vma
         * @brief create a GPU buffer from a fixed-size typed span
         * @tparam T element type of the span
         * @tparam N element count of the span
         * @param data source data to upload
         * @param type buffer usage type
         * @param extra_usage additional VkBufferUsageFlags to OR in (see the byte-span overload)
         * @return an owning vk_buffer (see create_buffer)
         */
        template <typename T, std::size_t N>
        vk_buffer create_buffer(std::span<T, N> data, buffer_type const type, VkBufferUsageFlags const extra_usage = 0) {
            return this->create_buffer(reinterpret_cast<uint8_t*>(data.data()), data.size_bytes(), type, extra_usage);
        }

        /**
         * @ingroup vulkan_vma
         * @brief create a GPU image of the given type and upload the data
         * @param data source bytes to upload
         * @param size_byte byte size of the data
         * @param create_info image width/height/format/mip levels etc.
         * @param type image usage type
         * @return an owning vk_image (copying shares the image, destroying releases one
         *         reference); empty when creation failed
         */
        vk_image create_image(uint8_t const* data, uint64_t size_byte, image_create_info const& create_info, image_type type);

        /**
         * @ingroup vulkan_vma
         * @brief create a GPU image from a typed span
         * @tparam T element type of the span
         * @param data source data to upload
         * @param create_info image width/height/format/mip levels etc.
         * @param type image usage type
         * @return the image handle
         */
        // `std::span<T const>` rather than `std::span<T>`: a span of const elements converts to it implicitly,
        // while the reverse needs a const_cast - and an upload source is exactly what must not be written to.
        //
        // THE ARGUMENT ORDER HERE WAS WRONG for as long as this overload has existed - `create_info` and
        // `size_bytes()` in each other's place against the pointer overload above - and NOTHING CAUGHT IT,
        // because no caller had ever instantiated the template. A defect in an uninstantiated template is not
        // a build failure; it is a trap. init_utils' texture upload now calls this overload, so the gate
        // compiles and runs it.
        template <typename T>
        vk_image create_image(std::span<T const> data, image_create_info create_info, image_type const type) {
            return this->create_image(reinterpret_cast<uint8_t const*>(data.data()), data.size_bytes(), create_info, type);
        }

        /**
         * @ingroup vulkan_vma
         * @brief create a GPU image from a fixed-size typed span
         * @tparam T element type of the span
         * @tparam N element count of the span
         * @param data source data to upload
         * @param create_info image width/height/format/mip levels etc.
         * @param type image usage type
         * @return an owning vk_image (see create_image)
         */
        template <typename T, size_t N>
        vk_image create_image(std::span<T const, N> data, image_create_info create_info, image_type const type) {
            return this->create_image(reinterpret_cast<uint8_t const*>(data.data()), data.size_bytes(), create_info, type);
        }

        /**
         * @ingroup vulkan_vma
         * @brief get the detail of a buffer by its handle
         * @param handle the buffer handle
         * @return pointer to the buffer detail, or nullptr if the handle is invalid
         */
        [[nodiscard]] buffer_detail const* get_buffer_detail(uint64_t handle);

        /**
         * @ingroup vulkan_vma
         * @brief get the detail of an image by its handle
         * @param handle the image handle
         * @return pointer to the image detail, or nullptr if the handle is invalid
         */
        [[nodiscard]] image_detail const* get_image_detail(uint64_t handle);

        /**
         * @brief log what this allocator holds: requested allocation bytes against the block bytes VMA
         *        took from the driver, per memory type, plus the per-heap budgets
         * @ingroup vulkan_vma
         *
         * Diagnostic for the process's commit charge: a block-per-small-allocation pattern shows up as
         * blockBytes an order of magnitude above allocationBytes, which is a block-size question
         * (preferredLargeHeapBlockSize) rather than real usage.
         */
        void log_statistics() const;
    };
} // namespace deren::vulkan

namespace {
    constexpr uint32_t sizeof_vk_format(VkFormat const format) {
        switch (format) {
        // 8-bit single channel
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_USCALED:
        case VK_FORMAT_R8_SSCALED:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8_SRGB:
            return 1;

        // 16-bit single channel / 8-bit dual channel
        case VK_FORMAT_R16_UNORM:
        case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_USCALED:
        case VK_FORMAT_R16_SSCALED:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_USCALED:
        case VK_FORMAT_R8G8_SSCALED:
        case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R8G8_SRGB:
            return 2;

        // 24-bit
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SNORM:
        case VK_FORMAT_R8G8B8_USCALED:
        case VK_FORMAT_R8G8B8_SSCALED:
        case VK_FORMAT_R8G8B8_UINT:
        case VK_FORMAT_R8G8B8_SINT:
        case VK_FORMAT_R8G8B8_SRGB:
            return 3;

        // 32-bit
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_USCALED:
        case VK_FORMAT_R16G16_SSCALED:
        case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_USCALED:
        case VK_FORMAT_R8G8B8A8_SSCALED:
        case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SNORM:
        case VK_FORMAT_B8G8R8A8_USCALED:
        case VK_FORMAT_B8G8R8A8_SSCALED:
        case VK_FORMAT_B8G8R8A8_UINT:
        case VK_FORMAT_B8G8R8A8_SINT:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UINT_PACK32:
            return 4;

        // 64-bit
        case VK_FORMAT_R64_UINT:
        case VK_FORMAT_R64_SINT:
        case VK_FORMAT_R64_SFLOAT:
        case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_USCALED:
        case VK_FORMAT_R16G16B16A16_SSCALED:
        case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 8;

        // 96-bit
        case VK_FORMAT_R32G32B32_UINT:
        case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT:
            return 12;

        // 128-bit
        case VK_FORMAT_R64G64_UINT:
        case VK_FORMAT_R64G64_SINT:
        case VK_FORMAT_R64G64_SFLOAT:
        case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;

        // Depth/stencil
        case VK_FORMAT_D16_UNORM:
            return 2;
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
            return 4;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return 8;
        // Stencil-only: one byte per texel. It used to fall through into the BC1 group below and be
        // reported as 8.
        case VK_FORMAT_S8_UINT:
            return 1;

        // BC compressed formats
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
            return 8;

        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC2_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_BC4_SNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC5_SNORM_BLOCK:
        case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        // ASTC compressed formats (all are 16 bytes/block)
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x4_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_5x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x5_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x6_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x8_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
        case VK_FORMAT_ASTC_10x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x10_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
        case VK_FORMAT_ASTC_12x12_UNORM_BLOCK:
        case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
            return 16;

        // ETC2 / EAC compressed formats (8 bytes/block)
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
            return 8;

        default:
            // Deliberately not 0: a 0 would zero every mip's host pointer offset in host_image_upload()
            // and make the size check below compare against nothing, i.e. a silently wrong upload
            // (or a meaningless region layout) instead of a failure. Every format this engine uploads
            // is listed above, so a miss here is a bug in the table, not a caller error.
            deren::utility::error("sizeof_vk_format: unsupported VkFormat {}", static_cast<int32_t>(format));
            deren::utility::panic("sizeof_vk_format: unsupported VkFormat");
        }
    }

    constexpr VmaAllocationCreateInfo get_allocation_info_from_type(deren::vulkan::buffer_type const type) {
        VmaAllocationCreateInfo info = {};
        switch (type) {
        case deren::vulkan::buffer_type::vertex:
            [[fallthrough]];
        case deren::vulkan::buffer_type::index:
            [[fallthrough]];
        case deren::vulkan::buffer_type::uniform_gpu_only: {
            info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            break;
        }
        case deren::vulkan::buffer_type::acceleration_structure_storage:
            [[fallthrough]];
        case deren::vulkan::buffer_type::acceleration_structure_scratch:
            [[fallthrough]];
        case deren::vulkan::buffer_type::storage_gpu_only: {
            // All three are device-local and host-untouched: an AS, its scratch and a compute-written
            // storage buffer are filled by GPU work and never mapped, so there is nothing to keep coherent.
            info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            break;
        }
        case deren::vulkan::buffer_type::uniform_coherent: {
            info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            // per-frame uniforms written straight into the persistent mapping with no flush: the
            // coherence the type documents has to be REQUIRED of the driver, not merely hoped for
            // (see coherent_host_visible_flags)
            info.requiredFlags = deren::vulkan::coherent_host_visible_flags;
            info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            break;
        }
        case deren::vulkan::buffer_type::uniform_cached: {
            info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
            info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            break;
        }
        case deren::vulkan::buffer_type::storage_coherent: {
            info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            info.requiredFlags = deren::vulkan::coherent_host_visible_flags; // see uniform_coherent above
            info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            break;
        }
        case deren::vulkan::buffer_type::readback_coherent: {
            // written by the GPU (transfer), read by the host: host-visible local memory, mapped
            // for random access reads
            info.usage = VMA_MEMORY_USAGE_CPU_ONLY;
            info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
            break;
        }
        }
        return info;
    }

    constexpr VkBufferCreateInfo get_create_info_from_type(deren::vulkan::buffer_type const type) {
        VkBufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;

        switch (type) {
        case deren::vulkan::buffer_type::vertex: {
            info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            break;
        }
        case deren::vulkan::buffer_type::index: {
            info.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            break;
        }
        case deren::vulkan::buffer_type::uniform_gpu_only: {
            info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            break;
        }
        case deren::vulkan::buffer_type::uniform_coherent:
        case deren::vulkan::buffer_type::uniform_cached: {
            info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            break;
        }
        case deren::vulkan::buffer_type::storage_coherent: {
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            break;
        }
        case deren::vulkan::buffer_type::readback_coherent: {
            info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            break;
        }
        case deren::vulkan::buffer_type::acceleration_structure_storage: {
            info.usage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            break;
        }
        case deren::vulkan::buffer_type::acceleration_structure_scratch: {
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            break;
        }
        case deren::vulkan::buffer_type::storage_gpu_only: {
            info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            break;
        }
        }
        return info;
    }

    constexpr VmaAllocationCreateInfo get_image_allocation_info_from_type(deren::vulkan::image_type const type) {
        VmaAllocationCreateInfo info = {};

        switch (type) {
        case deren::vulkan::image_type::texture_2d:
        case deren::vulkan::image_type::texture_2d_color:
        case deren::vulkan::image_type::texture_2d_depth:
        case deren::vulkan::image_type::texture_cubemap:
        case deren::vulkan::image_type::render_target: {
            info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            break;
        }
        case deren::vulkan::image_type::texture_2d_staging: {
            info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            // written through the persistent mapping by direct_image_upload, which (like the buffer
            // types above) does not flush on the per-frame path - so require coherence rather than
            // hoping for it (see coherent_host_visible_flags)
            info.requiredFlags = deren::vulkan::coherent_host_visible_flags;
            info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            break;
        }
        }
        return info;
    }

    constexpr VkImageCreateInfo get_image_create_info_from_type(
        deren::vulkan::image_type const type,
        deren::vulkan::image_create_info const& info) {
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.extent.width = info.width;
        image_info.extent.height = info.height;
        image_info.extent.depth = 1;
        image_info.mipLevels = info.mip_levels;
        image_info.arrayLayers = info.array_layers;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image_info.format = info.format;

        switch (type) {
        case deren::vulkan::image_type::texture_2d:
            image_info.imageType = VK_IMAGE_TYPE_2D;
            // HOST_TRANSFER is what the ONE upload mechanism needs (vkCopyMemoryToImageEXT, see
            // host_image_upload): the image is written by the implementation from the caller's memory, so
            // the usage bit follows the mechanism rather than a choice between mechanisms.
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT |
                               info.extra_usage;
            break;

        case deren::vulkan::image_type::texture_2d_staging:
            // LINEAR tiling is what makes this type legal at all: it is uploaded by
            // direct_image_upload, which writes raw pixel bytes through the host mapping, and the
            // layout of an OPTIMAL-tiled image is implementation-defined - the write would land in
            // undefined places (vkGetImageSubresourceLayout, the API that would describe it, is only
            // valid for LINEAR). Being host-visible, it is also created so the caller can write it
            // again at any time; the caller owns the layout transitions around that.
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.tiling = VK_IMAGE_TILING_LINEAR;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               info.extra_usage;
            break;

        case deren::vulkan::image_type::texture_2d_color:
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT | // see texture_2d above
                               info.extra_usage;
            break;

        case deren::vulkan::image_type::texture_2d_depth:
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               info.extra_usage;
            break;

        case deren::vulkan::image_type::texture_cubemap:
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT | // see texture_2d above
                               info.extra_usage;
            break;

        case deren::vulkan::image_type::render_target:
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT | // see texture_2d above: a target WITH data is uploaded the same way
                               info.extra_usage;
            break;
        }

        return image_info;
    }
} // namespace

namespace deren::vulkan {
    void vma_allocator::init(
        VkInstance const instance,              // NOLINT(*-misplaced-const)
        VkDevice const device,                  // NOLINT(*-misplaced-const)
        VkPhysicalDevice const physical_device, // NOLINT(*-misplaced-const)
        VkQueue const queue,                    // NOLINT(*-misplaced-const)
        uint32_t const queue_family_index) {

        std::lock_guard guard(this->access_mutex);
        if (this->allocator != VK_NULL_HANDLE) {
            return;
        }

        VmaAllocatorCreateInfo vma_allocator_create_info = {};
        vma_allocator_create_info.instance = instance;
        vma_allocator_create_info.device = device;
        vma_allocator_create_info.physicalDevice = physical_device;
        // BUFFER_DEVICE_ADDRESS is what lets a buffer carry VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        // at all: without it VMA refuses the combination (a debug assert that disappears in a release
        // build, leaving a plain -3 from vmaCreateBuffer) because it would have to add
        // VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT to the allocation and was not told it may. The
        // acceleration-structure build reads the scene's vertex and index buffers through exactly those
        // addresses, so this is a requirement of ray tracing, not an optimization.
        //
        // Unconditional, because it is not an optional feature: VMA only adds the memory flag for a
        // buffer that asks for the usage bit, and the engine enables the core 1.2 bufferDeviceAddress
        // feature by policy (all supported 1.2 features are passed through to vkCreateDevice). A device
        // without it could not run the 1.3 paths this engine requires anyway.
        vma_allocator_create_info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

        vmaCreateAllocator(&vma_allocator_create_info, &this->allocator);

        this->logical_device = device;
        this->upload_queue = queue;
        this->upload_queue_family_index = queue_family_index;
        this->command_cache.push_back(this->create_command_pair());
    }

    bool vma_allocator::is_host_coherent(uint32_t const memory_type_index) const noexcept {
        VkMemoryPropertyFlags properties = 0;
        vmaGetMemoryTypeProperties(this->allocator, memory_type_index, &properties);
        return (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }

    void vma_allocator::flush_if_not_coherent(VmaAllocation const allocation, uint32_t const memory_type_index, VkDeviceSize const offset, VkDeviceSize const size) const {
        if (!this->is_host_coherent(memory_type_index)) {
            vmaFlushAllocation(this->allocator, allocation, offset, size);
        }
    }

    void vma_allocator::invalidate_if_not_coherent(VmaAllocation const allocation, uint32_t const memory_type_index, VkDeviceSize const offset, VkDeviceSize const size) const {
        if (!this->is_host_coherent(memory_type_index)) {
            vmaInvalidateAllocation(this->allocator, allocation, offset, size);
        }
    }

    void vma_allocator::destroy() {
        if (this->allocator != VK_NULL_HANDLE) {
            std::lock_guard guard(this->access_mutex);
            for (auto const& buffer : buffers | std::ranges::views::values) {
                vmaDestroyBuffer(this->allocator, buffer.buffer, buffer.allocation);
            }
            this->buffers.clear();

            for (auto const& image : images | std::ranges::views::values) {
                vmaDestroyImage(this->allocator, image.image, image.allocation);
            }
            this->images.clear();

            for (auto const& fence : this->fence_cache) {
                vkDestroyFence(this->logical_device, fence, nullptr);
            }
            this->fence_cache.clear();
            for (auto& command_pool : this->command_cache | std::views::keys) {
                vkDestroyCommandPool(this->logical_device, command_pool, nullptr);
                command_pool = VK_NULL_HANDLE;
            }
            // Release the cached staging buffer
            if (this->staging_cache.buffer != VK_NULL_HANDLE) {
                vmaDestroyBuffer(this->allocator, this->staging_cache.buffer, this->staging_cache.allocation);
                this->staging_cache = {};
                this->buffer_size = 0;
            }

            vmaDestroyAllocator(this->allocator);
            this->allocator = VK_NULL_HANDLE;
        }
    }

    VkFence vma_allocator::create_fence() const {

        VkFence fence = VK_NULL_HANDLE;

        VkFenceCreateInfo fence_create_info = make_fence_info();
        vkCreateFence(this->logical_device, &fence_create_info, nullptr, &fence);

        return fence;
    }

    bool vma_allocator::ensure_staging_buffer(VkDeviceSize const size, VkBuffer& buffer, VmaAllocation& allocation, VmaAllocationInfo& info) {
        // Reuse if the cached capacity is sufficient
        if (this->staging_cache.buffer != VK_NULL_HANDLE && this->buffer_size >= size) {
            buffer = this->staging_cache.buffer;
            allocation = this->staging_cache.allocation;
            info = this->staging_cache.allocation_info;
            return true;
        }

        // Not enough capacity: destroy the current cache and re-create
        if (this->staging_cache.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(this->allocator, this->staging_cache.buffer, this->staging_cache.allocation);
            this->staging_cache = {};
            this->buffer_size = 0;
        }

        VkBufferCreateInfo staging_create_info = make_staging_buffer_info(size);

        VmaAllocationCreateInfo staging_alloc_info = {};
        staging_alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        staging_alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        VkResult const result = vmaCreateBuffer(
            this->allocator,
            &staging_create_info,
            &staging_alloc_info,
            &buffer,
            &allocation,
            &info);

        if (result != VK_SUCCESS) {
            deren::utility::error("Failed to create staging buffer: {}", static_cast<int32_t>(result));
            return false;
        }

        this->staging_cache = {buffer, allocation, info};
        this->buffer_size = size;
        return true;
    }

    bool vma_allocator::direct_upload(VmaAllocation const& allocation, VmaAllocationInfo& allocation_info, void const* data, VkDeviceSize const size) const {
        void* mapped_data = nullptr;
        if (VkResult const result = vmaMapMemory(this->allocator, allocation, &mapped_data); result != VK_SUCCESS) {
            deren::utility::error("Failed to map memory: {}", static_cast<int32_t>(result));
            return false;
        }

        // a null data pointer means "allocate only" (the read-back buffer type has no initial
        // contents - the GPU fills it)
        if (data != nullptr && size != 0) {
            memcpy(mapped_data, data, size);
        }

        vmaGetAllocationInfo(this->allocator, allocation, &allocation_info);
        if (!this->is_host_coherent(allocation_info.memoryType)) {
            vmaFlushAllocation(this->allocator, allocation, 0, size);
        }

        vmaUnmapMemory(this->allocator, allocation);
        return true;
    }

    bool vma_allocator::staging_upload(VkBuffer const dst_buffer, void const* data, VkDeviceSize const size) { // NOLINT(*-misplaced-const)
        VkBuffer staging_buffer = VK_NULL_HANDLE;
        VmaAllocation staging_allocation = VK_NULL_HANDLE;
        VmaAllocationInfo staging_info = {};

        // The staging buffer is shared; it must be held exclusively during writes + GPU copies
        std::lock_guard staging_guard(this->staging_mutex);

        // Reuse the cached staging buffer, rebuilding automatically when too small
        if (!this->ensure_staging_buffer(size, staging_buffer, staging_allocation, staging_info)) {
            return false;
        }

        // Copy data
        if (staging_info.pMappedData) {
            memcpy(staging_info.pMappedData, data, size);
            if (!this->is_host_coherent(staging_info.memoryType)) {
                vmaFlushAllocation(this->allocator, staging_allocation, 0, size);
            }
        } else {
            return false;
        }
        // Execute the copy command (cache access guarded by cache_mutex)
        std::pair<VkCommandPool, VkCommandBuffer> command_pair;
        VkFence fence = VK_NULL_HANDLE;
        {
            std::lock_guard guard(this->cache_mutex);
            if (!this->command_cache.empty()) {
                command_pair = this->command_cache.back();
                this->command_cache.pop_back();
            } else {
                command_pair = this->create_command_pair();
            }

            if (!this->fence_cache.empty()) {
                fence = this->fence_cache.back();
                this->fence_cache.pop_back();
            } else {
                fence = this->create_fence();
            }
        }

        VkCommandBuffer command_buffer = command_pair.second;

        VkCommandBufferBeginInfo begin_info = make_command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr);
        vkBeginCommandBuffer(command_buffer, &begin_info);

        VkBufferCopy copy_region = {};
        copy_region.size = size;
        vkCmdCopyBuffer(command_buffer, staging_buffer, dst_buffer, 1, &copy_region);

        vkEndCommandBuffer(command_buffer);

        VkSubmitInfo submit_info = make_submit_info(&command_buffer);

        {
            // VkQueue is externally synchronized; submits must be serialized
            std::lock_guard guard(this->queue_mutex);
            vkQueueSubmit(this->upload_queue, 1, &submit_info, fence);
        }

        vkWaitForFences(this->logical_device, 1, &fence, VK_TRUE, UINT64_MAX);

        // Cleanup
        vkResetFences(this->logical_device, 1, &fence);
        vkResetCommandBuffer(command_buffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
        {
            std::lock_guard guard(this->cache_mutex);
            this->fence_cache.push_back(fence);
            this->command_cache.push_back(command_pair);
        }

        return true;
    }

    bool vma_allocator::direct_image_upload(VmaAllocation const allocation, void const* data, VkDeviceSize const size) const { // NOLINT(*-misplaced-const)
        void* mapped_data = nullptr;
        if (VkResult const result = vmaMapMemory(this->allocator, allocation, &mapped_data); result != VK_SUCCESS) {
            deren::utility::error("Failed to map image memory: {}", static_cast<int32_t>(result));
            return false;
        }

        // a null data pointer means "allocate only", the same contract direct_upload() honours for
        // the buffer types: memcpy from null with a non-zero size is UB, not an empty write
        if (data != nullptr && size != 0) {
            memcpy(mapped_data, data, size);
        }

        VmaAllocationInfo alloc_info;
        vmaGetAllocationInfo(this->allocator, allocation, &alloc_info);
        if (!this->is_host_coherent(alloc_info.memoryType)) {
            vmaFlushAllocation(this->allocator, allocation, 0, size);
        }

        vmaUnmapMemory(this->allocator, allocation);
        return true;
    }

    bool vma_allocator::host_image_upload(VkImage dst_image, void const* data, VkDeviceSize size, image_create_info const& info) {
        // ---- THE IMAGE UPLOAD, AND THE ONLY ONE: VK_EXT_host_image_copy. The implementation performs the
        //      copy between the caller's memory and the image, so there is no staging buffer, no
        //      VkBufferImageCopy and no copy command - and no second path to choose between. It needs two
        //      things of the image, and both are OURS to provide: the VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT
        //      usage bit (asked for at creation; see get_image_create_info_from_type) and the GENERAL
        //      layout, which this renderer keeps every image in (VK_KHR_unified_image_layouts). The
        //      extension is a REQUIRED device capability checked at startup, so reaching here means both
        //      entry points resolved and GENERAL is in the device's copy-destination list.
        if (data == nullptr || size == 0) {
            return true; // an empty image (a shadow map or a target that is only rendered into) has no payload
        }

        std::pair<VkCommandPool, VkCommandBuffer> command_pair;
        VkFence fence = VK_NULL_HANDLE;
        {
            std::lock_guard guard(this->cache_mutex);
            if (!this->command_cache.empty()) {
                command_pair = this->command_cache.back();
                this->command_cache.pop_back();
            } else {
                command_pair = this->create_command_pair();
            }
            if (!this->fence_cache.empty()) {
                fence = this->fence_cache.back();
                this->fence_cache.pop_back();
            } else {
                fence = this->create_fence();
            }
        }
        VkCommandBuffer const command_buffer = command_pair.second;

        auto const release_pair = [&]() {
            std::lock_guard guard(this->cache_mutex);
            this->fence_cache.push_back(fence);
            this->command_cache.push_back(command_pair);
        };

        // ONE barrier, recorded into the shared command buffer, submitted and waited for. The host copy
        // between the two calls is NOT a queue operation, so the ordering around it has to be spelled as
        // two submissions: "make the image available TO THE HOST" before, "publish the host's writes to
        // the shaders" after. HOST as a pipeline stage is what carries both directions.
        auto const submit_barrier = [&](VkImageLayout const old_layout, VkImageLayout const new_layout, VkAccessFlags const src_access, VkAccessFlags const dst_access,
                                        VkPipelineStageFlags const src_stage, VkPipelineStageFlags const dst_stage) {
            VkCommandBufferBeginInfo const begin_info = make_command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr);
            vkBeginCommandBuffer(command_buffer, &begin_info);

            VkImageMemoryBarrier barrier = {};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = old_layout;
            barrier.newLayout = new_layout;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = dst_image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.levelCount = info.mip_levels;
            barrier.subresourceRange.layerCount = info.array_layers;
            barrier.srcAccessMask = src_access;
            barrier.dstAccessMask = dst_access;

            vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            vkEndCommandBuffer(command_buffer);

            VkSubmitInfo const submit_info = make_submit_info(&command_buffer);
            {
                // VkQueue is externally synchronized; submits must be serialized
                std::lock_guard guard(this->queue_mutex);
                vkQueueSubmit(this->upload_queue, 1, &submit_info, fence);
            }
            vkWaitForFences(this->logical_device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(this->logical_device, 1, &fence);
            vkResetCommandBuffer(command_buffer, VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT);
        };

        // Step 1: UNDEFINED -> GENERAL, and the transition's target stage is the HOST: the image is not
        // being written by a transfer command any more, it is about to be written BY THE CPU through the
        // extension, so HOST_WRITE (not TRANSFER_WRITE) is the access that has to be made available.
        submit_barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_HOST_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_HOST_BIT);

        // Step 2: the copy itself, one region per mip. The data is laid out in mip-major order (all layers
        // of mip 0, then all layers of mip 1, ...) exactly as the staging path's VkBufferImageCopy
        // described it with bufferOffset; here the same offset is a POINTER into the caller's memory.
        uint32_t const bytes_per_pixel = sizeof_vk_format(info.format);
        std::vector<VkMemoryToImageCopyEXT> regions;
        regions.reserve(info.mip_levels);
        VkDeviceSize memory_offset = 0;
        for (uint32_t mip = 0; mip < info.mip_levels; ++mip) {
            uint32_t const mip_width = std::max(1u, info.width >> mip);
            uint32_t const mip_height = std::max(1u, info.height >> mip);

            VkMemoryToImageCopyEXT region = {};
            region.sType = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT;
            region.pNext = nullptr;
            region.pHostPointer = static_cast<uint8_t const*>(data) + memory_offset;
            region.memoryRowLength = 0; // 0 means tightly packed
            region.memoryImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = mip;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = info.array_layers;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {mip_width, mip_height, 1};
            regions.push_back(region);

            memory_offset += static_cast<VkDeviceSize>(mip_width) * mip_height * info.array_layers * bytes_per_pixel;
        }
        if (memory_offset > size) {
            // The caller already refuses a short payload (see create_image's expected-size check); this is
            // the same fact where the per-mip offsets are computed, and it is the reason that check exists.
            deren::utility::error("host image upload: the per-mip layout needs {} bytes, the caller provided {}", memory_offset, size);
            release_pair();
            return false;
        }

        VkCopyMemoryToImageInfoEXT const copy_info = {.sType = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT,
                                                      .pNext = nullptr,
                                                      .flags = 0,
                                                      .dstImage = dst_image,
                                                      .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                      .regionCount = static_cast<uint32_t>(regions.size()),
                                                      .pRegions = regions.data()};
        VkResult const copied = this->copy_memory_to_image(this->logical_device, &copy_info);
        if (copied != VK_SUCCESS) {
            deren::utility::error("host image upload: vkCopyMemoryToImageEXT failed (VkResult {})", static_cast<int32_t>(copied));
            release_pair();
            return false;
        }

        // Step 3: publish it to the fragment stage (GENERAL -> GENERAL; the barrier is the dependency). The
        // source side is the HOST stage, because that is where the writes came from.
        submit_barrier(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        release_pair();
        return true;
    }
    vk_buffer vma_allocator::create_buffer(uint8_t const* data, uint64_t const size_byte, buffer_type const type, VkBufferUsageFlags const extra_usage) {
        // the returned owner carries lambdas that call back into this allocator; they are
        // created here (a member function), so they may call the private free/retain below
        auto const make_owner = [this](uint64_t const handle) {
            return vk_buffer{handle,
                             [this](uint64_t const h) { this->retain_buffer(h); },
                             [this](uint64_t const h) { this->free_buffer(h); }};
        };

        uint64_t handle = 0;
        // distribute() locks internally (enable_handle_distribute::access_mutex); no outer lock needed
        if (auto const result = this->distribute(); result) {
            handle = result.value();
        } else {
            return vk_buffer{}; // no handle left to hand out
        }

        auto const allocation_create_info = get_allocation_info_from_type(type);
        auto buffer_create_info = get_create_info_from_type(type);
        buffer_create_info.size = size_byte;
        // The caller's optional bits (see the header: they exist for usage that needs an optional
        // extension, so they are added here rather than baked into the type's own usage).
        buffer_create_info.usage |= extra_usage;

        VmaAllocation allocation = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocationInfo alloc_info = {};

        // VMA is internally thread-safe; allocation and upload need no access_mutex
        VkResult const result = vmaCreateBuffer(
            this->allocator,
            &buffer_create_info,
            &allocation_create_info,
            &buffer,
            &allocation,
            &alloc_info);

        if (result != VK_SUCCESS) {
            deren::utility::error("Failed to create buffer: {}", static_cast<int32_t>(result));
            this->recycle(handle);
            return vk_buffer{};
        }

        bool upload_success = false;
        switch (type) {
        case buffer_type::uniform_coherent:
        case buffer_type::uniform_cached:
        case buffer_type::storage_coherent:
        case buffer_type::readback_coherent:
            // Upload via direct mapping (a null data pointer means "allocate only": the read-back
            // type has nothing to upload, the GPU fills it later)
            upload_success = direct_upload(allocation, alloc_info, data, size_byte);
            break;

        case buffer_type::vertex:
        case buffer_type::index:
        case buffer_type::uniform_gpu_only:
            // Use a staging buffer
            upload_success = staging_upload(buffer, data, size_byte);
            break;

        case buffer_type::acceleration_structure_storage:
        case buffer_type::acceleration_structure_scratch:
        case buffer_type::storage_gpu_only:
            // Allocate only, and REFUSE to take contents: these are filled by GPU work (a build command, or
            // a compute pass for the mask bake), so a data pointer here is a caller that meant a different
            // buffer type - and staging_upload() would memcpy from it rather than say so.
            upload_success = data == nullptr;
            break;
        }

        if (!upload_success) {
            vmaDestroyBuffer(this->allocator, buffer, allocation);
            this->recycle(handle);
            deren::utility::panic("Failed to upload buffer");
        }

        {
            std::lock_guard guard(this->access_mutex);
            // built on the stack, then moved in (use_count keeps its default 1)
            buffer_detail detail;
            detail.buffer = buffer;
            detail.allocation = allocation;
            detail.allocation_info = alloc_info;
            this->buffers.emplace(handle, std::move(detail));
        }
        return make_owner(handle);
    }

    vk_image vma_allocator::create_image(uint8_t const* data, uint64_t const size_byte, image_create_info const& create_info, image_type const type) {
        // the returned owner carries lambdas that call back into this allocator; they are
        // created here (a member function), so they may call the private free/retain below
        auto const make_owner = [this](uint64_t const handle) {
            return vk_image{handle,
                            [this](uint64_t const h) { this->retain_image(h); },
                            [this](uint64_t const h) { this->free_image(h); }};
        };

        // XXH3-128 is pure CPU work; keep it outside the critical section. It is computed
        // before allocating so a content hit can reuse an existing image without any allocation
        // or upload. Empty images (data == nullptr, e.g. a depth shadow map that is rendered
        // into, never uploaded) have no content digest (all-zero) and are never deduplicated.
        // 128 bits (not 64): a wrong dedup hit would silently render the wrong texture, so the
        // digest collision chance should be negligible even across large scenes.
        deren::utility::xxh3_digest digest = {};
        if (data != nullptr && size_byte != 0) {
            digest = deren::utility::xxh3_128bits(std::span(data, size_byte));
        }

        // Only immutable, data-uploaded textures are shareable: depth / staging / render targets
        // must stay distinct even with identical parameters.
        constexpr auto is_dedupable = [](image_type const t) {
            return t == image_type::texture_2d || t == image_type::texture_2d_color || t == image_type::texture_cubemap;
        };
        if (is_dedupable(type) && digest != deren::utility::xxh3_digest{}) {
            std::lock_guard guard(this->access_mutex);
            for (auto& [existing_handle, detail] : this->images) {
                if (detail.type == type && detail.create_info == create_info && detail.digest == digest) {
                    detail.use_count.fetch_add(1); // shared: bump the reference count and reuse
                    return make_owner(existing_handle);
                }
            }
        }

        uint64_t handle = 0;
        // distribute() locks internally (enable_handle_distribute::access_mutex); no outer lock needed
        if (auto const result = this->distribute(); result) {
            handle = result.value();
        } else {
            return vk_image{}; // no handle left to hand out
        }

        VkDeviceSize const image_size = size_byte;

        // Expected size = array_layers * sum of all mip sizes * bytes per pixel; only meaningful
        // for images that carry uploaded data (empty render-target images have no payload), which is
        // why the whole computation - format lookup included - is skipped for them.
        if (data != nullptr) {
            VkDeviceSize expected_size = 0;
            for (uint32_t mip = 0; mip < create_info.mip_levels; ++mip) {
                expected_size += static_cast<VkDeviceSize>(std::max(1u, create_info.width >> mip)) *
                                 std::max(1u, create_info.height >> mip) *
                                 sizeof_vk_format(create_info.format);
            }
            expected_size *= create_info.array_layers;
            if (expected_size > image_size) {
                // REFUSE, rather than log and carry on. host_image_upload() below lays the per-mip
                // regions out from `expected_size` (the pointer-offset accumulation) while the caller's
                // memory holds `image_size` bytes, so continuing here turns a caller's size mistake into
                // a vkCopyMemoryToImageEXT that reads past the end of that memory - a host-side
                // out-of-range access, not a cosmetic log line. The other direction (expected < given)
                // only leaves part of the payload unread, so it stays a log.
                deren::utility::error("incorrect image size [{}], expected [{}] - refusing the upload", image_size, expected_size);
                this->recycle(handle);
                return vk_image{};
            }
            if (expected_size != image_size) {
                deren::utility::log("incorrect image size [{}], expected [{}]", image_size, expected_size);
            }
        }

        auto const alloc_info = get_image_allocation_info_from_type(type);
        auto image_create_info = get_image_create_info_from_type(type, create_info);
        image_create_info.mipLevels = create_info.mip_levels;
        image_create_info.extent.width = create_info.width;
        image_create_info.extent.height = create_info.height;
        image_create_info.extent.depth = 1;
        image_create_info.arrayLayers = create_info.array_layers;

        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VmaAllocationInfo alloc_detail = {};

        VkResult const vk_result = vmaCreateImage(
            this->allocator,
            &image_create_info,
            &alloc_info,
            &image,
            &allocation,
            &alloc_detail);

        if (vk_result != VK_SUCCESS) {
            deren::utility::error("Failed to create image: {}", static_cast<int32_t>(vk_result));
            this->recycle(handle);
            return vk_image{};
        }

        // Pick the upload path based on the type
        bool upload_success = false;
        switch (type) {
        case image_type::texture_2d_staging:
            upload_success = direct_image_upload(allocation, data, image_size);
            break;

        case image_type::texture_2d:
        case image_type::texture_2d_color:
        case image_type::texture_cubemap:
        case image_type::render_target:
            upload_success = host_image_upload(image, data, image_size, create_info);
            break;

        case image_type::texture_2d_depth:
            upload_success = true;
            break;
        }

        if (!upload_success) {
            vmaDestroyImage(this->allocator, image, allocation);
            this->recycle(handle);
            return vk_image{};
        }

        {
            std::lock_guard guard(this->access_mutex);
            // built on the stack, then moved in (use_count keeps its default 1)
            image_detail detail;
            detail.image = image;
            detail.allocation = allocation;
            detail.allocation_info = alloc_detail;
            detail.digest = digest;
            detail.create_info = create_info;
            detail.type = type;
            this->images.emplace(handle, std::move(detail));
        }
        return make_owner(handle);
    }

    buffer_detail const* vma_allocator::get_buffer_detail(uint64_t const handle) {
        std::lock_guard guard(this->access_mutex);
        if (this->buffers.contains(handle)) {
            return &this->buffers[handle];
        }
        return nullptr;
    }

    void vma_allocator::log_statistics() const {
        VmaTotalStatistics totals = {};
        vmaCalculateStatistics(this->allocator, &totals);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
        vmaGetHeapBudgets(this->allocator, budgets);
        deren::utility::log("vma: {} allocations in {} blocks | requested {:.1f} MB | blocks {:.1f} MB",
                            totals.total.statistics.allocationCount,
                            totals.total.statistics.blockCount,
                            static_cast<double>(totals.total.statistics.allocationBytes) / (1024.0 * 1024.0),
                            static_cast<double>(totals.total.statistics.blockBytes) / (1024.0 * 1024.0));
        for (uint32_t type = 0; type < VK_MAX_MEMORY_TYPES; ++type) {
            if (totals.memoryType[type].statistics.blockCount == 0) {
                continue;
            }
            deren::utility::log("vma:   memory type {}: {} allocations, requested {:.1f} MB, blocks {:.1f} MB",
                                type,
                                totals.memoryType[type].statistics.allocationCount,
                                static_cast<double>(totals.memoryType[type].statistics.allocationBytes) / (1024.0 * 1024.0),
                                static_cast<double>(totals.memoryType[type].statistics.blockBytes) / (1024.0 * 1024.0));
        }
        for (uint32_t heap = 0; heap < VK_MAX_MEMORY_HEAPS; ++heap) {
            if (budgets[heap].budget == 0) {
                continue;
            }
            deren::utility::log("vma:   heap {}: usage {:.1f} MB of budget {:.1f} MB ({:.1f} MB of blocks)",
                                heap,
                                static_cast<double>(budgets[heap].usage) / (1024.0 * 1024.0),
                                static_cast<double>(budgets[heap].budget) / (1024.0 * 1024.0),
                                static_cast<double>(budgets[heap].statistics.blockBytes) / (1024.0 * 1024.0));
        }
    }

    image_detail const* vma_allocator::get_image_detail(uint64_t const handle) {
        std::lock_guard guard(this->access_mutex);
        if (this->images.contains(handle)) {
            return &this->images[handle];
        }
        return nullptr;
    }

    void vma_allocator::free_buffer(uint64_t const handle) {
        std::lock_guard guard(this->access_mutex);
        auto const it = this->buffers.find(handle);
        if (it == this->buffers.end()) {
            return;
        }
        // shared resources are freed by reference count: only the last free really destroys
        if (it->second.use_count.fetch_sub(1) > 1) {
            return;
        }
        vmaDestroyBuffer(this->allocator, it->second.buffer, it->second.allocation);
        this->buffers.erase(it);
    }

    void vma_allocator::free_image(uint64_t const handle) {
        std::lock_guard guard(this->access_mutex);
        auto const it = this->images.find(handle);
        if (it == this->images.end()) {
            return;
        }
        // shared images are freed by reference count: only the last free really destroys
        if (it->second.use_count.fetch_sub(1) > 1) {
            return;
        }
        vmaDestroyImage(this->allocator, it->second.image, it->second.allocation);
        this->images.erase(it);
    }

    void vma_allocator::retain_buffer(uint64_t const handle) {
        std::lock_guard guard(this->access_mutex);
        auto const it = this->buffers.find(handle);
        if (it != this->buffers.end()) {
            it->second.use_count.fetch_add(1); // one more shared owner
        }
    }

    void vma_allocator::retain_image(uint64_t const handle) {
        std::lock_guard guard(this->access_mutex);
        auto const it = this->images.find(handle);
        if (it != this->images.end()) {
            it->second.use_count.fetch_add(1); // one more shared owner
        }
    }

    std::pair<VkCommandPool, VkCommandBuffer> vma_allocator::create_command_pair() const {
        VkCommandPoolCreateInfo command_pool_create_info = make_command_pool_info(this->upload_queue_family_index);

        VkCommandPool command_pool;
        if (vkCreateCommandPool(this->logical_device, &command_pool_create_info, nullptr, &command_pool) != VK_SUCCESS) {
            deren::utility::panic("Failed to create command pool");
        }

        VkCommandBuffer command_buffer;
        VkCommandBufferAllocateInfo buffer_allocate_info = make_command_buffer_allocate_info(command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        if (vkAllocateCommandBuffers(this->logical_device, &buffer_allocate_info, &command_buffer) != VK_SUCCESS) {
            deren::utility::panic("Failed to create command buffer");
        }
        return {command_pool, command_buffer};
    }
} // namespace deren::vulkan