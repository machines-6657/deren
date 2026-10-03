/**
 * @defgroup shaders Shaders (Slang)
 * @brief The Slang pass sources, the descriptor/push-constant contracts they share with the C++
 *        records, and how they are compiled.
 *
 * The renderer has no shader reflection and no runtime shader compilation: `dsh`-style `.spv`
 * files sit next to their sources, are loaded by `deren::chores::setup_pipeline()` and are turned into
 * `vk_pipeline` objects through the `runtime::make_*_pipeline()` calls (which own the render
 * state: color format, depth, and the shared scene pipeline layout).
 *
 * @section shader_passes The pass chain
 *
 * THE NAMES BELOW ARE STAGE NAMES, and they are also the `.spv` file names the runtime loads. The SOURCES
 * are Slang (`.slang` files, one per shader family - a family with several stages has several entries in one
 * file, which is why this list names stages rather than files). The former GLSL sources are archived in
 * `shaders/glsl.old/` and are not built; the four shared bodies still carry GLSL syntax and are `#include`d
 * by the Slang leaves (see docs/slang_migration.md for how that works).
 *
 * EVERY GEOMETRY STAGE BELOW IS A MESH STAGE, AND THE MESH FORM IS THE ONLY FORM: `VK_EXT_mesh_shader` is a
 * REQUIREMENT since docs/mesh_shaders.md step 4, so the geometry vertex entries (`pbr.vert`, `shadow.vert`) no longer
 * exist - neither as files nor in any registry. What runs is the MESHLET entry where one was built (one workgroup per
 * 85-triangle meshlet, culled against the pass's frustum) and the MESH entry otherwise (one workgroup per 85
 * triangles): the stage fetches its own vertices by device address because there is no input assembler, and every
 * mesh form shares the SAME fragment stage and the same projection body the vertex entry used to call - which is why
 * the capture gate is byte-identical to the references captured while that path was still there, and why removing it
 * changed no pixel. The fullscreen `post.vert` passes are not geometry stages and have no mesh form.
 *
 * @code
 *  light_cluster.comp            compute: sorts the punctual lights into the frame cluster grid (M5)
 *  shadow.mesh.spv + shadow.frag depth-only, from the sun, into the per-slot shadow map array
 *                                (one layer per cascade, `[render] shadow_cascades` = 1..4)
 *                                [or shadow.meshlet.spv: one workgroup per meshlet]
 *        |
 *  surface.glsl                  (include) the shared material-surface gather, and
 *  shading.glsl                  (include) the shared lighting - used by the G-buffer path below and
 *                                by the transparent pass
 *        |
 *  pbr.mesh.spv + gbuffer.frag   opaque geometry -> three 1x G-buffer targets + a 1x depth image
 *                                [or pbr.meshlet.spv: one workgroup per meshlet, camera-culled]
 *                                (albedo+metallic, world normal+roughness, material id+AO+flags),
 *                                the motion-vector target, and the emissive term ADDED into the
 *                                scene color (5th attachment)
 *        |
 *  post.vert + deferred.frag     fullscreen: read the G-buffer + depth, rebuild the world position
 *                                from the depth, light the surface with shading.glsl, add the result
 *                                into the scene color - sky where no geometry wrote depth
 *        |
 *  pbr.mesh.spv + pbr.frag/unlit.frag  the TRANSPARENT pass: alphaMode BLEND geometry, shaded while it
 *                                [or pbr.meshlet.spv]
 *                                draws and blended over the shaded frame (a G-buffer cannot carry a
 *                                blended surface)
 *        |
 *  post.vert + taa.frag          fullscreen: blend the scene color with the reprojected, clamped
 *                                history into the HDR target the post chain reads (optional; the
 *                                runtime then copies that into the history image for the next frame)
 *        |
 *  post.vert + gbuffer_debug.frag  (debug alternative, never together with the lighting stage) show
 *                                one stored channel instead of lighting it
 *        |
 *  post.vert + post.frag         mode 0 bright-pass prefilter (HDR -> bloom L0)
 *                                mode 1 downsample x3 (L0 -> L1 -> L2 -> L3)
 *                                mode 2 composite (HDR + bloom -> exposure -> ACES -> display)
 *        |
 *  post.vert + fxaa.frag         optional FXAA over the display-referred LDR image
 *        |
 *  Dear ImGui                    overlay, drawn on the final 1x swapchain image
 * @endcode
 *
 * That is the whole frame: `pbr.mesh.spv + gbuffer.frag` is the engine's only scene path (the opaque
 * surface write), and everything after it shades or composites the result. A 1x G-buffer cannot be
 * multisampled without per-sample shading, so there is no MSAA - `taa.frag` is the anti-aliasing.
 *
 * @section shader_taa Temporal anti-aliasing
 *
 * The scene renders at 1x, so `shaders/taa.frag` is the engine's anti-aliasing instead of MSAA:
 * `runtime::set_taa` jitters the projection by a Halton(2,3) sub-pixel offset every frame (the
 * G-buffer and the lighting stage both see the jittered projection - geometry and the depth
 * reconstruction agree about where each sample is), the G-buffer writes a motion vector per pixel
 * (current - previous, in UV space), and the resolve blends the current frame with the history
 * sampled at `uv - velocity`, clamped into the current 3x3 neighborhood, with a view-depth guard for
 * disocclusions. The result goes into the HDR target the post chain reads, and the runtime copies it
 * into the history image for the next frame that renders that swapchain image.
 *
 * Two invariants are worth stating because breaking either one is hard to see and expensive to find:
 * - the motion vectors come from the UNJITTERED view-projection pair (a jitter in there is read as
 *   camera motion and reprojects the history by up to a pixel every frame), and
 * - everything that is not the rendering transform itself uses the unjittered matrices: the shadow
 *   frustum fit, the BVH cull frustum, the depth-linearization terms. Letting the jitter into the
 *   shadow fit in particular re-quantizes the light-space box to whole texels every frame, so the
 *   shadow map's texel grid alternates between two alignments and a grazing-angle surface flickers
 *   between lit and shadowed - the TAA history then averages that flicker into a dark band.
 *
 * The motion vectors carry CAMERA motion, RIGID object motion and a DEFORMING mesh's own movement. The
 * camera half is the unjittered view-projection pair; the object half is binding 13, where the geometry
 * stage (`pbr_shade_vertex` in `shaders/pbr.slang` - one body for every entry in the file) reads the world
 * matrix this draw had one frame ago (`runtime::advance_motion_transforms()` publishes
 * it, once per frame, before anything is recorded) and passes the resulting previous world position down
 * as `v_prev_world_pos`. A vertex's movement INSIDE its own object space is the second half of that
 * position, and both of its sources are stored: the four joint matrices as they were one frame ago, from
 * a per-frame heap family of their own (`runtime::advance_motion_deformations()` publishes them into the
 * CURRENT frame slot's buffer, so the shader reads them at the same `skin_base` and the same frame slot
 * it already carried), and the morph weights as they were one frame ago, which live in a second weight
 * region of each morph block (see `runtime::morph_scratch()` for the writer's side of that contract).
 * NOT COVERED, stated rather than implied: alpha-blended geometry, which is composited outside the
 * G-buffer and writes no motion vector at all - see `docs/deformation_motion_vectors.md`, which carries
 * the measurements for both halves that ARE covered.
 *
 * Which pipeline a primitive draws with is decided per leaf: a default-semantics primitive asks
 * the pass for its default pipeline, so the same geometry renders through `pbr` (lit), `unlit`
 * (flat base color) or the G-buffer write without re-baking anything - see the runtime's render mode
 * combo. The G-buffer pipeline is not in the runtime's named pipeline cache: it declares four color
 * attachments, so it is only valid inside the G-buffer instance, and the pass hands it to
 * default-semantics leaves under `runtime::gbuffer_pipeline_name` ("gbuffer").
 *
 * @section shader_surface The shared material-surface gather (surface.glsl)
 *
 * `pbr.frag` and `gbuffer.frag` answer the same question - "what is this surface made of?" - and both
 * do it through `gather_surface()` in `shaders/surface.glsl`: the material table lookup, the glTF
 * alpha tests (the MASK `discard` lives in there, so no pass can forget it), the tangent-space normal
 * map with the double-sided flip, and the texture-derived factors. The include declares the descriptor
 * bindings and the push constant block it depends on (bindings 1 and 5, the shared material push
 * block), so a shader including it must not declare them again.
 *
 * @section shader_shading The shared lighting (shading.glsl)
 *
 * `shade_surface()` in `shaders/shading.glsl` is the engine's SINGLE lighting entry point: the
 * directional sun through the shadow test, the punctual lights, the split-sum IBL ambient, the
 * selectable BRDF/diffuse presets and the cel-shading bands. It takes a `shade_input` - world position,
 * normal, albedo, emissive, metallic, roughness, AO - which the caller fills from whatever it has (the
 * deferred path from G-buffer texels, a forward-style stage from its interpolated fragment inputs), so
 * the lighting cannot tell where the surface came from. The include reaches the resources it needs
 * through the descriptor heap, with `shaders/heap_slots.glsl` naming the slots (the camera UBO, the IBL
 * maps, the light UBO and the shadow map).
 *
 * This section used to read ONE FUNCTION, TWO PATHS - `pbr.frag` (forward) and `deferred.frag`
 * (deferred) as two callers, and their agreement as the A/B reference. THE FORWARD SCENE PATH IS GONE
 * (commit `78b6737`, "the G-buffer path is the only scene path, and the forward one is gone"), so there
 * is one path now and nothing to compare it against; the measurements taken while there were two are in
 * `docs/mainpage.md`'s M2 note.
 *
 * @section shader_sky The shared sky (sky.glsl)
 *
 * `sky_color()` is a pure function of a world-space direction with no bindings at all, and the deferred
 * path calls it for the pixels whose G-buffer depth is still the far plane. It was shared with a second
 * background while the forward path existed (`skybox.frag`, since deleted); one function with one
 * caller is still one function, and the point stands - the sky cannot drift from the geometry drawn in
 * front of it.
 *
 * @section shader_clusters Clustered light culling (M5)
 *
 * `shaders/light_cluster.slang` runs once per frame on the graphics queue, one invocation per
 * cluster: the screen cut into 64 px tiles and 16 EXPONENTIAL depth slices. It rebuilds the
 * cluster's view-space box (its tile's corner rays unprojected at the slice's near and far depth)
 * and appends every light whose bounding sphere intersects that box to the cluster's fixed-capacity
 * index row (`binding 12`, one atomic counter per cluster in `binding 11`). The shading stage
 * computes the same cluster from `gl_FragCoord` and its view depth - `cluster_slice_of()` in
 * `shading.glsl` and the compute shader MUST agree on the slice boundaries - then loops only that
 * row. The `cluster_grid.w` lane switches between the clustered list and the brute-force loop over
 * `light_count` lights, which is what the clustered path is verified against: the sphere test is
 * conservative, so the two produce byte-identical images. The grid dims and the slice depth range
 * ride the light UBO (`cluster_grid` / `cluster_depth`, appended AFTER the light array so the array
 * offset every other user of the block encodes stays 352).
 *
 * @section shader_ssao Screen-space ambient occlusion (M6)
 *
 * `deferred.frag` computes the AO itself, from the depth and normal it already reads: a golden-angle
 * hemisphere spiral around the pixel's view-space position, rotated per pixel by a hash of
 * `gl_FragCoord`, each sample projected back to screen and compared against the depth buffer (a
 * sample is occluded when the stored surface is closer to the camera and inside the radius, with the
 * range check fading the contact out). View-space z is negative in front of the camera, so "closer"
 * is a GREATER z - the sign that is easy to get backwards. The result multiplies the material's baked
 * AO into `shade_input.ao`, which `shade_surface()` applies to the IBL ambient (diffuse and
 * specular) and never to the direct sun. With an intensity of 0 the term returns exactly 1.0, so an
 * SSAO-off frame is bit for bit the pre-M6 frame. Parameters ride the deferred pass's push block
 * (the 4x4 inverse view-projection plus the `vec4 ssao`), so nothing new is bound. It is deliberately
 * hemisphere SSAO rather than horizon-based GTAO, and being screen-space it cannot see occluders off
 * screen - the usual set of approximations.
 *
 * @section shader_bindings The shared scene block in the resource heap
 *
 * THERE IS NO DESCRIPTOR SET HERE ANY MORE. Every shader in the main pass reads the same part of the
 * frame's bound resource heap: the grid slot each row below names, addressed either by a fixed offset
 * (the frame-invariant entries) or by the slot the stage pushes (the per-frame and per-generation
 * ones). The numbers in the first column are the historical set-0 binding numbers and are kept because
 * they are the order the heap grid was laid out in and the names the logs use; the types are what the
 * heap descriptor carries:
 *
 * | binding | contents | type | written by |
 * |---------|----------|------|------------|
 * | 0 | `CameraUBO` (view, proj, camera_pos) | uniform buffer | `pace_and_acquire()` per frame slot |
 * | 1 | `textures[]` | sampled image array (bindless) | texture upload / `set_textures` |
 * | 2 | `env_sampler` prefiltered environment | cubemap | `set_ibl()` |
 * | 3 | `irradiance_sampler` | cubemap | `set_ibl()` |
 * | 4 | `brdf_lut_sampler` | 2D LUT | `set_ibl()` |
 * | 5 | `Materials` table | storage buffer | `register_material()` |
 * | 6 | `InstanceTransforms` | storage buffer | `make_instanced_primitive()` |
 * | 7 | `LightUBO` | uniform buffer | `enable_shadows()` + per-frame lanes |
 * | 8 | `shadow_map` | `sampler2DArrayShadow` | the shadow pass |
 * | 9 | `SkinMatrices` | storage buffer | `set_skin_matrices()` |
 * | 10 | `MorphData` | storage buffer | `morph_scratch()` |
 * | 11 | `ClusterCounts` (`uint counts[]`) | storage buffer | the cluster compute pass (read: fragment) |
 * | 12 | `ClusterIndices` (`uint indices[]`) | storage buffer | the cluster compute pass (read: fragment) |
 * | 13 | `PreviousTransforms` (`mat4 matrices[]`) | storage buffer | `runtime::advance_motion_transforms()` per frame slot |
 *
 * @section shader_cascades Cascaded shadows (M4)
 *
 * `shadow_map` is a 2D ARRAY of `shadow_cascades` layers rather than an array of separate samplers,
 * because the layer is chosen per FRAGMENT: indexing a sampler array dynamically needs a
 * dynamically uniform index, while a texture-array layer is just a coordinate - so the shadow test
 * can pick its cascade per pixel. `LightUBO` carries one `light_view_proj` matrix per cascade plus
 * `cascade_splits` (the view-space far distance of each cascade), `cascade_texel_world` (the world
 * size of one texel of that cascade, which drives the world-space normal offset) and
 * `cascade_count` / `cascade_blend`. `calc_shadow()` computes the receiver's view-space depth,
 * picks the cascade whose `[splits[i-1], splits[i]]` range contains it, samples that layer with
 * `calc_shadow_cascade()`, and - inside the last `cascade_blend` fraction of the range - samples
 * the next cascade too and mixes the two, so the resolution/offset step between cascades does not
 * show as a line. A receiver outside its cascade's fitted box is reported lit: each cascade's map
 * covers its own fitted box only, and `runtime::update_shadow_frustum()` keeps those boxes on the
 * part of the scene the camera can see. The shadow pass renders the same caster set once per
 * cascade, each into its own array layer with its own `push_constant` cascade index - `shadow.slang`'s
 * geometry entries declare the material fields AND that trailing `uint cascade` in ONE block, because a heap
 * pipeline has one push block per stage file, and the shared layout's single range covers both
 * (`scene_push_constant_size` + `scene_cascade_push_size`).
 *
 * Bindings 0/7/8/9/10 are per frame slot, so a frame in flight never shares a buffer with the
 * frame being written. Indexing `textures[]` with a value from the material table is what needs
 * `#extension GL_EXT_nonuniform_qualifier : enable` in pbr.slang / unlit.slang / shadow.slang.
 *
 * @section shader_push Push constants
 *
 * Material push constants carry `material_index`, the draw `flags` (bit0 = instanced), the
 * `skin_base` / `morph_*` / `instance_base` indices and the per-model `model` matrix
 * (`material_push_constants` in `vulkan_primitive`). The vertex and fragment stages of one
 * pipeline share a single push-constant range, so any stage that does not need a field still
 * declares it to keep the block layout identical - a mismatch here is silent corruption, not a
 * compile error. The post/FXAA pipelines are separate: they use `post_push_constants` (exposure,
 * bloom intensity/threshold, mode, encode_gamma, FXAA knobs) on their own layout, and the G-buffer
 * debug view uses its own `gbuffer_debug_push_constants` (channel selector + the two projection
 * terms that linearize depth) on its own layout.
 *
 * @section shader_compile Compiling
 *
 * The build owns this step: `cmake --build` regenerates every `.spv` from its `.slang` source (and from the
 * shared includes it lists as dependencies), so editing a shader is just editing a shader. The binaries
 * are **not** tracked in the repository and are not hand-synced - they are a build output, and the
 * directory is mirrored next to the executable, which is the copy the runtime loads
 * (`deren::chores::locate_shaders_dir` prefers a `shaders/` sibling of the running binary over anything found
 * by walking up from the working directory). **slangc is required, not optional** - every stage is built from
 * a `.slang` source and the rule has no GLSL fallback any more, so a configuration without the compiler
 * fails loudly instead of compiling modules the project no longer uses. The retired GLSL stage sources are
 * archived in `shaders/glsl.old/`; the shared bodies (`surface.glsl`, `shading.glsl`, `sky.glsl`,
 * `ibl_specular.glsl`, `heap_slots.glsl`, `heap_slot_constants.glsl`) stay in `shaders/`, because the Slang
 * leaves include them.
 *
 * `shaders/compile_shaders.ps1` / `.sh` remain as a manual escape hatch for a machine without CMake -
 * they compile in place, which is what the build does too:
 *
 * @code
 *  powershell -ExecutionPolicy Bypass -File shaders/compile_shaders.ps1   # Windows
 *  sh shaders/compile_shaders.sh                                          # POSIX
 * @endcode
 *
 * Both scripts mirror `CMakeLists.txt`'s `VR_SLANG_SOURCES` (source, entry point, stage, output) and pass the
 * same flags, including `-I shaders/`, which is what lets a leaf `#include "surface.glsl"`. They are verified
 * by running them and comparing their output with the build's, byte for byte.
 *
 * @section shader_conventions Conventions and pitfalls
 *
 * - **ASCII only.** The Doxygen LaTeX manual runs the shader comments through pdflatex; a stray
 *   non-ASCII character (an em dash, a narrow no-break space) aborts the PDF build. Keep comments
 *   and docs ASCII.
 * - **`const` goes first.** GLSL wants `const float x`, not `float const x`, and the east-const form is rejected.
 * - **Keep the interface matching.** A VERTEX pipeline derived its input layout from the shader's inputs
 *   (64-byte interleaved stride, locations 0,1,2,4,5), which is why those entries declared the unused inputs and kept
 *   them alive in a never-taken branch; the geometry stages are mesh stages now (docs/mesh_shaders.md step 4), so
 *   nothing derives an input layout from them and the fetch reads the same 64-byte record by device address instead.
 *   The rule that survives is the fragment one: a fragment output must match the pipeline's
 *   color format, which is why the composite and the FXAA pass are separate pipelines.
 * - **A multi-target pipeline must not use the forward blend state.** `make_color_blend_attachment()`
 *   blends with src alpha, which is the right convention while alpha means coverage (opaque draws
 *   have alpha 1, so the math reduces to an overwrite). In a G-buffer alpha carries DATA (metallic,
 *   roughness, flags), so the same state scales the surface by its own alpha and mixes it with the
 *   cleared target - a fully-rough fragment would erase its own albedo. G-buffer pipelines pass
 *   `color_blending = false` and use `make_color_blend_attachment_opaque()`.
 * - **A ray payload is written by the stage that ENDS the ray, and never pre-initialised by the raygen.**
 *   A raygen that stores the same value the miss shader stores makes the miss shader's store redundant;
 *   this device's compiler then drops it and reads the payload back uninitialised, so every escaped ray
 *   came back classified as occluded and the sun was killed on all the sunlit ground. One store per path -
 *   `rt_shadow.rchit` writes 1.0 for a hit, `rt_shadow.rmiss` writes 0.0 for a miss - and no raygen store.
 *   The four-arm measurement that isolated it is in `vulkan/pass/ray_traced_shadow.cppm`.
 * - **Comments here are the reference.** Every non-obvious decision (bias choices, guards against
 *   NaN at grazing angles, banding, the gamma/encode split) is documented where it is implemented,
 *   and those comments are what Doxygen shows for the matching symbol.
 */
