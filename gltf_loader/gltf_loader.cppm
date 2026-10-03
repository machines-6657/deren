// ============================================================================
// module: gltf_loader
// module version: 0.3.0  (independent of the app version in CMakeLists project(VERSION))
//
// Pure-CPU glTF / GLB loader (vendored fastgltf + stb): drawable stream, retained
// node tree, animations / skins / morph targets / cameras / punctual lights.
// No Vulkan dependency.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <array> // the CLAIMED EXTRAS ROWS table (see `deren::gltf::claimed_extras_floats`)
#include <cstdint>
// `std::optional`: `scenes::texture_index_by_name` returns one, because a name that matches nothing is a
// state the caller handles rather than an error (see the method's note).
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <map> // the material's CLAIMED extras rows, by the asset pipeline's own spelling
#include <optional>
#include <string_view> // the name it looks up

export module deren.gltf_loader;
export import deren.vstd;
/**
 * @file gltf_loader.cppm
 * @defgroup gltf_loader glTF Loader
 * @brief load glTF/GLB files into pure CPU-side data structures, no Vulkan dependency.
 *        Besides the raw scene data it provides renderer-ready drawable iteration
 *        (resolved_material / drawable_iterator) in pure CPU types; the runtime's
 *        scene_drawable_iterator concept is structural over them, so no Vulkan type is
 *        needed here.
 * @note
 *      - built on fastgltf
 *      - load_model() returns std::expected, failures are reported via error_code
 *      - drawable_iterator models deren::vulkan::scene_drawable_iterator and can be fed directly
 *        to deren::vulkan::runtime::import_scene()
 */
namespace deren::gltf {

    /**
     * @ingroup gltf_loader
     * @brief component type of an accessor element, values are the glTF OpenGL constants
     */
    export enum class component_type : int32_t {
        byte_t = 5120,
        unsigned_byte_t = 5121,
        short_t = 5122,
        unsigned_short_t = 5123,
        int_t = 5124,
        unsigned_int_t = 5125,
        float_t = 5126,
        double_t = 5130,
        unknown = 0,
    };

    /**
     * @ingroup gltf_loader
     * @brief convert a glTF component type constant to component_type
     * @param gltf_constant the OpenGL constant (5120..5130)
     * @return the mapped component_type, unknown for unrecognized values
     */
    export constexpr component_type to_component_type(int32_t const gltf_constant) {
        switch (gltf_constant) {
        case 5120:
            return component_type::byte_t;
        case 5121:
            return component_type::unsigned_byte_t;
        case 5122:
            return component_type::short_t;
        case 5123:
            return component_type::unsigned_short_t;
        case 5124:
            return component_type::int_t;
        case 5125:
            return component_type::unsigned_int_t;
        case 5126:
            return component_type::float_t;
        case 5130:
            return component_type::double_t;
        default:
            return component_type::unknown;
        }
    }

    /**
     * @ingroup gltf_loader
     * @brief byte size of a single component of the given component type
     * @param type the component type
     * @return byte size, 0 for unknown
     */
    export constexpr uint8_t get_component_size(component_type const type) {
        switch (type) {
        case component_type::byte_t:
            [[fallthrough]];
        case component_type::unsigned_byte_t:
            return 1;
        case component_type::short_t:
            [[fallthrough]];
        case component_type::unsigned_short_t:
            return 2;
        case component_type::int_t:
            [[fallthrough]];
        case component_type::unsigned_int_t:
            [[fallthrough]];
        case component_type::float_t:
            return 4;
        case component_type::double_t:
            return 8;
        case component_type::unknown:
            return 0;
        }
        std::unreachable();
    }

    /**
     * @ingroup gltf_loader
     * @brief convert component_type back to the glTF OpenGL constant
     * @param type the component type
     * @return the glTF OpenGL constant, -1 for unknown
     */
    export constexpr int32_t to_gltf_macro_type(component_type const type) {
        if (type == component_type::unknown) {
            return -1;
        }
        return static_cast<int32_t>(type);
    }

    /**
     * @ingroup gltf_loader
     * @brief element type of an accessor (scalar/vector/matrix)
     */
    export enum class element_type {
        scale,
        vec2,
        vec3,
        vec4,
        mat2,
        mat3,
        mat4,
        unknown,
    };

    /**
     * @ingroup gltf_loader
     * @brief convert a glTF type constant to element_type
     * @param type the glTF type constant (0..6)
     * @return the mapped element_type
     */
    export constexpr element_type to_element_type(int32_t const type) {
        switch (type) {
        case 0:
            return element_type::scale;
        case 1:
            return element_type::vec2;
        case 2:
            return element_type::vec3;
        case 3:
            return element_type::vec4;
        case 4:
            return element_type::mat2;
        case 5:
            return element_type::mat3;
        case 6:
            return element_type::mat4;
        default:
            break;
        }
        std::unreachable();
    }

    /**
     * @ingroup gltf_loader
     * @brief element count of the given element type
     * @param type the element type
     * @return element count, 0 for unknown
     */
    export constexpr uint8_t get_element_size(element_type const type) {
        switch (type) {
        case element_type::scale:
            return 1;
        case element_type::vec2:
            return 2;
        case element_type::vec3:
            return 3;
        case element_type::vec4:
            [[fallthrough]];
        case element_type::mat2:
            return 4;
        case element_type::mat3:
            return 9;
        case element_type::mat4:
            return 16;
        case element_type::unknown:
            return 0;
        }
        std::unreachable();
    }

    /**
     * @ingroup gltf_loader
     * @brief error codes returned by load_model
     */
    export enum class error_code {
        file_not_found,
        file_type_error,
        file_load_failed,
    };

    /**
     * @ingroup gltf_loader
     * @brief decoded image data of a texture
     */
    export struct texture_data {
        std::vector<uint8_t> data;
        uint32_t width = 0;
        uint32_t height = 0;
        uint8_t component = 0; // aka. channels
        /**
         * THE glTF IMAGE'S NAME, which the loader used to discard.
         *
         * It is carried for ONE join, and it is the join a toon character needs: a glTF material's five slots
         * reach the albedo, the metallic-roughness, the normal, the occlusion and the emissive map, and a toon
         * character ALSO reads a diffuse ramp, a shadow LUT, a specular ramp, a matcap and a face SDF - none of
         * which glTF has a slot for. Those are named by the ASSET PIPELINE and referenced by name in the toon
         * material sidecar (see toon_material_sidecar.cppm), so the image NAME is the only thing that connects
         * the two files. Empty for an unnamed image, which is what most glTF files have (the repository's own
         * DamagedHelmet sample names none of its five).
         */
        std::string name = {};
    };

    /**
     * @ingroup gltf_loader
     * @brief raw vertex data portion together with its component type
     */
    export struct vertex_portion {
        std::vector<uint8_t> data;
        component_type component;
    };

    /**
     * @ingroup gltf_loader
     * @brief metallic-roughness material factors, defaults follow the glTF spec
     */
    export struct material_factors {
        glm::vec4 base_color_factor = glm::vec4(1.0f);
        glm::vec3 emissive_factor = glm::vec3(0.0f);
        float metallic_factor = 1.0f;
        float roughness_factor = 1.0f;
        float normal_scale = 1.0f;
        float occlusion_strength = 1.0f; // occlusion map influence: mix(1, sampled AO, strength)
        float alpha_cutoff = 0.5f;       // alphaMode MASK threshold (default per glTF spec)
        bool alpha_mask = false;         // alphaMode == MASK: discard fragments below alpha_cutoff
        bool alpha_blend = false;        // alphaMode == BLEND: alpha-blended (transparent) material
    };

    /**
     * @ingroup gltf_loader
     * @brief a glTF material: factors plus the texture slots it uses
     * @note texture_indices maps slot names ("albedo" / "metallic_roughness" / "normal" /
     *       "occlusion" / "emissive") to indices into scenes::textures
     */
    export struct material {
        material_factors factors = {};
        std::map<std::string, uint16_t> texture_indices = {};
        bool double_sided = false; // glTF doubleSided: back faces are rendered, normals flipped
        /**
         * THE glTF MATERIAL NAME, which the loader used to discard.
         *
         * It is carried for ONE consumer: `toon_family_of()`, which classifies a character material into the
         * family whose toon parameters it uses (see that function). A character model's materials are named
         * after what they ARE - `M_actor_zhuangfy_body_01`, `_face_01`, `_hair_01`, `_iris_01`, `_cloth_01` -
         * and that name is the only per-material fact available at import that says which family a material
         * belongs to. Empty for an unnamed material, which classifies as `toon_family::none` and keeps the
         * famiy-independent path every non-character model already had.
         */
        std::string name = {};
        /**
         * THE `extras` SCALARS THIS PORT HAS CLAIMED, by the asset pipeline's own spelling (`_Specular`,
         * `_ParallaxScale`), and only those - see `claimed_extras_floats`.
         *
         * WHAT `extras` IS AND WHY IT NEEDED A THIRD SOURCE: a glTF material carries the spec's six factors and
         * five texture slots, and this repository's toon characters carry a SECOND set of parameters in
         * `material.extras` - the game's own exported material table, as `efFloats` / `efColors` / `efTexSlots` /
         * `efToonSlots` / `efResolved` (194 float/colour names that the `.toon.tsv` sidecar does not carry; the
         * sidecar is this port's transcription of a subset of the same tables). glTF has no slot for them, the
         * sidecar the renderer reads has no row for this one, and a value the game states per material that the
         * shader could not see was simply not available - which is the gap this field closes.
         *
         * WHY IT IS A CLAIMED SUBSET RATHER THAN THE WHOLE `efFloats` BLOCK: the port's own rule is that a lane
         * nobody consumes is a SECOND source of truth for a value that has none, so a property is imported when
         * its consumer is wired and not before. The names are held in ONE table (`claimed_extras_floats`) whose
         * entries are added together with their reader, and a name absent from a given material's extras is
         * simply absent from this map - the consumer's fallback then answers, exactly as it did before.
         *
         * Empty for every material whose file has no `extras`, which is every non-character model: the loader
         * reads this block through fastgltf's extras callback and never invents a value for it.
         */
        std::map<std::string, float> extras_floats = {};
    };

    /**
     * @ingroup gltf_loader
     * @brief the `extras` scalar names this port has CLAIMED - the whitelist `material::extras_floats` is
     *        filled from
     *
     * AN ALLOW-LIST RATHER THAN A FILTER, and the difference is the point: `efFloats` on chen's cloth material
     * holds 190 names, of which the port reads one. Importing the block whole would put 190 unread values into
     * the model - a second source of truth for every one of them, in the exact shape this repository refuses
     * everywhere else - so the loader copies out ONLY the names listed here, and a name reaches this table in
     * the same change as the code that reads it.
     *
     * THE CURRENT ENTRIES, each with its reader:
     *   `_Specular`       ->  the per-material specular strength, a `toon_colour_lane::specular_strength` lane read
     *                         by `shaders/character_forward.slang` (`main_specular = ... * spec_strength *
     *                         rig.env.z`). It was the one extras-only property with a live consumer whose source
     *                         was a per-FAMILY constant (`shaders/toon_params.slang`'s `spec_strength`) and whose
     *                         value the asset states per MATERIAL; chen's `M_actor_chen_brow_01` is the material
     *                         that proves it (0.0 against the face family's 1.0).
     *   `_ParallaxScale`  ->  the per-material parallax depth, a `toon_colour_lane::parallax_scale` lane read by
     *                         the same stage (`parallax_offset = offset_dir * depth * float2(1.0, 0.25)`), whose
     *                         source was the stage's own `character_eye_parallax_depth` constant - chen states
     *                         0.03 on the iris and 0.5 on the brow, and values 0.03/0.5/0.5 over the three
     *                         materials that state the row at all.
     */
    export inline constexpr std::array<std::string_view, 2> claimed_extras_floats = {"_Specular", "_ParallaxScale"};

    /**
     * @ingroup gltf_loader
     * @brief one morph target of a primitive: per-vertex displacement attributes (POSITION /
     *        NORMAL deltas; the loader keeps other target attributes raw, consumers only blend
     *        position/normal). Same vertex count as the base attributes
     */
    export struct morph_target {
        std::map<std::string, vertex_portion> attributes = {};
    };

    /**
     * @ingroup gltf_loader
     * @brief a drawable primitive: vertex attributes, index data and its material reference
     * @note targets holds the primitive's morph targets (empty when the mesh is not morphable);
     *       a consumer blends base + sum(weight_i * delta_i) per vertex — see docs §10
     */
    export struct primitive {
        std::map<std::string, vertex_portion> vertex;
        std::vector<morph_target> targets = {};
        std::vector<uint8_t> index;
        component_type index_component_type;
        // index into scenes::materials; std::numeric_limits<uint32_t>::max() when the primitive
        // has no material (render with default factors and no textures)
        uint32_t material_index = std::numeric_limits<uint32_t>::max();
    };

    /**
     * @ingroup gltf_loader
     * @brief a mesh composed of primitives
     * @note weights are the mesh's default morph weights, one per target of its primitives
     *       (glTF mesh.weights; empty = all-zero). deren::gltf::node::weights, when present, overrides
     *       them; a "weights" animation channel drives them over time (see docs §10)
     */
    export struct mesh {
        std::vector<primitive> primitives;
        std::vector<float> weights = {};
    };

    // ---- animation (glTF keyframe animation, exported decoded; playback is a consumer concern) ----

    /**
     * @ingroup gltf_loader
     * @brief interpolation mode of one animation sampler (glTF "interpolation")
     */
    export enum class animation_interpolation : int32_t {
        linear = 0,       // glTF LINEAR: blend between consecutive keyframes (slerp for rotations)
        step = 1,         // glTF STEP: hold the previous keyframe's value until the next keyframe
        cubic_spline = 2, // glTF CUBICSPLINE: Hermite spline with per-key in/out tangents
    };

    /**
     * @ingroup gltf_loader
     * @brief animated node property of one animation channel (glTF "path")
     */
    export enum class animation_path : int32_t {
        translation = 1, // values are xyz triplets (one per keyframe)
        rotation = 2,    // values are xyzw quaternions (w scalar, one per keyframe)
        scale = 3,       // values are xyz triplets (one per keyframe)
        weights = 4,     // morph target weights: per-key scalar block, one value per target of
                         // the node's mesh (sampler::per_key = target count)
    };

    /**
     * @ingroup gltf_loader
     * @brief one decoded animation sampler: keyframe times + flat output values
     * @note
     *      - times: one float per keyframe, in seconds, monotonically non-decreasing (as stored)
     *      - values: flat floats. LINEAR / STEP hold key_count * per_key floats; CUBICSPLINE
     *        holds key_count * per_key * 3 floats, grouped per keyframe in glTF order:
     *        in-tangent, value, out-tangent. Whether a sampler was decoded from valid accessors
     *        is not tracked — a broken/unsupported sampler simply has empty times/values.
     *      - per_key: values per keyframe — 3 for translation/scale, 4 for rotation, the morph
     *        target count for the "weights" path (0 = not derivable; the sampler is unusable)
     *      - glTF requires float input; other numeric component types are converted to float
     *        when present (so the loader stays robust against non-conforming files)
     */
    export struct animation_sampler {
        std::vector<float> times = {};
        std::vector<float> values = {};
        std::size_t per_key = 0; // values per keyframe (see above)
        animation_interpolation interpolation = animation_interpolation::linear;
    };

    /**
     * @ingroup gltf_loader
     * @brief one animation channel: animate one TRS property of a node from a sampler
     * @note
     *      - sampler indexes the owning animation's samplers (glTF semantics)
     *      - target_node is the animated node's index in the glTF asset's node table. Locate
     *        the matching node of a scene's pool by deren::gltf::node::source_index (animations are
     *        file-scoped; a node may be reachable from several scenes)
     */
    export struct animation_channel {
        animation_path path = animation_path::translation;
        std::size_t sampler = 0;
        std::size_t target_node = 0;
    };

    /**
     * @ingroup gltf_loader
     * @brief one glTF animation: channels over samplers, file-scoped (not tied to one scene)
     */
    export struct animation {
        std::string name = {};
        std::vector<animation_sampler> samplers = {};
        std::vector<animation_channel> channels = {};
    };

    /**
     * @ingroup gltf_loader
     * @brief a glTF skin: the joints driving a skinned mesh and their inverse bind matrices
     * @note
     *      - joints are asset node indices (resolve against a scene's pool through
     *        deren::gltf::node::source_index, like animation_channel::target_node)
     *      - inverse_bind_matrices holds one mat4 per joint (joint order), decoded from the
     *        asset's IBM accessor; when the asset omits it (or it is broken) identity matrices
     *        are filled in — the glTF default
     *      - a skinned node's mesh vertices carry JOINTS_0 (joint indices into @p joints) and
     *        WEIGHTS_0 attributes; the drawable vertex layout keeps both (identity for
     *        non-skinned meshes), so a consumer only needs the per-frame joint matrices
     */
    export struct skin {
        std::string name = {};
        std::vector<std::size_t> joints = {};              // asset node indices, in joint order
        std::vector<glm::mat4> inverse_bind_matrices = {}; // one per joint (identity when omitted)
    };

    /**
     * @ingroup gltf_loader
     * @brief kind of a glTF camera (deren::gltf::node::camera_index -> scenes::cameras)
     */
    export enum class camera_type : int32_t {
        perspective = 0,  // yfov / znear / aspect_ratio? / zfar? (absent zfar = infinite)
        orthographic = 1, // xmag / ymag / ortho_znear / ortho_zfar
    };

    /**
     * @ingroup gltf_loader
     * @brief a glTF camera attached to a node
     * @note values are the raw glTF parameters (radians / distances); the camera sits on a
     *       node, so a consumer builds the view/projection from the node's world transform
     *       (resolved through deren::gltf::node::source_index, like joints — cameras can be animated)
     */
    export struct camera {
        std::string name = {};
        camera_type type = camera_type::perspective;
        // perspective
        float yfov = glm::radians(45.0f); // vertical field of view (radians)
        float znear = 0.01f;
        std::optional<float> aspect_ratio = std::nullopt; // absent = match the render target
        std::optional<float> zfar = std::nullopt;         // absent = infinite far plane
        // orthographic
        float xmag = 1.0f;
        float ymag = 1.0f;
        float ortho_znear = 0.0f;
        float ortho_zfar = 1.0f;
    };

    /**
     * @ingroup gltf_loader
     * @brief kind of a punctual light (KHR_lights_punctual, deren::gltf::node::light_index)
     */
    export enum class light_type : int32_t {
        directional = 0, // infinitely far, direction = -node +Z axis (world)
        point = 1,       // position = node origin (world)
        spot = 2,        // position + direction from the node, cone angles below
    };

    /**
     * @ingroup gltf_loader
     * @brief a punctual light (KHR_lights_punctual) referenced by a node's light_index
     * @note
     *      - color is linear RGB; intensity is lux (directional) or candela (point/spot);
     *        range and the spot cone angles are optional (absent = infinite range / default cone)
     *      - position/direction come from the owning node's world transform (resolved through
     *        deren::gltf::node::source_index; lights can be animated) — only the light *properties*
     *        are stored here
     */
    export struct light {
        std::string name = {};
        light_type type = light_type::point;
        glm::vec3 color = glm::vec3(1.0f);
        float intensity = 1.0f;
        std::optional<float> range = std::nullopt;           // point/spot attenuation cutoff
        std::optional<float> spot_inner_cone = std::nullopt; // spot only (radians)
        std::optional<float> spot_outer_cone = std::nullopt; // spot only (radians)
    };

    /**
     * @ingroup gltf_loader
     * @brief evaluated value of one animation channel at a point in time: a vec3 for the
     *        translation/scale paths, a quaternion for the rotation path, or a scalar block for
     *        the "weights" (morph) path
     * @note valid == false means the sampler had no keyframes (or broken values); nothing
     *       could be sampled
     */
    export struct channel_sample {
        bool valid = false;
        glm::vec3 vec3 = glm::vec3(0.0f);                   // translation / scale paths
        glm::quat quat = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // rotation path (normalized, w scalar)
        std::vector<float> scalars = {};                    // weights path: one value per morph target
    };

    /**
     * @ingroup gltf_loader
     * @brief evaluate the channel's sampler at @p t seconds (glTF keyframe sampling)
     * @param sampler the sampler to evaluate (see animation_sampler for the values layout)
     * @param path the property the sampler's values encode — determines the component count
     *        per key (3 for translation/scale, 4 for rotation) and which member of the
     *        returned channel_sample is filled
     * @param t playback time in seconds; clamped to the sampler's keyframe range
     * @return channel_sample with valid == false when the sampler has no usable keyframes
     * @note interpolation follows the sampler's mode: LINEAR lerps translations/scales and
     *       slerps rotations along the shortest arc; STEP holds the previous keyframe's
     *       value; CUBICSPLINE evaluates the Hermite spline from the per-key in/out tangents
     *       (rotation results are normalized afterwards, per the glTF spec)
     */
    export channel_sample sample_channel(animation_sampler const& sampler, animation_path path, float t);

    /**
     * @ingroup gltf_loader
     * @brief one node's animated state at a point in time: the TRS base pose (see deren::gltf::node)
     *        overridden by every channel of @p animation that targets it, plus the morph target
     *        weights when a "weights" channel targets it
     * @note the caller picks the animated node(s) per scene by matching
     *       deren::gltf::node::source_index against animation_channel::target_node, evaluates the
     *       per-node pose through this function, then composes T * R * S to write the node's
     *       local transform (see §8 of docs/gltf_loader_usage.md); weights (when non-empty)
     *       feed the morph blend of the node's mesh (see §10)
     */
    export struct node_pose {
        bool any_channel = false;                // true when at least one channel applied
        bool any_transform = false;              // true when a T/R/S channel applied (local changes)
        glm::vec3 translation = glm::vec3(0.0f); // base pose, overridden per channel path
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        glm::vec3 scale = glm::vec3(1.0f);
        std::vector<float> weights = {}; // active morph weights (weights channel); empty = none
    };

    /**
     * @ingroup gltf_loader
     * @brief evaluate every channel of @p animation targeting @p target_node (an asset node
     *        index, see animation_channel::target_node) at time @p t and merge the results
     *        onto the node's TRS base pose
     * @param animation the animation to play
     * @param target_node asset node index of the animated node
     * @param base the node's TRS base pose (deren::gltf::node translation/rotation/scale)
     * @param t playback time in seconds (clamped per sampler)
     * @return merged pose: node_pose::any_channel == true when at least one channel of the
     *         animation targeted this node and evaluated successfully
     */
    export node_pose sample_node(animation const& animation, std::size_t target_node, node_pose const& base, float t);

    /**
     * @ingroup gltf_loader
     * @brief a scene node: local transform + meshes + child links
     * @note
     *      - the node hierarchy is KEPT: scene.nodes holds the DFS pre-order of every reachable
     *        node (including intermediate transform-only nodes), and each node records its
     *        children as indices into the same scene.nodes list. Callers that need the full
     *        parent->child structure walk children[]; the drawable iterators (scenes::begin /
     *        drawable_iterator) simply flatten the same list and skip mesh-less nodes.
     *      - local_transform is the node's own transform relative to its parent (TRS-composed
     *        matrix, or the node's matrix when the asset stored a raw matrix); transform_matrix
     *        keeps the accumulated world matrix for backward compatibility (same value as the
     *        loader used to expose before the tree was retained)
     *      - source_index links the pool entry back to the glTF asset's node table (animation
     *        channels reference nodes by that index). translation / rotation / scale hold the
     *        TRS base pose when the file declared the node as TRS — the only form the glTF
     *        spec allows animation to target — and stay identity for matrix nodes (which are
     *        never animatable)
     */
    export struct node {
        std::string name = {};                       // glTF node name (empty when unnamed)
        glm::mat4 local_transform = glm::mat4(1.0f); // transform relative to the parent node
        std::vector<mesh> meshes = {};               // meshes attached at this node (may be empty)
        // indices into the owning scene.nodes (DFS pre-order): the direct children of this node
        std::vector<std::size_t> children = {};       // empty for leaves / transform-only nodes
        glm::mat4 transform_matrix = glm::mat4(1.0f); // world matrix (parent * local), kept for compatibility
        // animation substrate: asset node index + TRS base pose (see the @note above)
        std::size_t source_index = 0;                           // index in the glTF asset's node table
        glm::vec3 translation = glm::vec3(0.0f);                // TRS base pose; identity for matrix nodes
        glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity quaternion (w scalar)
        glm::vec3 scale = glm::vec3(1.0f);                      // TRS base pose; identity for matrix nodes
        // index into scenes::skins when this node's mesh is skinned (glTF node.skin); the
        // drawable's JOINTS_0 indices reference the skin's joint list
        std::optional<std::size_t> skin_index = std::nullopt;
        // morph weights override for this node's mesh (glTF node.weights; overrides mesh.weights,
        // empty = fall back to the mesh defaults / animation)
        std::optional<std::vector<float>> weights = std::nullopt;
        // cameras / lights attached to this node (glTF node.camera / KHR_lights_punctual):
        // indices into scenes::cameras / scenes::lights
        std::optional<std::size_t> camera_index = std::nullopt;
        std::optional<std::size_t> light_index = std::nullopt;
    };

    /**
     * @ingroup gltf_loader
     * @brief a named scene containing nodes
     * @note nodes holds every node reachable from the scene roots in DFS pre-order (roots
     *       first); root_indices lists the indices of the scene's root nodes inside nodes
     */
    export struct scene {
        std::string name;
        std::vector<node> nodes;
        std::vector<std::size_t> root_indices = {}; // indices into nodes: the scene roots
    };

    export struct scenes; // forward declaration (defined below; scene_iterator only holds a pointer)

    /**
     * @ingroup gltf_loader
     * @brief one drawable primitive of the scene hierarchy as seen by the iterator:
     *        the primitive plus the world transform of the node that owns it
     * @note the primitive carries its own material_index (primitive::material_index)
     */
    export struct drawable_ref {
        primitive const* primitive = nullptr;
        glm::mat4 transform_matrix = glm::mat4(1.0f);
    };

    /**
     * @ingroup gltf_loader
     * @brief single-pass input iterator over every drawable primitive of every scene:
     *        flattens scene -> node -> mesh -> primitive, skipping empty levels.
     *        Pure cursor over the loaded data (no scratch storage), yields drawable_ref.
     */
    export class scene_iterator {
    public:
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = drawable_ref;
        using difference_type = std::ptrdiff_t;
        using pointer = drawable_ref const*;
        using reference = drawable_ref const&;

        scene_iterator() = default; // end(): the default-constructed iterator is exhausted
        explicit scene_iterator(scenes const& owner);

        reference operator*() const noexcept;
        pointer operator->() const noexcept;
        scene_iterator& operator++();
        void operator++(int32_t);

        friend bool operator==(scene_iterator const& a, scene_iterator const& b) noexcept {
            if (a.exhausted || b.exhausted) {
                return a.exhausted == b.exhausted;
            }
            return a.iterating_scene == b.iterating_scene && a.scene_i == b.scene_i && a.node_i == b.node_i && a.mesh_i == b.mesh_i && a.prim_i == b.prim_i;
        }

    private:
        void advance(); // move to the next primitive, or set exhausted

        scenes const* iterating_scene = nullptr;
        size_t scene_i = 0;
        size_t node_i = 0;
        size_t mesh_i = 0;
        size_t prim_i = 0;
        mutable drawable_ref current = {}; // operator* result cache (valid until ++)
        bool exhausted = true;             // default = end(); begin() clears it before advancing
    };

    /**
     * @ingroup gltf_loader
     * @brief single-pass input iterator over the node TREE of every scene: DFS pre-order
     *        over the retained hierarchy (same document order as the loader stored the pool),
     *        INCLUDING transform-only (mesh-less) nodes. Structural view for the runtime's
     *        scene_node_iterator concept: name + local transform + depth (for rebuilding
     *        parent/child edges with a stack) + how many drawables hang off this node.
     * @note pure CPU cursor over the loaded data (no scratch storage); the drawable
     *       geometry of a node is read through the existing drawable_iterator stream,
     *       which advances over the same pool in the same order (skipping mesh-less
     *       nodes), so the two streams stay aligned node-for-node.
     */
    export class scene_node_iterator {
    public:
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using value_type = node const*;
        using difference_type = std::ptrdiff_t;
        using pointer = node const*;
        using reference = node const*;

        scene_node_iterator() = default; // end(): the default-constructed iterator is exhausted
        explicit scene_node_iterator(scenes const& owner);

        reference operator*() const noexcept;
        pointer operator->() const noexcept;
        scene_node_iterator& operator++();
        void operator++(int32_t);

        // ---- structural getters (satisfy the runtime's scene_node_iterator concept) ----
        [[nodiscard]] std::string_view get_name() const noexcept;
        [[nodiscard]] glm::mat4 get_local_transform() const noexcept;
        [[nodiscard]] std::size_t get_depth() const noexcept;          // 0 = scene root
        [[nodiscard]] std::size_t get_drawable_count() const noexcept; // primitives hanging off this node
        [[nodiscard]] std::size_t get_source_index() const noexcept;   // asset node index (node::source_index)

        friend bool operator==(scene_node_iterator const& a, scene_node_iterator const& b) noexcept {
            if (a.exhausted || b.exhausted) {
                return a.exhausted == b.exhausted;
            }
            return a.iterating_scene == b.iterating_scene && a.scene_i == b.scene_i && a.stack == b.stack;
        }

    private:
        // DFS state: stack of {node index in the scene's pool, next child position to descend
        // into}; the top of the stack is the node currently being visited. root_i tracks which
        // root of the current scene has been pushed last (roots are visited in root_indices order).
        struct frame {
            std::size_t node_i = 0;
            std::size_t next_child = 0;
            friend bool operator==(frame const& a, frame const& b) noexcept {
                return a.node_i == b.node_i && a.next_child == b.next_child;
            }
        };
        void push_next_root(); // advance to the next scene root, or set exhausted
        void descend();        // move to the next node in DFS pre-order

        scenes const* iterating_scene = nullptr;
        std::size_t scene_i = 0;
        std::size_t root_i = 0; // next root of the current scene to visit (index into root_indices)
        std::vector<frame> stack = {};
        bool exhausted = true; // default = end(); begin() clears it before advancing
    };

    /**
     * @ingroup gltf_loader
     * @brief loaded result of a glTF file: textures, materials, animations and scenes
     * @note textures holds one entry per glTF texture (in texture order); material texture_indices
     *       values index into this array; primitive.material_index indexes into materials
     * @note animations holds the file's keyframe animations (glTF animation objects in order);
     *       see deren::gltf::animation — channels target nodes by their asset node index, resolved
     *       against a scene's pool through deren::gltf::node::source_index
     * @note scenes is a range: begin()/end() yield every drawable primitive with its node's
     *       world transform (see deren::gltf::scene_iterator / deren::gltf::drawable_ref), so callers can
     *       iterate the whole scene without manual scene -> node -> mesh -> primitive loops
     * @note nodes_begin()/nodes_end() yield the retained node tree (see deren::gltf::scene_node_iterator)
     */
    export struct scenes {
        std::vector<texture_data> textures;
        std::vector<material> materials;
        std::vector<animation> animations = {};
        std::vector<skin> skins = {};     // file-scoped skins (glTF skin objects in order)
        std::vector<camera> cameras = {}; // file-scoped cameras (glTF camera objects in order)
        std::vector<light> lights = {};   // file-scoped punctual lights (KHR_lights_punctual)
        std::vector<scene> scene;
        // asset-level node lookup: glTF asset node table index (node::source_index) -> the
        // loader's node copy for it. A glTF asset node may be referenced from several scenes,
        // and the loader stores one copy per scene pool (identical metadata); the first copy
        // found during loading represents the node. Consumers that need loader metadata for
        // the nodes that actually live in a runtime scene tree (animation base poses, skin /
        // morph sources) look nodes up HERE by the tree node's source_index instead of
        // iterating every scene pool.
        std::unordered_map<std::size_t, node const*> node_by_source = {};

        [[nodiscard]] scene_iterator begin() const;
        static scene_iterator end() noexcept;
        [[nodiscard]] scene_node_iterator nodes_begin() const;
        static scene_node_iterator nodes_end() noexcept;

        /**
         * @brief the index into `textures` of the one NAMED @p name, or nothing when no texture has it
         *
         * THE OTHER HALF OF THE JOIN `texture_data::name` DESCRIBES: a toon material sidecar refers to its
         * ramp, LUT, matcap and SDF by bare asset name, so this is how a consumer turns such a name into
         * something it can load. A LINEAR SCAN, deliberately - a character carries tens of textures and a map
         * would be a second index to keep in step with the vector for no measurable gain.
         *
         * AN ABSENT NAME IS NOT AN ERROR HERE, because "the sidecar names a texture this file does not have" is
         * a real and common state: the sidecar is written for a character whose optional maps the artist may not
         * have exported. The caller decides what a miss means - the safe answer, and the one the sidecar's own
         * `_Use` rule already implies, is to leave that feature off.
         */
        [[nodiscard]] std::optional<uint16_t> texture_index_by_name(std::string_view name) const noexcept;

        /**
         * @brief the material NAMED @p name, or nullptr when no material has that name
         *
         * THE SECOND HALF OF THE SAME JOIN, one level up: the toon inputs are looked up BY MATERIAL NAME (the
         * sidecar is keyed that way, and the runtime asks its installed `toon_lookup` for "the colour of this
         * material and this lane"), so a consumer that holds a name and needs a per-material fact glTF itself
         * did not carry - the `extras` this loader now reads - has to get from the name back to the material.
         *
         * A LINEAR SCAN, deliberately and for the reason `texture_index_by_name` states: a character carries
         * tens of materials, and a map would be a second index to keep in step with the vector.
         *
         * A MISS IS NOT AN ERROR. A name that matches nothing is a real state (a sidecar written for a model
         * whose materials were renamed, a primitive with no material), and the caller's answer to it is its own
         * rule's - for the extras that means "this material states nothing", which is the same state as a
         * material whose file has no extras at all.
         */
        [[nodiscard]] material const* material_by_name(std::string_view name) const noexcept;
    };

    /**
     * @ingroup gltf_loader
     * @brief load a glTF/GLB file into CPU-side scene data
     * @param file_name path to the .gltf or .glb file
     * @return scenes on success, error_code on failure (file_not_found/file_type_error/file_load_failed)
     */
    export std::expected<scenes, error_code> load_model(std::string_view file_name);

    /**
     * @ingroup gltf_loader
     * @brief async twin of load_model(): runs the parse + texture decode on a std::async
     *        thread; call get() on the returned future when the scene is actually needed
     *        (e.g. to overlap model loading with other startup work)
     */
    export std::future<std::expected<scenes, error_code>> load_model_async(std::string_view file_name);

    // ---- renderer-ready drawable iteration ------------------------------------------------
    // Pure CPU types. The runtime's deren::vulkan::scene_drawable_iterator concept is STRUCTURAL over
    // the member shapes below, so this module never needs vulkan.model (or any Vulkan header);
    // the runtime template converts these values into its internal types itself.

    /** @brief interleaved vertex bytes of one drawable (spans into loader-owned storage) */
    export struct vertex_view {
        std::span<uint8_t const> data = {};
        uint32_t stride = 0;
        uint32_t count = 0;
    };

    /** @brief index bytes of one drawable; width is bytes per index (2 or 4) */
    export struct index_view {
        std::span<uint8_t const> data = {};
        uint8_t width = 2;
        uint32_t count = 0;
    };

    /**
     * @ingroup gltf_loader
     * @brief decoded RGBA8 texture bytes (mip-major) of one material slot
     * @note owner keeps the bytes alive while image_view is copied/moved around; data spans
     *       into *owner. valid == false means the slot is missing.
     */
    export struct image_view {
        std::span<uint8_t const> data = {};
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mip_levels = 1;
        bool valid = false;
        std::shared_ptr<std::vector<uint8_t>> owner = {};
    };

    /** @brief PBR factors of one resolved material (mirrors the runtime's material_factors) */
    export struct resolved_factors {
        glm::vec4 base_color_factor = glm::vec4(1.0f);
        glm::vec4 emissive_factor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        float metallic_factor = 1.0f;
        float roughness_factor = 1.0f;
        float normal_scale = 1.0f;
        float occlusion_strength = 1.0f; // mix(1, sampled AO, strength)
        float alpha_cutoff = 0.5f;       // alphaMode MASK threshold
        bool alpha_mask = false;         // alphaMode == MASK (fragment discard)
        bool alpha_blend = false;        // alphaMode == BLEND (alpha-blended / transparent)
    };

    /**
     * @ingroup gltf_loader
     * @brief one resolved material: the 5 texture slots + PBR factors, indexed by the glTF
     *        material index; a primitive without a material reads defaults instead
     */
    /**
     * @ingroup gltf_loader
     * @brief the character material family a material's NAME identifies, which selects its toon parameters
     *
     * THE DECOMPOSITION IS THE REFERENCE'S, not an invention: DanbaidongRP's toon pipeline ships one shader
     * per family (`PBRToonBase`, `PBRToonFace`, `PBRToonFace_AllDirSDF`, `PBRToonHair`, `PBRToonEye`,
     * `PBRToonEyeBlend`, `PBRToonStockings`, `PBRToonTrans`), and the MME pack this port started from has the
     * same split. This enum is the smaller set those collapse to for a port that does not yet carry the
     * per-family TEXTURES: the families still differ in their toon parameters (a hair shadow is harder and
     * wider than skin's, cloth's dark side is tinted differently), so the split earns its place before the
     * textures arrive - and it is the seam they arrive INTO.
     *
     * `none` is the default and means "no family claimed this material", which is every model that is not a
     * character and every character material whose name says nothing. The toon stage treats it exactly as it
     * treated every material before this existed, so the family test cannot change a frame that has no
     * character in it.
     */
    export enum class toon_family : uint32_t {
        none = 0,  // no family claimed it: the family-independent path
        base = 1,  // generic body/prop geometry
        skin = 2,  // skin: the softest shadow edge and the strongest tint
        face = 3,  // face and its adjacent layers (brow, mouth, lash)
        hair = 4,  // hair: the hardest, widest shadow edge
        eye = 5,   // eye: iris, sclera, highlight
        cloth = 6, // clothing: the flattest, most saturated dark side
    };

    /**
     * @ingroup gltf_loader
     * @brief classify a glTF material name into the family whose toon parameters it uses
     *
     * SUBSTRING MATCHING over a lowercased name, in PRIORITY ORDER, and the order is the substance of the
     * function rather than a detail: the reference's own classifier is written the same way for the same
     * reason - a material called `M_actor_zhuangfy_body_01_eye` would match both `body` and `eye`, and only
     * the order decides. The specific collisions that forced this order, and they are the ones its patterns
     * are written against: `睫毛`/`眉毛` (lash, brow) contain neither skin nor face but belong to the face
     * group; `眼白` (sclera) contains `眼` (eye) so it must be tested before the general eye patterns; and
     * `髪` (hair) vs `肌`/`皮肤` (skin) never overlap, so their order is free.
     *
     * THE NAME IS THE ONLY INPUT, SO THE TABLE HAS TO SPELL EVERY SCRIPT IT EXPECTS TO MEET. The CJK
     * patterns are compared as BYTES, so `顔` (Japanese) is not `颜` (simplified Chinese), `髪` is not
     * `发`, and `靴` is not `鞋` - and because a name that matches nothing is `none`, a missing script does
     * not fail loudly, it quietly costs every material that used it its family's shading. Japanese,
     * simplified and traditional forms are therefore all listed, as are the SINGLE-CHARACTER names an MMD
     * model uses as whole material names (`颜`, `发`, `目`, `眉`, `睫`, `口`, `齿`, `舌`, `鼻`, `鞋`,
     * `裤`), with the group order above absorbing the collisions those create (`手套` contains `手`,
     * `袖口`/`领口` contain `口`).
     *
     * @param name the glTF material name (case-insensitive ASCII; CJK matched byte-wise, all scripts)
     * @return the family, or `toon_family::none` when nothing matched
     */
    export toon_family toon_family_of(std::string_view name);

    /**
     * @ingroup gltf_loader
     * @brief the TWO OVERLAY MASKS a character carries, as the article's two `Trick` shaders distinguish them
     *
     * AN OVERLAY IS NOT A MATERIAL FAMILY, and that is why this is a second enum rather than a seventh
     * `toon_family`. A family says HOW this port SHADES a surface - ramp, layers, specular, the whole toon chain -
     * while an overlay surface is not shaded at all: `MyZmdEyeDarkShader` and `MyZmdHairShadowShader` are drawn by
     * a pass of their own that MULTIPLIES an already shaded character by a mask, and their entire material state is
     * `_MainTex`, `_Color`, `_Alpha` and `_DayStrength`. Both names therefore classify as `toon_family::none`
     * (see the top of `toon_family_of`) and are claimed HERE instead.
     *
     * WHY THE TWO ARE TOLD APART, since the second's fragment is nearly the first's: their masks are driven
     * differently. `MyZmdEyeDarkShader` multiplies by `mask * _Alpha` - the mask is what shapes the eye socket's
     * shadow - while `MyZmdHairShadowShader` multiplies by the scalar `_DayStrength` and never reads its mask at
     * all. Collapsing them into one kind would silently turn one of those into the other.
     *
     * THE NAMES ARE THE ARTICLE'S OWN, not the game's: the game's material sidecar (`ef_char_materials_full`) has no
     * colour and no day strength at all - it drives these masks procedurally from the light angle - so the two
     * shaders being ported are the article's simplification of that mechanism, and these are its two shaders.
     */
    export enum class overlay_kind : uint32_t {
        none = 0,        // not an overlay: every material whose name claims nothing
        eye_dark = 1,    // `MyZmdEyeDarkShader`: multiply by the mask, scaled by `_Alpha`
        hair_shadow = 2, // `MyZmdHairShadowShader`: multiply by `_Color` as far as `_DayStrength` says
    };

    /**
     * @ingroup gltf_loader
     * @brief classify a glTF material name into the overlay channel it belongs to, if any
     *
     * SUBSTRING MATCHING over a lowercased name, like `toon_family_of` and for the same reason (the name is the
     * only input, and a glTF material name is authored text). The two patterns are `eyeshadow` and `hairshadow`,
     * which are the names the game's own assets use - `M_eyeshadow_common_01` and `M_hairshadow_common_01` on
     * `chars\chen_full2.glb`, and `M_S_actor_zhuangfy_eyeshadow_01_lod0` in the game's own package.
     *
     * @param name the glTF material name (case-insensitive ASCII)
     * @return the channel, or `overlay_kind::none` when nothing matched
     */
    export overlay_kind overlay_kind_of(std::string_view name);

    /**
     * @ingroup gltf_loader
     * @brief the FACE SDF's three axes: which way the head looks, and its right and up
     *
     * WHY THE FACE NEEDS A HEAD FRAME AT ALL, and it is the whole reason the SDF lane exists: a low-poly anime
     * face is a few triangles whose normals do not describe its form - the cheeks and the nose are painted into
     * the texture - so its terminator is decided by a distance field thresholded against the light's angle IN
     * THE HEAD'S OWN FRAME rather than by `dot(N, L)`. The surface normal is never consulted.
     *
     * THE FRAME POINTS WHERE THE FACE LOOKS, WHICH IS glTF'S OWN CONVENTION RATHER THAN THE REFERENCE'S. glTF
     * specifies `+Y` up, `+Z` forward and `-X` right, with the front of an asset facing `+Z`, and every
     * character this repository has follows it: the mean bind normal of the face primitive measures
     * `(+0.10 +0.31 +0.95)` on `lizhiyan_toon.glb`, `(-0.18 +0.41 +0.89)` on `zhuangfy_toon.glb` and
     * `(0 -0.38 +0.93)` on the game's own `actor_zhuangfy.glb` - all three face `+Z`.
     *
     * `EfFaceGetHeadBasis` INSTEAD TAKES `-row3` AS FORWARD, and `head_basis_from_axes` therefore reproduces its
     * re-orthogonalisation but NOT its negation. The cost of the negation is measurable rather than cosmetic:
     * with `front = -row3` the light's angle in the head frame comes out at 0.83 for a sun the face is looking
     * almost straight into, the SDF atlas' mean threshold is 0.55, and 94.1% of the texels the face's own UVs
     * sample are then read as SHADOW - the whole face wears the family's shadow tint and the lane stops shaping
     * anything. With the sign corrected, the same measurement gives 11.9%.
     */
    export struct head_basis {
        glm::vec3 front = glm::vec3(0.0f, 0.0f, 1.0f);
        glm::vec3 right = glm::vec3(-1.0f, 0.0f, 0.0f);
        glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
        /// false when these are the FALLBACK constants rather than a real bone's axes - see
        /// `head_basis_fallback`. A caller that needs to know whether the head frame is real reads this; the
        /// shader does not, because the two cases differ only in whether the head can turn.
        bool from_skeleton = false;
    };

    /**
     * @ingroup gltf_loader
     * @brief the fallback head frame: glTF's own basis, `headFront = (0,0,1)`, `headRight = (-1,0,0)`,
     *        `headUp = (0,1,0)`
     *
     * IT IS DELIBERATELY NOT THE REFERENCE'S CONSTANTS, which substitute `headFront = (0,0,-1)` for a bone that
     * is missing or degenerate - 180 degrees away from the face of every asset measured here (see the note on
     * `head_basis`). It still matters that a fallback exists, because this repository's SDF-bearing models are
     * exactly the ones that reach it: `zhuangfy_scalar.glb` has NO SKELETON - seven nodes, zero skins, a static
     * pose split by body part - and for a model that cannot turn its head a fixed frame and a bone matrix give
     * the same three vectors.
     */
    export head_basis head_basis_fallback() noexcept;

    /**
     * @ingroup gltf_loader
     * @brief the head frame a head bone's matrix describes, or the fallback when it describes none
     *
     * TAKES THE TWO AXES RATHER THAN A MATRIX, deliberately: the reference reads `_31_32_33` and `_11_12_13`
     * off a row-major HLSL matrix, and expressing that as a `glm::mat4` column here would be a convention
     * argument nobody could check by reading. The caller extracts the rows; this function does the part that
     * has a right answer - normalise, negate, re-orthogonalise, and fall back when the axes are degenerate.
     *
     * @param forward_axis the head's forward row (HLSL `_31_32_33`), NOT yet negated
     * @param right_axis the head's right row (HLSL `_11_12_13`), NOT yet negated
     * @return the frame, `from_skeleton == true` only when both axes were usable and not parallel
     */
    export head_basis head_basis_from_axes(glm::vec3 forward_axis, glm::vec3 right_axis) noexcept;

    /**
     * @ingroup gltf_loader
     * @brief whether a glTF node's name is a head BONE's
     *
     * TOKEN EQUALITY RATHER THAN A SUBSTRING, and that is the substance of the function: models name bones
     * `head`, `Head`, `Bip01 Head`, `J_Head` and `Head_Nub`, and a substring test over `head` also matches
     * `headgear`, `overhead` and `Forehead` - which are exactly the sort of props and accessories that sit in
     * the same skeleton. The name is lowercased and split on non-alphanumerics, and a token has to BE `head`.
     * The CJK names are matched as substrings because they have no separators to split on.
     */
    export bool looks_like_head_joint(std::string_view node_name) noexcept;

    /**
     * @ingroup gltf_loader
     * @brief the index of the head JOINT inside a skin, or nothing when that skin has no head bone
     *
     * @return an index into `skin::joints` (NOT an asset node index - a caller needs it to pick the joint's
     *         matrix out of the per-frame skin matrix array, which is in joint order), or `std::nullopt`
     * @param scene_index which scene's node pool to resolve the joints against; `skin::joints` holds ASSET node
     *        indices, which are matched through `node::source_index`
     */
    export std::optional<std::size_t> head_joint_of(scenes const& scene, std::size_t scene_index, std::size_t skin_index) noexcept;

    /**
     * @ingroup gltf_loader
     * @brief a glTF material that has been RESOLVED: its factors, its five decoded texture slots, and the
     *        toon family its name classified into
     */
    export struct resolved_material {
        std::array<image_view, 5> slots = {}; // albedo, metallic_roughness, normal, occlusion, emissive
        resolved_factors factors = {};
        bool double_sided = false; // glTF doubleSided: disable back-face culling + flip normals
        /**
         * THE MATERIAL'S NAME, carried alongside the family it classified into.
         *
         * The family alone is NOT enough for the toon material sidecar, and this is why: the sidecar is keyed by
         * MATERIAL NAME, so a consumer holding only the family cannot find the entry that describes this
         * material - it would have to guess, and two materials of one family (the character's two cloth
         * materials, or its face and its brow) would collapse onto one entry. Carrying the name here is the
         * same decision as carrying it on `deren::gltf::material`, one stage closer to the consumer.
         */
        std::string name = {};
        /// the TOON FAMILY this material's name classified into (see toon_family_of), resolved ONCE here so
        /// no later stage re-derives it from a string
        uint32_t toon_family = 0;
        /// the OVERLAY CHANNEL this material's name classified into (see overlay_kind_of), resolved at the
        /// same moment as the family and for the same reason. 0 == `overlay_kind::none`, i.e. an ordinary
        /// surface; a non-zero value means this material is drawn by the overlay pass rather than shaded.
        uint32_t overlay_kind = 0;
    };

    /**
     * @ingroup gltf_loader
     * @brief resolve every scene material once into resolved_material: factors plus the 5
     *        texture slots, decoded to RGBA8 with full mip chains (CPU-side; shared glTF
     *        textures decode once). No Vulkan types involved.
     */
    export std::vector<resolved_material> resolve_materials(deren::gltf::scenes const& scenes);

    /**
     * @ingroup gltf_loader
     * @brief async twin of resolve_materials(): runs the texture decode + mip generation on a
     *        std::async thread; @p scenes must stay alive until the future is consumed
     */
    export std::future<std::vector<resolved_material>> resolve_materials_async(deren::gltf::scenes const& scenes);

    /**
     * @ingroup gltf_loader
     * @brief iterator over every drawable primitive of the scene that models
     *        deren::vulkan::scene_drawable_iterator: ++ advances, then geometry/material are read
     *        through the getters (get_vertex / get_index / get_transform + one getter per
     *        material slot), all as pure CPU values. Interleaved geometry is built lazily per
     *        drawable and cached until the next increment. Feed it directly to
     *        runtime::import_scene(): the runtime drives the traversal and converts.
     * @note default-constructed instance == end() (exhausted inner deren::gltf::scene_iterator)
     */
    export class drawable_iterator {
    public:
        drawable_iterator() = default; // end

        drawable_iterator(deren::gltf::scenes const& scenes, std::span<resolved_material const> materials)
            : inner(scenes)
            , materials(materials) {
        }

        drawable_iterator& operator++();

        friend bool operator!=(drawable_iterator const& a, drawable_iterator const& b) {
            return a.inner != b.inner;
        }

        vertex_view get_vertex() const;
        index_view get_index() const;
        glm::mat4 get_transform() const;
        image_view get_albedo() const;
        image_view get_metallic_roughness() const;
        image_view get_normal() const;
        image_view get_occlusion() const;
        image_view get_emissive() const;
        resolved_factors get_factors() const;
        bool get_double_sided() const;
        /// the current drawable's TOON FAMILY (see toon_family_of), already resolved with the material
        uint32_t get_toon_family() const;
        /// the current drawable's OVERLAY CHANNEL (see overlay_kind_of); 0 == `overlay_kind::none`, i.e. the
        /// material is an ordinary shaded surface and not one of the article's two framebuffer multiplies
        uint32_t get_overlay_kind() const;
        /// the current drawable's MATERIAL NAME, which is what the toon material sidecar is keyed by
        [[nodiscard]] std::string_view get_material_name() const;

    private:
        void ensure_built() const; // build the current drawable's interleaved geometry lazily
        resolved_material const* current_material() const;
        image_view slot(int32_t i) const;

        deren::gltf::scene_iterator inner;
        std::span<resolved_material const> materials = {};
        // scratch geometry of the current drawable (built lazily, valid until ++)
        mutable std::vector<uint8_t> vertex_bytes = {};
        mutable std::vector<uint8_t> index_bytes = {};
        mutable uint32_t vertex_stride = 0;
        mutable uint32_t vertex_count = 0;
        mutable uint8_t index_width = 2; // 2 or 4 bytes per index
        mutable uint32_t index_count = 0;
        mutable bool built = false;
    };

    /**
     * @ingroup gltf_loader
     * @brief world-space axis-aligned bounding box of a whole loaded model (see
     *        compute_scene_bounds): the minimal AABB enclosing every drawable primitive of
     *        every scene, with the glTF node world transforms applied
     */
    export struct scene_bounds {
        bool valid = false;              // false when the model has no position-carrying primitive
        glm::vec3 min = glm::vec3(0.0f); // AABB min corner
        glm::vec3 max = glm::vec3(0.0f); // AABB max corner
        std::size_t primitive_count = 0; // drawable primitives that contributed
    };

    /**
     * @ingroup gltf_loader
     * @brief world AABB of every drawable primitive of the model: each primitive's local AABB
     *        (from its raw POSITION attribute) is transformed by its node's world transform
     *        (TRS maps an AABB to an AABB, so transforming the 8 corners is exact) and united.
     * @note pure CPU over the retained scene data — the renderer uses this to frame the camera
     *       and center the scene before building anything (see main.cpp); primitives without
     *       POSITION are skipped
     */
    export scene_bounds compute_scene_bounds(deren::gltf::scenes const& scenes);

    /**
     * @ingroup gltf_loader
     * @brief diagnostics: log what the loader exported for @p scenes — the contents summary
     *        (textures/materials/primitives), the world AABB framing numbers (min/max/center/
     *        radius), the retained hierarchy shape (roots/nodes/max depth, per-node tree lines)
     *        and the animations/skins/morph targets/cameras/lights present in the file.
     * @param scenes the loaded model
     * @return the world scene bounds (see compute_scene_bounds); always valid — panics when the
     *         model has no drawable primitives (the caller needs geometry to frame the camera)
     */
    export scene_bounds log_scene_diagnostics(deren::gltf::scenes const& scenes);
} // namespace deren::gltf
