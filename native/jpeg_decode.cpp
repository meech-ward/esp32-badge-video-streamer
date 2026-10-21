// SPDX-License-Identifier: MIT
#include "jpeg_decode.hpp"

#include <climits>
#include <cstring>

#include "JPEGDEC.h"

namespace native_badge {
namespace {

// The library keeps all MCU, IDCT and Huffman scratch in this object. Static
// storage avoids a large core-1 stack and per-frame allocation.
JPEGDEC decoder;

struct Target {
    std::uint16_t* pixels;
    std::size_t capacity;
    std::uint16_t width;
    std::uint16_t height;
    std::uint32_t written = 0;
    bool bounds_error = false;
};

int draw_block(JPEGDRAW* block) {
    auto* target = block ? static_cast<Target*>(block->pUser) : nullptr;
    if (target == nullptr) return 0;
    if (block->pPixels == nullptr || block->iBpp != 16 || block->x < 0 || block->y < 0 ||
        block->iWidth <= 0 || block->iHeight <= 0 || block->iWidthUsed <= 0 ||
        block->iWidthUsed > block->iWidth || block->x >= target->width ||
        block->y >= target->height || block->iWidthUsed > target->width - block->x ||
        block->iHeight > target->height - block->y) {
        target->bounds_error = true;
        return 0;
    }
    const auto width = static_cast<std::size_t>(block->iWidthUsed);
    const auto height = static_cast<std::size_t>(block->iHeight);
    const auto stride = static_cast<std::size_t>(block->iWidth);
    const auto first = static_cast<std::size_t>(block->y) * target->width +
                       static_cast<std::size_t>(block->x);
    const auto end = first + (height - 1) * target->width + width;
    if (end > target->capacity) {
        target->bounds_error = true;
        return 0;
    }
    // JPEGDEC's rightmost MCU may have a padded source stride. Copy only the
    // used pixels so neither edge padding nor RGB conversion reaches the LCD.
    for (std::size_t row = 0; row < height; ++row) {
        std::memcpy(target->pixels + first + row * target->width,
                    block->pPixels + row * stride, width * sizeof(std::uint16_t));
    }
    target->written += static_cast<std::uint32_t>(width * height);
    return 1;
}

}  // namespace

DecodeResult decode_jpeg_rgb565(const std::uint8_t* input, std::size_t length,
                               std::uint16_t* output, std::size_t capacity_pixels,
                               std::uint16_t expected_width, std::uint16_t expected_height) {
    DecodeResult result;
    const auto required = static_cast<std::size_t>(expected_width) * expected_height;
    if (input == nullptr || output == nullptr || expected_width == 0 || expected_height == 0 ||
        length > INT_MAX || capacity_pixels < required) {
        result.error = kDecodeInvalidArgument;
        return result;
    }
    // A complete protocol payload must contain a complete JPEG. In particular,
    // a truncated frame must not borrow an old buffer tail to finish decoding.
    if (length < 4 || input[0] != 0xff || input[1] != 0xd8 ||
        input[length - 2] != 0xff || input[length - 1] != 0xd9) {
        result.error = kDecodeMalformedJpeg;
        return result;
    }
    // openRAM's API predates const input; its read callback only copies bytes.
    if (!decoder.openRAM(const_cast<std::uint8_t*>(input), static_cast<int>(length), draw_block)) {
        result.error = decoder.getLastError();
        if (result.error == JPEG_SUCCESS) result.error = kDecodeMalformedJpeg;
        return result;
    }
    const int width = decoder.getWidth();
    const int height = decoder.getHeight();
    result.width = static_cast<std::uint16_t>(width);
    result.height = static_cast<std::uint16_t>(height);
    if (width != expected_width || height != expected_height) {
        result.error = kDecodeDimensions;
        decoder.close();
        return result;
    }
    Target target{output, capacity_pixels, expected_width, expected_height};
    decoder.setUserPointer(&target);
    decoder.setPixelType(kRgb565LittleEndian ? RGB565_LITTLE_ENDIAN : RGB565_BIG_ENDIAN);
    const int decoded = decoder.decode(0, 0, 0);
    const int decoder_error = decoder.getLastError();
    decoder.close();
    result.pixels = target.written;
    if (target.bounds_error) result.error = kDecodeOutputBounds;
    else if (!decoded) result.error = decoder_error ? decoder_error : kDecodeMalformedJpeg;
    else if (target.written != required) result.error = kDecodeIncomplete;
    else result.ok = true;
    return result;
}

}  // namespace native_badge
