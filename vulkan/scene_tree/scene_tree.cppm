// ============================================================================
// module: deren.vulkan.scene_tree  (peer of deren.vulkan.runtime - the scene tree the
//         frame facade renders; versioned in lock-step with deren.vulkan.runtime,
//         see that module's banner: they share the scene / draw interface)
// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))
//
// Scene storage + the abstract leaf interface the GPU primitives implement
// (pure CPU - glm + vstd only):
//   - scene / scene_node { name, local, children, primitive_leaf } with the
//     node tree API (add_root / add_child / attach / clone / find)
//   - the abstract leaf interface scene_tree::primitive (pure virtual
//     set_world); the CPU-side walkers update_world() / visit_primitives()
//     accumulate world transforms every frame
//   - the structural iterator concepts (scene_iterator + the import template
//     iterators) that drive runtime import
// The GPU drawables (deren::vulkan::primitive + draw strategies) and the
// material / camera / light UBO records live in the peer module vulkan.primitive.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - in lock-step with vulkan.runtime.
// ============================================================================
module;

#include <cstddef>
#include <glm/glm.hpp>

export module deren.vulkan.scene_tree;
export import deren.vstd;

/**
 * @file scene_tree.cppm
 * @defgroup vulkan_scene_tree Vulkan Scene Tree
 * @brief pure-CPU scene storage: a transform hierarchy of scene_node objects
 *        with primitive leaves, where the leaf is the abstract
 *        scene_tree::primitive interface (set_world).
 *
 * @details
 *   - storage: scene / scene_node { name, local, children, primitive_leaf };
 *     the CPU-side walkers update_world() / visit_primitives() accumulate
 *     world transforms every frame
 *   - the abstract leaf interface scene_tree::primitive (pure virtual
 *     set_world) - GPU drawables implement it (deren.vulkan.primitive)
 *   - the structural iterator concepts (scene_iterator / the import template
 *     iterators) the runtime import templates drive
 * @note the GPU drawables (deren::vulkan::primitive + normal/instanced/static draw
 *       strategies) and the material / camera / light UBO records live in the
 *       peer module deren.vulkan.primitive; this module is pure CPU and does not
 *       import it (leaves are held through the abstract interface).
 */
namespace deren::vulkan::scene_tree {
    /**
     * @ingroup vulkan_scene_tree
     * @brief first motion slot of a leaf in the runtime's previous-transform buffer, or
     *        @c no_motion_slot when the leaf is not tracked frame to frame
     */
    export constexpr uint32_t no_motion_slot = 0xFFFFFFFFu;

    /**
     * @ingroup vulkan_scene_tree
     * @brief abstract primitive leaf of a scene node (the interface the GPU primitives
     *        below implement: normal_draw_primitive / instanced_draw_primitive)
     * @note pure interface: implementations own their GPU geometry and record
     *       their draw commands; scene_tree only feeds them their accumulated
     *       world transform every frame
     */
    export class primitive {
    public:
        virtual ~primitive() = default;

        /**
         * @brief store the accumulated world transform of the owning node
         * @param world parent_world * node.local (computed by update_world)
         */
        virtual void set_world(glm::mat4 const& world) = 0;

        /**
         * @brief first slot this leaf owns in the runtime's previous-transform buffer (scene set
         *        binding 13), i.e. where its world matrix from one frame ago is kept
         * @return the slot, or @c no_motion_slot for a leaf whose object motion is not tracked
         *         frame to frame (an instanced draw fills its slots at setup instead, because its
         *         per-instance transforms change far less often than the frame rate)
         * @note this is on the INTERFACE rather than on deren::vulkan::primitive because the runtime reads
         *       it while walking the tree, and the walker only knows the interface: RTTI is off, so
         *       there is no downcast to reach a concrete leaf.
         */
        [[nodiscard]] virtual uint32_t motion_slot() const noexcept {
            return no_motion_slot;
        }
    };

    /**
     * @ingroup vulkan_scene_tree
     * @brief one node of the scene tree: a local transform, child nodes and an
     *        optional primitive leaf. Value semantics: children are owned inline
     *        (copying a node copies its subtree).
     */
    export struct scene_node {
        // NOT `name`: find_node takes a parameter of that name, and a parameter would hide the
        // member - MSVC /W4 reports C4458, which /WX turns into an error (clang's -Wshadow is off).
        std::string node_name = {};        // debugging / future animation lookup
        glm::mat4 local = glm::mat4(1.0f); // local transform (T*R*S or full matrix)
        // identity of the source node this runtime node was rebuilt from: import_scene records
        // the structural iterator's get_source_index() here (the glTF loader stores the asset
        // node index), so callers can map e.g. animation channel targets onto the live tree
        std::size_t source_index = 0;
        std::vector<scene_node> children = {};
        std::unique_ptr<primitive> primitive_leaf = {}; // null for transform-only nodes

        scene_node() = default;
        // unique_ptr makes the node non-copyable; define an explicit clone for subtree copies
        scene_node(scene_node&&) noexcept = default;
        scene_node& operator=(scene_node&&) noexcept = default;
        scene_node(scene_node const&) = delete;
        scene_node& operator=(scene_node const&) = delete;

        /** @brief deep-copy this subtree (children and all) */
        [[nodiscard]] scene_node clone() const;

        /**
         * @ingroup vulkan_scene_tree
         * @brief append a new child node (empty: name "", identity local) and return it, so the
         *        caller fills it in place: add_child().node_name = ...; add_child().local = ...;
         * @return the appended child (reference valid until the next structural mutation of
         *         this node's children vector - pushing more children may reallocate)
         */
        scene_node& add_child();

        /**
         * @ingroup vulkan_scene_tree
         * @brief move @p child (and its whole subtree) into this node's children
         * @return the appended child (reference valid until the next structural mutation)
         */
        scene_node& add_child(scene_node child);

        /**
         * @ingroup vulkan_scene_tree
         * @brief attach an already-built primitive as this node's primitive leaf (replaces any
         *        existing leaf). Building happens through runtime::create_primitive(), which
         *        returns the primitive WITHOUT attaching it; this call places it under this
         *        node, so programmatic scenes can group primitives under transform nodes
         *        instead of only appending root leaves.
         * @param leaf the primitive to attach (ownership moves into this node)
         * @return the attached primitive (stable: it lives on the heap, unlike this node)
         * @note the node becomes a leaf node; any children it already had stay siblings below it
         * @note structural change: call runtime::scene_changed() afterwards so the culling BVH
         *       rebuilds (attach does not know about the runtime)
         */
        primitive* attach(std::unique_ptr<primitive> leaf);

        /**
         * @ingroup vulkan_scene_tree
         * @brief depth-first search this subtree for the first node with the given name
         *        (pre-order, matching scene_iterator's order); returns nullptr when absent.
         *        Empty names never match (they are the unnamed-node default).
         */
        [[nodiscard]] scene_node* find_node(std::string_view name) noexcept;
        [[nodiscard]] scene_node const* find_node(std::string_view name) const noexcept;
    };

    /**
     * @ingroup vulkan_scene_tree
     * @brief a named scene: a list of root nodes (mirrors deren::gltf::scene's shape)
     */
    export struct scene {
        // NOT `name`: find_node takes a parameter of that name, and a parameter would hide the
        // member - MSVC /W4 reports C4458, which /WX turns into an error (clang's -Wshadow is off).
        std::string scene_name = {};
        std::vector<scene_node> roots = {};

        /**
         * @ingroup vulkan_scene_tree
         * @brief append a new root node (empty: name "", identity local) and return it
         * @return the appended root (reference valid until the next structural mutation of
         *         roots - pushing more roots may reallocate)
         */
        scene_node& add_root();

        /**
         * @ingroup vulkan_scene_tree
         * @brief move @p root (and its whole subtree) in as a new root
         * @return the appended root (reference valid until the next structural mutation)
         */
        scene_node& add_root(scene_node root);

        /**
         * @ingroup vulkan_scene_tree
         * @brief depth-first search every root for the first node named @p name (pre-order);
         *        returns nullptr when absent. Empty names never match.
         */
        [[nodiscard]] scene_node* find_node(std::string_view name) noexcept;
        [[nodiscard]] scene_node const* find_node(std::string_view name) const noexcept;
    };

    /**
     * @ingroup vulkan_scene_tree
     * @brief depth-first walk that accumulates world transforms and pushes them
     *        into every primitive leaf: world(child) = world(parent) * child.local
     * @param node subtree root to walk (call once per scene root with mat4(1))
     * @param parent_world accumulated world of this node's parent
     */
    export void update_world(scene_node& node, glm::mat4 const& parent_world);

    /**
     * @ingroup vulkan_scene_tree
     * @brief walk the subtree and call @p visit on every primitive leaf
     * @tparam F invocable(scene_node const&, glm::mat4 const& world)
     */
    export template <class F>
    void visit_primitives(scene_node const& node, glm::mat4 const& parent_world, F&& visit) {
        glm::mat4 const world = parent_world * node.local;
        if (node.primitive_leaf) {
            visit(node, world);
        }
        for (scene_node const& child : node.children) {
            visit_primitives(child, world, visit);
        }
    }

    /**
     * @ingroup vulkan_scene_tree
     * @brief single-pass forward iterator over a scene's node tree: DFS pre-order across every
     *        root, INCLUDING transform-only (mesh-less) nodes, yielding each scene_node.
     *
     * Lets callers walk the live runtime tree with a range-for instead of hand-written
     * recursive lambdas:
     * @code {.cpp}
     * for (scene_node& node : scene) {          // scene.begin()/scene.end()
     *     // node.node_name / node.local / node.source_index / node.primitive_leaf
     * }
     * @endcode
     *
     * @note structural-frozen tree only: the iterator holds a stack of node addresses, so the
     *       tree must not be restructured (no make/import/clear, no push_back into a
     *       node's children) while an iterator is alive. Editing node.local or the leaf
     *       contents in place is fine - that is what the demo's per-frame animation writes do.
     * @note not a std iterator category (no reference typedefs): deliberately minimal - only
     *       ++ / != / * / -> , enough for range-for and manual loops
     */
    export class scene_iterator {
    public:
        scene_iterator() = default; // end()
        explicit scene_iterator(scene& owner);

        [[nodiscard]] scene_node& operator*() const noexcept;
        [[nodiscard]] scene_node* operator->() const noexcept;
        scene_iterator& operator++();

        /** @brief depth of the current node (0 = a scene root); lets callers tell roots apart */
        [[nodiscard]] std::size_t depth() const noexcept {
            return this->stack.empty() ? 0 : this->stack.back().second;
        }

        friend bool operator==(scene_iterator const& a, scene_iterator const& b) noexcept {
            if (a.exhausted || b.exhausted) {
                return a.exhausted == b.exhausted;
            }
            return a.stack == b.stack;
        }
        friend bool operator!=(scene_iterator const& a, scene_iterator const& b) noexcept {
            return !(a == b);
        }

    private:
        void descend(); // move to the next node in DFS pre-order, or set exhausted

        std::vector<std::pair<scene_node*, std::size_t>> stack = {}; // {node, depth}; back() = current
        bool exhausted = true;                                       // default = end(); begin() clears it
    };

    /** @brief begin()/end() of a scene's node tree (DFS pre-order, every root) */
    export inline scene_iterator begin(scene& owner) noexcept {
        return scene_iterator{owner};
    }
    /** @brief end() sentinel of a scene's node-tree range */
    export inline scene_iterator end(scene&) noexcept {
        return scene_iterator{};
    }
} // namespace deren::vulkan::scene_tree
