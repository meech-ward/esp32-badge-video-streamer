// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>

namespace native_badge {

struct TcpArrivalSnapshot {
    uint32_t first_raw_us = 0, segments = 0, ahead = 0, behind = 0;
    uint32_t max_ahead_bytes = 0, cover_events = 0, cover_wait_max_us = 0;
    uint32_t pending_ahead_us = 0;
};

// Observations before lwIP processes each segment, relative to rcv_nxt at
// that instant. Ahead does not prove the segment was accepted into OOSEQ;
// behind can overlap new bytes and is not proof of sender retransmission.
// A covering arrival only closes our measured span to the next segment which
// contains the expected byte; it does not prove every queued hole was filled.
class TcpArrivalWindow {
public:
    void reset() { data_ = {}; seen_ = pending_ = false; }
    void observe(uint32_t sequence, uint32_t expected, uint16_t length, uint32_t now_us) {
        if (!length) return;
        if (!seen_) { data_.first_raw_us = now_us; seen_ = true; }
        ++data_.segments;
        const int32_t relative = static_cast<int32_t>(sequence - expected);
        if (relative > 0) {
            ++data_.ahead;
            data_.max_ahead_bytes = std::max(data_.max_ahead_bytes, static_cast<uint32_t>(relative));
            if (!pending_) { pending_ = true; ahead_at_ = now_us; }
        } else {
            if (relative < 0) ++data_.behind;
            if (pending_ && static_cast<int64_t>(relative) + length > 0) {
                ++data_.cover_events;
                data_.cover_wait_max_us = std::max(data_.cover_wait_max_us, now_us - ahead_at_);
                pending_ = false;
            }
        }
    }
    TcpArrivalSnapshot snapshot(uint32_t now_us) const {
        auto result = data_;
        result.pending_ahead_us = pending_ ? now_us - ahead_at_ : 0;
        return result;
    }
private:
    TcpArrivalSnapshot data_;
    uint32_t ahead_at_ = 0;
    bool seen_ = false, pending_ = false;
};

}  // namespace native_badge
