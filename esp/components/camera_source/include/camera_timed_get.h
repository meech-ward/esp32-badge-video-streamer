// Project extension supplied by the SHA-pinned build-directory esp_camera.c.
// The managed public esp_camera_fb_get() retains its four-second timeout.
#pragma once

#include "esp_camera.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// One total queue-wait budget, including rejected JPEGs. Zero ticks returns
// nullptr/NULL without consuming a queued frame, matching cam_take(0).
// A returned frame has the usual metadata and must be given back with
// esp_camera_fb_return(). Finite validation work and task scheduling are not
// a hard real-time wall-clock guarantee.
camera_fb_t *streamer_camera_fb_get_timeout(TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif
