/**
 * @file utility/platform_path.cpp
 * @brief The platform half of deren::utility::executable_directory() - a plain (non-module) translation unit.
 * @ingroup utility
 *
 * Sibling of platform_sleep.cpp, and non-module for the same reason: <windows.h> pulls in (via
 * crtdbg) placement forms of operator new, and a module's global module fragment feeds the global
 * module - including it there made libc++'s operator new ambiguous in every TU that imported
 * `utility`. Keeping the system headers in a plain TU contains them to this file, and the module side
 * only sees the C entry point below.
 *
 * The caller (chores' shader lookup) uses this to prefer the shaders/ directory sitting next to the
 * executable, which is what a build writes. Deriving the path from the running binary rather than from
 * the working directory is what makes "the shaders you run are the ones this build compiled" true
 * without depending on where the process happens to have been started.
 */

#include <cstddef>
#include <cstdint>                // int32_t below: stated HERE rather than borrowed transitively - libc++'s <cstddef> happens to
                                  // provide it and libstdc++'s does not, which is a build that fails on one toolchain only
#include "platform_functions.hpp" // the one declaration of utility_platform_executable_directory; checked against the definition below

#include <iterator>

#if defined(_WIN32)
// THE `#ifndef` IS LOAD-BEARING, not decoration: libstdc++'s `c++config.h`, reached through <cstddef> above,
// already defines NOMINMAX when it pulls in a Windows header - so an unguarded `#define` here is a REdefinition,
// which the project's `-Werror` turns into a build failure (measured with GCC 16.2 on MinGW; libc++'s
// configuration does not define it, which is why the clang64 build never saw this).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

/**
 * @brief write the running executable's directory into @p out as UTF-8
 * @param out caller buffer
 * @param capacity its size in bytes; a NUL terminator is included in the count
 * @return the number of bytes written excluding the terminator, or -1 when unavailable
 * @note no allocation on purpose: the module side declares this as a plain C entry point, so this TU
 *       needs nothing from the module's world
 */
extern "C" int32_t utility_platform_executable_directory(char* out, std::size_t const capacity) {
    if (out == nullptr || capacity == 0) {
        return -1;
    }
    out[0] = '\0';
#if defined(_WIN32)
    wchar_t wide[32768] = {}; // long enough for a \\?\ path; the API reports truncation itself
    DWORD const written = GetModuleFileNameW(nullptr, wide, static_cast<DWORD>(std::size(wide)));
    if (written == 0 || written >= std::size(wide)) {
        return -1;
    }
    // strip the file name, keeping the directory (including its trailing separator)
    for (DWORD i = written; i > 0; --i) {
        if (wide[i - 1] == L'\\' || wide[i - 1] == L'/') {
            return WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int32_t>(i), out, static_cast<int32_t>(capacity), nullptr, nullptr);
        }
    }
    return -1; // no separator: not a filesystem path we can use
#elif defined(__linux__)
    // /proc/self/exe is a symlink to the binary, and readlink does not append a terminator
    ssize_t const written = readlink("/proc/self/exe", out, capacity - 1);
    if (written <= 0 || static_cast<std::size_t>(written) >= capacity - 1) {
        return -1;
    }
    out[written] = '\0';
    for (ssize_t i = written; i > 0; --i) {
        if (out[i - 1] == '/') {
            out[i] = '\0'; // keep the trailing separator
            return static_cast<int32_t>(i);
        }
    }
    return -1;
#else
    (void)out;
    (void)capacity;
    return -1; // macOS and others: the caller falls back to walking up from the cwd
#endif
}
