// Video streamer protocol v1: mirror of tufty/video_streamer/stream_protocol.py.
// The wire contract is docs/protocol.md. Pure C++, no ESP-IDF headers, so the
// host tests compile it unchanged. All integers are little-endian.
#pragma once

#include <cstddef>
#include <cstdint>

namespace streamer {

inline constexpr uint8_t kVersion = 1;
inline constexpr uint16_t kVideoPort = 8081;

inline constexpr size_t kHelloSize = 8;
inline constexpr size_t kHeaderSize = 16;

inline constexpr uint8_t kTypeFrame = 1;
inline constexpr uint8_t kTypeStatus = 2;

inline constexpr uint8_t kFsQqvga = 0;  // 160x120
inline constexpr uint8_t kFsQvga = 1;   // 320x240
inline constexpr uint8_t kFs240 = 2;    // 240x240
inline constexpr uint8_t kFsVga = 3;    // 640x480, host debugging only
inline constexpr uint8_t kFsCount = 4;

struct FrameDims {
    uint16_t w;
    uint16_t h;
};
inline constexpr FrameDims kFrameSizes[kFsCount] = {{160, 120}, {320, 240}, {240, 240}, {640, 480}};

inline constexpr uint8_t kMaxCredits = 4;
inline constexpr uint32_t kMaxPayload = 128 * 1024;
inline constexpr uint32_t kStatusMax = 512;
inline constexpr uint8_t kCredit = 'C';

constexpr bool valid_framesize(unsigned fs) { return fs < kFsCount; }
constexpr bool valid_quality(unsigned q) { return q >= 4 && q <= 63; }
constexpr bool valid_credits(unsigned c) { return c >= 1 && c <= kMaxCredits; }

namespace detail {
inline void put_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_u32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace detail

// ---- HELLO (client -> ESP, TCP) ----

struct Hello {
    uint8_t framesize = kFsQqvga;
    uint8_t quality = 6;
    uint8_t credits = 1;
};

// Returns false (and writes nothing) for out-of-range fields, like pack_hello's ProtocolError.
inline bool pack_hello(uint8_t* out, const Hello& h) {
    if (!valid_framesize(h.framesize) || !valid_quality(h.quality) || !valid_credits(h.credits)) return false;
    out[0] = 'V';
    out[1] = 'H';
    out[2] = kVersion;
    out[3] = h.framesize;
    out[4] = h.quality;
    out[5] = h.credits;
    detail::put_u16(out + 6, 0);
    return true;
}

inline bool unpack_hello(const uint8_t* buf, size_t len, Hello& out) {
    if (len != kHelloSize) return false;
    if (buf[0] != 'V' || buf[1] != 'H' || buf[2] != kVersion) return false;
    if (!valid_framesize(buf[3]) || !valid_quality(buf[4]) || !valid_credits(buf[5])) return false;
    if (detail::get_u16(buf + 6) != 0) return false;
    out.framesize = buf[3];
    out.quality = buf[4];
    out.credits = buf[5];
    return true;
}

// ---- Message header (ESP -> client, TCP) ----

struct Header {
    uint8_t type = 0;
    uint16_t seq = 0;
    uint32_t width = 0;  // pixels; on the wire as width / 8
    uint32_t len = 0;
    uint32_t age_us = 0;
};

inline void pack_header(uint8_t* out, uint8_t type, uint16_t seq, uint32_t width, uint32_t len,
                        uint32_t age_us = 0) {
    out[0] = 'V';
    out[1] = 'S';
    out[2] = kVersion;
    out[3] = type;
    detail::put_u16(out + 4, seq);
    detail::put_u16(out + 6, static_cast<uint16_t>(width / 8));
    detail::put_u32(out + 8, len);
    detail::put_u32(out + 12, age_us);
}

inline bool unpack_header(const uint8_t* buf, size_t len, Header& out) {
    if (len < kHeaderSize) return false;
    if (buf[0] != 'V' || buf[1] != 'S' || buf[2] != kVersion) return false;
    const uint8_t type = buf[3];
    if (type != kTypeFrame && type != kTypeStatus) return false;
    const uint32_t plen = detail::get_u32(buf + 8);
    if (plen > kMaxPayload || (type == kTypeStatus && plen > kStatusMax)) return false;
    out.type = type;
    out.seq = detail::get_u16(buf + 4);
    out.width = static_cast<uint32_t>(detail::get_u16(buf + 6)) * 8;
    out.len = plen;
    out.age_us = detail::get_u32(buf + 12);
    return true;
}

}  // namespace streamer
