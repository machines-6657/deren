// -*- C++ -*-
// ============================================================================
// file: tests/spike_backend_boundary.cpp
//
// THE BOUNDARY SPIKE (DYNAMIC_LINK_V2.md §4 step 1b, plan_rhi_v4.md §9 items 14-17).
//
// WHY THIS EXISTS AND WHY IT IS NOT IN `VR_TEST_TARGETS`: the plan's own review found that
// the four known-unknowns of the DLL flip were all sitting at the END of the critical path -
// sanitizers + an instrumented DLL, the real DLL's `shared_ptr` deleter, two static
// mimalloc instances, and a module BMI crossing a target boundary. Measuring them after the
// whole contract migration means one boundary failure invalidates weeks of interface work.
// So the flip's boundary is measured EARLY, in a throwaway build tree
// (`-DDEREN_BACKEND_SPIKE=ON`), and this test is the measurement.
//
// It is deliberately NOT part of the ctest set: it loads a DLL and (optionally) creates a
// real device, which the headless CI set cannot do, and `tests/test_docs.cpp` compares
// `VR_TEST_TARGETS` against the workflow - a GPU test in that list would be a lie in CI.
//
// WHAT IT MEASURES, ONE CHECK PER QUESTION:
//
//   Q5 dependency closure  `load()` succeeds on an ABSOLUTE path, which on Windows uses
//                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
//                          and therefore NEVER searches %PATH% (utility/dynamic_link.cppm:19-20).
//                          The real backend imports `vulkan-1.dll`, `libc++.dll`, USER32/GDI32
//                          and the statically linked glfw's system libraries; the probe DLL
//                          imported only KERNEL32/libc++/UCRT, so THIS is the new information.
//   Q6 CMake file set      the spike tree builds at all: `deren_vulkan` is a SHARED library
//                          with a PUBLIC `FILE_SET CXX_MODULES`, and this test consumes the
//                          CONTRACT (from the STATIC `promise` target) without consuming the
//                          backend's module. Whether those two facts can coexist is the
//                          question; a configure/build failure is the answer.
//   Q4 BMI across target   `deren_abi_version()` resolved from the DLL answers the number the
//                          TEST compiled from its own copy of the contract module - the
//                          "both sides compile the contract" rule, checked across a real
//                          image boundary. With `--with-device`, virtual calls on an object
//                          CONSTRUCTED INSIDE the DLL dispatch through the exe's own vtable
//                          copy, which is the stronger form of the same question.
//   Q2 the deleter         with `--with-device`: the `shared_ptr` is built with the deleter
//                          RESOLVED FROM THE DLL, so the `delete` runs inside the library that
//                          allocated the object. The static half of the probe proved the shape
//                          against a probe; this proves it against `deren::vulkan::core`.
//   Q1 sanitizers          not observable from inside a single test: it is decided at BUILD
//                          time (an instrumented DLL cannot put a second ASan runtime into the
//                          instrumented process - CMakeLists' probe_backend comment). The
//                          spike tree is configured twice, with and without the sanitizer
//                          flags, and the result is recorded in the spike's own report.
//   Q3 two mimalloc        likewise a property of the LINK, not of a call: measured with
//                          `llvm-nm` over both images (both must contain `mi_malloc`/`mi_free`)
//                          plus the owning-STL audit in scripts/check_backend_boundary.py.
//
// THE SAFE DEFAULT: without `--with-device` this test never creates a window or a device, so
// it runs anywhere and still answers Q4 (weak form), Q5, Q6 and the ABI handshake. With
// `--with-device` it constructs the real context - which creates a real window through the
// backend's own default path - and that is the one part that needs a machine with a display
// and a Vulkan device.
// ============================================================================
#include "vk_test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

import deren.promise.rhi;
import deren.utility.dynamic_link;

// After the imports it needs: the header names deren::promise::rhi types (see its own note).
#include "../promise/rhi/backend_entry.hpp"

namespace {

    namespace rhi = deren::promise::rhi;

    /// A MARK ON STDERR, UNBUFFERED AND FLUSHED, so a hang can be located.
    ///
    /// This exists because the first version of this spike had none: its stdout went to a redirected
    /// file, `fwrite` to a redirected stream is BLOCK-buffered, and the process was terminated before
    /// any of it was flushed - so a run that had got all the way to a live device left an EMPTY file and
    /// no evidence at all (the backend's own `debug.log` is what eventually showed how far it got).
    /// stderr is unbuffered by default and the `fflush` makes that explicit, so every mark that printed
    /// is a mark that happened.
    void mark(char const* what) {
        std::fprintf(stderr, "spike: %s\n", what);
        std::fflush(stderr);
    }

    // From CMake ($<TARGET_FILE:deren_vulkan>): the ABSOLUTE path, because the absolute branch is
    // the one whose search flags the plan depends on (a bare name would take the classic order,
    // which does search %PATH% - a different question).
    constexpr std::string_view spike_dll_path = VR_SPIKE_BACKEND_DLL;

    using make_core_fn = rhi::api_core* (*)(std::uint32_t, rhi::create_info const*, rhi::error*);
    using destroy_core_fn = void (*)(rhi::api_core*);

    /** @brief a resolved C symbol: `void const*` to function pointer, constness dropped on purpose */
    template <typename function>
    function as_function(void const* address) {
        return reinterpret_cast<function>(const_cast<void*>(address));
    }

    /// Q4 (weak form) + the ABI handshake, driven through the symbols the DLL itself exports.
    void check_the_handshake(make_core_fn make_core) {
        // A mismatched ABI is refused BEFORE any object exists, and the refusal is a value, not a
        // crash and not an exception (§4.2). The wrong number is deliberately the right one plus
        // one: it is the shape a stale engine would present.
        rhi::create_info const creation{};
        rhi::error status = rhi::error::ok;
        rhi::api_core* const refused = make_core(rhi::abi_version + 1u, &creation, &status);
        CHECK(refused == nullptr);
        CHECK(status == rhi::error::abi_mismatch);

        // A refused call still ends up inside a `shared_ptr` in the engine's own code, which is why
        // the deleter tolerates nullptr; `check_the_real_context` below covers the other half.
        CHECK(status != rhi::error::ok);

        // AND NO CREATION DESCRIPTOR IS A NAMED REFUSAL, not a defaulted context: `create_info{}` is
        // how the standard context is spelled (backend_entry.hpp), so a null pointer is a caller bug
        // that the REAL backend reports with a code rather than papering over.
        rhi::error missing = rhi::error::ok;
        CHECK(make_core(rhi::abi_version, nullptr, &missing) == nullptr);
        CHECK_MSG(missing == rhi::error::invalid_argument, "the real backend refuses a null create_info by name");
    }

    /// Q2 + Q4 (strong form): the real context, its virtuals, and its destruction inside the DLL.
    void check_the_real_context(make_core_fn make_core, destroy_core_fn destroy_core) {
        // THE CONTRACT'S ONE CREATION STRUCTURE, filled here the way the application fills it at
        // startup (main.cpp) - the same type, and the same call shape, the flip will use.
        //
        // `window_visible = false` IS A MEASURED REQUIREMENT, NOT A PREFERENCE: a VISIBLE window does not
        // come up in this session at all (this spike's first version hung with 0.5 s of CPU and no window
        // ever appeared), while the HIDDEN path runs to completion - which is exactly what the
        // application's own scripted capture does (`main.cpp` sets
        // `core_options.window_visible = capture.frames == 0`, so a capture gets no visible window). The
        // pixels, the handles and the lifetime ordering are identical either way, so hiding it costs this
        // measurement nothing; it only stops the spike depending on a window station that can map a
        // visible window.
        rhi::create_info creation{};
        creation.window_width = 640;
        creation.window_height = 480;
        creation.window_visible = false;

        mark("calling deren_make_api_core (instance/device/swapchain/heap are built in here)");
        rhi::error status = rhi::error::ok;
        rhi::api_core* const raw = make_core(rhi::abi_version, &creation, &status);
        mark("deren_make_api_core returned");
        CHECK(status == rhi::error::ok);
        CHECK(raw != nullptr);
        if (raw == nullptr) {
            return; // the backend refuses by returning null; the startup diagnosis is its own
        }

        // THE DELETER IS THE DLL'S OWN SYMBOL, not the one the linker would have given this test:
        // an object allocated inside the library must be freed inside it (plan §4.1 item 3). The
        // `shared_ptr` lives in an inner scope ON PURPOSE, so the destroy runs while the library is
        // still loaded - the ordering the engine has to reproduce with its own member order.
        {
            std::shared_ptr<rhi::api_core> core{raw, destroy_core};

            // A virtual call on an object CONSTRUCTED INSIDE THE DLL, dispatching through the
            // vtable compiled into this executable: the cross-image form of "both sides compile the
            // contract" (Q4).
            mark("calling abilities() across the image boundary");
            rhi::ability_bits const abilities = core->abilities();
            CHECK(rhi::has_ability(abilities, rhi::extension_kind::vulkan_escape));
            // THE SECOND BIT, ANNOUNCED ONLY ONCE BUFFERS BECAME PRODUCTIBLE (abi 5 moved the
            // acceleration-structure half of this ability to `ray_tracing`, which is the only ability
            // that can hand out that operand - so this bit stopped being hostage to a resource no
            // backend could make).
            CHECK(rhi::has_ability(abilities, rhi::extension_kind::device_address));

            mark("calling query_extension(vulkan_escape)");
            rhi::extension* const escape = core->query_extension(rhi::extension_kind::vulkan_escape);
            CHECK(escape != nullptr);
            if (escape != nullptr) {
                CHECK(escape->kind() == rhi::extension_kind::vulkan_escape);

                // The escape's whole promise is that these are the backend's LIVE handles, so a
                // non-null answer is the check that the ability is servable rather than announced.
                auto* const vulkan = static_cast<rhi::vulkan_escape*>(escape);
                CHECK(vulkan->native_instance() != nullptr);
                CHECK(vulkan->native_device() != nullptr);
                CHECK(vulkan->native_queue() != nullptr);

                // The escape does NOT hand out proc addresses (deliberately, §3.4): the front end
                // resolves `vkGetDeviceProcAddr` itself. The absence is part of the measured shape,
                // so the plan's own gap - "where does the front end get that function pointer" - is
                // recorded rather than rediscovered later.
            }

            // The resource factories are the S3 design surface and answer nullptr today; what this
            // ---- THE FIRST OWNED RESOURCE ACROSS THE CONTRACT -------------------------------------
            // `create_buffer` was the S3 gate ("it must answer null, not crash") until step 2a gave the
            // descriptor a shape and the backend a real implementation. What is checked now is the
            // whole owned-handle path on a REAL allocation: the factory builds the buffer inside the
            // DLL, the descriptor's size and its HOST-VISIBILITY decide what `mapped()` answers, and the
            // caller's single `release()` gives the reference back to the allocator (which is the
            // decrement `free_buffer` performs - release, not necessarily destruction).
            //
            // THE INITIAL BYTES ARE IN THE DESCRIPTOR rather than a second upload call: a backend that
            // keys resources on CONTENT can only recognise identical content if it is handed the content
            // at creation (rhi.api_core.cppm's `buffer_desc` note), so this is the shape that keeps the
            // renderer's deduplication alive across the boundary.
            std::array<std::byte, 64> initial_bytes{};
            for (std::size_t index = 0; index < initial_bytes.size(); ++index) {
                initial_bytes[index] = static_cast<std::byte>(index);
            }

            mark("create_buffer: a host-visible storage buffer, 64 B with initial contents");
            rhi::buffer_desc host_visible{};
            host_visible.size = 64u;
            host_visible.usage = rhi::buffer_usage::storage_coherent;
            host_visible.flags = rhi::to_bits(rhi::buffer_flag::device_address);
            host_visible.initial_bytes = std::span<std::byte const>(initial_bytes.data(), initial_bytes.size());

            rhi::object_manager<rhi::buffer> owned{core->create_buffer(host_visible)};
            CHECK(static_cast<bool>(owned));
            if (owned) {
                CHECK(owned->size() == 64u);
                // A HOST-VISIBLE buffer answers with the bytes the caller may write; an EMPTY span is the
                // contract's spelling of "this one is not host-visible" (see the type's note).
                CHECK(!owned->mapped().empty());
                CHECK(owned->mapped().size() == 64u);
            }

            mark("create_buffer: a GPU-ONLY buffer, allocate-only, must not be host-visible");
            rhi::buffer_desc gpu_only{};
            gpu_only.size = 256u;
            gpu_only.usage = rhi::buffer_usage::storage_gpu_only;
            rhi::object_manager<rhi::buffer> device_local{core->create_buffer(gpu_only)};
            CHECK(static_cast<bool>(device_local));
            if (device_local) {
                CHECK(device_local->size() == 256u);
                CHECK(device_local->mapped().empty());
            }

            mark("create_buffer: a zero-byte descriptor is refused by name-less nullptr (no buffer asked for)");
            rhi::buffer_desc nothing{};
            CHECK(core->create_buffer(nothing) == nullptr);

            // ---- THE device_address ABILITY, ON A REAL ALLOCATION ---------------------------------
            // The descriptor asked for `buffer_flag::device_address`, so the ability has to answer with
            // the buffer's real device address - and `vkGetBufferDeviceAddress` is only defined for a
            // buffer created with that usage, which is also why the ABILITY answers 0 for one created
            // without it. Both halves are checked here: the bit is a promise about service, and the
            // service is what these two lines measure.
            mark("query_extension(device_address) and buffer_address() on both buffers");
            rhi::extension* const address_extension = core->query_extension(rhi::extension_kind::device_address);
            CHECK(address_extension != nullptr);
            if (address_extension != nullptr) {
                CHECK(address_extension->kind() == rhi::extension_kind::device_address);
                auto* const addresses = static_cast<rhi::device_address*>(address_extension);
                if (owned) {
                    // the addressable one: a real address, and the offset is carried through
                    std::uint64_t const base = addresses->buffer_address(*owned, 0u);
                    CHECK(base != 0u);
                    CHECK(addresses->buffer_address(*owned, 16u) == base + 16u);
                }
                if (device_local) {
                    // the one that asked for no address: 0, not a guess
                    CHECK(addresses->buffer_address(*device_local, 0u) == 0u);
                }

                // ---- THE ESCAPE'S native_buffer(), WHICH IS WHAT MAKES THE ENGINE MIGRATION POSSIBLE
                // The raw Vulkan calls that take a buffer (`vkCmdBindVertexBuffers`, `VkDescriptorBufferInfo`,
                // an acceleration structure's build geometry) need the handle itself, and a device address
                // cannot stand in for it. This is the call that lets those sites stop reading the
                // allocator's detail map - so the two buffers must answer with two DISTINCT non-null
                // handles, or the engine would be handed the same VkBuffer for both.
                //
                // The escape object is fetched again here because the first one is scoped to its own
                // block above; both fetches answer the same member, so this is one object, not two.
                rhi::extension* const escape_extension = core->query_extension(rhi::extension_kind::vulkan_escape);
                auto* const escape_native = escape_extension != nullptr ? static_cast<rhi::vulkan_escape*>(escape_extension) : nullptr;
                if (owned && device_local && escape_native != nullptr) {
                    void* const native_host = escape_native->native_buffer(*owned);
                    void* const native_device = escape_native->native_buffer(*device_local);
                    CHECK(native_host != nullptr);
                    CHECK(native_device != nullptr);
                    CHECK(native_host != native_device);
                }
            }

            // frame_image() before any acquire is nullptr by contract; the frame verbs refuse rather
            // than record nonsense. Driving them here would need a swapchain acquisition.
            mark("about to leave the scope: two releases and the DLL's deleter (core teardown) run next");
        } // <- the managers release their buffers, then the DLL's deleter runs
        mark("core teardown returned");
    }

} // namespace

int main(int argc, char** argv) {
    bool with_device = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view{argv[index]} == "--with-device") {
            with_device = true;
        }
    }
    mark(with_device ? "start (--with-device)" : "start (safe mode)");

    // Q5 + Q6: an ABSOLUTE path through the platform's restricted search flags, on a library that
    // has to satisfy its own dependency closure (vulkan-1.dll, libc++.dll, USER32/GDI32, glfw's
    // system libraries) - the probe DLL had none of those.
    auto loaded_result = deren::utility::dynamic_link::load(std::string{spike_dll_path});
    CHECK(loaded_result.has_value());
    if (!loaded_result.has_value()) {
        deren::vk_test::write_line("spike: load failed for {}: {}", spike_dll_path, loaded_result.error().message);
        return deren::vk_test::finish("spike_backend_boundary");
    }

    // ---- THE LIBRARY IS DETACHED, NEVER UNLOADED, AND THAT IS A MEASUREMENT ---------------------
    // `FreeLibrary` on this backend NEVER RETURNS once it has been initialised. MEASURED, with the
    // stderr marks above: every step of this test completed (a real context built inside the DLL, the
    // contract's virtuals called across the image boundary, the DLL's own deleter run and returned) and
    // then the process sat at 0.45 s of CPU for 80+ s with 10 threads and 76 MB that never moved - the
    // only step left was the loader's destructor calling `FreeLibrary`. So the unload is what hangs,
    // not the test, and it is the DLL's process-detach path that blocks (GLFW is initialised inside it
    // and this backend never calls `glfwTerminate` - `core.constructor.cppm` says so in as many words -
    // so a detach that waits on that state is a deadlock, not a slow teardown).
    //
    // `detach()` IS THE DESIGNED EXIT FOR THIS (`dynamic_link::library`, with its own test in
    // tests/test_dynamic_link.cpp), and the plan's invariant 4 already says the DLL lives to the end of
    // the process - so the product does not unload either. Recorded in DYNAMIC_LINK_V2.md §13, where it
    // is a step-4 item: the engine's loader object must detach rather than let its destructor unload,
    // or the product deadlocks where nothing is watching (the window has already closed).
    auto loaded = std::move(*loaded_result);
    CHECK(loaded.native_handle() != nullptr);

    // Q4 (weak form): the DLL's compiled contract number equals the one this executable compiled.
    auto const version_symbol = loaded.symbol("deren_abi_version");
    CHECK(version_symbol.has_value());
    if (version_symbol.has_value()) {
        auto const abi_version = as_function<std::uint32_t (*)()>(version_symbol.value());
        CHECK(abi_version() == rhi::abi_version);
        deren::vk_test::write_line("spike: deren_abi_version() = {} (this executable compiled {})", abi_version(), rhi::abi_version);
    }

    auto const make_symbol = loaded.symbol("deren_make_api_core");
    auto const destroy_symbol = loaded.symbol("deren_destroy_api_core");
    CHECK(make_symbol.has_value());
    CHECK(destroy_symbol.has_value());
    if (make_symbol.has_value() && destroy_symbol.has_value()) {
        make_core_fn const make_core = as_function<make_core_fn>(make_symbol.value());
        destroy_core_fn const destroy_core = as_function<destroy_core_fn>(destroy_symbol.value());

        check_the_handshake(make_core);
        if (with_device) {
            check_the_real_context(make_core, destroy_core);
        } else {
            deren::vk_test::write_line("spike: device path skipped (pass --with-device to build a real context)");
        }
    }

    // THE LAST CALL ON THE LOADER: hand the handle back so the destructor cannot unload (see the note).
    static_cast<void>(loaded.detach());
    mark("all checks done; the library was DETACHED (never unloaded) - see the note");
    return deren::vk_test::finish("spike_backend_boundary");
}
