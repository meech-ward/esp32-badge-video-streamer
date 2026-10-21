#include "camera_source.hpp"
#include "camera_timed_get.h"

#include <atomic>
#include <cstring>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY
#include "driver/i2c.h"
#else
#include "driver/i2c_master.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "streamer/protocol.hpp"
#include "streamer/white_balance.hpp"
#include "streamer/ov5640_clock.hpp"
#include "streamer/ov5640_banding.hpp"

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
// Three driver buffers and the send copy use 512 KiB of 8 MiB PSRAM;
// the latest-frame queue stays at one so this adds no playback backlog.
constexpr size_t JPEG_BUF = streamer::kMaxPayload;
constexpr int FB_COUNT = CONFIG_STREAMER_CAMERA_FB_COUNT;
static_assert(FB_COUNT == 2 || FB_COUNT == 3);

// After a frame-size change the frames already captured (one queued, one in
// capture) still have the old size; drop them so headers and payload agree.
constexpr int DROP_AFTER_RESIZE = FB_COUNT;

constexpr framesize_t kFsMap[streamer::kFsCount] = {FRAMESIZE_QQVGA, FRAMESIZE_QVGA, FRAMESIZE_240X240,
                                               FRAMESIZE_VGA};

std::atomic<bool> s_ready{false};
SemaphoreHandle_t s_lock = nullptr;  // serialises sensor (SCCB) register writes
std::atomic<uint8_t> s_fs{0};
std::atomic<uint8_t> s_q{6};
std::atomic<int> s_drop{0};
const char* s_name = "none";
uint8_t* s_copy = nullptr;  // PSRAM, JPEG_BUF bytes; backs the Frame from grab()

// ------------------------------------------------------------- clock ----

// The sensor PLL fields accepted by the camera driver.
struct Pll {
    int mult, sys_div, pre_div, root_2x, pclk_root_div, pclk_manual, pclk_div;
};

// Keep the DVP pixel clock at 10 MHz while speeding sensor processing. With
// XCLK 20 MHz, the measured profiles are QQVGA SYSCLK 80 MHz and QVGA 70 MHz.
// QVGA's 2060x984 timing captures 34.5 fps, preserving 320x240 JPEG q6 detail.
// Both use system divider 1: QQVGA multiplier 80 and QVGA 70 retain the earlier
// 160 / 2 and 140 / 2 downstream clocks with 800 / 700 MHz VCOs (live checks,
// 2026-10-20). QVGA 75 MHz lost shadow/colour detail; 80 MHz stopped capture,
// even with a lower-VCO tuple.
// Clock arithmetic alone does not establish usable image output. Do not apply
// these profiles to other sizes/sensors. set_framesize rewrites the PLL, so
// restore the measured profile after every size change.
constexpr Pll kOv5640Fs0Pll = {80, 1, 2, 0, 2, 1, 8};
constexpr Pll kOv5640Fs1Pll = {70, 1, 2, 0, 2, 1, 7};

#if defined(CONFIG_STREAMER_OV5640_BANDING_50HZ) || defined(CONFIG_STREAMER_OV5640_BANDING_60HZ)
constexpr bool kBandingCorrection = true;
#else
constexpr bool kBandingCorrection = false;
#endif

const Pll* stream_clock(uint8_t fs) {
    return fs == 0 ? &kOv5640Fs0Pll : fs == 1 ? &kOv5640Fs1Pll : nullptr;
}

// sensor_t::get_reg cannot reliably report a failed byte read in the pinned
// driver. Use the public I2C API for checked clock/gain transactions instead.
// Configured SCCB pins make esp_camera_init create the Kconfig-selected bus.
#ifdef CONFIG_SCCB_HARDWARE_I2C_PORT1
constexpr int kSccbPort = 1;
#else
constexpr int kSccbPort = 0;
#endif
constexpr int kRegisterIoTimeoutMs = 100;

struct SensorRegisterIo {
    uint8_t address;
    esp_err_t error = ESP_OK;
#ifndef CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY
    i2c_master_dev_handle_t device = nullptr;
#endif

    bool read(uint16_t reg, uint8_t& value) {
        const uint8_t command[] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg)};
#ifdef CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY
        error = i2c_master_write_read_device(static_cast<i2c_port_t>(kSccbPort), address,
            command, sizeof command, &value, 1, pdMS_TO_TICKS(kRegisterIoTimeoutMs));
#else
        error = i2c_master_transmit_receive(device, command, sizeof command,
            &value, 1, kRegisterIoTimeoutMs);
#endif
        return error == ESP_OK;
    }

    bool write(uint16_t reg, uint8_t value) {
        const uint8_t command[] = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg), value};
#ifdef CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY
        error = i2c_master_write_to_device(static_cast<i2c_port_t>(kSccbPort), address,
            command, sizeof command, pdMS_TO_TICKS(kRegisterIoTimeoutMs));
#else
        error = i2c_master_transmit(device, command, sizeof command, kRegisterIoTimeoutMs);
#endif
        return error == ESP_OK;
    }
};

// Caller holds s_lock, or is init before other SCCB users start. The new IDF
// driver permits a temporary same-address device on the existing camera bus.
esp_err_t open_register_io(SensorRegisterIo& io) {
#ifndef CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t err = i2c_master_get_bus_handle(static_cast<i2c_port_num_t>(kSccbPort), &bus);
    if (err != ESP_OK) return err;
    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = io.address;
    config.scl_speed_hz = CONFIG_SCCB_CLK_FREQ;
    err = i2c_master_bus_add_device(bus, &config, &io.device);
    if (err != ESP_OK) return err;
#else
    (void)io;
#endif
    return ESP_OK;
}

esp_err_t close_register_io(SensorRegisterIo& io) {
#ifndef CONFIG_SCCB_HARDWARE_I2C_DRIVER_LEGACY
    return i2c_master_bus_rm_device(io.device);
#else
    (void)io;
    return ESP_OK;
#endif
}

// OV5640 v2.33 section 2.5 specifies PLLADCLK 190..230 MHz. Reset dividers
// 19/11/30 assume 24 MHz XCLK; at our 20 MHz they produce 166 2/3 MHz.
// Correct only PLL2's multiplier to 30 (200 MHz), leaving SYS/PCLK and image
// settings unchanged. This is not a demonstrated cure for column-pattern noise.
// Caller holds s_lock, or is init before any other SCCB user starts.
esp_err_t apply_adc_clock_locked(sensor_t* s) {
    if (s->id.PID != OV5640_PID) return ESP_OK;
    SensorRegisterIo io{s->slv_addr};
    const esp_err_t opened = open_register_io(io);
    if (opened != ESP_OK) return opened;
    const auto correction = streamer::ov5640::ensure_adc_200mhz(io, XCLK_HZ);
    const esp_err_t cleanup = close_register_io(io);
    if (correction.result != streamer::ov5640::AdcResult::Ok) {
        ESP_LOGE(TAG, "OV5640 ADC clock failed/unverified: step %u, %s",
            static_cast<unsigned>(correction.result), esp_err_to_name(io.error));
        return io.error != ESP_OK ? io.error : ESP_FAIL;
    }
    if (cleanup != ESP_OK) return cleanup;
    ESP_LOGI(TAG, "OV5640 ADC clock verified: 200 MHz, XCLK 20 MHz");
    return ESP_OK;
}


// Opt-in only: correcting the old 50 Hz table changes low-light exposure.
// The receiver never chooses a region, and the default makes no extra I/O.
esp_err_t apply_banding_locked(sensor_t* s, uint8_t fs) {
#if defined(CONFIG_STREAMER_OV5640_BANDING_50HZ) || defined(CONFIG_STREAMER_OV5640_BANDING_60HZ)
    if (s->id.PID != OV5640_PID) return ESP_OK;
    if (!stream_clock(fs)) return ESP_ERR_NOT_SUPPORTED;
    SensorRegisterIo io{s->slv_addr};
    const esp_err_t opened = open_register_io(io);
    if (opened != ESP_OK) return opened;
#ifdef CONFIG_STREAMER_OV5640_BANDING_50HZ
    constexpr auto mode = streamer::ov5640::BandingMode::Hz50;
    constexpr unsigned hz = 50;
#else
    constexpr auto mode = streamer::ov5640::BandingMode::Hz60;
    constexpr unsigned hz = 60;
#endif
    const auto update = streamer::ov5640::configure_banding(io, fs == 0 ? 80'000'000U : 70'000'000U, mode);
    const esp_err_t cleanup = close_register_io(io);
    if (update.result != streamer::ov5640::BandingResult::Ok) {
        ESP_LOGE(TAG, "OV5640 banding failed: step %u, rollback %s",
            static_cast<unsigned>(update.result),
            !update.rollback_attempted ? "not needed" : update.rollback_verified ? "verified" : "unverified");
        return io.error != ESP_OK ? io.error : ESP_FAIL;
    }
    if (cleanup != ESP_OK) return cleanup;
    ESP_LOGI(TAG, "OV5640 %u Hz banding: HTS %u VTS %u, steps %u/%u max %u/%u",
        hz, update.hts, update.vts, update.timing.step50, update.timing.step60,
        update.timing.max50, update.timing.max60);
    return ESP_OK;
#else
    (void)s;
    (void)fs;
    return ESP_OK;
#endif
}

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
esp_err_t apply_clock_locked(sensor_t* s, uint8_t fs) {
    if (s->id.PID != OV5640_PID) return ESP_OK;
    const Pll* profile = stream_clock(fs);
    if (!profile) return kBandingCorrection ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
    const esp_err_t err = set_pll_locked(s, *profile);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ov5640 fs%u stream clock: mult %d sys_div %d pclk_div %d",
                 fs, profile->mult, profile->sys_div, profile->pclk_div);
        return apply_banding_locked(s, fs);
    }
    ESP_LOGW(TAG, "ov5640 fs%u stream clock write failed", fs);
    return kBandingCorrection ? err : ESP_OK;  // retain legacy behavior when disabled
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

// Bound the complete camera wait, including malformed JPEG rejection and
// frames discarded after resizing. Queue availability alone cannot prevent the
// upstream getter from waiting four seconds after rejecting a queued JPEG.
camera_fb_t* take(uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    const TickType_t budget = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= budget) return nullptr;
        if (esp_camera_available_frames()) {
            camera_fb_t* fb = streamer_camera_fb_get_timeout(budget - elapsed);
            if (!fb) return nullptr;
            if (s_drop.load() > 0) {
                s_drop.fetch_sub(1);
                esp_camera_fb_return(fb);
                continue;
            }
            return fb;
        }
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

    if (kBandingCorrection && s->id.PID == OV5640_PID && !stream_clock(opt.framesize))
        return ESP_ERR_NOT_SUPPORTED;
    err = apply_adc_clock_locked(s);
    if (err != ESP_OK) return err;
    // set_framesize rewrites the PLL; restore the measured profile afterwards.
    if (s->set_framesize(s, kFsMap[opt.framesize]) != 0) {
        ESP_LOGW(TAG, "set_framesize failed");
        if (kBandingCorrection && s->id.PID == OV5640_PID) return ESP_FAIL;
    } else {
        err = apply_clock_locked(s, opt.framesize);
        if (err != ESP_OK) return err;
    }
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
    // A queued caller may have waited behind a failed timing restoration.
    if (!s_ready) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    sensor_t* s = esp_camera_sensor_get();
    esp_err_t err = ESP_OK;
    const bool corrected = kBandingCorrection && s->id.PID == OV5640_PID;
    if (corrected && !stream_clock(fs)) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (fs != s_fs) {
        const uint8_t previous_fs = s_fs;
        if (s->set_framesize(s, kFsMap[fs]) != 0) {
            err = ESP_FAIL;
        } else {
            restore_after_resize_locked(s);  // OV2640: dcw/colorbar
            err = apply_clock_locked(s, fs);  // OV5640: measured PLL and optional banding
        }
        if (err == ESP_OK) s_fs = fs;
        if (err == ESP_OK || corrected) {
            s_drop = DROP_AFTER_RESIZE;
        }
        if (err != ESP_OK && corrected) {
            // A size/PLL change already touched the sensor. Restore the last
            // known profile once; never stream with an unverified mixed table.
            const bool restored = s->set_framesize(s, kFsMap[previous_fs]) == 0 &&
                                  apply_clock_locked(s, previous_fs) == ESP_OK;
            if (!restored) {
                s_ready = false;
                ESP_LOGE(TAG, "camera timing restore failed; reboot required");
            }
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
    // Return capture memory before network I/O. Three slots with a one-frame
    // latest queue leave a spare while this copy briefly owns a driver slot.
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
