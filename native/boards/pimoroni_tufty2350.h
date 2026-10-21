/* Copyright (c) 2024 Raspberry Pi (Trading) Ltd.
 * SPDX-License-Identifier: BSD-3-Clause
 * Tufty 2350 wiring, derived from Pimoroni's Tufty board definition.
 * This header is also read by the assembler: preprocessor directives only.
 */
// pico_cmake_set PICO_PLATFORM=rp2350
// pico_cmake_set PICO_CYW43_SUPPORTED = 1
#ifndef STREAMER_NATIVE_TUFTY2350_BOARD_H
#define STREAMER_NATIVE_TUFTY2350_BOARD_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)
pico_board_cmake_set(PICO_CYW43_SUPPORTED, 1)
pico_board_cmake_set(PICO_RP2350A, 0)
#define PICO_RP2350A 0

#define BW_SWITCH_A 7
#define BW_SWITCH_B 9
#define BW_SWITCH_C 10
#define BW_SWITCH_UP 11
#define BW_SWITCH_DOWN 6
#define BW_SWITCH_HOME 22
#define BW_SW_POWER_EN 41
#define BW_LCD_CS 27
#define BW_LCD_DC 28
#define BW_LCD_WR 30
#define BW_LCD_RD 31
#define BW_LCD_D0 32
#define BW_LCD_BL 26
#define BW_LCD_VSYNC 21

// Match the preserved Badgeware firmware's CPU and voltage; no new overclock.
#define PLL_SYS_REFDIV 1
#define PLL_SYS_VCO_FREQ_HZ 1500000000
#define PLL_SYS_POSTDIV1 6
#define PLL_SYS_POSTDIV2 1
#define SYS_CLK_HZ 250000000
#define SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST 1
#define SYS_CLK_VREG_VOLTAGE_MIN 0b01101

#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1
#define PICO_FLASH_SPI_CLKDIV 4
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#define PICO_RP2350_A2_SUPPORTED 1

#define CYW43_WL_GPIO_COUNT 3
#define CYW43_PIN_WL_DYNAMIC 0
#define CYW43_DEFAULT_PIN_WL_REG_ON 23u
#define CYW43_DEFAULT_PIN_WL_DATA_OUT 24u
#define CYW43_DEFAULT_PIN_WL_DATA_IN 24u
#define CYW43_DEFAULT_PIN_WL_HOST_WAKE 24u
#define CYW43_DEFAULT_PIN_WL_CLOCK 29u
#define CYW43_DEFAULT_PIN_WL_CS 25u
#define CYW43_PIO_CLOCK_DIV_INT 4

#endif
