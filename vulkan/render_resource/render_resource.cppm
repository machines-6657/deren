// module version: 0.13.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/render_resource/render_resource.cppm
 * @brief A pass's resources, described as DATA: what exists, and what each pass does with it.
 * @defgroup vulkan_render_resource Render Resource Descriptions
 *
 * WHY THIS MODULE EXISTS. In this renderer a pass's inputs and outputs used to BE descriptor sets - the
 * G-buffer set was the interface between the G-buffer pass, the lighting stage, the stochastic lighting chain
 * and the ray-traced shadow pass; the scene block was the substrate; each pass's private family was its own I/O.
 * That interface used to be written TWICE BY HAND and kept in agreement by discipline: the layout
 * in `vulkan/pipelines`, the descriptor writes in `vulkan/runtime`'s `ensure_*_descriptors()`. Both drifts
 * that pair can have are already in this project's history, and both were found by the validation layer
 * rather than by review: a pool sized for four descriptors per set while the layout asked for five, and a
 * binding whose type changed without its writer noticing. One declaration, from which both were generated,
 * is what removed the pair.
 *
 * WHAT IT IS NOT, deliberately rather than unfinished:
 *
 *  * NOT a capability surface. Nothing here can reach a resource: no `core&`, no `runtime&`, no handles. It
 *    is copyable `constexpr` data with predicates over it - the opposite of a context object handed to code
 *    that must BIND things, which is the kind of object that grows as needs appear. A declaration cannot
 *    grow a capability.
 *  * NOT an allocator. Every image, buffer, view and sampler stays where it is: `deren.vulkan.core` creates them,
 *    `deren.vulkan.runtime` owns the pipelines. The schema says a resource EXISTS and what its scope and lifetime
 *    are; it does not say who makes it or how.
 *  * NOT Vulkan. The vocabulary is this module's own enums, so the description layer is pure CPU like
 *    `deren.vulkan.math` and `gltf_loader` - which is what lets its invariants be tested in `ctest`, on a machine
 *    with no GPU. (The capture gate cannot run in CI at all: its references are tied to one machine's
 *    driver.) Mapping these enums onto `VkDescriptorType`/`VkShaderStageFlags` belongs to the generator,
 *    which imports Vulkan the way the pipeline builders already do.
 *
 * THE RULE THAT KEEPS THE TWO HALVES FROM DRIFTING: the SCHEMA owns what a resource IS (kind, scope,
 * lifetime, how many images the family holds); a PASS owns what it DOES with one (which binding,
 * read or write, which sampler, which stage). The only token they share is `resource_id`. A pass that
 * restates a resource's kind or scope would be a second copy of a fact - exactly the drift this module
 * exists to remove.
 *
 * OWN BINDINGS VERSUS SHARED ONES, which the first real declaration made unavoidable: a pass with a private
 * family still reaches the frame's shared resources (a probe cell's ray needs the top level structure, the
 * material table and the texture array to resolve and shade what it finds). Such a binding is declared here as
 * USAGE - which resource, which access - and `binding_owner` says whether it is the pass's own per-image
 * binding or something the frame's heap provides; the declaration is the whole of what is left of that
 * vocabulary, because every stage is heap-native now and reaches those resources through the frame's descriptor
 * heap rather than through a set (see vulkan/core/descriptor_heap).
 */

module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

export module deren.vulkan.render_resource;

export namespace deren::vulkan::render_resource {

    // =============================================================================================
    // 1. WHAT EXISTS - the schema. Declared here, implemented by vulkan.core.
    // =============================================================================================

    /**
     * @brief every resource family this renderer declares
     *
     * `none` is first on purpose: it is the value a binding holds before it is filled in, and the validator
     * rejects it, so a binding that was never given a resource cannot pass silently.
     *
     * THE ORDER OF THIS ENUM IS NOT A CONTRACT - unlike `gpu_mark_id`, whose order IS the timing contract and
     * whose 15 call sites must agree with it. Nothing indexes by this; it is an identity and nothing more.
     * That is why this list can live in one place without becoming the positional coupling that enum is.
     */
    enum class resource_id : uint32_t {
        none = 0,
        // ---- the swapchain image itself, owned outside ----
        swapchain_image,
        // ---- the HDR chain and the G-buffer (one per swapchain image) ----
        hdr,
        bloom,
        ldr,
        gbuffer_targets, // 3: albedo (RGBA8), normal + roughness (RGBA16F), material + baked AO (RGBA8)
        gbuffer_depth,
        velocity,
        scene_color,
        taa_history,
        // ---- the stochastic punctual lighting chain (one per swapchain image) ----
        ml_trace,
        ml_resolve,
        ml_history,
        // ---- one per frame slot ----
        shadow_map,
        rt_shadow_visibility,
        camera_ubo,
        light_ubo,
        material_table,
        instance_table,
        motion_vectors,
        skin_matrices,
        morph_targets,
        cluster_counts,
        cluster_indices,
        // ---- device wide ----
        furnace_cube,
        scene_textures,
        white_texture,
        ibl_env,
        ibl_irradiance,
        brdf_lut,
        // ---- what every traced pass needs ----
        top_level_structure,
        /// the number of enumerators, so the schema's completeness can be checked by ITERATING rather than
        /// against a hand-kept list: `validate_schema()` requires exactly one entry per id in [1, count)
        count,
    };

    /** @brief what a resource IS, which decides the descriptor types it can be bound as */
    enum class resource_kind : uint8_t {
        image2d,
        image3d,
        image_cube,
        buffer,
        accel_struct,
    };

    /**
     * @brief which frame counter a resource is indexed by
     *
     * THIS IS THE FIELD THIS MODULE EXISTS FOR, and it is not a classification exercise: this project's
     * documented per-image-lifetime trap is the difference between the two, and getting it wrong has already
     * produced a real bug here (one `bool` guarding a resource that is one-per-swapchain-image, so every slot
     * after the first never received its first-use transition). A pass that uses a `per_frame_slot` resource
     * is handed the frame slot; one that uses a `per_swapchain_image` resource is handed the image index.
     */
    enum class resource_scope : uint8_t {
        per_frame_slot,      // shadow maps, the light/camera/material/instance buffers, the cluster buffers
        per_swapchain_image, // every GI image, the TAA history, the G-buffer, the HDR chain
        device_wide,         // the probe grid and its geometry, the furnace cube, the IBL cubes
    };

    /** @brief how long a resource lives: what decides whether a first-use transition or a clear exists */
    enum class resource_lifetime : uint8_t {
        per_frame,  // rewritten every frame by its owner
        persistent, // holds state ACROSS frames (histories, the accumulated grid) - see the reset sites
        imported,   // owned outside this renderer (the swapchain image)
    };

    /** @brief one family of resources: what it is, how it is indexed, and how many images it holds
     *  @ingroup vulkan_render_resource */
    struct resource_info {
        resource_id id = resource_id::none;
        std::string_view name = {};
        resource_kind kind = resource_kind::image2d;
        resource_scope scope = resource_scope::per_swapchain_image;
        resource_lifetime lifetime = resource_lifetime::per_frame;
        /// how many images the family holds: 3 G-buffer targets, 8 probe coefficients (side*4+coefficient,
        /// exactly how `core` indexes them), 1 for an ordinary image. A binding names one ELEMENT of this.
        uint16_t count = 1;
    };

    /**
     * @brief the schema itself
     *
     * EACH ENTRY'S PROVENANCE, so the table can be re-derived rather than trusted: the HDR chain, the
     * G-buffer, the velocity and scene-colour targets and the TAA history are created in
     * `core::create_render_targets` (`core.cpp:506-693`); the chain's images at `636-749`; the probe grid
     * and its geometry at `798-811`; the furnace cube at `814-825`; the ray-traced shadow visibility at
     * `831-844`, one per FRAME SLOT rather than per image; the shadow map is the one family `deren.vulkan.runtime`
     * creates itself (`runtime.cpp:495-560`, layered, one image per slot); the scene buffers and the IBL
     * textures are `deren.vulkan.runtime`'s (`runtime.cpp:148-231`, `163-166`); the top level structure belongs to
     * `deren.vulkan.acceleration_structure` and reaches shaders through the frame's heap.
     *
     * WHAT AN ENTRY DOES NOT SAY YET, on purpose: its format and its extent. Those are needed by the CREATION
     * step, not by the invariants checkable today, and a format copied here before it is verified against
     * `core.cpp` would be a second copy of a fact - the thing this file's header warns about.
     * @ingroup vulkan_render_resource
     */
    inline constexpr std::array<resource_info, 30> resource_schema = {{
        {.id = resource_id::swapchain_image, .name = "swapchain_image", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::imported},
        {.id = resource_id::hdr, .name = "hdr", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        // FOUR LEVELS, not one: `core::bloom_images` is `std::array<std::vector<VkImage>, bloom_level_count>`
        // with `bloom_level_count == 4`, and the post chain's passes each own ONE of them. The count was 1 (the
        // default) until the bloom chain was extracted, which made `element = level` illegal for every pass but
        // the first - and the validator's `element >= info->count` check is what says so, so this field IS the
        // contract a declaration is written against.
        {.id = resource_id::bloom, .name = "bloom", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame, .count = 4},
        {.id = resource_id::ldr, .name = "ldr", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::gbuffer_targets, .name = "gbuffer_targets", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame, .count = 3},
        {.id = resource_id::gbuffer_depth, .name = "gbuffer_depth", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::velocity, .name = "velocity", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::scene_color, .name = "scene_color", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::taa_history, .name = "taa_history", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::ml_trace, .name = "ml_trace", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        // The temporal resolve's output - what the lighting stage actually samples - and the accumulation it
        // reads back next frame. The history holds the radiance in rgb and the FRAME COUNT in alpha (an
        // integer up to 12 fits exactly in a float16), which is the per-pixel "how many frames of samples is
        // this": UE keeps it in an R8_UINT image of its own, and the alpha lane is the free one here.
        {.id = resource_id::ml_resolve, .name = "ml_resolve", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::ml_history, .name = "ml_history", .kind = resource_kind::image2d, .scope = resource_scope::per_swapchain_image, .lifetime = resource_lifetime::persistent},
        // FOUR LAYERS, not one: the runtime creates the shadow map as ONE layered image (`max_shadow_cascades` layers, one per cascade, plus the spare layers a shrank count leaves behind - see ensure_shadow_resources), and the shadow pass renders into them one at a time. The count was 1 (the default) until that pass was declared, which made `element = cascade` illegal for every cascade but the first - the same correction the bloom family needed, and the validator`s element check is what says so.
        {.id = resource_id::shadow_map, .name = "shadow_map", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame, .count = 4},
        {.id = resource_id::rt_shadow_visibility, .name = "rt_shadow_visibility", .kind = resource_kind::image2d, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::camera_ubo, .name = "camera_ubo", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::light_ubo, .name = "light_ubo", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::material_table, .name = "material_table", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::instance_table, .name = "instance_table", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::motion_vectors, .name = "motion_vectors", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::skin_matrices, .name = "skin_matrices", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::morph_targets, .name = "morph_targets", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::cluster_counts, .name = "cluster_counts", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::cluster_indices, .name = "cluster_indices", .kind = resource_kind::buffer, .scope = resource_scope::per_frame_slot, .lifetime = resource_lifetime::per_frame},
        {.id = resource_id::furnace_cube, .name = "furnace_cube", .kind = resource_kind::image_cube, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::scene_textures, .name = "scene_textures", .kind = resource_kind::image2d, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::white_texture, .name = "white_texture", .kind = resource_kind::image2d, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::ibl_env, .name = "ibl_env", .kind = resource_kind::image_cube, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::ibl_irradiance, .name = "ibl_irradiance", .kind = resource_kind::image_cube, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::brdf_lut, .name = "brdf_lut", .kind = resource_kind::image2d, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::persistent},
        {.id = resource_id::top_level_structure, .name = "top_level_structure", .kind = resource_kind::accel_struct, .scope = resource_scope::device_wide, .lifetime = resource_lifetime::per_frame},
    }};

    /// @brief the schema entry for @p id, or nullptr when nothing declares it
    [[nodiscard]] constexpr resource_info const* find(resource_id const id) noexcept {
        for (resource_info const& info : resource_schema) {
            if (info.id == id) {
                return &info;
            }
        }
        return nullptr;
    }

    // =============================================================================================
    // 2. WHAT A PASS DOES WITH ONE - the usage half. The only shared token is `resource_id`.
    // =============================================================================================

    /// @brief how a binding is declared, which is what fixes its `VkDescriptorType`
    enum class binding_kind : uint8_t {
        sampled_image,
        storage_image,
        sampler,
        uniform_buffer,
        storage_buffer,
        input_attachment,
        acceleration_structure,
    };

    /**
     * @brief what the pass DOES with it, which is NOT derivable from the kind
     *
     * A filter only READS its input storage image while writing its output; another pass writes
     * `gi_trace` and reads it back in the same dispatch. A barrier stage keys on this, not on the kind.
     */
    enum class binding_access : uint8_t {
        read,
        write,
        read_write,
    };

    /// @brief which of this renderer's samplers a binding wants (five exist; the choice is a field, not a ternary)
    enum class sampler_hint : uint8_t {
        none,
        gbuffer,
        taa,
        post,
        nearest,
        shadow,
    };

    // AN `image_layout` ENUM AND A `pass_binding::layout` FIELD STOOD HERE, and they are gone with
    // VK_KHR_unified_image_layouts (see docs/unified_image_layouts.md): this renderer keeps every image in
    // VK_IMAGE_LAYOUT_GENERAL, so a declaration has no layout left to state. They had been added after the
    // first conversion because the layout was NOT derivable from the kind - the probe cache kept all nine of
    // its own bindings in GENERAL while a sampled image elsewhere was in SHADER_READ_ONLY_OPTIMAL, so
    // "storage means GENERAL, sampled means SHADER_READ" was wrong for a real pass. One layout makes the
    // question disappear, and with it the validator rule that used to enforce the answer.

    /**
     * @brief where a binding's resource comes from: the pass's own per-image binding, or the frame's heap
     *
     * `own` marks one of the pass's private per-image bindings: the resolver indexes those by `binding`, and
     * the validator requires them to be numbered contiguously from zero. `shared` marks a binding the pass
     * reaches through the frame's descriptor heap - the scene's camera and material table, the G-buffer
     * surface, the post chain's images - whose descriptors some other stage publishes; a pass declares only
     * that it USES such a binding, and cannot change it.
     */
    enum class binding_owner : uint8_t {
        own,
        shared,
    };

    /// @brief shader stages as bits, so a binding can list more than one (the shared resources serve two)
    enum class stage_flag : uint8_t {
        none = 0u,
        vertex = 1u << 0u,
        fragment = 1u << 1u,
        compute = 1u << 2u,
    };

    [[nodiscard]] constexpr stage_flag operator|(stage_flag const a, stage_flag const b) noexcept {
        return static_cast<stage_flag>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }

    [[nodiscard]] constexpr bool has_stage(stage_flag const set, stage_flag const one) noexcept {
        return (static_cast<uint8_t>(set) & static_cast<uint8_t>(one)) != 0u;
    }

    /// @brief a name for each enum, so a validator message says "storage_image is not a buffer" not "1 is not 3"
    [[nodiscard]] constexpr std::string_view name_of(binding_kind const kind) noexcept {
        switch (kind) {
        case binding_kind::sampled_image:
            return "sampled_image";
        case binding_kind::storage_image:
            return "storage_image";
        case binding_kind::sampler:
            return "sampler";
        case binding_kind::uniform_buffer:
            return "uniform_buffer";
        case binding_kind::storage_buffer:
            return "storage_buffer";
        case binding_kind::input_attachment:
            return "input_attachment";
        case binding_kind::acceleration_structure:
            return "acceleration_structure";
        }
        return "?";
    }

    [[nodiscard]] constexpr std::string_view name_of(resource_kind const kind) noexcept {
        switch (kind) {
        case resource_kind::image2d:
            return "image2d";
        case resource_kind::image3d:
            return "image3d";
        case resource_kind::image_cube:
            return "image_cube";
        case resource_kind::buffer:
            return "buffer";
        case resource_kind::accel_struct:
            return "accel_struct";
        }
        return "?";
    }

    [[nodiscard]] constexpr std::string_view name_of(binding_access const access) noexcept {
        switch (access) {
        case binding_access::read:
            return "read";
        case binding_access::write:
            return "write";
        case binding_access::read_write:
            return "read_write";
        }
        return "?";
    }

    /// @brief one binding: one use. Everything the descriptor and the heap write need is here.
    /// @ingroup vulkan_render_resource
    struct pass_binding {
        uint32_t binding = 0;
        binding_owner owner = binding_owner::own;
        binding_kind kind = binding_kind::sampled_image;
        resource_id resource = resource_id::none;
        /// which image of the resource FAMILY this binding names (0 for a single-image resource)
        uint16_t element = 0;
        /// how many descriptors this binding declares: 1, or the capacity of a bindless array. The validator
        /// refuses zero, so a binding that claims no descriptor cannot pass silently.
        uint16_t descriptor_count = 1;
        binding_access access = binding_access::read;
        sampler_hint sampler = sampler_hint::none;
        stage_flag stages = stage_flag::compute;
    };

    /// @brief a push-constant range: declared and pinned by `static_assert`, never computed from a declaration
    struct push_block {
        uint32_t offset = 0;
        uint32_t size = 0;
        stage_flag stages = stage_flag::compute;
    };

    /// @brief which slot of a rendering instance a declared target fills
    enum class target_kind : uint8_t {
        color, // a colour attachment (one of the G-buffer's targets, the HDR image a resolve writes)
        depth, // THE depth attachment, of which a rendering instance has exactly one
    };

    /**
     * @brief an image a pass RENDERS INTO, which is a use that cannot be a descriptor
     *
     * A fullscreen pass's colour attachment is not in any set: it is bound by `vkCmdBeginRendering`, so the
     * declaration has to name it somewhere else - and it has to name it SOMEWHERE, because otherwise a pass
     * could render into an image it never declared, which is the one property this layer enforces. Keeping it
     * in `bindings` would be worse than a second list: a binding is a descriptor, the layout generator walks
     * that list, and a colour attachment has no `VkDescriptorType` at all.
     *
     * The LOAD OP and the clear value are deliberately NOT declared: the pass that renders into the image is
     * the one that opens the rendering instance (see `behaviour_kind::fullscreen` and `graphics`), so it is the
     * one that says whether the old contents matter. What is declared is only what the layer has to know: which
     * resource, which image of its family, and whether it fills the colour or the DEPTH slot.
     */
    struct render_target {
        resource_id resource = resource_id::none;
        uint16_t element = 0; // which image of the resource's family (per-swapchain-image resources: the index)
        target_kind kind = target_kind::color;
        /**
         * HOW MANY CONSECUTIVE ELEMENTS of the family this ONE entry claims, rendered ONE INSTANCE PER ELEMENT.
         *
         * WHY IT IS NOT "several attachments of one instance", which is the obvious reading: a rendering instance
         * has exactly ONE depth attachment, so the N layers of one depth array cannot be one instance's targets.
         * The shadow pass renders its cascades layer by layer, each with its own transition, instance and
         * secondary - and what its declaration could not say before was that all N of those layers are elements
         * of ONE family, so the host had to hand them over itself (`runtime::resolve_shadow_pass`, now gone).
         *
         * A CEILING RATHER THAN A PROMISE: how many elements a family HAS this frame is the FRAME's fact (the
         * shadow map allocates the layers the cascade knob asks for - one to four - and `ensure_shadow_resources`
         * may keep spare layers a shrank count left behind), so resolution hands over the first @c count elements
         * that EXIST and the pass renders what it is given. `element + count` past the family's own count is a
         * declaration error the validator refuses, so this field is a claim about the SCHEMA, and the frame's
         * answer is only ever a shorter run.
         */
        uint16_t count = 1;
    };

    /**
     * @brief an image a pass must TRANSITION but does not bind as a descriptor
     *
     * because a layout transition names an IMAGE and no descriptor at all - the same argument that made
     * `render_target` a separate list from `bindings`.
     *
     * The LAYOUTS are deliberately not declared: which pair of layouts a transition uses is the pass's
     * knowledge (its own barriers, its own order), and a declaration that named them would have to be a
     * barrier generator rather than a description of what the pass touches. What is declared is only what the
     * layer must know to hand the pass a handle: which resource, and which image of its family.
     */
    struct barrier_image {
        resource_id resource = resource_id::none;
        /// which image of the resource's family: the FRAME's image index for a per-swapchain-image resource
        /// (resolved by the host from the frame, exactly as `render_target` does), or the element for a family
        /// a pass reaches by position (the probe grid's eight)
        uint16_t element = 0;
    };

    /**
     * @brief a BUFFER a pass orders around but never binds itself - the buffer twin of `barrier_image`
     *
     * WHY THIS EXISTS, and it is a measured need rather than symmetry for its own sake: the clustered-light
     * sort (`deren.vulkan.pass.cluster`) writes two buffers that are part of the frame's shared scene resources
     * (bindings 11 and 12). The pass reaches them through the heap, so its declaration names no binding for
     * either buffer - and yet
     * its writes are not visible to the fragment stages that read them later in the same submission without a
     * buffer memory barrier, which only the writer can place. Before this field the renderer kept the whole
     * recording for exactly that reason: a pass could declare an IMAGE it moves and had no way to name a BUFFER
     * it moves. The pair of channel fields is now the same shape as `barrier_images`, one per resource class.
     *
     * The ACCESS masks are deliberately not declared, for the reason `barrier_image` gives about layouts: which
     * stages and accesses a barrier names is the pass's knowledge of its own ordering, and a declaration that
     * named them would be a barrier generator rather than a description of what the pass touches.
     */
    struct barrier_buffer {
        resource_id resource = resource_id::none;
        /// which buffer of the resource's family - the frame's slot for a per-frame-slot resource (resolved by
        /// the host from the frame, exactly as `barrier_image` does for images)
        uint16_t element = 0;
    };

    /**
     * @brief one pass's declared I/O
     *
     * `bindings` carries both kinds: the `binding_owner::own` entries are the pass's private per-image
     * bindings, which the validator requires to be numbered contiguously from zero, and the
     * `binding_owner::shared` entries are declared USAGE of resources the frame's heap provides.
     * @ingroup vulkan_render_resource
     */
    struct pass_io {
        std::string_view name = {};
        std::span<pass_binding const> bindings = {};
        /// the images this pass renders into, in the order it uses them (a fullscreen pass has one)
        std::span<render_target const> targets = {};
        /**
         * The images this pass TRANSITIONS but never binds (see `barrier_image`), in the order the pass's own
         * record() indexes them. Empty for every pass whose resources are all its own or all descriptors.
         */
        std::span<barrier_image const> barrier_images = {};
        /**
         * The BUFFERS this pass orders around but never binds (see `barrier_buffer`), in the order the pass's
         * own record() indexes them. Empty for every pass but the clustered-light sort today.
         */
        std::span<barrier_buffer const> barrier_buffers = {};
        std::optional<push_block> push = std::nullopt;
    };

    // =============================================================================================
    // 3. THE INVARIANTS - the reason this is a module and not a comment
    // =============================================================================================

    /// @brief kind compatibility: which resource kinds a binding kind may name
    [[nodiscard]] constexpr bool compatible(binding_kind const kind, resource_kind const resource) noexcept {
        switch (kind) {
        case binding_kind::sampled_image:
        case binding_kind::storage_image:
        case binding_kind::input_attachment:
            return resource == resource_kind::image2d || resource == resource_kind::image3d || resource == resource_kind::image_cube;
        case binding_kind::sampler:
            return true; // a standalone sampler names no resource of its own
        case binding_kind::uniform_buffer:
        case binding_kind::storage_buffer:
            return resource == resource_kind::buffer;
        case binding_kind::acceleration_structure:
            return resource == resource_kind::accel_struct;
        }
        return false;
    }

    /// @brief access compatibility: what each binding kind can do
    [[nodiscard]] constexpr bool compatible(binding_kind const kind, binding_access const access) noexcept {
        switch (kind) {
        case binding_kind::sampled_image:
        case binding_kind::sampler:
        case binding_kind::uniform_buffer:
        case binding_kind::input_attachment:
        case binding_kind::acceleration_structure:
            return access == binding_access::read;
        case binding_kind::storage_image:
        case binding_kind::storage_buffer:
            return true; // writable, or readable: the spatial filter's input is a read-only storage image
        }
        return false;
    }

    /** @brief the schema's own invariants: no empty name, no empty family, and ONE entry per declared id */
    [[nodiscard]] inline std::expected<void, std::string> validate_schema() {
        for (resource_info const& info : resource_schema) {
            if (info.id == resource_id::none) {
                return std::unexpected("a schema entry has no id");
            }
            if (static_cast<uint32_t>(info.id) >= static_cast<uint32_t>(resource_id::count)) {
                return std::unexpected("a schema entry uses an id outside the enumeration");
            }
            if (info.name.empty()) {
                return std::unexpected("a schema entry has no name");
            }
            if (info.count == 0) {
                return std::unexpected(std::string(info.name) + " declares zero images");
            }
            if (info.lifetime == resource_lifetime::imported && info.scope != resource_scope::per_swapchain_image) {
                return std::unexpected(std::string(info.name) + " is imported but not per-swapchain-image");
            }
        }
        // Completeness: every enumerator the renderer can name has exactly one entry. A forgotten entry is
        // the failure this catches, and it catches it without a second hand-kept list of ids.
        for (uint32_t raw = 1; raw < static_cast<uint32_t>(resource_id::count); ++raw) {
            std::size_t seen = 0;
            for (resource_info const& info : resource_schema) {
                if (static_cast<uint32_t>(info.id) == raw) {
                    ++seen;
                }
            }
            if (seen != 1) {
                return std::unexpected("the schema has " + std::to_string(seen) + " entries for resource id " + std::to_string(raw));
            }
        }
        return {};
    }

    /**
     * @brief a pass's declaration against the schema
     *
     * THE INVARIANTS, each one either a bug this project has met or a class of bug it is built to avoid: a
     * named pass; every binding names a resource the schema DECLARES (usage is a subset of the schema); the
     * binding kind fits the resource kind (a `sampler3D` cannot be declared as a 2D image; an acceleration
     * structure is not a buffer); the access fits the kind (a sampled image cannot be written); `element` is
     * inside the family; `descriptor_count` is at least one; a sampled image names a sampler and nothing else
     * does; no two bindings share a binding number, which would silently lose one of them; and the pass's OWN
     * bindings are numbered contiguously from zero, because those are the ones the resolver indexes by binding
     * number. A render TARGET gets the same two checks a binding gets - the schema declares the resource, and
     * the element is inside its family - plus uniqueness, because two targets naming one image would be a pass
     * rendering into itself twice. A target that claims a RUN of elements (`render_target::count`) is checked
     * as the run it is: the last element has to be inside the family, and two runs of one resource may not
     * overlap.
     * @ingroup vulkan_render_resource
     */
    [[nodiscard]] inline std::expected<void, std::string> validate(pass_io const& io) {
        if (io.name.empty()) {
            return std::unexpected("a pass declaration has no name");
        }
        std::string const who{io.name};
        std::size_t own_count = 0;
        std::size_t depth_targets = 0;
        for (render_target const& t : io.targets) {
            std::string const where = who + ": target " + std::to_string(t.element);
            resource_info const* const info = find(t.resource);
            if (info == nullptr) {
                return std::unexpected(where + " names a resource the schema does not declare");
            }
            if (info->kind != resource_kind::image2d && info->kind != resource_kind::image3d && info->kind != resource_kind::image_cube) {
                return std::unexpected(where + " names " + std::string(info->name) + ", which is not an image a pass can render into");
            }
            if (t.element >= info->count) {
                return std::unexpected(where + " names element " + std::to_string(t.element) + " of " + std::string(info->name) + ", which holds " +
                                       std::to_string(info->count));
            }
            if (t.count == 0u) {
                return std::unexpected(where + " claims no element of " + std::string(info->name) + ", so it renders nothing");
            }
            if (static_cast<uint32_t>(t.element) + t.count > info->count) {
                // A RUN that reaches past the family: `count` is a claim about the SCHEMA, and the frame can only
                // ever answer with a SHORTER run (see render_target::count).
                return std::unexpected(where + " claims elements " + std::to_string(t.element) + ".." + std::to_string(static_cast<uint32_t>(t.element) + t.count - 1u) + " of " +
                                       std::string(info->name) + ", which holds " + std::to_string(info->count));
            }
            if (t.kind == target_kind::depth && ++depth_targets > 1u) {
                // a rendering instance has exactly one depth attachment, so a second one cannot be recorded - and a
                // RUN of depth elements is still ONE entry here, because each element gets its own instance
                return std::unexpected(who + ": more than one DEPTH target is declared, and an instance has one");
            }
            for (render_target const& other : io.targets) {
                // Two entries whose element RUNS INTERSECT would have the pass render into one image twice - the
                // same defect the exact-match check below reports, one resource class over.
                if (&other != &t && other.resource == t.resource && t.element < static_cast<uint32_t>(other.element) + other.count && other.element < static_cast<uint32_t>(t.element) + t.count) {
                    return std::unexpected(where + " overlaps the entries " + std::to_string(other.element) + ".." + std::to_string(static_cast<uint32_t>(other.element) + other.count - 1u) + " of the same resource");
                }
            }
        }
        // THE BARRIER IMAGES, checked like a target: an image the pass will move between layouts has to exist
        // in the schema and has to BE an image (a transition names no descriptor, but it does name a resource).
        for (barrier_image const& t : io.barrier_images) {
            std::string const where = who + ": barrier image " + std::to_string(t.element);
            resource_info const* const info = find(t.resource);
            if (info == nullptr) {
                return std::unexpected(where + " names a resource the schema does not declare");
            }
            if (info->kind != resource_kind::image2d && info->kind != resource_kind::image3d && info->kind != resource_kind::image_cube) {
                return std::unexpected(where + " names " + std::string(info->name) + ", which is not an image");
            }
            if (t.element >= info->count) {
                return std::unexpected(where + " names element " + std::to_string(t.element) + " of " + std::string(info->name) + ", which holds " +
                                       std::to_string(info->count));
            }
            for (barrier_image const& other : io.barrier_images) {
                if (&other != &t && other.resource == t.resource && other.element == t.element) {
                    return std::unexpected(where + " is declared twice, and a pass indexes these by position");
                }
            }
        }
        // ... and THE BARRIER BUFFERS, the same three checks one resource class over: it has to exist in the
        // schema, it has to BE a buffer, and a pass indexes these by position so a duplicate is a defect.
        for (barrier_buffer const& t : io.barrier_buffers) {
            std::string const where = who + ": barrier buffer " + std::to_string(t.element);
            resource_info const* const info = find(t.resource);
            if (info == nullptr) {
                return std::unexpected(where + " names a resource the schema does not declare");
            }
            if (info->kind != resource_kind::buffer) {
                return std::unexpected(where + " names " + std::string(info->name) + ", which is not a buffer");
            }
            if (t.element >= info->count) {
                return std::unexpected(where + " names element " + std::to_string(t.element) + " of " + std::string(info->name) + ", which holds " +
                                       std::to_string(info->count));
            }
            for (barrier_buffer const& other : io.barrier_buffers) {
                if (&other != &t && other.resource == t.resource && other.element == t.element) {
                    return std::unexpected(where + " is declared twice, and a pass indexes these by position");
                }
            }
        }
        for (pass_binding const& b : io.bindings) {
            std::string const where = who + ": binding " + std::to_string(b.binding);
            resource_info const* const info = find(b.resource);
            if (info == nullptr) {
                return std::unexpected(where + " names a resource the schema does not declare");
            }
            if (!compatible(b.kind, info->kind)) {
                return std::unexpected(where + " is declared " + std::string(name_of(b.kind)) + " but " + std::string(info->name) + " is " +
                                       std::string(name_of(info->kind)));
            }
            if (!compatible(b.kind, b.access)) {
                return std::unexpected(where + " is a " + std::string(name_of(b.kind)) + " used for " + std::string(name_of(b.access)));
            }
            if (b.element >= info->count) {
                return std::unexpected(where + " names element " + std::to_string(b.element) + " of " + std::string(info->name) +
                                       ", which holds " + std::to_string(info->count));
            }
            if (b.descriptor_count == 0) {
                return std::unexpected(where + " declares no descriptors");
            }
            if ((b.kind == binding_kind::sampled_image) != (b.sampler != sampler_hint::none)) {
                return std::unexpected(where + " must name a sampler exactly when it is a sampled image");
            }
            for (pass_binding const& other : io.bindings) {
                if (&other != &b && other.binding == b.binding) {
                    return std::unexpected(where + " is declared twice");
                }
            }
            if (b.owner == binding_owner::own) {
                if (b.binding != own_count) {
                    return std::unexpected(who + ": the pass's own bindings are not numbered contiguously from zero (expected " +
                                           std::to_string(own_count) + ", found " + std::to_string(b.binding) + ")");
                }
                ++own_count;
            }
        }
        if (io.push.has_value()) {
            if (io.push->size == 0 || (io.push->size % 4u) != 0u || io.push->offset + io.push->size > 128u) {
                return std::unexpected(who + ": the push block does not fit the 128-byte guaranteed minimum in 4-byte units");
            }
        }
        return {};
    }

    // =============================================================================================
    // 4. THE FIRST REAL DECLARATION - the TAA resolve (see section 5 below)
    // =============================================================================================

    // =============================================================================================
    // 5. THE SECOND DECLARATION - the TAA resolve, read off shaders/taa.slang
    // =============================================================================================

    /**
     * @brief the temporal resolve's I/O, as its shader declares it
     *
     * This is the first declaration whose bindings are all the pass's own and all per swapchain image, with
     * nothing shared beside them: the four inputs (this frame's colour, the reprojected history, the motion
     * vectors and the depth the disocclusion guard reads) are all this pass's. `stages` is FRAGMENT and has
     * to be said: the resolved stage flags come from the declaration, and a resolve whose bindings
     * were declared COMPUTE would be a binding the fragment stage cannot see - which is the drift this
     * declaration exists to make impossible, now in the other direction (the field is easy to forget when every
     * other declaration so far was a compute pass).
     */
    inline constexpr std::array<pass_binding, 4> taa_bindings = {{
        {.binding = 0, .owner = binding_owner::own, .kind = binding_kind::sampled_image, .resource = resource_id::scene_color, .access = binding_access::read, .sampler = sampler_hint::taa, .stages = stage_flag::fragment},
        {.binding = 1, .owner = binding_owner::own, .kind = binding_kind::sampled_image, .resource = resource_id::taa_history, .access = binding_access::read, .sampler = sampler_hint::taa, .stages = stage_flag::fragment},
        {.binding = 2, .owner = binding_owner::own, .kind = binding_kind::sampled_image, .resource = resource_id::velocity, .access = binding_access::read, .sampler = sampler_hint::taa, .stages = stage_flag::fragment},
        {.binding = 3, .owner = binding_owner::own, .kind = binding_kind::sampled_image, .resource = resource_id::gbuffer_depth, .access = binding_access::read, .sampler = sampler_hint::taa, .stages = stage_flag::fragment},
    }};

    /// @brief the resolve RENDERS INTO the frame's HDR target, which is why it needs a target and not a binding
    inline constexpr render_target taa_target = {.resource = resource_id::hdr, .element = 0};
    inline constexpr std::array<render_target, 1> taa_targets = {taa_target};

    /// @brief the temporal resolve's declaration
    /// @ingroup vulkan_render_resource
    inline constexpr pass_io taa_io = {
        .name = "taa",
        .bindings = taa_bindings,
        .targets = taa_targets,
        // eight floats: the history flag, the two blend weights, the texel size, and the projection's two
        // depth terms (see vulkan.pass.taa::taa_pass::push_constants, which static_asserts this number)
        .push = push_block{.offset = 0, .size = 32, .stages = stage_flag::fragment},
    };

    // =============================================================================================
    // 6. THE THIRD DECLARATION - the scene pass, which owns no binding at all
    // =============================================================================================

    /**
     * @brief the scene pass's I/O: five colour attachments, one depth attachment, and no binding of its own
     *
     * THIS IS THE FIRST DECLARATION WITH NO OWN BINDINGS AT ALL, and that is what the pass IS: it draws the
     * scene's primitives into the frame's surface targets. The material table, the texture array, the camera
     * and light UBOs, the shadow map, the instance table and the top level structure all reach its shaders
     * through the frame's heap, whose contents its owners publish - so this declaration names no binding for
     * any of them. Those are `binding_owner::shared` resources the pass only USES, not owns.
     *
     * THE TARGETS ARE THE FRAME'S SURFACE, in the order the rendering instance needs them: the three stored
     * G-buffer targets, the motion-vector target, the scene colour target the lighting stage adds on top of,
     * and the G-buffer's own depth. The per-leaf push constants are the LEAVES' (a model matrix, a material
     * index), pushed through the draw path the primitive owns, so this declaration has no push block.
     *
     * `scene_color` IS AN ALIAS, and deliberately: the resolver maps it to the image the frame's scene pass
     * accumulates emissive into, which is `runtime::scene_target_view()` - the HDR target normally, and the
     * TAA resolve's input while that resolve runs (it takes the HDR target for its own output). That is a
     * FRAME decision, not a resource fact, so the declaration names what it means and the renderer decides
     * which image that is. It also means the scene pass and the TAA resolve agree by construction about which
     * image the scene writes and the resolve reads.
     */
    inline constexpr std::array<render_target, 6> scene_targets = {{
        {.resource = resource_id::gbuffer_targets, .element = 0},
        {.resource = resource_id::gbuffer_targets, .element = 1},
        {.resource = resource_id::gbuffer_targets, .element = 2},
        {.resource = resource_id::velocity, .element = 0},
        {.resource = resource_id::scene_color, .element = 0},
        {.resource = resource_id::gbuffer_depth, .element = 0, .kind = target_kind::depth},
    }};

    // cl 19.44 (MSVC 14.44) ICEs on this shape - and ONLY this shape: a module-interface-scope constexpr
    // variable whose value holds a DISENGAGED std::optional (fatal error C1001 in msc1.cpp:1589, exit
    // 0xC0000005). The four pass declarations whose push block is ABSENT (scene, transparent,
    // character_forward, cluster) are exactly that, each one independently - fixing any three still crashes.
    // constinit keeps the constant initialisation (the object is still statically initialised and immutable)
    // and gives up only constant-expression usability, which nothing asks of these four. Measured on this file:
    // clean 3/3 with constinit, ICE 4/4 without
    // (build-release-clang64/msvc/lead_lab/ice_rr/ICE_RR_FINDINGS.md).
    /// @brief the scene pass's declaration
    /// @ingroup vulkan_render_resource
    inline constinit pass_io const scene_io = {
        .name = "scene",
        .bindings = {},
        .targets = scene_targets,
        .push = std::nullopt,
    };

    /**
     * @brief the transparent pass's declaration: the same surface, entered with LOAD
     *
     * TWO TARGETS, not six: the alpha-blended leaves composite over the shaded frame, so the pass draws into
     * the scene colour target and depth-tests against the surface depth - and it LOADs both, because what it
     * composites over must survive. That decision is the pass's (it opens the instance), which is why the
     * declaration names the images and not the load ops (see render_target).
     *
     * It reaches the same shared resources the scene pass does - a blended surface reads the same materials, the
     * same camera and the same shadow map - so it declares no binding of its own either.
     */
    inline constexpr std::array<render_target, 2> transparent_targets = {{
        {.resource = resource_id::scene_color, .element = 0},
        {.resource = resource_id::gbuffer_depth, .element = 0, .kind = target_kind::depth},
    }};

    /// @brief the transparent pass's declaration
    /// @ingroup vulkan_render_resource
    // constinit for the same cl 19.44 ICE as scene_io above (a disengaged std::optional in a constexpr global)
    inline constinit pass_io const transparent_io = {
        .name = "transparent",
        .bindings = {},
        .targets = transparent_targets,
        .push = std::nullopt,
    };

    /**
     * @brief the character-forward pass's declaration: the same TWO targets as the transparent pass, and
     *        for the same structural reason
     *
     * It re-shades surfaces the deferred stage has already lit, so it needs the scene colour to overwrite
     * and the surface depth to test against - and it LOADs both, because everything it does not draw must
     * survive untouched. That is the whole reason it is a pass of its own rather than a step inside the
     * lighting instance: an image cannot be a sampled texture and a depth attachment at once.
     *
     * The difference from `transparent_io` is entirely in the pipeline and the raster state, none of which
     * a declaration names (see render_target): depth compare EQUAL rather than LESS_OR_EQUAL, depth write
     * OFF and held off, and blending OFF because this pass overwrites rather than composites.
     *
     * It declares no binding of its own: it reads the same materials, camera and light the scene pass does,
     * and all of those reach its shaders through the frame's heap.
     */
    inline constexpr std::array<render_target, 2> character_forward_targets = {{
        {.resource = resource_id::scene_color, .element = 0},
        {.resource = resource_id::gbuffer_depth, .element = 0, .kind = target_kind::depth},
    }};

    /// @brief the character-forward pass's declaration
    /// @ingroup vulkan_render_resource
    // constinit for the same cl 19.44 ICE as scene_io above (a disengaged std::optional in a constexpr global)
    inline constinit pass_io const character_forward_io = {
        .name = "character_forward",
        .bindings = {},
        .targets = character_forward_targets,
        .push = std::nullopt,
    };

    /**
     * @brief the screen-space depth rim's declaration: ONE target and NO bindings, because it samples the
     *        frame's own G-buffer through the heap
     *
     * THE TARGET IS THE SCENE COLOUR AND THE PIPELINE BLENDS ADDITIVELY: the rim is a contribution to the
     * frame the character-forward stage produced, so the pass never has to read the image it writes - which is
     * what keeps it from needing a copy of it.
     *
     * NO DEPTH ATTACHMENT IS DECLARED, and that is the whole reason this pass exists separately: it samples the
     * depth, and a pass cannot both attach an image and sample it. See the declaration's neighbours for the
     * opposite arrangement (the character-forward and transparent passes both attach the depth).
     *
     * NO BARRIER IMAGES EITHER: the G-buffer targets and the depth are published to a sampled layout by the
     * RENDERER's stage preamble, because that publication is per-image bookkeeping about images the G-buffer
     * pass wrote - the same rule the ray-traced shadow and the stochastic lighting stages follow. The push
     * block is 48 bytes: the two projection terms from the frame, the rim's width/gain/strength and its colour
     * from the pass, and the framework appends the two heap lanes on top.
     */
    inline constexpr std::array<render_target, 1> toon_screen_rim_targets = {{
        {.resource = resource_id::scene_color, .element = 0},
    }};

    /// @brief the screen-space depth rim's declaration
    /// @ingroup vulkan_render_resource
    inline constexpr pass_io toon_screen_rim_io = {
        .name = "toon_screen_rim",
        .bindings = {},
        .targets = toon_screen_rim_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = 48, .stages = stage_flag::fragment},
    };

    /**
     * @brief the REWRITTEN chain's rim: the same shape as the article's contour above, one stage over
     *
     * ONE TARGET AND NO BINDINGS, for the reasons that declaration gives - the rim is ADDED to the frame the
     * character stage produced (additive blend, LOAD, no depth attachment, no barrier of its own; the G-buffer it
     * samples is published by the renderer's stage preamble). IT IS A SECOND DECLARATION RATHER THAN A SECOND
     * TARGET ON THAT ONE because the two rims are mutually exclusive by construction: `toon_screen_rim` is the
     * ARTICLE's contour and the owner's feature table switches it OFF whenever `runtime::goo_toon_active()` -
     * this pass's own gate - is true.
     *
     * THE PUSH BLOCK IS 72 BYTES - the frame's inverse view-projection (64) and the two depth-linearization terms
     * (8) - and it is the smallest of any stage with a matrix in it because every other number this rim uses is
     * either the reference's own constant (in the shader) or the material's (a colour lane the shader reads through
     * the G-buffer's material id). The framework appends the two heap lanes on top, so the shader's block is 80.
     */
    inline constexpr std::array<render_target, 1> goo_rim_targets = {{
        {.resource = resource_id::scene_color, .element = 0},
    }};

    /// @brief the rewritten chain's rim declaration
    /// @ingroup vulkan_render_resource
    inline constexpr pass_io goo_rim_io = {
        .name = "goo_rim",
        .bindings = {},
        .targets = goo_rim_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = 72, .stages = stage_flag::fragment},
    };

    // =============================================================================================
    // 7. THE SHARED RESOURCES - what a full-screen compute pass reaches without owning a binding
    // =============================================================================================

    /**
     * @brief the image the stochastic punctual lighting pass rewrites, and the only resource it names
     *
     * It reaches the image through the heap (the raw estimate the trace writes and the resolved image the
     * lighting stage adds are grid slots the shaders name), so this pass owns no
     * descriptor of its own and declares no binding: what it has to say is which image it moves, because only
     * the writer can place the transitions (UNDEFINED -> GENERAL to write it as a storage image, GENERAL ->
     * SHADER_READ to hand it to the lighting stage that adds it, both recorded by this pass).
     */
    inline constexpr std::array<barrier_image, 1> megalights_trace_barriers = {{
        {.resource = resource_id::ml_trace, .element = 0},
    }};

    /**
     * @brief the stochastic punctual lighting pass's declaration: a half-resolution compute dispatch
     *
     * No own binding, one image named through `barrier_images` - because its inputs (the camera, the light UBO,
     * the cluster lists, the G-buffer surface) and its output (a half-resolution storage image) all reach its
     * shaders through the frame's heap.
     *
     * The push block is 96 bytes: the camera's inverse view-projection, the estimator's parameters (samples
     * per pixel, the minimum sample weight, the ray tmin, the frame counter) and the two origin-bias terms.
     */
    inline constexpr pass_io megalights_trace_io = {
        .name = "megalights_trace",
        .bindings = {},
        .targets = {},
        .barrier_images = megalights_trace_barriers,
        .push = push_block{.offset = 0, .size = 96, .stages = stage_flag::compute},
    };

    /**
     * @brief the temporal resolve's own bindings, in the order its shader declares them
     *
     * FIVE: the three images of the stochastic chain plus the two G-buffer targets the resolve needs - the
     * velocity it reprojects with and the depth it rejects the history against. Those two are sampled as
     * per-image views like the chain's own, so they are OWN bindings of this pass rather than shared heap
     * resources, and their transitions are this pass's to publish (`megalights_temporal_barriers` carries all
     * five resources for that reason).
     *
     *   0: `ml_trace`   this frame's raw estimate (the temporal pass's input)
     *   1: `ml_history` last frame's accumulation, radiance in rgb and the frame count in alpha
     *   2: `velocity`   the G-buffer's motion vectors (the reprojection source)
     *   3: `gbuffer_depth` the G-buffer's depth (what the history is rejected against)
     *   4: `ml_resolve` the accumulation this dispatch WRITES (a storage image)
     */
    inline constexpr std::array<pass_binding, 5> megalights_temporal_bindings = {{
        {.binding = 0, .owner = binding_owner::own, .kind = binding_kind::sampled_image, .resource = resource_id::ml_trace, .access = binding_access::read, .sampler = sampler_hint::gbuffer},
        {.binding = 1, .owner = binding_owner::own, .kind = binding_kind::sampled_image, .resource = resource_id::ml_history, .access = binding_access::read, .sampler = sampler_hint::gbuffer},
        {.binding = 2, .owner = binding_owner::own, .kind = binding_kind::sampled_image, .resource = resource_id::velocity, .access = binding_access::read, .sampler = sampler_hint::gbuffer},
        {.binding = 3, .owner = binding_owner::own, .kind = binding_kind::sampled_image, .resource = resource_id::gbuffer_depth, .access = binding_access::read, .sampler = sampler_hint::gbuffer},
        {.binding = 4, .owner = binding_owner::own, .kind = binding_kind::storage_image, .resource = resource_id::ml_resolve, .access = binding_access::write},
    }};

    /// @brief the images the temporal resolve moves, in the order its record() indexes them
    ///
    /// Its five OWN bindings include the two G-buffer targets it reads, so those are its transitions too: the
    /// pass is the first sampler of this image's depth and velocity this frame (see the stage's prepare).
    inline constexpr std::array<barrier_image, 4> megalights_temporal_barriers = {{
        {.resource = resource_id::ml_resolve, .element = 0},
        {.resource = resource_id::ml_history, .element = 0},
        {.resource = resource_id::gbuffer_depth, .element = 0},
        {.resource = resource_id::velocity, .element = 0},
    }};

    /**
     * @brief the temporal resolve's declaration: a half-resolution compute dispatch over its own bindings
     *
     * The five bindings above are everything its shader declares, so it declares nothing shared and has
     * nothing for the frame to publish on its behalf. Its push block is the projection's linearization pair,
     * the three accumulation bounds and the extents.
     */
    inline constexpr pass_io megalights_temporal_io = {
        .name = "megalights_temporal",
        .bindings = megalights_temporal_bindings,
        .targets = {},
        .barrier_images = megalights_temporal_barriers,
        .push = push_block{.offset = 0, .size = 32, .stages = stage_flag::compute},
    };

    /**
     * @brief the image the ray-traced shadow pass rewrites, and the only resource it has to name
     *
     * `rt_shadow` is per FRAME SLOT rather than per swapchain image (the rays are traced once per frame, not once
     * per presented image), so the host resolves this one from the frame's slot - the declaration says WHICH
     * resource, and the element is 0 because there is one family entry to choose from there. It is a storage
     * image the pass writes (UNDEFINED -> GENERAL) and the lighting stage samples (GENERAL -> SHADER_READ), both
     * transitions recorded by the pass itself.
     */
    inline constexpr std::array<barrier_image, 1> rt_shadow_barriers = {{
        {.resource = resource_id::rt_shadow_visibility, .element = 0},
    }};

    /**
     * @brief the ray-traced sun shadow pass's declaration: a full-resolution compute dispatch, sharing everything
     *
     * The traced-compute shape, with the frame's resolution instead of half: its shaders reach the frame's
     * shared resources (the camera, the light UBO, the top level structure at binding 16, the G-buffer surface
     * each ray starts from) through the heap, it owns no descriptor at all, and it names the one image it
     * rewrites through `barrier_images`. Its push block is the camera's inverse view-projection and four
     * ray-offset terms.
     */
    inline constexpr pass_io rt_shadow_io = {
        .name = "rt_shadow",
        .bindings = {},
        .targets = {},
        .barrier_images = rt_shadow_barriers,
        .push = push_block{.offset = 0, .size = 80, .stages = stage_flag::compute},
    };

    /**
     * @brief the two buffers the clustered-light sort writes, and the ONLY resources it has to name
     *
     * They are bindings 11 and 12 of the frame's shared scene resources - so this pass reaches them through the
     * heap and declares no binding of its own - but a pass's writes are not visible to the fragment stages
     * reading them later in the same submission without a BUFFER memory barrier, and only the writer can place
     * it. That is what `pass_io::barrier_buffers` exists for (see `barrier_buffer`), and this declaration is why
     * the field was added: before it, a pass could name an image it moves and had no way to name a buffer it
     * moves.
     *
     * Both are per FRAME SLOT (the slot's count and index arrays), which the host resolves from the frame.
     */
    inline constexpr std::array<barrier_buffer, 2> cluster_barriers = {{
        {.resource = resource_id::cluster_counts, .element = 0},
        {.resource = resource_id::cluster_indices, .element = 0},
    }};

    /**
     * @brief the clustered-light sort's declaration: a one-dimensional compute dispatch over the cluster grid
     *
     * It has no push block at all (the shader reads the light UBO and writes the cluster buffers through the
     * frame's shared resources), no own binding, no target and no image to transition - the whole declaration is
     * "the two buffers I write". Its dispatch size is neither the frame's nor half of
     * it: it is `tiles_x * tiles_y * slices`, which is why its behaviour declares `extent_rule::none` and the
     * host hands the count over in the pass's frame instead.
     */
    // constinit for the same cl 19.44 ICE as scene_io above (a disengaged std::optional in a constexpr global)
    inline constinit pass_io const cluster_io = {
        .name = "cluster",
        .bindings = {},
        .targets = {},
        .barrier_images = {},
        .barrier_buffers = cluster_barriers,
        .push = std::nullopt,
    };

    /// @brief the resource the deferred lighting stage RENDERS INTO, by declaration
    inline constexpr std::array<render_target, 1> deferred_targets = {{
        {.resource = resource_id::scene_color, .element = 0, .kind = target_kind::color},
    }};

    /**
     * @brief the deferred lighting stage's declaration: a fullscreen triangle that shades every pixel
     *
     * Every binding it uses is one of the frame's SHARED resources (the scene's camera, IBL, light
     * UBO and shadow map; the G-buffer's three surface targets, depth and velocity), so it declares
     * no binding of its own - the shape a traced compute pass has, with one render target
     * instead of a compute dispatch.
     *
     * THE TARGET IS A RECORDED DEVIATION: it names `scene_color`, and the host hands over whichever image the frame
     * actually lights - `scene_color` while the TAA resolve runs and `hdr` when it does not (the accessor the
     * renderer always used). A `render_target` names one resource today, and a "frame-dependent target" is a
     * framework decision this pass does not get to make on its own; the deviation is written down here rather than hidden.
     *
     * The push block is 92 bytes - a mat4, a vec4 (the SSAO knobs) and three floats (the render mode, whether the
     * stochastic punctual lighting pass answered this frame's punctual lights) -
     * and the attachment is LOADed, because the lighting ADDS to the emissive the G-buffer pass already wrote (which
     * is also why the pass builds its pipeline with the additive blend).
     */
    inline constexpr pass_io deferred_io = {
        .name = "deferred",
        .bindings = {},
        .targets = deferred_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = 88, .stages = stage_flag::fragment},
    };

    // =============================================================================================
    // THE POST CHAIN - the composite and the bloom chain's four levels
    // =============================================================================================

    /// @brief the push block the whole post chain shares, in bytes; its shape is `deren.vulkan.pass.post`'s
    /// @note 32 AND NOT 28 SINCE THE ARTICLE'S LUT WEIGHT JOINED IT (`post_push_constants::lut_weight`), the fourth
    ///       float of the FXAA lanes' row. The shader's `PostPush` declares the same field in the same place - the note
    ///       there is explicit that the layout up to the three heap indices has to match the CPU struct, because a
    ///       block that stopped short reads its lanes out of the wrong members.
    inline constexpr uint32_t post_push_bytes = 32;

    /**
     * @brief the bloom chain's four levels, as ONE declaration per level
     *
     * WHY FOUR DECLARATIONS AND NOT ONE PARAMETERIZED PASS: each of the four stages renders into a DIFFERENT
     * element of the bloom family and reads the level before it, so the LEVEL is the pass boundary. Each entry
     * here is one pass's whole I/O:
     *
     *  * `targets` is the level it writes (element `level` of the `bloom` family, which the schema declares with
     *    four elements - see its `count`);
     *  * `barrier_images` is the level it READS, which is also the image the pass has to move to a sampled layout
     *    before it renders. Level 0 reads the HDR target instead, and that transition is deliberately NOT here:
     *    the host owns it, because the composite reads HDR too and because it is needed on the frames the whole
     *    bloom chain is skipped (see `post_composite_io`);
     *  * the LAST level's pass also hands its own output back to a sampled layout, from its target - the
     *    composite samples all four levels, and level 3 has no successor to do it (the writer's hand-back the GI
     *    chain's passes also use).
     *
     * The push block is the post chain's ONE block (`post_push_bytes`): the shader declares it whole, every stage
     * writes a different `mode` into it, and the bytes the stages do not read are pushed with the same defaults
     * they always were - the prefilter passes `mode = 0`, the three downsamples `mode = 1`.
     */
    /// the levels' named target and source lists: a `pass_io` holds SPANS, so every list it names has to outlive
    /// it (a brace-initialized array inside the initializer would be a temporary the span dangles on)
    inline constexpr std::array<render_target, 1> post_bloom_0_target = {{render_target{.resource = resource_id::bloom, .element = 0, .kind = target_kind::color}}};
    inline constexpr std::array<render_target, 1> post_bloom_1_target = {{render_target{.resource = resource_id::bloom, .element = 1, .kind = target_kind::color}}};
    inline constexpr std::array<render_target, 1> post_bloom_2_target = {{render_target{.resource = resource_id::bloom, .element = 2, .kind = target_kind::color}}};
    inline constexpr std::array<render_target, 1> post_bloom_3_target = {{render_target{.resource = resource_id::bloom, .element = 3, .kind = target_kind::color}}};
    inline constexpr std::array<barrier_image, 1> post_bloom_1_source = {{barrier_image{.resource = resource_id::bloom, .element = 0}}};
    inline constexpr std::array<barrier_image, 1> post_bloom_2_source = {{barrier_image{.resource = resource_id::bloom, .element = 1}}};
    inline constexpr std::array<barrier_image, 1> post_bloom_3_source = {{barrier_image{.resource = resource_id::bloom, .element = 2}}};

    inline constexpr std::array<pass_io, 4> post_bloom_io = {{
        {.name = "post_bloom_0",
         .bindings = {},
         .targets = post_bloom_0_target,
         .barrier_images = {},
         .barrier_buffers = {},
         .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment}},
        {.name = "post_bloom_1",
         .bindings = {},
         .targets = post_bloom_1_target,
         .barrier_images = post_bloom_1_source,
         .barrier_buffers = {},
         .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment}},
        {.name = "post_bloom_2",
         .bindings = {},
         .targets = post_bloom_2_target,
         .barrier_images = post_bloom_2_source,
         .barrier_buffers = {},
         .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment}},
        {.name = "post_bloom_3",
         .bindings = {},
         .targets = post_bloom_3_target,
         .barrier_images = post_bloom_3_source,
         .barrier_buffers = {},
         .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment}},
    }};

    /// @brief what the composite RENDERS INTO by declaration: the swapchain (the FXAA-off case; see its comment)
    inline constexpr std::array<render_target, 1> post_composite_targets = {{render_target{.resource = resource_id::swapchain_image, .element = 0, .kind = target_kind::color}}};

    /**
     * @brief the composite's declaration: HDR plus the weighted bloom levels, tonemapped to the display
     *
     * It renders into the SWAPCHAIN by declaration and into the LDR image on the frames FXAA runs - a RECORDED
     * DEVIATION, the same one `deferred_io` records for `scene_color` and for the same reason: `render_target`
     * names one resource, and the composite's target decides its PIPELINE too (the swapchain's format against the
     * LDR image's R16F), so the host hands over both together.
     *
     * NO BARRIER IMAGE IS DECLARED, and that is the host's half rather than an omission: the HDR target's
     * transition to a sampled layout happens before the chain on EVERY frame (the composite reads it whether or
     * not bloom runs, and the bloom chain's prefilter reads it as well), so it has exactly one owner and that
     * owner has to be the frame loop. The four bloom levels arrive already sampled - the levels' own passes
     * moved them, or the host's off path did on a frame the chain was skipped.
     */
    inline constexpr pass_io post_composite_io = {
        .name = "post_composite",
        .bindings = {},
        .targets = post_composite_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment},
    };

    /// @brief what the FXAA pass RENDERS INTO by declaration: the swapchain, which it is the last writer of
    inline constexpr std::array<render_target, 1> fxaa_targets = {{render_target{.resource = resource_id::swapchain_image, .element = 0, .kind = target_kind::color}}};

    /// @brief the image the FXAA pass reads and therefore has to move to a sampled layout: the composite's LDR output
    inline constexpr std::array<barrier_image, 1> fxaa_barriers = {{barrier_image{.resource = resource_id::ldr, .element = 0}}};

    /**
     * @brief the FXAA pass's declaration: the gamma-encoded LDR image -> the anti-aliased swapchain
     *
     * It reads the SAME shared post resources the composite writes (the composite writes the LDR image through
     * binding 5 and FXAA reads it back through it, which is why FXAA cannot be folded into
     * `post.frag`: a descriptor may not name the image the pipeline is rendering into, and that would be a
     * different statically-used binding set).
     *
     * ITS INPUT IS DECLARED, unlike the composite's, and the difference is the frame: the LDR image is written by
     * the composite and read here, so this pass is the one that moves it - there is no frame where it has to be
     * moved and no pass runs, because a frame without FXAA never touches it at all.
     *
     * The push block is the chain's own (`post_push_bytes`): FXAA is mode 3 of the same shader's block.
     */
    inline constexpr pass_io fxaa_io = {
        .name = "fxaa",
        .bindings = {},
        .targets = fxaa_targets,
        .barrier_images = fxaa_barriers,
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = post_push_bytes, .stages = stage_flag::fragment},
    };

    /// @brief the upscale pass's own push block, in bytes: EASU's four `float4` constants and the two scalar
    ///        lanes; its shape is `deren.vulkan.pass.upscale`'s `push_constants`
    /// @note 72, and NOT the 80 bytes the shader's block adds up to: the framework appends the heap index lanes
    ///       AFTER these bytes (`runtime::push_stage_block`), so the pass declares only what it composes. Get
    ///       this wrong in the other direction - declare the lanes as part of the pass's own size - and they
    ///       land past the fields the shader reads them from, which is a silent wrong-image rather than a
    ///       validation error.
    inline constexpr uint32_t upscale_push_bytes = 72;

    /// @brief what the UPSCALE pass RENDERS INTO by declaration: the swapchain, i.e. the OUTPUT image whose
    ///        extent is the one thing this pass asks for that is not the frame's
    inline constexpr std::array<render_target, 1> upscale_targets = {{render_target{.resource = resource_id::swapchain_image, .element = 0, .kind = target_kind::color}}};

    /// @brief the image the upscale pass reads and therefore has to move to a sampled layout: the composite's LDR
    ///        output - the same image FXAA reads, which is why the two passes are mutually exclusive
    inline constexpr std::array<barrier_image, 1> upscale_barriers = {{barrier_image{.resource = resource_id::ldr, .element = 0}}};

    /**
     * @brief the UPSCALE pass's declaration: the display-referred LDR image -> the presented swapchain, by a
     *        linear (bilinear) resample
     *
     * WHY IT EXISTS: `core::render_extent()` is the RENDER chain's extent, so at `[render] render_scale < 1.0`
     * every image the chain renders into is created smaller while the swapchain keeps the output size. Before
     * this declaration the scaled frame was simply presented in the TOP-LEFT QUADRANT of that output - the
     * scene was shaded, at a lower resolution, and nothing resolved it. This pass is that resolve.
     *
     * THE TARGET IS THE SWAPCHAIN AND ITS EXTENT IS THE OUTPUT, and that pairing is the whole mechanism: this
     * is the one resource `runtime::resolve_resource_extent` answers with the swapchain's extent rather than
     * the frame's, so a pass declaring `extent_rule::resource` over it gets a viewport covering the presented
     * image while it samples an image at the render extent (`behaviour::resync_viewport` sets both).
     *
     * IT IS FXAA'S SIBLING AND ITS EXCLUSIVE ALTERNATIVE, and the exclusion is a correctness rule rather than
     * tidiness: both read the same LDR image, both want to be the frame's last writer, and FXAA's edge filter
     * is defined at the resolution it filters - so on a frame this pass resolves, FXAA must not run at all
     * (its result would be resampled away). `runtime::post_fxaa_active` carries that exclusion, and the
     * composite's target choice, the frame's `write_ldr` and the overlay's owner all read the two predicates
     * from `frame_facts` so nothing can disagree.
     *
     * The push block is this pass's OWN and is 72 bytes - EASU's four constants (4 x float4) and the two scalar
     * lanes (which filter, and the display transfer, whose value follows from the surface's format exactly as
     * FXAA's does). It is deliberately NOT `post_push_bytes`: the resolve samples one image and filters it, so
     * every lane of the post chain's block would be a dead field. The shader's block is this size PLUS the two
     * heap index lanes the framework appends (the LDR image is a per-swapchain-image heap slot the shader names
     * itself - see shaders/upscale.slang).
     */
    inline constexpr pass_io upscale_io = {
        .name = "upscale",
        .bindings = {},
        .targets = upscale_targets,
        .barrier_images = upscale_barriers,
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = upscale_push_bytes, .stages = stage_flag::fragment},
    };

    /// @brief what the debug view RENDERS INTO: the HDR target, which is the image it actually writes - so unlike
    ///        the deferred stage's and the composite's, this declaration carries no deviation
    inline constexpr std::array<render_target, 1> gbuffer_debug_targets = {{render_target{.resource = resource_id::hdr, .element = 0, .kind = target_kind::color}}};

    /// @brief the four images it moves to a sampled layout: the three stored surface targets and the motion vectors
    inline constexpr std::array<barrier_image, 4> gbuffer_debug_barriers = {{barrier_image{.resource = resource_id::gbuffer_targets, .element = 0},
                                                                             barrier_image{.resource = resource_id::gbuffer_targets, .element = 1},
                                                                             barrier_image{.resource = resource_id::gbuffer_targets, .element = 2},
                                                                             barrier_image{.resource = resource_id::velocity, .element = 0}}};

    /**
     * @brief the G-buffer debug view's declaration: the stored surface, one channel at a time, into the HDR target
     *
     * Everything it READS is one of the frame's shared G-buffer resources (the three targets, the depth, the
     * velocity), so it declares no binding of its own - and the DEPTH is deliberately not among its barrier
     * images: its old layout depends on whether the G-buffer instance rendered this frame, which is shared
     * per-image bookkeeping the host owns (the same hand-back the deferred stage's frame carries as a callback).
     * The velocity target IS declared, because the debug view runs INSTEAD of the lighting stage and is therefore
     * the only stage that hands it to a sampler on those frames.
     *
     * The push block is 16 bytes - the channel, the two projection terms the depth channel linearizes with, and the
     * motion gain - and all four values are the frame's (the renderer's knob and camera).
     */
    inline constexpr pass_io gbuffer_debug_io = {
        .name = "gbuffer-debug",
        .bindings = {},
        .targets = gbuffer_debug_targets,
        .barrier_images = gbuffer_debug_barriers,
        .barrier_buffers = {},
        .push = push_block{.offset = 0, .size = 16, .stages = stage_flag::fragment},
    };

    /// @brief the layers the shadow pass renders into: a RUN of elements, one rendering instance per cascade
    ///
    /// THIS USED TO BE A RECORDED DEVIATION, and the vocabulary that closed it is `render_target::count`: the
    /// declaration named the image's FIRST layer and the host handed over the layers this frame has
    /// (`runtime::resolve_shadow_pass`). The deviation was forced by the framework - the validator refuses a
    /// second DEPTH target, because a rendering instance has one depth attachment - but it was never a fact about
    /// the shadow pass: that pass renders 1..`max_shadow_cascades` LAYERS of ONE array image, one instance per
    /// cascade, which is exactly a run of elements. The count is the schema's own four (the family's count), and
    /// the FRAME caps it: `ensure_shadow_resources` allocates the layers the cascade knob asks for, and only
    /// those layers are published - so a one-cascade frame resolves one target and a three-cascade frame three.
    inline constexpr std::array<render_target, 1> shadow_targets = {{render_target{.resource = resource_id::shadow_map, .element = 0, .kind = target_kind::depth, .count = 4}}};
    /**
     * @brief the shadow pass's declaration: the scene's depth from the light, one cascade at a time
     *
     * The PUSH BLOCK is four bytes at offset 96 - the cascade index, pushed at `deren::vulkan::scene_cascade_push_offset`
     * (`scene_push_constant_size`), which is where the scene's own push block ends: this pass draws the scene's
     * leaves through the SCENE pipeline layout, so its push shares that layout's range rather than owning one. The
     * literal is checked against the constant where both are visible (`deren.vulkan.pass.shadow`'s static_assert), the
     * same "two copies, one size" rule the other pass blocks follow.
     *
     * NO BARRIER IMAGES: each layer is transitioned to a depth attachment immediately before the instance that
     * renders it, so the pass reaches every image it moves through `targets` - the shape the composite and the
     * debug view have. The targets are a RUN (`shadow_targets`), which is what makes that reach enough: the run
     * is however many layers this frame's map has, in cascade order.
     */
    inline constexpr pass_io shadow_io = {
        .name = "shadow",
        .bindings = {},
        .targets = shadow_targets,
        .barrier_images = {},
        .barrier_buffers = {},
        .push = push_block{.offset = 108, .size = 4, .stages = stage_flag::vertex | stage_flag::fragment},
    };
} // namespace deren::vulkan::render_resource
