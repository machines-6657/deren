module;

#include <array>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <span>
#include <utility>
#include <vector>

module deren.vulkan.shadow_fit;

import deren.utility;

namespace deren::vulkan::shadow_fit {
    namespace {
        /// light space is a pure rotation: lookAt(origin, -light_dir, up) makes light-space z equal to
        /// dot(light_dir, p), which is what the near/far derivation below relies on
        [[nodiscard]] glm::mat4 light_rotation_of(glm::vec3 const& light_dir) {
            return glm::lookAt(glm::vec3(0.0f), -light_dir, glm::vec3(0.0f, 1.0f, 0.0f));
        }
    } // namespace

    std::vector<std::pair<glm::vec3, glm::vec3>> fit_casters(std::span<std::pair<glm::vec3, glm::vec3> const> const world_boxes, glm::vec3 const light_dir, bool& unbounded) {
        unbounded = false;
        glm::mat4 const rotation = light_rotation_of(glm::normalize(light_dir));
        std::vector<std::pair<glm::vec3, glm::vec3>> boxes;
        boxes.reserve(world_boxes.size());
        for (auto const& [world_min, world_max] : world_boxes) {
            glm::vec3 caster_min(std::numeric_limits<float>::max());
            glm::vec3 caster_max(std::numeric_limits<float>::lowest());
            for (int32_t corner = 0; corner < 8; ++corner) {
                glm::vec3 const p((corner & 1) != 0 ? world_max.x : world_min.x,
                                  (corner & 2) != 0 ? world_max.y : world_min.y,
                                  (corner & 4) != 0 ? world_max.z : world_min.z);
                glm::vec3 const ls = glm::vec3(rotation * glm::vec4(p, 1.0f));
                caster_min = glm::min(caster_min, ls);
                caster_max = glm::max(caster_max, ls);
            }
            boxes.emplace_back(caster_min, caster_max);
        }
        return boxes;
    }

    fit_result fit(fit_params const& params) {
        fit_result result;
        // A degenerate camera (the very first frames, before the swapchain has an extent) would put an
        // inverse of a singular matrix under the frustum corners and the fit would be garbage
        // (measured: a light-space z span of 1631 for two unrelated scenes). The caller keeps the
        // enable_shadows() default for that frame and tries again next frame.
        if (!(params.proj[2][2] != 0.0f) || params.scene_radius <= 0.0f) {
            return result;
        }

        float const map_size = static_cast<float>(params.map_size);
        glm::vec3 const light_dir = glm::normalize(glm::vec3(params.light_dir));
        glm::vec3 const up(0.0f, 1.0f, 0.0f);
        glm::mat4 const light_rotation = light_rotation_of(light_dir);

        // ---- the camera's usable depth range, then the cascade splits over it ----
        // The camera's own projection carries a deliberately generous far plane (see
        // make_orbit_camera_ubo: max(100, distance + 2r, 8 * distance)) so that zooming in never clips
        // the scene - but fitting the SHADOW volume to that whole range throws the map away on empty
        // space AND (the actual bug this originally fixed) pushes the up-light end of the fit behind
        // the light camera's near plane, so the roof and upper walls were clipped out of the depth map
        // entirely and the sun poured straight through them. Fit only the part of the view volume that
        // can hold the scene: everything lies within |eye - scene centre| + scene_radius.
        // The PROJECTION, not the view*proj product: near/far live in its z row - for a RH_ZO
        // perspective matrix proj[2][2] = far / (near - far) and proj[3][2] = -(far * near) /
        // (far - near) - and the sub-frustum corners below come from its inverse. Extracting them from
        // the product yields nonsense, because the view rotation mixes the rows: measured, it turned
        // the near plane into -22 and made every cascade split NaN.
        glm::mat4 const base_proj = params.proj;
        float const camera_near = base_proj[3][2] / base_proj[2][2];
        float const camera_far = base_proj[2][2] * camera_near / (1.0f + base_proj[2][2]);
        float const scene_far = glm::distance(params.camera_pos, params.scene_center) + params.scene_radius;
        float const split_near = camera_near;
        float const split_far = std::max(std::min(camera_far, scene_far), camera_near * 2.0f);
        uint32_t const cascades = std::clamp(params.cascades, 1u, max_shadow_cascades);
        // Practical split scheme (Zhang et al.): a logarithmic and a uniform split blended by lambda.
        // Pure logarithmic puts almost all the resolution in the first few metres (the camera near
        // plane is 0.1), pure uniform wastes the near range - the blend is what real-time shadows use.
        // lambda is fixed at 0.75: higher favors the near field, lower the far one.
        constexpr float split_lambda = 0.75f;
        auto const split_distance = [&](float const t) {
            float const logarithmic = split_near * std::pow(split_far / split_near, t);
            float const uniform = split_near + (split_far - split_near) * t;
            return split_lambda * logarithmic + (1.0f - split_lambda) * uniform;
        };

        // the scene sphere's light-space box, for the unbounded-caster fallback (used by every cascade)
        std::pair<glm::vec3, glm::vec3> scene_sphere_ls = {glm::vec3(std::numeric_limits<float>::max()), glm::vec3(std::numeric_limits<float>::lowest())};
        for (int32_t corner = 0; corner < 8; ++corner) {
            glm::vec3 const p = params.scene_center +
                                glm::vec3((corner & 1) != 0 ? params.scene_radius : -params.scene_radius,
                                          (corner & 2) != 0 ? params.scene_radius : -params.scene_radius,
                                          (corner & 4) != 0 ? params.scene_radius : -params.scene_radius);
            glm::vec3 const ls = glm::vec3(light_rotation * glm::vec4(p, 1.0f));
            scene_sphere_ls.first = glm::min(scene_sphere_ls.first, ls);
            scene_sphere_ls.second = glm::max(scene_sphere_ls.second, ls);
        }

        // ---- one fit per cascade ----
        float last_texel_world = 0.0f;
        for (uint32_t cascade = 0; cascade < cascades; ++cascade) {
            float const cascade_near = cascade == 0 ? split_near : split_distance(static_cast<float>(cascade) / static_cast<float>(cascades));
            float const cascade_far = split_distance(static_cast<float>(cascade + 1) / static_cast<float>(cascades));

            // the camera projection restricted to THIS cascade's depth range (same x/y scaling)
            glm::mat4 fit_proj = base_proj;
            fit_proj[2][2] = cascade_far / (cascade_near - cascade_far);
            fit_proj[3][2] = -(cascade_far * cascade_near) / (cascade_far - cascade_near);
            glm::mat4 const inverse_view_proj = glm::inverse(fit_proj * params.view);

            // the sub-frustum's corners, each extended up-light by the caster reach: a caster that far
            // up-sun can still throw its shadow into this cascade
            std::array<glm::vec3, 16> points = {};
            std::size_t count = 0;
            for (int32_t zi = 0; zi < 2; ++zi) {
                for (int32_t yi = 0; yi < 2; ++yi) {
                    for (int32_t xi = 0; xi < 2; ++xi) {
                        glm::vec4 const clip(xi == 0 ? -1.0f : 1.0f, yi == 0 ? -1.0f : 1.0f, zi == 0 ? 0.0f : 1.0f, 1.0f);
                        glm::vec4 const world = inverse_view_proj * clip;
                        glm::vec3 const corner = glm::vec3(world) / world.w;
                        points[count++] = corner;
                        points[count++] = corner + light_dir * params.caster_extent;
                    }
                }
            }
            glm::vec3 min_ls(std::numeric_limits<float>::max());
            glm::vec3 max_ls(std::numeric_limits<float>::lowest());
            for (glm::vec3 const& point : points) {
                glm::vec3 const ls = glm::vec3(light_rotation * glm::vec4(point, 1.0f));
                min_ls = glm::min(min_ls, ls);
                max_ls = glm::max(max_ls, ls);
            }

            // merge the casters that can shadow this cascade. Casters outside the view still cast into
            // it (a wall behind the camera), so fitting only the frustum corners would clip them and
            // let sunlight leak: every caster whose light-space xy overlaps this cascade's footprint
            // contributes its depth range. Under the orthographic light a caster's shadow lands at the
            // caster's own light-space xy, so that overlap test is exact.
            float const cone_min_x = min_ls.x;
            float const cone_max_x = max_ls.x;
            float const cone_min_y = min_ls.y;
            float const cone_max_y = max_ls.y;
            for (auto const& [caster_min, caster_max] : params.caster_boxes) {
                bool const overlaps_view = caster_max.x >= cone_min_x && caster_min.x <= cone_max_x && caster_max.y >= cone_min_y && caster_min.y <= cone_max_y;
                if (!overlaps_view) {
                    continue; // this caster's shadow cannot land inside this cascade
                }
                // Only the merged DEPTH (light-space z) may grow past the cascade footprint: an
                // orthographic sun drops a caster's shadow at the caster's own light-space xy, and only
                // xy inside the view is ever sampled - so clamping the merged xy to the cone keeps the
                // box (and thus the texel density) as tight as the cascade fit, while the full z range
                // makes sure the caster's depth actually reaches the map. Merging the raw AABB instead
                // let one big floor slab inflate the box to the whole scene; leaving z alone clipped the
                // caster and leaked the sun through it.
                min_ls = glm::min(min_ls, glm::vec3(std::clamp(caster_min.x, cone_min_x, cone_max_x),
                                                    std::clamp(caster_min.y, cone_min_y, cone_max_y),
                                                    caster_min.z));
                max_ls = glm::max(max_ls, glm::vec3(std::clamp(caster_max.x, cone_min_x, cone_max_x),
                                                    std::clamp(caster_max.y, cone_min_y, cone_max_y),
                                                    caster_max.z));
            }
            if (params.unbounded_caster) {
                // A caster we cannot bound can be anywhere in the scene, so the only sound bound is
                // the whole scene sphere - but only its DEPTH may enter the fit: clamping the sphere's
                // xy to the cascade footprint keeps the box tight (the same argument as above), while
                // merging the raw sphere restored a box of scene size and made every thin caster's
                // shadow dissolve in the coarser texels.
                min_ls = glm::min(min_ls, glm::vec3(std::clamp(scene_sphere_ls.first.x, cone_min_x, cone_max_x),
                                                    std::clamp(scene_sphere_ls.first.y, cone_min_y, cone_max_y),
                                                    scene_sphere_ls.first.z));
                max_ls = glm::max(max_ls, glm::vec3(std::clamp(scene_sphere_ls.second.x, cone_min_x, cone_max_x),
                                                    std::clamp(scene_sphere_ls.second.y, cone_min_y, cone_max_y),
                                                    scene_sphere_ls.second.z));
            }

            // square the box (isotropic resolution), quantize its size to whole texels and snap its
            // center to the texel grid. Snapping the CENTER alone keeps the grid aligned while the
            // camera translates, but the box SIZE follows the fitted footprint continuously, so every
            // camera move rescaled the whole map and the shadow edges crawled anyway; rounding the size
            // up to a texel multiple means small moves keep the same texel size. The extra texel is
            // margin: without it the visible footprint touched the very edge of the map, where the
            // outermost half texel used to fall outside the box and casters there had no shadow edge.
            float const extent = std::max(max_ls.x - min_ls.x, max_ls.y - min_ls.y);
            float const half_raw = std::max(extent * 0.5f, 0.001f);
            float const texel_raw = (2.0f * half_raw) / map_size;
            float const half = std::max(std::ceil(half_raw / texel_raw) * texel_raw + texel_raw, 0.001f);
            float const texel = (2.0f * half) / map_size;
            glm::vec3 center_ls = (min_ls + max_ls) * 0.5f;
            center_ls.x = std::floor(center_ls.x / texel) * texel;
            center_ls.y = std::floor(center_ls.y / texel) * texel;
            glm::vec3 const center_ws = glm::vec3(glm::inverse(light_rotation) * glm::vec4(center_ls, 1.0f));

            // light camera far enough up-sun to see every fitted point, then fit near/far to the SAME
            // fitted range. Deriving near/far from the camera frustum corners alone clipped away every
            // caster merged above that sat further up-light than the corners: its depth never reached
            // the map, so the sun leaked straight through it. light_rotation is a pure rotation and
            // lookAt(.., -light_dir, ..) makes light-space z = dot(light_dir, p), so along the light
            // view (eye = center_ws + light_dir * distance) a point's depth is
            // center_ls.z + distance - ls_z - the extremes therefore come straight from min_ls/max_ls.z.
            //
            // distance comes from the fitted DEPTH SPAN, not from the scene radius: the eye has to sit
            // half a span up-light of the box centre for the box centre's plane to be in front of it,
            // and a scene-radius estimate can be smaller than that span (a deep fit pushed the up-light
            // end behind the eye, the near plane clamped to its 0.05 minimum and clipped those casters).
            float const half_span_z = 0.5f * (max_ls.z - min_ls.z);
            float const distance = half_span_z + std::max(1.0f, params.caster_extent);
            glm::vec3 const eye = center_ws + light_dir * distance;
            glm::mat4 const view = glm::lookAt(eye, center_ws, up);
            float const near_plane = std::max(distance + center_ls.z - max_ls.z - 1.0f, 0.05f);
            float const far_plane = distance + center_ls.z - min_ls.z + 1.0f;

            glm::mat4 proj = glm::orthoRH_ZO(-half, half, -half, half, near_plane, far_plane);
            proj[1][1] *= -1.0f; // same y-flip convention as the camera projection

            result.light_view_proj[cascade] = proj * view;
            result.cascade_texel_world[cascade] = (2.0f * half) / map_size;
            result.cascade_splits[cascade] = cascade_far;
            last_texel_world = (2.0f * half) / map_size;
        }

        // Unused cascade lanes must not hold garbage: a shader that samples a lane beyond the active
        // count (it does not - every path checks cascade_count first) would otherwise read whatever the
        // previous frame's fit left there. Repeating the last fitted matrix is the honest value.
        for (uint32_t cascade = cascades; cascade < max_shadow_cascades; ++cascade) {
            result.light_view_proj[cascade] = result.light_view_proj[cascades - 1];
            result.cascade_texel_world[cascade] = last_texel_world;
            result.cascade_splits[cascade] = result.cascade_splits[cascades - 1];
        }
        result.light_dir = light_dir;
        result.cascade_count = cascades;
        result.valid = true;

        if (params.log_summary) {
            deren::utility::log("shadow cascades: {} over the view range [{:.2f}, {:.2f}] (splits {:.2f}/{:.2f}/{:.2f}), texel world sizes {:.4f}/{:.4f}/{:.4f}/{:.4f}",
                                cascades,
                                split_near,
                                split_far,
                                result.cascade_splits[0],
                                result.cascade_splits[1],
                                result.cascade_splits[2],
                                result.cascade_texel_world[0],
                                result.cascade_texel_world[1],
                                result.cascade_texel_world[2],
                                result.cascade_texel_world[3]);
        }
        return result;
    }
} // namespace deren::vulkan::shadow_fit
