// OV5640 v2.33, section 2.5 / figure 2-3: PLL2 supplies the ADC clock,
// independently of PLL1's SYS/PCLK profile. No platform or floating-point code.
#pragma once

#include <array>
#include <cstdint>

namespace streamer::ov5640 {

using Pll2Registers = std::array<uint8_t, 4>;  // 0x303A through 0x303D

struct Frequency {
    uint64_t numerator = 0;
    uint32_t denominator = 1;
    bool valid = false;

    constexpr bool equals(uint32_t hz) const {
        return valid && numerator == static_cast<uint64_t>(hz) * denominator;
    }
    constexpr bool within(uint32_t low_hz, uint32_t high_hz) const {
        return valid && numerator >= static_cast<uint64_t>(low_hz) * denominator &&
               numerator <= static_cast<uint64_t>(high_hz) * denominator;
    }
};

constexpr Frequency adc_frequency(uint32_t xclk_hz, const Pll2Registers& r) {
    const uint32_t multiplier = r[1] & 0x1fU;
    if (!xclk_hz || !multiplier || (r[0] & 0x80U)) return {};
    constexpr uint32_t prediv_twice[] = {2, 3, 4, 6};
    constexpr uint32_t final_div_twice[] = {2, 2, 4, 5};
    const uint32_t prediv = prediv_twice[(r[3] >> 4U) & 3U];
    const uint32_t root_div = (r[3] & 4U) ? 2U : 1U;
    const uint32_t raw_sysdiv = r[2] & 15U;
    const uint32_t sysdiv = raw_sysdiv ? raw_sysdiv : 1U;  // zero also means /1
    const uint32_t final_div = final_div_twice[r[3] & 3U];
    return {static_cast<uint64_t>(xclk_hz) * multiplier * 4U,
            prediv * root_div * sysdiv * final_div, true};
}

enum class AdcResult { Ok, ReadFailed, UnsupportedConfig, WriteFailed, ReadbackFailed, VerifyFailed };

struct AdcCorrection {
    AdcResult result = AdcResult::ReadFailed;
    Pll2Registers before{}, after{};
    bool write_attempted = false;
};

// Register I/O must return false on transport failure. Apply only the measured
// 20 MHz input / reset-divider branch; leave unknown profiles untouched.
// A failed write may have reached the device: no atomic rollback is claimed.
template <typename Io>
AdcCorrection ensure_adc_200mhz(Io& io, uint32_t xclk_hz) {
    AdcCorrection out;
    for (unsigned i = 0; i < out.before.size(); ++i) {
        if (!io.read(static_cast<uint16_t>(0x303aU + i), out.before[i])) return out;
    }
    const auto& r = out.before;
    const uint8_t multiplier = r[1] & 0x1fU;
    if (xclk_hz != 20'000'000 || r[0] != 0 || r[2] != 0x11 || r[3] != 0x30 ||
        (multiplier != 25 && multiplier != 30)) {
        out.result = AdcResult::UnsupportedConfig;
        return out;
    }
    Pll2Registers expected = r;
    expected[1] = static_cast<uint8_t>((r[1] & 0xe0U) | 30U);
    if (expected[1] != r[1]) {
        out.write_attempted = true;
        if (!io.write(0x303b, expected[1])) {
            out.result = AdcResult::WriteFailed;
            return out;
        }
    }
    for (unsigned i = 0; i < out.after.size(); ++i) {
        if (!io.read(static_cast<uint16_t>(0x303aU + i), out.after[i])) {
            out.result = AdcResult::ReadbackFailed;
            return out;
        }
    }
    const Frequency actual = adc_frequency(xclk_hz, out.after);
    out.result = out.after == expected && actual.equals(200'000'000) &&
                 actual.within(190'000'000, 230'000'000)
        ? AdcResult::Ok : AdcResult::VerifyFailed;
    return out;
}

}  // namespace streamer::ov5640
