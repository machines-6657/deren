// module version: 0.3.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/ray_traced_shadow.cppm
 * @brief The ninth real pass, and the only one that traces outside the chain: the ray-traced sun shadow.
 * @defgroup vulkan_pass_rt_shadow Ray-Traced Shadow Pass
 *
 * WHAT IT OWNS: its RAY TRACING pipeline (three stages - raygen, closest hit and miss -
 * with the three shader groups and the shader binding table regions that go with them, built at create time from
 * its own declaration's push-block size); the frame's
 * recording - the two barriers around the visibility image it rewrites, the push
 * block's values (composed by the renderer) and the full-resolution `traceRays` launch; and the one-shot log that
 * says what was built, which used to live in the runtime as a bool next to the pipeline.
 *
 * WHAT IT DOES NOT OWN, in the same shape as the tracer and the spatial filter: no descriptor set of its own.
 * The camera, the light UBO, the top level structure, the surface each ray starts
 * from and the visibility image it writes are all heap slots the shaders name themselves - so the
 * only handle it needs beyond the pipeline is the IMAGE it transitions, and that arrives through
 * `pass_io::barrier_images`.
 *
 * THE OVER-OCCLUSION THIS PASS SHIPPED WITH WAS THE PAYLOAD, and the fix is the miss shader's one assignment.
 * It is recorded at this length because its shape is a trap: nothing about it is visible in the source,
 * validation is silent about it, and the two SPIR-V modules involved differ by a single float constant.
 *
 * WHAT WENT WRONG: the raygen stored 0.0 into the payload before `traceRayEXT` and used the value it read back
 * afterwards, and the miss shader was empty - "no hit is the initial value". On this device (NVIDIA RTX 4060,
 * 591.59.0.0) EVERY ESCAPED RAY came back classified as occluded under that arrangement: the DamagedHelmet
 * scenario measured 61.22 mean over the model against 76.48 for the shipped raster shadow map, 100% of the
 * differing pixels darker, so the whole sunlit ground and every convex lit side lost its sun. Four arms, all the
 * same scenario at the same capture pose, isolated it:
 *
 *   | arm                | raygen          | miss shader     | model mean | whole-frame mean|d| vs raster |
 *   |--------------------|-----------------|-----------------|------------|----------------------------------|
 *   | A (broken)         | stores 0.0      | empty           | 61.22      | 1.5388                           |
 *   | C (broken)         | stores 0.0      | stores 0.0      | 61.22      | 1.5388                           |
 *   | B (fixed)          | stores 0.0      | stores 0.25     | 76.48      | 0.0287                           |
 *   | D (fixed, shipped) | stores nothing  | stores 0.0      | 76.48      | 0.0287                           |
 *   | raster reference   | -               | -               | 76.48      | 0                                |
 *
 * Arm C is the interesting one: writing the SAME value the raygen already stored is not enough, because the
 * store is then redundant and the compiler drops it. `spirv-dis` shows the two miss modules differ only in that
 * constant (`OpStore %payload_occluded %float_0` against `%float_0_25`), so the elimination happens after SPIR-V,
 * in the driver's own compiler - and what it eliminates is the store a traversal needs in order to preserve the
 * payload at all. Arms B and D are byte-identical to each other (mean|d| = 0.0000) and both match the raster
 * reference at the shadow edge only: 157 of 255 at the penumbra, which is the hard-versus-PCF difference this
 * pass documents rather than a disagreement. The rule the pass follows from here on is therefore "exactly one
 * payload store per path and no raygen pre-initialisation": the hit group writes 1.0, the miss shader writes 0.0,
 * the raygen writes nothing. Both shaders carry the note.
 *
 * HOW IT WAS FOUND, because the same instrument is what any future traversal question wants: FOUR ROW CLASSES IN
 * ONE FRAME (row % 4) reporting the ray-query verdict, the traced verdict, which program actually ran, and
 * whether the two verdicts agree on that pixel - so every comparison is against the neighbouring row in the same
 * image rather than against another build. At the reference pose the query class and the trace class both
 * reproduced the raster reference (76.37 and 76.58 against the raster rows' 76.37 and 76.60) where the committed
 * build's same rows measured 61.1-61.3, and the "which program ran" class (the miss shader writing 0.25 straight
 * into the visibility) is what proved the write was reaching the image at all. Two earlier probes had produced
 * numbers that looked contradictory because they compared captures taken at different CAMERA POSES: comparing two
 * frames requires `--capture-camera`, and a whole-frame mean|d| of tens is the signature of that mistake rather
 * than of a rendering change.
 *
 * The SBT was verified along the way and is correct: group 0 raygen / 1 miss / 2 hit, the instance's SBT record
 * offset 0, one geometry per BLAS, mask 0xFF, facing-cull disabled, and validation silent throughout. (The
 * geometry's `VK_GEOMETRY_OPAQUE_BIT_KHR` was still set during that search and is deliberately NOT set now - see
 * the ordering note below.) One real fix came out of the search without being the cause - the pass's two image
 * barriers were the shared constants written for a COMPUTE producer, so the GENERAL -> SHADER_READ transition
 * named COMPUTE_SHADER in `srcStageMask` while the writer had become the ray-tracing stage; the producer stage is
 * overridden in the recording now, and it changed nothing (the frame stayed byte-identical), which is itself the
 * evidence that this was never an ordering problem.
 *
 * THE ALPHAMODE MASK CUT IS WHAT THE PIPELINE WAS FOR, and it is measured rather than asserted. `rt_shadow.rahit`
 * refuses an intersection whose material alpha is below the material's cutoff, fetching the hit's own UV from the
 * geometry's vertex and index buffers through binding 17's instance record (that shader records why an attribute
 * cannot carry it: `hitAttributeEXT` is written by an intersection shader and triangle geometry has none). Two
 * arms on the AlphaBlendModeTest asset - 3 MASK materials, cutoffs 0.25 and 0.75, `rt_mask_bake = false`, the
 * same config and the same capture pose, so the alpha cut is the ONLY mask handling in either:
 *
 *   | arm                    | whole frame | mean|d| vs the raster shadow | over the 9612 px the cut changes |
 *   |------------------------|-------------|------------------------------|----------------------------------|
 *   | alpha cut ON (shipped) | 124.41      | 0.7857                       | 3.0                              |
 *   | alpha cut OFF          | 123.11      | 2.0238                       | 136.6                            |
 *
 * The cut changes 9,612 pixels, EVERY ONE OF THEM BRIGHTER (a shadow removed, which is what a mask cut means), and
 * on 99.4% of them it is closer to the raster shadow than the uncut arm is - so the holes it cuts are the raster
 * path's holes. It is inert where it must be: the DamagedHelmet scene has no MASK material at all and renders
 * byte-identically with and without the stage.
 *
 * ORDERING NOTE: it needs no `gl_RayFlagsOpaqueEXT` (see `rt_shadow.rgen`) and no `VK_GEOMETRY_OPAQUE_BIT_KHR` (see
 * vulkan/acceleration_structure/acceleration_structure.cppm): both would DECLARE that no any-hit shader may run,
 * which is the opposite of what this stage is for. Neither flag was observed to actually suppress the stage on
 * this device - measured, three arms - but relying on a driver over-invoking it would be relying on the wrong
 * thing.
 *
 * WHY IT IS A PASS RATHER THAN A `record_*` FUNCTION: its position is an ordering constraint - after the G-buffer
 * pass (whose depth and normal the rays start from) and before the lighting stage (which multiplies the sun term
 * by its result), and running it earlier would mean starting rays from the PREVIOUS frame's surface. That
 * position is now the stage's, and the frame loop's off path (the transition the lighting stage's descriptor
 * needs on a frame where this pass does not run) is what the renderer keeps - it is about the STAGE, not the
 * pass's own work.
 */

module;

#include <array>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.ray_traced_shadow;

import deren.vulkan.pass;
import deren.vulkan.render_resource;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the compute pipeline this pass builds

export namespace deren::vulkan::pass {

    /**
     * @brief one ray per pixel against the scene's acceleration structures, terminated on the first hit
     *
     * Optional: without it (or without a device that has ray queries) the cascaded shadow maps keep running, which
     * is why the renderer's `light_state.rt_shadows` lane - the flag the lighting stage reads - is composed from
     * this pass's own `pipeline_ready()`.
     */
    class rt_shadow_pass final : public frame_pass {
    public:
        /// @brief the push block, which is also the one the `shaders/rt_shadow.*` stages declare
        struct push_constants {
            glm::mat4 inv_view_proj = glm::mat4(1.0f); // clip -> world, the block the lighting stage uses
            // x = ray tmin, y = absolute normal-offset floor, z = relative offset scale (per unit of
            // distance from the camera), w = unused
            glm::vec4 params = glm::vec4(0.01f, 0.002f, 0.0015f, 0.0f);
        };

        rt_shadow_pass() = default;
        ~rt_shadow_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::vulkan::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief whether the pass built what it records with (the renderer gates the ray-traced path on this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question, so an owner holding only a chain can ask it
        ///        (a pass with nothing of its own to build keeps the interface's `true`; see `frame_pass::ready`)
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        /// @brief the pipeline the runner binds before this pass records
        [[nodiscard]] VkPipeline pipeline() const noexcept override;

    private:
        // THREE GROUPS but FOUR stages: the shader binding table's order is raygen, miss, hit - the any-hit
        // shader is a SECOND STAGE of the hit group rather than a group of its own, so the regions below and the
        // stride they are addressed by are unchanged by it.
        static constexpr std::string_view raygen_name = "rt_shadow.rgen.spv";
        static constexpr std::string_view closest_hit_name = "rt_shadow.rchit.spv";
        static constexpr std::string_view miss_name = "rt_shadow.rmiss.spv";
        static constexpr std::string_view any_hit_name = "rt_shadow.rahit.spv";
        static constexpr uint32_t group_size = 8; // unused by a traceRays launch (the launch dims ARE the extent)
        /// the one declared barrier image, by the position the declaration gives it
        static constexpr uint32_t barrier_visibility = 0;
        static_assert(barrier_visibility + 1 == render_resource::rt_shadow_barriers.size(),
                      "the shadow pass's barrier slots must match the declaration it indexes");

        static constexpr std::array<std::string_view, 1> pipeline_names = {"rt_shadow"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::vulkan::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::ray_tracing,
            .group_size_x = group_size,
            .group_size_y = group_size,
            .group_size_z = 1,
            .extent = extent_rule::full, // one ray per PIXEL: the visibility image is the frame's size
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = false,
        };
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
        // The shader binding table's three regions, filled at create time from the pipeline's group handles. The
        // BUFFER is the owner's (see pass_context::create_upload_buffer); what the pass keeps is where each
        // region starts, which is the per-pipeline part.
        /// the traceRays entry point, loaded through vkGetDeviceProcAddr at create time (an extension command
        /// is not exported by the loader's import library - see the acceleration-structure module's note)
        PFN_vkCmdTraceRaysKHR trace_rays = nullptr;
        VkStridedDeviceAddressRegionKHR raygen_region = {};
        VkStridedDeviceAddressRegionKHR miss_region = {};
        VkStridedDeviceAddressRegionKHR hit_region = {};
        /// The callable region, which this pipeline has no shaders for - and which must still be a VALID
        /// pointer to an all-zero region: `vkCmdTraceRaysKHR` dereferences it, so passing nullptr is a
        /// validation error and (measured) a driver access violation rather than "no callables".
        VkStridedDeviceAddressRegionKHR callable_region = {};
        /// whether the "tracing WxH rays per frame" line has been logged (it used to be the runtime's flag)
        bool logged = false;
    };

    static_assert(sizeof(rt_shadow_pass::push_constants) == render_resource::rt_shadow_io.push->size,
                  "the shadow pass's declared push block must be the size of the struct the renderer composes");

} // namespace deren::vulkan::pass
