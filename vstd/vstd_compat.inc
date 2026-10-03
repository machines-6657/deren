// -*- C++ -*-
//===----------------------------------------------------------------------===//
// vstd portability layer
//
// WHY THIS EXISTS. The partitions in std/ are libc++'s generated re-export blocks, and the only
// implementation-specific things they contain are CONDITIONALS: `_LIBCPP_STD_VER`, the
// `_LIBCPP_HAS_*` capability flags, the `_LIBCPP_ENABLE_*` opt-ins, and `_LIBCPP_USING_IF_EXISTS`.
// Every entity they export is a STANDARD name (libc++ generates them from the standard's synopsis),
// so the partitions themselves do not need to change per toolchain - the macros do.
//
// This file gives each of those one VSTD_* spelling, and it is included after <__config> and before
// any partition, so a partition names VSTD_* while the value still comes from the toolchain:
//
//   * under libc++ every VSTD_* is defined AS its _LIBCPP_* counterpart whenever that is defined, so
//     the module this project has been building is the same module (the gate's nine scenarios are the
//     proof);
//   * under another standard library the same names come from the standard's own feature-test macros
//     and from __has_include, which is what makes the toolchain swap possible at all.
//
// THIS IS ALSO THE PER-COMPILER DIALECT LAYER, and it is split in two by what can be known WHEN:
// everything in THIS file is a question about the compiler (__cplusplus, _MSVC_LANG, __cpp_rtti,
// __cpp_char8_t, __has_include, __has_builtin) or about libc++, and it must run before any library
// header; the two questions that need a library header to have been included live in
// vstd_lib_capabilities.inc, which vstd.cppm includes after its header list. MSVC is where that
// distinction was measured rather than guessed - see the comments on each decision below.
//
// THE ONE RULE THAT MATTERS, and it is a rule about DEFINEDNESS rather than about value: a VSTD_*
// macro is defined ONLY WHEN THE CAPABILITY IS THERE, never as 0. The partitions test these two ways -
// `#if VSTD_HAS_THREADS` and `#ifdef VSTD_ENABLE_EXPERIMENTAL` - and only "defined iff capable" makes
// both forms mean exactly what the libc++ original meant. Defining them as 0 breaks the `#ifdef`
// form silently: the first version of this file did that, and it compiled libc++'s EXPERIMENTAL time
// zone database into the module because `#ifdef VSTD_ENABLE_EXPERIMENTAL` was true while
// `_LIBCPP_ENABLE_EXPERIMENTAL` was not defined at all.
//
// WHAT THIS CANNOT FIX, stated so nobody expects otherwise: a name that the *other* library has not
// implemented still fails to compile where a partition says `using std::that_name;`. That residue is
// found by compiling against that library and guarded there, one line at a time - no macro can
// conjure it, and `_LIBCPP_USING_IF_EXISTS` softens it only on a compiler with
// __attribute__((using_if_exists)) (clang; GCC and MSVC do not have it, so there the using is hard).
//
// MAINTENANCE: when a std/X.inc is refreshed from a newer libc++, a new _LIBCPP_* macro in it shows
// up as a compile error here - the intended failure mode. Add its VSTD_* spelling below.
//===----------------------------------------------------------------------===//

#ifndef VSTD_COMPAT_INC
#define VSTD_COMPAT_INC

// ---- __has_builtin, made explicit ------------------------------------------------------------
// The partitions write things like `#if VSTD_STD_VER >= 23 && __has_builtin(__builtin_is_implicit_lifetime)`.
// MSVC does not provide __has_builtin (measured on 19.44.35229: `#ifdef __has_builtin` is false), and
// those lines nonetheless compile today only because the traditional preprocessor is lenient about an
// unknown function-like identifier and evaluates the whole condition as false - a property of the
// preprocessor, not of the code, and one that /Zc:preprocessor does not have to keep. Defining it as
// "no builtins" makes the partition's intent explicit and keeps both preprocessors in agreement.
#ifndef __has_builtin
#  define __has_builtin(x) 0
#endif

// ---- the conditional-using attribute, made portable the same way libc++ itself makes it portable ----
// clang has it; GCC and MSVC do not, where it expands to nothing and a using of a name the library
// lacks becomes a hard error (the residue described above).
#ifndef VSTD_USING_IF_EXISTS
#  if defined(__has_attribute)
#    if __has_attribute(using_if_exists)
#      define VSTD_USING_IF_EXISTS __attribute__((using_if_exists))
#    endif
#  endif
#  ifndef VSTD_USING_IF_EXISTS
#    define VSTD_USING_IF_EXISTS
#  endif
#endif

#if defined(_LIBCPP_VERSION)

// ============================ libc++ ============================
// Every name IS the libc++ name, each one defined exactly when libc++ defines it, so this layer is
// invisible on the toolchain the project builds with today.
#  define VSTD_IMPLEMENTATION_LIBCXX 1

#  ifdef _LIBCPP_STD_VER
#    define VSTD_STD_VER _LIBCPP_STD_VER
#  endif
#  ifdef _LIBCPP_HAS_WIDE_CHARACTERS
#    define VSTD_HAS_WIDE_CHARACTERS _LIBCPP_HAS_WIDE_CHARACTERS
#  endif
#  ifdef _LIBCPP_HAS_LOCALIZATION
#    define VSTD_HAS_LOCALIZATION _LIBCPP_HAS_LOCALIZATION
#  endif
#  ifdef _LIBCPP_HAS_THREADS
#    define VSTD_HAS_THREADS _LIBCPP_HAS_THREADS
#  endif
#  ifdef _LIBCPP_HAS_FILESYSTEM
#    define VSTD_HAS_FILESYSTEM _LIBCPP_HAS_FILESYSTEM
#  endif
#  ifdef _LIBCPP_HAS_TIME_ZONE_DATABASE
#    define VSTD_HAS_TIME_ZONE_DATABASE _LIBCPP_HAS_TIME_ZONE_DATABASE
#  endif
#  ifdef _LIBCPP_HAS_CHAR8_T
#    define VSTD_HAS_CHAR8_T _LIBCPP_HAS_CHAR8_T
#  endif
#  ifdef _LIBCPP_HAS_UNICODE
#    define VSTD_HAS_UNICODE _LIBCPP_HAS_UNICODE
#  endif
#  ifdef _LIBCPP_HAS_RTTI
#    define VSTD_HAS_RTTI _LIBCPP_HAS_RTTI
#  endif
#  ifdef _LIBCPP_HAS_MONOTONIC_CLOCK
#    define VSTD_HAS_MONOTONIC_CLOCK _LIBCPP_HAS_MONOTONIC_CLOCK
#  endif
#  ifdef _LIBCPP_HAS_ATOMIC_HEADER
#    define VSTD_HAS_ATOMIC_HEADER _LIBCPP_HAS_ATOMIC_HEADER
#  endif
#  ifdef _LIBCPP_NO_LOCK_FREE_TYPES
#    define VSTD_NO_LOCK_FREE_TYPES _LIBCPP_NO_LOCK_FREE_TYPES
#  endif
// std::aligned_alloc: libc++ declares it in <cstdlib>; libstdc++ 16.2.0 keeps it in the GLOBAL
// namespace only (measured with `using std::aligned_alloc;`), and no feature-test macro covers it, so
// this is a library fact rather than a standard one.
#  define VSTD_HAS_STD_ALIGNED_ALLOC 1
#  ifdef _LIBCPP_HAS_EXPERIMENTAL_SYNCSTREAM
#    define VSTD_HAS_EXPERIMENTAL_SYNCSTREAM _LIBCPP_HAS_EXPERIMENTAL_SYNCSTREAM
#  endif
#  ifdef _LIBCPP_HAS_EXPERIMENTAL_OPTIONAL_ITERATOR
#    define VSTD_HAS_EXPERIMENTAL_OPTIONAL_ITERATOR _LIBCPP_HAS_EXPERIMENTAL_OPTIONAL_ITERATOR
#  endif
// The three opt-ins are DEFINED-NESS tests in the partitions (#ifdef), so they must stay undefined
// unless the toolchain defines them - which is what makes the module's experimental surface follow
// the toolchain's, exactly as the libc++ original did.
#  ifdef _LIBCPP_ENABLE_EXPERIMENTAL
#    define VSTD_ENABLE_EXPERIMENTAL _LIBCPP_ENABLE_EXPERIMENTAL
#  endif
#  ifdef _LIBCPP_ENABLE_CXX26_REMOVED_WSTRING_CONVERT
#    define VSTD_ENABLE_CXX26_REMOVED_WSTRING_CONVERT _LIBCPP_ENABLE_CXX26_REMOVED_WSTRING_CONVERT
#  endif
#  ifdef _LIBCPP_ENABLE_CXX20_REMOVED_UNCAUGHT_EXCEPTION
#    define VSTD_ENABLE_CXX20_REMOVED_UNCAUGHT_EXCEPTION _LIBCPP_ENABLE_CXX20_REMOVED_UNCAUGHT_EXCEPTION
#  endif

#else // !_LIBCPP_VERSION

// ============================ another standard library ============================
// Same rule: define a VSTD_* only when the capability is really there. The libc++-only opt-ins and
// libc++-experimental capabilities are therefore simply NOT defined, so the partitions that name them
// export nothing - which is correct, because nothing else has those entities.
#  define VSTD_IMPLEMENTATION_LIBCXX 0

// The standard language level in libc++'s numbering (20/23/26), so `VSTD_STD_VER >= 23` keeps meaning
// "the library is in C++23 mode or later".
//
// WHY __cplusplus IS NOT ENOUGH HERE (measured, MSVC 19.44.35229 / VS 17.14): without /Zc:__cplusplus
// MSVC leaves __cplusplus at its prehistoric 199711L even under /std:c++latest, while _MSVC_LANG
// reports the real level (202400) either way. Reading only __cplusplus therefore fell through every
// branch below - and because this block had no else, VSTD_STD_VER was simply left UNDEFINED. That
// failure is silent by construction: the partitions test `#if VSTD_STD_VER >= 23`, an undefined macro
// is 0 there, so the module quietly loses its entire modern surface and the first error appears in
// whoever included it (in this project: std::expected, from gltf_loader). So the level is taken from
// whichever macro reports it, and a level this layer cannot determine is a hard error HERE.
#  if defined(_MSVC_LANG) && _MSVC_LANG > __cplusplus
#    define VSTD_LANG_LEVEL _MSVC_LANG
#  else
#    define VSTD_LANG_LEVEL __cplusplus
#  endif
#  if VSTD_LANG_LEVEL >= 202302L
#    define VSTD_STD_VER 23
#  elif VSTD_LANG_LEVEL >= 202002L
#    define VSTD_STD_VER 20
#  elif VSTD_LANG_LEVEL >= 201703L
#    define VSTD_STD_VER 17
#  else
#    error "vstd: cannot determine the C++ language level - both __cplusplus and _MSVC_LANG are below 201703L. Compile as C++17 or later, and on MSVC add /Zc:__cplusplus or /std:c++20 (or later)."
#  endif
#  undef VSTD_LANG_LEVEL

#  if defined(__cpp_char8_t)
#    define VSTD_HAS_CHAR8_T 1
#  endif
#  if __has_include(<cwchar>)
#    define VSTD_HAS_WIDE_CHARACTERS 1
#  endif
#  if __has_include(<locale>)
#    define VSTD_HAS_LOCALIZATION 1
#  endif
#  if __has_include(<thread>)
#    define VSTD_HAS_THREADS 1
#  endif
#  if __has_include(<filesystem>)
#    define VSTD_HAS_FILESYSTEM 1
#  endif
// VSTD_HAS_TIME_ZONE_DATABASE and VSTD_HAS_UNICODE ARE NOT DECIDED HERE even though they are
// feature-test macros: on MSVC those macros only exist once a library header has been included
// (MSVC keeps them in <yvals_core.h>, and it has no <__config> for this file to pull in), and this
// file runs before any library header. They are decided in vstd/vstd_lib_capabilities.inc, which
// vstd.cppm includes after its header list and before `export module vstd;`. Measured: at THAT point
// MSVC reports __cpp_lib_chrono = 201907 and __cpp_lib_print = 202406, and `using
// std::chrono::locate_zone;` / `using std::vprint_nonunicode;` both compile - so the two capabilities
// are really there, it was only ever the timing of the question that was wrong.
// RTTI IS NOT EXCEPTIONS. This used to be `#if defined(__cpp_exceptions)`, which asks about a
// different capability: an MSVC build with /EHs-c- (or clang with -fno-exceptions) would have lost
// VSTD_HAS_RTTI while RTTI was still enabled. Measured on MSVC 19.44: __cpp_rtti = 199711 and
// _CPPRTTI = 1 with RTTI on, and _CPPRTTI disappears under /GR-.
#  if defined(__cpp_rtti) || defined(_CPPRTTI)
#    define VSTD_HAS_RTTI 1
#  endif
#  if __has_include(<chrono>)
#    define VSTD_HAS_MONOTONIC_CLOCK 1
#  endif
#  if __has_include(<atomic>)
#    define VSTD_HAS_ATOMIC_HEADER 1
  // (no VSTD_HAS_STD_ALIGNED_ALLOC here ON PURPOSE, and the measurement now covers both non-libc++
  // libraries this project has tried: libstdc++ 16.2.0 and MSVC 19.44 both keep aligned_alloc in the
  // GLOBAL namespace, so `using std::aligned_alloc;` after <cstdlib> fails on both (MSVC: C2039
  // "not a member of std" plus C2873) and no feature-test macro covers it. The partition guards its
  // line on this name, so leaving it undefined is what makes that line vanish. Re-measure on an
  // upgrade and define it if the deviation is fixed.)
#  endif

#endif // _LIBCPP_VERSION

#endif // VSTD_COMPAT_INC
