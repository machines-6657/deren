// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// ============================================================================
// module: vstd
// module version: 0.1.1a  (independent of the app version in CMakeLists project(VERSION))
//
// The project's STL module, MODIFIED FROM libc++ (LLVM's C++ standard
// library): a trimmed copy of libc++'s generated std-module output (upstream
// generator: utils/generate_libcxx_cppm_in.py, LLVM tree - the full generated
// std.cppm is NOT vendored, only the used std/*.inc partitions). Every
// exported entity is a libc++ entity re-exported via `using`; no STL is
// implemented or rewritten here. The whole module is byte-bound to the
// matching libc++ of the MSYS2 clang64 toolchain.
//
// extend: this module is the natural home for project-local STL extensions
//         (vstd-only additions beyond libc++) later on - bump the version
//         below when they land.
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features
//         (including extensions), PATCH on internal fixes - independently of
//         the rest of the project.
// ============================================================================

/**
 * @file vstd.cppm
 * @defgroup vstd vstd STL Module
 * @brief the project's STL module: modified from libc++ (LLVM), trimmed to the
 *        headers this project uses and consumed via `import vstd;`.
 *
 * @details
 * - The global-module-fragment `#include <...>` lines pull the definitions from
 *   the toolchain's libc++ headers; each `std/X.inc` partition then re-exports
 *   that header's entities (`export namespace std { using std::vector; ... }`).
 * - The file is an **editable whitelist**: one `#include <X>` pair with its
 *   `#include "std/X.inc"` partition per used header; the partition contents
 *   are upstream-generated and must not be hand-edited (see vstd/README.md).
 * - Byte-bound to the matching libc++ of the MSYS2 clang64 toolchain: refresh
 *   the partitions on toolchain upgrades.
 * @version 0.1.0a
 */

module;

// <__config> IS LIBC++-ONLY, and that made this one line the first thing a non-libc++ toolchain
// failed on: "fatal error: __config: No such file or directory" from g++ 16 on the very first
// compile. It is still included first where it exists, because it is what defines _LIBCPP_VERSION and
// the _LIBCPP_HAS_* flags the portability layer below reads; elsewhere it is simply absent, and the
// layer takes its other branch.
#if defined(__has_include)
#  if __has_include(<__config>)
#    include <__config>
#  endif
#endif

// THE PORTABILITY LAYER, and the reason it sits exactly here: <__config> (where the toolchain has one)
// is what makes the toolchain's own capability macros exist, and every partition below is written
// against the VSTD_* spelling this file defines - which under libc++ IS the _LIBCPP_* macro, so this
// changes nothing here and everything for a toolchain that is not libc++ (see vstd/vstd_compat.inc).
#include "vstd_compat.inc"

// The headers of Table 24: C++ library headers [tab:headers.cpp]
// and the headers of Table 25: C++ headers for C library facilities [tab:headers.cpp.c]
#include <algorithm>
#include <any>
#include <array>
// THE HEADER THE TOOLCHAIN MAY NOT HAVE YET, which is why this is an __has_include and not a VSTD_*
// flag: libc++ 22.1.8 does not ship <stdfloat> at all (the negative check further down is what says
// so), while libstdc++ 16.2.0 ships it and defines every __STDCPP_*_T__ macro with it. The partition
// std/stdfloat.inc is guarded by those STANDARD macros, so without this include it is active under
// libstdc++ while nothing has declared the names - measured: five errors, "float16_t has not been
// declared in 'std'". Including it under __has_include is what makes the whitelist and the partition
// agree on both toolchains.
#if defined(__has_include)
#  if __has_include(<stdfloat>)
#    include <stdfloat>
#  endif
#endif
// The same reasoning, and it was luck rather than agreement before: <atomic> was included under a
// toolchain FLAG (`VSTD_HAS_ATOMIC_HEADER`, libc++'s own, undefined under libstdc++) while
// std/atomic.inc is included unconditionally - it survived only because libstdc++ happens to declare
// std::atomic through other headers. __has_include asks the question that actually matters.
#if defined(__has_include)
#  if __has_include(<atomic>)
#    include <atomic>
#  endif
#endif
#include <bit>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <compare>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <ios>
#include <iosfwd>
#include <iterator>
#include <limits>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <numeric>
#include <optional>
#include <print>
#include <queue>
#include <ranges>
#include <ratio>
#include <set>
#include <shared_mutex>
#include <source_location>
#include <span>
#include <stack>
#include <stdexcept>
#include <stop_token>
#include <istream>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>
#include <version>

// *** Headers not yet available ***
//
// This validation is mainly to catch when a new header is added but adding the
// corresponding .inc file is forgotten. However, the check based on __has_include
// alone doesn't work on Windows because the Windows SDK is on the include path,
// and that means the MSVC STL headers can be found as well, tricking __has_include
// into thinking that libc++ provides the header.
//
#ifndef _WIN32
#  if __has_include(<debugging>)
#    error "please update the header information for <debugging> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<debugging>)
#  if __has_include(<generator>)
#    error "please update the header information for <generator> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<generator>)
#  if __has_include(<hazard_pointer>)
#    error "please update the header information for <hazard_pointer> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<hazard_pointer>)
#  if __has_include(<inplace_vector>)
#    error "please update the header information for <inplace_vector> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<inplace_vector>)
#  if __has_include(<linalg>)
#    error "please update the header information for <linalg> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<linalg>)
#  if __has_include(<rcu>)
#    error "please update the header information for <rcu> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<rcu>)
#  if __has_include(<spanstream>)
#    error "please update the header information for <spanstream> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<spanstream>)
#  if __has_include(<stacktrace>)
#    error "please update the header information for <stacktrace> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<stacktrace>)
#  if __has_include(<stdfloat>)
#    error "please update the header information for <stdfloat> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<stdfloat>)
#  if __has_include(<text_encoding>)
#    error "please update the header information for <text_encoding> in headers_not_available in utils/libcxx/header_information.py"
#  endif // __has_include(<text_encoding>)
#endif // _WIN32

// THE PORTABILITY LAYER, PART 2, and the reason it sits exactly here rather than beside part 1: part 1
// answers questions about the COMPILER and must run before any header; this one answers questions
// about the LIBRARY (__cpp_lib_print, __cpp_lib_chrono) and can only run once a library header has
// published those macros. Under libc++ it is a no-op - <__config> already answered them from libc++'s
// own capability flags - so the module this project has been building stays the same module.
// See vstd/vstd_lib_capabilities.inc for the measurement that put it here.
#include "vstd_lib_capabilities.inc"

export module deren.vstd;


#include "std/algorithm.inc"
#include "std/any.inc"
#include "std/array.inc"
#include "std/atomic.inc"
#include "std/bit.inc"
#include "std/cctype.inc"
#include "std/cerrno.inc"
#include "std/charconv.inc"
#include "std/chrono.inc"
#include "std/climits.inc"
#include "std/cmath.inc"
#include "std/compare.inc"
#include "std/concepts.inc"
#include "std/condition_variable.inc"
#include "std/cstddef.inc"
#include "std/cstdint.inc"
#include "std/cstdio.inc"
#include "std/cstdlib.inc"
#include "std/cstring.inc"
#include "std/deque.inc"
#include "std/exception.inc"
#include "std/expected.inc"
#include "std/filesystem.inc"
#include "std/format.inc"
#include "std/fstream.inc"
#include "std/functional.inc"
#include "std/future.inc"
#include "std/initializer_list.inc"
#include "std/ios.inc"
#include "std/iosfwd.inc"
#include "std/iterator.inc"
#include "std/limits.inc"
#include "std/list.inc"
#include "std/locale.inc"
#include "std/map.inc"
#include "std/memory.inc"
#include "std/memory_resource.inc"
#include "std/mutex.inc"
#include "std/new.inc"
#include "std/numeric.inc"
#include "std/optional.inc"
#include "std/print.inc"
#include "std/queue.inc"
#include "std/ranges.inc"
#include "std/ratio.inc"
#include "std/set.inc"
#include "std/shared_mutex.inc"
#include "std/source_location.inc"
#include "std/span.inc"
#include "std/stack.inc"
#include "std/stdexcept.inc"
#include "std/stop_token.inc"
#include "std/istream.inc"
#include "std/ostream.inc"
#include "std/sstream.inc"
#include "std/stdfloat.inc"
#include "std/streambuf.inc"
#include "std/string.inc"
#include "std/string_view.inc"
#include "std/system_error.inc"
#include "std/thread.inc"
#include "std/tuple.inc"
#include "std/type_traits.inc"
#include "std/typeindex.inc"
#include "std/typeinfo.inc"
#include "std/unordered_map.inc"
#include "std/unordered_set.inc"
#include "std/utility.inc"
#include "std/variant.inc"
#include "std/vector.inc"
#include "std/version.inc"


