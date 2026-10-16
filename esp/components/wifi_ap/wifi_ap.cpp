#include "wifi_ap.hpp"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

namespace wifi_ap {
namespace {

constexpr const char* TAG = "wifi_ap";

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    // video_tx drops a departed client on its own bounded TCP timeouts.
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        auto* e = static_cast<wifi_event_ap_staconnected_t*>(data);
        ESP_LOGI(TAG, "station " MACSTR " joined (aid %d)", MAC2STR(e->mac), e->aid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        auto* e = static_cast<wifi_event_ap_stadisconnected_t*>(data);
        ESP_LOGW(TAG, "station " MACSTR " left (aid %d, reason %d)", MAC2STR(e->mac), e->aid, e->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        auto* e = static_cast<ip_event_ap_staipassigned_t*>(data);
        ESP_LOGI(TAG, "station got " IPSTR, IP2STR(&e->ip));
    }
}

}  // namespace

esp_err_t start(const Options& opt) {
    const size_t pw_len = strlen(opt.password);
    const size_t ssid_len = strlen(opt.ssid);
    if (pw_len < 8 || pw_len > 63 || ssid_len == 0 || ssid_len > 32) {
        // Lengths only: never log the credentials themselves.
        ESP_LOGE(TAG, "bad SSID/password length (ssid %u, password %u); regenerate wifi_secrets.h",
                 static_cast<unsigned>(ssid_len), static_cast<unsigned>(pw_len));
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    if ((err = esp_netif_init()) != ESP_OK) return err;
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&init)) != ESP_OK) return err;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_event, nullptr));
    // Keep the AP config (and its password) out of NVS; it is rebuilt every boot.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t cfg = {};
    memcpy(cfg.ap.ssid, opt.ssid, ssid_len);
    cfg.ap.ssid_len = static_cast<uint8_t>(ssid_len);
    memcpy(cfg.ap.password, opt.password, pw_len);
    cfg.ap.channel = opt.channel;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = opt.max_clients;
    cfg.ap.beacon_interval = 100;
    // PMF capable but not required: whether the badge's CYW43 joins a
    // PMF-required AP is unverified.
    cfg.ap.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));

    // HT20, not HT40: one JPEG stream needs a few Mbit/s, far below HT20's
    // capacity, and a 20 MHz channel is more robust to 2.4 GHz neighbours and
    // is what most small clients (including the CYW43) actually use.
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power save only affects a station interface; set explicitly so an
    // AP+STA build later does not silently get modem sleep latency.
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_netif_ip_info_t ip = {};
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip);
    ESP_LOGI(TAG, "AP up: ch %u HT20 WPA2-PSK max %u clients, ip " IPSTR, opt.channel, opt.max_clients,
             IP2STR(&ip.ip));
    return ESP_OK;
}

}  // namespace wifi_ap
