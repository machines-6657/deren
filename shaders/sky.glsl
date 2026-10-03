/**
 * @file shaders/sky.glsl
 * @brief The analytic sky radiance, shared by the forward skybox pass and the deferred lighting pass.
 * @ingroup shaders
 *
 * The sky is a pure function of a world-space direction: no descriptor set, no push constant, no
 * bindings. That is exactly why it can be shared - skybox.frag evaluates it for the view ray of a
 * fullscreen triangle, and deferred.frag evaluates it for the pixels the G-buffer left empty (no
 * geometry wrote a depth below the far plane), so a deferred frame has the same background as a
 * forward one, from the same code.
 *
 * Matches vulkan.math::environment_color (which bakes the IBL cubemap), so the visible sky and the
 * environment reflections agree exactly.
 */

#ifndef DEREN_SKY_GLSL
#define DEREN_SKY_GLSL

/**
 * @brief analytic sky radiance for a world-space direction
 * @param dir normalized world-space view direction
 * @param sun_dir the sun's direction, pointing FROM the surface TOWARD the sun; the caller takes it from
 *        the light UBO's `light_dir`, so the disc in the sky and the light that casts the shadows are the
 *        same sun by construction rather than by two constants agreeing
 * @return linear HDR sky radiance (ground / horizon / sky bands plus a soft sun disc)
 *
 * Computed rather than sampled from the env cubemap - like UE's SkyAtmosphere - which eliminates
 * every cubemap face/texel artifact (the "inside a cube" look): no face seams, no banding, no texel
 * steps. The three bands are blended with smoothstep (C1-smooth) rather than piecewise linear.
 */
vec3 sky_color(vec3 dir, vec3 sun_dir) {
    // elevation t: 0 = nadir, 1 = zenith
    float t = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ground = vec3(0.05, 0.05, 0.07) * 0.75;
    vec3 horizon = vec3(0.17, 0.20, 0.27) * 0.75;
    vec3 sky = vec3(0.28, 0.45, 0.75) * 0.75;
    // C1-smooth blending between the three bands (smoothstep, not piecewise linear)
    float g = smoothstep(0.28, 0.50, t); // ground -> horizon
    float s = smoothstep(0.50, 0.92, t); // horizon -> sky
    vec3 env = ground + (horizon - ground) * g;
    env += (sky - env) * s;
    // soft-edged sun disc along the CALLER's sun direction - not a second constant of its own
    env += vec3(1.0, 0.95, 0.85) * smoothstep(0.98, 1.0, dot(dir, sun_dir)) * 1.5;
    return env;
}

#endif // DEREN_SKY_GLSL
