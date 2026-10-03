// Headless unit tests: deren.utility.dynamic_link (RHI plan v4 §7.4) and the promise
// contract it carries (the module deren.promise.rhi, §3.3 - §3.5, §4.1 - §4.2) =======
//
// The loader is the primitive the backend boundary is built on, so what is checked here is the
// contract rather than an implementation detail: a file that is not there is a returned error and
// not a crash, the platform suffix is completed when the caller leaves it off, a missing symbol is
// an error, a relative bare name is found, the library is unloaded when it goes out of scope, a
// moved-from library is empty instead of dangling, and each symbol lookup goes through the
// backend's own deleter exactly once.
//
// The library under test is tests/probe_backend.cpp, which CMake builds twice from one source
// file: the static half is linked into this test (so abi_export.hpp's static branch is exercised
// by the linker) and the DLL half is only ever opened at run time (the shared branch). Both halves
// are then driven through the SAME function, `check_core_contract()`, because "the two answer the
// same" is the whole point of the export keywords - and the C ABI is declared once, in
// promise/rhi/backend_entry.hpp, rather than again here.
#include "vk_test.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

import deren.promise.rhi;
import deren.utility.dynamic_link;

// After the imports it needs: the header names deren::promise::rhi types (see its own note).
#include "../promise/rhi/backend_entry.hpp"

namespace {

    namespace fs = std::filesystem;
    namespace rhi = deren::promise::rhi;

    // From CMake ($<TARGET_FILE_NAME:probe_backend>): the DLL suffix is platform-dependent, so the
    // test does not hard-code it.
    constexpr std::string_view probe_file_name = VR_TEST_PROBE_BACKEND_FILE_NAME;
    constexpr std::string_view missing_file_name = "deren_probe_backend_that_does_not_exist.dll";
    constexpr std::string_view unload_probe_file_name = "deren_probe_backend_unload_probe.dll";
    constexpr std::string_view detach_probe_file_name = "deren_probe_backend_detach_probe.dll";

    using make_core_fn = rhi::api_core* (*)(std::uint32_t, rhi::create_info const*, rhi::error*);
    using destroy_core_fn = void (*)(rhi::api_core*);

    /** @brief a resolved C symbol: void const* to function pointer, constness dropped on purpose */
    template <typename function>
    function as_function(void const* address) {
        return reinterpret_cast<function>(const_cast<void*>(address));
    }

    /**
     * @brief everything the promise contract promises, driven through one pair of entry points
     *
     * Called once with the symbols resolved from the loaded DLL and once with the ones the linker
     * found in the static half. `make_core` and `destroy_core` are the only things that differ;
     * every expectation below has to hold for both.
     */
    void check_core_contract(make_core_fn make_core, destroy_core_fn destroy_core, char const* which_half) {
        // The ABI number is part of the contract, not an implementation detail: pin the value so a
        // silent renumbering is a test failure and not a mystery at a customer's machine. 1 -> 2 in the
        // recording-surface batch: SIX new virtuals landed on EXISTING tier-1 types (a vtable shift).
        // 2 -> 3 in the one-creation-structure batch: `deren_make_api_core()` grew the creation
        // descriptor it always described itself as taking, and the backend's own twin of that
        // structure is gone - no vtable moved, but the C entry's signature did.
        // 3 -> 4 in the owned-handle batch: `release()` landed as a pure virtual on seven EXISTING
        // tier-1 types, which is the vtable-shifting case the rule above names.
        // 4 -> 5 in the addressable-buffer batch: `acceleration_structure_address` moved from the
        // `device_address` ability to `ray_tracing`, so BOTH tier-2 abilities changed shape.
        // 5 -> 6 when `vulkan_escape` gained `native_buffer` (a virtual appended to an existing
        // ability).
        // plan §10.3 measured the mechanism; this line is the number itself.
        CHECK(rhi::abi_version == 6u);
        CHECK(static_cast<std::uint32_t>(rhi::error::ok) == 0u);
        CHECK(static_cast<std::uint32_t>(rhi::error::abi_mismatch) == 7u);

        // THE CREATION DESCRIPTOR IS THE CONTRACT'S ONE STRUCTURE, and this is the proof that it
        // crosses: every field is filled with a value the defaults would not produce, and the probe
        // echoes `window_width` back through `frame_begin()` (see probe_backend.cpp). `struct_size`
        // stays at the type's own default - it is the ABI guard, not a knob.
        rhi::create_info creation{};
        creation.window_width = 3;
        creation.window_height = 5;
        creation.vsync = false;
        creation.validation_layers = true;
        creation.window_visible = false;
        creation.window_title = "probe";

        // A mismatched ABI is refused before any object exists, and it is reported through the out
        // parameter - a null `api_core` plus a code, never a crash and never an exception.
        rhi::error mismatch_error = rhi::error::ok;
        CHECK(make_core(rhi::abi_version + 1u, &creation, &mismatch_error) == nullptr);
        CHECK_MSG(mismatch_error == rhi::error::abi_mismatch, which_half);
        // ... and a caller that passes no out parameter is still not crashed into (the backend has
        // to tolerate the null: the engine passes one, a probe or a script may not).
        CHECK(make_core(rhi::abi_version + 1u, &creation, nullptr) == nullptr);

        // NO DESCRIPTOR IS A CALLER BUG, not a request for the standard context: the answer is a null
        // pointer and `invalid_argument`, and the contract spells "standard context" as `create_info{}`
        // (backend_entry.hpp). A backend that defaulted here would hide the bug.
        rhi::error missing_desc_error = rhi::error::ok;
        CHECK(make_core(rhi::abi_version, nullptr, &missing_desc_error) == nullptr);
        CHECK_MSG(missing_desc_error == rhi::error::invalid_argument, which_half);

        // The matching call hands out a live object and says so in the out parameter. The sentinel
        // is not `ok`, so a backend that never wrote it fails the check below.
        rhi::error make_error = rhi::error::abi_mismatch;
        std::shared_ptr<rhi::api_core> core{make_core(rhi::abi_version, &creation, &make_error), destroy_core};
        CHECK_MSG(make_error == rhi::error::ok, which_half);
        CHECK(core != nullptr);
        if (core == nullptr) {
            return;
        }

        // Ownership is real: the deleter the engine installed is the backend's own (for the DLL
        // half, the pointer resolved out of that DLL), the count is observable, and copying the
        // handle does not hand the object out twice.
        CHECK(core.use_count() == 1);
        {
            std::shared_ptr<rhi::api_core> const borrowed = core;
            CHECK(core.use_count() == 2);
        }
        CHECK(core.use_count() == 1);

        // ---- tier-2: what the backend says it can do, and what it hands over ---------------
        // The probe announces exactly two abilities. What matters here is the shape of the answer:
        // a bit set (not an ordered enum), no bit outside the known set, and every announced
        // ability reachable through `query_extension()` with the kind it claims.
        rhi::ability_bits const abilities = core->abilities();
        CHECK_MSG(abilities == (rhi::to_bits(rhi::extension_kind::device_address) |
                                rhi::to_bits(rhi::extension_kind::descriptor_heap)),
                  which_half);
        CHECK((abilities & ~rhi::all_abilities()) == rhi::no_abilities);

        // ---- G1: THE SAME INVARIANT WALKED OVER EVERY DEFINED BIT, instead of spelled out per kind -------
        // `all_extension_kinds()` is the CONTRACT's own list, so a sixth ability cannot be forgotten here:
        // for each kind, announced => `query_extension()` answers with an object whose `kind()` matches,
        // and not announced => it answers null. "Reported but not retrievable" AND "retrievable but not
        // reported" both fail, for the probe backend's two halves and - through this same function - for
        // the real backend, whose answer today is the empty set (see core's startup self-check, gate G2).
        for (rhi::extension_kind const kind : rhi::all_extension_kinds()) {
            rhi::extension* const ability = core->query_extension(kind);
            if (rhi::has_ability(abilities, kind)) {
                CHECK_MSG(ability != nullptr, which_half);
                if (ability != nullptr) {
                    CHECK(ability->kind() == kind);
                }
            } else {
                CHECK_MSG(ability == nullptr, which_half);
            }
        }

        // ---- tier-1: a factory, and a virtual call that comes back out of the backend ---------
        // The one ability the probe actually implements, fetched by the kind it announces - the caller
        // asked for this kind, so the downcast below is a static_cast and not a dynamic_cast (-fno-rtti).
        // The traversal above already checked that an announced bit answers with a matching object.
        rhi::extension* const address_ability = core->query_extension(rhi::extension_kind::device_address);

        // The descriptor is a POD that crosses the boundary by value; the answer is a polymorphic
        // handle the caller can only see through the base. The other factories take descriptors
        // that are still forward-declared, so they cannot even be called yet (S1 defines them) -
        // which is exactly what "the shape can be reviewed before the shapes it carries" means.
        rhi::buffer* const buffer = core->create_buffer(rhi::buffer_desc{.size = 64u});
        CHECK(buffer != nullptr);
        if (buffer != nullptr) {
            CHECK(buffer->size() == 64u);
            if (address_ability != nullptr) {
                // -fno-rtti: the caller knows what it asked for, so the downcast is a static_cast
                // and not a dynamic_cast (promise/rhi/rhi.extension.cppm says the same).
                auto* const address = static_cast<rhi::device_address*>(address_ability);
                // The answer depends on the size the descriptor carried, so this is a real round
                // trip: the descriptor went in, the virtual call came back out, and the arithmetic
                // the probe documents (0x1000 + size + offset) held on the way.
                CHECK(address->buffer_address(*buffer, 0u) == 0x1000ull + 64ull);
                CHECK(address->buffer_address(*buffer, 8u) == 0x1000ull + 64ull + 8ull);
            }
        }

        // ---- tier-1: THE OWNED REFERENCE IS RELEASED THROUGH THE CONTRACT ---------------------
        // `rhi::object_manager<T>` is the owner spelling added with `release()` (abi 3 -> 4): the
        // destructor (or `reset()`) calls the handle's `release()`, which runs INSIDE the backend and
        // drops ONE reference - it is deliberately not called `destroy()`, because a backend may serve
        // the same resource to several callers and then the object outlives the call. The proof here is
        // a round trip: the probe's `release()` zeroes the size its descriptor wrote into the buffer,
        // so a release that never crossed cannot show up as `size() == 0`.
        {
            rhi::object_manager<rhi::buffer> owned{core->create_buffer(rhi::buffer_desc{.size = 128u})};
            CHECK(static_cast<bool>(owned));
            CHECK(owned->size() == 128u);

            // move-only: the reference is transferred, and the source is left empty rather than
            // aliasing the same object (which would release it twice)
            rhi::object_manager<rhi::buffer> moved{std::move(owned)};
            CHECK(!static_cast<bool>(owned));
            CHECK(moved->size() == 128u);

            // release-without-release hands the raw handle back; the caller takes over the call
            rhi::buffer* const raw = moved.release();
            CHECK(raw != nullptr);
            CHECK(!static_cast<bool>(moved));
            CHECK(raw->size() == 128u); // no reference dropped yet
            raw->release();             // ... and now the caller drops it
            CHECK(raw->size() == 0u);

            // an empty manager is free: reset() twice is a no-op, and the object above is untouched
            // (the probe's buffer is static, so this is about the manager, not about the object)
            rhi::object_manager<rhi::buffer> empty{};
            empty.reset();
            empty.reset();
            CHECK(!static_cast<bool>(empty));
        }

        // ---- tier-1: the frame calls --------------------------------------------------------
        // No device means no command pool: a null command list is an answer, not a crash. The
        // frame counter starts at a non-zero value in the probe so that a result which was never
        // written by the backend cannot pass for one that was - and `image_index` is the probe's ECHO
        // OF THE CREATION DESCRIPTOR (`creation.window_width` = 3), so these lines are also the proof
        // that the structure handed to `deren_make_api_core()` reached the library and was read there.
        CHECK(core->begin_commands() == nullptr);
        rhi::submit_info const first = core->frame_begin();
        CHECK(first.frame_index == 7u);
        CHECK(first.image_index == 3u);
        rhi::submit_info const second = core->frame_begin();
        CHECK(second.frame_index == 8u);
        CHECK(second.image_index == 3u);
        core->present();
        core->wait_idle();

        // Destruction runs inside the backend, exactly once, and a deleter that only counts first
        // still leaves the actual delete to `destroy_core` (the resolved symbol). The descriptor is
        // passed again here and the out parameter is deliberately null: a backend has to tolerate both.
        int32_t destroy_calls = 0;
        {
            std::shared_ptr<rhi::api_core> scoped{
                make_core(rhi::abi_version, &creation, nullptr),
                [&destroy_calls, destroy_core](rhi::api_core* raw) {
                    ++destroy_calls;
                    destroy_core(raw);
                }};
            CHECK(scoped != nullptr);
        }
        CHECK_MSG(destroy_calls == 1, which_half);
    }

    void test_a_missing_file_is_a_returned_error(fs::path const& directory) {
        auto const missing =
            deren::utility::dynamic_link::load((directory / std::string{missing_file_name}).string());
        CHECK(!missing.has_value());
        if (missing.has_value()) {
            return;
        }
        // The system's error number AND its text survive: a caller can branch on the number
        // (2 = ERROR_FILE_NOT_FOUND, 3 = ERROR_PATH_NOT_FOUND, 126 = ERROR_MOD_NOT_FOUND) and log
        // the text as it came from the OS.
        CHECK(missing.error().code != 0);
        CHECK(!missing.error().message.empty());
    }

    void test_the_platform_suffix_is_completed(fs::path const& directory) {
        fs::path const complete = directory / std::string{probe_file_name};
        fs::path const without_suffix = complete.parent_path() / complete.stem();
        auto const loaded = deren::utility::dynamic_link::load(without_suffix.string());
        CHECK(loaded.has_value());
        if (loaded.has_value()) {
            CHECK(loaded->native_handle() != nullptr);
        }
    }

    void test_loading_the_probe_dll_and_using_its_symbols(fs::path const& directory) {
        auto loaded = deren::utility::dynamic_link::load((directory / std::string{probe_file_name}).string());
        CHECK(loaded.has_value());
        if (!loaded.has_value()) {
            return;
        }
        CHECK(loaded->native_handle() != nullptr);

        auto const version = loaded->symbol("deren_abi_version");
        CHECK(version.has_value());
        CHECK(!loaded->symbol("deren_no_such_symbol_in_the_probe_backend").has_value());
        CHECK(!loaded->symbol("").has_value());
        if (version.has_value()) {
            auto const abi_version = as_function<std::uint32_t (*)()>(version.value());
            CHECK(abi_version() == rhi::abi_version);
            CHECK(abi_version() == deren_abi_version()); // the DLL and the static half agree
        }

        auto const make = loaded->symbol("deren_make_api_core");
        auto const destroy = loaded->symbol("deren_destroy_api_core");
        CHECK(make.has_value());
        CHECK(destroy.has_value());
        if (!make.has_value() || !destroy.has_value()) {
            return;
        }

        // The pair driven here is the one RESOLVED FROM THE DLL, not the one the linker would give
        // this test: that is the plan's rule (§4.1) - an object made inside the library has to be
        // deleted inside the library - and the reason `deren_destroy_api_core` is exported by name
        // instead of being wrapped in engine-side glue.
        check_core_contract(as_function<make_core_fn>(make.value()), as_function<destroy_core_fn>(destroy.value()),
                            "dll");

        // A moved-from library is empty (not a second owner of the same handle), and the symbols
        // taken from the destination keep working.
        deren::utility::dynamic_link::library moved{std::move(*loaded)};
        CHECK(moved.native_handle() != nullptr);
        CHECK(loaded->native_handle() == nullptr);
        auto const moved_version = moved.symbol("deren_abi_version");
        CHECK(moved_version.has_value());
        if (moved_version.has_value()) {
            CHECK(as_function<std::uint32_t (*)()>(moved_version.value())() == rhi::abi_version);
        }
    }

    void test_the_library_is_unloaded_when_it_goes_out_of_scope(fs::path const& directory) {
        // A copy in the *current* directory, opened by its bare name: this is the relative branch
        // (classic search order) and it doubles as the unload proof - on Windows a loaded image
        // holds its file, so the file only becomes deletable after the destructor called
        // FreeLibrary.
        fs::path const copy = fs::current_path() / std::string{unload_probe_file_name};
        fs::copy_file(directory / std::string{probe_file_name}, copy, fs::copy_options::overwrite_existing);
        {
            auto const loaded = deren::utility::dynamic_link::load(copy.filename().string());
            CHECK(loaded.has_value());
            if (loaded.has_value()) {
                CHECK(loaded->symbol("deren_abi_version").has_value());
#if defined(_WIN32)
                std::error_code still_mapped{};
                CHECK(!fs::remove(copy, still_mapped)); // sharing violation: the image is mapped
#endif
            }
        }
        std::error_code remove_error{};
#if defined(_WIN32)
        CHECK(fs::remove(copy, remove_error)); // the destructor unloaded it
#else
        static_cast<void>(fs::remove(copy, remove_error)); // POSIX allows unlinking a loaded .so
#endif
    }

    void test_a_detached_library_is_not_unloaded(fs::path const& directory) {
        // THE INVERSE OF THE UNLOAD PROOF, and the reason `detach()` exists (DYNAMIC_LINK_V2.md §13):
        // a detached library keeps its handle, so its file stays mapped after this object is gone.
        // MEASURED on the real backend - unloading it after a context had been built and torn down
        // never returned - so "the product never unloads" has to be expressible, and this is what
        // proves the expression works rather than assuming it.
        fs::path const copy = fs::current_path() / std::string{detach_probe_file_name};
        fs::copy_file(directory / std::string{probe_file_name}, copy, fs::copy_options::overwrite_existing);
        {
            auto loaded = deren::utility::dynamic_link::load(copy.filename().string());
            CHECK(loaded.has_value());
            if (!loaded.has_value()) {
                return;
            }
            CHECK(loaded->symbol("deren_abi_version").has_value());

            void* const detached = loaded->detach();
            CHECK(detached != nullptr);
            // THE OBJECT IS EMPTY AFTERWARDS: the handle left with the caller, and every remaining
            // operation says so instead of using a handle it no longer owns.
            CHECK(loaded->native_handle() == nullptr);
            CHECK(!loaded->symbol("deren_abi_version").has_value());
            // destructor runs here: it must NOT unload (the handle is gone; nothing left to close)
        }
#if defined(_WIN32)
        std::error_code still_mapped{};
        CHECK(!fs::remove(copy, still_mapped)); // sharing violation: detach did NOT unload it
                                                // The file stays: it is still mapped by THIS test process, which is exactly the claim. The
                                                // next run starts by overwriting it, and the mapping dies with the process.
#else
        std::error_code remove_error{};
        static_cast<void>(fs::remove(copy, remove_error)); // POSIX unlinks a loaded .so happily
#endif
    }

    void test_the_static_half_behaves_the_same() {
        // No library is involved here: abi_export.hpp's static branch produces ordinary C symbols,
        // and the same contract is reached through the linker. `deren_make_api_core` and friends
        // are the declarations from promise/rhi/rhi.api_core.cppm - this test no longer spells the C ABI
        // out by hand, so a drift between the header and the backend is a link error.
        CHECK(deren_abi_version() == rhi::abi_version);
        check_core_contract(&deren_make_api_core, &deren_destroy_api_core, "static");
    }
} // namespace

int main(int argc, char** argv) {
    // The probe DLL sits next to this executable (both are outputs of the CMake binary directory)
    // while the working directory is <binary dir>/test-run, so argv[0] is where to look. Deriving
    // the path here keeps generated string literals with Windows path separators out of the build
    // ("\p" would be an escape sequence).
    fs::path const self = argc > 0 && argv != nullptr && argv[0] != nullptr ? fs::absolute(fs::path{argv[0]})
                                                                            : fs::current_path();
    fs::path const directory = self.parent_path();

    test_a_missing_file_is_a_returned_error(directory);
    test_the_platform_suffix_is_completed(directory);
    test_loading_the_probe_dll_and_using_its_symbols(directory);
    test_the_library_is_unloaded_when_it_goes_out_of_scope(directory);
    test_a_detached_library_is_not_unloaded(directory);
    test_the_static_half_behaves_the_same();

    return deren::vk_test::finish("test_dynamic_link");
}
