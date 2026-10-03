// ============================================================================
// module: deren.vulkan.acceleration_structure
// module version: 0.4.1  (independent of the app version in CMakeLists project(VERSION))
//
// Ray-tracing acceleration structures: the bottom level structures of the
// scene's shadow casters, built from the geometry buffers the raster passes
// already hold (no copy, no re-upload), and the host-side bookkeeping a later
// top level structure and its instance table are built on.
//
// Depends on deren.vulkan.core (device, queue, VMA).
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

export module deren.vulkan.acceleration_structure;

import deren.promise.rhi; // the contract's buffer handle + object_manager: the storage/scratch owners
export import deren.vstd;
export import deren.vulkan.core;

/**
 * @file vulkan/acceleration_structure/acceleration_structure.cppm
 * @defgroup vulkan_acceleration_structure Ray-Tracing Acceleration Structures
 * @brief Bottom level acceleration structures for the renderer's own geometry buffers.
 *
 * The engine's geometry already lives in device-local vertex and index buffers, and an acceleration
 * structure build reads exactly those - so a BLAS is a *view* of the renderer's geometry rather than a
 * second copy of it. That is the whole design: `geometry_source` carries two DEVICE ADDRESSES and a
 * triangle count, and nothing in this module knows what a primitive, a material or a draw call is.
 *
 * Two things are deliberately not here yet, and both are additive rather than structural changes:
 *  - COMPACTION. `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR` plus a size query and a
 *    copy typically halves the memory, at the price of a second command and a query reset; measuring
 *    the uncompacted cost first (Sponza: 17.8 MiB for 262k triangles) is what makes that decision
 *    reviewable rather than assumed. It is also a BOTTOM level concern here: the top level is rebuilt
 *    every frame, and a compacted structure is rebuilt in place rather than re-compacted.
 *  - the shading a ray-traced hit needs. The instance table IS filled (see instance_record) and its first
 *    consumer is now the shadow's any-hit stage, which resolves a hit back to a triangle's vertices and its
 *    material through it (see shaders/rt_shadow.rahit).
 *
 * What a hit can and cannot be told, today, is worth stating where it is decided:
 *  - NO geometry is built OPAQUE any more, and that is a reversal: the flag DECLARES "no any-hit shader may be
 *    invoked", and while every geometry carried it - which is what this module shipped with, because an inline
 *    ray query has no any-hit stage and a MASK surface was therefore as solid as its bounding triangles - the
 *    ray-tracing shadow's any-hit stage could not have run at all. Whether a geometry is opaque is a property
 *    of its MATERIAL, which this module is not told (it is handed vertex and index addresses), so the flag is
 *    off for every geometry and the alphaMode MASK decision is taken per HIT instead
 *    (shaders/rt_shadow.rahit, measured against the raster shadow there). Measured on an NVIDIA RTX 4060
 *    (591.59.0.0), lifting the flag changes no image: three capture arms with the closest-hit stage silenced
 *    all produced the correct frame, differing by 0.01 whole-frame mean - the flag changes the BUILT structure
 *    and with it the traversal order, not the visibility.
 *  - a skinned or morphed mesh is built from its SOURCE vertex buffer, which holds the bind pose: the
 *    deformation happens in the vertex shader and never reaches this memory. Such a primitive casts
 *    its bind-pose shadow until a compute skinning pass exists to write deformed vertices somewhere a
 *    build can read.
 */
namespace deren::vulkan::acceleration_structure {
    /**
     * @ingroup vulkan_acceleration_structure
     * @brief the usage bits a buffer must carry to be an acceleration-structure build input
     * @note the second bit needs VK_KHR_acceleration_structure, so this must only be OR'd into a
     *       buffer's usage when the device has it (the contract's spelling is
     *       `rhi::buffer_flag::acceleration_structure_input`, which core maps onto it). The first bit needs
     *       only the core 1.2 bufferDeviceAddress feature, which this engine enables by policy.
     * @note NO LONGER THE WAY THE RENDERER ASKS FOR THEM: both halves now pass the contract's
     *       `rhi::buffer_flag::device_address | acceleration_structure_input` to `create_buffer()` and the
     *       backend does this OR. The value is kept because it is exported surface that already shipped
     *       (removing it is a separate decision), not because a call site still reads it.
     */
    export constexpr VkBufferUsageFlags build_input_usage =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    /**
     * @ingroup vulkan_acceleration_structure
     * @brief one indexed triangle geometry to build a bottom level structure from
     * @note the addresses come from `vkGetBufferDeviceAddress` on the renderer's own vertex and index
     *       buffers; `index_address` may be 0 for a non-indexed geometry, which the build then reads as
     *       a flat vertex list
     */
    export struct geometry_source {
        VkDeviceAddress vertex_address = 0; // first vertex, already offset into the buffer
        uint32_t vertex_stride = 0;         // bytes per vertex (the engine's interleaved layout)
        uint32_t vertex_count = 0;
        VkDeviceAddress index_address = 0; // first index, already offset into the buffer
        VkIndexType index_type = VK_INDEX_TYPE_UINT32;
        uint32_t index_count = 0; // triangles = index_count / 3
        /**
         * THE OPACITY MICROMAP this geometry consults, when it has one: the handle, the PER-TRIANGLE index buffer
         * the traversal reads to find its micromap triangle, and the usage record the micromap was built with (the
         * attachment repeats it, so it is carried here rather than looked up).
         *
         * A micromap makes a micro-triangle's opacity the traversal's business instead of a shader's: a triangle
         * whose micro-triangles are opaque is committed without any-hit work, a transparent one is skipped, and an
         * UNKNOWN one invokes the any-hit shader - which is what makes an all-unknown micromap a no-op and a
         * decisive test at the same time.
         *
         * @note a null handle means no micromap, which is also the state of every geometry on a device without
         *       VK_EXT_opacity_micromap: nothing changes for it.
         */
        VkMicromapEXT opacity_micromap = VK_NULL_HANDLE;
        VkDeviceAddress opacity_index_address = 0;
        VkDeviceSize opacity_index_stride = 0;
        VkIndexType opacity_index_type = VK_INDEX_TYPE_UINT32;
        VkMicromapUsageEXT opacity_usage = {};
    };

    /**
     * @ingroup vulkan_acceleration_structure
     * @brief what a build cost, for the startup log
     * @note `build_ms` is HOST time inside record_build() - creating buffers and recording the command
     *       - which is what a load-time step can be judged by. The GPU time of the build itself is the
     *       frame's, and belongs in the pass timings like any other pass.
     */
    export struct build_stats {
        uint32_t geometry_count = 0;
        uint64_t triangle_count = 0;
        uint64_t structure_bytes = 0;
        uint64_t scratch_bytes = 0; // the scratch BUFFER, i.e. the aligned per-geometry ranges in it
        double build_ms = 0.0;
    };

    /**
     * @ingroup vulkan_acceleration_structure
     * @brief one entry of the instance table: what a shader needs to resolve a hit back to a surface
     * @note filled now - the build already walks the same instance list - even though the first ray
     *       that lands (a shadow ray) only asks "did anything block me": shading at a hit needs the
     *       triangle's vertex data and the material, and reconstructing that list later would mean
     *       walking the scene a second time for information this pass has in hand.
     * @note the two addresses are the BUFFER's device address (the base), not an offset into it, and
     *       they are the same value the bottom level build feeds the geometry - which is what makes a
     *       shader's `index_buffer[3 * triangle]` fetch the very triangle the ray hit. A buffer has an
     *       address only because the primitive upload sets SHADER_DEVICE_ADDRESS_BIT whenever the
     *       device supports it (see acceleration_structure::build_input_usage), so a shader reads them
     *       through a buffer reference and needs no descriptor for them.
     * @note `model` is the object -> world matrix the raster passes draw with, for the one thing a hit
     *       cannot get from the ray: the interpolated vertex NORMAL is in object space, and turning it
     *       into the shading normal is `normalize(mat3(model) * n)` - exactly what pbr.vert does. The
     *       hit's world POSITION does not need it: that is the ray's origin plus t times its direction.
     * @note `index_type` is a VkIndexType: a shader reads 16-bit indices by loading a 32-bit word and
     *       taking the half its index falls in, which avoids depending on 16-bit storage access.
     * @note std430 layout as the shader declares it: two 8-byte addresses, a 16-byte-aligned mat4, then
     *       four 4-byte fields = 96 bytes, no padding. The static_assert below is what keeps a field
     *       added here from silently shifting every lane after it in the shader's copy of this struct.
     */
    export struct instance_record {
        VkDeviceAddress vertex_address = 0; // the vertex buffer's device address (base)
        VkDeviceAddress index_address = 0;  // the index buffer's device address (base)
        glm::mat4 model = glm::mat4(1.0f);  // object -> world, for the vertex normal
        uint32_t vertex_stride = 0;
        uint32_t index_type = 0;     // VkIndexType, for the shader that indexes with it
        uint32_t material_index = 0; // into the scene's material table (set 0 binding 5)
        uint32_t primitive_index = 0;
    };
    static_assert(sizeof(instance_record) == 96, "the shader's copy of instance_record must match this layout");

    /**
     * @ingroup vulkan_acceleration_structure
     * @brief one instance of the top level structure: which bottom level, where it sits, what it is
     */
    export struct instance_source {
        glm::mat4 transform = glm::mat4(1.0f); // the world matrix the raster passes draw this instance with
        uint32_t blas_index = 0;               // index into the bottom_level_structures it was added to
        instance_record record = {};           // what the shader sees through instanceCustomIndex
    };

    /**
     * @ingroup vulkan_acceleration_structure
     * @brief the scene's bottom level structures, built in one command
     * @note the builds are batched into ONE `vkCmdBuildAccelerationStructuresKHR` call, which is why the
     *       scratch space is one buffer with a per-geometry aligned range. THE REASON IS NOT THAT THE
     *       BUILDS RUN IN ORDER: Vulkan gives no ordering between the builds of a single call, and a
     *       build that reads another's output needs its own synchronization. The per-geometry ranges are
     *       what remove the QUESTION instead of relying on an answer - no two builds share scratch, so no
     *       ordering is needed, and the batching is purely a CPU-side win.
     */
    export class bottom_level_structures {
        struct entry {
            VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
            // the memory the structure lives in: the contract's owner holds ONE reference and releases it
            // when the entry dies (release is not destruction - the backend may share the memory)
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> storage = {};
            VkDeviceSize scratch_offset = 0; // into the shared scratch buffer, already aligned
            VkDeviceSize scratch_size = 0;
            bool refittable = false;                                         // built with ALLOW_UPDATE, so record_update may refit it
            VkAccelerationStructureBuildRangeInfoKHR range = {};             // primitiveCount etc, for the build
            std::vector<VkAccelerationStructureGeometryKHR> geometries = {}; // one per entry, kept alive
        };

        /// Deliberately NOT called `vk`: add() and record_build() bind a local `core& vk`, and that
        /// local would hide a member of the same name - MSVC /W4 reports C4458, an error under /WX
        /// (clang does not warn: -Wshadow is not enabled there).
        core* gpu = nullptr; // non-const: VMA's detail lookups and buffer creation are not const
        std::vector<entry> entries = {};
        /// The extension entry points, resolved per device in the constructor. Declared incomplete here
        /// and defined in the .cpp, because a function pointer table is implementation detail - and a
        /// unique_ptr so the header does not have to name the four PFN types either.
        ///
        /// NO INITIALIZER HERE, AND THAT IS WHAT MAKES THE INCOMPLETE TYPE LEGAL: `entry_points` is only
        /// forward-declared at this point, so anything that constructs or destroys a unique_ptr<entry_points>
        /// IN THIS HEADER needs the complete type. libc++ (the clang64 build) tolerates the `= {}` this used to
        /// carry; libstdc++ does not - GCC instantiates `~unique_ptr<entry_points>` at the default member
        /// initializer itself and stops on `default_delete`'s `static_assert(sizeof(_Tp)>0)` (measured, both
        /// classes in this file). Default-initializing the member in the CONSTRUCTOR instead costs the same
        /// (the members are default-constructed before the body runs either way) and puts the instantiation in
        /// the .cpp, where `entry_points` is defined - which is the same reason the destructor is declared above
        /// and defined there.
        struct entry_points;
        std::unique_ptr<entry_points> functions;
        deren::promise::rhi::object_manager<deren::promise::rhi::buffer> scratch = {};
        VkDeviceAddress scratch_address = 0;
        VkDeviceSize scratch_size = 0;
        build_stats stats = {};
        /// the build infos handed to the command, assembled once per record_build (they point into
        /// `entries`, so they cannot outlive a resize of it)
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> build_infos = {};
        std::vector<VkAccelerationStructureBuildRangeInfoKHR const*> range_ptrs = {};
        /// the same two, assembled per record_update: separate from the build's so a refit cannot disturb
        /// what the build recorded (it keeps its infos for exactly this reason - an update reuses them)
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> update_infos = {};
        std::vector<VkAccelerationStructureBuildRangeInfoKHR const*> update_range_ptrs = {};
        /**
         * THE OPACITY MICROMAP ATTACHMENTS, one per geometry that has one.
         *
         * @note A DEQUE, and that is the whole point of the type: `VkAccelerationStructureGeometryKHR::pNext`
         *       points at these structs from add() until the build is recorded, so their addresses must survive
         *       every later insertion - which a vector does not promise and a deque does. The usage record sits
         *       beside its attachment in the same element because the attachment points at it.
         */
        struct micromap_attachment {
            VkAccelerationStructureTrianglesOpacityMicromapEXT attachment = {};
            VkMicromapUsageEXT usage = {};
        };
        std::deque<micromap_attachment> micromap_geometries = {};

    public:
        explicit bottom_level_structures(core& device);
        bottom_level_structures(bottom_level_structures const&) = delete;
        bottom_level_structures& operator=(bottom_level_structures const&) = delete;
        bottom_level_structures(bottom_level_structures&&) = delete;
        bottom_level_structures& operator=(bottom_level_structures&&) = delete;
        ~bottom_level_structures();

        /**
         * @ingroup vulkan_acceleration_structure
         * @brief create the structure and its storage for one geometry
         * @param source the geometry's addresses and triangle count
         * @param refittable build with ALLOW_UPDATE so record_update() may refit it every frame - for
         *        geometry whose BYTES change while its addresses and counts do not (a skinned mesh's
         *        vertices, written by shaders/compute_skin.slang). It costs traversal efficiency, which is
         *        why it is per geometry rather than a flag on the whole structure set.
         * @return the index this geometry got, or an error message on failure
         * @note this is HOST work (a size query plus an allocation); the GPU build happens in
         *       record_build(). A source with no triangles is skipped and returns the index it WOULD
         *       have had, so the caller's indices stay aligned with its own array.
         */
        std::expected<uint32_t, std::string> add(geometry_source const& source, bool refittable = false);

        /**
         * @ingroup vulkan_acceleration_structure
         * @brief allocate the scratch and record every build into @p command_buffer
         * @return success, or an error message
         * @note must be recorded OUTSIDE a rendering instance (it is a transfer/compute-class command)
         * @note call once: a second call would rebuild into the same structures with no scratch space
         *       left (the buffer is sized by the first call's requirements)
         */
        std::expected<void, std::string> record_build(VkCommandBuffer command_buffer);

        /**
         * @ingroup vulkan_acceleration_structure
         * @brief REFIT the listed structures in place, because the bytes behind their geometry changed
         * @param command_buffer where to record
         * @param indices the geometry indices to refit (only the ones added with refittable = true)
         * @return success, or an error message
         * @note a refit is legal exactly when the geometry's ADDRESSES AND COUNTS are unchanged and only
         *       the memory they point at has been rewritten - which is the zero-copy shape a compute
         *       skinning pass produces (see shaders/compute_skin.slang). It reuses the scratch the build
         *       allocated, so it is cheap: no size query, no allocation, no rebuild of the structure.
         * @note the caller must have made the writes visible to the acceleration structure build stage
         *       first (a command-level barrier), or the refit reads whatever was there before
         */
        std::expected<void, std::string> record_update(VkCommandBuffer command_buffer, std::span<uint32_t const> indices);

        /** @brief how many structures were added */
        [[nodiscard]] uint32_t size() const noexcept {
            return static_cast<uint32_t>(this->entries.size());
        }

        /** @brief the structure at @p index, or VK_NULL_HANDLE when it was skipped */
        [[nodiscard]] VkAccelerationStructureKHR handle(uint32_t index) const noexcept {
            return index < this->entries.size() ? this->entries[index].handle : VK_NULL_HANDLE;
        }

        /** @brief what the last build cost (see build_stats) */
        [[nodiscard]] build_stats const& last_stats() const noexcept {
            return this->stats;
        }
    };

    /**
     * @ingroup vulkan_acceleration_structure
     * @brief the scene's top level structure, rebuilt from the instance list once per frame
     *
     * @details the instances are host-visible arrays the caller fills through add(), and the structure
     *          itself is built into a command buffer. One set of resources PER FRAME SLOT, because with
     *          more than one frame in flight a single buffer would be rewritten by the frame being
     *          recorded while the previous one is still reading it - the same per-slot rule the
     *          engine's camera and material buffers follow.
     *
     * The build is MODE_BUILD every frame rather than MODE_UPDATE: an update can only change
     * transforms, requires the same instance count and ALLOW_UPDATE on the original build, and the
     * instance list here is culled per frame - so the cheaper update path is exactly the one that would
     * need the most bookkeeping to stay legal. It is PREFER_FAST_BUILD rather than PREFER_FAST_TRACE
     * for the same reason: this structure is built 60+ times a second and traversed a few times per
     * pixel, while the bottom levels are built once and traversed constantly.
     */
    export class top_level_structure {
        struct slot {
            // The two host-visible arrays the caller fills through add(). They are contract owners now,
            // and the native handle / the mapping are asked OF THE HANDLE where they are used
            // (vulkan_escape::native_buffer() / buffer::mapped()) instead of being cached beside it.
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> instances = {}; // host-visible VkAccelerationStructureInstanceKHR[capacity]
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> records = {};   // host-visible instance_record[capacity] (the instance table)
            uint32_t capacity = 0;                                                           // instances the buffers above can hold
            uint32_t count = 0;                                                              // instances added this frame
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> storage = {};   // the structure's own memory, sized for `capacity`
            VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
            VkDeviceSize structure_size = 0;                                               // the size it was CREATED with: what a heap address-range descriptor carries
            VkDeviceSize scratch_size = 0;                                                 // what the build of `count` instances needs
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> scratch = {}; // the build's scratch memory, kept once sized
        };

        /// Deliberately NOT called `vk`: add() and record_build() bind a local `core& vk`, and that
        /// local would hide a member of the same name - MSVC /W4 reports C4458, an error under /WX
        /// (clang does not warn: -Wshadow is not enabled there).
        core* gpu = nullptr; // non-const: VMA's detail lookups and buffer creation are not const
        /// The extension entry points, forward-declared and held by pointer for the same reason (and with the
        /// same NO-initializer rule) as `bottom_level_structures::functions` above - see the note there.
        struct entry_points;
        std::unique_ptr<entry_points> functions;
        std::vector<slot> slots = {};
        uint32_t current_slot = 0;
        build_stats stats = {};

    public:
        explicit top_level_structure(core& device, uint32_t frame_slot_count);
        top_level_structure(top_level_structure const&) = delete;
        top_level_structure& operator=(top_level_structure const&) = delete;
        top_level_structure(top_level_structure&&) = delete;
        top_level_structure& operator=(top_level_structure&&) = delete;
        ~top_level_structure();

        /**
         * @ingroup vulkan_acceleration_structure
         * @brief start a frame's instance list in @p frame_slot (dropping whatever it held)
         * @param frame_slot the slot the frame being recorded belongs to
         * @return success, or an error message when the slot's buffers cannot be sized
         * @note the slot is free to write because the runtime waits for it before recording (see
         *       runtime::pace_and_acquire), the same reason every other per-slot buffer is.
         */
        std::expected<void, std::string> begin(uint32_t frame_slot);

        /**
         * @ingroup vulkan_acceleration_structure
         * @brief append one instance to the current slot's list
         * @note @p source.blas_index must be an index of the bottom_level_structures the reference is
         *       taken from; a null handle there (a geometry that was skipped) skips the instance, so the
         *       caller's arrays stay aligned
         */
        std::expected<void, std::string> add(bottom_level_structures const& levels, instance_source const& source);

        /**
         * @ingroup vulkan_acceleration_structure
         * @brief record the build of the current slot's list into @p command_buffer
         * @param command_buffer a buffer being recorded outside a rendering instance
         * @return success, or an error message
         */
        std::expected<void, std::string> record_build(VkCommandBuffer command_buffer);

        /** @brief the structure the slot's frame must bind, or VK_NULL_HANDLE when it is empty */
        [[nodiscard]] VkAccelerationStructureKHR handle(uint32_t frame_slot) const noexcept {
            return frame_slot < this->slots.size() ? this->slots[frame_slot].handle : VK_NULL_HANDLE;
        }

        /**
         * @brief the size the slot's structure was created with
         * @return that size, or 0 when the slot does not exist
         * @note PUBLISHED FOR THE DESCRIPTOR HEAP: a heap acceleration-structure descriptor is an ADDRESS RANGE
         *       (VkResourceDescriptorDataEXT has no AS member - see docs/descriptor_heap_migration.md), and
         *       VkDeviceAddressRangeEXT must carry a REAL size - a lesson this renderer already paid for on the
         *       material table (VUID-VkDeviceAddressRangeKHR-address-11365). The size query at creation is the only
         *       place that number exists, so it is kept rather than re-derived by a caller that cannot know it.
         */
        [[nodiscard]] VkDeviceSize structure_size(uint32_t frame_slot) const noexcept {
            return frame_slot < this->slots.size() ? this->slots[frame_slot].structure_size : 0;
        }

        /**
         * @brief the size of the slot's instance table (binding 17): its capacity times the record's own size
         * @return that size, or 0 when the slot does not exist
         * @note PUBLISHED for the same reason structure_size is: the descriptor-set path writes this buffer with
         *       VK_WHOLE_SIZE (legal there), while a HEAP range must carry a real size
         *       (VUID-VkDeviceAddressRangeKHR-address-11365), and the capacity is this module's to know.
         */
        [[nodiscard]] VkDeviceSize instance_table_size(uint32_t frame_slot) const noexcept {
            return frame_slot < this->slots.size() ? static_cast<VkDeviceSize>(this->slots[frame_slot].capacity) * sizeof(instance_record) : 0;
        }

        /** @brief the slot's instance table (instance_record[count]); the shading-at-a-hit step binds it
         *  @note the slot holds a CONTRACT buffer now, so this asks the escape for the native handle
         *        (defined in the .cpp, where the escape helper lives) */
        [[nodiscard]] VkBuffer instance_table(uint32_t frame_slot) const noexcept;

        /**
         * @brief the slot's instance table as the CONTRACT buffer it is, or nullptr when the slot has no top
         *        level structure yet
         * @note ADDITIVE to `instance_table()` above rather than a replacement: that one answers the raw
         *       handle a caller that records raw Vulkan needs, and this one exists so a caller that needs the
         *       buffer's DEVICE ADDRESS can ask the backend for it (`rhi::device_address::buffer_address()`)
         *       instead of spelling `vkGetBufferDeviceAddress` on a handle it had to narrow itself.
         * @note A POINTER AND NOT A REFERENCE, because "no structure yet" has to stay expressible - it is
         *       exactly the state `instance_table()` answers VK_NULL_HANDLE for, and a reference cannot say it.
         */
        [[nodiscard]] deren::promise::rhi::buffer const* instance_table_buffer(uint32_t frame_slot) const noexcept;

        /** @brief how many instances the slot's last build held */
        [[nodiscard]] uint32_t instance_count(uint32_t frame_slot) const noexcept {
            return frame_slot < this->slots.size() ? this->slots[frame_slot].count : 0;
        }

        /** @brief what the last recorded build cost (geometry_count is the instance count) */
        [[nodiscard]] build_stats const& last_stats() const noexcept {
            return this->stats;
        }
    };
} // namespace deren::vulkan::acceleration_structure
