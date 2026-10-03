// module version: 0.15.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/pass.cppm
 * @brief What a pass IS: its declaration, the behaviour that says how to call it, and the runner that does.
 * @defgroup vulkan_pass Frame Pass Framework
 *
 * THE PROBLEM THIS REMOVES, measured rather than asserted. Adding one render pass to this renderer today costs,
 * at the time: a push-constant struct, a pipeline, a
 * `gpu_mark_id` enumerator and its call, a
 * `render_features` field, two feature strings, a viewport-resync line, a flag reset in the constructor AND in
 * `on_swapchain_recreated`, and a destroy in the destructor. This module is the shape that replaces that list
 * with "declare the I/O, implement record, name it in a stage".
 *
 * WHERE EACH FACT LIVES, because the point of this layer is that no fact lives twice:
 *
 *  * `deren.vulkan.render_resource` owns WHAT EXISTS and WHAT A PASS USES (pure data, ctest-tested, no device);
 *  * this module owns HOW A PASS IS CALLED (behaviour), WHAT A PASS IS GIVEN (`resolved_io`), and the ORDER a
 *    stage's passes run in (declaration order, never container order);
 *  * `deren.vulkan.core` keeps owning every image; `deren.vulkan.runtime` keeps owning every pipeline. Neither moves.
 *
 * THE ONE INTERFACE A PASS HAS IS `resolved_io`: the handles its own declaration asked for, indexed by its own
 * binding numbers. The runtime resolves them FROM the declaration, so a pass cannot reach a resource it did not
 * declare - which is what makes the dependency graph checkable instead of aspirational. `pass_host` is
 * deliberately NOT that interface: it is what the RUNNER talks to. Keeping the host away from passes is what
 * stops this from growing into a context object that hands out whatever the newest pass happens to want.
 *
 * WHAT IT IS NOT: not an allocator, not a barrier generator, not an ordering engine. The frame's stage order is
 * declared (it is the recording spine, and `gpu_mark_id`'s positional contract lives on it), and barriers stay
 * hand-written until the layer that derives them has its own measured step.
 */

module;

#include <algorithm> // std::max in the extent rule
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass;

import deren.vulkan.render_resource;
import deren.vulkan.render_resource.shared;

export import deren.vulkan.frame_constants; // the per-frame constants a pass reads (see resolved_io::constants)

export namespace deren::vulkan::pass {

    using render_resource::resource_id;

    /// forward: `pass_host` names it in a callback, and the class itself names `pass_host`
    class frame_pass;
    /// forward: `resolve_context` holds one, and it is defined with the resource table below (section 5)
    class resource_table;

    // =============================================================================================
    // 1. HOW A PASS IS CALLED - one small closed vocabulary, shared by every pass of that shape
    // =============================================================================================

    /// @brief where a pass's dispatch or draw extent comes from
    enum class extent_rule : uint8_t {
        full,     // the frame's extent
        half,     // half the frame's extent - the stochastic chain's resolution
        resource, // the extent of the resource named in `extent_of` (the probe grid is not the frame's size)
        /**
         * The pass sizes its OWN work and `resolved_io::extent` stays empty.
         *
         * Added for the clustered-light sort, whose dispatch is one-dimensional over `tiles_x * tiles_y *
         * slices` - a number derived from the frame's extent but equal to neither it nor half of it, so all
         * three rules above would have been a claim the host could not honour. What such a pass works at is its
         * own data, and the host hands that over in the pass's frame (see `scene_frame` for the same split).
         */
        none,
    };

    /// @brief the shape of the work: what the runner must do AROUND the pass, not what the pass computes
    enum class behaviour_kind : uint8_t {
        compute,     // a dispatch: the pass records it, the extent and the workgroup size are declared
        ray_tracing, // a vkCmdTraceRaysKHR: the pass records it, the launch dims ARE the declared extent, and
                     // the runner's half is the same bind - only the bind POINT differs (see record_stage)
        fullscreen,  // one fullscreen triangle per target
        graphics,    // a draw per piece of scene content (a leaf, a light, a caster)
        instanced,   // one draw per instance - the shadow cascades' shape
        // THE THREE GRAPHICS KINDS ALL MEAN THE SAME THING TO THE RUNNER, and that is the point this layer
        // reached: the PASS opens the rendering instance over the targets it declared (the load op and the
        // clear value are its knowledge, not the runner's), and the runner's half is the part a pass cannot
        // forget - binding the pipeline and, for a pass that asked, setting the viewport and scissor from the
        // extent its declaration produced. What differs between the three is how many draws the pass issues
        // and from where its draw list comes, which is the pass's business and no other layer's.
    };

    /**
     * @brief how this pass wants to be invoked
     *
     * `group_size` is a DECLARED FACT, not a convenience: it must equal the shader's `local_size_x/y/z`, and
     * that equality is exactly the kind of thing this layer exists to be able to check later against the
     * SPIR-V (the reflection parser is already in `deren.vulkan.core.pipeline:spirv_parser`). Today the same number
     * lives in a shader and in a dispatch call, in two files, with nothing tying them together.
     */
    struct behaviour {
        behaviour_kind kind = behaviour_kind::compute;
        uint32_t group_size_x = 8;
        uint32_t group_size_y = 8;
        uint32_t group_size_z = 1;
        extent_rule extent = extent_rule::full;
        resource_id extent_of = resource_id::none; // read only when extent == resource
        /**
         * WHICH ELEMENT of `extent_of` the extent is, read only when `extent == resource` and `extent_of` names
         * a family with more than one image.
         *
         * ADDED FOR THE BLOOM CHAIN, and the reason is a measured shape rather than symmetry: its four levels are
         * a 4-element family whose sizes are `max(1, swap >> (level + 1))`, so a rule that can name the family but
         * not the level cannot describe the pass's own target - and the two alternatives are both dishonest. A
         * new `extent_rule` per level would be four enumerators for one formula, and `extent_rule::none` means the
         * pass sizes its OWN work and `resolved_io::extent` stays empty, which would make the declaration claim
         * the host had nothing to do with the size. The mapping from (resource, element) to an extent is the
         * HOST's - only it knows its own images - so this field is carried, not interpreted here.
         */
        uint16_t extent_of_element = 0;
        /**
         * The pipelines this pass records with, BY NAME, in the order it will use them.
         *
         * NAMES RATHER THAN A BUILD REQUEST, decided deliberately: `deren.vulkan.runtime` already owns the pipelines
         * and already keys them by name (`make_pipeline` / `set_default_pipeline` / `get_pipeline`), so a pass
         * naming what it needs is the existing mechanism rather than a new one - and it keeps `deren.vulkan.pipelines`
         * where it is, with the heap-native pipelines built there. The host resolves the
         * names into `resolved_io::pipelines`, in this order, so `pipelines[i]` is the i-th name here.
         */
        std::span<std::string_view const> pipelines = {};
        /**
         * Whether the runner must set the viewport and scissor from `resolved_io::extent` before this pass.
         *
         * THIS FIELD EXISTS BECAUSE OF A MEASURED HAZARD: the viewport resync is a hand-maintained list of
         * pipelines in `update_pass_geometry` today, and dropping a pipeline from it makes a post pass set a
         * zero-width viewport. A behaviour kind that cannot forget it is the fix, and `fullscreen` is the kind
         * that sets it.
         */
        bool resync_viewport = false;
    };

    /// @brief the frame a pass is being recorded in: the two counters this project has confused before
    struct frame_identity {
        uint32_t image_index = 0; // per-swapchain-image resources (GI, TAA, the G-buffer)
        uint32_t slot = 0;        // per-frame-slot resources (shadow maps, the light/camera buffers)
        /// how many swapchain images THIS generation has, which is not the same number as `image_index` and
        /// is what a pass that owns per-image bindings sizes it from. The passes that own them need it,
        /// and it is the kind of fact that used to be reachable only from inside `deren.vulkan.runtime`
        uint32_t image_count = 0;
        VkExtent2D extent = {0, 0};
    };

    /// @brief one resolved own binding: the handles this binding's resource actually is
    struct resolved_binding {
        VkImageView view = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        /**
         * The image BEHIND @c view, because a descriptor takes a view and a BARRIER takes an image.
         *
         * This field exists because a pass that owns a per-image family needs it: the transitions and clears
         * its own nine images (same-layout storage barriers for the propagation, a clear when the global
         * lighting changed), and a pass that has only views cannot name them. It is resolved from the same
         * declaration element as the view, so a pass still reaches nothing it did not declare.
         */
        VkImage image = VK_NULL_HANDLE;
    };

    /**
     * @brief how many own bindings one pass may resolve (the most any pass declares today)
     */
    inline constexpr uint32_t max_own_bindings = 16;
    /// @brief how many pipelines one pass may name (the post chain's five are the most today)
    inline constexpr uint32_t max_pass_pipelines = 8;
    /// @brief how many images one pass may render into (one fullscreen pass has one; the deferred scene has five,
    ///        and the shadow pass's cascade RUN is the widest single declaration at four)
    inline constexpr uint32_t max_render_targets = 8;
    /// @brief how many images one pass may declare for its own transitions (the most any pass declares today)
    inline constexpr uint32_t max_barrier_images = 16;
    /// @brief how many BUFFERS one pass may declare for its own ordering (the cluster sort's two are the only ones today)
    inline constexpr uint32_t max_barrier_buffers = 8;
    /// @brief the largest push block a pass may declare: the 128 bytes Vulkan guarantees
    inline constexpr uint32_t max_push_bytes = 128;

    /**
     * @brief what a pass is given: its own declaration, resolved
     *
     * `own` is indexed by the pass's OWN BINDING NUMBER - `deren.vulkan.render_resource`'s validator requires those
     * to be contiguous from zero, so the index IS the declaration's `binding` field and nothing is looked up
     * in the frame path.
     *
     * THE STORAGE LIVES HERE, in fixed arrays the host fills and the spans view. That shape is deliberate:
     * the runner creates this struct per pass per frame and the pass records from it immediately, so one
     * struct owns everything the pass reads and there is no second place for a handle to live (and no
     * lifetime for a host to get wrong).
     */
    /**
     * @brief the BYTES of a stage's push block, for resolved_io::push_block
     * @note ONE place decides how a block becomes bytes, so fifteen call sites do not each invent it - and the
     *       caller no longer names a pipeline layout, because no heap pipeline has one.
     */
    template <typename Block>
    [[nodiscard]] std::span<std::byte const> push_bytes(Block const& block) noexcept {
        return std::span<std::byte const>(reinterpret_cast<std::byte const*>(&block), sizeof(Block));
    }

    struct resolved_io {
        frame_identity frame = {};
        /// THE command buffer is handed out per frame, at recording time, and never stored: a pass records
        /// into what it is given, which is why it holds no device state between frames.
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        /// the storage `own` views
        std::array<resolved_binding, max_own_bindings> own_storage = {};
        /// the pass's own binding number -> the handles that binding's resource is (exactly one is set)
        std::span<resolved_binding const> own = {};
        /**
         * The same own bindings, ONE VIEW PER SWAPCHAIN IMAGE: `own_per_image[k][image]` is the view binding @c k
         * has for swapchain image @c image (each span is `frame.image_count` long, or empty where the host filled
         * nothing).
         *
         * WHY A PASS NEEDS THIS, measured rather than anticipated: a pass that owns per-image state has to name
         * EACH IMAGE's views while it records - and `own` only carries the CURRENT frame's handles. The host
         * reaches into the core for this (`vulkan_core.gi_images[i]` and friends); a pass
         * cannot - the failure mode this guards against is reaching an image that belongs to another generation.
         */
        std::array<std::span<VkImageView const>, max_own_bindings> own_per_image = {};
        /**
         * @brief HOW A PASS SENDS ITS PUSH BLOCK NOW THAT NO PIPELINE HAS A LAYOUT
         *
         * Every stage is heap-native, and a heap pipeline is created with `layout = VK_NULL_HANDLE` because
         * validation refuses the alternative ("either set the layout to NULL or remove the heaps from the shader").
         * That leaves `vkCmdPushConstants` with nothing to push to - so a pass sends its block through
         * `vkCmdPushDataEXT`, which the shader reads exactly as it always read push constants (the extension says
         * so, and the heap-native probe does it). The framework holds an OWNER AND A CALLBACK rather than the heap
         * itself: the heap is the core's and a pass talks to the HOST.
         *
         * A stage's block now also carries the two heap indices (frame slot, swapchain image - and, for the post
         * chain, its source slot), which the host fills in before sending it. See vulkan/pass/pass.cppm's siblings
         * and shaders/heap_slots.glsl.
         */
        struct push_endpoint {
            void* owner = nullptr;
            bool (*push)(void* owner, VkCommandBuffer command_buffer, std::span<std::byte const> bytes, uint32_t extra_lane) = nullptr;

            /// @param command_buffer the command buffer the block is sent on
            /// @param bytes the stage's push block, with the heap index lanes already appended
            /// @param extra_lane the POST chain's own source slot, which only that chain's passes can name; every
            ///        other stage leaves it 0 and its shader does not declare the third field at all.
            [[nodiscard]] bool operator()(VkCommandBuffer command_buffer, std::span<std::byte const> bytes, uint32_t extra_lane = 0u) const {
                return this->push != nullptr && this->push(this->owner, command_buffer, bytes, extra_lane);
            }
        };
        push_endpoint push_block = {};
        /**
         * The storage `targets` views: the images this pass RENDERS INTO, in the order its declaration names
         * them, each with the view a rendering instance takes and the image a barrier takes.
         *
         * A pass declares a target because an attachment is a use that cannot be a descriptor (see
         * `render_target`): it is bound by `vkCmdBeginRendering`, not by a set. The pass that renders into it
         * OPENS the rendering instance - the load op and the clear value are its business, since only it knows
         * whether the old contents matter - and the runner's job is to have the pipeline bound and the
         * viewport set before it does.
         *
         * ONE SLOT PER ELEMENT, not one per declaration entry: a target claiming a RUN of elements
         * (`render_target::count`, i.e. the shadow map's cascades) lands here as one slot per element, so the
         * span's length is the frame's answer to "how many of them exist". A pass whose run is a sequence of
         * INSTANCES - each with its own attachment - opens one instance per slot, which is what the shadow pass
         * does; a pass with one target per instance reads `targets[0]`.
         */
        std::array<resolved_binding, max_render_targets> target_storage = {};
        std::span<resolved_binding const> targets = {};
        /**
         * The images this pass declared as BARRIER IMAGES (`pass_io::barrier_images`), in the declaration's
         * order - the handles it may move between layouts but never bind as a descriptor.
         *
         * This is a THIRD channel rather than a corner of `own`, and the distinction is the point: `own` is
         * indexed by the pass's own binding numbers, while these resources reach the pass's shaders through the
         * frame's heap (their descriptors are published by whichever stage owns them, e.g. the G-buffer) and the
         * pass only needs their IMAGES. A pass that wants one of these as a descriptor says so with a `shared`
         * binding; a pass that only needs to transition it declares it here.
         */
        std::array<resolved_binding, max_barrier_images> barrier_storage = {};
        std::span<resolved_binding const> barrier_images = {};
        /**
         * The BUFFERS this pass declared as BARRIER BUFFERS (`pass_io::barrier_buffers`), in the declaration's
         * order - the handles it may order around with a buffer memory barrier but never bind itself.
         *
         * The same argument as the images above, one resource class over, and it was measured rather than
         * assumed: the clustered-light sort writes two buffers that live in the SHARED scene set, so its
         * declaration names no binding for either - and without their handles it could not place the barrier
         * that makes its writes visible to the fragment stages reading them later in the same submission. Each
         * entry carries the BUFFER (the `view` and `image` lanes stay null: a barrier takes a buffer, and this
         * resource has no view).
         */
        std::array<resolved_binding, max_barrier_buffers> barrier_buffer_storage = {};
        std::span<resolved_binding const> barrier_buffers = {};
        /// the storage `pipelines` views
        std::array<VkPipeline, max_pass_pipelines> pipeline_storage = {};
        /// in the order `behaviour::pipelines` names them, one entry per name
        std::span<VkPipeline const> pipelines = {};
        /**
         * The push block the HOST composed for this pass this frame, as raw bytes.
         *
         * Raw, because the framework has no pass's type and will not learn one: the declaration's `push`
         * block is the size contract, and the pass reads the bytes as the struct it declared. The host
         * composes it because the values in it - the scene's bounds, an instance table's device address, the
         * global light direction - are the renderer's, not the pass's; what the pass owns is the block's
         * SHAPE and the fields it changes per dispatch (a chain's mode lane).
         */
        std::array<std::byte, max_push_bytes> push_storage = {};
        std::span<std::byte const> push = {};
        /**
         * THIS FRAME's shared constants: the camera, the fitted scene bounds and the sun, as the frame loop
         * produced them (see `deren.vulkan.frame_constants`).
         *
         * WHY THEY ARE HERE RATHER THAN COMPOSED INTO THE PUSH BLOCK BY THE HOST, which is what happens today:
         * a push block's values come from three places - this frame's facts (here), the pass's own parameters
         * (the pass's), and its own per-dispatch lanes (the pass's) - and only the first is the frame loop's
         * business. Composing the whole block in the renderer is what makes every new pass a new resolver
         * function in `deren.vulkan.runtime`; handing the facts over as DATA is what lets the pass do it itself,
         * without a callback that answers arbitrary questions (the shape `pass_host` is deliberately kept away
         * from - see its note).
         *
         * A VALUE, not a pointer: the frame loop produces one of these per frame whether or not any pass wants
         * it, and a pass reading it cannot outlive the frame it was recorded in.
         */
        frame_constants constants = {};
        /// the extent THIS pass works at: the frame's, half of it, or a resource's, per `behaviour::extent` -
        /// and EMPTY for a pass that declared `extent_rule::none`, which sizes its own work from its frame
        VkExtent2D extent = {0, 0};
    };

    // =============================================================================================
    // 1b. WHAT A RESOLVER IS GIVEN - the resources that exist this frame, and three narrow lookups
    // =============================================================================================

    /**
     * @brief a pipeline a pass owns
     * @note the frame resolves a `behaviour::pipelines` NAME to one of these, and the runner binds what it finds
     */
    struct owned_pipeline {
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    /**
     * @brief what a pass needs to turn its OWN declaration into this frame's handles
     *
     * WHY THIS IS NOT `pass_host`: the host is the RUNNER's interface (frame, feature gate, the resolve call
     * itself, the mark pair), and a pass never sees it - that rule is what stops this framework from growing a
     * context object that answers whatever the newest pass asks for. This struct is the opposite shape: it is
     * DATA the frame loop produced (the resources that exist, the frame identity, the command buffer) plus three
     * lookups that all take the DECLARATION's own keys - "the handles the declaration named", "the extent of the
     * resource element the behaviour named", "the pipeline the behaviour named". A pass cannot reach a resource
     * through it that its declaration did not name, because the table only answers for
     * (resource, element, instance) and the keys come from the declaration.
     *
     * A pass that does not override `frame_pass::resolve` sees this through the default implementation; a pass
     * that does (its frame decides something the declaration cannot say) gets exactly this and nothing else.
     */
    struct resolve_context {
        /// the resources that exist this frame, keyed the way a declaration names them (see resource_table)
        resource_table const* resources = nullptr;
        /// the frame being recorded: which image, which slot, how many images, the frame's extent
        frame_identity frame = {};
        /// THE command buffer, handed out here for the same reason `resolved_io::cmd` exists: a pass records
        /// into what it is given and stores no device state between frames
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        /**
         * The extent of a resource element the behaviour named (`extent_rule::resource`), or {0,0} for an
         * element the owner does not have - only the owner knows its own images' sizes
         */
        VkExtent2D (*extent_of)(void* owner, render_resource::resource_id id, uint32_t element) = nullptr;
        /// the pipeline a `behaviour::pipelines` NAME refers to, or all-null when the owner has none
        owned_pipeline (*pipeline)(void* owner, std::string_view name) = nullptr;
        /// what every lookup above is called with
        void* owner = nullptr;
    };

    // =============================================================================================
    // 2. WHAT A PASS IS GIVEN (create) AND WHAT THE RUNNER TALKS TO (record)
    //
    // TWO STRUCTS, and the split is a decision rather than bookkeeping: they have different OWNERS and
    // different LIFETIMES. `pass_context` answers "build what you own" and can be filled by anyone who has a
    // device and the shared facts (the runtime is one such owner, not the only one) - which is what lets a
    // pass exist outside this renderer. `pass_host` answers "run this frame" and only a frame loop can fill it.
    // =============================================================================================

    /**
     * @brief what a pass is given to BUILD what it owns; fillable by any owner, not only a runtime
     *
     * WHY IT IS SEPARATE FROM `pass_host`: a pass's create step needs things the frame loop does not have and
     * a frame loop needs things a create step does not. Merging them produced one struct that grew with every
     * pass - the shape this layer exists to avoid - and it made "who may create a pass" the same question as
     * "who may run a frame", which is not true: an editor, a test or another renderer's main() can build these
     * three facts (a device, five samplers, two lookups) and own a pass.
     *
     * WHAT IS IN IT, and what is deliberately not: the device; the renderer's five samplers, which a
     * declaration CHOOSES between by `sampler_hint` (a pass never names a `VkSampler` of its own, or the five
     * would become six); and a pass's own shader bytes, asked by name. NOT here: no instance, no
     * physical device, no allocator, no queue, no command pool, and no frame. `deren.vulkan.core` remains the only
     * thing that creates an IMAGE, so a pass cannot take over an image family through this struct.
     */
    struct pass_context {
        /// the device a pass builds its own objects on (the owner fills this from the filtered core view)
        VkDevice device = VK_NULL_HANDLE;
        render_resource::shared::sampler_set samplers = {};
        /**
         * The SPIR-V of one of this pass's shaders, by the name it declares; empty when the owner does not
         * have it.
         *
         * A CALLBACK rather than bytes, because a pass needs its shaders exactly once and only the ones it
         * declares - so it asks for them. The OWNER is the side that knows where shader files come from (in
         * this renderer the app loads them and hands them over), which keeps a path and a file format out of
         * this framework.
         */
        std::span<uint8_t const> (*shader)(void* owner, std::string_view name) = nullptr;
        /**
         * The three numbers a SHADER BINDING TABLE is built against: the handle size, the base alignment of a
         * region's device address and the alignment of a handle inside a region (see
         * `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`). Zeroed on a device without the ray-tracing
         * pipeline, which is also the answer to "build no table".
         *
         * WHY THE CONTEXT CARRIES IT: a pass that traces through a pipeline fills its own SBT, and the stride is
         * NOT the handle size on a device whose handle alignment is larger - a fact the pass cannot query (it has
         * no physical device) and must not guess.
         */
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_properties = {};
        /**
         * A small buffer the OWNER creates, uploads and keeps alive: what a pass needs when it must hand the
         * device a device ADDRESS rather than a binding (the shader binding table above is the first case).
         *
         * The owner keeps ownership because the memory has to come from its allocator and live with the
         * generation; the pass gets the handle and the address and nothing to destroy.
         */
        VkBuffer (*create_upload_buffer)(void* owner, void const* data, uint64_t bytes, VkBufferUsageFlags usage, VkDeviceAddress* out_address) = nullptr;
        /**
         * The SURFACE's format, which is a session-stable device fact rather than a frame's.
         *
         * WHY A PASS NEEDS IT: a pipeline that renders into the swapchain has to be created with the format that
         * image actually has, and that format is not a compile-time constant (it is whatever the surface reports;
         * `deren.vulkan.core` finds it at startup, and `hdr_format`/`gbuffer_formats` are the constants the passes can
         * already name). The post chain's pipeline builders take it as a parameter for exactly this reason, and
         * before this field the only way to hand it over was for the runtime to build those pipelines itself -
         * which is the per-stage ownership this framework has been removing.
         *
         * The EXTENT is deliberately not here: it changes with a resize, and a pass that bakes one into an object
         * rebuilds that object in `on_swapchain_recreated` - the hook that exists for it. A format never changes
         * for a given surface, so a pass may cache this at create time.
         */
        VkFormat swap_chain_image_format = VK_FORMAT_UNDEFINED;
        /**
         * The DEPTH format, the second session-stable format a pass may need - and the one the shadow pass cannot
         * do without: its pipeline has a depth attachment and no colour one, so `swap_chain_image_format` is the
         * wrong fact for it. It is a SESSION-STABLE device fact for the same reason the surface's is (the renderer
         * finds it once at startup), which is what lets a pass cache it at create time.
         */
        VkFormat depth_format = VK_FORMAT_UNDEFINED;
        /**
         * Whether the DEVICE can run a mesh pipeline, i.e. whether a pass may build one at all (see
         * docs/mesh_shaders.md).
         *
         * WHY A PASS MUST BE TOLD RATHER THAN ASK: `vkCreateShaderModule` REFUSES a module that declares the
         * SPIR-V `MeshShadingEXT` capability unless VK_EXT_mesh_shader and its `meshShader` feature are enabled
         * on the device, and it says so through the validation layer as an ERROR - which this renderer does not
         * accept even when the failure is caught. A pass therefore must not even TRY on a device without them.
         * The owner knows the answer from its capability query; a pass has no physical device to ask, and the
         * shader file's presence says nothing about the device.
         */
        bool mesh_shaders = false;
        /**
         * One of THIS pass's declared resources, at CREATE time: the handles its own resources are, or all-null
         * when the owner has none.
         *
         * WHY A PASS NEEDS IT, measured: a pass that reads a resource the RENDERER
         * holds - the alphaMode MASK bake's material table and bindless texture array, the compute-skinning
         * job's per-slot matrix buffers - had no way to name it, so the runtime built those resources on the
         * pass's behalf through a bespoke entry point per job. This is the channel that replaces them, and it speaks the
         * DECLARATION's vocabulary (`resource_id` + element) rather than a new one, so an owner can refuse an id
         * the pass never declared.
         *
         * THE LIFETIME RULE IS THE SAME ONE `resolved_io` FOLLOWS PER FRAME: what arrives here is a
         * session-stable handle (the runtime's material table, its texture array, a per-frame-SLOT buffer). A
         * per-swapchain-image VIEW is not stable - it is rebuilt with every generation - and those keep arriving
         * per frame through `own` / `own_per_image` / `barrier_images`, which is the channel that was added for
         * exactly that reason. A pass that cached one of those here would be storing a handle its own
         * `on_swapchain_recreated` cannot repair.
         */
        resolved_binding (*resource)(void* owner, render_resource::resource_id id, uint32_t element) = nullptr;
        /**
         * How many frames the owner has in flight - the number of per-frame-slot resources it will publish.
         *
         * A pass with per-slot state knows how many slots to ask for only from the owner, and the alternative -
         * looping until a slot answers with nothing - makes "the owner has three slots" and "this owner forgot
         * to publish the fourth" the same statement.
         */
        uint32_t frames_in_flight = 0;
        /// what the lookups above are called with (the renderer passes itself)
        void* owner = nullptr;
    };

    /**
     * @brief the runner's interface to the renderer: callbacks plus a context, no virtuals, no allocation
     *
     * The same injection shape `deren.vulkan.animation`'s `backend` uses (a struct of callbacks and a context, passed
     * in rather than inherited), which is why this framework depends on NEITHER `deren.vulkan.runtime` NOR
     * `deren.vulkan.core`: `main.cpp`'s replacement, or a test, can supply one.
     *
     * IT IS THE RUNNER'S, NOT A PASS'S: what a pass is given is `resolved_io` at record time and
     * `pass_context` at create time. A pass has no reason to see this struct at all, which is what stops it
     * from growing into a context object that hands out whatever the newest pass wants.
     */
    struct pass_host {
        /// HOW A PASS SENDS ITS PUSH BLOCK (see resolved_io::push_block): every pipeline is heap-native and
        /// therefore layout-less, so the block travels through vkCmdPushDataEXT. The runner copies this into the
        /// resolved io of every stage, exactly as it copies the other host callbacks.
        resolved_io::push_endpoint push_block = {};

        void* context = nullptr;
        /// the frame being recorded
        frame_identity (*frame)(void* context) = nullptr;
        /// whether a pass's feature is active this frame (`feature()`; empty means always)
        bool (*feature_active)(void* context, std::string_view feature) = nullptr;
        /// resolve a pass's own declaration to handles; false means "this frame cannot run it"
        bool (*resolve)(void* context, frame_pass const& pass, resolved_io& out) = nullptr;
        /// the mechanical pre-record step the behaviour asks for: bind the pipelines, resync the viewport
        void (*apply_behaviour)(void* context, frame_pass const& pass, resolved_io const& io) = nullptr;
        /// open and close one stage's timing interval: the STAGE owns the mark, not the pass
        void (*mark_begin)(void* context, std::string_view stage_name) = nullptr;
        void (*mark_end)(void* context, std::string_view stage_name) = nullptr;
    };

    /**
     * @brief the host hook a frame's LAST writer draws the overlay with
     *
     * ONE VALUE rather than two fields (`after_draw` plus the `owner` it is called with), and that is the point:
     * a callback and the context it must be called with are ONE fact, and the two-field form let a frame name a
     * function without the context that made it callable - or a context with a null function. The host publishes
     * it (see `runtime::overlay_draw`), the chain owner hands it to the passes that may be the frame's last
     * writer, and each of those decides for itself whether it is the one that draws it.
     */
    struct draw_callback {
        /// the host's function; null means "nothing to draw after this pass"
        void (*record)(void* owner, VkCommandBuffer command_buffer) = nullptr;
        /// what it is called with (the host's own context)
        void* owner = nullptr;

        /// @brief whether there is anything to call
        [[nodiscard]] bool valid() const noexcept {
            return this->record != nullptr;
        }
    };

    /**
     * @brief THE FRAME'S PUBLISHED FACTS: the per-frame values only the HOST can compute
     *
     * WHY THEY ARE PUBLISHED AT ALL, now that a pass builds its own frame (see `frame_pass::prepare_frame`): the
     * frame belongs to the pass, and a value only one pass reads belongs IN that pass - but these are ANSWERS the
     * host composes from things a pass cannot see (the frame's image index, the device's ray-query support,
     * whether this frame's structures were built, the chain owner's own feature table). The rule the framework
     * settled on is exactly this one: a per-frame decision a pass cannot derive is an explicit frame FIELD, and
     * what the pass cannot derive is what the host publishes here.
     *
     * A FIELD IS FILLED WITH THE SAME EXPRESSION IT REPLACED, never with a similarly named one, and two names in
     * this struct exist to make that explicit rather than to be tidy: `gi_specular` is the LOBE's COMPOSED
     * predicate (the knob AND hit shading AND this frame's structures), which is NOT the knob a feature fact of
     * the same name carries; `debug_view` is the composed `gbuffer-debug` answer as well. Both were composed by
     * the renderer before this struct existed, and substituting the raw knob would be a behaviour change no gate
     * scenario would catch (the knob is off in all of them).
     */
    struct frame_facts {
        /**
         * `megalights_active()`: the stochastic punctual lighting knob AND its pass AND the deferred shading
         * path. Published as a FRAME fact rather than read from the knob because the DEFERRED LIGHTING STAGE
         * has to act on it too: when this is true the punctual lights are the stochastic pass's business and
         * the lighting stage must not add them again (see its `punctual_replaced` lane) - the same
         * one-predicate-two-readers arrangement `gi_traced` has with the ambient.
         */
        bool megalights = false;
        /**
         * Whether the stochastic lighting image holds THIS frame's estimate, which is a DIFFERENT fact from the
         * one above and has to be: the lighting stage acts on it by NOT adding the punctual lights itself, and
         * acting on the knob instead would lose them entirely on a frame where the pass was gated off (a
         * descriptor set that could not be had, a pipeline that failed to build). The renderer sets this from
         * the stage's own `run_report` after it records, so a frame either has the estimate or keeps the raster
         * loop - never neither.
         */
        bool megalights_resolved = false;
        /**
         * Whether this IMAGE's stochastic accumulation may be read at all, which is the renderer's per-image
         * bookkeeping: false on the first frame of a generation (a resize destroys the histories) and false for
         * a frame the resolve did not record. The resolve then writes this frame's estimate with a frame count
         * of 1 instead of averaging in whatever the allocation held.
         */
        bool megalights_history_valid = false;
        /// whether the accumulation the temporal resolve blends into exists FOR THIS IMAGE
        /**
         * How much the diffuse accumulation is still COLD: 1 = it restarted this frame, 0 = converged
         * (the chain's own frame policy is the ramp's length, and the temporal resolve applies it).
         *
         * WHY THE RENDERER COMPUTES IT: the accumulation restarts when the generation changes, because that
         * is what destroys the history images (a resize, a minimize/restore). On the frames that follow there
         * is nothing to average with, so the composite would show the RAW half-resolution 2-ray trace - which
         * is what a user sees as a snowstorm in the shadowed, ambient-only regions. The resolve widens its
         * estimate while this is above zero, and is byte-identical to its unwidened form once it reaches it.
         * UE's spatial denoiser drives the same kind of widening from its frames-accumulated texture
         * (LumenReflectionDenoiserSpatial.usf:81-86).
         */
        /// `post_fxaa_active()`: the FXAA knob and the pass having built its pipeline, i.e. who writes the LDR image
        bool fxaa_resolves = false;
        /**
         * `post_upscale_active()`: the render chain running BELOW the output size and the upscale pass having
         * built its pipeline - i.e. the resolve is this frame's last writer, so the composite writes the LDR
         * image for IT to read (the same question `fxaa_resolves` answers for FXAA, and the two are mutually
         * exclusive: see `runtime::post_fxaa_active`).
         */
        bool upscale_resolves = false;
        /// the composed `gbuffer-debug` answer (see the note above), which is what suppresses the bloom sum
        bool debug_view = false;
        /// the clustered lighting's bin count, which is the sort's only input beyond its own dispatch
        uint32_t cluster_count = 0;
    };

    // =============================================================================================
    // 3. WHAT A PASS IS - the base every pass derives from, shaped like deren.vulkan.primitive
    // =============================================================================================

    /**
     * @brief the base class of every frame pass: pure virtual, one `final` class per pass
     *
     * Modelled on `deren.vulkan.primitive`, which has carried the renderer's dynamic dispatch since the primitive
     * work: a small pure-virtual interface, `final` derived classes, and the CONTRACT written down - there,
     * "the runtime binds the pipeline and the scene block before calling draw()"; here, "the runner validates
     * `io()`, resolves it, applies `behaviour()`, then calls `record()`".
     *
     * `feature()` returns a NAME rather than an enumerator: which features exist and who enables them is the
     * runtime's registry, not this layer's business, and a pass must be recordable-or-not without this module
     * knowing the list. The host resolves it; the cost is one string compare per pass per frame against a
     * dozen passes.
     */
    class frame_pass {
    public:
        frame_pass() = default;
        frame_pass(frame_pass const&) = delete;
        frame_pass& operator=(frame_pass const&) = delete;
        virtual ~frame_pass() = default;

        /// @brief the declaration: what it uses, in `deren.vulkan.render_resource`'s vocabulary
        [[nodiscard]] virtual render_resource::pass_io const& io() const noexcept = 0;
        /// @brief how the runner must call it
        [[nodiscard]] virtual behaviour const& behaviour() const noexcept = 0;
        /// @brief the feature that gates it ([render] keys); empty means "always"
        [[nodiscard]] virtual std::string_view feature() const noexcept = 0;
        /// @brief build what this pass owns (its pipeline, its per-generation state) from
        ///        @p context; once per device generation
        virtual void create(pass_context const& context) = 0;
        /// @brief the swapchain was rebuilt, so every per-image resource this pass held is stale
        virtual void on_swapchain_recreated(pass_host const& host) = 0;
        /**
         * @brief fill this frame's `resolved_io` FROM THIS PASS'S OWN DECLARATION
         *
         * THE DEFAULT IMPLEMENTATION IS THE POINT OF THE FRAMEWORK, and it is what the renderer's sixteen
         * hand-written `resolve_*_pass` functions are being replaced by, one pass at a time: walk the
         * declaration, ask the resource table for every own binding, target and barrier entry, take the
         * pipelines the behaviour names, and the extent from the behaviour's
         * own rule. A pass whose declaration says everything it needs does NOT override this - there is nothing
         * left for the owner to know.
         *
         * A PASS OVERRIDES IT when its FRAME decides something the DECLARATION cannot express, and the override
         * starts by calling `resolve_declaration(*this, context, out)` so the declaration's half stays shared:
         * the composite writing the LDR image when FXAA runs (one `render_target` names one resource), the
         * debug view's channel, a pass whose target comes from the frame's table. What an override must NOT do is
         * reach for a resource the declaration does not name - the context's lookups all take the declaration's
         * own keys for exactly that reason. ("The shadow pass handing over one target per cascade" used to be on
         * this list: it is a RUN of elements now, which the declaration CAN express - see
         * `render_target::count`.)
         *
         * @param context the resources that exist this frame + the owner's three lookups (see resolve_context)
         * @param out the struct to fill; the pass's `record` reads it immediately
         * @return false when this frame cannot run the pass, which the runner treats as "skip, record nothing"
         */
        [[nodiscard]] virtual bool resolve(resolve_context const& context, resolved_io& out) const;
        /**
         * @brief build THIS pass's frame from the facts the host published for this stage
         *
         * WHO COMPOSES A FRAME, after this: the pass. The frame is one pass's per-frame input, so it is that
         * pass's business and nobody else's - which is what removes the last place the RENDERER named a concrete
         * pass's frame type (`runtime::make_*_frame`, one builder per pass, each one a struct literal reading the
         * renderer's own members). The host publishes the few values a pass cannot derive (see `frame_facts`) and
         * calls this before `resolve`, so the frame is in place by the time the pass's own resolver and record
         * step read it.
         *
         * THE DEFAULT DOES NOTHING, and it is the correct default rather than a stub: a pass whose declaration
         * and own state say everything it needs has no frame at all. Twelve of this renderer's passes were
         * already in that position; the eight that were not override this.
         *
         * A PASS'S FRAME IS NOT A CHANNEL FOR ITS OWNER. What the chain owner must tell a pass per frame (that
         * a structure exists, that a pass's own recording is its own) is a SETTER on that pass, set
         * where the owner holds the pass - because a frame field written from outside the pass is a second owner
         * for the pass's own state, which is the defect this framework keeps removing.
         */
        virtual void prepare_frame([[maybe_unused]] frame_facts const& facts) noexcept {
        }
        /**
         * @brief the pipeline this pass OWNS, when it built one in `create`; `VK_NULL_HANDLE` otherwise
         *
         * THE TWELVE PASSES THAT BUILD THEIR OWN PIPELINE ALREADY ANSWER THIS (their `pipeline()` accessor has had
         * exactly this signature since each was extracted), so making it part of the interface costs them nothing
         * and gives the declaration-driven resolver the one fact it cannot get from the declaration: a pass that
         * owns its pipeline must be handed ITS OWN, never a registry entry that happens to share its
         * `behaviour::pipelines` name. The runner binds what this returns (`apply_pass_behaviour`), which is the
         * same relay the renderer's resolvers used to do by hand.
         *
         * A pass that owns TWO variants of one pipeline (the composite: the swapchain one and the HDR one) leaves
         * this null and fills `resolved_io::pipelines` in its own `resolve` - choosing between them is its frame's
         * decision, not the declaration's.
         */
        [[nodiscard]] virtual VkPipeline pipeline() const noexcept {
            return VK_NULL_HANDLE;
        }
        /**
         * @brief a pipeline this pass owns under the NAME another pass's `behaviour::pipelines` declares
         *
         * WHY THIS EXISTS: a chain's stages can SHARE one pipeline. The post chain's four bloom levels record with
         * the composite's R16F variant - one pipeline serves all five stages, so a copy
         * per level would be five identical pipelines and five chances to disagree about the push block (the post
         * header records that decision). The declaration already carries the name a pass records with
         * (`behaviour::pipelines`); what it cannot carry is WHOSE object that name is, so the OWNER resolves it and
         * asks this on every pass of the chain, taking the first answer - which is what keeps the resolution
         * chain-agnostic: the renderer does not know, and does not need to know, which pass owns what.
         * @param name one of this pass's own `behaviour::pipelines` names, or a sibling's
         * @return the pipeline, or VK_NULL_HANDLE when this pass owns nothing by that name
         */
        [[nodiscard]] virtual owned_pipeline named_pipeline([[maybe_unused]] std::string_view name) const noexcept {
            return {};
        }
        /**
         * @brief whether this pass built what it records with, i.e. whether it CAN run
         *
         * THE GENERIC FORM OF THE QUESTION EVERY OWNER HAS BEEN ASKING BY HAND. Fourteen of these passes have had a
         * `pipeline_ready()` accessor since each was extracted, and every caller that holds a typed reference asks it
         * - the renderer's feature registry above all ("the knob is on AND the pass built its pipeline"). A pass's
         * readiness is a property of the PASS, so it belongs on this interface, where a caller that holds only a
         * DECLARATION NAME can ask it through `pass_chain::ready(name)`.
         *
         * THE DEFAULT IS TRUE, deliberately: a pass that builds nothing of its own (the scene and transparent passes
         * record with the renderer's pipeline) is not "unready", it has nothing to be unready ABOUT. A pass that
         * builds a pipeline, or that keeps per-image state it could fail to build, overrides this with its own
         * answer.
         */
        [[nodiscard]] virtual bool ready() const noexcept {
            return true;
        }
        /**
         * @brief record into the frame, with the resources the declaration asked for already resolved
         *
         * NOT const, and this is a correction the first real pass forced rather than a convenience: a pass
         * that owns per-generation state has to be able to repair it while recording. The guarantee that layer
         * actually needs is the one this keeps:
         * a pass holds no device state BETWEEN frames and is handed everything it needs to record - the
         * command buffer, its own bindings, the pipelines, the extent and the push block. What
         * it must not do is reach for anything the run did not resolve, and that is enforced by what
         * `resolved_io` carries.
         */
        virtual void record(resolved_io const& io) = 0;
    };

    // =============================================================================================
    // 4. A STAGE - a group of passes the runtime records together, in declaration order
    // =============================================================================================

    /**
     * @brief one stage of the frame: the passes it runs and the marks around them
     *
     * A stage exists so that the ORDER and the MARKS stop being positional. Today the frame's order is a
     * recording spine and `gpu_mark_id` is a private nested enum whose order is a contract with 15 call sites
     * in eight subsystems - an extracted pass cannot even name that enumeration. Here the stage writes its own
     * pair around its children, so a pass says which stage it belongs to and nothing else.
     *
     * The passes are POINTERS in declaration order, never a container whose iteration order is an accident:
     * this renderer's verification rests on byte-identical captures, and a hash map's order would break it.
     */
    struct stage {
        std::string_view name = {};
        /// MUTABLE pointers: a pass stores what it owns into itself through `create` and
        /// `on_swapchain_recreated`, and `record` is non-const because a pass that keeps per-generation state
        /// repairs it there (see `frame_pass::record`).
        std::span<frame_pass*> passes = {};
        bool marks = true; // a stage nested inside another's instance may not want its own pair
    };

    /**
     * @brief what the runner did, which is what its test asserts
     *
     * A report rather than a log: the contract of this layer ("create once, resolve per frame, skip an inactive
     * pass WITHOUT resolving it, mark once per stage, recreate every pass on a new generation") is only worth
     * anything if it can be checked - and checked without a device.
     */
    struct run_report {
        uint32_t created = 0;
        uint32_t recorded = 0;
        uint32_t skipped_inactive = 0;
        uint32_t skipped_unresolved = 0;
        uint32_t marked = 0;
        uint32_t recreated = 0;
        /// the name of the pass whose declaration the runner refused; empty when the stage was built
        std::string_view rejected = {};
    };

    /**
     * @brief build every pass in the stage, and REFUSE the whole stage if any declaration is invalid
     *
     * The validation is `vulkan.render_resource::validate`, which is pure data: it needs no device, so a bad
     * declaration is caught at startup on every machine rather than by a validation-layer message at submit
     * time, on the machine that happens to run that pass.
     *
     * The CONTEXT is the only argument, because building a pass needs nothing else: a frame loop is not
     * involved, and a caller that builds passes without ever running a frame (an editor, a test, a tool that
     * compiles pipelines) needs exactly this one struct.
     */
    [[nodiscard]] inline run_report create_stage(stage const& st, pass_context const& context) {
        run_report report;
        for (frame_pass* const p : st.passes) {
            if (p == nullptr) {
                continue;
            }
            if (!render_resource::validate(p->io())) {
                report.rejected = p->io().name;
                return report;
            }
            p->create(context);
            ++report.created;
        }
        return report;
    }

    /**
     * @brief record one stage: resolve, apply the behaviour, record, and bracket it with the stage's mark
     *
     * THE ORDER OF THESE STEPS IS THE CONTRACT, and each has a reason:
     *  1. an INACTIVE pass is not resolved and not recorded at all - several passes document exactly that
     *     ("where any requirement is missing the pass is not even recorded"), and it is what makes a feature
     *     that is off byte-identical rather than merely invisible;
     *  2. `resolve` may still fail for a frame (a declared resource the frame does not have), and that is skipped
     *     WITHOUT recording, because a pass recorded with unresolved handles is worse than a pass not running;
     *  3. the behaviour's mechanical part happens BEFORE `record()`, never inside it, which is what makes the
     *     viewport resync unforgettable;
     *  4. the marks bracket the stage, so a pass cannot mark out of order - the defect the positional
     *     `gpu_mark_id` enumeration allows today.
     */
    [[nodiscard]] inline run_report record_stage(stage const& st, pass_host const& host) {
        run_report report;
        bool const marks_open = st.marks && host.mark_begin != nullptr && host.mark_end != nullptr;
        if (marks_open) {
            host.mark_begin(host.context, st.name);
        }
        for (frame_pass* const p : st.passes) {
            if (p == nullptr) {
                continue;
            }
            if (host.feature_active != nullptr && !p->feature().empty() && !host.feature_active(host.context, p->feature())) {
                ++report.skipped_inactive;
                continue;
            }
            resolved_io io = {};
            io.push_block = host.push_block; // the heap push every converted stage needs (see resolved_io)
            if (host.resolve == nullptr || !host.resolve(host.context, *p, io)) {
                ++report.skipped_unresolved;
                continue;
            }
            if (host.apply_behaviour != nullptr) {
                host.apply_behaviour(host.context, *p, io);
            }
            p->record(io);
            ++report.recorded;
        }
        if (marks_open) {
            host.mark_end(host.context, st.name);
            ++report.marked;
        }
        return report;
    }

    /**
     * @brief tell every pass in the stage that the swapchain was rebuilt
     *
     * THIS FUNCTION REMOVES A KNOWN HAZARD BY CONSTRUCTION. A pass that keeps per-generation state (the TAA
     * resolve's per-image history flags are the one left) has to forget it when the swapchain is rebuilt, and a
     * hand-kept reset list in `on_swapchain_recreated` is exactly the kind of fact that gets forgotten, so the
     * runner makes the call instead of the runtime remembering - and `run_report::recreated` counts what it did.
     */
    [[nodiscard]] inline run_report recreate_stage(stage const& st, pass_host const& host) {
        run_report report;
        for (frame_pass* const p : st.passes) {
            if (p == nullptr) {
                continue;
            }
            p->on_swapchain_recreated(host);
            ++report.recreated;
        }
        return report;
    }

    // =============================================================================================
    // 5. THE RESOURCE TABLE - what EXISTS right now, in the declaration's own vocabulary
    //
    // WHY THIS EXISTS. Every pass's declaration already says WHICH resource it uses (`resource_id`) and which
    // image of that family (`element`); what it cannot say is WHICH DEVICE HANDLE that is, because a resource's
    // view is a per-generation object and a pass does not create one. The host has that knowledge - it is the
    // renderer that owns the images - and today it hands it over through one hand-written resolver PER PASS
    // (`runtime::resolve_taa_pass`, `resolve_post_composite`, ... sixteen of them), each indexing the renderer's
    // own arrays. This table is the same knowledge kept ONCE, under the key the declaration already uses, so
    // that the per-declaration part of resolution can be a loop instead of sixteen functions.
    //
    // WHAT IT IS NOT: it is not an allocator, not a lifetime tracker and not a validity rule. A resource that
    // exists but is not registered here answers all-null, exactly as a resource the owner does not have - and
    // deciding whether a frame can run is still the resolver's job, not the table's.
    // =============================================================================================

    /**
     * @ingroup vulkan_pass
     * @brief which INSTANCE of a resource a frame names, decided by the schema's SCOPE rather than by the caller
     *
     * The rule is the one the declaration layer already documents per resource: a per-swapchain-image resource
     * is indexed by the frame's image, a per-frame-slot one by the frame's slot, and a device-wide one has a
     * single instance. Putting it in one function is what stops the two places that need it (whatever FILLS the
     * table and whatever READS it) from each writing their own switch.
     * @param scope the resource's scope, from `render_resource::find(id)->scope`
     * @param frame the frame being recorded
     * @return the instance index to use as the table's third key field
     */
    [[nodiscard]] constexpr uint32_t instance_for(render_resource::resource_scope const scope, frame_identity const& frame) noexcept {
        switch (scope) {
        case render_resource::resource_scope::per_swapchain_image:
            return frame.image_index;
        case render_resource::resource_scope::per_frame_slot:
            return frame.slot;
        case render_resource::resource_scope::device_wide:
            return 0;
        }
        return 0;
    }

    /**
     * @ingroup vulkan_pass
     * @brief the device handles behind every declared resource, keyed by (resource, element, instance)
     *
     * FILLED BY THE OWNER, READ BY THE FRAMEWORK: the renderer publishes what its core and its own members hold,
     * and the runner's resolution asks for it in the declaration's vocabulary. A pass never touches this type -
     * it receives the handles through `resolved_io`, which is the only interface it has.
     *
     * @note PUBLISHED PER FRAME, not once per swapchain generation, and that is a decision with a measured
     *       reason behind it: at least one family is an ALIAS whose target changes within a generation
     *       (`scene_color` is the TAA input or the HDR image depending on whether the resolve runs this frame),
     *       and a table that is refreshed once per generation would hand a pass a view from the wrong side of
     *       that choice. The cost is one pass over a handful of entries per frame, which is what the renderer
     *       already spends deciding the same facts.
     * @note a `resource_id` nobody published answers all-null rather than failing: "the owner does not have it"
     *       and "this frame cannot use it" are different statements, and only the second is a pass's business.
     */
    class resource_table {
    public:
        /// @brief forget everything: the caller is about to publish this frame's resources
        void clear() noexcept {
            this->entries.clear();
            this->families.clear();
        }

        /**
         * @brief publish a WHOLE family element at once: the per-image views and images, in instance order
         *
         * WHY THIS EXISTS ON TOP OF `publish`: a family the owner already holds as one contiguous array (every
         * per-swapchain-image family `deren.vulkan.core` creates) is both cheaper and MORE USEFUL published as the
         * spans it already is - the per-image channel (`resolved_io::own_per_image`) needs exactly that contiguous
         * run of views, and a per-instance copy would make the table the source of a second array that can drift
         * from the first. The spans are stored, not copied: they point at the owner's own storage, which outlives
         * the frame the table describes.
         *
         * @param id the resource, in the declaration's vocabulary
         * @param element which image of the family (the G-buffer's second target, the bloom chain's level 1)
         * @param views one view per instance, in instance order (an empty span publishes nothing)
         * @param images the images behind them, in the same order (a buffer family passes an empty span)
         */
        void publish_family(render_resource::resource_id const id, uint32_t const element, std::span<VkImageView const> views, std::span<VkImage const> images) noexcept {
            if (views.empty() && images.empty()) {
                return;
            }
            for (family_entry& f : this->families) {
                if (f.id == id && f.element == element) {
                    f.views = views;
                    f.images = images;
                    return;
                }
            }
            this->families.push_back(family_entry{.id = id, .element = element, .views = views, .images = images});
        }

        /**
         * @brief publish one resource instance's handles
         * @param id the resource, in the declaration's vocabulary
         * @param element which image of the family (the G-buffer's third target, the bloom chain's level 2, ...)
         * @param instance the swapchain image index or the frame slot, per the resource's scope (see instance_for)
         * @param binding the handles; a family of buffers carries only `buffer`
         * @note publishing the same key twice REPLACES it, so a frame that re-publishes an alias (or an owner
         *       that publishes in two passes) ends with the last value rather than with two entries
         */
        void publish(render_resource::resource_id const id, uint32_t const element, uint32_t const instance, resolved_binding const binding) noexcept {
            for (entry& e : this->entries) {
                if (e.id == id && e.element == element && e.instance == instance) {
                    e.binding = binding;
                    return;
                }
            }
            this->entries.push_back(entry{.id = id, .element = element, .instance = instance, .binding = binding});
        }

        /**
         * @brief the handles published for one resource instance, or all-null when the owner has none
         * @param id the resource, in the declaration's vocabulary
         * @param element which image of the family
         * @param instance the swapchain image index or the frame slot (see instance_for)
         * @note a single entry WINS over a family with the same key, which is what an alias needs: `scene_color`
         *       is published per frame (the TAA input or the HDR image), while the families around it are
         *       published once per frame from the owner's arrays
         */
        [[nodiscard]] resolved_binding find(render_resource::resource_id const id, uint32_t const element, uint32_t const instance) const noexcept {
            for (entry const& e : this->entries) {
                if (e.id == id && e.element == element && e.instance == instance) {
                    return e.binding;
                }
            }
            for (family_entry const& f : this->families) {
                if (f.id == id && f.element == element && instance < f.views.size()) {
                    VkImage const image = instance < f.images.size() ? f.images[instance] : VK_NULL_HANDLE;
                    return resolved_binding{.view = f.views[instance], .buffer = VK_NULL_HANDLE, .image = image};
                }
            }
            return {};
        }

        /**
         * @brief EVERY instance's view of one family element, in instance order - the per-image channel
         * @param id the resource, in the declaration's vocabulary
         * @param element which image of the family
         * @return the views the owner published as one run, or an EMPTY span when it published none (or published
         *         the family instance by instance, which is what a single entry is for)
         */
        [[nodiscard]] std::span<VkImageView const> views_of(render_resource::resource_id const id, uint32_t const element) const noexcept {
            for (family_entry const& f : this->families) {
                if (f.id == id && f.element == element) {
                    return f.views;
                }
            }
            return {};
        }

        /// @brief how many things are published (a diagnostic: what a frame published, and what a test pins)
        [[nodiscard]] uint32_t size() const noexcept {
            return static_cast<uint32_t>(this->entries.size() + this->families.size());
        }

        /// @brief how many instances of one (resource, element) are published - 0 when the owner has none
        [[nodiscard]] uint32_t instances_of(render_resource::resource_id const id, uint32_t const element) const noexcept {
            uint32_t count = 0;
            for (entry const& e : this->entries) {
                if (e.id == id && e.element == element) {
                    ++count;
                }
            }
            for (family_entry const& f : this->families) {
                if (f.id == id && f.element == element) {
                    count += static_cast<uint32_t>(std::max(f.views.size(), f.images.size()));
                }
            }
            return count;
        }

    private:
        struct entry {
            render_resource::resource_id id = render_resource::resource_id::none;
            uint32_t element = 0;
            uint32_t instance = 0;
            resolved_binding binding = {};
        };
        /// one family element, held as the run of views and images the owner already has (see publish_family)
        struct family_entry {
            render_resource::resource_id id = render_resource::resource_id::none;
            uint32_t element = 0;
            std::span<VkImageView const> views = {};
            std::span<VkImage const> images = {};
        };
        /// A vector rather than a map: the table holds what one frame's chain can NAME, it is filled once per
        /// frame in one pass, and the lookups happen during resolution - so the smallest container that works is
        /// the honest one, and its order is the publishing order rather than a hash (this renderer's
        /// verification rests on byte-identical captures, and an iteration order that is an accident is exactly
        /// what the stage runner refuses to depend on).
        std::vector<entry> entries = {};
        std::vector<family_entry> families = {};
    };

    // =============================================================================================
    // 6. THE DECLARATION-DRIVEN RESOLVER - what `frame_pass::resolve` does unless a pass overrides it
    // =============================================================================================

    /**
     * @ingroup vulkan_pass
     * @brief the extent a behaviour's rule asks for, from the frame and the owner's own images
     * @param how the pass's behaviour
     * @param context the frame and the owner's `extent_of` lookup
     * @note `half` is `max(1, axis / 2)` - the SAME formula the renderer creates its half-size images with, so a
     *       pass cannot disagree with the image it writes; `resource` is the owner's answer, because only it
     *       knows its own images' sizes; `none` means the pass sizes its own work and gets {0,0}
     */
    [[nodiscard]] inline VkExtent2D resolve_extent(behaviour const& how, resolve_context const& context) noexcept {
        switch (how.extent) {
        case extent_rule::full:
            return context.frame.extent;
        case extent_rule::half:
            return VkExtent2D{std::max(1u, context.frame.extent.width / 2u), std::max(1u, context.frame.extent.height / 2u)};
        case extent_rule::resource:
            return context.extent_of == nullptr ? VkExtent2D{} : context.extent_of(context.owner, how.extent_of, how.extent_of_element);
        case extent_rule::none:
            return VkExtent2D{};
        }
        return VkExtent2D{};
    }

    /**
     * @brief the pipeline half of the declaration-driven resolution (split out only to keep the function below
     *        readable: it is one loop and one rule)
     */
    [[nodiscard]] inline bool declaration_pipelines_ok(frame_pass const& pass, resolve_context const& context, resolved_io& out) {
        std::span<std::string_view const> const names = pass.behaviour().pipelines;
        if (names.empty()) {
            out.pipelines = {};
            return true;
        }
        if (names.size() > out.pipeline_storage.size() || context.pipeline == nullptr) {
            return false;
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            owned_pipeline const found = context.pipeline(context.owner, names[i]);
            if (found.pipeline == VK_NULL_HANDLE) {
                return false; // the frame cannot bind a pipeline the pass declared: do not record it
            }
            out.pipeline_storage[i] = found.pipeline;
        }
        out.pipelines = std::span<VkPipeline const>(out.pipeline_storage.data(), names.size());
        return true;
    }

    /**
     * @ingroup vulkan_pass
     * @brief resolve a pass's declaration into this frame's handles, entry by entry
     *
     * THE RULES, all of them from the declaration and the schema:
     *  - an OWN binding is what the table holds for its (resource, element, instance), where the instance is the
     *    schema's `scope` applied to the frame (see instance_for). A binding whose resource the owner does not
     *    have means THIS FRAME CANNOT RUN THE PASS - returning true with a null handle would record a
     *    descriptor pointing at nothing;
     *  - the same rule for a render TARGET and for a BARRIER image or buffer, in declaration order - except that a
     *    target claiming a RUN of elements (`render_target::count`) expands to one slot PER ELEMENT, and the run
     *    ENDS EARLY when the frame has fewer elements than the declaration allows (the shadow map's layers are the
     *    cascade knob's, so `targets` is as long as the frame's own answer);
     *  - a PIPELINE is asked for BY THE NAME `behaviour::pipelines` declares;
     *  - the EXTENT comes from the behaviour's rule (resolve_extent).
     *
     * WHAT IT DELIBERATELY LEAVES ALONE: `own_per_image` (the per-image channel a pass that owns per-image
     * state reads) and `push` (a push block's values are the pass's own parameters and this frame's
     * constants, so the pass composes it).
     *
     * @param pass the pass whose declaration is being resolved
     * @param context the resources that exist this frame + the owner's lookups
     * @param out the struct to fill
     * @return false when the frame does not have what the declaration names
     */
    [[nodiscard]] inline bool resolve_declaration(frame_pass const& pass, resolve_context const& context, resolved_io& out) {
        render_resource::pass_io const& declaration = pass.io();
        if (context.resources == nullptr) {
            return false;
        }
        out.frame = context.frame;
        out.cmd = context.cmd;
        out.push = {}; // a push block is composed by the pass that pushes it

        // ---- the pass's own bindings, indexed by their own binding number ----
        uint32_t own_count = 0;
        for (render_resource::pass_binding const& binding : declaration.bindings) {
            if (binding.owner != render_resource::binding_owner::own) {
                continue;
            }
            render_resource::resource_info const* const info = render_resource::find(binding.resource);
            if (info == nullptr || binding.binding >= out.own_storage.size()) {
                return false; // not a resource the schema knows, or not the contiguous own bindings the validator requires
            }
            resolved_binding const handles = context.resources->find(binding.resource, binding.element, instance_for(info->scope, context.frame));
            if (handles.view == VK_NULL_HANDLE && handles.buffer == VK_NULL_HANDLE && handles.image == VK_NULL_HANDLE) {
                return false; // this frame does not have it: do not record the pass at all
            }
            out.own_storage[binding.binding] = handles;
            own_count = std::max(own_count, static_cast<uint32_t>(binding.binding) + 1u);
        }
        out.own = std::span<resolved_binding const>(out.own_storage.data(), own_count);
        // THE PER-IMAGE CHANNEL: for an own binding whose resource is per SWAPCHAIN IMAGE, every image's view -
        // what a pass that works per image reads to reach the generation's other images (see
        // resolved_io::own_per_image), and the only channel through which a pass can name a generation's views
        // without the renderer publishing them itself. Empty for every other binding, which is the shape the
        // consumer reads: it checks each span's length before indexing it.
        for (render_resource::pass_binding const& binding : declaration.bindings) {
            if (binding.owner != render_resource::binding_owner::own || binding.binding >= out.own_per_image.size()) {
                continue;
            }
            render_resource::resource_info const* const info = render_resource::find(binding.resource);
            if (info == nullptr || info->scope != render_resource::resource_scope::per_swapchain_image) {
                continue;
            }
            out.own_per_image[binding.binding] = context.resources->views_of(binding.resource, binding.element);
        }

        // ---- the targets, in declaration order, a RUN expanding to one slot per element ----
        uint32_t slot = 0;
        for (render_resource::render_target const& target : declaration.targets) {
            render_resource::resource_info const* const info = render_resource::find(target.resource);
            if (info == nullptr) {
                return false;
            }
            for (uint16_t i = 0; i < target.count; ++i) {
                if (slot >= out.target_storage.size()) {
                    return false; // more targets than the fixed storage: the declaration outgrew the framework
                }
                resolved_binding const handles = context.resources->find(target.resource, target.element + i, instance_for(info->scope, context.frame));
                if (handles.view == VK_NULL_HANDLE && handles.image == VK_NULL_HANDLE) {
                    if (i == 0u) {
                        return false; // this frame does not have it: do not record the pass at all
                    }
                    // THE FRAME HAS FEWER ELEMENTS THAN THE DECLARATION ALLOWS, which is the ordinary case for a
                    // run: the shadow map has exactly the layers the cascade knob asked for, and the string of
                    // published elements is a PREFIX of the family (element 0 is created first). The run ends
                    // here and the pass renders what it was given.
                    break;
                }
                out.target_storage[slot++] = handles;
            }
        }
        out.targets = std::span<resolved_binding const>(out.target_storage.data(), slot);

        // ---- the barrier images and buffers, in declaration order ----
        if (declaration.barrier_images.size() > out.barrier_storage.size() || declaration.barrier_buffers.size() > out.barrier_buffer_storage.size()) {
            return false;
        }
        for (std::size_t i = 0; i < declaration.barrier_images.size(); ++i) {
            render_resource::resource_info const* const info = render_resource::find(declaration.barrier_images[i].resource);
            if (info == nullptr) {
                return false;
            }
            resolved_binding const handles = context.resources->find(declaration.barrier_images[i].resource, declaration.barrier_images[i].element, instance_for(info->scope, context.frame));
            if (handles.image == VK_NULL_HANDLE) {
                return false;
            }
            out.barrier_storage[i] = handles;
        }
        out.barrier_images = std::span<resolved_binding const>(out.barrier_storage.data(), declaration.barrier_images.size());
        for (std::size_t i = 0; i < declaration.barrier_buffers.size(); ++i) {
            render_resource::resource_info const* const info = render_resource::find(declaration.barrier_buffers[i].resource);
            if (info == nullptr) {
                return false;
            }
            resolved_binding const handles = context.resources->find(declaration.barrier_buffers[i].resource, declaration.barrier_buffers[i].element, instance_for(info->scope, context.frame));
            if (handles.buffer == VK_NULL_HANDLE) {
                return false;
            }
            out.barrier_buffer_storage[i] = handles;
        }
        out.barrier_buffers = std::span<resolved_binding const>(out.barrier_buffer_storage.data(), declaration.barrier_buffers.size());

        // ---- the pipelines: the pass's OWN first, then the names the behaviour declares ----
        if (pass.pipeline() != VK_NULL_HANDLE) {
            out.pipeline_storage[0] = pass.pipeline();
            out.pipelines = std::span<VkPipeline const>(out.pipeline_storage.data(), 1);
        } else if (!declaration_pipelines_ok(pass, context, out)) {
            return false;
        }

        out.extent = resolve_extent(pass.behaviour(), context);
        return true;
    }

    inline bool frame_pass::resolve(resolve_context const& context, resolved_io& out) const {
        return resolve_declaration(*this, context, out);
    }

} // namespace deren::vulkan::pass
