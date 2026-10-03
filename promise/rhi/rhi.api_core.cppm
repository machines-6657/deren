// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:api_core
//
// tier-1 of the promise contract: the backend's context and its factories (RHI
// plan v4, §1.11, §3.3, §4.1).
//
// `api_core` is the ONE object that crosses the boundary as a C++ type, and it is
// reached through a single C function, `deren_make_api_core` - declared with the
// other two entry points in promise/rhi/backend_entry.hpp, because an entry point is
// the ABI surface rather than a virtual base class (m03159). Everything below
// follows from the cross-boundary rules in §4.2:
//
//   - every member is virtual, the destructor is `virtual ... noexcept`, and there
//     is no data member and no non-inline definition: the vtable is the boundary,
//     and destruction has to reach the backend's own `operator delete`;
//   - an OWNED handle carries ONE REFERENCE, dropped by `release()`, once, inside the
//     backend; `object_manager<T>` is the owner spelling of that call (RAII, move-only).
//     RELEASE IS NOT NECESSARILY DESTRUCTION: the backend may serve the same resource to
//     several callers (a content-keyed registry + a reference count sit behind these
//     handles), so the object dies with its LAST reference and a caller must not read the
//     call as "the memory is free now" - nor touch the handle afterwards;
//   - a BORROWED view (`frame_image()`, `frame_readback_buffer()`, `begin_commands()`)
//     holds no reference at all and must never be managed;
//   - the engine builds `std::shared_ptr<api_core>(raw, &deren_destroy_api_core)`
//     (or the pointer it resolved from the loaded library) - never the default
//     deleter, which would free DLL memory with the executable's allocator;
//   - `abilities()` is the tier-2 bit set (promise/rhi/rhi.extension.cppm) and
//     `query_extension()` is the only way to reach an ability, so adding one does
//     not grow this vtable;
//   - parameters are PODs, `std::span`, or opaque handles. No `std::string`, no
//     container, no exception, no `dynamic_cast` (`-fno-rtti`);
//   - the portable code touches nothing else, and this interface must never absorb
//     engine concepts: no scene, no pass, no camera, no toon (§4.1 item 2).
//
// The `*_desc` structs other than `buffer_desc` are forward-declared on purpose.
// This round lands the virtual base classes; the descriptor contents are S1 design
// surface (§5's capability table, §6.4's usage and barrier model). A reference to an
// incomplete type is all a virtual declaration needs, so the shape of the contract
// can be reviewed before the shapes it carries are frozen.
//
// The `std::shared_ptr` wrapper the engine uses belongs on the ENGINE side (plan
// §4.1 item 3: "住在模块里 inline 即可，不需要 DLL 配合"), so it is not here.
// ============================================================================
module;

#include <cstdint>
#include <span> // std::span: buffer::mapped() hands the caller the bytes of a host-visible buffer

export module deren.promise.rhi:api_core;

import :extension;

/**
 * @file promise/rhi/rhi.api_core.cppm
 * @brief tier-1 of the promise contract: the backend's context and its factories.
 * @ingroup promise
 *
 * `api_core` is the one object that crosses the boundary as a C++ type, and it is reached through a
 * single C function, `deren_make_api_core()` in `promise/rhi/backend_entry.hpp` (§1.11: the way to
 * obtain the context stays C++, the rest of the boundary is C). It is also the one interface the
 * plan lets grow "big" - it is the backend's whole context and factory set - under the one
 * restriction that it never learns an engine concept: no scene, no pass, no camera (§4.1 item 2).
 *
 * Applications and passes reach optional features through `abilities()` + `query_extension()`
 * (tier-2, `promise/rhi/rhi.extension.cppm`) rather than through new virtuals here, which is what
 * keeps this vtable stable as the backend gains extensions.
 */

export namespace deren::promise::rhi {

    /// WHAT A BUFFER IS FOR, as the contract names it: the memory intent, not a Vulkan memory property.
    ///
    /// The vocabulary is the renderer's own census of buffer uses (twelve creation sites today, counted
    /// in vulkan/runtime, readback, ray_tracing and acceleration_structure), and every value says what
    /// the CALLER does with the buffer - "the host writes it every frame", "only the GPU touches it",
    /// "it holds an acceleration structure". The backend maps each one to memory properties, tiling and
    /// an upload strategy, which is exactly the knowledge the contract is not allowed to have.
    ///
    /// VALUES ARE APPENDED, NEVER MOVED OR REUSED: a value is part of the ABI the moment it ships (the
    /// same rule the `error` codes and the ability bits follow).
    enum class buffer_usage : std::uint32_t {
        vertex = 0,                         ///< vertex data; GPU-only, uploaded once at creation
        index = 1,                          ///< index data; GPU-only, uploaded once at creation
        uniform_gpu_only = 2,               ///< uniforms the host updates rarely (or never)
        uniform_coherent = 3,               ///< per-frame uniforms: host-visible and coherent
        uniform_cached = 4,                 ///< host-visible, cached: the host reads it back
        storage_coherent = 5,               ///< a storage buffer the host also writes (e.g. a table the GPU reads)
        readback_coherent = 6,              ///< the destination of a GPU -> CPU copy; created WITHOUT contents
        acceleration_structure_storage = 7, ///< memory an acceleration structure lives in; allocate-only
        acceleration_structure_scratch = 8, ///< an acceleration structure build's scratch space; allocate-only
        storage_gpu_only = 9,               ///< a storage buffer only the GPU touches; allocate-only
    };

    /// Capabilities a buffer needs BEYOND its memory intent.
    ///
    /// These are the three things the renderer's creation sites ask for that are not about memory at
    /// all: the buffer's DEVICE ADDRESS has to be obtainable (bufferDeviceAddress), an acceleration
    /// structure build has to be able to READ it, and the opacity-micromap extension has to be able to
    /// use it as storage. Naming them here rather than passing raw Vulkan usage flags is the whole
    /// point of the contract: a caller says WHAT IT NEEDS, and the backend knows which bits that is -
    /// and a backend that cannot serve one says so by refusing the descriptor instead of silently
    /// allocating something that will be read the wrong way.
    enum class buffer_flag : std::uint32_t {
        device_address = 1u << 0,               ///< `vkGetBufferDeviceAddress`-style addresses must work
        acceleration_structure_input = 1u << 1, ///< readable by an acceleration structure build
        micromap_storage = 1u << 2,             ///< usable as `VK_EXT_opacity_micromap` storage
        storage = 1u << 3,                      ///< the GPU reads/writes it as a storage buffer (SSBO)
        indirect = 1u << 4,                     ///< it carries indirect dispatch/draw commands
        shader_binding_table = 1u << 5,         ///< it carries a ray-tracing shader binding table
        micromap_build_input = 1u << 6,         ///< readable by an opacity-micromap build
    };

    /// A set of `buffer_flag` bits, spelled the way the ability set is (`to_bits` + `has_flag`).
    using buffer_flags = std::uint32_t;

    /// The empty flag set, spelled where a caller passes "none".
    inline constexpr buffer_flags no_buffer_flags = 0u;

    /// The same value as a bitmask, for a caller composing a set.
    [[nodiscard]] constexpr auto to_bits(buffer_flag flag) noexcept -> buffer_flags {
        return static_cast<buffer_flags>(flag);
    }

    /// Whether @p flags has @p flag set.
    [[nodiscard]] constexpr auto has_flag(buffer_flags flags, buffer_flag flag) noexcept -> bool {
        return (flags & to_bits(flag)) != no_buffer_flags;
    }

    /// How a buffer is created: its memory intent, the capabilities it needs, its size, and - when the
    /// caller already has the bytes - its initial contents.
    ///
    /// THE INITIAL BYTES ARE PART OF THE DESCRIPTOR, not a second upload call, because that is where the
    /// renderer's deduplication lives: a backend that keys its resources on CONTENT (this one does, by
    /// XXH3-128 - see `release()`'s note) can only recognise identical content if it is handed the
    /// content at creation. `initial_bytes` empty means ALLOCATE ONLY: the buffer is created with no
    /// contents (a GPU-only target, a read-back slot, an acceleration structure's storage), and a
    /// backend that deduplicates must never match one of those against another.
    ///
    /// `struct_size` is the same ABI guard `create_info` carries and follows the same append-only rule:
    /// a field whose whole extent is not inside the bytes the caller declares keeps this build's
    /// default, so an older program talking to a newer backend is detected instead of mis-read.
    struct buffer_desc {
        std::uint32_t struct_size = sizeof(buffer_desc);     ///< size of this structure as the CALLER compiled it
        std::uint64_t size = 0;                              ///< bytes
        buffer_usage usage = buffer_usage::storage_gpu_only; ///< what the caller does with it (see above)
        buffer_flags flags = no_buffer_flags;                ///< capabilities beyond the memory intent
        std::span<std::byte const> initial_bytes = {};       ///< contents to upload at creation; empty = allocate only
    };

    /// A 3D extent in texels. POD, passed by value.
    struct image_extent {
        std::uint32_t width = 0;
        std::uint32_t height = 1;
        std::uint32_t depth = 1;
    };

    /// Which role a resource is used in. The vocabulary is the plan's §6.4/§6.5 list, which is exactly
    /// the set of roles DX12 resource states and this renderer's barriers both name; Vulkan's catch-all
    /// layout `GENERAL` is deliberately NOT a member (it is a Vulkan spelling, not a use).
    enum class image_use : std::uint32_t {
        color_attachment = 0, ///< written as a render target (and read back as one)
        transfer_source = 1,  ///< read by a copy out of the image
    };

    /// One subresource of one image, tightly packed: what a copy reads.
    struct image_copy_region {
        image_extent extent = {}; ///< texels to copy; width, height and depth all non-zero
        std::uint32_t mip_level = 0;
        std::uint32_t base_array_layer = 0;
        std::uint32_t array_layer_count = 1;
        std::uint32_t offset_x = 0;
        std::uint32_t offset_y = 0;
        std::uint32_t offset_z = 0;
    };

    /// The format of an image, as far as the contract names it: the four 8-bit shapes a screen read-back
    /// can be unpacked from, plus `unknown` ("the backend cannot describe it"). Values are only ever
    /// APPENDED: the one decision the engine makes from a format is the BGRA/RGBA swizzle of a
    /// screenshot and whether it can be encoded at all.
    enum class image_format : std::uint32_t { unknown = 0,
                                              rgba8_unorm,
                                              rgba8_srgb,
                                              bgra8_unorm,
                                              bgra8_srgb };

    // ---- OWNERSHIP: WHAT `release()` IS, AND WHAT IT IS NOT ---------------------------------------
    //
    // EVERY handle a factory returns (`swapchain`, `buffer`, `image`, `sampler`, `shader`, `pipeline`,
    // `query`) is handed out WITH ONE REFERENCE, and the caller releases that reference by calling
    // `release()` EXACTLY ONCE. The call runs INSIDE the backend - that is the whole reason it is a
    // virtual instead of the caller reaching for `delete`: the allocator, the registry entry and the
    // reference count all stay on the side that created them, and the C ABI never has to hand out an
    // allocator.
    //
    // RELEASE IS NOT NECESSARILY DESTRUCTION, AND THAT IS WHY IT IS NOT CALLED `destroy()`. A backend
    // MAY SERVE THE SAME RESOURCE TO MORE THAN ONE CALLER - this one does it whenever two requests
    // carry identical content, by keeping a content-keyed (XXH3-128) registry and a reference count
    // behind these handles (vulkan/core/vma/vma.cppm's ownership note) - so `release()` drops ONE
    // reference, and the device object dies only when the LAST reference goes. Two consequences a
    // caller has to live with:
    //
    //   - THE CALL MAY FREE NOTHING. A caller that needs the memory back must not infer it from this
    //     call; the backend's own bookkeeping decides (`vma_allocator::log_statistics()`, and the
    //     registry sweep in its `destroy()`).
    //   - THE HANDLE IS DEAD AFTERWARDS EVEN IF THE OBJECT LIVES. Touching the pointer again is a
    //     caller bug: the caller no longer holds a reference, so another `release()` would drop
    //     somebody else's.
    //
    // HOW A SECOND REFERENCE IS TAKEN: BY ASKING A FACTORY AGAIN. THE FACTORY IS THE RETAIN - a
    // `create_image()` whose content and parameters match a live object answers with THAT object and a
    // NEW reference, and the caller's single `release()` balances it. There is deliberately NO
    // `retain()` virtual: a raw "add a reference to this handle" call would let a caller manufacture
    // references for handles it never received, which is exactly the over-release path the backend
    // removed when it made its free/retain private and handed out injectable owners instead
    // (vma_allocator's ownership comment).
    //
    // A BORROWED VIEW IS NOT AN OWNED HANDLE, and this is the other half of the ownership model.
    // `api_core::frame_image()`, `api_core::frame_readback_buffer()` and `api_core::begin_commands()`
    // answer with objects the BACKEND owns and lends for a window (a frame, or until the next
    // acquire). Those answer `release()` with a ONE-TIME NAMED LOG and release nothing - the
    // alternative, a silent no-op, would hide a caller that thinks it holds a reference it does not.
    // `command_list` therefore carries no `release()` at all: it is only ever a borrowed view.
    //
    // WHY A VIRTUAL AND NOT JUST THE VIRTUAL DESTRUCTOR: `delete p` through these bases is already a
    // vtable dispatch into the backend, so it *works* (measured on `core`: the engine references no
    // destructor symbol for it at all). What `delete` cannot express is RELEASE WITHOUT DESTRUCTION,
    // and what the C ABI cannot express is a `delete` at all. `release()` is the entry that both can.
    // ----------------------------------------------------------------------------------------------

    /// A buffer, owned by the backend and released by the caller through `release()`.
    struct buffer {
        virtual ~buffer() noexcept = default;

        /// RELEASE THE CALLER'S ONE REFERENCE. Runs inside the backend, whatever the compiler's `delete`
        /// would have done, and **may free nothing**: the resource can be shared/reused, so the object
        /// dies only with its last reference (see the ownership note above). Do not touch the handle
        /// afterwards. A borrowed view answers with a one-time log and releases nothing.
        virtual void release() noexcept = 0;

        [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
        /// The bytes of a HOST-VISIBLE buffer, as the caller may read them; EMPTY when this buffer cannot
        /// be mapped (a device-local one). Read only after the frame that wrote it has completed: the
        /// contract carries no fence here, so the caller's own pacing (wait_idle) is the ordering.
        [[nodiscard]] virtual std::span<std::byte> mapped() noexcept = 0;
    };

    /// An image, owned by the backend and released by the caller through `release()`.
    struct image {
        virtual ~image() noexcept = default;

        /// see `buffer::release()`: drops one reference inside the backend; the object may survive
        /// because a backend is allowed to serve identical content from one shared, content-keyed entry.
        virtual void release() noexcept = 0;

        [[nodiscard]] virtual image_extent extent() const noexcept = 0;
        [[nodiscard]] virtual image_format format() const noexcept = 0;
    };

    /// The descriptors of the remaining factories. Opaque until S1 (see the banner).
    struct image_desc;
    struct sampler_desc;
    struct shader_desc;
    struct pipeline_desc;
    struct swapchain_desc;
    struct query_desc;

    /// The remaining tier-1 objects: samplers, shaders, pipelines, swapchains, queries
    /// and command lists.
    ///
    /// Each one is a polymorphic handle and not much more yet: the operations the engine
    /// performs on them (bind, draw, dispatch, barrier, present) are exactly the S1
    /// design surface, held back by §6.4's "explicit barrier first, then shadow mode"
    /// decision. What they carry today is ownership: destroying one through this base
    /// has to reach the backend's destructor, which is the property the boundary rests
    /// on.
    struct sampler {
        virtual ~sampler() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

    struct shader {
        virtual ~shader() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

    struct pipeline {
        virtual ~pipeline() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

    struct swapchain {
        virtual ~swapchain() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

    struct query {
        virtual ~query() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

    /// The OWNER SPELLING of a contract handle: one reference, released through the contract.
    ///
    /// `object_manager<T>` is RAII for the handles above and nothing else - its destructor calls
    /// `release()`, so a scope that holds a created resource drops its reference on every path out,
    /// including the early returns. It is move-only and null-on-move, so the ownership transfer is
    /// visible in the type rather than in a comment.
    ///
    /// IT OWNS A REFERENCE, NOT NECESSARILY THE OBJECT. Two managers may point at the same object
    /// (that is what a factory answering with a shared, content-keyed resource produces), and that is
    /// correct: each holds one reference, each releases one, and the object dies with the last. What a
    /// manager must never be given is a BORROWED view - `frame_image()`, `frame_readback_buffer()` and
    /// `begin_commands()` answer with objects the backend owns and lends, and wrapping one here drops a
    /// reference the caller never had (the borrowed views answer that with a one-time named log).
    /// `command_list` has no `release()`, so it cannot even be managed: the type system refuses it.
    ///
    /// WHY IT IS HERE RATHER THAN WRITTEN OUT IN THE ENGINE: `release()` is a contract virtual and this
    /// wrapper is a dozen inline lines, so both sides compiling it costs nothing and the engine gets one
    /// correct RAII spelling instead of one per call site. It is deliberately NOT `std::shared_ptr`: a
    /// shared owner would have to answer "which of us destroys it", which is the reference-count
    /// question the backend answers behind the contract, and the shared_ptr control block would not
    /// survive the boundary anyway. Nor `std::unique_ptr`: that spells the release as `delete`, the one
    /// spelling that cannot cross the C ABI and cannot express release-without-destruction.
    ///
    /// THE NAME IS PENDING (this is the "template rhi_object_manager, name TBD" of the plan): the
    /// candidates were `object_manager`, `object_ptr` (mirrors `unique_ptr`) and `owned`. Renaming it is
    /// a find-and-replace in this partition plus its call sites, so the choice is cheap to make later.
    template <typename object>
    class object_manager {
    public:
        object_manager() noexcept = default;

        /// take the reference @p owned carries (null is allowed: a factory that refused hands back null)
        explicit object_manager(object* const owned) noexcept
            : owned_{owned} {
        }

        object_manager(object_manager&& other) noexcept
            : owned_{other.owned_} {
            other.owned_ = nullptr;
        }

        object_manager& operator=(object_manager&& other) noexcept {
            if (this != &other) {
                this->reset();
                this->owned_ = other.owned_;
                other.owned_ = nullptr;
            }
            return *this;
        }

        object_manager(object_manager const&) = delete;
        object_manager& operator=(object_manager const&) = delete;

        ~object_manager() noexcept {
            this->reset();
        }

        /// RELEASE THE REFERENCE, ONCE (idempotent): calls the handle's `release()` when it still holds
        /// one. **The object may outlive this call** - release is not destruction when the resource is
        /// shared (see the ownership note above).
        void reset() noexcept {
            if (this->owned_ != nullptr) {
                this->owned_->release();
                this->owned_ = nullptr;
            }
        }

        /// give up the reference WITHOUT releasing it (the caller takes over the `release()`)
        [[nodiscard]] object* release() noexcept {
            object* const released = this->owned_;
            this->owned_ = nullptr;
            return released;
        }

        void swap(object_manager& other) noexcept {
            object* const temporary = this->owned_;
            this->owned_ = other.owned_;
            other.owned_ = temporary;
        }

        [[nodiscard]] object* get() const noexcept {
            return this->owned_;
        }

        [[nodiscard]] object* operator->() const noexcept {
            return this->owned_;
        }

        [[nodiscard]] object& operator*() const noexcept {
            return *this->owned_;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return this->owned_ != nullptr;
        }

    private:
        object* owned_ = nullptr;
    };

    struct command_list {
        virtual ~command_list() noexcept = default;

        /// Declare that `resource` moves from one role to the other, and let the backend record what that
        /// needs. THE PAIR, not a single use, is deliberate: this is plan §6.4 option (a) ("explicit
        /// barrier") spelled in option (c)'s vocabulary - the caller knows what it just did (it recorded
        /// it), the backend knows what the pair costs. A single-use form cannot be derived here: the
        /// backend does not record the pass that wrote the image yet (the engine still records its frame
        /// through `vulkan_escape`), so it cannot know the "from".
        ///
        /// THE CALLER OWNS THE CORRECTNESS: a missing pair, or a reversed one, is a WRONG barrier - the
        /// one failure this contract cannot catch for you. The shadow gate compares the barriers this
        /// produces, field by field, against the recipes the renderer ships with.
        ///
        /// IT ANSWERS, IT DOES NOT DROP SILENTLY: a barrier that was not recorded leaves the image in a
        /// state nobody declared, which is exactly the validation failure this surface was built to avoid
        /// (task-148's). So the answer is an `error` and the caller has to look at it (`[[nodiscard]]`).
        /// `not_ready` = no frame is being recorded, or the list is not this frame's (the same window
        /// `begin_commands()` answers in); `unsupported` = a role pair this backend cannot spell;
        /// `invalid_argument` = an image this backend did not hand out.
        [[nodiscard]] virtual error use(image const& resource, image_use from, image_use to) noexcept = 0;

        /// Record a copy of `region` of `source` into `destination`, at the source's CURRENT layout (this
        /// renderer keeps every image in GENERAL - docs/unified_image_layouts.md).
        ///
        /// The destination is a DEVICE buffer, not host memory: "record it into this frame" and "read it
        /// on the host" are two moments, and the second one is `buffer::mapped()` once the frame lands.
        [[nodiscard]] virtual error copy_image_to_buffer(buffer& destination, image const& source, image_copy_region const& region) noexcept = 0;
    };

    /// What starting a frame hands back.
    ///
    /// §3.3 fixes the name and the fact that it is a value, not a handle; the fields the
    /// frame-in-flight ring and the timeline machinery need are §6.5's, so this is the
    /// smallest version that is still worth returning.
    struct submit_info {
        std::uint32_t frame_index = 0; ///< index in the frame-in-flight ring
        std::uint32_t image_index = 0; ///< the swapchain image this frame draws into
    };

    /// The backend's context and its factories - today's `vulkan::core`, behind the
    /// boundary.
    ///
    /// One object per backend instance, created by `deren_make_api_core()` and owned by
    /// the engine's `std::shared_ptr`. It is deliberately allowed to grow (it is the one
    /// interface the plan lets be "big"), and deliberately not allowed to learn engine
    /// concepts (§4.1 item 2).
    struct api_core {
        virtual ~api_core() noexcept = default;

        /// tier-2: the abilities this backend SERVES THROUGH THIS CONTRACT, as `extension_kind`
        /// bits. Queried once at startup, then compared against what the passes need (§3.6).
        ///
        /// A set bit is a promise about SERVICE, not about the device: `query_extension(kind)` must
        /// answer with an object that can carry out every operation the ability declares, on objects
        /// this backend can actually produce. A bit set with a null `query_extension()`, or an
        /// ability whose operands cannot be produced at all, is a backend BUG and fails the
        /// consistency gate - a device-level fact ("this device is 1.2, so bufferDeviceAddress
        /// exists") is not an ability until there is a `buffer` to ask about.
        [[nodiscard]] virtual ability_bits abilities() const noexcept = 0;

        /// tier-2: the ability itself, or `nullptr` when it was not announced. The ONLY
        /// entry point to abilities, which is what keeps this vtable stable when one is
        /// added. The caller `static_cast`s the result it asked for (no RTTI).
        ///
        /// The invariant is two-way: announced => an object whose `kind()` matches, and not
        /// announced => `nullptr`.
        [[nodiscard]] virtual extension* query_extension(extension_kind kind) noexcept = 0;

        // ---- factories ---------------------------------------------------------
        // A factory returns nullptr when it cannot honour the descriptor: the plan has
        // no throwing path across the boundary (§4.2), and the caller is expected to
        // have checked `abilities()` before asking for something optional (§3.6).
        [[nodiscard]] virtual swapchain* create_swapchain(swapchain_desc const& desc) = 0;
        [[nodiscard]] virtual buffer* create_buffer(buffer_desc const& desc) = 0;
        [[nodiscard]] virtual image* create_image(image_desc const& desc) = 0;
        [[nodiscard]] virtual sampler* create_sampler(sampler_desc const& desc) = 0;
        [[nodiscard]] virtual shader* create_shader(shader_desc const& desc) = 0;
        [[nodiscard]] virtual pipeline* create_pipeline(pipeline_desc const& desc) = 0;
        [[nodiscard]] virtual query* create_query(query_desc const& desc) = 0;

        // ---- frame -------------------------------------------------------------
        /// The RECORDING VIEW of the frame in flight, or nullptr when no frame is being recorded.
        ///
        /// ONE per frame, wrapping the frame's PRIMARY command buffer (secondary buffers stay the
        /// backend's). OWNERSHIP: the backend's - the caller borrows it, never deletes it, and must not
        /// touch it after the frame has been submitted. The backend may hand back the same object every
        /// frame. LIFETIME: from the moment the frame starts recording until that frame is submitted.
        /// THREADING: single-threaded; recording is the primary thread's (the repository's rule).
        ///
        /// The NAME is the plan's and is kept; the verb is not literal yet - the frame's own
        /// begin/end/submit still belong to the engine (only the engine knows the present recipe), so
        /// this call starts nothing. It hands out the list the engine's frame is recording into.
        [[nodiscard]] virtual command_list* begin_commands() = 0;

        /// The image the LAST acquire returned, as a BORROWED view - or nullptr BEFORE THE FIRST ONE: a
        /// backend that has never acquired has no "last" image to name, and the answer is not image 0
        /// (which is a real image, just not this frame's).
        /// VALID UNTIL THE NEXT ACQUIRE - one step longer than the frame it belongs to, and deliberately so.
        ///
        /// WHY NOT "until that frame is submitted": the frame-domain READ of what a frame wrote cannot
        /// happen INSIDE the frame. The image is a swapchain image, and the submit that hands it to the
        /// presentation engine closes the recording window; the read stage still needs the EXTENT and the
        /// FORMAT of the frame it captured, and this is the object that names them. MEASURED: answering
        /// frame-scoped here made the read-back report "no captured frame" on every capturing scene (14/14
        /// in `check_render.ps1 -Full`), because the read runs after `submit_and_present()`.
        ///
        /// THE RECORDING VERBS KEEP THE SHORTER WINDOW AND THAT IS NOT A CONTRADICTION: `use()` and
        /// `copy_image_to_buffer()` record INTO a frame, so with no frame being recorded they answer
        /// `error::not_ready` - and `begin_commands()` answers nullptr, the list is not even handed out. A
        /// getter that has to survive the submit and a recorder that must not are two lifetimes on purpose.
        ///
        /// Frame-scoped naming is also why this exists instead of a factory: the factories still answer
        /// nullptr and their descriptors have no definition, so no `rhi::image` handle can come from one yet.
        [[nodiscard]] virtual image* frame_image() noexcept = 0;

        /// The backend's host-visible read-back slot, as a BORROWED view: at least
        /// `frame_image()->extent()` texels wide in the frame's format. Valid until the next call, and it
        /// is the destination `copy_image_to_buffer()` writes into.
        [[nodiscard]] virtual buffer* frame_readback_buffer() noexcept = 0;

        /// Open a frame: the backend advances its frame-in-flight state and says which
        /// ring slot and which swapchain image this frame may use.
        [[nodiscard]] virtual submit_info frame_begin() = 0;

        /// Hand the recorded frame to the presentation engine.
        virtual void present() = 0;

        /// Block until nothing is in flight.
        virtual void wait_idle() = 0;
    };

} // namespace deren::promise::rhi
