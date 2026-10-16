// JPEG capture on the XIAO ESP32-S3 Sense camera connector.
#pragma once

#include <cstdint>
#include "esp_err.h"

namespace camera_source {

struct Options {
    uint8_t framesize;  // streamer::kFs* code
    uint8_t quality;    // 4..63, lower = better
    bool vflip;
    bool hmirror;
};

// Call once at boot, before Wi-Fi so DMA buffers get internal RAM first.
esp_err_t init(const Options& opt);
bool ready();
// Thread-safe; HELLO changes size and quality before sending frames.
esp_err_t configure(uint8_t framesize, uint8_t quality);
uint8_t framesize();
uint8_t quality();
const char* sensor_name();

// Software AWB drives manual gains on OV2640/OV5640. Other sensors use their
// own automatic white balance. Controller units: 0x40 is approximately 1.0x;
// OV5640's 12-bit register gains are this value shifted left by four.
enum class WbControl : uint8_t { Hw = 0, Soft = 1 };
WbControl wb_control();
const char* wb_control_name(WbControl c);
esp_err_t write_soft_gains(uint8_t r, uint8_t g, uint8_t b);
void soft_gains(uint8_t& r, uint8_t& g, uint8_t& b);

struct Frame {
    const uint8_t* data;
    uint32_t len;
    uint16_t width;
    uint16_t height;
    int64_t timestamp_us;  // driver capture-start time, esp_timer clock
};

// Only video_tx may consume frames. grab() copies the newest completed JPEG
// and returns the driver buffer immediately so capture continues during send.
// data remains valid until the next grab(); reject len above kMaxPayload.
bool grab(Frame& out, uint32_t timeout_ms);
// Same bounded wait without copying the JPEG, used between client sessions.
bool skip(uint32_t& len, uint32_t timeout_ms);

}  // namespace camera_source
