#pragma once

#include <cstddef>
#include <cstdint>

#ifndef NATIVE_RGB565_LE
#define NATIVE_RGB565_LE 0
#endif

namespace native_badge {

inline constexpr unsigned width = 320;
inline constexpr unsigned height = 240;
inline constexpr std::size_t pixels = width * height;

// Explicit wire-order colour, retained for default-format callers.
constexpr std::uint16_t rgb565_be(unsigned r, unsigned g, unsigned b) {
    const auto rgb = static_cast<std::uint16_t>(((r & 0xf8u) << 8) |
                                               ((g & 0xfcu) << 3) | (b >> 3));
    return static_cast<std::uint16_t>((rgb >> 8) | (rgb << 8));
}

// The A/B profile changes both JPEG output and the DMA word size together.
constexpr std::uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
#if NATIVE_RGB565_LE
    return static_cast<std::uint16_t>(((r & 0xf8u) << 8) | ((g & 0xfcu) << 3) | (b >> 3));
#else
    return rgb565_be(r, g, b);
#endif
}

void init();
// Starts a full 320x240 row-major RGB565 transfer. Default memory order is BE;
// the NATIVE_RGB565_LE experiment uses native uint16_t values instead.
// False means still busy.
// The caller must keep all pixels immutable until busy() becomes false.
// Optional TE wait is bounded to 25 ms; the streaming path defaults to no wait.
bool present(const std::uint16_t* rgb565, bool wait_te = false);
// Starts a contiguous w*h rectangle in the same pixel format. Useful for a
// small status strip when video is stalled. No stride or extra storage is used.
// Invalid/empty rectangles or a busy display return false. Keep pixels
// immutable until busy() becomes false; present() restores the full window.
bool present_rect(unsigned x, unsigned y, unsigned w, unsigned h, const std::uint16_t* rgb565);
bool busy();  // includes DMA AND the trailing PIO FIFO/WR drain
void wait();  // for startup/diagnostics; streaming can poll busy() instead
std::uint32_t last_transfer_us();
std::uint32_t transfer_count();
std::uint16_t buttons();  // raw active-low inputs: A1, B2, C4, UP8, DOWN16
bool home_pressed();

}  // namespace native_badge
