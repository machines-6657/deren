# gltf_loader Usage Guide

> CPU-side module for loading glTF / GLB files, with no Vulkan dependency.
> Backend: [fastgltf](https://github.com/spnda/fastgltf) (vendored under `third_party/fastgltf`, compiled from source into a `fastgltf_vendored` static target together with simdjson), textures decoded with stb_image.

- Language standard: C++23 (C++20 module)
- Public interface: `gltf_loader/gltf_loader.cppm` (`import deren.gltf_loader;`)
- Entry point: `deren::gltf::load_model(path)` -> `std::expected<deren::gltf::scenes, deren::gltf::error_code>`

---

## 1. API Overview

```cpp
namespace gltf {
    enum class component_type : int {          // glTF OpenGL constants
        byte_t = 5120, unsigned_byte_t = 5121, short_t = 5122, unsigned_short_t = 5123,
        int_t = 5124, unsigned_int_t = 5125, float_t = 5126, double_t = 5130, unknown = 0,
    };
    enum class element_type { scale, vec2, vec3, vec4, mat2, mat3, mat4, unknown };

    constexpr component_type to_component_type(int gltf_constant);
    constexpr uint8_t        get_component_size(component_type);
    constexpr int            to_gltf_macro_type(component_type);
    constexpr element_type   to_element_type(int gltf_constant);
    constexpr uint8_t        get_element_size(element_type);

    enum class error_code { file_not_found, file_type_error, file_load_failed };

    struct texture_data  { std::vector<unsigned char> data; uint32_t width, height; uint8_t component; };
    struct vertex_portion{ std::vector<unsigned char> data; component_type component; };
    struct material_factors { glm::vec4 base_color_factor; glm::vec3 emissive_factor;
                              float metallic_factor; float roughness_factor; float normal_scale; };
    struct material      { material_factors factors; std::map<std::string, uint16_t> texture_indices; };
    struct primitive     { std::map<std::string, vertex_portion> vertex;
                           std::vector<unsigned char> index;
                           component_type index_component_type;
                           uint32_t material_index; };   // into scenes::materials, UINT32_MAX = none
    struct mesh          { std::vector<primitive> primitives; };
    struct node          { std::string name; glm::mat4 local_transform;
                           std::vector<mesh> meshes; std::vector<std::size_t> children;
                           glm::mat4 transform_matrix; };   // see semantics below
    struct scene         { std::string name; std::vector<node> nodes;
                           std::vector<std::size_t> root_indices; };
    struct scenes        { std::vector<texture_data> textures; std::vector<material> materials;
                           std::vector<scene> scene; };

    enum class animation_interpolation { linear, step, cubic_spline };
    enum class animation_path         { translation, rotation, scale, weights }; // weights = morph-target channels (see section 10)
    struct animation_sampler  { std::vector<float> times; std::vector<float> values;
                                animation_interpolation interpolation; };
    struct animation_channel  { animation_path path; std::size_t sampler; std::size_t target_node; };
    struct animation          { std::string name;
                                std::vector<animation_sampler> samplers;
                                std::vector<animation_channel> channels; };
    struct skin               { std::string name; std::vector<std::size_t> joints;   // asset node indices
                                std::vector<glm::mat4> inverse_bind_matrices; };     // identity fallback
    // scenes also carries: std::vector<animation> animations; std::vector<skin> skins; and each
    // node carries source_index + translation/rotation/scale (TRS base pose) + skin_index  - 
    // see section 8 (animations) and section 9 (skinning)

    std::expected<scenes, error_code> load_model(std::string_view file_name);
}
```

**Semantics**

- The node hierarchy is **kept**: `scene.nodes` holds every node reachable from the scene
  roots in **DFS pre-order** (roots first, then each subtree), and `root_indices` lists the
  indices of the scene's roots inside `nodes`. Each node records its direct children as
  indices into the same `nodes` list (`node.children`, always greater than the node's own
  index), so the full parent->child structure is recoverable. Transform-only (intermediate)
  nodes appear too, with empty `meshes`.
- `local_transform` is the node's **own** transform relative to its parent (TRS-composed
  matrix, or the raw matrix when the asset stored one). `transform_matrix` keeps the
  accumulated **world-space** matrix (`parent_world * local_transform`)  -  same value the
  loader exposed before the hierarchy was retained, so existing world-space consumers keep
  working unchanged.
- Keys of `primitive.vertex` are glTF attribute names (`POSITION` / `NORMAL` / `TEXCOORD_0` / `COLOR_0` ...).
- Vertex and index data are **de-interleaved raw bytes**; their type is described by `component` / `index_component_type` (byteStride and sparse accessors are already handled by fastgltf).
- `scenes.materials` holds one entry per glTF material (in material order); each `material.texture_indices` uses the fixed roles `albedo` / `metallic_roughness` / `normal` / `occlusion` / `emissive`, and the values index into `scenes::textures` (in glTF texture order). `primitive.material_index` selects the primitive's material (`UINT32_MAX` when the glTF primitive has none).
- `scenes.animations` holds the file's keyframe animations in glTF order (see [section 8 Animations](#8-animations)). An animation is file-scoped  -  its channels can target nodes of any scene. Channel targets reference nodes by their **asset node index**, which each scene-pool `node.source_index` records.
- `scenes.node_by_source` is the **asset-level node lookup**: glTF asset node index (`node::source_index`) -> the loader's `node` copy for it (first scene pool that contains it; a node referenced from several scenes has identical copies). Consumers that animate or skin the nodes of a *runtime* scene tree  -  where each tree node only carries its `source_index`  -  look loader metadata up here instead of iterating every scene pool: `scenes.node_by_source.at(tree_node->source_index)` yields that node's TRS base pose, `skin_index`, attached meshes etc. (`animation::controller` does exactly this: the scene tree is the authoritative host, the loader table is pure data.)

---

## 2. Quick Start: Loading and Error Handling

```cpp
import deren.gltf_loader;
import deren.vstd; // the project's trimmed STL module (was `import std;`)

int main() {
    auto result = deren::gltf::load_model("assets/box.glb");   // auto-detects GLB / JSON
    if (!result) {
        switch (result.error()) {
            case deren::gltf::error_code::file_not_found:  std::println("file not found"); break;
            case deren::gltf::error_code::file_type_error:  std::println("not a valid glTF/GLB"); break;
            case deren::gltf::error_code::file_load_failed: std::println("parse failed (details on stderr)"); break;
        }
        return 1;
    }
    const auto& model = *result;   // deren::gltf::scenes
    std::println("scenes={} textures={}", model.scene.size(), model.textures.size());
}
```

> Three input forms are supported: ASCII `.gltf` (with external `.bin` / images), binary `.glb`, and embedded files using data URIs. Loading requires
> `LoadExternalBuffers | LoadExternalImages` (already configured inside the module).

---

## 3. Traversing the Scene Hierarchy

Two ways to visit the scene:

1. **Flat (drawable-oriented)**  -  iterate `scene.nodes` in order; every node that has meshes
   is a drawable with a ready world matrix. This is what the renderer's bounds scan and
   `drawable_iterator` use.

```cpp
for (const auto& scene : model.scene) {
    std::println("scene: {}", scene.name);
    for (const auto& node : scene.nodes) {
        const glm::mat4& world = node.transform_matrix;   // world matrix, ready to use
        for (const auto& mesh : node.meshes) {
            for (const auto& prim : mesh.primitives) {
                // each primitive is an independently drawable unit
            }
        }
    }
}
```

2. **Tree (hierarchy-oriented)**  -  start at `scene.root_indices` and recurse through
   `node.children`, composing world matrices from `local_transform` when you need to
   re-evaluate them (e.g. for animation / programmatic whole-group transforms):

```cpp
for (const auto& scene : model.scene) {
    // node is an index into scene.nodes; parent_world is identity at the roots
    auto walk = [&](auto&& self, std::size_t node, const glm::mat4& parent_world) -> void {
        const auto& n = scene.nodes[node];
        const glm::mat4 world = parent_world * n.local_transform;
        for (std::size_t child : n.children) self(self, child, world);
    };
    for (std::size_t root : scene.root_indices) walk(walk, root, glm::mat4(1.0f));
}
```

---

## 4. Reading Vertices and Indices

```cpp
const auto& prim = /* see above */;

// --- Vertex attributes ---
const auto& pos = prim.vertex.at("POSITION");
if (pos.component == deren::gltf::component_type::float_t) {
    auto* p = reinterpret_cast<const float*>(pos.data.data());
    const std::size_t vertex_count = pos.data.size() / (3 * sizeof(float));
    // p[i*3 + 0..2] = position of the i-th vertex
}

// --- Indices (uint16 / uint32 are common in glTF) ---
if (prim.index_component_type == deren::gltf::component_type::unsigned_short_t) {
    auto* idx = reinterpret_cast<const uint16_t*>(prim.index.data());
    const std::size_t index_count = prim.index.size() / sizeof(uint16_t);
}
```

Per-attribute byte sizes can be cross-checked with the helper functions:

```cpp
// one element = get_element_size(vec3) * get_component_size(float) = 3 * 4 = 12 bytes
if (pos.data.size() != deren::gltf::get_element_size(deren::gltf::element_type::vec3)
                       * deren::gltf::get_component_size(deren::gltf::component_type::float_t)
                       * vertex_count) { /* inconsistent data */ }
```

---

## 5. Using Textures

```cpp
// material lookup (default factors when the primitive has no material)
if (prim.material_index < model.materials.size()) {
    const deren::gltf::material& mat = model.materials[prim.material_index];
    // factors: mat.factors.base_color_factor / metallic_factor / roughness_factor / ...
    if (const auto it = mat.texture_indices.find("albedo"); it != mat.texture_indices.end()) {
        const deren::gltf::texture_data& tex = model.textures[it->second];
        // tex.data: width * height * component 8-bit pixels (already decoded, not raw PNG/JPEG bytes)
        std::println("texture {}x{} channels={} bytes={}",
                     tex.width, tex.height, tex.component, tex.data.size());
    }
}
```

| role                 | source                                          |
|----------------------|-------------------------------------------------|
| `albedo`             | `pbrMetallicRoughness.baseColorTexture`         |
| `metallic_roughness` | `pbrMetallicRoughness.metallicRoughnessTexture` |
| `normal`             | `normalTexture`                                 |
| `occlusion`          | `occlusionTexture`                              |
| `emissive`           | `emissiveTexture`                               |

---

## 6. Integration with Vulkan Rendering

`vulkan/model`'s `draw()` currently hardcodes `VK_INDEX_TYPE_UINT32`, while the loader preserves the original index type, so a conversion is needed before upload:

```cpp
std::vector<uint32_t> indices32;
if (prim.index_component_type == deren::gltf::component_type::unsigned_short_t) {
    auto* src = reinterpret_cast<const uint16_t*>(prim.index.data());
    indices32.assign(src, src + prim.index.size() / 2);
} else if (prim.index_component_type == deren::gltf::component_type::unsigned_int_t) {
    auto* src = reinterpret_cast<const uint32_t*>(prim.index.data());
    indices32.assign(src, src + prim.index.size() / 4);
}
```

Vertex buffers can be uploaded by memcpy'ing the raw bytes directly, provided the pipeline's vertex input layout matches the data layout (e.g. POSITION/NORMAL as `VK_FORMAT_R32G32B32_SFLOAT`, stored contiguously); otherwise re-arrange into an interleaved struct before upload.

---

## 7. Notes and Limitations

- The module is compiled with `-fno-exceptions`: fastgltf reports errors via `Expected` and never throws.
- Runtime dependencies: none beyond the OS  -  fastgltf/simdjson are compiled into the executable from `third_party/`, and the Windows Release exe is fully static.
- Static render data (meshes, vertices/indices, textures, materials), **keyframe animations** (section 8), **skins** (section 9) and **morph targets** (section 10) are exported. Non-indexed glTF primitives are supported: the loader synthesizes a sequential uint32 index buffer.
- Cameras and punctual lights (KHR_lights_punctual) are exported too: `scenes.cameras` / `scenes.lights` hold the file's cameras and lights in glTF order, and each node records its attachment through `camera_index` / `light_index`. Authored cameras are consumed in `main.cpp` as orbit-camera viewpoint seeds (gui "camera" selector); point/spot lights are loaded by `main.cpp` into the runtime's editable gui light slots (up to the GPU cap, fixed to the node's base-pose world transform, position offset by the scene import shift), so they light the scene and stay adjustable in the overlay. KHR *directional* lights are not mapped: the engine's sun is the shadow-casting analytic light configured by `enable_shadows()`, so the loader logs them as ignored.
- `log_scene_diagnostics(scenes)` is a one-call diagnostic dump: it logs the contents summary (textures/materials/primitives), the world AABB framing numbers (min/max/center/radius), the retained hierarchy shape (per-node tree lines) and the animations/skins/morph targets/cameras/lights present in the file, and returns the `scene_bounds` (panics when the model has no drawable primitives). `main.cpp` calls it once after loading, before framing the orbit camera.
- All `asset.scenes` are loaded; `asset.defaultScene` is not separately marked yet.
- Verified samples (in `main.cpp`): the glTF/GLB variants of `glTF-Sample-Assets/Models/{Box, BoxInterleaved, BoxTextured}`, covering ASCII + external bin, binary containers, interleaved byteStride, embedded textures, and the missing-file error code.

---

## 8. Animations

The loader decodes glTF keyframe animation into `scenes.animations` (glTF order, one entry per `animation` object). It is **file-scoped**: animations are not tied to a scene, and their channels can target nodes of any scene. Only static data is exported - evaluating keyframes into transforms is the consumer's job (playback runs in `main.cpp`, see the end of this section).

**Layout per animation**

```cpp
animation            // name + samplers + channels
|-- samplers[i]      // animation_sampler: decoded keyframes
|     times          //   one float per keyframe (seconds, non-decreasing as stored)
|     values         //   flat floats, see below
|     interpolation  //   linear | step | cubic_spline
`-- channels[j]      // animation_channel: animate one property of one node
      path           //   translation | rotation | scale | weights (morph)
      sampler        //   index into the owning animation's samplers
      target_node    //   index in the glTF asset's node table (NOT a scene pool index)
```

- `values` layout: LINEAR / STEP hold `key_count * components` floats  -  3 per key for
  `translation`/`scale` (xyz triplets), 4 for `rotation` (xyzw, `w` scalar). CUBICSPLINE holds
  `key_count * components * 3` floats, grouped per keyframe in glTF order: in-tangent, value,
  out-tangent.
- glTF only allows float animation accessors, but any numeric component type is converted to
  float when present (robustness against non-conforming files). A sampler whose accessors are
  out of range or unsupported stays in the list with **empty** `times`/`values` (its index
  alignment is preserved); skip empty samplers.
- `weights` channels (morph targets) are exported like any other path - a channel is dropped (and counted) only when its target/sampler cannot be resolved, or when a weights channel targets a node whose mesh carries no morphable primitives (see section 10).

**Resolving a channel to a scene node**

`target_node` is the node's index in the asset's node table; each pool entry of every scene
records its source index, so a channel resolves against a scene by scanning:

```cpp
// animate nodes of scene[0] with animation a0's translation channels
const auto& anim = model.animations[0];
const auto& scene = model.scene[0];
std::vector<std::pair<deren::gltf::node*, const deren::gltf::animation_sampler*>> animated;
for (const auto& ch : anim.channels) {
    if (ch.path != deren::gltf::animation_path::translation) continue;
    const auto& sampler = anim.samplers[ch.sampler];            // skip if times empty
    for (auto& node : scene.nodes) {
        if (node.source_index == ch.target_node) {
            animated.emplace_back(&node, &sampler);
        }
    }
}
```

**TRS base pose**

`node.translation` / `node.rotation` (`glm::quat`, identity when unset) / `node.scale` hold the
declared TRS when the node's file transform is TRS  -  the only form the glTF spec allows
animation to target. Nodes declared as matrices keep identity TRS (they are never animatable);
only their composed `local_transform` matters. To evaluate keyframes later, compose
`T * R * S` per node from the (possibly overridden) TRS components and write the result back as
the node's local transform.

**Sampling / playback**

The loader also evaluates keyframes (`sample_channel`, and the higher-level `sample_node` that
merges every channel of one animation targeting one node onto its base pose):

```cpp
deren::gltf::animation const& anim = model.animations[0];
// per animated node of the scene (match node.source_index against channel.target_node):
deren::gltf::node_pose pose{.translation = node.translation, .rotation = node.rotation, .scale = node.scale};
pose = deren::gltf::sample_node(anim, node.source_index, pose, t); // base pose + channels at t (seconds)
if (pose.any_channel) {
    node.local = glm::translate(glm::mat4(1.0f), pose.translation)
               * glm::mat4_cast(pose.rotation)
               * glm::scale(glm::mat4(1.0f), pose.scale);
}
```

- `sample_channel(sampler, path, t)` returns a `channel_sample` (vec3 for translation/scale,
  quaternion for rotation). `t` is clamped to the sampler's keyframe range; an empty/broken
  sampler yields `valid == false`.
- Interpolation follows the sampler's mode: LINEAR lerps translations/scales and slerps
  rotations along the shortest arc (one endpoint is flipped when the quaternions are far
  apart); STEP holds the previous keyframe; CUBICSPLINE evaluates the Hermite spline with the
  per-key in/out tangents (tangents scaled by the segment duration, rotation results
  normalized afterwards, as the spec requires).
- Playback itself is a consumer concern: `main.cpp` plays the first channel-bearing animation
  of the loaded model on a loop (writes the evaluated `T * R * S` back into the runtime scene
  tree each frame via `scene_node::source_index`), and the `gui` demo adds play/pause, a time
  scrubber and an animation dropdown for multi-animation files.

---

## 9. Skinning

`scenes.skins` holds the file's skins in glTF order:

```cpp
struct skin {
    std::string name;                              // e.g. "Armature"
    std::vector<std::size_t> joints;               // asset node indices, in joint order
    std::vector<glm::mat4> inverse_bind_matrices;  // one mat4 per joint (identity when omitted)
};
```

- A skinned mesh is attached to a node whose `node.skin_index` selects a skin. Its drawable
  vertices carry `JOINTS_0` (u8/u16 vec4 -> joint indices **into the skin's joint list**) and
  `WEIGHTS_0` (float or normalized u8/u16 vec4); the interleaved drawable layout keeps both
  (identity joints + full weight for unskinned meshes), so every pipeline shares one vertex
  layout.
- `inverse_bind_matrices` are decoded from the asset's `Mat4` float accessor; when the asset
  omits them (glTF default) or the accessor is broken, identity matrices are filled in.
- Joints are asset node indices: resolve them against a scene's pool through
  `node::source_index` (same as animation channels), then evaluate per frame:

```cpp
// skinMat_j = inv(W_mesh) * W_joint_j * IBM_j  -  world matrices of the skinned mesh node and of
// each joint (the joints follow the animation evaluation of section 8); per-vertex the shader blends
// these matrices by WEIGHTS_0 and the node's own world transform applies afterwards.
```

- The renderer (`main.cpp`) assigns each skinned primitive a block of the shared skin-matrix
  buffer (`material_push_constants::skin_base`; indices 0-3 are the identity block used by
  unskinned draws), rebuilds the per-skin matrices every frame and uploads them via
  `runtime::set_skin_matrices()`; the geometry stages of `pbr.slang` / `shadow.slang` sample them (their mesh
  entries - there is no vertex geometry entry any more, see docs/mesh_shaders.md step 4). Verified with
  `glTF-Sample-Assets` `RiggedSimple` (2 joints) and `BrainStem` (18 joints).

---

## 10. Morph targets

Morphable primitives are exported together with their targets and weights:

```cpp
struct morph_target {                                  // one per target of a primitive
    std::map<std::string, vertex_portion> attributes;  // POSITION / NORMAL deltas (float vec3,
                                                       // same vertex count as the base data)
};
// primitive.targets            -> the primitive's morph targets
// mesh.weights                 -> default morph weights (one per target; empty = all zero)
// node.weights (optional)      -> per-node override of the mesh defaults
// animation "weights" channels -> drive the node's weights over time (see section 8)
```

- A `weights` animation channel's sampler stores one scalar per keyframe **per morph target** of
  the node's mesh (`animation_sampler::per_key` = that target count; the loader resolves it from
  the target node). `sample_node` merges the evaluated values into `node_pose::weights`;
  `node_pose::any_transform` stays false for weights-only channels, so playback code must not
  touch the node's local transform for them.
- Per-vertex the blend is `base + sum(weight_i * delta_i)` (positions and normals); the renderer
  bakes each morphable primitive's deltas plus its **default weights** (`node.weights` >
  `mesh.weights` > zeros) into the scene morph buffer (the morph-data heap slot) and rewrites the weights
  region every frame when a `weights` channel animates the node. The geometry stages blend the
  morph deltas **before** skinning (the glTF order; both are linear on the same local vertex).
  Verified with `AnimatedMorphCube`, `SimpleMorph` and `MorphStressTest`.
