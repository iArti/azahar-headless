// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#pragma once

#include <chrono>
#include <memory>
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
 * Frame pacing log of the emulation thread (Magic Stream, phase 0). FrameLimiter reports every
 * system frame; every REPORT_FRAMES frames one summary line goes to the log. With
 * Settings::values.record_frame_times every frame is also written to
 * <log dir>/frame_timing_<date>.csv.
 *
 * "Submit" is the moment the renderer has handed the frame over for presentation (the end of
 * SwapBuffers), which is when a streaming encoder could take it.
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

private:
    void Report();
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

    TimingSamples work;
    TimingSamples interval;
    TimingSamples offset;
    TimingSamples oversleep;
    std::size_t slow_frames{};
    std::vector<Row> rows;
    std::unique_ptr<FileUtil::IOFile> csv;
};

} // namespace Core
