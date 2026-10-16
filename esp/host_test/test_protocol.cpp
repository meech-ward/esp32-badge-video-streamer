// Byte-for-byte interoperability against Python, including invalid inputs.
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "streamer/protocol.hpp"
#include "vectors.h"

using namespace streamer;
static int checks = 0;
static int failed = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { ++failed; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

void test_vectors() {
    for (const auto& v : vec::kHello) {
        uint8_t packed[kHelloSize] = {};
        const bool ok = pack_hello(packed, {static_cast<uint8_t>(v.fs), static_cast<uint8_t>(v.q),
                                            static_cast<uint8_t>(v.credits)});
        CHECK(ok == v.ok);
        if (ok) {
            CHECK(std::memcmp(packed, v.data, sizeof packed) == 0);
            Hello h;
            CHECK(unpack_hello(v.data, sizeof v.data, h));
            CHECK(h.framesize == v.fs && h.quality == v.q && h.credits == v.credits);
        }
    }
    for (const auto& v : vec::kHeader) {
        uint8_t packed[kHeaderSize];
        pack_header(packed, static_cast<uint8_t>(v.type), static_cast<uint16_t>(v.seq), v.width, v.len, v.age);
        CHECK(std::memcmp(packed, v.data, sizeof packed) == 0);
        Header h;
        const bool ok = unpack_header(v.data, sizeof v.data, h);
        CHECK(ok == v.ok);
        if (ok) CHECK(h.type == v.fields[0] && h.seq == v.fields[1] && h.width == v.fields[2] &&
                      h.len == v.fields[3] && h.age_us == v.fields[4]);
    }
}

void test_malformed() {
    uint8_t hello[kHelloSize];
    CHECK(pack_hello(hello, {}));
    const uint8_t expected[] = {0x56, 0x48, 1, 0, 6, 1, 0, 0};
    CHECK(std::memcmp(hello, expected, sizeof expected) == 0);
    Hello h;
    for (size_t len = 0; len < kHelloSize; ++len) CHECK(!unpack_hello(hello, len, h));
    CHECK(!unpack_hello(hello, kHelloSize + 1, h));
    for (size_t pos : {size_t(0), size_t(1), size_t(2), size_t(6), size_t(7)}) {
        uint8_t bad[kHelloSize];
        std::memcpy(bad, hello, sizeof bad);
        bad[pos] ^= 0xFF;
        CHECK(!unpack_hello(bad, sizeof bad, h));
    }
    uint8_t header[kHeaderSize];
    pack_header(header, kTypeFrame, 0, 160, kMaxPayload, 0xFFFFFFFF);
    Header out;
    CHECK(unpack_header(header, sizeof header, out));
    CHECK(out.len == kMaxPayload && out.age_us == 0xFFFFFFFF);
    for (size_t len = 0; len < kHeaderSize; ++len) CHECK(!unpack_header(header, len, out));
    for (size_t pos : {size_t(0), size_t(1), size_t(2), size_t(3)}) {
        uint8_t bad[kHeaderSize];
        std::memcpy(bad, header, sizeof bad);
        bad[pos] ^= 0xFF;
        CHECK(!unpack_header(bad, sizeof bad, out));
    }
}

int main() {
    test_vectors();
    test_malformed();
    std::printf("protocol: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
