// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace native_badge {
struct HudState {
    std::uint32_t fps100 = 0, decode_us = 0, gap_us = 0;
    bool connected = false, stats_visible = true;
};
// Caller owns an idle 320x240 RGB565 buffer. No allocations or font engine.
void draw_hud(std::uint16_t* frame, const HudState& state);
void draw_waiting_screen(std::uint16_t* frame, const HudState& state);
}  // namespace native_badge
