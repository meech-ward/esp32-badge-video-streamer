// WPA2 SoftAP at 192.168.4.1 (esp_netif default AP address and DHCP server).
#pragma once

#include <cstdint>

#include "esp_err.h"

namespace wifi_ap {

struct Options {
    const char* ssid;
    const char* password;  // 8..63 characters
    uint8_t channel;       // fixed, 1..13
    uint8_t max_clients;
};

// Requires nvs_flash_init() and the default event loop.
esp_err_t start(const Options& opt);

}  // namespace wifi_ap
