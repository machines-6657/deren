// ============================================================================
// module: deren.vulkan.shadow_fit
// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))
//
// Directional-shadow cascade fitting, as a pure CPU calculation: given the camera,
// the scene extent and the casters' light-space bounds, produce the per-cascade
// light view-projection matrices, the split distances and the texel world sizes.
//
// Extracted from deren.vulkan.runtime, where it was the mathematically heaviest and least
// testable part of a 4257-line translation unit - it had no GPU dependency at all, only
// the accident of living in the class that calls it. Nothing here touches Vulkan or the
// scene tree: gathering the caster boxes stays with the runtime (that needs scene_tree
// and primitive), and the cache that decides WHEN to refit stays with the runtime too
// (that is policy). What is left is the fit itself.
//
// Depends on glm, utility (logging) and deren.vulkan.primitive (max_shadow_cascades).
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <span>
#include <vector>

export module deren.vulkan.shadow_fit;

export import deren.vstd;
export import deren.vulkan.primitive; // max_shadow_cascades (and the light_ubo shape this produces)

/**
 * @file vulkan/shadow_fit/shadow_fit.cppm
 * @defgroup vulkan_shadow_fit Directional Shadow Cascade Fit
 * @brief Fits one orthographic light box per cascade over the part of the view volume that can hold
 *        the scene, and returns the matrices, split distances and texel sizes a renderer needs.
 *
 * Why this is a module rather than a function inside the runtime: the arithmetic here is the part of
 * the shadow system that goes wrong invisibly. Every failure mode it has had was found by looking at
 * an image and reasoning backwards - the camera's generous far plane pushing the up-light end of the
 * fit behind the light camera's near plane (so sun poured through the roof), merging a caster's raw
 * AABB inflating the box to scene size (so thin shadows dissolved), the TAA jitter re-quantizing the
 * light-space grid every frame (so grazing surfaces flickered). None of that is GPU work, and none of
 * it needed a device to be wrong - so it is now a pure function that a headless test can pin.
 *
 * The split between this module and its caller is deliberate:
 *  - the caller GATHERS what to fit (walking the scene tree, computing each leaf's world AABB and its
 *    light-space bounds) and CACHES the result (deciding when a refit is needed at all);
 *  - this module FITS: splits, per-cascade boxes, texel snapping, the orthographic matrices.
 * That keeps the module free of scene_tree and primitive, and keeps the cache policy where the state
 * it reasons about lives.
 */
namespace deren::vulkan::shadow_fit {
    /**
     * @ingroup vulkan_shadow_fit
     * @brief everything the fit needs about one frame's camera, scene and casters
     */
    export struct fit_params {
        /// the UNJITTERED projection and view. The TAA jitter must not reach the fit: it is a
        /// sub-pixel rendering offset, and letting it in re-quantizes the light-space box to whole
        /// texels every frame, so the grid alternates between two alignments and a grazing surface
        /// flickers between lit and shadowed (which the TAA history then averages into a dark band).
        glm::mat4 proj = glm::mat4(1.0f);
        glm::mat4 view = glm::mat4(1.0f);
        /// the camera's world position (the view range is measured from it)
        glm::vec3 camera_pos = {};
        /// the sun's direction, normalized here; `w` is ignored
        glm::vec4 light_dir = {};
        /// a conservative sphere around the scene: the fit clips the camera's own (deliberately
        /// generous) far plane to what can actually hold geometry, and the sphere is also the only
        /// sound bound for a caster whose geometry cannot be bounded
        glm::vec3 scene_center = {};
        float scene_radius = 1.0f;
        /// how far up-light a caster can sit and still throw a shadow into the view: each sub-frustum
        /// corner is extended by this so those casters are inside the fitted box
        float caster_extent = 1.0f;
        /// active cascades, 1..max_shadow_cascades (clamped here)
        uint32_t cascades = 1;
        /// shadow map edge length in texels; drives the texel-size quantization and the per-texel
        /// world size reported back
        uint32_t map_size = 2048;
        /// whether an unbounded caster was found (see fit_casters): its shadow can land anywhere, so
        /// the whole scene sphere's DEPTH range enters every cascade's fit
        bool unbounded_caster = false;
        /// emit the one-time "shadow cascades: N over the view range ..." log. The caller owns the
        /// once-only flag, because whether this is the first fit is its state, not the fit's.
        bool log_summary = false;
        /// every caster's light-space AABB (min, max), as produced by fit_casters()
        std::span<std::pair<glm::vec3, glm::vec3> const> caster_boxes = {};
    };

    /**
     * @ingroup vulkan_shadow_fit
     * @brief the fitted per-cascade state, ready to be written into a light UBO
     * @note `cascade_count` is the number of VALID entries in the arrays; the lanes beyond it repeat
     *       the last fitted one. A shader that samples a lane past the active count (it does not -
     *       every path checks cascade_count first) would otherwise read whatever the previous frame's
     *       fit left there, and repeating the last matrix is the honest value for that case.
     */
    export struct fit_result {
        std::array<glm::mat4, max_shadow_cascades> light_view_proj = {};
        std::array<float, max_shadow_cascades> cascade_splits = {};
        std::array<float, max_shadow_cascades> cascade_texel_world = {};
        /// the normalized light direction the fit used; the caller stores it (its `w` lane also
        /// carries 1 / map_size for the shader)
        glm::vec3 light_dir = {};
        uint32_t cascade_count = 0;
        /// false when the camera was degenerate: the caller must NOT treat this as a fit (the
        /// previous matrices stand) and must not advance its cache
        bool valid = false;
    };

    /**
     * @ingroup vulkan_shadow_fit
     * @brief collect every caster's light-space AABB, so one pass serves every cascade
     * @param world_boxes each caster's world-space AABB
     * @param light_dir the sun direction (normalized here)
     * @param unbounded set to true when a caller could not bound one of its casters; that cascade fit
     *        then also merges the scene sphere's depth (see fit_params::unbounded_caster)
     * @return the light-space boxes, in light_dir order
     * @note light space is a pure ROTATION - each cascade's translation comes from its own box centre
     *       - so one rotation serves every cascade, and with it one caster pass per frame instead of
     *       one per cascade
     */
    export std::vector<std::pair<glm::vec3, glm::vec3>> fit_casters(std::span<std::pair<glm::vec3, glm::vec3> const> world_boxes, glm::vec3 light_dir, bool& unbounded);

    /**
     * @ingroup vulkan_shadow_fit
     * @brief fit one orthographic light box per cascade over the usable part of the view volume
     * @param params the camera, scene and caster bounds to fit
     * @return the per-cascade matrices, splits and texel sizes; `valid == false` when the camera is
     *         degenerate (no aspect ratio yet, or a null projection term) and nothing should be used
     */
    export fit_result fit(fit_params const& params);
} // namespace deren::vulkan::shadow_fit
