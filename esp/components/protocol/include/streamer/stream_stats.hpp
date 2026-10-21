// Bounded stream timing counters shared by firmware and host checks.
#pragma once

#include <algorithm>
#include <cstdint>

namespace streamer {

struct DurationStats {
    uint64_t total_us = 0;
    uint32_t count = 0;
    uint32_t max_us = 0;

    void add(uint64_t us) {
        total_us += us;
        ++count;
        max_us = std::max(max_us, static_cast<uint32_t>(std::min<uint64_t>(us, UINT32_MAX)));
    }
    uint32_t average_us() const {
        return count ? static_cast<uint32_t>(std::min<uint64_t>(total_us / count, UINT32_MAX)) : 0;
    }
};

struct StreamTimings {
    DurationStats grab;  // camera wait plus copy, including unsuccessful waits
    DurationStats send;  // complete frame send, including socket waits
    DurationStats wait;  // socket-wait total per frame-send attempt, zero included
    uint64_t credit_wait_us = 0;  // connected, HELLO accepted, no credit: select() time
    uint32_t max_frame_bytes = 0;

    void frame(uint32_t bytes) { max_frame_bytes = std::max(max_frame_bytes, bytes); }
    uint32_t credit_wait_ms() const {
        return static_cast<uint32_t>(std::min<uint64_t>(credit_wait_us / 1000, UINT32_MAX));
    }
};

}  // namespace streamer
