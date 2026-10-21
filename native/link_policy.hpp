// A cached Wi-Fi UP status does not prove the AP or camera is reachable.
#pragma once
#include <cstdint>

namespace native_badge {
class LinkRecovery {
public:
    bool poll(uint32_t now, bool wifi_up, uint32_t frames, uint32_t failed_connections) {
        if (!wifi_up) { tracking_ = false; return false; }
        if (!tracking_) {
            tracking_ = true;
            frame_at_ = now;
            frames_ = frames;
            failures_ = failed_connections;
        }
        if (frames != frames_) {
            frames_ = frames;
            frame_at_ = now;
            failures_ = failed_connections;
        }
        return failed_connections - failures_ >= 3 || now - frame_at_ >= 8000;
    }
    void reset() { tracking_ = false; }
private:
    bool tracking_ = false;
    uint32_t frame_at_ = 0, frames_ = 0, failures_ = 0;
};
}  // namespace native_badge
