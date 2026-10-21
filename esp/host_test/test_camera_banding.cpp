#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <vector>

#include "streamer/ov5640_banding.hpp"

using namespace streamer::ov5640;
static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

constexpr std::array<uint16_t, 12> addresses = {
    0x380c, 0x380d, 0x380e, 0x380f,
    0x3a08, 0x3a09, 0x3a0a, 0x3a0b, 0x3a0d, 0x3a0e, 0x3c00, 0x3c01
};

struct FakeIo {
    // Deliberately different target fields make every update position
    // reachable by failure injection; debug/control bits must survive.
    std::array<uint8_t, 12> bytes = {
        0x08, 0x0c, 0x03, 0xd8, 0xa2, 0x27, 0xb0, 0xf6, 0xc4, 0x83, 0x5b, 0x24
    };
    unsigned reads = 0, writes = 0;
    std::vector<unsigned> failed_reads;
    std::map<unsigned, bool> failed_writes;  // attempt -> whether it committed
    std::map<unsigned, uint8_t> corrupt_reads;
    std::vector<uint16_t> write_addresses;

    unsigned index(uint16_t address) {
        for (unsigned i = 0; i < addresses.size(); ++i)
            if (addresses[i] == address) return i;
        CHECK(false);  // no writes to exposure/gain/night/VTS or unknown registers
        return static_cast<unsigned>(addresses.size());
    }
    bool read(uint16_t address, uint8_t& value) {
        const unsigned i = index(address);
        if (i >= bytes.size()) return false;
        ++reads;
        value = bytes[i];
        for (const unsigned failure : failed_reads) if (failure == reads) return false;
        if (const auto corrupt = corrupt_reads.find(reads); corrupt != corrupt_reads.end())
            value ^= corrupt->second;
        return true;
    }
    bool write(uint16_t address, uint8_t value) {
        const unsigned i = index(address);
        CHECK(i >= 4 && i < bytes.size());  // timings are read-only inputs
        if (i < 4 || i >= bytes.size()) return false;
        ++writes;
        write_addresses.push_back(address);
        const auto failure = failed_writes.find(writes);
        if (failure == failed_writes.end() || failure->second) bytes[i] = value;
        return failure == failed_writes.end();
    }
};

static void expect70(const BandingTiming& t) {
    CHECK(t.valid && t.step50 == 339 && t.step60 == 283 && t.max50 == 2 && t.max60 == 3);
}

int main() {
    expect70(calculate_banding(70'000'000, 2060, 984));
    const auto fast = calculate_banding(80'000'000, 2060, 984);
    CHECK(fast.valid && fast.step50 == 388 && fast.step60 == 323 && fast.max50 == 2 && fast.max60 == 3);
    // 60 Hz uses one final truncation: a staged integer calculation gives
    // 282 here, instead of the documented SYS/(HTS*120) formula's 283.
    CHECK(calculate_banding(70'000'000, 2060, 984).step60 == 283);
    CHECK(!calculate_banding(0, 2060, 984).valid);
    CHECK(!calculate_banding(70'000'000, 0, 984).valid);
    for (uint16_t vts : {uint16_t{0}, uint16_t{1}, uint16_t{4}})
        CHECK(!calculate_banding(70'000'000, 2060, vts).valid);
    CHECK(!calculate_banding(99, 1, 984).valid);
    CHECK(!calculate_banding(100, 1, 984).valid);  // zero 60 Hz step, even if 50 Hz is nonzero
    CHECK(!calculate_banding(102'400, 1, 1027).valid);  // step exceeds ten bits
    const auto largest_step = calculate_banding(102'300, 1, 1027);
    CHECK(largest_step.valid && largest_step.step50 == 1023 && largest_step.max50 == 1);
    CHECK(!calculate_banding(1200, 1, 15).valid);  // no complete 50 Hz band fits
    CHECK(calculate_banding(1200, 1, 16).valid);
    const auto largest_count = calculate_banding(600, 1, 319);
    CHECK(largest_count.valid && largest_count.max60 == 63);
    CHECK(!calculate_banding(600, 1, 324).valid);  // count exceeds six bits; never clamp
    CHECK(!calculate_banding(std::numeric_limits<uint32_t>::max(), 1, 984).valid);
    CHECK(calculate_banding(std::numeric_limits<uint32_t>::max(), 65535, 32768).valid);

    FakeIo io;
    const auto original = io.bytes;
    auto out = configure_banding(io, 70'000'000, BandingMode::Hz50);
    CHECK(out.result == BandingResult::Ok && out.write_attempted);
    CHECK(!out.rollback_attempted && !out.rollback_verified);
    expect70(out.timing);
    CHECK(out.hts == 2060 && out.vts == 984 && io.reads == 20 && io.writes == 8);
    CHECK((io.bytes == std::array<uint8_t, 12>{
        0x08,0x0c,0x03,0xd8,0xa1,0x53,0xb1,0x1b,0xc3,0x82,0x5f,0xa4}));
    for (unsigned i = 0; i < io.write_addresses.size(); ++i)
        CHECK(io.write_addresses[i] == addresses[i + 4]);
    const auto corrected = io.bytes;
    io.reads = io.writes = 0; io.write_addresses.clear();
    out = configure_banding(io, 70'000'000, BandingMode::Hz50);
    CHECK(out.result == BandingResult::Ok && !out.write_attempted);
    CHECK(io.bytes == corrected && io.reads == 20 && io.writes == 0);
    out = configure_banding(io, 70'000'000, BandingMode::Hz60);
    CHECK(out.result == BandingResult::Ok && io.writes == 1 && io.bytes[10] == 0x5b);
    CHECK(io.write_addresses.back() == 0x3c00 && io.bytes[11] == 0xa4);

    // Reproduce the old real 295/246 step pair and manual 50 Hz selector.
    io = {}; io.bytes[4] = 0xa1; io.bytes[10] = 0x04; io.bytes[11] = 0xa4;
    out = configure_banding(io, 70'000'000, BandingMode::Hz50);
    CHECK(out.result == BandingResult::Ok && io.writes == 5);
    CHECK(io.bytes[10] == 0x04 && io.bytes[11] == 0xa4);
    io = {}; io.bytes[0] = 0xe8;
    out = configure_banding(io, 70'000'000, BandingMode::Hz50);
    CHECK(out.result == BandingResult::Ok && out.hts == 2060 && io.bytes[0] == 0xe8);

    // Reserved/control bits are independent of the requested selector.
    for (unsigned debug = 0; debug <= 0xfc; debug += 4) {
        for (const auto mode : {BandingMode::Hz50, BandingMode::Hz60}) {
            io = {}; io.bytes[4] = static_cast<uint8_t>(debug | 2U);
            io.bytes[6] = static_cast<uint8_t>(debug);
            io.bytes[8] = static_cast<uint8_t>((debug & 0xc0U) | 4U);
            io.bytes[9] = static_cast<uint8_t>((debug & 0xc0U) | 3U);
            io.bytes[10] = static_cast<uint8_t>(debug);
            io.bytes[11] = static_cast<uint8_t>(debug);
            const auto before = io.bytes;
            CHECK(configure_banding(io, 70'000'000, mode).result == BandingResult::Ok);
            CHECK((io.bytes[4] & 0xfcU) == (before[4] & 0xfcU));
            CHECK((io.bytes[6] & 0xfcU) == (before[6] & 0xfcU));
            CHECK((io.bytes[8] & 0xc0U) == (before[8] & 0xc0U));
            CHECK((io.bytes[9] & 0xc0U) == (before[9] & 0xc0U));
            CHECK((io.bytes[10] & 0xfbU) == (before[10] & 0xfbU));
            CHECK((io.bytes[11] & 0x7fU) == (before[11] & 0x7fU));
        }
    }
    io = {};
    CHECK(configure_banding(io, 70'000'000, static_cast<BandingMode>(99)).result == BandingResult::InvalidMode);
    CHECK(io.reads == 0 && io.writes == 0);
    io = {}; io.bytes[1] = 0; io.bytes[0] = 0;
    CHECK(configure_banding(io, 70'000'000, BandingMode::Hz50).result == BandingResult::InvalidTiming);
    CHECK(io.reads == 4 && io.writes == 0);

    // Every timing/snapshot read must succeed before a single byte changes.
    for (unsigned failed = 1; failed <= 12; ++failed) {
        io = {}; io.failed_reads = {failed};
        out = configure_banding(io, 70'000'000, BandingMode::Hz50);
        CHECK(out.result == BandingResult::ReadFailed && !out.write_attempted);
        CHECK(!out.rollback_attempted && !out.rollback_verified && io.writes == 0);
        CHECK(io.bytes == original);
    }
    // A failed write may have committed. Restore it as well as preceding
    // writes, in reverse order, without retrying the failed update.
    for (unsigned failed = 1; failed <= 8; ++failed) {
        for (bool committed : {false, true}) {
            io = {}; io.failed_writes[failed] = committed;
            out = configure_banding(io, 70'000'000, BandingMode::Hz50);
            CHECK(out.result == BandingResult::WriteFailed && out.write_attempted);
            CHECK(out.rollback_attempted && out.rollback_verified && io.bytes == original);
            CHECK(io.reads == 20 && io.writes == failed * 2);
            for (unsigned i = 0; i < failed; ++i)
                CHECK(io.write_addresses[failed + i] == addresses[4 + failed - 1 - i]);
        }
    }
    for (unsigned failed = 13; failed <= 20; ++failed) {
        io = {}; io.failed_reads = {failed};
        out = configure_banding(io, 70'000'000, BandingMode::Hz50);
        CHECK(out.result == BandingResult::ReadbackFailed);
        CHECK(out.rollback_attempted && out.rollback_verified && io.bytes == original);
        CHECK(io.reads == 28 && io.writes == 16);
    }
    for (unsigned bad_read = 13; bad_read <= 20; ++bad_read) {
        io = {}; io.corrupt_reads[bad_read] = 0x80;  // also checks preserved high bits
        out = configure_banding(io, 70'000'000, BandingMode::Hz50);
        CHECK(out.result == BandingResult::VerifyFailed);
        CHECK(out.rollback_attempted && out.rollback_verified && io.bytes == original);
    }

    // Cleanup errors never erase the original failure or claim recovery.
    io = {}; io.failed_writes[8] = true; io.failed_writes[9] = false;
    out = configure_banding(io, 70'000'000, BandingMode::Hz50);
    CHECK(out.result == BandingResult::WriteFailed && out.rollback_attempted && !out.rollback_verified);
    CHECK(io.reads == 20 && io.writes == 16 && io.bytes[11] != original[11]);
    for (unsigned failed = 13; failed <= 20; ++failed) {
        io = {}; io.failed_writes[1] = true; io.failed_reads = {failed};
        out = configure_banding(io, 70'000'000, BandingMode::Hz50);
        CHECK(out.result == BandingResult::WriteFailed && !out.rollback_verified);
        CHECK(io.reads == 20 && io.writes == 2 && io.bytes == original);
    }
    io = {}; io.failed_writes[1] = true; io.corrupt_reads[20] = 1;
    out = configure_banding(io, 70'000'000, BandingMode::Hz50);
    CHECK(out.result == BandingResult::WriteFailed && !out.rollback_verified);
    io = {}; io.failed_writes[8] = true; io.failed_writes[9] = true;
    out = configure_banding(io, 70'000'000, BandingMode::Hz50);
    CHECK(out.result == BandingResult::WriteFailed && !out.rollback_verified);
    CHECK(io.bytes == original);  // conservative: cleanup I/O error is still reported

    // An unchanged request performs no restore writes, even if verification
    // fails: this transaction never changed the sensor in the first place.
    for (bool read_failed : {false, true}) {
        io = {}; io.bytes = corrected;
        if (read_failed) io.failed_reads = {13}; else io.corrupt_reads[13] = 0x80;
        out = configure_banding(io, 70'000'000, BandingMode::Hz50);
        CHECK(out.result == (read_failed ? BandingResult::ReadbackFailed : BandingResult::VerifyFailed));
        CHECK(!out.write_attempted && !out.rollback_attempted && io.writes == 0);
        CHECK(io.reads == 20 && io.bytes == corrected);
    }

    std::printf("camera banding: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
