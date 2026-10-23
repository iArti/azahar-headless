// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <fmt/format.h>
#include "jni/stream/stream_clock.h"

namespace Stream {

void Clock::OnFlip(uint64_t flip_us, uint32_t flip_seq, int32_t slack_us) {
    std::scoped_lock lock(mutex);
    // The panel's refresh period from its own counter over the whole session.
    if (!have_first) {
        have_first = true;
        first_us = flip_us;
        first_seq = flip_seq;
    } else if (flip_seq - first_seq >= 60 && flip_us > first_us) {
        period_us = static_cast<double>(flip_us - first_us) / (flip_seq - first_seq);
    }
    if (slack_us < -200000 || slack_us > 200000) {
        return;
    }
    slacks.push_back(slack_us);
    if (slacks.size() < kWindow) {
        return;
    }
    std::sort(slacks.begin(), slacks.end());
    last_p50 = slacks[slacks.size() / 2];
    slacks.clear();
    // Positive: frames are due well before the refresh that shows them, so emulate them later.
    pending_us = (static_cast<int64_t>(last_p50) - target_us) / 2;
}

std::chrono::microseconds Clock::TakeShift(std::chrono::microseconds emulated_frame) {
    std::scoped_lock lock(mutex);
    int64_t rate = 0;
    if (period_us > 0 && emulated_frame.count() > 0) {
        rate = std::clamp<int64_t>(static_cast<int64_t>(period_us) - emulated_frame.count(),
                                   -kMaxRateUs, kMaxRateUs);
    }
    const int64_t step = std::clamp<int64_t>(pending_us, -kMaxStepUs, kMaxStepUs);
    pending_us -= step;
    shifted_us += step;
    return std::chrono::microseconds(rate + step);
}

void Clock::SetTargetUs(int32_t us) {
    std::scoped_lock lock(mutex);
    target_us = us;
}

std::string Clock::Stats() {
    std::scoped_lock lock(mutex);
    return fmt::format(
        "{{\"period_us\":{:.1f},\"slack_p50_us\":{},\"target_us\":{},\"shifted_us\":{}}}",
        period_us, last_p50, target_us, shifted_us);
}

} // namespace Stream
