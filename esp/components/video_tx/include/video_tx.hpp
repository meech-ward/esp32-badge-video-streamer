// One credit-paced JPEG client on TCP 8081, plus approximately 1 Hz STATUS.
#pragma once
#include <cstdint>

namespace video_tx {
void start();  // server task on core 0

struct Sample {
    const uint8_t* data;
    uint32_t len;
    uint16_t width;
    uint16_t height;
    int64_t timestamp_us;
};
// Copy a frame already grabbed by the video task for AWB. No extra camera
// grab or credit is used; the copy happens after send. One caller: soft_awb.
// data remains valid until the next sample(). With no credits, this times out.
bool sample(Sample& out, uint32_t timeout_ms);
}  // namespace video_tx
