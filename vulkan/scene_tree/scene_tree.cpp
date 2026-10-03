module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

module deren.vulkan.scene_tree;

namespace deren::vulkan::scene_tree {
    void update_world(scene_node& node, glm::mat4 const& parent_world) {
        glm::mat4 const world = parent_world * node.local;
        if (node.primitive_leaf) {
            node.primitive_leaf->set_world(world);
        }
        for (scene_node& child : node.children) {
            update_world(child, world);
        }
    }

    // DFS pre-order over every root of the scene: the iterator holds an explicit stack of
    // {node, depth} frames; ++ pops the visited node and pushes its children (reversed so the
    // first child is visited next). Only valid while the tree structure is frozen (see
    // scene_iterator in scene_tree.cppm): no make/import/clear / child-vector pushes while
    // iterators live.
    scene_iterator::scene_iterator(scene& owner) {
        for (auto it = owner.roots.rbegin(); it != owner.roots.rend(); ++it) {
            this->stack.emplace_back(&*it, 0); // roots sit at depth 0
        }
        this->exhausted = this->stack.empty();
    }

    scene_node& scene_iterator::operator*() const noexcept {
        return *this->stack.back().first;
    }

    scene_node* scene_iterator::operator->() const noexcept {
        return this->stack.back().first;
    }

    scene_iterator& scene_iterator::operator++() {
        scene_node* const current = this->stack.back().first;
        std::size_t const child_depth = this->stack.back().second + 1;
        this->stack.pop_back();
        for (auto it = current->children.rbegin(); it != current->children.rend(); ++it) {
            this->stack.emplace_back(&*it, child_depth);
        }
        if (this->stack.empty()) {
            this->exhausted = true;
        }
        return *this;
    }

    scene_node scene_node::clone() const {
        scene_node copy;
        copy.node_name = this->node_name;
        copy.local = this->local;
        copy.children.reserve(this->children.size());
        for (scene_node const& child : this->children) {
            copy.children.push_back(child.clone());
        }
        // primitive leaves are intentionally NOT cloned: their ownership is
        // external (the runtime registers them once); copy preserves null
        return copy;
    }

    scene_node& scene_node::add_child() {
        this->children.emplace_back();
        return this->children.back();
    }

    scene_node& scene_node::add_child(scene_node child) {
        this->children.push_back(std::move(child));
        return this->children.back();
    }

    primitive* scene_node::attach(std::unique_ptr<primitive> leaf) {
        if (leaf == nullptr) {
            return nullptr;
        }
        primitive* const result = leaf.get();
        this->primitive_leaf = std::move(leaf);
        return result;
    }

    // iterative DFS (explicit stack, mirrors import_scene's traversal style): visit the node,
    // then its children in order; first name match wins (pre-order).
    scene_node* scene_node::find_node(std::string_view const name) noexcept {
        if (name.empty()) {
            return nullptr; // empty names are the unnamed default; never match
        }
        std::vector<scene_node*> stack;
        stack.push_back(this);
        while (!stack.empty()) {
            scene_node* const current = stack.back();
            stack.pop_back();
            if (current->node_name == name) {
                return current;
            }
            // push children reversed so the first child is visited next (DFS pre-order)
            for (auto it = current->children.rbegin(); it != current->children.rend(); ++it) {
                stack.push_back(&*it);
            }
        }
        return nullptr;
    }

    scene_node const* scene_node::find_node(std::string_view const name) const noexcept {
        if (name.empty()) {
            return nullptr;
        }
        std::vector<scene_node const*> stack;
        stack.push_back(this);
        while (!stack.empty()) {
            scene_node const* const current = stack.back();
            stack.pop_back();
            if (current->node_name == name) {
                return current;
            }
            for (auto it = current->children.rbegin(); it != current->children.rend(); ++it) {
                stack.push_back(&*it);
            }
        }
        return nullptr;
    }

    scene_node& scene::add_root() {
        this->roots.emplace_back();
        return this->roots.back();
    }

    scene_node& scene::add_root(scene_node root) {
        this->roots.push_back(std::move(root));
        return this->roots.back();
    }

    scene_node* scene::find_node(std::string_view const name) noexcept {
        for (scene_node& root : this->roots) {
            if (scene_node* const found = root.find_node(name)) {
                return found;
            }
        }
        return nullptr;
    }

    scene_node const* scene::find_node(std::string_view const name) const noexcept {
        for (scene_node const& root : this->roots) {
            if (scene_node const* const found = root.find_node(name)) {
                return found;
            }
        }
        return nullptr;
    }
} // namespace deren::vulkan::scene_tree
