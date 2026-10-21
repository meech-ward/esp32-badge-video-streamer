// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace native_badge {

class DecodeForecast {
public:
    void reset() { estimate_ = 0; }
    void completed(uint32_t duration_us, bool ok) {
        if (ok) estimate_ = std::clamp<uint32_t>(duration_us, 5000, 100000);
    }
    uint32_t estimate_us() const { return estimate_; }
    bool due(uint32_t now_us, uint32_t start_us, uint32_t lead_us) const {
        if (!lead_us || !estimate_ || !start_us) return false;
        const uint32_t lead = std::min(lead_us, estimate_ - 1000);
        return now_us - start_us >= estimate_ - lead;
    }
private:
    uint32_t estimate_ = 0;
};

struct GuardedJpegInputs {
    static constexpr size_t capacity = 64 * 1024, guard = 32;
    static constexpr size_t stride = capacity + 2 * guard, bytes = 2 * stride;
    uint8_t* base;
    uint8_t* slot(unsigned i) const { return base + i * stride + guard; }
    void initialize_guards() const {
        for (unsigned i = 0; i < 2; ++i) {
            auto* before = reinterpret_cast<volatile uint8_t*>(slot(i) - guard);
            auto* after = reinterpret_cast<volatile uint8_t*>(slot(i) + capacity);
            for (size_t n = 0; n < guard; ++n) { before[n] = 0xa5; after[n] = 0x5a; }
        }
    }
    bool intact() const {
        for (unsigned i = 0; i < 2; ++i) {
            const auto* before = reinterpret_cast<const volatile uint8_t*>(slot(i) - guard);
            const auto* after = reinterpret_cast<const volatile uint8_t*>(slot(i) + capacity);
            for (size_t n = 0; n < guard; ++n)
                if (before[n] != 0xa5 || after[n] != 0x5a) return false;
        }
        return true;
    }
};

}  // namespace native_badge
