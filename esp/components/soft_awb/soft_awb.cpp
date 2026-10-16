#include "soft_awb.hpp"

#include "camera_source.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "video_tx.hpp"
#include "streamer/white_balance.hpp"

namespace soft_awb {
namespace {

constexpr const char* TAG = "soft_awb";

// Core 1 below the camera task: reduced-scale decoding uses spare time
// without delaying capture.
constexpr UBaseType_t kPrio = 2;
// ~3 measurements a second. The period alone does not guarantee a frame shows
// the last write (at low fps a sampled frame can be several hundred ms old),
// so frames are also gated on their capture time (streamer::awb::FreshGate).
constexpr uint32_t kPeriodMs = 330;
// WHY 50 ms after the write: the frame timestamp is taken at capture start,
// so a frame that starts after the write is processed with the new gains if
// the DSP applies them at once; the margin also covers a DSP that latches them
// slightly before frame start (not measured).
constexpr int64_t kSettleUs = 50'000;
// Stale frames re-sampled per period before giving up (each waits one frame).
constexpr int kMaxStale = 8;
constexpr uint32_t kSampleTimeoutMs = 500;

// Decode to at most 80x60: enough pixels to find grey areas (4800), small
// enough to keep the decode cheap and the buffer at 14 KiB for every fs
// (160x120 /2, 320x240 /4, 240x240 /4 -> 60x60, 640x480 /8).
constexpr int kMaxW = 80, kMaxH = 60;
constexpr size_t kRgbBytes = kMaxW * kMaxH * 3;
// esp_jpeg's recommended TJpgDec work area for the ROM decoder.
constexpr size_t kWorkBytes = 3100;

uint8_t* s_rgb = nullptr;
uint8_t* s_work = nullptr;
esp_jpeg_image_scale_t pick_scale(uint16_t w, uint16_t h) {
    if (w <= kMaxW && h <= kMaxH) return JPEG_IMAGE_SCALE_0;
    if (w <= 2 * kMaxW && h <= 2 * kMaxH) return JPEG_IMAGE_SCALE_1_2;
    if (w <= 4 * kMaxW && h <= 4 * kMaxH) return JPEG_IMAGE_SCALE_1_4;
    return JPEG_IMAGE_SCALE_1_8;
}

void awb_task(void*) {
    streamer::awb::Controller ctl;
    streamer::awb::FreshGate gate(kSettleUs);
    uint8_t r, g, b;
    camera_source::soft_gains(r, g, b);
    ctl.reset({static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)});
    gate.wrote(esp_timer_get_time());
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
        video_tx::Sample f;
        bool got = false;
        for (int i = 0; i <= kMaxStale; ++i) {
            got = video_tx::sample(f, kSampleTimeoutMs);
            if (!got || gate.fresh(f.timestamp_us)) break;
            got = false;
        }
        if (!got) continue;
        const esp_jpeg_image_scale_t scale = pick_scale(f.width, f.height);
        esp_jpeg_image_cfg_t cfg = {};
        cfg.indata = const_cast<uint8_t*>(f.data);
        cfg.indata_size = f.len;
        cfg.outbuf = s_rgb;
        cfg.outbuf_size = kRgbBytes;
        cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;  // TJpgDec byte order R, G, B
        cfg.out_scale = scale;
        cfg.advanced.working_buffer = s_work;
        cfg.advanced.working_buffer_size = kWorkBytes;
        esp_jpeg_image_output_t img = {};
        if (esp_jpeg_decode(&cfg, &img) != ESP_OK) continue;
        const streamer::awb::Means m = streamer::awb::analyse(s_rgb, static_cast<size_t>(img.width) * img.height);
        if (ctl.step(m) == streamer::awb::Result::Adjusted) {
            camera_source::write_soft_gains(ctl.reg(0), ctl.reg(1), ctl.reg(2));
            // Gate even a partial failed write: a register may have changed.
            gate.wrote(esp_timer_get_time());
        }
    }
}

}  // namespace

void start() {
    if (!camera_source::ready() || camera_source::wb_control() != camera_source::WbControl::Soft) return;
    s_rgb = static_cast<uint8_t*>(heap_caps_malloc(kRgbBytes, MALLOC_CAP_SPIRAM));
    s_work = static_cast<uint8_t*>(heap_caps_malloc(kWorkBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!s_rgb || !s_work) {
        ESP_LOGE(TAG, "no memory for the AWB buffers; soft AWB measurement disabled");
        return;
    }
    xTaskCreatePinnedToCore(awb_task, "soft_awb", 4096, nullptr, kPrio, nullptr, 1);
}

}  // namespace soft_awb
