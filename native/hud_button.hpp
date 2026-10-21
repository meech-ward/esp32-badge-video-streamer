// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace native_badge {
// One event on a stable B press. This uses elapsed durations so millisecond
// wrap and a held button during connection loss cannot create extra toggles.
class HudButton {
public:
    bool update(std::uint32_t now, bool pressed) {
        if (pressed != candidate_) { candidate_ = pressed; changed_at_ = now; }
        if (candidate_ == stable_ || now - changed_at_ < 20) return false;
        stable_ = candidate_;
        return stable_;
    }
private:
    bool candidate_ = false, stable_ = false;
    std::uint32_t changed_at_ = 0;
};
}  // namespace native_badge
