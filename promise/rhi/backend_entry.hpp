// -*- C++ -*-
// ============================================================================
// file: promise/rhi/backend_entry.hpp
//
// The backend's C ABI surface: the three `extern "C"` symbols a backend exports and
// a host resolves (RHI plan v4, §1.15, §4.1 item 3).
//
// WHY THIS IS NOT PART OF deren.promise.rhi (m03159), EVEN THOUGH IT LIVES IN ITS
// DIRECTORY (m04074): the contract is a collection of virtual base classes, and an
// entry point is not one. A backend DEFINES these three, a host RESOLVES them, and
// neither side derives from anything to do it. So the file sits in `promise/rhi/`
// because it is the graphics-API boundary's surface and belongs with the contract it
// hands out - but it is NOT a module partition, never appears in the module's file
// set, and is not imported: it is an ordinary header that both sides `#include`, and
// a translation unit that includes it must first `import deren.promise.rhi;` for the
// types the declarations name. Nothing here is a type the engine and the backend
// share; the only shared thing is the NAME and the SIGNATURE, which is what this file
// is for.
//
// WHO INCLUDES IT, AND IN WHICH ORDER. The declarations below name
// `deren::promise::rhi::api_core` and `deren::promise::rhi::error`, so the translation unit has
// to do `import deren.promise.rhi;` BEFORE including this header - an import declaration
// cannot live in a header, and the two orderings that matter are already exercised by
// the two sides of the probe:
//
//     import deren.promise.rhi;                    // the boundary's types
//     #include "../promise/rhi/backend_entry.hpp" // the entry points
//
// The keyword comes from utility/abi_export.hpp, so the same declaration is
// `dllexport` while the backend is being built, `dllimport` while the host imports
// it, and nothing at all when the backend is linked in statically. CMake builds
// tests/probe_backend.cpp twice for exactly that reason.
//
// Measured on this machine (plan §10.3): the probe DLL exports exactly these three
// names and the statically linked test exports none of them.
// ============================================================================
#pragma once

#include "../../utility/abi_export.hpp"

#include <cstdint>

/**
 * @file promise/rhi/backend_entry.hpp
 * @brief the three `extern "C"` entry points that hand an `api_core` across the boundary.
 * @ingroup boundary
 *
 * The C ABI is deliberately tiny: a version number, a factory whose ownership transfer is spelled out
 * by the deleter it returns, and that deleter. Everything else the engine and the backend share is a
 * virtual base class in `deren.promise.rhi`, compiled by both sides independently (§4.1 item 1), so no
 * C++ type is passed by value and no symbol other than these three is exchanged.
 *
 * Include this after `import deren.promise.rhi;` - the declarations below name the contract's types, and
 * an `import` declaration cannot be written in a header.
 */

// The plan's §4.1 item 3 in one place: `deren_make_api_core()` returns a RAW pointer and the
// backend's own deleter by name, so the engine never calls the default `delete` on memory the
// backend allocated. The alternative shape - the backend returning a `std::shared_ptr` - needs
// `-Wreturn-type-c-linkage` to be silenced and makes "same standard library" a boundary premise
// rather than a preference; it was rejected in m02244.
extern "C" {

/// The backend's ABI number. Must equal `deren::promise::rhi::abi_version`.
DEREN_API_EXPORT std::uint32_t deren_abi_version();

/// The only producer of an `api_core`: transfers ownership of a raw pointer.
///
/// `desc` is the contract's ONE creation structure (promise/rhi/rhi.core_desc.cppm), passed by
/// POINTER because it is an append-only POD whose size the caller declares in `struct_size` - the
/// backend reads only the bytes the caller says it compiled, which is what lets an older program
/// talk to a newer backend. It is never null in a well-formed call, and a null one is refused with
/// `error::invalid_argument` rather than defaulted, because "I have no creation parameters" is a
/// caller bug and not a request for the standard context (`create_info{}` is how that is spelled).
///
/// On a version mismatch it returns `nullptr` and writes
/// `deren::promise::rhi::error::abi_mismatch`, which is why a caller may not assume the
/// out-parameter was written on failure.
DEREN_API_EXPORT deren::promise::rhi::api_core* deren_make_api_core(std::uint32_t abi_version,
                                                                    deren::promise::rhi::create_info const* desc,
                                                                    deren::promise::rhi::error* out_error);

/// The deleter itself, exported by name: the engine builds its `shared_ptr` around
/// it so that the delete runs inside the backend. Accepts `nullptr`, because a
/// refused `deren_make_api_core` still ends up in a `shared_ptr`.
DEREN_API_EXPORT void deren_destroy_api_core(deren::promise::rhi::api_core* core);

} // extern "C"
