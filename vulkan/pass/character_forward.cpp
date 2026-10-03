// The character-forward pass's implementation: the two hand-off barriers, the LOAD instance, and the
// session state that makes `ZTest Equal` + `ZWrite Off` true rather than merely intended.
//
// MODELLED ON transparent.cpp AND DELIBERATELY SIMPLER: that pass records into a per-slot SECONDARY
// command buffer because the renderer fans its leaves out across the task pool, and this one draws
// directly into the primary. A character has hundreds of leaves, not the scene's thousands, and the
// secondary's inheritance plumbing (the heaps, the rendering info, the begin/end pair) buys nothing at
// that size. If the leaf count ever argues for it, this is the file that changes.

module;

#include <array>
#include <cstdint>
#include <span>
#include <vulkan/vulkan.h>

module deren.vulkan.pass.character_forward;

import deren.vulkan.render_resource;
import deren.vulkan.constant_init;
import deren.utility;

namespace deren::vulkan::pass {

    render_resource::pass_io const& character_forward_pass::io() const noexcept {
        return render_resource::character_forward_io;
    }

    deren::vulkan::pass::behaviour const& character_forward_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view character_forward_pass::feature() const noexcept {
        // THE RENDERER'S GATE, as a feature name (see the scene pass): a scene with no toon character is a
        // scene this pass is INACTIVE on - which is what the runner checks before it resolves anything, so
        // the pass neither records nor resolves and costs nothing on those frames. That is also what keeps
        // every capture-gate scenario byte-identical while the feature is off.
        //
        // THE OVERLAY GROUP RIDES THE SAME GATE, and that is a decision rather than an accident of sharing a
        // pass: an overlay mask is part of the article's toon character stage (it multiplies what THIS stage
        // wrote), so a frame that does not run the toon stage has nothing for a mask to modify. The
        // consequence a reader should know: with the feature off, the two mask meshes are drawn by NOTHING -
        // they are out of the opaque, transparent and shadow lists (see `frame_overlay`) and this pass does not
        // run - which at their default `_Color` of white is also what drawing them would have looked like.
        return "character_forward";
    }

    void character_forward_pass::create(pass_context const&) {
        // Nothing to build: no own set (every slot these leaves read comes from the frame's heaps) and no
        // pipeline (the renderer registers the named one, and the leaves bind it through `default_name`).
    }

    void character_forward_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing is per-image here: the targets belong to the frame loop and the leaves own their buffers.
    }

    void character_forward_pass::set_frame(character_forward_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void character_forward_pass::record(resolved_io const& io) {
        // BOTH LEAF LISTS ARE THE GATE, not just the toon one: a frame whose only character geometry is the
        // article's two masks still has something to multiply, and returning on `leaves.empty()` alone would
        // silently drop it. Either list being non-empty is the pass having work to do.
        if (this->pass_frame.make_environment == nullptr || this->pass_frame.pipeline_name.empty() || io.targets.size() < 2 ||
            (this->pass_frame.leaves.empty() && this->pass_frame.overlay_leaves.empty() && this->pass_frame.outline_leaves.empty())) {
            return; // the runner resolves all of this or skips the pass (see make_character_forward_frame)
        }
        VkImageView const target_view = io.targets[0].view; // the scene colour target (declaration order)
        VkImage const target_image = io.targets[0].image;
        VkImageView const depth_view = io.targets[1].view; // the surface depth
        VkImage const depth_image = io.targets[1].image;

        // TWO HAND-OFFS, and both are needed for the same reason the transparent pass needs them: the
        // lighting stage (or the transparent pass after it) left the depth in SHADER_READ_ONLY, so it has
        // to go back to the attachment layout before this instance can test against it; and the scene
        // colour's last store has to be published before this instance LOADs the same image, because
        // dynamic rendering inserts no dependency between two instances.
        std::array<VkImageMemoryBarrier2, 2> barriers = {};
        barriers[0] = deren::vulkan::sampling_to_depth_attachment_transition;
        barriers[0].image = depth_image;
        barriers[1] = deren::vulkan::color_attachment_dependency;
        barriers[1].image = target_image;
        VkDependencyInfo const dependency = make_image_dependency_info(static_cast<uint32_t>(barriers.size()), barriers.data());
        vkCmdPipelineBarrier2(io.cmd, &dependency);

        // LOAD on both attachments: the lit frame and the opaque surface are what this pass draws OVER.
        VkRenderingAttachmentInfo const color_attachment = make_load_color_attachment_info(target_view);
        VkRenderingAttachmentInfo const depth_attachment = make_load_depth_attachment_info(depth_view);
        VkRenderingInfo const rendering_info = make_rendering_info(0, {{0, 0}, this->pass_frame.extent}, &color_attachment, 1, &depth_attachment);
        vkCmdBeginRendering(io.cmd, &rendering_info);

        render_environment env = this->pass_frame.make_environment(this->pass_frame.owner, io.cmd, /*gbuffer=*/false);
        // THE TWO THINGS THIS PASS STATES ABOUT ITSELF, neither of which the renderer could know:
        //
        //   WHICH PIPELINE. Every primitive's draw() calls `bind_default()`, so a session's default name is
        //   what its leaves bind - there is no redirect flag and no per-leaf pipeline name to set. The
        //   renderer registered the character pipeline under `pipeline_name`; from here on the leaves are
        //   drawn by it.
        //
        //   DEPTH WRITE OFF, THEN LOCKED. The set comes first (the state starts "unknown", so it is really
        //   emitted), and the lock comes second because every primitive's draw() sets its OWN depth write a
        //   few instructions later - without the lock this pass's `ZWrite Off` would be a statement of
        //   intent that the very next leaf undoes. See render_environment::depth_write_locked.
        env.default_name = this->pass_frame.pipeline_name;
        env.set_depth_write(false);
        env.depth_write_locked = true;

        for (primitive const* const leaf : this->pass_frame.leaves) {
            leaf->draw(env);
        }

        // ---- AND THE OUTLINE GROUP, AFTER THE LEAVES AND BEFORE THE OVERLAYS ----
        //
        // THE ARTICLE'S ① 描边 (`MyZmdOutlineShader`): the character's own leaves drawn a second time as an
        // INVERTED HULL - front faces culled, vertices pushed outward in the VERTEX stage - so the only fragments
        // that survive the depth test are the ring just outside each silhouette. It is a third list for the same
        // reason `overlay_leaves` is a second one: its own pipeline (`outline_pipeline_name`, Cull Front +
        // LESS_OR_EQUAL), its own order, and its own per-material gate (the material's `_OutlineWidth`, which is 0
        // for a material the game itself says has no outline - chen's `cloth_02`).
        //
        // WHY IT IS BEFORE THE OVERLAYS: they multiply what this stage wrote, so the hull is part of what they
        // multiply. WHY THE DEPTH TEST IS ENOUGH: the leaves have just re-shaded the character onto the depth
        // this pass LOADed, so every hull fragment inside a silhouette fails LESS_OR_EQUAL against its own
        // surface and only the outside ring is left.
        //
        // WHAT IS NOT REPRODUCED, and it is the one state the article's shader states that this pass cannot: its
        // `ZWrite On`. Depth write is off and LOCKED for the whole instance (see above), which for a single hull
        // drawn with front-face culling is equivalent - the nearest back face wins the depth test either way - and
        // whose only visible consequence is that the hull does not occlude what is drawn after it (the overlays
        // and the post chain, neither of which is behind a silhouette).
        if (!this->pass_frame.outline_pipeline_name.empty() && !this->pass_frame.outline_leaves.empty()) {
            env.default_name = this->pass_frame.outline_pipeline_name;
            // CULL FRONT FOR THIS GROUP ONLY, and it has to be stated HERE rather than in the pipeline: every
            // leaf's draw() calls `set_cull_mode` with its own double-sided flag (see the field's own note), so a
            // pipeline-level Cull Front would be undone by the first hull. Cleared immediately after, so the
            // overlay group below records its own culling exactly as before.
            env.forced_cull_front = true;
            for (primitive const* const leaf : this->pass_frame.outline_leaves) {
                leaf->draw(env);
            }
            env.forced_cull_front = false;
        }

        // ---- AND THE OVERLAY GROUP, AFTER THEM AND WITH ITS OWN PIPELINE ----
        //
        // THE ORDER IS THE MECHANISM (see `character_forward_frame::overlay_leaves`): the multiply has to see
        // the toon-SHADED pixel, so it cannot be interleaved with the leaves whose result it multiplies. The
        // pipeline is swapped by moving the session's DEFAULT NAME rather than by binding after the fact,
        // because every leaf's draw() calls `bind_default()` and would otherwise put the toon pipeline straight
        // back a few instructions later - the same shape as the depth-write lock above.
        //
        // DEPTH TEST stays on, with the compare the overlay pipeline carries (LESS_OR_EQUAL): the quads sit a
        // little in front of the surface they darken, so the test confines each mask to the face it was authored
        // over. THE ARTICLE'S `Stencil { Ref 1 Comp Equal }` ON THE HAIR SHADOW IS NOT REPRODUCED and cannot be
        // here: this renderer's dynamic rendering info declares NO stencil attachment
        // (`stencilAttachmentFormat` is VK_FORMAT_UNDEFINED, see vulkan/constant_init), so there is no stencil
        // plane for a Ref test - the geometry's own coverage is what stands in for it, which is why the hair
        // shadow is a mesh shaped around the forehead rather than a full-screen quad.
        if (!this->pass_frame.overlay_pipeline_name.empty() && !this->pass_frame.overlay_leaves.empty()) {
            env.default_name = this->pass_frame.overlay_pipeline_name;
            for (primitive const* const leaf : this->pass_frame.overlay_leaves) {
                leaf->draw(env);
            }
        }

        vkCmdEndRendering(io.cmd);

        // Hand the depth back to the layout everything downstream samples it in. TWO later stages read this
        // image (the resolve's disocclusion guard and the composite's edge test), and this pass is what took
        // it out of SHADER_READ - so leaving it as an attachment would make every read after it a layout
        // error. The colour target needs no hand-back: the composite transitions it for itself.
        VkImageMemoryBarrier2 to_sampling = deren::vulkan::shadow_map_sampling_transition; // attachment -> SHADER_READ
        to_sampling.image = depth_image;
        VkDependencyInfo const sampling_dependency = make_image_dependency_info(1, &to_sampling);
        vkCmdPipelineBarrier2(io.cmd, &sampling_dependency);
    }

} // namespace deren::vulkan::pass
