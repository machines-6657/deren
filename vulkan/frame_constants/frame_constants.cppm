// ============================================================================
// module: deren.vulkan.frame_constants
// module version: 0.4.0  (independent of the app version in CMakeLists project(VERSION))
//
// ONE FRAME'S SHARED CONSTANTS: the per-frame facts the frame loop produces and a
// pass may read while it records.
//
// WHY A VALUE RATHER THAN A CALLBACK, and the reason is the framework's own rule
// (see vulkan.pass: `pass_host` is the RUNNER's interface and a pass never sees it,
// because a context object that answers whatever the newest pass asks for is what
// that rule exists to prevent). These are not answers to questions - they are the
// frame's data, produced once by the frame loop whether or not any pass wants it,
// so a new field is a change to the thing every frame fills rather than a private
// arrangement between one pass and the host. That is the property that keeps the
// set small.
//
// WHAT IS IN IT, and how it was decided: every field is one the RESOLVE LAYER
// already reads out of the renderer today while composing a pass's push block -
// measured over `runtime::resolve_*` (the depth terms `proj[2][2]`/`proj[3][2]`,
// the target texel size from the extent, `inv_view_proj` for the ray passes,
// `camera_pos`/`view` for the shadow fit, `scene_center`/`scene_radius` for the
// probe grid and the GI radii, the sun's direction). Fields nothing consumes were
// left out on purpose: `prev_view_proj` is uploaded to the camera UBO and read by
// no pass, and the frame identity (image index, slot, image count, extent) already
// travels in `pass::resolved_io::frame`.
//
// DEPENDENCIES: glm and the Vulkan headers, and NOTHING else - deliberately not
// `deren.vulkan.primitive`, whose `camera_ubo`/`light_ubo` these fields mirror. The
// framework must not depend on `deren.vulkan.core` (deren.vulkan.pass names that rule), and
// `deren.vulkan.primitive` imports it; so the mirror is written out here, and the frame
// loop is the ONE place that fills it from the UBOs - which is where the two must
// agree, in one function, rather than in a type relationship nobody reads.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

export module deren.vulkan.frame_constants;

export import deren.vstd;

/**
 * @file vulkan/frame_constants/frame_constants.cppm
 * @defgroup vulkan_frame_constants Frame Constants
 * @brief the per-frame facts the frame loop produces and a pass reads while it records
 *
 * One value type, filled once per frame by the renderer (`runtime::update_frame_constants`) and handed to every
 * pass through `vulkan.pass::resolved_io::constants`. It exists so that a pass can compose its own push block
 * instead of the renderer composing it for it - without a callback interface, which is the shape the pass
 * framework deliberately keeps away from passes (see `vulkan.pass::pass_host`).
 */

namespace deren::vulkan {
    /**
     * @ingroup vulkan_frame_constants
     * @brief the frame loop's RENDERER SETTINGS, for the ones more than one pass reads
     *
     * THE RULE THAT DECIDES WHAT IS HERE, and it is what keeps this from becoming a bag of knobs: a setting a pass
     * reads ALONE is that pass's own parameter (the TAA resolve's two blend weights, the debug view's channel -
     * they live in those passes, and the renderer's public setters forward to them); a setting SEVERAL passes read
     * is the frame loop's, because no single pass can own a value its siblings must agree on. The post chain is
     * what made the distinction necessary: the composite, the four bloom levels and FXAA all push the exposure and
     * the bloom weights, and the composite and the GI spatial filter share one silhouette criterion.
     */
    export struct render_settings {
        /// the tonemap's exposure scale (the composite, the bloom levels and FXAA all push it)
        float exposure = 1.0f;
        /// the bloom sum's weight and the prefilter's threshold (the same three)
        float bloom_intensity = 0.0f;
        float bloom_threshold = 0.6f;
        /// FXAA's two thresholds (its own pass, and the composite, which pushes the same block)
        float fxaa_subpixel = 0.75f;
        float fxaa_edge_threshold = 0.166f;
    };

    /**
     * @ingroup vulkan_frame_constants
     * @brief one frame's shared constants, as the frame loop produced them
     *
     * @note every matrix here is the SAME one the camera UBO carries for this frame, so a pass that composes a
     *       push block from it and a shader that reads the UBO cannot disagree about the camera. `proj` keeps
     *       the TAA jitter (the geometry must be sampled at the jittered offsets); `view_proj_unjittered` is
     *       the pair the motion vectors come from (see `camera_ubo` in deren.vulkan.primitive for the full argument).
     */
    export struct frame_constants {
        /// this frame's view matrix
        glm::mat4 view = glm::mat4(1.0f);
        /// this frame's projection, INCLUDING the TAA jitter when TAA is on
        glm::mat4 proj = glm::mat4(1.0f);
        /// `proj * view` with the jitter removed: what motion vectors and reprojection are defined on
        glm::mat4 view_proj_unjittered = glm::mat4(1.0f);
        /// the inverse of the JITTERED `proj * view`: what a ray pass unprojects a pixel with
        glm::mat4 inv_view_proj = glm::mat4(1.0f);
        /// the camera's world position (xyz; `camera_ubo` pads the fourth lane)
        glm::vec3 camera_pos = glm::vec3(0.0f);
        /// the centre of the fitted scene bounds - the same cube the shadow fit and the probe grid anchor to
        glm::vec3 scene_center = glm::vec3(0.0f);
        /// the fitted scene's radius (the probe grid's cell size and the GI radii scale with it)
        float scene_radius = 0.0f;
        /// the sun's direction, normalized here; the `w` lane is unused
        glm::vec4 light_dir = glm::vec4(0.0f);
        /// the renderer settings more than one pass reads (see render_settings for the rule that decides)
        render_settings settings = {};
    };

} // namespace deren::vulkan
