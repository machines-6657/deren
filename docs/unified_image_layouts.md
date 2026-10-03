# `VK_KHR_unified_image_layouts`: what this renderer requires of it, and what it deleted

Every image this renderer owns lives in `VK_IMAGE_LAYOUT_GENERAL`, and the device is now REQUIRED to
support `VK_KHR_unified_image_layouts` - the extension whose promise is that GENERAL is as efficient as
the purpose-built layouts. This document records the contract that made that safe, the two layouts that
survive it, the mechanical change it caused across the tree, and the measurements the change was gated
on. One earlier measurement of mine is explicitly WITHDRAWN in section 4, because it attributed a pixel
difference to this step that was not this step's.

## 1. The contract, and what it does not promise

`VK_KHR_unified_image_layouts` (`unifiedImageLayouts`) guarantees that an implementation performs
optimally with `VK_IMAGE_LAYOUT_GENERAL` for every image usage, so a renderer no longer has to move an
image to `SHADER_READ_ONLY_OPTIMAL`, `COLOR_ATTACHMENT_OPTIMAL`, `TRANSFER_SRC/DST_OPTIMAL` or
`DEPTH_STENCIL_ATTACHMENT_OPTIMAL` to be fast. The extension adds NO commands: it is a feature bit plus
the guarantee, so it needs no entry point beyond `vkGetPhysicalDeviceFeatures2`.

IT DOES NOT REMOVE MEMORY BARRIERS, and the proposal says so in as many words: barriers are still
required for correctness, and on some hardware still for performance, "even if both src and dst layouts
are VK_IMAGE_LAYOUT_GENERAL". What this step therefore removed is the LAYOUT dimension of every barrier,
not the barrier: every barrier in `vulkan/constant_init/constant_init.cppm` still names its two stages
and its access masks, and those are untouched. That is the single most important sentence here, because
the failure mode of deleting a barrier that still mattered is a race, and a race is not visible in a
byte-identical frame gate.

Four things the extension does NOT fold into GENERAL, all of them in this tree or deliberately unused:

* `VK_IMAGE_LAYOUT_UNDEFINED` - creation and "the old contents are irrelevant". Still required: it is how
  an image's layout metadata is initialised, and the discard form is what makes `loadOp CLEAR` legal
  without tracking the previous contents. Every `initialLayout` in the tree is still UNDEFINED.
* `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` - the presentation engine is outside Vulkan, so present is the one
  consumer the extension cannot cover. `present_transition` and `undefined_to_present_transition` keep it
  as their new layout (their old layout is GENERAL and UNDEFINED respectively).
* `VK_IMAGE_LAYOUT_PREINITIALIZED` - unusable for us: the renderer never creates an image with
  PREINITIALIZED (it would need host-visible memory and a host write before any transition).
* attachment feedback loops and the video layouts - not used here at all: this renderer never samples an
  image in the same rendering instance that is writing it, and it has no video path, so
  `unifiedImageLayoutsVideo` is forced to VK_FALSE next to the pre-existing mesh-shader policy bit.

The "as efficient" half is the implementation's promise rather than something this repository measured;
what the repository can check is that the images really are in GENERAL everywhere (`vkCmdCopyImage`,
`vkCmdClearColorImage`, `vkCmdCopyImageToBuffer`, every `VkRenderingAttachmentInfo::imageLayout` and
every heap image descriptor now name GENERAL), that validation reports nothing, and that the frames are
unchanged (section 4).

## 2. The change, layer by layer

* `vulkan/core/init_utils/init_utils.cppm` - the feature struct, the extension probe, BOTH passes of the
  feature chain (the one before `vkGetPhysicalDeviceFeatures2` and the rebuilt one that `vkCreateDevice`
  actually uses; forgetting the second pass silently drops the struct), the availability flag, the
  video policy bit and a startup log line.
* `vulkan/core/core.constructor.cppm` - the extension NAME is pushed unconditionally and a device without
  `unifiedImageLayouts` panics at startup, in the style of the existing dynamic-rendering requirement.
  There is deliberately no fallback path: two layout regimes would be two renderers.
* `vulkan/constant_init/constant_init.cppm` - 21 barrier constants and 4 attachment-info helpers: the
  seven retired layouts became GENERAL. The constants keep their NAMES (they name roles:
  `shadow_map_sampling_transition`, `sampling_to_transfer_dst_transition`, ...) and the section header
  now states the rule, because a barrier whose two layouts are both GENERAL is still a real barrier.
* `vulkan/core/vma/vma.cppm`, `vulkan/runtime/runtime.{probes,readback,frames,constructor}.cppm`,
  `vulkan/pass/{taa,megalights_temporal}.cpp`, `vulkan/core/core.constructor.cppm` - copies, clears and
  every heap image descriptor write. The descriptor one is not cosmetic: a heap image descriptor states
  the layout the image is in, so a descriptor claiming `SHADER_READ_ONLY_OPTIMAL` for an image that is
  really in GENERAL is a lie validation rejects.
* The DECLARATION LAYER lost its layout: `render_resource::image_layout`, `pass_binding::layout`,
  `bindings::image_layout_of` and the validator rule "a storage image must declare GENERAL" are gone.
  They existed because "the layout is NOT derivable from the kind" (the probe cache kept all nine of its
  own bindings in GENERAL); with one layout there is nothing to state. `docs/pass_io_design.md` keeps the
  history and says why step 4 no longer takes a layout as input.

`third_party/imgui` still names `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`; it is third-party code and
out of scope, and the engine never lets imgui own an image that this renderer transitions.

## 3. The retired layouts, counted

Seven spellings were replaced by `VK_IMAGE_LAYOUT_GENERAL`: `COLOR_ATTACHMENT_OPTIMAL`,
`DEPTH_STENCIL_ATTACHMENT_OPTIMAL`, `DEPTH_ATTACHMENT_OPTIMAL`, `DEPTH_READ_ONLY_OPTIMAL`,
`SHADER_READ_ONLY_OPTIMAL`, `TRANSFER_SRC_OPTIMAL`, `TRANSFER_DST_OPTIMAL`. The substitution rewrote 60
lines across nine files (constant_init 32, core.constructor 11, runtime.probes 5, vma 4,
runtime.constructor 3, runtime.frames 2, taa 1, megalights_temporal 1, runtime.readback 1), and a second
pass caught 32 more that were comment prose naming the layouts without the `VK_IMAGE_LAYOUT_` prefix.
`UNDEFINED` and
`PRESENT_SRC_KHR` were not touched. Exactly two places still name a retired layout, and both are
QUOTES rather than descriptions: `docs/megalights.md` reproduces a validation message from a run made
before this change, and `vulkan/runtime/runtime.frames.cppm` quotes the driver's "expects ...
SHADER_READ_ONLY_OPTIMAL ... current layout is UNDEFINED" with a note that GENERAL is what it expects
now. A blind text substitution had rewritten the second quote, which is how it was caught: a quoted
error message is evidence and must not be edited to match the current code.

## 4. The gate, and one withdrawn measurement

* `cmake --build build-release-clang64 -j 12`: exit 0.
* `ctest --test-dir build-release-clang64`: 13/13.
* `scripts/windows/check_render.ps1 -Full`: all 14 scene hashes IDENTICAL to the run made immediately
  before this step. The scene hash is the first 16 hex digits of the PNG file's SHA-256, so identical
  hashes mean byte-identical frames.
* The run is Release, on this machine's GPU, with the Khronos validation layer active; no VUID or error
  line is printed in any of the 14 runs.

WITHDRAWN: I first reported that enabling the feature changed 1379 pixels in `deferred_taa_fxaa`
(max |delta| 17). That was an instrument error, not a result. The comparison was against the machine's
stored reference frames (`%LOCALAPPDATA%\deren\baseline`), which were ALREADY stale for 12 of the
14 scenes before this step began - the area-light work committed just before it had changed them, and
that staleness is recorded separately in `build-release-clang64/deren-ab/PROGRESS.md`. Measured against the
immediate predecessor run instead, every one of the 14 hashes is identical: the feature alone, and then
the collapse, each change no pixel at all. The lesson is the one this project keeps re-learning: a
baseline is only a baseline if it is the state you are changing FROM.

## 5. What this does not establish

* NO synchronization-validation layer was run. `VK_LAYER_KHRONOS_syncval` is not installed on this
  machine (`C:\VulkanSDK\1.4.357.0` has no such layer manifest), so "the remaining barriers are still
  correct" rests on three weaker things together: the change touched ONLY the two layout fields of each
  barrier, `vkCmdPipelineBarrier2` time windows and masks are unchanged, validation is silent, and the
  frames are byte-identical. That is not the same as a synchronisation check, and it is recorded as a
  gap rather than as a verification.
* Performance is not measured. If the extension's promise holds, nothing got slower; that is the
  driver's claim and this repository has no benchmark for it.
* A device without the extension now fails at startup with a message instead of running. That is the
  intended trade (section 2) and it means the renderer's minimum requirement moved.
* `host_copy_image` (`VK_EXT_host_image_copy`) is the NEXT step, deliberately not part of this one.
