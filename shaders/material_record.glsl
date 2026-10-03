// shaders/material_record.glsl - THE MATERIAL RECORD, declared where BOTH a scene stage and a fullscreen
// stage can reach it.
//
// WHY IT IS A FILE OF ITS OWN, and this is the constraint that forced it: a fullscreen stage cannot include
// `surface.glsl`. That file declares the shared scene push block as well as this struct, and the block is
// `mesh_stage_block_size` = 144 bytes - over the 128-byte `max_push_bytes` the heap-native pipeline path
// allows - because it carries the mesh geometry lanes. So every fullscreen stage in this renderer
// (`deferred`, `taa`, `gbuffer_debug`, `post`) declares a push block of its own, and any of them that needs
// the MATERIAL TABLE had no way to name the type it reads. This file is that way.
//
// IT IS ONLY A LAYOUT: the struct, nothing else - no heap block, no push block, no accessor. The table is
// reached through `material_at(slot, index)`, which `heap_access.slang` defines, so a stage that wants the
// table includes the shim and this file and is done.
//
// THE SPELLINGS ARE GLSL'S (`uvec4`, `vec4`) ON PURPOSE: under `-allow-glsl`, Slang accepts them and keeps
// their GLSL meaning, which `surface.glsl` already relies on - it is compiled by the Slang leaves today and
// uses these same types. One spelling, one layout, both consumers.
//
// THE LAYOUT MUST MATCH `deren::vulkan::material_record` (std430, 80 bytes, `static_assert`ed on the host side).
// A field added here without the host - or the host without here - moves every material's texture indices,
// and the failure is a wrong texture rather than a build error.
//
// AN INCLUDE GUARD, because this file is reached by two routes that meet: `surface.glsl` includes it, and a
// stage that needs the material table but CANNOT include `surface.glsl` (a fullscreen one - see above)
// includes it directly. A stage that took both routes would declare the struct twice.

#ifndef DEREN_MATERIAL_RECORD_GLSL
#define DEREN_MATERIAL_RECORD_GLSL

struct Material {
    uvec4 tex_indices; // albedo, metallic-roughness, normal, occlusion (indices into textures[])
    uint emissive_index;
    float alpha_cutoff;       // alphaMode MASK threshold
    float occlusion_strength; // mix(1, sampled AO, strength)
    uint toon_family;         // the toon material family (see gltf_loader's toon_family_of); 0 == none.
                              // It completes the std430 group of four uints that starts at emissive_index, so
                              // naming it changes no offset - see material_record's note in
                              // vulkan/primitive/primitive.cppm.
    vec4 base_color_factor;
    vec4 emissive_factor;
    float metallic_factor;
    float roughness_factor;
    float normal_scale;
    uint flags; // bit0: normal map, bit1: occlusion map, bit2: emissive map, bit3: double-sided,
                // bit4: alphaMode MASK, bit5: alphaMode BLEND,
                // bit6: the EYE-DARK overlay, bit7: the HAIR-SHADOW overlay (see overlay.slang and
                //       `deren::gltf::overlay_kind`: the article's two framebuffer multiplies compute different
                //       multipliers - one from the mask, one from `_DayStrength` - so the shared overlay
                //       fragment stage is told which it is drawing here rather than by a second pipeline),
                // bit8: the article's own TRANSPARENT variant of the toon material (`_TRANSPARENT_ON`), which
                //       its sidecar selects with `_SrcBlend 5 / _DstBlend 10` - see `toon_inputs::alpha_blend`
                //       in vulkan/primitive/primitive.cppm. SEPARATE from bit5 because the two are different
                //       statements: bit5 is glTF's coverage rule (alphaMode BLEND) and this one is the toon
                //       material's blend state, and the character-forward stage writes a real output alpha for
                //       either of them and 1.0 for everything else.
    // ---- THE TOON SLOTS: what glTF's five cannot reach (see material_record in primitive.cppm) ----
    // x = diffuse ramp, y = shadow LUT, z = specular ramp, w = matcap. THE WHITE FALLBACK (element 0) IS THE
    // "DO NOT READ" VALUE and that is the contract rather than a convention: a lane holds a real index only
    // when the material has that map AND the artist's `_Use` flag is on. So `toon_indices.x != 0u` IS "read
    // the ramp", no enable word is needed, and a ramp lookup against white - which would be a CONSTANT rather
    // than a no-op - cannot happen by accident.
    uvec4 toon_indices;
};

#endif // DEREN_MATERIAL_RECORD_GLSL
