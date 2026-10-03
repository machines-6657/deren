// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/pass/compute_skin.cppm
 * @brief Compute skinning for the traced features: it writes the vertices the VERTEX shader would compute into
 *        the buffer a bottom level structure is built from, so a traced shadow follows the pose instead of
 *        standing in the bind pose.
 * @defgroup vulkan_pass_compute_skin Compute-Skinning Job
 *
 * WHY THIS IS A JOB AND NOT A `frame_pass`, exactly as `vulkan.pass.mask_bake_job` is: its work is a LIST of
 * dispatches the renderer derives from the caster set it is walking (`rt_caster_levels`), and the SAME list is
 * recorded twice with different meanings - once on the frame the structures are created (the build below reads
 * those vertices) and once per frame after (the structure is REFITTED, because only the bytes change). A frame
 * pass would have to pretend the first of those is a frame. What it shares with a pass is the ownership rule and
 * the construction path: `create` is handed the same `pass_context` a pass gets, and the job owns its pipeline
 * and the ordering barrier its own write demands.
 *
 * THE PER-JOINT MATRICES IT READS ARE A HEAP SLOT, not a descriptor set: the shader names the slot the frame's
 * push block carries, so the job keeps no per-slot set and no pool.
 */

module;

#include <cstdint>
#include <expected>
#include <glm/glm.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>

export module deren.vulkan.pass.compute_skin;

import deren.vulkan.pass;
import deren.vulkan.core.handles; // vk_pipeline: the RAII owner of the pipeline this job builds

export namespace deren::vulkan::pass {

    /// @brief the push block, which is also `compute_skin.comp`'s
    ///
    /// Two device addresses as two 32-bit halves each, then four uints: 16 + 16 = 32 bytes. The strides are the
    /// shader's precondition, not a heuristic - it reads the joints at byte 32 and the weights at byte 48 of the
    /// engine's 64-byte interleaved vertex, so a caster whose vertices are packed differently is refused by the
    /// renderer rather than skinned with the wrong words.
    struct compute_skin_push_constants {
        glm::uvec2 source_vertices = glm::uvec2(0u); // the primitive's bind-pose vertices, low and high
        glm::uvec2 destination = glm::uvec2(0u);     // the skinned buffer this job fills
        uint32_t source_stride = 0;                  // 64: the engine's interleaved vertex
        uint32_t destination_stride = 0;             // 32: position, normal, uv
        uint32_t vertex_count = 0;
        uint32_t skin_base = 0; // this primitive's joint block in skins.matrices
    };

    static_assert(sizeof(compute_skin_push_constants) == 32, "the job's push block must stay the size the shader declares");

    /// @brief one skinned caster's dispatch: the two buffers it works between, and where its joints start
    struct compute_skin_request {
        VkDeviceAddress source_vertices = 0; // the primitive's bind-pose vertices
        VkDeviceAddress destination = 0;     // the skinned buffer this job fills (0 = not a skinned caster)
        uint32_t source_stride = 0;          // 64
        uint32_t destination_stride = 0;     // 32
        uint32_t vertex_count = 0;
        uint32_t skin_base = 0;
    };

    /**
     * @brief the compute-skinning job: it owns its pipeline layout, its pipeline, its per-slot sets
     *
     * `record` records ONE dispatch per request and then the memory barrier the acceleration structure build or
     * refit needs before it reads what those dispatches wrote (a compute SHADER_WRITE is not visible to a build
     * without it, and the symptom would be a shadow one frame behind - which reads as animation lag). It answers
     * whether it recorded anything, which is what the renderer gates the per-frame refit on.
     */
    class compute_skin_job {
    public:
        compute_skin_job() = default;
        ~compute_skin_job();

        compute_skin_job(compute_skin_job const&) = delete;
        compute_skin_job& operator=(compute_skin_job const&) = delete;
        compute_skin_job(compute_skin_job&&) = default;
        compute_skin_job& operator=(compute_skin_job&&) = delete;

        /**
         * @brief build the pipeline
         * @param context the same create-time context a pass gets
         */
        [[nodiscard]] std::expected<void, std::string> create(pass_context const& context);
        /**
         * @brief record one dispatch per request, then the build-ordering barrier; whether anything was recorded
         * @param command_buffer the command buffer the dispatches are recorded into
         * @param requests the frame's skin requests (one dispatch each)
         * @param push_owner the renderer, @param push_indices its endpoint: the block is sent as DATA with the two
         *        heap indices appended (this shader reads the per-frame joint matrices, so it declares both), which
         *        is why the frame slot is no longer a parameter here - the endpoint carries it.
         */
        [[nodiscard]] bool record(VkCommandBuffer command_buffer, std::span<compute_skin_request const> requests, void* push_owner,
                                  bool (*push_indices)(void* owner, VkCommandBuffer command_buffer, std::span<std::byte const> bytes, uint32_t extra_lane)) const noexcept;
        /// @brief whether the job built what it records with (the renderer's gate for skinning at all)
        [[nodiscard]] bool ready() const noexcept;

        [[nodiscard]] VkPipeline pipeline() const noexcept;

    private:
        static constexpr std::string_view shader_name = "compute_skin.comp.spv";
        static constexpr uint32_t group_size = 64; // `compute_skin.comp`'s local_size_x
        void release_owned() noexcept;

        VkDevice device = VK_NULL_HANDLE;
        // called pass_pipeline, not pipeline: the class declares pipeline() and a member of that name
        // would duplicate it and hide the override.
        std::optional<vk_pipeline> pass_pipeline = std::nullopt;
    };

} // namespace deren::vulkan::pass
