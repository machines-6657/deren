// -*- C++ -*-
// ============================================================================
// file: vulkan/core/core.entry.cpp
//
// THE REAL BACKEND'S C ABI SURFACE (plan_rhi_v4.md §4.1 item 3, S2): the three
// `extern "C"` symbols that promise/rhi/backend_entry.hpp declares, DEFINED here for
// deren::vulkan::core - the object that already owns the instance, the device
// and the swapchain, and that already derives from deren::promise::rhi::api_core
// (vulkan/core/core.declarations.cppm:271). There is no wrapper type: the object the
// engine gets back from deren_make_api_core() IS the core.
//
// WHY A PLAIN TU AND NOT A MODULE IMPLEMENTATION: the probe backend's lesson
// (tests/probe_backend.cpp is a plain TU for the same reason). `extern "C"` symbols
// declared inside a module purview get module attachment, and the whole point of these
// three is that they are reachable BY NAME from another image that does not import any
// module of this target. So the file imports what it needs, includes the declared
// surface, and defines it at global scope.
//
// THE ABI'S OWN SHAPE (backend_entry.hpp) IS TAKEN AS GIVEN, AND IT NOW CARRIES THE CREATION
// PARAMETERS - the gap the boundary spike measured (DYNAMIC_LINK_V2.md §9 item 4) is closed here:
//   - `deren_abi_version()` answers the contract's compile-time number, so a host that
//     disagrees learns it before an object exists.
//   - `deren_make_api_core(abi, desc, out_error)` takes the contract's ONE creation structure by
//     pointer, refuses a mismatched ABI with `error::abi_mismatch` and an absent descriptor with
//     `error::invalid_argument`, and hands the descriptor to the backend's constructor - which is the
//     same structure the program filled at startup, with no translation in between (there is no
//     backend-side twin of it any more; see rhi.core_desc.cppm). No exception crosses (§4.2).
//   - `deren_destroy_api_core(core)` deletes INSIDE the backend, which is the whole reason it is
//     exported by name instead of the engine calling `delete`.
//
// CMake compiles this into deren_vulkan either way (STATIC today, SHARED behind
// -DDEREN_BACKEND_SPIKE=ON): linked statically the symbol is an ordinary C name that nothing calls,
// and under the spike the same TU is the dllexport side of the boundary.
// ============================================================================
import deren.promise.rhi;
import deren.vulkan.core;

// After the imports it names: the header declares deren::promise::rhi types (see its note).
#include "../../promise/rhi/backend_entry.hpp"

namespace rhi = deren::promise::rhi;

extern "C" DEREN_API_EXPORT std::uint32_t deren_abi_version() {
    return rhi::abi_version;
}

extern "C" DEREN_API_EXPORT rhi::api_core* deren_make_api_core(std::uint32_t abi_version, rhi::create_info const* desc,
                                                               rhi::error* out_error) {
    // THE HANDSHAKE FIRST, BEFORE ANY OBJECT EXISTS (plan §4.2): an engine built against a
    // different contract learns "we disagree" as a return value, at the only moment where
    // nothing has been allocated yet and nothing has to be cleaned up.
    if (abi_version != rhi::abi_version) {
        if (out_error != nullptr) {
            *out_error = rhi::error::abi_mismatch;
        }
        return nullptr;
    }
    if (desc == nullptr) {
        // "I have no creation parameters" is a caller bug, not a request for the standard context:
        // `create_info{}` is how that is spelled, and defaulting here would hide the bug.
        if (out_error != nullptr) {
            *out_error = rhi::error::invalid_argument;
        }
        return nullptr;
    }
    if (out_error != nullptr) {
        *out_error = rhi::error::ok;
    }
    // OWNERSHIP TRANSFER, spelled out: the engine wraps this raw pointer in a
    // `shared_ptr` whose deleter is `deren_destroy_api_core` resolved from THIS library,
    // so the matching `delete` below runs in the allocator that made the object.
    //
    // The descriptor is read by the constructor and not kept: its `window_title` is borrowed only
    // until this call returns (GLFW copies the text into the window), which the contract's own note
    // states and the constructor's member note repeats.
    //
    // A failure INSIDE construction does not come back as an `error`: the backend's own
    // startup rule is a NAMED panic with the missing thing in the message (a required
    // extension, a required feature, a swapchain that cannot be created -
    // core.constructor.cppm's `panic` sites), and the spike confirmed that shape. So a
    // non-null return means "the context exists"; there is no partially-built context to
    // report on.
    return new deren::vulkan::core{*desc};
}

extern "C" DEREN_API_EXPORT void deren_destroy_api_core(rhi::api_core* core) {
    // Accepts nullptr on purpose: a refused `deren_make_api_core` still ends up inside a
    // `shared_ptr`, and that shared_ptr's deleter is this function.
    delete core;
}
