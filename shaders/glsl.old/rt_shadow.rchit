#version 460
#extension GL_EXT_ray_tracing : require

/**
 * @file shaders/rt_shadow.rchit
 * @brief The ray-traced shadow's hit group: it reports THAT the ray hit something, and nothing else.
 * @ingroup shaders
 *
 * The sun's visibility is a binary question, and `gl_RayFlagsTerminateOnFirstHitEXT` in the raygen means the
 * traversal stops here - so this stage has no attribute to read, no material to fetch and no shading to do. It
 * exists at all because a traced ray needs a hit group in its shader binding table, and it is the stage a
 * LATER step grows: an any-hit shader for alphaMode MASK (`ignoreIntersectionEXT` where the material's alpha
 * cuts the triangle out, the limitation the ray-query form cannot express) and an opacity micromap in front of
 * it belong exactly here.
 *
 * The payload is written here and NOWHERE ELSE on this path: the raygen deliberately does not initialise it (an
 * initialisation that stores the same value the miss shader stores is what the bug in the pass header was - the
 * compiler dropped the redundant store and every escaped ray came back occluded), so this 1.0 and the miss
 * shader's 0.0 are the two halves of the contract.
 */

layout(location = 0) rayPayloadInEXT float payload_occluded;

void main() {
    payload_occluded = 1.0;
}

