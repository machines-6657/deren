// vstd - MSVC dialect of the project's STL module.
//
// WHY THIS FILE EXISTS
// --------------------
// vstd/vstd.cppm is generated from libc++'s own std-module output and re-exports the standard
// library's surface as 71 `export namespace std { using ...; }` partitions.  That construction
// makes the MSVC 19.44 front end crash: as soon as a TU that imported vstd instantiates a
// `std::span` (or `std::array`, or `std::tuple`) entity, cl.exe dies with
// `fatal error C1001: internal compiler error` in symbols.c / function-signature.cpp / msc1.cpp.
// Measured on the real tree with the toolchain, not inferred: 7 of the 326 objects fail that way
// and every one of them imports vstd (evidence and minimal reproductions:
// build-release-clang64/msvc/lead_lab/ICE_FINDINGS.md, and the probe logs under
// build-release-clang64/msvc/dialect/).
//
// The same consumer built against the MSVC toolchain's OWN std module (`export module vstd;
// export import std;`) compiles clean, and so does the whole real tree's vstd step once the
// payload below is used.  So under MSVC vstd is that module: the toolchain's std module,
// re-exported under the project's module name.
//
// WHAT THAT COSTS - STATED PLAINLY, NOT HIDDEN
// -------------------------------------------
// * Under MSVC the standard library is the MSVC STL (with its own libc++-vs-MSVC semantics), not
//   the libc++ that vstd/vstd.cppm is generated from and byte-bound to.  The clang64 build - the
//   one this repository verifies, ships and benchmarks - is untouched by this file.
// * The VSTD_* portability macros (vstd/vstd_compat.inc) are NOT part of this module's face.  That
//   is safe today by measurement: nothing outside vstd/ names a VSTD_* macro, and nothing in the
//   tree writes `vstd::` for a project-local extension - every consumer only wants the standard
//   library's names, which this module re-exports as a superset.
// * MSVC's std module is a BMI, not a header set: CMakeLists.txt builds it (CMake's
//   CXX_MODULE_STD) rather than letting each TU include the headers.  There is no prebuilt
//   std.ifc in a Visual Studio installation; Visual Studio only ships modules/std.ixx.
//
// VERSIONING: this file does NOT change the face of the shipped (clang64) module, so
// vstd/vstd.cppm's banner stays at 0.1.1a.  If the project decides the MSVC dialect is part of
// vstd's interface (it does change which library's entities a consumer sees), bump it there and
// here together - the choice is the project's, not this file's.
export module deren.vstd;

export import std;
