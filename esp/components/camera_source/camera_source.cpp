#include "camera_source.hpp"

#include <atomic>
#include <cstring>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "streamer/protocol.hpp"
#include "streamer/white_balance.hpp"

namespace camera_source {
namespace {

constexpr const char* TAG = "camera";

// XIAO ESP32-S3 Sense camera connector pin map; GPIO21 is not a camera pin.
constexpr int PIN_PWDN = -1;
constexpr int PIN_RESET = -1;
constexpr int PIN_XCLK = 10;
constexpr int PIN_SIOD = 40;
constexpr int PIN_SIOC = 39;
constexpr int PIN_D0 = 15, PIN_D1 = 17, PIN_D2 = 18, PIN_D3 = 16;
constexpr int PIN_D4 = 14, PIN_D5 = 12, PIN_D6 = 11, PIN_D7 = 48;
constexpr int PIN_VSYNC = 38, PIN_HREF = 47, PIN_PCLK = 13;

// WHY 20 MHz: on the S3, XCLK is 160 MHz / integer (LCD_CAM), so 20 MHz is
// exact whereas 24 MHz would come out as 26.67 MHz.
constexpr int XCLK_HZ = 20'000'000;

// WHY a fixed JPEG buffer: the automatic size (w*h/5) is too small at QQVGA
// with low quality numbers, and frame buffers are sized once at init. 128 KiB
// is the protocol's MAX_PAYLOAD, so every frame that fits is also sendable.
// Two driver buffers plus the send copy in PSRAM is 384 KiB of 8 MiB.
constexpr size_t JPEG_BUF = streamer::kMaxPayload;
constexpr int FB_COUNT = 2;

// After a frame-size change the frames already captured (one queued, one in
// capture) still have the old size; drop them so headers and payload agree.
constexpr int DROP_AFTER_RESIZE = FB_COUNT;

constexpr framesize_t kFsMap[streamer::kFsCount] = {FRAMESIZE_QQVGA, FRAMESIZE_QVGA, FRAMESIZE_240X240,
                                               FRAMESIZE_VGA};

bool s_ready = false;
SemaphoreHandle_t s_lock = nullptr;  // serialises sensor (SCCB) register writes
std::atomic<uint8_t> s_fs{0};
std::atomic<uint8_t> s_q{6};
std::atomic<int> s_drop{0};
const char* s_name = "none";
uint8_t* s_copy = nullptr;  // PSRAM, JPEG_BUF bytes; backs the Frame from grab()

// ------------------------------------------------------------- clock ----

// Sensor PLL fields; only the measured OV5640 QQVGA override is used.
struct Pll {
    int mult, sys_div, pre_div, root_2x, pclk_root_div, pclk_manual, pclk_div;
};

// OV5640 at QQVGA (kFs code 0): SYSCLK 80 MHz instead of the driver's 40 MHz.
// WHY: esp32-camera's set_framesize writes set_pll(mult 160, sys_div 4,
// pre_div 2, root_2x 0, pclk_root_div 2, pclk_manual 1, pclk_div 4) = SYSCLK
// 40 MHz, PCLK 10 MHz for sizes below QVGA, which with the binned 2060 x 984
// timing is 19.5-20 fps at 160x120 (measured 2026-10-07, XCLK 20 MHz, JPEG
// q6, 4.1 KB frames). sys_div 2 with pclk_div 8 (SYSCLK 80 MHz, PCLK still
// 10 MHz) gave 34-36 fps with valid 3.2-3.5 KB frames and a snapshot that
// decodes to the real scene. The failure points around it: pclk_div 4 (PCLK
// 20 MHz) ran 36 then 25 fps, less stable; mult 200 (100 MHz) and 240
// (120 MHz) gave 40 fps of 1.2 KB frames that decode to a flat purple blob
// (broken readout); mult 180 with sys_div 2 (90 MHz) at 320x240 stalled the
// sensor (0 fps). 80 MHz at 320x240 was 14.5-15.5 fps against the driver's
// 11-16, and 19-20 fps at 240x240 against 15: no clear gain, so only
// QQVGA is overridden and the other sizes keep the driver's choice. The
// driver rewrites the PLL on every set_framesize, so this is re-applied
// after each one (init and configure()).
constexpr Pll kOv5640Fs0Pll = {160, 2, 2, 0, 2, 1, 8};
constexpr uint8_t kOv5640OverrideFs = 0;  // streamer kFs code: 160x120

// Caller holds s_lock (or is init). ESP_ERR_NOT_SUPPORTED without the hook.
esp_err_t set_pll_locked(sensor_t* s, const Pll& p) {
    // ov2640.c installs a stub hook that always fails; report that as unsupported, not as a failed write.
    if (!s->set_pll || s->id.PID == OV2640_PID) return ESP_ERR_NOT_SUPPORTED;
    // esp32-camera's hook order: (bypass, multiplier, sys_div, root_2x, pre_div, seld5 = pclk_root_div, ...)
    return s->set_pll(s, 0, p.mult, p.sys_div, p.root_2x, p.pre_div, p.pclk_root_div, p.pclk_manual, p.pclk_div) == 0
               ? ESP_OK
               : ESP_FAIL;
}

// Caller holds s_lock (or is init). After set_framesize: re-apply the
// per-size clock override where there is one.
void apply_clock_locked(sensor_t* s, uint8_t fs) {
    if (s->id.PID != OV5640_PID || fs != kOv5640OverrideFs) return;
    if (set_pll_locked(s, kOv5640Fs0Pll) == ESP_OK)
        ESP_LOGI(TAG, "ov5640 fs%u clock: sys_div %d -> ~35 fps", fs, kOv5640Fs0Pll.sys_div);
    else
        ESP_LOGW(TAG, "ov5640 fs%u clock override failed; driver clock (~20 fps) stays", fs);
}

// --------------------------------------------------------- white balance ----

// The manual white-balance registers per sensor. Addresses are in the form
// the driver's set_reg/get_reg take: (bank << 8) | addr on the OV2640, the
// 16-bit register address on the OV5640. gain_mask 0xFF writes one DSP byte;
// 0x0FFF makes ov5640.c's set_reg a 16-bit read-modify-write of the even/odd
// pair (a mask above 0xFF selects read_reg16/write_reg16 there), which is one
// call per channel and keeps the pair's reserved high nibble.
struct WbMap {
    const int* gain_reg;  // R, G, B
    int gain_mask;
    int ctrl_reg;
    int manual_bit;
    uint16_t (*to_reg)(uint8_t);  // controller units (0x40 ~ 1.0x) to the register value
};
constexpr WbMap kOv2640Wb = {streamer::awb::ov2640::kGainReg, 0xFF, streamer::awb::ov2640::kCtrlReg,
                             streamer::awb::ov2640::kManualBit, [](uint8_t v) { return static_cast<uint16_t>(v); }};
constexpr WbMap kOv5640Wb = {streamer::awb::ov5640::kGainReg, 0x0FFF, streamer::awb::ov5640::kCtrlReg,
                             streamer::awb::ov5640::kManualBit, [](uint8_t v) { return streamer::awb::ov5640_gain(v); }};

// nullptr: soft AWB not supported on this sensor.
const WbMap* wb_map(const sensor_t* s) {
    switch (s->id.PID) {
        case OV2640_PID: return &kOv2640Wb;
        case OV5640_PID: return &kOv5640Wb;
        default: return nullptr;
    }
}

std::atomic<uint8_t> s_wb{static_cast<uint8_t>(WbControl::Hw)};
std::atomic<uint8_t> s_soft[3] = {static_cast<uint8_t>(streamer::awb::kStartGains.r),
                                  static_cast<uint8_t>(streamer::awb::kStartGains.g),
                                  static_cast<uint8_t>(streamer::awb::kStartGains.b)};
int s_written[3] = {-1, -1, -1};  // last value written to the gain registers, controller units, -1 unknown

// Caller holds s_lock. force: also rewrite registers believed unchanged.
esp_err_t write_gains_locked(sensor_t* s, const uint8_t g[3], bool force) {
    const WbMap* m = wb_map(s);
    if (!m) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t err = ESP_OK;
    for (int c = 0; c < 3; ++c) {
        if (!force && s_written[c] == g[c]) continue;
        if (s->set_reg(s, m->gain_reg[c], m->gain_mask, m->to_reg(g[c])) != 0) {
            s_written[c] = -1;
            err = ESP_FAIL;
        } else {
            s_written[c] = g[c];
            s_soft[c] = g[c];
        }
    }
    return err;
}

// Caller holds s_lock (or is init).
esp_err_t enter_soft_locked() {
    sensor_t* s = esp_camera_sensor_get();
    const WbMap* m = wb_map(s);
    if (!m) return ESP_ERR_NOT_SUPPORTED;
    esp_err_t err = ESP_OK;
    if (s->status.wb_mode != 0 && s->set_wb_mode(s, 0) != 0) err = ESP_FAIL;
    if (err == ESP_OK && !s->status.awb && s->set_whitebal(s, 1) != 0) err = ESP_FAIL;
    if (err == ESP_OK && !s->status.awb_gain && s->set_awb_gain(s, 1) != 0) err = ESP_FAIL;
    const uint8_t g[3] = {s_soft[0], s_soft[1], s_soft[2]};
    // Gains first, then the manual bit, so no frame uses stale manual gains.
    if (err == ESP_OK) err = write_gains_locked(s, g, true);
    if (err == ESP_OK && s->set_reg(s, m->ctrl_reg, m->manual_bit, m->manual_bit) != 0) err = ESP_FAIL;
    if (err == ESP_OK) s_wb = static_cast<uint8_t>(WbControl::Soft);
    else if (s_wb.load() != static_cast<uint8_t>(WbControl::Soft))
        s_wb = static_cast<uint8_t>(WbControl::Hw);
    return err;
}

// Fixed colour defaults applied on top of the driver's initialization table.
// Lens correction is enabled explicitly: the OV2640 table leaves it off.
void check_default(int result, const char* name) {
    if (result != 0) ESP_LOGW(TAG, "default %s not applied", name);
}

void apply_defaults_locked(sensor_t* s, bool vflip, bool hmirror) {
    check_default(s->set_whitebal(s, 1), "awb");
    check_default(s->set_awb_gain(s, 1), "awb_gain");
    check_default(s->set_wb_mode(s, 0), "wb_mode");
    check_default(s->set_lenc(s, 1), "lenc");
    check_default(s->set_raw_gma(s, 1), "raw_gma");
    check_default(s->set_vflip(s, vflip ? 1 : 0), "vflip");
    check_default(s->set_hmirror(s, hmirror ? 1 : 0), "hmirror");
}

// WHY: the OV2640 frame-size tables (ov2640_settings_to_cif/svga/uxga) write
// CTRL2 with DCW on and COM7 with the colour-bar bit clear, without updating
// sensor->status. Rewrite both from status after every set_framesize so the
// hardware matches the active settings.
void restore_after_resize_locked(sensor_t* s) {
    if (s->id.PID != OV2640_PID) return;
    s->set_dcw(s, s->status.dcw);
    s->set_colorbar(s, s->status.colorbar);
}

// Next driver frame (resize leftovers skipped), or nullptr after timeout_ms.
// WHY poll instead of esp_camera_fb_get() alone: fb_get waits up to 4 s, and
// the only caller is the video task, which must keep sending STATUS, polling
// accept() and enforcing timeouts if the sensor stalls. fb_get is called only
// once a frame is queued, so it returns at once (single consumer). The poll
// adds at most one 1 ms tick of latency.
camera_fb_t* take(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        if (esp_camera_available_frames()) {
            camera_fb_t* fb = esp_camera_fb_get();
            if (!fb) return nullptr;
            if (s_drop.load() > 0) {
                s_drop.fetch_sub(1);
                esp_camera_fb_return(fb);
                continue;
            }
            return fb;
        }
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return nullptr;
        vTaskDelay(1);
    }
}

}  // namespace

esp_err_t init(const Options& opt) {
    if (!streamer::valid_framesize(opt.framesize) || !streamer::valid_quality(opt.quality)) return ESP_ERR_INVALID_ARG;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    camera_config_t cfg = {};
    cfg.pin_pwdn = PIN_PWDN;
    cfg.pin_reset = PIN_RESET;
    cfg.pin_xclk = PIN_XCLK;
    cfg.pin_sccb_sda = PIN_SIOD;
    cfg.pin_sccb_scl = PIN_SIOC;
    cfg.pin_d7 = PIN_D7;
    cfg.pin_d6 = PIN_D6;
    cfg.pin_d5 = PIN_D5;
    cfg.pin_d4 = PIN_D4;
    cfg.pin_d3 = PIN_D3;
    cfg.pin_d2 = PIN_D2;
    cfg.pin_d1 = PIN_D1;
    cfg.pin_d0 = PIN_D0;
    cfg.pin_vsync = PIN_VSYNC;
    cfg.pin_href = PIN_HREF;
    cfg.pin_pclk = PIN_PCLK;
    cfg.xclk_freq_hz = XCLK_HZ;
    cfg.ledc_timer = LEDC_TIMER_0;  // unused on the S3 (XCLK comes from LCD_CAM)
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = PIXFORMAT_JPEG;
    // WHY init at VGA: DMA line sizing is derived from the init frame size, so
    // start at the largest size we will ever request, then shrink below.
    cfg.frame_size = FRAMESIZE_VGA;
    cfg.jpeg_quality = opt.quality;
    cfg.fb_count = FB_COUNT;
    cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_LATEST;
    cfg.sccb_i2c_port = -1;
    cfg.jpeg_buffer_size = JPEG_BUF;

    s_copy = static_cast<uint8_t*>(heap_caps_malloc(JPEG_BUF, MALLOC_CAP_SPIRAM));
    if (!s_copy) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }
    sensor_t* s = esp_camera_sensor_get();
    if (camera_sensor_info_t* info = esp_camera_sensor_get_info(&s->id)) s_name = info->name;
    else s_name = "unknown";
    ESP_LOGI(TAG, "sensor %s PID 0x%04x VER 0x%02x MIDH 0x%02x MIDL 0x%02x", s_name, s->id.PID, s->id.VER,
             s->id.MIDH, s->id.MIDL);
    // Tested in the source project on XIAO units: OV2640 (first light 2026-10-06) and
    // OV5640 (2026-10-07). Anything else, the OV3660 included, has driver
    // support only.
    if (s->id.PID != OV2640_PID && s->id.PID != OV5640_PID)
        ESP_LOGW(TAG, "untested sensor PID 0x%04x; continuing", s->id.PID);

    // Frame size first: its register tables rewrite the PLL and colour
    // settings. Apply the clock override and colour defaults afterwards.
    if (s->set_framesize(s, kFsMap[opt.framesize]) != 0) ESP_LOGW(TAG, "set_framesize failed");
    else apply_clock_locked(s, opt.framesize);
    s->set_quality(s, opt.quality);
    apply_defaults_locked(s, opt.vflip, opt.hmirror);
    // Software AWB owns the gains on the supported sensors; other sensors
    // retain their own automatic white balance.
    if (wb_map(s) && enter_soft_locked() != ESP_OK)
        ESP_LOGW(TAG, "soft AWB not enabled; sensor AWB stays on");
    s_fs = opt.framesize;
    s_q = opt.quality;
    s_drop = DROP_AFTER_RESIZE;
    s_ready = true;
    ESP_LOGI(TAG, "ready: fs %u (%ux%u) q %u vflip %d hmirror %d, fb in PSRAM, %u KiB each", opt.framesize,
             streamer::kFrameSizes[opt.framesize].w, streamer::kFrameSizes[opt.framesize].h, opt.quality, opt.vflip,
             opt.hmirror, static_cast<unsigned>(JPEG_BUF / 1024));
    return ESP_OK;
}

bool ready() { return s_ready; }

esp_err_t configure(uint8_t fs, uint8_t q) {
    if (!streamer::valid_framesize(fs) || !streamer::valid_quality(q)) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sensor_t* s = esp_camera_sensor_get();
    esp_err_t err = ESP_OK;
    if (fs != s_fs) {
        if (s->set_framesize(s, kFsMap[fs]) != 0) {
            err = ESP_FAIL;
        } else {
            restore_after_resize_locked(s);  // OV2640: dcw/colorbar
            apply_clock_locked(s, fs);       // OV5640: fs0 PLL override
            s_fs = fs;
            s_drop = DROP_AFTER_RESIZE;
        }
    }
    if (err == ESP_OK && q != s_q) {
        if (s->set_quality(s, q) != 0) err = ESP_FAIL;
        else s_q = q;
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGW(TAG, "configure fs %u q %u failed", fs, q);
    else ESP_LOGI(TAG, "configured fs %u q %u", s_fs.load(), s_q.load());
    return err;
}

uint8_t framesize() { return s_fs; }
uint8_t quality() { return s_q; }
const char* sensor_name() { return s_name; }

WbControl wb_control() { return static_cast<WbControl>(s_wb.load()); }

const char* wb_control_name(WbControl c) { return c == WbControl::Soft ? "soft" : "hw"; }

esp_err_t write_soft_gains(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_wb.load() == static_cast<uint8_t>(WbControl::Soft)) {
        const uint8_t v[3] = {r, g, b};
        err = write_gains_locked(esp_camera_sensor_get(), v, false);
    }
    xSemaphoreGive(s_lock);
    return err;
}

void soft_gains(uint8_t& r, uint8_t& g, uint8_t& b) {
    r = s_soft[0];
    g = s_soft[1];
    b = s_soft[2];
}

bool grab(Frame& out, uint32_t timeout_ms) {
    if (!s_ready) return false;
    camera_fb_t* fb = take(timeout_ms);
    if (!fb) return false;
    // WHY copy and return the driver buffer at once: in CAMERA_GRAB_LATEST the
    // driver keeps capturing only while a buffer is free. Holding one of the
    // two across a slow send_all stops capture after one more frame, so the
    // next grab would get a frame that is a whole send old. With the buffer
    // back, the queue always holds the newest frame. The PSRAM copy costs
    // about 1-2 ms at VGA; fb_count = 3 would not help (queue depth is
    // fb_count - 1, so a held buffer still stalls capture).
    const size_t len = fb->len <= JPEG_BUF ? fb->len : 0;
    if (len) std::memcpy(s_copy, fb->buf, len);
    out.data = s_copy;
    out.len = static_cast<uint32_t>(fb->len);
    out.width = static_cast<uint16_t>(fb->width);
    out.height = static_cast<uint16_t>(fb->height);
    out.timestamp_us = static_cast<int64_t>(fb->timestamp.tv_sec) * 1'000'000 + fb->timestamp.tv_usec;
    esp_camera_fb_return(fb);
    return true;
}

bool skip(uint32_t& len, uint32_t timeout_ms) {
    if (!s_ready) return false;
    camera_fb_t* fb = take(timeout_ms);
    if (!fb) return false;
    len = static_cast<uint32_t>(fb->len);
    esp_camera_fb_return(fb);
    return true;
}

}  // namespace camera_source
