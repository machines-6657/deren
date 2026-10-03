# Host image copy: reading an image back without a staging buffer

**STATUS: IMPLEMENTED and gated. `VK_EXT_host_image_copy` is an OPTIONAL device extension here: when the device
supports it - and this renderer's images are readable that way - an image read-back is performed by the
IMPLEMENTATION into a pointer of the app's own memory, so there is no staging buffer, no
`vkCmdCopyImageToBuffer` and no transfer-stage barrier. When it does not, the read-back is exactly what it was
before. THE SCREENSHOT PATH WAS DELIBERATELY NOT CHANGED; see "Why the screenshot path keeps its copy command".
Only one caller moved: the heap-native graphics probe (`runtime::run_heap_graphics_probe`,
`vulkan/runtime/runtime.probes.cppm`), which renders into an offscreen image and reads one pixel back.**

- The extension is an OPTIONAL capability, and it is the opposite of `VK_KHR_unified_image_layouts` in exactly that
  respect: that one is a requirement whose absence refuses the device, this one has a working fallback (the staging
  buffer), so its absence only means the old path. Nothing about the frames depends on which path runs.
- The capability is THREE facts, not one, and all three are queried: the extension, its `hostImageCopy` feature, and
  whether `VK_IMAGE_LAYOUT_GENERAL` is one of the device's copy-source layouts. The third is the one that would be
  missed by reading the feature bit alone.
- The one caller that changed makes the choice ONCE, from `core::host_image_copy_available`, and every branch that
  follows tests that one value: the target image's usage bit, whether the staging buffer exists, which stage the
  barrier hands the render's writes to, and whether a copy command is recorded at all. The log line at the end names
  the path that actually ran, so a reader never has to infer it from the code.

## What the extension buys, and what it does not

`vkCopyImageToMemoryEXT` takes an `VkCopyImageToMemoryInfoEXT` naming a source image, its CURRENT layout and one or
more `VkImageToMemoryCopyEXT` regions, each with a host pointer, a subresource and an extent. The implementation
performs the copy; the app records nothing, submits nothing and owns no staging memory. It is an EXTENSION command,
so it is resolved with `vkGetDeviceProcAddr` and cached on `core` (`copy_image_to_memory`), the same way the mesh
dispatch commands are (`vulkan/core/core.declarations.cppm`, `vulkan/core/core.constructor.cppm`).

Two things it does NOT remove:

- The render that produced the image still has to be synchronized against. The probe records a barrier
  (`srcStage = COLOR_ATTACHMENT_OUTPUT`, `srcAccess = COLOR_ATTACHMENT_WRITE`, image stays in
  `VK_IMAGE_LAYOUT_GENERAL`) whose destination is the HOST stage / HOST_READ access, submits that command buffer and
  waits on its fence BEFORE calling the copy. The barrier is what makes the writes visible to the host; the
  submission is what makes the barrier happen. The copy call itself is then a host-side operation on an image no
  queue is using.
- The layout has to be right. `srcImageLayout` must BE the image's current layout
  (`VUID-VkCopyImageToMemoryInfo-srcImageLayout-09064`) and must appear in the device's copy-source list
  (`...-09065`); this renderer keeps every image in `VK_IMAGE_LAYOUT_GENERAL` (see
  \ref md_docs_2unified__image__layouts "Unified image layouts"), which is why GENERAL is the layout both queried
  and passed.

## The three-part capability check

`init_utils::query` decides `host_image_copy_available`, and the two halves are decided in two different places
because they come from different Vulkan structures:

1. **Extension + feature** (`vkEnumerateDeviceExtensionProperties`, then a `VkPhysicalDeviceHostImageCopyFeaturesEXT`
   in the feature `pNext` chain): `hostImageCopy` must be `VK_TRUE`. Enabling the feature and using any image with
   `VK_IMAGE_USAGE_HOST_TRANSFER_BIT` are the same fact (`VUID-VkImageCreateInfo-usage-10245`).
2. **The copy-source layouts** (`VkPhysicalDeviceHostImageCopyPropertiesEXT::pCopySrcLayouts`, in the property
   chain): the app passes an array and the count comes back as the device's total, so the query uses a fixed
   `std::array<VkImageLayout, 16>` and clamps. The only question asked of the list is "is GENERAL in it", and a list
   longer than the array would only ever make the answer a false NO (the staging path stays) rather than allow a
   copy in a layout the device does not accept. A false NO is the safe direction, which is why a fixed array is
   used instead of a two-pass query.

The check is conservative in the same way for the entry point: `core::host_image_copy_available` is recomputed
AFTER `vkGetDeviceProcAddr` returns, so a device that reports the feature but hands back a null pointer is treated
as not having the capability at all. Nothing panics: `VK_EXT_host_image_copy` is only pushed onto the enabled
extension list when the capability is there, and every read-back site tests the flag.

One consequence is worth stating because it is easy to get wrong later: the image the probe reads back is created
with `VK_IMAGE_USAGE_HOST_TRANSFER_BIT` INSTEAD OF `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, not in addition to it. The
usage bit follows the path that will actually run, so the staging path's bit is absent exactly when the staging
path cannot run - there is no image carrying a bit nothing uses. `VUID-VkCopyImageToMemoryInfo-srcImage-09113`
requires the bit for the non-stencil aspects of the source, so a wrong choice here is an invalid copy rather than a
slow one.

## What the mapping between image and host memory is

The probe's region is the whole image, tightly packed: `memoryRowLength = 0` and `memoryImageHeight = 0`, which the
spec defines as tightly packed and which is exactly what the staging path's `VkBufferImageCopy{bufferRowLength = 0,
bufferImageHeight = 0}` produced. Both paths therefore leave the same bytes in the same order in their destination,
which is what makes the two log lines comparable (`VUID-VkImageToMemoryCopy-memoryRowLength-09101`,
`...-memoryImageHeight-09102`). The other constraints the code satisfies deliberately: `aspectMask` has one bit
(`...-aspectMask-09103`), all three extents are non-zero (`...-imageExtent-06659/60/61`), the host pointer is large
enough for the whole region (`VUID-VkImageToMemoryCopy-pHostPointer-09066`), and `flags = 0` - the
`VK_HOST_IMAGE_COPY_MEMCPY_BIT` fast path is NOT requested, because it constrains the region to the whole image with
a zero offset and buys a memcpy the tight-packing path already gives.

## Why the screenshot path keeps its copy command

The other in-tree image -> buffer copy is the screenshot
(`runtime::record_screenshot_copy`, `vulkan/runtime/runtime.readback.cppm`), and it was NOT converted. The reason is
the source image: it is a SWAPCHAIN image, so its usage bits come from `VkSwapchainCreateInfoKHR::imageUsage` and
its support for a host copy is the WSI's business rather than this renderer's. No VUID was found that forbids
`VK_IMAGE_USAGE_HOST_TRANSFER_BIT` on a swapchain image (the registry's `VkSwapchainCreateInfoKHR-imageUsage` has
three VUIDs and none of them is about this bit), but "not forbidden by a VUID" is not "supported by this driver, for
this presentation engine, on this image", and the probe path proves the mechanism without betting a working feature
on an unverified one. The screenshot therefore records `vkCmdCopyImageToBuffer` into the readback staging buffer
exactly as before, and the module's doc comment ("this module knows nothing about IMAGES") stays TRUE: the host copy
was NOT added to `deren.vulkan.readback`, it is a capability of `core` and a call site in the probe. A future change that
finds the swapchain bit supported on real hardware should convert it and delete this paragraph, not this decision.

## Evidence

MEASURED, on this machine (see PROGRESS.md 3.10 for the commands and the raw outputs):

- The device reports the capability and the path taken is in the log: the init line
  `host image copy: available (VK_EXT_host_image_copy, an image read-back skips the staging copy)` and, for each of
  the three graphics probes, `... read that pixel back through the HOST IMAGE COPY (no staging buffer, no copy
  command)`.
- The host path produces the SAME BYTES as the staging path: the three probe lines' rgba values are unchanged
  between the build before this change and the build after it, and those values are the probe's own proof (the
  material table's slot reads white 255,255,255,255 and the deliberately wrong slot does not).
- No frame changed: `scripts/windows/check_render.ps1 -Full` reports 14 scenarios, 0 changed, 0 flaky, and `ctest`
  is 13/13 (including `test_docs`, which is why `docs/host_image_copy.md` is in the `Doxyfile` INPUT list).

## What is NOT established

- NO PERFORMANCE CLAIM. Removing a staging buffer and a copy command from a diagnostic probe is not a speedup
  anybody can see; the extension is here because it is the correct mechanism for the read-back this renderer does,
  not because a number moved. No timing was taken.
- The FALLBACK was not exercised on a device without the extension (this machine's device has it). The staging path
  is unchanged code and its branch is compiled, but "it still works" rests on that, not on a run.
- The screenshot path's swapchain images were not tested for HOST_TRANSFER support in either direction. The question
  is open, and the decision above is to leave that path alone.
- No validation-layer or synchronization-validation run was made for the host copy; the barrier chain is reasoned
  from the synchronization chapter plus the wait that precedes the call.
