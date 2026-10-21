#include <cstdint>
#include <cstdio>
#include <limits>

#include "streamer/ov5640_clock.hpp"

using namespace streamer::ov5640;
static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

struct FakeIo {
    Pll2Registers registers{0, 0x19, 0x11, 0x30};
    unsigned reads = 0, writes = 0, fail_read = 0;
    bool fail_write = false, fail_after_commit = false;
    int corrupt_readback = -1;

    bool read(uint16_t address, uint8_t& value) {
        ++reads;
        if (reads == fail_read) return false;
        if (address < 0x303a || address > 0x303d) return false;
        const auto index = static_cast<unsigned>(address - 0x303a);
        value = registers[index];
        if (reads > 4 && static_cast<int>(index) == corrupt_readback) value ^= 1U;
        return true;
    }
    bool write(uint16_t address, uint8_t value) {
        ++writes;
        CHECK(address == 0x303b);
        if (fail_write) return false;
        registers[1] = value;
        return !fail_after_commit;
    }
};

int main() {
    const Pll2Registers reset{0, 0x19, 0x11, 0x30};
    const Pll2Registers corrected{0, 0x1e, 0x11, 0x30};
    const auto original = adc_frequency(20'000'000, reset);
    CHECK(original.valid && original.numerator * 3U == 500'000'000ULL * original.denominator);
    CHECK(!original.within(190'000'000, 230'000'000));
    CHECK(adc_frequency(24'000'000, reset).equals(200'000'000));
    CHECK(adc_frequency(20'000'000, corrected).equals(200'000'000));
    CHECK(adc_frequency(20'000'000, corrected).within(190'000'000, 230'000'000));
    CHECK(adc_frequency(20'000'000, corrected).within(200'000'000, 200'000'000));
    CHECK(!adc_frequency(20'000'000, corrected).within(200'000'001, 230'000'000));
    CHECK(!adc_frequency(0, reset).valid);
    auto r = reset;
    r[0] = 0x80;
    CHECK(!adc_frequency(20'000'000, r).valid);
    r = reset; r[1] = 0;
    CHECK(!adc_frequency(20'000'000, r).valid);
    r = reset; r[2] = 0x10;
    const auto zero_div = adc_frequency(24'000'000, r);
    CHECK(zero_div.equals(200'000'000));
    // Half-integer pre/final dividers and root divider must remain exact.
    r = {0, 30, 1, 0x17};
    CHECK(adc_frequency(20'000'000, r).equals(80'000'000));
    r = corrected;
    const auto large = adc_frequency(std::numeric_limits<uint32_t>::max(), r);
    CHECK(large.numerator == static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) * 120U);

    FakeIo io;
    auto out = ensure_adc_200mhz(io, 20'000'000);
    CHECK(out.result == AdcResult::Ok && out.write_attempted);
    CHECK(io.reads == 8 && io.writes == 1 && io.registers == corrected);
    out = ensure_adc_200mhz(io, 20'000'000);
    CHECK(out.result == AdcResult::Ok && !out.write_attempted && io.writes == 1);
    for (unsigned high = 0; high <= 0xe0; high += 0x20) {
        io = {}; io.registers[1] = static_cast<uint8_t>(high | 25U);
        out = ensure_adc_200mhz(io, 20'000'000);
        CHECK(out.result == AdcResult::Ok);
        CHECK(io.registers[1] == (high | 30U));
    }
    for (unsigned fail = 1; fail <= 8; ++fail) {
        io = {}; io.fail_read = fail;
        out = ensure_adc_200mhz(io, 20'000'000);
        CHECK(out.result == (fail <= 4 ? AdcResult::ReadFailed : AdcResult::ReadbackFailed));
        CHECK(io.writes == (fail <= 4 ? 0U : 1U));
    }
    for (bool after_commit : {false, true}) {
        io = {}; io.fail_write = !after_commit; io.fail_after_commit = after_commit;
        out = ensure_adc_200mhz(io, 20'000'000);
        CHECK(out.result == AdcResult::WriteFailed && out.write_attempted);
        CHECK(io.reads == 4 && io.writes == 1);
        CHECK(io.registers[1] == (after_commit ? 30 : 25));
    }
    for (int index = 0; index < 4; ++index) {
        io = {}; io.corrupt_readback = index;
        CHECK(ensure_adc_200mhz(io, 20'000'000).result == AdcResult::VerifyFailed);
        CHECK(io.writes == 1);
    }
    for (const auto unexpected : {Pll2Registers{0x80,25,0x11,0x30},
             Pll2Registers{0,0,0x11,0x30}, Pll2Registers{0,24,0x11,0x30},
             Pll2Registers{0,25,0x10,0x30}, Pll2Registers{0,25,0x12,0x30},
             Pll2Registers{0,25,0x11,0x34}, Pll2Registers{1,25,0x11,0x30}}) {
        io = {}; io.registers = unexpected;
        CHECK(ensure_adc_200mhz(io, 20'000'000).result == AdcResult::UnsupportedConfig);
        CHECK(io.writes == 0 && io.registers == unexpected);
    }
    io = {};
    CHECK(ensure_adc_200mhz(io, 24'000'000).result == AdcResult::UnsupportedConfig && io.writes == 0);
    std::printf("camera clock: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
