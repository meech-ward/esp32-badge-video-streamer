// Panel setup/PIO adapted from Pimoroni badgeware-cpp, MIT, 7d1ef0882c6e.
// Direct RGB565 DMA replaces its RGBA conversion/transpose. See THIRD_PARTY.md.
#include "display.hpp"
#include <initializer_list>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "pico/stdlib.h"
#include "st7789_parallel.pio.h"

namespace native_badge {
namespace {

PIO const lcd_pio = pio2;  // reserve an independent PIO bank for the high LCD pins
unsigned lcd_sm;
int lcd_dma = -1;
bool transferring = false;
std::uint32_t started_us = 0;
std::uint32_t completed_us = 0;
std::uint32_t completed_count = 0;
bool pixel_mode = false;
void select_pixel_mode(bool pixels_instead_of_commands);

std::uint32_t stall_mask() { return 1u << (lcd_sm + PIO_FDEBUG_TXSTALL_LSB); }

void drain_pio() {
    // Clear the sticky flag AFTER DMA completes. An early FIFO underrun must
    // not be mistaken for the final WR edge leaving the bus.
    lcd_pio->fdebug = stall_mask();
    while (!(lcd_pio->fdebug & stall_mask())) tight_loop_contents();
}

void write(const std::uint8_t* bytes, std::size_t length) {
    dma_channel_set_trans_count(lcd_dma, length, false);
    dma_channel_set_read_addr(lcd_dma, bytes, true);
    dma_channel_wait_for_finish_blocking(lcd_dma);
    drain_pio();
}

void command(std::uint8_t cmd, const char* bytes = nullptr, std::size_t length = 0) {
    wait();
    select_pixel_mode(false);
    gpio_put(BW_LCD_CS, 0);
    gpio_put(BW_LCD_DC, 0);
    write(&cmd, 1);
    if (length) {
        gpio_put(BW_LCD_DC, 1);
        write(reinterpret_cast<const std::uint8_t*>(bytes), length);
    }
    gpio_put(BW_LCD_CS, 1);
}

void configure_dma(bool increment) {
    auto cfg = dma_channel_get_default_config(lcd_dma);
    channel_config_set_transfer_data_size(&cfg,
        NATIVE_RGB565_LE && pixel_mode ? DMA_SIZE_16 : DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, increment);
    channel_config_set_write_increment(&cfg, false);
    channel_config_set_dreq(&cfg, pio_get_dreq(lcd_pio, lcd_sm, true));
    dma_channel_configure(lcd_dma, &cfg, &lcd_pio->txf[lcd_sm], nullptr, 0, false);
}

void select_pixel_mode(bool pixels_instead_of_commands) {
#if NATIVE_RGB565_LE
    if (pixel_mode == pixels_instead_of_commands) return;
    // Only change the pull threshold with an empty FIFO and completed WR.
    // Reset OSR's shift count too: an 8-bit command must not leave a half-full
    // word that the subsequent 16-bit pixel mode could consume as a pixel.
    pio_sm_set_enabled(lcd_pio, lcd_sm, false);
    auto shift = lcd_pio->sm[lcd_sm].shiftctrl;
    shift &= ~(PIO_SM0_SHIFTCTRL_PULL_THRESH_BITS | PIO_SM0_SHIFTCTRL_OUT_SHIFTDIR_BITS);
    shift |= ((pixels_instead_of_commands ? 16u : 8u) << PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB);
    lcd_pio->sm[lcd_sm].shiftctrl = shift;
    pio_sm_restart(lcd_pio, lcd_sm);
    pio_sm_set_enabled(lcd_pio, lcd_sm, true);
    pixel_mode = pixels_instead_of_commands;
    configure_dma(true);
#else
    (void)pixels_instead_of_commands;
#endif
}

void set_window(unsigned x, unsigned y, unsigned w, unsigned h) {
    const auto right = x + w - 1;
    const auto bottom = y + h - 1;
    const char columns[] = {static_cast<char>(x >> 8), static_cast<char>(x),
                            static_cast<char>(right >> 8), static_cast<char>(right)};
    const char rows[] = {static_cast<char>(y >> 8), static_cast<char>(y),
                         static_cast<char>(bottom >> 8), static_cast<char>(bottom)};
    command(0x2a, columns, sizeof columns);
    command(0x2b, rows, sizeof rows);
}

void start_pixels(const std::uint16_t* rgb565, std::size_t count) {
    command(0x2c);
    select_pixel_mode(true);
    gpio_put(BW_LCD_CS, 0);
    gpio_put(BW_LCD_DC, 1);
    started_us = time_us_32();
    transferring = true;
    // Narrow 16-bit APB writes replicate the native RGB565 value in both
    // halves of the FIFO word. Shift-left OUT8/OUT8 therefore sends MSB first
    // without a byte-swap; the default BE profile still transfers bytes.
    dma_channel_set_trans_count(lcd_dma, NATIVE_RGB565_LE ? count : count * sizeof(std::uint16_t), false);
    dma_channel_set_read_addr(lcd_dma, rgb565, true);
}

void panel_init() {
    command(0x01);  // SWRESET
    sleep_ms(150);
    command(0x3a, "\x05", 1);  // RGB565
    command(0xb2, "\x0c\x0c\x00\x33\x33", 5);
    command(0xc0, "\x2c", 1);
    command(0xc2, "\x01", 1);
    command(0xc3, "\x0f", 1);
    command(0xc4, "\x20", 1);
    command(0xd0, "\xa4\xa1", 2);
    command(0xc6, "\x05", 1);  // approximately 90 Hz, like Badgeware's mode setup
    command(0xb0, "\x00\xc0", 2);
    command(0xb7, "\x35", 1);
    command(0xbb, "\x1b", 1);
    command(0xe0, "\xf0\x00\x06\x04\x05\x05\x31\x44\x48\x36\x12\x12\x2b\x34", 14);
    command(0xe1, "\xf0\x0b\x0f\x0f\x0d\x26\x31\x43\x47\x38\x14\x14\x2c\x32", 14);
    command(0x21);  // INVON
    command(0x11);  // SLPOUT
    sleep_ms(100);
    // Landscape row-major. 0xb0 was verified in the existing Tufty native
    // port; the stock driver uses portrait GRAM and transposes every frame.
    command(0x36, "\xb0", 1);
    set_window(0, 0, width, height);
    command(0x35, "\x00", 1);
    command(0x44, "\x00\x00", 2);

    // Clear panel memory before lighting it; no framebuffer allocation needed.
    command(0x2c);
    gpio_put(BW_LCD_CS, 0);
    gpio_put(BW_LCD_DC, 1);
    const std::uint8_t black = 0;
    configure_dma(false);
    write(&black, pixels * sizeof(std::uint16_t));
    configure_dma(true);
    gpio_put(BW_LCD_CS, 1);
    command(0x29);  // DISPON
}

}  // namespace

void init() {
    gpio_init(BW_SW_POWER_EN);
    gpio_set_dir(BW_SW_POWER_EN, GPIO_OUT);
    gpio_put(BW_SW_POWER_EN, 1);
    sleep_ms(50);
    for (unsigned pin : {BW_LCD_CS, BW_LCD_DC, BW_LCD_RD}) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 1);
    }
    gpio_init(BW_LCD_VSYNC);
    gpio_set_dir(BW_LCD_VSYNC, GPIO_IN);

    // GPIO32..39 require RP2350B and the GPIO16..47 PIO window.
    pio_set_gpio_base(lcd_pio, 16);
    lcd_sm = pio_claim_unused_sm(lcd_pio, true);
    const auto offset = pio_add_program(lcd_pio, &st7789_parallel_program);
    pio_gpio_init(lcd_pio, BW_LCD_WR);
    for (unsigned pin = BW_LCD_D0; pin < BW_LCD_D0 + 8; ++pin) pio_gpio_init(lcd_pio, pin);
    pio_sm_set_consecutive_pindirs(lcd_pio, lcd_sm, BW_LCD_WR, 1, true);
    pio_sm_set_consecutive_pindirs(lcd_pio, lcd_sm, BW_LCD_D0, 8, true);
    auto cfg = st7789_parallel_program_get_default_config(offset);
    sm_config_set_out_pins(&cfg, BW_LCD_D0, 8);
    sm_config_set_sideset_pins(&cfg, BW_LCD_WR);
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
    sm_config_set_out_shift(&cfg, false, true, 8);
    sm_config_set_clkdiv(&cfg, static_cast<float>(clock_get_hz(clk_sys)) / 44'000'000.0f);
    pio_sm_init(lcd_pio, lcd_sm, offset, &cfg);
    pio_sm_set_enabled(lcd_pio, lcd_sm, true);
    lcd_dma = dma_claim_unused_channel(true);
    configure_dma(true);

    auto pwm = pwm_get_default_config();
    pwm_config_set_wrap(&pwm, 65535);
    pwm_init(pwm_gpio_to_slice_num(BW_LCD_BL), &pwm, true);
    gpio_set_function(BW_LCD_BL, GPIO_FUNC_PWM);
    pwm_set_gpio_level(BW_LCD_BL, 0);
    panel_init();
    pwm_set_gpio_level(BW_LCD_BL, 50000);
    for (unsigned pin : {BW_SWITCH_A, BW_SWITCH_B, BW_SWITCH_C, BW_SWITCH_UP,
                         BW_SWITCH_DOWN, BW_SWITCH_HOME}) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_up(pin);
    }
}

bool busy() {
    if (!transferring) return false;
    if (dma_channel_is_busy(lcd_dma)) return true;
    // Re-arm TXSTALL only once DMA is finished; then the next stall proves
    // every queued byte reached the panel, including the final WR pulse.
    drain_pio();
    transferring = false;
    completed_us = time_us_32() - started_us;
    ++completed_count;
    gpio_put(BW_LCD_CS, 1);
    return false;
}

void wait() { while (busy()) tight_loop_contents(); }

bool present(const std::uint16_t* rgb565, bool wait_te) {
    if (rgb565 == nullptr || busy()) return false;
    if (wait_te) {
        const auto until = time_us_32();
        while (!gpio_get(BW_LCD_VSYNC) && time_us_32() - until < 25'000) tight_loop_contents();
    }
    set_window(0, 0, width, height);  // a prior status strip may have narrowed GRAM
    start_pixels(rgb565, pixels);
    return true;
}

bool present_rect(unsigned x, unsigned y, unsigned w, unsigned h, const std::uint16_t* rgb565) {
    if (!rgb565 || !w || !h || x >= width || y >= height || w > width - x || h > height - y) return false;
    if (busy()) return false;
    set_window(x, y, w, h);
    start_pixels(rgb565, static_cast<std::size_t>(w) * h);
    return true;
}

std::uint32_t last_transfer_us() { return completed_us; }
std::uint32_t transfer_count() { return completed_count; }

std::uint16_t buttons() {
    std::uint16_t result = 0;
    if (!gpio_get(BW_SWITCH_A)) result |= 1;
    if (!gpio_get(BW_SWITCH_B)) result |= 2;
    if (!gpio_get(BW_SWITCH_C)) result |= 4;
    if (!gpio_get(BW_SWITCH_UP)) result |= 8;
    if (!gpio_get(BW_SWITCH_DOWN)) result |= 16;
    return result;
}

bool home_pressed() { return !gpio_get(BW_SWITCH_HOME); }

}  // namespace native_badge
