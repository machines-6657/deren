// ============================================================================
// module: deren.vulkan.ray_tracing
// module version: 0.1.1  (independent of the app version in CMakeLists project(VERSION))
//
// THE STRUCTURE PHASE: the acceleration structures every traced effect casts rays against, the map that says
// which caster each one was built from, and the COPIES the hit-shading path reads that geometry through (the
// MASK expansion and the skinned vertices).
//
// WHY IT IS A MODULE AND NOT PART OF vulkan.acceleration_structure: that module owns the GPU OBJECTS and says so
// in its own header - "nothing in this module knows what a primitive, a material or a draw call is" - and what is
// here is exactly that knowledge: which casters, which material is alphaMode MASK, which primitive is skinned,
// which buffer a hit must be read from. The two layers are `add`/`record_build` (there) and the POLICY plus the
// GATHER (here).
//
// WHY IT IS NOT A PASS, although it is one of the frame's phases: the structures are read by THREE consumers
// (the ray-traced shadow pass's binding), so by this project's ownership rule they belong to the SHARED owner rather than to any one pass -
// and the phase is recorded BEFORE any rendering instance opens, which no pass's stage can express. What the
// renderer keeps is the POLICY (the three knobs and the two predicates), the caster set, the ORDER of the phase,
// and the scene-set binding it publishes the handle through.
//
// Depends on deren.vulkan.acceleration_structure (the two structure classes), deren.vulkan.primitive (the casters it walks
// and the material table the MASK rule reads) and the two PASS modules whose requests it composes - the jobs
// themselves stay the renderer's for now and are driven through `bake_hooks`.
// ============================================================================
module;

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

export module deren.vulkan.ray_tracing;

import deren.promise.rhi; // the contract's buffer handle + object_manager: the copies this phase owns
import deren.vulkan.acceleration_structure;
import deren.vulkan.pass.compute_skin; // the skinning job's request, which this module composes
import deren.vulkan.pass.mask_bake;    // ... and the MASK bake's
import deren.utility;

/**
 * @file vulkan/ray_tracing/ray_tracing.cppm
 * @defgroup vulkan_ray_tracing Ray-Tracing Structures and Casters
 * @brief The structure phase: the acceleration structures every traced effect casts rays against, the map
 *        that says which caster each one was built from, and the copies the hit-shading path reads that
 *        geometry through (the MASK expansion and the skinned vertices).
 *
 * WHY IT IS A MODULE AND NOT PART OF vulkan.acceleration_structure: that module owns the GPU objects and
 * says so in its own header - "nothing in this module knows what a primitive, a material or a draw call
 * is" - and what is here is exactly that knowledge: which casters, which material is alphaMode MASK, which
 * primitive is skinned, which buffer a hit must be read from. The two layers are `add`/`record_build`
 * (there) and the POLICY plus the GATHER (here).
 *
 * WHY IT IS NOT A PASS, although it is one of the frame's phases: the structures are read by three
 * consumers (the ray-traced shadow pass among them), so by this project's ownership rule they belong to
 * the SHARED owner rather than to any one pass - and the phase is recorded BEFORE any rendering instance
 * opens, which no pass's stage can express. What the renderer keeps is the POLICY (the three knobs and the
 * two predicates), the caster set, the ORDER of the phase, and the scene block's slot it publishes the
 * handle through.
 */

export import deren.vstd;
export import deren.vulkan.core;
export import deren.vulkan.primitive;

export namespace deren::vulkan::ray_tracing {

    /**
     * @brief ONE CASTER WHOSE BOTTOM LEVEL WAS BUILT, with the addresses a hit's shading reads it through
     *
     * The `caster` pointer is what the two walks share: the per-frame instance list walks THIS list, so a caster
     * that was skipped during the build is skipped there too and the two cannot disagree about which geometry a
     * ray hit. The MASK and skin addresses ride along because a hit's shading must read the copy the structure
     * was built from rather than the primitive's own buffer.
     */
    struct caster_level {
        primitive const* caster = nullptr;
        uint32_t blas_index = 0;
        uint32_t mask_stride = 0;
        VkDeviceAddress mask_vertex_address = 0;
        VkDeviceAddress skin_source_address = 0;
        VkDeviceAddress skin_destination_address = 0;
        uint32_t skin_source_stride = 0;
        uint32_t skin_destination_stride = 0; // 32 (position, normal, uv)
        uint32_t skin_vertex_count = 0;
        uint32_t skin_base = 0;
        /// index into `micromaps()`, or `micromap_none`: which opacity micromap this caster's geometry consults
        static constexpr uint32_t micromap_none = 0xFFFFFFFFu;
        uint32_t micromap_index = micromap_none;
    };

    /**
     * @brief THE TWO JOBS THIS PHASE DRIVES, handed in rather than owned
     *
     * The one-shot MASK bake and the per-frame compute skinning are GPU-owning objects built from a
     * `pass_context`, and they are the renderer's members today: this module drives them through these hooks so
     * that it does not depend on the passes that implement them (and so that moving them in, when that slice
     * comes, changes one side only).
     */
    struct bake_hooks {
        void* owner = nullptr;
        /// whether the MASK bake can take a request (its pipeline exists)
        bool (*mask_ready)(void* owner) = nullptr;
        /// bake ONE caster's alphaMode MASK into an expanded vertex buffer (the job owns the pipeline and the set)
        void (*record_mask_bake)(void* owner, VkCommandBuffer command_buffer, pass::mask_bake_request const& request) = nullptr;
        /// whether the skinning job can take a request (its pipeline and sets exist)
        bool (*skin_ready)(void* owner) = nullptr;
        /**
         * @brief re-skin this frame's skinned casters, and report whether any dispatch was recorded
         *
         * The casters are the map this phase built, so the caller builds its request list from the entries whose
         * `skin_destination_address` is non-zero - the same walk, and the same "which casters are skinned"
         * answer, the build itself used.
         */
        bool (*record_skin)(void* owner, VkCommandBuffer command_buffer, std::span<caster_level const> casters) = nullptr;
    };

    /**
     * @brief WHAT ONE `build` OR `update` NEEDS FROM THE RENDERER: data, knobs, and the two jobs
     *
     * The knobs travel with the call rather than living in the object because they are the RENDERER's policy (a
     * config key each, read every frame), and `materials` is a span rather than a pointer plus a count because
     * the MASK rule is the only reader and an out-of-range index is then a size check rather than arithmetic.
     */
    struct build_inputs {
        /// the frame's casters: the set the shadow pass draws, because a caster can sit off screen and still
        /// throw a shadow into the view (the visible set would be wrong)
        std::span<primitive const* const> casters = {};
        /// the material table the alphaMode MASK rule reads (`material_record::flags` bit 4)
        std::span<material_record const> materials = {};
        /// `[render] rt_mask_bake`
        bool mask_bake = false;
        /// `[render] rt_skin_bake`
        bool skin_bake = false;
        /// the two jobs (see bake_hooks)
        bake_hooks hooks = {};
    };

    /// @brief WHY the phase gave up: the renderer logs it and decides what the message means for its own knobs
    struct failure {
        std::string message = {};
        /// true when the caller should stop asking for the per-frame refit: a device that refused an update is
        /// not something to keep attempting against structures it will keep refusing
        bool disable_skin_bake = false;
    };

    /**
     * @brief THE STRUCTURES, THE CASTER MAP AND THE COPIES, as one value the renderer owns and drives
     *
     * BUILT ONCE, REFITTED AND RE-INSTANCED EVERY FRAME: the bottom levels are built from the geometry the
     * primitive uploads already hold (their device addresses), so nothing is copied except the two cases that
     * cannot be read directly - an alphaMode MASK material, whose holes have to be baked into an expanded vertex
     * list because an inline ray query has no any-hit stage to run the discard, and a skinned caster, whose
     * deformed vertices exist only in the vertex shader. The top level structure is rebuilt EVERY frame: the
     * instance list is culled per frame and a caster's world matrix can change, so it is frame data like any
     * other.
     */
    class structure_set {
    public:
        explicit structure_set(core& device_root) noexcept;
        structure_set(structure_set const&) = delete;
        structure_set& operator=(structure_set const&) = delete;

        /**
         * @brief build the bottom levels, once, from @p inputs (the frame that first wants the structures)
         * @return the reason it was refused, with the object left EMPTY - which is what makes the passes that
         *         read the structures gate themselves off rather than traverse a half-built set
         * @note a second call does nothing: a device that refused the build is not asked again, and a build that
         *       succeeded is not repeated (the geometry is the primitive's own memory)
         */
        [[nodiscard]] std::expected<void, failure> build(VkCommandBuffer command_buffer, build_inputs const& inputs);
        /**
         * @brief re-skin and REFIT the skinned structures, then rebuild this slot's instance list and structure
         *
         * THE SLOT IS AN ARGUMENT because the top level is per frame slot (with frames in flight, one buffer
         * would be rewritten by the frame being recorded while the previous one still reads it), and the caller
         * is what paces frames.
         */
        [[nodiscard]] std::expected<void, failure> update(VkCommandBuffer command_buffer, uint32_t frame_slot, build_inputs const& inputs);

        /// @brief whether a build was attempted, success or failure (there is no retry - see build)
        [[nodiscard]] bool attempted() const noexcept;
        /// @brief whether both structures exist, i.e. whether anything can be traversed
        [[nodiscard]] bool ready() const noexcept;
        /// @brief this slot's top level structure, or `VK_NULL_HANDLE` (the scene block's binding 16)
        [[nodiscard]] VkAccelerationStructureKHR handle(uint32_t frame_slot) const noexcept;
        /**
         * @brief the size this slot's top level structure was created with (see top_level_structure::structure_size)
         * @note forwarded rather than re-derived: the structure is `top_level`'s, and the descriptor heap writes
         *       it as an address RANGE whose size has to be real (docs/descriptor_heap_migration.md).
         */
        [[nodiscard]] VkDeviceSize structure_size(uint32_t frame_slot) const noexcept;
        /// @brief the size of this slot's instance table, for the heap's address-range descriptor (binding 17)
        [[nodiscard]] VkDeviceSize instance_table_size(uint32_t frame_slot) const noexcept;
        /// @brief this slot's instance table buffer, whose device ADDRESS the GI frame constants carry
        [[nodiscard]] VkBuffer instance_table(uint32_t frame_slot) const noexcept;
        /**
         * @brief this slot's instance table as the CONTRACT buffer it is, or nullptr when there is no top
         *        level structure for that slot yet
         * @note ADDITIVE to `instance_table()` above (the raw-handle form) and forwarded for the same reason
         *       it is: the buffer is `top_level`'s. This is the form a caller uses to reach the buffer's
         *       device ADDRESS through the contract's `device_address` ability instead of narrowing it and
         *       calling `vkGetBufferDeviceAddress` itself.
         */
        [[nodiscard]] deren::promise::rhi::buffer const* instance_table_buffer(uint32_t frame_slot) const noexcept;
        /// @brief the casters that were built, in the order they were added (see caster_level)
        [[nodiscard]] std::span<caster_level const> casters() const noexcept;
        /**
         * @brief destroy the micromaps this set owns
         * @note EXPLICIT, and it runs from the destructor as well as from abandon(): VkMicromapEXT is a child
         *       object of the device that nothing in this project wraps in RAII, and validation reports a
         *       surviving one at vkDestroyDevice (measured: it does, as soon as the micromaps exist).
         */
        ~structure_set();

    private:
    private:
        /// @brief the shared half of abandon() and the destructor: destroy every micromap, then drop them
        void release_micromaps() noexcept;

    public:
        /// @brief defined below, in the block that documents it: this declaration is only here so
        ///        caster_geometry (below, in a private section) can take a pointer to it
        struct micromap_resource;

    private:
    private:
        /// drop everything: the structures, the map and the copies they were built from (all four are one fact)
        void abandon() noexcept;
        [[nodiscard]] acceleration_structure::geometry_source caster_geometry(primitive const& caster,
                                                                              VkDeviceAddress source_vertex_address,
                                                                              VkDeviceAddress source_index_address,
                                                                              VkDeviceAddress mask_address,
                                                                              uint32_t mask_stride,
                                                                              VkDeviceAddress skin_address,
                                                                              uint32_t skin_stride,
                                                                              micromap_resource const* micromap) const noexcept;

        core* device = nullptr;
        std::optional<acceleration_structure::bottom_level_structures> bottom = {};
        /// named `top_level` rather than `top`: `build` keeps a local `auto& top`, and a member of that name
        /// would be hidden by it - MSVC /W4 reports C4458, which /WX makes an error
        std::optional<acceleration_structure::top_level_structure> top_level = {};
        /// named `caster_list` rather than `casters`: the accessor above is `casters()`, and dropping the
        /// underscore would declare the same name twice
        std::vector<caster_level> caster_list = {};
        /// the MASK expansions and the skinned vertex buffers: owned here for as long as the structures are, so
        /// the addresses inside `caster_list` stay valid. Contract owners now, so the GPU memory dies with
        /// this object's last reference rather than through the allocator's map.
        std::vector<deren::promise::rhi::object_manager<deren::promise::rhi::buffer>> mask_buffers = {};
        std::vector<deren::promise::rhi::object_manager<deren::promise::rhi::buffer>> skin_buffers = {};
        /// the bottom levels a per-frame refit touches, by index
        std::vector<uint32_t> skin_levels = {};
        /// named `build_attempted` rather than `attempted`: the query below is `attempted()`, and dropping
        /// the underscore would declare the same name twice
        bool build_attempted = false;
        bool top_logged = false;

    public:
        /**
         * @ingroup vulkan_ray_tracing
         * @brief ONE built opacity micromap and every buffer that describes it
         * @note the ownership of `micromap` is explicit rather than RAII: VkMicromapEXT has no wrapper in this
         *       project (it is not a buffer, an image or a pipeline), so `structure_set::abandon()` destroys it
         *       with vkDestroyMicromapEXT and the buffers below release their contract references with this
         *       struct.
         * @note the five buffers are the micromap's own memory (`storage`, created with MICROMAP_STORAGE), the
         *       packed opacity states (`data`), the per-triangle descriptor array (`triangles`), the build's
         *       scratch and the per-triangle index the GEOMETRY reads to find its micromap triangle (`indices` -
         *       the attachment's input, filled here so the two halves of the mechanism are created together).
         *       Each one is a move-only contract owner, which is what keeps the addresses below valid for as
         *       long as this struct lives.
         */
        struct micromap_resource {
            VkMicromapEXT micromap = VK_NULL_HANDLE;
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> storage = {};
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> data = {};
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> triangles = {};
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> indices = {};
            deren::promise::rhi::object_manager<deren::promise::rhi::buffer> scratch = {};
            VkMicromapUsageEXT usage = {}; // count / subdivisionLevel / format - the geometry attachment repeats it
            VkDeviceAddress data_address = 0;
            VkDeviceAddress triangles_address = 0;
            VkDeviceAddress scratch_address = 0;
            VkDeviceAddress indices_address = 0;
            VkDeviceSize triangle_array_stride = 0;
            VkDeviceSize index_stride = 0;
            uint32_t triangle_count = 0;
        };
        /// @brief the micromaps this set owns, one per alphaMode MASK caster (see build())
        [[nodiscard]] std::span<micromap_resource const> micromaps() const noexcept {
            return this->micromap_resources;
        }

    private:
        /// named `micromap_resources` rather than `micromaps`: the accessor above is `micromaps()`, and
        /// dropping the underscore would declare the same name twice
        std::vector<micromap_resource> micromap_resources = {};
    };

} // namespace deren::vulkan::ray_tracing
