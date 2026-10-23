// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <atomic>
#include <cstdint>
#include "common/polyfill_thread.h"

struct AMediaCodec;
struct ANativeWindow;

namespace Stream {

class Session;

/// Hardware H.264 encoder with a Surface input: the renderer draws straight into Window(), and
/// every output buffer is handed to the session as a scrcpy-style unit (12-byte header + data).
class Encoder {
public:
    Encoder(Session& session);
    ~Encoder();

    bool Start(int width, int height, int bitrate);
    ANativeWindow* Window() const {
        return window;
    }
    /// Asks for a key frame (the console lost a reference frame).
    void RequestKeyFrame();

private:
    void OutputLoop(std::stop_token token);

    Session& session;
    AMediaCodec* codec = nullptr;
    ANativeWindow* window = nullptr;
    int width = 0, height = 0;
    uint64_t frame_index = 0;
    std::jthread output;
};

} // namespace Stream
