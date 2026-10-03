module;

#include <cstddef>         // std::byte: the push block is bytes now (see push_block below)
#include <span>            // std::span: what the endpoint takes
#include <vulkan/vulkan.h> // VkCommandBuffer / VkPipelineLayout handle typedefs only

export module deren.vulkan.render_environment;

export import deren.vstd;
import deren.promise.rhi;

/**
 * @defgroup vulkan_render_environment Render Environment
 * @ingroup vulkan_runtime
 * @brief per-recording-session render state: the session's command buffer, which pipeline is
 * bound and how to bind others.
 *
 * One instance exists per parallel recording worker (each sub_render_task builds its own, per
 * frame), so the command buffer and the "currently bound pipeline" state are always
 * thread-local - workers never share them. The environment carries the session's command
 * buffer, its default pipeline and the injected binder, and lets a primitive's draw() bind the
 * pipeline it wants through that callback; the real vkCmdBindPipeline lives in the callback
 * (owned by the runtime, which captures its pipeline cache). The environment itself holds no
 * Vulkan objects - only the handle typedefs it needs to forward them.
 *
 * Bindings are deduplicated: bind_default() / bind_pipeline() no-op when the requested
 * pipeline is already the bound one, so consecutive draws sharing a pipeline do not re-bind
 * (the same saving the old per-pipeline grouping gave, now per session and per draw).
 */
namespace deren::vulkan {
    /**
     * @ingroup vulkan_render_environment
     * @brief per-recording-session render state handed to primitive::draw().
     *
     * Members are set by the recording site (the runtime) before the session's leaves draw:
     * - command_buffer: the buffer being recorded into (each session owns its own, so draw()
     *   needs no separate command-buffer parameter)
     * - default_name: this pass's default pipeline ("pbr" for the main pass, the shadow
     *   pipeline for the shadow pass) - the fallback a default-semantics primitive wants
     * - bind: injected binding action; takes the pipeline name and records the real
     *   vkCmdBindPipeline (+ any per-bind dynamic state) on this session's command buffer
     * - push_owner / push_block: how a draw sends its push block, which travels as DATA
     *   through vkCmdPushDataEXT because no heap-native pipeline has a layout
     * - bound: the pipeline name currently bound in this session (empty = nothing bound yet)
     * - two_sided: force two-sided rasterization on this session (the shadow pass; see below)
     *
     * A "default-semantics" primitive (normal / instanced / static draw) does:
     * @code
     * if (!env.in_default_pipeline()) { env.bind_default(); }
     * // ... bind geometry, push its block via env.push_block, draw on env.command_buffer ...
     * @endcode
     * A custom primitive stores its pipeline name and does:
     * @code
     * env.bind_pipeline(this->pipeline_name);
     * @endcode
     */
    export struct render_environment {
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;                        // session's recording target
        std::string_view default_name = {};                                     // this pass's default
        std::function<void(VkCommandBuffer, std::string_view)> bind = {};       // injected binder
        std::function<void(VkCommandBuffer, VkBool32)> set_depth_write_fn = {}; // injected depth-write setter
        VkPipelineLayout layout = VK_NULL_HANDLE;                               // shared scene layout
        /**
         * HOW A DRAW SENDS ITS PUSH BLOCK, now that no pipeline has a layout (see
         * `pass::resolved_io::push_endpoint`, which is the same pair for a pass).
         *
         * `vkCmdPushConstants` needs a layout to push to, and every heap-native pipeline is created with
         * `layout = VK_NULL_HANDLE`, so a stage block travels as DATA through `vkCmdPushDataEXT` instead - read
         * by the shader exactly as it always read push constants. A primitive's draw() has no access to the
         * heap, and this session object is what it does have, so the runtime fills these two where it fills
         * `layout`, and the endpoint appends the heap's two indices (frame slot, swapchain image) to the block.
         *
         * NOT a `std::function`: this is copied into every worker's session every frame, and a raw owner plus a
         * function pointer is what a per-draw call can afford.
         */
        void* push_owner = nullptr;
        bool (*push_block)(void* owner, VkCommandBuffer command_buffer, std::span<std::byte const> bytes, uint32_t extra_lane) = nullptr;
        /**
         * MESH RECORDING, in the three pieces a draw without an input assembler needs (docs/mesh_shaders.md
         * step 1). A mesh pipeline replaces the vertex stage AND the input assembler, so `vkCmdDrawIndexed`
         * - which names a vertex binding, an index buffer and a first index - has no mesh equivalent: the
         * dispatch is `vkCmdDrawMeshTasksEXT(groupCountX, groupCountY, groupCountZ)` and the geometry has to
         * reach the shader as DATA instead.
         *
         * These are set only on a session whose pass draws with a mesh pipeline (`mesh_stage`), and every one
         * of them is a raw owner/function-pointer pair for the same reason `push_block` is: this struct is
         * copied into every worker's session every frame, once per draw.
         *
         * - `buffer_address`: the device address of a bound buffer. The primitive knows WHICH buffer and the
         *   runtime knows the device, and `vkGetBufferDeviceAddress` is the latter's to call.
         * - `push_at`: a push at a raw block offset, for the geometry lanes that sit past the block
         *   `push_block` sends (which appends the heap indices at its own end and cannot place them).
         * - `draw_mesh_tasks`: the dispatch itself. The entry point is not exported by the loader's import
         *   library (`vkCmdDrawMeshTasksEXT` is an extension command), so the runtime resolves it once and
         *   answers null when the device has none - which is the answer "draw nothing" rather than a crash.
         */
        bool mesh_stage = false;
        /**
         * WHETHER THIS SESSION DRAWS MESHLETS (docs/mesh_shaders.md step 3): its leaves are dispatched ONE WORKGROUP
         * PER MESHLET, and the geometry lanes carry the primitive's meshlet run (`meshlet_base` as the lane's first
         * index, `meshlet_count` as its count) instead of the draw's index window - which is what the meshlet entry
         * reads: its own record out of the table, and the buffers' addresses out of the lanes.
         */
        bool meshlets = false;
        /**
         * WHETHER THIS SESSION'S MESHLET RUNS ARE CULLED BY THE HOST BEFORE THE DISPATCH (docs/mesh_shaders.md step 3,
         * the culling's cheapest stage). A meshlet that the frustum rejects still costs a workgroup launch today,
         * because the entry point is what discovers it; with this on, the recording path tests each of the
         * primitive's meshlets against the camera, writes the survivors into this frame's culled table and asks the
         * dispatch for exactly that many workgroups - so a culled meshlet costs nothing at all.
         *
         * THE HOST IS THE CHEAP STAGE HERE, not a fallback for one: it already holds the primitive's meshlets, the
         * draw's model matrix and the camera (the same three things the stage reads), so the test costs no new pass,
         * no command buffer, no ordering rule and no draw list. It is FALSE for the shadow pass on purpose - that
         * pass's frustum is per cascade, and it dispatches its casters per cascade.
         */
        bool meshlet_culled = false;
        /// the camera's `proj * view` as 16 floats, column-major, for the host-side cull; null means the session
        /// cannot cull and falls back to dispatching the whole run, which is always correct
        bool (*meshlet_view_proj)(void* owner, float* out16) = nullptr;
        /// write this frame's compacted run of a primitive's meshlets: @p records are the survivors, @p base is where
        /// the run starts in the table (the primitive's `meshlet_base`), and they go into the frame's own lane of the
        /// culled table. Answers false when the run does not fit, which the caller answers by not culling.
        bool (*meshlet_culled_write)(void* owner, uint32_t base, std::span<std::byte const> records) = nullptr;
        /**
         * WHERE THIS SESSION'S STAGE BLOCK STARTS RECEIVING THE GEOMETRY LANES, in bytes - i.e. the end of the last
         * member the block's earlier pushes covered (see deren::vulkan::primitive's `mesh_geometry_push_offset_*` and
         * `mesh_geometry_lanes_offset`). It is per SESSION rather than a constant because the two blocks in this
         * renderer end differently: the scene's after its heap index lanes (108), the shadow pass's after its
         * cascade lane (112). The lanes themselves are read by the shader at `mesh_geometry_lanes_offset`, which is
         * 112 in both blocks.
         *
         * EVERY draw of a session that sets this pushes its lanes, including the sessions whose pipeline is a
         * VERTEX one: one shader file is one block for every entry, so the vertex entries declare the lanes too and
         * a descriptor-heap pipeline requires every declared byte to be written before the draw. Zero means "this
         * session's stages declare no lanes" (a fullscreen pass's own block, a compute push).
         */
        uint32_t mesh_geometry_push_offset = 0;
        /// The device address of a contract buffer, asked for by the OWNER (the runtime), which is the
        /// side that holds the escape and the `device_address` ability.
        ///
        /// IT TAKES A CONTRACT BUFFER, NOT A `VkBuffer`, AND THAT IS A DELIBERATE NARROWING: the
        /// primitive's buffers are contract handles now (the migration for which this hook was
        /// rewritten), and a hook that demanded a raw handle would have forced every primitive to keep
        /// one - i.e. to keep reaching into the backend for exactly the thing the migration removes. The
        /// `owner` void* is the runtime's own state, and the address comes back through
        /// `native_buffer()` + `device_address::buffer_address()` on that side.
        VkDeviceAddress (*buffer_address)(void* owner, deren::promise::rhi::buffer const& buffer) = nullptr;
        bool (*push_at)(void* owner, VkCommandBuffer command_buffer, uint32_t offset, std::span<std::byte const> bytes) = nullptr;
        bool (*draw_mesh_tasks)(void* owner, VkCommandBuffer command_buffer, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) = nullptr;
        /**
         * THE SAME DISPATCH, WITH ITS COUNTS IN A BUFFER (docs/mesh_shaders.md step 3, second mechanism). `slot` is
         * the primitive's own command record - its `meshlet_base` - so a COMPUTE culling pass can rewrite that
         * record with the counts culling left and the dispatch picks them up with no host change at all. Set beside
         * `draw_mesh_tasks` wherever a mesh session is built; null means the device has no indirect entry point, and
         * the dispatch then goes through the direct call (which the runtime logs once rather than hiding).
         */
        bool (*draw_mesh_tasks_indirect)(void* owner, VkCommandBuffer command_buffer, uint32_t slot, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z) = nullptr;
        std::string_view bound = {}; // currently bound name
        // injected cull-mode setter (core dynamic state since Vulkan 1.3, so one pipeline serves
        // single- and double-sided materials)
        std::function<void(VkCommandBuffer, VkCullModeFlags)> set_cull_mode_fn = {};
        // Session-wide two-sided rasterization. The SHADOW pass sets it, and it is not a nicety: a
        // caster must never be dropped for facing it away from the light. A single-sided wall plane
        // whose only face points into the room (Sponza is full of them) is back-facing as seen from
        // the sun, so a back-face-culled depth pass simply does not record it - and since the camera
        // inside the room sees that same wall's front face, the wall looks perfectly solid while the
        // sunlight pours straight through it. Front-face culling fails on exactly the same geometry,
        // so the depth pass draws both sides (the depth test still keeps the nearest surface).
        bool two_sided = false;
        VkCullModeFlags cull_mode_recorded = VK_CULL_MODE_BACK_BIT;
        bool cull_mode_known = false;
        // depth-write state actually recorded so far. Starts "unknown" (nothing recorded yet):
        // the first set_depth_write() must ALWAYS emit vkCmdSetDepthWriteEnable even when the
        // requested state matches the pipeline default - a dynamic state that is never set is
        // invalid (VUID). known == true once any set has been recorded.
        VkBool32 depth_write_recorded = VK_TRUE;
        bool depth_write_known = false;
        /**
         * WHETHER THE SESSION, RATHER THAN THE LEAF, OWNS THE DEPTH-WRITE STATE.
         *
         * Every primitive's draw() sets its own depth write (`set_depth_write(!this->transparent)`), which is
         * right for the scene, shadow and transparent passes - an opaque surface writes depth and a blended
         * one does not, and that is the material's business. It is WRONG for a pass that must draw the same
         * leaves with the write held off: the leaf would turn it back on a few instructions after the pass
         * turned it off, and the pass's state would be a statement of intent rather than a fact. That is
         * exactly what the outline pass's `ZWrite Off` amounted to before this flag existed.
         *
         * The character-forward pass is the case that needs it (it re-shades the surface the G-buffer pass
         * already recorded, at depth-EQUAL, and must not touch the depth buffer at all). Default false, so
         * every existing session is bit-for-bit what it was.
         */
        bool depth_write_locked = false;

        /** @brief whether the session's default pipeline is the one currently bound */
        [[nodiscard]] bool in_default_pipeline() const noexcept {
            return this->bound == this->default_name;
        }

        /** @brief bind the session's default pipeline when it is not already bound */
        void bind_default() {
            if (!this->in_default_pipeline()) {
                this->bind(this->command_buffer, this->default_name);
                this->bound = this->default_name;
            }
        }

        /** @brief bind @p name when it is not the currently bound pipeline */
        void bind_pipeline(std::string_view const name) {
            if (this->bound != name) {
                this->bind(this->command_buffer, name);
                this->bound = name;
            }
        }

        /**
         * @brief record the depth-write state when it differs from what is already recorded
         * @param enabled true = depth writes on (opaque passes); false = off (transparent
         *        draws, which must not occlude later back-to-front geometry)
         * @note a request from a LEAF is IGNORED when the session has locked the state (see
         *       depth_write_locked): a pass that must hold the write off cannot have it turned back on by
         *       the leaves it is drawing. The session's own first set is the one that counts, which is why
         *       the lock is meant to be taken AFTER that set.
         */
        void set_depth_write(bool const enabled) {
            if (this->depth_write_locked) {
                return;
            }
            VkBool32 const want = enabled ? VK_TRUE : VK_FALSE;
            if (!this->depth_write_known || this->depth_write_recorded != want) {
                this->set_depth_write_fn(this->command_buffer, want);
                this->depth_write_recorded = want;
                this->depth_write_known = true;
            }
        }

        /**
         * @brief force FRONT-face culling for the next group of leaves, whatever their own materials say
         *
         * THE INVERTED HULL (the article's ① 描边) is the first caller and the reason this exists: a hull is drawn
         * with its FRONT faces culled, so that only the back faces survive the depth test and the result is a ring
         * OUTSIDE the silhouette rather than a second shaded surface over it. No pipeline state can state that on
         * its own here, because every leaf's draw() calls set_cull_mode() with its own `doubleSided` flag a few
         * instructions later - the exact shape `depth_write_locked` was added for, one state over.
         *
         * `false` IS THE WHOLE DEFAULT and it is not a fallback: a session that never sets it reaches
         * `set_cull_mode`'s original expression and records the identical command stream it did before this field
         * existed, which is what keeps every other pass and group unchanged.
         */
        bool forced_cull_front = false;

        /**
         * @brief record the cull mode for a material, honoring the session's two-sided flag
         * @param two_sided_material the primitive's own glTF `doubleSided` flag (renders both faces)
         * @note callers pass their material flag and let the session decide: the shadow pass forces
         *       VK_CULL_MODE_NONE regardless (see two_sided), the main pass keeps back-face culling
         *       for single-sided materials. Deduplicated like set_depth_write(), so consecutive
         *       leaves sharing a cull mode emit the state once.
         * @note `forced_cull_front` outranks both session and material (see its own note): it is how the inverted
         *       hull states Cull Front for a group of leaves that would otherwise each put BACK/NONE back.
         */
        void set_cull_mode(bool const two_sided_material) {
            VkCullModeFlags const want = this->forced_cull_front
                                             ? VK_CULL_MODE_FRONT_BIT
                                             : ((this->two_sided || two_sided_material) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
            if (!this->cull_mode_known || this->cull_mode_recorded != want) {
                this->set_cull_mode_fn(this->command_buffer, want);
                this->cull_mode_recorded = want;
                this->cull_mode_known = true;
            }
        }
    };
} // namespace deren::vulkan
