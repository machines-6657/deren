export module deren.utility.dynamic_link;

import deren.vstd;

// ============================================================================
// deren.utility.dynamic_link - load a shared library at run time and resolve C
// symbols in it.
//
// RHI plan v4 §7.4.2 (the preferred path): the loader IS a module, and the platform
// entry points are declared by hand in the module purview instead of pulling in
// <windows.h> / <dlfcn.h>. The reason is not purity, it is blast radius: the module
// purview is what every importer of this module sees, and <windows.h> arrives with
// min/max macros and a few thousand names. What this loader actually uses is seven
// functions and four constants. If it ever needs more than a hand-written window can
// carry, the C fallback of §7.4.1 (a plain TU that does include <windows.h>) is the
// escape hatch.
//
// Windows: LoadLibraryExW, never LoadLibraryA / the LoadLibrary macro (m02099).
//   - an absolute path is loaded with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
//     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS: only the DLL's own directory and the standard
//     system directories are searched, never %PATH% (DLL planting);
//   - a name without a path separator takes the classic search order instead, because
//     those two flags reject a relative path with ERROR_INVALID_PARAMETER;
//   - the path is UTF-8 and is converted with MultiByteToWideChar(CP_UTF8).
// POSIX: dlopen/dlsym/dlclose with explicit RTLD_NOW | RTLD_LOCAL. DynamicLink's
//   global GetFlag()/SetFlag() state is deliberately not copied: the flags belong to
//   the call, not to a process-global.
//
// The POSIX branch has never been built: CI is windows-latest for both jobs
// (.github/workflows/ci.yml). It is written, not measured.
// ============================================================================

#ifdef _WIN32

// ---- Win32, declared by hand (see the header comment) ----------------------
extern "C" __declspec(dllimport) void* __stdcall LoadLibraryExW(wchar_t const* file_name, void* file,
                                                                unsigned long flags);
extern "C" __declspec(dllimport) int __stdcall FreeLibrary(void* module);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* module, char const* name);
extern "C" __declspec(dllimport) unsigned long __stdcall GetLastError();
extern "C" __declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int code_page, unsigned long flags,
                                                                   char const* multi_byte, int multi_byte_length,
                                                                   wchar_t* wide, int wide_length);
extern "C" __declspec(dllimport) unsigned long __stdcall FormatMessageA(unsigned long flags, void const* source,
                                                                        unsigned long message_id,
                                                                        unsigned long language_id, char* buffer,
                                                                        unsigned long size, void* arguments);
extern "C" __declspec(dllimport) void* __stdcall LocalFree(void* memory);

namespace {
    // From winbase.h / winnls.h, copied instead of included for the reason above.
    constexpr unsigned long load_library_search_dll_load_dir = 0x00000100ul;
    constexpr unsigned long load_library_search_default_dirs = 0x00001000ul;
    constexpr unsigned int code_page_utf8 = 65001u;
    constexpr unsigned long format_message_allocate_buffer = 0x00000100ul;
    constexpr unsigned long format_message_from_system = 0x00001000ul;
    constexpr unsigned long format_message_ignore_inserts = 0x00002000ul;
} // namespace

#else

// ---- POSIX, declared by hand ----------------------------------------------
extern "C" void* dlopen(char const* file_name, int flags);
extern "C" void* dlsym(void* handle, char const* name);
extern "C" int dlclose(void* handle);
extern "C" char* dlerror();

namespace {
    constexpr int rtld_now = 2;   // resolve every symbol up front, not on first call
    constexpr int rtld_local = 0; // keep the library's symbols out of the global namespace
} // namespace

#endif

/**
 * @ingroup utility
 * @defgroup dynamic_link Dynamic Library Loader
 * @brief load a shared library at run time and resolve `extern "C"` symbols in it, without
 *        taking a link-time dependency on the library being loaded.
 *
 * This is the primitive the RHI plan's backend boundary is built on (plan §7.4): the
 * backend is a DLL next to the executable, the front end loads it by path, asks for a
 * handful of C entry points, and keeps the handle alive for as long as it uses them.
 *
 * @details
 * - Nothing here throws: every failure is a `load_error` (code + text) returned in a
 *   `std::expected`, so the caller decides what a missing backend means.
 * - The file name is completed with the platform suffix when the caller leaves it off
 *   (`.dll` on Windows, `lib` prefix + `.so` elsewhere).
 * - A `library` is move-only and closes in its destructor; `symbol()` hands out raw
 *   function pointers that die with the handle, so a `library` must outlive every call
 *   made through them.
 */
export namespace deren::utility::dynamic_link {

    /**
     * @ingroup dynamic_link
     * @brief why a load or a symbol lookup failed
     * @note
     *     - `code` is the platform error number (`GetLastError()` on Windows); it is 0
     *       where the platform only offers text (POSIX `dlerror()`)
     *     - `message` is human-readable and already stripped of trailing newlines, so it
     *       can go straight into a log line
     */
    struct load_error {
        std::int32_t code = 0;
        std::string message = {};
    };

    /**
     * @ingroup dynamic_link
     * @brief one loaded library plus symbol lookup on it
     * @note
     *     - not copyable (a copy would close the same handle twice), movable; the moved-from
     *       object is left empty and safe to destroy
     *     - the destructor closes the handle, so the library must not outlive the symbols
     *       taken from it
     *     - `detach()` is the other exit: it hands the handle back and leaves this object empty, so
     *       the destructor does NOT unload - which is what a library that must live until the process
     *       ends needs (see its own note)
     */
    class library {
    public:
        /** @brief adopt an already-loaded handle (normally done by load()); nullptr is the empty state */
        explicit library(void* opened) noexcept
            : handle(opened) {
        }

        library(library const&) = delete;
        library& operator=(library const&) = delete;

        library(library&& other) noexcept
            : handle(other.handle) {
            other.handle = nullptr;
        }

        library& operator=(library&& other) noexcept {
            if (this != &other) {
                this->close();
                this->handle = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }

        /** @brief unload the library (FreeLibrary / dlclose); safe on an empty library */
        ~library() {
            this->close();
        }

        /**
         * @brief resolve one exported `extern "C"` symbol
         * @param name the symbol's exact name (`GetProcAddress`/`dlsym` are case-sensitive)
         * @return the raw address, or the reason it is not there
         * @warning the address is only valid while this `library` is alive
         */
        [[nodiscard]] std::expected<void const*, load_error> symbol(std::string_view name) const noexcept;

        /** @brief the raw platform handle (nullptr when empty); owned by this object, never freed by the caller */
        [[nodiscard]] void* native_handle() const noexcept {
            return this->handle;
        }

        /**
         * @brief give up the platform handle WITHOUT unloading the library
         * @return the raw handle, which the CALLER now owns; nullptr on an already-empty library
         *
         * THE EXIT FOR "THE LIBRARY LIVES UNTIL THE PROCESS ENDS", which is the rule the backend's
         * boundary runs on (DYNAMIC_LINK_V2.md §13). MEASURED: unloading the backend after a context had
         * been built and torn down NEVER RETURNED - the destructor called `FreeLibrary` and the process
         * sat at 0.45 s of CPU for 80+ s with ten threads and a working set that never moved, while the
         * same program with the unload removed exited immediately. The wait is on the DLL's own detach
         * path (GLFW is initialised inside it and this backend terminates GLFW on no path -
         * `core.constructor.cppm` says so), so a product that unloads as it exits deadlocks where
         * nothing is watching: the window has already closed.
         *
         * AFTER DETACH THIS OBJECT IS EMPTY: `native_handle()` answers nullptr and `symbol()` refuses
         * with "the library is not loaded", so detach is the last call a caller makes on it. Whoever
         * holds the returned handle owns the decision to free it, and the product's answer is that
         * nobody does.
         */
        [[nodiscard]] void* detach() noexcept {
            void* const detached = this->handle;
            this->handle = nullptr;
            return detached;
        }

    private:
        void close() noexcept;

        void* handle = nullptr;
    };

    /**
     * @ingroup dynamic_link
     * @brief load a shared library
     * @param file_name path to the library, UTF-8; the platform suffix is appended when it is absent
     * @return the loaded library, or the reason the load failed
     * @note an absolute path is searched narrowly on Windows (the library's own directory plus the
     *       system directories); a name without a separator is a relative name and takes the
     *       classic search order
     */
    [[nodiscard]] std::expected<library, load_error> load(std::string_view file_name) noexcept;
} // namespace deren::utility::dynamic_link

namespace {

    /** @brief append the platform suffix when @p file_name does not already carry one */
    std::string complete_file_name(std::string_view file_name) {
#ifdef _WIN32
        constexpr std::string_view suffix = ".dll";
        if (file_name.ends_with(suffix)) {
            return std::string{file_name};
        }
        return std::string{file_name} + std::string{suffix};
#else
        if (file_name.ends_with(".so") || file_name.find(".so.") != std::string_view::npos) {
            return std::string{file_name};
        }
        std::string completed{};
        if (file_name.find('/') == std::string_view::npos && !file_name.starts_with("lib")) {
            completed = "lib";
        }
        completed += std::string{file_name};
        completed += ".so";
        return completed;
#endif
    }

#ifdef _WIN32
    /** @brief UTF-8 to UTF-16, with room for the terminating NUL LoadLibraryExW needs */
    bool to_wide(std::string_view utf8, std::vector<wchar_t>& wide) {
        if (utf8.empty()) {
            return false;
        }
        int const length =
            MultiByteToWideChar(code_page_utf8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (length <= 0) {
            return false;
        }
        wide.assign(static_cast<std::size_t>(length) + 1, L'\0');
        return MultiByteToWideChar(code_page_utf8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                                   length) > 0;
    }

    /** @brief true for "C:\x", "C:/x" and "\\server\share\x"; false for a name without a separator */
    bool is_absolute_path(std::string_view file_name) {
        if (file_name.size() >= 2 && file_name[1] == ':') {
            return true;
        }
        return file_name.size() >= 2 && (file_name[0] == '\\' || file_name[0] == '/') &&
               (file_name[1] == '\\' || file_name[1] == '/');
    }

    /** @brief GetLastError()'s text, trailing newlines removed; empty when the system has no text */
    std::string system_message(unsigned long code) {
        char* buffer = nullptr;
        unsigned long const length =
            FormatMessageA(format_message_allocate_buffer | format_message_from_system | format_message_ignore_inserts,
                           nullptr, code, 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
        if (length == 0 || buffer == nullptr) {
            return {};
        }
        std::string message{buffer, length};
        LocalFree(buffer);
        while (!message.empty() && (message.back() == '\r' || message.back() == '\n')) {
            message.pop_back();
        }
        return message;
    }

    /** @brief a message for a platform error code that has no text of its own */
    std::string failed_message(std::string_view call, unsigned long code) {
        return std::format("{} failed (GetLastError={})", call, code);
    }
#else
    /** @brief dlerror()'s pending text, consumed by the call */
    std::string last_dl_error() {
        char const* const text = dlerror();
        return text != nullptr ? std::string{text} : std::string{"dlopen/dlsym failed"};
    }
#endif
} // namespace

namespace deren::utility::dynamic_link {

    std::expected<void const*, load_error> library::symbol(std::string_view name) const noexcept {
        if (this->handle == nullptr) {
            return std::unexpected(load_error{0, "the library is not loaded"});
        }
        if (name.empty() || name.contains('\0')) {
            return std::unexpected(load_error{0, "the symbol name is empty or contains a NUL byte"});
        }
        std::string const terminated{name}; // GetProcAddress/dlsym want a NUL-terminated name
#ifdef _WIN32
        void* const address = GetProcAddress(this->handle, terminated.c_str());
        if (address == nullptr) {
            unsigned long const code = GetLastError();
            return std::unexpected(load_error{static_cast<std::int32_t>(code),
                                              std::format("the library has no symbol '{}' ({})", name,
                                                          failed_message("GetProcAddress", code))});
        }
#else
        static_cast<void>(dlerror()); // clear the pending error so the next call is unambiguous
        void* const address = dlsym(this->handle, terminated.c_str());
        if (address == nullptr) {
            return std::unexpected(
                load_error{0, std::format("the library has no symbol '{}' ({})", name, last_dl_error())});
        }
#endif
        return address;
    }

    void library::close() noexcept {
        if (this->handle == nullptr) {
            return;
        }
#ifdef _WIN32
        FreeLibrary(this->handle);
#else
        dlclose(this->handle);
#endif
        this->handle = nullptr;
    }

    std::expected<library, load_error> load(std::string_view file_name) noexcept {
        std::string const path = complete_file_name(file_name);
        if (path.empty()) {
            return std::unexpected(load_error{0, "the file name is empty"});
        }
#ifdef _WIN32
        std::vector<wchar_t> wide;
        if (!to_wide(path, wide)) {
            return std::unexpected(
                load_error{0, std::format("'{}' cannot be converted to a UTF-16 path", path)});
        }
        unsigned long const flags =
            is_absolute_path(path) ? (load_library_search_dll_load_dir | load_library_search_default_dirs) : 0ul;
        void* const handle = LoadLibraryExW(wide.data(), nullptr, flags);
        if (handle == nullptr) {
            unsigned long const code = GetLastError();
            std::string message = system_message(code);
            if (message.empty()) {
                message = failed_message("LoadLibraryExW", code);
            }
            return std::unexpected(load_error{static_cast<std::int32_t>(code), std::move(message)});
        }
#else
        void* const handle = dlopen(path.c_str(), rtld_now | rtld_local);
        if (handle == nullptr) {
            return std::unexpected(load_error{0, last_dl_error()});
        }
#endif
        return library{handle};
    }
} // namespace deren::utility::dynamic_link
