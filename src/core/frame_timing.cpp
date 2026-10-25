// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <ctime>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core_timing.h"
#include "core/frame_timing.h"

namespace Core {

namespace {
s64 Micros(std::chrono::steady_clock::duration d) {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
}
double Ms(s64 us) {
    return static_cast<double>(us) / 1000.0;
}
} // Anonymous namespace

void TimingSamples::Add(s64 us) {
    samples.push_back(us);
}

s64 TimingSamples::Percentile(int pct) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t n = samples.size();
    const std::size_t rank = std::clamp<std::size_t>((pct * n + 99) / 100, 1, n);
    return samples[rank - 1];
}

std::string OffsetSeries::Spread() const {
    const std::size_t n = points.size();
    if (n < 2) {
        return "-";
    }
    double mt = 0, mo = 0;
    for (const auto& [t, o] : points) {
        mt += static_cast<double>(t);
        mo += static_cast<double>(o);
    }
    mt /= static_cast<double>(n);
    mo /= static_cast<double>(n);
    double stt = 0, sto = 0;
    for (const auto& [t, o] : points) {
        stt += (static_cast<double>(t) - mt) * (static_cast<double>(t) - mt);
        sto += (static_cast<double>(t) - mt) * (static_cast<double>(o) - mo);
    }
    const double slope = stt > 0 ? sto / stt : 0;
    TimingSamples residuals;
    for (const auto& [t, o] : points) {
        residuals.Add(static_cast<s64>(static_cast<double>(o) - mo -
                                       slope * (static_cast<double>(t) - mt)));
    }
    const s64 mid = residuals.Percentile(50);
    return fmt::format("-{:.1f}/+{:.1f}", Ms(mid - residuals.Percentile(1)),
                       Ms(residuals.Percentile(99) - mid));
}

FrameTimingLog::FrameTimingLog() = default;

FrameTimingLog::~FrameTimingLog() {
    WriteCsv();
}

void FrameTimingLog::Record(Clock::time_point submit, s64 emulated_us, double speed_scale,
                            s64 sleep_requested_us, Clock::time_point wake) {
    if (!started) {
        started = true;
        origin = submit;
        origin_emulated_us = emulated_us;
        last_submit = submit;
        last_wake = wake;
        last_emulated_us = emulated_us;
        last_present = {};
        return;
    }
    // A pause or a state load: start over rather than report it as one very late frame.
    if (submit - last_submit > std::chrono::seconds{1}) {
        Clear();
        started = false;
        Record(submit, emulated_us, speed_scale, sleep_requested_us, wake);
        return;
    }

    constexpr s64 frame_us = static_cast<s64>(1'000'000 / SCREEN_REFRESH_RATE);
    const s64 work_us = Micros(submit - last_wake);
    const s64 sleep_us = Micros(wake - submit);
    work.Add(work_us);
    interval.Add(Micros(submit - last_submit));
    // Where the frame was handed over relative to an ideal clock that follows emulated time:
    // its spread is the margin a pacer downstream of the emulator has to hide.
    AddOffset(offset, submit, emulated_us, speed_scale);
    // Where the frame started, against the emulated time it started at: the limiter's own rhythm.
    AddOffset(start_offset, last_wake, last_emulated_us, speed_scale);
    if (sleep_requested_us > 0) {
        oversleep.Add(sleep_us - sleep_requested_us);
    }
    if (work_us > frame_us) {
        slow_frames++;
    }
    if (Settings::values.record_frame_times) {
        rows.push_back({Micros(submit - origin), emulated_us - origin_emulated_us, work_us,
                        sleep_requested_us, sleep_us});
    }

    last_submit = submit;
    last_wake = wake;
    last_emulated_us = emulated_us;
    if (work.Count() >= REPORT_FRAMES) {
        Report();
        WriteCsv();
    }
}

void FrameTimingLog::RecordPresent(Clock::time_point present, s64 emulated_us,
                                   double speed_scale) {
    if (!started) {
        return;
    }
    if (last_present != Clock::time_point{} && present - last_present < std::chrono::seconds{1}) {
        present_interval.Add(Micros(present - last_present));
    }
    last_present = present;
    AddOffset(present_offset, present, emulated_us, speed_scale);
}

void FrameTimingLog::AddOffset(OffsetSeries& series, Clock::time_point t, s64 emulated_us,
                               double speed_scale) const {
    const double scale = speed_scale > 0 ? speed_scale : 1.0;
    const s64 t_us = Micros(t - origin);
    series.Add(t_us, t_us - static_cast<s64>(
                                static_cast<double>(emulated_us - origin_emulated_us) / scale));
}

void FrameTimingLog::Clear() {
    work.Clear();
    interval.Clear();
    offset.Clear();
    oversleep.Clear();
    start_offset.Clear();
    present_interval.Clear();
    present_offset.Clear();
    slow_frames = 0;
}

void FrameTimingLog::Report() {
    const std::size_t frames = work.Count();
    LOG_INFO(Render,
             "Frame timing ({} frames): work p50 {:.1f} p95 {:.1f} p99 {:.1f} max {:.1f} ms, "
             "slow {}; submit interval p1 {:.1f} p50 {:.1f} p99 {:.1f} ms; submit vs emulated "
             "clock {} ms (p1/p99 around the trend); oversleep p50 {:.2f} p99 {:.2f} "
             "max {:.2f} ms ({} sleeps)",
             frames, Ms(work.Percentile(50)), Ms(work.Percentile(95)), Ms(work.Percentile(99)),
             Ms(work.Percentile(100)), slow_frames, Ms(interval.Percentile(1)),
             Ms(interval.Percentile(50)), Ms(interval.Percentile(99)), offset.Spread(),
             Ms(oversleep.Percentile(50)), Ms(oversleep.Percentile(99)),
             Ms(oversleep.Percentile(100)), oversleep.Count());
    LOG_INFO(Render,
             "Frame rhythm ({}): start vs emulated clock {} ms; present interval p1 {:.1f} p50 "
             "{:.1f} p99 {:.1f} ms, present vs emulated clock {} ms ({} presents)",
             Settings::values.present_on_deadline ? "present on deadline" : "present, then wait",
             start_offset.Spread(), Ms(present_interval.Percentile(1)),
             Ms(present_interval.Percentile(50)), Ms(present_interval.Percentile(99)),
             present_offset.Spread(), present_offset.Count());
    Clear();
}

void FrameTimingLog::WriteCsv() {
    if (rows.empty()) {
        return;
    }
    if (!csv) {
        const std::time_t t = std::time(nullptr);
        const std::string path =
            fmt::format("{}/frame_timing_{:%F-%H-%M-%S}.csv",
                        FileUtil::GetUserPath(FileUtil::UserPath::LogDir), *std::localtime(&t));
        csv = std::make_unique<FileUtil::IOFile>(path, "w");
        csv->WriteString("submit_us,emulated_us,work_us,sleep_requested_us,sleep_us\n");
    }
    std::string text;
    for (const Row& r : rows) {
        text += fmt::format("{},{},{},{},{}\n", r.submit_us, r.emulated_us, r.work_us,
                            r.sleep_requested_us, r.sleep_us);
    }
    csv->WriteString(text);
    csv->Flush();
    rows.clear();
}

} // namespace Core
