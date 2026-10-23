// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <vector>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include "common/logging/log.h"
#include "jni/stream/stream_encoder.h"
#include "jni/stream/stream_session.h"

namespace Stream {

namespace {
constexpr int32_t kColorFormatSurface = 0x7F000789;
constexpr int32_t kBitrateModeCbr = 2;
constexpr int32_t kProfileBaseline = 1;
constexpr uint32_t kFlagKeyFrame = 1;
constexpr uint32_t kFlagCodecConfig = 2;
constexpr uint64_t kPacketConfig = 1ULL << 62;
constexpr uint64_t kPacketKey = 1ULL << 61;

void Put32(uint8_t* b, uint32_t v) {
    b[0] = static_cast<uint8_t>(v >> 24);
    b[1] = static_cast<uint8_t>(v >> 16);
    b[2] = static_cast<uint8_t>(v >> 8);
    b[3] = static_cast<uint8_t>(v);
}

void Put64(uint8_t* b, uint64_t v) {
    Put32(b, static_cast<uint32_t>(v >> 32));
    Put32(b + 4, static_cast<uint32_t>(v));
}
} // namespace

Encoder::Encoder(Session& session_) : session{session_} {}

Encoder::~Encoder() {
    output.request_stop();
    if (output.joinable()) {
        output.join();
    }
    if (codec) {
        AMediaCodec_stop(codec);
        AMediaCodec_delete(codec);
    }
    if (window) {
        ANativeWindow_release(window);
    }
}

bool Encoder::Start(int width_, int height_, int bitrate) {
    width = width_;
    height = height_;
    codec = AMediaCodec_createEncoderByType("video/avc");
    if (!codec) {
        LOG_ERROR(Frontend, "Stream: no H.264 encoder");
        return false;
    }
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, 60);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 2);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatSurface);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BITRATE_MODE, kBitrateModeCbr);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PROFILE, kProfileBaseline);
    // One frame in, one frame out: no reordering and no lookahead queue in the encoder.
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_LATENCY, 1);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PRIORITY, 0);
    AMediaFormat_setInt32(format, "max-bframes", 0);
    const media_status_t configured =
        AMediaCodec_configure(codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    AMediaFormat_delete(format);
    if (configured != AMEDIA_OK) {
        LOG_ERROR(Frontend, "Stream: encoder configure failed ({})", static_cast<int>(configured));
        return false;
    }
    if (AMediaCodec_createInputSurface(codec, &window) != AMEDIA_OK || !window) {
        LOG_ERROR(Frontend, "Stream: encoder input surface failed");
        return false;
    }
    if (AMediaCodec_start(codec) != AMEDIA_OK) {
        LOG_ERROR(Frontend, "Stream: encoder start failed");
        return false;
    }
    // The console needs the picture size before the first frame (scrcpy session packet).
    std::vector<uint8_t> unit(12);
    Put32(unit.data(), 0x80000000u);
    Put32(unit.data() + 4, static_cast<uint32_t>(width));
    Put32(unit.data() + 8, static_cast<uint32_t>(height));
    session.SendUnit(std::move(unit), true);

    output = std::jthread([this](std::stop_token token) { OutputLoop(token); });
    LOG_INFO(Frontend, "Stream: H.264 encoder {}x{} at {} bit/s", width, height, bitrate);
    return true;
}

void Encoder::RequestKeyFrame() {
    if (!codec) {
        return;
    }
    AMediaFormat* params = AMediaFormat_new();
    AMediaFormat_setInt32(params, "request-sync", 0);
    AMediaCodec_setParameters(codec, params);
    AMediaFormat_delete(params);
}

void Encoder::OutputLoop(std::stop_token token) {
    while (!token.stop_requested()) {
        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (index < 0) {
            continue; // timeout, format change or buffers changed
        }
        size_t capacity = 0;
        const uint8_t* data = AMediaCodec_getOutputBuffer(codec, index, &capacity);
        if (data && info.size > 0) {
            const bool config = (info.flags & kFlagCodecConfig) != 0;
            const bool key = (info.flags & kFlagKeyFrame) != 0;
            std::vector<uint8_t> unit(12 + static_cast<size_t>(info.size));
            const uint64_t head = config ? kPacketConfig
                                         : (static_cast<uint64_t>(info.presentationTimeUs) &
                                            ((1ULL << 61) - 1)) |
                                               (key ? kPacketKey : 0);
            Put64(unit.data(), head);
            Put32(unit.data() + 8, static_cast<uint32_t>(info.size));
            std::copy_n(data + info.offset, info.size, unit.data() + 12);
            session.SendUnit(std::move(unit), config);
        }
        AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
    }
}

} // namespace Stream
