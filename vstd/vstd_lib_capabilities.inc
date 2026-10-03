// -*- C++ -*-
//===----------------------------------------------------------------------===//
// vstd portability layer, part 2: capabilities that can only be asked about AFTER a library header
//
// WHY THIS IS A SECOND FILE AND NOT PART OF vstd_compat.inc. vstd_compat.inc runs before any library
// header (it is included right after <__config>, which is what makes the toolchain's capability macros
// exist under libc++). That is the right place for questions about the COMPILER - __cplusplus,
// _MSVC_LANG, __cpp_char8_t, __cpp_rtti, __has_include - but the wrong place for a question about the
// LIBRARY, because a feature-test macro like __cpp_lib_print is only defined once a library header
// that publishes it has been included.
//
// MEASURED, MSVC 19.44.35229 / VS 17.14, /std:c++latest: __cpp_lib_print and __cpp_lib_chrono are
// UNDEFINED in a translation unit that has included nothing, and defined (202406 / 201907) after any
// STL header - MSVC keeps its feature-test macros in <yvals_core.h>, and it has no <__config> for the
// early layer to include. Both capabilities are genuinely present (`using std::vprint_nonunicode;` and
// `using std::chrono::locate_zone;` both compile), so asking early did not measure "MSVC lacks this",
// it measured "not yet declared" - and the result was a std module that silently dropped both surfaces.
//
// WHERE IT IS INCLUDED: vstd.cppm, inside the global module fragment, after the standard header list
// and before `export module vstd;` - the last point at which a macro can still be seen by every
// partition, and the first point at which the library has answered.
//
// UNDER LIBC++ THIS FILE DOES NOTHING, on purpose: <__config> has already answered both questions from
// the library's own capability flags (_LIBCPP_HAS_TIME_ZONE_DATABASE / _LIBCPP_HAS_UNICODE), and
// re-answering them from the standard feature-test macros could disagree with a library whose build
// switched the capability off. The rule from vstd_compat.inc still holds: a VSTD_* macro is defined
// ONLY when the capability is really there, and never as 0.
//===----------------------------------------------------------------------===//

#ifndef VSTD_LIB_CAPABILITIES_INC
#define VSTD_LIB_CAPABILITIES_INC

#if !VSTD_IMPLEMENTATION_LIBCXX

// std::chrono time zone database (std::chrono::locate_zone / zoned_time). P0415/P0355; MSVC reports
// __cpp_lib_chrono = 201907L for it. Note this is the only question here that is not new-in-C++23:
// libstdc++ has shipped the tzdb for longer than MSVC.
#  if !defined(VSTD_HAS_TIME_ZONE_DATABASE) && defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
#    define VSTD_HAS_TIME_ZONE_DATABASE 1
#  endif

// <print> (std::print / std::println and the v* forms). std/print.inc additionally guards its
// vprint_nonunicode re-export on VSTD_HAS_UNICODE, so both have to be right for that line to appear.
#  if !defined(VSTD_HAS_UNICODE) && defined(__cpp_lib_print)
#    define VSTD_HAS_UNICODE 1
#  endif

#endif // !VSTD_IMPLEMENTATION_LIBCXX

#endif // VSTD_LIB_CAPABILITIES_INC
