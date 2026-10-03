// ============================================================================
// module: deren.vulkan.math
// module version: 0.1.1  (independent of the app version in CMakeLists project(VERSION))
//
// CPU-side math: environment cubemap generation / IBL precompute (prefilter,
// irradiance, BRDF LUT). Pure CPU, no Vulkan dependency.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <cstdint> // int32_t / uint8_t below: the fixed-width names the project's arithmetic types come from

export module deren.vulkan.math;
export import deren.vstd;

/**
 * @file math.cppm
 * @defgroup vulkan_math Vulkan Math (CPU-side)
 * @brief CPU-side math for the renderer: IBL precomputation (prefiltered environment mip chain,
 *        irradiance cubemap, BRDF LUT) and half-float conversion
 * @note
 *      - pure CPU math, no Vulkan or GPU resources involved
 *      - a standalone pure-CPU module (the GPU scene/module side lives in deren.vulkan.scene_tree)
 */
namespace deren::vulkan {

    /**
     * @ingroup vulkan_math
     * @brief generate a procedural HDR environment cubemap (RGBA32F, 6 faces packed)
     * @param sun_direction the sun's direction, pointing FROM the surface TOWARD the sun, unnormalized; the
     *        default is the historic hard-coded vector, so a caller that does not care gets the frame it
     *        always got. It is a parameter because the visible sky (`shaders/sky.glsl`) draws its disc from
     *        the light UBO's direction, and a baked environment whose sun disagreed with it would put the
     *        reflections' glint somewhere the sky does not have a sun.
     */
    export std::vector<float> generate_environment_cubemap(int32_t size, std::array<float, 3> sun_direction = {0.3f, 1.0f, 0.5f});

    /**
     * @ingroup vulkan_math
     * @brief build the environment cubemap from an EQUIRECTANGULAR (lat-long) image instead of the
     *        procedural sky: the reference package's own way of describing its world
     * @param equirect linear-radiance texels, 4 floats each, row 0 = the image's TOP row (the order the
     *        engine's decoder hands them over), `width` columns
     * @param width source width in texels
     * @param height source height in texels
     * @param size the cubemap face size to produce; the output has the same layout and semantics as
     *        generate_environment_cubemap() (RGBA32F, six faces packed in +X -X +Y -Y +Z -Z order, alpha 1)
     * @param intensity a linear multiplier on every texel; 1 is the file's own radiance
     *
     * THE TWO AXES ARE THE ONLY THING THAT CAN GO WRONG HERE, so they are stated rather than implied:
     *   - vertical: `v = acos(clamp(dir.y)) / pi`, so v = 0 is the image's first row and maps to +Y (up).
     *     A studio HDRI is lit from above, so its upper half is the brighter one; if a future measurement
     *     reports the opposite, THIS line is the bug and not the asset.
     *   - horizontal: `u = 0.5 + atan2(dir.x, dir.z) / (2*pi)`, so u = 0.5 looks along +Z and u grows
     *     toward +X. The reference world's own equirect puts the image's centre on its -Y, which after the
     *     USD -> glTF axis conversion the background asset already went through is this engine's +Z: the
     *     two agree by construction rather than by luck. u wraps around, v clamps at both poles.
     * Sampling is bilinear in texel space. At the equator a 256-texel face over a 1024-wide source is near
     * 1:1 and loses nothing; the polar rows are oversampled in the source and are the one place where a
     * box average would beat this interpolation.
     */
    export std::vector<float> generate_environment_cubemap_from_equirect(std::span<float const> equirect, int32_t width, int32_t height, int32_t size, float intensity = 1.0f);

    /**
     * @ingroup vulkan_math
     * @brief GGX importance-sampled prefilter of the environment into a mip chain, one mip level per
     *        roughness step, each sample averaged over the source mip that matches its own solid angle
     *        (so the coarse levels stay smooth instead of picking up isolated bright texels - one of
     *        those covers a large screen area on a metal surface); level 0 is the mirror reflection and
     *        is copied rather than sampled
     * @param env the base environment cubemap from generate_environment_cubemap() (read-only view)
     * @param env_size base cubemap size
     * @param mip_count number of mip levels
     * @return mip-major RGBA32F data (mip0 all faces, then mip1, ...), ready for a cubemap upload
     */
    export std::vector<float> prefilter_environment(std::span<float const> env, int32_t env_size, int32_t mip_count);

    /**
     * @ingroup vulkan_math
     * @brief cosine-weighted hemisphere convolution for the diffuse irradiance cubemap
     */
    export std::vector<float> generate_irradiance_map(std::span<float const> env, int32_t env_size, int32_t irr_size);

    /**
     * @ingroup vulkan_math
     * @brief BRDF integration LUT filled with the Frostbite analytic approximation (RG32F: scale, bias)
     */
    export std::vector<float> generate_brdf_lut(int32_t size);

    // ---- async wrappers (each runs its synchronous twin on a fresh std::async thread) ----
    // The heavy CPU stages get _async siblings returning std::future; the caller only blocks
    // in get() when the result is actually needed, so independent stages can overlap.
    // @note span parameters are taken by view: the pointed-to data (e.g. the env cubemap)
    //       must stay alive until the returned future is consumed.

    export std::future<std::vector<float>> generate_environment_cubemap_async(int32_t size, std::array<float, 3> sun_direction = {0.3f, 1.0f, 0.5f});
    /**
     * @ingroup vulkan_math
     * @brief async twin of generate_environment_cubemap_from_equirect()
     * @note UNLIKE the span-taking twins above, this one OWNS its input: the pixels come from a file the
     *       caller loads a few lines earlier, and it would otherwise have to keep that buffer alive across
     *       a whole startup stage - the exact lifetime a moved-in vector makes impossible to get wrong.
     */
    export std::future<std::vector<float>> generate_environment_cubemap_from_equirect_async(std::vector<float> equirect, int32_t width, int32_t height, int32_t size, float intensity = 1.0f);
    export std::future<std::vector<float>> prefilter_environment_async(std::span<float const> env, int32_t env_size, int32_t mip_count);
    export std::future<std::vector<float>> generate_irradiance_map_async(std::span<float const> env, int32_t env_size, int32_t irr_size);
    export std::future<std::vector<float>> generate_brdf_lut_async(int32_t size);

    /**
     * @ingroup vulkan_math
     * @brief convert 4-channel float data to a packed RGBA16F byte stream
     */
    export std::vector<uint8_t> to_half_rgba(std::span<float const> data);

    /**
     * @ingroup vulkan_math
     * @brief convert 2-channel float data to a packed RG16F byte stream
     */
    export std::vector<uint8_t> to_half_rg(std::span<float const> data);
} // namespace deren::vulkan
