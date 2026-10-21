// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace native_badge {

#if defined(NATIVE_RGB565_LE) && NATIVE_RGB565_LE
inline constexpr bool kRgb565LittleEndian = true;
#else
inline constexpr bool kRgb565LittleEndian = false;
#endif

enum DecodeError : int {
    kDecodeOk = 0,
    kDecodeInvalidArgument = 100,
    kDecodeMalformedJpeg,
    kDecodeDimensions,
    kDecodeOutputBounds,
    kDecodeIncomplete,
};

struct DecodeResult {
    bool ok = false;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t pixels = 0;
    int error = kDecodeOk;  // JPEGDEC's error, or a DecodeError above
};

// Core 1 owns the single decoder instance. Calls must not overlap. The input
// remains immutable until return; output must not be displayed/DMA-read then.
// Successful output is row-major RGB565, big-endian bytes by default. The
// optional NATIVE_RGB565_LE build must use the matching 16-bit LCD DMA path.
// On failure the output may be partially written: retain the previous display.
// There is no scaling; diagnostic sizes must be requested explicitly.
DecodeResult decode_jpeg_rgb565(const std::uint8_t* input, std::size_t length,
                               std::uint16_t* output, std::size_t capacity_pixels,
                               std::uint16_t expected_width = 320,
                               std::uint16_t expected_height = 240);

}  // namespace native_badge
