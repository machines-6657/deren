#include "backend_platform.hpp"

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off: bcrypt.h requires Windows types to be declared first.
#include <windows.h>
#include <bcrypt.h>
// clang-format on
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace {
    std::wstring wide(char const* text) {
        int const size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
        if (size <= 0)
            return {};
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result.data(), size) == 0)
            return {};
        result.pop_back();
        return result;
    }
    int utf8(wchar_t const* text, char* out, std::size_t capacity) {
        return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out,
                                   static_cast<int>(capacity), nullptr, nullptr) > 0
                   ? 1
                   : 0;
    }
    void failure(char* out, std::size_t capacity, char const* operation, unsigned long code) {
        if (out && capacity)
            std::snprintf(out, capacity, "%s failed (code=%lu)", operation, code);
    }
    std::wstring canonical(char const* path) {
        std::wstring const name = wide(path);
        if (name.empty())
            return {};
        HANDLE const file = CreateFileW(name.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {};
        std::vector<wchar_t> buffer(32768);
        DWORD const size = GetFinalPathNameByHandleW(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                     FILE_NAME_NORMALIZED);
        CloseHandle(file);
        return size && size < buffer.size() ? std::wstring(buffer.data(), size) : std::wstring{};
    }
} // namespace

extern "C" int deren_backend_executable_directory(char* out, std::size_t capacity) {
    std::vector<wchar_t> path(32768);
    DWORD const size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size || size >= path.size())
        return 0;
    for (DWORD i = size; i > 0; --i) {
        if (path[i - 1] == L'\\' || path[i - 1] == L'/') {
            path[i] = L'\0';
            return utf8(path.data(), out, capacity);
        }
    }
    return 0;
}

extern "C" int deren_backend_glfw_module(char* path, std::size_t capacity, int* major, int* minor,
                                         int* revision, char* error, std::size_t error_capacity) {
    HMODULE module = nullptr;
    // 固定实际导入函数所属模块，包括后续校验失败路径；本产品不支持运行期卸载 GLFW。
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<wchar_t const*>(&glfwGetVersion), &module)) {
        failure(error, error_capacity, "GetModuleHandleExW(GLFW)", GetLastError());
        return 0;
    }
    std::vector<wchar_t> name(32768);
    DWORD const size = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size()));
    if (!size || size >= name.size() || !utf8(name.data(), path, capacity)) {
        failure(error, error_capacity, "GetModuleFileNameW(GLFW)", GetLastError());
        return 0;
    }
    glfwGetVersion(major, minor, revision); // GLFW permits this query before glfwInit.
    return 1;
}

extern "C" int deren_backend_same_file(char const* actual, char const* expected) {
    auto const a = canonical(actual);
    auto const b = canonical(expected);
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0 ? 1 : 0;
}

extern "C" void* deren_backend_open_sha256(char const* path, char* hex, char* error,
                                           std::size_t error_capacity) {
    std::wstring const name = wide(path);
    HANDLE const file = name.empty() ? INVALID_HANDLE_VALUE : CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        failure(error, error_capacity, "open/hash release artifact", GetLastError());
        return nullptr;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0, written = 0;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status >= 0)
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &written, 0);
    std::vector<unsigned char> object(object_size);
    if (status >= 0)
        status = BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0);
    std::vector<unsigned char> block(65536);
    bool read_ok = status >= 0;
    while (read_ok) {
        if (!ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &written, nullptr)) {
            read_ok = false;
            failure(error, error_capacity, "ReadFile(SHA256)", GetLastError());
            break;
        }
        if (!written)
            break;
        status = BCryptHashData(hash, block.data(), written, 0);
        if (status < 0)
            read_ok = false;
    }
    unsigned char digest[32]{};
    if (read_ok)
        status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!read_ok || status < 0) {
        if (status < 0)
            failure(error, error_capacity, "BCrypt SHA256", static_cast<unsigned long>(status));
        CloseHandle(file);
        return nullptr;
    }
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t i = 0; i < sizeof(digest); ++i) {
        hex[2 * i] = digits[digest[i] >> 4];
        hex[2 * i + 1] = digits[digest[i] & 15];
    }
    hex[64] = '\0';
    // 读锁一直持有至 LoadLibrary 完成，校验与映射期间禁止文件写入/替换。
    return file;
}

extern "C" void deren_backend_close_file(void* handle) {
    if (handle)
        CloseHandle(handle);
}
