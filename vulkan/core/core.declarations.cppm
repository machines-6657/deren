// ============================================================================
// module: deren.vulkan.core:declarations  - the INTERFACE PARTITION of deren.vulkan.core
//
// The declarations live here so the implementation halves can be partitions: a partition does not
// see the primary interface on its own, so every implementation partition imports THIS one.
// The primary (vulkan/core/core.cppm) is the three lines that re-export it, and `import deren.vulkan.core;`
// is unaffected anywhere - which is the whole point of paying for the split.
//
// MEASURED BEFORE COMMITTING TO IT: under this project's -Werror, clang 22 REJECTS the textbook
// pattern (the primary importing its implementation partitions) with
// -Wimport-implementation-partition-unit-in-interface-unit. The viable variant, verified on a scratch
// project with these exact flags, is this one: the primary imports ONLY interface partitions, and the
// implementation partitions are listed in CMake (which compiles every unit) without being imported by
// the interface. Their definitions are still archived and still link.
// ============================================================================
// ============================================================================
// module: deren.vulkan.core
// module version: 0.24.0  (independent of the app version in CMakeLists project(VERSION))
//
// GPU scaffolding: instance / device / swapchain / VMA / pipeline / descriptor
// plumbing (core.vma / core.pipeline / core.filter / core.init_utils submodules
// are part of this unit). Standalone Vulkan wrapper; depends on VMA + utility,
// with the struct-fill conventions coming from vulkan.constant_init.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <GLFW/glfw3.h>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vulkan/vulkan.h>

export module deren.vulkan.core:declarations;
import deren.utility;
export import deren.vstd;
export import deren.vulkan.core.handles;
export import :vma;
export import :descriptor_heap;

// THE BACKEND IMPLEMENTS THE RHI CONTRACT IN PLACE: `core` IS the `api_core` a host gets back from
// `deren_make_api_core()`, not a wrapper object standing beside it. It owns the instance, the device,
// the swapchain and the frame machinery, so it is the thing the contract describes; a second type in
// front of it would be a second lifetime to keep in step, and `deren_make_api_core()` would have to
// invent an owner for it. With the base here, the S2 factory returns `this` and the deleter deletes it.
//
// THE DEPENDENCY DIRECTION IS THE POINT: backend -> contract, never contract -> backend. The contract
// module imports nothing (promise/rhi/*.cppm), so a backend that implements it cannot drag anything
// back into the engine's contract.
import deren.promise.rhi;
export import :vma_handles;

/**
 * @file core.cppm
 */

namespace deren::vulkan {
    /**
     * @defgroup vulkan_core Vulkan Core Objects Manager
     * @brief manages core vulkan objects and windows instance init and destroy.
     * @note
     *      - RAII
     *      - includes VMA decorator, which is defined in ./vma/vma.cppm
     *
     * @warning
     *      - do not call the init or create function, just use the members or other functions
     *      - no thread-safe
     */

    /**
     * @ingroup vulkan_core
     * @brief agreed flat layout of the scene block, shared by every pipeline (see shaders/pbr.slang):
     *        set 0 binding 0 = CameraUBO (uniform buffer; one per frame slot, each slot's block
     *              points at its own - static, no per-frame descriptor writes),
     *              binding 1 = sampler2D textures[] (runtime array, partially bound + non-uniform index),
     *              binding 2/3/4 = prefiltered env / irradiance / BRDF LUT (combined image samplers),
     *              binding 5 = Material materials[] (storage buffer: per-material texture indices + factors),
     *              binding 6 = mat4 instance transforms[] (storage buffer, per-instance world matrices),
     *              binding 7 = LightUBO (uniform buffer: directional light view-proj + direction),
     *              binding 8 = shadow map (sampler2DArrayShadow, one array layer per cascade; LINEAR
     *              min/mag with compareEnable = VK_TRUE, so the hardware does the 2x2 comparison and
     *              shading.glsl's calc_shadow_cascade() averages a 3x3 grid of those taps),
     *              binding 9 = mat4 skin matrices[] (storage buffer: identity block + per-skin joints),
     *              binding 10 = float morph data[] (storage buffer: per-primitive morph deltas + weights),
     *              binding 11/12 = uint cluster light counts[] / uint cluster light indices[] (the
     *              clustered-culling result: written by the cluster compute pass, read by the
     *              fragment stage),
     *              binding 13 = mat4 previous world matrices[] (one per motion slot; the vertex stage
     *              reads its own entry so the fragment stage can build TAA's motion vector for a
     *              MOVING object, not only for camera motion)
     * @note hardcoded instead of parsed from SPIR-V: the indexed layout is flat, so pipelines skip
     *       descriptor / push constant parsing and every stage shares this one binding contract, which the
     *       frame's heap carries
     */
    // WHY `inline` AND NOT A PLAIN `constexpr` (this is the dynamic backend's one prerequisite in this
    // file): a namespace-scope constexpr declared in a module purview has MODULE linkage, so a consumer
    // in ANOTHER IMAGE does not fold it - it references the variable and needs the owning image to
    // export DATA for it. Measured on the DLL probe tree: the executable imported exactly one data
    // symbol, `_ZN5deren6vulkanW5derenW6vulkanW4core22scene_texture_capacityE`, and the runtime entry
    // veneer the boundary is built on can carry functions (a naked jump stub is a function) but NOT data.
    // `inline` gives the constant external linkage with a definition every importer may use, so each side
    // keeps its own copy and nothing crosses the boundary as data. The values are unchanged.
    export inline constexpr uint32_t scene_texture_capacity = 128;
    // material_push_constants: 6 uints + aligned mat4 = 96 bytes, see vulkan/scene_tree/scene_tree.cppm
    export inline constexpr uint32_t scene_push_constant_size = 96;
    /**
     * @ingroup vulkan_core
     * @brief offset of the SECOND push constant range of the shared scene layout, right after the
     *        per-primitive material block
     * @note currently one uint: the cascade index the shadow pass is rendering. It is a separate
     *       range rather than extra fields in the material block because that block is exactly 96
     *       bytes and the two together would exceed the 128 bytes the spec guarantees every
     *       implementation provides (the engine does not rely on a vendor's larger limit).
     */
    export inline constexpr uint32_t scene_cascade_push_offset = scene_push_constant_size;
    export inline constexpr uint32_t scene_cascade_push_size = sizeof(uint32_t);

    /**
     * @brief format of the HDR scene target the deferred lighting stage renders into and the post-process
     *        pass samples: the scene color target uses it, and each swapchain image owns one
     *        single-sample resolve target in it (see core::create_render_targets)
     */
    export inline constexpr VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;

    /**
     * @ingroup vulkan_core
     * @brief how many color targets the G-buffer pass writes (see gbuffer_formats)
     */
    export inline constexpr uint32_t gbuffer_target_count = 3;

    /**
     * @ingroup vulkan_core
     * @brief formats of the G-buffer targets, in attachment order (= the fragment output locations
     *        of shaders/gbuffer.slang), and the reason the deferred path is cheap to store:
     *        - 0 RGBA8_UNORM: albedo.rgb (base color, linear) + metallic in a
     *        - 1 RGBA16F: world normal.xyz (no encoding - the conservative layout trades 4 bytes per
     *          pixel for not having to reason about octahedral precision) + roughness in a
     *        - 2 RGBA8_UNORM: material_id low/high byte + ambient occlusion + material flags
     *        16 bytes per pixel in total; the depth is the pass's own single-sampled depth image.
     * @note every target is single-sampled (1x) on purpose: a G-buffer cannot be multisampled
     *       without per-sample shading, which is the trade that makes TAA the anti-aliasing
     *       (the anti-aliasing story is TAA/FXAA on the lit image instead).
     */
    export inline constexpr std::array<VkFormat, gbuffer_target_count> gbuffer_formats = {
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM,
    };

    /**
     * @ingroup vulkan_core
     * @brief motion-vector format: the fourth G-buffer target, written by the G-buffer pass and read
     *        by TAA (RG16F because a motion vector is a signed sub-pixel quantity in UV space and
     *        8-bit would quantize it to ~1/255 of the screen - coarser than the jitter TAA exists to
     *        resolve)
     */
    export inline constexpr VkFormat gbuffer_velocity_format = VK_FORMAT_R16G16_SFLOAT;

    /**
     * @ingroup vulkan_core
     * @brief color attachments the G-buffer pass declares: the three surface targets above, the
     *        motion-vector target, and the scene-color target it ADDS the emissive term into
     * @note emissive is lighting-independent, so it does not belong to the deferred lighting stage -
     *       and it needs the material's emissive texture and the fragment's UVs, neither of which the
     *       G-buffer stores. Adding it in the base pass is what commercial deferred renderers do (the
     *       G-buffer pass writes the surface and adds emissive to the scene color), and it is why that
     *       last attachment is blended ONE/ONE while the surface and velocity targets are overwritten.
     *       It is CLEARed to zero by the instance, and the deferred lighting stage adds the lighting
     *       (and the sky, where no geometry wrote depth) on top.
     */
    export inline constexpr uint32_t gbuffer_pass_attachment_count = gbuffer_target_count + 2; // + velocity + scene color

    /**
     * @ingroup vulkan_core
     * @brief how many GPU timing marks one frame may write (the query pool is sized
     *        MAX_FRAMES_IN_FLIGHT * this, and each frame slot owns its own contiguous range)
     * @note a mark is one vkCmdWriteTimestamp; the frame's pass boundaries use a handful of them,
     *       and the remaining capacity is headroom for the passes later milestones add. A frame
     *       that records more marks than this silently stops marking (the extra passes are simply
     *       not measured) instead of overflowing into the next slot's range.
     */
    export inline constexpr uint32_t gpu_timing_mark_capacity = 16;

    /**
     * @ingroup vulkan_core
     * @brief GPU durations of one completed frame, in mark order (see core::mark_gpu_timing)
     * @note entry i is the time between mark i and mark i + 1, so a frame that wrote
     *       @p mark_count marks yields mark_count - 1 durations
     */
    export struct gpu_timing_result {
        std::array<double, gpu_timing_mark_capacity> milliseconds = {}; // elapsed per consecutive mark pair
        uint32_t mark_count = 0;                                        // marks the frame wrote (0 = no measurement)
    };

    /**
     * @ingroup vulkan_core
     * @brief whether a field @p size bytes long at @p offset of a contract structure is inside the
     *        @p struct_size bytes the caller says it compiled
     *
     * THE APPEND-ONLY ABI GUARD, written once because both the context's descriptor and the buffer's
     * are read through it (core.constructor.cppm and core.api_core.cppm). A contract structure grows by
     * APPENDING fields and every field carries a default, so a caller that compiled an older, shorter
     * shape is served the defaults it never declared instead of having this build read past its end.
     */
    [[nodiscard]] constexpr bool covered_by(uint32_t const struct_size, size_t const offset, size_t const size) noexcept {
        return static_cast<size_t>(struct_size) >= offset + size;
    }

    /**
     * @brief the device, its allocator and every resource created on it - the renderer's lifetime ROOT
     *
     * `std::enable_shared_from_this` is here because the core is the one object that can be SHARED: a caller may
     * hold it (see `runtime`'s constructor that takes a `shared_ptr<core>`), and the resources it owns are
     * created through it, so a creation site can hand out a reference-counted handle to the device it is
     * building on. It does NOT mean a `core` must be heap-allocated: nothing calls `shared_from_this()` yet.
     */
    export struct core : deren::promise::rhi::api_core, deren::utility::enable_stack_destruct, std::enable_shared_from_this<core> {
        // creation options this core was built with (window size, vsync); the window and the
        // swap chain honor them
        deren::promise::rhi::create_info create_options = {};

        // ---- S2 BATCH 2: THE RECORDING SURFACE, THE FRAME'S VIEWS AND THE ESCAPE --------------------
        //
        // THE CONTRACT SPEAKS IN BORROWED VIEWS AND THESE ARE THE FOUR THIS BACKEND CAN ANSWER. The
        // factories still answer nullptr (the resource model is S3), so the only `rhi` handles that can
        // exist today are borrowings of objects this class ALREADY OWNS: the frame's primary command
        // buffer, the swapchain image the frame draws into, the host-visible read-back slot, and the
        // Vulkan escape. Each is a member of this class, so "the backend owns it, the caller borrows
        // it" is the object graph rather than a rule in a comment.
        //
        // `owner` is raw and non-owning: the views live INSIDE the core they point at, so their
        // lifetime is this object's, and a caller that keeps one past the core's death holds a
        // dangling pointer - the contract says the same thing about every borrowed view.
        //
        // `use()` turns the contract's (from, to) ROLE PAIR into the barrier this renderer's own recipe
        // describes, and `copy_image_to_buffer()` records the copy the screenshot path used to spell
        // out by hand. The compile-time shadow gate in core.api_core.cpp proves, field by field, that
        // the first lands exactly on constant_init's two recipes (plan §8.3, gate A5).

        /// The frame's PRIMARY command buffer, as the contract's recording surface (`begin_commands()`).
        struct frame_commands final : deren::promise::rhi::command_list {
            core* owner = nullptr;
            /// `use()` ANSWERS with an `error` now (it does not drop a barrier silently); this flag is only
            /// about how often the backend spells out the REASON for a refusal, so a per-frame caller
            /// cannot turn one broken pair into a log flood
            bool unexpected_use_logged = false;
            /// the A5 dump (both sides of each barrier) is emitted once per pair per process
            std::uint32_t shadow_gate_dumped = 0;

            [[nodiscard]] deren::promise::rhi::error use(deren::promise::rhi::image const& resource, deren::promise::rhi::image_use from, deren::promise::rhi::image_use to) noexcept override;
            [[nodiscard]] deren::promise::rhi::error copy_image_to_buffer(deren::promise::rhi::buffer& destination,
                                                                          deren::promise::rhi::image const& source,
                                                                          deren::promise::rhi::image_copy_region const& region) noexcept override;
        };

        /// The swapchain image the frame in flight draws into, as the contract's `image`.
        ///
        /// A BORROWED VIEW, NOT AN OWNED HANDLE: this object is a member of the core, the core owns the
        /// swapchain image behind it, and the contract's `release()` must never drop a reference for it
        /// (the caller never held one). That is why the override below answers with a ONE-TIME NAMED LOG
        /// instead of a silent no-op (rhi.api_core.cppm's ownership note): a caller that wrapped a
        /// borrowed view in `object_manager` has a bug, and the log is the only place that bug can be
        /// said out loud.
        struct frame_image_slot final : deren::promise::rhi::image {
            core* owner = nullptr;
            /// one log per process, not one per call: a loop that mis-uses the view must not flood the log
            /// (the same shape `frame_commands::use` uses for a foreign image)
            mutable bool borrowed_release_logged = false;

            [[nodiscard]] deren::promise::rhi::image_extent extent() const noexcept override;
            [[nodiscard]] deren::promise::rhi::image_format format() const noexcept override;
            /// BORROWED: logs once and drops no reference (see the note above)
            void release() noexcept override;
            /// the raw handle the backend's own recording needs (never carried across the boundary)
            [[nodiscard]] VkImage handle() const noexcept;
        };

        /// The backend's host-visible read-back slot, as the contract's `buffer`.
        ///
        /// A borrowed view for exactly the reasons `frame_image_slot` is; see its note.
        struct frame_readback_slot final : deren::promise::rhi::buffer {
            core* owner = nullptr;
            mutable bool borrowed_release_logged = false;

            [[nodiscard]] std::uint64_t size() const noexcept override;
            [[nodiscard]] std::span<std::byte> mapped() noexcept override;
            /// BORROWED: logs once and drops no reference
            void release() noexcept override;
            /// the raw handle `copy_image_to_buffer()` records into
            [[nodiscard]] VkBuffer handle() const noexcept;
        };

        /// AN OWNED BUFFER: what `create_buffer()` hands the caller.
        ///
        /// THE OPPOSITE OF THE TWO VIEWS ABOVE, and the difference is the whole ownership model:
        /// this object is HEAP-ALLOCATED BY THE FACTORY, so the caller's one `release()` can give the
        /// reference back to the allocator that made it - `release()` is `delete this`, and the
        /// destructor resets the `vk_buffer` RAII owner, which IS the allocator's reference-count
        /// decrement (rhi.api_core.cppm's ownership note: release, not necessarily destruction).
        ///
        /// It caches the size and the mapped pointer because the allocator's detail lookup is an
        /// UNLOCKED BORROW of its internal map (vulkan/core/vma/vma.cppm's `get_buffer_detail`): the
        /// pointer into that map must not be kept, the VALUES read out of it may be. That is the rule
        /// the ownership analysis (DYNAMIC_LINK_V2.md §11.2) says every contract query has to follow.
        struct owned_buffer final : deren::promise::rhi::buffer {
            deren::vulkan::vk_buffer owned = {};
            /// the Vulkan handle, cached at creation: `vk_buffer::handle()` is the ALLOCATOR's registry
            /// key (a uint64), not the `VkBuffer`, and reaching the real handle means a detail lookup -
            /// an unlocked borrow whose VALUES may be copied but whose pointer must not be kept.
            VkBuffer native = VK_NULL_HANDLE;
            std::uint64_t size_bytes = 0;
            void* mapped_bytes = nullptr;
            /// whether the descriptor asked for `buffer_flag::device_address`; the ability answers 0
            /// when it did not, because Vulkan only gives an address to a buffer created with the usage
            /// - asking for one this object never declared is the caller's bug, not a backend guess.
            bool addressable = false;
            /// the capability bits the descriptor declared, kept for the diagnostic a failed address
            /// query needs (nothing else reads it)
            deren::promise::rhi::buffer_flags declared_flags = deren::promise::rhi::no_buffer_flags;

            [[nodiscard]] std::uint64_t size() const noexcept override;
            [[nodiscard]] std::span<std::byte> mapped() noexcept override;
            /// give the reference back: `delete this`, whose destructor resets `owned`
            void release() noexcept override;
        };

        /// tier-2 `device_address`: a buffer's device address.
        ///
        /// ANNOUNCED ONLY NOW, AND THE REASON IS THE ABILITY'S OWN RULE rather than a change of heart:
        /// it used to declare an acceleration-structure address as well, and no backend could produce an
        /// acceleration structure, so the bit would have promised a service nothing could perform
        /// (rhi.extension.cppm's "every operation the ability declares must be performable"). With that
        /// operand moved to `ray_tracing` (abi 5) and buffers producible since the buffer batch, the
        /// bit is servable in the strict sense: `query_extension(device_address)` answers, and
        /// `buffer_address()` is real.
        struct buffer_address_view final : deren::promise::rhi::device_address {
            core* owner = nullptr;

            [[nodiscard]] deren::promise::rhi::extension_kind kind() const noexcept override;
            [[nodiscard]] std::uint64_t buffer_address(deren::promise::rhi::buffer const& resource, std::uint64_t offset) const noexcept override;
        };

        /// tier-2 `vulkan_escape`: the raw handles a pass needs where the contract has no concept.
        ///
        /// REQUIRED BY THE ENGINE TODAY, AND THAT IS TRANSITIONAL rather than a designed exception: the
        /// engine still records its own frame by hand (54 `vkCmdPipelineBarrier2` sites, 17 of them in
        /// runtime.frames.cppm), so a Vulkan backend that did not announce this bit would make the
        /// engine fail at startup by name. Once the passes record through the contract, the escape
        /// shrinks to the few calls the contract has no concept for.
        struct frame_escape final : deren::promise::rhi::vulkan_escape {
            core* owner = nullptr;

            [[nodiscard]] deren::promise::rhi::extension_kind kind() const noexcept override;
            [[nodiscard]] void* native_instance() const noexcept override;
            [[nodiscard]] void* native_physical_device() const noexcept override;
            [[nodiscard]] void* native_device() const noexcept override;
            [[nodiscard]] void* native_queue() const noexcept override;
            [[nodiscard]] void* native_command_buffer(deren::promise::rhi::command_list& commands) const noexcept override;
            [[nodiscard]] std::span<char const* const> enabled_instance_extensions() const noexcept override;
            [[nodiscard]] std::span<char const* const> enabled_device_extensions() const noexcept override;
            /// BORROWED: the `VkBuffer` behind a contract buffer this backend handed out (nullptr when it
            /// carries none). A released buffer must not be passed - see the contract's note.
            [[nodiscard]] void* native_buffer(deren::promise::rhi::buffer const& resource) const noexcept override;
        };

        // ---- WHAT THE RECORDING SURFACE OWNS --------------------------------------------------------
        //
        // THE COMMAND BUFFERS MOVED HERE FROM THE RUNTIME. They are RAII device objects and
        // `begin_commands()` has to hand out the frame's list, so the type that owns the device owns
        // them. WHAT DID NOT MOVE: the frame's SHAPE (one primary per frame slot, allocated once at
        // construction) and its LIFECYCLE - the engine still begins, ends and submits them, and still
        // decides the present recipe (runtime.frames.cppm). The runtime borrows the container as a span.
        //
        // RELEASED BY A CLEANUP, NOT BY THE MEMBER DESTRUCTORS: a `vk_command_buffer` frees itself
        // through the device and its pool, and a `vk_buffer` through VMA's allocator - both are gone by
        // the time member destructors run, because do_cleanup() destroys them from the destructor BODY.
        // A cleanup registered last runs first (do_cleanup is LIFO) and empties these two.
        std::vector<vk_command_buffer> frame_command_buffers;
        /// the recording view `begin_commands()` answers with (one object, reused every frame)
        frame_commands commands_view;
        /// the two frame-domain views `frame_image()` / `frame_readback_buffer()` answer with
        frame_image_slot frame_image_view;
        frame_readback_slot readback_slot_view;
        /// the tier-2 escape object `query_extension(vulkan_escape)` answers with
        frame_escape escape_view;
        /// the tier-2 address object `query_extension(device_address)` answers with
        buffer_address_view address_view;
        /// the read-back slot's allocation and its cached handle / mapping / capacity: host-visible,
        /// host-coherent and TRANSFER_DST, grown on demand (see frame_readback_buffer())
        vk_buffer readback_slot_buffer;
        VkBuffer readback_slot_handle = VK_NULL_HANDLE;
        void* readback_slot_mapped = nullptr;
        VkDeviceSize readback_slot_size = 0;
        /// the extension names this context ENABLED, in this core's own storage (the escape's guard
        /// rail: a pass checks the extension it wants is in here BEFORE it resolves an entry point).
        /// Filled once at construction; the names are literals, so the pointers stay valid.
        std::vector<char const*> instance_extension_names;
        std::vector<char const*> device_extension_names;
        /// true between the ACQUIRE of a frame and the SUBMIT that hands it to the queue: it is the window
        /// `begin_commands()`, `use()`, `copy_image_to_buffer()` and `native_command_buffer()` answer in
        /// (nullptr, or `error::not_ready`, outside it)
        bool frame_in_flight = false;
        /// whether an acquire has EVER succeeded. `frame_image()` answers nullptr before the first one:
        /// `acquired_image_index` defaults to 0, which is a REAL image (the wrong one), so the index alone
        /// cannot tell "no frame yet" from "frame 0"
        bool frame_acquired = false;

        /// the VkCommandBuffer of the frame slot in flight (VK_NULL_HANDLE when there is none)
        [[nodiscard]] VkCommandBuffer frame_command_buffer() const noexcept;

        VkInstance instance = VK_NULL_HANDLE;
        // called logical_device, not device: the structured binding of that name in core.constructor.cppm
        // would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        VkDevice logical_device = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        // properties of the picked physical device (VkPhysicalDeviceProperties: limits such as
        // timestampPeriod, bufferImageGranularity, maxPushConstantsSize + the device name).
        // Filled in init_device_and_queue() from the capabilities query it already runs - no
        // second vkGetPhysicalDeviceProperties round trip.
        VkPhysicalDeviceProperties device_properties = {};
        // Ray tracing is optional and comes from the device, not from a build option: when this is
        // false the extensions were not enabled (see device_capabilities) and every ray-traced path
        // skips itself. The properties carry the two limits the AS builder needs - the scratch
        // buffer's required address alignment and the per-level instance/geometry caps - and are a
        // plain data holder like device_properties above (assigned from the query, never passed to
        // Vulkan), so they are zero-initialized rather than carrying a fixed sType.
        bool ray_query_available = false;
        /// @brief whether the RT pipeline (traceRaysEXT + a shader binding table) can be used at all:
        ///        the extension set AND the feature, enabled together at device creation
        bool ray_tracing_pipeline_available = false;
        /// @brief whether VK_EXT_opacity_micromap is enabled: the micromap state that makes an
        ///        alphaMode MASK surface opaque/transparent per microtriangle. Needs the RT pipeline.
        bool opacity_micromap_available = false;
        VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties = {};
        /// @brief the micromap subdivision levels the device allows (queried with its feature)
        VkPhysicalDeviceOpacityMicromapPropertiesEXT opacity_micromap_properties = {};
        /// @brief the SBT numbers (handle size, region base alignment, handle alignment, recursion depth)
        ///        a ray-tracing pipeline's shader binding table has to be built against
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline_properties = {};
        /**
         * @brief VK_EXT_mesh_shader: whether the device runs a MESH pipeline at all, and the dispatch command
         *        that goes with it (docs/mesh_shaders.md)
         * @note the command is fetched through `vkGetDeviceProcAddr` because `vkCmdDrawMeshTasksEXT` is an
         *       EXTENSION command that the loader's import library does not export - calling it directly is a
         *       link error (`undefined symbol: vkCmdDrawMeshTasksEXT`), which is how this was learned. It is
         *       resolved once here rather than per use, and it is null on a device without the extension.
         * @note this is the FEATURE half only (extension + `meshShader`); whether a pass may use it also depends
         *       on the push budget the stage block needs, and that decision is the RUNTIME's (see
         *       runtime::mesh_shaders), because a pass is not the only thing that could want it.
         */
        bool mesh_shader_available = false;
        PFN_vkCmdDrawMeshTasksEXT mesh_dispatch = nullptr;
        /// ... and the INDIRECT form (`vkCmdDrawMeshTasksIndirectEXT`), resolved the same way and null on the same
        /// devices: it reads the three group counts out of a BUFFER instead of taking them as arguments, which is
        /// the seam a COMPUTE culling pass needs - the counts are then decided on the GPU, after culling, rather
        /// than by the host that recorded the draw (see runtime::draw_mesh_tasks_indirect)
        PFN_vkCmdDrawMeshTasksIndirectEXT mesh_dispatch_indirect = nullptr;
        /// VK_EXT_host_image_copy (REQUIRED - see host_image_copy_available below): the copy between an
        /// image and HOST memory that the IMPLEMENTATION performs - no command buffer, no staging buffer,
        /// no submission. Resolved through vkGetDeviceProcAddr like the mesh commands above, because the
        /// loader's import library does not export it; the startup check panics when it cannot be resolved.
        PFN_vkCopyImageToMemoryEXT copy_image_to_memory = nullptr;
        /// the other direction of the same extension: HOST memory -> image. The image upload path uses it
        /// (instead of staging buffer + vkCmdCopyBufferToImage); resolved here and checked at startup for
        /// the same reason as its twin above.
        PFN_vkCopyMemoryToImageEXT copy_memory_to_image = nullptr;
        /// whether the host image copy is CONFIRMED USABLE on this device: the extension, its
        /// hostImageCopy feature, BOTH entry points resolving, and GENERAL present in the device's
        /// copy-source AND copy-destination layout lists - every one of them checked at construction, and
        /// a missing one is a NAMED STARTUP FAILURE (core.constructor.cppm), not a downgrade. In a running
        /// process this member is therefore true: it exists so the paths that use the extension can state
        /// their precondition, not to choose between implementations.
        bool host_image_copy_available = false;
        // called graphics_queue_family_index, not graphics_family_index: the structured binding of that name
        // in core.constructor.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        uint32_t graphics_queue_family_index = 0;
        // called present_queue_family_index, not present_family_index: the structured binding of that name
        // in core.constructor.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        uint32_t present_queue_family_index = 0;
        VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;

        GLFWwindow* window = nullptr;

        // ---- facade operations (keep raw Vulkan / GLFW calls out of the caller) ----
        void wait_idle() const noexcept;                              // vkDeviceWaitIdle
        void set_window_title(std::string_view title) const noexcept; // glfwSetWindowTitle

        // ---- deren.promise.rhi::api_core: THE RHI CONTRACT, IMPLEMENTED IN PLACE -------------------
        //
        // Declared here, defined in vulkan/core/core.api_core.cpp. The rule for every one of them is
        // "answer for the device that was really created, never in the abstract": `abilities()` probes
        // the physical device below, the frame calls run the acquire/submit/present machinery on THIS
        // swapchain, and a factory that cannot honour a descriptor answers nullptr instead of a
        // half-built object (the plan's no-throwing-path rule, §4.2).
        //
        // `wait_idle()` below is the non-const OVERLOAD of the const facade call above - the contract's
        // virtual is not const, and both spellings answer `vkDeviceWaitIdle`.
        [[nodiscard]] deren::promise::rhi::ability_bits abilities() const noexcept override;
        [[nodiscard]] deren::promise::rhi::extension* query_extension(deren::promise::rhi::extension_kind kind) noexcept override;
        [[nodiscard]] deren::promise::rhi::swapchain* create_swapchain(deren::promise::rhi::swapchain_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::buffer* create_buffer(deren::promise::rhi::buffer_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::image* create_image(deren::promise::rhi::image_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::sampler* create_sampler(deren::promise::rhi::sampler_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::shader* create_shader(deren::promise::rhi::shader_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::pipeline* create_pipeline(deren::promise::rhi::pipeline_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::query* create_query(deren::promise::rhi::query_desc const& desc) override;
        [[nodiscard]] deren::promise::rhi::command_list* begin_commands() override;
        // ---- S2 batch 2: the recording surface's frame-domain views --------------------------------
        // `begin_commands()` now hands out a REAL list (the frame's primary command buffer, which this
        // class owns - see command_buffers below), so the tier-2 verbs that take a `command_list&`
        // (push_data / dispatch_mesh / build_acceleration_structure) become reachable, and the
        // read-back's copy can be recorded into the frame it belongs to.
        [[nodiscard]] deren::promise::rhi::image* frame_image() noexcept override;
        [[nodiscard]] deren::promise::rhi::buffer* frame_readback_buffer() noexcept override;
        [[nodiscard]] deren::promise::rhi::submit_info frame_begin() override;
        void present() override;
        void wait_idle() override;

        VkSurfaceKHR surface = VK_NULL_HANDLE;

        // called graphics_queue_handle, not graphics_queue: the structured binding of that name in
        // core.constructor.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        VkQueue graphics_queue_handle = VK_NULL_HANDLE;
        // called present_queue_handle, not present_queue: the structured binding of that name in
        // core.constructor.cppm would hide this member and MSVC /W4 reports C4458 (an error under /WX).
        VkQueue present_queue_handle = VK_NULL_HANDLE;
        uint32_t graphics_queue_family = VK_QUEUE_FAMILY_IGNORED;

        VkSwapchainKHR swap_chain = {};
        std::vector<VkImage> swap_chain_images = {};
        VkFormat swap_chain_image_format = {};
        VkExtent2D swap_chain_extent = {};
        /// the swapchain image the contract's frame_begin() acquired (vkAcquireNextImageKHR), i.e. what
        /// the contract's present() hands to the presentation engine. The runtime's own pacing path
        /// keeps its own `current_image_index` until it migrates onto the contract (S3).
        uint32_t acquired_image_index = 0;
        /**
         * @brief the extent the RENDER chain runs at: `swap_chain_extent` scaled by `render_scale`
         *
         * This is the ONE definition of "the frame's resolution": `runtime::pass_frame` hands it to every
         * pass (so a `full` or `half` `extent_rule` is relative to it), and every render target this core
         * creates is created with it. At `render_scale == 1.0` it returns `swap_chain_extent` exactly, so a
         * frame at the default scale is the frame this renderer always produced - which is what makes the
         * decoupling verifiable rather than merely plausible.
         *
         * ROUNDED to nearest rather than truncated: at half scale the floor would take a pixel off an odd
         * output width ON TOP of the halving, and both the images and the passes have to agree on the
         * number, so there is one formula and it is this one.
         */
        [[nodiscard]] VkExtent2D render_extent() const noexcept {
            auto const scaled = [this](uint32_t const axis) {
                uint32_t const value = static_cast<uint32_t>(static_cast<float>(axis) * this->render_scale + 0.5f);
                return value == 0u ? 1u : value; // a zero extent is not a small frame, it is an invalid one
            };
            return VkExtent2D{scaled(this->swap_chain_extent.width), scaled(this->swap_chain_extent.height)};
        }
        /// @brief the render scale `render_extent()` applies; clamped to (0, 1] at construction
        float render_scale = 1.0f;
        // Whether the swapchain images were created with VK_IMAGE_USAGE_TRANSFER_SRC_BIT (i.e. the
        // surface supports it). The screenshot read-back copies from a swapchain image and is only
        // legal when this is true - see init_swap_chain().
        bool swapchain_transfer_src_supported = false;
        // NOTE: there is deliberately NO `swapchain_host_transfer_supported` beside it. A swapchain
        // image's usage has to be a subset of the surface's supportedUsageFlags
        // (VUID-VkSwapchainCreateInfoKHR-imageUsage-01276), and the surfaces this renderer runs on do not
        // list VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT - so `vkCopyImageToMemoryEXT` can never read a
        // swapchain image here, and the read-back has exactly one mechanism (the copy command). A member
        // whose only purpose was to choose between two mechanisms is a liability, not a capability.

        // MSAA used to live here. It is gone with the forward path that was its only consumer: a
        // G-buffer cannot be multisampled without per-sample shading, so the scene has always
        // rendered at 1x, and with no second path there is nothing left for the setting to select.
        // The anti-aliasing story is TAA (and FXAA) on the shaded image instead.
        std::vector<VkImageView> swap_chain_image_views = {};

        VkFormat color_format = VK_FORMAT_UNDEFINED;
        // HDR scene targets (one per swapchain image, format hdr_format): the lighting stage (or the
        // TAA resolve, when TAA is on) writes them, and the post-process pass samples them
        std::vector<VkImage> hdr_images = {};
        std::vector<VkDeviceMemory> hdr_image_memories = {};
        std::vector<VkImageView> hdr_image_views = {};
        // create_render_targets() runs again on every swapchain recreation (it rebuilds the
        // HDR/LDR/bloom/G-buffer targets); its teardown must be pushed onto the cleanup stack only
        // once, or the stack grows one identical lambda per resize.
        bool resolve_cleanup_registered = false;
        // bloom targets: a 4-level chain (1/2, 1/4, 1/8, 1/16 of the swapchain extent, min 1x1),
        // one chain per swapchain image; the post pass prefilters into level 0, downsamples
        // through the levels and composites a weighted sum of all of them
        static constexpr uint32_t bloom_level_count = 4;
        std::array<std::vector<VkImage>, bloom_level_count> bloom_images = {};
        std::array<std::vector<VkDeviceMemory>, bloom_level_count> bloom_image_memories = {};
        std::array<std::vector<VkImageView>, bloom_level_count> bloom_image_views = {};
        // Display-referred (LDR) targets, one per swapchain image: with FXAA enabled the post
        // composite renders here instead of straight into the swapchain, FXAA reads it back and
        // writes the swapchain. hdr_format (R16F) even though the values are display range: FXAA
        // needs a *gamma-encoded* image to run its luma thresholds on, and a 16F target lets the
        // composite store that encoding itself (an sRGB attachment would decode it again on read,
        // and 8-bit would band).
        std::vector<VkImage> ldr_images = {};
        std::vector<VkDeviceMemory> ldr_image_memories = {};
        std::vector<VkImageView> ldr_image_views = {};

        // ---- G-buffer targets (see gbuffer_formats): one set per swapchain image, single-sampled,
        // written by the G-buffer pass and sampled by the deferred lighting / debug view. They are
        // created and destroyed with the HDR/LDR/bloom targets (create_render_targets +
        // recreate_swap_chain), so a resize rebuilds them in the same step.
        std::array<std::vector<VkImage>, gbuffer_target_count> gbuffer_images = {};
        std::array<std::vector<VkDeviceMemory>, gbuffer_target_count> gbuffer_image_memories = {};
        std::array<std::vector<VkImageView>, gbuffer_target_count> gbuffer_image_views = {};
        // The G-buffer pass has its own depth image rather than sharing the main one: a dynamic
        // rendering instance requires every attachment to have the same sample count, and keeping
        // them separate lets the G-buffer depth be SAMPLED later while the main one is never read.
        // Single-sampled, sampled (the lighting pass
        // reads it), cleared by the G-buffer pass like the main depth.
        std::vector<VkImage> gbuffer_depth_images = {};
        std::vector<VkDeviceMemory> gbuffer_depth_image_memories = {};
        std::vector<VkImageView> gbuffer_depth_image_views = {};
        // Motion vectors (gbuffer_velocity_format), one per swapchain image: written by the G-buffer
        // pass, read by the TAA resolve.
        std::vector<VkImage> velocity_images = {};
        std::vector<VkDeviceMemory> velocity_image_memories = {};
        std::vector<VkImageView> velocity_image_views = {};

        // ---- the stochastic punctual lighting chain's images ----
        // Its raw trace, its accumulation, its history and its spatial filter's output (four half-resolution
        // families) stood here. They went with the chain. The half-resolution pair below belongs to the
        // stochastic punctual lighting chain, which is a different feature that happens to share the size.
        // The stochastic PUNCTUAL LIGHTING chain's first image (see docs/megalights.md): the raw estimate
        // the trace writes, at half resolution - STORAGE for the compute pass that
        // writes it and SAMPLED for the lighting stage that adds it. One per swapchain image, because what
        // it holds depends on the frame's jittered camera.
        std::vector<VkImage> ml_images = {};
        std::vector<VkDeviceMemory> ml_image_memories = {};
        std::vector<VkImageView> ml_image_views = {};
        // ... and the temporal resolve's two, the same half resolution: the ACCUMULATION it writes (what the
        // lighting stage samples) and the history that becomes next frame's input - the latter written only by
        // a copy, so TRANSFER_DST plus SAMPLED and nothing else, exactly like the GI history beside it.
        std::vector<VkImage> ml_resolve_images = {};
        std::vector<VkDeviceMemory> ml_resolve_image_memories = {};
        std::vector<VkImageView> ml_resolve_image_views = {};
        std::vector<VkImage> ml_history_images = {};
        std::vector<VkDeviceMemory> ml_history_image_memories = {};
        std::vector<VkImageView> ml_history_image_views = {};
        // per-cell surface-offset image stood here, with the sampler that read them. The cache was the traced
        // chain's answer for the hits the screen cannot resolve; it is gone.
        // The furnace verification mode's constant environment: one texel per face, all six faces at the
        // mode's level. One element vectors rather than a scalar handle so the teardown paths that already
        // know how to destroy a target set can be reused unchanged. Its CONTENTS come from a clear, which
        // together with the binding that points the IBL at it is the next slice; until then nothing samples
        // it, which is what keeps this addition invisible.
        std::vector<VkImage> furnace_cube_images = {};
        std::vector<VkDeviceMemory> furnace_cube_memories = {};
        std::vector<VkImageView> furnace_cube_views = {};

        // ---- ray-traced sun visibility (see the shaders/rt_shadow.* pipeline stages) ----
        // FULL resolution, one per FRAME SLOT rather than per swapchain image: it is written and read
        // within one frame, and BOTH ends live in the frame's scene block, which is per slot. A
        // per-image image would have to be paired there with a per-slot top level structure, and
        // the same image can be recorded on either slot - so the two are different lifetimes and mixing
        // them would be wrong on exactly the frames where they disagree.
        std::vector<VkImage> rt_shadow_images = {};
        std::vector<VkDeviceMemory> rt_shadow_image_memories = {};
        std::vector<VkImageView> rt_shadow_image_views = {};

        // ---- temporal anti-aliasing (see runtime::set_taa) ----
        // The scene color TAA resolves FROM, one per swapchain image: when TAA is on, the geometry
        // and lighting stages write this image instead of the HDR target, and the TAA resolve blends
        // it with the history into the HDR target - which keeps the whole post chain (bloom,
        // composite, FXAA) reading exactly what it read before TAA existed.
        std::vector<VkImage> scene_color_images = {};
        std::vector<VkDeviceMemory> scene_color_image_memories = {};
        std::vector<VkImageView> scene_color_image_views = {};
        // The previous RESOLVED frame, one per swapchain image, read by the TAA resolve as history.
        // It is a separate image rather than a copy of the HDR target because a pass cannot sample the
        // image it renders into: the TAA resolve writes the HDR target (for the post chain) and the
        // runtime copies that into this image afterwards, which is one vkCmdCopyImage per frame - no
        // ping-pong, no per-frame descriptor rewrites.
        // TRANSFER_DST | SAMPLED: it is only ever written by the copy and read by the resolve.
        std::vector<VkImage> taa_history_images = {};
        std::vector<VkDeviceMemory> taa_history_image_memories = {};
        std::vector<VkImageView> taa_history_image_views = {};
        /**
         * @brief create a single-sampled device-local image with its memory, and return both
         * @param width / @param height the extent in texels
         * @param format the image format
         * @param tiling OPTIMAL or LINEAR (staging images that are mapped on the host)
         * @param usage the usage flags the image is created with
         * @param properties the memory type the image is bound to
         * @note every target the engine creates is single-sampled: the only multisampled images it
         *       ever had were the forward path's, and that path is gone. The sample count is fixed
         *       rather than a parameter so there is one less thing a caller can get wrong.
         */

        /**
         * @ingroup vulkan_core
         * @brief create a single-sampled 3D target image (the same allocation path as the 2D one)
         * @param width / @p height / @p depth the three extents, in texels
         * @note its own entry point rather than a defaulted fourth parameter on create_target_image:
         *       the two differ in exactly one field of VkImageCreateInfo (imageType), and a caller that
         *       reads `create_target_image_3d(w, h, d, ...)` cannot pass a depth of 1 by accident and
         *       then sample the result as a volume.
         */
        /**
         * @ingroup vulkan_core
         * @brief create a single-sampled CUBE target: a six-layer 2D array with CUBE_COMPATIBLE set
         * @param size the edge length of one face, in texels (all six faces are the same size)
         * @note its own entry point rather than a generalised array helper, for the same reason
         *       create_target_image_3d has one: a cube is six layers AND the compatibility flag, and a caller
         *       that got one of those wrong would have an image the sampler refuses.
         */

        // called depth_attachment_format, not depth_format: make_depth_pipeline keeps a parameter named
        // depth_format, which would hide the member of that name and MSVC /W4 reports C4458 (an error under /WX).
        VkFormat depth_attachment_format = {};
        std::vector<VkImage> depth_images = {};
        std::vector<VkDeviceMemory> depth_image_memories = {};
        std::vector<VkImageView> depth_image_views = {};

        VkCommandPool command_pool = {};

        /** @brief create the shared samplers above: device-level, reference-counted by nobody, destroyed with core */

        // ---- the SHARED samplers, created once with the device (see create_samplers) ----
        //
        // THEY LIVE HERE because a sampler is a device-level object with no per-frame state and no owner among the
        // passes: a pass DECLARES one by hint (see render_resource::shared::sampler_set) and the renderer hands over
        // the handles, so the object's owner has to be the device root - the same argument every image in this class
        // answers. Before this they were scattered across the runtime's scene setup, a pipeline builder and two
        // ensure_* functions - the "naming accident" this comment records.
        //
        // `env_sampler` is deliberately NOT here: its max_lod is the app's environment mip count, not a device fact.
        vk_sampler texture_sampler = {};      // the bindless texture array: REPEAT, and all its mip levels
        vk_sampler gbuffer_sampler = {};      // the G-buffer's stored surface: NEAREST, clamp (exact texel centres)
        vk_sampler taa_sampler = {};          // the TAA resolve: LINEAR magnification, NEAREST minification
        vk_sampler post_sampler = {};         // the post chain and the FXAA filter: LINEAR, clamp
        vk_sampler post_nearest_sampler = {}; // the composite's GI upsample: NEAREST, clamp (depths are not colours)
        vk_sampler shadow_sampler = {};       // the cascaded map: depth-compare + LINEAR (hardware PCF), clamp
        /**
         * @brief the CREATE INFO of @ref texture_sampler, kept because the descriptor heap needs it as an
         *        EMBEDDED SAMPLER: a heap binding for a combined image sampler takes its sampler from a
         *        VkSamplerCreateInfo (the driver creates one), not from a VkSampler, and it has to be the SAME
         *        sampler the descriptor-set path uses - a different max_lod alone changes which mip is read.
         * @note filled where the sampler is created (create_samplers), so the two cannot drift.
         */
        VkSamplerCreateInfo texture_sampler_info = {};
        /**
         * @brief the CREATE INFO of every shared sampler, in the order shaders/heap_slots.glsl names them, because
         *        a heap descriptor for a sampler IS a create info - and the heap is created LATER in the
         *        constructor than these samplers are (create_samplers runs first), so the infos have to be kept
         *        here to be written onto the sampler grid once it exists.
         * @note index 0 texture (linear, repeat mips), 1 post (linear, clamp, one mip), 2 gbuffer (nearest, clamp),
         *       3 post-nearest (nearest, clamp), 4 taa (linear mag / nearest min, clamp), 5 shadow (depth compare).
         */
        std::array<VkSamplerCreateInfo, 6> shared_sampler_infos = {};

        vma_allocator vma = {};
        /// the device-wide descriptor heap (see vulkan/core/descriptor_heap): one resource heap and one
        /// sampler heap, created right after the device so every pass can be written against it
        descriptor_heap descriptor_heaps = {};
        /// the heap's limits, copied out of the capability query (see core::init_device_and_queue) because the
        /// heap itself is created in the constructor, after vma.init() - the capabilities are not in scope there
        heap_limits descriptor_heap_limits = {};
        /**
         * @brief the reserved heap blocks, in bytes, or VK_WHOLE_SIZE when the heap is not in use
         *
         * @note THE LAYOUT IS OWNED HERE, and that is not tidiness: the heap's contents are written by the RUNTIME
         *       (it is what knows the textures and the material table) while the SLOT NUMBERS that point the
         *       shaders at them are named in `shaders/heap_slots.glsl`. Both sides have to use the same number, and
         *       a mismatch - a write at one offset, a shader reading another - is invisible to validation and shows
         *       up only as a wrong picture. So the blocks are reserved once, here, and published; the startup log
         *       prints the whole table, which is how a drift between the two sides is seen.
         */
        VkDeviceSize heap_texture_array_offset = VK_WHOLE_SIZE;
        VkDeviceSize heap_material_table_offset = VK_WHOLE_SIZE;

        /**
         * @brief THE SLOT GRID: where every descriptor this renderer uses lives, in SLOTS of @ref heap_slot_stride
         *        bytes, and why an index is a slot number rather than a byte offset
         *
         * @note A HEAP-NATIVE SHADER CAN ONLY NAME `array[index]`, which GL_EXT_descriptor_heap resolves to
         *       `heapBase + index * stride`. Every such array aliases the heap FROM OFFSET 0, so an index IS a
         *       byte offset divided by that array's stride - and the only way a shader can find its data at a
         *       compile-time-known index is for the data to sit on a grid whose base is FIXED. Both heaps start
         *       their grid at 1 MiB (past any reserved window a driver reports here - 94 KiB of resource heap,
         *       64 KiB of sampler heap - and the same number on every device), the stride is 64 B, and every
         *       array below is a CONTIGUOUS RUN OF SLOTS.
         *
         * @note @ref heap_slot_stride IS AN OVERRIDE, not the device's stride: the resources are mixed kinds
         *       (16 B buffers, 32 B images at this device), and one power-of-two stride covering all of them is
         *       what makes a single grid possible - the shader declares `descriptor_stride = 64` and the host
         *       writes 64 B apart. A device whose largest descriptor exceeded it would make the grid ambiguous,
         *       so that is checked once at startup and the grid is REFUSED rather than wrong.
         *
         * @note THESE NUMBERS ARE MIRRORED IN shaders/heap_slots.glsl and they have to be, because the shader
         *       bakes its base indices. A drift is invisible to validation and shows up as a wrong picture, so
         *       the constructor logs this table (see the `descriptor heap: slot grid` line).
         */
        static constexpr VkDeviceSize heap_slot_stride = 64;
        /// THE GRID'S BYTE OFFSET FOR A SLOT (see core::heap_slots and docs/descriptor_heap_migration.md): every
        /// descriptor is 64 B from the next, so a write HERE and a heap-native shader's `array[slot]` with
        /// `descriptor_stride = 64` are the same address by construction. There is no second stride to disagree
        /// with - which is exactly what the older per-slot block could not promise, because it mixed the device's
        /// 16 B buffer stride with its 32 B image stride and put each binding at its own offset.
        /// @note A SLOT NUMBER IS ALREADY ABSOLUTE: `core::heap_slots::x` includes `heap_slot_base`, so this is a
        ///       multiply and nothing else. The first version added `heap_grid_offset` as well and doubled the
        ///       1 MiB base - every write landed past the heap and was refused, which the heap's own bounds check
        ///       reported (`... did not fit at offset 2130432`).
        /// @note THIS IS THE ONE COPY: it used to be duplicated in runtime:constructor, runtime:frames and
        ///       runtime.cpp, which is what a helper defined in terms of the stride below invites. It belongs
        ///       beside the constant it multiplies, and it is exported so those units can drop their own.
        [[nodiscard]] static constexpr VkDeviceSize heap_slot_offset(uint32_t const slot) noexcept {
            return static_cast<VkDeviceSize>(slot) * heap_slot_stride;
        }

        // NOT renamed, unlike the other members that round: `heap_slot_base` is a NAME CONTRACT with
        // shaders/heap_slot_constants.glsl, which declares the same constant under the same name, and
        // tests/test_render_resources.cpp parses both files and requires the names AND the values to match
        // (tests/test_goo_toon_math.cpp greps this line by that name too). The C4458 that a lambda parameter of
        // this name caused in core.constructor.cppm is fixed THERE instead, by renaming the parameter - a local,
        // so the contract stays intact.
        static constexpr uint32_t heap_slot_base = 16384;  // 1 MiB / 64 B: the grid's slot 0
        static constexpr uint32_t heap_slot_count = 1024;  // 703 slots are in use, the rest is room to grow
        static constexpr uint32_t heap_image_capacity = 8; // per-swapchain-image arrays (3-4 images in practice)
        /// The SAMPLER heap is a second grid with its own base, and it cannot share the resource one: the API caps
        /// the sampler heap at 128 KiB, so 64 KiB - the reserved window the embedded-sampler path requires - is the
        /// largest base it can have. Its stride is the device's own sampler descriptor size (32 B here).
        static constexpr uint32_t heap_sampler_base = 2048; // 64 KiB / 32 B
        static constexpr VkDeviceSize heap_sampler_stride = 32;
        struct heap_slots {
            static constexpr uint32_t textures = heap_slot_base + 0u;    // binding 1, the bindless array
            static constexpr uint32_t materials = heap_slot_base + 512u; // binding 5
            /**
             * @brief the top level structure, binding 16, as a TWO-SLOT array - and the reason it is not beside the
             *        per-frame buffers above
             *
             * @note THE TLAS IS REBUILT EVERY FRAME (see runtime::write_rt_structure_binding, which is called per
             *       frame slot), so a single slot would hold one frame's structure while the other frame is still
             *       in flight - the same hazard the camera and light UBOs have, and they are two slots each for it.
             *       This was first laid out as ONE slot beside the per-frame buffers, which is the kind of mistake
             *       a picture cannot show until the shaders read the heap: the fix is cheap now and would have been
             *       a silent wrong image later. It lives at the END of the used region because growing it in place
             *       would renumber every array after it.
             */
            static constexpr uint32_t tlas = heap_slot_base + 703u;
            /**
             * @brief the MESHLET TABLE (docs/mesh_shaders.md step 3): one 48-byte record per meshlet
             *
             * @note ONE SLOT, not a per-frame pair, and that is a property of the data rather than a shortcut: the
             *       table is written ONCE, while the scene is imported and before any frame is recorded, and never
             *       touched again - so there is no frame in flight whose contents could disagree with it. (The TLAS
             *       above is the counter-example: it is rebuilt every frame, which is why it owns two slots.)
             * @note it lives at the END of the used region for the reason stated there: the arrays above are
             *       numbered by their position, so growing one in place would renumber everything after it.
             */
            static constexpr uint32_t meshlets = heap_slot_base + 745u;
            /**
             * @brief the MESH CULLING COUNTERS (docs/mesh_shaders.md step 3, "what the culling buys"): one per-frame
             *        lane of 8 uints, added to by the mesh entries and read back by the host at shutdown
             *
             * @note ON THE HEAP rather than in a host-only buffer, because a mesh stage's only way to reach memory
             *       is a heap descriptor - there is no binding model left to hang a counter off. The read-back is a
             *       plain mapped read after `wait_idle`, which is why the buffer is host-visible and coherent.
             */
            static constexpr uint32_t meshlet_stats = heap_slot_base + 746u;
            /**
             * @brief THE HOST-CULLED MESHLET TABLE (docs/mesh_shaders.md step 3, the culling's cheapest stage): a
             *        per-frame lane of `meshlet_capacity` records, written by the host while it records a CULLED
             *        session's draws and read by that session's mesh entry
             *
             * @note per frame rather than one slot, unlike the table itself: this one is rewritten every frame from
             *       the camera, so the frame in flight that is being recorded must not overwrite the one the GPU is
             *       still reading - the rule every per-frame buffer in this renderer follows.
             * @note the compaction is the whole point: the host writes SURVIVORS contiguously, so the dispatch's
             *       group count is the survivor count and no workgroup is launched for a culled meshlet.
             */
            static constexpr uint32_t meshlet_culled = heap_slot_base + 747u;
            // THE TOON LANES THAT DO NOT FIT THE MATERIAL RECORD: one `uvec4` per material, written once at
            // import. x is the face SDF map's texture-array index (`_SDFLightmap`), y the metallic/gloss map's
            // (`_MetallicGlossMap`), and z and w are reserved for the lanes the rest of the character work
            // adds. See heap_slot_constants.glsl for why they are a buffer of their own rather than components
            // of the material record, and why it is one descriptor rather than a per-frame pair.
            static constexpr uint32_t toon_lanes = heap_slot_base + 748u;
            // THE HEAD FRAME the face SDF shades against, one block per frame slot because on a model whose
            // head turns it changes every frame. It is its own block rather than a field of the camera's: the
            // head frame belongs to the CHARACTER, not to the eye looking at it.
            static constexpr uint32_t scene_head = heap_slot_base + 749u;
            // THE TOON LIGHT RIG (`deren::vulkan::toon_rig`): the character stage's global numbers - the sun/head-light
            // split, their shadow-side colours and the chain's scalars - written once from the application's
            // config. ONE descriptor and not a per-frame pair, because the values are fixed for a run exactly as
            // the material table's and the toon lane table's are: nothing writes it while a frame is in flight.
            //
            // ONE SLOT PAST THE HEAD BLOCK AND NOT BESIDE IT, which is a MEASURED correction rather than
            // spacing: `scene_head` above is a PER-FRAME-SLOT array, so it occupies 749 AND 750 (two frames in
            // flight), and a rig placed at 750 was therefore overwritten every frame by the second slot's head
            // frame. What that looked like was not a crash and not a validation error: the rig simply read as the
            // head frame's own numbers - `_DayStrength` came out of the head basis' first lane, i.e. zero - so
            // the two-state lighting silently sat in its NIGHT state for every frame of both captures. The
            // captures differed by 0 pixels before this line moved.
            static constexpr uint32_t toon_rig = heap_slot_base + 751u;
            // THE MATERIAL COLOURS (see `deren::vulkan::toon_colour_lane`): one `vec4` per lane per material, written once
            // at import from the same sidecar the texture lanes come from. A buffer of its own because the value is
            // FOUR FLOATS and the material record has nowhere to put it here: the record is INLINE in the per-draw
            // push block (see `sdf_lanes` above for the measurement), so a colour lane could not be a record field
            // even if the record had room. The REFERENCE PORT solved the same problem by growing ITS record from
            // 128 B to 192 B - a shape this renderer cannot copy, and does not need to, because the lane table
            // pattern already exists here.
            //
            // ... AND BECAUSE THEY LIVE HERE RATHER THAN IN THE RECORD, THEY ARE THEIR OWN TERM OF THE MATERIAL
            // DEDUP KEY: `material_slot_cache` (see `runtime.declarations.cppm`) keys the record, the two texture
            // lane blocks AND these six lanes' bytes, because `register_material` writes this table only AFTER its
            // early return - six lanes left out of the key are six lanes the second of two record-identical
            // materials reads from the first one. See the "材质去重键补上 colour lanes" section of `remaining_port_spec.md` for the probe.
            static constexpr uint32_t toon_colours = heap_slot_base + 752u;
            /// THE ARTICLE'S POST LUT: see `runtime::set_post_lut` and `heap_slots_post_lut` in the shader's slot
            /// file, which `test_render_resources` holds against this spelling.
            static constexpr uint32_t post_lut = heap_slot_base + 753u;
            /**
             * THE GOO REFERENCE'S PRE-INTEGRATED FGD LOOKUP TABLE (see `runtime::set_goo_fgd_lut` and
             * `heap_slots_goo_fgd_lut` in the shader's slot file, which `test_render_resources` holds against this
             * spelling).
             *
             * 754 AND NOT 750: `scene_head` is a PER-FRAME-SLOT array and occupies 749 AND 750 - the note on
             * `toon_rig` above records what happened to the last constant that forgot this - so the next free slot
             * above the post LUT is 754.
             *
             * IT IS A GLOBAL IMAGE RATHER THAN A MATERIAL LANE, which is the architecture ruling the step-5 spec
             * makes from the reference's own graph (§3.4): the Goo `GetPreIntegratedFGDGGXAndDisneyDiffuse` group
             * has ONE `ShaderNodeTexImage`, `users == 3` containers share it, and its coordinate is computed from
             * `sqrt(NoV)` / `perceptualRoughness` / `fresnel0` - never from a material's uv. Routing it through
             * `toon_slot` would need eleven sidecar rows pointing at one file, and zero new information.
             */
            static constexpr uint32_t goo_fgd_lut = heap_slot_base + 754u;
            static constexpr uint32_t scene_camera = heap_slot_base + 514u;        // binding 0, per frame slot
            static constexpr uint32_t scene_light = heap_slot_base + 516u;         // binding 7, per frame slot
            static constexpr uint32_t cluster_counts = heap_slot_base + 518u;      // binding 11, per frame slot
            static constexpr uint32_t cluster_indices = heap_slot_base + 520u;     // binding 12, per frame slot
            static constexpr uint32_t instance_transforms = heap_slot_base + 522u; // binding 6, per frame slot
            /** @brief the transforms of the previous frame, kept for motion vectors and TAA reprojection */
            static constexpr uint32_t previous_transforms = heap_slot_base + 524u; // binding 13, per frame slot
            /** @brief the skinned joint matrices for this frame, written by the skinning pass */
            static constexpr uint32_t skin_matrices = heap_slot_base + 526u;   // binding 9, per frame slot
            static constexpr uint32_t morph_data = heap_slot_base + 528u;      // binding 10, per frame slot
            static constexpr uint32_t mask_instances = heap_slot_base + 530u;  // binding 17, per frame slot
            static constexpr uint32_t env_cube = heap_slot_base + 532u;        // binding 2
            static constexpr uint32_t irradiance_cube = heap_slot_base + 533u; // binding 3
            static constexpr uint32_t brdf_lut = heap_slot_base + 534u;        // binding 4
            static constexpr uint32_t shadow_map = heap_slot_base + 535u;      // binding 8, per image
            /** @brief the ray-traced visibility image, sampled by the lighting stage */
            static constexpr uint32_t rt_visibility = heap_slot_base + 543u; // binding 15, per image
            /**
             * @brief the SAME image as @ref rt_visibility, as a STORAGE descriptor instead of a sampled one
             *
             * @note TWO DESCRIPTORS FOR ONE IMAGE, and not redundancy: SAMPLED_IMAGE and STORAGE_IMAGE are different
             *       descriptor kinds and no single heap descriptor is both, while this image is WRITTEN by the
             *       ray-traced visibility pass and SAMPLED by the lighting stage. It lives at the end of the used
             *       region for the same reason the TLAS does - growing an array in place would renumber every array
             *       after it.
             */
            static constexpr uint32_t rt_visibility_storage = heap_slot_base + 711u;
            static constexpr uint32_t gbuffer_albedo = heap_slot_base + 551u; // per image, then five in a row
            static constexpr uint32_t gbuffer_normal = heap_slot_base + 559u;
            static constexpr uint32_t gbuffer_material = heap_slot_base + 567u;
            static constexpr uint32_t gbuffer_depth = heap_slot_base + 575u;
            static constexpr uint32_t gbuffer_velocity = heap_slot_base + 583u;
            static constexpr uint32_t ml_trace = heap_slot_base + 591u; // per image: megalights' chain
            /// @brief the same two images written as STORAGE descriptors (see @ref rt_visibility_storage)
            /// @note their compute passes WRITE them and the lighting stage SAMPLES them, and no single heap
            ///       descriptor is both kinds - so the trace and the resolve each need a second slot, at the end of
            ///       the used region for the same reason the others are there.
            static constexpr uint32_t ml_trace_storage = heap_slot_base + 719u;
            static constexpr uint32_t ml_resolved_storage = heap_slot_base + 735u;
            /**
             * @brief the joint blocks as they were ONE FRAME AGO, per frame slot: the deformation half of a
             *        motion vector
             *
             * @note A SECOND per-frame family rather than more slots inside @ref skin_matrices, because a
             *       vertex's motion vector needs the matrices the PREVIOUS frame drew with and the current
             *       buffer has already been overwritten with this frame's by the time the frame records.
             *       The layout, the indices and the frame-slot rule are identical to the current family's -
             *       that is what lets the shader read the same `skin_base` from this slot and lets the
             *       runtime publish into the CURRENT frame slot's buffer (see
             *       runtime::advance_motion_deformations), exactly as @ref previous_transforms does for the
             *       world matrices. It lives at the END of the used region for the same reason the TLAS and
             *       the two storage twins do: growing an array in place would renumber every array after it.
             */
            static constexpr uint32_t skin_matrices_previous = heap_slot_base + 743u;
            static constexpr uint32_t ml_history = heap_slot_base + 599u;
            static constexpr uint32_t ml_resolved = heap_slot_base + 607u;
            static constexpr uint32_t taa_current = heap_slot_base + 623u; // per image: TAA's pair
            static constexpr uint32_t taa_history = heap_slot_base + 631u;
            static constexpr uint32_t post_color = heap_slot_base + 639u; // per image: the post chain
            static constexpr uint32_t bloom_l0 = heap_slot_base + 647u;
            static constexpr uint32_t bloom_l1 = heap_slot_base + 655u;
            static constexpr uint32_t bloom_l2 = heap_slot_base + 663u;
            static constexpr uint32_t bloom_l3 = heap_slot_base + 671u;
            static constexpr uint32_t display_color = heap_slot_base + 695u;
        };
        /// @brief the byte offset of slot 0 (see @ref heap_slots), or VK_WHOLE_SIZE when the heap is not in use
        VkDeviceSize heap_grid_offset = VK_WHOLE_SIZE;

        // ---- frame synchronization (timeline semaphores; see create_sync_objects) ----
        // vkAcquireNextImageKHR and vkQueuePresentKHR both require BINARY semaphores:
        //   - image_available_semaphores: binary, per frame slot (acquire signal)
        //   - present_ready_semaphores: binary, ONE PER SWAPCHAIN IMAGE — present may run on a
        //     separate queue, so a per-slot binary could be re-signaled before the previous
        //     present consumed it; a swapchain image is only re-acquired after its present
        //     finished, which keeps this per-image gate safe across queues
        //   - frame_done_semaphores / frame_done_values: TIMELINE, per frame slot, counting
        //     submissions — submit() signals it (GPU completion), wait_frame_slot() is the host
        //     pacing wait that used to be vkWaitForFences
        std::vector<VkSemaphore> image_available_semaphores = {}; // binary, per frame slot (acquire)
        std::vector<VkSemaphore> present_ready_semaphores = {};   // binary, per swapchain image (present wait)
        std::vector<VkSemaphore> frame_done_semaphores = {};      // timeline, per frame slot
        std::vector<uint64_t> frame_done_values = {};             // last signaled value per slot (host bookkeeping)

        size_t current_frame = 0;

        void to_next_frame() noexcept;
        void wait_frame_slot(uint32_t slot) const; // host wait until this slot's last submission completed

        static constexpr int32_t MAX_FRAMES_IN_FLIGHT = 2;

        // ---- GPU pass timing (VK_QUERY_TYPE_TIMESTAMP) ----
        // A timestamp pool with one contiguous range of gpu_timing_mark_capacity queries per frame
        // slot: the frame records vkCmdResetQueryPool + one vkCmdWriteTimestamp per pass boundary
        // and the reader converts consecutive marks into milliseconds after the slot's submission
        // completed (see mark_gpu_timing / read_gpu_timings). Timestamps need no feature bit, but a
        // queue family that cannot write them reports timestampValidBits == 0, and the tick length
        // comes from the device limits - either missing means gpu_timing_supported stays false and
        // every timing call is a no-op, so callers do not have to check the device themselves.
        VkQueryPool timestamp_query_pool = VK_NULL_HANDLE;
        float timestamp_period_ns = 0.0f;  // ns per tick (VkPhysicalDeviceLimits::timestampPeriod)
        uint32_t timestamp_valid_bits = 0; // graphics family counter width (0 = cannot timestamp)
        bool gpu_timing_supported = false;
        // marks the CURRENT recording of each slot has written (reset by begin_gpu_timing). Also
        // read back as "how many queries to fetch" for the submission that just completed, because
        // a slot is only read after it was paced and before it is recorded again.
        std::array<uint32_t, MAX_FRAMES_IN_FLIGHT> gpu_timing_marks = {};
        // frame_done value each slot's timings were last read for: a slot is read at most once per
        // submission, so a frame that hits an early return cannot fetch the same results twice
        std::array<uint64_t, MAX_FRAMES_IN_FLIGHT> gpu_timing_read_value = {};

        /**
         * @ingroup vulkan_core
         * @brief open this frame's timing range: reset the slot's queries and forget the previous
         *        frame's marks
         * @param command_buffer the frame's command buffer (the reset is recorded on the GPU
         *        timeline, which keeps it off the host/GPU race the pool would otherwise have)
         * @param slot the frame slot being recorded
         * @note call once per frame, before any mark and outside a dynamic rendering instance;
         *       a no-op when the device cannot timestamp
         */
        void begin_gpu_timing(VkCommandBuffer command_buffer, uint32_t slot) noexcept;

        /**
         * @ingroup vulkan_core
         * @brief write one timing mark into this frame's range
         * @param command_buffer the frame's command buffer
         * @param slot the frame slot being recorded
         * @param stage pipeline stage the mark resolves at: callers use TOP_OF_PIPE for the first
         *        mark of the frame and BOTTOM_OF_PIPE for every pass boundary, so mark i + 1 minus
         *        mark i is exactly how long pass i took
         * @note a no-op when the device cannot timestamp or the frame already wrote
         *       gpu_timing_mark_capacity marks
         */
        void mark_gpu_timing(VkCommandBuffer command_buffer, uint32_t slot, VkPipelineStageFlagBits stage) noexcept;

        /**
         * @ingroup vulkan_core
         * @brief convert a completed submission's marks into milliseconds
         * @param slot the frame slot to read (its last submission must have completed - pace the
         *        slot first, see wait_frame_slot)
         * @return the durations between consecutive marks, or a zero mark_count when timings are
         *         unavailable, the slot never submitted, or this submission was already read
         * @note never waits on the GPU and never blocks: vkGetQueryPoolResults is called without
         *       VK_QUERY_RESULT_WAIT_BIT, and an unavailable result reports "no measurement"
         *       instead of stalling
         */
        gpu_timing_result read_gpu_timings(uint32_t slot);

        /** @brief whether this device can measure GPU pass timings (see begin_gpu_timing) */
        [[nodiscard]] bool gpu_timing_available() const noexcept {
            return this->gpu_timing_supported;
        }

        core();
        /**
         * @ingroup vulkan_core
         * @brief THE construction: the contract's creation structure, taken directly
         * @param options the creation descriptor, `deren::promise::rhi::create_info`
         *        (promise/rhi/rhi.core_desc.cppm): title, size, render scale, vsync, validation layers,
         *        window visibility, and the optional native_window the CALLER owns
         *
         * THE CONTRACT'S STRUCTURE IS THE ONLY ONE. `vulkan::core_create_info` and the
         * `to_backend_create_info()` translation that stood between the two spellings are gone: the
         * initialisation run below reads the boundary's own fields, so there is one structure to add
         * a field to instead of two plus a mapping to keep in step. The one thing a translation still
         * does is the ABI GUARD: `options.struct_size` is compared once and a field whose whole extent
         * is not inside the bytes the caller declares keeps this build's default (see
         * core.constructor.cppm's `sanitize_create_info`), per the append-only rule in
         * rhi.core_desc.cppm.
         *
         * `options.native_window` non-null means BIND THE CALLER'S WINDOW: no window is created, none
         * is destroyed and none is shown or hidden - the caller keeps it alive for as long as this
         * core lives (the size/title/visibility fields are ignored in that mode). The caller also owns
         * GLFW's lifetime in that mode: it initialised GLFW before this core was constructed and it
         * terminates it after this core and its surface are gone.
         *
         * `options.window_title` is BORROWED until this call returns (GLFW copies it into the window);
         * the other fields are copied into `create_options` and read for the core's whole life, which
         * is why nothing here stores the title pointer.
         */
        explicit core(deren::promise::rhi::create_info const& options);
        ~core();

        vk_command_buffer make_command_buffer() const;
        /** @brief allocate a SECONDARY command buffer (recorded inside a dynamic rendering
         *         instance, executed there via vkCmdExecuteCommands) */
        vk_command_buffer make_secondary_command_buffer() const;
        /** @brief like make_secondary_command_buffer() but allocated from @p pool (a per-thread
         *         pool from make_command_pool(); the RAII wrapper frees into that same pool) */
        vk_command_buffer make_secondary_command_buffer(VkCommandPool pool) const;
        /**
         * @brief create an extra graphics command pool (RESET flag set, graphics queue family)
         *        whose lifetime is tied to this core (destroyed by the registered cleanup).
         *        Parallel recording needs one pool PER RECORDING THREAD - a single pool's
         *        command buffers must not be begun concurrently on different threads.
         * @note not const: registers the pool's destruction on this core (like create_command_pool)
         */
        VkCommandPool make_command_pool();

        std::optional<vk_shader_module> make_shader_module(std::span<uint8_t> shader) const noexcept;

        /**
         * @ingroup vulkan_core
         * @brief create an image view covering the whole image (all mip levels and layers)
         * @param image the image to view
         * @param format the view format
         * @param type view type (VK_IMAGE_VIEW_TYPE_2D / VK_IMAGE_VIEW_TYPE_CUBE ...)
         * @return raii vk_image_view owning the created view
         */
        vk_image_view make_image_view(VkImage image, VkFormat format, VkImageViewType type) const;

        /**
         * @ingroup vulkan_core
         * @brief create a 2D depth image view (DEPTH aspect) over the whole image
         * @param image the image to view
         * @param format the view format (a depth format)
         * @return raii vk_image_view owning the created view
         * @note the regular make_image_view uses the COLOR aspect; depth images (e.g. the shadow
         *       map) need the DEPTH aspect to be sampled as depth
         */
        vk_image_view make_depth_image_view(VkImage image, VkFormat format) const;

        /**
         * @ingroup vulkan_core
         * @brief create a 2D ARRAY depth image view covering every layer (DEPTH aspect)
         * @param image the layered depth image
         * @param format the view format (a depth format)
         * @return raii vk_image_view owning the created view
         * @note the cascaded shadow map is ONE layered image sampled as an array: a fragment shader
         *       picks its cascade per pixel, and dynamic indexing of a sampler array would require
         *       dynamically uniform indices, while a texture-array layer is just a coordinate
         */
        vk_image_view make_depth_array_view(VkImage image, VkFormat format) const;

        /**
         * @ingroup vulkan_core
         * @brief create a 2D depth image view of ONE layer (DEPTH aspect), for rendering into it
         * @param image the layered depth image
         * @param format the view format (a depth format)
         * @param layer the array layer to view
         * @return raii vk_image_view owning the created view
         */
        vk_image_view make_depth_layer_view(VkImage image, VkFormat format, uint32_t layer) const;

        /**
         * @ingroup vulkan_core
         * @brief create a linear/min-linear sampler with the given wrap mode
         * @param address_mode wrap mode applied to all three axes
         * @param max_lod maximum mip level the sampler may access
         * @return raii vk_sampler owning the created sampler
         */
        vk_sampler make_sampler(VkSamplerAddressMode address_mode, float max_lod) const;

        /**
         * @ingroup vulkan_core
         * @brief create the shadow map sampling sampler (NEAREST + clamp-to-edge)
         * @return raii vk_sampler owning the created sampler
         * @note pbr.frag does manual percentage-closer filtering: it fetches the stored depth
         *       with this NEAREST sampler at a few neighbor texels and averages the comparisons,
         *       so no depth-comparison/linear-filter format feature is required
         */
        vk_sampler make_shadow_sampler() const;

        /**
         * @ingroup vulkan_core
         * @brief create a depth-only graphics pipeline (no color attachment, single sample),
         *        used by the shadow pass to render depth into the shadow map
         * @param vertex_shader_code raw SPIR-V binary of the vertex shader
         * @param fragment_shader_code raw SPIR-V binary of the fragment shader
         * @param depth_format depth attachment format (dynamic rendering only)
         * @param depth_bias_constant_factor constant rasterization depth bias added to depth
         * @param depth_bias_slope_factor slope-scaled depth bias (removes shadow acne on angled surfaces)
         * @param depth_bias_clamp maximum depth bias magnitude (0 = no clamp)
         * @return vk_pipeline on success, error message on failure
         */
        std::expected<vk_pipeline, std::string_view> make_depth_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkFormat depth_format,
            float depth_bias_constant_factor = 0.0f,
            float depth_bias_slope_factor = 0.0f,
            float depth_bias_clamp = 0.0f) const;

        // The clustered-light-culling compute pipeline is NOT here any more: the CLUSTER PASS owns it
        // (deren.vulkan.pass.cluster builds it through vulkan.pipelines::build_cluster from the shared scene block
        // layout). `core::make_cluster_pipeline` built it against the core's own scene pipeline layout, which a
        // pass cannot own - and a pipeline only that pass names is that pass's to build and to release.

        /**
         * @ingroup vulkan_core
         * @brief rebuild the swapchain and every per-generation target
         * @return true when a NEW generation was actually built; false when the recreation was
         *         DEFERRED because the window has no drawable size (a minimized window reports a 0x0
         *         currentExtent, and vkCreateSwapchainKHR rejects that).
         *
         * THE RETURN VALUE IS NOT DECORATION. A caller that treats a deferred call as a rebuild
         * invalidates all the per-image state - the temporal histories, the descriptor families, the
         * layout flags - for a generation that still exists, and pays a full re-convergence for a
         * non-event: a minimize/restore dropped the GI and TAA history twice, once for the deferred
         * recreate and once for the real one.
         */
        [[nodiscard]] bool recreate_swap_chain();
        // one-time log for the "recreation deferred because the window has no drawable size" case
        // (see recreate_swap_chain); reset as soon as a recreation actually runs
        bool zero_extent_recreation_logged = false;

        /**
         * @ingroup vulkan_core
         * @brief submit the recorded command buffer for the current frame slot
         * @param command_buffer the command buffer to submit
         * @param image_index the acquired swapchain image index: it selects the per-image binary
         *        semaphore this submit signals for present to wait on
         * @return the result of vkQueueSubmit
         *
         * Signals two semaphores. This slot's TIMELINE (GPU completion, and the host pacing that
         * wait_frame_slot() blocks on) and present_ready_semaphores[image_index]. The second is a
         * BINARY semaphore per swapchain IMAGE rather than per frame slot because vkQueuePresentKHR
         * cannot wait a timeline semaphore, and present may run on a separate queue - keying the gate
         * on the image means an image is only re-acquired after its own present finished, which keeps
         * a re-signal from racing across queues.
         */
        /// @brief acquire the next swapchain image (vkAcquireNextImageKHR) for the frame slot in progress
        ///
        /// THE ACQUIRE BELONGS TO WHOEVER OWNS THE DEVICE AND THE SWAPCHAIN, which is this object: it
        /// is the primitive behind the contract's `frame_begin()` AND behind the runtime's own pacing
        /// path, so both go through one implementation instead of two copies.
        /// The POLICY for the result stays with the caller: the runtime maps OUT_OF_DATE to a
        /// swapchain rebuild plus `skipped`, while the contract's `frame_begin()` collapses anything
        /// but success into a zeroed `submit_info` (tier-1 has no error channel, plan §3.3).
        [[nodiscard]] VkResult acquire_next_image(uint32_t& image_index);
        VkResult submit(VkCommandBuffer command_buffer, uint32_t image_index);

        /**
         * @ingroup vulkan_core
         * @brief present the rendered swapchain image, waiting on that image's binary present-ready
         *        semaphore (signaled by submit())
         * @param image_index the swapchain image index to present
         * @return the result of vkQueuePresentKHR
         */
        VkResult present(uint32_t image_index) const;

        /**
         * @ingroup vulkan_core
         * @brief create the G-buffer pipeline: the shared scene layout, the three gbuffer_formats
         *        color targets and a single-sampled depth attachment
         * @param vertex_shader_code raw SPIR-V of the vertex stage (pbr.vert: instancing / skinning /
         *        morphing are identical to what the forward path did)
         * @param fragment_shader_code raw SPIR-V of the fragment stage (gbuffer.frag: writes the
         *        three targets and shades nothing)
         * @param first_stage the stage that emits the geometry: VERTEX for the input-assembler path (the default),
         *        MESH for a MESH entry that fetches its own vertices (docs/mesh_shaders.md step 2) - the fragment
         *        stage is the same shader either way, which is what makes the two pipelines comparable
         * @return vk_pipeline on success, error message on failure
         * @note single-sampled on purpose (a G-buffer cannot be multisampled without per-sample
         *       shading), so this pipeline may NOT be recorded into an instance whose attachments are
         *       the HDR target: its own attachments are the core::gbuffer_* targets and the pass that owns them
         */
        std::expected<vk_pipeline, std::string_view> make_gbuffer_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_VERTEX_BIT) const;

        /**
         * @brief the CHARACTER-FORWARD pipeline: the toon shading stage that OVERWRITES the deferred-lit
         *        character pixels, ONE HDR colour target, no blending, depth test on, depth compare EQUAL
         *
         * WHAT MAKES IT DIFFERENT FROM THE OTHER FORWARD BUILDER, and each difference is forced:
         *  - ONE target, and it is `hdr_format`, not the swapchain format. The named forward pipelines
         *    (`runtime::make_pipeline`: `unlit`, `pbr`, `pbr_premult`) are built for the SWAPCHAIN image
         *    because that is what a forward session's default target was; this pass runs INSIDE the HDR
         *    chain, before the resolve, so that the grade and the tonemap stay the post chain's job. A
         *    toon stage that wrote the swapchain would be tonemapping a second time.
         *  - BLENDING OFF (`make_color_blend_attachment_opaque`), because the pass OVERWRITES. The named
         *    forward pipelines blend src-alpha, which is right for coverage and wrong here: the toon result
         *    is a finished colour, not a layer.
         *  - DEPTH COMPARE `EQUAL`. See make_depth_stencil_state's second parameter: this is the only
         *    caller in the renderer that needs an operator other than LESS_OR_EQUAL.
         *
         * DEPTH WRITE IS NOT SET HERE because it is a dynamic state: the pass turns it off per draw, the
         * same way the outline pass does. Leaving it at the builder's default (on) would let a toon
         * fragment write depth over the surface the lighting stage already committed.
         *
         * @param vertex_shader_code the MESH stage that emits the geometry (docs/mesh_shaders.md step 4:
         *        there is no vertex geometry path left, so this is the pbr mesh entry, exactly as the
         *        named forward pipelines use)
         * @param fragment_shader_code the toon fragment stage
         * @param first_stage MESH for the mesh entry, MESH for the meshlet entry
         * @return vk_pipeline on success, error message on failure
         */
        std::expected<vk_pipeline, std::string_view> make_character_forward_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_MESH_BIT_EXT) const;

        /**
         * @ingroup vulkan_core
         * @brief build the OVERLAY pipeline: the article's two framebuffer multiplies (`MyZmdEyeDarkShader`,
         *        `MyZmdHairShadowShader`), which darken an already shaded character by a mask
         *
         * WHAT MAKES IT AN OVERLAY, and each of the four facts is forced:
         *  - ONE target, and it is `hdr_format`: the multiply has to read the TOON result, which lives in the
         *    HDR chain before the resolve - the same reason `make_character_forward_pipeline` takes this format.
         *  - BLEND `dst = src * dst` (`make_color_blend_attachment_multiply`). This is the article's
         *    `BlendOp Multiply` / `Blend 5,1` expressed in core Vulkan factors, so no blend extension is needed;
         *    see that function for the derivation and for why the alpha channel is left alone.
         *  - DEPTH TEST ON, compare `LESS_OR_EQUAL` (and NOT the toon stage's `EQUAL`): an overlay is a different
         *    mesh whose quads sit slightly in FRONT of the surface they darken, so `EQUAL` would reject it.
         *  - DEPTH WRITE is not set here because it is a DYNAMIC state: the character-forward pass has already
         *    turned it off and LOCKED it for the whole instance (see `render_environment::depth_write_locked`),
         *    which is also what keeps the mask from occluding anything.
         *
         * WHAT IS NOT REPRODUCED IS THE ARTICLE'S STENCIL on the hair shadow (`Stencil { Ref 1 Comp Equal Pass
         * Keep }`), and it is unreachable rather than omitted: this renderer's dynamic rendering info declares
         * `stencilAttachmentFormat = VK_FORMAT_UNDEFINED` (see vulkan/constant_init), so no pass in the chain has
         * a stencil plane to test against. The substitute is the geometry's own coverage - the mask meshes are
         * shaped to the features they darken - which is why the difference is recorded here rather than hidden.
         *
         * @param vertex_shader_code the MESH stage that emits the geometry (docs/mesh_shaders.md step 4: the
         *        geometry path is mesh-only, so this is `pbr.mesh.spv`, exactly as the toon stage uses)
         * @param fragment_shader_code the overlay fragment stage (`overlay.frag.spv`)
         * @param first_stage MESH for the mesh entry, MESH for the meshlet entry
         * @return vk_pipeline on success, error message on failure
         */
        std::expected<vk_pipeline, std::string_view> make_overlay_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_MESH_BIT_EXT) const;
        /**
         * @brief the ARTICLE'S ① 描边 pipeline: the inverted hull, drawn over the toon result
         *
         * THREE STATES, and each is the article's rather than a preference: ONE HDR colour target (the hull is part
         * of the character stage, so it lands where that stage wrote and the tonemap stays the post chain's), an
         * OPAQUE blend (`MyZmdOutlineShader` states no `Blend`), and `LESS_OR_EQUAL` - the article's own `ZTest`
         * default, which is also the only operator that can keep a hull's outer ring, since those fragments are
         * not the surface the G-buffer recorded (see the definition in core.cpp).
         *
         * CULL FRONT IS NOT A PARAMETER HERE: the rasterization state is dynamic, so the front-face culling that
         * makes a hull an outline is `render_environment::forced_cull_front`, set by the character-forward pass
         * around the outline group only.
         */
        std::expected<vk_pipeline, std::string_view> make_outline_pipeline(
            std::span<uint8_t const> vertex_shader_code,
            std::span<uint8_t const> fragment_shader_code,
            VkShaderStageFlagBits first_stage = VK_SHADER_STAGE_MESH_BIT_EXT) const;

    private:
        // ---- THE INITIALIZATION STEPS, and they are private because the constructor is their only caller:
        //      the order in which they run IS the initialization order (see the constructor in core.cpp), and a
        //      caller that could run one on its own would be able to build half a core. The exception is the
        //      swapchain recreation path, which is a member of this class and re-runs three of them. What stays
        //      PUBLIC is the state they produce (the device, the queues, the samplers, the image views and the
        //      layouts) plus the facade operations (wait_idle, set_window_title).
        void init_instance() noexcept;
        void init_window(int32_t width, int32_t height, std::string_view window_name = "") noexcept;
        void init_surface() noexcept;
        void init_device_and_queue() noexcept;
        void init_swap_chain() noexcept;
        void init_image_views() noexcept;
        void create_render_targets();
        /**
         * @brief create a single-sampled device-local image with its memory, and return both
         * @param width / @param height the extent in texels
         * @param format the image format
         * @param tiling OPTIMAL or LINEAR (staging images that are mapped on the host)
         * @param usage the usage flags the image is created with
         * @param properties the memory type the image is bound to
         * @note every target the engine creates is single-sampled: the only multisampled images it
         *       ever had were the forward path's, and that path is gone. The sample count is fixed
         *       rather than a parameter so there is one less thing a caller can get wrong.
         */
        void create_target_image(
            uint32_t width,
            uint32_t height,
            VkFormat format,
            VkImageTiling tiling,
            VkImageUsageFlags usage,
            VkMemoryPropertyFlags properties,
            VkImage& image,
            VkDeviceMemory& image_memory) const noexcept;
        /**
         * @ingroup vulkan_core
         * @brief create a single-sampled CUBE target: a six-layer 2D array with CUBE_COMPATIBLE set
         * @param size the edge length of one face, in texels (all six faces are the same size)
         * @note its own entry point rather than a generalised array helper, for the same reason
         *       create_target_image_3d has one: a cube is six layers AND the compatibility flag, and a caller
         *       that got one of those wrong would have an image the sampler refuses.
         */
        void create_target_image_cube(
            uint32_t size,
            VkFormat format,
            VkImageTiling tiling,
            VkImageUsageFlags usage,
            VkMemoryPropertyFlags properties,
            VkImage& image,
            VkDeviceMemory& image_memory) const noexcept;
        void create_target_image_3d(
            uint32_t width,
            uint32_t height,
            uint32_t depth,
            VkFormat format,
            VkImageTiling tiling,
            VkImageUsageFlags usage,
            VkMemoryPropertyFlags properties,
            VkImage& image,
            VkDeviceMemory& image_memory) const noexcept;
        void create_depth_image(VkImage& image, VkDeviceMemory& image_memory, VkImageView& image_view) const noexcept;
        void create_depth_resources() noexcept;
        void create_color_resources();
        void create_command_pool() noexcept;
        void create_samplers();
        void create_sync_objects();
        void create_timestamp_query_pool() noexcept;
    };
} // namespace deren::vulkan