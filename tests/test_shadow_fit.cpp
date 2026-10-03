// Headless unit tests: deren.vulkan.shadow_fit (pure CPU) ===========================
// The cascade fit is arithmetic that decides whether shadows land on screen at all: it had no GPU
// dependency to begin with, it just lived inside deren.vulkan.runtime, where nothing could reach it. These
// tests pin the invariants its own comments claim - the per-cascade resolution and the depth range -
// without a device.
#include "vk_test.h"

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <utility>
#include <vector>

import deren.vulkan.shadow_fit;

namespace {
    constexpr float near_plane = 0.1f;
    constexpr float far_plane = 100.0f;

    // A camera at the origin looking down -z, an object 5 units away, and a sun from the side.
    // This is the shape the renderer's own orbit camera produces (RH_ZO, y-flip included), which
    // matters: the fit reads near/far out of the projection's z row.
    deren::vulkan::shadow_fit::fit_params make_params(uint32_t const cascades, float const scene_radius, bool const log_summary = false) {
        deren::vulkan::shadow_fit::fit_params params = {};
        params.proj = glm::perspective(glm::radians(45.0f), 1.0f, near_plane, far_plane);
        params.view = glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        params.camera_pos = glm::vec3(0.0f);
        params.light_dir = glm::vec4(glm::normalize(glm::vec3(0.4f, 0.8f, 0.3f)), 1.0f);
        params.scene_center = glm::vec3(0.0f, 0.0f, -5.0f);
        params.scene_radius = scene_radius;
        params.caster_extent = 1.0f;
        params.cascades = cascades;
        params.map_size = 2048;
        params.log_summary = log_summary;
        return params;
    }

    /// one caster centred at @p center with half-extent @p half
    std::pair<glm::vec3, glm::vec3> box_at(glm::vec3 const& center, float const half) {
        return {center - glm::vec3(half), center + glm::vec3(half)};
    }

    void test_degenerate_camera_is_rejected() {
        // no aspect yet (first frames) or a null projection term: the fit must refuse rather than
        // invert a singular matrix, which produced a light-space z span of 1631 in a real run
        deren::vulkan::shadow_fit::fit_params params = make_params(3, 1.5f);
        params.proj[2][2] = 0.0f;
        CHECK(!deren::vulkan::shadow_fit::fit(params).valid);

        deren::vulkan::shadow_fit::fit_params no_radius = make_params(3, 0.0f);
        CHECK(!deren::vulkan::shadow_fit::fit(no_radius).valid);

        // and the healthy case IS accepted, so the guard is not rejecting valid input
        CHECK(deren::vulkan::shadow_fit::fit(make_params(3, 1.5f)).valid);
    }

    // The documented reason the cascades exist: the near range gets a fraction of the texel size the
    // far range needs. This is the property that a hand-check on a screenshot established once
    // (0.0021 / 0.0056 / 0.0210 against 0.0161 for a single map); now it is an assertion.
    void test_texel_size_grows_with_distance() {
        deren::vulkan::shadow_fit::fit_result const fit = deren::vulkan::shadow_fit::fit(make_params(3, 1.5f));
        CHECK(fit.valid);
        CHECK(fit.cascade_count == 3);
        CHECK(fit.cascade_texel_world[0] < fit.cascade_texel_world[1]);
        CHECK(fit.cascade_texel_world[1] < fit.cascade_texel_world[2]);
        // every cascade is quantized to whole texels of its own box, so the size is strictly positive
        for (uint32_t i = 0; i < fit.cascade_count; ++i) {
            CHECK(fit.cascade_texel_world[i] > 0.0f);
        }
    }

    void test_splits_are_monotonic_and_end_at_the_usable_far() {
        deren::vulkan::shadow_fit::fit_params params = make_params(3, 1.5f);
        deren::vulkan::shadow_fit::fit_result const fit = deren::vulkan::shadow_fit::fit(params);
        CHECK(fit.valid);

        // the practical split scheme blends logarithmic and uniform, so the splits increase
        CHECK(fit.cascade_splits[0] > near_plane);
        CHECK(fit.cascade_splits[0] < fit.cascade_splits[1]);
        CHECK(fit.cascade_splits[1] < fit.cascade_splits[2]);
        // clipped at the scene sphere: |eye - centre| + radius = 5 + 1.5, well inside the camera's
        // deliberately generous far plane - fitting the WHOLE camera range is the bug this prevents
        float const expected_far = 5.0f + 1.5f;
        CHECK(std::abs(fit.cascade_splits[2] - expected_far) < 0.01f);
    }

    // The scene sphere clips the camera's far plane, so a deeper scene reaches further: this is the
    // difference between "fit the view volume" and "fit the part of it that can hold geometry".
    void test_far_split_follows_the_scene_not_the_camera() {
        deren::vulkan::shadow_fit::fit_result const near_scene = deren::vulkan::shadow_fit::fit(make_params(3, 1.5f));
        deren::vulkan::shadow_fit::fit_params far_params = make_params(3, 6.0f);
        far_params.scene_center = glm::vec3(0.0f, 0.0f, -20.0f);
        deren::vulkan::shadow_fit::fit_result const far_scene = deren::vulkan::shadow_fit::fit(far_params);
        CHECK(near_scene.valid && far_scene.valid);
        CHECK(far_scene.cascade_splits[2] > near_scene.cascade_splits[2]);
        // and still refuses to exceed the camera's own far plane
        CHECK(far_scene.cascade_splits[2] <= far_plane);
    }

    // The whole point of the fit: the box a cascade was fitted to must actually contain it, or shadows
    // are clipped away and the sun leaks through. A cascade covers a DEPTH SLICE of the view, so what
    // must be inside its box is the sub-frustum for that slice - not the whole camera frustum.
    //
    // Two earlier versions of this test asserted the wrong thing and failed, which is worth recording
    // because both are natural mistakes: the scene CENTRE (a point at one depth, which legitimately
    // falls outside a slice that does not cover it) and the ENTIRE camera frustum (whose far corners
    // are 100 units away, nowhere near the near cascade's slice).
    void test_each_cascade_contains_its_own_depth_slice() {
        deren::vulkan::shadow_fit::fit_params params = make_params(3, 1.5f);
        std::vector<std::pair<glm::vec3, glm::vec3>> const casters = {box_at(glm::vec3(0.0f, 0.0f, -5.0f), 1.0f)};
        bool unbounded = false;
        std::vector<std::pair<glm::vec3, glm::vec3>> const light_boxes = deren::vulkan::shadow_fit::fit_casters(casters, glm::vec3(params.light_dir), unbounded);
        params.caster_boxes = light_boxes;
        CHECK(!unbounded);

        deren::vulkan::shadow_fit::fit_result const fit = deren::vulkan::shadow_fit::fit(params);
        CHECK(fit.valid);

        // the camera frustum's corners: unproject the clip-space cube (z 0..1 is the RH_ZO depth range)
        glm::mat4 const inverse_view_proj = glm::inverse(params.proj * params.view);
        std::vector<glm::vec3> frustum_corners;
        for (int32_t zi = 0; zi < 2; ++zi) {
            for (int32_t yi = 0; yi < 2; ++yi) {
                for (int32_t xi = 0; xi < 2; ++xi) {
                    glm::vec4 const clip(xi == 0 ? -1.0f : 1.0f, yi == 0 ? -1.0f : 1.0f, zi == 0 ? 0.0f : 1.0f, 1.0f);
                    glm::vec4 const world = inverse_view_proj * clip;
                    frustum_corners.push_back(glm::vec3(world) / world.w);
                }
            }
        }

        std::size_t checked = 0;
        for (uint32_t i = 0; i < fit.cascade_count; ++i) {
            // the slice's depth band in world units along the view direction. The camera looks down
            // -z, so a corner's distance is simply -z.
            float const slice_near = (i == 0) ? 0.0f : fit.cascade_splits[i - 1];
            float const slice_far = fit.cascade_splits[i];
            for (glm::vec3 const& corner : frustum_corners) {
                float const distance = -corner.z;
                if (distance < slice_near || distance > slice_far) {
                    continue; // this corner belongs to another cascade's slice
                }
                ++checked;
                glm::vec4 const clip = fit.light_view_proj[i] * glm::vec4(corner, 1.0f);
                CHECK(clip.w > 0.0f); // in front of the light camera
                glm::vec3 const ndc = glm::vec3(clip) / clip.w;
                CHECK(std::abs(ndc.x) <= 1.0f);
                CHECK(std::abs(ndc.y) <= 1.0f);
            }
        }
        // the near slice genuinely contains corners, so this is not passing vacuously
        CHECK(checked > 0);
    }

    // A caster cannot inflate the box sideways: under an orthographic sun a shadow lands at the
    // caster's own light-space xy, and only xy inside the view is sampled - so a big caster far
    // outside the cascade must not coarsen it (merging its raw AABB is the bug that dissolved thin
    // shadows). It may only contribute DEPTH.
    void test_distant_caster_does_not_coarsen_the_cascade() {
        deren::vulkan::shadow_fit::fit_params base = make_params(1, 1.5f);
        bool unbounded = false;
        std::vector<std::pair<glm::vec3, glm::vec3>> const in_view = {box_at(glm::vec3(0.0f, 0.0f, -5.0f), 0.5f)};
        // named locals, not an inline call: fit_casters returns by value, so binding the span to the
        // temporary would dangle by the time fit() reads it (and -Werror says so)
        std::vector<std::pair<glm::vec3, glm::vec3>> const base_boxes = deren::vulkan::shadow_fit::fit_casters(in_view, glm::vec3(base.light_dir), unbounded);
        base.caster_boxes = base_boxes;
        deren::vulkan::shadow_fit::fit_result const tight = deren::vulkan::shadow_fit::fit(base);

        // same fit, plus a caster 1000 units away in x (its light-space xy is far outside the cone)
        std::vector<std::pair<glm::vec3, glm::vec3>> const with_far = {in_view[0], box_at(glm::vec3(1000.0f, 0.0f, -5.0f), 5.0f)};
        deren::vulkan::shadow_fit::fit_params widened = make_params(1, 1.5f);
        std::vector<std::pair<glm::vec3, glm::vec3>> const widened_boxes = deren::vulkan::shadow_fit::fit_casters(with_far, glm::vec3(widened.light_dir), unbounded);
        widened.caster_boxes = widened_boxes;
        deren::vulkan::shadow_fit::fit_result const same = deren::vulkan::shadow_fit::fit(widened);

        CHECK(tight.valid && same.valid);
        CHECK(std::abs(same.cascade_texel_world[0] - tight.cascade_texel_world[0]) < 1e-5f);
    }

    // A caster we cannot bound falls back to the whole scene sphere. What that fallback does is easy to
    // get wrong in the head, so this pins the actual contract rather than an assumed one:
    //
    // It adds DEPTH ONLY. Clamping the sphere's xy to the cascade footprint means the LIGHT-SPACE BOX
    // does not grow sideways at all - animating an unbounded caster therefore cannot change the shadow
    // resolution, which is the property the later "only the merged DEPTH may grow past the cascade
    // footprint" comment promises. An earlier version of this test asserted the texel size grows; it
    // does not, and that is the design working.
    //
    // The consequence that does change is what the light camera must be able to see: the fitted z span
    // has to reach the sphere's own depth extremes, or the unbounded caster's shadow is clipped away
    // and the sun leaks through - the exact failure the fallback exists to prevent.
    void test_unbounded_caster_extends_only_the_depth_range() {
        deren::vulkan::shadow_fit::fit_params params = make_params(1, 5.0f);
        std::vector<std::pair<glm::vec3, glm::vec3>> const in_view = {box_at(glm::vec3(0.0f, 0.0f, -5.0f), 0.2f)};
        bool unbounded = false;
        std::vector<std::pair<glm::vec3, glm::vec3>> const boxes = deren::vulkan::shadow_fit::fit_casters(in_view, glm::vec3(params.light_dir), unbounded);
        params.caster_boxes = boxes;
        deren::vulkan::shadow_fit::fit_result const with_bounds = deren::vulkan::shadow_fit::fit(params);

        params.unbounded_caster = true;
        deren::vulkan::shadow_fit::fit_result const with_sphere = deren::vulkan::shadow_fit::fit(params);

        CHECK(with_bounds.valid && with_sphere.valid);
        // sideways: unchanged (xy is clamped to the cascade footprint either way)
        CHECK(std::abs(with_sphere.cascade_texel_world[0] - with_bounds.cascade_texel_world[0]) < 1e-5f);

        // depth: the scene sphere's extremes along the light now fall inside the light camera's fitted
        // span. light space is a rotation whose z is dot(light_dir, p) - the same relation the fit's
        // near/far derivation relies on - so the light camera's view-space z of a point p is
        // dot(light_dir, centre) - dot(light_dir, p) relative to the box centre, which the projection
        // maps into [0, 1].
        glm::vec3 const light_dir = glm::normalize(glm::vec3(params.light_dir));
        auto ndc_z = [&](glm::vec3 const& p) {
            glm::vec4 const clip = with_sphere.light_view_proj[0] * glm::vec4(p, 1.0f);
            return clip.z / clip.w;
        };
        // the two points of the scene sphere furthest along +/- the light direction
        glm::vec3 const near_extreme = params.scene_center - light_dir * params.scene_radius;
        glm::vec3 const far_extreme = params.scene_center + light_dir * params.scene_radius;
        for (glm::vec3 const& extreme : {near_extreme, far_extreme}) {
            float const z = ndc_z(extreme);
            CHECK(z >= 0.0f);
            CHECK(z <= 1.0f);
        }
    }

    // Lanes past the active cascade count repeat the last fitted one: the shader checks cascade_count
    // first, but a stale lane must not be garbage either way.
    void test_unused_cascade_lanes_repeat_the_last_fit() {
        deren::vulkan::shadow_fit::fit_result const fit = deren::vulkan::shadow_fit::fit(make_params(2, 1.5f));
        CHECK(fit.valid);
        CHECK(fit.cascade_count == 2);
        for (uint32_t i = 2; i < deren::vulkan::max_shadow_cascades; ++i) {
            CHECK(fit.light_view_proj[i] == fit.light_view_proj[1]);
            CHECK(fit.cascade_splits[i] == fit.cascade_splits[1]);
            CHECK(fit.cascade_texel_world[i] == fit.cascade_texel_world[1]);
        }
    }

    // The single-cascade case is the historic behavior and must stay a working fit, not a special
    // path that silently degrades.
    void test_single_cascade_covers_the_usable_range() {
        deren::vulkan::shadow_fit::fit_result const fit = deren::vulkan::shadow_fit::fit(make_params(1, 1.5f));
        CHECK(fit.valid);
        CHECK(fit.cascade_count == 1);
        CHECK(fit.cascade_splits[0] > 6.0f); // the whole usable range in one box
        CHECK(fit.light_view_proj[0] != glm::mat4(0.0f));
    }

    // fit_casters rotates world AABBs into light space; the result must be the light-space extent of
    // the same box, which for an axis-aligned box seen along an axis is just a permutation.
    void test_fit_casters_projects_into_light_space() {
        std::vector<std::pair<glm::vec3, glm::vec3>> const world = {box_at(glm::vec3(2.0f, 0.0f, -3.0f), 1.0f)};
        bool unbounded = true;
        // light straight down -z: light space keeps x/y and measures depth along -z
        std::vector<std::pair<glm::vec3, glm::vec3>> const light = deren::vulkan::shadow_fit::fit_casters(world, glm::vec3(0.0f, 0.0f, -1.0f), unbounded);
        CHECK(!unbounded);
        CHECK(light.size() == 1);
        if (light.size() == 1) {
            // the projected extent has the same size on every axis (a rotation cannot change lengths)
            float const world_size = 2.0f;
            CHECK(std::abs((light[0].second.x - light[0].first.x) - world_size) < 1e-4f);
            CHECK(std::abs((light[0].second.y - light[0].first.y) - world_size) < 1e-4f);
            CHECK(std::abs((light[0].second.z - light[0].first.z) - world_size) < 1e-4f);
        }
    }
} // namespace

int32_t main() {
    test_degenerate_camera_is_rejected();
    test_texel_size_grows_with_distance();
    test_splits_are_monotonic_and_end_at_the_usable_far();
    test_far_split_follows_the_scene_not_the_camera();
    test_each_cascade_contains_its_own_depth_slice();
    test_distant_caster_does_not_coarsen_the_cascade();
    test_unbounded_caster_extends_only_the_depth_range();
    test_unused_cascade_lanes_repeat_the_last_fit();
    test_single_cascade_covers_the_usable_range();
    test_fit_casters_projects_into_light_space();
    return deren::vk_test::finish("test_shadow_fit");
}
