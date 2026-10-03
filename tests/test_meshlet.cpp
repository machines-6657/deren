// Headless unit tests: deren.vulkan.meshlet (pure CPU) ==============================================
// The meshlet split is the one part of step 3 that can be checked WITHOUT a device, and it is the part whose
// bugs are invisible on screen: a meshlet whose bounding sphere is too small is culled while it is visible (a
// hole in the shadow map, on the frames where it happens to face the light), and a meshlet whose index window
// overlaps its neighbour draws triangles twice (invisible for a depth pass, wrong for anything blended). The
// capture gate cannot run in CI at all, so the three properties that make the split usable are asserted here:
//
//   1. COVERAGE - the meshlets partition the window's whole triangles, in order, with no overlap and no gap;
//   2. CONSERVATIVE BOUNDS - every vertex a meshlet indexes is inside its sphere (a box's corner sphere
//      contains the box, so this is what makes "culled" imply "nothing visible was in it");
//   3. LIMITS AND DETERMINISM - at most `meshlet_max_triangles` triangles each, and the same input gives the
//      same output twice (the table a task stage reads must be build-stable, or two frames disagree).
//
// The malformed inputs are asserted to produce an EMPTY result rather than a crash or a read past a buffer: the
// primitive upload validates its windows, so a bad window reaching this far is a second line of defence.
#include "vk_test.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

import deren.vulkan.meshlet;

namespace {
    /// an interleaved vertex array with a position first (the renderer's layout, without the other attributes)
    struct mesh_fixture {
        std::vector<uint8_t> vertices;
        std::vector<uint8_t> indices;
        uint32_t vertex_stride = 0;
        uint32_t vertex_count = 0;
        uint32_t index_width = 4;
    };

    /// a strip of @p count triangles: 3*count+... vertices spread over a helix, so no two meshlets have the
    /// same bounds and a too-small radius is visible in the checks rather than cancelled by symmetry
    mesh_fixture make_fixture(uint32_t const triangle_count, uint32_t const index_width = 4) {
        mesh_fixture fixture;
        fixture.vertex_stride = 16u; // position (12) + a 4-byte pad: the layout reader must tolerate extra data
        fixture.vertex_count = triangle_count * 3u;
        fixture.index_width = index_width;
        fixture.vertices.resize(static_cast<std::size_t>(fixture.vertex_count) * fixture.vertex_stride);
        float* const positions = reinterpret_cast<float*>(fixture.vertices.data());
        for (uint32_t v = 0; v < fixture.vertex_count; ++v) {
            float* const vertex = positions + static_cast<std::size_t>(v) * 4u;
            float const t = static_cast<float>(v);
            vertex[0] = std::cos(t * 0.7f) * (1.0f + t * 0.01f);
            vertex[1] = std::sin(t * 0.7f) * (1.0f + t * 0.01f);
            vertex[2] = t * 0.05f;
        }
        fixture.indices.resize(static_cast<std::size_t>(triangle_count) * 3u * index_width);
        for (uint32_t i = 0; i < triangle_count * 3u; ++i) {
            if (index_width == 2u) {
                uint16_t const value = static_cast<uint16_t>(i);
                std::memcpy(fixture.indices.data() + static_cast<std::size_t>(i) * 2u, &value, sizeof(value));
            } else {
                uint32_t const value = i;
                std::memcpy(fixture.indices.data() + static_cast<std::size_t>(i) * 4u, &value, sizeof(value));
            }
        }
        return fixture;
    }

    deren::vulkan::meshlet_build_input input_for(mesh_fixture const& fixture, uint32_t const first_index = 0, uint32_t const index_count = 0, int32_t const base_vertex = 0) {
        return deren::vulkan::meshlet_build_input{
            .vertex_data = std::span<uint8_t const>(fixture.vertices),
            .vertex_stride = fixture.vertex_stride,
            .vertex_count = fixture.vertex_count,
            .index_data = std::span<uint8_t const>(fixture.indices),
            .index_width = fixture.index_width,
            .first_index = first_index,
            .index_count = index_count == 0u ? fixture.vertex_count : index_count,
            .base_vertex = base_vertex,
        };
    }

    /// the position vertex @p v holds, read the way the splitter reads it
    void position_of(mesh_fixture const& fixture, uint32_t const v, float (&out)[3]) {
        std::memcpy(out, fixture.vertices.data() + static_cast<std::size_t>(v) * fixture.vertex_stride, sizeof(out));
    }
} // namespace

int32_t main() {
    // ---- 1. a window of exactly one meshlet, and one that needs three ----
    {
        mesh_fixture const exact = make_fixture(deren::vulkan::meshlet_max_triangles);
        std::vector<deren::vulkan::meshlet> const one = deren::vulkan::build_meshlets(input_for(exact));
        CHECK_MSG(one.size() == 1u, "85 triangles fit one meshlet");
        CHECK(one[0].first_index == 0u);
        CHECK(one[0].index_count == deren::vulkan::meshlet_max_indices);
        CHECK(one[0].radius > 0.0f);

        mesh_fixture const three = make_fixture(deren::vulkan::meshlet_max_triangles * 2u + 5u);
        std::vector<deren::vulkan::meshlet> const split = deren::vulkan::build_meshlets(input_for(three));
        CHECK_MSG(split.size() == 3u, "175 triangles need three meshlets");
        CHECK(split[0].index_count == deren::vulkan::meshlet_max_indices);
        CHECK(split[1].index_count == deren::vulkan::meshlet_max_indices);
        CHECK(split[2].index_count == 5u * 3u);

        // COVERAGE, exactly: the windows are contiguous, in order, and cover the whole triangle count
        uint32_t expected_first = 0;
        for (deren::vulkan::meshlet const& m : split) {
            CHECK(m.first_index == expected_first);
            CHECK(m.index_count % 3u == 0u);
            CHECK(m.index_count != 0u);
            CHECK(m.index_count <= deren::vulkan::meshlet_max_indices);
            expected_first += m.index_count;
        }
        CHECK_MSG(expected_first == three.vertex_count, "the meshlets cover every index of the window");

        // DETERMINISM: the same input twice, byte for byte
        std::vector<deren::vulkan::meshlet> const again = deren::vulkan::build_meshlets(input_for(three));
        CHECK(again.size() == split.size());
        CHECK(std::memcmp(again.data(), split.data(), split.size() * sizeof(deren::vulkan::meshlet)) == 0);
    }

    // ---- 2. CONSERVATIVE BOUNDS: every vertex of every meshlet is inside its sphere ----
    {
        mesh_fixture const fixture = make_fixture(200u);
        for (uint32_t const width : {2u, 4u}) {
            mesh_fixture const typed = make_fixture(200u, width);
            std::vector<deren::vulkan::meshlet> const meshlets = deren::vulkan::build_meshlets(input_for(typed));
            CHECK(!meshlets.empty());
            uint32_t checked_vertices = 0;
            for (deren::vulkan::meshlet const& m : meshlets) {
                for (uint32_t i = m.first_index; i < m.first_index + m.index_count; ++i) {
                    uint32_t index = 0;
                    if (typed.index_width == 2u) {
                        uint16_t value = 0;
                        std::memcpy(&value, typed.indices.data() + static_cast<std::size_t>(i) * 2u, sizeof(value));
                        index = value;
                    } else {
                        std::memcpy(&index, typed.indices.data() + static_cast<std::size_t>(i) * 4u, sizeof(index));
                    }
                    float position[3] = {};
                    position_of(typed, index, position);
                    float const dx = position[0] - m.center_x;
                    float const dy = position[1] - m.center_y;
                    float const dz = position[2] - m.center_z;
                    float const distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    // a small tolerance: the radius is computed from the same floats, and this is a check that a
                    // too-small sphere is caught, not a rounding study
                    CHECK_MSG(distance <= m.radius + 1e-4f, "a meshlet's vertex lies inside its bounding sphere");
                    ++checked_vertices;
                }
            }
            CHECK_MSG(checked_vertices == 200u * 3u, "every triangle's three vertices were checked");
            // ... and the sphere is not absurdly large either: a meshlet of 85 sequential vertices on this helix
            // spans a small part of it, so a radius near the whole mesh's extent would mean the box was not reset
            CHECK_MSG(meshlets[0].radius < 60.0f, "the first meshlet's radius is its own extent, not the mesh's");
        }
        (void)fixture;
    }

    // ---- 3. base_vertex and first_index are carried through ----
    {
        mesh_fixture const fixture = make_fixture(4u);
        std::vector<deren::vulkan::meshlet> const tail = deren::vulkan::build_meshlets(input_for(fixture, 3u, 6u, -7));
        CHECK_MSG(tail.size() == 1u, "six indices are two triangles");
        CHECK(tail[0].first_index == 0u); // relative to the window, which is what the mesh stage adds to
        CHECK(tail[0].index_count == 6u);
        CHECK(tail[0].base_vertex == -7);
    }

    // ---- 4. DEGENERATE INPUTS are empty results, never crashes or out-of-range reads ----
    {
        mesh_fixture fixture = make_fixture(10u);
        // no triangles
        CHECK(deren::vulkan::build_meshlets(input_for(fixture, 0u, 2u)).empty());
        // ... and an index_count the splitter's caller could not have meant: the input struct's `0` means "the
        // whole array" (see input_for), so this case is spelled with an explicitly empty index span instead
        deren::vulkan::meshlet_build_input empty_indices = input_for(fixture);
        empty_indices.index_data = {};
        CHECK(deren::vulkan::build_meshlets(empty_indices).empty());
        // no vertices
        deren::vulkan::meshlet_build_input no_vertices = input_for(fixture);
        no_vertices.vertex_count = 0u;
        CHECK(deren::vulkan::build_meshlets(no_vertices).empty());
        // a stride shorter than one position
        deren::vulkan::meshlet_build_input short_stride = input_for(fixture);
        short_stride.vertex_stride = 8u;
        CHECK(deren::vulkan::build_meshlets(short_stride).empty());
        // a window that runs past the index buffer
        deren::vulkan::meshlet_build_input past_end = input_for(fixture);
        past_end.index_count = fixture.vertex_count + 30u;
        CHECK(deren::vulkan::build_meshlets(past_end).empty());
        // ... and an index that points past the vertices: the window is still covered (the meshlets exist, so the
        // draw is not silently dropped) and the bogus vertex simply does not extend the bounds
        deren::vulkan::meshlet_build_input bad_index = input_for(fixture);
        uint32_t const bogus = fixture.vertex_count + 100u;
        std::memcpy(fixture.indices.data(), &bogus, sizeof(bogus)); // the span bad_index holds points at this vector
        std::vector<deren::vulkan::meshlet> const fallback = deren::vulkan::build_meshlets(bad_index);
        CHECK_MSG(fallback.size() == 1u, "an out-of-range index does not drop the meshlet");
        CHECK(std::isfinite(fallback[0].radius));
    }

    // ---- 5. THE RECORD RULE the upload applies, asserted here so the two cannot drift ----
    // `runtime::create_primitive` checks every record it copies into the GPU table with this function, and the
    // shader downstream trusts the result: a MESH stage hands `index_count` to `SetMeshOutputCounts`, so a record
    // that breaks the rule is a dispatch asking the device for output it does not have (the first consumer attempt
    // hung the GPU that way rather than drawing something wrong).
    {
        mesh_fixture const fixture = make_fixture(200u);
        std::vector<deren::vulkan::meshlet> const meshlets = deren::vulkan::build_meshlets(input_for(fixture));
        CHECK(!meshlets.empty());
        for (deren::vulkan::meshlet const& m : meshlets) {
            CHECK_MSG(deren::vulkan::meshlet_record_sound(m, fixture.vertex_count), "the splitter's own records pass the upload's rule");
        }
        deren::vulkan::meshlet const good = meshlets.front();

        deren::vulkan::meshlet zero_count = good;
        zero_count.index_count = 0u;
        CHECK(!deren::vulkan::meshlet_record_sound(zero_count, fixture.vertex_count)); // an empty meshlet

        deren::vulkan::meshlet over_budget = good;
        over_budget.index_count = deren::vulkan::meshlet_max_indices + 3u;
        CHECK(!deren::vulkan::meshlet_record_sound(over_budget, fixture.vertex_count)); // more output than the device has

        deren::vulkan::meshlet partial_triangle = good;
        partial_triangle.index_count = good.index_count - 1u;
        CHECK(!deren::vulkan::meshlet_record_sound(partial_triangle, fixture.vertex_count)); // not whole triangles

        deren::vulkan::meshlet past_window = good;
        past_window.first_index = fixture.vertex_count;
        CHECK(!deren::vulkan::meshlet_record_sound(past_window, fixture.vertex_count)); // outside the draw

        deren::vulkan::meshlet bad_radius = good;
        bad_radius.radius = -1.0f;
        CHECK(!deren::vulkan::meshlet_record_sound(bad_radius, fixture.vertex_count));

        // ... and a draw that is SHORTER than the window the splitter saw is rejected too: the rule is relative to
        // the draw, which is why it takes the index count rather than assuming it
        CHECK(!deren::vulkan::meshlet_record_sound(good, good.index_count - 3u));
    }

    // ---- 6. THE NORMAL CONE, asserted by its defining property ----
    // A culler may only reject a meshlet as back-facing if EVERY face normal it holds lies inside the cone the
    // record declares - so that is what is checked here, against the same triangles the splitter was given. A cone
    // that is too narrow would cull a meshlet that is partly visible (a hole), and one that is too wide only costs
    // culls; both directions of that trade are why the axis is the normal AVERAGE and `cone_cos` the worst dot.
    {
        mesh_fixture const fixture = make_fixture(200u);
        std::vector<deren::vulkan::meshlet> const meshlets = deren::vulkan::build_meshlets(input_for(fixture));
        CHECK(!meshlets.empty());
        uint32_t cone_checked = 0;
        for (deren::vulkan::meshlet const& m : meshlets) {
            CHECK(m.cone_cos <= 1.0f && m.cone_cos >= -1.0f);
            for (uint32_t t = 0; t < m.index_count; t += 3u) {
                float a[3] = {};
                float b[3] = {};
                float c[3] = {};
                auto const read_position = [&fixture](uint32_t const index, float (&out)[3]) {
                    std::memcpy(out, fixture.vertices.data() + static_cast<std::size_t>(index) * fixture.vertex_stride, sizeof(out));
                };
                // THE WINDOW STARTS AT first_index, not at zero: the fixture's indices are 0..n-1 in order, so an
                // index IS the offset of the vertex it names - reading from `t` alone checks the WRONG triangles
                // for every meshlet after the first, which is how this test failed the first time it ran.
                read_position(m.first_index + t + 0u, a);
                read_position(m.first_index + t + 1u, b);
                read_position(m.first_index + t + 2u, c);
                float const e0[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                float const e1[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                float const n[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2], e0[0] * e1[1] - e0[1] * e1[0]};
                float const length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (length <= 0.0f) {
                    continue; // a degenerate triangle states nothing, and the splitter skips it too
                }
                float const dot = (n[0] * m.axis_x + n[1] * m.axis_y + n[2] * m.axis_z) / length;
                CHECK_MSG(dot >= m.cone_cos - 1e-4f, "every face normal lies inside the cone");
                ++cone_checked;
            }
        }
        CHECK_MSG(cone_checked != 0u, "the cone was checked against real triangles");
    }

    return deren::vk_test::finish("test_meshlet");
}
