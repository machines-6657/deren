// The shadow pass's implementation: the depth-only pipeline (built against the SCENE pipeline layout and the
// context's depth format) and the per-cascade recording - the layer's transition to a depth attachment, the
// depth-only instance at the map's edge, the pre-recorded secondary's execution and the instance's end. Moved out of
// `runtime::make_shadow_pipeline` + the shadow block of `begin_recording` UNCHANGED in behaviour, except for the
// things the pass does not own (the hand-back barrier over every allocated layer, and the per-cascade content, which
// the renderer records through the frame's callback) - so the capture gate, which runs with shadows on in all twelve
// scenarios, decides the move.

module;

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.shadow;

import deren.vulkan.constant_init;
import deren.vulkan.pipelines; // build_shadow: the depth-only pipeline this pass owns
import deren.utility;

namespace deren::vulkan::pass {

    shadow_pass::~shadow_pass() {
        this->release_owned();
    }

    void shadow_pass::release_owned() noexcept {
        this->mesh_pipeline.reset();
        this->meshlet_pipeline.reset();
    }

    render_resource::pass_io const& shadow_pass::io() const noexcept {
        return render_resource::shadow_io;
    }

    deren::vulkan::pass::behaviour const& shadow_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view shadow_pass::feature() const noexcept {
        // THE NAME THE RENDERER ALREADY ANSWERS: `f.shadow` is the two knobs, this pipeline's existence and "the flat
        // render mode reads no shadow map" - one answer shared by the frame loop's stage gate, the overlay and the
        // startup log, so the three cannot disagree about whether this pass runs.
        return "shadow";
    }

    void shadow_pass::create(pass_context const& context) {
        if (context.device == VK_NULL_HANDLE || context.depth_format == VK_FORMAT_UNDEFINED) {
            return;
        }
        if (this->device != VK_NULL_HANDLE && this->device != context.device) {
            this->release_owned();
        }
        this->device = context.device;
        if (this->mesh_pipeline.has_value()) {
            return; // already built for this device
        }
        std::span<uint8_t const> const fragment_spirv = context.shader != nullptr ? context.shader(context.owner, fragment_shader_name) : std::span<uint8_t const>{};
        // ---- THE VERTEX FORM IS GONE (docs/mesh_shaders.md step 4): the pass's geometry arrives through a MESH
        // stage, so the vertex module is neither loaded nor registered anywhere, and the mesh one is REQUIRED. A
        // device without VK_EXT_mesh_shader does not reach here at all (the runtime refuses to start), which is why
        // the two checks below are defensive: they keep a mis-registered shader a log line rather than a crash.
        if (context.shader == nullptr || fragment_spirv.empty()) {
            deren::utility::log("shadow disabled: the owner has no {}", fragment_shader_name);
            return;
        }
        if (!context.mesh_shaders) {
            deren::utility::log("shadow disabled: the device has no mesh shaders, and the vertex form is gone (docs/mesh_shaders.md step 4)");
            return;
        }
        std::span<uint8_t const> const mesh_spirv = context.shader != nullptr ? context.shader(context.owner, mesh_shader_name) : std::span<uint8_t const>{};
        if (mesh_spirv.empty()) {
            deren::utility::log("shadow disabled: the owner has no {}", mesh_shader_name);
            return;
        }
        // The pipeline is heap-native: nothing about the draw's descriptors or push travels through a layout.
        // IT IS THE MESH FORM THAT IS BUILT FIRST NOW, and it is required: with the vertex form gone this pass cannot
        // draw its casters any other way, so a refusal is a DISABLED PASS rather than a fallback - visible as a
        // missing shadow rather than as a wrong picture, and named in the log.
        auto mesh_built = pipelines::build_shadow(context.device, context.depth_format, create_bias_constant, create_bias_slope, create_bias_clamp, mesh_spirv, fragment_spirv, VK_SHADER_STAGE_MESH_BIT_EXT);
        if (!mesh_built) {
            deren::utility::log("shadow disabled: the mesh pipeline was refused ({})", mesh_built.error());
            this->release_owned();
            return;
        }
        this->mesh_pipeline = std::move(*mesh_built);
        deren::utility::log("SUCCESS: shadow MESH pipeline created (the depth-only pass, fed by mesh dispatches)");
        // ---- ... and the MESHLET form (docs/mesh_shaders.md step 3): only the MESH module differs (same fragment
        // stage), and a missing shader or a refusal is a log line - the mesh form above is a complete answer.
        std::span<uint8_t const> const meshlet_spirv = context.shader != nullptr ? context.shader(context.owner, meshlet_shader_name) : std::span<uint8_t const>{};
        if (!meshlet_spirv.empty()) {
            auto meshlet_built = pipelines::build_shadow(context.device, context.depth_format, create_bias_constant, create_bias_slope, create_bias_clamp, meshlet_spirv, fragment_spirv, VK_SHADER_STAGE_MESH_BIT_EXT);
            if (meshlet_built) {
                this->meshlet_pipeline = std::move(*meshlet_built);
                deren::utility::log("SUCCESS: shadow MESHLET pipeline created (one workgroup per meshlet, window read from the table)");
            } else {
                deren::utility::log("shadow: the meshlet pipeline was refused ({}), so the pass keeps the mesh form", meshlet_built.error());
            }
        }
    }

    void shadow_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing to reset: the pipeline depends on the DEPTH format and on nothing whose size changes, and the map
        // it renders into is sized by the frame's `map_size` rather than by the surface.
    }

    bool shadow_pass::pipeline_ready() const noexcept {
        // ONE FORM IS ENOUGH, and neither is the vertex one: the mesh form is required at create and the meshlet form
        // is preferred over it, so "ready" is "either was built" (docs/mesh_shaders.md step 4)
        return this->meshlet_pipeline.has_value() || this->mesh_pipeline.has_value();
    }

    VkPipeline shadow_pass::pipeline() const noexcept {
        // THE MESHLET FORM WHEN THERE IS ONE, and the MESH form otherwise: they are the same pass (same targets, same
        // fragment stage, same casters), so which one draws is not the frame's business. There is NO vertex form
        // since step 4 (docs/mesh_shaders.md): a null here means no shadow map rather than a different rasterizer.
        if (this->meshlet_pipeline.has_value()) {
            return this->meshlet_pipeline->get_pipeline();
        }
        if (this->mesh_pipeline.has_value()) {
            return this->mesh_pipeline->get_pipeline();
        }
        return VK_NULL_HANDLE; // no mesh form, no shadow map: the vertex form is gone (see create)
    }

    void shadow_pass::set_frame(shadow_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void shadow_pass::record(resolved_io const& io) {
        if (!this->pipeline_ready() || io.targets.empty()) {
            return;
        }
        if (this->pass_frame.record_cascade == nullptr || this->pass_frame.run_tasks == nullptr || this->pass_frame.map_size == 0u) {
            return;
        }
        // ONE LAYER PER CASCADE, and never more than the map has: the DECLARATION claims a RUN of cascade layers
        // (render_resource::shadow_targets) and the FRAME caps it at the layers the image actually has - so the
        // two counts meet here. The secondaries are the frame's own count, and they are what decides how many
        // layers this frame renders: a cascade with no secondary to record into is not rendered at all.
        uint32_t const layers = static_cast<uint32_t>(std::min<std::size_t>(this->pass_frame.cascades.size(), io.targets.size()));
        if (layers == 0u) {
            return;
        }
        VkPipeline const pipeline = this->pipeline();
        // ... and HOW it must be fed travels with it: a mesh pipeline has no input assembler, so its casters are
        // dispatched rather than drawn (see the frame's record_cascade).
        bool const meshlets = this->meshlet_pipeline.has_value();
        bool const mesh_stage = meshlets || this->mesh_pipeline.has_value();
        // ---- THE CONTENT: one task per cascade, each into its OWN secondary ----
        // A VkCommandPool is not thread safe, which is why every cascade has its own {pool, buffer} pair (the same
        // rule the main pass's workers follow). Only the CONTENT moves off the primary thread: the barriers, the
        // instances and the executions below stay here, in the layer order the attachments require, so a parallel
        // frame's recorded commands are identical to a sequential one's.
        std::vector<std::function<void()>> tasks;
        tasks.reserve(layers);
        std::vector<bool> recorded(layers, false);
        for (uint32_t cascade = 0; cascade < layers; ++cascade) {
            tasks.emplace_back([this, cascade, pipeline, mesh_stage, meshlets, &recorded] {
                VkCommandBuffer const secondary = this->pass_frame.cascades[cascade];
                if (secondary == VK_NULL_HANDLE) {
                    return;
                }
                recorded[cascade] = this->pass_frame.record_cascade(this->pass_frame.owner, secondary, cascade, pipeline, mesh_stage, meshlets);
            });
        }
        this->pass_frame.run_tasks(this->pass_frame.owner, tasks);

        // ---- ONE INSTANCE PER CASCADE, in the primary ----
        VkExtent2D const map_extent = {this->pass_frame.map_size, this->pass_frame.map_size};
        for (uint32_t cascade = 0; cascade < layers; ++cascade) {
            VkImageView const layer_view = io.targets[cascade].view;
            VkImage const layer_image = io.targets[cascade].image;
            if (layer_view == VK_NULL_HANDLE || layer_image == VK_NULL_HANDLE) {
                continue;
            }
            // THIS layer to a renderable depth attachment: one barrier per layer, because the transition constant's
            // subresource range is single-layer and each layer is its own attachment here. Its loadOp CLEAR discards
            // the previous frame's contents, so UNDEFINED as the old layout is valid.
            VkImageMemoryBarrier2 layer_barrier = deren::vulkan::depth_attachment_transition;
            layer_barrier.image = layer_image;
            layer_barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, cascade, 1};
            VkDependencyInfo const layer_dependency = make_image_dependency_info(1, &layer_barrier);
            vkCmdPipelineBarrier2(io.cmd, &layer_dependency);
            // Depth-only rendering into this cascade (no colour attachment), with loadOp CLEAR (the far plane) and
            // storeOp STORE - the map has to survive for the shading stages that sample it.
            VkRenderingAttachmentInfo const depth_attachment = make_depth_attachment_info(layer_view, VK_ATTACHMENT_STORE_OP_STORE);
            VkRenderingInfo const rendering_info = make_rendering_info(VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT, {{0, 0}, map_extent}, false, nullptr, &depth_attachment);
            vkCmdBeginRendering(io.cmd, &rendering_info);
            // Never execute a secondary whose begin failed - executing an unrecorded command buffer is a VUID and can
            // wedge the frame slot, which is what the recorded flags are for (a null buffer or a log line from the
            // callback leaves its flag false).
            if (recorded[cascade]) {
                VkCommandBuffer const secondary = this->pass_frame.cascades[cascade];
                vkCmdExecuteCommands(io.cmd, 1, &secondary);
            }
            vkCmdEndRendering(io.cmd);
        }
    }

} // namespace deren::vulkan::pass
