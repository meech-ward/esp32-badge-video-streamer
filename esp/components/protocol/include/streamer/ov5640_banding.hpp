// OV5640 band steps are line counts for 1/100s (50 Hz) and 1/120s (60 Hz).
// Keep timing correction separate from the caller's explicit mains policy.
#pragma once

#include <array>
#include <cstdint>

namespace streamer::ov5640 {

enum class BandingMode { Hz50, Hz60 };

struct BandingTiming {
    bool valid = false;
    uint16_t step50 = 0, step60 = 0;
    uint8_t max50 = 0, max60 = 0;
};

constexpr BandingTiming calculate_banding(uint32_t sys_hz, uint16_t hts, uint16_t vts) {
    if (!sys_hz || !hts || vts <= 4) return {};
    // Divide once in Hz: staged integer division loses an extra line at some
    // clocks. The four-line margin follows OmniVision's driver example.
    const uint64_t step50 = sys_hz / (static_cast<uint64_t>(hts) * 100U);
    const uint64_t step60 = sys_hz / (static_cast<uint64_t>(hts) * 120U);
    if (!step50 || !step60 || step50 > 1023 || step60 > 1023) return {};
    const uint64_t max50 = (vts - 4U) / step50;
    const uint64_t max60 = (vts - 4U) / step60;
    if (!max50 || !max60 || max50 > 63 || max60 > 63) return {};
    return {true, static_cast<uint16_t>(step50), static_cast<uint16_t>(step60),
        static_cast<uint8_t>(max50), static_cast<uint8_t>(max60)};
}

enum class BandingResult {
    Ok, InvalidMode, InvalidTiming, ReadFailed, WriteFailed, ReadbackFailed, VerifyFailed
};

struct BandingUpdate {
    BandingResult result = BandingResult::ReadFailed;
    BandingTiming timing{};
    uint16_t hts = 0, vts = 0;
    bool write_attempted = false;
    bool rollback_attempted = false;
    bool rollback_verified = false;
};

namespace banding_detail {
constexpr std::array<uint16_t, 8> registers = {
    0x3a08, 0x3a09, 0x3a0a, 0x3a0b, 0x3a0d, 0x3a0e, 0x3c00, 0x3c01
};
}  // namespace banding_detail

// I/O must report transport errors. All snapshots precede writes; only
// changed bytes are written, with the manual selector applied last. This
// never writes exposure, gain, VTS, night mode or the banding-enable control.
// On failure, restore attempted bytes once in reverse order (a failed write
// may have committed), then check all eight original bytes. This is neither
// an atomic update nor a claim that internal ISP history has been restored.
template <typename Io>
BandingUpdate configure_banding(Io& io, uint32_t sys_hz, BandingMode mode) {
    BandingUpdate out;
    if (mode != BandingMode::Hz50 && mode != BandingMode::Hz60) {
        out.result = BandingResult::InvalidMode;
        return out;
    }

    std::array<uint8_t, 4> sensor_timing{};
    for (unsigned i = 0; i < sensor_timing.size(); ++i) {
        if (!io.read(static_cast<uint16_t>(0x380cU + i), sensor_timing[i])) return out;
    }
    // 0x380C[7:5] are debug bits; the remaining bits form 13-bit HTS.
    out.hts = static_cast<uint16_t>((static_cast<unsigned>(sensor_timing[0] & 0x1fU) << 8U) |
        sensor_timing[1]);
    out.vts = static_cast<uint16_t>((static_cast<unsigned>(sensor_timing[2]) << 8U) |
        sensor_timing[3]);
    out.timing = calculate_banding(sys_hz, out.hts, out.vts);
    if (!out.timing.valid) {
        out.result = BandingResult::InvalidTiming;
        return out;
    }

    constexpr auto addresses = banding_detail::registers;
    std::array<uint8_t, addresses.size()> before{};
    for (unsigned i = 0; i < before.size(); ++i) {
        if (!io.read(addresses[i], before[i])) return out;
    }
    auto expected = before;
    expected[0] = static_cast<uint8_t>((before[0] & 0xfcU) | (out.timing.step50 >> 8U));
    expected[1] = static_cast<uint8_t>(out.timing.step50 & 0xffU);
    expected[2] = static_cast<uint8_t>((before[2] & 0xfcU) | (out.timing.step60 >> 8U));
    expected[3] = static_cast<uint8_t>(out.timing.step60 & 0xffU);
    expected[4] = static_cast<uint8_t>((before[4] & 0xc0U) | out.timing.max60);
    expected[5] = static_cast<uint8_t>((before[5] & 0xc0U) | out.timing.max50);
    expected[6] = static_cast<uint8_t>((before[6] & 0xfbU) |
        (mode == BandingMode::Hz50 ? 4U : 0U));
    expected[7] = static_cast<uint8_t>(before[7] | 0x80U);

    std::array<unsigned, addresses.size()> attempted{};
    unsigned attempted_count = 0;
    out.result = BandingResult::Ok;
    for (unsigned i = 0; i < expected.size(); ++i) {
        if (expected[i] == before[i]) continue;
        out.write_attempted = true;
        attempted[attempted_count++] = i;
        if (!io.write(addresses[i], expected[i])) {
            out.result = BandingResult::WriteFailed;
            break;
        }
    }
    if (out.result == BandingResult::Ok) {
        bool read_ok = true, matches = true;
        for (unsigned i = 0; i < expected.size(); ++i) {
            uint8_t actual = 0;
            if (!io.read(addresses[i], actual)) read_ok = false;
            else if (actual != expected[i]) matches = false;
        }
        if (!read_ok) out.result = BandingResult::ReadbackFailed;
        else if (!matches) out.result = BandingResult::VerifyFailed;
    }
    if (out.result == BandingResult::Ok || !attempted_count) return out;

    out.rollback_attempted = true;
    bool restored = true;
    while (attempted_count) {
        const unsigned i = attempted[--attempted_count];
        if (!io.write(addresses[i], before[i])) restored = false;
    }
    // Do not stop at the first cleanup error: verify the complete snapshot,
    // with no retry, and retain the original update failure in result.
    for (unsigned i = 0; i < before.size(); ++i) {
        uint8_t actual = 0;
        if (!io.read(addresses[i], actual)) restored = false;
        else if (actual != before[i]) restored = false;
    }
    out.rollback_verified = restored;
    return out;
}

}  // namespace streamer::ov5640
