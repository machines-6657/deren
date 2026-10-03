/**
 * @file utility/platform_dialog.cpp
 * @brief The platform half of deren::utility::ask_open_file() - a plain (non-module) translation unit.
 * @ingroup utility
 *
 * Sibling of platform_sleep.cpp and platform_path.cpp, and non-module for the same reason: <windows.h>
 * pulls in (via crtdbg) placement forms of operator new, and a module's global module fragment feeds the
 * global module - including it there made libc++'s operator new ambiguous in every TU that imported
 * `utility`. Keeping the system headers in a plain TU contains them to this file, and the module side
 * only sees the C entry point below.
 *
 * ONE ENTRY POINT, THREE BACKENDS, and a NULL ANSWER THAT IS NOT AN ERROR. The caller (chores' model
 * resolution) asks for a file before the window exists, which is why this is the platform's own dialog
 * and not an in-app browser: an overlay-based picker would need the window and a frame loop, and the
 * model is loaded before either. The return value keeps "the user said no" apart from "nobody could
 * ask": 1 = picked (out holds the path), 0 = cancelled, -1 = no backend answered (headless session, no
 * zenity/kdialog, a platform without one). The caller falls back to its default model on anything but 1,
 * so a build running unattended still starts.
 */

#include "platform_functions.hpp" // the one declaration of utility_platform_ask_open_file; checked against the definition below

#include <cstddef>
#include <cstdint> // int32_t below, for the same measured reason as platform_path.cpp's include
#include <cstdio>
#include <cstring>
#include <iterator> // std::size on the stack buffers below (same reason as platform_path.cpp)

#if defined(_WIN32)
// THE `#ifndef` IS LOAD-BEARING, not decoration: libstdc++'s `c++config.h` - reached through <cstddef> and
// <cstdio> above - already defines NOMINMAX when it pulls in a Windows header, so an unguarded `#define` here is
// a REdefinition, which the project's `-Werror` turns into a build failure (measured with GCC 16.2 on MinGW).
// libc++'s configuration does not define it, which is why the clang64 build never saw this.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
// WINDOWS.H MUST COME FIRST, and the build's clang-format step sorts includes alphabetically, which puts
// commdlg.h first ("c" < "w") and breaks the file: commdlg.h includes prsht.h, and prsht.h needs the
// Windows base types (CALLBACK, HWND, UINT) that only windows.h defines. The guard is not decoration -
// without it the formatter silently rewrites this block on the next build and the file stops compiling.
#include <windows.h>
#include <commdlg.h> // GetOpenFileNameW
// clang-format on
#elif defined(__APPLE__) || defined(__linux__)
#include <sys/wait.h>
#endif

namespace {
#if defined(_WIN32)
    /// copy a UTF-8 string into @p out as wide characters, truncating rather than failing
    void utf8_to_wide(char const* const text, wchar_t* const out, std::size_t const capacity) {
        if (out == nullptr || capacity == 0) {
            return;
        }
        out[0] = L'\0';
        if (text == nullptr || text[0] == '\0') {
            return;
        }
        int32_t const written = MultiByteToWideChar(CP_UTF8, 0, text, -1, out, static_cast<int32_t>(capacity));
        if (written <= 0) {
            out[0] = L'\0';
        }
        out[capacity - 1] = L'\0';
    }
#endif

    /// whether the title or the pattern list carries a character a shell would act on: the posix
    /// backends build a command line, and a title with a quote in it would end the argument early
    bool has_shell_metacharacter(char const* const text) {
        if (text == nullptr) {
            return false;
        }
        for (char const* p = text; *p != '\0'; ++p) {
            switch (*p) {
            case '\'':
            case '"':
            case '`':
            case '$':
            case '\\':
            case '\n':
            case '\r':
                return true;
            default:
                break;
            }
        }
        return false;
    }
} // namespace

/**
 * @brief ask the user for an existing file with the platform's own open dialog
 * @param title window/prompt title (UTF-8; the backend trims or refuses a shell-hostile one)
 * @param filter_patterns `;`-separated glob patterns, e.g. "*.glb;*.gltf"; empty or hostile = all files
 * @param out caller buffer that receives the chosen path as UTF-8
 * @param capacity its size in bytes; a NUL terminator is included in the count
 * @return 1 = picked (out holds the path), 0 = the user cancelled, -1 = no backend could ask
 * @note no allocation on purpose, like platform_path.cpp: the module side declares this as a plain C
 *       entry point, so this TU needs nothing from the module's world
 */
extern "C" int32_t utility_platform_ask_open_file(char const* title, char const* filter_patterns, char* out, std::size_t const capacity) {
    if (out == nullptr || capacity == 0) {
        return -1;
    }
    out[0] = '\0';
    if (has_shell_metacharacter(title) || has_shell_metacharacter(filter_patterns)) {
        return -1; // refuse rather than build a command line out of a string a shell would reinterpret
    }
#if defined(_WIN32)
    wchar_t wide_title[256] = {};
    wchar_t wide_patterns[512] = {};
    utf8_to_wide(title, wide_title, std::size(wide_title));
    utf8_to_wide(filter_patterns, wide_patterns, std::size(wide_patterns));
    // the filter is a double-NUL-terminated list of "label\0patterns\0" pairs, so a ';'-separated list
    // becomes one labelled entry plus an "all files" escape hatch
    wchar_t filter[1200] = {};
    wchar_t* p = filter;
    if (wide_patterns[0] != L'\0') {
        static constexpr wchar_t prefix[] = L"Model files (";
        for (wchar_t const* s = prefix; *s != L'\0'; ++s) {
            *p++ = *s;
        }
        for (wchar_t const* s = wide_patterns; *s != L'\0'; ++s) {
            *p++ = *s;
        }
        *p++ = L')';
        *p++ = L'\0';
        for (wchar_t const* s = wide_patterns; *s != L'\0'; ++s) {
            *p++ = *s;
        }
        *p++ = L'\0';
    }
    static constexpr wchar_t all_label[] = L"All files (*.*)";
    static constexpr wchar_t all_pattern[] = L"*.*";
    for (wchar_t const* s = all_label; *s != L'\0'; ++s) {
        *p++ = *s;
    }
    *p++ = L'\0';
    for (wchar_t const* s = all_pattern; *s != L'\0'; ++s) {
        *p++ = *s;
    }
    *p++ = L'\0';
    *p = L'\0'; // the list's own double NUL

    wchar_t file[32768] = {}; // long enough for a \\?\ path; the API reports truncation itself
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr; // no window exists yet: the dialog is the process's first UI
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = wide_title[0] != L'\0' ? wide_title : nullptr;
    // OFN_NOCHANGEDIR matters: the process's working directory is what locates shaders/ and the default
    // model, and the common dialog changes it unless told not to.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn) == FALSE) {
        // a cancel leaves the extended error at zero; anything else is a failure to show the dialog
        return CommDlgExtendedError() == 0 ? 0 : -1;
    }
    int32_t const written = WideCharToMultiByte(CP_UTF8, 0, file, -1, out, static_cast<int32_t>(capacity), nullptr, nullptr);
    if (written <= 0) {
        out[0] = '\0';
        return -1;
    }
    out[capacity - 1] = '\0';
    return 1;
#elif defined(__APPLE__) || defined(__linux__)
    // The posix backends run the desktop's own picker through the shell: zenity (GNOME and most other
    // desktops), then kdialog (KDE), and osascript on macOS. Their exit status is what says whether the
    // command was missing (127) or the user cancelled (1), which is why the loop stops on a cancel
    // instead of falling through to the next backend.
    char const* const safe_title = title != nullptr && title[0] != '\0' ? title : "choose a file";
    char const* const all_files = "*";
    char const* const patterns = filter_patterns != nullptr && filter_patterns[0] != '\0' ? filter_patterns : all_files;
    char spaced[512] = {}; // zenity's --file-filter wants the patterns space separated
    {
        std::size_t n = 0;
        for (char const* s = patterns; *s != '\0' && n + 1 < sizeof(spaced); ++s) {
            spaced[n++] = *s == ';' ? ' ' : *s;
        }
        spaced[n] = '\0';
    }
#if defined(__APPLE__)
    char command[1024] = {};
    std::snprintf(command, sizeof(command),
                  "osascript -e 'POSIX path of (choose file with prompt \"%s\")' 2>/dev/null", safe_title);
    char const* const backends[] = {command};
#else
    char zenity[1024] = {};
    char kdialog[1024] = {};
    std::snprintf(zenity, sizeof(zenity),
                  "zenity --file-selection --title='%s' --file-filter='Models | %s' 2>/dev/null", safe_title, spaced);
    std::snprintf(kdialog, sizeof(kdialog),
                  "kdialog --getopenfilename . '%s' --title '%s' 2>/dev/null", spaced, safe_title);
    char const* const backends[] = {zenity, kdialog};
#endif
    for (char const* const backend : backends) {
        std::FILE* const pipe = popen(backend, "r");
        if (pipe == nullptr) {
            continue;
        }
        char line[32768] = {};
        char* const got = std::fgets(line, sizeof(line), pipe);
        int32_t const status = pclose(pipe);
        if (got == nullptr) {
            // no output: either the command does not exist (try the next backend) or the user cancelled,
            // and the encoded status is what tells them apart
            int32_t const code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            if (code == 1) {
                return 0; // 1 is zenity's and kdialog's "cancelled": do not ask a second time
            }
            continue;
        }
        std::size_t const length = std::strlen(line);
        if (length == 0) {
            continue;
        }
        std::size_t const trimmed = line[length - 1] == '\n' ? length - 1 : length;
        if (trimmed == 0 || trimmed + 1 > capacity) {
            continue;
        }
        std::memcpy(out, line, trimmed);
        out[trimmed] = '\0';
        return 1;
    }
    // every backend was missing (127 / ENOENT), or produced nothing usable: "nobody could ask", which the
    // caller treats like a cancel. -Werror's -Wreturn-type wants this spelled out, and the loop above only
    // covers the two working cases.
    return -1;
#else
    return -1; // no dialog backend on this platform: the caller falls back to its default model
#endif
}
