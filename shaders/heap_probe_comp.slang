// shaders/heap_probe_comp.slang - the SLANG source of the COMPUTE half of the heap-native probe.
//
// PRODUCES `heap_probe.comp.spv`, the same name the GLSL version produced, so the host side is untouched.
// Ported from shaders/heap_probe.comp (which is a file of its own on the GLSL side too, and for the same
// reason: a compute pipeline is not the graphics probe's pair, and the two have DIFFERENT push layouts - one
// push-constant block per stage means they cannot share a file without one of them being ambiguous).
//
// WHAT IT ANSWERS, in a number rather than an opinion: does the heap-native path work in THIS renderer -
// `descriptor_heap` declarations, a pipeline created with the heap flag and a NULL layout, its parameters
// delivered by vkCmdPushDataEXT, and the two heaps bound in the command buffer that records it? It reads ONE
// texture from the bindless array (sampled through a sampler taken from the SAMPLER heap, because a combined
// image sampler cannot be declared heap-native at all) AND the material table's default record, whose value
// the host knows in advance (white), so the answer has a negative control as well as a positive one.
//
// THE OUTPUT GOES THROUGH A DEVICE ADDRESS, which is the mechanism this file shares with the any-hit shader:
// the GLSL side used GL_EXT_buffer_reference, and Slang spells the same thing as a C-style pointer cast from
// the 64-bit address the host pushes as two 32-bit halves. Verified before this port (see the RT entry in
// docs/slang_migration.md): `(uint*)address` emits `OpConvertUToPtr` into a `PhysicalStorageBuffer` pointer
// under `OpCapability PhysicalStorageBufferAddresses`.
//
// THE ENTRY TAKES NO PARAMETERS: the GLSL declares `layout(local_size_x = 1) in;` and the host dispatches one
// workgroup of one invocation, writing `values[0]` and `values[1]` at fixed positions - there is no index to
// derive, which is why there is no `SV_DispatchThreadID` here either.

#include "heap_access.slang"

// THE MATERIAL LAYOUT IS NOT DECLARED HERE ANY MORE: it is `material_record.glsl`, the one place the
// record's std430 layout lives, and this stage includes it. A second copy is a second thing to keep in
// step with the host's `static_assert(sizeof(material_record) == 80)`, and a copy that drifts indexes
// every material after the first by the wrong STRIDE - which shows up as a wrong texture on every
// material but one, not as a build error.
#include "material_record.glsl"

struct HeapProbeCompPush
{
    uint2 destination;  // the output buffer's device address, as two 32-bit halves (the shape this project pushes)
    uint texture_index; // ABSOLUTE slot of the texture to sample, i.e. heap_slots_textures + the texture's index
    uint sampler_index; // ABSOLUTE slot in the SAMPLER heap, e.g. heap_sampler_texture
};
[[vk::push_constant]] ConstantBuffer<HeapProbeCompPush> pc;

/// @brief the 64-bit device address held in two lanes (little-endian, low half first)
uint64_t probe_device_address(uint2 halves)
{
    return (uint64_t(halves.y) << 32u) | uint64_t(halves.x);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void comp_main()
{
    // WHY THE EXPLICIT-LOD FETCH, when a compute stage IS allowed implicit LOD (TRAP 4): an implicit fetch
    // needs a DERIVATIVE QUAD, and this probe is deliberately ONE invocation in one workgroup - `numthreads(1,
    // 1, 1)` - so Slang refuses the implicit form outright (`E31210: derivative group quad thread count
    // error`, which cost one build). Explicit LOD 0 is the same fetch here, because the probe samples a
    // 1-mip placeholder: the readback below is what says so.
    const float4 sampled = heap_texel_lod0(heap_at<Texture2D<float4>>(pc.texture_index), pc.sampler_index, float2(0.5, 0.5));
    const uint red = uint(clamp(sampled.r, 0.0, 1.0) * 65535.0);
    const uint alpha = uint(clamp(sampled.a, 0.0, 1.0) * 65535.0);
    // ... and the material table's default record, which the host knows the value of
    const float default_base_red = material_at(heap_slots_materials, 0).base_color_factor.x;
    const uint base_red = uint(clamp(default_base_red, 0.0, 1.0) * 65535.0);

    uint* answer = (uint*)probe_device_address(pc.destination);
    answer[0] = red | (alpha << 16u);
    answer[1] = base_red;
}
