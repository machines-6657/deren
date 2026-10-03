module;

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <fastgltf/types.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
// THE ONE PLACE THIS PROJECT'S SOURCE SEES SIMDJSON, and it sees it because fastgltf reports a material's
// `extras` block by CALLING BACK with a `simdjson::dom::object*` - there is no other way to reach that block
// through this fastgltf version, which parses `extras` and does not store it (see `ExtrasParseCallback` in
// fastgltf/core.hpp and the call site in fastgltf.cpp's `parseMaterials`). The include is PRIVATE and SYSTEM in
// CMake, so nothing that consumes `gltf_loader` inherits either the type or the warnings of upstream code.
#include <simdjson.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
// `std::optional`: `scenes::texture_index_by_name` answers "no such name" with one, and `load_texture`'s
// name lookup is a lookup that can miss by design.
#include <optional>

module deren.gltf_loader;

import deren.utility;

namespace {
    using fastgltf::Asset;

    struct parsed_data {
        std::vector<uint8_t> data;
        deren::gltf::component_type component_type = deren::gltf::component_type::unknown;
        deren::gltf::element_type element_type = deren::gltf::element_type::unknown;
        uint64_t count = 0;
        uint64_t byte_size = 0;
    };

    deren::gltf::component_type to_component_type(fastgltf::ComponentType const type) {
        switch (type) {
        case fastgltf::ComponentType::Byte:
            return deren::gltf::component_type::byte_t;
        case fastgltf::ComponentType::UnsignedByte:
            return deren::gltf::component_type::unsigned_byte_t;
        case fastgltf::ComponentType::Short:
            return deren::gltf::component_type::short_t;
        case fastgltf::ComponentType::UnsignedShort:
            return deren::gltf::component_type::unsigned_short_t;
        case fastgltf::ComponentType::Int:
            return deren::gltf::component_type::int_t;
        case fastgltf::ComponentType::UnsignedInt:
            return deren::gltf::component_type::unsigned_int_t;
        case fastgltf::ComponentType::Float:
            return deren::gltf::component_type::float_t;
        case fastgltf::ComponentType::Double:
            return deren::gltf::component_type::double_t;
        default:
            return deren::gltf::component_type::unknown;
        }
    }

    deren::gltf::element_type to_element_type(fastgltf::AccessorType const type) {
        switch (type) {
        case fastgltf::AccessorType::Scalar:
            return deren::gltf::element_type::scale;
        case fastgltf::AccessorType::Vec2:
            return deren::gltf::element_type::vec2;
        case fastgltf::AccessorType::Vec3:
            return deren::gltf::element_type::vec3;
        case fastgltf::AccessorType::Vec4:
            return deren::gltf::element_type::vec4;
        case fastgltf::AccessorType::Mat2:
            return deren::gltf::element_type::mat2;
        case fastgltf::AccessorType::Mat3:
            return deren::gltf::element_type::mat3;
        case fastgltf::AccessorType::Mat4:
            return deren::gltf::element_type::mat4;
        default:
            return deren::gltf::element_type::unknown;
        }
    }

    deren::gltf::error_code to_error_code(fastgltf::Error const error) {
        switch (error) {
        case fastgltf::Error::InvalidFileData:
        case fastgltf::Error::InvalidGLB:
        case fastgltf::Error::InvalidJson:
        case fastgltf::Error::InvalidGltf:
        case fastgltf::Error::InvalidOrMissingAssetField:
        case fastgltf::Error::UnsupportedVersion:
            return deren::gltf::error_code::file_type_error;
        default:
            return deren::gltf::error_code::file_load_failed;
        }
    }

    // fastgltf does not define bitwise operators for Options, combine flags via the underlying type.
    constexpr fastgltf::Options load_options() {
        constexpr auto flags = static_cast<std::uint64_t>(fastgltf::Options::LoadExternalBuffers) | static_cast<std::uint64_t>(fastgltf::Options::LoadExternalImages);
        return static_cast<fastgltf::Options>(flags);
    }

    template <typename T>
    std::vector<uint8_t> copy_accessor(Asset const& asset, fastgltf::Accessor const& accessor) {
        std::vector<uint8_t> data(accessor.count * sizeof(T));
        fastgltf::copyFromAccessor<T>(asset, accessor, data.data());
        return data;
    }

    // copyFromAccessor handles byteStride de-interleaving, sparse accessors and
    // normalized component conversion for every (AccessorType, ComponentType) pair
    // that fastgltf's ElementTraits provides.
    parsed_data get_data_from_accessor(Asset const& asset, fastgltf::Accessor const& accessor) {
        using namespace fastgltf;
        using namespace fastgltf::math;

        parsed_data result;
        result.component_type = to_component_type(accessor.componentType);
        result.element_type = to_element_type(accessor.type);
        result.count = accessor.count;

        switch (accessor.type) {
        case AccessorType::Scalar:
            switch (accessor.componentType) {
            case ComponentType::Byte:
                result.data = copy_accessor<std::int8_t>(asset, accessor);
                break;
            case ComponentType::UnsignedByte:
                result.data = copy_accessor<std::uint8_t>(asset, accessor);
                break;
            case ComponentType::Short:
                result.data = copy_accessor<std::int16_t>(asset, accessor);
                break;
            case ComponentType::UnsignedShort:
                result.data = copy_accessor<std::uint16_t>(asset, accessor);
                break;
            case ComponentType::Int:
                result.data = copy_accessor<std::int32_t>(asset, accessor);
                break;
            case ComponentType::UnsignedInt:
                result.data = copy_accessor<std::uint32_t>(asset, accessor);
                break;
            case ComponentType::Float:
                result.data = copy_accessor<float>(asset, accessor);
                break;
            case ComponentType::Double:
                result.data = copy_accessor<double>(asset, accessor);
                break;
            default:
                break;
            }
            break;
        case AccessorType::Vec2:
            switch (accessor.componentType) {
            case ComponentType::Byte:
                result.data = copy_accessor<s8vec2>(asset, accessor);
                break;
            case ComponentType::UnsignedByte:
                result.data = copy_accessor<u8vec2>(asset, accessor);
                break;
            case ComponentType::Short:
                result.data = copy_accessor<s16vec2>(asset, accessor);
                break;
            case ComponentType::UnsignedShort:
                result.data = copy_accessor<u16vec2>(asset, accessor);
                break;
            case ComponentType::Int:
                result.data = copy_accessor<s32vec2>(asset, accessor);
                break;
            case ComponentType::UnsignedInt:
                result.data = copy_accessor<u32vec2>(asset, accessor);
                break;
            case ComponentType::Float:
                result.data = copy_accessor<fvec2>(asset, accessor);
                break;
            case ComponentType::Double:
                result.data = copy_accessor<dvec2>(asset, accessor);
                break;
            default:
                break;
            }
            break;
        case AccessorType::Vec3:
            switch (accessor.componentType) {
            case ComponentType::Byte:
                result.data = copy_accessor<s8vec3>(asset, accessor);
                break;
            case ComponentType::UnsignedByte:
                result.data = copy_accessor<u8vec3>(asset, accessor);
                break;
            case ComponentType::Short:
                result.data = copy_accessor<s16vec3>(asset, accessor);
                break;
            case ComponentType::UnsignedShort:
                result.data = copy_accessor<u16vec3>(asset, accessor);
                break;
            case ComponentType::Int:
                result.data = copy_accessor<s32vec3>(asset, accessor);
                break;
            case ComponentType::UnsignedInt:
                result.data = copy_accessor<u32vec3>(asset, accessor);
                break;
            case ComponentType::Float:
                result.data = copy_accessor<fvec3>(asset, accessor);
                break;
            case ComponentType::Double:
                result.data = copy_accessor<dvec3>(asset, accessor);
                break;
            default:
                break;
            }
            break;
        case AccessorType::Vec4:
            switch (accessor.componentType) {
            case ComponentType::Byte:
                result.data = copy_accessor<s8vec4>(asset, accessor);
                break;
            case ComponentType::UnsignedByte:
                result.data = copy_accessor<u8vec4>(asset, accessor);
                break;
            case ComponentType::Short:
                result.data = copy_accessor<s16vec4>(asset, accessor);
                break;
            case ComponentType::UnsignedShort:
                result.data = copy_accessor<u16vec4>(asset, accessor);
                break;
            case ComponentType::Int:
                result.data = copy_accessor<s32vec4>(asset, accessor);
                break;
            case ComponentType::UnsignedInt:
                result.data = copy_accessor<u32vec4>(asset, accessor);
                break;
            case ComponentType::Float:
                result.data = copy_accessor<fvec4>(asset, accessor);
                break;
            case ComponentType::Double:
                result.data = copy_accessor<dvec4>(asset, accessor);
                break;
            default:
                break;
            }
            break;
        case AccessorType::Mat2:
            result.data = copy_accessor<fmat2x2>(asset, accessor);
            break;
        case AccessorType::Mat3:
            result.data = copy_accessor<fmat3x3>(asset, accessor);
            break;
        case AccessorType::Mat4:
            result.data = copy_accessor<fmat4x4>(asset, accessor);
            break;
        default:
            break;
        }

        result.byte_size = result.data.size();
        return result;
    }

    // Extract raw bytes from a fastgltf data source (loaded buffers/images are sources::Array).
    std::span<std::byte const> get_source_bytes(Asset const& asset, fastgltf::DataSource const& source) {
        using namespace fastgltf;
        if (auto const* array = std::get_if<sources::Array>(&source)) {
            return {array->bytes.data(), array->bytes.size_bytes()};
        }
        if (auto const* vector = std::get_if<sources::Vector>(&source)) {
            return {vector->bytes.data(), vector->bytes.size()};
        }
        if (auto const* byte_view = std::get_if<sources::ByteView>(&source)) {
            return byte_view->bytes;
        }
        if (auto const* buffer_view = std::get_if<sources::BufferView>(&source)) {
            auto const& view = asset.bufferViews[buffer_view->bufferViewIndex];
            auto const& buffer = asset.buffers[view.bufferIndex];
            auto bytes = get_source_bytes(asset, buffer.data);
            return bytes.subspan(view.byteOffset, view.byteLength);
        }
        return {};
    }

    deren::gltf::texture_data load_texture(Asset const& asset, std::size_t const texture_index) {
        deren::gltf::texture_data out;
        if (texture_index >= asset.textures.size()) {
            return out;
        }
        auto const& texture = asset.textures[texture_index];
        if (!texture.imageIndex) {
            return out;
        }
        auto const& image = asset.images[*texture.imageIndex];
        // THE NAME IS SET BEFORE ANYTHING CAN FAIL, so a texture that decodes to nothing is still FINDABLE by
        // name: a consumer asking for the sidecar's ramp should be able to tell "this file has that image but it
        // did not decode" from "this file has no such image", and only the name makes that distinction
        // expressible. It is the join between the toon sidecar and this file - see texture_data::name.
        out.name = std::string(image.name);
        auto const bytes = get_source_bytes(asset, image.data);
        if (bytes.empty()) {
            return out;
        }

        int32_t width = 0;
        int32_t height = 0;
        int32_t channels = 0;
        uint8_t* pixels = stbi_load_from_memory(
            reinterpret_cast<uint8_t const*>(bytes.data()),
            static_cast<int32_t>(bytes.size()),
            &width, &height, &channels, 0);
        if (pixels == nullptr) {
            return out;
        }

        out.data.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * static_cast<std::size_t>(channels));
        out.width = static_cast<uint32_t>(width);
        out.height = static_cast<uint32_t>(height);
        out.component = static_cast<uint8_t>(channels);
        stbi_image_free(pixels);
        return out;
    }

    std::map<std::string, uint16_t> get_texture_indices(fastgltf::Material const& material) {
        std::map<std::string, uint16_t> texture_indices;
        if (material.pbrData.baseColorTexture) {
            texture_indices["albedo"] = static_cast<uint16_t>(material.pbrData.baseColorTexture->textureIndex);
        }
        if (material.pbrData.metallicRoughnessTexture) {
            texture_indices["metallic_roughness"] = static_cast<uint16_t>(material.pbrData.metallicRoughnessTexture->textureIndex);
        }
        if (material.occlusionTexture) {
            texture_indices["occlusion"] = static_cast<uint16_t>(material.occlusionTexture->textureIndex);
        }
        if (material.normalTexture) {
            texture_indices["normal"] = static_cast<uint16_t>(material.normalTexture->textureIndex);
        }
        if (material.emissiveTexture) {
            texture_indices["emissive"] = static_cast<uint16_t>(material.emissiveTexture->textureIndex);
        }
        return texture_indices;
    }

    // ---- THE MATERIAL `extras` BLOCK, WHICH FASTGLTF REPORTS BY CALLBACK ----
    //
    // WHY A CALLBACK RATHER THAN A FIELD: this fastgltf version parses each material's `extras` object and,
    // unless a callback is installed, DROPS it - `fastgltf::Material` has no member for it and the JSON is not
    // retained (see `ExtrasParseCallback` in fastgltf/core.hpp and its call site in fastgltf.cpp's
    // `parseMaterials`). The callback fires once per material while the array is walked and passes
    // `asset.materials.size()` as the object index; the material is appended to that vector AFTER the callback
    // runs, so the index IS the material's final index and this vector is addressed exactly like
    // `asset.materials` - in step by construction rather than by a search.
    struct material_extras {
        std::vector<std::map<std::string, float>> floats = {};
    };

    void collect_material_extras(simdjson::dom::object* extras, std::size_t const object_index, fastgltf::Category const object_type, void* user_pointer) {
        if (object_type != fastgltf::Category::Materials || extras == nullptr || user_pointer == nullptr) {
            return; // another object's extras: this loader claims material rows and nothing else
        }
        auto& collected = *static_cast<material_extras*>(user_pointer);
        if (collected.floats.size() <= object_index) {
            collected.floats.resize(object_index + 1);
        }
        // THE BLOCK'S NAME IS THE ASSET PIPELINE'S (`efFloats`), and its absence is not an error: `extras` is
        // free-form in glTF, so a file whose `extras` is some other tool's is a file this loader has nothing to
        // say about - the material simply states no claimed value and every consumer's fallback answers.
        simdjson::dom::object block;
        if (extras->at_key("efFloats").get_object().get(block) != simdjson::SUCCESS) {
            return;
        }
        std::map<std::string, float>& out = collected.floats[object_index];
        // ONLY THE CLAIMED NAMES (see `deren::gltf::claimed_extras_floats`): an allow-list scan rather than a copy of
        // the block, so the number of values that enter the model is the number of values something reads.
        for (std::string_view const claimed : deren::gltf::claimed_extras_floats) {
            double value = 0.0;
            if (block.at_key(claimed).get_double().get(value) == simdjson::SUCCESS) {
                out.emplace(std::string(claimed), static_cast<float>(value));
            }
        }
    }

    deren::gltf::material load_material(fastgltf::Material const& material, std::map<std::string, float> const* const extras_floats) {
        deren::gltf::material result;
        result.factors.base_color_factor = glm::vec4(material.pbrData.baseColorFactor[0],
                                                     material.pbrData.baseColorFactor[1],
                                                     material.pbrData.baseColorFactor[2],
                                                     material.pbrData.baseColorFactor[3]);
        result.factors.metallic_factor = material.pbrData.metallicFactor;
        result.factors.roughness_factor = material.pbrData.roughnessFactor;
        result.factors.emissive_factor = glm::vec3(material.emissiveFactor[0],
                                                   material.emissiveFactor[1],
                                                   material.emissiveFactor[2]);
        result.factors.normal_scale = material.normalTexture ? material.normalTexture->scale : 1.0f;
        result.factors.occlusion_strength = material.occlusionTexture ? material.occlusionTexture->strength : 1.0f;
        // alphaMode: MASK gets a fragment-discard threshold; BLEND marks a transparent material
        // (the runtime draws it alpha-blended, back-to-front, depth-write off)
        result.factors.alpha_cutoff = material.alphaCutoff;
        result.factors.alpha_mask = material.alphaMode == fastgltf::AlphaMode::Mask;
        result.factors.alpha_blend = material.alphaMode == fastgltf::AlphaMode::Blend;
        result.double_sided = material.doubleSided;
        result.texture_indices = get_texture_indices(material);
        // THE NAME IS CARRIED, not used here: it is what `toon_family_of` classifies a character material by
        // (see gltf_loader.cppm), and this is the last point in the pipeline where it still exists.
        result.name = material.name;
        // ... AND THE CLAIMED `extras` ROWS, verbatim from the asset: the loader does not interpret them, does
        // not default them and does not carry the ones nobody reads (see `claimed_extras_floats`). A material
        // whose block states none of them keeps an empty map, which is the same state as a file with no extras.
        if (extras_floats != nullptr) {
            result.extras_floats = *extras_floats;
        }
        return result;
    }

    deren::gltf::primitive load_primitive(fastgltf::Primitive const& primitive, Asset const& asset) {
        std::map<std::string, deren::gltf::vertex_portion> vertex;
        for (auto const& attribute : primitive.attributes) {
            auto data = get_data_from_accessor(asset, asset.accessors[attribute.accessorIndex]);
            std::string const name(attribute.name);
            vertex[name].component = data.component_type;
            vertex[name].data = std::move(data.data);
        }

        // morph targets: per-target displacement attributes (POSITION/NORMAL deltas), decoded
        // exactly like the base attributes (same vertex count per target). The glTF spec
        // requires morph-target deltas to be FLOAT; non-float attributes are dropped here so
        // consumers can read the data as float deltas without a per-portion component check.
        std::vector<deren::gltf::morph_target> targets;
        targets.reserve(primitive.targets.size());
        for (auto const& target_attributes : primitive.targets) {
            deren::gltf::morph_target target;
            for (auto const& attribute : target_attributes) {
                auto data = get_data_from_accessor(asset, asset.accessors[attribute.accessorIndex]);
                if (data.component_type != deren::gltf::component_type::float_t) {
                    continue; // non-conforming morph delta: drop (consumers blend float deltas)
                }
                std::string const name(attribute.name);
                target.attributes[name].component = data.component_type;
                target.attributes[name].data = std::move(data.data);
            }
            targets.push_back(std::move(target));
        }

        parsed_data index_data;
        if (primitive.indicesAccessor) {
            index_data = get_data_from_accessor(asset, asset.accessors[*primitive.indicesAccessor]);
        }

        return {
            .vertex = std::move(vertex),
            .targets = std::move(targets),
            .index = std::move(index_data.data),
            .index_component_type = index_data.component_type,
            .material_index = primitive.materialIndex
                                  ? static_cast<uint32_t>(*primitive.materialIndex)
                                  : std::numeric_limits<uint32_t>::max(),
        };
    }

    glm::mat4 to_glm_mat4(fastgltf::math::fmat4x4 const& matrix) {
        glm::mat4 result;
        for (int32_t column = 0; column < 4; ++column) {
            for (int32_t row = 0; row < 4; ++row) {
                result[column][row] = matrix[column][row];
            }
        }
        return result;
    }

    deren::gltf::scene load_scene(Asset const& asset, std::size_t const scene_index) {
        deren::gltf::scene result;
        result.name = asset.scenes[scene_index].name;

        // Build one deren::gltf::node per asset node, keeping the DFS pre-order the pool layout
        // promises (a node is immediately followed by its whole subtree). The traversal is
        // ITERATIVE with an explicit stack instead of recursion: a hostile file with a deeply
        // nested / cyclic node graph must not overflow the call stack, and cycles must not loop
        // forever. A child index outside asset.nodes is skipped, never dereferenced.
        //
        // Per-node conversion (name/TRS/meshes/weights) is identical to the recursive version.
        // enter_node() appends the converted node to the pool and returns its pool index.
        auto const enter_node = [&result, &asset](std::size_t const node_index, fastgltf::math::fmat4x4 const& parent_world) -> std::size_t {
            fastgltf::Node const& fnode = asset.nodes[node_index];
            fastgltf::math::fmat4x4 const world = fastgltf::getTransformMatrix(fnode, parent_world);

            deren::gltf::node current_node = {};
            current_node.name = fnode.name;
            // Link the pool entry back to the asset's node table (animation channels target
            // nodes by this index) and keep the declared TRS base pose when the node is TRS:
            // per the glTF spec animation only ever targets TRS properties, so matrix nodes
            // (non-animatable) leave translation/rotation/scale at identity.
            current_node.source_index = node_index;
            if (fnode.skinIndex) {
                current_node.skin_index = *fnode.skinIndex; // glTF node.skin -> scenes::skins
            }
            // glTF node.weights: per-node morph weights, overriding the mesh defaults when present
            if (!fnode.weights.empty()) {
                std::vector<float> node_weights;
                node_weights.reserve(fnode.weights.size());
                for (fastgltf::num const w : fnode.weights) {
                    node_weights.push_back(static_cast<float>(w));
                }
                current_node.weights = std::move(node_weights);
            }
            // cameras / lights attached to this node (raw property references -> scenes tables)
            if (fnode.cameraIndex) {
                current_node.camera_index = *fnode.cameraIndex;
            }
            if (fnode.lightIndex) {
                current_node.light_index = *fnode.lightIndex;
            }
            if (auto const* trs = std::get_if<fastgltf::TRS>(&fnode.transform)) {
                current_node.translation = glm::vec3(trs->translation[0], trs->translation[1], trs->translation[2]);
                glm::quat rotation = {};
                // fastgltf stores quaternions as xyzw with w the scalar, same layout as glm
                rotation.x = trs->rotation[0];
                rotation.y = trs->rotation[1];
                rotation.z = trs->rotation[2];
                rotation.w = trs->rotation[3];
                current_node.rotation = rotation;
                current_node.scale = glm::vec3(trs->scale[0], trs->scale[1], trs->scale[2]);
            }
            current_node.local_transform = to_glm_mat4(fastgltf::getTransformMatrix(fnode)); // base = identity -> own transform
            current_node.transform_matrix = to_glm_mat4(world);
            if (fnode.meshIndex) {
                deren::gltf::mesh current_mesh = {};
                std::string_view const mesh_name = asset.meshes[*fnode.meshIndex].name;
                std::size_t prim_index = 0;
                for (auto const& primitive : asset.meshes[*fnode.meshIndex].primitives) {
                    // glTF allows POINTS/LINES/STRIP/FAN modes too; only TRIANGLES is renderable
                    // by the pipeline (the interleaved mesh builder below assumes triangles), so
                    // skip the others with a warning instead of drawing garbage.
                    if (primitive.type != fastgltf::PrimitiveType::Triangles) {
                        deren::utility::log("gltf: skipping primitive {} of mesh '{}': mode {} is not supported (only TRIANGLES render)",
                                            prim_index, mesh_name, static_cast<int32_t>(primitive.type));
                        ++prim_index;
                        continue;
                    }
                    current_mesh.primitives.push_back(load_primitive(primitive, asset));
                    ++prim_index;
                }
                // glTF mesh.weights: default morph weights (one per target of the primitives)
                for (fastgltf::num const w : asset.meshes[*fnode.meshIndex].weights) {
                    current_mesh.weights.push_back(static_cast<float>(w));
                }
                current_node.meshes.push_back(std::move(current_mesh));
            }

            result.nodes.push_back(std::move(current_node));
            return result.nodes.size() - 1;
        };

        // one DFS frame: the asset node being expanded + how many of its children are done.
        // Its pool index is implicit: parent frames record the pool index each child will start
        // at BEFORE the child subtree is appended (pre-order), so when a frame pops, the parent
        // appends that recorded index to its children list.
        struct frame {
            std::size_t node_index = 0;      // asset node index
            std::size_t child_i = 0;         // next child of asset.nodes[node_index].children
            fastgltf::math::fmat4x4 world{}; // accumulated world of this node (for its children)
        };
        std::vector<frame> stack;
        stack.reserve(64);
        // asset node index -> its pool index (first occurrence). Guards against revisiting a
        // node: a valid glTF tree never repeats a node; a cycle would otherwise loop forever.
        std::unordered_map<std::size_t, std::size_t> node_pool_index;

        // expand one node: append it (as a pool root or as the parent's next child) and push its
        // frame. Returns the pool index the node occupies.
        auto const expand = [&](std::size_t const node_index, std::size_t const parent_pool_index, fastgltf::math::fmat4x4 const& parent_world) -> bool {
            if (node_index >= asset.nodes.size()) {
                deren::utility::log("gltf: out-of-range node reference {} (asset has {} nodes), skipping", node_index, asset.nodes.size());
                return false;
            }
            auto const [it, inserted] = node_pool_index.try_emplace(node_index, 0);
            if (!inserted) {
                deren::utility::log("gltf: node {} referenced more than once (cyclic or shared graph), skipping the repeat", node_index);
                return false;
            }
            std::size_t const pool_index = enter_node(node_index, parent_world);
            it->second = pool_index;
            if (parent_pool_index != std::numeric_limits<std::size_t>::max()) {
                // pre-order: this node's pool index is the child slot of its parent
                result.nodes[parent_pool_index].children.push_back(pool_index);
            }
            fastgltf::Node const& fnode = asset.nodes[node_index];
            fastgltf::math::fmat4x4 const world = fastgltf::getTransformMatrix(fnode, parent_world);
            stack.push_back(frame{node_index, 0, world});
            return true;
        };

        for (std::size_t const root : asset.scenes[scene_index].nodeIndices) {
            // world starts from identity for scene roots
            if (!expand(root, std::numeric_limits<std::size_t>::max(), fastgltf::math::fmat4x4{})) {
                continue; // bad root reference: skip, keep the scene loadable
            }
            result.root_indices.push_back(result.nodes.size() - 1);
            while (!stack.empty()) {
                frame& top = stack.back();
                fastgltf::Node const& fnode = asset.nodes[top.node_index];
                if (top.child_i >= fnode.children.size()) {
                    stack.pop_back(); // this node's subtree done
                    continue;
                }
                std::size_t const child = fnode.children[top.child_i++];
                // expand the child under this node (child's world = parent world * local)
                expand(child, node_pool_index.at(top.node_index), top.world);
            }
        }
        return result;
    }

    // ---- animation: decode keyframe accessors into flat float arrays ----

    // Read one scalar component (little-endian, element index @p index) of a decoded accessor
    // byte buffer as a float, converting any supported component type.
    float read_float_component(uint8_t const* data, deren::gltf::component_type const type, std::size_t const index) {
        switch (type) {
        case deren::gltf::component_type::float_t: {
            float value;
            std::memcpy(&value, data + index * sizeof(float), sizeof(float));
            return value;
        }
        case deren::gltf::component_type::double_t: {
            double value;
            std::memcpy(&value, data + index * sizeof(double), sizeof(double));
            return static_cast<float>(value);
        }
        case deren::gltf::component_type::byte_t:
            return static_cast<float>(static_cast<std::int8_t>(data[index]));
        case deren::gltf::component_type::unsigned_byte_t:
            return static_cast<float>(data[index]);
        case deren::gltf::component_type::short_t: {
            std::int16_t value;
            std::memcpy(&value, data + index * sizeof(std::int16_t), sizeof(std::int16_t));
            return static_cast<float>(value);
        }
        case deren::gltf::component_type::unsigned_short_t: {
            std::uint16_t value;
            std::memcpy(&value, data + index * sizeof(std::uint16_t), sizeof(std::uint16_t));
            return static_cast<float>(value);
        }
        case deren::gltf::component_type::int_t: {
            std::int32_t value;
            std::memcpy(&value, data + index * sizeof(std::int32_t), sizeof(std::int32_t));
            return static_cast<float>(value);
        }
        case deren::gltf::component_type::unsigned_int_t: {
            std::uint32_t value;
            std::memcpy(&value, data + index * sizeof(std::uint32_t), sizeof(std::uint32_t));
            return static_cast<float>(value);
        }
        default:
            return 0.0f;
        }
    }

    // Decode one accessor into a flat float array: Scalar elements -> 1 value each, Vec3 -> 3,
    // Vec4 -> 4 (any supported component type is converted to float). Returns empty for
    // element/component combinations that cannot hold animation values (Mat*, vec2, unknown).
    std::vector<float> flatten_float_accessor(Asset const& asset, fastgltf::Accessor const& accessor) {
        parsed_data const raw = get_data_from_accessor(asset, accessor);
        std::size_t components = 0;
        switch (accessor.type) {
        case fastgltf::AccessorType::Scalar:
            components = 1;
            break;
        case fastgltf::AccessorType::Vec3:
            components = 3;
            break;
        case fastgltf::AccessorType::Vec4:
            components = 4;
            break;
        default:
            return {}; // not a valid animation value shape
        }
        if (raw.data.empty() || raw.component_type == deren::gltf::component_type::unknown) {
            return {};
        }
        std::vector<float> out;
        out.reserve(raw.count * components);
        for (std::size_t i = 0; i < raw.count * components; ++i) {
            out.push_back(read_float_component(raw.data.data(), raw.component_type, i));
        }
        return out;
    }

    deren::gltf::animation load_animation(Asset const& asset, std::size_t const animation_index) {
        fastgltf::Animation const& f_anim = asset.animations[animation_index];
        deren::gltf::animation result;
        result.name = f_anim.name;

        // Samplers: keyframe times (Scalar input accessor) + output values (Vec3/Vec4 output
        // accessor). A sampler with an out-of-range accessor or one that decodes to empty is
        // still pushed (as an empty entry) so channel->sampler indices stay aligned with the
        // file; consumers skip samplers with empty times.
        for (fastgltf::AnimationSampler const& f_sampler : f_anim.samplers) {
            deren::gltf::animation_sampler sampler = {};
            switch (f_sampler.interpolation) {
            case fastgltf::AnimationInterpolation::Step:
                sampler.interpolation = deren::gltf::animation_interpolation::step;
                break;
            case fastgltf::AnimationInterpolation::CubicSpline:
                sampler.interpolation = deren::gltf::animation_interpolation::cubic_spline;
                break;
            case fastgltf::AnimationInterpolation::Linear:
                [[fallthrough]];
            default:
                sampler.interpolation = deren::gltf::animation_interpolation::linear;
                break;
            }
            if (f_sampler.inputAccessor < asset.accessors.size()) {
                sampler.times = flatten_float_accessor(asset, asset.accessors[f_sampler.inputAccessor]);
            }
            if (f_sampler.outputAccessor < asset.accessors.size()) {
                sampler.values = flatten_float_accessor(asset, asset.accessors[f_sampler.outputAccessor]);
            }
            result.samplers.push_back(std::move(sampler));
        }

        // Channels: export TRS + morph-weights paths. A channel whose target/sampler cannot be
        // resolved (or a weights channel whose node carries no morphable mesh) is dropped.
        std::size_t skipped_weights = 0;
        for (fastgltf::AnimationChannel const& f_channel : f_anim.channels) {
            if (!f_channel.nodeIndex || f_channel.samplerIndex >= result.samplers.size()) {
                continue; // broken reference: cannot resolve
            }
            deren::gltf::animation_channel channel = {};
            channel.sampler = f_channel.samplerIndex;
            channel.target_node = *f_channel.nodeIndex;
            std::size_t per_key = 0;
            switch (f_channel.path) {
            case fastgltf::AnimationPath::Translation:
                channel.path = deren::gltf::animation_path::translation;
                per_key = 3;
                break;
            case fastgltf::AnimationPath::Rotation:
                channel.path = deren::gltf::animation_path::rotation;
                per_key = 4;
                break;
            case fastgltf::AnimationPath::Scale:
                channel.path = deren::gltf::animation_path::scale;
                per_key = 3;
                break;
            case fastgltf::AnimationPath::Weights: {
                // morph weights: one scalar per keyframe per morph target of the node's mesh
                fastgltf::Node const& fnode = asset.nodes[*f_channel.nodeIndex];
                if (!fnode.meshIndex || *fnode.meshIndex >= asset.meshes.size() || asset.meshes[*fnode.meshIndex].primitives.empty()) {
                    ++skipped_weights;
                    continue;
                }
                std::size_t const target_count = asset.meshes[*fnode.meshIndex].primitives[0].targets.size();
                if (target_count == 0) {
                    ++skipped_weights;
                    continue;
                }
                channel.path = deren::gltf::animation_path::weights;
                per_key = target_count;
                break;
            }
            default:
                continue;
            }
            // record the per-keyframe shape on the sampler (a sampler shared by several channels
            // keeps the first shape it was seen with)
            deren::gltf::animation_sampler& sampler = result.samplers[channel.sampler];
            if (sampler.per_key == 0) {
                sampler.per_key = per_key;
            }
            result.channels.push_back(channel);
        }
        if (skipped_weights > 0) {
            std::string_view const anim_name = result.name.empty() ? std::string_view("<unnamed>") : std::string_view(result.name);
            deren::utility::log("gltf: animation '{}': {} morph-weight channel(s) skipped (node has no morphable mesh)", anim_name, skipped_weights);
        }
        return result;
    }

    // ---- skins: decode inverse bind matrices (Mat4 float accessor, column-major) ----

    std::vector<glm::mat4> read_mat4_accessor(Asset const& asset, fastgltf::Accessor const& accessor) {
        std::vector<glm::mat4> out;
        if (accessor.type != fastgltf::AccessorType::Mat4 || accessor.componentType != fastgltf::ComponentType::Float) {
            return out;
        }
        parsed_data const raw = get_data_from_accessor(asset, accessor);
        if (raw.data.size() < accessor.count * sizeof(glm::mat4)) {
            return out;
        }
        out.reserve(accessor.count);
        for (std::size_t i = 0; i < accessor.count; ++i) {
            glm::mat4 m = {};
            std::memcpy(&m, raw.data.data() + i * sizeof(glm::mat4), sizeof(glm::mat4));
            out.push_back(m);
        }
        return out;
    }

    deren::gltf::skin load_skin(Asset const& asset, std::size_t const skin_index) {
        fastgltf::Skin const& f_skin = asset.skins[skin_index];
        deren::gltf::skin result;
        result.name = f_skin.name;
        result.joints.assign(f_skin.joints.begin(), f_skin.joints.end());
        if (f_skin.inverseBindMatrices && *f_skin.inverseBindMatrices < asset.accessors.size()) {
            result.inverse_bind_matrices = read_mat4_accessor(asset, asset.accessors[*f_skin.inverseBindMatrices]);
        }
        if (result.inverse_bind_matrices.size() != result.joints.size()) {
            // omitted (glTF default: identity) or broken IBM accessor: fall back to identity
            result.inverse_bind_matrices.clear();
            result.inverse_bind_matrices.assign(result.joints.size(), glm::mat4(1.0f));
        }
        return result;
    }

    // ---- cameras + punctual lights (KHR_lights_punctual): raw property export only ----

    deren::gltf::camera load_camera(fastgltf::Camera const& f_camera) {
        deren::gltf::camera result;
        result.name = f_camera.name;
        if (auto const* perspective = std::get_if<fastgltf::Camera::Perspective>(&f_camera.camera)) {
            result.type = deren::gltf::camera_type::perspective;
            result.yfov = static_cast<float>(perspective->yfov);
            result.znear = static_cast<float>(perspective->znear);
            if (perspective->aspectRatio) {
                result.aspect_ratio = static_cast<float>(*perspective->aspectRatio);
            }
            if (perspective->zfar) {
                result.zfar = static_cast<float>(*perspective->zfar);
            }
        } else if (auto const* orthographic = std::get_if<fastgltf::Camera::Orthographic>(&f_camera.camera)) {
            result.type = deren::gltf::camera_type::orthographic;
            result.xmag = static_cast<float>(orthographic->xmag);
            result.ymag = static_cast<float>(orthographic->ymag);
            result.ortho_znear = static_cast<float>(orthographic->znear);
            result.ortho_zfar = static_cast<float>(orthographic->zfar);
        }
        return result;
    }

    deren::gltf::light load_light(fastgltf::Light const& f_light) {
        deren::gltf::light result;
        result.name = f_light.name;
        switch (f_light.type) {
        case fastgltf::LightType::Directional:
            result.type = deren::gltf::light_type::directional;
            break;
        case fastgltf::LightType::Point:
            result.type = deren::gltf::light_type::point;
            break;
        case fastgltf::LightType::Spot:
            result.type = deren::gltf::light_type::spot;
            break;
        }
        result.color = glm::vec3(static_cast<float>(f_light.color[0]), static_cast<float>(f_light.color[1]), static_cast<float>(f_light.color[2]));
        result.intensity = static_cast<float>(f_light.intensity);
        if (f_light.range) {
            result.range = static_cast<float>(*f_light.range);
        }
        if (f_light.innerConeAngle) {
            result.spot_inner_cone = static_cast<float>(*f_light.innerConeAngle);
        }
        if (f_light.outerConeAngle) {
            result.spot_outer_cone = static_cast<float>(*f_light.outerConeAngle);
        }
        return result;
    }

    // ---- CPU-side geometry building for drawable_iterator (interleaved pbr.vert layout) ----

    // Interleaved vertex, layout matches pbr.vert / shadow.vert input locations 0,1,2,4,5
    // (stride 64): position(12) normal(12) uv(8) joints(16) weights(16). No tangent attribute:
    // pbr.frag rebuilds the TBN frame from screen-space derivatives (mirrored UVs included).
    struct vertex {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 uv;
        glm::uvec4 joints = glm::uvec4(0u);                    // JOINTS_0 (indices into the node's skin)
        glm::vec4 weights = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); // WEIGHTS_0 (identity when unskinned)
    };

    struct built_mesh {
        std::vector<vertex> vertices;
        std::vector<uint8_t> index_data;
        uint8_t index_width = 2; // bytes per index (2 or 4)
        uint32_t index_count = 0;
    };

    built_mesh build_mesh(deren::gltf::primitive const& prim) {
        built_mesh result;
        auto const get_portion = [&prim](std::string_view const name) -> deren::gltf::vertex_portion const* {
            auto const it = prim.vertex.find(std::string(name));
            return it == prim.vertex.end() ? nullptr : &it->second;
        };
        auto const* position_portion = get_portion("POSITION");
        auto const* normal_portion = get_portion("NORMAL");
        auto const* uv_portion = get_portion("TEXCOORD_0");
        // skinned attributes (optional): JOINTS_0 is u8/u16 vec4 of joint indices into the
        // node's skin, WEIGHTS_0 is float vec4 (or normalized u8/u16)
        auto const* joints_portion = get_portion("JOINTS_0");
        auto const* weights_portion = get_portion("WEIGHTS_0");
        if (position_portion == nullptr) {
            deren::utility::panic("primitive has no POSITION attribute");
        }

        constexpr glm::vec2 default_uv(0.0f, 0.0f);

        // ---- typed attribute reader: one element of @p portion as floats -------------------------
        // glTF does NOT require POSITION / NORMAL / TEXCOORD_0 to be float: the core spec allows
        // TEXCOORD_0 as normalized UNSIGNED_BYTE / UNSIGNED_SHORT, and KHR_mesh_quantization extends
        // that to POSITION / NORMAL as BYTE / SHORT. Reinterpreting such a buffer as glm::vec3 /
        // glm::vec2 reads 4x (u8) or 2x (u16) past its end, so the component type has to be honoured
        // on the way in. The index is clamped against the portion's REAL element count for the same
        // reason: that count is the byte length divided by the component size, which is what a
        // size()/sizeof(glm::vec3) shortcut gets wrong for every non-float type.
        //
        // @param signed_normalized true for a signed normalized integer component, whose full range
        //        maps to [-1, 1] instead of [0, 1] (glTF's signed normalized conversion) - the case
        //        a quantized NORMAL hits. Unnormalized integers are converted as-is, which is the
        //        honest reading of a file whose accessor says so.
        auto const read_attribute = []<std::size_t N>(deren::gltf::vertex_portion const& portion, std::size_t const index, bool const signed_normalized) -> std::array<float, N> {
            std::array<float, N> out = {};
            std::size_t const component_bytes = deren::gltf::get_component_size(portion.component);
            if (component_bytes == 0) {
                return out; // unknown component type: nothing to read
            }
            std::size_t const elements = portion.data.size() / component_bytes;
            if (index * N + N > elements) {
                return out; // short accessor: the count guard below already logged the mismatch
            }
            if (portion.component == deren::gltf::component_type::float_t) {
                auto const* const base = reinterpret_cast<float const*>(portion.data.data());
                for (std::size_t c = 0; c < N; ++c) {
                    out[c] = base[index * N + c];
                }
                return out;
            }
            auto const* const base = portion.data.data();
            for (std::size_t c = 0; c < N; ++c) {
                std::size_t const at = index * N + c;
                switch (portion.component) {
                case deren::gltf::component_type::unsigned_byte_t:
                    out[c] = static_cast<float>(base[at]) / 255.0f;
                    break;
                case deren::gltf::component_type::byte_t: {
                    float const raw = static_cast<float>(static_cast<std::int8_t>(base[at]));
                    out[c] = signed_normalized ? std::max(raw / 127.0f, -1.0f) : raw;
                    break;
                }
                case deren::gltf::component_type::unsigned_short_t: {
                    auto const* const p = reinterpret_cast<std::uint16_t const*>(base) + at;
                    out[c] = static_cast<float>(*p) / 65535.0f;
                    break;
                }
                case deren::gltf::component_type::short_t: {
                    auto const* const p = reinterpret_cast<std::int16_t const*>(base) + at;
                    float const raw = static_cast<float>(*p);
                    out[c] = signed_normalized ? std::max(raw / 32767.0f, -1.0f) : raw;
                    break;
                }
                default:
                    // int32_t / uint32_t / double (and unknown) are not a legal vertex-attribute
                    // component under the core spec or KHR_mesh_quantization, so the remaining
                    // elements stay zero rather than being reinterpreted as something else
                    out[c] = 0.0f;
                    break;
                }
            }
            return out;
        };

        // ---- attribute count guard: every attribute's byte length fixes a candidate vertex count,
        //      and the smallest one wins - the count is the byte length over the component size, so
        //      a non-float POSITION or a short NORMAL/UV/JOINTS/WEIGHTS accessor cannot make the
        //      read paths below run past its end. Clamp to the shortest attribute and log the
        //      mismatch (error-tolerant load). ----
        auto const portion_elements = [](deren::gltf::vertex_portion const& portion, std::size_t const vec_size) -> std::size_t {
            std::size_t component_bytes = 4; // float / int32_t / uint32_t
            switch (portion.component) {
            case deren::gltf::component_type::unsigned_byte_t:
                component_bytes = 1;
                break;
            case deren::gltf::component_type::short_t:
            case deren::gltf::component_type::unsigned_short_t:
                component_bytes = 2;
                break;
            case deren::gltf::component_type::double_t:
                component_bytes = 8;
                break;
            default:
                break;
            }
            return portion.data.size() / (component_bytes * vec_size);
        };
        std::size_t const position_count = portion_elements(*position_portion, 3);
        std::size_t vertex_count = position_count;
        auto const guard_vertex_count = [&vertex_count, &portion_elements](deren::gltf::vertex_portion const* portion, std::size_t const vec_size) {
            if (portion != nullptr) {
                vertex_count = std::min(vertex_count, portion_elements(*portion, vec_size));
            }
        };
        guard_vertex_count(normal_portion, 3);
        guard_vertex_count(uv_portion, 2);
        guard_vertex_count(joints_portion, 4);
        guard_vertex_count(weights_portion, 4);
        if (vertex_count != position_count) {
            deren::utility::log("gltf: attribute count mismatch (POSITION has {} vertices, another attribute only {}) - rendering the shorter prefix", position_count, vertex_count);
        }

        std::vector<glm::vec3> positions;
        std::vector<glm::vec2> uvs;
        positions.reserve(vertex_count);
        uvs.reserve(vertex_count);
        for (size_t i = 0; i < vertex_count; ++i) {
            // component-type aware: POSITION may be quantized (KHR_mesh_quantization), and TEXCOORD_0
            // may be normalized u8/u16 in the CORE spec - see read_attribute. N is the accessor's own
            // component count, so each call strides by exactly its own element size (asking for 3
            // components of a vec2 UV walks into the next vertex).
            std::array<float, 3> const p = read_attribute.operator()<3>(*position_portion, i, true);
            positions.emplace_back(p[0], p[1], p[2]);
            if (uv_portion == nullptr) {
                uvs.push_back(default_uv);
            } else {
                std::array<float, 2> const uv = read_attribute.operator()<2>(*uv_portion, i, false);
                uvs.emplace_back(uv[0], uv[1]);
            }
        }

        // Index data: 2 or 4 bytes per index (u8 indices are widened to u16 below). glTF
        // primitives may omit "indices" entirely (non-indexed triangle soup, e.g. the Fox
        // sample) — synthesize a sequential uint32 index buffer [0, vertex_count) so the rest
        // of the pipeline can stay indexed-only.
        std::vector<uint8_t> synthesized_indices;
        std::vector<uint8_t> widened_indices; // u8 -> u16 widening result (empty unless used)
        uint8_t const index_width = [&] {
            if (prim.index.empty()) {
                if (vertex_count > 0) {
                    synthesized_indices.resize(vertex_count * sizeof(uint32_t));
                    auto* const dst = reinterpret_cast<uint32_t*>(synthesized_indices.data());
                    for (std::size_t i = 0; i < vertex_count; ++i) {
                        dst[i] = static_cast<uint32_t>(i);
                    }
                }
                return static_cast<uint8_t>(4);
            }
            if (prim.index_component_type == deren::gltf::component_type::unsigned_int_t) {
                return static_cast<uint8_t>(4);
            }
            if (prim.index_component_type == deren::gltf::component_type::unsigned_short_t) {
                return static_cast<uint8_t>(2);
            }
            if (prim.index_component_type == deren::gltf::component_type::unsigned_byte_t) {
                // u8 indices are legal glTF (componentType 5121; at most 256 vertices): widen to u16
                widened_indices.resize(prim.index.size() * sizeof(uint16_t));
                auto* const dst = reinterpret_cast<uint16_t*>(widened_indices.data());
                for (std::size_t i = 0; i < prim.index.size(); ++i) {
                    dst[i] = static_cast<uint16_t>(prim.index[i]);
                }
                return static_cast<uint8_t>(2);
            }
            deren::utility::panic(std::source_location::current(), "unsupported index component type: {}", static_cast<int32_t>(prim.index_component_type));
        }();
        std::vector<uint8_t> const& index_bytes = !widened_indices.empty()
                                                      ? widened_indices
                                                      : (prim.index.empty() ? synthesized_indices : prim.index);
        uint32_t const index_count = static_cast<uint32_t>(index_bytes.size() / index_width);

        // ---- Normals: use the authored NORMAL attribute when the primitive has one; otherwise
        //      GENERATE them from the triangle connectivity. A hard-coded fallback direction
        //      (the old +Y) shades every face as if it pointed up, which turns whole regions
        //      dark the moment the geometry faces sideways - extreme on normal-less skinned
        //      meshes like RecursiveSkeletons' boards (their visible faces got normals pointing
        //      away from the camera/light). Generating smooth per-vertex normals is what the
        //      glTF spec expects viewers to do when the attribute is missing.
        std::vector<glm::vec3> normals;
        normals.resize(vertex_count);
        if (normal_portion != nullptr) {
            for (size_t i = 0; i < vertex_count; ++i) {
                // signed_normalized: a quantized NORMAL is a signed normalized integer whose full
                // range is [-1, 1] (remapping it as [0, 1] would point every normal into one octant)
                std::array<float, 3> const n = read_attribute.operator()<3>(*normal_portion, i, true);
                normals[i] = glm::vec3(n[0], n[1], n[2]);
            }
        } else {
            // accumulate area-weighted face normals per vertex over the triangle list (glTF
            // front faces are counter-clockwise, so cross(e1, e2) points outward)
            auto const index_at = [index_bytes, index_width](size_t const k) -> uint32_t {
                size_t const off = k * index_width;
                if (index_width == 4) {
                    return *reinterpret_cast<uint32_t const*>(index_bytes.data() + off);
                }
                // u8 indices were widened to u16 above, so 2 is the smallest width here
                return *reinterpret_cast<uint16_t const*>(index_bytes.data() + off);
            };
            for (size_t t = 0; t + 2 < index_count; t += 3) {
                uint32_t const a = index_at(t);
                uint32_t const b = index_at(t + 1);
                uint32_t const c = index_at(t + 2);
                if (a >= vertex_count || b >= vertex_count || c >= vertex_count) {
                    continue; // malformed index list: skip the triangle
                }
                glm::vec3 const e1 = positions[b] - positions[a];
                glm::vec3 const e2 = positions[c] - positions[a];
                glm::vec3 const face = glm::cross(e1, e2);
                if (glm::dot(face, face) > 0.0f) { // skip degenerate (zero-area) triangles
                    normals[a] += face;
                    normals[b] += face;
                    normals[c] += face;
                }
            }
            for (glm::vec3& n : normals) {
                float const len2 = glm::dot(n, n);
                // a vertex that only touched degenerate triangles keeps a sane up normal
                n = len2 > 0.0f ? n / std::sqrt(len2) : glm::vec3(0.0f, 1.0f, 0.0f);
            }
        }

        // skinned attributes decoded per vertex (portions declared above, default semantics
        // keep non-skinned meshes correct under the shared skinned vertex layout)
        auto const read_joints = [joints_portion](size_t const i) -> glm::uvec4 {
            glm::uvec4 out(0u);
            if (joints_portion == nullptr) {
                return out;
            }
            if (joints_portion->component == deren::gltf::component_type::unsigned_byte_t) {
                for (int32_t c = 0; c < 4; ++c) {
                    out[c] = joints_portion->data[i * 4 + static_cast<std::size_t>(c)];
                }
            } else if (joints_portion->component == deren::gltf::component_type::unsigned_short_t) {
                auto const* p = reinterpret_cast<std::uint16_t const*>(joints_portion->data.data());
                for (int32_t c = 0; c < 4; ++c) {
                    out[c] = p[i * 4 + static_cast<std::size_t>(c)];
                }
            }
            return out;
        };
        auto const read_weights = [weights_portion](size_t const i) -> glm::vec4 {
            if (weights_portion == nullptr) {
                return glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
            }
            if (weights_portion->component == deren::gltf::component_type::float_t) {
                auto const* p = reinterpret_cast<float const*>(weights_portion->data.data());
                return glm::vec4(p[i * 4 + 0], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
            }
            if (weights_portion->component == deren::gltf::component_type::unsigned_byte_t) { // normalized
                return glm::vec4(static_cast<float>(weights_portion->data[i * 4 + 0]) / 255.0f,
                                 static_cast<float>(weights_portion->data[i * 4 + 1]) / 255.0f,
                                 static_cast<float>(weights_portion->data[i * 4 + 2]) / 255.0f,
                                 static_cast<float>(weights_portion->data[i * 4 + 3]) / 255.0f);
            }
            if (weights_portion->component == deren::gltf::component_type::unsigned_short_t) { // normalized
                auto const* p = reinterpret_cast<std::uint16_t const*>(weights_portion->data.data());
                return glm::vec4(static_cast<float>(p[i * 4 + 0]) / 65535.0f,
                                 static_cast<float>(p[i * 4 + 1]) / 65535.0f,
                                 static_cast<float>(p[i * 4 + 2]) / 65535.0f,
                                 static_cast<float>(p[i * 4 + 3]) / 65535.0f);
            }
            return glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        };

        // interleave into the single-binding layout the pbr pipeline expects (stride 64)
        result.vertices.reserve(vertex_count);
        for (size_t i = 0; i < vertex_count; ++i) {
            result.vertices.push_back(vertex{.position = positions[i], .normal = normals[i], .uv = uvs[i], .joints = read_joints(i), .weights = read_weights(i)});
        }
        if (prim.index.empty()) {
            result.index_data = std::move(synthesized_indices); // non-indexed -> synthesized
        } else {
            // The WIDENED bytes, not the accessor's own ones. index_width and index_count above were
            // both derived from index_bytes, so uploading prim.index instead binds a buffer that
            // disagrees with the width the draw declares: for a u8 accessor (componentType 5121,
            // legal glTF, used by every small test asset with <= 256 vertices) the buffer is half the
            // size the UINT16 index type needs - validation reports "index size (2) * (...) is greater
            // than the index buffer size", and the draw reads the indices at the wrong stride, so the
            // triangles come out wrong as well as out of bounds.
            result.index_data = index_bytes;
        }
        result.index_width = index_width;
        result.index_count = index_count;
        return result;
    }

    // Convert stb-decoded texture data to RGBA (3 channels get alpha, 1 channel is gray-scaled)
    std::vector<uint8_t> to_rgba(deren::gltf::texture_data const& texture) {
        size_t const pixel_count = static_cast<size_t>(texture.width) * texture.height;
        std::vector<uint8_t> rgba(pixel_count * 4, 255);
        switch (texture.component) {
        case 4:
            rgba = texture.data;
            break;
        case 3:
            for (size_t i = 0; i < pixel_count; ++i) {
                rgba[i * 4 + 0] = texture.data[i * 3 + 0];
                rgba[i * 4 + 1] = texture.data[i * 3 + 1];
                rgba[i * 4 + 2] = texture.data[i * 3 + 2];
            }
            break;
        case 1:
            for (size_t i = 0; i < pixel_count; ++i) {
                rgba[i * 4 + 0] = texture.data[i];
                rgba[i * 4 + 1] = texture.data[i];
                rgba[i * 4 + 2] = texture.data[i];
            }
            break;
        default:
            rgba.clear();
            break;
        }
        return rgba;
    }

    // 8-bit sRGB <-> linear conversion: color textures (slot 0) are averaged in linear space so
    // their mips keep correct brightness; the other (UNORM data) slots are averaged in byte space.
    float srgb_to_linear(uint8_t const c) {
        static std::array<float, 256> const table = [] {
            std::array<float, 256> t = {};
            for (int32_t i = 0; i < 256; ++i) {
                float const v = static_cast<float>(i) / 255.0f;
                t[i] = v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
            }
            return t;
        }();
        return table[c];
    }

    uint8_t linear_to_srgb(float const v) {
        static std::array<uint8_t, 4096> const table = [] {
            std::array<uint8_t, 4096> t = {};
            for (int32_t i = 0; i < 4096; ++i) {
                float const v = static_cast<float>(i) / 4095.0f;
                float const s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                t[i] = static_cast<uint8_t>(std::clamp(std::lround(s * 255.0f), 0L, 255L));
            }
            return t;
        }();
        int32_t const idx = std::clamp(static_cast<int32_t>(std::lround(v * 4095.0f)), 0, 4095);
        return table[idx];
    }

    // A full RGBA8 mip chain: mip0, mip1, ... laid out contiguously (mip-major). The level
    // count is floor(log2(min(width, height))) + 1.
    struct mip_chain {
        std::vector<uint8_t> data = {};
        uint32_t mip_levels = 0;
    };

    mip_chain generate_mip_chain(std::span<uint8_t const> const rgba, uint32_t const width, uint32_t const height, bool const srgb) {
        uint32_t const mip_count = static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(std::min(width, height))))) + 1;
        mip_chain result;
        result.mip_levels = mip_count;
        result.data.reserve(static_cast<size_t>(width) * height * 4 * 4 / 3); // geometric series for power-of-two

        std::vector<uint8_t> a(rgba.begin(), rgba.end());
        std::vector<uint8_t> b;
        std::span<uint8_t const> cur = a;
        uint32_t w = width;
        uint32_t h = height;
        for (uint32_t mip = 0; mip < mip_count; ++mip) {
            result.data.insert(result.data.end(), cur.begin(), cur.end());
            if (w == 1 && h == 1) {
                break;
            }
            uint32_t const nw = std::max(1u, w / 2);
            uint32_t const nh = std::max(1u, h / 2);
            b.assign(static_cast<size_t>(nw) * nh * 4, 0);
            for (uint32_t y = 0; y < nh; ++y) {
                uint32_t const sy0 = std::min(y * 2, h - 1);
                uint32_t const sy1 = std::min(y * 2 + 1, h - 1);
                for (uint32_t x = 0; x < nw; ++x) {
                    uint32_t const sx0 = std::min(x * 2, w - 1);
                    uint32_t const sx1 = std::min(x * 2 + 1, w - 1);
                    size_t const p00 = (static_cast<size_t>(sy0) * w + sx0) * 4;
                    size_t const p10 = (static_cast<size_t>(sy0) * w + sx1) * 4;
                    size_t const p01 = (static_cast<size_t>(sy1) * w + sx0) * 4;
                    size_t const p11 = (static_cast<size_t>(sy1) * w + sx1) * 4;
                    size_t const dst = (static_cast<size_t>(y) * nw + x) * 4;
                    for (int32_t c = 0; c < 4; ++c) {
                        if (srgb && c < 3) {
                            float const l = (srgb_to_linear(cur[p00 + c]) + srgb_to_linear(cur[p10 + c]) + srgb_to_linear(cur[p01 + c]) + srgb_to_linear(cur[p11 + c])) * 0.25f;
                            b[dst + c] = linear_to_srgb(l);
                        } else {
                            b[dst + c] = static_cast<uint8_t>((static_cast<uint32_t>(cur[p00 + c]) + cur[p10 + c] + cur[p01 + c] + cur[p11 + c] + 2) / 4);
                        }
                    }
                }
            }
            std::swap(a, b);
            cur = a;
            w = nw;
            h = nh;
        }
        return result;
    }
} // namespace

namespace deren::gltf {
    head_basis head_basis_fallback() noexcept {
        // THE REFERENCE'S OWN CONSTANTS, verbatim from `EfFaceGetHeadBasis`'s `valid < 0.5` branch: it
        // substitutes exactly these three vectors when the head bone is missing or degenerate. Reproduced
        // rather than invented, because a model with NO SKELETON is the case this repository actually has.
        return head_basis{};
    }

    head_basis head_basis_from_axes(glm::vec3 const forward_axis, glm::vec3 const right_axis) noexcept {
        head_basis out = head_basis_fallback();
        float const forward_len_sq = glm::dot(forward_axis, forward_axis);
        float const right_len_sq = glm::dot(right_axis, right_axis);
        // THE DEGENERATE CASE IS THE REFERENCE'S TOO (`forwardLengthSq > 1e-8 && rightLengthSq > 1e-8`), and it
        // is not hypothetical: a bone that has not been posed arrives as a zero matrix, and normalising that
        // yields NaNs - which reach the shader as a face that is black or flickering rather than as an error.
        if (forward_len_sq <= 1e-8f || right_len_sq <= 1e-8f) {
            return out;
        }
        // THE FRAME POINTS WHERE THE FACE LOOKS - the note on `head_basis` in the interface unit carries the
        // measurement that settled it. `forward_axis` is the head bone's `row3` and on every model here it IS
        // the direction the face looks, so it is taken AS IT IS; `right_axis` is `row1` and IS negated, because
        // glTF's right for a `+Z`-facing asset is `-X` while the bone's `row1` is `+X`.
        out.front = glm::normalize(forward_axis);
        out.right = -glm::normalize(right_axis);
        // `up = right x front`, which is the same handedness the fallback's three constants already have
        // (`-X x +Z = +Y`). Taking the cross the other way round yields an up pointing at the floor - and the
        // shader only ever uses this as the plane normal it projects the light onto, so a flipped up changes no
        // pixel - but a frame whose axes disagree with the fallback's is a trap for whoever reads this next.
        glm::vec3 const up_axis = glm::cross(out.right, out.front);
        if (glm::dot(up_axis, up_axis) < 1e-8f) {
            // THE TWO AXES ARE PARALLEL, so there is no frame to build: the reference sets `valid = 0` and
            // keeps its default up, and this returns the whole fallback rather than half of a frame.
            return out;
        }
        out.up = glm::normalize(up_axis);
        // THE RE-ORTHOGONALISATION IS THE REFERENCE'S LAST STEP and it matters: a bone's forward and right rows
        // are only orthogonal if whoever rigged it made them so, and the SDF's angle is taken between the light
        // and a FRAME - three axes that are nearly right are not three axes. `front x up` is the same handedness
        // as the `up` above (`+Z x +Y = -X`).
        out.right = glm::normalize(glm::cross(out.front, out.up));
        out.from_skeleton = true;
        return out;
    }

    bool looks_like_head_joint(std::string_view const node_name) noexcept {
        if (node_name.empty()) {
            return false;
        }
        // THE CJK NAMES FIRST, because they have no separators to split on: 頭 / 头 ARE the head, and a name
        // like `頭_01` yields no useful ASCII token.
        if (node_name.find("頭") != std::string_view::npos || node_name.find("头") != std::string_view::npos) {
            return true;
        }
        // TOKEN EQUALITY, NOT A SUBSTRING, and that is the substance of this function: models name bones
        // `head`, `Head`, `Bip01 Head`, `J_Head`, `Head_Nub` - and a substring test over `head` also matches
        // `headgear`, `overhead` and `Forehead`, which are exactly the props and accessories that share a
        // character's skeleton. So the name is split on non-alphanumerics and a token has to BE `head`.
        std::size_t i = 0;
        while (i < node_name.size()) {
            while (i < node_name.size() && std::isalnum(static_cast<uint8_t>(node_name[i])) == 0) {
                ++i;
            }
            std::size_t const start = i;
            while (i < node_name.size() && std::isalnum(static_cast<uint8_t>(node_name[i])) != 0) {
                ++i;
            }
            if (i == start) {
                continue;
            }
            std::string token;
            token.reserve(i - start);
            for (std::size_t k = start; k < i; ++k) {
                token.push_back(static_cast<char>(std::tolower(static_cast<uint8_t>(node_name[k]))));
            }
            if (token == "head") {
                return true;
            }
        }
        return false;
    }

    std::optional<std::size_t> head_joint_of(scenes const& scene, std::size_t const scene_index, std::size_t const skin_index) noexcept {
        if (skin_index >= scene.skins.size() || scene_index >= scene.scene.size()) {
            return std::nullopt;
        }
        skin const& skin_object = scene.skins[skin_index];
        std::vector<node> const& nodes = scene.scene[scene_index].nodes;
        // `skin::joints` HOLDS ASSET NODE INDICES, not positions in the scene's pool, so each one is resolved
        // through `node::source_index` - the same indirection every other asset index in this loader uses. The
        // RETURNED value is the JOINT index, because that is what picks a matrix out of the per-frame skin
        // matrix array; the two are different numbers and confusing them would give a face shaded by its foot.
        for (std::size_t joint = 0; joint < skin_object.joints.size(); ++joint) {
            for (node const& candidate : nodes) {
                if (candidate.source_index == skin_object.joints[joint]) {
                    if (looks_like_head_joint(candidate.name)) {
                        return joint;
                    }
                    break;
                }
            }
        }
        return std::nullopt;
    }

    toon_family toon_family_of(std::string_view const name) {
        if (name.empty()) {
            return toon_family::none;
        }
        // LOWERCASED because a glTF material name is authored text: the character models this was written
        // against use `M_actor_zhuangfy_body_01` (mixed case), and the references' own classifiers lowercase
        // before matching. ASCII only, deliberately - the CJK patterns below are matched byte-wise, and a
        // Unicode case fold would need a table for no gain.
        std::string lowered;
        lowered.reserve(name.size());
        for (char const c : name) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<uint8_t>(c))));
        }
        auto const has = [&lowered](std::string_view const needle) { return lowered.find(needle) != std::string::npos; };
        // ---- THE TWO OVERLAY MATERIALS ARE NOT TOON MATERIALS, AND THAT COMES BEFORE EVERY GROUP BELOW ----
        //
        // `M_eyeshadow_common_01` and `M_hairshadow_common_01` are the masks the game multiplies over an already
        // shaded character, and the ARTICLE DRAWS THEM IN THEIR OWN PASS rather than as a family: its
        // `MyZmdEyeDarkShader` and `MyZmdHairShadowShader` are framebuffer multiplies (`BlendOp` / `Blend` /
        // `ZWrite 0`, plus `Stencil { Ref 1 Comp Equal }` on the hair shadow) carrying only `_Color`, `_Alpha`,
        // `_DayStrength` and a mask texture - there is no ramp, no layer and no specular in either of them.
        //
        // WHY THIS IS HERE AND NOT LEFT TO THE HAIR GROUP, which used to claim the hair shadow: the reference's own
        // classifier calls a `hairshadow` material HAIR, and that is where this table's hair entry came from - but a
        // family is "how this port SHADES the material", and shading an overlay quad with the strand model of the
        // hair it exists to darken is a stand-in from before the overlay pass existed here.
        //
        // HOW MUCH THAT COSTS, MEASURED rather than asserted: with the two overlay materials renamed so that
        // neither classifier claims them (the same geometry drawn as ordinary surfaces - `make_overlay_control.py`,
        // `chars\chen_full2_control.glb`), the frame differs from this port's by 0.06% at the standard pose and
        // 0.81% at the face close-up. THIS COMMENT USED TO QUOTE 8.22% AND 68.92% for the same comparison, and
        // those numbers cannot have come from the asset they name: `chen_full2.glb` had its two overlay nodes
        // outside `scenes[0].nodes` at the time, so a scene-graph loader imported eight primitives out of ten and
        // the difference could only have been zero. The smaller figures above are the ones measured against a
        // file whose overlays are actually reachable, and the hair-shadow argument does not depend on their size:
        // what makes the quad wrong is being shaded at all, not how much of the frame it covers.
        if (has("eyeshadow") || has("hairshadow")) {
            // ... AND THE SAME NAME IS CLAIMED BY `overlay_kind_of`, which is where the two are actually
            // resolved into the channel the overlay pass draws (see `overlay_kind`). The two functions are one
            // decision read from two sides: this one says "no family shades it", that one says "the overlay pass
            // multiplies by its mask", and a name added to only one of them is a material that is neither shaded
            // nor overlaid - which is a surface that silently disappears rather than a wrong picture.
            return toon_family::none;
        }
        // PRIORITY ORDER IS THE SUBSTANCE OF THIS FUNCTION - see the declaration's note in the interface
        // unit. Each group is written as the patterns the reference's classifier uses for the same family,
        // and the ones that could collide with a later group come first.
        //
        // THE CJK PATTERNS ARE MATCHED AS BYTES, SO THE SAME WORD IN ANOTHER SCRIPT IS A DIFFERENT WORD
        // HERE - and that is not hypothetical. `顔` (U+9854, Japanese) and `颜` (U+989C, simplified
        // Chinese) are one word and two byte sequences, so a table that listed only the first classified
        // NONE of the MMD models' face materials; the same held for `髪`/`发` (hair), `靴`/`鞋` (shoe) and
        // `裤` (trousers, with no Japanese entry at all). Measured on `zhuangfy_toon.glb` before this was
        // fixed: 25 of its 35 materials fell through to `none`, including every `face` and every `hair`
        // material - which silently costs the hair its highlight and the eye its soft ramp, because a
        // material that matches nothing gets the family-independent path. Both scripts are therefore
        // listed side by side, and the SIMPLIFIED forms are the ones these models actually use.
        //
        // AND THE TOKENS ARE OFTEN A SINGLE CHARACTER. An MMD model names its materials `颜`, `发`, `目`,
        // `眉`, `睫`, `口`, `齿`, `舌`, `鼻`, `鞋`, `裤` - not the two-character words (`頭髮`, `眉毛`,
        // `睫毛`, `口內`) the first version of this table was written against, so the bare characters are
        // matched too. Each bare character below is one a model in this repository actually uses as a
        // WHOLE material name, and the order absorbs the collisions that creates: `手套` (glove) contains
        // `手` and `袖口` (cuff) and `领口` (collar) contain `口`, so cloth is tested BEFORE skin and face
        // and carries `手套`/`袖`/`领` of its own; `发饰` (hair ornament) contains `发` and is claimed by
        // hair, which is the reference's own call - it classifies its `hairshadow` material as hair.
        //
        // eye BEFORE face: `眼白`/`目白`/`sclera` and `高光`/`highlight` are eye layers.
        if (has("虹膜") || has("瞳") || has("眼白") || has("目白") || has("高光") || has("iris") || has("sclera") ||
            has("eyewhite") || has("eyehl") || has("eye_hl") || has("catchlight") || has("eyebase") ||
            // the bare characters: `目` is the whole name of an MMD eye material, and `目`/`眼` are the
            // prefixes of every layer around it (`目白` sclera, `目影` eye shadow, `目HL` highlight)
            has("目") || has("眼")) {
            return toon_family::eye;
        }
        // hair BEFORE skin and face: `髪`/`发` are unambiguous, but this comes early because the reference's
        // hair materials also carry `頭` (head), which a face pattern must not claim.
        if (has("髪") || has("髮") || has("发") || has("头发") || has("頭髪") || has("hair")) {
            return toon_family::hair;
        }
        // cloth BEFORE skin: the two sets never overlap, and this order is the reference's.
        if (has("衣") || has("布") || has("服") || has("裙") || has("靴") || has("鞋") || has("裤") || has("褲") ||
            has("袜") || has("襪") || has("帽") || has("手套") || has("袖") || has("领") || has("領") ||
            has("cloth") || has("coat") || has("dress") || has("skirt") || has("shoe") || has("boot")) {
            return toon_family::cloth;
        }
        // skin: `body` is here because the character models name the skin material `..._body_01`, which is
        // the reference's own mapping (its `MaterialRole.Skin` classifier matches `body` too).
        if (has("皮肤") || has("皮膚") || has("肌") || has("手") || has("脚") || has("腳") || has("足") ||
            has("腿") || has("臂") || has("耳") || has("指") || has("skin") || has("body")) {
            return toon_family::skin;
        }
        // face LAST of the named families: `面`/`脸`/`臉`/`顔`/`颜` and `face` are short tokens that appear
        // inside other words, so everything more specific above gets its chance first. The lash/brow/mouth
        // layers belong to the face group and are matched here.
        if (has("面") || has("脸") || has("臉") || has("顔") || has("颜") || has("face") || has("brow") ||
            has("lash") || has("mouth") || has("teeth") || has("tongue") || has("睫毛") || has("眉毛") ||
            has("口内") || has("口內") ||
            // the bare characters, which is how an MMD model names the face and every line drawn on it:
            // `颜`/`脸` the face, then `眉` brow, `睫` lash, `口`/`口线` mouth and its line, `齿` teeth,
            // `舌` tongue, `鼻`/`鼻线` nose and its line, `唇` lips, `二重` the double-eyelid line, `表情`
            // the expression overlay that draws blush and tears over the same geometry. Cloth is what keeps
            // these unambiguous: `袖口`/`领口` are claimed above, so `口` here can only be a mouth.
            has("眉") || has("睫") || has("口") || has("齿") || has("齒") || has("舌") || has("鼻") ||
            has("唇") || has("二重") || has("表情")) {
            return toon_family::face;
        }
        return toon_family::none;
    }

    overlay_kind overlay_kind_of(std::string_view const name) {
        if (name.empty()) {
            return overlay_kind::none;
        }
        // THE SAME LOWERCASING `toon_family_of` DOES, and for the same reason: a glTF material name is authored
        // text (`M_eyeshadow_common_01` is lowercased here, `M_S_actor_zhuangfy_EyeShadow_01_lod0` would not be).
        // ASCII only, deliberately - see that function's note.
        std::string lowered;
        lowered.reserve(name.size());
        for (char const c : name) {
            lowered.push_back(static_cast<char>(std::tolower(static_cast<uint8_t>(c))));
        }
        // `hairshadow` FIRST, because `eyeshadow` is not a substring of it and vice versa - they are disjoint, so
        // the order is only a reading convenience rather than the priority rule `toon_family_of` needs. What the
        // order does NOT have to absorb is the near miss both share: `shadow` alone claims nothing, so a material
        // called `M_shadow_decal_01` stays an ordinary surface instead of silently becoming an overlay.
        if (lowered.find("hairshadow") != std::string::npos) {
            return overlay_kind::hair_shadow;
        }
        if (lowered.find("eyeshadow") != std::string::npos) {
            return overlay_kind::eye_dark;
        }
        return overlay_kind::none;
    }

    std::optional<uint16_t> scenes::texture_index_by_name(std::string_view const name) const noexcept {
        if (name.empty()) {
            return std::nullopt; // an unnamed texture must never match an empty query: most files are all-unnamed
        }
        for (std::size_t i = 0; i < this->textures.size(); ++i) {
            if (this->textures[i].name == name) {
                return static_cast<uint16_t>(i);
            }
        }
        return std::nullopt;
    }

    material const* scenes::material_by_name(std::string_view const name) const noexcept {
        if (name.empty()) {
            return nullptr; // an unnamed material must never match an empty query, exactly as for a texture above
        }
        for (material const& candidate : this->materials) {
            if (candidate.name == name) {
                return &candidate;
            }
        }
        return nullptr;
    }

    std::expected<scenes, error_code> load_model(std::string_view file_name) {
        std::filesystem::path const path(file_name);
        if (!std::filesystem::is_regular_file(path)) {
            return std::unexpected(error_code::file_not_found);
        }

        auto buffer_exp = fastgltf::GltfDataBuffer::FromPath(path);
        if (!buffer_exp) {
            deren::utility::error("gltf load err: failed to read file: {}", fastgltf::getErrorMessage(buffer_exp.error()));
            return std::unexpected(error_code::file_load_failed);
        }

        // Parser: enable KHR_lights_punctual so asset.lights / node.lightIndex are populated
        // (cameras are part of the default categories). Files without the extension parse
        // identically to before.
        //
        // ... AND INSTALL THE `extras` COLLECTOR, which is the only route to a material's extras block in this
        // fastgltf version (see `collect_material_extras`). It is installed for EVERY load and is inert on a
        // file with no such block, so "this model is not a character" and "this character has no extras" are
        // the same code path rather than two.
        material_extras extras = {};
        fastgltf::Parser parser(fastgltf::Extensions::KHR_lights_punctual);
        parser.setUserPointer(&extras);
        parser.setExtrasParseCallback(&collect_material_extras);
        auto asset_exp = parser.loadGltf(buffer_exp.get(), path.parent_path(), load_options());
        if (!asset_exp) {
            deren::utility::error("gltf load err: {}", fastgltf::getErrorMessage(asset_exp.error()));
            return std::unexpected(to_error_code(asset_exp.error()));
        }

        Asset asset = std::move(asset_exp.get());

        // Strict structural validation (node/mesh/skin/camera/accessor index bounds, extension
        // consistency, sampler rules). Parser::loadGltf does NOT run this itself - without it a
        // malformed file could drive out-of-bounds reads in the index-based loaders below
        // (load_scene / load_animation / load_skin / load_primitive dereference raw asset
        // indices). Cost is one O(n) pass over the parsed asset at load time.
        if (fastgltf::Error const validation_error = fastgltf::validate(asset); validation_error != fastgltf::Error::None) {
            deren::utility::error("gltf validate err [validate]: {}", fastgltf::getErrorMessage(validation_error));
            return std::unexpected(to_error_code(validation_error));
        }

        scenes result;
        result.scene.reserve(asset.scenes.size());
        for (std::size_t i = 0; i < asset.scenes.size(); ++i) {
            result.scene.push_back(load_scene(asset, i));
        }
        // asset-level node lookup (see scenes::node_by_source): first copy per source_index
        // wins; a node referenced from several scenes is stored once per scene pool, and all
        // copies carry the same metadata, so the first found is a fine representative.
        for (deren::gltf::scene const& loader_scene : result.scene) {
            for (deren::gltf::node const& loader_node : loader_scene.nodes) {
                result.node_by_source.try_emplace(loader_node.source_index, &loader_node);
            }
        }

        result.animations.reserve(asset.animations.size());
        for (std::size_t i = 0; i < asset.animations.size(); ++i) {
            result.animations.push_back(load_animation(asset, i));
        }

        result.skins.reserve(asset.skins.size());
        for (std::size_t i = 0; i < asset.skins.size(); ++i) {
            result.skins.push_back(load_skin(asset, i));
        }

        result.cameras.reserve(asset.cameras.size());
        for (fastgltf::Camera const& f_camera : asset.cameras) {
            result.cameras.push_back(load_camera(f_camera));
        }

        result.lights.reserve(asset.lights.size());
        for (fastgltf::Light const& f_light : asset.lights) {
            result.lights.push_back(load_light(f_light));
        }

        result.textures.reserve(asset.textures.size());
        for (std::size_t i = 0; i < asset.textures.size(); ++i) {
            result.textures.push_back(load_texture(asset, i));
        }

        result.materials.reserve(asset.materials.size());
        for (std::size_t i = 0; i < asset.materials.size(); ++i) {
            // The collector is indexed the way `asset.materials` is (see `collect_material_extras`); a material
            // the callback never saw - impossible today, and cheap to keep honest - states no claimed extras.
            result.materials.push_back(load_material(asset.materials[i], i < extras.floats.size() ? &extras.floats[i] : nullptr));
        }

        return result;
    }

    // ---- deren::gltf::scenes iteration: flatten scene -> node -> mesh -> primitive ----

    scene_iterator::scene_iterator(scenes const& owner)
        : iterating_scene(&owner)
        , exhausted(false) {
        this->advance();
    }

    scene_iterator::reference scene_iterator::operator*() const noexcept {
        auto const& owner = *this->iterating_scene;
        auto const& prim = owner.scene[this->scene_i].nodes[this->node_i].meshes[this->mesh_i].primitives[this->prim_i];
        this->current.primitive = &prim;
        this->current.transform_matrix = owner.scene[this->scene_i].nodes[this->node_i].transform_matrix;
        return this->current;
    }

    scene_iterator::pointer scene_iterator::operator->() const noexcept {
        return &this->current;
    }

    scene_iterator& scene_iterator::operator++() {
        ++this->prim_i; // step past the current primitive
        this->advance();
        return *this;
    }

    void scene_iterator::operator++(int32_t) {
        ++*this;
    }

    void scene_iterator::advance() {
        if (this->iterating_scene == nullptr || this->exhausted) {
            this->exhausted = true;
            return;
        }
        auto const& owner = *this->iterating_scene;
        while (this->scene_i < owner.scene.size()) {
            auto const& s = owner.scene[this->scene_i];
            if (this->node_i >= s.nodes.size()) {
                ++this->scene_i;
                this->node_i = this->mesh_i = this->prim_i = 0;
                continue;
            }
            auto const& node = s.nodes[this->node_i];
            if (this->mesh_i >= node.meshes.size()) {
                ++this->node_i;
                this->mesh_i = this->prim_i = 0;
                continue;
            }
            auto const& mesh = node.meshes[this->mesh_i];
            if (this->prim_i >= mesh.primitives.size()) {
                ++this->mesh_i;
                this->prim_i = 0;
                continue;
            }
            return; // (scene_i, node_i, mesh_i, prim_i) is a valid drawable primitive
        }
        this->exhausted = true;
    }

    scene_iterator scenes::begin() const {
        return scene_iterator(*this);
    }

    scene_iterator scenes::end() noexcept {
        return scene_iterator();
    }

    // ---- deren::gltf::scenes node-tree iteration (structural view, includes transform-only nodes) ----

    void scene_node_iterator::push_next_root() {
        while (this->scene_i < this->iterating_scene->scene.size()) {
            deren::gltf::scene const& sc = this->iterating_scene->scene[this->scene_i];
            if (this->root_i < sc.root_indices.size()) {
                this->stack.push_back({sc.root_indices[this->root_i++], 0});
                return;
            }
            ++this->scene_i;
            this->root_i = 0;
        }
        this->exhausted = true;
    }

    void scene_node_iterator::descend() {
        while (!this->exhausted) {
            if (this->stack.empty()) {
                this->push_next_root();
                if (this->exhausted) {
                    return;
                }
                return; // visiting the fresh root now
            }
            frame& top = this->stack.back();
            deren::gltf::node const& node = this->iterating_scene->scene[this->scene_i].nodes[top.node_i];
            if (top.next_child < node.children.size()) {
                std::size_t const child = node.children[top.next_child++];
                this->stack.push_back({child, 0});
                return; // visiting the child now
            }
            this->stack.pop_back(); // this node's subtree done; retry its parent / next root
        }
    }

    scene_node_iterator::scene_node_iterator(scenes const& owner)
        : iterating_scene(&owner)
        , exhausted(false) {
        this->push_next_root(); // land on the first root of the first scene (or exhaust)
    }

    scene_node_iterator::reference scene_node_iterator::operator*() const noexcept {
        return &this->iterating_scene->scene[this->scene_i].nodes[this->stack.back().node_i];
    }

    scene_node_iterator::pointer scene_node_iterator::operator->() const noexcept {
        return this->operator*();
    }

    scene_node_iterator& scene_node_iterator::operator++() {
        this->descend();
        return *this;
    }

    void scene_node_iterator::operator++(int32_t) {
        ++*this;
    }

    std::string_view scene_node_iterator::get_name() const noexcept {
        return this->iterating_scene->scene[this->scene_i].nodes[this->stack.back().node_i].name;
    }

    glm::mat4 scene_node_iterator::get_local_transform() const noexcept {
        return this->iterating_scene->scene[this->scene_i].nodes[this->stack.back().node_i].local_transform;
    }

    std::size_t scene_node_iterator::get_depth() const noexcept {
        return this->stack.size() - 1;
    }

    std::size_t scene_node_iterator::get_drawable_count() const noexcept {
        deren::gltf::node const& node = this->iterating_scene->scene[this->scene_i].nodes[this->stack.back().node_i];
        std::size_t count = 0;
        for (deren::gltf::mesh const& mesh : node.meshes) {
            count += mesh.primitives.size();
        }
        return count;
    }

    std::size_t scene_node_iterator::get_source_index() const noexcept {
        return this->iterating_scene->scene[this->scene_i].nodes[this->stack.back().node_i].source_index;
    }

    scene_node_iterator scenes::nodes_begin() const {
        return scene_node_iterator(*this);
    }

    scene_node_iterator scenes::nodes_end() noexcept {
        return scene_node_iterator();
    }

    // ---- resolved materials + renderer-ready drawable iteration (pure CPU) ----

    std::vector<resolved_material> resolve_materials(deren::gltf::scenes const& scenes) {
        constexpr std::array<std::string_view, 5> slot_names = {"albedo", "metallic_roughness", "normal", "occlusion", "emissive"};

        // decoded texture cache: one entry per glTF texture index; shared textures decode once,
        // and the shared_ptr owners keep every image_view's span alive for the caller
        struct decoded_texture {
            std::shared_ptr<std::vector<uint8_t>> data = {};
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t mip_levels = 1;
        };
        std::vector<std::optional<decoded_texture>> cache(scenes.textures.size());

        auto const decode = [&scenes, &cache](uint16_t const texture_index, bool const srgb) -> decoded_texture {
            decoded_texture out = {};
            auto& entry = cache[texture_index];
            if (!entry) {
                entry = decoded_texture{};
                deren::gltf::texture_data const& tex = scenes.textures[texture_index];
                std::vector<uint8_t> const rgba = to_rgba(tex);
                if (!rgba.empty() && tex.width > 0 && tex.height > 0) {
                    mip_chain mips = generate_mip_chain(rgba, tex.width, tex.height, srgb);
                    entry->data = std::make_shared<std::vector<uint8_t>>(std::move(mips.data));
                    entry->width = tex.width;
                    entry->height = tex.height;
                    entry->mip_levels = mips.mip_levels;
                }
            }
            if (entry->data) {
                out = *entry;
            }
            return out;
        };

        std::vector<resolved_material> result;
        result.reserve(scenes.materials.size());
        for (auto const& mat : scenes.materials) {
            resolved_material out = {};
            out.factors.base_color_factor = mat.factors.base_color_factor;
            out.factors.emissive_factor = glm::vec4(mat.factors.emissive_factor, 1.0f);
            out.factors.metallic_factor = mat.factors.metallic_factor;
            out.factors.roughness_factor = mat.factors.roughness_factor;
            out.factors.normal_scale = mat.factors.normal_scale;
            out.factors.occlusion_strength = mat.factors.occlusion_strength;
            out.factors.alpha_cutoff = mat.factors.alpha_cutoff;
            out.factors.alpha_mask = mat.factors.alpha_mask;
            out.factors.alpha_blend = mat.factors.alpha_blend;
            out.double_sided = mat.double_sided;
            // THE FAMILY IS RESOLVED HERE, ONCE. This is the last place the material's NAME exists - the
            // resolved form that everything downstream consumes carries five slots, the factors and the
            // family, and no string. Classifying at the consumer instead would mean either carrying the name
            // through the whole pipeline or re-deriving it, and a per-draw string match is exactly the kind
            // of work that belongs at import.
            out.toon_family = static_cast<uint32_t>(toon_family_of(mat.name));
            // ... AND THE OVERLAY CHANNEL, resolved at the same moment from the same name: a material that
            // classified into one of the article's two framebuffer multiplies is not shaded at all, so this is
            // the fact that takes it out of the shading passes and into the overlay pass. The two are disjoint
            // by construction - `toon_family_of` answers `none` for exactly these names.
            out.overlay_kind = static_cast<uint32_t>(overlay_kind_of(mat.name));
            // ... AND THE NAME ITSELF, because the toon sidecar is keyed by it: a consumer that has only the
            // family cannot look up the entry describing THIS material. See resolved_material::name.
            out.name = mat.name;
            for (int32_t i = 0; i < 5; ++i) {
                auto const it = mat.texture_indices.find(std::string(slot_names[i]));
                if (it == mat.texture_indices.end() || it->second >= scenes.textures.size()) {
                    continue;
                }
                // slot 0 (albedo) is sRGB color: average its mips in linear space
                decoded_texture const decoded = decode(it->second, i == 0);
                if (!decoded.data) {
                    continue;
                }
                image_view& slot = out.slots[i];
                slot.data = *decoded.data;
                slot.width = decoded.width;
                slot.height = decoded.height;
                slot.mip_levels = decoded.mip_levels;
                slot.owner = decoded.data;
                slot.valid = true;
            }
            result.push_back(std::move(out));
        }
        return result;
    }

    drawable_iterator& drawable_iterator::operator++() {
        ++this->inner;
        this->built = false; // geometry of the next drawable is rebuilt lazily
        return *this;
    }

    void drawable_iterator::ensure_built() const {
        if (this->built) {
            return;
        }
        built_mesh const mesh = build_mesh(*((*this->inner).primitive));
        auto const* const first = reinterpret_cast<uint8_t const*>(mesh.vertices.data());
        this->vertex_bytes.assign(first, first + mesh.vertices.size() * sizeof(vertex));
        this->vertex_stride = sizeof(vertex);
        this->vertex_count = static_cast<uint32_t>(mesh.vertices.size());
        this->index_bytes = mesh.index_data;
        this->index_width = mesh.index_width;
        this->index_count = mesh.index_count;
        this->built = true;
    }

    vertex_view drawable_iterator::get_vertex() const {
        this->ensure_built();
        return vertex_view{.data = this->vertex_bytes, .stride = this->vertex_stride, .count = this->vertex_count};
    }

    index_view drawable_iterator::get_index() const {
        this->ensure_built();
        return index_view{.data = this->index_bytes, .width = this->index_width, .count = this->index_count};
    }

    glm::mat4 drawable_iterator::get_transform() const {
        return (*this->inner).transform_matrix;
    }

    resolved_material const* drawable_iterator::current_material() const {
        uint32_t const index = (*this->inner).primitive->material_index;
        return index < this->materials.size() ? &this->materials[index] : nullptr;
    }

    image_view drawable_iterator::slot(int32_t const i) const {
        resolved_material const* material = this->current_material();
        // The slot count is a fixed five (resolved_material::slots) and `i` is an arbitrary int32_t from
        // the caller, so the index is checked rather than trusted.
        if (material == nullptr || i < 0 || static_cast<std::size_t>(i) >= material->slots.size()) {
            return image_view{};
        }
        return material->slots[static_cast<std::size_t>(i)];
    }

    image_view drawable_iterator::get_albedo() const {
        return this->slot(0);
    }

    image_view drawable_iterator::get_metallic_roughness() const {
        return this->slot(1);
    }

    image_view drawable_iterator::get_normal() const {
        return this->slot(2);
    }

    image_view drawable_iterator::get_occlusion() const {
        return this->slot(3);
    }

    image_view drawable_iterator::get_emissive() const {
        return this->slot(4);
    }

    resolved_factors drawable_iterator::get_factors() const {
        resolved_material const* material = this->current_material();
        return material == nullptr ? resolved_factors{} : material->factors;
    }

    bool drawable_iterator::get_double_sided() const {
        resolved_material const* material = this->current_material();
        return material != nullptr && material->double_sided;
    }

    uint32_t drawable_iterator::get_toon_family() const {
        resolved_material const* material = this->current_material();
        return material == nullptr ? 0u : material->toon_family; // 0 == toon_family::none
    }

    uint32_t drawable_iterator::get_overlay_kind() const {
        resolved_material const* material = this->current_material();
        return material == nullptr ? 0u : material->overlay_kind; // 0 == overlay_kind::none
    }

    std::string_view drawable_iterator::get_material_name() const {
        resolved_material const* material = this->current_material();
        return material == nullptr ? std::string_view{} : std::string_view(material->name);
    }

    // ---- async twins (see gltf_loader.cppm): delegate to the sync functions on a
    //      std::async thread; the caller consumes the future when the result is needed ----

    std::future<std::expected<scenes, error_code>> load_model_async(std::string_view const file_name) {
        // copy the path: the view must stay valid while the async task runs
        return std::async(std::launch::async, [path = std::string(file_name)] { return load_model(path); });
    }

    std::future<std::vector<resolved_material>> resolve_materials_async(deren::gltf::scenes const& scenes) {
        return std::async(std::launch::async, [&scenes] { return resolve_materials(scenes); });
    }

    // ---- keyframe sampling (pure CPU; see animation_sampler docs for the values layout) ----

    channel_sample sample_channel(animation_sampler const& sampler, animation_path const path, float const t) {
        channel_sample out = {};
        std::size_t const keys = sampler.times.size();
        if (keys == 0) {
            return out; // no keyframes: nothing to sample
        }
        // values per keyframe: the sampler records it (morph-weights channels vary per mesh);
        // fall back to the path rule when a sampler carries no per_key shape
        std::size_t comps = sampler.per_key;
        if (comps == 0) {
            comps = path == animation_path::rotation ? 4 : 3;
        }
        bool const cubic = sampler.interpolation == animation_interpolation::cubic_spline;
        std::size_t const stored_per_key = comps * (cubic ? 3 : 1);
        if (sampler.values.size() < keys * stored_per_key) {
            return out; // value count does not match the key count: broken sampler
        }
        out.valid = true;

        // read one key's block: 'offset' selects the value triplet (0 for linear, comps for the
        // middle value triplet of a cubic block) or a tangent (comps / 2 * comps of a cubic block)
        auto const read_block = [&](std::size_t const key, std::size_t const offset, std::vector<float>& block) {
            block.resize(comps);
            std::size_t const base = key * stored_per_key + offset;
            for (std::size_t c = 0; c < comps; ++c) {
                block[c] = sampler.values[base + c];
            }
        };
        auto const assign = [&](std::vector<float> const& block) {
            if (path == animation_path::rotation) {
                out.quat = glm::quat(block[3], block[0], block[1], block[2]); // glm ctor order (w, x, y, z)
            } else if (path == animation_path::weights) {
                out.scalars = block; // one value per morph target
            } else {
                out.vec3 = glm::vec3(block[0], block[1], block[2]);
            }
        };

        // clamp t into the keyframe range, then find the left key: times[key] <= t < times[key + 1]
        float const time = std::clamp(t, sampler.times.front(), sampler.times.back());
        std::size_t key = 0;
        while (key + 1 < keys && sampler.times[key + 1] <= time) {
            ++key;
        }
        auto const hold_key = [&] {
            std::vector<float> value;
            read_block(key, cubic ? comps : 0, value);
            assign(value);
        };

        // STEP interpolation and the range end hold the left key's value
        if (sampler.interpolation == animation_interpolation::step || key + 1 >= keys) {
            hold_key();
            return out;
        }

        float const dt = sampler.times[key + 1] - sampler.times[key];
        if (dt <= 0.0f) { // duplicate timestamps (invalid per the spec): hold the key's value
            hold_key();
            return out;
        }
        float const u = (time - sampler.times[key]) / dt;

        std::vector<float> a;
        std::vector<float> b;
        read_block(key, cubic ? comps : 0, a);
        read_block(key + 1, cubic ? comps : 0, b);

        if (cubic) {
            // Hermite spline over the segment; tangents are scaled by the segment duration
            std::vector<float> out_tangent;
            std::vector<float> in_tangent;
            read_block(key, 2 * comps, out_tangent);
            read_block(key + 1, 0, in_tangent);
            float const h00 = 2.0f * u * u * u - 3.0f * u * u + 1.0f;
            float const h10 = u * u * u - 2.0f * u * u + u;
            float const h01 = -2.0f * u * u * u + 3.0f * u * u;
            float const h11 = u * u * u - u * u;
            std::vector<float> value(comps);
            for (std::size_t c = 0; c < comps; ++c) {
                value[c] = h00 * a[c] + h10 * dt * out_tangent[c] + h01 * b[c] + h11 * dt * in_tangent[c];
            }
            if (path == animation_path::rotation) {
                // component-wise spline over the quaternion, then normalize (per the spec)
                glm::quat const q(value[3], value[0], value[1], value[2]);
                float const norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                out.quat = norm > 0.0f ? glm::normalize(q) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            } else {
                assign(value);
            }
            return out;
        }

        // LINEAR
        if (path == animation_path::rotation) {
            glm::quat const q0(a[3], a[0], a[1], a[2]);
            glm::quat q1(b[3], b[0], b[1], b[2]);
            if (glm::dot(q0, q1) < 0.0f) {
                q1 = glm::quat(-q1.w, -q1.x, -q1.y, -q1.z); // shortest arc: flip one endpoint
            }
            out.quat = glm::normalize(glm::slerp(q0, q1, u));
        } else {
            std::vector<float> value(comps);
            for (std::size_t c = 0; c < comps; ++c) {
                value[c] = a[c] + (b[c] - a[c]) * u;
            }
            assign(value);
        }
        return out;
    }

    node_pose sample_node(animation const& animation, std::size_t const target_node, node_pose const& base, float const t) {
        node_pose pose = base;
        for (animation_channel const& channel : animation.channels) {
            if (channel.target_node != target_node || channel.sampler >= animation.samplers.size()) {
                continue;
            }
            channel_sample const sample = sample_channel(animation.samplers[channel.sampler], channel.path, t);
            if (!sample.valid) {
                continue;
            }
            switch (channel.path) {
            case animation_path::translation:
                pose.translation = sample.vec3;
                pose.any_transform = true;
                break;
            case animation_path::rotation:
                pose.rotation = sample.quat;
                pose.any_transform = true;
                break;
            case animation_path::scale:
                pose.scale = sample.vec3;
                pose.any_transform = true;
                break;
            case animation_path::weights:
                pose.weights = sample.scalars; // active morph weights for the node's mesh
                break;
            }
            pose.any_channel = true;
        }
        return pose;
    }

    // ---- whole-model world AABB (pure CPU, over the retained scene data) ----

    scene_bounds compute_scene_bounds(deren::gltf::scenes const& scenes) {
        scene_bounds result;
        glm::vec3 scene_min(std::numeric_limits<float>::infinity());
        glm::vec3 scene_max(-std::numeric_limits<float>::infinity());
        // scenes is iterable: begin()/end() flatten scene -> node -> mesh -> primitive with the
        // owning node's world transform (drawable_ref::transform_matrix)
        for (drawable_ref const& drawable : scenes) {
            auto const it = drawable.primitive->vertex.find("POSITION");
            if (it == drawable.primitive->vertex.end()) {
                continue; // no positions: nothing to bound
            }
            ++result.primitive_count;
            glm::vec3 local_min(std::numeric_limits<float>::infinity());
            glm::vec3 local_max(-std::numeric_limits<float>::infinity());
            std::size_t const vertex_count = it->second.data.size() / sizeof(glm::vec3);
            for (std::size_t i = 0; i < vertex_count; ++i) {
                glm::vec3 const p = reinterpret_cast<glm::vec3 const*>(it->second.data.data())[i];
                local_min = glm::min(local_min, p);
                local_max = glm::max(local_max, p);
            }
            // TRS transforms map an AABB to an AABB, so transforming the 8 corners is exact
            for (int32_t x = 0; x < 2; ++x) {
                for (int32_t y = 0; y < 2; ++y) {
                    for (int32_t z = 0; z < 2; ++z) {
                        glm::vec3 const corner(x ? local_max.x : local_min.x, y ? local_max.y : local_min.y, z ? local_max.z : local_min.z);
                        glm::vec4 const world = drawable.transform_matrix * glm::vec4(corner, 1.0f);
                        scene_min = glm::min(scene_min, glm::vec3(world));
                        scene_max = glm::max(scene_max, glm::vec3(world));
                    }
                }
            }
        }
        if (result.primitive_count == 0) {
            return result; // valid == false
        }
        result.valid = true;
        result.min = scene_min;
        result.max = scene_max;
        return result;
    }

    // ---- diagnostics: log what the loader exported for a loaded model ----

    scene_bounds log_scene_diagnostics(deren::gltf::scenes const& scenes) {
        scene_bounds const bounds = compute_scene_bounds(scenes);
        if (!bounds.valid) {
            deren::utility::panic("model has no drawable primitives");
        }
        glm::vec3 const scene_center = bounds.min * 0.5f + bounds.max * 0.5f;
        float const scene_radius = glm::length(bounds.max - bounds.min) * 0.5f;

        deren::utility::log("scene loaded: {} textures, {} materials, {} primitives", scenes.textures.size(), scenes.materials.size(), bounds.primitive_count);
        deren::utility::log("scene bounds (aabb): min ({:.3f}, {:.3f}, {:.3f}), max ({:.3f}, {:.3f}, {:.3f}), center ({:.3f}, {:.3f}, {:.3f}), radius {:.3f}",
                            bounds.min.x, bounds.min.y, bounds.min.z, bounds.max.x, bounds.max.y, bounds.max.z,
                            scene_center.x, scene_center.y, scene_center.z, scene_radius);

        // Scene hierarchy summary: report the retained tree shape (roots / total nodes / max
        // depth / mesh-bearing nodes) for diagnostics. Walks the retained tree through
        // scene_node_iterator (DFS pre-order, transform-only nodes included).
        {
            size_t total_nodes = 0;
            size_t mesh_nodes = 0;
            size_t max_depth = 0;
            std::vector<std::string> tree_lines;
            for (scene_node_iterator it = scenes.nodes_begin(); it != deren::gltf::scenes::nodes_end(); ++it) {
                ++total_nodes;
                size_t const depth = it.get_depth();
                max_depth = std::max(max_depth, depth);
                bool const has_mesh = it.get_drawable_count() > 0;
                if (has_mesh) {
                    ++mesh_nodes;
                }
                std::string_view const name = it.get_name();
                tree_lines.push_back(std::format("{}{}{}", std::string(depth * 2, ' '),
                                                 name.empty() ? std::string("<unnamed>") : std::string(name),
                                                 has_mesh ? " [mesh]" : ""));
            }
            deren::utility::log("scene hierarchy: {} roots, {} nodes total ({} with meshes), max depth {}",
                                !scenes.scene.empty() ? scenes.scene.front().root_indices.size() : 0,
                                total_nodes, mesh_nodes, max_depth);
            for (std::string const& line : tree_lines) {
                deren::utility::log("  {}", line);
            }
        }

        // Animation summary: scenes.animations holds the decoded keyframe animations
        // (channels -> samplers); playback of the first channel-bearing animation runs in the
        // frame loop (gui transport). This block only logs what the loader exported.
        if (!scenes.animations.empty()) {
            deren::utility::log("animations: {}", scenes.animations.size());
            for (animation const& anim : scenes.animations) {
                std::string_view const anim_name = anim.name.empty() ? std::string_view("<unnamed>") : std::string_view(anim.name);
                // a typical animation shares one keyframe count across its samplers; report the first
                size_t const keys = anim.samplers.empty() ? 0 : anim.samplers.front().times.size();
                deren::utility::log("  animation '{}': {} channels, {} samplers, {} keyframes", anim_name, anim.channels.size(), anim.samplers.size(), keys);
            }
        }

        // Skin summary: scenes.skins holds the file's skins (joint asset-node indices + inverse
        // bind matrices); the skin rigs and per-frame joint matrices are built by the caller.
        if (!scenes.skins.empty()) {
            deren::utility::log("skins: {}", scenes.skins.size());
            for (skin const& s : scenes.skins) {
                std::string_view const skin_name = s.name.empty() ? std::string_view("<unnamed>") : std::string_view(s.name);
                deren::utility::log("  skin '{}': {} joints", skin_name, s.joints.size());
            }
        }

        // Morph summary: primitives may carry morph targets (POSITION/NORMAL deltas), meshes/
        // nodes default weights, and "weights" animation channels.
        {
            size_t morph_prims = 0;
            size_t morph_targets = 0;
            size_t weighty_meshes = 0;
            for (scene const& loader_scene : scenes.scene) {
                for (node const& loader_node : loader_scene.nodes) {
                    for (mesh const& m : loader_node.meshes) {
                        if (!m.weights.empty()) {
                            ++weighty_meshes;
                        }
                        for (primitive const& prim : m.primitives) {
                            if (!prim.targets.empty()) {
                                ++morph_prims;
                                morph_targets += prim.targets.size();
                            }
                        }
                    }
                }
            }
            if (morph_prims > 0) {
                deren::utility::log("morph: {} primitive(s) with morph targets ({} total targets, {} mesh(es) with default weights)", morph_prims, morph_targets, weighty_meshes);
            }
        }

        // Camera / light summary: nodes may reference glTF cameras and punctual lights
        // (KHR_lights_punctual). Authored cameras are consumed by the caller as orbit-camera
        // viewpoint seeds; point/spot lights are consumed by the demo main (auto-enabled via
        // the runtime's punctual-light UBO) - this block logs what the loader exported.
        if (!scenes.cameras.empty()) {
            size_t perspective = 0;
            for (camera const& cam : scenes.cameras) {
                if (cam.type == camera_type::perspective) {
                    ++perspective;
                }
            }
            deren::utility::log("cameras: {} ({} perspective, {} orthographic)", scenes.cameras.size(), perspective, scenes.cameras.size() - perspective);
        }
        if (!scenes.lights.empty()) {
            size_t directional = 0;
            size_t point = 0;
            size_t spot = 0;
            for (light const& l : scenes.lights) {
                switch (l.type) {
                case light_type::directional:
                    ++directional;
                    break;
                case light_type::point:
                    ++point;
                    break;
                case light_type::spot:
                    ++spot;
                    break;
                }
            }
            deren::utility::log("lights: {} ({} directional, {} point, {} spot)", scenes.lights.size(), directional, point, spot);
        }
        return bounds;
    }
} // namespace deren::gltf
