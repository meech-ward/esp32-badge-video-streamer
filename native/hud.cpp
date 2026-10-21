// SPDX-License-Identifier: MIT
#include "hud.hpp"

#include <algorithm>
#include <cstdio>

#include "display.hpp"

namespace native_badge {
namespace {

// Small fixed 5x7 alphabet: five columns, bit 0 at the top. Keeping the HUD
// independent of a font engine avoids image objects and per-frame allocation.
constexpr std::uint8_t digits[][5] = {
    {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31},
    {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e},
};
constexpr std::uint8_t letters[][5] = {
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36},
    {0x3e,0x41,0x41,0x41,0x22}, {0x7f,0x41,0x41,0x22,0x1c},
    {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f},
    {0x00,0x41,0x7f,0x41,0x00}, {0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f},
    {0x3e,0x41,0x41,0x41,0x3e}, {0x7f,0x09,0x09,0x09,0x06},
    {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f}, {0x1f,0x20,0x40,0x20,0x1f},
    {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
};

std::uint8_t column(char character, unsigned x) {
    if (character >= '0' && character <= '9') return digits[character - '0'][x];
    if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
    if (character >= 'A' && character <= 'Z') return letters[character - 'A'][x];
    if (character == '.') return x == 2 ? 0x40 : 0;
    if (character == ':') return x == 2 ? 0x24 : 0;
    if (character == '-') return 0x08;
    if (character == '/') return static_cast<std::uint8_t>(0x40u >> x);
    return 0;
}

void text(std::uint16_t* frame, unsigned x, unsigned y, const char* value, std::uint16_t colour) {
    while (*value && x + 5 <= width && y + 7 <= height) {
        for (unsigned dx = 0; dx < 5; ++dx) {
            const auto bits = column(*value, dx);
            for (unsigned dy = 0; dy < 7; ++dy)
                if (bits & (1u << dy)) frame[(y + dy) * width + x + dx] = colour;
        }
        x += 6;
        ++value;
    }
}

void fill(std::uint16_t* frame, unsigned x, unsigned y, unsigned w, unsigned h, std::uint16_t colour) {
    for (unsigned row = y; row < y + h; ++row) std::fill_n(frame + row * width + x, w, colour);
}

constexpr auto black = rgb565(0, 0, 0);
constexpr auto white = rgb565(235, 245, 255);
}  // namespace

void draw_hud(std::uint16_t* frame, const HudState& state) {
    if (!frame || !state.stats_visible) return;
    fill(frame, 0, 0, width, 20, black);
    char line[64];
    std::snprintf(line, sizeof line, "%lu.%lu FPS  JPEG %lu MS",
                  static_cast<unsigned long>(state.fps100 / 100),
                  static_cast<unsigned long>((state.fps100 / 10) % 10),
                  static_cast<unsigned long>((state.decode_us + 500) / 1000));
    text(frame, 4, 2, line, white);
    std::snprintf(line, sizeof line, "GAP %lu MS  B:STATS",
                  static_cast<unsigned long>((state.gap_us + 500) / 1000));
    text(frame, 4, 11, line, white);
}

void draw_waiting_screen(std::uint16_t* frame, const HudState& state) {
    if (!frame) return;
    std::fill_n(frame, pixels, black);
    text(frame, 100, 112, state.connected ? "WAITING FOR VIDEO" : "CONNECTING TO CAMERA", white);
    draw_hud(frame, state);
}

}  // namespace native_badge
