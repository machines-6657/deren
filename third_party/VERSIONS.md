# Vendored Third-Party Versions

Pinned versions + local patches for the vendored code under `third_party/`
(see each subtree's LICENSE). These are the checked-in copies the build
uses; system-package equivalents (e.g. MSYS2 packages) are not vendored and
are not listed here.

## imgui

- version: 1.92.9b (`IMGUI_VERSION` in `third_party/imgui/imgui.h`)
- upstream: https://github.com/ocornut/imgui
- local changes: none recorded - vendored as-is in commit 3e34dc6, which
  states "vendored imgui 1.92.9b core + GLFW/Vulkan backends"
- backends: `imgui_impl_glfw.*` and `imgui_impl_vulkan.*` ship with the
  source under `third_party/imgui/backends/` (used by the `vulkan.gui`
  overlay)
- build: compiled as a standalone plain-C++ `imgui` static target (see
  CMakeLists.txt); imgui is not a module, is exempt from the repo's
  `-Werror` set (upstream code triggers warnings we do not own), and is
  excluded from the clang-format glob (which only covers
  main/gltf_loader/utility/vulkan, never `third_party`)

## xxhash

- version: 0.8.3 (`XXH_VERSION_MAJOR` / `XXH_VERSION_MINOR` /
  `XXH_VERSION_RELEASE` in `third_party/xxhash/xxhash.h`)
- upstream: https://github.com/Cyan4973/xxHash
- local changes: no local patches

## spirv-reflect

- version: none pinned - the vendored tree carries no version marker (no
  SPIRV_REFLECT version macro in `spirv_reflect.h`; its VERSION HISTORY
  comment only records the 2018-03-27 1.0 initial release, so it cannot pin
  this snapshot)
- upstream: https://github.com/KhronosGroup/SPIRV-Reflect
- local changes: vendored with local patches; upstream baseline unknown -
  check git history (the subtree entered the repo at the initial commit
  under the repo root and was moved into `third_party/` by commit 2c4c4c2).
  The one tracked local change is commit 44f4f9e ("fix(spirv-reflect):
  disable crt debug memory mapping macro"): `#define _CRTDBG_MAP_ALLOC` in
  `spirv_reflect.c` is commented out.

## fastgltf

- version: 0.9.0 (`project(fastgltf VERSION 0.9.0 ...)` in
  `third_party/fastgltf/CMakeLists.txt`)
- upstream: https://github.com/spnda/fastgltf
- local changes: none - the release tarball is vendored as-is
- build: the three translation units (`src/fastgltf.cpp`, `src/base64.cpp`,
  `src/io.cpp`) are compiled into a plain-C++ `fastgltf_vendored` static
  target together with simdjson (below); fastgltf is exempt from the repo's
  `-Werror` set. Its headers deliberately do NOT include simdjson (simdjson
  is hidden inside the .cpp files), so consumers only need fastgltf's
  headers; `gltf_loader` links `fastgltf_vendored`.
- note: vendored so the project builds fully self-contained (no
  system-package fastgltf / no network FetchContent); the same sources build
  on Windows/MSYS2 and Linux.

## simdjson

- version: 4.6.9 (the amalgamated single header/source pair is generated
  from tag v4.6.9; see the `singleheader/simdjson.h` header comment)
- upstream: https://github.com/simdjson/simdjson
- local changes: none - only the amalgamated pair is vendored
  (`singleheader/simdjson.h` + `singleheader/simdjson.cpp`)
- build: `simdjson.cpp` is compiled into the `fastgltf_vendored` target;
  its include dir is private to that target (fastgltf hides simdjson from
  its public headers)
- license: dual MIT / Apache-2.0 (`LICENSE` + `LICENSE-MIT` in the subtree)

## stb_image

- version: not pinned by upstream (public-domain single header; the vendored
  `third_party/stb/stb_image.h` snapshot is taken from upstream master)
- upstream: https://github.com/nothings/stb
- local changes: none - single header vendored as-is
- build: header-only; `third_party/` is on gltf_loader's include path so
  `#include <stb/stb_image.h>` resolves. gltf_loader.cpp defines
  `STB_IMAGE_IMPLEMENTATION` to compile the decoder into that one TU.
- license: public domain / MIT (see the header's license comment)

## VMA (Vulkan Memory Allocator)

- version: not pinned - copied from the LunarG Vulkan SDK 1.4.357.0
  (`Include/vma/vk_mem_alloc.h`); the SDK snapshot carries no VMA version
  macro, so it cannot pin a release
- upstream: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
- local changes: none - single header vendored as-is
- build: header-only; `<vma/vk_mem_alloc.h>` resolves against the
  `third_party/` include root, which is marked SYSTEM on the consuming
  targets so upstream VMA's warnings stay suppressed (the SDK include is an
  isystem include for the same reason). The vendored copy is preferred over
  the SDK's `Include/vma` because Linux distro Vulkan packages do not ship
  VMA at all.
- license: MIT (see the header's license comment)

## mimalloc

- version: 3.4.4 (`MI_MALLOC_VERSION 30404` in
  `third_party/mimalloc/include/mimalloc.h`; release tag `v3.4.4`)
- upstream: https://github.com/microsoft/mimalloc
- local changes: none - the release tree is vendored as-is (only `include/`
  + the upstream `mi_sources` set under `src/`; tests/docs/CMake machinery
  are not needed for a static target)
- build: the upstream static-library source set (alloc.c ... threadlocal.c
  + `src/prim/prim.c`) compiles into a plain-C `mimalloc_vendored` static
  target; headers are SYSTEM so the `-Werror` consumers that include
  `<mimalloc.h>` do not see upstream warnings. `utility.better_pmr` routes
  every `std::pmr` allocation through it (`mi_aligned_alloc` /
  `mi_free_aligned`).
- note: vendored so the project needs no system mimalloc (previously the
  MSYS2 package's `libmimalloc.dll` was linked); the Windows Release exe is
  fully static, so this dll dependency is gone entirely.
- license: MIT (`LICENSE` in the subtree)
