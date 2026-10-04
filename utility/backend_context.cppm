module;
#include "backend_platform.hpp"
#include "deren_backend_identity.hpp"

export module deren.utility.backend_context;

import deren.vstd;
import deren.promise.rhi;
import deren.utility.dynamic_link;

export namespace deren::utility {
    enum class backend_load_stage { package_path,
                                    manifest,
                                    compatibility,
                                    glfw_runtime,
                                    artifact_hash,
                                    library_load,
                                    entry_points,
                                    actual_abi,
                                    core_creation };
    struct backend_load_error {
        backend_load_stage stage{};
        std::string message;
    };
    class backend_context;
    using backend_token = std::shared_ptr<backend_context>;
    [[nodiscard]] std::expected<void, backend_load_error> verify_shared_glfw_runtime();
    [[nodiscard]] std::expected<backend_token, backend_load_error>
    load_backend(promise::rhi::create_info const& desc);

    // 主机拥有此包装器，DLL 内创建的 core 只交回 DLL 的删除器。
    // 资源包装器持有 token，析构先 release 资源，再放开 token。
    class backend_context final {
    public:
        backend_context(backend_context const&) = delete;
        backend_context& operator=(backend_context const&) = delete;
        ~backend_context() {
            if (core_)
                destroy_(core_);
        }
        [[nodiscard]] promise::rhi::api_core& core() const noexcept {
            return *core_;
        }
        [[nodiscard]] std::string_view build_id() const noexcept {
            return build_id_;
        }

    private:
        using destroy_function = void (*)(promise::rhi::api_core*);
        backend_context(promise::rhi::api_core* core, destroy_function destroy, std::string build)
            : core_(core)
            , destroy_(destroy)
            , build_id_(std::move(build)) {
        }
        friend std::expected<backend_token, backend_load_error>
        load_backend(promise::rhi::create_info const&);
        promise::rhi::api_core* core_ = nullptr;
        destroy_function destroy_ = nullptr;
        std::string build_id_;
    };
} // namespace deren::utility

namespace {
    using deren::utility::backend_load_error;
    using deren::utility::backend_load_stage;
    namespace identity = deren::backend_identity;

    backend_load_error fail(backend_load_stage stage, std::string message) {
        return {stage, std::move(message)};
    }
    std::string utf8_path(std::filesystem::path const& path) {
        auto const text = path.u8string();
        return {reinterpret_cast<char const*>(text.data()), text.size()};
    }
    std::expected<std::filesystem::path, backend_load_error> executable_dir() {
        std::array<char, 131072> buffer{};
        if (!deren_backend_executable_directory(buffer.data(), buffer.size()))
            return std::unexpected(fail(backend_load_stage::package_path,
                                        "Cannot determine the executable directory; refusing current-directory/PATH fallback"));
        auto path = std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(buffer.data())));
        if (!path.is_absolute())
            return std::unexpected(fail(backend_load_stage::package_path,
                                        "The executable directory is not absolute"));
        return path;
    }
    bool sha256_text(std::string_view value) {
        return value.size() == 64 && std::ranges::all_of(value, [](char c) {
                   return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
               });
    }
    bool component(std::string_view value) {
        return !value.empty() && value.size() <= 128 && value != "." && value != ".." &&
               std::ranges::all_of(value, [](char c) {
                   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-';
               });
    }

    // The generated release schema is deliberately flat. No third-party JSON parser is installed.
    // Strict types, duplicate keys, trailing data, controls and oversized files are rejected.
    struct manifest_value {
        std::string text;
        bool number = false;
    };
    class manifest_reader {
    public:
        explicit manifest_reader(std::string_view input)
            : input_(input) {
        }
        bool read(std::map<std::string, manifest_value>& result) {
            if (!consume('{'))
                return false;
            if (consume('}'))
                return finish();
            do {
                std::string key;
                if (!string(key) || !consume(':'))
                    return false;
                whitespace();
                manifest_value value;
                if (peek() == '"') {
                    if (!string(value.text))
                        return false;
                } else {
                    value.number = true;
                    auto const start = position_;
                    while (peek() >= '0' && peek() <= '9')
                        ++position_;
                    if (position_ == start)
                        return false;
                    value.text = input_.substr(start, position_ - start);
                    if (value.text.size() > 1 && value.text[0] == '0')
                        return false;
                }
                if (!result.emplace(std::move(key), std::move(value)).second)
                    return false;
                if (consume('}'))
                    return finish();
            } while (consume(','));
            return false;
        }

    private:
        char peek() const {
            return position_ < input_.size() ? input_[position_] : '\0';
        }
        void whitespace() {
            while (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n')
                ++position_;
        }
        bool consume(char c) {
            whitespace();
            if (peek() != c)
                return false;
            ++position_;
            return true;
        }
        bool finish() {
            whitespace();
            return position_ == input_.size();
        }
        bool string(std::string& out) {
            if (!consume('"'))
                return false;
            while (position_ < input_.size()) {
                char c = input_[position_++];
                if (c == '"')
                    return true;
                if (static_cast<unsigned char>(c) < 32)
                    return false;
                if (c == '\\') {
                    if (position_ == input_.size())
                        return false;
                    c = input_[position_++];
                    switch (c) {
                    case '"':
                    case '\\':
                    case '/':
                        break;
                    case 'b':
                        c = '\b';
                        break;
                    case 'f':
                        c = '\f';
                        break;
                    case 'n':
                        c = '\n';
                        break;
                    case 'r':
                        c = '\r';
                        break;
                    case 't':
                        c = '\t';
                        break;
                    default:
                        return false;
                    }
                }
                if (c == '\0')
                    return false;
                out.push_back(c);
            }
            return false;
        }
        std::string_view input_;
        std::size_t position_ = 0;
    };
    std::expected<std::map<std::string, manifest_value>, backend_load_error>
    read_manifest(std::filesystem::path const& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return std::unexpected(fail(backend_load_stage::manifest,
                                        std::format("Missing/unreadable backend manifest: {}", utf8_path(path))));
        std::string contents;
        std::array<char, 4096> block{};
        while (input) {
            input.read(block.data(), static_cast<std::streamsize>(block.size()));
            contents.append(block.data(), static_cast<std::size_t>(input.gcount()));
            if (contents.size() > 65536)
                return std::unexpected(fail(backend_load_stage::manifest,
                                            "Backend manifest exceeds the 64 KiB release limit"));
        }
        std::map<std::string, manifest_value> result;
        if (!input.eof() || !manifest_reader(contents).read(result))
            return std::unexpected(fail(backend_load_stage::manifest,
                                        "Malformed backend manifest (flat JSON, unique keys and typed values required)"));
        return result;
    }
    struct file_lock {
        void* handle = nullptr;
        ~file_lock() {
            deren_backend_close_file(handle);
        }
    };
    std::expected<void, backend_load_error> check_glfw(std::filesystem::path const& directory) {
        std::array<char, 131072> actual{};
        std::array<char, 1024> error{};
        int major = 0, minor = 0, revision = 0;
        if (!deren_backend_glfw_module(actual.data(), actual.size(), &major, &minor, &revision,
                                       error.data(), error.size()))
            return std::unexpected(fail(backend_load_stage::glfw_runtime, error.data()));
        std::string const expected_path = utf8_path(directory / "glfw3.dll");
        if (!deren_backend_same_file(actual.data(), expected_path.c_str()))
            return std::unexpected(fail(backend_load_stage::glfw_runtime,
                                        std::format("Imported GLFW path mismatch: actual='{}', required='{}'", actual.data(), expected_path)));
        auto const version = std::format("{}.{}.{}", major, minor, revision);
        if (version != identity::glfw_version)
            return std::unexpected(fail(backend_load_stage::glfw_runtime,
                                        std::format("Imported GLFW version mismatch: actual={}, required={}", version, identity::glfw_version)));
        std::array<char, 65> digest{};
        file_lock lock{deren_backend_open_sha256(actual.data(), digest.data(), error.data(), error.size())};
        if (!lock.handle)
            return std::unexpected(fail(backend_load_stage::glfw_runtime, error.data()));
        if (std::string_view(digest.data()) != identity::glfw_sha256)
            return std::unexpected(fail(backend_load_stage::glfw_runtime,
                                        std::format("Imported GLFW SHA256 mismatch: actual={}, required={}", digest.data(), identity::glfw_sha256)));
        return {};
    }
} // namespace

namespace deren::utility {
    std::expected<void, backend_load_error> verify_shared_glfw_runtime() {
        auto directory = executable_dir();
        if (!directory)
            return std::unexpected(directory.error());
        return check_glfw(*directory);
    }

    std::expected<backend_token, backend_load_error> load_backend(promise::rhi::create_info const& desc) {
        static_assert(identity::contract_abi == promise::rhi::abi_version,
                      "Generated release identity must match the compiled contract");
        auto directory = executable_dir();
        if (!directory)
            return std::unexpected(directory.error());
        auto selection = read_manifest(*directory / "backend.selection.json");
        if (!selection)
            return std::unexpected(selection.error());
        auto const selected = selection->find("backend_build_id");
        auto const selection_schema = selection->find("selection_schema");
        if (selection_schema == selection->end() || !selection_schema->second.number ||
            selection_schema->second.text != "1" || selected == selection->end() || selected->second.number ||
            !component(selected->second.text))
            return std::unexpected(fail(backend_load_stage::package_path,
                                        "Invalid backend.selection.json: schema 1 and a single safe backend_build_id are required"));
        std::string const selected_build_id = selected->second.text;
        auto const backend_dir = *directory / "backends" / "vulkan" / selected_build_id;
        auto manifest = read_manifest(backend_dir / "backend.manifest.json");
        if (!manifest)
            return std::unexpected(manifest.error());
        auto require = [&](std::string const& key, std::string_view expected, bool number = false)
            -> std::expected<void, backend_load_error> {
            auto const value = manifest->find(key);
            if (value == manifest->end() || value->second.number != number || value->second.text != expected)
                return std::unexpected(fail(backend_load_stage::compatibility,
                                            std::format("Backend manifest '{}' mismatch: required='{}', actual='{}'", key, expected,
                                                        value == manifest->end() ? "<missing>" : value->second.text)));
            return {};
        };
        for (auto const& pair : std::array<std::pair<std::string_view, std::string_view>, 5>{{{"compat_id", identity::compat_id}, {"shader_interface_id", identity::shader_interface_id}, {"backend_build_id", selected_build_id}, {"glfw_version", identity::glfw_version}, {"glfw_sha256", identity::glfw_sha256}}}) {
            auto check = require(std::string(pair.first), pair.second);
            if (!check)
                return std::unexpected(check.error());
        }
        auto schema = require("manifest_schema", "1", true);
        if (!schema)
            return std::unexpected(schema.error());
        auto abi = require("contract_abi", std::to_string(promise::rhi::abi_version), true);
        if (!abi)
            return std::unexpected(abi.error());
        auto const hash = manifest->find("backend_sha256");
        if (hash == manifest->end() || hash->second.number || !sha256_text(hash->second.text))
            return std::unexpected(fail(backend_load_stage::manifest, "Missing/invalid backend_sha256"));
        auto glfw = check_glfw(*directory);
        if (!glfw)
            return std::unexpected(glfw.error());
        std::string const backend_path = utf8_path(backend_dir / "deren_vulkan.dll");
        std::array<char, 65> digest{};
        std::array<char, 1024> platform_error{};
        file_lock lock{deren_backend_open_sha256(backend_path.c_str(), digest.data(), platform_error.data(),
                                                 platform_error.size())};
        if (!lock.handle)
            return std::unexpected(fail(backend_load_stage::artifact_hash, platform_error.data()));
        if (hash->second.text != digest.data())
            return std::unexpected(fail(backend_load_stage::artifact_hash,
                                        std::format("Backend DLL SHA256 mismatch: required={}, actual={}", hash->second.text, digest.data())));
        auto library = dynamic_link::load(backend_path);
        if (!library)
            return std::unexpected(fail(backend_load_stage::library_load,
                                        std::format("Cannot load '{}': {}", backend_path, library.error().message)));
        // detach 在任何 lookup/ABI/factory 失败前建立，所有成功加载的 DLL 都保持至进程结束。
        // Lookup still uses a non-owning temporary; its scope guard detaches on every return path.
        struct pin_on_exit {
            dynamic_link::library& library;
            ~pin_on_exit() {
                static_cast<void>(library.detach());
            }
        } pin{*library};
        auto version_symbol = library->symbol("deren_abi_version");
        auto make_symbol = library->symbol("deren_make_api_core");
        auto destroy_symbol = library->symbol("deren_destroy_api_core");
        if (!version_symbol || !make_symbol || !destroy_symbol) {
            auto const& error = !version_symbol ? version_symbol.error() : !make_symbol ? make_symbol.error()
                                                                                        : destroy_symbol.error();
            return std::unexpected(fail(backend_load_stage::entry_points, error.message));
        }
        using version_fn = std::uint32_t (*)();
        using make_fn = promise::rhi::api_core* (*)(std::uint32_t, promise::rhi::create_info const*, promise::rhi::error*);
        auto version = reinterpret_cast<version_fn>(const_cast<void*>(*version_symbol));
        auto make = reinterpret_cast<make_fn>(const_cast<void*>(*make_symbol));
        auto destroy = reinterpret_cast<backend_context::destroy_function>(const_cast<void*>(*destroy_symbol));
        auto const actual_abi = version();
        if (actual_abi != promise::rhi::abi_version)
            return std::unexpected(fail(backend_load_stage::actual_abi,
                                        std::format("Loaded backend ABI mismatch: actual={}, required={}", actual_abi, promise::rhi::abi_version)));
        promise::rhi::error creation_error = promise::rhi::error::invalid_argument;
        auto* core = make(promise::rhi::abi_version, &desc, &creation_error);
        if (!core || creation_error != promise::rhi::error::ok) {
            if (core)
                destroy(core);
            return std::unexpected(fail(backend_load_stage::core_creation,
                                        std::format("Backend core creation refused (RHI error={}); see the backend startup diagnostic",
                                                    static_cast<std::uint32_t>(creation_error))));
        }
        return backend_token(new backend_context(core, destroy, selected_build_id));
    }
} // namespace deren::utility
