/*
 * Copyright (c) 2026 Raspberry Pi (Trading) Ltd.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Timing/quad configuration adapted from Pico SDK hardware_psram at
 * 98a542c1a62fb549ffb5d66a3e5892b06276b670. See THIRD_PARTY.md.
 * The small boot-only direct probe supports the receiver and diagnostics on
 * SDK 2.2.0 without importing newer runtime/linker/flash callback machinery.
 */
#include "psram.hpp"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/padsbank0.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "pico.h"

namespace native_badge::psram {
namespace {
constexpr unsigned kCs = 8;
constexpr std::uint32_t kTimeout = 1u << 31;

// No function calls, literals in flash, interrupts or other core may access
// XIP while direct mode is enabled. Bounded polling also makes an absent or
// unresponsive device fail initialization instead of waiting forever.
__attribute__((always_inline)) inline bool direct_idle() {
    for (unsigned i = 0; i < 1'000'000; ++i) {
        if (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS)) return true;
    }
    return false;
}

__attribute__((always_inline)) inline bool receive_ready() {
    for (unsigned i = 0; i < 1'000'000; ++i) {
        if (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_RXEMPTY_BITS)) return true;
    }
    return false;
}

__attribute__((always_inline)) inline void deselect() {
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    // APS6404 requires at least 18ns CS-high between commands. Eight CPU
    // cycles give 32ns at the fixed 250MHz prototype clock, plus bus latency.
    __asm volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop" ::: "memory");
}

std::uint32_t __no_inline_not_in_flash_func(probe_and_configure)(std::uint32_t timing) {
    qmi_hw->direct_csr = (10u << QMI_DIRECT_CSR_CLKDIV_LSB) | QMI_DIRECT_CSR_EN_BITS;
    if (!direct_idle()) { qmi_hw->direct_csr = 0; return kTimeout; }

    // Exit a QPI state retained across reset, then read the APS6404 JEDEC ID
    // over a single data lane. Only CS1 is asserted: flash CS0 is untouched.
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
        (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) | 0xf5u;
    if (!direct_idle() || !receive_ready()) { qmi_hw->direct_csr = 0; return kTimeout; }
    (void)qmi_hw->direct_rx;
    deselect();

    std::uint32_t id = 0;
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    for (unsigned i = 0; i < 8; ++i) {
        qmi_hw->direct_tx = i == 0 ? 0x9fu : 0xffu;
        if (!direct_idle() || !receive_ready()) { qmi_hw->direct_csr = 0; return kTimeout; }
        const auto value = qmi_hw->direct_rx & 0xffu;
        if (i == 5) id |= value;
        if (i == 6) id |= value << 8;
    }
    deselect();
    if ((id & 0xffu) != 0x5du) { qmi_hw->direct_csr = 0; return id; }

    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = 0x35u;  // enter quad mode
    if (!direct_idle() || !receive_ready()) { qmi_hw->direct_csr = 0; return kTimeout; }
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr = 0;

    qmi_hw->m[1].timing = timing;
    qmi_hw->m[1].rcmd = 0xebu << QMI_M1_RCMD_PREFIX_LSB;
    qmi_hw->m[1].rfmt =
        (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB) |
        (QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB) |
        (QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB) |
        (QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB) |
        (QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB);
    qmi_hw->m[1].wcmd = 0x38u << QMI_M1_WCMD_PREFIX_LSB;
    qmi_hw->m[1].wfmt =
        (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB) |
        (QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB) |
        (QMI_M1_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_WFMT_DUMMY_WIDTH_LSB) |
        (QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB) |
        (QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB);
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;
    return id;
}
}  // namespace

InitResult init_boot_only() {
    InitResult result;
    const auto hz = clock_get_hz(clk_sys);
    if (!hz) { result.error = Error::InvalidTiming; return result; }
    const auto divisor = (hz + 133'000'000u - 1u) / 133'000'000u;
    const auto div = divisor == 1 && hz > 100'000'000u ? 2u : divisor;
    const auto rx = div + (hz / div > 100'000'000u ? 1u : 0u);
    const auto period_fs = 1'000'000'000'000'000ull / hz;
    const auto max_select = 8'000'000'000ull / (64u * period_fs);
    const auto deselect = (18'000'000ull + period_fs - 1) / period_fs;
    const auto half_div = (div + 1u) / 2u;
    if (!div || div > 255 || rx > 7 || !max_select || max_select > 63 ||
        deselect < half_div || deselect - half_div > 31) {
        result.error = Error::InvalidTiming;
        return result;
    }
    result.clock_divisor = static_cast<std::uint8_t>(div);
    result.rx_delay = static_cast<std::uint8_t>(rx);
    const auto timing = (1u << QMI_M1_TIMING_COOLDOWN_LSB) |
        (QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB) |
        (static_cast<std::uint32_t>(max_select) << QMI_M1_TIMING_MAX_SELECT_LSB) |
        (static_cast<std::uint32_t>(deselect - half_div) << QMI_M1_TIMING_MIN_DESELECT_LSB) |
        (rx << QMI_M1_TIMING_RXDELAY_LSB) | (div << QMI_M1_TIMING_CLKDIV_LSB);

    gpio_set_function(kCs, GPIO_FUNC_XIP_CS1);
    // RP2350-E14: the A2 boot ROM can clear isolation on GPIO0 instead of
    // the configured CS1 pad. Explicitly clear it on Tufty's actual GPIO8.
    hw_clear_bits(&pads_bank0_hw->io[kCs], PADS_BANK0_GPIO0_ISO_BITS);
    const auto saved = save_and_disable_interrupts();
    const auto id = probe_and_configure(timing);
    restore_interrupts(saved);
    result.kgd = static_cast<std::uint8_t>(id);
    result.eid = static_cast<std::uint8_t>(id >> 8);
    if (id & kTimeout) result.error = Error::Timeout;
    else if (result.kgd != 0x5d) result.error = Error::NotDetected;
    else {
        const auto density = result.eid >> 5;
        if (result.eid == 0x26 || density == 2 || density == 3) result.bytes = 8u * 1024 * 1024;
        else if (density == 0) result.bytes = 2u * 1024 * 1024;
        else if (density == 1) result.bytes = 4u * 1024 * 1024;
        else if (density == 4) result.bytes = 16u * 1024 * 1024;
        else result.error = Error::NotDetected;
    }
    return result;
}

}  // namespace native_badge::psram
