module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

module deren.vulkan.animation;

import deren.vstd;
import deren.utility;

namespace deren::vulkan::animation {
    // clip_duration / display_name are module-private helpers declared in the interface unit
    // (controller.cppm); the non-template members below use them from here.

    // ---- sampling (format-neutral keyframe evaluation; glTF rules) ----

    channel_sample sample_channel(sampler const& sampler, channel_path const path, float const t) {
        channel_sample out = {};
        std::size_t const keys = sampler.times.size();
        if (keys == 0) {
            return out; // no keyframes: nothing to sample
        }
        // values per keyframe: the sampler records it (morph-weights channels vary per mesh);
        // fall back to the path rule when a sampler carries no per_key shape
        std::size_t const comps = sampler.per_key != 0 ? sampler.per_key : (path == channel_path::rotation ? 4 : 3);
        bool const cubic = sampler.interp == interpolation::cubic_spline;
        std::size_t const stored_per_key = comps * (cubic ? 3 : 1);
        if (sampler.values.size() < keys * stored_per_key) {
            return out; // value count does not match the key count: broken sampler
        }
        out.valid = true;

        // read one component straight out of the flat keyframe values (no temporary blocks):
        // 'key' selects the keyframe, 'comp' the component, 'offset' the triplet inside a
        // cubic block (0 = in tangent, comps = middle value, 2 * comps = out tangent)
        auto const read = [&](std::size_t const key, std::size_t const comp, std::size_t const offset) -> float {
            return sampler.values[key * stored_per_key + offset + comp];
        };
        // assign a keyframe's value block to the output (rotation -> quat, weights -> scalars)
        auto const set_value = [&](std::size_t const key, std::size_t const offset) {
            if (path == channel_path::rotation) {
                out.quat = glm::quat(read(key, 3, offset), read(key, 0, offset), read(key, 1, offset), read(key, 2, offset)); // glm ctor order (w, x, y, z)
            } else if (path == channel_path::weights) {
                out.scalars.resize(comps); // one value per morph target (the only result that needs a heap block)
                for (std::size_t c = 0; c < comps; ++c) {
                    out.scalars[c] = read(key, c, offset);
                }
            } else {
                out.vec3 = glm::vec3(read(key, 0, offset), read(key, 1, offset), read(key, 2, offset));
            }
        };

        // clamp t into the keyframe range, then find the left key: times[key] <= t < times[key + 1]
        float const time = std::clamp(t, sampler.times.front(), sampler.times.back());
        std::size_t key = 0;
        while (key + 1 < keys && sampler.times[key + 1] <= time) {
            ++key;
        }
        // STEP interpolation and the range end hold the left key's value (its middle triplet)
        auto const hold = [&] { set_value(key, cubic ? comps : 0); };
        if (sampler.interp == interpolation::step || key + 1 >= keys) {
            hold();
            return out;
        }

        float const dt = sampler.times[key + 1] - sampler.times[key];
        if (dt <= 0.0f) { // duplicate timestamps (invalid per the spec): hold the key's value
            hold();
            return out;
        }
        float const u = (time - sampler.times[key]) / dt;

        if (cubic) {
            // Hermite spline over the segment; tangents are scaled by the segment duration
            float const h00 = 2.0f * u * u * u - 3.0f * u * u + 1.0f;
            float const h10 = u * u * u - 2.0f * u * u + u;
            float const h01 = -2.0f * u * u * u + 3.0f * u * u;
            float const h11 = u * u * u - u * u;
            auto const eval = [&](std::size_t const c) {
                float const a = read(key, c, comps);         // this key's middle value
                float const out_t = read(key, c, 2 * comps); // this key's out tangent
                float const b = read(key + 1, c, comps);     // next key's middle value
                float const in_t = read(key + 1, c, 0);      // next key's in tangent
                return h00 * a + h10 * dt * out_t + h01 * b + h11 * dt * in_t;
            };
            if (path == channel_path::rotation) {
                // component-wise spline over the quaternion, then normalize (per the spec)
                glm::quat const q(eval(3), eval(0), eval(1), eval(2));
                float const norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                out.quat = norm > 0.0f ? glm::normalize(q) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            } else if (path == channel_path::weights) {
                out.scalars.resize(comps);
                for (std::size_t c = 0; c < comps; ++c) {
                    out.scalars[c] = eval(c);
                }
            } else {
                out.vec3 = glm::vec3(eval(0), eval(1), eval(2));
            }
            return out;
        }

        // LINEAR
        if (path == channel_path::rotation) {
            glm::quat const q0(read(key, 3, 0), read(key, 0, 0), read(key, 1, 0), read(key, 2, 0));
            glm::quat q1(read(key + 1, 3, 0), read(key + 1, 0, 0), read(key + 1, 1, 0), read(key + 1, 2, 0));
            if (glm::dot(q0, q1) < 0.0f) {
                q1 = glm::quat(-q1.w, -q1.x, -q1.y, -q1.z); // shortest arc: flip one endpoint
            }
            out.quat = glm::normalize(glm::slerp(q0, q1, u));
        } else if (path == channel_path::weights) {
            out.scalars.resize(comps);
            for (std::size_t c = 0; c < comps; ++c) {
                float const a = read(key, c, 0);
                out.scalars[c] = a + (read(key + 1, c, 0) - a) * u;
            }
        } else {
            glm::vec3 const a(read(key, 0, 0), read(key, 1, 0), read(key, 2, 0));
            glm::vec3 const b(read(key + 1, 0, 0), read(key + 1, 1, 0), read(key + 1, 2, 0));
            out.vec3 = a + (b - a) * u;
        }
        return out;
    }

    node_pose sample_node(clip const& clip, std::size_t const target_node, node_pose const& base, float const t) {
        node_pose pose = base;
        for (channel const& channel : clip.channels) {
            if (channel.target_node != target_node || channel.sampler >= clip.samplers.size()) {
                continue;
            }
            channel_sample const sample = sample_channel(clip.samplers[channel.sampler], channel.path, t);
            if (!sample.valid) {
                continue;
            }
            switch (channel.path) {
            case channel_path::translation:
                pose.translation = sample.vec3;
                pose.any_transform = true;
                break;
            case channel_path::rotation:
                pose.rotation = sample.quat;
                pose.any_transform = true;
                break;
            case channel_path::scale:
                pose.scale = sample.vec3;
                pose.any_transform = true;
                break;
            case channel_path::weights:
                pose.weights = sample.scalars; // active morph weights for the node's mesh
                break;
            }
            pose.any_channel = true;
        }
        return pose;
    }

    // ---- init is a template member defined in the interface unit (controller.cppm)
    //      so any importer can instantiate it with its concrete animation source ----

    // ---- playback table / gui binding ----

    std::size_t controller::skin_rig_count() const noexcept {
        return this->skin_rigs.size();
    }

    std::optional<glm::mat4> controller::joint_world(std::size_t const rig_index, std::size_t const joint_index) const noexcept {
        if (rig_index >= this->skin_rigs.size()) {
            return std::nullopt;
        }
        skin_rig const& rig = this->skin_rigs[rig_index];
        if (joint_index >= rig.s.joints.size()) {
            return std::nullopt;
        }
        // THE JOINT IS AN ASSET NODE INDEX HERE, and `skin_world_index` is keyed by exactly that: the cache is
        // built over "every wanted node", joints and mesh nodes together (see skin_sources), so a joint this
        // controller collected is a lookup and a joint it did not is a miss rather than a wrong matrix.
        auto const found = this->skin_world_index.find(rig.s.joints[joint_index]);
        if (found == this->skin_world_index.end() || found->second >= this->skin_world_cache.size()) {
            return std::nullopt;
        }
        return this->skin_world_cache[found->second];
    }

    std::size_t controller::playable_count() const noexcept {
        return this->playable_clips.size();
    }

    std::string_view controller::playable_name(std::size_t const index) const noexcept {
        if (index >= this->playable_clips.size()) {
            return {};
        }
        return display_name(this->playable_clips[index].name);
    }

    float controller::playable_max_duration() const noexcept {
        return this->max_duration;
    }

    bool controller::has_active() const noexcept {
        return this->active != nullptr;
    }

    std::size_t controller::current() const noexcept {
        return this->current_index;
    }

    void controller::select(std::size_t const index) {
        if (index >= this->playable_clips.size()) {
            return;
        }
        // reset every animated node to its base pose first, so nodes the PREVIOUS clip moved
        // but the new one does not sample return to rest (same compose rule as update())
        for (auto const& [source, targets] : this->source_nodes) {
            auto const base_it = this->base_poses.find(source);
            node_pose const base = base_it == this->base_poses.end() ? node_pose{} : base_it->second;
            glm::mat4 const trs = glm::translate(glm::mat4(1.0f), base.translation) * glm::mat4_cast(base.rotation) * glm::scale(glm::mat4(1.0f), base.scale);
            for (node_target const& target : targets) {
                target.node->local = target.scene_root ? glm::translate(glm::mat4(1.0f), this->import_shift) * trs : trs;
            }
        }
        this->host.scene_changed();
        this->active = &this->playable_clips[index];
        this->current_index = index;
        this->time = 0.0f;
        this->duration = clip_duration(*this->active);
        this->debug_source = this->pick_debug_source(*this->active);
        this->refresh_debug_name();
    }

    void controller::set_playing(bool const playing) noexcept {
        this->playing_flag = playing;
    }

    bool controller::is_playing() const noexcept {
        return this->playing_flag;
    }

    void controller::set_time(float const t) {
        // scrub: clamp and pause so the clock does not fight the drag (gui slider semantics)
        this->time = std::clamp(t, 0.0f, this->duration);
        this->playing_flag = false;
    }

    float controller::current_time() const noexcept {
        return this->time;
    }

    float controller::loop_duration() const noexcept {
        return this->duration;
    }

    // ---- per-frame drive (after pace_and_acquire(), before begin_recording()) ----

    // sample one loader source into its runtime nodes + the active slot's morph weights;
    // returns whether any node local moved (morph-only writes are not "changed": they do not
    // invalidate the culling BVH)
    bool controller::sample_source(std::size_t const source, std::vector<node_target> const& targets) {
        auto const base_it = this->base_poses.find(source);
        node_pose const base = base_it == this->base_poses.end() ? node_pose{} : base_it->second;
        node_pose const pose = sample_node(*this->active, source, base, this->time);
        // Morph weights of every rig of this source: the active clip's sampled weights when it
        // animates this source (pose.weights matches the rig's target count), otherwise the
        // baked DEFAULT weights. Writing the defaults whenever the clip does not drive the
        // source is what clears a previous clip's weights after a clip switch - without it the
        // mesh would keep the last animated pose (morph residue) on any clip that has no
        // weights channel for it.
        float* const active_scratch = this->host.morph_scratch_active();
        if (active_scratch != nullptr && !this->morph_rigs.empty()) {
            for (morph_rig const& rig : this->morph_rigs) {
                if (rig.source != source) {
                    continue;
                }
                // The weights are the FIRST region of a sparse morph block (see the layout note on the bake
                // in controller.cppm): the block offset IS the weight offset. The dense form had to step over
                // vertex_count * target_count * 6 floats of deltas to reach them, which is what made the
                // weight write depend on the geometry's size.
                std::size_t const weight_offset = static_cast<std::size_t>(rig.morph_base);
                std::span<float const> const weights = pose.weights.size() == rig.target_count
                                                           ? std::span<float const>(pose.weights)
                                                           : std::span<float const>(rig.default_weights);
                // Publish the weights this primitive had ONE FRAME AGO into the block's second weight
                // region BEFORE overwriting the current one: that copy is the whole of the morph half of a
                // morphing vertex's motion vector (see the layout note in shaders/pbr.slang, and
                // docs/deformation_motion_vectors.md for why the previous weights are stored here rather
                // than in a buffer of their own). Both regions are `rig.target_count` floats, so this is
                // the ONLY place that has to keep them in step.
                float* const weight_dst = active_scratch + weight_offset;
                std::memcpy(weight_dst + rig.target_count, weight_dst, static_cast<std::size_t>(rig.target_count) * sizeof(float));
                std::memcpy(weight_dst, weights.data(), weights.size_bytes());
            }
        }
        if (!pose.any_transform) {
            return false; // weights-only channels don't move the node's local transform
        }
        glm::mat4 const trs = glm::translate(glm::mat4(1.0f), pose.translation) * glm::mat4_cast(pose.rotation) * glm::scale(glm::mat4(1.0f), pose.scale);
        for (node_target const& target : targets) {
            target.node->local = target.scene_root ? glm::translate(glm::mat4(1.0f), this->import_shift) * trs : trs;
        }
        if (source == this->debug_source) {
            this->debug_translation = pose.translation;
        }
        return true;
    }

    void controller::update(float const dt_seconds) {
        if (this->host.scene == nullptr) {
            return;
        }

        // 1. playback: advance the clock (when playing) and sample the active clip into the
        //    scene node locals + the active slot's morph weights. Each source samples
        //    independently (channels are keyed by target node; only that source's own runtime
        //    nodes are written), so heavy animations fan the per-source sampling out over the
        //    runtime's shared task pool while light ones stay on this thread.
        if (this->active != nullptr) {
            if (this->playing_flag) {
                this->time += dt_seconds;
                if (this->time >= this->duration) {
                    this->time = std::fmod(this->time, this->duration);
                }
            }

            bool changed = false;
            if (this->parallel_sampling && !this->sample_keys.empty()) {
                // slice the stable source list over the runtime's shared task pool; each worker
                // owns a contiguous range and reports whether it moved any node. Different
                // sources touch different runtime nodes / morph offsets, so no shared state is
                // written concurrently (debug_translation has one writer: its own source's slice).
                // The tasks capture only `this` (+ the slice bounds): self-contained, so the
                // list can be handed to the host's run_tasks and executed on the host pool.
                // sampling_tasks is MEMBER scratch, reused every frame (no per-frame allocation
                // on this hot path; only touched from this frame thread).
                std::size_t const total = this->sample_keys.size();
                uint32_t const workers = std::max(1, this->host.task_worker_count());
                // a single monotonic flag replaces per-slice flags: the main thread resets it
                // before spawning, workers only ever set it, and run_tasks is synchronous, so it
                // is only read back after every task finished
                this->sampling_changed.store(false);
                std::vector<std::function<void()>>& tasks = this->sampling_tasks;
                tasks.clear();
                tasks.reserve(workers);
                for (uint32_t w = 0; w < workers; ++w) {
                    std::size_t const begin = total * w / workers;
                    std::size_t const end = total * (w + 1) / workers;
                    if (begin >= end) {
                        continue;
                    }
                    tasks.emplace_back([this, begin, end] {
                        for (std::size_t i = begin; i < end; ++i) {
                            std::size_t const source = this->sample_keys[i];
                            auto const targets_it = this->source_nodes.find(source);
                            if (targets_it != this->source_nodes.end() && this->sample_source(source, targets_it->second)) {
                                this->sampling_changed.store(true);
                            }
                        }
                    });
                }
                // forward the task list to the host's pool and wait for this stage's
                // group: run_tasks is synchronous, so sampling finishes before update() returns
                this->host.run_tasks(tasks);
                changed = this->sampling_changed.load();
            } else {
                for (auto const& [source, targets] : this->source_nodes) {
                    changed = this->sample_source(source, targets) || changed;
                }
            }
            if (changed) {
                this->host.scene_changed(); // node.locals edited -> culling BVH must track them
            }
        }

        // 2. skin matrices: the joint worlds follow the locals above, so rebuild every frame
        //    [identity block | per-rig joint blocks] into the active slot's skin buffer
        if (!this->skin_rigs.empty()) {
            // collect the world matrix of every wanted node (mesh nodes + joints) into the
            // REUSED dense cache: skin_world_index maps a source to its cache slot (fixed after
            // init), so this writes plain vector slots instead of building a fresh
            // unordered_map every frame. Wanted nodes that the DFS never reaches (not in the
            // tree) keep the identity they were filled with.
            std::ranges::fill(this->skin_world_cache, glm::mat4(1.0f));
            auto const collect_worlds = [this](auto&& self, deren::vulkan::scene_tree::scene_node& node, glm::mat4 const& parent_world) -> void {
                glm::mat4 const world = parent_world * node.local;
                if (auto const it = this->skin_world_index.find(node.source_index); it != this->skin_world_index.end()) {
                    this->skin_world_cache[it->second] = world;
                }
                for (deren::vulkan::scene_tree::scene_node& child : node.children) {
                    self(self, child, world);
                }
            };
            for (deren::vulkan::scene_tree::scene_node& root : this->host.scene->roots) {
                collect_worlds(collect_worlds, root, glm::mat4(1.0f));
            }
            auto const world_of = [this](std::size_t const source) -> glm::mat4 {
                auto const it = this->skin_world_index.find(source);
                return it == this->skin_world_index.end() ? glm::mat4(1.0f) : this->skin_world_cache[it->second];
            };
            std::vector<glm::mat4>& matrices = this->skin_matrices_scratch;
            matrices.clear();
            matrices.reserve(4 + (this->skin_rigs.size() * 8));
            matrices.insert(matrices.end(), {glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f)});
            for (skin_rig const& rig : this->skin_rigs) {
                // ONE SKINNING SPACE FOR THE WHOLE RIG, taken from its first mesh source. glTF says a
                // skinned mesh's own node transform is IGNORED - the joints' world transforms are the
                // skinning space - so a single inverse is right for every node sharing the skin, and the
                // init logs the case where more than one node shares it. If an asset ever turns up whose
                // shared nodes carry DIFFERENT transforms, this line is the one to revisit.
                glm::mat4 const mesh_world_inv = glm::inverse(world_of(rig.mesh_sources.front()));
                for (std::size_t j = 0; j < rig.s.joints.size(); ++j) {
                    matrices.push_back(mesh_world_inv * world_of(rig.s.joints[j]) * rig.s.inverse_bind[j]);
                }
            }
            this->host.set_skin_matrices_active(matrices);
            // per-second report data: first rig's LAST joint world x-axis (rotations change it,
            // unlike the joint's position which stays fixed under rotation-only animations)
            this->skin_debug_valid = false;
            if (!this->skin_rigs.front().s.joints.empty()) {
                std::size_t const last_joint = this->skin_rigs.front().s.joints.back();
                if (this->skin_world_index.contains(last_joint)) {
                    this->skin_debug_valid = true;
                    this->skin_debug_translation = glm::vec3(world_of(last_joint)[0]); // world x axis
                }
            }
        }
    }

    // ---- read-only bridge ----

    bool controller::has_runtime_node(std::size_t const source) const noexcept {
        return this->source_nodes.contains(source);
    }

    // ---- diagnostics ----

    std::string_view controller::active_name() const noexcept {
        return this->active == nullptr ? std::string_view{} : display_name(this->active->name);
    }

    std::string_view controller::get_debug_node_name() const noexcept {
        return this->debug_node_name;
    }

    glm::vec3 controller::get_debug_translation() const noexcept {
        return this->debug_translation;
    }

    bool controller::is_skin_debug_valid() const noexcept {
        return this->skin_debug_valid;
    }

    glm::vec3 controller::get_skin_debug_translation() const noexcept {
        return this->skin_debug_translation;
    }

    std::string_view controller::get_skin_debug_name() const noexcept {
        return this->skin_debug_name;
    }

    // ---- internal helpers ----

    // the node reported per second: prefer a translation channel target (its value is visible
    // in the log), fall back to the first channel target present in the tree
    std::size_t controller::pick_debug_source(clip const& clip) const {
        std::size_t fallback = std::numeric_limits<std::size_t>::max();
        for (channel const& channel : clip.channels) {
            if (!this->source_nodes.contains(channel.target_node)) {
                continue;
            }
            if (channel.path == channel_path::translation) {
                return channel.target_node;
            }
            if (fallback == std::numeric_limits<std::size_t>::max()) {
                fallback = channel.target_node;
            }
        }
        return fallback;
    }

    void controller::refresh_debug_name() {
        this->debug_node_name.clear();
        if (this->debug_source == std::numeric_limits<std::size_t>::max()) {
            return;
        }
        auto const it = this->source_nodes.find(this->debug_source);
        if (it != this->source_nodes.end() && !it->second.empty()) {
            this->debug_node_name = it->second.front().node->node_name;
        }
    }
} // namespace deren::vulkan::animation
