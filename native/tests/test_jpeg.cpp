// SPDX-License-Identifier: MIT
#include "jpeg_decode.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "JPEG check failed: %s\n", label);
        std::exit(1);
    }
}
std::vector<std::uint8_t> read(const char* path) {
    std::ifstream file(path, std::ios::binary);
    check(file.good(), "fixture opened");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void guarded_decode(const std::vector<std::uint8_t>& jpeg, std::uint16_t width,
                    std::uint16_t height, bool expected_ok, int first_high = -1, int first_low = -1) {
    constexpr std::uint16_t guard = 0xa55a;
    const auto count = static_cast<std::size_t>(width) * height;
    std::vector<std::uint16_t> output(count + 2, guard);
    const auto result = native_badge::decode_jpeg_rgb565(jpeg.data(), jpeg.size(), output.data() + 1,
                                                       count, width, height);
    check(result.ok == expected_ok, "decode result");
    check(output.front() == guard && output.back() == guard, "framebuffer edge canaries");
    if (expected_ok) {
        check(result.width == width && result.height == height, "decoded dimensions");
        check(result.pixels == count && result.error == 0, "complete frame");
        auto hash = std::uint32_t{2166136261u};
        for (std::size_t i = 1; i <= count; ++i) {
            const auto colour = native_badge::kRgb565LittleEndian ? output[i] :
                static_cast<std::uint16_t>((output[i] >> 8) | (output[i] << 8));
            hash = (hash ^ (colour >> 8)) * 16777619u;
            hash = (hash ^ (colour & 255u)) * 16777619u;
        }
        std::printf("COLOUR_HASH %u %u %lu\n", width, height, static_cast<unsigned long>(hash));
        if (first_high >= 0) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(output.data() + 1);
            if (native_badge::kRgb565LittleEndian) std::swap(first_high, first_low);
            check(bytes[0] == first_high && bytes[1] == first_low, "LCD RGB565 byte order");
            check(bytes[2 * count - 2] == first_high && bytes[2 * count - 1] == first_low,
                  "last pixel and source stride");
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    check(argc >= 7, "at least six JPEG fixture paths");
    const auto red = read(argv[1]);
    const auto blue = read(argv[2]);
    const auto odd = read(argv[3]);
    const auto gray = read(argv[4]);
    const auto progressive = read(argv[5]);
    const auto colour_grid = read(argv[6]);
    guarded_decode(red, 320, 240, true, 0xf8, 0x00);
    guarded_decode(blue, 160, 120, true, 0x00, 0x1f);
    guarded_decode(odd, 17, 19, true);  // clipped right/bottom MCU edges
    guarded_decode(gray, 320, 240, true);
    guarded_decode(progressive, 320, 240, false);
    guarded_decode(colour_grid, 320, 240, true);
    for (int i = 7; i < argc; ++i) guarded_decode(read(argv[i]), 320, 240, true);

    std::vector<std::uint16_t> output(320 * 240, 0xa55a);
    auto result = native_badge::decode_jpeg_rgb565(red.data(), red.size(), output.data(), output.size() - 1);
    check(!result.ok && result.error == native_badge::kDecodeInvalidArgument, "undersized destination rejected");
    check(std::all_of(output.begin(), output.end(), [](auto p) { return p == 0xa55a; }),
          "undersized destination unchanged");
    result = native_badge::decode_jpeg_rgb565(red.data(), red.size(), output.data(), output.size(), 160, 120);
    check(!result.ok && result.error == native_badge::kDecodeDimensions, "wrong source dimensions rejected");
    check(std::all_of(output.begin(), output.end(), [](auto p) { return p == 0xa55a; }),
          "wrong-sized source does not write pixels");
    result = native_badge::decode_jpeg_rgb565(nullptr, red.size(), output.data(), output.size());
    check(!result.ok && result.error == native_badge::kDecodeInvalidArgument, "null input");
    result = native_badge::decode_jpeg_rgb565(red.data(), red.size(), nullptr, output.size());
    check(!result.ok && result.error == native_badge::kDecodeInvalidArgument, "null output");
    result = native_badge::decode_jpeg_rgb565(red.data(), red.size(), output.data(), output.size(), 0, 240);
    check(!result.ok && result.error == native_badge::kDecodeInvalidArgument, "zero target width");
    guarded_decode({}, 320, 240, false);
    guarded_decode({0xff, 0xd8, 0xff}, 320, 240, false);
    auto truncated = red;
    truncated.resize(red.size() / 2);
    guarded_decode(truncated, 320, 240, false);
    truncated.push_back(0xff);
    truncated.push_back(0xd9);
    guarded_decode(truncated, 320, 240, false);  // premature EOI must not present a partial frame
    auto corrupt = red;
    corrupt[0] = 0;
    guarded_decode(corrupt, 320, 240, false);
    // Decode again after failures: scratch/callback state must not leak between jobs.
    guarded_decode(red, 320, 240, true, 0xf8, 0x00);
    guarded_decode(blue, 160, 120, true, 0x00, 0x1f);
    std::printf("NATIVE_JPEG_OK %u checks\n", checks);
}
