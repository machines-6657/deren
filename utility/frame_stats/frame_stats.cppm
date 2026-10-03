module;

#include <cstdint>

export module deren.utility:frame_stats;

import deren.vstd;

/**
 * @ingroup utility
 * @defgroup frame_stats Frame Statistics
 * @file frame_stats.cppm
 * @brief per-frame FPS statistics with a rolling report window (deren::utility::frame_stats)
 *
 * The render loop calls tick() once per actually-presented frame; the class accumulates a
 * one-second window of frame-gap times and exposes both the running smoothed value (for a
 * per-frame overlay label) and the completed window's value (for the once-per-second log
 * line). Minimized / swapchain-recreate iterations call on_skipped() instead of tick(), so
 * pauses never distort the window - the frame-gap baseline is refreshed without counting.
 *
 * Typical use (the demo's render loop):
 * @code {.cpp}
 * deren::utility::frame_stats stats;
 * while (...) {
 *     if (frame skipped) { stats.on_skipped(); continue; }
 *     // ... frame phases ...
 *     if (frame presented) {
 *         stats.tick();
 *         if (stats.window_rolled()) {
 *             deren::utility::log("fps: {:.1f} ({:.2f} ms/frame)",
 *                          stats.window_fps(), stats.window_frame_ms());
 *         }
 *     }
 * }
 * @endcode
 */
namespace deren::utility {
    /**
     * @ingroup frame_stats
     * @brief rolling FPS statistics over a report window (one second by default)
     * @note single-threaded by design: tick()/on_skipped() run on the frame owner thread only
     */
    export class frame_stats {
    public:
        /**
         * @brief configure the report window
         * @param window_seconds window length; tick() reports when the window fills
         */
        explicit frame_stats(double window_seconds = 1.0) noexcept
            : window_length{window_seconds} {
        }

        /**
         * @brief record one presented frame: add its frame gap, count it, and roll the window
         *        over when the accumulated time reaches the window length
         * @note call ONLY for real frames; skipped iterations must call on_skipped() instead
         */
        void tick() noexcept {
            auto const now = std::chrono::steady_clock::now();
            if (this->last_frame.time_since_epoch().count() == 0) {
                this->last_frame = now; // first tick: no gap yet, start the baseline
            } else {
                this->window_elapsed += std::chrono::duration<double>(now - this->last_frame).count();
                this->last_frame = now;
                this->window_frames += 1;
            }
            if (this->window_elapsed >= this->window_length && this->window_frames > 0) {
                // window filled: publish its final numbers (same formula the overlay showed
                // live), then start a fresh window
                this->last_window_fps = static_cast<double>(this->window_frames) / this->window_elapsed;
                this->last_window_frame_ms = 1000.0 * this->window_elapsed / static_cast<double>(this->window_frames);
                this->window_elapsed = 0.0;
                this->window_frames = 0;
                this->rolled = true;
            } else {
                this->rolled = false;
            }
        }

        /**
         * @brief a frame was skipped (minimized / swapchain recreation): refresh the frame-gap
         *        baseline without counting a frame, so the pause never inflates the window
         */
        void on_skipped() noexcept {
            this->last_frame = std::chrono::steady_clock::now();
            this->rolled = false;
        }

        /**
         * @brief whether the report window just filled on the last tick(); the caller runs its
         *        once-per-second reporting (log line) when this is true
         */
        [[nodiscard]] bool window_rolled() const noexcept {
            return this->rolled;
        }

        /**
         * @brief running smoothed fps of the current window (live value for a per-frame
         *        overlay label); equals the completed window's fps right after a rollover
         */
        [[nodiscard]] double smoothed_fps() const noexcept {
            return this->window_elapsed > 0.0 ? static_cast<double>(this->window_frames) / this->window_elapsed : 0.0;
        }

        /** @brief fps of the last completed window (0.0 before the first rollover) */
        [[nodiscard]] double window_fps() const noexcept {
            return this->last_window_fps;
        }

        /** @brief milliseconds per frame of the last completed window (0.0 before the first rollover) */
        [[nodiscard]] double window_frame_ms() const noexcept {
            return this->last_window_frame_ms;
        }

    private:
        // called window_length, not window_seconds: the window_seconds parameter of the constructor would hide a
        // member of that name and MSVC /W4 reports C4458, an error under /WX
        double window_length = 1.0;                         // rolling-window length
        std::chrono::steady_clock::time_point last_frame{}; // zero = no frame ticked yet
        double window_elapsed = 0.0;                        // summed frame gaps in this window
        uint32_t window_frames = 0;                         // frames counted in this window
        bool rolled = false;                                // set when the last tick() filled a window
        double last_window_fps = 0.0;                       // fps of the last completed window; called last_window_fps, not window_fps: the window_fps() method of this class would collide with a member of that name
        double last_window_frame_ms = 0.0;                  // ms/frame of the last completed window; called last_window_frame_ms, not window_frame_ms: the window_frame_ms() method of this class would collide with a member of that name
    };
} // namespace deren::utility
