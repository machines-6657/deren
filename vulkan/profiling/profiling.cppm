// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file vulkan/profiling/profiling.cppm
 * @defgroup vulkan_profiling Frame Timing Instrumentation
 * @brief Frame timing instrumentation: the CPU frame phases, measured per frame and reported as a
 *        60-frame window (see cpu_phases), with the RAII scope timer the pass recorders use.
 *
 * Extracted from deren.vulkan.runtime, whose implementation had grown past 4900 lines with the timing, the
 * pass recorders, the pipelines and the resources all in one file. It is a module rather than an
 * implementation partition because the timing state is legitimately useful on its own: an overlay, a
 * test or another layer can read cpu_phases without going through the runtime facade. The GPU half
 * (the per-pass timestamps) follows in a second step.
 */

module;

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

export module deren.vulkan.profiling;

import deren.utility; // the completed window is logged once per fold

namespace deren::vulkan::profiling {
    /**
     * @brief the CPU frame phases that are measured per frame
     * @ingroup vulkan_profiling
     *
     * A phase whose name ends in '*' is a SUB-phase: it is measured inside another phase, so it is
     * reported but deliberately left out of the reported total (adding it would count that time twice).
     */
    export enum class cpu_phase : uint32_t {
        pace,
        begin,
        scene,
        cluster, // sub-phase of scene
        shadow,  // sub-phase of scene
        post,
        submit,
        submit_queue, // sub-phase of submit
        present,      // sub-phase of submit
        count,
    };

    /// phase names in cpu_phase order; the trailing '*' marks a sub-phase (see cpu_phase)
    export constexpr std::array<std::string_view, static_cast<std::size_t>(cpu_phase::count)> cpu_phase_names = {
        "pace", "begin", "scene", "cluster*", "shadow*", "post", "submit", "submit-queue*", "present*"};

    /**
     * @brief the rolling window of measured CPU frame phases
     * @ingroup vulkan_profiling
     *
     * add()/end() accumulate the frame being measured; fold() closes the window every @c window_length
     * frames and publishes it as a label, because a per-frame running mean makes an overlay re-wrap its
     * text every frame (the label is only as wide as its widest value). The label is what both the log
     * and the overlay read, so it holds the last COMPLETED window rather than a partially filled one.
     */
    export class cpu_phases {
    public:
        static constexpr uint32_t window_length = 60;

        /// "[render] gpu_timings" also gates the CPU phases: one switch for "measure this frame"
        void set_enabled(bool const on) noexcept {
            this->enabled = on;
        }

        /// accumulate one phase of the frame being measured (not gated, exactly like the runtime was)
        void add(cpu_phase const phase, std::chrono::steady_clock::duration const elapsed) noexcept {
            std::size_t const index = static_cast<std::size_t>(phase);
            if (index >= this->frame.size()) {
                return;
            }
            this->frame[index] += std::chrono::duration<double, std::milli>(elapsed).count();
        }

        /// end a phase; ending @c submit folds the frame into the window and reports a completed one
        void end(cpu_phase const phase, std::chrono::steady_clock::time_point const start) noexcept {
            if (!this->enabled) {
                return;
            }
            std::size_t const index = static_cast<std::size_t>(phase);
            if (index >= this->frame.size()) {
                return;
            }
            this->frame[index] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (phase != cpu_phase::submit) {
                return; // one fold per frame, at the phase that ends it
            }
            // The frame completed: fold it into the window (a frame that never reaches submit - a
            // minimized window, a failed acquire - keeps no partial numbers), and report once per
            // window. The report goes to the log; the compact label is what the overlay shows, and it
            // only changes when a window completes, so the overlay text cannot twitch every frame.
            for (std::size_t i = 0; i < this->sum.size(); ++i) {
                this->sum[i] += this->frame[i];
                this->frame[i] = 0.0;
            }
            if (++this->window_frames < window_length) {
                return;
            }
            std::string report = std::format("cpu frame phases (avg of {} frames):", window_length);
            std::string label = std::format("cpu ({}f):", window_length);
            double total = 0.0;
            for (std::size_t i = 0; i < this->sum.size(); ++i) {
                double const mean = this->sum[i] / static_cast<double>(window_length);
                report += std::format(" {} {:.2f} ms |", cpu_phase_names[i], mean);
                label += std::format("{} {:>5.2f}", i == 0 ? "" : " |", mean);
                // Sub-phases (names ending in '*') are measured INSIDE scene: adding them to the total
                // again would count that time twice.
                if (cpu_phase_names[i].back() != '*') {
                    total += mean;
                }
                this->sum[i] = 0.0;
            }
            this->window_frames = 0;
            report += std::format(" total {:.2f} ms", total);
            this->report_label = label + std::format("\n     total {:>5.2f} ms", total);
            deren::utility::log("{}", report);
        }

        [[nodiscard]] std::string summary() const {
            if (!this->enabled) {
                return "cpu timings: off ([render] gpu_timings = false)";
            }
            if (this->report_label.empty()) {
                return "cpu: collecting...";
            }
            return this->report_label;
        }

        [[nodiscard]] std::array<double, static_cast<std::size_t>(cpu_phase::count)> means() const noexcept {
            std::array<double, static_cast<std::size_t>(cpu_phase::count)> result = {};
            for (std::size_t i = 0; i < result.size(); ++i) {
                result[i] = this->sum[i] / static_cast<double>(this->window_frames == 0 ? window_length : this->window_frames);
            }
            return result;
        }

    private:
        bool enabled = true;
        std::array<double, static_cast<std::size_t>(cpu_phase::count)> frame = {}; // the frame being measured
        std::array<double, static_cast<std::size_t>(cpu_phase::count)> sum = {};   // the window being accumulated
        uint32_t window_frames = 0;
        std::string report_label = {};
    };

    /**
     * @brief scope timer: measures one CPU phase from construction to destruction
     * @ingroup vulkan_profiling
     */
    export class cpu_phase_timer {
    public:
        cpu_phase_timer(cpu_phases& target, cpu_phase const which) noexcept
            : phases{&target}
            , phase{which}
            , start{std::chrono::steady_clock::now()} {
        }

        ~cpu_phase_timer() {
            this->phases->end(this->phase, this->start);
        }

        cpu_phase_timer(cpu_phase_timer const&) = delete;
        cpu_phase_timer& operator=(cpu_phase_timer const&) = delete;
        cpu_phase_timer(cpu_phase_timer&&) = delete;
        cpu_phase_timer& operator=(cpu_phase_timer&&) = delete;

    private:
        cpu_phases* phases = nullptr;
        cpu_phase phase = cpu_phase::count;
        std::chrono::steady_clock::time_point start = {};
    };
} // namespace deren::vulkan::profiling