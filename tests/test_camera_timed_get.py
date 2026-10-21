"""Fault-inject the pinned camera getter/HAL and the real outer wait loop."""
import hashlib
import importlib.util
from io import BytesIO
import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "esp/components/camera_source"
MANAGED = ROOT / "esp/managed_components/espressif__esp32-camera"
API = MANAGED / "driver/esp_camera.c"
HAL = MANAGED / "driver/cam_hal.c"
SPEC = importlib.util.spec_from_file_location("camera_timed_staging", COMPONENT / "prepare_driver.py")
PATCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCHER)
pytestmark = pytest.mark.skipif(not API.is_file() or not HAL.is_file(),
                                reason="ESP managed camera dependency is not installed")


def function(source, signature):
    """Copy a pinned real function into the host boundary fixture unchanged."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


def test_timed_getter_staging_fails_closed(monkeypatch):
    with pytest.raises(ValueError, match="API revision changed"):
        PATCHER.prepare_camera_api(b"unreviewed API")
    source = API.read_bytes()
    changed = source.replace(b"cam_take(FB_GET_TIMEOUT)", b"cam_take(9999)")
    monkeypatch.setattr(PATCHER, "PINNED_CAMERA_API_SHA256", hashlib.sha256(changed).hexdigest())
    with pytest.raises(ValueError, match="patch site changed"):
        PATCHER.prepare_camera_api(changed)


def test_timed_getter_staging_preserves_managed_api(tmp_path):
    source = API.read_bytes()
    output = tmp_path / "staged/esp_camera.c"
    result = subprocess.run([sys.executable, str(COMPONENT / "prepare_driver.py"),
                             str(API), str(output), "--timed-getter"],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    assert API.read_bytes() == source
    assert output.read_text() == PATCHER.prepare_camera_api(source)
    rejected = subprocess.run([sys.executable, str(COMPONENT / "prepare_driver.py"),
                               str(API), str(API), "--timed-getter"],
                              capture_output=True, text=True, timeout=30)
    assert rejected.returncode != 0 and "outside" in rejected.stderr
    assert API.read_bytes() == source


@pytest.mark.parametrize("buffers", [2, 3])
def test_cmake_stages_timed_api_in_every_capture_profile(tmp_path, buffers):
    cmake = shutil.which("cmake")
    if not cmake:
        pytest.skip("CMake is needed for the host-only source-selection check")
    source = tmp_path / "source"
    build = tmp_path / "build"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(f"""cmake_minimum_required(VERSION 3.16)
project(camera_source_selection C)
add_library(camera_stub STATIC "{API}" "{HAL}")
function(idf_component_register)
endfunction()
function(idf_component_get_property output component property)
  if(property STREQUAL "COMPONENT_LIB")
    set(${{output}} camera_stub PARENT_SCOPE)
  else()
    set(${{output}} "{MANAGED}" PARENT_SCOPE)
  endif()
endfunction()
function(idf_build_get_property output property)
  set(${{output}} "{sys.executable}" PARENT_SCOPE)
endfunction()
set(CONFIG_STREAMER_CAMERA_FB_COUNT {buffers})
include("{COMPONENT / 'CMakeLists.txt'}")
get_target_property(chosen camera_stub SOURCES)
file(WRITE "${{CMAKE_BINARY_DIR}}/chosen.txt" "${{chosen}}")
""")
    result = subprocess.run([cmake, "-S", str(source), "-B", str(build)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    chosen = (build / "chosen.txt").read_text().split(";")
    assert str(API) not in chosen
    assert str(build / "camera_driver/esp_camera.c") in chosen
    staged_hal = build / "camera_driver/cam_hal.c"
    assert str(HAL) not in chosen
    assert staged_hal.exists()
    assert staged_hal.read_text() == PATCHER.prepare(HAL.read_bytes())


MOCK_CAMERA = r"""#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct camera_fb_t {
    uint8_t *buf;
    size_t len, width, height;
    int format;
} camera_fb_t;
#ifdef __cplusplus
extern "C" {
#endif
camera_fb_t *esp_camera_fb_get(void);
void esp_camera_fb_return(camera_fb_t *fb);
bool esp_camera_available_frames(void);
#ifdef __cplusplus
}
#endif
"""

MOCK_BRIDGE = r"""#pragma once
#include "camera_timed_get.h"
#ifdef __cplusplus
extern "C" {
#endif
void mock_reset(TickType_t start);
void mock_enqueue(unsigned slot, TickType_t arrival, bool valid);
void mock_enqueue_tail(unsigned slot, TickType_t arrival, bool truncated);
void mock_psram(bool enabled);
unsigned mock_cache_count(void);
size_t mock_cache_length(void);
void mock_state_missing(bool missing);
void mock_metadata(unsigned size, int format);
TickType_t mock_elapsed(void);
unsigned mock_returns(unsigned slot);
unsigned mock_outstanding(void);
unsigned mock_budget_count(void);
TickType_t mock_budget(unsigned index);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
#ifdef __cplusplus
}
#endif
"""

HAL_BOUNDARY = r"""
#include "mock_bridge.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include "jpeg_endpoint.h"
#define CONFIG_IDF_TARGET_ESP32S3 1
#define portTICK_PERIOD_MS 1
#define pdTRUE 1
#define pdFALSE 0
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_CAMERA_ETS_PRINTF(...) ((void)0)
#define CAM_WARN_THROTTLE(counter, ...) (++(counter))
#define JPEG_EOI_MARKER_LEN 2
static const char *TAG = "host";
typedef struct {
    void *frame_buffer_queue;
    bool jpeg_mode, psram_mode;
    size_t dma_node_buffer_size;
    int in_bytes_per_pixel, fb_bytes_per_pixel;
} cam_obj_t;
static cam_obj_t camera_object;
static cam_obj_t *cam_obj = &camera_object;
typedef struct { struct { struct { unsigned framesize; } status; int pixformat; } sensor; } camera_state_t;
static camera_state_t state;
static camera_state_t *s_state = &state;
static const struct { size_t width, height; } resolution[] = {{160, 120}, {320, 240}};
static camera_fb_t frames[8];
JPEG_FIXTURES
static uint8_t payloads[8][sizeof stale_capture];
static bool held[8];
static unsigned returned[8];
typedef struct { TickType_t at; camera_fb_t *frame; } event_t;
static event_t events[8];
static unsigned event_count, next_event, wait_count;
static TickType_t waits[32], now_ticks, origin;
static unsigned cache_count;
static size_t cache_length;
void mock_reset(TickType_t start) {
    memset(frames, 0, sizeof frames); memset(held, 0, sizeof held);
    memset(returned, 0, sizeof returned); memset(waits, 0, sizeof waits);
    event_count = next_event = wait_count = 0; now_ticks = origin = start;
    camera_object = (cam_obj_t){.frame_buffer_queue = events, .jpeg_mode = true};
    state = (camera_state_t){0}; s_state = &state;
    cache_count = 0; cache_length = 0;
}
void mock_enqueue(unsigned slot, TickType_t arrival, bool valid) {
    assert(slot < 8 && event_count < 8);
    if (event_count) assert(arrival >= events[event_count - 1].at);
    memcpy(payloads[slot], valid_jpeg, sizeof valid_jpeg);
    frames[slot] = (camera_fb_t){.buf = payloads[slot], .len = valid ? sizeof valid_jpeg : 4};
    events[event_count++] = (event_t){arrival, &frames[slot]};
}
void mock_enqueue_tail(unsigned slot, TickType_t arrival, bool truncated) {
    mock_enqueue(slot, arrival, true);
    memcpy(payloads[slot], stale_capture, sizeof stale_capture);
    frames[slot].len = truncated ? 5649 : sizeof stale_capture;
}
void mock_psram(bool enabled) { cam_obj->psram_mode = enabled; }
unsigned mock_cache_count(void) { return cache_count; }
size_t mock_cache_length(void) { return cache_length; }
void mock_state_missing(bool missing) { s_state = missing ? NULL : &state; }
void mock_metadata(unsigned size, int format) {
    assert(size < 2); state.sensor.status.framesize = size; state.sensor.pixformat = format;
}
TickType_t mock_elapsed(void) { return now_ticks - origin; }
TickType_t xTaskGetTickCount(void) { return now_ticks; }
void vTaskDelay(TickType_t ticks) { now_ticks += ticks; }
unsigned mock_returns(unsigned slot) { assert(slot < 8); return returned[slot]; }
unsigned mock_outstanding(void) {
    unsigned n = 0; for (unsigned i = 0; i < 8; ++i) n += held[i]; return n;
}
unsigned mock_budget_count(void) { return wait_count; }
TickType_t mock_budget(unsigned index) { assert(index < wait_count); return waits[index]; }
bool esp_camera_available_frames(void) {
    return next_event < event_count && events[next_event].at <= mock_elapsed();
}
static int xQueueReceive(void *queue, void *out, TickType_t timeout) {
    assert(queue == events);
    if (next_event < event_count) {
        TickType_t elapsed = mock_elapsed();
        TickType_t delay = events[next_event].at > elapsed ? events[next_event].at - elapsed : 0;
        if (delay <= timeout) {
            now_ticks += delay;
            camera_fb_t *fb = events[next_event++].frame;
            unsigned slot = (unsigned)(fb - frames); assert(slot < 8 && !held[slot]);
            held[slot] = true; *(camera_fb_t **)out = fb; return pdTRUE;
        }
    }
    now_ticks += timeout; return pdFALSE;
}
static void cam_give(camera_fb_t *fb) {
    unsigned slot = (unsigned)(fb - frames); assert(slot < 8 && held[slot]);
    held[slot] = false; ++returned[slot];
}
static void ll_cam_dma_reset(cam_obj_t *unused) { (void)unused; }
static void cam_drop_psram_cache(void *ptr, size_t length) {
    assert(ptr); ++cache_count; cache_length = length;
}
static size_t ll_cam_memcpy(cam_obj_t *unused, uint8_t *to, uint8_t *from, size_t n) {
    (void)unused; memmove(to, from, n); return n;
}
"""

PROGRAM = r"""
#include "mock_bridge.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
static std::atomic<int> s_drop{0};
// The real take() is inserted here; no test replacement owns its deadline.
TAKE_FUNCTION
static void reset(TickType_t start = 0) { mock_reset(start); s_drop = 0; }
static void returned(unsigned slot) { assert(mock_returns(slot) == 1 && mock_outstanding() == 0); }
int main() {
    reset(); mock_state_missing(true);
    assert(streamer_camera_fb_get_timeout(100) == nullptr && esp_camera_fb_get() == nullptr);
    assert(mock_budget_count() == 0 && mock_elapsed() == 0);

    reset(); mock_metadata(1, 7); mock_enqueue(0, 0, true);
    auto *fb = streamer_camera_fb_get_timeout(100);
    assert(fb && fb->width == 320 && fb->height == 240 && fb->format == 7);
    assert(mock_elapsed() == 0 && mock_budget(0) == 100 && mock_outstanding() == 1);
    esp_camera_fb_return(fb); returned(0);

    reset(); mock_metadata(0, 3); mock_enqueue(0, 0, true);
    fb = esp_camera_fb_get();
    assert(fb && fb->width == 160 && fb->height == 120 && fb->format == 3);
    assert(mock_budget(0) == 4000); esp_camera_fb_return(fb); returned(0);

    // Captured lengths reproduce the observed 5650-byte current JPEG followed
    // by a stale larger-frame tail through byte 7561. Metadata also contains
    // FFD9, so neither the first raw byte pair nor the final pair is correct.
    for (bool psram : {false, true}) {
        reset(); mock_psram(psram); mock_enqueue_tail(0, 0, false);
        fb = take(100); assert(fb && fb->len == 5650);
        assert(mock_cache_count() == (psram ? 1u : 0u));
        if (psram) assert(mock_cache_length() == 7561);
        esp_camera_fb_return(fb); returned(0);
        reset(); mock_psram(psram); mock_enqueue_tail(0, 0, true);
        assert(take(100) == nullptr && mock_elapsed() == 100); returned(0);
        // Bytes beyond the captured length include EOI, but are never searched.
        if (psram) assert(mock_cache_length() == 5649);
    }

    // Real cam_take rejects this queued JPEG (missing EOI), releases it, then
    // blocks on an empty queue. This reproduces the old four-second escape.
    reset(); mock_enqueue(0, 0, false);
    assert(esp_camera_fb_get() == nullptr && mock_elapsed() == 4000); returned(0);
    reset(); mock_enqueue(0, 0, false);
    assert(take(100) == nullptr && mock_elapsed() == 100);
    assert(mock_budget_count() == 1 && mock_budget(0) == 100); returned(0);

    // The quiet availability poll must not call/log the driver for no frame.
    reset(); assert(take(100) == nullptr && mock_elapsed() == 100);
    assert(mock_budget_count() == 0 && mock_outstanding() == 0);

    reset(); mock_enqueue(0, 0, false); mock_enqueue(1, 40, true);
    fb = take(100); assert(fb && mock_elapsed() == 40);
    assert(mock_returns(0) == 1 && mock_returns(1) == 0);
    esp_camera_fb_return(fb); assert(mock_returns(1) == 1 && mock_outstanding() == 0);

    reset(); mock_enqueue(0, 0, false); mock_enqueue(1, 40, false); mock_enqueue(2, 90, false);
    assert(take(100) == nullptr && mock_elapsed() == 100);
    assert(mock_budget_count() == 1);
    for (unsigned i = 0; i < 3; ++i) assert(mock_returns(i) == 1);
    assert(mock_outstanding() == 0);

    // The outer deadline must survive valid-but-discarded resize frames. The
    // final malformed frame has only 20 ticks left, not another fresh 100.
    for (TickType_t start : {TickType_t{0}, UINT32_MAX - 50}) {
        reset(start); s_drop = 2;
        mock_enqueue(0, 0, true); mock_enqueue(1, 60, true); mock_enqueue(2, 80, false);
        assert(take(100) == nullptr && mock_elapsed() == 100 && s_drop == 0);
        assert(mock_budget_count() == 3);
        assert(mock_budget(0) == 100 && mock_budget(1) == 40 && mock_budget(2) == 20);
        for (unsigned i = 0; i < 3; ++i) assert(mock_returns(i) == 1);
        assert(mock_outstanding() == 0);
    }

    reset(); mock_enqueue(0, 100, true);
    assert(take(100) == nullptr && mock_elapsed() == 100 && mock_budget_count() == 0);
    assert(mock_outstanding() == 0); // a frame arriving at expiry was not acquired

    reset(); mock_enqueue(0, 0, true);
    assert(take(0) == nullptr && mock_elapsed() == 0 && mock_budget_count() == 0);
    assert(streamer_camera_fb_get_timeout(0) == nullptr && mock_elapsed() == 0);
    assert(mock_outstanding() == 0 && mock_returns(0) == 0);
    fb = take(100); assert(fb); esp_camera_fb_return(fb); returned(0);
    std::puts("real getter/HAL: malformed wait 4000 -> 100 ticks; resize budgets 100/40/20; rollover/leases OK");
}
"""


def test_actual_getter_hal_and_outer_deadline(tmp_path):
    cc = os.environ.get("HOST_CC") or shutil.which("clang") or shutil.which("gcc")
    cxx = os.environ.get("HOST_CXX") or shutil.which("clang++") or shutil.which("g++")
    if not cc or not cxx:
        pytest.skip("host C and C++ compilers are required")
    # Compile the actual staged HAL/parser/getter rather than a model of their
    # rejection, retry, trim, and ownership behavior. Both pins remain checked.
    hal = PATCHER.prepare(HAL.read_bytes())
    api = PATCHER.prepare_camera_api(API.read_bytes())
    (tmp_path / "esp_camera.h").write_text(MOCK_CAMERA)
    (tmp_path / "freertos").mkdir()
    (tmp_path / "freertos/FreeRTOS.h").write_text(
        "#pragma once\n#include <stdint.h>\ntypedef uint32_t TickType_t;\n"
        "#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))\n")
    (tmp_path / "mock_bridge.h").write_text(MOCK_BRIDGE)
    real_take = function(hal, "camera_fb_t *cam_take(TickType_t timeout)")
    # A one-name boundary shim records budgets; its body calls the unmodified
    # staged retry/endpoint loop, not a model of that behavior.
    real_take = real_take.replace("camera_fb_t *cam_take(", "static camera_fb_t *actual_cam_take(", 1)
    image = BytesIO()
    Image.new("L", (8, 8), 127).save(image, format="JPEG", quality=70)
    jpeg = image.getvalue()

    def padded(size):
        payload = b"metadata:\xff\xd9" + b"\x55" * (size - len(jpeg) - 4 - 11)
        app = b"\xff\xe2" + (len(payload) + 2).to_bytes(2, "big") + payload
        result = jpeg[:2] + app + jpeg[2:]
        assert len(result) == size
        return result

    short, larger = padded(5650), padded(7561)
    stale_capture = short + larger[len(short):]
    assert len(stale_capture) == 7561 and stale_capture[5648:5650] == b"\xff\xd9"
    fixtures = "\n".join(f"static const uint8_t {name}[] = {{{','.join(map(str, value))}}};"
                         for name, value in (("valid_jpeg", jpeg), ("stale_capture", stale_capture)))
    hal_source = HAL_BOUNDARY.replace("JPEG_FIXTURES", fixtures)
    hal_source += function(hal, "static int cam_verify_jpeg_eoi(")
    hal_source += real_take
    hal_source += """
static camera_fb_t *cam_take(TickType_t ticks) {
    assert(wait_count < 32); waits[wait_count++] = ticks; return actual_cam_take(ticks);
}
#define FB_GET_TIMEOUT (4000 / portTICK_PERIOD_MS)
"""
    hal_source += function(api, "camera_fb_t *streamer_camera_fb_get_timeout(")
    hal_source += function(api, "camera_fb_t *esp_camera_fb_get()")
    hal_source += function(api, "void esp_camera_fb_return(camera_fb_t *fb)")
    c_file = tmp_path / "actual_driver.c"
    c_file.write_text(hal_source)
    outer = function((COMPONENT / "camera_source.cpp").read_text(), "camera_fb_t* take(uint32_t timeout_ms)")
    cpp_file = tmp_path / "wait_test.cpp"
    cpp_file.write_text(PROGRAM.replace("TAKE_FUNCTION", outer))
    flags = ["-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer", "-I" + str(tmp_path), "-I" + str(COMPONENT / "include")]
    obj, binary = tmp_path / "driver.o", tmp_path / "wait_test"
    built_c = subprocess.run([cc, "-std=c11", *flags, "-c", str(c_file), "-o", str(obj)],
                             capture_output=True, text=True, timeout=30)
    assert built_c.returncode == 0, built_c.stdout + built_c.stderr
    built_cpp = subprocess.run([cxx, "-std=c++20", *flags, str(cpp_file), str(obj), "-o", str(binary)],
                               capture_output=True, text=True, timeout=30)
    assert built_cpp.returncode == 0, built_cpp.stdout + built_cpp.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert run.returncode == 0, run.stdout + run.stderr
    assert "malformed wait 4000 -> 100 ticks" in run.stdout
