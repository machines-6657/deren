#version 460
#extension GL_EXT_ray_tracing : require

/**
 * @file shaders/rt_shadow.rmiss
 * @brief The ray-traced shadow's miss shader: the ray escaped, so the light is not blocked.
 * @ingroup shaders
 *
 * THIS SHADER'S ONE ASSIGNMENT IS LOAD-BEARING, which is the opposite of what an "empty miss shader" would
 * suggest. The payload is the ONLY channel a traversal's outcome comes back through, and a ray that escapes
 * must say so through it: leaving the miss case to a value the raygen initialised is what this pass did
 * first, and it was wrong on this device - measured on an NVIDIA RTX 4060 (591.59.0.0), a raygen that stores
 * 0.0 before the trace plus a miss shader that does not write the payload made EVERY escaped ray come back
 * classified as occluded. The DamagedHelmet capture measured 61.22 mean over the model against 76.48 for the
 * raster shadow map, with 100% of the differing pixels DARKER - the whole sunlit ground and every convex lit
 * side had its sun killed, which is the worst failure mode a shadow can have.
 *
 * The interaction is not "the miss shader must be non-empty": storing the SAME 0.0 the raygen stored is not
 * enough either, because then the store is redundant and the compiler drops it (the two SPIR-V modules differ
 * only in that constant - `OpStore %payload_occluded %float_0` vs `%float_0_25` - and measured 61.22 vs
 * 76.48). What works is exactly one store per path, with the raygen initialising nothing: `rt_shadow.rgen`
 * deliberately does not pre-write the payload, the hit group writes 1.0, and this shader writes 0.0. Two
 * arms built that way - miss writing 0.0 with no raygen initialisation, and miss writing 0.25 with a raygen
 * initialisation - are byte-identical to each other and match the raster reference at mean|d| = 0.0287.
 * The pass header carries the whole table.
 */

layout(location = 0) rayPayloadInEXT float payload_occluded;

void main() {
    payload_occluded = 0.0; // "nothing blocked the light": the ray escaped the scene unoccluded
}
