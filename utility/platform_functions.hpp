#pragma once
/**
 * @file utility/platform_functions.hpp
 * @brief The C entry points of utility's three plain (non-module) platform translation units.
 * @ingroup utility
 *
 * WHY THIS IS A HEADER AND NOT A DECLARATION IN utility.cpp's GLOBAL MODULE FRAGMENT. That fragment is
 * where the three used to be declared, and that is legal C++: clang accepts it, and it is what the
 * shipped clang64 build has always compiled. MSVC does not - C5202 ("a global module fragment can only
 * contain preprocessing directives") is a /W4 warning and the MSVC branch builds with /WX, so the same
 * three declarations became fatal errors there (measured: utility/utility.cpp(6), fatal C2220 in
 * build-release-clang64/msvc/dialect/dialect-build-deren.log). A header is the portable spelling of
 * "a preprocessor directive carries the declaration", so the declarations moved here and every file
 * that needs them includes this one header instead of restating them.
 *
 * WHY THE FUNCTIONS ARE NOT MODULES AT ALL is documented in each definition's own file comment:
 * <windows.h> pulls in (via crtdbg) placement forms of operator new, and a module's global module
 * fragment feeds the global module - including it there made libc++'s operator new ambiguous in every
 * TU that imported `utility`. The module side therefore only sees the C entry points declared below.
 *
 * The three definitions include this header too, so a signature changed on either side cannot drift.
 */

#include <cstddef>
#include <cstdint>

// Sleep for a relative duration; a non-positive request returns immediately. Implemented in
// platform_sleep.cpp. `deren::utility::sleep_for_nanoseconds` in utility.cppm is the module-side wrapper.
extern "C" void utility_platform_sleep_ns(std::int64_t nanoseconds);

// The running executable's directory, written into the caller's buffer; -1 = unavailable.
// Implemented in platform_path.cpp.
extern "C" int32_t utility_platform_executable_directory(char* out, std::size_t capacity);

// File dialog; the chosen path is written into the caller's buffer. 1 = picked, 0 = cancelled,
// -1 = no backend could ask. Implemented in platform_dialog.cpp.
extern "C" int32_t utility_platform_ask_open_file(char const* title, char const* filter_patterns, char* out, std::size_t capacity);
