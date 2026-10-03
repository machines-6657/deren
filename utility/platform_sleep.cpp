/**
 * @file utility/platform_sleep.cpp
 * @brief The platform half of deren::utility::sleep_for_nanoseconds - a plain (non-module) translation unit.
 * @ingroup utility
 *
 * Deliberately NOT a module implementation unit: <windows.h> pulls in (via crtdbg) placement forms of
 * operator new, and a module's global module fragment feeds the global module - including it there made
 * libc++'s operator new ambiguous in every TU that imported `utility`. Keeping the system headers in a
 * plain TU contains them to this file, and the module side only sees the C entry point below.
 */

#include "platform_functions.hpp" // the one declaration of utility_platform_sleep_ns; the definition below is checked against it

#include <cstdint>

#if defined(_WIN32)
// THE `#ifndef` IS LOAD-BEARING, not decoration: libstdc++'s `c++config.h`, reached through <cstdint> above,
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
#else
#include <cerrno>
#include <time.h>
#endif

extern "C" void utility_platform_sleep_ns(std::int64_t const nanoseconds) {
    if (nanoseconds <= 0) {
        return;
    }
#if defined(_WIN32)
    // A high-resolution waitable timer (Windows 10 1803+) waits to within microseconds without calling
    // timeBeginPeriod, which would change the timer resolution for every process on the machine. One
    // timer per process, created on first use.
    static HANDLE const timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        Sleep(static_cast<DWORD>(nanoseconds / 1000000));
        return;
    }
    LARGE_INTEGER due = {};
    due.QuadPart = -nanoseconds / 100; // 100 ns units, negative = relative
    if (due.QuadPart == 0) {
        due.QuadPart = -1;
    }
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) == 0) {
        Sleep(static_cast<DWORD>(nanoseconds / 1000000));
        return;
    }
    WaitForSingleObject(timer, INFINITE);
#else
    // POSIX: a relative nanosleep, restarted on EINTR. The caller re-computes the remaining time every
    // frame, so the relative form does not drift.
    timespec request = {};
    request.tv_sec = static_cast<time_t>(nanoseconds / 1000000000);
    // `long` because `timespec::tv_nsec` IS `long` in POSIX: this is the platform API's declared type, and the
    // project's fixed-width rule stops where somebody else's interface begins.
    request.tv_nsec = static_cast<long>(nanoseconds % 1000000000);
    while (nanosleep(&request, &request) == -1 && errno == EINTR) {
    }
#endif
}