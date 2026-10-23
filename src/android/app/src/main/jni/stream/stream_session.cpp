// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <algorithm>
#include <arpa/inet.h>
#include <fmt/format.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "common/logging/log.h"
#include "jni/stream/stream_encoder.h"
#include "jni/stream/stream_session.h"

namespace Stream {

namespace {
uint16_t Get16(const uint8_t* b) {
    return static_cast<uint16_t>(b[0] << 8 | b[1]);
}
uint32_t Get32(const uint8_t* b) {
    return static_cast<uint32_t>(b[0]) << 24 | static_cast<uint32_t>(b[1]) << 16 |
           static_cast<uint32_t>(b[2]) << 8 | b[3];
}
void Put16(uint8_t* b, uint16_t v) {
    b[0] = static_cast<uint8_t>(v >> 8);
    b[1] = static_cast<uint8_t>(v);
}
void Put32(uint8_t* b, uint32_t v) {
    b[0] = static_cast<uint8_t>(v >> 24);
    b[1] = static_cast<uint8_t>(v >> 16);
    b[2] = static_cast<uint8_t>(v >> 8);
    b[3] = static_cast<uint8_t>(v);
}
constexpr uint16_t kAllFragments = 0xffff;
} // namespace

bool ParseInput(const uint8_t* p, size_t n, uint32_t& seq, InputState& out) {
    if (n < 25 || p[0] != 'M' || p[1] != 'I' || p[2] != 1) {
        return false;
    }
    seq = Get32(p + 4);
    out.buttons = Get32(p + 8);
    out.lx = static_cast<int16_t>(Get16(p + 12));
    out.ly = static_cast<int16_t>(Get16(p + 14));
    out.rx = static_cast<int16_t>(Get16(p + 16));
    out.ry = static_cast<int16_t>(Get16(p + 18));
    out.touch = p[20] != 0;
    out.touch_x = Get16(p + 21);
    out.touch_y = Get16(p + 23);
    return true;
}

Session::Session(InputSink input_sink_) : input_sink{std::move(input_sink_)} {}

Session::~Session() {
    Close();
}

int Session::Open(const std::string& console_ip, int console_port) {
    console.sin_family = AF_INET;
    console.sin_port = htons(static_cast<uint16_t>(console_port));
    if (inet_pton(AF_INET, console_ip.c_str(), &console.sin_addr) != 1) {
        LOG_ERROR(Frontend, "Stream: bad console address {}", console_ip);
        return -1;
    }
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    // A key frame leaves as one burst of datagrams.
    const int buffer = 2 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
    sockaddr_in me{};
    me.sin_family = AF_INET;
    socklen_t len = sizeof(me);
    if (bind(fd, reinterpret_cast<sockaddr*>(&me), sizeof(me)) < 0 ||
        getsockname(fd, reinterpret_cast<sockaddr*>(&me), &len) < 0) {
        close(fd);
        fd = -1;
        return -1;
    }
    receiver = std::jthread([this](std::stop_token token) { ReceiveLoop(token); });
    LOG_INFO(Frontend, "Stream: session with {}:{}, local UDP {}", console_ip, console_port,
             ntohs(me.sin_port));
    return ntohs(me.sin_port);
}

void Session::Close() {
    StopVideo();
    receiver.request_stop();
    if (receiver.joinable()) {
        receiver.join();
    }
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

ANativeWindow* Session::StartVideo(int width, int height, int bitrate) {
    StopVideo();
    auto e = std::make_unique<Encoder>(*this);
    if (!e->Start(width, height, bitrate)) {
        return nullptr;
    }
    std::scoped_lock lock(encoder_mutex);
    encoder = std::move(e);
    return encoder->Window();
}

void Session::StopVideo() {
    std::unique_ptr<Encoder> e;
    {
        std::scoped_lock lock(encoder_mutex);
        e = std::move(encoder);
    }
    e.reset(); // joins the output thread, which may be inside SendUnit
}

void Session::SendUnit(std::vector<uint8_t>&& unit, bool critical) {
    std::scoped_lock lock(send_mutex);
    const uint32_t seq = next_seq++;
    Cached& slot = cache[seq % kCache];
    slot.seq = seq;
    slot.critical = critical;
    slot.unit = std::move(unit);
    // Session and codec-config units are tiny and every later frame depends on them.
    for (int i = 0; i < (critical ? 3 : 1); i++) {
        SendFragments(seq, slot.unit, critical, -1);
    }
    units_sent++;
    bytes_sent += slot.unit.size();
}

void Session::SendFragments(uint32_t seq, const std::vector<uint8_t>& unit, bool critical,
                            int only) {
    if (fd < 0) {
        return;
    }
    const size_t count = std::max<size_t>(1, (unit.size() + kPayload - 1) / kPayload);
    uint8_t packet[kHeader + kPayload];
    packet[0] = 'M';
    packet[1] = 'S';
    packet[2] = 1;
    packet[3] = critical ? 1 : 0;
    Put32(packet + 4, seq);
    Put16(packet + 10, static_cast<uint16_t>(count));
    for (size_t i = 0; i < count; i++) {
        if (only >= 0 && static_cast<size_t>(only) != i) {
            continue;
        }
        const size_t off = i * kPayload;
        const size_t n = std::min(kPayload, unit.size() - off);
        Put16(packet + 8, static_cast<uint16_t>(i));
        std::copy_n(unit.data() + off, n, packet + kHeader);
        sendto(fd, packet, kHeader + n, 0, reinterpret_cast<const sockaddr*>(&console),
               sizeof(console));
    }
}

void Session::AnswerNack(const uint8_t* p, size_t n) {
    if (n < 10) {
        return;
    }
    const uint32_t seq = Get32(p + 4);
    const size_t count = std::min<size_t>(Get16(p + 8), (n - 10) / 2);
    std::scoped_lock lock(send_mutex);
    const Cached& slot = cache[seq % kCache];
    if (slot.seq != seq) {
        return; // too old
    }
    for (size_t k = 0; k < count; k++) {
        const uint16_t index = Get16(p + 10 + 2 * k);
        SendFragments(seq, slot.unit, slot.critical, index == kAllFragments ? -1 : index);
    }
    nacks++;
}

void Session::ReceiveLoop(std::stop_token token) {
    uint8_t buf[1500];
    while (!token.stop_requested()) {
        pollfd pf{fd, POLLIN, 0};
        if (poll(&pf, 1, 100) <= 0) {
            continue;
        }
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 4 || buf[0] != 'M' || buf[2] != 1) {
            continue;
        }
        switch (buf[1]) {
        case 'N':
            AnswerNack(buf, static_cast<size_t>(n));
            break;
        case 'K': {
            std::scoped_lock lock(encoder_mutex);
            if (encoder) {
                encoder->RequestKeyFrame();
                key_requests++;
            }
            break;
        }
        case 'I': {
            uint32_t seq;
            InputState state;
            if (!ParseInput(buf, static_cast<size_t>(n), seq, state)) {
                break;
            }
            // Every packet carries the full state: only a newer one matters.
            if (have_input && static_cast<int32_t>(seq - last_input_seq) <= 0) {
                break;
            }
            have_input = true;
            last_input_seq = seq;
            inputs++;
            input_sink(state);
            break;
        }
        default:
            break;
        }
    }
}

std::string Session::Stats() {
    return fmt::format(
        "{{\"units\":{},\"bytes\":{},\"nacks\":{},\"key_requests\":{},\"inputs\":{}}}",
        units_sent.load(), bytes_sent.load(), nacks.load(), key_requests.load(), inputs.load());
}

} // namespace Stream
