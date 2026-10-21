// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

// Exact integer semantics of the three DSP instructions used by JPEGDEC.
// Host-only: proves decoded colour identity; it makes no speed prediction.
inline std::int32_t lane(std::uint32_t value, unsigned shift) {
    return static_cast<std::int16_t>(value >> shift);
}
inline std::uint32_t jpeg_ssub16(std::uint32_t a, std::uint32_t b) {
    const auto lo = static_cast<std::uint16_t>(lane(a, 0) - lane(b, 0));
    const auto hi = static_cast<std::uint16_t>(lane(a, 16) - lane(b, 16));
    return lo | (static_cast<std::uint32_t>(hi) << 16);
}
inline std::uint32_t jpeg_smlad(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const auto sum = static_cast<std::int64_t>(lane(a, 0)) * lane(b, 0) +
                     static_cast<std::int64_t>(lane(a, 16)) * lane(b, 16) +
                     static_cast<std::int32_t>(c);
    return static_cast<std::uint32_t>(sum);
}
inline std::uint32_t jpeg_usat16(std::uint32_t a, unsigned n) {
    const auto maximum = (1 << n) - 1;
    auto lo = lane(a, 0);
    auto hi = lane(a, 16);
    lo = lo < 0 ? 0 : lo > maximum ? maximum : lo;
    hi = hi < 0 ? 0 : hi > maximum ? maximum : hi;
    return static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
}
#define ARM_MATH_CM4 1
#define __SSUB16(a, b) jpeg_ssub16(a, b)
#define __SMLAD(a, b, c) jpeg_smlad(a, b, c)
#define __USAT16(a, n) jpeg_usat16(a, n)
