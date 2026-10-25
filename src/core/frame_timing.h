// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "common/common_types.h"

namespace FileUtil {
class IOFile;
}

namespace Core {

/// Timing samples (microseconds) of one reporting period, with nearest-rank percentiles.
class TimingSamples {
public:
    void Add(s64 us);
    /// Sorts the samples in place. Returns 0 when there are none.
    s64 Percentile(int pct);
    std::size_t Count() const {
        return samples.size();
    }
    void Clear() {
        samples.clear();
    }

private:
    std::vector<s64> samples;
};

/**
 * Offsets (us) of events from a reference clock over one reporting period. The spread is taken
 * around a straight line fitted through them, so a steady rate difference (the speed limit, or a
 * pacing hook following a display clock) does not count as jitter.
 */
class OffsetSeries {
public:
    void Add(s64 t_us, s64 offset_us) {
        points.emplace_back(t_us, offset_us);
    }
    std::size_t Count() const {
        return points.size();
    }
    void Clear() {
        points.clear();
    }
    /// "-a/+b": p1 and p99 of the residuals around their median, in ms.
    std::string Spread() const;

private:
    std::vector<std::pair<s64, s64>> points;
};

/**
 * Frame pacing log of the emulation thread (Magic Stream, phase 0). FrameLimiter reports every
 * system frame; every REPORT_FRAMES frames one summary line goes to the log. With
 * Settings::values.record_frame_times every frame is also written to
 * <log dir>/frame_timing_<date>.csv.
 *
 * "Submit" is the moment the frame limiter is entered. "Present" is when the main window's frame
 * is handed to the presentation engine, which is when a streaming encoder can take it: by
 * default it comes before the limiter's wait (so its spread follows the spread of the work), with
 * Settings::values.present_on_deadline after it.
 */
class FrameTimingLog {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t REPORT_FRAMES = 600;

    FrameTimingLog();
    ~FrameTimingLog();

    /**
     * @param submit when the frame was handed over for presentation
     * @param emulated_us emulated time at the end of the frame
     * @param speed_scale frame limit as a fraction (1.0 = 100%)
     * @param sleep_requested_us how long the limiter decided to wait (0 or less: no wait)
     * @param wake when the limiter returned to emulation
     */
    void Record(Clock::time_point submit, s64 emulated_us, double speed_scale,
                s64 sleep_requested_us, Clock::time_point wake);

    /// @param emulated_us emulated time at the end of the presented frame
    void RecordPresent(Clock::time_point present, s64 emulated_us, double speed_scale);

private:
    void Report();
    void Clear();
    /// Adds walltime minus the emulated clock since the start of the period (us)
    void AddOffset(OffsetSeries& series, Clock::time_point t, s64 emulated_us,
                   double speed_scale) const;
    void WriteCsv();

    struct Row {
        s64 submit_us;
        s64 emulated_us;
        s64 work_us;
        s64 sleep_requested_us;
        s64 sleep_us;
    };

    bool started{};
    Clock::time_point origin;
    Clock::time_point last_submit;
    Clock::time_point last_wake;
    s64 origin_emulated_us{};
    s64 last_emulated_us{};
    Clock::time_point last_present;

    TimingSamples work;
    TimingSamples interval;
    OffsetSeries offset;
    TimingSamples oversleep;
    OffsetSeries start_offset;
    TimingSamples present_interval;
    OffsetSeries present_offset;
    std::size_t slow_frames{};
    std::vector<Row> rows;
    std::unique_ptr<FileUtil::IOFile> csv;
};

} // namespace Core
