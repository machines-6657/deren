#pragma once
// Tiny CHECK harness for the headless unit tests - deliberately no external test
// framework (the repo stays dependency-free). Every failed CHECK prints file:line
// and bumps the failure counter; the test's main returns 1 when anything failed so
// CTest sees the test as failed. One test executable = one translation unit.
//
// TESTS ARE PLAIN TUs, and this harness formats its own output (`std::format` into one `std::fwrite`) rather
// than calling the project's `deren::utility::println`, WHICH IS A MEASURED WORKAROUND FOR A CLANG BUG rather than a
// layering preference:
//
//   * `<print>` is not an option at all: MinGW's libstdc++ 16.2 declares it without the terminal-writing half
//     of its implementation (`std::__open_terminal` / `std::__write_to_terminal` are absent from
//     `libstdc++.a`), so a `std::println` call site fails to LINK there - see utility.cppm's print family.
//   * and CALLING THAT FAMILY FROM HERE CRASHES CLANG 22.1.8: with an inline function in this header calling
//     the module's variadic template, `test_shadow_fit.cpp` and `test_animation.cpp` die in code generation
//     with "clang frontend command failed due to signal" inside `EmitBuiltinNewDeleteCall`
//     (`std::__libcpp_allocate`). The minimal reproduction of that shape - a module exporting such a template,
//     a header with an inline function calling it, a TU that imports and then includes - compiles cleanly on
//     its own, so the trigger is the combination of the two rather than either half.
//
// A harness is not worth a compiler bug, so it formats here: `std::format` is compiler and library neutral, and
// every test TU stays a plain TU that imports only the module under test. The engine's own printing goes
// through `deren::utility::print`/`println` as it should.

#include <cstddef>
#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace deren::vk_test {
    [[nodiscard]] inline int32_t& failures() {
        static int32_t count = 0;
        return count;
    }
    [[nodiscard]] inline int32_t& checks() {
        static int32_t count = 0;
        return count;
    }

    /// one formatted line on stdout; the format string is checked at compile time exactly as `std::format`'s is
    template <typename... Args>
    inline void write_line(std::format_string<Args...> fmt, Args&&... args) {
        std::string text = std::format(fmt, std::forward<Args>(args)...);
        text.push_back('\n');
        static_cast<void>(std::fwrite(text.data(), 1, text.size(), stdout));
    }

    inline void report(char const* expression, char const* file, int32_t const line, char const* message) {
        ++failures();
        if (message != nullptr) {
            write_line("FAIL {}:{}: {}  ({})", file, line, expression, message);
        } else {
            write_line("FAIL {}:{}: {}", file, line, expression);
        }
    }

    /** @brief print the summary and return the process exit code (0 = all checks passed) */
    inline int32_t finish(char const* test_name) {
        int32_t const failed = failures();
        write_line("[{}] {} checks, {} failed -> {}", test_name, checks(), failed, failed == 0 ? "PASS" : "FAIL");
        return failed == 0 ? 0 : 1;
    }
} // namespace deren::vk_test

// A check's condition is materialized in a local and the `if` tests THAT, rather than testing the expression inline
// as `if (!(cond))`: MSVC /W4 reports C4127 ("conditional expression is constant") for a check over a constant
// pinned at compile time - which the math tests do on purpose, e.g. `CHECK_MSG(k_desaturation_luma_r ==
// 0.21267299354076385f, ...)` - and the `if constexpr` it suggests cannot stand in for one here, because this
// harness also carries runtime conditions (`CHECK(file.is_open())`) and some pinned constants are not constant
// expressions at all (a `float const` computed through a lambda). The check itself is unchanged: the condition is
// still evaluated exactly once, the report text is the same, and so is the process exit code.
#define CHECK(cond)                                                \
    do {                                                           \
        ++::deren::vk_test::checks();                                     \
        bool const check_failed = !(cond);                         \
        if (check_failed) {                                        \
            ::deren::vk_test::report(#cond, __FILE__, __LINE__, nullptr); \
        }                                                          \
    } while (false)

#define CHECK_MSG(cond, message)                                     \
    do {                                                             \
        ++::deren::vk_test::checks();                                       \
        bool const check_failed = !(cond);                           \
        if (check_failed) {                                          \
            ::deren::vk_test::report(#cond, __FILE__, __LINE__, (message)); \
        }                                                            \
    } while (false)
