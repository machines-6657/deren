# MegaLights: how UE 5.8.2 lights many shadowed punctual lights, and what this renderer would take from it

A study of the UE 5.8.2 source (available at `C:\UnrealEngine-5.8.2-release`), read for the capability this
renderer does not have at all: **its punctual lights cast no shadows**. `shaders/shading.glsl:923` says so in
as many words ("punctual lights (point/spot, no shadow casting in this version)"), and the clustered loop
below it walks the pixel's cluster list and accumulates radiance with no visibility term - so a demo light
behind a column lights the wall in front of it. The sun is the only shadowed light, through the cascades
(`calc_shadow`) or through one ray-traced visibility ray per pixel (`shaders/rt_shadow.slang`).

MegaLights is Epic's answer to the same problem at a scale this renderer never reaches: it evaluates a few
STOCHASTIC samples of the light set per pixel, traces one visibility ray per sample, and denoises the result
in space and time. The point of studying it here is not its scale but its ESTIMATOR: which lights a pixel
spends its samples on, how those samples stay unbiased when the choice is biased, and what stops a bright
OCCLUDED light from consuming the whole budget - the last question being the one this project has already
measured the hard way on the GI side (the finding survives in `docs/megalights.md`: a denoiser that removes
signal and noise at the same rate is a crude denoiser, and the fix is to spend the samples better, not to
filter harder).

Everything below is a mechanism, with the file and line it lives at, and a note on what it would take here.
The four working notes it is consolidated from are in `build/dsh-scratch/ue-notes/` (sampling, visibility and
tiles, denoiser, driver) and every claim below carries the same `file:line` as those.

## 1. The estimator: weighted reservoir sampling over the culled light grid

One compute thread per DOWNSAMPLED pixel (2x2 by default, `r.MegaLights.DownsampleMode = 2`), 8x8 thread
groups, N = `r.MegaLights.NumSamplesPerPixel` samples (default 4, supported 1/2/4) laid out as a 2x2 block of
sample texels (`MegaLights/MegaLightsSampling.usf:286-300`, `MegaLights.cpp:60-67,:776-790`).

The candidates are the lights of the pixel's LIGHT GRID CELL - `min(NumMegaLights, MaxCulledLightsPerCell)`,
with `MaxCulledLightsPerCell = 32` (`Sampling.usf:317-320`, `LightGridCommon.ush:76-79`,
`LightGridInjection.cpp:123`). This is the same structure this renderer already builds for the deferred path:
`shaders/light_cluster.slang` bins the punctual lights into 64 px tiles x 16 depth slices with a fixed
capacity of `deren::vulkan::cluster_light_capacity = 32` per cluster, and `shaders/shading.glsl` reads that list per
pixel. **The port gets its candidate set for free, and UE's cell cap is numerically the same 32.**

For every candidate the sampler computes a target PDF weight (`MegaLights/MegaLightsLightTargetPDF.ush:23-59`):

    Lum = shadedLuminance(light, pixel) * PreExposure      // the REAL BSDF, IES included
    W   = log2(Lum * SmoothFalloffMask(Lum, MinSampleWeight) + 1)
    SmoothFalloffMask(X, T) = pow2(saturate(1 - pow4(T / X)))          // MegaLights.ush:409-420

so the weight is a LOG-compressed luminance rather than a raw power: a light a hundred times brighter buys
about seven times the samples, which is deliberate - the comment says it stops very bright lights (which are
often the hidden ones) from monopolizing the ray budget, and it is the first half of the answer to "why is
my shadowed interior noisy". `MinSampleWeight` (0.001) is the threshold of that smooth cut, applied TWICE -
once to the sampling PDF and again to the final shading weight, from the same `Lum` value, so the mask the
estimator divides by is the mask the sampler used (`Sampling.usf:55`, `MegaLightsShading.usf:449-450`).

Before shading, a cheap estimate culls lights that cannot matter: `EstimateLocalLight` (light power x radial
falloff / pi, `MegaLightsEstimateLight.ush:9-44`) against `MinSampleWeightEstimate = MinSampleWeight /
LightAttenuationFalloff = 0.001 / 0.18 ~= 0.00556` (`MegaLights.cpp:1047-1058`). NOTE, because the name
invites the wrong reading: there is no `MegaLightsLightAttenuationFalloff` attenuation function in 5.8.2 -
that CVar only scales this conservative early cull, and the real attenuation is the shared deferred
`GetLocalLightAttenuation` (radius mask x inverse square, `MegaLightsShading.ush:44-51`).

Then the selection itself, which is a per-pixel **weighted reservoir (RIS) with N reservoirs**
(`Sampling.ush:84-115`), stratified by one blue-noise scalar per downsampled pixel
(`LightIndexRandom[i] = (BlueNoiseScalar(P, frame) + i) / N`, `Sampling.usf:395-396`):

    Tau = WeightSum / (WeightSum + W);  WeightSum += W
    per reservoir i:  r[i] < Tau ? (keep the old light, r[i] /= Tau)
                                 : (take this light,    r[i] = (r[i] - Tau) / (1 - Tau), w[i] = W)
                      r[i] = clamp(r[i], 0.0, 0.9999)

and the sample's weight in the estimator is `WeightSum / w[i]` (`Sampling.usf:544`) - i.e. `1/p_i` with the
selection pmf `p_i = W_i / Sum(W)`, so the lighting is the standard unbiased `f/p` estimate with the
reservoir's strata folded in. Shading applies
`min(Weight, bGuidedAsVisible ? 20 : 5) * NumMergedSamples / N * SmoothFalloffMask(Lum)`
(`MegaLightsShading.usf:144-145,:294,:450-453`). The 20/5 are FIREFLY clamps, not part of the estimator.

Two things this renderer can simplify outright, both from the same fact - its light count is BOUNDED:

* UE packs a 15-bit light index because it supports 32768 local lights. With `max_punctual_lights = 128` a
  plain light index fits in a byte, and the visible/hidden SETS below become an exact 128-bit mask instead of
  a probabilistic hash.
* UE's TILE CLASSIFICATION (13 tile modes, indirect dispatch buckets, per-mode shading permutations) exists to
  select SUBSTRATE/material permutations per tile and to clear empty tiles (`MegaLightsDefinitions.h:10-28`,
  `StochasticLighting/StochasticLightingTileClassification.usf:600-641`, `MegaLights.usf:77-212`). This
  renderer has ONE BRDF, so none of that applies. **And the "light complexity" pass that looked like a
  cost -> sample-count feedback loop is VISUALIZATION-ONLY** (`MegaLightsLightComplexity.usf`, gated by
  `ShouldShowLightSamplingCostHeatmap`): the real budget is `downsample factor x NumSamplesPerPixel`, fixed,
  and the same for every tile. That correction removes the largest piece a naive port would have built.

## 2. What keeps a bright occluded light from eating the budget: the visible/hidden hash

This is the mechanism to take most seriously, because it targets exactly the artifact this project has been
staring at. Per 8x8-tile UE keeps two bit-sets of light indices that were SELECTED AND TRACED last frame -
`VisibleLightHash` (4 uints = 128 bits) and `HiddenLightHash` (2 uints = 64 bits)
(`MegaLightsVisibility.ush:6-7`, `MegaLights.cpp:523-524`). Membership is a hash of the light index and TWO
independent 5-bit tests (false positive 2^-10 each way, `Visibility.ush:9-57`); the bits are built per tile in
`VisibleLightHashCS` from this frame's stored samples with `Weight > 0` (`MegaLightsVisibleLightHash.usf:30-137`),
and `bVisible` comes from the RAY TRACE, not from sampling: a sample starts visible and is cleared when its
shadow ray hits geometry (`MegaLightsHardwareRayTracing.usf:372-375`). "Hidden" therefore means "this tile
spent a sample on this light and the ray was blocked".

`r.MegaLights.GuideByHistory` (default true, `MegaLights.cpp:86-91`) consumes those sets next frame, per
candidate light (`Sampling.usf:166-211`):

    if the reprojected tile's visible hash does NOT contain this light's previous index:
        W *= has_valid_history ? 0.1 : 0.4
      - UNLESS Lum * LightPowerHistoryRatio < MinSampleWeight, i.e. it was dropped
        by the weight cutoff last frame rather than occluded, in which case it still
        counts as visible and gets no discount                     (`Sampling.usf:171-179`)

The stated intent is verbatim the artifact: "reduce sampling chance for lights which were hidden last frame,
reduces noise in areas where bright lights are shadowed". The mechanism is worth stating precisely because it
looks like it should introduce bias and does not: the SAME discounted `W` is used in the numerator (the
stored sample weight) and the denominator (the normalization), so the estimate stays unbiased - the only
asymmetry is that when such a light does turn out visible its sample is clamped to 5 instead of 20, which
reads as a darker first frame and is documented as intentional (`MegaLightsResolve.cpp:23-29`). Without it,
the PDF is blind to occlusion, so rays are spent on a light that contributes nothing and the rare ray that
finds it visible carries `Sum(W)/W_i` on its own - fireflies and blotchy, slowly converging noise in exactly
the penumbra and contact-shadow regions. There is NO spatial or temporal reservoir reuse in MegaLights
(`Sampling.ush` rebuilds it every frame); history only rescales `W`.

## 3. The visibility ray

One ray per light sample. Origin is the pixel's world position from the DOWNSAMPLED depth; the direction and
the distance come from the light's analytic form; transmission rays are traced INVERTED (origin pushed to the
far end, direction negated) so a hit distance is recovered as `Ray.TMax - HitT`
(`MegaLights/MegaLightsRayTracing.ush:85-127`, `MegaLightsHardwareRayTracing.usf:104-145`). Flags:
FRONT-face culling, `SKIP_CLOSEST_HIT_SHADER | ACCEPT_FIRST_HIT_AND_END_SEARCH | FORCE_OPAQUE`, shadow-ray
instance mask, 8192 traversal iterations maximum. The self-intersection guard is an origin bias along the
normal of `lerp(max(0.01, 0.1), 0.01, saturate(N dot D))`, flipped for two-sided materials - note the SHAPE,
which this project arrived at independently on the GI side: a world-space bias that shrinks as the light
approaches the normal.

## 4. The denoiser

Rejection lives in the tile-classification pass, not the denoiser (`StochasticLightingTileClassification.usf:807-1099`,
driven by `MegaLights.cpp:1301-1367`), and it is a single depth test with a grazing-angle widening:

    abs(historyDepth - reprojectedDepth) < reprojectedDepth * 0.03 / lerp(0.1, 1, saturate(N dot V))

plus rejections on "the previous frame had no sample here" (`NumFramesAccumulated > 0`), off-screen
reprojection, and a bilinear history weight <= 0.01. The history position is the previous-frame projection of
this pixel, with the G-buffer velocity substituted when the pixel is dynamic (`LumenPosition.ush:36-60`).

The temporal resolve accumulates in YCoCg scaled by an exposure constant of 16, with a per-pixel RUNNING MEAN
whose length is tracked in an R8_UINT (packed `X * 8 + 0.5`, so 1/8-frame steps to 31.875)
(`MegaLightsDenoiserTemporal.usf:31,:426-428`):

    maxFrames = min(lerp(4, 12, softClampConfidence), 1 / (ShadingConfidence * 0.5))
    alpha     = 1 / NumFramesAccumulated                      // a running mean, not a fixed 0.9
    result    = lerp(clampedHistory, current, alpha)
    history miss -> frames = 1;  no valid sample -> history passthrough, frames not incremented

`MegaLightsDenoiser.ush:12-16` is the whole confidence policy in three lines, and it is the piece this
project's GI chain most obviously lacked: **a static pixel accumulates up to 12 frames - more when the
sampling confidence is high - so the spatial filter does not have to do the noise removal on its own.**
The GI chain used a fixed 0.9 EMA (10 frames) forever, which is why its spatial filter was wide and the
dark parts went soft (`docs/megalights.md` has the measurement); that chain was removed, and
`shaders/megalights_temporal.slang:9` records the same measurement as the reason its own accumulation is
a running mean whose cap is `ML_MAX_FRAMES = 12`.

The history is clamped into a neighbourhood statistics box rather than a min/max of the noisy current frame:
a 5x5 groupshared pass (corners and centre skipped, 16 taps) computes mean and stddev, the box is
`mean +/- clampScale * stddev` expanded to include the centre, `clampScale = 1 + 3 * LinearStep(4, 2, maxFrames)`,
and the clamp is SOFT - a confidence of `saturate(1 - distance / max(extent, 0.1/16))`
(`MegaLightsDenoiserTemporal.usf:158-174,:383-423`). Moments (`PF_FloatRGBA`: mean and mean^2, clamped to
65504) are accumulated with the same alpha and are what drives the spatial filter; a second R8G8 image stores
the two soft-clamp confidences for the next frame.

The spatial filter is ONE stochastic gather, not an a-trous pyramid:
`N = lerp(4, 8, disocclusionFactor)` Hammersley points on a concentric disk of radius 8 px
(`MegaLightsDenoiserSpatial.usf:472-490`), where
`disocclusionFactor = 1 - saturate((frames - 1) / min(2, quantizedMaxConfFrames - 1))` doubles the sample
count, doubles the specular lobe half-angle and switches on a 5x5 weighted spatial-variance fallback
(`:254-262,:276-277`). Each tap is weighted by
`exp2(-10000 * relPlaneDepthDiff^2) * (1 - saturate(angle / lobeHalfAngle)) * exp2(-|lumC - lumN| / max(stdDev, 0.001))`,
with neighbours loaded as full-res material for the depth/normal edges, and the filter is GATED: it runs only
where the normalized stddev exceeds 0.2 (disocclusion) or 0.5 (steady state) - otherwise the signal is left to
TSR (`:390-419`). The composite is the last stage of that same pass: `RWSceneColor += AccumulatedSceneColor`
(`:554-593`), and on the deferred path the shade pass does NOT write SceneColor at all (its only store is
hair-strands-only, `MegaLightsShading.usf:590-595`) - so there is no separate "resolve" pass to look for.

## 5. What the driver keeps, and the two readings it corrects

Per-frame passes, in order, for the deferred path (`MegaLights.cpp`, `MegaLightsSampling/RayTracing/Resolve/Denoising.cpp`):
tile classification and its downsampled twin -> indirect args -> clear -> `GenerateSamples` per tile type ->
VSM page marking -> trace compaction -> VSM screen-space traces -> hardware inline ray tracing (with a
material retrace for alpha-masked hits) -> `ShadeLightSamples` -> visible/hidden hash build and filter ->
`ClearTemporalAccumulation` -> temporal filter -> spatial filter (which composites into SceneColor).

Resources that persist across frames (per view): the diffuse and specular lighting histories, the moments
history, the frame-count history (R8_UINT), the visible/hidden light hash histories, a per-light
`LightPowerHistoryRatio` buffer, and (for the volume path) two cascades of translucency-volume hashes
(`MegaLightsViewState.h:19-30`, `ML/MegaLightsLightPowerDelta.usf:48-59`). Everything else - the downsampled
depth and normal, the light samples and their rays, the resolved lighting, the shading confidence, the tile
allocator and indirect args - is transient per frame.

The integration with the deferred renderer is a PARTIAL REPLACE: lights a frame claims are excluded from the
raster light loop (`bHandledByMegaLights`, `LightRendering.cpp:1483,:1536`, sorted to a tail range at
`:1581-1622`), and the stochastic result is ADDED to `SceneTextures.Color` after `RenderLights`
(`DeferredShadingRenderer.cpp:3467` then `:3471`). The two corrections worth keeping: **the "light
complexity" pass is visualization-only** (see section 1) and **the composite happens inside the spatial
denoiser**, not in a resolve pass of its own (section 4).

## 6. What a port here would take

The pieces this renderer already has, and what they map to:

| UE | here |
|---|---|
| the culled light grid cell (`MaxCulledLightsPerCell = 32`) | `shaders/light_cluster.slang`'s cluster list, capacity `cluster_light_capacity = 32` |
| 15-bit light index, 32768 lights | `max_punctual_lights = 128`, so a light index is a byte |
| visible/hidden probabilistic bit-hash (2^-10) | an EXACT 128-bit mask per tile - 4 uints, no hashing |
| `GetLocalLightAttenuation`, IES, light functions | `light.punctual_lights[]` in the light UBO, already evaluated by `shaders/shading.glsl` |
| inline `RayQuery` shadow ray, front-face culled, accept-first-hit | `shaders/rt_shadow.slang`'s shadow ray, same flags |
| 13 tile modes, indirect dispatch buckets, per-mode permutations | nothing - one BRDF, one path |
| `R11G11B10` lighting + separate diffuse/specular histories | the GI chain's `RGBA16F` images, at half resolution, per swapchain image |
| `RWSceneColor +=` in the spatial denoiser | `deferred_pass`'s additive blend into `scene_color` (`make_color_blend_attachment_additive`), the shape `ml_resolve` would copy |

The two mechanisms a first port should NOT drop, because they are the ones that make the difference between
"another noisy feature" and "shadowed punctual lights that converge": the **log-compressed target PDF** and
the **history-guided sampling** of section 2. The denoiser policy of section 4 is the third, and it is worth
noting that adopting it would also fix the GI chain's own measured trade-off (`docs/megalights.md`), since
both chains are the same estimator shape: a stochastic estimate, a moment, a frame count, and a variance-gated
filter.
