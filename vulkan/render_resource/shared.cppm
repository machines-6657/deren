// module version: 0.1.1  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/render_resource/shared.cppm
 * @brief The SHARED handles: the Vulkan side of the resource description, in a nested module.
 * @defgroup vulkan_render_resource_shared Render Resource Shared Handles
 *
 * WHY THIS IS A MODULE OF ITS OWN rather than part of `deren.vulkan.render_resource`, and it is not tidiness: the
 * description layer is deliberately PURE CPU - its own enums, no Vulkan type - and that is what lets its
 * invariants (usage is a subset of the schema, kind and access fit, a pass's own bindings are contiguous, a
 * pool count is derivable) be checked in `ctest` on a machine with no GPU. The capture gate cannot run in CI
 * at all, because its references are tied to one machine's driver, so that property is the only verification
 * this layer can have there. Handles are `VkImageView`/`VkBuffer`/`VkSampler`, so they live HERE, nested under
 * the same region - the repository's own convention (`deren.vulkan.core:vma_handles`, `deren.vulkan.core.pipeline`) is
 * that a region's internal parts nest while peer areas stay flat.
 *
 * WHAT BELONGS HERE: handles for resources the SCHEMA declares as shared (`resource_scope::device_wide` and
 * the ones every pass may reference), filled ONCE by the runtime and passed around by const reference. The
 * rule for admitting something is that more than one consumer needs it and none of them owns it - a pass's own
 * family stays with that pass, and anything only the frame loop touches stays in the frame loop.
 *
 * WHAT IS HERE TODAY AND WHAT WILL JOIN IT: the samplers, because they are process-wide objects that a
 * declaration CHOOSES between (`sampler_hint`) and must never name directly - a pass that could name a raw
 * `VkSampler` could name the wrong one, and each exists for a reason (the G-buffer's is NEAREST, the post
 * chain's is LINEAR over 2D). The shared IMAGE and BUFFER handles
 * (the IBL cubes and the BRDF LUT, the bindless texture array, the top level structure) are NOT here yet, and
 * deliberately: this layer has learned that its shape is discovered by a consumer, and the consumer that will
 * need them is whichever writes the scene block through a declaration. Inventing the table before that would
 * guess at its keys.
 */

module;

#include <cstdint>
#include <vulkan/vulkan.h>

export module deren.vulkan.render_resource.shared;

import deren.vulkan.render_resource;

export namespace deren::vulkan::render_resource::shared {

    /**
     * @brief the samplers this renderer owns, addressed by what a declaration asks for
     * @ingroup vulkan_render_resource_shared
     *
     * FIVE of them have a `sampler_hint` a declaration can choose by (see `of`). `textures` is the sixth and
     * has none YET: it is the sampler the bindless texture ARRAY is read through (repeat addressing, a long LOD
     * range), it was reachable only from the renderer's hand-written set code, and it joined this struct when
     * the alphaMode MASK bake - a job with no declaration of its own - had to write the scene layout's binding 1
     * itself. A hint for it belongs with the first DECLARATION that names the array.
     */
    struct sampler_set {
        VkSampler gbuffer = VK_NULL_HANDLE;
        VkSampler taa = VK_NULL_HANDLE;
        VkSampler post = VK_NULL_HANDLE;
        VkSampler nearest = VK_NULL_HANDLE;
        VkSampler shadow = VK_NULL_HANDLE;
        /// @brief the bindless texture array's sampler (no hint yet: see the struct's doc)
        VkSampler textures = VK_NULL_HANDLE;

        /// @brief the sampler a declared hint means; `none` is "this binding has no sampler", which the
        ///        declaration's validator enforces exactly
        [[nodiscard]] constexpr VkSampler of(sampler_hint const hint) const noexcept {
            switch (hint) {
            case sampler_hint::none:
                return VK_NULL_HANDLE;
            case sampler_hint::gbuffer:
                return gbuffer;
            case sampler_hint::taa:
                return taa;
            case sampler_hint::post:
                return post;
            case sampler_hint::nearest:
                return nearest;
            case sampler_hint::shadow:
                return shadow;
            }
            return VK_NULL_HANDLE;
        }
    };

} // namespace deren::vulkan::render_resource::shared
