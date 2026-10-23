// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <netinet/in.h>
#include "common/polyfill_thread.h"
#include "jni/stream/stream_clock.h"

struct ANativeWindow;

namespace Stream {

class Encoder;

/// Full controller state sent by the console in every 'I' packet (Magic Stream direct protocol).
struct InputState {
    uint32_t buttons = 0; ///< SC_PAD_* bits of the console, plus kZL/kZR for the triggers
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool touch = false;
    uint16_t touch_x = 0, touch_y = 0; ///< 3DS bottom screen, 320x240
};

// Console button bits (magic-stream src/sc_proto.h); the triggers use two otherwise unused bits.
enum : uint32_t {
    kSouth = 0x0001,
    kEast = 0x0002,
    kWest = 0x0008,
    kNorth = 0x0010,
    kL1 = 0x0040,
    kR1 = 0x0080,
    kZL = 0x0100,
    kZR = 0x0200,
    kSelect = 0x0400,
    kStart = 0x0800,
    kHome = 0x1000,
    kUp = 0x10000,
    kDown = 0x20000,
    kLeft = 0x40000,
    kRight = 0x80000,
};

/// Parses an 'I' datagram: 'M' 'I' 1 0 | u32 seq | u32 buttons | 4 x s16 sticks | u8 touch |
/// u16 x | u16 y. Returns false for a malformed packet.
bool ParseInput(const uint8_t* p, size_t n, uint32_t& seq, InputState& out);

/// One streaming session with a console: the UDP socket, video units with re-sending of lost
/// fragments, and incoming input/key-frame requests. The encoder is attached per game.
class Session {
public:
    using InputSink = std::function<void(const InputState&)>;

    Session(InputSink input_sink);
    ~Session();

    /// Binds a local UDP port and starts the receive thread. Returns the local port or -1.
    int Open(const std::string& console_ip, int console_port);
    void Close();

    /// Creates the H.264 encoder; its input window is what the renderer draws into.
    ANativeWindow* StartVideo(int width, int height, int bitrate);
    void StopVideo();

    /// Called by the encoder thread for every output unit: scrcpy 12-byte header + payload.
    void SendUnit(std::vector<uint8_t>&& unit, bool critical);

    std::string Stats();

    /// Frame rhythm along the phone's pipeline (CLOCK_MONOTONIC us): the end of each emulated
    /// frame, the buffer timestamp the renderer gave it and the moment it left the encoder.
    void MarkFrameEnd();
    void MarkEncoded(int64_t buffer_us);

    /// Pacing to the console's panels, fed by its 'C' packets.
    Stream::Clock& PacingClock() {
        return clock;
    }

private:
    void ReceiveLoop(std::stop_token token);
    void SendFragments(uint32_t seq, const std::vector<uint8_t>& unit, bool critical, int only);
    void AnswerNack(const uint8_t* p, size_t n);

    static constexpr size_t kHeader = 12;
    static constexpr size_t kPayload = 1200;
    static constexpr size_t kCache = 128;

    InputSink input_sink;
    int fd = -1;
    sockaddr_in console{};
    std::jthread receiver;

    std::mutex send_mutex; ///< guards the socket writes and the unit cache
    uint32_t next_seq = 0;
    struct Cached {
        uint32_t seq = UINT32_MAX;
        bool critical = false;
        std::vector<uint8_t> unit;
    };
    std::array<Cached, kCache> cache;

    std::mutex encoder_mutex;
    std::unique_ptr<Encoder> encoder;

    Stream::Clock clock;
    struct Series {
        int64_t last = 0;
        std::vector<int32_t> values;
        void Interval(int64_t t);
    };
    std::mutex rhythm_mutex;
    Series frame_end, buffer_ts, encoded;
    std::vector<int32_t> encode_us;
    uint32_t last_input_seq = 0;
    bool have_input = false;
    std::atomic<uint64_t> units_sent{0}, bytes_sent{0}, nacks{0}, key_requests{0}, inputs{0}, clocks{0};
};

} // namespace Stream
