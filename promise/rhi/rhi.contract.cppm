// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:contract
//
// The two pieces every other partition of deren.promise.rhi needs: the ABI number the
// backend and the engine compare, and the error enum that turns a failure into a
// return value instead of an exception (RHI plan v4, §4.1 item 3 and §4.2).
//
// `deren::promise::rhi::abi_version` is the compile-time constant whose twin is the
// backend's `deren_abi_version()`. Plan §8.1 row 4 makes the comparison a tested
// check rather than a convention: the loader resolves the symbol and the test
// asserts the returned number equals this constant. The value is part of the C
// ABI: it moves only when the shape of `api_core` or of a tier-2 ability changes
// in a way an older engine could not survive.
//
// Nothing here allocates, throws, or names a container. Both sides of the boundary
// compile this partition (plan §4.1 item 1); neither exports a symbol for it.
// ============================================================================
module;

#include <cstdint>

export module deren.promise.rhi:contract;

/**
 * @file promise/rhi/rhi.contract.cppm
 * @brief the ABI number the backend and the engine compare, and the `error` enum every promise entry
 *        point reports through.
 * @ingroup promise
 *
 * Both are part of the C ABI rather than of any implementation: `abi_version` is the number
 * `deren_abi_version()` returns and `deren_make_api_core()` refuses to build against, and `error`
 * is what a failure travels in because an exception must not cross the boundary (§4.2).
 * Deliberately the smallest partition in the contract: both sides compile it, so anything added here
 * is added to both compilations at once.
 */

export namespace deren::promise::rhi {

    /// The C ABI number of this build of the contract.
    ///
    /// `deren_abi_version()` in the backend returns it and `deren_make_api_core()`
    /// refuses any other value, which is the "engine and backend disagree" failure
    /// path the skeleton keeps testable on purpose (plan §4.2).
    ///
    /// 1 -> 2 in the recording-surface batch: SIX virtuals were added to EXISTING tier-1 types
    /// (`api_core::frame_image`/`frame_readback_buffer`, `image::format`, `buffer::mapped`,
    /// `command_list::use`/`copy_image_to_buffer`), which shifts the vtable every caller reaches
    /// through. Appending `error` values or adding a NEW interface does not move this number; a
    /// virtual on an existing type does. FROM S2 ON THIS IS STRICTER: the DLL ABI promise is live
    /// then, so a renumbering breaks binaries in the field rather than only recompiling this tree.
    ///
    /// 2 -> 3 in the one-creation-structure batch: `deren_make_api_core()` grew the parameter it was
    /// always described as taking - a `deren::promise::rhi::create_info const*` - and the contract's
    /// creation structure is now the ONLY one (the backend's `vulkan::core_create_info` twin and the
    /// `to_backend_create_info()` translation between them are gone). No vtable moved, but the C
    /// entry's SIGNATURE did: an engine built for 2 calls that symbol with two arguments and would
    /// have its third read from whatever the stack held, which is exactly the failure this number
    /// exists to catch before anything is allocated (the mismatch is refused with `error::abi_mismatch`).
    ///
    /// 3 -> 4 in the owned-handle batch: `release()` landed as a pure virtual on SEVEN existing tier-1
    /// types (`buffer`, `image`, `sampler`, `shader`, `pipeline`, `swapchain`, `query`), which shifts the
    /// vtable every caller reaches through - the same reason 1 -> 2 moved the number. It is an ADD to
    /// existing types, not a new interface, so it is exactly the case the rule above names.
    ///
    /// A RENAME IN PLACE DOES NOT MOVE IT, and that is worth spelling out where the number is defined:
    /// the entry was briefly called `destroy()`, was renamed to `release()` to say "this drops ONE
    /// reference; the resource may be shared/reused and therefore outlive the call", and the number
    /// stayed 4. The SLOT is unchanged (same position, same order), only the symbol name is, so a build
    /// compiled against one spelling and a build compiled against the other remain binary compatible -
    /// the rule is about the SHAPE of the vtables, not about their spelling.
    /// 4 -> 5 in the addressable-buffer batch: `acceleration_structure_address` MOVED from the
    /// `device_address` ability to `ray_tracing`, where the only ability that can produce its operand
    /// lives. Both tier-2 abilities change shape (one loses a virtual, the other gains one), which is
    /// exactly the case the rule above names: an engine built for 4 would dispatch those slots
    /// differently. The move is what lets `device_address` be announced at all - while it carried an
    /// operand no backend could produce yet, announcing it was forbidden by the ability's own rule.
    /// 5 -> 6 when `vulkan_escape` gained `native_buffer`: a virtual APPENDED to an existing tier-2
    /// ability. Appending keeps the slots already there where they were, but the vtable every caller
    /// reaches through is a different shape, which is the case this number exists for. It is the call
    /// that lets an escaping pass use a contract buffer's raw handle instead of the allocator's detail
    /// map, i.e. one more way for the engine to stop reaching into the backend.
    inline constexpr std::uint32_t abi_version = 6u;

    /// Why a promise entry point could not do what it was asked.
    ///
    /// An exception must not cross the boundary (plan §4.2), so every failure a
    /// backend can report travels as one of these through an out-parameter.
    ///
    /// `abi_mismatch` = 7 is not a system error number: it is the code the minimal
    /// use case measured in plan §10.3 reports, and it is kept here so that the
    /// refusal stays observable from the engine side.
    enum class error : std::uint32_t {
        ok = 0,               ///< the call did what it was asked
        abi_mismatch = 7,     ///< the caller's abi_version is not the backend's
        unsupported = 8,      ///< the backend has no mechanism that can serve THIS resource/format
        invalid_argument = 9, ///< the region does not fit the image, or the destination is too small
        not_ready = 10,       ///< no frame is in flight, or the frame that drew it is not done
        device_lost = 11,     ///< the device refused the submission/copy (VkResult failure)
    };

} // namespace deren::promise::rhi
