// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace native_badge {

// A live picture must decode successfully and finish its LCD transfer;
// traffic or a partially received JPEG alone does not count.
class FrameFreshness {
public:
    void presented(uint32_t now_ms, uint32_t known_age_ms = 0) {
        // A late observation of LCD completion must not rejuvenate an image
        // which was already old before a network/IOCTL stall.
        last_ = now_ms - known_age_ms;
        seen_ = known_age_ms < 300;
    }
    void clear() { seen_ = false; }
    bool fresh(uint32_t now_ms) const { return seen_ && now_ms - last_ < 300; }
private:
    uint32_t last_ = 0;
    bool seen_ = false;
};

}  // namespace native_badge
