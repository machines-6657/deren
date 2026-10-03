// ============================================================================
// module: deren.vulkan.animation
// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))
//
// Keyframe playback / skinning / morph targets, format-neutral and runtime-agnostic:
// driven through an injected `backend` surface and structural `source` concepts -
// imports no loader and no vulkan.runtime. Reuse with any scene storage / source.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

export module deren.vulkan.animation;

import deren.vstd;
import deren.utility;
import deren.vulkan.scene_tree; // scene + node types (pure-CPU scene storage)
import deren.vulkan.primitive;  // GPU leaf primitive + scene capacity constants the rigs drive

/**
 * @file controller.cppm
 * @defgroup vulkan_animation Vulkan Animation Controller
 * @brief bridge between animation/skin/morph data and a scene runtime: plays keyframe
 *        animations by sampling pure CPU, writing the evaluated T/R/S back into scene node
 *        locals, and rebuilding the per-frame skin matrices + morph weights into the host's
 *        per-slot buffers.
 *
 * The controller never depends on the concrete host class (deren::vulkan::runtime) NOR on a concrete
 * animation source format: it talks to whatever owns the scene through an injected
 * backend (callbacks + a scene&), and init() is a TEMPLATE over a source concept - any type
 * exposing the required member shapes (an animations table, a skins table, an asset-node
 * lookup) can drive it. deren::gltf::scenes satisfies the concept and is instantiated at the call
 * site, so this module never imports a loader. Animation data is value-copied into the
 * controller's own format-neutral structures (deren::vulkan::animation) at init(); playback never
 * touches the source afterwards.
 *
 * Contract summary (mirrors make_primitive / import_scene / set_ibl):
 *   - init() registers materials/geometry state and writes every scene block's shared buffers,
 *     so call it before the first frame, or only while the runtime is idle.
 *   - update(dt) writes the paced frame slot's skin/morph buffers and scene node locals, so
 *     call it after the host paced a frame slot and before it records (after the slot's
 *     timeline wait).
 *
 * @note everything animation-related lives in deren::vulkan::animation (the format-neutral data
 *       model, the structural concepts, the backend host surface and the controller), so
 *       names stay short - no animation_/anim prefixes needed inside.
 */
namespace deren::vulkan::animation {
    /**
     * @ingroup vulkan_animation
     * @brief format-neutral animation data model (reference semantics mirror glTF keyframe
     *        animation, but no glTF type is involved): samplers/channels/clips/skins plus the
     *        pure CPU sampling functions. Loaders convert their format into these structures
     *        once; the controller plays them without knowing the source format.
     */
    /** @brief interpolation mode of one animation sampler */
    export enum class interpolation : int32_t {
        linear = 0,       // blend between consecutive keyframes (slerp for rotations)
        step = 1,         // hold the previous keyframe's value until the next keyframe
        cubic_spline = 2, // Hermite spline with per-key in/out tangents
    };

    /** @brief animated node property of one animation channel */
    export enum class channel_path : int32_t {
        translation = 1, // values are xyz triplets (one per keyframe)
        rotation = 2,    // values are xyzw quaternions (w scalar, one per keyframe)
        scale = 3,       // values are xyz triplets (one per keyframe)
        weights = 4,     // morph target weights: per-key scalar block, one value per target
    };

    /** @brief one decoded animation sampler: keyframe times + flat output values */
    export struct sampler {
        std::vector<float> times = {};
        std::vector<float> values = {};
        std::size_t per_key = 0; // values per keyframe (3/4/4/weights; 0 = unknown)
        interpolation interp = interpolation::linear;
    };

    /** @brief one animation channel: animate one property of a node from a sampler */
    export struct channel {
        channel_path path = channel_path::translation;
        std::size_t sampler = 0;     // index into the owning clip's samplers
        std::size_t target_node = 0; // animated node's source index (asset node index)
    };

    /** @brief one playable clip: channels over samplers (mirrors a glTF animation object) */
    export struct clip {
        std::string name = {};
        std::vector<sampler> samplers = {};
        std::vector<channel> channels = {};
    };

    /** @brief a skin: the joints driving a skinned mesh + their inverse bind matrices */
    export struct skin {
        std::string name = {};
        std::vector<std::size_t> joints = {};     // source indices, in joint order
        std::vector<glm::mat4> inverse_bind = {}; // one per joint (identity when omitted)
    };

    /** @brief one node's animated state: TRS base pose overridden by every channel of the
     *         sampled clip that targets it, plus the active morph weights */
    export struct node_pose {
        bool any_channel = false;                // true when at least one channel applied
        bool any_transform = false;              // true when a T/R/S channel applied (local changes)
        glm::vec3 translation = glm::vec3(0.0f); // base pose, overridden per channel path
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale = glm::vec3(1.0f);
        std::vector<float> weights = {}; // active morph weights (weights channel); empty = none
    };

    /**
     * @ingroup vulkan_animation
     * @brief evaluated value of one animation channel at a point in time
     */
    export struct channel_sample {
        bool valid = false;
        glm::vec3 vec3 = glm::vec3(0.0f);                   // translation / scale paths
        glm::quat quat = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // rotation path (normalized)
        std::vector<float> scalars = {};                    // weights path: one value per target
    };

    /** @brief evaluate the channel's sampler at @p t seconds (glTF keyframe sampling rules) */
    export channel_sample sample_channel(sampler const& sampler, channel_path path, float t);

    /**
     * @brief evaluate every channel of @p clip targeting @p target_node at @p t and
     *        merge the results onto the node's TRS base pose
     * @param base the node's base pose (source base pose, e.g. from the loader node)
     * @return merged pose; see channel_sample for the per-channel fill rules
     */
    export node_pose sample_node(clip const& clip, std::size_t target_node, node_pose const& base, float t);

    // ---- module-private helpers (module linkage: visible to this interface's template bodies
    //      AND the implementation unit's non-template members, but not exported; plain functions,
    //      not anonymous-namespace ones, so the init template's unqualified calls resolve) ----
    [[maybe_unused]] float clip_duration(clip const& clip) {
        float duration = 0.0f;
        for (sampler const& sampler : clip.samplers) {
            if (!sampler.times.empty()) {
                duration = std::max(duration, sampler.times.back());
            }
        }
        return duration > 0.0f ? duration : 1.0f;
    }

    [[maybe_unused]] std::string_view display_name(std::string_view const name) {
        return name.empty() ? std::string_view("<unnamed>") : name;
    }

    // The source concepts are STRUCTURAL over the member shapes below (mirroring how
    // scene_tree constrains its iterators): a loader satisfies them with its own pure-CPU
    // types, no shared type identity required. The init template converts the read values into
    // the controller's own structures. Element-level shapes are separate concepts so the
    // top-level one stays readable.

    /** @brief one sampler of a playable clip: keyframe times + flat values + per-key shape */
    export template <class T>
    concept sampler_source = requires(T const& s) {
        requires std::ranges::range<decltype(s.times)>;         // keyframe times
        typename std::ranges::range_value_t<decltype(s.times)>; //   (float keys)
        requires std::convertible_to<std::ranges::range_value_t<decltype(s.times)>, float>;
        requires std::ranges::range<decltype(s.values)>;         // flat keyframe values
        typename std::ranges::range_value_t<decltype(s.values)>; //   (float values)
        requires std::convertible_to<std::ranges::range_value_t<decltype(s.values)>, float>;
        { s.per_key } -> std::convertible_to<std::size_t>;                       // values per keyframe
        requires std::is_enum_v<std::remove_cvref_t<decltype(s.interpolation)>>; // interpolation mode
    };

    /** @brief one channel of a playable clip: property path + sampler + target node */
    export template <class T>
    concept channel_source = requires(T const& c) {
        requires std::is_enum_v<std::remove_cvref_t<decltype(c.path)>>; // property path
        { c.sampler } -> std::convertible_to<std::size_t>;              // sampler index
        { c.target_node } -> std::convertible_to<std::size_t>;          // animated node
    };

    /** @brief one playable clip: name + samplers + channels */
    export template <class T>
    concept clip_source = requires(T const& a) {
        { a.name } -> std::convertible_to<std::string_view>; // clip name
        requires std::ranges::range<decltype(a.samplers)>;
        typename std::ranges::range_value_t<decltype(a.samplers)>;
        requires sampler_source<std::ranges::range_value_t<decltype(a.samplers)>>;
        requires std::ranges::range<decltype(a.channels)>;
        typename std::ranges::range_value_t<decltype(a.channels)>;
        requires channel_source<std::ranges::range_value_t<decltype(a.channels)>>;
    };

    /** @brief one skin: name + joint list + inverse bind matrices */
    export template <class T>
    concept skin_source = requires(T const& k) {
        { k.name } -> std::convertible_to<std::string_view>;
        requires std::ranges::range<decltype(k.joints)>;
        typename std::ranges::range_value_t<decltype(k.joints)>;
        requires std::convertible_to<std::ranges::range_value_t<decltype(k.joints)>, std::size_t>;
        requires std::ranges::range<decltype(k.inverse_bind_matrices)>;
        typename std::ranges::range_value_t<decltype(k.inverse_bind_matrices)>;
        requires std::convertible_to<std::ranges::range_value_t<decltype(k.inverse_bind_matrices)>, glm::mat4>;
    };

    /** @brief one mesh's primitive: base attributes (POSITION etc.) + morph targets */
    export template <class T>
    concept primitive_source = requires(T const& p) {
        requires std::ranges::range<decltype(p.vertex)>;  // base attribute map (POSITION lookup)
        requires std::ranges::range<decltype(p.targets)>; // morph targets (may be empty)
    };

    /** @brief one mesh of a node: primitives (morph deltas) + default morph weights */
    export template <class T>
    concept mesh_source = requires(T const& m) {
        requires std::ranges::range<decltype(m.primitives)>;
        typename std::ranges::range_value_t<decltype(m.primitives)>;
        requires primitive_source<std::ranges::range_value_t<decltype(m.primitives)>>;
        requires std::ranges::range<decltype(m.weights)>; // default morph weights
    };

    /** @brief one node's metadata: TRS base pose + optional skin ref + attached meshes */
    export template <class T>
    concept node_source = requires(T const& n) {
        { n.translation } -> std::convertible_to<glm::vec3>;
        { n.rotation } -> std::convertible_to<glm::quat>;
        { n.scale } -> std::convertible_to<glm::vec3>;
        n.skin_index;                                    // optional asset-node index of the skin driving this node's mesh
        requires std::ranges::range<decltype(n.meshes)>; // meshes carry morph delta data
        typename std::ranges::range_value_t<decltype(n.meshes)>;
        requires mesh_source<std::ranges::range_value_t<decltype(n.meshes)>>;
    };

    /**
     * @ingroup vulkan_animation
     * @brief an animation data source: what controller::init() needs from a loaded
     *        file. Structural concept - any type exposing these member shapes can drive the
     *        controller (deren::gltf::scenes satisfies it; a future format just implements the same
     *        shapes). The members mirror what the glTF loader already provides:
     *        - animations: the file's keyframe animations (channel-bearing ones are playable)
     *        - skins: joint lists + inverse bind matrices
     *        - node_by_source: asset node index -> node metadata (TRS base pose + skin ref +
     *          attached morph mesh data), queried per scene-tree node's source_index
     */
    export template <class S>
    concept source = requires(S const& s) {
        requires std::ranges::range<decltype(s.animations)>; // playable clips
        typename std::ranges::range_value_t<decltype(s.animations)>;
        requires clip_source<std::ranges::range_value_t<decltype(s.animations)>>;
        requires std::ranges::range<decltype(s.skins)>; // skins (may be empty)
        typename std::ranges::range_value_t<decltype(s.skins)>;
        requires skin_source<std::ranges::range_value_t<decltype(s.skins)>>;
        requires std::ranges::range<decltype(s.node_by_source)>;                                  // asset node index -> node*
        typename std::tuple_element_t<1, std::ranges::range_value_t<decltype(s.node_by_source)>>; // node const*
        requires node_source<std::remove_pointer_t<std::tuple_element_t<1, std::ranges::range_value_t<decltype(s.node_by_source)>>>>;
    };

    /**
     * @ingroup vulkan_animation
     * @brief the host surface a controller drives, injected at init(): the scene
     *        tree it mutates plus callbacks for everything else it needs from the host.
     *
     * Kept deliberately narrow: only what per-frame playback touches. The scene is a
     * direct reference (animation must walk and edit nodes in place); the rest are callbacks
     * so the controller does not depend on the host class - any object exposing the same
     * surface can drive animations. Assemble it on the host side (see chores).
     */
    export struct backend {
        deren::vulkan::scene_tree::scene* scene = nullptr; // tree to animate (nullptr = not bound)

        // ---- per-frame (active slot) access, used by update() ----
        std::function<float*()> morph_scratch_active;                             // host-visible morph scratch of the paced slot
        std::function<void(std::span<glm::mat4 const>)> set_skin_matrices_active; // upload skin matrices to the paced slot

        // ---- setup-time (explicit slot) access, used by init() ----
        std::function<float*(uint32_t)> morph_scratch_slot;                               // morph scratch of one frame slot
        std::function<void(std::span<glm::mat4 const>, uint32_t)> set_skin_matrices_slot; // upload to one frame slot

        // ---- frame-loop cooperation ----
        std::function<void()> scene_changed;                             // node locals edited -> caller invalidates caches
        std::function<void(std::span<std::function<void()>>)> run_tasks; // fan tasks out on the host's worker pool (sync)
        std::function<int32_t()> task_worker_count;                      // pool workers, for slicing fan-out tasks
    };

    /**
     * @ingroup vulkan_animation
     * @brief plays keyframe animation on a scene tree: owns the playback clock and the
     *        value-copied clip/skin data, samples the active clip into scene node locals and
     *        rebuilds the per-frame skin matrices + morph weights into the host's per-slot
     *        buffers.
     */
    export class controller {
    public:
        /**
         * @ingroup vulkan_animation
         * @brief build the playback table and resolve the skin/morph rigs against the
         *        backend's scene: collect the playable (channel-bearing) clips, map the scene
         *        tree's nodes onto their source metadata (TRS base poses etc. via the source's
         *        asset node table), bake the morph deltas with their default weights into every
         *        frame slot's morph buffer and upload the identity skin block into every slot's
         *        skin buffer. Skinned/morphable primitives get their push.skin_base /
         *        push.morph_* fields set here.
         * @param scenes any type satisfying source (deren::gltf::scenes does): clip keyframes +
         *        skins + mesh (morph) data. Only consulted as DATA; the authoritative node
         *        host is host.scene (the scene tree the controller animates) - nodes not in
         *        that tree are ignored.
         * @param host the host surface to drive (scene + per-slot callbacks)
         * @param import_shift translation the import applied to every scene ROOT node's local
         *        (animated roots must re-apply it, like import_scene did)
         * @note call before the first frame, or only while the host is idle (no frame in
         *       flight) - this writes scene buffers/descriptors like make_primitive() does.
         * @note a template: the definition is in this interface so any TU that imports the
         *       module can instantiate it at the call site with a concrete source type.
         */
        template <source S>
        void init(S const& scenes, backend const& host, glm::vec3 const& import_shift) {
            this->host = host;
            this->import_shift = import_shift;

            // live-tree lookup: asset node index -> host scene nodes + root flag (import applied
            // the shift to root locals only, so animated roots must re-apply it). scene_iterator
            // walks the whole tree in DFS pre-order; roots sit at depth 0. The SCENE TREE is the
            // authoritative host: only sources that actually live in it are animated.
            for (auto it = deren::vulkan::scene_tree::begin(*this->host.scene); it != deren::vulkan::scene_tree::end(*this->host.scene); ++it) {
                this->source_nodes[it->source_index].push_back(node_target{&*it, /*scene_root=*/it.depth() == 0});
            }

            // TRS base pose per TREE node: look each tree node's asset source up in the source's
            // asset-level node table (scenes.node_by_source) instead of iterating per-scene node
            // pools - a node referenced by several scenes has identical copies, and the tree only
            // contains the nodes that were actually imported. The pose is value-copied into the
            // controller's own node_pose (no source type retained).
            for (auto const& [source, targets] : this->source_nodes) {
                auto const loader_it = scenes.node_by_source.find(source);
                if (loader_it == scenes.node_by_source.end()) {
                    continue; // synthesized tree node (e.g. an extra "/prim" leaf) has no source node
                }
                auto const& loader_node = *loader_it->second;
                node_pose base = {};
                base.translation = loader_node.translation;
                base.rotation = loader_node.rotation;
                base.scale = loader_node.scale;
                this->base_poses.try_emplace(source, std::move(base));
            }

            // playable table: channel-bearing clips, in source order, VALUE-COPIED into the
            // controller's own clip structures (samplers/channels converted once, so playback
            // never touches the source data afterwards); auto-pick the first
            for (auto const& candidate : scenes.animations) {
                if (candidate.channels.empty()) {
                    continue;
                }
                clip converted = {};
                converted.name = candidate.name;
                converted.samplers.reserve(candidate.samplers.size());
                for (auto const& loader_sampler : candidate.samplers) {
                    sampler s = {};
                    s.times = loader_sampler.times;
                    s.values = loader_sampler.values;
                    s.per_key = loader_sampler.per_key;
                    s.interp = static_cast<interpolation>(loader_sampler.interpolation);
                    converted.samplers.push_back(std::move(s));
                }
                converted.channels.reserve(candidate.channels.size());
                for (auto const& loader_channel : candidate.channels) {
                    channel c = {};
                    c.path = static_cast<channel_path>(loader_channel.path);
                    c.sampler = loader_channel.sampler;
                    c.target_node = loader_channel.target_node;
                    converted.channels.push_back(c);
                }
                this->playable_clips.push_back(std::move(converted));
            }
            this->max_duration = 1.0f;
            for (clip const& playable : this->playable_clips) {
                this->max_duration = std::max(this->max_duration, clip_duration(playable));
            }
            if (!this->playable_clips.empty()) {
                this->active = &this->playable_clips[0];
                this->current_index = 0;
                this->time = 0.0f;
                this->duration = clip_duration(*this->active);
                this->debug_source = this->pick_debug_source(*this->active);
                this->refresh_debug_name();
                deren::utility::log("animation: playing '{}' ({} channels, {:.2f}s loop)", display_name(this->active->name), this->active->channels.size(), this->duration);
            }

            // Parallel sampling decision ("lite" fan-out, not full core count): only when the
            // clip is heavy enough that per-source sampling (each source scans all channels) is
            // worth splitting across the backend's shared task pool. Light clips (a handful of
            // channels) stay on the caller thread - the pool sync would cost more than the work.
            // The pool itself lives on the host (injected as host.run_tasks), so we only record
            // the decision here + a stable source list to slice update()'s sampling over.
            {
                std::size_t max_channels = 0;
                for (clip const& playable : this->playable_clips) {
                    max_channels = std::max(max_channels, playable.channels.size());
                }
                if (max_channels >= 32 && this->source_nodes.size() >= 64) {
                    this->parallel_sampling = true;
                    // stable source list for slicing update()'s sampling across the pool workers
                    // (source_nodes is fixed after init; select() only swaps the active clip)
                    this->sample_keys.reserve(this->source_nodes.size());
                    for (auto const& [source, targets] : this->source_nodes) {
                        this->sample_keys.push_back(source);
                    }
                    deren::utility::log("animation: parallel sampling enabled ({} channels / {} sources, runtime task pool)", max_channels, this->sample_keys.size());
                }
            }

            // ---- skin rigs: resolve each exported skin that drives an imported mesh ----
            if (!scenes.skins.empty()) {
                uint32_t next_block = 4; // identity block occupies indices 0-3
                for (std::size_t skin_id = 0; skin_id < scenes.skins.size(); ++skin_id) {
                    auto const& loader_skin = scenes.skins[skin_id];
                    // EVERY source node referencing this skin that is present in the tree - not the first
                    // one: see skin_rig::mesh_sources.
                    std::vector<std::size_t> mesh_sources = {};
                    for (auto const& [source, loader_node] : scenes.node_by_source) {
                        if (loader_node->skin_index && *loader_node->skin_index == skin_id && this->source_nodes.contains(source)) {
                            mesh_sources.push_back(source);
                        }
                    }
                    if (mesh_sources.empty()) {
                        continue; // the skin is not used by the imported scene
                    }
                    bool const all_joints_present = std::ranges::all_of(loader_skin.joints, [this](std::size_t const joint) { return this->source_nodes.contains(joint); });
                    if (!all_joints_present) {
                        deren::utility::log("skinning: skin '{}' skipped (joint(s) missing from the imported scene)", display_name(loader_skin.name));
                        continue;
                    }
                    if (static_cast<uint32_t>(loader_skin.joints.size()) > deren::vulkan::scene_skin_capacity - next_block) {
                        deren::utility::log("skinning: skin '{}' skipped ({} joints, skin matrix buffer capacity {} exceeded)", display_name(loader_skin.name), loader_skin.joints.size(), deren::vulkan::scene_skin_capacity);
                        continue;
                    }
                    uint32_t const block_base = next_block;
                    next_block += static_cast<uint32_t>(loader_skin.joints.size());
                    // point every primitive leaf of every skinned node at the block: the node's own
                    // leaf plus extra-primitive child leaves (import adds them under the node with
                    // source_index 0); real child nodes keep skin_base 0. MULTIPLE NODES, NOT JUST THE
                    // FIRST: two nodes may reference this skin, and each of them may be instantiated
                    // several times (source_nodes maps a source index to every node using it).
                    auto const assign_block = [block_base](auto&& self, deren::vulkan::scene_tree::scene_node& node, std::size_t const source) -> void {
                        if (node.primitive_leaf != nullptr && (node.source_index == 0 || node.source_index == source)) {
                            static_cast<deren::vulkan::primitive*>(node.primitive_leaf.get())->push.skin_base = block_base;
                        }
                        for (deren::vulkan::scene_tree::scene_node& child : node.children) {
                            self(self, child, source);
                        }
                    };
                    for (std::size_t const source : mesh_sources) {
                        for (auto const& entry : this->source_nodes.at(source)) {
                            assign_block(assign_block, *entry.node, source);
                        }
                    }
                    if (mesh_sources.size() > 1) {
                        deren::utility::log("skinning: skin '{}' is used by {} nodes; all of them now point at its joint block", display_name(loader_skin.name), mesh_sources.size());
                    }
                    // value-copy the skin (joints + inverse bind matrices) into the rig
                    skin s = {};
                    s.name = loader_skin.name;
                    s.joints = loader_skin.joints;
                    s.inverse_bind = loader_skin.inverse_bind_matrices;
                    this->skin_rigs.push_back(skin_rig{std::move(s), std::move(mesh_sources), block_base});
                }
                // wanted set for the per-frame world collection: every accepted rig's mesh node +
                // every joint it references (deduplicated; fixed after this init pass)
                for (skin_rig const& rig : this->skin_rigs) {
                    for (std::size_t const source : rig.mesh_sources) {
                        this->skin_sources.insert(source);
                    }
                    for (std::size_t const joint : rig.s.joints) {
                        this->skin_sources.insert(joint);
                    }
                }
                // dense slot map over the wanted set: update() writes each wanted node's world
                // matrix into a REUSED vector (skin_world_cache) instead of building a fresh
                // unordered_map every frame
                this->skin_world_index.reserve(this->skin_sources.size());
                this->skin_world_cache.reserve(this->skin_sources.size());
                for (std::size_t const wanted : this->skin_sources) {
                    this->skin_world_index.emplace(wanted, static_cast<uint32_t>(this->skin_world_cache.size()));
                    this->skin_world_cache.emplace_back(); // placeholder; rewritten every frame
                }
                if (!skin_rigs.empty()) {
                    deren::utility::log("skinning: {} skin rig(s) active ({} joint matrix block(s) + identity block)", this->skin_rigs.size(), next_block - 4);
                    this->skin_debug_name = std::string(display_name(this->skin_rigs.front().s.name));
                }
            }
            // identity block for unskinned draws: upload once into EVERY slot's skin buffer
            {
                constexpr std::array<glm::mat4, 4> identity_block = {glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)};
                for (uint32_t slot = 0; slot < deren::vulkan::core::MAX_FRAMES_IN_FLIGHT; ++slot) {
                    this->host.set_skin_matrices_slot(identity_block, slot);
                }
            }

            // ---- morph rigs: bake deltas + default weights into every slot's morph buffer ----
            float* const morph_scratch_mem = this->host.morph_scratch_slot(0);
            if (morph_scratch_mem != nullptr) {
                // read one float delta attribute of a morph target. The source only keeps FLOAT
                // morph deltas (glTF loader drops non-float target attributes), so the data can
                // be read directly as glm::vec3 without a component-type check.
                auto const read_delta_vec3 = [](auto const& attrs, std::string_view const name, std::size_t const i) -> glm::vec3 {
                    auto const it = attrs.find(std::string(name));
                    if (it == attrs.end()) {
                        return glm::vec3(0.0f); // missing delta -> no displacement
                    }
                    return reinterpret_cast<glm::vec3 const*>(it->second.data.data())[i];
                };
                // collect leaves per effective source (a "/prim" extra leaf inherits its parent's source)
                std::unordered_map<std::size_t, std::vector<deren::vulkan::primitive*>> source_leaves;
                auto const collect_leaves = [&source_leaves](auto&& self, deren::vulkan::scene_tree::scene_node& node, std::size_t const parent_source) -> void {
                    bool const is_extra = node.node_name.ends_with("/prim");
                    std::size_t const source = is_extra ? parent_source : node.source_index;
                    if (node.primitive_leaf != nullptr) {
                        source_leaves[source].push_back(static_cast<deren::vulkan::primitive*>(node.primitive_leaf.get()));
                    }
                    for (deren::vulkan::scene_tree::scene_node& child : node.children) {
                        self(self, child, source);
                    }
                };
                for (deren::vulkan::scene_tree::scene_node& root : this->host.scene->roots) {
                    collect_leaves(collect_leaves, root, 0);
                }
                std::size_t total_floats = 0;
                for (auto& [source, leaves] : source_leaves) {
                    auto const loader_it = scenes.node_by_source.find(source);
                    if (loader_it == scenes.node_by_source.end()) {
                        continue;
                    }
                    auto const& loader_node = *loader_it->second;
                    // flatten every mesh's primitives of this node (same order the tree leaves
                    // were imported in) into one list so leaves[i] pairs with prim i
                    using prim_t = std::remove_reference_t<decltype(loader_node.meshes.front().primitives.front())>;
                    std::vector<prim_t const*> loader_prims;
                    for (auto const& mesh : loader_node.meshes) {
                        for (auto const& prim : mesh.primitives) {
                            loader_prims.push_back(&prim);
                        }
                    }
                    // default weights: node.weights override, else the mesh defaults, else zeros
                    std::vector<float> default_weights;
                    if (loader_node.weights) {
                        default_weights = *loader_node.weights;
                    } else if (!loader_node.meshes.empty()) {
                        default_weights = loader_node.meshes[0].weights;
                    }
                    for (std::size_t i = 0; i < leaves.size() && i < loader_prims.size(); ++i) {
                        auto const& loader_prim = *loader_prims[i];
                        if (loader_prim.targets.empty()) {
                            continue;
                        }
                        auto const pos_portion = loader_prim.vertex.find("POSITION");
                        if (pos_portion == loader_prim.vertex.end()) {
                            continue;
                        }
                        uint32_t const verts = leaves[i]->vertex_count;
                        uint32_t const target_count = static_cast<uint32_t>(loader_prim.targets.size());
                        if (pos_portion->second.data.size() / sizeof(glm::vec3) != verts) {
                            deren::utility::log("morph: skipping primitive (vertex count mismatch with its POSITION data)");
                            continue;
                        }
                        // SPARSE morph block: only the (vertex, target) pairs whose delta is non-zero are
                        // stored, so the block is proportional to the DEFORMATION rather than to
                        // verts * targets.  A dense stride over a character that shares one 70,824-vertex
                        // buffer between 35 primitives and carries 43 facial morphs asks for ~70 MB per
                        // primitive; the sparse form asks for ~1 MB, which is the difference between a
                        // face that animates and one that is skipped outright.
                        //
                        // TWO weight regions per primitive, not one: the current weights and the weights
                        // this vertex had ONE FRAME AGO, which is what gives a MORPHING mesh a motion
                        // vector that carries its deformation (see docs/deformation_motion_vectors.md).
                        // They come FIRST so the shader can locate every other region from morph_base,
                        // morph_targets and morph_vertices alone - adding a field to the shared push block
                        // would have meant touching every pass.  Layout, mirrored in
                        // pbr_shade_vertex (pbr.slang) and shadow_shade_vertex (shadow.slang):
                        //   [weights: targets][previous weights: targets][offsets: verts + 1][entries: 7]
                        // An entry is (target, dpos.xyz, dnrm.xyz); the offsets array is what turns the walk
                        // over a vertex's moved targets into O(moved) instead of a search per target.
                        std::size_t const weight_floats = static_cast<std::size_t>(target_count) * 2u;
                        std::size_t const offset_floats = static_cast<std::size_t>(verts) + 1u;
                        // pass 1: the per-vertex offsets, which also gives the entry count the capacity
                        // check needs - a single pass could only find that out after writing
                        std::vector<std::uint32_t> sparse_offsets(offset_floats, 0u);
                        std::uint32_t entries = 0;
                        for (uint32_t v = 0; v < verts; ++v) {
                            sparse_offsets[v] = entries;
                            for (uint32_t t = 0; t < target_count; ++t) {
                                glm::vec3 const dpos = read_delta_vec3(loader_prim.targets[t].attributes, "POSITION", v);
                                glm::vec3 const dnrm = read_delta_vec3(loader_prim.targets[t].attributes, "NORMAL", v);
                                if (glm::dot(dpos, dpos) > 0.0f || glm::dot(dnrm, dnrm) > 0.0f) {
                                    ++entries;
                                }
                            }
                        }
                        sparse_offsets[verts] = entries;
                        std::size_t const entry_floats = static_cast<std::size_t>(entries) * 7u;
                        if (total_floats + weight_floats + offset_floats + entry_floats > deren::vulkan::scene_morph_capacity) {
                            deren::utility::log("morph: scene morph buffer capacity exceeded, remaining primitives skipped");
                            break;
                        }
                        float* dst = morph_scratch_mem + total_floats;
                        std::vector<float> rig_defaults;
                        rig_defaults.reserve(target_count);
                        for (uint32_t t = 0; t < target_count; ++t) {
                            float const w = t < default_weights.size() ? default_weights[t] : 0.0f;
                            *dst++ = w;
                            rig_defaults.push_back(w);
                        }
                        // ... and the previous-weights region starts out EQUAL to the current one, so a
                        // morphable primitive that nothing animates reports no morph deformation at all
                        // (its two regions stay equal) instead of reporting the distance from zero weights.
                        // A per-frame writer keeps them in step by copying the current region forward before
                        // it overwrites it - see the weight write in update().
                        for (uint32_t t = 0; t < target_count; ++t) {
                            *dst++ = rig_defaults[t];
                        }
                        // offsets ride in the same float buffer as the data, which is exact while they stay
                        // below 2^24 - a vertex count, so they do
                        for (uint32_t v = 0; v <= verts; ++v) {
                            *dst++ = static_cast<float>(sparse_offsets[v]);
                        }
                        // pass 2: the entries themselves, in the same (vertex, target) order the offsets
                        // were built in, so a vertex's entries are contiguous
                        for (uint32_t v = 0; v < verts; ++v) {
                            for (uint32_t t = 0; t < target_count; ++t) {
                                glm::vec3 const dpos = read_delta_vec3(loader_prim.targets[t].attributes, "POSITION", v);
                                glm::vec3 const dnrm = read_delta_vec3(loader_prim.targets[t].attributes, "NORMAL", v);
                                if (glm::dot(dpos, dpos) <= 0.0f && glm::dot(dnrm, dnrm) <= 0.0f) {
                                    continue;
                                }
                                *dst++ = static_cast<float>(t);
                                *dst++ = dpos.x;
                                *dst++ = dpos.y;
                                *dst++ = dpos.z;
                                *dst++ = dnrm.x;
                                *dst++ = dnrm.y;
                                *dst++ = dnrm.z;
                            }
                        }
                        this->morph_rigs.push_back(morph_rig{leaves[i], verts, target_count, static_cast<uint32_t>(total_floats), source, std::move(rig_defaults)});
                        leaves[i]->push.morph_base = static_cast<uint32_t>(total_floats);
                        leaves[i]->push.morph_targets = target_count;
                        leaves[i]->push.morph_vertices = verts;
                        total_floats += weight_floats + offset_floats + entry_floats;
                    }
                }
                if (!this->morph_rigs.empty()) {
                    deren::utility::log("morph: baked {} morphable primitive(s) into the scene morph buffer ({} floats)", this->morph_rigs.size(), total_floats);
                    // duplicate the baked blocks (contiguous [0, total_floats)) into every other
                    // frame slot's morph buffer: deltas are static, only the per-frame weight
                    // rewrites target the active slot's buffer
                    for (uint32_t slot = 1; slot < deren::vulkan::core::MAX_FRAMES_IN_FLIGHT; ++slot) {
                        float* const other = this->host.morph_scratch_slot(slot);
                        if (other != nullptr) {
                            std::memcpy(other, morph_scratch_mem, total_floats * sizeof(float));
                        }
                    }
                }
            }
        }

        // ---- playback table / gui binding ----

        /**
         * @brief the WORLD matrix a JOINT had in the frame this controller last produced, or nothing
         *
         * WHY THIS IS A PUBLIC READER RATHER THAN A CALLBACK: the face SDF shades a face in the HEAD'S frame, and
         * that frame is `-row3` / `-row1` of this matrix (see gltf_loader's `head_basis_from_axes`). Pushing it
         * through a host callback would mean the controller had to know what a head basis is; reading it means
         * the layer that already knows - the application, which links both the loader and the runtime - asks a
         * question about a joint and gets a matrix.
         *
         * THE CACHE IT READS IS REBUILT EVERY FRAME (`skin_world_cache`, indexed through `skin_world_index`), so
         * the answer is the pose the last update produced rather than a stale one, and a caller that reads it
         * after `update()` and before the next one gets a consistent frame.
         *
         * @param rig_index index into the active skin rigs, in the order the scene was imported
         * @param joint_index index into that rig's `skin::joints` - the SAME number `deren::gltf::head_joint_of`
         *        returns, and NOT an asset node index; the two differ and confusing them reads the wrong bone
         * @return the joint's world matrix, or nothing when either index is out of range or the joint was not
         *         collected into the cache - a caller has no matrix to use, which is not an error
         */
        [[nodiscard]] std::optional<glm::mat4> joint_world(std::size_t rig_index, std::size_t joint_index) const noexcept;

        /** @brief number of active skin rigs, so a caller can iterate them without reaching into the internals */
        [[nodiscard]] std::size_t skin_rig_count() const noexcept;

        /** @brief number of channel-bearing clips (the combo lists these) */
        [[nodiscard]] std::size_t playable_count() const noexcept;
        /** @brief display name of playable @p index ("<unnamed>" when the glTF has none) */
        [[nodiscard]] std::string_view playable_name(std::size_t index) const noexcept;
        /** @brief longest playable duration (fixed slider range, like the old demo combo) */
        [[nodiscard]] float playable_max_duration() const noexcept;
        /** @brief true when a playable clip is selected (auto-picks the first on init) */
        [[nodiscard]] bool has_active() const noexcept;
        /** @brief index of the active playable in the playable list */
        [[nodiscard]] std::size_t current() const noexcept;
        /** @brief switch to playable @p index: reset every animated node to its base pose (so
         *         nodes the previous clip moved but the new one does not return), then set
         *         time to zero. Keeps the playing flag as-is.
         */
        void select(std::size_t index);
        /** @brief pause/resume the clock (sampling keeps running while paused) */
        void set_playing(bool playing) noexcept;
        /** @brief whether the clock advances each update() */
        [[nodiscard]] bool is_playing() const noexcept;
        /** @brief scrub to @p t seconds and pause (mirrors the gui time-slider behavior: the
         *         next update() samples the new time without the clock fighting the drag) */
        void set_time(float t);
        /** @brief current playback time in seconds */
        [[nodiscard]] float current_time() const noexcept;
        /** @brief loop length of the active clip in seconds */
        [[nodiscard]] float loop_duration() const noexcept;

        /**
         * @ingroup vulkan_animation
         * @brief advance and apply one frame: sample the active clip at the (possibly
         *        advanced) time, write each animated node's T/R/S local (scene roots keep the
         *        import shift) and mark the scene changed, write the active frame slot's morph
         *        weights, then rebuild + upload the skin matrices into the active slot.
         * @param dt_seconds clock advance when playing (e.g. frame_clock::delta_seconds())
         * @note call after runtime.pace_and_acquire() and before runtime.begin_recording(): the runtime's
         *       per-slot buffers may only be written once pace_and_acquire() paced the slot.
         */
        void update(float dt_seconds);

        // ---- read-only bridge for demo-side consumers ----

        /** @brief whether an asset node index occurs in the runtime scene tree */
        [[nodiscard]] bool has_runtime_node(std::size_t source) const noexcept;

        // ---- per-second diagnostics (demo log lines) ----

        /** @brief display name of the active clip ("" when none) */
        [[nodiscard]] std::string_view active_name() const noexcept;
        /** @brief name of the reported animated node ("" when none) */
        [[nodiscard]] std::string_view get_debug_node_name() const noexcept;
        /** @brief translation of the reported animated node this frame */
        [[nodiscard]] glm::vec3 get_debug_translation() const noexcept;
        /** @brief true when the first skin rig's last joint world was resolved this frame */
        [[nodiscard]] bool is_skin_debug_valid() const noexcept;
        /** @brief world x-axis of the first skin rig's LAST joint (rotation debug) */
        [[nodiscard]] glm::vec3 get_skin_debug_translation() const noexcept;
        /** @brief display name of the first active skin rig ("" when none) */
        [[nodiscard]] std::string_view get_skin_debug_name() const noexcept;

    private:
        struct node_target {
            deren::vulkan::scene_tree::scene_node* node = nullptr;
            bool scene_root = false;
        };
        struct skin_rig {
            skin s = {}; // value-copied joints + inverse bind matrices
            // EVERY asset node that references this skin. glTF lets several nodes share one skin - two
            // meshes, or one mesh instanced twice - and all of them have to point at this rig's joint
            // block and have their world matrices collected; a single index here rigged only the first.
            std::vector<std::size_t> mesh_sources = {};
            uint32_t block_base = 0; // block start in the skin buffer (after identity)
        };
        struct morph_rig {
            deren::vulkan::primitive* prim = nullptr;
            uint32_t vertex_count = 0;
            uint32_t target_count = 0;
            uint32_t morph_base = 0; // float index into the morph buffer
            std::size_t source = 0;  // owning loader node (weights channel target)
            // baked rest weights (target_count floats): written when no clip animates this
            // source's weights, so a previous clip's weights cannot linger after a clip switch
            std::vector<float> default_weights = {};
        };

        backend host; // injected host surface (scene + callbacks); scene == nullptr when unbound
        glm::vec3 import_shift{};
        // whether this scene's animation is heavy enough to fan sampling out over the backend's
        // shared task pool (many channels over many sources): decided in init(), used by update()
        bool parallel_sampling = false;
        // source keys in stable order for parallel sampling (the source set is fixed after
        // init(); sample_keys mirrors source_nodes's keys so update() can slice them)
        std::vector<std::size_t> sample_keys = {};
        // value-copied playable clips (channel-bearing, in source order). Filled once in
        // init() and never mutated afterwards, so active may point into it safely.
        // playable_clips, not playable: init()'s range-for loops bind `clip const&
        // playable`, which would hide a member of that name (MSVC /W4 C4458, an error
        // under /WX).
        std::vector<clip> playable_clips = {};
        std::unordered_map<std::size_t, std::vector<node_target>> source_nodes = {};
        std::unordered_map<std::size_t, node_pose> base_poses = {};
        clip const* active = nullptr; // == &playable_clips[current_index] when has_active()
        std::size_t current_index = 0;
        float time = 0.0f;
        float duration = 1.0f;
        float max_duration = 1.0f;
        /// Deliberately NOT called `playing`: set_playing()'s parameter of that name would hide it and
        /// MSVC /W4 reports C4458, an error under /WX (clang does not warn: -Wshadow is not enabled
        /// there). Not `is_playing` either - that name belongs to the is_playing() accessor below
        /// (MSVC C2365 / clang "redefinition as different kind of symbol").
        bool playing_flag = true;
        std::size_t debug_source = std::numeric_limits<std::size_t>::max();
        glm::vec3 debug_translation{};
        std::string debug_node_name = {};
        std::vector<skin_rig> skin_rigs = {};
        std::vector<morph_rig> morph_rigs = {};
        // ---- per-frame scratch, reused across update() calls to keep the hot path
        //      allocation-free; only touched on the caller's frame thread ----
        std::atomic<bool> sampling_changed = false;             // any sampling slice moved a node
        std::vector<std::function<void()>> sampling_tasks = {}; // parallel sampling fan-out
        // skin world collection: dense slot map (source -> cache index, fixed after init) plus
        // a reused per-frame world cache, so update() never allocates a per-frame map
        std::unordered_map<std::size_t, uint32_t> skin_world_index = {};
        std::vector<glm::mat4> skin_world_cache = {};
        std::vector<glm::mat4> skin_matrices_scratch = {}; // per-frame skin upload buffer
        bool skin_debug_valid = false;
        glm::vec3 skin_debug_translation{};
        std::string skin_debug_name = {};
        // asset node indices whose world matrix update() must collect each frame: every rig's
        // mesh node plus every joint it references. Fixed after init(); the per-frame DFS only
        // tests each visited node against this set (O(1) contains) instead of scanning every
        // rig x joint pair per node.
        std::unordered_set<std::size_t> skin_sources = {};

        // the node reported per second: prefer a translation channel target, fall back to the
        // first channel target present in the tree
        std::size_t pick_debug_source(clip const& clip) const;
        void refresh_debug_name();

        // sample one loader source into its scene nodes + the active slot's morph weights at
        // this->time; returns whether any node local moved (morph-only writes are not
        // "changed": they do not invalidate the culling BVH). A member function so the
        // sampling fan-out tasks only capture `this` (+ their source range): the task list is
        // self-contained and can be handed to the host's run_tasks for pool execution.
        bool sample_source(std::size_t source, std::vector<node_target> const& targets);
    };
} // namespace deren::vulkan::animation
