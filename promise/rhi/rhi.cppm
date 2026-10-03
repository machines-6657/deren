// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi
// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))
//
// The RHI boundary's contract: the abstract interfaces the engine and the backend
// share, and nothing else (RHI plan v4, §3.1 and §4.1). One module, four
// partitions - `:contract` (the ABI number and the `error` enum), `:core_desc`
// (how the program asks for a context), `:extension` (tier-2: the abilities a
// backend may or may not have) and `:api_core` (tier-1: the context, its
// factories and the frame calls).
//
// WHY THE NAME IS deren.promise.rhi (m03716, corrected by m03931): the contract layer as a whole is
// `promise` - a family of abstract interfaces any boundary of this project can express in the same
// shapes - and the graphics-API instance of it is the `rhi` submodule inside it. So the outer
// segment names the family, the inner segment names the API the layer is written for, the module is
// deren.promise.rhi, the types are deren::promise::rhi::*, and the directory is promise/rhi/. Both
// segments are lower case on purpose: every other module segment in this repository is lower case
// and names the directory that holds it (`vulkan/`, `utility/`, `vstd/`), module names end up in
// CMake/BMI paths and in doc page names, and two directories differing only in case are a hazard on
// the case-insensitive filesystems this project is built on. The acronym stays upper case in prose
// ("the RHI"), where it is a word rather than an identifier; all-caps identifiers here are macros
// (DEREN_API_*, VR_TEST_*).
//
// WHY A MODULE RATHER THAN A DIRECTORY OF HEADERS: this layer is a set of virtual
// base classes that BOTH sides compile into their own image (m03159; §4.1's rule is
// that neither side exports a module symbol for it). A module makes the two
// compilations come from one interface unit - listed once per target in CMake - so
// they cannot drift, and it takes the preprocessor out of the layer entirely: no
// partition below this line includes a header, so no macro can leak into the shape
// of the contract.
//
// WHAT IS DELIBERATELY NOT IN HERE:
//
//   - the three `extern "C"` entry points (`deren_abi_version`,
//     `deren_make_api_core`, `deren_destroy_api_core`) and the `DEREN_API_*`
//     keywords they are declared with. They are the ABI SURFACE, not an
//     interface: a backend defines them and a host resolves them, and neither
//     derives from anything. `promise/rhi/backend_entry.hpp` declares them,
//     `utility/abi_export.hpp` owns the keyword. (This is the m03159 ruling:
//     promise is a collection of virtual base classes.)
//   - the engine-side `std::shared_ptr` wrapper: §4.1 item 3 puts it on the
//     engine's side of the line.
//   - anything that is a descriptor's CONTENT: only `buffer_desc` is fixed so far,
//     because `create_buffer()` needs something to carry (§5's capability table
//     and §6.4's usage model are the rest).
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================

export module deren.promise.rhi;

export import :contract;
export import :core_desc;
export import :extension;
export import :api_core;

/**
 * @file promise/rhi/rhi.cppm
 * @defgroup promise The Promise Contract (backend boundary)
 * @brief the abstract interfaces the engine and the backend share - a module of vtables, PODs and
 *        spans, compiled by both sides and exported by neither (RHI plan v4 §3.1, §4.1, m03159).
 *
 * The partitions that make up the contract, and the one file that is deliberately not part of it:
 *
 * | part | what it is |
 * | --- | --- |
 * | `promise/rhi/rhi.contract.cppm` | the ABI number and the `error` enum every entry point reports through |
 * | `promise/rhi/rhi.core_desc.cppm` | how the program asks for a context: the structure `deren_make_api_core()` takes |
 * | `promise/rhi/rhi.extension.cppm` | tier-2: the five abilities a backend may or may not have |
 * | `promise/rhi/rhi.api_core.cppm` | tier-1: the context, its factories, the frame calls |
 * | `promise/rhi/backend_entry.hpp` | the three `extern "C"` entry points that hand an `api_core` out (same directory, but neither a partition nor imported) |
 *
 * @details
 * - **Two tiers, one rule.** `api_core` carries only what every backend has a concept for (DX12
 *   included), and an ability a backend may lack lives in tier-2, announced as one bit in
 *   `api_core::abilities()` (§1.5, §1.8). A pass that needs an ability declares it; a backend that
 *   does not have it is a named failure, never a silent downgrade (§1.9).
 * - **Nothing here allocates, throws, or names a container** (§4.2): every interface is a vtable,
 *   every parameter is a POD, a `std::span` or an opaque handle, and every failure is a returned
 *   `error`.
 * - **A module, but not one that crosses the boundary as a symbol.** Both sides compile this
 *   interface unit into their own image, which is what keeps the engine off the DLL's import
 *   library; the only exchanged names are the three `extern "C"` entry points in
 *   `promise/rhi/backend_entry.hpp` (the header, not the module).
 * - **Ownership is spelled out, not implied.** An object made inside the backend is deleted inside
 *   the backend: the engine builds its `std::shared_ptr` around `deren_destroy_api_core`, resolved
 *   from the backend it loaded (§4.1 item 3).
 */
