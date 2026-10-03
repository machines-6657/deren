// ============================================================================
// module: deren.vulkan.runtime:readback  - the screenshot read-back, READ half
//
// THE READ HALF OF THE SCREENSHOT, AND NOTHING ELSE. Since S2 batch 2 the COPY is recorded through the
// promise contract's recording surface, inside the frame it belongs to (see end_recording in
// runtime.frames.cppm), and it writes into the BACKEND's read-back slot. What is left here is what
// happens after the frame lands:
//
//   acquire_current_frame_image waits for the device, reads the bytes the backend mapped for us and
//   unpacks them (the swapchain's BGRA byte order becomes the PNG writer's RGBA);
//   consume_screenshot_request is the one-shot flag the caller reads.
//
// THE ENGINE TOUCHES NO VULKAN HANDLE HERE ANY MORE - that is the point of the migration: the image,
// its extent and its format come from the contract's frame image, the bytes come from the contract's
// buffer view, and the wait is the contract's context call. What stays the engine's is the DECISION:
// which bytes to unpack, in which order, and what to say when the format cannot be described.
//
// Imports are NOT transitive: this partition imports what its own code calls.
// ============================================================================
module;

#include <cstddef>
#include <cstdint>
#include <cstring>  // std::memcpy of an RGBA frame
#include <expected> // std::unexpected for the failure strings
#include <span>     // the bytes the contract's buffer view hands back
#include <string>
#include <vector>

module deren.vulkan.runtime:readback;

import :declarations;

import deren.promise.rhi;

namespace deren::vulkan {
    std::expected<runtime::frame_image, std::string> runtime::acquire_current_frame_image() {
        // NO `screenshot_pending` CHECK HERE, AND THAT IS A MEASURED DECISION rather than an omission:
        // the one caller is `if (consume_screenshot_request()) { acquire_current_frame_image(); }`
        // (main.cpp), and consume_screenshot_request() CLEARS the flag while answering it - so a check
        // here fired on every SUCCESSFUL capture and reported "no captured frame" for a frame that had
        // just been captured (measured: 14/14 scenarios came back "no screenshot produced"). What is
        // checked instead is what actually has to hold: a frame image with an extent, and a backend slot
        // that is mapped and large enough.
        core& device = this->vulkan_core;
        // THE SAME IMAGE AND THE SAME SLOT THE COPY USED. `frame_image()` answers for the image the last
        // acquire returned - the copy was recorded into THAT frame's command buffer, and by the time the
        // caller asks the frame has been submitted - and the read-back slot is the backend's own,
        // host-visible buffer.
        deren::promise::rhi::image* const image = device.frame_image();
        deren::promise::rhi::buffer* const slot = device.frame_readback_buffer();
        if (image == nullptr || slot == nullptr) {
            return std::unexpected(std::string("screenshot: the backend has no frame image or read-back slot"));
        }
        deren::promise::rhi::image_extent const extent = image->extent();
        if (extent.width == 0 || extent.height == 0) {
            return std::unexpected(std::string("screenshot: the frame image has no extent"));
        }
        // The format the backend names for the frame image decides how the bytes are unpacked. A format
        // the contract cannot describe is the "unsupported swapchain format" error this path always had.
        deren::promise::rhi::image_format const format = image->format();
        bool const bgra = format == deren::promise::rhi::image_format::bgra8_srgb || format == deren::promise::rhi::image_format::bgra8_unorm;
        bool const rgba = format == deren::promise::rhi::image_format::rgba8_srgb || format == deren::promise::rhi::image_format::rgba8_unorm;
        if (!bgra && !rgba) {
            return std::unexpected(std::string("screenshot: unsupported swapchain format (need 8-bit RGBA/BGRA)"));
        }

        // ONE WAIT IS ALL THAT IS LEFT. The copy was recorded INSIDE the submitted frame, so the bytes
        // are on the device until that frame completes; the contract's buffer view carries no fence, so
        // the caller's pacing - this wait, which is the context's own - is the ordering. The same rule
        // the contract states for `buffer::mapped()`.
        device.wait_idle();

        std::size_t const buffer_size = static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height) * 4u;
        std::span<std::byte> const mapped = slot->mapped();
        if (mapped.empty() || mapped.size() < buffer_size) {
            return std::unexpected(std::string("screenshot: read-back slot unavailable or too small"));
        }

        frame_image result = {};
        result.width = extent.width;
        result.height = extent.height;
        result.rgba.resize(buffer_size);
        auto const* source = reinterpret_cast<std::uint8_t const*>(mapped.data());
        if (bgra) {
            // the swapchain is BGRA (sRGB); the PNG writer wants RGBA
            for (std::size_t i = 0; i < result.rgba.size(); i += 4) {
                result.rgba[i + 0] = source[i + 2]; // R
                result.rgba[i + 1] = source[i + 1]; // G
                result.rgba[i + 2] = source[i + 0]; // B
                result.rgba[i + 3] = source[i + 3]; // A
            }
        } else {
            std::memcpy(result.rgba.data(), source, buffer_size);
        }
        return result;
    }

    bool runtime::consume_screenshot_request() noexcept {
        // Could the requested frame be captured? Single-shot by design: the flag is cleared HERE, on
        // success and on failure alike. A failing read-back (unsupported swapchain format, missing
        // read-back buffer) used to leave the flag set, so main's loop called
        // acquire_current_frame_image() - which begins with a device-wide wait - and logged an error
        // every single frame until exit. A dropped capture is the correct outcome; one F12 is one
        // attempt.
        bool const captured = this->screenshot_pending;
        this->screenshot_pending = false;
        return captured;
    }
} // namespace deren::vulkan
