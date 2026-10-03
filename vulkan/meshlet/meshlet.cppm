/**
 * @file meshlet.cppm
 * @defgroup vulkan_meshlet Meshlet split
 * @brief cut a draw's index window into MESHLETS - triangle runs small enough for one mesh workgroup to emit,
 *        each with an object-space bounding sphere - so a task stage can decide per meshlet whether to run it
 *        at all (docs/mesh_shaders.md step 3).
 *
 * WHY THIS IS A MODULE OF ITS OWN, and a PURE one: the split is arithmetic over the vertex and index bytes, it
 * needs no device, no allocator and no Vulkan type, and getting it wrong is invisible on screen in exactly the
 * way a bad bounding volume always is (a meshlet culled while it is visible is a hole; a meshlet that is NOT
 * culled is just work). Keeping it free of dependencies is what lets the headless tests check the three
 * properties that make it usable - full coverage, conservative bounds, deterministic order - on the CPU, where
 * CI can run them, instead of through a capture that only shows the holes that happen to face the camera.
 *
 * IT DELIBERATELY DOES NOT DECIDE WHAT A MESHLET IS FOR. It emits the same triangles the input window describes,
 * in the input's order, split into runs of at most `meshlet_max_triangles`: what a consumer does with them
 * (cull, reorder, deduplicate vertices, build a meshlet-local index buffer) is the consumer's business, and this
 * file's contract is only that the union of its output is the input.
 */
module;

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

export module deren.vulkan.meshlet;

export import deren.vstd;

namespace deren::vulkan {
    /**
     * @ingroup vulkan_meshlet
     * @brief the triangles one mesh workgroup emits, i.e. the largest run a meshlet may contain
     *
     * 85 is a DEVICE LIMIT and not a tuning choice: `maxMeshOutputVertices` and `maxMeshOutputPrimitives` are
     * both 256 on the device this renderer targets, a mesh stage that writes each triangle's three vertices
     * separately (which is what a stage without meshlet-local index sharing does) spends three output vertices
     * per triangle, and 85 * 3 = 255 is the largest count that fits both. See docs/mesh_shaders.md section 2.
     */
    export constexpr uint32_t meshlet_max_triangles = 85;
    /// the index count that bound implies (three per triangle)
    export constexpr uint32_t meshlet_max_indices = meshlet_max_triangles * 3u;
    /**
     * @ingroup vulkan_meshlet
     * @brief how many meshlets the GPU table holds, i.e. the renderer's whole-scene meshlet budget
     *
     * @note 65536 records of 48 bytes is 3 MiB, which is nothing next to the geometry it describes, and it
     *       covers the heaviest scene this renderer is tested against by a wide margin: the Sponza import cuts
     *       3145 meshlets out of 103 primitives (measured, docs/mesh_shaders.md step 3). A scene past the capacity
     *       keeps its GEOMETRY and loses the meshlet path's culling for the overflow: the upload clamps and says
     *       so, in the same spirit as the material table's overflow path - a renderer that drops geometry when a
     *       budget runs out turns a budget into a hole.
     */
    export constexpr uint32_t meshlet_capacity = 65536u;

    /**
     * @ingroup vulkan_meshlet
     * @brief one meshlet: a window of the draw's index buffer plus the sphere its vertices occupy
     * @note the sphere is in OBJECT space (the space the vertex bytes are in), so a consumer transforms its
     *       centre by the same model/instance matrix it projects the vertices with, and scales the radius by
     *       that matrix's largest axis scale. `radius` covers the window's axis-aligned box, which is what makes
     *       it CONSERVATIVE: a box's corner sphere contains the box, so a meshlet that is culled cannot contain
     *       a vertex outside the plane that culled it.
     */
    export struct meshlet {
        uint32_t first_index = 0; ///< first index of this meshlet inside the draw's index window
        uint32_t index_count = 0; ///< indices it covers (always a multiple of three, never zero)
        int32_t base_vertex = 0;  ///< the draw's base vertex, repeated per meshlet: it is added to every index
        /**
         * @brief padding, and it is a MEASURED requirement rather than tidiness: the GPU-side struct is a Vulkan
         *        std430 `StructuredBuffer` element, where a `float3` has a 16-byte ALIGNMENT - so the shader puts
         *        `center` at byte 16 and `radius` at 28, and a 28-byte host record (center at 12, radius at 24) made
         *        every record after the first read four bytes off, with the sphere fields landing in the wrong
         *        members entirely. The symptom was not a wrong picture but a WEDGED GPU: the window and base vertex
         *        came out of the wrong bytes, the index fetch left the buffer, and the device never signalled another
         *        fence (validation reported only the reuse of a command buffer whose work had not finished).
         */
        uint32_t _pad = 0;
        float center_x = 0.0f; ///< object-space centre of the bounding sphere
        float center_y = 0.0f;
        float center_z = 0.0f;
        float radius = 0.0f; ///< object-space radius (0 for a single vertex or a fully degenerate window)
        /**
         * THE NORMAL CONE, for culling a meshlet that faces entirely away from the eye (docs/mesh_shaders.md step
         * 3): `axis` is the normalized average of the window's triangle normals and `cone_cos` the smallest dot
         * product any of them has with it, so every face normal is within `acos(cone_cos)` of the axis. A culler
         * can then reject the whole meshlet when even the cone's BEST-case normal points away from the eye, which
         * is the conservative form of a back-face test: it errs towards drawing, like every other test here.
         *
         * @note a degenerate window (no non-zero-area triangle) sets `axis = 0` and `cone_cos = 1`, which the
         *       test reads as "no statement about this meshlet's facing" and therefore never culls it.
         */
        float axis_x = 0.0f;
        float axis_y = 0.0f;
        float axis_z = 0.0f;
        float cone_cos = 1.0f;
    };
    // THE LAYOUT IS THE SHADER'S, asserted field by field, because it has to survive a look at the SPIR-V rather
    // than a hope: `spirv-dis shadow.meshlet.spv` shows exactly these offsets (0, 4, 8, 16 with the sphere at
    // 16/20/24 and the radius at 28, stride 32). A host record that disagrees is a GPU fault, not a wrong pixel.
    static_assert(offsetof(meshlet, first_index) == 0);
    static_assert(offsetof(meshlet, index_count) == 4);
    static_assert(offsetof(meshlet, base_vertex) == 8);
    static_assert(offsetof(meshlet, center_x) == 16);
    static_assert(offsetof(meshlet, center_y) == 20);
    static_assert(offsetof(meshlet, center_z) == 24);
    static_assert(offsetof(meshlet, radius) == 28);
    static_assert(offsetof(meshlet, axis_x) == 32);
    static_assert(offsetof(meshlet, cone_cos) == 44);
    static_assert(sizeof(meshlet) == 48, "a meshlet is 48 bytes: the window, the sphere, and the normal cone - each 16-byte block matching the shader's std430 layout");

    /**
     * @ingroup vulkan_meshlet
     * @brief whether @p record is a window a MESH stage can act on inside a draw of @p index_count indices
     *
     * THE RULE LIVES HERE, not in the upload and not in the shader, because two places need the same answer and
     * neither can afford to guess: `runtime::create_primitive` checks every record as it leaves the host, and
     * tests/test_meshlet.cpp asserts that the check rejects what it should. A record that breaks it is not a wrong
     * PICTURE - a MESH stage hands `index_count` to `SetMeshOutputCounts` and reads its indices with the rest, so a
     * malformed one is a dispatch asking the device for output it does not have (measured: a first consumer attempt
     * with a table read at the wrong index hung the GPU rather than drawing something wrong).
     *
     * @param record the meshlet record, as the splitter produced it or as the GPU table would return it
     * @param index_count how many indices the draw it belongs to covers
     */
    export bool meshlet_record_sound(meshlet const& record, uint32_t index_count) noexcept {
        return record.index_count != 0u && record.index_count % 3u == 0u && record.index_count <= meshlet_max_indices &&
               record.first_index + record.index_count <= index_count && std::isfinite(record.radius) && record.radius >= 0.0f;
    }

    /**
     * @ingroup vulkan_meshlet
     * @brief the geometry one draw window is split over, exactly as the primitive upload hands it over
     * @note the index bytes are read with `index_width` (2 or 4) and the vertex bytes with `vertex_stride`, so
     *       the split sees the same bytes the mesh stage will fetch - no second interpretation of the layout.
     */
    export struct meshlet_build_input {
        std::span<uint8_t const> vertex_data = {}; ///< the interleaved vertices, position first
        uint32_t vertex_stride = 0;                ///< bytes per vertex (64 for this renderer's layout)
        uint32_t vertex_count = 0;                 ///< vertices the data holds
        std::span<uint8_t const> index_data = {};  ///< the index buffer the window indexes into
        uint32_t index_width = 4;                  ///< bytes per index: 2 (uint16) or 4 (uint32)
        uint32_t first_index = 0;                  ///< the draw's first index inside `index_data`
        uint32_t index_count = 0;                  ///< how many indices the draw covers
        int32_t base_vertex = 0;                   ///< the draw's base vertex (static-draw chunks; else 0)
    };

    /**
     * @ingroup vulkan_meshlet
     * @brief split one draw window into meshlets of at most `meshlet_max_triangles` triangles
     * @param input the window and the memory it reads
     * @return the meshlets, in index order, covering whole triangles only - i.e. `index_count / 3 * 3` indices.
     *         Empty when the input is degenerate (no vertices, no indices, a stride shorter than one position, or
     *         fewer than three indices).
     * @note AN INDEX OUTSIDE THE VERTEX RANGE IS SKIPPED rather than read: a malformed window would otherwise read
     *       past the vertex buffer on the CPU here (and inside a bounding box that then culls the meshlet, which
     *       is a hole nobody can trace back to its cause). The primitive upload validates its windows, so this is
     *       the second line of defence, and it is silent on purpose - it is not a condition a caller can act on.
     * @note the split is DETERMINISTIC and ORDER-PRESERVING: meshlet N's indices follow meshlet N-1's, and the
     *       triangles inside one meshlet keep the window's order. A consumer that draws them all therefore emits
     *       exactly the input triangles, in an order whose only change is that meshlets are whole runs.
     */
    export std::vector<meshlet> build_meshlets(meshlet_build_input const& input) {
        std::vector<meshlet> meshlets;
        uint32_t const triangles = input.index_count / 3u;
        if (triangles == 0u || input.vertex_count == 0u || input.index_width < 2u || input.vertex_stride < 3u * sizeof(float) ||
            input.vertex_data.size() < static_cast<std::size_t>(input.vertex_count) * input.vertex_stride) {
            return meshlets;
        }
        // the window must lie inside the index buffer (the caller's contract, re-checked because a read past it
        // would be indistinguishable from a wrong meshlet)
        std::size_t const needed_indices = static_cast<std::size_t>(input.first_index) + triangles * 3u;
        if (input.index_data.size() < needed_indices * input.index_width) {
            return meshlets;
        }

        /// one index of the window, as an uint32_t value (the same two cases the mesh stage's fetch has)
        auto const index_at = [&input](uint32_t const i) -> uint32_t {
            uint8_t const* const at = input.index_data.data() + static_cast<std::size_t>(input.first_index + i) * input.index_width;
            if (input.index_width == 2u) {
                uint16_t value = 0;
                std::memcpy(&value, at, sizeof(value));
                return value;
            }
            uint32_t value = 0;
            std::memcpy(&value, at, sizeof(value));
            return value;
        };
        /// the position of vertex @p v, or false when it is outside the buffer (see the note above)
        auto const position_at = [&input](uint32_t const v, float (&out)[3]) -> bool {
            if (v >= input.vertex_count) {
                return false;
            }
            std::memcpy(out, input.vertex_data.data() + static_cast<std::size_t>(v) * input.vertex_stride, sizeof(out));
            return true;
        };

        for (uint32_t first_triangle = 0; first_triangle < triangles; first_triangle += meshlet_max_triangles) {
            uint32_t const meshlet_triangles = std::min(meshlet_max_triangles, triangles - first_triangle);
            float low[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
            float high[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
            uint32_t positions = 0;
            for (uint32_t corner = 0; corner < meshlet_triangles * 3u; ++corner) {
                float position[3] = {};
                if (!position_at(index_at(first_triangle * 3u + corner), position)) {
                    continue;
                }
                for (int32_t axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], position[axis]);
                    high[axis] = std::max(high[axis], position[axis]);
                }
                ++positions;
            }
            meshlet out;
            out.first_index = first_triangle * 3u;
            out.index_count = meshlet_triangles * 3u;
            out.base_vertex = input.base_vertex;
            if (positions != 0u) {
                out.center_x = 0.5f * (low[0] + high[0]);
                out.center_y = 0.5f * (low[1] + high[1]);
                out.center_z = 0.5f * (low[2] + high[2]);
                float const dx = high[0] - out.center_x;
                float const dy = high[1] - out.center_y;
                float const dz = high[2] - out.center_z;
                out.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            // ---- THE NORMAL CONE: the average of the window's face normals, and the worst dot product against it ----
            // The average is accumulated UNNORMALIZED and normalized once at the end, which is the standard way to
            // get a stable axis for a fan of normals. `cone_cos` is then the SMALLEST dot any face normal has with
            // that axis, so every normal lies within `acos(cone_cos)` of it - the quantity a conservative back-face
            // test needs. A window whose triangles are all degenerate keeps axis 0 and cone_cos 1, which reads as
            // "no statement" and therefore never culls (see the shader's test).
            float axis[3] = {0.0f, 0.0f, 0.0f};
            float normals[meshlet_max_triangles][3] = {};
            uint32_t normal_count = 0;
            for (uint32_t triangle = 0; triangle < meshlet_triangles; ++triangle) {
                float a[3] = {};
                float b[3] = {};
                float c[3] = {};
                uint32_t const base = (first_triangle + triangle) * 3u;
                if (!position_at(index_at(base), a) || !position_at(index_at(base + 1u), b) || !position_at(index_at(base + 2u), c)) {
                    continue;
                }
                float const e0[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                float const e1[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                float const n[3] = {e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2], e0[0] * e1[1] - e0[1] * e1[0]};
                float const length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (length <= 0.0f) {
                    continue; // degenerate: it contributes no direction and no statement
                }
                normals[normal_count][0] = n[0] / length;
                normals[normal_count][1] = n[1] / length;
                normals[normal_count][2] = n[2] / length;
                for (int32_t axis_index = 0; axis_index < 3; ++axis_index) {
                    axis[axis_index] += normals[normal_count][axis_index];
                }
                ++normal_count;
            }
            if (normal_count != 0u) {
                float const length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
                if (length > 0.0f) {
                    out.axis_x = axis[0] / length;
                    out.axis_y = axis[1] / length;
                    out.axis_z = axis[2] / length;
                    float worst = 1.0f;
                    for (uint32_t normal = 0; normal < normal_count; ++normal) {
                        worst = std::min(worst, normals[normal][0] * out.axis_x + normals[normal][1] * out.axis_y + normals[normal][2] * out.axis_z);
                    }
                    out.cone_cos = std::max(-1.0f, worst); // clamped: a dot product can only round below -1
                }
            }
            meshlets.push_back(out);
        }
        return meshlets;
    }
} // namespace deren::vulkan
