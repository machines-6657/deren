// -*- C++ -*-
// ============================================================================
// file: tests/probe_backend.cpp
//
// The probe backend of tests/test_dynamic_link.cpp: the smallest stand-in for the
// real promise/rhi/ contract (the module deren.promise.rhi, RHI plan v4 §3.3 - §3.5, §4.1).
// CMake builds this one source file twice - as a DLL (with DEREN_API_SHARED +
// DEREN_API_BUILD) and as a static library (with neither) - and the test links the
// static half while opening the DLL half at run time, so both branches of
// utility/abi_export.hpp are exercised and the two halves must answer the same.
//
// This file imports the contract and includes the ABI surface instead of declaring
// either itself (it used to declare the C ABI; the declarations live in
// promise/rhi/backend_entry.hpp now), which is also the check that the header can be
// compiled by the backend side: the three entry points below are EXACTLY the ones
// the header declares, and deriving from the module's abstract classes is what makes
// the compiler verify that every virtual is implemented.
//
// What the probe implements, and why it is this small:
//
//   - `api_core::abilities()` announces device_address | descriptor_heap, and
//     `query_extension()` answers those two and returns nullptr for anything else.
//     That pair is what the plan's "named failure, no silent downgrade" rests on
//     (§1.9, §3.6): an engine that asks for ray_tracing gets a null, not a stub.
//   - The factories return nullptr except `create_buffer()`: a probe has no device,
//     so it has nothing to hand out for the rest.
//   - `create_buffer()` hands out a STATICALLY allocated probe buffer and stores the
//     descriptor's size in it. That is what makes the tier-2 path testable without
//     answering the ownership question: `device_address::buffer_address()` is asked
//     about that very object, so the test sees a POD descriptor cross into the DLL,
//     a virtual call come back out, and both halves agree. Static storage also means
//     the engine has nothing to free - releasing a resource through the backend
//     (plan §4.2) is S1's, and this round does not pretend to answer it.
//   - `probe_buffer::release()` completes the owned-handle path the contract now spells
//     (`rhi::buffer::release()` + `rhi::object_manager<T>`): there is no reference count to drop
//     on a static buffer, but the CALL has to arrive, so `release()` zeroes the size the
//     descriptor wrote. The test drives it through `object_manager` and reads `size()`
//     back - the same round-trip shape as the descriptor echo below.
//   - `frame_begin()` counts frames AND echoes the creation descriptor's `window_width`
//     in its by-value POD result, so a cross-boundary virtual call with a by-value POD
//     result is observable too - and so is the one structure `deren_make_api_core()`
//     takes: the test fills `create_info`, the probe carries the field in and hands it
//     back, and a request that never reached the library cannot produce the number.
//   - `deren_make_api_core()` takes the contract's creation descriptor
//     (`rhi::create_info const*`), refuses a mismatched ABI with `abi_mismatch` and a
//     missing descriptor with `invalid_argument`, and allocates the object with `new`
//     so that the deleter the host resolved from THIS library is the one that frees it.
// ============================================================================
import deren.promise.rhi;

#include "../promise/rhi/backend_entry.hpp"

#include <cstdint>
#include <span>

namespace {

    namespace rhi = deren::promise::rhi;

    /// The probe's buffer: a real object, statically allocated (see the banner).
    struct probe_buffer final : rhi::buffer {
        /// THE PROBE'S `release()` IS OBSERVABLE ON PURPOSE, which is what makes the owned-handle path
        /// testable at all: the buffer is statically allocated, so there is no reference count to drop,
        /// but the call still has to ARRIVE (the test releases through `object_manager`, then reads
        /// `size()` - a released probe buffer answers 0, so a `release()` that never crossed cannot pass
        /// for one that did). `size()` is the channel because it is already this object's observable
        /// field: the `create_buffer` descriptor's size goes in, `release()` takes it away.
        void release() noexcept override {
            this->released = true;
            this->size_bytes = 0;
        }

        [[nodiscard]] std::uint64_t size() const noexcept override {
            return this->size_bytes;
        }

        /// The probe's buffer is not host-visible, and the contract's answer for that is an EMPTY span
        /// rather than a promise it cannot keep (only a host-visible buffer returns bytes).
        [[nodiscard]] std::span<std::byte> mapped() noexcept override {
            return {};
        }

        std::uint64_t size_bytes = 0;
        bool released = false; ///< set by release(); the test reads it through size() == 0
    };

    /// device_address, implemented over that buffer: the answer depends on the size
    /// the descriptor carried, so the test can tell "the ability was called about
    /// this object" from "the ability was called about something else".
    ///
    /// IT HAS ONE METHOD (buffer addresses) since abi 5: the acceleration-structure half moved to
    /// `ray_tracing`, which is the only ability that can hand out that operand.
    struct probe_device_address final : rhi::device_address {
        [[nodiscard]] rhi::extension_kind kind() const noexcept override {
            return rhi::extension_kind::device_address;
        }

        [[nodiscard]] std::uint64_t buffer_address(rhi::buffer const& resource, std::uint64_t offset) const noexcept override {
            return address_base + resource.size() + offset;
        }

        static constexpr std::uint64_t address_base = 0x1000ull;
    };

    /// descriptor_heap, announced but inert: recording into a command list needs a
    /// device, and the probe's `begin_commands()` returns nullptr.
    struct probe_descriptor_heap final : rhi::descriptor_heap {
        [[nodiscard]] rhi::extension_kind kind() const noexcept override {
            return rhi::extension_kind::descriptor_heap;
        }

        void push_data(rhi::command_list&, std::span<std::byte const>) override {
        }
    };

    /// The probe's api_core. `final` so that a missing override is a compile error
    /// rather than an inherited pure virtual in an abstract class nobody notices.
    struct impl final : rhi::api_core {
        [[nodiscard]] rhi::ability_bits abilities() const noexcept override {
            return rhi::to_bits(rhi::extension_kind::device_address) |
                   rhi::to_bits(rhi::extension_kind::descriptor_heap);
        }

        [[nodiscard]] rhi::extension* query_extension(rhi::extension_kind kind) noexcept override {
            switch (kind) {
            case rhi::extension_kind::device_address:
                return &this->address;
            case rhi::extension_kind::descriptor_heap:
                return &this->heap;
            default:
                return nullptr; // not announced, so not available (§3.6)
            }
        }

        [[nodiscard]] rhi::swapchain* create_swapchain(rhi::swapchain_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::buffer* create_buffer(rhi::buffer_desc const& desc) override {
            this->buffer.size_bytes = desc.size;
            return &this->buffer;
        }

        [[nodiscard]] rhi::image* create_image(rhi::image_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::sampler* create_sampler(rhi::sampler_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::shader* create_shader(rhi::shader_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::pipeline* create_pipeline(rhi::pipeline_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::query* create_query(rhi::query_desc const&) override {
            return nullptr;
        }

        [[nodiscard]] rhi::command_list* begin_commands() override {
            return nullptr; // no device, no pool: the test checks this is an answer, not a crash
        }

        [[nodiscard]] rhi::submit_info frame_begin() override {
            rhi::submit_info info{};
            info.frame_index = this->frames_handed_out;
            // THE ECHO OF THE CREATION DESCRIPTOR (see deren_make_api_core below): `submit_info` is the
            // one by-value POD result the contract returns, so the field the test filled is the field
            // the test reads back - a descriptor that never crossed cannot produce this number.
            info.image_index = this->creation_window_width;
            ++this->frames_handed_out;
            return info;
        }

        void present() override {
            ++this->presents;
        }

        void wait_idle() override {
            ++this->waits;
        }

        /// NO DEVICE, NO SWAPCHAIN IMAGE, NO READ-BACK SLOT: the probe's honest answer to the frame
        /// getters is nullptr, the same shape as its `begin_commands()`.
        [[nodiscard]] rhi::image* frame_image() noexcept override {
            return nullptr;
        }

        [[nodiscard]] rhi::buffer* frame_readback_buffer() noexcept override {
            return nullptr;
        }

        probe_buffer buffer{};
        probe_device_address address{};
        probe_descriptor_heap heap{};
        std::uint32_t frames_handed_out = 7; // deliberately not 0: a default would hide a lost write
        /// the creation descriptor's `window_width`, carried in by deren_make_api_core and echoed out
        /// of frame_begin(); 0 would be an impl nobody filled, which the test's non-zero fill catches
        std::uint32_t creation_window_width = 0;
        std::uint32_t presents = 0;
        std::uint32_t waits = 0;
    };

} // namespace

extern "C" DEREN_API_EXPORT std::uint32_t deren_abi_version() {
    return deren::promise::rhi::abi_version;
}

extern "C" DEREN_API_EXPORT deren::promise::rhi::api_core* deren_make_api_core(std::uint32_t abi_version,
                                                                               deren::promise::rhi::create_info const* desc,
                                                                               deren::promise::rhi::error* out_error) {
    if (abi_version != deren::promise::rhi::abi_version) {
        if (out_error != nullptr) {
            *out_error = deren::promise::rhi::error::abi_mismatch;
        }
        return nullptr;
    }
    // "no creation parameters" is a caller bug, not a request for the standard context: the contract's
    // own `create_info{}` is how that is spelled (see backend_entry.hpp).
    if (desc == nullptr) {
        if (out_error != nullptr) {
            *out_error = deren::promise::rhi::error::invalid_argument;
        }
        return nullptr;
    }
    if (out_error != nullptr) {
        *out_error = deren::promise::rhi::error::ok;
    }
    // THE CREATION DESCRIPTOR IS ECHOED BACK THROUGH AN EXISTING CALL. The probe has no device, so it
    // cannot honour the descriptor's window / vsync / render-scale fields; what it CAN do - and what
    // makes the crossing observable - is carry `window_width` into the one by-value POD result the
    // contract returns: `frame_begin()`'s `submit_info.image_index`. The test fills the descriptor and
    // reads the number back, so a request that never reached the library cannot pass for one that did.
    auto* const created = new impl();
    created->creation_window_width = static_cast<std::uint32_t>(desc->window_width);
    return created;
}

extern "C" DEREN_API_EXPORT void deren_destroy_api_core(deren::promise::rhi::api_core* core) {
    delete core;
}
