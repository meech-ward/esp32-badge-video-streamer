// STATUS formatting is host-tested against the protocol's fixed payload limit.
#pragma once

#include <cstdio>
#include <cstdint>

#include "streamer/stream_stats.hpp"

namespace streamer {

struct VideoStatus {
    float fps = 0, grab_fps = 0, last_kb = 0;
    uint32_t heap = 0, psram = 0;
    int64_t uptime_s = 0;
    const char* sensor = "none";
    uint8_t framesize = 0, quality = 8;
    const char* wb = "hw";
    StreamTimings timing;
    uint32_t drop_oversize = 0;  // cumulative for this video session
    int rssi = -127;  // video client's associated-STA average dBm; -127 unknown
};

inline int format_video_status(char* out, size_t cap, const VideoStatus& s) {
    // Existing keys retain their meaning. cam_fps is consumed/grabbed frames,
    // not an independent count of frames produced by the camera driver.
    return std::snprintf(out, cap,
        "{\"fps\":%.1f,\"cam_fps\":%.1f,\"kb\":%.1f,\"rssi\":%d,"
        "\"heap\":%lu,\"psram\":%lu,\"uptime_s\":%lld,\"sensor\":\"%s\",\"fs\":%u,\"q\":%u,"
        "\"wb\":\"%s\",\"g_us\":%lu,\"gm_us\":%lu,\"tx_us\":%lu,\"tm_us\":%lu,\"w_us\":%lu,\"cw_ms\":%lu,\"mx_kb\":%.1f,\"drop_oversize\":%lu}",
        static_cast<double>(s.fps), static_cast<double>(s.grab_fps), static_cast<double>(s.last_kb),
        s.rssi,
        static_cast<unsigned long>(s.heap), static_cast<unsigned long>(s.psram), static_cast<long long>(s.uptime_s),
        s.sensor, s.framesize, s.quality, s.wb,
        static_cast<unsigned long>(s.timing.grab.average_us()), static_cast<unsigned long>(s.timing.grab.max_us),
        static_cast<unsigned long>(s.timing.send.average_us()), static_cast<unsigned long>(s.timing.send.max_us),
        static_cast<unsigned long>(s.timing.wait.average_us()), static_cast<unsigned long>(s.timing.credit_wait_ms()),
        static_cast<double>(s.timing.max_frame_bytes) / 1024.0,
        static_cast<unsigned long>(s.drop_oversize));
}

}  // namespace streamer
