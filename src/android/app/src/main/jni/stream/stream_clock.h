// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Stream {

/**
 * Paces emulation to the console's panels. The console shows frame n at the first refresh after
 * n * period + offset (its offset covers the arrival jitter) and reports for every frame it shows
 * the slack: that refresh minus the frame's due time ('C' packets).
 *
 * Rate: each frame the frame limiter is shifted by (panel period - emulated frame time), so one
 * emulated frame takes one panel refresh (3DS 59.83 Hz on a ~60.1 Hz panel).
 * Phase: once per window the median slack is compared with the target and half the difference is
 * applied, at most kMaxStepUs per frame. Less slack means less waiting on the console.
 */
class Clock {
public:
    /// Called from the receive thread for every 'C' packet.
    void OnFlip(uint64_t flip_us, uint32_t flip_seq, int32_t slack_us);
    /// Called by the frame limiter once per frame with the frame's emulated duration.
    std::chrono::microseconds TakeShift(std::chrono::microseconds emulated_frame);
    void SetTargetUs(int32_t us);
    /// {"period_us":…,"slack_p50_us":…,"target_us":…,"shifted_us":…}
    std::string Stats();

    static constexpr int32_t kMaxStepUs = 300;
    static constexpr int32_t kMaxRateUs = 1000;
    static constexpr size_t kWindow = 60;

private:
    std::mutex mutex;
    std::vector<int32_t> slacks;
    uint64_t first_us = 0;
    uint32_t first_seq = 0;
    bool have_first = false;
    double period_us = 0;
    int32_t target_us = 1000;
    int64_t pending_us = 0;
    int64_t shifted_us = 0;
    int32_t last_p50 = 0;
};

} // namespace Stream
