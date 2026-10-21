#!/usr/bin/env python3
"""Stage pinned camera extensions without modifying managed components."""
import argparse
import hashlib
from pathlib import Path

PINNED_SHA256 = "664b3ba6bd3236e92e63f4db27ea6d5ddd8e2620a37a94b41edd7424d1fd6334"
PINNED_CAMERA_API_SHA256 = "7493cf44f39b32841ca983ba64cfc44a34b1f0ddb8124407a0ac519d649e581a"


def prepare(source):
    if hashlib.sha256(source).hexdigest() != PINNED_SHA256:
        raise ValueError("camera driver revision changed; review driver staging")
    text = source.decode("utf8")

    def replace(old, new, expected=1):
        nonlocal text
        if text.count(old) != expected:
            raise ValueError("camera driver patch site changed; review before building")
        text = text.replace(old, new)

    def replace_region(first, after, new):
        nonlocal text
        if text.count(first) != 1 or text.count(after) != 1:
            raise ValueError("camera driver patch site changed; review before building")
        start = text.index(first)
        end = text.index(after)
        if end <= start:
            raise ValueError("camera driver patch order changed; review before building")
        text = text[:start] + new + text[end:]

    # Three slots leave a spare while the consumer briefly owns another slot.
    # Keep only the latest completed frame: no history/playback queue is added.
    replace('frame_buffer_queue_len = cam_obj->frame_cnt - 1;', 'frame_buffer_queue_len = 1;')
    replace('#include "cam_hal.h"', '#include "cam_hal.h"\n#include "jpeg_endpoint.h"\n#include <limits.h>')
    replace('static const uint8_t JPEG_EOI_BYTES[] = {0xFF, 0xD9};        /* EOI = FF D9 */\n', '')
    replace_region('/* Compute the scan window for JPEG EOI detection in PSRAM. */',
                   'static int cam_verify_jpeg_soi(', '')
    replace_region('static int cam_verify_jpeg_eoi(', 'static bool cam_get_next_frame(', """static int cam_verify_jpeg_eoi(const uint8_t *inbuf, uint32_t length)
{
    const size_t end = streamer_jpeg_endpoint(inbuf, length);
    return end >= JPEG_EOI_MARKER_LEN && end - JPEG_EOI_MARKER_LEN <= INT_MAX
        ? (int)(end - JPEG_EOI_MARKER_LEN) : -1;
}

""")
    replace_region('            /* find the end marker for JPEG. Data after that can be discarded */',
                   '            CAM_WARN_THROTTLE(warn_eoi_miss_cnt,', """            /* Parse the captured frame from its start. A length-delimited header
             * can contain FFD9, and a larger previous frame can leave a stale EOI
             * in the captured tail. Never search the allocation capacity. */
            if (cam_obj->psram_mode) {
                /* DMA bypasses cache; make all captured bytes visible before parsing. */
                cam_drop_psram_cache(dma_buffer->buf, dma_buffer->len);
            }
            int offset_e = cam_verify_jpeg_eoi(dma_buffer->buf, dma_buffer->len);
            if (offset_e >= 0) {
                dma_buffer->len = offset_e + JPEG_EOI_MARKER_LEN;
                return dma_buffer;
            }

""")
    return text


def prepare_camera_api(source):
    if hashlib.sha256(source).hexdigest() != PINNED_CAMERA_API_SHA256:
        raise ValueError("camera API revision changed; review driver staging")
    text = source.decode("utf8")
    original = """camera_fb_t *esp_camera_fb_get()
{
    if (s_state == NULL) {
        return NULL;
    }
    camera_fb_t *fb = cam_take(FB_GET_TIMEOUT);
    //set the frame properties
    if (fb) {
        fb->width = resolution[s_state->sensor.status.framesize].width;
        fb->height = resolution[s_state->sensor.status.framesize].height;
        fb->format = s_state->sensor.pixformat;
    }
    return fb;
}
"""
    if text.count(original) != 1 or text.count('#include "esp_camera.h"') != 1:
        raise ValueError("camera API patch site changed; review before building")
    bounded = original.replace("esp_camera_fb_get()",
                               "streamer_camera_fb_get_timeout(TickType_t timeout_ticks)")
    bounded = bounded.replace("cam_take(FB_GET_TIMEOUT)", "cam_take(timeout_ticks)")
    legacy = """
camera_fb_t *esp_camera_fb_get()
{
    return streamer_camera_fb_get_timeout(FB_GET_TIMEOUT);
}
"""
    text = text.replace('#include "esp_camera.h"',
                        '#include "esp_camera.h"\n#include "camera_timed_get.h"')
    return text.replace(original, bounded + legacy)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--timed-getter", action="store_true",
                        help="stage esp_camera.c's project-specific bounded getter")
    args = parser.parse_args()
    if args.output.resolve().is_relative_to(args.source.parent.resolve()):
        parser.error("output must be outside the managed driver's directory")
    try:
        source = args.source.read_bytes()
        staged = (prepare_camera_api(source) if args.timed_getter
                  else prepare(source))
    except ValueError as exc:
        parser.error(str(exc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(staged)


if __name__ == "__main__":
    main()
