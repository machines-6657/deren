# vstd — the project's STL module

> **What this is**: `vstd` is **modified from libc++** (LLVM's C++ standard
> library, the C++23 library shipped with clang). It is *not* a rewrite of
> the STL, *not* derived from any other std implementation (e.g. GNU
> libstdc++), and *not* an independent implementation: it is libc++'s own
> generated `std` module, renamed to `vstd` and **trimmed to the headers this
> project actually uses**. The exported entities are the libc++ entities,
> re-exported with `using`; the definitions come from the toolchain's libc++
> headers at compile time.
>
> The whole module is therefore **byte-bound to the matching libc++** of the
> MSYS2 clang64 toolchain — keep it in sync on toolchain upgrades (see below).
>
> **UPDATE (portability layer)**: the binding is now confined to ONE file. The partitions
> name `VSTD_*` instead of `_LIBCPP_*`, and `vstd/vstd_compat.inc` defines each of those
> names — under libc++ *as* the `_LIBCPP_*` macro it replaces, and under another standard
> library from that library's own feature-test macros. The partitions themselves contain
> no implementation-specific macro, and no internal namespace: see "Portability layer"
> below for what that buys and what it does not.

## Layout

```
vstd/
  vstd.cppm      module interface: global-fragment #include <X> lines
                 (pull libc++ definitions) + export module vstd; +
                 one #include "std/X.inc" per used header (editable whitelist)
  vstd_msvc.cppm MSVC dialect of the same module: `export import std;`, i.e.
                 the MSVC toolchain's own std module - see "MSVC dialect"
  std/           the .inc partitions referenced by vstd.cppm (71 today),
                 copied from libc++'s module output - do not hand-edit
  LICENSE        LLVM project license (Apache-2.0 with LLVM exceptions),
                 covering the .inc content
  README.md      this file
```

Each `.inc` partition is a pure re-export block derived from the libc++
original: `export namespace std { using std::vector; ... }`. The full
upstream `std.cppm` / `std.compat.cppm` are **not** vendored here; only the
used partitions are kept.

## MSVC dialect (`vstd_msvc.cppm`)

The MSVC build does **not** compile `vstd.cppm`. It compiles `vstd_msvc.cppm`, whose whole body is

```c++
export module vstd;
export import std;
```

i.e. the MSVC toolchain's own `std` module, re-exported under the project's module name. This is
not a preference, it is a workaround for a measured compiler bug: with the libc++-derived
re-export construction, `cl.exe` 19.44 dies with `fatal error C1001` (internal compiler error,
three different signatures) on every TU that instantiates `std::span`, `std::array` or
`std::tuple` through the module - 7 of the tree's 326 objects, every one of them an importer of
`vstd`. The same consumers compile clean against the toolchain's own `std` module. Minimal
reproductions, a partition-level bisection and the counter-experiments live in
`build-release-clang64/msvc/lead_lab/ICE_FINDINGS.md`; the switch itself is in `CMakeLists.txt`
(plus an `if(MSVC)` branch at `vstd_lib` and CMake's experimental `import std;` gate, which has to
be set before `project()`).

What the dialect costs, stated rather than hidden:

- under MSVC the standard library is the **MSVC STL**, not the libc++ that `vstd.cppm` is
  generated from and byte-bound to. The clang64 build - the verified, shipped, benchmarked one -
  is untouched: with the gate and the branch in place its exe is byte-identical
  (`sha256 89084B8FD7400DE6A6878AEFCD88C5FA0208552552AE8E2DF943910B14AE10E0`).
- the `VSTD_*` portability macros are not part of the MSVC module's face. Measured safe today:
  no source outside `vstd/` names a `VSTD_*` macro, and no consumer writes `vstd::` for a
  project-local extension - consumers only want standard names, and the MSVC STL is a superset of
  the libc++ surface for them.
- there is no prebuilt `std.ifc` in a Visual Studio installation (only `modules/std.ixx`), so
  CMake builds that module as part of the build (`CXX_MODULE_STD`).

The version banner in `vstd.cppm` is deliberately **not** bumped by this file: it does not change
the clang64 module's face. If the project ever treats the MSVC dialect as part of `vstd`'s
interface, bump it in `vstd.cppm` and here together.

## Portability layer

`vstd/vstd_compat.inc` is included once, after `<__config>` and before the partitions. It gives every
implementation-specific macro the partitions use one portable spelling:

| partition says | libc++ (`_LIBCPP_*`) | another standard library |
|---|---|---|
| `VSTD_STD_VER` | `_LIBCPP_STD_VER` | derived from `__cplusplus`, in libc++'s numbering |
| `VSTD_HAS_*` (13 names) | the `_LIBCPP_HAS_*` flag | the standard's feature-test macro, else `__has_include` of the header |
| `VSTD_ENABLE_*` (3 opt-ins) | the `_LIBCPP_ENABLE_*` opt-in | never defined (nothing else has those entities) |
| `VSTD_USING_IF_EXISTS` | `__has_attribute(using_if_exists)` | expands to nothing |

**THE RULE THAT MATTERS: a `VSTD_*` name is defined ONLY WHEN THE CAPABILITY IS THERE, never as 0.**
The partitions test both ways — `#if VSTD_HAS_THREADS` and `#ifdef VSTD_ENABLE_EXPERIMENTAL` — and
only "defined iff capable" makes both mean what the libc++ original meant. The first version of the
layer defined them as 0 and silently compiled libc++'s *experimental* time zone database into the
module, because `#ifdef` was true while the toolchain's opt-in was not defined.

**What the layer cannot do**, so nobody expects otherwise: a name the other library has not
implemented still fails where a partition says `using std::that_name;`. That residue is found by
compiling against that library and guarded there, line by line; and `VSTD_USING_IF_EXISTS` softens it
only on a compiler with `__attribute__((using_if_exists))` (clang — GCC and MSVC do not, so there the
`using` is hard).

**What is NOT allowed back in**: a partition may not name a libc++ internal namespace. Four did —
`compare.inc`, `concepts.inc`, `iterator.inc`, `ranges.inc` re-exported the standard CPOs as
`using std::__cpo::...` / `using std::ranges::__cpo::...` inside an `inline namespace __cpo`. Those
are inline namespaces in libc++, so the public spelling (`std::partial_order`, `std::ranges::begin`,
…) names the same entity, and that is what the partitions say now.

## Portability notes (inspected)

The partitions are mechanically generated from libc++ and were audited:

- every line is a `using`-declaration inside `export namespace std` (plus
  comments/blank lines); no pragmas, no `#elif`/`#else`; `#if`/`#endif` pairs match 1:1.
- the only conditionals are the `VSTD_*` names above and `__has_builtin`; measured over the 71
  partitions: zero `_LIBCPP_*`, zero `_MSC_VER`/`__linux__`/`__APPLE__`/`__GLIBCXX`.
- **one platform branch remains, and it is upstream's**: `vstd.cppm` wraps the
  `__has_include(<debugging>)` / `__has_include(<text_encoding>)` sanity check in `#ifndef _WIN32`,
  because on Windows the MSVC STL headers are on the search path and would make `__has_include` lie.
  (An earlier revision of this file claimed "zero `_WIN32` hits"; measured, there are two, both that
  guard.) It is harmless and necessary.

So the content is **toolchain-bound, not platform-bound**: it compiles
against the matching libc++ headers and must be regenerated with them, but it
has no Windows/Linux/compiler-specific forks of its own.

## Versioning

`vstd` follows the same independent version scheme as every other module in the
repo (banner at the top of `vstd.cppm`, independent of the app version in
`project(VERSION)`):

- current: **0.1.0a**
- MAJOR: breaking interface changes
- MINOR: additive features — including project-local **extensions** (`vstd`
  is the natural home for STL additions beyond libc++; the module is not just a
  re-export shim, so bump MINOR when one lands)
- PATCH: internal fixes (trim changes, partition refreshes on a libc++ patch
  upgrade)

## Editing / trimming

`vstd.cppm` keeps one `#include <X>` + `#include "std/X.inc"` pair per used
header, in the upstream order.

- **Adding** an STL feature: the compiler reports the missing entity; add that
  header's pair to `vstd.cppm` and copy its partition into `std/` (from the
  toolchain's upstream libc++ module output).
- **Removing**: drop both lines and the partition file.

## Toolchain upgrade

The snapshot must match the installed libc++ (byte-compatible headers), as
always. On a clang64 upgrade:

1. From the new toolchain's libc++ include tree, obtain the fresh upstream
   module output (the generated `std.cppm` and its `std/` partition set - the
   same source this repo originally modified).
2. For every header currently listed in `vstd.cppm`, copy the fresh `X.inc`
   over `std/X.inc` (contents may change between libc++ versions) and confirm
   the `#include <X>` lines still exist upstream; **rename every `_LIBCPP_*`
   macro in the fresh partition to its `VSTD_*` spelling** (the names are listed
   in `vstd/vstd_compat.inc`), then re-verify the trim with a full build.
3. If a fresh partition names a macro the compat layer does not define yet, the
   build fails on that name - the intended failure mode, not something to work
   around: add its `VSTD_*` spelling to `vstd/vstd_compat.inc` (defined only when
   the capability is there) and rebuild.
4. `vstd/std/compare.inc`, `concepts.inc`, `iterator.inc` and `ranges.inc` carry
   a locally written `PORTABILITY LAYER` note where libc++'s generated output
   re-exported an internal namespace (`std::__cpo`, `std::ranges::__cpo`). The
   fresh copy will bring those lines back: replace them with the public spelling
   again (see "Portability layer").
