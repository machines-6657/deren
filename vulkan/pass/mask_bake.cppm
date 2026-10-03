// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/mask_bake.cppm
 * @brief The alphaMode MASK bake: the compute job that collapses a masked caster's holes into an expanded copy
 *        of its vertices, so an inline ray query - which has no any-hit stage to discard in - still sees them.
 * @defgroup vulkan_pass_mask_bake AlphaMode MASK Bake
 *
 * WHY THIS IS NOT A `frame_pass`, and the distinction is the point rather than a shortcut: the framework's pass
 * contract is PER FRAME (a declaration, a `resolved_io` built from it, a stage the runner walks every frame,
 * `on_swapchain_recreated` for its generation state). This work is the opposite: it runs ONCE, inside the
 * command buffer that builds the bottom level structures, and its input is the caster list the runtime is
 * walking at that moment. Pretending it were a frame pass would mean a feature gate that is true on exactly one
 * frame and a frame struct standing in for a build loop - two lies instead of one honest difference. What it
 * DOES share with a pass is the ownership rule this branch has been extracting: the compute pipeline and the
 * recording are the JOB's, and the renderer keeps only the policy (which casters, which buffers, when).
 *
 * WHY IT HAS NO DECLARATION in `render_resource` either: everything it reads is a heap slot (the bindless
 * texture array and the material table), named by the shader itself, so a declaration would describe a layout
 * this renderer never creates and the job never binds.
 */

module;

#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.mask_bake;

import deren.vulkan.pass;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the pipeline this job builds

export namespace deren::vulkan::pass {

    /// @brief the push block, which is also `mask_bake.comp`'s
    ///
    /// Device addresses as two 32-bit halves, the same shape every traced pass here pushes them in. Three
    /// uvec2s then five uints: 24 + 20 = 44 bytes, and that is the number the pipeline layout's range is
    /// declared with (`sizeof` this struct). MEASURED, because the note this replaced claimed "48 bytes on the
    /// CPU and 44 in the shader" and the first number was wrong: `glm::uvec2` is 8 bytes with ALIGNMENT 4 in
    /// this build (not glm's SIMD-aligned 8), so the struct needs no tail padding and the CPU and shader blocks
    /// are the same 44 bytes. The static assertion below keeps that true rather than remembered.
    struct mask_bake_push_constants {
        glm::uvec2 source_vertices = glm::uvec2(0u); // the source vertex buffer, low and high halves
        glm::uvec2 source_indices = glm::uvec2(0u);  // ... its index buffer (zero = not indexed)
        glm::uvec2 destination = glm::uvec2(0u);     // ... the expanded buffer this job fills
        uint32_t source_stride = 0;
        uint32_t destination_stride = 0; // 32: position(3) + normal(3) + uv(2), what hit shading reads
        uint32_t index_type = 1;         // VkIndexType: 0 = uint16, 1 = uint32 (ignored when unindexed)
        uint32_t triangle_count = 0;
        uint32_t material_index = 0; // the material whose alpha texture and cutoff decide the mask
    };

    /**
     * @brief what the renderer hands the bake for ONE caster
     *
     * The policy is the runtime's: it decides which casters are masked and allocates the expanded buffer each
     * one is baked into (the buffer outlives the bake - the structure build reads it and hit shading reads its
     * vertices through the instance table for as long as the structures live). What this job does with the
     * request is the dispatch.
     */
    struct mask_bake_request {
        VkDeviceAddress source_vertices = 0; // the caster's vertex buffer
        VkDeviceAddress source_indices = 0;  // ... its index buffer (0 = not indexed)
        VkDeviceAddress destination = 0;     // the expanded copy this bake writes
        uint32_t source_stride = 0;
        uint32_t destination_stride = 32; // the engine's traced vertex layout
        uint32_t index_type = 1;          // VkIndexType
        uint32_t triangle_count = 0;
        uint32_t material_index = 0;
    };

    /**
     * @brief the one-shot bake job: it owns its pipeline
     *
     * `create` is handed the same `pass_context` a pass gets and NOTHING else: the material table and the
     * bindless texture array are heap slots the shader names itself. `record` is one caster's dispatch.
     * Everything the job owns is released in its destructor, so what the renderer holds is one member instead of
     * four raw handles.
     */
    class mask_bake_job {
    public:
        mask_bake_job() = default;
        ~mask_bake_job();

        mask_bake_job(mask_bake_job const&) = delete;
        mask_bake_job& operator=(mask_bake_job const&) = delete;
        mask_bake_job(mask_bake_job&&) = default;
        mask_bake_job& operator=(mask_bake_job&&) = delete;

        /// @brief build the pipeline; an error message says what was missing
        [[nodiscard]] std::expected<void, std::string> create(pass_context const& context);
        /**
         * @brief dispatch ONE caster's bake
         * @param command_buffer the command buffer the dispatch is recorded into
         * @param request the caster's bake request
         * @param push_owner the renderer, @param push_raw its endpoint for a stage that declares NO index lanes
         *
         * The block goes to the pipeline as DATA now: no pipeline in this renderer has a layout (`vkCmdPushConstants`
         * would have nothing to push to), so `deren.vulkan.core`'s conversion made every stage read `vkCmdPushDataEXT`
         * instead. RAW - with no heap indices appended - is the exact endpoint this shader wants: it declares none,
         * because everything it reads is a device address pushed here or a material record in the heap.
         */
        void record(VkCommandBuffer command_buffer, mask_bake_request const& request, void* push_owner,
                    bool (*push_raw)(void* owner, VkCommandBuffer command_buffer, std::span<std::byte const> bytes)) const noexcept;
        /// @brief whether the job built what it records with (the renderer's gate for baking at all)
        [[nodiscard]] bool ready() const noexcept;

        [[nodiscard]] VkPipeline pipeline() const noexcept;

    private:
        static constexpr std::string_view shader_name = "mask_bake.comp.spv";
        static constexpr uint32_t group_size = 64; // `mask_bake.comp`'s local_size_x
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
    };

    /// THE PUSH BLOCK'S SIZE IS PART OF THE SHADER'S CONTRACT, and it is the range the pipeline layout is
    /// created with: 44 bytes, the same as the block `mask_bake.comp` declares. It was 44 all along - the
    /// comment above used to claim 48 on the CPU, which nothing checked until this assertion did.
    static_assert(sizeof(mask_bake_push_constants) == 44, "the bake's push block must stay the size the shader declares");

} // namespace deren::vulkan::pass
