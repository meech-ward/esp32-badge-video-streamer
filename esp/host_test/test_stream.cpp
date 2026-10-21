// Timing windows and STATUS payload bounds without ESP-IDF.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include "streamer/protocol.hpp"
#include "streamer/status_json.hpp"

static int g_checks = 0;
static int g_fail = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_fail; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void timings() {
    streamer::StreamTimings t;
    CHECK(t.grab.average_us() == 0 && t.grab.max_us == 0);
    t.grab.add(2000);
    t.grab.add(100000);  // a camera timeout belongs in the window too
    CHECK(t.grab.average_us() == 51000 && t.grab.max_us == 100000);
    t.send.add(100);
    t.send.add(10000);
    t.wait.add(0);  // immediate sends contribute zero, not a missing sample
    t.wait.add(9000);
    CHECK(t.send.average_us() == 5050 && t.send.max_us == 10000);
    CHECK(t.wait.average_us() == 4500 && t.wait.count == t.send.count);
    t.credit_wait_us = 12999;
    CHECK(t.credit_wait_ms() == 12);
    t.frame(3000);
    t.frame(2000);
    t.frame(5000);
    CHECK(t.max_frame_bytes == 5000);
    t = {};
    CHECK(t.grab.count == 0 && t.send.count == 0 && t.credit_wait_us == 0 && t.max_frame_bytes == 0);
    // A pathological multi-minute/multi-hour send must saturate telemetry,
    // rather than wrapping into a fast-looking measurement.
    t.send.add(static_cast<uint64_t>(UINT32_MAX) + 1000);
    CHECK(t.send.average_us() == UINT32_MAX && t.send.max_us == UINT32_MAX);
    t.credit_wait_us = UINT64_MAX;
    CHECK(t.credit_wait_ms() == UINT32_MAX);
}

void status_json() {
    streamer::VideoStatus s;
    s.fps = 36;
    s.grab_fps = 36;
    s.last_kb = 3.5f;
    s.sensor = "OV2640";
    s.wb = "soft";
    s.timing.grab.add(100);
    s.timing.grab.add(300);
    s.timing.send.add(900);
    s.timing.send.add(1100);
    s.timing.wait.add(0);
    s.timing.wait.add(500);
    s.timing.credit_wait_us = 13000;
    s.timing.frame(4096);
    s.drop_oversize = 3;
    s.rssi = -42;
    std::array<char, streamer::kStatusMax> out{};
    int len = streamer::format_video_status(out.data(), out.size(), s);
    CHECK(len > 0 && len < static_cast<int>(out.size()));
    CHECK(std::strstr(out.data(), "\"fps\":36.0,\"cam_fps\":36.0,\"kb\":3.5") != nullptr);
    CHECK(std::strstr(out.data(), "\"rssi\":-42,") != nullptr);
    CHECK(std::strstr(out.data(), "\"g_us\":200,\"gm_us\":300,\"tx_us\":1000,\"tm_us\":1100,\"w_us\":250,\"cw_ms\":13,\"mx_kb\":4.0,\"drop_oversize\":3}") != nullptr);
    CHECK(static_cast<size_t>(len) == std::strlen(out.data()));
    // Deliberately exceed physically plausible ranges: even the widest
    // counters and signed timestamps fit, including the terminating NUL.
    s.fps = static_cast<float>(UINT32_MAX);
    s.grab_fps = static_cast<float>(UINT32_MAX);
    s.last_kb = static_cast<float>(streamer::kMaxPayload) / 1024.0f;
    s.heap = s.psram = UINT32_MAX;
    s.uptime_s = INT64_MAX;
    s.sensor = "OV5640";
    s.framesize = 3;
    s.quality = 63;
    s.wb = "soft";
    s.timing.grab = {UINT32_MAX, 1, UINT32_MAX};
    s.timing.send = s.timing.wait = s.timing.grab;
    s.timing.credit_wait_us = UINT64_MAX;
    s.timing.max_frame_bytes = UINT32_MAX;
    s.drop_oversize = UINT32_MAX;
    s.rssi = -127;
    len = streamer::format_video_status(out.data(), out.size(), s);
    CHECK(len > 0 && len < static_cast<int>(out.size()));
    CHECK(out[static_cast<size_t>(len) - 1] == '}');
    std::printf("worst-case STATUS: %d/%zu bytes\n", len, out.size());
    std::array<char, 24> tiny;
    tiny.fill('!');
    const int wanted = streamer::format_video_status(tiny.data() + 1, 16, s);
    CHECK(wanted == len && tiny[0] == '!' && tiny[16] == '\0' && tiny[17] == '!');
}

int main() {
    timings();
    status_json();
    std::printf("stream: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
