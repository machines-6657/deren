// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/core/filter/filters.cppm
 * @brief The filtered views over a `deren::vulkan::core`: one for the application (`user_filter`) and one for a pass's
 *        initialization (`pass_filter`).
 * @defgroup vulkan_core_filters Vulkan Core Filters
 *
 * WHY FILTERS AT ALL, and why two: `deren.vulkan.core` is the device root - it holds the instance, the device, the
 * swapchain, the allocator, every image and every descriptor pool. Handing that whole interface to a consumer
 * hands it the ability to do anything to the device, and the consumer then has no way to say what it actually
 * needs. A filter is a NAMED, narrow view of that root: the app gets one, a pass's init gets another, and each
 * can grow the operations its consumer is allowed to have. Both hold a `std::shared_ptr<core>`, so a filter can
 * outlive the object it was made from - which is what makes it usable as a member of something that shares a
 * device with a second owner.
 *
 * THE TWO FILTERS, and the line between them:
 *
 *  * `user_filter` - what the RUNTIME exposes to the application through its `operator->`. It forwards the
 *    things external code may safely touch (the window, the swapchain's extent and format, the current frame,
 *    a command buffer, a shader module, a sampler) and NOTHING that manages the frame (acquire/submit/present
 *    stay the runtime's). It was called `core_filter` and took a `core&`; the rename is what makes room for a
 *    family of filters rather than one.
 *
 *  * `pass_filter` - what a PASS's create step is given. It answers exactly the questions a pass cannot answer
 *    from its own declaration, and it is deliberately SMALLER than the runtime's own view: it hands out
 *    session-stable handles of resources the owner has registered (see `register_resource`) and the allocator a
 *    pass that must create its own buffers or images needs (the descriptor set it used to allocate from the
 *    core's pool is gone with the heap). It does NOT hand out per-generation views: those change with every
 *    swapchain, and a pass receives them per frame through `resolved_io` - the framework's own per-image channel
 *    (see `resolved_io::own_per_image`).
 *
 * WHAT NEITHER FILTER FORWARDS: frame management (acquire/present/submit) and the core's own initialization
 * internals. Those are the runtime's, and a consumer that needs one of them needs the runtime, not a filter.
 */

module;

#include <GLFW/glfw3.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

export module deren.vulkan.core.filters;
export import deren.vstd;
export import deren.vulkan.core;
export import deren.vulkan.render_resource; // resource_id: what a pass asks for, in the declaration's own vocabulary

export namespace deren::vulkan {

    /**
     * @brief one resource's device handles, as a pass sees them
     *
     * The same three handles `deren::vulkan::pass::resolved_binding` carries at record time - a descriptor takes a
     * VIEW, a barrier takes an IMAGE, and a buffer binding takes a BUFFER - but declared here, on the core side,
     * because `render_resource` is deliberately pure CPU data (no Vulkan type in it) and the framework must not
     * depend on this module. The runtime copies the three fields across, which is the whole conversion.
     */
    struct resource_handles {
        VkImageView view = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
    };

    /**
     * @ingroup vulkan_core_filters
     * @brief filtered view over a core: what the APPLICATION may touch, exposed by `runtime::operator->`
     * @note
     *      - holds a `std::shared_ptr<core>`, so it keeps the device alive while it exists
     *      - the runtime exposes it via `operator->`, so external code never sees the raw core
     *      - frame management (acquire/submit/present) and the core's own initialization are deliberately not
     *        forwarded: they belong to the runtime
     */
    class user_filter {
        // called owner_share, not owner: the owner parameter of the constructor would hide a member of that name and
        // MSVC /W4 reports C4458, an error under /WX
        /// the share that keeps the device alive, and the raw pointer every method forwards through
        std::shared_ptr<core> owner_share;
        core* vk_core = nullptr;

    public:
        explicit user_filter(std::shared_ptr<core> owner) noexcept;

        // ---- read-only access to objects external code may safely touch ----
        [[nodiscard]] VkDevice get_device() const noexcept;
        [[nodiscard]] GLFWwindow* get_window() const noexcept;
        [[nodiscard]] VkExtent2D get_swap_chain_extent() const noexcept;
        [[nodiscard]] VkFormat get_swap_chain_image_format() const noexcept;
        [[nodiscard]] uint32_t get_current_frame() const noexcept;
        static constexpr int32_t max_frames_in_flight = core::MAX_FRAMES_IN_FLIGHT;

        // ---- facade operations (forwarded from core so callers need no raw API) ----
        void wait_idle() const noexcept;
        void set_window_title(std::string_view title) const noexcept;

        // ---- safe factory operations ----
        [[nodiscard]] vk_command_buffer make_command_buffer() const;
        [[nodiscard]] std::optional<vk_shader_module> make_shader_module(std::span<uint8_t> shader) const noexcept;
        [[nodiscard]] vk_image_view make_image_view(VkImage image, VkFormat format, VkImageViewType type) const;
        [[nodiscard]] vk_sampler make_sampler(VkSamplerAddressMode address_mode, float max_lod) const;

        /**
         * @ingroup vulkan_core_filters
         * @brief access the VMA allocator for GPU buffer / image allocation
         * @return reference to the core's vma_allocator (thread-safe for creation and lookup)
         * @note non-const accessor: allocating GPU memory mutates the allocator, so this is only available on a
         *       non-const filter - a const runtime cannot allocate
         */
        [[nodiscard]] vma_allocator& get_vma() noexcept;

        // ---- swapchain handling ----
        /// @return true when a new generation was actually built; false when the recreate was deferred
        ///         (0x0 window) and nothing died - see core::recreate_swap_chain's return value
        [[nodiscard]] bool recreate_swap_chain() const;
    };

    /**
     * @ingroup vulkan_core_filters
     * @brief filtered view over a core, for a PASS's create step: the resources it declared, and nothing else
     *
     * WHAT IT IS FOR, in one sentence: a pass must be able to build what it owns (its pipeline) and to NAME the
     * resources its own declaration lists, without being handed the device root - which is what the runtime did
     * on its behalf before this filter existed (see
     * `runtime::create_mask_bake` and `runtime::create_compute_skin`, whose bespoke input structs this replaces).
     *
     * THE LIFETIME CONTRACT, and it is the reason this class is small: what `resource()` answers at CREATE time
     * is a SESSION-STABLE handle. The runtime's own material table and its bindless texture array are created
     * once and only ever have their CONTENTS rewritten; its per-frame-slot buffers (the skin matrices, the
     * cluster bins) are created once and rewritten per slot. A per-swapchain-image VIEW is NOT stable that way -
     * it is rebuilt with every generation - so those are deliberately not served here: a pass receives them per
     * frame through `resolved_io` (the framework's `own` / `own_per_image` / `barrier_images` channels). Handing
     * one out at create time would be the per-image-lifetime trap this branch has already paid for twice.
     */
    class pass_filter {
        // called owner_share, not owner: the owner parameter of the constructor would hide a member of that name and
        // MSVC /W4 reports C4458, an error under /WX
        /// the share that keeps the device alive, and the raw pointer every method forwards through
        std::shared_ptr<core> owner_share;
        core* vk_core = nullptr;
        /**
         * What the OWNER (the runtime) has published for its passes to name.
         *
         * A vector rather than a map: the resources a pass may ask for are a handful, this is filled once at
         * startup, and the lookup happens during create - so the smallest container that works is the honest
         * one. A `resource_id` that was never registered resolves to a null handle, which is what a pass's
         * "this owner has none" branch already handles.
         */
        std::vector<std::pair<uint32_t, resource_handles>> registered;

    public:
        explicit pass_filter(std::shared_ptr<core> owner) noexcept;

        /// @brief the device a pass builds its own objects on
        [[nodiscard]] VkDevice device() const noexcept;
        /// @brief the surface's format: what a pipeline that renders into the swapchain must be created with
        [[nodiscard]] VkFormat swap_chain_image_format() const noexcept;
        /// @brief the surface's current extent (a pass that bakes it into an object recreates it in
        ///        `on_swapchain_recreated`, which is what that hook is for)
        [[nodiscard]] VkExtent2D swap_chain_extent() const noexcept;

        /**
         * @brief the allocator, for a pass that must create its own buffer or image
         *
         * The door rather than a wrapper per resource kind: a pass that creates GPU memory says so by asking
         * for the allocator, and the handles it gets back are RAII (`vk_buffer` / `vk_image` from
         * `deren.vulkan.core:vma_handles`), so a pass's own resources are released by its own destructor in the order
         * it wrote them. What the allocator does NOT do for a pass is decide the lifetime RULES: a per-generation
         * resource still has to be rebuilt in `on_swapchain_recreated`, and a descriptor family built over one
         * still has to retire its pool rather than destroy it (see `deren.vulkan.bindings`).
         */
        [[nodiscard]] vma_allocator& vma() noexcept;

        /// @brief publish one of the owner's resources, by the DECLARATION's identity (resource + element)
        void register_resource(render_resource::resource_id id, uint32_t element, resource_handles handles) noexcept;
        /**
         * @brief what this pass declared, if the owner has it: session-stable handles, or all-null
         * @param id the resource, from the same `resource_id` vocabulary the declaration uses
         * @param element which one of the family: the frame SLOT for a per-frame-slot resource (the skin
         *        matrices, the cluster bins), or 0 for a device-wide one
         */
        [[nodiscard]] resource_handles resource(render_resource::resource_id id, uint32_t element) const noexcept;
    };

} // namespace deren::vulkan
