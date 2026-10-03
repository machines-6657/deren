#!/bin/sh
# Recompile every shader in shaders/ to SPIR-V binaries, in place.
#
# The BUILD already does this (see the SLANG -> SPIR-V section of CMakeLists.txt): `cmake --build`
# recompiles every .spv from its source, and the resulting directory is mirrored next to the executable,
# which is the copy the runtime loads. This script is the escape hatch for a machine without CMake - it
# produces the same binaries in the same place.
#
# EVERY SHADER IS SLANG NOW. The canonical list of what builds which .spv, with which entry point and stage,
# is VR_SLANG_SOURCES in CMakeLists.txt; the table below MIRRORS it and every flag matches the CMake rule
# exactly. The retired GLSL stage sources live in shaders/glsl.old/ and are NOT compiled by anything; the
# shared bodies the Slang leaves include (surface.glsl, shading.glsl, sky.glsl, ibl_specular.glsl,
# heap_slots.glsl, heap_slot_constants.glsl) stay in shaders/ and are inputs to every one of these
# compilations. POSIX-sh twin of compile_shaders.ps1, so the two tables must stay identical.
#
# Usage:
#     sh shaders/compile_shaders.sh      (from the project root, or anywhere)
#     ./shaders/compile_shaders.sh       (when the file is executable)

set -eu

script_dir=$(cd "$(dirname "$0")" && pwd)

# ---- locate slangc: PATH first, then VULKAN_SDK (Windows layout Bin\slangc.exe) ----
slangc_path=""
if command -v slangc >/dev/null 2>&1; then
    slangc_path=$(command -v slangc)
elif [ -n "${VULKAN_SDK:-}" ]; then
    for candidate in "$VULKAN_SDK/Bin/slangc.exe" "$VULKAN_SDK/bin/slangc" "$VULKAN_SDK/Bin/slangc"; do
        if [ -x "$candidate" ]; then
            slangc_path=$candidate
            break
        fi
    done
fi
if [ -z "$slangc_path" ]; then
    echo "slangc not found. Install Slang or the Vulkan SDK, or add slangc to PATH." >&2
    exit 1
fi

# ---- source:entry:stage:output, the shape VR_SLANG_SOURCES uses ----
#
# EXACTLY the CMake flags, including the deliberate absence of -fp-mode: the DEFAULT mode is the one whose
# codegen matches glslang's ('-fp-mode precise' made nine gate scenarios differ, measured).
count=0
while IFS=: read -r src entry_point stage dst; do
    [ -n "$src" ] || continue
    "$slangc_path" "$script_dir/$src" -I "$script_dir" -allow-glsl -DVR_SLANG \
        -target spirv -profile spirv_1_6 -capability spvDescriptorHeapEXT \
        -spirv-resource-heap-stride 64 -spirv-sampler-heap-stride 32 \
        -fvk-use-gl-layout -matrix-layout-column-major \
        -entry "$entry_point" -stage "$stage" -o "$script_dir/$dst"
    echo "compiled: $src ($entry_point/$stage) -> $dst"
    count=$((count + 1))
done <<'SLANG_SOURCES'
unlit.slang:main:fragment:unlit.frag.spv
character_forward.slang:main:fragment:character_forward.frag.spv
goo_toon.slang:goo_toon_frag_main:fragment:goo_toon.frag.spv
toon_screen_rim.slang:main:fragment:toon_screen_rim.frag.spv
goo_rim.slang:main:fragment:goo_rim.frag.spv
overlay.slang:main:fragment:overlay.frag.spv
fxaa.slang:main:fragment:fxaa.frag.spv
upscale.slang:main:fragment:upscale.frag.spv
post.slang:main:vertex:post.vert.spv
post.slang:frag_main:fragment:post.frag.spv
gbuffer_debug.slang:main:fragment:gbuffer_debug.frag.spv
taa.slang:main:fragment:taa.frag.spv
gbuffer.slang:main:fragment:gbuffer.frag.spv
pbr.slang:main:fragment:pbr.frag.spv
pbr.slang:mesh_main:mesh:pbr.mesh.spv
pbr.slang:meshlet_main:mesh:pbr.meshlet.spv
pbr.slang:outline_mesh_main:mesh:outline.mesh.spv
outline.slang:frag_main:fragment:outline.frag.spv
shadow.slang:main:fragment:shadow.frag.spv
shadow.slang:mesh_main:mesh:shadow.mesh.spv
shadow.slang:meshlet_main:mesh:shadow.meshlet.spv
light_cluster.slang:main:compute:light_cluster.comp.spv
heap_probe.slang:main:vertex:heap_probe.vert.spv
heap_probe.slang:frag_main:fragment:heap_probe.frag.spv
heap_probe.slang:mesh_main:mesh:heap_probe.mesh.spv
heap_probe_comp.slang:comp_main:compute:heap_probe.comp.spv
rt_shadow.slang:chit_main:closesthit:rt_shadow.rchit.spv
rt_shadow.slang:miss_main:miss:rt_shadow.rmiss.spv
rt_shadow.slang:rgen_main:raygeneration:rt_shadow.rgen.spv
rt_shadow.slang:ahit_main:anyhit:rt_shadow.rahit.spv
compute_skin.slang:comp_main:compute:compute_skin.comp.spv
mask_bake.slang:comp_main:compute:mask_bake.comp.spv
megalights_temporal.slang:comp_main:compute:megalights_temporal.comp.spv
megalights_trace.slang:comp_main:compute:megalights_trace.comp.spv
deferred.slang:main:fragment:deferred.frag.spv
SLANG_SOURCES

echo "all shaders compiled successfully ($count from Slang)."
