module;

#include <cstdint> // ::uint64_t (used unqualified below)

export module deren.utility:frame_clock;

import deren.vstd;

/**
 * @ingroup utility
 * @defgroup frame_clock Frame Clock
 * @brief per-frame cheap time: one writer thread stamps a couple of atomics each frame, any
 *        number of reader threads get the time as an atomic load instead of calling the clock
 *        directly (steady_clock::now() costs tens of ns per call, an atomic read ~1 ns). The
 *        stamping writer is deliberately the frame/update thread itself - a dedicated sleeper
 *        thread would lag by the OS timer granularity (~15.6 ms on Windows) - so readers are
 *        at most one frame behind.
 *
 * Typical use: the render (or game/update) thread calls stamp() once per frame, parallel
 * workers / animation code read last_ns() or delta_ns() anywhere else. The clock is
 * monotonic with an implementation-defined origin (steady_clock since its epoch).
 */
namespace deren::utility {
    /**
     * @ingroup frame_clock
     * @brief monotonic per-frame clock with single-writer stamps and cheap multi-reader reads
     * @note
     *     - stamp() must only be called from ONE thread (the frame owner); every other method
     *       is safe to call from any thread at any time (relaxed atomic loads)
     *     - last_ns()/delta_ns() are raw nanoseconds since the steady clock's origin; convert
     *       with delta_seconds()/last_seconds() or read now_ns() for a true 'right now'
     *     - readers may observe last_ns() ahead of delta_ns()'s matching value by one stamp:
     *       values are individually consistent, not a lock-free snapshot pair
     */
    export class frame_clock {
    public:
        /** @brief record the current steady time as the new frame stamp (frame owner thread) */
        void stamp() noexcept {
            uint64_t const now = now_ns();
            uint64_t const previous = this->last_ns_value.load(std::memory_order_relaxed);
            // First stamp (previous == 0, the clock's zero value): there is no previous frame,
            // so the delta is 0 - NOT now - 0, which would be the machine's uptime and make the
            // first delta_seconds() jump the animation to a random phase.
            uint64_t const delta = (previous == 0) ? 0 : now - previous;
            this->delta_ns_value.store(delta, std::memory_order_relaxed);
            this->last_ns_value.store(now, std::memory_order_relaxed);
        }

        /** @brief nanoseconds (steady origin) of the most recent stamp */
        [[nodiscard]] uint64_t last_ns() const noexcept {
            return this->last_ns_value.load(std::memory_order_relaxed);
        }

        /** @brief nanoseconds elapsed between the two most recent stamps (0 until the second one) */
        [[nodiscard]] uint64_t delta_ns() const noexcept {
            return this->delta_ns_value.load(std::memory_order_relaxed);
        }

        /** @brief seconds of the most recent stamp (float; convenience) */
        [[nodiscard]] double last_seconds() const noexcept {
            return static_cast<double>(this->last_ns()) * 1e-9;
        }

        /** @brief seconds elapsed between the two most recent stamps (float; convenience) */
        [[nodiscard]] double delta_seconds() const noexcept {
            return static_cast<double>(this->delta_ns()) * 1e-9;
        }

        /** @brief true 'right now' in the same steady-clock scale (baseline / calibration) */
        [[nodiscard]] static uint64_t now_ns() noexcept {
            return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        }

    private:
        // called last_ns_value, not last_ns: the last_ns() method of this class would collide with a member of that name
        std::atomic<uint64_t> last_ns_value{0};
        // called delta_ns_value, not delta_ns: the delta_ns() method of this class would collide with a member of that name
        std::atomic<uint64_t> delta_ns_value{0};
    };
} // namespace deren::utility
