#include "console.hpp"
#include "sdkconfig.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// Log V2 locks stdout before its callback, bypassing a nonblocking queue.
#if CONFIG_LOG_VERSION != 1
#error "Nonblocking USB logging requires CONFIG_LOG_VERSION_1=y"
#endif

namespace console {
namespace {
struct LogLine { size_t len; char text[512]; };
constexpr UBaseType_t kQueueDepth = 8;
QueueHandle_t s_queue = nullptr;

int queued_vprintf(const char* format, va_list args) {
    LogLine line;
    const int length = vsnprintf(line.text, sizeof line.text, format, args);
    if (length < 0) return 0;
    line.len = std::min<size_t>(static_cast<size_t>(length), sizeof line.text - 1);
    if (static_cast<size_t>(length) >= sizeof line.text) {
        line.text[sizeof line.text - 3] = '~';
        line.text[sizeof line.text - 2] = '\n';
    }
    // A slow/disconnected USB host may drop a warning but never block video.
    if (s_queue) xQueueSend(s_queue, &line, 0);
    return length;
}

void log_task(void*) {
    LogLine line;
    for (;;) {
        if (xQueueReceive(s_queue, &line, portMAX_DELAY) == pdTRUE)
            fwrite(line.text, 1, line.len, stdout);
    }
}
}  // namespace

void start() {
    s_queue = xQueueCreate(kQueueDepth, sizeof(LogLine));
    if (!s_queue || xTaskCreatePinnedToCore(log_task, "usb_logs", 4096, nullptr, 1, nullptr, 0) != pdPASS) {
        if (s_queue) vQueueDelete(s_queue);
        s_queue = nullptr;
        // Allocation failure still must not restore blocking SDK output.
        esp_log_set_vprintf(queued_vprintf);
        return;
    }
    esp_log_set_vprintf(queued_vprintf);
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW("console", "USB log driver install failed");
        return;
    }
    usb_serial_jtag_vfs_use_driver();
}
}  // namespace console
