# deren

**deren** is a real-time renderer written in modern C++23 (C++20 modules / `.cppm`), built
with CMake 4.3 + Ninja on MSYS2 clang64. Its graphics backend is being separated out
behind an RHI - what is the backend versus what is the engine is still an open question
(see the note below).

> **Renamed on 2026-10-02: this project is `deren`.** It was `vulkan_render` until then, and the
> executable, the version macros, the reference-frame directory and the git repository were renamed
> with it; the working directory is renamed last, from outside a session that holds it open. Two things were deliberately left alone: the C++ module
> namespaces still read `vulkan.*`, and four doxygen groups still read `vulkan_render_*`. Renaming
> those is a separate, still-open decision - it belongs with the backend-boundary question (what is
> the Vulkan backend versus what is the engine), and doing it now would settle that question by
> accident. `ENABLE_VULKAN_RENDERDOC_CAPTURE` also still says what it says: it is RenderDoc's own
> environment variable, not ours.

Current version: **0.3.0** - single source is `project(VERSION)` in `CMakeLists.txt`
(surfaced by `--version`, the startup log banner and the instance's `app_info`); bump it there
and keep this line in sync.

This page is the map: what is worth reading first, the order the rest of the manual is
written in, and where the generated reference begins. The reference half of this PDF
(module / topic / class / file documentation) is mechanical - it is the API surface, not
the reasoning. The reasoning is in the pages below.

## Highlights

The parts of this renderer that carry the most design weight, in the order a reader
usually meets them. Each one links to the page or module that holds the detail.

- **Deferred-only scene path.** The G-buffer path is the ONLY scene path - the forward one
  was removed in commit `78b6737`. Every shading stage in the engine calls one function,
  `shade_surface()` in `shaders/shading.glsl`, and shares the material-surface gather in
  `shaders/surface.glsl`.
- **Temporal anti-aliasing on a 1x G-buffer.** A Halton(2,3) projection jitter, per-pixel
  camera motion vectors written by the G-buffer, and a reprojected, neighborhood-clamped
  history with a view-depth disocclusion guard. TAA is the engine's anti-aliasing because a
  1x G-buffer cannot be multisampled; it replaces a multisampled path rather than adding to
  one.
- **Cascaded shadow maps plus pass-graph-driven reuse.** One layered 2D-array depth map,
  three cascades by default, practical split scheme with lambda 0.75, per-pixel cascade
  selection blended across the boundary. A slot's cascades are re-rendered only when the
  fitted matrices, the caster world matrices, the uploaded skin matrices or a morph-scratch
  revision changed - the skip is byte-identical by construction, and the skin upload has to
  be part of the signal or an animated model silently keeps a frozen map.
- **Clustered light culling.** `shaders/light_cluster.slang` sorts up to 128 punctual lights
  into a 64 px-tile x 16-exponential-depth-slice grid once per frame with one atomic counter
  per cluster. Measured with 64 lights the forward shading pass drops 1.28 -> 0.44 ms and
  the deferred lighting pass 0.25 -> 0.08 ms, with a byte-identical image - the cluster test
  is conservative.
- **Screen-space occlusion with no extra pass.** A golden-angle hemisphere spiral traced
  against the G-buffer depth and normals, folded into `shade_input.ao` so it scales the IBL
  ambient exactly like a baked AO map. No extra pass, no extra render target.
- **Deformation motion vectors.** The previous-frame skin matrices and previous-frame morph
  weights are both stored, so a skinned or morphed vertex reprojects where it actually was.
  See \ref md_docs_2deformation__motion__vectors "Deformation motion vectors" for the
  design, the measurements and the two half-steps it was built in.
- **Mesh shaders.** See \ref md_docs_2mesh__shaders "Mesh shaders" for the geometry
  path, its measurements and the migration that got there.
- **Megalights.** See \ref md_docs_2megalights "Megalights" for the many-light
  sampling, its reservoirs and its temporal reuse.
- **Ray-traced shadows.** `deren.vulkan.pass.ray_traced_shadow` sits beside the cascade path; see
  the module documentation and `docs/reference/` for the source notes it was built from.
- **A toon character pipeline.** Per-material families (base / skin / face / hair / eye /
  cloth) select which shader lanes run; five optional sidecar lanes (diffuse ramp, shadow
  LUT, specular ramp, matcap, SDF lightmap) come from a `<model>.glb.toon.tsv` file so a
  model can be tuned without recompiling; the face terminator is a distance field
  thresholded against the light angle **in the head's own frame**, not against N.L. The
  MMD/VMD motion path (IK, retargeting, morph tracks, physics baking) lives entirely in
  `deren.vulkan.animation.mmd_motion`. A missing sidecar is an empty sidecar - the toon lanes
  simply stay off.
- **Heap-native descriptors.** No descriptor sets: bindings live in a heap the renderer
  manages. \ref md_docs_2descriptor__heap__migration "Descriptor heap migration" and
  \ref md_docs_2descriptor__heap__handover "Descriptor heap handover" are the record of
  how it got there and what it cost.

## How to read this manual

The pages are ordered as a reading path, not alphabetically: conventions and the shader
contract first, then the frame structure, then the frontier subsystems, then the usage
guide, and finally the migration history - which is where the reasons for the current shape
of the code live.

| # | Document | What it answers |
|---|----------|-----------------|
| 1 | \subpage md_docs_2conventions "Conventions" | Naming, file layout and the code rules the rest of the manual assumes. |
| 2 | \subpage md_docs_2shaders "Shaders" | How shaders are written and bound: the pass chain, the shared scene set, the slot conventions. |
| 3 | \subpage md_docs_2pass__io__design "Pass I/O design" | The declared contract a pass states about what it reads and writes, and how it is resolved. |
| 4 | \subpage md_docs_2runtime__split "Runtime split" | How the runtime, the scene tree and the render environment divide the frame between them. |
| 5 | \subpage md_docs_2mesh__shaders "Mesh shaders" | The mesh-shader geometry path: what it replaced, what it measured. |
| 6 | \subpage md_docs_2megalights "Megalights" | Stochastic many-light sampling and its temporal reuse. |
| 7 | \subpage md_docs_2deformation__motion__vectors "Deformation motion vectors" | Why a deforming mesh needs more than a camera motion vector, and what was stored. |
| 8 | \subpage md_docs_2gltf__loader__usage "glTF loader usage" | Loading models, animations, skins, morph targets and punctual lights. |
| 9 | \subpage md_docs_2slang__migration "Slang migration" | Moving the shaders to Slang and compiling to SPIR-V. |
| 10 | \subpage md_docs_2descriptor__heap__migration "Descriptor heap migration" | Replacing descriptor sets with a renderer-managed heap. |
| 11 | \subpage md_docs_2descriptor__heap__handover "Descriptor heap handover" | How the heap is handed to the passes, and the constraints that imposes. |
| 12 | \subpage md_docs_2migration__tradeoffs "Migration tradeoffs" | What each of those migrations cost, and what was given up for it. |
| 13 | \subpage md_docs_2compiler__tolerance "Compiler tolerance" | What the build needs from a toolchain, what a second one (GCC) measured, and what still stops it. |

Three shorter paths, if you are not reading front to back:

- **Run it and see a frame:** the README at the repository root for controls and the config
  reference, then \ref md_docs_2gltf__loader__usage "glTF loader usage" for the model
  side.
- **Extend the renderer** (a new pass, primitive strategy or loader):
  \ref md_docs_2pass__io__design "Pass I/O design",
  \ref md_docs_2runtime__split "Runtime split",
  \ref md_docs_2shaders "Shaders".
- **Understand why the code looks like this:** the four migration pages (9-12 above), in
  order.

## What is in the box

Most modules are independent building blocks that meet only through narrow interfaces, so
you are free to recombine or rewire them.

- `deren.vstd` - the project's STL module (modified from libc++ and trimmed to the project's
  usage; consumed as `import deren.vstd;`, module version 0.1.0a - see `vstd/README.md`)
- `deren.vulkan.core` - instance / device / swapchain / VMA allocator / pipeline / descriptor
  plumbing
- `deren.vulkan.scene_tree` - pure-CPU scene storage (transform hierarchy of scene_node objects
  with abstract primitive leaves)
- `deren.vulkan.primitive` - the GPU primitives (normal / instanced / static draws) plus the
  material / camera / light UBO records of the GPU scene set
- `deren.vulkan.runtime` - the frame facade (per-frame-slot scene resources, granular frame
  phases: poll_events -> recreate_if_minimized -> pace_and_acquire -> begin_recording ->
  record_main_drawcalls -> end_recording -> submit_and_present, plus one-call
  render_frame()), drives the peer scene_tree / primitive modules, debug GUI overlay.
  Shadow + main pass commands are recorded into per-slot secondary command buffers and the
  main pass fans its leaf recording out over the shared task pool (sub_render_task
  batches); each recording worker gets its own render_environment (thread-local
  pipeline-bind state)
- `deren.vulkan.render_environment` - per-recording-session render state: the session's command
  buffer, the available named pipelines (pointer to the runtime's stable name table), the
  session's default pipeline and a deduplicated binder (std::function, injected by the
  runtime) that primitives call through draw(render_environment&). Holds no Vulkan module
  dependency.
- `deren.vulkan.animation` - animation::controller: glTF keyframe playback / skinning / morphs on
  the runtime scene tree (heavy animations fan per-source sampling over a small
  deren.utility:thread_pool), plus the MMD/VMD motion path in `deren.vulkan.animation.mmd_motion`
- `deren.gltf_loader` - pure-CPU glTF/GLB loading: meshes, keyframe animation, skins, morph
  targets, cameras and punctual lights (KHR_lights_punctual); world-AABB + loader
  diagnostics
- `deren.chores` - demo bootstrap helpers for main(): startup config analysis (config + argv
  merge, shaders/model location), pipeline setup, instancing stress grid, shader loading
- `deren.utility` - log/panic, handle distribution, thread pool (deren.utility:thread_pool), BVH, data
  blocks, frame_clock, pmr routing
- `deren.app_config` - TOML startup configuration merged with argv

Modular composition is the point, not a side effect:

- `deren.gltf_loader`, `deren.app_config` and `deren.utility` are **pure CPU with no Vulkan dependency** -
  standalone libraries that embed into any host application;
- `deren.vulkan.animation` is **format-neutral and runtime-agnostic**: it drives whatever scene
  storage a caller injects through the `backend` surface and initializes from any loader
  whose data satisfies the structural `source` concept (it imports no loader and no
  `deren.vulkan.runtime`);
- `deren.vulkan.core` / `deren.vulkan.runtime` are a configurable facade (`core_create_info`, granular
  per-frame phase calls) - the demo entry point (`main.cpp` + `deren.chores`) is a thin glue layer
  on top and can be replaced wholesale.

Use the modules as-is to extend this renderer (new pass / primitive strategy / loader) or
link only the ones you need into your own project.

Module reference is grouped under the `vulkan_core`, `vulkan_runtime`,
`vulkan_runtime_scene_tree`, `vulkan_render_environment`, `vulkan_render_resource`,
`vulkan_pass`, `vulkan_animation`, `vulkan_gui`, `vulkan_math`, `gltf_loader`, `chores`,
`utility` and `app_config` groups; the GLSL / Slang shaders are collected under the
`shaders` group (see the shader reference page for the pass chain, the shared scene set and
the conventions).

## Rendering

PBR (Cook-Torrance + image-based lighting), directional shadows with manual
percentage-closer filtering, skybox, keyframe animation / skinning / morph playback, and a
Dear ImGui debug overlay that is on by default (`[gui] show = false` in config disables it).

The frame is also **measured**: with `[render] gpu_timings` (on by default) each pass
boundary writes a GPU timestamp into a per-frame-slot query pool, and the values are read
back after the slot completed and averaged over a 60-frame window (logged and shown in the
overlay). Every rendering feature below is steered by those numbers rather than by
guesswork.

The renderer was evolved to a deferred pipeline one milestone at a time, and **the G-buffer
path is now the ONLY scene path** - the forward one was removed in commit `78b6737` ("the
G-buffer path is the only scene path, and the forward one is gone") - so everything below
describes what the engine does rather than what it is becoming. **M1 (done)** is the
G-buffer: the opaque pass stores the surface (albedo/metallic, world normal/roughness,
material id/AO/flags) in three 1x targets, with a channel debug view. **M2 (done)** is the
deferred lighting stage: `shaders/deferred.slang` shades every pixel from those targets and
adds the result into the HDR target (sky where no geometry wrote depth), through
`shade_surface()` - the SINGLE lighting entry point (`shaders/shading.glsl`) that every
shading stage in the engine now calls - with emissive added by the base pass and the
material-surface gather shared in `shaders/surface.glsl`. While the forward path still
existed the two were A/B references and agreed to 0.32/255 mean absolute luminance
difference on Sponza; that number is a HISTORICAL measurement, not a current comparison -
there is no longer a second path to compare against, the shared shading code simply IS the
path. **M3 (done)** is temporal anti-aliasing on that path: a Halton(2,3) projection jitter,
per-pixel camera motion vectors written by the G-buffer, and `shaders/taa.slang` resolving
the jitter against a reprojected, neighborhood-clamped history (with a view-depth guard for
disocclusions) - measured against the same frame without TAA, 1.05/255 mean difference with
9% less high-frequency energy, and, as the measurement was taken at the time, the
then-still-present forward path byte-identical. TAA is the engine's anti-aliasing because a
1x G-buffer cannot be multisampled. **M4 (done)** is cascaded shadow maps: the sun's shadow
pass fills a layered 2D-array depth map (1..4 cascades, three by default), each cascade
fitting its own light-space box to its own slice of the view range (practical split scheme,
lambda 0.75), with the fragment shader selecting its cascade per pixel from the view depth
and blending across the boundary - one cascade is byte-identical to the pre-M4 single-map
path. **M5 (done)** is clustered light culling: `shaders/light_cluster.slang` (the engine's
first compute pipeline, on the graphics queue) sorts up to 128 punctual lights into a 64
px-tile x 16-exponential-depth-slice grid once per frame with one atomic counter per
cluster, and the shading stage loops only its own cluster's list - measured with 64 lights,
the forward shading pass drops 1.28 -> 0.44 ms and the deferred lighting pass 0.25 -> 0.08
ms with a byte-identical image, since the cluster test is conservative. **M6 (done)** is
screen-space ambient occlusion in the deferred lighting stage: a golden-angle hemisphere
spiral traced against the G-buffer depth and normals, folded into the `shade_input` ao so it
scales the IBL ambient exactly like a baked AO map, with no extra pass or render target and
an off path that is byte-identical to the pre-M6 frame. **M7 (done)** is the collation pass:
the shadow map size became a config knob, the documented example config is parsed and pinned
by a unit test, and the configuration / GUI / reference documentation cover every milestone.
**Shadow-map reuse** (the first optimization that reads the pass graph instead of shrinking
it) followed: a slot's cascade maps are re-rendered only when the fitted matrices (a refit
counter) or the caster geometry (an XXH3-64 fingerprint of every caster's world matrix, of
the uploaded skin matrices and of a morph-scratch revision) changed since that slot last
rendered them, so the skip is byte-identical by construction - a skinned caster keeps a
constant world matrix, which is why the skin upload has to be part of the signal or an
animated model silently keeps a frozen map. Still ahead on this path: alpha-blended geometry
in the deferred path (it is composited outside the G-buffer and so writes no motion vector
at all), and anything else that moves inside its own object space. What a DEFORMING mesh
needs is done: the previous-frame skin matrices and the previous-frame morph weights are
both stored, so a skinned or morphed vertex reprojects where it actually was - see
\ref md_docs_2deformation__motion__vectors "Deformation motion vectors" for the design,
the measurements and the two half-steps it was built in.
