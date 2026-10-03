// -*- C++ -*-
// ============================================================================
// module: deren.promise.rhi:core_desc
//
// HOW A BACKEND CONTEXT IS CREATED, AS THE CONTRACT'S ONE CREATION STRUCTURE (plan_rhi_v4.md §1.11,
// §4.1 item 3): the program fills `create_info` and hands it to `deren_make_api_core()`; the backend
// builds its context from it.
//
// ONE STRUCTURE, NOT TWO. This type is the whole of the creation descriptor: the backend has no
// second, Vulkan-side spelling of it and therefore no translation function between the two. It used
// to have exactly that pair - `deren::vulkan::core_create_info` plus a `to_backend_create_info()` -
// and the arrangement defeated the reason this type exists:
//
//   - the entry is the ABI, so everything the entry needs must be a type BOTH SIDES compile. The
//     backend's own creation structure could not be that: it is a Vulkan-side detail that changes
//     with the backend, and putting it in the entry would make the entry depend on one backend. Two
//     spellings of one thing also means two places to add the next field and a translation to keep in
//     step with both - the "no two mechanisms in one file" rule, one level up.
//   - the program is the side that knows what the user asked for (window size, title, vsync, render
//     scale, validation layers), so the program is the side that fills it.
//
// WHAT MAY APPEAR IN IT, and what may not (§4.2's rule for everything that crosses): PODs, spans and
// opaque handles. In particular:
//
//   - the title is `char const*` (UTF-8, NUL-terminated) and NOT `std::string`: a `std::string` here
//     would make "same standard library, same allocator" a premise of the boundary, and the backend
//     copies the text into the window system's own storage at construction anyway. It is BORROWED
//     ONLY UNTIL THE CALL RETURNS - a caller passing a temporary's `.c_str()` is the bug this note
//     exists to prevent (main.cpp spells the valid shape: the text outlives the call).
//   - a caller-provided window is `void*` and not `GLFWwindow*`: the contract does not know what a
//     window system is (the Vulkan backend reinterprets it; a DX12 backend would read an HWND from
//     the same field). Its lifetime belongs to the caller.
//   - a `float` is passed as `float` (the render scale is compared and multiplied in the backend, not
//     accumulated), so no fixed-point spelling is needed.
//
// EVOLUTION: `struct_size` is the first member and defaults to this type's size, so a newer program
// talking to an older backend (or the reverse) can be detected instead of silently mis-read; the
// fields are only ever APPENDED, never reordered, and a shape change that cannot be appended gets a
// new name (`create_info_v2`) announced through the ABI number - the same rule the three entry
// points follow (§3.4(3)). The backend applies the guard by copying a field only when its whole
// extent lies inside the bytes the caller declares (core.constructor.cppm's `sanitize_create_info`).
//
// WHAT IS DELIBERATELY NOT HERE: the backend's defaults for anything the program leaves at zero, and
// any per-backend knob (a Vulkan-only feature is tier-2's business, not the context's).
// ============================================================================
module;

#include <cstdint>

export module deren.promise.rhi:core_desc;

export namespace deren::promise::rhi {

    /**
     * @ingroup promise
     * @brief how the program asks for a backend context: window geometry and title, presentation and
     *        debug choices, and the render scale.
     *
     * @details
     * - Every field has a default, so `create_info{}` is a valid "give me the standard context".
     * - The program fills it once, at startup, and hands it to `deren_make_api_core()`, whose pointer
     *   parameter is this type. It is THE creation descriptor: there is no backend-side twin and no
     *   translation step, because the backend's constructor takes this structure directly.
     * - `native_window` is borrowed, never owned: when it is not null the backend binds to that window
     *   and does not create or destroy one, and the caller has to keep it alive for as long as the
     *   context lives. When it is null the backend creates the window from the fields above.
     */
    struct create_info {
        /// size of this structure as the CALLER compiled it (ABI guard, see the banner)
        std::uint32_t struct_size = sizeof(create_info);
        /// window title, UTF-8 and NUL-terminated; borrowed only until the call returns
        char const* window_title = "deren";
        std::int32_t window_width = 1080;
        std::int32_t window_height = 960;
        /// the resolution the rendering chain runs at, as a fraction of the swapchain extent: the
        /// backend CLAMPS it rather than rejecting it (the Vulkan backend to [0.1, 1.0]), so a 16:9
        /// window can shade fewer pixels and upscale. A value outside the backend's range is not an
        /// error and not a request: the backend's answer is what the frame uses.
        float render_scale = 1.0f;
        /// true prefers FIFO (the display's rate, no tearing); false prefers the uncapped present mode
        bool vsync = true;
        /// ask for the validation layers: a debug aid, not a quality knob
        bool validation_layers = false;
        /// whether a window the backend creates itself is shown; ignored when `native_window` is set
        bool window_visible = true;
        /// a window the CALLER owns (an HWND/GLFWwindow*/whatever the backend understands), or null to
        /// have the backend create one. Borrowed, never freed by the backend.
        void* native_window = nullptr;
    };

} // namespace deren::promise::rhi
