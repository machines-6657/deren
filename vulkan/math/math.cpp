module;

#include <glm/glm.hpp>

module deren.vulkan.math;

namespace deren::vulkan {
    namespace {
        constexpr float k_pi = 3.14159265359f;

        // Cubemap face direction: texel (u, v) in [-1, 1] -> unit direction (Vulkan/GL cubemap convention)
        glm::vec3 cube_face_direction(int32_t const face, float const u, float const v) {
            switch (face) {
            case 0:
                return glm::normalize(glm::vec3(1.0f, -v, -u)); // +X
            case 1:
                return glm::normalize(glm::vec3(-1.0f, -v, u)); // -X
            case 2:
                return glm::normalize(glm::vec3(u, 1.0f, v)); // +Y
            case 3:
                return glm::normalize(glm::vec3(u, -1.0f, -v)); // -Y
            case 4:
                return glm::normalize(glm::vec3(u, -v, 1.0f)); // +Z
            default:
                return glm::normalize(glm::vec3(-u, -v, -1.0f)); // -Z
            }
        }

        // Procedural environment (HDR): gradient sky/ground + sun disc.
        // Keep in sync with shaders/sky.glsl sky_color(): the visible sky is computed
        // analytically per-pixel (no cubemap sampling), so the IBL cubemap baked from this
        // function must produce exactly the same colors for reflections to match the sky - which is
        // why the sun direction is a PARAMETER here rather than a constant: `sky.glsl` takes the
        // same one from the light UBO, and two constants that must match eventually stop matching.
        glm::vec3 environment_color(glm::vec3 const& dir, std::array<float, 3> const& sun_direction) {
            float const t = std::clamp(dir.y * 0.5f + 0.5f, 0.0f, 1.0f); // 0 nadir, 1 zenith
            auto const smooth = [](float const e0, float const e1, float const x) {
                float const u = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
                return u * u * (3.0f - 2.0f * u);
            };
            constexpr glm::vec3 ground = glm::vec3(0.05f, 0.05f, 0.07f) * 0.75f;
            constexpr glm::vec3 horizon = glm::vec3(0.17f, 0.20f, 0.27f) * 0.75f;
            constexpr glm::vec3 sky = glm::vec3(0.28f, 0.45f, 0.75f) * 0.75f;
            float const g = smooth(0.28f, 0.50f, t); // ground -> horizon
            float const s = smooth(0.50f, 0.92f, t); // horizon -> sky
            glm::vec3 env = ground + (horizon - ground) * g;
            env += (sky - env) * s;
            glm::vec3 const sun_dir = glm::normalize(glm::vec3(sun_direction[0], sun_direction[1], sun_direction[2]));
            float const sun = smooth(0.98f, 1.0f, glm::dot(dir, sun_dir)); // soft-edged disc
            env += glm::vec3(1.0f, 0.95f, 0.85f) * sun * 1.5f;             // visible sun for metallic highlights
            return env;
        }

        // Cubemap face mapping: direction -> (face, u, v) with u/v in [-1, 1]. This is the one owner of
        // the convention (cube_face_direction() above is its inverse) and every sampler here goes
        // through it, so the bakes cannot drift apart from each other or from the GPU's own lookup.
        //
        // THE MAJOR-AXIS DIVIDE IS WHAT MAKES IT THE INVERSE: cube_face_direction() normalizes its
        // result, so the reverse mapping has to divide the two minor components by the major one (the
        // standard cubemap "sc/|ma|, tc/|ma|" rule) - for a +X-major direction, u = -dir.z / |dir.x|,
        // not -dir.z. Without the divide the mapping is not the inverse of the direction function at
        // all: for a unit direction |major| <= 1, so the lookup lands at u_correct * |major| - every
        // fetch is pulled toward its face centre and the six faces stop agreeing at the seams. That
        // warps the irradiance and prefilter bakes (the CPU samplers below are the only consumers),
        // i.e. exactly the environment the shaders then sample with the GPU's own correct convention.
        void cube_face_uv(glm::vec3 const& dir, int32_t& face, float& u, float& v) {
            float const ax = std::abs(dir.x);
            float const ay = std::abs(dir.y);
            float const az = std::abs(dir.z);
            float major = 0.0f;
            if (ax >= ay && ax >= az) {
                face = dir.x >= 0.0f ? 0 : 1;
                u = face == 0 ? -dir.z : dir.z;
                v = -dir.y;
                major = ax;
            } else if (ay >= ax && ay >= az) {
                face = dir.y >= 0.0f ? 2 : 3;
                u = dir.x;
                v = face == 2 ? dir.z : -dir.z;
                major = ay;
            } else {
                face = dir.z >= 0.0f ? 4 : 5;
                u = face == 4 ? dir.x : -dir.x;
                v = -dir.y;
                major = az;
            }
            // major is 0 only for a null direction, which no caller passes: leave (u, v) at the face
            // centre instead of dividing by zero.
            if (major > 0.0f) {
                u /= major;
                v /= major;
            }
        }

        // Equirectangular (lat-long) lookup: the direction -> (u, v) mapping that
        // generate_environment_cubemap_from_equirect() documents, plus bilinear interpolation in texel
        // space. Row 0 of the buffer is the image's TOP row (the decoder's order) and v = 0 (the zenith)
        // lands there; u wraps around the seam, v clamps at the poles.
        glm::vec3 sample_equirect(std::span<float const> const pixels, int32_t const width, int32_t const height, glm::vec3 const& dir) {
            float const u = 0.5f + std::atan2(dir.x, dir.z) / (2.0f * k_pi);
            float const v = std::acos(std::clamp(dir.y, -1.0f, 1.0f)) / k_pi;
            // texel-centre convention: texel i covers [i, i+1) and is sampled at i + 0.5
            float const fx = u * static_cast<float>(width) - 0.5f;
            float const fy = v * static_cast<float>(height) - 0.5f;
            float const x0f = std::floor(fx);
            float const y0f = std::floor(fy);
            float const tx = fx - x0f;
            float const ty = fy - y0f;
            int32_t const y0 = std::clamp(static_cast<int32_t>(y0f), 0, height - 1);
            int32_t const y1 = std::clamp(static_cast<int32_t>(y0f) + 1, 0, height - 1);
            auto const wrap = [width](int32_t const x) {
                int32_t const w = x % width;
                return w < 0 ? w + width : w;
            };
            int32_t const x0 = wrap(static_cast<int32_t>(x0f));
            int32_t const x1 = wrap(static_cast<int32_t>(x0f) + 1);
            auto const texel = [&](int32_t const x, int32_t const y) {
                size_t const offset = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
                return glm::vec3(pixels[offset], pixels[offset + 1], pixels[offset + 2]);
            };
            glm::vec3 const top = glm::mix(texel(x0, y0), texel(x1, y0), tx);
            glm::vec3 const bottom = glm::mix(texel(x0, y1), texel(x1, y1), tx);
            return glm::mix(top, bottom, ty);
        }

        // Van der Corput sequence (second component of Hammersley)
        float radical_inverse_vdc(uint32_t bits) {
            bits = (bits << 16u) | (bits >> 16u);
            bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
            bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
            bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
            bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
            return static_cast<float>(bits) * 2.3283064365386963e-10f;
        }

        glm::vec2 hammersley(uint32_t const i, uint32_t const n) {
            return glm::vec2(static_cast<float>(i) / static_cast<float>(n), radical_inverse_vdc(i));
        }

        // GGX importance sampling: build the half vector from Hammersley samples
        glm::vec3 importance_sample_ggx(glm::vec2 const& xi, glm::vec3 const& n, float const roughness) {
            float const a = roughness * roughness;
            float const phi = 2.0f * k_pi * xi.x;
            float const cos_theta = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
            float const sin_theta = std::sqrt(std::max(1.0f - cos_theta * cos_theta, 0.0f));
            glm::vec3 const h(sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta);
            glm::vec3 const up = std::abs(n.z) < 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            glm::vec3 const tangent = glm::normalize(glm::cross(up, n));
            glm::vec3 const bitangent = glm::cross(n, tangent);
            return glm::normalize(tangent * h.x + bitangent * h.y + n * h.z);
        }

        // IEEE 754 binary32 -> binary16 (truncated; plenty for ambient light)
        uint16_t float_to_half(float const value) {
            uint32_t const bits = std::bit_cast<uint32_t>(value);
            uint16_t const sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
            int32_t const exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
            uint32_t const mantissa = bits & 0x7FFFFFu;
            if (exponent >= 31) {
                return static_cast<uint16_t>(sign | 0x7C00u); // infinity
            }
            if (exponent <= 0) {
                return sign; // subnormal/zero -> 0
            }
            return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
        }
    } // namespace

    std::vector<float> generate_environment_cubemap(int32_t const size, std::array<float, 3> const sun_direction) {
        std::vector<float> data(static_cast<size_t>(6) * size * size * 4);
        for (int32_t face = 0; face < 6; ++face) {
            for (int32_t y = 0; y < size; ++y) {
                for (int32_t x = 0; x < size; ++x) {
                    float const u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                    float const v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                    glm::vec3 const color = environment_color(cube_face_direction(face, u, v), sun_direction);
                    size_t const offset = (static_cast<size_t>(face) * size * size + static_cast<size_t>(y) * size + x) * 4;
                    data[offset + 0] = color.r;
                    data[offset + 1] = color.g;
                    data[offset + 2] = color.b;
                    data[offset + 3] = 1.0f;
                }
            }
        }
        return data;
    }

    std::vector<float> generate_environment_cubemap_from_equirect(std::span<float const> const equirect, int32_t const width, int32_t const height, int32_t const size, float const intensity) {
        // The source is required, and a degenerate one would otherwise be read out of bounds: the one
        // caller panics before getting here, so this is the belt to that pair of braces rather than a
        // silent fallback to a sky nobody asked for.
        if (width <= 0 || height <= 0 || equirect.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4 || size <= 0) {
            return std::vector<float>(static_cast<size_t>(std::max(size, 0)) * std::max(size, 0) * 6 * 4, 0.0f);
        }
        std::vector<float> data(static_cast<size_t>(6) * size * size * 4);
        for (int32_t face = 0; face < 6; ++face) {
            for (int32_t y = 0; y < size; ++y) {
                for (int32_t x = 0; x < size; ++x) {
                    float const u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                    float const v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size) * 2.0f - 1.0f;
                    glm::vec3 const color = sample_equirect(equirect, width, height, cube_face_direction(face, u, v)) * intensity;
                    size_t const offset = (static_cast<size_t>(face) * size * size + static_cast<size_t>(y) * size + x) * 4;
                    data[offset + 0] = color.r;
                    data[offset + 1] = color.g;
                    data[offset + 2] = color.b;
                    data[offset + 3] = 1.0f;
                }
            }
        }
        return data;
    }

    // The levels a cubemap of this size can carry at all: level 0 down to 1x1. The prefilter bake takes
    // its depth from `[lighting] env_mip_count` (the SHADER's chain), while the irradiance bake is free to
    // build the whole chain: it is the CPU side and each level is one 2x2 box filter of the level above.
    int32_t environment_pyramid_levels(int32_t const env_size) {
        int32_t levels = 1;
        for (int32_t size = env_size; size > 1; size >>= 1) {
            ++levels;
        }
        return levels;
    }

    // One box-filtered source mip chain: level k is 2^k times smaller than the environment. The
    // prefilter reads a level per sample instead of always reading level 0, which is what keeps
    // the coarse levels smooth (see prefilter_environment).
    std::vector<std::vector<float>> build_environment_pyramid(std::span<float const> const env, int32_t const env_size, int32_t const levels) {
        std::vector<std::vector<float>> pyramid;
        pyramid.reserve(static_cast<std::size_t>(levels));
        pyramid.emplace_back(env.begin(), env.end());
        for (int32_t level = 1; level < levels; ++level) {
            int32_t const source_size = std::max(1, env_size >> (level - 1));
            int32_t const target_size = std::max(1, env_size >> level);
            std::vector<float> const& source = pyramid.back();
            std::vector<float> target(static_cast<std::size_t>(6) * target_size * target_size * 4, 0.0f);
            for (int32_t face = 0; face < 6; ++face) {
                for (int32_t y = 0; y < target_size; ++y) {
                    for (int32_t x = 0; x < target_size; ++x) {
                        glm::vec4 sum(0.0f);
                        for (int32_t dy = 0; dy < 2; ++dy) {
                            for (int32_t dx = 0; dx < 2; ++dx) {
                                int32_t const sx = std::min(x * 2 + dx, source_size - 1);
                                int32_t const sy = std::min(y * 2 + dy, source_size - 1);
                                std::size_t const at = (static_cast<std::size_t>(face) * source_size * source_size + static_cast<std::size_t>(sy) * source_size + sx) * 4;
                                sum += glm::vec4(source[at], source[at + 1], source[at + 2], source[at + 3]);
                            }
                        }
                        std::size_t const at = (static_cast<std::size_t>(face) * target_size * target_size + static_cast<std::size_t>(y) * target_size + x) * 4;
                        target[at + 0] = sum.x * 0.25f;
                        target[at + 1] = sum.y * 0.25f;
                        target[at + 2] = sum.z * 0.25f;
                        target[at + 3] = 1.0f;
                    }
                }
            }
            pyramid.push_back(std::move(target));
        }
        return pyramid;
    }

    // Bilinear fetch of one pyramid level, inside the face of @p dir. Filtering stops at the face
    // edge (the environment is a smooth analytic gradient and the sun disc sits well inside a
    // face, so nothing visible crosses a seam).
    glm::vec3 sample_cubemap_level(std::span<float const> const level, int32_t const size, glm::vec3 const& dir) {
        int32_t face = 0;
        float u = 0.0f;
        float v = 0.0f;
        cube_face_uv(dir, face, u, v);
        float const fx = std::clamp((u * 0.5f + 0.5f) * static_cast<float>(size) - 0.5f, 0.0f, static_cast<float>(size - 1));
        float const fy = std::clamp((v * 0.5f + 0.5f) * static_cast<float>(size) - 0.5f, 0.0f, static_cast<float>(size - 1));
        int32_t const x0 = static_cast<int32_t>(fx);
        int32_t const y0 = static_cast<int32_t>(fy);
        int32_t const x1 = std::min(x0 + 1, size - 1);
        int32_t const y1 = std::min(y0 + 1, size - 1);
        float const tx = fx - static_cast<float>(x0);
        float const ty = fy - static_cast<float>(y0);
        auto const fetch = [&](int32_t const x, int32_t const y) {
            std::size_t const at = (static_cast<std::size_t>(face) * size * size + static_cast<std::size_t>(y) * size + x) * 4;
            return glm::vec3(level[at], level[at + 1], level[at + 2]);
        };
        glm::vec3 const top = glm::mix(fetch(x0, y0), fetch(x1, y0), tx);
        glm::vec3 const bottom = glm::mix(fetch(x0, y1), fetch(x1, y1), tx);
        return glm::mix(top, bottom, ty);
    }

    // Trilinear fetch across the pyramid: one bilinear fetch per level, blended by the fraction.
    glm::vec3 sample_environment_trilinear(std::vector<std::vector<float>> const& pyramid, int32_t const env_size, glm::vec3 const& dir, float const lod) {
        float const clamped = std::clamp(lod, 0.0f, static_cast<float>(pyramid.size() - 1));
        int32_t const low = static_cast<int32_t>(clamped);
        int32_t const high = std::min(low + 1, static_cast<int32_t>(pyramid.size()) - 1);
        float const blend = clamped - static_cast<float>(low);
        glm::vec3 const a = sample_cubemap_level(pyramid[static_cast<std::size_t>(low)], std::max(1, env_size >> low), dir);
        glm::vec3 const b = sample_cubemap_level(pyramid[static_cast<std::size_t>(high)], std::max(1, env_size >> high), dir);
        return glm::mix(a, b, blend);
    }

    std::vector<float> prefilter_environment(std::span<float const> const env, int32_t const env_size, int32_t const mip_count) {
        // Why the samples read a source mip instead of level 0: this environment carries a hard
        // sun disc, and a narrow GGX lobe either lands on it or misses it. With 64 taps of level 0
        // a coarse level ended up with isolated texels several times brighter than the rest of the
        // level (measured at 16^2: brightest texel 1.22 against a level mean of 0.17, i.e. 7x the
        // mean, and 2.3x its own 3x3 neighbourhood). On screen ONE coarse texel covers a large
        // area, so those texels read as big highlight patches sweeping across a rotating metal
        // surface - the artifact this replaces. Averaging each tap over the solid angle it
        // actually represents (Karis) removes them: same measurement, brightest texel 0.42
        // (2.5x the mean, 1.3x its neighbourhood) with every level mean unchanged - the fix is
        // variance, not energy. It is also FASTER than the 64-tap-of-level-0 version (535 ms
        // against 964 ms for 256^2 x 5 levels here): level 0 needs no samples at all, its texels
        // ARE the mirror reflection, and the coarse levels take half the taps of the level before.
        std::vector<std::vector<float>> const pyramid = build_environment_pyramid(env, env_size, mip_count);
        float const texel_solid_angle = 4.0f * k_pi / (6.0f * static_cast<float>(env_size) * static_cast<float>(env_size));
        std::vector<float> result;
        for (int32_t mip = 0; mip < mip_count; ++mip) {
            int32_t const mip_size = std::max(1, env_size >> mip);
            float const roughness = static_cast<float>(mip) / static_cast<float>(mip_count - 1);
            // 128 taps at the first roughness level, halved per level (32 is the floor); level 0 needs
            // none - see the comment above. That is still about the cost of the naive version this
            // replaces, because the source mips do the averaging and level 0 does no work at all.
            uint32_t const sample_count = mip == 0 ? 1u : std::max(32u, 128u >> (mip - 1));
            float const alpha = roughness * roughness;
            std::vector<float> mip_data(static_cast<size_t>(6) * mip_size * mip_size * 4, 0.0f);
            for (int32_t face = 0; face < 6; ++face) {
                for (int32_t y = 0; y < mip_size; ++y) {
                    for (int32_t x = 0; x < mip_size; ++x) {
                        float const u = (static_cast<float>(x) + 0.5f) / static_cast<float>(mip_size) * 2.0f - 1.0f;
                        float const v = (static_cast<float>(y) + 0.5f) / static_cast<float>(mip_size) * 2.0f - 1.0f;
                        glm::vec3 const n = cube_face_direction(face, u, v);
                        glm::vec3 color(0.0f);
                        if (mip == 0) {
                            // roughness 0 is a mirror: every sample has H == N and therefore L == N, so
                            // this level IS the environment (at lod 0 the trilinear fetch is bilinear)
                            color = sample_environment_trilinear(pyramid, env_size, n, 0.0f);
                        } else {
                            glm::vec3 sum(0.0f);
                            float total_weight = 0.0f;
                            for (uint32_t i = 0; i < sample_count; ++i) {
                                glm::vec3 const h = importance_sample_ggx(hammersley(i, sample_count), n, roughness);
                                glm::vec3 const l = glm::normalize(2.0f * glm::dot(n, h) * h - n);
                                float const ndotl = glm::dot(n, l);
                                if (ndotl <= 0.0f) {
                                    continue;
                                }
                                // GGX pdf of this sample (V == N here, so VoH == NoH), then the source mip
                                // whose texel footprint matches the solid angle the sample stands for
                                float const alpha2 = alpha * alpha;
                                float const ndoth = std::max(glm::dot(n, h), 0.0f);
                                float const denominator = ndoth * ndoth * (alpha2 - 1.0f) + 1.0f;
                                float const distribution = alpha2 / std::max(k_pi * denominator * denominator, 1e-8f);
                                float const pdf = distribution * ndoth / std::max(4.0f * ndoth, 1e-6f) + 1e-4f;
                                float const sample_solid_angle = 1.0f / (static_cast<float>(sample_count) * pdf);
                                float const lod = std::max(0.5f * std::log2(sample_solid_angle / texel_solid_angle), 0.0f);
                                sum += sample_environment_trilinear(pyramid, env_size, l, lod) * ndotl;
                                total_weight += ndotl;
                            }
                            color = total_weight > 0.0f ? sum / total_weight : glm::vec3(0.0f);
                        }
                        size_t const offset = (static_cast<size_t>(face) * mip_size * mip_size + static_cast<size_t>(y) * mip_size + x) * 4;
                        mip_data[offset + 0] = color.r;
                        mip_data[offset + 1] = color.g;
                        mip_data[offset + 2] = color.b;
                        mip_data[offset + 3] = 1.0f;
                    }
                }
            }
            result.insert(result.end(), mip_data.begin(), mip_data.end());
        }
        return result;
    }

    std::vector<float> generate_irradiance_map(std::span<float const> const env, int32_t const env_size, int32_t const irr_size) {
        std::vector<float> result(static_cast<size_t>(6) * irr_size * irr_size * 4, 0.0f);
        // Loop-invariant constant: deliberately at function scope (not inside the loops)
        constexpr uint32_t sample_count = 512; // NOLINT (some toolchains flag the constant when scoped to the inner loop)
        // AREA-AVERAGED TAPS, the cure `prefilter_environment` already applies to the specular chain and
        // for the same measured reason: a point tap of level 0 either lands on a bright texel of the
        // environment or misses it, so no finite tap count converges cleanly. An irradiance texel covers a
        // large angle on screen (11 deg at the default irr_size 32), so that per-tap error reads as blobby
        // banding, and a cylinder (a leg, an arm) stretches it into streaks. MEASURED on the character's
        // thigh against the captured studio environment: fine-detail std 9.31 where the procedural sky gave
        // 4.36, dropping to 4.86 only when the irradiance map is made coarse enough (irr_size 8) to hide
        // the error, and rising to 10.29 at irr_size 128 (the error scales with the map, which is the
        // signature of a SAMPLING fault and not of the environment's content). Each tap now reads the
        // pyramid level whose texel footprint matches the solid angle it stands for (Karis, the same rule
        // the specular prefilter uses): a VARIANCE fix, not an energy one - the level means and the thigh's
        // mean luma stay where they were.
        std::vector<std::vector<float>> const pyramid = build_environment_pyramid(env, env_size, environment_pyramid_levels(env_size));
        float const texel_solid_angle = 4.0f * k_pi / (6.0f * static_cast<float>(env_size) * static_cast<float>(env_size));
        for (int32_t face = 0; face < 6; ++face) {
            for (int32_t y = 0; y < irr_size; ++y) {
                for (int32_t x = 0; x < irr_size; ++x) {
                    float const u = (static_cast<float>(x) + 0.5f) / static_cast<float>(irr_size) * 2.0f - 1.0f;
                    float const v = (static_cast<float>(y) + 0.5f) / static_cast<float>(irr_size) * 2.0f - 1.0f;
                    glm::vec3 const n = cube_face_direction(face, u, v);
                    glm::vec3 const up = std::abs(n.z) < 0.999f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
                    glm::vec3 const tangent = glm::normalize(glm::cross(up, n));
                    glm::vec3 const bitangent = glm::cross(n, tangent);
                    glm::vec3 sum(0.0f);
                    float total_weight = 0.0f;
                    for (uint32_t i = 0; i < sample_count; ++i) {
                        glm::vec2 const xi = hammersley(i, sample_count);
                        float const phi = 2.0f * k_pi * xi.x;
                        float const cos_theta = std::sqrt(xi.y);
                        float const sin_theta = std::sqrt(std::max(1.0f - xi.y, 0.0f));
                        glm::vec3 const local(sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta);
                        glm::vec3 const l = glm::normalize(tangent * local.x + bitangent * local.y + n * local.z);
                        // The tap is drawn cosine-weighted (pdf = cos/pi over the hemisphere), so it stands
                        // for 1/(N*pdf) steradians; that is the footprint the source level has to average.
                        float const pdf = std::max(cos_theta / k_pi, 1e-6f);
                        float const sample_solid_angle = 1.0f / (static_cast<float>(sample_count) * pdf);
                        float const lod = std::max(0.5f * std::log2(sample_solid_angle / texel_solid_angle), 0.0f);
                        sum += sample_environment_trilinear(pyramid, env_size, l, lod) * cos_theta;
                        total_weight += cos_theta;
                    }
                    glm::vec3 const color = total_weight > 0.0f ? sum / total_weight : glm::vec3(0.0f);
                    size_t const offset = (static_cast<size_t>(face) * irr_size * irr_size + static_cast<size_t>(y) * irr_size + x) * 4;
                    result[offset + 0] = color.r;
                    result[offset + 1] = color.g;
                    result[offset + 2] = color.b;
                    result[offset + 3] = 1.0f;
                }
            }
        }
        return result;
    }

    std::vector<float> generate_brdf_lut(int32_t const size) {
        std::vector<float> result(static_cast<size_t>(size) * size * 2);
        for (int32_t y = 0; y < size; ++y) {
            for (int32_t x = 0; x < size; ++x) {
                float const ndotv = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);
                float const roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
                constexpr glm::vec4 c0(-1.0f, -0.0275f, -0.572f, 0.022f);
                constexpr glm::vec4 c1(1.0f, 0.0425f, 1.04f, -0.04f);
                glm::vec4 const r = roughness * c0 + c1;
                float const a004 = std::min(r.x * r.x, std::exp2(-9.28f * ndotv)) * r.x + r.y;
                result[static_cast<size_t>(y) * size * 2 + static_cast<size_t>(x) * 2 + 0] = -1.04f * a004 + r.z;
                result[static_cast<size_t>(y) * size * 2 + static_cast<size_t>(x) * 2 + 1] = 1.04f * a004 + r.w;
            }
        }
        return result;
    }

    std::vector<uint8_t> to_half_rgba(std::span<float const> const data) {
        std::vector<uint8_t> out(data.size() * 2);
        for (size_t i = 0; i < data.size(); ++i) {
            uint16_t const h = float_to_half(data[i]);
            out[i * 2 + 0] = static_cast<uint8_t>(h & 0xFFu);
            out[i * 2 + 1] = static_cast<uint8_t>(h >> 8);
        }
        return out;
    }

    std::vector<uint8_t> to_half_rg(std::span<float const> const data) {
        std::vector<uint8_t> out(data.size() * 2);
        for (size_t i = 0; i < data.size(); ++i) {
            uint16_t const h = float_to_half(data[i]);
            out[i * 2 + 0] = static_cast<uint8_t>(h & 0xFFu);
            out[i * 2 + 1] = static_cast<uint8_t>(h >> 8);
        }
        return out;
    }

    // ---- async wrappers (see math.cppm): delegate to the synchronous functions on a
    //      std::async thread; the caller consumes the future when the result is needed ----

    std::future<std::vector<float>> generate_environment_cubemap_async(int32_t const size, std::array<float, 3> const sun_direction) {
        // The direction is captured BY VALUE: the caller's vector may be gone by the time the async thread runs.
        return std::async(std::launch::async, [size, sun_direction] { return generate_environment_cubemap(size, sun_direction); });
    }

    std::future<std::vector<float>> generate_environment_cubemap_from_equirect_async(std::vector<float> equirect, int32_t const width, int32_t const height, int32_t const size, float const intensity) {
        // The pixels are MOVED in, not viewed: see the declaration's own note.
        return std::async(std::launch::async, [equirect = std::move(equirect), width, height, size, intensity] {
            return generate_environment_cubemap_from_equirect(equirect, width, height, size, intensity);
        });
    }

    std::future<std::vector<float>> prefilter_environment_async(std::span<float const> const env, int32_t const env_size, int32_t const mip_count) {
        return std::async(std::launch::async, [env, env_size, mip_count] { return prefilter_environment(env, env_size, mip_count); });
    }

    std::future<std::vector<float>> generate_irradiance_map_async(std::span<float const> const env, int32_t const env_size, int32_t const irr_size) {
        return std::async(std::launch::async, [env, env_size, irr_size] { return generate_irradiance_map(env, env_size, irr_size); });
    }

    std::future<std::vector<float>> generate_brdf_lut_async(int32_t const size) {
        return std::async(std::launch::async, [size] { return generate_brdf_lut(size); });
    }
} // namespace deren::vulkan
