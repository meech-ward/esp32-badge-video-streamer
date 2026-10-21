// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstddef>
#include <cstdint>

namespace native_badge::psram {

inline constexpr std::uintptr_t cached_base = 0x11000000;
inline constexpr std::uintptr_t uncached_base = 0x15000000;

enum class Error : unsigned { None, InvalidTiming, Timeout, NotDetected };

struct InitResult {
    std::size_t bytes = 0;
    Error error = Error::None;
    std::uint8_t kgd = 0;
    std::uint8_t eid = 0;
    std::uint8_t clock_divisor = 0;
    std::uint8_t rx_delay = 0;
};

// Boot-only bring-up of Tufty 2350's GPIO8 CS1 APS6404 for the receiver and
// diagnostics. Call before core1, Wi-Fi, or any DMA that reads flash/PSRAM.
// The direct-QMI window executes entirely in SRAM with interrupts disabled.
// The caller must not access either PSRAM alias unless bytes is nonzero.
// Use ONE alias consistently for a physical buffer; cached writes are dirty
// until cleaned and cannot be read safely through the uncached alias.
InitResult init_boot_only();

}  // namespace native_badge::psram
