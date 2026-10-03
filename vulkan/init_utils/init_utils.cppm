// ============================================================================
// module: deren.vulkan.init_utils
// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))
//
// The runtime's initialization utilities: the resource-creation patterns its init
// functions repeat. Sister module: deren.vulkan.core:init_utils holds the DEVICE-side
// equivalents (device/queue/swapchain selection, memory-type and format queries);
// this one holds the RENDERER-side ones - the host-visible buffers, the recording
// pools and the uploaded textures the runtime's members are made of - and imports
// deren.vulkan.core, never the other way round.
//
// Why this is a module rather than a set of runtime:: members: a member function of
// a class attached to a named module must be DEFINED in that module, so
// runtime::init_scene_resources() and its siblings cannot move anywhere. What CAN
// move is the part of them that needs no `this`: a creation pattern that is the
// same for the camera UBO, the material table, the light UBO and the cluster
// buffers, and differs only in a capacity, a buffer type and what to call the
// resource in the failure log. Those three differences are parameters now.
//
// The split that stays: the runtime keeps the POLICY (which resources exist, how
// many frame slots, which of them are host-visible) and this module keeps the
// CREATION (allocate, map, label the failure).
//
// Depends on deren.vulkan.core (the pool factories and the vma handles) and utility
// (panic).
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

export module deren.vulkan.init_utils;

export import deren.vstd;
export import deren.vulkan.core; // the vma handles + the vk_* wrappers this interface names

/**
 * @file vulkan/init_utils/init_utils.cppm
 * @defgroup vulkan_init_utils_runtime Runtime Init Utils
 * @brief the resource-creation patterns deren.vulkan.runtime's initialization repeats
 *
 * Every function here was lifted out of a `runtime::init_*` / `runtime::ensure_*` member, and each one
 * exists because the SAME sequence appeared at more than one site with a different string in it: create
 * through VMA, panic when the handle is empty, ask VMA for the detail, panic when there is none, and keep
 * the mapped pointer the host writes through. The buffer sites alone were the camera UBO x3 slots, the
 * material table, the instance transforms, the motion matrices x3, the skin matrices x3, the morph data
 * x3, the light UBO x3 and the cluster counts x3 - each with a hand-written panic message naming the
 * resource, and each re-deriving the same failure handling.
 *
 * @note the span parameters are `std::byte const` and the module calls vma's RAW pointer overloads
 *       deliberately: vma's `create_buffer(std::span<T>)` / `create_image(std::span<T>)` templates
 *       `reinterpret_cast` the data to `uint8_t*`, so they accept only a NON-const span - which no
 *       call site here has, because their sources are const arrays and const zero-filled vectors. (The
 *       two `create_image` span templates are additionally unusable as written: they forward
 *       `(data, create_info, size, type)` to an overload declared `(data, size, create_info, type)`, so
 *       instantiating either one is a compile error. Recorded rather than fixed here, because that fix
 *       belongs to deren.vulkan.core:vma and not to this module.)
 */
namespace deren::vulkan::init_utils {
    /**
     * @ingroup vulkan_init_utils_runtime
     * @brief the shared task pool's width: hardware_concurrency() / 4 (floor 1, fall back to 2 when the
     *        runtime cannot report the core count)
     * @return the number of worker threads the runtime's pool is built with
     * @note a QUARTER of the hardware threads, not a half: measured on a 16-thread machine, moving this
     *       to hw/2 cost 11-12% fps (822 -> 735 forward, 1706 -> 1497 unlit) and lengthened the shadow
     *       sub-phase (0.61 -> 0.65 ms) - the recording stages are not worker-starved at hw/4, and more
     *       workers only add wake/join, cache and driver-side recording contention. Kept as a documented
     *       negative result so the experiment is not repeated
     */
    export [[nodiscard]] int32_t default_task_pool_threads() noexcept;

    /**
     * @ingroup vulkan_init_utils_runtime
     * @brief create ONE host-visible buffer through VMA and keep its mapped pointer
     * @param device the core whose vma allocator creates it
     * @param initial the bytes to upload; empty means "allocate only" for the allocate-only buffer types
     * @param type the buffer's usage/memory type (see buffer_type: host-visible ones are the *_coherent
     *        variants, which is what makes the mapped pointer usable)
     * @param what what the resource is called in the failure log ("camera ubo buffer")
     * @param buffer receives the owning handle; empty on entry, valid on return
     * @param mapped receives VMA's mapped pointer for that allocation
     * @note panics (with @p what in the message) when the buffer cannot be created or its detail cannot
     *       be looked up - the failure mode the call sites all handled by hand before
     */
    export void create_host_buffer(core& device,
                                   std::span<std::byte const> initial,
                                   buffer_type type,
                                   std::string_view what,
                                   vk_buffer& buffer,
                                   void*& mapped,
                                   /// extra usage bits to OR in. A buffer whose DESCRIPTOR goes on the descriptor heap
                                   /// needs VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, because a heap descriptor for
                                   /// a buffer is an address range (VUID-VkBufferDeviceAddressInfo-buffer-02601).
                                   VkBufferUsageFlags extra_usage = 0);

    /**
     * @ingroup vulkan_init_utils_runtime
     * @brief create @p slots host-visible buffers of the same shape (one per frame slot) and collect
     *        their mapped pointers beside them
     * @param device the core whose vma allocator creates them
     * @param slots how many to create (the runtime passes core::MAX_FRAMES_IN_FLIGHT)
     * @param initial the bytes each one starts with
     * @param type the buffer's usage/memory type
     * @param what what the resource is called in the failure log
     * @param buffers receives the owning handles, appended in slot order
     * @param mapped receives the mapped pointers in the same order; pass nullptr for a buffer the host
     *        never writes through (the cluster index rows, which only the GPU fills)
     * @note the per-slot shape exists for a reason (a frame in flight must not share the buffer the next
     *       frame rewrites), which is why the helper takes the slot count rather than being called in a
     *       loop by every caller
     */
    export void create_host_buffers(core& device,
                                    uint32_t slots,
                                    std::span<std::byte const> initial,
                                    buffer_type type,
                                    std::string_view what,
                                    std::vector<vk_buffer>& buffers,
                                    std::vector<void*>* mapped = nullptr,
                                    /// see create_host_buffer above: the bits a heap-backed descriptor needs
                                    VkBufferUsageFlags extra_usage = 0);

    /**
     * @ingroup vulkan_init_utils_runtime
     * @brief a command pool that owns one SECONDARY command buffer, for recording on a worker thread
     * @param device the core to create both from
     * @return the {pool, buffer} pair the runtime's per-cascade and per-worker recording vectors store
     * @note one pool PER consumer, never a shared one: a VkCommandPool is not thread safe and these
     *       buffers are filled concurrently (see the recording stages' notes in deren.vulkan.runtime)
     */
    export [[nodiscard]] std::pair<VkCommandPool, vk_command_buffer> create_recording_pool(core& device);

    /**
     * @ingroup vulkan_init_utils_runtime
     * @brief an uploaded 2D texture together with the view that samples it
     */
    export struct texture_2d {
        /// the owning image (the RAII wrapper keeps the allocation alive)
        vk_image image = {};
        /// a 2D color view of it, which is what a descriptor binds
        vk_image_view view = {};
    };

    /**
     * @ingroup vulkan_init_utils_runtime
     * @brief upload a 2D texture and create its 2D view
     * @param device the core whose vma allocator and view factory are used
     * @param pixels the texel data; its size must match what @p info describes
     * @param info width/height/mip levels/layer count/format (the caller's shape)
     * @param what what the texture is called in the failure log ("white fallback texture")
     * @return the image and its view, both in SHADER_READ_ONLY layout terms - the caller owns the
     *         layout transition and the descriptor write, this only creates
     * @note panics (with @p what in the message) when the image cannot be created or its detail cannot
     *       be looked up
     */
    export [[nodiscard]] texture_2d create_texture_2d(core& device,
                                                      std::span<std::byte const> pixels,
                                                      image_create_info const& info,
                                                      std::string_view what);
} // namespace deren::vulkan::init_utils
