#include "display.hpp"
#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include <cstdio>

alignas(4) static std::uint16_t frame[native_badge::pixels];

int main() {
    stdio_init_all();
    native_badge::init();
    for (unsigned y = 0; y < native_badge::height; ++y) {
        for (unsigned x = 0; x < native_badge::width; ++x) {
            const unsigned r = x * 255 / (native_badge::width - 1);
            const unsigned g = y * 255 / (native_badge::height - 1);
            frame[y * native_badge::width + x] = native_badge::rgb565(r, g, 40);
        }
    }
    auto next = time_us_64();
    bool rectangle_next = false;
    while (true) {
        if (native_badge::home_pressed()) reset_usb_boot(0, 0);
        if (!native_badge::busy() && time_us_64() >= next) {
            // Copy the gradient's top 13 rows into the footer, then restore
            // the complete gradient. This exercises both window directions.
            if (rectangle_next) {
                native_badge::present_rect(0, native_badge::height - 13,
                                          native_badge::width, 13, frame);
            } else {
                native_badge::present(frame);
            }
            rectangle_next = !rectangle_next;
            next += 500'000;
            if (stdio_usb_connected()) {
                std::printf("NATIVE_DISPLAY width=320 height=240 cpu_hz=%lu dma_us=%lu frames=%lu buttons=%u\n",
                            static_cast<unsigned long>(SYS_CLK_HZ),
                            static_cast<unsigned long>(native_badge::last_transfer_us()),
                            static_cast<unsigned long>(native_badge::transfer_count()), native_badge::buttons());
            }
        }
        sleep_us(100);
    }
}
