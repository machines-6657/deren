# Compiler tolerance

What this project needs from a toolchain, and what was MEASURED when a second one was tried. The short
version: the code is C++23 with C++20 modules and was written against **libc++**; three things in it turned out
to be libc++-only and are fixed, and what still stops a second toolchain is the toolchain's own problem and one
vendored header's.

## What is supported

The configuration CI builds and the capture gate runs on: MSYS2 **clang64** (clang 22, libc++, UCRT), CMake +
Ninja, shaders compiled by `slangc`. `-Werror` is on for first-party code in every configuration, so a warning
this compiler emits but CI's does not is a build failure here rather than a curiosity.

MSVC is a second configuration the BUILD FILES support but the scripts and CI do not drive:
`scripts/windows/build.ps1` requires `clang++` and `.github/workflows/ci.yml` installs MSYS2 clang64. The three
cache variables an MSVC build needs are documented in the README rather than here.

Everything below about GCC is a MEASUREMENT, not a promise. The audit ran GCC 16.2 (MinGW, ucrt64) against the
same tree: it now compiles all of it and links five of the thirteen test executables, and does not link the rest.
Nothing in the build files supports that configuration yet, and this document is why nobody has to re-derive
what happens if it is tried.

## The three things that were libc++-only

### `std::print` does not link on MinGW's libstdc++

MinGW's libstdc++ 16.2 ships `<print>`'s **declarations** without the terminal-writing half of its
implementation, so a call site fails at LINK time with

```
undefined reference to `std::__open_terminal(_iobuf*)'
undefined reference to `std::__write_to_terminal(void*, std::span<char, ...>)'
```

`nm --defined-only libstdc++.a` finds no definition of either, and `bits/print.h`'s `vprint_unicode` is what
references them. The tree therefore uses **`deren::utility::print` / `deren::utility::println`** (`utility/utility.cppm`),
which are `std::print`'s semantics with the project owning them: `std::format_string` parameters, so the format
string is still checked at compile time, one `std::fwrite`, no flush, and `std::format` taken from `deren.vstd`.

Two details in that family are load-bearing rather than style:

  - `standard_output()` exists because **the C standard streams are macros**. `stdout` expands to a call into
    the C library's FILE table rather than naming an object a module could export, and the alternative - a
    textual `<cstdio>` in the module INTERFACE - declares libc++'s entities twice, once in the module's global
    fragment and once through `export import deren.vstd`. clang 22.1.8 answers that with a crash in code generation
    (`EmitBuiltinNewDeleteCall` on `std::__libcpp_allocate`), so the macro stays in the implementation unit.
  - the family's own internal calls are **qualified** (`deren::utility::print(...)`), because an unqualified one is
    ambiguous against libc++'s `std::print(std::FILE*, format_string<Args...>, Args&&...)`, which an unqualified
    lookup inside the namespace finds.

### `NOMINMAX` must be guarded before `<windows.h>`

libstdc++'s `c++config.h`, reached through `<cstddef>` / `<cstdint>` / `<cstdio>` on the way to `<windows.h>`,
already defines `NOMINMAX`. The three platform translation units used to define it unconditionally, which is a
**re**definition - and `-Werror` makes that a build failure. They guard it now; libc++'s configuration does not
define it, which is why the clang64 build never saw this.

### A `std::unique_ptr` over a forward-declared type needs no in-class initializer

`vulkan/acceleration_structure/acceleration_structure.cppm` holds two `std::unique_ptr<entry_points>` whose
type is only forward-declared there (the definition is in the .cpp), which is the ordinary pImpl shape. Both had
`= {}`, and **libstdc++ instantiates `~unique_ptr<entry_points>` at that default member initializer itself**,
stopping on `default_delete`'s `static_assert(sizeof(_Tp)>0)`; libc++ does not. Declaring the destructor out of
line - which both classes already did - is NOT enough. Removing the initializer is: the member is
default-initialized by the constructor instead, which is the same work and puts the instantiation in the .cpp
where the type is complete.

MEASURED: with those two initializers in place the GCC build reports exactly two compile errors, both this; with
them removed it reports zero. **So: in a module interface, a `unique_ptr` to an incomplete type is declared
without an initializer, and the initialization happens in the implementation unit.**

## One clang bug, worked around rather than fixed

`tests/vk_test.h` formats its own output (`std::format` + one `std::fwrite`) and deliberately does NOT call the
project's print family. The reason is a **clang 22.1.8 crash**: with an inline function in that header calling
the module's variadic template, `test_shadow_fit.cpp` and `test_animation.cpp` die in code generation
("clang frontend command failed due to signal", inside `EmitBuiltinNewDeleteCall` /
`std::__libcpp_allocate`). The minimal reproduction of that shape - a module exporting such a template, a header
with an inline function calling it, and a translation unit that imports and then includes - compiles cleanly on
its own, so the trigger is the combination of the two rather than either half. A test harness is not worth a
compiler bug, so the harness formats here and every test stays a plain translation unit that imports only the
module under test.

## What still stops a full GCC build

Both are outside this repository's code, and both are recorded here so the answer to "why does it not build with
GCC?" is a sentence rather than a re-run:

  - **libstdc++ emits template-local statics in more than one object without merging them**: 54 references to
    `multiple definition of std::__format::__write_escaped_unicode_part<char, _Sink_iter<char>>::__replace_rep`
    (`<format>:1155`) plus one `std::_Sp_make_shared_tag::_S_ti()::__tag`. Any project that instantiates
    `std::format` from two translation units hits this on this toolchain, whether or not it uses `<print>`.
  - **the vendored VMA's allocator helpers**: 80 `undefined reference to (anonymous namespace)::vma_aligned_alloc`
    / `vma_aligned_free`, referenced from outside the unit that defines them. VMA's platform branches for those
    are `__ANDROID__` / `__APPLE__` / `__linux__` and none of them is MinGW, so this is a GCC-modules
    interaction with a vendored header rather than a configuration this project sets.

## Reproducing the audit

There is nothing to install for the second toolchain except GCC itself, but the dependency roots differ: the
MSYS2 packages this project uses (`glm`, `toml++`, `glfw3`, the Vulkan headers) live in the **clang64** root, so
a ucrt64 GCC build needs either their ucrt64 packages or the clang64 roots borrowed. What the audit did:

  1. a **wrapper compiler** that appends `-Wno-error` after the project's `-Werror` (a `.bat` forwarding to
     `g++.exe %* -Wno-error`), because `-Werror` is in the target's own options and therefore comes after
     `CMAKE_CXX_FLAGS` - without it, the macro-redefinition warnings above stop the build before any of the
     interesting failures;
  2. `-DCMAKE_CXX_FLAGS="-isystem C:/msys64/clang64/include"` and
     `-DCMAKE_EXE_LINKER_FLAGS="-L C:/msys64/clang64/lib"`, which is what isolates "the code does not compile"
     from "the packages are not installed";
  3. `cmake --build <dir> -- -k 0`, so every target is attempted and the failures are all visible in one log
     rather than one per run.

The shaders are unaffected: `slangc` is a separate toolchain and compiles the same SPIR-V for either host
compiler.
